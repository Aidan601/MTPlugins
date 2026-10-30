#pragma once
// C15: coalesce kd-tree leaf lists (tris_coalesce.cpp). Leaves hold singly linked {next, surf} lists: stock appends by
// walking to the tail, removes by walking the whole list, scans a leaf for coplanar pairs with a pointer chase per item
// and picks split axes with three walks. A per-leaf mirror (node, surf, bounds, normal in list order) answers them;
// list nodes are still allocated and freed through the exe's thunks in stock order. Reset around each group driver call.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <limits>
#include "../../../src/radiant_addrs.h"
#include "../../../src/third_party/minhook/include/MinHook.h"

namespace coalescetree
{
    struct ListNode { ListNode* next; uint8_t* surf; };
    struct Entry { ListNode* node; uint8_t* surf; int bounds[6]; float normal[3]; };
    struct Mirror { uint8_t* leaf; ListNode* head; ListNode* tail; std::vector<Entry> items; };

    using ListFn = int64_t (*)(uint8_t* leaf, uint8_t* surf);
    using ChooserFn = bool (*)(int* out, ListNode* list, int count, const int* center);
    using GroupFn = int64_t (*)(int64_t group);
    using PairTestFn = int (*)(uint8_t* a, uint8_t* b);
    using MergeFn = int64_t (*)(uint8_t* a, uint8_t* b);
    using NewFn = void* (*)(size_t);
    using DeleteFn = void (*)(void*);

    inline ListFn g_append = nullptr, g_remove = nullptr, g_scan = nullptr;  // trampolines
    inline ChooserFn g_chooser = nullptr;
    inline GroupFn g_group = nullptr;
    inline PairTestFn g_pairTest = nullptr;
    inline MergeFn g_merge = nullptr;
    inline NewFn g_new = nullptr;
    inline DeleteFn g_delete = nullptr;
    inline bool g_verify = false;
    inline unsigned long long g_appends = 0, g_removes = 0, g_scans = 0, g_scanItems = 0, g_prefiltered = 0, g_choices = 0,
                              g_rebuilds = 0, g_mismatches = 0;
    inline std::vector<Mirror> g_mirrors(1024);  // open addressing by leaf pointer
    inline size_t g_used = 0;
    inline std::vector<int> g_scratch;           // chooser: 6 ints per item

    inline size_t HashPtr(const void* p) { uint64_t h = reinterpret_cast<uintptr_t>(p); h ^= h >> 33; h *= 0xFF51AFD7ED558CCDull; h ^= h >> 33; return size_t(h); }
    inline ListNode** HeadSlot(uint8_t* leaf) { return reinterpret_cast<ListNode**>(leaf + 8); }

    inline void Fill(Entry& e, ListNode* n)
    {
        e.node = n;
        e.surf = n->surf;
        memcpy(e.bounds, e.surf + 24, sizeof e.bounds);
        const uint8_t* props = *reinterpret_cast<uint8_t* const*>(e.surf + 16);
        if (props) memcpy(e.normal, props + 200, sizeof e.normal);
        else e.normal[0] = e.normal[1] = e.normal[2] = std::numeric_limits<float>::quiet_NaN();  // NaN never prefilters: stock decides
    }

    inline void Rebuild(Mirror& m)
    {
        ++g_rebuilds;
        m.items.clear();
        m.head = *HeadSlot(m.leaf);
        m.tail = nullptr;
        for (ListNode* n = m.head; n; n = n->next) { m.items.emplace_back(); Fill(m.items.back(), n); m.tail = n; }
    }

    inline void Reset()
    {
        if (!g_used) return;
        for (Mirror& m : g_mirrors) { m.leaf = nullptr; m.items.clear(); }
        g_used = 0;
    }

    inline Mirror& Get(uint8_t* leaf)
    {
        if (g_used * 2 >= g_mirrors.size())
        {
            std::vector<Mirror> old(g_mirrors.size() * 2);
            old.swap(g_mirrors);
            const size_t mask = g_mirrors.size() - 1;
            for (Mirror& m : old)
            {
                if (!m.leaf) continue;
                size_t i = HashPtr(m.leaf) & mask;
                while (g_mirrors[i].leaf) i = (i + 1) & mask;
                g_mirrors[i] = std::move(m);
            }
        }
        const size_t mask = g_mirrors.size() - 1;
        size_t i = HashPtr(leaf) & mask;
        while (g_mirrors[i].leaf && g_mirrors[i].leaf != leaf) i = (i + 1) & mask;
        Mirror& m = g_mirrors[i];
        if (!m.leaf) { m.leaf = leaf; m.head = reinterpret_cast<ListNode*>(~uintptr_t(0)); m.tail = nullptr; ++g_used; }
        if (m.head != *HeadSlot(leaf) || (m.tail && m.tail->next)) Rebuild(m);
        else if (g_verify)
        {
            // the mirror must be the live list, entry for entry
            size_t k = 0;
            bool ok = true;
            for (ListNode* n = m.head; n && ok; n = n->next, ++k)
            {
                if (k >= m.items.size() || m.items[k].node != n || m.items[k].surf != n->surf) { ok = false; break; }
                Entry live; Fill(live, n);
                if (memcmp(live.bounds, m.items[k].bounds, sizeof live.bounds) || memcmp(live.normal, m.items[k].normal, sizeof live.normal)) ok = false;
            }
            if (ok && k != m.items.size()) ok = false;
            if (!ok) { ++g_mismatches; Rebuild(m); }
        }
        return m;
    }

    inline int64_t Append(uint8_t* leaf, uint8_t* surf)
    {
        ++g_appends;
        Mirror& m = Get(leaf);
        auto* n = static_cast<ListNode*>(g_new(sizeof(ListNode)));  // stock allocates before its walk
        n->surf = surf;
        n->next = nullptr;
        if (m.tail) m.tail->next = n;
        else *HeadSlot(leaf) = n;
        if (!m.tail) m.head = n;
        m.tail = n;
        m.items.emplace_back();
        Fill(m.items.back(), n);
        return 0;
    }

    inline int64_t Remove(uint8_t* leaf, uint8_t* surf)
    {
        ++g_removes;
        Mirror& m = Get(leaf);
        size_t kept = 0;
        ListNode* prev = nullptr;
        for (size_t k = 0; k < m.items.size(); ++k)
        {
            Entry& e = m.items[k];
            if (e.surf == surf)
            {
                ListNode* n = e.node;
                if (prev) prev->next = n->next;
                else *HeadSlot(leaf) = n->next;
                g_delete(n);
                continue;
            }
            prev = e.node;
            if (kept != k) m.items[kept] = e;
            ++kept;
        }
        m.items.resize(kept);
        m.head = *HeadSlot(leaf);
        m.tail = prev;
        return 0;
    }

    // 0x99A10: per-axis overlap with a tolerance, wrapping adds as stock does.
    inline bool Overlap(const int* a, const int* b, int tol)
    {
        auto add = [](int x, int y) { return int(unsigned(x) + unsigned(y)); };
        for (int i = 0; i < 3; ++i)
        {
            const int bm = b[3 + i], am = a[3 + i];
            if (!(a[i] <= add(bm, tol) || add(bm, tol) <= bm)) return false;
            if (!(b[i] <= add(am, tol) || add(am, tol) <= am)) return false;
        }
        return true;
    }

    // 0xEA2D0: test the surface against every item ahead of it in the leaf's list, merge on the first hit.
    inline int64_t Scan(uint8_t* leaf, uint8_t* surf)
    {
        const uint8_t* props = *reinterpret_cast<uint8_t* const*>(surf + 16);
        if (!*(surf + 50) || !props) return g_scan(leaf, surf);
        ++g_scans;
        Mirror& m = Get(leaf);
        const uint16_t stamp = *reinterpret_cast<const uint16_t*>(surf + 48);
        const int* sb = reinterpret_cast<const int*>(surf + 24);
        const float* sn = reinterpret_cast<const float*>(props + 200);
        for (size_t k = 0; k < m.items.size(); ++k)
        {
            const Entry& e = m.items[k];
            uint8_t* other = e.surf;
            if (other == surf) return 0;
            uint16_t* otherStamp = reinterpret_cast<uint16_t*>(other + 48);
            if (*otherStamp == stamp) continue;
            *otherStamp = stamp;
            ++g_scanItems;
            // the pair test's first two rejections, same float order: (y*y + x*x) + z*z
            const float dot = (sn[1] * e.normal[1] + sn[0] * e.normal[0]) + sn[2] * e.normal[2];
            int r;
            if (dot < 0.99000001f || !Overlap(sb, e.bounds, 819))
            {
                ++g_prefiltered;
                r = 0;
                if (g_verify && g_pairTest(surf, other)) ++g_mismatches;
            }
            else r = g_pairTest(surf, other);
            if (!r) continue;
            if (r == 1) g_merge(surf, other);
            else g_merge(other, surf);
            return 1;
        }
        return 0;
    }

    // 0xE9950: split axis for a list, one walk instead of three.
    inline bool Chooser(int* out, ListNode* list, int count, const int* center)
    {
        ++g_choices;
        g_scratch.clear();
        for (const ListNode* n = list; n; n = n->next)
        {
            const int* b = reinterpret_cast<const int*>(n->surf + 24);
            g_scratch.insert(g_scratch.end(), b, b + 6);
        }
        int result[4] = { -1, 0, 0, 0 };
        const size_t items = g_scratch.size() / 6;
        for (int i = 0; i < 3; ++i)
        {
            int low = 0, high = 0, straddle = 0;
            const int c = center[i];
            for (size_t k = 0; k < items; ++k)
            {
                const int* b = g_scratch.data() + 6 * k;
                if (b[i] > c + 819) ++high;
                else if (b[3 + i] < c - 819) ++low;
                else ++straddle;
            }
            if (straddle + low != count && straddle + high != count &&
                (result[0] == -1 || straddle + abs(low - high) < result[3] + abs(result[1] - result[2])))
            { result[0] = i; result[1] = low; result[2] = high; result[3] = straddle; }
        }
        if (g_verify)
        {
            int stock[4] = { 0, 0, 0, 0 };
            const bool rs = g_chooser(stock, list, count, center);
            if (rs != (result[0] != -1) || stock[0] != result[0] || (rs && memcmp(stock, result, sizeof stock))) { ++g_mismatches; memcpy(result, stock, sizeof result); }
        }
        memcpy(out, result, sizeof result);
        return result[0] != -1;
    }

    inline int64_t Group(int64_t group)
    {
        Reset();
        const int64_t r = g_group(group);
        Reset();
        return r;
    }

    inline bool Verify(uintptr_t base)
    {
        using namespace cod2mapaddr;
        return !memcmp(reinterpret_cast<void*>(base + CoalesceGroupRva), CoalesceGroupSig, sizeof CoalesceGroupSig) &&
               !memcmp(reinterpret_cast<void*>(base + CoalesceChooserRva), CoalesceChooserSig, sizeof CoalesceChooserSig) &&
               !memcmp(reinterpret_cast<void*>(base + CoalesceAppendRva), CoalesceAppendSig, sizeof CoalesceAppendSig) &&
               !memcmp(reinterpret_cast<void*>(base + CoalesceRemoveRva), CoalesceRemoveSig, sizeof CoalesceRemoveSig) &&
               !memcmp(reinterpret_cast<void*>(base + CoalesceLeafScanRva), CoalesceLeafScanSig, sizeof CoalesceLeafScanSig) &&
               !memcmp(reinterpret_cast<void*>(base + CoalescePairTestRva), CoalescePairTestSig, sizeof CoalescePairTestSig);
    }

    inline bool Install(uintptr_t base, bool verify)
    {
        using namespace cod2mapaddr;
        g_verify = verify;
        g_pairTest = reinterpret_cast<PairTestFn>(base + CoalescePairTestRva);
        g_merge = reinterpret_cast<MergeFn>(base + CoalesceMergeRva);
        g_new = reinterpret_cast<NewFn>(base + CollisionListNewRva);
        g_delete = reinterpret_cast<DeleteFn>(base + CoalesceListDeleteRva);
        g_scratch.reserve(6 * 4096);
        return MH_CreateHook(reinterpret_cast<void*>(base + CoalesceGroupRva), reinterpret_cast<void*>(&Group), reinterpret_cast<void**>(&g_group)) == MH_OK &&
               MH_CreateHook(reinterpret_cast<void*>(base + CoalesceChooserRva), reinterpret_cast<void*>(&Chooser), reinterpret_cast<void**>(&g_chooser)) == MH_OK &&
               MH_CreateHook(reinterpret_cast<void*>(base + CoalesceAppendRva), reinterpret_cast<void*>(&Append), reinterpret_cast<void**>(&g_append)) == MH_OK &&
               MH_CreateHook(reinterpret_cast<void*>(base + CoalesceRemoveRva), reinterpret_cast<void*>(&Remove), reinterpret_cast<void**>(&g_remove)) == MH_OK &&
               MH_CreateHook(reinterpret_cast<void*>(base + CoalesceLeafScanRva), reinterpret_cast<void*>(&Scan), reinterpret_cast<void**>(&g_scan)) == MH_OK;
    }

    inline void Report(FILE* out)
    {
        fprintf(out, "[BetterBuild] Coalesce Tree: %llu appends, %llu removes, %llu leaf scans over %llu items (%llu prefiltered), %llu axis choices, %llu list rebuilds",
                g_appends, g_removes, g_scans, g_scanItems, g_prefiltered, g_choices, g_rebuilds);
        if (g_verify) fprintf(out, ", %llu verify mismatches", g_mismatches);
        fprintf(out, ".\n");
    }
}
