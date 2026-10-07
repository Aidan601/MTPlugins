#include <windows.h>
#include <shlwapi.h>
#include "app.h"
#include "log.h"
#include "../sdk/modtools_plugin.h"

namespace
{
    constexpr uint32_t kApeTimestamp = 0x5DF44F21;
    constexpr uint32_t kApeSizeOfImage = 0x264A000;

    app::Kind Detect()
    {
        wchar_t path[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
        const wchar_t* name = PathFindFileNameW(path);
        if (_wcsicmp(name, L"Radiant_modtools.exe") == 0) return app::Kind::Radiant;
        if (_wcsicmp(name, L"asseteditor_modtools.exe") == 0) return app::Kind::Ape;
        if (_wcsicmp(name, L"modlauncher.exe") == 0) return app::Kind::Launcher;
        return app::Kind::Unknown;
    }
}

namespace app
{
    Kind Current() { static Kind k = Detect(); return k; }
    bool IsRadiant() { return Current() == Kind::Radiant; }
    bool IsApe() { return Current() == Kind::Ape; }
    bool IsLauncher() { return Current() == Kind::Launcher; }
    const char* Name() { return IsRadiant() ? "Radiant" : IsApe() ? "APE" : IsLauncher() ? "Launcher" : "unknown"; }
    int ApiId() { return IsRadiant() ? MODTOOLS_APP_RADIANT : IsApe() ? MODTOOLS_APP_APE : IsLauncher() ? MODTOOLS_APP_LAUNCHER : 0; }
    const wchar_t* LogFileName() { return IsLauncher() ? L"RadiantPlugins.launcher.log" : IsApe() ? L"RadiantPlugins.ape.log" : L"RadiantPlugins.log"; }
    const wchar_t* CtlFileName() { return IsLauncher() ? L"RadiantPlugins.launcher.ctl" : IsApe() ? L"RadiantPlugins.ape.ctl" : L"RadiantPlugins.ctl"; }
    const wchar_t* CtlWindowClass() { return IsLauncher() ? L"RadiantPlugins_LauncherControl" : IsApe() ? L"RadiantPlugins_ApeControl" : L"RadiantPlugins_Control"; }
    const wchar_t* MainWindowTitleHint() { return IsLauncher() ? L"Mod Tools Launcher" : IsApe() ? L"Asset Property Editor" : L"Radiant"; }
    uintptr_t ModuleBase() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

    bool VerifyApeBinary()
    {
        static int cached = -1;
        if (cached >= 0) return cached != 0;
        if (!IsApe()) { cached = 0; return false; }
        auto base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        uint32_t ts = nt->FileHeader.TimeDateStamp;
        uint32_t size = nt->OptionalHeader.SizeOfImage;
        bool ok = ts == kApeTimestamp && size == kApeSizeOfImage;
        LOG("app: APE exe base=0x%p timestamp=0x%08X sizeOfImage=0x%X -> %s", base, ts, size, ok ? "MATCH" : "MISMATCH");
        cached = ok ? 1 : 0;
        return ok;
    }
}
