#pragma once
// C11: connected-group labelling when a large soup is split (0x1BBD60 -> 0x1BBC30). Candidates come from the
// directed-edge table (edgetable.h), tested ascending with stock's predicate, relabel becomes a union of label sets.
// Numeric labels match stock's (the caller emits groups by ascending label).
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../../../src/radiant_addrs.h"
#include "../../../src/third_party/minhook/include/MinHook.h"
#include "edgetable.h"

namespace soupsplit
{
    using LabelFn = uint32_t (*)(uint8_t* ctx, const uint32_t* soup, uint16_t* labels);
    using SharesEdgeFn = char (*)(uint8_t* ctx, const uint32_t* soup, uint32_t t, uint32_t k);
    inline LabelFn g_label = nullptr;  // trampoline
    inline SharesEdgeFn g_sharesEdge = nullptr;
    inline bool g_verify = false;
    inline unsigned long long g_calls = 0, g_stockCalls = 0, g_merges = 0, g_mismatches = 0;
    constexpr uint32_t kSmall = 64;
    constexpr int kPrev[3] = { 2, 0, 1 };

    inline uint32_t Fast(uint8_t* ctx, const uint32_t* soup, uint16_t* labels)
    {
        const uint32_t triCount = soup[2];
        const uint8_t* verts = *reinterpret_cast<uint8_t**>(ctx + 16) + 68ull * soup[0];
        const uint16_t* idx = reinterpret_cast<const uint16_t*>(*reinterpret_cast<uint8_t**>(ctx + 96) + 12ull * soup[3]);
        auto pos = [&](uint32_t t, int e) { return reinterpret_cast<const float*>(verts + 68ull * idx[6ull * t + e]); };

        static edgetable::Table table;   // directed edge -> earlier triangles, reused across soups
        static std::vector<uint32_t> candidates;
        static std::vector<uint16_t> parent;  // labels 1.., parent[l] == l for a live label
        table.Reset(size_t(triCount) * 3);
        parent.assign(size_t(triCount) + 2, 0);
        auto find = [&](uint16_t l) {
            uint16_t root = l;
            while (parent[root] != root) root = parent[root];
            while (parent[l] != root) { const uint16_t next = parent[l]; parent[l] = root; l = next; }
            return root;
        };
        uint16_t nextLabel = 1;
        uint32_t groups = 0;
        for (uint32_t t = 0; t < triCount; ++t)
        {
            candidates.clear();
            for (int e = 0; e < 3; ++e)
            {
                edgetable::Key reversed;
                if (edgetable::MakeKey(pos(t, e), pos(t, kPrev[e]), reversed)) table.Gather(reversed, candidates);
            }
            edgetable::SortUnique(candidates);
            uint16_t cur = 0;  // stock: labels[t] starts at 0
            for (uint32_t k : candidates)
            {
                const uint16_t lk = find(labels[k]);
                if (lk == cur || !g_sharesEdge(ctx, soup, t, k)) continue;
                if (cur) { parent[cur] = lk; --groups; ++g_merges; }
                cur = lk;
            }
            if (!cur) { cur = nextLabel++; parent[cur] = cur; ++groups; }
            labels[t] = cur;
            for (int e = 0; e < 3; ++e)
            {
                edgetable::Key key;
                if (edgetable::MakeKey(pos(t, kPrev[e]), pos(t, e), key)) table.Insert(key, t);
            }
        }
        for (uint32_t t = 0; t < triCount; ++t) labels[t] = find(labels[t]);
        return groups;
    }

    inline uint32_t Label(uint8_t* ctx, const uint32_t* soup, uint16_t* labels)
    {
        ++g_calls;
        const uint32_t triCount = soup[2];
        if (triCount < kSmall || triCount >= 0xFFFE) { ++g_stockCalls; return g_label(ctx, soup, labels); }
        if (!g_verify) return Fast(ctx, soup, labels);
        std::vector<uint16_t> ours(triCount);
        const uint32_t fastGroups = Fast(ctx, soup, ours.data());
        const uint32_t stockGroups = g_label(ctx, soup, labels);
        if (fastGroups != stockGroups || memcmp(ours.data(), labels, sizeof(uint16_t) * triCount)) ++g_mismatches;
        return stockGroups;
    }

    inline bool Verify(uintptr_t base)
    {
        using namespace cod2mapaddr;
        return !memcmp(reinterpret_cast<void*>(base + SoupLabelRva), SoupLabelSig, sizeof SoupLabelSig) &&
               !memcmp(reinterpret_cast<void*>(base + SharesEdgeRva), SharesEdgeSig, sizeof SharesEdgeSig);
    }

    inline bool Install(uintptr_t base, bool verify)
    {
        using namespace cod2mapaddr;
        g_verify = verify;
        g_sharesEdge = reinterpret_cast<SharesEdgeFn>(base + SharesEdgeRva);
        return MH_CreateHook(reinterpret_cast<void*>(base + SoupLabelRva), reinterpret_cast<void*>(&Label),
                             reinterpret_cast<void**>(&g_label)) == MH_OK;
    }

    inline void Report(FILE* out)
    {
        fprintf(out, "[BetterBuild] Soup Split Groups: %llu soups (%llu small, stock), %llu label merges", g_calls, g_stockCalls, g_merges);
        if (g_verify) fprintf(out, ", %llu verify mismatches (stock kept)", g_mismatches);
        fprintf(out, ".\n");
    }
}
