#include <windows.h>
#include <mutex>
#include <atomic>
#include <map>
#include <array>
#include <cmath>
#include <deque>
#include <fstream>
#include "presence.h"
#include "net.h"
#include "compat.h"
#include "mapsync.h"
#include "savesync.h"
#include "zonesync.h"
#include "lightsync.h"
#include "assets.h"
#include "diag.h"
#include "backup.h"
#include "prefabs.h"

// MAP (grep the names, line numbers drift):
//   State ..................... g_mutex guards g_peers, g_chat, g_conflicts, g_hostKey, g_inventory (written on
//                               the net thread), g_localName, g_prefabChain, g_scopeAnnounce, g_started,
//                               g_endedByHost, g_endReason, g_wasConnected
//   Map match state ........... g_hostKey, g_blocked, g_prompting, g_declinedKey, g_askAfterSave, g_switchTarget,
//                               g_hostWarned
//   Temporary host map ........ g_tempPath, g_tempKey, g_tempRequested, g_switchPath, g_lastSavedPath, g_closeFilter,
//                               g_leaving, g_switching, g_switchAttempts, Fetch and g_fetch (prefab inventory +
//                               files before the switch)
//   Radiant globals ........... kAddr_g_pMainWin, kAddr_g_modified (read through host->RadiantAddress)
//   Helpers ................... PushChat, Narrow, ToWide, TempPathFor, DropTempFile, ClearModifiedFlag
//   Map match / switch ........ ResetMapMatch, BeginSwitch, OnTempMapWritten, TrySwitch, TickMapMatch (main thread)
//   Incoming messages ......... PeerRef, OnMessage (net thread, under g_mutex: one case per net::MSG_* type)
//   namespace presence (public)
//     session ................. g_sessHost, g_sessBind, g_sessPort, ResolveName, StartHost, StartClient, Start,
//                               Stop, SessionText
//     ticks (main thread) ..... TickSelection (g_lastSelKey, g_lastSelSet), TickLiveMove (peer drags,
//                               g_live* counters), Tick
//     scope / map events ...... Snapshot, OnScopeChanged, PrefabChainText, OnMapLoaded, OnMapSaved
//     temporary map exit ...... LeaveTempMap, CloseFilter (QEvent::Close, never swallowed), InstallCloseWatch,
//                               MapStatus
//     colors .................. SetLocalColor, ParseColor, ColorHex
//     pane text / chat ........ HeldByPeers, LiveMoveStatus, SendChat, Notice, ChatHistory, InterpolatedPose

namespace
{
    std::mutex                         g_mutex;
    std::map<uint32_t, presence::Peer> g_peers;
    std::string                        g_localName;
    uint64_t                           g_lastSendTick = 0;
    std::vector<std::string>           g_prefabChain;          // main thread: the prefabs we are inside, outermost first
    std::vector<presence::PrefabXf>    g_prefabXf;             // main thread: per level, the entered instance's origin/angles in the outer scope
    bool                               g_scopeAnnounce = false; // under g_mutex: send MSG_SCOPE on the next tick
    uint64_t                           g_lastScopeSend = 0;
    float                              g_lastOrigin[3] = { 0, 0, 0 }, g_lastAngles[3] = { 0, 0, 0 };
    bool                               g_started = false;
    std::atomic<bool>                  g_endedByHost{ false };  // set by MSG_BYE on the net thread
    std::string                        g_endReason;             // shown in the pane after a session ended on its own
    uint64_t                           g_lastReconnectTick = 0;
    uint64_t                           g_dialSince = 0;          // client: when StartClient began dialing (the first connect gives up after a minute)
    bool                               g_wasConnected = false;  // client: IsConnected() seen true since the last (re)connect
    std::vector<presence::Conflict>    g_conflicts;
    std::deque<presence::ChatLine>     g_chat;

    // Map match: is the map this Radiant has open the host's? (host: which peers are elsewhere)
    std::string g_hostKey;              // the host's root map as last announced (WELCOME, MAP_INFO), under g_mutex
    bool        g_blocked = false;      // client, main thread: our map differs from the host's, sync is off
    bool        g_prompting = false;    // the Yes/No box is up (the idle tick keeps running underneath it)
    std::string g_declinedKey;          // the host map the user chose not to open (asked again only for another map)
    bool        g_askAfterSave = false; // the user said No because of unsaved changes: ask again once they saved
    std::string g_switchTarget;         // host map we agreed to open, retried each tick until ReloadMap accepts
    uint64_t    g_lastSwitchNote = 0;
    uint64_t    g_lastHostCheck = 0;
    bool        g_hostWarned = false;   // host: the pane warning about peers on other maps is ours

    // Temporary copy of the host's map: a client with no file at the host's path asks for the map (MSG_MAP_REQUEST),
    // mapsync writes the answer to map_source\_teamcreate_temp\<same relative path>, the switch opens it from there.
    std::wstring g_tempPath;            // "" = none
    std::string  g_tempKey;             // the host key it stands for
    bool         g_tempRequested = false;
    uint64_t     g_tempRequestTick = 0;
    std::wstring g_switchPath;          // where g_switchTarget is loaded from when not from PathForKey (the temporary copy)
    std::wstring g_lastSavedPath;       // the last file Radiant wrote (OnMapSaved), cleared before a Save As we ask for
    void*        g_closeFilter = nullptr;
    bool         g_leaving = false;     // LeaveTempMap's box is up
    bool         g_switching = false;   // inside the ReloadMap of a switch (its OnMapLoaded must not start another one)
    int          g_switchAttempts = 0;  // reloads of the same host map that did not leave us on it
    std::string  g_switchAttemptKey;
    // Prefab check before the switch (prefabs.cpp): inventory from the host, missing/different files into the mirror, then the map.
    enum class Fetch { None, Inventory, Files, Map };
    Fetch        g_fetch = Fetch::None;
    uint64_t     g_fetchTick = 0;
    std::vector<prefabs::Item> g_inventory;   // from the net thread, under g_mutex
    bool         g_inventoryArrived = false;  // under g_mutex
    bool         g_filesArrived = false;      // main thread (mapsync callback)

    // Radiant globals read through host->RadiantAddress (same values as src/radiant_addrs.h)
    constexpr uintptr_t kAddr_g_pMainWin = 0x1444C6560;   // RadiantMainWin* (a QMainWindow: the object the close watch filters)
    constexpr uintptr_t kAddr_g_modified = 0x14521EB94;   // int: Radiant's unsaved-changes flag

    void PushChat(uint32_t from, const std::string& name, const std::string& text)
    {
        g_chat.push_back(presence::ChatLine{ GetTickCount64(), from, name, text });
        while (g_chat.size() > 200) g_chat.pop_front();
        LOG("chat: <%s> %s", name.c_str(), text.c_str());
    }

    uint32_t Resolve(uint32_t id, uint32_t chosen) { return chosen ? (chosen | 0xFF000000) : presence::AutoColor(id); }

    std::string Narrow(const std::wstring& w)
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n ? n - 1 : 0, '\0');
        if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
        return s;
    }

    std::wstring ToWide(const std::string& s)
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n ? n - 1 : 0, L'\0');
        if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
        return w;
    }

    // What we tell the peers we have open: the scope key ("zm/zm_escape.map"), empty before the map loaded.
    std::string LocalMapKey() { return mapsync::RootKey(); }

    std::wstring LowerPath(std::wstring p) { for (auto& c : p) { if (c == L'/') c = L'\\'; c = wchar_t(towlower(c)); } return p; }
    bool SameFile(const std::wstring& a, const std::wstring& b) { return !a.empty() && !b.empty() && LowerPath(a) == LowerPath(b); }

    // map_source\_teamcreate_temp\<key with backslashes>, next to where the real file would be ("" = no map_source known)
    std::wstring TempPathFor(const std::string& key)
    {
        std::wstring real = mapsync::PathForKey(key);
        size_t at = LowerPath(real).find(L"\\map_source\\");
        if (at == std::wstring::npos) return L"";
        return real.substr(0, at + 12) + L"_teamcreate_temp\\" + real.substr(at + 12);
    }

    bool OnTempMapNow() { return !g_tempPath.empty() && SameFile(hooks::CurrentMapPath(), g_tempPath); }

    void ForgetTemp() { g_tempPath.clear(); g_tempKey.clear(); g_tempRequested = false; g_switchPath.clear(); g_fetch = Fetch::None; g_filesArrived = false; }

    // Delete the temporary copy: the map file and Radiant's .bak of it, with `wholeFolder` also the mirrored
    // prefabs and the folder (the session is over). Leaving for another map keeps the folder: the next map's
    // copy and its prefabs may already be in it.
    void DropTempFile(const char* why, bool wholeFolder)
    {
        if (g_tempPath.empty()) return;
        bool ok = DeleteFileW(g_tempPath.c_str()) != 0;
        LOG("presence: temporary copy %S %s (%s)", g_tempPath.c_str(), ok ? "deleted" : "could not be deleted", why);
        size_t dot = g_tempPath.find_last_of(L'.');
        if (dot != std::wstring::npos) DeleteFileW((g_tempPath.substr(0, dot) + L".bak").c_str());
        if (wholeFolder) prefabs::DeleteMirrorFolder();
        ForgetTemp();
    }

    void ClearModifiedFlag()
    {
        int* p = rp::host->RadiantAddress ? static_cast<int*>(rp::host->RadiantAddress(rp::self, kAddr_g_modified)) : nullptr;
        if (p) *p = 0;
    }

    void ResetMapMatch()
    {
        { std::lock_guard<std::mutex> lock(g_mutex); g_hostKey.clear(); }
        if (g_blocked) { g_blocked = false; mapsync::SetBlocked(false); mapsync::Warn(""); }
        g_prompting = false; g_declinedKey.clear(); g_askAfterSave = false; g_switchTarget.clear(); g_switchPath.clear(); g_hostWarned = false;
        if (g_tempRequested) { g_tempRequested = false; mapsync::SetTempTarget("", L"", nullptr); }   // a copy still on its way is not wanted any more
        if (g_fetch != Fetch::None) { g_fetch = Fetch::None; mapsync::ExpectFiles({}, nullptr); }
        g_switchAttempts = 0; g_switchAttemptKey.clear();
        { std::lock_guard<std::mutex> lock(g_mutex); g_inventory.clear(); g_inventoryArrived = false; }
        g_filesArrived = false;
    }

    // Main thread, from a posted job: our map is not the host's, and the host's map is at `path` (its real place or the temporary copy).
    // With nothing to lose the switch happens, with unsaved changes the user decides: save first, discard, or keep their map.
    void BeginSwitch(const std::string& hostKey, const std::string& local, const std::wstring& tempPath)
    {
        bool go = false;
        bool modified = rp::host->IsMapModified(rp::self) != 0;
        if (!modified) { go = true; LOG("presence: opening the host's map '%s'%s", hostKey.c_str(), tempPath.empty() ? "" : " (temporary copy)"); }
        else
        {
            std::wstring mine = local.empty() ? L"an unsaved map" : L"'" + ToWide(local) + L"'";
            std::wstring text = L"The host has '" + ToWide(hostKey) + L"' open. You have " + mine + L" open with unsaved changes.\n\nSave your changes first?\n\n"
                L"Yes: save, then open the host's map.\nNo: discard the changes and open the host's map.\nCancel: keep your map (cameras and chat keep working, edits are not shared).";
            int r = MessageBoxW(mt::MainWindow(), text.c_str(), L"Team Create", MB_YESNOCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND);
            if (r == IDYES)
            {
                g_lastSavedPath.clear();
                rp::host->TriggerMenuAction(rp::self, OnTempMapNow() ? "File/Save As" : "File/Save");    // modal for a Save As (also what an unnamed map gets)
                if (rp::host->IsMapModified(rp::self) && g_lastSavedPath.empty()) LOG("presence: save cancelled; keeping the current map, edits are not shared");
                else { go = true; LOG("presence: changes saved; opening the host's map '%s'", hostKey.c_str()); }
            }
            else if (r == IDNO) { go = true; LOG("presence: unsaved changes discarded; opening the host's map '%s'", hostKey.c_str()); }
            else LOG("presence: user keeps their map; edits are not shared with the host's '%s'", hostKey.c_str());
        }
        if (!go)
        {
            if (!tempPath.empty()) { DeleteFileW(tempPath.c_str()); LOG("presence: temporary copy %S removed again", tempPath.c_str()); }
            g_declinedKey = hostKey;
        }
        else
        {
            if (!g_tempPath.empty() && !SameFile(g_tempPath, tempPath)) DropTempFile("leaving it for another map", false);
            if (!tempPath.empty()) { g_tempPath = tempPath; g_tempKey = hostKey; g_switchPath = tempPath; }
            g_switchTarget = hostKey;
        }
        g_tempRequested = false;
        g_fetch = Fetch::None;
        g_prompting = false;
    }

    // mapsync wrote the host's map to the temporary path (main thread, from its Tick).
    void OnTempMapWritten(const std::string& hostKey, const std::wstring& temp, bool ok)
    {
        if (!ok) { g_tempRequested = false; g_declinedKey = hostKey; mapsync::Warn("Could not write a temporary copy of the host's '" + hostKey + "'. Edits are not shared."); return; }
        mapsync::Warn("Opening a temporary copy of the host's '" + hostKey + "'.");
        g_prompting = true;
        std::string local = LocalMapKey();
        mt::Post([hostKey, local, temp] { BeginSwitch(hostKey, local, temp); });
    }

    // Main thread: the reload we agreed to, retried until the host accepts it (not inside a prefab, no
    // mouse button held, no reload in progress). Camera not kept: it is another map.
    void TrySwitch(uint64_t now)
    {
        std::string hostKey; { std::lock_guard<std::mutex> lock(g_mutex); hostKey = g_hostKey; }
        if (!mapsync::SameMap(g_switchTarget, hostKey)) { LOG("presence: the host moved on to '%s'; not opening '%s'", hostKey.c_str(), g_switchTarget.c_str()); g_switchTarget.clear(); g_switchPath.clear(); return; }
        std::wstring path = g_switchPath.empty() ? mapsync::PathForKey(g_switchTarget) : g_switchPath;
        if (path.empty()) { LOG("presence: no path for '%s'; giving up the switch", g_switchTarget.c_str()); g_switchTarget.clear(); return; }
        const char* wait = hooks::PrefabDepth() > 0 ? "once you leave the prefab" : hooks::MouseButtonHeld() ? "once the mouse button is released" : hooks::IsReloading() ? "after the current reload" : nullptr;
        if (wait)
        {
            mapsync::Warn("Opening '" + g_switchTarget + "' " + wait + ".");
            if (now - g_lastSwitchNote > 5000) { g_lastSwitchNote = now; LOG("presence: opening '%s' %s", g_switchTarget.c_str(), wait); }
            return;
        }
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) { LOG("presence: %S is gone; giving up the switch to '%s'", path.c_str(), g_switchTarget.c_str()); g_declinedKey = g_switchTarget; g_switchTarget.clear(); g_switchPath.clear(); return; }
        if (g_switchAttemptKey != g_switchTarget) { g_switchAttemptKey = g_switchTarget; g_switchAttempts = 0; }
        if (++g_switchAttempts > 2) { LOG("presence: '%s' did not load twice; giving up (see Radiant's console)", g_switchTarget.c_str()); mapsync::Warn("Could not open the host's '" + g_switchTarget + "'. Edits are not shared."); g_declinedKey = g_switchTarget; g_switchTarget.clear(); g_switchPath.clear(); return; }
        LOG("presence: opening the host's map %S", path.c_str());
        // the target is cleared before the load: Radiant's load pumps the idle tick and OnMapLoaded runs inside it,
        // and neither may start a second Map_LoadFile (a nested load asserts in fx_glue_material.cpp:264 "!s_mapIsLoading")
        std::string target = g_switchTarget; std::wstring spath = g_switchPath;
        g_switchTarget.clear(); g_switchPath.clear();
        g_switching = true;
        bool ok = rp::host->ReloadMap(rp::self, path.c_str(), 0) != 0;
        g_switching = false;
        if (!ok) { g_switchTarget = target; g_switchPath = spath; --g_switchAttempts; if (now - g_lastSwitchNote > 5000) { g_lastSwitchNote = now; LOG("presence: the host refused to open '%s'; retrying", target.c_str()); } }
    }

    // Main thread, every tick (cheap) and right after a map load: compare and act.
    void TickMapMatch(uint64_t now)
    {
        if (!g_started || !net::IsConnected()) return;
        std::string local = LocalMapKey();
        if (net::IsHost())
        {
            if (now - g_lastHostCheck < 1000 || local.empty()) return;
            g_lastHostCheck = now;
            std::string bad;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                for (const auto& kv : g_peers)
                    if (!kv.second.mapKey.empty() && !mapsync::SameMap(kv.second.mapKey, local))
                        bad += (bad.empty() ? "" : ", ") + kv.second.name + " has '" + kv.second.mapKey + "'";
            }
            if (!bad.empty()) { if (!g_hostWarned) LOG("presence: %s open; our map is '%s'", bad.c_str(), local.c_str()); mapsync::Warn(bad + " open, you have '" + local + "'. Their edits are not shared until they open your map."); g_hostWarned = true; }
            else if (g_hostWarned) { mapsync::Warn(""); g_hostWarned = false; }
            return;
        }
        std::string hostKey; { std::lock_guard<std::mutex> lock(g_mutex); hostKey = g_hostKey; }
        if (!g_switchTarget.empty())
        {
            if (mapsync::SameMap(g_switchTarget, local)) { g_switchTarget.clear(); g_switchPath.clear(); }   // arrived (our reload, or the user opened it)
            else if (g_switching || hooks::IsReloading()) return;                                          // inside a load: nothing until it is over
            else { TrySwitch(now); return; }
        }
        if (g_switching) { if (!local.empty() && mapsync::SameMap(hostKey, local) && g_blocked) { g_blocked = false; mapsync::SetBlocked(false); mapsync::Warn(""); mapsync::OnSessionStarted(false); LOG("presence: same map as the host ('%s'); waiting for the host's full map", local.c_str()); } return; }
        if (hostKey.empty()) return;                        // no welcome yet
        if (!local.empty() && mapsync::SameMap(hostKey, local))
        {
            if (g_blocked)
            {
                g_blocked = false; mapsync::SetBlocked(false); mapsync::Warn("");
                mapsync::OnSessionStarted(false);            // hold our capture until the host's full map is in
                LOG("presence: same map as the host ('%s'); waiting for the host's full map", local.c_str());
            }
            g_declinedKey.clear(); g_tempRequested = false;
            return;
        }
        // local is empty (an unnamed map) or another map: go to the host's
        if (!g_blocked)
        {
            g_blocked = true; mapsync::SetBlocked(true);
            mapsync::Warn("The host has '" + hostKey + "' open" + (local.empty() ? "." : ", you have '" + local + "'. Edits are not shared."));
            LOG("presence: the host has '%s' open, we have '%s'", hostKey.c_str(), local.empty() ? "(no map)" : local.c_str());
        }
        if (g_prompting || g_declinedKey == hostKey) return;
        // 1. the host's prefab inventory, 2. missing or different prefabs into the mirror, 3. the map itself:
        // the real file when nothing had to be mirrored, else a temporary copy that refers to the mirror
        if (g_fetch == Fetch::None)
        {
            net::Writer w; w.str(hostKey); net::Send(net::MSG_INVENTORY_REQUEST, w.buf);
            g_fetch = Fetch::Inventory; g_fetchTick = now;
            mapsync::Warn("Checking the prefabs of the host's '" + hostKey + "'...");
            LOG("presence: asked the host for the prefab inventory of '%s'", hostKey.c_str());
            return;
        }
        if (g_fetch == Fetch::Inventory)
        {
            bool arrived; std::vector<prefabs::Item> items;
            { std::lock_guard<std::mutex> lock(g_mutex); arrived = g_inventoryArrived; if (arrived) { items.swap(g_inventory); g_inventoryArrived = false; } }
            if (!arrived) { if (now - g_fetchTick < 15000) return; LOG("presence: no prefab inventory from the host within 15 s (older version?); going on without"); }
            std::vector<std::string> needed;
            for (const auto& it : items)
            {
                if (it.missing) { LOG("presence: the host itself lacks prefab '%s'; skipped", it.key.c_str()); continue; }
                std::string bytes; std::wstring real = prefabs::RealPath(it.key);
                bool same = !real.empty() && prefabs::ReadBytes(real, bytes) && uint32_t(bytes.size()) == it.size && prefabs::Hash(bytes) == it.hash;
                if (!same) { needed.push_back(it.key); LOG("presence: prefab '%s' %s here; mirroring it", it.key.c_str(), bytes.empty() ? "is missing" : "differs"); }
            }
            prefabs::ResetMirrorPrefabs();                     // stale copies from a previous map in this folder must not be picked up
            for (const auto& k : needed) prefabs::MarkMirrored(k);
            if (needed.empty()) { LOG("presence: all %zu prefab(s) of the host's map match this PC", items.size()); g_fetch = Fetch::Map; g_fetchTick = now; }
            else
            {
                g_filesArrived = false;
                mapsync::ExpectFiles(needed, [] { g_filesArrived = true; });
                net::Writer w; w.u16(uint16_t(needed.size())); for (const auto& k : needed) w.str(k);
                net::Send(net::MSG_FILE_REQUEST, w.buf);
                g_fetch = Fetch::Files; g_fetchTick = now;
                mapsync::Warn("Getting " + std::to_string(needed.size()) + " prefab(s) of '" + hostKey + "' from the host...");
                return;
            }
        }
        if (g_fetch == Fetch::Files)
        {
            if (!g_filesArrived)
            {
                if (now - g_fetchTick < 60000) return;
                LOG("presence: not every prefab arrived within 60 s; opening the map anyway");
                mapsync::ExpectFiles({}, nullptr);
            }
            g_filesArrived = false; g_fetch = Fetch::Map; g_fetchTick = now;
        }
        // Fetch::Map
        std::wstring path = mapsync::PathForKey(hostKey);
        bool exists = !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (exists && prefabs::MirrorEmpty())
        {
            g_prompting = true;
            mt::Post([hostKey, local] { BeginSwitch(hostKey, local, L""); });
            return;
        }
        // no copy of that map on this PC, or mirrored prefabs it must refer to: the host's map as a temporary copy
        if (!g_tempRequested)
        {
            std::wstring temp = TempPathFor(hostKey);
            if (temp.empty()) { g_declinedKey = hostKey; g_fetch = Fetch::None; mapsync::Warn("The host has '" + hostKey + "' open and no map_source folder is known here to put a copy in."); LOG("presence: no map_source folder known; cannot make a temporary copy of '%s'", hostKey.c_str()); return; }
            g_tempRequested = true; g_tempRequestTick = now;
            mapsync::SetTempTarget(hostKey, temp, [hostKey, temp](bool ok) { OnTempMapWritten(hostKey, temp, ok); });
            net::Writer w; w.str(hostKey); net::Send(net::MSG_MAP_REQUEST, w.buf);
            mapsync::Warn(std::string(exists ? "Some prefabs differ from the host's; " : "You have no copy of the host's '" + hostKey + "'; ") + "getting the map from the host as a temporary copy.");
            LOG("presence: %s; asked the host for '%s', temporary copy: %S", exists ? "mirrored prefabs in use" : "no copy of the map here", hostKey.c_str(), temp.c_str());
            return;
        }
        if (now - g_tempRequestTick > 60000)
        {
            g_tempRequested = false; g_declinedKey = hostKey; g_fetch = Fetch::None; mapsync::SetTempTarget("", L"", nullptr);
            mapsync::Warn("The host did not send '" + hostKey + "' within a minute. Edits are not shared.");
            LOG("presence: no full map for '%s' from the host within 60 s; giving up the temporary copy", hostKey.c_str());
        }
    }

    presence::Peer& PeerRef(uint32_t id)
    {
        auto it = g_peers.find(id);
        if (it == g_peers.end())
        {
            presence::Peer p; p.id = id; p.name = "peer" + std::to_string(id);
            p.color = presence::AutoColor(id);
            it = g_peers.emplace(id, p).first;
        }
        return it->second;
    }

    void OnMessage(const net::Message& m)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        switch (m.type)
        {
        case net::MSG_PEER_JOINED:
        {
            net::Reader r(m.payload);
            uint32_t id = r.u32(); std::string name = r.str();
            uint32_t color = r.u32(); if (!r.ok) color = 0;
            std::string map = r.str(); if (!r.ok) map.clear();
            if (id == net::LocalPeerId()) break;
            presence::Peer& p = PeerRef(id);
            p.name = name; p.lastSeenTick = GetTickCount64();
            p.color = Resolve(id, color);
            p.mapKey = map;
            g_scopeAnnounce = true;                                  // tell the newcomer where we are editing
            LOG("presence: peer %u '%s' joined (color %s, map '%s')", id, name.c_str(), presence::ColorHex(p.color).c_str(), map.c_str());
            if (net::IsHost())
            {
                // the full map goes to a newcomer on our map, or one that has not loaded yet (it is applied
                // after its load), one on another map gets it when it announces ours (MSG_MAP_INFO)
                std::string ours = mapsync::RootKey();
                if (map.empty() || ours.empty() || mapsync::SameMap(map, ours)) mapsync::OnPeerJoined(id);
                else LOG("presence: full map not sent to peer %u '%s': it has '%s' open, we have '%s'", id, name.c_str(), map.c_str(), ours.c_str());
                diag::OnPeerJoined(id);
                zonesync::OnPeerJoined(id);                          // a build of our map to fetch in the background
            }
            break;
        }
        case net::MSG_PEER_LEFT:
        {
            net::Reader r(m.payload);
            uint32_t id = r.u32();
            auto it = g_peers.find(id);
            if (it != g_peers.end()) { LOG("presence: peer %u '%s' left", id, it->second.name.c_str()); g_peers.erase(it); }
            break;
        }
        case net::MSG_CAMERA:
        {
            if (m.from == net::LocalPeerId() || m.from == 0) break;
            net::Reader r(m.payload);
            presence::Peer& p = PeerRef(m.from);
            uint64_t now = GetTickCount64();
            if (p.hasCamera) { memcpy(p.prevOrigin, p.origin, sizeof p.origin); memcpy(p.prevAngles, p.angles, sizeof p.angles); p.prevSampleTick = p.sampleTick; }
            for (int i = 0; i < 3; ++i) p.origin[i] = r.f32();
            for (int i = 0; i < 3; ++i) p.angles[i] = r.f32();
            if (!p.hasCamera) { memcpy(p.prevOrigin, p.origin, sizeof p.origin); memcpy(p.prevAngles, p.angles, sizeof p.angles); p.prevSampleTick = now; }
            p.sampleTick = now;
            p.hasCamera = r.ok; p.lastSeenTick = now;
            break;
        }
        case net::MSG_SELECTION:
        {
            if (m.from == net::LocalPeerId() || m.from == 0) break;
            net::Reader r(m.payload);
            presence::Peer& p = PeerRef(m.from);
            uint16_t n = r.u16(); p.selection.clear();
            for (uint16_t i = 0; i < n && r.ok; ++i)
            {
                presence::Peer::SelBox b;
                b.brushGuid = r.str(); b.entityGuid = r.str();
                for (int k = 0; k < 3; ++k) b.mins[k] = r.f32();
                for (int k = 0; k < 3; ++k) b.maxs[k] = r.f32();
                if (r.ok) p.selection.push_back(std::move(b));
            }
            for (size_t i = 0; i < p.selection.size() && r.ok && r.pos < r.buf.size(); ++i)
            {
                uint8_t kind = r.u8();
                if (kind != 1) continue;
                uint16_t faces = r.u16();
                std::vector<float> planes(size_t(faces) * 9);
                for (float& f : planes) f = r.f32();
                if (r.ok) p.selection[i].planes.swap(planes);
            }
            ++p.selectionSeq;
            p.lastSeenTick = GetTickCount64();
            break;
        }
        case net::MSG_CHAT:
        {
            if (m.from == net::LocalPeerId()) break;
            net::Reader r(m.payload);
            std::string text = r.str();
            if (!r.ok || text.empty()) break;
            auto it = g_peers.find(m.from);
            PushChat(m.from, it != g_peers.end() ? it->second.name : "peer" + std::to_string(m.from), text);
            break;
        }
        case net::MSG_SCOPE:
        {
            if (m.from == net::LocalPeerId() || m.from == 0) break;
            net::Reader r(m.payload);
            uint8_t depth = r.u8();
            std::vector<std::string> chain;
            for (uint8_t i = 0; i < depth && r.ok; ++i) chain.push_back(r.str());
            if (!r.ok) break;
            // trailer: u8 n, then per level origin[3] angles[3]
            std::vector<presence::PrefabXf> xf;
            if (r.pos < r.buf.size())
            {
                uint8_t n = r.u8();
                for (uint8_t i = 0; i < n && r.ok; ++i)
                {
                    presence::PrefabXf x;
                    for (int k = 0; k < 3; ++k) x.origin[k] = r.f32();
                    for (int k = 0; k < 3; ++k) x.angles[k] = r.f32();
                    if (r.ok) xf.push_back(x);
                }
            }
            presence::Peer& p = PeerRef(m.from);
            if (!p.scopeKnown || p.prefabChain != chain)
            {
                std::string place;
                for (size_t i = 0; i < xf.size(); ++i) { char b[128]; _snprintf_s(b, _TRUNCATE, "%s(%g %g %g / %g %g %g)", i ? " > " : " at ", xf[i].origin[0], xf[i].origin[1], xf[i].origin[2], xf[i].angles[0], xf[i].angles[1], xf[i].angles[2]); place += b; }
                LOG("presence: peer %u '%s' is %s%s", m.from, p.name.c_str(), chain.empty() ? "outside prefabs" : ("inside prefab " + presence::PrefabChainText(chain, false)).c_str(), place.c_str());
            }
            p.scopeKnown = true; p.prefabChain = chain; p.prefabXf = xf;
            break;
        }
        case net::MSG_PEER_COLOR:
        {
            if (m.from == net::LocalPeerId() || m.from == 0) break;
            net::Reader r(m.payload);
            uint32_t color = r.u32();
            if (!r.ok) break;
            presence::Peer& p = PeerRef(m.from);
            p.color = Resolve(m.from, color);
            LOG("presence: peer %u '%s' changed color to %s", m.from, p.name.c_str(), presence::ColorHex(p.color).c_str());
            break;
        }
        case net::MSG_BYE:
        {
            if (!net::IsHost()) g_endedByHost = true;      // handled on the main thread in Tick
            break;
        }
        case net::MSG_WELCOME:
        {
            net::Reader r(m.payload);
            r.u32(); std::string hostName = r.str(); std::string map = r.str();
            uint32_t color = r.u32(); if (!r.ok) color = 0;
            presence::Peer& p = PeerRef(net::kHostPeerId);
            p.name = hostName; p.lastSeenTick = GetTickCount64();
            p.color = Resolve(net::kHostPeerId, color);
            p.mapKey = map; g_hostKey = map;                   // compared with ours on the main thread (TickMapMatch)
            break;
        }
        case net::MSG_MAP_INFO:
        {
            if (m.from == net::LocalPeerId() || m.from == 0) break;
            net::Reader r(m.payload);
            std::string map = r.str();
            if (!r.ok) break;
            presence::Peer& p = PeerRef(m.from);
            std::string prev = p.mapKey;
            p.mapKey = map;
            LOG("presence: peer %u '%s' now has '%s' open (was '%s')", m.from, p.name.c_str(), map.c_str(), prev.c_str());
            if (m.from == net::kHostPeerId && !net::IsHost()) g_hostKey = map;
            if (net::IsHost())
            {
                // a client that was on another map (its join-time full map was dropped there, or never sent)
                // and is on ours now: send the full map again, it applies as a diff on the client
                std::string ours = mapsync::RootKey();
                if (!ours.empty() && mapsync::SameMap(map, ours) && !prev.empty() && !mapsync::SameMap(prev, ours))
                { LOG("presence: peer %u '%s' switched to our map; sending the full map", m.from, p.name.c_str()); mapsync::OnPeerJoined(m.from); }
            }
            break;
        }
        case net::MSG_MAP_REQUEST:
        {
            // a client without a copy of our map: send it the full map (it writes a temporary copy and opens it)
            if (!net::IsHost() || m.from == net::LocalPeerId()) break;
            net::Reader r(m.payload);
            std::string map = r.str();
            if (!r.ok) break;
            std::string ours = mapsync::RootKey();
            auto it = g_peers.find(m.from);
            const char* name = it != g_peers.end() ? it->second.name.c_str() : "?";
            if (!ours.empty() && mapsync::SameMap(map, ours)) { LOG("presence: peer %u '%s' asked for our map '%s'; sending the full map", m.from, name, ours.c_str()); mapsync::OnPeerJoined(m.from); }
            else LOG("presence: peer %u '%s' asked for '%s' but our map is '%s'; ignored", m.from, name, map.c_str(), ours.c_str());
            break;
        }
        case net::MSG_INVENTORY_REQUEST:
        {
            if (!net::IsHost() || m.from == net::LocalPeerId()) break;
            net::Reader r(m.payload); std::string map = r.str(); if (!r.ok) break;
            std::vector<prefabs::Item> items = mapsync::HostInventory();
            net::Writer w; w.str(mapsync::RootKey()); w.u16(uint16_t(items.size() > 65535 ? 65535 : items.size()));
            for (size_t i = 0; i < items.size() && i < 65535; ++i)
            {
                char hex[24]; _snprintf_s(hex, _TRUNCATE, "%016llx", (unsigned long long)items[i].hash);
                w.str(items[i].key); w.u32(items[i].size); w.str(hex); w.u8(items[i].missing ? 1 : 0);
            }
            net::SendTo(m.from, net::MSG_INVENTORY, w.buf);
            LOG("presence: peer %u asked for the prefab inventory of '%s': %zu prefab(s) sent", m.from, map.c_str(), items.size());
            break;
        }
        case net::MSG_INVENTORY:
        {
            if (m.from != net::kHostPeerId || net::IsHost()) break;
            net::Reader r(m.payload); std::string map = r.str(); uint16_t n = r.u16();
            std::vector<prefabs::Item> items;
            for (uint16_t i = 0; i < n && r.ok; ++i)
            {
                prefabs::Item it; it.key = r.str(); it.size = r.u32(); std::string hex = r.str(); it.missing = r.u8() != 0;
                it.hash = strtoull(hex.c_str(), nullptr, 16);
                if (r.ok) items.push_back(it);
            }
            LOG("presence: prefab inventory of '%s' from the host: %zu prefab(s)", map.c_str(), items.size());
            g_inventory.swap(items); g_inventoryArrived = true;      // OnMessage runs under g_mutex
            break;
        }
        case net::MSG_FILE_REQUEST:
        {
            // the host serves its clients, a client serves the host (a prefab it placed mid-session that the host lacks,
            // see mapsync's wanted files), requests relayed from other clients are ignored
            if (m.from == net::LocalPeerId()) break;
            if (!net::IsHost() && m.from != net::kHostPeerId) break;
            net::Reader r(m.payload); uint16_t n = r.u16();
            for (uint16_t i = 0; i < n && r.ok; ++i) { std::string key = r.str(); if (r.ok) mapsync::SendPrefabFile(m.from, key); }
            break;
        }
        case net::MSG_ASSET_MANIFEST:
        case net::MSG_ASSET_REQUEST:
        case net::MSG_ASSET_CHUNK:
        case net::MSG_ASSET_CHUNK_Z:
        case net::MSG_ASSET_REQUEST_Z:
        case net::MSG_ASSET_CAPS:
        case net::MSG_ASSET_CONVERTED: assets::OnMessage(m); break;
        case net::MSG_SAVE: savesync::OnMessage(m); break;
        case net::MSG_ZONE_REQUEST:
        case net::MSG_ZONE_MANIFEST:
        case net::MSG_ZONE_GET:
        case net::MSG_ZONE_CHUNK:
        case net::MSG_ZONE_REQUEST2:
        case net::MSG_ZONE_MANIFEST2:
        case net::MSG_ZONE_ANNOUNCE:
        case net::MSG_DIRECT_OFFER:
        case net::MSG_DIRECT_PUNCH:
        case net::MSG_PLAY_STATE: zonesync::OnMessage(m); break;
        case net::MSG_LIGHT_BUILD: lightsync::OnMessage(m); break;
        case net::MSG_LOG_LINES:
        case net::MSG_FILE_PART:
        case net::MSG_FILE:
        {
            auto it = g_peers.find(m.from);
            diag::OnMessage(m, it != g_peers.end() ? it->second.name : std::string());
            break;
        }
        default: mapsync::OnMessage(m); break;
        }
    }
}

namespace presence
{
    // active session parameters (what the reconnect uses, set by StartHost/StartClient, not by the ini)
    std::string g_sessHost, g_sessBind;
    uint16_t    g_sessPort = 0;

    void ResolveName(const std::string& name)
    {
        g_localName = name;
        if (g_localName.empty()) { wchar_t u[256] = {}; DWORD n = 256; GetUserNameW(u, &n); g_localName = Narrow(u); }
    }

    bool StartHost(uint16_t port, const std::string& name, const std::string& bindAddr)
    {
        Stop();
        assets::EndSession("a new session started");
        ResolveName(name);
        backup::NewSession();
        ResetMapMatch();
        g_sessPort = port; g_sessBind = bindAddr.empty() ? "0.0.0.0" : bindAddr; g_sessHost.clear();
        net::SetLocalColor(Cfg().cameraColor);
        net::SetHandler(OnMessage);
        bool ok = net::StartHost(port, g_localName, LocalMapKey(), g_sessBind);
        g_started = ok;
        if (ok) assets::Start(true);
        mapsync::OnSessionStarted(true);
        LOG("presence: hosting as '%s' on %s:%u -> %s", g_localName.c_str(), g_sessBind.c_str(), port, net::Status().c_str());
        return ok;
    }

    bool StartClient(const std::string& host, uint16_t port, const std::string& name)
    {
        Stop();
        assets::EndSession("a new session started");
        ResolveName(name);
        backup::NewSession();
        ResetMapMatch();
        g_sessHost = host; g_sessPort = port;
        g_endedByHost = false; g_endReason.clear(); g_wasConnected = false;
        net::SetLocalColor(Cfg().cameraColor);
        net::SetHandler(OnMessage);
        // the dial runs on a worker thread (a dead VPN address blocks connect() for ~20 s), Tick adopts
        // the link when it comes up and retries every 5 s until Stop or the host says goodbye
        net::ConnectAsync(host, port, g_localName, LocalMapKey());
        g_lastReconnectTick = g_dialSince = GetTickCount64();
        g_started = true;
        assets::Start(false);
        LOG("presence: connecting as '%s' to %s:%u", g_localName.c_str(), host.c_str(), port);
        return true;
    }

    void Start()
    {
        const Config& c = Cfg();
        if (!c.autostart) { LOG("presence: no session started with Radiant ([net] autostart=0); Ctrl+Shift+T opens the Team Create pane"); return; }
        if (c.role != L"host" && c.role != L"client") { LOG("presence: role=off, networking disabled (Ctrl+Shift+T in Radiant opens the session dialog)"); return; }
        if (c.role == L"host") StartHost(uint16_t(c.port), Narrow(c.userName), Narrow(c.bindAddr));
        else StartClient(Narrow(c.hostAddr), uint16_t(c.port), Narrow(c.userName));
    }

    void Stop()
    {
        if (!g_started && !net::IsRunning()) return;
        net::Stop();
        ResetMapMatch();
        std::lock_guard<std::mutex> lock(g_mutex);
        g_peers.clear();
        g_conflicts.clear();
        g_started = false;
        g_endedByHost = false;
        LOG("presence: session stopped");
    }

    bool IsActive() { return g_started; }
    std::string EndReason() { std::lock_guard<std::mutex> lock(g_mutex); return g_endReason; }

    std::string SessionText(bool hideAddress)
    {
        if (!g_started) { std::string r = EndReason(); return r.empty() ? "Not connected" : "Not connected. " + r; }
        if (net::IsHost()) return "Hosting on " + g_sessBind + ":" + std::to_string(g_sessPort) + " as " + g_localName;   // the bind address, never the public one
        std::string where = hideAddress ? "the host" : g_sessHost + ":" + std::to_string(g_sessPort);
        if (net::IsConnected()) return "Connected to " + where + " as " + g_localName;
        std::string st = net::Status();       // net's own lines carry the address too ("connect to 1.2.3.4:27960 failed ...")
        if (hideAddress && !g_sessHost.empty())
            for (size_t at = st.find(g_sessHost); at != std::string::npos; at = st.find(g_sessHost, at + 4)) st.replace(at, g_sessHost.size(), "host");
        return "Connecting to " + where + " ... (retries every 5 s; " + st + ")";
    }

    uint64_t    g_lastSelTick = 0;
    std::string g_lastSelKey;
    std::string g_lastSelSet;                       // the selected GUIDs alone (without bounds): a change means new objects may need announcing
    std::map<std::string, std::array<float, 3>> g_lastSentSize;   // per selected brush: the box size last sent, a change = shape edit, send the planes

    // Every 100 ms: walk Radiant's selected-brush list and broadcast when the GUIDs or bounds changed, so peers see a drag live
    // (change detected at 1/16 unit, finer than Radiant's quarter-unit nudge)
    void TickSelection(uint64_t now)
    {
        if (now - g_lastSelTick < 100) return;
        g_lastSelTick = now;
        std::vector<hooks::SelectedBrush> sel;
        if (!hooks::GetSelection(sel)) return;
        mapsync::NoteSelected(sel);
        {
            std::string setKey; for (const auto& s : sel) setKey += s.brushGuid;
            if (setKey != g_lastSelSet) { g_lastSelSet = setKey; if (!sel.empty()) mapsync::AnnounceSelectedNew(); }
        }
        // soft locks: anything in our selection that a peer also holds
        {
            std::vector<presence::Conflict> conflicts;
            std::lock_guard<std::mutex> lock(g_mutex);
            for (const auto& kv : g_peers)
                for (const auto& box : kv.second.selection)
                    for (const auto& s : sel)
                        if (s.brushGuid == box.brushGuid || (!s.entityGuid.empty() && s.entityGuid == box.entityGuid))
                        {
                            presence::Conflict c; c.brushGuid = s.brushGuid; memcpy(c.mins, s.mins, sizeof c.mins); memcpy(c.maxs, s.maxs, sizeof c.maxs);
                            c.peerId = kv.first; c.peerName = kv.second.name; c.color = kv.second.color;
                            conflicts.push_back(c);
                            break;
                        }
            if (conflicts.size() != g_conflicts.size())
            {
                if (!conflicts.empty()) LOG("presence: %zu selected object(s) are also selected by %s", conflicts.size(), conflicts[0].peerName.c_str());
                else LOG("presence: selection conflict cleared");
            }
            g_conflicts.swap(conflicts);
        }
        std::string key;
        char num[64];
        for (const auto& s : sel)
        {
            key += s.brushGuid;
            _snprintf_s(num, _TRUNCATE, "|%d %d %d %d %d %d|", int(s.mins[0] * 16), int(s.mins[1] * 16), int(s.mins[2] * 16), int(s.maxs[0] * 16), int(s.maxs[1] * 16), int(s.maxs[2] * 16));
            key += num;
        }
        if (key == g_lastSelKey) return;
        g_lastSelKey = key;
        net::Writer w;
        size_t count = sel.size() > 65535 ? 65535 : sel.size();
        w.u16(uint16_t(count));
        for (size_t i = 0; i < count; ++i)
        {
            const auto& s = sel[i];
            w.str(s.brushGuid); w.str(s.entityGuid);
            for (int k = 0; k < 3; ++k) w.f32(s.mins[k]);
            for (int k = 0; k < 3; ++k) w.f32(s.maxs[k]);
        }
        // Trailer: per brush a kind byte, 1 = the face plane points follow (u16 faces, 9 floats each), sent when the box size differs
        // from the last message for that brush (resize, rotate, vertex edit), a plain move keeps the size and peers derive it from the bounds.
        // Brush pointers come from the selection instances, walked in the same order as GetSelection.
        {
            std::vector<hooks::RadPtr> insts = hooks::SelectedBrushPtrs();
            std::map<std::string, std::array<float, 3>> sizes;
            for (size_t i = 0; i < count; ++i)
            {
                const auto& s = sel[i];
                std::array<float, 3> size = { s.maxs[0] - s.mins[0], s.maxs[1] - s.mins[1], s.maxs[2] - s.mins[2] };
                sizes[s.brushGuid] = size;
                bool changed = false;
                auto it = g_lastSentSize.find(s.brushGuid);
                if (it == g_lastSentSize.end()) changed = false;   // first message for this brush: a plain selection, no shape edit yet
                else for (int k = 0; k < 3; ++k) if (std::fabs(it->second[k] - size[k]) > 0.01f) changed = true;
                std::vector<float> planes;
                if (changed && insts.size() == sel.size()) planes = hooks::BrushPlanes(hooks::BrushOfInst(insts[i]));
                if (planes.empty()) { w.u8(0); continue; }
                w.u8(1); w.u16(uint16_t(planes.size() / 9));
                for (float f : planes) w.f32(f);
            }
            g_lastSentSize.swap(sizes);
        }
        net::Send(net::MSG_SELECTION, w.buf);
        static int logged = 0; if (logged++ < 20) LOG("presence: selection changed, %zu brush(es) sent", sel.size());
    }

    // Live peer drags: a peer's selection message carries the bounds of every brush they hold (10 Hz), so our copy of each brush is moved
    // by the same offset through Brush_Move and the commit upsert after their deselect finds the object already there. Translation only:
    // a size change waits for the commit. Brushes we hold ourselves, and brushes we do not have yet (their paste), are skipped.
    unsigned g_liveMoved = 0, g_liveSkippedSize = 0, g_liveMissing = 0, g_liveReshaped = 0;
    uint64_t g_liveLastTick = 0;
    void TickLiveMove()
    {
        if (!Cfg().syncEnabled || !Cfg().syncLiveMove || mapsync::Blocked()) return;
        if (!rp::Has(&rp::host->MoveBrush) || !rp::host->MoveBrush) return;
        struct Job { uint32_t peer; std::string name; std::vector<presence::Peer::SelBox> boxes; };
        std::vector<Job> jobs;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (auto& kv : g_peers)
            {
                presence::Peer& p = kv.second;
                if (p.selectionSeq == p.liveMoveSeq && p.liveMoveRetry == 0) continue;
                if (p.selectionSeq != p.liveMoveSeq) p.liveMoveRetry = 0;
                p.liveMoveSeq = p.selectionSeq;
                if (p.liveMoveRetry) --p.liveMoveRetry;
                if (!p.selection.empty()) jobs.push_back({ kv.first, p.name, p.selection });
            }
        }
        for (const Job& job : jobs)
        {
            std::vector<std::string> guids; guids.reserve(job.boxes.size());
            for (const auto& b : job.boxes) guids.push_back(b.brushGuid);
            std::vector<ModToolsBrush> brushes = rp::FindBrushesByGuid(guids);
            // A point entity's invisible brush may carry another GUID on our side (never written to the file),
            // so a box whose brush is unknown is resolved through its entity when that entity has exactly one linked brush.
            {
                std::vector<size_t> pending; std::vector<std::string> entityGuids;
                for (size_t i = 0; i < brushes.size(); ++i) if (!brushes[i] && !job.boxes[i].entityGuid.empty()) { pending.push_back(i); entityGuids.push_back(job.boxes[i].entityGuid); }
                if (!pending.empty())
                {
                    std::vector<ModToolsEntity> ents = rp::FindEntitiesByGuid(entityGuids);
                    for (size_t j = 0; j < pending.size(); ++j)
                    {
                        if (!ents[j]) continue;
                        ModToolsBrush only = nullptr; int count = 0;
                        rp::ForEachEntityBrush(ents[j], [&](ModToolsBrush b) { only = b; ++count; });
                        if (count == 1) brushes[pending[j]] = only;
                    }
                }
            }
            unsigned moved = 0; bool missing = false;
            for (size_t i = 0; i < brushes.size(); ++i)
            {
                const presence::Peer::SelBox& b = job.boxes[i];
                if (!brushes[i]) { ++g_liveMissing; missing = true; continue; }
                // the sender changed the brush's shape: take its face planes as they are (same face order, both copies
                // come from the same text), a mismatch in face count leaves it to the commit
                if (!b.planes.empty())
                {
                    if (rp::SetBrushPlanes(brushes[i], b.planes)) { ++moved; ++g_liveReshaped; }
                    else ++g_liveSkippedSize;
                    continue;
                }
                float mn[3], mx[3];
                if (!rp::BrushBounds(brushes[i], mn, mx)) continue;
                bool sameSize = true, differs = false;
                float delta[3];
                for (int k = 0; k < 3; ++k)
                {
                    if (std::fabs((b.maxs[k] - b.mins[k]) - (mx[k] - mn[k])) > 0.05f) sameSize = false;
                    delta[k] = b.mins[k] - mn[k];
                    if (std::fabs(delta[k]) > 0.001f) differs = true; else delta[k] = 0.0f;
                }
                if (!sameSize) { ++g_liveSkippedSize; continue; }
                if (!differs) continue;
                if (rp::MoveBrush(brushes[i], delta)) ++moved;
            }
            // a brush we do not have yet is probably the peer's new object whose upsert is still in the sync queue
            // (applied within 500 ms): keep re-applying this selection for a while so it lands once the brush exists
            if (missing)
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                auto it = g_peers.find(job.peer);
                if (it != g_peers.end() && it->second.liveMoveRetry == 0 && it->second.selectionSeq == it->second.liveMoveSeq) it->second.liveMoveRetry = 60;
            }
            if (moved)
            {
                g_liveMoved += moved; g_liveLastTick = GetTickCount64();
                static int logged = 0;
                if (logged++ < 20 || Cfg().verbose) LOG("presence: live move: %u brush(es) followed %s's drag", moved, job.name.c_str());
            }
        }
    }

    void Tick()
    {
        if (!g_started) return;
        uint64_t now = GetTickCount64();
        if (g_endedByHost)
        {
            // the host closed the session on purpose: stop cleanly instead of reconnecting forever
            Stop();
            { std::lock_guard<std::mutex> lock(g_mutex); g_endReason = "The host ended the session."; }
            LOG("presence: the host ended the session; stopped");
            presence::LeaveTempMap("the host ended the session", false);
            return;
        }
        if (!net::IsConnected())
        {
            if (!net::IsHost() && g_wasConnected)
            {
                // client: the host link dropped (host crashed, network gone): stop like a BYE instead of reconnecting forever
                Stop();
                { std::lock_guard<std::mutex> lock(g_mutex); g_endReason = "Lost the connection to the host."; }
                LOG("presence: lost the connection to the host; stopped");
                presence::LeaveTempMap("the connection to the host was lost", false);
                return;
            }
            g_wasConnected = false;
            if (!net::IsHost() && now - g_dialSince > 60000)
            {
                // a host that never answered within a minute (wrong address, host not started, firewall): stop instead of dialing forever
                std::string where = g_sessHost + ":" + std::to_string(g_sessPort);
                Stop();
                { std::lock_guard<std::mutex> lock(g_mutex); g_endReason = "Could not reach the host within a minute."; }   // no address: the camera view hides it under hide_ip
                LOG("presence: no answer from %s within a minute; stopped", where.c_str());
                return;
            }
            // client: the first connect has not come up yet, dial again every 5 s on a worker thread
            if (!net::IsHost() && !net::IsConnecting() && now - g_lastReconnectTick > 5000)
            {
                g_lastReconnectTick = now;
                net::Stop();
                { std::lock_guard<std::mutex> lock(g_mutex); g_peers.clear(); }
                LOG("presence: reconnecting to %s:%u", g_sessHost.c_str(), g_sessPort);
                net::ConnectAsync(g_sessHost, g_sessPort, g_localName, LocalMapKey());
            }
            return;
        }
        if (!g_wasConnected)
        {
            // a client link just came up (first connect or a reconnect): hold local capture until the host's full sync
            g_wasConnected = true;
            if (!net::IsHost()) { mapsync::OnSessionStarted(false); diag::OnConnected(); }
            std::lock_guard<std::mutex> lock(g_mutex); g_scopeAnnounce = true;
        }
        {
            bool announce;
            { std::lock_guard<std::mutex> lock(g_mutex); announce = g_scopeAnnounce; g_scopeAnnounce = false; }
            if (announce || now - g_lastScopeSend > 15000)
            {
                g_lastScopeSend = now;
                net::Writer w; w.u8(uint8_t(g_prefabChain.size() > 255 ? 255 : g_prefabChain.size()));
                for (size_t i = 0; i < g_prefabChain.size() && i < 255; ++i) w.str(g_prefabChain[i]);
                size_t nx = g_prefabXf.size() < g_prefabChain.size() ? g_prefabXf.size() : g_prefabChain.size();
                w.u8(uint8_t(nx > 255 ? 255 : nx));
                for (size_t i = 0; i < nx && i < 255; ++i) { for (int k = 0; k < 3; ++k) w.f32(g_prefabXf[i].origin[k]); for (int k = 0; k < 3; ++k) w.f32(g_prefabXf[i].angles[k]); }
                net::Send(net::MSG_SCOPE, w.buf);
            }
        }
        TickSelection(now);
        TickLiveMove();
        TickMapMatch(now);
        mapsync::Tick();
        if (now - g_lastSendTick < kSendIntervalMs) return;
        float o[3], a[3];
        if (!hooks::GetCamera(o, a)) return;
        bool moved = false;
        for (int i = 0; i < 3; ++i) moved |= std::fabs(o[i] - g_lastOrigin[i]) > 0.01f || std::fabs(a[i] - g_lastAngles[i]) > 0.01f;
        if (!moved && now - g_lastSendTick < 2000) return;   // keep-alive every 2 s even when still
        g_lastSendTick = now;
        memcpy(g_lastOrigin, o, sizeof o); memcpy(g_lastAngles, a, sizeof a);
        net::Writer w;
        for (int i = 0; i < 3; ++i) w.f32(o[i]);
        for (int i = 0; i < 3; ++i) w.f32(a[i]);
        net::Send(net::MSG_CAMERA, w.buf);
    }

    std::vector<Peer> Snapshot()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::vector<Peer> v;
        for (auto& kv : g_peers) v.push_back(kv.second);
        return v;
    }

    std::string LocalName() { return g_localName; }

    void OnScopeChanged(const std::wstring& fullPath, int depth)
    {
        std::vector<std::string> chain = g_prefabChain;
        if (depth <= 0) chain.clear();
        else
        {
            chain.resize(size_t(depth));                          // entering adds a level, leaving drops the inner ones
            chain[size_t(depth) - 1] = mapsync::KeyOfPath(fullPath);
        }
        if (chain == g_prefabChain) return;
        g_prefabChain = chain;
        // the placement of every entered instance, from the misc_prefab entities on Radiant's prefab stack
        g_prefabXf.clear();
        std::string place;
        for (int level = 1; level <= depth; ++level)
        {
            PrefabXf x;
            if (!hooks::PrefabStackPlacement(level, x.origin, x.angles) && !chain.empty())
                LOG("presence: no placement for prefab level %d (older host?); the peers will place you at the prefab's own origin", level);
            g_prefabXf.push_back(x);
            char b[128]; _snprintf_s(b, _TRUNCATE, "%s(%g %g %g / %g %g %g)", level > 1 ? " > " : " at ", x.origin[0], x.origin[1], x.origin[2], x.angles[0], x.angles[1], x.angles[2]); place += b;
        }
        LOG("presence: you are %s%s", chain.empty() ? "outside prefabs" : ("inside prefab " + PrefabChainText(chain, false)).c_str(), place.c_str());
        std::lock_guard<std::mutex> lock(g_mutex); g_scopeAnnounce = true;
    }

    std::vector<std::string> LocalPrefabChain() { return g_prefabChain; }
    std::vector<PrefabXf>    LocalPrefabXf() { return g_prefabXf; }

    // ---- prefab space transforms ----------------------------------------------------------------
    namespace
    {
        constexpr float kDegXf = 3.14159265f / 180.0f;
        // A misc_prefab's "angles" (pitch yaw roll) as the rotation that takes a point of the prefab file into the
        // outer scope: id's AnglesToAxis (yaw about z, then pitch about y, then roll about x, positive pitch tips
        // the forward axis down), columns = forward, left, up.
        void PrefabMatrix(const float angles[3], float m[3][3])
        {
            float p = angles[0] * kDegXf, y = angles[1] * kDegXf, r = angles[2] * kDegXf;
            float sp = sinf(p), cp = cosf(p), sy = sinf(y), cy = cosf(y), sr = sinf(r), cr = cosf(r);
            float fwd[3]   = { cp * cy, cp * sy, -sp };
            float right[3] = { -sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp };
            float up[3]    = { cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp };
            for (int i = 0; i < 3; ++i) { m[i][0] = fwd[i]; m[i][1] = -right[i]; m[i][2] = up[i]; }
        }
        Xform Forward(const PrefabXf& x) { Xform f; PrefabMatrix(x.angles, f.m); for (int k = 0; k < 3; ++k) f.t[k] = x.origin[k]; return f; }
        Xform Inverse(const Xform& a)
        {
            Xform r;
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[j][i];
            for (int i = 0; i < 3; ++i) r.t[i] = -(r.m[i][0] * a.t[0] + r.m[i][1] * a.t[1] + r.m[i][2] * a.t[2]);
            return r;
        }
        Xform Compose(const Xform& a, const Xform& b)   // b first, then a
        {
            Xform r;
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
            for (int i = 0; i < 3; ++i) r.t[i] = a.m[i][0] * b.t[0] + a.m[i][1] * b.t[1] + a.m[i][2] * b.t[2] + a.t[i];
            return r;
        }
        std::string LowerKey(std::string s) { for (auto& c : s) c = char(tolower(uint8_t(c))); return s; }
    }

    bool PeerToLocal(const Peer& p, Xform& out)
    {
        const std::vector<std::string>& theirs = p.prefabChain;
        const std::vector<std::string>& ours = g_prefabChain;
        size_t common = 0;
        while (common < theirs.size() && common < ours.size() && LowerKey(theirs[common]) == LowerKey(ours[common])) ++common;
        if (common == theirs.size() && common == ours.size()) return false;
        Xform x;
        // their remaining levels: innermost first up to the common scope
        for (size_t i = common; i < theirs.size(); ++i) x = Compose(x, Forward(i < p.prefabXf.size() ? p.prefabXf[i] : PrefabXf()));
        // then down into our remaining levels
        for (size_t i = common; i < ours.size(); ++i) x = Compose(Inverse(Forward(i < g_prefabXf.size() ? g_prefabXf[i] : PrefabXf())), x);
        out = x;
        return true;
    }
    void XfPoint(const Xform& x, const float in[3], float out[3])
    {
        float v[3] = { in[0], in[1], in[2] };
        for (int i = 0; i < 3; ++i) out[i] = x.m[i][0] * v[0] + x.m[i][1] * v[1] + x.m[i][2] * v[2] + x.t[i];
    }
    void XfCameraAngles(const Xform& x, const float in[3], float out[3])
    {
        float p = in[0] * kDegXf, y = in[1] * kDegXf;
        float f[3] = { cosf(p) * cosf(y), cosf(p) * sinf(y), sinf(p) };      // Radiant's camera: positive pitch looks up
        float g[3];
        for (int i = 0; i < 3; ++i) g[i] = x.m[i][0] * f[0] + x.m[i][1] * f[1] + x.m[i][2] * f[2];
        float z = g[2] > 1 ? 1.0f : g[2] < -1 ? -1.0f : g[2];
        out[0] = asinf(z) / kDegXf; out[1] = atan2f(g[1], g[0]) / kDegXf; out[2] = 0;
    }
    void XfBox(const Xform& x, const float mins[3], const float maxs[3], float outMins[3], float outMaxs[3])
    {
        for (int k = 0; k < 3; ++k) { outMins[k] = 1e30f; outMaxs[k] = -1e30f; }
        for (int c = 0; c < 8; ++c)
        {
            float p[3] = { (c & 1) ? maxs[0] : mins[0], (c & 2) ? maxs[1] : mins[1], (c & 4) ? maxs[2] : mins[2] }, q[3];
            XfPoint(x, p, q);
            for (int k = 0; k < 3; ++k) { if (q[k] < outMins[k]) outMins[k] = q[k]; if (q[k] > outMaxs[k]) outMaxs[k] = q[k]; }
        }
    }

    std::string PrefabChainText(const std::vector<std::string>& chain, bool fileNamesOnly)
    {
        std::string out;
        for (const auto& k : chain)
        {
            size_t slash = k.find_last_of('/');
            out += (out.empty() ? "" : " > ") + (fileNamesOnly && slash != std::string::npos ? k.substr(slash + 1) : k);
        }
        return out;
    }

    void OnMapLoaded()
    {
        // re-check before announcing, so a client that just switched to the host's map is unblocked
        // before the host's answer (the full map) can arrive
        TickMapMatch(GetTickCount64());
        net::SetLocalMap(LocalMapKey());
    }

    void OnMapSaved(const wchar_t* path)
    {
        g_lastSavedPath = path ? path : L"";
        if (g_askAfterSave) { g_askAfterSave = false; LOG("presence: map saved; asking about the host's map again"); }
    }

    bool OnTempMap() { return OnTempMapNow(); }

    bool LeaveTempMap(const char* reason, bool closing)
    {
        if (g_tempPath.empty()) return true;
        if (g_leaving) return true;                           // the box is already up (a second close click)
        std::wstring temp = g_tempPath; std::string key = g_tempKey;
        if (!OnTempMapNow()) { DropTempFile("not open any more", true); return true; }
        g_leaving = true;
        std::wstring text = L"The Team Create session is over (" + ToWide(reason) + L").\n\nThe map you were editing is a temporary copy of the host's '" + ToWide(key) + L"':\n" + temp +
            L"\n\nSave it on this PC?\n\nYes: choose where under map_source (Save Map As).\nNo: discard it" + (closing ? L" and close Radiant." : L" and start an empty map.");
        int r = MessageBoxW(mt::MainWindow(), text.c_str(), L"Team Create", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
        bool done;
        if (r == IDYES)
        {
            g_lastSavedPath.clear();
            rp::host->TriggerMenuAction(rp::self, "File/Save As");
            if (!g_lastSavedPath.empty())
            {
                if (SameFile(g_lastSavedPath, temp)) { LOG("presence: temporary map saved in place (%S); kept", temp.c_str()); ForgetTemp(); }
                else
                {
                    std::wstring saved = g_lastSavedPath;
                    LOG("presence: temporary map saved as %S", saved.c_str());
                    // mirrored prefabs move to _prefabs (a local file they replace is backed up first), the saved
                    // map gets its references pointed back at _prefabs, and Radiant re-reads it
                    prefabs::MoveResult mr = prefabs::MoveMirrorToReal();
                    if (mr.moved)
                    {
                        std::string text;
                        if (prefabs::ReadBytes(saved, text)) { fw::NoteSelfWriteBegin(); std::wstring tmp = saved + L".rtc_tmp"; { std::ofstream f(tmp, std::ios::binary); std::string d = prefabs::Delocalize(text); f.write(d.data(), std::streamsize(d.size())); } MoveFileExW(tmp.c_str(), saved.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH); fw::NoteSelfWriteEnd(); }
                        LOG("presence: %zu prefab(s) moved into _prefabs (%zu replaced, %zu failed); the saved map now refers to them there", mr.moved, mr.overwritten, mr.failed);
                    }
                    DropTempFile("saved elsewhere", true);
                    if (mr.moved && !closing) rp::host->ReloadMap(rp::self, saved.c_str(), 1);
                }
                done = true;
            }
            else if (closing)
            {
                // the close goes on regardless: a swallowed QCloseEvent leaves Qt's is_closing flag set and Radiant asserts at exit on the next attempt
                LOG("presence: Save As cancelled while closing; the temporary map file stays at %S", temp.c_str());
                MessageBoxW(mt::MainWindow(), (L"The temporary map stays on disk at:\n" + temp + L"\n\nOpen it from there next time, or delete it.").c_str(), L"Team Create", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
                ForgetTemp();
                done = true;
            }
            else
            {
                LOG("presence: Save As cancelled; the temporary map stays open");
                mapsync::Warn("The temporary map is still open: File > Save As to keep it, or Stop / close Radiant and answer No to discard it.");
                done = false;
            }
        }
        else
        {
            ClearModifiedFlag();                              // Radiant must not ask about these changes again
            LOG("presence: temporary map discarded%s", closing ? "" : "; starting an empty map");
            DropTempFile("discarded", true);
            if (!closing) { rp::host->TriggerMenuAction(rp::self, "File/New Map"); mapsync::OnMapClosed(); }
            done = true;
        }
        g_leaving = false;
        return done;
    }

    int CloseFilter(void*, ModToolsWidget, int type)
    {
        if (type != 19 /* QEvent::Close */ || (g_tempPath.empty() && !g_started)) return 0;
        if (!g_tempPath.empty()) { LOG("presence: Radiant is closing with a temporary map open"); LeaveTempMap("Radiant is closing", true); }
        else if (assets::HasStreamed() && rp::host->IsMapModified(rp::self))
        {
            // Radiant's own "save changes?" comes after this filter, too late for the streamed assets, so the save is offered here
            int r = MessageBoxW(mt::MainWindow(), L"Radiant is closing and your map has unsaved changes.\n\nSave it before the Team Create session ends? "
                L"The assets streamed from the other mappers that it uses are offered next.\n\nYes: save the map.\nNo: close without saving.",
                L"Team Create", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
            if (r == IDYES)
            {
                g_lastSavedPath.clear();
                rp::host->TriggerMenuAction(rp::self, "File/Save");   // modal Save As for an unnamed map
                LOG("presence: Radiant closing: %s", g_lastSavedPath.empty() ? "save cancelled, Radiant asks again" : "map saved first");
            }
            else { ClearModifiedFlag(); LOG("presence: Radiant closing: changes discarded"); }
        }
        // streamed assets the saved map uses: asked now, the process may not get another tick
        assets::EndSession("Radiant is closing");
        // a close cancelled later (Radiant's own unsaved-changes box) keeps Radiant and the session: streaming resumes
        if (g_started) mt::Post([] { if (g_started) { LOG("presence: Radiant stayed open; asset streaming resumes"); assets::Start(net::IsHost()); } });
        return 0;                                             // never swallow the close (see LeaveTempMap), Radiant's own close handling follows
    }

    void InstallCloseWatch()
    {
        if (!rp::host->RadiantAddress || !rp::host->MakeEventFilter || !rp::host->ui || !rp::host->ui->InstallFilter) return;
        void** pp = static_cast<void**>(rp::host->RadiantAddress(rp::self, kAddr_g_pMainWin));
        if (!pp || !*pp) { LOG("presence: main window object not available; no close watch for temporary maps"); return; }
        g_closeFilter = rp::host->MakeEventFilter(rp::self, CloseFilter, nullptr);
        if (!g_closeFilter) return;
        rp::host->ui->InstallFilter(*pp, g_closeFilter);
        LOG("presence: close watch installed on the main window");
    }

    std::string MapStatus()
    {
        std::string hostKey; { std::lock_guard<std::mutex> lock(g_mutex); hostKey = g_hostKey; }
        std::string s = "local='" + LocalMapKey() + "' host='" + hostKey + "' blocked=" + (g_blocked ? "1" : "0") +
            " switching='" + g_switchTarget + "' declined='" + g_declinedKey + "' ask_after_save=" + (g_askAfterSave ? "1" : "0") + " prompting=" + (g_prompting ? "1" : "0") +
            " temp='" + Narrow(g_tempPath) + "' temp_requested=" + (g_tempRequested ? "1" : "0") + " on_temp=" + (OnTempMapNow() ? "1" : "0");
        s += " fetch=" + std::to_string(int(g_fetch)) + "\n" + prefabs::Status();
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& kv : g_peers) s += "\npeer " + std::to_string(kv.first) + " '" + kv.second.name + "': map '" + kv.second.mapKey + "'";
        return s;
    }

    uint32_t AutoColor(uint32_t peerId) { return kPalette[peerId % (sizeof kPalette / sizeof kPalette[0])]; }
    uint32_t ChosenColor() { return Cfg().cameraColor; }
    // before a session there is no peer id yet, the host is always 1, so show that as the automatic colour
    uint32_t LocalColor() { return Resolve(net::LocalPeerId() ? net::LocalPeerId() : net::kHostPeerId, Cfg().cameraColor); }

    void SetLocalColor(uint32_t argb)
    {
        if (argb) argb |= 0xFF000000;
        Cfg().cameraColor = argb;
        rp::ConfigSet("net", "color", argb ? ColorHex(argb) : std::string());
        net::SetLocalColor(argb);
        LOG("presence: camera color set to %s", argb ? ColorHex(argb).c_str() : "automatic");
    }

    uint32_t ParseColor(const std::string& in)
    {
        size_t a = 0, b = in.size();
        while (a < b && isspace(static_cast<unsigned char>(in[a]))) ++a;
        while (b > a && isspace(static_cast<unsigned char>(in[b - 1]))) --b;
        if (a < b && in[a] == '#') ++a;
        if (b - a != 6) return 0;
        uint32_t v = 0;
        for (size_t i = a; i < b; ++i)
        {
            char c = in[i]; int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return 0;
            v = v << 4 | uint32_t(d);
        }
        return v | 0xFF000000;
    }

    std::string ColorHex(uint32_t argb)
    {
        char buf[16]; _snprintf_s(buf, _TRUNCATE, "#%06X", argb & 0xFFFFFF);
        return buf;
    }

    std::set<std::string> HeldByPeers()
    {
        std::set<std::string> out;
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& kv : g_peers)
            for (const auto& b : kv.second.selection) { if (!b.brushGuid.empty()) out.insert(b.brushGuid); if (!b.entityGuid.empty()) out.insert(b.entityGuid); }
        return out;
    }

    std::string LiveMoveStatus()
    {
        char buf[256];
        _snprintf_s(buf, _TRUNCATE, "live_move=%d moved=%u reshaped=%u shape_skipped=%u missing=%u last_ms_ago=%llu",
            Cfg().syncEnabled && Cfg().syncLiveMove ? 1 : 0, g_liveMoved, g_liveReshaped, g_liveSkippedSize, g_liveMissing,
            g_liveLastTick ? (unsigned long long)(GetTickCount64() - g_liveLastTick) : 0ULL);
        return buf;
    }

    std::vector<Conflict> Conflicts() { std::lock_guard<std::mutex> lock(g_mutex); return g_conflicts; }

    void SendChat(const std::string& text)
    {
        if (!g_started || text.empty()) return;
        net::Writer w; w.str(text.substr(0, 400));
        net::Send(net::MSG_CHAT, w.buf);
        std::lock_guard<std::mutex> lock(g_mutex);
        PushChat(net::LocalPeerId(), g_localName, text.substr(0, 400));
    }

    void Notice(const std::string& text)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        PushChat(0, "Team Create", text);
    }

    std::vector<ChatLine> ChatHistory(size_t maxLines)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::vector<ChatLine> v;
        size_t start = g_chat.size() > maxLines ? g_chat.size() - maxLines : 0;
        for (size_t i = start; i < g_chat.size(); ++i) v.push_back(g_chat[i]);
        return v;
    }

    void InterpolatedPose(const Peer& p, float origin[3], float angles[3])
    {
        uint64_t now = GetTickCount64();
        uint64_t interval = p.sampleTick > p.prevSampleTick ? p.sampleTick - p.prevSampleTick : kSendIntervalMs;
        if (interval > 500) interval = 500;                      // after a pause, snap rather than crawl
        float t = interval ? float(now - p.sampleTick) / float(interval) : 1.0f;
        if (t < 0) t = 0; if (t > 1) t = 1;
        for (int i = 0; i < 3; ++i)
        {
            origin[i] = p.prevOrigin[i] + (p.origin[i] - p.prevOrigin[i]) * t;
            float d = p.angles[i] - p.prevAngles[i];
            while (d > 180) d -= 360; while (d < -180) d += 360;    // shortest way round for yaw wrap
            angles[i] = p.prevAngles[i] + d * t;
        }
    }
}
