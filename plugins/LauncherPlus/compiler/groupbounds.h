#pragma once
// C10: SplitSparseSoups group merge bounds (inside 0x1AD8E0). Three loops are patched in place with calls: A scan
// 0x1AE130 (per-label vertex lists, fold A), B scan 0x1AE241 (fold B), relabel 0x1AE3AF (also updates the lists).
// Folds use stock's ClearBounds 0x127020 and AddPointToBounds 0x122780 in order, so bounds are bit-identical.
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>
#include "../../../src/radiant_addrs.h"

namespace groupbounds
{
    using ClearFn = void (*)(float* mins, float* maxs);
    using AddFn = void (*)(const float* point, float* mins, float* maxs);
    inline ClearFn g_clear = nullptr;
    inline AddFn g_add = nullptr;
    inline uint8_t** g_verts = nullptr;  // 68-byte records, position first
    inline bool g_verify = false;
    inline unsigned long long g_aScans = 0, g_bScans = 0, g_merges = 0, g_naive = 0, g_mismatches = 0;

    // per-label vertex indices, ascending, valid from an A scan to the end of that A iteration
    // (thread_local in case SplitSparseSoups ever leaves the main thread)
    inline thread_local std::vector<std::vector<uint16_t>> g_lists;
    inline thread_local bool g_valid = false;

    inline uint16_t VertexCount(const uint8_t* soup) { return *reinterpret_cast<const uint16_t*>(soup + 0x14); }
    inline const float* Position(const uint8_t* soup, uint32_t v)
    {
        return reinterpret_cast<const float*>(*g_verts + 68ull * (*reinterpret_cast<const uint32_t*>(soup + 0x10) + v));
    }

    // stock's loop: every vertex from `start`, label compared as u16
    inline void FoldNaive(const uint8_t* soup, const uint16_t* labels, uint16_t label, uint32_t start, float* mins, float* maxs)
    {
        g_clear(mins, maxs);
        for (uint32_t v = start; v < VertexCount(soup); ++v)
            if (labels[v] == label) g_add(Position(soup, v), mins, maxs);
    }

    inline void FoldList(const uint8_t* soup, uint16_t label, float* mins, float* maxs)
    {
        g_clear(mins, maxs);
        if (label < g_lists.size())
            for (uint16_t v : g_lists[label]) g_add(Position(soup, v), mins, maxs);
    }

    inline void Check(const uint8_t* soup, const uint16_t* labels, uint16_t label, float* mins, float* maxs)
    {
        float m[3], x[3];
        FoldNaive(soup, labels, label, 0, m, x);
        if (memcmp(m, mins, sizeof m) || memcmp(x, maxs, sizeof x)) ++g_mismatches;
        memcpy(mins, m, sizeof m);  // stock's answer is kept
        memcpy(maxs, x, sizeof x);
    }

    // r8d = A | start << 16 (start is 0 here, anything else runs stock's loop)
    inline void ScanA(const uint8_t* soup, uint16_t* labels, uint32_t packed, uint8_t* frame)
    {
        ++g_aScans;
        const uint16_t a = uint16_t(packed), start = uint16_t(packed >> 16);
        float* mins = reinterpret_cast<float*>(frame + 0x08);
        float* maxs = reinterpret_cast<float*>(frame + 0x18);
        if (start != 0) { ++g_naive; g_valid = false; FoldNaive(soup, labels, a, start, mins, maxs); return; }
        const uint16_t n = VertexCount(soup);
        uint16_t top = 0;
        for (uint16_t v = 0; v < n; ++v) top = std::max(top, labels[v]);
        if (g_lists.size() < size_t(top) + 1) g_lists.resize(size_t(top) + 1);
        for (auto& list : g_lists) list.clear();
        for (uint16_t v = 0; v < n; ++v) g_lists[labels[v]].push_back(v);
        g_valid = true;
        FoldList(soup, a, mins, maxs);
        if (g_verify) Check(soup, labels, a, mins, maxs);
    }

    inline void ScanB(const uint8_t* soup, uint16_t* labels, uint32_t b, uint8_t* frame)
    {
        ++g_bScans;
        float* mins = reinterpret_cast<float*>(frame + 0x38);
        float* maxs = reinterpret_cast<float*>(frame + 0x28);
        if (!g_valid) { ++g_naive; FoldNaive(soup, labels, uint16_t(b), 0, mins, maxs); return; }
        FoldList(soup, uint16_t(b), mins, maxs);
        if (g_verify) Check(soup, labels, uint16_t(b), mins, maxs);
    }

    inline void Relabel(const uint8_t* soup, uint16_t* labels, uint32_t a32, uint32_t b32)
    {
        ++g_merges;
        const uint16_t a = uint16_t(a32), b = uint16_t(b32);
        for (uint16_t v = 0; v < VertexCount(soup); ++v)  // stock: == b becomes a, then > b drops by one
        {
            if (labels[v] == b) labels[v] = a;
            if (labels[v] > b) --labels[v];
        }
        if (!g_valid) return;
        if (a >= g_lists.size() || b >= g_lists.size()) { g_valid = false; return; }
        std::vector<uint16_t> merged;
        merged.reserve(g_lists[a].size() + g_lists[b].size());
        std::merge(g_lists[a].begin(), g_lists[a].end(), g_lists[b].begin(), g_lists[b].end(), std::back_inserter(merged));
        g_lists[a].swap(merged);
        // a < b always (A's inner loop starts at A + 1), labels above b shift down
        if (a > b) { g_valid = false; return; }
        g_lists.erase(g_lists.begin() + b);
        g_lists.emplace_back();
    }

    // stub: mov rcx,rsi / mov rdx,r13 / r8d / r9 / mov rax,imm64 / call rax / [movzx ebx,word [rsi+14h]] / jmp back
    inline size_t Emit(uint8_t* at, const uint8_t* argBytes, size_t argSize, void* fn, bool setEbx, uintptr_t back)
    {
        uint8_t code[64];
        size_t n = 0;
        auto put = [&](std::initializer_list<uint8_t> bytes) { for (uint8_t x : bytes) code[n++] = x; };
        put({ 0x48, 0x89, 0xF1 });                 // mov rcx, rsi
        put({ 0x4C, 0x89, 0xEA });                 // mov rdx, r13
        memcpy(code + n, argBytes, argSize); n += argSize;
        put({ 0x48, 0xB8 });                       // mov rax, imm64
        const uint64_t target = reinterpret_cast<uint64_t>(fn);
        memcpy(code + n, &target, 8); n += 8;
        put({ 0xFF, 0xD0 });                       // call rax
        if (setEbx) put({ 0x0F, 0xB7, 0x5E, 0x14 }); // movzx ebx, word ptr [rsi+0x14]
        const int32_t rel = int32_t(intptr_t(back) - intptr_t(reinterpret_cast<uintptr_t>(at) + n + 5));
        put({ 0xE9 });
        memcpy(code + n, &rel, 4); n += 4;
        memcpy(at, code, n);
        return n;
    }

    struct Region { uintptr_t rva, end; const unsigned char* sig; size_t sigSize; };

    inline bool Verify(uintptr_t base)
    {
        using namespace cod2mapaddr;
        auto match = [base](uintptr_t rva, const void* sig, size_t size) { return !memcmp(reinterpret_cast<void*>(base + rva), sig, size); };
        return match(GroupScanARva, GroupScanASig, sizeof GroupScanASig) && match(GroupScanBRva, GroupScanBSig, sizeof GroupScanBSig) &&
               match(GroupRelabelRva, GroupRelabelSig, sizeof GroupRelabelSig) &&
               match(BoundsClearRva, BoundsClearSig, sizeof BoundsClearSig) && match(BoundsAddRva, BoundsAddSig, sizeof BoundsAddSig);
    }

    inline bool Install(uintptr_t base, bool verify)
    {
        using namespace cod2mapaddr;
        g_verify = verify;
        g_clear = reinterpret_cast<ClearFn>(base + BoundsClearRva);
        g_add = reinterpret_cast<AddFn>(base + BoundsAddRva);
        g_verts = reinterpret_cast<uint8_t**>(base + SoupVertsRva);
        const Region regions[] = { { GroupScanARva, GroupScanARva + sizeof GroupScanASig, GroupScanASig, sizeof GroupScanASig },
                                   { GroupScanBRva, GroupScanBRva + sizeof GroupScanBSig, GroupScanBSig, sizeof GroupScanBSig },
                                   { GroupRelabelRva, GroupRelabelRva + sizeof GroupRelabelSig, GroupRelabelSig, sizeof GroupRelabelSig } };
        uint8_t* lo = reinterpret_cast<uint8_t*>(base + GroupScanARva);
        const size_t span = GroupRelabelRva + sizeof GroupRelabelSig - GroupScanARva;
        DWORD old = 0;
        if (!VirtualProtect(lo, span, PAGE_EXECUTE_READWRITE, &old)) return false;
        for (const Region& r : regions) memset(reinterpret_cast<void*>(base + r.rva), 0xCC, r.end - r.rva);
        // A: r8d = movzx r14w | movzx di << 16, r9 = rbp.
        static const uint8_t argsA[] = { 0x45, 0x0F, 0xB7, 0xC6, 0x0F, 0xB7, 0xC7, 0xC1, 0xE0, 0x10, 0x41, 0x09, 0xC0, 0x49, 0x89, 0xE9 };
        // B: r8d = movzx di, r9 = rbp.
        static const uint8_t argsB[] = { 0x44, 0x0F, 0xB7, 0xC7, 0x49, 0x89, 0xE9 };
        // Relabel: r8d = movzx r14w (A), r9d = movzx di (B).
        static const uint8_t argsR[] = { 0x45, 0x0F, 0xB7, 0xC6, 0x44, 0x0F, 0xB7, 0xCF };
        bool ok = Emit(reinterpret_cast<uint8_t*>(base + GroupScanARva), argsA, sizeof argsA, reinterpret_cast<void*>(&ScanA), true,
                       base + GroupScanARva + sizeof GroupScanASig) <= sizeof GroupScanASig;
        ok = ok && Emit(reinterpret_cast<uint8_t*>(base + GroupScanBRva), argsB, sizeof argsB, reinterpret_cast<void*>(&ScanB), true,
                        base + GroupScanBRva + sizeof GroupScanBSig) <= sizeof GroupScanBSig;
        ok = ok && Emit(reinterpret_cast<uint8_t*>(base + GroupRelabelRva), argsR, sizeof argsR, reinterpret_cast<void*>(&Relabel), false,
                        base + GroupRelabelRva + sizeof GroupRelabelSig) <= sizeof GroupRelabelSig;
        VirtualProtect(lo, span, old, &old);
        FlushInstructionCache(GetCurrentProcess(), lo, span);
        return ok;
    }

    inline void Report(FILE* out)
    {
        fprintf(out, "[BetterBuild] Group Bounds: %llu A scans, %llu B scans, %llu merges, %llu stock-order scans", g_aScans, g_bScans,
                g_merges, g_naive);
        if (g_verify) fprintf(out, ", %llu verify mismatches (stock kept)", g_mismatches);
        fprintf(out, ".\n");
    }
}
