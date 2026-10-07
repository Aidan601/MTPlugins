// Cone correction math (see cone_correct.h).
#include "cone_correct.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace br::cone
{
namespace
{
// Running-window sum along one axis: n elements, spaced step apart, starting at base.
void SumLine(float* base, int n, std::size_t step, int r, std::vector<double>& line)
{
    line.resize(std::size_t(n));
    for (int i = 0; i < n; ++i) line[std::size_t(i)] = base[std::size_t(i) * step];
    double window = 0;
    for (int i = 0; i <= std::min(r, n - 1); ++i) window += line[std::size_t(i)];
    for (int i = 0; i < n; ++i)
    {
        base[std::size_t(i) * step] = float(window);
        const int add = i + r + 1, drop = i - r;
        if (add < n) window += line[std::size_t(add)];
        if (drop >= 0) window -= line[std::size_t(drop)];
    }
}
}

void BoxSum(float* a, int dx, int dy, int dz, int r)
{
    if (!a || dx <= 0 || dy <= 0 || dz <= 0 || r <= 0) return;
    std::vector<double> line;
    const std::size_t sx = 1, sy = std::size_t(dx), sz = std::size_t(dx) * std::size_t(dy);
    for (int z = 0; z < dz; ++z) for (int y = 0; y < dy; ++y) SumLine(a + z * sz + y * sy, dx, sx, r, line);
    for (int z = 0; z < dz; ++z) for (int x = 0; x < dx; ++x) SumLine(a + z * sz + x * sx, dy, sy, r, line);
    for (int y = 0; y < dy; ++y) for (int x = 0; x < dx; ++x) SumLine(a + y * sy + x * sx, dz, sz, r, line);
}

Result Apply(float* coeffs, const float* sharp, int dx, int dy, int dz, int r, float minRatio, float maxRatio, bool edgeClass)
{
    Result out;
    if (!coeffs || !sharp || dx <= 0 || dy <= 0 || dz <= 0) return out;
    const std::size_t n = std::size_t(dx) * std::size_t(dy) * std::size_t(dz);
    // class 0 = boundary layer, 1 = inside (all class 1 without edgeClass): a voxel uses its own class's sums, else the other's
    std::vector<uint8_t> cls(n, 1);
    if (edgeClass)
        for (std::size_t v = 0; v < n; ++v) cls[v] = OnEdge(uint32_t(v), dx, dy, dz) ? 0 : 1;
    std::vector<float> weight[2], num[2], den[2];
    for (int c = 0; c < 2; ++c) { weight[c].assign(n, 0.0f); num[c].resize(n); den[c].resize(n); }
    for (std::size_t v = 0; v < n; ++v)
    {
        const bool sampled = !std::isnan(sharp[6 * v]);
        weight[cls[v]][v] = sampled ? 1.0f : 0.0f;
        out.samples += sampled;
    }
    if (!out.samples) return out;
    for (int c = 0; c < 2; ++c) BoxSum(weight[c].data(), dx, dy, dz, r);
    std::vector<float> ratio(6 * n, 1.0f);
    for (int f = 0; f < 6; ++f)
    {
        for (int c = 0; c < 2; ++c) { std::fill(num[c].begin(), num[c].end(), 0.0f); std::fill(den[c].begin(), den[c].end(), 0.0f); }
        for (std::size_t v = 0; v < n; ++v)
        {
            if (std::isnan(sharp[6 * v])) continue;
            num[cls[v]][v] = std::max(0.0f, sharp[6 * v + f]);
            den[cls[v]][v] = std::max(0.0f, Luma(coeffs + 18 * v + 3 * f));
        }
        for (int c = 0; c < 2; ++c) { BoxSum(num[c].data(), dx, dy, dz, r); BoxSum(den[c].data(), dx, dy, dz, r); }
        for (std::size_t v = 0; v < n; ++v)
        {
            const int c = cls[v];
            float nu, de;
            if (weight[c][v] > 0.5f) { nu = num[c][v]; de = den[c][v]; }
            else if (weight[1 - c][v] > 0.5f) { nu = num[1 - c][v]; de = den[1 - c][v]; }
            else continue;
            if (de > 1e-12f && std::isfinite(nu)) ratio[6 * v + f] = std::clamp(nu / de, minRatio, maxRatio);
        }
    }
    double sum = 0;
    for (std::size_t v = 0; v < n; ++v)
    {
        if (!(weight[0][v] > 0.5f || weight[1][v] > 0.5f)) continue;
        ++out.scaled;
        for (int f = 0; f < 6; ++f)
        {
            const float k = ratio[6 * v + f];
            sum += k;
            for (int c = 0; c < 3; ++c) coeffs[18 * v + 3 * f + c] *= k;
        }
    }
    out.meanRatio = out.scaled ? sum / (6.0 * out.scaled) : 1.0;
    return out;
}
}
