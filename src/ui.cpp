#include <windows.h>
#include <string>
#include <vector>
#include "ui.h"
#include "plugins.h"
#include "mainthread.h"
#include "config.h"
#include "app.h"
#include "log.h"

namespace
{
    struct Hotkey { int id; const void* tag; int vk; int mods; void (*fn)(void*); void* user; bool down; ULONGLONG lastFire; };
    std::vector<Hotkey> g_hotkeys;
    int   g_nextId = 1;
    HHOOK g_kbHook = nullptr;
    HWND  g_ctl = nullptr;           // message-only window: WM_COPYDATA command channel for unattended tests

    int ModsNow()
    {
        int m = 0;
        if (GetKeyState(VK_CONTROL) & 0x8000) m |= 1;
        if (GetKeyState(VK_SHIFT) & 0x8000) m |= 2;
        if (GetKeyState(VK_MENU) & 0x8000) m |= 4;
        return m;
    }

    bool Fire(int vk, int mods)
    {
        bool any = false;
        ULONGLONG now = GetTickCount64();
        for (Hotkey& h : g_hotkeys) if (h.vk == vk && h.mods == mods) h.lastFire = now;
        std::vector<Hotkey> snapshot = g_hotkeys;      // a handler may register/unregister
        for (const Hotkey& h : snapshot)
            if (h.vk == vk && h.mods == mods && h.fn) { h.fn(h.user); any = true; }
        return any;
    }

    // WH_KEYBOARD hook on Radiant's main thread: runs when its message pump retrieves a key message,
    // so a short tap is never missed (the idle-tick poll below is the fallback).
    LRESULT CALLBACK KeyboardProc(int code, WPARAM wp, LPARAM lp)
    {
        if (code == HC_ACTION && !(lp & 0x80000000) && !(lp & 0x40000000))
        {
            if (Fire(int(wp), ModsNow())) return 1;
        }
        return CallNextHookEx(g_kbHook, code, wp, lp);
    }

    // Test channel: a script sends a command string with WM_COPYDATA, the reply is written to
    // RadiantPlugins.ctl next to the host DLL (and logged), because the script cannot read Qt widgets.
    LRESULT CALLBACK CtlProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_COPYDATA)
        {
            auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
            std::string cmd(static_cast<const char*>(cds->lpData), cds->cbData);
            std::string out = plugins::Command(cmd);
            std::wstring path = Cfg().dllDir + L"\\" + app::CtlFileName();
            HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) { DWORD w = 0; WriteFile(f, out.data(), DWORD(out.size()), &w, nullptr); CloseHandle(f); }
            LOG("ctl: '%s' -> %s", cmd.c_str(), out.c_str());
            return 1;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    void CreateControlWindow()
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = CtlProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = app::CtlWindowClass();
        RegisterClassW(&wc);
        g_ctl = CreateWindowExW(0, app::CtlWindowClass(), L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        LOGW(L"ui: control window 0x%p (%s)", g_ctl, app::CtlWindowClass());
    }
}

namespace ui
{
    int RegisterHotkey(const void* tag, int vk, int modifiers, void (*fn)(void*), void* user)
    {
        Hotkey h{ g_nextId++, tag, vk, modifiers, fn, user, false, 0 };
        g_hotkeys.push_back(h);
        return h.id;
    }

    void UnregisterTag(const void* tag)
    {
        for (auto it = g_hotkeys.begin(); it != g_hotkeys.end();) { if (it->tag == tag) it = g_hotkeys.erase(it); else ++it; }
    }

    void Tick()
    {
        if (!g_kbHook)
        {
            g_kbHook = SetWindowsHookExW(WH_KEYBOARD, KeyboardProc, nullptr, GetCurrentThreadId());
            LOG("ui: keyboard hook on thread %lu -> %s", GetCurrentThreadId(), g_kbHook ? "ok" : "FAILED");
        }
        if (!g_ctl) CreateControlWindow();
        // Fallback poll while any window of this process is in the foreground (polling alone missed short taps,
        // the hook alone is not guaranteed for every message route)
        DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        bool fg = pid == GetCurrentProcessId();
        int mods = 0;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= 1;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mods |= 2;
        if (GetAsyncKeyState(VK_MENU) & 0x8000) mods |= 4;
        ULONGLONG now = GetTickCount64();
        for (size_t i = 0; i < g_hotkeys.size(); ++i)
        {
            Hotkey& h = g_hotkeys[i];
            bool down = fg && (GetAsyncKeyState(h.vk) & 0x8000) && mods == h.mods;
            bool press = down && !h.down;
            h.down = down;
            // the keyboard hook normally fired for this press already, the poll only fills in when it did not
            if (press && now - h.lastFire > 400) { Fire(h.vk, h.mods); if (i >= g_hotkeys.size()) break; }
        }
    }

    void Shutdown()
    {
        if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; }
        if (g_ctl) { DestroyWindow(g_ctl); g_ctl = nullptr; }
        g_hotkeys.clear();
    }
}
