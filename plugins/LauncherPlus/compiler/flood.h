#pragma once
// C8: collision partition flood fill (BuildCollision). Stock repeats full passes per partition. The result is
// the components of triangles sharing a vertex index, numbered by lowest triangle index: a union-find and one
// ascending walk give the same labels. Inputs stock would assert on go to stock.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../../../src/radiant_addrs.h"
#include "../../../src/third_party/minhook/include/MinHook.h"

namespace flood
{
    using FloodFn = void (*)();
    inline FloodFn g_flood = nullptr;  // trampoline
    inline uint8_t** g_tris = nullptr;
    inline uint8_t** g_verts = nullptr;
    inline int* g_first = nullptr;
    inline int* g_end = nullptr;
    inline int* g_counter = nullptr;
    inline bool g_verify = false;
    inline unsigned long long g_calls = 0, g_stockCalls = 0, g_tris_seen = 0, g_partitions = 0, g_mismatches = 0;

    inline int& TriLabel(int t) { return *reinterpret_cast<int*>(*g_tris + 120ull * t); }
    inline const uint32_t* TriVerts(int t) { return reinterpret_cast<const uint32_t*>(*g_tris + 120ull * t) + 26; }
    inline int& VertLabel(uint32_t v) { return *reinterpret_cast<int*>(*g_verts + 16ull * v); }

    // labels per triangle (index t - first), false when stock must run
    inline bool Compute(int first, int end, int counter, std::vector<int>& triLabels, int& lastLabel)
    {
        uint32_t maxVert = 0;
        for (int t = first; t < end; ++t)
        {
            if (TriLabel(t)) return false;
            const uint32_t* v = TriVerts(t);
            for (int k = 0; k < 3; ++k)
            {
                if (VertLabel(v[k])) return false;
                if (v[k] > maxVert) maxVert = v[k];
            }
        }
        static std::vector<uint32_t> parent;
        parent.resize(size_t(maxVert) + 1);
        for (int t = first; t < end; ++t)
        {
            const uint32_t* v = TriVerts(t);
            for (int k = 0; k < 3; ++k) parent[v[k]] = v[k];
        }
        auto find = [&](uint32_t x) {
            while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
            return x;
        };
        for (int t = first; t < end; ++t)
        {
            const uint32_t* v = TriVerts(t);
            const uint32_t a = find(v[0]);
            for (int k = 1; k < 3; ++k)
            {
                const uint32_t b = find(v[k]);
                if (a != b) parent[b] = a;
            }
        }
        triLabels.resize(size_t(end - first));
        static std::vector<int> rootLabel;
        rootLabel.assign(size_t(maxVert) + 1, 0);
        for (int t = first; t < end; ++t)
        {
            const uint32_t* v = TriVerts(t);
            int& label = rootLabel[find(v[0])];
            if (!label) label = ++counter;
            triLabels[size_t(t - first)] = label;
        }
        lastLabel = counter;
        return true;
    }

    inline void Flood()
    {
        ++g_calls;
        const int first = *g_first, end = *g_end, counter = *g_counter;
        static std::vector<int> triLabels;
        int last = 0;
        if (first >= end || !Compute(first, end, counter, triLabels, last)) { ++g_stockCalls; g_flood(); return; }
        g_tris_seen += unsigned(end - first);
        g_partitions += unsigned(last - counter);
        if (g_verify)
        {
            g_flood();
            bool same = *g_counter == last;
            for (int t = first; t < end && same; ++t)
            {
                const int label = triLabels[size_t(t - first)];
                const uint32_t* v = TriVerts(t);
                same = TriLabel(t) == label && VertLabel(v[0]) == label && VertLabel(v[1]) == label && VertLabel(v[2]) == label;
            }
            if (!same) ++g_mismatches;
            return;
        }
        for (int t = first; t < end; ++t)
        {
            const int label = triLabels[size_t(t - first)];
            TriLabel(t) = label;
            const uint32_t* v = TriVerts(t);
            for (int k = 0; k < 3; ++k) VertLabel(v[k]) = label;
        }
        *g_counter = last;
    }

    inline bool Verify(uintptr_t base)
    {
        using namespace cod2mapaddr;
        return !memcmp(reinterpret_cast<void*>(base + CollisionFloodRva), CollisionFloodSig, sizeof CollisionFloodSig);
    }

    inline bool Install(uintptr_t base, bool verify)
    {
        using namespace cod2mapaddr;
        g_verify = verify;
        g_tris = reinterpret_cast<uint8_t**>(base + CollisionTrisRva);
        g_verts = reinterpret_cast<uint8_t**>(base + CollisionVertsRva);
        g_first = reinterpret_cast<int*>(base + CollisionFirstRva);
        g_end = reinterpret_cast<int*>(base + CollisionEndRva);
        g_counter = reinterpret_cast<int*>(base + CollisionCounterRva);
        return MH_CreateHook(reinterpret_cast<void*>(base + CollisionFloodRva), reinterpret_cast<void*>(&Flood),
                             reinterpret_cast<void**>(&g_flood)) == MH_OK;
    }

    inline void Report(FILE* out)
    {
        fprintf(out, "[BetterBuild] Collision Flood: %llu calls (%llu stock), %llu triangles, %llu partitions", g_calls, g_stockCalls,
                g_tris_seen, g_partitions);
        if (g_verify) fprintf(out, ", %llu verify mismatches (stock kept)", g_mismatches);
        fprintf(out, ".\n");
    }
}
