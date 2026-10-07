#pragma once
#include <cstdint>

// Which mod-tools editor the host was loaded into, decided once from the executable name. Radiant-specific code
// (hooks, addresses, overlay, Treyarch's docks) is gated on app::IsRadiant(), APE gets stock Qt docks and a timer tick.
namespace app
{
    enum class Kind { Unknown, Radiant, Ape, Launcher };

    Kind        Current();
    bool        IsRadiant();
    bool        IsApe();
    bool        IsLauncher();
    const char* Name();                 // "Radiant", "APE" or "unknown"
    int         ApiId();                // MODTOOLS_APP_RADIANT / MODTOOLS_APP_APE / 0 (the value plugins see)
    const wchar_t* LogFileName();       // RadiantPlugins.log / RadiantPlugins.ape.log (both editors may run at once)
    const wchar_t* CtlFileName();       // command-channel reply file next to the DLL
    const wchar_t* CtlWindowClass();    // command-channel window class (RadiantPlugins_Control / RadiantPlugins_ApeControl)
    const wchar_t* MainWindowTitleHint(); // substring that identifies the editor's main window

    // APE build the offsets belong to: TimeDateStamp 0x5DF44F21, SizeOfImage 0x264A000.
    bool VerifyApeBinary();
    uintptr_t ModuleBase();
}
