// RadiantPlus: fixes and quality-of-life changes to Radiant itself. Radiant only.
// Frame cap: shortens Radiant's 17 ms idle timer so the Max FPS preference is honoured.
// Bake boost: steps the light bake several times per tick and throttles 3D redraws while it runs.
#define MODTOOLS_PLUGIN_IMPL
#include "modtools_plugin.hpp"
#include "ray_guiding.h"
#include "bake_guiding.h"
#include "bake_fill.h"
#include "bake_gather.h"
#include "bake_tracecache.h"
#include "bake_lowrays.h"
#include "crashguard.h"
#include "image_formats.h"
#include "autolod.h"
#include "themes.h"
#include "xylod.h"
#include "seloutline.h"
#include "camwalk.h"
#include "gamefov.h"
#include "campresets.h"
#include "loadprefetch.h"
#include "assettouch.h"
#include "filesearch.h"
#include "filtercache.h"
#include "fxswap.h"
#include "fxpreview.h"
#include "iconsize.h"
#include "prefabexplore.h"
#include "colorpicker.h"
#include "filedialogs.h"
#include "dynbuffers.h"
#include "prefswindow.h"
#include "actionlog.h"
#include "rotgizmo.h"
#include "dxvk.h"
#include "discordrpc.h"
#include <windows.h>
#include <shlwapi.h>
#include <d3d11.h>
#include <intrin.h>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "version.lib")
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <deque>
#include <list>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <limits>
#include <unordered_set>

namespace
{
    // IDA addresses (Radiant_modtools.exe, no ASLR), resolved through host->RadiantAddress
    const uintptr_t kIdleTimerPtr = 0x145216990;   // QTimer* driving RoutineProcessing (started with 17 ms)
    const uintptr_t kMaxFpsPref   = 0x1453A1D10;   // float, prefs block + 0xF0, registry Prefs\MaxFPS (0 = no limit)
    const uintptr_t kRedrawFlags  = 0x1444C656C;   // int, pending UpdateWindows bits
    const uintptr_t kTickUpdateReturn = 0x14064C190; // return address of RoutineProcessing's UpdateWindows call
    const uintptr_t kFrameBudget  = 0x1444C6578;   // double, Radiant's limiter: frames it may still ask for
    const uintptr_t kHudFps       = 0x145B0DC08;   // float, the HUD's FPS: smoothed 1000 / frame ms
    const int       kRadiantTickMs = 17;
    const int       kPaceTickMs = 8;        // longest idle tick while pacing

    // light bake
    const uintptr_t kMainWinPtr            = 0x1444C6560;   // RadiantMainWin*, +64 -> QE children, +240 -> CCamWnd*
    const uintptr_t kCamRoutineProcessing  = 0x140232270;   // steps the bake once when +232 == 4
    const uintptr_t kCamOnRender           = 0x14024F080;   // the whole 3D view frame incl. HUD
    const uintptr_t kUpdateGI              = 0x1402F38B0;   // returns remaining work (0 = idle)
    const uintptr_t kInflightJobs          = 0x145581E54;   // int: ray-tracing jobs in flight (capped at 8 by UpdateGI)
    const uintptr_t kToolbarBuild          = 0x14065BA50;   // every Build lighting slot, jumps to CCamWnd build 0x1402F31D0
    const uintptr_t kToolbarReload         = 0x14065BA90;   // Reload lighting from disk (also Cancel and reload)
    // performance instrumentation
    const uintptr_t kMainStatusUpdater     = 0x14064BC40;
    const uintptr_t kMainUpdateWindows     = 0x14064C2B0;
    const uintptr_t kCamReloadAssets       = 0x140238300;
    const uintptr_t kCamMouseMotion        = 0x140230FF0;
    const uintptr_t kCamOnPaint            = 0x14023A540;
    const uintptr_t kBrushCacheUpdate      = 0x1402124B0;
    const uintptr_t kCamProcessAllBrushes  = 0x140243120;
    const uintptr_t kQtXYOnRender          = 0x140C2B300;
    const uintptr_t kXYProcessAllBrushes   = 0x140C2D5F0;
    const uintptr_t kRendererBeginFrame    = 0x1411E7710;
    const uintptr_t kRendererEndFrame      = 0x1411E7760;
    const uintptr_t kDxgiPresent           = 0x1411AEDA0;
    // deep diagnostics (observation only)
    const uintptr_t kFpsDeepLightDirtyThunk      = 0x14000E5D4; // -> 0x1402F3820, compares camera light-state vectors
    const uintptr_t kFpsDeepInputBitsThunk       = 0x14000A7D6; // -> 0x1409BBBD0, global input bitset
    const uintptr_t kFpsDeepSpecialStateThunk    = 0x14002F00E; // -> 0x1402F58B0, special light/bake state gate
    const uintptr_t kFpsDeepPerObjectThunk       = 0x14007F275; // -> 0x1401DC540, per-brush/entity dispatcher
    const uintptr_t kFpsDeepLightReturn          = 0x14064C0C4; // return address in RadiantMainWin::RoutineProcessing
    const uintptr_t kFpsDeepInputReturn          = 0x14064C0D6;
    const uintptr_t kFpsDeepSpecialReturn        = 0x14064C109;
    const uintptr_t kFpsDeepQueueBase            = 0x145585BB0; // 10 x 32-byte camera render-stage queues
    const uintptr_t kFpsDeepJqProfActive         = 0x1443B0E68; // nonzero => built-in JQPROF event capture active
    const uintptr_t kFpsDeepJqProfTrigger        = 0x145216980; // built-in short capture trigger
    const uintptr_t kFpsDeepForceCameraA         = 0x1453A1CC4; // both nonzero force camera-dirty path
    const uintptr_t kFpsDeepForceCameraB         = 0x1453A1CD4;
    // early cull targets (renderer functions)
    const uintptr_t kSceneEarlyCullPoint          = 0x14003AC79;
    const uintptr_t kSceneEarlyGetTransform       = 0x140029EBA;
    const uintptr_t kSceneEarlyTransformPoint     = 0x14001F15E;
    const uintptr_t kSceneEarlySpecialRender      = 0x1401F58D0;
    const uintptr_t kSceneEarlyGeneralRender      = 0x1401F87E0;
    const uintptr_t kSceneEarlyRadiantBrushCallback = 0x1402AEE10;
    const uintptr_t kSceneEarlyBrushInstCallback    = 0x14025E770;
    const uintptr_t kSceneEarlyVolumeDecalCallback  = 0x1402AEF10;
    const uintptr_t kSceneEarlyGenericSceneCallback = 0x1401A8A00;
    // RadiantBrush spatial sidecar
    const uintptr_t kSpatialBrushPrepare          = 0x140213380;
    const uintptr_t kSpatialCategoryVectors       = 0x144487830;
    const uintptr_t kSpatialBrushDirtyFlag        = 0x144487804;
    const uintptr_t kSpatialRadiantBrushIterator  = 0x14008602F; // thunk used by Cam_ProcessAllBrushes
    // 2D viewport cull: BrushInst callback entered before the wireframe dispatcher
    const uintptr_t kGridCullXYBrushCallback      = 0x140C3F6B0;
    const uintptr_t kFpsDeepInputMask            = 0x145215AC0; // 10-byte internal key/button state snapshot
    const size_t    off_MainWin_QEChildren = 64;
    const size_t    off_QEChildren_CamWnd  = 240;
    const size_t    off_Cam_Mode           = 232;           // 4 = real lighting view mode (the bake steps only then)
    const size_t    off_Cam_LightState     = 3960;          // int: the light state being baked (-1 = none)
    const size_t    off_Cam_VolumesBegin   = 6408;          // std::vector<{LightingVolume*, ...}> (16 bytes per entry)
    const size_t    off_Cam_VolumesEnd     = 6416;
    const size_t    off_Cam_Bounce         = 6660;          // int: current bounce
    const size_t    off_Cam_ProbesBegin    = 6664;          // vector, 112 bytes per entry
    const size_t    off_Cam_ProbesEnd      = 6672;
    const size_t    off_Volume_Quality     = 1160;          // int: 0 low, 1 medium, 2 high (export data 2/4/8)
    const size_t    off_Volume_State       = 1164;          // int[4] per light state: 3 sun, 4 bounces, 5 smooth, 6 final, 7/8 lit
    // CXYWnd and BrushInst/brush_t fields for the 2D cull
    const size_t    off_XY_Scale            = 132;
    const size_t    off_XY_Origin           = 136;
    const size_t    off_XY_ViewType         = 264;
    const size_t    off_BInst_Brush          = 48;
    const size_t    off_BInst_WorldMins      = 480;
    const size_t    off_BInst_WorldMaxs      = 492;
    const size_t    off_Brush_Mins           = 64;
    const size_t    off_Brush_Maxs           = 76;
    const size_t    off_Brush_PaddedMins     = 88;
    const size_t    off_Brush_PaddedMaxs     = 100;

    using SetIntervalFn = void (*)(void* timer, int ms);
    using IntervalFn    = int (*)(const void* timer);
    using CamRoutineFn  = __int64 (__fastcall *)(void* cam);
    using OnRenderFn    = __int64 (__fastcall *)(void* cam, __int64 a2, __int64 a3, __int64 a4);
    using UpdateGIFn    = __int64 (__fastcall *)(void* cam, char tickMode);
    using ObjectFn      = __int64 (__fastcall *)(void* object);
    using UpdateWindowsFn = __int64 (__fastcall *)(void* mainWin, int flags);
    using ReloadAssetsFn = __int64 (__fastcall *)(void* cam, unsigned char force);
    using MouseMotionFn = __int64 (__fastcall *)(void* cam, float motion);
    using BrushCacheFn  = __int64 (__fastcall *)();
    using CamBrushesFn  = __int64 (__fastcall *)(void* cam, int pass, int flags);
    using XYBrushesFn   = __int64 (__fastcall *)(void* xy, int pass, __int64* data, __int64 flags);
    using BeginFrameFn  = char (__fastcall *)();
    using FrameFn       = __int64 (__fastcall *)();

    void**        g_timerPtr = nullptr;
    const float*  g_maxFps = nullptr;
    SetIntervalFn g_setInterval = nullptr;
    IntervalFn    g_interval = nullptr;

    // [fps] in RadiantPlus.ini
    bool g_uncap = true;
    int  g_tickMs = 0;          // 0 = derive from Max FPS
    bool g_pace = true;         // hold every view redraw of the idle tick to Max FPS

    // frame pacing
    volatile int* g_redrawFlags = nullptr;
    double*  g_frameBudget = nullptr;
    const float* g_hudFps = nullptr;
    HANDLE   g_paceTimer = nullptr;  // high resolution waitable timer
    void*    g_tickUpdateReturn = nullptr;
    double   g_paceNext = 0;        // earliest time of the next frame, seconds
    uint64_t g_paceHeld = 0;        // UpdateWindows calls put off to a later tick
    uint64_t g_paceDirtyUntil = 0;  // fps dirty test: mark the 3D view dirty every tick until then
    bool     g_firstFpsDone = false;

    // state
    bool     g_applied = false;     // interval changed at least once (restore on unload)
    int      g_lastSet = -1;
    int      g_resets = 0;          // times Radiant put its own interval back
    uint64_t g_lastCheck = 0;
    uint64_t g_lastResetLog = 0;

    // measurement for the fps command
    uint64_t g_statStart = 0;
    uint32_t g_ticks = 0, g_frames = 0;
    double   g_ticksPerSec = 0, g_framesPerSec = 0;

    // ---- light bake boost ----
    bool         g_boost = true;          // [bake] boost
    int          g_boostBudgetMs = 8;     // [bake] budget_ms: main-thread time per tick spent stepping the bake
    int          g_boostRedrawMs = 500;   // [bake] redraw_ms: 3D view redraw interval while baking (0 = never skip)
    bool         g_bakeAvail = false;
    uint8_t**    g_mainWinPtr = nullptr;
    const volatile int* g_inflight = nullptr;
    CamRoutineFn g_oCamRoutine = nullptr;
    OnRenderFn   g_oOnRender = nullptr;
    UpdateGIFn   g_updateGI = nullptr;
    bool         g_bakeActive = false;    // seen through the tick poll
    uint64_t     g_bakeStart = 0;
    uint64_t     g_lastRender = 0;
    uint32_t     g_skippedFrames = 0, g_extraSteps = 0, g_boostTicks = 0;
    LARGE_INTEGER g_qpf = {};
    // Cancel guard: Radiant's cancel does not wait for running probe jobs, and a rebuild or reload before they finish
    // frees data they still read. A build or reload asked for meanwhile is held and replayed once they are done.
    using ToolbarBuildFn  = void(__fastcall*)(void*, char, char);
    using ToolbarReloadFn = void(__fastcall*)(void*);
    bool            g_cancelGuard = true;   // [bake] cancel_guard
    ToolbarBuildFn  g_oToolbarBuild = nullptr;
    ToolbarReloadFn g_oToolbarReload = nullptr;
    struct HeldAction { bool reload; void* mainWin; char sun, probes; };
    std::vector<HeldAction> g_held;         // main thread only
    uint64_t        g_heldSince = 0;
    uint32_t        g_heldTotal = 0;
    // Heavy-map performance: cache the D3DKMT GPU statistics and time the renderer stages. Detours are removed on unload.
    const uintptr_t kGpuStatsQuery       = 0x14100E990; // expensive D3DKMTQueryStatistics walker used by DrawHud
    const uintptr_t kRenderMainPipeline  = 0x1411B8FC0; // deferred/forward lighting, transparency, volumetrics, etc.
    const uintptr_t kRenderGBuffer       = 0x1411B8AE0; // prepass + GBuffer opaque/semi/decal
    const uintptr_t kRenderPostFx        = 0x141101FE0; // bloom/exposure/lens/tonemap/SMAA/etc.

    using GpuStatsFn      = __int64 (__fastcall *)(__int64 state, __int64 process, uint64_t* systemStats, uint64_t* processStats);
    using MainPipelineFn  = uint64_t* (__fastcall *)(unsigned char* state, __int64 debugEvents);
    using RenderStageFn   = __int64 (__fastcall *)(__int64 state, __int64 debugEvents);

    GpuStatsFn     g_oGpuStats = nullptr;
    MainPipelineFn g_oMainPipeline = nullptr;
    RenderStageFn  g_oGBuffer = nullptr;
    RenderStageFn  g_oPostFx = nullptr;

    bool g_gpuStatsCache = true;
    int  g_gpuStatsMs = 1000;
    __int64 g_gpuLastState = 0;
    __int64 g_gpuLastProcess = 0;
    uint64_t g_gpuStatsCalls = 0;
    uint64_t g_gpuStatsBypassed = 0;
    uint64_t g_gpuStatsOriginal = 0;

    struct PerfV2Metric
    {
        uint64_t calls = 0;
        uint64_t totalTicks = 0;
        uint64_t minTicks = ~uint64_t(0);
        uint64_t maxTicks = 0;
        void Reset() { calls = totalTicks = maxTicks = 0; minTicks = ~uint64_t(0); }
    };

    PerfV2Metric g_mMainPipeline;
    PerfV2Metric g_mGBuffer;
    PerfV2Metric g_mPostFx;
    PerfV2Metric g_mGpuHook;
    PerfV2Metric g_mGpuOriginal;
    bool g_perfV2Capture = false;
    uint64_t g_perfV2Start = 0;
    uint64_t g_perfV2Stop = 0;
    uint64_t g_perfV2GpuCalls = 0;
    uint64_t g_perfV2GpuBypassed = 0;
    uint64_t g_perfV2GpuOriginal = 0;

    uint64_t PerfV2Qpc()
    {
        LARGE_INTEGER q{};
        QueryPerformanceCounter(&q);
        return uint64_t(q.QuadPart);
    }

    uint64_t PerfV2FileTimeNow()
    {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER u{};
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        return u.QuadPart;
    }

    double PerfV2TicksToMs(uint64_t ticks)
    {
        if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
        return g_qpf.QuadPart ? (double(ticks) * 1000.0 / double(g_qpf.QuadPart)) : 0.0;
    }

    void PerfV2Record(PerfV2Metric& m, uint64_t begin)
    {
        const uint64_t elapsed = PerfV2Qpc() - begin;
        ++m.calls;
        m.totalTicks += elapsed;
        if (elapsed < m.minTicks) m.minTicks = elapsed;
        if (elapsed > m.maxTicks) m.maxTicks = elapsed;
    }

    void PerfV2Reset(bool start)
    {
        g_mMainPipeline.Reset();
        g_mGBuffer.Reset();
        g_mPostFx.Reset();
        g_mGpuHook.Reset();
        g_mGpuOriginal.Reset();
        g_perfV2GpuCalls = g_perfV2GpuBypassed = g_perfV2GpuOriginal = 0;
        g_perfV2Start = PerfV2Qpc();
        g_perfV2Stop = g_perfV2Start;
        g_perfV2Capture = start;
    }

    void PerfV2Stop()
    {
        if (g_perfV2Capture)
        {
            g_perfV2Stop = PerfV2Qpc();
            g_perfV2Capture = false;
        }
    }

    double PerfV2Seconds()
    {
        const uint64_t end = g_perfV2Capture ? PerfV2Qpc() : g_perfV2Stop;
        if (!g_perfV2Start || end <= g_perfV2Start || !g_qpf.QuadPart) return 0.0;
        return double(end - g_perfV2Start) / double(g_qpf.QuadPart);
    }

    void PerfV2AppendMetric(std::string& out, const char* name, uintptr_t addr, const PerfV2Metric& m, double seconds)
    {
        const double total = PerfV2TicksToMs(m.totalTicks);
        const double avg = m.calls ? total / double(m.calls) : 0.0;
        const double minv = m.calls ? PerfV2TicksToMs(m.minTicks) : 0.0;
        const double maxv = m.calls ? PerfV2TicksToMs(m.maxTicks) : 0.0;
        const double rate = seconds > 0.0 ? double(m.calls) / seconds : 0.0;
        const double pct = seconds > 0.0 ? total / (seconds * 10.0) : 0.0;
        char line[384];
        snprintf(line, sizeof line,
                 "  %s [0x%llX]: calls=%llu rate=%.3f/s total=%.3f ms avg=%.3f ms min=%.3f ms max=%.3f ms window=%.3f%%\n",
                 name, (unsigned long long)addr, (unsigned long long)m.calls, rate, total, avg, minv, maxv, pct);
        out += line;
    }

    std::string PerfV2Report()
    {
        const double seconds = PerfV2Seconds();
        char head[768];
        snprintf(head, sizeof head,
                 "Radiant+ Pipeline Report\nState: %s  Window: %.3f s\n"
                 "GPU-stat cache: enabled=%d interval=%d ms capture_calls=%llu cached=%llu original=%llu lifetime_calls=%llu lifetime_cached=%llu lifetime_original=%llu\n"
                 "Times are inclusive; nested rows overlap.\nRenderer pipeline\n",
                 g_perfV2Capture ? "RUNNING" : "STOPPED", seconds,
                 g_gpuStatsCache ? 1 : 0, g_gpuStatsMs,
                 (unsigned long long)g_perfV2GpuCalls, (unsigned long long)g_perfV2GpuBypassed, (unsigned long long)g_perfV2GpuOriginal,
                 (unsigned long long)g_gpuStatsCalls, (unsigned long long)g_gpuStatsBypassed, (unsigned long long)g_gpuStatsOriginal);
        std::string out = head;
        PerfV2AppendMetric(out, "Main lighting/render pipeline", kRenderMainPipeline, g_mMainPipeline, seconds);
        PerfV2AppendMetric(out, "GBuffer pipeline", kRenderGBuffer, g_mGBuffer, seconds);
        PerfV2AppendMetric(out, "PostFX pipeline", kRenderPostFx, g_mPostFx, seconds);
        out += "HUD GPU statistics\n";
        PerfV2AppendMetric(out, "GPU stats hook total", kGpuStatsQuery, g_mGpuHook, seconds);
        PerfV2AppendMetric(out, "GPU stats original/query path", kGpuStatsQuery, g_mGpuOriginal, seconds);
        return out;
    }

    void PerfV2ObserveProfileCommand(const std::string& c)
    {
        if (c.rfind("profile start", 0) == 0)
            PerfV2Reset(true);
        else if (c == "profile reset")
            PerfV2Reset(false);
        else if (c == "profile stop" || c == "profile report")
            PerfV2Stop();
    }

    __int64 __fastcall hk_GpuStats(__int64 state, __int64 process, uint64_t* systemStats, uint64_t* processStats)
    {
        ++g_gpuStatsCalls;
        if (g_perfV2Capture) ++g_perfV2GpuCalls;
        const bool capture = g_perfV2Capture;
        const uint64_t hookBegin = capture ? PerfV2Qpc() : 0;

        bool cached = false;
        if (g_gpuStatsCache && state && systemStats && processStats &&
            g_gpuLastState == state && g_gpuLastProcess == process)
        {
            unsigned char* s = reinterpret_cast<unsigned char*>(state);
            const uint64_t lastRadiantUpdate = *reinterpret_cast<const uint64_t*>(s + 16);
            const uint64_t nowFileTime = PerfV2FileTimeNow();
            const uint64_t interval100ns = uint64_t(g_gpuStatsMs) * 10000ULL;
            if (s[24] && s[25] && lastRadiantUpdate && nowFileTime >= lastRadiantUpdate &&
                nowFileTime - lastRadiantUpdate < interval100ns)
            {
                memcpy(systemStats, s + 56, 5 * sizeof(uint64_t));
                memcpy(processStats, s + 96, 5 * sizeof(uint64_t));
                cached = true;
            }
        }

        if (cached)
        {
            ++g_gpuStatsBypassed;
            if (capture)
            {
                ++g_perfV2GpuBypassed;
                PerfV2Record(g_mGpuHook, hookBegin);
            }
            return (__int64)processStats[4];
        }

        ++g_gpuStatsOriginal;
        if (capture) ++g_perfV2GpuOriginal;
        const uint64_t originalBegin = capture ? PerfV2Qpc() : 0;
        const __int64 r = g_oGpuStats(state, process, systemStats, processStats);
        if (state)
        {
            unsigned char* s = reinterpret_cast<unsigned char*>(state);
            if (s[24] && s[25])
            {
                g_gpuLastState = state;
                g_gpuLastProcess = process;
            }
        }
        if (capture)
        {
            PerfV2Record(g_mGpuOriginal, originalBegin);
            PerfV2Record(g_mGpuHook, hookBegin);
        }
        return r;
    }

    uint64_t* __fastcall hk_MainPipeline(unsigned char* state, __int64 debugEvents)
    {
        if (!g_perfV2Capture) return g_oMainPipeline(state, debugEvents);
        const uint64_t begin = PerfV2Qpc();
        uint64_t* r = g_oMainPipeline(state, debugEvents);
        PerfV2Record(g_mMainPipeline, begin);
        return r;
    }

    __int64 __fastcall hk_GBuffer(__int64 state, __int64 debugEvents)
    {
        if (!g_perfV2Capture) return g_oGBuffer(state, debugEvents);
        const uint64_t begin = PerfV2Qpc();
        const __int64 r = g_oGBuffer(state, debugEvents);
        PerfV2Record(g_mGBuffer, begin);
        return r;
    }

    __int64 __fastcall hk_PostFx(__int64 state, __int64 debugEvents)
    {
        if (!g_perfV2Capture) return g_oPostFx(state, debugEvents);
        const uint64_t begin = PerfV2Qpc();
        const __int64 r = g_oPostFx(state, debugEvents);
        PerfV2Record(g_mPostFx, begin);
        return r;
    }

    void SetupPerfV2()
    {
        QueryPerformanceFrequency(&g_qpf);
        bool gpu = false, main = false, gb = false, post = false;

        if (void* p = rp::host->RadiantAddress(rp::self, kGpuStatsQuery))
            gpu = rp::host->Detour(rp::self, p, reinterpret_cast<void*>(&hk_GpuStats), reinterpret_cast<void**>(&g_oGpuStats)) != 0;
        if (void* p = rp::host->RadiantAddress(rp::self, kRenderMainPipeline))
            main = rp::host->Detour(rp::self, p, reinterpret_cast<void*>(&hk_MainPipeline), reinterpret_cast<void**>(&g_oMainPipeline)) != 0;
        if (void* p = rp::host->RadiantAddress(rp::self, kRenderGBuffer))
            gb = rp::host->Detour(rp::self, p, reinterpret_cast<void*>(&hk_GBuffer), reinterpret_cast<void**>(&g_oGBuffer)) != 0;
        if (void* p = rp::host->RadiantAddress(rp::self, kRenderPostFx))
            post = rp::host->Detour(rp::self, p, reinterpret_cast<void*>(&hk_PostFx), reinterpret_cast<void**>(&g_oPostFx)) != 0;

        PerfV2Reset(false);
        rp::Log("perf-v2: gpu_stats_cache=%d interval_ms=%d hooks gpu=%d main=%d gbuffer=%d postfx=%d",
                g_gpuStatsCache ? 1 : 0, g_gpuStatsMs, gpu ? 1 : 0, main ? 1 : 0, gb ? 1 : 0, post ? 1 : 0);
    }
    // Adaptive interaction fast path for heavy maps: while the camera moves, skip the pass-2 per-object draw and turn off Update XY on drag.
    const uintptr_t kPerfV3BrushDispatch = 0x1401DC540;
    // Cam_Draw thunk used by the five Cam_ProcessAllBrushes call sites (some pass-2 paths bypass the lower dispatcher)
    const uintptr_t kPerfV31CamProcessAllBrushesThunk = 0x14006AB5E;
    using PerfV31CamProcessAllBrushesFn = __int64 (__fastcall *)(__int64 cam, int passMode, int flags);
    PerfV31CamProcessAllBrushesFn g_oPerfV31CamProcessAllBrushes = nullptr;
    uint64_t g_perfV31Mode2PassesBypassed = 0;
    // Turbo lighting: during an interactive frame only, use a simpler camera mode instead of 4 (real lighting), restored on scope exit
    bool g_perfV4TurboLighting = false;
    int  g_perfV4TurboMode = 1;
    uint64_t g_perfV4TurboFrames = 0;
    uint64_t g_perfV4ModeSwaps = 0;
    uint64_t g_perfV4ModeRestores = 0;
    uint64_t g_perfV4BakeBlocks = 0;
    uint64_t g_perfV4NonLitFrames = 0;
    bool PerfV4TurboModeValid(int mode);
    const uintptr_t kPerfV3UpdateXYOnDrag = 0x1453A1CAC;

    // [performance] xy_drag_hz: while a mouse button is held the 2D views (UpdateWindows bits 0x6) redraw at most this often,
    // the last request is kept and drawn when the interval passes or the button goes up. 0 = Radiant's own behaviour.
    int      g_xyDragHz = 10;
    int      g_xyDragShare = 20;        // [performance] xy_drag_share: max % of the drag time spent in 2D redraws
    int      g_xyFocusHz = 60;          // [performance] xy_focus_hz: the rate cap while the dragging cursor is in a 2D view
    double   g_xyDragLast2DMs = 0;      // cost of the last 2D redraw cycle (sum of QtXYRender::OnRender)
    bool     g_xyDragAccumReset = false; // set by hk_UpdateWindows on a 2D request, cleared by the next OnRender
    uint64_t g_xyDragLastDrawMs = 0;
    int      g_xyDragPending = 0;
    uint64_t g_xyDragThrottled = 0;   // 2D requests folded into a later frame
    uint64_t g_xyDragDeferredDraws = 0;  // deferred frames actually drawn

    uint64_t XyDragNowMs()
    {
        static LARGE_INTEGER freq = {};
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        return uint64_t(t.QuadPart * 1000 / freq.QuadPart);
    }

    void XyDragNote2DFrame(double ms)
    {
        if (g_xyDragAccumReset) { g_xyDragLast2DMs = 0; g_xyDragAccumReset = false; }
        g_xyDragLast2DMs += ms;
    }

    // Rewrites the UpdateWindows flags, called from hk_UpdateWindows
    int XyDragThrottle(int flags)
    {
        if (g_xyDragHz <= 0) return flags;
        const uint64_t now = XyDragNowMs();
        // interval = the rate cap, stretched so 2D frames take at most xy_drag_share % of the drag time
        uint64_t interval = 1000 / uint64_t(g_xyDragHz);
        int share = g_xyDragShare;
        const bool held = ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000) != 0;
        // a drag with the cursor in a 2D view gets xy_focus_hz and half of the time
        if (held && (flags & 0x6 || g_xyDragPending) && g_xyFocusHz > 0 && br::xylod::CursorOn2DView())
        {
            interval = 1000 / uint64_t(g_xyFocusHz);
            if (share > 0 && share < 50) share = 50;
        }
        if (share > 0 && share < 100)
        {
            const uint64_t byCost = uint64_t(g_xyDragLast2DMs * (100.0 - share) / share);
            if (byCost > interval) interval = byCost;
        }
        if (flags & 0x6)
        {
            if (held && now - g_xyDragLastDrawMs < interval)
            {
                g_xyDragPending |= flags & 0x6;
                flags &= ~0x6;
                ++g_xyDragThrottled;
            }
            else
            {
                g_xyDragPending = 0;
                g_xyDragLastDrawMs = now;
            }
        }
        else if (g_xyDragPending && (!held || now - g_xyDragLastDrawMs >= interval))
        {
            flags |= g_xyDragPending;
            g_xyDragPending = 0;
            g_xyDragLastDrawMs = now;
            ++g_xyDragDeferredDraws;
        }
        return flags;
    }

    using PerfV3BrushDispatchFn = char (__fastcall *)(unsigned int passMode, __int64 brushInst, __int64 a3,
                                                       __int64 a4, __int64 drawContext, char nested);

    PerfV3BrushDispatchFn g_oPerfV3BrushDispatch = nullptr;
    volatile unsigned char* g_perfV3UpdateXYOnDrag = nullptr;

    bool g_perfV3Enabled = true;
    bool g_perfV3SkipMode2 = false;
    bool g_perfV3SuppressXY = false;
    bool g_perfV3Force = false;
    bool g_perfV3Active = false;
    int  g_perfV3SettleMs = 150;
    uint64_t g_perfV3FastUntil = 0;
    bool g_perfV3XYSuppressed = false;
    unsigned char g_perfV3XYSaved = 0;
    uint64_t g_perfV3FastFrames = 0;
    uint64_t g_perfV3Mode2Bypassed = 0;
    uint64_t g_perfV3Transitions = 0;
    uint64_t g_perfV3XYSuppressions = 0;


    bool g_perfV7Enabled = true;
    bool g_perfV7StaticMode2 = false;         // diagnostic toggle
    bool g_perfV7DeferXYDuringCamera = false; // diagnostic toggle
    bool g_perfV7IdleReuse = false;            // diagnostic toggle
    int  g_perfV7IdleAfterMs = 175;
    int  g_perfV7IdleRefreshMs = 250;

    uint64_t g_perfV7StaticMode2Bypassed = 0;
    uint64_t g_perfV7XYFramesDeferred = 0;
    uint64_t g_perfV7Idle3DReused = 0;
    uint64_t g_perfV7Idle2DReused = 0;
    uint64_t g_perfV7Full3DFrames = 0;
    uint64_t g_perfV7Full2DFrames = 0;
    uint64_t g_perfV7LastFull3D = 0;
    uint64_t g_perfV7LastFull2D = 0;
    bool g_perfV7NeedFull3D = true;
    bool g_perfV7NeedFull2D = true;

    bool V6MapLoading(); // defined in the load block below

    DWORD PerfV7IdleForMs()
    {
        LASTINPUTINFO li{};
        li.cbSize = sizeof(li);
        if (!GetLastInputInfo(&li)) return 0;
        const DWORD now = GetTickCount();
        return now - li.dwTime; // wrap-safe
    }

    bool PerfV7CanIdleReuse()
    {
        if (!g_perfV7Enabled || !g_perfV7IdleReuse) return false;
        if (g_perfV3Active || g_perfV3Force) return false;
        if (V6MapLoading()) return false;
        return PerfV7IdleForMs() >= DWORD(g_perfV7IdleAfterMs);
    }

    void PerfV7ForceFreshViews()
    {
        g_perfV7NeedFull3D = true;
        g_perfV7NeedFull2D = true;
    }

    bool PerfV3KeyDown(int vk)
    {
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    }

    bool PerfV3RadiantForeground()
    {
        HWND fg = GetForegroundWindow();
        if (!fg) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        return pid == GetCurrentProcessId();
    }

    bool PerfV3CameraInputHeld()
    {
        if (!PerfV3RadiantForeground()) return false;
        if (PerfV3KeyDown(VK_RBUTTON) || PerfV3KeyDown(VK_MBUTTON)) return true;
        const int keys[] = { 'W', 'A', 'S', 'D', 'Q', 'E', VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT };
        for (int vk : keys) if (PerfV3KeyDown(vk)) return true;
        return false;
    }

    void PerfV3RestoreXY(bool requestRedraw)
    {
        if (!g_perfV3XYSuppressed) return;
        if (g_perfV3UpdateXYOnDrag) *g_perfV3UpdateXYOnDrag = g_perfV3XYSaved;
        g_perfV3XYSuppressed = false;
        if (requestRedraw && rp::host && rp::host->RequestRedraw)
            rp::host->RequestRedraw(rp::self, MODTOOLS_VIEW_2D);
    }

    void PerfV3ApplyAdaptiveState(bool active)
    {
        if (active == g_perfV3Active) return;
        g_perfV3Active = active;
        ++g_perfV3Transitions;
        PerfV7ForceFreshViews();
        if (active)
        {
            if (g_perfV3SuppressXY && g_perfV3UpdateXYOnDrag && !g_perfV3XYSuppressed && *g_perfV3UpdateXYOnDrag)
            {
                g_perfV3XYSaved = *g_perfV3UpdateXYOnDrag;
                *g_perfV3UpdateXYOnDrag = 0;
                g_perfV3XYSuppressed = true;
                ++g_perfV3XYSuppressions;
            }
        }
        else
        {
            PerfV3RestoreXY(true);
        }
    }

    void PerfV3PollInteraction()
    {
        const uint64_t now = GetTickCount64();
        if (!g_perfV3Enabled)
        {
            g_perfV3FastUntil = 0;
            PerfV3ApplyAdaptiveState(false);
            return;
        }
        if (g_perfV3Force || PerfV3CameraInputHeld())
            g_perfV3FastUntil = now + uint64_t(g_perfV3SettleMs);
        const bool active = g_perfV3Force || now < g_perfV3FastUntil;
        PerfV3ApplyAdaptiveState(active);
    }

    __int64 __fastcall hk_PerfV31CamProcessAllBrushes(__int64 cam, int passMode, int flags)
    {
        const bool interactiveBypass = g_perfV3Enabled && g_perfV3Active && g_perfV3SkipMode2;
        const bool stationaryBypass = g_perfV7Enabled && g_perfV7StaticMode2;
        if (passMode == 2 && (interactiveBypass || stationaryBypass))
        {
            ++g_perfV31Mode2PassesBypassed;
            if (stationaryBypass && !g_perfV3Active) ++g_perfV7StaticMode2Bypassed;
            return 0;
        }
        return g_oPerfV31CamProcessAllBrushes(cam, passMode, flags);
    }
    char __fastcall hk_PerfV3BrushDispatch(unsigned int passMode, __int64 brushInst, __int64 a3,
                                            __int64 a4, __int64 drawContext, char nested)
    {
        const bool interactiveBypass = g_perfV3Enabled && g_perfV3Active && g_perfV3SkipMode2;
        const bool stationaryBypass = g_perfV7Enabled && g_perfV7StaticMode2;
        if (passMode == 2 && (interactiveBypass || stationaryBypass))
        {
            ++g_perfV3Mode2Bypassed;
            return 0;
        }
        return g_oPerfV3BrushDispatch(passMode, brushInst, a3, a4, drawContext, nested);
    }

    void SetupPerfV3()
    {
        g_perfV3UpdateXYOnDrag = static_cast<volatile unsigned char*>(
            rp::host->RadiantAddress(rp::self, kPerfV3UpdateXYOnDrag));
        bool brushHook = false;
        if (void* p = rp::host->RadiantAddress(rp::self, kPerfV3BrushDispatch))
            brushHook = rp::host->Detour(rp::self, p, reinterpret_cast<void*>(&hk_PerfV3BrushDispatch),
                                         reinterpret_cast<void**>(&g_oPerfV3BrushDispatch)) != 0;
        bool mode2PassHook = false;
        if (void* p = rp::host->RadiantAddress(rp::self, kPerfV31CamProcessAllBrushesThunk))
            mode2PassHook = rp::host->Detour(rp::self, p, reinterpret_cast<void*>(&hk_PerfV31CamProcessAllBrushes),
                                             reinterpret_cast<void**>(&g_oPerfV31CamProcessAllBrushes)) != 0;
        rp::Log("xydrag: 2D views redraw at most %d times/s and take at most %d%% of the time while a mouse button is held (0 = off)", g_xyDragHz, g_xyDragShare);
        rp::Log("perf-v3.1: enabled=%d settle_ms=%d skip_mode2=%d suppress_xy=%d brush_hook=%d mode2_hook=%d xy_pref=%p",
                g_perfV3Enabled ? 1 : 0, g_perfV3SettleMs, g_perfV3SkipMode2 ? 1 : 0,
                g_perfV3SuppressXY ? 1 : 0, brushHook ? 1 : 0, mode2PassHook ? 1 : 0,
                const_cast<unsigned char*>(g_perfV3UpdateXYOnDrag));
    }

    void PerfV3Shutdown()
    {
        g_perfV3Force = false;
        g_perfV3FastUntil = 0;
        PerfV3ApplyAdaptiveState(false);
    }

    void PerfV3ResetCounters()
    {
        g_perfV3FastFrames = 0;
        g_perfV3Mode2Bypassed = 0;
        g_perfV31Mode2PassesBypassed = 0;
        g_perfV3Transitions = 0;
        g_perfV3XYSuppressions = 0;
    }

    std::string PerfV3Status()
    {
        char buf[1024];
        const int xyNow = g_perfV3UpdateXYOnDrag ? int(*g_perfV3UpdateXYOnDrag) : -1;
        snprintf(buf, sizeof buf,
                 "fast_mode=%d active=%d force=%d settle_ms=%d skip_mode2=%d brush_hook=%d mode2_hook=%d suppress_xy=%d xy_now=%d xy_suppressed=%d fast_frames=%llu mode2_bypassed=%llu mode2_passes_bypassed=%llu transitions=%llu xy_suppressions=%llu",
                 g_perfV3Enabled ? 1 : 0, g_perfV3Active ? 1 : 0, g_perfV3Force ? 1 : 0,
                 g_perfV3SettleMs, g_perfV3SkipMode2 ? 1 : 0, g_oPerfV3BrushDispatch ? 1 : 0,
                 g_oPerfV31CamProcessAllBrushes ? 1 : 0, g_perfV3SuppressXY ? 1 : 0, xyNow, g_perfV3XYSuppressed ? 1 : 0,
                 (unsigned long long)g_perfV3FastFrames, (unsigned long long)g_perfV3Mode2Bypassed,
                 (unsigned long long)g_perfV31Mode2PassesBypassed,
                 (unsigned long long)g_perfV3Transitions, (unsigned long long)g_perfV3XYSuppressions);
        return buf;
    }

    void PerfV7ResetCounters()
    {
        g_perfV7StaticMode2Bypassed = 0;
        g_perfV7XYFramesDeferred = 0;
        g_perfV7Idle3DReused = 0;
        g_perfV7Idle2DReused = 0;
        g_perfV7Full3DFrames = 0;
        g_perfV7Full2DFrames = 0;
    }

    std::string PerfV7Status()
    {
        char b[1024];
        snprintf(b, sizeof b,
                 "render7=%d fidelity=full static_mode2=%d defer_xy_camera=%d idle_reuse=%d idle_after_ms=%d refresh_ms=%d "
                 "static_mode2_bypassed=%llu xy_deferred=%llu idle3d_reused=%llu idle2d_reused=%llu "
                 "full3d=%llu full2d=%llu need_full3d=%d need_full2d=%d",
                 g_perfV7Enabled ? 1 : 0,
                 g_perfV7StaticMode2 ? 1 : 0,
                 g_perfV7DeferXYDuringCamera ? 1 : 0,
                 g_perfV7IdleReuse ? 1 : 0,
                 g_perfV7IdleAfterMs, g_perfV7IdleRefreshMs,
                 (unsigned long long)g_perfV7StaticMode2Bypassed,
                 (unsigned long long)g_perfV7XYFramesDeferred,
                 (unsigned long long)g_perfV7Idle3DReused,
                 (unsigned long long)g_perfV7Idle2DReused,
                 (unsigned long long)g_perfV7Full3DFrames,
                 (unsigned long long)g_perfV7Full2DFrames,
                 g_perfV7NeedFull3D ? 1 : 0,
                 g_perfV7NeedFull2D ? 1 : 0);
        return b;
    }

    // Deep renderer and load profiler: measures the pass dispatcher, draw counters, GeoCache pressure and map loading, rendering unchanged.
    const uintptr_t kDeepRenderPassDispatch = 0x1411BAF00;
    const uintptr_t kDeepSortA              = 0x1411CDBB0;
    const uintptr_t kDeepSortB              = 0x1411CDDE0;
    const uintptr_t kDeepGeoSubmit          = 0x1411E6670;
    const uintptr_t kDeepGeoGrow            = 0x141042190;
    const uintptr_t kDeepInstanceCache      = 0x1410985D0;
    const uintptr_t kDeepPrefabLoop         = 0x140C6C250;
    const uintptr_t kDeepMapParser          = 0x140CDB0C0;
    const uintptr_t kDeepAssetCacheOpen     = 0x140FA0300;
    const uintptr_t kDeepAssetCacheDecomp   = 0x140FA2FE0;


    const uintptr_t kDeepDipCounters        = 0x145B0DBF0; // low = DIP submissions, high = logical DIP work
    const uintptr_t kDeepDiipCounters       = 0x145B0DBF8; // low = DIIP submissions, high = instances
    const uintptr_t kDeepTriCounter         = 0x145B0DC00;

    using DeepRenderPassFn = __int64 (__fastcall *)(int passId, unsigned int context);
    using DeepSortFn = __int64 (__fastcall *)(__int64 begin, __int64 end, __int64 depth, unsigned char flag);
    using DeepGeoSubmitFn = __int64 (__fastcall *)(__int64 a1, __int64 a2, unsigned int a3);
    using DeepGeoGrowFn = char (__fastcall *)(__int64 cache, int addVerts, int addIndices);
    using DeepInstanceFn = __int64 (__fastcall *)(__int64 list, unsigned int first, unsigned int last);
    using DeepPrefabLoopFn = void (__fastcall *)(__int64 a1);
    using DeepMapParserFn = __int64 (__fastcall *)(__int64 a1, __int64 a2, char a3);
    using DeepAssetOpenFn = char (__fastcall *)(__int64 a1, __int64 a2, char a3);
    using DeepAssetDecompFn = __int64 (__fastcall *)(__int64 a1, void* file);

    DeepRenderPassFn g_oDeepRenderPass = nullptr;
    DeepSortFn g_oDeepSortA = nullptr;
    DeepSortFn g_oDeepSortB = nullptr;
    DeepGeoSubmitFn g_oDeepGeoSubmit = nullptr;
    DeepGeoGrowFn g_oDeepGeoGrow = nullptr;
    DeepInstanceFn g_oDeepInstance = nullptr;
    DeepPrefabLoopFn g_oDeepPrefabLoop = nullptr;
    DeepMapParserFn g_oDeepMapParser = nullptr;
    DeepAssetOpenFn g_oDeepAssetOpen = nullptr;
    DeepAssetDecompFn g_oDeepAssetDecomp = nullptr;

    // Submission and flush-cause census: counts only, rendering unchanged. Hooks the backend DrawIndexed and classifies calls from 0x1411E6670 by return address.
    const uintptr_t kCensusCoalesceRange = 0x1411E8920;
    const uintptr_t kCensusCoalesceOne   = 0x1411EC3A0;
    const uintptr_t kCensusGeoCopy       = 0x1410423C0;
    const uintptr_t kCensusDrawHud       = 0x1402488A0;
    const uintptr_t kCensusRendererInterfaceGlobal = 0x145574210;

    // Alternate submit wrapper: accepts only stage 5 or 6, then tail-submits to GeoSubmit without the normal dispatcher hook.
    const uintptr_t kV722SpecialStageSubmit = 0x1411BB0E0;

    // Stock GeoCache resolver (lookup, capacity, copy and hash insertion in one native call), used to pre-populate persistent meshes.
    const uintptr_t kV723GeoResolveMesh = 0x1411EC7C0;
    // Return address after the pass-dispatcher thunk call into GeoSubmit, one of the two cold-owner paths primed.
    const uintptr_t kV723NormalDispatcherGeoSubmitReturn = 0x1411BB020;

    // Return addresses after the ten direct DrawIndexed calls inside 0x1411E6670
    constexpr size_t kCensusFlushCauseCount = 10;
    const uintptr_t kCensusFlushReturnStatic[kCensusFlushCauseCount] = {
        0x1411E6816, // pending draw flushed because mesh/VB or vertex declaration changed
        0x1411E6AED, // GeoCache capacity path flushed pending draw before grow/reset/copy
        0x1411E6EA2, // primary geometry state tuple changed (VB/stride/base-state)
        0x1411E6F83, // secondary/index-buffer binding changed
        0x1411E703D, // surface resource/binding at +48 changed
        0x1411E70EC, // surface resource/binding at +56 changed
        0x1411E71A5, // surface resource set at +64 changed
        0x1411E735F, // pending direct range flushed before an instanced draw
        0x1411E7487, // next direct index range was not contiguous with accumulated range
        0x1411E753F  // end of current coalesce/submit range
    };
    const char* const kCensusFlushCauseName[kCensusFlushCauseCount] = {
        "mesh_or_vertex_decl",
        "geocache_capacity",
        "primary_geometry_state",
        "secondary_index_binding",
        "surface_binding_+48",
        "surface_binding_+56",
        "surface_resource_set_+64",
        "before_instanced_draw",
        "noncontiguous_index_range",
        "end_of_submit_range"
    };

    using CensusCoalesceRangeFn = __int64 (__fastcall *)(__int64 list, unsigned int first, unsigned int last);
    using CensusCoalesceOneFn = char (__fastcall *)(uint32_t* outBaseVertex, unsigned int* outBaseIndex,
                                                     __int64 vertices, int vertexCount, unsigned int vertexStride,
                                                     __int64 indices, int indexCount, char useBaseVertex, __int64 flushState);
    using CensusGeoCopyFn = unsigned int* (__fastcall *)(__int64 cache, __int64 mesh, uint32_t* outIndex, unsigned int* outVertex);
    using CensusDrawHudFn = __int64 (__fastcall *)(__int64 cam, __int64 a2, int width, int height);
    using CensusBackendDrawFn = __int64 (__fastcall *)(void* renderer, unsigned int indexCount,
                                                        unsigned int startIndex, unsigned int baseVertex);
    using V722SpecialStageFn = __int64 (__fastcall *)(unsigned int stage);
    using V723GeoResolveFn = __int64 (__fastcall *)(__int64 mesh, uintptr_t* outVB, uintptr_t* outIB,
                                                    uint32_t* outIndex, uint32_t* outVertex,
                                                    unsigned int*** flushState);

    CensusCoalesceRangeFn g_oCensusCoalesceRange = nullptr;
    CensusCoalesceOneFn g_oCensusCoalesceOne = nullptr;
    CensusGeoCopyFn g_oCensusGeoCopy = nullptr;
    CensusDrawHudFn g_oCensusDrawHud = nullptr;
    CensusBackendDrawFn g_oCensusBackendDraw = nullptr;
    V722SpecialStageFn g_oV722SpecialStage = nullptr;
    V723GeoResolveFn g_v723GeoResolve = nullptr;
    extern uint8_t* g_v6GeoCache; // defined in the load/cache block below
    void* g_censusBackendDrawTarget = nullptr;
    void* g_censusFlushReturnRuntime[kCensusFlushCauseCount] = {};
    bool g_censusBackendHookUnavailable = false;

    volatile uint32_t* g_deepDip = nullptr;
    volatile uint32_t* g_deepDiip = nullptr;
    volatile uint32_t* g_deepTris = nullptr;

    // Locality fallback (experimental, default off): reorders state-equivalent generic opaque GeoCache surfaces by (baseVertex,startIndex),
    // calls the stock 0x1411E6670 submitter, then restores Radiant's pointer order. Anything malformed falls back to stock order.
    const uintptr_t kV718GeoHashMask      = 0x14557FF78;
    const uintptr_t kV718GeoHashThreshold = 0x14557FF80;
    const uintptr_t kV718GeoHashBuckets   = 0x14557FF60;
    const uintptr_t kV718GeoHashSentinel  = 0x14557FF50;

    uint64_t* g_v718GeoHashMask = nullptr;
    uint64_t* g_v718GeoHashThreshold = nullptr;
    uintptr_t* g_v718GeoHashBuckets = nullptr;
    uintptr_t* g_v718GeoHashSentinel = nullptr;

    bool g_v718LocalityEnabled = false; // experimental: never persisted or auto-enabled
    thread_local int g_v718PassId = -1;
    thread_local unsigned int g_v718PassContext = 0xFFFFFFFFu;
    thread_local unsigned int g_v718PassDepth = 0;
    thread_local unsigned int g_v722SpecialStage = 0xFFFFFFFFu;
    thread_local unsigned int g_v722SpecialStageDepth = 0;
    thread_local int g_v722UnknownCallerSlot = -1;
    uintptr_t g_v722AddressBias = 0;

    // Learned cold-layout packer (experimental, default off): records mesh-hash order from a warm pass, then on the next reload
    // pre-resolves current persistent meshes in that order with Radiant's 0x1411EC7C0 resolver. Draw order is unchanged.
    enum class V723Target : int { OIT = 0, GBufferOpaque = 1, Prepass = 2, EmissiveOpaque = 3 };
    V723Target g_v723Target = V723Target::OIT;
    bool g_v723ColdPackEnabled = false;
    bool g_v723ColdPackArmed = false;
    bool g_v723LearnEnabled = false;
    bool g_v723LearnReady = false;
    thread_local bool g_v723Priming = false;
    std::unordered_map<uint32_t, uint32_t> g_v723LearnRank;
    uint64_t g_v723LearnSubmits = 0;
    uint64_t g_v723LearnSurfaces = 0;
    uint64_t g_v723LearnUnique = 0;
    uint64_t g_v723LearnNoNew = 0;
    uint64_t g_v723PrimeScans = 0;
    uint64_t g_v723PrimeLists = 0;
    uint64_t g_v723PrimeCandidates = 0;
    uint64_t g_v723PrimeInserted = 0;
    uint64_t g_v723PrimeRanked = 0;
    uint64_t g_v723PrimeUnranked = 0;
    uint64_t g_v723PrimeVertices = 0;
    uint64_t g_v723PrimeIndices = 0;
    uint64_t g_v723PrimeResolverFailures = 0;
    uint64_t g_v723PrimeExceptions = 0;
    uint64_t g_v723PrimeNoMissStreak = 0;
    uint64_t g_v723PrimePreMissStreak = 0;
    uint64_t g_v723PrimeAutoDisarms = 0;
    uint64_t g_v723PrimeAlreadyCached = 0;
    uint64_t g_v723PrimeOwnerSkips = 0;

    // Stable-geometry identity census: measures overlap with a content-derived geometry key, modifies nothing. mesh+132 is not a stable identity across reloads.
    struct V724GeomKey
    {
        uint64_t h = 0;
        uint32_t vertices = 0;
        uint32_t indices = 0;
        bool operator==(const V724GeomKey& o) const
        {
            return h == o.h && vertices == o.vertices && indices == o.indices;
        }
    };
    struct V724GeomKeyHash
    {
        size_t operator()(const V724GeomKey& k) const noexcept
        {
            uint64_t x = k.h ^ (uint64_t(k.vertices) << 32) ^ uint64_t(k.indices);
            x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
            x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
            x ^= x >> 33;
            return size_t(x);
        }
    };

    bool g_v724LearnEnabled = false;
    bool g_v724LearnReady = false;
    bool g_v724ProbeEnabled = false;
    uint64_t g_v724LearnNoNew = 0;
    uint64_t g_v724LearnSubmits = 0;
    uint64_t g_v724LearnSurfaces = 0;
    uint64_t g_v724LearnHashFails = 0;
    uint64_t g_v724ProbeSubmits = 0;
    uint64_t g_v724ProbeSurfaces = 0;
    uint64_t g_v724ProbeMatchedSurfaces = 0;
    uint64_t g_v724ProbeHashFails = 0;
    uint64_t g_v724BytesHashed = 0;
    uint64_t g_v724MeshCacheHits = 0;
    uint64_t g_v724MeshCacheMisses = 0;
    uint64_t g_v724ProbeNoNew = 0;
    uint64_t g_v724KeySceneEpoch = 0;
    std::unordered_map<V724GeomKey, uint32_t, V724GeomKeyHash> g_v724LearnRank;
    std::unordered_map<uintptr_t, V724GeomKey> g_v724MeshKeyCache;
    std::unordered_set<V724GeomKey, V724GeomKeyHash> g_v724ProbeSeenAll;
    std::unordered_set<V724GeomKey, V724GeomKeyHash> g_v724ProbeSeenStage5;
    std::unordered_set<V724GeomKey, V724GeomKeyHash> g_v724ProbeSeenDispatcher;
    uint64_t g_v724ProbeMatchedUniqueAll = 0;
    uint64_t g_v724ProbeMatchedUniqueStage5 = 0;
    uint64_t g_v724ProbeMatchedUniqueDispatcher = 0;

    std::atomic<uint64_t> g_v718GeoEpoch{1};
    std::atomic<uint64_t> g_v718SceneEpoch{1}; // coarse map-load generation, clears content variants on change

    struct V718StateKey
    {
        uintptr_t shaderOrMaterial = 0; // surface +0, compared before binding technique/decl
        uint32_t vertexDecl = 0;         // mesh +104
        uint32_t geometryState = 0;      // surface +24, part of the primary geometry tuple
        uint32_t auxState = 0;           // surface +28, conservative partition only
        uintptr_t binding48 = 0;         // surface +48
        uintptr_t binding56 = 0;         // surface +56

        bool operator==(const V718StateKey& o) const
        {
            return shaderOrMaterial == o.shaderOrMaterial && vertexDecl == o.vertexDecl &&
                   geometryState == o.geometryState && auxState == o.auxState &&
                   binding48 == o.binding48 && binding56 == o.binding56;
        }
    };

    struct V718Item
    {
        uintptr_t surface = 0;
        uint32_t baseVertex = 0;
        uint32_t startIndex = 0;
        uint32_t indexCount = 0;
        uint32_t originalSlot = 0;
    };

    struct V718Plan
    {
        uintptr_t list = 0;
        uint32_t count = 0;
        int passId = -1;
        unsigned int context = 0;
        unsigned int techType = 0;
        uint64_t geoEpoch = 0;
        uint64_t sceneEpoch = 0;
        std::vector<uintptr_t> original;
        std::vector<uintptr_t> reordered;
        uint64_t eligibleSurfaces = 0;
        uint64_t movedSurfaces = 0;
        uint64_t beneficialRuns = 0;
        uint64_t predictedBefore = 0;
        uint64_t predictedAfter = 0;
        bool beneficial = false;
        uint64_t fingerprint = 0;
    };

    // A1 is a renderer scratch-list address, not a stable identity: it holds several recurring pointer sequences.
    // A small set of exact content variants is cached per bucket, a bucket that keeps producing new contents is marked volatile and uses stock order.
    struct V720PlanBucket
    {
        uint64_t geoEpoch = 0;
        uint64_t sceneEpoch = 0;
        std::unordered_map<uint64_t, V718Plan> variants;
        bool volatileList = false;
    };

    constexpr size_t kV720MaxVariantsPerBucket = 8;
    std::unordered_map<uint64_t, V720PlanBucket> g_v720Buckets;
    uint64_t g_v720CacheGeoEpoch = 0;
    uint64_t g_v720CacheSceneEpoch = 0;
    uint64_t g_v718PlanBuilds = 0;
    uint64_t g_v718PlanRebuilds = 0; // additional content variants built
    uint64_t g_v718PlanCacheHits = 0;
    uint64_t g_v718PointerInvalidations = 0; // exact-hash collisions and corrupt matches only
    uint64_t g_v720VariantMisses = 0;
    uint64_t g_v720VolatileSkips = 0;
    uint64_t g_v720VolatileMarks = 0;
    uint64_t g_v720CacheClears = 0;
    uint64_t g_v720FingerprintPointers = 0;
    uint64_t g_v718PlanApplies = 0;
    uint64_t g_v718AppliedSurfaces = 0;
    uint64_t g_v718LookupMisses = 0;
    uint64_t g_v718ReadFailures = 0;
    uint64_t g_v718WriteFailures = 0;
    uint64_t g_v718UnsafePassSkips = 0;
    uint64_t g_v718NoBenefitPlans = 0;
    uint64_t g_v718EpochInvalidations = 0;

    bool V718Read64(uintptr_t address, uint64_t& out)
    {
        __try { out = *reinterpret_cast<const uint64_t*>(address); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { out = 0; return false; }
    }

    bool V718Read32(uintptr_t address, uint32_t& out)
    {
        __try { out = *reinterpret_cast<const uint32_t*>(address); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { out = 0; return false; }
    }

    bool V718CopyPointers(uintptr_t* dst, const uintptr_t* src, size_t count)
    {
        __try
        {
            if (count) memcpy(dst, src, count * sizeof(uintptr_t));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static inline uint64_t V718Mix(uint64_t h, uint64_t v)
    {
        h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
        h *= 0x100000001B3ULL;
        return h;
    }

    bool V718SafeOpaquePass(int passId, unsigned int context)
    {
        // camera pipeline labels, context 0: prepass stages 0/3/4, 1: GBuffer Opaque stage 1 (Semi Opaque excluded), 2: Emissive/Lit Forward/SSS Opaque 0/3/4
        if (context == 0) return passId == 0 || passId == 3 || passId == 4;
        if (context == 1) return passId == 1;
        if (context == 2) return passId == 0 || passId == 3 || passId == 4;
        return false;
    }

    bool V718ReadEligible(uintptr_t surface, V718StateKey& key, uintptr_t& mesh,
                          uint32_t& indexCount, uint32_t& meshHash)
    {
        if (!surface) return false;
        uint64_t u = 0;
        uint32_t d = 0;

        if (!V718Read64(surface + 0, u)) return false;
        key.shaderOrMaterial = uintptr_t(u);
        if (!key.shaderOrMaterial) return false;

        if (!V718Read64(surface + 8, u) || !u) return false;
        mesh = uintptr_t(u);

        // Only stock-persistent, non-instanced GeoCache geometry.
        if (!V718Read32(surface + 16, d) || d != 0) return false;
        if (!V718Read64(surface + 40, u) || u != 0) return false;
        if (!V718Read64(surface + 64, u) || u != 0) return false; // excludes siege/animated resource sets
        if (!V718Read64(mesh + 48, u) || u != 0) return false;   // direct/transient VB path excluded
        if (!V718Read32(mesh + 104, d) || d != 0) return false;  // VERTDECL_GENERIC only
        key.vertexDecl = d;
        if (!V718Read32(mesh + 124, d) || d != 0) return false;  // stock instancing count excluded

        if (!V718Read32(surface + 24, key.geometryState)) return false;
        if (!V718Read32(surface + 28, key.auxState)) return false;
        if (!V718Read64(surface + 48, u)) return false;
        key.binding48 = uintptr_t(u);
        if (!V718Read64(surface + 56, u)) return false;
        key.binding56 = uintptr_t(u);

        if (!V718Read32(mesh + 112, indexCount) || !indexCount) return false;
        if (!V718Read32(mesh + 132, meshHash)) return false;
        return true;
    }

    bool V718GeoLookup(uint32_t meshHash, uint32_t& startIndex, uint32_t& baseVertex)
    {
        if (!g_v718GeoHashMask || !g_v718GeoHashThreshold || !g_v718GeoHashBuckets || !g_v718GeoHashSentinel)
            return false;

        uint64_t mask = 0, threshold = 0, bucketBase = 0, sentinel = 0;
        if (!V718Read64(reinterpret_cast<uintptr_t>(g_v718GeoHashMask), mask) ||
            !V718Read64(reinterpret_cast<uintptr_t>(g_v718GeoHashThreshold), threshold) ||
            !V718Read64(reinterpret_cast<uintptr_t>(g_v718GeoHashBuckets), bucketBase) ||
            !V718Read64(reinterpret_cast<uintptr_t>(g_v718GeoHashSentinel), sentinel) ||
            !bucketBase || !sentinel)
            return false;

        uint64_t h = 0xCBF29CE484222325ULL;
        uint32_t k = meshHash;
        for (int b = 0; b < 4; ++b)
        {
            h ^= uint8_t(k >> (8 * b));
            h *= 0x100000001B3ULL;
        }
        uint64_t bucket = mask & (h ^ (h >> 32));
        if (threshold <= bucket)
            bucket += ~uint64_t(0) - (mask >> 1);

        uintptr_t bucketPtr = uintptr_t(bucketBase + 16ULL * bucket);
        uint64_t first = 0, endLink = 0;
        if (!V718Read64(bucketPtr, first)) return false;
        uint64_t end = sentinel;
        if (first != sentinel)
        {
            if (!V718Read64(bucketPtr + 8, endLink) || !endLink || !V718Read64(uintptr_t(endLink), end))
                return false;
        }

        uint64_t node = first;
        for (unsigned guard = 0; node && node != end && guard < 4096; ++guard)
        {
            uint32_t nodeHash = 0;
            if (!V718Read32(uintptr_t(node) + 16, nodeHash)) return false;
            if (nodeHash == meshHash)
            {
                if (!V718Read32(uintptr_t(node) + 20, startIndex) ||
                    !V718Read32(uintptr_t(node) + 24, baseVertex))
                    return false;
                return true;
            }
            uint64_t next = 0;
            if (!V718Read64(uintptr_t(node), next)) return false;
            node = next;
        }
        return false;
    }

    const char* V723TargetName()
    {
        switch (g_v723Target)
        {
            case V723Target::OIT: return "OIT c3/p9";
            case V723Target::GBufferOpaque: return "GBufferOpaque c1/p1";
            case V723Target::Prepass: return "Prepass c0/p0";
            case V723Target::EmissiveOpaque: return "EmissiveOpaque c2/p0";
            default: return "unknown";
        }
    }

    bool V723TargetMatches(int passId, unsigned int context)
    {
        switch (g_v723Target)
        {
            case V723Target::OIT: return context == 3 && passId == 9;
            case V723Target::GBufferOpaque: return context == 1 && passId == 1;
            case V723Target::Prepass: return context == 0 && passId == 0;
            case V723Target::EmissiveOpaque: return context == 2 && passId == 0;
            default: return false;
        }
    }

    void V723ResetRuntimeCounters(bool preserveLearned)
    {
        g_v723LearnSubmits = g_v723LearnSurfaces = g_v723LearnUnique = g_v723LearnNoNew = 0;
        g_v723PrimeScans = g_v723PrimeLists = g_v723PrimeCandidates = g_v723PrimeInserted = 0;
        g_v723PrimeRanked = g_v723PrimeUnranked = 0;
        g_v723PrimeVertices = g_v723PrimeIndices = 0;
        g_v723PrimeResolverFailures = g_v723PrimeExceptions = 0;
        g_v723PrimeNoMissStreak = g_v723PrimePreMissStreak = 0;
        g_v723PrimeAutoDisarms = g_v723PrimeAlreadyCached = g_v723PrimeOwnerSkips = 0;
        if (!preserveLearned)
        {
            g_v723LearnRank.clear();
            g_v723LearnReady = false;
            g_v723LearnEnabled = false;
        }
    }

    void V723ArmForColdLoad()
    {
        if (!g_v723ColdPackEnabled || g_v723LearnRank.empty() || !g_v723GeoResolve)
        {
            g_v723ColdPackArmed = false;
            return;
        }
        g_v723ColdPackArmed = true;
        g_v723PrimeNoMissStreak = 0;
        g_v723PrimePreMissStreak = 0;
    }

    void V723LearnFromList(uintptr_t list, uint32_t count)
    {
        if (!g_v723LearnEnabled || !list || count == 0 || count > 0x100000) return;
        ++g_v723LearnSubmits;
        uint64_t newRanks = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            uint64_t surface64 = 0;
            if (!V718Read64(list + uint64_t(i) * 8ULL, surface64)) continue;
            V718StateKey key{}; uintptr_t mesh = 0; uint32_t idx = 0, hash = 0;
            if (!V718ReadEligible(uintptr_t(surface64), key, mesh, idx, hash)) continue;
            ++g_v723LearnSurfaces;
            const uint32_t nextRank = uint32_t(g_v723LearnRank.size());
            if (g_v723LearnRank.emplace(hash, nextRank).second)
            {
                ++newRanks;
                ++g_v723LearnUnique;
            }
        }

        if (newRanks) g_v723LearnNoNew = 0;
        else ++g_v723LearnNoNew;
        // A camera pass can be submitted more than once per frame: sixteen submits with no new mesh hashes freeze the rank table.
        if (!g_v723LearnRank.empty() && g_v723LearnNoNew >= 16)
        {
            g_v723LearnEnabled = false;
            g_v723LearnReady = true;
        }
    }

    struct V723PrimeItem
    {
        uintptr_t mesh = 0;
        uint32_t hash = 0;
        uint32_t rank = 0xFFFFFFFFu;
        uint32_t vertices = 0;
        uint32_t indices = 0;
        uint32_t originalSlot = 0;
        V718StateKey state{};
    };

    static bool V723StateLess(const V718StateKey& a, const V718StateKey& b)
    {
        if (a.shaderOrMaterial != b.shaderOrMaterial) return a.shaderOrMaterial < b.shaderOrMaterial;
        if (a.binding48 != b.binding48) return a.binding48 < b.binding48;
        if (a.binding56 != b.binding56) return a.binding56 < b.binding56;
        if (a.geometryState != b.geometryState) return a.geometryState < b.geometryState;
        if (a.vertexDecl != b.vertexDecl) return a.vertexDecl < b.vertexDecl;
        return a.auxState < b.auxState;
    }

    bool V723GeoStructuresReady()
    {
        if (!g_v723GeoResolve || !g_v6GeoCache || !g_v718GeoHashBuckets || !g_v718GeoHashSentinel) return false;
        uint64_t buckets = 0, sentinel = 0;
        if (!V718Read64(reinterpret_cast<uintptr_t>(g_v718GeoHashBuckets), buckets) ||
            !V718Read64(reinterpret_cast<uintptr_t>(g_v718GeoHashSentinel), sentinel)) return false;
        return buckets != 0 && sentinel != 0;
    }

    bool V723ResolveSafe(uintptr_t mesh, uintptr_t* vb, uintptr_t* ib, uint32_t* start, uint32_t* base,
                         unsigned int*** flushArg)
    {
        __try
        {
            g_v723GeoResolve(__int64(mesh), vb, ib, start, base, flushArg);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ++g_v723PrimeExceptions;
            return false;
        }
    }

    // Defined later in the census block, declared here for the cold-owner gate.
    uintptr_t V722StaticAddress(void* p);

    bool V723ColdOwnerActive(uintptr_t geoSubmitReturn)
    {
        // Cold ownership: special stage 5 owns ~69.5% of first inserts, the pass dispatcher return at 0x1411BB020 ~30.5%, stage 6 none. Never prime from other GeoSubmit callers.
        if (g_v722SpecialStageDepth && g_v722SpecialStage == 5) return true;
        return V722StaticAddress(reinterpret_cast<void*>(geoSubmitReturn)) == kV723NormalDispatcherGeoSubmitReturn;
    }

    void V723PrimeList(uintptr_t list, uint32_t count)
    {
        if (!g_v723ColdPackEnabled || !g_v723ColdPackArmed || g_v723Priming ||
            g_v723LearnRank.empty() || !list || count == 0 || count > 0x100000 || !V723GeoStructuresReady())
            return;

        ++g_v723PrimeScans;
        std::vector<V723PrimeItem> items;
        items.reserve(std::min<uint32_t>(count, 4096u));
        std::unordered_set<uint32_t> seen;
        seen.reserve(std::min<uint32_t>(count, 8192u));

        for (uint32_t i = 0; i < count; ++i)
        {
            uint64_t surface64 = 0;
            if (!V718Read64(list + uint64_t(i) * 8ULL, surface64)) continue;
            V718StateKey key{}; uintptr_t mesh = 0; uint32_t idxCount = 0, hash = 0;
            if (!V718ReadEligible(uintptr_t(surface64), key, mesh, idxCount, hash)) continue;
            if (!seen.emplace(hash).second) continue;

            uint32_t start = 0, base = 0;
            if (V718GeoLookup(hash, start, base))
            {
                ++g_v723PrimeAlreadyCached;
                continue;
            }

            uint32_t vertexCount = 0;
            if (!V718Read32(mesh + 120, vertexCount) || !vertexCount || !idxCount) continue;
            V723PrimeItem it{};
            it.mesh = mesh; it.hash = hash; it.vertices = vertexCount; it.indices = idxCount;
            it.originalSlot = i; it.state = key;
            auto r = g_v723LearnRank.find(hash);
            if (r != g_v723LearnRank.end()) it.rank = r->second;
            items.push_back(it);
        }

        if (items.empty())
        {
            if (g_v723PrimeInserted)
            {
                if (++g_v723PrimeNoMissStreak >= 64)
                {
                    g_v723ColdPackArmed = false;
                    ++g_v723PrimeAutoDisarms;
                }
            }
            else if (++g_v723PrimePreMissStreak >= 256)
            {
                // Enabled on an already-warm map: stop scanning instead of paying permanent O(N) overhead.
                g_v723ColdPackArmed = false;
                ++g_v723PrimeAutoDisarms;
            }
            return;
        }

        g_v723PrimeNoMissStreak = 0;
        g_v723PrimePreMissStreak = 0;
        ++g_v723PrimeLists;
        g_v723PrimeCandidates += items.size();

        // Known target-pass ranks first, unseen meshes stay deterministic and state-grouped. Affects GeoCache placement only.
        std::stable_sort(items.begin(), items.end(), [](const V723PrimeItem& a, const V723PrimeItem& b) {
            const bool ak = a.rank != 0xFFFFFFFFu, bk = b.rank != 0xFFFFFFFFu;
            if (ak != bk) return ak > bk;
            if (ak && a.rank != b.rank) return a.rank < b.rank;
            if (V723StateLess(a.state, b.state)) return true;
            if (V723StateLess(b.state, a.state)) return false;
            if (a.hash != b.hash) return a.hash < b.hash;
            return a.originalSlot < b.originalSlot;
        });

        unsigned int pendingCount = 0, pendingStart = 0, pendingBase = 0;
        unsigned int* flushFields[3] = { &pendingCount, &pendingStart, &pendingBase };
        unsigned int** flushMiddle = flushFields;
        unsigned int*** flushArg = &flushMiddle;

        g_v723Priming = true;
        for (const V723PrimeItem& it : items)
        {
            uint32_t existingStart = 0, existingBase = 0;
            if (V718GeoLookup(it.hash, existingStart, existingBase)) continue;
            uintptr_t vb = 0, ib = 0;
            uint32_t start = 0, base = 0;
            const bool ok = V723ResolveSafe(it.mesh, &vb, &ib, &start, &base, flushArg);
            if (!ok)
            {
                ++g_v723PrimeResolverFailures;
                continue;
            }
            uint32_t verifyStart = 0, verifyBase = 0;
            if (V718GeoLookup(it.hash, verifyStart, verifyBase))
            {
                ++g_v723PrimeInserted;
                g_v723PrimeVertices += it.vertices;
                g_v723PrimeIndices += it.indices;
                if (it.rank != 0xFFFFFFFFu) ++g_v723PrimeRanked;
                else ++g_v723PrimeUnranked;
            }
            else
            {
                ++g_v723PrimeResolverFailures;
            }
        }
        g_v723Priming = false;
    }

    static inline uint64_t V724MixWord(uint64_t h, uint64_t x)
    {
        h ^= x + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
        h *= 0x100000001B3ULL;
        return h;
    }

    bool V724BuildGeomKeyRaw(uintptr_t mesh, V724GeomKey& out, uint64_t& bytesHashed)
    {
        __try
        {
            if (!mesh) return false;
            const uintptr_t vb = *reinterpret_cast<const uintptr_t*>(mesh + 0);
            const uintptr_t vbEnd = *reinterpret_cast<const uintptr_t*>(mesh + 8);
            const uintptr_t ib = *reinterpret_cast<const uintptr_t*>(mesh + 24);
            const uint32_t indices = *reinterpret_cast<const uint32_t*>(mesh + 112);
            const uint32_t vertices = *reinterpret_cast<const uint32_t*>(mesh + 120);
            const uint32_t decl = *reinterpret_cast<const uint32_t*>(mesh + 104);
            if (!vb || !vbEnd || vbEnd < vb || !ib || !vertices || !indices || decl != 0) return false;
            const uint64_t vbBytes = uint64_t(vbEnd - vb);
            const uint64_t expectedVb = uint64_t(vertices) * 44ULL;
            const uint64_t ibBytes = uint64_t(indices) * 2ULL;
            if (vbBytes != expectedVb || vbBytes > (256ULL << 20) || ibBytes > (128ULL << 20)) return false;

            uint64_t h = 0xCBF29CE484222325ULL;
            h = V724MixWord(h, vertices);
            h = V724MixWord(h, indices);
            h = V724MixWord(h, vbBytes);
            const uint8_t* vp = reinterpret_cast<const uint8_t*>(vb);
            uint64_t i = 0;
            for (; i + 8 <= vbBytes; i += 8)
            {
                uint64_t x = 0;
                memcpy(&x, vp + i, 8);
                h = V724MixWord(h, x);
            }
            if (i < vbBytes)
            {
                uint64_t tail = 0;
                memcpy(&tail, vp + i, size_t(vbBytes - i));
                h = V724MixWord(h, tail ^ (vbBytes - i));
            }

            const uint8_t* ip = reinterpret_cast<const uint8_t*>(ib);
            i = 0;
            for (; i + 8 <= ibBytes; i += 8)
            {
                uint64_t x = 0;
                memcpy(&x, ip + i, 8);
                h = V724MixWord(h, x);
            }
            if (i < ibBytes)
            {
                uint64_t tail = 0;
                memcpy(&tail, ip + i, size_t(ibBytes - i));
                h = V724MixWord(h, tail ^ ((ibBytes - i) << 8));
            }
            out.h = h;
            out.vertices = vertices;
            out.indices = indices;
            bytesHashed = vbBytes + ibBytes;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void V724RefreshSceneCache()
    {
        const uint64_t scene = g_v718SceneEpoch.load(std::memory_order_acquire);
        if (scene != g_v724KeySceneEpoch)
        {
            g_v724MeshKeyCache.clear();
            g_v724KeySceneEpoch = scene;
        }
    }

    bool V724GetGeomKey(uintptr_t mesh, V724GeomKey& out)
    {
        V724RefreshSceneCache();
        auto it = g_v724MeshKeyCache.find(mesh);
        if (it != g_v724MeshKeyCache.end())
        {
            ++g_v724MeshCacheHits;
            out = it->second;
            return true;
        }
        ++g_v724MeshCacheMisses;
        uint64_t bytes = 0;
        V724GeomKey k{};
        if (!V724BuildGeomKeyRaw(mesh, k, bytes)) return false;
        g_v724BytesHashed += bytes;
        g_v724MeshKeyCache.emplace(mesh, k);
        out = k;
        return true;
    }

    void V724Reset(bool preserveLearned)
    {
        g_v724ProbeEnabled = false;
        g_v724ProbeSubmits = g_v724ProbeSurfaces = g_v724ProbeMatchedSurfaces = 0;
        g_v724ProbeHashFails = g_v724ProbeMatchedUniqueAll = 0;
        g_v724ProbeMatchedUniqueStage5 = g_v724ProbeMatchedUniqueDispatcher = 0;
        g_v724ProbeNoNew = 0;
        g_v724ProbeSeenAll.clear();
        g_v724ProbeSeenStage5.clear();
        g_v724ProbeSeenDispatcher.clear();
        if (!preserveLearned)
        {
            g_v724LearnEnabled = false;
            g_v724LearnReady = false;
            g_v724LearnNoNew = g_v724LearnSubmits = g_v724LearnSurfaces = g_v724LearnHashFails = 0;
            g_v724LearnRank.clear();
        }
    }

    void V724LearnFromList(uintptr_t list, uint32_t count)
    {
        if (!g_v724LearnEnabled || !list || !count || count > 0x100000) return;
        ++g_v724LearnSubmits;
        uint64_t added = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            uint64_t surface64 = 0;
            if (!V718Read64(list + uint64_t(i) * 8ULL, surface64)) continue;
            V718StateKey state{}; uintptr_t mesh = 0; uint32_t idx = 0, stockHash = 0;
            if (!V718ReadEligible(uintptr_t(surface64), state, mesh, idx, stockHash)) continue;
            ++g_v724LearnSurfaces;
            V724GeomKey key{};
            if (!V724GetGeomKey(mesh, key)) { ++g_v724LearnHashFails; continue; }
            const uint32_t rank = uint32_t(g_v724LearnRank.size());
            if (g_v724LearnRank.emplace(key, rank).second) ++added;
        }
        if (added) g_v724LearnNoNew = 0; else ++g_v724LearnNoNew;
        if (!g_v724LearnRank.empty() && g_v724LearnNoNew >= 16)
        {
            g_v724LearnEnabled = false;
            g_v724LearnReady = true;
        }
    }

    void V724ProbeOwnerList(uintptr_t list, uint32_t count, bool stage5, bool dispatcher)
    {
        if (!g_v724ProbeEnabled || !g_v724LearnReady || !list || !count || count > 0x100000) return;
        ++g_v724ProbeSubmits;
        uint64_t newUnique = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            uint64_t surface64 = 0;
            if (!V718Read64(list + uint64_t(i) * 8ULL, surface64)) continue;
            V718StateKey state{}; uintptr_t mesh = 0; uint32_t idx = 0, stockHash = 0;
            if (!V718ReadEligible(uintptr_t(surface64), state, mesh, idx, stockHash)) continue;
            ++g_v724ProbeSurfaces;
            V724GeomKey key{};
            if (!V724GetGeomKey(mesh, key)) { ++g_v724ProbeHashFails; continue; }
            const bool matched = g_v724LearnRank.find(key) != g_v724LearnRank.end();
            if (matched) ++g_v724ProbeMatchedSurfaces;
            if (g_v724ProbeSeenAll.emplace(key).second)
            {
                ++newUnique;
                if (matched) ++g_v724ProbeMatchedUniqueAll;
            }
            if (stage5 && g_v724ProbeSeenStage5.emplace(key).second && matched)
                ++g_v724ProbeMatchedUniqueStage5;
            if (dispatcher && g_v724ProbeSeenDispatcher.emplace(key).second && matched)
                ++g_v724ProbeMatchedUniqueDispatcher;
        }
        if (newUnique) g_v724ProbeNoNew = 0; else ++g_v724ProbeNoNew;
        if (g_v724ProbeNoNew >= 128) g_v724ProbeEnabled = false;
    }

    std::string V724Status()
    {
        char b[2048];
        const double uniquePct = g_v724ProbeSeenAll.empty() ? 0.0 :
            100.0 * double(g_v724ProbeMatchedUniqueAll) / double(g_v724ProbeSeenAll.size());
        const double surfacePct = g_v724ProbeSurfaces ?
            100.0 * double(g_v724ProbeMatchedSurfaces) / double(g_v724ProbeSurfaces) : 0.0;
        snprintf(b, sizeof b,
                 "geomkey-v7.24 census_only=1 coldpack_retired=1 target=OIT c3/p9 learn_enabled=%d learn_ready=%d learned_keys=%llu\n"
                 "learn: submits=%llu eligible_surfaces=%llu hash_fail=%llu no_new_streak=%llu\n"
                 "probe: enabled=%d submits=%llu surfaces=%llu matched_surfaces=%llu (%.1f%%) unique=%llu matched_unique=%llu (%.1f%%) no_new_streak=%llu\n"
                 "owner_unique: stage5=%llu matched=%llu dispatcher=%llu matched=%llu\n"
                 "hashing: current_scene_cache=%llu cache_hits=%llu cache_misses=%llu bytes_hashed=%.2f MB hash_fail=%llu scene_epoch=%llu\n"
                 "safety: no_geocache_writes=1 no_resolver_calls=1 no_render_list_writes=1 no_draw_order_changes=1 no_state_changes=1\n",
                 g_v724LearnEnabled ? 1 : 0, g_v724LearnReady ? 1 : 0,
                 (unsigned long long)g_v724LearnRank.size(),
                 (unsigned long long)g_v724LearnSubmits, (unsigned long long)g_v724LearnSurfaces,
                 (unsigned long long)g_v724LearnHashFails, (unsigned long long)g_v724LearnNoNew,
                 g_v724ProbeEnabled ? 1 : 0, (unsigned long long)g_v724ProbeSubmits,
                 (unsigned long long)g_v724ProbeSurfaces, (unsigned long long)g_v724ProbeMatchedSurfaces, surfacePct,
                 (unsigned long long)g_v724ProbeSeenAll.size(), (unsigned long long)g_v724ProbeMatchedUniqueAll, uniquePct,
                 (unsigned long long)g_v724ProbeNoNew,
                 (unsigned long long)g_v724ProbeSeenStage5.size(), (unsigned long long)g_v724ProbeMatchedUniqueStage5,
                 (unsigned long long)g_v724ProbeSeenDispatcher.size(), (unsigned long long)g_v724ProbeMatchedUniqueDispatcher,
                 (unsigned long long)g_v724MeshKeyCache.size(), (unsigned long long)g_v724MeshCacheHits,
                 (unsigned long long)g_v724MeshCacheMisses, double(g_v724BytesHashed) / (1024.0 * 1024.0),
                 (unsigned long long)g_v724ProbeHashFails,
                 (unsigned long long)g_v718SceneEpoch.load(std::memory_order_acquire));
        return std::string(b);
    }

    std::string V723Status()
    {
        char b[2048];
        const bool ready = g_v723GeoResolve && g_v6GeoCache && g_oDeepGeoSubmit;
        snprintf(b, sizeof b,
                 "coldpack-v7.23 enabled=%d armed=%d ready=%d default_off=1 target=%s learn_enabled=%d learn_ready=%d learned_hashes=%llu\n"
                 "learn: submits=%llu eligible_surfaces=%llu unique=%llu no_new_streak=%llu\n"
                 "prime: scans=%llu lists_with_misses=%llu candidates=%llu inserted=%llu ranked=%llu unranked=%llu verts=%llu inds=%llu already_cached=%llu owner_skips=%llu\n"
                 "safety: stock_render_list_order=1 stock_draw_order=1 stock_state_order=1 stock_geocache_resolver=1 generic_persistent_only=1 current_mesh_pointers_only=1 cold_owners=stage5+normal_dispatcher instanced=stock animated=stock transient=stock same_process_reload_required=1 auto_disarm=1\n"
                 "failopen: resolver_fail=%llu exceptions=%llu auto_disarms=%llu no_miss_streak=%llu pre_miss_streak=%llu\n",
                 g_v723ColdPackEnabled ? 1 : 0, g_v723ColdPackArmed ? 1 : 0, ready ? 1 : 0,
                 V723TargetName(), g_v723LearnEnabled ? 1 : 0, g_v723LearnReady ? 1 : 0,
                 (unsigned long long)g_v723LearnRank.size(),
                 (unsigned long long)g_v723LearnSubmits, (unsigned long long)g_v723LearnSurfaces,
                 (unsigned long long)g_v723LearnUnique, (unsigned long long)g_v723LearnNoNew,
                 (unsigned long long)g_v723PrimeScans, (unsigned long long)g_v723PrimeLists,
                 (unsigned long long)g_v723PrimeCandidates, (unsigned long long)g_v723PrimeInserted,
                 (unsigned long long)g_v723PrimeRanked, (unsigned long long)g_v723PrimeUnranked,
                 (unsigned long long)g_v723PrimeVertices, (unsigned long long)g_v723PrimeIndices,
                 (unsigned long long)g_v723PrimeAlreadyCached, (unsigned long long)g_v723PrimeOwnerSkips,
                 (unsigned long long)g_v723PrimeResolverFailures, (unsigned long long)g_v723PrimeExceptions,
                 (unsigned long long)g_v723PrimeAutoDisarms, (unsigned long long)g_v723PrimeNoMissStreak,
                 (unsigned long long)g_v723PrimePreMissStreak);
        return std::string(b);
    }

    bool V718PointersEqual(uintptr_t list, const std::vector<uintptr_t>& expected)
    {
        __try
        {
            if (expected.empty()) return true;
            return memcmp(reinterpret_cast<const void*>(list), expected.data(),
                          expected.size() * sizeof(uintptr_t)) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool V720FingerprintList(uintptr_t list, uint32_t count, uint64_t& out)
    {
        __try
        {
            const uintptr_t* p = reinterpret_cast<const uintptr_t*>(list);
            uint64_t h = 0xCBF29CE484222325ULL ^ (uint64_t(count) * 0x9E3779B97F4A7C15ULL);
            for (uint32_t i = 0; i < count; ++i)
            {
                // Full pointer-array fingerprint (linear read, no surface dereferences), a hash hit still needs an exact memcmp.
                h = V718Mix(h, uint64_t(p[i]) ^ (uint64_t(i) * 0xD6E8FEB86659FD93ULL));
            }
            g_v720FingerprintPointers += count;
            out = h;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out = 0;
            ++g_v718ReadFailures;
            return false;
        }
    }

    uint64_t V718PlanKey(uintptr_t list, uint32_t count, int passId, unsigned int context, unsigned int techType)
    {
        uint64_t h = V718Mix(0x84222325CBF29CE4ULL, list);
        h = V718Mix(h, count);
        h = V718Mix(h, uint32_t(passId));
        h = V718Mix(h, context);
        return V718Mix(h, techType);
    }

    uint64_t V718CountDraws(const std::vector<V718Item>& items)
    {
        if (items.empty()) return 0;
        uint64_t draws = 1;
        uint32_t base = items[0].baseVertex;
        uint64_t end = uint64_t(items[0].startIndex) + items[0].indexCount;
        for (size_t i = 1; i < items.size(); ++i)
        {
            if (items[i].baseVertex != base || uint64_t(items[i].startIndex) != end)
            {
                ++draws;
                base = items[i].baseVertex;
            }
            end = uint64_t(items[i].startIndex) + items[i].indexCount;
        }
        return draws;
    }

    bool V718BuildPlan(V718Plan& plan, uintptr_t list, uint32_t count, int passId,
                       unsigned int context, unsigned int techType)
    {
        plan = V718Plan{};
        plan.list = list; plan.count = count; plan.passId = passId; plan.context = context;
        plan.techType = techType;
        plan.geoEpoch = g_v718GeoEpoch.load(std::memory_order_acquire);
        plan.sceneEpoch = g_v718SceneEpoch.load(std::memory_order_acquire);
        plan.original.resize(count);
        plan.reordered.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            uint64_t p = 0;
            if (!V718Read64(list + uint64_t(i) * 8ULL, p)) { ++g_v718ReadFailures; return false; }
            plan.original[i] = uintptr_t(p);
            plan.reordered[i] = uintptr_t(p);
        }

        uint32_t i = 0;
        while (i < count)
        {
            V718StateKey key{}; uintptr_t mesh = 0; uint32_t idxCount = 0, hash = 0;
            if (!V718ReadEligible(plan.original[i], key, mesh, idxCount, hash)) { ++i; continue; }

            uint32_t j = i + 1;
            for (; j < count; ++j)
            {
                V718StateKey k2{}; uintptr_t m2 = 0; uint32_t c2 = 0, h2 = 0;
                if (!V718ReadEligible(plan.original[j], k2, m2, c2, h2) || !(k2 == key)) break;
            }
            if (j - i < 2) { i = j; continue; }

            std::vector<V718Item> items;
            items.reserve(j - i);
            bool complete = true;
            for (uint32_t slot = i; slot < j; ++slot)
            {
                V718StateKey dummy{}; uintptr_t m = 0; uint32_t c = 0, mh = 0;
                if (!V718ReadEligible(plan.original[slot], dummy, m, c, mh)) { complete = false; break; }
                uint32_t start = 0, base = 0;
                if (!V718GeoLookup(mh, start, base)) { ++g_v718LookupMisses; complete = false; break; }
                V718Item it{}; it.surface = plan.original[slot]; it.baseVertex = base;
                it.startIndex = start; it.indexCount = c; it.originalSlot = slot;
                items.push_back(it);
            }
            if (!complete) { i = j; continue; }

            plan.eligibleSurfaces += items.size();
            const uint64_t before = V718CountDraws(items);
            std::stable_sort(items.begin(), items.end(), [](const V718Item& a, const V718Item& b) {
                if (a.baseVertex != b.baseVertex) return a.baseVertex < b.baseVertex;
                if (a.startIndex != b.startIndex) return a.startIndex < b.startIndex;
                return a.originalSlot < b.originalSlot;
            });
            const uint64_t after = V718CountDraws(items);
            plan.predictedBefore += before;
            plan.predictedAfter += after;
            if (after < before)
            {
                ++plan.beneficialRuns;
                for (uint32_t n = 0; n < uint32_t(items.size()); ++n)
                {
                    const uintptr_t p = items[n].surface;
                    if (plan.reordered[i + n] != p) ++plan.movedSurfaces;
                    plan.reordered[i + n] = p;
                }
            }
            i = j;
        }

        plan.beneficial = plan.predictedAfter < plan.predictedBefore && plan.movedSurfaces != 0;
        ++g_v718PlanBuilds;
        if (!plan.beneficial) ++g_v718NoBenefitPlans;
        return true;
    }

    V718Plan* V718GetPlan(uintptr_t list, uint32_t count, int passId, unsigned int context,
                          unsigned int techType)
    {
        if (!list || count < 2) return nullptr;

        const uint64_t geoEpoch = g_v718GeoEpoch.load(std::memory_order_acquire);
        const uint64_t sceneEpoch = g_v718SceneEpoch.load(std::memory_order_acquire);

        // Cache generations are global: clear once when the GeoCache layout or map generation changes, which also bounds memory.
        if (g_v720CacheGeoEpoch != geoEpoch || g_v720CacheSceneEpoch != sceneEpoch)
        {
            if (g_v720CacheGeoEpoch || g_v720CacheSceneEpoch)
            {
                ++g_v718EpochInvalidations;
                ++g_v720CacheClears;
            }
            g_v720Buckets.clear();
            g_v720CacheGeoEpoch = geoEpoch;
            g_v720CacheSceneEpoch = sceneEpoch;
        }

        const uint64_t baseKey = V718PlanKey(list, count, passId, context, techType);
        V720PlanBucket& bucket = g_v720Buckets[baseKey];
        bucket.geoEpoch = geoEpoch;
        bucket.sceneEpoch = sceneEpoch;

        if (bucket.volatileList)
        {
            ++g_v720VolatileSkips;
            return nullptr;
        }

        uint64_t fingerprint = 0;
        if (!V720FingerprintList(list, count, fingerprint))
            return nullptr;

        auto hit = bucket.variants.find(fingerprint);
        if (hit != bucket.variants.end())
        {
            V718Plan& p = hit->second;
            if (p.list == list && p.count == count && p.passId == passId && p.context == context &&
                p.techType == techType && p.geoEpoch == geoEpoch && p.sceneEpoch == sceneEpoch &&
                p.fingerprint == fingerprint && V718PointersEqual(list, p.original))
            {
                ++g_v718PlanCacheHits;
                return &p;
            }

            // Fingerprint matched but the pointer sequence did not: disable this bucket and stay stock.
            ++g_v718PointerInvalidations;
            bucket.volatileList = true;
            bucket.variants.clear();
            ++g_v720VolatileMarks;
            return nullptr;
        }

        ++g_v720VariantMisses;
        if (bucket.variants.size() >= kV720MaxVariantsPerBucket)
        {
            bucket.volatileList = true;
            bucket.variants.clear();
            ++g_v720VolatileMarks;
            ++g_v720VolatileSkips;
            return nullptr;
        }

        V718Plan p;
        if (!V718BuildPlan(p, list, count, passId, context, techType))
            return nullptr;
        p.fingerprint = fingerprint;
        p.geoEpoch = geoEpoch;
        p.sceneEpoch = sceneEpoch;

        if (!bucket.variants.empty()) ++g_v718PlanRebuilds;
        auto inserted = bucket.variants.emplace(fingerprint, std::move(p));
        return &inserted.first->second;
    }

    void V718Reset(bool clearPlans)
    {
        g_v718PlanBuilds = g_v718PlanRebuilds = g_v718PlanCacheHits = g_v718PointerInvalidations = 0;
        g_v720VariantMisses = g_v720VolatileSkips = g_v720VolatileMarks = 0;
        g_v720CacheClears = g_v720FingerprintPointers = 0;
        g_v718PlanApplies = g_v718AppliedSurfaces = 0;
        g_v718LookupMisses = g_v718ReadFailures = g_v718WriteFailures = 0;
        g_v718UnsafePassSkips = g_v718NoBenefitPlans = g_v718EpochInvalidations = 0;
        if (clearPlans)
        {
            g_v720Buckets.clear();
            g_v720CacheGeoEpoch = g_v718GeoEpoch.load(std::memory_order_acquire);
            g_v720CacheSceneEpoch = g_v718SceneEpoch.load(std::memory_order_acquire);
        }
    }

    std::string V718Status()
    {
        uint64_t surfaces = 0, moved = 0, runs = 0, before = 0, after = 0, beneficialPlans = 0;
        uint64_t variants = 0, volatileBuckets = 0;
        for (const auto& bk : g_v720Buckets)
        {
            const V720PlanBucket& bkt = bk.second;
            if (bkt.volatileList) ++volatileBuckets;
            variants += bkt.variants.size();
            for (const auto& kv : bkt.variants)
            {
                const V718Plan& p = kv.second;
                surfaces += p.eligibleSurfaces; moved += p.movedSurfaces; runs += p.beneficialRuns;
                before += p.predictedBefore; after += p.predictedAfter;
                if (p.beneficial) ++beneficialPlans;
            }
        }
        const bool ready = g_oDeepRenderPass && g_oDeepGeoSubmit && g_v718GeoHashMask &&
                           g_v718GeoHashThreshold && g_v718GeoHashBuckets && g_v718GeoHashSentinel;
        char b[2200];
        snprintf(b, sizeof b,
                 "drawlocality-v7.20 enabled=%d ready=%d default_off=1 buckets=%llu variants=%llu beneficial_variants=%llu builds=%llu extra_variant_builds=%llu variant_hits=%llu variant_misses=%llu applies=%llu applied_surfaces=%llu\n"
                 "variant_cache: max_variants_per_bucket=%llu volatile_buckets=%llu volatile_marks=%llu volatile_skips=%llu fingerprint_pointers=%llu exact_mismatch=%llu cache_clears=%llu\n"
                 "cached_variant_opportunity: eligible_surfaces=%llu moved=%llu beneficial_runs=%llu predicted_direct_DIP=%llu -> %llu (saved=%llu, %.1f%%)\n"
                 "safety: opaque_stages_only=1 generic_geocache_only=1 animated=stock instanced=stock transparent/decal/semiopaque/OIT=stock restore_original_order=1 bounded_volatile_fallback=1\n"
                 "failopen: lookup_miss=%llu read_fail=%llu write_fail=%llu unsafe_pass_skip=%llu epoch_invalidations=%llu no_benefit_plans=%llu geo_epoch=%llu scene_epoch=%llu",
                 g_v718LocalityEnabled ? 1 : 0, ready ? 1 : 0,
                 (unsigned long long)g_v720Buckets.size(), (unsigned long long)variants,
                 (unsigned long long)beneficialPlans,
                 (unsigned long long)g_v718PlanBuilds, (unsigned long long)g_v718PlanRebuilds,
                 (unsigned long long)g_v718PlanCacheHits, (unsigned long long)g_v720VariantMisses,
                 (unsigned long long)g_v718PlanApplies, (unsigned long long)g_v718AppliedSurfaces,
                 (unsigned long long)kV720MaxVariantsPerBucket, (unsigned long long)volatileBuckets,
                 (unsigned long long)g_v720VolatileMarks, (unsigned long long)g_v720VolatileSkips,
                 (unsigned long long)g_v720FingerprintPointers, (unsigned long long)g_v718PointerInvalidations,
                 (unsigned long long)g_v720CacheClears,
                 (unsigned long long)surfaces, (unsigned long long)moved, (unsigned long long)runs,
                 (unsigned long long)before, (unsigned long long)after,
                 (unsigned long long)(before >= after ? before - after : 0),
                 before ? (100.0 * double(before - after) / double(before)) : 0.0,
                 (unsigned long long)g_v718LookupMisses, (unsigned long long)g_v718ReadFailures,
                 (unsigned long long)g_v718WriteFailures, (unsigned long long)g_v718UnsafePassSkips,
                 (unsigned long long)g_v718EpochInvalidations, (unsigned long long)g_v718NoBenefitPlans,
                 (unsigned long long)g_v718GeoEpoch.load(std::memory_order_acquire),
                 (unsigned long long)g_v718SceneEpoch.load(std::memory_order_acquire));
        return b;
    }

    void SetupLocalityV718()
    {
        g_v718GeoHashMask = static_cast<uint64_t*>(rp::host->RadiantAddress(rp::self, kV718GeoHashMask));
        g_v718GeoHashThreshold = static_cast<uint64_t*>(rp::host->RadiantAddress(rp::self, kV718GeoHashThreshold));
        g_v718GeoHashBuckets = static_cast<uintptr_t*>(rp::host->RadiantAddress(rp::self, kV718GeoHashBuckets));
        g_v718GeoHashSentinel = static_cast<uintptr_t*>(rp::host->RadiantAddress(rp::self, kV718GeoHashSentinel));
        g_v718LocalityEnabled = false;
        V718Reset(true);
        rp::Log("fps-v7.21 draw-locality: bounded locality retired-by-evidence default OFF; pass/layout census active; geo globals mask=%p threshold=%p buckets=%p sentinel=%p",
                g_v718GeoHashMask, g_v718GeoHashThreshold, g_v718GeoHashBuckets, g_v718GeoHashSentinel);
    }

    struct DeepMetric
    {
        uint64_t calls = 0;
        uint64_t totalTicks = 0;
        uint64_t minTicks = ~uint64_t(0);
        uint64_t maxTicks = 0;
        void Reset() { calls = totalTicks = maxTicks = 0; minTicks = ~uint64_t(0); }
    };

    bool g_censusCapture = false;
    uint64_t g_censusStart = 0;
    uint64_t g_censusStop = 0;
    DeepMetric g_censusGeoSubmit;
    DeepMetric g_censusCoalesceRange;
    DeepMetric g_censusInstance;

    uint64_t g_censusSubmitLists = 0;
    uint64_t g_censusSubmitSurfaces = 0;
    uint64_t g_censusRangeCalls = 0;
    uint64_t g_censusRangeRequested = 0;
    uint64_t g_censusRangeProcessed = 0;
    uint64_t g_censusRangePartial = 0;

    uint64_t g_censusCoalesceCalls = 0;
    uint64_t g_censusCoalesceSuccess = 0;
    uint64_t g_censusCoalesceFail = 0;
    uint64_t g_censusCoalesceVertices = 0;
    uint64_t g_censusCoalesceIndices = 0;
    uint64_t g_censusCoalesceVertexBytes = 0;
    uint64_t g_censusCoalesceIndexBytes = 0;

    uint64_t g_censusGeoMissCopies = 0;
    uint64_t g_censusGeoMissVertices = 0;
    uint64_t g_censusGeoMissIndices = 0;
    uint64_t g_censusGeoMissBytes = 0;
    uint64_t g_censusGeoResetStart = 0;

    uint64_t g_censusInstanceRanges = 0;
    uint64_t g_censusInstanceRequested = 0;
    uint64_t g_censusInstanceProcessed = 0;
    uint64_t g_censusInstancePartial = 0;

    uint64_t g_censusFrames = 0;
    uint64_t g_censusDipPhysical = 0;
    uint64_t g_censusDipLogical = 0;
    uint64_t g_censusDiipPhysical = 0;
    uint64_t g_censusInstances = 0;
    uint64_t g_censusTriangles = 0;
    uint32_t g_censusMaxDipPhysical = 0;
    uint32_t g_censusMaxDipLogical = 0;
    uint64_t g_censusFramesOver10k = 0;
    uint64_t g_censusFramesOver30k = 0;
    uint64_t g_censusHudSnapshots = 0;
    uint64_t g_censusFallbackSnapshots = 0;
    bool g_censusLastSubmitDrawValid = false;
    thread_local unsigned int g_censusGeoSubmitDepth = 0;

    uint64_t g_censusBackendGeoDraws = 0;
    uint64_t g_censusBackendOtherDraws = 0;
    uint64_t g_censusBackendUnknownGeoDraws = 0;
    uint64_t g_censusFlushDraws[kCensusFlushCauseCount] = {};
    uint64_t g_censusFlushIndices[kCensusFlushCauseCount] = {};

    // Attribute the flush census to the active renderer pass (plain counters, no QPC or scene scan in the per-DIP hook).
    uint64_t g_censusPassGeoDraws[4][10] = {};
    uint64_t g_censusPassFlushDraws[4][10][kCensusFlushCauseCount] = {};
    uint64_t g_censusPassFlushIndices[4][10][kCensusFlushCauseCount] = {};
    uint64_t g_censusPassGeoMissCopies[4][10] = {};
    uint64_t g_censusPassGeoMissVertices[4][10] = {};
    uint64_t g_censusPassGeoMissIndices[4][10] = {};
    uint64_t g_censusUnknownPassGeoDraws = 0;
    uint64_t g_censusUnknownPassFlushDraws[kCensusFlushCauseCount] = {};
    uint64_t g_censusUnknownPassGeoMissCopies = 0;

    // Stage 5/6 attribution, index 0 = stage 5, index 1 = stage 6.
    uint64_t g_censusSpecialGeoDraws[2] = {};
    uint64_t g_censusSpecialFlushDraws[2][kCensusFlushCauseCount] = {};
    uint64_t g_censusSpecialGeoMissCopies[2] = {};
    uint64_t g_censusSpecialGeoMissVertices[2] = {};
    uint64_t g_censusSpecialGeoMissIndices[2] = {};

    // Cold submits outside both known contexts are bucketed by GeoSubmit caller return address (fixed size, no allocation, IDA static addresses).
    struct V722CallerBucket
    {
        uintptr_t caller = 0;
        uint64_t submits = 0;
        uint64_t surfaces = 0;
        uint64_t draws = 0;
        uint64_t noncontig = 0;
        uint64_t coldMisses = 0;
        uint64_t coldVerts = 0;
        uint64_t coldInds = 0;
    };
    constexpr int kV722CallerBucketCount = 32;
    V722CallerBucket g_censusUnknownCallers[kV722CallerBucketCount] = {};
    uint64_t g_censusUnknownCallerOverflow = 0;

    const char* DeepPassName(int p, int c); // defined below CensusReport

    struct DeepPassMetric
    {
        DeepMetric time;
        uint64_t dipCalls = 0;
        uint64_t dipLogical = 0;
        uint64_t diipCalls = 0;
        uint64_t instances = 0;
        uint64_t tris = 0;
        void Reset()
        {
            time.Reset();
            dipCalls = dipLogical = diipCalls = instances = tris = 0;
        }
    };

    struct DeepDrawSnapshot
    {
        uint32_t dipCalls = 0;
        uint32_t dipLogical = 0;
        uint32_t diipCalls = 0;
        uint32_t instances = 0;
        uint32_t tris = 0;
    };

    DeepDrawSnapshot g_censusLastSubmitDraw{};


    // ---- deep diagnostic state ----
    struct FpsDeepQueueMetric
    {
        uint64_t dispatches = 0;
        uint64_t groups = 0;
        uint64_t nonEmptyGroups = 0;
        uint64_t records = 0;
        uint64_t maxGroups = 0;
        uint64_t maxRecords = 0;
        void Reset() { dispatches = groups = nonEmptyGroups = records = maxGroups = maxRecords = 0; }
    };

    bool g_fpsDeepCapture = false;
    uint64_t g_fpsDeepStartMs = 0;
    uint64_t g_fpsDeepStopMs = 0;
    uint8_t* g_fpsDeepQueueBase = nullptr;
    FpsDeepQueueMetric g_fpsDeepQueue[10][4] = {};
    uint64_t g_fpsDeepQueueFaults = 0;

    uint64_t DeepNow(); // defined below

    // Queue compaction experiment, off by default and retired.
    bool g_perfV73CompactQueues = false; // retired: groups with empty +40 vectors can still render brushes and patches
    uint64_t g_perfV73CompactScans = 0;
    uint64_t g_perfV73CompactApplied = 0;
    uint64_t g_perfV73CompactUnsafe = 0;
    uint64_t g_perfV73CompactGroupsBefore = 0;
    uint64_t g_perfV73CompactGroupsAfter = 0;
    uint64_t g_perfV73CompactScanTicks = 0;
    thread_local std::vector<uint64_t> g_perfV73CompactScratch;
    thread_local bool g_perfV73CompactGuard = false;

    bool PerfV73GroupHasRecords(uint64_t group, bool& valid)
    {
        valid = false;
        if (!group) { valid = true; return false; }
        __try
        {
            const uint64_t vecObj = *reinterpret_cast<const uint64_t*>(group + 40);
            if (!vecObj) { valid = true; return false; }
            const uint64_t begin = *reinterpret_cast<const uint64_t*>(vecObj + 0);
            const uint64_t end   = *reinterpret_cast<const uint64_t*>(vecObj + 8);
            if (end < begin) return false;
            const uint64_t bytes = end - begin;
            if (bytes % 208u) return false;
            valid = true;
            return end > begin;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    struct PerfV73QueueSwap
    {
        uint8_t* q = nullptr;
        uint32_t originalCount = 0;
        uint64_t* originalPtr = nullptr;
        bool active = false;
    };

    bool PerfV73CompactBegin(int passId, PerfV73QueueSwap& swap)
    {
        // Retired: an empty +40 (208-byte record) vector is not a valid liveness test, such groups can still render brushes and patches.
        (void)passId;
        (void)swap;
        return false;
    }

    void PerfV73CompactEnd(PerfV73QueueSwap& swap)
    {
        (void)swap;
        g_perfV73CompactGuard = false;
    }

    void PerfV73CompactResetCounters()
    {
        g_perfV73CompactScans = g_perfV73CompactApplied = g_perfV73CompactUnsafe = 0;
        g_perfV73CompactGroupsBefore = g_perfV73CompactGroupsAfter = g_perfV73CompactScanTicks = 0;
    }

    std::string PerfV73CompactStatus()
    {
        return "compact=0 retired=1 reason=incomplete_group_semantics_brush_patch_loss";
    }

    bool g_deepCapture = false;
    uint64_t g_deepStart = 0;
    uint64_t g_deepStop = 0;
    DeepPassMetric g_deepPass[10][4] = {};
    DeepPassMetric g_deepUnknownPass = {};
    DeepMetric g_deepSortA;
    DeepMetric g_deepSortB;
    DeepMetric g_deepGeoSubmit;
    DeepMetric g_deepGeoGrow;
    DeepMetric g_deepInstance;
    DeepMetric g_deepPrefabLoop;
    DeepMetric g_deepMapParser;
    DeepMetric g_deepAssetOpen;
    DeepMetric g_deepAssetDecomp;
    DeepMetric g_v6MapLoadTime;

    uint64_t g_deepGeoGrowCalls = 0;
    uint64_t g_deepGeoGrowSuccess = 0;
    uint64_t g_deepGeoGrowFail = 0;       // false makes the caller take the GeoCache full-flush path
    uint64_t g_deepGeoResizeEvents = 0;
    uint32_t g_deepMaxVB = 0;
    uint32_t g_deepMaxIB = 0;
    uint32_t g_deepMaxUsedVB = 0;
    uint32_t g_deepMaxUsedIB = 0;
    uint32_t g_deepMaxAddVB = 0;
    uint32_t g_deepMaxAddIB = 0;

    uint64_t g_deepInstanceRanges = 0;
    uint64_t g_deepInstanceSurfacesRequested = 0;
    uint64_t g_deepInstanceSurfacesProcessed = 0;
    uint64_t g_deepInstancePartialRanges = 0;

    uint64_t g_deepMapParserEntities = 0;
    uint64_t g_deepAssetOpenSuccess = 0;
    uint64_t g_deepAssetOpenFail = 0;
    uint64_t g_deepDecompressedBytes = 0;

    uint64_t DeepNow()
    {
        LARGE_INTEGER q{};
        QueryPerformanceCounter(&q);
        return uint64_t(q.QuadPart);
    }

    // Load-time work: no intermediate redraws while Map_LoadFile runs, no asset-cache touch (reopen/SetFileTime) during loads,
    // decompressed asset blobs cached in RAM (size/write-time validated), GeoCache pre-grown while idle.
    const uintptr_t kV6MapLoadFile              = 0x1400532A6; // tiny jmp thunk -> Map_LoadFile 0x140CD1760
    const uintptr_t kV6AssetTimestampBranch     = 0x140FA052E; // JZ short -> timestamp reopen/SetFileTime block
    const uintptr_t kV6AssetBlobVtable          = 0x1426B3C98;
    const uintptr_t kV6GeoCacheObject           = 0x14557FEE0;
    const uintptr_t kV61GeoCacheResetThunk      = 0x14007EE6F; // jmp -> 0x1410425F0, clears GeoCache buffers/capacities

    // Learn the cache-asset request order from one load, then pre-read and decompress those .lz4 files on worker threads next time
    // (bounded transient ready queue, the decompressed working set is not kept resident).
    const uintptr_t kV63AssetCacheLocalRoot      = 0x14338F558; // std::string local asset-cache directory
    const uintptr_t kV63Lz4Decompress            = 0x140E920C0; // LZ4 block decompressor used by AssetCache.cpp
    using V63Lz4Fn = __int64 (__fastcall *)(unsigned char* src, char* dst, int srcSize, int dstSize);

    using V6MapLoadFn = __int64 (__fastcall *)(char* path);
    using V61GeoResetFn = __int64 (__fastcall *)(uint8_t* cache);
    V6MapLoadFn g_oV6MapLoad = nullptr;
    V61GeoResetFn g_oV61GeoReset = nullptr;

    bool g_v6SuppressLoadRendering = false;
    bool g_v6SkipLoadTimestamps = true;
    bool g_v6LoadPassOpen = false;       // main thread: OnMapLoading opened a filter cache pass that OnMapLoaded closes
    uint64_t g_v6TouchTailUntil = 0;     // main thread: touch skip ends at this tick (3 s after the load), 0 = no tail
    bool g_v6AssetRamEnabled = false;
    bool g_v6GeoPreallocate = false;
    int  g_v6AssetRamMB = 0; // 0 = auto from physical/available RAM
    int  g_v6AssetRamMaxEntryMB = 4;
    int  g_v61ResolvedAssetRamMB = 0;

    volatile LONG g_v6MapLoadDepth = 0;
    uint64_t g_v6MapLoads = 0;
    uint64_t g_v6RenderSuppress3D = 0;
    uint64_t g_v6RenderSuppress2D = 0;
    uint64_t g_v6TimestampSkips = 0;

    uint8_t* g_v6TimestampBranch = nullptr;
    uint8_t  g_v6TimestampOriginal = 0;
    bool     g_v6TimestampPatchReady = false;
    bool     g_v6TimestampPatched = false;

    uint8_t* g_v6GeoCache = nullptr;
    void*    g_v6AssetBlobVtable = nullptr;
    bool     g_v6RendererReady = false;
    bool     g_v6GeoPrimed = false;
    uint64_t g_v6GeoPrimeAttempts = 0;
    uint64_t g_v6GeoPrimeSuccess = 0;
    uint64_t g_v6GeoPrimeTicks = 0;
    uint64_t g_v61GeoResets = 0;
    uint64_t g_v61GeoResetPrimes = 0;

    // Persistent static index-run batcher: Radiant's 0x1411E6670 submitter flushes at 0x1411E7484 only because the next GeoCache index range is
    // non-contiguous (or crosses the 64K base-vertex page). Only those redundant flushes are suppressed, an immutable combined index buffer is cached
    // and drawn once at the next real state boundary. Everything dynamic or non-GeoCache stays stock.
    const uintptr_t kV8CurrentIndexBinding = 0x145B0CDE8;
    const size_t kV8MaxPendingPieces = 512;
    const size_t kV8MaxCacheEntries = 4096;
    const size_t kV8MaxSeenEntries = 16384;
    const uint64_t kV8MaxCacheBytes = 128ull * 1024ull * 1024ull;
    const uint32_t kV8MinPieces = 3;       // immutable-cache threshold
    const uint32_t kV8MinIndices = 96;     // immutable-cache threshold
    // R32 streaming arena with cross-page batching, for compatible runs not yet in an immutable cache entry.
    const uint32_t kV81StreamMinPieces = 2;
    const uint32_t kV81StreamMinIndices = 24;
    const uint32_t kV81StreamCapacityIndices = 4u * 1024u * 1024u; // 16 MiB R32 index arena

    struct V8DrawPiece
    {
        uint32_t indexCount = 0;
        uint32_t startIndex = 0;
        uint32_t baseVertex = 0;
        bool operator==(const V8DrawPiece& o) const noexcept
        {
            return indexCount == o.indexCount && startIndex == o.startIndex && baseVertex == o.baseVertex;
        }
    };

    struct V8BatchEntry
    {
        uint64_t hash = 0;
        uint64_t geoEpoch = 0;
        uint32_t baseVertex = 0;
        uint32_t indexCount = 0;
        uint32_t indexFormat = 57u; // DXGI_FORMAT_R16_UINT, 42 for cross-page R32 batches
        uint64_t bytes = 0;
        uint64_t lastUse = 0;
        ID3D11Buffer* buffer = nullptr;
        std::vector<V8DrawPiece> pieces;
    };

    bool g_v8StaticBatchEnabled = true;   // fail-open checks gate use
    bool g_v8Ready = false;
    bool g_v8CreateDisabled = false;
    void** g_v8CurrentIndexBindingGlobal = nullptr;
    uint32_t* g_v8PhysicalDipCounter = nullptr;
    uint64_t g_v8CacheEpoch = 0;
    uint64_t g_v8UseSerial = 0;
    uint64_t g_v8CacheBytes = 0;
    uint64_t g_v8CacheHits = 0;
    uint64_t g_v8CacheMisses = 0;
    uint64_t g_v8BatchBuilds = 0;
    uint64_t g_v8BuildFailures = 0;
    uint64_t g_v8BatchesIssued = 0;
    uint64_t g_v8StockDrawsSaved = 0;
    uint64_t g_v8StockReplays = 0;
    uint64_t g_v8BypassNonGeo = 0;
    uint64_t g_v8BypassSmall = 0;
    uint64_t g_v8BypassBaseChange = 0;
    uint64_t g_v8PendingOverflows = 0;
    uint64_t g_v8CounterAdjusts = 0;
    uint64_t g_v8CacheClears = 0;
    uint64_t g_v8SeenOverflow = 0;

    // Streaming fallback: one dynamic R32 index arena, NO_OVERWRITE advances through unused space and DISCARD is used only on wrap.
    ID3D11Buffer* g_v81StreamBuffer = nullptr;
    ID3D11DeviceContext* g_v81StreamContext = nullptr;
    uint32_t g_v81StreamCursor = 0;
    bool g_v81StreamHasWritten = false;
    bool g_v81StreamCreateDisabled = false;
    uint64_t g_v81StreamBatches = 0;
    uint64_t g_v81StreamPieces = 0;
    uint64_t g_v81StreamIndices = 0;
    uint64_t g_v81StreamBytes = 0;
    uint64_t g_v81StreamDrawsSaved = 0;
    uint64_t g_v81StreamWraps = 0;
    uint64_t g_v81StreamMapFailures = 0;
    uint64_t g_v81StreamCreateFailures = 0;
    uint64_t g_v81StreamFallbackReplays = 0;
    uint64_t g_v82CrossPageStreamBatches = 0;
    uint64_t g_v82CrossPageImmutableBuilds = 0;
    uint64_t g_v82CrossPagePieces = 0;

    std::unordered_map<uint64_t, std::vector<std::unique_ptr<V8BatchEntry>>> g_v8BatchCache;
    std::unordered_map<uint64_t, uint8_t> g_v8SeenCounts;

    thread_local unsigned int g_v8GeoSubmitDepth = 0;
    thread_local V8DrawPiece g_v8Pending[kV8MaxPendingPieces];
    thread_local size_t g_v8PendingCount = 0;
    thread_local void* g_v8PendingRenderer = nullptr;

    using V8BindIndexBufferFn = void (__fastcall *)(void* renderer, void* buffer, unsigned int format, unsigned int offset);

    void V8ReleaseEntry(V8BatchEntry& e)
    {
        if (e.buffer)
        {
            e.buffer->Release();
            e.buffer = nullptr;
        }
    }

    void V8ClearCache(bool clearSeen = true)
    {
        for (auto& kv : g_v8BatchCache)
            for (auto& e : kv.second)
                if (e) V8ReleaseEntry(*e);
        g_v8BatchCache.clear();
        if (clearSeen) g_v8SeenCounts.clear();
        g_v8CacheBytes = 0;
        g_v8CacheEpoch = g_v718GeoEpoch.load(std::memory_order_acquire);
        ++g_v8CacheClears;
        g_v8PendingCount = 0;
        g_v8PendingRenderer = nullptr;
    }

    uint64_t V8HashPieces(const V8DrawPiece* pieces, size_t count, uint64_t epoch)
    {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](uint64_t v)
        {
            for (int i = 0; i < 8; ++i)
            {
                h ^= uint8_t(v & 0xffu);
                h *= 1099511628211ull;
                v >>= 8;
            }
        };
        mix(epoch);
        mix(uint64_t(count));
        for (size_t i = 0; i < count; ++i)
        {
            mix(pieces[i].indexCount);
            mix(pieces[i].startIndex);
            mix(pieces[i].baseVertex);
        }
        return h;
    }

    bool V8PiecesEqual(const std::vector<V8DrawPiece>& a, const V8DrawPiece* b, size_t n)
    {
        if (a.size() != n) return false;
        for (size_t i = 0; i < n; ++i)
            if (!(a[i] == b[i])) return false;
        return true;
    }

    bool V8CopyGeoIndices(uint16_t* dst, const V8DrawPiece* pieces, size_t count, uint32_t& totalOut)
    {
        totalOut = 0;
        if (!dst || !pieces || !count || !g_v6GeoCache) return false;
        __try
        {
            const uint32_t usedIndices = *reinterpret_cast<const uint32_t*>(g_v6GeoCache + 92);
            const uint16_t* src = *reinterpret_cast<uint16_t* const*>(g_v6GeoCache + 56);
            if (!src) return false;
            for (size_t i = 0; i < count; ++i)
            {
                const V8DrawPiece& p = pieces[i];
                if (!p.indexCount || p.startIndex > usedIndices || p.indexCount > usedIndices - p.startIndex)
                    return false;
                memcpy(dst + totalOut, src + p.startIndex, size_t(p.indexCount) * sizeof(uint16_t));
                totalOut += p.indexCount;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool V82CopyGeoIndices32(uint32_t* dst, const V8DrawPiece* pieces, size_t count, uint32_t& totalOut)
    {
        totalOut = 0;
        if (!dst || !pieces || !count || !g_v6GeoCache) return false;
        __try
        {
            const uint32_t usedIndices = *reinterpret_cast<const uint32_t*>(g_v6GeoCache + 92);
            const uint16_t* src = *reinterpret_cast<uint16_t* const*>(g_v6GeoCache + 56);
            if (!src) return false;
            for (size_t i = 0; i < count; ++i)
            {
                const V8DrawPiece& p = pieces[i];
                if (!p.indexCount || p.startIndex > usedIndices || p.indexCount > usedIndices - p.startIndex)
                    return false;
                for (uint32_t j = 0; j < p.indexCount; ++j)
                {
                    const uint64_t absolute = uint64_t(p.baseVertex) + uint64_t(src[p.startIndex + j]);
                    if (absolute > 0xffffffffull) return false;
                    dst[totalOut++] = uint32_t(absolute);
                }
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool V8CreateIndexBufferRaw(const uint16_t* data, uint32_t count, ID3D11Buffer** out)
    {
        if (out) *out = nullptr;
        if (!data || !count || !out || !g_v6GeoCache) return false;
        __try
        {
            ID3D11Buffer* stock = *reinterpret_cast<ID3D11Buffer**>(g_v6GeoCache + 16);
            if (!stock) return false;
            ID3D11Device* device = nullptr;
            stock->GetDevice(&device);
            if (!device) return false;
            D3D11_BUFFER_DESC desc{};
            const uint64_t bytes64 = uint64_t(count) * sizeof(uint16_t);
            if (!bytes64 || bytes64 > 0xffffffffull)
            {
                device->Release();
                return false;
            }
            desc.ByteWidth = UINT(bytes64);
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
            D3D11_SUBRESOURCE_DATA init{};
            init.pSysMem = data;
            ID3D11Buffer* buffer = nullptr;
            const HRESULT hr = device->CreateBuffer(&desc, &init, &buffer);
            device->Release();
            if (FAILED(hr) || !buffer) return false;
            *out = buffer;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool V82CreateIndexBufferRaw32(const uint32_t* data, uint32_t count, ID3D11Buffer** out)
    {
        if (out) *out = nullptr;
        if (!data || !count || !out || !g_v6GeoCache) return false;
        __try
        {
            ID3D11Buffer* stock = *reinterpret_cast<ID3D11Buffer**>(g_v6GeoCache + 16);
            if (!stock) return false;
            ID3D11Device* device = nullptr;
            stock->GetDevice(&device);
            if (!device) return false;
            D3D11_BUFFER_DESC desc{};
            const uint64_t bytes64 = uint64_t(count) * sizeof(uint32_t);
            if (!bytes64 || bytes64 > 0xffffffffull)
            {
                device->Release();
                return false;
            }
            desc.ByteWidth = UINT(bytes64);
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
            D3D11_SUBRESOURCE_DATA init{};
            init.pSysMem = data;
            ID3D11Buffer* buffer = nullptr;
            const HRESULT hr = device->CreateBuffer(&desc, &init, &buffer);
            device->Release();
            if (FAILED(hr) || !buffer) return false;
            *out = buffer;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool V8BindIndexBuffer(void* renderer, void* buffer, unsigned int format = 57u)
    {
        if (!renderer || !buffer) return false;
        __try
        {
            void** vt = *reinterpret_cast<void***>(renderer);
            if (!vt || !vt[19]) return false; // +0x98, same stock binder as 0x1411E6670
            auto fn = reinterpret_cast<V8BindIndexBufferFn>(vt[19]);
            fn(renderer, buffer, format, 0u);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool V8CurrentBindingIsGeoCache(void*& stockIndexBuffer)
    {
        stockIndexBuffer = nullptr;
        if (!g_v8CurrentIndexBindingGlobal || !g_v6GeoCache) return false;
        __try
        {
            stockIndexBuffer = *reinterpret_cast<void**>(g_v6GeoCache + 16);
            const void* current = *g_v8CurrentIndexBindingGlobal;
            return stockIndexBuffer && current == stockIndexBuffer;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            stockIndexBuffer = nullptr;
            return false;
        }
    }

    void V81ReleaseStreamArena()
    {
        if (g_v81StreamBuffer)
        {
            g_v81StreamBuffer->Release();
            g_v81StreamBuffer = nullptr;
        }
        if (g_v81StreamContext)
        {
            g_v81StreamContext->Release();
            g_v81StreamContext = nullptr;
        }
        g_v81StreamCursor = 0;
        g_v81StreamHasWritten = false;
    }

    bool V81EnsureStreamArena(void* stockIndexBuffer)
    {
        if (g_v81StreamBuffer && g_v81StreamContext) return true;
        if (g_v81StreamCreateDisabled || !stockIndexBuffer) return false;
        __try
        {
            ID3D11Buffer* stock = reinterpret_cast<ID3D11Buffer*>(stockIndexBuffer);
            ID3D11Device* device = nullptr;
            stock->GetDevice(&device);
            if (!device) return false;

            ID3D11DeviceContext* context = nullptr;
            device->GetImmediateContext(&context);
            if (!context)
            {
                device->Release();
                ++g_v81StreamCreateFailures;
                g_v81StreamCreateDisabled = true;
                return false;
            }

            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = kV81StreamCapacityIndices * UINT(sizeof(uint32_t));
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            ID3D11Buffer* buffer = nullptr;
            const HRESULT hr = device->CreateBuffer(&desc, nullptr, &buffer);
            device->Release();
            if (FAILED(hr) || !buffer)
            {
                context->Release();
                ++g_v81StreamCreateFailures;
                g_v81StreamCreateDisabled = true;
                return false;
            }

            g_v81StreamBuffer = buffer;
            g_v81StreamContext = context;
            g_v81StreamCursor = 0;
            g_v81StreamHasWritten = false;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ++g_v81StreamCreateFailures;
            g_v81StreamCreateDisabled = true;
            V81ReleaseStreamArena();
            return false;
        }
    }

    bool V81StreamPieces(void* renderer, const V8DrawPiece* pieces, size_t count, void* stockIndexBuffer)
    {
        if (!renderer || !pieces || count < kV81StreamMinPieces || !stockIndexBuffer || !g_oCensusBackendDraw || !g_v6GeoCache)
            return false;

        uint64_t total64 = 0;
        bool crossPage = false;
        const uint32_t firstBase = pieces[0].baseVertex;
        for (size_t i = 0; i < count; ++i)
        {
            if (pieces[i].baseVertex != firstBase) crossPage = true;
            total64 += pieces[i].indexCount;
        }
        if (total64 < kV81StreamMinIndices || total64 > kV81StreamCapacityIndices)
            return false;
        const uint32_t total = uint32_t(total64);

        if (!V81EnsureStreamArena(stockIndexBuffer)) return false;

        D3D11_MAP mapType = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (!g_v81StreamHasWritten || g_v81StreamCursor > kV81StreamCapacityIndices - total)
        {
            mapType = D3D11_MAP_WRITE_DISCARD;
            g_v81StreamCursor = 0;
            if (g_v81StreamHasWritten) ++g_v81StreamWraps;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = E_FAIL;
        __try
        {
            hr = g_v81StreamContext->Map(g_v81StreamBuffer, 0, mapType, 0, &mapped);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            hr = E_FAIL;
        }
        if (FAILED(hr) || !mapped.pData)
        {
            ++g_v81StreamMapFailures;
            return false;
        }

        bool copiedOk = false;
        uint32_t copied = 0;
        __try
        {
            uint32_t* dst = reinterpret_cast<uint32_t*>(mapped.pData) + g_v81StreamCursor;
            copiedOk = V82CopyGeoIndices32(dst, pieces, count, copied) && copied == total;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            copiedOk = false;
        }
        g_v81StreamContext->Unmap(g_v81StreamBuffer, 0);
        if (!copiedOk) return false;

        const uint32_t start = g_v81StreamCursor;
        if (!V8BindIndexBuffer(renderer, g_v81StreamBuffer, 42u /* DXGI_FORMAT_R32_UINT */)) return false;
        g_oCensusBackendDraw(renderer, total, start, 0u); // absolute indices, stock vertex buffer stays bound
        if (!V8BindIndexBuffer(renderer, stockIndexBuffer, 57u /* DXGI_FORMAT_R16_UINT */))
        {
            g_v8StaticBatchEnabled = false;
            return false;
        }

        g_v81StreamCursor += total;
        g_v81StreamHasWritten = true;
        ++g_v81StreamBatches;
        g_v81StreamPieces += count;
        g_v81StreamIndices += total;
        g_v81StreamBytes += uint64_t(total) * sizeof(uint32_t);
        g_v81StreamDrawsSaved += uint64_t(count - 1);
        if (crossPage)
        {
            ++g_v82CrossPageStreamBatches;
            g_v82CrossPagePieces += count;
        }
        return true;
    }

    void V8AdjustPhysicalCounter(uint32_t subtract)
    {
        if (!subtract || !g_v8PhysicalDipCounter) return;
        __try
        {
            const uint32_t cur = *g_v8PhysicalDipCounter;
            if (cur >= subtract)
            {
                *g_v8PhysicalDipCounter = cur - subtract;
                ++g_v8CounterAdjusts;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    V8BatchEntry* V8FindBatch(uint64_t h, uint64_t epoch, const V8DrawPiece* pieces, size_t count)
    {
        auto it = g_v8BatchCache.find(h);
        if (it == g_v8BatchCache.end()) return nullptr;
        for (auto& ptr : it->second)
        {
            V8BatchEntry* e = ptr.get();
            if (e && e->geoEpoch == epoch && e->buffer && V8PiecesEqual(e->pieces, pieces, count))
                return e;
        }
        return nullptr;
    }

    V8BatchEntry* V8BuildBatch(uint64_t h, uint64_t epoch, const V8DrawPiece* pieces, size_t count)
    {
        if (g_v8CreateDisabled || !pieces || count < kV8MinPieces || !g_v6GeoCache) return nullptr;
        if (g_v8CacheBytes >= kV8MaxCacheBytes || g_v8BatchCache.size() >= kV8MaxCacheEntries)
            return nullptr;

        uint64_t total64 = 0;
        const uint32_t firstBase = pieces[0].baseVertex;
        bool sameBase = true;
        for (size_t i = 0; i < count; ++i)
        {
            if (pieces[i].baseVertex != firstBase) sameBase = false;
            total64 += pieces[i].indexCount;
        }
        if (total64 < kV8MinIndices || total64 > 0x3fffffffu) return nullptr;
        const uint32_t total = uint32_t(total64);

        ID3D11Buffer* buffer = nullptr;
        uint32_t batchBase = firstBase;
        uint32_t batchFormat = 57u; // R16
        uint64_t bytes = uint64_t(total) * sizeof(uint16_t);
        if (sameBase)
        {
            std::vector<uint16_t> merged(total);
            uint32_t copied = 0;
            if (!V8CopyGeoIndices(merged.data(), pieces, count, copied) || copied != total)
            {
                ++g_v8BuildFailures;
                return nullptr;
            }
            if (!V8CreateIndexBufferRaw(merged.data(), total, &buffer) || !buffer)
            {
                ++g_v8BuildFailures;
                g_v8CreateDisabled = true;
                return nullptr;
            }
        }
        else
        {
            std::vector<uint32_t> merged(total);
            uint32_t copied = 0;
            if (!V82CopyGeoIndices32(merged.data(), pieces, count, copied) || copied != total)
            {
                ++g_v8BuildFailures;
                return nullptr;
            }
            if (!V82CreateIndexBufferRaw32(merged.data(), total, &buffer) || !buffer)
            {
                ++g_v8BuildFailures;
                g_v8CreateDisabled = true;
                return nullptr;
            }
            batchBase = 0u;
            batchFormat = 42u; // DXGI_FORMAT_R32_UINT
            bytes = uint64_t(total) * sizeof(uint32_t);
            ++g_v82CrossPageImmutableBuilds;
        }

        auto e = std::make_unique<V8BatchEntry>();
        e->hash = h;
        e->geoEpoch = epoch;
        e->baseVertex = batchBase;
        e->indexCount = total;
        e->indexFormat = batchFormat;
        e->bytes = bytes;
        e->lastUse = ++g_v8UseSerial;
        e->buffer = buffer;
        e->pieces.assign(pieces, pieces + count);
        V8BatchEntry* raw = e.get();
        g_v8CacheBytes += e->bytes;
        g_v8BatchCache[h].push_back(std::move(e));
        ++g_v8BatchBuilds;
        return raw;
    }

    bool V8ReplayPieces(void* renderer, const V8DrawPiece* pieces, size_t count)
    {
        if (!g_oCensusBackendDraw || !renderer) return false;
        for (size_t i = 0; i < count; ++i)
            g_oCensusBackendDraw(renderer, pieces[i].indexCount, pieces[i].startIndex, pieces[i].baseVertex);
        g_v8StockReplays += count;
        return true;
    }

    bool V8IssueBatch(void* renderer, V8BatchEntry* e, void* stockIndexBuffer)
    {
        if (!renderer || !e || !e->buffer || !stockIndexBuffer || !g_oCensusBackendDraw) return false;
        if (!V8BindIndexBuffer(renderer, e->buffer, e->indexFormat)) return false;
        g_oCensusBackendDraw(renderer, e->indexCount, 0u, e->baseVertex);
        const bool restored = V8BindIndexBuffer(renderer, stockIndexBuffer, 57u /* DXGI_FORMAT_R16_UINT */);
        if (!restored)
        {
            g_v8StaticBatchEnabled = false; // fail open on later calls
            return false;
        }
        e->lastUse = ++g_v8UseSerial;
        ++g_v8BatchesIssued;
        return true;
    }

    // Flushes suppressed pieces. currentWillIncrement: the running caller has not yet bumped Radiant's physical DIP counter.
    bool V8FlushPending(void* renderer, bool currentWillIncrement, void* stockIndexBuffer)
    {
        if (!g_v8PendingCount) return true;
        const size_t n = g_v8PendingCount;
        const uint64_t epoch = g_v718GeoEpoch.load(std::memory_order_acquire);
        if (g_v8CacheEpoch != epoch)
            V8ClearCache(true);

        if (n < kV8MinPieces)
        {
            ++g_v8BypassSmall;
            bool ok = false;
            if (n >= kV81StreamMinPieces)
                ok = V81StreamPieces(renderer, g_v8Pending, n, stockIndexBuffer);
            if (!ok)
            {
                ++g_v81StreamFallbackReplays;
                ok = V8ReplayPieces(renderer, g_v8Pending, n);
            }
            else
            {
                V8AdjustPhysicalCounter(uint32_t(n - 1));
                g_v8StockDrawsSaved += uint64_t(n - 1);
            }
            g_v8PendingCount = 0;
            return ok;
        }

        const uint64_t h = V8HashPieces(g_v8Pending, n, epoch);
        V8BatchEntry* e = V8FindBatch(h, epoch, g_v8Pending, n);
        if (e)
        {
            ++g_v8CacheHits;
        }
        else
        {
            ++g_v8CacheMisses;
            uint8_t seen = 0;
            auto it = g_v8SeenCounts.find(h);
            if (it != g_v8SeenCounts.end()) seen = it->second;
            if (seen < 255)
            {
                if (it != g_v8SeenCounts.end()) it->second = uint8_t(seen + 1);
                else if (g_v8SeenCounts.size() < kV8MaxSeenEntries) g_v8SeenCounts.emplace(h, 1u);
                else ++g_v8SeenOverflow;
            }
            // Build only after the exact run repeats, unique camera-dependent runs stay stock.
            if (seen >= 1)
                e = V8BuildBatch(h, epoch, g_v8Pending, n);
        }

        if (!e)
        {
            bool ok = V81StreamPieces(renderer, g_v8Pending, n, stockIndexBuffer);
            if (ok)
            {
                V8AdjustPhysicalCounter(uint32_t(n - 1));
                g_v8StockDrawsSaved += uint64_t(n - 1);
            }
            else
            {
                ++g_v81StreamFallbackReplays;
                ok = V8ReplayPieces(renderer, g_v8Pending, n);
            }
            g_v8PendingCount = 0;
            return ok;
        }

        if (!V8IssueBatch(renderer, e, stockIndexBuffer))
        {
            ++g_v8BuildFailures;
            const bool ok = V8ReplayPieces(renderer, g_v8Pending, n);
            g_v8PendingCount = 0;
            return ok;
        }

        // N counted stock draws become one real draw, so subtract N-1 from the host's DIP counter.
        const uint32_t subtract = currentWillIncrement ? uint32_t(n - 1) : uint32_t(n - 1);
        V8AdjustPhysicalCounter(subtract);
        g_v8StockDrawsSaved += uint64_t(n - 1);
        g_v8PendingCount = 0;
        return true;
    }

    bool V8ResolveReady()
    {
        if (!g_v8CurrentIndexBindingGlobal)
            g_v8CurrentIndexBindingGlobal = static_cast<void**>(rp::host->RadiantAddress(rp::self, kV8CurrentIndexBinding));
        if (!g_v8PhysicalDipCounter)
            g_v8PhysicalDipCounter = static_cast<uint32_t*>(rp::host->RadiantAddress(rp::self, kDeepDipCounters));
        g_v8Ready = g_v6GeoCache && g_v8CurrentIndexBindingGlobal && g_oCensusBackendDraw;
        if (g_v8Ready && !g_v8CacheEpoch)
            g_v8CacheEpoch = g_v718GeoEpoch.load(std::memory_order_acquire);
        return g_v8Ready;
    }

    std::string V8Status()
    {
        std::ostringstream out;
        const uint64_t entries = [&]()
        {
            uint64_t n = 0;
            for (const auto& kv : g_v8BatchCache) n += kv.second.size();
            return n;
        }();
        out << "staticbatch-v8.2 enabled=" << (g_v8StaticBatchEnabled ? 1 : 0)
            << " ready=" << (g_v8Ready ? 1 : 0)
            << " create_disabled=" << (g_v8CreateDisabled ? 1 : 0)
            << " cache_entries=" << entries
            << " cache_mb=" << std::fixed << std::setprecision(2) << (double(g_v8CacheBytes) / (1024.0 * 1024.0)) << "\n";
        out << "reuse: hits=" << g_v8CacheHits << " misses=" << g_v8CacheMisses
            << " builds=" << g_v8BatchBuilds << " build_fail=" << g_v8BuildFailures
            << " batches=" << g_v8BatchesIssued << " stock_draws_saved=" << g_v8StockDrawsSaved << "\n";
        out << "fallback: stock_replays=" << g_v8StockReplays << " non_geocache=" << g_v8BypassNonGeo
            << " small=" << g_v8BypassSmall << " base_page_splits=" << g_v8BypassBaseChange
            << " pending_overflow=" << g_v8PendingOverflows << " seen_overflow=" << g_v8SeenOverflow << "\n";
        out << "stream-v8.2-r32: arena_mb=" << std::fixed << std::setprecision(1)
            << (double(kV81StreamCapacityIndices * sizeof(uint32_t)) / (1024.0 * 1024.0))
            << " batches=" << g_v81StreamBatches << " pieces=" << g_v81StreamPieces
            << " uploaded_mb=" << std::setprecision(2) << (double(g_v81StreamBytes) / (1024.0 * 1024.0))
            << " draws_saved=" << g_v81StreamDrawsSaved << " wraps=" << g_v81StreamWraps
            << " map_fail=" << g_v81StreamMapFailures << " create_fail=" << g_v81StreamCreateFailures
            << " fallback_replays=" << g_v81StreamFallbackReplays << " cross_page_batches=" << g_v82CrossPageStreamBatches << " cross_page_pieces=" << g_v82CrossPagePieces << " immutable_r32_builds=" << g_v82CrossPageImmutableBuilds << "\n";
        out << "fidelity: stock_vertex_buffer=1 stock_render_order=1 stock_state_order=1 stock_visibility=1 stock_lighting=1 "
               "persistent_immutable_r16_r32_batches=1 streaming_r32_index_arena=1 cross_geocache_pages=1 dynamic_transient_stock=1";
        return out.str();
    }

    using V61AssetLru = std::list<std::string>;
    struct V6AssetRamEntry
    {
        std::shared_ptr<std::vector<unsigned char>> bytes;
        uint64_t compressedSize = 0;
        uint64_t writeTime = 0;
        V61AssetLru::iterator lru;
    };

    std::unordered_map<std::string, V6AssetRamEntry> g_v6AssetRam;
    V61AssetLru g_v6AssetRamOrder;
    std::mutex g_v6AssetRamMutex;
    uint64_t g_v6AssetRamBytes = 0;
    uint64_t g_v6AssetRamHits = 0;
    uint64_t g_v6AssetRamMisses = 0;
    uint64_t g_v6AssetRamStores = 0;
    uint64_t g_v6AssetRamEvictions = 0;
    uint64_t g_v6AssetRamBytesServed = 0;
    uint64_t g_v6AssetRamBytesStored = 0;
    thread_local std::string g_v6AssetKey;


    struct V63PlanEntry
    {
        std::string key;
        uint32_t expectedUses = 1;
    };

    struct V63ReadyBlob
    {
        std::shared_ptr<std::vector<unsigned char>> bytes;
        uint32_t remainingUses = 1;
        size_t planIndex = 0;
    };

    bool g_v63PrefetchEnabled = true;
    int  g_v63PrefetchWorkers = 1;
    int  g_v63PrefetchMB = 64;
    int  g_v63PrefetchMaxEntryMB = 16;

    V63Lz4Fn g_v63Lz4 = nullptr;
    uint8_t* g_v63CacheRootObject = nullptr;

    std::vector<V63PlanEntry> g_v63LearnedPlan;
    std::vector<V63PlanEntry> g_v63ActivePlan;
    std::vector<V63PlanEntry> g_v63CurrentPlan;
    std::unordered_map<std::string, size_t> g_v63CurrentPlanIndex;

    std::unordered_map<std::string, V63ReadyBlob> g_v63Ready;
    std::unordered_set<std::string> g_v63SkipKeys;
    std::mutex g_v63Mutex;
    std::mutex g_v63PlanMutex;
    std::atomic<bool> g_v63PrefetchActive{false};
    std::unordered_map<std::string, size_t> g_v632LearnedIndex;
    std::atomic<size_t> g_v632DemandIndex{0};
    std::atomic<bool> g_v632DemandSeen{false};
    size_t g_v632Lookahead = 32;
    std::condition_variable g_v63Cv;
    std::vector<std::thread> g_v63Threads;
    std::atomic<size_t> g_v63NextJob{0};
    std::atomic<bool> g_v63Stop{false};
    // Radiant exits through ExitProcess and the host skips plugin shutdown then, so workers would still be joinable at static destruction
    // (std::terminate). Declared after g_v63Threads so it is destroyed first, detaching is enough as the workers are already gone.
    struct V63ExitThreadGuard
    {
        ~V63ExitThreadGuard() { for (std::thread& t : g_v63Threads) if (t.joinable()) t.detach(); }
    } g_v63ExitThreadGuard;

    uint64_t g_v63ReadyBytes = 0;
    uint64_t g_v63ReadyPeakBytes = 0;
    uint64_t g_v63LoadsLearned = 0;
    uint64_t g_v63LoadsPrefetched = 0;
    std::atomic<uint64_t> g_v63JobsStarted{0};
    std::atomic<uint64_t> g_v63JobsReady{0};
    std::atomic<uint64_t> g_v63JobsFailed{0};
    std::atomic<uint64_t> g_v63JobsSkipped{0};
    uint64_t g_v63Hits = 0;
    uint64_t g_v63Misses = 0;
    uint64_t g_v63BytesServed = 0;
    std::atomic<uint64_t> g_v63WorkerReadTicks{0};
    std::atomic<uint64_t> g_v63WorkerDecompTicks{0};

    bool V6MapLoading()
    {
        return InterlockedCompareExchange(&g_v6MapLoadDepth, 0, 0) > 0;
    }

    bool V6WriteCodeByte(uint8_t* address, uint8_t value)
    {
        if (!address) return false;
        DWORD oldProtect = 0;
        if (!VirtualProtect(address, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
        *address = value;
        FlushInstructionCache(GetCurrentProcess(), address, 1);
        DWORD ignored = 0;
        VirtualProtect(address, 1, oldProtect, &ignored);
        return true;
    }

    bool V6SetTimestampBypass(bool enabled)
    {
        if (!g_v6TimestampPatchReady || !g_v6TimestampBranch) return false;
        if (enabled == g_v6TimestampPatched) return true;
        const uint8_t want = enabled ? 0xEB : g_v6TimestampOriginal; // unconditional short jump vs original JZ
        if (!V6WriteCodeByte(g_v6TimestampBranch, want)) return false;
        g_v6TimestampPatched = enabled;
        return true;
    }

    std::string V6RadiantString(__int64 stringObject)
    {
        if (!stringObject) return {};
        const uint8_t* p = reinterpret_cast<const uint8_t*>(stringObject);
        const uint64_t size = *reinterpret_cast<const uint64_t*>(p + 16);
        const uint64_t capacity = *reinterpret_cast<const uint64_t*>(p + 24);
        if (size > (1ull << 20)) return {}; // keys are tiny, reject a corrupt or foreign layout
        const char* chars = capacity >= 16 ? *reinterpret_cast<char* const*>(p) : reinterpret_cast<const char*>(p);
        if (!chars) return {};
        return std::string(chars, chars + size);
    }

    bool V6FileIdentity(HANDLE file, uint64_t& compressedSize, uint64_t& writeTime)
    {
        compressedSize = 0;
        writeTime = 0;
        if (!file || file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        FILETIME write{};
        if (!GetFileSizeEx(file, &size)) return false;
        if (!GetFileTime(file, nullptr, nullptr, &write)) return false;
        ULARGE_INTEGER wt{};
        wt.LowPart = write.dwLowDateTime;
        wt.HighPart = write.dwHighDateTime;
        compressedSize = uint64_t(size.QuadPart);
        writeTime = wt.QuadPart;
        return true;
    }

    int V61ResolveAssetRamMB()
    {
        if (g_v6AssetRamMB > 0) return g_v6AssetRamMB;
        if (g_v61ResolvedAssetRamMB > 0) return g_v61ResolvedAssetRamMB;

        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        uint64_t totalMB = 0, availMB = 0;
        if (GlobalMemoryStatusEx(&ms))
        {
            totalMB = ms.ullTotalPhys >> 20;
            availMB = ms.ullAvailPhys >> 20;
        }
        // A multi-GB decompressed cache pushes a heavy session into memory pressure and slows LZ4, so this is only a small hot cache.
        uint64_t target = totalMB ? totalMB / 64 : 256;
        if (availMB) target = (std::min)(target, availMB / 16);
        if (target < 128) target = 128;
        if (target > 512) target = 512;
        g_v61ResolvedAssetRamMB = int(target);
        return g_v61ResolvedAssetRamMB;
    }

    uint64_t V6AssetRamLimitBytes()
    {
        return uint64_t(V61ResolveAssetRamMB()) * 1024ull * 1024ull;
    }

    uint64_t V6AssetRamMaxEntryBytes()
    {
        return uint64_t(g_v6AssetRamMaxEntryMB > 0 ? g_v6AssetRamMaxEntryMB : 0) * 1024ull * 1024ull;
    }

    void V6AssetRamClear()
    {
        std::lock_guard<std::mutex> lock(g_v6AssetRamMutex);
        g_v6AssetRam.clear();
        g_v6AssetRamOrder.clear();
        g_v6AssetRamBytes = 0;
    }

    void V6AssetRamEvictFor(uint64_t need)
    {
        const uint64_t limit = V6AssetRamLimitBytes();
        while (g_v6AssetRamBytes + need > limit && !g_v6AssetRamOrder.empty())
        {
            const std::string key = g_v6AssetRamOrder.front();
            g_v6AssetRamOrder.pop_front();
            auto it = g_v6AssetRam.find(key);
            if (it == g_v6AssetRam.end()) continue;
            if (it->second.bytes)
            {
                const uint64_t n = uint64_t(it->second.bytes->size());
                g_v6AssetRamBytes = n <= g_v6AssetRamBytes ? g_v6AssetRamBytes - n : 0;
            }
            g_v6AssetRam.erase(it);
            ++g_v6AssetRamEvictions;
        }
    }

    bool V6AssetRamLoad(__int64 outObject, HANDLE file)
    {
        if (!g_v6AssetRamEnabled || g_v6AssetKey.empty() || !outObject || !g_v6AssetBlobVtable) return false;

        // Check membership before touching file metadata: on zm_giant ~10k requests per load are misses.
        std::shared_ptr<std::vector<unsigned char>> bytes;
        uint64_t expectedCompressedSize = 0;
        uint64_t expectedWriteTime = 0;
        {
            std::lock_guard<std::mutex> lock(g_v6AssetRamMutex);
            auto it = g_v6AssetRam.find(g_v6AssetKey);
            if (it == g_v6AssetRam.end() || !it->second.bytes)
            {
                ++g_v6AssetRamMisses;
                return false;
            }
            bytes = it->second.bytes;
            expectedCompressedSize = it->second.compressedSize;
            expectedWriteTime = it->second.writeTime;
        }

        uint64_t compressedSize = 0, writeTime = 0;
        if (!V6FileIdentity(file, compressedSize, writeTime) ||
            compressedSize != expectedCompressedSize || writeTime != expectedWriteTime)
        {
            std::lock_guard<std::mutex> lock(g_v6AssetRamMutex);
            auto it = g_v6AssetRam.find(g_v6AssetKey);
            if (it != g_v6AssetRam.end())
            {
                if (it->second.bytes)
                {
                    const uint64_t n = uint64_t(it->second.bytes->size());
                    g_v6AssetRamBytes = n <= g_v6AssetRamBytes ? g_v6AssetRamBytes - n : 0;
                }
                g_v6AssetRamOrder.erase(it->second.lru);
                g_v6AssetRam.erase(it);
            }
            ++g_v6AssetRamMisses;
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(g_v6AssetRamMutex);
            auto it = g_v6AssetRam.find(g_v6AssetKey);
            if (it == g_v6AssetRam.end() || !it->second.bytes)
            {
                ++g_v6AssetRamMisses;
                return false;
            }
            g_v6AssetRamOrder.splice(g_v6AssetRamOrder.end(), g_v6AssetRamOrder, it->second.lru);
            it->second.lru = std::prev(g_v6AssetRamOrder.end());
            bytes = it->second.bytes;
            ++g_v6AssetRamHits;
            g_v6AssetRamBytesServed += uint64_t(bytes->size());
        }

        void* copy = bytes->empty() ? nullptr : malloc(bytes->size());
        if (!bytes->empty() && !copy) return false;
        if (!bytes->empty()) memcpy(copy, bytes->data(), bytes->size());

        uint8_t* o = reinterpret_cast<uint8_t*>(outObject);
        *reinterpret_cast<void**>(o + 0) = g_v6AssetBlobVtable;
        *reinterpret_cast<HANDLE*>(o + 8) = file;
        *reinterpret_cast<void**>(o + 16) = copy;
        *reinterpret_cast<uint64_t*>(o + 24) = uint64_t(bytes->size());
        *reinterpret_cast<uint64_t*>(o + 32) = 0;
        return true;
    }

    void V6AssetRamStore(__int64 outObject, HANDLE file)
    {
        if (!g_v6AssetRamEnabled || g_v6AssetKey.empty() || !outObject) return;

        const uint8_t* o = reinterpret_cast<const uint8_t*>(outObject);
        const void* data = *reinterpret_cast<void* const*>(o + 16);
        const uint64_t size = *reinterpret_cast<const uint64_t*>(o + 24);
        const uint64_t limit = V6AssetRamLimitBytes();
        const uint64_t maxEntry = V6AssetRamMaxEntryBytes();
        if (!data || !size || !limit || size > limit || (maxEntry && size > maxEntry)) return;

        uint64_t compressedSize = 0, writeTime = 0;
        if (!V6FileIdentity(file, compressedSize, writeTime)) return;

        auto bytes = std::make_shared<std::vector<unsigned char>>();
        try
        {
            bytes->resize(size_t(size));
        }
        catch (...)
        {
            return;
        }
        memcpy(bytes->data(), data, size_t(size));

        std::lock_guard<std::mutex> lock(g_v6AssetRamMutex);
        auto existing = g_v6AssetRam.find(g_v6AssetKey);
        if (existing != g_v6AssetRam.end())
        {
            if (existing->second.compressedSize == compressedSize && existing->second.writeTime == writeTime)
            {
                g_v6AssetRamOrder.splice(g_v6AssetRamOrder.end(), g_v6AssetRamOrder, existing->second.lru);
                existing->second.lru = std::prev(g_v6AssetRamOrder.end());
                return;
            }
            if (existing->second.bytes)
            {
                const uint64_t n = uint64_t(existing->second.bytes->size());
                g_v6AssetRamBytes = n <= g_v6AssetRamBytes ? g_v6AssetRamBytes - n : 0;
            }
            g_v6AssetRamOrder.erase(existing->second.lru);
            g_v6AssetRam.erase(existing);
        }

        // Never let a sequential map scan churn the cache: fill the hot cache until full, then stop admitting for this load.
        if (g_v6AssetRamBytes + size > limit)
        {
            if (!V6MapLoading())
                V6AssetRamEvictFor(size);
            else
                return;
        }
        if (g_v6AssetRamBytes + size > limit) return;

        g_v6AssetRamOrder.push_back(g_v6AssetKey);
        V6AssetRamEntry entry;
        entry.bytes = bytes;
        entry.compressedSize = compressedSize;
        entry.writeTime = writeTime;
        entry.lru = std::prev(g_v6AssetRamOrder.end());
        g_v6AssetRam.emplace(g_v6AssetKey, std::move(entry));
        g_v6AssetRamBytes += size;
        g_v6AssetRamBytesStored += size;
        ++g_v6AssetRamStores;
    }

    bool V6PrimeGeoCache(const char* reason)
    {
        if (!g_v6GeoPreallocate || !g_v6RendererReady || !g_v6GeoCache || !g_oDeepGeoGrow)
            return false;

        const uint32_t targetVB = 0x02000000u;
        const uint32_t targetIB = 0x08000000u;
        const uint32_t beforeVB = *reinterpret_cast<uint32_t*>(g_v6GeoCache + 80);
        const uint32_t beforeIB = *reinterpret_cast<uint32_t*>(g_v6GeoCache + 84);
        const uint32_t usedVB = *reinterpret_cast<uint32_t*>(g_v6GeoCache + 88);
        const uint32_t usedIB = *reinterpret_cast<uint32_t*>(g_v6GeoCache + 92);

        if (beforeVB >= targetVB && beforeIB >= targetIB)
        {
            g_v6GeoPrimed = true;
            return true;
        }
        g_v6GeoPrimed = false;

        // Grow while the cache is nearly empty, growing after a heavy map is populated recreates buffers and copies a lot of GeoCache data.
        if (usedVB > 4u * 1024u * 1024u || usedIB > 16u * 1024u * 1024u)
            return false;

        const int addVB = int(targetVB - (usedVB < targetVB ? usedVB : targetVB));
        const int addIB = int(targetIB - (usedIB < targetIB ? usedIB : targetIB));
        ++g_v6GeoPrimeAttempts;
        const uint64_t begin = DeepNow();
        const char ok = g_oDeepGeoGrow(reinterpret_cast<__int64>(g_v6GeoCache), addVB, addIB);
        g_v6GeoPrimeTicks += DeepNow() - begin;

        const uint32_t afterVB = *reinterpret_cast<uint32_t*>(g_v6GeoCache + 80);
        const uint32_t afterIB = *reinterpret_cast<uint32_t*>(g_v6GeoCache + 84);
        if (ok && afterVB >= targetVB && afterIB >= targetIB)
        {
            ++g_v6GeoPrimeSuccess;
            g_v6GeoPrimed = true;
            rp::Log("perf-v6.3.2: GeoCache pre-sized for %s (%u/%u -> %u/%u)",
                    reason ? reason : "idle", beforeVB, beforeIB, afterVB, afterIB);
            return true;
        }
        return false;
    }


    int V63ResolvedWorkerCount()
    {
        int n = g_v63PrefetchWorkers;
        if (n <= 0)
        {
            const unsigned hc = std::thread::hardware_concurrency();
            n = hc >= 12 ? 6 : hc >= 8 ? 4 : hc >= 4 ? 3 : 2;
        }
        if (n < 1) n = 1;
        if (n > 8) n = 8;
        return n;
    }

    uint64_t V63PrefetchLimitBytes()
    {
        int mb = g_v63PrefetchMB;
        if (mb < 16) mb = 16;
        if (mb > 1024) mb = 1024;
        return uint64_t(mb) * 1024ull * 1024ull;
    }

    uint64_t V63MaxEntryBytes()
    {
        int mb = g_v63PrefetchMaxEntryMB;
        if (mb < 1) mb = 1;
        if (mb > 256) mb = 256;
        return uint64_t(mb) * 1024ull * 1024ull;
    }

    std::string V63CacheRoot()
    {
        if (!g_v63CacheRootObject) return {};
        return V6RadiantString(reinterpret_cast<__int64>(g_v63CacheRootObject));
    }

    std::string V63AssetPath(const std::string& root, const std::string& key)
    {
        if (root.empty() || key.empty()) return {};
        std::string p = root;
        const char last = p.empty() ? 0 : p.back();
        if (last != '/' && last != '\\') p.push_back('/');
        p += key;
        p += ".lz4";
        return p;
    }

    void V63ResetMetrics()
    {
        g_v63LoadsLearned = 0;
        g_v63LoadsPrefetched = 0;
        g_v63JobsStarted.store(0, std::memory_order_relaxed);
        g_v63JobsReady.store(0, std::memory_order_relaxed);
        g_v63JobsFailed.store(0, std::memory_order_relaxed);
        g_v63JobsSkipped.store(0, std::memory_order_relaxed);
        g_v63Hits = 0;
        g_v63Misses = 0;
        g_v63BytesServed = 0;
        g_v63WorkerReadTicks.store(0, std::memory_order_relaxed);
        g_v63WorkerDecompTicks.store(0, std::memory_order_relaxed);
        g_v63ReadyPeakBytes = 0;
    }

    void V63RecordRequest(const std::string& key)
    {
        if (!g_v63PrefetchEnabled || !V6MapLoading() || key.empty()) return;

        // Asset-cache work is not confined to one Radiant thread: the learning bookkeeping is serialized.
        std::lock_guard<std::mutex> lock(g_v63PlanMutex);

        auto it = g_v63CurrentPlanIndex.find(key);
        if (it == g_v63CurrentPlanIndex.end())
        {
            const size_t idx = g_v63CurrentPlan.size();
            V63PlanEntry e;
            e.key = key;
            e.expectedUses = 1;
            g_v63CurrentPlan.push_back(std::move(e));
            g_v63CurrentPlanIndex.emplace(g_v63CurrentPlan.back().key, idx);
        }
        else
        {
            V63PlanEntry& e = g_v63CurrentPlan[it->second];
            if (e.expectedUses != 0xFFFFFFFFu) ++e.expectedUses;
        }
    }

    void V63ReleaseReservation(uint64_t bytes)
    {
        {
            std::lock_guard<std::mutex> lock(g_v63Mutex);
            g_v63ReadyBytes = bytes <= g_v63ReadyBytes ? g_v63ReadyBytes - bytes : 0;
        }
        g_v63Cv.notify_all();
    }

    bool V63ReadAndDecompress(const std::string& path, std::shared_ptr<std::vector<unsigned char>>& outBytes,
                              uint64_t& readTicks, uint64_t& decompTicks)
    {
        readTicks = 0;
        decompTicks = 0;
        if (!g_v63Lz4 || path.empty()) return false;

        const uint64_t readBegin = DeepNow();
        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            readTicks = DeepNow() - readBegin;
            return false;
        }

        LARGE_INTEGER size{};
        DWORD got = 0;
        uint32_t unpackedSize = 0;
        bool ok = GetFileSizeEx(h, &size) != FALSE &&
                  size.QuadPart > 4 && size.QuadPart <= 0x7FFFFFFFLL &&
                  ReadFile(h, &unpackedSize, 4, &got, nullptr) != FALSE && got == 4;

        if (!ok || unpackedSize == 0 || uint64_t(unpackedSize) > V63MaxEntryBytes())
        {
            CloseHandle(h);
            readTicks = DeepNow() - readBegin;
            return false;
        }

        const uint32_t compressedSize = uint32_t(size.QuadPart - 4);
        std::vector<unsigned char> compressed;
        try { compressed.resize(compressedSize); }
        catch (...) { CloseHandle(h); readTicks = DeepNow() - readBegin; return false; }

        got = 0;
        ok = ReadFile(h, compressed.data(), compressedSize, &got, nullptr) != FALSE && got == compressedSize;
        CloseHandle(h);
        readTicks = DeepNow() - readBegin;
        if (!ok) return false;

        std::shared_ptr<std::vector<unsigned char>> bytes;
        try { bytes = std::make_shared<std::vector<unsigned char>>(unpackedSize); }
        catch (...) { return false; }

        const uint64_t decBegin = DeepNow();
        const __int64 decoded = g_v63Lz4(compressed.data(), reinterpret_cast<char*>(bytes->data()),
                                         int(compressedSize), int(unpackedSize));
        decompTicks = DeepNow() - decBegin;
        if (decoded != __int64(unpackedSize)) return false;

        outBytes = std::move(bytes);
        return true;
    }

    void V632AtomicMax(std::atomic<size_t>& value, size_t want)
    {
        size_t cur = value.load(std::memory_order_relaxed);
        while (cur < want && !value.compare_exchange_weak(
                   cur, want, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
        }
    }

    void V632AdvanceDemand(size_t idx)
    {
        const bool first = !g_v632DemandSeen.exchange(true, std::memory_order_acq_rel);
        if (first)
            g_v632DemandIndex.store(idx, std::memory_order_release);
        else
            V632AtomicMax(g_v632DemandIndex, idx);

        // Anything at or behind the main thread cannot help, purge it so old predictions never fill the bounded queue.
        const size_t demand = g_v632DemandIndex.load(std::memory_order_acquire);
        {
            std::lock_guard<std::mutex> lock(g_v63Mutex);
            for (auto it = g_v63Ready.begin(); it != g_v63Ready.end(); )
            {
                if (it->second.planIndex < demand)
                {
                    if (it->second.bytes)
                    {
                        const uint64_t n = uint64_t(it->second.bytes->size());
                        g_v63ReadyBytes = n <= g_v63ReadyBytes ? g_v63ReadyBytes - n : 0;
                    }
                    it = g_v63Ready.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        // Workers start just after the current demand point, not on stale jobs from the start of the learned plan.
        V632AtomicMax(g_v63NextJob, demand + 1);
        g_v63Cv.notify_all();
    }

    bool V632LookupPlanIndex(const std::string& key, size_t& outIdx)
    {
        if (key.empty()) return false;
        std::lock_guard<std::mutex> planLock(g_v63PlanMutex);
        auto it = g_v632LearnedIndex.find(key);
        if (it == g_v632LearnedIndex.end()) return false;
        outIdx = it->second;
        return true;
    }

    bool V632ClaimJob(size_t& outIdx)
    {
        std::unique_lock<std::mutex> lock(g_v63Mutex);
        g_v63Cv.wait(lock, [&] {
            if (g_v63Stop.load(std::memory_order_acquire)) return true;
            if (!g_v632DemandSeen.load(std::memory_order_acquire)) return false;
            const size_t demand = g_v632DemandIndex.load(std::memory_order_acquire);
            const size_t next = g_v63NextJob.load(std::memory_order_acquire);
            return next < g_v63ActivePlan.size() && next <= demand + g_v632Lookahead;
        });

        if (g_v63Stop.load(std::memory_order_acquire)) return false;

        for (;;)
        {
            const size_t demand = g_v632DemandIndex.load(std::memory_order_acquire);
            size_t next = g_v63NextJob.load(std::memory_order_acquire);
            if (next < demand + 1)
            {
                lock.unlock();
                V632AtomicMax(g_v63NextJob, demand + 1);
                lock.lock();
                continue;
            }
            if (next >= g_v63ActivePlan.size() || next > demand + g_v632Lookahead)
                return false;
            if (g_v63NextJob.compare_exchange_weak(
                    next, next + 1, std::memory_order_acq_rel, std::memory_order_relaxed))
            {
                outIdx = next;
                return true;
            }
        }
    }

    void V63Worker(std::string root)
    {
        for (;;)
        {
            if (g_v63Stop.load(std::memory_order_acquire)) return;

            size_t idx = 0;
            if (!V632ClaimJob(idx))
            {
                if (g_v63Stop.load(std::memory_order_acquire)) return;
                continue;
            }
            if (idx >= g_v63ActivePlan.size()) continue;

            const V63PlanEntry plan = g_v63ActivePlan[idx];
            if (plan.key.empty()) continue;

            {
                std::lock_guard<std::mutex> lock(g_v63Mutex);
                if (g_v63SkipKeys.find(plan.key) != g_v63SkipKeys.end())
                {
                    g_v63JobsSkipped.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
            }

            g_v63JobsStarted.fetch_add(1, std::memory_order_relaxed);
            const std::string path = V63AssetPath(root, plan.key);
            if (path.empty())
            {
                g_v63JobsFailed.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            const uint64_t readBegin = DeepNow();
            HANDLE h = CreateFileA(path.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                g_v63WorkerReadTicks.fetch_add(DeepNow() - readBegin, std::memory_order_relaxed);
                g_v63JobsFailed.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            LARGE_INTEGER fileSize{};
            uint32_t unpackedSize = 0;
            DWORD got = 0;
            bool ok = GetFileSizeEx(h, &fileSize) != FALSE &&
                      fileSize.QuadPart > 4 && fileSize.QuadPart <= 0x7FFFFFFFLL &&
                      ReadFile(h, &unpackedSize, 4, &got, nullptr) != FALSE && got == 4 &&
                      unpackedSize > 0 && uint64_t(unpackedSize) <= V63MaxEntryBytes();

            if (!ok)
            {
                CloseHandle(h);
                g_v63WorkerReadTicks.fetch_add(DeepNow() - readBegin, std::memory_order_relaxed);
                g_v63JobsFailed.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            {
                std::unique_lock<std::mutex> lock(g_v63Mutex);
                g_v63Cv.wait(lock, [&] {
                    return g_v63Stop.load(std::memory_order_acquire) ||
                           g_v63SkipKeys.find(plan.key) != g_v63SkipKeys.end() ||
                           g_v63ReadyBytes + uint64_t(unpackedSize) <= V63PrefetchLimitBytes();
                });
                if (g_v63Stop.load(std::memory_order_acquire))
                {
                    CloseHandle(h);
                    return;
                }
                if (g_v63SkipKeys.find(plan.key) != g_v63SkipKeys.end())
                {
                    CloseHandle(h);
                    g_v63JobsSkipped.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                g_v63ReadyBytes += uint64_t(unpackedSize); // reserve before allocation and decompression
                if (g_v63ReadyBytes > g_v63ReadyPeakBytes) g_v63ReadyPeakBytes = g_v63ReadyBytes;
            }

            const uint32_t compressedSize = uint32_t(fileSize.QuadPart - 4);
            std::vector<unsigned char> compressed;
            try { compressed.resize(compressedSize); }
            catch (...)
            {
                CloseHandle(h);
                V63ReleaseReservation(uint64_t(unpackedSize));
                g_v63JobsFailed.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            got = 0;
            ok = ReadFile(h, compressed.data(), compressedSize, &got, nullptr) != FALSE && got == compressedSize;
            CloseHandle(h);
            g_v63WorkerReadTicks.fetch_add(DeepNow() - readBegin, std::memory_order_relaxed);
            if (!ok)
            {
                V63ReleaseReservation(uint64_t(unpackedSize));
                g_v63JobsFailed.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            std::shared_ptr<std::vector<unsigned char>> bytes;
            try { bytes = std::make_shared<std::vector<unsigned char>>(unpackedSize); }
            catch (...)
            {
                V63ReleaseReservation(uint64_t(unpackedSize));
                g_v63JobsFailed.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            const uint64_t decBegin = DeepNow();
            const __int64 decoded = g_v63Lz4(
                compressed.data(), reinterpret_cast<char*>(bytes->data()),
                int(compressedSize), int(unpackedSize));
            g_v63WorkerDecompTicks.fetch_add(DeepNow() - decBegin, std::memory_order_relaxed);

            if (decoded != __int64(unpackedSize))
            {
                V63ReleaseReservation(uint64_t(unpackedSize));
                g_v63JobsFailed.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(g_v63Mutex);
                const size_t demandNow = g_v632DemandIndex.load(std::memory_order_acquire);
                if (g_v63Stop.load(std::memory_order_acquire) ||
                    (g_v632DemandSeen.load(std::memory_order_acquire) && idx <= demandNow) ||
                    g_v63SkipKeys.find(plan.key) != g_v63SkipKeys.end())
                {
                    g_v63ReadyBytes = uint64_t(bytes->size()) <= g_v63ReadyBytes
                        ? g_v63ReadyBytes - uint64_t(bytes->size()) : 0;
                    g_v63JobsSkipped.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    V63ReadyBlob blob;
                    blob.bytes = std::move(bytes);
                    blob.remainingUses = plan.expectedUses ? plan.expectedUses : 1;
                    blob.planIndex = idx;
                    g_v63Ready[plan.key] = std::move(blob);
                    g_v63JobsReady.fetch_add(1, std::memory_order_relaxed);
                }
            }
            g_v63Cv.notify_all();
        }
    }

    void V63StopWorkers(bool clearReady)
    {
        g_v63PrefetchActive.store(false, std::memory_order_release);
        g_v63Stop.store(true, std::memory_order_release);
        g_v63Cv.notify_all();
        for (std::thread& t : g_v63Threads)
            if (t.joinable()) t.join();
        g_v63Threads.clear();

        if (clearReady)
        {
            std::lock_guard<std::mutex> lock(g_v63Mutex);
            g_v63Ready.clear();
            g_v63SkipKeys.clear();
            g_v63ReadyBytes = 0;
        }
    }

    void V63BeginMapLoad()
    {
        V63StopWorkers(true);

        {
            std::lock_guard<std::mutex> planLock(g_v63PlanMutex);
            g_v63CurrentPlan.clear();
            g_v63CurrentPlanIndex.clear();
            g_v63ActivePlan.clear();

            // Avoid reallocations while thousands of cache keys are learned.
            const size_t reserveCount = g_v63LearnedPlan.empty()
                ? size_t(16384)
                : (g_v63LearnedPlan.size() + size_t(1024));
            g_v63CurrentPlan.reserve(reserveCount);
            g_v63CurrentPlanIndex.reserve(reserveCount);
        }

        g_v63NextJob.store(0, std::memory_order_relaxed);
        g_v632DemandIndex.store(0, std::memory_order_relaxed);
        g_v632DemandSeen.store(false, std::memory_order_release);
        g_v63Stop.store(false, std::memory_order_release);

        if (!g_v63PrefetchEnabled || !g_v63Lz4)
            return;

        std::string root;
        {
            std::lock_guard<std::mutex> planLock(g_v63PlanMutex);
            if (g_v63LearnedPlan.empty())
                return; // first load is learning only
            g_v63ActivePlan = g_v63LearnedPlan;
            g_v632LearnedIndex.clear();
            g_v632LearnedIndex.reserve(g_v63ActivePlan.size() + 512);
            for (size_t i = 0; i < g_v63ActivePlan.size(); ++i)
                g_v632LearnedIndex.emplace(g_v63ActivePlan[i].key, i);
        }

        root = V63CacheRoot();
        if (root.empty())
        {
            std::lock_guard<std::mutex> planLock(g_v63PlanMutex);
            g_v63ActivePlan.clear();
            return;
        }

        ++g_v63LoadsPrefetched;
        g_v63PrefetchActive.store(true, std::memory_order_release);

        const int workerCount = V63ResolvedWorkerCount();
        g_v63Threads.reserve(size_t(workerCount));
        for (int i = 0; i < workerCount; ++i)
            g_v63Threads.emplace_back([root] { V63Worker(root); });
    }

    void V63EndMapLoad()
    {
        V63StopWorkers(true);

        {
            std::lock_guard<std::mutex> planLock(g_v63PlanMutex);
            if (!g_v63CurrentPlan.empty())
            {
                g_v63LearnedPlan = g_v63CurrentPlan;
                ++g_v63LoadsLearned;
            }
            g_v63ActivePlan.clear();
            g_v63CurrentPlan.clear();
            g_v63CurrentPlanIndex.clear();
            g_v632LearnedIndex.clear();
        }
        g_v632DemandSeen.store(false, std::memory_order_release);
    }

    bool V63TryConsume(__int64 outObject, HANDLE file)
    {
        if (!g_v63PrefetchEnabled ||
            !g_v63PrefetchActive.load(std::memory_order_acquire) ||
            !V6MapLoading() || g_v6AssetKey.empty() ||
            !outObject || !g_v6AssetBlobVtable)
            return false;

        size_t planIndex = 0;
        if (V632LookupPlanIndex(g_v6AssetKey, planIndex))
            V632AdvanceDemand(planIndex);

        std::shared_ptr<std::vector<unsigned char>> bytes;
        bool releaseReadyBytes = false;
        {
            std::lock_guard<std::mutex> lock(g_v63Mutex);
            auto it = g_v63Ready.find(g_v6AssetKey);
            if (it == g_v63Ready.end() || !it->second.bytes)
            {
                ++g_v63Misses;
                // The main thread is about to read it itself: tell a worker that has not started this key not to keep a duplicate.
                g_v63SkipKeys.insert(g_v6AssetKey);
                g_v63Cv.notify_all();
                return false;
            }

            bytes = it->second.bytes;
            ++g_v63Hits;
            g_v63BytesServed += uint64_t(bytes->size());

            if (it->second.remainingUses > 1)
            {
                --it->second.remainingUses;
            }
            else
            {
                g_v63ReadyBytes = uint64_t(bytes->size()) <= g_v63ReadyBytes
                    ? g_v63ReadyBytes - uint64_t(bytes->size()) : 0;
                g_v63Ready.erase(it);
                releaseReadyBytes = true;
            }
        }
        if (releaseReadyBytes) g_v63Cv.notify_all();

        void* copy = bytes->empty() ? nullptr : malloc(bytes->size());
        if (!bytes->empty() && !copy) return false;
        if (!bytes->empty()) memcpy(copy, bytes->data(), bytes->size());

        uint8_t* o = reinterpret_cast<uint8_t*>(outObject);
        *reinterpret_cast<void**>(o + 0) = g_v6AssetBlobVtable;
        *reinterpret_cast<HANDLE*>(o + 8) = file;
        *reinterpret_cast<void**>(o + 16) = copy;
        *reinterpret_cast<uint64_t*>(o + 24) = uint64_t(bytes->size());
        *reinterpret_cast<uint64_t*>(o + 32) = 0;
        return true;
    }

    void V63ClearLearned()
    {
        V63StopWorkers(true);
        std::lock_guard<std::mutex> planLock(g_v63PlanMutex);
        g_v63LearnedPlan.clear();
        g_v63ActivePlan.clear();
        g_v63CurrentPlan.clear();
        g_v63CurrentPlanIndex.clear();
        g_v632LearnedIndex.clear();
        g_v632DemandSeen.store(false, std::memory_order_release);
    }

    std::string V63Status()
    {
        size_t ready = 0;
        uint64_t readyBytes = 0;
        {
            std::lock_guard<std::mutex> lock(g_v63Mutex);
            ready = g_v63Ready.size();
            readyBytes = g_v63ReadyBytes;
        }

        size_t learned = 0, activePlan = 0, learningNow = 0;
        {
            std::lock_guard<std::mutex> planLock(g_v63PlanMutex);
            learned = g_v63LearnedPlan.size();
            activePlan = g_v63ActivePlan.size();
            learningNow = g_v63CurrentPlan.size();
        }

        char b[1100];
        snprintf(b, sizeof b,
                 "prefetch=%d prefetch_active=%d learned=%llu active=%llu learning=%llu workers=%d cap=%dMB max_entry=%dMB demand_seen=%d demand=%llu window=%llu "
                 "ready=%llu ready_mb=%.1f peak_mb=%.1f loads_learned=%llu loads_prefetched=%llu "
                 "jobs=%llu ready_jobs=%llu failed=%llu skipped=%llu hits=%llu misses=%llu served=%.1fMB "
                 "worker_read_ms=%.1f worker_decomp_ms=%.1f",
                 g_v63PrefetchEnabled ? 1 : 0,
                 g_v63PrefetchActive.load(std::memory_order_acquire) ? 1 : 0,
                 (unsigned long long)learned,
                 (unsigned long long)activePlan,
                 (unsigned long long)learningNow,
                 V63ResolvedWorkerCount(), g_v63PrefetchMB, g_v63PrefetchMaxEntryMB,
                 g_v632DemandSeen.load(std::memory_order_acquire) ? 1 : 0,
                 (unsigned long long)g_v632DemandIndex.load(std::memory_order_acquire),
                 (unsigned long long)g_v632Lookahead,
                 (unsigned long long)ready,
                 double(readyBytes) / (1024.0 * 1024.0),
                 double(g_v63ReadyPeakBytes) / (1024.0 * 1024.0),
                 (unsigned long long)g_v63LoadsLearned,
                 (unsigned long long)g_v63LoadsPrefetched,
                 (unsigned long long)g_v63JobsStarted.load(std::memory_order_relaxed),
                 (unsigned long long)g_v63JobsReady.load(std::memory_order_relaxed),
                 (unsigned long long)g_v63JobsFailed.load(std::memory_order_relaxed),
                 (unsigned long long)g_v63JobsSkipped.load(std::memory_order_relaxed),
                 (unsigned long long)g_v63Hits,
                 (unsigned long long)g_v63Misses,
                 double(g_v63BytesServed) / (1024.0 * 1024.0),
                 PerfV2TicksToMs(g_v63WorkerReadTicks.load(std::memory_order_relaxed)),
                 PerfV2TicksToMs(g_v63WorkerDecompTicks.load(std::memory_order_relaxed)));
        return b;
    }

    void V6ResetCaptureCounters()
    {
        g_v6MapLoads = 0;
        g_v6RenderSuppress3D = 0;
        g_v6RenderSuppress2D = 0;
        g_v6TimestampSkips = 0;
        g_v6AssetRamHits = 0;
        g_v6AssetRamMisses = 0;
        g_v6AssetRamStores = 0;
        g_v6AssetRamEvictions = 0;
        g_v6AssetRamBytesServed = 0;
        g_v6AssetRamBytesStored = 0;
        V63ResetMetrics();
    }

    std::string V6Status()
    {
        size_t entries = 0;
        uint64_t ramBytes = 0;
        {
            std::lock_guard<std::mutex> lock(g_v6AssetRamMutex);
            entries = g_v6AssetRam.size();
            ramBytes = g_v6AssetRamBytes;
        }
        char b[1536];
        snprintf(b, sizeof b,
                 "perf-v6.3.2: map_loading=%d loads=%llu suppress_render=%d suppressed_3d=%llu suppressed_2d=%llu "
                 "skip_timestamp=%d timestamp_skips=%llu patch_ready=%d patched=%d "
                 "asset_ram=%d entries=%llu ram=%.1f/%dMB(config=%d) hits=%llu misses=%llu stores=%llu evictions=%llu served=%.1fMB "
                 "geo_prealloc=%d ready=%d primed=%d attempts=%llu success=%llu prime_ms=%.3f resets=%llu reset_primes=%llu",
                 V6MapLoading() ? 1 : 0, (unsigned long long)g_v6MapLoads,
                 g_v6SuppressLoadRendering ? 1 : 0,
                 (unsigned long long)g_v6RenderSuppress3D, (unsigned long long)g_v6RenderSuppress2D,
                 g_v6SkipLoadTimestamps ? 1 : 0, (unsigned long long)g_v6TimestampSkips,
                 g_v6TimestampPatchReady ? 1 : 0, g_v6TimestampPatched ? 1 : 0,
                 g_v6AssetRamEnabled ? 1 : 0, (unsigned long long)entries,
                 double(ramBytes) / (1024.0 * 1024.0), V61ResolveAssetRamMB(), g_v6AssetRamMB,
                 (unsigned long long)g_v6AssetRamHits, (unsigned long long)g_v6AssetRamMisses,
                 (unsigned long long)g_v6AssetRamStores, (unsigned long long)g_v6AssetRamEvictions,
                 double(g_v6AssetRamBytesServed) / (1024.0 * 1024.0),
                 g_v6GeoPreallocate ? 1 : 0, g_v6RendererReady ? 1 : 0, g_v6GeoPrimed ? 1 : 0,
                 (unsigned long long)g_v6GeoPrimeAttempts, (unsigned long long)g_v6GeoPrimeSuccess,
                 PerfV2TicksToMs(g_v6GeoPrimeTicks),
                 (unsigned long long)g_v61GeoResets, (unsigned long long)g_v61GeoResetPrimes);
        return std::string(b) + " " + V63Status();
    }

    void DeepRecord(DeepMetric& m, uint64_t begin)
    {
        if (!begin) return;
        const uint64_t e = DeepNow() - begin;
        ++m.calls;
        m.totalTicks += e;
        if (e < m.minTicks) m.minTicks = e;
        if (e > m.maxTicks) m.maxTicks = e;
    }

    DeepDrawSnapshot DeepReadDraw()
    {
        DeepDrawSnapshot s{};
        if (g_deepDip)
        {
            s.dipCalls = g_deepDip[0];
            s.dipLogical = g_deepDip[1];
        }
        if (g_deepDiip)
        {
            s.diipCalls = g_deepDiip[0];
            s.instances = g_deepDiip[1];
        }
        if (g_deepTris) s.tris = *g_deepTris;
        return s;
    }

    void DeepAddDrawDelta(DeepPassMetric& p, const DeepDrawSnapshot& b, const DeepDrawSnapshot& a)
    {
        p.dipCalls += uint32_t(a.dipCalls - b.dipCalls);
        p.dipLogical += uint32_t(a.dipLogical - b.dipLogical);
        p.diipCalls += uint32_t(a.diipCalls - b.diipCalls);
        p.instances += uint32_t(a.instances - b.instances);
        p.tris += uint32_t(a.tris - b.tris);
    }

    void CensusRecordElapsed(DeepMetric& m, uint64_t elapsed)
    {
        ++m.calls;
        m.totalTicks += elapsed;
        if (elapsed < m.minTicks) m.minTicks = elapsed;
        if (elapsed > m.maxTicks) m.maxTicks = elapsed;
    }

    void CensusAddFrame(const DeepDrawSnapshot& s, bool fromHud)
    {
        if (!g_censusCapture) return;
        ++g_censusFrames;
        if (fromHud) ++g_censusHudSnapshots; else ++g_censusFallbackSnapshots;
        g_censusDipPhysical += s.dipCalls;
        g_censusDipLogical += s.dipLogical;
        g_censusDiipPhysical += s.diipCalls;
        g_censusInstances += s.instances;
        g_censusTriangles += s.tris;
        if (s.dipCalls > g_censusMaxDipPhysical) g_censusMaxDipPhysical = s.dipCalls;
        if (s.dipLogical > g_censusMaxDipLogical) g_censusMaxDipLogical = s.dipLogical;
        if (s.dipCalls >= 10000) ++g_censusFramesOver10k;
        if (s.dipCalls >= 30000) ++g_censusFramesOver30k;
    }

    int V722SpecialIndex()
    {
        if (!g_v722SpecialStageDepth) return -1;
        if (g_v722SpecialStage == 5u) return 0;
        if (g_v722SpecialStage == 6u) return 1;
        return -1;
    }

    uintptr_t V722StaticAddress(void* p)
    {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return (g_v722AddressBias && v >= g_v722AddressBias) ? (v - g_v722AddressBias) : v;
    }

    int V722FindCallerSlot(uintptr_t caller, uint64_t surfaces)
    {
        if (!caller) return -1;
        int freeSlot = -1;
        for (int i = 0; i < kV722CallerBucketCount; ++i)
        {
            if (g_censusUnknownCallers[i].caller == caller)
            {
                ++g_censusUnknownCallers[i].submits;
                g_censusUnknownCallers[i].surfaces += surfaces;
                return i;
            }
            if (!g_censusUnknownCallers[i].caller && freeSlot < 0) freeSlot = i;
        }
        if (freeSlot >= 0)
        {
            g_censusUnknownCallers[freeSlot].caller = caller;
            g_censusUnknownCallers[freeSlot].submits = 1;
            g_censusUnknownCallers[freeSlot].surfaces = surfaces;
            return freeSlot;
        }
        ++g_censusUnknownCallerOverflow;
        return -1;
    }

    void CensusReset(bool start)
    {
        g_censusGeoSubmit.Reset();
        g_censusCoalesceRange.Reset();
        g_censusInstance.Reset();
        g_censusSubmitLists = g_censusSubmitSurfaces = 0;
        g_censusRangeCalls = g_censusRangeRequested = g_censusRangeProcessed = g_censusRangePartial = 0;
        g_censusCoalesceCalls = g_censusCoalesceSuccess = g_censusCoalesceFail = 0;
        g_censusCoalesceVertices = g_censusCoalesceIndices = 0;
        g_censusCoalesceVertexBytes = g_censusCoalesceIndexBytes = 0;
        g_censusGeoMissCopies = g_censusGeoMissVertices = g_censusGeoMissIndices = g_censusGeoMissBytes = 0;
        g_censusInstanceRanges = g_censusInstanceRequested = g_censusInstanceProcessed = g_censusInstancePartial = 0;
        g_censusFrames = g_censusDipPhysical = g_censusDipLogical = 0;
        g_censusDiipPhysical = g_censusInstances = g_censusTriangles = 0;
        g_censusMaxDipPhysical = g_censusMaxDipLogical = 0;
        g_censusFramesOver10k = g_censusFramesOver30k = 0;
        g_censusHudSnapshots = g_censusFallbackSnapshots = 0;
        g_censusBackendGeoDraws = g_censusBackendOtherDraws = g_censusBackendUnknownGeoDraws = 0;
        for (size_t i = 0; i < kCensusFlushCauseCount; ++i)
        {
            g_censusFlushDraws[i] = 0;
            g_censusFlushIndices[i] = 0;
            g_censusUnknownPassFlushDraws[i] = 0;
        }
        memset(g_censusPassGeoDraws, 0, sizeof(g_censusPassGeoDraws));
        memset(g_censusPassFlushDraws, 0, sizeof(g_censusPassFlushDraws));
        memset(g_censusPassFlushIndices, 0, sizeof(g_censusPassFlushIndices));
        memset(g_censusPassGeoMissCopies, 0, sizeof(g_censusPassGeoMissCopies));
        memset(g_censusPassGeoMissVertices, 0, sizeof(g_censusPassGeoMissVertices));
        memset(g_censusPassGeoMissIndices, 0, sizeof(g_censusPassGeoMissIndices));
        g_censusUnknownPassGeoDraws = 0;
        g_censusUnknownPassGeoMissCopies = 0;
        memset(g_censusSpecialGeoDraws, 0, sizeof(g_censusSpecialGeoDraws));
        memset(g_censusSpecialFlushDraws, 0, sizeof(g_censusSpecialFlushDraws));
        memset(g_censusSpecialGeoMissCopies, 0, sizeof(g_censusSpecialGeoMissCopies));
        memset(g_censusSpecialGeoMissVertices, 0, sizeof(g_censusSpecialGeoMissVertices));
        memset(g_censusSpecialGeoMissIndices, 0, sizeof(g_censusSpecialGeoMissIndices));
        memset(g_censusUnknownCallers, 0, sizeof(g_censusUnknownCallers));
        g_censusUnknownCallerOverflow = 0;
        g_censusLastSubmitDraw = {};
        g_censusLastSubmitDrawValid = false;
        g_censusGeoResetStart = g_v61GeoResets;
        g_censusStart = DeepNow();
        g_censusStop = g_censusStart;
        g_censusCapture = start;
    }

    void CensusStop()
    {
        if (g_censusCapture)
        {
            g_censusStop = DeepNow();
            g_censusCapture = false;
        }
    }

    double CensusSeconds()
    {
        const uint64_t end = g_censusCapture ? DeepNow() : g_censusStop;
        if (!g_censusStart || end <= g_censusStart || !g_qpf.QuadPart) return 0.0;
        return double(end - g_censusStart) / double(g_qpf.QuadPart);
    }

    double CensusMs(const DeepMetric& m)
    {
        return g_qpf.QuadPart ? (1000.0 * double(m.totalTicks) / double(g_qpf.QuadPart)) : 0.0;
    }

    std::string CensusReport()
    {
        const double seconds = CensusSeconds();
        const double frames = g_censusFrames ? double(g_censusFrames) : 1.0;
        const double geoMs = CensusMs(g_censusGeoSubmit);
        const double rangeMs = CensusMs(g_censusCoalesceRange);
        const double instMs = CensusMs(g_censusInstance);
        const uint64_t coalBytes = g_censusCoalesceVertexBytes + g_censusCoalesceIndexBytes;
        const double coalMB = double(coalBytes) / (1024.0 * 1024.0);
        const double missMB = double(g_censusGeoMissBytes) / (1024.0 * 1024.0);
        const double avgDip = double(g_censusDipPhysical) / frames;
        const double avgLogical = double(g_censusDipLogical) / frames;
        const double mergeFactor = g_censusDipPhysical ? double(g_censusDipLogical) / double(g_censusDipPhysical) : 0.0;
        const double mergedPct = g_censusDipLogical ? 100.0 * (1.0 - double(g_censusDipPhysical) / double(g_censusDipLogical)) : 0.0;
        const uint64_t geoResets = g_v61GeoResets >= g_censusGeoResetStart ? g_v61GeoResets - g_censusGeoResetStart : 0;

        std::string out;
        char b[2048];
        snprintf(b, sizeof b,
                 "Radiant+ FPS V7.24 - Stable Geometry-Key + Special-Stage Census\n"
                 "State: %s  Window: %.3f s  Frames: %llu\n"
                 "Instrumentation only: no culling, batching, quality, lighting, ordering, or render behavior changes.\n"
                 "Hooks: geo_submit=%d special_stage56=%d coalesce_range=%d coalesce_one=%d geocache_copy=%d instance=%d hud_snapshot=%d backend_draw=%d counters=%d\n"
                 "Frame counter snapshots: HUD-entry=%llu fallback-last-post-GeoSubmit=%llu\n\n",
                 g_censusCapture ? "RUNNING" : "STOPPED", seconds, (unsigned long long)g_censusFrames,
                 g_oDeepGeoSubmit ? 1 : 0, g_oV722SpecialStage ? 1 : 0, g_oCensusCoalesceRange ? 1 : 0, g_oCensusCoalesceOne ? 1 : 0,
                 g_oCensusGeoCopy ? 1 : 0, g_oDeepInstance ? 1 : 0, g_oCensusDrawHud ? 1 : 0,
                 g_oCensusBackendDraw ? 1 : 0, (g_deepDip && g_deepDiip && g_deepTris) ? 1 : 0,
                 (unsigned long long)g_censusHudSnapshots, (unsigned long long)g_censusFallbackSnapshots);
        out += b;

        snprintf(b, sizeof b,
                 "Renderer submission timing (QPC only at range/function granularity)\n"
                 "  Geo submit 0x1411E6670: calls=%llu total=%.3f ms avg/call=%.4f ms avg/frame=%.3f ms\n"
                 "  Coalesce range 0x1411E8920: calls=%llu total=%.3f ms avg/call=%.4f ms avg/frame=%.3f ms nested_share_of_geo=%.1f%%\n"
                 "  InstanceCache 0x1410985D0: calls=%llu total=%.3f ms avg/call=%.4f ms avg/frame=%.3f ms\n\n",
                 (unsigned long long)g_censusGeoSubmit.calls, geoMs,
                 g_censusGeoSubmit.calls ? geoMs / double(g_censusGeoSubmit.calls) : 0.0, geoMs / frames,
                 (unsigned long long)g_censusCoalesceRange.calls, rangeMs,
                 g_censusCoalesceRange.calls ? rangeMs / double(g_censusCoalesceRange.calls) : 0.0, rangeMs / frames,
                 g_censusGeoSubmit.totalTicks ? 100.0 * double(g_censusCoalesceRange.totalTicks) / double(g_censusGeoSubmit.totalTicks) : 0.0,
                 (unsigned long long)g_censusInstance.calls, instMs,
                 g_censusInstance.calls ? instMs / double(g_censusInstance.calls) : 0.0, instMs / frames);
        out += b;

        snprintf(b, sizeof b,
                 "Submission / coalescing volume\n"
                 "  submit lists=%llu surfaces_seen=%llu (%.1f/frame)\n"
                 "  coalesce ranges=%llu requested=%llu processed=%llu partial_ranges=%llu\n"
                 "  coalesce calls=%llu success=%llu fail=%llu vertices=%llu indices=%llu\n"
                 "  accepted into transient coalescer=%.2f MB total (%.2f MB/frame, %.2f MB/s) [VB=%.2f MB IB=%.2f MB]\n\n",
                 (unsigned long long)g_censusSubmitLists, (unsigned long long)g_censusSubmitSurfaces,
                 double(g_censusSubmitSurfaces) / frames,
                 (unsigned long long)g_censusRangeCalls, (unsigned long long)g_censusRangeRequested,
                 (unsigned long long)g_censusRangeProcessed, (unsigned long long)g_censusRangePartial,
                 (unsigned long long)g_censusCoalesceCalls, (unsigned long long)g_censusCoalesceSuccess,
                 (unsigned long long)g_censusCoalesceFail, (unsigned long long)g_censusCoalesceVertices,
                 (unsigned long long)g_censusCoalesceIndices,
                 coalMB, coalMB / frames, seconds > 0.0 ? coalMB / seconds : 0.0,
                 double(g_censusCoalesceVertexBytes) / (1024.0 * 1024.0),
                 double(g_censusCoalesceIndexBytes) / (1024.0 * 1024.0));
        out += b;

        snprintf(b, sizeof b,
                 "Persistent GeoCache evidence\n"
                 "  miss/inserts (0x1410423C0 while inside 0x1411E6670)=%llu (%.3f/frame)\n"
                 "  miss geometry copied=%.2f MB vertices=%llu indices=%llu\n"
                 "  destructive GeoCache resets during window=%llu\n"
                 "  NOTE: exact hit count is intentionally NOT duplicated here; the lookup is inlined in 0x1411E6670."
                 " Counting misses directly avoids a second O(N) surface scan that would perturb the test.\n\n",
                 (unsigned long long)g_censusGeoMissCopies, double(g_censusGeoMissCopies) / frames,
                 missMB, (unsigned long long)g_censusGeoMissVertices, (unsigned long long)g_censusGeoMissIndices,
                 (unsigned long long)geoResets);
        out += b;

        snprintf(b, sizeof b,
                 "Actual vs logical draw submission (frame counters; HUD entry when available, otherwise last post-GeoSubmit snapshot)\n"
                 "  physical DIP: total=%llu avg/frame=%.1f max=%u\n"
                 "  logical DIP:  total=%llu avg/frame=%.1f max=%u\n"
                 "  merge factor logical/physical=%.3fx   logical work merged away=%.1f%%\n"
                 "  frames >=10k physical DIPs=%llu   frames >=30k=%llu\n"
                 "  DIIP physical total=%llu avg/frame=%.1f  instances total=%llu avg/frame=%.1f  tris avg/frame=%.1f\n\n",
                 (unsigned long long)g_censusDipPhysical, avgDip, g_censusMaxDipPhysical,
                 (unsigned long long)g_censusDipLogical, avgLogical, g_censusMaxDipLogical,
                 mergeFactor, mergedPct,
                 (unsigned long long)g_censusFramesOver10k, (unsigned long long)g_censusFramesOver30k,
                 (unsigned long long)g_censusDiipPhysical, double(g_censusDiipPhysical) / frames,
                 (unsigned long long)g_censusInstances, double(g_censusInstances) / frames,
                 double(g_censusTriangles) / frames);
        out += b;

        {
            uint64_t causeTotal = 0;
            for (size_t i = 0; i < kCensusFlushCauseCount; ++i) causeTotal += g_censusFlushDraws[i];
            const double coverage = g_censusDipPhysical ? 100.0 * double(g_censusBackendGeoDraws) / double(g_censusDipPhysical) : 0.0;
            snprintf(b, sizeof b,
                     "Direct DrawIndexed flush-cause census inside 0x1411E6670\n"
                     "  backend direct draws inside GeoSubmit=%llu (%.1f/frame, %.1f%% of frame physical DIP counter)\n"
                     "  classified=%llu unknown_return=%llu backend_draws_outside_GeoSubmit=%llu\n",
                     (unsigned long long)g_censusBackendGeoDraws, double(g_censusBackendGeoDraws) / frames, coverage,
                     (unsigned long long)causeTotal, (unsigned long long)g_censusBackendUnknownGeoDraws,
                     (unsigned long long)g_censusBackendOtherDraws);
            out += b;
            for (size_t i = 0; i < kCensusFlushCauseCount; ++i)
            {
                const double pct = g_censusBackendGeoDraws ? 100.0 * double(g_censusFlushDraws[i]) / double(g_censusBackendGeoDraws) : 0.0;
                const double perFrame = double(g_censusFlushDraws[i]) / frames;
                const double avgIndices = g_censusFlushDraws[i] ? double(g_censusFlushIndices[i]) / double(g_censusFlushDraws[i]) : 0.0;
                snprintf(b, sizeof b, "  %-28s draws=%llu  %.1f/frame  %5.1f%%  avg_indices=%.1f\n",
                         kCensusFlushCauseName[i], (unsigned long long)g_censusFlushDraws[i], perFrame, pct, avgIndices);
                out += b;
            }
            out += "\nInterpretation for V7.24 identity/packing target:\n";
            out += "  - If noncontiguous_index_range dominates, persistent combined meshes / reordered static geometry has the largest direct-DIP opportunity.\n";
            out += "  - If +48/+56/+64 or geometry-state changes dominate, bucket static surfaces by the complete matching state/resource key before combining.\n";
            out += "  - before_instanced_draw is not a static-brush merge target; repeated XModels should stay on a separate instancing path.\n\n";
        }

        {
            struct PassRow { int c; int p; uint64_t draws; };
            std::vector<PassRow> rows;
            for (int c = 0; c < 4; ++c)
                for (int p = 0; p < 10; ++p)
                    if (g_censusPassGeoDraws[c][p]) rows.push_back({c, p, g_censusPassGeoDraws[c][p]});
            std::sort(rows.begin(), rows.end(), [](const PassRow& a, const PassRow& b) { return a.draws > b.draws; });

            out += "V7.22 direct-draw / GeoCache-layout attribution by renderer pass\n";
            out += "  (Use a normal warm capture for draw ownership. To learn cold-cache allocation ownership, start census BEFORE reloading/opening the map.)\n";
            for (const PassRow& row : rows)
            {
                const uint64_t* causes = g_censusPassFlushDraws[row.c][row.p];
                const uint64_t noncontig = causes[8];
                const uint64_t bind48 = causes[4];
                const uint64_t mesh = causes[0];
                uint64_t other = row.draws;
                if (other >= noncontig) other -= noncontig; else other = 0;
                if (other >= bind48) other -= bind48; else other = 0;
                if (other >= mesh) other -= mesh; else other = 0;
                const char* name = DeepPassName(row.p, row.c);
                snprintf(b, sizeof b,
                         "  c%d/p%d %-24s direct=%llu (%.1f/frame, %.1f%% geo) noncontig=%llu (%.1f%% pass) +48=%llu mesh/decl=%llu other=%llu cold_misses=%llu verts=%llu inds=%llu\n",
                         row.c, row.p, name ? name : "unknown",
                         (unsigned long long)row.draws, double(row.draws) / frames,
                         g_censusBackendGeoDraws ? 100.0 * double(row.draws) / double(g_censusBackendGeoDraws) : 0.0,
                         (unsigned long long)noncontig, row.draws ? 100.0 * double(noncontig) / double(row.draws) : 0.0,
                         (unsigned long long)bind48, (unsigned long long)mesh, (unsigned long long)other,
                         (unsigned long long)g_censusPassGeoMissCopies[row.c][row.p],
                         (unsigned long long)g_censusPassGeoMissVertices[row.c][row.p],
                         (unsigned long long)g_censusPassGeoMissIndices[row.c][row.p]);
                out += b;
            }
            if (g_censusUnknownPassGeoDraws || g_censusUnknownPassGeoMissCopies)
            {
                snprintf(b, sizeof b, "  unknown-pass direct=%llu cold_misses=%llu\n",
                         (unsigned long long)g_censusUnknownPassGeoDraws,
                         (unsigned long long)g_censusUnknownPassGeoMissCopies);
                out += b;
            }
            out += "  Decision rule: target the pass with the largest noncontiguous draw count; cold_misses reveal which earlier pass currently determines persistent GeoCache allocation order.\n\n";
        }

        {
            out += "V7.22 alternate stage-5/6 + unknown GeoSubmit owner attribution\n";
            for (int si = 0; si < 2; ++si)
            {
                const unsigned stage = unsigned(si + 5);
                const uint64_t draws = g_censusSpecialGeoDraws[si];
                const uint64_t noncontig = g_censusSpecialFlushDraws[si][8];
                snprintf(b, sizeof b,
                         "  special-stage-%u direct=%llu noncontig=%llu cold_misses=%llu verts=%llu inds=%llu\n",
                         stage, (unsigned long long)draws, (unsigned long long)noncontig,
                         (unsigned long long)g_censusSpecialGeoMissCopies[si],
                         (unsigned long long)g_censusSpecialGeoMissVertices[si],
                         (unsigned long long)g_censusSpecialGeoMissIndices[si]);
                out += b;
            }

            struct CallerRow { int slot; uint64_t misses; uint64_t draws; };
            std::vector<CallerRow> callers;
            for (int i = 0; i < kV722CallerBucketCount; ++i)
                if (g_censusUnknownCallers[i].caller)
                    callers.push_back({i, g_censusUnknownCallers[i].coldMisses, g_censusUnknownCallers[i].draws});
            std::sort(callers.begin(), callers.end(), [](const CallerRow& a, const CallerRow& b)
            {
                if (a.misses != b.misses) return a.misses > b.misses;
                return a.draws > b.draws;
            });
            const size_t show = callers.size() < 12 ? callers.size() : size_t(12);
            for (size_t ri = 0; ri < show; ++ri)
            {
                const V722CallerBucket& cb = g_censusUnknownCallers[callers[ri].slot];
                snprintf(b, sizeof b,
                         "  caller=0x%llX submits=%llu surfaces=%llu direct=%llu noncontig=%llu cold_misses=%llu verts=%llu inds=%llu\n",
                         (unsigned long long)cb.caller, (unsigned long long)cb.submits,
                         (unsigned long long)cb.surfaces, (unsigned long long)cb.draws,
                         (unsigned long long)cb.noncontig, (unsigned long long)cb.coldMisses,
                         (unsigned long long)cb.coldVerts, (unsigned long long)cb.coldInds);
                out += b;
            }
            snprintf(b, sizeof b, "  caller_bucket_overflow=%llu address_bias=0x%llX\n\n",
                     (unsigned long long)g_censusUnknownCallerOverflow, (unsigned long long)g_v722AddressBias);
            out += b;
        }

        out += "V7.23 learned cold-layout packer (RETIRED in V7.24)\n";
        out += V723Status();
        out += "\nV7.24 stable geometry-key overlap census\n";
        out += V724Status();
        out += "\n";

        snprintf(b, sizeof b,
                 "Instance range accounting\n"
                 "  ranges=%llu requested_surfaces=%llu processed=%llu partial=%llu\n\n"
                 "How to read this:\n"
                 "  - Large coalesce-range ms/frame + many MB/frame => persistent static coalesced geometry is a strong first target.\n"
                 "  - Small coalesce cost but high physical DIP count / low merge factor => state sorting + static draw batching is the stronger target.\n"
                 "  - Near-zero GeoCache misses/resets => do NOT spend a build making GeoCache more persistent; it is already warm.\n"
                 "  - Large geo-submit ms/frame with low coalesce cost means the remaining state/resource binding and draw submission loop deserves the next RE pass.\n"
                 "  - V7.22 also separates the alternate stage-5/6 submit path and prints unknown GeoSubmit caller addresses for cold-cache ownership.\n",
                 (unsigned long long)g_censusInstanceRanges, (unsigned long long)g_censusInstanceRequested,
                 (unsigned long long)g_censusInstanceProcessed, (unsigned long long)g_censusInstancePartial);
        out += b;
        return out;
    }

    void DeepReset(bool start)
    {
        for (int p = 0; p < 10; ++p)
            for (int c = 0; c < 4; ++c)
                g_deepPass[p][c].Reset();
        g_deepUnknownPass.Reset();
        g_deepSortA.Reset();
        g_deepSortB.Reset();
        g_deepGeoSubmit.Reset();
        g_deepGeoGrow.Reset();
        g_deepInstance.Reset();
        g_deepPrefabLoop.Reset();
        g_deepMapParser.Reset();
        g_deepAssetOpen.Reset();
        g_deepAssetDecomp.Reset();
        g_v6MapLoadTime.Reset();
        V6ResetCaptureCounters();

        g_deepGeoGrowCalls = g_deepGeoGrowSuccess = g_deepGeoGrowFail = g_deepGeoResizeEvents = 0;
        g_deepMaxVB = g_deepMaxIB = g_deepMaxUsedVB = g_deepMaxUsedIB = g_deepMaxAddVB = g_deepMaxAddIB = 0;
        g_deepInstanceRanges = g_deepInstanceSurfacesRequested = g_deepInstanceSurfacesProcessed = g_deepInstancePartialRanges = 0;
        g_deepMapParserEntities = g_deepAssetOpenSuccess = g_deepAssetOpenFail = g_deepDecompressedBytes = 0;

        g_deepStart = DeepNow();
        g_deepStop = g_deepStart;
        g_deepCapture = start;
    }

    void DeepStop()
    {
        if (g_deepCapture)
        {
            g_deepStop = DeepNow();
            g_deepCapture = false;
        }
    }

    double DeepSeconds()
    {
        const uint64_t end = g_deepCapture ? DeepNow() : g_deepStop;
        if (!g_deepStart || end <= g_deepStart || !g_qpf.QuadPart) return 0.0;
        return double(end - g_deepStart) / double(g_qpf.QuadPart);
    }

    const char* DeepPassName(int p, int c)
    {
        if (c == 0 && p == 0) return "Prepass 0";
        if (c == 0 && p == 3) return "Prepass 3";
        if (c == 0 && p == 4) return "Prepass 4";
        if (c == 1 && p == 1) return "GBuffer Opaque";
        if (c == 1 && p == 2) return "GBuffer Semi Opaque";
        if (c == 1 && p == 6) return "GBuffer Decal";
        if (c == 2 && p == 5) return "Sky";
        if (c == 2 && p == 0) return "Emissive Opaque";
        if (c == 2 && p == 3) return "Lit Forward Opaque";
        if (c == 2 && p == 4) return "Lit Forward SSS";
        if (c == 2 && p == 7) return "Lit Forward Decal";
        if (c == 2 && p == 8) return "Distortion FX";
        if (c == 2 && p == 9) return "Transparent";
        if (c == 3 && p == 9) return "OIT Draw";
        return nullptr;
    }

    void DeepAppendMetric(std::string& out, const char* name, const DeepMetric& m, double seconds)
    {
        if (!m.calls) return;
        const double total = PerfV2TicksToMs(m.totalTicks);
        const double avg = total / double(m.calls);
        const double minv = PerfV2TicksToMs(m.minTicks);
        const double maxv = PerfV2TicksToMs(m.maxTicks);
        char b[512];
        snprintf(b, sizeof b,
                 "  %-28s calls=%llu rate=%.2f/s total=%.3fms avg=%.3fms min=%.3fms max=%.3fms\n",
                 name, (unsigned long long)m.calls, seconds > 0.0 ? double(m.calls) / seconds : 0.0,
                 total, avg, minv, maxv);
        out += b;
    }

    std::string DeepReport()
    {
        const double seconds = DeepSeconds();
        std::string out;
        char b[1024];
        snprintf(b, sizeof b,
                 "Radiant+ Deep V7.1 Full-Fidelity Report\nState: %s  Window: %.3f s\n"
                 "Real-lighting shortcut: RETIRED (camera mode is never changed by V7)\n"
                 "Hooks: pass=%d sortA=%d sortB=%d geo=%d grow=%d inst=%d prefab=%d map=%d cache_open=%d cache_decomp=%d map_load=%d counters=%d\n\n",
                 g_deepCapture ? "RUNNING" : "STOPPED", seconds,
                 g_oDeepRenderPass ? 1 : 0, g_oDeepSortA ? 1 : 0, g_oDeepSortB ? 1 : 0,
                 g_oDeepGeoSubmit ? 1 : 0, g_oDeepGeoGrow ? 1 : 0, g_oDeepInstance ? 1 : 0,
                 g_oDeepPrefabLoop ? 1 : 0, g_oDeepMapParser ? 1 : 0,
                 g_oDeepAssetOpen ? 1 : 0, g_oDeepAssetDecomp ? 1 : 0,
                 g_oV6MapLoad ? 1 : 0,
                 (g_deepDip && g_deepDiip && g_deepTris) ? 1 : 0);
        out += b;

        out += "Renderer pass dispatcher (counter deltas inside each pass)\n";
        uint64_t tdip = 0, tlogical = 0, tdiip = 0, tinst = 0, ttris = 0;
        for (int c = 0; c < 4; ++c)
        {
            for (int p = 0; p < 10; ++p)
            {
                const DeepPassMetric& m = g_deepPass[p][c];
                if (!m.time.calls) continue;
                const double total = PerfV2TicksToMs(m.time.totalTicks);
                const double avg = total / double(m.time.calls);
                const char* n = DeepPassName(p, c);
                char name[80];
                if (n) snprintf(name, sizeof name, "%s", n);
                else snprintf(name, sizeof name, "pass %d / ctx %d", p, c);
                const double combine = m.dipCalls ? double(m.dipLogical) / double(m.dipCalls) : 0.0;
                snprintf(b, sizeof b,
                         "  %-24s p=%d c=%d calls=%llu total=%.3fms avg=%.3fms DIP=%llu/%llu combine=%.2fx DIIP=%llu inst=%llu tris=%llu\n",
                         name, p, c, (unsigned long long)m.time.calls, total, avg,
                         (unsigned long long)m.dipCalls, (unsigned long long)m.dipLogical, combine,
                         (unsigned long long)m.diipCalls, (unsigned long long)m.instances,
                         (unsigned long long)m.tris);
                out += b;
                tdip += m.dipCalls;
                tlogical += m.dipLogical;
                tdiip += m.diipCalls;
                tinst += m.instances;
                ttris += m.tris;
            }
        }
        if (g_deepUnknownPass.time.calls)
        {
            const DeepPassMetric& m = g_deepUnknownPass;
            snprintf(b, sizeof b,
                     "  unknown/out-of-range: calls=%llu DIP=%llu/%llu DIIP=%llu inst=%llu tris=%llu\n",
                     (unsigned long long)m.time.calls, (unsigned long long)m.dipCalls,
                     (unsigned long long)m.dipLogical, (unsigned long long)m.diipCalls,
                     (unsigned long long)m.instances, (unsigned long long)m.tris);
            out += b;
        }
        snprintf(b, sizeof b,
                 "  TOTAL dispatcher counters: DIP=%llu/%llu combine=%.2fx DIIP=%llu inst=%llu tris=%llu\n\n",
                 (unsigned long long)tdip, (unsigned long long)tlogical,
                 tdip ? double(tlogical) / double(tdip) : 0.0,
                 (unsigned long long)tdiip, (unsigned long long)tinst, (unsigned long long)ttris);
        out += b;

        out += "Renderer internals\n";
        DeepAppendMetric(out, "sort A", g_deepSortA, seconds);
        DeepAppendMetric(out, "sort B", g_deepSortB, seconds);
        DeepAppendMetric(out, "GeoCache submit", g_deepGeoSubmit, seconds);
        DeepAppendMetric(out, "GeoCache grow/check", g_deepGeoGrow, seconds);
        DeepAppendMetric(out, "InstanceCache pack/map", g_deepInstance, seconds);
        snprintf(b, sizeof b,
                 "  GeoCache: grow_calls=%llu success=%llu failed/full_flush=%llu resize_events=%llu max_capacity(v/i)=%u/%u max_used(v/i)=%u/%u max_add(v/i)=%u/%u\n"
                 "  InstanceCache: ranges=%llu surfaces_requested=%llu surfaces_processed=%llu partial_ranges=%llu\n\n",
                 (unsigned long long)g_deepGeoGrowCalls, (unsigned long long)g_deepGeoGrowSuccess,
                 (unsigned long long)g_deepGeoGrowFail, (unsigned long long)g_deepGeoResizeEvents,
                 g_deepMaxVB, g_deepMaxIB, g_deepMaxUsedVB, g_deepMaxUsedIB, g_deepMaxAddVB, g_deepMaxAddIB,
                 (unsigned long long)g_deepInstanceRanges,
                 (unsigned long long)g_deepInstanceSurfacesRequested,
                 (unsigned long long)g_deepInstanceSurfacesProcessed,
                 (unsigned long long)g_deepInstancePartialRanges);
        out += b;

        out += "Loading / asset cache\n";
        DeepAppendMetric(out, "Map_LoadFile total", g_v6MapLoadTime, seconds);
        DeepAppendMetric(out, "prefab serial loop", g_deepPrefabLoop, seconds);
        DeepAppendMetric(out, "iwmap parser", g_deepMapParser, seconds);
        DeepAppendMetric(out, "asset-cache open/read", g_deepAssetOpen, seconds);
        DeepAppendMetric(out, "asset-cache decompress", g_deepAssetDecomp, seconds);
        snprintf(b, sizeof b,
                 "  parser_entities=%llu cache_open_ok=%llu cache_open_fail=%llu decompressed_bytes=%llu\n",
                 (unsigned long long)g_deepMapParserEntities,
                 (unsigned long long)g_deepAssetOpenSuccess,
                 (unsigned long long)g_deepAssetOpenFail,
                 (unsigned long long)g_deepDecompressedBytes);
        out += b;
        out += "V6 optimizations\n  ";
        out += V6Status();
        out += "\n";
        return out;
    }

    void FpsDeepSnapshotQueue(int passId, unsigned int context)
    {
        if (!g_fpsDeepCapture || !g_fpsDeepQueueBase || passId < 0 || passId >= 10 || context >= 4) return;

        FpsDeepQueueMetric& m = g_fpsDeepQueue[passId][context];
        ++m.dispatches;

        __try
        {
            uint8_t* q = g_fpsDeepQueueBase + size_t(passId) * 32u;
            const uint32_t groupCount = *reinterpret_cast<volatile uint32_t*>(q + 0);
            uint64_t* groupPtrs = *reinterpret_cast<uint64_t**>(q + 8);

            // Corrupt or transitioning data should not happen, but diagnostics must never take Radiant down.
            if (groupCount > 1000000u || (groupCount && !groupPtrs))
            {
                ++g_fpsDeepQueueFaults;
                return;
            }

            uint64_t recordsThisDispatch = 0;
            uint64_t nonEmpty = 0;
            for (uint32_t i = 0; i < groupCount; ++i)
            {
                const uint64_t group = groupPtrs[i];
                if (!group) continue;
                const uint64_t vecObj = *reinterpret_cast<const uint64_t*>(group + 40);
                if (!vecObj) continue;

                const uint64_t begin = *reinterpret_cast<const uint64_t*>(vecObj + 0);
                const uint64_t end   = *reinterpret_cast<const uint64_t*>(vecObj + 8);
                if (end < begin) { ++g_fpsDeepQueueFaults; continue; }

                const uint64_t bytes = end - begin;
                if (bytes % 208u) { ++g_fpsDeepQueueFaults; continue; }
                const uint64_t n = bytes / 208u;
                if (n)
                {
                    ++nonEmpty;
                    recordsThisDispatch += n;
                }
            }

            m.groups += groupCount;
            m.nonEmptyGroups += nonEmpty;
            m.records += recordsThisDispatch;
            if (groupCount > m.maxGroups) m.maxGroups = groupCount;
            if (recordsThisDispatch > m.maxRecords) m.maxRecords = recordsThisDispatch;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ++g_fpsDeepQueueFaults;
        }
    }

    __int64 __fastcall hk_DeepRenderPass(int passId, unsigned int context)
    {
        if (g_fpsDeepCapture) FpsDeepSnapshotQueue(passId, context);

        const int prevPass = g_v718PassId;
        const unsigned int prevContext = g_v718PassContext;
        ++g_v718PassDepth;
        g_v718PassId = passId;
        g_v718PassContext = context;

        PerfV73QueueSwap compactSwap{};
        PerfV73CompactBegin(passId, compactSwap);

        if (!g_deepCapture)
        {
            const __int64 r = g_oDeepRenderPass(passId, context);
            PerfV73CompactEnd(compactSwap);
            if (g_v718PassDepth) --g_v718PassDepth;
            g_v718PassId = prevPass;
            g_v718PassContext = prevContext;
            return r;
        }

        const DeepDrawSnapshot before = DeepReadDraw();
        const uint64_t begin = DeepNow();
        const __int64 r = g_oDeepRenderPass(passId, context);
        const DeepDrawSnapshot after = DeepReadDraw();
        PerfV73CompactEnd(compactSwap);

        DeepPassMetric* m = (passId >= 0 && passId < 10 && context < 4)
            ? &g_deepPass[passId][context] : &g_deepUnknownPass;
        DeepRecord(m->time, begin);
        DeepAddDrawDelta(*m, before, after);

        if (g_v718PassDepth) --g_v718PassDepth;
        g_v718PassId = prevPass;
        g_v718PassContext = prevContext;
        return r;
    }

    __int64 __fastcall hk_DeepSortA(__int64 beginPtr, __int64 endPtr, __int64 depth, unsigned char flag)
    {
        if (!g_deepCapture)
            return g_oDeepSortA(beginPtr, endPtr, depth, flag);
        const uint64_t begin = DeepNow();
        const __int64 r = g_oDeepSortA(beginPtr, endPtr, depth, flag);
        DeepRecord(g_deepSortA, begin);
        return r;
    }

    __int64 __fastcall hk_DeepSortB(__int64 beginPtr, __int64 endPtr, __int64 depth, unsigned char flag)
    {
        if (!g_deepCapture)
            return g_oDeepSortB(beginPtr, endPtr, depth, flag);
        const uint64_t begin = DeepNow();
        const __int64 r = g_oDeepSortB(beginPtr, endPtr, depth, flag);
        DeepRecord(g_deepSortB, begin);
        return r;
    }

    __int64 __fastcall hk_V722SpecialStage(unsigned int stage)
    {
        const unsigned int prevStage = g_v722SpecialStage;
        ++g_v722SpecialStageDepth;
        g_v722SpecialStage = stage;
        const __int64 r = g_oV722SpecialStage(stage);
        if (g_v722SpecialStageDepth) --g_v722SpecialStageDepth;
        g_v722SpecialStage = prevStage;
        return r;
    }

    __int64 __fastcall hk_DeepGeoSubmit(__int64 a1, __int64 a2, unsigned int a3)
    {
        const bool deep = g_deepCapture;
        const bool census = g_censusCapture;
        const bool staticBatch = g_v8StaticBatchEnabled && V8ResolveReady();
        const bool normalPassKnown = g_v718PassDepth && g_v718PassContext < 4 && g_v718PassId >= 0 && g_v718PassId < 10;
        const bool specialPassKnown = V722SpecialIndex() >= 0;
        const int prevCallerSlot = g_v722UnknownCallerSlot;
        if (census && !normalPassKnown && !specialPassKnown)
            g_v722UnknownCallerSlot = V722FindCallerSlot(V722StaticAddress(_ReturnAddress()), a2 > 0 ? uint64_t(a2) : 0);
        else
            g_v722UnknownCallerSlot = -1;

        const uint32_t v723Count = (a2 > 0 && a2 <= 0x100000) ? uint32_t(a2) : 0u;
        const uintptr_t v723Return = reinterpret_cast<uintptr_t>(_ReturnAddress());
        if (g_v723LearnEnabled && g_v718PassDepth && V723TargetMatches(g_v718PassId, g_v718PassContext))
            V723LearnFromList(uintptr_t(a1), v723Count);

        // Cold priming is retired (ranked=0 packing doubled direct DIPs), never call V723PrimeList.
        g_v723ColdPackEnabled = false;
        g_v723ColdPackArmed = false;

        if (g_v724LearnEnabled && g_v718PassDepth && g_v718PassContext == 3 && g_v718PassId == 9)
            V724LearnFromList(uintptr_t(a1), v723Count);
        if (g_v724ProbeEnabled)
        {
            const bool ownerStage5 = g_v722SpecialStageDepth && g_v722SpecialStage == 5;
            const bool ownerDispatcher = V722StaticAddress(reinterpret_cast<void*>(v723Return)) == kV723NormalDispatcherGeoSubmitReturn;
            if (ownerStage5 || ownerDispatcher)
                V724ProbeOwnerList(uintptr_t(a1), v723Count, ownerStage5, ownerDispatcher);
        }

        const bool safePass = g_v718PassDepth && V718SafeOpaquePass(g_v718PassId, g_v718PassContext);
        const bool locality = g_v718LocalityEnabled && safePass && a1 && a2 > 1 && a2 <= 0x100000;
        if (g_v718LocalityEnabled && !safePass) ++g_v718UnsafePassSkips;

        V718Plan* localityPlan = nullptr;
        bool localityApplied = false;
        if (locality)
        {
            localityPlan = V718GetPlan(uintptr_t(a1), uint32_t(a2), g_v718PassId, g_v718PassContext, a3);
            if (localityPlan && localityPlan->beneficial && localityPlan->reordered.size() == size_t(a2))
            {
                if (V718CopyPointers(reinterpret_cast<uintptr_t*>(a1), localityPlan->reordered.data(), size_t(a2)))
                {
                    localityApplied = true;
                    ++g_v718PlanApplies;
                    g_v718AppliedSurfaces += uint64_t(a2);
                }
                else
                {
                    ++g_v718WriteFailures;
                    g_v718LocalityEnabled = false; // fail open for all future submits
                }
            }
        }

        if (!deep && !census && !localityApplied && !staticBatch)
        {
            g_v722UnknownCallerSlot = prevCallerSlot;
            return g_oDeepGeoSubmit(a1, a2, a3);
        }

        if (staticBatch)
        {
            ++g_v8GeoSubmitDepth;
            g_v8PendingCount = 0;
            g_v8PendingRenderer = nullptr;
        }
        if (census)
        {
            ++g_censusGeoSubmitDepth;
            ++g_censusSubmitLists;
            if (a2 > 0) g_censusSubmitSurfaces += uint64_t(a2);
        }
        const uint64_t begin = (deep || census) ? DeepNow() : 0;
        const __int64 r = g_oDeepGeoSubmit(a1, a2, a3);
        const uint64_t elapsed = (deep || census) ? DeepNow() - begin : 0;

        // End-of-range stock flushes should leave no pending pieces, never carry geometry into another submit.
        if (staticBatch)
        {
            if (g_v8PendingCount && g_v8PendingRenderer)
            {
                void* stockIB = nullptr;
                if (V8CurrentBindingIsGeoCache(stockIB))
                    V8ReplayPieces(g_v8PendingRenderer, g_v8Pending, g_v8PendingCount);
                g_v8PendingCount = 0;
            }
            if (g_v8GeoSubmitDepth) --g_v8GeoSubmitDepth;
            g_v8PendingRenderer = nullptr;
        }

        // The prepared list is never left modified: later passes and editor code see the stock pointer order.
        if (localityApplied && localityPlan)
        {
            if (!V718CopyPointers(reinterpret_cast<uintptr_t*>(a1), localityPlan->original.data(), size_t(a2)))
            {
                ++g_v718WriteFailures;
                g_v718LocalityEnabled = false;
            }
        }

        if (census && g_censusGeoSubmitDepth) --g_censusGeoSubmitDepth;
        if (deep) CensusRecordElapsed(g_deepGeoSubmit, elapsed);
        if (census)
        {
            CensusRecordElapsed(g_censusGeoSubmit, elapsed);
            g_censusLastSubmitDraw = DeepReadDraw();
            g_censusLastSubmitDrawValid = true;
        }
        g_v722UnknownCallerSlot = prevCallerSlot;
        return r;
    }

    char __fastcall hk_DeepGeoGrow(__int64 cache, int addVerts, int addIndices)
    {
        if (!g_deepCapture) return g_oDeepGeoGrow(cache, addVerts, addIndices);
        const uint32_t beforeVB = cache ? *reinterpret_cast<uint32_t*>(cache + 80) : 0;
        const uint32_t beforeIB = cache ? *reinterpret_cast<uint32_t*>(cache + 84) : 0;
        const uint32_t usedVB = cache ? *reinterpret_cast<uint32_t*>(cache + 88) : 0;
        const uint32_t usedIB = cache ? *reinterpret_cast<uint32_t*>(cache + 92) : 0;
        const uint64_t begin = DeepNow();
        const char r = g_oDeepGeoGrow(cache, addVerts, addIndices);
        DeepRecord(g_deepGeoGrow, begin);
        ++g_deepGeoGrowCalls;
        if (r) ++g_deepGeoGrowSuccess; else ++g_deepGeoGrowFail;
        if (uint32_t(addVerts) > g_deepMaxAddVB) g_deepMaxAddVB = uint32_t(addVerts);
        if (uint32_t(addIndices) > g_deepMaxAddIB) g_deepMaxAddIB = uint32_t(addIndices);
        if (usedVB > g_deepMaxUsedVB) g_deepMaxUsedVB = usedVB;
        if (usedIB > g_deepMaxUsedIB) g_deepMaxUsedIB = usedIB;
        if (cache)
        {
            const uint32_t afterVB = *reinterpret_cast<uint32_t*>(cache + 80);
            const uint32_t afterIB = *reinterpret_cast<uint32_t*>(cache + 84);
            if (afterVB != beforeVB || afterIB != beforeIB) ++g_deepGeoResizeEvents;
            if (afterVB > g_deepMaxVB) g_deepMaxVB = afterVB;
            if (afterIB > g_deepMaxIB) g_deepMaxIB = afterIB;
        }
        return r;
    }

    __int64 __fastcall hk_DeepInstance(__int64 list, unsigned int first, unsigned int last)
    {
        const bool deep = g_deepCapture;
        const bool census = g_censusCapture;
        if (!deep && !census) return g_oDeepInstance(list, first, last);
        const uint64_t begin = DeepNow();
        const __int64 r = g_oDeepInstance(list, first, last);
        const uint64_t elapsed = DeepNow() - begin;
        if (deep)
        {
            CensusRecordElapsed(g_deepInstance, elapsed);
            ++g_deepInstanceRanges;
            g_deepInstanceSurfacesRequested += uint64_t(last - first);
            if (r >= first && uint64_t(r) <= last)
            {
                g_deepInstanceSurfacesProcessed += uint64_t(r - first);
                if (uint64_t(r) < last) ++g_deepInstancePartialRanges;
            }
        }
        if (census)
        {
            CensusRecordElapsed(g_censusInstance, elapsed);
            ++g_censusInstanceRanges;
            if (last >= first) g_censusInstanceRequested += uint64_t(last - first);
            if (r >= first && uint64_t(r) <= last)
            {
                g_censusInstanceProcessed += uint64_t(r - first);
                if (uint64_t(r) < last) ++g_censusInstancePartial;
            }
        }
        return r;
    }

    __int64 __fastcall hk_CensusCoalesceRange(__int64 list, unsigned int first, unsigned int last)
    {
        if (!g_censusCapture) return g_oCensusCoalesceRange(list, first, last);
        const uint64_t begin = DeepNow();
        const __int64 r = g_oCensusCoalesceRange(list, first, last);
        const uint64_t elapsed = DeepNow() - begin;
        CensusRecordElapsed(g_censusCoalesceRange, elapsed);
        ++g_censusRangeCalls;
        if (last >= first) g_censusRangeRequested += uint64_t(last - first);
        if (r >= first && uint64_t(r) <= last)
        {
            g_censusRangeProcessed += uint64_t(r - first);
            if (uint64_t(r) < last) ++g_censusRangePartial;
        }
        return r;
    }

    char __fastcall hk_CensusCoalesceOne(uint32_t* outBaseVertex, unsigned int* outBaseIndex,
                                          __int64 vertices, int vertexCount, unsigned int vertexStride,
                                          __int64 indices, int indexCount, char useBaseVertex, __int64 flushState)
    {
        if (!g_censusCapture)
            return g_oCensusCoalesceOne(outBaseVertex, outBaseIndex, vertices, vertexCount, vertexStride,
                                        indices, indexCount, useBaseVertex, flushState);

        ++g_censusCoalesceCalls;
        const char r = g_oCensusCoalesceOne(outBaseVertex, outBaseIndex, vertices, vertexCount, vertexStride,
                                             indices, indexCount, useBaseVertex, flushState);
        if (r)
        {
            ++g_censusCoalesceSuccess;
            if (vertexCount > 0)
            {
                const uint64_t vc = uint64_t(uint32_t(vertexCount));
                g_censusCoalesceVertices += vc;
                g_censusCoalesceVertexBytes += vc * uint64_t(vertexStride);
            }
            if (indexCount > 0)
            {
                const uint64_t ic = uint64_t(uint32_t(indexCount));
                g_censusCoalesceIndices += ic;
                g_censusCoalesceIndexBytes += ic * 2ULL;
            }
        }
        else
        {
            ++g_censusCoalesceFail;
        }
        return r;
    }

    unsigned int* __fastcall hk_CensusGeoCopy(__int64 cache, __int64 mesh, uint32_t* outIndex, unsigned int* outVertex)
    {
        if (g_censusCapture && g_censusGeoSubmitDepth && mesh)
        {
            const uint32_t vertices = *reinterpret_cast<const uint32_t*>(mesh + 120);
            const uint32_t indices = *reinterpret_cast<const uint32_t*>(mesh + 112);
            ++g_censusGeoMissCopies;
            g_censusGeoMissVertices += vertices;
            g_censusGeoMissIndices += indices;
            g_censusGeoMissBytes += uint64_t(vertices) * 44ULL + uint64_t(indices) * 2ULL;
            if (g_v718PassDepth && g_v718PassContext < 4 && g_v718PassId >= 0 && g_v718PassId < 10)
            {
                ++g_censusPassGeoMissCopies[g_v718PassContext][g_v718PassId];
                g_censusPassGeoMissVertices[g_v718PassContext][g_v718PassId] += vertices;
                g_censusPassGeoMissIndices[g_v718PassContext][g_v718PassId] += indices;
            }
            else if (const int si = V722SpecialIndex(); si >= 0)
            {
                ++g_censusSpecialGeoMissCopies[si];
                g_censusSpecialGeoMissVertices[si] += vertices;
                g_censusSpecialGeoMissIndices[si] += indices;
            }
            else
            {
                ++g_censusUnknownPassGeoMissCopies;
                if (g_v722UnknownCallerSlot >= 0 && g_v722UnknownCallerSlot < kV722CallerBucketCount)
                {
                    V722CallerBucket& cb = g_censusUnknownCallers[g_v722UnknownCallerSlot];
                    ++cb.coldMisses;
                    cb.coldVerts += vertices;
                    cb.coldInds += indices;
                }
            }
        }
        unsigned int* r = g_oCensusGeoCopy(cache, mesh, outIndex, outVertex);
        g_v718GeoEpoch.fetch_add(1, std::memory_order_release); // cached offsets/layout may have changed
        return r;
    }

    __int64 __fastcall hk_CensusDrawHud(__int64 cam, __int64 a2, int width, int height)
    {
        if (g_censusCapture)
            CensusAddFrame(DeepReadDraw(), true); // entry is before DrawHud clears the DIP/DIIP/TRIS counters
        return g_oCensusDrawHud(cam, a2, width, height);
    }

    __int64 __fastcall hk_CensusBackendDraw(void* renderer, unsigned int indexCount,
                                             unsigned int startIndex, unsigned int baseVertex)
    {
        // No QPC here, this runs once per physical DIP. _ReturnAddress() identifies the flush site inside 0x1411E6670.
        // Only the stock non-contiguous-range flush is suppressed, a real state boundary ends the batch first.
        if (g_v8StaticBatchEnabled && g_v8GeoSubmitDepth && V8ResolveReady())
        {
            void* stockIB = nullptr;
            const bool geoBound = V8CurrentBindingIsGeoCache(stockIB);
            void* ret = _ReturnAddress();
            int cause = -1;
            for (size_t i = 0; i < kCensusFlushCauseCount; ++i)
            {
                if (ret == g_censusFlushReturnRuntime[i]) { cause = int(i); break; }
            }

            if (!geoBound || cause < 0)
            {
                if (g_v8PendingCount && g_v8PendingRenderer)
                    V8ReplayPieces(g_v8PendingRenderer, g_v8Pending, g_v8PendingCount);
                g_v8PendingCount = 0;
                ++g_v8BypassNonGeo;
            }
            else
            {
                const V8DrawPiece cur{indexCount, startIndex, baseVertex};
                if (cause == 8) // 0x1411E7484: flush only because the index range or base page is non-contiguous
                {
                    if (g_v8PendingCount >= kV8MaxPendingPieces)
                    {
                        ++g_v8PendingOverflows;
                        V8FlushPending(renderer, false, stockIB);
                    }
                    g_v8Pending[g_v8PendingCount++] = cur;
                    g_v8PendingRenderer = renderer;
                    // Suppress this redundant draw, the range is replayed or batched at the next state boundary.
                    goto v8_after_draw;
                }

                if (g_v8PendingCount)
                {
                    // Same old state: can include it across a 64K GeoCache base-page change by promoting the batch to R32.
                    if (g_v8PendingCount < kV8MaxPendingPieces)
                    {
                        g_v8Pending[g_v8PendingCount++] = cur;
                        g_v8PendingRenderer = renderer;
                        const size_t consumed = g_v8PendingCount;
                        const bool hadBatchCandidate = consumed >= kV8MinPieces;
                        V8FlushPending(renderer, true, stockIB);
                        if (hadBatchCandidate)
                        {
                            // Counter correction for the current draw (HUD only, rendering is unaffected).
                        }
                        goto v8_after_draw;
                    }
                    V8FlushPending(renderer, false, stockIB);
                }
                // No compatible pending batch: stock boundary draw.
            }
        }
        if (g_censusCapture)
        {
            if (g_censusGeoSubmitDepth)
            {
                ++g_censusBackendGeoDraws;
                const bool passKnown = g_v718PassDepth && g_v718PassContext < 4 && g_v718PassId >= 0 && g_v718PassId < 10;
                const int specialIndex = passKnown ? -1 : V722SpecialIndex();
                if (passKnown) ++g_censusPassGeoDraws[g_v718PassContext][g_v718PassId];
                else if (specialIndex >= 0) ++g_censusSpecialGeoDraws[specialIndex];
                else
                {
                    ++g_censusUnknownPassGeoDraws;
                    if (g_v722UnknownCallerSlot >= 0 && g_v722UnknownCallerSlot < kV722CallerBucketCount)
                        ++g_censusUnknownCallers[g_v722UnknownCallerSlot].draws;
                }

                void* ret = _ReturnAddress();
                bool matched = false;
                for (size_t i = 0; i < kCensusFlushCauseCount; ++i)
                {
                    if (ret == g_censusFlushReturnRuntime[i])
                    {
                        ++g_censusFlushDraws[i];
                        g_censusFlushIndices[i] += uint64_t(indexCount);
                        if (passKnown)
                        {
                            ++g_censusPassFlushDraws[g_v718PassContext][g_v718PassId][i];
                            g_censusPassFlushIndices[g_v718PassContext][g_v718PassId][i] += uint64_t(indexCount);
                        }
                        else if (specialIndex >= 0)
                        {
                            ++g_censusSpecialFlushDraws[specialIndex][i];
                        }
                        else
                        {
                            ++g_censusUnknownPassFlushDraws[i];
                            if (i == 8 && g_v722UnknownCallerSlot >= 0 && g_v722UnknownCallerSlot < kV722CallerBucketCount)
                                ++g_censusUnknownCallers[g_v722UnknownCallerSlot].noncontig;
                        }
                        matched = true;
                        break;
                    }
                }
                if (!matched) ++g_censusBackendUnknownGeoDraws;
            }
            else
            {
                ++g_censusBackendOtherDraws;
            }
        }
        return g_oCensusBackendDraw(renderer, indexCount, startIndex, baseVertex);

    v8_after_draw:
        // Census observes the logical stock callsite even when the physical draw was merged.
        if (g_censusCapture)
        {
            if (g_censusGeoSubmitDepth)
            {
                ++g_censusBackendGeoDraws;
                void* ret2 = _ReturnAddress();
                for (size_t i = 0; i < kCensusFlushCauseCount; ++i)
                    if (ret2 == g_censusFlushReturnRuntime[i]) { ++g_censusFlushDraws[i]; g_censusFlushIndices[i] += uint64_t(indexCount); break; }
            }
            else ++g_censusBackendOtherDraws;
        }
        return 0;
    }

    bool EnsureCensusBackendDrawHook()
    {
        if (g_oCensusBackendDraw) return true;
        if (g_censusBackendHookUnavailable) return false;

        void* globalAddress = rp::host->RadiantAddress(rp::self, kCensusRendererInterfaceGlobal);
        if (!globalAddress)
        {
            rp::Log("fps-v7.24 census: renderer interface global unavailable");
            return false;
        }
        void* renderer = *reinterpret_cast<void**>(globalAddress);
        if (!renderer)
        {
            rp::Log("fps-v7.22 census: renderer interface not initialized yet");
            return false;
        }
        void** vtable = *reinterpret_cast<void***>(renderer);
        if (!vtable || !vtable[12]) // +0x60 / sizeof(void*)
        {
            rp::Log("fps-v7.22 census: renderer DrawIndexed vtable slot unavailable");
            return false;
        }

        for (size_t i = 0; i < kCensusFlushCauseCount; ++i)
            g_censusFlushReturnRuntime[i] = rp::host->RadiantAddress(rp::self, kCensusFlushReturnStatic[i]);

        g_censusBackendDrawTarget = vtable[12];
        if (!rp::host->Detour(rp::self, g_censusBackendDrawTarget, reinterpret_cast<void*>(&hk_CensusBackendDraw),
                              reinterpret_cast<void**>(&g_oCensusBackendDraw)))
        {
            g_censusBackendDrawTarget = nullptr;
            g_censusBackendHookUnavailable = true;
            if (g_v8StaticBatchEnabled)
            {
                g_v8StaticBatchEnabled = false;
                g_v8PendingCount = 0;
            }
            rp::Log("fps-v7.22 census: backend DrawIndexed detour unavailable target=%p; static batching disabled for this session", vtable[12]);
            return false;
        }
        rp::Log("fps-v7.22 census: backend DrawIndexed hooked target=%p", g_censusBackendDrawTarget);
        return true;
    }


    void __fastcall hk_DeepPrefabLoop(__int64 a1)
    {
        if (!g_deepCapture) { g_oDeepPrefabLoop(a1); return; }
        const uint64_t begin = DeepNow();
        g_oDeepPrefabLoop(a1);
        DeepRecord(g_deepPrefabLoop, begin);
    }

    __int64 __fastcall hk_DeepMapParser(__int64 a1, __int64 a2, char a3)
    {
        if (!g_deepCapture) return g_oDeepMapParser(a1, a2, a3);
        const uint64_t begin = DeepNow();
        const __int64 r = g_oDeepMapParser(a1, a2, a3);
        DeepRecord(g_deepMapParser, begin);
        if (r > 0) g_deepMapParserEntities += uint64_t(r);
        return r;
    }

    char __fastcall hk_DeepAssetOpen(__int64 a1, __int64 a2, char a3)
    {
        const bool capture = g_deepCapture;
        const uint64_t begin = capture ? DeepNow() : 0;
        std::string previousKey = std::move(g_v6AssetKey);
        g_v6AssetKey = V6RadiantString(a1);

        const char r = g_oDeepAssetOpen(a1, a2, a3);

        if (r && g_v6TimestampPatched)
        {
            ++g_v6TimestampSkips;
            // the skipped touch, set after the load. Name as 0x140FA0300 formats it ("%s/%s.lz4" into 256 bytes, so cut at 255)
            std::string path = V63CacheRoot() + "/" + g_v6AssetKey + ".lz4";
            if (path.size() > 255) path.resize(255);
            br::assettouch::Queue(path);
        }

        g_v6AssetKey = std::move(previousKey);
        if (capture)
        {
            DeepRecord(g_deepAssetOpen, begin);
            if (r) ++g_deepAssetOpenSuccess; else ++g_deepAssetOpenFail;
        }
        return r;
    }

    __int64 __fastcall hk_DeepAssetDecomp(__int64 a1, void* file)
    {
        const bool capture = g_deepCapture;
        const uint64_t begin = capture ? DeepNow() : 0;

        if (V6MapLoading()) V63RecordRequest(g_v6AssetKey);

        if (V63TryConsume(a1, static_cast<HANDLE>(file)))
        {
            if (capture)
            {
                DeepRecord(g_deepAssetDecomp, begin);
                if (a1) g_deepDecompressedBytes += *reinterpret_cast<uint64_t*>(a1 + 24);
            }
            return a1;
        }

        if (V6AssetRamLoad(a1, static_cast<HANDLE>(file)))
        {
            if (capture)
            {
                DeepRecord(g_deepAssetDecomp, begin);
                if (a1) g_deepDecompressedBytes += *reinterpret_cast<uint64_t*>(a1 + 24);
            }
            return a1;
        }

        const __int64 r = g_oDeepAssetDecomp(a1, file);
        if (r && a1)
            V6AssetRamStore(a1, static_cast<HANDLE>(file));

        if (capture)
        {
            DeepRecord(g_deepAssetDecomp, begin);
            if (a1)
            {
                const uint64_t bytes = *reinterpret_cast<uint64_t*>(a1 + 24);
                g_deepDecompressedBytes += bytes;
            }
        }
        return r;
    }

    __int64 __fastcall hk_V61GeoReset(uint8_t* cache)
    {
        const __int64 r = g_oV61GeoReset(cache);
        if (cache == g_v6GeoCache)
        {
            ++g_v61GeoResets;
            g_v718GeoEpoch.fetch_add(1, std::memory_order_release);
            V8ClearCache(true);
            g_v6GeoPrimed = false;
            if (g_v723ColdPackEnabled) V723ArmForColdLoad();
            // Radiant's native demand growth is left alone by default, manual geo_preallocate remains (a reset cost ~0.7 s, a zm_giant reload has three).
        }
        return r;
    }

    __int64 __fastcall hk_V6MapLoad(char* path)
    {
        const bool capture = g_deepCapture;
        const uint64_t begin = capture ? DeepNow() : 0;
        const LONG depth = InterlockedIncrement(&g_v6MapLoadDepth);
        if (depth == 1)
        {
            ++g_v6MapLoads;
            g_v718SceneEpoch.fetch_add(1, std::memory_order_release);
            if (g_v723ColdPackEnabled) V723ArmForColdLoad();
            V63BeginMapLoad();
            if (g_v6SkipLoadTimestamps) V6SetTimestampBypass(true);
        }

        const __int64 r = g_oV6MapLoad(path);

        if (depth == 1) V63EndMapLoad();
        const LONG after = InterlockedDecrement(&g_v6MapLoadDepth);
        if (after == 0)
        {
            if (!g_v6TouchTailUntil) V6SetTimestampBypass(false);   // else OnTick ends it after the load's tail
            if (rp::host && rp::host->RequestRedraw)
                rp::host->RequestRedraw(rp::self, MODTOOLS_VIEW_3D | MODTOOLS_VIEW_2D);
        }
        if (capture) DeepRecord(g_v6MapLoadTime, begin);
        return r;
    }

    template <typename Fn>
    bool DeepInstall(const char* name, uintptr_t address, Fn detour, Fn* original)
    {
        void* p = rp::host->RadiantAddress(rp::self, address);
        if (!p || !rp::host->Detour(rp::self, p, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original)))
        {
            rp::Log("deep-v6: hook unavailable for %s at 0x%llx", name, (unsigned long long)address);
            return false;
        }
        return true;
    }

    void SetupDeepV5()
    {
        g_deepDip = static_cast<volatile uint32_t*>(rp::host->RadiantAddress(rp::self, kDeepDipCounters));
        g_deepDiip = static_cast<volatile uint32_t*>(rp::host->RadiantAddress(rp::self, kDeepDiipCounters));
        g_deepTris = static_cast<volatile uint32_t*>(rp::host->RadiantAddress(rp::self, kDeepTriCounter));

        g_v6TimestampBranch = static_cast<uint8_t*>(rp::host->RadiantAddress(rp::self, kV6AssetTimestampBranch));
        g_v6AssetBlobVtable = rp::host->RadiantAddress(rp::self, kV6AssetBlobVtable);
        g_v6GeoCache = static_cast<uint8_t*>(rp::host->RadiantAddress(rp::self, kV6GeoCacheObject));
        g_v63Lz4 = reinterpret_cast<V63Lz4Fn>(rp::host->RadiantAddress(rp::self, kV63Lz4Decompress));
        g_v63CacheRootObject = static_cast<uint8_t*>(rp::host->RadiantAddress(rp::self, kV63AssetCacheLocalRoot));
        g_v723GeoResolve = reinterpret_cast<V723GeoResolveFn>(rp::host->RadiantAddress(rp::self, kV723GeoResolveMesh));
        if (g_v6TimestampBranch)
        {
            g_v6TimestampOriginal = *g_v6TimestampBranch;
            g_v6TimestampPatchReady = g_v6TimestampOriginal == 0x74;
            if (!g_v6TimestampPatchReady)
                rp::Log("perf-v6.3.2: timestamp bypass disabled; expected JZ (0x74) at 0x%llx, found 0x%02x",
                        (unsigned long long)kV6AssetTimestampBranch, unsigned(g_v6TimestampOriginal));
        }

        void* geoRuntimeForBias = rp::host->RadiantAddress(rp::self, kDeepGeoSubmit);
        if (geoRuntimeForBias) g_v722AddressBias = reinterpret_cast<uintptr_t>(geoRuntimeForBias) - kDeepGeoSubmit;

        const bool p = DeepInstall("renderer pass dispatcher", kDeepRenderPassDispatch, &hk_DeepRenderPass, &g_oDeepRenderPass);
        const bool sp = DeepInstall("special stage 5/6 submit", kV722SpecialStageSubmit, &hk_V722SpecialStage, &g_oV722SpecialStage);
        const bool sa = DeepInstall("render sort A", kDeepSortA, &hk_DeepSortA, &g_oDeepSortA);
        const bool sb = DeepInstall("render sort B", kDeepSortB, &hk_DeepSortB, &g_oDeepSortB);
        const bool gs = DeepInstall("GeoCache submit", kDeepGeoSubmit, &hk_DeepGeoSubmit, &g_oDeepGeoSubmit);
        const bool gg = DeepInstall("GeoCache grow/check", kDeepGeoGrow, &hk_DeepGeoGrow, &g_oDeepGeoGrow);
        const bool ic = DeepInstall("InstanceCache pack/map", kDeepInstanceCache, &hk_DeepInstance, &g_oDeepInstance);
        const bool pf = DeepInstall("prefab serial loop", kDeepPrefabLoop, &hk_DeepPrefabLoop, &g_oDeepPrefabLoop);
        const bool mp = DeepInstall("iwmap parser", kDeepMapParser, &hk_DeepMapParser, &g_oDeepMapParser);
        const bool ao = DeepInstall("asset-cache open/read", kDeepAssetCacheOpen, &hk_DeepAssetOpen, &g_oDeepAssetOpen);
        const bool ad = DeepInstall("asset-cache decompress", kDeepAssetCacheDecomp, &hk_DeepAssetDecomp, &g_oDeepAssetDecomp);
        const bool ml = DeepInstall("Map_LoadFile thunk", kV6MapLoadFile, &hk_V6MapLoad, &g_oV6MapLoad);
        const bool gr = DeepInstall("GeoCache reset thunk", kV61GeoCacheResetThunk, &hk_V61GeoReset, &g_oV61GeoReset);

        DeepReset(false);
        rp::Log("deep-v6.3.2: hooks pass=%d special56=%d sortA=%d sortB=%d geo=%d grow=%d inst=%d prefab=%d map=%d cache_open=%d cache_decomp=%d map_load=%d geo_reset=%d counters=%d",
                p ? 1 : 0, sp ? 1 : 0, sa ? 1 : 0, sb ? 1 : 0, gs ? 1 : 0, gg ? 1 : 0,
                ic ? 1 : 0, pf ? 1 : 0, mp ? 1 : 0, ao ? 1 : 0, ad ? 1 : 0, ml ? 1 : 0, gr ? 1 : 0,
                (g_deepDip && g_deepDiip && g_deepTris) ? 1 : 0);
        rp::Log("perf-v6.3.2: load_render_suppress=%d skip_cache_timestamp=%d asset_ram=%d cap=%dMB max_entry=%dMB geo_prealloc=%d "
                "prefetch=%d workers=%d prefetch_cap=%dMB max_entry=%dMB lz4=%p root=%p",
                g_v6SuppressLoadRendering ? 1 : 0, g_v6SkipLoadTimestamps ? 1 : 0,
                g_v6AssetRamEnabled ? 1 : 0, g_v6AssetRamMB, g_v6AssetRamMaxEntryMB,
                g_v6GeoPreallocate ? 1 : 0,
                g_v63PrefetchEnabled ? 1 : 0, V63ResolvedWorkerCount(), g_v63PrefetchMB, g_v63PrefetchMaxEntryMB,
                reinterpret_cast<void*>(g_v63Lz4), static_cast<void*>(g_v63CacheRootObject));
    }
    void SetupCensusV717()
    {
        const bool range = DeepInstall("submission coalesce range", kCensusCoalesceRange,
                                       &hk_CensusCoalesceRange, &g_oCensusCoalesceRange);
        const bool one = DeepInstall("submission coalesce one", kCensusCoalesceOne,
                                     &hk_CensusCoalesceOne, &g_oCensusCoalesceOne);
        const bool copy = DeepInstall("GeoCache miss copy", kCensusGeoCopy,
                                      &hk_CensusGeoCopy, &g_oCensusGeoCopy);
        const bool hud = DeepInstall("CCamWnd::DrawHud census snapshot", kCensusDrawHud,
                                     &hk_CensusDrawHud, &g_oCensusDrawHud);
        const bool backend = EnsureCensusBackendDrawHook();
        CensusReset(false);
        rp::Log("fps-v7.22 census: hooks geo(existing)=%d range=%d coalesce=%d geocopy=%d inst(existing)=%d hud=%d backend_draw=%d; OFF by default",
                g_oDeepGeoSubmit ? 1 : 0, range ? 1 : 0, one ? 1 : 0, copy ? 1 : 0,
                g_oDeepInstance ? 1 : 0, hud ? 1 : 0, backend ? 1 : 0);
    }


    enum PerfMetric
    {
        PM_BETTER_RADIANT_TICK,
        PM_MAIN_STATUS,
        PM_UPDATE_WINDOWS,
        PM_RELOAD_ASSETS,
        PM_MOUSE_MOTION,
        PM_CAM_ROUTINE,
        PM_CAM_ON_PAINT,
        PM_CAM_ON_RENDER,
        PM_BRUSH_CACHE,
        PM_BRUSH_CACHE_CAM,
        PM_BRUSH_CACHE_XY,
        PM_CAM_PROCESS_BRUSHES,
        PM_CAM_PROCESS_PASS0,
        PM_CAM_PROCESS_PASS1,
        PM_CAM_PROCESS_PASS2,
        PM_CAM_PROCESS_OTHER,
        PM_XY_ON_RENDER,
        PM_XY_PROCESS_BRUSHES,
        PM_XY_PROCESS_PASS0,
        PM_XY_PROCESS_PASS1,
        PM_XY_PROCESS_OTHER,
        PM_BEGIN_FRAME,
        PM_END_FRAME,
        PM_PRESENT,
        PM_PRESENT_CAM,
        PM_PRESENT_XY,
        PM_PRESENT_OTHER,
        PM_COUNT
    };

    struct LocalProfile
    {
        bool active = false;
        uint64_t startTick = 0;
        uint64_t endTick = 0;
        ModToolsPerfCounter metrics[PM_COUNT] = {};
        uint64_t updateCameraRequests = 0;
        uint64_t update2DRequests = 0;
    } g_profile;

    bool g_metricAvailable[PM_COUNT] = {};
    double g_profileWindowSeconds = 10.0;

    uint64_t g_profileDeadlineMs = 0;
    std::string g_profileLabel;
    bool g_captureUncap = false;
    int g_captureTickSetting = 0;
    int g_captureTimerInterval = -1;
    float g_captureMaxFps = 0.0f;
    int g_captureUpdateXY = -1;
    enum ViewContext { VIEW_NONE, VIEW_CAMERA, VIEW_XY };
    ViewContext g_viewContext = VIEW_NONE;


    // Camera-only object broad-phase using the same bounds and native CullPoint path Radiant uses later.
    using SceneEarlyCullPointFn = __int64 (__fastcall *)(__int64 point, float radius);
    using SceneEarlyGetTransformFn = void* (__fastcall *)(void* out48);
    using SceneEarlyTransformPointFn = void (__fastcall *)(const float* matrix48, const float* in3, float* out3);
    using SceneEarlySpecialRenderFn = void* (__fastcall *)(int mode, __int64 object, __int64 transform, int a4, __int64 drawState);
    using SceneEarlyGeneralRenderFn = char (__fastcall *)(unsigned int mode, __int64 object, __int64 transform, __int64 a4, __int64 drawState);

    SceneEarlyCullPointFn g_sceneEarlyCullPoint = nullptr;
    SceneEarlyGetTransformFn g_sceneEarlyGetTransform = nullptr;
    SceneEarlyTransformPointFn g_sceneEarlyTransformPoint = nullptr;
    SceneEarlySpecialRenderFn g_oSceneEarlySpecialRender = nullptr;
    SceneEarlyGeneralRenderFn g_oSceneEarlyGeneralRender = nullptr;

    using SceneEarlyRadiantBrushCallbackFn = char (__fastcall *)(__int64 callbackState, __int64* entry);
    using SceneEarlyBrushInstCallbackFn = char (__fastcall *)(__int64 callbackState, __int64 object);
    using SceneEarlyVolumeDecalCallbackFn = __int64 (__fastcall *)(__int64 callbackState, __int64* entry);
    using SceneEarlyGenericSceneCallbackFn = char (__fastcall *)(__int64 callbackState, __int64 object);

    SceneEarlyRadiantBrushCallbackFn g_oSceneEarlyRadiantBrushCallback = nullptr;
    SceneEarlyBrushInstCallbackFn g_oSceneEarlyBrushInstCallback = nullptr;
    SceneEarlyVolumeDecalCallbackFn g_oSceneEarlyVolumeDecalCallback = nullptr;
    SceneEarlyGenericSceneCallbackFn g_oSceneEarlyGenericSceneCallback = nullptr;

    bool g_sceneEarlyRadiantBrushHook = false;
    bool g_sceneEarlyBrushInstHook = false;
    bool g_sceneEarlyVolumeDecalHook = false;
    bool g_sceneEarlyGenericSceneHook = false;

    uint64_t g_sceneEarlyRadiantBrushCalls = 0;
    uint64_t g_sceneEarlyBrushInstCalls = 0;
    uint64_t g_sceneEarlyVolumeDecalCalls = 0;
    uint64_t g_sceneEarlyGenericSceneCalls = 0;
    uint64_t g_sceneEarlyRadiantBrushRejected = 0;
    uint64_t g_sceneEarlyBrushInstRejected = 0;

    bool g_sceneEarlyEnabled = false;
    bool g_sceneEarlySpecialHook = false;
    bool g_sceneEarlyGeneralHook = false;
    float g_sceneEarlyRadiusScale = 1.15f;
    float g_sceneEarlyRadiusPad = 8.0f;

    uint64_t g_sceneEarlySpecialCalls = 0;
    uint64_t g_sceneEarlyGeneralCalls = 0;
    uint64_t g_sceneEarlyTests = 0;
    uint64_t g_sceneEarlyRejected = 0;
    uint64_t g_sceneEarlyPassed = 0;
    uint64_t g_sceneEarlyFailOpen = 0;
    uint64_t g_sceneEarlyFaults = 0;

    void SceneEarlyResetCounters()
    {
        g_sceneEarlySpecialCalls = g_sceneEarlyGeneralCalls = 0;
        g_sceneEarlyTests = g_sceneEarlyRejected = g_sceneEarlyPassed = 0;
        g_sceneEarlyFailOpen = g_sceneEarlyFaults = 0;
        g_sceneEarlyRadiantBrushCalls = 0;
        g_sceneEarlyBrushInstCalls = 0;
        g_sceneEarlyVolumeDecalCalls = 0;
        g_sceneEarlyGenericSceneCalls = 0;
        g_sceneEarlyRadiantBrushRejected = 0;
        g_sceneEarlyBrushInstRejected = 0;
    }

    bool SceneEarlyCanRun()
    {
        return g_sceneEarlyEnabled &&
               g_viewContext == VIEW_CAMERA &&
               g_sceneEarlyCullPoint &&
               g_sceneEarlyGetTransform &&
               g_sceneEarlyTransformPoint;
    }

    bool SceneEarlyReject(__int64 object)
    {
        if (!SceneEarlyCanRun() || !object)
            return false;

        __try
        {
            const float* centerSrc = reinterpret_cast<const float*>(object + 0x1E0);
            float radius = *reinterpret_cast<const float*>(object + 0x1F8);

            if (!std::isfinite(radius) || radius <= 0.0f || radius > 100000000.0f ||
                !std::isfinite(centerSrc[0]) || !std::isfinite(centerSrc[1]) || !std::isfinite(centerSrc[2]))
            {
                ++g_sceneEarlyFailOpen;
                return false;
            }

            alignas(16) float matrix[12] = {};
            void* matrixPtr = g_sceneEarlyGetTransform(matrix);
            if (!matrixPtr)
            {
                ++g_sceneEarlyFailOpen;
                return false;
            }

            float center[3] = { centerSrc[0], centerSrc[1], centerSrc[2] };
            float transformed[3] = {};
            g_sceneEarlyTransformPoint(reinterpret_cast<const float*>(matrixPtr), center, transformed);

            if (!std::isfinite(transformed[0]) || !std::isfinite(transformed[1]) || !std::isfinite(transformed[2]))
            {
                ++g_sceneEarlyFailOpen;
                return false;
            }

            radius = radius * g_sceneEarlyRadiusScale + g_sceneEarlyRadiusPad;
            ++g_sceneEarlyTests;

            const bool visible = (g_sceneEarlyCullPoint(reinterpret_cast<__int64>(transformed), radius) & 0xFF) != 0;
            if (!visible)
            {
                ++g_sceneEarlyRejected;
                return true;
            }

            ++g_sceneEarlyPassed;
            return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ++g_sceneEarlyFaults;
            ++g_sceneEarlyFailOpen;
            return false;
        }
    }

    void* __fastcall hk_SceneEarlySpecialRender(int mode, __int64 object, __int64 transform, int a4, __int64 drawState)
    {
        if (g_sceneEarlyEnabled && g_viewContext == VIEW_CAMERA)
        {
            ++g_sceneEarlySpecialCalls;
            if (SceneEarlyReject(object))
                return nullptr;
        }
        return g_oSceneEarlySpecialRender(mode, object, transform, a4, drawState);
    }

    char __fastcall hk_SceneEarlyGeneralRender(unsigned int mode, __int64 object, __int64 transform, __int64 a4, __int64 drawState)
    {
        if (g_sceneEarlyEnabled && g_viewContext == VIEW_CAMERA)
        {
            ++g_sceneEarlyGeneralCalls;
            if (SceneEarlyReject(object))
                return 0;
        }
        return g_oSceneEarlyGeneralRender(mode, object, transform, a4, drawState);
    }



    // RadiantBrush BVH sidecar: Radiant keeps its category cache (vector #1, camera mask 0x40, stock order). The sidecar rebuilds only when the
    // cache is dirty or the vector changes, queries BVH node spheres once per traversal and yields an admission bitset in vector order.

    struct SpatialRawVector
    {
        uintptr_t begin = 0;
        uintptr_t end = 0;
        uintptr_t capacity = 0;
    };

    struct SpatialItem
    {
        uint32_t originalIndex = 0;
        uintptr_t key = 0;      // first qword of the 16-byte category entry
        float center[3] = {};
        float radius = 0.0f;
    };

    struct SpatialNode
    {
        float center[3] = {};
        float radius = 0.0f;
        uint32_t start = 0;
        uint32_t count = 0;
        int left = -1;
        int right = -1;
        bool leaf = false;
    };

    using SpatialPrepareFn = __int64 (__fastcall *)(__int64 a1, __int64 a2);

    using SpatialIteratorFn = __int64 (__fastcall *)(int mask, __int64 callbackWrapper);
    using SpatialInvokeFn = void (__fastcall *)(uintptr_t callbackObject, uintptr_t* entryKey);

    SpatialPrepareFn g_oSpatialPrepare = nullptr;
    SpatialIteratorFn g_oSpatialIterator = nullptr;
    bool g_spatialIteratorHook = false;
    thread_local bool g_spatialIteratorDispatch = false;
    SpatialRawVector* g_spatialCategory1 = nullptr;
    volatile uint8_t* g_spatialDirtyFlag = nullptr;

    bool g_spatialPrepareHook = false;
    bool g_spatialEnabled = false;
    bool g_spatialBuilt = false;
    std::atomic<bool> g_spatialNeedsRebuild{true};

    uintptr_t g_spatialBuiltBegin = 0;
    uintptr_t g_spatialBuiltEnd = 0;
    uint64_t g_spatialBuildSerial = 0;
    int g_spatialRoot = -1;

    std::vector<SpatialItem> g_spatialItems;
    std::vector<uint32_t> g_spatialInvalidOriginal;
    std::vector<SpatialNode> g_spatialNodes;
    std::vector<uint8_t> g_spatialAdmission;
    std::vector<uint32_t> g_spatialVisibleIndices;

    constexpr uint32_t kSpatialLeafSize = 24;
    constexpr float kSpatialNodeRadiusScale = 1.25f;
    constexpr float kSpatialNodeRadiusPad = 16.0f;

    uint64_t g_spatialBuilds = 0;
    uint64_t g_spatialQueries = 0;
    uint64_t g_spatialCallbacks = 0;
    uint64_t g_spatialSkipped = 0;
    uint64_t g_spatialAdmitted = 0;
    uint64_t g_spatialMismatches = 0;
    uint64_t g_spatialFailOpen = 0;
    uint64_t g_spatialNodeTests = 0;
    uint64_t g_spatialVisibleLeaves = 0;
    uint64_t g_spatialBuildTicks = 0;
    uint64_t g_spatialQueryTicks = 0;
    uint64_t g_spatialIteratorCalls = 0;
    uint64_t g_spatialIteratorCandidates = 0;
    uint64_t g_spatialIteratorInvoked = 0;
    uint64_t g_spatialIteratorSkipped = 0;
    uint64_t g_spatialIteratorFallbacks = 0;
    uint64_t g_spatialIteratorTicks = 0;

    struct SpatialTraversalTLS
    {
        uintptr_t begin = 0;
        uintptr_t end = 0;
        size_t count = 0;
        size_t index = 0;
        uint64_t buildSerial = 0;
        bool active = false;
        bool valid = false;
    };
    thread_local SpatialTraversalTLS g_spatialTls;

    void SpatialResetCounters()
    {
        g_spatialQueries = 0;
        g_spatialCallbacks = 0;
        g_spatialSkipped = 0;
        g_spatialAdmitted = 0;
        g_spatialMismatches = 0;
        g_spatialFailOpen = 0;
        g_spatialNodeTests = 0;
        g_spatialVisibleLeaves = 0;
        g_spatialBuildTicks = 0;
        g_spatialQueryTicks = 0;
        g_spatialIteratorCalls = 0;
        g_spatialIteratorCandidates = 0;
        g_spatialIteratorInvoked = 0;
        g_spatialIteratorSkipped = 0;
        g_spatialIteratorFallbacks = 0;
        g_spatialIteratorTicks = 0;
    }

    double SpatialTicksMs(uint64_t ticks)
    {
        return g_qpf.QuadPart ? (1000.0 * double(ticks) / double(g_qpf.QuadPart)) : 0.0;
    }

    bool SpatialVectorSnapshot(uintptr_t& begin, uintptr_t& end, size_t& count)
    {
        begin = end = 0;
        count = 0;
        if (!g_spatialCategory1)
            return false;

        __try
        {
            begin = g_spatialCategory1->begin;
            end = g_spatialCategory1->end;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }

        if (!begin || end < begin)
            return false;

        const uintptr_t bytes = end - begin;
        if ((bytes & 0xF) != 0)
            return false;

        count = size_t(bytes >> 4);
        // Sanity cap, current captures are far below it.
        if (count > 2000000)
            return false;
        return true;
    }

    bool SpatialReadKey(uintptr_t begin, size_t index, uintptr_t& key)
    {
        key = 0;
        __try
        {
            key = *reinterpret_cast<const uintptr_t*>(begin + index * 16);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool SpatialReadItem(uintptr_t begin, size_t index, SpatialItem& out)
    {
        __try
        {
            const uintptr_t entry = begin + index * 16;
            const uintptr_t key = *reinterpret_cast<const uintptr_t*>(entry);
            if (!key)
                return false;

            const uintptr_t object = *reinterpret_cast<const uintptr_t*>(key + 0x20);
            if (!object)
                return false;

            const float* c = reinterpret_cast<const float*>(object + 0x1E0);
            const float r = *reinterpret_cast<const float*>(object + 0x1F8);

            if (!std::isfinite(c[0]) || !std::isfinite(c[1]) || !std::isfinite(c[2]) ||
                !std::isfinite(r) || r <= 0.0f || r > 100000000.0f)
                return false;

            out.originalIndex = static_cast<uint32_t>(index);
            out.key = key;
            out.center[0] = c[0];
            out.center[1] = c[1];
            out.center[2] = c[2];
            out.radius = r;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void SpatialNodeBounds(uint32_t start, uint32_t count, float outCenter[3], float& outRadius)
    {
        const float maxf = std::numeric_limits<float>::max();
        float mins[3] = { maxf, maxf, maxf };
        float maxs[3] = { -maxf, -maxf, -maxf };

        for (uint32_t i = 0; i < count; ++i)
        {
            const SpatialItem& it = g_spatialItems[start + i];
            for (int a = 0; a < 3; ++a)
            {
                const float lo = it.center[a] - it.radius;
                const float hi = it.center[a] + it.radius;
                if (lo < mins[a]) mins[a] = lo;
                if (hi > maxs[a]) maxs[a] = hi;
            }
        }

        float ex[3] = {};
        for (int a = 0; a < 3; ++a)
        {
            outCenter[a] = (mins[a] + maxs[a]) * 0.5f;
            ex[a] = (maxs[a] - mins[a]) * 0.5f;
        }
        outRadius = std::sqrt(ex[0] * ex[0] + ex[1] * ex[1] + ex[2] * ex[2]);
    }

    int SpatialBuildNode(uint32_t start, uint32_t count)
    {
        const int nodeIndex = static_cast<int>(g_spatialNodes.size());
        g_spatialNodes.emplace_back();

        SpatialNode node;
        node.start = start;
        node.count = count;
        SpatialNodeBounds(start, count, node.center, node.radius);

        if (count <= kSpatialLeafSize)
        {
            node.leaf = true;
            g_spatialNodes[nodeIndex] = node;
            return nodeIndex;
        }

        float cmin[3] = {
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max()
        };
        float cmax[3] = {
            -std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max()
        };

        for (uint32_t i = 0; i < count; ++i)
        {
            const SpatialItem& it = g_spatialItems[start + i];
            for (int a = 0; a < 3; ++a)
            {
                if (it.center[a] < cmin[a]) cmin[a] = it.center[a];
                if (it.center[a] > cmax[a]) cmax[a] = it.center[a];
            }
        }

        const float sx = cmax[0] - cmin[0];
        const float sy = cmax[1] - cmin[1];
        const float sz = cmax[2] - cmin[2];
        int axis = 0;
        if (sy > sx) axis = 1;
        if ((axis == 0 ? sx : sy) < sz) axis = 2;

        const uint32_t leftCount = count / 2;
        const uint32_t mid = start + leftCount;

        std::nth_element(
            g_spatialItems.begin() + start,
            g_spatialItems.begin() + mid,
            g_spatialItems.begin() + start + count,
            [axis](const SpatialItem& a, const SpatialItem& b)
            {
                return a.center[axis] < b.center[axis];
            });

        node.left = SpatialBuildNode(start, leftCount);
        node.right = SpatialBuildNode(mid, count - leftCount);
        node.leaf = false;
        g_spatialNodes[nodeIndex] = node;
        return nodeIndex;
    }

    bool SpatialRebuild(uintptr_t begin, uintptr_t end, size_t count)
    {
        const uint64_t t0 = DeepNow();

        std::vector<SpatialItem> items;
        std::vector<uint32_t> invalid;
        items.reserve(count);
        invalid.reserve(count / 32 + 8);

        for (size_t i = 0; i < count; ++i)
        {
            SpatialItem item;
            if (SpatialReadItem(begin, i, item))
                items.push_back(item);
            else
                invalid.push_back(static_cast<uint32_t>(i));
        }

        g_spatialItems.swap(items);
        g_spatialInvalidOriginal.swap(invalid);
        g_spatialNodes.clear();
        g_spatialNodes.reserve(g_spatialItems.size() * 2 / kSpatialLeafSize + 64);

        g_spatialRoot = -1;
        if (!g_spatialItems.empty())
            g_spatialRoot = SpatialBuildNode(0, static_cast<uint32_t>(g_spatialItems.size()));

        g_spatialBuiltBegin = begin;
        g_spatialBuiltEnd = end;
        g_spatialBuilt = true;
        g_spatialNeedsRebuild.store(false, std::memory_order_release);
        ++g_spatialBuildSerial;
        ++g_spatialBuilds;
        g_spatialBuildTicks += DeepNow() - t0;
        return true;
    }

    bool SpatialQuery(uintptr_t begin, uintptr_t end, size_t count)
    {
        const uint64_t t0 = DeepNow();
        ++g_spatialQueries;

        g_spatialAdmission.assign(count, 0);
        for (uint32_t idx : g_spatialInvalidOriginal)
        {
            if (idx < g_spatialAdmission.size())
                g_spatialAdmission[idx] = 1; // invalid bounds always fail open
        }

        if (!g_sceneEarlyCullPoint || !g_sceneEarlyGetTransform || !g_sceneEarlyTransformPoint ||
            !g_spatialBuilt || g_spatialRoot < 0)
        {
            std::fill(g_spatialAdmission.begin(), g_spatialAdmission.end(), uint8_t(1));
            ++g_spatialFailOpen;
            g_spatialQueryTicks += DeepNow() - t0;
            return false;
        }

        alignas(16) float matrix[12] = {};
        void* matrixPtr = g_sceneEarlyGetTransform(matrix);
        if (!matrixPtr)
        {
            std::fill(g_spatialAdmission.begin(), g_spatialAdmission.end(), uint8_t(1));
            ++g_spatialFailOpen;
            g_spatialQueryTicks += DeepNow() - t0;
            return false;
        }

        std::vector<int> stack;
        stack.reserve(64);
        stack.push_back(g_spatialRoot);

        while (!stack.empty())
        {
            const int ni = stack.back();
            stack.pop_back();

            if (ni < 0 || size_t(ni) >= g_spatialNodes.size())
            {
                ++g_spatialFailOpen;
                std::fill(g_spatialAdmission.begin(), g_spatialAdmission.end(), uint8_t(1));
                g_spatialQueryTicks += DeepNow() - t0;
                return false;
            }

            const SpatialNode& node = g_spatialNodes[size_t(ni)];
            float transformed[3] = {};
            g_sceneEarlyTransformPoint(
                reinterpret_cast<const float*>(matrixPtr),
                node.center,
                transformed);

            if (!std::isfinite(transformed[0]) || !std::isfinite(transformed[1]) || !std::isfinite(transformed[2]))
            {
                ++g_spatialFailOpen;
                std::fill(g_spatialAdmission.begin(), g_spatialAdmission.end(), uint8_t(1));
                g_spatialQueryTicks += DeepNow() - t0;
                return false;
            }

            ++g_spatialNodeTests;
            const float radius = node.radius * kSpatialNodeRadiusScale + kSpatialNodeRadiusPad;
            const bool visible =
                (g_sceneEarlyCullPoint(reinterpret_cast<__int64>(transformed), radius) & 0xFF) != 0;

            if (!visible)
                continue;

            if (node.leaf)
            {
                ++g_spatialVisibleLeaves;
                for (uint32_t i = 0; i < node.count; ++i)
                {
                    const SpatialItem& item = g_spatialItems[node.start + i];
                    if (item.originalIndex < g_spatialAdmission.size())
                        g_spatialAdmission[item.originalIndex] = 1;
                }
            }
            else
            {
                if (node.right >= 0) stack.push_back(node.right);
                if (node.left >= 0) stack.push_back(node.left);
            }
        }

        g_spatialQueryTicks += DeepNow() - t0;
        return true;
    }

    bool SpatialBeginTraversal()
    {
        uintptr_t begin = 0, end = 0;
        size_t count = 0;
        if (!SpatialVectorSnapshot(begin, end, count))
        {
            ++g_spatialFailOpen;
            g_spatialTls = {};
            return false;
        }

        const bool changed =
            !g_spatialBuilt ||
            begin != g_spatialBuiltBegin ||
            end != g_spatialBuiltEnd ||
            g_spatialNeedsRebuild.load(std::memory_order_acquire);

        if (changed)
        {
            if (!SpatialRebuild(begin, end, count))
            {
                ++g_spatialFailOpen;
                g_spatialTls = {};
                return false;
            }
        }

        if (g_spatialAdmission.size() != count)
            g_spatialAdmission.assign(count, uint8_t(1));

        SpatialQuery(begin, end, count);

        // Convert the admission bitset to original-order indices once per traversal (a ~30 KB scan beats ~30k virtual callbacks).
        g_spatialVisibleIndices.clear();
        g_spatialVisibleIndices.reserve(count);
        for (size_t i = 0; i < count; ++i)
        {
            if (g_spatialAdmission[i])
                g_spatialVisibleIndices.push_back(static_cast<uint32_t>(i));
        }

        g_spatialTls.begin = begin;
        g_spatialTls.end = end;
        g_spatialTls.count = count;
        g_spatialTls.index = 0;
        g_spatialTls.buildSerial = g_spatialBuildSerial;
        g_spatialTls.active = true;
        g_spatialTls.valid = true;
        return true;
    }

    bool SpatialAdmitCallback(__int64* entry)
    {
        ++g_spatialCallbacks;

        if (!g_spatialEnabled || g_viewContext != VIEW_CAMERA || !entry)
            return true;

        if (!g_spatialTls.active ||
            g_spatialTls.index >= g_spatialTls.count ||
            g_spatialTls.buildSerial != g_spatialBuildSerial)
        {
            if (!SpatialBeginTraversal())
                return true;
        }

        const size_t idx = g_spatialTls.index++;
        if (!g_spatialTls.valid || idx >= g_spatialAdmission.size())
        {
            ++g_spatialFailOpen;
            return true;
        }

        uintptr_t expectedKey = 0;
        if (!SpatialReadKey(g_spatialTls.begin, idx, expectedKey))
        {
            ++g_spatialFailOpen;
            g_spatialTls.valid = false;
            return true;
        }

        uintptr_t actualKey = 0;
        __try
        {
            actualKey = static_cast<uintptr_t>(*entry);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ++g_spatialFailOpen;
            g_spatialTls.valid = false;
            return true;
        }

        if (expectedKey != actualKey)
        {
            ++g_spatialMismatches;
            ++g_spatialFailOpen;
            g_spatialTls.valid = false;
            return true;
        }

        if (!g_spatialAdmission[idx])
        {
            ++g_spatialSkipped;
            return false;
        }

        ++g_spatialAdmitted;
        return true;
    }


    __int64 SpatialRadiantBrushIteratorImpl(int mask, __int64 callbackWrapper);
    __int64 __fastcall hk_SpatialRadiantBrushIterator(int mask, __int64 callbackWrapper)
    {
        if (g_viewContext != VIEW_CAMERA) return SpatialRadiantBrushIteratorImpl(mask, callbackWrapper);
        // the editor view's walk is prefetched ahead of the callback (camwalk.cpp)
        const int slot = br::camwalk::IterBegin(mask);
        LARGE_INTEGER a, b, f;
        QueryPerformanceCounter(&a);
        const __int64 r = SpatialRadiantBrushIteratorImpl(mask, callbackWrapper);
        QueryPerformanceCounter(&b);
        QueryPerformanceFrequency(&f);
        br::camwalk::IterEnd(slot, double(b.QuadPart - a.QuadPart) * 1000.0 / double(f.QuadPart));
        return r;
    }
    __int64 SpatialRadiantBrushIteratorImpl(int mask, __int64 callbackWrapper)
    {
        // Only the proven 3D-camera category is replaced, every other caller/mask stays stock.
        if (!g_spatialEnabled || g_viewContext != VIEW_CAMERA || mask != 0x40 || !callbackWrapper)
            return g_oSpatialIterator(mask, callbackWrapper);

        const uint64_t t0 = DeepNow();
        ++g_spatialIteratorCalls;

        // Radiant runs the dirty-gated prepare at the start of 0x1402131D0, keep that cache update before taking our snapshot.
        if (!g_oSpatialPrepare)
        {
            ++g_spatialIteratorFallbacks;
            return g_oSpatialIterator(mask, callbackWrapper);
        }

        if (g_spatialDirtyFlag)
        {
            __try
            {
                if (*g_spatialDirtyFlag)
                    g_spatialNeedsRebuild.store(true, std::memory_order_release);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_spatialNeedsRebuild.store(true, std::memory_order_release);
            }
        }

        // The prepare routine is global and dirty-driven (its args are unused), calling it keeps stock category maintenance.
        g_oSpatialPrepare(0, 0);

        uintptr_t begin = 0, end = 0;
        size_t count = 0;
        if (!SpatialVectorSnapshot(begin, end, count) || !SpatialBeginTraversal())
        {
            ++g_spatialIteratorFallbacks;
            g_spatialIteratorTicks += DeepNow() - t0;
            return g_oSpatialIterator(mask, callbackWrapper);
        }

        uintptr_t callbackObject = 0;
        SpatialInvokeFn invoke = nullptr;

        __try
        {
            callbackObject = *reinterpret_cast<const uintptr_t*>(callbackWrapper + 24);
            if (!callbackObject)
            {
                ++g_spatialIteratorFallbacks;
                g_spatialIteratorTicks += DeepNow() - t0;
                return g_oSpatialIterator(mask, callbackWrapper);
            }

            const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(callbackObject);
            if (!vtable)
            {
                ++g_spatialIteratorFallbacks;
                g_spatialIteratorTicks += DeepNow() - t0;
                return g_oSpatialIterator(mask, callbackWrapper);
            }

            invoke = reinterpret_cast<SpatialInvokeFn>(
                *reinterpret_cast<const uintptr_t*>(vtable + 16));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ++g_spatialIteratorFallbacks;
            ++g_spatialFailOpen;
            g_spatialIteratorTicks += DeepNow() - t0;
            return g_oSpatialIterator(mask, callbackWrapper);
        }

        if (!invoke)
        {
            ++g_spatialIteratorFallbacks;
            g_spatialIteratorTicks += DeepNow() - t0;
            return g_oSpatialIterator(mask, callbackWrapper);
        }

        g_spatialIteratorCandidates += count;
        g_spatialIteratorInvoked += g_spatialVisibleIndices.size();
        g_spatialIteratorSkipped += count - g_spatialVisibleIndices.size();

        // The vector is in ascending original-index order, so callback and render order match stock.
        g_spatialIteratorDispatch = true;
        __try
        {
            for (uint32_t originalIndex : g_spatialVisibleIndices)
            {
                if (originalIndex >= count)
                {
                    ++g_spatialFailOpen;
                    continue;
                }

                uintptr_t entryKey = 0;
                if (!SpatialReadKey(begin, originalIndex, entryKey))
                {
                    ++g_spatialFailOpen;
                    continue;
                }

                invoke(callbackObject, &entryKey);
            }
        }
        __finally
        {
            g_spatialIteratorDispatch = false;
        }

        g_spatialIteratorTicks += DeepNow() - t0;

        // Cam_ProcessAllBrushes ignores the return value. The stock one closes an internal profiler scope that this path never opens, so return 0.
        return 0;
    }

    __int64 __fastcall hk_SpatialBrushPrepare(__int64 a1, __int64 a2)
    {
        if (g_spatialDirtyFlag)
        {
            __try
            {
                if (*g_spatialDirtyFlag)
                    g_spatialNeedsRebuild.store(true, std::memory_order_release);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_spatialNeedsRebuild.store(true, std::memory_order_release);
            }
        }
        return g_oSpatialPrepare(a1, a2);
    }

    std::string SpatialStatus()
    {
        char b[1800];
        size_t vectorCount = 0;
        uintptr_t vb = 0, ve = 0;
        SpatialVectorSnapshot(vb, ve, vectorCount);

        const double avgNodes =
            g_spatialQueries ? double(g_spatialNodeTests) / double(g_spatialQueries) : 0.0;
        const double avgIteratorCandidates =
            g_spatialIteratorCalls ? double(g_spatialIteratorCandidates) / double(g_spatialIteratorCalls) : 0.0;
        const double avgIteratorInvoked =
            g_spatialIteratorCalls ? double(g_spatialIteratorInvoked) / double(g_spatialIteratorCalls) : 0.0;
        const double avgIteratorSkipped =
            g_spatialIteratorCalls ? double(g_spatialIteratorSkipped) / double(g_spatialIteratorCalls) : 0.0;

        snprintf(
            b, sizeof b,
            "spatial=%d ready=%d built=%d hooks[prepare=%d iterator=%d] "
            "vector=%llu valid=%llu invalid=%llu nodes=%llu leaf=%u "
            "builds=%llu queries=%llu node_tests=%llu avg_nodes/query=%.1f "
            "iterator[calls=%llu candidates=%llu invoked=%llu skipped=%llu fallbacks=%llu] "
            "avg/iterator[candidates=%.1f invoked=%.1f skipped=%.1f] "
            "callback_fallback[calls=%llu skipped=%llu admitted=%llu mismatch=%llu] "
            "failopen=%llu build_ms=%.3f query_ms=%.3f iterator_ms=%.3f",
            g_spatialEnabled ? 1 : 0,
            (g_spatialPrepareHook && g_spatialIteratorHook && g_spatialCategory1 &&
             g_sceneEarlyCullPoint && g_sceneEarlyGetTransform &&
             g_sceneEarlyTransformPoint && g_sceneEarlyRadiantBrushHook) ? 1 : 0,
            g_spatialBuilt ? 1 : 0,
            g_spatialPrepareHook ? 1 : 0,
            g_spatialIteratorHook ? 1 : 0,
            (unsigned long long)vectorCount,
            (unsigned long long)g_spatialItems.size(),
            (unsigned long long)g_spatialInvalidOriginal.size(),
            (unsigned long long)g_spatialNodes.size(),
            (unsigned)kSpatialLeafSize,
            (unsigned long long)g_spatialBuilds,
            (unsigned long long)g_spatialQueries,
            (unsigned long long)g_spatialNodeTests,
            avgNodes,
            (unsigned long long)g_spatialIteratorCalls,
            (unsigned long long)g_spatialIteratorCandidates,
            (unsigned long long)g_spatialIteratorInvoked,
            (unsigned long long)g_spatialIteratorSkipped,
            (unsigned long long)g_spatialIteratorFallbacks,
            avgIteratorCandidates, avgIteratorInvoked, avgIteratorSkipped,
            (unsigned long long)g_spatialCallbacks,
            (unsigned long long)g_spatialSkipped,
            (unsigned long long)g_spatialAdmitted,
            (unsigned long long)g_spatialMismatches,
            (unsigned long long)g_spatialFailOpen,
            SpatialTicksMs(g_spatialBuildTicks),
            SpatialTicksMs(g_spatialQueryTicks),
            SpatialTicksMs(g_spatialIteratorTicks));
        return b;
    }

    void SetupSpatialV714()
    {
        g_spatialEnabled = false;
        g_spatialBuilt = false;
        g_spatialNeedsRebuild.store(true, std::memory_order_release);
        SpatialResetCounters();

        g_spatialCategory1 = reinterpret_cast<SpatialRawVector*>(
            rp::host->RadiantAddress(rp::self, kSpatialCategoryVectors + 24));
        g_spatialDirtyFlag = reinterpret_cast<volatile uint8_t*>(
            rp::host->RadiantAddress(rp::self, kSpatialBrushDirtyFlag));

        g_spatialPrepareHook =
            DeepInstall("spatial RadiantBrush prepare", kSpatialBrushPrepare,
                        &hk_SpatialBrushPrepare, &g_oSpatialPrepare);

        g_spatialIteratorHook =
            DeepInstall("spatial RadiantBrush iterator", kSpatialRadiantBrushIterator,
                        &hk_SpatialRadiantBrushIterator, &g_oSpatialIterator);

        rp::Log(
            "fps-v7.15 spatial: ready=%d prepare=%d iterator=%d vector1=%p dirty=%p; OFF by default",
            (g_spatialPrepareHook && g_spatialIteratorHook && g_spatialCategory1 &&
             g_spatialDirtyFlag) ? 1 : 0,
            g_spatialPrepareHook ? 1 : 0,
            g_spatialIteratorHook ? 1 : 0,
            static_cast<void*>(g_spatialCategory1),
            static_cast<void*>(const_cast<uint8_t*>(g_spatialDirtyFlag)));
    }

    char __fastcall hk_SceneEarlyRadiantBrushCallback(__int64 callbackState, __int64* entry)
    {
        if (g_viewContext == VIEW_CAMERA)
        {
            br::camwalk::IterCallback(entry);   // the editor view's walk is prefetched ahead (camwalk.cpp)
            ++g_sceneEarlyRadiantBrushCalls;

            if (g_spatialEnabled && !g_spatialIteratorDispatch)
            {
                // Fallback for a camera path that reaches the callback without our iterator hook.
                if (!SpatialAdmitCallback(entry))
                    return 1;
            }
            else if (!g_spatialEnabled && g_sceneEarlyEnabled && entry)
            {
                __try
                {
                    const __int64 v2 = *entry;
                    const __int64 object = v2 ? *reinterpret_cast<const __int64*>(v2 + 0x20) : 0;
                    if (object && SceneEarlyReject(object))
                    {
                        ++g_sceneEarlyRadiantBrushRejected;
                        return 1;
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    ++g_sceneEarlyFaults;
                    ++g_sceneEarlyFailOpen;
                }
            }
        }
        return g_oSceneEarlyRadiantBrushCallback(callbackState, entry);
    }

    char __fastcall hk_SceneEarlyBrushInstCallback(__int64 callbackState, __int64 object)
    {
        if (g_viewContext == VIEW_CAMERA)
        {
            ++g_sceneEarlyBrushInstCalls;
            if (g_sceneEarlyEnabled && object && SceneEarlyReject(object))
            {
                ++g_sceneEarlyBrushInstRejected;
                return 1;
            }
        }
        return g_oSceneEarlyBrushInstCallback(callbackState, object);
    }

    __int64 __fastcall hk_SceneEarlyVolumeDecalCallback(__int64 callbackState, __int64* entry)
    {
        if (g_viewContext == VIEW_CAMERA)
            ++g_sceneEarlyVolumeDecalCalls;
        return g_oSceneEarlyVolumeDecalCallback(callbackState, entry);
    }

    char __fastcall hk_SceneEarlyGenericSceneCallback(__int64 callbackState, __int64 object)
    {
        if (g_viewContext == VIEW_CAMERA)
            ++g_sceneEarlyGenericSceneCalls;
        return g_oSceneEarlyGenericSceneCallback(callbackState, object);
    }

    std::string SceneEarlyStatus()
    {
        char b[1024];
        const double rejectPct = g_sceneEarlyTests ? 100.0 * double(g_sceneEarlyRejected) / double(g_sceneEarlyTests) : 0.0;
        snprintf(b, sizeof b,
                 "sceneearly=%d ready=%d callback_hooks[rbrush=%d brush=%d voldecal=%d generic=%d] "
                 "callback_calls[rbrush=%llu brush=%llu voldecal=%llu generic=%llu] "
                 "tests=%llu rejected=%llu (%.1f%%) family_reject[rbrush=%llu brush=%llu] "
                 "passed=%llu failopen=%llu faults=%llu margin=radius*%.2f+%.1f",
                 g_sceneEarlyEnabled ? 1 : 0,
                 (g_sceneEarlyCullPoint && g_sceneEarlyGetTransform && g_sceneEarlyTransformPoint &&
                  g_sceneEarlyRadiantBrushHook && g_sceneEarlyBrushInstHook) ? 1 : 0,
                 g_sceneEarlyRadiantBrushHook ? 1 : 0,
                 g_sceneEarlyBrushInstHook ? 1 : 0,
                 g_sceneEarlyVolumeDecalHook ? 1 : 0,
                 g_sceneEarlyGenericSceneHook ? 1 : 0,
                 (unsigned long long)g_sceneEarlyRadiantBrushCalls,
                 (unsigned long long)g_sceneEarlyBrushInstCalls,
                 (unsigned long long)g_sceneEarlyVolumeDecalCalls,
                 (unsigned long long)g_sceneEarlyGenericSceneCalls,
                 (unsigned long long)g_sceneEarlyTests,
                 (unsigned long long)g_sceneEarlyRejected,
                 rejectPct,
                 (unsigned long long)g_sceneEarlyRadiantBrushRejected,
                 (unsigned long long)g_sceneEarlyBrushInstRejected,
                 (unsigned long long)g_sceneEarlyPassed,
                 (unsigned long long)g_sceneEarlyFailOpen,
                 (unsigned long long)g_sceneEarlyFaults,
                 double(g_sceneEarlyRadiusScale),
                 double(g_sceneEarlyRadiusPad));
        return b;
    }

    void SetupSceneEarlyV712()
    {
        g_sceneEarlyEnabled = false;
        SceneEarlyResetCounters();

        g_sceneEarlyCullPoint = reinterpret_cast<SceneEarlyCullPointFn>(
            rp::host->RadiantAddress(rp::self, kSceneEarlyCullPoint));
        g_sceneEarlyGetTransform = reinterpret_cast<SceneEarlyGetTransformFn>(
            rp::host->RadiantAddress(rp::self, kSceneEarlyGetTransform));
        g_sceneEarlyTransformPoint = reinterpret_cast<SceneEarlyTransformPointFn>(
            rp::host->RadiantAddress(rp::self, kSceneEarlyTransformPoint));

        // The deeper renderer hooks saw zero calls in the real camera.
        g_sceneEarlySpecialHook = false;
        g_sceneEarlyGeneralHook = false;

        g_sceneEarlyRadiantBrushHook =
            DeepInstall("sceneearly RadiantBrush camera callback", kSceneEarlyRadiantBrushCallback,
                        &hk_SceneEarlyRadiantBrushCallback, &g_oSceneEarlyRadiantBrushCallback);
        g_sceneEarlyBrushInstHook =
            DeepInstall("sceneearly BrushInst camera callback", kSceneEarlyBrushInstCallback,
                        &hk_SceneEarlyBrushInstCallback, &g_oSceneEarlyBrushInstCallback);
        g_sceneEarlyVolumeDecalHook =
            DeepInstall("sceneearly VolumeDecal camera callback", kSceneEarlyVolumeDecalCallback,
                        &hk_SceneEarlyVolumeDecalCallback, &g_oSceneEarlyVolumeDecalCallback);
        g_sceneEarlyGenericSceneHook =
            DeepInstall("sceneearly GenericScene camera callback", kSceneEarlyGenericSceneCallback,
                        &hk_SceneEarlyGenericSceneCallback, &g_oSceneEarlyGenericSceneCallback);

        rp::Log("fps-v7.13 sceneearly: callbacks rbrush=%d brush=%d voldecal=%d generic=%d; OFF by default; camera only",
                g_sceneEarlyRadiantBrushHook ? 1 : 0,
                g_sceneEarlyBrushInstHook ? 1 : 0,
                g_sceneEarlyVolumeDecalHook ? 1 : 0,
                g_sceneEarlyGenericSceneHook ? 1 : 0);
    }

    // 2D viewport cull: XY_ProcessAllBrushes visits every BrushInst and rejects most late in the draw path. This hook returns handled only when the
    // conservative world bounds are fully outside the active XY, XZ or YZ viewport. Bad pointers, odd bounds, unknown viewport or point-like entities fall back to Radiant.

    using GridCullCallbackFn = char (__fastcall *)(__int64 callbackState, __int64 brushInst);

    struct GridCullView
    {
        bool active = false;
        bool probe = false;
        int type = -1;
        int dim1 = 0;
        int dim2 = 1;
        float min1 = 0.0f;
        float max1 = 0.0f;
        float min2 = 0.0f;
        float max2 = 0.0f;
        float padding = 0.0f;
        uint64_t callsStart = 0;
        uint64_t rejectedStart = 0;
    };

    struct GridCullMode
    {
        bool valid = false;
        bool useCull = false;
        int width = 0;
        int height = 0;
        float origin[3] = {};
        float scale = 0.0f;
    };

    GridCullCallbackFn g_oGridCullXYBrushCallback = nullptr;
    bool g_gridCullHook = false;
    bool g_gridCullEnabled = false; // experimental, off by default
    float g_gridCullPadding = 1024.0f;
    uint64_t g_gridCullViews = 0;
    uint64_t g_gridCullCalls = 0;
    uint64_t g_gridCullRejected = 0;
    uint64_t g_gridCullPassed = 0;
    uint64_t g_gridCullFailOpen = 0;
    uint64_t g_gridCullFaults = 0;
    uint64_t g_gridCullInstanceBounds = 0;
    uint64_t g_gridCullBrushBounds = 0;
    uint64_t g_gridCullProbes = 0;
    uint64_t g_gridCullBypassedViews = 0;
    uint64_t g_gridCullDisabledProbes = 0;
    thread_local GridCullView g_gridCullView;
    GridCullMode g_gridCullModes[3] = {};

    constexpr float kGridCullMinUsefulRatio = 0.125f;
    constexpr float kGridCullPanRecheck = 0.25f;
    constexpr float kGridCullScaleRecheck = 0.10f;

    void GridCullResetCounters()
    {
        g_gridCullViews = 0;
        g_gridCullCalls = 0;
        g_gridCullRejected = 0;
        g_gridCullPassed = 0;
        g_gridCullFailOpen = 0;
        g_gridCullFaults = 0;
        g_gridCullInstanceBounds = 0;
        g_gridCullBrushBounds = 0;
        g_gridCullProbes = 0;
        g_gridCullBypassedViews = 0;
        g_gridCullDisabledProbes = 0;
        for (GridCullMode& mode : g_gridCullModes)
            mode = {};
    }

    bool GridCullBegin(void* xy, GridCullView& previous)
    {
        previous = g_gridCullView;
        g_gridCullView = {};
        if (!g_gridCullEnabled || g_viewContext != VIEW_XY || !xy || !rp::host->XYWindow)
            return false;

        __try
        {
            const uint8_t* const wnd = static_cast<const uint8_t*>(xy);
            const int type = *reinterpret_cast<const int*>(wnd + off_XY_ViewType);
            const float scale = *reinterpret_cast<const float*>(wnd + off_XY_Scale);
            const float* const origin = reinterpret_cast<const float*>(wnd + off_XY_Origin);
            if (type < 0 || type > 2 || !std::isfinite(scale) || scale <= 0.0001f || scale > 100000.0f ||
                !std::isfinite(origin[0]) || !std::isfinite(origin[1]) || !std::isfinite(origin[2]))
            {
                ++g_gridCullFailOpen;
                return false;
            }

            HWND hwnd = static_cast<HWND>(rp::host->XYWindow(rp::self, type));
            RECT rect{};
            if (!hwnd || !GetClientRect(hwnd, &rect))
            {
                ++g_gridCullFailOpen;
                return false;
            }
            const int width = rect.right - rect.left;
            const int height = rect.bottom - rect.top;
            if (width < 16 || height < 16 || width > 32768 || height > 32768)
            {
                ++g_gridCullFailOpen;
                return false;
            }

            // Matches CXYWnd::XY_Draw: type 2 = XY, type 1 = XZ, type 0 = YZ.
            const int dim1 = type == 0 ? 1 : 0;
            const int dim2 = type == 2 ? 1 : 2;
            const float halfWidth = float(width) * 0.5f / scale;
            const float halfHeight = float(height) * 0.5f / scale;
            if (!std::isfinite(halfWidth) || !std::isfinite(halfHeight) || halfWidth <= 0.0f || halfHeight <= 0.0f)
            {
                ++g_gridCullFailOpen;
                return false;
            }

            GridCullView view{};
            view.active = true;
            view.dim1 = dim1;
            view.dim2 = dim2;
            view.min1 = origin[dim1] - halfWidth;
            view.max1 = origin[dim1] + halfWidth;
            view.min2 = origin[dim2] - halfHeight;
            view.max2 = origin[dim2] + halfHeight;
            // Exact for regular brushes, with a margin for model/entity bounds whose renderer extent is broader.
            view.padding = std::max(g_gridCullPadding, 8.0f / scale);
            if (!std::isfinite(view.min1) || !std::isfinite(view.max1) ||
                !std::isfinite(view.min2) || !std::isfinite(view.max2) ||
                !std::isfinite(view.padding))
            {
                ++g_gridCullFailOpen;
                return false;
            }

            GridCullMode& mode = g_gridCullModes[type];
            const bool recheck =
                !mode.valid ||
                mode.width != width ||
                mode.height != height ||
                std::fabs(mode.scale - scale) > std::max(0.001f, scale * kGridCullScaleRecheck) ||
                std::fabs(mode.origin[dim1] - origin[dim1]) > halfWidth * kGridCullPanRecheck ||
                std::fabs(mode.origin[dim2] - origin[dim2]) > halfHeight * kGridCullPanRecheck;
            if (recheck)
            {
                mode = {};
                mode.width = width;
                mode.height = height;
                mode.scale = scale;
                memcpy(mode.origin, origin, sizeof mode.origin);
                ++g_gridCullProbes;
            }

            view.type = type;
            view.probe = !mode.valid;
            view.active = view.probe || mode.useCull;
            view.callsStart = g_gridCullCalls;
            view.rejectedStart = g_gridCullRejected;
            if (!view.active)
            {
                ++g_gridCullBypassedViews;
                return false;
            }
            g_gridCullView = view;
            ++g_gridCullViews;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ++g_gridCullFaults;
            ++g_gridCullFailOpen;
            return false;
        }
    }

    void GridCullEnd()
    {
        const GridCullView& view = g_gridCullView;
        if (!view.active || !view.probe || view.type < 0 || view.type > 2)
            return;

        GridCullMode& mode = g_gridCullModes[view.type];
        const uint64_t calls = g_gridCullCalls - view.callsStart;
        const uint64_t rejected = g_gridCullRejected - view.rejectedStart;
        mode.valid = true;
        mode.useCull = calls > 0 && double(rejected) / double(calls) >= kGridCullMinUsefulRatio;
        if (!mode.useCull)
            ++g_gridCullDisabledProbes;
    }

    bool GridCullReject(__int64 brushInst)
    {
        const GridCullView& view = g_gridCullView;
        if (!view.active || !brushInst)
            return false;

        ++g_gridCullCalls;
        __try
        {
            const uint8_t* const inst = reinterpret_cast<const uint8_t*>(brushInst);
            const uint8_t* const brush = *reinterpret_cast<uint8_t* const*>(inst + off_BInst_Brush);
            float lo[3] = {};
            float hi[3] = {};
            bool haveBounds = false;

            // Fixed-size model brushes are often a point or small proxy. BrushInst carries the renderer's cached world AABB at +480 (the native frustum culler's envelope), prefer it.
            const float* const instanceMins = reinterpret_cast<const float*>(inst + off_BInst_WorldMins);
            const float* const instanceMaxs = reinterpret_cast<const float*>(inst + off_BInst_WorldMaxs);
            bool instanceValid = true;
            for (int axis = 0; axis < 3; ++axis)
            {
                if (!std::isfinite(instanceMins[axis]) || !std::isfinite(instanceMaxs[axis]) ||
                    instanceMins[axis] > instanceMaxs[axis])
                    instanceValid = false;
            }
            if (instanceValid)
            {
                for (int axis = 0; axis < 3; ++axis)
                {
                    lo[axis] = instanceMins[axis];
                    hi[axis] = instanceMaxs[axis];
                }
                haveBounds = true;
                ++g_gridCullInstanceBounds;
            }

            // Include the authoring brush bounds too, in case the cached model bounds are stale or narrower.
            if (brush)
            {
                const float* const mins = reinterpret_cast<const float*>(brush + off_Brush_Mins);
                const float* const maxs = reinterpret_cast<const float*>(brush + off_Brush_Maxs);
                const float* const paddedMins = reinterpret_cast<const float*>(brush + off_Brush_PaddedMins);
                const float* const paddedMaxs = reinterpret_cast<const float*>(brush + off_Brush_PaddedMaxs);
                bool brushValid = true;
                for (int axis = 0; axis < 3; ++axis)
                {
                    if (!std::isfinite(mins[axis]) || !std::isfinite(maxs[axis]) || mins[axis] > maxs[axis])
                    {
                        brushValid = false;
                        break;
                    }
                }
                if (brushValid)
                {
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        float brushLo = mins[axis];
                        float brushHi = maxs[axis];
                        if (std::isfinite(paddedMins[axis]) && std::isfinite(paddedMaxs[axis]) && paddedMins[axis] <= paddedMaxs[axis])
                        {
                            brushLo = std::min(brushLo, paddedMins[axis]);
                            brushHi = std::max(brushHi, paddedMaxs[axis]);
                        }
                        if (haveBounds)
                        {
                            lo[axis] = std::min(lo[axis], brushLo);
                            hi[axis] = std::max(hi[axis], brushHi);
                        }
                        else
                        {
                            lo[axis] = brushLo;
                            hi[axis] = brushHi;
                        }
                    }
                    haveBounds = true;
                    ++g_gridCullBrushBounds;
                }
            }

            if (!haveBounds)
            {
                ++g_gridCullFailOpen;
                return false;
            }

            // No assumption for a point-like fixed-size entity whose mesh extent is not in brush_t bounds.
            if (hi[view.dim1] - lo[view.dim1] < 0.01f && hi[view.dim2] - lo[view.dim2] < 0.01f)
            {
                ++g_gridCullFailOpen;
                return false;
            }

            const bool outside =
                lo[view.dim1] > view.max1 + view.padding ||
                hi[view.dim1] < view.min1 - view.padding ||
                lo[view.dim2] > view.max2 + view.padding ||
                hi[view.dim2] < view.min2 - view.padding;
            if (outside)
            {
                ++g_gridCullRejected;
                return true;
            }

            ++g_gridCullPassed;
            return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ++g_gridCullFaults;
            ++g_gridCullFailOpen;
            return false;
        }
    }

    char __fastcall hk_GridCullXYBrushCallback(__int64 callbackState, __int64 brushInst)
    {
        if (g_gridCullEnabled && g_gridCullView.active && GridCullReject(brushInst))
            return 1; // handled: skip Radiant's wireframe dispatcher
        if (br::xylod::Skip(brushInst, nullptr))
            return 1; // too small to matter while the user interacts (xylod.cpp)
        return g_oGridCullXYBrushCallback(callbackState, brushInst);
    }

    std::string GridCullStatus()
    {
        char b[768];
        const double rejectedPct = g_gridCullCalls ? 100.0 * double(g_gridCullRejected) / double(g_gridCullCalls) : 0.0;
        snprintf(b, sizeof b,
                 "gridcull=%d ready=%d padding=%.0f views=%llu probes=%llu bypassed_views=%llu disabled_probes=%llu callbacks=%llu rejected=%llu (%.1f%%) passed=%llu instance_bounds=%llu brush_bounds=%llu failopen=%llu faults=%llu",
                 g_gridCullEnabled ? 1 : 0,
                 g_gridCullHook ? 1 : 0,
                 double(g_gridCullPadding),
                 (unsigned long long)g_gridCullViews,
                 (unsigned long long)g_gridCullProbes,
                 (unsigned long long)g_gridCullBypassedViews,
                 (unsigned long long)g_gridCullDisabledProbes,
                 (unsigned long long)g_gridCullCalls,
                 (unsigned long long)g_gridCullRejected,
                 rejectedPct,
                 (unsigned long long)g_gridCullPassed,
                 (unsigned long long)g_gridCullInstanceBounds,
                 (unsigned long long)g_gridCullBrushBounds,
                 (unsigned long long)g_gridCullFailOpen,
                 (unsigned long long)g_gridCullFaults);
        return b;
    }

    void SetupGridCullV716()
    {
        GridCullResetCounters();
        g_gridCullHook = DeepInstall("gridcull XY BrushInst callback", kGridCullXYBrushCallback,
                                     &hk_GridCullXYBrushCallback, &g_oGridCullXYBrushCallback);
        if (!g_gridCullHook) g_gridCullEnabled = false;
        rp::Log("gridcull-v7.18: callback=%d enabled=%d padding=%.0f; cached model bounds, adaptive fail-open",
                g_gridCullHook ? 1 : 0, g_gridCullEnabled ? 1 : 0, double(g_gridCullPadding));
    }

    using FpsDeepLightDirtyFn = bool (__fastcall *)(void* cam);
    using FpsDeepInputBitsFn = bool (__fastcall *)(int first, int second);
    using FpsDeepSpecialStateFn = bool (__fastcall *)(void* cam);
    using FpsDeepPerObjectFn = char (__fastcall *)(unsigned int mode, __int64 brush, __int64 a3, __int64 a4, __int64 drawState, char a6);

    FpsDeepLightDirtyFn g_oFpsDeepLightDirty = nullptr;
    FpsDeepInputBitsFn g_oFpsDeepInputBits = nullptr;
    FpsDeepSpecialStateFn g_oFpsDeepSpecialState = nullptr;
    FpsDeepPerObjectFn g_oFpsDeepPerObject = nullptr;

    void* g_fpsDeepLightReturn = nullptr;
    void* g_fpsDeepInputReturn = nullptr;
    void* g_fpsDeepSpecialReturn = nullptr;
    uintptr_t* g_fpsDeepJqProfActivePtr = nullptr;
    uint8_t* g_fpsDeepJqProfTriggerPtr = nullptr;
    volatile int* g_fpsDeepForceCameraAPtr = nullptr;
    volatile int* g_fpsDeepForceCameraBPtr = nullptr;

    uint8_t* g_fpsDeepInputMaskPtr = nullptr;
    uint64_t g_fpsDeepBit0Set = 0, g_fpsDeepBit1Set = 0, g_fpsDeepBits01Both = 0;
    uint64_t g_fpsDeepWinLButton = 0, g_fpsDeepWinRButton = 0, g_fpsDeepWinMButton = 0;
    uint64_t g_fpsDeepInputMaskReads = 0, g_fpsDeepInputMaskChanges = 0, g_fpsDeepInputMaskFaults = 0;
    uint8_t g_fpsDeepLastInputMask[10] = {};
    bool g_fpsDeepHaveInputMask = false;
    uint64_t g_fpsDeepMouseMotionCalls = 0, g_fpsDeepMouseMotionNonZero = 0;
    float g_fpsDeepMouseMotionAbsSum = 0.0f;

    uint64_t g_fpsDeepLightCalls = 0, g_fpsDeepLightTrue = 0;
    uint64_t g_fpsDeepInputCalls = 0, g_fpsDeepInputTrue = 0;
    uint64_t g_fpsDeepSpecialCalls = 0, g_fpsDeepSpecialTrue = 0;
    uint64_t g_fpsDeepLightSizeMismatch = 0, g_fpsDeepLightValueMismatch = 0;
    uint64_t g_fpsDeepLightUnchangedDirty = 0, g_fpsDeepLightChangedDirty = 0;
    uint64_t g_fpsDeepLightHashFaults = 0;
    uint64_t g_fpsDeepPrevLightA = 0, g_fpsDeepPrevLightB = 0;
    uint64_t g_fpsDeepPrevLightCountA = 0, g_fpsDeepPrevLightCountB = 0;
    bool g_fpsDeepHavePrevLight = false;

    uint64_t g_fpsDeepUpdateCalls = 0, g_fpsDeepCameraRequests = 0, g_fpsDeep2DRequests = 0;
    uint64_t g_fpsDeepForcePairSamples = 0, g_fpsDeepTickSamples = 0;
    uint64_t g_fpsDeepJqProfActiveSamples = 0, g_fpsDeepJqProfTriggerSamples = 0;

    struct FpsDeepObjectMetric
    {
        uint64_t calls = 0;
        uint64_t eligibleAfter = 0;
        uint64_t directCustom = 0;
        uint64_t resultTrue = 0;
        void Reset() { calls = eligibleAfter = directCustom = resultTrue = 0; }
    };
    FpsDeepObjectMetric g_fpsDeepObjects[3][4] = {}; // NONE/CAMERA/XY x mode0/mode1/mode2/other
    uint64_t g_fpsDeepObjectFaults = 0;

    struct FpsDeepVecSig
    {
        uint64_t hash = 1469598103934665603ull;
        uint64_t count = 0;
        bool valid = false;
    };

    FpsDeepVecSig FpsDeepVectorSignature(void* cam, size_t offset)
    {
        FpsDeepVecSig s;
        if (!cam) return s;
        __try
        {
            const uint8_t* v = static_cast<const uint8_t*>(cam) + offset;
            const uint8_t* begin = *reinterpret_cast<const uint8_t* const*>(v + 0);
            const uint8_t* end   = *reinterpret_cast<const uint8_t* const*>(v + 8);
            if (!begin && !end) { s.valid = true; return s; }
            if (!begin || !end || end < begin) return s;
            const uint64_t bytes = uint64_t(end - begin);
            if ((bytes & 15u) != 0 || bytes > 4ull * 1024ull * 1024ull) return s;
            s.count = bytes / 16u;

            uint64_t h = 1469598103934665603ull;
            for (uint64_t i = 0; i < bytes; ++i)
            {
                h ^= begin[i];
                h *= 1099511628211ull;
            }
            s.hash = h;
            s.valid = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            s.valid = false;
        }
        return s;
    }

    bool __fastcall hk_FpsDeepLightDirty(void* cam)
    {
        const bool r = g_oFpsDeepLightDirty(cam);
        if (!g_fpsDeepCapture || _ReturnAddress() != g_fpsDeepLightReturn) return r;

        ++g_fpsDeepLightCalls;
        if (r) ++g_fpsDeepLightTrue;

        const FpsDeepVecSig a = FpsDeepVectorSignature(cam, 6336);
        const FpsDeepVecSig b = FpsDeepVectorSignature(cam, 1304);
        if (!a.valid || !b.valid)
        {
            ++g_fpsDeepLightHashFaults;
            return r;
        }

        if (r)
        {
            if (a.count != b.count) ++g_fpsDeepLightSizeMismatch;
            else ++g_fpsDeepLightValueMismatch;

            const bool unchangedPair = g_fpsDeepHavePrevLight &&
                a.count == g_fpsDeepPrevLightCountA && b.count == g_fpsDeepPrevLightCountB &&
                a.hash == g_fpsDeepPrevLightA && b.hash == g_fpsDeepPrevLightB;
            if (unchangedPair) ++g_fpsDeepLightUnchangedDirty;
            else ++g_fpsDeepLightChangedDirty;
        }

        g_fpsDeepPrevLightA = a.hash;
        g_fpsDeepPrevLightB = b.hash;
        g_fpsDeepPrevLightCountA = a.count;
        g_fpsDeepPrevLightCountB = b.count;
        g_fpsDeepHavePrevLight = true;
        return r;
    }

    bool __fastcall hk_FpsDeepInputBits(int first, int second)
    {
        const bool r = g_oFpsDeepInputBits(first, second);
        if (g_fpsDeepCapture && _ReturnAddress() == g_fpsDeepInputReturn)
        {
            ++g_fpsDeepInputCalls;
            if (r) ++g_fpsDeepInputTrue;

            if (g_fpsDeepInputMaskPtr)
            {
                __try
                {
                    uint8_t now[10];
                    memcpy(now, g_fpsDeepInputMaskPtr, sizeof(now));
                    ++g_fpsDeepInputMaskReads;

                    if (now[0] & 0x01) ++g_fpsDeepBit0Set;
                    if (now[0] & 0x02) ++g_fpsDeepBit1Set;
                    if ((now[0] & 0x03) == 0x03) ++g_fpsDeepBits01Both;

                    if (g_fpsDeepHaveInputMask && memcmp(now, g_fpsDeepLastInputMask, sizeof(now)) != 0)
                        ++g_fpsDeepInputMaskChanges;
                    memcpy(g_fpsDeepLastInputMask, now, sizeof(now));
                    g_fpsDeepHaveInputMask = true;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    ++g_fpsDeepInputMaskFaults;
                }
            }

            if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ++g_fpsDeepWinLButton;
            if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ++g_fpsDeepWinRButton;
            if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) ++g_fpsDeepWinMButton;
        }
        return r;
    }

    bool __fastcall hk_FpsDeepSpecialState(void* cam)
    {
        const bool r = g_oFpsDeepSpecialState(cam);
        if (g_fpsDeepCapture && _ReturnAddress() == g_fpsDeepSpecialReturn)
        {
            ++g_fpsDeepSpecialCalls;
            if (r) ++g_fpsDeepSpecialTrue;
        }
        return r;
    }

    char __fastcall hk_FpsDeepPerObject(unsigned int mode, __int64 brush, __int64 a3, __int64 a4, __int64 drawState, char a6)
    {
        // objects inside prefab instances and the entered prefab's parents come here without the XY brush callback
        if (br::xylod::Skip(brush, reinterpret_cast<const float*>(a3))) return 0;
        const char r = g_oFpsDeepPerObject(mode, brush, a3, a4, drawState, a6);
        if (!g_fpsDeepCapture) return r;

        int view = g_viewContext == VIEW_CAMERA ? 1 : g_viewContext == VIEW_XY ? 2 : 0;
        const int m = mode <= 2 ? int(mode) : 3;
        FpsDeepObjectMetric& metric = g_fpsDeepObjects[view][m];
        ++metric.calls;
        if (r) ++metric.resultTrue;

        __try
        {
            if (brush)
            {
                if (*reinterpret_cast<const uint8_t*>(brush + 40)) ++metric.eligibleAfter;
                if (*reinterpret_cast<const uint64_t*>(brush + 56)) ++metric.directCustom;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ++g_fpsDeepObjectFaults;
        }
        return r;
    }

    void FpsDeepResetData(bool start)
    {
        g_fpsDeepCapture = false;
        g_fpsDeepStartMs = g_fpsDeepStopMs = 0;

        g_fpsDeepLightCalls = g_fpsDeepLightTrue = 0;
        g_fpsDeepInputCalls = g_fpsDeepInputTrue = 0;
        g_fpsDeepSpecialCalls = g_fpsDeepSpecialTrue = 0;
        g_fpsDeepLightSizeMismatch = g_fpsDeepLightValueMismatch = 0;
        g_fpsDeepLightUnchangedDirty = g_fpsDeepLightChangedDirty = 0;
        g_fpsDeepLightHashFaults = 0;
        g_fpsDeepPrevLightA = g_fpsDeepPrevLightB = 0;
        g_fpsDeepPrevLightCountA = g_fpsDeepPrevLightCountB = 0;
        g_fpsDeepHavePrevLight = false;

        g_fpsDeepUpdateCalls = g_fpsDeepCameraRequests = g_fpsDeep2DRequests = 0;
        g_fpsDeepForcePairSamples = g_fpsDeepTickSamples = 0;
        g_fpsDeepJqProfActiveSamples = g_fpsDeepJqProfTriggerSamples = 0;
        g_fpsDeepObjectFaults = 0;
        g_fpsDeepQueueFaults = 0;

        g_fpsDeepBit0Set = g_fpsDeepBit1Set = g_fpsDeepBits01Both = 0;
        g_fpsDeepWinLButton = g_fpsDeepWinRButton = g_fpsDeepWinMButton = 0;
        g_fpsDeepInputMaskReads = g_fpsDeepInputMaskChanges = g_fpsDeepInputMaskFaults = 0;
        memset(g_fpsDeepLastInputMask, 0, sizeof(g_fpsDeepLastInputMask));
        g_fpsDeepHaveInputMask = false;
        g_fpsDeepMouseMotionCalls = g_fpsDeepMouseMotionNonZero = 0;
        g_fpsDeepMouseMotionAbsSum = 0.0f;

        for (auto& views : g_fpsDeepObjects)
            for (auto& m : views) m.Reset();
        for (auto& passes : g_fpsDeepQueue)
            for (auto& m : passes) m.Reset();

        if (start)
        {
            g_fpsDeepStartMs = GetTickCount64();
            g_fpsDeepCapture = true;
        }
    }

    void FpsDeepStop()
    {
        if (g_fpsDeepCapture)
        {
            g_fpsDeepStopMs = GetTickCount64();
            g_fpsDeepCapture = false;
        }
    }

    double FpsDeepSeconds()
    {
        if (!g_fpsDeepStartMs) return 0.0;
        const uint64_t end = g_fpsDeepCapture ? GetTickCount64() : g_fpsDeepStopMs;
        return end >= g_fpsDeepStartMs ? double(end - g_fpsDeepStartMs) / 1000.0 : 0.0;
    }

    std::string FpsDeepReport()
    {
        std::string out;
        char b[1024];
        const double sec = FpsDeepSeconds();
        snprintf(b, sizeof b,
                 "Radiant+ FPS Deep V7.15 Report\nState: %s  Window: %.3f s\n"
                 "FULL FIDELITY: diagnostics only; no viewport/pass/lighting suppression\n\n",
                 g_fpsDeepCapture ? "RUNNING" : "STOPPED", sec);
        out += b;

        const double lightPct = g_fpsDeepLightCalls ? 100.0 * double(g_fpsDeepLightTrue) / double(g_fpsDeepLightCalls) : 0.0;
        const double unchangedPct = g_fpsDeepLightTrue ? 100.0 * double(g_fpsDeepLightUnchangedDirty) / double(g_fpsDeepLightTrue) : 0.0;
        snprintf(b, sizeof b,
                 "Camera redraw causes (RoutineProcessing callsite only)\n"
                 "  light-state compare: calls=%llu true/dirty=%llu (%.1f%%)\n"
                 "    dirty reason: size_mismatch=%llu value_mismatch=%llu changed_pair=%llu unchanged_pair=%llu (%.1f%% of dirty) hash_faults=%llu\n"
                 "  input bits:          calls=%llu true=%llu\n"
                 "  special-state gate:  calls=%llu true/block=%llu\n"
                 "  UpdateWindows: calls=%llu camera_requests=%llu 2D_requests=%llu\n"
                 "  force-camera pair: %llu/%llu tick samples\n\n",
                 (unsigned long long)g_fpsDeepLightCalls, (unsigned long long)g_fpsDeepLightTrue, lightPct,
                 (unsigned long long)g_fpsDeepLightSizeMismatch, (unsigned long long)g_fpsDeepLightValueMismatch,
                 (unsigned long long)g_fpsDeepLightChangedDirty, (unsigned long long)g_fpsDeepLightUnchangedDirty,
                 unchangedPct, (unsigned long long)g_fpsDeepLightHashFaults,
                 (unsigned long long)g_fpsDeepInputCalls, (unsigned long long)g_fpsDeepInputTrue,
                 (unsigned long long)g_fpsDeepSpecialCalls, (unsigned long long)g_fpsDeepSpecialTrue,
                 (unsigned long long)g_fpsDeepUpdateCalls, (unsigned long long)g_fpsDeepCameraRequests,
                 (unsigned long long)g_fpsDeep2DRequests,
                 (unsigned long long)g_fpsDeepForcePairSamples, (unsigned long long)g_fpsDeepTickSamples);
        out += b;

        if (g_fpsDeepLightTrue >= 20 && g_fpsDeepLightUnchangedDirty * 100 >= g_fpsDeepLightTrue * 80)
            out += "  >>> STRONG REDRAW BUG CANDIDATE: the light-state vectors remain unchanged while the compare keeps marking the camera dirty.\n\n";
        else if (g_fpsDeepLightTrue >= 20)
            out += "  Note: light-state dirty events are occurring, but they are not predominantly an unchanged-pair mismatch.\n\n";

        if (g_fpsDeepInputMaskReads)
        {
            char maskHex[64] = {};
            char* w = maskHex;
            size_t left = sizeof(maskHex);
            for (int i = 0; i < 10 && left > 3; ++i)
            {
                const int n = snprintf(w, left, "%02X%s", unsigned(g_fpsDeepLastInputMask[i]), i == 9 ? "" : " ");
                if (n <= 0 || size_t(n) >= left) break;
                w += n; left -= size_t(n);
            }

            snprintf(b, sizeof b,
                     "Input-state mapper for the 0/1 redraw gate\n"
                     "  internal mask reads=%llu changes=%llu faults=%llu last=[%s]\n"
                     "  bit0_set=%llu bit1_set=%llu both_set=%llu / %llu redraw-gate calls\n"
                     "  Win32 physical buttons: L=%llu R=%llu M=%llu / %llu samples\n"
                     "  camera mouse-motion routine: calls=%llu nonzero=%llu abs_motion_sum=%.3f\n",
                     (unsigned long long)g_fpsDeepInputMaskReads,
                     (unsigned long long)g_fpsDeepInputMaskChanges,
                     (unsigned long long)g_fpsDeepInputMaskFaults,
                     maskHex,
                     (unsigned long long)g_fpsDeepBit0Set,
                     (unsigned long long)g_fpsDeepBit1Set,
                     (unsigned long long)g_fpsDeepBits01Both,
                     (unsigned long long)g_fpsDeepInputCalls,
                     (unsigned long long)g_fpsDeepWinLButton,
                     (unsigned long long)g_fpsDeepWinRButton,
                     (unsigned long long)g_fpsDeepWinMButton,
                     (unsigned long long)g_fpsDeepInputCalls,
                     (unsigned long long)g_fpsDeepMouseMotionCalls,
                     (unsigned long long)g_fpsDeepMouseMotionNonZero,
                     double(g_fpsDeepMouseMotionAbsSum));
            out += b;

            if (g_fpsDeepInputCalls >= 20 &&
                g_fpsDeepInputTrue == g_fpsDeepInputCalls &&
                g_fpsDeepWinLButton == 0 && g_fpsDeepWinRButton == 0 && g_fpsDeepWinMButton == 0 &&
                g_fpsDeepMouseMotionNonZero == 0 && g_fpsDeepInputMaskChanges <= 1)
            {
                out += "  >>> STALE/ALWAYS-ON 0/1 GATE CANDIDATE: internal bits keep the camera redraw gate true with no physical mouse button, no camera motion, and a stable input mask.\\n";
                out += "      Do not patch it yet: V7.3 maps the exact bit so we can distinguish an intentional always-on editor state from a stale input flag.\\n";
            }
            out += "\\n";
        }

        static const char* viewNames[3] = { "none/other", "camera", "XY/XZ/YZ" };
        out += "Per-object brush/entity dispatcher (0x1401DC540)\n";
        for (int v = 0; v < 3; ++v)
        {
            uint64_t total = 0;
            for (int m = 0; m < 4; ++m) total += g_fpsDeepObjects[v][m].calls;
            if (!total) continue;
            snprintf(b, sizeof b, "  %s total=%llu\n", viewNames[v], (unsigned long long)total);
            out += b;
            for (int m = 0; m < 4; ++m)
            {
                const FpsDeepObjectMetric& x = g_fpsDeepObjects[v][m];
                if (!x.calls) continue;
                const char* mn = m == 0 ? "mode0" : m == 1 ? "mode1" : m == 2 ? "mode2" : "other";
                snprintf(b, sizeof b,
                         "    %-6s calls=%llu eligible_after=%llu (%.1f%%) direct/custom=%llu result_true=%llu\n",
                         mn, (unsigned long long)x.calls, (unsigned long long)x.eligibleAfter,
                         100.0 * double(x.eligibleAfter) / double(x.calls),
                         (unsigned long long)x.directCustom, (unsigned long long)x.resultTrue);
                out += b;
            }
        }
        snprintf(b, sizeof b, "  object_read_faults=%llu\n\n", (unsigned long long)g_fpsDeepObjectFaults);
        out += b;

        out += "Renderer stage queues (208-byte records observed before dispatch)\n";
        uint64_t allDispatch = 0, allGroups = 0, allRecords = 0;
        for (int p = 0; p < 10; ++p)
        {
            for (int c = 0; c < 4; ++c)
            {
                const FpsDeepQueueMetric& q = g_fpsDeepQueue[p][c];
                if (!q.dispatches) continue;
                allDispatch += q.dispatches; allGroups += q.groups; allRecords += q.records;
                snprintf(b, sizeof b,
                         "  pass=%d ctx=%d dispatch=%llu groups=%llu nonempty=%llu records=%llu avg_records=%.1f max_groups=%llu max_records=%llu\n",
                         p, c, (unsigned long long)q.dispatches, (unsigned long long)q.groups,
                         (unsigned long long)q.nonEmptyGroups, (unsigned long long)q.records,
                         q.dispatches ? double(q.records) / double(q.dispatches) : 0.0,
                         (unsigned long long)q.maxGroups, (unsigned long long)q.maxRecords);
                out += b;
            }
        }
        snprintf(b, sizeof b,
                 "  TOTAL dispatch=%llu groups=%llu records=%llu queue_read_faults=%llu\n\n",
                 (unsigned long long)allDispatch, (unsigned long long)allGroups,
                 (unsigned long long)allRecords, (unsigned long long)g_fpsDeepQueueFaults);
        out += b;

        snprintf(b, sizeof b,
                 "Built-in JQPROF sampling\n"
                 "  active=%llu/%llu ticks trigger=%llu/%llu ticks\n",
                 (unsigned long long)g_fpsDeepJqProfActiveSamples, (unsigned long long)g_fpsDeepTickSamples,
                 (unsigned long long)g_fpsDeepJqProfTriggerSamples, (unsigned long long)g_fpsDeepTickSamples);
        out += b;
        if (g_fpsDeepJqProfActiveSamples)
            out += "  >>> WARNING: Radiant's internal JQPROF event recorder was active during the capture and can add substantial per-call overhead.\n";

        return out;
    }

    void SetupFpsDeepV72()
    {
        g_fpsDeepQueueBase = static_cast<uint8_t*>(rp::host->RadiantAddress(rp::self, kFpsDeepQueueBase));
        g_fpsDeepLightReturn = rp::host->RadiantAddress(rp::self, kFpsDeepLightReturn);
        g_fpsDeepInputReturn = rp::host->RadiantAddress(rp::self, kFpsDeepInputReturn);
        g_fpsDeepSpecialReturn = rp::host->RadiantAddress(rp::self, kFpsDeepSpecialReturn);
        g_fpsDeepJqProfActivePtr = static_cast<uintptr_t*>(rp::host->RadiantAddress(rp::self, kFpsDeepJqProfActive));
        g_fpsDeepJqProfTriggerPtr = static_cast<uint8_t*>(rp::host->RadiantAddress(rp::self, kFpsDeepJqProfTrigger));
        g_fpsDeepForceCameraAPtr = static_cast<volatile int*>(rp::host->RadiantAddress(rp::self, kFpsDeepForceCameraA));
        g_fpsDeepForceCameraBPtr = static_cast<volatile int*>(rp::host->RadiantAddress(rp::self, kFpsDeepForceCameraB));
        g_fpsDeepInputMaskPtr = static_cast<uint8_t*>(rp::host->RadiantAddress(rp::self, kFpsDeepInputMask));

        const bool ld = DeepInstall("fpsdeep light dirty thunk", kFpsDeepLightDirtyThunk, &hk_FpsDeepLightDirty, &g_oFpsDeepLightDirty);
        const bool ib = DeepInstall("fpsdeep input bits thunk", kFpsDeepInputBitsThunk, &hk_FpsDeepInputBits, &g_oFpsDeepInputBits);
        const bool ss = DeepInstall("fpsdeep special state thunk", kFpsDeepSpecialStateThunk, &hk_FpsDeepSpecialState, &g_oFpsDeepSpecialState);
        const bool po = DeepInstall("fpsdeep per-object thunk", kFpsDeepPerObjectThunk, &hk_FpsDeepPerObject, &g_oFpsDeepPerObject);

        FpsDeepResetData(false);
        rp::Log("fpsdeep-v7.15: hooks light=%d input=%d special=%d per_object=%d queue=%d jqprof=%d; instrumentation only/full fidelity",
                ld ? 1 : 0, ib ? 1 : 0, ss ? 1 : 0, po ? 1 : 0,
                g_fpsDeepQueueBase ? 1 : 0, g_fpsDeepJqProfActivePtr ? 1 : 0);
    }


    ObjectFn        g_oMainStatus = nullptr;
    UpdateWindowsFn g_oUpdateWindows = nullptr;
    ReloadAssetsFn  g_oReloadAssets = nullptr;
    MouseMotionFn   g_oMouseMotion = nullptr;
    ObjectFn        g_oCamOnPaint = nullptr;
    BrushCacheFn    g_oBrushCache = nullptr;
    CamBrushesFn    g_oCamProcessBrushes = nullptr;
    ObjectFn        g_oQtXYOnRender = nullptr;
    XYBrushesFn     g_oXYProcessBrushes = nullptr;
    BeginFrameFn    g_oBeginFrame = nullptr;
    FrameFn         g_oEndFrame = nullptr;
    FrameFn         g_oPresent = nullptr;

    float MaxFps();

    uint64_t ProfileNow()
    {
        LARGE_INTEGER value;
        QueryPerformanceCounter(&value);
        return static_cast<uint64_t>(value.QuadPart);
    }

    uint64_t ProfileBegin() { return g_profile.active ? ProfileNow() : 0; }

    void ProfileAdd(int metric, uint64_t elapsed)
    {
        ModToolsPerfCounter& counter = g_profile.metrics[metric];
        if (!counter.calls || elapsed < counter.minTicks) counter.minTicks = elapsed;
        if (elapsed > counter.maxTicks) counter.maxTicks = elapsed;
        ++counter.calls;
        counter.totalTicks += elapsed;
    }

    uint64_t ProfileFinish(int metric, uint64_t begin)
    {
        if (!begin || !g_profile.active) return 0;
        uint64_t elapsed = ProfileNow() - begin;
        ProfileAdd(metric, elapsed);
        return elapsed;
    }

    bool HostProfileAvailable()
    {
        return rp::Has(&rp::host->PerformanceProfile) && rp::host->PerformanceProfile;
    }

    void ClearLocalProfile(bool start)
    {
        g_profile = {};
        if (start)
        {
            g_profile.startTick = ProfileNow();
            g_profile.active = true;
            g_profileDeadlineMs = GetTickCount64() + static_cast<uint64_t>(g_profileWindowSeconds * 1000.0);
        }
        else
            g_profileDeadlineMs = 0;
    }

    void StartProfile()
    {
        void* timer = g_timerPtr ? *g_timerPtr : nullptr;
        g_captureUncap = g_uncap;
        g_captureTickSetting = g_tickMs;
        g_captureTimerInterval = timer && g_interval ? g_interval(timer) : -1;
        g_captureMaxFps = MaxFps();
        g_captureUpdateXY = rp::Has(&rp::host->CamXYUpdateOnDrag) && rp::host->CamXYUpdateOnDrag
            ? rp::host->CamXYUpdateOnDrag(rp::self) : -1;
        if (HostProfileAvailable()) rp::host->PerformanceProfile(rp::self, MODTOOLS_HOST_PERF_START, nullptr);
        ClearLocalProfile(true);
    }

    void StopProfile()
    {
        if (!g_profile.active) return;
        g_profile.endTick = ProfileNow();
        g_profile.active = false;
        g_profileDeadlineMs = 0;
        if (HostProfileAvailable()) rp::host->PerformanceProfile(rp::self, MODTOOLS_HOST_PERF_STOP, nullptr);
    }

    float MaxFps() { return g_maxFps ? *g_maxFps : 0.0f; }

    // Interval that lets Radiant's limiter pace the frames, about two ticks per frame. At Max FPS 120 an 8 ms tick is jittery (113 to 125), 4 ms gives exactly 120.
    int DesiredTickMs()
    {
        if (!g_uncap) return kRadiantTickMs;
        if (g_tickMs > 0) return g_tickMs;
        float fps = MaxFps();
        if (fps <= 0.0f) return 1;                       // Max FPS 0 = Radiant draws every tick
        int ms = int(500.0f / fps);
        // pacing waits out the last tick before a frame, so that tick stays short
        const int most = g_pace ? kPaceTickMs : kRadiantTickMs;
        return ms < 1 ? 1 : ms > most ? most : ms;
    }

    double PaceNow()
    {
        static LARGE_INTEGER freq = {};
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        return double(t.QuadPart) / double(freq.QuadPart);
    }

    // Sleeps to the given time, the last part on the processor
    void PaceWaitUntil(double due)
    {
        if (!g_paceTimer) g_paceTimer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* high resolution */, TIMER_ALL_ACCESS);
        for (;;)
        {
            const double left = due - PaceNow();
            if (left <= 0) return;
            if (left > 0.001 && g_paceTimer)
            {
                LARGE_INTEGER li; li.QuadPart = -LONGLONG((left - 0.0006) * 1e7);
                if (SetWaitableTimer(g_paceTimer, &li, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(g_paceTimer, 50);
            }
            else if (left > 0.0025) Sleep(1);
            else YieldProcessor();
        }
    }

    bool PaceOn() { return g_uncap && g_pace && g_redrawFlags && MaxFps() >= 1.0f; }

    // True = not time for a frame yet, the request went back into the redraw flags for a later tick.
    bool PaceHoldFrame(int flags)
    {
        if (!PaceOn()) return false;
        double now = PaceNow();
        const double interval = 1.0 / MaxFps();
        if (g_paceNext > now + interval) g_paceNext = now + interval;
        const double left = g_paceNext - now;
        if (left >= DesiredTickMs() * 0.00125)
        {
            *g_redrawFlags |= flags;
            ++g_paceHeld;
            return true;
        }
        // the last tick before the slot waits for it
        if (left > 0) { PaceWaitUntil(g_paceNext); now = PaceNow(); }
        // from this frame's start, so a late frame is never made up for by a short one
        g_paceNext = now + interval;
        return false;
    }

    void Apply(bool force)
    {
        void* timer = g_timerPtr ? *g_timerPtr : nullptr;
        if (!timer) return;                              // before Radiant's main loop starts
        int want = DesiredTickMs();
        int have = g_interval(timer);
        if (have == want && !force) return;
        uint64_t now = GetTickCount64();
        if (g_applied && have == kRadiantTickMs && g_lastSet != kRadiantTickMs)
        {
            ++g_resets;
            if (now - g_lastResetLog > 60000) { g_lastResetLog = now; rp::Log("fps: Radiant restarted its idle timer at %d ms (%d times so far), set back to %d ms", have, g_resets, want); }
        }
        else if (want != g_lastSet)
            rp::Log("fps: idle tick %d ms -> %d ms (Max FPS preference %.0f)", have, want, MaxFps());
        g_setInterval(timer, want);
        g_lastSet = want;
        g_applied = true;
    }

    void LoadConfig()
    {
        g_uncap = rp::ConfigGetBool("fps", "uncap", true);
        g_tickMs = rp::ConfigGetInt("fps", "tick_ms", 0);
        if (g_tickMs < 0) g_tickMs = 0;
        if (g_tickMs > 1000) g_tickMs = 1000;
        g_pace = rp::ConfigGetBool("fps", "pace", true);
        g_boost = rp::ConfigGetBool("bake", "boost", true);
        g_cancelGuard = rp::ConfigGetBool("bake", "cancel_guard", true);
        g_boostBudgetMs = rp::ConfigGetInt("bake", "budget_ms", 8);
        if (g_boostBudgetMs < 0) g_boostBudgetMs = 0;
        if (g_boostBudgetMs > 200) g_boostBudgetMs = 200;
        g_boostRedrawMs = rp::ConfigGetInt("bake", "redraw_ms", 500);
        if (g_boostRedrawMs < 0) g_boostRedrawMs = 0;
        if (g_boostRedrawMs > 10000) g_boostRedrawMs = 10000;
        g_gpuStatsCache = rp::ConfigGetBool("performance", "gpu_stats_cache", true);
        g_gpuStatsMs = rp::ConfigGetInt("performance", "gpu_stats_ms", 1000);
        if (g_gpuStatsMs < 1000) g_gpuStatsMs = 1000;
        if (g_gpuStatsMs > 10000) g_gpuStatsMs = 10000;
        g_xyDragHz = rp::ConfigGetInt("performance", "xy_drag_hz", 10);
        if (g_xyDragHz < 0) g_xyDragHz = 0;
        if (g_xyDragHz > 120) g_xyDragHz = 120;
        g_xyDragShare = rp::ConfigGetInt("performance", "xy_drag_share", 20);
        if (g_xyDragShare < 0) g_xyDragShare = 0;
        if (g_xyDragShare > 100) g_xyDragShare = 100;
        g_xyFocusHz = rp::ConfigGetInt("performance", "xy_focus_hz", 60);
        if (g_xyFocusHz < 0) g_xyFocusHz = 0;
        if (g_xyFocusHz > 240) g_xyFocusHz = 240;
        // The viewport cull is opt-in.
        g_gridCullEnabled = rp::ConfigGetBool("performance", "grid_cull", false);
        g_gridCullPadding = static_cast<float>(rp::ConfigGetInt("performance", "grid_cull_padding", 1024));
        if (!std::isfinite(g_gridCullPadding) || g_gridCullPadding < 64.0f) g_gridCullPadding = 64.0f;
        if (g_gridCullPadding > 16384.0f) g_gridCullPadding = 16384.0f;
        g_perfV3Enabled = rp::ConfigGetBool("performance", "interactive_fast_mode", true);
        g_perfV3SettleMs = rp::ConfigGetInt("performance", "interactive_settle_ms", 150);
        if (g_perfV3SettleMs < 0) g_perfV3SettleMs = 0;
        if (g_perfV3SettleMs > 2000) g_perfV3SettleMs = 2000;
        // Fast paths that suppress visible or editor work are forced off at startup (diagnostic commands remain).
        g_perfV3SkipMode2 = false;
        g_perfV3SuppressXY = false;
        rp::ConfigSet("performance", "interactive_skip_mode2", "0");
        rp::ConfigSet("performance", "interactive_suppress_xy_drag", "0");
        g_perfV7Enabled = rp::ConfigGetBool("performance", "render7_enabled", true);

        // Same for the render7 toggles, overwriting values that may be persisted in the user's config.
        g_perfV7StaticMode2 = false;
        g_perfV7DeferXYDuringCamera = false;
        g_perfV7IdleReuse = false;
        rp::ConfigSet("performance", "render7_static_mode2", "0");
        rp::ConfigSet("performance", "render7_defer_xy_camera", "0");
        rp::ConfigSet("performance", "render7_idle_reuse", "0");
        g_perfV7IdleAfterMs = rp::ConfigGetInt("performance", "render7_idle_after_ms", 175);
        if (g_perfV7IdleAfterMs < 50) g_perfV7IdleAfterMs = 50;
        if (g_perfV7IdleAfterMs > 5000) g_perfV7IdleAfterMs = 5000;
        g_perfV7IdleRefreshMs = rp::ConfigGetInt("performance", "render7_idle_refresh_ms", 250);
        if (g_perfV7IdleRefreshMs < 50) g_perfV7IdleRefreshMs = 50;
        if (g_perfV7IdleRefreshMs > 5000) g_perfV7IdleRefreshMs = 5000;
        g_v6SuppressLoadRendering = rp::ConfigGetBool("performance", "map_suppress_rendering", false);
        g_v6SkipLoadTimestamps = rp::ConfigGetBool("performance", "map_skip_asset_timestamp", true);
        g_v6AssetRamEnabled = rp::ConfigGetBool("performance", "asset_ram_cache", false);
        g_v6AssetRamMB = rp::ConfigGetInt("performance", "asset_ram_cache_mb", 0);
        if (g_v6AssetRamMB < 0) g_v6AssetRamMB = 0;
        if (g_v6AssetRamMB > 8192) g_v6AssetRamMB = 8192;
        g_v6AssetRamMaxEntryMB = rp::ConfigGetInt("performance", "asset_ram_max_entry_mb", 4);
        if (g_v6AssetRamMaxEntryMB < 1) g_v6AssetRamMaxEntryMB = 1;
        if (g_v6AssetRamMaxEntryMB > 512) g_v6AssetRamMaxEntryMB = 512;
        g_v6GeoPreallocate = rp::ConfigGetBool("performance", "geo_preallocate", false);
        // Speculative asset prefetch is retired (it did not beat the baseline).
        g_v63PrefetchEnabled = false;
        rp::ConfigSet("performance", "asset_prefetch", "0");
        g_v63PrefetchWorkers = rp::ConfigGetInt("performance", "asset_prefetch_workers", 1);
        if (g_v63PrefetchWorkers < 0) g_v63PrefetchWorkers = 0;
        if (g_v63PrefetchWorkers > 8) g_v63PrefetchWorkers = 8;
        g_v63PrefetchMB = rp::ConfigGetInt("performance", "asset_prefetch_mb", 64);
        if (g_v63PrefetchMB < 64) g_v63PrefetchMB = 64;
        if (g_v63PrefetchMB > 1024) g_v63PrefetchMB = 1024;
        g_v63PrefetchMaxEntryMB = rp::ConfigGetInt("performance", "asset_prefetch_max_entry_mb", 16);
        if (g_v63PrefetchMaxEntryMB < 1) g_v63PrefetchMaxEntryMB = 1;
        if (g_v63PrefetchMaxEntryMB > 256) g_v63PrefetchMaxEntryMB = 256;
        // Migrate old prefetch defaults persisted during testing.
        if (g_v63PrefetchWorkers == 4) g_v63PrefetchWorkers = 1;
        if (g_v63PrefetchMB == 256) g_v63PrefetchMB = 64;
        if (g_v63PrefetchMaxEntryMB == 64) g_v63PrefetchMaxEntryMB = 16;
        g_perfV4TurboLighting = false; // retired, real/baked lighting is always preserved
        g_perfV4TurboMode = rp::ConfigGetInt("performance", "interactive_turbo_mode", 1);
        if (!PerfV4TurboModeValid(g_perfV4TurboMode)) g_perfV4TurboMode = 1;
        g_profileWindowSeconds = atof(rp::ConfigGet("profile", "window_seconds", "10").c_str());
        if (g_profileWindowSeconds < 0.5) g_profileWindowSeconds = 0.5;
        if (g_profileWindowSeconds > 3600.0) g_profileWindowSeconds = 3600.0;
    }

    // ---- bake state access ----
    uint8_t* CamWnd()
    {
        if (!g_mainWinPtr || !*g_mainWinPtr) return nullptr;
        uint8_t* children = *reinterpret_cast<uint8_t**>(*g_mainWinPtr + off_MainWin_QEChildren);
        if (!children) return nullptr;
        return *reinterpret_cast<uint8_t**>(children + off_QEChildren_CamWnd);
    }
    int CamMode(uint8_t* cam) { return cam ? *reinterpret_cast<int*>(cam + off_Cam_Mode) : -1; }

    // Mode 4 is the cam window's normal lit mode (UpdateGI runs every tick, returns at once when idle). A bake runs when a volume's light state is 3..6.
    bool BakeActive(uint8_t* cam)
    {
        if (!cam || CamMode(cam) != 4) return false;
        uint8_t* vb = *reinterpret_cast<uint8_t**>(cam + off_Cam_VolumesBegin);
        uint8_t* ve = *reinterpret_cast<uint8_t**>(cam + off_Cam_VolumesEnd);
        if (!vb || !ve || ve < vb || ve - vb > 16 * 64) return false;
        for (uint8_t* v = vb; v < ve; v += 16)
        {
            uint8_t* vol = *reinterpret_cast<uint8_t**>(v);
            if (!vol) continue;
            const int* st = reinterpret_cast<const int*>(vol + off_Volume_State);
            for (int i = 0; i < 4; ++i) if (st[i] >= 3 && st[i] <= 6) return true;
        }
        return false;
    }
    bool BakeRunning() { return BakeActive(CamWnd()); }

    // A light state in 3..6 on any volume, whatever the view mode (BakeActive also requires the lit view).
    bool BakeStateActive(uint8_t* cam)
    {
        if (!cam) return false;
        uint8_t* vb = *reinterpret_cast<uint8_t**>(cam + off_Cam_VolumesBegin);
        uint8_t* ve = *reinterpret_cast<uint8_t**>(cam + off_Cam_VolumesEnd);
        if (!vb || !ve || ve < vb || ve - vb > 16 * 64) return false;
        for (uint8_t* v = vb; v < ve; v += 16)
            if (uint8_t* vol = *reinterpret_cast<uint8_t**>(v))
                for (int i = 0; i < 4; ++i) { const int st = reinterpret_cast<const int*>(vol + off_Volume_State)[i]; if (st >= 3 && st <= 6) return true; }
        return false;
    }
    int InflightJobs() { return g_inflight ? *g_inflight : 0; }

    void Hold(const HeldAction& a)
    {
        if (g_held.empty()) g_heldSince = GetTickCount64();
        // Repeated clicks collapse: keep the latest build and one reload, in the order first asked.
        auto same = std::find_if(g_held.begin(), g_held.end(), [&](const HeldAction& h) { return h.reload == a.reload; });
        if (same != g_held.end()) *same = a; else g_held.push_back(a);
        ++g_heldTotal;
        rp::Log("bake: cancel guard holds %s until %d cancelled probe job(s) finish", a.reload ? "the lighting reload" : "the build", InflightJobs());
    }

    void __fastcall hk_ToolbarBuild(void* mainWin, char sun, char probes)
    {
        // With a bake in progress the click means pause/resume/cancel: Radiant's, at once.
        const int inflight = InflightJobs();
        const bool active = BakeStateActive(CamWnd());
        rp::Log("bake: build requested (sun %d, probes %d): %d probe job(s) in flight, bake %s", sun ? 1 : 0, probes ? 1 : 0, inflight, active ? "active" : "idle");
        if (g_cancelGuard && (inflight > 0 || !g_held.empty()) && !active)
        { Hold({false, mainWin, sun, probes}); return; }
        g_oToolbarBuild(mainWin, sun, probes);
    }
    void __fastcall hk_ToolbarReload(void* mainWin)
    {
        if (g_cancelGuard && (InflightJobs() > 0 || !g_held.empty())) { Hold({true, mainWin, 0, 0}); return; }
        g_oToolbarReload(mainWin);
    }

    // From the tick: replay held actions once no job is left (or after 60 s if the counter never drops).
    void ReleaseHeld()
    {
        if (g_held.empty()) return;
        const bool timeout = GetTickCount64() - g_heldSince > 60000;
        if (InflightJobs() > 0 && !timeout) return;
        std::vector<HeldAction> run; run.swap(g_held);
        rp::Log("bake: cancel guard releases %zu held action(s) after %.1f s%s", run.size(), double(GetTickCount64() - g_heldSince) / 1000.0,
                timeout ? " (timeout, jobs still counted)" : "");
        for (const HeldAction& a : run)
            if (a.reload) g_oToolbarReload(a.mainWin); else g_oToolbarBuild(a.mainWin, a.sun, a.probes);
    }

    double NowMs()
    {
        LARGE_INTEGER c; QueryPerformanceCounter(&c);
        return g_qpf.QuadPart ? double(c.QuadPart) * 1000.0 / double(g_qpf.QuadPart) : double(GetTickCount64());
    }

    // CCamWnd::RoutineProcessing: Radiant steps the bake once here. With the boost on, keep stepping until no work or the budget is spent
    // (UpdateGI yields when every job is still running, so the budget bounds main-thread cost per tick).
    __int64 __fastcall hk_CamRoutine(void* cam)
    {
        uint64_t begin = ProfileBegin();
        __int64 r = g_oCamRoutine(cam);
        ProfileFinish(PM_CAM_ROUTINE, begin);
        if (!g_bakeAvail || !g_updateGI || !g_boost || g_boostBudgetMs <= 0 || !BakeActive(static_cast<uint8_t*>(cam))) return r;
        ++g_boostTicks;
        double end = NowMs() + g_boostBudgetMs;
        while (NowMs() < end)
        {
            __int64 remaining = g_updateGI(cam, 1);
            ++g_extraSteps;
            if (remaining == 0) break;
        }
        return r;
    }

    bool PerfV4TurboModeValid(int mode)
    {
        return mode >= 0 && mode <= 6 && mode != 4;
    }

    struct PerfV4ModeGuard
    {
        uint8_t* cam = nullptr;
        int savedMode = -1;
        int turboMode = -1;
        bool swapped = false;

        explicit PerfV4ModeGuard(void* p)
        {
            cam = static_cast<uint8_t*>(p);
            if (!g_perfV4TurboLighting || !g_perfV3Enabled || !g_perfV3Active || !cam || !PerfV4TurboModeValid(g_perfV4TurboMode))
                return;

            const int currentMode = CamMode(cam);
            if (currentMode != 4)
            {
                ++g_perfV4NonLitFrames;
                return;
            }

            if (BakeActive(cam))
            {
                ++g_perfV4BakeBlocks;
                return;
            }

            savedMode = currentMode;
            turboMode = g_perfV4TurboMode;
            *reinterpret_cast<int*>(cam + off_Cam_Mode) = turboMode;
            swapped = true;
            ++g_perfV4ModeSwaps;
        }

        ~PerfV4ModeGuard()
        {
            if (!swapped || !cam) return;
            // Do not stomp a deliberate mode change made by Radiant during the frame.
            if (CamMode(cam) == turboMode)
                *reinterpret_cast<int*>(cam + off_Cam_Mode) = savedMode;
            ++g_perfV4ModeRestores;
            ++g_perfV4TurboFrames;
        }
    };

    void PerfV4ResetCounters()
    {
        g_perfV4TurboFrames = 0;
        g_perfV4ModeSwaps = 0;
        g_perfV4ModeRestores = 0;
        g_perfV4BakeBlocks = 0;
        g_perfV4NonLitFrames = 0;
    }

    std::string PerfV4Status()
    {
        char out[768];
        uint8_t* cam = CamWnd();
        const int camMode = CamMode(cam);
        snprintf(out, sizeof out,
                 "turbo_lighting=%d turbo_mode=%d cam_mode=%d fast_active=%d force=%d turbo_frames=%llu swaps=%llu restores=%llu bake_blocks=%llu nonlit_frames=%llu",
                 g_perfV4TurboLighting ? 1 : 0, g_perfV4TurboMode, camMode,
                 g_perfV3Active ? 1 : 0, g_perfV3Force ? 1 : 0,
                 (unsigned long long)g_perfV4TurboFrames,
                 (unsigned long long)g_perfV4ModeSwaps,
                 (unsigned long long)g_perfV4ModeRestores,
                 (unsigned long long)g_perfV4BakeBlocks,
                 (unsigned long long)g_perfV4NonLitFrames);
        return out;
    }
    // CCamWnd::OnRender: the whole 3D view frame. While a bake runs draw at most one frame per redraw_ms.
    __int64 __fastcall hk_OnRender(void* cam, __int64 a2, __int64 a3, __int64 a4)
    {
        if (g_v6SuppressLoadRendering && V6MapLoading())
        {
            ++g_v6RenderSuppress3D;
            return 0;
        }
        PerfV3PollInteraction();
        if (g_perfV3Active) ++g_perfV3FastFrames;

        const bool bakingNow = g_bakeAvail && BakeActive(static_cast<uint8_t*>(cam));
        if (!bakingNow && PerfV7CanIdleReuse())
        {
            const uint64_t now = GetTickCount64();
            const bool refreshDue = !g_perfV7LastFull3D ||
                (now - g_perfV7LastFull3D >= uint64_t(g_perfV7IdleRefreshMs));
            if (!g_perfV7NeedFull3D && !refreshDue)
            {
                ++g_perfV7Idle3DReused;
                return 0;
            }
        }

        if (g_bakeAvail && g_boost && g_boostRedrawMs > 0 && bakingNow)
        {
            uint64_t now = GetTickCount64();
            if (now - g_lastRender < uint64_t(g_boostRedrawMs)) { ++g_skippedFrames; return 0; }
            g_lastRender = now;
        }
        ViewContext previous = g_viewContext;
        g_viewContext = VIEW_CAMERA;
        uint64_t begin = ProfileBegin();
        __int64 result = g_oOnRender(cam, a2, a3, a4);
        g_perfV7LastFull3D = GetTickCount64();
        g_perfV7NeedFull3D = false;
        ++g_perfV7Full3DFrames;
        ProfileFinish(PM_CAM_ON_RENDER, begin);
        g_viewContext = previous;
        return result;
    }

    __int64 __fastcall hk_MainStatus(void* mainWin)
    {
        uint64_t begin = ProfileBegin();
        __int64 result = g_oMainStatus(mainWin);
        ProfileFinish(PM_MAIN_STATUS, begin);
        return result;
    }

    __int64 __fastcall hk_UpdateWindows(void* mainWin, int flags)
    {
        if (_ReturnAddress() == g_tickUpdateReturn && PaceHoldFrame(flags)) return 0;
        flags = XyDragThrottle(flags);
        uint64_t begin = ProfileBegin();
        if (flags & 0x6) g_xyDragAccumReset = true;   // the 2D views draw later in Qt's paint event, see XyDragNote2DFrame
        __int64 result = g_oUpdateWindows(mainWin, flags);
        if (g_fpsDeepCapture)
        {
            ++g_fpsDeepUpdateCalls;
            if (flags & 0x101) ++g_fpsDeepCameraRequests;
            if (flags & 0x6) ++g_fpsDeep2DRequests;
        }
        if (g_profile.active)
        {
            if (flags & 0x101) ++g_profile.updateCameraRequests;
            if (flags & 0x6) ++g_profile.update2DRequests;
        }
        ProfileFinish(PM_UPDATE_WINDOWS, begin);
        return result;
    }

    __int64 __fastcall hk_ReloadAssets(void* cam, unsigned char force)
    {
        uint64_t begin = ProfileBegin();
        __int64 result = g_oReloadAssets(cam, force);
        ProfileFinish(PM_RELOAD_ASSETS, begin);
        return result;
    }

    __int64 __fastcall hk_MouseMotion(void* cam, float motion)
    {
        uint64_t begin = ProfileBegin();
        if (g_fpsDeepCapture)
        {
            ++g_fpsDeepMouseMotionCalls;
            if (motion != 0.0f)
            {
                ++g_fpsDeepMouseMotionNonZero;
                g_fpsDeepMouseMotionAbsSum += motion < 0.0f ? -motion : motion;
            }
        }
        __int64 result = g_oMouseMotion(cam, motion);
        ProfileFinish(PM_MOUSE_MOTION, begin);
        return result;
    }

    __int64 __fastcall hk_CamOnPaint(void* cam)
    {
        uint64_t begin = ProfileBegin();
        __int64 result = g_oCamOnPaint(cam);
        ProfileFinish(PM_CAM_ON_PAINT, begin);
        return result;
    }

    __int64 __fastcall hk_BrushCache()
    {
        uint64_t begin = ProfileBegin();
        br::filtercache::PassBegin();       // filter key tests are answered once per entity inside (filtercache.cpp)
        __int64 result = g_oBrushCache();
        br::filtercache::PassEnd();
        uint64_t elapsed = ProfileFinish(PM_BRUSH_CACHE, begin);
        if (begin && g_profile.active)
        {
            if (g_viewContext == VIEW_CAMERA) ProfileAdd(PM_BRUSH_CACHE_CAM, elapsed);
            else if (g_viewContext == VIEW_XY) ProfileAdd(PM_BRUSH_CACHE_XY, elapsed);
        }
        return result;
    }

    __int64 __fastcall hk_CamProcessBrushes(void* cam, int pass, int flags)
    {
        if (br::fxpreview::HidingMap()) return 0;      // FX Preview: the map stays out of the 3D view, the effect still draws
        uint64_t begin = ProfileBegin();
        __int64 result = g_oCamProcessBrushes(cam, pass, flags);
        uint64_t elapsed = ProfileFinish(PM_CAM_PROCESS_BRUSHES, begin);
        if (begin && g_profile.active)
        {
            int metric = pass == 0 ? PM_CAM_PROCESS_PASS0 : pass == 1 ? PM_CAM_PROCESS_PASS1 : pass == 2 ? PM_CAM_PROCESS_PASS2 : PM_CAM_PROCESS_OTHER;
            ProfileAdd(metric, elapsed);
        }
        return result;
    }

    __int64 __fastcall hk_QtXYOnRender(void* xy)
    {
        if (g_v6SuppressLoadRendering && V6MapLoading())
        {
            ++g_v6RenderSuppress2D;
            return 0;
        }

        // Never drop an XY/XZ/YZ render request (inferring camera interaction from held input made the grid choppy).

        if (PerfV7CanIdleReuse())
        {
            const uint64_t now = GetTickCount64();
            const bool refreshDue = !g_perfV7LastFull2D ||
                (now - g_perfV7LastFull2D >= uint64_t(g_perfV7IdleRefreshMs));
            if (!g_perfV7NeedFull2D && !refreshDue)
            {
                ++g_perfV7Idle2DReused;
                return 0;
            }
        }

        ViewContext previous = g_viewContext;
        g_viewContext = VIEW_XY;
        uint64_t begin = ProfileBegin();
        LARGE_INTEGER lodFreq, lodBegin, lodEnd;
        QueryPerformanceFrequency(&lodFreq);
        QueryPerformanceCounter(&lodBegin);
        br::xylod::RenderBegin();
        const uint64_t xyDragBegin = XyDragNowMs();
        __int64 result = g_oQtXYOnRender(xy);
        XyDragNote2DFrame(double(XyDragNowMs() - xyDragBegin));
        QueryPerformanceCounter(&lodEnd);
        br::xylod::RenderEnd(double(lodEnd.QuadPart - lodBegin.QuadPart) * 1000.0 / double(lodFreq.QuadPart));
        g_perfV7LastFull2D = GetTickCount64();
        g_perfV7NeedFull2D = false;
        ++g_perfV7Full2DFrames;
        ProfileFinish(PM_XY_ON_RENDER, begin);
        g_viewContext = previous;
        return result;
    }

    __int64 __fastcall hk_XYProcessBrushes(void* xy, int pass, __int64* data, __int64 flags)
    {
        GridCullView previous{};
        GridCullBegin(xy, previous);
        br::xylod::PassBegin(xy);
        uint64_t begin = ProfileBegin();
        __int64 result = g_oXYProcessBrushes(xy, pass, data, flags);
        br::xylod::PassEnd();
        uint64_t elapsed = ProfileFinish(PM_XY_PROCESS_BRUSHES, begin);
        if (begin && g_profile.active) ProfileAdd(pass == 0 ? PM_XY_PROCESS_PASS0 : pass == 1 ? PM_XY_PROCESS_PASS1 : PM_XY_PROCESS_OTHER, elapsed);
        GridCullEnd();
        g_gridCullView = previous;
        return result;
    }

    char __fastcall hk_BeginFrame()
    {
        g_v6RendererReady = true;
        if (g_censusCapture && g_viewContext == VIEW_CAMERA)
        {
            g_censusLastSubmitDraw = {};
            g_censusLastSubmitDrawValid = false;
        }
        uint64_t begin = ProfileBegin();
        char result = g_oBeginFrame();
        ProfileFinish(PM_BEGIN_FRAME, begin);
        return result;
    }

    __int64 __fastcall hk_EndFrame()
    {
        // If the DrawHud hook is not installed, use the last post-GeoSubmit snapshot.
        if (g_censusCapture && !g_oCensusDrawHud && g_viewContext == VIEW_CAMERA && g_censusLastSubmitDrawValid)
            CensusAddFrame(g_censusLastSubmitDraw, false);
        uint64_t begin = ProfileBegin();
        __int64 result = g_oEndFrame();
        ProfileFinish(PM_END_FRAME, begin);
        return result;
    }

    __int64 __fastcall hk_Present()
    {
        uint64_t begin = ProfileBegin();
        __int64 result = g_oPresent();
        uint64_t elapsed = ProfileFinish(PM_PRESENT, begin);
        if (begin && g_profile.active)
        {
            int metric = g_viewContext == VIEW_CAMERA ? PM_PRESENT_CAM : g_viewContext == VIEW_XY ? PM_PRESENT_XY : PM_PRESENT_OTHER;
            ProfileAdd(metric, elapsed);
        }
        return result;
    }

    template <typename Fn>
    bool InstallProfileHook(const char* name, uintptr_t idaAddress, Fn detour, Fn* original, int metric)
    {
        void* target = rp::host->RadiantAddress(rp::self, idaAddress);
        if (!target || !rp::host->Detour(rp::self, target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original)))
        {
            rp::Log("profile: detour unavailable for %s at 0x%llx", name, static_cast<unsigned long long>(idaAddress));
            return false;
        }
        g_metricAvailable[metric] = true;
        return true;
    }

    void SetupPerformanceHooks()
    {
        g_metricAvailable[PM_BETTER_RADIANT_TICK] = true;

        // These two hooks are shared with the bake feature, reuse them instead of stacking detours.
        if (g_oCamRoutine) g_metricAvailable[PM_CAM_ROUTINE] = true;
        else InstallProfileHook("CCamWnd::RoutineProcessing", kCamRoutineProcessing, &hk_CamRoutine, &g_oCamRoutine, PM_CAM_ROUTINE);
        if (g_oOnRender) g_metricAvailable[PM_CAM_ON_RENDER] = true;
        else InstallProfileHook("CCamWnd::OnRender", kCamOnRender, &hk_OnRender, &g_oOnRender, PM_CAM_ON_RENDER);

        InstallProfileHook("main-window status updater", kMainStatusUpdater, &hk_MainStatus, &g_oMainStatus, PM_MAIN_STATUS);
        InstallProfileHook("RadiantMainWin::UpdateWindows", kMainUpdateWindows, &hk_UpdateWindows, &g_oUpdateWindows, PM_UPDATE_WINDOWS);
        InstallProfileHook("CCamWnd::ReloadAssets", kCamReloadAssets, &hk_ReloadAssets, &g_oReloadAssets, PM_RELOAD_ASSETS);
        InstallProfileHook("camera mouse-motion routine", kCamMouseMotion, &hk_MouseMotion, &g_oMouseMotion, PM_MOUSE_MOTION);
        InstallProfileHook("CCamWnd::OnPaint", kCamOnPaint, &hk_CamOnPaint, &g_oCamOnPaint, PM_CAM_ON_PAINT);
        InstallProfileHook("BrushCache_Update", kBrushCacheUpdate, &hk_BrushCache, &g_oBrushCache, PM_BRUSH_CACHE);
        if (InstallProfileHook("CCamWnd::Cam_ProcessAllBrushes", kCamProcessAllBrushes, &hk_CamProcessBrushes, &g_oCamProcessBrushes, PM_CAM_PROCESS_BRUSHES))
            g_metricAvailable[PM_CAM_PROCESS_PASS0] = g_metricAvailable[PM_CAM_PROCESS_PASS1] = g_metricAvailable[PM_CAM_PROCESS_PASS2] = g_metricAvailable[PM_CAM_PROCESS_OTHER] = true;
        InstallProfileHook("QtXYRender::OnRender", kQtXYOnRender, &hk_QtXYOnRender, &g_oQtXYOnRender, PM_XY_ON_RENDER);
        if (InstallProfileHook("CXYWnd::XY_ProcessAllBrushes", kXYProcessAllBrushes, &hk_XYProcessBrushes, &g_oXYProcessBrushes, PM_XY_PROCESS_BRUSHES))
            g_metricAvailable[PM_XY_PROCESS_PASS0] = g_metricAvailable[PM_XY_PROCESS_PASS1] = g_metricAvailable[PM_XY_PROCESS_OTHER] = true;
        InstallProfileHook("Renderer BeginFrame", kRendererBeginFrame, &hk_BeginFrame, &g_oBeginFrame, PM_BEGIN_FRAME);
        InstallProfileHook("Renderer EndFrame", kRendererEndFrame, &hk_EndFrame, &g_oEndFrame, PM_END_FRAME);
        if (InstallProfileHook("DXGI Present", kDxgiPresent, &hk_Present, &g_oPresent, PM_PRESENT))
            g_metricAvailable[PM_PRESENT_CAM] = g_metricAvailable[PM_PRESENT_XY] = g_metricAvailable[PM_PRESENT_OTHER] = true;

        g_metricAvailable[PM_BRUSH_CACHE_CAM] = g_metricAvailable[PM_BRUSH_CACHE_XY] = g_metricAvailable[PM_BRUSH_CACHE];
        int installed = 0;
        for (bool available : g_metricAvailable) if (available) ++installed;
        rp::Log("profile: aggregate instrumentation ready (%d/%d metric rows; disabled until 'profile start')", installed, PM_COUNT);
    }

    void SetupBake()
    {
        g_mainWinPtr = static_cast<uint8_t**>(rp::host->RadiantAddress(rp::self, kMainWinPtr));
        g_inflight = static_cast<const volatile int*>(rp::host->RadiantAddress(rp::self, kInflightJobs));
        g_updateGI = reinterpret_cast<UpdateGIFn>(rp::host->RadiantAddress(rp::self, kUpdateGI));
        void* routine = rp::host->RadiantAddress(rp::self, kCamRoutineProcessing);
        void* onRender = rp::host->RadiantAddress(rp::self, kCamOnRender);
        QueryPerformanceFrequency(&g_qpf);
        if (!g_mainWinPtr || !g_inflight || !g_updateGI || !routine || !onRender) { rp::Log("bake: addresses missing, boost unavailable"); return; }
        if (!rp::host->Detour(rp::self, routine, reinterpret_cast<void*>(&hk_CamRoutine), reinterpret_cast<void**>(&g_oCamRoutine)))
        { rp::Log("bake: detour on CCamWnd::RoutineProcessing failed, boost unavailable"); return; }
        g_metricAvailable[PM_CAM_ROUTINE] = true;
        if (!rp::host->Detour(rp::self, onRender, reinterpret_cast<void*>(&hk_OnRender), reinterpret_cast<void**>(&g_oOnRender)))
        { rp::Log("bake: detour on CCamWnd::OnRender failed, redraw throttle unavailable"); }
        else g_metricAvailable[PM_CAM_ON_RENDER] = true;
        g_bakeAvail = true;
        void* build = rp::host->RadiantAddress(rp::self, kToolbarBuild);
        void* reload = rp::host->RadiantAddress(rp::self, kToolbarReload);
        const bool guard = build && reload &&
            rp::host->Detour(rp::self, build, reinterpret_cast<void*>(&hk_ToolbarBuild), reinterpret_cast<void**>(&g_oToolbarBuild)) &&
            rp::host->Detour(rp::self, reload, reinterpret_cast<void*>(&hk_ToolbarReload), reinterpret_cast<void**>(&g_oToolbarReload));
        if (!guard) { g_cancelGuard = false; rp::Log("bake: toolbar build/reload detours failed, cancel guard unavailable"); }
        rp::Log("bake: boost=%d budget_ms=%d redraw_ms=%d cancel_guard=%d", g_boost ? 1 : 0, g_boostBudgetMs, g_boostRedrawMs, g_cancelGuard ? 1 : 0);
    }

    void PollBake()
    {
        ReleaseHeld();
        bool running = BakeRunning();
        if (running && !g_bakeActive)
        {
            g_bakeActive = true; g_bakeStart = GetTickCount64(); g_skippedFrames = g_extraSteps = g_boostTicks = 0;
            rp::Log("bake: started (boost %s)", g_boost ? "on" : "off");
        }
        else if (!running && g_bakeActive)
        {
            g_bakeActive = false;
            rp::Log("bake: %s", br::bakefill::Status().c_str());
            rp::Log("bake: %s", br::bakegather::Status().c_str());
            br::bakegather::ClearStats();
            rp::Log("bake: %s", br::baketrace::Status().c_str());
            br::baketrace::ClearStats();
            br::bakefill::Clear();
            rp::Log("bake: finished after %.1f s (boost %s: %u ticks boosted, %u extra steps, %u frames skipped)",
                    double(GetTickCount64() - g_bakeStart) / 1000.0, g_boost ? "on" : "off", g_boostTicks, g_extraSteps, g_skippedFrames);
        }
    }

    std::string BakeStatus()
    {
        char buf[1024];
        uint8_t* cam = CamWnd();
        if (!cam) return "bake: no cam window yet";
        uint8_t* vb = *reinterpret_cast<uint8_t**>(cam + off_Cam_VolumesBegin);
        uint8_t* ve = *reinterpret_cast<uint8_t**>(cam + off_Cam_VolumesEnd);
        uint8_t* pb = *reinterpret_cast<uint8_t**>(cam + off_Cam_ProbesBegin);
        uint8_t* pe = *reinterpret_cast<uint8_t**>(cam + off_Cam_ProbesEnd);
        int volumes = (vb && ve && ve >= vb) ? int((ve - vb) / 16) : -1;
        int probes = (pb && pe && pe >= pb) ? int((pe - pb) / 112) : -1;
        int n = snprintf(buf, sizeof buf, "mode=%d lightstate=%d bounce=%d volumes=%d probes=%d inflight=%d running=%s elapsed=%.1fs boost=%d budget_ms=%d redraw_ms=%d boosted_ticks=%u extra_steps=%u skipped_frames=%u cancel_guard=%d held=%zu held_total=%u",
                         CamMode(cam), *reinterpret_cast<int*>(cam + off_Cam_LightState), *reinterpret_cast<int*>(cam + off_Cam_Bounce), volumes, probes,
                         g_inflight ? *g_inflight : -1, g_bakeActive ? "yes" : "no", g_bakeActive ? double(GetTickCount64() - g_bakeStart) / 1000.0 : 0.0,
                         g_boost ? 1 : 0, g_boostBudgetMs, g_boostRedrawMs, g_boostTicks, g_extraSteps, g_skippedFrames,
                         g_cancelGuard ? 1 : 0, g_held.size(), g_heldTotal);
        if (volumes > 0 && volumes < 16)
            for (int i = 0; i < volumes; ++i)
            {
                uint8_t* vol = *reinterpret_cast<uint8_t**>(vb + 16 * i);
                if (!vol) continue;
                const int* st = reinterpret_cast<const int*>(vol + off_Volume_State);
                n += snprintf(buf + n, sizeof buf - n, " vol%d=[%d %d %d %d] quality=%d", i, st[0], st[1], st[2], st[3], *reinterpret_cast<const int*>(vol + off_Volume_Quality));
                if (n >= int(sizeof buf) - 40) break;
            }
        return buf;
    }

    double ProfileSeconds(uint64_t endTick = 0)
    {
        if (!g_profile.startTick || !g_qpf.QuadPart) return 0.0;
        if (!endTick) endTick = g_profile.active ? ProfileNow() : g_profile.endTick;
        return endTick > g_profile.startTick ? double(endTick - g_profile.startTick) / double(g_qpf.QuadPart) : 0.0;
    }

    void AppendCounter(std::ostringstream& out, const char* name, const ModToolsPerfCounter& counter,
                       bool available, double seconds, uint64_t frequency)
    {
        out << "  " << name << ": ";
        if (!available)
        {
            out << "unavailable (detour was not installed)\n";
            return;
        }
        double totalMs = frequency ? double(counter.totalTicks) * 1000.0 / double(frequency) : 0.0;
        double averageMs = counter.calls ? totalMs / double(counter.calls) : 0.0;
        double minMs = counter.calls && frequency ? double(counter.minTicks) * 1000.0 / double(frequency) : 0.0;
        double maxMs = counter.calls && frequency ? double(counter.maxTicks) * 1000.0 / double(frequency) : 0.0;
        double rate = seconds > 0.0 ? double(counter.calls) / seconds : 0.0;
        double windowPercent = seconds > 0.0 ? totalMs / (seconds * 10.0) : 0.0;
        out << "calls=" << counter.calls << " rate=" << rate << "/s total=" << totalMs
            << " ms avg=" << averageMs << " ms min=" << minMs << " ms max=" << maxMs
            << " ms window=" << windowPercent << "%\n";
    }

    std::string ProfileStatus()
    {
        std::ostringstream out;
        int installed = 0;
        for (bool available : g_metricAvailable) if (available) ++installed;
        const char* state = g_profile.active ? "running" : g_profile.startTick ? "stopped" : "empty";
        out << std::fixed << std::setprecision(2)
            << "profile=" << state << " elapsed=" << ProfileSeconds() << " s window=" << g_profileWindowSeconds
            << " s label=" << (g_profileLabel.empty() ? "(none)" : g_profileLabel)
            << " internal_metrics=" << installed << "/" << PM_COUNT
            << " host_metrics=" << (HostProfileAvailable() ? "available" : "unavailable");
        return out.str();
    }

    std::string ProfileReport()
    {
        if (!g_profile.startTick) return "Radiant+ Performance Report\nNo capture is available. Run 'RadiantPlus profile start' first.";

        ModToolsHostPerfSnapshot host = {};
        bool haveHost = HostProfileAvailable() && rp::host->PerformanceProfile(rp::self, MODTOOLS_HOST_PERF_SNAPSHOT, &host) != 0;
        double seconds = ProfileSeconds();
        uint64_t frequency = static_cast<uint64_t>(g_qpf.QuadPart);
        std::ostringstream out;
        out << std::fixed << std::setprecision(3);
        out << "Radiant+ Performance Report\n";
        out << "State: " << (g_profile.active ? "RUNNING" : "STOPPED")
            << "  Window: " << seconds << " s / " << g_profileWindowSeconds << " s"
            << "  Scenario: " << (g_profileLabel.empty() ? "(unlabeled)" : g_profileLabel) << "\n";
        out << "Capture settings: uncap=" << (g_captureUncap ? 1 : 0)
            << " tick_ms(setting)=" << g_captureTickSetting
            << " timer_interval=" << g_captureTimerInterval
            << " MaxFPS=" << g_captureMaxFps
            << " UpdateXYOnDrag=";
        if (g_captureUpdateXY < 0) out << "unknown\n"; else out << g_captureUpdateXY << "\n";
        out << "Times are inclusive. window=% is one main-thread wall-clock window; nested rows overlap.\n\n";

        out << "Tick and host/plugin work\n";
        if (haveHost)
        {
            AppendCounter(out, "Raw timer callbacks / total hk_RoutineProcessing", host.metrics[MODTOOLS_HOST_PERF_ROUTINE_HOOK], true, seconds, host.qpcFrequency);
            AppendCounter(out, "RadiantMainWin::RoutineProcessing [0x14064BFB0]", host.metrics[MODTOOLS_HOST_PERF_RADIANT_ROUTINE_PROCESSING], true, seconds, host.qpcFrequency);
            AppendCounter(out, "plugins::OnTick", host.metrics[MODTOOLS_HOST_PERF_PLUGINS_ON_TICK], true, seconds, host.qpcFrequency);
        }
        else
        {
            ModToolsPerfCounter empty = {};
            AppendCounter(out, "Raw timer callbacks / total hk_RoutineProcessing", empty, false, seconds, frequency);
            AppendCounter(out, "RadiantMainWin::RoutineProcessing [0x14064BFB0]", empty, false, seconds, frequency);
            AppendCounter(out, "plugins::OnTick", empty, false, seconds, frequency);
        }
        AppendCounter(out, "RadiantPlus::OnTick", g_profile.metrics[PM_BETTER_RADIANT_TICK], g_metricAvailable[PM_BETTER_RADIANT_TICK], seconds, frequency);
        AppendCounter(out, "main-window status updater [0x14064BC40]", g_profile.metrics[PM_MAIN_STATUS], g_metricAvailable[PM_MAIN_STATUS], seconds, frequency);
        AppendCounter(out, "RadiantMainWin::UpdateWindows [0x14064C2B0]", g_profile.metrics[PM_UPDATE_WINDOWS], g_metricAvailable[PM_UPDATE_WINDOWS], seconds, frequency);
        out << "  UpdateWindows requests: camera=" << g_profile.updateCameraRequests << " 2D=" << g_profile.update2DRequests << "\n\n";

        out << "Camera\n";
        AppendCounter(out, "CCamWnd::ReloadAssets [0x140238300]", g_profile.metrics[PM_RELOAD_ASSETS], g_metricAvailable[PM_RELOAD_ASSETS], seconds, frequency);
        AppendCounter(out, "camera mouse-motion [0x140230FF0]", g_profile.metrics[PM_MOUSE_MOTION], g_metricAvailable[PM_MOUSE_MOTION], seconds, frequency);
        AppendCounter(out, "CCamWnd::RoutineProcessing [0x140232270]", g_profile.metrics[PM_CAM_ROUTINE], g_metricAvailable[PM_CAM_ROUTINE], seconds, frequency);
        AppendCounter(out, "CCamWnd::OnPaint [0x14023A540]", g_profile.metrics[PM_CAM_ON_PAINT], g_metricAvailable[PM_CAM_ON_PAINT], seconds, frequency);
        AppendCounter(out, "CCamWnd::OnRender [0x14024F080]", g_profile.metrics[PM_CAM_ON_RENDER], g_metricAvailable[PM_CAM_ON_RENDER], seconds, frequency);
        if (haveHost)
        {
            AppendCounter(out, "CCamWnd::Cam_Draw [0x140244120]", host.metrics[MODTOOLS_HOST_PERF_CAM_DRAW], true, seconds, host.qpcFrequency);
            out << "  Camera frames: " << host.metrics[MODTOOLS_HOST_PERF_CAM_DRAW].calls
                << "  FPS=" << (seconds > 0.0 ? double(host.metrics[MODTOOLS_HOST_PERF_CAM_DRAW].calls) / seconds : 0.0) << "\n";
        }
        AppendCounter(out, "BrushCache_Update [0x1402124B0]", g_profile.metrics[PM_BRUSH_CACHE], g_metricAvailable[PM_BRUSH_CACHE], seconds, frequency);
        AppendCounter(out, "  BrushCache camera context", g_profile.metrics[PM_BRUSH_CACHE_CAM], g_metricAvailable[PM_BRUSH_CACHE_CAM], seconds, frequency);
        AppendCounter(out, "CCamWnd::Cam_ProcessAllBrushes [0x140243120]", g_profile.metrics[PM_CAM_PROCESS_BRUSHES], g_metricAvailable[PM_CAM_PROCESS_BRUSHES], seconds, frequency);
        if (haveHost && host.metrics[MODTOOLS_HOST_PERF_CAM_DRAW].calls)
            out << "  Cam_ProcessAllBrushes calls/camera frame=" << double(g_profile.metrics[PM_CAM_PROCESS_BRUSHES].calls) / double(host.metrics[MODTOOLS_HOST_PERF_CAM_DRAW].calls) << "\n";
        AppendCounter(out, "  pass mode 0", g_profile.metrics[PM_CAM_PROCESS_PASS0], g_metricAvailable[PM_CAM_PROCESS_PASS0], seconds, frequency);
        AppendCounter(out, "  pass mode 1", g_profile.metrics[PM_CAM_PROCESS_PASS1], g_metricAvailable[PM_CAM_PROCESS_PASS1], seconds, frequency);
        AppendCounter(out, "  pass mode 2", g_profile.metrics[PM_CAM_PROCESS_PASS2], g_metricAvailable[PM_CAM_PROCESS_PASS2], seconds, frequency);
        AppendCounter(out, "  other pass mode", g_profile.metrics[PM_CAM_PROCESS_OTHER], g_metricAvailable[PM_CAM_PROCESS_OTHER], seconds, frequency);
        if (haveHost) AppendCounter(out, "CCamWnd::DrawHud [0x1402488A0]", host.metrics[MODTOOLS_HOST_PERF_DRAW_HUD], true, seconds, host.qpcFrequency);
        out << "\n2D viewports\n";
        AppendCounter(out, "QtXYRender::OnRender [0x140C2B300]", g_profile.metrics[PM_XY_ON_RENDER], g_metricAvailable[PM_XY_ON_RENDER], seconds, frequency);
        if (haveHost)
        {
            AppendCounter(out, "CXYWnd::XY_Draw [0x140C2E3E0]", host.metrics[MODTOOLS_HOST_PERF_XY_DRAW], true, seconds, host.qpcFrequency);
            out << "  Frames: XY=" << host.xyFrames[0] << " (" << (seconds > 0.0 ? double(host.xyFrames[0]) / seconds : 0.0)
                << "/s) XZ=" << host.xyFrames[1] << " (" << (seconds > 0.0 ? double(host.xyFrames[1]) / seconds : 0.0)
                << "/s) YZ=" << host.xyFrames[2] << " (" << (seconds > 0.0 ? double(host.xyFrames[2]) / seconds : 0.0) << "/s)\n";
        }
        AppendCounter(out, "  BrushCache 2D context", g_profile.metrics[PM_BRUSH_CACHE_XY], g_metricAvailable[PM_BRUSH_CACHE_XY], seconds, frequency);
        AppendCounter(out, "CXYWnd::XY_ProcessAllBrushes [0x140C2D5F0]", g_profile.metrics[PM_XY_PROCESS_BRUSHES], g_metricAvailable[PM_XY_PROCESS_BRUSHES], seconds, frequency);
        if (haveHost && host.metrics[MODTOOLS_HOST_PERF_XY_DRAW].calls)
            out << "  XY_ProcessAllBrushes calls/2D frame=" << double(g_profile.metrics[PM_XY_PROCESS_BRUSHES].calls) / double(host.metrics[MODTOOLS_HOST_PERF_XY_DRAW].calls) << "\n";
        AppendCounter(out, "  pass mode 0", g_profile.metrics[PM_XY_PROCESS_PASS0], g_metricAvailable[PM_XY_PROCESS_PASS0], seconds, frequency);
        AppendCounter(out, "  pass mode 1", g_profile.metrics[PM_XY_PROCESS_PASS1], g_metricAvailable[PM_XY_PROCESS_PASS1], seconds, frequency);
        AppendCounter(out, "  other pass mode", g_profile.metrics[PM_XY_PROCESS_OTHER], g_metricAvailable[PM_XY_PROCESS_OTHER], seconds, frequency);
        out << "\nRenderer\n";
        AppendCounter(out, "Renderer BeginFrame [0x1411E7710]", g_profile.metrics[PM_BEGIN_FRAME], g_metricAvailable[PM_BEGIN_FRAME], seconds, frequency);
        AppendCounter(out, "Renderer EndFrame [0x1411E7760]", g_profile.metrics[PM_END_FRAME], g_metricAvailable[PM_END_FRAME], seconds, frequency);
        AppendCounter(out, "DXGI Present [0x1411AEDA0]", g_profile.metrics[PM_PRESENT], g_metricAvailable[PM_PRESENT], seconds, frequency);
        AppendCounter(out, "  Present camera context", g_profile.metrics[PM_PRESENT_CAM], g_metricAvailable[PM_PRESENT_CAM], seconds, frequency);
        AppendCounter(out, "  Present 2D context", g_profile.metrics[PM_PRESENT_XY], g_metricAvailable[PM_PRESENT_XY], seconds, frequency);
        AppendCounter(out, "  Present other context", g_profile.metrics[PM_PRESENT_OTHER], g_metricAvailable[PM_PRESENT_OTHER], seconds, frequency);
        return out.str();
    }


    // ---- Qt action lookup (no Qt headers: Radiant's Qt5 exports, MS x64 ABI, QList/QString returned through a hidden pointer) ----
    struct QListDataD { int ref; int alloc; int begin; int end; void* array[1]; };
    struct QListRaw { QListDataD* d; };
    struct QArrayDataRaw { int ref; int size; unsigned alloc; ptrdiff_t offset; };
    using AllWidgetsFn = QListRaw* (*)(QListRaw* ret);
    using ActionsFn    = QListRaw* (*)(const void* widget, QListRaw* ret);
    using ActionTextFn = void** (*)(const void* action, void** retQString);
    using ActivateFn   = void (*)(void* action, int event);
    using MenuOfFn     = void* (*)(const void* action);
    using ListDisposeFn = void (*)(QListDataD* d);
    using QStringDtorFn = void (*)(void** s);

    std::string QStringToUtf8(void** qs)
    {
        QArrayDataRaw* d = static_cast<QArrayDataRaw*>(*qs);
        if (!d || d->size <= 0) return std::string();
        const wchar_t* w = reinterpret_cast<const wchar_t*>(reinterpret_cast<char*>(d) + d->offset);
        int n = WideCharToMultiByte(CP_UTF8, 0, w, d->size, nullptr, 0, nullptr, nullptr);
        std::string out(n > 0 ? n : 0, '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, d->size, &out[0], n, nullptr, nullptr);
        return out;
    }
    void ListRelease(QListRaw& l, ListDisposeFn dispose)
    {
        if (!l.d || l.d->ref == -1) return;
        if (InterlockedDecrement(reinterpret_cast<volatile long*>(&l.d->ref)) == 0 && dispose) dispose(l.d);
        l.d = nullptr;
    }

    // Visit every QAction attached to any widget (menus included, one level): fn(action, text) returns true to stop.
    template <class Fn> bool ForEachAction(Fn fn)
    {
        HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll"), c = GetModuleHandleW(L"Qt5Core64r.dll");
        if (!w || !c) return false;
        auto allWidgets = reinterpret_cast<AllWidgetsFn>(GetProcAddress(w, "?allWidgets@QApplication@@SA?AV?$QList@PEAVQWidget@@@@XZ"));
        auto actions = reinterpret_cast<ActionsFn>(GetProcAddress(w, "?actions@QWidget@@QEBA?AV?$QList@PEAVQAction@@@@XZ"));
        auto text = reinterpret_cast<ActionTextFn>(GetProcAddress(w, "?text@QAction@@QEBA?AVQString@@XZ"));
        auto tip = reinterpret_cast<ActionTextFn>(GetProcAddress(w, "?toolTip@QAction@@QEBA?AVQString@@XZ"));
        auto dispose = reinterpret_cast<ListDisposeFn>(GetProcAddress(c, "?dispose@QListData@@SAXPEAUData@1@@Z"));
        auto qsDtor = reinterpret_cast<QStringDtorFn>(GetProcAddress(c, "??1QString@@QEAA@XZ"));
        if (!allWidgets || !actions || !text || !qsDtor) return false;
        QListRaw widgets = {}; allWidgets(&widgets);
        bool stopped = false;
        if (widgets.d)
            for (int i = widgets.d->begin; i < widgets.d->end && !stopped; ++i)
            {
                void* widget = widgets.d->array[i];
                if (!widget) continue;
                QListRaw acts = {}; actions(widget, &acts);
                if (acts.d)
                    for (int j = acts.d->begin; j < acts.d->end && !stopped; ++j)
                    {
                        void* a = acts.d->array[j];
                        if (!a) continue;
                        void* qs = nullptr; text(a, &qs);
                        std::string t = QStringToUtf8(&qs);
                        qsDtor(&qs);
                        if (t.empty() && tip) { void* qt = nullptr; tip(a, &qt); t = "tip:" + QStringToUtf8(&qt); qsDtor(&qt); }
                        if (fn(a, t)) stopped = true;
                    }
                ListRelease(acts, dispose);
            }
        ListRelease(widgets, dispose);
        return true;
    }

    std::string StripMnemonic(const std::string& s)
    {
        std::string o; for (char ch : s) if (ch != '&') o += ch; return o;
    }

    bool TriggerActionByText(const char* wanted, std::string& found)
    {
        HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll");
        auto activate = w ? reinterpret_cast<ActivateFn>(GetProcAddress(w, "?activate@QAction@@QEAAXW4ActionEvent@1@@Z")) : nullptr;
        if (!activate) return false;
        void* hit = nullptr;
        ForEachAction([&](void* a, const std::string& t) {
            if (_stricmp(StripMnemonic(t).c_str(), wanted) == 0) { hit = a; found = t; return true; }
            return false;
        });
        if (!hit) return false;
        activate(hit, 0);   // QAction::Trigger
        return true;
    }


    // Widgets by tooltip (the Build Lighting button is a QToolButton with no QAction text): fn(widget, className, toolTip).
    struct QMetaObjectRaw;
    using ClassNameFn = const char* (*)(const QMetaObjectRaw* mo);
    template <class Fn> bool ForEachWidget(Fn fn)
    {
        HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll"), c = GetModuleHandleW(L"Qt5Core64r.dll");
        if (!w || !c) return false;
        auto allWidgets = reinterpret_cast<AllWidgetsFn>(GetProcAddress(w, "?allWidgets@QApplication@@SA?AV?$QList@PEAVQWidget@@@@XZ"));
        auto tip = reinterpret_cast<ActionTextFn>(GetProcAddress(w, "?toolTip@QWidget@@QEBA?AVQString@@XZ"));
        auto className = reinterpret_cast<ClassNameFn>(GetProcAddress(c, "?className@QMetaObject@@QEBAPEBDXZ"));
        auto dispose = reinterpret_cast<ListDisposeFn>(GetProcAddress(c, "?dispose@QListData@@SAXPEAUData@1@@Z"));
        auto qsDtor = reinterpret_cast<QStringDtorFn>(GetProcAddress(c, "??1QString@@QEAA@XZ"));
        if (!allWidgets || !tip || !className || !qsDtor) return false;
        QListRaw widgets = {}; allWidgets(&widgets);
        bool stopped = false;
        if (widgets.d)
            for (int i = widgets.d->begin; i < widgets.d->end && !stopped; ++i)
            {
                void* widget = widgets.d->array[i];
                if (!widget) continue;
                // QObject vtable slot 0 = metaObject()
                auto metaObject = (*reinterpret_cast<const QMetaObjectRaw* (__fastcall ***)(void*)>(widget))[0];
                const char* cls = className(metaObject(widget));
                void* qs = nullptr; tip(widget, &qs);
                std::string t = QStringToUtf8(&qs);
                qsDtor(&qs);
                if (fn(widget, cls ? cls : "", t)) stopped = true;
            }
        ListRelease(widgets, dispose);
        return true;
    }

    // Also finds QAction children not attached to a displayed menu.
    template <class Fn> void ForEachObjectAction(Fn fn)
    {
        HMODULE core = GetModuleHandleW(L"Qt5Core64r.dll"), widgets = GetModuleHandleW(L"Qt5Widgets64r.dll");
        auto children = reinterpret_cast<const QListRaw* (*)(const void*)>(GetProcAddress(core, "?children@QObject@@QEBAAEBV?$QList@PEAVQObject@@@@XZ"));
        auto className = reinterpret_cast<ClassNameFn>(GetProcAddress(core, "?className@QMetaObject@@QEBAPEBDXZ"));
        auto text = reinterpret_cast<ActionTextFn>(GetProcAddress(widgets, "?text@QAction@@QEBA?AVQString@@XZ"));
        auto destroy = reinterpret_cast<QStringDtorFn>(GetProcAddress(core, "??1QString@@QEAA@XZ"));
        if (!children || !className || !text || !destroy) return;
        std::vector<void*> pending;
        std::unordered_set<void*> seen;
        ForEachWidget([&](void* widget, const char*, const std::string&) { pending.push_back(widget); return false; });
        while (!pending.empty())
        {
            void* object = pending.back(); pending.pop_back();
            if (!object || !seen.insert(object).second) continue;
            auto metaObject = (*reinterpret_cast<const QMetaObjectRaw* (__fastcall ***)(void*)>(object))[0];
            const char* cls = className(metaObject(object));
            if (cls && (!strcmp(cls, "QAction") || !strcmp(cls, "QWidgetAction")))
            {
                void* value = nullptr; text(object, &value); const std::string label = QStringToUtf8(&value); destroy(&value);
                if (fn(object, label)) return;
            }
            const QListRaw* list = children(object); // borrowed, QObject-owned list, do not release
            if (list && list->d) for (int i = list->d->begin; i < list->d->end; ++i) pending.push_back(list->d->array[i]);
        }
    }

    bool ClickButtonByToolTip(const char* prefix, std::string& cls)
    {
        HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll");
        auto click = w ? reinterpret_cast<void (*)(void*)>(GetProcAddress(w, "?click@QAbstractButton@@QEAAXXZ")) : nullptr;
        if (!click) return false;
        void* hit = nullptr;
        ForEachWidget([&](void* wd, const char* c, const std::string& t) {
            if (t.rfind(prefix, 0) == 0 && strstr(c, "Button")) { hit = wd; cls = c; return true; }
            return false;
        });
        if (!hit) return false;
        click(hit);   // QAbstractButton::click
        return true;
    }

    void* g_llRadio = nullptr;     // our QRadioButton for the draft tier (LL)
    void* g_llAction = nullptr;    // the toolbar's QWidgetAction that owns it
    void* g_llToolbar = nullptr;

    // Lists the toolbar radio buttons (QRadioButton, text LL / L / M / H) or clicks the one for quality ll|low|medium|high.
    std::string BakeQualityRadio(const std::string& quality)
    {
        HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll"), core = GetModuleHandleW(L"Qt5Core64r.dll");
        auto click = w ? reinterpret_cast<void (*)(void*)>(GetProcAddress(w, "?click@QAbstractButton@@QEAAXXZ")) : nullptr;
        auto text = w ? reinterpret_cast<ActionTextFn>(GetProcAddress(w, "?text@QAbstractButton@@QEBA?AVQString@@XZ")) : nullptr;
        auto checked = w ? reinterpret_cast<bool (*)(const void*)>(GetProcAddress(w, "?isChecked@QAbstractButton@@QEBA_NXZ")) : nullptr;
        auto qsDtor = core ? reinterpret_cast<QStringDtorFn>(GetProcAddress(core, "??1QString@@QEAA@XZ")) : nullptr;
        if (!click || !text || !checked || !qsDtor) return "Qt button functions not found";
        const char* want = quality == "ll" || quality == "draft" ? "LL" : quality == "low" ? "L" : quality == "medium" ? "M" : quality == "high" ? "H" : nullptr;
        std::string list; void* hit = nullptr;
        ForEachWidget([&](void* wd, const char* cls, const std::string& tip) {
            if (strcmp(cls, "QRadioButton") != 0) return false;
            void* qs = nullptr; text(wd, &qs);
            std::string t = StripMnemonic(QStringToUtf8(&qs)); qsDtor(&qs);
            list += t + (checked(wd) ? "*" : "") + (tip.empty() ? "" : " (" + tip + ")") + " ";
            if (want && t == want) hit = wd;
            return false;
        });
        if (want && !strcmp(want, "LL")) hit = g_llRadio;
        if (!want) return "quality radios: " + list;
        if (!hit) return "no " + std::string(want) + " radio button; radios: " + list;
        if (!checked(hit)) click(hit);
        rp::Log("bake: quality %s selected", quality.c_str());
        return "selected " + quality;
    }

    // ---- LL radio: the draft bake tier ----
    // A fourth quality radio in front of L / M / H on the toolbar, in their button group. bake_lowrays.cpp asks DraftWanted at build start. [bake] draft_radio=0 leaves the toolbar alone.
    bool DraftWanted() { return g_llRadio && rp::host->ui && rp::host->ui->IsChecked(g_llRadio); }
    bool DraftBusy() { return BakeStateActive(CamWnd()); }

    // The toolbar's own radio for a quality (0 L, 1 M, 2 H), by its tooltip.
    void* ToolbarQualityRadio(int quality)
    {
        const char* want = quality == 0 ? "Low Quality Bake" : quality == 1 ? "Medium Quality Bake" : "High Quality Bake";
        void* hit = nullptr;
        ForEachWidget([&](void* wd, const char* cls, const std::string& tip) {
            if (strcmp(cls, "QRadioButton") != 0 || tip != want) return false;
            hit = wd;
            return true;
        });
        return hit;
    }
    void* LowQualityRadio() { return ToolbarQualityRadio(0); }

    // The same radio in the Export Lighting dialog (QtLightingExportDialog, built per use on the stack): its uic struct at dialog+56 holds the options widget [1],
    // the quality row's QHBoxLayout [3] (label, L, M, H) and the L radio [5], the radios have no button group. On OK the toolbar takes the dialog's choice,
    // since the build start asks the toolbar's LL radio.
    const uintptr_t kExportDialogCtor = 0x140930A20;   // QtLightingExportDialog*(this, QWidget* parent)
    const uintptr_t kExportDialogOpts = 0x140930C40;   // bool* (dialog, bool out[5]): after exec, on OK only
    using ExportDialogCtorFn = int64_t(__fastcall*)(int64_t, int64_t);
    using ExportDialogOptsFn = bool* (__fastcall*)(int64_t, bool*);
    ExportDialogCtorFn g_oExportDialogCtor = nullptr;
    ExportDialogOptsFn g_oExportDialogOpts = nullptr;
    int64_t g_llDialog = 0;           // the dialog our radio is in
    void* g_llDialogRadio = nullptr;  // owned by that dialog

    void AddDraftRadioToExportDialog(int64_t dialog)
    {
        g_llDialog = 0; g_llDialogRadio = nullptr;
        if (!g_llRadio || !dialog || !rp::host->ui) return;
        void** ui = *reinterpret_cast<void***>(dialog + 56);
        HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll");
        struct QRectRaw { int x1, y1, x2, y2; };
        auto insertWidget = reinterpret_cast<void (*)(void*, int, void*, int, int)>(GetProcAddress(w, "?insertWidget@QBoxLayout@@QEAAXHPEAVQWidget@@HV?$QFlags@W4AlignmentFlag@Qt@@@@@Z"));
        auto geometry = reinterpret_cast<const QRectRaw* (*)(const void*)>(GetProcAddress(w, "?geometry@QWidget@@QEBAAEBVQRect@@XZ"));
        auto setGeometry = reinterpret_cast<void (*)(void*, const QRectRaw*)>(GetProcAddress(w, "?setGeometry@QWidget@@QEAAXAEBVQRect@@@Z"));
        auto sizeHint = reinterpret_cast<int* (*)(const void*, int*)>(GetProcAddress(w, "?sizeHint@QRadioButton@@UEBA?AVQSize@@XZ"));
        auto resize = reinterpret_cast<void (*)(void*, const int*)>(GetProcAddress(w, "?resize@QWidget@@QEAAXAEBVQSize@@@Z"));
        if (!ui || !ui[0] || !ui[1] || !ui[3] || !ui[5] || !ui[13] || !insertWidget || !geometry || !setGeometry || !sizeHint || !resize) return;
        void* radio = rp::host->ui->RadioButton("LL", ui[1]);
        if (!radio) return;
        rp::host->ui->SetToolTip(radio, "Draft Quality Bake");
        insertWidget(ui[3], 1, radio, 0, 0);   // after the "Export quality:" label, in front of L
        // Everything grows by the new radio's width (the parents have fixed geometry).
        int hint[2] = {0, 0}; sizeHint(radio, hint);
        const int grow = hint[0] > 0 && hint[0] < 200 ? hint[0] + 6 : 44;
        auto widen = [&](void* wd, int dx, int dw) { QRectRaw r = *geometry(wd); r.x1 += dx; r.x2 += dx + dw; setGeometry(wd, &r); };
        // The dialog by resize: setGeometry would set WA_Moved and QDialog would skip centring it on Radiant.
        const QRectRaw d = *geometry(reinterpret_cast<void*>(dialog));
        const int size[2] = {d.x2 - d.x1 + 1 + grow, d.y2 - d.y1 + 1};
        resize(reinterpret_cast<void*>(dialog), size);
        widen(ui[0], 0, grow);          // the Options group box
        widen(ui[1], 0, grow);          // its content widget
        widen(ui[13], grow / 2, 0);     // OK / Cancel stay centred
        if (DraftWanted()) rp::host->ui->SetChecked(radio, 1);
        g_llDialog = dialog; g_llDialogRadio = radio;
    }

    int64_t __fastcall hk_ExportDialogCtor(int64_t dialog, int64_t parent)
    {
        const int64_t r = g_oExportDialogCtor(dialog, parent);
        __try { AddDraftRadioToExportDialog(dialog); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_llDialog = 0; g_llDialogRadio = nullptr; }
        return r;
    }

    void SyncToolbarFromExportDialog()
    {
        const int* quality = static_cast<const int*>(rp::host->RadiantAddress(rp::self, 0x1453A23A4));
        void* pick = rp::host->ui->IsChecked(g_llDialogRadio) ? g_llRadio : quality ? ToolbarQualityRadio(*quality) : nullptr;
        if (pick && !rp::host->ui->IsChecked(pick)) rp::host->ui->SetChecked(pick, 1);
    }

    bool* __fastcall hk_ExportDialogOpts(int64_t dialog, bool* out)
    {
        if (dialog && dialog == g_llDialog && g_llDialogRadio && g_llRadio)
        {
            __try { SyncToolbarFromExportDialog(); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        g_llDialog = 0; g_llDialogRadio = nullptr;
        return g_oExportDialogOpts(dialog, out);
    }

    void SetupDraftRadio()
    {
        if (g_llRadio || !br::bakell::Switchable() || rp::ConfigGet("bake", "draft_radio", "1") == "0") return;
        HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll");
        if (!w || !rp::host->ui || !rp::HasUi(&rp::host->ui->SetToolTip)) return;
        HMODULE core = GetModuleHandleW(L"Qt5Core64r.dll");
        auto parentWidget = reinterpret_cast<void* (*)(const void*)>(GetProcAddress(w, "?parentWidget@QWidget@@QEBAPEAV1@XZ"));
        auto actions = reinterpret_cast<ActionsFn>(GetProcAddress(w, "?actions@QWidget@@QEBA?AV?$QList@PEAVQAction@@@@XZ"));
        auto widgetFor = reinterpret_cast<void* (*)(const void*, void*)>(GetProcAddress(w, "?widgetForAction@QToolBar@@QEBAPEAVQWidget@@PEAVQAction@@@Z"));
        auto dispose = core ? reinterpret_cast<ListDisposeFn>(GetProcAddress(core, "?dispose@QListData@@SAXPEAUData@1@@Z")) : nullptr;
        auto insertWidget = reinterpret_cast<void* (*)(void*, void*, void*)>(GetProcAddress(w, "?insertWidget@QToolBar@@QEAAPEAVQAction@@PEAV2@PEAVQWidget@@@Z"));
        auto group = reinterpret_cast<void* (*)(const void*)>(GetProcAddress(w, "?group@QAbstractButton@@QEBAPEAVQButtonGroup@@XZ"));
        auto addButton = reinterpret_cast<void (*)(void*, void*, int)>(GetProcAddress(w, "?addButton@QButtonGroup@@QEAAXPEAVQAbstractButton@@H@Z"));
        void* low = LowQualityRadio();
        void* toolbar = low && parentWidget ? parentWidget(low) : nullptr;
        void* buttons = low && group ? group(low) : nullptr;
        if (!toolbar || !buttons || !actions || !widgetFor || !insertWidget || !addButton)
        { rp::Log("bake: LL radio not added (the quality radios or their Qt functions were not found)"); return; }
        // The toolbar action holding the L radio, found by identity: plugins load before layout (all widgets at 0,0), so a lookup by position picked the wrong widget.
        void* before = nullptr;
        QListRaw list = {}; actions(toolbar, &list);
        if (list.d)
            for (int i = list.d->begin; i < list.d->end && !before; ++i)
                if (list.d->array[i] && widgetFor(toolbar, list.d->array[i]) == low) before = list.d->array[i];
        ListRelease(list, dispose);
        if (!before) { rp::Log("bake: LL radio not added (the L radio's toolbar action was not found)"); return; }
        void* radio = rp::host->ui->RadioButton("LL", toolbar);
        if (!radio) return;
        rp::host->ui->SetToolTip(radio, "Draft Quality Bake");
        g_llAction = insertWidget(toolbar, before, radio);
        addButton(buttons, radio, -1);
        g_llRadio = radio; g_llToolbar = toolbar;
        // Reloaded with the tier still on (bake_lowrays.cpp keeps it over an unload): show it, unless M or H is picked.
        const int* quality = static_cast<const int*>(rp::host->RadiantAddress(rp::self, 0x1453A23A4));
        if (br::bakell::Active() && quality && *quality == 0) rp::host->ui->SetChecked(radio, 1);
        void* ctor = rp::host->RadiantAddress(rp::self, kExportDialogCtor);
        void* opts = rp::host->RadiantAddress(rp::self, kExportDialogOpts);
        const bool dialog = ctor && opts &&
            rp::host->Detour(rp::self, ctor, reinterpret_cast<void*>(&hk_ExportDialogCtor), reinterpret_cast<void**>(&g_oExportDialogCtor)) &&
            rp::host->Detour(rp::self, opts, reinterpret_cast<void*>(&hk_ExportDialogOpts), reinterpret_cast<void**>(&g_oExportDialogOpts));
        rp::Log("bake: LL radio added to the lighting toolbar%s", dialog ? " and the Export Lighting dialog" : " (not to the Export Lighting dialog: hooks failed)");
    }

    void RemoveDraftRadio()
    {
        if (!g_llRadio) return;
        HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll");
        auto group = reinterpret_cast<void* (*)(const void*)>(GetProcAddress(w, "?group@QAbstractButton@@QEBAPEAVQButtonGroup@@XZ"));
        auto removeButton = reinterpret_cast<void (*)(void*, void*)>(GetProcAddress(w, "?removeButton@QButtonGroup@@QEAAXPEAVQAbstractButton@@@Z"));
        auto removeAction = reinterpret_cast<void (*)(void*, void*)>(GetProcAddress(w, "?removeAction@QWidget@@QEAAXPEAVQAction@@@Z"));
        // Low stays selected in its place, the tier stays on until the next build with the radio back.
        if (rp::host->ui->IsChecked(g_llRadio))
            if (void* low = LowQualityRadio()) rp::host->ui->SetChecked(low, 1);
        if (group && removeButton) if (void* buttons = group(g_llRadio)) removeButton(buttons, g_llRadio);
        // The action deletes its widget. Delete now, not deleteLater (never processed from the command channel, themes.cpp DeleteNow). QObject vtable slot 3.
        bool deleted = false;
        if (g_llAction && removeAction)
        {
            removeAction(g_llToolbar, g_llAction);
            __try { (*reinterpret_cast<void (__fastcall***)(void*, unsigned)>(g_llAction))[3](g_llAction, 1); deleted = true; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        if (!deleted) rp::host->ui->SetVisible(g_llRadio, 0);
        g_llRadio = g_llAction = g_llToolbar = nullptr;
        g_llDialog = 0; g_llDialogRadio = nullptr;   // an open dialog keeps its radio (the dialog owns it)
    }

    // ---- DXVK toggle ----
    // Radiant loads dxgi.dll/d3d11.dll from its exe TLS initialiser, so the only way to swap in DXVK is the DLLs next to the exe winning the OS search order.
    // On copies <plugin folder>\dxvk\{dxgi,d3d11}.dll into <root>\bin, off removes them (renamed to *.dxvk-off while an editor maps them). Applies at the next start, to every program in bin.
    std::wstring DxvkExeDir()
    {
        wchar_t p[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, p, MAX_PATH * 2);
        PathRemoveFileSpecW(p);
        return p;
    }
    std::wstring DxvkPluginDir()
    {
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&DxvkPluginDir), &m);
        wchar_t p[MAX_PATH * 2] = {};
        GetModuleFileNameW(m, p, MAX_PATH * 2);
        PathRemoveFileSpecW(p);
        return p;
    }
    std::string DxvkNarrow(const std::wstring& w)
    {
        std::string out(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &out[0], int(out.size()), nullptr, nullptr);
        if (!out.empty() && out.back() == '\0') out.pop_back();
        return out;
    }
    // true when the file's version resource says CompanyName "DXVK" (its DLLs carry fake Windows version numbers)
    bool DxvkIsDxvkFile(const std::wstring& path)
    {
        DWORD dummy = 0, size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
        if (!size) return false;
        std::vector<char> data(size);
        if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return false;
        struct Lang { WORD lang, cp; } * langs = nullptr; UINT len = 0;
        if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &len) || len < sizeof(Lang)) return false;
        for (UINT i = 0; i < len / sizeof(Lang); ++i)
        {
            wchar_t key[64];
            swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\CompanyName", langs[i].lang, langs[i].cp);
            wchar_t* value = nullptr; UINT vlen = 0;
            if (VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&value), &vlen) && value && _wcsicmp(value, L"DXVK") == 0) return true;
        }
        return false;
    }
    std::string DxvkBundledVersion()
    {
        FILE* f = _wfopen((DxvkPluginDir() + L"\\dxvk\\version.txt").c_str(), L"rb");
        if (!f) return "";
        char buf[32] = {}; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f);
        std::string v(buf, n);
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
        return v;
    }
    // active in this process? (dxgi.dll mapped from a DXVK file)
    bool DxvkLive(std::wstring* pathOut)
    {
        HMODULE m = GetModuleHandleW(L"dxgi.dll");
        if (!m) return false;
        wchar_t path[MAX_PATH * 2] = {};
        GetModuleFileNameW(m, path, MAX_PATH * 2);
        if (pathOut) *pathOut = path;
        if (DxvkIsDxvkFile(path)) return true;
        // just switched off: the mapped file was renamed to *.dxvk-off, the module is still DXVK
        return !PathFileExistsW(path) && DxvkIsDxvkFile(std::wstring(path) + L".dxvk-off");
    }
    bool DxvkSourcePresent()
    {
        const std::wstring d = DxvkPluginDir() + L"\\dxvk";
        return PathFileExistsW((d + L"\\dxgi.dll").c_str()) && PathFileExistsW((d + L"\\d3d11.dll").c_str());
    }
    // the setting = DXVK's two files in bin
    bool DxvkInstalledInBin()
    {
        const std::wstring b = DxvkExeDir();
        return DxvkIsDxvkFile(b + L"\\dxgi.dll") && DxvkIsDxvkFile(b + L"\\d3d11.dll");
    }
    // *.dxvk-off leftovers go once nothing maps them
    void DxvkCleanup()
    {
        for (const wchar_t* n : { L"dxgi.dll.dxvk-off", L"d3d11.dll.dxvk-off" })
            DeleteFileW((DxvkExeDir() + L"\\" + n).c_str());
    }
    std::string DxvkStatus()
    {
        DxvkCleanup();
        std::wstring path;
        const bool live = DxvkLive(&path);
        const bool on = DxvkInstalledInBin();
        const std::string ver = DxvkBundledVersion();
        std::string out = live ? "DXVK active in this Radiant (" + DxvkNarrow(path) + ")" : "DXVK not active in this Radiant (native Direct3D 11)";
        out += "; setting: " + std::string(on ? "on (dxgi.dll + d3d11.dll in bin)" : "off");
        if (on != live) out += " (restart Radiant to apply)";
        if (on) out += "; note: APE uses DXVK too while on";
        if (!DxvkSourcePresent()) out += "; the bundled copy is missing: put x64 dxgi.dll and d3d11.dll from github.com/doitsujin/dxvk/releases into " + DxvkNarrow(DxvkPluginDir() + L"\\dxvk");
        else if (!ver.empty()) out += "; bundled DXVK " + ver;
        return out;
    }
    std::string DxvkSet(bool on)
    {
        const std::wstring bin = DxvkExeDir(), src = DxvkPluginDir() + L"\\dxvk";
        std::string problem;
        for (const wchar_t* n : { L"dxgi.dll", L"d3d11.dll" })
        {
            const std::wstring dst = bin + L"\\" + n;
            if (on)
            {
                if (!DxvkSourcePresent()) { problem = "bundled DXVK files missing"; break; }
                if (PathFileExistsW(dst.c_str()) && !DxvkIsDxvkFile(dst)) { problem = "bin\\" + DxvkNarrow(n) + " exists and is not DXVK's, not touching it"; break; }
                if (!CopyFileW((src + L"\\" + n).c_str(), dst.c_str(), FALSE))
                {
                    const DWORD err = GetLastError();
                    if (err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED) continue;   // an editor maps the old copy: same DXVK build stays
                    problem = "copy of " + DxvkNarrow(n) + " into bin failed (error " + std::to_string(err) + ")"; break;
                }
            }
            else
            {
                if (!PathFileExistsW(dst.c_str())) continue;
                if (!DxvkIsDxvkFile(dst)) { problem = "bin\\" + DxvkNarrow(n) + " is not DXVK's, not touching it"; break; }
                if (DeleteFileW(dst.c_str())) continue;
                const std::wstring off = dst + L".dxvk-off";
                DeleteFileW(off.c_str());
                if (!MoveFileExW(dst.c_str(), off.c_str(), MOVEFILE_REPLACE_EXISTING))
                { problem = "could not remove bin\\" + DxvkNarrow(n) + " (error " + std::to_string(GetLastError()) + ")"; break; }
            }
        }
        rp::Log("dxvk: set %s%s%s", on ? "on" : "off", problem.empty() ? "" : ": ", problem.c_str());
        std::string out = DxvkStatus();
        if (!problem.empty()) out = "DXVK: " + problem + "; " + out;
        return out;
    }
    // Preferences (themes.cpp, Camera tab) asks on OK/Apply, the change runs on a tick once Preferences is closed so the restart question never runs inside its loop.
    int g_dxvkPending = -1;                 // -1 = nothing asked, else the setting asked for

    // Radiant's own message box (QMessageBox through its QuestionBox helper): buttons are QMessageBox standard buttons, returns the one pressed.
    const uintptr_t kDxvkQuestionBox = 0x140D337C0;   // (parent, text, title, buttons, icon), see RADIANT_FACTS
    bool DxvkAsk(const std::string& text, bool yesNo)
    {
        using Fn = __int64(__fastcall*)(void*, const char*, const char*, int, int);
        const int kYes = 0x4000, kNo = 0x10000, kOk = 0x400;
        if (Fn fn = reinterpret_cast<Fn>(rp::host->RadiantAddress(rp::self, kDxvkQuestionBox)))
        {
            // Radiant's box puts the text up to the first '\n' in setText and the rest in setInformativeText, so a blank line there doubles the gap
            std::string t = text;
            size_t nl = t.find("\n\n");
            if (nl != std::string::npos && nl == t.find('\n')) t.erase(nl, 1);
            const __int64 r = fn(nullptr, t.c_str(), "Radiant+", yesNo ? (kYes | kNo) : kOk, 0);   // last argument: QMessageBox::Icon, 0 = none
            return r == kYes;
        }
        return MessageBoxA(nullptr, text.c_str(), "Radiant+", yesNo ? (MB_YESNO | MB_ICONQUESTION) : (MB_OK | MB_ICONINFORMATION)) == IDYES;
    }

    // Restart Radiant with the same command line: a hidden PowerShell waits for this process to end (up to 10 minutes), starts it again, then File/Exit.
    std::string DxvkPsQuote(const std::wstring& w)
    {
        std::string s = DxvkNarrow(w), out;
        for (char c : s) { out += c; if (c == '\'') out += '\''; }
        return out;
    }
    void DxvkRestartRadiant()
    {
        wchar_t cwd[MAX_PATH * 2] = {}, exe[MAX_PATH * 2] = {};
        GetCurrentDirectoryW(MAX_PATH * 2, cwd);
        GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
        // everything after the first token of the command line
        std::wstring cl = GetCommandLineW(), args;
        size_t i = 0;
        if (!cl.empty() && cl[0] == L'"') { i = cl.find(L'"', 1); i = (i == std::wstring::npos) ? cl.size() : i + 1; }
        else { i = cl.find(L' '); if (i == std::wstring::npos) i = cl.size(); }
        while (i < cl.size() && cl[i] == L' ') ++i;
        args = cl.substr(i);
        const std::wstring ps1 = DxvkPluginDir() + L"\\dxvk\\restart_radiant.ps1";
        FILE* f = _wfopen(ps1.c_str(), L"wb");
        if (!f) { rp::Log("dxvk: cannot write %s", DxvkNarrow(ps1).c_str()); return; }
        std::string text = "$p = Get-Process -Id " + std::to_string(GetCurrentProcessId()) + " -ErrorAction SilentlyContinue\r\n"
                           "if ($p) { $p.WaitForExit(600000) | Out-Null }\r\n"
                           "Start-Process -FilePath '" + DxvkPsQuote(exe) + "' -WorkingDirectory '" + DxvkPsQuote(cwd) + "'" +
                           (args.empty() ? "" : " -ArgumentList '" + DxvkPsQuote(args) + "'") + "\r\n"
                           "Remove-Item -LiteralPath $MyInvocation.MyCommand.Path -ErrorAction SilentlyContinue\r\n";
        fwrite(text.data(), 1, text.size(), f); fclose(f);
        std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + ps1 + L"\"";
        STARTUPINFOW si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
        else { rp::Log("dxvk: restart helper failed (%lu)", GetLastError()); return; }
        rp::Log("dxvk: restart requested (%s %s), File/Exit", DxvkNarrow(exe).c_str(), DxvkNarrow(args).c_str());
        if (!rp::host->TriggerMenuAction || !rp::host->TriggerMenuAction(rp::self, "File/Exit"))
        {
            struct Find { static BOOL CALLBACK Cb(HWND h, LPARAM lp) { DWORD p = 0; GetWindowThreadProcessId(h, &p); RECT r{}; GetWindowRect(h, &r);
                if (p == GetCurrentProcessId() && IsWindowVisible(h) && r.right - r.left > 800) { *reinterpret_cast<HWND*>(lp) = h; return FALSE; } return TRUE; } };
            HWND main = nullptr; EnumWindows(Find::Cb, reinterpret_cast<LPARAM>(&main));
            if (main) PostMessageW(main, WM_CLOSE, 0, 0);
        }
    }

    // the Preferences request, from the tick: set it, then offer the restart when this Radiant runs the other renderer
    void DxvkApplyPending()
    {
        if (g_dxvkPending < 0 || br::theme::PrefsOpen()) return;
        const bool on = g_dxvkPending == 1;
        g_dxvkPending = -1;
        if (DxvkInstalledInBin() == on) return;
        const std::string reply = DxvkSet(on);
        if (DxvkInstalledInBin() != on)   // nothing changed: say why, no restart offer
        {
            std::string why = !DxvkSourcePresent()
                ? "Its files are missing from " + DxvkNarrow(DxvkPluginDir() + L"\\dxvk") + ". Reinstall MTPlugins with Radiant+ ticked (it ships DXVK), or put x64 dxgi.dll and d3d11.dll from github.com/doitsujin/dxvk/releases there."
                : reply;
            DxvkAsk(std::string("DXVK (Vulkan) renderer could not be turned ") + (on ? "on" : "off") + ".\n\n" + why, false);
            return;
        }
        std::wstring path;
        const bool live = DxvkLive(&path);
        if (live == on) return;   // switched back before a restart: already running it
        std::string text = std::string("DXVK (Vulkan) renderer turned ") + (on ? "on" : "off") + ".";
        if (on) text += "\n\nWhile on, APE uses it too.";
        text += "\n\nRestart Radiant now to apply?";
        if (DxvkAsk(text, true)) DxvkRestartRadiant();
    }

    // One-time Max FPS default in the prefs block and the registry, a later change in Preferences is kept.
    void ApplyFirstMaxFps()
    {
        const int applied = rp::ConfigGetInt("fps", "max_fps_default_applied", 0);
        if (applied >= 2) return;
        const float cur = MaxFps();
        // 1 = the 9999 default ran here: replace only what it left behind
        if (applied == 1 && cur >= 1.0f && cur != 9999.0f)
        {
            rp::ConfigSet("fps", "max_fps_default_applied", "2");
            return;
        }
        const float fps = 144.0f;
        if (g_maxFps) *const_cast<float*>(g_maxFps) = fps;
        uint32_t u; memcpy(&u, &fps, 4);
        // QSettings float: type 0x87, then the big-endian value
        const unsigned char bytes[] = { 0, 0, 0, 0x87, uint8_t(u >> 24), uint8_t(u >> 16), uint8_t(u >> 8), uint8_t(u) };
        std::wstring v = L"@Variant(";
        for (unsigned char b : bytes) v += static_cast<wchar_t>(b);
        v += L")";
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Treyarch\\Radiant\\Prefs", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS)
        {
            RegSetValueExW(key, L"MaxFPS", 0, REG_BINARY, reinterpret_cast<const BYTE*>(v.data()), static_cast<DWORD>(v.size() * sizeof(wchar_t)));
            RegCloseKey(key);
        }
        rp::ConfigSet("fps", "max_fps_default_applied", "2");
        rp::Log("fps: Max FPS preference %g -> %.0f (default)", cur, fps);
    }

    // ---- Discord presence ----
    // detail mode: "Radiant: editing <map>" with the map's kind underneath
    discordrpc::Activity DiscordActivity()
    {
        discordrpc::Activity a;
        std::wstring path = rp::CurrentMapPath();
        if (path.empty()) { a.details = "Radiant: no map open"; return a; }
        for (wchar_t& ch : path) ch = ch == L'/' ? L'\\' : towlower(ch);
        std::wstring name = path.substr(path.find_last_of(L'\\') + 1);
        if (name.find_last_of(L'.') != std::wstring::npos) name.resize(name.find_last_of(L'.'));
        char utf8[512] = {};
        WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, utf8, sizeof utf8 - 1, nullptr, nullptr);
        a.details = std::string("Radiant: editing ") + utf8;
        if (rp::Has(&rp::host->PrefabDepth) && rp::host->PrefabDepth && rp::host->PrefabDepth(rp::self) > 0) a.state = "Inside a prefab";
        else if (path.find(L"\\_prefabs\\") != std::wstring::npos) a.state = "Prefab";
        else if (path.find(L"\\map_source\\zm\\") != std::wstring::npos) a.state = "Zombies map";
        else if (path.find(L"\\map_source\\mp\\") != std::wstring::npos) a.state = "Multiplayer map";
        else if (path.find(L"\\map_source\\cp\\") != std::wstring::npos) a.state = "Campaign map";
        return a;
    }

    int OnLoad(void)
    {
        g_timerPtr = static_cast<void**>(rp::host->RadiantAddress(rp::self, kIdleTimerPtr));
        g_maxFps = static_cast<const float*>(rp::host->RadiantAddress(rp::self, kMaxFpsPref));
        g_redrawFlags = static_cast<volatile int*>(rp::host->RadiantAddress(rp::self, kRedrawFlags));
        g_tickUpdateReturn = rp::host->RadiantAddress(rp::self, kTickUpdateReturn);
        g_frameBudget = static_cast<double*>(rp::host->RadiantAddress(rp::self, kFrameBudget));
        g_hudFps = static_cast<const float*>(rp::host->RadiantAddress(rp::self, kHudFps));
        HMODULE core = GetModuleHandleW(L"Qt5Core64r.dll");
        if (core)
        {
            g_setInterval = reinterpret_cast<SetIntervalFn>(GetProcAddress(core, "?setInterval@QTimer@@QEAAXH@Z"));
            g_interval = reinterpret_cast<IntervalFn>(GetProcAddress(core, "?interval@QTimer@@QEBAHXZ"));
        }
        if (!g_timerPtr || !g_maxFps || !g_setInterval || !g_interval)
        {
            rp::Log("not loading: this Radiant build or its Qt is not the known one (timer %p, prefs %p, Qt %p/%p)",
                    static_cast<void*>(g_timerPtr), static_cast<void*>(const_cast<float*>(g_maxFps)), static_cast<void*>(g_setInterval), static_cast<void*>(g_interval));
            return 0;
        }
        LoadConfig();
        rp::Log("loaded: uncap=%d pace=%d tick_ms=%d (0 = from Max FPS), Max FPS preference %.0f", g_uncap ? 1 : 0, g_pace ? 1 : 0, g_tickMs, MaxFps());
        br::crashguard::Setup();
        br::formats::Setup();
        br::autolod::Setup();
        br::theme::Setup();
        SetupBake();
        {
            br::bakeguide::Env env{};
            env.address = [](std::uintptr_t ida) { return rp::host->RadiantAddress(rp::self, ida); };
            env.detour = [](void* target, void* detour, void** original) { return rp::host->Detour(rp::self, target, detour, original) != 0; };
            env.log = [](const char* line) { rp::Log("%s", line); };
            env.get = [](const char* key, const char* fallback) { return rp::ConfigGet("bake", key, fallback); };
            env.set = [](const char* key, const char* value) { rp::ConfigSet("bake", key, value); };
            env.camWnd = &CamWnd;
            env.running = &BakeRunning;
            br::bakell::SetupEditor(env, &DraftWanted, &DraftBusy);
            SetupDraftRadio();
            br::bakeguide::Setup(env);
            br::bakefill::Setup(env);
            br::bakegather::Setup(env);
            br::baketrace::Setup(env, DxvkPluginDir() + L"\\trace_cache");
        }
        SetupPerfV2();
        SetupPerfV3();
        rp::Log("fps-v7.1: FULL-FIDELITY enabled=%d static_mode2=%d defer_xy_camera=%d idle_reuse=%d; no viewport/pass suppression at startup; baked lighting preserved",
                g_perfV7Enabled ? 1 : 0, g_perfV7StaticMode2 ? 1 : 0,
                g_perfV7DeferXYDuringCamera ? 1 : 0, g_perfV7IdleReuse ? 1 : 0);
        SetupDeepV5();
        SetupCensusV717();
        V8ResolveReady();
        rp::Log("fps-v8.2: persistent static index batching + cross-page R32 streaming arena enabled=%d ready=%d; state-compatible GeoCache runs only",
                g_v8StaticBatchEnabled ? 1 : 0, g_v8Ready ? 1 : 0);
        SetupLocalityV718();
        SetupPerformanceHooks();
        SetupFpsDeepV72();
        SetupSceneEarlyV712();
        SetupSpatialV714();
        SetupGridCullV716();
        br::xylod::Setup();
        br::camwalk::Setup();
        br::loadpf::Setup(DxvkPluginDir());
        br::filesearch::Setup();
        br::filtercache::Setup();
        br::fxswap::Setup();
        br::fxpreview::Setup();
        br::prefabexplore::Setup();
        br::iconsize::Setup();
        br::colorpicker::Setup();
        br::seloutline::Setup();
        br::filedialogs::Setup();
        br::dynbuffers::Setup();
        br::prefswin::Setup();
        br::rotgizmo::Setup();
        br::gamefov::Setup();
        br::campresets::Setup();
        br::actionlog::Setup();
        discordrpc::Setup("radiant", "Radiant");
        g_perfV73CompactQueues = false; // experimental, never persisted or auto-enabled
        PerfV73CompactResetCounters();
        rp::Log("fps-v7.3.3: queue compaction retired; +40 empty groups can carry visible brushes/patches");
        rp::Log("dxvk: %s", DxvkStatus().c_str());
        // no "Radiant+" menu: DXVK is in Preferences > Camera, the action log beside the Output pane's Clear button
        g_statStart = GetTickCount64();
        return 1;
    }

    void OnUnload(void)
    {
        StopProfile();
        DeepStop();
        CensusStop();
        FpsDeepStop();
        g_v718LocalityEnabled = false;
        g_v720Buckets.clear();
        g_v8StaticBatchEnabled = false;
        V8ClearCache(true);
        V81ReleaseStreamArena();
        g_sceneEarlyEnabled = false;
        g_spatialEnabled = false;
        g_gridCullEnabled = false;
        void* timer = g_timerPtr ? *g_timerPtr : nullptr;
        if (g_applied && timer && g_interval(timer) != kRadiantTickMs)
        {
            g_setInterval(timer, kRadiantTickMs);
            rp::Log("fps: idle tick restored to Radiant's %d ms", kRadiantTickMs);
        }
        g_applied = false; g_lastSet = -1;
        if (g_paceTimer) { CloseHandle(g_paceTimer); g_paceTimer = nullptr; }
        g_bakeAvail = false;             // the host removes the detours
        discordrpc::Shutdown();
        br::rotgizmo::Shutdown();
        br::campresets::Shutdown();
        br::fxpreview::Shutdown();
        br::prefabexplore::Shutdown();
        br::iconsize::Shutdown();
        br::colorpicker::Shutdown();
        br::seloutline::Shutdown();
        br::dynbuffers::Shutdown();
        br::actionlog::Shutdown();
        RemoveDraftRadio();
        br::bakeguide::Shutdown();
        br::crashguard::Shutdown();
        br::theme::Shutdown();
        V6SetTimestampBypass(false);
        V63StopWorkers(true);
        V6AssetRamClear();
        br::loadpf::Shutdown();
        br::assettouch::Flush();
        br::assettouch::Shutdown();
        PerfV3Shutdown();
    }

    // Every root map load, from the host (its Map_LoadFile hook, reloads and the command-line map, which the thunk hook above misses).
    // The asset-cache touch skip (map_skip_asset_timestamp) runs from here and stays on for 3 s after the load, then br::assettouch sets the skipped touches.
    void OnMapLoading(const wchar_t* path)
    {
        g_v6TouchTailUntil = 0;
        if (g_v6SkipLoadTimestamps) V6SetTimestampBypass(true);
        br::filesearch::LoadBegin();
        if (!g_v6LoadPassOpen)
        {
            br::filtercache::PassBegin();   // a whole load is one filter pass (AddNewEntity runs the filters outside BrushCache_Update)
            g_v6LoadPassOpen = true;
        }
        br::loadpf::MapLoadBegin(path);
    }
    void OnMapLoaded(const wchar_t*)
    {
        br::crashguard::AfterMapLoad();
        g_v6TouchTailUntil = GetTickCount64() + 3000;
        if (g_v6LoadPassOpen) { g_v6LoadPassOpen = false; br::filtercache::PassEnd(); }
        br::loadpf::MapLoadEnd();
        br::fxpreview::MapLoaded();
    }
    void OnBeforeScopeChange(void) { br::fxpreview::BeforeScopeChange(); }
    void OnScopeChanged(const wchar_t*, int depth) { br::fxpreview::ScopeChanged(depth); }

    void OnTick(void)
    {
        uint64_t now = GetTickCount64();
        DxvkApplyPending();
        br::theme::Tick();
        br::xylod::Tick();
        br::fxswap::Tick();
        br::fxpreview::Tick();
        br::prefabexplore::Tick();
        br::iconsize::Tick();
        br::dynbuffers::Tick();
        br::actionlog::Tick();
        br::loadpf::Tick();
        if (discordrpc::Due()) discordrpc::Update(DiscordActivity());
        if (g_v6TouchTailUntil && now >= g_v6TouchTailUntil && !V6MapLoading())
        {
            g_v6TouchTailUntil = 0;
            if (!g_v6TimestampPatched || V6SetTimestampBypass(false)) br::assettouch::Flush();
            br::filesearch::LoadEnd();
        }
        br::rotgizmo::Tick();
        br::campresets::Tick();
        if (g_v8StaticBatchEnabled && !g_oCensusBackendDraw) EnsureCensusBackendDrawHook();
        if (g_v8StaticBatchEnabled && !g_v8Ready) V8ResolveReady();
        if (g_profile.active && g_profileDeadlineMs && now >= g_profileDeadlineMs)
        {
            StopProfile();
            rp::Log("profile: '%s' capture complete after %.3f s; use 'profile report'",
                    g_profileLabel.empty() ? "unlabeled" : g_profileLabel.c_str(), ProfileSeconds());
        }
        uint64_t profileBegin = ProfileBegin();
        ++g_ticks;
        // Radiant's limiter asks on every tick, the pacing alone picks the frame's time
        if (g_frameBudget && PaceOn() && *g_frameBudget < 1.0) *g_frameBudget = 1.0;
        if (g_paceDirtyUntil)
        {
            if (now >= g_paceDirtyUntil) g_paceDirtyUntil = 0;
            else if (g_redrawFlags) *g_redrawFlags |= 1;
        }
        PerfV3PollInteraction();

        if (g_fpsDeepCapture)
        {
            ++g_fpsDeepTickSamples;
            if (g_fpsDeepForceCameraAPtr && g_fpsDeepForceCameraBPtr &&
                *g_fpsDeepForceCameraAPtr && *g_fpsDeepForceCameraBPtr)
                ++g_fpsDeepForcePairSamples;
            if (g_fpsDeepJqProfActivePtr && *g_fpsDeepJqProfActivePtr)
                ++g_fpsDeepJqProfActiveSamples;
            if (g_fpsDeepJqProfTriggerPtr && *g_fpsDeepJqProfTriggerPtr)
                ++g_fpsDeepJqProfTriggerSamples;
        }

        // No automatic GeoCache preallocation: native demand growth was cheaper than repeated full-size re-primes over the reset phases of a zm_giant reload.
        if (now - g_statStart >= 1000)
        {
            double secs = double(now - g_statStart) / 1000.0;
            g_ticksPerSec = g_ticks / secs; g_framesPerSec = g_frames / secs;
            g_ticks = g_frames = 0; g_statStart = now;
        }
        if (now - g_lastCheck < 200)
        {
            ProfileFinish(PM_BETTER_RADIANT_TICK, profileBegin);
            return;
        }
        g_lastCheck = now;
        // prefs are loaded once the idle timer exists
        if (!g_firstFpsDone && g_timerPtr && *g_timerPtr) { g_firstFpsDone = true; ApplyFirstMaxFps(); }
        Apply(false);
        if (g_bakeAvail) PollBake();
        ProfileFinish(PM_BETTER_RADIANT_TICK, profileBegin);
    }

    // The 3D view calls this once per presented frame: the frame counter for the fps command.
    void OnDraw(const ModToolsDrawContext* ctx)
    {
        if (ctx->view == 1) ++g_frames;
        br::rotgizmo::Draw(ctx);
        br::seloutline::Draw(ctx->view);
    }

    const char* OnCommand(const char* cmd)
    {
        static std::string reply;
        std::string c = cmd;
        if (br::crashguard::Command(c, reply)) return reply.c_str();
        if (br::formats::Command(c, reply)) return reply.c_str();
        if (br::theme::Command(c, reply)) return reply.c_str();
        PerfV2ObserveProfileCommand(c);
        char buf[512];
        if (c == "dxvk" || c == "dxvk status") { reply = DxvkStatus(); return reply.c_str(); }
        if (c == "dxvk 0" || c == "dxvk 1")    { reply = DxvkSet(c.back() == '1'); return reply.c_str(); }
        if (c == "dxvk restart")               { DxvkRestartRadiant(); reply = "restarting Radiant (File/Exit, then the same command line)"; return reply.c_str(); }
        if (br::xylod::Command(c, reply)) return reply.c_str();
        if (br::seloutline::Command(c, reply)) return reply.c_str();
        if (br::camwalk::Command(c, reply)) return reply.c_str();
        if (br::loadpf::Command(c, reply)) return reply.c_str();
        if (br::filesearch::Command(c, reply)) return reply.c_str();
        if (br::filtercache::Command(c, reply)) return reply.c_str();
        if (br::fxswap::Command(c, reply)) return reply.c_str();
        if (br::fxpreview::Command(c, reply)) return reply.c_str();
        if (br::prefabexplore::Command(c, reply)) return reply.c_str();
        if (br::iconsize::Command(c, reply)) return reply.c_str();
        if (br::colorpicker::Command(c, reply)) return reply.c_str();
        if (br::filedialogs::Command(c, reply)) return reply.c_str();
        if (br::dynbuffers::Command(c, reply)) return reply.c_str();
        if (br::prefswin::Command(c, reply)) return reply.c_str();
        if (br::rotgizmo::Command(c, reply)) return reply.c_str();
        if (br::gamefov::Command(c, reply)) return reply.c_str();
        if (br::campresets::Command(c, reply)) return reply.c_str();
        if (br::actionlog::Command(c, reply)) return reply.c_str();
        if (discordrpc::Command(c, reply)) return reply.c_str();
        if (c == "gridcull" || c == "gridcull status")
        {
            reply = GridCullStatus();
            return reply.c_str();
        }
        if (c == "gridcull 0" || c == "gridcull 1")
        {
            g_gridCullEnabled = c.back() == '1' && g_gridCullHook;
            rp::ConfigSet("performance", "grid_cull", g_gridCullEnabled ? "1" : "0");
            GridCullResetCounters();
            reply = GridCullStatus();
            return reply.c_str();
        }
        if (c == "gridcull reset")
        {
            GridCullResetCounters();
            reply = GridCullStatus();
            return reply.c_str();
        }
        if (c == "spatial" || c == "spatial status")
        {
            reply = SpatialStatus();
            return reply.c_str();
        }
        if (c == "spatial 0" || c == "spatial 1")
        {
            const bool want = c.back() == '1';
            const bool ready =
                g_spatialPrepareHook &&
                g_spatialIteratorHook &&
                g_spatialCategory1 &&
                g_sceneEarlyCullPoint &&
                g_sceneEarlyGetTransform &&
                g_sceneEarlyTransformPoint &&
                g_sceneEarlyRadiantBrushHook;

            g_spatialEnabled = want && ready;
            if (g_spatialEnabled)
                g_sceneEarlyEnabled = false; // never stack the old O(N) culler on top of BVH

            g_spatialTls = {};
            g_spatialIteratorDispatch = false;
            SpatialResetCounters();
            reply = SpatialStatus();
            return reply.c_str();
        }
        if (c == "spatial rebuild")
        {
            g_spatialNeedsRebuild.store(true, std::memory_order_release);
            g_spatialTls = {};
            reply = SpatialStatus();
            return reply.c_str();
        }
        if (c == "spatial reset")
        {
            SpatialResetCounters();
            g_spatialTls = {};
            reply = SpatialStatus();
            return reply.c_str();
        }

        if (c == "sceneearly" || c == "sceneearly status")
        {
            reply = SceneEarlyStatus();
            return reply.c_str();
        }
        if (c == "sceneearly 0" || c == "sceneearly 1")
        {
            const bool want = c.back() == '1';
            const bool ready =
                g_sceneEarlyCullPoint && g_sceneEarlyGetTransform && g_sceneEarlyTransformPoint &&
                g_sceneEarlyRadiantBrushHook && g_sceneEarlyBrushInstHook;
            g_sceneEarlyEnabled = want && ready;
            if (g_sceneEarlyEnabled)
                g_spatialEnabled = false;
            SceneEarlyResetCounters();
            reply = SceneEarlyStatus();
            return reply.c_str();
        }
        if (c == "sceneearly reset")
        {
            SceneEarlyResetCounters();
            reply = SceneEarlyStatus();
            return reply.c_str();
        }

        if (c == "render7 compact" || c == "render7 compact status")
        {
            reply = PerfV73CompactStatus();
            return reply.c_str();
        }
        if (c == "render7 compact 0" || c == "render7 compact 1")
        {
            g_perfV73CompactQueues = false;
            PerfV73CompactResetCounters();
            reply = PerfV73CompactStatus();
            return reply.c_str();
        }
        if (c == "render7 compact reset")
        {
            PerfV73CompactResetCounters();
            reply = PerfV73CompactStatus();
            return reply.c_str();
        }

        if (c == "staticbatch" || c == "staticbatch status" || c == "batch8" || c == "batch8 status")
        {
            V8ResolveReady();
            reply = V8Status();
            return reply.c_str();
        }
        if (c == "staticbatch 0" || c == "batch8 0")
        {
            g_v8StaticBatchEnabled = false;
            g_v8PendingCount = 0;
            reply = V8Status();
            return reply.c_str();
        }
        if (c == "staticbatch 1" || c == "batch8 1")
        {
            g_v8CreateDisabled = false;
            g_v81StreamCreateDisabled = false;
            g_v8StaticBatchEnabled = EnsureCensusBackendDrawHook() && V8ResolveReady();
            reply = V8Status();
            return reply.c_str();
        }
        if (c == "staticbatch reset" || c == "batch8 reset")
        {
            V8ClearCache(true);
            V81ReleaseStreamArena();
            g_v8CreateDisabled = false;
            g_v81StreamCreateDisabled = false;
            reply = V8Status();
            return reply.c_str();
        }

        if (c == "drawlocality" || c == "drawlocality status" ||
            c == "geolocality" || c == "geolocality status")
        {
            reply = V718Status();
            return reply.c_str();
        }
        if (c == "drawlocality 0" || c == "drawlocality 1" ||
            c == "geolocality 0" || c == "geolocality 1")
        {
            const bool want = c.back() == '1';
            const bool ready = g_oDeepRenderPass && g_oDeepGeoSubmit && g_v718GeoHashMask &&
                               g_v718GeoHashThreshold && g_v718GeoHashBuckets && g_v718GeoHashSentinel;
            g_v718LocalityEnabled = want && ready;
            V718Reset(true);
            reply = V718Status();
            return reply.c_str();
        }
        if (c == "drawlocality reset" || c == "geolocality reset")
        {
            V718Reset(true);
            reply = V718Status();
            return reply.c_str();
        }

        if (c == "coldpack" || c == "coldpack status")
        {
            reply = V723Status();
            return reply.c_str();
        }
        if (c == "coldpack 0")
        {
            g_v723ColdPackEnabled = false;
            g_v723ColdPackArmed = false;
            reply = V723Status();
            return reply.c_str();
        }
        if (c == "coldpack 1" || c == "coldpack arm")
        {
            g_v723ColdPackEnabled = false;
            g_v723ColdPackArmed = false;
            reply = "coldpack-v7.23 RETIRED in V7.24: unstable mesh-hash identity produced ranked=0 and severe GeoCache fragmentation; use 'RadiantPlus geomkey learn oit' instead";
            return reply.c_str();
        }
        if (c == "coldpack learn oit" || c == "coldpack learn gbuffer" ||
            c == "coldpack learn prepass" || c == "coldpack learn emissive")
        {
            if (c.find("gbuffer") != std::string::npos) g_v723Target = V723Target::GBufferOpaque;
            else if (c.find("prepass") != std::string::npos) g_v723Target = V723Target::Prepass;
            else if (c.find("emissive") != std::string::npos) g_v723Target = V723Target::EmissiveOpaque;
            else g_v723Target = V723Target::OIT;
            g_v723ColdPackEnabled = false;
            g_v723ColdPackArmed = false;
            V723ResetRuntimeCounters(false);
            g_v723LearnEnabled = true;
            reply = V723Status();
            return reply.c_str();
        }
        if (c == "coldpack learn stop")
        {
            g_v723LearnEnabled = false;
            g_v723LearnReady = !g_v723LearnRank.empty();
            reply = V723Status();
            return reply.c_str();
        }
        if (c == "coldpack reset")
        {
            V723ResetRuntimeCounters(true);
            if (g_v723ColdPackEnabled) V723ArmForColdLoad();
            reply = V723Status();
            return reply.c_str();
        }
        if (c == "coldpack clear")
        {
            g_v723ColdPackEnabled = false;
            g_v723ColdPackArmed = false;
            V723ResetRuntimeCounters(false);
            reply = V723Status();
            return reply.c_str();
        }

        if (c == "geomkey" || c == "geomkey status")
        {
            reply = V724Status();
            return reply.c_str();
        }
        if (c == "geomkey learn oit")
        {
            V724Reset(false);
            g_v724LearnEnabled = true;
            reply = V724Status();
            return reply.c_str();
        }
        if (c == "geomkey learn stop")
        {
            g_v724LearnEnabled = false;
            g_v724LearnReady = !g_v724LearnRank.empty();
            reply = V724Status();
            return reply.c_str();
        }
        if (c == "geomkey probe 1" || c == "geomkey probe")
        {
            V724Reset(true);
            g_v724ProbeEnabled = g_v724LearnReady && !g_v724LearnRank.empty();
            reply = V724Status();
            return reply.c_str();
        }
        if (c == "geomkey probe 0")
        {
            g_v724ProbeEnabled = false;
            reply = V724Status();
            return reply.c_str();
        }
        if (c == "geomkey reset")
        {
            V724Reset(false);
            reply = V724Status();
            return reply.c_str();
        }

        if (c == "census start" || c == "submitcensus start")
        {
            const bool backend = EnsureCensusBackendDrawHook();
            CensusReset(true);
            reply = backend
                ? "submission census v7.24 started; flush-cause backend hook active; run one stable camera view, then 'census stop'"
                : "submission census v7.24 started WITHOUT backend flush-cause hook; timing/counter census still active";
            return reply.c_str();
        }
        if (c == "census stop" || c == "submitcensus stop")
        {
            CensusStop();
            reply = CensusReport();
            return reply.c_str();
        }
        if (c == "census reset" || c == "submitcensus reset")
        {
            const bool restart = g_censusCapture;
            CensusReset(restart);
            reply = restart ? "submission census v7.24 reset and restarted" : "submission census v7.24 data reset";
            return reply.c_str();
        }
        if (c == "census" || c == "census status" || c == "census report" ||
            c == "submitcensus" || c == "submitcensus status" || c == "submitcensus report")
        {
            reply = CensusReport();
            return reply.c_str();
        }

        if (c == "fpsdeep start")
        {
            FpsDeepResetData(true);
            reply = "fpsdeep-v7.15 started; instrumentation only, full fidelity";
            return reply.c_str();
        }
        if (c == "fpsdeep stop")
        {
            FpsDeepStop();
            reply = FpsDeepReport();
            return reply.c_str();
        }
        if (c == "fpsdeep reset")
        {
            const bool restart = g_fpsDeepCapture;
            FpsDeepResetData(restart);
            reply = restart ? "fpsdeep-v7.15 reset and restarted" : "fpsdeep-v7.15 data reset";
            return reply.c_str();
        }
        if (c == "fpsdeep" || c == "fpsdeep status" || c == "fpsdeep report")
        {
            reply = FpsDeepReport();
            return reply.c_str();
        }
        if (c == "profile start" || c.rfind("profile start ", 0) == 0)
        {
            g_profileLabel = c.size() > 13 ? c.substr(13) : "";
            while (!g_profileLabel.empty() && g_profileLabel.front() == ' ') g_profileLabel.erase(g_profileLabel.begin());
            StartProfile();
            snprintf(buf, sizeof buf, "profile started: window=%.2f s label=%s", g_profileWindowSeconds,
                     g_profileLabel.empty() ? "(none)" : g_profileLabel.c_str());
            reply = buf; return reply.c_str();
        }
        if (c == "profile stop")
        {
            bool wasRunning = g_profile.active;
            StopProfile();
            snprintf(buf, sizeof buf, wasRunning ? "profile stopped at %.3f s; use 'profile report'" : "profile was not running (elapsed %.3f s)", ProfileSeconds());
            reply = buf; return reply.c_str();
        }
        if (c == "profile reset")
        {
            bool restart = g_profile.active;
            if (restart) StartProfile();
            else
            {
                ClearLocalProfile(false);
                if (HostProfileAvailable()) rp::host->PerformanceProfile(rp::self, MODTOOLS_HOST_PERF_RESET, nullptr);
            }
            reply = restart ? "profile reset and restarted" : "profile data reset";
            return reply.c_str();
        }
        if (c == "profile status")
        {
            reply = ProfileStatus(); return reply.c_str();
        }
        if (c == "profile report")
        {
            reply = ProfileReport(); return reply.c_str();
        }
        if (c.rfind("profile window ", 0) == 0)
        {
            double value = atof(c.substr(15).c_str());
            if (value < 0.5) value = 0.5;
            if (value > 3600.0) value = 3600.0;
            g_profileWindowSeconds = value;
            snprintf(buf, sizeof buf, "%.3f", value);
            rp::ConfigSet("profile", "window_seconds", buf);
            snprintf(buf, sizeof buf, "profile window=%.3f s (applies to the next start/reset)", value);
            reply = buf; return reply.c_str();
        }
        if (c == "deep start")
        {
            DeepReset(true);
            reply = "deep-v7.1 profiler started; real lighting preserved";
            return reply.c_str();
        }
        if (c == "deep stop")
        {
            DeepStop();
            reply = DeepReport();
            return reply.c_str();
        }
        if (c == "deep reset")
        {
            DeepReset(false);
            reply = "deep-v7.2 profiler reset";
            return reply.c_str();
        }
        if (c == "deep" || c == "deep status" || c == "deep report")
        {
            reply = DeepReport();
            return reply.c_str();
        }
        if (c == "opt" || c == "opt status")
        {
            reply = V6Status();
            return reply.c_str();
        }
        if (c == "assetram clear")
        {
            V6AssetRamClear();
            reply = V6Status();
            return reply.c_str();
        }
        if (c == "assetram 0" || c == "assetram 1")
        {
            g_v6AssetRamEnabled = c.back() == '1';
            rp::ConfigSet("performance", "asset_ram_cache", g_v6AssetRamEnabled ? "1" : "0");
            if (!g_v6AssetRamEnabled) V6AssetRamClear();
            reply = V6Status();
            return reply.c_str();
        }
        if (c.rfind("assetram mb ", 0) == 0)
        {
            int mb = atoi(c.substr(12).c_str());
            if (mb < 0) mb = 0;
            if (mb > 8192) mb = 8192;
            g_v6AssetRamMB = mb;
            g_v61ResolvedAssetRamMB = 0;
            rp::ConfigSet("performance", "asset_ram_cache_mb", std::to_string(mb));
            if (mb == 0) V6AssetRamClear();
            reply = V6Status();
            return reply.c_str();
        }
        if (c == "loadrender 0" || c == "loadrender 1")
        {
            g_v6SuppressLoadRendering = c.back() == '1';
            rp::ConfigSet("performance", "map_suppress_rendering", g_v6SuppressLoadRendering ? "1" : "0");
            reply = V6Status();
            return reply.c_str();
        }
        if (c == "timestamp 0" || c == "timestamp 1")
        {
            g_v6SkipLoadTimestamps = c.back() == '1';
            rp::ConfigSet("performance", "map_skip_asset_timestamp", g_v6SkipLoadTimestamps ? "1" : "0");
            if (!g_v6SkipLoadTimestamps && !V6MapLoading()) V6SetTimestampBypass(false);
            reply = V6Status();
            return reply.c_str();
        }
        if (c == "geoprime 0" || c == "geoprime 1")
        {
            g_v6GeoPreallocate = c.back() == '1';
            rp::ConfigSet("performance", "geo_preallocate", g_v6GeoPreallocate ? "1" : "0");
            if (g_v6GeoPreallocate) V6PrimeGeoCache("command");
            reply = V6Status();
            return reply.c_str();
        }
        if (c == "prefetch" || c == "prefetch status")
        {
            reply = V63Status();
            return reply.c_str();
        }
        if (c == "prefetch clear")
        {
            V63ClearLearned();
            reply = V63Status();
            return reply.c_str();
        }
        if (c == "prefetch 0" || c == "prefetch 1")
        {
            g_v63PrefetchEnabled = c.back() == '1';
            rp::ConfigSet("performance", "asset_prefetch", g_v63PrefetchEnabled ? "1" : "0");
            if (!g_v63PrefetchEnabled) V63StopWorkers(true);
            reply = V63Status();
            return reply.c_str();
        }
        if (c.rfind("prefetch workers ", 0) == 0)
        {
            int n = atoi(c.substr(17).c_str());
            if (n < 0) n = 0;
            if (n > 8) n = 8;
            g_v63PrefetchWorkers = n;
            rp::ConfigSet("performance", "asset_prefetch_workers", std::to_string(n));
            reply = V63Status();
            return reply.c_str();
        }
        if (c.rfind("prefetch mb ", 0) == 0)
        {
            int mb = atoi(c.substr(12).c_str());
            if (mb < 16) mb = 16;
            if (mb > 1024) mb = 1024;
            g_v63PrefetchMB = mb;
            rp::ConfigSet("performance", "asset_prefetch_mb", std::to_string(mb));
            reply = V63Status();
            return reply.c_str();
        }
        if (c.rfind("prefetch window ", 0) == 0)
        {
            int n = atoi(c.substr(16).c_str());
            if (n < 4) n = 4;
            if (n > 256) n = 256;
            g_v632Lookahead = size_t(n);
            reply = V63Status();
            return reply.c_str();
        }
        if (c == "fps" || c == "fps status")
        {
            void* timer = g_timerPtr ? *g_timerPtr : nullptr;
            snprintf(buf, sizeof buf, "uncap=%d pace=%d tick_ms(setting)=%d interval=%d desired=%d max_fps_pref=%g ticks/s=%.1f cam_frames/s=%.1f hud_fps=%.1f paced_holds=%llu radiant_resets=%d",
                     g_uncap ? 1 : 0, g_pace ? 1 : 0, g_tickMs, timer ? g_interval(timer) : -1, DesiredTickMs(), MaxFps(), g_ticksPerSec, g_framesPerSec,
                     g_hudFps ? *g_hudFps : 0.0f, (unsigned long long)g_paceHeld, g_resets);
            reply = buf; return reply.c_str();
        }
        if (c == "pace 0" || c == "pace 1")
        {
            g_pace = c.back() == '1';
            rp::ConfigSet("fps", "pace", g_pace ? "1" : "0");
            Apply(true);
            reply = g_pace ? "pace on (every view redraw held to Max FPS)" : "pace off (Radiant's own limiter only)"; return reply.c_str();
        }
        if (c.rfind("fps dirty ", 0) == 0)
        {
            int secs = atoi(c.c_str() + 10);
            if (secs < 0) secs = 0;
            if (secs > 60) secs = 60;
            g_paceDirtyUntil = secs ? GetTickCount64() + uint64_t(secs) * 1000 : 0;
            snprintf(buf, sizeof buf, "3D view marked dirty every tick for %d s", secs);
            reply = buf; return reply.c_str();
        }
        if (c == "xydrag" || c == "xydrag status")
        {
            snprintf(buf, sizeof buf, "xy_drag_hz=%d (0 = off) share=%d%% focus_hz=%d last_2d_frame=%.1f ms pending=0x%x throttled_requests=%llu deferred_draws=%llu",
                     g_xyDragHz, g_xyDragShare, g_xyFocusHz, g_xyDragLast2DMs, g_xyDragPending, (unsigned long long)g_xyDragThrottled, (unsigned long long)g_xyDragDeferredDraws);
            reply = buf; return reply.c_str();
        }
        if (c.rfind("xydrag ", 0) == 0)
        {
            int hz = atoi(c.c_str() + 7);
            if (hz < 0) hz = 0;
            if (hz > 120) hz = 120;
            g_xyDragHz = hz;
            g_xyDragPending = 0;
            rp::ConfigSet("performance", "xy_drag_hz", std::to_string(hz));
            snprintf(buf, sizeof buf, "xy_drag_hz=%d%s", hz, hz ? "" : " (2D views redraw on every drag step, Radiant's own behaviour)");
            reply = buf; return reply.c_str();
        }
        if (c == "uncap 0" || c == "uncap 1")
        {
            g_uncap = c.back() == '1';
            rp::ConfigSet("fps", "uncap", g_uncap ? "1" : "0");
            Apply(true);
            reply = g_uncap ? "uncap on" : "uncap off (Radiant's 17 ms tick)"; return reply.c_str();
        }
        if (c.rfind("tick ", 0) == 0)
        {
            std::string v = c.substr(5);
            g_tickMs = v == "auto" ? 0 : atoi(v.c_str());
            if (g_tickMs < 0) g_tickMs = 0;
            if (g_tickMs > 1000) g_tickMs = 1000;
            rp::ConfigSet("fps", "tick_ms", std::to_string(g_tickMs));
            Apply(true);
            snprintf(buf, sizeof buf, "tick_ms=%d (desired interval %d ms)", g_tickMs, DesiredTickMs());
            reply = buf; return reply.c_str();
        }
        if (c == "bake guide verify")
        {
            try
            {
                auto guide = std::make_unique<br::guiding::Guide>();
                double integral = 0;
                for (unsigned i = 0; i < 4096; ++i)
                {
                    auto sample = guide->Draw(br::guiding::Sequence(i, 4096, 1));
                    integral += std::max(0.0, sample.direction.z) / sample.pdf / 4096;
                }
                if (std::abs(integral - br::guiding::Pi) > 0.01)
                    reply = "Ray guiding kernel verification failed: constant-light integral.";
                else
                    reply = "Ray guiding kernel verification passed. This checks the CPU math only; BO3 bake sampling is unchanged.";
            }
            catch (const std::exception& error) { reply = std::string("Ray guiding kernel verification failed: ") + error.what(); }
            catch (...) { reply = "Ray guiding kernel verification failed."; }
            return reply.c_str();
        }
        if ((c.rfind("bake guide", 0) == 0 || c.rfind("bake cone", 0) == 0 || c.rfind("bake adaptive", 0) == 0) &&
            br::bakeguide::Command(c, reply)) return reply.c_str();
        if ((c.rfind("bake fillcache", 0) == 0 || c.rfind("bake fillfast", 0) == 0) && br::bakefill::Command(c, reply)) return reply.c_str();
        if (c.rfind("bake gatherfast", 0) == 0 && br::bakegather::Command(c, reply)) return reply.c_str();
        if (c.rfind("bake tracecache", 0) == 0 && br::baketrace::Command(c, reply)) return reply.c_str();
        if (c == "bake quality" || c.rfind("bake quality ", 0) == 0)
        {
            // The L / M / H radio buttons beside the Build Lighting bolt pick the bake quality (256 / 1024 / 4096 rays per voxel).
            reply = BakeQualityRadio(c.size() > 13 ? c.substr(13) : "");
            return reply.c_str();
        }
        if (c.rfind("bake start", 0) == 0)
        {
            std::string q = c.size() > 11 ? c.substr(11) : "";
            if (!q.empty())
            {
                std::string r = BakeQualityRadio(q);
                if (r.rfind("selected", 0) != 0) { reply = r; return reply.c_str(); }
            }
            std::string found;
            bool ok = ClickButtonByToolTip("Build lighting", found);
            snprintf(buf, sizeof buf, ok ? "triggered the Build Lighting button%s%s" : "the Build Lighting action was not found", q.empty() ? "" : " with quality ", q.c_str());
            if (ok) rp::Log("bake: triggered the Build Lighting button (%s)", q.empty() ? "current quality" : q.c_str());
            reply = buf; return reply.c_str();
        }
        if (c == "bake cancel rebuild")
        {
            // Cancel guard test path: Radiant's cancel, then a build in the same tick (a fast double click).
            std::string found;
            if (!ClickButtonByToolTip("Cancel lighting", found)) { reply = "no bake to cancel"; return reply.c_str(); }
            rp::Log("bake: triggered the Cancel lighting button, rebuilding at once");
            if (g_mainWinPtr && *g_mainWinPtr && g_oToolbarBuild) hk_ToolbarBuild(*g_mainWinPtr, 0, 0);
            snprintf(buf, sizeof buf, "cancelled and asked for a rebuild: %d job(s) in flight, %zu held", InflightJobs(), g_held.size());
            reply = buf; return reply.c_str();
        }
        if (c == "bake cancel")
        {
            // While a bake runs the bolt reads "Cancel lighting", clicking it is Radiant's own cancel (cancel guard test path).
            std::string found;
            const bool ok = ClickButtonByToolTip("Cancel lighting", found);
            reply = ok ? "triggered the Cancel lighting button" : "no bake to cancel (the Cancel lighting button was not found)";
            if (ok) rp::Log("bake: triggered the Cancel lighting button");
            return reply.c_str();
        }
        if (c == "bake widgets")
        {
            reply.clear();
            ForEachWidget([&](void*, const char* cls, const std::string& t) { if (t.find("ighting") != std::string::npos || t.find("Bake") != std::string::npos) reply += std::string(cls) + ": " + t + "\n"; return false; });
            if (reply.empty()) reply = "(none)";
            return reply.c_str();
        }
        if (c == "bake debug text")
        {
            const auto widgets = GetModuleHandleW(L"Qt5Widgets64r.dll");
            const auto core = GetModuleHandleW(L"Qt5Core64r.dll");
            auto destroy = reinterpret_cast<QStringDtorFn>(GetProcAddress(core, "??1QString@@QEAA@XZ"));
            auto visible = reinterpret_cast<bool (*)(const void*)>(GetProcAddress(widgets, "?isVisible@QWidget@@QEBA_NXZ"));
            reply.clear();
            ForEachWidget([&](void* widget, const char* cls, const std::string&) {
                if (!destroy || !visible || !visible(widget)) return false;
                const char* symbol = !strcmp(cls, "QPlainTextEdit") ? "?toPlainText@QPlainTextEdit@@QEBA?AVQString@@XZ" :
                                     !strcmp(cls, "QTextEdit") ? "?toPlainText@QTextEdit@@QEBA?AVQString@@XZ" :
                                     !strcmp(cls, "QLabel") ? "?text@QLabel@@QEBA?AVQString@@XZ" : nullptr;
                auto get = symbol ? reinterpret_cast<ActionTextFn>(GetProcAddress(widgets, symbol)) : nullptr;
                if (get) { void* value = nullptr; get(widget, &value); std::string text = QStringToUtf8(&value); destroy(&value);
                    if (text.size() > 12000) text.erase(0, text.size() - 12000);
                    if (!text.empty()) reply += std::string(cls) + ": " + text + "\n"; }
                return reply.size() > 60000;
            });
            return reply.c_str();
        }
        if (c.rfind("bake debug action ", 0) == 0)
        {
            if (BakeRunning()) { reply = "a bake is running"; return reply.c_str(); }
            const std::string wanted = c.substr(18);
            void* hit = nullptr;
            ForEachObjectAction([&](void* action, const std::string& text) { if (StripMnemonic(text) != wanted) return false; hit = action; return true; });
            auto activate = reinterpret_cast<ActivateFn>(GetProcAddress(GetModuleHandleW(L"Qt5Widgets64r.dll"), "?activate@QAction@@QEAAXW4ActionEvent@1@@Z"));
            if (hit && activate) activate(hit, 0);
            reply = hit && activate ? "triggered " + wanted : "action not found";
            return reply.c_str();
        }
        if (c.rfind("bake debug actions", 0) == 0)
        {
            const std::string filter = c.size() > 19 ? c.substr(19) : "";
            auto widgets = GetModuleHandleW(L"Qt5Widgets64r.dll");
            auto checked = reinterpret_cast<bool (*)(const void*)>(GetProcAddress(widgets, "?isChecked@QAction@@QEBA_NXZ"));
            auto enabled = reinterpret_cast<bool (*)(const void*)>(GetProcAddress(widgets, "?isEnabled@QAction@@QEBA_NXZ"));
            reply.clear();
            ForEachObjectAction([&](void* action, const std::string& text) {
                if (filter.empty() || text.find(filter) != std::string::npos)
                    reply += text + " checked=" + (checked && checked(action) ? "1" : "0") + " enabled=" + (enabled && enabled(action) ? "1" : "0") + "\n";
                return false;
            });
            return reply.c_str();
        }
        if (c == "bake actions")
        {
            reply.clear();
            ForEachAction([&](void*, const std::string& t) { if (t.find("Bake") != std::string::npos || t.find("Light") != std::string::npos || t.find("lighting") != std::string::npos) reply += t + "\n"; return false; });
            if (reply.empty()) reply = "(none)";
            return reply.c_str();
        }
        if (c == "bake" || c == "bake status") { reply = BakeStatus() + "\n" + br::bakeguide::Summary() + "\n" + br::bakefill::Status() + "\n" + br::bakegather::Status() + "\n" + br::baketrace::Status() + "\n" + br::bakell::Status(); return reply.c_str(); }
        if (c == "bake guard 0" || c == "bake guard 1")
        {
            if (!g_oToolbarBuild) { reply = "cancel guard unavailable (detours failed)"; return reply.c_str(); }
            g_cancelGuard = c.back() == '1';
            if (!g_cancelGuard) ReleaseHeld();
            rp::ConfigSet("bake", "cancel_guard", g_cancelGuard ? "1" : "0");
            reply = g_cancelGuard ? "cancel guard on" : "cancel guard off"; return reply.c_str();
        }
        if (c == "bake boost 0" || c == "bake boost 1")
        {
            g_boost = c.back() == '1';
            rp::ConfigSet("bake", "boost", g_boost ? "1" : "0");
            reply = g_boost ? "bake boost on" : "bake boost off"; return reply.c_str();
        }
        if (c.rfind("bake budget ", 0) == 0)
        {
            g_boostBudgetMs = atoi(c.substr(12).c_str()); if (g_boostBudgetMs < 0) g_boostBudgetMs = 0; if (g_boostBudgetMs > 200) g_boostBudgetMs = 200;
            rp::ConfigSet("bake", "budget_ms", std::to_string(g_boostBudgetMs));
            snprintf(buf, sizeof buf, "bake budget_ms=%d", g_boostBudgetMs); reply = buf; return reply.c_str();
        }
        if (c.rfind("bake redraw ", 0) == 0)
        {
            g_boostRedrawMs = atoi(c.substr(12).c_str()); if (g_boostRedrawMs < 0) g_boostRedrawMs = 0; if (g_boostRedrawMs > 10000) g_boostRedrawMs = 10000;
            rp::ConfigSet("bake", "redraw_ms", std::to_string(g_boostRedrawMs));
            snprintf(buf, sizeof buf, "bake redraw_ms=%d", g_boostRedrawMs); reply = buf; return reply.c_str();
        }
        if (c == "pipeline" || c == "pipeline report")
        {
            reply = PerfV2Report();
            return reply.c_str();
        }
        if (c == "pipeline start")
        {
            PerfV2Reset(true);
            reply = "pipeline profiler started";
            return reply.c_str();
        }
        if (c == "pipeline stop")
        {
            PerfV2Stop();
            reply = PerfV2Report();
            return reply.c_str();
        }
        if (c == "pipeline reset")
        {
            PerfV2Reset(false);
            reply = "pipeline profiler reset";
            return reply.c_str();
        }
        if (c == "gpu" || c == "gpu status")
        {
            snprintf(buf, sizeof buf,
                     "gpu_stats_cache=%d interval_ms=%d hook=%d calls=%llu cached=%llu original=%llu",
                     g_gpuStatsCache ? 1 : 0, g_gpuStatsMs, g_oGpuStats ? 1 : 0,
                     (unsigned long long)g_gpuStatsCalls, (unsigned long long)g_gpuStatsBypassed,
                     (unsigned long long)g_gpuStatsOriginal);
            reply = buf;
            return reply.c_str();
        }
        if (c == "gpu cache 0" || c == "gpu cache 1")
        {
            g_gpuStatsCache = c.back() == '1';
            rp::ConfigSet("performance", "gpu_stats_cache", g_gpuStatsCache ? "1" : "0");
            reply = g_gpuStatsCache ? "gpu stats cache on" : "gpu stats cache off";
            return reply.c_str();
        }
        if (c.rfind("gpu cachems ", 0) == 0)
        {
            g_gpuStatsMs = atoi(c.substr(12).c_str());
            if (g_gpuStatsMs < 1000) g_gpuStatsMs = 1000;
            if (g_gpuStatsMs > 10000) g_gpuStatsMs = 10000;
            rp::ConfigSet("performance", "gpu_stats_ms", std::to_string(g_gpuStatsMs));
            snprintf(buf, sizeof buf, "gpu_stats_ms=%d", g_gpuStatsMs);
            reply = buf;
            return reply.c_str();
        }
        if (c == "fast" || c == "fast status")
        {
            PerfV3PollInteraction();
            reply = PerfV3Status();
            return reply.c_str();
        }
        if (c == "fast 0" || c == "fast 1")
        {
            g_perfV3Enabled = c.back() == '1';
            if (!g_perfV3Enabled) { g_perfV3Force = false; g_perfV3FastUntil = 0; }
            rp::ConfigSet("performance", "interactive_fast_mode", g_perfV3Enabled ? "1" : "0");
            PerfV3PollInteraction();
            reply = PerfV3Status();
            return reply.c_str();
        }
        if (c == "fast force 0" || c == "fast force 1")
        {
            g_perfV3Force = c.back() == '1';
            if (g_perfV3Force) g_perfV3Enabled = true;
            PerfV3PollInteraction();
            reply = PerfV3Status();
            return reply.c_str();
        }
        if (c == "fast mode2 0" || c == "fast mode2 1")
        {
            g_perfV3SkipMode2 = c.back() == '1';
            rp::ConfigSet("performance", "interactive_skip_mode2", g_perfV3SkipMode2 ? "1" : "0");
            reply = PerfV3Status();
            return reply.c_str();
        }
        if (c == "fast xy 0" || c == "fast xy 1")
        {
            // UpdateXYOnDrag is never suppressed.
            g_perfV3SuppressXY = false;
            rp::ConfigSet("performance", "interactive_suppress_xy_drag", "0");
            PerfV3RestoreXY(true);
            reply = PerfV3Status();
            return reply.c_str();
        }
        if (c.rfind("fast settle ", 0) == 0)
        {
            g_perfV3SettleMs = atoi(c.substr(12).c_str());
            if (g_perfV3SettleMs < 0) g_perfV3SettleMs = 0;
            if (g_perfV3SettleMs > 2000) g_perfV3SettleMs = 2000;
            rp::ConfigSet("performance", "interactive_settle_ms", std::to_string(g_perfV3SettleMs));
            reply = PerfV3Status();
            return reply.c_str();
        }
        if (c == "fast reset")
        {
            PerfV3ResetCounters();
            PerfV7ResetCounters();
            reply = PerfV3Status() + " | " + PerfV7Status();
            return reply.c_str();
        }
        if (c == "render7" || c == "render7 status")
        {
            reply = PerfV7Status();
            return reply.c_str();
        }
        if (c == "render7 0" || c == "render7 1")
        {
            g_perfV7Enabled = c.back() == '1';
            rp::ConfigSet("performance", "render7_enabled", g_perfV7Enabled ? "1" : "0");
            PerfV7ForceFreshViews();
            reply = PerfV7Status();
            return reply.c_str();
        }
        if (c == "render7 mode2 0" || c == "render7 mode2 1")
        {
            g_perfV7StaticMode2 = c.back() == '1';
            rp::ConfigSet("performance", "render7_static_mode2", g_perfV7StaticMode2 ? "1" : "0");
            PerfV7ForceFreshViews();
            reply = PerfV7Status();
            return reply.c_str();
        }
        if (c == "render7 xy 0" || c == "render7 xy 1")
        {
            // Compatibility only: XY/XZ/YZ render calls are never dropped.
            g_perfV7DeferXYDuringCamera = false;
            rp::ConfigSet("performance", "render7_defer_xy_camera", "0");
            PerfV7ForceFreshViews();
            reply = PerfV7Status();
            return reply.c_str();
        }
        if (c == "render7 reuse 0" || c == "render7 reuse 1")
        {
            g_perfV7IdleReuse = c.back() == '1';
            rp::ConfigSet("performance", "render7_idle_reuse", g_perfV7IdleReuse ? "1" : "0");
            PerfV7ForceFreshViews();
            reply = PerfV7Status();
            return reply.c_str();
        }
        if (c.rfind("render7 idle ", 0) == 0)
        {
            int ms = atoi(c.substr(13).c_str());
            if (ms < 50) ms = 50;
            if (ms > 5000) ms = 5000;
            g_perfV7IdleAfterMs = ms;
            rp::ConfigSet("performance", "render7_idle_after_ms", std::to_string(ms));
            reply = PerfV7Status();
            return reply.c_str();
        }
        if (c.rfind("render7 refresh ", 0) == 0)
        {
            int ms = atoi(c.substr(16).c_str());
            if (ms < 50) ms = 50;
            if (ms > 5000) ms = 5000;
            g_perfV7IdleRefreshMs = ms;
            rp::ConfigSet("performance", "render7_idle_refresh_ms", std::to_string(ms));
            reply = PerfV7Status();
            return reply.c_str();
        }
        if (c == "render7 reset")
        {
            PerfV7ResetCounters();
            PerfV7ForceFreshViews();
            reply = PerfV7Status();
            return reply.c_str();
        }
        if (c == "turbo" || c == "turbo status" || c == "turbo 0" || c == "turbo 1")
        {
            g_perfV4TurboLighting = false;
            reply = "turbo lighting shortcut retired in V5; real/baked lighting is always preserved";
            return reply.c_str();
        }
        if (c.rfind("turbo mode ", 0) == 0)
        {
            g_perfV4TurboLighting = false;
            reply = "turbo mode is retired in V5; camera mode remains unchanged";
            return reply.c_str();
        }
        if (c == "turbo reset")
        {
            PerfV4ResetCounters();
            reply = PerfV4Status();
            return reply.c_str();
        }
        return nullptr;
    }
}

namespace br::dxvk
{
bool Setting() { return DxvkInstalledInBin(); }
void Request(bool on) { g_dxvkPending = on ? 1 : 0; }
}

extern "C" MODTOOLS_PLUGIN_EXPORT const ModToolsPluginApi* ModToolsPluginEntry(ModToolsPlugin* self, const ModToolsHostApi* host)
{
    rp::Bind(self, host);
    static ModToolsPluginApi api = {};
    api.size = sizeof api;
    api.apiVersion = MODTOOLS_PLUGIN_API_VERSION;
    api.name = "RadiantPlus";
    api.displayName = "Radiant+";
    api.version = "1.1.1";
    api.author = "Figglebottom";
    api.description = "QOL fixes and improvements to Radiant.";
    api.url = "https://github.com/Aidan601/MTPlugins/tree/main/plugins/RadiantPlus";
    api.OnLoad = OnLoad;
    api.OnUnload = OnUnload;
    api.OnTick = OnTick;
    api.OnMapLoading = OnMapLoading;
    api.OnMapLoaded = OnMapLoaded;
    api.OnBeforeScopeChange = OnBeforeScopeChange;
    api.OnScopeChanged = OnScopeChanged;
    api.OnDraw = OnDraw;
    api.OnCommand = OnCommand;
    api.apps = MODTOOLS_APP_RADIANT;
    return &api;
}
