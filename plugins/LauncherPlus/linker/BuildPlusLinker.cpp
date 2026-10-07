// linker_modtools wrapper started by BuildPlusBridge.dll. The linker (and its localized child) runs in a kill-on-close job.
// Usage: BuildPlusLinker.exe --real <linker_modtools.exe> -- <linker arguments>
// With [linker] patches=1 and a known linker build, BuildPlusLinkerRT.dll is loaded into the suspended linker.
#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>
#include "../worker/inject.h"

namespace
{
    // linker builds the runtime patches fit (T7 3421953), the exe is Treyarch-packed, L3akMod (bin\libtiff64r.dll) leaves it alone
    struct KnownLinker { const char* name; unsigned char sha256[32]; };
    const KnownLinker kLinkers[] = {
        { "stock T7 3421953 (with or without L3akMod)", { 0xed,0x40,0x8a,0xa9,0x7f,0x32,0x3c,0x93,0x10,0x32,0x23,0x3a,0x66,0x3e,0x6d,0xdf,
                             0xb7,0xcb,0x68,0xd7,0x11,0x1b,0xe0,0x19,0x7e,0x94,0xe2,0x8c,0x12,0x31,0x86,0xd0 } },
    };

    std::wstring Parent(const std::wstring& p) { return p.substr(0, p.find_last_of(L"\\/")); }

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

    const KnownLinker* Identify(const std::wstring& real)
    {
        for (const KnownLinker& k : kLinkers)
            if (inject::Sha256Equals(real, k.sha256)) return &k;
        return nullptr;
    }

    struct Options
    {
        bool preferPerformance = true;
        bool patches = false;
        bool verify = false;
        uintptr_t mode = 0;
    };

    int Run(const std::wstring& real, const std::vector<std::wstring>& args, const Options& opt, const std::wstring& pluginDir)
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

        char banner[160];
        sprintf_s(banner, "Linking through BetterBuild: optimizations %s, prefer_performance %s.",
                  !opt.patches ? "off (stock linker code)" : opt.verify ? "on, verify mode (stock results kept)" : "on",
                  opt.preferPerformance ? "on" : "off");
        Say(banner);

        STARTUPINFOW startup = {};
        startup.cb = sizeof startup;
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_FORCEOFFFEEDBACK;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION child = {};
        const DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | (opt.preferPerformance ? ABOVE_NORMAL_PRIORITY_CLASS : 0);
        auto start = [&]() -> DWORD {
            if (!CreateProcessW(real.c_str(), command.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &startup, &child))
                return GetLastError();
            if (!AssignProcessToJobObject(job, child.hProcess))
            {
                const DWORD error = GetLastError();
                TerminateProcess(child.hProcess, 1);
                CloseHandle(child.hThread); CloseHandle(child.hProcess);
                child = {};
                return error;
            }
            return 0;
        };
        if (const DWORD error = start())
        {
            CloseHandle(job);
            return Fail("Could not start linker_modtools", error);
        }
        auto abort = [&](const char* what, DWORD error) {
            TerminateProcess(child.hProcess, error ? error : 1);
            CloseHandle(child.hThread); CloseHandle(child.hProcess); CloseHandle(job);
            return Fail(what, error);
        };
        if (opt.patches)
        {
            const std::wstring dll = pluginDir + L"\\BuildPlusLinkerRT.dll";
            if (!Identify(real))
                Say("Unknown linker_modtools.exe build, linking with stock code.");
            else if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES)
                Say("BuildPlusLinkerRT.dll is missing, linking with stock code.");
            else if (inject::Install(child, dll, "BuildPlusLinkerStart", opt.mode) != 1)
            {
                // the suspended linker may hold a half-set-up DLL: restart it clean
                Say("Linker optimizations could not be installed (antivirus or a changed DLL?), linking with stock code.");
                TerminateProcess(child.hProcess, ERROR_DLL_INIT_FAILED);
                WaitForSingleObject(child.hProcess, INFINITE);
                CloseHandle(child.hThread); CloseHandle(child.hProcess);
                child = {};
                if (const DWORD error = start())
                {
                    CloseHandle(job);
                    return Fail("Could not start linker_modtools", error);
                }
            }
        }
        if (opt.preferPerformance)
        {
            PROCESS_POWER_THROTTLING_STATE power = {};
            power.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
            power.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
            SetProcessInformation(child.hProcess, ProcessPowerThrottling, &power, sizeof power);
        }
        if (ResumeThread(child.hThread) == DWORD(-1)) return abort("Could not resume linker_modtools", GetLastError());
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
    std::wstring real;
    int separator = -1;
    for (int i = 1; i < argc; ++i)
    {
        if (!_wcsicmp(argv[i], L"--real") && i + 1 < argc) { real = argv[++i]; continue; }
        if (!wcscmp(argv[i], L"--")) { separator = i; break; }
    }
    if (real.empty() || separator < 0)
    {
        fwprintf(stderr, L"usage: BuildPlusLinker --real <linker_modtools.exe> -- <linker arguments>\n");
        return 2;
    }
    std::vector<std::wstring> args(argv + separator + 1, argv + argc);

    std::vector<wchar_t> module(32768);
    GetModuleFileNameW(nullptr, module.data(), (DWORD)module.size());
    const std::wstring pluginDir = Parent(module.data());
    const std::wstring ini = pluginDir + L"\\LauncherPlus.ini";
    Options opt;
    opt.preferPerformance = GetPrivateProfileIntW(L"linker", L"prefer_performance", 1, ini.c_str()) != 0;
    // started below normal priority (PlayTest's live material link): stay in the background
    const DWORD ownClass = GetPriorityClass(GetCurrentProcess());
    if (ownClass == BELOW_NORMAL_PRIORITY_CLASS || ownClass == IDLE_PRIORITY_CLASS) opt.preferPerformance = false;
    opt.patches = GetPrivateProfileIntW(L"linker", L"patches", 1, ini.c_str()) != 0;
    // patch bits for BuildPlusLinkerRT.dll (keep in sync with its enum)
    opt.mode = 0;
    if (GetPrivateProfileIntW(L"linker", L"skip_telemetry", 1, ini.c_str())) opt.mode |= 1u << 0;
    if (GetPrivateProfileIntW(L"linker", L"umbra_poll", 1, ini.c_str())) opt.mode |= 1u << 1;
    if (GetPrivateProfileIntW(L"linker", L"reuse_d3d_device", 1, ini.c_str())) opt.mode |= 1u << 2;
    if (GetPrivateProfileIntW(L"linker", L"parallel_mip", 1, ini.c_str())) opt.mode |= 1u << 3;
    if (GetPrivateProfileIntW(L"linker", L"reuse_wic", 1, ini.c_str())) opt.mode |= 1u << 4;
    if (GetPrivateProfileIntW(L"linker", L"prefetch_files", 1, ini.c_str())) opt.mode |= 1u << 5;
    if (GetPrivateProfileIntW(L"linker", L"fast_stat", 1, ini.c_str())) opt.mode |= 1u << 6;
    if (GetPrivateProfileIntW(L"linker", L"fast_recenter", 1, ini.c_str())) opt.mode |= 1u << 7;
    opt.verify = GetPrivateProfileIntW(L"linker", L"verify_patches", 0, ini.c_str()) != 0;
    if (opt.verify) opt.mode |= 1u << 8;
    if (GetPrivateProfileIntW(L"linker", L"skip_localized", 1, ini.c_str())) opt.mode |= 1u << 9;
    if (GetPrivateProfileIntW(L"linker", L"skip_sound_convert", 1, ini.c_str())) opt.mode |= 1u << 10;
    return Run(real, args, opt, pluginDir);
}
