// Launcher Plus: improvements to the stock Mod Tools Launcher, runs only in modlauncher.exe.
// Loads BuildPlusBridge.dll (hooks CreateProcessW), adds the "BuildPlus" menu (settings go to LauncherPlus.ini, read by the wrappers)
// and the Defender exclusions item ([defender] asked, exclusions). Themes: launchertheme.cpp, Publish Mod: publish.cpp.
#define MODTOOLS_PLUGIN_IMPL
#include "modtools_plugin.hpp"
#include "launchertheme.h"
#include "publish.h"
#include "lightquality.h"
#include "steamstart.h"
#include "discordrpc.h"
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <objbase.h>
#include <string>
#include <vector>

namespace
{
    HANDLE g_controlMap = nullptr;
    LONG* g_controlCount = nullptr;
    std::string g_status = "Not active.";
    std::wstring g_ini;
    std::wstring g_dir;
    std::wstring g_bin;              // the launcher's folder, <game>\bin
    ModToolsWidget g_menu = nullptr;
    ModToolsWidget g_defenderAction = nullptr;
    LONG g_defenderBusy = 0;         // one PowerShell at a time
    bool g_defenderPrompted = false; // the first-launch offer, at most once per process

    // The menu's two switches: checked = redirect plus optimizations (enabled=1, patches=1), unchecked = the stock tool.
    struct Toggle
    {
        const wchar_t* section;
        const char* label;
        ModToolsWidget action;
        int enabledDefault;   // enabled= when the key is missing
    };
    Toggle g_toggles[] = {
        { L"compile", "Compile with BetterBuild", nullptr, 1 },
        { L"linker", "Link with BetterBuild", nullptr, 1 },
        { L"light", "Light with BetterBuild", nullptr, 1 },
    };

    bool IsOn(const Toggle& t)
    {
        return GetPrivateProfileIntW(t.section, L"enabled", t.enabledDefault, g_ini.c_str()) &&
               GetPrivateProfileIntW(t.section, L"patches", 1, g_ini.c_str());
    }

    // the bridge sends the Light step through BuildPlusLight.exe, which maps the quality box's Draft entry
    bool LightRouted()
    {
        return GetPrivateProfileIntW(L"launcherplus", L"enabled", 1, g_ini.c_str()) && IsOn(g_toggles[2]);
    }

    bool DefenderApplied() { return GetPrivateProfileIntW(L"defender", L"exclusions", 0, g_ini.c_str()) != 0; }

    HWND MainWindow()
    {
        return rp::Has(&rp::host->MainWindow) ? static_cast<HWND>(rp::host->MainWindow(rp::self)) : nullptr;
    }

    void SyncMenu()
    {
        if (!rp::HasUi(&rp::host->ui->SetActionChecked))
        {
            static bool said = false;
            if (!said) rp::Log("host too old for menu check marks (no SetActionChecked); the switches still work, restart the launcher after updating RadiantPlugins.dll");
            said = true;
            return;
        }
        for (const Toggle& t : g_toggles)
            if (t.action) rp::host->ui->SetActionChecked(t.action, 1, IsOn(t) ? 1 : 0);
        if (g_defenderAction) rp::host->ui->SetActionChecked(g_defenderAction, 1, DefenderApplied() ? 1 : 0);
    }

    // Applies to the next compile or link, one already running keeps what it read at its start.
    void OnToggle(void* user)
    {
        const Toggle& t = *static_cast<Toggle*>(user);
        const wchar_t* value = IsOn(t) ? L"0" : L"1";
        WritePrivateProfileStringW(t.section, L"enabled", value, g_ini.c_str());
        WritePrivateProfileStringW(t.section, L"patches", value, g_ini.c_str());
        rp::Log("%s: %ls", t.label, value[0] == L'1' ? L"on" : L"off");
        SyncMenu();
    }

    // ---- Windows Defender process exclusions -------------------------------------------------------------------

    // Runs powershell.exe hidden and returns its exit code (-1 if it could not start). Not elevated.
    int RunPowerShell(const std::wstring& args, DWORD timeoutMs)
    {
        std::wstring line = L"powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden " + args;
        std::vector<wchar_t> buffer(line.begin(), line.end());
        buffer.push_back(0);
        STARTUPINFOW si = { sizeof si };
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            return -1;
        CloseHandle(pi.hThread);
        DWORD code = static_cast<DWORD>(-1);
        if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
        else TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        return static_cast<int>(code);
    }

    // Defender is the active antivirus with real-time protection on (0), or not (anything else). No admin needed.
    int DefenderActive()
    {
        return RunPowerShell(L"-Command \"$s = Get-MpComputerStatus; if ($s.RealTimeProtectionEnabled -and "
                             L"$s.AMRunningMode -eq 'Normal') { exit 0 } else { exit 3 }\"", 30000);
    }

    // Runs defender_exclusions.ps1 <add|remove> "<bin>" elevated (one UAC prompt). Returns the script's exit code,
    // 1223 when the UAC prompt was declined, -1 when PowerShell could not start.
    int RunExclusionScript(const wchar_t* mode)
    {
        const std::wstring script = g_dir + L"\\defender_exclusions.ps1";
        if (GetFileAttributesW(script.c_str()) == INVALID_FILE_ATTRIBUTES) return -2;
        std::wstring params = L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" +
                              script + L"\" " + mode + L" \"" + g_bin + L"\"";
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        SHELLEXECUTEINFOW info = { sizeof info };
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        info.hwnd = MainWindow();
        info.lpVerb = L"runas";
        info.lpFile = L"powershell.exe";
        info.lpParameters = params.c_str();
        info.nShow = SW_HIDE;
        int result = -1;
        if (ShellExecuteExW(&info))
        {
            DWORD code = static_cast<DWORD>(-1);
            if (info.hProcess)
            {
                if (WaitForSingleObject(info.hProcess, 180000) == WAIT_OBJECT_0) GetExitCodeProcess(info.hProcess, &code);
                else TerminateProcess(info.hProcess, 1);
                CloseHandle(info.hProcess);
            }
            result = static_cast<int>(code);
        }
        else result = static_cast<int>(GetLastError());   // ERROR_CANCELLED (1223): the UAC prompt was declined
        if (SUCCEEDED(com)) CoUninitialize();
        return result;
    }

    void Inform(const char* text, UINT icon)
    {
        MessageBoxA(MainWindow(), text, "BetterBuild", MB_OK | icon);
    }

    // Called on the main thread with the script's result.
    void OnExclusionResult(bool add, int code)
    {
        rp::Log("Defender exclusions %s: result %d", add ? "add" : "remove", code);
        if (code == 0)
        {
            SYSTEMTIME st; GetLocalTime(&st);
            wchar_t when[32]; swprintf_s(when, L"%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
            WritePrivateProfileStringW(L"defender", L"exclusions", add ? L"1" : L"0", g_ini.c_str());
            WritePrivateProfileStringW(L"defender", add ? L"applied" : L"removed", when, g_ini.c_str());
            Inform(add ? "Done. Windows Defender now skips the three build programs. Undo it any time from the Launcher+ menu."
                       : "Done. Windows Defender scans the three build programs again.", MB_ICONINFORMATION);
        }
        else if (code == 1223) { /* the UAC prompt was declined: nothing changed, nothing to say */ }
        else if (code == 3)
            Inform("Windows Defender is not the active antivirus on this PC (another product is), so there is nothing "
                   "to exclude. Nothing was changed.", MB_ICONINFORMATION);
        else if (code == -2)
            Inform("defender_exclusions.ps1 is missing from the Launcher+ plugin folder. Nothing was changed.", MB_ICONWARNING);
        else
            Inform("Windows Defender did not accept the change. This happens when its settings are managed by an "
                   "organization, or when a different antivirus is installed. Nothing was changed.\n\n"
                   "You can add the exclusions by hand: Windows Security > Virus & threat protection > Manage settings > "
                   "Exclusions > Add an exclusion > Process, for cod2map64.exe, linker_modtools.exe and snd_convert.exe.",
                   MB_ICONWARNING);
        InterlockedExchange(&g_defenderBusy, 0);
        SyncMenu();
    }

    struct ExclusionJob { bool add; };

    DWORD WINAPI ExclusionThread(void* user)
    {
        ExclusionJob* job = static_cast<ExclusionJob*>(user);
        const bool add = job->add;
        delete job;
        int code = -1;
        try { code = RunExclusionScript(add ? L"add" : L"remove"); } catch (...) { code = -1; }
        rp::PostToMainThread([add, code] { OnExclusionResult(add, code); });
        return 0;
    }

    void StartExclusionChange(bool add)
    {
        if (InterlockedCompareExchange(&g_defenderBusy, 1, 0) != 0) return;
        HANDLE h = CreateThread(nullptr, 0, ExclusionThread, new ExclusionJob{ add }, 0, nullptr);
        if (h) CloseHandle(h);
        else InterlockedExchange(&g_defenderBusy, 0);
    }

    const char* const kOfferText =
        "Windows Defender scans every file a build opens, so compiles and links take longer.\n\n"
        "Let BetterBuild tell Defender to skip the three build programs (cod2map64.exe, linker_modtools.exe and "
        "snd_convert.exe)? Windows asks for administrator permission once. Nothing else is excluded, and the "
        "Launcher+ menu undoes it.";

    void OnDefenderToggle(void*)
    {
        if (g_defenderBusy) return;
        if (DefenderApplied())
        {
            if (MessageBoxA(MainWindow(), "Let Windows Defender scan the three build programs again? Builds take longer. "
                            "Windows asks for administrator permission once.",
                            "BetterBuild", MB_YESNO | MB_ICONQUESTION) == IDYES)
                StartExclusionChange(false);
        }
        else if (MessageBoxA(MainWindow(), kOfferText, "BetterBuild", MB_YESNO | MB_ICONQUESTION) == IDYES)
            StartExclusionChange(true);
    }

    // First start: one offer, asked=1 whatever the answer. Skipped when applied or Defender is not the active antivirus.
    DWORD WINAPI FirstOfferThread(void*)
    {
        int active = 3;
        try { active = DefenderActive(); } catch (...) {}
        rp::PostToMainThread([active] {
            WritePrivateProfileStringW(L"defender", L"asked", L"1", g_ini.c_str());
            if (active != 0) { rp::Log("Defender offer skipped: Defender is not the active antivirus (%d)", active); return; }
            if (g_defenderPrompted || g_defenderBusy || DefenderApplied()) return;
            g_defenderPrompted = true;
            if (MessageBoxA(MainWindow(), kOfferText, "BetterBuild", MB_YESNO | MB_ICONQUESTION) == IDYES) StartExclusionChange(true);
            else rp::Log("Defender offer declined; the Launcher+ menu has it.");
        });
        return 0;
    }

    void MaybeOfferExclusions()
    {
        if (DefenderApplied() || GetPrivateProfileIntW(L"defender", L"asked", 0, g_ini.c_str())) return;
        HANDLE h = CreateThread(nullptr, 0, FirstOfferThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }

    // Files an antivirus may quarantine (the wrappers inject a DLL). A missing wrapper silently falls back to the stock tool.
    void CheckFiles()
    {
        static const wchar_t* const kFiles[] = { L"BuildPlusWorker.exe", L"BuildPlusCompiler.dll",
                                                 L"BuildPlusLinker.exe", L"BuildPlusLinkerRT.dll" };
        std::string missing;
        for (const wchar_t* f : kFiles)
            if (GetFileAttributesW((g_dir + L"\\" + f).c_str()) == INVALID_FILE_ATTRIBUTES)
                { if (!missing.empty()) missing += ", "; for (const wchar_t* c = f; *c; ++c) missing += static_cast<char>(*c); }
        if (missing.empty()) return;
        rp::Log("BetterBuild files missing: %s (quarantined by an antivirus?)", missing.c_str());
        g_status = "Some BetterBuild files are missing: " + missing + ". Builds use the stock tools.";
        Inform(("These BetterBuild files are missing from the plugin folder:\n\n" + missing +
                "\n\nBuilds run with the stock tools until they are back. If an antivirus removed them (a false "
                "positive: they start the build tools and load an add-on into them), restore them from Windows "
                "Security > Protection history and report the file at microsoft.com/wdsi/filesubmission, or "
                "reinstall the plugins.").c_str(), MB_ICONWARNING);
    }

    // A "BuildPlus" menu in the launcher's menu bar, beside the host's "Plugins" menu (the host hides it on unload).
    void AddLauncherMenu()
    {
        if (!rp::Has(&rp::host->AddMenu)) return;
        g_menu = rp::host->AddMenu(rp::self, "Launcher+");
        if (!g_menu) { rp::Log("BetterBuild: launcher menu bar not found; edit LauncherPlus.ini to switch compile or link off."); return; }
        for (Toggle& t : g_toggles) t.action = rp::host->AddMenuItem(rp::self, g_menu, t.label, OnToggle, &t);
        if (rp::Has(&rp::host->AddMenuSeparator)) rp::host->AddMenuSeparator(rp::self, g_menu);
        g_defenderAction = rp::host->AddMenuItem(rp::self, g_menu, "Skip Windows Defender scans while building", OnDefenderToggle, nullptr);
        SyncMenu();
    }

    void ReleaseControl()
    {
        if (g_controlCount) { InterlockedDecrement(g_controlCount); UnmapViewOfFile(g_controlCount); }
        if (g_controlMap) CloseHandle(g_controlMap);
        g_controlCount = nullptr;
        g_controlMap = nullptr;
    }

    // ---- Discord presence ----
    // the first checked row of the Maps and Mods tree, the item every step works on
    std::string CheckedItem()
    {
        using FindFn = void* (*)(uintptr_t);
        static const FindFn find = reinterpret_cast<FindFn>(GetProcAddress(GetModuleHandleW(L"Qt5Widgets64r.dll"), "?find@QWidget@@SAPEAV1@_K@Z"));   // QWidget::find(WId)
        const ModToolsUiApi* ui = rp::host->ui;
        const HWND hwnd = MainWindow();
        void* main = find && hwnd ? find(reinterpret_cast<uintptr_t>(hwnd)) : nullptr;
        if (!ui || !main || !rp::HasUi(&ui->TreeItemChecked)) return "";
        for (int n = 0; n < 16; ++n)
        {
            void* tree = ui->FindChild(main, "QTreeWidget", n);
            if (!tree) break;
            if (ui->TreeChildCount(tree, nullptr) != 2) continue;
            auto textOf = [&](ModToolsTreeItem it) { char b[512] = {}; ui->TreeItemText(it, 0, b, sizeof b); return std::string(b); };
            ModToolsTreeItem maps = ui->TreeChild(tree, nullptr, 0), mods = ui->TreeChild(tree, nullptr, 1);
            if (textOf(maps) != "Maps" || textOf(mods) != "Mods") continue;
            for (int i = 0; i < ui->TreeChildCount(tree, maps); ++i)
            {
                ModToolsTreeItem it = ui->TreeChild(tree, maps, i);
                if (ui->TreeItemChecked(it)) return textOf(it);
            }
            for (int i = 0; i < ui->TreeChildCount(tree, mods); ++i)
            {
                ModToolsTreeItem mod = ui->TreeChild(tree, mods, i);
                for (int k = 0; k < ui->TreeChildCount(tree, mod); ++k)
                    if (ui->TreeItemChecked(ui->TreeChild(tree, mod, k))) return textOf(mod);
            }
            return "";
        }
        return "";
    }

    bool HasVisibleWindow(DWORD pid)
    {
        struct Find { DWORD pid; bool found; } f = { pid, false };
        EnumWindows([](HWND w, LPARAM p) -> BOOL
        {
            Find* f = reinterpret_cast<Find*>(p);
            DWORD owner = 0;
            GetWindowThreadProcessId(w, &owner);
            if (owner != f->pid || !IsWindowVisible(w)) return TRUE;
            f->found = true;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&f));
        return f.found;
    }

    // the build step running now, from the launcher's child processes (stock tools or our wrappers), nullptr = none
    const char* RunningStep()
    {
        const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return nullptr;
        const DWORD me = GetCurrentProcessId();
        const char* step = nullptr;
        PROCESSENTRY32W e = { sizeof e };
        for (BOOL ok = Process32FirstW(snap, &e); ok && !step; ok = Process32NextW(snap, &e))
        {
            if (e.th32ParentProcessID != me) continue;
            if (!_wcsicmp(e.szExeFile, L"cod2map64.exe") || !_wcsicmp(e.szExeFile, L"BuildPlusWorker.exe")) step = "compiling";
            else if (!_wcsicmp(e.szExeFile, L"BuildPlusLight.exe")) step = "lighting";
            else if (!_wcsicmp(e.szExeFile, L"Radiant_modtools.exe")) step = HasVisibleWindow(e.th32ProcessID) ? nullptr : "lighting";   // a Radiant with a window is the editor
            else if (!_wcsicmp(e.szExeFile, L"linker_modtools.exe") || !_wcsicmp(e.szExeFile, L"BuildPlusLinker.exe")) step = "linking";
            else if (!_wcsicmp(e.szExeFile, L"BlackOps3.exe")) step = "testing";
        }
        CloseHandle(snap);
        return step;
    }

    // detail mode: "Launcher: compiling <map>", plain "Launcher" between builds
    discordrpc::Activity DiscordActivity()
    {
        discordrpc::Activity a;
        const char* step = RunningStep();
        if (!step) { a.details = "Launcher"; return a; }
        const std::string item = CheckedItem();
        a.details = std::string("Launcher: ") + step + " " + (item.empty() ? "a map" : item);
        if (!strcmp(step, "testing")) a.details += " in game";
        return a;
    }

    int OnLoad()
    {
        if (rp::App() != MODTOOLS_APP_LAUNCHER) return 0;
        g_dir = rp::PluginDir();
        g_ini = g_dir + L"\\LauncherPlus.ini";
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        g_bin = exe;
        const size_t slash = g_bin.find_last_of(L'\\');
        if (slash != std::wstring::npos) g_bin.resize(slash);
        HMODULE bridge = GetModuleHandleW(L"BuildPlusBridge.dll");
        if (!bridge) bridge = LoadLibraryW((g_dir + L"\\BuildPlusBridge.dll").c_str());
        using StartFn = BOOL (WINAPI*)();
        auto start = bridge ? reinterpret_cast<StartFn>(GetProcAddress(bridge, "BuildPlusStart")) : nullptr;
        if (!start || !start())
        {
            rp::Log("BetterBuild bridge failed to start. Restart the launcher after updating its DLLs.");
            return 0;
        }
        wchar_t name[96];
        swprintf_s(name, L"Local\\RadiantBuildPlusControl_%lu", GetCurrentProcessId());
        g_controlMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LONG), name);
        g_controlCount = g_controlMap ? static_cast<LONG*>(MapViewOfFile(g_controlMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LONG))) : nullptr;
        if (!g_controlCount) { ReleaseControl(); return 0; }
        InterlockedIncrement(g_controlCount);
        g_status = "Active for this launcher. Unticking Launcher+ in any Plugins window turns it off for the next build; "
                   "a build already running is not changed.";
        rp::Log("%s", g_status.c_str());
        steamstart::Setup(g_bin);   // modtools_launcher.bat, the file Steam runs
        AddLauncherMenu();
        launchertheme::Setup(g_ini);     // the themes, picked in Edit > Options
        publish::Setup();                // the Publish Mod dialog: thumbnail check, Workshop Description
        lightquality::Setup(g_ini, LightRouted);   // Draft in the Light quality box
        discordrpc::Setup("launcher", "Mod Tools Launcher");
        SyncMenu();
        CheckFiles();
        MaybeOfferExclusions();
        return 1;
    }

    void OnTick()
    {
        launchertheme::Tick(); publish::Tick(); lightquality::Tick();
        if (discordrpc::Due()) discordrpc::Update(DiscordActivity());
    }

    void OnUnload()
    {
        discordrpc::Shutdown();
        publish::Shutdown();
        lightquality::Shutdown();
        launchertheme::Shutdown();
        ReleaseControl();
        for (Toggle& t : g_toggles) t.action = nullptr;  // the host removes the menu
        g_defenderAction = nullptr;
        g_menu = nullptr;
    }

    const char* OnCommand(const char* text)
    {
        const std::string cmd = text ? text : "";
        static std::string reply;
        reply.clear();
        if (launchertheme::Command(cmd, reply)) return reply.c_str();
        if (publish::Command(cmd, reply)) return reply.c_str();
        if (discordrpc::Command(cmd, reply)) return reply.c_str();
        if (cmd == "status")
        {
            reply = g_status;
            for (const Toggle& t : g_toggles) reply += std::string(" ") + t.label + (IsOn(t) ? ": on." : ": off.");
            reply += " " + lightquality::Status() + ".";
            reply += DefenderApplied() ? " Defender exclusions: applied." : " Defender exclusions: not applied.";
            reply += " " + steamstart::Status() + ".";
            return reply.c_str();
        }
        if (cmd == "defender add" || cmd == "defender remove")   // same path as the menu, minus the question
        {
            StartExclusionChange(cmd == "defender add");
            reply = g_defenderBusy ? "started (Windows asks for administrator permission)" : "busy";
            return reply.c_str();
        }
        return nullptr;
    }
}

extern "C" MODTOOLS_PLUGIN_EXPORT const ModToolsPluginApi* ModToolsPluginEntry(ModToolsPlugin* self, const ModToolsHostApi* host)
{
    rp::Bind(self, host);
    static ModToolsPluginApi api = {};
    api.size = sizeof api;
    api.apiVersion = MODTOOLS_PLUGIN_API_VERSION;
    api.name = "LauncherPlus";
    api.displayName = "Launcher+";
    api.version = "1.1.1";
    api.author = "Figglebottom";
    api.description = "QOL fixes and improvements to the Launcher.";
    api.url = "https://github.com/Aidan601/MTPlugins/tree/main/plugins/LauncherPlus";
    api.OnLoad = OnLoad;
    api.OnUnload = OnUnload;
    api.OnTick = OnTick;
    api.OnCommand = OnCommand;
    api.apps = MODTOOLS_APP_LAUNCHER;
    return &api;
}
