#pragma once
#include <functional>

// Runs work on the editor's main thread. Jobs are queued from any thread and drained from the
// RoutineProcessing detour, a WH_GETMESSAGE hook and a window timer. APE has no idle function, so
// StartTimerTick drives the drain and the host tick. Anything touching editor state must run through here.
namespace mt
{
    bool  Init();                         // finds the main window, installs the hook (retries while Radiant boots)
    void  Shutdown();
    void  Post(std::function<void()> job); // enqueue + wake the pump
    void  PostTagged(const void* tag, std::function<void()> job);   // same, with an owner tag so its jobs can be discarded
    void  DiscardTag(const void* tag);    // drop queued jobs of that owner (plugin unload)
    void  DrainNow();                     // run queued jobs, ONLY call from the main thread (RoutineProcessing hook)
    // Repeating main-window timer running `fn` every `ms` (APE's replacement for the RoutineProcessing detour).
    void  StartTimerTick(void (*fn)(), unsigned ms);
    bool  IsMainThread();
    HWND  MainWindow();
    DWORD MainThreadId();
}
