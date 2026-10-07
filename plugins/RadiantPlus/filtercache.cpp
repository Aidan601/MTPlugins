// Filter cache, see filtercache.h. Two exact changes to the Filters-pane evaluation (0x140591AD0): the entity key leaf test (0x1405944A0) kept per
// (owner, revision, node) inside one BrushCache_Update pass, and the name pattern test (0x140594BF0) compared in place.
// `filtercache verify` runs Radiant's own functions next to both and counts any difference.
#include "modtools_plugin.hpp"
#include "filtercache.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace br::filtercache
{
namespace
{
    // IDA addresses (no ASLR), resolved through host->RadiantAddress
    const uintptr_t kKeyTest   = 0x1405944A0;   // expression leaf 5: entity key/value test (brush_t*, node*)
    const uintptr_t kNameMatch = 0x140594BF0;   // pattern test (pattern*, const char* name)
    const size_t off_Brush_Owner   = 16;
    const size_t off_Entity_Rev    = 320;       // revision ints (+320, +328, +332 move on edits)
    const size_t off_Entity_Keys   = 304;       // key/value std::map: head node +0, size +8 (in the identity: memory can be reused in one pass)
    const size_t off_Pattern_Text  = 0;
    const size_t off_Pattern_Flags = 8;

    using KeyTestFn = __int64 (__fastcall*)(const void* brush, const void* node);
    using NameMatchFn = __int64 (__fastcall*)(const void* pattern, const char* name);

    bool g_enabled = true;                      // [performance] filter_cache
    bool g_verify = false;                      // test: run Radiant's own next to both, count differences
    KeyTestFn g_oKeyTest = nullptr;
    NameMatchFn g_oNameMatch = nullptr;
    bool g_keyHook = false, g_matchHook = false;

    // Each key node gets a dense index per pass (small open-addressing table), each owner an answer array by that
    // index (-1 = not asked yet), looked up only when the owner changes. A hash map per test cost as much as the test.
    const int kMaxNodes = 512;
    const int kNodeSlots = 2048;                // power of two
    struct NodeSlot { const void* node; int index; uint32_t stamp; };
    NodeSlot g_nodes[kNodeSlots];
    int      g_nodeCount = 0;
    struct OwnerAnswers { const void* owner; const void* keys; int rev[4]; int8_t answer[kMaxNodes]; };
    std::vector<OwnerAnswers*> g_pool;          // reused between passes
    size_t   g_poolUsed = 0;
    std::unordered_map<const void*, OwnerAnswers*> g_owners;
    OwnerAnswers* g_cur = nullptr;              // the last owner asked about
    uint32_t g_stamp = 1;                       // bumped to empty the node table

    DWORD g_passThread = 0;                     // the thread inside BrushCache_Update, 0 = no pass
    int   g_passDepth = 0;

    void ClearTable()
    {
        g_nodeCount = 0;
        if (++g_stamp == 0) { for (NodeSlot& n : g_nodes) n.stamp = 0; g_stamp = 1; }
        g_owners.clear();
        g_poolUsed = 0;
        g_cur = nullptr;
    }

    int NodeIndex(const void* node)
    {
        uint32_t i = uint32_t((reinterpret_cast<uintptr_t>(node) >> 4) * 0x9E3779B1u) & (kNodeSlots - 1);
        for (int probe = 0; probe < kNodeSlots; ++probe, i = (i + 1) & (kNodeSlots - 1))
        {
            NodeSlot& n = g_nodes[i];
            if (n.stamp != g_stamp)
            {
                if (g_nodeCount >= kMaxNodes) return -1;
                n.node = node; n.index = g_nodeCount++; n.stamp = g_stamp;
                return n.index;
            }
            if (n.node == node) return n.index;
        }
        return -1;
    }

    OwnerAnswers* Answers(const void* owner, const void* keys, const int rev[4])
    {
        if (g_cur && g_cur->owner == owner && g_cur->keys == keys && g_cur->rev[0] == rev[0] && g_cur->rev[1] == rev[1] && g_cur->rev[2] == rev[2] && g_cur->rev[3] == rev[3])
            return g_cur;
        OwnerAnswers*& slot = g_owners[owner];
        if (!slot)
        {
            if (g_poolUsed == g_pool.size()) g_pool.push_back(new OwnerAnswers);
            slot = g_pool[g_poolUsed++];
            slot->owner = owner;
            slot->rev[0] = rev[0] - 1;          // forces the reset below
        }
        if (slot->keys != keys || slot->rev[0] != rev[0] || slot->rev[1] != rev[1] || slot->rev[2] != rev[2] || slot->rev[3] != rev[3])
        {
            slot->keys = keys;
            slot->rev[0] = rev[0]; slot->rev[1] = rev[1]; slot->rev[2] = rev[2]; slot->rev[3] = rev[3];
            std::memset(slot->answer, -1, sizeof slot->answer);
        }
        g_cur = slot;
        return slot;
    }

    uint64_t g_passes = 0, g_keyCalls = 0, g_keyHits = 0, g_keyBypass = 0, g_matchCalls = 0;
    uint64_t g_keyMismatch = 0, g_matchMismatch = 0, g_faults = 0;
    double   g_lastPassMs = 0, g_maxPassMs = 0;
    LARGE_INTEGER g_passBegin = {};

    // brush->owner and its revision ints, false without an owner or on a fault
    bool ReadOwner(const void* brush, const void*& owner, const void*& keys, int rev[4])
    {
        __try
        {
            const uint8_t* const o = *reinterpret_cast<const uint8_t* const*>(static_cast<const uint8_t*>(brush) + off_Brush_Owner);
            if (!o) return false;
            const int* const r = reinterpret_cast<const int*>(o + off_Entity_Rev);
            owner = o;
            keys = *reinterpret_cast<const void* const*>(o + off_Entity_Keys);
            rev[0] = r[0]; rev[1] = r[2]; rev[2] = r[3];
            rev[3] = int(*reinterpret_cast<const uint64_t*>(o + off_Entity_Keys + 8));   // key count
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ++g_faults;
            return false;
        }
    }

    __int64 __fastcall hk_KeyTest(const void* brush, const void* node)
    {
        if (!g_enabled) return g_oKeyTest(brush, node);
        if (!g_passThread || g_passThread != GetCurrentThreadId() || !brush || !node)
        {
            ++g_keyBypass;
            return g_oKeyTest(brush, node);
        }
        ++g_keyCalls;
        const void* owner = nullptr;
        const void* keys = nullptr;
        int rev[4];
        if (!ReadOwner(brush, owner, keys, rev))
        {
            ++g_keyBypass;
            return g_oKeyTest(brush, node);
        }
        const int idx = NodeIndex(node);
        if (idx < 0) return g_oKeyTest(brush, node);
        OwnerAnswers* const answers = Answers(owner, keys, rev);
        const int8_t known = answers->answer[idx];
        if (known >= 0)
        {
            ++g_keyHits;
            if (!g_verify) return known;
            const __int64 real = g_oKeyTest(brush, node);
            if (uint8_t(real) != uint8_t(known)) ++g_keyMismatch;
            return real;
        }
        const __int64 r = g_oKeyTest(brush, node);
        answers->answer[idx] = int8_t(uint8_t(r) != 0 ? 1 : 0);
        return r;
    }

    // Radiant's pattern test without the std::string copy of the name.
    bool Matches(const char* pattern, unsigned flags, const char* name)
    {
        if ((flags & 1) && (flags & 2)) return std::strstr(name, pattern) != nullptr;
        if (!(flags & 1) && !(flags & 2)) return std::strcmp(name, pattern) == 0;
        const size_t lp = std::strlen(pattern), ln = std::strlen(name);
        if (lp > ln) return false;
        if (flags & 1) return std::memcmp(name, pattern, lp) == 0;    // prefix
        return std::memcmp(name + (ln - lp), pattern, lp) == 0;       // suffix
    }

    __int64 __fastcall hk_NameMatch(const void* pattern, const char* name)
    {
        if (!g_enabled || !pattern || !name) return g_oNameMatch(pattern, name);
        ++g_matchCalls;
        bool mine;
        __try
        {
            const uint8_t* const p = static_cast<const uint8_t*>(pattern);
            const char* const text = *reinterpret_cast<const char* const*>(p + off_Pattern_Text);
            if (!text) return g_oNameMatch(pattern, name);
            mine = Matches(text, *reinterpret_cast<const unsigned*>(p + off_Pattern_Flags), name);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ++g_faults;
            return g_oNameMatch(pattern, name);
        }
        if (g_verify)
        {
            const __int64 real = g_oNameMatch(pattern, name);
            if ((uint8_t(real) != 0) != mine) ++g_matchMismatch;
            return real;
        }
        return mine ? 1 : 0;
    }

    std::string Status()
    {
        char b[640];
        snprintf(b, sizeof b,
                 "filter_cache=%d%s hooks key=%d match=%d | passes %llu, last %.1f ms, longest %.1f ms | key tests %llu in passes, "
                 "%llu answered from the cache (%.1f%%), %llu outside | name tests %llu | verify differences key=%llu name=%llu faults=%llu",
                 g_enabled ? 1 : 0, g_verify ? " VERIFY" : "", g_keyHook ? 1 : 0, g_matchHook ? 1 : 0,
                 (unsigned long long)g_passes, g_lastPassMs, g_maxPassMs, (unsigned long long)g_keyCalls,
                 (unsigned long long)g_keyHits, g_keyCalls ? 100.0 * double(g_keyHits) / double(g_keyCalls) : 0.0,
                 (unsigned long long)g_keyBypass, (unsigned long long)g_matchCalls,
                 (unsigned long long)g_keyMismatch, (unsigned long long)g_matchMismatch, (unsigned long long)g_faults);
        return b;
    }
}

void Setup()
{
    const std::string mode = rp::ConfigGet("performance", "filter_cache", "1");
    g_enabled = mode != "0";
    g_verify = mode == "verify";
    void* const key = rp::host->RadiantAddress(rp::self, kKeyTest);
    void* const match = rp::host->RadiantAddress(rp::self, kNameMatch);
    g_keyHook = key && rp::host->Detour(rp::self, key, reinterpret_cast<void*>(&hk_KeyTest), reinterpret_cast<void**>(&g_oKeyTest));
    g_matchHook = match && rp::host->Detour(rp::self, match, reinterpret_cast<void*>(&hk_NameMatch), reinterpret_cast<void**>(&g_oNameMatch));
    rp::Log("filtercache: enabled=%d hooks key=%d match=%d", g_enabled ? 1 : 0, g_keyHook ? 1 : 0, g_matchHook ? 1 : 0);
}

void PassBegin()
{
    const DWORD self = GetCurrentThreadId();
    if (g_passDepth++ > 0) return;          // nested: the outer pass owns the cache
    g_passThread = self;
    ClearTable();
    QueryPerformanceCounter(&g_passBegin);
}

void PassEnd()
{
    if (g_passDepth <= 0 || --g_passDepth > 0) return;
    g_passThread = 0;
    ClearTable();
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    g_lastPassMs = double(now.QuadPart - g_passBegin.QuadPart) * 1000.0 / double(freq.QuadPart);
    if (g_lastPassMs > g_maxPassMs) g_maxPassMs = g_lastPassMs;
    ++g_passes;
}

bool Command(const std::string& c, std::string& reply)
{
    if (c.rfind("filtercache", 0) != 0 || (c.size() > 11 && c[11] != ' ')) return false;
    const std::string arg = c.size() > 12 ? c.substr(12) : "status";
    if (arg == "0" || arg == "1")
    {
        g_enabled = arg == "1";
        g_verify = false;
        rp::ConfigSet("performance", "filter_cache", arg);
    }
    else if (arg == "verify")
    {
        g_enabled = true;
        g_verify = true;    // not saved
    }
    else if (arg == "reset")
    {
        g_passes = g_keyCalls = g_keyHits = g_keyBypass = g_matchCalls = g_keyMismatch = g_matchMismatch = g_faults = 0;
        g_lastPassMs = g_maxPassMs = 0;
    }
    else if (arg != "status")
    {
        reply = "filtercache [status|0|1|verify|reset]";
        return true;
    }
    reply = Status();
    return true;
}
}
