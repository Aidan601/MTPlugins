#include <windows.h>
#include <deque>
#include <mutex>
#include "mainthread.h"
#include "app.h"
#include "log.h"

namespace
{
    struct Job { const void* tag; std::function<void()> fn; };

    HHOOK                              g_hook = nullptr;
    HWND                               g_mainWnd = nullptr;
    DWORD                              g_mainTid = 0;
    std::mutex                         g_mutex;
    std::deque<Job>                    g_jobs;
    bool                               g_draining = false;
    const UINT                         kWakeMsg = WM_NULL;

    struct FindCtx { DWORD pid; HWND best; int bestScore; };

    BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp)
    {
        FindCtx* ctx = reinterpret_cast<FindCtx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != ctx->pid) return TRUE;
        if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;      // owned popups/dialogs
        if (!IsWindowVisible(hwnd)) return TRUE;

        wchar_t cls[128] = {}, title[256] = {};
        GetClassNameW(hwnd, cls, 128);
        GetWindowTextW(hwnd, title, 256);
        int score = 0;
        if (wcsstr(cls, L"Qt5QWindow")) score += 2;
        if (wcsstr(cls, L"ToolSaveBits") || wcsstr(cls, L"Splash")) score -= 10;
        if (title[0]) score += 1;
        if (wcsstr(title, app::MainWindowTitleHint())) score += 4;
        RECT r; GetWindowRect(hwnd, &r);
        int w = r.right - r.left, h = r.bottom - r.top;
        if (w > 600 && h > 400) score += 2;
        if (w > 1000 && h > 700) score += 6;         // the editor frame is big, the splash is ~656x519
        if (wcsstr(title, L".map")) score += 3;        // after a load the title is the map path
        if (score > ctx->bestScore) { ctx->bestScore = score; ctx->best = hwnd; }
        return TRUE;
    }

    // SEH wrapper, no C++ objects with destructors (C2712)
    void RunJobSeh(std::function<void()>* job)
    {
        __try { (*job)(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { LOG("mt: job raised exception 0x%08X", GetExceptionCode()); }
    }

    void Drain()
    {
        if (g_draining) return;          // a job pumped messages, don't nest
        g_draining = true;
        for (;;)
        {
            std::function<void()> job;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_jobs.empty()) break;
                job = std::move(g_jobs.front().fn);
                g_jobs.pop_front();
            }
            RunJobSeh(&job);
        }
        g_draining = false;
    }

    bool HasPending()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return !g_jobs.empty();
    }

    LRESULT CALLBACK GetMsgProc(int code, WPARAM wParam, LPARAM lParam)
    {
        static bool alive = false;
        if (!alive) { alive = true; LOG("mt: GetMsgProc alive on tid %lu", GetCurrentThreadId()); }
        if (code == HC_ACTION && HasPending()) Drain();   // any pump activity on the main thread will do
        return CallNextHookEx(g_hook, code, wParam, lParam);
    }

    // Second trigger: a window timer (works even if the pump only peeks).
    const UINT_PTR kTimerId = 0x7C01;
    void CALLBACK TimerProc(HWND hwnd, UINT, UINT_PTR id, DWORD)
    {
        KillTimer(hwnd, id);
        if (HasPending()) Drain();
    }

    // Repeating tick timer (APE)
    const UINT_PTR kTickTimerId = 0x7C02;
    void (*g_tickFn)() = nullptr;
    unsigned g_tickMs = 0;
    HWND g_tickWnd = nullptr;
    void CALLBACK TickProc(HWND, UINT, UINT_PTR, DWORD)
    {
        static bool alive = false;
        if (!alive) { alive = true; LOG("mt: timer tick alive on tid %lu", GetCurrentThreadId()); }
        if (g_tickFn) g_tickFn();
    }

    HWND FindMainWindow(int* score = nullptr)
    {
        FindCtx ctx{ GetCurrentProcessId(), nullptr, 0 };
        EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
        if (score) *score = ctx.bestScore;
        return ctx.bestScore >= 3 ? ctx.best : nullptr;
    }
}

namespace mt
{
    bool Init()
    {
        if (g_hook) return true;
        int score = 0;
        HWND best = FindMainWindow(&score);
        if (!best) return false;

        g_mainWnd = best;
        g_mainTid = GetWindowThreadProcessId(g_mainWnd, nullptr);
        g_hook = SetWindowsHookExW(WH_GETMESSAGE, GetMsgProc, nullptr, g_mainTid);
        wchar_t title[256] = {}; GetWindowTextW(g_mainWnd, title, 256);
        LOGW(L"mt: main window 0x%p (tid %lu) '%s' hook=%s", g_mainWnd, g_mainTid, title, g_hook ? L"ok" : L"FAILED");
        return g_hook != nullptr;
    }

    void Shutdown()
    {
        if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
        if (g_tickWnd) { KillTimer(g_tickWnd, kTickTimerId); g_tickWnd = nullptr; g_tickFn = nullptr; }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_jobs.clear();
    }

    void Post(std::function<void()> job) { PostTagged(nullptr, std::move(job)); }

    void PostTagged(const void* tag, std::function<void()> job)
    {
        { std::lock_guard<std::mutex> lock(g_mutex); g_jobs.push_back(Job{ tag, std::move(job) }); }
        if (g_mainTid)
        {
            PostThreadMessageW(g_mainTid, kWakeMsg, 0, 0);
            if (g_mainWnd) SetTimer(g_mainWnd, kTimerId, 1, TimerProc);
        }
    }

    void DiscardTag(const void* tag)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto it = g_jobs.begin(); it != g_jobs.end();) { if (it->tag == tag) it = g_jobs.erase(it); else ++it; }
    }

    void StartTimerTick(void (*fn)(), unsigned ms)
    {
        g_tickFn = fn; g_tickMs = ms;
        HWND h = MainWindow();
        if (!h) return;
        g_tickWnd = h;
        SetTimer(h, kTickTimerId, ms, TickProc);
        LOG("mt: timer tick armed on 0x%p every %u ms", h, ms);
    }

    void  DrainNow()     { if (HasPending()) Drain(); }
    bool  IsMainThread() { return GetCurrentThreadId() == g_mainTid; }
    HWND  MainWindow()
    {
        // The splash can win the first search: re-resolve when the window is gone/hidden or a better one exists
        HWND best = FindMainWindow();
        if (best && best != g_mainWnd && GetWindowThreadProcessId(best, nullptr) == g_mainTid)
        {
            LOG("mt: main window re-resolved 0x%p -> 0x%p", g_mainWnd, best);
            g_mainWnd = best;
            if (g_tickFn && g_tickWnd != best) { g_tickWnd = best; SetTimer(best, kTickTimerId, g_tickMs, TickProc); }
        }
        return g_mainWnd;
    }
    DWORD MainThreadId() { return g_mainTid; }
}
