// RadiantPlugins.dll: the plugin host for the BO3 mod tools editors, loaded into Radiant_modtools.exe or
// asseteditor_modtools.exe (APE) by the d3d9.dll loader or RadiantPlugins_Launch.exe. It owns every hook
// into the editor and loads the plugins found in <root>\bin\plugins (see sdk/modtools_plugin.h).
#include <windows.h>
#include <string>
#include "log.h"
#include "config.h"
#include "radiant_addrs.h"
#include "hooks.h"
#include "mainthread.h"
#include "render.h"
#include "plugins.h"
#include "pluginmgr.h"
#include "ui.h"
#include "dock.h"
#include "app.h"
#include "ape.h"
#include "qt.h"
#include "support.h"
#include "MinHook.h"

namespace
{
    HMODULE g_self = nullptr;

    // ---- first show ----
    // QWidget::setVisible is detoured: when the main window is shown the first time, the first host tick runs right there
    // on the main thread, before Qt paints, so the first frame already has the theme, panes and menus.
    using SetVisible_t = void(__fastcall*)(void* widget, bool visible);
    SetVisible_t o_SetVisible = nullptr;
    void* (*p_QWidget_window)(const void*) = nullptr;
    HANDLE g_shown = nullptr;           // auto-reset, set at the first show: the init thread stops waiting for the window
    HANDLE g_initDone = nullptr;        // manual-reset, set when the init thread is through: the first tick may run
    bool   g_firstShowTaken = false;    // main thread only: the first tick has run (first show, or the command-line map load)
    bool   g_firstShowSeen = false;     // main thread only: the main window's first show has passed

    void LauncherTick()
    {
        mt::DrainNow();
        ui::Tick();
        pluginmgr::Tick();
        plugins::OnTick();
        dock::RememberVisibility();
    }

    // the app's regular tick once (Radiant's runs inside its RoutineProcessing detour, hooks.cpp)
    void FirstTick()
    {
        if (app::IsRadiant()) { mt::DrainNow(); ui::Tick(); pluginmgr::Tick(); plugins::OnTick(); }
        else if (app::IsApe()) ape::Tick();
        else if (app::IsLauncher()) LauncherTick();
    }

    bool IsAppMainWindow(void* w)
    {
        if (app::IsRadiant()) return rt::VerifyBinary() && w == dock::MainWindowWidget();
        return p_QWidget_window && p_QWidget_window(w) == w && qt::Bind() && qt::IsMainWindow(w);
    }

    // Waits for the init thread while answering what other threads send to this one (its window search SendMessages the main thread).
    bool WaitInitDone(DWORD ms)
    {
        ULONGLONG end = GetTickCount64() + ms;
        for (;;)
        {
            ULONGLONG now = GetTickCount64();
            if (now >= end) return false;
            DWORD r = MsgWaitForMultipleObjects(1, &g_initDone, FALSE, DWORD(end - now), QS_SENDMESSAGE);
            if (r == WAIT_OBJECT_0) return true;
            if (r != WAIT_OBJECT_0 + 1) return false;
            MSG m; PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        }
    }

    void __fastcall hk_SetVisible(void* w, bool visible)
    {
        o_SetVisible(w, visible);
        if (!visible || g_firstShowSeen || !IsAppMainWindow(w)) return;
        g_firstShowSeen = true;
        ULONGLONG t0 = GetTickCount64();
        if (g_firstShowTaken)
        {
            // the map load ran the first tick on the hidden window: one more before the first paint, for what waits on a visible one
            if (WaitForSingleObject(g_initDone, 0) != WAIT_OBJECT_0) return;
            FirstTick();
            LOG("init: main window shown after the map load's first tick, one more tick before its first paint (%llu ms)", GetTickCount64() - t0);
            return;
        }
        g_firstShowTaken = true;
        SetEvent(g_shown);
        if (!WaitInitDone(3000)) { LOG("init: main window shown before the host was ready; plugins load on the regular tick"); return; }
        LOG("init: main window 0x%p shown, first tick before its first paint", w);
        FirstTick();
        LOG("init: first tick done in %llu ms", GetTickCount64() - t0);
    }

    bool HeadlessRadiant()
    {
        std::wstring cl = GetCommandLineW();
        for (auto& c : cl) c = static_cast<wchar_t>(towlower(c));
        return cl.find(L" -led") != std::wstring::npos;     // the Light step's bake: no window, plugins stay on the old path
    }

    void HookFirstShow()
    {
        HMODULE qw = GetModuleHandleW(L"Qt5Widgets64r.dll");
        void* target = qw ? reinterpret_cast<void*>(GetProcAddress(qw, "?setVisible@QWidget@@UEAAX_N@Z")) : nullptr;
        p_QWidget_window = qw ? reinterpret_cast<void* (*)(const void*)>(GetProcAddress(qw, "?window@QWidget@@QEBAPEAV1@XZ")) : nullptr;
        if (!target || !p_QWidget_window) { LOG("init: QWidget::setVisible not found; plugins load on the regular tick"); return; }
        if (MH_CreateHook(target, reinterpret_cast<void*>(&hk_SetVisible), reinterpret_cast<void**>(&o_SetVisible)) != MH_OK) { LOG("init: setVisible detour failed; plugins load on the regular tick"); return; }
        if (MH_EnableHook(target) != MH_OK) { MH_RemoveHook(target); LOG("init: setVisible detour failed; plugins load on the regular tick"); return; }
    }

    std::wstring DllDirectory()
    {
        wchar_t path[MAX_PATH * 2] = {};
        GetModuleFileNameW(g_self, path, MAX_PATH * 2);
        std::wstring p(path);
        size_t slash = p.find_last_of(L"\\/");
        return slash == std::wstring::npos ? L"." : p.substr(0, slash);
    }

    DWORD WINAPI InitThread(LPVOID)
    {
        std::wstring dir = DllDirectory();
        rtclog::Init(dir + L"\\" + app::LogFileName());
        LOG("==== RadiantPlugins host attached to %s, pid %lu ====", app::Name(), GetCurrentProcessId());
        LoadConfig(dir);
        if (app::Current() == app::Kind::Unknown) { LOG("init: not a supported mod tool; staying passive"); return 0; }

        hooks::Install();          // Radiant only (MinHook is still initialised for plugin detours)
        if (app::IsApe()) ape::InstallStartupFixes();   // before APE's own startup reaches its GDT cache save
        support::StartGuardBegin();    // may ask "start without plugins?" (blocks this thread only, plugins wait for the answer)
        g_shown = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_initDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HookFirstShow();

        // Radiant shows a splash first, the main window appears after asset/DB init (APE: gdt.db open + tree build).
        for (int i = 0; i < 480 && !mt::Init(); ++i) WaitForSingleObject(g_shown, 250);
        if (!mt::MainWindow()) LOG("init: main window never appeared; main-thread jobs unavailable");

        if (app::IsRadiant())
        {
            // d3d11 is loaded once Radiant creates its views, retry briefly if early.
            for (int i = 0; i < 40 && !GetModuleHandleW(L"d3d11.dll"); ++i) Sleep(250);
            render::Init();
            // plugins load at the main window's first show (hk_SetVisible), else from the idle tick (pluginmgr::Tick)
        }
        else if (app::IsApe())
        {
            // no idle function to detour: a window timer on the main window drives the tick
            app::VerifyApeBinary();
            ape::Start();
        }
        else if (app::IsLauncher())
        {
            // Only exported Qt interfaces, no launcher RVAs or editor-specific callbacks.
            mt::StartTimerTick(LauncherTick, 50);
        }
        LOG("init: done");
        SetEvent(g_initDone);
        return 0;
    }
}

namespace init
{
    // A map named on Radiant's command line loads inside InitInstance before the main window shows. hk_Map_LoadFile calls
    // this first: if the window is not shown yet, the first tick runs now and the show later adds one tick before its paint.
    void BeforeMapLoad()
    {
        if (!app::IsRadiant() || g_firstShowTaken || !g_initDone || HeadlessRadiant()) return;
        if (!WaitInitDone(3000)) { LOG("init: map load before the host was ready; plugins load at the first show"); return; }
        if (!mt::IsMainThread()) { LOG("init: map load off the main thread; plugins load at the first show"); return; }
        if (!dock::Ready()) { LOG("init: map load before the main window exists; plugins load at the first show"); return; }
        g_firstShowTaken = true;
        SetEvent(g_shown);
        ULONGLONG t0 = GetTickCount64();
        LOG("init: map load before the main window's first show, first tick now so plugins see the load");
        FirstTick();
        LOG("init: first tick done in %llu ms", GetTickCount64() - t0);
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        g_self = hModule;
        DisableThreadLibraryCalls(hModule);
        ape::ArmGdtScan(hModule);       // APE only: before its main() reaches the GDT walk (pointer writes, loader-lock safe)
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        break;
    case DLL_PROCESS_DETACH:
        if (reserved == nullptr)        // FreeLibrary, not process exit
        {
            plugins::Shutdown();
            pluginmgr::Shutdown();
            ui::Shutdown();
            dock::Shutdown();
            hooks::Remove();
            mt::Shutdown();
        }
        rtclog::Shutdown();
        break;
    }
    return TRUE;
}
