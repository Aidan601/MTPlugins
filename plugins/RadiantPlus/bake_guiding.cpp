// Light bake improvements for the probe voxel gather (see bake_guiding.h): adaptive override, cone correction
// (cone_correct.h), experimental ray guiding (Silvennoinen and Sloan, "Ray Guiding for Production Lightmap Baking", 2019).

// MAP
//   addresses, offsets, function types ........ kProbeGather.., off_Probe_*, *Fn
//   environment ............................... g_env, Log, Addr, Cfg*
//   settings and statistics ................... Settings, g_set, Stats
//   cone correction state ..................... ConeState, FindCone, ConeGather, FinishCone
//   per-probe state ........................... TraceInfo, ProbeGuide, registries (FindTrace, FindGuide)
//   per-thread state .......................... Tls, PrepareVoxel
//   hooks ..................................... hk_VoxelTrace, hk_ProbeGather, hk_GatherVoxel, hk_DirLookup,
//                                               hk_SampleDir, hk_AccHit, hk_AccMiss, hk_Merge, hk_Adaptive
//   learning and guide build .................. FinishLearningVoxel
//   dump and commands ......................... DumpProbes, Command

// Ray guiding flow: each voxel learns a guide from a stock dry run, re-traces a guided batch on every bounce, combines
// stock and guided batches per face, then restores stock hits and exposure. Mip shifts are runtime-only diagnostics.
#include "bake_guiding.h"
#include "cone_correct.h"
#include "bake_gather.h"
#include "bake_lowrays.h"
#include "ray_guiding.h"
#include <windows.h>
#include <cstdarg>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace br::bakeguide
{
namespace
{
using guiding::Guide;
using guiding::Point;
using guiding::Sample;
using guiding::Training;
using guiding::Vec3;

// ---- addresses (Radiant_modtools.exe, resolved through host->RadiantAddress) --------------------------------------------
const uintptr_t kProbeGather   = 0x141125CB0; // per-probe bounce job: void(probe, dims*, volumes*, char, char, char, char init, scene*, char adaptive, qword, int*, int)
const uintptr_t kGatherVoxel   = 0x141131670; // per-voxel gather from the hit cache: int64(ctx*, uint voxel), ctx[1] = probe
const uintptr_t kDirLookup     = 0x141129EC0; // const float* (int quality, uint set, uint index) -> gUniformDirs entry (probe-local)
const uintptr_t kSampleDir     = 0x141129F60; // float* (float* out, uint quality, uint set, uint index, float m[9]): rotated direction
const uintptr_t kAccHit        = 0x141119DD0; // (acc*, dir*, rgb*, basisA[6]*, basisB[6]*, float weight)
const uintptr_t kAccMiss       = 0x14111A180; // (acc*, dir*, basisA[6]*, basisB[6]*, float weight)
const uintptr_t kMerge         = 0x14111AB80; // float(acc1*, acc2*): blend acc2's cube into acc1 by total weights, returns max relative diff
const uintptr_t kVoxelTrace    = 0x141122D70; // per-voxel cache trace: (probe, uint voxel, scene*, float m[9])
const uintptr_t kTrace32       = 0x1411232B0; // trace 32 rays from index start: (probe, voxel, start, set, scene*, uint*, uint*, uint*, float acc[12]*, float m[9])
const uintptr_t kAdaptive      = 0x141131D20; // adaptive re-gather of unconverged voxels (observed only)
const uintptr_t kHitFixup      = 0x14112F630; // per-voxel pass after the trace: resolves each hit's lighting volume (flag bits 0-3) and
                                              // detail level (bits 4-9), clears bit 10, updates the voxel's validity bits, (ctx[15]*, uint voxel)
const uintptr_t kVoxelFinal    = 0x141132CC0; // last per-voxel stage of the gather job, after gather, adaptive and free-space fill:
                                              // (ctx*, uint voxel), ctx[0] = probe, clamps +1752, applies exposure, writes +1360
                                              // (the copy that becomes the BC6H volume texture), so edits must land before it
const uintptr_t kSmoothTask    = 0x140302410; // state 5 "Probe Smooth Postprocess" std::async callable: char(capture*), 0 = skipped
                                              // (option 59), blends +1360 where probe volumes overlap, then repacks BC6H
const uintptr_t kCaptureBegin  = 0x14116C230; // char(int w, int h): starts the probe debugger's voxel capture (global buffers, hMutex)
const uintptr_t kSetRenderOpt  = 0x1409BC2B0; // void(int index, char on): Radiant's render options bit array 0x145215AC0 (locked)
const uintptr_t kGetRenderOpt  = 0x1409BBAC0; // bool(int index)
const int kOptDisablePostSmooth = 59, kOptDisableFreeSpace = 60;
const uintptr_t kBasisA        = 0x141191030; // void(float x, float y, float z, float out[6])
const uintptr_t kBasisB        = 0x141192110; // void(float x, float y, float z, float out[6]): max(0, +-axis) / pi
const uintptr_t kUniformDirs   = 0x1455822A0; // std::vector<float[3]>[4 qualities][8 sets]
const uintptr_t kBasisTableA   = 0x1455828A0; // std::vector<float[6]> per [quality][set], parallel to gUniformDirs
const uintptr_t kBasisTableB   = 0x145581FA0;

const size_t off_Probe_Matrix    = 12;    // float[9], volume axes
const size_t off_Probe_Dims      = 96;    // int[3], multiples of 4
const size_t off_Probe_Adaptive  = 136;   // int adaptive_quality_boost 0..3 (Off/Low/Medium/High), the gather reads it on entry
const size_t off_Probe_Id        = 388;
const size_t off_Probe_HitDist   = 1552;  // uint16* per ray (voxel * stride + index): quantized hit distance
const size_t off_Probe_HitFlags  = 1576;  // uint16* per ray: miss / backface / offset flags
const size_t off_Probe_Stride    = 1636;  // uint rays per voxel in the hit arrays
const size_t off_Probe_Flags     = 1648;  // uint64* per voxel: bit 0 valid, bit 40 traced (not inside geometry)
const size_t off_Probe_Final     = 1360;  // std::vector<float>, 18 per voxel: the final values (exposure applied) state 5 smooths
const size_t off_Probe_Coeffs    = 1752;  // float* per voxel: 6 faces x RGB
const size_t off_Probe_Rays      = 10176; // uint rays per voxel (N)
const size_t off_Probe_Quality   = 10180; // uint gUniformDirs quality index
const size_t off_Probe_TfarOnly  = 10224; // byte: hit writer stores only rays past tfar (guiding refuses these probes)
const size_t off_Acc_Weight      = 172;   // float: total sample weight
const size_t off_Acc_Count       = 96;    // uint: samples
const size_t off_Acc_Cube        = 100;   // float[18]: finalized cube
const size_t off_Cam_LightState  = 3960;
const size_t off_Cam_Bounce      = 6660;
const uint64_t kVoxelValid  = 1ull;
const uint64_t kVoxelTraced = 0x10000000000ull;

using ProbeGatherFn = void(__fastcall*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
using GatherVoxelFn = int64_t(__fastcall*)(int64_t*, int64_t);
using DirLookupFn   = const float*(__fastcall*)(int64_t, int64_t, int64_t);
using SampleDirFn   = float*(__fastcall*)(float*, int64_t, int64_t, int64_t, const float*);
using AccHitFn      = int64_t(__fastcall*)(int64_t, const float*, const float*, const float*, const float*, float);
using AccMissFn     = int64_t(__fastcall*)(int64_t, const float*, const float*, const float*, float);
using MergeFn       = float(__fastcall*)(float*, int64_t); // returns the halves' max relative difference (xmm0)
using VoxelTraceFn  = int64_t(__fastcall*)(int64_t, int64_t, int64_t, const float*);
using Trace32Fn     = int64_t(__fastcall*)(int64_t, int64_t, int64_t, int64_t, int64_t, uint32_t*, uint32_t*, uint32_t*, float*, const float*);
using AdaptiveFn    = int64_t(__fastcall*)(int64_t*, int64_t);
using HitFixupFn    = int64_t(__fastcall*)(int64_t*, int64_t);
using VoxelFinalFn  = int64_t(__fastcall*)(int64_t*, int64_t);
using BasisFn       = void(__fastcall*)(float, float, float, float*);
using SmoothTaskFn  = char(__fastcall*)(void*);
using SetRenderOptFn = void(__fastcall*)(int, char);
using GetRenderOptFn = bool(__fastcall*)(int);

ProbeGatherFn o_ProbeGather; GatherVoxelFn o_GatherVoxel; DirLookupFn o_DirLookup; SampleDirFn o_SampleDir;
AccHitFn o_AccHit; AccMissFn o_AccMiss; MergeFn o_Merge; VoxelTraceFn o_VoxelTrace; AdaptiveFn o_Adaptive; HitFixupFn o_HitFixup; VoxelFinalFn o_VoxelFinal;
SmoothTaskFn o_SmoothTask;
Trace32Fn g_trace32; BasisFn g_basisA, g_basisB;
const uint64_t* g_uniformDirs; const uint64_t* g_basisTableA; const uint64_t* g_basisTableB;
std::uint8_t* (*g_camWnd)();
bool (*g_running)();
bool g_hooked = false;       // every hook (ray guiding)
bool g_coreHooked = false;   // the gather-level hooks (adaptive override, cone correction, dumps)
// Radiant's voxel gather, or its exact fast version (bake_gather.cpp) unless ray guiding's per-sample hooks are in.
int64_t StockGather(int64_t* ctx, int64_t voxel) { return g_hooked ? o_GatherVoxel(ctx, voxel) : bakegather::Gather(o_GatherVoxel, ctx, voxel); }

// ---- environment ------------------------------------------------------------------------------------------------------
Env g_env{};
void Log(const char* fmt, ...)
{
    if (!g_env.log) return;
    char line[1024];
    va_list args; va_start(args, fmt); vsnprintf(line, sizeof line, fmt, args); va_end(args);
    g_env.log(line);
}
void* Addr(uintptr_t ida) { return g_env.address ? g_env.address(ida) : nullptr; }
std::string Cfg(const char* key, const char* fallback) { return g_env.get ? g_env.get(key, fallback) : std::string(fallback); }
bool CfgBool(const char* key, bool fallback) { const std::string v = Cfg(key, fallback ? "1" : "0"); return v == "1" || v == "true" || v == "on" || v == "yes"; }
void CfgSet(const char* key, const std::string& value) { if (g_env.set) g_env.set(key, value.c_str()); }

// ---- settings and statistics ----------------------------------------------------------------------------------------
enum class Combine { Equal, Variance };
enum class Target { Luminance, Faces };
struct Settings
{
    std::atomic<bool> enabled{false};
    std::atomic<bool> observe{true};
    Combine combine = Combine::Variance;
    Target target = Target::Faces;
    double smoothing = 0.25;
    double uniform = 0.05;
    // 0: every voxel warps the same point set (like Radiant, whose voxels all shoot gUniformDirs[q][0]), so neighbouring voxels
    // make correlated, smooth errors, 1: a per-voxel Cranley-Patterson rotation (independent speckle noise).
    bool decorrelate = false;
    // Half: guide the second half and combine three batches. Full: guide both halves and combine four batches.
    bool fullSplit = false;
    int guidedMipDelta = 0, stockMipDelta = 0;    // runtime-only cone proxy, in integer mip levels
    // Adaptive override: 0 keeps each probe's own adaptive_quality_boost, 1..3 raises it to at least Low/Medium/High.
    std::atomic<int> adaptive{0};
    // Cone correction: sharp-cone gathers on every stride-th voxel per axis, ratio box-smoothed over radius voxels.
    std::atomic<bool> cone{false};
    int coneStride = 2, coneRadius = 6;
    bool coneEdge = true;   // the grid's boundary layer takes its ratio from boundary samples only (cone_correct.h)
};
Settings g_set;

struct Stats
{
    std::atomic<uint64_t> learnedSamples{0}, learnedVoxels{0}, guides{0}, guidedVoxels{0}, retracedRays{0},
                          gdVoxels{0}, adaptiveCalls{0}, prepareFailures{0}, refusedProbes{0};
    std::atomic<uint64_t> adaptiveRaised{0}, coneGathers{0}, coneSamples{0}, coneScaled{0}, coneSkipped{0};
    std::atomic<uint64_t> coneRatioMilli{0}, coneRatioCount{0};  // sum of per-probe mean ratios (x1000) on the final bounce
    std::atomic<uint64_t> finalMs{0}, gatherMs{0};  // gather job time summed over probes: final bounce, every bounce
    std::atomic<uint64_t> mergeCalls{0}, mergeGuided{0}, mergeFewSamples{0}, mergeNoWeight{0};
    void Reset()
    {
        learnedSamples = learnedVoxels = guides = guidedVoxels = retracedRays = gdVoxels = adaptiveCalls = prepareFailures = refusedProbes = 0;
        finalMs = gatherMs = 0; mergeCalls = mergeGuided = mergeFewSamples = mergeNoWeight = 0;
        adaptiveRaised = coneGathers = coneSamples = coneScaled = coneSkipped = coneRatioMilli = coneRatioCount = 0;
    }
};
Stats g_stats;
std::mutex g_dumpMutex; std::string g_dumpDir, g_dumpTag = "run"; std::atomic<bool> g_dumpAll{false};

double Now() { LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f); return double(q.QuadPart) / double(f.QuadPart); }
double Luma(const float* rgb) { return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]; }
unsigned Face(const float* d)
{
    const float ax = std::fabs(d[0]), ay = std::fabs(d[1]), az = std::fabs(d[2]);
    unsigned axis = ax > ay ? (ax > az ? 0 : 2) : (ay > az ? 1 : 2);
    return 2 * axis + (d[axis] < 0 ? 1 : 0);
}
std::uint64_t Mix(std::uint64_t x)
{
    x += 0x9e3779b97f4a7c15ULL; x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL; x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL; return x ^ (x >> 31);
}

// A std::vector's begin/end in Radiant's tables (3 qwords per entry, index 8 * quality + set).
bool TableRange(const uint64_t* table, unsigned quality, unsigned set, uint64_t& begin, uint64_t& end)
{
    if (!table || quality > 3 || set > 7) return false;
    begin = table[3 * (8 * quality + set)]; end = table[3 * (8 * quality + set) + 1];
    return begin && end > begin;
}

// ---- per-probe state ------------------------------------------------------------------------------------------------
// The values the hit fix-up pass reads through its context (slot: meaning), copied from the trace pass.
struct FixupValues
{
    int32_t dims[3];        // 0: grid dimensions
    float offset;           // 1: 0.5 or 0
    float invDims[3];       // 2
    int32_t slot3[3];       // 3: probe+72
    float halfExtent[3];    // 4
    int32_t matrix[9];      // 5: probe+12 (row order, the direction transform)
    int32_t origin[3];      // 8: probe+0
    int32_t matrixCols[9];  // 9
    int32_t rays;           // 10
    float maxDistance;      // 11: hit distance quantization range
    uint8_t gridMode;       // 12
    int32_t quality;        // 13
    float threshold;        // 14
};
struct TraceInfo { uint8_t* probe = nullptr; int64_t scene = 0; float matrix[9]{}; bool haveFixup = false; FixupValues fixup{}; };
SRWLOCK g_traceLock = SRWLOCK_INIT;
TraceInfo g_traces[512];

void RecordTrace(uint8_t* probe, int64_t scene, const float* m)
{
    bool known = false;
    AcquireSRWLockShared(&g_traceLock);
    for (auto& t : g_traces) if (t.probe == probe && t.scene == scene) { known = true; break; }
    ReleaseSRWLockShared(&g_traceLock);
    if (known) return;
    AcquireSRWLockExclusive(&g_traceLock);
    TraceInfo* slot = nullptr;
    for (auto& t : g_traces) if (t.probe == probe) { slot = &t; break; }
    if (!slot) for (auto& t : g_traces) if (!t.probe) { slot = &t; break; }
    if (!slot) { slot = &g_traces[Mix(uint64_t(probe)) % 512]; }
    if (slot->probe != probe) { *slot = TraceInfo{}; slot->probe = probe; }
    slot->scene = scene; std::memcpy(slot->matrix, m, sizeof slot->matrix);
    ReleaseSRWLockExclusive(&g_traceLock);
}
void RecordFixup(uint8_t* probe, const int64_t* ctx)
{
    FixupValues v{};
    std::memcpy(v.dims, reinterpret_cast<void*>(ctx[0]), 12);
    v.offset = *reinterpret_cast<float*>(ctx[1]);
    std::memcpy(v.invDims, reinterpret_cast<void*>(ctx[2]), 12);
    std::memcpy(v.slot3, reinterpret_cast<void*>(ctx[3]), 12);
    std::memcpy(v.halfExtent, reinterpret_cast<void*>(ctx[4]), 12);
    std::memcpy(v.matrix, reinterpret_cast<void*>(ctx[5]), 36);
    std::memcpy(v.origin, reinterpret_cast<void*>(ctx[8]), 12);
    std::memcpy(v.matrixCols, reinterpret_cast<void*>(ctx[9]), 36);
    v.rays = *reinterpret_cast<int32_t*>(ctx[10]);
    v.maxDistance = *reinterpret_cast<float*>(ctx[11]);
    v.gridMode = *reinterpret_cast<uint8_t*>(ctx[12]);
    v.quality = *reinterpret_cast<int32_t*>(ctx[13]);
    v.threshold = *reinterpret_cast<float*>(ctx[14]);
    AcquireSRWLockExclusive(&g_traceLock);
    TraceInfo* slot = nullptr;
    for (auto& t : g_traces) if (t.probe == probe) { slot = &t; break; }
    if (!slot) for (auto& t : g_traces) if (!t.probe) { slot = &t; break; }
    if (!slot) slot = &g_traces[Mix(uint64_t(probe)) % 512];
    if (slot->probe != probe) { *slot = TraceInfo{}; slot->probe = probe; }
    slot->fixup = v; slot->haveFixup = true;
    ReleaseSRWLockExclusive(&g_traceLock);
}
bool FindTrace(uint8_t* probe, TraceInfo& out)
{
    AcquireSRWLockShared(&g_traceLock);
    bool found = false;
    for (auto& t : g_traces) if (t.probe == probe) { out = t; found = true; break; }
    ReleaseSRWLockShared(&g_traceLock);
    return found;
}

struct ProbeGuide
{
    uint8_t* probe = nullptr;
    int id = 0;
    int dims[3]{};
    uint32_t voxels = 0, rays = 0, half = 0, quality = 0;
    uint64_t seed = 0;
    TraceInfo trace;
    int64_t volumes = 0;                            // the bounce job's volume list (std::vector of 48-byte entries)
    int64_t fixupCtx[15]{};                         // pointers into trace.fixup plus the volume list
    std::vector<Point> stockPoints;                 // gUniformDirs[quality][0][i] on the unit square
    std::vector<unsigned char> stockFaces;
    std::vector<float> stockLobes;                  // 6 per ray: max(0, +-axis) / pi, the cube faces' cosine lobes
    std::vector<unsigned char> gdVoxel;             // voxel merged by inverse variance
    std::mutex sumMutex; double sumEqual = 0, sumVariance = 0;
    std::atomic<int64_t> mipRequested{0}, mipApplied{0}; // quarter-mip sums, all generated / resolved hits
    std::atomic<uint64_t> mipRays{0}, mipResolved{0}, mipClipped{0}, mipSharper{0}, mipBlurrier{0};
    std::atomic<uint64_t> guided{0};
    std::atomic<int> validation{0};                 // self-checks done, -1 = failed (guiding stops for this probe)
    // The luminance correction must land in the job's final stage (after it, +1752 is no longer read): once per job.
    std::atomic<bool> rhoApplied{false};
    SRWLOCK rhoLock = SRWLOCK_INIT;
    double lastRho = 1; uint64_t lastGd = 0;
};
SRWLOCK g_guideLock = SRWLOCK_INIT;
std::vector<std::unique_ptr<ProbeGuide>> g_guides;

ProbeGuide* FindGuide(uint8_t* probe)
{
    AcquireSRWLockShared(&g_guideLock);
    ProbeGuide* r = nullptr;
    for (auto& g : g_guides) if (g->probe == probe) { r = g.get(); break; }
    ReleaseSRWLockShared(&g_guideLock);
    return r;
}
void DropGuide(uint8_t* probe)
{
    AcquireSRWLockExclusive(&g_guideLock);
    g_guides.erase(std::remove_if(g_guides.begin(), g_guides.end(), [&](auto& g) { return g->probe == probe; }), g_guides.end());
    ReleaseSRWLockExclusive(&g_guideLock);
}

// Builds the learning state for a probe, null (and a log line) when the probe cannot be guided.
std::unique_ptr<ProbeGuide> NewGuide(uint8_t* probe)
{
    auto g = std::make_unique<ProbeGuide>();
    g->probe = probe;
    g->id = *reinterpret_cast<int*>(probe + off_Probe_Id);
    std::memcpy(g->dims, probe + off_Probe_Dims, sizeof g->dims);
    g->rays = *reinterpret_cast<uint32_t*>(probe + off_Probe_Rays);
    g->quality = *reinterpret_cast<uint32_t*>(probe + off_Probe_Quality);
    g->half = g->rays / 2;
    const char* why = nullptr;
    uint64_t b = 0, e = 0;
    if (g->dims[0] <= 0 || g->dims[1] <= 0 || g->dims[2] <= 0 || g->dims[0] % 4 || g->dims[1] % 4 || g->dims[2] % 4) why = "grid dimensions are not multiples of 4";
    else if (g->rays < 64 || g->rays % 64) why = "unexpected ray count";
    else if (*(probe + off_Probe_TfarOnly)) why = "probe stores only rays past tfar";
    else if (!TableRange(g_uniformDirs, g->quality, 0, b, e) || (e - b) / 12 != g->rays) why = "uniform direction table does not match the ray count";
    else if (*reinterpret_cast<uint32_t*>(probe + off_Probe_Stride) != g->rays) why = "hit array stride differs from the ray count";
    if (why)
    {
        Log("bake guide: probe %d not guided: %s (dims %d %d %d, rays %u, quality %u)", g->id, why, g->dims[0], g->dims[1], g->dims[2], g->rays, g->quality);
        ++g_stats.refusedProbes;
        return nullptr;
    }
    g->voxels = uint32_t(g->dims[0]) * uint32_t(g->dims[1]) * uint32_t(g->dims[2]);
    g->gdVoxel.assign(g->voxels, 0);
    g->seed = Mix(uint64_t(g->id) * 0x9e3779b97f4a7c15ULL ^ 0x5851f42d4c957f2dULL);
    g->stockPoints.resize(g->rays);
    g->stockFaces.resize(g->rays);
    g->stockLobes.reserve(6 * size_t(g->rays));
    const float* dirs = reinterpret_cast<const float*>(b);
    for (uint32_t i = 0; i < g->rays; ++i)
    {
        const float* d = dirs + 3 * i;
        try { g->stockPoints[i] = guiding::ToSquare({d[0], d[1], d[2]}); } catch (...) { g->stockPoints[i] = {0.5, 0.5}; }
        g->stockFaces[i] = static_cast<unsigned char>(Face(d));
        for (int a = 0; a < 3; ++a)
        {
            g->stockLobes.push_back(float(std::max(0.0f, d[a]) / guiding::Pi));
            g->stockLobes.push_back(float(std::max(0.0f, -d[a]) / guiding::Pi));
        }
    }
    return g;
}

// ---- per-thread state -----------------------------------------------------------------------------------------------
struct GuidedSample { float dir[3]; float weight; float basisA[6]; float basisB[6]; int mipQuarters = 0; };
struct Tls
{
    ProbeGuide* g = nullptr;
    uint32_t voxel = 0;
    bool learning = false, guided = false;
    uint64_t beginA = 0, endA = 0;
    // learning
    std::vector<float> lum;                          // per ray, NaN = not seen, -1 = miss
    std::vector<float> contribution;                 // per ray training weight
    // guided
    std::vector<GuidedSample> samples;               // rays [half, N)
    std::vector<uint16_t> savedDist, savedFlags;     // the stock hits of rays [half, N) while a voxel is guided
    double sum[2]{}, sum2[2]{}, raw[2]{}; uint32_t n[2]{};
    double faceSum[2][6]{}, faceSum2[2][6]{}; float threshold = 0; uint32_t bright[2]{};
    // local mode
    std::unique_ptr<Training> localTraining;
    std::unique_ptr<Guide> localGuide;
    bool captureStockB = false, haveStockB = false;
    float stockB[18]{}; float stockBW = 0;
    double stockBSum[6]{}, stockBSum2[6]{}; uint32_t stockBN = 0;
    bool haveStockA = false; float stockA[18]{}; float stockAW = 0;
    double stockASum[6]{}, stockASum2[6]{}; uint32_t stockAN = 0;
    uint32_t gFirst = 0;                             // first guided ray index (half, or 0 with the full split)
};
thread_local Tls t_tls;

// Index of the ray whose basis row this is, or -1 when the pointer is not in the current probe's table.
int64_t RayIndex(const Tls& t, const float* basisA)
{
    const uint64_t p = uint64_t(basisA);
    if (p < t.beginA || p >= t.endA || (p - t.beginA) % 24) return -1;
    return int64_t((p - t.beginA) / 24);
}

bool PrepareVoxel(Tls& t, ProbeGuide& g, uint32_t voxel, const Guide& guide, uint32_t first)
{
    try
    {
        // One point set per guided half, so each half is a complete sample of the guide density on its own.
        t.gFirst = first;
        t.samples.resize(g.rays - first);
        std::vector<Sample> s(g.half);
        for (uint32_t h = first / g.half; h < 2; ++h)
        {
        const uint64_t seed0 = g_set.decorrelate ? Mix(g.seed ^ Mix(voxel)) : g.seed;
        const uint64_t seed = h ? seed0 : Mix(seed0 ^ 0xa0761d6478bd642fULL);
        for (uint32_t k = 0; k < g.half; ++k) s[k] = guide.Draw(guiding::Sequence(k, g.half, seed), k);
        guiding::CoherentOrder(s);                  // directionally coherent 32-ray packets, the estimator is permutation invariant
        const uint32_t offset = h * g.half - first;
        for (uint32_t k = 0; k < g.half; ++k)
        {
            auto& o = t.samples[offset + k];
            o.dir[0] = float(s[k].direction.x); o.dir[1] = float(s[k].direction.y); o.dir[2] = float(s[k].direction.z);
            const double w = 1.0 / (4.0 * guiding::Pi * s[k].pdf);
            if (!std::isfinite(w) || w <= 0) return false;
            o.weight = float(w);
            o.mipQuarters = 4 * g_set.guidedMipDelta;
            std::memset(o.basisA, 0, sizeof o.basisA); std::memset(o.basisB, 0, sizeof o.basisB);
            g_basisA(o.dir[0], o.dir[1], o.dir[2], o.basisA);
            g_basisB(o.dir[0], o.dir[1], o.dir[2], o.basisB);
        }
        }
        return true;
    }
    catch (...) { return false; }
}

// ---- learning and guide build ---------------------------------------------------------------------------------------
void FinishLearningVoxel(Tls& t, ProbeGuide& g, Training& local)
{
    double faceSum[6]{}; uint32_t faceN[6]{}; double all = 0; uint32_t allN = 0;
    for (uint32_t i = 0; i < g.rays; ++i)
    {
        const float l = t.lum[i];
        if (std::isnan(l) || l < 0) continue;
        faceSum[g.stockFaces[i]] += l; ++faceN[g.stockFaces[i]]; all += l; ++allN;
    }
    uint32_t seen = 0;
    for (uint32_t i = 0; i < g.rays; ++i) if (!std::isnan(t.lum[i])) ++seen;
    if (seen < g.rays / 2) return;                  // not a normal gather of this voxel
    // Fill misses the way Radiant does (the average of their cube face).
    for (uint32_t i = 0; i < g.rays; ++i)
    {
        float& l = t.lum[i];
        if (std::isnan(l) || l >= 0) continue;
        const unsigned f = g.stockFaces[i];
        l = faceN[f] ? float(faceSum[f] / faceN[f]) : allN ? float(all / allN) : 0.0f;
    }
    // Target density. Luminance: p ~ L, the paper's choice for one irradiance lobe. Faces: BO3 fits six cube faces, so learn
    // p ~ L * sum_f lobe_f / I_f with I_f = this voxel's estimate of face f, a mixture of the six faces' product densities
    // in which dim faces get as many rays as bright ones. Each voxel adds the same total so bright voxels do not dominate.
    double faceIntegral[6]{};
    if (g_set.target == Target::Faces)
        for (uint32_t i = 0; i < g.rays; ++i)
            if (!std::isnan(t.lum[i])) for (int f = 0; f < 6; ++f) faceIntegral[f] += double(t.lum[i]) * g.stockLobes[6 * size_t(i) + f];
    double faceMax = 0;
    for (double v : faceIntegral) faceMax = std::max(faceMax, v);
    t.contribution.assign(g.rays, 0.0f);
    double total = 0;
    for (uint32_t i = 0; i < g.rays; ++i)
    {
        if (std::isnan(t.lum[i])) continue;
        double x = t.lum[i];
        if (g_set.target == Target::Faces)
        {
            double s = 0;
            for (int f = 0; f < 6; ++f) if (faceIntegral[f] > 1e-6 * faceMax && faceMax > 0) s += g.stockLobes[6 * size_t(i) + f] / faceIntegral[f];
            x *= s;
        }
        t.contribution[i] = float(x); total += x;
    }
    if (!(total > 0)) return;
    const double scale = double(seen) / total;
    for (uint32_t i = 0; i < g.rays; ++i)
        if (!std::isnan(t.lum[i])) local.AddPoint(g.stockPoints[i], double(t.contribution[i]) * scale);
    g_stats.learnedSamples += seen;
    ++g_stats.learnedVoxels;
}

// ---- hooks ----------------------------------------------------------------------------------------------------------
void BounceInfo(int& state, int& bounce, int& count)
{
    state = bounce = count = -1;
    uint8_t* cam = g_camWnd ? g_camWnd() : nullptr;
    if (!cam) return;
    state = *reinterpret_cast<int*>(cam + off_Cam_LightState);
    if (state < 0 || state > 3) return;
    bounce = *reinterpret_cast<int*>(cam + off_Cam_Bounce);
    count = *reinterpret_cast<int*>(cam + 244 * (state + 17));
}

// One probe's final-bounce result: "RGP1", id, dims[3], bounce, count, guided flag, matrix[9], origin[3],
// flags (uint64 per voxel), cube (18 floats per voxel).
void DumpOne(uint8_t* p, int bounce, int count, bool guided)
{
    std::string dir, tag;
    { std::lock_guard<std::mutex> lock(g_dumpMutex); dir = g_dumpDir; tag = g_dumpTag; }
    if (dir.empty()) return;
    int32_t hdr[7]{*reinterpret_cast<int*>(p + off_Probe_Id), 0, 0, 0, bounce, count, guided ? 1 : 0};
    std::memcpy(hdr + 1, p + off_Probe_Dims, 12);
    if (hdr[1] <= 0 || hdr[2] <= 0 || hdr[3] <= 0) return;
    const uint64_t voxels = uint64_t(hdr[1]) * hdr[2] * hdr[3];
    const uint64_t* flags = *reinterpret_cast<uint64_t**>(p + off_Probe_Flags);
    const float* cube = *reinterpret_cast<float**>(p + off_Probe_Coeffs);
    if (!flags || !cube || voxels > 64'000'000) return;
    char name[512];
    snprintf(name, sizeof name, "%s\\%s_probe%dx%dx%d_b%d.rgp", dir.c_str(), tag.c_str(), hdr[1], hdr[2], hdr[3], bounce);
    FILE* f = nullptr;
    if (fopen_s(&f, name, "wb") || !f) { Log("bake guide: cannot write %s", name); return; }
    float m[12]{};
    std::memcpy(m, p + off_Probe_Matrix, 36); std::memcpy(m + 9, p, 12);
    fwrite("RGP1", 1, 4, f); fwrite(hdr, 4, 7, f); fwrite(m, 4, 12, f);
    fwrite(flags, 8, size_t(voxels), f); fwrite(cube, 4, size_t(18 * voxels), f);
    fclose(f);
    Log("bake guide: wrote %s", name);
}

// Every probe's final values after state 5 (what the game gets): "<tag>_smooth" (or "_nosmooth" when the smoothing was
// skipped), same RGP1 layout as DumpOne with +1360 in place of +1752.
void DumpFinal(bool smoothed)
{
    std::string dir, tag;
    { std::lock_guard<std::mutex> lock(g_dumpMutex); dir = g_dumpDir; tag = g_dumpTag; }
    uint8_t* cam = g_camWnd ? g_camWnd() : nullptr;
    if (dir.empty() || !cam) return;
    int state, bounce, count;
    BounceInfo(state, bounce, count);
    uint8_t* pb = *reinterpret_cast<uint8_t**>(cam + 6664), *pe = *reinterpret_cast<uint8_t**>(cam + 6672);
    if (!pb || pe < pb) return;
    int written = 0, skipped = 0;
    for (uint8_t* e = pb; e + 112 <= pe; e += 112)
    {
        uint8_t* p = *reinterpret_cast<uint8_t**>(e + 40);
        if (!p) continue;
        int32_t hdr[7]{*reinterpret_cast<int*>(p + off_Probe_Id), 0, 0, 0, std::max(0, count - 1), count, 0};
        std::memcpy(hdr + 1, p + off_Probe_Dims, 12);
        if (hdr[1] <= 0 || hdr[2] <= 0 || hdr[3] <= 0) continue;
        const uint64_t voxels = uint64_t(hdr[1]) * hdr[2] * hdr[3];
        const uint64_t* flags = *reinterpret_cast<uint64_t**>(p + off_Probe_Flags);
        const float* fb = *reinterpret_cast<float**>(p + off_Probe_Final);
        const float* fe = *reinterpret_cast<float**>(p + off_Probe_Final + 8);
        if (!flags || !fb || fe < fb || uint64_t(fe - fb) != 18 * voxels || voxels > 64'000'000) { ++skipped; continue; }
        char name[512];
        snprintf(name, sizeof name, "%s\\%s_%s_probe%dx%dx%d_b%d.rgp", dir.c_str(), tag.c_str(), smoothed ? "smooth" : "nosmooth",
                 hdr[1], hdr[2], hdr[3], hdr[4]);
        FILE* f = nullptr;
        if (fopen_s(&f, name, "wb") || !f) { ++skipped; continue; }
        float m[12]{};
        std::memcpy(m, p + off_Probe_Matrix, 36); std::memcpy(m + 9, p, 12);
        fwrite("RGP1", 1, 4, f); fwrite(hdr, 4, 7, f); fwrite(m, 4, 12, f);
        fwrite(flags, 8, size_t(voxels), f); fwrite(fb, 4, size_t(18 * voxels), f);
        fclose(f);
        ++written;
    }
    Log("bake: post-smooth dump (%s): %d probes written, %d without final values", smoothed ? "smoothed" : "smoothing skipped", written, skipped);
}

char __fastcall hk_SmoothTask(void* capture)
{
    const char smoothed = o_SmoothTask(capture);
    try { DumpFinal(smoothed != 0); } catch (...) {}
    return smoothed;
}

int64_t __fastcall hk_VoxelTrace(int64_t probe, int64_t voxel, int64_t scene, const float* matrix)
{
    if (probe && scene && matrix) RecordTrace(reinterpret_cast<uint8_t*>(probe), scene, matrix);
    return o_VoxelTrace(probe, voxel, scene, matrix);
}

// Points the guide at the scene, volume list and fix-up values of the current bounce job. False when the trace pass of this
// probe was not observed (Radiant re-traces a probe only when it changes).
bool BindJob(ProbeGuide& g, uint8_t* p, int64_t volumes, int64_t scene)
{
    TraceInfo recorded;
    if (!scene || !volumes || !(FindTrace(p, recorded) && recorded.haveFixup && recorded.fixup.rays == int32_t(g.rays))) return false;
    const FixupValues keep = g.trace.fixup;
    const bool bound = g.fixupCtx[6] != 0;
    g.trace = recorded;
    if (bound && std::memcmp(&keep, &recorded.fixup, sizeof keep)) return false;   // the probe was re-traced mid-bake
    g.trace.scene = scene;
    std::memcpy(g.trace.matrix, p + off_Probe_Matrix, sizeof g.trace.matrix);
    g.volumes = volumes;
    FixupValues& f = g.trace.fixup;
    const int64_t slots[15] = {int64_t(f.dims), int64_t(&f.offset), int64_t(f.invDims), int64_t(f.slot3), int64_t(f.halfExtent),
                               int64_t(f.matrix), int64_t(p), volumes, int64_t(f.origin), int64_t(f.matrixCols), int64_t(&f.rays),
                               int64_t(&f.maxDistance), int64_t(&f.gridMode), int64_t(&f.quality), int64_t(&f.threshold)};
    std::memcpy(g.fixupCtx, slots, sizeof slots);
    return true;
}

bool LayoutMatches(const ProbeGuide& g)
{
    return *reinterpret_cast<uint32_t*>(g.probe + off_Probe_Rays) == g.rays && *reinterpret_cast<uint32_t*>(g.probe + off_Probe_Stride) == g.rays &&
           *reinterpret_cast<uint32_t*>(g.probe + off_Probe_Quality) == g.quality && !std::memcmp(g.probe + off_Probe_Dims, g.dims, sizeof g.dims);
}

// The cached sampler decodes bits 4..9 in quarter-mip units. Keep all visibility/volume bits.
// A -1 mip shift approximates N*4 cones, -2 approximates N*16, without changing ray array bounds.
uint16_t ShiftHitMip(uint16_t flags, int delta)
{
    return guiding::ShiftHitMipQuarters(flags, 4 * delta);
}

void RetraceHalf(Tls& t, ProbeGuide& g, uint32_t voxel, bool guided, uint32_t first = UINT32_MAX)
{
    if (first == UINT32_MAX) first = g.half;
    uint64_t* voxelFlags = *reinterpret_cast<uint64_t**>(g.probe + off_Probe_Flags);
    const uint64_t savedVoxelFlags = voxelFlags[voxel];
    t.g = &g; t.voxel = voxel; t.guided = guided;
    uint32_t c1 = 0, c2 = 0, c3 = 0; float acc12[12]{};
    for (uint32_t start = first; start < g.rays; start += 32)
        g_trace32(int64_t(g.probe), voxel, start, 0, g.trace.scene, &c1, &c2, &c3, acc12, g.trace.matrix);
    o_HitFixup(g.fixupCtx, voxel);                  // resolve each new hit's lighting volume and detail level
    if (guided && g_set.guidedMipDelta)
    {
        uint16_t* flags = *reinterpret_cast<uint16_t**>(g.probe + off_Probe_HitFlags) + size_t(voxel) * g.rays;
        int64_t requested = 0, applied = 0;
        uint64_t resolved = 0, clipped = 0, sharper = 0, blurrier = 0;
        for (uint32_t i = first; i < g.rays; ++i)
        {
            const int q = t.samples[i - first].mipQuarters;
            requested += q;
            const uint16_t before = flags[i];
            if ((before & 0x3000) == 0x3000 || (before & 0x400)) continue;
            flags[i] = guiding::ShiftHitMipQuarters(before, q);
            const int actual = int((flags[i] >> 4) & 63) - int((before >> 4) & 63);
            applied += actual; ++resolved; clipped += actual != q;
            sharper += actual < 0; blurrier += actual > 0;
        }
        if (g_set.observe)
        {
            g.mipRequested += requested; g.mipApplied += applied; g.mipRays += g.rays - first;
            g.mipResolved += resolved; g.mipClipped += clipped; g.mipSharper += sharper; g.mipBlurrier += blurrier;
        }
    }
    voxelFlags[voxel] = savedVoxelFlags;           // guiding never changes which voxels Radiant counts as valid
    t.guided = false; t.g = nullptr;
}

// Everything the per-voxel gather writes for one voxel (cube, reference radiance, half difference, voxel flags, the job's
// per-voxel done byte and six per-voxel outputs), so a dry-run gather can be undone.
struct VoxelOutputs { float cube[18]; float ref[3]; float extra[6]; float conv; uint64_t flags; uint8_t done; };
void SaveOutputs(uint8_t* probe, const int64_t* ctx, uint32_t v, VoxelOutputs& o)
{
    std::memcpy(o.cube, *reinterpret_cast<float**>(probe + off_Probe_Coeffs) + 18 * size_t(v), sizeof o.cube);
    std::memcpy(o.ref, *reinterpret_cast<float**>(probe + 1672) + 3 * size_t(v), sizeof o.ref);
    for (int k = 0; k < 6; ++k) o.extra[k] = (*reinterpret_cast<float**>(ctx[15] + 24 * k))[v];
    o.conv = (*reinterpret_cast<float**>(probe + 1480))[v];
    o.flags = (*reinterpret_cast<uint64_t**>(probe + off_Probe_Flags))[v];
    o.done = (*reinterpret_cast<uint8_t**>(ctx[3]))[v];
}
void RestoreOutputs(uint8_t* probe, const int64_t* ctx, uint32_t v, const VoxelOutputs& o)
{
    std::memcpy(*reinterpret_cast<float**>(probe + off_Probe_Coeffs) + 18 * size_t(v), o.cube, sizeof o.cube);
    std::memcpy(*reinterpret_cast<float**>(probe + 1672) + 3 * size_t(v), o.ref, sizeof o.ref);
    for (int k = 0; k < 6; ++k) (*reinterpret_cast<float**>(ctx[15] + 24 * k))[v] = o.extra[k];
    (*reinterpret_cast<float**>(probe + 1480))[v] = o.conv;
    (*reinterpret_cast<uint64_t**>(probe + off_Probe_Flags))[v] = o.flags;
    (*reinterpret_cast<uint8_t**>(ctx[3]))[v] = o.done;
}

// ---- cone correction ------------------------------------------------------------------------------------------------
// One probe's bounce job: the sharp-cone luminances of its lattice voxels (6 per voxel, NaN = not sampled).
struct ConeState
{
    uint8_t* probe = nullptr;
    int dims[3]{};
    uint32_t voxels = 0, rays = 0;
    int delta = 0;                     // mip levels to high's cone, one per factor 4 in rays (draft -3, low -2, medium -1)
    int bounce = -1, count = -1;
    std::vector<float> sharp;
    std::atomic<uint32_t> gathers{0};
    std::atomic<uint32_t> debugSkipped{0};
    std::atomic<bool> applied{false};
    SRWLOCK applyLock = SRWLOCK_INIT;
};
SRWLOCK g_coneLock = SRWLOCK_INIT;
std::vector<ConeState*> g_cones;       // live jobs, owned by their hk_ProbeGather frame

ConeState* FindCone(uint8_t* probe)
{
    ConeState* found = nullptr;
    AcquireSRWLockShared(&g_coneLock);
    for (ConeState* c : g_cones) if (c->probe == probe) { found = c; break; }
    ReleaseSRWLockShared(&g_coneLock);
    return found;
}

// Why a probe gets no cone correction this bounce, or null when it does (fills the state).
const char* PrepareCone(uint8_t* p, ConeState& c)
{
    const uint32_t quality = *reinterpret_cast<uint32_t*>(p + off_Probe_Quality);
    c.rays = *reinterpret_cast<uint32_t*>(p + off_Probe_Rays);
    std::memcpy(c.dims, p + off_Probe_Dims, sizeof c.dims);
    if (quality >= 2) return "high quality";
    if (*(p + off_Probe_TfarOnly)) return "tfar-only probe";
    if (c.dims[0] <= 0 || c.dims[1] <= 0 || c.dims[2] <= 0) return "no grid";
    const uint64_t voxels = uint64_t(c.dims[0]) * uint64_t(c.dims[1]) * uint64_t(c.dims[2]);
    if (voxels > 16'000'000) return "grid too large";
    if (!c.rays || c.rays > 16384) return "ray layout";
    // The hit arrays, voxel flags and cube array are unpacked or resized inside the job: ConeGather checks them per voxel.
    c.probe = p; c.voxels = uint32_t(voxels); c.delta = 0;
    for (uint32_t r = c.rays; r < 4096; r *= 4) --c.delta;
    c.sharp.assign(size_t(6) * c.voxels, std::nanf(""));
    return nullptr;
}

// The probe debugger's voxel (View > Debug > Debug Probe Voxel): the gather logs "Dumping Voxel", records its samples
// and writes the ProbeVoxelView images, a second gather would overwrite them with the sharp-cone samples, so that voxel
// stays out of the lattice. ctx[4] = byte* debug on, ctx[5] = uint[3] debug voxel, ctx[0] = uint[3] dims.
uint32_t g_debugVoxel[3]{};            // test key debug_voxel=x,y,z: forced as the gather's debug voxel (headless test)
bool g_debugVoxelOn = false, g_coneDebugSkip = true;
bool g_draftAdaptive = false;           // test key low_rays_adaptive=1: the draft tier keeps the adaptive pass
std::atomic<uint32_t> g_debugCaptures{0}, g_debugSkippedTotal{0};   // Radiant's debug-voxel capture starts (0x14116C230), counted in that test
using CaptureBeginFn = char(__fastcall*)(int, int);
CaptureBeginFn o_CaptureBegin;
char __fastcall hk_CaptureBegin(int w, int h) { ++g_debugCaptures; return o_CaptureBegin(w, h); }

bool IsDebugVoxel(const int64_t* ctx, uint32_t v)
{
    const uint8_t* on = reinterpret_cast<const uint8_t*>(ctx[4]);
    if (!on || !*on) return false;
    const uint32_t* dv = reinterpret_cast<const uint32_t*>(ctx[5]);
    const uint32_t* dims = reinterpret_cast<const uint32_t*>(ctx[0]);
    if (!dv || !dims) return false;
    return dv[0] < dims[0] && dv[1] < dims[1] && dv[2] < dims[2] && v == dv[0] + dims[0] * (dv[1] + dims[1] * dv[2]);
}

// Stock gather, then (lattice voxels only) a second gather with every resolved hit's mip shifted to high's cone, the
// second result is recorded and every output of it undone, so Radiant sees only the stock gather.
int64_t ConeGather(int64_t* ctx, int64_t voxelArg, ConeState& c)
{
    const int64_t r = StockGather(ctx, voxelArg);
    const uint32_t v = uint32_t(voxelArg);
    if (v >= c.voxels || !cone::OnLattice(v, c.dims[0], c.dims[1], c.dims[2], g_set.coneStride)) return r;
    if (g_coneDebugSkip && IsDebugVoxel(ctx, v)) { ++c.debugSkipped; return r; }
    const uint64_t* voxelFlags = *reinterpret_cast<uint64_t**>(c.probe + off_Probe_Flags);
    if (!voxelFlags || !*reinterpret_cast<uint16_t**>(c.probe + off_Probe_HitFlags) || !*reinterpret_cast<float**>(c.probe + off_Probe_Coeffs) ||
        *reinterpret_cast<uint32_t*>(c.probe + off_Probe_Stride) != c.rays || *reinterpret_cast<uint32_t*>(c.probe + off_Probe_Rays) != c.rays) return r;
    const uint64_t vflags = voxelFlags[v];
    if (!(vflags & kVoxelValid) || !(vflags & kVoxelTraced)) return r;
    uint16_t* begin = *reinterpret_cast<uint16_t**>(c.probe + off_Probe_HitFlags) + size_t(v) * c.rays;
    thread_local std::vector<uint16_t> original;
    VoxelOutputs saved;
    try { original.assign(begin, begin + c.rays); }
    catch (...) { ++g_stats.prepareFailures; return r; }
    SaveOutputs(c.probe, ctx, v, saved);
    for (uint32_t i = 0; i < c.rays; ++i) begin[i] = ShiftHitMip(begin[i], c.delta);
    StockGather(ctx, voxelArg);
    const float* cube = *reinterpret_cast<float**>(c.probe + off_Probe_Coeffs) + 18 * size_t(v);
    for (int f = 0; f < 6; ++f) c.sharp[6 * size_t(v) + f] = cone::Luma(cube + 3 * f);
    std::memcpy(begin, original.data(), c.rays * sizeof(uint16_t));
    RestoreOutputs(c.probe, ctx, v, saved);
    ++c.gathers;
    return r;
}

// Once per probe job, from the first final-stage voxel (after the gather, adaptive pass and free-space fill, the other
// workers wait on applyLock): rescale every voxel by the smoothed sharp/stock ratio.
void FinishCone(ConeState& c)
{
    const int bounce = c.bounce, count = c.count;
    float* coeffs = *reinterpret_cast<float**>(c.probe + off_Probe_Coeffs);
    if (!coeffs) return;
    const cone::Result res = cone::Apply(coeffs, c.sharp.data(), c.dims[0], c.dims[1], c.dims[2], g_set.coneRadius,
                                        0.2f, 5.0f, g_set.coneEdge);
    g_stats.coneGathers += c.gathers; g_stats.coneSamples += res.samples; g_stats.coneScaled += res.scaled;
    if (count > 0 && bounce == count - 1 && res.scaled) { g_stats.coneRatioMilli += uint64_t(res.meanRatio * 1000 + 0.5); ++g_stats.coneRatioCount; }
    g_debugSkippedTotal += c.debugSkipped;
    if (c.debugSkipped)
        Log("bake cone: probe %d bounce %d: the probe debugger's voxel was left out of the sharp gathers", *reinterpret_cast<int*>(c.probe + off_Probe_Id), bounce);
    if (g_set.observe)
        Log("bake cone: probe %d bounce %d/%d dims %d %d %d delta %d: %u sharp gathers, %u samples, %u voxels scaled, mean ratio %.4f",
            *reinterpret_cast<int*>(c.probe + off_Probe_Id), bounce, count, c.dims[0], c.dims[1], c.dims[2], c.delta,
            c.gathers.load(), res.samples, res.scaled, res.meanRatio);
}

void ApplyLuminanceCorrection(ProbeGuide& g, double& rho, uint64_t& gd)
{
    rho = 1; gd = 0;
    if (g_set.combine != Combine::Variance) return;
    {
        std::lock_guard<std::mutex> lock(g.sumMutex);
        if (g.sumVariance > 0 && g.sumEqual > 0) rho = g.sumEqual / g.sumVariance;
        g.sumVariance = g.sumEqual = 0;
    }
    if (!std::isfinite(rho) || rho < 0.25 || rho > 4) rho = 1;
    float* coeffs = *reinterpret_cast<float**>(g.probe + off_Probe_Coeffs);
    for (uint32_t v = 0; v < g.voxels && coeffs; ++v)
        if (g.gdVoxel[v]) { ++gd; g.gdVoxel[v] = 0; for (int k = 0; k < 18; ++k) coeffs[18 * v + k] *= float(rho); }
}

void __fastcall hk_ProbeGather(int64_t probe, int64_t a2, int64_t volumes, int64_t a4, int64_t a5, int64_t a6, int64_t init,
                               int64_t scene, int64_t adaptive, int64_t a10, int64_t a11, int64_t a12)
{
    uint8_t* p = reinterpret_cast<uint8_t*>(probe);
    int state, bounce, count;
    BounceInfo(state, bounce, count);
    // The draft tier was switched and no build start reset this probe (bake_lowrays.h): its hit store has the other
    // Low ray count, so the job would misread it. The probe keeps the lighting it has until a build traces it again.
    if (bakell::Stale(p))
    {
        Log("bake: probe %d skipped, its trace has %u rays per voxel and Low is %u now (rebuild probes)", *reinterpret_cast<int*>(p + off_Probe_Id),
            *reinterpret_cast<uint32_t*>(p + off_Probe_Stride), bakell::LowRays());
        return;
    }
    const bool isInit = (init & 0xff) != 0;
    const bool on = g_set.enabled.load() && !isInit && bounce >= 0 && count >= 1;
    ProbeGuide* g = nullptr;
    const char* what = "stock";
    if (!isInit && bounce == 0) DropGuide(p);
    if (on)
    {
        if (bounce == 0)
        {
            try
            {
                if (auto fresh = NewGuide(p))
                {
                    g = fresh.get();
                    AcquireSRWLockExclusive(&g_guideLock);
                    // Reserve before taking this lock would race other probe jobs. Release even on allocation failure.
                    try { g_guides.push_back(std::move(fresh)); }
                    catch (...) { ReleaseSRWLockExclusive(&g_guideLock); throw; }
                    ReleaseSRWLockExclusive(&g_guideLock);
                }
            }
            catch (...) { g = nullptr; ++g_stats.prepareFailures; }
        }
        else g = FindGuide(p);
        if (g && !BindJob(*g, p, volumes, scene))
        {
            Log("bake guide: probe %d trace unavailable or changed; guiding stopped", g->id);
            ++g_stats.refusedProbes; DropGuide(p); g = nullptr;
        }
        if (g) what = "local guided";
    }

    if (g) { g->rhoApplied = false; g->lastRho = 1; g->lastGd = 0; }
    // Cone correction for this job (not for guided probes: the guided gather already runs its own passes).
    std::unique_ptr<ConeState> cone;
    if (g_set.cone && !isInit && !g)
    {
        try
        {
            cone = std::make_unique<ConeState>();
            if (const char* why = PrepareCone(p, *cone))
            {
                if (g_set.observe && bounce == 0) Log("bake cone: probe %d not corrected: %s", *reinterpret_cast<int*>(p + off_Probe_Id), why);
                cone.reset(); ++g_stats.coneSkipped;
            }
            else
            {
                cone->bounce = bounce; cone->count = count;
                AcquireSRWLockExclusive(&g_coneLock);
                try { g_cones.push_back(cone.get()); }
                catch (...) { ReleaseSRWLockExclusive(&g_coneLock); throw; }
                ReleaseSRWLockExclusive(&g_coneLock);
            }
        }
        catch (...) { cone.reset(); ++g_stats.prepareFailures; }
    }
    // Adaptive override: the gather reads the probe's level on entry (0 turns its adaptive pass off), so raise it for the
    // call and put the map's value back unless something else changed it meanwhile.
    int* level = reinterpret_cast<int*>(p + off_Probe_Adaptive);
    const int want = g_set.adaptive.load(), had = *level;
    // The draft tier (bake_lowrays.h) bakes without it: its 32-ray halves disagree almost everywhere, and the pass would
    // re-shoot those voxels at Medium's ray count.
    const bool draft = !isInit && bakell::Active() && *reinterpret_cast<uint32_t*>(p + off_Probe_Rays) == bakell::LowRays() && !g_draftAdaptive;
    const bool raised = !isInit && !draft && want > had && want <= 3;
    if (raised) { *level = want; ++g_stats.adaptiveRaised; }
    if (draft && had > 0) *level = 0;

    const double t0 = Now();
    if (g_debugVoxelOn)   // test key debug_voxel: the job's own debug arguments (uint[3]* voxel, char on)
    { a2 = reinterpret_cast<int64_t>(g_debugVoxel); a6 = (a6 & ~int64_t(0xff)) | 1; }
    o_ProbeGather(probe, a2, volumes, a4, a5, a6, init, scene, adaptive, a10, a11, a12);
    const double t1 = Now();
    if (raised && *level == want) *level = had;
    if (draft && had > 0 && *level == 0) *level = had;
    if (!isInit && count > 0)
    {
        g_stats.gatherMs += uint64_t((t1 - t0) * 1000);
        if (bounce == count - 1) g_stats.finalMs += uint64_t((t1 - t0) * 1000);
    }
    if (cone)
    {
        AcquireSRWLockExclusive(&g_coneLock);
        g_cones.erase(std::remove(g_cones.begin(), g_cones.end(), cone.get()), g_cones.end());
        ReleaseSRWLockExclusive(&g_coneLock);
        if (!cone->applied && g_set.observe) Log("bake cone: probe %d bounce %d: the final stage never ran, not corrected", *reinterpret_cast<int*>(p + off_Probe_Id), bounce);
    }
    char extra[128] = "";
    if (g)
    {
        const double rho = g->lastRho; const uint64_t gd = g->lastGd;
        const uint64_t rays = g->mipRays.exchange(0), resolved = g->mipResolved.exchange(0);
        const int64_t requested = g->mipRequested.exchange(0), applied = g->mipApplied.exchange(0);
        const uint64_t clipped = g->mipClipped.exchange(0), sharper = g->mipSharper.exchange(0), blurrier = g->mipBlurrier.exchange(0);
        if (g_set.observe && rays)
            Log("bake guide: cone probe %d bounce %d mode %s requested_mean %.5f applied_mean %.5f rays %llu resolved %llu clipped %llu sharper %llu blurrier %llu",
                    g->id, bounce, "fixed", double(requested) / (4.0 * rays),
                    resolved ? double(applied) / (4.0 * resolved) : 0.0, (unsigned long long)rays, (unsigned long long)resolved,
                    (unsigned long long)clipped, (unsigned long long)sharper, (unsigned long long)blurrier);
        snprintf(extra, sizeof extra, " guided %llu voxels, %llu by inverse variance, rho %.4f",
                 (unsigned long long)g->guided.exchange(0), (unsigned long long)gd, rho);
    }
    if (!isInit && bounce >= 0 && (bounce == count - 1 || g_dumpAll)) DumpOne(p, bounce, count, g != nullptr);
    if (g && bounce == count - 1) DropGuide(p);
    if (g_set.observe.load())
        Log("bake guide: probe %d state %d bounce %d/%d dims %d %d %d rays %u quality %u: %s %.3f s%s",
                *reinterpret_cast<int*>(p + off_Probe_Id), state, bounce, count,
                *reinterpret_cast<int*>(p + off_Probe_Dims), *reinterpret_cast<int*>(p + off_Probe_Dims + 4), *reinterpret_cast<int*>(p + off_Probe_Dims + 8),
                *reinterpret_cast<uint32_t*>(p + off_Probe_Rays), *reinterpret_cast<uint32_t*>(p + off_Probe_Quality), what, t1 - t0, extra);
}

int64_t __fastcall hk_GatherVoxel(int64_t* ctx, int64_t voxelArg)
{
    ProbeGuide* g = ctx ? FindGuide(reinterpret_cast<uint8_t*>(ctx[1])) : nullptr;
    if (!g && ctx)
        if (ConeState* c = FindCone(reinterpret_cast<uint8_t*>(ctx[1]))) return ConeGather(ctx, voxelArg, *c);
    if (!g && ctx && g_set.stockMipDelta)
    {
        uint8_t* p = reinterpret_cast<uint8_t*>(ctx[1]);
        const uint32_t voxel = uint32_t(voxelArg), rays = *reinterpret_cast<uint32_t*>(p + off_Probe_Rays);
        const uint32_t stride = *reinterpret_cast<uint32_t*>(p + off_Probe_Stride);
        uint16_t* flags = *reinterpret_cast<uint16_t**>(p + off_Probe_HitFlags);
        const uint64_t* vf = *reinterpret_cast<uint64_t**>(p + off_Probe_Flags);
        if (flags && vf && (vf[voxel] & kVoxelValid) && rays && rays == stride && rays <= 8192)
        {
            thread_local std::vector<uint16_t> original;
            uint16_t* begin = flags + size_t(voxel) * stride;
            try { original.assign(begin, begin + rays); }
            catch (...) { ++g_stats.prepareFailures; return o_GatherVoxel(ctx, voxelArg); }
            for (uint32_t i = 0; i < rays; ++i) begin[i] = ShiftHitMip(begin[i], g_set.stockMipDelta);
            const int64_t result = o_GatherVoxel(ctx, voxelArg);
            std::memcpy(begin, original.data(), rays * sizeof(uint16_t));
            return result;
        }
    }
    if (!g) return StockGather(ctx, voxelArg);
    const uint32_t voxel = uint32_t(voxelArg);
    Tls& t = t_tls;
    if (voxel >= g->voxels || !LayoutMatches(*g) || !TableRange(g_basisTableA, g->quality, 0, t.beginA, t.endA)) return o_GatherVoxel(ctx, voxelArg);
    {
        const uint64_t lflags = (*reinterpret_cast<uint64_t**>(g->probe + off_Probe_Flags))[voxel];
        uint16_t* dist = *reinterpret_cast<uint16_t**>(g->probe + off_Probe_HitDist);
        uint16_t* hitFlags = *reinterpret_cast<uint16_t**>(g->probe + off_Probe_HitFlags);
        if (!(lflags & kVoxelValid) || !(lflags & kVoxelTraced) || g->validation < 0 || !dist || !hitFlags) return o_GatherVoxel(ctx, voxelArg);
        const size_t base = size_t(voxel) * g->rays + g->half;
        const uint32_t first = g_set.fullSplit ? 0 : g->half;
        std::vector<uint16_t> savedA, savedAFlags;
        try
        {
            t.savedDist.assign(dist + base, dist + base + g->half);
            t.savedFlags.assign(hitFlags + base, hitFlags + base + g->half);
            t.lum.assign(g->rays, std::nanf(""));
            if (first == 0)
            {
                const size_t a0 = size_t(voxel) * g->rays;
                savedA.assign(dist + a0, dist + a0 + g->half);
                savedAFlags.assign(hitFlags + a0, hitFlags + a0 + g->half);
            }
        }
        catch (...) { ++g_stats.prepareFailures; return o_GatherVoxel(ctx, voxelArg); }
        if (g->validation < 4)
        {
            RetraceHalf(t, *g, voxel, false);
            uint32_t same = 0;
            for (uint32_t k = 0; k < g->half; ++k) same += dist[base + k] == t.savedDist[k] && hitFlags[base + k] == t.savedFlags[k];
            std::memcpy(dist + base, t.savedDist.data(), g->half * sizeof(uint16_t));
            std::memcpy(hitFlags + base, t.savedFlags.data(), g->half * sizeof(uint16_t));
            ++g->validation;
            if (same < g->half - g->half / 32)
            { g->validation = -1; Log("bake guide: probe %d self-check failed, guiding stopped for this probe", g->id); return o_GatherVoxel(ctx, voxelArg); }
        }
        // 1. dry run with stock rays: per-ray luminance for this voxel's own guide and the stock second-half batch
        VoxelOutputs saved;
        SaveOutputs(g->probe, ctx, voxel, saved);
        std::memset(t.stockBSum, 0, sizeof t.stockBSum); std::memset(t.stockBSum2, 0, sizeof t.stockBSum2);
        std::memset(t.stockASum, 0, sizeof t.stockASum); std::memset(t.stockASum2, 0, sizeof t.stockASum2);
        t.stockBN = t.stockAN = 0; t.haveStockB = t.haveStockA = false; t.captureStockB = true;
        t.g = g; t.voxel = voxel; t.learning = true;
        o_GatherVoxel(ctx, voxelArg);
        t.learning = false; t.captureStockB = false; t.g = nullptr;
        // ctx[15] is the voxel's baked exposure (geometric mean of ray luminance per face, every ray counted once): guided
        // rays aim at bright directions and would raise it, so the stock rays' values are kept
        float stockExposure[6];
        for (int k = 0; k < 6; ++k) stockExposure[k] = (*reinterpret_cast<float**>(ctx[15] + 24 * k))[voxel];
        RestoreOutputs(g->probe, ctx, voxel, saved);
        // 2. this voxel's guide
        try
        {
            if (!t.localTraining) t.localTraining = std::make_unique<Training>();
            if (!t.localGuide) t.localGuide = std::make_unique<Guide>();
            t.localTraining->Clear();
            FinishLearningVoxel(t, *g, *t.localTraining);
            if (t.localTraining->count[0] < g->rays / 2 || !(t.localTraining->flux[0] > 0)) { t.haveStockB = false; return o_GatherVoxel(ctx, voxelArg); }
            guiding::Settings s; s.smoothing = g_set.smoothing; s.uniformFraction = g_set.uniform;
            t.localGuide->Build(*t.localTraining, s);
            ++g_stats.guides;
        }
        catch (...) { t.haveStockB = false; ++g_stats.prepareFailures; return o_GatherVoxel(ctx, voxelArg); }
        if (!PrepareVoxel(t, *g, voxel, *t.localGuide, g_set.fullSplit ? 0 : g->half)) { t.haveStockB = false; ++g_stats.prepareFailures; return o_GatherVoxel(ctx, voxelArg); }
        // 3. guided second half, gathered and blended with both stock halves, the stock hits go back for the rest of the job
        RetraceHalf(t, *g, voxel, true, first);
        g_stats.retracedRays += g->rays - first;
        t.sum[0] = t.sum[1] = t.sum2[0] = t.sum2[1] = t.raw[0] = t.raw[1] = 0; t.n[0] = t.n[1] = 0;
        std::memset(t.faceSum, 0, sizeof t.faceSum); std::memset(t.faceSum2, 0, sizeof t.faceSum2); t.bright[0] = t.bright[1] = 0;
        t.g = g; t.voxel = voxel; t.guided = true;
        const int64_t r = o_GatherVoxel(ctx, voxelArg);
        t.guided = false; t.g = nullptr; t.haveStockB = t.haveStockA = false; t.gFirst = g->half;
        for (int k = 0; k < 6; ++k) (*reinterpret_cast<float**>(ctx[15] + 24 * k))[voxel] = stockExposure[k];
        std::memcpy(dist + base, t.savedDist.data(), g->half * sizeof(uint16_t));
        std::memcpy(hitFlags + base, t.savedFlags.data(), g->half * sizeof(uint16_t));
        if (first == 0)
        {
            const size_t a0 = size_t(voxel) * g->rays;
            std::memcpy(dist + a0, savedA.data(), g->half * sizeof(uint16_t)); std::memcpy(hitFlags + a0, savedAFlags.data(), g->half * sizeof(uint16_t));
        }
        ++g->guided; ++g_stats.guidedVoxels;
        return r;
    }
}

int64_t __fastcall hk_VoxelFinal(int64_t* ctx, int64_t voxel)
{
    if (ctx)
        if (ProbeGuide* g = FindGuide(reinterpret_cast<uint8_t*>(ctx[0])); g && !g->rhoApplied.load(std::memory_order_acquire))
        {
            AcquireSRWLockExclusive(&g->rhoLock);
            if (!g->rhoApplied.load(std::memory_order_relaxed))
            {
                try { ApplyLuminanceCorrection(*g, g->lastRho, g->lastGd); }
                catch (...) { ++g_stats.prepareFailures; }
                g->rhoApplied.store(true, std::memory_order_release);
            }
            ReleaseSRWLockExclusive(&g->rhoLock);
        }
    if (ctx)
        if (ConeState* c = FindCone(reinterpret_cast<uint8_t*>(ctx[0])); c && !c->applied.load(std::memory_order_acquire))
        {
            AcquireSRWLockExclusive(&c->applyLock);
            if (!c->applied.load(std::memory_order_relaxed))
            {
                try { FinishCone(*c); }
                catch (...) { ++g_stats.prepareFailures; }
                c->applied.store(true, std::memory_order_release);
            }
            ReleaseSRWLockExclusive(&c->applyLock);
        }
    return o_VoxelFinal(ctx, voxel);
}

int64_t __fastcall hk_HitFixup(int64_t* ctx, int64_t voxel)
{
    // Capture the pass's inputs once per probe trace (the first voxel), for re-running it on guided hits later.
    if (ctx && ctx[6] && uint32_t(voxel) % 4096 == 0) RecordFixup(reinterpret_cast<uint8_t*>(ctx[6]), ctx);
    return o_HitFixup(ctx, voxel);
}

const float* __fastcall hk_DirLookup(int64_t quality, int64_t set, int64_t index)
{
    Tls& t = t_tls;
    if (t.guided && t.g && uint32_t(quality) == t.g->quality && uint32_t(set) == 0 && uint32_t(index) >= t.gFirst && uint32_t(index) < t.g->rays)
        return t.samples[uint32_t(index) - t.gFirst].dir;
    return o_DirLookup(quality, set, index);
}

float* __fastcall hk_SampleDir(float* out, int64_t quality, int64_t set, int64_t index, const float* m)
{
    Tls& t = t_tls;
    if (t.guided && t.g && uint32_t(quality) == t.g->quality && uint32_t(set) == 0 && uint32_t(index) >= t.gFirst && uint32_t(index) < t.g->rays)
    {
        const float* d = t.samples[uint32_t(index) - t.gFirst].dir;
        out[0] = d[0] * m[0] + d[1] * m[3] + d[2] * m[6];
        out[1] = d[0] * m[1] + d[1] * m[4] + d[2] * m[7];
        out[2] = d[0] * m[2] + d[1] * m[5] + d[2] * m[8];
        return out;
    }
    return o_SampleDir(out, quality, set, index, m);
}

int64_t __fastcall hk_AccHit(int64_t acc, const float* dir, const float* rgb, const float* basisA, const float* basisB, float weight)
{
    Tls& t = t_tls;
    if (t.g)
    {
        const int64_t i = RayIndex(t, basisA);
        if (i >= 0 && t.learning)
        {
            if (uint32_t(i) < t.g->rays) t.lum[size_t(i)] = std::max(0.0f, float(Luma(rgb)));
            if (t.captureStockB)
            {
                const bool b = uint32_t(i) >= t.g->half;
                const double x = double(weight) * Luma(rgb);
                double* s1 = b ? t.stockBSum : t.stockASum; double* s2 = b ? t.stockBSum2 : t.stockASum2;
                for (int f = 0; f < 6; ++f) { const double y = x * basisA[f]; s1[f] += y; s2[f] += y * y; }
                ++(b ? t.stockBN : t.stockAN);
            }
        }
        else if (i >= 0 && t.guided)
        {
            const unsigned batch = uint32_t(i) >= t.g->half ? 1 : 0;
            float w = weight;
            if (uint32_t(i) >= t.gFirst)
            {
                const GuidedSample& s = t.samples[uint32_t(i) - t.gFirst];
                w *= s.weight; basisA = s.basisA; basisB = s.basisB;
            }
            const double x = double(w) * Luma(rgb);
            t.sum[batch] += x; t.sum2[batch] += x * x; t.raw[batch] += Luma(rgb); ++t.n[batch];
            const float* th = reinterpret_cast<float*>(acc + 392);
            if (rgb[0] > th[0] || rgb[1] > th[1] || rgb[2] > th[2]) ++t.bright[batch];
            for (int f = 0; f < 6; ++f) { const double y = x * basisA[f]; t.faceSum[batch][f] += y; t.faceSum2[batch][f] += y * y; }
            return o_AccHit(acc, dir, rgb, basisA, basisB, w);
        }
    }
    return o_AccHit(acc, dir, rgb, basisA, basisB, weight);
}

int64_t __fastcall hk_AccMiss(int64_t acc, const float* dir, const float* basisA, const float* basisB, float weight)
{
    Tls& t = t_tls;
    if (t.g)
    {
        const int64_t i = RayIndex(t, basisA);
        if (i >= 0 && t.learning) { if (uint32_t(i) < t.g->rays) t.lum[size_t(i)] = -1.0f; if (t.captureStockB) ++(uint32_t(i) >= t.g->half ? t.stockBN : t.stockAN); }
        else if (i >= 0 && t.guided)
        {
            const unsigned batch = uint32_t(i) >= t.g->half ? 1 : 0;
            ++t.n[batch];
            if (uint32_t(i) >= t.gFirst)
            {
                const GuidedSample& s = t.samples[uint32_t(i) - t.gFirst];
                return o_AccMiss(acc, dir, s.basisA, s.basisB, weight * s.weight);
            }
        }
    }
    return o_AccMiss(acc, dir, basisA, basisB, weight);
}

double CubeLuma(const float* cube) { double s = 0; for (int f = 0; f < 6; ++f) s += Luma(cube + 3 * f); return s; }

float __fastcall hk_Merge(float* acc1, int64_t acc2)
{
    Tls& t = t_tls;
    ++g_stats.mergeCalls;
    if (t.learning && t.captureStockB && acc1 && acc2)
    {
        // local mode dry run: keep the stock second half's finalized cube and weight
        std::memcpy(t.stockB, reinterpret_cast<float*>(acc2 + off_Acc_Cube), sizeof t.stockB);
        t.stockBW = *reinterpret_cast<float*>(acc2 + off_Acc_Weight);
        t.haveStockB = t.stockBW > 0 && t.stockBN > 1;
        std::memcpy(t.stockA, acc1 + off_Acc_Cube / 4, sizeof t.stockA);
        t.stockAW = acc1[off_Acc_Weight / 4];
        t.haveStockA = t.stockAW > 0 && t.stockAN > 1;
        return o_Merge(acc1, acc2);
    }
    if (!t.guided || !t.g || !acc1 || !acc2) return o_Merge(acc1, acc2);
    ++g_stats.mergeGuided;
    const float W1 = acc1[off_Acc_Weight / 4], W2 = *reinterpret_cast<float*>(acc2 + off_Acc_Weight);
    const bool enough = *reinterpret_cast<uint32_t*>(acc2 + off_Acc_Count) && t.n[0] > 1 && t.n[1] > 1 && W1 > 0 && W2 > 0;
    if (!enough) { ++g_stats.mergeFewSamples; return o_Merge(acc1, acc2); }
    // Batches: 0 stock first half (acc1), 1 guided second half (acc2), 2 stock second half from the dry run (local mode).
    // 3 = stock first half from the dry run (full split only, where acc1 holds the guided first half).
    const int batches = t.haveStockB ? (t.gFirst == 0 && t.haveStockA ? 4 : 3) : 2;
    float cubes[4][18];
    std::memcpy(cubes[0], acc1 + off_Acc_Cube / 4, 72);
    std::memcpy(cubes[1], reinterpret_cast<float*>(acc2 + off_Acc_Cube), 72);
    if (batches >= 3) std::memcpy(cubes[2], t.stockB, 72);
    if (batches == 4) std::memcpy(cubes[3], t.stockA, 72);
    const double W[4] = {W1, W2, t.stockBW, t.stockAW};
    const float diff = o_Merge(acc1, acc2);        // Radiant's own blend of the halves plus its convergence measure
    float* out = acc1 + off_Acc_Cube / 4;
    double lEq = 0, lOut = 0;
    for (int f = 0; f < 6; ++f)
    {
        double wf[4]{};
        if (g_set.combine == Combine::Variance)
        {
            // Graybill-Deal per cube face: weights by the inverse variance of each batch's projected contributions (w * L * basis_f).
            double v[4]{};
            for (int b = 0; b < batches; ++b)
            {
                const double n = b < 2 ? t.n[b] : b == 2 ? t.stockBN : t.stockAN;
                const double s1 = b < 2 ? t.faceSum[b][f] : b == 2 ? t.stockBSum[f] : t.stockASum[f];
                const double s2 = b < 2 ? t.faceSum2[b][f] : b == 2 ? t.stockBSum2[f] : t.stockASum2[f];
                const double m = s1 / n;
                v[b] = std::max(0.0, (s2 - n * m * m) / (n - 1)) / n;
            }
            double vmax = 0; for (int b = 0; b < batches; ++b) vmax = std::max(vmax, v[b]);
            const double floorV = std::max(1e-30, 1e-4 * vmax);
            for (int b = 0; b < batches; ++b) wf[b] = 1 / std::max(v[b], floorV);
        }
        else for (int b = 0; b < batches; ++b) wf[b] = W[b];
        double sw = 0, se = 0; for (int b = 0; b < batches; ++b) { sw += wf[b]; se += W[b]; }
        float blended[3]{}, eq[3]{};
        for (int k = 0; k < 3; ++k)
        {
            double x = 0, y = 0;
            for (int b = 0; b < batches; ++b) { x += wf[b] * cubes[b][3 * f + k]; y += W[b] * cubes[b][3 * f + k]; }
            blended[k] = float(x / sw); eq[k] = float(y / se);
        }
        lEq += Luma(eq); lOut += Luma(blended);
        std::memcpy(out + 3 * f, blended, sizeof blended);
    }
    if (g_set.combine == Combine::Variance)
    {
        std::lock_guard<std::mutex> lock(t.g->sumMutex);
        t.g->sumEqual += lEq; t.g->sumVariance += lOut;
        t.g->gdVoxel[t.voxel] = 1;
        ++g_stats.gdVoxels;
    }
    return diff;
}

int64_t __fastcall hk_Adaptive(int64_t* ctx, int64_t voxel)
{
    ++g_stats.adaptiveCalls;
    return o_Adaptive(ctx, voxel);
}

bool Hook(uintptr_t ida, void* detour, void* original, const char* name)
{
    void* target = Addr(ida);
    if (!target || !g_env.detour || !g_env.detour(target, detour, static_cast<void**>(original)))
    { Log("bake: hook %s at %p failed", name, target); return false; }
    return true;
}

// The gather-level hooks: adaptive override, cone correction, dumps. Cheap (per probe job and per voxel).
bool InstallCoreHooks()
{
    if (g_coreHooked) return true;
    bool ok = Hook(kVoxelFinal, reinterpret_cast<void*>(&hk_VoxelFinal), &o_VoxelFinal, "VoxelFinal") &&
              Hook(kAdaptive, reinterpret_cast<void*>(&hk_Adaptive), &o_Adaptive, "Adaptive") &&
              Hook(kGatherVoxel, reinterpret_cast<void*>(&hk_GatherVoxel), &o_GatherVoxel, "GatherVoxel") &&
              Hook(kProbeGather, reinterpret_cast<void*>(&hk_ProbeGather), &o_ProbeGather, "ProbeGather");
    if (ok && g_debugVoxelOn && Hook(kCaptureBegin, reinterpret_cast<void*>(&hk_CaptureBegin), &o_CaptureBegin, "CaptureBegin"))
        Log("bake: debug voxel test %u %u %u (cone_debug_skip=%d)", g_debugVoxel[0], g_debugVoxel[1], g_debugVoxel[2], g_coneDebugSkip ? 1 : 0);
    if (ok && !Hook(kSmoothTask, reinterpret_cast<void*>(&hk_SmoothTask), &o_SmoothTask, "SmoothTask"))
        Log("bake: post-smooth dump unavailable (hook failed)");
    g_coreHooked = ok;
    Log("bake: gather hooks %s", ok ? "installed" : "incomplete (adaptive override and cone correction off)");
    return ok;
}

// Ray guiding adds per-ray hooks (the sampler and accumulators run for every ray), so they are only installed for it.
bool InstallHooks()
{
    if (g_hooked) return true;
    g_trace32 = reinterpret_cast<Trace32Fn>(Addr(kTrace32));
    g_basisA = reinterpret_cast<BasisFn>(Addr(kBasisA));
    g_basisB = reinterpret_cast<BasisFn>(Addr(kBasisB));
    g_uniformDirs = static_cast<const uint64_t*>(Addr(kUniformDirs));
    g_basisTableA = static_cast<const uint64_t*>(Addr(kBasisTableA));
    g_basisTableB = static_cast<const uint64_t*>(Addr(kBasisTableB));
    if (!g_trace32 || !g_basisA || !g_basisB || !g_uniformDirs || !g_basisTableA || !g_basisTableB)
    { Log("bake guide: addresses missing, ray guiding unavailable"); return false; }
    // The per-sample hooks only act inside a guided or learning voxel (thread-local state), elsewhere they pass through.
    bool ok = Hook(kVoxelTrace, reinterpret_cast<void*>(&hk_VoxelTrace), &o_VoxelTrace, "VoxelTrace") &&
              Hook(kDirLookup, reinterpret_cast<void*>(&hk_DirLookup), &o_DirLookup, "DirLookup") &&
              Hook(kSampleDir, reinterpret_cast<void*>(&hk_SampleDir), &o_SampleDir, "SampleDir") &&
              Hook(kAccHit, reinterpret_cast<void*>(&hk_AccHit), &o_AccHit, "AccHit") &&
              Hook(kAccMiss, reinterpret_cast<void*>(&hk_AccMiss), &o_AccMiss, "AccMiss") &&
              Hook(kMerge, reinterpret_cast<void*>(&hk_Merge), &o_Merge, "Merge") &&
              Hook(kHitFixup, reinterpret_cast<void*>(&hk_HitFixup), &o_HitFixup, "HitFixup") &&
              InstallCoreHooks();
    g_hooked = ok;
    Log("bake guide: hooks %s", ok ? "installed" : "incomplete (ray guiding off)");
    return ok;
}

// ---- dump and commands ----------------------------------------------------------------------------------------------
// Binary dump for offline error analysis: "RGD1", probe count, then per probe: id, dims[3], matrix[9], origin[3],
// flags (uint64 per voxel), cube (18 floats per voxel).
std::string DumpProbes(const std::string& path)
{
    uint8_t* cam = g_camWnd ? g_camWnd() : nullptr;
    if (!cam) return "no cam window";
    uint8_t* pb = *reinterpret_cast<uint8_t**>(cam + 6664), *pe = *reinterpret_cast<uint8_t**>(cam + 6672);
    if (!pb || pe < pb) return "no probes";
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") || !f) return "cannot open " + path;
    const uint32_t n = uint32_t((pe - pb) / 112);
    fwrite("RGD1", 1, 4, f); fwrite(&n, 4, 1, f);
    uint32_t written = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint8_t* probe = *reinterpret_cast<uint8_t**>(pb + 112 * i + 40);
        int32_t hdr[4]{-1, 0, 0, 0};
        float m[12]{};
        if (!probe) { fwrite(hdr, 4, 4, f); fwrite(m, 4, 12, f); continue; }
        hdr[0] = *reinterpret_cast<int*>(probe + off_Probe_Id);
        std::memcpy(hdr + 1, probe + off_Probe_Dims, 12);
        std::memcpy(m, probe + off_Probe_Matrix, 36);
        std::memcpy(m + 9, probe, 12);
        const uint64_t voxels = uint64_t(std::max(0, hdr[1])) * std::max(0, hdr[2]) * std::max(0, hdr[3]);
        const uint64_t* flags = *reinterpret_cast<uint64_t**>(probe + off_Probe_Flags);
        const float* cube = *reinterpret_cast<float**>(probe + off_Probe_Coeffs);
        if (!flags || !cube || voxels > 64'000'000) { hdr[1] = hdr[2] = hdr[3] = 0; fwrite(hdr, 4, 4, f); fwrite(m, 4, 12, f); continue; }
        fwrite(hdr, 4, 4, f); fwrite(m, 4, 12, f);
        fwrite(flags, 8, size_t(voxels), f); fwrite(cube, 4, size_t(18 * voxels), f);
        ++written;
    }
    fclose(f);
    return "dumped " + std::to_string(written) + " of " + std::to_string(n) + " probes to " + path;
}

std::string Status()
{
    char buf[2048];
    snprintf(buf, sizeof buf,
             "ray guiding %s (hooks %s), mode=local split=%s bounces=all target=%s decorrelate=%d combine=%s smoothing=%.2f uniform=%.3f observe=%d mip_guided=%d mip_stock=%d; last bake: %llu guides, %llu learned samples, "
             "%llu guided voxels (%llu inverse variance), %llu re-traced rays, %llu refused probes, %llu prepare failures, %llu adaptive calls; "
             "final bounce %.1f s (summed over probes); merges %llu, in guided voxels %llu, too few samples %llu, no weight %llu; autodump %s tag %s",
             g_set.enabled ? "on" : "off", g_hooked ? "installed" : "not installed", g_set.fullSplit ? "full" : "half", g_set.target == Target::Faces ? "faces" : "luminance", g_set.decorrelate ? 1 : 0, g_set.combine == Combine::Variance ? "variance" : "equal",
             g_set.smoothing, g_set.uniform, g_set.observe ? 1 : 0, g_set.guidedMipDelta, g_set.stockMipDelta,
             (unsigned long long)g_stats.guides, (unsigned long long)g_stats.learnedSamples.load(), (unsigned long long)g_stats.guidedVoxels,
             (unsigned long long)g_stats.gdVoxels, (unsigned long long)g_stats.retracedRays, (unsigned long long)g_stats.refusedProbes,
             (unsigned long long)g_stats.prepareFailures, (unsigned long long)g_stats.adaptiveCalls,
             g_stats.finalMs / 1000.0,
             (unsigned long long)g_stats.mergeCalls, (unsigned long long)g_stats.mergeGuided, (unsigned long long)g_stats.mergeFewSamples,
             (unsigned long long)g_stats.mergeNoWeight, g_dumpDir.empty() ? "(off)" : g_dumpDir.c_str(), g_dumpTag.c_str());
    return buf;
}

const char* const kAdaptiveNames[4] = {"map", "low", "medium", "high"};
int ParseAdaptive(const std::string& v)
{
    for (int i = 0; i < 4; ++i) if (v == kAdaptiveNames[i]) return i;
    if (v == "off" || v == "0") return 0;
    return -1;
}

std::string CoreStatus()
{
    char buf[768];
    const uint64_t n = g_stats.coneRatioCount;
    snprintf(buf, sizeof buf,
             "adaptive=%s cone=%d (stride %d, radius %d, edge class %d), gather hooks %s; last bake: %llu probe jobs raised to the adaptive level, "
             "%llu adaptive voxel passes, %llu sharp gathers, %llu cone samples, %llu voxels corrected, %llu jobs skipped, "
             "final-bounce mean ratio %.3f over %llu probes; gather jobs %.2f s (final bounce %.2f s), summed over probes",
             kAdaptiveNames[std::clamp(g_set.adaptive.load(), 0, 3)], g_set.cone ? 1 : 0, g_set.coneStride, g_set.coneRadius,
             g_set.coneEdge ? 1 : 0, g_coreHooked ? "installed" : "not installed", (unsigned long long)g_stats.adaptiveRaised, (unsigned long long)g_stats.adaptiveCalls,
             (unsigned long long)g_stats.coneGathers, (unsigned long long)g_stats.coneSamples, (unsigned long long)g_stats.coneScaled,
             (unsigned long long)g_stats.coneSkipped, n ? double(g_stats.coneRatioMilli) / 1000.0 / double(n) : 1.0, (unsigned long long)n,
             g_stats.gatherMs / 1000.0, g_stats.finalMs / 1000.0);
    std::string out = buf;
    if (g_debugVoxelOn)
        out += "; debug voxel test: " + std::to_string(g_debugCaptures.load()) + " captures, " +
               std::to_string(g_debugSkippedTotal.load()) + " sharp gathers left out";
    return out;
}

bool Baking() { return g_running && g_running(); }
}

std::string Summary() { return CoreStatus(); }

bool SetRenderOption(int index, bool on)
{
    auto set = reinterpret_cast<SetRenderOptFn>(Addr(kSetRenderOpt));
    auto get = reinterpret_cast<GetRenderOptFn>(Addr(kGetRenderOpt));
    if (!set || !get) return false;
    set(index, on ? 1 : 0);
    return get(index) == on;
}

void ApplyRenderOptions()
{
    if (!CfgBool("free_space_skip", true))
        Log("bake: free-space skip %s (every open voxel is traced)", SetRenderOption(kOptDisableFreeSpace, true) ? "off" : "could not be turned off");
    if (!CfgBool("post_smooth", true))
        Log("bake: probe post-smooth %s", SetRenderOption(kOptDisablePostSmooth, true) ? "off" : "could not be turned off");
}

void Setup(const Env& env)
{
    g_env = env;
    g_camWnd = env.camWnd; g_running = env.running;
    g_set.adaptive = std::max(0, ParseAdaptive(Cfg("adaptive", "map")));
    g_set.cone = CfgBool("cone", true);
    g_set.coneStride = std::clamp(atoi(Cfg("cone_stride", "2").c_str()), 1, 4);
    g_set.coneRadius = std::clamp(atoi(Cfg("cone_radius", "6").c_str()), 1, 16);
    g_set.coneEdge = CfgBool("cone_edge", true);
    g_coneDebugSkip = CfgBool("cone_debug_skip", true);
    g_draftAdaptive = CfgBool("low_rays_adaptive", false);
    g_set.stockMipDelta = std::clamp(atoi(Cfg("mip_stock", "0").c_str()), -4, 4);   // test key: every hit's mip shifted
    {
        const std::string dv = Cfg("debug_voxel", "");
        unsigned x, y, z;
        if (!dv.empty() && sscanf_s(dv.c_str(), "%u,%u,%u", &x, &y, &z) == 3)
        { g_debugVoxel[0] = x; g_debugVoxel[1] = y; g_debugVoxel[2] = z; g_debugVoxelOn = true; }
    }
    g_set.enabled = CfgBool("guide", false);
    g_set.observe = CfgBool("guide_observe", false);
    g_set.combine = Cfg("guide_combine", "variance") == "equal" ? Combine::Equal : Combine::Variance;
    g_set.target = Cfg("guide_target", "faces") == "luminance" ? Target::Luminance : Target::Faces;
    g_set.decorrelate = CfgBool("guide_decorrelate", false);
    g_set.fullSplit = Cfg("guide_split", "half") == "full";
    g_set.smoothing = std::clamp(atof(Cfg("guide_smoothing", "0.25").c_str()), 0.0, 1.0);
    g_set.uniform = std::clamp(atof(Cfg("guide_uniform", "0.05").c_str()), 1e-6, 1.0);
    // Diagnostics for unattended (launcher) bakes: dump each probe's final bounce to dump_dir as <dump_tag>_probe*.rgp.
    g_dumpDir = Cfg("dump_dir", ""); g_dumpTag = Cfg("dump_tag", "run");
    if (g_set.enabled) InstallHooks();
    else if (g_set.adaptive || g_set.cone || g_set.observe || !g_dumpDir.empty() || bakell::Active() || bakell::Switchable()) InstallCoreHooks();
    Log("bake: %s", CoreStatus().c_str());
    Log("bake guide: %s", Status().c_str());
}

void Shutdown()
{
    g_set.enabled = false;                          // the host removes the detours
    AcquireSRWLockExclusive(&g_guideLock); g_guides.clear(); ReleaseSRWLockExclusive(&g_guideLock);
}

bool Command(const std::string& c, std::string& reply)
{
    if (c == "bake adaptive" || c == "bake cone") { reply = CoreStatus(); return true; }
    if (c.rfind("bake adaptive ", 0) == 0)
    {
        const int level = ParseAdaptive(c.substr(14));
        if (level < 0) { reply = "bake adaptive map|low|medium|high"; return true; }
        if (Baking()) { reply = "a bake is running"; return true; }
        if (level && !InstallCoreHooks()) { reply = "gather hooks could not be installed (see log)"; return true; }
        g_set.adaptive = level; CfgSet("adaptive", kAdaptiveNames[level]);
        reply = CoreStatus(); return true;
    }
    if (c == "bake cone 0" || c == "bake cone 1")
    {
        if (Baking()) { reply = "a bake is running"; return true; }
        const bool on = c.back() == '1';
        if (on && !InstallCoreHooks()) { reply = "gather hooks could not be installed (see log)"; return true; }
        g_set.cone = on; CfgSet("cone", on ? "1" : "0");
        reply = CoreStatus(); return true;
    }
    if (c.rfind("bake option ", 0) == 0)
    {
        // Radiant's own render options (the View > Debug toggles, without their menu check marks): for reference bakes.
        const std::string arg = c.substr(12);
        const bool freeSpace = arg.rfind("freespace ", 0) == 0, smooth = arg.rfind("postsmooth ", 0) == 0;
        if ((!freeSpace && !smooth) || (arg.back() != '0' && arg.back() != '1')) { reply = "bake option freespace|postsmooth 0|1"; return true; }
        if (Baking()) { reply = "a bake is running"; return true; }
        const bool on = arg.back() == '1';
        const bool ok = SetRenderOption(freeSpace ? kOptDisableFreeSpace : kOptDisablePostSmooth, !on);
        reply = std::string(freeSpace ? "free-space skip " : "post-smooth ") + (on ? "on" : "off") + (ok ? "" : " (failed)");
        return true;
    }
    if (c == "bake cone edge 0" || c == "bake cone edge 1")
    {
        if (Baking()) { reply = "a bake is running"; return true; }
        g_set.coneEdge = c.back() == '1'; CfgSet("cone_edge", g_set.coneEdge ? "1" : "0");
        reply = CoreStatus(); return true;
    }
    if (c.rfind("bake cone radius ", 0) == 0 || c.rfind("bake cone stride ", 0) == 0)
    {
        if (Baking()) { reply = "a bake is running"; return true; }
        const bool radius = c[10] == 'r';
        const int v = atoi(c.substr(17).c_str());
        if (radius) { g_set.coneRadius = std::clamp(v, 1, 16); CfgSet("cone_radius", std::to_string(g_set.coneRadius)); }
        else { g_set.coneStride = std::clamp(v, 1, 4); CfgSet("cone_stride", std::to_string(g_set.coneStride)); }
        reply = CoreStatus(); return true;
    }
    if (c.rfind("bake guide", 0) != 0) return false;
    const std::string arg = c.size() > 11 ? c.substr(11) : "";
    if (arg.empty() || arg == "status") { reply = Status(); return true; }
    if (arg == "mip guided 0" || arg == "mip guided -1" || arg == "mip guided -2" ||
        arg == "mip stock 0" || arg == "mip stock -1" || arg == "mip stock -2")
    {
        if (Baking()) { reply = "a bake is running"; return true; }
        const bool guided = arg.rfind("mip guided", 0) == 0;
        (guided ? g_set.guidedMipDelta : g_set.stockMipDelta) = atoi(arg.substr(guided ? 11 : 10).c_str());
        reply = arg + " (runtime cone proxy; ray count unchanged)"; return true;
    }
    if (arg == "0" || arg == "1" || arg == "observe 0" || arg == "observe 1")
    {
        const bool value = arg.back() == '1';
        if (Baking()) { reply = "a bake is running"; return true; }
        const bool observe = arg.rfind("observe", 0) == 0;
        if (value && !(observe ? g_coreHooked : g_hooked) && !(observe ? InstallCoreHooks() : InstallHooks()))
        { reply = "hooks could not be installed (see log)"; return true; }
        if (observe) { g_set.observe = value; CfgSet("guide_observe", value ? "1" : "0"); }
        else { g_set.enabled = value; CfgSet("guide", value ? "1" : "0"); }
        reply = Status(); return true;
    }
    if (arg == "combine equal" || arg == "combine variance")
    {
        if (Baking()) { reply = "a bake is running"; return true; }
        g_set.combine = arg == "combine equal" ? Combine::Equal : Combine::Variance;
        CfgSet("guide_combine", g_set.combine == Combine::Equal ? "equal" : "variance");
        reply = Status(); return true;
    }
    if (arg == "split half" || arg == "split full")
    {
        if (Baking()) { reply = "a bake is running"; return true; }
        g_set.fullSplit = arg == "split full";
        CfgSet("guide_split", g_set.fullSplit ? "full" : "half");
        reply = Status(); return true;
    }
    if (arg == "mode local" || arg == "bounces all") { reply = Status(); return true; }
    if (arg == "mode cluster" || arg == "bounces final" || arg.rfind("cluster ", 0) == 0)
    { reply = "only mode local with all bounces is supported"; return true; }
    if (arg == "decorrelate 0" || arg == "decorrelate 1")
    {
        if (Baking()) { reply = "a bake is running"; return true; }
        g_set.decorrelate = arg.back() == '1';
        CfgSet("guide_decorrelate", g_set.decorrelate ? "1" : "0");
        reply = Status(); return true;
    }
    if (arg == "target faces" || arg == "target luminance")
    {
        if (Baking()) { reply = "a bake is running"; return true; }
        g_set.target = arg == "target faces" ? Target::Faces : Target::Luminance;
        CfgSet("guide_target", g_set.target == Target::Faces ? "faces" : "luminance");
        reply = Status(); return true;
    }
    if (arg.rfind("smoothing ", 0) == 0 || arg.rfind("uniform ", 0) == 0)
    {
        if (Baking()) { reply = "a bake is running"; return true; }
        const bool smoothing = arg[0] == 's';
        const double v = atof(arg.substr(smoothing ? 10 : 8).c_str());
        if (smoothing) { g_set.smoothing = std::clamp(v, 0.0, 1.0); CfgSet("guide_smoothing", std::to_string(g_set.smoothing)); }
        else { g_set.uniform = std::clamp(v, 1e-6, 1.0); CfgSet("guide_uniform", std::to_string(g_set.uniform)); }
        reply = Status(); return true;
    }
    if (arg == "reset") { g_stats.Reset(); reply = Status(); return true; }
    if (arg.rfind("autodump", 0) == 0)
    {
        std::lock_guard<std::mutex> lock(g_dumpMutex);
        g_dumpDir = arg.size() > 9 ? arg.substr(9) : "";
        if (!g_dumpDir.empty() && !InstallCoreHooks()) { g_dumpDir.clear(); reply = "gather hooks could not be installed (see log)"; return true; }
        reply = "autodump " + (g_dumpDir.empty() ? std::string("off") : g_dumpDir); return true;
    }
    if (arg == "dumpall 0" || arg == "dumpall 1") { g_dumpAll = arg.back() == '1'; reply = arg; return true; }
    if (arg.rfind("tag ", 0) == 0) { std::lock_guard<std::mutex> lock(g_dumpMutex); g_dumpTag = arg.substr(4); reply = "tag " + g_dumpTag; return true; }
    if (arg.rfind("dump ", 0) == 0) { reply = DumpProbes(arg.substr(5)); return true; }
    reply = "bake guide [status|0|1|observe 0|observe 1|combine equal|combine variance|smoothing <a>|uniform <f>|reset|dump <path>|autodump <dir>|tag <name>]";
    return true;
}
}
