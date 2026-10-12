// Techset include search cache, see filesearch.h. The finder std::string* 0x140298180(ctx, out, subdir, name, recurse) stats
// <root><subdir><name> and on a miss lists every subdirectory and recurses, so only a thread's outermost call is cached.
#include "modtools_plugin.hpp"
#include "filesearch.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>

namespace ba::filesearch
{
namespace
{
    const uintptr_t kFind       = 0x140298180;   // the finder, called by the techset include loader only
    const uintptr_t kAssign     = 0x14002F176;   // std::string::assign(const char*, size_t) thunk
    const uintptr_t kScanThread = 0x14118D9D8;   // the techsetdef scan QThread (a global object), started by apeMainWindow::Init

    using FindFn = void* (__fastcall*)(void* ctx, void* out, const void* subdir, const void* name, char recurse);
    using AssignFn = void* (__fastcall*)(void* str, const char* text, size_t length);
    using IsRunningFn = bool (__fastcall*)(const void* thread);
    FindFn g_oFind = nullptr;
    AssignFn g_assign = nullptr;
    IsRunningFn g_isRunning = nullptr;
    void* g_thread = nullptr;

    bool g_enabled = true;          // [startup] techset_search_cache
    bool g_verify = false;          // test: run the finder anyway and compare
    bool g_hooked = false;
    std::atomic<bool> g_open{false};
    thread_local int t_depth = 0;

    SRWLOCK g_lock = SRWLOCK_INIT;
    std::unordered_map<std::string, std::string> g_cache;

    std::atomic<long long> g_calls{0}, g_hits{0}, g_misses{0}, g_mismatch{0}, g_ticks{0};
    LONG64 g_freq = 1;
    ULONGLONG g_openedAt = 0;
    std::string g_lastReport = "the scan had not finished";

    // MSVC 2012 std::string: 16-byte buffer or pointer at +0, size at +16, capacity at +24.
    bool ReadString(const void* s, std::string& out)
    {
        __try
        {
            const uint8_t* p = static_cast<const uint8_t*>(s);
            const uint64_t size = *reinterpret_cast<const uint64_t*>(p + 16);
            const uint64_t cap = *reinterpret_cast<const uint64_t*>(p + 24);
            if (size > 4096 || cap < size) return false;
            const char* text = cap >= 16 ? *reinterpret_cast<const char* const*>(p) : reinterpret_cast<const char*>(p);
            if (!text) return false;
            out.assign(text, size_t(size));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void* Construct(void* out, const std::string& text)
    {
        uint64_t* o = static_cast<uint64_t*>(out);
        o[3] = 15; o[2] = 0; *reinterpret_cast<char*>(o) = 0;   // an empty std::string, as the finder starts it
        if (!text.empty()) g_assign(out, text.data(), text.size());
        return out;
    }

    void* Original(void* ctx, void* out, const void* subdir, const void* name, char recurse)
    {
        ++t_depth;
        void* r = g_oFind(ctx, out, subdir, name, recurse);
        --t_depth;
        return r;
    }

    void* __fastcall hk_Find(void* ctx, void* out, const void* subdir, const void* name, char recurse)
    {
        if (!g_enabled || t_depth > 0 || !g_open.load(std::memory_order_relaxed)) return Original(ctx, out, subdir, name, recurse);
        std::string key, b;
        if (!ReadString(subdir, key) || !ReadString(name, b)) return Original(ctx, out, subdir, name, recurse);
        key += '\x01'; key += b; key += recurse ? '1' : '0';
        ++g_calls;
        bool known = false;
        std::string cached;
        AcquireSRWLockShared(&g_lock);
        auto it = g_cache.find(key);
        if (it != g_cache.end()) { known = true; cached = it->second; }
        ReleaseSRWLockShared(&g_lock);
        if (known && !g_verify) { ++g_hits; return Construct(out, cached); }

        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        void* r = Original(ctx, out, subdir, name, recurse);
        QueryPerformanceCounter(&t1);
        std::string result;
        const bool readable = ReadString(out, result);
        if (known)
        {
            ++g_hits;
            if (!readable || result != cached) ++g_mismatch;
            return r;
        }
        ++g_misses;
        g_ticks += t1.QuadPart - t0.QuadPart;
        if (!readable) return r;
        AcquireSRWLockExclusive(&g_lock);
        g_cache[key] = result;
        ReleaseSRWLockExclusive(&g_lock);
        return r;
    }

    bool ScanRunning()
    {
        __try { return g_isRunning && g_thread && g_isRunning(g_thread); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void Close()
    {
        if (!g_open.exchange(false)) return;
        AcquireSRWLockExclusive(&g_lock);
        const size_t answers = g_cache.size();
        g_cache.clear();
        ReleaseSRWLockExclusive(&g_lock);
        char line[400];
        snprintf(line, sizeof line, "scan finished %.1f s after the plugin loaded: %lld searches, %lld from the cache, %lld run in %.2f s, %zu answers%s",
                 double(GetTickCount64() - g_openedAt) / 1000.0, g_calls.load(), g_hits.load(), g_misses.load(),
                 double(g_ticks.load()) / double(g_freq), answers,
                 g_verify ? (", verify mismatches " + std::to_string(g_mismatch.load())).c_str() : "");
        g_lastReport = line;
        rp::Log("filesearch: %s", line);
    }
}

void Install()
{
    const std::string mode = rp::ConfigGet("startup", "techset_search_cache", "1");
    g_enabled = mode != "0";
    g_verify = mode == "verify";
    LARGE_INTEGER qpf; QueryPerformanceFrequency(&qpf); g_freq = qpf.QuadPart ? qpf.QuadPart : 1;
    if (g_hooked || !rp::Ape()) return;
    void* find = rp::Ape()->ApeAddress(rp::self, kFind);
    g_assign = reinterpret_cast<AssignFn>(rp::Ape()->ApeAddress(rp::self, kAssign));
    g_thread = rp::Ape()->ApeAddress(rp::self, kScanThread);
    HMODULE core = GetModuleHandleW(L"Qt5Core64r.dll");
    g_isRunning = core ? reinterpret_cast<IsRunningFn>(GetProcAddress(core, "?isRunning@QThread@@QEBA_NXZ")) : nullptr;
    DWORD64 base = 0; PRUNTIME_FUNCTION fn = find ? RtlLookupFunctionEntry(DWORD64(find), &base, nullptr) : nullptr;
    if (!fn || DWORD64(fn->BeginAddress) + base != DWORD64(find) || !g_assign || !g_thread || !g_isRunning)
    { rp::Log("filesearch: techset finder not found, stock search"); return; }
    if (!rp::host->Detour(rp::self, find, reinterpret_cast<void*>(&hk_Find), reinterpret_cast<void**>(&g_oFind)))
    { rp::Log("filesearch: detour failed"); return; }
    g_hooked = true;
    const bool running = ScanRunning();
    if (running) { g_openedAt = GetTickCount64(); g_open = true; }
    else g_lastReport = "the scan had finished before the plugin loaded";
    rp::Log("filesearch: techset search cache enabled=%d verify=%d, scan %s", g_enabled ? 1 : 0, g_verify ? 1 : 0, running ? "running, cache open" : "already done");
}

void Tick()
{
    if (g_open.load(std::memory_order_relaxed) && !ScanRunning()) Close();
}

bool Command(const std::string& c, std::string& reply)
{
    if (c.rfind("filesearch", 0) != 0 || (c.size() > 10 && c[10] != ' ')) return false;
    const std::string arg = c.size() > 11 ? c.substr(11) : "status";
    if (arg == "0" || arg == "1")
    {
        g_enabled = arg == "1";
        g_verify = false;
        rp::ConfigSet("startup", "techset_search_cache", arg);
    }
    else if (arg == "verify") { g_enabled = true; g_verify = true; }
    else if (arg != "status") { reply = "filesearch [status|0|1|verify]"; return true; }
    reply = std::string("filesearch: hooked=") + (g_hooked ? "1" : "0") + " enabled=" + (g_enabled ? "1" : "0") + " verify=" + (g_verify ? "1" : "0")
          + " open=" + (g_open ? "1" : "0") + "; " + g_lastReport;
    return true;
}
}
