#pragma once
// C7 kernel: level-order initializer for the grid-tree layout (from BuildBoost 0.3), same node order and float math as stock.
#include <cstdint>
#include <xmmintrin.h>

namespace gridinit_impl
{
    struct GridNode { float split[2]; uint64_t head; };
    static_assert(sizeof(GridNode) == 16, "compiler ABI");
    constexpr unsigned kGridDepth = 20;
    constexpr unsigned kGridNodes = (1u << (kGridDepth + 1)) - 1;

    struct GridPlan
    {
        unsigned short low[21][1024] = {};
        unsigned short high[21][1024] = {};
        unsigned repeats[21] = {};
        unsigned axis[21] = {};

        static unsigned Gather(unsigned value, unsigned mask)
        {
            unsigned packed = 0, outputBit = 1;
            for (unsigned inputBit = 1; mask; inputBit <<= 1)
            {
                if (!(mask & inputBit)) continue;
                if (value & inputBit) packed |= outputBit;
                mask &= ~inputBit;
                outputBit <<= 1;
            }
            return packed;
        }

        explicit GridPlan(const int* axes)
        {
            for (unsigned depth = 0; depth <= kGridDepth; ++depth)
            {
                axis[depth] = axes[depth];
                unsigned mask = 0, lowBits = 0;
                for (unsigned previous = 0; previous < depth; ++previous)
                    if (axes[previous] == axes[depth])
                    {
                        mask |= 1u << (depth - previous - 1);
                        ++repeats[depth];
                        if (depth - previous - 1 < 10) ++lowBits;
                    }
                for (unsigned i = 0; i < 1024; ++i)
                {
                    low[depth][i] = static_cast<unsigned short>(Gather(i, mask & 1023));
                    high[depth][i] = static_cast<unsigned short>(Gather(i, mask >> 10) << lowBits);
                }
            }
        }
    };

    inline void InitializeGrid(GridNode* nodes, const float* mins, const float* maxs, const GridPlan& plan)
    {
        // at most nine same-axis ancestor splits in this axis schedule
        struct Interval { float lo, hi; } intervals[512];
        float splits[512];
        for (unsigned depth = 0; depth <= kGridDepth; ++depth)
        {
            intervals[0] = {mins[plan.axis[depth]], maxs[plan.axis[depth]]};
            unsigned count = 1;
            for (unsigned round = 0; round < plan.repeats[depth]; ++round)
            {
                for (unsigned i = count; i-- > 0;)
                {
                    const Interval previous = intervals[i];
                    const float midpoint = (previous.hi + previous.lo) * 0.5f;
                    intervals[2 * i] = {midpoint, previous.hi};
                    intervals[2 * i + 1] = {previous.lo, midpoint};
                }
                count *= 2;
            }
            for (unsigned i = 0; i < count; ++i)
                splits[i] = (intervals[i].hi + intervals[i].lo) * 0.5f;

            const unsigned width = 1u << depth;
            GridNode* row = nodes + width - 1;
            for (unsigned block = 0; block < width; block += 1024)
            {
                const unsigned high = plan.high[depth][block >> 10];
                const unsigned length = width < 1024 ? width : 1024;
                for (unsigned i = 0; i < length; ++i)
                {
                    const float split = splits[high | plan.low[depth][i]];
                    _mm_storeu_ps(reinterpret_cast<float*>(row + block + i), _mm_set_ps(0, 0, split, split));
                }
            }
        }
    }
}
