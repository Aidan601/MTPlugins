// FX window: every effect under share/raw/fx in a thumbnail grid (like the Models pane) next to a preview box. Hover or
// click plays the effect in the box through Radiant's own fx system: the effect lives far above the map and every box
// frame is a hidden camera frame (drawn, read back, never presented), so the 3D view never shows it. Double-click sets
// it on the selected fx entities, a drag onto the 3D view places it like the FX Browser does. Thumbnails are hidden
// frames too. Nothing is added to the map or the undo history.
#include "modtools_plugin.hpp"
#include "fxpreview.h"
#include "fxswap.h"
#include <windows.h>
#include <d3d11.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace br::fxpreview
{
namespace
{
    // IDA addresses (no ASLR), resolved through host->RadiantAddress
    const uintptr_t kEdHolderCtor    = 0x1404632C0;  // (holder): the 599544-byte editor def holder an fx class keeps at +400: int handle, then the def at +8
    const uintptr_t kFxLoadEfx       = 0x14013BF10;  // (const char* "dir/name", def): parses fx/<name>.efx into the def, 0 when missing
    const uintptr_t kFxLoadLights    = 0x1405B5740;  // (const char* name, holder): the dynamicLight blocks of the text into the holder
    const uintptr_t kFxFixSegments   = 0x1405B5830;  // (holder): segment light names after a load
    const uintptr_t kFxRegisterDef   = 0x14013C020;  // (int* out, def, char reload): compiles and registers the def, *out = handle (the name's string id), 0 on failure
    const uintptr_t kFxDefByHandle   = 0x14013B7F0;  // (int handle) -> compiled def: +24 looping life ms, +28 one-shot life ms, +40 float[3] half size, +52 float[3] centre
    const uintptr_t kFxSpawn         = 0x1405B2BD0;  // (int def, const float origin[3], const float axis[9], int delayMs, float timeScale) -> effect handle, 0 = failed
    const uintptr_t kFxKill          = 0x1405B29C0;  // (unsigned effect): kills the effect and its children
    const uintptr_t kFxEffectPtr     = 0x140160490;  // (int client, unsigned effect) -> the live effect or NULL
    const uintptr_t kFxSystemCount   = 0x1443900B0;  // int, local fx clients
    const uintptr_t kFxSystemPtr     = 0x1443900B8;  // client 0's fx system, +2292 = fx time ms
    const uintptr_t kRenderOption    = 0x1409BBAC0;  // (int bit) -> bool, bit 1 = Fx > Fx Rendering Enabled
    const uintptr_t kColorTable      = 0x1453A14A8;  // float[48][4], row 4 = COLOR_CAMERABACK (Cam_Draw clears with it)
    const uintptr_t kMainWin         = 0x1444C6560;  // RadiantMainWin*, +64 = RadiantQtBridge, bridge+240 = CCamWnd, bridge+1888 = EffectPlaybackMgr
    const uintptr_t kPlaybackMgrVtbl = 0x142310788;  // EffectPlaybackMgr's vtable, checked before the manager is trusted
    const uintptr_t kToolboxMeta     = 0x142466D00;  // QtAbstractToolbox::staticMetaObject: signals RecentItemsUpdated, ItemDoubleClicked, AddItemAtCameraLocation
    // Hidden frames: CCamWnd::OnRender's steps without its Present
    const uintptr_t kPlaybackUpdate  = 0x1404732E0;  // (mgr): EffectPlaybackMgr::Update, advances the fx clock by now - mgr+40
    const uintptr_t kNowSeconds      = 0x1453A1848;  // double, the idle tick's clock the manager reads
    const uintptr_t kViewSet         = 0x1411AED10;  // (int view) -> bool: makes `view` current when none is (the camera is view 0)
    const uintptr_t kViewIndex       = 0x1433B5740;  // int, the current view, 7 = none (Present sets it back)
    const uintptr_t kBrushCacheUpd   = 0x1402124B0;  // (): per-frame brush cache update
    const uintptr_t kPoolA           = 0x1411863C0;  // (): per-frame resource pool upkeep
    const uintptr_t kPoolB           = 0x1410AD280;
    const uintptr_t kBeginFrame      = 0x1411E7710;  // () -> bool, 0 when a frame is already open
    const uintptr_t kEndFrame        = 0x1411E7760;
    const uintptr_t kViewTarget      = 0x1411AEEA0;  // (int view) -> the colour target of the view
    const uintptr_t kViewDepth       = 0x1411AEFF0;  // (int view) -> its depth target
    const uintptr_t kSetTargets      = 0x141259200;  // (colour, depth, 0, 0)
    const uintptr_t kClear           = 0x141259590;  // (char flags 7, const float rgba[4], float depth)
    const uintptr_t kCamDraw         = 0x140244120;  // (CCamWnd*)
    const uintptr_t kViewSetup       = 0x140231770;  // (CCamWnd*, float out[14]): the view the fx step and Cam_Draw use
    const uintptr_t kDxDevice        = 0x1455740C0;  // ID3D11Device*
    const uintptr_t kDxContext       = 0x145574210;  // ID3D11DeviceContext*
    const uintptr_t kSwapChain0      = 0x145584A78;  // IDXGISwapChain* of view 0 (the camera)

    const size_t off_Bridge_CamWnd      = 240;
    const size_t off_Bridge_PlaybackMgr = 1888;
    const size_t off_Cam_Origin    = 208;   // float[3]
    const size_t off_Cam_Angles    = 220;   // float[3] pitch yaw roll
    const size_t off_Mgr_LastNow   = 40;    // double
    const size_t off_Mgr_Clock     = 48;    // double seconds, the fx clock
    const size_t off_Mgr_Paused    = 58;    // byte
    const size_t off_Sys_TimeMs    = 2292;
    const size_t off_Def_LoopCount = 12;    // u16 looping emitters
    const size_t off_Def_LoopLife  = 24;
    const size_t off_Def_ShotLife  = 28;
    const size_t off_Def_HalfSize  = 40;
    const size_t off_Def_Centre    = 52;
    const int    kCameraBackRow    = 4;
    const float  kNeutral[4]       = { 0.18f, 0.18f, 0.18f, 1.0f };
    const float  kStage[3]         = { 0, 0, 60000 };   // far above any map
    const size_t kHolderSize       = 599544;
    const int    kThumbSize        = 128;
    const double kThumbSeconds     = 1.0;
    const size_t kSearchCap        = 2000;
    const unsigned kBoxFrameMs     = 66;    // about 15 box frames a second
    const int    kEndlessWindowMs  = 10000; // the slider's span and Play all's dwell for an endless effect

    typedef void*    (*HolderCtorFn)(void* holder);
    typedef char     (*LoadEfxFn)(const char* name, void* def);
    typedef void     (*LoadLightsFn)(const char* name, void* holder);
    typedef void     (*FixSegmentsFn)(void* holder);
    typedef int*     (*RegisterDefFn)(int* out, void* def, char reload);
    typedef const char* (*DefByHandleFn)(int handle);
    typedef unsigned (*SpawnFn)(int def, const float* origin, const float* axis, int delayMs, float timeScale);
    typedef void     (*KillFn)(unsigned effect);
    typedef void*    (*EffectPtrFn)(int client, unsigned effect);
    typedef bool     (*RenderOptionFn)(int bit);
    typedef void     (*MgrUpdateFn)(void* mgr);
    typedef char     (*ViewSetFn)(int view);
    typedef void     (*VoidFn)();
    typedef char     (*BoolFn)();
    typedef void*    (*ViewGetFn)(int view);
    typedef void     (*SetTargetsFn)(void* colour, void* depth, int mip, unsigned slice);
    typedef void     (*ClearFn)(char flags, const float* rgba, float depth);
    typedef __int64  (*CamDrawFn)(void* cam);

    struct Radiant
    {
        HolderCtorFn holderCtor; LoadEfxFn loadEfx; LoadLightsFn loadLights; FixSegmentsFn fixSegments; RegisterDefFn registerDef;
        DefByHandleFn defByHandle; SpawnFn spawn; KillFn kill; EffectPtrFn effectPtr; RenderOptionFn renderOption;
        const int* sysCount; const char* const* sysPtr; float* colors; void** mainWin; void* mgrVtbl;
        MgrUpdateFn mgrUpdate; const double* now; ViewSetFn viewSet; int* viewIndex; VoidFn brushCacheUpd; VoidFn poolA; VoidFn poolB;
        BoolFn beginFrame; VoidFn endFrame; ViewGetFn viewTarget; ViewGetFn viewDepth; SetTargetsFn setTargets; ClearFn clear; CamDrawFn camDraw;
        ID3D11Device** dxDevice; ID3D11DeviceContext** dxContext; IDXGISwapChain** swapChain0;
    } rt = {};

    // Qt 5.3 exports (no headers): the icon grid, the preview box, the thumbnail images, the placement signal
    struct QArrayDataRaw { int ref; int size; unsigned alloc; ptrdiff_t offset; };
    struct QPointRaw { int x, y; };
    struct QSizeRaw { int w, h; };
    struct QRectRaw { int x1, y1, x2, y2; };
    struct GenericArg { const char* name; void* data; };
    struct Qt
    {
        void  (*setViewMode)(void* view, int mode);
        void  (*setIconSize)(void* view, const QSizeRaw* size);
        void  (*setGridSize)(void* view, const QSizeRaw* size);
        void  (*setResizeMode)(void* view, int mode);
        void  (*setMovement)(void* view, int movement);
        void  (*setWordWrap)(void* view, bool on);
        void  (*setUniformItemSizes)(void* view, bool on);
        void  (*setSpacing)(void* view, int px);
        void* (*item)(const void* list, int row);
        void  (*setItemIcon)(void* item, const void* icon);
        void* (*itemAt)(const void* list, const QPointRaw* pos);
        int   (*row)(const void* list, const void* item);
        void  (*setItemToolTip)(void* item, const void* qstring);
        QRectRaw* (*visualItemRect)(const void* list, QRectRaw* ret, const void* item);
        int   (*widgetHeight)(const void* widget);
        int   (*widgetWidth)(const void* widget);
        QPointRaw* (*mapFromGlobal)(const void* widget, QPointRaw* ret, const QPointRaw* pos);
        void  (*setStyleSheet)(void* widget, const void* qstring);
        void** (*objectName)(const void* obj, void** retQString);
        void  (*labelSetPixmap)(void* label, const void* pixmap);
        void* (*iconFromPixmap)(void* icon, const void* pixmap);
        void  (*iconDtor)(void* icon);
        void* (*pixmapFromFile)(void* pixmap, const void* qstring, const char* format, int flags);
        void* (*pixmapFromImage)(void* pixmap, const void* image, int flags);
        void  (*pixmapDtor)(void* pixmap);
        void* (*imageCtor)(void* img, const unsigned char* data, int w, int h, int bytesPerLine, int format, void* cleanup, void* info);
        void  (*imageDtor)(void* img);
        void* (*imageCopy)(const void* img, void* out, int x, int y, int w, int h);
        void* (*imageScaled)(const void* img, void* out, int w, int h, int aspect, int mode);
        bool  (*imageSave)(const void* img, const void* qstring, const char* format, int quality);
        void* (*stringCtor)(void* qs, const char* text);
        void  (*stringDtor)(void* qs);
        int   (*indexOfSignal)(const void* meta, const char* signature);
        int   (*methodOffset)(const void* meta);
        void  (*activate)(void* obj, const void* meta, int localSignal, void** argv);
        bool  (*invokeMethod)(void* obj, const char* member, int connection, const GenericArg* a0, const GenericArg* a1, const GenericArg* a2,
                              const GenericArg* a3, const GenericArg* a4, const GenericArg* a5, const GenericArg* a6, const GenericArg* a7, const GenericArg* a8, const GenericArg* a9);
    } qt = {};
    template <class T> bool Get(T& fn, HMODULE m, const char* name)
    {
        fn = m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
        return fn != nullptr;
    }
    template <class T> T At(uintptr_t ida) { return reinterpret_cast<T>(rp::host->RadiantAddress(rp::self, ida)); }

    struct Loaded { std::string name; void* holder; int handle; };
    std::vector<Loaded> g_loaded;       // editor defs kept alive while their compiled defs are registered

    struct Entry { std::string dir, name; };   // "fire", "fx_embers_burst": the effect is "fire/fx_embers_burst"
    std::vector<Entry> g_all;           // every .efx under share/raw/fx
    std::vector<int> g_rows;            // the grid: indices into g_all
    std::vector<char> g_rowState;       // per grid row: 0 not looked at, 1 icon set, 2 thumbnail queued or failed
    std::vector<int> g_queue;           // thumbnails to make, indices into g_all (the rows on screen go first)
    std::vector<int> g_failed;          // effects whose thumbnail failed, not tried again this session
    size_t g_queueDone = 0, g_queueTotal = 0;
    uint64_t g_nextVisibleMs = 0;
    bool g_scanned = false;
    std::wstring g_fxRoot, g_thumbRoot;

    bool g_enabled = false, g_ready = false, g_qtOk = false;
    // the effect in the preview box
    std::string g_name;                 // "dir/name"
    unsigned g_effect = 0;              // the live effect handle, 0 = none
    int g_def = 0;
    int g_startMs = 0;                  // fx time when the effect started (elapsed = fx time - this)
    int g_lengthMs = 3000;              // the effect's own life from its compiled def
    float g_boxCam[3] = { 0, 0, 0 };    // the hidden camera for the box, framed by the effect's bounds
    bool g_boxDirty = false;            // draw a frame even when the effect ended (the last look stays)
    uint64_t g_nextFrameMs = 0, g_lastStepMs = 0;
    unsigned g_boxFrames = 0, g_boxMisses = 0;
    bool g_thumbFrame = false;          // a hidden frame is being drawn: the map stays out
    bool g_loop = false;
    bool g_playAll = false;
    size_t g_playAllRow = 0;
    bool g_follow = true;               // FX Browser clicks preview
    bool g_hover = true;                // hovering a thumbnail previews it
    std::string g_hoverName;
    uint64_t g_hoverDue = 0;
    // a drag from the grid
    bool g_pressed = false, g_dragging = false;
    int g_pressX = 0, g_pressY = 0;
    std::string g_dragName;
    bool g_settingSlider = false;
    uint64_t g_nextLabelMs = 0;
    std::string g_lastLabel, g_lastStatus, g_lastPause, g_lastProgress;

    // widgets
    void* g_pane = nullptr;
    void* g_search = nullptr;
    void* g_grid = nullptr;
    void* g_gridViewport = nullptr;
    void* g_gridFilter = nullptr;
    void* g_box = nullptr;              // the preview QLabel
    void* g_progress = nullptr;
    void* g_edit = nullptr;
    void* g_pauseButton = nullptr;
    void* g_loopBox = nullptr;
    void* g_playAllBox = nullptr;
    void* g_slider = nullptr;
    void* g_elapsed = nullptr;
    void* g_status = nullptr;

    // ---- small helpers ----
    std::string Utf8(const wchar_t* w, int n)
    {
        int len = n > 0 ? WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr) : 0;
        std::string out(len > 0 ? len : 0, '\0');
        if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w, n, &out[0], len, nullptr, nullptr);
        return out;
    }
    std::wstring Wide(const std::string& s)
    {
        int len = s.empty() ? 0 : MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
        std::wstring out(len > 0 ? len : 0, L'\0');
        if (len > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), &out[0], len);
        return out;
    }
    std::string QStringText(void* d)
    {
        const QArrayDataRaw* a = static_cast<const QArrayDataRaw*>(d);
        if (!a || a->size <= 0) return std::string();
        return Utf8(reinterpret_cast<const wchar_t*>(reinterpret_cast<const char*>(a) + a->offset), a->size);
    }
    std::string Lower(std::string s) { for (char& c : s) c = char(tolower(static_cast<unsigned char>(c))); return s; }

    const char* FxSystem() { return rt.sysCount && rt.sysPtr && *rt.sysCount > 0 ? *rt.sysPtr : nullptr; }
    int FxTimeMs() { const char* s = FxSystem(); return s ? *reinterpret_cast<const int*>(s + off_Sys_TimeMs) : 0; }
    char* Bridge()
    {
        char* main = rt.mainWin ? static_cast<char*>(*rt.mainWin) : nullptr;
        return main ? *reinterpret_cast<char**>(main + 64) : nullptr;
    }
    char* PlaybackMgr()
    {
        char* bridge = Bridge();
        char* mgr = bridge ? *reinterpret_cast<char**>(bridge + off_Bridge_PlaybackMgr) : nullptr;
        return mgr && *reinterpret_cast<void**>(mgr) == rt.mgrVtbl ? mgr : nullptr;
    }
    char* CamWnd() { char* bridge = Bridge(); return bridge ? *reinterpret_cast<char**>(bridge + off_Bridge_CamWnd) : nullptr; }
    bool Paused() { char* m = PlaybackMgr(); return m && m[off_Mgr_Paused] != 0; }
    bool FxRenderingOn() { return rt.renderOption ? rt.renderOption(1) : true; }

    std::string CleanName(std::string fx)
    {
        for (char& ch : fx) if (ch == '\\') ch = '/';
        while (!fx.empty() && (fx.back() == ' ' || fx.back() == '\r' || fx.back() == '\n')) fx.pop_back();
        const size_t start = fx.find_first_not_of("/ ");
        fx.erase(0, start == std::string::npos ? fx.size() : start);
        if (fx.size() > 4 && _stricmp(fx.c_str() + fx.size() - 4, ".efx") == 0) fx.resize(fx.size() - 4);
        return fx;
    }
    std::string FullName(const Entry& e) { return e.dir.empty() ? e.name : e.dir + "/" + e.name; }
    std::wstring EfxPath(const Entry& e)
    {
        std::wstring d = Wide(e.dir);
        for (wchar_t& c : d) if (c == L'/') c = L'\\';
        return g_fxRoot + (d.empty() ? L"" : L"\\" + d) + L"\\" + Wide(e.name) + L".efx";
    }
    std::wstring ThumbPath(const Entry& e)
    {
        std::wstring p = g_thumbRoot;
        if (!e.dir.empty()) { std::wstring d = Wide(e.dir); for (wchar_t& c : d) if (c == L'/') c = L'\\'; p += L"\\" + d; }
        return p + L"\\" + Wide(e.name) + L".png";
    }
    bool FileTime(const std::wstring& p, uint64_t& out)
    {
        WIN32_FILE_ATTRIBUTE_DATA d;
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d) || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
        out = (uint64_t(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
        return true;
    }
    bool FileExists(const std::wstring& p) { uint64_t t; return FileTime(p, t); }
    // a thumbnail counts only when it is newer than its effect file
    bool ThumbCurrent(const Entry& e)
    {
        uint64_t png = 0, efx = 0;
        if (!FileTime(ThumbPath(e), png)) return false;
        return !FileTime(EfxPath(e), efx) || png >= efx;
    }
    void MakeDirs(const std::wstring& dir)
    {
        for (size_t i = 3; i <= dir.size(); ++i)
            if (i == dir.size() || dir[i] == L'\\') CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
    }

    std::string EditText(void* edit)
    {
        char buf[512] = {};
        if (edit && rp::host->ui) rp::host->ui->EditText(edit, buf, sizeof buf);
        return buf;
    }
    void SetLabel(void* label, std::string& last, const std::string& text)
    {
        if (!label || text == last) return;
        rp::host->ui->SetLabelText(label, text.c_str());
        last = text;
    }
    void SetStatus(const std::string& text) { SetLabel(g_status, g_lastStatus, text); }

    // ---- effects through Radiant's fx system ----
    // The compiled def handle of `name`, loading it the way an fx entity's class does: an editor def parsed from
    // fx/<name>.efx, then compiled and registered under the name. 0 when the effect does not exist.
    int LoadDef(const std::string& name)
    {
        for (const Loaded& l : g_loaded) if (l.name == name) return l.handle;
        void* holder = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, kHolderSize);
        if (!holder) return 0;
        rt.holderCtor(holder);
        char* def = static_cast<char*>(holder) + 8;
        if (!rt.loadEfx(name.c_str(), def)) { HeapFree(GetProcessHeap(), 0, holder); return 0; }
        rt.loadLights(name.c_str(), holder);
        rt.fixSegments(holder);
        int handle = 0;
        rt.registerDef(&handle, def, 1);
        *static_cast<int*>(holder) = handle;
        if (!handle) { HeapFree(GetProcessHeap(), 0, holder); return 0; }
        g_loaded.push_back({ name, holder, handle });
        return handle;
    }
    void ForgetDefs()
    {
        for (const Loaded& l : g_loaded) HeapFree(GetProcessHeap(), 0, l.holder);   // the compiled defs stay registered in Radiant
        g_loaded.clear();
    }

    // the effect's own life in ms (the longer of its looping and one-shot lives); 0 = endless (looping emitters without a life)
    int EffectLengthMs(int def)
    {
        const char* cdef = rt.defByHandle(def);
        if (!cdef) return 3000;
        const int loop = *reinterpret_cast<const int*>(cdef + off_Def_LoopLife), shot = *reinterpret_cast<const int*>(cdef + off_Def_ShotLife);
        const int loopingEmitters = *reinterpret_cast<const unsigned short*>(cdef + off_Def_LoopCount);
        const int len = loop > shot ? loop : shot;
        if (len > 0 && len < 600000) return len;
        return loopingEmitters > 0 ? 0 : 3000;
    }
    int SliderTopMs() { return g_lengthMs > 0 ? g_lengthMs : kEndlessWindowMs; }
    // The camera's axes are cached by Radiant (its view setup rebuilds them from mouse moves, not from the angles field),
    // so the stage camera keeps the user's heading and is placed behind the effect along it, looking at the effect's centre.
    void CameraForward(float forward[3])
    {
        typedef float* (*ViewSetupFn)(void* cam, float* out);
        ViewSetupFn setup = At<ViewSetupFn>(kViewSetup);
        float v[14] = { 0, 0, 0, 1, 0, 0 };
        if (setup && CamWnd()) setup(CamWnd(), v);
        const float len = sqrtf(v[3] * v[3] + v[4] * v[4] + v[5] * v[5]);
        if (len > 0.5f) { forward[0] = v[3] / len; forward[1] = v[4] / len; forward[2] = v[5] / len; }
        else { forward[0] = 1; forward[1] = 0; forward[2] = 0; }
    }
    // the hidden camera that frames the effect's bounds from the user's heading
    void FrameCamera(int def, float camAt[3])
    {
        const char* cdef = rt.defByHandle(def);
        float half[3] = { 100, 100, 100 }, centre[3] = { 0, 0, 0 };
        if (cdef) { memcpy(half, cdef + off_Def_HalfSize, sizeof half); memcpy(centre, cdef + off_Def_Centre, sizeof centre); }
        float extent = 0;
        for (float v : half) extent = v > extent ? v : extent;
        if (!(extent > 1.0f) || extent > 20000.0f) extent = 100.0f;
        const float dist = extent * 2.2f + 48.0f;
        float f[3];
        CameraForward(f);
        for (int i = 0; i < 3; ++i) camAt[i] = kStage[i] + centre[i] - f[i] * dist;
    }

    void Kill()
    {
        if (g_effect && rt.effectPtr(0, g_effect)) rt.kill(g_effect);
        g_effect = 0;
    }

    // ---- hidden frames ----
    // Advances Radiant's fx clock by `seconds` the way its idle tick does (one simulation step).
    void StepFx(double seconds)
    {
        char* m = PlaybackMgr();
        if (!m) return;
        *reinterpret_cast<double*>(m + off_Mgr_LastNow) = *rt.now - seconds;
        rt.mgrUpdate(m);
    }

    // Reads the camera view's back buffer as BGRA rows. False when the format is not 8-bit RGBA/BGRA.
    bool ReadBackBuffer(std::vector<unsigned char>& bgra, int& w, int& h)
    {
        IDXGISwapChain* sc = *rt.swapChain0;
        ID3D11Device* dev = *rt.dxDevice;
        ID3D11DeviceContext* ctx = *rt.dxContext;
        if (!sc || !dev || !ctx) return false;
        ID3D11Texture2D* back = nullptr;
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) return false;
        D3D11_TEXTURE2D_DESC desc = {};
        back->GetDesc(&desc);
        const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
        const bool isBgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
        bool ok = false;
        if (rgba || isBgra)
        {
            D3D11_TEXTURE2D_DESC st = desc;
            st.Usage = D3D11_USAGE_STAGING;
            st.BindFlags = 0;
            st.MiscFlags = 0;
            st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            st.SampleDesc.Count = 1;
            st.SampleDesc.Quality = 0;
            ID3D11Texture2D* staging = nullptr;
            if (SUCCEEDED(dev->CreateTexture2D(&st, nullptr, &staging)) && staging)
            {
                if (desc.SampleDesc.Count > 1) ctx->ResolveSubresource(staging, 0, back, 0, st.Format);
                else ctx->CopyResource(staging, back);
                D3D11_MAPPED_SUBRESOURCE map = {};
                if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map)))
                {
                    w = int(desc.Width); h = int(desc.Height);
                    bgra.resize(size_t(w) * h * 4);
                    for (int y = 0; y < h; ++y)
                    {
                        const unsigned char* src = static_cast<const unsigned char*>(map.pData) + size_t(y) * map.RowPitch;
                        unsigned char* dst = bgra.data() + size_t(y) * w * 4;
                        if (isBgra) memcpy(dst, src, size_t(w) * 4);
                        else for (int x = 0; x < w; ++x) { dst[x * 4] = src[x * 4 + 2]; dst[x * 4 + 1] = src[x * 4 + 1]; dst[x * 4 + 2] = src[x * 4]; dst[x * 4 + 3] = 255; }
                    }
                    ctx->Unmap(staging, 0);
                    ok = true;
                }
                staging->Release();
            }
        }
        else rp::Log("fxpreview: back buffer format %d is not 8-bit RGBA", int(desc.Format));
        back->Release();
        return ok;
    }

    // One camera frame drawn into the back buffer and never presented, its pixels in `out`.
    bool HiddenFrame(std::vector<unsigned char>& out, int& w, int& h)
    {
        void* cam = CamWnd();
        if (!cam || *rt.viewIndex != 7) return false;
        if (!rt.viewSet(0)) return false;
        bool ok = false;
        g_thumbFrame = true;
        rt.brushCacheUpd();
        rt.poolA();
        rt.poolB();
        if (rt.beginFrame())
        {
            rt.setTargets(rt.viewTarget(0), rt.viewDepth(0), 0, 0);
            rt.clear(7, kNeutral, 1.0f);
            rt.camDraw(cam);
            rt.endFrame();
            ok = ReadBackBuffer(out, w, h);
        }
        g_thumbFrame = false;
        *rt.viewIndex = 7;      // what Present would have done
        return ok;
    }

    // The fx system only simulates effects inside the frustum of the camera the step was given, so the stage effect is
    // stepped with the camera on the stage and the map's effects with the user's camera (Radiant's own step). The fx clock
    // (mgr+48) and the step's last-time (mgr+40) come back as they were, so the two steps never add up.
    bool g_dbgNoGameView = false, g_dbgNoClock = false;   // "fxpreview dbg ..." switches for isolating stage problems
    struct ClockGuard
    {
        char* m; double last, clock;
        ClockGuard() : m(g_dbgNoClock ? nullptr : PlaybackMgr()), last(0), clock(0)
        {
            if (m) { last = *reinterpret_cast<double*>(m + off_Mgr_LastNow); clock = *reinterpret_cast<double*>(m + off_Mgr_Clock); }
        }
        ~ClockGuard()
        {
            if (m) { *reinterpret_cast<double*>(m + off_Mgr_LastNow) = last; *reinterpret_cast<double*>(m + off_Mgr_Clock) = clock; }
        }
    };
    // the user's camera, game view and background colour, put back when the scope ends. Game view goes off because its
    // view setup does not take the camera from the origin and angles the editor view reads.
    struct StageView
    {
        char* cam; float origin[3], angles[3], back[4]; int gameView;
        explicit StageView(const float camAt[3]) : cam(CamWnd()), gameView(0)
        {
            memcpy(back, rt.colors + kCameraBackRow * 4, sizeof back);     // Cam_Draw clears with this row
            memcpy(rt.colors + kCameraBackRow * 4, kNeutral, sizeof kNeutral);
            if (!g_dbgNoGameView && rp::host->GameView && rp::host->SetGameView) { gameView = rp::host->GameView(rp::self); if (gameView) rp::host->SetGameView(rp::self, 0); }
            if (!cam) return;
            memcpy(origin, cam + off_Cam_Origin, sizeof origin);
            memcpy(angles, cam + off_Cam_Angles, sizeof angles);
            memcpy(cam + off_Cam_Origin, camAt, sizeof origin);     // the heading stays the user's (cached axes)
        }
        ~StageView()
        {
            memcpy(rt.colors + kCameraBackRow * 4, back, sizeof back);
            if (gameView) rp::host->SetGameView(rp::self, 1);
            if (!cam) return;
            memcpy(cam + off_Cam_Origin, origin, sizeof origin);
            memcpy(cam + off_Cam_Angles, angles, sizeof angles);
        }
    };

    // the centre square of a frame as a QImage (64-byte object buffer) scaled to `size`
    void CentreImage(const std::vector<unsigned char>& bgra, int w, int h, int size, char out[64])
    {
        const int side = h < w ? h : w;
        const int crop = int(side * 0.8);
        alignas(16) char img[64], cropped[64];
        qt.imageCtor(img, bgra.data(), w, h, w * 4, 4 /* RGB32 */, nullptr, nullptr);
        qt.imageCopy(img, cropped, (w - crop) / 2, (h - crop) / 2, crop, crop);
        qt.imageScaled(cropped, out, size, size, 1 /* KeepAspectRatio */, 1 /* Smooth */);
        qt.imageDtor(cropped);
        qt.imageDtor(img);
    }

    bool SavePng(const std::vector<unsigned char>& bgra, int w, int h, int size, const std::wstring& path)
    {
        if (w <= 0 || h <= 0) return false;
        alignas(16) char scaled[64], qs[16];
        CentreImage(bgra, w, h, size, scaled);
        qt.stringCtor(qs, Utf8(path.c_str(), int(path.size())).c_str());
        const bool ok = qt.imageSave(scaled, qs, "PNG", -1);
        qt.stringDtor(qs);
        qt.imageDtor(scaled);
        return ok;
    }

    void ShowInBox(const std::vector<unsigned char>& bgra, int w, int h)
    {
        if (!g_box || w <= 0 || h <= 0) return;
        int size = qt.widgetHeight(g_box);
        const int bw = qt.widgetWidth(g_box);
        if (bw < size) size = bw;
        if (size < 64) size = 64;
        alignas(16) char scaled[64], pix[64];
        CentreImage(bgra, w, h, size, scaled);
        qt.pixmapFromImage(pix, scaled, 0);
        qt.labelSetPixmap(g_box, pix);
        qt.pixmapDtor(pix);
        qt.imageDtor(scaled);
    }

    // ---- the preview box ----
    // Starts `name` far above the map, delayMs after now (negative = that far into its life), framed for the box.
    bool Start(const std::string& name, int delayMs, std::string& why)
    {
        if (!g_ready) { why = "FX preview is unavailable"; return false; }
        if (name.empty()) { why = "no effect name"; return false; }
        if (!FxSystem()) { why = "no fx system"; return false; }
        const int def = LoadDef(name);
        if (!def) { why = "effect '" + name + "' not found or unreadable (fx/" + name + ".efx)"; return false; }
        Kill();
        FrameCamera(def, g_boxCam);
        static const float axis[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        unsigned h = 0;
        {
            StageView view(g_boxCam);          // the fx system judges a new effect by the camera it is spawned under
            h = rt.spawn(def, kStage, axis, delayMs, 1.0f);
        }
        if (!h) { why = "the fx system refused the effect (too many effects, or fx rendering is off)"; return false; }
        g_effect = h;
        g_def = def;
        g_name = name;
        g_startMs = FxTimeMs() + delayMs;
        g_lengthMs = EffectLengthMs(def);
        g_boxDirty = true;
        g_nextFrameMs = 0;
        g_lastStepMs = GetTickCount64();
        if (g_slider) rp::host->ui->SliderSetRange(g_slider, 0, SliderTopMs() / 10);
        return true;
    }

    void Stop()
    {
        Kill();
        g_playAll = false;
        if (g_playAllBox) rp::host->ui->SetChecked(g_playAllBox, 0);
    }

    double ElapsedSeconds() { return g_effect ? double(FxTimeMs() - g_startMs) / 1000.0 : 0.0; }

    void TogglePause()
    {
        if (rp::host->TriggerMenuAction && rp::host->TriggerMenuAction(rp::self, "Fx/Pause FX Playback") == 1) return;
        if (char* m = PlaybackMgr()) m[off_Mgr_Paused] = m[off_Mgr_Paused] ? 0 : 1;   // the menu entry was not found: flip the flag itself
    }

    bool Preview(const std::string& raw)
    {
        const std::string name = CleanName(raw);
        std::string why;
        if (name.empty()) { SetStatus("No effect name."); return false; }
        if (g_edit) rp::host->ui->SetEditText(g_edit, name.c_str());
        if (!Start(name, 0, why)) { SetStatus(why); return false; }
        char buf[400];
        if (g_lengthMs > 0) snprintf(buf, sizeof buf, "%s (%.2f s)%s", name.c_str(), g_lengthMs / 1000.0, FxRenderingOn() ? "" : ", Fx > Fx Rendering Enabled is off");
        else snprintf(buf, sizeof buf, "%s (looping)%s", name.c_str(), FxRenderingOn() ? "" : ", Fx > Fx Rendering Enabled is off");
        SetStatus(buf);
        return true;
    }

    // one simulation step and one box frame now, then the end-of-life handling (loop, play all)
    void BoxTick(uint64_t now)
    {
        if (!g_effect && !g_boxDirty) return;
        if (now < g_nextFrameMs) return;
        double dt = g_lastStepMs ? double(now - g_lastStepMs) / 1000.0 : 0.0;
        if (dt > 0.1) dt = 0.1;
        g_lastStepMs = now;
        g_nextFrameMs = now + kBoxFrameMs;
        if (g_effect && !rt.effectPtr(0, g_effect)) g_effect = 0;      // the fx system dropped it (map load, limit)
        const bool alive = g_effect != 0;
        if (alive || g_boxDirty)
        {
            if (alive) FrameCamera(g_def, g_boxCam);      // the user may have turned the camera
            StageView view(g_boxCam);
            if (alive) { ClockGuard clock; StepFx(dt); }
            std::vector<unsigned char> pixels;
            int w = 0, h = 0;
            if (HiddenFrame(pixels, w, h)) { ShowInBox(pixels, w, h); ++g_boxFrames; }
            else ++g_boxMisses;
            g_boxDirty = false;
        }
        if (!alive || Paused()) return;
        const int elapsed = FxTimeMs() - g_startMs;
        if (g_playAll && !g_rows.empty())
        {
            if (elapsed < (g_lengthMs > 0 ? g_lengthMs : kEndlessWindowMs)) return;
            g_playAllRow = (g_playAllRow + 1) % g_rows.size();
            Preview(FullName(g_all[size_t(g_rows[g_playAllRow])]));
            return;
        }
        if (g_lengthMs <= 0 || elapsed < g_lengthMs) return;   // endless effects play until Stop or the next pick
        std::string why;
        if (g_loop) Start(g_name, 0, why);
        else Kill();                                            // the last frame stays in the box
    }

    // Renders `name` hidden and writes the busiest of three looks over its first seconds as the thumbnail.
    bool Thumbnail(const std::string& name, const std::wstring& path, std::string& why)
    {
        if (!g_ready) { why = "unavailable"; return false; }
        const int def = LoadDef(name);
        if (!def) { why = "effect '" + name + "' not found or unreadable"; return false; }
        char* m = PlaybackMgr();
        if (!m) { why = "no playback manager"; return false; }
        float camAt[3];
        FrameCamera(def, camAt);
        const unsigned savedEffect = g_effect;
        if (savedEffect) rt.kill(savedEffect);                                   // the box effect stays out of the thumbnail
        g_effect = 0;
        const char paused = m[off_Mgr_Paused];
        m[off_Mgr_Paused] = 0;
        static const float axis[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        bool ok = false;
        {
            StageView view(camAt);             // in place before the spawn: the fx system judges a new effect by the camera
            ClockGuard clock;
            const unsigned h = rt.spawn(def, kStage, axis, 0, 1.0f);
            const double at[3] = { kThumbSeconds * 0.35, kThumbSeconds, kThumbSeconds * 1.8 }, step = 0.05;
            std::vector<unsigned char> best, pixels;
            int w = 0, hh = 0, bestW = 0, bestH = 0;
            size_t bestScore = 0;
            double t = 0;
            if (!h) why = "the fx system refused the effect";
            else for (int i = 0; i < 3; ++i)
            {
                for (; t < at[i]; t += step) StepFx(step);
                if (!HiddenFrame(pixels, w, hh)) { why = "hidden frame failed"; break; }
                size_t score = 0;
                for (size_t p = 0; p + 3 < pixels.size(); p += 16)    // pixels that are not the plain background
                    if (abs(int(pixels[p]) - 46) + abs(int(pixels[p + 1]) - 46) + abs(int(pixels[p + 2]) - 46) > 40) ++score;
                if (score >= bestScore) { bestScore = score; best.swap(pixels); bestW = w; bestH = hh; }
            }
            if (!best.empty())
            {
                MakeDirs(path.substr(0, path.find_last_of(L'\\')));
                ok = SavePng(best, bestW, bestH, kThumbSize, path);
                if (!ok) why = "could not write " + Utf8(path.c_str(), int(path.size()));
            }
            if (h) rt.kill(h);
        }
        m[off_Mgr_Paused] = paused;
        if (savedEffect) { std::string ignored; Start(g_name, -int(ElapsedSeconds() * 1000.0), ignored); }
        return ok;
    }

    // ---- the fx tree ----
    void ScanDir(const std::wstring& dir, const std::string& rel, int depth)
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do
        {
            if (fd.cFileName[0] == L'.') continue;
            const std::string name = Utf8(fd.cFileName, int(wcslen(fd.cFileName)));
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (depth < 12) ScanDir(dir + L"\\" + fd.cFileName, rel.empty() ? name : rel + "/" + name, depth + 1);
            }
            else if (name.size() > 4 && _stricmp(name.c_str() + name.size() - 4, ".efx") == 0)
                g_all.push_back({ rel, name.substr(0, name.size() - 4) });
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    void ScanTree()
    {
        g_all.clear();
        ScanDir(g_fxRoot, "", 0);
        std::sort(g_all.begin(), g_all.end(), [](const Entry& a, const Entry& b) { return a.dir != b.dir ? a.dir < b.dir : a.name < b.name; });
        g_scanned = true;
        rp::Log("fxpreview: %zu effects under %s", g_all.size(), Utf8(g_fxRoot.c_str(), int(g_fxRoot.size())).c_str());
    }

    // ---- the grid ----
    std::wstring PlaceholderPath()
    {
        const std::wstring p = g_thumbRoot + L"\\_unreadable.png";
        if (FileExists(p)) return p;
        const int n = kThumbSize;
        std::vector<unsigned char> px(size_t(n) * n * 4);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
            {
                unsigned char* d = &px[(size_t(y) * n + x) * 4];
                const bool cross = abs(x - y) < 3 || abs(x + y - (n - 1)) < 3;
                d[0] = cross ? 60 : 36; d[1] = cross ? 60 : 36; d[2] = cross ? 160 : 36; d[3] = 255;
            }
        MakeDirs(g_thumbRoot);
        SavePng(px, n, n, n, p);
        return p;
    }

    void SetRowIcon(int row, const Entry& e, bool placeholder = false)
    {
        if (!g_grid || !g_qtOk) return;
        const std::wstring p = placeholder ? PlaceholderPath() : ThumbPath(e);
        if (!FileExists(p)) return;
        void* item = qt.item(g_grid, row);
        if (!item) return;
        alignas(16) char pix[64], icon[16], qs[16];
        qt.stringCtor(qs, Utf8(p.c_str(), int(p.size())).c_str());
        qt.pixmapFromFile(pix, qs, nullptr, 0);
        qt.iconFromPixmap(icon, pix);
        qt.setItemIcon(item, icon);
        qt.iconDtor(icon);
        qt.pixmapDtor(pix);
        qt.stringDtor(qs);
    }

    // the rows only get text here, icons follow as rows come on screen (the Tick)
    void FillGrid(const std::vector<int>& rows, const std::string& title)
    {
        const ModToolsUiApi* ui = rp::host->ui;
        g_rows = rows;
        g_rowState.assign(g_rows.size(), 0);
        g_playAllRow = 0;
        ui->ListClear(g_grid);
        for (size_t i = 0; i < g_rows.size(); ++i)
        {
            const Entry& e = g_all[size_t(g_rows[i])];
            ui->ListAdd(g_grid, e.name.c_str());
            if (g_qtOk)
            {
                void* item = qt.item(g_grid, int(i));
                alignas(16) char qs[16];
                qt.stringCtor(qs, FullName(e).c_str());
                if (item) qt.setItemToolTip(item, qs);
                qt.stringDtor(qs);
            }
        }
        char buf[256];
        snprintf(buf, sizeof buf, "%s: %zu effect%s", title.c_str(), g_rows.size(), g_rows.size() == 1 ? "" : "s");
        SetStatus(buf);
        g_nextVisibleMs = 0;
    }

    void ShowAll()
    {
        std::vector<int> rows;
        rows.reserve(g_all.size());
        for (size_t i = 0; i < g_all.size(); ++i) rows.push_back(int(i));
        FillGrid(rows, "fx");
    }

    void ShowSearch(const std::string& text)
    {
        std::vector<std::string> words;
        size_t start = 0;
        const std::string lower = Lower(text);
        while (start < lower.size())
        {
            size_t end = lower.find(' ', start);
            if (end == std::string::npos) end = lower.size();
            if (end > start) words.push_back(lower.substr(start, end - start));
            start = end + 1;
        }
        if (words.empty()) { ShowAll(); return; }
        std::vector<int> rows;
        for (size_t i = 0; i < g_all.size() && rows.size() < kSearchCap; ++i)
        {
            std::string hay = Lower(FullName(g_all[i]));
            for (char& c : hay) if (c == '_') c = ' ';
            bool all = true;
            for (const std::string& w : words) if (hay.find(w) == std::string::npos) { all = false; break; }
            if (all) rows.push_back(int(i));
        }
        FillGrid(rows, rows.size() >= kSearchCap ? "Search (first " + std::to_string(kSearchCap) + ")" : "Search");
    }

    // the grid rows on screen, by the item under the top-left corner and the item rectangles after it
    std::vector<int> VisibleRows()
    {
        std::vector<int> out;
        if (!g_qtOk || !g_grid || !g_gridViewport || g_rows.empty()) return out;
        const int vh = qt.widgetHeight(g_gridViewport);
        int first = -1;
        for (int probeY = 2; probeY < vh && first < 0; probeY += kThumbSize / 3)
        {
            const QPointRaw p = { kThumbSize / 2, probeY };
            void* item = qt.itemAt(g_grid, &p);
            if (item) first = qt.row(g_grid, item);
        }
        if (first < 0) return out;
        for (int r = first; r < int(g_rows.size()) && out.size() < 400; ++r)
        {
            void* item = qt.item(g_grid, r);
            if (!item) break;
            QRectRaw rc = {};
            qt.visualItemRect(g_grid, &rc, item);
            if (rc.y1 > vh) break;
            out.push_back(r);
        }
        return out;
    }

    // icons for the rows on screen, thumbnails queued (first) for those without a current one
    void RefreshVisible()
    {
        const std::vector<int> rows = VisibleRows();
        size_t inserted = 0;
        for (int r : rows)
        {
            if (size_t(r) >= g_rowState.size() || g_rowState[size_t(r)]) continue;
            const int idx = g_rows[size_t(r)];
            const Entry& e = g_all[size_t(idx)];
            if (ThumbCurrent(e)) { SetRowIcon(r, e); g_rowState[size_t(r)] = 1; continue; }
            g_rowState[size_t(r)] = 2;
            if (std::find(g_failed.begin(), g_failed.end(), idx) != g_failed.end()) { SetRowIcon(r, e, true); g_rowState[size_t(r)] = 1; continue; }
            if (std::find(g_queue.begin(), g_queue.end(), idx) != g_queue.end()) continue;
            g_queue.insert(g_queue.begin() + inserted++, idx);
            ++g_queueTotal;
        }
    }

    void Progress()
    {
        if (!g_progress) return;
        char buf[128];
        if (g_queueTotal > 4 && !g_queue.empty()) snprintf(buf, sizeof buf, "Thumbnails: %zu of %zu", g_queueDone, g_queueTotal);
        else buf[0] = 0;
        SetLabel(g_progress, g_lastProgress, buf);
    }

    void Enqueue(const std::vector<int>& rows)
    {
        for (int r : rows)
            if (!ThumbCurrent(g_all[size_t(r)]) && std::find(g_failed.begin(), g_failed.end(), r) == g_failed.end()
                && std::find(g_queue.begin(), g_queue.end(), r) == g_queue.end()) g_queue.push_back(r);
        g_queueTotal = g_queue.size() + g_queueDone;
        Progress();
    }

    // makes a few queued thumbnails, within `budgetMs`, and refreshes the rows on show
    void WorkQueue(unsigned budgetMs)
    {
        const uint64_t until = GetTickCount64() + budgetMs;
        while (!g_queue.empty() && GetTickCount64() < until)
        {
            const int idx = g_queue.front();
            g_queue.erase(g_queue.begin());
            const Entry& e = g_all[size_t(idx)];
            std::string why;
            const bool ok = Thumbnail(FullName(e), ThumbPath(e), why);
            if (!ok) { g_failed.push_back(idx); rp::Log("fxpreview: thumbnail of %s failed: %s", FullName(e).c_str(), why.c_str()); }
            ++g_queueDone;
            for (size_t r = 0; r < g_rows.size(); ++r) if (g_rows[r] == idx) { SetRowIcon(int(r), e, !ok); if (r < g_rowState.size()) g_rowState[r] = 1; break; }
        }
        if (g_queue.empty()) { g_queueDone = g_queueTotal = 0; }
        Progress();
    }

    // places `name` where the mouse is over the 3D view by emitting the FX Browser's own AddItemAtCameraLocation(QString)
    bool PlaceAtCamera(const std::string& name)
    {
        void* browser = br::fxswap::Browser();
        if (!browser || !g_qtOk) { SetStatus("FX Browser not found."); return false; }
        alignas(16) char qs[16];
        qt.stringCtor(qs, ("/" + name).c_str());
        GenericArg a0 = { "QString", qs }, none = { nullptr, nullptr };
        bool ok = qt.invokeMethod(browser, "AddItemAtCameraLocation", 1 /* DirectConnection */, &a0, &none, &none, &none, &none, &none, &none, &none, &none, &none);
        if (!ok)
        {
            // by index: the signal sits on QtAbstractToolbox (moc metaObject 0x142466D00), activate wants its local index
            const void* meta = reinterpret_cast<const void* (*)(void*)>((*static_cast<void***>(browser))[0])(browser);   // QObject::metaObject
            const void* owner = At<const void*>(kToolboxMeta);
            const int abs = meta ? qt.indexOfSignal(meta, "AddItemAtCameraLocation(QString)") : -1;
            if (abs >= 0 && owner)
            {
                void* argv[2] = { nullptr, qs };
                qt.activate(browser, owner, abs - qt.methodOffset(owner), argv);
                ok = true;
            }
            else rp::Log("fxpreview: AddItemAtCameraLocation not found on the FX Browser (index %d)", abs);
        }
        qt.stringDtor(qs);
        return ok;
    }

    // ---- pane callbacks ----
    void OnPreview(void*) { Preview(EditText(g_edit)); }
    void OnRestart(void*)
    {
        if (g_name.empty()) { OnPreview(nullptr); return; }
        std::string why;
        if (!Start(g_name, 0, why)) SetStatus(why);
    }
    void OnStop(void*) { Stop(); SetStatus("Stopped."); }
    void OnPause(void*) { TogglePause(); }
    void OnRefresh(void*)
    {
        ScanTree();
        ForgetDefs();
        g_failed.clear();
        g_queue.clear();
        g_queueDone = g_queueTotal = 0;
        ShowSearch(EditText(g_search));
        SetStatus("Refreshed: " + std::to_string(g_all.size()) + " effects, changed effects get new thumbnails as they show.");
    }
    void OnLoopBox(void*, int v) { g_loop = v != 0; }
    void OnPlayAllBox(void*, int v)
    {
        g_playAll = v != 0;
        if (g_playAll && !g_rows.empty())
        {
            g_playAllRow = 0;
            for (size_t r = 0; r < g_rows.size(); ++r) if (FullName(g_all[size_t(g_rows[r])]) == g_name) { g_playAllRow = r; break; }
            if (!g_effect) Preview(FullName(g_all[size_t(g_rows[g_playAllRow])]));
        }
    }
    void OnFollowBox(void*, int v) { g_follow = v != 0; }
    void OnHoverBox(void*, int v) { g_hover = v != 0; }
    void OnSlider(void*, int v)
    {
        if (g_settingSlider || g_name.empty()) return;
        std::string why;
        if (!Start(g_name, -v * 10, why)) SetStatus(why);
    }
    void OnSearch(void*, int) { ShowSearch(EditText(g_search)); }
    void OnGridRow(void*, int row)
    {
        if (row < 0 || size_t(row) >= g_rows.size()) return;
        Preview(FullName(g_all[size_t(g_rows[size_t(row)])]));
    }

    std::string NameAtCursor()
    {
        if (!g_grid || !g_gridViewport || !g_qtOk) return std::string();
        const DWORD pos = GetMessagePos();
        QPointRaw global = { short(LOWORD(pos)), short(HIWORD(pos)) }, local = {};
        qt.mapFromGlobal(g_gridViewport, &local, &global);
        void* item = qt.itemAt(g_grid, &local);
        if (!item) return std::string();
        const int r = qt.row(g_grid, item);
        return r >= 0 && size_t(r) < g_rows.size() ? FullName(g_all[size_t(g_rows[size_t(r)])]) : std::string();
    }
    bool CursorOverCamera()
    {
        HWND cam = static_cast<HWND>(rp::host->CamWindow ? rp::host->CamWindow(rp::self) : nullptr);
        if (!cam) return false;
        POINT pt;
        GetCursorPos(&pt);
        HWND under = WindowFromPoint(pt);
        return under && (under == cam || IsChild(cam, under) || IsChild(under, cam));
    }
    int OnGridEvent(void*, ModToolsWidget watched, int type)
    {
        if (watched != g_gridViewport) return 0;
        const DWORD pos = GetMessagePos();
        const int x = short(LOWORD(pos)), y = short(HIWORD(pos));
        if (type == 2)                     // MouseButtonPress: a drag may start here
        {
            g_pressed = (GetKeyState(VK_LBUTTON) & 0x8000) != 0;
            g_dragging = false;
            g_pressX = x; g_pressY = y;
            g_dragName = g_pressed ? NameAtCursor() : std::string();
        }
        else if (type == 5)                // MouseMove
        {
            if (g_pressed && !g_dragName.empty() && (abs(x - g_pressX) > GetSystemMetrics(SM_CXDRAG) || abs(y - g_pressY) > GetSystemMetrics(SM_CYDRAG)))
                g_dragging = true;
            if (g_hover && !g_dragging)    // a short dwell, then the effect plays in the box
            {
                const std::string name = NameAtCursor();
                if (name != g_hoverName) { g_hoverName = name; g_hoverDue = GetTickCount64() + 150; }
            }
        }
        else if (type == 3)                // MouseButtonRelease: a drag that ends over the 3D view places the effect
        {
            if (g_dragging && CursorOverCamera())
            {
                SetStatus(PlaceAtCamera(g_dragName) ? "Placed " + g_dragName + " in the map." : "Could not place " + g_dragName + " (FX Browser not found).");
            }
            g_pressed = g_dragging = false;
        }
        else if (type == 11) g_hoverName.clear();      // Leave
        else if (type == 4)                            // double-click: the selected fx entities get it
        {
            const std::string name = NameAtCursor();
            if (name.empty()) return 0;
            const int n = br::fxswap::ApplyToSelection(name);
            char buf[320];
            snprintf(buf, sizeof buf, n > 0 ? "%s set on %d selected fx entit%s." : n == 0 ? "%s: select fx entities first (double-click sets their effect, a drag into the 3D view places a new one)." : "%s: not applied (%d).",
                     name.c_str(), n, n == 1 ? "y" : "ies");
            SetStatus(buf);
        }
        return 0;
    }

    void* FindViewport(void* list)
    {
        for (int i = 0; i < 64; ++i)
        {
            void* w = rp::host->ui->FindChild(list, "QWidget", i);
            if (!w) break;
            void* qs = nullptr;
            qt.objectName(w, &qs);
            const std::string n = QStringText(qs);
            qt.stringDtor(&qs);
            if (n == "qt_scrollarea_viewport") return w;
        }
        return nullptr;
    }

    bool BindQt()
    {
        HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll"), g = GetModuleHandleW(L"Qt5Gui64r.dll"), c = GetModuleHandleW(L"Qt5Core64r.dll");
        return Get(qt.setViewMode, w, "?setViewMode@QListView@@QEAAXW4ViewMode@1@@Z")
            && Get(qt.setIconSize, w, "?setIconSize@QAbstractItemView@@QEAAXAEBVQSize@@@Z")
            && Get(qt.setGridSize, w, "?setGridSize@QListView@@QEAAXAEBVQSize@@@Z")
            && Get(qt.setResizeMode, w, "?setResizeMode@QListView@@QEAAXW4ResizeMode@1@@Z")
            && Get(qt.setMovement, w, "?setMovement@QListView@@QEAAXW4Movement@1@@Z")
            && Get(qt.setWordWrap, w, "?setWordWrap@QListView@@QEAAX_N@Z")
            && Get(qt.setUniformItemSizes, w, "?setUniformItemSizes@QListView@@QEAAX_N@Z")
            && Get(qt.setSpacing, w, "?setSpacing@QListView@@QEAAXH@Z")
            && Get(qt.item, w, "?item@QListWidget@@QEBAPEAVQListWidgetItem@@H@Z")
            && Get(qt.setItemIcon, w, "?setIcon@QListWidgetItem@@QEAAXAEBVQIcon@@@Z")
            && Get(qt.itemAt, w, "?itemAt@QListWidget@@QEBAPEAVQListWidgetItem@@AEBVQPoint@@@Z")
            && Get(qt.row, w, "?row@QListWidget@@QEBAHPEBVQListWidgetItem@@@Z")
            && Get(qt.setItemToolTip, w, "?setToolTip@QListWidgetItem@@QEAAXAEBVQString@@@Z")
            && Get(qt.visualItemRect, w, "?visualItemRect@QListWidget@@QEBA?AVQRect@@PEBVQListWidgetItem@@@Z")
            && Get(qt.widgetHeight, w, "?height@QWidget@@QEBAHXZ")
            && Get(qt.widgetWidth, w, "?width@QWidget@@QEBAHXZ")
            && Get(qt.mapFromGlobal, w, "?mapFromGlobal@QWidget@@QEBA?AVQPoint@@AEBV2@@Z")
            && Get(qt.setStyleSheet, w, "?setStyleSheet@QWidget@@QEAAXAEBVQString@@@Z")
            && Get(qt.labelSetPixmap, w, "?setPixmap@QLabel@@QEAAXAEBVQPixmap@@@Z")
            && Get(qt.objectName, c, "?objectName@QObject@@QEBA?AVQString@@XZ")
            && Get(qt.invokeMethod, c, "?invokeMethod@QMetaObject@@SA_NPEAVQObject@@PEBDW4ConnectionType@Qt@@VQGenericArgument@@333333333@Z")
            && Get(qt.indexOfSignal, c, "?indexOfSignal@QMetaObject@@QEBAHPEBD@Z")
            && Get(qt.methodOffset, c, "?methodOffset@QMetaObject@@QEBAHXZ")
            && Get(qt.activate, c, "?activate@QMetaObject@@SAXPEAVQObject@@PEBU1@HPEAPEAX@Z")
            && Get(qt.iconFromPixmap, g, "??0QIcon@@QEAA@AEBVQPixmap@@@Z")
            && Get(qt.iconDtor, g, "??1QIcon@@QEAA@XZ")
            && Get(qt.pixmapFromFile, g, "??0QPixmap@@QEAA@AEBVQString@@PEBDV?$QFlags@W4ImageConversionFlag@Qt@@@@@Z")
            && Get(qt.pixmapFromImage, g, "?fromImage@QPixmap@@SA?AV1@AEBVQImage@@V?$QFlags@W4ImageConversionFlag@Qt@@@@@Z")
            && Get(qt.pixmapDtor, g, "??1QPixmap@@UEAA@XZ")
            && Get(qt.imageCtor, g, "??0QImage@@QEAA@PEBEHHHW4Format@0@P6AXPEAX@Z2@Z")
            && Get(qt.imageDtor, g, "??1QImage@@UEAA@XZ")
            && Get(qt.imageCopy, g, "?copy@QImage@@QEBA?AV1@HHHH@Z")
            && Get(qt.imageScaled, g, "?scaled@QImage@@QEBA?AV1@HHW4AspectRatioMode@Qt@@W4TransformationMode@3@@Z")
            && Get(qt.imageSave, g, "?save@QImage@@QEBA_NAEBVQString@@PEBDH@Z")
            && Get(qt.stringCtor, c, "??0QString@@QEAA@PEBD@Z")
            && Get(qt.stringDtor, c, "??1QString@@QEAA@XZ");
    }

    void BuildPane()
    {
        const ModToolsUiApi* ui = rp::host->ui;
        if (!ui || !g_qtOk || !rp::Has(&rp::host->CreatePane) || !rp::Has(&rp::host->OnChanged) || !rp::HasUi(&ui->Slider)
            || !rp::HasUi(&ui->Splitter) || !rp::HasUi(&ui->FindChild) || !rp::HasUi(&ui->SetToolTip) || !rp::Has(&rp::host->MakeEventFilter)) return;
        void* root = ui->Widget(nullptr);
        void* col = ui->VBox(root);
        ui->LayoutMargins(col, 4, 4, 4, 4);
        ui->LayoutSpacing(col, 3);

        void* row = ui->HBox();
        g_search = ui->LineEdit("", root);
        ui->SetPlaceholder(g_search, "Search every effect (words in any order, Enter)");
        ui->BoxAddWidget(row, g_search, 1);
        void* refresh = ui->PushButton("Refresh", root);
        ui->SetToolTip(refresh, "Read the fx folder again: new effects appear, changed effects get new thumbnails");
        ui->BoxAddWidget(row, refresh, 0);
        ui->BoxAddLayout(col, row, 0);

        void* split = ui->Splitter(1, root);
        g_grid = ui->ListWidget(root);
        const QSizeRaw icon = { kThumbSize, kThumbSize }, cell = { kThumbSize + 12, kThumbSize + 36 };
        qt.setViewMode(g_grid, 1);          // IconMode
        qt.setMovement(g_grid, 0);          // Static
        qt.setResizeMode(g_grid, 1);        // Adjust
        qt.setIconSize(g_grid, &icon);
        qt.setGridSize(g_grid, &cell);
        qt.setUniformItemSizes(g_grid, true);
        qt.setWordWrap(g_grid, true);
        qt.setSpacing(g_grid, 2);
        // the editor stylesheets style QAbstractItemView::item, which squeezes icon-mode cells: give the cells their own size
        char sheet[256];
        snprintf(sheet, sizeof sheet, "QListView { icon-size: %dpx; } QListView::item { width: %dpx; height: %dpx; padding: 2px; margin: 0px; border: none; }",
                 kThumbSize, cell.w - 4, cell.h - 4);
        alignas(16) char qs[16];
        qt.stringCtor(qs, sheet);
        qt.setStyleSheet(g_grid, qs);
        qt.stringDtor(qs);
        ui->SplitterAdd(split, g_grid, 3);
        void* right = ui->Widget(root);
        void* rcol = ui->VBox(right);
        ui->LayoutMargins(rcol, 0, 0, 0, 0);
        ui->LayoutSpacing(rcol, 3);
        g_box = ui->Label("", right);
        ui->SetMinimumSize(g_box, 240, 240);
        ui->SetToolTip(g_box, "The effect plays here, drawn by Radiant's fx system without touching the 3D view");
        ui->BoxAddWidget(rcol, g_box, 1);
        ui->SetLayout(right, rcol);
        ui->SplitterAdd(split, right, 1);
        ui->BoxAddWidget(col, split, 1);

        g_progress = ui->Label("", root);
        ui->BoxAddWidget(col, g_progress, 0);

        row = ui->HBox();
        ui->BoxAddWidget(row, ui->Label("Effect", root), 0);
        g_edit = ui->LineEdit("", root);
        ui->SetPlaceholder(g_edit, "dir/name");
        ui->BoxAddWidget(row, g_edit, 1);
        void* preview = ui->PushButton("Preview", root);
        ui->BoxAddWidget(row, preview, 0);
        g_pauseButton = ui->PushButton("Pause", root);
        ui->SetToolTip(g_pauseButton, "Radiant's own FX playback pause (the FX Playback toolbar), it pauses the map's effects too");
        ui->BoxAddWidget(row, g_pauseButton, 0);
        void* restart = ui->PushButton("Restart", root);
        ui->BoxAddWidget(row, restart, 0);
        void* stop = ui->PushButton("Stop", root);
        ui->BoxAddWidget(row, stop, 0);
        ui->BoxAddLayout(col, row, 0);

        row = ui->HBox();
        g_slider = ui->Slider(1, 0, 300, root);
        ui->SetToolTip(g_slider, "Where the effect is in its life (drag to jump, it restarts that far in)");
        ui->BoxAddWidget(row, g_slider, 1);
        g_elapsed = ui->Label("0.00 / 0.00 s", root);
        ui->BoxAddWidget(row, g_elapsed, 0);
        ui->BoxAddLayout(col, row, 0);

        row = ui->HBox();
        g_loopBox = ui->CheckBox("Loop", root);
        ui->SetToolTip(g_loopBox, "Restart the effect when it reaches the end of its life");
        ui->BoxAddWidget(row, g_loopBox, 0);
        g_playAllBox = ui->CheckBox("Play all", root);
        ui->SetToolTip(g_playAllBox, "Play every effect in the grid one after another, each for its own length");
        ui->BoxAddWidget(row, g_playAllBox, 0);
        void* hoverBox = ui->CheckBox("Hover plays", root);
        ui->SetChecked(hoverBox, 1);
        ui->SetToolTip(hoverBox, "Resting the mouse on a thumbnail plays that effect");
        ui->BoxAddWidget(row, hoverBox, 0);
        void* followBox = ui->CheckBox("Follow the FX Browser", root);
        ui->SetChecked(followBox, 1);
        ui->SetToolTip(followBox, "A click on an effect in Radiant's FX Browser previews it too");
        ui->BoxAddWidget(row, followBox, 0);
        ui->BoxAddStretch(row, 1);
        ui->BoxAddLayout(col, row, 0);

        g_status = ui->Label("Hover or click an effect to play it. Drag one into the 3D view to place it, double-click to set it on the selected fx entities.", root);
        ui->SetWordWrap(g_status, 1);
        ui->BoxAddWidget(col, g_status, 0);
        ui->SetLayout(root, col);

        rp::host->OnClicked(rp::self, preview, OnPreview, nullptr);
        rp::host->OnClicked(rp::self, g_pauseButton, OnPause, nullptr);
        rp::host->OnClicked(rp::self, restart, OnRestart, nullptr);
        rp::host->OnClicked(rp::self, stop, OnStop, nullptr);
        rp::host->OnClicked(rp::self, refresh, OnRefresh, nullptr);
        rp::host->OnChanged(rp::self, g_loopBox, OnLoopBox, nullptr);
        rp::host->OnChanged(rp::self, g_playAllBox, OnPlayAllBox, nullptr);
        rp::host->OnChanged(rp::self, followBox, OnFollowBox, nullptr);
        rp::host->OnChanged(rp::self, hoverBox, OnHoverBox, nullptr);
        rp::host->OnChanged(rp::self, g_slider, OnSlider, nullptr);
        rp::host->OnChanged(rp::self, g_search, OnSearch, nullptr);
        rp::host->OnChanged(rp::self, g_grid, OnGridRow, nullptr);
        g_gridViewport = FindViewport(g_grid);
        g_gridFilter = rp::host->MakeEventFilter(rp::self, OnGridEvent, nullptr);
        if (g_gridViewport && g_gridFilter) ui->InstallFilter(g_gridViewport, g_gridFilter);

        g_pane = rp::host->CreatePane(rp::self, "FX", root, 900, 520);
        if (g_pane && rp::host->AddMenu && rp::host->PaneToggleAction && rp::host->AddMenuAction)
            if (void* menu = rp::host->AddMenu(rp::self, "FX Window"))
                rp::host->AddMenuAction(rp::self, menu, rp::host->PaneToggleAction(rp::self, g_pane));
    }

    std::string Status()
    {
        char buf[640];
        snprintf(buf, sizeof buf, "fxpreview: %s, effect '%s' handle %u elapsed %.2f of %.2f s, paused %d, fx rendering %d, mgr %s, fx time %d ms, %zu effects, grid %zu rows, queue %zu, loop %d, play all %d, box frames %u misses %u, box %dx%d\n",
                 !g_enabled ? "off ([fxpreview] enabled=0)" : g_ready ? "ready" : "unavailable", g_name.c_str(), g_effect, ElapsedSeconds(), g_lengthMs / 1000.0,
                 Paused() ? 1 : 0, FxRenderingOn() ? 1 : 0, PlaybackMgr() ? "found" : "missing", FxTimeMs(), g_all.size(), g_rows.size(), g_queue.size(),
                 g_loop ? 1 : 0, g_playAll ? 1 : 0, g_boxFrames, g_boxMisses, g_box && g_qtOk ? qt.widgetWidth(g_box) : -1, g_box && g_qtOk ? qt.widgetHeight(g_box) : -1);
        return buf;
    }
}

void Setup()
{
    g_enabled = rp::host->ConfigGetInt(rp::self, "fxpreview", "enabled", 0) != 0;
    if (!g_enabled) { rp::Log("fxpreview: off ([fxpreview] enabled=0)"); return; }
    rt.holderCtor   = At<HolderCtorFn>(kEdHolderCtor);
    rt.loadEfx      = At<LoadEfxFn>(kFxLoadEfx);
    rt.loadLights   = At<LoadLightsFn>(kFxLoadLights);
    rt.fixSegments  = At<FixSegmentsFn>(kFxFixSegments);
    rt.registerDef  = At<RegisterDefFn>(kFxRegisterDef);
    rt.defByHandle  = At<DefByHandleFn>(kFxDefByHandle);
    rt.spawn        = At<SpawnFn>(kFxSpawn);
    rt.kill         = At<KillFn>(kFxKill);
    rt.effectPtr    = At<EffectPtrFn>(kFxEffectPtr);
    rt.renderOption = At<RenderOptionFn>(kRenderOption);
    rt.sysCount     = At<const int*>(kFxSystemCount);
    rt.sysPtr       = At<const char* const*>(kFxSystemPtr);
    rt.colors       = At<float*>(kColorTable);
    rt.mainWin      = At<void**>(kMainWin);
    rt.mgrVtbl      = At<void*>(kPlaybackMgrVtbl);
    rt.mgrUpdate    = At<MgrUpdateFn>(kPlaybackUpdate);
    rt.now          = At<const double*>(kNowSeconds);
    rt.viewSet      = At<ViewSetFn>(kViewSet);
    rt.viewIndex    = At<int*>(kViewIndex);
    rt.brushCacheUpd = At<VoidFn>(kBrushCacheUpd);
    rt.poolA        = At<VoidFn>(kPoolA);
    rt.poolB        = At<VoidFn>(kPoolB);
    rt.beginFrame   = At<BoolFn>(kBeginFrame);
    rt.endFrame     = At<VoidFn>(kEndFrame);
    rt.viewTarget   = At<ViewGetFn>(kViewTarget);
    rt.viewDepth    = At<ViewGetFn>(kViewDepth);
    rt.setTargets   = At<SetTargetsFn>(kSetTargets);
    rt.clear        = At<ClearFn>(kClear);
    rt.camDraw      = At<CamDrawFn>(kCamDraw);
    rt.dxDevice     = At<ID3D11Device**>(kDxDevice);
    rt.dxContext    = At<ID3D11DeviceContext**>(kDxContext);
    rt.swapChain0   = At<IDXGISwapChain**>(kSwapChain0);
    g_ready = rt.holderCtor && rt.loadEfx && rt.loadLights && rt.fixSegments && rt.registerDef && rt.defByHandle && rt.spawn && rt.kill && rt.effectPtr
           && rt.renderOption && rt.sysCount && rt.sysPtr && rt.colors && rt.mainWin && rt.mgrVtbl && rt.mgrUpdate && rt.now && rt.viewSet && rt.viewIndex
           && rt.brushCacheUpd && rt.poolA && rt.poolB && rt.beginFrame && rt.endFrame && rt.viewTarget && rt.viewDepth && rt.setTargets && rt.clear
           && rt.camDraw && rt.dxDevice && rt.dxContext && rt.swapChain0 && rp::host->RadiantRoot;
    if (!g_ready) { rp::Log("fxpreview: unavailable (Radiant mismatch)"); return; }
    g_qtOk = BindQt();
    const wchar_t* rootDir = rp::host->RadiantRoot(rp::self);
    g_fxRoot = std::wstring(rootDir ? rootDir : L"") + L"\\share\\raw\\fx";
    g_thumbRoot = std::wstring(rootDir ? rootDir : L"") + L"\\radiant\\fx_thumbs";
    BuildPane();
    if (g_grid)
    {
        ScanTree();
        ShowAll();
    }
    rp::Log("fxpreview: %s", g_pane ? "FX window ready" : "ready, no window (Qt exports missing)");
}

void Tick()
{
    if (!g_ready) return;
    const uint64_t now = GetTickCount64();
    const bool visible = g_pane && rp::host->PaneVisible(rp::self, g_pane);
    if (!g_hoverName.empty() && now >= g_hoverDue)
    {
        const std::string name = g_hoverName;
        g_hoverName.clear();
        if (name != g_name || !g_effect) Preview(name);
    }
    if (visible)
    {
        BoxTick(now);
        if (now >= g_nextVisibleMs) { RefreshVisible(); g_nextVisibleMs = now + 200; }
        if (!g_queue.empty()) WorkQueue(80);
    }
    else if (g_effect && !rt.effectPtr(0, g_effect)) g_effect = 0;
    if (now < g_nextLabelMs) return;
    g_nextLabelMs = now + 50;
    if (!rp::host->ui) return;
    char buf[64];
    if (g_lengthMs > 0) snprintf(buf, sizeof buf, "%.2f / %.2f s", ElapsedSeconds(), g_lengthMs / 1000.0);
    else snprintf(buf, sizeof buf, "%.2f s (looping)", ElapsedSeconds());
    SetLabel(g_elapsed, g_lastLabel, buf);
    if (g_slider)
    {
        const int top = SliderTopMs() / 10;
        int v = int(ElapsedSeconds() * 100.0);
        if (g_lengthMs <= 0 && top > 0) v %= top;
        g_settingSlider = true;
        rp::host->ui->SliderSetValue(g_slider, v < 0 ? 0 : v > top ? top : v);
        g_settingSlider = false;
    }
    const char* pause = Paused() ? "Play" : "Pause";
    if (g_pauseButton && g_lastPause != pause) { rp::host->ui->SetButtonText(g_pauseButton, pause); g_lastPause = pause; }
}

void Shutdown()
{
    if (!g_ready) return;
    g_queue.clear();
    Stop();
    ForgetDefs();
}

bool HidingMap() { return g_thumbFrame; }

void OnBrowserClick(const std::string& name)
{
    if (!g_ready || !g_follow || !g_pane || !rp::host->PaneVisible(rp::self, g_pane)) return;
    Preview(name);
}

bool Command(const std::string& command, std::string& reply)
{
    if (command.compare(0, 9, "fxpreview") != 0) return false;
    std::string arg = command.size() > 10 ? command.substr(10) : std::string();
    if (arg.empty() || arg == "status") { reply = Status(); return true; }
    if (!g_ready) { reply = Status(); return true; }
    if (arg == "stop") { Stop(); reply = "fxpreview: stopped\n"; return true; }
    if (arg == "dbg view")       // the fx step's view parameters with the user's camera and with the stage camera
    {
        typedef float* (*ViewSetupFn)(void* cam, float* out);
        ViewSetupFn setup = At<ViewSetupFn>(kViewSetup);
        char* cam = CamWnd();
        if (!setup || !cam) { reply = "fxpreview: no view setup\n"; return true; }
        float a[14] = {}, b[14] = {};
        setup(cam, a);
        float camAt[3];
        FrameCamera(g_def, camAt);
        { StageView view(camAt); setup(cam, b); }
        const char* s = FxSystem();
        char buf[700];
        snprintf(buf, sizeof buf, "fxpreview: view user [%.0f %.0f %.0f | %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f] stage [%.0f %.0f %.0f | %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f] sys+2308=%d dt=%d game=%d\n",
                 a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13],
                 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13],
                 s ? s[2308] : -1, s ? *reinterpret_cast<const int*>(s + 2296) : -1, rp::host->GameView ? rp::host->GameView(rp::self) : -1);
        reply = buf;
        return true;
    }
    if (arg.compare(0, 4, "dbg ") == 0)
    {
        if (arg.find("gameview") != std::string::npos) g_dbgNoGameView = arg.back() == '0';
        if (arg.find("clock") != std::string::npos) g_dbgNoClock = arg.back() == '0';
        reply = "fxpreview: game view switch " + std::string(g_dbgNoGameView ? "off" : "on") + ", clock restore " + (g_dbgNoClock ? "off" : "on") + "\n";
        return true;
    }
    if (arg == "pause") { TogglePause(); reply = Status(); return true; }
    if (arg == "refresh") { OnRefresh(nullptr); reply = Status(); return true; }
    if (arg == "all") { ShowAll(); reply = Status(); return true; }
    if (arg == "makeall")
    {
        std::vector<int> all;
        for (size_t i = 0; i < g_all.size(); ++i) all.push_back(int(i));
        Enqueue(all);
        reply = Status();
        return true;
    }
    if (arg == "loop") { g_loop = !g_loop; if (g_loopBox) rp::host->ui->SetChecked(g_loopBox, g_loop); reply = Status(); return true; }
    if (arg == "playall") { OnPlayAllBox(nullptr, g_playAll ? 0 : 1); if (g_playAllBox) rp::host->ui->SetChecked(g_playAllBox, g_playAll); reply = Status(); return true; }
    if (arg.compare(0, 7, "search ") == 0) { if (g_search) rp::host->ui->SetEditText(g_search, arg.c_str() + 7); ShowSearch(arg.substr(7)); reply = Status(); return true; }
    if (arg.compare(0, 6, "place ") == 0) { reply = PlaceAtCamera(CleanName(arg.substr(6))) ? "fxpreview: placed\n" : "fxpreview: FX Browser not found\n"; return true; }
    if (arg.compare(0, 5, "seek ") == 0)
    {
        std::string why;
        reply = Start(g_name, -atoi(arg.c_str() + 5), why) ? Status() : "fxpreview: " + why + "\n";
        return true;
    }
    // "fxpreview thumb <dir/name> [out.png]": a hidden-frame thumbnail (the cache path by default)
    if (arg.compare(0, 6, "thumb ") == 0)
    {
        char name[256] = {}, out[512] = {};
        if (sscanf_s(arg.c_str() + 6, "%255s %511s", name, unsigned(sizeof name), out, unsigned(sizeof out)) < 1) { reply = "fxpreview thumb <dir/name> [out.png]\n"; return true; }
        const std::string clean = CleanName(name);
        std::wstring path = Wide(out);
        if (path.empty())
        {
            const size_t slash = clean.find_last_of('/');
            path = ThumbPath({ slash == std::string::npos ? "" : clean.substr(0, slash), slash == std::string::npos ? clean : clean.substr(slash + 1) });
        }
        std::string why;
        const uint64_t t0 = GetTickCount64();
        const bool ok = Thumbnail(clean, path, why);
        char buf[900];
        snprintf(buf, sizeof buf, "fxpreview thumb: %s%s (%llu ms)\n", ok ? "written " : "failed: ", ok ? Utf8(path.c_str(), int(path.size())).c_str() : why.c_str(),
                 (unsigned long long)(GetTickCount64() - t0));
        reply = buf;
        return true;
    }
    if (arg.compare(0, 5, "play ") == 0)
    {
        reply = Preview(arg.substr(5)) ? Status() : "fxpreview: " + g_lastStatus + "\n";
        return true;
    }
    reply = "fxpreview: status | play <dir/name> | seek <ms> | stop | pause | loop | playall | refresh | all | search <words> | place <dir/name> | thumb <dir/name> [out.png] | makeall\n";
    return true;
}
}
