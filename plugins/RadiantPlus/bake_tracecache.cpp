// Traced-hit cache (see bake_tracecache.h). Trace job 0x1411341D0, pass 0x1411237B0 and SceneObj (0x1411A0F70) are in
// The pass writes +1504 hit store, +1552/+1576 hits,
// +1648 voxel flags, +1672 reference radiance, +1696 positions, +1720 AO (6 per voxel, not grids), +1752 cube, +1784 quality.
#include "bake_tracecache.h"
#include "bake_lowrays.h"
#include <windows.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace br::baketrace
{
namespace
{
const uintptr_t kTracePass   = 0x1411237B0;   // void(probe*, SceneObj** scene, int quality, char grid, int* debugVoxel, char freeSpace)
const uintptr_t kEmbreeLoad  = 0x1411A19C0;   // loads (avx\\)embree64r.dll
const uintptr_t kRayParams   = 0x141121E30;   // void(probe*): +10176..+10255 from the probe's quality and bounds
const uintptr_t kStoreClear  = 0x1411202C0;   // void(store*): frees the blocks and the unpacked +1552/+1576
const uintptr_t kBlockResize = 0x141165750;   // vector<Block>::resize(vec*, n), value-initialised
const uintptr_t kMalloc      = 0x140B894B0;   // Radiant's malloc / free wrappers (std::vector storage)
const uintptr_t kFree        = 0x140B894F0;
const uintptr_t kBlockMalloc = 0x140B89520;   // block data (the store clear frees it with 0x140B89560)
const uintptr_t kLz4         = 0x140E95120;   // int(src, dst, size): LZ4_compress, dst >= size + size/255 + 16
const uintptr_t kUnLz4       = 0x140E920C0;   // int(src, dst, compressed, maxOut): LZ4_decompress_safe

const size_t off_ProbeIndex = 388, off_ExtraScene = 1048, off_Slot = 1056, off_Store = 1504, off_Voxels = 1632,
             off_Rays = 1636, off_StoreEmpty = 1640, off_Flags = 1648, off_Ref = 1672, off_Pos = 1696, off_AO = 1720,
             off_Cube = 1752, off_Quality = 1784, off_Dims = 96;
const size_t kProbeBytes = 10256;   // through the clip plane vector at +10232

br::bakeguide::Env g_env{};
std::wstring g_dir;
std::atomic<int> g_mode{0};   // 0 off, 1 on, 2 verify, 3 diag
std::atomic<uint64_t> g_passes{0}, g_hits{0}, g_misses{0}, g_saved{0}, g_verified{0}, g_mismatches{0}, g_bypassed{0}, g_rejected{0},
    g_restoreFailed{0}, g_lowSkips{0};
uint64_t g_capMB = 4096;

// Low-memory guard, same floor as the fill caches: a snapshot or a save payload is hundreds of MB per in-flight job, and
// a worker's bad_alloc kills the bake (it comes back out of UpdateGI uncaught). Checked at most every 500 ms.
uint64_t g_floorBytes = uint64_t(3) << 30;
std::atomic<uint64_t> g_memTick{0};
std::atomic<bool> g_memLow{false};
bool MemoryLow()
{
    const uint64_t now = GetTickCount64();
    if (now - g_memTick.load(std::memory_order_relaxed) >= 500)
    {
        g_memTick.store(now, std::memory_order_relaxed);
        MEMORYSTATUSEX m{}; m.dwLength = sizeof m;
        if (GlobalMemoryStatusEx(&m)) g_memLow = m.ullAvailPageFile < g_floorBytes;
    }
    return g_memLow.load(std::memory_order_relaxed);
}

void Log(const char* fmt, ...)
{
    if (!g_env.log) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    g_env.log(buf);
}

template <class T> T At(const void* base, size_t off) { return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(base) + off); }

// ---- hashing ----
inline uint64_t Rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
inline uint64_t Fin(uint64_t h)
{
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL; h ^= h >> 33;
    return h;
}
inline uint64_t Mix(uint64_t h, uint64_t v) { return Fin(h ^ (v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2))); }
uint64_t Hash(const void* data, size_t n, uint64_t seed = 0)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    const uint64_t K1 = 0x9E3779B185EBCA87ULL, K2 = 0xC2B2AE3D27D4EB4FULL;
    uint64_t a = seed ^ K1, b = seed + K2, c = Rotl(seed, 17) ^ K2, d = seed - K1;
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
    {
        uint64_t w[4];
        std::memcpy(w, p + i, 32);
        a = Rotl(a + w[0] * K2, 31) * K1; b = Rotl(b + w[1] * K2, 31) * K1;
        c = Rotl(c + w[2] * K2, 31) * K1; d = Rotl(d + w[3] * K2, 31) * K1;
    }
    uint64_t h = Rotl(a, 1) + Rotl(b, 7) + Rotl(c, 12) + Rotl(d, 18) + n;
    for (; i + 8 <= n; i += 8) { uint64_t w; std::memcpy(&w, p + i, 8); h = Rotl(h ^ (w * K2), 27) * K1 + 0x52dce729; }
    for (; i < n; ++i) h = Rotl(h ^ (p[i] * K1), 11) * K2;
    return Fin(h);
}
uint64_t HashVec(const uint8_t* probe, size_t off, uint64_t seed = 0)
{
    const uint8_t* b = At<const uint8_t*>(probe, off);
    const uint8_t* e = At<const uint8_t*>(probe, off + 8);
    if (!b || e <= b) return Mix(seed, 0);
    return Hash(b, size_t(e - b), seed);
}

// ---- Embree scene fingerprints ----
enum : int { kIndexBuffer = 0x01000000, kVertexBuffer = 0x02000000 };
struct Geom
{
    size_t tris = 0, verts = 0;
    void* mapIdx = nullptr;
    void* mapVtx = nullptr;
    uint64_t idx = 0, vtx = 0, xfm = 0;
    void* source = nullptr;   // instanced RTCScene
    bool instance = false;
};
std::mutex g_sceneLock;
std::unordered_map<void*, std::vector<Geom>> g_scenes;   // RTCScene -> geometries by geomID
std::unordered_map<void*, uint64_t> g_fpMemo;           // RTCScene -> fingerprint of its geometry (without records)

using NewSceneFn = void*(__cdecl*)(int, int);
using DeleteSceneFn = void(__cdecl*)(void*);
using NewTriMeshFn = unsigned(__cdecl*)(void*, int, size_t, size_t, size_t);
using MapBufferFn = void*(__cdecl*)(void*, unsigned, int);
using UnmapBufferFn = void(__cdecl*)(void*, unsigned, int);
using NewInstanceFn = unsigned(__cdecl*)(void*, void*);
using SetTransformFn = void(__cdecl*)(void*, unsigned, int, const float*);
NewSceneFn o_NewScene = nullptr;
DeleteSceneFn o_DeleteScene = nullptr;
NewTriMeshFn o_NewTriMesh = nullptr;
MapBufferFn o_MapBuffer = nullptr;
UnmapBufferFn o_UnmapBuffer = nullptr;
NewInstanceFn o_NewInstance = nullptr;
SetTransformFn o_SetTransform = nullptr;
std::atomic<bool> g_embreeHooked{false};

Geom& GeomAt(void* scene, unsigned id)   // g_sceneLock held
{
    std::vector<Geom>& v = g_scenes[scene];
    if (v.size() <= id) v.resize(size_t(id) + 1);
    g_fpMemo.erase(scene);
    return v[id];
}

void* __cdecl hk_NewScene(int flags, int aflags)
{
    void* s = o_NewScene(flags, aflags);
    std::lock_guard<std::mutex> lk(g_sceneLock);
    g_scenes[s].clear();
    g_fpMemo.erase(s);
    return s;
}
void __cdecl hk_DeleteScene(void* s)
{
    {
        std::lock_guard<std::mutex> lk(g_sceneLock);
        g_scenes.erase(s);
        g_fpMemo.erase(s);
    }
    o_DeleteScene(s);
}
unsigned __cdecl hk_NewTriMesh(void* s, int flags, size_t tris, size_t verts, size_t steps)
{
    const unsigned id = o_NewTriMesh(s, flags, tris, verts, steps);
    std::lock_guard<std::mutex> lk(g_sceneLock);
    Geom& g = GeomAt(s, id);
    g = Geom{};
    g.tris = tris; g.verts = verts;
    return id;
}
void* __cdecl hk_MapBuffer(void* s, unsigned id, int type)
{
    void* p = o_MapBuffer(s, id, type);
    std::lock_guard<std::mutex> lk(g_sceneLock);
    Geom& g = GeomAt(s, id);
    (type == kIndexBuffer ? g.mapIdx : g.mapVtx) = p;
    return p;
}
void __cdecl hk_UnmapBuffer(void* s, unsigned id, int type)
{
    void* p = nullptr;
    size_t bytes = 0;
    {
        std::lock_guard<std::mutex> lk(g_sceneLock);
        Geom& g = GeomAt(s, id);
        p = type == kIndexBuffer ? g.mapIdx : g.mapVtx;
        bytes = type == kIndexBuffer ? g.tris * 12 : g.verts * 16;
    }
    const uint64_t h = p && bytes ? Hash(p, bytes, uint64_t(type)) : Mix(uint64_t(type), 0);
    {
        std::lock_guard<std::mutex> lk(g_sceneLock);
        Geom& g = GeomAt(s, id);
        (type == kIndexBuffer ? g.idx : g.vtx) = h;
    }
    o_UnmapBuffer(s, id, type);
}
unsigned __cdecl hk_NewInstance(void* s, void* source)
{
    const unsigned id = o_NewInstance(s, source);
    std::lock_guard<std::mutex> lk(g_sceneLock);
    Geom& g = GeomAt(s, id);
    g = Geom{};
    g.instance = true; g.source = source;
    return id;
}
void __cdecl hk_SetTransform(void* s, unsigned id, int layout, const float* m)
{
    const size_t n = layout == 0 ? 12 : layout == 1 ? 12 : 16;
    const uint64_t h = m ? Hash(m, n * 4, 0x7F + uint64_t(layout)) : 0;
    {
        std::lock_guard<std::mutex> lk(g_sceneLock);
        GeomAt(s, id).xfm = h;
    }
    o_SetTransform(s, id, layout, m);
}

// Fingerprint of one RTCScene's geometry, order-independent over its geometries (g_sceneLock held). 0 = unknown scene.
uint64_t GeometryFp(void* scene, int depth = 0)
{
    if (!scene || depth > 4) return 0;
    auto memo = g_fpMemo.find(scene);
    if (memo != g_fpMemo.end()) return memo->second;
    auto it = g_scenes.find(scene);
    if (it == g_scenes.end()) return 0;
    uint64_t sum = 0, x = 0, count = 0;
    for (const Geom& g : it->second)
    {
        uint64_t h;
        if (g.instance) h = Mix(Mix(0x1157, GeometryFp(g.source, depth + 1)), g.xfm);
        else if (g.tris || g.verts) h = Mix(Mix(Mix(0x3E5, g.tris), g.verts), Mix(g.idx, g.vtx));
        else continue;
        sum += Fin(h); x ^= Fin(h * 0x9E3779B97F4A7C15ULL); ++count;
    }
    const uint64_t fp = Mix(Mix(sum, x), count);
    g_fpMemo[scene] = fp;
    return fp;
}

// Fingerprint of a SceneObj: its RTCScene's geometry plus the per-geometry records the tracer reads through the hit.
// Returns 0 when the scene was not seen being built (hooks installed late): such a probe is never cached.
uint64_t SceneFp(const uint8_t* obj)
{
    if (!obj) return 0x5EED;   // "no scene" is a valid, stable input (probes without an extra scene)
    void* rtc = At<void*>(obj, 0);
    std::lock_guard<std::mutex> lk(g_sceneLock);
    const uint64_t geo = GeometryFp(rtc);
    if (!geo) return 0;
    uint64_t h = geo;
    for (size_t off : {size_t(24), size_t(48)})
        h = Mix(h, HashVec(obj, off, off));
    return Mix(h, At<uint8_t>(obj, 16));
}

// ---- Embree rays for the guard ----
struct alignas(16) Ray4
{
    float orgx[4], orgy[4], orgz[4], dirx[4], diry[4], dirz[4], tnear[4], tfar[4], time[4];
    int mask[4];
    float Ngx[4], Ngy[4], Ngz[4], u[4], v[4];
    int geomID[4], primID[4], instID[4];
};
using Intersect4Fn = void(__cdecl*)(const void* valid, void* scene, Ray4& ray);
Intersect4Fn g_intersect4 = nullptr;

bool HookEmbree()
{
    if (g_embreeHooked) return true;
    HMODULE m = GetModuleHandleW(L"embree64r.dll");
    if (!m || !g_env.detour) return false;
    struct { const char* name; void* detour; void** original; } hooks[] = {
        {"rtcNewScene", reinterpret_cast<void*>(&hk_NewScene), reinterpret_cast<void**>(&o_NewScene)},
        {"rtcDeleteScene", reinterpret_cast<void*>(&hk_DeleteScene), reinterpret_cast<void**>(&o_DeleteScene)},
        {"rtcNewTriangleMesh", reinterpret_cast<void*>(&hk_NewTriMesh), reinterpret_cast<void**>(&o_NewTriMesh)},
        {"rtcMapBuffer", reinterpret_cast<void*>(&hk_MapBuffer), reinterpret_cast<void**>(&o_MapBuffer)},
        {"rtcUnmapBuffer", reinterpret_cast<void*>(&hk_UnmapBuffer), reinterpret_cast<void**>(&o_UnmapBuffer)},
        {"rtcNewInstance", reinterpret_cast<void*>(&hk_NewInstance), reinterpret_cast<void**>(&o_NewInstance)},
        {"rtcSetTransform", reinterpret_cast<void*>(&hk_SetTransform), reinterpret_cast<void**>(&o_SetTransform)},
    };
    for (auto& h : hooks)
    {
        void* target = reinterpret_cast<void*>(GetProcAddress(m, h.name));
        if (!target || !g_env.detour(target, h.detour, h.original))
        {
            Log("bake: trace cache unavailable (Embree hook %s failed)", h.name);
            return false;
        }
    }
    g_intersect4 = reinterpret_cast<Intersect4Fn>(GetProcAddress(m, "rtcIntersect4"));
    g_embreeHooked = true;
    Log("bake: trace cache: Embree scene hooks installed");
    return true;
}

using EmbreeLoadFn = int64_t(__fastcall*)();
EmbreeLoadFn o_EmbreeLoad = nullptr;
int64_t __fastcall hk_EmbreeLoad()
{
    const int64_t r = o_EmbreeLoad();
    try { HookEmbree(); } catch (...) {}
    return r;
}

// ---- probe snapshots (diagnostics) ----
bool SafeCopy(void* dst, const void* src, size_t n)
{
    __try { std::memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Hash of everything the pass is known to write.
uint64_t OutputHash(const uint8_t* probe, bool grid, uint64_t parts[8])
{
    parts[0] = Mix(Mix(At<uint32_t>(probe, off_Voxels), At<uint32_t>(probe, off_Rays)), Mix(At<uint8_t>(probe, off_StoreEmpty), At<uint32_t>(probe, off_Quality)));
    parts[1] = HashVec(probe, off_Flags);
    parts[2] = HashVec(probe, off_Ref);
    parts[3] = HashVec(probe, off_Pos);
    parts[4] = grid ? 0 : HashVec(probe, off_AO);
    parts[5] = HashVec(probe, off_Cube);
    uint64_t store = 0;
    for (size_t vec : {size_t(0), size_t(24)})
    {
        const uint8_t* b = At<const uint8_t*>(probe, off_Store + vec);
        const uint8_t* e = At<const uint8_t*>(probe, off_Store + vec + 8);
        for (const uint8_t* blk = b; blk && blk + 24 <= e; blk += 24)
        {
            const uint8_t* data = At<const uint8_t*>(blk, 0);
            const int comp = At<int>(blk, 8), raw = At<int>(blk, 12), first = At<int>(blk, 16);
            store = Mix(store, Mix(Mix(comp, raw), first));
            if (data && comp > 0) store = Mix(store, Hash(data, size_t(comp)));
        }
    }
    parts[6] = store;
    parts[7] = HashVec(probe, off_Store + 96);   // +1600
    uint64_t h = 0;
    for (int i = 0; i < 8; ++i) h = Mix(h, parts[i]);
    return h;
}

std::mutex g_diffLock;
std::vector<uint8_t> g_changedUnion(kProbeBytes / 8, 0);
std::atomic<int> g_diffLogged{0};

void LogDiff(int index, const uint8_t* before, const uint8_t* after)
{
    std::string ranges;
    int start = -1;
    std::lock_guard<std::mutex> lk(g_diffLock);
    for (size_t w = 0; w <= kProbeBytes / 8; ++w)
    {
        const bool changed = w < kProbeBytes / 8 && std::memcmp(before + w * 8, after + w * 8, 8) != 0;
        if (changed) g_changedUnion[w] = 1;
        if (changed && start < 0) start = int(w);
        if (!changed && start >= 0)
        {
            char r[32];
            snprintf(r, sizeof r, " %d-%d", start * 8, int(w) * 8 - 1);
            ranges += r;
            start = -1;
        }
    }
    if (g_diffLogged.fetch_add(1) < 3) Log("bake: trace diag probe %d changed bytes:%s", index, ranges.c_str());
}

std::string UnionRanges()
{
    std::lock_guard<std::mutex> lk(g_diffLock);
    std::string ranges;
    int start = -1;
    for (size_t w = 0; w <= g_changedUnion.size(); ++w)
    {
        const bool c = w < g_changedUnion.size() && g_changedUnion[w];
        if (c && start < 0) start = int(w);
        if (!c && start >= 0) { char r[32]; snprintf(r, sizeof r, " %d-%d", start * 8, int(w) * 8 - 1); ranges += r; start = -1; }
    }
    return ranges;
}

// ---- Radiant functions the restore uses ----
using ProbeFn = void(__fastcall*)(uint8_t*);
using BlockResizeFn = uint64_t(__fastcall*)(void* vec, uint64_t n);
using MallocFn = void*(__fastcall*)(size_t);
using FreeFn = void(__fastcall*)(void*);
using Lz4CompressFn = int(__fastcall*)(const char* src, char* dst, int size);
using Lz4DecompressFn = int(__fastcall*)(const char* src, char* dst, int compressed, int maxOut);
ProbeFn g_rayParams = nullptr, g_storeClear = nullptr;
BlockResizeFn g_blockResize = nullptr;
MallocFn g_malloc = nullptr, g_blockMalloc = nullptr;
FreeFn g_free = nullptr;
Lz4CompressFn g_lz4 = nullptr;
Lz4DecompressFn g_unlz4 = nullptr;


// The guard's rays: from kGuardVoxels voxels spread over the grid along 8 fixed diagonals, against the bake's scene and
// the probe's extra scene. Same origins, directions and triangles give the same hit distances bit for bit.
const int kGuardVoxels = 64, kGuardDirs = 8;
struct Sample { float org[3]; uint32_t dir; float tMain, tExtra; };
static_assert(sizeof(Sample) == 24, "sample layout");
const float kDiag = 0.57735026f;
const float kDirs[kGuardDirs][3] = {{kDiag, kDiag, kDiag}, {-kDiag, kDiag, kDiag}, {kDiag, -kDiag, kDiag}, {-kDiag, -kDiag, kDiag},
                                    {kDiag, kDiag, -kDiag}, {-kDiag, kDiag, -kDiag}, {kDiag, -kDiag, -kDiag}, {-kDiag, -kDiag, -kDiag}};

void Shoot(void* rtc, Sample* s, size_t n, bool extra)
{
    for (size_t i = 0; i < n; i += 4)
    {
        Ray4 r{};
        alignas(16) int valid[4] = {0, 0, 0, 0};
        for (int l = 0; l < 4; ++l)
        {
            r.mask[l] = -1; r.geomID[l] = -1; r.primID[l] = -1; r.instID[l] = -1;
            r.tfar[l] = 1e30f;
            if (i + l >= n) continue;
            const Sample& x = s[i + l];
            valid[l] = -1;
            r.orgx[l] = x.org[0]; r.orgy[l] = x.org[1]; r.orgz[l] = x.org[2];
            r.dirx[l] = kDirs[x.dir][0]; r.diry[l] = kDirs[x.dir][1]; r.dirz[l] = kDirs[x.dir][2];
        }
        if (rtc) g_intersect4(valid, rtc, r);
        for (int l = 0; l < 4 && i + l < n; ++l)
            (extra ? s[i + l].tExtra : s[i + l].tMain) = r.geomID[l] == -1 ? -1.0f : r.tfar[l];
    }
}

void* RtcOf(const uint8_t* sceneObj) { return sceneObj ? At<void*>(sceneObj, 0) : nullptr; }

void MakeSamples(const uint8_t* probe, std::vector<Sample>& out)
{
    out.clear();
    const uint8_t* pb = At<const uint8_t*>(probe, off_Pos);
    const uint8_t* pe = At<const uint8_t*>(probe, off_Pos + 8);
    const size_t n = pb && pe > pb ? size_t(pe - pb) / 12 : 0;
    if (!n) return;
    const size_t voxels = n < size_t(kGuardVoxels) ? n : size_t(kGuardVoxels);
    for (size_t i = 0; i < voxels; ++i)
    {
        const size_t v = (i * n) / voxels + (n / voxels) / 2;
        for (int d = 0; d < kGuardDirs; ++d)
        {
            Sample s{};
            std::memcpy(s.org, pb + 12 * (v < n ? v : n - 1), 12);
            s.dir = uint32_t(d);
            out.push_back(s);
        }
    }
}

// ---- cache files ----
const uint32_t kMagic = 0x43545242;   // "BRTC"
const uint32_t kVersion = 2;
struct Header
{
    uint32_t magic, version;
    uint64_t key, outHash;
    uint32_t voxels, rays;
    int32_t quality;
    uint32_t grid, blocks, samples;
    uint64_t payloadBytes, payloadHash;
    uint64_t parts[8];   // OutputHash parts, for the restore check's log
};
static_assert(sizeof(Header) == 128, "header layout");
const size_t kVecOffs[5] = {off_Flags, off_Ref, off_Pos, off_AO, off_Cube};
const size_t kVecElem[5] = {8, 12, 12, 4, 12};
const size_t kVecPer[5] = {1, 1, 1, 6, 6};   // elements per voxel
const size_t kChunk = 1 << 20;

std::wstring FilePath(uint64_t key)
{
    wchar_t name[40];
    swprintf(name, 40, L"\\%016llx.bth", (unsigned long long)key);
    return g_dir + name;
}

void Put(std::vector<uint8_t>& b, const void* p, size_t n) { b.insert(b.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); }
template <class T> void PutV(std::vector<uint8_t>& b, T v) { Put(b, &v, sizeof v); }

void PutLz4(std::vector<uint8_t>& b, const uint8_t* data, size_t bytes)
{
    PutV<uint64_t>(b, bytes);
    std::vector<char> tmp(kChunk + kChunk / 255 + 64);
    for (size_t off = 0; off < bytes; off += kChunk)
    {
        const int raw = int(bytes - off < kChunk ? bytes - off : kChunk);
        const int comp = g_lz4(reinterpret_cast<const char*>(data + off), tmp.data(), raw);
        if (comp <= 0 || comp >= raw) { PutV<int32_t>(b, raw); PutV<int32_t>(b, 0); Put(b, data + off, size_t(raw)); }
        else { PutV<int32_t>(b, raw); PutV<int32_t>(b, comp); Put(b, tmp.data(), size_t(comp)); }
    }
}

struct Reader
{
    const uint8_t* p;
    const uint8_t* e;
    bool ok = true;
    bool Get(void* dst, size_t n)
    {
        if (!ok || size_t(e - p) < n) return ok = false;
        std::memcpy(dst, p, n); p += n;
        return true;
    }
    template <class T> T V() { T v{}; Get(&v, sizeof v); return v; }
    const uint8_t* Skip(size_t n)
    {
        if (!ok || size_t(e - p) < n) { ok = false; return nullptr; }
        const uint8_t* r = p; p += n;
        return r;
    }
};

bool GetLz4(Reader& r, std::vector<uint8_t>& out)
{
    const uint64_t bytes = r.V<uint64_t>();
    if (!r.ok || bytes > (uint64_t(1) << 34)) return false;
    out.resize(size_t(bytes));
    for (size_t off = 0; off < bytes; )
    {
        const int raw = r.V<int32_t>(), comp = r.V<int32_t>();
        if (!r.ok || raw <= 0 || size_t(raw) > kChunk || off + size_t(raw) > bytes) return false;
        if (comp == 0) { if (!r.Get(out.data() + off, size_t(raw))) return false; }
        else
        {
            const uint8_t* src = r.Skip(size_t(comp));
            if (!src || g_unlz4(reinterpret_cast<const char*>(src), reinterpret_cast<char*>(out.data() + off), comp, raw) != raw) return false;
        }
        off += size_t(raw);
    }
    return true;
}

struct Snapshot
{
    Header h{};
    std::vector<uint8_t> vec[5];
    struct Block { int32_t comp, raw, first; const uint8_t* data; };
    std::vector<Block> blocks[2];
    std::vector<Sample> samples;
    std::vector<uint8_t> file;   // backing store for the block data
};

uint64_t g_bytesSaved = 0, g_bytesLoaded = 0;
std::mutex g_statLock;
double g_msTraced = 0, g_msLoaded = 0;

bool Save(uint8_t* probe, uint64_t key, uint64_t outHash, const uint64_t parts[8], int quality, bool grid, const uint8_t* sceneObj)
{
    std::vector<uint8_t> payload;
    const uint32_t voxels = At<uint32_t>(probe, off_Voxels);
    for (int i = 0; i < 5; ++i)
    {
        const uint8_t* b = At<const uint8_t*>(probe, kVecOffs[i]);
        const uint8_t* e = At<const uint8_t*>(probe, kVecOffs[i] + 8);
        const size_t bytes = b && e > b ? size_t(e - b) : 0;
        if (i == 3 && grid) { PutV<uint64_t>(payload, 0); continue; }   // grids keep whatever AO they had
        if (bytes != size_t(voxels) * kVecElem[i] * kVecPer[i]) return false;
        PutLz4(payload, b, bytes);
    }
    uint32_t blocks = 0;
    for (size_t vec : {size_t(0), size_t(24)})
    {
        const uint8_t* b = At<const uint8_t*>(probe, off_Store + vec);
        const uint8_t* e = At<const uint8_t*>(probe, off_Store + vec + 8);
        const uint32_t n = b && e > b ? uint32_t(size_t(e - b) / 24) : 0;
        if (vec == 0) blocks = n;
        else if (n != blocks) return false;
        for (uint32_t k = 0; k < n; ++k)
        {
            const uint8_t* blk = b + 24 * size_t(k);
            const int32_t comp = At<int32_t>(blk, 8), raw = At<int32_t>(blk, 12), first = At<int32_t>(blk, 16);
            const uint8_t* data = At<const uint8_t*>(blk, 0);
            if (comp <= 0 || !data) return false;
            PutV(payload, comp); PutV(payload, raw); PutV(payload, first);
            Put(payload, data, size_t(comp));
        }
    }
    std::vector<Sample> samples;
    MakeSamples(probe, samples);
    Shoot(RtcOf(sceneObj), samples.data(), samples.size(), false);
    Shoot(RtcOf(At<const uint8_t*>(probe, off_ExtraScene)), samples.data(), samples.size(), true);
    if (!samples.empty()) Put(payload, samples.data(), samples.size() * sizeof(Sample));

    Header h{};
    h.magic = kMagic; h.version = kVersion; h.key = key; h.outHash = outHash;
    h.voxels = voxels; h.rays = At<uint32_t>(probe, off_Rays); h.quality = quality; h.grid = grid ? 1 : 0;
    h.blocks = blocks; h.samples = uint32_t(samples.size());
    h.payloadBytes = payload.size(); h.payloadHash = Hash(payload.data(), payload.size(), key);
    std::memcpy(h.parts, parts, sizeof h.parts);

    CreateDirectoryW(g_dir.c_str(), nullptr);
    const std::wstring path = FilePath(key), tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w1 = 0, w2 = 0;
    const bool ok = WriteFile(f, &h, sizeof h, &w1, nullptr) && w1 == sizeof h &&
                    WriteFile(f, payload.data(), DWORD(payload.size()), &w2, nullptr) && w2 == payload.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp.c_str()); return false; }
    std::lock_guard<std::mutex> lk(g_statLock);
    g_bytesSaved += sizeof h + payload.size();
    return true;
}

// Reads and checks a cache file, the snapshot's block pointers point into snap.file.
bool Load(uint64_t key, Snapshot& snap)
{
    const std::wstring path = FilePath(key);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart >= LONGLONG(sizeof(Header)) && size.QuadPart < (LONGLONG(1) << 32);
    if (ok)
    {
        snap.file.resize(size_t(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(f, snap.file.data(), DWORD(snap.file.size()), &got, nullptr) && got == snap.file.size();
    }
    if (ok)
    {
        FILETIME now;   // last use, for the size cap's oldest-first pruning
        GetSystemTimeAsFileTime(&now);
        SetFileTime(f, nullptr, nullptr, &now);
    }
    CloseHandle(f);
    if (!ok) return false;
    std::memcpy(&snap.h, snap.file.data(), sizeof(Header));
    const Header& h = snap.h;
    if (h.magic != kMagic || h.version != kVersion || h.key != key || h.payloadBytes != snap.file.size() - sizeof(Header)) return false;
    const uint8_t* payload = snap.file.data() + sizeof(Header);
    if (Hash(payload, size_t(h.payloadBytes), key) != h.payloadHash) return false;
    Reader r{payload, payload + h.payloadBytes};
    for (int i = 0; i < 5; ++i)
    {
        if (i == 3 && h.grid) { if (r.V<uint64_t>() != 0) return false; continue; }
        if (!GetLz4(r, snap.vec[i]) || snap.vec[i].size() != size_t(h.voxels) * kVecElem[i] * kVecPer[i]) return false;
    }
    for (int s = 0; s < 2; ++s)
    {
        snap.blocks[s].resize(h.blocks);
        for (Snapshot::Block& b : snap.blocks[s])
        {
            b.comp = r.V<int32_t>(); b.raw = r.V<int32_t>(); b.first = r.V<int32_t>();
            if (!r.ok || b.comp <= 0) return false;
            b.data = r.Skip(size_t(b.comp));
            if (!b.data) return false;
        }
    }
    snap.samples.resize(h.samples);
    if (h.samples && !r.Get(snap.samples.data(), h.samples * sizeof(Sample))) return false;
    return r.ok && r.p == r.e;
}

// The guard: saved rays must hit at the same distances in today's scenes. Radiant rebuilds the BVH every run, which
// moves a hit by an ulp or two, so distances match within 1e-5 relative and at most kGuardSlack rays may differ.
const int kGuardSlack = 2;
bool SameHit(float a, float b)
{
    if ((a < 0) != (b < 0)) return false;
    if (a < 0) return true;
    return std::fabs(a - b) <= 1e-5f * std::max(1.0f, std::fabs(a));
}
int GuardMismatches(const Snapshot& snap, const uint8_t* sceneObj, const uint8_t* probe, std::string* detail = nullptr)
{
    std::vector<Sample> now = snap.samples;
    Shoot(RtcOf(sceneObj), now.data(), now.size(), false);
    Shoot(RtcOf(At<const uint8_t*>(probe, off_ExtraScene)), now.data(), now.size(), true);
    int bad = 0;
    for (size_t i = 0; i < now.size(); ++i)
        if (!SameHit(now[i].tMain, snap.samples[i].tMain) || !SameHit(now[i].tExtra, snap.samples[i].tExtra))
        {
            if (detail && bad < 6)
            {
                char b[96];
                snprintf(b, sizeof b, " [%zu: %.9g/%.9g %.9g/%.9g]", i, snap.samples[i].tMain, now[i].tMain, snap.samples[i].tExtra, now[i].tExtra);
                *detail += b;
            }
            ++bad;
        }
    return bad;
}

bool AssignVec(uint8_t* probe, size_t off, const std::vector<uint8_t>& data)
{
    uint8_t** v = reinterpret_cast<uint8_t**>(probe + off);   // {first, last, end}
    const size_t bytes = data.size();
    if (!v[0] || size_t(v[2] - v[0]) < bytes)
    {
        uint8_t* p = static_cast<uint8_t*>(g_malloc(bytes ? bytes : 16));
        if (!p) return false;
        if (v[0]) g_free(v[0]);
        v[0] = p;
        v[2] = p + bytes;
    }
    if (bytes) std::memcpy(v[0], data.data(), bytes);
    v[1] = v[0] + bytes;
    return true;
}

// Puts the probe in the state the trace pass leaves it in (see the file comment for what the pass writes).
bool Restore(uint8_t* probe, int quality, bool grid, const Snapshot& snap)
{
    *reinterpret_cast<int*>(probe + off_Quality) = quality;   // the pass sets it first: the ray count derives from it
    g_rayParams(probe);
    if (At<uint32_t>(probe, 10176) != snap.h.rays) return false;
    uint8_t* store = probe + off_Store;
    g_storeClear(store);
    *reinterpret_cast<uint32_t*>(probe + off_Rays) = snap.h.rays;
    *reinterpret_cast<uint32_t*>(probe + off_Voxels) = snap.h.voxels;
    *(probe + off_StoreEmpty) = 0;
    for (int i = 0; i < 5; ++i)
    {
        if (i == 3 && grid) continue;
        if (!AssignVec(probe, kVecOffs[i], snap.vec[i])) return false;
    }
    for (int s = 0; s < 2; ++s)
    {
        void* vec = store + 24 * s;
        g_blockResize(vec, snap.h.blocks);
        uint8_t* b = At<uint8_t*>(store, 24 * s);
        const uint8_t* e = At<uint8_t*>(store, 24 * s + 8);
        if (size_t(e - b) != size_t(snap.h.blocks) * 24) return false;
        for (uint32_t k = 0; k < snap.h.blocks; ++k)
        {
            const Snapshot::Block& src = snap.blocks[s][k];
            uint8_t* blk = b + 24 * size_t(k);
            void* data = g_blockMalloc(size_t(src.comp));
            if (!data) return false;
            std::memcpy(data, src.data, size_t(src.comp));
            *reinterpret_cast<void**>(blk) = data;
            *reinterpret_cast<int32_t*>(blk + 8) = src.comp;
            *reinterpret_cast<int32_t*>(blk + 12) = src.raw;
            *reinterpret_cast<int32_t*>(blk + 16) = src.first;
        }
    }
    *(probe + off_StoreEmpty) = snap.h.blocks == 0 ? 1 : 0;
    return true;
}

// ---- the trace pass ----
using TracePassFn = int64_t(__fastcall*)(uint8_t* probe, uint8_t** scene, int quality, char grid, int* debugVoxel, char freeSpace);
TracePassFn o_TracePass = nullptr;

struct Key
{
    uint64_t value = 0;
    uint64_t scene = 0, extra = 0;
    bool usable = false;
};

Key MakeKey(uint8_t* probe, const uint8_t* sceneObj, int quality, char grid, const int* debugVoxel, char freeSpace)
{
    Key k;
    k.scene = sceneObj ? SceneFp(sceneObj) : 0;
    k.extra = SceneFp(At<const uint8_t*>(probe, off_ExtraScene));
    uint64_t h = Mix(0x7ACEC0DE0000ULL + kVersion, Hash(probe, 120));   // placement, rotation, bounds, dims, spacing
    h = Mix(h, Mix(Mix(uint64_t(uint32_t(quality)), uint8_t(grid)), uint8_t(freeSpace)));
    h = Mix(h, At<uint8_t>(probe, 363));
    if (debugVoxel) h = Mix(h, Hash(debugVoxel, 12));
    if (const uint32_t salt = br::bakell::KeySalt(quality)) h = Mix(h, salt);   // the draft tier's ray count and direction sets
    h = Mix(Mix(h, k.scene), k.extra);
    k.value = h;
    k.usable = k.scene && k.extra;
    return k;
}

struct VerifyStats { uint64_t flagsDiff = 0; double posMax = 0, aoMean = 0; int64_t storeCached = 0, storeFresh = 0; };
VerifyStats Compare(const uint8_t* probe, const Snapshot& snap, bool grid)
{
    VerifyStats s;
    const uint64_t* f = At<const uint64_t*>(probe, off_Flags);
    const uint64_t* c = reinterpret_cast<const uint64_t*>(snap.vec[0].data());
    for (uint32_t v = 0; v < snap.h.voxels; ++v) if (f[v] != c[v]) ++s.flagsDiff;
    const float* p = At<const float*>(probe, off_Pos);
    const float* q = reinterpret_cast<const float*>(snap.vec[2].data());
    for (size_t i = 0; i < size_t(snap.h.voxels) * 3; ++i) { const double d = std::fabs(double(p[i]) - q[i]); if (d > s.posMax) s.posMax = d; }
    if (!grid)
    {
        const float* a = At<const float*>(probe, off_AO);
        const float* b = reinterpret_cast<const float*>(snap.vec[3].data());
        double sum = 0;
        for (size_t i = 0; i < size_t(snap.h.voxels) * 6; ++i) sum += std::fabs(double(a[i]) - b[i]);
        s.aoMean = snap.h.voxels ? sum / (double(snap.h.voxels) * 6) : 0;
    }
    for (const Snapshot::Block& b : snap.blocks[0]) s.storeCached += b.comp;
    for (const uint8_t* b = At<const uint8_t*>(probe, off_Store), *e = At<const uint8_t*>(probe, off_Store + 8); b && b + 24 <= e; b += 24)
        s.storeFresh += At<int32_t>(b, 8);
    return s;
}

double Ms(LARGE_INTEGER a, LARGE_INTEGER b)
{
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    return double(b.QuadPart - a.QuadPart) * 1000.0 / double(fq.QuadPart);
}

int64_t Diag(uint8_t* probe, uint8_t** scene, int quality, char grid, int* debugVoxel, char freeSpace, const Key& key)
{
    std::vector<uint8_t> before(kProbeBytes), after(kProbeBytes);
    const bool snap = SafeCopy(before.data(), probe, kProbeBytes);
    const int slot = At<int>(probe, off_Slot);
    const ULONGLONG t0 = GetTickCount64();
    const int64_t r = o_TracePass(probe, scene, quality, grid, debugVoxel, freeSpace);
    const ULONGLONG t1 = GetTickCount64();
    const int index = int(g_passes.load());
    if (snap && SafeCopy(after.data(), probe, kProbeBytes)) LogDiff(index, before.data(), after.data());
    uint64_t parts[8];
    const uint64_t out = OutputHash(probe, grid != 0, parts);
    Log("bake: trace diag probe %p dims %dx%dx%d q%d grid%d fs%d slot %d extra %p key %016llx%s scenefp %016llx extrafp %016llx out %016llx "
        "[%016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx] %llu ms",
        probe, At<int>(probe, off_Dims), At<int>(probe, off_Dims + 4), At<int>(probe, off_Dims + 8), quality, int(grid), int(freeSpace), slot,
        At<void*>(probe, off_ExtraScene), (unsigned long long)key.value, key.usable ? "" : " (unusable)",
        (unsigned long long)key.scene, (unsigned long long)key.extra, (unsigned long long)out,
        (unsigned long long)parts[0], (unsigned long long)parts[1], (unsigned long long)parts[2], (unsigned long long)parts[3],
        (unsigned long long)parts[4], (unsigned long long)parts[5], (unsigned long long)parts[6], (unsigned long long)parts[7],
        (unsigned long long)(t1 - t0));
    return r;
}

int64_t __fastcall hk_TracePass(uint8_t* probe, uint8_t** scene, int quality, char grid, int* debugVoxel, char freeSpace)
{
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (!mode || !probe) return o_TracePass(probe, scene, quality, grid, debugVoxel, freeSpace);
    ++g_passes;
    const uint8_t* sceneObj = scene ? *scene : nullptr;
    Key key;
    try { key = MakeKey(probe, sceneObj, quality, grid, debugVoxel, freeSpace); } catch (...) { key = Key{}; }
    if (mode == 3) return Diag(probe, scene, quality, grid, debugVoxel, freeSpace, key);

    if (MemoryLow())
    {
        // Too little free commit for a snapshot or a save payload: trace the probe the stock way, same result, slower.
        ++g_lowSkips;
        return o_TracePass(probe, scene, quality, grid, debugVoxel, freeSpace);
    }
    const int slot = At<int>(probe, off_Slot);
    if (!key.usable || (slot >= 0 && slot <= 3) || !g_intersect4)
    {
        // Unknown scene (hooks installed after it was built), or a render slot whose brush filter the rays honour.
        ++g_bypassed;
        return o_TracePass(probe, scene, quality, grid, debugVoxel, freeSpace);
    }
    LARGE_INTEGER t0, t1, t2;
    QueryPerformanceCounter(&t0);
    Snapshot snap;
    bool have = false;
    try { have = Load(key.value, snap) && snap.h.quality == quality && snap.h.grid == uint32_t(grid ? 1 : 0); } catch (...) { have = false; }
    if (have && mode == 1)
    {
        std::string detail;
        const int bad = GuardMismatches(snap, sceneObj, probe, &detail);
        if (bad > kGuardSlack)
        {
            ++g_rejected;
            Log("bake: trace cache: probe %p key %016llx rejected, %d of %u guard rays hit differently:%s; tracing it", probe,
                (unsigned long long)key.value, bad, snap.h.samples, detail.c_str());
            DeleteFileW(FilePath(key.value).c_str());
        }
        else
        {
            bool ok = false;
            try { ok = Restore(probe, quality, grid != 0, snap); } catch (...) { ok = false; }
            uint64_t parts[8];
            if (ok && OutputHash(probe, grid != 0, parts) == snap.h.outHash)
            {
                QueryPerformanceCounter(&t1);
                ++g_hits;
                std::lock_guard<std::mutex> lk(g_statLock);
                g_bytesLoaded += snap.file.size();
                g_msLoaded += Ms(t0, t1);
                return 0;
            }
            ++g_restoreFailed;
            std::string which;
            for (int i = 0; i < 8; ++i) if (ok && parts[i] != snap.h.parts[i]) which += " " + std::to_string(i);
            Log("bake: trace cache: probe %p key %016llx dims %dx%dx%d grid %d restore check failed (%s%s), tracing it", probe,
                (unsigned long long)key.value, At<int>(probe, off_Dims), At<int>(probe, off_Dims + 4), At<int>(probe, off_Dims + 8), int(grid),
                ok ? "parts" : "restore", which.c_str());
        }
    }
    QueryPerformanceCounter(&t1);
    const int64_t r = o_TracePass(probe, scene, quality, grid, debugVoxel, freeSpace);
    QueryPerformanceCounter(&t2);
    ++g_misses;
    {
        std::lock_guard<std::mutex> lk(g_statLock);
        g_msTraced += Ms(t1, t2);
    }
    try
    {
        uint64_t parts[8];
        const uint64_t out = OutputHash(probe, grid != 0, parts);
        if (have && mode == 2)
        {
            ++g_verified;
            const VerifyStats s = Compare(probe, snap, grid != 0);
            const int bad = GuardMismatches(snap, sceneObj, probe);
            if (bad > kGuardSlack) ++g_mismatches;
            Log("bake: trace verify probe %p dims %dx%dx%d: %s, guard %d/%u rays differ, flags differ on %llu of %u voxels, positions max %.3g, AO mean %.4g, store %lld vs %lld bytes",
                probe, At<int>(probe, off_Dims), At<int>(probe, off_Dims + 4), At<int>(probe, off_Dims + 8),
                out == snap.h.outHash ? "identical" : "differs", bad, snap.h.samples, (unsigned long long)s.flagsDiff, snap.h.voxels,
                s.posMax, s.aoMean, (long long)s.storeCached, (long long)s.storeFresh);
        }
        else if (Save(probe, key.value, out, parts, quality, grid != 0, sceneObj)) ++g_saved;
    }
    catch (...) {}
    return r;
}

const char* ModeName(int m) { return m == 1 ? "on" : m == 2 ? "verify" : m == 3 ? "diag" : "off"; }
int ParseMode(const std::string& v) { return v == "0" ? 0 : v == "verify" ? 2 : v == "diag" ? 3 : 1; }

bool g_installed = false;
bool Install()
{
    if (g_installed) return true;
    if (!g_env.address || !g_env.detour) return false;
    g_rayParams = reinterpret_cast<ProbeFn>(g_env.address(kRayParams));
    g_storeClear = reinterpret_cast<ProbeFn>(g_env.address(kStoreClear));
    g_blockResize = reinterpret_cast<BlockResizeFn>(g_env.address(kBlockResize));
    g_malloc = reinterpret_cast<MallocFn>(g_env.address(kMalloc));
    g_free = reinterpret_cast<FreeFn>(g_env.address(kFree));
    g_blockMalloc = reinterpret_cast<MallocFn>(g_env.address(kBlockMalloc));
    g_lz4 = reinterpret_cast<Lz4CompressFn>(g_env.address(kLz4));
    g_unlz4 = reinterpret_cast<Lz4DecompressFn>(g_env.address(kUnLz4));
    if (!g_rayParams || !g_storeClear || !g_blockResize || !g_malloc || !g_free || !g_blockMalloc || !g_lz4 || !g_unlz4) return false;
    if (!g_env.detour(g_env.address(kTracePass), reinterpret_cast<void*>(&hk_TracePass), reinterpret_cast<void**>(&o_TracePass)))
    {
        Log("bake: trace cache unavailable (trace pass hook failed)");
        return false;
    }
    if (!HookEmbree() &&
        !g_env.detour(g_env.address(kEmbreeLoad), reinterpret_cast<void*>(&hk_EmbreeLoad), reinterpret_cast<void**>(&o_EmbreeLoad)))
        Log("bake: trace cache: Embree load hook failed, every probe will be traced");
    g_installed = true;
    return true;
}

// Keeps the cache folder under g_capMB: the files used longest ago go first (a load stamps its file's write time).
void Prune()
{
    struct F { std::wstring name; uint64_t size, time; };
    std::vector<F> files;
    uint64_t total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_dir + L"\\*.bth*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const uint64_t size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        const uint64_t time = (uint64_t(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
        const std::wstring name = fd.cFileName;
        if (name.size() > 4 && name.compare(name.size() - 4, 4, L".tmp") == 0) { DeleteFileW((g_dir + L"\\" + name).c_str()); continue; }
        files.push_back({name, size, time});
        total += size;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    const uint64_t cap = g_capMB << 20;
    if (total <= cap) return;
    std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return a.time < b.time; });
    uint64_t removed = 0, count = 0;
    for (const F& f : files)
    {
        if (total <= cap - cap / 5) break;
        if (DeleteFileW((g_dir + L"\\" + f.name).c_str())) { total -= f.size; removed += f.size; ++count; }
    }
    Log("bake: trace cache: pruned %llu files (%.0f MB) to stay under %llu MB", (unsigned long long)count, double(removed) / 1048576.0,
        (unsigned long long)g_capMB);
}
}

std::string Status()
{
    char buf[768];
    double msTraced, msLoaded; uint64_t saved, loaded;
    {
        std::lock_guard<std::mutex> lk(g_statLock);
        msTraced = g_msTraced; msLoaded = g_msLoaded; saved = g_bytesSaved; loaded = g_bytesLoaded;
    }
    snprintf(buf, sizeof buf,
             "trace cache %s%s: %llu probe traces, %llu loaded from the cache (%.1f s, %.0f MB), %llu traced (%.1f s), %llu saved (%.0f MB), "
             "%llu rejected by the guard rays, %llu restore failures, %llu bypassed, %llu skipped on low memory (floor %llu MB free commit), "
             "%llu verified (%llu guard mismatches)",
             ModeName(g_mode.load()), g_installed ? "" : " (unavailable)", (unsigned long long)g_passes.load(), (unsigned long long)g_hits.load(),
             msLoaded / 1000.0, double(loaded) / 1048576.0, (unsigned long long)g_misses.load(), msTraced / 1000.0,
             (unsigned long long)g_saved.load(), double(saved) / 1048576.0, (unsigned long long)g_rejected.load(),
             (unsigned long long)g_restoreFailed.load(), (unsigned long long)g_bypassed.load(), (unsigned long long)g_lowSkips.load(),
             (unsigned long long)(g_floorBytes >> 20), (unsigned long long)g_verified.load(), (unsigned long long)g_mismatches.load());
    std::string s = buf;
    if (g_mode.load() == 3) s += "; changed probe bytes" + UnionRanges();
    return s;
}

void ClearStats()
{
    g_passes = 0; g_hits = 0; g_misses = 0; g_saved = 0; g_verified = 0; g_mismatches = 0; g_bypassed = 0; g_rejected = 0; g_restoreFailed = 0;
    g_lowSkips = 0;
    std::lock_guard<std::mutex> lk(g_statLock);
    g_msTraced = 0; g_msLoaded = 0; g_bytesSaved = 0; g_bytesLoaded = 0;
}

void Setup(const br::bakeguide::Env& env, const std::wstring& cacheDir)
{
    g_env = env;
    g_dir = cacheDir;
    const int mode = ParseMode(env.get ? env.get("trace_cache", "1") : std::string("1"));
    const std::string cap = env.get ? env.get("trace_cache_mb", "4096") : std::string("4096");
    g_capMB = std::max<uint64_t>(64, std::strtoull(cap.c_str(), nullptr, 10));
    const int floorMb = env.get ? atoi(env.get("trace_min_free_mb", "3072").c_str()) : 3072;
    g_floorBytes = uint64_t(floorMb < 0 ? 0 : floorMb) << 20;
    g_mode = mode && Install() ? mode : 0;
    if (g_mode.load()) { try { Prune(); } catch (...) {} }
    Log("bake: %s", Status().c_str());
}

bool Command(const std::string& c, std::string& reply)
{
    if (c.rfind("bake tracecache", 0) != 0) return false;
    const std::string arg = c.size() > 16 ? c.substr(16) : "";
    if (arg == "0" || arg == "1" || arg == "verify" || arg == "diag")
    {
        const int mode = ParseMode(arg);
        if (mode && !Install()) { reply = "trace cache unavailable"; return true; }
        g_mode = mode;
        if (g_env.set) g_env.set("trace_cache", arg.c_str());
    }
    reply = Status();
    return true;
}
}
