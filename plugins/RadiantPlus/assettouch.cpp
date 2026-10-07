// Deferred asset-cache touch, see assettouch.h. Radiant's own touch (0x140FA0300, after the read):
// CreateFileA(path, 0x120116 FILE_GENERIC_WRITE, share 0, OPEN_EXISTING), SetFileTime(h, NULL, &now, NULL), close.
#include "modtools_plugin.hpp"
#include "assettouch.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace br::assettouch
{
namespace
{
    SRWLOCK g_lock = SRWLOCK_INIT;
    std::vector<std::pair<std::string, FILETIME>> g_queue;
    std::vector<std::pair<std::string, FILETIME>> g_work;   // owned by the worker while it runs
    std::atomic<bool> g_running{false};
    std::atomic<bool> g_stop{false};
    std::atomic<long> g_done{0}, g_failed{0}, g_batches{0};
    std::atomic<unsigned long long> g_lastMs{0};
    std::string g_firstFail;                                 // worker only, read after it ends

    DWORD WINAPI Worker(void*)
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&Worker), &self);
        const ULONGLONG t0 = GetTickCount64();
        for (const auto& item : g_work)
        {
            if (g_stop.load(std::memory_order_relaxed)) break;
            const HANDLE h = CreateFileA(item.first.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                const DWORD error = GetLastError();
                if (g_firstFail.empty()) g_firstFail = item.first + " (error " + std::to_string(error) + ")";
                g_failed.fetch_add(1);
                continue;
            }
            if (SetFileTime(h, nullptr, &item.second, nullptr)) g_done.fetch_add(1); else g_failed.fetch_add(1);
            CloseHandle(h);
        }
        g_lastMs = GetTickCount64() - t0;
        g_running = false;
        if (self) FreeLibraryAndExitThread(self, 0);
        return 0;
    }
}

void Queue(const std::string& path)
{
    if (path.empty()) return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    AcquireSRWLockExclusive(&g_lock);
    g_queue.emplace_back(path, now);
    ReleaseSRWLockExclusive(&g_lock);
}

void Flush()
{
    if (g_running || g_stop) return;
    AcquireSRWLockExclusive(&g_lock);
    if (g_queue.empty()) { ReleaseSRWLockExclusive(&g_lock); return; }
    g_work.swap(g_queue);
    g_queue.clear();
    ReleaseSRWLockExclusive(&g_lock);
    g_running = true;
    g_batches.fetch_add(1);
    const HANDLE t = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
    if (t) { SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t); }
    else { g_running = false; rp::Log("assettouch: worker thread failed, %zu cache timestamps not set", g_work.size()); g_work.clear(); }
}

void Shutdown()
{
    for (int i = 0; i < 300 && g_running.load(); ++i) Sleep(10);   // let a batch finish (about 1 ms per file)
    g_stop = true;
    for (int i = 0; i < 200 && g_running.load(); ++i) Sleep(10);
    if (g_running) rp::Log("assettouch: worker still running at unload (it holds the DLL until it ends)");
}

std::string Status()
{
    AcquireSRWLockShared(&g_lock);
    const size_t queued = g_queue.size();
    ReleaseSRWLockShared(&g_lock);
    char line[200];
    snprintf(line, sizeof line, "deferred cache touch: %ld set, %ld failed, %zu queued, %ld batches, last batch %llu ms%s",
             g_done.load(), g_failed.load(), queued, g_batches.load(), g_lastMs.load(), g_running ? " (running)" : "");
    return std::string(line) + (!g_running && !g_firstFail.empty() ? "; first failure " + g_firstFail : "");
}
}
