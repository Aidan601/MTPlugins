// skip_localized: keep the localized fastfile instead of relinking it when nothing it depends on changed (hooks msvcr110!_spawnv).
// A skip returns 0, any other value makes the linker print an error. State: linker_state\<map>_<language>.txt.
#pragma once
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#include <sys/stat.h>
#include <string>
#include <vector>
#include <map>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace localized
{
    using SpawnvFn = intptr_t (__cdecl*)(int, const char*, const char* const*);
    using StatFn = int (__cdecl*)(const char*, struct _stat64i32*);
    inline SpawnvFn g_spawnv = nullptr;
    inline StatFn g_stat = nullptr;
    inline std::wstring g_stateDir;

    inline std::wstring Widen(const std::string& s)
    {
        if (s.empty()) return L"";
        const int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring out(n, 0);
        MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), out.data(), n);
        return out;
    }

    inline std::string Narrow(const std::wstring& w)
    {
        if (w.empty()) return "";
        const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string out(n, 0);
        WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), out.data(), n, nullptr, nullptr);
        return out;
    }

    inline std::string LowerA(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }

    inline bool ReadAll(const std::wstring& path, std::string& out)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size = {};
        if (!GetFileSizeEx(h, &size) || size.QuadPart > (1LL << 30)) { CloseHandle(h); return false; }
        out.resize((size_t)size.QuadPart);
        size_t off = 0;
        while (off < out.size())
        {
            DWORD got = 0;
            const DWORD want = (DWORD)(out.size() - off > (1u << 22) ? (1u << 22) : out.size() - off);
            if (!ReadFile(h, out.data() + off, want, &got, nullptr) || !got) { CloseHandle(h); return false; }
            off += got;
        }
        CloseHandle(h);
        return true;
    }

    inline std::string Sha256Hex(const std::string& data)
    {
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        unsigned char digest[32] = {};
        bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
                  BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0 &&
                  BCryptHashData(hash, (PUCHAR)data.data(), (ULONG)data.size(), 0) >= 0 &&
                  BCryptFinishHash(hash, digest, sizeof digest, 0) >= 0;
        if (hash) BCryptDestroyHash(hash);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        if (!ok) return "";
        char out[65];
        for (int i = 0; i < 32; ++i) snprintf(out + 2 * i, 3, "%02x", digest[i]);
        return out;
    }

    inline std::string FileHash(const std::wstring& path)
    {
        std::string data;
        return ReadAll(path, data) ? Sha256Hex(data) : "";
    }

    // size + last write time, "" when missing
    inline std::string Identity(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA fa = {};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return "";
        char out[64];
        snprintf(out, sizeof out, "%llu,%llu", ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow,
                 ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime);
        return out;
    }

    struct Job
    {
        std::string map, language;
        std::wstring root, deps, statePath, mapDirLower, gdtDb, zoneDir;
    };

    inline bool Parse(const char* const* argv, Job& job)
    {
        // argv seen: -language english -modsource <dir> -spawnedchild -localized <map>. -modsource's value is the first non-option token after it.
        bool spawned = false;
        for (size_t i = 0; argv && argv[i]; ++i)
        {
            if (!_stricmp(argv[i], "-spawnedchild")) spawned = true;
            else if (!_stricmp(argv[i], "-language") && argv[i + 1] && argv[i + 1][0] != '-') job.language = argv[++i];
            else if (!_stricmp(argv[i], "-modsource"))
            {
                size_t k = i + 1;
                while (argv[k] && argv[k][0] == '-') { if (!_stricmp(argv[k], "-spawnedchild")) spawned = true; ++k; }
                if (argv[k]) job.map = argv[k];
                i = argv[k] ? k : i;
            }
        }
        if (!spawned || job.map.empty() || job.language.empty()) return false;
        wchar_t exe[MAX_PATH * 2] = {};
        if (!GetModuleFileNameW(nullptr, exe, MAX_PATH * 2)) return false;
        std::wstring bin(exe);
        bin = bin.substr(0, bin.find_last_of(L"\\/"));
        job.root = bin.substr(0, bin.find_last_of(L"\\/"));
        const std::wstring mapDir = job.root + L"\\usermaps\\" + Widen(job.map);
        job.deps = mapDir + L"\\zone_source\\" + Widen(job.language) + L"\\assetinfo\\" + Widen(job.map) + L".deps";
        job.zoneDir = mapDir + L"\\zone";
        job.mapDirLower = mapDir + L"\\";
        CharLowerBuffW(job.mapDirLower.data(), (DWORD)job.mapDirLower.size());
        job.gdtDb = job.root + L"\\gdtdb\\gdt.db";
        job.statePath = g_stateDir + L"\\" + Widen(job.map) + L"_" + Widen(job.language) + L".txt";
        return true;
    }

    struct State
    {
        std::map<std::string, std::string> kv;   // deps=, gdt=, ff=, ffid=, hash:<lower path>=, kept= (time of the last skip)
        bool Load(const std::wstring& path)
        {
            std::string text;
            if (!ReadAll(path, text)) return false;
            size_t pos = 0;
            while (pos < text.size())
            {
                size_t end = text.find('\n', pos);
                if (end == std::string::npos) end = text.size();
                std::string line = text.substr(pos, end - pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const size_t eq = line.find('=');
                if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
                pos = end + 1;
            }
            return kv.count("deps") && kv.count("ff") && kv.count("ffid") && kv.count("gdt");
        }
        bool Save(const std::wstring& path) const
        {
            CreateDirectoryW(path.substr(0, path.find_last_of(L'\\')).c_str(), nullptr);
            FILE* f = _wfopen(path.c_str(), L"wb");
            if (!f) return false;
            for (const auto& [k, v] : kv) fprintf(f, "%s=%s\n", k.c_str(), v.c_str());
            fclose(f);
            return true;
        }
    };

    struct DepFile { std::string path; long long mtime; };
    struct Deps { std::vector<DepFile> files; long long errorcount = -1; bool ok = false; };

    inline Deps ParseDeps(const std::wstring& path)
    {
        Deps out;
        std::string text;
        if (!ReadAll(path, text)) return out;
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            size_t first = pos;
            while (first < end && (text[first] == ' ' || text[first] == '\t')) ++first;
            std::string line = text.substr(first, end - first);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.compare(0, 5, "file,"))
            {
                const size_t c1 = line.find(',', 5);
                if (c1 != std::string::npos) out.files.push_back({ line.substr(5, c1 - 5), _atoi64(line.c_str() + c1 + 1) });
            }
            else if (!line.compare(0, 11, "errorcount,")) out.errorcount = _atoi64(line.c_str() + 11);
            pos = end + 1;
        }
        out.ok = !out.files.empty() && out.errorcount >= 0;
        return out;
    }

    inline bool InsideMap(const Job& job, const std::string& path)
    {
        std::wstring w = Widen(path);
        CharLowerBuffW(w.data(), (DWORD)w.size());
        return w.compare(0, job.mapDirLower.size(), job.mapDirLower) == 0;
    }

    // The newest *_<map>.ff in zone\ other than <map>.ff: the child's output
    inline std::wstring FindLocalizedFf(const Job& job)
    {
        WIN32_FIND_DATAW fd = {};
        HANDLE h = FindFirstFileW((job.zoneDir + L"\\*_" + Widen(job.map) + L".ff").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return L"";
        std::wstring best;
        ULONGLONG bestTime = 0;
        do
        {
            const ULONGLONG t = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
            if (t >= bestTime) { bestTime = t; best = job.zoneDir + L"\\" + fd.cFileName; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return best;
    }

    // "" = keep the fastfile, otherwise why the child has to run
    inline std::string Decide(const Job& job, size_t& checked)
    {
        checked = 0;
        State st;
        if (!st.Load(job.statePath)) return "first link recorded by BetterBuild";
        if (Identity(job.deps) != st.kv["deps"]) return "the dependency list was rewritten by another link";
        if (Identity(job.gdtDb) != st.kv["gdt"]) return "gdt.db changed";
        const std::wstring ff = Widen(st.kv["ff"]);
        if (Identity(ff).empty()) return "the localized fastfile is missing";
        if (Identity(ff) != st.kv["ffid"]) return "the localized fastfile was rewritten by another link";
        const Deps deps = ParseDeps(job.deps);
        if (!deps.ok) return "no usable dependency list";
        if (deps.errorcount) return "the last localized link reported errors";
        for (const DepFile& f : deps.files)
        {
            ++checked;
            if (InsideMap(job, f.path))
            {
                auto it = st.kv.find("hash:" + LowerA(f.path));
                if (it == st.kv.end() || it->second != FileHash(Widen(f.path))) return f.path + " changed";
                continue;
            }
            struct _stat64i32 s = {};
            if (!g_stat || g_stat(f.path.c_str(), &s) != 0) return f.path + " is missing";
            if ((long long)s.st_mtime != f.mtime) return f.path + " changed";
        }
        return "";
    }

    inline void Remember(const Job& job)
    {
        const Deps deps = ParseDeps(job.deps);
        if (!deps.ok || deps.errorcount) { DeleteFileW(job.statePath.c_str()); return; }
        const std::wstring ff = FindLocalizedFf(job);
        if (ff.empty()) { DeleteFileW(job.statePath.c_str()); return; }
        State st;
        st.kv["deps"] = Identity(job.deps);
        st.kv["gdt"] = Identity(job.gdtDb);
        st.kv["ff"] = Narrow(ff);
        st.kv["ffid"] = Identity(ff);
        for (const DepFile& f : deps.files)
            if (InsideMap(job, f.path)) st.kv["hash:" + LowerA(f.path)] = FileHash(Widen(f.path));
        st.Save(job.statePath);
    }

    inline intptr_t __cdecl HookSpawnv(int mode, const char* path, const char* const* argv)
    {
        Job job;
        if (!Parse(argv, job)) return g_spawnv(mode, path, argv);
        const ULONGLONG t0 = GetTickCount64();
        size_t checked = 0;
        const std::string why = Decide(job, checked);
        if (why.empty())
        {
            fprintf(stdout, "[BetterBuild] Localized fastfile kept: none of its %zu inputs changed since the last link (checked in %llu ms; skip_localized=0 in LauncherPlus.ini always relinks it).\n",
                    checked, GetTickCount64() - t0);
            fflush(stdout);
            // kept=<FILETIME of this link>: read by the Publish dialog's language check (publish.cpp), not by Decide()
            State st;
            if (st.Load(job.statePath))
            {
                FILETIME now; GetSystemTimeAsFileTime(&now);
                st.kv["kept"] = std::to_string(((unsigned long long)now.dwHighDateTime << 32) | now.dwLowDateTime);
                st.Save(job.statePath);
            }
            return 0;
        }
        fprintf(stdout, "[BetterBuild] Localized fastfile relinked: %s.\n", why.c_str());
        fflush(stdout);
        // The exit code is no verdict (2000 after good links): a .deps rewritten during the spawn is what Remember() judges by.
        const std::string depsBefore = Identity(job.deps);
        const intptr_t result = g_spawnv(mode, path, argv);
        if (Identity(job.deps) != depsBefore && !Identity(job.deps).empty()) Remember(job);
        else DeleteFileW(job.statePath.c_str());
        return result;
    }

#ifndef LOCALIZED_DECIDE_ONLY   // publish.cpp reuses Decide() without hooks
    // Hooks are created here, enabled by the caller's MH_EnableHook(MH_ALL_HOOKS).
    inline bool Install(const std::wstring& stateDir)
    {
        g_stateDir = stateDir;
        HMODULE crt = GetModuleHandleW(L"msvcr110.dll");
        if (!crt) crt = LoadLibraryW(L"msvcr110.dll");
        if (!crt) return false;
        g_stat = reinterpret_cast<StatFn>(GetProcAddress(crt, "_stat64i32"));
        if (!g_stat) return false;
        return MH_CreateHookApi(L"msvcr110", "_spawnv", reinterpret_cast<LPVOID>(&HookSpawnv), reinterpret_cast<LPVOID*>(&g_spawnv)) == MH_OK;
    }
#endif
}
