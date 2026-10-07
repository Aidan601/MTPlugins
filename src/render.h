#pragma once
#include <cstdint>
#include <string>
#include "modtools_plugin.h"

// Overlay drawing on top of Radiant's views (Direct3D 11). The Cam_Draw / XY_Draw detours announce the view about to be
// presented, the host asks every plugin to draw (OnDraw) and plugins add lines/text below. Main thread only.
namespace render
{
    enum class View { None, Cam, XY };

    struct CamParams { float origin[3]; float angles[3]; int width, height; bool gameView; float fovDeg; };   // fovDeg: Radiant's Cam_GetFov at that draw, 0 = unknown
    struct XYParams  { int type; float origin[3]; float scale; };      // type: 2 = XY top, 1 = XZ front, 0 = YZ side

    bool Init();
    void Shutdown();

    void BeginCamView(const CamParams& p);   // called right before Radiant draws the 3D view
    void BeginXYView(const XYParams& p);     // called right before Radiant draws a 2D view
    void EndView();                          // called when the view's draw call returned
    // Called after Radiant's own cam HUD text (DrawHud detour): builds the cam frame so plugin labels use Radiant's font (DrawText2D).
    // hudW/hudH = the pane size the HUD lays out in, argW/argH = what OnRender passed (logged).
    void CamHudPass(int hudW, int hudH, int argW, int argH);
    void SetEngineTextAvailable(bool hooked);

    // Project a world point with the given 3D camera into pixels (false when behind the camera).
    bool ProjectCam(const CamParams& c, const float p[3], float& sx, float& sy);
    // Parameters of the last 3D frame (pane size comes from the swap chain, so it is only known after a Present).
    CamParams LastCam();
    void* CamWindow();   // HWND of the camera pane's swap chain (null until the first 3D frame)
    void* XYWindow(int type);   // HWND of a 2D view's swap chain (2 = XY top, 1 = XZ front, 0 = YZ side, null until it drew)

    // ---- for plugins, valid only while a frame is being built (inside OnDraw) ----
    bool InFrame();
    bool ProjectCurrent(const float p[3], float& sx, float& sy);       // with the view being drawn (3D or 2D)
    void LineWorld(const float a[3], const float b[3], uint32_t argb);
    void LineScreen(float x0, float y0, float x1, float y1, uint32_t argb);
    // Alpha-blended filled triangle in the host overlay. Points behind the active camera are skipped.
    void TriangleWorld(const float a[3], const float b[3], const float c[3], uint32_t argb);
    // Label with its top-left at (x, y). In the 3D view (HUD pass) Radiant's HUD font with its dark box, in the 2D views,
    // or without the HUD hook, the 4x6 stroke font (upper case). px 2 = normal size.
    void Text(float x, float y, const std::string& text, uint32_t argb, float px);
    // Size a label would take in the font Text uses right now (or would use in the 3D view outside a frame).
    // Returns true when that is Radiant's font.
    bool MeasureText(const std::string& text, float px, int view, float& width, float& height);

    // A bounded native-material preview request from Splined Models. The host only acts on a
    // draw after it has read back and matched that draw's world-space vertex bounds.
    void SetNativeModelPreview(const ModToolsNativeModelPreview* preview);
    void SetNativeModelPreviewSet(const ModToolsNativeModelPreview* previews, size_t count);
    int NativeModelPreviewStatus();
    unsigned NativeModelPreviewDrawCount();
    int NativeModelPreviewStatusForId(const char* requestId);
    unsigned NativeModelPreviewDrawCountForId(const char* requestId);
}
