// APE's startup GDT walk served from a folder index: import filters skip folders that lead to no indexed GDT, then a
// background full walk after the window is up rewrites the index and triggers APE's refresh on any difference.
// [ape] fast_gdt_scan=0 in RadiantPlugins.ini turns the index off (the check still runs).

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "ape.h"
#include "app.h"
#include "config.h"
#include "mainthread.h"
#include "plugins.h"
#include "log.h"
#include "radiant_addrs.h"

namespace
{
    using RefreshFn = void (*)(void* cache);
    using FindFirstExFn = HANDLE (WINAPI*)(LPCSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
    using FindNextFn = BOOL (WINAPI*)(HANDLE, LPWIN32_FIND_DATAA);
    using FindCloseFn = BOOL (WINAPI*)(HANDLE);

    using ReadChangesFn = BOOL (WINAPI*)(HANDLE, LPVOID, DWORD, BOOL, DWORD, LPDWORD, LPOVERLAPPED, LPOVERLAPPED_COMPLETION_ROUTINE);
    RefreshFn g_refresh = nullptr;
    ReadChangesFn g_rdc = nullptr;
    void** g_slotRDC = nullptr;
    std::atomic<bool> g_armed{ false }, g_ended{ false };
    std::mutex g_initMutex;
    std::atomic<bool> g_initDone{ false };
    bool g_initOk = false, g_fast = true;
    FindFirstExFn g_ff = nullptr;              // the values the exe's import slots held
    FindNextFn g_fn = nullptr;
    FindCloseFn g_fc = nullptr;
    void** g_slotFF = nullptr; void** g_slotFN = nullptr; void** g_slotFC = nullptr;

    std::atomic<bool> g_active{ false };       // from DllMain to the end of APE's startup walk
    bool g_prune = false;                      // index loaded and matching
    std::string g_prefix;                      // TA_GAME_PATH + "\" (APE: GetGamePathStr() + '\\' + folder + "\\*.*")
    std::vector<std::string> g_roots;          // apeFileCache +0x20, in order
    std::unordered_map<std::string, std::unordered_set<std::string>> g_children;   // known folder -> its subfolders on a path to a GDT
    std::atomic<int> g_opened{ 0 }, g_pruned{ 0 };

    struct Stamp { DWORD hi, lo; FILETIME t; };
    bool Same(const Stamp& a, const Stamp& b) { return a.hi == b.hi && a.lo == b.lo && a.t.dwLowDateTime == b.t.dwLowDateTime && a.t.dwHighDateTime == b.t.dwHighDateTime; }
    std::mutex g_seenMutex;
    std::unordered_map<std::string, Stamp> g_seen;   // what the startup walk listed: "<folder>\<lower-case name>" -> stamps

    // per thread: the find handle of a known folder this thread is listing (APE's workers open, step and close on one thread)
    thread_local HANDLE t_handle = nullptr;
    thread_local const std::unordered_set<std::string>* t_kids = nullptr;
    thread_local std::string t_rel;

    std::wstring g_dllDir;                     // the host's folder (Cfg() may not be loaded yet when the walk starts)
    std::wstring IndexPath() { return g_dllDir + L"\\RadiantPlugins.apegdt.txt"; }
    void* Cache() { return reinterpret_cast<void*>(app::ModuleBase() + (apeaddr::apeFileCache_Instance - 0x140000000ULL)); }

    void Lower(std::string& s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c + 32); }   // APE's _strlwr, "C" locale
    bool IsGdt(const char* name)
    {
        size_t n = strlen(name);
        return n >= 5 && name[n - 4] == '.' && (name[n - 3] | 0x20) == 'g' && (name[n - 2] | 0x20) == 'd' && (name[n - 1] | 0x20) == 't';
    }
    bool IsSubdir(const WIN32_FIND_DATAA& d) { return (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && d.cFileName[0] != '.'; }

    // the folder of a pattern APE's walker built, "" when it is not one
    bool RelOf(LPCSTR pattern, std::string& rel)
    {
        size_t n = strlen(pattern), p = g_prefix.size();
        if (n < p + 5 || strncmp(pattern, g_prefix.c_str(), p) != 0 || strcmp(pattern + n - 4, "\\*.*") != 0) return false;
        rel.assign(pattern + p, n - p - 4);
        return !rel.empty();
    }
    void Record(const std::string& rel, const WIN32_FIND_DATAA& d)
    {
        if ((d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !IsGdt(d.cFileName)) return;
        std::string key = d.cFileName; Lower(key); key = rel + "\\" + key;
        std::lock_guard<std::mutex> lock(g_seenMutex);
        g_seen[key] = Stamp{ d.nFileSizeHigh, d.nFileSizeLow, d.ftLastWriteTime };
    }
    void Init();
    void End(const char* why);
    bool Keep(const WIN32_FIND_DATAA& d) { return !t_kids || !IsSubdir(d) || t_kids->count(d.cFileName) != 0; }

    HANDLE WINAPI Hk_FindFirstFileExA(LPCSTR pattern, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags)
    {
        std::string rel;
        if (!g_active || !pattern || !data || level > FindExInfoBasic) return g_ff(pattern, level, data, op, filter, flags);
        if (!g_initDone) { std::lock_guard<std::mutex> lock(g_initMutex); if (!g_initDone) Init(); }
        if (!g_initOk || !RelOf(pattern, rel)) return g_ff(pattern, level, data, op, filter, flags);
        const std::unordered_set<std::string>* kids = nullptr;
        if (g_prune)
        {
            auto it = g_children.find(rel);
            if (it == g_children.end()) { ++g_pruned; SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
            kids = &it->second;
        }
        ++g_opened;
        HANDLE h = g_ff(pattern, level, data, op, filter, flags);
        if (h == INVALID_HANDLE_VALUE) return h;
        auto* d = static_cast<WIN32_FIND_DATAA*>(data);
        t_handle = h; t_kids = kids; t_rel = rel;
        while (!Keep(*d))
        {
            ++g_pruned;
            if (!g_fn(h, d)) { t_handle = nullptr; g_fc(h); SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
        }
        Record(rel, *d);
        return h;
    }
    BOOL WINAPI Hk_FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA d)
    {
        BOOL r = g_fn(h, d);
        if (h != t_handle || !d) return r;
        while (r && !Keep(*d)) { ++g_pruned; r = g_fn(h, d); }
        if (r) Record(t_rel, *d);
        return r;
    }
    BOOL WINAPI Hk_FindClose(HANDLE h)
    {
        if (h == t_handle) { t_handle = nullptr; t_kids = nullptr; }
        return g_fc(h);
    }

    // the exe's import slot of kernel32!<name>
    void** ImportSlot(const char* name)
    {
        auto* base = reinterpret_cast<BYTE*>(app::ModuleBase());
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) return nullptr;
        for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp)
        {
            if (_stricmp(reinterpret_cast<char*>(base + imp->Name), "kernel32.dll") != 0 || !imp->OriginalFirstThunk) continue;
            auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->OriginalFirstThunk);
            auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++slots)
            {
                if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
                auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (strcmp(reinterpret_cast<char*>(ibn->Name), name) == 0) return reinterpret_cast<void**>(&slots->u1.Function);
            }
        }
        return nullptr;
    }
    void WriteSlot(void** slot, void* value)
    {
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return;
        InterlockedExchangePointer(slot, value);
        VirtualProtect(slot, sizeof(void*), old, &old);
    }
    // the end of the startup walk: Start starts APE's change watcher right after it, and the watcher's first call is this
    // (APE's FileMonitor thread, the other caller, never runs during the walk: Start holds the main thread)
    BOOL WINAPI Hk_ReadDirectoryChangesW(HANDLE dir, LPVOID buf, DWORD len, BOOL sub, DWORD filter, LPDWORD got, LPOVERLAPPED ov, LPOVERLAPPED_COMPLETION_ROUTINE cr)
    {
        if (g_opened > 0 || g_pruned > 0) End("APE's change watcher started");
        return g_rdc(dir, buf, len, sub, filter, got, ov, cr);
    }
    bool PatchImports()
    {
        g_slotFF = ImportSlot("FindFirstFileExA"); g_slotFN = ImportSlot("FindNextFileA"); g_slotFC = ImportSlot("FindClose");
        g_slotRDC = ImportSlot("ReadDirectoryChangesW");
        if (!g_slotFF || !g_slotFN || !g_slotFC || !g_slotRDC) return false;
        g_ff = reinterpret_cast<FindFirstExFn>(*g_slotFF); g_fn = reinterpret_cast<FindNextFn>(*g_slotFN); g_fc = reinterpret_cast<FindCloseFn>(*g_slotFC);
        g_rdc = reinterpret_cast<ReadChangesFn>(*g_slotRDC);
        WriteSlot(g_slotFC, reinterpret_cast<void*>(&Hk_FindClose));
        WriteSlot(g_slotFN, reinterpret_cast<void*>(&Hk_FindNextFileA));
        WriteSlot(g_slotRDC, reinterpret_cast<void*>(&Hk_ReadDirectoryChangesW));
        WriteSlot(g_slotFF, reinterpret_cast<void*>(&Hk_FindFirstFileExA));
        return true;
    }
    void RestoreImports()
    {
        // the filters stay in the DLL, so a thread still inside one finishes normally
        if (g_slotFF) WriteSlot(g_slotFF, reinterpret_cast<void*>(g_ff));
        if (g_slotFN) WriteSlot(g_slotFN, reinterpret_cast<void*>(g_fn));
        if (g_slotFC) WriteSlot(g_slotFC, reinterpret_cast<void*>(g_fc));
        if (g_slotRDC) WriteSlot(g_slotRDC, reinterpret_cast<void*>(g_rdc));
    }

    // apeFileCache +0x20: QList<QByteArray> (Qt 5.3: d = { ref, alloc, begin, end, array[] }, each item a QByteArray d-pointer
    // { ref, size, alloc, offset @+16 }, chars at d + offset)
    bool ReadRoots(void* cache, std::vector<std::string>& out)
    {
        __try
        {
            char* d = *reinterpret_cast<char**>(static_cast<char*>(cache) + apeaddr::off_FileCache_Roots);
            int begin = *reinterpret_cast<int*>(d + 8), end = *reinterpret_cast<int*>(d + 12);
            if (begin < 0 || end < begin || end - begin > 256) return false;
            for (int i = begin; i < end; ++i)
            {
                char* ba = reinterpret_cast<char**>(d + 16)[i];
                int size = *reinterpret_cast<int*>(ba + 4);
                ptrdiff_t off = *reinterpret_cast<ptrdiff_t*>(ba + 16);
                if (size <= 0 || size > 1024) return false;
                out.emplace_back(ba + off, size_t(size));
            }
            return !out.empty();
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { out.clear(); return false; }
    }
    int EntryCount(void* cache)
    {
        __try
        {
            int* d = *reinterpret_cast<int**>(static_cast<char*>(cache) + apeaddr::off_FileCache_Entries);
            return d ? d[1] : 0;   // null before the exe's static initialisers ran
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }
    int VectorSize(void* vec)
    {
        __try { return vec ? reinterpret_cast<int*>(*static_cast<char**>(vec))[1] : -1; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }

    // the index: "game=", "root=" lines (must match this run), then one "dir=" line per folder that held a GDT
    bool LoadIndex()
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, IndexPath().c_str(), L"rb") != 0 || !f) return false;
        std::vector<std::string> roots, dirs; std::string game; char line[2048];
        while (fgets(line, sizeof line, f))
        {
            std::string s = line; while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (s.rfind("game=", 0) == 0) game = s.substr(5);
            else if (s.rfind("root=", 0) == 0) roots.push_back(s.substr(5));
            else if (s.rfind("dir=", 0) == 0) dirs.push_back(s.substr(4));
        }
        fclose(f);
        if (game != g_prefix || roots != g_roots || dirs.empty()) return false;
        for (const std::string& d : dirs)
        {
            const std::string* root = nullptr;
            for (const std::string& r : g_roots) if (d == r || (d.size() > r.size() && d.compare(0, r.size(), r) == 0 && d[r.size()] == '\\')) { root = &r; break; }
            if (!root) continue;
            g_children[d];
            // every folder between the root and this one lists the next one down
            std::string cur = d;
            while (cur.size() > root->size())
            {
                size_t sl = cur.find_last_of('\\');
                std::string parent = cur.substr(0, sl), name = cur.substr(sl + 1);
                g_children[parent].insert(name);
                cur = parent;
            }
        }
        return !g_children.empty();
    }
    bool SaveIndex(const std::vector<std::string>& dirs)
    {
        std::wstring path = IndexPath(), tmp = path + L".tmp";
        FILE* f = nullptr;
        if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) return false;
        fprintf(f, "# RadiantPlugins: the folders under APE's GDT roots that held a .gdt at the last full walk (src/ape_gdtscan.cpp).\r\n"
                   "# APE's startup walk opens only these and their parents; a background walk after startup rewrites this file.\r\n");
        fprintf(f, "game=%s\r\n", g_prefix.c_str());
        for (const std::string& r : g_roots) fprintf(f, "root=%s\r\n", r.c_str());
        for (const std::string& d : dirs) fprintf(f, "dir=%s\r\n", d.c_str());
        bool ok = fflush(f) == 0;
        fclose(f);
        if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp.c_str()); return false; }
        return true;
    }

    // the same walk as APE's, on our own threads: folders off a shared queue, subfolders not starting with '.', files *.gdt
    struct FullWalk
    {
        std::mutex m; std::deque<std::string> queue; int busy = 0; size_t folders = 0;
        std::unordered_map<std::string, Stamp> files; std::unordered_set<std::string> gdtDirs;

        void Worker()
        {
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);   // lowest I/O priority: APE's own reads go first
            WIN32_FIND_DATAA d;
            for (;;)
            {
                std::string rel;
                {
                    std::unique_lock<std::mutex> lock(m);
                    while (queue.empty())
                    {
                        if (busy == 0) return;
                        lock.unlock(); Sleep(1); lock.lock();
                    }
                    rel = std::move(queue.front()); queue.pop_front(); ++busy; ++folders;
                }
                std::vector<std::string> subs; std::vector<std::pair<std::string, Stamp>> found;
                HANDLE h = FindFirstFileExA((g_prefix + rel + "\\*.*").c_str(), FindExInfoBasic, &d, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
                if (h != INVALID_HANDLE_VALUE)
                {
                    do
                    {
                        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (d.cFileName[0] != '.') subs.push_back(rel + "\\" + d.cFileName); }
                        else if (IsGdt(d.cFileName)) { std::string n = d.cFileName; Lower(n); found.emplace_back(rel + "\\" + n, Stamp{ d.nFileSizeHigh, d.nFileSizeLow, d.ftLastWriteTime }); }
                    } while (FindNextFileA(h, &d));
                    FindClose(h);
                }
                std::lock_guard<std::mutex> lock(m);
                for (std::string& s : subs) queue.push_back(std::move(s));
                if (!found.empty()) gdtDirs.insert(rel);
                for (auto& fs : found) files[fs.first] = fs.second;
                --busy;
            }
        }
        void Run(int threads)
        {
            for (const std::string& r : g_roots) queue.push_back(r);
            busy = 0;
            std::vector<std::thread> pool;
            for (int i = 0; i < threads; ++i) pool.emplace_back([this] { __try { Worker(); } __except (EXCEPTION_EXECUTE_HANDLER) {} });
            for (std::thread& t : pool) t.join();
        }
    };

    void RefreshOnMain(int diffs)
    {
        mt::Post([diffs]
        {
            __try
            {
                *(static_cast<char*>(Cache()) + apeaddr::off_FileCache_Dirty) = 1;
                g_refresh(Cache());
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { LOG("ape: GDT check: APE's refresh faulted"); return; }
            LOG("ape: GDT check: APE refreshed its GDT list (%d differences)", diffs);
        });
    }

    void Check()
    {
        // after the window is up and the plugins are in, so the walk competes with nothing APE does at startup
        for (int i = 0; i < 2400 && !(mt::MainWindow() && plugins::Initialized()); ++i) Sleep(250);
        Sleep(2000);
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        auto t0 = std::chrono::steady_clock::now();
        FullWalk w;
        w.Run(4);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        int added = 0, gone = 0, changed = 0;
        {
            std::lock_guard<std::mutex> lock(g_seenMutex);
            for (auto& f : w.files) { auto it = g_seen.find(f.first); if (it == g_seen.end()) ++added; else if (!Same(it->second, f.second)) ++changed; }
            for (auto& s : g_seen) if (!w.files.count(s.first)) ++gone;
        }
        std::vector<std::string> dirs(w.gdtDirs.begin(), w.gdtDirs.end());
        std::sort(dirs.begin(), dirs.end());
        bool saved = SaveIndex(dirs);
        int diffs = added + gone + changed;
        LOG("ape: GDT check: %zu folders, %zu GDT files in %.0f ms (background); since APE's startup walk %d new, %d gone, %d changed; index %s (%zu folders)%s",
            w.folders, w.files.size(), ms, added, gone, changed, saved ? "written" : "NOT written", dirs.size(), diffs ? "; refreshing" : "");
        if (diffs && g_refresh) RefreshOnMain(diffs);
    }

    // first filtered call once apeFileCache has its roots (a pool thread of APE's walk): TA_GAME_PATH, the roots, the ini
    // switch, the index. An earlier call (other startup code, roots still empty) leaves it for the next one.
    void Init()
    {
        char game[1024] = {};
        if (!GetEnvironmentVariableA("TA_GAME_PATH", game, sizeof game)) { g_initDone = true; return; }
        if (EntryCount(Cache()) != 0) { g_initDone = true; return; }   // Start is over (a late injection): never filter a refresh
        std::vector<std::string> roots;
        if (!ReadRoots(Cache(), roots)) return;
        g_roots = std::move(roots);
        g_prefix = std::string(game) + "\\";
        g_fast = GetPrivateProfileIntW(L"ape", L"fast_gdt_scan", 1, (g_dllDir + L"\\RadiantPlugins.ini").c_str()) != 0;
        g_prune = g_fast && LoadIndex();
        g_initOk = true;
        g_initDone = true;   // last: a thread that sees it set reads the fields above without the lock
    }

    std::chrono::steady_clock::time_point g_armTime;
    void End(const char* why)
    {
        if (g_ended.exchange(true)) return;
        g_active = false;
        RestoreImports();
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_armTime).count();
        if (!g_initOk || (g_opened == 0 && g_pruned == 0))
        {
            LOG("ape: GDT startup walk not seen (%s); APE's walk ran unchanged", g_initOk ? why : "no TA_GAME_PATH or roots");
            return;
        }
        size_t files; { std::lock_guard<std::mutex> lock(g_seenMutex); files = g_seen.size(); }
        LOG("ape: GDT startup walk: %zu GDT files, %d folders opened, %d skipped, done %.0f ms after the host loaded (%s; %s)", files, g_opened.load(), g_pruned.load(), ms,
            g_prune ? "folder index" : g_fast ? "full walk, no usable index yet" : "full walk, [ape] fast_gdt_scan=0", why);
        SetEnvironmentVariableW(L"RADIANTPLUGINS_APE_GDT_CHECK", L"1");   // APEPlus: no commit-timer rescan needed right after startup
        HANDLE th = CreateThread(nullptr, 0, [](LPVOID) -> DWORD { __try { Check(); } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("ape: GDT check faulted"); } return 0; }, nullptr, 0, nullptr);
        if (th) CloseHandle(th);
    }

    bool StartsFunction(void* target)
    {
        DWORD64 base = 0; PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(DWORD64(target), &base, nullptr);
        return fn && DWORD64(fn->BeginAddress) + base == DWORD64(target);
    }
}

namespace ape
{
    // DllMain: nothing but checks of the exe in memory and four pointer writes (no MinHook, no log, no ini)
    void ArmGdtScan(void* self)
    {
        if (!app::IsApe()) return;
        auto* base = reinterpret_cast<BYTE*>(app::ModuleBase());
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
        if (nt->FileHeader.TimeDateStamp != 0x5DF44F21 || nt->OptionalHeader.SizeOfImage != 0x264A000) return;   // app::VerifyApeBinary's build
        if (EntryCount(Cache()) != 0) return;   // injected after APE's startup walk
        wchar_t path[MAX_PATH * 2] = {};
        GetModuleFileNameW(static_cast<HMODULE>(self), path, MAX_PATH * 2);
        g_dllDir = path; size_t sl = g_dllDir.find_last_of(L"\\/"); g_dllDir = sl == std::wstring::npos ? L"." : g_dllDir.substr(0, sl);
        g_armTime = std::chrono::steady_clock::now();
        g_active = true;
        if (!PatchImports()) { g_active = false; RestoreImports(); return; }
        g_armed = true;
    }

    void InstallGdtScan()
    {
        void* refresh = reinterpret_cast<void*>(app::ModuleBase() + (apeaddr::apeFileCache_Refresh - 0x140000000ULL));
        if (StartsFunction(refresh)) g_refresh = static_cast<RefreshFn>(refresh);
        if (!g_armed) LOG("ape: GDT startup walk filter not armed (imports not found)");
        else LOG("ape: GDT startup walk filter armed from DllMain (index %s)%s", GetPrivateProfileIntW(L"ape", L"fast_gdt_scan", 1, Cfg().iniPath.c_str()) ? "on" : "off",
                 g_refresh ? "" : "; APE's refresh not where expected, the check will only rewrite the index");
    }

    // ape::Start (main window up): the walk is long over, stop filtering if the watcher sentinel never came
    void EndGdtScan() { if (g_armed) End("main window up"); }
}
