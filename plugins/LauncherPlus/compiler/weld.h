#pragma once
// C6: vertex weld grid (GenerateComplexWindings, EmitTrianglesForContext). One chain per exact cell instead of 16,384
// shared buckets: same matches and order, stock's compare. Allocation is replicated through the exe's new/delete
// so the process heap sees stock's sequence.
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../../../src/radiant_addrs.h"
#include "../../../src/third_party/minhook/include/MinHook.h"

namespace weld
{
    using WeldFn = int* (*)(const float* base, int coordCount, int stride, int count, float eps);
    using EqualFn = bool (*)(const float* a, const float* b, float eps, int n);
    using NewFn = void* (*)(size_t);
    using DeleteFn = void (*)(void*);

    inline WeldFn g_weld = nullptr;  // trampoline
    inline EqualFn g_equal = nullptr;
    inline NewFn g_new = nullptr;
    inline DeleteFn g_delete = nullptr;
    inline int* g_buckets = nullptr;
    inline bool g_verify = false;
    inline unsigned long long g_calls = 0, g_stockCalls = 0, g_vertices = 0, g_mismatches = 0;

    struct CellTable
    {
        struct Slot { int x, y, z; int head; };  // head -1 = empty slot
        std::vector<Slot> slots;
        size_t mask = 0;

        void Reset(size_t vertices)
        {
            size_t size = 1024;
            while (size < vertices * 2) size <<= 1;
            slots.assign(size, Slot{ 0, 0, 0, -1 });
            mask = size - 1;
        }
        static size_t Hash(int x, int y, int z)
        {
            uint64_t h = uint32_t(x) * 0x9E3779B97F4A7C15ull;
            h ^= uint32_t(y) * 0xC2B2AE3D27D4EB4Full + (h >> 29);
            h ^= uint32_t(z) * 0x165667B19E3779F9ull + (h >> 32);
            return size_t(h ^ (h >> 31));
        }
        int* Find(int x, int y, int z, bool create)
        {
            for (size_t i = Hash(x, y, z) & mask;; i = (i + 1) & mask)
            {
                Slot& s = slots[i];
                if (s.head == -1)
                {
                    if (!create) return nullptr;
                    s = { x, y, z, -1 };
                    s.head = -2;  // claimed, no vertex yet
                    return &s.head;
                }
                if (s.x == x && s.y == y && s.z == z) return &s.head;
            }
        }
    };

    inline int Round(float v) { return int(floorf(v + 0.5f)); }  // stock: floorf(v + 0.5f), truncating conversion

    // fills map (count ints) as stock does, next is scratch of count ints
    inline void Compute(const float* base, int coordCount, int stride, int count, float eps, int* map, int* next)
    {
        static CellTable table;
        table.Reset(size_t(count));
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(base);
        for (int i = 0; i < count; ++i)
        {
            const float* p = reinterpret_cast<const float*>(bytes + size_t(stride) * i);
            const int rz = coordCount == 3 ? Round(p[2]) : 0;
            int* head = table.Find(Round(p[0]), Round(p[1]), rz, true);
            next[i] = *head == -2 ? -1 : *head;
            *head = i;
            map[i] = i;

            int lo[3], hi[3];
            for (int k = 0; k < 3; ++k)
            {
                if (k < coordCount) { lo[k] = Round(p[k] - eps); hi[k] = Round(p[k] + eps); }
                else lo[k] = hi[k] = 0;
            }
            for (int z = lo[2]; z <= hi[2]; ++z)
                for (int y = lo[1]; y <= hi[1]; ++y)
                    for (int x = lo[0]; x <= hi[0]; ++x)
                    {
                        const int* cell = table.Find(x, y, z, false);
                        if (!cell || *cell < 0) continue;
                        int j = *cell == i ? next[i] : *cell;
                        for (; j != -1; j = next[j])
                            if (g_equal(reinterpret_cast<const float*>(bytes + size_t(stride) * j), p, eps, coordCount)) map[i] = map[j];
                    }
        }
    }

    inline int* Weld(const float* base, int coordCount, int stride, int count, float eps)
    {
        ++g_calls;
        // stock asserts on these
        if (!base || count <= 0 || (coordCount != 2 && coordCount != 3) || stride < 4 * coordCount || !(eps < 0.5f))
        { ++g_stockCalls; return g_weld(base, coordCount, stride, count, eps); }
        g_vertices += unsigned(count);
        if (g_verify)
        {
            std::vector<int> map(count), next(count);
            Compute(base, coordCount, stride, count, eps, map.data(), next.data());
            int* stock = g_weld(base, coordCount, stride, count, eps);
            if (memcmp(stock, map.data(), sizeof(int) * size_t(count))) ++g_mismatches;
            return stock;
        }
        memset(g_buckets, 0xFF, sizeof(int) * 16384);  // stock leaves this static table reset
        int* map = static_cast<int*>(g_new(sizeof(int) * size_t(count)));   // same allocation sequence as stock
        int* next = static_cast<int*>(g_new(sizeof(int) * size_t(count)));
        Compute(base, coordCount, stride, count, eps, map, next);
        g_delete(next);
        return map;
    }

    inline bool Verify(uintptr_t base)
    {
        using namespace cod2mapaddr;
        return !memcmp(reinterpret_cast<void*>(base + WeldRva), WeldSig, sizeof WeldSig) &&
               !memcmp(reinterpret_cast<void*>(base + EpsilonEqualRva), EpsilonEqualSig, sizeof EpsilonEqualSig);
    }

    inline bool Install(uintptr_t base, bool verify)
    {
        using namespace cod2mapaddr;
        g_verify = verify;
        g_equal = reinterpret_cast<EqualFn>(base + EpsilonEqualRva);
        g_new = reinterpret_cast<NewFn>(base + ExeNewRva);
        g_delete = reinterpret_cast<DeleteFn>(base + ExeDeleteRva);
        g_buckets = reinterpret_cast<int*>(base + WeldBucketsRva);
        return MH_CreateHook(reinterpret_cast<void*>(base + WeldRva), reinterpret_cast<void*>(&Weld),
                             reinterpret_cast<void**>(&g_weld)) == MH_OK;
    }

    inline void Report(FILE* out)
    {
        fprintf(out, "[BetterBuild] Weld Grid: %llu calls (%llu stock), %llu vertices", g_calls, g_stockCalls, g_vertices);
        if (g_verify) fprintf(out, ", %llu verify mismatches (stock kept)", g_mismatches);
        fprintf(out, ".\n");
    }
}
