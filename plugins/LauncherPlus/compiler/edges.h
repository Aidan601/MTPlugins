#pragma once
// C4: per-soup triangle edge neighbours (EmitTrianglesForContext). Stock tests every triangle pair.
// Directed edges are hashed by exact endpoint positions (edgetable.h), then stock's own 3x3 first-match
// test runs on the candidates in ascending order, so the writes match. Indices use stock's 6-u16 stride.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../../../src/radiant_addrs.h"
#include "../../../src/third_party/minhook/include/MinHook.h"
#include "edgetable.h"

namespace edges
{
    using EdgesFn = uint64_t (*)(uint8_t* ctx, uint32_t firstVert, uint64_t unused, const uint16_t* idx, uint32_t triCount, int* out);
    inline EdgesFn g_edges = nullptr;  // trampoline
    inline bool g_verify = false;
    inline unsigned long long g_calls = 0, g_stockCalls = 0, g_pairs = 0, g_mismatches = 0;
    constexpr uint32_t kSmall = 24;
    constexpr int kPrev[3] = { 2, 0, 1 };

    inline uint64_t Fast(uint8_t* ctx, uint32_t firstVert, const uint16_t* idx, uint32_t triCount, int* out)
    {
        memset(out, 0xFF, sizeof(int) * 3ull * triCount);
        const uint8_t* verts = *reinterpret_cast<uint8_t**>(ctx + 16) + 68ull * firstVert;
        auto pos = [&](uint32_t t, int e) { return reinterpret_cast<const float*>(verts + 68ull * idx[6ull * t + e]); };
        auto equal = [](const float* a, const float* b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]; };

        static edgetable::Table table;   // directed edge -> triangles, reused across soups
        static std::vector<uint32_t> candidates;
        table.Reset(size_t(triCount) * 3);
        auto insert = [&](uint32_t t) {
            for (int e = 0; e < 3; ++e)
            {
                edgetable::Key key;
                if (edgetable::MakeKey(pos(t, kPrev[e]), pos(t, e), key)) table.Insert(key, t);
            }
        };
        if (triCount) insert(0);
        for (uint32_t j = 1; j < triCount; ++j)
        {
            candidates.clear();
            for (int k = 0; k < 3; ++k)
            {
                edgetable::Key reversed;
                if (edgetable::MakeKey(pos(j, k), pos(j, kPrev[k]), reversed)) table.Gather(reversed, candidates);
            }
            edgetable::SortUnique(candidates);
            for (uint32_t i : candidates)
            {
                ++g_pairs;
                for (int k = 0; k < 3; ++k)
                {
                    bool hit = false;
                    for (int m = 0; m < 3; ++m)
                        if (equal(pos(j, kPrev[k]), pos(i, m)) && equal(pos(j, k), pos(i, kPrev[m])))
                        {
                            out[3ull * j + kPrev[k]] = int(i);
                            out[3ull * i + kPrev[m]] = int(j);
                            hit = true;
                            break;
                        }
                    if (hit) break;
                }
            }
            insert(j);
        }
        return triCount > 1 ? 3ull * triCount : reinterpret_cast<uint64_t>(out);
    }

    inline uint64_t Edges(uint8_t* ctx, uint32_t firstVert, uint64_t unused, const uint16_t* idx, uint32_t triCount, int* out)
    {
        ++g_calls;
        if (triCount < kSmall) { ++g_stockCalls; return g_edges(ctx, firstVert, unused, idx, triCount, out); }
        if (!g_verify) return Fast(ctx, firstVert, idx, triCount, out);
        std::vector<int> ours(3ull * triCount);
        const uint64_t fastResult = Fast(ctx, firstVert, idx, triCount, ours.data());
        const uint64_t stockResult = g_edges(ctx, firstVert, unused, idx, triCount, out);
        if (fastResult != stockResult && triCount > 1) ++g_mismatches;
        else if (memcmp(ours.data(), out, sizeof(int) * ours.size())) ++g_mismatches;
        return stockResult;
    }

    inline bool Verify(uintptr_t base)
    {
        return !memcmp(reinterpret_cast<void*>(base + cod2mapaddr::EdgeNeighboursRva), cod2mapaddr::EdgeNeighboursSig,
                       sizeof cod2mapaddr::EdgeNeighboursSig);
    }

    inline bool Install(uintptr_t base, bool verify)
    {
        g_verify = verify;
        return MH_CreateHook(reinterpret_cast<void*>(base + cod2mapaddr::EdgeNeighboursRva), reinterpret_cast<void*>(&Edges),
                             reinterpret_cast<void**>(&g_edges)) == MH_OK;
    }

    inline void Report(FILE* out)
    {
        fprintf(out, "[BetterBuild] Edge Neighbours: %llu soups (%llu small, stock), %llu candidate pairs", g_calls, g_stockCalls, g_pairs);
        if (g_verify) fprintf(out, ", %llu verify mismatches (stock kept)", g_mismatches);
        fprintf(out, ".\n");
    }
}
