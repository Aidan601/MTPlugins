// Interactive 2D detail: while a mouse button is held, input waits or 2D frames follow closely, objects smaller than
// a few pixels (prefab interiors included) are left out. The pixel limit follows the measured frame time, the selection
// is always whole, and one full frame is drawn when the interaction stops.
#include "modtools_plugin.hpp"
#include "xylod.h"
#include "fxpreview.h"
#include <windows.h>
#include <intrin.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace br::xylod
{
namespace
{
    // IDA addresses (Radiant_modtools.exe, no ASLR), resolved through host->RadiantAddress.
    const uintptr_t kBrushCollectionForEach = 0x140B00BB0;  // (BrushCollection*, std::function<bool(BrushInst*)>*)
    const uintptr_t kSelectedBrushes        = 0x1453A2278;  // BrushCollection "Selected Brushes"
    const uintptr_t kPrefabDraw             = 0x1401F3C50;  // draws a prefab instance's interior: (mode, entity, float[12] parent, list, drawState, a6)
    const uintptr_t kPrefabDrawEnd          = 0x1401F4210;  // just past its last instruction
    const uintptr_t kComposeTransform       = 0x140C8ED90;  // (entity, float[12] parent, float[12] out): the instance's transform
    const uintptr_t kVisibility             = 0x1401E8FE0;  // (BrushInst*, bool): nonzero = filtered out, first call of the interior loop

    const size_t off_XY_Scale        = 132;
    const size_t off_XY_Origin       = 136;
    const size_t off_XY_ViewType     = 264;
    const size_t off_BInst_Brush     = 48;
    const size_t off_BInst_WorldMins = 480;
    const size_t off_BInst_WorldMaxs = 492;
    const size_t off_Brush_Mins      = 64;
    const size_t off_Brush_Maxs      = 76;

    const int kLevels[] = { 0, 3, 6, 12, 24, 48, 96, 192 };
    const int kLevelCount = int(sizeof kLevels / sizeof kLevels[0]);
    const uint64_t kBurstMs = 120;      // a 2D frame this soon after the last one belongs to the same interaction
    const uint64_t kSettleMs = 250;     // quiet time before the full frame

    const int kZoomBuckets = 40;        // scale 2^-12 .. 2^8
    const uint64_t kHoldMs = 5000;

    int ZoomBucket(float scale)
    {
        const int b = int(std::floor(std::log2(scale) * 2.0f + 0.5f)) + 24;
        return b < 0 ? 0 : b >= kZoomBuckets ? kZoomBuckets - 1 : b;
    }

    struct View
    {
        int      level = 0;             // index into kLevels used by the last interactive frame
        // one learned level per half octave of view scale (cost depends on zoom above all)
        signed char zoomLevel[kZoomBuckets];
        signed char zoomHold[kZoomBuckets];     // going below this level went over budget there...
        uint64_t zoomHoldMs[kZoomBuckets];      // ...at this time (forgotten after kHoldMs: the view moves on)
        int      loweredBucket = -1;    // the last frame tried one level less in this bucket
        View() { for (int i = 0; i < kZoomBuckets; ++i) { zoomLevel[i] = -1; zoomHold[i] = 0; zoomHoldMs[i] = 0; } }
        bool     needSettle = false;    // the last frame left objects out
        uint64_t forceFullUntil = 0;    // the settle frame: draw whole until this time
        uint64_t lastEndMs = 0;
        double   lastMs = 0;            // the last frame, any kind
        float    drawnScale = 0;        // the view scale and origin of the last frame, any kind
        float    drawnOrigin[3] = {};
        double   fullMs = 0;            // the last full frame
        float    fullScale = 0;
    };

    using ForEachFn = __int64 (__fastcall*)(void* collection, void* function);
    using PrefabDrawFn = void (__fastcall*)(int mode, void* entity, const float* parent, void* list, void* drawState, void* a6);
    using ComposeFn = __int64 (__fastcall*)(void* entity, const float* parent, float* out);
    using VisibilityFn = __int64 (__fastcall*)(__int64 brushInst, __int64 flag);

    bool     g_enabled = true;          // [performance] xy_lod
    int      g_budgetMs = 16;           // [performance] xy_lod_budget_ms
    int      g_maxPx = 96;              // [performance] xy_lod_max_px
    int      g_forcePx = 0;             // test: this limit on every frame, at rest too (not saved)
    bool     g_hook = false;
    ForEachFn g_oForEach = nullptr;
    PrefabDrawFn g_oPrefabDraw = nullptr;
    ComposeFn g_compose = nullptr;
    VisibilityFn g_oVisibility = nullptr;
    bool     g_nestedHooks = false;     // the interior loop's early exit, without it objects leave at the dispatcher
    uintptr_t g_prefabDrawBegin = 0, g_prefabDrawEnd = 0;
    void*    g_selected = nullptr;
    View     g_views[3];
    void*    g_lastXY[3] = {};          // test: the CXYWnd of each view type, for "xylod scale"

    // the frame being drawn (main thread, t_* guard the hooks that other threads reach too)
    int      g_frameType = -1;
    bool     g_frameInteractive = false;
    bool     g_frameReduced = false;    // this frame culls and limits
    int      g_framePx = 0;
    float    g_frameScale = 0;
    float    g_frameOrigin[3] = {};
    int      g_frameBucket = 0;
    char     g_trace[32][12];           // test: the last frames as "<limit>:<ms>", F = full
    unsigned g_traceNext = 0;
    uint64_t g_frameSeen = 0, g_frameSkipped = 0, g_frameCulled = 0;
    thread_local bool  t_on = false;
    thread_local float t_world2 = 0;    // squared world-space size limit, 0 = none
    thread_local bool  t_cull = false;  // the view rectangle below is valid
    thread_local int   t_dim1 = 0, t_dim2 = 1;
    thread_local float t_min1 = 0, t_max1 = 0, t_min2 = 0, t_max2 = 0;
    thread_local int   t_selected = 0;  // inside the walk of the selected brushes
    const int kMaxDepth = 24;
    thread_local int   t_depth = 0;     // prefab instances being drawn, innermost last
    thread_local float t_xform[kMaxDepth][16];
    thread_local __int64 t_passed = 0;  // the last object Skip let through (the dispatcher asks again)

    uint64_t g_benchUntil = 0;          // test: request a 2D frame every tick until then
    uint64_t g_benchFrames = 0;
    double   g_benchMs = 0;
    uint64_t g_lodFrames = 0, g_fullFrames = 0, g_settles = 0, g_seen = 0, g_skipped = 0, g_culled = 0, g_faults = 0;

    uint64_t NowMs()
    {
        static LARGE_INTEGER freq = {};
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        return uint64_t(t.QuadPart * 1000 / freq.QuadPart);
    }

    bool ButtonHeld()
    {
        return ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000) != 0;
    }

    bool InputWaiting()
    {
        return HIWORD(GetQueueStatus(QS_MOUSEBUTTON | QS_KEY)) != 0;    // the wheel counts as a button message
    }

    int MaxLevel()
    {
        int m = 0;
        for (int i = 0; i < kLevelCount; ++i) if (kLevels[i] <= g_maxPx) m = i;
        return m;
    }

    bool ScaleNear(float a, float b, float tolerance)
    {
        if (a <= 0 || b <= 0) return false;
        const float r = a > b ? a / b : b / a;
        return r <= 1.0f + tolerance;
    }

    // The level for an interactive frame at this zoom: what was learned there, else at the nearest zoom, else a
    // guess from what the last full frame cost.
    int LevelFor(const View& v, float scale, int bucket)
    {
        if (v.fullMs > 0 && ScaleNear(v.fullScale, scale, 0.1f) && v.fullMs <= g_budgetMs * 1.25) return 0;
        for (int d = 0; d < kZoomBuckets; ++d)
        {
            if (bucket - d >= 0 && v.zoomLevel[bucket - d] >= 0) return v.zoomLevel[bucket - d];   // further out first: it costs more
            if (bucket + d < kZoomBuckets && v.zoomLevel[bucket + d] >= 0) return v.zoomLevel[bucket + d];
        }
        if (v.fullMs <= 0 || !ScaleNear(v.fullScale, scale, 1.0f)) return 0;
        const double ratio = v.fullMs / double(g_budgetMs);
        return ratio <= 1.25 ? 0 : ratio <= 2 ? 1 : ratio <= 4 ? 2 : ratio <= 8 ? 3 : 4;
    }

    __int64 __fastcall hk_ForEach(void* collection, void* function)
    {
        if (br::fxpreview::HidingMapHelpers()) return 1;     // FX window frames: no entity helpers or selection
        if (!t_on || collection != g_selected) return g_oForEach(collection, function);
        ++t_selected;
        const __int64 r = g_oForEach(collection, function);
        --t_selected;
        return r;
    }

    void __fastcall hk_PrefabDraw(int mode, void* entity, const float* parent, void* list, void* drawState, void* a6)
    {
        if (!t_on || !entity || !parent)
        {
            g_oPrefabDraw(mode, entity, parent, list, drawState, a6);
            return;
        }
        if (t_depth >= kMaxDepth)
        {
            // deeper than the stack: hk_Visibility stands back, those objects are decided at the dispatcher
            ++t_depth;
            g_oPrefabDraw(mode, entity, parent, list, drawState, a6);
            --t_depth;
            return;
        }
        g_compose(entity, parent, t_xform[t_depth]);
        ++t_depth;
        g_oPrefabDraw(mode, entity, parent, list, drawState, a6);
        --t_depth;
    }

    __int64 __fastcall hk_Visibility(__int64 brushInst, __int64 flag)
    {
        if (t_on && t_depth > 0 && t_depth <= kMaxDepth)
        {
            const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
            if (ret >= g_prefabDrawBegin && ret < g_prefabDrawEnd && Skip(brushInst, t_xform[t_depth - 1]))
                return 1;   // "filtered out": the loop goes to the next object
        }
        return g_oVisibility(brushInst, flag);
    }

    bool ReadView(const void* xy, int& type, float& scale, float origin[3])
    {
        __try
        {
            const uint8_t* const wnd = static_cast<const uint8_t*>(xy);
            type = *reinterpret_cast<const int*>(wnd + off_XY_ViewType);
            scale = *reinterpret_cast<const float*>(wnd + off_XY_Scale);
            for (int a = 0; a < 3; ++a) origin[a] = *reinterpret_cast<const float*>(wnd + off_XY_Origin + 4 * a);
            return type >= 0 && type <= 2 && std::isfinite(scale) && scale > 0.0001f && scale < 100000.0f &&
                   std::isfinite(origin[0]) && std::isfinite(origin[1]) && std::isfinite(origin[2]);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ++g_faults;
            return false;
        }
    }

    std::string Status()
    {
        char b[900];
        int n = snprintf(b, sizeof b, "xy_lod=%d ready=%d budget=%d ms max=%d px%s bench: %llu frames %.2f ms avg; frames: detail-reduced=%llu full=%llu settle_redraws=%llu objects: seen=%llu outside_view=%llu too_small=%llu faults=%llu",
                         g_enabled ? 1 : 0, g_hook ? 1 : 0, g_budgetMs, g_maxPx,
                         g_forcePx ? (" FORCED " + std::to_string(g_forcePx) + " px").c_str() : "",
                         (unsigned long long)g_benchFrames, g_benchFrames ? g_benchMs / double(g_benchFrames) : 0.0,
                         (unsigned long long)g_lodFrames, (unsigned long long)g_fullFrames, (unsigned long long)g_settles,
                         (unsigned long long)g_seen, (unsigned long long)g_culled, (unsigned long long)g_skipped, (unsigned long long)g_faults);
        static const char* const names[3] = { "YZ", "XZ", "XY" };
        for (int i = 0; i < 3 && n > 0 && n < int(sizeof b); ++i)
        {
            const View& v = g_views[i];
            if (!v.lastEndMs) continue;
            n += snprintf(b + n, sizeof b - n, " | %s: limit=%d px last=%.1f ms full=%.1f ms scale=%.4f hwnd=0x%llx",
                          names[i], kLevels[v.level], v.lastMs, v.fullMs, double(v.drawnScale),
                          (unsigned long long)(rp::host->XYWindow ? reinterpret_cast<uintptr_t>(rp::host->XYWindow(rp::self, i)) : 0));
        }
        std::string out = b;
        out += " | last frames (limit:ms, F = full):";
        for (unsigned i = 0; i < 32; ++i)
        {
            const char* const t = g_trace[(g_traceNext + i) % 32];
            if (*t) { out += ' '; out += t; }
        }
        return out;
    }
}

void Setup()
{
    g_enabled = rp::ConfigGetBool("performance", "xy_lod", true);
    g_budgetMs = rp::ConfigGetInt("performance", "xy_lod_budget_ms", 16);
    if (g_budgetMs < 2) g_budgetMs = 2;
    if (g_budgetMs > 200) g_budgetMs = 200;
    g_maxPx = rp::ConfigGetInt("performance", "xy_lod_max_px", 96);
    if (g_maxPx < 3) g_maxPx = 3;
    if (g_maxPx > 192) g_maxPx = 192;
    g_selected = rp::host->RadiantAddress(rp::self, kSelectedBrushes);
    void* const forEach = rp::host->RadiantAddress(rp::self, kBrushCollectionForEach);
    g_hook = g_selected && forEach &&
             rp::host->Detour(rp::self, forEach, reinterpret_cast<void*>(&hk_ForEach), reinterpret_cast<void**>(&g_oForEach));
    // without the hook the selection could not be told apart: stay off
    if (g_hook)
    {
        void* const prefabDraw = rp::host->RadiantAddress(rp::self, kPrefabDraw);
        void* const visibility = rp::host->RadiantAddress(rp::self, kVisibility);
        g_compose = reinterpret_cast<ComposeFn>(rp::host->RadiantAddress(rp::self, kComposeTransform));
        g_prefabDrawBegin = reinterpret_cast<uintptr_t>(prefabDraw);
        g_prefabDrawEnd = g_prefabDrawBegin + (kPrefabDrawEnd - kPrefabDraw);
        g_nestedHooks = prefabDraw && visibility && g_compose &&
            rp::host->Detour(rp::self, prefabDraw, reinterpret_cast<void*>(&hk_PrefabDraw), reinterpret_cast<void**>(&g_oPrefabDraw)) &&
            rp::host->Detour(rp::self, visibility, reinterpret_cast<void*>(&hk_Visibility), reinterpret_cast<void**>(&g_oVisibility));
    }
    rp::Log("xylod: interactive 2D detail enabled=%d ready=%d nested=%d budget=%d ms max=%d px", g_enabled ? 1 : 0, g_hook ? 1 : 0, g_nestedHooks ? 1 : 0, g_budgetMs, g_maxPx);
}

void RenderBegin()
{
    g_frameType = -1;
    g_frameInteractive = false;
    g_framePx = 0;
    g_frameSeen = g_frameSkipped = g_frameCulled = 0;
}

void PassBegin(const void* xy)
{
    t_on = false;
    if ((!g_enabled && !g_forcePx) || !g_hook || !xy) return;
    int type = -1;
    float scale = 0, origin[3] = {};
    if (!ReadView(xy, type, scale, origin)) return;

    g_lastXY[type] = const_cast<void*>(xy);
    if (g_frameType != type)
    {
        // first pass of this view's frame: decide once, the frame's other passes follow
        View& v = g_views[type];
        const uint64_t now = NowMs();
        g_frameType = type;
        g_frameScale = scale;
        for (int a = 0; a < 3; ++a) g_frameOrigin[a] = origin[a];
        // the view itself moved since its last frame: a pan or a zoom, whatever the input was
        const bool moved = v.lastEndMs && (v.drawnScale != scale || v.drawnOrigin[0] != origin[0] ||
                                           v.drawnOrigin[1] != origin[1] || v.drawnOrigin[2] != origin[2]);
        g_frameInteractive = false;
        g_frameReduced = false;
        g_framePx = 0;
        if (g_forcePx)
        {
            g_frameReduced = true;
            g_framePx = g_forcePx > 1 ? g_forcePx : 0;      // force 1 = the view cull alone
        }
        else if (now < v.forceFullUntil)
            v.forceFullUntil = 0;
        else if (g_benchUntil)
            ;
        else if (moved || ButtonHeld() || (v.lastEndMs && now - v.lastEndMs < kBurstMs) || InputWaiting())
        {
            g_frameInteractive = true;
            g_frameReduced = true;
            g_frameBucket = ZoomBucket(scale);
            v.level = LevelFor(v, scale, g_frameBucket);
            if (v.level > MaxLevel()) v.level = MaxLevel();
            g_framePx = kLevels[v.level];
        }
    }
    if (!g_frameReduced) return;
    const float world = float(g_framePx) / scale;
    t_world2 = world * world;
    // the visible world rectangle (type 2 = XY, 1 = XZ, 0 = YZ), widened: a brush is exact, but an entity can draw
    // a little outside its bounds (names, arrows)
    t_cull = false;
    RECT rect{};
    const HWND hwnd = rp::host->XYWindow ? static_cast<HWND>(rp::host->XYWindow(rp::self, type)) : nullptr;
    if (hwnd && GetClientRect(hwnd, &rect) && rect.right - rect.left >= 16 && rect.bottom - rect.top >= 16)
    {
        t_dim1 = type == 0 ? 1 : 0;
        t_dim2 = type == 2 ? 1 : 2;
        const float pad = 64.0f / scale + 128.0f;
        const float halfW = float(rect.right - rect.left) * 0.5f / scale + pad;
        const float halfH = float(rect.bottom - rect.top) * 0.5f / scale + pad;
        t_min1 = origin[t_dim1] - halfW; t_max1 = origin[t_dim1] + halfW;
        t_min2 = origin[t_dim2] - halfH; t_max2 = origin[t_dim2] + halfH;
        t_cull = true;
    }
    t_selected = 0;
    t_depth = 0;
    t_passed = 0;
    t_on = t_cull || t_world2 > 0;
}

void PassEnd()
{
    t_on = false;
}

bool Skip(__int64 brushInst, const float* m)
{
    if (!t_on || t_selected || !brushInst || brushInst == t_passed) return false;
    ++g_frameSeen;
    __try
    {
        const uint8_t* const inst = reinterpret_cast<const uint8_t*>(brushInst);
        const uint8_t* const brush = *reinterpret_cast<uint8_t* const*>(inst + off_BInst_Brush);
        if (!brush) return false;
        // the authoring brush's bounds, in the space of the prefab that holds it
        const float* const lo = reinterpret_cast<const float*>(brush + off_Brush_Mins);
        const float* const hi = reinterpret_cast<const float*>(brush + off_Brush_Maxs);
        const float ex = hi[0] - lo[0], ey = hi[1] - lo[1], ez = hi[2] - lo[2];
        if (!(ex >= 0 && ey >= 0 && ez >= 0)) return false;        // unset or NaN bounds: draw
        float size2 = ex * ex + ey * ey + ez * ez;
        float wlo[3], whi[3];
        if (!m || (m[0] == 0 && m[1] == 0 && m[2] == 0 && m[3] == 1 && m[7] == 1 && m[11] == 1 &&
                   m[4] == 0 && m[5] == 0 && m[6] == 0 && m[8] == 0 && m[9] == 0 && m[10] == 0))
        {
            for (int a = 0; a < 3; ++a) { wlo[a] = lo[a]; whi[a] = hi[a]; }
        }
        else
        {
            // m = origin, then the three axes
            float scale2 = 0;
            for (int a = 0; a < 3; ++a)
            {
                const float cx = m[3 + a] * (lo[0] + hi[0]) * 0.5f, cy = m[6 + a] * (lo[1] + hi[1]) * 0.5f, cz = m[9 + a] * (lo[2] + hi[2]) * 0.5f;
                const float c = m[a] + cx + cy + cz;
                const float r = (std::fabs(m[3 + a]) * ex + std::fabs(m[6 + a]) * ey + std::fabs(m[9 + a]) * ez) * 0.5f;
                wlo[a] = c - r;
                whi[a] = c + r;
                const float axis2 = m[3 * a + 3] * m[3 * a + 3] + m[3 * a + 4] * m[3 * a + 4] + m[3 * a + 5] * m[3 * a + 5];
                if (axis2 > scale2) scale2 = axis2;
            }
            if (!(scale2 > 0) || !(scale2 < 1e12f)) return false;
            size2 *= scale2;
        }
        // a model or prefab instance is a small proxy brush: the instance carries the world bounds of what is drawn
        const float* const ilo = reinterpret_cast<const float*>(inst + off_BInst_WorldMins);
        const float* const ihi = reinterpret_cast<const float*>(inst + off_BInst_WorldMaxs);
        const float ix = ihi[0] - ilo[0], iy = ihi[1] - ilo[1], iz = ihi[2] - ilo[2];
        if (ix >= 0 && iy >= 0 && iz >= 0)
        {
            const float i2 = ix * ix + iy * iy + iz * iz;
            if (i2 > size2) size2 = i2;
            for (int a = 0; a < 3; ++a)
            {
                if (ilo[a] < wlo[a]) wlo[a] = ilo[a];
                if (ihi[a] > whi[a]) whi[a] = ihi[a];
            }
        }
        if (!(size2 >= 0.0001f)) return false;
        // wholly outside the view (the negated form also draws on NaN)
        if (t_cull && !(wlo[t_dim1] <= t_max1 && whi[t_dim1] >= t_min1 && wlo[t_dim2] <= t_max2 && whi[t_dim2] >= t_min2))
        {
            ++g_frameCulled;
            return true;
        }
        // the 3D diagonal bounds the on-screen size under any rotation
        if (size2 < t_world2)
        {
            ++g_frameSkipped;
            return true;
        }
        t_passed = brushInst;
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ++g_faults;
        return false;
    }
}

void RenderEnd(double ms)
{
    if (g_frameType < 0) return;
    View& v = g_views[g_frameType];
    const uint64_t now = NowMs();
    v.lastEndMs = now;
    v.lastMs = ms;
    v.drawnScale = g_frameScale;
    for (int a = 0; a < 3; ++a) v.drawnOrigin[a] = g_frameOrigin[a];
    char* const trace = g_trace[g_traceNext++ % 32];
    if (g_frameReduced) snprintf(trace, sizeof g_trace[0], "%d:%.0f", g_framePx, ms);
    else snprintf(trace, sizeof g_trace[0], "F:%.0f", ms);
    if (g_benchUntil) { ++g_benchFrames; g_benchMs += ms; }
    g_seen += g_frameSeen;
    g_skipped += g_frameSkipped;
    g_culled += g_frameCulled;
    if (g_frameInteractive)
    {
        ++g_lodFrames;
        const int maxLevel = MaxLevel();
        const int b = g_frameBucket;
        const bool lowered = v.loweredBucket == b;
        v.loweredBucket = -1;
        if (g_frameSkipped || g_frameCulled) v.needSettle = true;
        if (ms > g_budgetMs * 1.25)
        {
            if (lowered) { v.zoomHold[b] = (signed char)(v.level + 1); v.zoomHoldMs[b] = now; }   // one less was too much
            v.level += ms > g_budgetMs * 3.0 ? 2 : 1;
            if (v.level > maxLevel) v.level = maxLevel;
        }
        else if (ms < g_budgetMs * 0.45 && v.level > 0 &&
                 !(v.level <= v.zoomHold[b] && now - v.zoomHoldMs[b] < kHoldMs))
        {
            --v.level;
            v.loweredBucket = b;
        }
        v.zoomLevel[b] = (signed char)v.level;
    }
    else if (!g_forcePx)
    {
        ++g_fullFrames;
        v.fullMs = ms;
        v.fullScale = g_frameScale;
        v.needSettle = false;
    }
    g_frameType = -1;
}

void Tick()
{
    if (!g_hook) return;
    if (g_benchUntil)
    {
        if (NowMs() < g_benchUntil) { rp::host->RequestRedraw(rp::self, MODTOOLS_VIEW_2D); return; }
        g_benchUntil = 0;
        rp::Log("xylod: bench: %llu 2D frames, %.2f ms average (forced limit %d px)",
                (unsigned long long)g_benchFrames, g_benchFrames ? g_benchMs / double(g_benchFrames) : 0.0, g_forcePx);
    }
    bool any = false;
    for (const View& v : g_views) any = any || v.needSettle;
    if (!any || ButtonHeld() || InputWaiting()) return;
    const uint64_t now = NowMs();
    for (const View& v : g_views)
        if (v.needSettle && now - v.lastEndMs < kSettleMs) return;
    for (View& v : g_views)
    {
        if (!v.needSettle) continue;
        v.needSettle = false;
        v.forceFullUntil = now + 1000;
    }
    ++g_settles;
    rp::host->RequestRedraw(rp::self, MODTOOLS_VIEW_2D);
}

bool CursorOn2DView()
{
    if (!rp::host->XYWindow) return false;
    POINT pt;
    if (!GetCursorPos(&pt)) return false;
    const HWND under = WindowFromPoint(pt);
    if (!under) return false;
    for (int type = 0; type < 3; ++type)
    {
        const HWND view = static_cast<HWND>(rp::host->XYWindow(rp::self, type));
        if (view && (view == under || IsChild(view, under))) return true;
    }
    return false;
}

bool Command(const std::string& c, std::string& reply)
{
    if (c.rfind("xylod", 0) != 0 || (c.size() > 5 && c[5] != ' ')) return false;
    const std::string arg = c.size() > 6 ? c.substr(6) : "status";
    if (arg == "0" || arg == "1")
    {
        g_enabled = arg == "1";
        rp::ConfigSet("performance", "xy_lod", arg);
        if (!g_enabled && rp::host->RequestRedraw) rp::host->RequestRedraw(rp::self, MODTOOLS_VIEW_2D);
    }
    else if (arg.rfind("budget ", 0) == 0)
    {
        g_budgetMs = atoi(arg.c_str() + 7);
        if (g_budgetMs < 2) g_budgetMs = 2;
        if (g_budgetMs > 200) g_budgetMs = 200;
        rp::ConfigSet("performance", "xy_lod_budget_ms", std::to_string(g_budgetMs));
    }
    else if (arg.rfind("max ", 0) == 0)
    {
        g_maxPx = atoi(arg.c_str() + 4);
        if (g_maxPx < 3) g_maxPx = 3;
        if (g_maxPx > 192) g_maxPx = 192;
        rp::ConfigSet("performance", "xy_lod_max_px", std::to_string(g_maxPx));
    }
    else if (arg.rfind("force ", 0) == 0)
    {
        g_forcePx = atoi(arg.c_str() + 6);
        if (g_forcePx < 0) g_forcePx = 0;
        if (g_forcePx > 4096) g_forcePx = 4096;
        if (rp::host->RequestRedraw) rp::host->RequestRedraw(rp::self, MODTOOLS_VIEW_2D);
    }
    else if (arg.rfind("bench ", 0) == 0)
    {
        // full frames (or the forced limit) for that many seconds, the average goes to the log and "xylod"
        int seconds = atoi(arg.c_str() + 6);
        if (seconds < 1) seconds = 1;
        if (seconds > 120) seconds = 120;
        g_benchFrames = 0;
        g_benchMs = 0;
        g_benchUntil = NowMs() + uint64_t(seconds) * 1000;
    }
    else if (arg.rfind("scale ", 0) == 0)
    {
        // test: set the zoom of the views drawn so far (what the wheel does), no input needed
        const float scale = float(atof(arg.c_str() + 6));
        if (scale >= 0.005f && scale <= 64.0f)
            for (void* xy : g_lastXY)
                if (xy) *reinterpret_cast<float*>(static_cast<uint8_t*>(xy) + off_XY_Scale) = scale;
        rp::host->RequestRedraw(rp::self, MODTOOLS_VIEW_2D);
    }
    else if (arg == "reset")
    {
        g_lodFrames = g_fullFrames = g_settles = g_seen = g_skipped = g_culled = g_faults = 0;
        for (View& v : g_views) v = {};
    }
    else if (arg != "status")
    {
        reply = "xylod [status|0|1|budget <ms>|max <px>|force <px>|bench <s>|reset]";
        return true;
    }
    reply = Status();
    return true;
}
}
