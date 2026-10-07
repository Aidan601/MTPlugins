// Bridge inside modlauncher.exe: hooks CreateProcessW and sends the launcher's sibling tools (bin\cod2map64.exe, ...)
// through a BuildPlus wrapper next to this DLL. Loaded once, never unloaded (the Plugins switch only gates new launches).
// Wrapper contract: "<wrapper>" --real "<full path of the real tool>" -- <original args...>, the rest passes through.
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include "MinHook.h"

namespace
{
    struct Redirect
    {
        const wchar_t* tool;     // file name beside modlauncher.exe
        const wchar_t* wrapper;  // file name beside this DLL
        const wchar_t* section;  // LauncherPlus.ini section holding enabled=
        int enabledDefault;      // enabled= when the key is missing
        const wchar_t* needArg;  // redirect only launches carrying this argument (null = every launch)
    };
    constexpr Redirect kRedirects[] = {
        { L"cod2map64.exe",        L"BuildPlusWorker.exe", L"compile", 1, nullptr },
        { L"linker_modtools.exe",  L"BuildPlusLinker.exe", L"linker",  1, nullptr },
        // The Light step only: the launcher's "Radiant" button starts the same exe without -ledSilent.
        { L"Radiant_modtools.exe", L"BuildPlusLight.exe",  L"light",   1, L"-ledSilent" },
    };

    bool HasArg(LPWSTR* argv, int argc, const wchar_t* arg)
    {
        for (int i = 1; i < argc; ++i) if (!_wcsicmp(argv[i], arg)) return true;
        return false;
    }

    using CreateFn = decltype(&CreateProcessW);
    CreateFn g_create = nullptr;
    HMODULE g_self = nullptr;
    SRWLOCK g_startLock = SRWLOCK_INIT;
    bool g_started = false;
    std::wstring g_pluginDir, g_binDir, g_ini;

    std::wstring Parent(const std::wstring& p) { return p.substr(0, p.find_last_of(L"\\/")); }

    std::wstring FullPath(const wchar_t* p)
    {
        if (!p || !*p) return {};
        std::vector<wchar_t> full(32768);
        DWORD n = GetFullPathNameW(p, (DWORD)full.size(), full.data(), nullptr);
        return n && n < full.size() ? std::wstring(full.data(), n) : std::wstring();
    }

    // CommandLineToArgvW-compatible quoting.
    std::wstring Quote(const std::wstring& s)
    {
        if (!s.empty() && s.find_first_of(L" \t\n\v\"") == std::wstring::npos) return s;
        std::wstring out = L"\"";
        size_t slashes = 0;
        for (wchar_t c : s)
        {
            if (c == L'\\') { ++slashes; continue; }
            out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            slashes = 0;
            out += c;
        }
        out.append(slashes * 2, L'\\');
        return out + L"\"";
    }

    // the plugin holds a named counter per launcher process, zero or missing means disabled
    bool PluginEnabled()
    {
        wchar_t name[96];
        swprintf_s(name, L"Local\\RadiantBuildPlusControl_%lu", GetCurrentProcessId());
        HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
        if (!map) return false;
        auto count = static_cast<const volatile LONG*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(LONG)));
        bool enabled = count && *count > 0;
        if (count) UnmapViewOfFile(const_cast<const LONG*>(count));
        CloseHandle(map);
        return enabled;
    }

    const Redirect* Match(const std::wstring& target)
    {
        if (_wcsicmp(Parent(target).c_str(), g_binDir.c_str())) return nullptr;
        const wchar_t* name = target.c_str() + target.find_last_of(L"\\/") + 1;
        for (const Redirect& r : kRedirects)
            if (!_wcsicmp(name, r.tool)) return &r;
        return nullptr;
    }

    BOOL WINAPI Hook(LPCWSTR app, LPWSTR command, LPSECURITY_ATTRIBUTES processAttrs,
                     LPSECURITY_ATTRIBUTES threadAttrs, BOOL inherit, DWORD flags, LPVOID env,
                     LPCWSTR cwd, LPSTARTUPINFOW startup, LPPROCESS_INFORMATION process)
    {
        // Startup contracts we do not reproduce pass through unchanged (the launcher uses plain QProcess).
        try
        {
            if (!(flags & (DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS | EXTENDED_STARTUPINFO_PRESENT)) &&
                GetPrivateProfileIntW(L"launcherplus", L"enabled", 1, g_ini.c_str()) && PluginEnabled())
            {
                int argc = 0;
                LPWSTR* argv = command && *command ? CommandLineToArgvW(command, &argc) : nullptr;
                const std::wstring target = FullPath(app && *app ? app : argc ? argv[0] : nullptr);
                const Redirect* r = target.empty() ? nullptr : Match(target);
                const std::wstring wrapper = r ? g_pluginDir + L"\\" + r->wrapper : std::wstring();
                if (r && (!r->needArg || HasArg(argv, argc, r->needArg)) &&
                    GetPrivateProfileIntW(r->section, L"enabled", r->enabledDefault, g_ini.c_str()) &&
                    GetFileAttributesW(wrapper.c_str()) != INVALID_FILE_ATTRIBUTES)
                {
                    std::wstring line = Quote(wrapper) + L" --real " + Quote(target) + L" --";
                    for (int i = 1; i < argc; ++i) line += L" " + Quote(argv[i]);
                    if (argv) LocalFree(argv);
                    std::vector<wchar_t> buffer(line.begin(), line.end());
                    buffer.push_back(0);
                    return g_create(wrapper.c_str(), buffer.data(), processAttrs, threadAttrs, inherit,
                                    flags, env, cwd, startup, process);
                }
                if (argv) LocalFree(argv);
            }
        }
        catch (...) { /* allocation failure: leave the original launch alone */ }
        return g_create(app, command, processAttrs, threadAttrs, inherit, flags, env, cwd, startup, process);
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI BuildPlusStart()
{
    AcquireSRWLockExclusive(&g_startLock);
    if (!g_started)
    {
        try
        {
            std::vector<wchar_t> path(32768);
            GetModuleFileNameW(g_self, path.data(), (DWORD)path.size());
            g_pluginDir = Parent(path.data());
            g_ini = g_pluginDir + L"\\LauncherPlus.ini";
            GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
            g_binDir = Parent(path.data());
            const MH_STATUS init = MH_Initialize();
            if (init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED)
            {
                void* target = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateProcessW"));
                if (target && MH_CreateHook(target, reinterpret_cast<void*>(Hook), reinterpret_cast<void**>(&g_create)) == MH_OK)
                {
                    g_started = MH_EnableHook(target) == MH_OK;
                    if (!g_started) MH_RemoveHook(target);
                }
            }
        }
        catch (...) { g_started = false; }
    }
    const bool started = g_started;
    ReleaseSRWLockExclusive(&g_startLock);
    return started;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) { g_self = instance; DisableThreadLibraryCalls(instance); }
    return TRUE;
}
