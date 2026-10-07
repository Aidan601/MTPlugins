// Free-space fill list cache (see bake_fill.h), interfaces in
// Fast fill: Minv and M[15] depend only on an entry's bits, so the first fill of a list runs Radiant's code and captures
// them, later fills redo only the value arithmetic in Radiant's rounding order.
#include "bake_fill.h"
#include <windows.h>
#include <xmmintrin.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace br::bakefill
{
namespace
{
const uintptr_t kBuildList = 0x14117C860;
const uintptr_t kMalloc    = 0x140B894B0;   // void*(size_t), the exe's allocator
const uintptr_t kFree      = 0x140B894F0;   // void(void*)
const uintptr_t kFill      = 0x14117C9A0;
const uintptr_t kInverse   = 0x140F427B0;   // void(const float M[16], float Minv[16]), row-major, no conditioning check
const uintptr_t kMatVec    = 0x140F41480;   // void(const float M[16], const float v[4], float out[4]), row-major
const uintptr_t kOffsets   = 0x14272A220;   // int32[124][3]: 0..25 the 3x3x3 shell, 26..123 the radius-2 shell

struct ListObj { uint8_t* begin; uint8_t* end; uint8_t* cap; uint8_t flag; };
struct ByteVec { uint8_t* begin; uint8_t* end; uint8_t* cap; };
using BuildFn  = void(__fastcall*)(const ByteVec*, int, int, int, ListObj*);
using MallocFn = void*(__cdecl*)(size_t);
using FreeFn   = void(__cdecl*)(void*);

using FillFn    = void(__fastcall*)(float*, ListObj*, int, int, int, uint32_t);
using InverseFn = void(__fastcall*)(const float*, float*);
using MatVecFn  = void(__fastcall*)(const float*, const float*, float*);
BuildFn o_Build = nullptr;
FillFn o_Fill = nullptr;
InverseFn o_Inverse = nullptr;
MatVecFn g_matVec = nullptr;
int32_t g_offsets[124][3];
bool g_fillHooked = false;
std::atomic<int> g_fillMode{1};      // 0 off, 1 fast, 2 fast + verify every call against Radiant's own fill
MallocFn g_malloc = nullptr;
FreeFn g_free = nullptr;
br::bakeguide::Env g_env{};
bool g_hooked = false;
std::atomic<int> g_mode{1};            // 0 off, 1 cache, 2 cache + verify every hit against Radiant's own build
size_t g_maxBytes = size_t(1) << 30;

// Low-memory guard: below the free commit floor the caches are emptied and store nothing (a worker's bad_alloc kills
// Radiant). Checked at most every 500 ms.
uint64_t g_floorBytes = uint64_t(3) << 30;
std::atomic<uint64_t> g_memTick{0};
std::atomic<bool> g_memLow{false};
std::atomic<uint64_t> g_lowDrops{0};
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

struct Key
{
    uint64_t h1, h2; uint32_t size; int dx, dy, dz;
    bool operator==(const Key& o) const { return h1 == o.h1 && h2 == o.h2 && size == o.size && dx == o.dx && dy == o.dy && dz == o.dz; }
};
struct KeyHash { size_t operator()(const Key& k) const { return size_t(k.h1 ^ (k.h2 * 0x9E3779B97F4A7C15ull)); } };
struct Stored { std::vector<uint8_t> entries; uint8_t flag; };
SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<Key, Stored, KeyHash> g_cache;
size_t g_bytes = 0;
std::atomic<uint64_t> g_hits{0}, g_misses{0}, g_passes{0}, g_verified{0}, g_mismatches{0}, g_clears{0};

void Log(const char* fmt, ...)
{
    if (!g_env.log) return;
    char line[512];
    va_list a; va_start(a, fmt); vsnprintf(line, sizeof line, fmt, a); va_end(a);
    g_env.log(line);
}

uint64_t Mix(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; return x ^ (x >> 33); }

// Two independent 64-bit hashes of the mask.
Key MakeKey(const uint8_t* p, size_t n, int dx, int dy, int dz)
{
    uint64_t a = 0x243F6A8885A308D3ull ^ n, b = 0x13198A2E03707344ull + n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
    {
        uint64_t w; std::memcpy(&w, p + i, 8);
        a = Mix(a ^ w) + 0x9E3779B97F4A7C15ull;
        b = (b ^ Mix(w + 0xA4093822299F31D0ull)) * 0x100000001B3ull;
    }
    uint64_t tail = 0;
    if (i < n) std::memcpy(&tail, p + i, n - i);
    a = Mix(a ^ tail); b = Mix(b + tail);
    return Key{a, b, uint32_t(n), dx, dy, dz};
}

bool Serve(ListObj* out, const Stored& s)
{
    const size_t n = s.entries.size();
    uint8_t* mem = nullptr;
    if (n)
    {
        mem = static_cast<uint8_t*>(g_malloc(n));
        if (!mem) return false;
        std::memcpy(mem, s.entries.data(), n);
    }
    out->begin = mem; out->end = mem + n; out->cap = mem + n; out->flag = s.flag;
    return true;
}

void __fastcall hk_Build(const ByteVec* mask, int dx, int dy, int dz, ListObj* out)
{
    const int mode = g_mode.load();
    // Only fresh, empty lists (the plane lists), a non-empty one returns at once in Radiant anyway.
    if (!mode || !mask || !out || out->begin || out->end || !mask->begin || mask->end <= mask->begin ||
        dx <= 0 || dy <= 0 || dz <= 0 || size_t(mask->end - mask->begin) != size_t(dx) * size_t(dy) * size_t(dz))
    { ++g_passes; o_Build(mask, dx, dy, dz, out); return; }
    Key key;
    try { key = MakeKey(mask->begin, size_t(mask->end - mask->begin), dx, dy, dz); }
    catch (...) { ++g_passes; o_Build(mask, dx, dy, dz, out); return; }
    bool served = false;
    AcquireSRWLockShared(&g_lock);
    auto it = g_cache.find(key);
    if (it != g_cache.end()) served = Serve(out, it->second);
    Stored copy;
    if (served && mode == 2) copy = it->second;
    ReleaseSRWLockShared(&g_lock);
    if (served)
    {
        ++g_hits;
        if (mode == 2)
        {
            ListObj check{nullptr, nullptr, nullptr, 0};
            o_Build(mask, dx, dy, dz, &check);
            const size_t n = size_t(check.end - check.begin);
            // Compare the 21 meaningful bytes of each 24-byte entry (bits, voxel, useFit), Radiant leaves the padding unset.
            size_t bad = SIZE_MAX;
            bool same = check.flag == copy.flag && n == copy.entries.size() && n % 24 == 0;
            for (size_t e = 0; same && e < n; e += 24)
                if (std::memcmp(check.begin + e, copy.entries.data() + e, 21)) { same = false; bad = e / 24; }
            ++g_verified;
            if (!same)
            {
                ++g_mismatches;
                Log("bake fill cache: MISMATCH on a %dx%dx%d list (%zu vs %zu bytes, flag %d vs %d, first entry %zd)", dx, dy, dz, n,
                    copy.entries.size(), check.flag, copy.flag, bad == SIZE_MAX ? -1 : ptrdiff_t(bad));
            }
            if (check.begin) g_free(check.begin);
        }
        return;
    }
    ++g_misses;
    o_Build(mask, dx, dy, dz, out);
    try
    {
        Stored s;
        s.entries.assign(out->begin, out->end);
        s.flag = out->flag;
        const size_t add = s.entries.size() + sizeof(Key) + 64;
        const bool low = MemoryLow();
        AcquireSRWLockExclusive(&g_lock);
        if (low) { if (!g_cache.empty()) { std::unordered_map<Key, Stored, KeyHash>().swap(g_cache); g_bytes = 0; ++g_lowDrops; } }
        else
        {
            if (g_bytes + add > g_maxBytes) { g_cache.clear(); g_bytes = 0; ++g_clears; }
            if (g_cache.emplace(key, std::move(s)).second) g_bytes += add;
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
    catch (...) {}
}

void ClearLocked() { g_cache.clear(); g_bytes = 0; }
const char* ModeName(int m) { return m == 0 ? "off" : m == 2 ? "verify" : "on"; }

// ---- fast fill ----
struct FitCache { std::vector<float> fits; };    // per fit entry in list order: Minv[16], M[15]
std::unordered_map<Key, FitCache, KeyHash> g_fits;
SRWLOCK g_fitLock = SRWLOCK_INIT;
size_t g_fitBytes = 0;
std::atomic<uint64_t> g_fillFast{0}, g_fillCaptured{0}, g_fillPassed{0}, g_fillVerified{0}, g_fillMismatches{0};
thread_local std::vector<float>* t_capture = nullptr;

void __fastcall hk_Inverse(const float* m, float* inv)
{
    o_Inverse(m, inv);
    if (std::vector<float>* c = t_capture)
    {
        try { c->insert(c->end(), inv, inv + 16); c->push_back(m[15]); }
        catch (...) { t_capture = nullptr; }
    }
}

// The list's meaningful content (bits, voxel, useFit per entry) plus dims and channels.
Key ListKey(const ListObj* l, int dx, int dy, int dz, uint32_t ch, size_t& fits)
{
    uint64_t a = 0x452821E638D01377ull ^ ch, b = 0xBE5466CF34E90C6Cull + ch;
    fits = 0;
    for (const uint8_t* e = l->begin; e + 24 <= l->end; e += 24)
    {
        uint64_t w0, w1, w2 = 0;
        std::memcpy(&w0, e, 8); std::memcpy(&w1, e + 8, 8); std::memcpy(&w2, e + 16, 5);
        a = Mix(a ^ w0) + w1; a = Mix(a ^ w2);
        b = (b ^ Mix(w1 + 0x3C6EF372FE94F82Bull)) * 0x100000001B3ull ^ Mix(w2 ^ w0);
        fits += e[20] != 0;
    }
    return Key{Mix(a), Mix(b), uint32_t(l->end - l->begin), dx, dy, dz};
}

double BitsToDouble(uint64_t u) { double d; std::memcpy(&d, &u, 8); return d; }
const double kLumR = BitsToDouble(0x3FCB367A0F9096BCull), kLumG = BitsToDouble(0x3FE6E2EB1C432CA5ull),
             kLumB = BitsToDouble(0x3FB27BB2FEC56D5Dull), kLumFloor = BitsToDouble(0x3F10000000000000ull);
double Lum(float r, float g, float b)
{
    double l = (double(g) * kLumG + double(r) * kLumR) + double(b) * kLumB;
    if (kLumFloor > l) l = kLumFloor;       // comisd: a NaN is kept
    return l;
}

// Per call: each neighbour's index offset, (x, y, z, 1) and weight 1/|o|^2. The four plane-fit sums of a channel ride
// in one SSE register (lane l adds w * (p_l * x)). Only row 3 of mat-vec 0x140F41480 is used: ((m12*b0 + m13*b1) + m14*b2) + m15*b3.
void FastFill(float* values, const ListObj* list, int dx, int dy, int dz, uint32_t ch, const float* fits)
{
    const int32_t total = int32_t(ch) * dx * dy * dz;
    for (int32_t i = 0; i < total; ++i) if (!(0.0f < values[i])) { if (values[i] == values[i]) values[i] = 0.0f; }
    int32_t delta[124];
    __m128 dir[124];
    float weight[124];
    for (int k = 0; k < 124; ++k)
    {
        const int32_t ox = g_offsets[k][0], oy = g_offsets[k][1], oz = g_offsets[k][2];
        const float px = float(ox), py = float(oy), pz = float(oz);
        delta[k] = ox + dx * (oy + dy * oz);
        dir[k] = _mm_setr_ps(px, py, pz, 1.0f);
        weight[k] = 1.0f / ((px * px + py * py) + pz * pz);
    }
    __m128 b[18];
    float avg[18];
    for (const uint8_t* e = list->begin; e + 24 <= list->end; e += 24)
    {
        uint64_t bits[2]; std::memcpy(bits, e, 16);
        uint32_t idx; std::memcpy(&idx, e + 16, 4);
        const bool fit = e[20] != 0;
        if (fit)
        {
            for (uint32_t c = 0; c < ch; ++c) b[c] = _mm_setzero_ps();
            for (int k = 0; k < 124; ++k)
            {
                if (!((bits[k >> 6] >> (k & 63)) & 1)) continue;
                const float* v = values + size_t(int32_t(idx) + delta[k]) * ch;
                const __m128 w = _mm_set1_ps(weight[k]), p = dir[k];
                for (uint32_t c = 0; c < ch; ++c) b[c] = _mm_add_ps(b[c], _mm_mul_ps(w, _mm_mul_ps(p, _mm_set1_ps(v[c]))));
            }
            const float m12 = fits[12], m13 = fits[13], m14 = fits[14], m15 = fits[15];
            const float recip = 1.0f / fits[16];
            for (uint32_t c = 0; c < ch; ++c)
            {
                alignas(16) float s[4];
                _mm_store_ps(s, b[c]);
                values[size_t(idx) * ch + c] = (((m12 * s[0]) + (m13 * s[1])) + (m14 * s[2])) + (m15 * s[3]);
                avg[c] = recip * s[3];
            }
            if (ch == 18)
                for (int f = 0; f < 6; ++f)
                {
                    float* o = values + size_t(idx) * 18 + 3 * f;
                    const double lo = Lum(o[0], o[1], o[2]), la = Lum(avg[3 * f], avg[3 * f + 1], avg[3 * f + 2]);
                    float ratio = float(lo / la);
                    if (0.0f > ratio) ratio = 0.0f;
                    o[0] = ratio * avg[3 * f]; o[1] = ratio * avg[3 * f + 1]; o[2] = ratio * avg[3 * f + 2];
                }
            fits += 17;
        }
        else
        {
            float acc[18] = {};
            int count = 0;
            for (int k = 0; k < 26; ++k)
            {
                if (!((bits[0] >> k) & 1)) continue;
                const float* v = values + size_t(int32_t(idx) + delta[k]) * ch;
                for (uint32_t c = 0; c < ch; ++c) acc[c] += v[c];
                ++count;
            }
            const float recip = 1.0f / float(count);
            for (uint32_t c = 0; c < ch; ++c) values[size_t(idx) * ch + c] = recip * acc[c];
        }
    }
}

void __fastcall hk_Fill(float* values, ListObj* list, int dx, int dy, int dz, uint32_t ch)
{
    const int mode = g_fillMode.load();
    if (!mode || !values || !list || !list->begin || list->end <= list->begin || (list->end - list->begin) % 24 ||
        ch == 0 || ch > 18 || dx <= 0 || dy <= 0 || dz <= 0 || t_capture)
    { ++g_fillPassed; o_Fill(values, list, dx, dy, dz, ch); return; }
    size_t fitCount = 0;
    const Key key = ListKey(list, dx, dy, dz, ch, fitCount);
    std::vector<float> fits;
    AcquireSRWLockShared(&g_fitLock);
    auto it = g_fits.find(key);
    const bool have = it != g_fits.end();
    if (have) fits = it->second.fits;       // copy: another thread may clear the cache
    ReleaseSRWLockShared(&g_fitLock);
    if (!have)
    {
        std::vector<float> cap;
        try { cap.reserve(fitCount * 17); } catch (...) { ++g_fillPassed; o_Fill(values, list, dx, dy, dz, ch); return; }
        t_capture = &cap;
        o_Fill(values, list, dx, dy, dz, ch);
        const bool ok = t_capture != nullptr && cap.size() == fitCount * 17;
        t_capture = nullptr;
        ++g_fillCaptured;
        if (ok)
            try
            {
                const bool low = MemoryLow();
                AcquireSRWLockExclusive(&g_fitLock);
                if (low) { if (!g_fits.empty()) { std::unordered_map<Key, FitCache, KeyHash>().swap(g_fits); g_fitBytes = 0; ++g_lowDrops; } }
                else
                {
                    if (g_fitBytes + cap.size() * 4 > g_maxBytes) { g_fits.clear(); g_fitBytes = 0; }
                    g_fitBytes += cap.size() * 4;
                    g_fits[key].fits = std::move(cap);
                }
                ReleaseSRWLockExclusive(&g_fitLock);
            }
            catch (...) { ReleaseSRWLockExclusive(&g_fitLock); }
        return;
    }
    if (mode == 2)
    {
        const size_t n = size_t(ch) * size_t(dx) * size_t(dy) * size_t(dz);
        std::vector<float> ref;
        try { ref.assign(values, values + n); } catch (...) { ++g_fillPassed; o_Fill(values, list, dx, dy, dz, ch); return; }
        o_Fill(ref.data(), list, dx, dy, dz, ch);
        FastFill(values, list, dx, dy, dz, ch, fits.data());
        ++g_fillVerified;
        if (std::memcmp(ref.data(), values, n * sizeof(float)))
        {
            size_t first = 0; while (first < n && !std::memcmp(&ref[first], &values[first], 4)) ++first;
            ++g_fillMismatches;
            Log("bake fill: MISMATCH %dx%dx%d ch %u at float %zu: stock %.9g fast %.9g", dx, dy, dz, ch, first, ref[first], values[first]);
            std::memcpy(values, ref.data(), n * sizeof(float));   // keep Radiant's result
        }
        ++g_fillFast;
        return;
    }
    FastFill(values, list, dx, dy, dz, ch, fits.data());
    ++g_fillFast;
}

bool InstallFill(const br::bakeguide::Env& env)
{
    if (g_fillHooked) return true;
    const void* table = env.address(kOffsets);
    g_matVec = reinterpret_cast<MatVecFn>(env.address(kMatVec));
    void* fill = env.address(kFill);
    void* inv = env.address(kInverse);
    if (!table || !g_matVec || !fill || !inv) return false;
    std::memcpy(g_offsets, table, sizeof g_offsets);
    // Sanity: the first 26 offsets are the 3x3x3 shell (components in -1..1, never all 0).
    for (int k = 0; k < 124; ++k)
    {
        const int32_t* o = g_offsets[k];
        const int lim = k < 26 ? 1 : 2;
        if (o[0] < -lim || o[0] > lim || o[1] < -lim || o[1] > lim || o[2] < -lim || o[2] > lim || (!o[0] && !o[1] && !o[2])) return false;
    }
    g_fillHooked = env.detour(inv, reinterpret_cast<void*>(&hk_Inverse), reinterpret_cast<void**>(&o_Inverse)) &&
                   env.detour(fill, reinterpret_cast<void*>(&hk_Fill), reinterpret_cast<void**>(&o_Fill));
    return g_fillHooked;
}
}

void Clear()
{
    AcquireSRWLockExclusive(&g_lock);
    ClearLocked();
    std::unordered_map<Key, Stored, KeyHash>().swap(g_cache);   // give the bucket array back too
    ReleaseSRWLockExclusive(&g_lock);
    AcquireSRWLockExclusive(&g_fitLock);
    std::unordered_map<Key, FitCache, KeyHash>().swap(g_fits);
    g_fitBytes = 0;
    ReleaseSRWLockExclusive(&g_fitLock);
}

std::string Status()
{
    char buf[1024];
    AcquireSRWLockShared(&g_lock);
    const size_t lists = g_cache.size(), bytes = g_bytes;
    ReleaseSRWLockShared(&g_lock);
    AcquireSRWLockShared(&g_fitLock);
    const size_t fitLists = g_fits.size(), fitBytes = g_fitBytes;
    ReleaseSRWLockShared(&g_fitLock);
    snprintf(buf, sizeof buf, "fill cache %s (hook %s): %zu lists, %.1f MB; %llu hits, %llu misses, %llu passed through, %llu verified, %llu mismatches, %llu clears; "
             "fast fill %s (hook %s): %zu lists, %.1f MB; %llu fast, %llu captured, %llu passed through, %llu verified, %llu mismatches; "
             "%llu low-memory drops (floor %llu MB free commit)",
             ModeName(g_mode.load()), g_hooked ? "installed" : "not installed", lists, double(bytes) / 1048576.0,
             (unsigned long long)g_hits, (unsigned long long)g_misses, (unsigned long long)g_passes,
             (unsigned long long)g_verified, (unsigned long long)g_mismatches, (unsigned long long)g_clears,
             ModeName(g_fillMode.load()), g_fillHooked ? "installed" : "not installed", fitLists, double(fitBytes) / 1048576.0,
             (unsigned long long)g_fillFast, (unsigned long long)g_fillCaptured, (unsigned long long)g_fillPassed,
             (unsigned long long)g_fillVerified, (unsigned long long)g_fillMismatches,
             (unsigned long long)g_lowDrops.load(), (unsigned long long)(g_floorBytes >> 20));
    return buf;
}

void Setup(const br::bakeguide::Env& env)
{
    g_env = env;
    const std::string v = env.get ? env.get("fill_cache", "1") : std::string("1");
    g_mode = v == "0" ? 0 : v == "verify" ? 2 : 1;
    const int mb = env.get ? atoi(env.get("fill_cache_mb", "512").c_str()) : 512;
    g_maxBytes = size_t(mb < 16 ? 16 : mb) << 20;
    const int floorMb = env.get ? atoi(env.get("fill_min_free_mb", "3072").c_str()) : 3072;
    g_floorBytes = uint64_t(floorMb < 0 ? 0 : floorMb) << 20;
    if (g_mode.load() && !g_hooked)
    {
        g_malloc = reinterpret_cast<MallocFn>(env.address(kMalloc));
        g_free = reinterpret_cast<FreeFn>(env.address(kFree));
        void* target = env.address(kBuildList);
        g_hooked = g_malloc && g_free && target && env.detour(target, reinterpret_cast<void*>(&hk_Build), reinterpret_cast<void**>(&o_Build));
        if (!g_hooked) g_mode = 0;
    }
    const std::string f = env.get ? env.get("fill_fast", "1") : std::string("1");
    g_fillMode = f == "0" ? 0 : f == "verify" ? 2 : 1;
    if (g_fillMode.load() && !InstallFill(env)) { g_fillMode = 0; Log("bake: fast fill unavailable (addresses or detours)"); }
    Log("bake: %s", Status().c_str());
}

bool Command(const std::string& c, std::string& reply)
{
    if (c.rfind("bake fillfast", 0) == 0)
    {
        const std::string arg = c.size() > 14 ? c.substr(14) : "";
        if (arg == "0" || arg == "1" || arg == "verify")
        {
            if (arg != "0" && !InstallFill(g_env)) { reply = "fast fill could not be installed"; return true; }
            g_fillMode = arg == "0" ? 0 : arg == "verify" ? 2 : 1;
            if (g_env.set) g_env.set("fill_fast", arg.c_str());
        }
        reply = Status();
        return true;
    }
    if (c.rfind("bake fillcache", 0) != 0) return false;
    const std::string arg = c.size() > 15 ? c.substr(15) : "";
    if (arg == "0" || arg == "1" || arg == "verify")
    {
        if (arg != "0" && !g_hooked)
        {
            g_malloc = reinterpret_cast<MallocFn>(g_env.address(kMalloc));
            g_free = reinterpret_cast<FreeFn>(g_env.address(kFree));
            void* target = g_env.address(kBuildList);
            g_hooked = g_malloc && g_free && target && g_env.detour(target, reinterpret_cast<void*>(&hk_Build), reinterpret_cast<void**>(&o_Build));
            if (!g_hooked) { reply = "fill cache hook could not be installed"; return true; }
        }
        g_mode = arg == "0" ? 0 : arg == "verify" ? 2 : 1;
        if (g_env.set) g_env.set("fill_cache", arg.c_str());
    }
    reply = Status();
    return true;
}
}
