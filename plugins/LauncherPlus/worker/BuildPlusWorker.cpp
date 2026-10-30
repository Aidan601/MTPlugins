// BuildPlusWorker.exe: transparent cod2map64 wrapper started by BuildPlusBridge.dll: [--no-skip] --real <cod2map64.exe> -- <compiler arguments>
// Output, working directory and exit code are the compiler's own. It runs in a kill-on-close job, so Cancel kills cod2map too.
#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>
#include "inject.h"
#include "smartents.h"
#include "compileskip.h"
#include "decals.h"
#include "../../common/physprim.h"

namespace
{
    std::wstring Parent(const std::wstring& p) { return p.substr(0, p.find_last_of(L"\\/")); }

    void Say(const char* text);

    // Physics primitives (../../common/physprim.h): cod2map stops at the first physics_box or physics_cylinder in a
    // level map. If the map has any, the compiler reads a scratch copy with them turned into brushes, removed after the run.
    struct PrimitiveCopy
    {
        std::wstring dir, map, settings;
        ~PrimitiveCopy()
        {
            if (!settings.empty()) DeleteFileW(settings.c_str());
            if (!map.empty()) DeleteFileW(map.c_str());
            if (!dir.empty()) RemoveDirectoryW(dir.c_str());
        }
        // args: the -loadFrom path is swapped for the copy when one was written
        void Prepare(std::vector<std::wstring>& args)
        {
            size_t at = args.size();
            for (size_t i = 0; i + 1 < args.size(); ++i) if (!_wcsicmp(args[i].c_str(), L"-loadFrom")) { at = i + 1; break; }
            if (at >= args.size()) return;
            const std::wstring src = args[at];
            HANDLE f = CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f == INVALID_HANDLE_VALUE) return;
            LARGE_INTEGER size = {};
            std::string text;
            if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (1LL << 31))
            {
                text.resize((size_t)size.QuadPart);
                DWORD got = 0;
                if (!ReadFile(f, &text[0], (DWORD)text.size(), &got, nullptr) || got != text.size()) text.clear();
            }
            CloseHandle(f);
            if (!physprim::Contains(text)) return;
            physprim::Result r;
            const std::string converted = physprim::Convert(text, &r);
            if (!r.boxes && !r.cylinders) return;
            const size_t slash = src.find_last_of(L"\\/");
            const std::wstring folder = slash == std::wstring::npos ? L"." : src.substr(0, slash);
            const std::wstring name = slash == std::wstring::npos ? src : src.substr(slash + 1);
            dir = folder + L"\\__buildplus_primitives";
            CreateDirectoryW(dir.c_str(), nullptr);
            map = dir + L"\\" + name;
            HANDLE out = CreateFileW(map.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD wrote = 0;
            const bool ok = out != INVALID_HANDLE_VALUE && WriteFile(out, converted.data(), (DWORD)converted.size(), &wrote, nullptr) && wrote == converted.size();
            if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
            if (!ok) { Say("Could not write the copy of the map with its physics primitives as brushes; compiling the map as it is."); return; }
            const size_t dot = name.find_last_of(L'.');
            const std::wstring stem = dot == std::wstring::npos ? name : name.substr(0, dot);
            const std::wstring nav = folder + L"\\" + stem + L"_navmesh_settings.json";
            if (GetFileAttributesW(nav.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                settings = dir + L"\\" + stem + L"_navmesh_settings.json";
                if (!CopyFileW(nav.c_str(), settings.c_str(), FALSE)) settings.clear();
            }
            args[at] = map;
            char line[400];
            snprintf(line, sizeof line, "Physics primitives: %zu box(es) and %zu cylinder(s) compiled as ordinary brushes (cod2map refuses them in a level map; the .map is unchanged)%s.",
                     r.boxes, r.cylinders, r.failed ? ", some could not be read and stay as they are" : "");
            Say(line);
        }
    };

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

    bool HasOption(const std::vector<std::wstring>& args, const wchar_t* option)
    {
        for (const std::wstring& arg : args) if (!_wcsicmp(arg.c_str(), option)) return true;
        return false;
    }

    // The launcher shows the compiler's stdout, so every line meant for the user goes there, flushed at once.
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

    struct Options
    {
        bool preferPerformance = true;
        bool patches = false;
        bool verify = false;
        bool timing = false;
        bool smartOnlyEnts = true;
        int skipUnchanged = 0;   // 0 off, 1 on, 2 verify (decide and report, compile anyway)
    };

    // One cod2map run: returns its exit code, or a negative value after a wrapper-side failure that Fail() reported.
    int RunCompiler(const std::wstring& real, const std::vector<std::wstring>& args, const Options& opt, const std::wstring& pluginDir)
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
            return -Fail("Cannot create the cancellation job", error);
        }

        STARTUPINFOW startup = {};
        startup.cb = sizeof startup;
        startup.dwFlags = STARTF_USESTDHANDLES;
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
                return error ? error : 1;
            }
            if (opt.preferPerformance)
            {
                PROCESS_POWER_THROTTLING_STATE power = {};
                power.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
                power.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
                SetProcessInformation(child.hProcess, ProcessPowerThrottling, &power, sizeof power);
            }
            return 0;
        };
        if (const DWORD error = start())
        {
            CloseHandle(job);
            return -Fail("Could not start cod2map64", error);
        }
        auto abort = [&](const char* what, DWORD error) {
            TerminateProcess(child.hProcess, error ? error : 1);
            CloseHandle(child.hThread); CloseHandle(child.hProcess); CloseHandle(job);
            return -Fail(what, error);
        };

        if (opt.patches)
        {
            if (!inject::Sha256Equals(real, inject::kCompilerSha256))
                Say("Unknown cod2map64.exe build, compiling with stock code.");
            else if (inject::Install(child, pluginDir + L"\\BuildPlusCompiler.dll", "BuildPlusCompilerStart", opt.verify ? 2 : 1) != 1)
            {
                // The suspended compiler may hold a half-set-up DLL: throw it away and run a clean stock one.
                Say("Compiler optimizations could not be installed (antivirus or a changed DLL?), compiling with stock code.");
                TerminateProcess(child.hProcess, ERROR_DLL_INIT_FAILED);
                WaitForSingleObject(child.hProcess, INFINITE);
                CloseHandle(child.hThread); CloseHandle(child.hProcess);
                child = {};
                if (const DWORD error = start())
                {
                    CloseHandle(job);
                    return -Fail("Could not start cod2map64", error);
                }
            }
        }
        if (ResumeThread(child.hThread) == DWORD(-1)) return abort("Could not resume cod2map64", GetLastError());
        CloseHandle(child.hThread);
        WaitForSingleObject(child.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(child.hProcess, &exitCode);
        CloseHandle(child.hProcess);
        CloseHandle(job);
        return (int)exitCode;
    }

    int Run(const std::wstring& real, std::vector<std::wstring> args, const Options& opt, const std::wstring& pluginDir)
    {
        char banner[256];
        snprintf(banner, sizeof banner, "Compiling through BetterBuild: optimizations %s, prefer_performance %s, timing %s.",
                 !opt.patches ? "off (stock cod2map code)" : opt.verify ? "on, verify mode (stock results kept)" : "on",
                 opt.preferPerformance ? "on" : "off", opt.timing ? "on" : "off");
        Say(banner);
        if (opt.timing && !HasOption(args, L"-timing")) args.insert(args.begin(), L"-timing");

        // Smart compile: -onlyents when only entities changed since the last compile of this output (smartents.h).
        const bool skipCheck = opt.skipUnchanged != 0 && opt.patches;
        smartents::Plan plan = smartents::Decide(real, args, pluginDir, opt.smartOnlyEnts, skipCheck);
        PrimitiveCopy primitives;   // after Decide: the smart compile keys on the user's .map, the compiler reads the copy
        primitives.Prepare(args);
        if (plan.callerOnlyEnts)
        {
            // PlayTest's own -onlyents compile: run it as asked, but repair what cod2map's path breaks (decals.h)
            decals::Keeper keeper;
            keeper.Capture(plan.output);
            const int code = RunCompiler(real, args, opt, pluginDir);
            if (code < 0) return -code;
            size_t records = 0;
            if (code == 0)
            {
                const int restored = keeper.Restore(plan.output, records);
                if (restored > 0) { snprintf(banner, sizeof banner, "Restored the reveal UVs of %zu volume decals that cod2map -onlyents zeroes.", records); Say(banner); }
                else if (restored < 0) Say("Warning: the volume decal lump changed in this -onlyents compile; a full compile rebuilds it correctly.");
            }
            return code;
        }

        // Skip unchanged: no cod2map run at all when nothing the last compile read or wrote changed (compileskip.h).
        compileskip::Job skip;
        std::string skipWhy;
        unchanged::State skipOld;
        if (skipCheck)
        {
            skip = compileskip::Prepare(real, pluginDir, plan);
            if (skip.applicable)
            {
                const ULONGLONG t0 = GetTickCount64();
                size_t checked = 0;
                skipWhy = compileskip::Decide(skip, checked);
                const double seconds = (GetTickCount64() - t0) / 1000.0;
                if (skipWhy.empty() && opt.skipUnchanged == 1)
                {
                    snprintf(banner, sizeof banner, "Compile skipped: nothing it reads changed since the last compile (%zu files checked in %.1f s), "
                             "the BSP and nav mesh are kept. skip_unchanged=0 in LauncherPlus.ini always compiles.", checked, seconds);
                    Say(banner);
                    return 0;
                }
                const std::string line = skipWhy.empty() ? "Skip unchanged (verify mode): nothing changed, compiling anyway to compare" : "Compile needed: " + skipWhy;
                snprintf(banner, sizeof banner, "%s (checked in %.1f s).", line.substr(0, 200).c_str(), seconds);
                Say(banner);
                if (GetEnvironmentVariableW(L"BB_SKIP_DECIDE_ONLY", nullptr, 0)) return 0;   // tests: the decision only
                if (skipWhy.empty()) skipOld.Load(skip.statePath);
                CreateDirectoryW(Parent(skip.statePath).c_str(), nullptr);
                SetEnvironmentVariableW(L"BB_COMPILE_JOURNAL", skip.journal.c_str());
            }
            else
            {
                snprintf(banner, sizeof banner, "Skip unchanged: not checked (%s).", skip.reason.substr(0, 200).c_str());
                Say(banner);
            }
        }
        const std::string bspBefore = smartents::Identity(plan.output);
        const unsigned long long start = compileskip::Now();
        auto compile = [&](const std::vector<std::wstring>& a) {
            if (skip.applicable) DeleteFileW(skip.journal.c_str());
            return RunCompiler(real, a, opt, pluginDir);
        };
        auto finish = [&](int code) {
            if (!skip.applicable) return code;
            SetEnvironmentVariableW(L"BB_COMPILE_JOURNAL", nullptr);
            const std::string kept = compileskip::Remember(skip, bspBefore, start, code);
            if (skipWhy.empty())
            {
                const std::string diff = "Skip unchanged verify: " + compileskip::Compare(skip, skipOld, start) + (kept.empty() ? "." : ", not recorded: " + kept + ".");
                Say(diff.c_str());
            }
            else if (!kept.empty() && code == 0)
                Say(("Skip unchanged: this compile is not recorded (" + kept + "), the next one runs too.").c_str());
            DeleteFileW(skip.journal.c_str());
            return code;
        };

        if (opt.smartOnlyEnts)
        {
            if (plan.onlyEnts)
            {
                snprintf(banner, sizeof banner, "Smart compile: %s since the last compile, running cod2map -onlyents "
                         "(geometry, collision and nav mesh kept; smart_onlyents=0 in LauncherPlus.ini compiles fully).", plan.reason.c_str());
                Say(banner);
                decals::Keeper keeper;
                keeper.Capture(plan.output);
                std::vector<std::wstring> quick = args;
                quick.insert(quick.begin(), L"-onlyents");
                const int code = compile(quick);
                if (code < 0) return finish(-code);
                size_t records = 0;
                const int restored = code == 0 && !smartents::Identity(plan.output).empty() ? keeper.Restore(plan.output, records) : -1;
                if (restored >= 0)
                {
                    if (restored > 0) { snprintf(banner, sizeof banner, "Smart compile: restored the reveal UVs of %zu volume decals that cod2map -onlyents zeroes.", records); Say(banner); }
                    smartents::Remember(plan);
                    return finish(0);
                }
                Say(code == 0 ? "Smart compile: the volume decal lump did not come back as expected, compiling fully instead."
                              : "Smart compile: the -onlyents run failed, compiling fully instead.");
                smartents::Forget(plan);
            }
            else
            {
                snprintf(banner, sizeof banner, "Smart compile: full compile (%s).", plan.reason.c_str());
                Say(banner);
            }
        }
        const int code = compile(args);
        if (code < 0) return finish(-code);
        if (plan.applicable)
        {
            if (code == 0) smartents::Remember(plan);
            else smartents::Forget(plan);
        }
        return finish(code);
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
        fwprintf(stderr, L"usage: BuildPlusWorker [--no-skip] --real <cod2map64.exe> -- <compiler arguments>\n");
        return 2;
    }
    std::vector<std::wstring> args(argv + separator + 1, argv + argc);

    std::vector<wchar_t> module(32768);
    GetModuleFileNameW(nullptr, module.data(), (DWORD)module.size());
    const std::wstring pluginDir = Parent(module.data());
    const std::wstring ini = pluginDir + L"\\LauncherPlus.ini";
    Options opt;
    opt.preferPerformance = GetPrivateProfileIntW(L"compile", L"prefer_performance", 1, ini.c_str()) != 0;
    opt.patches = GetPrivateProfileIntW(L"compile", L"patches", 1, ini.c_str()) != 0;
    opt.verify = GetPrivateProfileIntW(L"compile", L"verify_patches", 0, ini.c_str()) != 0;
    opt.timing = GetPrivateProfileIntW(L"compile", L"timing", 0, ini.c_str()) != 0;
    opt.smartOnlyEnts = GetPrivateProfileIntW(L"compile", L"smart_onlyents", 1, ini.c_str()) != 0;
    {
        wchar_t value[32] = {};
        GetPrivateProfileStringW(L"compile", L"skip_unchanged", L"1", value, 32, ini.c_str());
        opt.skipUnchanged = noSkip ? 0 : !_wcsicmp(value, L"verify") ? 2 : _wtoi(value) ? 1 : 0;
    }
    return Run(real, std::move(args), opt, pluginDir);
}
