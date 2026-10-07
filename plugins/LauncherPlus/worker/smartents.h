// Smart compile: run cod2map with -onlyents when nothing that shapes geometry changed (SHA-256 signature, see kSeedClasses),
// a failed run is followed by a full one. State per output BSP: <plugin dir>\compile_state\.
#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <string>
#include <vector>
#include <set>
#include <cstdio>
#include <cstring>
#include <cwctype>

namespace smartents
{
    // Classnames whose positions seed the nav mesh flood, so -onlyents cannot redo them. Wildcards, case-insensitive.
    // Fallback copy of the "seeds" list in <root>\radiant\configs\navmesh.json.
    const char* const kSeedClasses[] = { "node_*", "mp_*spawn*", "cp_*spawn*", "info_player_start", "actor_*" };

    inline bool Wildcard(const char* pattern, const char* text)
    {
        for (;;)
        {
            if (*pattern == '*')
            {
                while (*pattern == '*') ++pattern;
                if (!*pattern) return true;
                for (const char* t = text; ; ++t)
                {
                    if (Wildcard(pattern, t)) return true;
                    if (!*t) return false;
                }
            }
            if (!*text) return !*pattern;
            if (!*pattern || tolower((unsigned char)*pattern) != tolower((unsigned char)*text)) return false;
            ++pattern; ++text;
        }
    }

    // "seeds": [ { "type": "x" }, ... ] out of navmesh.json (a crude scan: the file is Treyarch's, small and flat)
    inline std::vector<std::string> SeedPatterns(const std::string& json)
    {
        std::vector<std::string> out;
        size_t pos = json.find("\"seeds\"");
        if (pos == std::string::npos) return out;
        const size_t end = json.find(']', pos);
        while ((pos = json.find("\"type\"", pos)) != std::string::npos && pos < end)
        {
            const size_t q1 = json.find('"', json.find(':', pos));
            const size_t q2 = q1 == std::string::npos ? q1 : json.find('"', q1 + 1);
            if (q2 == std::string::npos || q2 > end) break;
            out.push_back(json.substr(q1 + 1, q2 - q1 - 1));
            pos = q2;
        }
        return out;
    }

    struct Sha256
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        bool ok = false;
        Sha256()
        {
            ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
                 BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
        }
        ~Sha256()
        {
            if (hash) BCryptDestroyHash(hash);
            if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        void Add(const void* data, size_t size)
        {
            if (ok && size) ok = BCryptHashData(hash, (PUCHAR)data, (ULONG)size, 0) >= 0;
        }
        void Add(const std::string& s) { Add(s.data(), s.size()); Add("\n", 1); }
        std::string Hex()
        {
            unsigned char digest[32] = {};
            if (!ok || BCryptFinishHash(hash, digest, sizeof digest, 0) < 0) return "";
            char out[65];
            for (int i = 0; i < 32; ++i) snprintf(out + 2 * i, 3, "%02x", digest[i]);
            return out;
        }
    };

    inline std::string Narrow(const std::wstring& w)
    {
        if (w.empty()) return "";
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string out(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), n, nullptr, nullptr);
        return out;
    }

    inline std::wstring Widen(const std::string& s)
    {
        if (s.empty()) return L"";
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring out(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
        return out;
    }

    inline std::wstring Lower(std::wstring s) { for (wchar_t& c : s) c = (wchar_t)towlower(c); return s; }

    inline bool ReadWhole(const std::wstring& path, std::string& out)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size = {};
        if (!GetFileSizeEx(h, &size) || size.QuadPart > (1LL << 31)) { CloseHandle(h); return false; }
        out.resize((size_t)size.QuadPart);
        size_t off = 0;
        while (off < out.size())
        {
            DWORD got = 0;
            if (!ReadFile(h, out.data() + off, (DWORD)(std::min<size_t>)(out.size() - off, 1u << 20), &got, nullptr) || !got) { CloseHandle(h); return false; }
            off += got;
        }
        CloseHandle(h);
        return true;
    }

    // size + last write time, or empty when the file is missing
    inline std::string Identity(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA fa = {};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return "";
        char out[64];
        snprintf(out, sizeof out, "%llu,%llu", ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow,
                 ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime);
        return out;
    }

    struct Entity { size_t begin, end; bool nested; std::string classname; };

    // Top-level { } blocks, braces inside quoted strings do not count (GUIDs are written as "{...}").
    inline bool SplitEntities(const std::string& text, std::vector<Entity>& out)
    {
        int depth = 0;
        bool quoted = false;
        Entity cur = {};
        for (size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            if (c == '"') { quoted = !quoted; continue; }
            if (c == '\n') quoted = false;   // a stray quote never swallows the rest of the file
            if (quoted) continue;
            if (c == '{')
            {
                if (depth == 0) cur = { i, 0, false, "" };
                else if (depth == 1) cur.nested = true;
                ++depth;
            }
            else if (c == '}')
            {
                if (depth == 0) return false;
                if (--depth == 0)
                {
                    cur.end = i + 1;
                    const size_t k = text.find("\"classname\" \"", cur.begin);
                    if (k != std::string::npos && k < cur.end)
                    {
                        const size_t v = k + 13, e = text.find('"', v);
                        if (e != std::string::npos && e < cur.end) cur.classname = text.substr(v, e - v);
                    }
                    out.push_back(cur);
                }
            }
        }
        return depth == 0;
    }


    inline std::string Value(const std::string& text, const Entity& ent, const char* key)
    {
        const std::string needle = std::string("\"") + key + "\" \"";
        const size_t k = text.find(needle, ent.begin);
        if (k == std::string::npos || k >= ent.end) return "";
        const size_t v = k + needle.size(), e = text.find('"', v);
        return e == std::string::npos || e >= ent.end ? "" : text.substr(v, e - v);
    }

    struct Signature
    {
        Sha256 sha;
        std::wstring mapSourceRoot;   // <root>\map_source\ (prefab "model" values are relative to it)
        std::vector<std::string> seeds;
        std::set<std::wstring> visiting;

        bool IsSeedClass(const std::string& classname) const
        {
            for (const std::string& p : seeds) if (Wildcard(p.c_str(), classname.c_str())) return true;
            return false;
        }

        // a whole file's bytes (nav settings): missing is a state of its own, not a failure
        void HashConfig(const std::wstring& path)
        {
            std::string text;
            if (ReadWhole(path, text)) { sha.Add("config " + Narrow(Lower(path))); sha.Add(text); }
            else sha.Add("no config " + Narrow(Lower(path)));
        }
        std::string failure;          // set when a file could not be read or parsed: no signature, full compile
        int files = 0, geometry = 0, points = 0;

        void HashMap(const std::wstring& path, int depth)
        {
            if (!failure.empty()) return;
            const std::wstring key = Lower(path);
            if (depth > 16 || visiting.count(key)) { failure = "prefab nesting too deep or cyclic at " + Narrow(path); return; }
            std::string text;
            if (!ReadWhole(path, text))
            {
                // cod2map compiles without a missing prefab, the reference is in the parent's entity text anyway
                if (depth > 0 && GetLastError() == ERROR_FILE_NOT_FOUND) { sha.Add("missing " + Narrow(key)); return; }
                failure = "cannot read " + Narrow(path);
                return;
            }
            std::vector<Entity> entities;
            if (!SplitEntities(text, entities)) { failure = "unbalanced braces in " + Narrow(path); return; }
            visiting.insert(key);
            ++files;
            sha.Add("file " + Narrow(key));
            for (const Entity& ent : entities)
            {
                const bool prefab = !_stricmp(ent.classname.c_str(), "misc_prefab");
                // a misc_model with "bake" set is baked into the nav mesh input, so it counts as geometry
                const bool baked = !_stricmp(ent.classname.c_str(), "misc_model") && atoi(Value(text, ent, "bake").c_str()) != 0;
                // volume decals fill LUMP_VOLUME_DECALS, which -onlyents rebuilds wrongly (decals.h), so they must be unchanged
                const bool decal = !_stricmp(ent.classname.c_str(), "misc_volume_decal");
                if (!ent.nested && !prefab && !baked && !decal && !IsSeedClass(ent.classname)) { ++points; continue; }
                ++geometry;
                // entity text minus carriage returns (a CRLF/LF resave must not force a full compile)
                std::string body;
                body.reserve(ent.end - ent.begin);
                for (size_t i = ent.begin; i < ent.end; ++i) if (text[i] != '\r') body += text[i];
                sha.Add(body);
                if (prefab)
                {
                    std::string model = Value(text, ent, "model");
                    for (char& c : model) if (c == '/') c = '\\';
                    if (!model.empty()) HashMap(mapSourceRoot + Widen(model), depth + 1);
                }
            }
            visiting.erase(key);
        }
    };

    struct State
    {
        std::string signature, bsp, navmesh, navvolume;
        bool Load(const std::wstring& path)
        {
            std::string text;
            if (!ReadWhole(path, text)) return false;
            size_t pos = 0;
            while (pos < text.size())
            {
                size_t end = text.find('\n', pos);
                if (end == std::string::npos) end = text.size();
                std::string line = text.substr(pos, end - pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const size_t eq = line.find('=');
                if (eq != std::string::npos)
                {
                    const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
                    if (k == "signature") signature = v; else if (k == "bsp") bsp = v;
                    else if (k == "navmesh") navmesh = v; else if (k == "navvolume") navvolume = v;
                }
                pos = end + 1;
            }
            return !signature.empty();
        }
        bool Save(const std::wstring& path, const std::wstring& output, const std::wstring& map) const
        {
            CreateDirectoryW(path.substr(0, path.find_last_of(L'\\')).c_str(), nullptr);
            FILE* f = _wfopen(path.c_str(), L"wb");
            if (!f) return false;
            fprintf(f, "version=1\noutput=%s\nmap=%s\nsignature=%s\nbsp=%s\nnavmesh=%s\nnavvolume=%s\n",
                    Narrow(output).c_str(), Narrow(map).c_str(), signature.c_str(), bsp.c_str(), navmesh.c_str(), navvolume.c_str());
            fclose(f);
            return true;
        }
    };

    struct Plan
    {
        bool applicable = false;        // the command line is a compile we understand (-loadFrom <map> <out.d3dbsp>)
        bool callerOnlyEnts = false;    // the caller (PlayTest) passed -onlyents itself: no decision, no state
        bool onlyEnts = false;          // the decision
        std::string reason;             // why (for the launcher log)
        std::string signature;
        std::wstring map, output, navmesh, navvolume, statePath;
    };

    inline std::wstring FullPath(const std::wstring& p)
    {
        std::vector<wchar_t> buf(32768);
        const DWORD n = GetFullPathNameW(p.c_str(), (DWORD)buf.size(), buf.data(), nullptr);
        return n && n < buf.size() ? std::wstring(buf.data(), n) : p;
    }

    inline std::wstring StripExt(const std::wstring& p)
    {
        const size_t dot = p.find_last_of(L'.'), slash = p.find_last_of(L"\\/");
        return dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash) ? p.substr(0, dot) : p;
    }

    // real = <root>\bin\cod2map64.exe, args = the launcher's compiler arguments. With enabled=false only the command line is parsed.
    inline Plan Decide(const std::wstring& real, const std::vector<std::wstring>& args, const std::wstring& pluginDir, bool enabled)
    {
        Plan plan;
        for (size_t i = 0; i < args.size(); ++i)
        {
            if (!_wcsicmp(args[i].c_str(), L"-onlyents")) plan.callerOnlyEnts = true;
            if (!_wcsicmp(args[i].c_str(), L"-loadFrom") && i + 1 < args.size()) plan.map = FullPath(args[++i]);
        }
        if (args.empty() || plan.map.empty() || Lower(args.back()).find(L".d3dbsp") == std::wstring::npos)
        {
            plan.reason = "unrecognised compiler command line";
            return plan;
        }
        plan.output = FullPath(args.back());
        plan.navmesh = StripExt(plan.output) + L"_navmesh.hkt";
        plan.navvolume = StripExt(plan.output) + L"_navvolume.hkt";
        if (plan.callerOnlyEnts) { plan.reason = "the caller already asked for -onlyents"; return plan; }
        if (!enabled) { plan.reason = "off in LauncherPlus.ini"; return plan; }
        plan.applicable = true;

        std::wstring bin = real.substr(0, real.find_last_of(L"\\/"));
        std::wstring root = bin.substr(0, bin.find_last_of(L"\\/"));
        Signature sig;
        sig.mapSourceRoot = root + L"\\map_source\\";
        sig.sha.Add("buildplus smart compile v1");
        for (const std::wstring& a : args) sig.sha.Add("arg " + Narrow(Lower(a)));
        // the nav mesh generator's inputs besides the map: settings (bin\, per-map override) and the entity class lists
        {
            std::string navJson;
            const std::wstring navConfig = root + L"\\radiant\\configs\\navmesh.json";
            if (ReadWhole(navConfig, navJson)) sig.seeds = SeedPatterns(navJson);
            if (sig.seeds.empty()) for (const char* s : kSeedClasses) sig.seeds.push_back(s);
            sig.HashConfig(navConfig);
            sig.HashConfig(bin + L"\\default_navmesh_settings.json");
            sig.HashConfig(StripExt(plan.map) + L"_navmesh_settings.json");
            sig.HashConfig(StripExt(plan.output) + L"_navmesh_settings.json");
        }
        sig.HashMap(plan.map, 0);
        if (!sig.failure.empty()) { plan.applicable = false; plan.reason = sig.failure; return plan; }
        plan.signature = sig.sha.Hex();
        if (plan.signature.empty()) { plan.applicable = false; plan.reason = "SHA-256 unavailable"; return plan; }

        {
            Sha256 name;
            const std::string key = Narrow(Lower(plan.output));
            name.Add(key.data(), key.size());
            plan.statePath = pluginDir + L"\\compile_state\\" + Widen(name.Hex().substr(0, 24)) + L".txt";
        }
        State previous;
        if (!previous.Load(plan.statePath)) { plan.reason = "first compile of this map through BetterBuild"; return plan; }
        if (previous.signature != plan.signature) { plan.reason = "brushes, patches, prefabs or player starts changed"; return plan; }
        const std::string bsp = Identity(plan.output);
        if (bsp.empty()) { plan.reason = "the previous output is missing"; return plan; }
        if (bsp != previous.bsp) { plan.reason = "the previous output was replaced by another compile"; return plan; }
        if (Identity(plan.navmesh).empty() || Identity(plan.navvolume).empty()) { plan.reason = "the nav mesh files beside the output are missing"; return plan; }
        if (Identity(plan.navmesh) != previous.navmesh || Identity(plan.navvolume) != previous.navvolume) { plan.reason = "the nav mesh files beside the output were replaced"; return plan; }
        plan.onlyEnts = true;
        char buf[256];
        snprintf(buf, sizeof buf, "only entities changed (%d files, %d geometry entities and %d point entities checked)", sig.files, sig.geometry, sig.points);
        plan.reason = buf;
        return plan;
    }

    // After a successful compile (exit 0, output present): remember what it produced.
    inline bool Remember(const Plan& plan)
    {
        State now;
        now.signature = plan.signature;
        now.bsp = Identity(plan.output);
        now.navmesh = Identity(plan.navmesh);
        now.navvolume = Identity(plan.navvolume);
        if (now.bsp.empty()) return false;
        return now.Save(plan.statePath, plan.output, plan.map);
    }

    inline void Forget(const Plan& plan) { DeleteFileW(plan.statePath.c_str()); }
}
