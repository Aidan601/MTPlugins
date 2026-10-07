#pragma once
// Cone correction for Radiant's probe voxel gather (pure math, used by bake_guiding.cpp): fewer rays read blurrier mips and come out brighter,
// so a sparse lattice is gathered again at high's cone and the box-smoothed ratio rescales every voxel.
#include <cstdint>

namespace br::cone
{
struct Result
{
    std::uint32_t samples = 0;  // lattice voxels that had a sharp gather
    std::uint32_t scaled = 0;   // voxels whose faces were rescaled (a sample within the radius)
    double meanRatio = 1;       // mean applied face ratio over the scaled voxels
};

// Luminance of one RGB triple (Rec. 709).
inline float Luma(const float* rgb) { return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2]; }

// In-place box sum of radius r over a dx*dy*dz grid (x fastest), clipped at the edges.
void BoxSum(float* a, int dx, int dy, int dz, int r);

// coeffs: 18 floats per voxel (6 faces x RGB), rescaled in place. sharp: 6 luminances per voxel, NaN where not sampled.
// Per face ratio = sum(sharp) / sum(stock) over samples within radius r, clamped to [minRatio, maxRatio].
// edgeClass: boundary and inner voxels use only samples of their own class (all samples when none in range).
Result Apply(float* coeffs, const float* sharp, int dx, int dy, int dz, int r, float minRatio = 0.2f, float maxRatio = 5.0f,
             bool edgeClass = true);

// Whether voxel index v (x fastest) lies on the grid's boundary layer.
inline bool OnEdge(std::uint32_t v, int dx, int dy, int dz)
{
    const std::uint32_t x = v % std::uint32_t(dx), y = (v / std::uint32_t(dx)) % std::uint32_t(dy), z = v / (std::uint32_t(dx) * std::uint32_t(dy));
    return x == 0 || y == 0 || z == 0 || x + 1 == std::uint32_t(dx) || y + 1 == std::uint32_t(dy) || z + 1 == std::uint32_t(dz);
}

// Whether voxel index v (x fastest) lies on the sampling lattice of the given stride (last layer of each axis included).
inline bool OnLattice(std::uint32_t v, int dx, int dy, int dz, int stride)
{
    const std::uint32_t x = v % std::uint32_t(dx), y = (v / std::uint32_t(dx)) % std::uint32_t(dy), z = v / (std::uint32_t(dx) * std::uint32_t(dy));
    const std::uint32_t s = std::uint32_t(stride);
    return (x % s == 0 || x + 1 == std::uint32_t(dx)) && (y % s == 0 || y + 1 == std::uint32_t(dy)) && (z % s == 0 || z + 1 == std::uint32_t(dz));
}
}
