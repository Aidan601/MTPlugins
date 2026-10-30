// gdt.db reload without the main-thread freeze. Radiant only.
#include "modtools_plugin.hpp"
#include "gdtreload.h"
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace br::gdtreload
{
namespace
{
    // Radiant (Jul 25 2024, no ASLR); checked by the host's binary match.
    constexpr uintptr_t kDestroyEntry  = 0x140E52B50;   // gdt_db entry destructor: closes the old copies, called with the old entry after a reload
    constexpr uintptr_t kQtDbCheck     = 0x1409FB000;   // asset browsers' 100 ms check: closes their connection when the snapshot name changed
    constexpr uintptr_t kQtDbName      = 0x14333BEE0;   // std::string: the URI the asset browsers have open (the live copy)
    constexpr uintptr_t kMonitorThread = 0x143387B70;   // HANDLE of the gdt.db monitor thread (fires the reload on a write-time change)
    constexpr uintptr_t kStartEvent    = 0x143387B38;   // HANDLE: the loader thread's start event
    constexpr uintptr_t kDbPath        = 0x143387B98;   // std::string: the gdt.db path

    // gdt_db entry: +0 snapshot db, +8 snapshot URI, +40 live URI, +72 live db
    constexpr size_t off_Entry_SnapUri = 8;
    constexpr size_t off_Entry_LiveUri = 40;

    constexpr int kSqliteOpenReadOnly   = 0x1;
    constexpr int kSqliteOpenUri        = 0x40;
    constexpr int kSqliteOpenSharedCache = 0x20000;

    using DestroyEntryFn = void (__fastcall*)(void* entry);
    using QtDbCheckFn    = bool (__fastcall*)(void* obj);
    using GetAttrExAFn   = BOOL (WINAPI*)(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
    using SqliteOpenFn   = int (*)(const char*, void**, int, const char*);
    using SqliteCloseFn  = int (*)(void*);
    using SqliteStepFn   = int (*)(void*);

    DestroyEntryFn g_oDestroyEntry = nullptr;
    QtDbCheckFn    g_oQtDbCheck = nullptr;
    GetAttrExAFn   g_oGetAttrExA = nullptr;
    SqliteOpenFn   g_sqliteOpen = nullptr;
    SqliteCloseFn  g_sqliteClose = nullptr;
    SqliteStepFn   g_oSqliteStep = nullptr;

    bool  g_offload = true;         // [gdtdb] offload
    int   g_quietMs = 2000;         // [gdtdb] quiet_ms
    bool  g_hooksOk = false;
    bool  g_monitorHooked = false;
    DWORD g_monitorTid = 0;
    DWORD g_mainTid = 0;
    volatile uint64_t g_lastMainStepMs = 0;   // the main thread's last sqlite3_step
    constexpr uint64_t kMainIdleMs = 1500;     // free only after the main thread stayed off SQLite this long
    constexpr uint64_t kIdleWaitMaxMs = 60000; // then free anyway
    bool g_stepHooked = false;
    volatile LONG g_stopping = 0;
    volatile LONG g_inAttrHook = 0;

    // one old copy pair handed to the worker: both held until the asset browsers moved to the new entry
    struct Pending
    {
        std::string snapUri, liveUri;
        void* snap = nullptr;       // our connection to the old snapshot
        void* live = nullptr;       // our connection to the old live copy (the one the asset browsers query)
        bool released = false;      // the asset browsers closed their connection to this entry
        uint64_t sinceMs = 0;
        uint64_t releasedMs = 0;    // when they did (their refresh queries follow right after)
    };
    CRITICAL_SECTION g_cs;
    std::vector<Pending> g_pending;
    HANDLE g_wake = nullptr;
    HANDLE g_stop = nullptr;
    HANDLE g_thread = nullptr;
    constexpr uint64_t kReleaseTimeoutMs = 30000;   // close anyway after this (harmless while the browsers still hold a copy)

    // stats for the command reply
    unsigned g_reloads = 0, g_workerFrees = 0, g_coalesced = 0;
    double g_mainMs = 0, g_lastMainMs = 0, g_workerMs = 0, g_lastWorkerMs = 0;

    double NowMs()
    {
        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        return 1000.0 * static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
    }

    // MSVC std::string: buffer or pointer at +0, size +16, capacity +24 (pointer when capacity >= 16)
    std::string StdString(const void* s)
    {
        const char* base = static_cast<const char*>(s);
        const size_t size = *reinterpret_cast<const size_t*>(base + 16);
        const size_t cap = *reinterpret_cast<const size_t*>(base + 24);
        const char* p = cap >= 16 ? *reinterpret_cast<const char* const*>(base) : base;
        return std::string(p, size);
    }

    void* OpenKeepAlive(const std::string& uri)
    {
        void* db = nullptr;
        if (g_sqliteOpen(uri.c_str(), &db, kSqliteOpenReadOnly | kSqliteOpenUri | kSqliteOpenSharedCache, nullptr) != 0)
        {
            if (db) g_sqliteClose(db);
            return nullptr;
        }
        return db;
    }

    // The old entry: hold both copies open, let Radiant close its own connections (cheap now), free them on the worker.
    void __fastcall hk_DestroyEntry(void* entry)
    {
        if (!g_offload || g_stopping || !g_sqliteOpen) { g_oDestroyEntry(entry); return; }
        Pending p;
        p.snapUri = StdString(static_cast<char*>(entry) + off_Entry_SnapUri);
        p.liveUri = StdString(static_cast<char*>(entry) + off_Entry_LiveUri);
        p.snap = p.snapUri.empty() ? nullptr : OpenKeepAlive(p.snapUri);
        p.live = p.liveUri.empty() ? nullptr : OpenKeepAlive(p.liveUri);
        if (!p.snap || !p.live)
        {
            if (p.snap) g_sqliteClose(p.snap);
            if (p.live) g_sqliteClose(p.live);
            rp::Log("gdtreload: could not hold the old copies open (%s, %s), Radiant frees them itself", p.snapUri.c_str(), p.liveUri.c_str());
            g_oDestroyEntry(entry);
            return;
        }
        const double t0 = NowMs();
        g_oDestroyEntry(entry);
        const double ms = NowMs() - t0;
        p.sinceMs = GetTickCount64();
        EnterCriticalSection(&g_cs);
        g_pending.push_back(p);
        ++g_reloads;
        g_mainMs += ms;
        g_lastMainMs = ms;
        LeaveCriticalSection(&g_cs);
        rp::Log("gdtreload: reload #%u, Radiant let go of the old copies in %.1f ms on the main thread, %s and %s go to the worker",
                g_reloads, ms, p.snapUri.c_str(), p.liveUri.c_str());
        SetEvent(g_wake);
    }

    // After the asset browsers closed their connection to an old entry, the worker may free its copies.
    bool __fastcall hk_QtDbCheck(void* obj)
    {
        const bool open = g_oQtDbCheck(obj);
        if (open || g_stopping) return open;
        const std::string name = StdString(reinterpret_cast<void*>(kQtDbName));
        if (name.empty()) return open;
        bool wake = false;
        EnterCriticalSection(&g_cs);
        for (Pending& p : g_pending)
            if (!p.released && (p.liveUri == name || p.snapUri == name)) { p.released = true; p.releasedMs = GetTickCount64(); wake = true; }
        LeaveCriticalSection(&g_cs);
        if (wake) SetEvent(g_wake);
        return open;
    }

    // The monitor thread's look at gdt.db: answer once the file has been quiet for [gdtdb] quiet_ms.
    BOOL WINAPI hk_GetFileAttributesExA(LPCSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info)
    {
        if (g_quietMs <= 0 || g_stopping || !g_monitorTid || GetCurrentThreadId() != g_monitorTid || level != GetFileExInfoStandard || !name)
            return g_oGetAttrExA(name, level, info);
        const std::string path = StdString(reinterpret_cast<void*>(kDbPath));
        if (path.empty() || _stricmp(path.c_str(), name) != 0) return g_oGetAttrExA(name, level, info);
        InterlockedIncrement(&g_inAttrHook);
        WIN32_FILE_ATTRIBUTE_DATA last = {};
        BOOL ok = g_oGetAttrExA(name, level, &last);
        const DWORD step = static_cast<DWORD>(g_quietMs < 1000 ? 250 : 500);
        uint64_t quietSince = GetTickCount64();
        const uint64_t deadline = quietSince + 120000;
        unsigned changes = 0;
        while (ok && !g_stopping)
        {
            const uint64_t now = GetTickCount64();
            if (now - quietSince >= static_cast<uint64_t>(g_quietMs) || now >= deadline) break;
            Sleep(step);
            WIN32_FILE_ATTRIBUTE_DATA cur = {};
            if (!g_oGetAttrExA(name, level, &cur)) break;
            if (memcmp(&cur.ftLastWriteTime, &last.ftLastWriteTime, sizeof cur.ftLastWriteTime) != 0
                || cur.nFileSizeLow != last.nFileSizeLow || cur.nFileSizeHigh != last.nFileSizeHigh)
            {
                last = cur;
                quietSince = GetTickCount64();
                ++changes;
            }
        }
        if (changes)
        {
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_coalesced));
            rp::Log("gdtreload: gdt.db changed %u more time(s) while the monitor waited, one reload instead of %u", changes, changes + 1);
        }
        if (ok) memcpy(info, &last, sizeof last);
        InterlockedDecrement(&g_inAttrHook);
        return ok;
    }

    // The main thread's SQLite use: the worker's frees hold SQLite's page cache mutex, so they wait for a quiet main thread.
    int hk_SqliteStep(void* stmt)
    {
        if (GetCurrentThreadId() == g_mainTid) g_lastMainStepMs = GetTickCount64();
        return g_oSqliteStep(stmt);
    }

    // True once the main thread has not touched SQLite for kMainIdleMs, counted from floorMs at the earliest
    // (or the wait ran out, or we are stopping).
    bool WaitMainIdle(uint64_t startMs, uint64_t floorMs)
    {
        for (;;)
        {
            const uint64_t now = GetTickCount64();
            uint64_t last = g_lastMainStepMs;
            if (floorMs > last) last = floorMs;
            if (g_stopping || !g_stepHooked || now - startMs >= kIdleWaitMaxMs) return false;
            if (now - last >= kMainIdleMs) return true;
            if (WaitForSingleObject(g_stop, 100) == WAIT_OBJECT_0) return false;
        }
    }

    // Frees the old copies once the browsers released the entry (or after the timeout), while the main thread is off SQLite.
    DWORD WINAPI Worker(LPVOID)
    {
        HANDLE waits[2] = { g_stop, g_wake };
        for (;;)
        {
            const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 1000);
            const bool stopping = w == WAIT_OBJECT_0 || g_stopping;
            std::vector<Pending> todo;
            EnterCriticalSection(&g_cs);
            const uint64_t now = GetTickCount64();
            for (size_t i = 0; i < g_pending.size();)
            {
                Pending& p = g_pending[i];
                if (p.released || stopping || now - p.sinceMs >= kReleaseTimeoutMs)
                {
                    todo.push_back(p);
                    g_pending.erase(g_pending.begin() + i);
                }
                else ++i;
            }
            LeaveCriticalSection(&g_cs);
            for (Pending& t : todo)
            {
                const uint64_t waitStart = GetTickCount64();
                const bool idle1 = WaitMainIdle(waitStart, t.releasedMs);
                const double t0 = NowMs();
                if (t.live) g_sqliteClose(t.live);
                const double t1 = NowMs();
                const bool idle2 = WaitMainIdle(waitStart, t.releasedMs);
                const double t2a = NowMs();
                if (t.snap) g_sqliteClose(t.snap);
                const double t2 = NowMs();
                const double waited = static_cast<double>(GetTickCount64() - waitStart) - (t1 - t0) - (t2 - t2a);
                EnterCriticalSection(&g_cs);
                ++g_workerFrees;
                g_lastWorkerMs = (t1 - t0) + (t2 - t2a);
                g_workerMs += g_lastWorkerMs;
                LeaveCriticalSection(&g_cs);
                rp::Log("gdtreload: worker freed %s in %.0f ms and %s in %.0f ms after waiting %.0f ms for a quiet main thread%s%s",
                        t.liveUri.c_str(), t1 - t0, t.snapUri.c_str(), t2 - t2a, waited,
                        idle1 && idle2 ? "" : " (gave up waiting)",
                        t.released ? "" : " (browsers had not let go, Radiant frees that one when they do)");
            }
            if (stopping)
            {
                EnterCriticalSection(&g_cs);
                const bool empty = g_pending.empty();
                LeaveCriticalSection(&g_cs);
                if (empty) return 0;
            }
        }
    }

    template <class T> T At(uintptr_t a) { return reinterpret_cast<T>(a); }
}

void Setup()
{
    g_offload = rp::host->ConfigGetInt(rp::self, "gdtdb", "offload", 1) != 0;
    g_quietMs = rp::host->ConfigGetInt(rp::self, "gdtdb", "quiet_ms", 2000);
    InitializeCriticalSection(&g_cs);
    HMODULE sq = GetModuleHandleW(L"sqlite64r.dll");
    if (sq)
    {
        g_sqliteOpen = reinterpret_cast<SqliteOpenFn>(GetProcAddress(sq, "sqlite3_open_v2"));
        g_sqliteClose = reinterpret_cast<SqliteCloseFn>(GetProcAddress(sq, "sqlite3_close_v2"));
    }
    HANDLE monitor = *At<HANDLE*>(kMonitorThread);
    g_monitorTid = monitor ? GetThreadId(monitor) : 0;
    g_mainTid = GetCurrentThreadId();
    g_lastMainStepMs = GetTickCount64();
    if (!g_sqliteOpen || !g_sqliteClose) { rp::Log("gdtreload: sqlite64r.dll not resolved, off"); return; }
    void* step = sq ? reinterpret_cast<void*>(GetProcAddress(sq, "sqlite3_step")) : nullptr;
    g_stepHooked = step && rp::host->Detour(rp::self, step, reinterpret_cast<void*>(&hk_SqliteStep), reinterpret_cast<void**>(&g_oSqliteStep)) != 0;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (!g_wake || !g_stop || !g_thread) { rp::Log("gdtreload: worker not started, off"); return; }
    bool a = rp::host->Detour(rp::self, At<void*>(kDestroyEntry), reinterpret_cast<void*>(&hk_DestroyEntry), reinterpret_cast<void**>(&g_oDestroyEntry)) != 0;
    bool b = a && rp::host->Detour(rp::self, At<void*>(kQtDbCheck), reinterpret_cast<void*>(&hk_QtDbCheck), reinterpret_cast<void**>(&g_oQtDbCheck)) != 0;
    g_hooksOk = a && b;
    void* attr = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetFileAttributesExA"));
    g_monitorHooked = attr && g_monitorTid
        && rp::host->Detour(rp::self, attr, reinterpret_cast<void*>(&hk_GetFileAttributesExA), reinterpret_cast<void**>(&g_oGetAttrExA)) != 0;
    rp::Log("gdtreload: old copies freed on a worker: %s (main-thread SQLite use %s); one reload per quiet file: %s (quiet_ms=%d, monitor thread %u)",
            !g_hooksOk ? "unavailable (detours failed)" : g_offload ? "on" : "off ([gdtdb] offload=0)", g_stepHooked ? "watched" : "not watched",
            !g_monitorHooked ? "unavailable" : g_quietMs > 0 ? "on" : "off ([gdtdb] quiet_ms=0)", g_quietMs, g_monitorTid);
}

void Shutdown()
{
    InterlockedExchange(&g_stopping, 1);
    // a monitor thread inside the attribute wait leaves it within one step
    for (int i = 0; i < 40 && g_inAttrHook; ++i) Sleep(50);
    if (g_thread)
    {
        SetEvent(g_stop);
        if (WaitForSingleObject(g_thread, 60000) != WAIT_OBJECT_0) rp::Log("gdtreload: worker still busy at unload");
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_wake) { CloseHandle(g_wake); g_wake = nullptr; }
    if (g_stop) { CloseHandle(g_stop); g_stop = nullptr; }
    DeleteCriticalSection(&g_cs);
}

bool Command(const std::string& command, std::string& reply)
{
    if (command.compare(0, 9, "gdtreload") != 0) return false;
    const std::string arg = command.size() > 10 ? command.substr(10) : std::string();
    if (arg == "on" || arg == "off")
    {
        g_offload = arg == "on";
        rp::host->ConfigSet(rp::self, "gdtdb", "offload", g_offload ? "1" : "0");
    }
    else if (arg.compare(0, 6, "quiet ") == 0)
    {
        g_quietMs = atoi(arg.c_str() + 6);
        rp::host->ConfigSet(rp::self, "gdtdb", "quiet_ms", std::to_string(g_quietMs).c_str());
    }
    else if (arg == "fire")      // test: ask Radiant's loader for a reload without touching gdt.db
    {
        HANDLE ev = *At<HANDLE*>(kStartEvent);
        reply = ev && SetEvent(ev) ? "reload requested\n" : "no loader start event\n";
        return true;
    }
    EnterCriticalSection(&g_cs);
    char buf[768];
    snprintf(buf, sizeof buf,
             "gdtreload: offload %s (%s), quiet_ms %d (%s); %u reloads, main thread %.1f ms total (last %.1f), worker frees %u in %.0f ms total (last %.0f), %u coalesced, %u pending\n"
             "  gdtreload on|off, gdtreload quiet <ms>, gdtreload fire\n",
             g_offload ? "on" : "off", g_hooksOk ? "hooked" : "unavailable", g_quietMs, g_monitorHooked ? "hooked" : "unavailable",
             g_reloads, g_mainMs, g_lastMainMs, g_workerFrees, g_workerMs, g_lastWorkerMs, g_coalesced, static_cast<unsigned>(g_pending.size()));
    LeaveCriticalSection(&g_cs);
    reply = buf;
    return true;
}
}
