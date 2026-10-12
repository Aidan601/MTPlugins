// Vertex cache triangle reorder, see vcache.h. Same choices as APE's 0x14021E2F0, but the dead-end pick (fewest live
// triangles over every live vertex, a full scan in APE) comes from a min tree.
#include "modtools_plugin.hpp"
#include "vcache.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ba::vcache
{
// ---- the reorder ----
// Same arguments as APE's: indices (3 per triangle), triangle count, score weights {miss, live, position}, out (3 per triangle),
// batch (triangles per block of the final block interleave). False only for inputs it leaves to APE.
bool Reorder(const uint32_t* idx, uint32_t tris, const float* weights, uint32_t* out, uint32_t batch)
{
    if (batch == 0) return false;
    const uint32_t n = 3 * tris;
    memset(out, 0xFF, size_t(n) * 4);
    if (tris == 0) return true;
    uint32_t maxV = 0;
    for (uint32_t k = 0; k < n; ++k) if (idx[k] > maxV) maxV = idx[k];
    if (maxV >= 0x10000000u) return false;
    const uint32_t verts = maxV + 1;

    // adjacency: each vertex's triangles in triangle order, a triangle once per distinct vertex
    std::vector<uint32_t> first(size_t(verts) + 1, 0), adj(n), live;
    auto distinct = [&](uint32_t t, uint32_t v[3]) -> int {
        const uint32_t* f = idx + size_t(t) * 3;
        int c = 0; v[c++] = f[0];
        if (f[1] != f[0]) v[c++] = f[1];
        if (f[2] != f[0] && f[2] != f[1]) v[c++] = f[2];
        return c;
    };
    for (uint32_t t = 0; t < tris; ++t) { uint32_t v[3]; const int c = distinct(t, v); for (int j = 0; j < c; ++j) ++first[v[j] + 1]; }
    for (uint32_t v = 0; v < verts; ++v) first[v + 1] += first[v];
    {
        std::vector<uint32_t> fill(first.begin(), first.end() - 1);
        for (uint32_t t = 0; t < tris; ++t) { uint32_t v[3]; const int c = distinct(t, v); for (int j = 0; j < c; ++j) adj[fill[v[j]]++] = t; }
    }
    live.resize(verts);
    for (uint32_t v = 0; v < verts; ++v) live[v] = first[v + 1] - first[v];
    std::vector<uint8_t> alive(tris, 1);

    // min tree over (live count, vertex): the dead-end pick is the lowest count, then the lowest vertex
    uint32_t leaves = 1;
    while (leaves < verts) leaves <<= 1;
    const uint64_t kNone = ~0ull;
    std::vector<uint64_t> tree(size_t(leaves) * 2, kNone);
    for (uint32_t v = 0; v < verts; ++v) if (live[v]) tree[leaves + v] = (uint64_t(live[v]) << 32) | v;
    for (uint32_t i = leaves - 1; i >= 1; --i) tree[i] = tree[2 * i] < tree[2 * i + 1] ? tree[2 * i] : tree[2 * i + 1];
    auto setKey = [&](uint32_t v) {
        size_t i = size_t(leaves) + v;
        tree[i] = live[v] ? (uint64_t(live[v]) << 32) | v : kNone;
        for (i >>= 1; i >= 1; i >>= 1) { const uint64_t a = tree[2 * i], b = tree[2 * i + 1]; tree[i] = a < b ? a : b; }
    };

    // the cache: 14 entries, [0] oldest, a push drops the oldest (a degenerate triangle can push one vertex twice)
    const int kCache = 14;
    struct Cache { uint32_t e[14]; };
    auto inCache = [&](const Cache& c, uint32_t v) { for (int j = 0; j < kCache; ++j) if (c.e[j] == v) return true; return false; };
    auto push = [&](Cache& c, uint32_t v) { memmove(c.e, c.e + 1, sizeof(uint32_t) * (kCache - 1)); c.e[kCache - 1] = v; };
    // a triangle's misses against the cache before it is added, then its missing corners pushed in corner order
    auto addTri = [&](Cache& c, const uint32_t* f) {
        const bool h0 = inCache(c, f[0]), h1 = inCache(c, f[1]), h2 = inCache(c, f[2]);
        uint32_t m = 0;
        if (!h0) { push(c, f[0]); ++m; }
        if (!h1) { push(c, f[1]); ++m; }
        if (!h2) { push(c, f[2]); ++m; }
        return m;
    };
    std::vector<uint32_t> fan;
    fan.reserve(64);
    uint32_t* emit = out;
    // every live triangle of v, each time the one with the fewest corners outside the cache (first on ties), then swapped
    // with the last of the list. Returns the misses. Emits and kills only when write is set.
    auto runFan = [&](uint32_t v, Cache& c, bool write) {
        fan.clear();
        for (uint32_t k = first[v]; k < first[v + 1]; ++k) if (alive[adj[k]]) fan.push_back(adj[k]);
        uint32_t misses = 0;
        while (!fan.empty())
        {
            uint32_t best = 0, bestOut = ~0u;
            for (uint32_t k = 0; k < fan.size(); ++k)
            {
                const uint32_t* f = idx + size_t(fan[k]) * 3;
                const uint32_t o = (inCache(c, f[0]) ? 0u : 1u) + (inCache(c, f[1]) ? 0u : 1u) + (inCache(c, f[2]) ? 0u : 1u);
                if (o < bestOut) { bestOut = o; best = k; }
            }
            const uint32_t t = fan[best];
            const uint32_t* f = idx + size_t(t) * 3;
            misses += addTri(c, f);
            if (write)
            {
                alive[t] = 0;
                *emit++ = f[0]; *emit++ = f[1]; *emit++ = f[2];
                uint32_t d[3]; const int cnt = distinct(t, d);
                for (int j = 0; j < cnt; ++j) { --live[d[j]]; setKey(d[j]); }
            }
            fan[best] = fan.back();
            fan.pop_back();
        }
        return misses;
    };

    Cache cache;
    for (int j = 0; j < kCache; ++j) cache.e[j] = ~0u;
    const float w0 = weights[0], w1 = weights[1], w2 = weights[2];
    while (tree[1] != kNone)
    {
        uint32_t pick = ~0u;
        float bestScore = 3.40282347e38f;
        for (int j = 0; j < kCache; ++j)
        {
            const uint32_t v = cache.e[j];
            if (v == ~0u || !live[v]) continue;
            Cache sim = cache;
            const uint32_t misses = runFan(v, sim, false);
            uint32_t pos = 0;
            while (pos < uint32_t(kCache) && sim.e[pos] != v) ++pos;
            const float score = (float(misses) * w0 - float(live[v]) * w1) + float(pos) * w2 / 14.0f;
            if (bestScore > score) { bestScore = score; pick = v; }
        }
        if (pick == ~0u) pick = uint32_t(tree[1]);
        runFan(pick, cache, true);
    }
    if (emit != out + n) return false;

    // blocks of `batch` triangles, taken alternately from two cursors (APE's own interleave, kept as is)
    const uint32_t blocks = tris / batch + (tris % batch ? 1 : 0);
    uint32_t a = 0, b = 3 * batch * (blocks >> 1);
    if (blocks & 1) { a = b; b = 0; }
    const uint32_t step = 3 * batch;
    std::vector<uint32_t> tmp(n);
    for (uint32_t done = 0; done < n;)
    {
        uint32_t c = done + step > n ? n - done : step;
        if (!c) break;
        memcpy(&tmp[done], out + a, size_t(c) * 4); done += c; a += c;
        c = done + step > n ? n - done : step;
        if (!c) break;
        memcpy(&tmp[done], out + b, size_t(c) * 4); done += c; b += c;
    }
    memcpy(out, tmp.data(), size_t(n) * 4);
    return true;
}

namespace
{
    const uintptr_t kReorder = 0x14021E2F0;   // (indices, triangles, float weights[3], out, batch), called by the surface build
    using ReorderFn = void (__fastcall*)(uint32_t*, uint32_t, float*, uint32_t*, uint32_t);
    ReorderFn g_orig = nullptr;
    bool g_hooked = false;
    std::atomic<int> g_mode{ 1 };   // 0 APE's, 1 ours, 2 both and compare (APE's result kept)
    std::atomic<uint64_t> g_calls{ 0 }, g_tris{ 0 }, g_fallback{ 0 }, g_checked{ 0 }, g_mismatch{ 0 }, g_usOurs{ 0 }, g_usApe{ 0 };

    uint64_t Us()
    {
        static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        return uint64_t(t.QuadPart * 1000000 / f.QuadPart);
    }

    void __fastcall hk_Reorder(uint32_t* idx, uint32_t tris, float* w, uint32_t* out, uint32_t batch)
    {
        ++g_calls; g_tris += tris;
        const int mode = g_mode;
        if (mode == 1)
        {
            const uint64_t t0 = Us();
            bool ok = false;
            try { ok = Reorder(idx, tris, w, out, batch); } catch (...) { ok = false; }
            if (ok) { g_usOurs += Us() - t0; return; }
            ++g_fallback;
            g_orig(idx, tris, w, out, batch);
            return;
        }
        std::vector<uint32_t> in;
        if (mode == 2) in.assign(idx, idx + size_t(tris) * 3);
        const uint64_t t0 = Us();
        g_orig(idx, tris, w, out, batch);
        g_usApe += Us() - t0;
        if (mode == 0) return;
        std::vector<uint32_t> mine(size_t(tris) * 3 + 1);
        const uint64_t t1 = Us();
        bool ok = false;
        try { ok = Reorder(in.data(), tris, w, mine.data(), batch); } catch (...) { ok = false; }
        g_usOurs += Us() - t1;
        ++g_checked;
        if (!ok || memcmp(mine.data(), out, size_t(tris) * 12))
        {
            ++g_mismatch;
            rp::Log("reorder: MISMATCH on %u triangles (%s)", tris, ok ? "different order" : "refused");
        }
    }

    bool IsStart(void* p) { DWORD64 base = 0; PRUNTIME_FUNCTION fn = p ? RtlLookupFunctionEntry(DWORD64(p), &base, nullptr) : nullptr; return fn && DWORD64(fn->BeginAddress) + base == DWORD64(p); }

    std::string Status()
    {
        char buf[400];
        snprintf(buf, sizeof buf, "reorder: hooked=%d mode=%s calls=%llu triangles=%llu fallbacks=%llu checked=%llu mismatches=%llu, ours %.0f ms, APE's %.0f ms",
                 g_hooked ? 1 : 0, g_mode == 0 ? "ape" : g_mode == 1 ? "fast" : "verify", (unsigned long long)g_calls.load(), (unsigned long long)g_tris.load(),
                 (unsigned long long)g_fallback.load(), (unsigned long long)g_checked.load(), (unsigned long long)g_mismatch.load(),
                 g_usOurs / 1000.0, g_usApe / 1000.0);
        return buf;
    }
}

void Install()
{
    if (g_hooked || !rp::Ape()) return;
    g_mode = rp::ConfigGetBool("preview", "fast_reorder", true) ? 1 : 0;
    void* fn = rp::Ape()->ApeAddress(rp::self, kReorder);
    if (!IsStart(fn)) { rp::Log("reorder: not found, APE's reorder"); return; }
    if (!rp::host->Detour(rp::self, fn, reinterpret_cast<void*>(&hk_Reorder), reinterpret_cast<void**>(&g_orig))) { rp::Log("reorder: detour failed"); return; }
    g_hooked = true;
    rp::Log("reorder: installed (%s)", g_mode == 1 ? "fast" : "APE's, fast_reorder=0");
}

bool Command(const std::string& c, std::string& reply)
{
    if (c.rfind("reorder", 0) != 0) return false;
    const std::string arg = c.size() > 8 ? c.substr(8) : "";
    if (arg == "0" || arg == "1" || arg == "verify")
    {
        g_mode = arg == "0" ? 0 : arg == "1" ? 1 : 2;
        g_calls = 0; g_tris = 0; g_fallback = 0; g_checked = 0; g_mismatch = 0; g_usOurs = 0; g_usApe = 0;
    }
    reply = Status();
    return true;
}
}
