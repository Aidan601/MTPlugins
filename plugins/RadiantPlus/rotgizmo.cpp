// Rotation gizmo for the 3D view: while Mouse Rotate is on, three axis rings (X red, Y green, Z blue) sit at the rotation pivot. A left drag on a ring
// turns the selection through Select_RotateAxis (one undo step, Shift snaps, right click cancels, local axes follow one selected object).
// The 3D view window is subclassed for the mouse, the rings are overlay lines, the only detours hide Radiant's own handles.
#include "rotgizmo.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#pragma comment(lib, "comctl32.lib")

namespace br::rotgizmo
{
namespace
{
    // IDA addresses (no ASLR), resolved through host->RadiantAddress
    const uintptr_t kRotateMode       = 0x145216CB1;  // byte: Mouse Rotate mode is on (CXYWnd::RotateMode)
    const uintptr_t kRotateOrigin     = 0x145216CD8;  // float[3]: the pivot (CXYWnd::RotateOrigin), set when the mode is entered
    const uintptr_t kMainWin          = 0x1444C6560;  // RadiantMainWin*
    const uintptr_t kSelectRotateAxis = 0x140D03F70;  // (int axis, float deg, char paint, char mouse, uchar, char axisRelative)
    const size_t    off_MainWin_ActiveXY = 840;       // CXYWnd*, Select_RotateAxis reads the pivot through it when mouse = 1
    const uintptr_t kGridIndex        = 0x1453A1C20;  // int: the grid size as an index (first field of the prefs block)
    const uintptr_t kNoGridSnap       = 0x1453A1D24;  // int: nonzero = snap to grid is off
    const uintptr_t kRotateStepTable  = 0x143370458;  // int[10]: rotation step in degrees per grid index (0x140C27F70, 90 past the table)
    const uintptr_t kManipulatorDraw  = 0x1402E85B0;  // (manipulator*): draws Radiant's axis handles at the pivot, once per 3D frame
    const uintptr_t kHandleLabelDraw  = 0x1402E9560;  // (manipulator*): the X/Y/Z letters at the handle tips
    const uintptr_t kHandleMapClear   = 0x1402BC480;  // (manipulator + 144): empties the handle map
    const size_t    off_Manip_HandleMap = 144;
    const uintptr_t kHandleColors     = 0x1453A1768;  // float[3][4]: X, Y, Z handle colours (rgba, 0..1), view color table rows
    const uintptr_t kDrawText2D       = 0x1411E1FE0;  // (text, Font**, pos, xAdvance, yDir, rgba, outEnd, uint8 backgroundAlpha): the HUD's text
    const uintptr_t kDebugFont        = 0x145573AA0;  // Font*: the HUD font, line height at +0x40, material at +0x48
    // Local axis mode: a byte of the 3D view. With exactly one BrushInst selected (0x140B02740) the manipulator draw
    // turns its handles to that object's angles, and Select_RotateAxis's last argument turns about those axes.
    const size_t    off_MainWin_Children = 64;        // QE children, +240 = CCamWnd*
    const size_t    off_Children_Cam  = 240;
    const size_t    off_Cam_LocalAxis = 632;          // byte: local axis mode on
    const size_t    off_Cam_Origin    = 208;          // float[3]; +220 angles
    const size_t    off_Cam_Angles    = 220;
    const uintptr_t kSelectedBrushes  = 0x1453A2278;  // BrushCollection, +56 = list sentinel, first node *(sentinel+8)
    const size_t    off_Coll_Sentinel = 56;
    const size_t    off_Node_Next     = 8;
    const size_t    off_BInst_Owner   = 16;           // EntityInst*, its +0 = entity_t*
    const size_t    off_Entity_Angles = 292;          // float[3] pitch yaw roll

    using DrawText2D_t = void(__fastcall*)(const char* text, void** font, const float* pos, const float* xAdvance, const float* yDir, const float* rgba, float* outEnd, unsigned char backgroundAlpha);
    DrawText2D_t g_drawText2D = nullptr;
    void**       g_debugFont = nullptr;

    using SelectRotateAxis_t = void*(__fastcall*)(unsigned axis, float deg, char paint, char mouse, unsigned char a5, char axisRelative);

    const int   kSegments = 64;
    const float kPickPx = 8.0f;
    const float kPi = 3.14159265358979f;
    const UINT_PTR kSubclassId = 0x52504752;   // "RPGR"
    const uint32_t kAxisColor[3] = { 0xFFFF4040, 0xFF40E040, 0xFF4080FF };   // when Radiant's handle colours cannot be read
    const uint32_t kHotColor     = 0xFFFFE040;
    const char     kAxisName[3]  = { 'X', 'Y', 'Z' };

    bool  g_enabled = true;           // our rings
    bool  g_radiantHandles = false;   // Radiant's own straight handles while Mouse Rotate is on
    int   g_sizePx = 90;
    const int kSizeMin = 30, kSizeMax = 400, kSizeWheelStep = 10;
    uint64_t g_sizeSaveMs = 0;        // when a size changed by the wheel goes to the ini (0 = nothing pending)
    int   g_wheelRemainder = 0;
    uint64_t g_wheelUntilMs = 0;      // a resize by the wheel goes on until then (each notch extends it) ...
    POINT g_wheelAt = {};             // ... while the cursor stays where it began (screen pixels)
    bool  g_wheelOn = true;           // the wheel over a ring changes the size
    float g_snapDeg = 15.0f;

    const unsigned char* g_mode = nullptr;
    const float*         g_origin = nullptr;
    void**               g_mainWin = nullptr;
    SelectRotateAxis_t   g_rotate = nullptr;
    const int*           g_gridIndex = nullptr;
    const int*           g_noGridSnap = nullptr;
    const int*           g_stepTable = nullptr;
    const char*          g_selected = nullptr;

    using ManipulatorDraw_t = void(__fastcall*)(void* manipulator);
    using HandleMapClear_t = void*(__fastcall*)(void* map);
    ManipulatorDraw_t g_oManipulatorDraw = nullptr;
    ManipulatorDraw_t g_oHandleLabelDraw = nullptr;
    HandleMapClear_t  g_handleMapClear = nullptr;
    const float*      g_handleColors = nullptr;

    // Radiant's own handle colour for the axis, so rings and handles match under any color preset
    uint32_t AxisColor(int axis, uint32_t alpha)
    {
        uint32_t rgb = kAxisColor[axis] & 0xFFFFFF;
        if (g_handleColors)
        {
            const float* c = g_handleColors + axis * 4;
            auto byte = [](float v) { return uint32_t(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0f + 0.5f); };
            if (c[0] == c[0] && c[1] == c[1] && c[2] == c[2]) rgb = byte(c[0]) << 16 | byte(c[1]) << 8 | byte(c[2]);
        }
        return alpha << 24 | rgb;
    }

    HWND g_camera = nullptr;
    bool g_hasSelection = false;
    uint64_t g_selectionCheckMs = 0;

    // Screen positions of the rings in the last 3D frame, the mouse works on these
    struct Ring { float x[kSegments]; float y[kSegments]; bool ok[kSegments]; bool front[kSegments]; };
    Ring  g_rings[3];
    bool  g_ringsValid = false;
    float g_centreX = 0, g_centreY = 0;
    int   g_hover = -1;

    struct Drag
    {
        bool  active = false;
        int   axis = 0;
        float startX = 0, startY = 0;
        float tanX = 0, tanY = 0;     // screen direction of a right-handed turn about the axis at the grabbed point
        float phi = 0;                // ring angle of the grabbed point, radians
        float target = 0;             // degrees asked for by the mouse, right-handed about the axis
        float applied = 0;            // degrees Radiant has turned the selection so far
        bool  local = false;          // turning about the selected object's own axis (local axis mode)
        float axes[3][3] = {};        // the ring frame at the grab, kept for the drag: the object turns, the ring does not
        bool  eatRightUp = false;
    } g_drag;

    // the ring frame: world axes, or the selected object's own axes in local axis mode
    float g_axes[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    bool  g_local = false;

    // diagnostic: frames where the overlay's camera differs from the 3D view's camera when drawing (the rings would trail)
    unsigned g_frames = 0, g_camLagFrames = 0;
    float    g_camLagMax = 0;

    bool Ready() { return g_mode && g_origin && g_mainWin && g_rotate; }

    bool ModeOn()
    {
        if (!Ready() || !*g_mode) return false;
        void* mainWin = *g_mainWin;
        return mainWin && *reinterpret_cast<void**>(static_cast<char*>(mainWin) + off_MainWin_ActiveXY);
    }

    bool Visible() { return g_enabled && ModeOn() && g_hasSelection; }

    template <typename T> T Field(const void* base, size_t offset) { return *reinterpret_cast<const T*>(static_cast<const char*>(base) + offset); }

    // the 3D view (CCamWnd*), through the main window's QE children
    void* CamWnd()
    {
        void* mainWin = g_mainWin ? *g_mainWin : nullptr;
        void* children = mainWin ? Field<void*>(mainWin, off_MainWin_Children) : nullptr;
        return children ? Field<void*>(children, off_Children_Cam) : nullptr;
    }

    // Local axis mode with exactly one selected object: that object's angle axes (AngleVectors: forward = X, right = -Y,
    // up = Z), else world. Main thread only.
    bool ReadLocalAxes(float axes[3][3])
    {
        if (!g_mainWin || !g_selected) return false;
        const void* cam = CamWnd();
        if (!cam || !Field<unsigned char>(cam, off_Cam_LocalAxis)) return false;
        const void* sentinel = Field<void*>(g_selected, off_Coll_Sentinel);
        if (!sentinel) return false;
        const void* first = Field<void*>(sentinel, off_Node_Next);
        if (!first || first == sentinel || Field<void*>(first, off_Node_Next) != sentinel) return false;   // none, or more than one
        const void* owner = Field<void*>(first, off_BInst_Owner);
        const void* entity = owner ? Field<void*>(owner, 0) : nullptr;
        if (!entity) return false;
        const float* angles = reinterpret_cast<const float*>(static_cast<const char*>(entity) + off_Entity_Angles);
        const float d = kPi / 180.0f;
        const float sp = sinf(angles[0] * d), cp = cosf(angles[0] * d);
        const float sy = sinf(angles[1] * d), cy = cosf(angles[1] * d);
        const float sr = sinf(angles[2] * d), cr = cosf(angles[2] * d);
        const float forward[3] = { cp * cy, cp * sy, -sp };
        const float right[3] = { -sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp };
        const float up[3] = { cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp };
        for (int i = 0; i < 3; ++i) { axes[0][i] = forward[i]; axes[1][i] = -right[i]; axes[2][i] = up[i]; }
        for (int i = 0; i < 9; ++i) if (!(axes[i / 3][i % 3] == axes[i / 3][i % 3])) return false;
        return true;
    }

    void CheckCamera(const ModToolsDrawContext* ctx)
    {
        const void* cam = CamWnd();
        if (!cam) return;
        const float* origin = reinterpret_cast<const float*>(static_cast<const char*>(cam) + off_Cam_Origin);
        const float* angles = reinterpret_cast<const float*>(static_cast<const char*>(cam) + off_Cam_Angles);
        float diff = 0;
        for (int i = 0; i < 3; ++i)
        {
            diff = fmaxf(diff, fabsf(origin[i] - ctx->cam.origin[i]));
            diff = fmaxf(diff, fabsf(angles[i] - ctx->cam.angles[i]));
        }
        ++g_frames;
        if (diff <= 0.01f) return;
        ++g_camLagFrames;
        if (diff > g_camLagMax) g_camLagMax = diff;
        if (g_camLagFrames <= 5)
            rp::Log("gizmo: overlay camera %.2f %.2f %.2f / %.2f %.2f differs from the live camera %.2f %.2f %.2f / %.2f %.2f",
                    ctx->cam.origin[0], ctx->cam.origin[1], ctx->cam.origin[2], ctx->cam.angles[0], ctx->cam.angles[1],
                    origin[0], origin[1], origin[2], angles[0], angles[1]);
    }

    void RefreshAxes()
    {
        float local[3][3];
        g_local = ReadLocalAxes(local);
        for (int a = 0; a < 3; ++a)
            for (int i = 0; i < 3; ++i) g_axes[a][i] = g_local ? local[a][i] : (a == i ? 1.0f : 0.0f);
    }

    // A point of ring `axis` at ring angle phi (radians), `radius` from the centre in frame `axes`. The ring lies in the
    // plane of the other two axes (u x v = the axis), so a growing phi is a right-handed turn.
    void RingPoint(const float axes[3][3], int axis, const float c[3], float radius, float phi, float out[3])
    {
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        const float cu = cosf(phi) * radius, sv = sinf(phi) * radius;
        for (int i = 0; i < 3; ++i) out[i] = c[i] + axes[u][i] * cu + axes[v][i] * sv;
    }

    // The angle step in degrees, 0 = free. With snap to grid on it is Radiant's rotation step for the grid size, Shift
    // asks for the ini step instead.
    float SnapStep()
    {
        if (g_snapDeg > 0 && (GetKeyState(VK_SHIFT) & 0x8000)) return g_snapDeg;
        if (!g_gridIndex || !g_noGridSnap || !g_stepTable || *g_noGridSnap) return 0.0f;
        const int index = *g_gridIndex;
        const int step = index >= 0 && index <= 9 ? g_stepTable[index] : 90;
        return step > 0 ? float(step) : 90.0f;
    }

    // Radiant's handles stay unless turned off for Mouse Rotate. The handle map is emptied like the draw does, so no
    // old handle stays clickable while hidden.
    void __fastcall hk_ManipulatorDraw(void* manipulator)
    {
        if (!g_radiantHandles && manipulator && g_mode && *g_mode)
        {
            if (g_handleMapClear) g_handleMapClear(static_cast<char*>(manipulator) + off_Manip_HandleMap);
            return;
        }
        g_oManipulatorDraw(manipulator);
    }

    // The X/Y/Z letters use tip positions only the handle draw refreshes, so hidden handles would leave them drifting.
    void __fastcall hk_HandleLabelDraw(void* manipulator)
    {
        if (!g_radiantHandles && manipulator && g_mode && *g_mode) return;
        g_oHandleLabelDraw(manipulator);
    }


    // Select_RotateAxis turns against the right-hand rule (Q3Radiant negates the angle), also with `local`
    // (axisRelative: the first selected object's own axis).
    void Turn(int axis, float degrees, bool local)
    {
        if (degrees == 0.0f || !ModeOn()) return;
        g_rotate(unsigned(axis), -degrees, 1, 1, 0, local ? 1 : 0);
    }

    void ApplyPending()
    {
        if (!g_drag.active || g_drag.target == g_drag.applied) return;
        Turn(g_drag.axis, g_drag.target - g_drag.applied, g_drag.local);
        g_drag.applied = g_drag.target;
    }

    void FinishDrag(bool takeBack)
    {
        if (!g_drag.active) return;
        if (takeBack) g_drag.target = 0;
        ApplyPending();
        g_drag.active = false;      // before ReleaseCapture (WM_CAPTURECHANGED comes back here)
        rp::host->EndUndo(rp::self);
        if (g_camera && GetCapture() == g_camera) ReleaseCapture();
        rp::Log("gizmo: %s%c %s %.1f degrees", g_drag.local ? "local " : "", kAxisName[g_drag.axis], takeBack ? "drag taken back at" : "turned by", takeBack ? 0.0f : g_drag.applied);
    }

    float SegmentDistance(float px, float py, float ax, float ay, float bx, float by)
    {
        const float dx = bx - ax, dy = by - ay;
        const float len2 = dx * dx + dy * dy;
        float t = len2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        const float cx = ax + dx * t - px, cy = ay + dy * t - py;
        return sqrtf(cx * cx + cy * cy);
    }

    // the ring under the cursor (a near half wins over another ring's far half)
    bool Pick(float px, float py, int* axisOut, int* segmentOut)
    {
        if (!g_ringsValid) return false;
        float best = 1e9f;
        for (int a = 0; a < 3; ++a)
        {
            const Ring& r = g_rings[a];
            for (int i = 0; i < kSegments; ++i)
            {
                const int j = (i + 1) % kSegments;
                if (!r.ok[i] || !r.ok[j]) continue;
                const float d = SegmentDistance(px, py, r.x[i], r.y[i], r.x[j], r.y[j]);
                if (d > kPickPx) continue;
                const float score = d + (r.front[i] ? 0.0f : kPickPx);
                if (score < best) { best = score; *axisOut = a; *segmentOut = i; }
            }
        }
        return best < 1e9f;
    }

    bool BeginDrag(HWND hwnd, int x, int y)
    {
        if (g_drag.active || !Visible()) return false;
        int axis = 0, segment = 0;
        if (!Pick(float(x), float(y), &axis, &segment)) return false;
        const Ring& r = g_rings[axis];
        const int next = (segment + 1) % kSegments;
        float tx = r.x[next] - r.x[segment], ty = r.y[next] - r.y[segment];
        const float len = sqrtf(tx * tx + ty * ty);
        if (len < 1e-3f) return false;      // the ring is seen edge-on here: no direction to drag in
        g_drag = Drag();
        g_drag.active = true;
        g_drag.axis = axis;
        g_drag.startX = float(x); g_drag.startY = float(y);
        g_drag.tanX = tx / len; g_drag.tanY = ty / len;
        const float along = ((float(x) - r.x[segment]) * tx + (float(y) - r.y[segment]) * ty) / (len * len);
        g_drag.phi = 2.0f * kPi * (float(segment) + (along < 0 ? 0 : along > 1 ? 1 : along)) / float(kSegments);
        g_drag.local = g_local;
        memcpy(g_drag.axes, g_axes, sizeof g_axes);
        rp::host->BeginUndo(rp::self, "gizmo rotate selection");
        SetCapture(hwnd);
        return true;
    }

    void UpdateDrag(int x, int y)
    {
        const float along = (float(x) - g_drag.startX) * g_drag.tanX + (float(y) - g_drag.startY) * g_drag.tanY;
        float degrees = along / float(g_sizePx) * (180.0f / kPi);
        const float step = SnapStep();
        if (step > 0) degrees = floorf(degrees / step + 0.5f) * step;
        g_drag.target = degrees;      // applied on the next tick, so a burst of mouse moves is one rotation
    }

    void SaveSize()
    {
        g_sizeSaveMs = 0;
        char value[16];
        snprintf(value, sizeof(value), "%d", g_sizePx);
        rp::host->ConfigSet(rp::self, "gizmo", "size_px", value);
    }

    // The wheel over a lit ring changes the ring radius, 10 px a notch (no modifier is free: the camera wheel uses them
    // all). A resize that began on a ring continues while notches keep coming and the cursor stays put. Saved shortly
    // after the last notch.
    bool WheelResize(WPARAM wp, LPARAM lp)
    {
        if (!g_wheelOn || !Visible() || g_drag.active) return false;
        const POINT at = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        const uint64_t now = GetTickCount64();
        const bool goesOn = now < g_wheelUntilMs && abs(at.x - g_wheelAt.x) <= 3 && abs(at.y - g_wheelAt.y) <= 3;
        if (!goesOn)
        {
            if (g_hover < 0) return false;
            g_wheelAt = at;
            g_wheelRemainder = 0;
        }
        g_wheelUntilMs = now + 600;
        g_wheelRemainder += GET_WHEEL_DELTA_WPARAM(wp);
        const int notches = g_wheelRemainder / WHEEL_DELTA;
        g_wheelRemainder -= notches * WHEEL_DELTA;
        int size = g_sizePx + notches * kSizeWheelStep;
        size = size < kSizeMin ? kSizeMin : size > kSizeMax ? kSizeMax : size;
        if (size != g_sizePx) { g_sizePx = size; g_sizeSaveMs = GetTickCount64() + 800; }
        return true;
    }

    LRESULT CALLBACK CameraSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR)
    {
        switch (msg)
        {
        case WM_LBUTTONDOWN:
            if (BeginDrag(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp))) return 0;
            break;
        case WM_MOUSEMOVE:
            if (g_drag.active) { UpdateDrag(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0; }
            {
                int axis = -1, segment = 0;
                const bool buttons = (wp & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) != 0;
                g_hover = !buttons && Visible() && Pick(float(GET_X_LPARAM(lp)), float(GET_Y_LPARAM(lp)), &axis, &segment) ? axis : -1;
            }
            break;
        case WM_LBUTTONUP:
            if (g_drag.active) { UpdateDrag(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); FinishDrag(false); return 0; }
            break;
        case WM_RBUTTONDOWN:
            if (g_drag.active) { FinishDrag(true); g_drag.eatRightUp = true; return 0; }
            break;
        case WM_RBUTTONUP:
            if (g_drag.eatRightUp) { g_drag.eatRightUp = false; return 0; }
            break;
        case WM_MOUSELEAVE:
            g_hover = -1;
            break;
        case WM_MOUSEWHEEL:
            if (WheelResize(wp, lp)) return 0;
            break;
        case WM_CANCELMODE:
        case WM_CAPTURECHANGED:
        case WM_KILLFOCUS:
            FinishDrag(false);
            break;
        case WM_ACTIVATEAPP:
            if (!wp) FinishDrag(false);
            break;
        case WM_NCDESTROY:
            FinishDrag(false);
            RemoveWindowSubclass(hwnd, CameraSubclass, id);
            if (g_camera == hwnd) g_camera = nullptr;
            break;
        }
        return DefSubclassProc(hwnd, msg, wp, lp);
    }

    void DetachCamera()
    {
        FinishDrag(false);
        if (g_camera && IsWindow(g_camera)) RemoveWindowSubclass(g_camera, CameraSubclass, kSubclassId);
        g_camera = nullptr;
    }

    void AttachCamera()
    {
        HWND current = static_cast<HWND>(rp::host->CamWindow(rp::self));
        if (current == g_camera) return;
        DetachCamera();
        if (current && IsWindow(current) && SetWindowSubclass(current, CameraSubclass, kSubclassId, 0)) g_camera = current;
    }

    // The lit ring, looked up every frame (the rings move with the camera). Nothing is lit while a mouse button is held
    // or the cursor is outside the 3D view.
    void RefreshHover()
    {
        if (g_drag.active) return;
        g_hover = -1;
        if (!g_camera || !g_ringsValid) return;
        if ((GetKeyState(VK_LBUTTON) | GetKeyState(VK_RBUTTON) | GetKeyState(VK_MBUTTON)) & 0x8000) return;
        POINT pt;
        if (!GetCursorPos(&pt)) return;
        const HWND under = WindowFromPoint(pt);
        if ((under != g_camera && !IsChild(g_camera, under)) || !ScreenToClient(g_camera, &pt)) return;
        int axis = -1, segment = 0;
        if (Pick(float(pt.x), float(pt.y), &axis, &segment)) g_hover = axis;
    }

    void Line(float x0, float y0, float x1, float y1, uint32_t argb, bool thick)
    {
        rp::host->DrawScreenLine(rp::self, x0, y0, x1, y1, argb);
        if (!thick) return;
        // one pixel to the side, across the line's direction
        const bool steep = fabsf(y1 - y0) > fabsf(x1 - x0);
        rp::host->DrawScreenLine(rp::self, x0 + (steep ? 1.0f : 0.0f), y0 + (steep ? 0.0f : 1.0f), x1 + (steep ? 1.0f : 0.0f), y1 + (steep ? 0.0f : 1.0f), argb);
    }

    // A mark across ring `axis` at ring angle phi (radians): from the ring outwards, `length` in ring radii.
    void Mark(const float axes[3][3], int axis, const float c[3], float radius, float phi, float length, uint32_t argb, bool thick)
    {
        float a[3], b[3];
        RingPoint(axes, axis, c, radius, phi, a);
        RingPoint(axes, axis, c, radius * (1.0f + length), phi, b);
        float ax, ay, bx, by;
        if (rp::host->Project(rp::self, a, &ax, &ay) && rp::host->Project(rp::self, b, &bx, &by)) Line(ax, ay, bx, by, argb, thick);
    }

    // The snap positions of a ring: the world angles when idle, from the grabbed point in a drag (the snap is on the
    // angle turned). Steps under 5 degrees are thinned out.
    void SnapMarks(const float axes[3][3], int axis, const float c[3], float radius, float step, float phi0, uint32_t argb)
    {
        if (step <= 0) return;
        float shown = step;
        while (shown < 5.0f) shown += step;
        const int count = int(360.0f / shown + 0.5f);
        if (count < 2 || count > 72) return;
        for (int i = 0; i < count; ++i)
        {
            const float degrees = float(i) * shown;
            const bool quarter = fmodf(degrees + 0.01f, 90.0f) < 0.02f;
            Mark(axes, axis, c, radius, phi0 + degrees * kPi / 180.0f, quarter ? 0.14f : 0.07f, argb, quarter);
        }
    }

    // Text with its top-left at pixel (x, y) in Radiant's HUD font, without the host labels' dark box. Straight to DrawText2D, only right inside the
    // host's HUD pass (MeasureLabel says 1 there: origin = pane centre, +y up, 1 unit = 1 pixel), elsewhere the host's stroke font.
    void BareText(const ModToolsDrawContext* ctx, float x, float y, const char* text, uint32_t argb, float w, float h, bool engine)
    {
        void* font = g_debugFont ? *g_debugFont : nullptr;
        if (!engine || !g_drawText2D || !font || !*reinterpret_cast<void**>(static_cast<char*>(font) + 0x48))
        {
            rp::host->DrawLabel(rp::self, x, y, text, argb, 2.0f);
            return;
        }
        const float pos[3] = { floorf(x) - float(ctx->width) * 0.5f, float(ctx->height) * 0.5f - floorf(y + h), 0.0f };
        const float xAdvance[3] = { 1, 0, 0 }, yDir[3] = { 0, -1, 0 };
        const float rgba[4] = { ((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f, ((argb >> 24) & 0xFF) / 255.0f };
        g_drawText2D(text, g_debugFont, pos, xAdvance, yDir, rgba, nullptr, 0);
    }

    // The letter of ring `axis`, 45 degrees between the other two axes on the camera-facing quarter, pushed out of the ring.
    void AxisLabel(const ModToolsDrawContext* ctx, const float axes[3][3], int axis, const float c[3], float radius, const float toCam[3], float cx, float cy, uint32_t argb)
    {
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        const float k = 0.70710678f * radius;
        const float su = axes[u][0] * toCam[0] + axes[u][1] * toCam[1] + axes[u][2] * toCam[2] < 0 ? -k : k;
        const float sv = axes[v][0] * toCam[0] + axes[v][1] * toCam[1] + axes[v][2] * toCam[2] < 0 ? -k : k;
        float p[3];
        for (int i = 0; i < 3; ++i) p[i] = c[i] + axes[u][i] * su + axes[v][i] * sv;
        float x = 0, y = 0;
        if (!rp::host->Project(rp::self, p, &x, &y)) return;
        float dx = x - cx, dy = y - cy;
        const float len = sqrtf(dx * dx + dy * dy);
        if (len > 1e-3f) { dx /= len; dy /= len; } else { dx = 0; dy = -1; }
        const char text[2] = { kAxisName[axis], 0 };
        float w = 12, h = 12;
        bool engine = false;
        if (rp::Has(&rp::host->MeasureLabel) && rp::host->MeasureLabel) engine = rp::host->MeasureLabel(rp::self, text, 2.0f, &w, &h) == 1;
        const float out = 6.0f + 0.5f * (w > h ? w : h);
        BareText(ctx, x + dx * out - w * 0.5f, y + dy * out - h * 0.5f, text, argb, w, h, engine);
    }

    std::string Status()
    {
        char buf[640];
        float local[3][3];
        const bool isLocal = ReadLocalAxes(local);
        char frame[160] = "world";
        if (isLocal)
            snprintf(frame, sizeof(frame), "local X %.3f %.3f %.3f Y %.3f %.3f %.3f Z %.3f %.3f %.3f",
                     local[0][0], local[0][1], local[0][2], local[1][0], local[1][1], local[1][2], local[2][0], local[2][1], local[2][2]);
        snprintf(buf, sizeof(buf), "gizmo: rotate=%d radiant_handles=%d (hook=%d labels=%d) snap=%g ready=%d mode=%d selection=%d attached=%d size=%d px shift snap=%g, pivot %.1f %.1f %.1f, on screen=%d at %.0f %.0f, axes %s, camera lag %u of %u frames (max %.2f)%s",
                 g_enabled ? 1 : 0, g_radiantHandles ? 1 : 0, g_oManipulatorDraw ? 1 : 0, g_oHandleLabelDraw ? 1 : 0, SnapStep(), Ready() ? 1 : 0, ModeOn() ? 1 : 0, g_hasSelection ? 1 : 0, g_camera ? 1 : 0, g_sizePx, g_snapDeg,
                 g_origin ? g_origin[0] : 0.0f, g_origin ? g_origin[1] : 0.0f, g_origin ? g_origin[2] : 0.0f, g_ringsValid ? 1 : 0, g_centreX, g_centreY,
                 frame, g_camLagFrames, g_frames, g_camLagMax, g_drag.active ? ", dragging" : "");
        return buf;
    }
}

namespace
{
    void SetRings(bool on)
    {
        g_enabled = on;
        rp::host->ConfigSet(rp::self, "gizmo", "rotate", on ? "1" : "0");
        if (!on) Shutdown();
    }
    void SetHandles(bool on)
    {
        g_radiantHandles = on;
        rp::host->ConfigSet(rp::self, "gizmo", "radiant_handles", on ? "1" : "0");
    }
}

void Setup()
{
    g_enabled = rp::ConfigGetInt("gizmo", "rotate", 1) != 0;
    g_sizePx = rp::ConfigGetInt("gizmo", "size_px", 90);
    if (g_sizePx < kSizeMin) g_sizePx = kSizeMin;
    if (g_sizePx > kSizeMax) g_sizePx = kSizeMax;
    g_wheelOn = rp::ConfigGetInt("gizmo", "size_wheel", 1) != 0;
    g_snapDeg = float(rp::ConfigGetInt("gizmo", "snap_degrees", 15));
    if (g_snapDeg < 0) g_snapDeg = 0;
    g_mode = static_cast<const unsigned char*>(rp::host->RadiantAddress(rp::self, kRotateMode));
    g_origin = static_cast<const float*>(rp::host->RadiantAddress(rp::self, kRotateOrigin));
    g_mainWin = static_cast<void**>(rp::host->RadiantAddress(rp::self, kMainWin));
    g_rotate = reinterpret_cast<SelectRotateAxis_t>(rp::host->RadiantAddress(rp::self, kSelectRotateAxis));
    g_gridIndex = static_cast<const int*>(rp::host->RadiantAddress(rp::self, kGridIndex));
    g_noGridSnap = static_cast<const int*>(rp::host->RadiantAddress(rp::self, kNoGridSnap));
    g_stepTable = static_cast<const int*>(rp::host->RadiantAddress(rp::self, kRotateStepTable));
    g_selected = static_cast<const char*>(rp::host->RadiantAddress(rp::self, kSelectedBrushes));
    g_radiantHandles = rp::ConfigGetInt("gizmo", "radiant_handles", 0) != 0;
    g_handleMapClear = reinterpret_cast<HandleMapClear_t>(rp::host->RadiantAddress(rp::self, kHandleMapClear));
    if (void* draw = rp::host->RadiantAddress(rp::self, kManipulatorDraw))
        if (!rp::host->Detour(rp::self, draw, hk_ManipulatorDraw, reinterpret_cast<void**>(&g_oManipulatorDraw))) g_oManipulatorDraw = nullptr;
    if (void* labels = rp::host->RadiantAddress(rp::self, kHandleLabelDraw))
        if (!rp::host->Detour(rp::self, labels, hk_HandleLabelDraw, reinterpret_cast<void**>(&g_oHandleLabelDraw))) g_oHandleLabelDraw = nullptr;
    g_handleColors = static_cast<const float*>(rp::host->RadiantAddress(rp::self, kHandleColors));
    g_drawText2D = reinterpret_cast<DrawText2D_t>(rp::host->RadiantAddress(rp::self, kDrawText2D));
    g_debugFont = static_cast<void**>(rp::host->RadiantAddress(rp::self, kDebugFont));
    if (!rp::Has(&rp::host->BeginUndo) || !rp::host->BeginUndo || !rp::host->EndUndo || !rp::host->CamWindow) g_rotate = nullptr;
    rp::Log("gizmo: rotation gizmo enabled=%d ready=%d size=%d px snap=%g", g_enabled ? 1 : 0, Ready() ? 1 : 0, g_sizePx, g_snapDeg);
}

bool Rings() { return g_enabled; }
bool RadiantHandles() { return g_radiantHandles; }
void Set(bool rings, bool radiantHandles)
{
    if (rings != g_enabled) SetRings(rings);
    if (radiantHandles != g_radiantHandles) SetHandles(radiantHandles);
}

int SizePx() { return g_sizePx; }
bool SizeWheel() { return g_wheelOn; }
void SetSize(int px)
{
    px = px < kSizeMin ? kSizeMin : px > kSizeMax ? kSizeMax : px;
    if (px == g_sizePx && !g_sizeSaveMs) return;
    g_sizePx = px;
    SaveSize();
}

void Shutdown()
{
    if (g_sizeSaveMs) SaveSize();
    DetachCamera();
    g_ringsValid = false;
    g_hover = -1;
}

void Tick()
{
    if (!Ready()) return;
    if (g_sizeSaveMs && GetTickCount64() >= g_sizeSaveMs) SaveSize();
    if (!g_enabled) { if (g_camera) DetachCamera(); return; }
    AttachCamera();
    const uint64_t now = GetTickCount64();
    if (now - g_selectionCheckMs >= 150 || g_selectionCheckMs == 0)
    {
        g_selectionCheckMs = now;
        ModToolsSelectedBrush one;
        g_hasSelection = *g_mode && rp::host->GetSelection(rp::self, &one, 1) > 0;
    }
    if (!g_drag.active) return;
    if (!ModeOn()) { FinishDrag(false); return; }
    ApplyPending();
}

void Draw(const ModToolsDrawContext* ctx)
{
    if (!ctx || ctx->view != 1) return;
    g_ringsValid = false;
    if (!Visible()) { g_hover = -1; return; }
    CheckCamera(ctx);

    const float c[3] = { g_origin[0], g_origin[1], g_origin[2] };
    float cx = 0, cy = 0;
    if (!rp::host->Project(rp::self, c, &cx, &cy)) return;

    // pixels per unit at the pivot's depth, from a step along the camera's right vector
    const float pitch = ctx->cam.angles[0] * kPi / 180.0f, yaw = ctx->cam.angles[1] * kPi / 180.0f;
    const float fwd[3] = { cosf(pitch) * cosf(yaw), cosf(pitch) * sinf(yaw), sinf(pitch) };
    float right[3] = { fwd[1], -fwd[0], 0.0f };      // forward x up
    const float rl = sqrtf(right[0] * right[0] + right[1] * right[1]);
    if (rl < 1e-4f) { right[0] = 0; right[1] = -1; } else { right[0] /= rl; right[1] /= rl; }
    const float toCam[3] = { ctx->cam.origin[0] - c[0], ctx->cam.origin[1] - c[1], ctx->cam.origin[2] - c[2] };
    const float dist = sqrtf(toCam[0] * toCam[0] + toCam[1] * toCam[1] + toCam[2] * toCam[2]);
    const float step = dist * 0.1f > 1.0f ? dist * 0.1f : 1.0f;
    const float probe[3] = { c[0] + right[0] * step, c[1] + right[1] * step, c[2] };
    float px = 0, py = 0;
    if (!rp::host->Project(rp::self, probe, &px, &py)) return;
    const float pixelsPerUnit = sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy)) / step;
    if (pixelsPerUnit < 1e-6f) return;
    const float radius = float(g_sizePx) / pixelsPerUnit;

    if (!g_drag.active) RefreshAxes();
    const float (*axes)[3] = g_drag.active ? g_drag.axes : g_axes;
    for (int a = 0; a < 3; ++a)
    {
        Ring& r = g_rings[a];
        for (int i = 0; i < kSegments; ++i)
        {
            float p[3];
            RingPoint(axes, a, c, radius, 2.0f * kPi * float(i) / float(kSegments), p);
            r.ok[i] = rp::host->Project(rp::self, p, &r.x[i], &r.y[i]) != 0;
            r.front[i] = (p[0] - c[0]) * toCam[0] + (p[1] - c[1]) * toCam[1] + (p[2] - c[2]) * toCam[2] >= 0;
        }
    }
    g_centreX = cx; g_centreY = cy;
    g_ringsValid = true;
    RefreshHover();

    for (int a = 0; a < 3; ++a)
    {
        if (g_drag.active && a != g_drag.axis) continue;
        const bool hot = g_drag.active || a == g_hover;
        const Ring& r = g_rings[a];
        for (int i = 0; i < kSegments; ++i)
        {
            const int j = (i + 1) % kSegments;
            if (!r.ok[i] || !r.ok[j]) continue;
            const bool front = r.front[i] || g_drag.active;
            Line(r.x[i], r.y[i], r.x[j], r.y[j], hot ? (front ? kHotColor : 0x70FFE040) : AxisColor(a, front ? 0xFF : 0x70), front);
        }
    }
    const float snap = SnapStep();
    if (g_drag.active)
    {
        SnapMarks(axes, g_drag.axis, c, radius, snap, g_drag.phi, 0xC0FFFFFF);
        Mark(axes, g_drag.axis, c, radius, g_drag.phi, 0.22f, 0xFFFFFFFF, true);                                    // where the drag began
        Mark(axes, g_drag.axis, c, radius, g_drag.phi + g_drag.target * kPi / 180.0f, 0.22f, kHotColor, true);      // where it is now
    }
    else if (g_hover >= 0) SnapMarks(axes, g_hover, c, radius, snap, 0.0f, 0xC0FFFFFF);
    for (int a = 0; a < 3; ++a)
    {
        if (g_drag.active && a != g_drag.axis) continue;
        AxisLabel(ctx, axes, a, c, radius, toCam, cx, cy, g_drag.active || a == g_hover ? kHotColor : AxisColor(a, 0xFF));
    }
    Line(cx - 5, cy, cx + 5, cy, 0xFFFFFFFF, false);
    Line(cx, cy - 5, cx, cy + 5, 0xFFFFFFFF, false);

    if (g_drag.active)
    {
        char text[48];
        snprintf(text, sizeof(text), "%s%c %.1f", g_drag.local ? "local " : "", kAxisName[g_drag.axis], g_drag.target);
        rp::host->DrawLabel(rp::self, cx + 12, cy + 12, text, kHotColor, 2.0f);
    }
}

bool Command(const std::string& command, std::string& reply)
{
    if (command.rfind("gizmo", 0) != 0) return false;
    if (command == "gizmo" || command == "gizmo status") { reply = Status(); return true; }
    if (command == "gizmo 0" || command == "gizmo 1") { SetRings(command.back() == '1'); reply = Status(); return true; }
    if (command == "gizmo handles 0" || command == "gizmo handles 1") { SetHandles(command.back() == '1'); reply = Status(); return true; }
    if (command.rfind("gizmo size ", 0) == 0)
    {
        const int px = atoi(command.c_str() + 11);
        if (px <= 0) { reply = "usage: gizmo size <30..400> (ring radius in pixels)"; return true; }
        SetSize(px);
        reply = Status();
        return true;
    }
    // "gizmo turn <x|y|z> <degrees>": one undoable right-handed turn about the pivot for tests (needs Mouse Rotate on),
    // about the object's own axis in local axis mode
    if (command.rfind("gizmo turn ", 0) == 0)
    {
        char axisName = 0; float degrees = 0;
        if (sscanf(command.c_str() + 11, " %c %f", &axisName, &degrees) != 2 || !ModeOn() || g_drag.active)
        { reply = "usage: gizmo turn <x|y|z> <degrees>, with Mouse Rotate on and a selection"; return true; }
        const int axis = (axisName | 0x20) - 'x';
        if (axis < 0 || axis > 2) { reply = "axis must be x, y or z"; return true; }
        float local[3][3];
        const bool isLocal = ReadLocalAxes(local);
        rp::host->BeginUndo(rp::self, "gizmo rotate selection");
        Turn(axis, degrees, isLocal);
        rp::host->EndUndo(rp::self);
        reply = isLocal ? "turned about the local axis" : "turned about the world axis";
        return true;
    }
    reply = "gizmo [status|0|1|handles 0|1|size <30..400>|turn <x|y|z> <degrees>]";
    return true;
}
}
