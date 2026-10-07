// Map load file prefetch, see loadprefetch.h. Adapted from LauncherPlus's Light step prefetch
// (plugins/LauncherPlus/light/light_prefetch.cpp), but recording runs per Map_LoadFile (plus a short tail) and every
// load of a map with a list starts its own prefetch. The headless -led Radiant is left to LauncherPlus.
#include "modtools_plugin.hpp"
#include "loadprefetch.h"
#include "assettouch.h"
#include <windows.h>
#include <shellapi.h>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace br::loadpf
{
namespace
{
    using CreateFileWFn = HANDLE (WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    using CreateFileAFn = HANDLE (WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    CreateFileWFn g_createFileW = nullptr;
    CreateFileAFn g_createFileA = nullptr;

    constexpr size_t   kMaxListed = 200000;
    constexpr uint64_t kTailMs = 3000;     // opens right after Map_LoadFile count too

    bool g_enabled = false;
    bool g_hooked = false;
    std::wstring g_dir;                    // <plugin>\load_prefetch
    std::wstring g_tempDir;                // lower case
    thread_local bool t_prefetcher = false;

    // ---- recording (any thread while g_recording) ----
    std::atomic<bool> g_recording{false};
    SRWLOCK g_recordLock = SRWLOCK_INIT;
    std::unordered_set<std::wstring> g_readSeen;
    std::vector<std::wstring> g_readOrder;
    std::unordered_set<std::wstring> g_written;
    std::atomic<long long> g_opens{0}, g_openTicks{0};
    LONG64 g_freq = 1;

    // ---- the load being recorded (main thread) ----
    std::wstring g_curList;                // list file name of the map being loaded
    std::string  g_curMap;
    uint64_t g_loadStart = 0, g_loadEnd = 0;

    // ---- prefetch jobs ----
    struct Job
    {
        std::wstring name;
        std::vector<std::wstring> list;
        std::atomic<long> next{0}, opened{0}, live{0};
        std::atomic<bool> cancel{false};
        uint64_t start = 0;
        std::atomic<uint64_t> end{0};
    };
    std::vector<Job*> g_jobs;              // kept until Shutdown: a cancelled job's threads may still read it
    Job* g_active = nullptr;
    std::atomic<bool> g_stop{false};
    std::atomic<long> g_liveThreads{0};

    std::string g_lastReport = "no map loaded yet";

    std::wstring Lower(std::wstring s)
    {
        if (!s.empty()) CharLowerBuffW(&s[0], DWORD(s.size()));
        return s;
    }

    std::string Narrow(const std::wstring& w)
    {
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
        std::string out(n > 0 ? n : 0, '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), &out[0], n, nullptr, nullptr);
        return out;
    }

    void Record(const wchar_t* name, DWORD access, DWORD disposition, DWORD flags)
    {
        if ((flags & FILE_FLAG_BACKUP_SEMANTICS) || !wcsncmp(name, L"\\\\.\\", 4)) return;
        const bool writes = (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE)) || disposition != OPEN_EXISTING;
        const bool reads = (access & (GENERIC_READ | FILE_READ_DATA)) != 0;
        if (!writes && !reads) return;
        std::wstring key = Lower(name);
        for (wchar_t& c : key) if (c == L'/') c = L'\\';
        const auto endsWith = [&](const wchar_t* tail) { const size_t n = wcslen(tail); return key.size() >= n && !key.compare(key.size() - n, n, tail); };
        // never listed: temp files and SQLite databases (gdt.db is written by several programs)
        if (!writes && (endsWith(L".tmp") || endsWith(L".db") ||
                        (!g_tempDir.empty() && key.size() > g_tempDir.size() && !key.compare(0, g_tempDir.size(), g_tempDir))))
            return;
        AcquireSRWLockExclusive(&g_recordLock);
        if (writes) g_written.insert(std::move(key));
        else if (g_readOrder.size() < kMaxListed && g_readSeen.insert(key).second) g_readOrder.push_back(std::move(key));
        ReleaseSRWLockExclusive(&g_recordLock);
    }

    // per path while recording: opens, failed opens, write opens, time (for the report's slowest paths)
    struct PathStat { long opens = 0, fails = 0, writes = 0; long long ticks = 0; };
    SRWLOCK g_statLock = SRWLOCK_INIT;
    std::unordered_map<std::wstring, PathStat> g_stats;

    void Stat(const wchar_t* name, DWORD access, long long ticks, bool ok)
    {
        std::wstring key = Lower(name);
        AcquireSRWLockExclusive(&g_statLock);
        PathStat& st = g_stats[key];
        ++st.opens; st.ticks += ticks;
        if (!ok) ++st.fails;
        if (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES | DELETE)) ++st.writes;
        ReleaseSRWLockExclusive(&g_statLock);
    }

    HANDLE WINAPI HookCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE templ)
    {
        if (!g_recording.load(std::memory_order_relaxed) || t_prefetcher || !name)
            return g_createFileW(name, access, share, sa, disposition, flags, templ);
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        const HANDLE h = g_createFileW(name, access, share, sa, disposition, flags, templ);
        const DWORD error = GetLastError();
        QueryPerformanceCounter(&t1);
        g_openTicks.fetch_add(t1.QuadPart - t0.QuadPart, std::memory_order_relaxed);
        g_opens.fetch_add(1, std::memory_order_relaxed);
        Stat(name, access, t1.QuadPart - t0.QuadPart, h != INVALID_HANDLE_VALUE);
        if (h != INVALID_HANDLE_VALUE) Record(name, access, disposition, flags);
        SetLastError(error);
        return h;
    }

    // kernelbase!CreateFileA does not go through CreateFileW's entry.
    HANDLE WINAPI HookCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE templ)
    {
        if (!g_recording.load(std::memory_order_relaxed) || t_prefetcher || !name)
            return g_createFileA(name, access, share, sa, disposition, flags, templ);
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        const HANDLE h = g_createFileA(name, access, share, sa, disposition, flags, templ);
        const DWORD error = GetLastError();
        QueryPerformanceCounter(&t1);
        g_openTicks.fetch_add(t1.QuadPart - t0.QuadPart, std::memory_order_relaxed);
        g_opens.fetch_add(1, std::memory_order_relaxed);
        wchar_t wide[MAX_PATH * 2];
        if (MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH * 2) > 0)
        {
            Stat(wide, access, t1.QuadPart - t0.QuadPart, h != INVALID_HANDLE_VALUE);
            if (h != INVALID_HANDLE_VALUE) Record(wide, access, disposition, flags);
        }
        SetLastError(error);
        return h;
    }

    DWORD WINAPI PrefetchMain(void* param)
    {
        // hold a reference on this DLL so an unload that times out cannot unmap the running code
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&PrefetchMain), &self);
        t_prefetcher = true;
        Job* job = static_cast<Job*>(param);
        while (!g_stop.load(std::memory_order_relaxed) && !job->cancel.load(std::memory_order_relaxed))
        {
            const long i = job->next.fetch_add(1);
            if (i >= long(job->list.size())) break;
            const HANDLE h = CreateFileW(job->list[i].c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); job->opened.fetch_add(1); }
        }
        if (job->live.fetch_sub(1) == 1) job->end = GetTickCount64();
        g_liveThreads.fetch_sub(1);
        if (self) FreeLibraryAndExitThread(self, 0);
        return 0;
    }

    // list name: the map's file name plus a hash of its full lower-case path
    std::wstring ListName(const std::wstring& mapPath)
    {
        wchar_t full[MAX_PATH * 2] = {};
        const DWORD n = GetFullPathNameW(mapPath.c_str(), MAX_PATH * 2, full, nullptr);
        std::wstring map = Lower(n && n < MAX_PATH * 2 ? full : mapPath);
        for (wchar_t& c : map) if (c == L'/') c = L'\\';
        if (map.size() < 5 || map.compare(map.size() - 4, 4, L".map")) return L"";
        unsigned hash = 2166136261u;
        for (wchar_t c : map) hash = (hash ^ c) * 16777619u;
        std::wstring name = map.substr(map.find_last_of(L'\\') + 1);
        name.resize(name.size() - 4);
        for (wchar_t& c : name) if (!iswalnum(c) && c != L'_' && c != L'-') c = L'_';
        wchar_t tail[32];
        swprintf_s(tail, L"_%08x.txt", hash);
        return name.substr(0, 64) + tail;
    }

    std::string ReadSmallFile(const std::wstring& path)
    {
        std::string text;
        const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return text;
        LARGE_INTEGER size = {};
        if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (64 << 20))
        {
            text.resize(size_t(size.QuadPart));
            DWORD got = 0;
            if (!ReadFile(f, &text[0], DWORD(text.size()), &got, nullptr)) got = 0;
            text.resize(got);
        }
        CloseHandle(f);
        return text;
    }

    bool WriteFileAtomic(const std::wstring& path, const std::string& text)
    {
        CreateDirectoryW(g_dir.c_str(), nullptr);
        const std::wstring tmp = path + L".tmp";
        const HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        DWORD wrote = 0;
        const bool ok = WriteFile(f, text.data(), DWORD(text.size()), &wrote, nullptr) && wrote == text.size();
        CloseHandle(f);
        if (ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
        DeleteFileW(tmp.c_str());
        return false;
    }

    // starts the prefetch of one list unless it is already running or just ran
    void StartJob(const std::wstring& name, const char* why)
    {
        if (name.empty() || (g_active && g_active->name == name)) return;
        const std::string text = ReadSmallFile(g_dir + L"\\" + name);
        if (text.empty()) return;
        Job* job = new Job();
        job->name = name;
        for (size_t at = 0; at < text.size();)
        {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            const int len = int(end - at);
            if (len > 0)
            {
                std::wstring w(len, L'\0');
                w.resize(MultiByteToWideChar(CP_UTF8, 0, text.data() + at, len, &w[0], len));
                if (!w.empty()) job->list.push_back(std::move(w));
            }
            at = end + 1;
        }
        if (job->list.empty()) { delete job; return; }
        if (g_active) g_active->cancel = true;
        g_jobs.push_back(job);
        g_active = job;
        SYSTEM_INFO info = {};
        GetSystemInfo(&info);
        const int cores = int(info.dwNumberOfProcessors);
        const int threads = cores < 4 ? 4 : cores > 16 ? 16 : cores;
        job->start = GetTickCount64();
        job->live = threads;
        for (int i = 0; i < threads; ++i)
        {
            g_liveThreads.fetch_add(1);
            const HANDLE t = CreateThread(nullptr, 0, &PrefetchMain, job, 0, nullptr);
            if (t) { SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t); continue; }
            g_liveThreads.fetch_sub(1);
            if (job->live.fetch_sub(1) == 1) job->end = GetTickCount64();
        }
        rp::Log("loadprefetch: opening %zu files of %s on %d threads (%s)", job->list.size(), Narrow(name).c_str(), threads, why);
    }

    // writes the recorded list of the load that just finished and the report line
    void Finish()
    {
        g_recording = false;
        AcquireSRWLockExclusive(&g_recordLock);
        std::string text;
        size_t count = 0;
        for (const std::wstring& p : g_readOrder)
        {
            if (g_written.count(p)) continue;
            text += Narrow(p);
            text += '\n';
            ++count;
        }
        // a second load in the same Radiant skips files it already holds, so keep old entries whose files still exist
        const std::string old = ReadSmallFile(g_dir + L"\\" + g_curList);
        for (size_t at = 0; at < old.size() && count < kMaxListed;)
        {
            size_t end = old.find('\n', at);
            if (end == std::string::npos) end = old.size();
            const int len = int(end - at);
            if (len > 0)
            {
                std::wstring w(len, L'\0');
                w.resize(MultiByteToWideChar(CP_UTF8, 0, old.data() + at, len, &w[0], len));
                if (!w.empty() && !g_readSeen.count(w) && !g_written.count(w) && GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES)
                {
                    text.append(old, at, len);
                    text += '\n';
                    ++count;
                }
            }
            at = end + 1;
        }
        g_readOrder.clear(); g_readSeen.clear(); g_written.clear();
        ReleaseSRWLockExclusive(&g_recordLock);

        const bool saved = !text.empty() && WriteFileAtomic(g_dir + L"\\" + g_curList, text);
        if (saved) WriteFileAtomic(g_dir + L"\\last.txt", Narrow(g_curList));
        char prefetch[160] = "no list yet (first load of this map since the prefetch came in)";
        if (g_active && g_active->name == g_curList)
        {
            const uint64_t end = g_active->end.load();
            snprintf(prefetch, sizeof prefetch, "prefetch opened %ld of %zu files in %.1f s%s", g_active->opened.load(),
                     g_active->list.size(), ((end ? end : GetTickCount64()) - g_active->start) / 1000.0, end ? "" : " (still running)");
        }
        char line[640];
        snprintf(line, sizeof line, "%s: Map_LoadFile %.1f s; Radiant's opens during it and %llu s after: %lld in %.2f s (all threads); %s; list %s (%zu files)",
                 g_curMap.c_str(), (g_loadEnd - g_loadStart) / 1000.0, (unsigned long long)(kTailMs / 1000), g_opens.load(),
                 double(g_openTicks.load()) / double(g_freq), prefetch, saved ? "saved" : "not saved", count);
        g_lastReport = line;
        rp::Log("loadprefetch: %s", line);
        // where the open time went: failed opens, write opens, then the slowest paths
        AcquireSRWLockExclusive(&g_statLock);
        std::vector<std::pair<long long, const std::wstring*>> top;
        long long failTicks = 0, writeTicks = 0; long fails = 0, writes = 0, paths = 0;
        for (auto& kv : g_stats)
        {
            ++paths;
            fails += kv.second.fails; writes += kv.second.writes;
            if (kv.second.fails) failTicks += kv.second.ticks;
            if (kv.second.writes) writeTicks += kv.second.ticks;
            top.push_back({ kv.second.ticks, &kv.first });
        }
        std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
        rp::Log("loadprefetch:   %ld paths; %ld failed opens (paths with one: %.2f s), %ld opens with write access (paths with one: %.2f s)",
                paths, fails, double(failTicks) / double(g_freq), writes, double(writeTicks) / double(g_freq));
        for (size_t i = 0; i < top.size() && i < 12; ++i)
        {
            const PathStat& st = g_stats[*top[i].second];
            rp::Log("loadprefetch:   %.2f s %ld opens (%ld failed, %ld write) %s", double(st.ticks) / double(g_freq), st.opens, st.fails, st.writes,
                    Narrow(*top[i].second).c_str());
        }
        g_stats.clear();
        ReleaseSRWLockExclusive(&g_statLock);
    }

    std::wstring CommandLineMap()
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::wstring map;
        for (int i = 1; argv && i < argc; ++i)
        {
            const std::wstring a = Lower(argv[i]);
            if (a.size() > 4 && !a.compare(a.size() - 4, 4, L".map")) { map = argv[i]; break; }
        }
        if (argv) LocalFree(argv);
        return map;
    }

    bool Headless()
    {
        const std::wstring cl = Lower(GetCommandLineW());
        return cl.find(L" -led") != std::wstring::npos;   // the Light step's Radiant (LauncherPlus's prefetch runs there)
    }

    bool HookExport(const char* proc, void* detour, void** original)
    {
        HMODULE m = GetModuleHandleW(L"kernelbase.dll");
        void* target = m ? reinterpret_cast<void*>(GetProcAddress(m, proc)) : nullptr;
        return target && rp::host->Detour(rp::self, target, detour, original) != 0;
    }
}

void Setup(const std::wstring& pluginDir)
{
    g_enabled = rp::ConfigGetBool("performance", "load_prefetch", true);
    g_dir = pluginDir + L"\\load_prefetch";
    LARGE_INTEGER qpf; QueryPerformanceFrequency(&qpf); g_freq = qpf.QuadPart ? qpf.QuadPart : 1;
    wchar_t temp[MAX_PATH + 1] = {};
    const DWORD tn = GetTempPathW(MAX_PATH + 1, temp);
    if (tn && tn <= MAX_PATH)
    {
        wchar_t longTemp[MAX_PATH * 2] = {};
        const DWORD ln = GetLongPathNameW(temp, longTemp, MAX_PATH * 2);
        g_tempDir = Lower(ln && ln < MAX_PATH * 2 ? longTemp : temp);
    }
    if (Headless())
    {
        rp::Log("loadprefetch: off in the Light step's headless Radiant (LauncherPlus prefetches there)");
        g_enabled = false;
        return;
    }
    g_hooked = HookExport("CreateFileW", reinterpret_cast<void*>(&HookCreateFileW), reinterpret_cast<void**>(&g_createFileW)) &&
               HookExport("CreateFileA", reinterpret_cast<void*>(&HookCreateFileA), reinterpret_cast<void**>(&g_createFileA));
    rp::Log("loadprefetch: map load prefetch enabled=%d hooks=%d", g_enabled ? 1 : 0, g_hooked ? 1 : 0);
    if (!g_enabled || !g_hooked) return;
    // during Radiant's startup: the command-line map, else the last one loaded
    const std::wstring cmdMap = CommandLineMap();
    if (!cmdMap.empty()) StartJob(ListName(cmdMap), "the map on the command line");
    else
    {
        std::string last = ReadSmallFile(g_dir + L"\\last.txt");
        while (!last.empty() && (last.back() == '\n' || last.back() == '\r' || last.back() == ' ')) last.pop_back();
        if (!last.empty() && last.find_first_of("\\/:") == std::string::npos)
        {
            std::wstring w(last.size(), L'\0');
            w.resize(MultiByteToWideChar(CP_UTF8, 0, last.data(), int(last.size()), &w[0], int(w.size())));
            StartJob(w, "the map loaded last");
        }
    }
}

void MapLoadBegin(const wchar_t* path)
{
    if (!g_enabled || !g_hooked || !path || !*path) return;
    if (g_recording) Finish();             // a load inside the last one's tail: close that one first
    g_curList = ListName(path);
    if (g_curList.empty()) return;
    g_curMap = Narrow(path);
    const size_t slash = g_curMap.find_last_of("\\/");
    if (slash != std::string::npos) g_curMap = g_curMap.substr(slash + 1);
    StartJob(g_curList, "map load");
    g_opens = 0; g_openTicks = 0;
    g_loadStart = GetTickCount64();
    g_loadEnd = 0;
    g_recording = true;
}

void MapLoadEnd()
{
    if (g_recording) g_loadEnd = GetTickCount64();
}

void Tick()
{
    if (g_recording && g_loadEnd && GetTickCount64() - g_loadEnd >= kTailMs) Finish();
}

void Shutdown()
{
    g_recording = false;
    g_stop = true;
    for (int i = 0; i < 500 && g_liveThreads.load() > 0; ++i) Sleep(10);
    if (g_liveThreads.load() == 0)
    {
        for (Job* j : g_jobs) delete j;
        g_jobs.clear();
        g_active = nullptr;
    }
    else rp::Log("loadprefetch: %ld prefetch threads still running at unload (they hold the DLL until they end)", g_liveThreads.load());
}

bool Command(const std::string& c, std::string& reply)
{
    if (c.rfind("loadprefetch", 0) != 0 || (c.size() > 12 && c[12] != ' ')) return false;
    const std::string arg = c.size() > 13 ? c.substr(13) : "status";
    if (arg == "0" || arg == "1")
    {
        g_enabled = arg == "1";
        rp::ConfigSet("performance", "load_prefetch", arg);
    }
    else if (arg == "clear")
    {
        WIN32_FIND_DATAW fd;
        const HANDLE f = FindFirstFileW((g_dir + L"\\*.txt").c_str(), &fd);
        int n = 0;
        if (f != INVALID_HANDLE_VALUE)
        {
            do { if (DeleteFileW((g_dir + L"\\" + fd.cFileName).c_str())) ++n; } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        reply = "loadprefetch: deleted " + std::to_string(n) + " lists";
        return true;
    }
    else if (arg == "reload")
    {
        // test loop: reload the open map on the next idle tick (Map_LoadFile through the host, no prompt)
        rp::host->PostToMainThread(rp::self, [](void*) {
            wchar_t map[MAX_PATH * 2] = {};
            if (rp::host->CurrentMapPath(rp::self, map, MAX_PATH * 2))
                rp::Log("loadprefetch: test reload -> %d", rp::host->ReloadMap(rp::self, map, 1));
        }, nullptr);
        reply = "loadprefetch: reload queued";
        return true;
    }
    else if (arg != "status")
    {
        reply = "loadprefetch [status|0|1|clear|reload]";
        return true;
    }
    reply = std::string("loadprefetch: enabled=") + (g_enabled ? "1" : "0") + " hooks=" + (g_hooked ? "1" : "0") +
            "; last load: " + g_lastReport + "; " + br::assettouch::Status();
    return true;
}
}
