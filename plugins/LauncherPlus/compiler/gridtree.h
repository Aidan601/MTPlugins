#pragma once
// C2: grid tree queries that skip empty subtrees. Hooks on the list writers keep a live item count per subtree
// (rebuild or free switch skipping off until the next reset). Walks keep stock's order and comparisons and skip
// a child only when its count is zero. Verify mode proves every skipped subtree empty.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <limits>
#include <intrin.h>
#include <xmmintrin.h>
#include "../../../src/radiant_addrs.h"
#include "../../../src/third_party/minhook/include/MinHook.h"

namespace gridtree
{
    constexpr unsigned kDepth = 20;
    constexpr uint32_t kNodes = (1u << (kDepth + 1)) - 1;

    struct Node { float lo, hi; uint8_t* head; };
    static_assert(sizeof(Node) == 16, "compiler layout");

    inline const float* Mins(const uint8_t* item) { return reinterpret_cast<const float*>(item + 24); }
    inline const float* Maxs(const uint8_t* item) { return reinterpret_cast<const float*>(item + 36); }
    inline uint8_t* Prev(const uint8_t* item) { return *reinterpret_cast<uint8_t* const*>(item + 72); }
    inline uint8_t* Next(const uint8_t* item) { return *reinterpret_cast<uint8_t* const*>(item + 80); }

    using Callback = void (*)(uint8_t* item, uint64_t depth, uint64_t node, const int* axes);
    using InsertFn = void (*)(uint8_t* item);
    using RemoveFn = void (*)(uint8_t* item);
    using ResetFn = void* (*)(const int* mins, const int* maxs);
    using VoidFn = void (*)();
    using DescendFn = Node* (*)(const float* mins, const float* maxs);
    using QueryFn = float (*)(const float* mins, const float* maxs, uint64_t node, int depth, Callback cb);
    using CountFn = int (*)(const float* mins, const float* maxs, int node, int depth, int limit);
    using OverlapFn = bool (*)(const float* amins, const float* amaxs, const float* bmins, const float* bmaxs);

    inline Node** g_nodes = nullptr;
    using AllocFn = void* (*)();
    inline AllocFn g_alloc = nullptr;   // stock: operator new(0x1FFFFF0) per GenerateComplexWindings call
    inline void* g_persist = nullptr;   // one buffer for the whole compile
    inline unsigned long long g_allocsSkipped = 0;
    inline const bool* g_small = nullptr;
    inline const int* g_axes = nullptr;
    inline InsertFn g_insert = nullptr;
    inline RemoveFn g_remove = nullptr;
    inline ResetFn g_reset = nullptr;
    inline VoidFn g_rebuild = nullptr, g_free = nullptr;
    inline QueryFn g_query = nullptr;
    inline CountFn g_count = nullptr;
    inline DescendFn g_descend = nullptr;
    inline OverlapFn g_overlap = nullptr;
    inline bool g_verify = false;

    inline bool g_valid = false;                   // counts and packed arrays track the lists (since the last reset)
    inline std::vector<uint32_t> g_subtree;        // items in each node's subtree
    inline std::vector<uint32_t> g_dirty;          // nodes whose count left zero since the last reset
    inline uint64_t g_mutations = 0;               // bumped by every list change we see

    // A node's items in list order, stored reversed (back() is the head) so a head push is an append.
    // Removed items become tombstones: null item, NaN bounds, which never pass the overlap test.
    struct Packed
    {
        std::vector<float> lo[3], hi[3];
        std::vector<uint8_t*> items;
        uint32_t dead = 0;
        bool synced = true;  // false once stock left the list in a shape we do not model
    };
    struct Slot { uint32_t node, index; };
    inline std::vector<Packed*> g_packed;          // per node, created on first insert
    inline std::vector<uint32_t> g_packedNodes;    // nodes holding a Packed
    inline std::unordered_map<uint8_t*, Slot> g_slots;
    inline unsigned long long g_packedCounts = 0;
    inline unsigned long long g_queries = 0, g_callbacks = 0, g_packedScans = 0, g_linkedScans = 0, g_countItemTests = 0;
    inline unsigned long long g_visited = 0, g_skipped = 0, g_verifyFailures = 0, g_rebuilds = 0, g_unsynced = 0;

    inline void AddPath(uint32_t node, int delta)
    {
        for (;;)
        {
            uint32_t& c = g_subtree[node];
            if (!c) g_dirty.push_back(node);
            c += delta;
            if (!node) break;
            node = (node - 1) / 2;
        }
    }

    inline void Append(uint32_t node, uint8_t* item)
    {
        Packed*& p = g_packed[node];
        if (!p) { p = new Packed; g_packedNodes.push_back(node); }
        const float* lo = Mins(item);
        const float* hi = Maxs(item);
        for (int k = 0; k < 3; ++k) { p->lo[k].push_back(lo[k]); p->hi[k].push_back(hi[k]); }
        p->items.push_back(item);
        g_slots[item] = { node, uint32_t(p->items.size() - 1) };
    }

    inline void Compact(Packed& p)
    {
        size_t out = 0;
        for (size_t i = 0; i < p.items.size(); ++i)
        {
            if (!p.items[i]) continue;
            for (int k = 0; k < 3; ++k) { p.lo[k][out] = p.lo[k][i]; p.hi[k][out] = p.hi[k][i]; }
            p.items[out] = p.items[i];
            g_slots[p.items[out]].index = uint32_t(out);
            ++out;
        }
        for (int k = 0; k < 3; ++k) { p.lo[k].resize(out); p.hi[k].resize(out); }
        p.items.resize(out);
        p.dead = 0;
    }

    inline void Kill(const Slot& slot)
    {
        Packed& p = *g_packed[slot.node];
        const float nan = std::numeric_limits<float>::quiet_NaN();
        for (int k = 0; k < 3; ++k) { p.lo[k][slot.index] = nan; p.hi[k][slot.index] = nan; }
        p.items[slot.index] = nullptr;
        if (++p.dead > 32 && p.dead * 2 > p.items.size()) Compact(p);
    }

    inline void DropPacked()
    {
        for (uint32_t n : g_packedNodes) { delete g_packed[n]; g_packed[n] = nullptr; }
        g_packedNodes.clear();
        g_slots.clear();
    }

    inline void Invalidate()
    {
        g_valid = false;
        DropPacked();
    }

    // verify mode: is every list in the subtree empty?
    inline bool SubtreeEmpty(uint32_t root)
    {
        std::vector<uint32_t> stack{ root };
        while (!stack.empty())
        {
            const uint32_t n = stack.back();
            stack.pop_back();
            if ((*g_nodes)[n].head) return false;
            if (2 * n + 2 < kNodes) { stack.push_back(2 * n + 1); stack.push_back(2 * n + 2); }
        }
        return true;
    }

    // verify mode: does the packed array equal the live list?
    inline bool Matches(const Node& n, const Packed& p)
    {
        size_t i = p.items.size();
        for (uint8_t* item = n.head; item; item = Next(item))
        {
            while (i && !p.items[i - 1]) --i;
            if (!i || p.items[--i] != item) return false;
            for (int k = 0; k < 3; ++k)
                if (memcmp(&p.lo[k][i], Mins(item) + k, 4) || memcmp(&p.hi[k][i], Maxs(item) + k, 4)) return false;
        }
        while (i && !p.items[i - 1]) --i;
        return !i;
    }

    // skip a child only with valid counts and an empty subtree
    inline bool Skip(uint32_t child)
    {
        if (!g_valid || g_subtree[child]) return false;
        ++g_skipped;
        if (g_verify && !SubtreeEmpty(child)) { ++g_verifyFailures; return false; }
        return true;
    }

    inline void Insert(uint8_t* item)
    {
        ++g_mutations;
        if (!g_valid || !*g_nodes) { g_insert(item); return; }
        const uint32_t node = uint32_t(g_descend(Mins(item), Maxs(item)) - *g_nodes);
        g_insert(item);
        if (node < kNodes && (*g_nodes)[node].head == item && !g_slots.count(item)) { AddPath(node, +1); Append(node, item); }
        else Invalidate();  // not the push we model: stock order everywhere until the next reset
    }

    inline void Remove(uint8_t* item)
    {
        ++g_mutations;
        g_remove(item);
        if (!g_valid || !*g_nodes) return;
        const auto it = g_slots.find(item);
        if (it == g_slots.end()) return;
        // stock leaves the item linked but clears its next pointer unless it is mid-list or the node's head
        if (Prev(item) || (*g_nodes)[it->second.node].head == item)
        {
            ++g_unsynced;
            g_packed[it->second.node]->synced = false;
            return;
        }
        AddPath(it->second.node, -1);
        Kill(it->second);
        g_slots.erase(it);
    }

    inline void* Reset(const int* mins, const int* maxs)
    {
        ++g_mutations;
        void* result = g_reset(mins, maxs);
        for (uint32_t n : g_dirty) g_subtree[n] = 0;
        g_dirty.clear();
        DropPacked();
        g_valid = true;
        return result;
    }

    inline void Rebuild() { ++g_mutations; ++g_rebuilds; Invalidate(); g_rebuild(); }
    // already allocated or freed: stock asserts, so those cases go to stock
    inline void* Alloc()
    {
        if (*g_nodes) return g_alloc();
        if (!g_persist) g_persist = VirtualAlloc(nullptr, 0x2000000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_persist) return g_alloc();
        ++g_allocsSkipped;
        *g_nodes = static_cast<Node*>(g_persist);
        return g_persist;
    }
    inline void Free()
    {
        ++g_mutations; Invalidate();
        if (g_persist && *g_nodes == g_persist) { *g_nodes = nullptr; return; }
        g_free();
    }

    // stock's per-node loop: test, callback, then read next (a callback may unlink the item)
    inline void LinkedScan(uint8_t* item, const float* mins, const float* maxs, uint32_t node, int depth, Callback cb)
    {
        ++g_linkedScans;
        while (item)
        {
            const float* lo = Mins(item);
            const float* hi = Maxs(item);
            if (lo[0] <= maxs[0] && mins[0] <= hi[0] && lo[1] <= maxs[1] && mins[1] <= hi[1] &&
                lo[2] <= maxs[2] && mins[2] <= hi[2])
            {
                ++g_callbacks;
                cb(item, uint32_t(depth + 1), node, g_axes);
            }
            item = Next(item);
        }
    }

    // same order as LinkedScan, four items at a time, the first callback that changes a list hands the rest to LinkedScan
    inline void PackedScan(const Packed& p, const float* mins, const float* maxs, uint32_t node, int depth, Callback cb)
    {
        ++g_packedScans;
        const __m128 qlo0 = _mm_set1_ps(mins[0]), qlo1 = _mm_set1_ps(mins[1]), qlo2 = _mm_set1_ps(mins[2]);
        const __m128 qhi0 = _mm_set1_ps(maxs[0]), qhi1 = _mm_set1_ps(maxs[1]), qhi2 = _mm_set1_ps(maxs[2]);
        size_t i = p.items.size();
        while (i)
        {
            const size_t base = i >= 4 ? i - 4 : 0;
            unsigned bits;
            if (i >= 4)
            {
                __m128 m = _mm_and_ps(_mm_cmple_ps(_mm_loadu_ps(&p.lo[0][base]), qhi0), _mm_cmple_ps(qlo0, _mm_loadu_ps(&p.hi[0][base])));
                m = _mm_and_ps(m, _mm_and_ps(_mm_cmple_ps(_mm_loadu_ps(&p.lo[1][base]), qhi1), _mm_cmple_ps(qlo1, _mm_loadu_ps(&p.hi[1][base]))));
                m = _mm_and_ps(m, _mm_and_ps(_mm_cmple_ps(_mm_loadu_ps(&p.lo[2][base]), qhi2), _mm_cmple_ps(qlo2, _mm_loadu_ps(&p.hi[2][base]))));
                bits = unsigned(_mm_movemask_ps(m));
            }
            else
            {
                bits = 0;
                for (size_t j = 0; j < i; ++j)
                    if (p.lo[0][j] <= maxs[0] && mins[0] <= p.hi[0][j] && p.lo[1][j] <= maxs[1] && mins[1] <= p.hi[1][j] &&
                        p.lo[2][j] <= maxs[2] && mins[2] <= p.hi[2][j])
                        bits |= 1u << j;
            }
            while (bits)
            {
                unsigned long lane;
                _BitScanReverse(&lane, bits);
                bits &= ~(1u << lane);
                uint8_t* item = p.items[base + lane];
                const uint64_t before = g_mutations;
                ++g_callbacks;
                cb(item, uint32_t(depth + 1), node, g_axes);
                if (g_mutations != before) { LinkedScan(Next(item), mins, maxs, node, depth, cb); return; }
            }
            i = base;
        }
    }

    // stock's visit order and comparisons, the return value is never read
    inline float Query(const float* mins, const float* maxs, uint64_t nodeArg, int depth, Callback cb)
    {
        uint32_t node = uint32_t(nodeArg);
        if (!depth) ++g_queries;
        for (;;)
        {
            ++g_visited;
            const Node* n = *g_nodes + node;
            const Packed* p = g_valid ? g_packed[node] : nullptr;
            if (p && p->synced && (!g_verify || Matches(*n, *p))) PackedScan(*p, mins, maxs, node, depth, cb);
            else
            {
                if (g_verify && p && p->synced) ++g_verifyFailures;
                if (g_verify && g_valid && !p && n->head) ++g_verifyFailures;
                LinkedScan(n->head, mins, maxs, node, depth, cb);
            }
            if (depth == int(kDepth) || *g_small) return 0.0f;
            const int axis = g_axes[depth];
            if (n->lo >= mins[axis] && !Skip(2 * node + 2)) Query(mins, maxs, 2 * node + 2, depth + 1, cb);
            if (maxs[axis] < n->hi) return 0.0f;
            node = 2 * node + 1;
            ++depth;
            if (Skip(node)) return 0.0f;
        }
    }

    // stock's count test (0x1226A0) rejects only when a min > the other's max, so NaN overlaps: tombstones are skipped by null item
    inline int PackedCount(const Packed& p, const float* mins, const float* maxs, int limit)
    {
        const __m128 qlo0 = _mm_set1_ps(mins[0]), qlo1 = _mm_set1_ps(mins[1]), qlo2 = _mm_set1_ps(mins[2]);
        const __m128 qhi0 = _mm_set1_ps(maxs[0]), qhi1 = _mm_set1_ps(maxs[1]), qhi2 = _mm_set1_ps(maxs[2]);
        int found = 0;
        const size_t size = p.items.size();
        size_t i = 0;
        for (; i + 4 <= size; i += 4)
        {
            __m128 m = _mm_and_ps(_mm_cmpngt_ps(_mm_loadu_ps(&p.lo[0][i]), qhi0), _mm_cmpngt_ps(qlo0, _mm_loadu_ps(&p.hi[0][i])));
            m = _mm_and_ps(m, _mm_and_ps(_mm_cmpngt_ps(_mm_loadu_ps(&p.lo[1][i]), qhi1), _mm_cmpngt_ps(qlo1, _mm_loadu_ps(&p.hi[1][i]))));
            m = _mm_and_ps(m, _mm_and_ps(_mm_cmpngt_ps(_mm_loadu_ps(&p.lo[2][i]), qhi2), _mm_cmpngt_ps(qlo2, _mm_loadu_ps(&p.hi[2][i]))));
            for (unsigned bits = unsigned(_mm_movemask_ps(m)); bits; bits &= bits - 1)
            {
                unsigned long lane;
                _BitScanForward(&lane, bits);
                if (p.items[i + lane] && ++found > limit) return found;
            }
        }
        for (; i < size; ++i)
            if (p.items[i] && !(p.lo[0][i] > maxs[0]) && !(mins[0] > p.hi[0][i]) && !(p.lo[1][i] > maxs[1]) &&
                !(mins[1] > p.hi[1][i]) && !(p.lo[2][i] > maxs[2]) && !(mins[2] > p.hi[2][i]) && ++found > limit)
                return found;
        return found;
    }

    inline int Count(const float* mins, const float* maxs, int nodeArg, int depth, int limit)
    {
        ++g_visited;
        const uint32_t node = uint32_t(nodeArg);
        const Node* n = *g_nodes + node;
        int found = 0;
        const Packed* p = g_valid ? g_packed[node] : nullptr;
        if (p && p->synced && (!g_verify || Matches(*n, *p)))
        {
            ++g_packedCounts;
            found = PackedCount(*p, mins, maxs, limit);
            if (g_verify)
            {
                int stock = 0;
                for (uint8_t* item = n->head; item; item = Next(item))
                    if (g_overlap(Mins(item), Maxs(item), mins, maxs) && ++stock > limit) break;
                if (stock != found) { ++g_verifyFailures; found = stock; }
            }
            if (found > limit) return found;
        }
        else
        {
            for (uint8_t* item = n->head; item; item = Next(item))
                if (++g_countItemTests && g_overlap(Mins(item), Maxs(item), mins, maxs) && ++found > limit) return found;
        }
        if (depth != int(kDepth) && !*g_small)
        {
            const int axis = g_axes[depth];
            if (n->lo >= mins[axis] && !Skip(2 * node + 2))
            {
                found += Count(mins, maxs, int(2 * node + 2), depth + 1, limit - found);
                if (found > limit) return found;
            }
            if (maxs[axis] >= n->hi && !Skip(2 * node + 1))
                found += Count(mins, maxs, int(2 * node + 1), depth + 1, limit - found);
        }
        return found;
    }

    inline bool Verify(uintptr_t base)
    {
        using namespace cod2mapaddr;
        auto match = [base](uintptr_t rva, const void* sig, size_t size) { return !memcmp(reinterpret_cast<void*>(base + rva), sig, size); };
        return match(GridInsertRva, GridInsertSig, sizeof GridInsertSig) && match(GridRemoveRva, GridRemoveSig, sizeof GridRemoveSig) &&
               match(GridResetRva, GridResetSig, sizeof GridResetSig) && match(GridRebuildRva, GridRebuildSig, sizeof GridRebuildSig) &&
               match(GridAllocRva, GridAllocSig, sizeof GridAllocSig) &&
               match(GridFreeRva, GridFreeSig, sizeof GridFreeSig) && match(GridDescendRva, GridDescendSig, sizeof GridDescendSig) &&
               match(GridQueryRva, GridQuerySig, sizeof GridQuerySig) && match(GridCountRva, GridCountSig, sizeof GridCountSig) &&
               match(BoundsOverlapRva, BoundsOverlapSig, sizeof BoundsOverlapSig) && match(GridAxesRva, GridAxes, sizeof GridAxes);
    }

    inline bool Install(uintptr_t base, bool verify)
    {
        using namespace cod2mapaddr;
        g_verify = verify;
        g_nodes = reinterpret_cast<Node**>(base + GridNodesRva);
        g_small = reinterpret_cast<const bool*>(base + GridSmallRva);
        g_axes = reinterpret_cast<const int*>(base + GridAxesRva);
        g_descend = reinterpret_cast<DescendFn>(base + GridDescendRva);
        g_overlap = reinterpret_cast<OverlapFn>(base + BoundsOverlapRva);
        g_subtree.assign(kNodes, 0);
        g_packed.assign(kNodes, nullptr);
        auto hook = [base](uintptr_t rva, void* detour, void* original) {
            return MH_CreateHook(reinterpret_cast<void*>(base + rva), detour, reinterpret_cast<void**>(original)) == MH_OK;
        };
        return hook(GridInsertRva, reinterpret_cast<void*>(&Insert), &g_insert) &&
               hook(GridRemoveRva, reinterpret_cast<void*>(&Remove), &g_remove) &&
               hook(GridResetRva, reinterpret_cast<void*>(&Reset), &g_reset) &&
               hook(GridRebuildRva, reinterpret_cast<void*>(&Rebuild), &g_rebuild) &&
               hook(GridAllocRva, reinterpret_cast<void*>(&Alloc), &g_alloc) &&
               hook(GridFreeRva, reinterpret_cast<void*>(&Free), &g_free) &&
               hook(GridQueryRva, reinterpret_cast<void*>(&Query), &g_query) &&
               hook(GridCountRva, reinterpret_cast<void*>(&Count), &g_count);
    }

    inline void Report(FILE* out)
    {
        fprintf(out, "[BetterBuild] Grid Tree: %llu queries, %llu callbacks, %llu packed node scans, %llu stock-order node scans, "
                "%llu empty subtrees skipped, %llu packed counts, %llu stock-order count item tests, %llu rebuilds, %llu unmodelled removes",
                g_queries, g_callbacks, g_packedScans, g_linkedScans, g_skipped, g_packedCounts, g_countItemTests, g_rebuilds, g_unsynced);
        if (g_verify) fprintf(out, ", %llu verify failures (stock order used)", g_verifyFailures);
        fprintf(out, ", %llu node array allocations kept persistent", g_allocsSkipped);
        fprintf(out, ".\n");
    }
}
