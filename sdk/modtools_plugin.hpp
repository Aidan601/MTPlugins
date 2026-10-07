// modtools_plugin.hpp: optional header-only C++ helpers over modtools_plugin.h.
// Usage: define MODTOOLS_PLUGIN_IMPL in exactly one .cpp (storage for rp::self / rp::host), call rp::Bind(self, host) in ModToolsPluginEntry.
#pragma once
#include "modtools_plugin.h"
#include <string>
#include <vector>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>

namespace rp
{
    extern ModToolsPlugin*       self;
    extern const ModToolsHostApi* host;

    inline void Bind(ModToolsPlugin* s, const ModToolsHostApi* h) { self = s; host = h; }
    inline bool Bound() { return self && host; }

    // Later host functions must be tested before calling.
    inline bool Has(const void* fnField) { return host && fnField && reinterpret_cast<const char*>(fnField) < reinterpret_cast<const char*>(host) + host->size; }
    inline bool HasUi(const void* fnField) { return host && host->ui && fnField && reinterpret_cast<const char*>(fnField) < reinterpret_cast<const char*>(host->ui) + host->ui->size; }

    inline void Log(const char* fmt, ...)
    {
        if (!host) return;
        char buf[2048]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
        host->Log(self, "%s", buf);
    }
    inline void LogW(const wchar_t* fmt, ...)
    {
        if (!host) return;
        wchar_t buf[2048]; va_list ap; va_start(ap, fmt); _vsnwprintf(buf, sizeof buf / sizeof buf[0], fmt, ap); va_end(ap);
        buf[sizeof buf / sizeof buf[0] - 1] = 0;
        host->LogW(self, L"%s", buf);
    }

    inline std::string ConfigGet(const char* section, const char* key, const char* def = "")
    {
        char buf[1024];
        size_t n = host->ConfigGet(self, section, key, def, buf, sizeof buf);
        if (n < sizeof buf) return std::string(buf, n);
        std::string s(n, '\0'); host->ConfigGet(self, section, key, def, &s[0], n + 1); return s;
    }
    inline int  ConfigGetInt(const char* section, const char* key, int def) { return host->ConfigGetInt(self, section, key, def); }
    inline bool ConfigGetBool(const char* section, const char* key, bool def) { return host->ConfigGetInt(self, section, key, def ? 1 : 0) != 0; }
    inline void ConfigSet(const char* section, const char* key, const std::string& value) { host->ConfigSet(self, section, key, value.c_str()); }

    inline std::wstring PluginDir() { return host->PluginDir(self); }
    // Which editor the plugin is running in (MODTOOLS_APP_RADIANT when the host predates APE support).
    inline int  App() { return Has(&host->App) && host->App ? host->App(self) : MODTOOLS_APP_RADIANT; }
    inline bool InRadiant() { return App() == MODTOOLS_APP_RADIANT; }
    inline bool InApe() { return App() == MODTOOLS_APP_APE; }
    inline bool InLauncher() { return App() == MODTOOLS_APP_LAUNCHER; }
    inline const ModToolsApeApi* Ape() { return Has(&host->ape) ? host->ape : nullptr; }
    inline std::wstring CurrentMapPath()
    {
        wchar_t buf[1024]; size_t n = host->CurrentMapPath(self, buf, 1024);
        if (n < 1024) return std::wstring(buf, n);
        std::wstring s(n, L'\0'); host->CurrentMapPath(self, &s[0], n + 1); return s;
    }
    inline std::string EditText(ModToolsWidget edit)
    {
        char buf[1024]; size_t n = host->ui->EditText(edit, buf, sizeof buf);
        if (n < sizeof buf) return std::string(buf, n);
        std::string s(n, '\0'); host->ui->EditText(edit, &s[0], n + 1); return s;
    }
    inline std::string EntityGuid(ModToolsEntity e) { char g[40] = {}; return host->EntityGuid(self, e, g) ? std::string(g) : std::string(); }
    inline std::string BrushGuid(ModToolsBrush b)   { char g[40] = {}; return host->BrushGuid(self, b, g) ? std::string(g) : std::string(); }
    inline bool WriteEntityText(ModToolsEntity e, std::string& out)
    {
        char* s = host->WriteEntityText(self, e);
        if (!s) return false;
        out = s; host->FreeString(self, s); return true;
    }
    inline std::vector<ModToolsSelectedBrush> GetSelection()
    {
        std::vector<ModToolsSelectedBrush> v(256);
        size_t n = host->GetSelection(self, v.data(), v.size());
        if (n > v.size()) { v.resize(n); n = host->GetSelection(self, v.data(), v.size()); }
        v.resize(n < v.size() ? n : v.size());
        return v;
    }
    inline std::vector<ModToolsBrushInst> SelectedBrushInsts()
    {
        std::vector<ModToolsBrushInst> v(256);
        size_t n = host->SelectedBrushInsts(self, v.data(), v.size());
        if (n > v.size()) { v.resize(n); n = host->SelectedBrushInsts(self, v.data(), v.size()); }
        v.resize(n < v.size() ? n : v.size());
        return v;
    }
    // Brush GUID texts -> handles (NULL where absent, all NULL on a host without FindBrushesByGuid).
    inline std::vector<ModToolsBrush> FindBrushesByGuid(const std::vector<std::string>& guids)
    {
        std::vector<ModToolsBrush> out(guids.size(), nullptr);
        if (guids.empty() || !Has(&host->FindBrushesByGuid) || !host->FindBrushesByGuid) return out;
        std::vector<const char*> keys; keys.reserve(guids.size());
        for (const std::string& g : guids) keys.push_back(g.c_str());
        host->FindBrushesByGuid(self, keys.data(), keys.size(), out.data());
        return out;
    }
    inline std::vector<ModToolsEntity> FindEntitiesByGuid(const std::vector<std::string>& guids)
    {
        std::vector<ModToolsEntity> out(guids.size(), nullptr);
        if (guids.empty() || !Has(&host->FindEntitiesByGuid) || !host->FindEntitiesByGuid) return out;
        std::vector<const char*> keys; keys.reserve(guids.size());
        for (const std::string& g : guids) keys.push_back(g.c_str());
        host->FindEntitiesByGuid(self, keys.data(), keys.size(), out.data());
        return out;
    }
    inline std::vector<float> BrushPlanes(ModToolsBrush b)
    {
        std::vector<float> v;
        if (!Has(&host->BrushPlanes) || !host->BrushPlanes) return v;
        v.resize(9 * 64);
        size_t n = host->BrushPlanes(self, b, v.data(), 64);
        if (n > 64) { v.resize(9 * n); n = host->BrushPlanes(self, b, v.data(), n); }
        v.resize(9 * n);
        return v;
    }
    // Untracked edits and undo tags (see ModToolsHostApi::BeginUntracked), false/no-op on hosts without them.
    inline bool HasUntracked() { return Has(&host->SetUndoTags) && host->BeginUntracked && host->EndUntracked && host->GetUndoTags && host->SetUndoTags; }
    inline void BeginUntracked() { if (HasUntracked()) host->BeginUntracked(self); }
    inline void EndUntracked() { if (HasUntracked()) host->EndUntracked(self); }
    inline bool GetUndoTags(void* o, bool entity, int tags[3]) { return HasUntracked() && host->GetUndoTags(self, o, entity ? 1 : 0, tags) != 0; }
    inline bool SetUndoTags(void* o, bool entity, const int tags[3]) { return HasUntracked() && host->SetUndoTags(self, o, entity ? 1 : 0, tags) != 0; }
    inline bool SetBrushPlanes(ModToolsBrush b, const std::vector<float>& planes)
    {
        return Has(&host->SetBrushPlanes) && host->SetBrushPlanes && !planes.empty() && planes.size() % 9 == 0 && host->SetBrushPlanes(self, b, planes.data(), planes.size() / 9) != 0;
    }
    inline bool EntityFixedSize(ModToolsEntity e) { return Has(&host->EntityFixedSize) && host->EntityFixedSize && host->EntityFixedSize(self, e) != 0; }
    inline ModToolsBrush BrushOfInst(ModToolsBrushInst inst) { return Has(&host->BrushOfInst) && host->BrushOfInst ? host->BrushOfInst(self, inst) : nullptr; }
    inline bool WriteEntityTextWith(ModToolsEntity e, const std::vector<ModToolsBrush>& brushes, std::string& out)
    {
        if (!Has(&host->WriteEntityTextWith) || !host->WriteEntityTextWith) return false;
        char* s = host->WriteEntityTextWith(self, e, brushes.data(), brushes.size());
        if (!s) return false;
        out = s; host->FreeString(self, s);
        return true;
    }
    inline bool MoveBrush(ModToolsBrush b, const float delta[3]) { return Has(&host->MoveBrush) && host->MoveBrush && host->MoveBrush(self, b, delta) != 0; }
    inline bool BrushBounds(ModToolsBrush b, float mins[3], float maxs[3]) { return Has(&host->BrushBounds) && host->BrushBounds && host->BrushBounds(self, b, mins, maxs) != 0; }
    // Run a lambda (or any callable) on the main thread.
    template <typename F> void PostToMainThread(F fn)
    {
        struct Box { F f; };
        host->PostToMainThread(self, [](void* u) { Box* b = static_cast<Box*>(u); b->f(); delete b; }, new Box{ std::move(fn) });
    }
    template <typename F> void ForEachEntity(F fn)
    {
        host->ForEachEntity(self, [](ModToolsEntity e, void* u) { (*static_cast<F*>(u))(e); }, &fn);
    }
    template <typename F> void ForEachEntityBrush(ModToolsEntity e, F fn)
    {
        host->ForEachEntityBrush(self, e, [](ModToolsBrush b, void* u) { (*static_cast<F*>(u))(b); }, &fn);
    }

    // ---- [Radiant] entity keys and in-place edits (MAIN THREAD) --------------------------------------------------
    // Radiant has no get/set key call: an entity is read as .map text (WriteEntityText) and changed by replacing it
    // (delete and import in one undo step, old GUIDs written back). See GETTING_STARTED.md section 7.

    // An entity's keys in file order ("classname" included). Values never contain a double quote or a line break.
    using KeyValues = std::vector<std::pair<std::string, std::string>>;

    namespace detail
    {
        inline std::string Trim(const std::string& s)
        {
            size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        }
        // Calls fn(trimmed line, raw line, depth before the line) for every line of entity text. Depth 1 is the
        // entity block itself (guid, layer and key lines), 2 and more are its brushes. Braces stand on their own lines.
        template <typename F> void ForEachLine(const std::string& text, F fn)
        {
            int depth = 0; size_t pos = 0;
            while (pos < text.size())
            {
                size_t nl = text.find('\n', pos);
                std::string raw = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
                pos = nl == std::string::npos ? text.size() : nl + 1;
                if (!raw.empty() && raw.back() == '\r') raw.pop_back();
                std::string line = Trim(raw);
                fn(line, raw, depth);
                if (line == "{") ++depth; else if (line == "}") --depth;
            }
        }
        // `"key" "value"` -> key, value, false for any other line
        inline bool SplitKeyLine(const std::string& line, std::string& key, std::string& value)
        {
            if (line.size() < 2 || line[0] != '"') return false;
            size_t q1 = line.find('"', 1), q2 = q1 == std::string::npos ? q1 : line.find('"', q1 + 1), q3 = line.rfind('"');
            if (q2 == std::string::npos || q3 <= q2) return false;
            key = line.substr(1, q1 - 1); value = line.substr(q2 + 1, q3 - q2 - 1);
            return true;
        }
        inline bool CleanText(const std::string& s) { return s.find_first_of("\"\r\n") == std::string::npos; }
        inline bool SameText(const std::string& a, const char* b) { return _stricmp(a.c_str(), b) == 0; }
        // The block Radiant's paste merges into the world (empty for an import that only adds entities).
        inline const char* EmptyWorldBlock() { return "{\n\"classname\" \"worldspawn\"\n}\n"; }
        // Hosts before 1.1.0 drop every target and linkto key on import.
        inline bool ImportKeepsLinks() { return host && host->hostVersion >= 0x10100; }
    }

    // Keys of entity text as WriteEntityText gives it (the entity's own lines, not its brushes').
    inline KeyValues ParseKeys(const std::string& entityText)
    {
        KeyValues kv; std::string k, v;
        detail::ForEachLine(entityText, [&](const std::string& line, const std::string&, int depth) {
            if (depth == 1 && detail::SplitKeyLine(line, k, v)) kv.emplace_back(k, v);
        });
        return kv;
    }
    inline std::string FindKey(const KeyValues& kv, const std::string& key, const std::string& def = "")
    {
        for (const auto& p : kv) if (p.first == key) return p.second;
        return def;
    }
    inline KeyValues GetKeys(ModToolsEntity e) { std::string t; return e && WriteEntityText(e, t) ? ParseKeys(t) : KeyValues(); }
    inline std::string GetKey(ModToolsEntity e, const std::string& key, const std::string& def = "") { return FindKey(GetKeys(e), key, def); }

    // The GUIDs of the brush blocks in entity text, in order (a point entity's text has none).
    inline std::vector<std::string> BrushGuidsInText(const std::string& entityText)
    {
        std::vector<std::string> out;
        detail::ForEachLine(entityText, [&](const std::string& line, const std::string&, int depth) {
            if (depth == 2 && line.compare(0, 5, "guid ") == 0)
            {
                std::string g = detail::Trim(line.substr(5));
                if (g.size() >= 2 && g.front() == '"') g = g.substr(1, g.size() - 2);
                out.push_back(g);
            }
        });
        return out;
    }

    // Entity text with `set` keys changed (or added after the last key when missing) and `remove` keys dropped.
    // Brushes, the guid line and the layer line are kept as they are.
    inline std::string SetKeysInText(const std::string& entityText, const KeyValues& set, const std::vector<std::string>& remove = {})
    {
        std::string out; out.reserve(entityText.size() + 64 * set.size());
        std::vector<bool> written(set.size(), false);
        size_t insertAt = std::string::npos;    // right after the last line of the entity's own header
        std::string k, v;
        detail::ForEachLine(entityText, [&](const std::string& line, const std::string& raw, int depth) {
            if (depth == 1 && detail::SplitKeyLine(line, k, v))
            {
                bool drop = false;
                for (const std::string& r : remove) if (r == k) drop = true;
                for (size_t i = 0; i < set.size() && !drop; ++i)
                    if (set[i].first == k) { if (!written[i]) out += "\"" + k + "\" \"" + set[i].second + "\"\n"; written[i] = true; drop = true; }
                if (!drop) out += raw + "\n";
                insertAt = out.size();
                return;
            }
            if (depth == 1 && (line.compare(0, 5, "guid ") == 0 || line.compare(0, 6, "layer ") == 0) && insertAt == std::string::npos) { out += raw + "\n"; insertAt = out.size(); return; }
            out += raw + "\n";
        });
        std::string add;
        for (size_t i = 0; i < set.size(); ++i) if (!written[i]) add += "\"" + set[i].first + "\" \"" + set[i].second + "\"\n";
        if (!add.empty()) out.insert(insertAt == std::string::npos ? 0 : insertAt, add);
        return out;
    }

    // The GUIDs of the entities that own selected brushes, once each, in selection order (worldspawn left out).
    inline std::vector<std::string> SelectedEntityGuids()
    {
        std::vector<std::string> out;
        ModToolsEntity w = host->WorldEntity(self);
        std::string world = w ? EntityGuid(w) : std::string();
        for (const ModToolsSelectedBrush& s : GetSelection())
        {
            std::string g = s.entityGuid;
            bool seen = g == world;
            for (const std::string& o : out) if (o == g) seen = true;
            if (!seen) out.push_back(g);
        }
        return out;
    }

    // Add entities from .map text (one or more "{ ... }" entity blocks, a leading worldspawn block is added for
    // you). One undo step named `undoLabel`. The new objects replace the selection and get fresh GUIDs,
    // read them back with SelectedEntityGuids(). Keys are kept as written (hosts before 1.1.0 drop target and linkto).
    inline bool ImportEntities(const std::string& entityBlocks, const char* undoLabel)
    {
        host->DeselectAll(self);
        std::string text = std::string(detail::EmptyWorldBlock()) + entityBlocks;
        return host->ImportMapText(self, text.c_str(), undoLabel) != 0;
    }

    // Replace a live entity with `newText` (its WriteEntityText, edited) as one undo step, keeping GUIDs. Returns its GUID, "" on failure (logged).
    // `e` is gone afterwards (use FindEntitiesByGuid), the new one is selected. Refused: worldspawn, reflection_probe, "target"/"linkto" keys on hosts before 1.1.0.
    // A misc_prefab gets a NEW GUID. Ctrl+Z restores point entities exactly, but an entity that owns brushes comes back as world brushes only.
    inline std::string ReplaceEntity(ModToolsEntity e, const std::string& newText, const char* undoLabel)
    {
        if (!e) return std::string();
        if (e == host->WorldEntity(self)) { Log("ReplaceEntity: worldspawn cannot be rebuilt in place"); return std::string(); }
        std::string oldText;
        if (!WriteEntityText(e, oldText)) { Log("ReplaceEntity: the entity could not be read"); return std::string(); }
        std::string cls = FindKey(ParseKeys(oldText), "classname"), guid = EntityGuid(e);
        if (detail::SameText(cls, "reflection_probe")) { Log("ReplaceEntity: %s cannot be rebuilt in place", cls.c_str()); return std::string(); }
        for (const auto& kv : ParseKeys(newText))
            if (!detail::ImportKeepsLinks() && (kv.first == "target" || kv.first == "linkto"))
            { Log("ReplaceEntity: %s %s has a '%s' key, which this host's import would drop (update MTPlugins, or edit it in Entity Info)", cls.c_str(), guid.c_str(), kv.first.c_str()); return std::string(); }
        host->DeselectAll(self);    // links selected brushes back into their entity
        std::vector<ModToolsBrush> brushes;
        ForEachEntityBrush(e, [&](ModToolsBrush b) { brushes.push_back(b); });
        if (brushes.empty()) { Log("ReplaceEntity: %s %s has no brushes to replace", cls.c_str(), guid.c_str()); return std::string(); }
        std::vector<std::string> brushGuids = BrushGuidsInText(newText);
        bool bracket = Has(&host->EndUndo) && host->BeginUndo && host->EndUndo;    // one Ctrl+Z
        if (bracket) host->BeginUndo(self, undoLabel);
        std::string result;
        if (!host->DeleteBrushes(self, brushes.data(), brushes.size(), undoLabel))
            Log("ReplaceEntity: Radiant's delete failed for %s", guid.c_str());
        else if (!host->ImportMapText(self, (std::string(detail::EmptyWorldBlock()) + newText).c_str(), undoLabel))
            Log("ReplaceEntity: Radiant refused the new text of %s (the old entity is deleted; Ctrl+Z brings it back)", guid.c_str());
        else
        {
            std::vector<std::string> fresh = SelectedEntityGuids();    // the import leaves the new entity selected
            bool restamp = !detail::SameText(cls, "misc_prefab");
            std::vector<const char*> bg; for (const std::string& g : brushGuids) bg.push_back(g.c_str());
            if (restamp && host->RestampImportedGuids(self, guid.c_str(), bg.data(), bg.size())) result = guid;
            else result = fresh.empty() ? std::string() : fresh.front();
        }
        if (bracket) host->EndUndo(self);
        return result;
    }

    // Set and remove keys of one entity (see ReplaceEntity for what that does and its limits). "classname" cannot be
    // removed and "guid" is not a key. Returns the entity's GUID afterwards, "" on failure.
    inline std::string SetKeys(ModToolsEntity e, const KeyValues& set, const std::vector<std::string>& remove, const char* undoLabel)
    {
        for (const auto& p : set)
            if (p.first.empty() || p.first == "guid" || !detail::CleanText(p.first) || !detail::CleanText(p.second))
            { Log("SetKeys: key '%s' or its value is not allowed (no quotes or line breaks)", p.first.c_str()); return std::string(); }
        for (const std::string& r : remove) if (r == "classname") { Log("SetKeys: classname cannot be removed"); return std::string(); }
        std::string text;
        if (!e || !WriteEntityText(e, text)) return std::string();
        return ReplaceEntity(e, SetKeysInText(text, set, remove), undoLabel);
    }
    inline std::string SetKey(ModToolsEntity e, const std::string& key, const std::string& value, const char* undoLabel)
    {
        return SetKeys(e, { { key, value } }, {}, undoLabel);
    }
}

#ifdef MODTOOLS_PLUGIN_IMPL
namespace rp { ModToolsPlugin* self = nullptr; const ModToolsHostApi* host = nullptr; }
#endif
