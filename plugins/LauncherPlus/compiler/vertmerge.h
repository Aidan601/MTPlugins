#pragma once
// C3: per-soup duplicate vertex merge (EmitTrianglesForContext). Candidates come from a hash in 0.002-unit cells
// (27 neighbour cells, ascending, first accept wins), index rewrites are composed and applied once.
// Non-finite positions never match in stock and are never hashed.
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include "../../../src/radiant_addrs.h"
#include "../../../src/third_party/minhook/include/MinHook.h"

namespace vertmerge
{
    using MergeFn = uint32_t (*)(uint8_t* ctx, int first, uint32_t n, uint16_t* idx, uint32_t triCount, uint8_t* tris, int flags);
    using MatchFn = bool (*)(uint8_t* ctx, uint32_t a, uint32_t b, uint8_t* triA, uint8_t* triB, int flags);

    inline MergeFn g_merge = nullptr;  // trampoline
    inline MatchFn g_match = nullptr;
    inline volatile int* g_rejects = nullptr;
    inline bool g_verify = false;
    inline unsigned long long g_kind[5] = {};
    struct Step { uint32_t v, u, last; };
    inline std::vector<Step> g_steps;  // verify mode: merges of the last Fast call
    inline unsigned long long g_calls = 0, g_stockCalls = 0, g_merges = 0, g_mismatches = 0;
    constexpr uint32_t kSmall = 48;  // below this the stock scan is already cheap

    struct CellKey
    {
        int64_t x, y, z;
        bool operator==(const CellKey& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct CellHash
    {
        size_t operator()(const CellKey& k) const
        {
            uint64_t h = uint64_t(k.x) * 0x9E3779B97F4A7C15ull;
            h ^= uint64_t(k.y) * 0xC2B2AE3D27D4EB4Full + (h >> 29);
            h ^= uint64_t(k.z) * 0x165667B19E3779F9ull + (h >> 32);
            return size_t(h);
        }
    };

    inline const float* Position(uint8_t* ctx, uint32_t vertex)
    {
        return reinterpret_cast<const float*>(*reinterpret_cast<uint8_t**>(ctx + 16) + 68ull * vertex);
    }

    inline bool Cell(const float* p, int64_t* c)
    {
        for (int k = 0; k < 3; ++k)
        {
            if (!std::isfinite(p[k])) return false;
            c[k] = int64_t(std::floor(double(p[k]) * 500.0));
        }
        return true;
    }

    inline uint32_t Fast(uint8_t* ctx, int first, uint32_t n, uint16_t* idx, uint32_t triCount, uint8_t* tris, int flags)
    {
        uint8_t* verts = *reinterpret_cast<uint8_t**>(ctx + 16);
        uint8_t* layers = *reinterpret_cast<uint8_t**>(ctx + 24);
        const uint32_t original = n;
        std::vector<int> tri((n + 2) / 3 * 3);
        for (size_t k = 0; k < tri.size(); ++k) tri[k] = int(k / 3);
        std::vector<std::vector<uint32_t>> group(n);  // original slots each current slot stands for
        for (uint32_t s = 0; s < n; ++s) group[s].push_back(s);
        std::unordered_map<CellKey, std::vector<uint32_t>, CellHash> cells;
        cells.reserve(n * 2);
        std::vector<uint32_t> candidates;

        auto settle = [&](uint32_t slot) {
            int64_t c[3];
            if (Cell(Position(ctx, first + slot), c)) cells[{ c[0], c[1], c[2] }].push_back(slot);
        };
        settle(0);
        uint32_t v = 1;
        while (v < n)
        {
            int64_t c[3];
            int accepted = -1;
            if (Cell(Position(ctx, first + v), c))
            {
                candidates.clear();
                for (int64_t dx = -1; dx <= 1; ++dx)
                    for (int64_t dy = -1; dy <= 1; ++dy)
                        for (int64_t dz = -1; dz <= 1; ++dz)
                        {
                            const auto it = cells.find({ c[0] + dx, c[1] + dy, c[2] + dz });
                            if (it != cells.end()) candidates.insert(candidates.end(), it->second.begin(), it->second.end());
                        }
                std::sort(candidates.begin(), candidates.end());
                for (uint32_t u : candidates)
                    if (g_match(ctx, first + u, first + v, tris + 120ull * tri[u], tris + 120ull * tri[v], flags)) { accepted = int(u); break; }
            }
            if (accepted < 0) { settle(v); ++v; continue; }

            ++g_merges;
            const uint32_t last = --n;
            if (g_verify) g_steps.push_back({ v, uint32_t(accepted), last });
            memcpy(verts + 68ull * (first + v), verts + 68ull * (first + last), 68);
            if (layers) memcpy(layers + 72ull * (first + v), layers + 72ull * (first + last), 72);
            tri[v] = tri[last];
            std::vector<uint32_t>& into = group[accepted];
            into.insert(into.end(), group[v].begin(), group[v].end());
            group[v].clear();
            if (last != v) group[v].swap(group[last]);
        }

        std::vector<uint16_t> map(original);
        for (uint32_t s = 0; s < original; ++s)
            for (uint32_t o : group[s]) map[o] = uint16_t(s);
        // stock steps 12 bytes per triangle and rewrites the first three of every six u16 (0x1BB939), reproduced exactly
        for (uint32_t t = 0; t < triCount; ++t)
            for (uint32_t j = 0; j < 3; ++j)
            {
                uint16_t& e = idx[6ull * t + j];
                if (e < original) e = map[e];
            }
        return n;
    }

    inline uint32_t Merge(uint8_t* ctx, int first, uint32_t n, uint16_t* idx, uint32_t triCount, uint8_t* tris, int flags)
    {
        ++g_calls;
        if (n < kSmall || n > 0xFFFF) { ++g_stockCalls; return g_merge(ctx, first, n, idx, triCount, tris, flags); }
        if (!g_verify) return Fast(ctx, first, n, idx, triCount, tris, flags);

        // verify: run ours, keep a copy, restore the inputs, run stock, compare
        uint8_t* verts = *reinterpret_cast<uint8_t**>(ctx + 16) + 68ull * first;
        uint8_t* layerBase = *reinterpret_cast<uint8_t**>(ctx + 24);
        uint8_t* layers = layerBase ? layerBase + 72ull * first : nullptr;
        const std::vector<uint8_t> inVerts(verts, verts + 68ull * n);
        const std::vector<uint8_t> inLayers = layers ? std::vector<uint8_t>(layers, layers + 72ull * n) : std::vector<uint8_t>();
        const size_t idxSpan = triCount ? 6ull * triCount - 3 : 0;  // entries stock can touch
        const std::vector<uint16_t> inIdx(idx, idx + idxSpan);
        const int rejectsBefore = *g_rejects;

        g_steps.clear();
        const uint32_t fastN = Fast(ctx, first, n, idx, triCount, tris, flags);
        const int fastRejects = *g_rejects - rejectsBefore;
        const std::vector<uint8_t> outVerts(verts, verts + 68ull * n);
        const std::vector<uint8_t> outLayers = layers ? std::vector<uint8_t>(layers, layers + 72ull * n) : std::vector<uint8_t>();
        const std::vector<uint16_t> outIdx(idx, idx + idxSpan);

        memcpy(verts, inVerts.data(), inVerts.size());
        if (layers) memcpy(layers, inLayers.data(), inLayers.size());
        memcpy(idx, inIdx.data(), inIdx.size() * 2);
        *g_rejects = rejectsBefore;
        const uint32_t stockN = g_merge(ctx, first, n, idx, triCount, tris, flags);
        const bool nSame = stockN == fastN;
        const bool rejectsSame = *g_rejects - rejectsBefore == fastRejects;
        const bool vertsSame = !memcmp(verts, outVerts.data(), outVerts.size());
        const bool layersSame = !layers || !memcmp(layers, outLayers.data(), outLayers.size());
        const bool idxSame = !memcmp(idx, outIdx.data(), outIdx.size() * 2);
        if (!(nSame && rejectsSame && vertsSame && layersSame && idxSame))
        {
            ++g_mismatches;
            g_kind[0] += !nSame; g_kind[1] += !rejectsSame; g_kind[2] += !vertsSame; g_kind[3] += !layersSame; g_kind[4] += !idxSame;
            if (g_mismatches <= 6)
            {
                size_t firstVert = 0;
                while (firstVert < outVerts.size() && verts[firstVert] == outVerts[firstVert]) ++firstVert;
                // replay the merges with a literal per-merge index rewrite
                std::vector<uint16_t> naive(inIdx);
                for (const Step& st : g_steps)
                    for (uint32_t t = 0; t < triCount; ++t)
                        for (uint32_t j = 0; j < 3; ++j)
                        {
                            uint16_t& e = naive[6ull * t + j];
                            if (e == st.v) e = uint16_t(st.u); else if (e == st.last) e = uint16_t(st.v);
                        }
                size_t d = 0;
                while (d < outIdx.size() && idx[d] == outIdx[d]) ++d;
                fprintf(stderr, "[BetterBuild] Vertex Merge idx: naive==stock %d naive==fast %d; first diff at %zu of %zu: in=%u stock=%u fast=%u naive=%u, merges=%zu\n",
                        !memcmp(naive.data(), idx, naive.size() * 2), naive == outIdx, d, outIdx.size(), d < inIdx.size() ? inIdx[d] : 0,
                        d < outIdx.size() ? idx[d] : 0, d < outIdx.size() ? outIdx[d] : 0, d < naive.size() ? naive[d] : 0, g_steps.size());
                fprintf(stderr, "[BetterBuild] Vertex Merge mismatch: n=%u stock=%u fast=%u rejects stock=%d fast=%d firstVertByte=%zu (slot %zu +%zu) tris=%u flags=%d layers=%d\n",
                        n, stockN, fastN, *g_rejects - rejectsBefore, fastRejects, firstVert, firstVert / 68, firstVert % 68, triCount, flags, layers != nullptr);
            }
        }
        return stockN;
    }

    inline bool Verify(uintptr_t base)
    {
        using namespace cod2mapaddr;
        return !memcmp(reinterpret_cast<void*>(base + VertexMergeRva), VertexMergeSig, sizeof VertexMergeSig) &&
               !memcmp(reinterpret_cast<void*>(base + VertexMatchRva), VertexMatchSig, sizeof VertexMatchSig);
    }

    inline bool Install(uintptr_t base, bool verify)
    {
        using namespace cod2mapaddr;
        g_verify = verify;
        g_match = reinterpret_cast<MatchFn>(base + VertexMatchRva);
        g_rejects = reinterpret_cast<volatile int*>(base + VertexMatchRejectsRva);
        return MH_CreateHook(reinterpret_cast<void*>(base + VertexMergeRva), reinterpret_cast<void*>(&Merge),
                             reinterpret_cast<void**>(&g_merge)) == MH_OK;
    }

    inline void Report(FILE* out)
    {
        fprintf(out, "[BetterBuild] Vertex Merge: %llu soups (%llu small, stock), %llu merges", g_calls, g_stockCalls, g_merges);
        if (g_verify) fprintf(out, ", %llu verify mismatches (stock result kept; n %llu, rejects %llu, verts %llu, layers %llu, idx %llu)",
                              g_mismatches, g_kind[0], g_kind[1], g_kind[2], g_kind[3], g_kind[4]);
        fprintf(out, ".\n");
    }
}
