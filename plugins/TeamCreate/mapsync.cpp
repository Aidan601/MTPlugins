#include <windows.h>
#include <shlobj.h>
#include <mutex>
#include <functional>
#include <set>
#include <map>
#include <deque>
#include <array>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include "mapsync.h"
#include "presence.h"
#include "mapmodel.h"
#include "net.h"
#include "backup.h"
#include "prefabs.h"
#include "compat.h"

// Scopes: Radiant edits one file at a time, the root map or (after "Enter Prefab") the prefab's own .map. Every sync message names its scope (a key relative to map_source).
// Messages for the scope being edited are applied in place, others are merged into a stored model of that file (a prefab is also rewritten on disk, the map reloads once
// the user is idle at the root). Entering a scope applies the changed objects in place, then aligns the model silently.

// MAP (grep the names, line numbers drift):
//   State ..................... RemoteOp, g_model (the scope being edited), g_scopePath, g_scopeKey, g_depth,
//                               g_rootPath, g_rootKey, g_ready, g_dirty, g_applying, g_silent, g_blocked,
//                               g_awaitFullSync, g_retry, g_tempKey, g_tempPath, g_expectedFiles,
//                               g_mutex guards g_pending (ops queued by the net thread) + g_status, g_warning
//   Other scopes .............. Scope, g_scopes, g_rootReloadPending, g_scopeUnwritten, TempSession, SetStatus, SetWarning
//   Scope keys and files ...... IsPrefabKey, ScopeBase, SameScope, NoteForeignRootScope, ScopeKey, RealPathOfKey,
//                               PathOfKey, BaseName, Narrow, Widen, ReadFile, WriteFileAtomic, LoadModelFromDisk
//   Worldspawn identity ....... WorldOf, IsWorldGuid, SyncWorldGuid, g_remoteWorldGuids, g_worldKeysPending
//   Outgoing .................. SendEntityUpsert, SendBrushUpsert, SendDelete
//   Capture (local -> peers) .. NoteSelection, CaptureEntity, AnnounceSelectedNewImpl, CaptureSelectedKeys, Capture,
//                               CaptureStats (g_cs), g_liveKeys, g_prevSelected, g_everSelected, g_writerSkips, g_heldDeletes, g_forceDeletes
//   Merge into a model ........ MergeInto, DiffGuids, SyntheticOps
//   In-place apply ............ LiveIndex, BuildIndex, WorldBrushImportText, EntityImportText, NeedsReloadClass,
//                               DeferToReload, HasLinkKeys, ApplyOpsInPlace (batched), ApplyToRadiant (the reload path),
//                               g_reloadClassPending
//   Undo identity ............. UndoIds, ReadUndoIds, WriteUndoIds
//   Scopes not being edited ... ScopeFor, WriteScopeFile, MergeOffline, ScopeOfOp, GuidOfOp
//   Full sync ................. HandleFullSync, SendFullSync
//   Wanted prefab files ....... Wanted, g_wanted, g_awaitFile, MissingPrefabsOf, TickWantedFiles, ReleaseHeldOps (a peer's
//                               entity uses a prefab file this PC lacks, asked for over MSG_FILE_REQUEST)
//   namespace mapsync (public)  Init, OnMapLoaded, BeforeScopeChange, OnScopeChanged, OnScopeSaved,
//                               Tick (main thread, every 500 ms), Settled, DeletesCommand, OnSessionStarted,
//                               OnMessage (net thread: pushes into g_pending), OnPeerJoined, Status, Warning,
//                               Warn, KeyOfPath, RootKey, SameMap, PathForKey, SetBlocked, ExpectFiles,
//                               HostInventory, SendPrefabFile, OnMapClosed

namespace
{
    using namespace mapmodel;

    struct RemoteOp { uint16_t type; std::vector<uint8_t> payload; uint32_t from; };

    std::mutex            g_mutex;                // guards g_pending + g_status
    std::deque<RemoteOp>  g_pending;              // remote changes waiting for the main thread
    std::string           g_status = "idle";
    std::string           g_warning;               // shown in the pane, e.g. the host has another map open

    // main-thread state: the scope being edited
    Model                 g_model;
    std::wstring          g_scopePath;            // file of the current scope
    std::string           g_scopeKey;             // wire name of the current scope
    int                   g_depth = 0;            // prefab depth of the current scope
    std::wstring          g_rootPath;             // the root map (scope at depth 0)
    std::string           g_rootKey;
    bool                  g_ready = false;
    bool                  g_dirty = false;        // model has remote changes not yet applied
    uint64_t              g_lastRemoteTick = 0;
    uint64_t              g_lastTick = 0;
    std::set<std::string> g_prevSelected;         // brush GUIDs selected last tick
    std::map<std::string, std::string> g_liveKeys; // selected entity GUID -> its header last tick, while it differs from the model
    unsigned              g_liveKeySends = 0;
    std::set<std::string> g_writerSkips;          // live brush GUIDs Radiant's writer never emits (empty brush of point entities)
    std::set<std::string> g_everSelected;         // GUIDs the user has had selected (Radiant only deletes what is selected)
    size_t                g_heldDeletes = 0;      // deletions held back (never selected here, too many at once)
    bool                  g_forceDeletes = false; // "TeamCreate deletes send": broadcast them anyway
    bool                  g_applying = false;
    bool                  g_silent = false;       // capture without broadcasting (aligning after a scope switch)
    std::deque<RemoteOp>  g_retry;                // in-place ops deferred (target selected)
    // A peer's entity uses a prefab file this PC lacks: it is asked for (MSG_FILE_REQUEST, a client asks the host, the host
    // asks the sender), arrives as a MSG_FULL_SYNC (HandleFullSync writes it where PathOfKey says) and the entity's
    // in-place apply waits for it. After 60 s the entity is applied without it.
    struct Wanted { uint64_t firstTick = 0, lastRequest = 0; uint32_t from = 0; std::wstring path; };
    std::map<std::string, Wanted> g_wanted;       // lower-case prefab key -> state
    struct HeldOp { RemoteOp op; std::vector<std::string> keys; };
    std::deque<HeldOp>    g_awaitFile;            // current-scope ops waiting for a wanted file
    std::string GuidOfOp(const RemoteOp& op);
    bool HeldByFile(const std::string& guid)      // upsert waits in g_awaitFile (capture must not report it deleted)
    {
        for (const auto& h : g_awaitFile) if (GuidOfOp(h.op) == guid) return true;
        return false;
    }
    unsigned              g_sentUpserts = 0, g_sentDeletes = 0, g_recvOps = 0, g_applies = 0;
    double                g_maxCaptureMs = 0;
    volatile bool         g_awaitFullSync = false;   // client: no broadcast before the host's snapshot is in
    volatile uint64_t     g_awaitSince = 0;
    bool                  g_blocked = false;         // client: our map is not the host's, sync is off
    std::string           g_tempKey;                 // client: host map whose full sync goes to a temp file (SetTempTarget)
    std::wstring          g_tempPath;
    std::function<void(bool)> g_tempDone;
    std::set<std::string>     g_expectedFiles;          // client: prefab keys whose full sync goes to the mirror
    std::function<void()>     g_filesDone;
    bool TempSession() { return g_rootKey.rfind("_teamcreate_temp/", 0) == 0; }

    // scopes not being edited
    struct Scope
    {
        Model                 model;
        std::set<std::string> touched;            // GUIDs changed remotely meanwhile
        std::wstring          path;
        bool                  fromDisk = false;   // model came from the file
    };
    std::map<std::string, Scope> g_scopes;
    bool                  g_rootReloadPending = false;   // a prefab file of the root map was rewritten, reload when idle
    std::map<std::string, bool> g_scopeUnwritten;        // scope key -> reloadNeeded: write waiting for the no-backup question

    void SetStatus(const std::string& s) { std::lock_guard<std::mutex> lock(g_mutex); g_status = s; }
    void SetWarning(const std::string& s) { std::lock_guard<std::mutex> lock(g_mutex); g_warning = s; }

    bool IsPrefabKey(const std::string& key) { return key.rfind("_prefabs/", 0) == 0; }

    // Last path component of a scope key ("zm/zm_teamtest.map" -> "zm_teamtest.map").
    std::string ScopeBase(const std::string& key) { size_t s = key.find_last_of('/'); return s == std::string::npos ? key : key.substr(s + 1); }

    // Two scope keys name the same file. Maps in different folders on two machines match by file name,
    // prefabs by full path (two prefabs can share a file name).
    bool SameScope(const std::string& a, const std::string& b)
    {
        if (a == b) return true;
        if (IsPrefabKey(a) || IsPrefabKey(b)) return false;
        return ScopeBase(a) == ScopeBase(b);
    }

    // A message for a root map we do not have open: nothing can be applied, say so
    void NoteForeignRootScope(const std::string& key, const std::string& what)
    {
        static std::string lastKey;
        if (key == lastKey) return;
        lastKey = key;
        std::string msg = "The other side has '" + key + "' open, you have '" + (g_depth ? g_rootKey : g_scopeKey) + "'. Open the same map (same path under map_source) to share edits.";
        LOG("sync: %s for '%s' ignored: %s", what.c_str(), key.c_str(), msg.c_str());
        SetWarning(msg);
    }

    std::set<std::string> g_remoteWorldGuids;    // GUIDs peers use for their worldspawn (Radiant regenerates it on paste)
    bool                  g_worldKeysPending = false;   // worldspawn key/values changed remotely, applied via reload
    bool                  g_reloadClassPending = false; // a class needing Radiant's load path arrived (NeedsReloadClass)
    bool                  g_alignPending = false;       // scope switched outside a session: re-read at the first session tick

    Entity* WorldOf(Model& m)
    {
        for (auto& e : m.entities) if (e.classname == "worldspawn") return &e;
        return nullptr;
    }
    Entity* ModelWorld() { return WorldOf(g_model); }
    bool IsWorldGuid(const std::string& guid)
    {
        Entity* w = ModelWorld();
        return (w && w->guid == guid) || g_remoteWorldGuids.count(guid) != 0;
    }

    void SendEntityUpsert(const Entity& e, size_t order);   // defined below

    // Radiant's paste gives worldspawn a fresh GUID, so it is tracked by class: rename the model entry when it differs
    void SyncWorldGuid()
    {
        hooks::RadPtr live = hooks::WorldEntity();
        Entity* w = ModelWorld();
        if (!live || !w) return;
        std::string lg = hooks::EntityGuid(live);
        if (lg.empty() || lg == w->guid) return;
        LOG("sync: worldspawn guid changed %s -> %s (Radiant paste); renaming in the model", w->guid.c_str(), lg.c_str());
        w->guid = lg;
        w->header = ReplaceGuid(w->header, lg);
        // Announce it: our world BRUSH_UPSERTs carry this GUID as owner, and a peer maps it to its own worldspawn
        // only from its remote-worldspawn set, else pasted world brushes are lost
        SendEntityUpsert(*w, 0);
    }

    std::string Narrow(const std::wstring& w)
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n ? n - 1 : 0, '\0');
        if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
        return s;
    }
    std::wstring Widen(const std::string& s)
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n ? n - 1 : 0, L'\0');
        if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
        return w;
    }

    std::string BaseName(const std::wstring& p)
    {
        size_t slash = p.find_last_of(L"\\/");
        return Narrow(slash == std::wstring::npos ? p : p.substr(slash + 1));
    }

    // Wire name of a file: lower case, relative to map_source with '/' separators (bare name outside map_source)
    std::string ScopeKey(const std::wstring& path)
    {
        std::wstring p = path;
        for (auto& c : p) { if (c == L'/') c = L'\\'; c = wchar_t(towlower(c)); }
        size_t at = p.find(L"\\map_source\\");
        std::wstring rel = at == std::wstring::npos ? Widen(BaseName(p)) : p.substr(at + 12);
        for (auto& c : rel) if (c == L'\\') c = L'/';
        return prefabs::DelocalizeKey(Narrow(rel));      // a mirrored prefab is '_prefabs/...' to everybody else
    }
    // Inverse for unseen scopes: <map_source>\<key>, map_source from the open map's path, else the mod tools root
    std::wstring RealPathOfKey(const std::string& key)
    {
        std::wstring rel = Widen(key);
        for (auto& c : rel) if (c == L'/') c = L'\\';
        std::wstring lower = g_rootPath;
        for (auto& c : lower) { if (c == L'/') c = L'\\'; c = wchar_t(towlower(c)); }
        size_t at = lower.find(L"\\map_source\\");
        if (at != std::wstring::npos) return g_rootPath.substr(0, at + 12) + rel;
        std::wstring root;
        if (rp::host->RadiantRoot) { const wchar_t* r = rp::host->RadiantRoot(rp::self); if (r && *r) root = r; }
        while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
        if (root.empty()) return L"";
        return root + L"\\map_source\\" + rel;
    }
    // Mirror-aware: a mirrored prefab lives under _teamcreate_temp, and on a temporary map a prefab missing here
    // is created in the mirror too, never in the user's _prefabs
    std::wstring PathOfKey(const std::string& key)
    {
        if (IsPrefabKey(key))
        {
            if (prefabs::IsMirrored(key)) return prefabs::MirrorPath(key);
            std::wstring real = RealPathOfKey(key);
            if (TempSession() && !real.empty() && GetFileAttributesW(real.c_str()) == INVALID_FILE_ATTRIBUTES) { prefabs::MarkMirrored(key); LOG("sync: prefab '%s' is new here; it goes to the mirror", key.c_str()); return prefabs::MirrorPath(key); }
            return real;
        }
        return RealPathOfKey(key);
    }

    bool ReadFile(const std::wstring& path, std::string& out)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::ostringstream ss; ss << f.rdbuf(); out = ss.str();
        return true;
    }

    bool WriteFileAtomic(const std::wstring& path, const std::string& text)
    {
        std::wstring tmp = path + L".rtc_tmp";
        { size_t sl = path.find_last_of(L"\\/"); if (sl != std::wstring::npos) SHCreateDirectoryExW(nullptr, path.substr(0, sl).c_str(), nullptr); }
        { std::ofstream f(tmp, std::ios::binary); if (!f) return false; f.write(text.data(), std::streamsize(text.size())); }
        return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }

    bool LoadModelFromDisk(const std::wstring& path, Model& m)
    {
        std::string text, err;
        if (!ReadFile(path, text)) return false;
        if (!ParseMap(text, m, &err)) { LOG("sync: parse failed for %S: %s", path.c_str(), err.c_str()); return false; }
        return true;
    }

    double NowMs() { LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return double(t.QuadPart) * 1000.0 / double(f.QuadPart); }

    // ---- outgoing ---------------------------------------------------------------------
    void SendEntityUpsert(const Entity& e, size_t order)
    {
        if (g_silent) return;
        net::Writer w; w.str(g_scopeKey); w.str(e.guid); w.u32(uint32_t(order)); w.blob(prefabs::Delocalize(e.header));
        net::Send(net::MSG_ENTITY_UPSERT, w.buf); ++g_sentUpserts;
    }
    void SendBrushUpsert(const std::string& entGuid, const Brush& b)
    {
        if (g_silent) return;
        net::Writer w; w.str(g_scopeKey); w.str(entGuid); w.str(b.guid); w.blob(b.text);
        net::Send(net::MSG_BRUSH_UPSERT, w.buf); ++g_sentUpserts;
    }
    void SendDelete(bool entity, const std::string& guid)
    {
        if (g_silent) return;
        net::Writer w; w.str(g_scopeKey); w.u8(entity ? 0 : 1); w.str(guid);
        net::Send(net::MSG_DELETE, w.buf); ++g_sentDeletes;
    }

    void NoteSelection(const std::vector<hooks::SelectedBrush>& sel)
    {
        for (const auto& s : sel) { if (!s.brushGuid.empty()) g_everSelected.insert(s.brushGuid); if (!s.entityGuid.empty()) g_everSelected.insert(s.entityGuid); }
    }

    // ---- capture ------------------------------------------------------------------------
    // Re-serialize one live entity through Radiant's writer, merge into the model, broadcast diffs.
    // Selected brushes are missing from the writer output (Radiant unlinks them), not deleted.
    void CaptureEntity(hooks::RadPtr ent, const std::set<std::string>& selected)
    {
        double p0 = NowMs();
        std::string text;
        if (!hooks::WriteEntityText(ent, text)) { LOG("sync: WriteEntityText failed"); return; }
        double p1 = NowMs();
        Entity parsed; std::string err;
        if (!ParseEntity(text, parsed, &err)) { LOG("sync: cannot parse writer output: %s", err.c_str()); return; }
        if (parsed.guid.empty()) return;
        double p2 = NowMs();

        Entity* m = g_model.FindEntity(parsed.guid);
        if (!m && parsed.classname == "worldspawn") { m = ModelWorld(); if (m) { m->guid = parsed.guid; m->header = ReplaceGuid(m->header, parsed.guid); } }
        size_t order = m ? size_t(m - &g_model.entities[0]) : g_model.entities.size();
        // Objects a peer holds are moved live here (presence::TickLiveMove): their live text is ahead of the model
        // on purpose and sending it back would overwrite their drag. Their commit upsert updates the model.
        std::set<std::string> held = presence::HeldByPeers();
        bool entityHeld = held.count(parsed.guid) != 0;
        if (!m)
        {
            g_model.entities.push_back(Entity{ parsed.guid, parsed.header, {}, parsed.classname });
            m = &g_model.entities.back();
            SendEntityUpsert(*m, order);
            if (!g_silent) LOG("sync: new entity %s (%s)", parsed.guid.c_str(), parsed.classname.c_str());
        }
        else if (m->header != parsed.header && !entityHeld)
        {
            if (Cfg().verbose) { std::string h = m->header; for (auto& c : h) if (c == '\n') c = '|'; LOG("sync:   old header: %.300s", h.c_str()); }
            m->header = parsed.header; m->classname = parsed.classname;
            SendEntityUpsert(*m, order);
            if (!g_silent) LOG("sync: entity keys changed %s (%s)", parsed.guid.c_str(), parsed.classname.c_str());
            if (Cfg().verbose) { std::string h = parsed.header; for (auto& c : h) if (c == '\n') c = '|'; LOG("sync:   new header: %.300s", h.c_str()); }
        }
        double p3 = NowMs();
        std::set<std::string> emitted;
        for (const Brush& b : parsed.brushes) emitted.insert(b.guid);
        size_t liveBrushes = 0;
        hooks::ForEachEntityBrush(ent, [&](hooks::RadPtr br) { ++liveBrushes; std::string g = hooks::BrushGuid(br); if (!g.empty() && !emitted.count(g)) g_writerSkips.insert(g); });
        double p4 = NowMs();
        for (const Brush& b : parsed.brushes)
        {
            if (b.guid.empty()) continue;
            Entity* owner = nullptr;
            Brush* mb = g_model.FindBrush(b.guid, &owner);
            if (mb && held.count(b.guid)) continue;                                   // a peer is dragging it
            if (mb && owner != m) { g_model.RemoveBrush(b.guid); mb = nullptr; }   // moved between entities
            if (!mb) { m->brushes.push_back(b); SendBrushUpsert(m->guid, b); if (!g_silent) LOG("sync: new brush %s in %s", b.guid.c_str(), m->classname.c_str()); }
            else if (mb->text != b.text) { mb->text = b.text; SendBrushUpsert(m->guid, b); if (!g_silent) LOG("sync: brush changed %s", b.guid.c_str()); }
        }
        double p5 = NowMs();
        if (p5 - p0 > 1000) LOG("sync: PROBE CaptureEntity %s: write %.0f parse %.0f header+send %.0f brushwalk %.0f (%zu live brushes) brushes %.0f ms, text %zu bytes", parsed.guid.c_str(), p1 - p0, p2 - p1, p3 - p2, p4 - p3, liveBrushes, p5 - p4, text.size());
        (void)selected;
    }

    struct CaptureStats { double walk = 0, sel = 0, captured = 0, dead = 0, slowest = 0; size_t nCaptured = 0; std::string slowestClass; } g_cs;

    void AnnounceSelectedNewImpl()
    {
        if (!Cfg().syncEnabled || !g_ready || !net::IsConnected() || g_blocked || g_awaitFullSync || g_silent) return;
        std::vector<hooks::RadPtr> insts = hooks::SelectedBrushPtrs();
        if (insts.empty()) return;
        SyncWorldGuid();
        std::map<hooks::RadPtr, std::vector<hooks::RadPtr>> byOwner;   // owner -> selected brushes the model lacks
        std::set<std::string> wanted;
        for (hooks::RadPtr inst : insts)
        {
            hooks::RadPtr b = hooks::BrushOfInst(inst); if (!b) continue;
            std::string guid = hooks::BrushGuid(b);
            if (guid.empty() || g_writerSkips.count(guid) || g_model.FindBrush(guid)) continue;
            hooks::RadPtr owner = hooks::BrushOwner(b); if (!owner) continue;
            if (hooks::EntityFixedSize(owner)) { byOwner[owner]; continue; }           // point entity: header only, its brush is never written
            float mn[3], mx[3];
            if (!hooks::BrushBounds(b, mn, mx)) continue;
            bool flat = false; for (int k = 0; k < 3; ++k) if (mx[k] - mn[k] < 0.01f) flat = true;
            if (flat) continue;                                                    // a brush still being dragged out, wait until it has volume
            byOwner[owner].push_back(b); wanted.insert(guid);
        }
        for (auto& kv : byOwner)
        {
            std::string text;
            if (!hooks::WriteEntityTextWith(kv.first, kv.second, text)) continue;
            Entity parsed; std::string err;
            if (!ParseEntity(text, parsed, &err) || parsed.guid.empty()) continue;
            Entity* m = g_model.FindEntity(parsed.guid);
            if (!m && parsed.classname == "worldspawn") m = ModelWorld();
            size_t order = m ? size_t(m - &g_model.entities[0]) : g_model.entities.size();
            bool newEntity = false;
            if (!m)
            {
                g_model.entities.push_back(Entity{ parsed.guid, parsed.header, {}, parsed.classname });
                m = &g_model.entities.back(); newEntity = true;
                SendEntityUpsert(*m, order);
            }
            else if (kv.second.empty()) continue;                                  // a known fixed-size entity: nothing new
            size_t sentBrushes = 0;
            for (const Brush& b : parsed.brushes)
            {
                if (!wanted.count(b.guid) || g_model.FindBrush(b.guid)) continue;
                m->brushes.push_back(b); SendBrushUpsert(m->guid, b); ++sentBrushes;
            }
            if (newEntity || sentBrushes) LOG("sync: announced %s%s (%s) while still selected: %zu new brush(es)", newEntity ? "new entity " : "", parsed.guid.c_str(), parsed.classname.c_str(), sentBrushes);
        }
    }

    // Key/value edits of an entity still selected (property editor). The header is sent once it is the same on two
    // passes (settled, not a rotate drag). "origin" is left out (moves are the live drag's job), only the header goes out.
    std::string HeaderWithoutOrigin(const std::string& header)
    {
        std::string out; out.reserve(header.size());
        size_t i = 0;
        while (i < header.size())
        {
            size_t e = header.find('\n', i); if (e == std::string::npos) e = header.size();
            if (header.compare(i, 9, "\"origin\" ") != 0) out.append(header, i, e - i).push_back('\n');
            i = e + 1;
        }
        return out;
    }

    void CaptureSelectedKeys(const std::vector<hooks::SelectedBrush>& sel, const std::map<std::string, hooks::RadPtr>& entPtr, const std::set<std::string>& captured)
    {
        const size_t kMaxEntities = 16;   // a writer call per entity per pass, a big selection waits for the deselect
        std::set<std::string> owners;
        for (const auto& s : sel) if (!s.entityGuid.empty() && !captured.count(s.entityGuid)) owners.insert(s.entityGuid);
        std::map<std::string, std::string> seen;
        if (!Cfg().syncLiveKeys || g_silent || owners.size() > kMaxEntities) { g_liveKeys.clear(); return; }
        std::set<std::string> held;
        bool heldRead = false;
        for (const auto& eg : owners)
        {
            if (IsWorldGuid(eg)) continue;                          // worldspawn keys go through the reload path
            Entity* m = g_model.FindEntity(eg);
            if (!m || m->classname == "worldspawn") continue;       // new entities are AnnounceSelectedNew's
            auto it = entPtr.find(eg);
            if (it == entPtr.end()) continue;
            std::string text;
            if (!hooks::WriteEntityText(it->second, text)) continue;
            Entity parsed; std::string err;
            if (!ParseEntity(text, parsed, &err) || parsed.guid != eg) continue;
            if (parsed.header == m->header || HeaderWithoutOrigin(parsed.header) == HeaderWithoutOrigin(m->header)) continue;
            auto prev = g_liveKeys.find(eg);
            if (prev == g_liveKeys.end() || HeaderWithoutOrigin(prev->second) != HeaderWithoutOrigin(parsed.header)) { seen[eg] = parsed.header; continue; }
            if (!heldRead) { held = presence::HeldByPeers(); heldRead = true; }
            if (held.count(eg)) continue;                           // a peer holds it too
            m->header = parsed.header; m->classname = parsed.classname;
            SendEntityUpsert(*m, size_t(m - &g_model.entities[0])); ++g_liveKeySends;
            LOG("sync: entity keys changed %s (%s) while selected, sent live", eg.c_str(), parsed.classname.c_str());
        }
        g_liveKeys.swap(seen);
    }

    void Capture(uint64_t now)
    {
        g_cs = CaptureStats();
        double tA = NowMs();
        SyncWorldGuid();
        // model membership as hash sets (the model's Find* are linear scans)
        std::unordered_set<std::string> modelEntities;
        std::unordered_set<std::string> modelBrushes;
        for (const auto& e : g_model.entities) { modelEntities.insert(e.guid); for (const auto& b : e.brushes) modelBrushes.insert(b.guid); }
        // live sets
        std::set<std::string> liveEntities;
        std::map<std::string, std::string> liveBrushOwner;          // brush guid -> entity guid
        std::map<std::string, hooks::RadPtr> entPtr;
        std::unordered_map<std::string, size_t> liveBrushCount;
        hooks::ForEachEntity([&](hooks::RadPtr ent)
        {
            std::string eg = hooks::EntityGuid(ent);
            if (eg.empty()) return;
            liveEntities.insert(eg); entPtr[eg] = ent;
            size_t n = 0;
            hooks::ForEachEntityBrush(ent, [&](hooks::RadPtr br) { std::string bg = hooks::BrushGuid(br); if (!bg.empty()) { liveBrushOwner[bg] = eg; ++n; } });
            liveBrushCount[eg] = n;
        });
        // A point entity always has one invisible "empty brush" the writer never emits: remember it as a writer skip
        // instead of re-serializing every point entity (3000 writer calls = 10 s)
        std::unordered_map<std::string, size_t> modelBrushCount;
        for (const auto& e : g_model.entities) modelBrushCount[e.guid] = e.brushes.size();
        for (const auto& kv : liveBrushOwner)
        {
            if (modelBrushes.count(kv.first) || g_writerSkips.count(kv.first)) continue;
            auto mc = modelBrushCount.find(kv.second); auto lc = liveBrushCount.find(kv.second);
            if (mc != modelBrushCount.end() && mc->second == 0 && lc != liveBrushCount.end() && lc->second == 1) g_writerSkips.insert(kv.first);
        }
        double tB = NowMs(); g_cs.walk = tB - tA;
        std::vector<hooks::SelectedBrush> sel;
        hooks::GetSelection(sel);
        g_cs.sel = NowMs() - tB;
        std::set<std::string> selected;
        std::set<std::string> touchedOwners;
        NoteSelection(sel);
        for (const auto& s : sel)
        {
            selected.insert(s.brushGuid);
            liveBrushOwner[s.brushGuid] = s.entityGuid;             // selected brushes are unlinked from their entity
            if (!modelBrushes.count(s.brushGuid) && !g_writerSkips.count(s.brushGuid)) touchedOwners.insert(s.entityGuid);   // brand new brush
        }
        // brushes released from the selection: their owners were just edited
        for (const auto& g : g_prevSelected)
            if (!selected.count(g)) { auto it = liveBrushOwner.find(g); if (it != liveBrushOwner.end()) touchedOwners.insert(it->second); }
        // entities unknown to the model, or with brushes unknown to the model
        for (const auto& eg : liveEntities) if (!modelEntities.count(eg)) touchedOwners.insert(eg);
        for (const auto& kv : liveBrushOwner) if (!selected.count(kv.first) && !modelBrushes.count(kv.first) && !g_writerSkips.count(kv.first)) touchedOwners.insert(kv.second);
        // aligning after a scope switch: re-read every entity so unshared edits end up in the model unbroadcast
        if (g_silent) for (const auto& eg : liveEntities) touchedOwners.insert(eg);

        double tC = NowMs();
        for (const auto& eg : touchedOwners)
        {
            auto it = entPtr.find(eg);
            if (it == entPtr.end()) continue;
            double t0 = NowMs();
            CaptureEntity(it->second, selected);
            double dt = NowMs() - t0; ++g_cs.nCaptured;
            if (dt > g_cs.slowest) { g_cs.slowest = dt; const Entity* me = g_model.FindEntity(eg); g_cs.slowestClass = me ? me->classname : "?"; }
        }
        CaptureSelectedKeys(sel, entPtr, touchedOwners);
        g_cs.captured = NowMs() - tC;
        double tD = NowMs();

        // deletions (not while the brush is merely selected, nor while remote changes are merged but not yet applied)
        std::vector<std::string> deadBrushes, deadEntities;
        if (g_dirty && !g_silent) { g_prevSelected = selected; return; }
        // A deletion is "explained" when the object was ever selected here (Radiant's Delete and the undo of a paste only
        // remove selected objects). Explained deletions always go out, however many (Shift+A on a model, Delete).
        std::vector<std::string> oddBrushes, oddEntities;
        for (const auto& e : g_model.entities)
        {
            if (e.classname == "worldspawn")
            {
                for (const auto& b : e.brushes)
                    if (!liveBrushOwner.count(b.guid) && !selected.count(b.guid)) (g_everSelected.count(b.guid) ? deadBrushes : oddBrushes).push_back(b.guid);
                continue;
            }
            if (!liveEntities.count(e.guid))
            {
                if (HeldByFile(e.guid)) continue;      // waiting for its prefab file: in the model, not yet in Radiant
                bool seen = g_everSelected.count(e.guid) != 0;
                for (const auto& b : e.brushes) if (!seen && g_everSelected.count(b.guid)) seen = true;
                (seen ? deadEntities : oddEntities).push_back(e.guid);
                continue;
            }
            for (const auto& b : e.brushes)
                if (!liveBrushOwner.count(b.guid) && !selected.count(b.guid)) (g_everSelected.count(b.guid) ? deadBrushes : oddBrushes).push_back(b.guid);
        }
        // Safety valve: a diff bug must never wipe a peer's map. Over a quarter of the model (and over a handful)
        // vanishing without ever being selected is held back, explained deletions still go out.
        size_t totalBrushes = 0; for (const auto& e : g_model.entities) totalBrushes += e.brushes.size();
        size_t odd = oddBrushes.size() + oddEntities.size(), total = totalBrushes + g_model.entities.size();
        g_heldDeletes = 0;
        if (odd > 8 && total && odd * 4 > total && !g_silent && !g_forceDeletes)
        {
            static uint64_t lastWarn = 0;
            if (now - lastWarn > 5000) { lastWarn = now; LOG("sync: HOLDING BACK %zu deletions of objects never selected here (%zu objects in the model); looks like a diff error, not an edit ('TeamCreate deletes send' sends them)", odd, total); }
            g_heldDeletes = odd;
        }
        else
        {
            if (g_forceDeletes && odd) LOG("sync: sending %zu held-back deletions on request", odd);
            deadBrushes.insert(deadBrushes.end(), oddBrushes.begin(), oddBrushes.end());
            deadEntities.insert(deadEntities.end(), oddEntities.begin(), oddEntities.end());
        }
        g_forceDeletes = false;
        size_t dead = deadBrushes.size() + deadEntities.size();
        if (dead > 8 && !g_silent) LOG("sync: %zu objects deleted at once (brushes %zu, entities %zu)", dead, deadBrushes.size(), deadEntities.size());
        bool quiet = dead > 8;   // one summary line instead of a line per object
        for (const auto& g : deadBrushes) { g_model.RemoveBrush(g); SendDelete(false, g); if (!g_silent && !quiet) LOG("sync: brush deleted %s", g.c_str()); }
        for (const auto& g : deadEntities) { g_model.RemoveEntity(g); SendDelete(true, g); if (!g_silent && !quiet) LOG("sync: entity deleted %s", g.c_str()); }
        if (g_silent) LOG("sync: model aligned with Radiant: %zu entities re-read, %zu stale object(s) dropped", touchedOwners.size(), dead);
        g_cs.dead = NowMs() - tD;

        g_prevSelected = selected;
    }

    // ---- merge ------------------------------------------------------------------------
    // Applies one remote op to a model, false when nothing changed. `touched` gets the GUIDs to rebuild,
    // `current` enables the worldspawn cases that only apply to the scope Radiant is editing.
    bool MergeInto(Model& m, const RemoteOp& op, std::set<std::string>& touched, bool current)
    {
        net::Reader r(op.payload);
        switch (op.type)
        {
        case net::MSG_ENTITY_UPSERT:
        {
            std::string scope = r.str(); std::string guid = r.str(); uint32_t order = r.u32(); std::string header = prefabs::Localize(r.blob());
            if (!r.ok || guid.empty()) return false;
            if (ClassnameOfHeader(header) == "worldspawn")
            {
                g_remoteWorldGuids.insert(guid);
                Entity* w = WorldOf(m);
                if (!w) return false;
                std::string h = ReplaceGuid(header, w->guid);
                if (h == w->header) return false;
                w->header = h;
                touched.insert(w->guid);                                 // SyntheticOps turns this into g_worldKeysPending
                if (current) { g_worldKeysPending = true; LOG("sync: worldspawn keys updated from peer (applied on the next idle reload; Radiant's import ignores keys-only worldspawn blocks)"); }
                return true;                                             // never a second worldspawn
            }
            Entity* e = m.FindEntity(guid);
            if (e) { e->header = header; e->classname = ClassnameOfHeader(header); }
            else
            {
                Entity ne; ne.guid = guid; ne.header = header; ne.classname = ClassnameOfHeader(header);
                size_t at = order < m.entities.size() ? order : m.entities.size();
                m.entities.insert(m.entities.begin() + at, ne);
            }
            touched.insert(guid);
            return true;
        }
        case net::MSG_BRUSH_UPSERT:
        {
            std::string scope = r.str(); std::string entGuid = r.str(); std::string guid = r.str(); std::string text = r.blob();
            if (!r.ok || guid.empty()) return false;
            Entity* w = WorldOf(m);
            if (w && (entGuid == w->guid || g_remoteWorldGuids.count(entGuid))) entGuid = w->guid;
            Entity* owner = nullptr;
            Brush* b = m.FindBrush(guid, &owner);
            if (b && owner && owner->guid == entGuid) { b->text = text; touched.insert(guid); return true; }
            if (b) m.RemoveBrush(guid);
            Entity* e = m.FindEntity(entGuid);
            if (!e) { LOG("sync: brush %s for unknown entity %s ignored", guid.c_str(), entGuid.c_str()); return false; }
            e->brushes.push_back(Brush{ guid, text });
            touched.insert(guid);
            return true;
        }
        case net::MSG_DELETE:
        {
            std::string scope = r.str(); uint8_t kind = r.u8(); std::string guid = r.str();
            if (!r.ok) return false;
            Entity* w = WorldOf(m);
            if (kind == 0 && ((w && w->guid == guid) || g_remoteWorldGuids.count(guid))) { LOG("sync: ignoring delete of a worldspawn guid"); return false; }
            if (kind == 0) m.RemoveEntity(guid); else m.RemoveBrush(guid);
            touched.insert(guid);
            return true;
        }
        default: return false;
        }
    }

    // GUIDs whose block differs between two models (entities by header, brushes by text/owner, plus removals)
    std::set<std::string> DiffGuids(const Model& a, const Model& b)
    {
        std::set<std::string> out;
        std::map<std::string, const Entity*> ea, eb;
        std::map<std::string, std::pair<const Entity*, const Brush*>> ba, bb;
        for (const auto& e : a.entities) { ea[e.guid] = &e; for (const auto& br : e.brushes) ba[br.guid] = { &e, &br }; }
        for (const auto& e : b.entities) { eb[e.guid] = &e; for (const auto& br : e.brushes) bb[br.guid] = { &e, &br }; }
        for (const auto& kv : ea) { auto it = eb.find(kv.first); if (it == eb.end() || it->second->header != kv.second->header) out.insert(kv.first); }
        for (const auto& kv : eb) if (!ea.count(kv.first)) out.insert(kv.first);
        for (const auto& kv : ba) { auto it = bb.find(kv.first); if (it == bb.end() || it->second.second->text != kv.second.second->text || it->second.first->guid != kv.second.first->guid) out.insert(kv.first); }
        for (const auto& kv : bb) if (!ba.count(kv.first)) out.insert(kv.first);
        return out;
    }

    // Ops that make Radiant match the model for the given GUIDs (the model is the truth for them).
    std::deque<RemoteOp> SyntheticOps(Model& m, const std::set<std::string>& guids)
    {
        std::deque<RemoteOp> ops;
        for (const auto& g : guids)
        {
            const Entity* e = m.FindEntity(g);
            if (e)
            {
                if (e->classname == "worldspawn") { g_worldKeysPending = true; continue; }
                net::Writer w; w.str(g_scopeKey); w.str(g); w.u32(0); w.blob("");
                ops.push_back(RemoteOp{ net::MSG_ENTITY_UPSERT, w.buf, 0 });
                continue;
            }
            Entity* owner = nullptr;
            if (m.FindBrush(g, &owner) && owner)
            {
                net::Writer w; w.str(g_scopeKey); w.str(owner->guid); w.str(g); w.blob("");
                ops.push_back(RemoteOp{ net::MSG_BRUSH_UPSERT, w.buf, 0 });
                continue;
            }
            // gone from the model: whichever kind it was, both deletes are no-ops when nothing matches
            net::Writer w1; w1.str(g_scopeKey); w1.u8(0); w1.str(g); ops.push_back(RemoteOp{ net::MSG_DELETE, w1.buf, 0 });
            net::Writer w2; w2.str(g_scopeKey); w2.u8(1); w2.str(g); ops.push_back(RemoteOp{ net::MSG_DELETE, w2.buf, 0 });
        }
        return ops;
    }

    // ---- in-place apply --------------------------------------------------
    struct LiveIndex
    {
        std::map<std::string, hooks::RadPtr> entities;                 // guid -> entity_t*
        std::map<std::string, hooks::RadPtr> brushes;                  // guid -> brush_t* (entity lists only)
        std::map<std::string, std::vector<hooks::RadPtr>> entBrushes;  // entity guid -> its brush pointers
        std::map<std::string, hooks::RadPtr> brushOwner;              // brush guid -> owning entity_t*
        std::set<std::string> selectedGuids;
        std::string worldGuid;                                         // the model's worldspawn
        hooks::RadPtr worldPtr = nullptr;                              // the live one (its GUID changes on import)
    };

    LiveIndex BuildIndex()
    {
        LiveIndex ix;
        hooks::ForEachEntity([&](hooks::RadPtr ent)
        {
            std::string eg = hooks::EntityGuid(ent);
            if (eg.empty()) return;
            ix.entities[eg] = ent;
            hooks::ForEachEntityBrush(ent, [&](hooks::RadPtr br) { std::string bg = hooks::BrushGuid(br); if (!bg.empty()) { ix.brushes[bg] = br; ix.brushOwner[bg] = ent; ix.entBrushes[eg].push_back(br); } });
        });
        std::vector<hooks::SelectedBrush> sel; hooks::GetSelection(sel);
        for (const auto& s : sel) { ix.selectedGuids.insert(s.brushGuid); ix.selectedGuids.insert(s.entityGuid); }
        for (const auto& e : g_model.entities) if (e.classname == "worldspawn") { ix.worldGuid = e.guid; break; }
        ix.worldPtr = hooks::WorldEntity();
        return ix;
    }

    std::string WorldBrushImportText(const std::string& brushText)
    {
        return "{\n\"classname\" \"worldspawn\"\n// brush 0\n" + brushText + "}\n";
    }
    // Map_ImportBuffer merges the FIRST block into worldspawn (fresh GUIDs): a leading empty worldspawn block avoids that
    std::string EntityImportText(const Entity& e)
    {
        return "{\n\"classname\" \"worldspawn\"\n}\n" + SerializeEntity(e);
    }

    // Classes whose runtime instance Radiant builds only on its load path (the clipboard import skips it, the lighting
    // pass then asserts on probeInst) go through the reload path. Configurable: [sync] reload_classes.
    bool NeedsReloadClass(const std::string& classname)
    {
        static std::set<std::string> classes;
        static bool built = false;
        if (!built)
        {
            built = true;
            std::string list = Narrow(Cfg().syncReloadClasses);
            size_t i = 0;
            while (i <= list.size())
            {
                size_t c = list.find(',', i);
                std::string one = list.substr(i, c == std::string::npos ? std::string::npos : c - i);
                size_t a = one.find_first_not_of(" \t");
                size_t b = one.find_last_not_of(" \t");
                if (a != std::string::npos)
                {
                    one = one.substr(a, b - a + 1);
                    for (auto& ch : one) ch = char(tolower(static_cast<unsigned char>(ch)));
                    classes.insert(one);
                }
                if (c == std::string::npos) break;
                i = c + 1;
            }
            std::string joined; for (const auto& c2 : classes) { if (!joined.empty()) joined += ","; joined += c2; }
            LOG("sync: classes routed through the reload path: %s", joined.empty() ? "(none)" : joined.c_str());
        }
        if (classes.empty() || classname.empty()) return false;
        std::string k = classname;
        for (auto& ch : k) ch = char(tolower(static_cast<unsigned char>(ch)));
        return classes.count(k) != 0;
    }

    void DeferToReload(const std::string& guid, const std::string& classname, const char* what)
    {
        if (!g_reloadClassPending)
            LOG("sync: %s %s (%s) needs Radiant's own load path; applying on the next idle reload", what, guid.c_str(), classname.c_str());
        g_reloadClassPending = true;
    }

    // ---- undo identity ----
    // A peer's change is applied as delete + import (BeginUntracked, no undo step). Undo finds live objects by undoId/redoId (plus entityId on saved brush copies),
    // so the replacement takes over the replaced object's tags, else Ctrl+Z would undo the import itself and the object would vanish for everyone.
    struct UndoIds
    {
        bool hasEntity = false;
        std::array<int, 3> entity{ { 0, 0, 0 } };
        std::map<std::string, std::array<int, 3>> brushes;     // brush GUID -> tags
        std::vector<std::array<int, 3>> unnamed;               // a fixed-size entity's invisible brush (GUID differs per PC)
    };

    UndoIds ReadUndoIds(hooks::RadPtr ent, const std::vector<hooks::RadPtr>& brushes)
    {
        UndoIds u;
        if (ent) u.hasEntity = hooks::GetUndoTags(ent, true, u.entity.data());
        bool fixed = ent && hooks::EntityFixedSize(ent);
        for (hooks::RadPtr b : brushes)
        {
            std::array<int, 3> t{ { 0, 0, 0 } };
            if (!hooks::GetUndoTags(b, false, t.data())) continue;
            if (fixed) u.unnamed.push_back(t); else u.brushes[hooks::BrushGuid(b)] = t;
        }
        return u;
    }

    // After an import (its objects are the selection): old tags onto objects with the same GUID, zero tags on new ones.
    // Brushes find their tags through their owner: `byEntity` by entity GUID, `world` for world brushes. Worldspawn is left alone.
    void WriteUndoIds(const std::map<std::string, UndoIds>& byEntity, const UndoIds* world)
    {
        if (!hooks::HasUntracked()) return;
        hooks::RadPtr worldEnt = hooks::WorldEntity();
        std::map<hooks::RadPtr, size_t> nextUnnamed;
        std::set<hooks::RadPtr> owners;
        size_t carried = 0;
        for (hooks::RadPtr inst : hooks::SelectedBrushPtrs())
        {
            hooks::RadPtr b = hooks::BrushOfInst(inst);
            if (!b) continue;
            hooks::RadPtr owner = hooks::BrushOwner(b);
            const UndoIds* u = world;
            if (owner != worldEnt)
            {
                owners.insert(owner);
                auto it = byEntity.find(hooks::EntityGuid(owner));
                u = it != byEntity.end() ? &it->second : nullptr;
            }
            std::array<int, 3> t{ { 0, 0, 0 } };
            if (u)
            {
                auto it = u->brushes.find(hooks::BrushGuid(b));
                size_t& next = nextUnnamed[owner];
                if (it != u->brushes.end()) { t = it->second; ++carried; }
                else if (next < u->unnamed.size()) { t = u->unnamed[next++]; ++carried; }
            }
            hooks::SetUndoTags(b, false, t.data());
        }
        for (hooks::RadPtr owner : owners)
        {
            auto it = byEntity.find(hooks::EntityGuid(owner));
            std::array<int, 3> t{ { 0, 0, 0 } };
            if (it != byEntity.end() && it->second.hasEntity) t = it->second.entity;
            else { hooks::GetUndoTags(owner, true, t.data()); t[0] = t[1] = 0; }   // keep the entityId Radiant gave it
            hooks::SetUndoTags(owner, true, t.data());
        }
        if (Cfg().verbose) LOG("sync:   undo identity: %zu entit%s, %zu brush tag(s) carried over", owners.size(), owners.size() == 1 ? "y" : "ies", carried);
    }

    // Radiant's import remaps target/targetname and linkto/linkname pairs inside one buffer: entities with those keys
    // are imported on their own so names arrive unchanged
    bool HasLinkKeys(const Entity& e)
    {
        for (const char* k : { "\n\"target\" ", "\n\"targetname\" ", "\n\"linkto\" ", "\n\"linkname\" " })
            if (e.header.find(k) != std::string::npos) return true;
        return false;
    }

    // In-place apply. A mass paste or delete arrives as one op per object and one by one costs 75 to 400 ms each
    // (280 deleted models froze a PC for 82 s). The model holds the end state: every touched object is removed with
    // ONE DeleteBrushes and re-created with one import per kind. Ops whose target is selected go to g_retry.
    void ApplyOpsInPlace(std::deque<RemoteOp>& ops)
    {
        if (ops.empty()) return;
        double t0 = NowMs();
        g_applying = true;
        std::vector<hooks::RadPtr> keep = hooks::SelectedBrushPtrs();
        LiveIndex ix = BuildIndex();

        // what each op touches: "e<guid>" = rebuild an entity from the model, "w<guid>" = a world brush,
        // "b<guid>" = remove one brush of an entity, "" = nothing to do in place
        std::vector<std::string> keys(ops.size());
        std::set<std::string> deferKeys;
        for (size_t i = 0; i < ops.size(); ++i)
        {
            const RemoteOp& op = ops[i];
            net::Reader r(op.payload);
            std::string key; bool selected = false;
            if (op.type == net::MSG_ENTITY_UPSERT)
            {
                std::string scope = r.str(); std::string guid = r.str();
                Entity* e = IsWorldGuid(guid) ? nullptr : g_model.FindEntity(guid);   // worldspawn keys use the reload path
                if (e && NeedsReloadClass(e->classname)) DeferToReload(guid, e->classname, "entity");
                else if (e) { key = "e" + guid; selected = ix.selectedGuids.count(guid) != 0; }
            }
            else if (op.type == net::MSG_BRUSH_UPSERT)
            {
                std::string scope = r.str(); std::string entGuid = r.str(); std::string guid = r.str();
                Entity* owner = nullptr;
                Brush* b = g_model.FindBrush(guid, &owner);
                if (b && owner)
                {
                    selected = ix.selectedGuids.count(guid) || ix.selectedGuids.count(entGuid);
                    if (owner->guid == ix.worldGuid) key = "w" + guid;
                    else if (NeedsReloadClass(owner->classname)) { DeferToReload(owner->guid, owner->classname, "brush of"); selected = false; }
                    else key = "e" + owner->guid;                    // brush of another entity: replace the whole entity
                }
            }
            else if (op.type == net::MSG_DELETE)
            {
                std::string scope = r.str(); uint8_t kind = r.u8(); std::string guid = r.str();
                selected = ix.selectedGuids.count(guid) != 0;
                if (kind == 0) { if (!IsWorldGuid(guid) && ix.entBrushes.count(guid)) key = "e" + guid; }
                else
                {
                    auto ow = ix.brushOwner.find(guid);
                    if (ow != ix.brushOwner.end()) key = (ow->second == ix.worldPtr ? "w" : "b") + guid;
                }
            }
            keys[i] = key;
            if (!key.empty() && selected) deferKeys.insert(key);
        }

        // touched objects in first-seen order, the rest of an object's ops wait with it when selected
        std::vector<std::string> entities, worldBrushes, brushDeletes;
        std::set<std::string> seen;
        size_t done = 0, deferred = 0;
        for (size_t i = 0; i < ops.size(); ++i)
        {
            const std::string& k = keys[i];
            if (deferKeys.count(k)) { g_retry.push_back(ops[i]); ++deferred; continue; }
            ++done;
            if (k.empty() || !seen.insert(k).second) continue;
            (k[0] == 'e' ? entities : k[0] == 'w' ? worldBrushes : brushDeletes).push_back(k.substr(1));
        }
        std::set<std::string> entitySet(entities.begin(), entities.end());

        // remove every live copy at once, keeping the undo tags (see UndoIds)
        std::map<std::string, UndoIds> undo;
        UndoIds worldUndo;
        std::vector<hooks::RadPtr> del;
        std::set<hooks::RadPtr> delSet;
        auto remove = [&](hooks::RadPtr b) { if (delSet.insert(b).second) del.push_back(b); };
        for (const auto& g : entities)
        {
            auto bit = ix.entBrushes.find(g);
            if (bit == ix.entBrushes.end()) continue;
            auto eit = ix.entities.find(g);
            undo[g] = ReadUndoIds(eit != ix.entities.end() ? eit->second : nullptr, bit->second);
            for (hooks::RadPtr b : bit->second) remove(b);
        }
        for (const auto& g : worldBrushes)
        {
            auto bit = ix.brushes.find(g);
            if (bit == ix.brushes.end()) continue;
            UndoIds u = ReadUndoIds(nullptr, { bit->second });
            worldUndo.brushes.insert(u.brushes.begin(), u.brushes.end());
            remove(bit->second);
        }
        for (const auto& g : brushDeletes)
        {
            auto ow = ix.brushOwner.find(g);
            if (ow == ix.brushOwner.end() || entitySet.count(hooks::EntityGuid(ow->second))) continue;   // its entity is rebuilt anyway
            remove(ix.brushes[g]);
        }

        hooks::BeginUntracked();                                     // a peer's edit: no local undo step
        bool deleted = del.empty() || hooks::DeleteBrushes(del, "teamcreate");
        double t1 = NowMs();
        size_t worldIn = 0, entIn = 0, singles = 0;
        if (!deleted)
        {
            LOG("sync: in-place delete of %zu brush(es) failed; %zu change(s) retried", del.size(), done);
            for (size_t i = 0; i < ops.size(); ++i) if (!deferKeys.count(keys[i])) g_retry.push_back(ops[i]);
            done = 0;
        }
        else
        {
            // world brushes: one worldspawn block holding all of them (restamped in selection order = text order)
            std::string text = "{\n\"classname\" \"worldspawn\"\n";
            std::vector<std::string> guids;
            for (const auto& g : worldBrushes)
            {
                Entity* owner = nullptr;
                Brush* b = g_model.FindBrush(g, &owner);
                if (!b || !owner || owner->guid != ix.worldGuid) continue;
                text += "// brush " + std::to_string(guids.size()) + "\n" + b->text;
                guids.push_back(g);
            }
            if (!guids.empty())
            {
                hooks::DeselectAll();
                bool ok = hooks::ImportMapText(text + "}\n", "teamcreate") && hooks::RestampImportedGuids("", guids);
                if (ok) { WriteUndoIds({}, &worldUndo); worldIn = guids.size(); hooks::PublishToPlayTest(); }
                else { LOG("sync: in-place import of %zu world brush(es) could not be restamped; the next idle reload fixes them", guids.size()); g_reloadClassPending = true; }
                hooks::DeselectAll();                                // the import leaves its objects selected, not "user is editing"
            }

            // entities: one import for the batch, entities with link keys (or an old host) one by one
            std::vector<Entity*> batch, single;
            for (const auto& g : entities)
            {
                Entity* e = g_model.FindEntity(g);
                if (!e) continue;                                    // deleted: removing it was all
                (HasLinkKeys(*e) || !hooks::CanRestampImportedEntities() ? single : batch).push_back(e);
            }
            if (batch.size() == 1) { single.push_back(batch[0]); batch.clear(); }
            if (!batch.empty())
            {
                std::string etext = "{\n\"classname\" \"worldspawn\"\n}\n";   // Map_ImportBuffer merges the first block into worldspawn
                std::vector<std::string> eg;
                std::vector<std::vector<std::string>> bg;
                for (Entity* e : batch)
                {
                    etext += SerializeEntity(*e);
                    eg.push_back(e->guid);
                    bg.emplace_back();
                    for (const auto& b : e->brushes) bg.back().push_back(b.guid);
                }
                hooks::DeselectAll();
                size_t n = hooks::ImportMapText(etext, "teamcreate") ? hooks::RestampImportedEntities(eg, bg) : 0;
                if (n == batch.size()) { WriteUndoIds(undo, nullptr); entIn += n; hooks::PublishToPlayTest(); }
                else { LOG("sync: in-place import of %zu entities restamped %zu; the next idle reload fixes them", batch.size(), n); g_reloadClassPending = true; }
                hooks::DeselectAll();
            }
            for (Entity* e : single)
            {
                hooks::DeselectAll();
                std::vector<std::string> bg; for (const auto& b : e->brushes) bg.push_back(b.guid);
                bool ok = hooks::ImportMapText(EntityImportText(*e), "teamcreate") && hooks::RestampImportedGuids(e->guid, bg);
                if (ok) { WriteUndoIds(undo, nullptr); ++entIn; ++singles; hooks::PublishToPlayTest(); }
                else { LOG("sync: in-place import of %s (%s) could not be restamped; the next idle reload fixes it", e->guid.c_str(), e->classname.c_str()); g_reloadClassPending = true; }
                hooks::DeselectAll();
            }
        }
        hooks::EndUntracked();
        hooks::DeselectAll();
        if (!keep.empty()) hooks::SelectBrushes(keep);             // targets under the selection were deferred, so these survived
        g_applying = false;
        if (done)
        {
            g_applies += unsigned(done);
            LOG("sync: %zu remote change(s) applied in place%s: %zu brush(es) removed, %zu world brush(es) and %zu entit%s (%zu alone) imported, %.0f ms (delete %.0f ms)",
                done, deferred ? " (some deferred: target selected)" : "", del.size(), worldIn, entIn, entIn == 1 ? "y" : "ies", singles, NowMs() - t0, t1 - t0);
        }
        ops.clear();
    }

    // Reload path: write the model to the file and let Radiant re-read it (also re-reads prefabs)
    void ApplyToRadiant(const char* why)
    {
        if (g_depth > 0) return;
        // never reload under a held mouse button: freelook would stay hidden (the release is swallowed), flags stay set
        if (hooks::MouseButtonHeld())
        {
            static uint64_t lastNote = 0; uint64_t now = GetTickCount64();
            if (now - lastNote > 5000) { lastNote = now; LOG("sync: reload path (%s) waits for the mouse button to be released", why); }
            return;
        }
        // the user's file is replaced by the merged model: back it up first, a failed copy asks the user (posted)
        backup::Gate gate = backup::BeforeWriteLater(g_scopePath);
        if (gate == backup::Gate::Wait) { SetStatus("WAITING: NO BACKUP"); return; }
        if (gate == backup::Gate::Skip)
        {
            g_retry.clear(); g_worldKeysPending = false; g_rootReloadPending = false; g_reloadClassPending = false; g_dirty = false;
            SetWarning("NOT APPLIED: NO BACKUP OF THE MAP");
            static uint64_t lastNote = 0; uint64_t now = GetTickCount64();
            if (now - lastNote > 5000) { lastNote = now; LOG("sync: reload path (%s) dropped: %S could not be backed up and the user said No", why, g_scopePath.c_str()); }
            return;
        }
        g_applying = true;
        std::string text = Serialize(g_model);
        fw::NoteSelfWriteBegin();
        bool ok = WriteFileAtomic(g_scopePath, text);
        fw::NoteSelfWriteEnd();
        if (!ok) { LOG("sync: cannot write %S", g_scopePath.c_str()); g_applying = false; return; }
        LOG("sync: reload path (%s): wrote %zu bytes, reloading", why, text.size());
        backup::NoteWritten(g_scopePath);   // no second copy while the file stays as is
        {
            // always tell the user their file on disk was replaced (a save they did not make)
            size_t slash = g_scopePath.find_last_of(L"\\/");
            presence::Notice("Team Create rewrote " + Narrow(slash == std::wstring::npos ? g_scopePath : g_scopePath.substr(slash + 1)) + " on disk and reloaded it (" + why + "). Earlier versions: radiant\\teamcreate_backup.");
        }
        g_retry.clear(); g_worldKeysPending = false; g_rootReloadPending = false; g_reloadClassPending = false;
        if (!hooks::ReloadMapNow(g_scopePath))   // triggers OnMapLoaded, the model is re-read
        { g_rootReloadPending = true; LOG("sync: reload refused by the host; retrying once quiet"); }
        g_dirty = false; g_recvOps = 0; ++g_applies;
        g_applying = false;
    }

    // ---- other scopes -----------------------------------------------------------------
    Scope& ScopeFor(const std::string& key)
    {
        auto it = g_scopes.find(key);
        if (it != g_scopes.end()) return it->second;
        Scope& s = g_scopes[key];
        s.path = PathOfKey(key);
        s.fromDisk = true;
        if (s.path.empty() || !LoadModelFromDisk(s.path, s.model)) { LOG("sync: scope '%s': no local file (%S); starting empty", key.c_str(), s.path.c_str()); s.model.fileHeader = "iwmap 4\n"; }
        else LOG("sync: scope '%s' loaded from %S (%zu entities)", key.c_str(), s.path.c_str(), s.model.entities.size());
        return s;
    }

    void WriteScopeFile(const std::string& key, Scope& s, bool reloadNeeded = true)
    {
        if (key == g_rootKey || s.path.empty()) return;          // the root file is written by the reload path only
        // only prefab files are rewritten while not being edited, any other map stays in memory
        if (key.rfind("_prefabs/", 0) != 0) { LOG("sync: scope '%s' is not a prefab; kept in memory only", key.c_str()); return; }
        backup::Gate gate = backup::BeforeWriteLater(s.path);
        if (gate == backup::Gate::Wait) { g_scopeUnwritten[key] = reloadNeeded; return; }     // retried from Tick after the answer
        if (gate == backup::Gate::Skip) { LOG("sync: scope '%s' not written to %S (no backup, the user said No); kept in memory", key.c_str(), s.path.c_str()); return; }
        fw::NoteSelfWriteBegin();
        bool ok = WriteFileAtomic(s.path, Serialize(s.model));
        fw::NoteSelfWriteEnd();
        LOG("sync: scope '%s' %s %S%s", key.c_str(), ok ? "written to" : "COULD NOT write", s.path.c_str(), reloadNeeded ? "" : " (a wanted file: no reload, its entities are applied in place)");
        if (ok)
        {
            backup::NoteWritten(s.path);
            // one chat line per file per minute while a peer edits inside a prefab
            static std::map<std::string, uint64_t> told; uint64_t now = GetTickCount64();
            if (now - told[key] >= 60000) { told[key] = now; presence::Notice("Team Create updated prefab " + key.substr(key.find_last_of('/') == std::string::npos ? 0 : key.find_last_of('/') + 1) + " on disk with another mapper's changes. Earlier versions: radiant\\teamcreate_backup."); }
        }
        if (ok && reloadNeeded) g_rootReloadPending = true;
    }

    void MergeOffline(const std::string& key, const RemoteOp& op)
    {
        if (!IsPrefabKey(key) && !SameScope(key, g_rootKey)) { NoteForeignRootScope(key, "edit"); return; }
        // store under our own key so the same map in another folder lands in our root scope
        const std::string sk = (!IsPrefabKey(key) && SameScope(key, g_rootKey)) ? g_rootKey : key;
        Scope& s = ScopeFor(sk);
        if (MergeInto(s.model, op, s.touched, false)) { ++g_recvOps; g_lastRemoteTick = GetTickCount64(); WriteScopeFile(sk, s); }
    }

    std::string ScopeOfOp(const RemoteOp& op)
    {
        net::Reader r(op.payload);
        return r.str();
    }

    std::string GuidOfOp(const RemoteOp& op)
    {
        net::Reader r(op.payload); r.str();
        if (op.type == net::MSG_ENTITY_UPSERT) return r.str();
        if (op.type == net::MSG_BRUSH_UPSERT) { r.str(); return r.str(); }
        if (op.type == net::MSG_DELETE) { r.u8(); return r.str(); }
        return "";
    }

    void HandleFullSync(const RemoteOp& op)
    {
        net::Reader r(op.payload);
        std::string key = r.str(); std::string text = prefabs::Localize(r.blob());
        if (!r.ok) return;
        Model m; std::string err;
        if (!ParseMap(text, m, &err)) { LOG("sync: full sync parse failed: %s", err.c_str()); return; }
        if (SameScope(key, g_scopeKey))
        {
            g_awaitFullSync = false;
            std::set<std::string> touched = DiffGuids(g_model, m);
            if (touched.empty()) { LOG("sync: full sync for '%s' (%zu entities) identical to ours; nothing to apply", key.c_str(), m.entities.size()); return; }
            // the peer's worldspawn GUID may differ (Radiant regenerates it on paste): keep ours
            Entity* mine = ModelWorld(); Entity* theirs = WorldOf(m);
            if (mine && theirs && mine->guid != theirs->guid) { g_remoteWorldGuids.insert(theirs->guid); theirs->header = ReplaceGuid(theirs->header, mine->guid); theirs->guid = mine->guid; touched.erase(theirs->guid); touched = DiffGuids(g_model, m); }
            g_model = m;
            std::deque<RemoteOp> ops = SyntheticOps(g_model, touched);
            LOG("sync: full sync for '%s' (%zu entities): %zu object(s) differ, applying in place", key.c_str(), m.entities.size(), touched.size());
            ApplyOpsInPlace(ops);
            g_dirty = !g_retry.empty() || g_worldKeysPending;
            g_lastRemoteTick = GetTickCount64();
            ++g_recvOps;
            return;
        }
        if (!IsPrefabKey(key) && !SameScope(key, g_rootKey)) { NoteForeignRootScope(key, "full sync"); return; }
        Scope& s = ScopeFor(key);
        std::set<std::string> touched = DiffGuids(s.model, m);
        if (touched.empty()) { LOG("sync: full sync for '%s' matches our copy", key.c_str()); return; }
        s.model = m;
        for (const auto& g : touched) s.touched.insert(g);
        LOG("sync: full sync for '%s' stored (%zu object(s) differ)", key.c_str(), touched.size());
        std::string lk = key; for (auto& c : lk) c = char(tolower(uint8_t(c)));
        WriteScopeFile(key, s, g_wanted.count(lk) == 0);       // a file we asked for: held entities import it
    }

    void SendFullSync(uint32_t peerId, const std::string& key, const Model& m)
    {
        std::string text = prefabs::Delocalize(Serialize(m));
        net::Writer w; w.str(key); w.blob(text);
        net::SendTo(peerId, net::MSG_FULL_SYNC, w.buf);
        LOG("sync: full '%s' (%zu bytes) sent to peer %u", key.c_str(), text.size(), peerId);
    }

    // ---- wanted prefab files ------------------------------------------------------------------
    // Prefab keys an entity upsert refers to whose file is missing here become wanted. Runs before the merge, so on a
    // temporary map PathOfKey marks a new key mirrored and Localize points the header at the mirror.
    std::vector<std::string> MissingPrefabsOf(const RemoteOp& op)
    {
        if (op.type != net::MSG_ENTITY_UPSERT) return {};
        net::Reader r(op.payload); r.str(); r.str(); r.u32(); std::string header = r.blob();
        if (!r.ok) return {};
        std::vector<std::string> out;
        for (const auto& key : prefabs::Refs(header))
        {
            std::wstring path = PathOfKey(key);
            if (path.empty() || GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
            out.push_back(key);
            if (!g_wanted.count(key))
            {
                Wanted w; w.firstTick = GetTickCount64(); w.from = op.from; w.path = path; g_wanted[key] = w;
                LOG("sync: an entity from peer %u uses prefab '%s', which is not on this PC; asking for the file (it goes to %S)", op.from, key.c_str(), path.c_str());
            }
        }
        return out;
    }
    void TickWantedFiles(uint64_t now)
    {
        if (g_wanted.empty()) return;
        std::map<uint32_t, std::vector<std::string>> ask;   // by the peer to ask
        for (auto it = g_wanted.begin(); it != g_wanted.end();)
        {
            Wanted& w = it->second;
            if (GetFileAttributesW(w.path.c_str()) != INVALID_FILE_ATTRIBUTES) { LOG("sync: prefab '%s' is here now", it->first.c_str()); it = g_wanted.erase(it); continue; }
            if (now - w.firstTick > 60000) { LOG("sync: prefab '%s' did not arrive within 60 s; its entities are applied without it (invisible until the file exists and the map is reloaded)", it->first.c_str()); it = g_wanted.erase(it); continue; }
            if (now - w.lastRequest >= 5000) { w.lastRequest = now; ask[net::IsHost() ? w.from : net::kHostPeerId].push_back(it->first); }
            ++it;
        }
        for (const auto& kv : ask)
        {
            net::Writer wr; wr.u16(uint16_t(kv.second.size())); for (const auto& k : kv.second) wr.str(k);
            if (net::IsHost()) net::SendTo(kv.first, net::MSG_FILE_REQUEST, wr.buf); else net::Send(net::MSG_FILE_REQUEST, wr.buf);
            LOG("sync: asked peer %u for %zu prefab file(s): %s", kv.first, kv.second.size(), kv.second[0].c_str());
        }
    }
    // held ops whose files are here (or given up on) go back to the apply queue in order
    void ReleaseHeldOps(std::deque<RemoteOp>& apply)
    {
        for (auto it = g_awaitFile.begin(); it != g_awaitFile.end();)
        {
            bool waiting = false; for (const auto& k : it->keys) waiting |= g_wanted.count(k) != 0;
            if (waiting) { ++it; continue; }
            apply.push_back(it->op); it = g_awaitFile.erase(it);
        }
    }
}

namespace mapsync
{
    void Init() { LOG("sync: %s", Cfg().syncEnabled ? "enabled" : "disabled"); }

    void OnMapLoaded(const std::wstring& fullPath)
    {
        g_ready = false;
        g_rootPath = fullPath; g_rootKey = ScopeKey(fullPath);
        g_scopePath = fullPath; g_scopeKey = g_rootKey; g_depth = 0;
        g_scopes.clear(); g_scopeUnwritten.clear(); g_retry.clear(); g_rootReloadPending = false; g_worldKeysPending = false; g_reloadClassPending = false;
        g_alignPending = false;
        g_wanted.clear(); g_awaitFile.clear();
        if (!g_blocked) SetWarning("");        // a blocked client keeps its "different map" line
        if (g_rootKey.rfind("_teamcreate_temp/", 0) == 0) { size_t n = prefabs::RescanMirror(); if (n) LOG("sync: temporary map; %zu mirrored prefab(s)", n); }
        else prefabs::ClearMirror();
        uint64_t t0 = GetTickCount64();
        if (!LoadModelFromDisk(fullPath, g_model)) { LOG("sync: cannot read %S", fullPath.c_str()); return; }
        size_t brushes = 0; for (auto& e : g_model.entities) brushes += e.brushes.size();
        LOG("sync: model built from %S: %zu entities, %zu brushes (scope '%s') in %llu ms", fullPath.c_str(), g_model.entities.size(), brushes, g_scopeKey.c_str(), (unsigned long long)(GetTickCount64() - t0));
        g_prevSelected.clear(); g_liveKeys.clear();
        g_writerSkips.clear();
        // a map opened while the session runs: keep the pre-session version like OnSessionStarted (no copy after a reload-path write)
        if (presence::IsActive() && g_rootKey.rfind("_teamcreate_temp/", 0) != 0) backup::Snapshot(fullPath);
        g_ready = true;
        if (!g_applying) g_dirty = false;
    }

    // Radiant is the truth for the current scope (edits made before the session, or before the scope was shared):
    // every entity is re-read through Radiant's writer without broadcasting.
    void AlignScope()
    {
        g_alignPending = false;
        g_silent = true;
        Capture(GetTickCount64());
        g_silent = false;
    }

    void BeforeScopeChange()
    {
        // outside a session nothing reads the model, nothing to commit
        if (!g_ready || !Cfg().syncEnabled || !net::IsConnected()) return;
        // commit the selection (selected brushes are unlinked): deselect, capture, re-select
        std::vector<hooks::RadPtr> keep = hooks::SelectedBrushPtrs();
        if (!keep.empty()) hooks::DeselectAll();
        Capture(GetTickCount64());
        if (!keep.empty()) hooks::SelectBrushes(keep);
    }

    void OnScopeChanged(const std::wstring& fullPath, int depth)
    {
        if (!g_ready) return;
        std::string key = ScopeKey(fullPath);
        if (key == g_scopeKey) { g_depth = depth; return; }
        // stash the scope we are leaving (deferred in-place ops become "touched")
        {
            Scope& old = g_scopes[g_scopeKey];
            old.model = g_model; old.path = g_scopePath; old.fromDisk = false;
            for (const auto& op : g_retry) { std::string g = GuidOfOp(op); if (!g.empty()) old.touched.insert(g); }
            g_retry.clear();
        }
        // load the new one
        std::set<std::string> touched; bool fromDisk;
        {
            Scope& s = ScopeFor(key);
            g_model = s.model; touched = s.touched; fromDisk = s.fromDisk;
            g_scopes.erase(key);
        }
        g_scopeKey = key; g_scopePath = fullPath; g_depth = depth;
        g_prevSelected.clear(); g_writerSkips.clear(); g_liveKeys.clear();
        LOG("sync: scope now '%s' (depth %d, model %s, %zu entities, %zu object(s) changed remotely meanwhile)", key.c_str(), depth, fromDisk ? "from disk" : "from this session", g_model.entities.size(), touched.size());
        // remote changes that arrived meanwhile: the model is the truth for them
        if (!touched.empty())
        {
            std::deque<RemoteOp> ops = SyntheticOps(g_model, touched);
            ApplyOpsInPlace(ops);
        }
        // everything else: Radiant is the truth. Outside a session the re-read (0.8 s on 3000 entities) waits for the first session tick
        if (net::IsConnected()) AlignScope(); else g_alignPending = true;
        g_dirty = !g_retry.empty() || g_worldKeysPending;
        if (g_depth == 0 && g_rootReloadPending) LOG("sync: prefab files changed while you were inside a prefab; the map reloads when you are idle");
    }

    void OnScopeSaved() { /* the user saved the prefab file: the model already matches Radiant */ }

    void Tick()
    {
        // blocked runs without a map too: a client on an empty map still writes the prefabs and temporary copy it asked for
        if (!Cfg().syncEnabled || (!g_ready && !g_blocked) || !net::IsConnected()) return;
        uint64_t now = GetTickCount64();
        if (now - g_lastTick < 500) return;
        g_lastTick = now;

        if (g_blocked)
        {
            // edits for a map we do not have open are dropped (the full sync after the switch is the whole map),
            // except the full map asked for as a temporary copy (SetTempTarget), which is written to disk
            std::deque<RemoteOp> dropped;
            { std::lock_guard<std::mutex> lock(g_mutex); dropped.swap(g_pending); }
            for (const auto& op : dropped)
            {
                if (op.type != net::MSG_FULL_SYNC) continue;
                net::Reader r(op.payload); std::string key = r.str(); std::string text = r.blob();
                if (!r.ok) continue;
                std::string lk = key; for (auto& c : lk) c = char(tolower(uint8_t(c)));
                if (g_expectedFiles.count(lk))
                {
                    // a prefab we asked for: into the mirror, its references pointed there too
                    std::wstring path = prefabs::MirrorPath(lk);
                    bool ok = !path.empty() && WriteFileAtomic(path, prefabs::Localize(text));
                    LOG("sync: prefab '%s' (%zu bytes) from the host %s %S", lk.c_str(), text.size(), ok ? "written to the mirror" : "COULD NOT be written to", path.c_str());
                    g_expectedFiles.erase(lk);
                    if (g_expectedFiles.empty()) { auto done = g_filesDone; g_filesDone = nullptr; if (done) done(); }
                    continue;
                }
                if (g_tempKey.empty() || !SameScope(key, g_tempKey)) continue;
                bool ok = WriteFileAtomic(g_tempPath, prefabs::Localize(text));
                LOG("sync: the host's full map '%s' (%zu bytes) %s %S", key.c_str(), text.size(), ok ? "written to the temporary copy" : "COULD NOT be written to", g_tempPath.c_str());
                auto done = g_tempDone; g_tempKey.clear(); g_tempPath.clear(); g_tempDone = nullptr;
                if (done) done(ok);
            }
            SetStatus("SYNC OFF: DIFFERENT MAP");
            return;
        }
        if (g_alignPending) AlignScope();

        // remote changes first: merge into the model of their scope, then apply the current scope's in place
        std::deque<RemoteOp> ops;
        { std::lock_guard<std::mutex> lock(g_mutex); ops.swap(g_pending); }
        std::deque<RemoteOp> apply;
        for (const auto& op : ops)
        {
            if (op.type == net::MSG_FULL_SYNC) { HandleFullSync(op); continue; }
            std::vector<std::string> missing = MissingPrefabsOf(op);
            std::string scope = ScopeOfOp(op);
            if (SameScope(scope, g_scopeKey))
            {
                std::set<std::string> touched;
                if (MergeInto(g_model, op, touched, true))
                {
                    if (missing.empty()) apply.push_back(op); else g_awaitFile.push_back(HeldOp{ op, missing });
                    g_dirty = true; g_lastRemoteTick = now; ++g_recvOps;
                }
            }
            else MergeOffline(scope, op);
        }
        TickWantedFiles(now);
        if (!g_awaitFile.empty()) ReleaseHeldOps(apply);
        if (!g_retry.empty()) { for (auto it = g_retry.rbegin(); it != g_retry.rend(); ++it) apply.push_front(*it); g_retry.clear(); }
        if (!apply.empty() && Cfg().syncInPlace) ApplyOpsInPlace(apply);
        if (Cfg().syncInPlace) g_dirty = !g_retry.empty() || g_worldKeysPending || g_reloadClassPending;

        std::vector<hooks::SelectedBrush> sel; hooks::GetSelection(sel);
        if (g_awaitFullSync)
        {
            if (now - g_awaitSince < 10000) { SetStatus("WAITING FOR HOST"); return; }
            g_awaitFullSync = false; LOG("sync: no full sync from the host within 10 s; sharing local state anyway");
        }
        if (!g_scopeUnwritten.empty())
        {
            auto waiting = std::move(g_scopeUnwritten); g_scopeUnwritten.clear();
            for (const auto& w : waiting) { auto it = g_scopes.find(w.first); if (it != g_scopes.end()) WriteScopeFile(w.first, it->second, w.second); }
        }
        bool idle = now - g_lastRemoteTick >= Cfg().syncApplyDelayMs && sel.empty();
        bool quiet = now - g_lastRemoteTick >= 3000 && sel.empty();     // a reload is expensive: wait longer after prefab edits
        if (g_depth == 0 && ((idle && g_dirty) || (quiet && g_rootReloadPending))) { ApplyToRadiant(g_rootReloadPending ? "prefab files changed" : g_worldKeysPending ? "worldspawn keys" : g_reloadClassPending ? "an entity needs Radiant's load path" : "deferred changes"); return; }

        LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
        Capture(now);
        QueryPerformanceCounter(&t1);
        double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
        if (ms > g_maxCaptureMs) g_maxCaptureMs = ms;
        static uint64_t lastPerfLog = 0;
        static double windowMax = 0; if (ms > windowMax) windowMax = ms;
        if (now - lastPerfLog > 60000 && windowMax > 20)
        {
            lastPerfLog = now;
            LOG("sync: capture pass cost over the last minute: max %.0f ms (walk %.0f ms this pass, %zu entities)", windowMax, g_cs.walk, g_model.entities.size());
            windowMax = 0;
        }
        if (ms > 100 && now - lastPerfLog > 10000)
        {
            lastPerfLog = now;
            size_t brushes = 0; for (const auto& e : g_model.entities) brushes += e.brushes.size();
            LOG("sync: capture pass took %.0f ms (%zu entities, %zu brushes in the model); max %.0f ms; walk %.0f sel %.0f captured %zu in %.0f (slowest %.0f ms, %s) dead %.0f", ms, g_model.entities.size(), brushes, g_maxCaptureMs,
                g_cs.walk, g_cs.sel, g_cs.nCaptured, g_cs.captured, g_cs.slowest, g_cs.slowestClass.c_str(), g_cs.dead);
        }
        char buf[160];
        char held[48] = "";
        if (g_heldDeletes) _snprintf_s(held, _TRUNCATE, "  %zu DELETES HELD", g_heldDeletes);
        if (!g_wanted.empty()) _snprintf_s(held, _TRUNCATE, "  GETTING %zu PREFAB(S)", g_wanted.size());
        _snprintf_s(buf, _TRUNCATE, "SENT %u  RECV %u%s%s%s", g_sentUpserts + g_sentDeletes, g_applies, (g_dirty || g_rootReloadPending) ? "  PENDING" : "", g_depth > 0 ? "  PREFAB" : "", held);
        SetStatus(buf);
    }

    void AnnounceSelectedNew() { AnnounceSelectedNewImpl(); }
    bool Settled()
    {
        { std::lock_guard<std::mutex> lock(g_mutex); if (!g_pending.empty()) return false; }
        return g_retry.empty() && g_awaitFile.empty() && !g_dirty && !g_rootReloadPending;
    }
    void NoteSelected(const std::vector<hooks::SelectedBrush>& sel) { NoteSelection(sel); }
    std::string DeletesCommand(const std::string& arg)
    {
        if (arg == "send") { g_forceDeletes = true; return "the next capture pass sends the " + std::to_string(g_heldDeletes) + " held-back deletion(s)"; }
        return "held=" + std::to_string(g_heldDeletes) + " everSelected=" + std::to_string(g_everSelected.size()) + " sentDeletes=" + std::to_string(g_sentDeletes);
    }

    void OnSessionStarted(bool host)
    {
        // Radiant's saves during the session overwrite the map with no copy: keep the pre-session version (not for a temporary copy)
        if (!TempSession()) backup::Snapshot(hooks::CurrentMapPath());
        if (host && g_ready && g_alignPending) AlignScope();     // before any full sync is built
        g_awaitFullSync = !host;
        g_awaitSince = GetTickCount64();
    }

    void OnMessage(const net::Message& m)
    {
        if (m.type < net::MSG_ENTITY_UPSERT || m.type > net::MSG_FULL_SYNC) return;
        if (m.from == net::LocalPeerId()) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pending.push_back(RemoteOp{ m.type, m.payload, m.from });
    }

    void OnPeerJoined(uint32_t peerId)
    {
        if (!net::IsHost() || !Cfg().syncEnabled || !g_ready) return;
        // a slightly stale full sync is corrected by the deltas
        if (g_depth > 0) { auto it = g_scopes.find(g_rootKey); if (it != g_scopes.end()) SendFullSync(peerId, g_rootKey, it->second.model); }
        SendFullSync(peerId, g_scopeKey, g_model);
    }

    std::string Status() { std::lock_guard<std::mutex> lock(g_mutex); return g_status; }
    std::string Warning() { std::lock_guard<std::mutex> lock(g_mutex); return g_warning; }
    void Warn(const std::string& text) { SetWarning(text); }

    std::string  KeyOfPath(const std::wstring& fullPath) { return ScopeKey(fullPath); }
    std::string  RootKey() { return g_ready ? g_rootKey : std::string(); }
    bool         SameMap(const std::string& a, const std::string& b) { return SameScope(a, b); }
    std::wstring PathForKey(const std::string& key) { return PathOfKey(key); }
    void SetBlocked(bool blocked)
    {
        if (g_blocked == blocked) return;
        g_blocked = blocked;
        if (!blocked) { std::lock_guard<std::mutex> lock(g_mutex); g_pending.clear(); g_status = "idle"; }
        if (blocked) { g_wanted.clear(); g_awaitFile.clear(); }   // whatever arrived for the other map
        LOG("sync: %s", blocked ? "OFF: this map is not the host's" : "on again: same map as the host");
    }
    bool Blocked() { return g_blocked; }
    void SetTempTarget(const std::string& key, const std::wstring& path, std::function<void(bool)> done) { g_tempKey = key; g_tempPath = path; g_tempDone = std::move(done); }
    void ExpectFiles(const std::vector<std::string>& keys, std::function<void()> done)
    {
        g_expectedFiles.clear();
        for (auto k : keys) { for (auto& c : k) c = char(tolower(uint8_t(c))); g_expectedFiles.insert(k); }
        g_filesDone = std::move(done);
    }
    std::string AssetText()
    {
        if (!g_ready) return {};
        std::string text = Serialize(g_model);
        for (const auto& scope : g_scopes) if (scope.first != g_scopeKey) text += Serialize(scope.second.model);
        return text;
    }
    std::vector<prefabs::Item> HostInventory()
    {
        if (!g_ready) return {};
        std::string rootText;
        if (g_depth > 0) { auto it = g_scopes.find(g_rootKey); if (it != g_scopes.end()) rootText = Serialize(it->second.model); }
        else rootText = Serialize(g_model);
        return prefabs::Inventory(rootText);
    }
    void SendPrefabFile(uint32_t peerId, const std::string& key)
    {
        std::string text;
        if (key == g_scopeKey) text = Serialize(g_model);
        else { auto it = g_scopes.find(key); if (it != g_scopes.end()) text = Serialize(it->second.model); }
        if (text.empty()) { std::wstring p = PathOfKey(key); if (p.empty() || !prefabs::ReadBytes(p, text)) { LOG("sync: peer %u asked for '%s' but there is no such file here", peerId, key.c_str()); return; } }
        text = prefabs::Delocalize(text);
        net::Writer w; w.str(key); w.blob(text);
        net::SendTo(peerId, net::MSG_FULL_SYNC, w.buf);
        LOG("sync: prefab '%s' (%zu bytes) sent to peer %u", key.c_str(), text.size(), peerId);
    }
    void OnMapClosed()
    {
        g_ready = false; g_rootKey.clear(); g_scopeKey.clear(); g_rootPath.clear(); g_scopePath.clear(); g_depth = 0;
        g_scopes.clear(); g_retry.clear(); g_model = Model(); g_wanted.clear(); g_awaitFile.clear();
        { std::lock_guard<std::mutex> lock(g_mutex); g_pending.clear(); }
        SetWarning("");
        LOG("sync: no root map (empty map); waiting for the next load");
    }
}
