// skip_sound_convert: do not start sound\snd_convert.exe when nothing it reads changed (hooks kernelbase!CreateProcessA, linker+0x7C760).
// A skip, or a run finished here with its output passed through, returns TRUE with signaled event handles in PROCESS_INFORMATION. State: linker_state\<map>_snd_<language>.txt.
#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "bcrypt.lib")
#include <algorithm>
#include <string>
#include <vector>
#include <map>
#include <cstdio>
#include <cstring>

namespace soundskip
{
    using CreateProcessAFn = BOOL (WINAPI*)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
    inline CreateProcessAFn g_createProcessA = nullptr;
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
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string out(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), n, nullptr, nullptr);
        return out;
    }

    inline std::wstring LowerW(std::wstring s) { if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size()); return s; }

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

    inline std::string Identity(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA fa = {};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return "";
        char out[64];
        snprintf(out, sizeof out, "%llu,%llu", ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow,
                 ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime);
        return out;
    }

    struct Sha256
    {
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        bool ok = false;
        size_t files = 0;
        Sha256()
        {
            ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
                 BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
        }
        ~Sha256()
        {
            if (hash) BCryptDestroyHash(hash);
            if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        }
        void Add(const void* p, size_t n) { if (ok && n) ok = BCryptHashData(hash, (PUCHAR)p, (ULONG)n, 0) >= 0; }
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

    // Every file under dir (recursive), sorted by lower-case relative path (stable order).
    struct Entry { std::wstring rel, full; unsigned long long size, mtime; };
    inline void Walk(const std::wstring& dir, const std::wstring& rel, const std::wstring& skipRelLower, std::vector<Entry>& out, int depth = 0)
    {
        if (depth > 24) return;
        WIN32_FIND_DATAW fd = {};
        HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) return;
        do
        {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            const std::wstring r = rel.empty() ? fd.cFileName : rel + L"\\" + fd.cFileName;
            if (!skipRelLower.empty() && LowerW(r) == skipRelLower) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) Walk(dir + L"\\" + fd.cFileName, r, skipRelLower, out, depth + 1);
            else out.push_back({ r, dir + L"\\" + fd.cFileName, ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow,
                                 ((unsigned long long)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime });
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    inline void SortEntries(std::vector<Entry>& v)
    {
        std::sort(v.begin(), v.end(), [](const Entry& a, const Entry& b) { return LowerW(a.rel) < LowerW(b.rel); });
    }

    struct Job
    {
        std::wstring root, mapDir, zone, language, statePath;
        std::string commandLine;
    };

    // "<exe>" "" pc usermaps\<map> usermaps\<map> zone_source usermaps\<map> <language> <zone> [skip_deps]
    inline bool Parse(LPCSTR app, LPCSTR cmd, Job& job)
    {
        if (!app || !cmd) return false;
        std::wstring exe = LowerW(Widen(app));
        const size_t k = exe.rfind(L"\\sound\\snd_convert.exe");
        if (k == std::wstring::npos) return false;
        job.root = Widen(app).substr(0, k);
        job.commandLine = cmd;
        std::vector<std::string> tok;
        const char* p = cmd;
        while (*p)
        {
            while (*p == ' ') ++p;
            if (!*p) break;
            std::string t;
            if (*p == '"') { ++p; while (*p && *p != '"') t += *p++; if (*p) ++p; }
            else while (*p && *p != ' ') t += *p++;
            tok.push_back(t);
        }
        // the first %s was empty in every observed link, so anchor on "zone_source" (followed by <map dir> <language> <zone>)
        size_t zs = 0;
        while (zs < tok.size() && _stricmp(tok[zs].c_str(), "zone_source")) ++zs;
        if (zs + 3 >= tok.size()) return false;
        const std::string& mapTok = tok[zs + 1];
        job.mapDir = job.root + L"\\" + Widen(mapTok);
        job.language = Widen(tok[zs + 2]);
        job.zone = Widen(tok[zs + 3]);
        job.statePath = g_stateDir + L"\\" + Widen(mapTok.substr(mapTok.find_last_of("\\/") + 1)) + L"_snd_" + job.language + L".txt";
        return true;
    }

    // What the signature reads from disk, apart from the zone file and the tool's identity.
    struct Inputs
    {
        std::wstring root, mapDir;
        std::vector<Entry> cfg, defs, srcs;
        std::vector<std::string> data;
        std::vector<char> ok;
    };
    inline void Collect(const std::wstring& root, const std::wstring& mapDir, Inputs& in)
    {
        in.root = root; in.mapDir = mapDir;
        // configuration by content: share\raw\sound (26 MB) and the map's own sound folder minus its zone\ outputs
        Walk(root + L"\\share\\raw\\sound", L"", L"", in.cfg);
        Walk(mapDir + L"\\sound", L"", L"zone", in.cfg);
        SortEntries(in.cfg);
        in.data.resize(in.cfg.size());
        in.ok.resize(in.cfg.size());
        for (size_t i = 0; i < in.cfg.size(); ++i) in.ok[i] = ReadAll(in.cfg[i].full, in.data[i]) ? 1 : 0;
        Walk(root + L"\\deffiles", L"", L"", in.defs);
        SortEntries(in.defs);
        Walk(root + L"\\sound_assets", L"", L"", in.srcs);
        SortEntries(in.srcs);
    }

    inline Inputs* g_pre = nullptr;
    inline HANDLE g_preDone = nullptr;
    // TA_TOOLS_PATH ends with a backslash ("III\\usermaps"), so separators are collapsed before comparing
    inline std::wstring NormPath(const std::wstring& in)
    {
        std::wstring p;
        for (wchar_t c : in) { if (c == L'/') c = L'\\'; if (c == L'\\' && !p.empty() && p.back() == L'\\') continue; p += c; }
        while (!p.empty() && p.back() == L'\\') p.pop_back();
        return LowerW(p);
    }
    // The map from the linker's command line. Root = the folder above bin\.
    inline void StartPrefetch()
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) return;
        // -modsource <map>; the localized child's line is -modsource -spawnedchild -localized <map>
        std::wstring map;
        for (int i = 1; i < argc; ++i)
            if (!_wcsicmp(argv[i], L"-modsource") && i + 1 < argc) map = argv[i + 1][0] == L'-' ? argv[argc - 1] : argv[i + 1];
        LocalFree(argv);
        wchar_t exe[MAX_PATH * 2] = {};
        const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
        if (map.empty() || map[0] == L'-' || !n || n >= MAX_PATH * 2) return;
        std::wstring root(exe);
        root = root.substr(0, root.find_last_of(L"\\/"));    // <root>\bin
        root = root.substr(0, root.find_last_of(L"\\/"));    // <root>
        g_pre = new Inputs{};
        g_pre->root = root;
        g_pre->mapDir = root + L"\\usermaps\\" + map;
        g_preDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!g_preDone) { delete g_pre; g_pre = nullptr; return; }
        HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            const std::wstring root = g_pre->root, mapDir = g_pre->mapDir;
            Collect(root, mapDir, *g_pre);
            SetEvent(g_preDone);
            return 0;
        }, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
        else { CloseHandle(g_preDone); g_preDone = nullptr; delete g_pre; g_pre = nullptr; }
    }
    // The prefetched inputs when they are for this job (taken once), else null.
    inline Inputs* TakePrefetched(const Job& job)
    {
        if (!g_preDone || !g_pre) return nullptr;
        if (WaitForSingleObject(g_preDone, 120000) != WAIT_OBJECT_0) return nullptr;
        Inputs* p = g_pre;
        g_pre = nullptr;
        if (NormPath(p->root) == NormPath(job.root) && NormPath(p->mapDir) == NormPath(job.mapDir)) return p;
        delete p;
        return nullptr;
    }

    inline std::string Signature(const Job& job, size_t& files)
    {
        Sha256 sha;
        sha.Add("buildplus sound skip v1");
        sha.Add("tool " + Identity(job.root + L"\\sound\\snd_convert.exe") + " " + Identity(job.root + L"\\sound\\snd_encode.dll"));
        sha.Add("cmd " + job.commandLine);
        Inputs local;
        Inputs* pre = TakePrefetched(job);
        if (!pre) Collect(job.root, job.mapDir, local);
        const Inputs& in = pre ? *pre : local;
        for (size_t i = 0; i < in.cfg.size(); ++i)
        {
            sha.Add("cfg " + Narrow(LowerW(in.cfg[i].rel)));
            if (in.ok[i]) sha.Add(in.data[i]); else sha.Add("unreadable");
        }
        files += in.cfg.size();
        std::string zoneText;
        sha.Add("zone " + (ReadAll(job.mapDir + L"\\zone_source\\" + job.zone + L".zone", zoneText) ? zoneText : std::string("missing")));
        // deffiles\*sound*.awi by identity
        for (const Entry& e : in.defs)
        {
            const std::wstring lower = LowerW(e.rel);
            if (lower.find(L"sound") == std::wstring::npos || lower.find(L".awi") == std::wstring::npos) continue;
            char buf[64]; snprintf(buf, sizeof buf, " %llu,%llu", e.size, e.mtime);
            sha.Add("def " + Narrow(lower) + buf);
            ++files;
        }
        // the wav sources: path, size and write time of everything under sound_assets (a directory walk only)
        for (const Entry& e : in.srcs)
        {
            char buf[64]; snprintf(buf, sizeof buf, " %llu,%llu", e.size, e.mtime);
            sha.Add("src " + Narrow(LowerW(e.rel)) + buf);
        }
        files += in.srcs.size();
        delete pre;
        return sha.Hex();
    }

    struct State
    {
        std::map<std::string, std::string> kv;
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
            return kv.count("sig") != 0;
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

    inline std::string Decide(const Job& job, const std::string& sig, size_t& outputs)
    {
        outputs = 0;
        State st;
        if (!st.Load(job.statePath)) return "first link recorded by BetterBuild";
        if (!st.kv["failed"].empty()) return "the last run exited with code " + st.kv["failed"];
        if (st.kv["sig"] != sig) return "its inputs changed";
        for (const auto& [k, v] : st.kv)
        {
            if (k.compare(0, 4, "out:")) continue;
            ++outputs;
            if (Identity(Widen(k.substr(4))) != v) return "its previous output " + k.substr(4) + " was replaced or is missing";
        }
        if (!outputs) return "no recorded output";
        return "";
    }

    // the parent's last state, re-read after the localized child (its own stock snd_convert run rewrites the language banks)
    inline std::wstring g_lastStatePath, g_lastMapDir;

    inline void RefreshOutputs()
    {
        if (g_lastStatePath.empty()) return;
        State st;
        if (!st.Load(g_lastStatePath) || !st.kv["failed"].empty()) return;
        for (auto it = st.kv.begin(); it != st.kv.end();) it = it->first.compare(0, 4, "out:") ? std::next(it) : st.kv.erase(it);
        std::vector<Entry> list;
        Walk(g_lastMapDir + L"\\sound\\zone", L"", L"", list);
        if (list.empty()) { DeleteFileW(g_lastStatePath.c_str()); return; }
        for (const Entry& e : list) st.kv["out:" + Narrow(e.full)] = Identity(e.full);
        st.Save(g_lastStatePath);
    }

    inline void Remember(const Job& job, const std::string& sig, DWORD code)
    {
        State st;
        st.kv["sig"] = sig;
        st.kv["cmd"] = job.commandLine;   // the whole-link check (linkskip.h) rebuilds the job from it
        // a failed run can stop before the loaded bank (.sabl) is written, so it is kept only to rerun the next link
        if (code != 0) { st.kv["failed"] = std::to_string(code); st.Save(job.statePath); return; }
        std::vector<Entry> list;
        Walk(job.mapDir + L"\\sound\\zone", L"", L"", list);
        if (list.empty()) { DeleteFileW(job.statePath.c_str()); return; }
        for (const Entry& e : list) st.kv["out:" + Narrow(e.full)] = Identity(e.full);
        st.Save(job.statePath);
    }

#ifndef SOUNDSKIP_DECIDE_ONLY   // linkskip.h reuses Parse, Signature and Decide without hooks
    // snd_convert hashes its sources on 4 threads into an unlocked Dictionary: a lost entry fails the run with "Missing source checksum".
    // (skip_sound_convert).
    constexpr int kRaceAttempts = 3;

    struct ToolOutput { HANDLE read; std::string text; };

    inline DWORD WINAPI PassOutput(LPVOID p)
    {
        auto* out = static_cast<ToolOutput*>(p);
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(out->read, buf, sizeof buf, &n, nullptr) && n)
        {
            fwrite(buf, 1, n, stdout);
            fflush(stdout);
            out->text.append(buf, n);
        }
        return 0;
    }

    // Runs the tool to its end with its output passed to ours (the linker starts it without handles, so stock hides every message).
    inline bool RunTool(LPCSTR app, LPCSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, DWORD flags, LPVOID env, LPCSTR cwd, LPSTARTUPINFOA si,
                        DWORD& code, bool& raced)
    {
        SECURITY_ATTRIBUTES inheritable = { sizeof inheritable, nullptr, TRUE };
        HANDLE readEnd = nullptr, writeEnd = nullptr;
        if (!CreatePipe(&readEnd, &writeEnd, &inheritable, 0)) return false;
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOA startup = si ? *si : STARTUPINFOA{};
        startup.cb = sizeof startup;
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = writeEnd;
        startup.hStdError = writeEnd;
        std::string line = cmd ? cmd : "";
        PROCESS_INFORMATION pi = {};
        const BOOL ok = g_createProcessA(app, line.data(), pa, ta, TRUE, flags & ~DWORD(CREATE_SUSPENDED), env, cwd, &startup, &pi);
        CloseHandle(writeEnd);
        if (!ok) { CloseHandle(readEnd); return false; }
        CloseHandle(pi.hThread);
        ToolOutput out{ readEnd, {} };
        HANDLE reader = CreateThread(nullptr, 0, &PassOutput, &out, 0, nullptr);
        if (!reader) PassOutput(&out);   // ends when the tool exits
        WaitForSingleObject(pi.hProcess, INFINITE);
        code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        if (reader)
        {
            // a process started by the tool may still hold the pipe
            while (WaitForSingleObject(reader, 2000) == WAIT_TIMEOUT) CancelSynchronousIo(reader);
            CloseHandle(reader);
        }
        CloseHandle(readEnd);
        raced = out.text.find("Missing source checksum") != std::string::npos;
        return true;
    }

    inline BOOL WINAPI HookCreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags,
                                          LPVOID env, LPCSTR cwd, LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi)
    {
        Job job;
        if (!Parse(app, cmd, job))
        {
            if (app && strstr(app, "snd_convert")) { fprintf(stdout, "[BetterBuild] Sound conversion: unrecognised command line, running it as stock does: %s\n", cmd ? cmd : ""); fflush(stdout); }
            return g_createProcessA(app, cmd, pa, ta, inherit, flags, env, cwd, si, pi);
        }
        const ULONGLONG t0 = GetTickCount64();
        size_t files = 0, outputs = 0;
        const std::string sig = Signature(job, files);
        const std::string why = sig.empty() ? "SHA-256 unavailable" : Decide(job, sig, outputs);
        if (why.empty() && pi)
        {
            fprintf(stdout, "[BetterBuild] Sound conversion skipped: nothing under sound_assets, share\\raw\\sound or the map's sound folder changed since the last link (%zu files checked in %llu ms; skip_sound_convert=0 in LauncherPlus.ini always runs it).\n",
                    files, GetTickCount64() - t0);
            fflush(stdout);
            State st;
            if (st.Load(job.statePath) && st.kv["cmd"] != job.commandLine) { st.kv["cmd"] = job.commandLine; st.Save(job.statePath); }
            g_lastStatePath = job.statePath; g_lastMapDir = job.mapDir;
            *pi = {};
            pi->hProcess = CreateEventW(nullptr, TRUE, TRUE, nullptr);
            pi->hThread = CreateEventW(nullptr, TRUE, TRUE, nullptr);
            pi->dwProcessId = GetCurrentProcessId();
            pi->dwThreadId = GetCurrentThreadId();
            return TRUE;
        }
        fprintf(stdout, "[BetterBuild] Sound conversion running: %s (%zu files checked in %llu ms).\n", why.c_str(), files, GetTickCount64() - t0);
        fflush(stdout);
        DWORD code = 1;
        bool raced = false;
        if (!pi || !RunTool(app, cmd, pa, ta, flags, env, cwd, si, code, raced))
            return g_createProcessA(app, cmd, pa, ta, inherit, flags, env, cwd, si, pi);
        for (int attempt = 2; code != 0 && raced && attempt <= kRaceAttempts; ++attempt)
        {
            fprintf(stdout, "[BetterBuild] snd_convert lost a file in its own parallel source scan (a bug in the tool, not in the file): running it again, attempt %d of %d.\n", attempt, kRaceAttempts);
            fflush(stdout);
            if (!RunTool(app, cmd, pa, ta, flags, env, cwd, si, code, raced)) break;
        }
        if (code != 0)
        {
            fprintf(stdout, "[BetterBuild] snd_convert exited with code %lu (its messages are above; the link continues as stock does, the next link runs it again).\n", code);
            fflush(stdout);
        }
        if (!sig.empty()) { Remember(job, sig, code); g_lastStatePath = job.statePath; g_lastMapDir = job.mapDir; }
        // the tool already ran: the linker waits on signaled events as after a skip
        *pi = {};
        pi->hProcess = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        pi->hThread = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        pi->dwProcessId = GetCurrentProcessId();
        pi->dwThreadId = GetCurrentThreadId();
        return TRUE;
    }

    inline bool Install(const std::wstring& stateDir)
    {
        g_stateDir = stateDir;
        // The packed linker's import table reaches kernelbase directly (kernel32's stub never fired)
        return MH_CreateHookApi(L"kernelbase", "CreateProcessA", reinterpret_cast<LPVOID>(&HookCreateProcessA), reinterpret_cast<LPVOID*>(&g_createProcessA)) == MH_OK;
    }
#endif
}
