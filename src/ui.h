#pragma once
#include <string>

// Input plumbing for every plugin: the hotkey registry (WH_KEYBOARD hook plus idle-tick poll) and the WM_COPYDATA
// command channel used by test scripts. Main thread only.
namespace ui
{
    void Tick();       // main thread: installs the hooks lazily, polls hotkeys
    // A hotkey: virtual key + MODTOOLS_MOD_* bits. `tag` groups registrations (a plugin) so they can be dropped together.
    int  RegisterHotkey(const void* tag, int vk, int modifiers, void (*fn)(void*), void* user);
    void UnregisterTag(const void* tag);
    void Shutdown();
}
