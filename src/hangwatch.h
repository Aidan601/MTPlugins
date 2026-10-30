#pragma once
#include <windows.h>

// Freeze watch: a background thread pings a window on the main thread and, when a ping waits
// [diag] freeze_ms, samples the main thread's stack and logs where it was.
namespace hangwatch
{
    void Start();                                     // init thread, after mt::Init found the main window
    void NameFunction(const void* start, const char* name);   // label a Radiant function in the stacks (static string)
}
