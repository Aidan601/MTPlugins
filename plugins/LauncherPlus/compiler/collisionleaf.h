#pragma once
// C13: collision BSP leaf id test (collision.cpp). The per-context driver pushes every collision triangle down the
// tree with its partition id; at a leaf stock walks a singly linked {next, id} list to see whether the id is
// present before prepending a node. A side set per leaf answers the test, prepends stay stock (the exe's new).
// The set is reset around each driver call (leaves are freed with the tree after it).
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include "../../../src/radiant_addrs.h"
#include "../../../src/third_party/minhook/include/MinHook.h"

namespace collisionleaf
{
    using PushFn = int (*)(void* winding, unsigned id, uint8_t* node);
    using DriverFn = int64_t (*)(int64_t tree);
    using FreeWindingFn = void (*)(void* winding);
    using NewFn = void* (*)(size_t bytes);

    struct ListNode { ListNode* next; unsigned id; unsigned pad; };
    struct Leaf { uint8_t* node; ListNode* head; uint32_t index; };

    inline PushFn g_push = nullptr;        // trampoline
    inline DriverFn g_driver = nullptr;    // trampoline
    inline FreeWindingFn g_freeWinding = nullptr;
    inline NewFn g_new = nullptr;
    inline bool g_verify = false;
    inline unsigned long long g_tests = 0, g_hits = 0, g_rebuilds = 0, g_mismatches = 0;
    inline std::vector<Leaf> g_leaves(1024);      // open addressing by node pointer, node == nullptr is empty
    inline std::vector<uint64_t> g_ids(1 << 16);  // open addressing, key (index + 1) << 32 | id, 0 is empty
    inline size_t g_leafCount = 0, g_idCount = 0;
    inline uint32_t g_nextIndex = 0;

    inline size_t HashPtr(const void* p) { uint64_t h = reinterpret_cast<uintptr_t>(p); h ^= h >> 33; h *= 0xFF51AFD7ED558CCDull; h ^= h >> 33; return size_t(h); }
    inline size_t HashKey(uint64_t k) { k ^= k >> 31; k *= 0x9E3779B97F4A7C15ull; return size_t(k ^ (k >> 29)); }

    inline void Reset()
    {
        if (g_leafCount) std::fill(g_leaves.begin(), g_leaves.end(), Leaf{});
        if (g_idCount) std::fill(g_ids.begin(), g_ids.end(), 0ull);
        g_leafCount = g_idCount = 0;
        g_nextIndex = 0;
    }

    inline bool HasId(uint64_t key)
    {
        const size_t mask = g_ids.size() - 1;
        for (size_t i = HashKey(key) & mask;; i = (i + 1) & mask)
        {
            if (g_ids[i] == key) return true;
            if (!g_ids[i]) return false;
        }
    }

    inline void AddId(uint64_t key)
    {
        if (g_idCount * 2 >= g_ids.size())
        {
            std::vector<uint64_t> old(g_ids.size() * 2);
            old.swap(g_ids);
            const size_t mask = g_ids.size() - 1;
            for (uint64_t k : old)
            {
                if (!k) continue;
                size_t i = HashKey(k) & mask;
                while (g_ids[i]) i = (i + 1) & mask;
                g_ids[i] = k;
            }
        }
        const size_t mask = g_ids.size() - 1;
        size_t i = HashKey(key) & mask;
        while (g_ids[i] && g_ids[i] != key) i = (i + 1) & mask;
        if (!g_ids[i]) { g_ids[i] = key; ++g_idCount; }
    }

    inline Leaf& FindLeaf(uint8_t* node)
    {
        if (g_leafCount * 2 >= g_leaves.size())
        {
            std::vector<Leaf> old(g_leaves.size() * 2);
            old.swap(g_leaves);
            const size_t mask = g_leaves.size() - 1;
            for (const Leaf& l : old)
            {
                if (!l.node) continue;
                size_t i = HashPtr(l.node) & mask;
                while (g_leaves[i].node) i = (i + 1) & mask;
                g_leaves[i] = l;
            }
        }
        const size_t mask = g_leaves.size() - 1;
        size_t i = HashPtr(node) & mask;
        while (g_leaves[i].node && g_leaves[i].node != node) i = (i + 1) & mask;
        Leaf& l = g_leaves[i];
        if (!l.node) { l.node = node; l.head = reinterpret_cast<ListNode*>(~uintptr_t(0)); l.index = 0; ++g_leafCount; }
        return l;
    }

    // Stock's test: the id is somewhere in the leaf's list.
    inline bool StockHas(const ListNode* head, unsigned id)
    {
        for (const ListNode* n = head; n; n = n->next) if (n->id == id) return true;
        return false;
    }

    inline uint64_t Key(uint32_t index, unsigned id) { return (uint64_t(index) + 1) << 32 | id; }

    inline int Push(void* winding, unsigned id, uint8_t* node)
    {
        if (!winding || *reinterpret_cast<int*>(node) != -1 || *reinterpret_cast<int*>(node + 48) == 1) return g_push(winding, id, node);
        g_freeWinding(winding);
        ++g_tests;
        ListNode** headSlot = reinterpret_cast<ListNode**>(node + 120);
        Leaf& leaf = FindLeaf(node);
        if (leaf.head != *headSlot)
        {
            // a list we did not build (first sight, or changed behind us): index afresh and take its ids
            ++g_rebuilds;
            leaf.index = g_nextIndex++;
            for (const ListNode* n = *headSlot; n; n = n->next) AddId(Key(leaf.index, n->id));
            leaf.head = *headSlot;
        }
        bool found = HasId(Key(leaf.index, id));
        if (g_verify)
        {
            const bool stock = StockHas(*headSlot, id);
            if (stock != found) { ++g_mismatches; found = stock; }
        }
        if (found) { ++g_hits; return 0; }
        auto* n = static_cast<ListNode*>(g_new(sizeof(ListNode)));
        n->id = id;
        n->next = *headSlot;
        *headSlot = n;
        leaf.head = n;
        AddId(Key(leaf.index, id));
        return 1;
    }

    inline int64_t Driver(int64_t tree)
    {
        Reset();
        const int64_t r = g_driver(tree);
        Reset();
        return r;
    }

    inline bool Verify(uintptr_t base)
    {
        using namespace cod2mapaddr;
        return !memcmp(reinterpret_cast<void*>(base + CollisionPushRva), CollisionPushSig, sizeof CollisionPushSig) &&
               !memcmp(reinterpret_cast<void*>(base + CollisionPushDriverRva), CollisionPushDriverSig, sizeof CollisionPushDriverSig) &&
               !memcmp(reinterpret_cast<void*>(base + FreeWindingRva), FreeWindingSig, sizeof FreeWindingSig);
    }

    inline bool Install(uintptr_t base, bool verify)
    {
        using namespace cod2mapaddr;
        g_verify = verify;
        g_freeWinding = reinterpret_cast<FreeWindingFn>(base + FreeWindingRva);
        g_new = reinterpret_cast<NewFn>(base + CollisionListNewRva);
        return MH_CreateHook(reinterpret_cast<void*>(base + CollisionPushRva), reinterpret_cast<void*>(&Push), reinterpret_cast<void**>(&g_push)) == MH_OK &&
               MH_CreateHook(reinterpret_cast<void*>(base + CollisionPushDriverRva), reinterpret_cast<void*>(&Driver), reinterpret_cast<void**>(&g_driver)) == MH_OK;
    }

    inline void Report(FILE* out)
    {
        fprintf(out, "[BetterBuild] Collision Leaf Set: %llu leaf tests, %llu already present, %llu list rebuilds", g_tests, g_hits, g_rebuilds);
        if (g_verify) fprintf(out, ", %llu verify mismatches (stock kept)", g_mismatches);
        fprintf(out, ".\n");
    }
}
