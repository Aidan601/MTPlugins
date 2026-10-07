// Injected by BuildPlusLight.exe into a suspended "Radiant_modtools.exe -ledSilent". Hosts RadiantPlus's bake module with
// MinHook (plugins never load in -ledSilent mode), configured by LauncherPlus.ini [light]. adaptive, cone, cone_edge and
// guide fall back to RadiantPlus.ini [bake] when empty. Writes BuildPlusLight.log, and .rgp dumps when dump_dir is set.
#include <windows.h>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include "../../../src/third_party/minhook/include/MinHook.h"
#include "../../RadiantPlus/bake_guiding.h"
#include "../../RadiantPlus/bake_fill.h"
#include "../../RadiantPlus/bake_gather.h"
#include "../../RadiantPlus/bake_tracecache.h"
#include "../../RadiantPlus/bake_lowrays.h"
#include "light_prefetch.h"

namespace
{
    const uintptr_t kImageBase = 0x140000000;   // no ASLR, the wrapper checked the SHA-256
    const uintptr_t kUpdateGI  = 0x1402F38B0;   // CCamWnd::UpdateGI(CCamWnd*, char tickMode), the export loop calls it with 0
    const uintptr_t kInflight  = 0x145581E54;   // int: probe jobs in flight
    // the -ledSilent handler CmdLine_forceclean: bool(app, QList<QString>* args), returns 0 to end the process
    const uintptr_t kLedHandler = 0x14064C540;
    const uintptr_t kExitCode   = 0x145216998;   // int, written by 0x140B899A0 (255 on any failed map, else 0)
    // bake state: volumes vector cam+6408 (16 bytes each), per volume the
    // light states' bake state at +1164 (3 sun, 4 bounces, 5 smooth, 6 final, 7/8 done), probes vector cam+6664 (112 bytes each)
    const size_t off_Cam_LightState = 3960, off_Cam_Volumes = 6408, off_Cam_Bounce = 6660, off_Cam_Probes = 6664;
    const size_t off_Volume_State = 1164;

    using UpdateGIFn = int64_t(__fastcall*)(uint8_t*, char);
    UpdateGIFn g_oUpdateGI = nullptr;
    uint8_t* volatile g_cam = nullptr;
    std::wstring g_ini, g_logPath;
    HANDLE g_log = INVALID_HANDLE_VALUE;
    SRWLOCK g_logLock = SRWLOCK_INIT;
    bool g_started = false;
    volatile bool g_exiting = false;

    std::wstring Parent(const std::wstring& p) { return p.substr(0, p.find_last_of(L"\\/")); }

    void Write(HANDLE h, const std::string& text)
    {
        if (h == INVALID_HANDLE_VALUE || !h) return;
        DWORD written = 0;
        WriteFile(h, text.data(), DWORD(text.size()), &written, nullptr);
    }
    // lines for the launcher's log (Radiant's stdout)
    void Say(const std::string& text) { Write(GetStdHandle(STD_OUTPUT_HANDLE), "[BetterBuild] " + text + "\n"); }

    void Log(const char* line)
    {
        // at exit other threads are gone, possibly holding the lock: never wait
        if (g_exiting) { if (!TryAcquireSRWLockExclusive(&g_logLock)) return; }
        else AcquireSRWLockExclusive(&g_logLock);
        SYSTEMTIME t; GetLocalTime(&t);
        char stamp[32];
        snprintf(stamp, sizeof stamp, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        Write(g_log, std::string(stamp) + line + "\r\n");
        ReleaseSRWLockExclusive(&g_logLock);
    }

    std::wstring Wide(const char* s) { std::wstring w; while (s && *s) w += wchar_t(static_cast<unsigned char>(*s++)); return w; }   // ASCII keys
    std::string Ansi(const std::wstring& w)
    {
        const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string out(n > 0 ? size_t(n) : 1, '\0');
        if (n > 0) WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, &out[0], n, nullptr, nullptr);
        out.resize(strlen(out.c_str()));
        return out;
    }
    // keys that change the result follow RadiantPlus.ini [bake] so the editor and the launcher bake alike, a non-empty [light] value overrides
    struct SharedKey { const char* key; const char* fallback; };   // fallback: bake_guiding.cpp's default
    const SharedKey kShared[] = { { "adaptive", "map" }, { "cone", "1" }, { "cone_edge", "1" }, { "guide", "0" } };
    std::wstring g_brIni;   // <plugins>\RadiantPlus\RadiantPlus.ini

    const SharedKey* Shared(const char* key)
    {
        for (const SharedKey& s : kShared) if (!strcmp(s.key, key)) return &s;
        return nullptr;
    }
    std::string IniValue(const std::wstring& ini, const wchar_t* section, const char* key)
    {
        wchar_t buf[1024];
        GetPrivateProfileStringW(section, Wide(key).c_str(), L"", buf, 1024, ini.c_str());
        return Ansi(buf);
    }
    std::string Get(const char* key, const char* fallback)
    {
        if (!strcmp(key, "low_rays"))   // set by BuildPlusLight.exe from the launcher's Draft pick
        {
            wchar_t rays[16] = {};
            return GetEnvironmentVariableW(L"BUILDPLUS_LOW_RAYS", rays, 16) ? Ansi(rays) : std::string("256");
        }
        if (const SharedKey* s = Shared(key))
        {
            std::string v = IniValue(g_ini, L"light", key);
            if (v.empty()) v = IniValue(g_brIni, L"bake", key);
            return v.empty() ? s->fallback : v;
        }
        wchar_t buf[1024];
        GetPrivateProfileStringW(L"light", Wide(key).c_str(), Wide(fallback).c_str(), buf, 1024, g_ini.c_str());
        return Ansi(buf);
    }
    std::string Norm(std::string v)
    {
        for (char& c : v) c = char(tolower(static_cast<unsigned char>(c)));
        if (v == "true" || v == "on" || v == "yes") return "1";
        if (v == "false" || v == "off" || v == "no") return "0";
        return v;
    }
    // log every [light] override that differs from the editor's setting
    void SayOverrides()
    {
        for (const SharedKey& s : kShared)
        {
            const std::string light = IniValue(g_ini, L"light", s.key);
            if (light.empty()) continue;
            std::string editor = IniValue(g_brIni, L"bake", s.key);
            if (editor.empty()) editor = s.fallback;
            if (Norm(light) != Norm(editor))
                Say(std::string("LauncherPlus.ini [light] ") + s.key + "=" + light + " overrides RadiantPlus's " + s.key + "=" + editor +
                    ": this bake will not match the editor's Build Lighting.");
        }
    }
    void Set(const char* key, const char* value) { WritePrivateProfileStringW(L"light", Wide(key).c_str(), Wide(value).c_str(), g_ini.c_str()); }

    void* Address(uintptr_t ida) { return reinterpret_cast<void*>(ida); }
    bool Detour(void* target, void* detour, void** original)
    {
        if (MH_CreateHook(target, detour, original) != MH_OK) return false;
        if (MH_EnableHook(target) != MH_OK) { MH_RemoveHook(target); return false; }
        return true;
    }

    // [light] timeline=1: a log line per bake state change and a heartbeat every 2 s (jobs in flight, process CPU)
    bool g_timeline = false;
    std::string g_lastState;
    ULONGLONG g_lastBeat = 0, g_start = 0;
    ULONGLONG g_cpu0 = 0, g_wall0 = 0;
    ULONGLONG ProcessCpuMs()
    {
        FILETIME c, e, k, u;
        if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
        return ((ULONGLONG(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (ULONGLONG(u.dwHighDateTime) << 32 | u.dwLowDateTime)) / 10000;
    }
    void Timeline(uint8_t* cam)
    {
        char line[512];
        int n = snprintf(line, sizeof line, "state light=%d bounce=%d", *reinterpret_cast<int*>(cam + off_Cam_LightState), *reinterpret_cast<int*>(cam + off_Cam_Bounce));
        uint8_t* vb = *reinterpret_cast<uint8_t**>(cam + off_Cam_Volumes);
        uint8_t* ve = *reinterpret_cast<uint8_t**>(cam + off_Cam_Volumes + 8);
        if (vb && ve > vb && ve - vb <= 16 * 32)
            for (uint8_t* v = vb; v < ve && n < int(sizeof line) - 32; v += 16)
                if (uint8_t* vol = *reinterpret_cast<uint8_t**>(v))
                {
                    const int* st = reinterpret_cast<const int*>(vol + off_Volume_State);
                    n += snprintf(line + n, sizeof line - n, " [%d %d %d %d]", st[0], st[1], st[2], st[3]);
                }
        const ULONGLONG now = GetTickCount64();
        if (!g_start) { g_start = now; g_wall0 = now; g_cpu0 = ProcessCpuMs(); }
        if (g_lastState != line)
        {
            g_lastState = line;
            char out[640];
            snprintf(out, sizeof out, "timeline %.1f s: %s", double(now - g_start) / 1000.0, line);
            Log(out);
        }
        if (now - g_lastBeat >= 2000)
        {
            const ULONGLONG cpu = ProcessCpuMs();
            uint8_t* pb = *reinterpret_cast<uint8_t**>(cam + off_Cam_Probes);
            uint8_t* pe = *reinterpret_cast<uint8_t**>(cam + off_Cam_Probes + 8);
            char out[256];
            snprintf(out, sizeof out, "timeline %.1f s: beat inflight=%d probes=%lld cores=%.1f", double(now - g_start) / 1000.0,
                     *reinterpret_cast<volatile int*>(kInflight), (pb && pe >= pb) ? (long long)((pe - pb) / 112) : -1LL,
                     now > g_wall0 ? double(cpu - g_cpu0) / double(now - g_wall0) : 0.0);
            Log(out);
            g_lastBeat = now; g_wall0 = now; g_cpu0 = cpu;
        }
    }

    // [light] fast_exit=1: after the export write the summary, flush every CRT stream (Radiant's and ours) and end
    // the process instead of Radiant's teardown. The .led is closed before "LED Exported" is printed.
    using LedHandlerFn = char(__fastcall*)(void*, void*);
    LedHandlerFn g_oLedHandler = nullptr;
    bool g_fastExit = true;
    void Finish();
    char __fastcall hk_LedHandler(void* app, void* args)
    {
        br::bakeguide::ApplyRenderOptions();   // after Radiant resets the render options, before the export
        const char keepRunning = g_oLedHandler(app, args);
        const int code = *reinterpret_cast<volatile int*>(kExitCode);
        char line[128];
        snprintf(line, sizeof line, "export finished, exit code %d%s", code, !keepRunning && g_fastExit ? ", fast exit" : "");
        Log(line);
        if (keepRunning || !g_fastExit) return keepRunning;
        Finish();
        using FlushAllFn = int(__cdecl*)();
        if (HMODULE crt = GetModuleHandleW(L"MSVCR110.dll"))
            if (auto flush = reinterpret_cast<FlushAllFn>(GetProcAddress(crt, "_flushall"))) flush();
        fflush(nullptr);
        TerminateProcess(GetCurrentProcess(), UINT(code));
        return keepRunning;
    }

    int64_t __fastcall hk_UpdateGI(uint8_t* cam, char tickMode)
    {
        if (!g_cam)
        {
            // the map is loaded: the load's opens are what the next Light step prefetches
            const std::string prefetch = lightpf::Save();
            if (!prefetch.empty()) { Log(prefetch.c_str()); Say(prefetch + "."); }
        }
        g_cam = cam;
        const int64_t r = g_oUpdateGI(cam, tickMode);
        if (g_timeline && cam) { try { Timeline(cam); } catch (...) {} }
        return r;
    }
    uint8_t* CamWnd() { return g_cam; }
}

extern "C" __declspec(dllexport) DWORD WINAPI BuildPlusLightStart(void*)
{
    try
    {
        if (g_started) return 1;
        if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != kImageBase) return 0;
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&BuildPlusLightStart), &self);
        wchar_t path[MAX_PATH * 2] = {};
        if (!self || !GetModuleFileNameW(self, path, MAX_PATH * 2)) return 0;
        const std::wstring dir = Parent(path);
        g_ini = dir + L"\\LauncherPlus.ini";
        g_brIni = Parent(dir) + L"\\RadiantPlus\\RadiantPlus.ini";
        g_logPath = dir + L"\\BuildPlusLight.log";
        g_log = CreateFileW(g_logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (MH_Initialize() != MH_OK) return 0;
        if (Get("prefetch_files", "1") == "1") lightpf::Start(dir, &Log);   // first: the map load starts when we return
        if (!Detour(Address(kUpdateGI), reinterpret_cast<void*>(&hk_UpdateGI), reinterpret_cast<void**>(&g_oUpdateGI))) return 0;
        br::bakeguide::Env env{};
        env.address = &Address;
        env.detour = &Detour;
        env.log = &Log;
        env.get = &Get;
        env.set = &Set;
        env.camWnd = &CamWnd;
        env.running = nullptr;
        // the draft tier changes the ray tables of every quality, so only Low bakes get it
        if (wcsstr(GetCommandLineW(), L"+low")) br::bakell::Setup(env);
        br::bakeguide::Setup(env);
        br::bakefill::Setup(env);
        br::bakegather::Setup(env);
        br::baketrace::Setup(env, dir + L"\\trace_cache");
        g_timeline = Get("timeline", "0") == "1";
        g_fastExit = Get("fast_exit", "1") == "1";
        if (!Detour(Address(kLedHandler), reinterpret_cast<void*>(&hk_LedHandler), reinterpret_cast<void**>(&g_oLedHandler)))
        { g_fastExit = false; Log("fast exit unavailable (hook failed)"); }
        g_started = true;
        const std::string adaptive = Get("adaptive", "map"), cone = Get("cone", "1");
        Say("Lighting through BetterBuild: adaptive sampling " + (adaptive == "map" ? std::string("as set on each probe") : "at least " + adaptive) +
            ", cone correction " + (Norm(cone) == "1" ? "on" : "off") + ".");
        SayOverrides();
        return 1;
    }
    catch (...) { return 0; }
}

namespace
{
    // one summary line for the launcher log, from the fast exit or at DLL_PROCESS_DETACH
    void Finish()
    {
        static volatile LONG done = 0;
        if (!g_started || InterlockedExchange(&done, 1)) return;
        const std::string summary = br::bakeguide::Summary();
        Log(summary.c_str());
        Log(br::bakefill::Status().c_str());
        Log(br::bakegather::Status().c_str());
        Log(br::baketrace::Status().c_str());
        Log(br::bakell::Status().c_str());
        const size_t cut = summary.find("; gather jobs");
        Say("Light bake: " + (cut == std::string::npos ? summary : summary.substr(0, cut)) + ".");
        if (g_log != INVALID_HANDLE_VALUE) FlushFileBuffers(g_log);
    }
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_DETACH && g_started)
    {
        g_exiting = true;
        Finish();
    }
    return TRUE;
}
