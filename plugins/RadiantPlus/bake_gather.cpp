// Fast per-voxel gather (see bake_gather.h), single precision. Interface of 0x141131670 and its per-ray sample
// 0x1411334 Operation order and roundings follow
// the binary, logf/expf are MSVCR110's own.
#include "bake_gather.h"
#include <windows.h>
#include <xmmintrin.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace br::bakegather
{
namespace
{
const uintptr_t kAccInit    = 0x141119CC0;   // acc*(acc*, const float ref[3])
const uintptr_t kAccFinal   = 0x14111AB50;   // void(acc*, char debug)
const uintptr_t kMerge      = 0x14111AB80;   // float(acc* a, acc* b): blends b into a, returns the max relative difference
const uintptr_t kDirTables  = 0x1455822A0;   // std::vector<float[3]>[4 qualities][8 sets]
const uintptr_t kTableA     = 0x1455828A0;   // std::vector<float[6]>[4][8], parallel to the directions
const uintptr_t kTableB     = 0x145581FA0;
const uintptr_t kThresholds = 0x142724398;   // float[]: adaptive thresholds, index probe+0x88 - 1

const size_t off_NoMissLookup = 0x84, off_Adaptive = 0x88, off_Conv = 0x5C8, off_HitDist = 0x610, off_HitFlags = 0x628,
             off_Stride = 0x664, off_Flags = 0x670, off_Ref = 0x688, off_VoxelPos = 0x6A0, off_Cube = 0x6D8;
const size_t off_Data_Intensity = 0x180, off_Data_Cube = 0x11A8;   // lighting volume data: origin at +0

struct alignas(16) Acc { uint8_t b[768]; };
inline float& F(Acc& a, size_t off) { return *reinterpret_cast<float*>(a.b + off); }

using AccInitFn = void*(__fastcall*)(void*, const float*);
using AccFinalFn = void(__fastcall*)(void*, char);
using MergeFn = float(__fastcall*)(void*, void*);
using MathFn = float(__cdecl*)(float);
AccInitFn g_accInit = nullptr;
AccFinalFn g_accFinal = nullptr;
MergeFn g_merge = nullptr;
MathFn g_logf = nullptr, g_expf = nullptr;
const uint64_t* g_dirTables = nullptr;
const uint64_t* g_tableA = nullptr;
const uint64_t* g_tableB = nullptr;
const float* g_thresholds = nullptr;

br::bakeguide::Env g_env{};
std::atomic<int> g_mode{0};   // 0 off, 1 fast, 2 fast + verify every voxel against Radiant's own gather
bool g_ready = false;
std::atomic<uint64_t> g_fast{0}, g_passed{0}, g_verified{0}, g_mismatches{0};

void Log(const char* fmt, ...)
{
    if (!g_env.log) return;
    char buf[768];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    g_env.log(buf);
}

template <class T> T At(const void* base, size_t off) { return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(base) + off); }

// One ray's frame, fixed per probe job: the rotated direction and the tangent pair the hit-point offsets use.
struct Frame { float dW[3], T[3], Bt[3]; };

struct ProbeFrames
{
    const float* m9 = nullptr;
    float m[9]{};
    uint32_t q = ~0u, rays = 0;
    const float* dirs = nullptr;
    std::vector<Frame> frames;
};
thread_local ProbeFrames t_frames;

bool BuildFrames(ProbeFrames& pf, const float* m9, uint32_t q, uint32_t rays, const float* dirs)
{
    if (pf.m9 == m9 && pf.q == q && pf.rays == rays && pf.dirs == dirs && !std::memcmp(pf.m, m9, sizeof pf.m)) return true;
    try { pf.frames.resize(rays); } catch (...) { pf.rays = 0; return false; }
    const float* m = m9;
    for (uint32_t j = 0; j < rays; ++j)
    {
        const float x0 = dirs[3 * j], x1 = dirs[3 * j + 1], x2 = dirs[3 * j + 2];
        Frame& f = pf.frames[j];
        // 0x141129F60: rotation by m (row-major 3x3)
        f.dW[0] = ((x1 * m[3]) + (x0 * m[0])) + (x2 * m[6]);
        f.dW[1] = ((x0 * m[1]) + (x1 * m[4])) + (x2 * m[7]);
        f.dW[2] = ((x0 * m[2]) + (x1 * m[5])) + (x2 * m[8]);
        // tangent: (-y, x, 0) when |x| > |z|, else (0, -z, y), normalised (1/sqrt, 0 for a zero vector)
        float e6, e10, e11;
        if (std::fabs(f.dW[0]) > std::fabs(f.dW[2])) { e6 = -f.dW[1]; e10 = f.dW[0]; e11 = 0.0f; }
        else { e10 = -f.dW[2]; e11 = f.dW[1]; e6 = 0.0f; }
        const float n2 = ((e10 * e10) + (e6 * e6)) + (e11 * e11);
        float inv = 0.0f;
        if (n2 > 0.0f) inv = 1.0f / _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(n2)));
        f.T[0] = inv * e6; f.T[1] = e10 * inv; f.T[2] = e11 * inv;
        // bitangent: cross(T, dW) as 0x1400DD8C0 computes it
        f.Bt[0] = (f.dW[2] * f.T[1]) - (f.T[2] * f.dW[1]);
        f.Bt[1] = (f.T[2] * f.dW[0]) - (f.T[0] * f.dW[2]);
        f.Bt[2] = (f.T[0] * f.dW[1]) - (f.dW[0] * f.T[1]);
    }
    pf.m9 = m9; std::memcpy(pf.m, m9, sizeof pf.m); pf.q = q; pf.rays = rays; pf.dirs = dirs;
    return true;
}

// ---- hit lookup ------------------------------------------------------------------------------------------------------
// One bilinear tap: r0 = &texel[y][x], r1 = &texel[y + 1][x] (texels are float4), r1 null = a single texel.
struct Tap { const float* r0; const float* r1; float w00, w10, w01, w11; };
enum : uint8_t { kMiss = 0, kBlack = 1, kHit = 2 };
struct RayLook { uint8_t kind, levels; float f, gain; Tap t[2]; };   // levels 0: one texel, 1: bilinear, 2: trilinear

inline const float* Level(const uint8_t* cube, uint32_t face, uint32_t level)
{
    return *reinterpret_cast<const float* const*>(cube + 24 * size_t(face + 6 * level));
}

// Texel coordinates for one mip level (0x141190700's clamp: first against res - 1.001, then 0).
inline void MakeTap(const uint8_t* cube, uint32_t face, int level, float u, float v, Tap& t)
{
    const int res = 256 >> level;
    const float* tex = Level(cube, face, uint32_t(level));
    if (res == 1) { t.r0 = tex; t.r1 = nullptr; return; }
    const float half = float(res - 1) * 0.5f;
    float X = ((u + 1.0f) * half) + 0.5f, Y = ((v + 1.0f) * half) + 0.5f;
    const float lim = float(res) - 1.001f;
    { const float c = (X > lim) ? lim : X; if (0.0f > c) X = 0.0f; else if (X > lim) X = lim; }
    { const float c = (Y > lim) ? lim : Y; if (0.0f > c) Y = 0.0f; else if (Y > lim) Y = lim; }
    const int iy = int(Y), ix = int(X);   // truncation (cvttss2si)
    const float fy = Y - float(iy), fx = X - float(ix);
    t.w11 = fy * fx;
    t.w01 = (1.0f - fx) * fy;
    t.w00 = (1.0f - fy) * (1.0f - fx);
    t.w10 = (1.0f - fy) * fx;
    t.r0 = tex + 4 * size_t(uint32_t(iy * res + ix));
    t.r1 = tex + 4 * size_t(uint32_t((iy + 1) * res + ix));
    _mm_prefetch(reinterpret_cast<const char*>(t.r0), _MM_HINT_T0);
    _mm_prefetch(reinterpret_cast<const char*>(t.r0 + 4), _MM_HINT_T0);
    _mm_prefetch(reinterpret_cast<const char*>(t.r1), _MM_HINT_T0);
    _mm_prefetch(reinterpret_cast<const char*>(t.r1 + 4), _MM_HINT_T0);
}

inline void TapColor(const Tap& t, float c[3])
{
    if (!t.r1) { c[0] = t.r0[0]; c[1] = t.r0[1]; c[2] = t.r0[2]; return; }
    const float* t00 = t.r0; const float* t10 = t.r0 + 4; const float* t01 = t.r1; const float* t11 = t.r1 + 4;
    for (int k = 0; k < 3; ++k) c[k] = (((t00[k] * t.w00) + (t10[k] * t.w10)) + (t01[k] * t.w01)) + (t11[k] * t.w11);
}

// Face selection (0x14115D190): major axis, (first, second) in that face's frame.
inline uint32_t Face(const float l[3], float& first, float& second, float& major)
{
    const float ax = std::fabs(l[0]), ay = std::fabs(l[1]), az = std::fabs(l[2]);
    if (ax > ay && ax > az)
    {
        second = -l[1]; major = ax;
        if (l[0] > 0.0f) { first = -l[2]; return 0; }
        first = l[2]; return 1;
    }
    if (ay > az)
    {
        first = l[0]; major = ay;
        if (l[1] > 0.0f) { second = l[2]; return 2; }
        second = -l[2]; return 3;
    }
    second = -l[1]; major = az;
    if (l[2] > 0.0f) { first = l[0]; return 4; }
    first = -l[0]; return 5;
}

struct VoxelInputs
{
    const uint8_t* probe; uint32_t voxel, half, stride, q;
    const uint16_t* hitFlags; const uint16_t* hitDist;
    float vp[3], maxDist, scale;
    bool noMissLookup;
    const uint8_t* volumes; const uint8_t* sky;   // lighting volume entries (48 bytes), and the sky entry for index 15
};

// The lookup for ray j (0x1411334F0 up to the accumulate), texels prefetched.
inline void Look(const VoxelInputs& in, const Frame& fr, uint32_t j, RayLook& r)
{
    const size_t idx = size_t(uint32_t(in.stride * in.voxel + j));
    const uint32_t flags = in.hitFlags[idx];
    if ((flags & 0x3000) == 0x3000) { r.kind = kMiss; return; }
    if (flags & 0x400) { r.kind = in.noMissLookup ? kMiss : kBlack; return; }
    float d = in.maxDist;
    const uint32_t d16 = in.hitDist[idx];
    if (d16 != 0xFFFF) d = (d * 1.5259255e-05f) * float(int(d16));
    const float s1 = float(int(((flags >> 12) & 3) - 1)), s2 = float(int((flags >> 14) - 1));
    const float mip = float(int((flags >> 4) & 0x3F)) * 0.25f;
    const uint32_t vol = flags & 0xF;
    const uint8_t* entry = vol == 0xF ? in.sky : in.volumes + 48 * size_t(vol);
    const uint8_t* data = entry ? At<const uint8_t*>(entry, 0) : nullptr;
    if (!data) { r.kind = kMiss; return; }   // 0x14112B960 returns 0: the sample falls back to a miss
    float p[3];
    for (int k = 0; k < 3; ++k) p[k] = ((in.vp[k] + (fr.dW[k] * d)) + (fr.Bt[k] * s1)) + (fr.T[k] * s2);
    const float* o = reinterpret_cast<const float*>(data);
    const float* m = reinterpret_cast<const float*>(entry + 8);
    const float dx = p[0] - o[0], dy = p[1] - o[1], dz = p[2] - o[2];
    float l[3];
    l[0] = ((dy * m[3]) + (dx * m[0])) + (dz * m[6]);
    l[1] = ((dx * m[1]) + (dy * m[4])) + (dz * m[7]);
    l[2] = ((dx * m[2]) + (dy * m[5])) + (dz * m[8]);
    float first, second, major;
    const uint32_t face = Face(l, first, second, major);
    const float u = first / major, v = second / major;
    const uint8_t* cube = data + off_Data_Cube;
    r.kind = kHit;
    r.gain = At<float>(data, off_Data_Intensity);
    if (!(mip < 8.0f)) { r.levels = 0; r.t[0].r0 = Level(cube, face, 8); r.t[0].r1 = nullptr; return; }
    if (!(0.0f < mip)) { r.levels = 1; MakeTap(cube, face, 0, u, v, r.t[0]); return; }
    const int lv = int(mip);
    r.levels = 2;
    r.f = mip - float(lv);
    MakeTap(cube, face, lv, u, v, r.t[0]);
    MakeTap(cube, face, lv + 1, u, v, r.t[1]);
}

inline void LookColor(const RayLook& r, float scale, float c[3])
{
    if (r.levels < 2) TapColor(r.t[0], c);
    else
    {
        float a[3], b[3];
        TapColor(r.t[0], a); TapColor(r.t[1], b);
        const float g = 1.0f - r.f;
        for (int k = 0; k < 3; ++k) c[k] = (a[k] * g) + (b[k] * r.f);
    }
    for (int k = 0; k < 3; ++k) c[k] = (c[k] * r.gain) * scale;
}

// ---- accumulators (0x141119DD0 hit, 0x14111A180 miss), weight 1 --------------------------------------------------------
// Kept in SSE registers while a voxel's rays are summed (lane k = colour channel k), then stored into Radiant's accumulator.
inline uint32_t Bin(const float* d)
{
    const float ax = std::fabs(d[0]), ay = std::fabs(d[1]), az = std::fabs(d[2]);
    uint32_t axis;
    if (ax <= ay) axis = (ay <= az) ? 2 : 1;
    else axis = (ax > az) ? 0 : 2;
    return (d[axis] < 0.0f ? 1u : 0u) + 2 * axis;
}

struct LocalAcc
{
    __m128 binColor[6], nrm[6], brB[6], brA[6], total;   // acc +24+12*bin, +320+12r, +176+12r, +248+12r, +416
    __m128 missB[6][2], missA[6][2];                     // acc +428+24*bin, +572+24*bin (rows 0-3, 4-5)
    float binW[6], W, thr[3];                            // acc +4*bin, +172, bright thresholds +392
    int32_t count;                                       // acc +96
    void Init(const Acc& a)
    {
        const __m128 z = _mm_setzero_ps();
        for (int i = 0; i < 6; ++i)
        {
            binColor[i] = nrm[i] = brB[i] = brA[i] = z;
            missB[i][0] = missB[i][1] = missA[i][0] = missA[i][1] = z;
            binW[i] = 0.0f;
        }
        total = z; W = 0.0f; count = 0;
        std::memcpy(thr, a.b + 392, 12);
    }
    static void Put3(Acc& a, size_t off, __m128 v) { alignas(16) float t[4]; _mm_store_ps(t, v); std::memcpy(a.b + off, t, 12); }
    void Store(Acc& a) const
    {
        for (int i = 0; i < 6; ++i)
        {
            F(a, 4 * i) = binW[i];
            Put3(a, 24 + 12 * i, binColor[i]);
            Put3(a, 320 + 12 * i, nrm[i]);
            Put3(a, 176 + 12 * i, brB[i]);
            Put3(a, 248 + 12 * i, brA[i]);
            alignas(16) float t[8];
            _mm_store_ps(t, missB[i][0]); _mm_store_ps(t + 4, missB[i][1]); std::memcpy(a.b + 428 + 24 * i, t, 24);
            _mm_store_ps(t, missA[i][0]); _mm_store_ps(t + 4, missA[i][1]); std::memcpy(a.b + 572 + 24 * i, t, 24);
        }
        *reinterpret_cast<int32_t*>(a.b + 96) = count;
        F(a, 172) = W;
        Put3(a, 416, total);
    }
};

inline __m128 Row6Lo(const float* r) { return _mm_loadu_ps(r); }
inline __m128 Row6Hi(const float* r) { return _mm_castpd_ps(_mm_load_sd(reinterpret_cast<const double*>(r + 4))); }

inline void AccHit(LocalAcc& acc, const float* dir, const float c[3], const float* rowA, const float* rowB)
{
    const float w = 1.0f;
    const __m128 wv = _mm_set1_ps(w);
    const __m128 cv = _mm_setr_ps(c[0], c[1], c[2], 0.0f);
    const uint32_t bin = Bin(dir);
    acc.binW[bin] = w + acc.binW[bin];
    acc.binColor[bin] = _mm_add_ps(acc.binColor[bin], _mm_mul_ps(cv, wv));
    ++acc.count;
    acc.W = w + acc.W;
    acc.total = _mm_add_ps(acc.total, _mm_mul_ps(cv, wv));
    if (c[0] > acc.thr[0] || c[1] > acc.thr[1] || c[2] > acc.thr[2])
    {
        for (int r = 0; r < 6; ++r)
        {
            acc.brB[r] = _mm_add_ps(acc.brB[r], _mm_mul_ps(_mm_mul_ps(cv, _mm_set1_ps(rowB[r])), wv));
            acc.brA[r] = _mm_add_ps(acc.brA[r], _mm_mul_ps(_mm_mul_ps(cv, _mm_set1_ps(rowA[r])), wv));
        }
    }
    else
    {
        for (int r = 0; r < 6; ++r) acc.nrm[r] = _mm_add_ps(acc.nrm[r], _mm_mul_ps(_mm_mul_ps(cv, _mm_set1_ps(rowA[r])), wv));
    }
}

inline void AccMiss(LocalAcc& acc, const float* dir, const float* rowA, const float* rowB)
{
    const float w = 1.0f;
    const __m128 wv = _mm_set1_ps(w);
    const uint32_t bin = Bin(dir);
    acc.missB[bin][0] = _mm_add_ps(_mm_mul_ps(wv, Row6Lo(rowB)), acc.missB[bin][0]);
    acc.missB[bin][1] = _mm_add_ps(_mm_mul_ps(wv, Row6Hi(rowB)), acc.missB[bin][1]);
    acc.missA[bin][0] = _mm_add_ps(_mm_mul_ps(wv, Row6Lo(rowA)), acc.missA[bin][0]);
    acc.missA[bin][1] = _mm_add_ps(_mm_mul_ps(wv, Row6Hi(rowA)), acc.missA[bin][1]);
    acc.W = w + acc.W;
}

struct Exposure { uint32_t count[6]; float sum[6]; };

inline void AddExposure(Exposure& e, const float* dir, float lum)
{
    // 0x14113CC70: face k counts when dir . (+-axis) > 0
    if (dir[0] > 0.0f) { ++e.count[0]; e.sum[0] = e.sum[0] + lum; }
    if (-dir[0] > 0.0f) { ++e.count[1]; e.sum[1] = e.sum[1] + lum; }
    if (dir[1] > 0.0f) { ++e.count[2]; e.sum[2] = e.sum[2] + lum; }
    if (-dir[1] > 0.0f) { ++e.count[3]; e.sum[3] = e.sum[3] + lum; }
    if (dir[2] > 0.0f) { ++e.count[4]; e.sum[4] = e.sum[4] + lum; }
    if (-dir[2] > 0.0f) { ++e.count[5]; e.sum[5] = e.sum[5] + lum; }
}

// ---- the voxel ----------------------------------------------------------------------------------------------------------
const float* VecBegin(const uint64_t* table, uint32_t q, size_t rowBytes, uint32_t need)
{
    const uint64_t* vec = table + 3 * size_t(8 * q + 0);
    const uint64_t begin = vec[0], end = vec[1];
    if (!begin || end < begin || (end - begin) / rowBytes < need) return nullptr;
    return reinterpret_cast<const float*>(begin);
}

thread_local std::vector<RayLook> t_looks;
constexpr uint32_t kBlock = 64;   // ray pairs looked up (and prefetched) before they are accumulated

// false = this voxel is not one the fast path covers (the caller runs Radiant's gather)
bool FastGather(int64_t* ctx, uint32_t v)
{
    uint8_t* probe = reinterpret_cast<uint8_t*>(ctx[1]);
    if (!probe || !ctx[2] || !ctx[3] || !ctx[4] || !ctx[6] || !ctx[7] || !ctx[8] || !ctx[9] || !ctx[10] || !ctx[11] || !ctx[12] || !ctx[14] || !ctx[15]) return false;
    if (*reinterpret_cast<const uint8_t*>(ctx[2]) || *reinterpret_cast<const uint8_t*>(ctx[4])) return false;   // global probe, debug voxel capture
    VoxelInputs in{};
    in.probe = probe; in.voxel = v;
    in.half = *reinterpret_cast<const uint32_t*>(ctx[6]);
    in.q = *reinterpret_cast<const uint32_t*>(ctx[7]);
    in.stride = At<uint32_t>(probe, off_Stride);
    const uint32_t rays = 2 * in.half;
    if (!in.half || in.half > 32768 || rays > in.stride || in.q >= 4) return false;
    in.hitFlags = At<const uint16_t*>(probe, off_HitFlags);
    in.hitDist = At<const uint16_t*>(probe, off_HitDist);
    const float* voxelPos = At<const float*>(probe, off_VoxelPos);
    uint64_t* vflags = At<uint64_t*>(probe, off_Flags);
    float* refs = At<float*>(probe, off_Ref);
    float* conv = At<float*>(probe, off_Conv);
    float* cube = At<float*>(probe, off_Cube);
    uint8_t* done = *reinterpret_cast<uint8_t**>(ctx[3]);
    if (!in.hitFlags || !in.hitDist || !voxelPos || !vflags || !refs || !conv || !cube || !done) return false;
    const float* dirs = VecBegin(g_dirTables, in.q, 12, rays);
    const float* tA = VecBegin(g_tableA, in.q, 24, rays);
    const float* tB = VecBegin(g_tableB, in.q, 24, rays);
    if (!dirs || !tA || !tB) return false;
    const float* m9 = reinterpret_cast<const float*>(ctx[10]);
    ProbeFrames& pf = t_frames;
    if (!BuildFrames(pf, m9, in.q, rays, dirs)) return false;
    std::vector<RayLook>& looks = t_looks;
    if (looks.size() < 2 * kBlock) { try { looks.resize(2 * kBlock); } catch (...) { return false; } }

    in.maxDist = *reinterpret_cast<const float*>(ctx[9]);
    in.scale = *reinterpret_cast<const float*>(ctx[12]);
    in.noMissLookup = At<uint8_t>(probe, off_NoMissLookup) != 0;
    in.volumes = *reinterpret_cast<const uint8_t* const*>(ctx[11]);
    in.sky = reinterpret_cast<const uint8_t*>(ctx[13]);
    std::memcpy(in.vp, voxelPos + 3 * size_t(v), sizeof in.vp);

    Acc A, B;
    g_accInit(&A, refs + 3 * size_t(v));
    g_accInit(&B, refs + 3 * size_t(v));
    conv[v] = 0.0f;
    vflags[v] &= ~0x20000000000ull;
    if (vflags[v] & 1)
    {
        Exposure e{};
        LocalAcc la, lb;
        la.Init(A); lb.Init(B);
        const uint32_t half = in.half;
        for (uint32_t p0 = 0; p0 < half; p0 += kBlock)
        {
            const uint32_t n = half - p0 < kBlock ? half - p0 : kBlock;
            for (uint32_t k = 0; k < n; ++k)
            {
                const uint32_t ja = p0 + k, jb = p0 + k + half;
                Look(in, pf.frames[ja], ja, looks[2 * k]);
                Look(in, pf.frames[jb], jb, looks[2 * k + 1]);
            }
            for (uint32_t k = 0; k < 2 * n; ++k)
            {
                const uint32_t j = (k & 1) ? p0 + (k >> 1) + half : p0 + (k >> 1);
                LocalAcc& acc = (k & 1) ? lb : la;
                const RayLook& r = looks[k];
                const float* dir = dirs + 3 * size_t(j);
                const float* rowA = tA + 6 * size_t(j);
                const float* rowB = tB + 6 * size_t(j);
                if (r.kind == kMiss) { AccMiss(acc, dir, rowA, rowB); continue; }
                if (r.kind == kBlack) { const float black[3] = {0.0f, 0.0f, 0.0f}; AccHit(acc, dir, black, rowA, rowB); continue; }
                float c[3];
                LookColor(r, in.scale, c);
                AccHit(acc, dir, c, rowA, rowB);
                const float lum = g_logf(((c[1] * 0.7152f) + (c[0] * 0.2126f)) + (c[2] * 0.0722f));
                AddExposure(e, dir, lum);
            }
        }
        la.Store(A); lb.Store(B);
        g_accFinal(&A, 0);
        g_accFinal(&B, 0);
        const float diff = g_merge(&A, &B);
        conv[v] = diff;
        const uint8_t adaptive = *reinterpret_cast<const uint8_t*>(ctx[14]);
        if (adaptive)
        {
            const int level = At<int32_t>(probe, off_Adaptive);
            if (diff >= g_thresholds[level - 1]) vflags[v] |= 0x20000000000ull;
        }
        const uint64_t* expo = reinterpret_cast<const uint64_t*>(ctx[15]);
        for (int k = 0; k < 6; ++k)
            if (e.count[k])
                reinterpret_cast<float*>(expo[3 * k])[v] = g_expf(e.sum[k] / float(int64_t(e.count[k])));
        if (!adaptive) std::memcpy(refs + 3 * size_t(v), &F(A, 416), 12);
        done[v] = 1;
    }
    else done[v] = 0;
    std::memcpy(cube + 18 * size_t(v), &F(A, 100), 72);
    return true;
}

// Everything the gather writes for one voxel, to compare the two gathers in verify mode.
struct Outputs { float cube[18], ref[3], expo[6], conv; uint64_t flags; uint8_t done; };
void Save(const int64_t* ctx, uint32_t v, Outputs& o)
{
    const uint8_t* probe = reinterpret_cast<const uint8_t*>(ctx[1]);
    std::memcpy(o.cube, At<const float*>(probe, off_Cube) + 18 * size_t(v), sizeof o.cube);
    std::memcpy(o.ref, At<const float*>(probe, off_Ref) + 3 * size_t(v), sizeof o.ref);
    for (int k = 0; k < 6; ++k) o.expo[k] = (*reinterpret_cast<const float* const*>(ctx[15] + 24 * k))[v];
    o.conv = At<const float*>(probe, off_Conv)[v];
    o.flags = At<const uint64_t*>(probe, off_Flags)[v];
    o.done = (*reinterpret_cast<const uint8_t* const*>(ctx[3]))[v];
}
void Restore(int64_t* ctx, uint32_t v, const Outputs& o)
{
    uint8_t* probe = reinterpret_cast<uint8_t*>(ctx[1]);
    std::memcpy(At<float*>(probe, off_Cube) + 18 * size_t(v), o.cube, sizeof o.cube);
    std::memcpy(At<float*>(probe, off_Ref) + 3 * size_t(v), o.ref, sizeof o.ref);
    for (int k = 0; k < 6; ++k) (*reinterpret_cast<float**>(ctx[15] + 24 * k))[v] = o.expo[k];
    At<float*>(probe, off_Conv)[v] = o.conv;
    At<uint64_t*>(probe, off_Flags)[v] = o.flags;
    (*reinterpret_cast<uint8_t**>(ctx[3]))[v] = o.done;
}
bool Same(const Outputs& a, const Outputs& b)
{
    return !std::memcmp(a.cube, b.cube, sizeof a.cube) && !std::memcmp(a.ref, b.ref, sizeof a.ref) && !std::memcmp(a.expo, b.expo, sizeof a.expo) &&
           !std::memcmp(&a.conv, &b.conv, 4) && a.flags == b.flags && a.done == b.done;
}

const char* ModeName(int m) { return m == 2 ? "verify" : m ? "on" : "off"; }

bool Resolve()
{
    if (g_ready) return true;
    if (!g_env.address) return false;
    g_accInit = reinterpret_cast<AccInitFn>(g_env.address(kAccInit));
    g_accFinal = reinterpret_cast<AccFinalFn>(g_env.address(kAccFinal));
    g_merge = reinterpret_cast<MergeFn>(g_env.address(kMerge));
    g_dirTables = static_cast<const uint64_t*>(g_env.address(kDirTables));
    g_tableA = static_cast<const uint64_t*>(g_env.address(kTableA));
    g_tableB = static_cast<const uint64_t*>(g_env.address(kTableB));
    g_thresholds = static_cast<const float*>(g_env.address(kThresholds));
    // Radiant's own logf/expf (MSVCR110): the exposure must round like the stock gather's.
    if (HMODULE crt = GetModuleHandleW(L"MSVCR110.dll"))
    {
        g_logf = reinterpret_cast<MathFn>(GetProcAddress(crt, "logf"));
        g_expf = reinterpret_cast<MathFn>(GetProcAddress(crt, "expf"));
    }
    g_ready = g_accInit && g_accFinal && g_merge && g_dirTables && g_tableA && g_tableB && g_thresholds && g_logf && g_expf;
    if (!g_ready) Log("bake: fast gather unavailable (addresses or MSVCR110 logf/expf missing)");
    return g_ready;
}
}

bool Enabled() { return g_mode.load(std::memory_order_relaxed) != 0; }

int64_t Gather(GatherFn original, int64_t* ctx, int64_t voxelArg)
{
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (!mode || !ctx) return original(ctx, voxelArg);
    const uint32_t v = uint32_t(voxelArg);
    if (mode == 1)
    {
        if (FastGather(ctx, v)) { ++g_fast; return ctx[1]; }
        ++g_passed;
        return original(ctx, voxelArg);
    }
    Outputs before, fast, stock;
    Save(ctx, v, before);
    if (!FastGather(ctx, v)) { ++g_passed; return original(ctx, voxelArg); }
    Save(ctx, v, fast);
    Restore(ctx, v, before);
    const int64_t r = original(ctx, voxelArg);
    Save(ctx, v, stock);
    ++g_verified;
    if (!Same(fast, stock))
    {
        const uint64_t n = ++g_mismatches;
        if (n <= 20)
        {
            int field = -1;
            for (int k = 0; k < 18 && field < 0; ++k) if (std::memcmp(&fast.cube[k], &stock.cube[k], 4)) field = k;
            Log("bake: fast gather MISMATCH probe %p voxel %u: cube[%d] %.9g vs %.9g, ref %.9g/%.9g, conv %.9g/%.9g, expo0 %.9g/%.9g, flags %llx/%llx, done %u/%u",
                reinterpret_cast<void*>(ctx[1]), v, field, field >= 0 ? fast.cube[field] : 0.0, field >= 0 ? stock.cube[field] : 0.0,
                fast.ref[0], stock.ref[0], fast.conv, stock.conv, fast.expo[0], stock.expo[0],
                (unsigned long long)fast.flags, (unsigned long long)stock.flags, fast.done, stock.done);
        }
    }
    return r;
}

std::string Status()
{
    char buf[320];
    snprintf(buf, sizeof buf, "fast gather %s%s: %llu voxels fast, %llu passed to Radiant, %llu verified, %llu mismatches",
             ModeName(g_mode.load()), g_ready ? "" : " (unavailable)", (unsigned long long)g_fast.load(), (unsigned long long)g_passed.load(),
             (unsigned long long)g_verified.load(), (unsigned long long)g_mismatches.load());
    return buf;
}

void ClearStats() { g_fast = 0; g_passed = 0; g_verified = 0; g_mismatches = 0; }

void Setup(const br::bakeguide::Env& env)
{
    g_env = env;
    const std::string v = env.get ? env.get("gather_fast", "1") : std::string("1");
    const int mode = v == "0" ? 0 : v == "verify" ? 2 : 1;
    g_mode = mode && Resolve() ? mode : 0;
    Log("bake: %s", Status().c_str());
}

bool Command(const std::string& c, std::string& reply)
{
    if (c.rfind("bake gatherfast", 0) != 0) return false;
    const std::string arg = c.size() > 16 ? c.substr(16) : "";
    if (arg == "0" || arg == "1" || arg == "verify")
    {
        if (arg != "0" && !Resolve()) { reply = "fast gather unavailable"; return true; }
        g_mode = arg == "0" ? 0 : arg == "verify" ? 2 : 1;
        if (g_env.set) g_env.set("gather_fast", arg.c_str());
    }
    reply = Status();
    return true;
}
}
