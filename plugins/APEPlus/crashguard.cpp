// APE's assert box made survivable, see crashguard.h.
// APE's box is fatal both ways (OK = exit(-1), Cancel = int 3), and its exit runs static destructors under a live renderer.
#include "modtools_plugin.hpp"
#include "crashguard.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

namespace ba::crashguard
{
namespace
{
    const uintptr_t kAssert = 0x1402A4FD0;   // (file, line, kind, expr, fmt, ...) -> nonzero = continue, callers int 3 on zero
    const uintptr_t kSlerp = 0x1402B2940;    // quat slerp (from xyzw, to xyzw, frac, out), sanity checks the result (com_math.cpp:2314)
    const uintptr_t kOpts = 0x14118D620;     // previewWindowOpts (pointer)
    const size_t kOptsTime = 0xB528;         // double: play position in seconds

    enum Mode { MODE_APE = 0, MODE_ASK = 1, MODE_CONTINUE = 2 };
    using AssertFn = __int64 (*)(const char* file, unsigned line, int kind, const char* expr, const char* fmt, ...);
    using ExitFn = void (*)(int code);
    using SlerpFn = void (*)(const float* from, const float* to, float frac, float* out);

    AssertFn g_target = nullptr, g_origAssert = nullptr;
    ExitFn g_origExit = nullptr;
    SlerpFn g_origSlerp = nullptr;
    bool g_hooked = false, g_exitHooked = false, g_slerpHooked = false;
    std::atomic<bool> g_slerpOn{true};
    std::atomic<long> g_slerpFixes{0}, g_timeFixes{0};
    int g_mode = MODE_ASK;
    std::atomic<bool> g_exiting{false};
    std::atomic<long> g_count{0};
    volatile LONG g_inBox = 0;

    struct Site { long hits = 0; bool continued = false; };
    std::mutex g_sitesMutex;
    std::map<std::string, Site> g_sites;   // "file:line"

    const char* ModeName(int m) { return m == MODE_APE ? "ape" : m == MODE_CONTINUE ? "continue" : "ask"; }
    int ParseMode(const std::string& s)
    {
        if (s == "ape" || s == "0") return MODE_APE;
        if (s == "continue" || s == "2") return MODE_CONTINUE;
        return MODE_ASK;
    }
    const char* KindName(int kind) { return kind == 0 ? "assertion" : kind == 1 ? "sanity check" : "internal error"; }

    // the callers as module+rva, so a log line names the code that fed the bad value
    std::string Stack()
    {
        void* frames[12] = {};
        USHORT n = RtlCaptureStackBackTrace(2, 12, frames, nullptr);
        std::string out;
        for (USHORT i = 0; i < n; ++i)
        {
            HMODULE mod = nullptr;
            char name[MAX_PATH] = "?";
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCWSTR>(frames[i]), &mod) && mod)
            {
                char path[MAX_PATH] = {};
                GetModuleFileNameA(mod, path, MAX_PATH);
                const char* base = strrchr(path, '\\');
                snprintf(name, sizeof name, "%s", base ? base + 1 : path);
            }
            char b[MAX_PATH + 32];
            snprintf(b, sizeof b, "%s%s+0x%llx", i ? " < " : "", name,
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(mod)));
            out += b;
        }
        return out;
    }

    bool AskContinue(const char* text)
    {
        int shown = 0;
        for (;;) { const int c = ShowCursor(TRUE); ++shown; if (c >= 0 || shown >= 8) break; }
        const int r = MessageBoxA(GetActiveWindow(), text, "APE internal check failed (APE+ crash guard)",
                                  MB_YESNO | MB_ICONERROR | MB_TASKMODAL | MB_SETFOREGROUND | MB_DEFBUTTON1);
        for (int i = 0; i < shown; ++i) ShowCursor(FALSE);
        return r != IDNO;
    }

    __int64 Hk_Assert(const char* file, unsigned line, int kind, const char* expr, const char* fmt, ...)
    {
        char msg[1024] = {};
        if (fmt && *fmt)
        {
            va_list va; va_start(va, fmt);
            _vsnprintf(msg, sizeof msg - 1, fmt, va);
            va_end(va);
        }
        char key[300];
        snprintf(key, sizeof key, "%s:%u", file ? file : "?", line);
        long hits = 0; bool continued = false;
        {
            std::lock_guard<std::mutex> lock(g_sitesMutex);
            Site& s = g_sites[key];
            hits = ++s.hits; continued = s.continued;
        }
        const long n = ++g_count;
        const bool exiting = g_exiting.load();
        // a check that fails every preview frame: the first 5 in full, then every 100th
        if (hits <= 5 || hits % 100 == 0)
            rp::Log("crashguard: APE %s #%ld (site hit %ld)%s: %s (%s)%s%s\n    stack: %s", KindName(kind), n, hits,
                    exiting ? " while APE exits" : "", expr ? expr : "?", key, msg[0] ? ": " : "", msg, Stack().c_str());
        if (IsDebuggerPresent()) return 0;                       // break into the debugger, as APE does
        if (exiting) return 1;                                   // static teardown after exit(): no box, no second exit
        if (g_mode == MODE_APE) return g_origAssert(file, line, kind, expr, "%s", msg);
        if (g_mode == MODE_CONTINUE || continued) return 1;
        if (InterlockedCompareExchange(&g_inBox, 1, 0) != 0) return 1;   // another assert while the box is up
        char text[2048];
        _snprintf(text, sizeof text - 1,
                  "APE hit an internal check (%s) and would normally close here.\n\n%s\n%s\n%s\n\n"
                  "Yes: keep working. Save your work now (File > Save), then restart APE. "
                  "This check will not ask again this session.\n"
                  "No: close APE now (unsaved work is lost).\n\n"
                  "Logged by the APE+ plugin (RadiantPlugins.ape.log).",
                  KindName(kind), expr ? expr : "?", msg, key);
        text[sizeof text - 1] = 0;
        const bool go = AskContinue(text);
        InterlockedExchange(&g_inBox, 0);
        if (!go)
        {
            rp::Log("crashguard: close chosen after the %s", KindName(kind));
            TerminateProcess(GetCurrentProcess(), 3);           // no static teardown, so no chain of de-init boxes
        }
        {
            std::lock_guard<std::mutex> lock(g_sitesMutex);
            g_sites[key].continued = true;
        }
        rp::Log("crashguard: continued past %s", key);
        return 1;
    }

    bool Finite4(const float* q) { return std::isfinite(q[0]) && std::isfinite(q[1]) && std::isfinite(q[2]) && std::isfinite(q[3]); }

    // a NaN fraction or key would fail APE's result check: the earlier key is used instead
    void Hk_Slerp(const float* from, const float* to, float frac, float* out)
    {
        const bool fromOk = Finite4(from), toOk = Finite4(to);
        if (!g_slerpOn || (fromOk && toOk && std::isfinite(frac))) { g_origSlerp(from, to, frac, out); return; }
        static const float kIdentity[4] = { 0.f, 0.f, 0.f, 1.f };
        const float* q = fromOk ? from : toOk ? to : kIdentity;
        const long n = ++g_slerpFixes;
        if (n <= 3 || n % 1000 == 0)
            rp::Log("crashguard: slerp #%ld got a NaN or infinite input (fraction %g, from %s, to %s), the %s rotation used\n    stack: %s",
                    n, frac, fromOk ? "ok" : "bad", toOk ? "ok" : "bad", fromOk ? "earlier" : toOk ? "later" : "identity", Stack().c_str());
        if (out != q) { out[0] = q[0]; out[1] = q[1]; out[2] = q[2]; out[3] = q[3]; }
    }

    void Hk_Exit(int code)
    {
        g_exiting = true;
        g_origExit(code);
    }

    bool StartsFunction(void* target)
    {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fn = target ? RtlLookupFunctionEntry(DWORD64(target), &base, nullptr) : nullptr;
        return fn && DWORD64(fn->BeginAddress) + base == DWORD64(target);
    }
}

void InstallSlerp();

void Install()
{
    InstallSlerp();
    g_mode = ParseMode(rp::ConfigGet("crashguard", "asserts", "ask"));
    if (g_mode == MODE_APE) { rp::Log("crashguard: asserts left to APE"); return; }
    g_target = rp::Ape() ? reinterpret_cast<AssertFn>(rp::Ape()->ApeAddress(rp::self, kAssert)) : nullptr;
    if (!g_target || !StartsFunction(reinterpret_cast<void*>(g_target)))
    {
        rp::Log("crashguard: APE's assert handler not found, guard off");
        return;
    }
    g_hooked = rp::host->Detour(rp::self, reinterpret_cast<void*>(g_target), reinterpret_cast<void*>(&Hk_Assert),
                                reinterpret_cast<void**>(&g_origAssert)) != 0;
    HMODULE crt = GetModuleHandleW(L"msvcr110.dll");
    void* exitFn = crt ? reinterpret_cast<void*>(GetProcAddress(crt, "exit")) : nullptr;
    g_exitHooked = exitFn && rp::host->Detour(rp::self, exitFn, reinterpret_cast<void*>(&Hk_Exit), reinterpret_cast<void**>(&g_origExit)) != 0;
    rp::Log("crashguard: assert guard %s (mode %s), exit watch %s", g_hooked ? "on" : "FAILED", ModeName(g_mode), g_exitHooked ? "on" : "off");
}

void InstallSlerp()
{
    g_slerpOn = rp::ConfigGetBool("crashguard", "slerp", true);
    void* slerp = rp::Ape() ? rp::Ape()->ApeAddress(rp::self, kSlerp) : nullptr;
    if (!slerp || !StartsFunction(slerp)) { rp::Log("crashguard: slerp not found, NaN guard off"); return; }
    g_slerpHooked = rp::host->Detour(rp::self, slerp, reinterpret_cast<void*>(&Hk_Slerp), reinterpret_cast<void**>(&g_origSlerp)) != 0;
    rp::Log("crashguard: slerp NaN guard %s%s", g_slerpHooked ? "hooked" : "FAILED", g_slerpOn ? "" : " (off by [crashguard] slerp=0)");
}

void BeforeFrame()
{
    void* opts = rp::Ape() ? rp::Ape()->ApeAddress(rp::self, kOpts) : nullptr;
    __try
    {
        uint8_t* o = opts ? *static_cast<uint8_t**>(opts) : nullptr;
        if (!o) return;
        double* t = reinterpret_cast<double*>(o + kOptsTime);
        if (std::isfinite(*t)) return;
        *t = 0.0;
        const long n = ++g_timeFixes;
        if (n <= 3 || n % 1000 == 0) rp::Log("crashguard: the preview's play position was NaN or infinite, set back to 0 (#%ld)", n);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void Shutdown()
{
    g_hooked = g_exitHooked = g_slerpHooked = false;   // the host removes the detours
}

bool Command(const std::string& command, std::string& reply)
{
    if (command.compare(0, 10, "crashguard") != 0) return false;
    std::string arg = command.size() > 10 ? command.substr(11) : std::string();
    if (arg == "ask" || arg == "continue" || arg == "ape")
    {
        g_mode = ParseMode(arg);
        rp::ConfigSet("crashguard", "asserts", arg);
        reply = "crashguard asserts " + arg + (g_hooked || g_mode == MODE_APE ? "" : " (saved, the hook installs on the next load)");
        return true;
    }
    if (arg == "slerp on" || arg == "slerp off")
    {
        g_slerpOn = arg == "slerp on";
        rp::ConfigSet("crashguard", "slerp", g_slerpOn ? "1" : "0");
        reply = std::string("crashguard slerp NaN guard ") + (g_slerpOn ? "on" : "off") + (g_slerpHooked ? "" : " (not hooked)");
        return true;
    }
    if (arg == "slerp test")
    {
        SlerpFn slerp = rp::Ape() ? reinterpret_cast<SlerpFn>(rp::Ape()->ApeAddress(rp::self, kSlerp)) : nullptr;
        if (!slerp) { reply = "no slerp"; return true; }
        const float from[4] = { 0.f, 0.f, 0.f, 1.f }, to[4] = { 0.f, 0.f, 0.70710678f, 0.70710678f };
        float out[4] = {}, half[4] = {};
        const long before = g_count.load();
        slerp(from, to, 0.5f, half);
        slerp(from, to, std::nanf(""), out);
        char b[256];
        snprintf(b, sizeof b, "slerp test: half way %.4f %.4f %.4f %.4f, NaN fraction -> %g %g %g %g, %ld asserts",
                 half[0], half[1], half[2], half[3], out[0], out[1], out[2], out[3], g_count.load() - before);
        reply = b;
        return true;
    }
    if (arg == "test")
    {
        if (!g_hooked) { reply = "crashguard is not hooked"; return true; }
        __int64 r = g_target("crashguard_test.cpp", 1, 1, "(crashguard test)", "%s", "a test assert from APE+");
        reply = std::string("test assert returned ") + (r ? "continue" : "break");
        return true;
    }
    reply = std::string("crashguard ") + (g_hooked ? "on" : "off") + ", mode " + ModeName(g_mode) + ", exit watch " +
            (g_exitHooked ? "on" : "off") + ", " + std::to_string(g_count.load()) + " asserts, slerp guard " +
            (g_slerpHooked && g_slerpOn ? "on" : "off") + " (" + std::to_string(g_slerpFixes.load()) + " fixed), play position fixed " +
            std::to_string(g_timeFixes.load()) + " times";
    std::lock_guard<std::mutex> lock(g_sitesMutex);
    for (const auto& s : g_sites)
        reply += "\n  " + s.first + ": " + std::to_string(s.second.hits) + (s.second.continued ? " (continued)" : "");
    if (arg.empty() || arg == "status") return true;
    reply = "crashguard [status|ask|continue|ape|test|slerp on|off]\n" + reply;
    return true;
}
}
