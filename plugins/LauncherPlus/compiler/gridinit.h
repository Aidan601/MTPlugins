#pragma once
// C7: grid tree initialisation. Stock recurses through all 2,097,151 nodes of the depth-20 tree. The same
// float midpoints are computed per depth and streamed breadth-first (grid_init_impl.h). Only the root call
// is replaced: non-finite or overflowing bounds and every other call go to stock.
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../../../src/radiant_addrs.h"
#include "../../../src/third_party/minhook/include/MinHook.h"
#include "grid_init_impl.h"

namespace gridinit
{
    using InitFn = void (*)(int index, const float* mins, const float* maxs, int depth);
    inline InitFn g_init = nullptr;  // trampoline
    inline gridinit_impl::GridNode** g_nodes = nullptr;
    inline const gridinit_impl::GridPlan g_plan(cod2mapaddr::GridAxes);
    inline bool g_verify = false;
    inline unsigned long long g_fast = 0, g_stock = 0, g_mismatches = 0;

    inline void Init(int index, const float* mins, const float* maxs, int depth)
    {
        bool fast = !index && !depth && g_nodes && *g_nodes;
        for (int k = 0; fast && k < 3; ++k)
            fast = std::isfinite(mins[k]) && std::isfinite(maxs[k]) && std::isfinite(mins[k] + maxs[k]);
        if (!fast) { ++g_stock; g_init(index, mins, maxs, depth); return; }
        ++g_fast;
        if (!g_verify) { gridinit_impl::InitializeGrid(*g_nodes, mins, maxs, g_plan); return; }
        std::vector<gridinit_impl::GridNode> ours(gridinit_impl::kGridNodes);
        gridinit_impl::InitializeGrid(ours.data(), mins, maxs, g_plan);
        g_init(index, mins, maxs, depth);  // stock result kept
        if (memcmp(ours.data(), *g_nodes, sizeof(gridinit_impl::GridNode) * gridinit_impl::kGridNodes)) ++g_mismatches;
    }

    inline bool Verify(uintptr_t base)
    {
        using namespace cod2mapaddr;
        return !memcmp(reinterpret_cast<void*>(base + GridInitRva), GridInitSig, sizeof GridInitSig) &&
               !memcmp(reinterpret_cast<void*>(base + GridAxesRva), GridAxes, sizeof GridAxes);
    }

    inline bool Install(uintptr_t base, bool verify)
    {
        g_verify = verify;
        g_nodes = reinterpret_cast<gridinit_impl::GridNode**>(base + cod2mapaddr::GridNodesRva);
        return MH_CreateHook(reinterpret_cast<void*>(base + cod2mapaddr::GridInitRva), reinterpret_cast<void*>(&Init),
                             reinterpret_cast<void**>(&g_init)) == MH_OK;
    }

    inline void Report(FILE* out)
    {
        fprintf(out, "[BetterBuild] Grid Init: %llu fast, %llu stock", g_fast, g_stock);
        if (g_verify) fprintf(out, ", %llu verify mismatches (stock kept)", g_mismatches);
        fprintf(out, ".\n");
    }
}
