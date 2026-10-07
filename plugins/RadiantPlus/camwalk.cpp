// Prefetch for the 3D view's editor-graphics walk (game view off): the camera callback 0x1402AEE10 of BrushCache_ProcessRadBrushes (0x1402131D0, mask 0x40)
// prefetches the record, BrushInst, EntityInst, entity and class a few objects ahead along category vector 1. Only memory is read,
// nothing is skipped or reordered, and the prefetch stops when the position stops matching.
#include "modtools_plugin.hpp"
#include "camwalk.h"
#include <windows.h>
#include <xmmintrin.h>
#include <cstdint>
#include <cstdio>
#include <string>

namespace br::camwalk
{
namespace
{
    // IDA addresses (no ASLR), resolved through host->RadiantAddress
    const uintptr_t kCategories = 0x144487830;  // six { begin, end, capacity } vectors of 16-byte { record*, mask }
    const int       kWalkMask   = 0x40;         // BrushCache_ProcessRadBrushes(0x40) walks vector 1
    const int       kWalkVector = 1;
    const size_t off_Record_BrushInst   = 32;
    const size_t off_Record_Late        = 92;
    const size_t off_BInst_EntityInst   = 16;
    const size_t off_BInst_ModelInst    = 56;
    const size_t off_EInst_Entity       = 0;
    const size_t off_Entity_EClass      = 240;
    const size_t off_EClass_Flags       = 384;

    // test only: Radiant's own game view toggle (the host's "gameview" leaves the editor walk in the game view state)
    const uintptr_t kMainWinPtr          = 0x1444C6560;  // RadiantMainWin*
    const size_t    off_MainWin_QEChildren = 64;
    const size_t    off_QEChildren_FilterWindow = 376;
    const uintptr_t kFilterWindowVtable  = 0x142515CC8;  // QtFilterWindow::`vftable'
    const uintptr_t kToggleGameView      = 0x1408964C0;  // (QtFilterWindow*): flips the box and both flags, refilters
    const uintptr_t kGameView            = 0x1453A1EE9;  // char: GAME VIEW, 0x1453A1EE8 = its inverse (editor walk on)

    bool g_enabled = true;                      // [performance] cam_prefetch
    const uint8_t* const* g_categories = nullptr;

    // the walk in progress (main thread)
    const uint8_t* g_vec = nullptr;             // vector 1 while the prefetch follows it, null = not following
    intptr_t g_vecCount = 0;
    intptr_t g_cursor = -2;                     // -1 = take the vector at the first callback, -2 = off for this walk

    // diagnostics: cost of the 3D frame's walks, per mask
    struct WalkStat { int mask = 0; uint64_t calls = 0, objects = 0; double ms = 0; };
    WalkStat g_stats[12];
    int      g_slot = -1;
    uint64_t g_followed = 0, g_lost = 0, g_faults = 0;

    const uint8_t* Ptr(const uint8_t* p, size_t off) { return *reinterpret_cast<const uint8_t* const*>(p + off); }
    const uint8_t* RecordAt(intptr_t i) { return Ptr(g_vec, size_t(16 * i)); }
    void Prefetch(const uint8_t* p) { _mm_prefetch(reinterpret_cast<const char*>(p), _MM_HINT_T0); }

    // each stage loads pointers an earlier call prefetched, then prefetches the next link
    void PrefetchAhead(intptr_t i)
    {
        __try
        {
            if (i + 24 < g_vecCount)
                if (const uint8_t* r = RecordAt(i + 24)) { Prefetch(r + off_Record_BrushInst); Prefetch(r + off_Record_Late); }
            if (i + 12 < g_vecCount)
                if (const uint8_t* r = RecordAt(i + 12))
                    if (const uint8_t* bi = Ptr(r, off_Record_BrushInst)) { Prefetch(bi + off_BInst_EntityInst); Prefetch(bi + off_BInst_ModelInst); }
            if (i + 6 < g_vecCount)
                if (const uint8_t* r = RecordAt(i + 6))
                    if (const uint8_t* bi = Ptr(r, off_Record_BrushInst))
                        if (const uint8_t* ei = Ptr(bi, off_BInst_EntityInst)) Prefetch(ei);
            if (i + 3 < g_vecCount)
                if (const uint8_t* r = RecordAt(i + 3))
                    if (const uint8_t* bi = Ptr(r, off_Record_BrushInst))
                        if (const uint8_t* ei = Ptr(bi, off_BInst_EntityInst))
                            if (const uint8_t* en = Ptr(ei, off_EInst_Entity)) Prefetch(en + off_Entity_EClass);
            if (i + 1 < g_vecCount)
                if (const uint8_t* r = RecordAt(i + 1))
                    if (const uint8_t* bi = Ptr(r, off_Record_BrushInst))
                        if (const uint8_t* ei = Ptr(bi, off_BInst_EntityInst))
                            if (const uint8_t* en = Ptr(ei, off_EInst_Entity))
                                if (const uint8_t* ec = Ptr(en, off_Entity_EClass)) Prefetch(ec + off_EClass_Flags);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ++g_faults;
            g_vec = nullptr;
            g_cursor = -2;
        }
    }

    std::string Status()
    {
        char b[1024];
        int n = snprintf(b, sizeof b, "cam_prefetch=%d ready=%d walks followed=%llu lost=%llu faults=%llu |",
                         g_enabled ? 1 : 0, g_categories ? 1 : 0, (unsigned long long)g_followed,
                         (unsigned long long)g_lost, (unsigned long long)g_faults);
        for (const WalkStat& s : g_stats)
            if (s.calls && n > 0 && n < int(sizeof b))
                n += snprintf(b + n, sizeof b - n, " mask 0x%x: %llu walks, %.0f objects and %.2f ms each;", s.mask,
                              (unsigned long long)s.calls, double(s.objects) / double(s.calls), s.ms / double(s.calls));
        return b;
    }

    std::string ToggleGameView(bool on)
    {
        uint8_t* const* const mainWin = static_cast<uint8_t* const*>(rp::host->RadiantAddress(rp::self, kMainWinPtr));
        void* const toggle = rp::host->RadiantAddress(rp::self, kToggleGameView);
        void* const vtable = rp::host->RadiantAddress(rp::self, kFilterWindowVtable);
        const volatile char* const gameView = static_cast<const volatile char*>(rp::host->RadiantAddress(rp::self, kGameView));
        const uint8_t* const children = mainWin && *mainWin ? Ptr(*mainWin, off_MainWin_QEChildren) : nullptr;
        uint8_t* const filter = children ? const_cast<uint8_t*>(Ptr(children, off_QEChildren_FilterWindow)) : nullptr;
        if (!filter || !toggle || !vtable || !gameView || *reinterpret_cast<void* const*>(filter) != vtable) return "no filter window";
        if ((gameView[0] != 0) != on) reinterpret_cast<void (__fastcall*)(void*)>(toggle)(filter);
        char b[64];
        snprintf(b, sizeof b, "game view=%d editor walk=%d", int(gameView[0]), int(gameView[-1]));
        return b;
    }
}

void Setup()
{
    g_enabled = rp::ConfigGetBool("performance", "cam_prefetch", true);
    g_categories = static_cast<const uint8_t* const*>(rp::host->RadiantAddress(rp::self, kCategories));
    rp::Log("camwalk: editor view walk prefetch enabled=%d ready=%d", g_enabled ? 1 : 0, g_categories ? 1 : 0);
}

int IterBegin(int mask)
{
    g_vec = nullptr;
    g_cursor = g_enabled && g_categories && mask == kWalkMask ? -1 : -2;
    g_slot = -1;
    for (int i = 0; i < int(sizeof g_stats / sizeof g_stats[0]); ++i)
    {
        if (g_stats[i].calls && g_stats[i].mask != mask) continue;
        g_stats[i].mask = mask;
        ++g_stats[i].calls;
        g_slot = i;
        break;
    }
    return g_slot;
}

void IterEnd(int token, double ms)
{
    if (token >= 0) g_stats[token].ms += ms;
    g_slot = -1;
    g_vec = nullptr;
    g_cursor = -2;
}

void IterCallback(const void* entry)
{
    if (g_slot >= 0) ++g_stats[g_slot].objects;
    if (g_cursor == -2 || !entry) return;
    const void* const record = *static_cast<const void* const*>(entry);
    if (g_cursor == -1)
    {
        // vectors are ready before the first callback: take vector 1 and check it is the one walked
        const uint8_t* const* const v = g_categories + 3 * kWalkVector;
        g_vec = v[0];
        g_vecCount = v[0] && v[1] > v[0] ? (v[1] - v[0]) / 16 : 0;
        g_cursor = 0;
        if (!g_vecCount || RecordAt(0) != record) { g_vec = nullptr; g_cursor = -2; ++g_lost; return; }
        ++g_followed;
    }
    else if (++g_cursor >= g_vecCount || RecordAt(g_cursor) != record)
    {
        ++g_lost;
        g_vec = nullptr;
        g_cursor = -2;
        return;
    }
    PrefetchAhead(g_cursor);
}

bool Command(const std::string& c, std::string& reply)
{
    if (c.rfind("camwalk", 0) != 0 || (c.size() > 7 && c[7] != ' ')) return false;
    const std::string arg = c.size() > 8 ? c.substr(8) : "status";
    if (arg == "0" || arg == "1")
    {
        g_enabled = arg == "1";
        rp::ConfigSet("performance", "cam_prefetch", arg);
    }
    else if (arg == "gameview 0" || arg == "gameview 1")
    {
        reply = ToggleGameView(arg[9] == '1');
        return true;
    }
    else if (arg == "reset")
    {
        for (WalkStat& s : g_stats) s = WalkStat{};
        g_followed = g_lost = g_faults = 0;
    }
    else if (arg != "status")
    {
        reply = "camwalk [status|0|1|reset|gameview 0|gameview 1]";
        return true;
    }
    reply = Status();
    return true;
}
}
