// Game view field of view. Cam_GetFov 0x1402316D0 gives the Fov preference outside game view and a fixed 80.67 inside it (90 in probe view).
// A detour swaps that constant for the FOV set here. Cam_Draw's view setup 0x140231770 calls the lens builder 0x140FAEFE0
// (tan(vfov/2) at view+0x30, tan(hfov/2) at +0x34), so a second detour rescales those for the stock lens.
#include "modtools_plugin.hpp"
#include "gamefov.h"
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace br::gamefov
{
namespace
{
    // IDA addresses (no ASLR), resolved through host->RadiantAddress
    const uintptr_t kCamGetFov = 0x1402316D0;   // float Cam_GetFov(): the 3D view's FOV as the horizontal FOV of a 16:9 view
    const uintptr_t kGameView  = 0x1453A1EE9;   // char: 1 = game view
    const uintptr_t kFovPref   = 0x1453A1D88;   // float: Preferences > Camera > Field of View (non GameView)
    const uintptr_t kLensView  = 0x140FAEFE0;   // view* (cam, view* out, float focal, float focusDist): game view projection
    const float kLensFocal = 14.64f, kLensFocus = 12700.0f;   // the game view's lens (0x1422B40DC, 0x1422B4150)
    const float kRadToDeg = 57.295776f;         // Radiant's own constant (dword_14228F67C)

    using GetFov_t = float(__fastcall*)();
    GetFov_t g_orig = nullptr;
    using LensView_t = uint8_t*(__fastcall*)(void*, uint8_t*, float, float);
    LensView_t g_origLens = nullptr;
    const volatile char*  g_gameView = nullptr;
    const volatile float* g_fovPref = nullptr;
    float g_fov = kDefault;        // cg_fov convention
    float g_stock16x9 = 0.0f;      // what Radiant returns in game view
    float g_ours16x9 = 0.0f;       // what we return instead
    float g_oursTanX = 0.0f;       // tan(hfov/2) of the 16:9 game view frame
    bool  g_active = false;        // g_fov differs from Radiant's 65

    float To16x9(float cgFov) { return float(atan(tan(double(cgFov) * 0.5 / kRadToDeg) * 4.0 / 3.0) * 2.0) * kRadToDeg; }

    void Recompute()
    {
        g_ours16x9 = To16x9(g_fov);
        g_oursTanX = float(tan(double(g_fov) * 0.5 / kRadToDeg) * 4.0 / 3.0);
        g_active = fabsf(g_fov - kDefault) > 0.001f;
    }

    // only the game view value is replaced (probe view, editor preference and the other branch pass through)
    float __fastcall hk_GetFov()
    {
        const float r = g_orig();
        if (!g_active || fabsf(r - g_stock16x9) > 0.01f) return r;
        if (g_gameView && !*g_gameView && g_fovPref && *g_fovPref == r) return r;
        return g_ours16x9;
    }

    // game view projection: only the stock lens is changed, the aspect is kept
    uint8_t* __fastcall hk_LensView(void* cam, uint8_t* view, float focal, float focusDist)
    {
        uint8_t* r = g_origLens(cam, view, focal, focusDist);
        if (!g_active || focal != kLensFocal || focusDist != kLensFocus) return r;
        float* tanY = reinterpret_cast<float*>(view + 0x30);
        float* tanX = reinterpret_cast<float*>(view + 0x34);
        if (!(*tanX > 0.0f)) return r;
        const float k = g_oursTanX / *tanX;
        *tanX *= k;
        *tanY *= k;
        return r;
    }

    float Clamp(float v) { return v < kMin ? kMin : v > kMax ? kMax : v; }
}

void Setup()
{
    g_stock16x9 = float(atanf(0.84905046f) * 2.0f) * kRadToDeg;   // the getter's own expression
    g_fov = Clamp(float(atof(rp::ConfigGet("camera", "game_fov", "65").c_str())));
    if (!(g_fov > 0)) g_fov = kDefault;
    Recompute();
    g_gameView = static_cast<const volatile char*>(rp::host->RadiantAddress(rp::self, kGameView));
    g_fovPref = static_cast<const volatile float*>(rp::host->RadiantAddress(rp::self, kFovPref));
    void* getter = rp::host->RadiantAddress(rp::self, kCamGetFov);
    if (!getter || !rp::host->Detour(rp::self, getter, reinterpret_cast<void*>(&hk_GetFov), reinterpret_cast<void**>(&g_orig))) g_orig = nullptr;
    void* lens = rp::host->RadiantAddress(rp::self, kLensView);
    if (!lens || !rp::host->Detour(rp::self, lens, reinterpret_cast<void*>(&hk_LensView), reinterpret_cast<void**>(&g_origLens))) g_origLens = nullptr;
    rp::Log("gamefov: game view FOV %g (%.2f as 16:9, Radiant's %.2f), getter detour %s, lens detour %s", g_fov, g_ours16x9, g_stock16x9,
            g_orig ? "ok" : "FAILED", g_origLens ? "ok" : "FAILED");
}

float Fov() { return g_fov; }

void Set(float degrees)
{
    degrees = Clamp(degrees);
    if (fabsf(degrees - g_fov) < 0.001f) return;
    g_fov = degrees;
    Recompute();
    char text[32];
    snprintf(text, sizeof text, "%g", g_fov);
    rp::host->ConfigSet(rp::self, "camera", "game_fov", text);
    if (rp::Has(&rp::host->RequestRedraw) && rp::host->RequestRedraw) rp::host->RequestRedraw(rp::self, MODTOOLS_VIEW_3D | MODTOOLS_VIEW_2D);
    rp::Log("gamefov: game view FOV set to %g", g_fov);
}

bool Command(const std::string& command, std::string& reply)
{
    if (command.compare(0, 7, "gamefov") != 0) return false;
    if (command.size() > 8) Set(float(atof(command.c_str() + 8)));
    char buf[256];
    snprintf(buf, sizeof buf, "gamefov: %g (game view %s, getter now %.2f, getter detour %s, lens detour %s)\n", g_fov,
             g_gameView && *g_gameView ? "on" : "off", g_orig ? hk_GetFov() : -1.0f, g_orig ? "ok" : "missing", g_origLens ? "ok" : "missing");
    reply = buf;
    return true;
}
}
