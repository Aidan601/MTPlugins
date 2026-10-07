// d3d9.dll: loads RadiantPlugins.dll into Radiant and APE without a launcher.
// The editors import only D3DPERF_* from d3d9.dll, so a copy in <root>\bin wins over System32. DllMain loads the host from a worker thread.
// Every export forwards to the real System32\d3d9.dll. Settings: RadiantLoader.ini [loader] enabled, dll, process, delay_ms, log.
#include <windows.h>
#include <shlwapi.h>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <string>

#pragma comment(lib, "shlwapi.lib")

namespace
{
    HMODULE g_self = nullptr;
    HMODULE g_real = nullptr;               // the real System32\d3d9.dll, loaded on first forwarded call
    bool    g_log = true;

    std::wstring SelfDir()
    {
        wchar_t path[MAX_PATH * 2] = {};
        GetModuleFileNameW(g_self, path, MAX_PATH * 2);
        PathRemoveFileSpecW(path);
        return path;
    }

    std::wstring ProcessName()
    {
        wchar_t path[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
        return PathFindFileNameW(path);
    }

    void Log(const wchar_t* fmt, ...)
    {
        wchar_t line[1024];
        va_list ap;
        va_start(ap, fmt);
        _vsnwprintf_s(line, _TRUNCATE, fmt, ap);
        va_end(ap);

        wchar_t full[1200];
        SYSTEMTIME t; GetLocalTime(&t);
        _snwprintf_s(full, _TRUNCATE, L"[%02d:%02d:%02d] loader: %s\r\n", t.wHour, t.wMinute, t.wSecond, line);
        OutputDebugStringW(full);
        if (!g_log) return;

        HANDLE h = CreateFileW((SelfDir() + L"\\RadiantLoader.log").c_str(), FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;          // <root>\bin may not be writable
        char utf8[2400];
        int n = WideCharToMultiByte(CP_UTF8, 0, full, -1, utf8, sizeof utf8, nullptr, nullptr);
        DWORD written = 0;
        if (n > 1) WriteFile(h, utf8, static_cast<DWORD>(n - 1), &written, nullptr);
        CloseHandle(h);
    }

    std::wstring IniString(const wchar_t* key, const wchar_t* fallback)
    {
        wchar_t buf[MAX_PATH * 2] = {};
        GetPrivateProfileStringW(L"loader", key, fallback, buf, MAX_PATH * 2, (SelfDir() + L"\\RadiantLoader.ini").c_str());
        std::wstring s = buf;
        while (!s.empty() && iswspace(s.back())) s.pop_back();
        size_t a = 0; while (a < s.size() && iswspace(s[a])) ++a;
        return s.substr(a);
    }

    int IniInt(const wchar_t* key, int fallback)
    {
        return static_cast<int>(GetPrivateProfileIntW(L"loader", key, fallback, (SelfDir() + L"\\RadiantLoader.ini").c_str()));
    }

    // the real d3d9.dll by full path (never ourselves)
    FARPROC Real(const char* name)
    {
        if (!g_real)
        {
            wchar_t sys[MAX_PATH] = {};
            GetSystemDirectoryW(sys, MAX_PATH);
            HMODULE m = LoadLibraryW((std::wstring(sys) + L"\\d3d9.dll").c_str());
            if (m && InterlockedCompareExchangePointer(reinterpret_cast<PVOID*>(&g_real), m, nullptr) != nullptr)
                FreeLibrary(m);                          // another thread won the race
        }
        return g_real ? GetProcAddress(g_real, name) : nullptr;
    }

    DWORD WINAPI LoadHostThread(LPVOID)
    {
        // runs after the loader lock is released, so LoadLibraryW is safe, and still before Radiant's main()
        g_log = IniInt(L"log", 1) != 0;
        if (IniInt(L"enabled", 1) == 0) { Log(L"disabled in RadiantLoader.ini"); return 0; }

        std::wstring want = IniString(L"process", L"Radiant_modtools.exe,asseteditor_modtools.exe,modlauncher.exe");
        std::wstring self = ProcessName();
        if (!want.empty())
        {
            bool listed = false;
            size_t start = 0;
            while (start <= want.size())
            {
                size_t end = want.find_first_of(L",;", start);
                if (end == std::wstring::npos) end = want.size();
                std::wstring one = want.substr(start, end - start);
                while (!one.empty() && iswspace(one.back())) one.pop_back();
                size_t a = 0; while (a < one.size() && iswspace(one[a])) ++a;
                one = one.substr(a);
                if (!one.empty() && _wcsicmp(one.c_str(), self.c_str()) == 0) { listed = true; break; }
                start = end + 1;
            }
            if (!listed) return 0;      // some other process that imports d3d9
        }

        std::wstring dll;
        wchar_t env[MAX_PATH * 2] = {};
        if (GetEnvironmentVariableW(L"RADIANT_PLUGINS_DLL", env, MAX_PATH * 2) && env[0]) dll = env;
        if (dll.empty()) dll = IniString(L"dll", L"");
        if (!dll.empty() && PathIsRelativeW(dll.c_str())) dll = SelfDir() + L"\\" + dll;
        if (dll.empty()) dll = SelfDir() + L"\\RadiantPlugins.dll";

        if (GetModuleHandleW(L"RadiantPlugins.dll"))
        {
            Log(L"host already loaded in this process, nothing to do");
            return 0;
        }
        if (!PathFileExistsW(dll.c_str()))
        {
            Log(L"host NOT found: %s  (set [loader] dll= in RadiantLoader.ini)", dll.c_str());
            return 0;
        }

        int delay = IniInt(L"delay_ms", 0);
        if (delay > 0) Sleep(static_cast<DWORD>(delay));

        HMODULE h = LoadLibraryW(dll.c_str());
        if (h) Log(L"loaded %s into %s (pid %lu)", dll.c_str(), self.c_str(), GetCurrentProcessId());
        else   Log(L"LoadLibrary FAILED for %s (%lu)", dll.c_str(), GetLastError());
        return 0;
    }
}

// ---- exports: everything Radiant, APE or libGLESv264r.dll may want from d3d9.dll -------------------

extern "C"
{
    __declspec(dllexport) void* WINAPI Direct3DCreate9(UINT sdk)
    {
        typedef void* (WINAPI *Fn)(UINT);
        Fn fn = reinterpret_cast<Fn>(Real("Direct3DCreate9"));
        return fn ? fn(sdk) : nullptr;
    }

    __declspec(dllexport) HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, void** out)
    {
        typedef HRESULT (WINAPI *Fn)(UINT, void**);
        Fn fn = reinterpret_cast<Fn>(Real("Direct3DCreate9Ex"));
        return fn ? fn(sdk, out) : E_NOTIMPL;
    }

    __declspec(dllexport) void* WINAPI Direct3DShaderValidatorCreate9(void)
    {
        typedef void* (WINAPI *Fn)(void);
        Fn fn = reinterpret_cast<Fn>(Real("Direct3DShaderValidatorCreate9"));
        return fn ? fn() : nullptr;
    }

    __declspec(dllexport) int WINAPI D3DPERF_BeginEvent(DWORD colour, LPCWSTR name)
    {
        typedef int (WINAPI *Fn)(DWORD, LPCWSTR);
        Fn fn = reinterpret_cast<Fn>(Real("D3DPERF_BeginEvent"));
        return fn ? fn(colour, name) : 0;
    }

    __declspec(dllexport) int WINAPI D3DPERF_EndEvent(void)
    {
        typedef int (WINAPI *Fn)(void);
        Fn fn = reinterpret_cast<Fn>(Real("D3DPERF_EndEvent"));
        return fn ? fn() : 0;
    }

    __declspec(dllexport) void WINAPI D3DPERF_SetMarker(DWORD colour, LPCWSTR name)
    {
        typedef void (WINAPI *Fn)(DWORD, LPCWSTR);
        Fn fn = reinterpret_cast<Fn>(Real("D3DPERF_SetMarker"));
        if (fn) fn(colour, name);
    }

    __declspec(dllexport) void WINAPI D3DPERF_SetRegion(DWORD colour, LPCWSTR name)
    {
        typedef void (WINAPI *Fn)(DWORD, LPCWSTR);
        Fn fn = reinterpret_cast<Fn>(Real("D3DPERF_SetRegion"));
        if (fn) fn(colour, name);
    }

    __declspec(dllexport) BOOL WINAPI D3DPERF_QueryRepeatFrame(void)
    {
        typedef BOOL (WINAPI *Fn)(void);
        Fn fn = reinterpret_cast<Fn>(Real("D3DPERF_QueryRepeatFrame"));
        return fn ? fn() : FALSE;
    }

    __declspec(dllexport) void WINAPI D3DPERF_SetOptions(DWORD options)
    {
        typedef void (WINAPI *Fn)(DWORD);
        Fn fn = reinterpret_cast<Fn>(Real("D3DPERF_SetOptions"));
        if (fn) fn(options);
    }

    __declspec(dllexport) DWORD WINAPI D3DPERF_GetStatus(void)
    {
        typedef DWORD (WINAPI *Fn)(void);
        Fn fn = reinterpret_cast<Fn>(Real("D3DPERF_GetStatus"));
        return fn ? fn() : 0;
    }

    // marker export: --install-loader / --uninstall-loader tell our d3d9.dll from someone else's with it
    __declspec(dllexport) const char* RadiantPluginsLoader(void) { return "RadiantPlugins loader 1.0"; }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = module;
        DisableThreadLibraryCalls(module);
        // only CreateThread here: DllMain runs under the loader lock
        HANDLE th = CreateThread(nullptr, 0, LoadHostThread, nullptr, 0, nullptr);
        if (th) CloseHandle(th);
    }
    return TRUE;
}
