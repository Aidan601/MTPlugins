// linker_modtools wrapper started by BuildPlusBridge.dll. The linker (and its localized child) runs in a kill-on-close job.
// Usage: BuildPlusLinker.exe [--no-skip] --real <linker_modtools.exe> -- <linker arguments>
// With [linker] patches=1 and a known linker build, BuildPlusLinkerRT.dll is loaded into the suspended linker.
#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>
#include "../worker/inject.h"
#include "linkskip.h"
#include "xpakclean.h"
#include "assetreport.h"

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
        int skipUnchanged = 0;   // 0 off, 1 on, 2 verify (decide and report, link anyway)
        bool compactXpak = true;
        uintptr_t mode = 0;
    };

    int Link(const std::wstring& real, const std::vector<std::wstring>& args, const Options& opt, const std::wstring& pluginDir,
             unchanged::Tap* tap);

    int Run(const std::wstring& real, const std::vector<std::wstring>& args, Options opt, const std::wstring& pluginDir)
    {
        if (!opt.skipUnchanged || !opt.patches) return Link(real, args, opt, pluginDir, nullptr);
        linkskip::Job job = linkskip::Prepare(real, args, pluginDir, (unsigned)opt.mode);
        if (!job.applicable)
        {
            char line[200];
            snprintf(line, sizeof line, "Skip unchanged: not checked (%s).", job.reason.c_str());
            Say(line);
            return Link(real, args, opt, pluginDir, nullptr);
        }
        const ULONGLONG t0 = GetTickCount64();
        size_t checked = 0;
        const std::string why = linkskip::Decide(job, checked);
        const double seconds = (GetTickCount64() - t0) / 1000.0;
        std::string line;
        if (why.empty() && opt.skipUnchanged == 1)
        {
            char buf[400];
            snprintf(buf, sizeof buf, "Link skipped: nothing it reads changed since the last link (%zu files checked in %.1f s), zone\\%s.ff kept. "
                     "skip_unchanged=0 in LauncherPlus.ini always links.", checked, seconds, localized::Narrow(job.map).c_str());
            Say(buf);
            linkskip::Replay(job);
            return 0;
        }
        line = why.empty() ? "Skip unchanged (verify mode): nothing changed, linking anyway to compare" : "Linking: " + why;
        char tail[64];
        snprintf(tail, sizeof tail, " (checked in %.1f s).", seconds);
        Say((line + tail).c_str());
        if (GetEnvironmentVariableW(L"BB_SKIP_DECIDE_ONLY", nullptr, 0)) return 0;   // tests: the decision only
        unchanged::State old;
        if (why.empty()) old.Load(job.statePath);

        DeleteFileW(job.journal.c_str());
        CreateDirectoryW(job.stateDir.c_str(), nullptr);
        SetEnvironmentVariableW(L"BB_LINK_JOURNAL", job.journal.c_str());
        opt.mode |= linkskip::kJournalBit;
        bool xpakAbsent = false;
        if (opt.compactXpak)
        {
            const std::string cleaned = xpakclean::Before(job.mapDir, job.map, job.stateDir, xpakAbsent);
            if (!cleaned.empty()) Say(cleaned.c_str());
        }
        unchanged::Tap tap;
        const bool tapped = tap.Start();
        const linkskip::Before before = linkskip::Snapshot(job);
        const int code = Link(real, args, opt, pluginDir, tapped ? &tap : nullptr);
        const bool complete = tapped && tap.Finish();
        SetEnvironmentVariableW(L"BB_LINK_JOURNAL", nullptr);
        const std::string kept = linkskip::Remember(job, before, tap, complete, (DWORD)code);
        if (opt.compactXpak && kept.empty()) xpakclean::After(job.mapDir, job.map, job.stateDir, xpakAbsent);   // a link good enough to record
        if (kept.empty())
        {
            const std::string content = assetreport::Line(job.mapDir, job.map, job.language, job.stateDir);
            if (!content.empty()) Say(content.c_str());
        }
        if (why.empty())
        {
            const std::string diff = linkskip::Compare(job, old, before);
            Say(("Skip unchanged verify: " + diff + (kept.empty() ? "." : ", not recorded: " + kept + ".")).c_str());
        }
        else if (!kept.empty()) Say(("Skip unchanged: this link is not recorded (" + kept + "), the next one runs too.").c_str());
        DeleteFileW(job.journal.c_str());
        return code;
    }

    int Link(const std::wstring& real, const std::vector<std::wstring>& args, const Options& opt, const std::wstring& pluginDir,
             unchanged::Tap* tap)
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
        startup.hStdOutput = tap ? tap->out.write : GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = tap ? tap->err.write : GetStdHandle(STD_ERROR_HANDLE);
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
        if (tap) tap->ChildStarted();
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
    bool noSkip = false;   // --no-skip: no skip_unchanged decision and no record (PlayTest's forced and throwaway builds)
    for (int i = 1; i < argc; ++i)
    {
        if (!_wcsicmp(argv[i], L"--real") && i + 1 < argc) { real = argv[++i]; continue; }
        if (!_wcsicmp(argv[i], L"--no-skip")) { noSkip = true; continue; }
        if (!wcscmp(argv[i], L"--")) { separator = i; break; }
    }
    if (real.empty() || separator < 0)
    {
        fwprintf(stderr, L"usage: BuildPlusLinker [--no-skip] --real <linker_modtools.exe> -- <linker arguments>\n");
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
    {
        wchar_t value[32] = {};
        GetPrivateProfileStringW(L"linker", L"skip_unchanged", L"1", value, 32, ini.c_str());
        opt.skipUnchanged = noSkip ? 0 : !_wcsicmp(value, L"verify") ? 2 : _wtoi(value) ? 1 : 0;
    }
    if (opt.verify) opt.mode |= 1u << 8;
    if (GetPrivateProfileIntW(L"linker", L"skip_localized", 1, ini.c_str())) opt.mode |= 1u << 9;
    if (GetPrivateProfileIntW(L"linker", L"skip_sound_convert", 1, ini.c_str())) opt.mode |= 1u << 10;
    if (GetPrivateProfileIntW(L"linker", L"autolod_fixes", 1, ini.c_str())) opt.mode |= (1u << 11) | (1u << 12);   // face limit, normals
    if (GetPrivateProfileIntW(L"linker", L"meshopt_lods", 0, ini.c_str())) opt.mode |= 1u << 13;
    if (GetPrivateProfileIntW(L"linker", L"cache_map_entities", 1, ini.c_str())) opt.mode |= 1u << 15;
    opt.compactXpak = GetPrivateProfileIntW(L"linker", L"compact_xpak", 1, ini.c_str()) != 0;
    // the runtime DLL reads the auto LOD budget from the environment the linker inherits
    const UINT autoLodBudget = GetPrivateProfileIntW(L"linker", L"autolod_budget_mb", 0, ini.c_str());
    if (autoLodBudget && !GetEnvironmentVariableW(L"BB_AUTOLOD_BUDGET_MB", nullptr, 0))
        SetEnvironmentVariableW(L"BB_AUTOLOD_BUDGET_MB", std::to_wstring(autoLodBudget).c_str());
    return Run(real, args, opt, pluginDir);
}
