// Wrapper for the launcher's Light step (kill-on-close job), started by BuildPlusBridge.dll. With [light] patches=1 and
// the known Radiant build it loads BuildPlusLightRT.dll first, on failure a fresh stock Radiant bakes instead.
// Usage: BuildPlusLight.exe [--low-rays 64] --real <Radiant_modtools.exe> -- -ledSilent +low|+medium|+high ... <map>
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <cstdio>
#include "../worker/inject.h"

namespace
{
    // Radiant_modtools.exe, the current Steam build the bake module's addresses are for
    constexpr unsigned char kRadiantSha256[32] = {
        0xb4,0x4c,0x6d,0x21,0x42,0x6b,0x9e,0x33,0xb4,0x46,0x33,0x45,0x02,0x06,0x71,0x61,
        0x3e,0xfd,0xc7,0x67,0x8b,0xd3,0x9e,0x3b,0x36,0xd5,0x46,0x34,0xeb,0x85,0x10,0x91};

    std::wstring Parent(const std::wstring& p) { return p.substr(0, p.find_last_of(L"\\/")); }

    // true when the Mod Tools Launcher started us (through the bridge), not a script
    bool FromLauncher()
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return false;
        PROCESSENTRY32W e = { sizeof(e) };
        DWORD parent = 0;
        for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
            if (e.th32ProcessID == GetCurrentProcessId()) { parent = e.th32ParentProcessID; break; }
        bool launcher = false;
        e.dwSize = sizeof(e);
        for (BOOL ok = parent ? Process32FirstW(snap, &e) : FALSE; ok; ok = Process32NextW(snap, &e))
            if (e.th32ProcessID == parent) { launcher = !_wcsicmp(e.szExeFile, L"modlauncher.exe"); break; }
        CloseHandle(snap);
        return launcher;
    }

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

    // the launcher shows stdout, so user messages go there
    void Say(const char* text)
    {
        fprintf(stdout, "[BetterBuild] %s\n", text);
        fflush(stdout);
    }

    int Fail(const char* what, DWORD error)
    {
        fprintf(stdout, "[BetterBuild] %s (Win32 %lu).\n", what, error);
        fflush(stdout);
        return error ? (int)error : 1;
    }

    int Run(const std::wstring& real, const std::vector<std::wstring>& args, bool patches, const std::wstring& pluginDir)
    {
        std::wstring line = Quote(real);
        for (const std::wstring& arg : args) line += L" " + Quote(arg);
        std::vector<wchar_t> command(line.begin(), line.end());
        command.push_back(0);

        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits))
        {
            const DWORD error = GetLastError();
            if (job) CloseHandle(job);
            return Fail("Cannot create the cancellation job", error);
        }

        STARTUPINFOW startup = {};
        startup.cb = sizeof startup;
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION child = {};
        auto start = [&]() -> DWORD {
            if (!CreateProcessW(real.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &startup, &child))
                return GetLastError();
            if (!AssignProcessToJobObject(job, child.hProcess))
            {
                const DWORD error = GetLastError();
                TerminateProcess(child.hProcess, 1);
                CloseHandle(child.hThread); CloseHandle(child.hProcess);
                return error ? error : 1;
            }
            return 0;
        };
        if (const DWORD error = start())
        {
            CloseHandle(job);
            return Fail("Could not start Radiant for the light bake", error);
        }

        if (patches)
        {
            if (!inject::Sha256Equals(real, kRadiantSha256))
                Say("Unknown Radiant_modtools.exe build, lighting with stock code.");
            else if (inject::Install(child, pluginDir + L"\\BuildPlusLightRT.dll", "BuildPlusLightStart", 1) != 1)
            {
                Say("Light improvements could not be installed (antivirus or a changed DLL?), lighting with stock code.");
                TerminateProcess(child.hProcess, ERROR_DLL_INIT_FAILED);
                WaitForSingleObject(child.hProcess, INFINITE);
                CloseHandle(child.hThread); CloseHandle(child.hProcess);
                child = {};
                if (const DWORD error = start())
                {
                    CloseHandle(job);
                    return Fail("Could not start Radiant for the light bake", error);
                }
            }
        }
        else Say("Lighting through BetterBuild with stock code ([light] patches=0).");
        if (ResumeThread(child.hThread) == DWORD(-1))
        {
            const DWORD error = GetLastError();
            TerminateProcess(child.hProcess, 1);
            CloseHandle(child.hThread); CloseHandle(child.hProcess); CloseHandle(job);
            return Fail("Could not resume Radiant", error);
        }
        CloseHandle(child.hThread);
        WaitForSingleObject(child.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(child.hProcess, &exitCode);
        CloseHandle(child.hProcess);
        CloseHandle(job);
        return (int)exitCode;
    }
}

int wmain(int argc, wchar_t** argv)
{
    std::wstring real, lowRays;
    int separator = -1;
    for (int i = 1; i < argc; ++i)
    {
        if (!_wcsicmp(argv[i], L"--real") && i + 1 < argc) { real = argv[++i]; continue; }
        if (!_wcsicmp(argv[i], L"--low-rays") && i + 1 < argc) { lowRays = argv[++i]; continue; }   // 64 = draft tier for a +low bake
        if (!wcscmp(argv[i], L"--")) { separator = i; break; }
    }
    if (real.empty() || separator < 0)
    {
        fwprintf(stderr, L"usage: BuildPlusLight [--low-rays 64] --real <Radiant_modtools.exe> -- <Radiant arguments>\n");
        return 2;
    }
    std::vector<std::wstring> args(argv + separator + 1, argv + argc);

    std::vector<wchar_t> module(32768);
    GetModuleFileNameW(nullptr, module.data(), (DWORD)module.size());
    const std::wstring pluginDir = Parent(module.data());
    const std::wstring ini = pluginDir + L"\\LauncherPlus.ini";
    const bool patches = GetPrivateProfileIntW(L"light", L"patches", 1, ini.c_str()) != 0;
    // Launcher+ puts Draft in front of Low in the quality box, so the launcher's flag is one step off: use the recorded pick
    // Other callers keep their own flag and bake Low at 256 rays unless they pass --low-rays 64 (BuildPlusLightRT reads BUILDPLUS_LOW_RAYS)
    wchar_t quality[16] = {};
    if (FromLauncher()) GetPrivateProfileStringW(L"light", L"quality", L"", quality, 16, ini.c_str());
    SetEnvironmentVariableW(L"BUILDPLUS_LOW_RAYS", !_wcsicmp(quality, L"draft") || lowRays == L"64" ? L"64" : L"256");
    const wchar_t* flag = !_wcsicmp(quality, L"draft") || !_wcsicmp(quality, L"low") ? L"+low" : !_wcsicmp(quality, L"medium") ? L"+medium" : !_wcsicmp(quality, L"high") ? L"+high" : nullptr;
    if (flag)
        for (std::wstring& arg : args)
            if (!_wcsicmp(arg.c_str(), L"+low") || !_wcsicmp(arg.c_str(), L"+medium") || !_wcsicmp(arg.c_str(), L"+high")) { arg = flag; break; }
    return Run(real, args, patches, pluginDir);
}
