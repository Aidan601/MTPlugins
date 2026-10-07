// Source file search cache, see filesearch.h. The finder std::string* 0x140F28F90(ctx, out, subdir, name, recurse) constructs out (found path or empty).
// With recurse it calls itself per subdirectory, so only a thread's outermost call is cached. A hit builds out with Radiant's own
// std::string assign (0x140057B9E) so the string lives in Radiant's heap.
#include "modtools_plugin.hpp"
#include "filesearch.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace br::filesearch
{
namespace
{
    const uintptr_t kFind   = 0x140F28F90;   // the finder
    const uintptr_t kAssign = 0x140057B9E;   // std::string::assign(const char*, size_t) thunk

    using FindFn = void* (__fastcall*)(void* ctx, void* out, const void* subdir, const void* name, char recurse);
    using AssignFn = void* (__fastcall*)(void* str, const char* text, size_t length);
    FindFn g_oFind = nullptr;
    AssignFn g_assign = nullptr;

    bool g_enabled = true;          // [performance] file_search_cache
    bool g_verify = false;          // test: run the finder anyway and compare
    std::atomic<bool> g_scope{false};
    thread_local int t_depth = 0;

    struct Entry { std::string result; long hits = 0; double ms = 0; };
    SRWLOCK g_lock = SRWLOCK_INIT;
    std::unordered_map<std::string, Entry> g_cache;

    std::atomic<long long> g_calls{0}, g_hits{0}, g_misses{0}, g_mismatch{0}, g_found{0}, g_notFound{0}, g_ticks{0};
    LONG64 g_freq = 1;
    std::string g_lastReport = "no map loaded yet";

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

    void* __fastcall hk_Find(void* ctx, void* out, const void* subdir, const void* name, char recurse)
    {
        if (!g_enabled || !g_assign || t_depth > 0 || !g_scope.load(std::memory_order_relaxed))
        {
            ++t_depth;
            void* r = g_oFind(ctx, out, subdir, name, recurse);
            --t_depth;
            return r;
        }
        std::string a, b;
        if (!ReadString(subdir, a) || !ReadString(name, b))
        {
            ++t_depth;
            void* r = g_oFind(ctx, out, subdir, name, recurse);
            --t_depth;
            return r;
        }
        std::string key = a;
        key += '\x01'; key += b; key += recurse ? '1' : '0';
        ++g_calls;
        bool known = false;
        std::string cached;
        AcquireSRWLockExclusive(&g_lock);
        auto it = g_cache.find(key);
        if (it != g_cache.end()) { known = true; cached = it->second.result; ++it->second.hits; }
        ReleaseSRWLockExclusive(&g_lock);
        if (known && !g_verify)
        {
            ++g_hits;
            return Construct(out, cached);
        }

        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        ++t_depth;
        void* r = g_oFind(ctx, out, subdir, name, recurse);
        --t_depth;
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
        if (result.empty()) ++g_notFound; else ++g_found;
        AcquireSRWLockExclusive(&g_lock);
        Entry& e = g_cache[key];
        e.result = result;
        e.ms = 1000.0 * double(t1.QuadPart - t0.QuadPart) / double(g_freq);
        ReleaseSRWLockExclusive(&g_lock);
        return r;
    }

    std::string Printable(const std::string& key)
    {
        std::string s = key;
        for (char& c : s) if (c == '\x01') c = '|';
        return s;
    }
}

void Setup()
{
    const std::string mode = rp::ConfigGet("performance", "file_search_cache", "1");
    g_enabled = mode != "0";
    g_verify = mode == "verify";
    LARGE_INTEGER qpf; QueryPerformanceFrequency(&qpf); g_freq = qpf.QuadPart ? qpf.QuadPart : 1;
    g_assign = reinterpret_cast<AssignFn>(rp::host->RadiantAddress(rp::self, kAssign));
    void* find = rp::host->RadiantAddress(rp::self, kFind);
    const bool hooked = find && g_assign && rp::host->Detour(rp::self, find, reinterpret_cast<void*>(&hk_Find), reinterpret_cast<void**>(&g_oFind)) != 0;
    rp::Log("filesearch: file search cache enabled=%d verify=%d hook=%d", g_enabled ? 1 : 0, g_verify ? 1 : 0, hooked ? 1 : 0);
}

void LoadBegin()
{
    AcquireSRWLockExclusive(&g_lock);
    g_cache.clear();
    ReleaseSRWLockExclusive(&g_lock);
    g_calls = 0; g_hits = 0; g_misses = 0; g_mismatch = 0; g_found = 0; g_notFound = 0; g_ticks = 0;
    g_scope = true;
}

void LoadEnd()
{
    if (!g_scope.exchange(false)) return;
    char line[400];
    snprintf(line, sizeof line, "%lld searches, %lld answered from the cache, %lld run (%lld found, %lld not found) in %.2f s%s",
             g_calls.load(), g_hits.load(), g_misses.load(), g_found.load(), g_notFound.load(), double(g_ticks.load()) / double(g_freq),
             g_verify ? (" ; verify mismatches " + std::to_string(g_mismatch.load())).c_str() : "");
    g_lastReport = line;
    rp::Log("filesearch: %s", line);
    AcquireSRWLockExclusive(&g_lock);
    std::vector<std::pair<double, const std::pair<const std::string, Entry>*>> top;
    for (auto& kv : g_cache) top.push_back({ kv.second.ms * (1 + kv.second.hits), &kv });
    std::sort(top.begin(), top.end(), [](auto& x, auto& y) { return x.first > y.first; });
    for (size_t i = 0; i < top.size() && i < 8; ++i)
        rp::Log("filesearch:   %.1f ms x %ld more: %s -> %s", top[i].second->second.ms, top[i].second->second.hits,
                Printable(top[i].second->first).c_str(), top[i].second->second.result.empty() ? "(not found)" : top[i].second->second.result.c_str());
    g_cache.clear();
    ReleaseSRWLockExclusive(&g_lock);
}

bool Command(const std::string& c, std::string& reply)
{
    if (c.rfind("filesearch", 0) != 0 || (c.size() > 10 && c[10] != ' ')) return false;
    const std::string arg = c.size() > 11 ? c.substr(11) : "status";
    if (arg == "0" || arg == "1")
    {
        g_enabled = arg == "1";
        g_verify = false;
        rp::ConfigSet("performance", "file_search_cache", arg);
    }
    else if (arg == "verify") { g_enabled = true; g_verify = true; }
    else if (arg != "status")
    {
        reply = "filesearch [status|0|1|verify]";
        return true;
    }
    reply = std::string("filesearch: enabled=") + (g_enabled ? "1" : "0") + " verify=" + (g_verify ? "1" : "0") + "; last load: " + g_lastReport;
    return true;
}
}
