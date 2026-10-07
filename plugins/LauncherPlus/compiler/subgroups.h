#pragma once
// C5: triangle subgroups (EmitTrianglesForContext, per run of equal sort key). Same entries and sort as
// stock, but only the window entries sharing an unordered id pair with entry i are tested (every connected
// case shares one), in ascending order. The relabel pass becomes a union of label sets resolved at the end.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include "../../../src/radiant_addrs.h"
#include "../../../src/third_party/minhook/include/MinHook.h"

namespace subgroups
{
    using SubgroupsFn = uint64_t (*)(uint8_t** surfTris, const int* surfCounts, int surfCount, const int* vertMap);
    using SortIdsFn = void (*)(const int* src, int* dst);
    using CompareFn = int (__cdecl*)(const void*, const void*);
    using QsortFn = void (__cdecl*)(void*, size_t, size_t, CompareFn);
    using NewFn = void* (*)(size_t);
    using DeleteFn = void (*)(void*);

    inline SubgroupsFn g_subgroups = nullptr;  // trampoline
    inline SortIdsFn g_sortIds = nullptr;
    inline CompareFn g_entryCompare = nullptr, g_triCompare = nullptr;
    inline QsortFn g_qsort = nullptr;          // the CRT qsort cod2map imports (MSVCR110)
    inline NewFn g_new = nullptr;
    inline DeleteFn g_delete = nullptr;
    inline bool g_verify = false;
    inline unsigned long long g_calls = 0, g_stockCalls = 0, g_merges = 0, g_mismatches = 0;
    constexpr int kSmall = 64;

    struct Entry { int d[3]; int pad; uint8_t* tri; };
    static_assert(sizeof(Entry) == 24, "stock layout");

    inline uint16_t& Label(uint8_t* tri) { return *reinterpret_cast<uint16_t*>(tri + 2); }

    // i and k share an edge: stock's exact test chain (ids ascending within each entry).
    inline bool Connected(const Entry& i, const Entry& k)
    {
        if (i.d[2] == k.d[2])
            return i.d[0] == k.d[0] || i.d[1] == k.d[0] || i.d[0] == k.d[1] || i.d[1] == k.d[1];
        if (i.d[2] == k.d[1]) return i.d[0] == k.d[0] || i.d[1] == k.d[0];
        if (i.d[1] == k.d[2]) return i.d[0] == k.d[0] || i.d[0] == k.d[1];
        return i.d[1] == k.d[1] && i.d[0] == k.d[0];
    }

    inline uint64_t Fast(uint8_t** surfTris, const int* surfCounts, int surfCount, const int* vertMap, int total, bool stockAllocator)
    {
        Entry* entries = static_cast<Entry*>(stockAllocator ? g_new(sizeof(Entry) * size_t(total)) : ::operator new(sizeof(Entry) * size_t(total)));
        int n = 0;
        for (int s = 0; s < surfCount; ++s)
            for (int t = 0; t < surfCounts[s]; ++t)
            {
                uint8_t* tri = surfTris[s] + 120ull * t;
                Label(tri) = 0;
                const int* v = reinterpret_cast<const int*>(tri + 24);
                const int ids[3] = { vertMap[v[0]], vertMap[v[1]], vertMap[v[2]] };
                g_sortIds(ids, entries[n].d);
                entries[n].tri = tri;
                ++n;
            }
        g_qsort(entries, size_t(n), sizeof(Entry), g_entryCompare);

        // labels are 1.., parent[l] == l for a live label, stored tri labels may be stale until resolved
        std::vector<uint16_t> parent(size_t(n) + 2);
        auto find = [&](uint16_t l) {
            uint16_t root = l;
            while (parent[root] != root) root = parent[root];
            while (parent[l] != root) { const uint16_t next = parent[l]; parent[l] = root; l = next; }
            return root;
        };
        // one sorted (pair, entry) table per run, ranges ascending in entry index
        struct PairRef { uint64_t key; int k; };
        static std::vector<PairRef> pairs;
        static std::vector<int> candidates;
        pairs.clear();
        auto pack = [](int a, int b) { return (uint64_t(uint32_t(a)) << 32) | uint32_t(b); };
        for (int k = 0; k < n; ++k)
        {
            const int* d = entries[k].d;
            pairs.push_back({ pack(d[0], d[1]), k });
            pairs.push_back({ pack(d[0], d[2]), k });
            pairs.push_back({ pack(d[1], d[2]), k });
        }
        std::sort(pairs.begin(), pairs.end(), [](const PairRef& a, const PairRef& b) { return a.key != b.key ? a.key < b.key : a.k < b.k; });

        uint16_t nextLabel = 1;
        unsigned groups = 0;
        int j = 0;
        for (int i = 0; i < n; ++i)
        {
            while (entries[i].d[1] > entries[j].d[2]) ++j;  // signed, as stock
            candidates.clear();
            const int* d = entries[i].d;
            const uint64_t keys[3] = { pack(d[0], d[1]), pack(d[0], d[2]), pack(d[1], d[2]) };
            for (uint64_t key : keys)
            {
                auto it = std::lower_bound(pairs.begin(), pairs.end(), PairRef{ key, j },
                                           [](const PairRef& a, const PairRef& b) { return a.key != b.key ? a.key < b.key : a.k < b.k; });
                for (; it != pairs.end() && it->key == key && it->k < i; ++it) candidates.push_back(it->k);
            }
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
            uint16_t cur = 0;  // stock: the label i already carries, always 0 here
            for (int k : candidates)
            {
                const uint16_t lk = find(Label(entries[k].tri));
                if (lk == cur || !Connected(entries[i], entries[k])) continue;
                if (cur) { parent[cur] = lk; --groups; ++g_merges; }
                cur = lk;
            }
            if (!cur)
            {
                cur = nextLabel++;
                parent[cur] = cur;
                ++groups;
            }
            Label(entries[i].tri) = cur;
        }
        for (int i = 0; i < n; ++i) Label(entries[i].tri) = find(Label(entries[i].tri));
        if (stockAllocator) g_delete(entries); else ::operator delete(entries);
        if (groups > 1)
            for (int s = 0; s < surfCount; ++s) g_qsort(surfTris[s], size_t(surfCounts[s]), 120, g_triCompare);
        return groups;
    }

    inline uint64_t Subgroups(uint8_t** surfTris, const int* surfCounts, int surfCount, const int* vertMap)
    {
        ++g_calls;
        long long total = 0;
        for (int s = 0; s < surfCount; ++s) total += surfCounts[s];
        // tiny runs, overflowing label space and stock's asserting cases stay stock
        if (total < kSmall || total >= 0xFFFF || surfCount <= 0)
        { ++g_stockCalls; return g_subgroups(surfTris, surfCounts, surfCount, vertMap); }
        if (!g_verify) return Fast(surfTris, surfCounts, surfCount, vertMap, int(total), true);

        std::vector<std::vector<uint8_t>> before(surfCount), ours(surfCount);
        for (int s = 0; s < surfCount; ++s) before[s].assign(surfTris[s], surfTris[s] + 120ull * surfCounts[s]);
        const uint64_t fastGroups = Fast(surfTris, surfCounts, surfCount, vertMap, int(total), false) & 0xFFFF;
        for (int s = 0; s < surfCount; ++s)
        {
            ours[s].assign(surfTris[s], surfTris[s] + 120ull * surfCounts[s]);
            memcpy(surfTris[s], before[s].data(), before[s].size());
        }
        const uint64_t stock = g_subgroups(surfTris, surfCounts, surfCount, vertMap);
        bool same = (stock & 0xFFFF) == fastGroups;
        for (int s = 0; s < surfCount && same; ++s) same = !memcmp(surfTris[s], ours[s].data(), ours[s].size());
        if (!same) ++g_mismatches;
        return stock;
    }

    inline bool Verify(uintptr_t base)
    {
        using namespace cod2mapaddr;
        auto match = [base](uintptr_t rva, const void* sig, size_t size) { return !memcmp(reinterpret_cast<void*>(base + rva), sig, size); };
        return match(SubgroupsRva, SubgroupsSig, sizeof SubgroupsSig) && match(SortIdsRva, SortIdsSig, sizeof SortIdsSig) &&
               match(EntryCompareRva, EntryCompareSig, sizeof EntryCompareSig) &&
               match(TriLabelCompareRva, TriLabelCompareSig, sizeof TriLabelCompareSig) &&
               GetProcAddress(GetModuleHandleW(L"MSVCR110.dll"), "qsort") != nullptr;
    }

    inline bool Install(uintptr_t base, bool verify)
    {
        using namespace cod2mapaddr;
        g_verify = verify;
        g_sortIds = reinterpret_cast<SortIdsFn>(base + SortIdsRva);
        g_entryCompare = reinterpret_cast<CompareFn>(base + EntryCompareRva);
        g_triCompare = reinterpret_cast<CompareFn>(base + TriLabelCompareRva);
        g_qsort = reinterpret_cast<QsortFn>(GetProcAddress(GetModuleHandleW(L"MSVCR110.dll"), "qsort"));
        g_new = reinterpret_cast<NewFn>(base + ExeNewRva);
        g_delete = reinterpret_cast<DeleteFn>(base + ExeDeleteRva);
        return g_qsort && MH_CreateHook(reinterpret_cast<void*>(base + SubgroupsRva), reinterpret_cast<void*>(&Subgroups),
                                        reinterpret_cast<void**>(&g_subgroups)) == MH_OK;
    }

    inline void Report(FILE* out)
    {
        fprintf(out, "[BetterBuild] Subgroups: %llu runs (%llu small, stock), %llu label merges", g_calls, g_stockCalls, g_merges);
        if (g_verify) fprintf(out, ", %llu verify mismatches (stock kept)", g_mismatches);
        fprintf(out, ".\n");
    }
}
