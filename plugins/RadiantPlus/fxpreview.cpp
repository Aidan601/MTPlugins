// FX window: every effect under share/raw/fx in a thumbnail grid (like the Models pane) next to a preview box. A
// click plays the effect in the box through Radiant's own fx system: the effect lives far above the map and every box
// frame is a hidden camera frame (drawn, read back, never presented), so the 3D view never shows it. Double-click sets
// it on the selected fx entities, a drag onto the 3D view places it like the FX Browser does. Thumbnails are filmstrips
// of hidden frames that loop in the grid. Nothing is added to the map or the undo history.
#include "modtools_plugin.hpp"
#include "fxpreview.h"
#include "fxswap.h"
#include "crashguard.h"
#include <windows.h>
#include <d3d11.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
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
    const uintptr_t kFxDefByHandle   = 0x14013B7F0;  // (int handle) -> compiled def: +12 u16 looping emitters, +24 looping life ms, +28 one-shot life ms (its bounds at +40/+52 stay the 10-unit default)
    const uintptr_t kFxSpawn         = 0x1405B2BD0;  // (int def, const float origin[3], const float axis[9], int delayMs, float unused) -> effect handle, 0 = failed
    const uintptr_t kFxKill          = 0x1405B29C0;  // (unsigned effect): kills the effect and its children, the references stay
    const uintptr_t kPlaybackStopAll = 0x140472F00;  // FX playback's stop-all (Stop Effect, Retrigger FX): kill-all 0x14015A540 on client 0
    const uintptr_t kFxKillOne       = 0x140159060;  // (sys, effect): kills one effect and its children, the kill-all's per-effect call
    const uintptr_t kFxRelease       = 0x1405B28C0;  // (unsigned effect): drops the spawner's reference and the effect's own, the slot is freed
    const uintptr_t kFxEffectPtr     = 0x140160490;  // (int client, unsigned effect) -> the live effect or NULL
    const uintptr_t kFxSystemCount   = 0x1443900B0;  // int, local fx clients
    const uintptr_t kFxSystemPtr     = 0x1443900B8;  // client 0's fx system, +2292 = fx time ms
    const int off_Sys_Effects   = 2208;     // effect array, effect = base + 32 * slot id (384 bytes each)
    const int off_Sys_Ring      = 2912;     // effect ring: +24 oldest, +32 next, +36 u16[4096] slot ids, full at 4096
    const int off_Fx_Origin     = 92;
    const int off_Fx_Handle     = 216;
    const int off_Fx_Status     = 240;      // u64: low 14 bits references, 0x8000 released by its spawner
    const uintptr_t kFxChildSpawns   = 0x140162E90;  // (def, cap) -> spawn count of a child def: the step reads it without a null check
    const uintptr_t kFxLightIcon     = 0x1400F8D90;  // (ctx, ...): the yellow icon model of a light elem, ctx+72 = the elem def
    const uintptr_t kFxEffectUpdate  = 0x140167F00;  // (sys, effect, float): one effect from effect+200 (last update ms) to sys+2292, origin at effect+92
    const uintptr_t kFxSetCamera     = 0x1401613A0;  // (int client, int which, const float* view): the camera a step culls spawns against, +148 = plane count
    const uintptr_t kFxDrawSprites   = 0x1400FB5D0;  // (sys, effect, ...10 args): FX_Draw's per-effect sprites, called before kFxDrawEffect
    const uintptr_t kFxDrawEffect    = 0x1400FB8C0;  // (sys, effect, ...12 args): FX_Draw's per-effect draw, effect +92 origin, +160 def handle
    const uintptr_t kDebugLists      = 0x1403A91A0;  // (a, b, c): Cam_Draw's two persistent primitive lists (0x14448FC80, 0x14448FCD0)
    const uintptr_t kCachedLines     = 0x140B59CC0;  // (cam + 0xEF8): Cam_Draw's pre-built line sets, editor view only
    const uintptr_t kRenderOption    = 0x1409BBAC0;  // (int bit) -> bool, bit 1 = Fx > Fx Rendering Enabled
    const uintptr_t kColorTable      = 0x1453A14A8;  // float[48][4], row 4 = COLOR_CAMERABACK (Cam_Draw clears with it)
    const uintptr_t kMainWin         = 0x1444C6560;  // RadiantMainWin*, +64 = RadiantQtBridge, bridge+240 = CCamWnd, bridge+1888 = EffectPlaybackMgr
    const uintptr_t kPlaybackMgrVtbl = 0x142310788;  // EffectPlaybackMgr's vtable, checked before the manager is trusted
    const uintptr_t kToolboxMeta     = 0x142466D00;  // QtAbstractToolbox::staticMetaObject: signals RecentItemsUpdated, ItemDoubleClicked, AddItemAtCameraLocation
    const uintptr_t kViewDrop        = 0x1404DD740;  // (bridge + 1960, view 0 = camera, x, y from the top left, const QMimeData*) -> bool: a browser drop, the camera traces the cursor ray
    const uintptr_t kUpdateViews     = 0x140007DFB;  // (int flags), -1 redraws every view (the drop events' tail)
    const uintptr_t kSelectDeselect  = 0x140D023E0;  // Select_Deselect(char clearDrag)
    const uintptr_t kAnimSliderCtor  = 0x1401D9CF0;  // (this, QWidget* parent): the Animation window's QSlider subclass (128 bytes, 10 px font)
    const uintptr_t kToolboxFindItem = 0x140821160;  // (QStandardItem* root, const QString& name) -> QStandardItem*, depth first by data(256)
    const uintptr_t kToolboxToggleFav = 0x14081E5D0; // (toolbox, QStandardItem*): Toggle Favorite, the tree row and QSettings <name>/Favorites
    const uintptr_t kGameView        = 0x1453A1EE9;  // char, then 0x1453A1EEA probe view: the 3D view draws a picture of cam+3764 x cam+3768 centred in the pane
    // Hidden frames: CCamWnd::OnRender's steps without its Present
    const uintptr_t kPlaybackUpdate  = 0x1404732E0;  // (mgr): EffectPlaybackMgr::Update, advances the fx clock by now - mgr+40
    const uintptr_t kNowSeconds      = 0x1453A1848;  // double, the idle tick's clock the manager reads
    const uintptr_t kCamAnglesToAxes = 0x140FAEDD0;  // (cam + 200): the camera's cached axes (+512/+524/+536) from its angles (+220), as Cam_Draw does
    const uintptr_t kExposureAdapt   = 0x14023ACF0;  // (cam, ...): auto exposure, pops the readback list and smooths cam+3712..3720
    const uintptr_t kReadbackPopWait = 0x1402A9AD0;  // (list, shared_ptr* out): pops the oldest readback, waiting for the GPU
    const uintptr_t kReadbackPopReady = 0x1402A95E0; // same, null when the oldest is not ready yet
    const size_t    off_Cam_Exposure = 3712;         // float[3]: EV, exposure scale, EV shown on the HUD
    const size_t    off_Cam_Readbacks = 6936;        // the exposure readback list Cam_Draw hands the renderer, +8 = entry count
    const uintptr_t kLockSunVolume   = 0x1453A23AA;  // byte, the Exposure toolbar's "Lock Sun Volume": the camera keeps its sun volume and light state
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
    const uintptr_t kCamGetFov       = 0x1402316D0;  // () -> float: the editor view's FOV, horizontal for a 16:9 view
    const uintptr_t kDxDevice        = 0x1455740C0;  // ID3D11Device*
    const uintptr_t kDxContext       = 0x145574210;  // ID3D11DeviceContext*
    const uintptr_t kSwapChain0      = 0x145584A78;  // IDXGISwapChain* of view 0 (the camera)

    const size_t off_Bridge_CamWnd      = 240;
    const size_t off_Dock_Content       = 80;   // TADockWidget: the content QWidget in its QVBoxLayout
    const size_t off_Toolbox_Model      = 424;  // QtAbstractToolbox: QStandardItemModel*
    const size_t off_Toolbox_Unsorted   = 568;  // QStandardItem*, the "Unsorted" row (empty for effects)
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
    const size_t off_Def_Elems     = 32;    // FxElemDef array, 584 bytes each, counts at +12/+14/+16
    const size_t kElemSize         = 584;
    const size_t off_Elem_Type     = 200;   // u8, 14 = runner
    const size_t off_Elem_Children = 201;   // u8 runner child count: 1 = handle at +240, more = +240 points at 64-byte entries whose first int is the handle
    const size_t off_Elem_Child    = 240;
    const size_t off_Elem_OnDeath  = 404;   // child def handles the step looks up without a null check (0 = none)
    const size_t off_Elem_OnImpact = 408;
    const size_t off_Elem_Emitted  = 412;
    const size_t off_Elem_SpawnRange = 24;  // floats min, max (0 0 = any distance)
    const unsigned kElemViewModel  = 0x1000;  // elem +0 flags: drawWithViewModel
    const size_t off_Elem_FadeIn   = 32;    // floats base, width: fully in closer than base
    const size_t off_Elem_FadeOut  = 40;    // floats base, width: fully in farther than base + width
    const int    kCameraBackRow    = 4;
    const float  kNeutral[4]       = { 0.18f, 0.18f, 0.18f, 1.0f };
    float        kStage[3]         = { 0, 0, 3000 };    // PlaceStage puts it above the open map ("fxpreview dbg stage x y z" moves it)
    const float  kStageAbove       = 2000.0f;           // stage height over the map's highest brush
    const float  kStageLimit       = 15000.0f;          // no stage coordinate past this: far stages lose line, beam and ring elems
    const float  kStageAngles[3]   = { 10, 90, 0 };     // the thumbnail camera's heading: along +y, standing above the stage, its view 10 degrees up (positive pitch looks up)
    const size_t kHolderSize       = 599544;
    const int    kThumbSize        = 128;
    const char*  kThumbVersion     = "4";   // bumped when thumbnails change their look: older cached ones are remade
    const double kThumbSeconds     = 1.0;
    const float  kMeasureDist      = 400.0f; // stage camera distance for the measuring looks
    const float  kFill             = 0.5f;   // the box camera puts the measured content at this share of the frame height
    const float  kBoxCrop          = 0.7f;   // the box shows this share of the frame height, around the content
    const size_t kSearchCap        = 2000;
    unsigned     g_boxFps          = 30;    // [fxpreview] box_fps
    const int    kEndlessWindowMs  = 10000; // the slider's span and Play all's dwell for an endless effect
    const int    kSlowBelowMs      = 300;   // effects shorter than this are scrubbed in the box at quarter speed
    const int    kScrubSpanMs      = 250;   // the part of a short effect's life the scrub repeats
    const int    kMinCycleMs       = 1000;  // a shorter effect (a muzzle flash) still plays this long before it loops or stops
    // animated thumbnails: a filmstrip per effect (poster frame, then the loop), played in the grid at kStripFps
    const int    kStripFps         = 12;
    const int    kStripMinFrames   = 12;
    const int    kStripMaxFrames   = 24;
    const int    kStripSpanMaxMs   = 4000;  // longer one-shot effects show their first 4 s, sped up
    const int    kStripWarmMs      = 1000;  // endless effects are filmed from this far into their life
    const int    kStripEndlessMs   = 2000;

    typedef void*    (*HolderCtorFn)(void* holder);
    typedef char     (*LoadEfxFn)(const char* name, void* def);
    typedef void     (*LoadLightsFn)(const char* name, void* holder);
    typedef void     (*FixSegmentsFn)(void* holder);
    typedef int*     (*RegisterDefFn)(int* out, void* def, char reload);
    typedef const char* (*DefByHandleFn)(int handle);
    typedef unsigned (*SpawnFn)(int def, const float* origin, const float* axis, int delayMs, float unused);
    typedef void     (*KillFn)(unsigned effect);
    typedef void     (*ReleaseFn)(unsigned effect);
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
        DefByHandleFn defByHandle; SpawnFn spawn; KillFn kill; ReleaseFn release; EffectPtrFn effectPtr; RenderOptionFn renderOption;
        const int* sysCount; const char* const* sysPtr; float* colors; void** mainWin; void* mgrVtbl;
        MgrUpdateFn mgrUpdate; const double* now; ViewSetFn viewSet; int* viewIndex; VoidFn brushCacheUpd; VoidFn poolA; VoidFn poolB; void (*anglesToAxes)(void* camView);
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
        void* (*currentItem)(const void* list);
        bool  (*isItemSelected)(const void* list, const void* item);
        void  (*clearSelection)(void* view);
        int   (*viewState)(const void* view);
        void  (*viewSetState)(void* view, int state);
        void  (*setCurrentItem)(void* list, void* item);
        void  (*setItemIcon)(void* item, const void* icon);
        void* (*itemAt)(const void* list, const QPointRaw* pos);
        int   (*row)(const void* list, const void* item);
        void  (*setItemToolTip)(void* item, const void* qstring);
        QRectRaw* (*visualItemRect)(const void* list, QRectRaw* ret, const void* item);
        int   (*widgetHeight)(const void* widget);
        int   (*widgetWidth)(const void* widget);
        void  (*splitterSetCollapsible)(void* splitter, int index, bool on);
        void  (*widgetSetMaxWidth)(void* widget, int w);
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
        void* (*imageFromFile)(void* img, const void* qstring, const char* format);
        int   (*imageWidth)(const void* img);
        int   (*imageHeight)(const void* img);
        void* (*stringCtor)(void* qs, const char* text);
        void  (*stringDtor)(void* qs);
        void* (*mimeCtor)(void* mime);
        void  (*mimeDtor)(void* mime);
        void  (*mimeSetData)(void* mime, const void* format, const void* bytes);
        void* (*bytesCtor)(void* bytes, const char* data, int size);
        void  (*bytesDtor)(void* bytes);
        void  (*opDelete)(void* p);
        void* (*widgetLayout)(const void* widget);
        void* (*toolButtonCtor)(void* button, void* parent);
        void* (*iconFromFile)(void* icon, const void* path);
        void  (*buttonSetIcon)(void* button, const void* icon);
        void  (*buttonSetIconSize)(void* button, const void* size);
        void  (*buttonSetCheckable)(void* button, bool on);
        void  (*buttonSetText)(void* button, const void* text);
        void  (*toolButtonSetStyle)(void* button, int style);
        void  (*toolButtonSetPopupMode)(void* button, int mode);
        void  (*toolButtonSetMenu)(void* button, void* menu);
        void  (*sliderSetOrientation)(void* slider, int orientation);
        void* (*modelRoot)(const void* model);
        void* (*itemCtor)(void* item, const void* text);
        void  (*itemAppendRow)(void* item, void* child);
        void  (*itemSetEditable)(void* item, bool on);
        int   (*itemRowCount)(const void* item);
        void* (*variantFromString)(void* v, const void* text);
        void  (*variantDtor)(void* v);
        void  (*layoutRemoveWidget)(void* layout, void* widget);
        void  (*widgetSetParent)(void* widget, void* parent);
        void  (*deleteLater)(void* obj);
        void* (*visibleRegion)(const void* widget, void* ret);
        bool  (*regionIsEmpty)(const void* region);
        void  (*regionDtor)(void* region);
        void* (*dragCtor)(void* drag, void* source);
        void  (*dragDtor)(void* drag);
        void  (*dragSetMime)(void* drag, void* mime);
        int   (*dragExec)(void* drag, int actions);
        int   (*indexOfSignal)(const void* meta, const char* signature);
        int   (*methodOffset)(const void* meta);
        void  (*activate)(void* obj, const void* meta, int localSignal, void** argv);
        bool  (*invokeMethod)(void* obj, const char* member, int connection, const GenericArg* a0, const GenericArg* a1, const GenericArg* a2,
                              const GenericArg* a3, const GenericArg* a4, const GenericArg* a5, const GenericArg* a6, const GenericArg* a7, const GenericArg* a8, const GenericArg* a9);
        // optional: the Options and right-click menus
        void* (*opNew)(size_t size);    // MSVCR110's operator new: Qt frees the menus it owns with that CRT
        void* (*menuCtor)(void* menu, void* parent);
        void  (*buttonSetMenu)(void* button, void* menu);
        void  (*menuPopup)(void* menu, const QPointRaw* pos, void* atAction);
        void  (*actionSetCheckable)(void* action, bool on);
        void  (*actionSetChecked)(void* action, bool on);
        bool  (*actionIsChecked)(const void* action);
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
    std::wstring g_thumbBase;           // the cache with the XYZ grid; without it: <base>_nogrid (the grid is filmed into the strips)

    bool g_enabled = false, g_ready = false, g_qtOk = false;
    // the effect in the preview box
    std::string g_name;                 // "dir/name"
    unsigned g_effect = 0;              // the live effect handle, 0 = none
    const size_t off_Sys_PlaneCount = 148;
    bool g_noCull = true;               // while a preview effect is live no step culls spawns by frustum ("fxpreview dbg cull 0|1")
    int g_inStage = 0;                  // open StageView scopes: their steps leave the map's effects unculled too
    typedef __int64 (*SetCameraFn)(int client, int which, void* view);
    SetCameraFn g_origSetCamera = nullptr;
    const char* g_stageDefPtr = nullptr;    // the compiled def on the stage: its light icon models are never made
    unsigned g_iconCalls = 0, g_iconSkips = 0;
    typedef __int64 (*LightIconFn)(char* ctx, void* elem, void* a3, int lod);
    LightIconFn g_origLightIcon = nullptr;
    // ctx+72 = the elem def being drawn: skipped when it is one of the staged def's elems
    bool StageElem(const char* elem)
    {
        const char* def = g_stageDefPtr;
        if (!def || !elem) return false;
        const int n = *reinterpret_cast<const short*>(def + 12) + *reinterpret_cast<const short*>(def + 14) + *reinterpret_cast<const short*>(def + 16);
        const char* elems = *reinterpret_cast<const char* const*>(def + 32);
        return elems && n > 0 && elem >= elems && elem < elems + size_t(n) * 584;
    }
    int Timeline(int ms);
    const char* FxSystem();
    // a step on the box's or the jobs' clock updates only our effects (child effects included): no light icons from it
    bool OurStep() { const char* s = FxSystem(); return s && Timeline(*reinterpret_cast<const int*>(s + 2292)) != 0; }
    __int64 hk_LightIcon(char* ctx, void* elem, void* a3, int lod)
    {
        ++g_iconCalls;
        if (OurStep() || (ctx && StageElem(*reinterpret_cast<const char* const*>(ctx + 72)))) { ++g_iconSkips; return 0; }
        return g_origLightIcon(ctx, elem, a3, lod);
    }
    // Radiant's own steps run with the user's camera, far from the stage: the stage's effects pass the time there without
    // emitting (spawn ranges), so they are left for the stage step, which then runs the whole interval with its camera.
    float g_boxCam[3] = { 0, 0, 3000 };    // the hidden camera for the box, placed from the effect's measured fit
    std::vector<int> g_stageHandles;        // the staged def and its runner children: the effects the hook judges from the stage
    float g_judgeDist = 400.0f;             // distance the stage effect is judged from, inside its elems' spawn and fade ranges
    float g_nearestDist = 0.0f;             // closer than this its near fade (fadeOutRange) hides it: the box camera stays out
    typedef __int64 (*EffectUpdateFn)(char* sys, char* effect, float a3);
    EffectUpdateFn g_origEffectUpdate = nullptr;
    int g_holdStage = 2;                // "fxpreview dbg hold 0|1|2": 0 off, 1 skip them (test), 2 judged from the box camera
    DWORD g_mainTid = 0;
    volatile long g_heldUpdates = 0;
    // Three fx timelines: Radiant's, the preview box's (kBoxAhead past it) and the thumbnail jobs' (kFilmAhead past both).
    // What a clock spawns is born on its timeline, a step or a frame never updates or draws another timeline's effects, so
    // Radiant's FX playback settings and clock never reach the box or the tiles.
    const int kFilmAhead = 100000000;
    const int kBoxAhead = 20000000;
    volatile long g_filmBase = LONG_MAX;    // fx time ms the job's clock starts from, LONG_MAX without a job
    volatile long g_boxBase = LONG_MAX;     // fx time ms the box's clock starts from, LONG_MAX before the first preview
    int Timeline(int ms) { return ms >= g_filmBase ? 2 : ms >= g_boxBase ? 1 : 0; }
    bool OtherClock(const char* sys, const char* effect)
    {
        if ((g_filmBase == LONG_MAX && g_boxBase == LONG_MAX) || !sys || !effect) return false;
        return Timeline(*reinterpret_cast<const int*>(sys + 2292)) != Timeline(*reinterpret_cast<const int*>(effect + 196));   // fx time ms, spawn time ms
    }
    // born on the box's or the jobs' timeline: ours, never a map effect (a map effect of the same def near the stage is not)
    bool OurEffect(const char* effect) { return effect && Timeline(*reinterpret_cast<const int*>(effect + 196)) != 0; }
    // FX playback's Stop Effect and Retrigger FX kill every effect: ours (born on the box's or the jobs' timeline) stay
    typedef double (*StopAllFn)(void* a, void* b);
    StopAllFn g_origStopAll = nullptr;
    int g_inStopAll = 0;
    double hk_StopAll(void* a, void* b)
    {
        ++g_inStopAll;
        const double r = g_origStopAll(a, b);
        --g_inStopAll;
        return r;
    }
    typedef __int64 (*KillOneFn)(char* sys, char* effect);
    KillOneFn g_origKillOne = nullptr;
    __int64 hk_KillOne(char* sys, char* effect)
    {
        if (g_inStopAll && effect && Timeline(*reinterpret_cast<const int*>(effect + 196)) != 0) return 0;
        return g_origKillOne(sys, effect);
    }
    __int64 hk_EffectUpdate(char* sys, char* effect, float a3)
    {
        if (OtherClock(sys, effect)) return 0;
        // worker threads run this after the step returned: the step's camera (sys+0, set by 0x1401613A0) tells whose step it is
        if (g_holdStage && effect && sys && g_stageDefPtr)
        {
            const float* o = reinterpret_cast<const float*>(effect + 92);
            const int handle = *reinterpret_cast<const int*>(effect + 160);
            const float* cam = reinterpret_cast<const float*>(sys);
            const auto nearStage = [](const float* p) { return fabsf(p[0] - kStage[0]) < 4000.0f && fabsf(p[1] - kStage[1]) < 4000.0f && fabsf(p[2] - kStage[2]) < 4000.0f; };
            if (OurEffect(effect) && nearStage(o) && GetCurrentThreadId() == g_mainTid && std::find(g_stageHandles.begin(), g_stageHandles.end(), handle) != g_stageHandles.end())
            {
                _InterlockedIncrement(&g_heldUpdates);
                if (g_holdStage == 1 && !nearStage(cam)) return 0;
                // the update judges distance from the camera copies too: all four get a point g_judgeDist from the stage, towards the box camera
                float d[3] = { g_boxCam[0] - kStage[0], g_boxCam[1] - kStage[1], g_boxCam[2] - kStage[2] };
                float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (len < 1.0f) { d[0] = 0; d[1] = -1; d[2] = 0; len = 1.0f; }
                const float judge[3] = { kStage[0] + d[0] / len * g_judgeDist, kStage[1] + d[1] / len * g_judgeDist, kStage[2] + d[2] / len * g_judgeDist };
                static const size_t kCams[4] = { 0, 368, 736, 1104 };   // the step's camera, its draw copy, camera 1 and its copy
                // and no frustum: camera 1 (+736) keeps the user's planes, which cull the stage's spawns unless the user looks at it
                float saved[4][3];
                int planes[4];
                for (int i = 0; i < 4; ++i)
                {
                    memcpy(saved[i], sys + kCams[i], sizeof saved[i]); memcpy(sys + kCams[i], judge, sizeof saved[i]);
                    int* count = reinterpret_cast<int*>(sys + kCams[i] + off_Sys_PlaneCount);
                    planes[i] = *count; if (g_noCull) *count = 0;
                }
                const __int64 r = g_origEffectUpdate(sys, effect, a3);
                for (int i = 0; i < 4; ++i) { memcpy(sys + kCams[i], saved[i], sizeof saved[i]); *reinterpret_cast<int*>(sys + kCams[i] + off_Sys_PlaneCount) = planes[i]; }
                return r;
            }
        }
        return g_origEffectUpdate(sys, effect, a3);
    }
    typedef __int64 (*ChildSpawnsFn)(const void* def, unsigned cap);
    ChildSpawnsFn g_origChildSpawns = nullptr;
    __int64 hk_ChildSpawns(const void* def, unsigned cap) { return def ? g_origChildSpawns(def, cap) : 0; }   // a missing child def spawns nothing
    int g_def = 0;
    int g_startMs = 0;                  // fx time when the effect started (elapsed = fx time - this)
    int g_lengthMs = 3000;              // the effect's own life from its compiled def
    // Where an effect's lit pixels sat in a frame taken kMeasureDist away: centre as fractions of the frame height from
    // the frame centre (right and down positive), size = the trimmed box's larger side as a fraction of the height.
    struct Fit { bool ok = false; float cx = 0, cy = 0, size = 0, scale = 1.0f; };   // scale: the look's distance over kMeasureDist
    std::map<std::string, Fit> g_fits;
    std::set<std::string> g_bad;            // effects that raised a Radiant assert: never played again this session
    // asserts raised while the FX window spawns or steps its own effect are logged without a box, Hit() = one happened
    struct QuietScope
    {
        long before;
        QuietScope() : before(br::crashguard::AssertCount()) { br::crashguard::QuietAsserts(true); }
        ~QuietScope() { br::crashguard::QuietAsserts(false); }
        bool Hit() const { return br::crashguard::AssertCount() != before; }
    };
    float g_fitScale = 1.0f;            // the last Looks() frame's distance over kMeasureDist (its fit is in kMeasureDist terms)
    bool g_boxDirty = false;            // draw a frame even when the effect ended (the last look stays)
    uint64_t g_lastStepMs = 0;
    unsigned g_boxFrames = 0, g_boxMisses = 0;
    bool g_thumbFrame = false;          // a hidden frame is being drawn: the map stays out
    DWORD g_thumbThread = 0;            // the thread drawing it (main)
    bool g_skipMapDraws = true;         // "fxpreview dbg mapdraw 1" lets the map's helpers and selection draw again
    // hidden frames leave out the map's helper lines: target links (persistent lists) and the editor view's cached lines
    typedef __int64 (*DebugListsFn)(__int64 a, __int64 b, __int64 c);
    DebugListsFn g_origDebugLists = nullptr;
    __int64 hk_DebugLists(__int64 a, __int64 b, __int64 c)
    {
        if (g_thumbFrame && g_skipMapDraws) return 0;
        return g_origDebugLists(a, b, c);
    }
    // the user's frames leave out the stage's effects (they hang over the map), hidden frames draw them
    bool HiddenFromUser(const char* effect)
    {
        if (g_thumbFrame || !OurEffect(effect) || g_stageHandles.empty()) return false;
        const float* o = reinterpret_cast<const float*>(effect + off_Fx_Origin);
        const int handle = *reinterpret_cast<const int*>(effect + 160);
        return fabsf(o[0] - kStage[0]) < 1000.0f && fabsf(o[1] - kStage[1]) < 1000.0f && fabsf(o[2] - kStage[2]) < 1000.0f
            && std::find(g_stageHandles.begin(), g_stageHandles.end(), handle) != g_stageHandles.end();
    }
    typedef __int64 (*FxDrawSpritesFn)(__int64, char*, __int64, __int64, __int64, __int64, __int64, __int64, __int64, __int64);
    FxDrawSpritesFn g_origFxDrawSprites = nullptr;
    const char* FxSystem();
    __int64 hk_FxDrawSprites(__int64 sys, char* effect, __int64 a3, __int64 a4, __int64 a5, __int64 a6, __int64 a7, __int64 a8, __int64 a9, __int64 a10)
    {
        if (HiddenFromUser(effect) || OtherClock(FxSystem(), effect)) return 0;
        return g_origFxDrawSprites(sys, effect, a3, a4, a5, a6, a7, a8, a9, a10);
    }
    typedef __int64 (*FxDrawEffectFn)(__int64, char*, __int64, __int64, __int64, __int64, __int64, __int64, __int64, __int64, __int64, __int64);
    FxDrawEffectFn g_origFxDrawEffect = nullptr;
    __int64 hk_FxDrawEffect(__int64 sys, char* effect, __int64 a3, __int64 a4, __int64 a5, __int64 a6, __int64 a7, __int64 a8, __int64 a9, __int64 a10, __int64 a11, __int64 a12)
    {
        if (HiddenFromUser(effect) || OtherClock(FxSystem(), effect)) return 0;
        return g_origFxDrawEffect(sys, effect, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
    }
    typedef void* (*CachedLinesFn)(void* lines);
    CachedLinesFn g_origCachedLines = nullptr;
    void* hk_CachedLines(void* lines)
    {
        if (g_thumbFrame && g_skipMapDraws) return nullptr;
        return g_origCachedLines(lines);
    }
    bool g_loop = true;                 // [fxpreview] loop
    bool g_halted = false;              // Stop pressed: Loop waits for the next pick
    uint64_t g_loopRetryMs = 0;         // a loop restart that failed is tried again after this
    bool g_everLit = false;             // the box showed the current effect at least once
    uint64_t g_lastLitUs = 0;           // the last box frame with the effect in it
    // a drag from the grid
    bool g_pressed = false;
    int g_pressX = 0, g_pressY = 0;
    std::string g_dragName;
    bool g_settingSlider = false;
    uint64_t g_nextLabelMs = 0;
    std::string g_lastLabel, g_lastStatus, g_lastPause;

    // widgets
    // the window lives in Radiant's FX Browser pane: our root, or the browser's own content with a Legacy bar under it
    void* g_root = nullptr;
    void* g_bar = nullptr;
    void* g_legacyBox = nullptr;
    char* g_host = nullptr;             // the QtEffectsBrowser (a dock pane) holding them, null until found
    void* g_stock = nullptr;            // its content widget (TADockWidget +80)
    bool g_legacy = false;
    uint64_t g_nextAttachMs = 0;
    void* g_search = nullptr;
    void* g_gridViewport = nullptr;
    void* g_gridFilter = nullptr;
    void* g_box = nullptr;              // the preview QLabel
    void* g_boxColumn = nullptr;        // its column right of the grid, shown only while a tile is selected
    bool g_columnShown = false, g_columnWidened = false;
    void* g_widenColumn = nullptr;      // first shown just now: a wide minimum makes the splitter give it room, relaxed at the next tick
    void ShowColumn(bool on)
    {
        if (!g_boxColumn || on == g_columnShown) return;
        if (on && !g_columnWidened) { rp::host->ui->SetMinimumSize(g_boxColumn, 280, 0); g_widenColumn = g_boxColumn; g_columnWidened = true; }
        rp::host->ui->SetVisible(g_boxColumn, on ? 1 : 0);
        g_columnShown = on;
    }
    void* g_grid = nullptr;
    // a tile is selected (highlighted): the column and the box follow this alone
    bool GridSelected()
    {
        if (!g_grid || !qt.currentItem || !qt.isItemSelected) return false;
        void* item = qt.currentItem(g_grid);
        return item && qt.isItemSelected(g_grid, item);
    }
    void ClearGridSelection() { if (g_grid && qt.clearSelection) qt.clearSelection(g_grid); }
    void* g_pauseButton = nullptr;
    void* g_loopBox = nullptr;
    void* g_legacyBox2 = nullptr;       // Legacy in the toggle row (g_legacyBox sits under Radiant's own browser)
    void* g_slider = nullptr;
    void* g_elapsed = nullptr;          // only without Radiant's slider class, which draws the times itself
    // that slider: +48 mode, +96 / +100 the start and end times it draws (s), +112 fps, +116 frames, +120 the animation
    // whose name (char[] at +0xA704) it centres. Mode 0 with g_sliderAnim (an empty name) draws "time | frame" at both ends.
    char* g_animSlider = nullptr;
    const size_t kSliderAnimName = 0xA704;
    char g_sliderAnim[0xA704 + 256] = {};
    void* g_scopeAll = nullptr;
    void* g_scopeUse = nullptr;
    void* g_scopeTop = nullptr;
    void* g_favBox = nullptr;
    void* g_boxFilter = nullptr;
    void* g_gridMenu = nullptr;         // right-click menu of a grid item
    void* g_favAction = nullptr;
    void* g_iconSlider = nullptr;
    void* g_leftPane = nullptr;         // the grid side of the splitter, as wide as the Icon Size bar's end at least
    int g_iconRowPaneW = 0, g_iconRowX = 0;       // Icon Size at the bottom, as in the Models and Textures panes
    int g_iconPending = 0;              // a slider value waiting to be applied (the icons are cut again for each size)
    uint64_t g_iconDueMs = 0;

    // the top bar: 0 all, 1 in use at this level, 2 in use at this level and the levels above (Radiant's "All In Use Top")
    int g_scope = 0;
    bool g_favOnly = false;
    std::set<std::string> g_favs;       // lower-case "dir/name"
    std::vector<std::vector<std::string>> g_levelFx;   // fxdefs of the levels above the current one, captured before Enter Prefab
    std::vector<std::string> g_lastUse; // the in-use list the grid was filtered with
    std::string g_lastFilter, g_typedFilter;
    uint64_t g_filterDue = 0, g_nextUseMs = 0, g_nextTypedMs = 0;
    std::string g_menuName;             // the grid item the right-click menu is for
    int g_iconSize = 128;               // [fxpreview] icon_size: shown size of the thumbnails (files stay kThumbSize)

    // the box camera: an orbit around the effect's measured centre
    float g_orbitYaw = 90.0f, g_orbitPitch = 10.0f, g_orbitZoom = 1.0f;
    float g_pan[3] = { 0, 0, 0 };
    float g_tanV = 0.6704f;             // tan(vfov/2) of stage frames (editor view, Fov 100 until read)
    bool g_showGrid = true;             // [fxpreview] grid
    int g_dragMode = 0;                 // 1 orbit, 2 pan, 3 zoom
    int g_dragX = 0, g_dragY = 0;
    int g_wheelDelta = 0;               // the last WM_MOUSEWHEEL, read by the box's Wheel event
    HHOOK g_wheelHook = nullptr;
    bool g_asyncBox = true;             // "fxpreview dbg async 0|1": box frames copied and mapped a tick later
    uint64_t g_nextFrameUs = 0, g_boxUs = 0, g_showUs = 0, g_fpsSinceUs = 0;
    unsigned g_boxShown = 0, g_fpsFrames = 0;
    float g_fpsNow = 0;

    // ---- small helpers ----
    uint64_t NowUs()
    {
        static LARGE_INTEGER freq = {};
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return uint64_t(c.QuadPart / freq.QuadPart) * 1000000ull + uint64_t(c.QuadPart % freq.QuadPart) * 1000000ull / uint64_t(freq.QuadPart);
    }
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
    unsigned JobEffect();                   // the filmstrip job's live effect, 0 = none
    bool JobActive();
    void JobHit();                          // Radiant raised an error while the job's effect lived
    void AbortJob(bool requeue);
    int FxTimeMs() { const char* s = FxSystem(); return s ? *reinterpret_cast<const int*>(s + off_Sys_TimeMs) : 0; }
    // Radiant's own step runs with the user's camera, so the stage (far above the map) would lose every spawn it makes.
    __int64 hk_SetCamera(int client, int which, void* view)
    {
        const __int64 r = g_origSetCamera(client, which, view);
        if (g_noCull && (g_effect || JobEffect() || g_inStage > 0) && client == 0 && which == 0)
            if (const char* s = FxSystem()) *reinterpret_cast<int*>(const_cast<char*>(s) + off_Sys_PlaneCount) = 0;
        return r;
    }
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
        return p + L"\\" + Wide(e.name) + L".jpg";      // a filmstrip, see Thumbnail
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
    uint64_t g_thumbStamp = 0;          // the cache version file's time: older thumbnails are remade
    uint64_t g_runnerStamp = 0;         // runners.txt's time: older thumbnails of effects with runners are remade
    const char* kRunnerVersion = "1";   // bumped when only effects with runners (child effects) film differently
    // the .efx has a runner block (child effects), read once per session
    bool HasRunners(const Entry& e)
    {
        static std::map<std::string, bool> known;
        const std::string key = e.dir + "/" + e.name;
        const auto it = known.find(key);
        if (it != known.end()) return it->second;
        bool any = false;
        if (FILE* f = _wfopen(EfxPath(e).c_str(), L"rb"))
        {
            char line[512];
            while (!any && fgets(line, sizeof line, f)) any = !strncmp(line, "\trunner", 7) && (line[7] == '\r' || line[7] == '\n' || line[7] == 0);
            fclose(f);
        }
        return known[key] = any;
    }
    bool ThumbCurrent(const Entry& e)
    {
        uint64_t png = 0, efx = 0;
        if (!FileTime(ThumbPath(e), png) || png < g_thumbStamp) return false;
        if (png < g_runnerStamp && HasRunners(e)) return false;     // filmed with its children's light icons
        return !FileTime(EfxPath(e), efx) || png >= efx;
    }
    void MakeDirs(const std::wstring& dir)
    {
        for (size_t i = 3; i <= dir.size(); ++i)
            if (i == dir.size() || dir[i] == L'\\') CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
    }
    // <thumb root>\version.txt holds kThumbVersion, rewritten (a new time) when the version differs
    void StampFile(const wchar_t* name, const char* version, uint64_t& stamp)
    {
        const std::wstring file = g_thumbRoot + L"\\" + name;
        char have[16] = {};
        if (FILE* f = _wfopen(file.c_str(), L"rb")) { fread(have, 1, sizeof have - 1, f); fclose(f); }
        if (strcmp(have, version) != 0)
        {
            MakeDirs(g_thumbRoot);
            if (FILE* f = _wfopen(file.c_str(), L"wb")) { fputs(version, f); fclose(f); }
        }
        FileTime(file, stamp);
    }
    void StampThumbCache()
    {
        StampFile(L"version.txt", kThumbVersion, g_thumbStamp);
        StampFile(L"runners.txt", kRunnerVersion, g_runnerStamp);
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
    void SetStatus(const std::string& text) { g_lastStatus = text; }     // the pane has no status line, the command replies read it

    // ---- effects through Radiant's fx system ----
    // The compiled def handle of `name`, loading it the way an fx entity's class does: an editor def parsed from
    // fx/<name>.efx, then compiled and registered under the name. 0 when the effect does not exist.
    // A ripped effect can name child effects that never compiled: the fx step reads their defs through a null pointer
    // (Radiant crashes the same way on them). True when every child handle resolves, three levels deep.
    bool ChildrenResolve(int handle, int depth)
    {
        const char* def = rt.defByHandle(handle);
        if (!def) return false;
        if (depth > 3) return true;
        const int n = *reinterpret_cast<const short*>(def + 12) + *reinterpret_cast<const short*>(def + 14) + *reinterpret_cast<const short*>(def + 16);
        const char* elems = *reinterpret_cast<const char* const*>(def + off_Def_Elems);
        if (n <= 0 || n > 4096 || !elems) return n <= 0;
        for (int i = 0; i < n; ++i)
        {
            const char* e = elems + size_t(i) * kElemSize;
            const int kids[3] = { *reinterpret_cast<const int*>(e + off_Elem_OnDeath), *reinterpret_cast<const int*>(e + off_Elem_OnImpact), *reinterpret_cast<const int*>(e + off_Elem_Emitted) };
            for (int k : kids) if (k && !ChildrenResolve(k, depth + 1)) return false;
            if (static_cast<unsigned char>(e[off_Elem_Type]) != 14) continue;
            const int count = static_cast<unsigned char>(e[off_Elem_Children]);
            if (count == 1) { if (!ChildrenResolve(*reinterpret_cast<const int*>(e + off_Elem_Child), depth + 1)) return false; }
            else if (count > 1)
            {
                const char* list = *reinterpret_cast<const char* const*>(e + off_Elem_Child);
                if (!list) return false;
                for (int c = 0; c < count; ++c) if (!ChildrenResolve(*reinterpret_cast<const int*>(list + size_t(c) * 64), depth + 1)) return false;
            }
        }
        return true;
    }
    // the distances where an elem is spawned and fully faded in, runner children included
    void ElemBands(const char* def, int depth, std::vector<std::pair<float, float>>& out)
    {
        if (!def || depth > 2) return;
        const int n = *reinterpret_cast<const short*>(def + 12) + *reinterpret_cast<const short*>(def + 14) + *reinterpret_cast<const short*>(def + 16);
        const char* elems = *reinterpret_cast<const char* const*>(def + off_Def_Elems);
        if (n <= 0 || n > 4096 || !elems) return;
        for (int i = 0; i < n; ++i)
        {
            const char* e = elems + size_t(i) * kElemSize;
            const float* sr = reinterpret_cast<const float*>(e + off_Elem_SpawnRange);
            const float* fi = reinterpret_cast<const float*>(e + off_Elem_FadeIn);
            const float* fo = reinterpret_cast<const float*>(e + off_Elem_FadeOut);
            float lo = sr[0] > 0.0f ? sr[0] : 0.0f, hi = sr[1] > 0.0f ? sr[1] : 1e9f;
            if (fo[0] + fo[1] > lo) lo = fo[0] + fo[1];
            if (fi[0] > 0.0f && fi[0] < hi) hi = fi[0];
            if (hi < lo) hi = lo;
            out.push_back({ lo, hi });
            if (static_cast<unsigned char>(e[off_Elem_Type]) != 14) continue;
            const int count = static_cast<unsigned char>(e[off_Elem_Children]);
            if (count == 1) ElemBands(rt.defByHandle(*reinterpret_cast<const int*>(e + off_Elem_Child)), depth + 1, out);
            else if (count > 1)
                if (const char* list = *reinterpret_cast<const char* const*>(e + off_Elem_Child))
                    for (int c = 0; c < count; ++c) ElemBands(rt.defByHandle(*reinterpret_cast<const int*>(list + size_t(c) * 64)), depth + 1, out);
        }
    }
    // Radiant spawns and fades elems by camera distance (LOD sets like 0-400, 400-800): the nearest distance most elems
    // share, then the middle of their common band (at most 800 wide).
    float JudgeDistance(const char* def)
    {
        std::vector<std::pair<float, float>> bands;
        ElemBands(def, 0, bands);
        if (bands.empty()) return kMeasureDist;
        std::vector<float> cand;
        for (const auto& b : bands) { cand.push_back(b.first + 1.0f); if (b.second < 1e9f) cand.push_back((b.first + b.second) * 0.5f); }
        std::sort(cand.begin(), cand.end());
        float best = cand[0];
        size_t bestCount = 0;
        for (float c : cand)
        {
            size_t count = 0;
            for (const auto& b : bands) if (b.first <= c && c <= b.second) ++count;
            if (count > bestCount) { bestCount = count; best = c; }
        }
        float lo = 0.0f, hi = 1e9f;
        for (const auto& b : bands) if (b.first <= best && best <= b.second) { lo = std::max(lo, b.first); hi = std::min(hi, b.second); }
        hi = std::min(hi, lo + 800.0f);
        g_nearestDist = lo;
        return std::max(1.0f, (lo + hi) * 0.5f);
    }
    // drawWithViewModel elems are placed by the camera's orientation (a 1p muzzle flash, drawn at the gun): the stage def
    // draws them as world elems, so the box shows them from any angle. Put back when the stage def changes.
    std::vector<std::pair<unsigned*, unsigned>> g_vmCleared;
    void RestoreViewModelFlags()
    {
        for (const auto& c : g_vmCleared) *c.first = c.second;
        g_vmCleared.clear();
    }
    void ClearViewModelFlags(const char* def, int depth)
    {
        if (!def || depth > 2) return;
        const int n = *reinterpret_cast<const short*>(def + 12) + *reinterpret_cast<const short*>(def + 14) + *reinterpret_cast<const short*>(def + 16);
        char* elems = *reinterpret_cast<char* const*>(def + off_Def_Elems);
        if (n <= 0 || n > 4096 || !elems) return;
        for (int i = 0; i < n; ++i)
        {
            char* e = elems + size_t(i) * kElemSize;
            unsigned* flags = reinterpret_cast<unsigned*>(e);
            if (*flags & kElemViewModel) { g_vmCleared.push_back({ flags, *flags }); *flags &= ~kElemViewModel; }
            if (static_cast<unsigned char>(e[off_Elem_Type]) != 14) continue;
            const int count = static_cast<unsigned char>(e[off_Elem_Children]);
            const auto child = [&](int h) { g_stageHandles.push_back(h); ClearViewModelFlags(rt.defByHandle(h), depth + 1); };
            if (count == 1) child(*reinterpret_cast<const int*>(e + off_Elem_Child));
            else if (count > 1)
                if (const char* list = *reinterpret_cast<const char* const*>(e + off_Elem_Child))
                    for (int c = 0; c < count; ++c) child(*reinterpret_cast<const int*>(list + size_t(c) * 64));
        }
    }
    // the box's def and a thumbnail job's are on the stage together: their effects are judged from the stage camera and
    // their view model elems cleared (g_stageDefPtr: the job's while one runs)
    int g_stageBox = 0, g_stageJob = 0, g_jobDef = 0;
    void Restage()
    {
        RestoreViewModelFlags();
        g_stageHandles.clear();
        g_stageDefPtr = nullptr;
        for (int def : { g_stageBox, g_stageJob })
            if (const char* p = def ? rt.defByHandle(def) : nullptr) { g_stageHandles.push_back(def); ClearViewModelFlags(p, 0); g_stageDefPtr = p; }
        g_nearestDist = 0.0f;
        g_judgeDist = g_stageDefPtr ? JudgeDistance(g_stageDefPtr) : kMeasureDist;
    }
    void SetStageDef(int def)
    {
        if (def && def == g_jobDef) g_stageJob = def; else g_stageBox = def;
        Restage();
    }
    void StageJobDef(int def) { g_jobDef = g_stageJob = def; Restage(); }
    // The stage sits kStageAbove over the open map's highest brush, above its centre: hidden frames draw no brushes, and
    // stages far from the map lose line, beam and ring elems.
    bool g_stagePlaced = false;
    void PlaceStage()
    {
        if (g_stagePlaced) return;
        g_stagePlaced = true;
        float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
        size_t brushes = 0;
        if (rp::host->BrushBounds)
            rp::ForEachEntity([&](ModToolsEntity e) {
                rp::ForEachEntityBrush(e, [&](ModToolsBrush b) {
                    float mn[3], mx[3];
                    if (!rp::host->BrushBounds(rp::self, b, mn, mx)) return;
                    for (int i = 0; i < 3; ++i) { lo[i] = std::min(lo[i], mn[i]); hi[i] = std::max(hi[i], mx[i]); }
                    ++brushes;
                });
            });
        float at[3] = { 0, 0, 3000 };
        if (brushes) { at[0] = (lo[0] + hi[0]) * 0.5f; at[1] = (lo[1] + hi[1]) * 0.5f; at[2] = hi[2] + kStageAbove; }
        for (float& v : at) v = std::max(-kStageLimit, std::min(kStageLimit, v));
        memcpy(kStage, at, sizeof kStage);
        rp::Log("fxpreview: stage at %.0f %.0f %.0f (%zu brushes)", at[0], at[1], at[2], brushes);
    }
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
        if (!ChildrenResolve(handle, 0))
        {
            rp::Log("fxpreview: '%s' names a child effect that is not loaded, treated as unreadable", name.c_str());
            HeapFree(GetProcessHeap(), 0, holder);      // the compiled def stays registered, nothing spawns it
            return 0;
        }
        g_loaded.push_back({ name, holder, handle });
        return handle;
    }
    void ForgetDefs()
    {
        for (const Loaded& l : g_loaded) HeapFree(GetProcessHeap(), 0, l.holder);   // the compiled defs stay registered in Radiant
        g_loaded.clear();
    }
    // An effect naming an xmodel: its first load converts the model and builds its LODs on the main thread (7 to 23 s for
    // a vehicle), so only a click or the FX Browser loads it.
    std::map<std::string, bool> g_usesModels;
    std::set<int> g_modelSkipped;       // thumbnails left out for that, made once the effect was previewed
    bool UsesModels(const std::string& name)
    {
        const auto it = g_usesModels.find(name);
        if (it != g_usesModels.end()) return it->second;
        bool uses = false;
        HANDLE f = CreateFileW((g_fxRoot + L"\\" + Wide(name) + L".efx").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER size = {};
            std::string text;
            DWORD got = 0;
            if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (64 << 20))
            {
                text.resize(size_t(size.QuadPart));
                if (!ReadFile(f, &text[0], DWORD(text.size()), &got, nullptr)) got = 0;
                text.resize(got);
            }
            CloseHandle(f);
            // model { "name" }: an empty block is left out
            const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
            for (size_t p = text.find("model"); p != std::string::npos && !uses; p = text.find("model", p + 5))
            {
                if (p == 0 || !space(text[p - 1])) continue;
                size_t q = p + 5;
                while (q < text.size() && space(text[q])) ++q;
                if (q >= text.size() || text[q] != '{') continue;
                for (++q; q < text.size() && space(text[q]); ++q) {}
                uses = q + 1 < text.size() && text[q] == '"' && text[q + 1] != '"';
            }
        }
        g_usesModels[name] = uses;
        return uses;
    }
    bool NeedsModels(const std::string& name)
    {
        for (const Loaded& l : g_loaded) if (l.name == name) return false;
        return UsesModels(name);
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
    // the user's heading from the view setup, for "dbg stage here"
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
    void StageForward(float f[3])
    {
        const float p = kStageAngles[0] * 3.14159265f / 180.0f, y = kStageAngles[1] * 3.14159265f / 180.0f;
        f[0] = cosf(p) * cosf(y); f[1] = cosf(p) * sinf(y); f[2] = -sinf(p);     // the thumbnail camera stands back along this, above the stage
    }
    // the hidden camera behind the stage along its heading, at the distance the effect's fit asks for
    float FitDistance(const Fit* fit)
    {
        float dist = kMeasureDist;
        if (fit && fit->ok && fit->size > 0.001f) dist = kMeasureDist * fit->size / kFill;
        return dist < 24.0f ? 24.0f : dist > 20000.0f ? 20000.0f : dist;
    }
    void FrameCamera(float dist, float camAt[3])
    {
        float f[3];
        StageForward(f);
        for (int i = 0; i < 3; ++i) camAt[i] = kStage[i] - f[i] * dist;
    }
    void FrameCamera(const Fit* fit, float camAt[3]) { FrameCamera(FitDistance(fit), camAt); }
    const Fit* FitOf(const std::string& name)
    {
        std::map<std::string, Fit>::const_iterator it = g_fits.find(name);
        return it == g_fits.end() ? nullptr : &it->second;
    }

    // Every spawned handle ends here once: kill alone leaves two references and the slot is never reused (4096 slots).
    void Dispose(unsigned& effect)
    {
        if (!effect) return;
        if (rt.effectPtr(0, effect)) { rt.kill(effect); rt.release(effect); }
        effect = 0;
    }
    // effects in the fx system's ring, -1 when unknown
    int LiveEffects()
    {
        const char* sys = *rt.sysCount > 0 ? *rt.sysPtr : nullptr;
        const char* ring = sys ? *reinterpret_cast<char* const*>(sys + off_Sys_Ring) : nullptr;
        return ring ? *reinterpret_cast<const int*>(ring + 32) - *reinterpret_cast<const int*>(ring + 24) : -1;
    }
    bool g_scrub = false;               // a short effect: each box frame respawns it at a slowly rising age
    uint64_t g_scrubStartUs = 0;
    void Kill()
    {
        Dispose(g_effect);
        g_scrub = false;
        g_stageBox = 0;
        Restage();                      // a thumbnail job keeps its def staged
    }

    // ---- hidden frames ----
    // Radiant's idle clock holds a stale value until its first tick and then restarts near 0: a step before that leaves
    // mgr+40 at the stale time and Radiant's first tick moves the fx clock back by it (nothing draws after). So no step
    // until Radiant's own update has run (mgr+40 set) and the clock moves.
    bool g_clockLive = false;
    double g_nowSeen = -1.0;
    void WatchClock()
    {
        char* m = PlaybackMgr();
        const double now = *rt.now, last = m ? *reinterpret_cast<double*>(m + off_Mgr_LastNow) : 0.0;
        if (!m || last == 0.0 || now + 1.0 < last) g_clockLive = false;
        else if (!g_clockLive && g_nowSeen >= 0.0 && now != g_nowSeen && now - last < 60.0) g_clockLive = true;
        g_nowSeen = now;
    }
    // Advances Radiant's fx clock by `seconds` the way its idle tick does (one simulation step).
    void StepFx(double seconds)
    {
        char* m = PlaybackMgr();
        if (!m) return;
        *reinterpret_cast<double*>(m + off_Mgr_LastNow) = *rt.now - seconds;
        rt.mgrUpdate(m);
    }
    // The box's step under the stage camera: the time since Radiant's last tick (test aids: more steps, a minimum step).
    double g_minStepMs = 0.0;           // "fxpreview dbg minstep <ms>"
    int g_boxSteps = 1;                 // "fxpreview dbg steps <n>"
    int g_restoreSteps = 1;             // "fxpreview dbg restore <n>"
    double g_boxClock = 0.0;            // the box's fx clock (s), what mgr+48 holds during its steps
    double g_boxLastNow = 0.0;          // Radiant's idle time at the box's last step
    bool g_boxPaused = false;           // the preview's own pause
    int BoxMs() { return int(g_boxClock * 1000.0); }
    // before a preview starts: the box's timeline, re-based when Radiant's clock came near it
    void BoxTimeline()
    {
        const int user = FxTimeMs();
        if (Timeline(user) != 0) return;
        if (g_boxBase != LONG_MAX && user + kBoxAhead / 2 < g_boxBase) return;
        g_boxBase = user + kBoxAhead;
        g_boxClock = double(g_boxBase) / 1000.0 + 600.0;   // a start up to 10 min into a life still lands on the timeline
        g_boxLastNow = 0.0;
    }
    // A step or frame on our own clock: Radiant's clock (mgr+48), tick time (+40), time scale (+24), last repeat trigger
    // (+32) and the repeat and pause bytes (+56..+61, a far clock would retrigger the map's effects) go back after it.
    struct OwnClock
    {
        char* m = nullptr;
        double* own = nullptr;
        double clock = 0, last = 0, scale = 1, trigger = 0;
        char bytes[6] = {};
        static double& D(char* p, size_t off) { return *reinterpret_cast<double*>(p + off); }
        explicit OwnClock(double* ownClock, bool paused = false)
        {
            if (!ownClock || (m = PlaybackMgr()) == nullptr) { m = nullptr; return; }
            own = ownClock;
            clock = D(m, off_Mgr_Clock); last = D(m, off_Mgr_LastNow); scale = D(m, 24); trigger = D(m, 32);
            memcpy(bytes, m + 56, sizeof bytes);
            memset(m + 56, 0, sizeof bytes);
            m[off_Mgr_Paused] = paused ? 1 : 0;
            D(m, 24) = 1.0;
            D(m, off_Mgr_Clock) = *own;
        }
        ~OwnClock()
        {
            if (!m) return;
            *own = D(m, off_Mgr_Clock);
            D(m, off_Mgr_Clock) = clock; D(m, off_Mgr_LastNow) = last; D(m, 24) = scale; D(m, 32) = trigger;
            memcpy(m + 56, bytes, sizeof bytes);
        }
    };
    // inside the box's OwnClock: the real time since the box's last step
    void StepBox()
    {
        char* m = PlaybackMgr();
        if (!m) return;
        double& last = *reinterpret_cast<double*>(m + off_Mgr_LastNow);
        const double now = *rt.now;
        last = g_boxLastNow > 0.0 && now >= g_boxLastNow && now - g_boxLastNow < 0.25 ? g_boxLastNow : now - 1.0 / 30.0;
        g_boxLastNow = now;
        for (int i = 0; i < g_boxSteps; ++i)
        {
            const double floor = *rt.now - g_minStepMs / 1000.0;
            if (last > floor) last = floor;
            rt.mgrUpdate(m);
        }
    }
    // A frame draws fx data built from the last step's camera: after a box frame, a zero-time step with the user's camera
    // so the user's next frame does not draw the map's effects from the stage.
    void RestoreUserSteps()
    {
        char* m = PlaybackMgr();
        if (!m || g_inStage > 0) return;
        double& last = *reinterpret_cast<double*>(m + off_Mgr_LastNow);
        for (int i = 0; i < g_restoreSteps; ++i)
        {
            const double floor = *rt.now - g_minStepMs / 1000.0;
            if (last > floor) last = floor;
            rt.mgrUpdate(m);
        }
    }

    // ---- GPU downscale: a back buffer box copied into mip 0 of a mipped texture, the smaller mips made by the GPU ----
    struct MipTex { ID3D11Texture2D* tex; ID3D11ShaderResourceView* srv; int w, h, levels; DXGI_FORMAT fmt; };
    MipTex g_mipStrip = {}, g_mipLook = {};
    bool g_gpuShrink = true;            // "fxpreview dbg gpushrink 0|1"
    struct StageTex { ID3D11Texture2D* tex; int w, h; DXGI_FORMAT fmt; };
    StageTex g_lookStage = {};
    void ReleaseMip(MipTex& m) { if (m.srv) m.srv->Release(); if (m.tex) m.tex->Release(); m = MipTex(); }
    DXGI_FORMAT TypedFormat(DXGI_FORMAT f)
    {
        if (f == DXGI_FORMAT_R8G8B8A8_TYPELESS) return DXGI_FORMAT_R8G8B8A8_UNORM;
        if (f == DXGI_FORMAT_B8G8R8A8_TYPELESS) return DXGI_FORMAT_B8G8R8A8_UNORM;
        return f;
    }
    // False when the format cannot make mips (the caller copies at full size)
    bool MipDown(MipTex& m, ID3D11Texture2D* back, DXGI_FORMAT fmt, const D3D11_BOX& src, int levels)
    {
        ID3D11Device* dev = *rt.dxDevice;
        ID3D11DeviceContext* ctx = *rt.dxContext;
        const int w = int(src.right - src.left), h = int(src.bottom - src.top);
        const DXGI_FORMAT typed = TypedFormat(fmt);
        if (!m.tex || m.w != w || m.h != h || m.levels != levels || m.fmt != typed)
        {
            ReleaseMip(m);
            UINT support = 0;
            if (FAILED(dev->CheckFormatSupport(typed, &support)) || !(support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN)) return false;
            D3D11_TEXTURE2D_DESC d = {};
            d.Width = UINT(w); d.Height = UINT(h); d.MipLevels = UINT(levels); d.ArraySize = 1; d.Format = typed;
            d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            if (FAILED(dev->CreateTexture2D(&d, nullptr, &m.tex)) || !m.tex) { m = MipTex(); return false; }
            if (FAILED(dev->CreateShaderResourceView(m.tex, nullptr, &m.srv)) || !m.srv) { ReleaseMip(m); return false; }
            m.w = w; m.h = h; m.levels = levels; m.fmt = typed;
        }
        ctx->CopySubresourceRegion(m.tex, 0, 0, 0, 0, back, 0, &src);
        ctx->GenerateMips(m.srv);
        return true;
    }
    void ReleaseLookTex() { ReleaseMip(g_mipLook); if (g_lookStage.tex) g_lookStage.tex->Release(); g_lookStage = StageTex(); }

    // Reads the camera view's back buffer as BGRA rows, at half size when `half` and the GPU can make mips (g_readHalf
    // says which). False when the format is not 8-bit RGBA/BGRA.
    bool g_readHalf = false;
    bool ReadBackBuffer(std::vector<unsigned char>& bgra, int& w, int& h, bool half = false)
    {
        g_readHalf = false;
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
        const D3D11_BOX even = { 0, 0, 0, desc.Width & ~1u, desc.Height & ~1u, 1 };
        if ((rgba || isBgra) && half && g_gpuShrink && desc.SampleDesc.Count == 1 && desc.Width >= 32 && desc.Height >= 32 && MipDown(g_mipLook, back, desc.Format, even, 2))
        {
            const int hw = int(even.right / 2), hh = int(even.bottom / 2);
            if (!g_lookStage.tex || g_lookStage.w != hw || g_lookStage.h != hh || g_lookStage.fmt != g_mipLook.fmt)
            {
                if (g_lookStage.tex) g_lookStage.tex->Release();
                g_lookStage = StageTex();
                D3D11_TEXTURE2D_DESC st = {};
                st.Width = UINT(hw); st.Height = UINT(hh); st.MipLevels = 1; st.ArraySize = 1; st.Format = g_mipLook.fmt;
                st.SampleDesc.Count = 1; st.Usage = D3D11_USAGE_STAGING; st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (SUCCEEDED(dev->CreateTexture2D(&st, nullptr, &g_lookStage.tex)) && g_lookStage.tex) { g_lookStage.w = hw; g_lookStage.h = hh; g_lookStage.fmt = g_mipLook.fmt; }
                else g_lookStage = StageTex();
            }
            if (g_lookStage.tex)
            {
                ctx->CopySubresourceRegion(g_lookStage.tex, 0, 0, 0, 0, g_mipLook.tex, D3D11CalcSubresource(1, 0, 2), nullptr);
                D3D11_MAPPED_SUBRESOURCE map = {};
                if (SUCCEEDED(ctx->Map(g_lookStage.tex, 0, D3D11_MAP_READ, 0, &map)))
                {
                    w = hw; h = hh;
                    bgra.resize(size_t(w) * h * 4);
                    for (int y = 0; y < h; ++y)
                    {
                        const unsigned char* src = static_cast<const unsigned char*>(map.pData) + size_t(y) * map.RowPitch;
                        unsigned char* dst = bgra.data() + size_t(y) * w * 4;
                        if (isBgra) memcpy(dst, src, size_t(w) * 4);
                        else for (int x = 0; x < w; ++x) { dst[x * 4] = src[x * 4 + 2]; dst[x * 4 + 1] = src[x * 4 + 1]; dst[x * 4 + 2] = src[x * 4]; dst[x * 4 + 3] = 255; }
                    }
                    ctx->Unmap(g_lookStage.tex, 0);
                    ok = g_readHalf = true;
                }
            }
        }
        if (!ok && (rgba || isBgra))
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
        else if (!rgba && !isBgra) rp::Log("fxpreview: back buffer format %d is not 8-bit RGBA", int(desc.Format));
        back->Release();
        return ok;
    }

    // One camera frame drawn into the back buffer and never presented, its pixels in `out`.
    // ---- Radiant's playback update, quiet while a preview effect lives ----
    typedef void (*MgrUpdateRawFn)(void* mgr);
    MgrUpdateRawFn g_origMgrUpdate = nullptr;
    unsigned g_ownUpdates = 0, g_userFrames = 0;    // Radiant's own fx updates and user frames (the "dbg seq" trace)
    void hk_MgrUpdate(void* mgr)
    {
        if (g_inStage == 0) ++g_ownUpdates;
        if (!g_effect && !JobEffect()) { g_origMgrUpdate(mgr); return; }
        bool hit = false;
        {
            QuietScope quiet;
            g_origMgrUpdate(mgr);
            hit = quiet.Hit();
        }
        if (hit && JobEffect()) JobHit();
        if (hit && g_effect)
        {
            rp::Log("fxpreview: '%s' raised a Radiant error while playing, stopped and not played again", g_name.c_str());
            g_bad.insert(g_name);
            Kill();
        }
    }

    // ---- the camera's auto exposure stays out of hidden frames ----
    typedef void (*ExposureAdaptFn)(void* cam, char a2, float a3, void* a4, char a5, float a6, char a7, float a8, float a9);
    ExposureAdaptFn g_origExposureAdapt = nullptr;
    unsigned g_readbacksDropped = 0;
    int g_discardLeft = 0;              // the user's next readbacks to throw away after hidden frames
    int g_discardAfter = 3;             // "fxpreview dbg discard <n>"
    int g_dropTraceLeft = 0;            // "fxpreview dbg droplog <n>"
    int g_expoTraceLeft = 0;            // "fxpreview dbg expo <n>": log the next n user frames' exposure
    unsigned g_hiddenSince = 0;         // hidden frames since the user's last frame
    uint64_t g_lastUserUs = 0;          // the user's last frame (NowUs)
    bool g_exposureGuard = true;        // "fxpreview dbg exposure 0|1"
    float g_fixedEv = 15.6f;            // [fxpreview] exposure: hidden frames' EV (cam+3712, scale 2^(EV-3) at +3716), 0 = the 3D view's
    void ReleaseShared(void* sp[2]);
    void hk_ExposureAdapt(void* cam, char a2, float a3, void* a4, char a5, float a6, char a7, float a8, float a9)
    {
        if (g_thumbFrame && g_exposureGuard) return;      // a hidden frame neither pops the user's readbacks nor moves the exposure
        if (g_discardLeft > 0 && g_exposureGuard && cam)   // the user's first frames after hidden ones measure wrong: dropped, exposure held
        {
            typedef void** (*PopFn)(void* list, void* out[2]);
            static PopFn popReady = At<PopFn>(kReadbackPopReady);
            char* list = static_cast<char*>(cam) + off_Cam_Readbacks;
            while (popReady && g_discardLeft > 0 && *reinterpret_cast<size_t*>(list + 8) > 0)
            {
                void* sp[2] = { nullptr, nullptr };
                popReady(list, sp);
                const bool popped = sp[0] != nullptr;   // not ready: an empty shared_ptr (with a control block), nothing popped
                ReleaseShared(sp);
                if (!popped) break;
                --g_discardLeft;
            }
            g_hiddenSince = 0;
            ++g_userFrames;
            g_lastUserUs = NowUs();
            return;
        }
        ++g_userFrames;
        g_lastUserUs = NowUs();
        const size_t before = cam ? *reinterpret_cast<size_t*>(static_cast<char*>(cam) + off_Cam_Readbacks + 8) : 0;
        g_origExposureAdapt(cam, a2, a3, a4, a5, a6, a7, a8, a9);
        if (g_expoTraceLeft > 0 && cam)
        {
            const float* e = reinterpret_cast<const float*>(static_cast<char*>(cam) + off_Cam_Exposure);
            const size_t after = *reinterpret_cast<size_t*>(static_cast<char*>(cam) + off_Cam_Readbacks + 8);
            rp::Log("fxpreview: expo %llu ev %.3f scale %.2f a3 %.3f readbacks %zu->%zu hidden-since %u", (unsigned long long)GetTickCount64(), e[0], e[1], a3, before, after, g_hiddenSince);
            --g_expoTraceLeft;
        }
        g_hiddenSince = 0;
    }
    void ReleaseShared(void* sp[2])
    {
        char* ctrl = static_cast<char*>(sp[1]);
        if (!ctrl) return;
        void** vtbl = *reinterpret_cast<void***>(ctrl);
        if (_InterlockedDecrement(reinterpret_cast<volatile long*>(ctrl + 8)) == 0)
        {
            reinterpret_cast<void (*)(void*)>(vtbl[0])(ctrl);
            if (_InterlockedDecrement(reinterpret_cast<volatile long*>(ctrl + 12)) == 0) reinterpret_cast<void (*)(void*)>(vtbl[1])(ctrl);
        }
        sp[0] = sp[1] = nullptr;
    }
    // Drops the camera's exposure readbacks: the entry a hidden frame pushed would steer the user's exposure.
    // wait: every entry (thumbnails, their pixels were read so the GPU is done). Else only finished ones, plus a backlog.
    // Returns the entries left, which the user's next frames drop.
    size_t DropReadbacks(char* cam, bool wait)
    {
        typedef void** (*PopFn)(void* list, void* out[2]);
        static PopFn popWait = At<PopFn>(kReadbackPopWait), popReady = At<PopFn>(kReadbackPopReady);
        if (!cam) return 0;
        char* list = cam + off_Cam_Readbacks;
        const size_t* count = reinterpret_cast<const size_t*>(list + 8);
        const size_t before = *count;
        const unsigned droppedBefore = g_readbacksDropped;
        struct Trace { size_t before; const size_t* count; const unsigned* dropped; unsigned base; ~Trace() { if (g_dropTraceLeft > 0) { --g_dropTraceLeft; rp::Log("fxpreview: readbacks %zu -> %zu, dropped %u", before, *count, *dropped - base); } } }
            trace = { before, count, &g_readbacksDropped, droppedBefore };
        for (int guard = 0; guard < 16 && *count > 0; ++guard)
        {
            PopFn pop = wait || *count > 4 ? popWait : popReady;   // an entry 4 frames old is finished
            if (!pop) break;
            void* sp[2] = { nullptr, nullptr };
            pop(list, sp);
            const bool popped = sp[0] != nullptr;   // not finished: an empty shared_ptr (with a control block), nothing popped
            ReleaseShared(sp);
            if (!popped) break;
            ++g_readbacksDropped;
        }
        return *count;
    }

    struct BoxView;
    bool CopyBoxRegion(const BoxView& view);
    bool g_stripOn = false;             // hidden frames copy into the next filmstrip slice, not the box's slots
    bool CopyStripRegion();
    // 0 = not drawn, 1 = the frame's pixels in `out`, 2 = box mode: the crop is on its way to a staging texture
    bool g_halfLooks = false;           // set by Looks: its frames are read back at half size
    int HiddenFrame(std::vector<unsigned char>& out, int& w, int& h, const BoxView* box = nullptr)
    {
        void* cam = CamWnd();
        if (!cam || *rt.viewIndex != 7) return false;
        if (!rt.viewSet(0)) return false;
        int ok = 0;
        float exposure[3];
        memcpy(exposure, static_cast<char*>(cam) + off_Cam_Exposure, sizeof exposure);
        if (g_fixedEv > 0.0f)       // the box and thumbnails look the same whatever the 3D view's auto exposure is doing
        {
            float* e = reinterpret_cast<float*>(static_cast<char*>(cam) + off_Cam_Exposure);
            e[0] = g_fixedEv;
            e[1] = exp2f(g_fixedEv - 3.0f);
        }
        g_thumbFrame = true;
        g_thumbThread = GetCurrentThreadId();
        rt.brushCacheUpd();
        rt.poolA();
        rt.poolB();
        if (rt.beginFrame())
        {
            rt.setTargets(rt.viewTarget(0), rt.viewDepth(0), 0, 0);
            rt.clear(7, kNeutral, 1.0f);
            rt.camDraw(cam);
            rt.endFrame();
            if (box && (g_stripOn ? CopyStripRegion() : CopyBoxRegion(*box))) ok = 2;
            else ok = ReadBackBuffer(out, w, h, g_halfLooks) ? 1 : 0;
            const size_t left = g_exposureGuard ? DropReadbacks(static_cast<char*>(cam), ok != 2) : 0;
            ++g_hiddenSince;
            g_discardLeft = int(left) + g_discardAfter;     // the user's frames drop what is left, then their next few
        }
        g_thumbFrame = false;
        if (g_exposureGuard || g_fixedEv > 0.0f) memcpy(static_cast<char*>(cam) + off_Cam_Exposure, exposure, sizeof exposure);
        *rt.viewIndex = 7;      // what Present would have done
        return ok;
    }

    // "fxpreview dbg gameview 0" keeps the game view on for stage frames (isolating stage problems)
    bool g_dbgNoGameView = false;
    // the user's camera, game view and background colour, put back when the scope ends. Game view goes off because its
    // view setup does not take the camera from the origin and angles the editor view reads.
    struct StageView
    {
        char* cam; float origin[3], angles[3], back[4]; int gameView; char sunLock;
        explicit StageView(const float camAt[3], const float* heading = nullptr) : cam(CamWnd()), gameView(0), sunLock(0)
        {
            ++g_inStage;
            if (char* lock = At<char*>(kLockSunVolume)) { sunLock = *lock; *lock = 1; }   // no sun volume up there: the user's stays active
            memcpy(back, rt.colors + kCameraBackRow * 4, sizeof back);     // Cam_Draw clears with this row
            memcpy(rt.colors + kCameraBackRow * 4, kNeutral, sizeof kNeutral);
            if (!g_dbgNoGameView && rp::host->GameView && rp::host->SetGameView) { gameView = rp::host->GameView(rp::self); if (gameView) rp::host->SetGameView(rp::self, 0); }
            if (!cam) return;
            memcpy(origin, cam + off_Cam_Origin, sizeof origin);
            memcpy(angles, cam + off_Cam_Angles, sizeof angles);
            memcpy(cam + off_Cam_Origin, camAt, sizeof origin);
            memcpy(cam + off_Cam_Angles, heading ? heading : kStageAngles, sizeof angles);
            if (rt.anglesToAxes) rt.anglesToAxes(cam + 200);            // the cached axes turn with the angles
        }
        ~StageView()
        {
            --g_inStage;
            if (char* lock = At<char*>(kLockSunVolume)) *lock = sunLock;
            memcpy(rt.colors + kCameraBackRow * 4, back, sizeof back);
            if (gameView) rp::host->SetGameView(rp::self, 1);
            if (!cam) return;
            memcpy(cam + off_Cam_Origin, origin, sizeof origin);
            memcpy(cam + off_Cam_Angles, angles, sizeof angles);
            if (rt.anglesToAxes) rt.anglesToAxes(cam + 200);
        }
    };

    // pixels that are not the plain background, their trimmed box (3 % of them cut from each side) as a Fit
    bool MeasureFrame(const std::vector<unsigned char>& bgra, int w, int h, Fit& fit, int step = 2)
    {
        fit = Fit();
        if (w <= 0 || h <= 0) return false;
        std::vector<int> cols(size_t(w), 0), rows(size_t(h), 0);
        long total = 0;
        for (int y = 0; y < h; y += step)
        {
            const unsigned char* row = bgra.data() + size_t(y) * w * 4;
            for (int x = 0; x < w; x += step)
            {
                const unsigned char* px = row + size_t(x) * 4;
                if (abs(int(px[0]) - 46) + abs(int(px[1]) - 46) + abs(int(px[2]) - 46) > 40) { ++cols[size_t(x)]; ++rows[size_t(y)]; ++total; }
            }
        }
        if (total < 2) return false;
        const long cut = long(total * 0.03);
        int x0 = 0, x1 = w - 1, y0 = 0, y1 = h - 1;
        for (long c = 0; x0 < w - 1 && c + cols[size_t(x0)] <= cut; ++x0) c += cols[size_t(x0)];
        for (long c = 0; x1 > x0 && c + cols[size_t(x1)] <= cut; --x1) c += cols[size_t(x1)];
        for (long c = 0; y0 < h - 1 && c + rows[size_t(y0)] <= cut; ++y0) c += rows[size_t(y0)];
        for (long c = 0; y1 > y0 && c + rows[size_t(y1)] <= cut; --y1) c += rows[size_t(y1)];
        fit.ok = true;
        fit.cx = (float(x0 + x1) * 0.5f - float(w) * 0.5f) / float(h);
        fit.cy = (float(y0 + y1) * 0.5f - float(h) * 0.5f) / float(h);
        const int side = (x1 - x0) > (y1 - y0) ? (x1 - x0) : (y1 - y0);
        fit.size = float(side + 2) / float(h);
        return true;
    }

    // a square of `side` pixels around (cx, cy), kept inside the frame, as a QImage (64-byte object buffer) scaled to `size`
    void CropImage(const std::vector<unsigned char>& bgra, int w, int h, float cx, float cy, int side, int size, char out[64])
    {
        const int limit = h < w ? h : w;
        if (side > limit) side = limit;
        if (side < 16) side = 16;
        int x = int(cx) - side / 2, y = int(cy) - side / 2;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x + side > w) x = w - side;
        if (y + side > h) y = h - side;
        alignas(16) char img[64], cropped[64];
        qt.imageCtor(img, bgra.data(), w, h, w * 4, 4 /* RGB32 */, nullptr, nullptr);
        qt.imageCopy(img, cropped, x, y, side, side);
        qt.imageScaled(cropped, out, size, size, 1 /* KeepAspectRatio */, 1 /* Smooth */);
        qt.imageDtor(cropped);
        qt.imageDtor(img);
    }
    // the thumbnail: a square 1.4 times the fit's box (at least 12 % of the height) around its centre
    bool SavePng(const std::vector<unsigned char>& bgra, int w, int h, const Fit& fit, int size, const std::wstring& path)
    {
        if (w <= 0 || h <= 0) return false;
        float cx = float(w) * 0.5f, cy = float(h) * 0.5f, share = kBoxCrop;
        const float k = g_fitScale > 0.0f ? 1.0f / g_fitScale : 1.0f;
        if (fit.ok) { cx += fit.cx * k * float(h); cy += fit.cy * k * float(h); share = fit.size * k * 1.4f; }
        if (share < 0.12f) share = 0.12f;
        alignas(16) char scaled[64], qs[16];
        CropImage(bgra, w, h, cx, cy, int(share * float(h)), size, scaled);
        qt.stringCtor(qs, Utf8(path.c_str(), int(path.size())).c_str());
        const bool ok = qt.imageSave(scaled, qs, "PNG", -1);
        qt.stringDtor(qs);
        qt.imageDtor(scaled);
        return ok;
    }

    // ---- the box camera: an orbit around the effect's measured centre ----
    const float kDeg = 3.14159265f / 180.0f;
    // the view a camera's angles (cam+220) give: positive pitch looks UP, yaw 90 looks along +y
    void Basis(const float ang[3], float f[3], float r[3], float u[3])
    {
        const float p = ang[0] * kDeg, y = ang[1] * kDeg;
        f[0] = cosf(p) * cosf(y); f[1] = cosf(p) * sinf(y); f[2] = sinf(p);
        r[0] = sinf(y); r[1] = -cosf(y); r[2] = 0;
        u[0] = r[1] * f[2] - r[2] * f[1]; u[1] = r[2] * f[0] - r[0] * f[2]; u[2] = r[0] * f[1] - r[1] * f[0];
    }
    struct BoxView { float eye[3], ang[3], f[3], r[3], u[3], pivot[3], dist, tanV, size; };
    // The fit's look stood kMeasureDist * scale back along StageForward and viewed with kStageAngles: its frame offset is
    // cast back through that camera to the stage's depth. The orbit pitch is the user's (positive looks down).
    // home: the reset orbit (thumbnails), not the user's
    BoxView MakeBoxView(const Fit* fit, bool home = false)
    {
        BoxView v = {};
        const bool ok = fit && fit->ok && fit->scale > 0.0f;
        const float none[3] = { 0, 0, 0 };
        const float* pan = home ? none : g_pan;
        for (int i = 0; i < 3; ++i) v.pivot[i] = kStage[i] + pan[i];
        if (ok)
        {
            float back[3], f0[3], r0[3], u0[3], eye[3], rel[3];
            StageForward(back);
            Basis(kStageAngles, f0, r0, u0);
            for (int i = 0; i < 3; ++i) { eye[i] = kStage[i] - back[i] * kMeasureDist * fit->scale; rel[i] = kStage[i] - eye[i]; }
            const float depth = rel[0] * f0[0] + rel[1] * f0[1] + rel[2] * f0[2];
            const float tx = fit->cx / fit->scale * 2.0f * g_tanV, ty = fit->cy / fit->scale * 2.0f * g_tanV;
            for (int i = 0; i < 3; ++i) v.pivot[i] = eye[i] + depth * (f0[i] + r0[i] * tx - u0[i] * ty) + pan[i];
        }
        v.ang[0] = home ? -kStageAngles[0] : -g_orbitPitch; v.ang[1] = home ? kStageAngles[1] : g_orbitYaw; v.ang[2] = 0;
        Basis(v.ang, v.f, v.r, v.u);
        v.dist = FitDistance(fit) * (home ? 1.0f : g_orbitZoom);
        if (v.dist < 4.0f) v.dist = 4.0f;
        if (v.dist < g_nearestDist * 1.1f) v.dist = g_nearestDist * 1.1f;
        for (int i = 0; i < 3; ++i) v.eye[i] = v.pivot[i] - v.f[i] * v.dist;
        v.tanV = g_tanV;
        v.size = ok ? fit->size * 2.0f * g_tanV * kMeasureDist : 0.0f;
        return v;
    }
    // inside a StageView: Cam_Draw's own view setup, +0x30 = tan(vfov/2), else the FOV preference
    float g_tanFov = 0.0f;              // from Cam_GetFov, for "status"
    void ReadTanV()
    {
        static float (*getFov)() = At<float (*)()>(kCamGetFov);
        typedef float* (*ViewSetupFn)(void* cam, float* out);
        static ViewSetupFn setup = At<ViewSetupFn>(kViewSetup);
        const float fov = getFov ? getFov() : 0.0f;
        if (fov > 5.0f && fov < 170.0f) g_tanFov = tanf(fov * 0.5f * kDeg) * 9.0f / 16.0f;
        float v[64] = {};
        if (setup && CamWnd()) setup(CamWnd(), v);
        if (v[12] > 0.05f && v[12] < 5.0f) g_tanV = v[12];
        else if (g_tanFov > 0.0f) g_tanV = g_tanFov;
    }
    void ResetOrbit()
    {
        g_orbitYaw = kStageAngles[1]; g_orbitPitch = kStageAngles[0]; g_orbitZoom = 1.0f;
        g_pan[0] = g_pan[1] = g_pan[2] = 0;
        g_boxDirty = true;
    }

    // ---- the box image: the frame's centre square, scaled down, the grid behind the effect ----
    struct Overlay
    {
        int size = 0;
        std::vector<float> px;          // premultiplied rgb (0-1) and alpha
        explicit Overlay(int n) : size(n), px(size_t(n) * n * 4, 0.0f) {}
        void Plot(int x, int y, const float c[3], float a)
        {
            if (x < 0 || y < 0 || x >= size || y >= size || a <= 0.0f) return;
            float* p = &px[(size_t(y) * size + x) * 4];
            for (int i = 0; i < 3; ++i) p[i] = p[i] * (1.0f - a) + c[i] * a;
            p[3] = p[3] * (1.0f - a) + a;
        }
        // Liang-Barsky against the image, then two pixels across the line per step by coverage
        void Line(float x0, float y0, float x1, float y1, const float c[3], float a)
        {
            const float lo = -1.0f, hi = float(size);
            float t0 = 0, t1 = 1;
            const float dx = x1 - x0, dy = y1 - y0;
            const float p[4] = { -dx, dx, -dy, dy }, q[4] = { x0 - lo, hi - x0, y0 - lo, hi - y0 };
            for (int i = 0; i < 4; ++i)
            {
                if (fabsf(p[i]) < 1e-6f) { if (q[i] < 0) return; continue; }
                const float t = q[i] / p[i];
                if (p[i] < 0) { if (t > t1) return; if (t > t0) t0 = t; }
                else { if (t < t0) return; if (t < t1) t1 = t; }
            }
            float ax = x0 + t0 * dx, ay = y0 + t0 * dy, bx = x0 + t1 * dx, by = y0 + t1 * dy;
            const bool steep = fabsf(by - ay) > fabsf(bx - ax);
            if (steep) { std::swap(ax, ay); std::swap(bx, by); }
            if (ax > bx) { std::swap(ax, bx); std::swap(ay, by); }
            const float grad = bx - ax > 1e-4f ? (by - ay) / (bx - ax) : 0.0f;
            const int xs = int(floorf(ax + 0.5f)), xe = int(floorf(bx + 0.5f));
            float y = ay + grad * (float(xs) - ax) - 0.5f;
            for (int x = xs; x <= xe; ++x, y += grad)
            {
                const int yi = int(floorf(y));
                const float fr = y - float(yi);
                if (steep) { Plot(yi, x, c, a * (1.0f - fr)); Plot(yi + 1, x, c, a * fr); }
                else { Plot(x, yi, c, a * (1.0f - fr)); Plot(x, yi + 1, c, a * fr); }
            }
        }
        // 5x7 glyph, rows top down, bit 4 = left column
        void Glyph(int x, int y, const unsigned char rows[7], const float c[3])
        {
            for (int j = 0; j < 7; ++j)
                for (int i = 0; i < 5; ++i)
                    if (rows[j] & (16 >> i)) Plot(x + i, y + j, c, 1.0f);
        }
    };
    // world -> box pixels for the view a frame was drawn with: the frame's projection, its crop square, the box scale
    struct Projector
    {
        BoxView v; float focal, cx, cy, x0, y0, scale;
        bool Project(const float w[3], float& sx, float& sy, float nearZ, float c[3]) const
        {
            const float d[3] = { w[0] - v.eye[0], w[1] - v.eye[1], w[2] - v.eye[2] };
            c[0] = d[0] * v.r[0] + d[1] * v.r[1] + d[2] * v.r[2];
            c[1] = d[0] * v.u[0] + d[1] * v.u[1] + d[2] * v.u[2];
            c[2] = d[0] * v.f[0] + d[1] * v.f[1] + d[2] * v.f[2];
            if (c[2] < nearZ) return false;
            sx = (cx + c[0] / c[2] * focal - x0) * scale;
            sy = (cy - c[1] / c[2] * focal - y0) * scale;
            return true;
        }
        void ToBox(const float c[3], float& sx, float& sy) const
        {
            sx = (cx + c[0] / c[2] * focal - x0) * scale;
            sy = (cy - c[1] / c[2] * focal - y0) * scale;
        }
        void WorldLine(Overlay& o, const float a[3], const float b[3], const float col[3], float alpha) const
        {
            const float nearZ = 1.0f;
            float ca[3], cb[3], sx, sy;
            const bool ina = Project(a, sx, sy, nearZ, ca), inb = Project(b, sx, sy, nearZ, cb);
            if (!ina && !inb) return;
            if (!ina || !inb)          // clip to the near plane in camera space
            {
                float* out = ina ? cb : ca;
                const float* in = ina ? ca : cb;
                const float t = (nearZ - in[2]) / (out[2] - in[2]);
                for (int i = 0; i < 3; ++i) out[i] = in[i] + (out[i] - in[i]) * t;
            }
            float ax, ay, bx, by;
            ToBox(ca, ax, ay);
            ToBox(cb, bx, by);
            o.Line(ax, ay, bx, by, col, alpha);
        }
    };
    const unsigned char kGlyphX[7] = { 17, 17, 10, 4, 10, 17, 17 }, kGlyphY[7] = { 17, 17, 10, 4, 4, 4, 4 }, kGlyphZ[7] = { 31, 1, 2, 4, 8, 16, 31 };
    // ground grid through the effect's origin (about 4 cells across the effect or the view, whichever is larger), X red, Y green, Z blue
    void DrawGrid(Overlay& o, const Projector& P)
    {
        const float extent = std::max(P.v.size > 1.0f ? P.v.size : 32.0f, P.v.dist * P.v.tanV);
        float step = 1.0f;
        while (step * 4.0f < extent) step *= 2.0f;
        const int n = 10;
        const float half = step * n, gx = kStage[0], gy = kStage[1], gz = kStage[2];
        const float grey[3] = { 0.62f, 0.62f, 0.62f };
        for (int i = -n; i <= n; ++i)
        {
            if (i == 0) continue;
            const float a = i % 4 == 0 ? 0.45f : 0.22f;
            const float p0[3] = { gx + i * step, gy - half, gz }, p1[3] = { gx + i * step, gy + half, gz };
            const float q0[3] = { gx - half, gy + i * step, gz }, q1[3] = { gx + half, gy + i * step, gz };
            P.WorldLine(o, p0, p1, grey, a);
            P.WorldLine(o, q0, q1, grey, a);
        }
        const float red[3] = { 0.86f, 0.27f, 0.27f }, green[3] = { 0.35f, 0.78f, 0.35f }, blue[3] = { 0.35f, 0.51f, 0.94f };
        const float x0[3] = { gx - half, gy, gz }, x1[3] = { gx + half, gy, gz };
        const float y0[3] = { gx, gy - half, gz }, y1[3] = { gx, gy + half, gz };
        const float z0[3] = { gx, gy, gz }, z1[3] = { gx, gy, gz + step * 4.0f };
        P.WorldLine(o, x0, x1, red, 0.85f);
        P.WorldLine(o, y0, y1, green, 0.85f);
        P.WorldLine(o, z0, z1, blue, 0.85f);
    }
    // the view's axes in the bottom-left corner, farthest first
    void DrawGizmo(Overlay& o, const BoxView& v)
    {
        const float axes[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
        const float cols[3][3] = { { 0.95f, 0.35f, 0.35f }, { 0.45f, 0.9f, 0.45f }, { 0.45f, 0.6f, 1.0f } };
        const unsigned char* glyphs[3] = { kGlyphX, kGlyphY, kGlyphZ };
        int order[3] = { 0, 1, 2 };
        std::sort(order, order + 3, [&](int a, int b) { return axes[a][0] * v.f[0] + axes[a][1] * v.f[1] + axes[a][2] * v.f[2] > axes[b][0] * v.f[0] + axes[b][1] * v.f[1] + axes[b][2] * v.f[2]; });
        const float s = o.size < 200 ? 0.65f : 1.0f;   // thumbnails get a smaller one
        const float cx = 24.0f * s, cy = float(o.size) - 24.0f * s, len = 16.0f * s;
        for (int k : order)
        {
            const float* a = axes[k];
            const float dx = a[0] * v.r[0] + a[1] * v.r[1] + a[2] * v.r[2], dy = -(a[0] * v.u[0] + a[1] * v.u[1] + a[2] * v.u[2]);
            o.Line(cx, cy, cx + dx * len, cy + dy * len, cols[k], 1.0f);
            o.Glyph(int(cx + dx * (len + 6.0f)) - 2, int(cy + dy * (len + 6.0f)) - 3, glyphs[k], cols[k]);
        }
    }
    // area average of a side x side BGRA square into size x size
    void Shrink(const unsigned char* src, int side, unsigned char* dst, int size)
    {
        std::vector<int> edge(size_t(size) + 1);
        for (int i = 0; i <= size; ++i) edge[size_t(i)] = int((long long)i * side / size);
        for (int y = 0; y < size; ++y)
        {
            const int ya = edge[size_t(y)], yb = std::min(side, std::max(edge[size_t(y) + 1], ya + 1));
            for (int x = 0; x < size; ++x)
            {
                const int xa = edge[size_t(x)], xb = std::min(side, std::max(edge[size_t(x) + 1], xa + 1));
                unsigned sum[3] = { 0, 0, 0 }, n = 0;
                for (int yy = ya; yy < yb; ++yy)
                {
                    const unsigned char* p = src + (size_t(yy) * side + xa) * 4;
                    for (int xx = xa; xx < xb; ++xx, p += 4) { sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; ++n; }
                }
                unsigned char* d = dst + (size_t(y) * size + x) * 4;
                if (!n) n = 1;
                d[0] = (unsigned char)(sum[0] / n); d[1] = (unsigned char)(sum[1] / n); d[2] = (unsigned char)(sum[2] / n); d[3] = 255;
            }
        }
    }
    // the frame's clear colour after exposure: the median of the image border (BGR)
    void BorderColor(const unsigned char* img, int size, int bg[3])
    {
        std::vector<unsigned char> ch[3];
        for (int i = 0; i < size; i += 2)
        {
            const size_t at[4] = { size_t(i), size_t(size - 1) * size + i, size_t(i) * size, size_t(i) * size + size - 1 };
            for (size_t k : at)
                for (int c = 0; c < 3; ++c) ch[c].push_back(img[k * 4 + c]);
        }
        for (int c = 0; c < 3; ++c)
        {
            if (ch[c].empty()) { bg[c] = 46; continue; }
            std::nth_element(ch[c].begin(), ch[c].begin() + ch[c].size() / 2, ch[c].end());
            bg[c] = ch[c][ch[c].size() / 2];
        }
    }
    // the grid shows where the frame is plain background: a lit or dark effect pixel covers it
    void Composite(unsigned char* img, const Overlay& o, bool behind, const int bg[3] = nullptr)
    {
        static const int kGrey[3] = { 46, 46, 46 };
        if (!bg) bg = kGrey;
        for (size_t i = 0, n = size_t(o.size) * o.size; i < n; ++i)
        {
            const float* p = &o.px[i * 4];
            if (p[3] < 0.004f) continue;
            unsigned char* d = img + i * 4;
            float w = 1.0f;
            if (behind)
            {
                const int diff = abs(int(d[0]) - bg[0]) + abs(int(d[1]) - bg[1]) + abs(int(d[2]) - bg[2]);
                w = 1.0f - float(diff) / 48.0f;
                if (w <= 0.0f) continue;
            }
            const float k = 1.0f - p[3] * w;
            for (int c = 0; c < 3; ++c)
            {
                const float v = float(d[c]) * k + p[2 - c] * 255.0f * w;      // overlay is rgb, the image bgr
                d[c] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f);
            }
        }
    }
    // a frame's crop square (side px at x0,y0 of a fw x fh frame) as a size x size BGRA image, the grid and axes on it
    // px: the square's pixels when already shrunk on the GPU. gridCache: the grid drawn once for frames of one view.
    void BoxImage(const unsigned char* square, int side, const BoxView& view, int fw, int fh, int x0, int y0, int size, bool grid, bool gizmo, unsigned char* img, int px = 0, Overlay* gridCache = nullptr)
    {
        Shrink(square, px > 0 ? px : side, img, size);
        if (!grid) return;
        Overlay local(0), top(gizmo ? size : 0);       // local starts empty so the grid is drawn below
        Overlay& back = gridCache ? *gridCache : local;
        if (back.size != size)
        {
            back = Overlay(size);
            Projector P = {};
            P.v = view;
            P.focal = float(fh) * 0.5f / view.tanV;
            P.cx = float(fw) * 0.5f; P.cy = float(fh) * 0.5f;
            P.x0 = float(x0); P.y0 = float(y0);
            P.scale = float(size) / float(side);
            DrawGrid(back, P);
        }
        int bg[3];
        BorderColor(img, size, bg);
        Composite(img, back, true, bg);
        if (!gizmo) return;
        DrawGizmo(top, view);
        Composite(img, top, false);
    }
    // a frame's crop square (side px at x0,y0 of a fw x fh frame) into the box
    unsigned g_emptyFrames = 0;         // box frames with no effect pixel (status)
    int g_seqLeft = 0;                  // "fxpreview dbg seq <n>"
    std::string g_seq;
    std::vector<unsigned char> g_shownImg;  // the last box image as shown ("fxpreview dbg shown <file>")
    int g_shownSize = 0;
    void ShowBox(const unsigned char* square, int side, const BoxView& view, int fw, int fh, int x0, int y0)
    {
        if (!g_box || side <= 0) return;
        const uint64_t t0 = NowUs();
        int lit = 0;
        for (int y = 0; y < side && lit < 3; y += 4)
            for (int x = 0; x < side && lit < 3; x += 4)
            {
                const unsigned char* p = square + (size_t(y) * side + x) * 4;
                if (abs(int(p[0]) - 46) + abs(int(p[1]) - 46) + abs(int(p[2]) - 46) > 40) ++lit;
            }
        if (lit < 3) ++g_emptyFrames;
        else { g_everLit = true; g_lastLitUs = NowUs(); }
        if (g_seqLeft > 0)
        {
            static unsigned lastOwn = 0, lastUser = 0;
            char item[32];
            snprintf(item, sizeof item, " %c%u/%u", lit < 3 ? '.' : '#', g_ownUpdates - lastOwn, g_userFrames - lastUser);
            g_seq += item;
            lastOwn = g_ownUpdates; lastUser = g_userFrames;
            if (--g_seqLeft == 0) { rp::Log("fxpreview: seq (lit, own updates/user frames since the last box frame):%s", g_seq.c_str()); g_seq.clear(); }
        }
        int size = std::min(qt.widgetHeight(g_box), qt.widgetWidth(g_box));
        if (size < 64) size = 64;
        std::vector<unsigned char> img(size_t(size) * size * 4);
        BoxImage(square, side, view, fw, fh, x0, y0, size, g_showGrid, g_showGrid, img.data());
        g_shownImg = img; g_shownSize = size;
        // fromImage shares an RGB32 image's pixels, so the pixmap gets a deep copy, not our buffer
        alignas(16) char qimg[64], owned[64], pix[64];
        qt.imageCtor(qimg, img.data(), size, size, size * 4, 4 /* RGB32 */, nullptr, nullptr);
        qt.imageCopy(qimg, owned, 0, 0, size, size);
        qt.pixmapFromImage(pix, owned, 0);
        qt.labelSetPixmap(g_box, pix);
        qt.pixmapDtor(pix);
        qt.imageDtor(owned);
        qt.imageDtor(qimg);
        ++g_boxShown;
        ++g_fpsFrames;
        g_showUs += NowUs() - t0;
    }
    int BoxSide(int w, int h) { return std::max(16, std::min(std::min(w, h), int(float(h) * kBoxCrop))); }
    // a whole frame read back (MSAA, the async path off): its centre square
    void ShowFrame(const std::vector<unsigned char>& bgra, int w, int h, const BoxView& view)
    {
        if (w <= 0 || h <= 0) return;
        const int side = BoxSide(w, h), x0 = (w - side) / 2, y0 = (h - side) / 2;
        std::vector<unsigned char> sq(size_t(side) * side * 4);
        for (int y = 0; y < side; ++y) memcpy(&sq[size_t(y) * side * 4], &bgra[(size_t(y + y0) * w + x0) * 4], size_t(side) * 4);
        ShowBox(sq.data(), side, view, w, h, x0, y0);
    }

    // ---- box readback: the crop square copied on the GPU, mapped on a later tick without waiting ----
    struct BoxSlot { ID3D11Texture2D* tex; int side; DXGI_FORMAT fmt; bool pending; unsigned seq; BoxView view; int fw, fh, x0, y0; };
    BoxSlot g_slots[3] = {};
    std::vector<unsigned char> g_lastSquare;    // "fxpreview dbg lastbox <png>"
    int g_lastSide = 0;
    unsigned g_slotSeq = 0;
    bool IsBgra(DXGI_FORMAT f) { return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_TYPELESS; }
    bool IsRgba(DXGI_FORMAT f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_TYPELESS; }
    void ReleaseSlots()
    {
        for (BoxSlot& s : g_slots) { if (s.tex) s.tex->Release(); s = BoxSlot(); }
    }
    BoxSlot* OldestPending()
    {
        BoxSlot* best = nullptr;
        for (BoxSlot& s : g_slots) if (s.pending && (!best || s.seq < best->seq)) best = &s;
        return best;
    }
    // maps finished copies oldest first and shows the newest, wait = map the oldest even while the GPU is on it
    bool CollectBox(bool wait)
    {
        ID3D11DeviceContext* ctx = *rt.dxContext;
        if (!ctx) return false;
        std::vector<unsigned char> sq;
        BoxSlot shown = {};
        bool got = false;
        while (BoxSlot* s = OldestPending())
        {
            D3D11_MAPPED_SUBRESOURCE map = {};
            const HRESULT hr = ctx->Map(s->tex, 0, D3D11_MAP_READ, wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) break;
            s->pending = false;
            wait = false;
            if (FAILED(hr)) continue;
            const int side = s->side;
            sq.resize(size_t(side) * side * 4);
            for (int y = 0; y < side; ++y)
            {
                const unsigned char* src = static_cast<const unsigned char*>(map.pData) + size_t(y) * map.RowPitch;
                unsigned char* dst = &sq[size_t(y) * side * 4];
                if (IsBgra(s->fmt)) memcpy(dst, src, size_t(side) * 4);
                else for (int x = 0; x < side; ++x) { dst[x * 4] = src[x * 4 + 2]; dst[x * 4 + 1] = src[x * 4 + 1]; dst[x * 4 + 2] = src[x * 4]; dst[x * 4 + 3] = 255; }
            }
            ctx->Unmap(s->tex, 0);
            shown = *s;
            got = true;
        }
        if (got) { ShowBox(sq.data(), shown.side, shown.view, shown.fw, shown.fh, shown.x0, shown.y0); g_lastSquare.swap(sq); g_lastSide = shown.side; }
        return got;
    }
    // right after a box frame's endFrame: the centre square of the back buffer into a free staging texture
    bool CopyBoxRegion(const BoxView& view)
    {
        if (!g_asyncBox) return false;
        IDXGISwapChain* sc = *rt.swapChain0;
        ID3D11Device* dev = *rt.dxDevice;
        ID3D11DeviceContext* ctx = *rt.dxContext;
        if (!sc || !dev || !ctx) return false;
        ID3D11Texture2D* back = nullptr;
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) return false;
        D3D11_TEXTURE2D_DESC desc = {};
        back->GetDesc(&desc);
        bool ok = false;
        if (desc.SampleDesc.Count == 1 && (IsBgra(desc.Format) || IsRgba(desc.Format)))
        {
            BoxSlot* slot = nullptr;
            for (BoxSlot& s : g_slots) if (!s.pending) { slot = &s; break; }
            if (!slot) { CollectBox(true); for (BoxSlot& s : g_slots) if (!s.pending) { slot = &s; break; } }
            const int w = int(desc.Width), h = int(desc.Height), side = BoxSide(w, h);
            if (slot && (!slot->tex || slot->side != side || slot->fmt != desc.Format))
            {
                if (slot->tex) slot->tex->Release();
                slot->tex = nullptr;
                D3D11_TEXTURE2D_DESC st = {};
                st.Width = UINT(side); st.Height = UINT(side); st.MipLevels = 1; st.ArraySize = 1; st.Format = desc.Format;
                st.SampleDesc.Count = 1; st.Usage = D3D11_USAGE_STAGING; st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(dev->CreateTexture2D(&st, nullptr, &slot->tex))) slot->tex = nullptr;
                slot->side = side; slot->fmt = desc.Format;
            }
            if (slot && slot->tex)
            {
                const int x0 = (w - side) / 2, y0 = (h - side) / 2;
                const D3D11_BOX src = { UINT(x0), UINT(y0), 0, UINT(x0 + side), UINT(y0 + side), 1 };
                ctx->CopySubresourceRegion(slot->tex, 0, 0, 0, 0, back, 0, &src);
                slot->pending = true; slot->seq = ++g_slotSeq; slot->view = view;
                slot->fw = w; slot->fh = h; slot->x0 = x0; slot->y0 = y0;
                ok = true;
            }
        }
        back->Release();
        return ok;
    }

    // ---- filmstrip readback: each frame's centre square into one slice of a staging array, mapped after the last ----
    // The square is shrunk on the GPU to the smallest mip still at least kThumbSize (side px of the frame, px read back)
    struct StripTex { ID3D11Texture2D* tex; int side, px, level; DXGI_FORMAT fmt; int used, fw, fh, x0, y0; };
    StripTex g_strip = {};
    void ReleaseStrip() { if (g_strip.tex) g_strip.tex->Release(); g_strip = StripTex(); ReleaseMip(g_mipStrip); ReleaseLookTex(); }
    bool CopyStripRegion()
    {
        IDXGISwapChain* sc = *rt.swapChain0;
        ID3D11Device* dev = *rt.dxDevice;
        ID3D11DeviceContext* ctx = *rt.dxContext;
        if (!sc || !dev || !ctx || g_strip.used >= kStripMaxFrames) return false;
        ID3D11Texture2D* back = nullptr;
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) return false;
        D3D11_TEXTURE2D_DESC desc = {};
        back->GetDesc(&desc);
        bool ok = false;
        const int w = int(desc.Width), h = int(desc.Height);
        int level = 0;
        while (g_gpuShrink && level < 4 && (BoxSide(w, h) >> (level + 1)) >= kThumbSize) ++level;
        const int side = (BoxSide(w, h) >> level) << level;     // whole mip texels
        const int x0 = (w - side) / 2, y0 = (h - side) / 2;
        const D3D11_BOX src = { UINT(x0), UINT(y0), 0, UINT(x0 + side), UINT(y0 + side), 1 };
        if (desc.SampleDesc.Count == 1 && (IsBgra(desc.Format) || IsRgba(desc.Format)))
        {
            if (level && !MipDown(g_mipStrip, back, desc.Format, src, level + 1)) level = 0;
            const int px = side >> level;
            const DXGI_FORMAT fmt = level ? g_mipStrip.fmt : desc.Format;
            const bool same = g_strip.tex && g_strip.side == side && g_strip.px == px && g_strip.fmt == fmt;
            if (!same && g_strip.used == 0)
            {
                if (g_strip.tex) g_strip.tex->Release();
                g_strip = StripTex();
                D3D11_TEXTURE2D_DESC st = {};
                st.Width = UINT(px); st.Height = UINT(px); st.MipLevels = 1; st.ArraySize = UINT(kStripMaxFrames); st.Format = fmt;
                st.SampleDesc.Count = 1; st.Usage = D3D11_USAGE_STAGING; st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(dev->CreateTexture2D(&st, nullptr, &g_strip.tex))) g_strip.tex = nullptr;
                g_strip.side = side; g_strip.px = px; g_strip.fmt = fmt;
            }
            if (g_strip.tex && (same || g_strip.used == 0))
            {
                const UINT dst = D3D11CalcSubresource(0, UINT(g_strip.used), 1);
                if (level) ctx->CopySubresourceRegion(g_strip.tex, dst, 0, 0, 0, g_mipStrip.tex, D3D11CalcSubresource(UINT(level), 0, UINT(level + 1)), nullptr);
                else ctx->CopySubresourceRegion(g_strip.tex, dst, 0, 0, 0, back, 0, &src);
                g_strip.fw = w; g_strip.fh = h; g_strip.x0 = x0; g_strip.y0 = y0;
                ++g_strip.used;
                ok = true;
            }
        }
        back->Release();
        return ok;
    }

    // ---- the preview box ----
    // Starts `name` far above the map, delayMs after now (negative = that far into its life), framed for the box.
    bool Looks(int def, std::vector<unsigned char>& best, int& bestW, int& bestH, Fit& fit, std::string& why);
    bool Start(const std::string& name, int delayMs, std::string& why)
    {
        if (!g_ready) { why = "FX preview is unavailable"; return false; }
        if (!g_clockLive) { why = "Radiant's fx clock is not running yet"; return false; }
        if (name.empty()) { why = "no effect name"; return false; }
        g_halted = false;
        g_everLit = false;
        PlaceStage();
        if (!FxSystem()) { why = "no fx system"; return false; }
        if (g_bad.count(name)) { why = "effect '" + name + "' raised a Radiant error before, it is not played"; return false; }
        const int def = LoadDef(name);
        if (!def) { why = "effect '" + name + "' not found or unreadable (fx/" + name + ".efx)"; return false; }
        Kill();
        BoxTimeline();
        QuietScope quiet;
        unsigned h = 0;
        bool hit = false;
        {
            OwnClock own(&g_boxClock);
            SetStageDef(def);
            if (!FitOf(name)) { std::vector<unsigned char> frame; int w = 0, hh = 0; Fit fit; std::string ignored; Looks(def, frame, w, hh, fit, ignored); g_fits[name] = fit; }
            hit = quiet.Hit();
            if (!hit)
            {
                if (name != g_name) g_pan[0] = g_pan[1] = g_pan[2] = 0;     // the orbit's angles and zoom carry over
                const BoxView bv = MakeBoxView(FitOf(name));
                memcpy(g_boxCam, bv.eye, sizeof g_boxCam);
                static const float axis[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
                StageView view(bv.eye, bv.ang);
                StepFx(0.0);                        // hands the fx system the stage camera: a spawn is culled against its last one
                h = rt.spawn(def, kStage, axis, delayMs, 1.0f);
            }
        }
        RestoreUserSteps();
        if (hit) { g_bad.insert(name); why = "effect '" + name + "' raised a Radiant error (see the log), it is not played"; return false; }
        if (h && quiet.Hit()) { Dispose(h); g_bad.insert(name); why = "effect '" + name + "' raised a Radiant error (see the log), it is not played"; return false; }
        if (!h) { why = "the fx system refused the effect (too many effects, or fx rendering is off)"; return false; }
        g_effect = h;
        g_def = def;
        g_name = name;
        g_startMs = BoxMs() + delayMs;
        g_lengthMs = EffectLengthMs(def);
        g_scrub = g_lengthMs > 0 && g_lengthMs < kSlowBelowMs;
        g_scrubStartUs = NowUs();
        g_boxDirty = true;
        g_nextFrameUs = 0;
        g_lastStepMs = GetTickCount64();
        if (g_slider) rp::host->ui->SliderSetRange(g_slider, 0, SliderTopMs() / 10);
        return true;
    }

    void Stop()
    {
        Kill();
        g_halted = true;
        ClearGridSelection();           // nothing picked: the tick hides the box
    }

    double ElapsedSeconds() { return g_effect ? double(BoxMs() - g_startMs) / 1000.0 : 0.0; }
    // what the slider and the play button show: a thumbnail being filmed takes the box effect off the stage (it resumes at
    // the same age) and pauses the fx clock between its slices, neither is the user's
    double ShownElapsed();
    bool ShownPaused();

    void AbortJob(bool requeue);
    void TogglePause()
    {
        g_boxPaused = !g_boxPaused;     // the box's own clock, Radiant's FX playback pause is the map's
    }

    // Treyarch's stub effects: every visual is the debug material, nothing draws
    bool IsPlaceholder(const std::string& name)
    {
        static std::map<std::string, bool> known;
        const auto it = known.find(name);
        if (it != known.end()) return it->second;
        std::wstring path = g_fxRoot + L"\\" + Wide(name) + L".efx";
        for (wchar_t& c : path) if (c == L'/') c = L'\\';
        bool any = false, other = false;
        if (FILE* f = _wfopen(path.c_str(), L"rb"))
        {
            char line[512];
            while (!other && fgets(line, sizeof line, f))
            {
                if (line[0] != '\t' || line[1] != '\t' || line[2] != '"') continue;
                const char* v = line + 3;
                if (!strncmp(v, "gfx_debug_deleted_fx\"", 21) || !strncmp(v, "gfx_debug_missing_fx\"", 21)) any = true;
                else other = true;
            }
            fclose(f);
        }
        return known[name] = any && !other;
    }
    bool Preview(const std::string& raw)
    {
        const std::string name = CleanName(raw);
        std::string why;
        if (name.empty()) { SetStatus("No effect name."); return false; }
        if (!Start(name, 0, why)) { SetStatus(why); return false; }
        g_boxPaused = false;            // a picked effect plays
        for (auto it = g_modelSkipped.begin(); it != g_modelSkipped.end(); ++it)    // loaded now: its thumbnail is cheap
            if (FullName(g_all[size_t(*it)]) == name)
            {
                g_failed.erase(std::remove(g_failed.begin(), g_failed.end(), *it), g_failed.end());
                for (size_t r = 0; r < g_rows.size() && r < g_rowState.size(); ++r) if (g_rows[r] == *it) g_rowState[r] = 0;
                g_modelSkipped.erase(it);
                break;
            }
        char buf[400];
        if (IsPlaceholder(name)) { SetStatus(name + ": a placeholder effect (gfx_debug_deleted_fx or gfx_debug_missing_fx only), it draws nothing"); return true; }
        if (g_lengthMs > 0) snprintf(buf, sizeof buf, "%s (%.2f s%s)%s", name.c_str(), g_lengthMs / 1000.0, g_scrub ? ", shown at quarter speed" : "", FxRenderingOn() ? "" : ", Fx > Fx Rendering Enabled is off");
        else snprintf(buf, sizeof buf, "%s (looping)%s", name.c_str(), FxRenderingOn() ? "" : ", Fx > Fx Rendering Enabled is off");
        SetStatus(buf);
        return true;
    }

    // one simulation step and one box frame now, then the end-of-life handling (loop)
    void BoxTick(uint64_t now)
    {
        if (OldestPending()) CollectBox(false);         // last tick's frame, when the GPU is done with it
        if (!g_effect && !g_boxDirty)
        {
            // Loop: the fx system can drop an effect before its computed life ends, it starts over all the same
            if (g_loop && !g_halted && !g_name.empty() && !g_boxPaused && now >= g_loopRetryMs)
            {
                std::string why;
                if (!Start(g_name, 0, why)) g_loopRetryMs = now + 2000;
            }
            return;
        }
        const uint64_t us = NowUs(), interval = 1000000 / (g_boxFps ? g_boxFps : 30);
        if (us < g_nextFrameUs) return;
        g_nextFrameUs = us - g_nextFrameUs < interval ? g_nextFrameUs + interval : us + interval;   // steady cadence, no catch-up bursts
        if (us - g_fpsSinceUs >= 1000000) { g_fpsNow = g_fpsSinceUs ? float(g_fpsFrames) * 1e6f / float(us - g_fpsSinceUs) : 0.0f; g_fpsFrames = 0; g_fpsSinceUs = us; }
        g_lastStepMs = now;
        if (g_effect && !rt.effectPtr(0, g_effect)) g_effect = 0;      // the fx system dropped it (map load, limit)
        const bool alive = g_effect != 0 || (g_scrub && g_def);
        static uint64_t lastFrameUs = 0;
        if (g_boxPaused && lastFrameUs) g_scrubStartUs += us - lastFrameUs;     // a paused scrub keeps its age
        lastFrameUs = us;
        if (alive || g_boxDirty)
        {
            OwnClock own(&g_boxClock, g_boxPaused);
            BoxView bv = MakeBoxView(FitOf(g_name));
            memcpy(g_boxCam, bv.eye, sizeof g_boxCam);
            StageView view(bv.eye, bv.ang);
            ReadTanV();                                     // for the next frame's pivot and this one's grid
            bv.tanV = g_tanV;
            if (alive && g_scrub && !g_boxPaused)           // a flash outlives no box step: a fresh look at age t / 4 instead
            {
                static const float axis[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
                const int age = int((NowUs() - g_scrubStartUs) / 4000) % kScrubSpanMs;
                Dispose(g_effect);
                StepFx(0.0);                                // a spawn is culled against the last step's camera
                g_effect = rt.spawn(g_def, kStage, axis, -age, 1.0f);
                StepFx(0.001);
            }
            else if (alive) StepBox();                      // one more step this tick, under the stage camera
            std::vector<unsigned char> pixels;
            int w = 0, h = 0;
            const uint64_t t0 = NowUs();
            const int got = HiddenFrame(pixels, w, h, &bv);
            if (got == 1) ShowFrame(pixels, w, h, bv);
            if (got) { ++g_boxFrames; g_boxUs += NowUs() - t0; }
            else ++g_boxMisses;
            g_boxDirty = false;
        }
        if (alive || g_boxDirty) RestoreUserSteps();
        if (!alive || g_boxPaused) return;
        const int elapsed = BoxMs() - g_startMs;
        // Loop: an effect whose life runs on after it is gone (a blast with 20 s of unseen smoke) starts over once the box is empty
        if (g_loop && g_everLit && elapsed >= kMinCycleMs && NowUs() - g_lastLitUs > 1500000)
        {
            std::string why;
            Start(g_name, 0, why);
            return;
        }
        if (g_lengthMs <= 0 || elapsed < std::max(g_lengthMs, kMinCycleMs)) return;   // endless effects play until Stop or the next pick
        std::string why;
        if (g_loop) Start(g_name, 0, why);
        else Kill();                                            // the last frame stays in the box
    }

    // Looks at `def` from kMeasureDist (0.35, 1 and 1.8 s into its life): the first at least half as busy as the busiest
    // (a burst's flash beats its smoke), or the busiest of all six when early looks were added. Then measured.
    bool Looks(int def, std::vector<unsigned char>& best, int& bestW, int& bestH, Fit& fit, std::string& why)
    {
        float camAt[3];
        FrameCamera(kMeasureDist, camAt);
        static const float axis[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        SetStageDef(def);
        StageView view(camAt);
        StepFx(0.0);                        // the fx system's camera becomes the stage before the spawns
        // three looks over its first seconds, three early ones when those show next to nothing (muzzle flashes, sparks)
        const int at[6] = { int(kThumbSeconds * 350), int(kThumbSeconds * 1000), int(kThumbSeconds * 1800), 15, 60, 150 };
        std::vector<unsigned char> frames[6];
        size_t scores[6] = { 0, 0, 0, 0, 0, 0 };
        bool halves[6] = {};
        struct HalfScope { HalfScope() { g_halfLooks = true; } ~HalfScope() { g_halfLooks = false; } } halfScope;
        int w = 0, h = 0;
        size_t top = 0;
        fit = Fit();
        int looks = 3;
        for (int i = 0; i < looks; ++i)
        {
            unsigned e = rt.spawn(def, kStage, axis, -at[i], 1.0f);     // that far into its life: the step emits the backlog
            if (!e) { why = "the fx system refused the effect"; return false; }
            StepFx(0.001);
            const bool got = HiddenFrame(frames[i], w, h);
            halves[i] = g_readHalf;
            Dispose(e);
            if (!got) { why = "hidden frame failed"; return false; }
            const std::vector<unsigned char>& px = frames[i];
            for (size_t p = 0; p + 3 < px.size(); p += halves[i] ? 4 : 16)    // pixels that are not the plain background
                if (abs(int(px[p]) - 46) + abs(int(px[p + 1]) - 46) + abs(int(px[p + 2]) - 46) > 40) ++scores[i];
            if (scores[i] > top) top = scores[i];
            if (i == 2 && top < 8) looks = 6;
        }
        int pick = 0;
        if (looks == 3) while (pick < 2 && scores[pick] * 2 < top) ++pick;
        else for (int i = 1; i < looks; ++i) if (scores[i] > scores[pick]) pick = i;
        best.swap(frames[pick]);
        bestW = w; bestH = h;
        MeasureFrame(best, bestW, bestH, fit, halves[pick] ? 1 : 2);
        if (fit.ok && fit.size < 0.12f)     // a small effect: the same look again from closer, the fit kept in kMeasureDist terms
        {
            const float dist = kMeasureDist * fit.size / 0.3f;
            FrameCamera(dist < 24.0f ? 24.0f : dist, camAt);
            memcpy(view.cam + off_Cam_Origin, camAt, sizeof camAt);
            StepFx(0.0);
            unsigned e = rt.spawn(def, kStage, axis, -at[pick], 1.0f);
            std::vector<unsigned char> close;
            Fit closeFit;
            if (e)
            {
                StepFx(0.001);
                const bool got = HiddenFrame(close, w, h);
                const bool half = g_readHalf;
                Dispose(e);
                if (got && MeasureFrame(close, w, h, closeFit, half ? 1 : 2))
                {
                    const float k = (dist < 24.0f ? 24.0f : dist) / kMeasureDist;
                    fit.cx = closeFit.cx * k; fit.cy = closeFit.cy * k; fit.size = closeFit.size * k;
                    best.swap(close); bestW = w; bestH = h;
                    fit.ok = true;
                    g_fitScale = k;         // SavePng crops this frame with the fit scaled back to it
                    fit.scale = k;          // the box casts the fit back through this measuring camera
                    return true;
                }
            }
        }
        g_fitScale = 1.0f;
        fit.scale = 1.0f;
        return true;
    }

    // ---- the filmstrip job: an effect filmed from the box's home view, grid and axes on, spread over ticks ----
    // One spawn stepped frame to frame (a respawn per frame would reshuffle the particles), kStripFps frames of its life
    // or a sped-up first kStripSpanMaxMs, a poster (the busiest frame) on top. The fx clock is held between ticks.
    struct StripJob
    {
        bool active = false, hit = false;
        double clock = 0;               // the job's own fx clock (seconds), see OtherClock
        int striped = 0;                // frames of horizontal streaks (see Striped)
        bool whole = false;             // refilmed in one go after streaks
        int idx = -1;                   // index into g_all, -1 for a command's strip
        std::string name, why;
        std::wstring path;
        int def = 0, phase = 0, n = 0, k = 0, start = 0;
        double dt = 0;
        unsigned effect = 0;
        BoxView bv = {};
        std::vector<int> slice;
        std::vector<std::vector<unsigned char>> squares;
        int side = 0, px = 0, fw = 0, fh = 0, x0 = 0, y0 = 0;    // px: the read back square's size
        std::vector<unsigned char> strip;
        Overlay grid{ 0 };              // the grid, drawn once
        size_t bestLit = 0;
        int best = 0, lastLit = -1;
        uint64_t us = 0, beganMs = 0;
        int slices = 0;
    };
    StripJob g_job;
    std::vector<unsigned char> g_doneStrip;     // the strip the job just wrote (BGRA, kThumbSize wide), cut without a decode
    // Stage effects nobody released (an earlier load of this plugin) are released once
    void ReclaimStage()
    {
        const char* sys = *rt.sysCount > 0 ? *rt.sysPtr : nullptr;
        const char* ring = sys ? *reinterpret_cast<char* const*>(sys + off_Sys_Ring) : nullptr;
        const char* base = sys ? *reinterpret_cast<char* const*>(sys + off_Sys_Effects) : nullptr;
        if (!ring || !base) return;
        PlaceStage();
        std::vector<unsigned> lost;
        const int from = *reinterpret_cast<const int*>(ring + 24), to = *reinterpret_cast<const int*>(ring + 32);
        for (int n = from; n != to && n - from < 4096; ++n)
        {
            const unsigned short id = *reinterpret_cast<const unsigned short*>(ring + 36 + 2 * (n & 0xFFF));
            const char* e = base + 32 * size_t(id);
            const unsigned long long status = *reinterpret_cast<const unsigned long long*>(e + off_Fx_Status);
            const float* o = reinterpret_cast<const float*>(e + off_Fx_Origin);
            const unsigned h = *reinterpret_cast<const unsigned*>(e + off_Fx_Handle);
            if (!(status & 0x3FFF) || (status & 0x8000) || h == g_effect || h == g_job.effect) continue;
            if (fabsf(o[0] - kStage[0]) < 1.0f && fabsf(o[1] - kStage[1]) < 1.0f && fabsf(o[2] - kStage[2]) < 1.0f) lost.push_back(h);
        }
        for (unsigned h : lost) Dispose(h);
        if (!lost.empty()) rp::Log("fxpreview: released %zu stage effects left by an earlier load, %d effects live", lost.size(), LiveEffects());
    }
    unsigned g_stripUs = 0, g_strips = 0;   // the last strip's time on the main thread, strips made (status)
    unsigned g_sliceMaxUs = 0;              // the longest job slice since "dbg resetstats"
    uint64_t g_sumWorkUs = 0, g_sumWallMs = 0, g_sumSlices = 0, g_sumStrips = 0;     // strips since "dbg resetstats"

    unsigned g_refilms = 0;                 // strips filmed again in one go after streaks
    bool g_fakeStripes = false;             // "fxpreview dbg stripes": the next strip counts as streaked (tests the refilm)
    // Horizontal streaks over the whole frame: rows differ far more than columns. Clean frames measure 1 to 7 both ways,
    // streaked ones 14 to 38 against 1 to 3.
    bool Striped(const unsigned char* img, int size)
    {
        long rows = 0, cols = 0;
        for (int y = 0; y + 1 < size; ++y)
            for (int x = 0; x + 1 < size; ++x)
            {
                const unsigned char* p = img + (size_t(y) * size + x) * 4;
                const int v = p[0] + p[1] + p[2], right = p[4] + p[5] + p[6], down = p[size * 4] + p[size * 4 + 1] + p[size * 4 + 2];
                rows += abs(v - down); cols += abs(v - right);
            }
        const double n = double(size - 1) * (size - 1) * 3.0, r = rows / n, c = cols / n;
        return r > 8.0 && r > c * 3.0;
    }
    uint64_t g_jobSliceUs = 12000;          // work per tick while thumbnails are made ("fxpreview dbg slice <ms>")

    // frames stacked top down, JPEG for a .jpg path (PNG when the JPEG writer is missing)
    bool SaveStrip(const std::vector<unsigned char>& strip, int size, int frames, const std::wstring& path)
    {
        alignas(16) char img[64], qs[16];
        qt.imageCtor(img, strip.data(), size, size * frames, size * 4, 4 /* RGB32 */, nullptr, nullptr);
        qt.stringCtor(qs, Utf8(path.c_str(), int(path.size())).c_str());
        const bool jpg = path.size() > 4 && _wcsicmp(path.c_str() + path.size() - 4, L".jpg") == 0;
        const bool ok = (jpg && qt.imageSave(img, qs, "JPG", 90)) || qt.imageSave(img, qs, "PNG", -1);
        qt.stringDtor(qs);
        qt.imageDtor(img);
        return ok;
    }
    bool WriteStrip(const std::vector<unsigned char>& strip, int frames, const std::wstring& path)
    {
        const size_t cut = path.find_last_of(L'\\');
        if (cut != std::wstring::npos) MakeDirs(path.substr(0, cut));
        if (!SaveStrip(strip, kThumbSize, frames, path)) return false;
        if (path.size() > 4 && _wcsicmp(path.c_str() + path.size() - 4, L".jpg") == 0)
            DeleteFileW((path.substr(0, path.size() - 4) + L".png").c_str());      // the old still thumbnail
        return true;
    }

    // ---- the grid's strips are written by one worker thread (QImage is reentrant), Shutdown waits for it ----
    struct SaveItem { std::vector<unsigned char> px; int frames = 0; std::wstring path; };
    std::mutex g_saveMx;
    std::condition_variable g_saveCv;
    std::vector<SaveItem> g_saves;
    std::thread g_saver;
    bool g_saverStop = false;
    // Radiant exits through ExitProcess and the host skips plugin shutdown then, so the saver would still be joinable at static
    // destruction (std::terminate). Declared after g_saver so it is destroyed first, detaching is enough as the thread is already gone.
    struct SaverExitGuard
    {
        ~SaverExitGuard() { if (g_saver.joinable()) g_saver.detach(); }
    } g_saverExitGuard;
    void SaverLoop()
    {
        for (;;)
        {
            SaveItem it;
            {
                std::unique_lock<std::mutex> lock(g_saveMx);
                g_saveCv.wait(lock, [] { return g_saverStop || !g_saves.empty(); });
                if (g_saves.empty()) return;
                it = std::move(g_saves.front());
                g_saves.erase(g_saves.begin());
            }
            try
            {
                if (!WriteStrip(it.px, it.frames, it.path)) rp::Log("fxpreview: could not write %s", Utf8(it.path.c_str(), int(it.path.size())).c_str());
            }
            catch (...) {}
        }
    }
    void SaveLater(std::vector<unsigned char> px, int frames, const std::wstring& path)
    {
        {
            std::lock_guard<std::mutex> lock(g_saveMx);
            g_saves.push_back({ std::move(px), frames, path });
            g_saverStop = false;
        }
        if (!g_saver.joinable()) g_saver = std::thread(SaverLoop);
        g_saveCv.notify_one();
    }
    void StopSaver()
    {
        { std::lock_guard<std::mutex> lock(g_saveMx); g_saverStop = true; }
        g_saveCv.notify_one();
        if (g_saver.joinable()) g_saver.join();
    }

    // ends the job: the clock and pause state back, the box effect restarted. Leaves g_job.why and the result.
    void StageJobDef(int def);
    bool EndJob(bool ok)
    {
        Dispose(g_job.effect);
        g_job.active = false;
        g_filmBase = LONG_MAX;
        StageJobDef(0);
        g_job.strip.clear(); g_job.strip.shrink_to_fit();
        g_job.squares.clear();
        g_stripUs = unsigned(g_job.us);
        ++g_strips;
        g_sumWorkUs += g_job.us; g_sumWallMs += GetTickCount64() - g_job.beganMs; g_sumSlices += unsigned(g_job.slices); ++g_sumStrips;
        return ok;
    }
    void AbortJob(bool requeue)
    {
        if (!g_job.active) return;
        if (requeue && g_job.idx >= 0) g_queue.insert(g_queue.begin(), g_job.idx);
        EndJob(false);
    }
    double ShownElapsed() { return ElapsedSeconds(); }
    bool ShownPaused() { return g_boxPaused; }
    bool BeginJob(int idx, const std::string& name, const std::wstring& path, std::string& why)
    {
        if (!g_ready) { why = "unavailable"; return false; }
        if (!g_clockLive) { why = "Radiant's fx clock is not running yet"; return false; }
        if (g_bad.count(name)) { why = "it raised a Radiant error before"; return false; }
        PlaceStage();
        const int def = LoadDef(name);
        if (!def) { why = "effect '" + name + "' not found or unreadable"; return false; }
        char* m = PlaybackMgr();
        if (!m) { why = "no playback manager"; return false; }
        AbortJob(true);
        g_job = StripJob();
        g_job.active = true;
        g_job.idx = idx; g_job.name = name; g_job.path = path; g_job.def = def;
        g_job.beganMs = GetTickCount64();
        g_jobDef = def;
        g_filmBase = std::max(FxTimeMs(), g_boxBase == LONG_MAX ? 0 : BoxMs()) + kFilmAhead;
        g_job.clock = double(g_filmBase) / 1000.0 + 60.0;   // look spawns start up to a few seconds into a life
        return true;
    }
    // One slice of the job, about `budgetUs`. 0 = more to do, 1 = written, -1 = failed (g_job.why).
    int StepJob(uint64_t budgetUs)
    {
        if (!g_job.active) return -1;
        if (g_job.whole) budgetUs = ~0ull;
        char* m = PlaybackMgr();
        if (!m) { g_job.why = "no playback manager"; return EndJob(false) ? 1 : -1; }
        if (g_job.hit) { g_job.why = "Radiant reported an error while drawing it (see the log)"; return EndJob(false) ? 1 : -1; }
        const uint64_t t0 = NowUs();
        const auto spent = [&]() { return NowUs() - t0 >= budgetUs; };
        static const float axis[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        StripJob& j = g_job;
        const int phaseAtStart = j.phase;
        OwnClock clock(&j.clock);
        bool failed = false;
        {
            QuietScope quiet;
            if (j.phase == 0)               // the fit (measuring looks), the view, the spawn
            {
                SetStageDef(j.def);
                if (!FitOf(j.name))
                {
                    std::vector<unsigned char> best;
                    int w = 0, h = 0;
                    Fit fit;
                    if (Looks(j.def, best, w, h, fit, j.why)) g_fits[j.name] = fit;
                    else failed = true;
                }
                if (!failed)
                {
                    const int life = EffectLengthMs(j.def);
                    j.start = life > 0 ? 0 : kStripWarmMs;
                    const int span = life > 0 ? std::min(life, kStripSpanMaxMs) : kStripEndlessMs;
                    j.n = std::max(kStripMinFrames, std::min(kStripMaxFrames, (span * kStripFps + 500) / 1000));
                    j.dt = double(span) / 1000.0 / j.n;
                    j.slice.assign(size_t(j.n), -1);
                    j.squares.assign(size_t(j.n), std::vector<unsigned char>());
                    j.bv = MakeBoxView(FitOf(j.name), true);
                    StageView view(j.bv.eye, j.bv.ang);
                    ReadTanV();
                    j.bv = MakeBoxView(FitOf(j.name), true);
                    j.bv.tanV = g_tanV;
                    if (view.cam) memcpy(view.cam + off_Cam_Origin, j.bv.eye, sizeof j.bv.eye);
                    memcpy(g_boxCam, j.bv.eye, sizeof g_boxCam);    // the update hook judges the stage effect from here
                    StepFx(0.0);                                    // a spawn is culled against the last step's camera
                    j.effect = rt.spawn(j.def, kStage, axis, -j.start, 1.0f);
                    if (!j.effect) { j.why = "the fx system refused the effect"; failed = true; }
                    g_strip.used = 0;
                    j.phase = 1;
                }
            }
            else if (j.phase == 1)          // frames into the staging slices (or read back whole)
            {
                StageView view(j.bv.eye, j.bv.ang);
                memcpy(g_boxCam, j.bv.eye, sizeof g_boxCam);
                while (!failed && j.k < j.n)
                {
                    StepFx(j.k ? j.dt : 0.001);
                    std::vector<unsigned char> px;
                    int w = 0, h = 0;
                    g_stripOn = true;
                    const int got = HiddenFrame(px, w, h, &j.bv);
                    g_stripOn = false;
                    if (got == 2) j.slice[size_t(j.k)] = g_strip.used - 1;
                    else if (got == 1)              // no GPU copy (MSAA): the centre square of the read back frame
                    {
                        j.side = j.px = BoxSide(w, h); j.fw = w; j.fh = h; j.x0 = (w - j.side) / 2; j.y0 = (h - j.side) / 2;
                        std::vector<unsigned char>& sq = j.squares[size_t(j.k)];
                        sq.resize(size_t(j.side) * j.side * 4);
                        for (int y = 0; y < j.side; ++y) memcpy(&sq[size_t(y) * j.side * 4], &px[(size_t(y + j.y0) * w + j.x0) * 4], size_t(j.side) * 4);
                    }
                    else { j.why = "hidden frame failed"; failed = true; }
                    ++j.k;
                    if (spent()) break;
                }
                if (!failed && j.k >= j.n)          // filmed: composed from the next tick on, when the GPU is done
                {
                    Dispose(j.effect);
                    if (g_strip.used) { j.side = g_strip.side; j.px = g_strip.px; j.fw = g_strip.fw; j.fh = g_strip.fh; j.x0 = g_strip.x0; j.y0 = g_strip.y0; }
                    j.strip.assign(size_t(kThumbSize) * kThumbSize * 4 * size_t(j.n + 1), 0);
                    j.k = 0;
                    j.phase = 2;
                }
            }
            if (quiet.Hit()) { g_bad.insert(j.name); j.why = "Radiant reported an error while drawing it (see the log)"; failed = true; }
        }
        if (!failed && j.phase == 2 && (phaseAtStart == 2 || budgetUs == ~0ull))   // read back from the next slice on, when the GPU is done
        {
            ID3D11DeviceContext* ctx = *rt.dxContext;
            const size_t frameBytes = size_t(kThumbSize) * kThumbSize * 4;
            std::vector<unsigned char> sq;
            while (j.k < j.n)
            {
                const int k = j.k;
                const unsigned char* src = j.squares[size_t(k)].empty() ? nullptr : j.squares[size_t(k)].data();
                if (j.slice[size_t(k)] >= 0 && ctx && g_strip.tex)
                {
                    const UINT sub = D3D11CalcSubresource(0, UINT(j.slice[size_t(k)]), 1);
                    D3D11_MAPPED_SUBRESOURCE map = {};
                    if (SUCCEEDED(ctx->Map(g_strip.tex, sub, D3D11_MAP_READ, 0, &map)))
                    {
                        sq.resize(size_t(j.px) * j.px * 4);
                        for (int y = 0; y < j.px; ++y)
                        {
                            const unsigned char* row = static_cast<const unsigned char*>(map.pData) + size_t(y) * map.RowPitch;
                            unsigned char* dst = &sq[size_t(y) * j.px * 4];
                            if (IsBgra(g_strip.fmt)) memcpy(dst, row, size_t(j.px) * 4);
                            else for (int x = 0; x < j.px; ++x) { dst[x * 4] = row[x * 4 + 2]; dst[x * 4 + 1] = row[x * 4 + 1]; dst[x * 4 + 2] = row[x * 4]; dst[x * 4 + 3] = 255; }
                        }
                        ctx->Unmap(g_strip.tex, sub);
                        src = sq.data();
                    }
                }
                if (!src || j.side <= 0 || j.px <= 0) { j.why = "a strip frame could not be read back"; failed = true; break; }
                size_t lit = 0;
                for (size_t p = 0; p + 3 < size_t(j.px) * j.px * 4; p += j.px < j.side ? 4 : 16)
                    if (abs(int(src[p]) - 46) + abs(int(src[p + 1]) - 46) + abs(int(src[p + 2]) - 46) > 40) ++lit;
                if (lit > j.bestLit) { j.bestLit = lit; j.best = k; }
                if (lit > 2) j.lastLit = k;
                BoxImage(src, j.side, j.bv, j.fw, j.fh, j.x0, j.y0, kThumbSize, g_showGrid, false, &j.strip[frameBytes * size_t(k + 1)], j.px, &j.grid);
                if (Striped(&j.strip[frameBytes * size_t(k + 1)], kThumbSize) || (g_fakeStripes && !j.whole)) ++j.striped;
                j.squares[size_t(k)].clear();
                ++j.k;
                if (spent()) break;
            }
            if (!failed && j.k >= j.n && j.striped && !j.whole)     // streaks: once more without ticks between the frames
            {
                rp::Log("fxpreview: %d of %d frames of %s streaked, filmed again in one go", j.striped, j.n, j.name.c_str());
                ++g_refilms;
                g_fakeStripes = false;
                j.whole = true;
                j.phase = 0; j.k = 0; j.striped = 0; j.bestLit = 0; j.best = 0; j.lastLit = -1; j.grid = Overlay(0);
                return 0;
            }
            if (!failed && j.k >= j.n)
            {
                if (j.striped) rp::Log("fxpreview: %d of %d frames of %s streaked again, kept", j.striped, j.n, j.name.c_str());
                memcpy(j.strip.data(), &j.strip[frameBytes * size_t(j.best + 1)], frameBytes);
                if (j.lastLit >= 0 && j.lastLit + 4 < j.n)     // an empty tail (a burst inside a long life) shortens to a 3-frame pause
                {
                    j.n = std::max(j.lastLit + 4, 6);
                    j.strip.resize(frameBytes * size_t(j.n + 1));
                }
                j.phase = 3;
            }
        }
        bool done = false;
        if (!failed && j.phase == 3 && (!spent() || j.idx >= 0))
        {
            if (j.idx >= 0) { g_doneStrip = j.strip; SaveLater(std::move(j.strip), j.n + 1, j.path); }     // the grid's: written behind
            else if (!WriteStrip(j.strip, j.n + 1, j.path)) { j.why = "could not write " + Utf8(j.path.c_str(), int(j.path.size())); failed = true; }
            done = true;
        }
        const uint64_t took = NowUs() - t0;
        j.us += took;
        ++j.slices;
        if (took > g_sliceMaxUs) g_sliceMaxUs = unsigned(took);
        if (failed) return EndJob(false) ? 1 : -1;
        if (done) return EndJob(true) ? 1 : -1;
        return 0;                               // Radiant's ticks leave the filmed effect where it is (OtherClock)
    }
    // Films `name` and writes its filmstrip now (the command's path; the grid uses the job a slice per tick).
    bool Thumbnail(const std::string& name, const std::wstring& path, std::string& why)
    {
        if (!BeginJob(-1, name, path, why)) return false;
        int r = 0;
        while ((r = StepJob(~0ull)) == 0) {}
        if (r < 0) why = g_job.why;
        return r > 0;
    }
    unsigned JobEffect() { return g_job.active ? g_job.effect : 0; }
    bool JobActive() { return g_job.active; }
    void JobHit()
    {
        Dispose(g_job.effect);
        g_job.hit = true;
        g_bad.insert(g_job.name);
        rp::Log("fxpreview: '%s' raised a Radiant error while it was filmed, not played again", g_job.name.c_str());
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
        SavePng(px, n, n, Fit(), n, p);
        return p;
    }

    // ---- animated thumbnails in the grid: each strip cut into icons of the shown size, the rows on screen cycle them ----
    struct IconBuf { alignas(16) char b[16]; };     // a constructed QIcon, moved by plain copies
    // icons: [0] the poster, then the loop. poster: frame 0 at file size, rescaled while the size slider moves
    struct Anim { std::vector<IconBuf> icons; uint64_t seenMs = 0; alignas(16) char poster[64]; bool hasPoster = false; };
    std::map<int, Anim> g_anims;        // by effect (index into g_all)
    std::vector<int> g_onScreen;        // grid rows on screen at the last RefreshVisible
    bool g_animate = true;              // [fxpreview] animate
    uint64_t g_lastAnimTick = 0;
    const size_t kAnimCache = 300;      // effects kept cut into icons, the least recently seen go first
    void FreeIcons(Anim& a) { for (IconBuf& i : a.icons) qt.iconDtor(i.b); a.icons.clear(); }
    void FreeAnim(Anim& a) { FreeIcons(a); if (a.hasPoster) qt.imageDtor(a.poster); a.hasPoster = false; }
    void FreeAnims() { for (auto& kv : g_anims) FreeAnim(kv.second); g_anims.clear(); }
    // a strip file (a plain square image is one frame) cut into icons of the shown size
    uint64_t g_loadUs = 0; unsigned g_loads = 0;     // icons cut since "dbg resetstats" (status)
    uint64_t g_liveUs = 0; unsigned g_lives = 0;     // live icon size steps (status)
    bool LoadIcons(const std::wstring& path, Anim& a, const std::vector<unsigned char>* strip = nullptr)
    {
        const uint64_t t0 = NowUs();
        alignas(16) char qs[16], img[64];
        const size_t frameBytes = size_t(kThumbSize) * kThumbSize * 4;
        if (strip && strip->size() >= frameBytes)
            qt.imageCtor(img, strip->data(), kThumbSize, int(strip->size() / frameBytes) * kThumbSize, kThumbSize * 4, 4 /* RGB32 */, nullptr, nullptr);
        else
        {
            qt.stringCtor(qs, Utf8(path.c_str(), int(path.size())).c_str());
            qt.imageFromFile(img, qs, nullptr);
            qt.stringDtor(qs);
        }
        const int w = qt.imageWidth(img), h = qt.imageHeight(img);
        const int n = w > 0 ? std::min(h / w, kStripMaxFrames + 1) : 0;
        for (int k = 0; k < n; ++k)
        {
            alignas(16) char frame[64], scaled[64], pix[64];
            IconBuf icon;
            const bool scale = w != g_iconSize;
            qt.imageCopy(img, frame, 0, k * w, w, w);
            if (k == 0 && !a.hasPoster) { qt.imageCopy(frame, a.poster, 0, 0, w, w); a.hasPoster = true; }
            if (scale) qt.imageScaled(frame, scaled, g_iconSize, g_iconSize, 1 /* KeepAspectRatio */, 1 /* Smooth */);
            qt.pixmapFromImage(pix, scale ? scaled : frame, 0);
            qt.iconFromPixmap(icon.b, pix);
            qt.pixmapDtor(pix);
            if (scale) qt.imageDtor(scaled);
            qt.imageDtor(frame);
            a.icons.push_back(icon);
        }
        qt.imageDtor(img);
        g_loadUs += NowUs() - t0; ++g_loads;
        return n > 0;
    }
    // the frame a row shows now: the poster when still, else the loop's frame for this tick
    const IconBuf* AnimFrame(const Anim& a)
    {
        if (a.icons.empty()) return nullptr;
        if (!g_animate || a.icons.size() < 3) return &a.icons[0];
        return &a.icons[1 + size_t(g_lastAnimTick % (a.icons.size() - 1))];
    }
    void SetRowIcon(int row, const Entry& e, bool placeholder = false, bool reload = false, const std::vector<unsigned char>* strip = nullptr)
    {
        if (!g_grid || !g_qtOk || size_t(row) >= g_rows.size()) return;
        void* item = qt.item(g_grid, row);
        if (!item) return;
        Anim& a = g_anims[g_rows[size_t(row)]];
        if (reload || a.icons.empty())
        {
            FreeAnim(a);
            const std::wstring p = placeholder ? PlaceholderPath() : ThumbPath(e);
            if (strip && !placeholder) LoadIcons(p, a, strip);
            else if (FileExists(p)) LoadIcons(p, a);
        }
        a.seenMs = GetTickCount64();
        if (const IconBuf* icon = AnimFrame(a)) qt.setItemIcon(item, icon->b);
    }
    // the rows on screen step to the next frame kStripFps times a second
    void AnimateGrid(uint64_t now)
    {
        if (!g_animate || !g_grid || !g_qtOk) return;
        const uint64_t tick = now * kStripFps / 1000;
        if (tick == g_lastAnimTick) return;
        g_lastAnimTick = tick;
        for (int r : g_onScreen)
        {
            if (size_t(r) >= g_rows.size() || size_t(r) >= g_rowState.size() || g_rowState[size_t(r)] != 1) continue;
            const auto it = g_anims.find(g_rows[size_t(r)]);
            if (it == g_anims.end() || it->second.icons.size() < 3) continue;
            if (void* item = qt.item(g_grid, r)) qt.setItemIcon(item, AnimFrame(it->second)->b);
        }
    }
    void ShowPosters()
    {
        for (int r : g_onScreen)
        {
            if (size_t(r) >= g_rows.size()) continue;
            const auto it = g_anims.find(g_rows[size_t(r)]);
            if (it == g_anims.end() || it->second.icons.empty()) continue;
            if (void* item = qt.item(g_grid, r)) qt.setItemIcon(item, AnimFrame(it->second)->b);
        }
    }
    void TrimAnims(uint64_t now)
    {
        if (g_anims.size() <= kAnimCache) return;
        std::vector<std::pair<uint64_t, int>> age;
        for (const auto& kv : g_anims) if (kv.second.seenMs + 2000 < now) age.push_back({ kv.second.seenMs, kv.first });
        std::sort(age.begin(), age.end());
        for (size_t i = 0; i < age.size() && g_anims.size() > kAnimCache; ++i)
        {
            FreeAnim(g_anims[age[i].second]);
            g_anims.erase(age[i].second);
        }
    }

    // the rows only get text here, icons follow as rows come on screen (the Tick)
    void FillGrid(const std::vector<int>& rows, const std::string& title)
    {
        const ModToolsUiApi* ui = rp::host->ui;
        g_rows = rows;
        g_rowState.assign(g_rows.size(), 0);
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

    // ---- the top bar: scope, favorites, the name filter ----
    std::string NormFx(std::string s)
    {
        for (char& c : s) c = c == '\\' ? '/' : char(tolower(static_cast<unsigned char>(c)));
        const size_t first = s.find_first_not_of('/');
        s.erase(0, first == std::string::npos ? s.size() : first);
        if (s.size() > 4 && s.compare(s.size() - 4, 4, ".efx") == 0) s.resize(s.size() - 4);
        return s;
    }
    int Depth() { return rp::Has(&rp::host->PrefabDepth) ? rp::host->PrefabDepth(rp::self) : 0; }
    std::vector<std::string> UsedHere()
    {
        std::vector<std::string> v;
        br::fxswap::InUse(v);
        for (std::string& x : v) x = NormFx(x);
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        return v;
    }
    // this level's effects, with top: the levels above it too (captured before each Enter Prefab)
    std::vector<std::string> UsedList(bool top)
    {
        std::vector<std::string> v = UsedHere();
        if (top)
        {
            const int d = Depth();
            for (int l = 0; l < d && l < int(g_levelFx.size()); ++l) v.insert(v.end(), g_levelFx[size_t(l)].begin(), g_levelFx[size_t(l)].end());
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        }
        return v;
    }
    void Refilter(const std::string& text)
    {
        g_lastFilter = text;
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
        for (std::string& w : words) for (char& c : w) if (c == '_') c = ' ';
        std::set<std::string> use;
        if (g_scope) { g_lastUse = UsedList(g_scope == 2); use.insert(g_lastUse.begin(), g_lastUse.end()); }
        std::vector<int> rows;
        const bool filtered = g_scope || g_favOnly || !words.empty();
        for (size_t i = 0; i < g_all.size(); ++i)
        {
            if (filtered)
            {
                const std::string key = Lower(FullName(g_all[i]));
                if (g_scope && !use.count(key)) continue;
                if (g_favOnly && !g_favs.count(key)) continue;
                if (!words.empty())
                {
                    std::string hay = key;
                    for (char& c : hay) if (c == '_') c = ' ';
                    bool all = true;
                    for (const std::string& w : words) if (hay.find(w) == std::string::npos) { all = false; break; }
                    if (!all) continue;
                }
            }
            rows.push_back(int(i));
            if (!words.empty() && rows.size() >= kSearchCap) break;
        }
        std::string title = g_scope == 2 ? "In use (top)" : g_scope == 1 ? "In use" : "fx";
        if (g_favOnly) title += ", favorites";
        if (!words.empty()) title += rows.size() >= kSearchCap ? ", filtered (first " + std::to_string(kSearchCap) + ")" : ", filtered";
        FillGrid(rows, title);
    }
    void ShowAll() { Refilter(std::string()); }
    void ShowSearch(const std::string& text) { Refilter(text); }

    std::wstring FavPath() { return g_thumbRoot.substr(0, g_thumbRoot.find_last_of(L'\\')) + L"\\fx_favorites.txt"; }
    void LoadFavs()
    {
        g_favs.clear();
        FILE* f = _wfopen(FavPath().c_str(), L"rb");
        if (!f) return;
        char line[512];
        while (fgets(line, sizeof line, f))
        {
            std::string s = line;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
            if (!s.empty()) g_favs.insert(NormFx(s));
        }
        fclose(f);
    }
    void SaveFavs()
    {
        FILE* f = _wfopen(FavPath().c_str(), L"wb");
        if (!f) { rp::Log("fxpreview: could not write %s", Utf8(FavPath().c_str(), int(FavPath().size())).c_str()); return; }
        for (const std::string& n : g_favs) fprintf(f, "%s\r\n", n.c_str());
        fclose(f);
    }
    // Favorites are shared with Radiant's own browser: its QSettings group, HKCU\Software\Treyarch\Radiant\EffectsBrowser\Favorites,
    // value names "/dir/name" with "/" written "-!sl!-". fx_favorites.txt keeps a copy for when the browser is missing.
    bool ReadStockFavs(std::set<std::string>& out)
    {
        HKEY key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Treyarch\\Radiant\\EffectsBrowser\\Favorites", 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
        wchar_t value[1024];
        for (DWORD i = 0; ; ++i)
        {
            DWORD len = DWORD(sizeof value / sizeof value[0]);
            if (RegEnumValueW(key, i, value, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            std::string n = Utf8(value, int(len));
            for (size_t at; (at = n.find("-!sl!-")) != std::string::npos; ) n.replace(at, 6, "/");
            if (!n.empty()) out.insert(NormFx(n));
        }
        RegCloseKey(key);
        return true;
    }
    // the browser's row for `name` (proper case, "dir/name"), null when its tree does not list it
    void* StockItem(const std::string& name)
    {
        if (!g_host || !g_qtOk || !qt.modelRoot) return nullptr;
        void* model = *reinterpret_cast<void**>(g_host + off_Toolbox_Model);
        void* root = model ? qt.modelRoot(model) : nullptr;
        if (!root) return nullptr;
        alignas(16) char qs[16];
        qt.stringCtor(qs, ("/" + name).c_str());
        void* item = reinterpret_cast<void* (*)(void*, const void*)>(At<void*>(kToolboxFindItem))(root, qs);
        qt.stringDtor(qs);
        return item;
    }
    std::string ProperName(const std::string& key)
    {
        for (const Entry& e : g_all) if (NormFx(FullName(e)) == key) return FullName(e);
        return key;
    }
    // Radiant's Toggle Favorite on that row
    bool StockToggle(const std::string& name)
    {
        void* item = StockItem(name);
        if (!item) return false;
        reinterpret_cast<void (*)(void*, void*)>(At<void*>(kToolboxToggleFav))(g_host, item);
        return true;
    }
    // the shared list: Radiant's favorites once the browser is found, the file before
    void SyncFavs()
    {
        std::set<std::string> stock;
        if (!g_host || !ReadStockFavs(stock)) return;
        for (const std::string& f : g_favs)             // effects the browser does not list stay ours alone
            if (!stock.count(f) && !StockItem(ProperName(f))) stock.insert(f);
        if (stock == g_favs) return;
        g_favs = stock;
        SaveFavs();
        if (g_favOnly) Refilter(g_lastFilter);
    }
    // Radiant's browser leaves out the effects directly in fx\ (its Unsorted row stays empty): they go there, so Legacy
    // lists them and their favorites can be shared. Its own repopulate drops them, the tick puts them back.
    void* g_unsortedFilled = nullptr;   // the Unsorted row filled last
    int g_rootEffects = 0;              // how many went in
    bool FillUnsorted()
    {
        if (!g_host || !qt.itemCtor || !qt.itemAppendRow || !qt.itemSetEditable || !qt.itemRowCount || !qt.variantFromString || !qt.variantDtor || !qt.opNew) return false;
        void* unsorted = *reinterpret_cast<void**>(g_host + off_Toolbox_Unsorted);
        if (!unsorted || (unsorted == g_unsortedFilled && (qt.itemRowCount(unsorted) > 0 || !g_rootEffects))) return false;
        int added = 0;
        for (const Entry& e : g_all)
        {
            if (!e.dir.empty() || StockItem(e.name)) continue;
            void* item = qt.opNew(64);      // sizeof(QStandardItem) is 16, the model deletes it
            if (!item) break;
            memset(item, 0, 64);
            alignas(16) char text[16], name[16], var[32];
            qt.stringCtor(text, (e.name + ".efx").c_str());
            qt.itemCtor(item, text);
            qt.stringDtor(text);
            qt.itemSetEditable(item, false);
            qt.stringCtor(name, ("/" + e.name).c_str());
            qt.variantFromString(var, name);
            reinterpret_cast<void (*)(void*, const void*, int)>((*static_cast<void***>(item))[2])(item, var, 256);   // setData
            qt.variantDtor(var);
            qt.stringDtor(name);
            qt.itemAppendRow(unsorted, item);
            ++added;
        }
        g_unsortedFilled = unsorted;
        g_rootEffects = added;
        if (added) rp::Log("fxpreview: %d effects from fx\\ added to the FX Browser's Unsorted row", added);
        return added > 0;
    }
    // first attach: favorites made before the sharing go into Radiant's list
    void MergeFavs()
    {
        std::set<std::string> stock;
        if (!g_host || !ReadStockFavs(stock)) return;
        for (const std::string& f : g_favs)
            if (!stock.count(f)) StockToggle(ProperName(f));
        SyncFavs();
    }
    bool ToggleFavorite(const std::string& name)
    {
        const std::string key = NormFx(name);
        const bool now = !g_favs.count(key);
        if (StockToggle(ProperName(key))) { SyncFavs(); return g_favs.count(key) != 0; }
        if (now) g_favs.insert(key); else g_favs.erase(key);
        SaveFavs();
        if (g_favOnly) Refilter(g_lastFilter);
        return now;
    }

    // the grid rows on screen, by the item under the top-left corner and the item rectangles after it
    std::vector<int> VisibleRows()
    {
        std::vector<int> out;
        if (!g_qtOk || !g_grid || !g_gridViewport || g_rows.empty()) return out;
        const int vh = qt.widgetHeight(g_gridViewport);
        int first = -1;
        for (int probeY = 2; probeY < vh && first < 0; probeY += g_iconSize / 3)
        {
            const QPointRaw p = { g_iconSize / 2, probeY };
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

    // icons for the rows on screen, thumbnails queued (first) for those without a current one.
    // True when strips are left to cut (about 12 ms of it per call).
    bool RefreshVisible()
    {
        const std::vector<int> rows = VisibleRows();
        g_onScreen = rows;
        const uint64_t now = GetTickCount64(), until = NowUs() + 12000;
        bool more = false;
        size_t inserted = 0;
        for (int r : rows)
        {
            if (size_t(r) >= g_rowState.size()) continue;
            const int idx = g_rows[size_t(r)];
            const auto it = g_anims.find(idx);
            if (it != g_anims.end()) it->second.seenMs = now;
            if (g_rowState[size_t(r)]) continue;
            const Entry& e = g_all[size_t(idx)];
            if (ThumbCurrent(e))
            {
                if (it == g_anims.end() && NowUs() > until) { more = true; continue; }
                SetRowIcon(r, e);
                g_rowState[size_t(r)] = 1;
                continue;
            }
            g_rowState[size_t(r)] = 2;
            if (std::find(g_failed.begin(), g_failed.end(), idx) != g_failed.end()) { SetRowIcon(r, e, true); g_rowState[size_t(r)] = 1; continue; }
            if (std::find(g_queue.begin(), g_queue.end(), idx) != g_queue.end()) continue;
            g_queue.insert(g_queue.begin() + inserted++, idx);
            ++g_queueTotal;
        }
        TrimAnims(now);
        return more;
    }

    void Enqueue(const std::vector<int>& rows)
    {
        for (int r : rows)
            if (!ThumbCurrent(g_all[size_t(r)]) && std::find(g_failed.begin(), g_failed.end(), r) == g_failed.end()
                && std::find(g_queue.begin(), g_queue.end(), r) == g_queue.end()) g_queue.push_back(r);
        g_queueTotal = g_queue.size() + g_queueDone;
    }

    // makes a few queued thumbnails, within `budgetMs`, and refreshes the rows on show
    bool g_paceThumbs = true;           // "fxpreview dbg pace 0|1"
    uint64_t g_lastWorkUs = 0;          // the end of the last job slice (NowUs)
    uint64_t g_idleGapUs = 3000;        // "fxpreview dbg idlegap <ms>"
    void FinishThumb(int idx, bool ok, const std::string& why)
    {
        const Entry& e = g_all[size_t(idx)];
        if (!ok) { g_failed.push_back(idx); if (!why.empty()) rp::Log("fxpreview: thumbnail of %s failed: %s", FullName(e).c_str(), why.c_str()); }
        ++g_queueDone;
        for (size_t r = 0; r < g_rows.size(); ++r) if (g_rows[r] == idx) { SetRowIcon(int(r), e, !ok, true, ok && !g_doneStrip.empty() ? &g_doneStrip : nullptr); if (r < g_rowState.size()) g_rowState[r] = 1; break; }
        g_doneStrip.clear();
    }
    // a slice of the current strip, the next queued effect begun when none runs
    uint64_t g_batchBeganUs = 0, g_batchUs = 0;     // the queue from first job to empty (status)
    unsigned g_batchStrips = 0;
    void WorkQueue(uint64_t budgetUs)
    {
        if (!g_batchBeganUs) { g_batchBeganUs = NowUs(); g_batchStrips = 0; }
        while (!g_job.active && !g_queue.empty())
        {
            const int idx = g_queue.front();
            g_queue.erase(g_queue.begin());
            const Entry& e = g_all[size_t(idx)];
            std::string why;
            if (NeedsModels(FullName(e))) { g_modelSkipped.insert(idx); FinishThumb(idx, false, std::string()); }
            else if (!BeginJob(idx, FullName(e), ThumbPath(e), why)) FinishThumb(idx, false, why);
        }
        if (g_job.active)
        {
            const int idx = g_job.idx;
            const int r = StepJob(budgetUs);
            RestoreUserSteps();
            if (r != 0 && idx >= 0) { FinishThumb(idx, r > 0, g_job.why); ++g_batchStrips; }
        }
        if (g_queue.empty() && !g_job.active) { g_queueDone = g_queueTotal = 0; ReleaseStrip(); g_batchUs = NowUs() - g_batchBeganUs; g_batchBeganUs = 0; }
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

    // places `name` at the 3D view's point under (x, y) (pixels of the view's window) through the drop code the FX
    // Browser's drags reach: the entity lands where the cursor ray hits, 100 units out when it hits nothing
    bool DropOnCamera(const std::string& name, int x, int y)
    {
        char* bridge = Bridge();
        char* cam = CamWnd();
        HWND view = static_cast<HWND>(rp::host->CamWindow ? rp::host->CamWindow(rp::self) : nullptr);
        RECT rc = {};
        if (!bridge || !cam || !view || !GetClientRect(view, &rc) || !g_qtOk || !qt.mimeCtor || !qt.mimeDtor || !qt.mimeSetData || !qt.bytesCtor || !qt.bytesDtor) return false;
        // the drop code takes x from the left and h - y from the bottom (h = the camera widget's height, toolbar included),
        // in game view both inside the centred picture
        int pw = rc.right, ph = rc.bottom;
        if (*At<const char*>(kGameView) || *At<const char*>(kGameView + 1))
        {
            pw = *reinterpret_cast<const int*>(cam + 3764); ph = *reinterpret_cast<const int*>(cam + 3768);
            if (pw <= 0 || ph <= 0 || pw > rc.right || ph > rc.bottom) { pw = rc.right; ph = rc.bottom; }
        }
        const int left = (rc.right - pw) / 2, top = (rc.bottom - ph) / 2;
        const int dx = x - left, dy = qt.widgetHeight(cam) - (ph - (y - top));
        alignas(16) char mime[64] = {}, format[16], bytes[16];
        qt.mimeCtor(mime);
        qt.stringCtor(format, "radiant/x-fx");
        qt.bytesCtor(bytes, name.c_str(), int(name.size()));
        qt.mimeSetData(mime, format, bytes);
        reinterpret_cast<void (*)(char)>(At<void*>(kSelectDeselect))(1);    // a create with a selection converts it, a drop makes a new fx
        const bool ok = reinterpret_cast<bool (*)(char*, int, int, int, void*)>(At<void*>(kViewDrop))(bridge + 1960, 0, dx, dy, mime);
        if (ok) reinterpret_cast<void (*)(int)>(At<void*>(kUpdateViews))(-1);
        qt.bytesDtor(bytes);
        qt.stringDtor(format);
        qt.mimeDtor(mime);
        return ok;
    }
    // a Qt drag with the FX Browser's own data: the 3D and 2D views place the effect where it is dropped, as for that browser
    // The drag's loop takes the mouse release, so the grid stays in DragSelectingState and Qt skips its resize relayout
    // (QListView::resizeEvent wants NoState): back to NoState once the button is up.
    void IdleGrid()
    {
        if (!g_grid || !qt.viewState || !qt.viewSetState || (GetKeyState(VK_LBUTTON) & 0x8000)) return;
        const int st = qt.viewState(g_grid);
        if (st == 1 || st == 2) qt.viewSetState(g_grid, 0);     // DraggingState, DragSelectingState
    }
    void StartDrag(const std::string& name)
    {
        if (!g_qtOk || !qt.opNew || !qt.opDelete || !qt.dragCtor || !qt.dragDtor || !qt.dragSetMime || !qt.dragExec
            || !qt.mimeCtor || !qt.mimeSetData || !qt.bytesCtor || !qt.bytesDtor) { SetStatus("Dragging is not available."); return; }
        void* mime = qt.opNew(64);          // the drag owns and deletes it
        void* drag = qt.opNew(64);          // sizeof(QDrag) and sizeof(QMimeData) are 16
        if (!mime || !drag) { if (mime) qt.opDelete(mime); if (drag) qt.opDelete(drag); return; }
        memset(mime, 0, 64);
        memset(drag, 0, 64);
        qt.mimeCtor(mime);
        alignas(16) char format[16], bytes[16];
        qt.stringCtor(format, "radiant/x-fx");
        qt.bytesCtor(bytes, name.c_str(), int(name.size()));
        qt.mimeSetData(mime, format, bytes);
        qt.bytesDtor(bytes);
        qt.stringDtor(format);
        qt.dragCtor(drag, g_gridViewport);
        qt.dragSetMime(drag, mime);
        const int action = qt.dragExec(drag, 1 /* Qt::CopyAction */);     // runs until the drop
        qt.dragDtor(drag);
        qt.opDelete(drag);
        IdleGrid();
        if (action) SetStatus("Placed " + name + " in the map.");
    }

    // ---- pane callbacks ----
    void OnRestart(void*)
    {
        if (g_name.empty()) return;
        std::string why;
        if (!Start(g_name, 0, why)) SetStatus(why);
    }
    void OnStop(void*) { Stop(); SetStatus("Stopped."); }
    void OnPause(void*) { TogglePause(); }
    void OnRefresh(void*)
    {
        AbortJob(false);
        ScanTree();
        ForgetDefs();
        FreeAnims();
        g_failed.clear();
        g_modelSkipped.clear();
        g_usesModels.clear();
        g_queue.clear();
        g_queueDone = g_queueTotal = 0;
        ShowSearch(EditText(g_search));
        SetStatus("Refreshed: " + std::to_string(g_all.size()) + " effects, changed effects get new thumbnails as they show.");
    }
    void OnLoopBox(void*, int v)
    {
        g_loop = v != 0;
        rp::host->ConfigSet(rp::self, "fxpreview", "loop", g_loop ? "1" : "0");
        std::string why;
        if (g_loop && !g_effect && !g_name.empty()) Start(g_name, 0, why);    // it already ended: play it again
    }
    // ---- the Options menu ----
    void* g_gridAction = nullptr;
    void* g_playAllAction = nullptr;    // Play All: every thumbnail plays its effect ([fxpreview] animate)
    const int kIconMin = 100, kIconMax = 512;  // the Textures pane's slider: its value is the icon size in px (the thumbnail files are 128 px)
    bool ActionChecked(void* a) { return a && qt.actionIsChecked && qt.actionIsChecked(a); }
    void SetActionChecked(void* a, bool on) { if (a && qt.actionSetChecked) qt.actionSetChecked(a, on); }
    void IconLayout(int n)
    {
        g_iconSize = n;
        const QSizeRaw icon = { n, n }, cell = { n + 12, n + 36 };
        qt.setIconSize(g_grid, &icon);
        qt.setGridSize(g_grid, &cell);
        // the editor stylesheets style QAbstractItemView::item, which squeezes icon-mode cells: give the cells their own size
        char sheet[256];
        snprintf(sheet, sizeof sheet, "QListView { icon-size: %dpx; } QListView::item { width: %dpx; height: %dpx; padding: 2px; margin: 0px; border: none; }",
                 n, cell.w - 4, cell.h - 4);
        alignas(16) char qs[16];
        qt.stringCtor(qs, sheet);
        qt.setStyleSheet(g_grid, qs);
        qt.stringDtor(qs);
    }
    void ApplyIconSize(int n)
    {
        if (!g_grid || !g_qtOk) return;
        IconLayout(n);
        FreeAnims();                    // cut again at the new size as rows come on screen
        g_rowState.assign(g_rowState.size(), 0);
        g_nextVisibleMs = 0;
    }
    // while the slider moves: the new layout, and the rows on screen show their poster rescaled (a full cut takes too long)
    void LiveIconSize(int n)
    {
        if (!g_grid || !g_qtOk) return;
        const uint64_t t0 = NowUs();
        IconLayout(n);
        g_onScreen = VisibleRows();
        for (int r : g_onScreen)
        {
            if (size_t(r) >= g_rows.size()) continue;
            const auto it = g_anims.find(g_rows[size_t(r)]);
            if (it == g_anims.end() || !it->second.hasPoster) continue;
            Anim& a = it->second;
            alignas(16) char scaled[64], pix[64];
            IconBuf icon;
            qt.imageScaled(a.poster, scaled, n, n, 1 /* KeepAspectRatio */, 0 /* Fast */);
            qt.pixmapFromImage(pix, scaled, 0);
            qt.iconFromPixmap(icon.b, pix);
            qt.pixmapDtor(pix);
            qt.imageDtor(scaled);
            FreeIcons(a);
            a.icons.push_back(icon);
            if (void* item = qt.item(g_grid, r)) qt.setItemIcon(item, icon.b);
        }
        g_liveUs += NowUs() - t0; ++g_lives;
    }
    uint64_t g_iconLiveMs = 0;
    void OnIconSlider(void*, int v)
    {
        if (v == g_iconSize && !g_iconPending) return;
        g_iconPending = v;
        g_iconDueMs = GetTickCount64() + 250;
    }
    // live steps at most every 30 ms, the full cut once the slider rests
    void ApplyPendingIconSize(uint64_t now)
    {
        if (!g_iconPending) return;
        const int n = g_iconPending;
        if (now < g_iconDueMs)
        {
            if (n != g_iconSize && now >= g_iconLiveMs) { LiveIconSize(n); g_iconLiveMs = now + 30; }
            return;
        }
        g_iconPending = 0;
        ApplyIconSize(n);
        rp::host->ConfigSet(rp::self, "fxpreview", "icon_size", std::to_string(n).c_str());
    }
    // the Icon Size bar spans half the pane as in the Textures pane, its end is the grid side's minimum width
    void SizeIconRow()
    {
        if (!g_leftPane || !g_iconSlider || !g_root || !g_qtOk) return;
        const QPointRaw zero = { 0, 0 };
        QPointRaw slider = {}, left = {};
        qt.mapFromGlobal(g_iconSlider, &slider, &zero);
        qt.mapFromGlobal(g_leftPane, &left, &zero);
        const int x = left.x - slider.x, paneW = qt.widgetWidth(g_root);
        if (x <= 0 || paneW <= 0 || (paneW == g_iconRowPaneW && x == g_iconRowX)) return;
        g_iconRowPaneW = paneW;
        g_iconRowX = x;
        const int w = std::max(100, (paneW - 8 - x) / 2);
        rp::host->ui->SetMinimumSize(g_iconSlider, w, 0);
        qt.widgetSetMaxWidth(g_iconSlider, w);
    }
    // XYZ Grid: the box's ground grid and corner axes, and the thumbnails from the cache that matches
    void SetShowGrid(bool on)
    {
        g_showGrid = on;
        SetActionChecked(g_gridAction, on);
        g_boxDirty = true;
        rp::host->ConfigSet(rp::self, "fxpreview", "grid", on ? "1" : "0");
        const std::wstring root = on ? g_thumbBase : g_thumbBase + L"_nogrid";
        if (g_thumbBase.empty() || root == g_thumbRoot) return;
        AbortJob(false);
        g_queue.clear();
        g_queueDone = g_queueTotal = 0;
        g_thumbRoot = root;
        StampThumbCache();
        if (g_qtOk) FreeAnims();
        g_rowState.assign(g_rowState.size(), 0);
        g_nextVisibleMs = 0;
    }
    void OnGridAction(void*) { SetShowGrid(ActionChecked(g_gridAction)); }
    void SetAnimate(bool on)
    {
        g_animate = on;
        SetActionChecked(g_playAllAction, on);
        ShowPosters();
        rp::host->ConfigSet(rp::self, "fxpreview", "animate", on ? "1" : "0");
    }
    void OnPlayAllAction(void*) { SetAnimate(ActionChecked(g_playAllAction)); }
    void OnResetCamera(void*) { ResetOrbit(); }
    void OnScope(void* user, int v)
    {
        if (!v) return;
        g_scope = int(reinterpret_cast<intptr_t>(user));
        g_nextUseMs = GetTickCount64() + 1500;
        Refilter(g_lastFilter);
    }
    void OnFavBox(void*, int v) { g_favOnly = v != 0; Refilter(g_lastFilter); }

    // ---- the right-click menu of a grid item ----
    void CopyText(const std::string& text)
    {
        if (!OpenClipboard(nullptr)) return;
        EmptyClipboard();
        const std::wstring w = Wide(text);
        if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t)))
        {
            memcpy(GlobalLock(h), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(h);
            if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
        }
        CloseClipboard();
    }
    void ApplyName(const std::string& name)
    {
        const int n = br::fxswap::ApplyToSelection(name);
        char buf[320];
        snprintf(buf, sizeof buf, n > 0 ? "%s set on %d selected fx entit%s." : n == 0 ? "%s: select fx entities first (double-click sets their effect, a drag into the 3D view places a new one)." : "%s: not applied (%d).",
                 name.c_str(), n, n == 1 ? "y" : "ies");
        SetStatus(buf);
    }
    void OnMenuFavorite(void*)
    {
        if (g_menuName.empty()) return;
        SetStatus(g_menuName + (ToggleFavorite(g_menuName) ? " added to the favorites." : " removed from the favorites."));
    }
    void OnMenuCopy(void*) { if (!g_menuName.empty()) { CopyText(g_menuName); SetStatus("Copied " + g_menuName + "."); } }
    void OnSlider(void*, int v)
    {
        if (g_settingSlider || g_name.empty()) return;
        std::string why;
        if (!Start(g_name, -v * 10, why)) SetStatus(why);
    }
    void OnSearch(void*, int) { g_filterDue = 0; Refilter(EditText(g_search)); }
    void OnGridRow(void*, int row)
    {
        if (row < 0 || size_t(row) >= g_rows.size()) return;
        const std::string name = FullName(g_all[size_t(g_rows[size_t(row)])]);
        if (name == g_name && g_effect) return;     // the press already started it
        Preview(name);
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
    int OnGridEvent(void*, ModToolsWidget watched, int type)
    {
        if (watched != g_gridViewport) return 0;
        const DWORD pos = GetMessagePos();
        const int x = short(LOWORD(pos)), y = short(HIWORD(pos));
        if (type == 2)                     // MouseButtonPress: a drag may start here
        {
            g_pressed = (GetKeyState(VK_LBUTTON) & 0x8000) != 0;
            g_pressX = x; g_pressY = y;
            g_dragName = g_pressed ? NameAtCursor() : std::string();
            if (g_pressed && g_dragName.empty())        // a click on empty space picks nothing
            {
                Stop();
                g_name.clear();
                SetStatus("Click an effect to play it.");
            }
            else if (g_pressed && (g_dragName != g_name || !g_effect)) Preview(g_dragName);   // also a click on the selected tile
        }
        else if (type == 5)                // MouseMove
        {
            if (g_pressed && !g_dragName.empty() && (GetKeyState(VK_LBUTTON) & 0x8000)
                && (abs(x - g_pressX) > GetSystemMetrics(SM_CXDRAG) || abs(y - g_pressY) > GetSystemMetrics(SM_CYDRAG)))
            {
                g_pressed = false;
                StartDrag(g_dragName);
                return 1;
            }
        }
        else if (type == 3) g_pressed = false;         // MouseButtonRelease
        else if (type == 4)                            // double-click: the selected fx entities get it
        {
            const std::string name = NameAtCursor();
            if (name.empty()) return 0;
            ApplyName(name);
        }
        else if (type == 82 && g_gridMenu && qt.menuPopup)     // ContextMenu
        {
            g_menuName = NameAtCursor();
            if (g_menuName.empty()) return 0;
            if (g_favAction) rp::host->ui->SetActionText(g_favAction, g_favs.count(NormFx(g_menuName)) ? "Remove from Favorites" : "Add to Favorites");
            const QPointRaw at = { x, y };
            qt.menuPopup(g_gridMenu, &at, nullptr);
            return 1;
        }
        return 0;
    }

    // ---- mouse on the preview box: left drag orbits, middle or Shift drag pans, right drag and the wheel zoom ----
    LRESULT CALLBACK WheelHook(int code, WPARAM wp, LPARAM lp)
    {
        if (code == HC_ACTION && wp == PM_REMOVE)
        {
            const MSG* m = reinterpret_cast<const MSG*>(lp);
            if (m->message == WM_MOUSEWHEEL) g_wheelDelta = GET_WHEEL_DELTA_WPARAM(m->wParam);
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    int OnBoxEvent(void*, ModToolsWidget watched, int type)
    {
        if (watched != g_box) return 0;
        POINT pt;
        GetCursorPos(&pt);
        const bool left = (GetKeyState(VK_LBUTTON) & 0x8000) != 0, mid = (GetKeyState(VK_MBUTTON) & 0x8000) != 0, right = (GetKeyState(VK_RBUTTON) & 0x8000) != 0;
        switch (type)
        {
        case 2:                            // MouseButtonPress
            g_dragMode = mid || (left && (GetKeyState(VK_SHIFT) & 0x8000)) ? 2 : left ? 1 : right ? 3 : 0;
            g_dragX = pt.x; g_dragY = pt.y;
            return g_dragMode ? 1 : 0;
        case 5:                            // MouseMove
        {
            if (!g_dragMode) return 0;
            if (!left && !mid && !right) { g_dragMode = 0; return 0; }
            const float dx = float(pt.x - g_dragX), dy = float(pt.y - g_dragY);
            g_dragX = pt.x; g_dragY = pt.y;
            if (dx == 0 && dy == 0) return 1;
            if (g_dragMode == 1)
            {
                g_orbitYaw -= dx * 0.4f;
                g_orbitPitch += dy * 0.4f;
                if (g_orbitPitch > 89.0f) g_orbitPitch = 89.0f;
                if (g_orbitPitch < -89.0f) g_orbitPitch = -89.0f;
                if (g_orbitYaw > 360.0f) g_orbitYaw -= 360.0f;
                if (g_orbitYaw < 0.0f) g_orbitYaw += 360.0f;
            }
            else if (g_dragMode == 2)
            {
                const BoxView v = MakeBoxView(FitOf(g_name));
                const int size = std::max(64, std::min(qt.widgetHeight(g_box), qt.widgetWidth(g_box)));
                const float perPx = 2.0f * v.tanV * v.dist * kBoxCrop / float(size);
                for (int i = 0; i < 3; ++i) g_pan[i] += (-v.r[i] * dx + v.u[i] * dy) * perPx;
            }
            else g_orbitZoom *= expf(dy * 0.01f);
            if (g_orbitZoom < 0.05f) g_orbitZoom = 0.05f;
            if (g_orbitZoom > 20.0f) g_orbitZoom = 20.0f;
            g_boxDirty = true;
            return 1;
        }
        case 3:                            // MouseButtonRelease
            if (!left && !mid && !right) g_dragMode = 0;
            return 1;
        case 4:                            // MouseButtonDblClick
            ResetOrbit();
            return 1;
        case 31:                           // Wheel
            if (g_wheelDelta)
            {
                g_orbitZoom *= powf(0.85f, float(g_wheelDelta) / 120.0f);
                if (g_orbitZoom < 0.05f) g_orbitZoom = 0.05f;
                if (g_orbitZoom > 20.0f) g_orbitZoom = 20.0f;
                g_wheelDelta = 0;
                g_boxDirty = true;
            }
            return 1;
        case 82:                           // ContextMenu: a right drag zooms instead
            return 1;
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
        Get(qt.opNew, GetModuleHandleW(L"MSVCR110.dll"), "??2@YAPEAX_K@Z");
        Get(qt.menuCtor, w, "??0QMenu@@QEAA@PEAVQWidget@@@Z");
        Get(qt.buttonSetMenu, w, "?setMenu@QPushButton@@QEAAXPEAVQMenu@@@Z");
        Get(qt.menuPopup, w, "?popup@QMenu@@QEAAXAEBVQPoint@@PEAVQAction@@@Z");
        Get(qt.actionSetCheckable, w, "?setCheckable@QAction@@QEAAX_N@Z");
        Get(qt.actionSetChecked, w, "?setChecked@QAction@@QEAAX_N@Z");
        Get(qt.actionIsChecked, w, "?isChecked@QAction@@QEBA_NXZ");
        Get(qt.mimeCtor, c, "??0QMimeData@@QEAA@XZ");
        Get(qt.mimeDtor, c, "??1QMimeData@@UEAA@XZ");
        Get(qt.mimeSetData, c, "?setData@QMimeData@@QEAAXAEBVQString@@AEBVQByteArray@@@Z");
        Get(qt.bytesCtor, c, "??0QByteArray@@QEAA@PEBDH@Z");
        Get(qt.bytesDtor, c, "??1QByteArray@@QEAA@XZ");
        Get(qt.opDelete, GetModuleHandleW(L"MSVCR110.dll"), "??3@YAXPEAX@Z");
        Get(qt.widgetLayout, w, "?layout@QWidget@@QEBAPEAVQLayout@@XZ");
        Get(qt.toolButtonCtor, w, "??0QToolButton@@QEAA@PEAVQWidget@@@Z");
        Get(qt.iconFromFile, g, "??0QIcon@@QEAA@AEBVQString@@@Z");
        Get(qt.buttonSetIcon, w, "?setIcon@QAbstractButton@@QEAAXAEBVQIcon@@@Z");
        Get(qt.buttonSetIconSize, w, "?setIconSize@QAbstractButton@@QEAAXAEBVQSize@@@Z");
        Get(qt.buttonSetCheckable, w, "?setCheckable@QAbstractButton@@QEAAX_N@Z");
        Get(qt.buttonSetText, w, "?setText@QAbstractButton@@QEAAXAEBVQString@@@Z");
        Get(qt.toolButtonSetStyle, w, "?setToolButtonStyle@QToolButton@@QEAAXW4ToolButtonStyle@Qt@@@Z");
        Get(qt.toolButtonSetPopupMode, w, "?setPopupMode@QToolButton@@QEAAXW4ToolButtonPopupMode@1@@Z");
        Get(qt.toolButtonSetMenu, w, "?setMenu@QToolButton@@QEAAXPEAVQMenu@@@Z");
        Get(qt.sliderSetOrientation, w, "?setOrientation@QAbstractSlider@@QEAAXW4Orientation@Qt@@@Z");
        Get(qt.currentItem, w, "?currentItem@QListWidget@@QEBAPEAVQListWidgetItem@@XZ");
        Get(qt.isItemSelected, w, "?isItemSelected@QListWidget@@QEBA_NPEBVQListWidgetItem@@@Z");
        Get(qt.clearSelection, w, "?clearSelection@QAbstractItemView@@QEAAXXZ");
        Get(qt.viewState, w, "?state@QAbstractItemView@@IEBA?AW4State@1@XZ");
        Get(qt.viewSetState, w, "?setState@QAbstractItemView@@IEAAXW4State@1@@Z");
        Get(qt.setCurrentItem, w, "?setCurrentItem@QListWidget@@QEAAXPEAVQListWidgetItem@@@Z");
        Get(qt.modelRoot, g, "?invisibleRootItem@QStandardItemModel@@QEBAPEAVQStandardItem@@XZ");
        Get(qt.itemCtor, g, "??0QStandardItem@@QEAA@AEBVQString@@@Z");
        Get(qt.itemAppendRow, g, "?appendRow@QStandardItem@@QEAAXPEAV1@@Z");
        Get(qt.itemSetEditable, g, "?setEditable@QStandardItem@@QEAAX_N@Z");
        Get(qt.itemRowCount, g, "?rowCount@QStandardItem@@QEBAHXZ");
        Get(qt.variantFromString, c, "??0QVariant@@QEAA@AEBVQString@@@Z");
        Get(qt.variantDtor, c, "??1QVariant@@QEAA@XZ");
        Get(qt.layoutRemoveWidget, w, "?removeWidget@QLayout@@QEAAXPEAVQWidget@@@Z");
        Get(qt.widgetSetParent, w, "?setParent@QWidget@@QEAAXPEAV1@@Z");
        Get(qt.deleteLater, c, "?deleteLater@QObject@@QEAAXXZ");
        Get(qt.visibleRegion, w, "?visibleRegion@QWidget@@QEBA?AVQRegion@@XZ");
        Get(qt.regionIsEmpty, g, "?isEmpty@QRegion@@QEBA_NXZ");
        Get(qt.regionDtor, g, "??1QRegion@@QEAA@XZ");
        Get(qt.dragCtor, g, "??0QDrag@@QEAA@PEAVQObject@@@Z");
        Get(qt.dragDtor, g, "??1QDrag@@UEAA@XZ");
        Get(qt.dragSetMime, g, "?setMimeData@QDrag@@QEAAXPEAVQMimeData@@@Z");
        Get(qt.dragExec, g, "?exec@QDrag@@QEAA?AW4DropAction@Qt@@V?$QFlags@W4DropAction@Qt@@@@@Z");
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
            && Get(qt.widgetSetMaxWidth, w, "?setMaximumWidth@QWidget@@QEAAXH@Z")
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
            && Get(qt.imageFromFile, g, "??0QImage@@QEAA@AEBVQString@@PEBD@Z")
            && Get(qt.imageWidth, g, "?width@QImage@@QEBAHXZ")
            && Get(qt.imageHeight, g, "?height@QImage@@QEBAHXZ")
            && Get(qt.stringCtor, c, "??0QString@@QEAA@PEBD@Z")
            && Get(qt.stringDtor, c, "??1QString@@QEAA@XZ");
    }

    // a QMenu from Qt's own allocator (its parent deletes it with that CRT)
    void* NewMenu(void* parent)
    {
        if (!qt.opNew || !qt.menuCtor || !qt.buttonSetMenu || !qt.menuPopup || !rp::Has(&rp::host->AddMenuItem)) return nullptr;
        void* m = qt.opNew(64);             // sizeof(QMenu) is 48
        if (!m) return nullptr;
        memset(m, 0, 64);
        qt.menuCtor(m, parent);
        return m;
    }
    // checked -1 = a plain item
    void* MenuItem(void* menu, const char* text, ModToolsCallback fn, void* user, int checked)
    {
        void* a = rp::host->AddMenuItem(rp::self, menu, text, fn, user);
        if (a && checked >= 0 && qt.actionSetCheckable && qt.actionSetChecked) { qt.actionSetCheckable(a, true); qt.actionSetChecked(a, checked != 0); }
        return a;
    }

    // the Animation window's controls: QToolButtons with Radiant's own icons at 30 px, its slider class
    void SetButtonIcon(void* button, const char* path)
    {
        if (!button || !qt.iconFromFile || !qt.buttonSetIcon) return;
        alignas(16) char qs[16], icon[16];
        qt.stringCtor(qs, path);
        qt.iconFromFile(icon, qs);
        qt.buttonSetIcon(button, icon);
        qt.iconDtor(icon);
        qt.stringDtor(qs);
    }
    // a toggle (checkable, 24 px like the Animation window's repeat) falls back to a check box
    void* AnimButton(void* parent, const char* icon, const char* text, const char* tip, bool toggle = false)
    {
        const ModToolsUiApi* ui = rp::host->ui;
        void* b = nullptr;
        if (qt.opNew && qt.toolButtonCtor && qt.iconFromFile && qt.buttonSetIcon && qt.buttonSetIconSize && (!toggle || qt.buttonSetCheckable)
            && (b = qt.opNew(64)) != nullptr)
        {
            memset(b, 0, 64);               // sizeof(QToolButton) is 48, the parent deletes it
            qt.toolButtonCtor(b, parent);
            SetButtonIcon(b, icon);
            const QSizeRaw size = { toggle ? 24 : 30, toggle ? 24 : 30 };
            qt.buttonSetIconSize(b, &size);
            if (toggle) qt.buttonSetCheckable(b, true);
        }
        else b = toggle ? ui->CheckBox(text, parent) : ui->PushButton(text, parent);
        ui->SetToolTip(b, tip);
        return b;
    }
    // Options as in the Models and Textures panes: a text-only QToolButton whose menu opens on a press
    void* OptionsButton(void* parent, bool* tool)
    {
        *tool = false;
        void* b = nullptr;
        if (qt.opNew && qt.toolButtonCtor && qt.buttonSetText && qt.toolButtonSetStyle && qt.toolButtonSetPopupMode && qt.toolButtonSetMenu
            && (b = qt.opNew(64)) != nullptr)
        {
            memset(b, 0, 64);
            qt.toolButtonCtor(b, parent);
            alignas(16) char text[16];
            qt.stringCtor(text, "Options");
            qt.buttonSetText(b, text);
            qt.stringDtor(text);
            qt.toolButtonSetStyle(b, 1);         // Qt::ToolButtonTextOnly
            qt.toolButtonSetPopupMode(b, 2);     // InstantPopup
            *tool = true;
            return b;
        }
        return rp::host->ui->PushButton("Options", parent);
    }
    void* AnimSlider(void* parent)
    {
        char* sl = qt.opNew && qt.sliderSetOrientation ? static_cast<char*>(qt.opNew(160)) : nullptr;
        if (!sl) return rp::host->ui->Slider(1, 0, 300, parent);
        memset(sl, 0, 160);
        reinterpret_cast<void* (*)(void*, void*)>(At<void*>(kAnimSliderCtor))(sl, parent);
        qt.sliderSetOrientation(sl, 1);
        *reinterpret_cast<int*>(sl + 48) = 0;
        *reinterpret_cast<float*>(sl + 112) = 30.0f;
        *reinterpret_cast<char**>(sl + 120) = g_sliderAnim;
        rp::host->ui->SliderSetRange(sl, 0, 300);
        g_animSlider = sl;
        return sl;
    }

    void ApplyLegacy();
    void OnLegacyBox(void*, int v)
    {
        g_legacy = v != 0;
        rp::host->ConfigSet(rp::self, "fxpreview", "legacy", g_legacy ? "1" : "0");
        for (void* box : { g_legacyBox, g_legacyBox2 }) if (box) rp::host->ui->SetChecked(box, g_legacy ? 1 : 0);
        ApplyLegacy();
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

        // the top bar of the Models and Textures panes
        void* row = ui->HBox();
        ui->LayoutSpacing(row, 10);
        g_scopeAll = ui->RadioButton("All", root);
        g_scopeUse = ui->RadioButton("All In Use", root);
        g_scopeTop = ui->RadioButton("All In Use Top", root);
        ui->SetChecked(g_scopeAll, 1);
        ui->SetToolTip(g_scopeUse, "The effects the fx entities at the current level use");
        ui->SetToolTip(g_scopeTop, "The effects the fx entities use at the current level and the levels above it (inside a prefab: the map's too)");
        g_favBox = ui->CheckBox("Favorites", root);
        ui->SetToolTip(g_favBox, "Only your favorite effects (right-click an effect to add or remove it)");
        ui->BoxAddWidget(row, g_scopeAll, 0);
        ui->BoxAddWidget(row, g_scopeUse, 0);
        ui->BoxAddWidget(row, g_scopeTop, 0);
        ui->BoxAddWidget(row, g_favBox, 0);
        ui->BoxAddStretch(row, 1);
        bool optionsTool = false;
        void* options = OptionsButton(root, &optionsTool);
        ui->BoxAddWidget(row, options, 0);
        ui->BoxAddLayout(col, row, 0);
        g_search = ui->LineEdit("", root);
        ui->SetPlaceholder(g_search, "Filter FX By Name");
        ui->BoxAddWidget(col, g_search, 0);

        // left: the grid with the toggle row and the status under it, right: the preview column over the full height
        void* split = ui->Splitter(1, root);
        void* left = ui->Widget(root);
        void* lcol = ui->VBox(left);
        ui->LayoutMargins(lcol, 0, 0, 0, 0);
        ui->LayoutSpacing(lcol, 3);
        g_grid = ui->ListWidget(left);
        qt.setViewMode(g_grid, 1);          // IconMode
        qt.setMovement(g_grid, 0);          // Static
        qt.setResizeMode(g_grid, 1);        // Adjust
        qt.setUniformItemSizes(g_grid, true);
        qt.setWordWrap(g_grid, true);
        qt.setSpacing(g_grid, 2);
        ApplyIconSize(g_iconSize);
        ui->BoxAddWidget(lcol, g_grid, 1);
        void* right = ui->Widget(root);
        void* rcol = ui->VBox(right);
        ui->LayoutMargins(rcol, 0, 0, 0, 0);
        ui->LayoutSpacing(rcol, 3);
        g_box = ui->Label("", right);
        ui->SetMinimumSize(g_box, 160, 160);
        ui->SetToolTip(g_box, "The effect plays here, drawn by Radiant's fx system without touching the 3D view.\nLeft drag orbits, the wheel or a right drag zooms, a middle or Shift drag pans, double-click resets the view.");
        ui->BoxAddWidget(rcol, g_box, 1);
        // the preview's own controls under the box
        row = ui->HBox();
        void* restart = AnimButton(right, ":/animation/animation/player_begin.png", "Restart", "Restart the effect");
        ui->BoxAddWidget(row, restart, 0);
        g_pauseButton = AnimButton(right, ":/animation/animation/player_pause.png", "Pause", "Pause or play the preview");
        ui->BoxAddWidget(row, g_pauseButton, 0);
        void* stop = AnimButton(right, ":/animation/animation/Stop-64.png", "Stop", "Stop the preview");
        ui->BoxAddWidget(row, stop, 0);
        g_loopBox = AnimButton(right, ":/animation/animation/Reload.png", "Loop", "Loop: restart the effect when it ends", true);
        ui->SetChecked(g_loopBox, g_loop ? 1 : 0);
        ui->BoxAddWidget(row, g_loopBox, 0);
        ui->BoxAddStretch(row, 1);
        g_slider = AnimSlider(right);
        if (!g_animSlider)
        {
            g_elapsed = ui->Label("0.00 / 0.00 s", right);
            ui->BoxAddWidget(row, g_elapsed, 0);
        }
        ui->BoxAddLayout(rcol, row, 0);
        ui->SetToolTip(g_slider, "Where the effect is in its life (drag to jump, it restarts that far in)");
        ui->BoxAddWidget(rcol, g_slider, 0);
        ui->SetLayout(right, rcol);
        row = ui->HBox();
        g_legacyBox2 = ui->CheckBox("Legacy", left);
        ui->SetChecked(g_legacyBox2, g_legacy ? 1 : 0);
        ui->SetToolTip(g_legacyBox2, "Radiant's own FX Browser in place of the thumbnails and the preview box");
        ui->BoxAddWidget(row, g_legacyBox2, 0);
        void* gap = ui->Widget(left);
        ui->SetMinimumSize(gap, 16, 0);
        ui->BoxAddWidget(row, gap, 0);
        ui->BoxAddWidget(row, ui->Label("Icon Size:", left), 0);
        g_iconSlider = ui->Slider(1, kIconMin, kIconMax, left);
        ui->SliderSetValue(g_iconSlider, g_iconSize);
        ui->SetToolTip(g_iconSlider, "Thumbnail size");
        ui->BoxAddWidget(row, g_iconSlider, 0);         // its width follows the pane (SizeIconRow)
        ui->BoxAddStretch(row, 1);
        ui->BoxAddLayout(lcol, row, 0);

        ui->SetLayout(left, lcol);
        ui->SplitterAdd(split, left, 3);
        ui->SplitterAdd(split, right, 0);
        qt.splitterSetCollapsible = reinterpret_cast<void (*)(void*, int, bool)>(GetProcAddress(GetModuleHandleW(L"Qt5Widgets64r.dll"), "?setCollapsible@QSplitter@@QEAAXH_N@Z"));
        if (qt.splitterSetCollapsible) qt.splitterSetCollapsible(split, 0, false);     // the preview stops where the Icon Size bar ends
        ui->SetVisible(right, 0);
        g_boxColumn = right;
        g_leftPane = left;
        ui->BoxAddWidget(col, split, 1);
        ui->SetLayout(root, col);

        rp::host->OnClicked(rp::self, g_pauseButton, OnPause, nullptr);
        rp::host->OnClicked(rp::self, restart, OnRestart, nullptr);
        rp::host->OnClicked(rp::self, stop, OnStop, nullptr);
        rp::host->OnChanged(rp::self, g_loopBox, OnLoopBox, nullptr);
        rp::host->OnChanged(rp::self, g_legacyBox2, OnLegacyBox, nullptr);
        rp::host->OnChanged(rp::self, g_iconSlider, OnIconSlider, nullptr);
        rp::host->OnChanged(rp::self, g_scopeAll, OnScope, reinterpret_cast<void*>(intptr_t(0)));
        rp::host->OnChanged(rp::self, g_scopeUse, OnScope, reinterpret_cast<void*>(intptr_t(1)));
        rp::host->OnChanged(rp::self, g_scopeTop, OnScope, reinterpret_cast<void*>(intptr_t(2)));
        rp::host->OnChanged(rp::self, g_favBox, OnFavBox, nullptr);
        rp::host->OnChanged(rp::self, g_slider, OnSlider, nullptr);
        rp::host->OnChanged(rp::self, g_search, OnSearch, nullptr);
        rp::host->OnChanged(rp::self, g_grid, OnGridRow, nullptr);
        g_gridViewport = FindViewport(g_grid);
        g_gridFilter = rp::host->MakeEventFilter(rp::self, OnGridEvent, nullptr);
        if (g_gridViewport && g_gridFilter) ui->InstallFilter(g_gridViewport, g_gridFilter);
        g_boxFilter = rp::host->MakeEventFilter(rp::self, OnBoxEvent, nullptr);
        if (g_boxFilter) ui->InstallFilter(g_box, g_boxFilter);
        if (!g_wheelHook) g_wheelHook = SetWindowsHookExW(WH_GETMESSAGE, WheelHook, nullptr, GetCurrentThreadId());

        // Options: Play All, the grid, the box camera, Refresh
        if (void* menu = NewMenu(options))
        {
            g_playAllAction = MenuItem(menu, "Play All", OnPlayAllAction, nullptr, g_animate);
            g_gridAction = MenuItem(menu, "XYZ Grid", OnGridAction, nullptr, g_showGrid);
            rp::host->AddMenuSeparator(rp::self, menu);
            MenuItem(menu, "Reset Preview Camera", OnResetCamera, nullptr, -1);
            MenuItem(menu, "Refresh", OnRefresh, nullptr, -1);
            (optionsTool ? qt.toolButtonSetMenu : qt.buttonSetMenu)(options, menu);
        }
        else rp::host->OnClicked(rp::self, options, OnRefresh, nullptr);     // no menu: the button refreshes
        if ((g_gridMenu = NewMenu(g_grid)) != nullptr)
        {
            g_favAction = MenuItem(g_gridMenu, "Add to Favorites", OnMenuFavorite, nullptr, -1);
            MenuItem(g_gridMenu, "Copy Name", OnMenuCopy, nullptr, -1);
        }

        g_root = root;
        g_bar = ui->Widget(nullptr);
        void* barRow = ui->HBox();
        ui->LayoutMargins(barRow, 4, 0, 4, 2);
        g_legacyBox = ui->CheckBox("Legacy", g_bar);
        ui->SetChecked(g_legacyBox, g_legacy ? 1 : 0);
        ui->SetToolTip(g_legacyBox, "Radiant's own FX Browser in place of the thumbnails and the preview box");
        ui->BoxAddWidget(barRow, g_legacyBox, 0);
        ui->BoxAddStretch(barRow, 1);
        ui->SetLayout(g_bar, barRow);
        rp::host->OnChanged(rp::self, g_legacyBox, OnLegacyBox, nullptr);
    }

    // true while the thumbnails and the box are on screen (the FX Browser open, its tab in front, Legacy off)
    bool Shown()
    {
        if (!g_host || !g_root || g_legacy || !qt.visibleRegion || !qt.regionIsEmpty || !qt.regionDtor) return false;
        alignas(16) char region[16] = {};
        qt.visibleRegion(g_root, region);
        const bool empty = qt.regionIsEmpty(region);
        qt.regionDtor(region);
        return !empty;
    }
    void ApplyLegacy()
    {
        if (!g_host) return;
        const ModToolsUiApi* ui = rp::host->ui;
        if (g_legacy) Stop();
        else SyncFavs();                                // Legacy may have changed Radiant's favorites
        ui->SetVisible(g_root, g_legacy ? 0 : 1);
        ui->SetVisible(g_bar, g_legacy ? 1 : 0);        // our window has its own Legacy box in the toggle row
        if (g_stock) ui->SetVisible(g_stock, g_legacy ? 1 : 0);
    }
    // puts the bar and the window into the FX Browser pane once Radiant made it
    void Attach()
    {
        if (g_host || !g_root || !g_bar || !qt.widgetLayout) return;
        char* browser = static_cast<char*>(br::fxswap::Browser());
        if (!browser) return;
        void* layout = qt.widgetLayout(browser);
        void* stock = *reinterpret_cast<void**>(browser + off_Dock_Content);
        if (!layout || !stock) return;
        rp::host->ui->BoxAddWidget(layout, g_root, 1);
        rp::host->ui->BoxAddWidget(layout, g_bar, 0);       // under Radiant's own browser
        g_host = browser;
        g_stock = stock;
        FillUnsorted();
        MergeFavs();
        ApplyLegacy();
        rp::Log("fxpreview: in the FX Browser 0x%p (legacy %d)", browser, g_legacy ? 1 : 0);
    }
    // hands the pane back to Radiant's own browser (unload)
    void Detach()
    {
        if (g_host)
        {
            void* layout = qt.widgetLayout ? qt.widgetLayout(g_host) : nullptr;
            if (layout && qt.layoutRemoveWidget) { qt.layoutRemoveWidget(layout, g_bar); qt.layoutRemoveWidget(layout, g_root); }
            if (g_stock) rp::host->ui->SetVisible(g_stock, 1);
            g_host = nullptr;
            g_stock = nullptr;
        }
        if (g_animSlider) { *reinterpret_cast<char**>(g_animSlider + 120) = nullptr; *reinterpret_cast<int*>(g_animSlider + 48) = 3; }   // our name buffer goes with the DLL
        for (void* w : { g_bar, g_root })
            if (w && qt.widgetSetParent && qt.deleteLater) { rp::host->ui->SetVisible(w, 0); qt.widgetSetParent(w, nullptr); qt.deleteLater(w); }
        g_bar = g_root = g_legacyBox = g_legacyBox2 = g_loopBox = g_leftPane = nullptr;
        g_animSlider = nullptr;
        g_gridAction = g_playAllAction = nullptr;
    }

    std::string Status()
    {
        char buf[1200];
        snprintf(buf, sizeof buf, "fxpreview: %s, effect '%s' handle %u elapsed %.2f of %.2f s, paused %d, fx rendering %d, mgr %s, fx time %d ms, %zu effects, grid %zu rows, queue %zu, loop %d, play all %d, box frames %u shown %u empty %u (draw %.2f ms, show %.2f ms) %.1f fps of %u, async %d, misses %u, box %dx%d, orbit %.1f %.1f x%.2f, tanV %.4f (fov %.4f), fit %d %.3f %.3f %.3f cam %.0f %.0f %.0f judge %.0f, grid %d, scope %d fav %d (%zu), held %ld, icon calls %u skipped %u, readbacks dropped %u, exposure guard %d, fixed ev %.2f, fx slots %d of 4096, exposure %.3f %.4f %.3f, animate %d, strips cut %zu, on screen %zu, strips made %u (last %.1f ms, longest slice %.1f ms, refilmed %u), per strip %.1f ms work %.1f ms wall %.1f slices (%llu strips), last batch %u in %.0f ms, icons cut %u (%.2f ms each), live size steps %u (%.2f ms each)\n",
                 !g_enabled ? "off ([fxpreview] enabled=0)" : g_ready ? "ready" : "unavailable", g_name.c_str(), g_effect, ElapsedSeconds(), g_lengthMs / 1000.0,
                 g_boxPaused ? 1 : 0, FxRenderingOn() ? 1 : 0, PlaybackMgr() ? "found" : "missing", FxTimeMs(), g_all.size(), g_rows.size(), g_queue.size(),
                 g_loop ? 1 : 0, g_animate ? 1 : 0, g_boxFrames, g_boxShown, g_emptyFrames, g_boxFrames ? double(g_boxUs) / 1000.0 / g_boxFrames : 0.0, g_boxShown ? double(g_showUs) / 1000.0 / g_boxShown : 0.0, g_fpsNow, g_boxFps, g_asyncBox ? 1 : 0, g_boxMisses, g_box && g_qtOk ? qt.widgetWidth(g_box) : -1, g_box && g_qtOk ? qt.widgetHeight(g_box) : -1, g_orbitYaw, g_orbitPitch, g_orbitZoom, g_tanV, g_tanFov, FitOf(g_name) ? int(FitOf(g_name)->ok) : -1, FitOf(g_name) ? FitOf(g_name)->cx : 0.0f, FitOf(g_name) ? FitOf(g_name)->cy : 0.0f, FitOf(g_name) ? FitOf(g_name)->size : 0.0f, g_boxCam[0], g_boxCam[1], g_boxCam[2], g_judgeDist, g_showGrid ? 1 : 0, g_scope, g_favOnly ? 1 : 0, g_favs.size(), g_heldUpdates, g_iconCalls, g_iconSkips, g_readbacksDropped, g_exposureGuard ? 1 : 0, g_fixedEv, LiveEffects(), CamWnd() ? reinterpret_cast<const float*>(CamWnd() + off_Cam_Exposure)[0] : 0.0f, CamWnd() ? reinterpret_cast<const float*>(CamWnd() + off_Cam_Exposure)[1] : 0.0f, CamWnd() ? reinterpret_cast<const float*>(CamWnd() + off_Cam_Exposure)[2] : 0.0f,
                 g_animate ? 1 : 0, g_anims.size(), g_onScreen.size(), g_strips, g_stripUs / 1000.0, g_sliceMaxUs / 1000.0, g_refilms,
                 g_sumStrips ? double(g_sumWorkUs) / 1000.0 / g_sumStrips : 0.0, g_sumStrips ? double(g_sumWallMs) / g_sumStrips : 0.0, g_sumStrips ? double(g_sumSlices) / g_sumStrips : 0.0, (unsigned long long)g_sumStrips, g_batchStrips, g_batchUs / 1000.0, g_loads, g_loads ? g_loadUs / 1000.0 / g_loads : 0.0, g_lives, g_lives ? g_liveUs / 1000.0 / g_lives : 0.0);
        return buf;
    }
}

void Setup()
{
    g_mainTid = GetCurrentThreadId();
    g_enabled = rp::host->ConfigGetInt(rp::self, "fxpreview", "enabled", 0) != 0;
    if (!g_enabled) { rp::Log("fxpreview: off ([fxpreview] enabled=0)"); return; }
    const int fps = rp::host->ConfigGetInt(rp::self, "fxpreview", "box_fps", 30);
    g_boxFps = unsigned(fps < 5 ? 5 : fps > 60 ? 60 : fps);
    const int icon = rp::host->ConfigGetInt(rp::self, "fxpreview", "icon_size", 128);
    g_iconSize = std::max(kIconMin, std::min(kIconMax, icon));
    g_legacy = rp::host->ConfigGetInt(rp::self, "fxpreview", "legacy", 0) != 0;
    g_showGrid = rp::host->ConfigGetInt(rp::self, "fxpreview", "grid", 1) != 0;
    g_animate = rp::host->ConfigGetInt(rp::self, "fxpreview", "animate", 1) != 0;
    g_loop = rp::host->ConfigGetInt(rp::self, "fxpreview", "loop", 1) != 0;
    {
        char ev[32] = {};
        rp::host->ConfigGet(rp::self, "fxpreview", "exposure", "15.6", ev, sizeof ev);
        const float v = float(atof(ev));
        g_fixedEv = v <= 0.0f ? 0.0f : std::max(5.0f, std::min(25.0f, v));
    }
    rt.holderCtor   = At<HolderCtorFn>(kEdHolderCtor);
    rt.loadEfx      = At<LoadEfxFn>(kFxLoadEfx);
    rt.loadLights   = At<LoadLightsFn>(kFxLoadLights);
    rt.fixSegments  = At<FixSegmentsFn>(kFxFixSegments);
    rt.registerDef  = At<RegisterDefFn>(kFxRegisterDef);
    rt.defByHandle  = At<DefByHandleFn>(kFxDefByHandle);
    rt.spawn        = At<SpawnFn>(kFxSpawn);
    rt.kill         = At<KillFn>(kFxKill);
    rt.release      = At<ReleaseFn>(kFxRelease);
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
    rt.anglesToAxes = At<void (*)(void*)>(kCamAnglesToAxes);
    if (void* upd = At<void*>(kPlaybackUpdate))
        if (!rp::host->Detour || !rp::host->Detour(rp::self, upd, reinterpret_cast<void*>(&hk_MgrUpdate), reinterpret_cast<void**>(&g_origMgrUpdate))) g_origMgrUpdate = nullptr;
    if (void* adapt = At<void*>(kExposureAdapt))
        if (!rp::host->Detour || !rp::host->Detour(rp::self, adapt, reinterpret_cast<void*>(&hk_ExposureAdapt), reinterpret_cast<void**>(&g_origExposureAdapt))) g_origExposureAdapt = nullptr;
    if (void* icon = At<void*>(kFxLightIcon))
        if (!rp::host->Detour || !rp::host->Detour(rp::self, icon, reinterpret_cast<void*>(&hk_LightIcon), reinterpret_cast<void**>(&g_origLightIcon))) g_origLightIcon = nullptr;
    if (void* stopAll = At<void*>(kPlaybackStopAll))
        if (!rp::host->Detour || !rp::host->Detour(rp::self, stopAll, reinterpret_cast<void*>(&hk_StopAll), reinterpret_cast<void**>(&g_origStopAll))) g_origStopAll = nullptr;
    if (void* killOne = At<void*>(kFxKillOne))
        if (!rp::host->Detour || !rp::host->Detour(rp::self, killOne, reinterpret_cast<void*>(&hk_KillOne), reinterpret_cast<void**>(&g_origKillOne))) g_origKillOne = nullptr;
    if (void* childSpawns = At<void*>(kFxChildSpawns))
        if (!rp::host->Detour || !rp::host->Detour(rp::self, childSpawns, reinterpret_cast<void*>(&hk_ChildSpawns), reinterpret_cast<void**>(&g_origChildSpawns))) g_origChildSpawns = nullptr;
    if (void* upd1 = At<void*>(kFxEffectUpdate))
        if (!rp::host->Detour || !rp::host->Detour(rp::self, upd1, reinterpret_cast<void*>(&hk_EffectUpdate), reinterpret_cast<void**>(&g_origEffectUpdate))) g_origEffectUpdate = nullptr;
    if (void* setCam = At<void*>(kFxSetCamera))
        if (!rp::host->Detour || !rp::host->Detour(rp::self, setCam, reinterpret_cast<void*>(&hk_SetCamera), reinterpret_cast<void**>(&g_origSetCamera))) g_origSetCamera = nullptr;
    if (void* sprites = At<void*>(kFxDrawSprites))
        if (!rp::host->Detour || !rp::host->Detour(rp::self, sprites, reinterpret_cast<void*>(&hk_FxDrawSprites), reinterpret_cast<void**>(&g_origFxDrawSprites))) g_origFxDrawSprites = nullptr;
    if (void* drawFx = At<void*>(kFxDrawEffect))
        if (!rp::host->Detour || !rp::host->Detour(rp::self, drawFx, reinterpret_cast<void*>(&hk_FxDrawEffect), reinterpret_cast<void**>(&g_origFxDrawEffect))) g_origFxDrawEffect = nullptr;
    if (void* lists = At<void*>(kDebugLists))
        if (!rp::host->Detour || !rp::host->Detour(rp::self, lists, reinterpret_cast<void*>(&hk_DebugLists), reinterpret_cast<void**>(&g_origDebugLists))) g_origDebugLists = nullptr;
    if (void* lines = At<void*>(kCachedLines))
        if (!rp::host->Detour || !rp::host->Detour(rp::self, lines, reinterpret_cast<void*>(&hk_CachedLines), reinterpret_cast<void**>(&g_origCachedLines))) g_origCachedLines = nullptr;
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
    g_ready = rt.holderCtor && rt.loadEfx && rt.loadLights && rt.fixSegments && rt.registerDef && rt.defByHandle && rt.spawn && rt.kill && rt.release && rt.effectPtr
           && rt.renderOption && rt.sysCount && rt.sysPtr && rt.colors && rt.mainWin && rt.mgrVtbl && rt.mgrUpdate && rt.now && rt.viewSet && rt.viewIndex
           && rt.brushCacheUpd && rt.poolA && rt.poolB && rt.beginFrame && rt.endFrame && rt.viewTarget && rt.viewDepth && rt.setTargets && rt.clear
           && rt.camDraw && rt.dxDevice && rt.dxContext && rt.swapChain0 && rp::host->RadiantRoot;
    if (!g_ready) { rp::Log("fxpreview: unavailable (Radiant mismatch)"); return; }
    g_qtOk = BindQt();
    const wchar_t* rootDir = rp::host->RadiantRoot(rp::self);
    g_fxRoot = std::wstring(rootDir ? rootDir : L"") + L"\\share\\raw\\fx";
    g_thumbRoot = std::wstring(rootDir ? rootDir : L"") + L"\\radiant\\fx_thumbs";
    {
        wchar_t dir[MAX_PATH] = {};         // test Radiants keep their own cache
        char d8[MAX_PATH] = {};
        rp::host->ConfigGet(rp::self, "fxpreview", "thumb_dir", "", d8, sizeof d8);
        if (d8[0] && MultiByteToWideChar(CP_UTF8, 0, d8, -1, dir, MAX_PATH) > 0) g_thumbRoot = dir;
    }
    g_thumbBase = g_thumbRoot;
    if (!g_showGrid) g_thumbRoot = g_thumbBase + L"_nogrid";
    StampThumbCache();
    LoadFavs();
    BuildPane();
    if (g_grid)
    {
        ScanTree();
        ShowAll();
    }
    rp::Log("fxpreview: %s", g_root ? "ready, waiting for the FX Browser" : "ready, no window (Qt exports missing)");
}

void Tick()
{
    if (!g_ready) return;
    WatchClock();
    if (!g_clockLive) return;
    static bool reclaimed = false;
    if (!reclaimed) { reclaimed = true; ReclaimStage(); }
    const uint64_t now = GetTickCount64();
    if (g_widenColumn) { rp::host->ui->SetMinimumSize(g_widenColumn, 0, 0); g_widenColumn = nullptr; }
    ShowColumn(GridSelected());
    ApplyPendingIconSize(now);
    SizeIconRow();
    IdleGrid();
    if (now >= g_nextAttachMs)
    {
        g_nextAttachMs = now + 1000;
        if (!g_host) Attach();
        else if (FillUnsorted()) MergeFavs();      // the browser repopulated (Reload) and dropped them
    }
    const bool visible = Shown();
    if (visible)
    {
        BoxTick(now);
        if (now >= g_nextVisibleMs) g_nextVisibleMs = now + (RefreshVisible() ? 15 : 200);
        AnimateGrid(now);
        // one slice after each of the user's frames (hidden frames back to back shift the user's next frame), back to back
        // with a short gap for Qt's events while the view is idle
        if (!g_queue.empty() || g_job.active)
        {
            const uint64_t us = NowUs(), gap = us - g_lastUserUs > 250000 ? g_idleGapUs : 33000;
            if (!g_paceThumbs || g_hiddenSince == 0 || us - g_lastWorkUs > gap) { WorkQueue(g_paceThumbs ? g_jobSliceUs : 80000); g_lastWorkUs = NowUs(); }
        }
        if (g_search && now >= g_nextTypedMs)      // the filter follows typing after a short pause
        {
            g_nextTypedMs = now + 100;
            const std::string typed = EditText(g_search);
            if (typed != g_typedFilter) { g_typedFilter = typed; g_filterDue = now + 250; }
        }
        if (g_filterDue && now >= g_filterDue) { g_filterDue = 0; if (g_typedFilter != g_lastFilter) Refilter(g_typedFilter); }
        if (g_scope && now >= g_nextUseMs)          // fx entities added, removed or changed
        {
            g_nextUseMs = now + 1500;
            if (UsedList(g_scope == 2) != g_lastUse) Refilter(g_lastFilter);
        }
    }
    else
    {
        AbortJob(true);                 // a hidden pane makes no thumbnails, and the fx clock is not held
        if (g_effect && !rt.effectPtr(0, g_effect)) g_effect = 0;
    }
    if (now < g_nextLabelMs) return;
    g_nextLabelMs = now + 50;
    if (!rp::host->ui) return;
    char buf[64];
    if (g_lengthMs > 0) snprintf(buf, sizeof buf, "%.2f / %.2f s", ShownElapsed(), g_lengthMs / 1000.0);
    else snprintf(buf, sizeof buf, "%.2f s (looping)", ShownElapsed());
    if (g_elapsed) SetLabel(g_elapsed, g_lastLabel, buf);
    if (g_animSlider)
    {
        const float top = float(SliderTopMs()) / 1000.0f;
        reinterpret_cast<float*>(g_animSlider + 96)[0] = float(ShownElapsed());
        reinterpret_cast<float*>(g_animSlider + 96)[1] = top;
        *reinterpret_cast<int*>(g_animSlider + 116) = int(top * 30.0f + 0.5f) + 1;
    }
    if (g_slider)
    {
        const int top = SliderTopMs() / 10;
        int v = int(ShownElapsed() * 100.0);
        if (g_lengthMs <= 0 && top > 0) v %= top;
        g_settingSlider = true;
        rp::host->ui->SliderSetValue(g_slider, v < 0 ? 0 : v > top ? top : v);
        g_settingSlider = false;
    }
    const char* pause = ShownPaused() ? ":/animation/animation/player_play.png" : ":/animation/animation/player_pause.png";
    if (g_pauseButton && g_lastPause != pause) { SetButtonIcon(g_pauseButton, pause); g_lastPause = pause; }
}

void Shutdown()
{
    if (!g_ready) return;
    g_queue.clear();
    AbortJob(false);
    Stop();
    ForgetDefs();
    ReleaseSlots();
    ReleaseStrip();
    StopSaver();
    if (g_qtOk) { FreeAnims(); Detach(); }
    if (g_wheelHook) { UnhookWindowsHookEx(g_wheelHook); g_wheelHook = nullptr; }
}

void BeforeScopeChange()
{
    if (!g_ready) return;
    const int d = Depth();
    if (d < 0 || d > 64) return;
    if (int(g_levelFx.size()) < d + 1) g_levelFx.resize(size_t(d) + 1);
    g_levelFx[size_t(d)] = UsedHere();
}

void ScopeChanged(int depth)
{
    if (!g_ready) return;
    if (depth >= 0 && int(g_levelFx.size()) > depth) g_levelFx.resize(size_t(depth));
    g_nextUseMs = 0;
}

void MapLoaded()
{
    AbortJob(true);
    g_levelFx.clear();
    g_stagePlaced = false;
    g_nextUseMs = 0;
}

bool HidingMap() { return g_thumbFrame && GetCurrentThreadId() == g_thumbThread; }
bool HidingMapHelpers() { return g_skipMapDraws && HidingMap(); }

bool Command(const std::string& command, std::string& reply)
{
    if (command.compare(0, 9, "fxpreview") != 0) return false;
    std::string arg = command.size() > 10 ? command.substr(10) : std::string();
    if (arg.empty() || arg == "status") { reply = Status(); return true; }
    if (!g_ready) { reply = Status(); return true; }
    if (arg == "stop") { Stop(); reply = "fxpreview: stopped\n"; return true; }
    if (arg == "legacy 0" || arg == "legacy 1")
    {
        OnLegacyBox(nullptr, arg.back() == '1');
        reply = std::string("fxpreview: legacy ") + (g_legacy ? "on" : "off") + (g_host ? ", in the FX Browser" : ", FX Browser not found yet") + (Shown() ? ", shown" : "") + "\n";
        return true;
    }
    if (arg == "dbg view")       // the fx step's view parameters with the user's camera and with the stage camera
    {
        typedef float* (*ViewSetupFn)(void* cam, float* out);
        ViewSetupFn setup = At<ViewSetupFn>(kViewSetup);
        char* cam = CamWnd();
        if (!setup || !cam) { reply = "fxpreview: no view setup\n"; return true; }
        float a[14] = {}, b[14] = {};
        setup(cam, a);
        float camAt[3];
        FrameCamera(FitOf(g_name), camAt);
        { StageView view(camAt); setup(cam, b); }
        const char* s = FxSystem();
        char buf[700];
        snprintf(buf, sizeof buf, "fxpreview: view user [%.0f %.0f %.0f | %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f] stage [%.0f %.0f %.0f | %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f] sys+2308=%d dt=%d game=%d planes=%d camset=%d\n",
                 a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13],
                 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13],
                 s ? s[2308] : -1, s ? *reinterpret_cast<const int*>(s + 2296) : -1, rp::host->GameView ? rp::host->GameView(rp::self) : -1,
                 s ? *reinterpret_cast<const int*>(s + off_Sys_PlaneCount) : -1, s ? *reinterpret_cast<const int*>(s + 12) : -1);
        reply = buf;
        return true;
    }
    if (arg.compare(0, 9, "dbg look ") == 0)      // turns the user's camera (test aid for the exposure guard)
    {
        char* cam = CamWnd();
        float a[2] = { 0, 0 };
        if (!cam || !rt.anglesToAxes || sscanf(arg.c_str() + 9, "%f %f", &a[0], &a[1]) != 2) { reply = "fxpreview dbg look <pitch> <yaw>\n"; return true; }
        const float angles[3] = { a[0], a[1], 0 };
        memcpy(cam + off_Cam_Angles, angles, sizeof angles);
        rt.anglesToAxes(cam + 200);
        reply = "fxpreview: camera turned\n";
        return true;
    }
    if (arg.compare(0, 9, "dbg turn ") == 0)      // write angles, draw one hidden frame, read the view setup's forward back
    {
        typedef float* (*ViewSetupFn)(void* cam, float* out);
        ViewSetupFn setup = At<ViewSetupFn>(kViewSetup);
        char* cam = CamWnd();
        float a[2] = { 0, 0 };
        if (!setup || !cam || sscanf(arg.c_str() + 9, "%f %f", &a[0], &a[1]) != 2) { reply = "fxpreview dbg turn <pitch> <yaw>\n"; return true; }
        float saved[3], before[14] = {}, after[14] = {}, late[14] = {};
        memcpy(saved, cam + off_Cam_Angles, sizeof saved);
        const float angles[3] = { a[0], a[1], 0 };
        memcpy(cam + off_Cam_Angles, angles, sizeof angles);
        setup(cam, before);
        std::vector<unsigned char> px; int w = 0, h = 0;
        const bool drew = HiddenFrame(px, w, h);
        setup(cam, after);
        memcpy(cam + off_Cam_Angles, saved, sizeof saved);
        setup(cam, late);
        char buf[300];
        snprintf(buf, sizeof buf, "fxpreview: turn drew %d, forward before [%.2f %.2f %.2f] after [%.2f %.2f %.2f] restored [%.2f %.2f %.2f]\n", drew ? 1 : 0,
                 before[3], before[4], before[5], after[3], after[4], after[5], late[3], late[4], late[5]);
        reply = buf;
        return true;
    }
    if (arg.compare(0, 4, "dbg ") == 0)
    {
        if (arg.compare(4, 6, "elems ") == 0)       // every elem of a def as floats (layout research)
        {
            const char* cdef = rt.defByHandle(LoadDef(CleanName(arg.substr(10))));
            if (!cdef) { reply = "fxpreview elems: not loaded\n"; return true; }
            const unsigned short* counts = reinterpret_cast<const unsigned short*>(cdef + 12);
            const char* elems = *reinterpret_cast<const char* const*>(cdef + off_Def_Elems);
            const int n = counts[0] + counts[1] + counts[2];
            reply = "counts " + std::to_string(counts[0]) + " " + std::to_string(counts[1]) + " " + std::to_string(counts[2]) + "\n";
            for (int i = 0; i < n && elems; ++i)
            {
                const float* f = reinterpret_cast<const float*>(elems + size_t(i) * 584);
                for (int k = 0; k < 146; ++k) { char b[60]; snprintf(b, sizeof b, "%d:%d:%g:%08x\n", i, k * 4, f[k], reinterpret_cast<const unsigned*>(f)[k]); reply += b; }
            }
            return true;
        }
        if (arg.compare(4, 5, "scan ") == 0)        // lit pixels of one look at each age, plus the def's length
        {
            const std::string name = CleanName(arg.substr(9));
            const int def = LoadDef(name);
            if (!def) { reply = "fxpreview scan: not loaded " + name + "\n"; return true; }
            PlaceStage();
            Dispose(g_effect);
            SetStageDef(def);
            static const float axis[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
            static const int ages[] = { 0, 5, 10, 20, 40, 80, 160, 350, 1000, 1800, 3000 };
            float camAt[3];
            FrameCamera(kMeasureDist, camAt);
            reply = "fxpreview scan " + name + " len " + std::to_string(EffectLengthMs(def)) + ":";
            {
                QuietScope quiet;
                StageView view(camAt);
                StepFx(0.0);
                for (int age : ages)
                {
                    unsigned e = rt.spawn(def, kStage, axis, -age, 1.0f);
                    if (!e) { reply += " refused"; break; }
                    StepFx(0.001);
                    std::vector<unsigned char> px; int w = 0, h = 0;
                    const bool got = HiddenFrame(px, w, h);
                    Dispose(e);
                    size_t lit = 0;
                    if (got) for (size_t p = 0; p + 3 < px.size(); p += 16)
                        if (abs(int(px[p]) - 46) + abs(int(px[p + 1]) - 46) + abs(int(px[p + 2]) - 46) > 40) ++lit;
                    reply += " " + std::to_string(age) + "=" + std::to_string(lit);
                }
                if (quiet.Hit()) reply += " (assert)";
            }
            RestoreUserSteps();
            reply += "\n";
            return true;
        }
        if (arg.compare(4, 6, "stage ") == 0)
        {
            float v[3] = { 0, 0, 0 };
            g_stagePlaced = true;
            if (sscanf(arg.c_str() + 10, "%f %f %f", &v[0], &v[1], &v[2]) == 3) memcpy(kStage, v, sizeof v);
            else if (arg.compare(10, 4, "here") == 0 && rp::host->GetCamera)      // 224 units (or the given distance) in front of the user's camera
            {
                float o[3], a[3], f[3];
                const float dist = arg.size() > 15 ? float(atof(arg.c_str() + 15)) : 224.0f;
                if (rp::host->GetCamera(rp::self, o, a)) { CameraForward(f); for (int i = 0; i < 3; ++i) kStage[i] = o[i] + f[i] * dist; }
            }
            char buf[128];
            snprintf(buf, sizeof buf, "fxpreview: stage %.0f %.0f %.0f\n", kStage[0], kStage[1], kStage[2]);
            reply = buf;
            return true;
        }
        if (arg.find("gameview") != std::string::npos) g_dbgNoGameView = arg.back() == '0';
        if (arg.find("cull") != std::string::npos) g_noCull = arg.back() == '0';
        if (arg == "dbg stripes") g_fakeStripes = true;
        if (arg == "dbg forgetfits") g_fits.clear();
        if (arg.compare(4, 8, "mapdraw ") == 0) g_skipMapDraws = arg.back() == '0';
        if (arg.compare(4, 10, "gpushrink ") == 0) g_gpuShrink = arg.back() == '1';
        if (arg.compare(4, 8, "idlegap ") == 0) g_idleGapUs = uint64_t(std::max(0, atoi(arg.c_str() + 12))) * 1000;
        if (arg.compare(4, 6, "slice ") == 0) g_jobSliceUs = uint64_t(std::max(1, atoi(arg.c_str() + 10))) * 1000;
        if (arg.compare(4, 5, "expo ") == 0) g_expoTraceLeft = atoi(arg.c_str() + 9);
        if (arg.compare(4, 5, "pace ") == 0) g_paceThumbs = arg.back() == '1';
        if (arg.compare(4, 8, "discard ") == 0) g_discardAfter = atoi(arg.c_str() + 12);
        if (arg.find("exposure") != std::string::npos) g_exposureGuard = arg.back() == '1';
        if (arg.compare(4, 8, "lastbox ") == 0 || arg.compare(4, 6, "frame ") == 0 || arg.compare(4, 6, "shown ") == 0)
        {
            const bool frame = arg.compare(4, 6, "frame ") == 0;
            const std::string out = arg.substr(arg.compare(4, 8, "lastbox ") == 0 ? 12 : 10);
            std::vector<unsigned char> px;
            int w = 0, h = 0;
            if (frame)
            {
                const BoxView bv = MakeBoxView(FitOf(g_name));
                StageView view(bv.eye, bv.ang);
                StepBox();
                HiddenFrame(px, w, h);
            }
            else if (arg.compare(4, 6, "shown ") == 0) { px = g_shownImg; w = h = g_shownSize; }
            else { px = g_lastSquare; w = h = g_lastSide; }
            if (frame && w > 0)      // where the projection puts the stage origin and the pivot in this frame
            {
                BoxView bv = MakeBoxView(FitOf(g_name));
                bv.tanV = g_tanV;
                Projector P = {};
                P.v = bv; P.focal = float(h) * 0.5f / bv.tanV; P.cx = float(w) * 0.5f; P.cy = float(h) * 0.5f; P.scale = 1.0f;
                float a[3], b[3], ax = 0, ay = 0, bx = 0, by = 0;
                P.Project(kStage, ax, ay, 0.01f, a);
                P.Project(bv.pivot, bx, by, 0.01f, b);
                char buf[200];
                snprintf(buf, sizeof buf, "fxpreview: stage at %.1f %.1f, pivot at %.1f %.1f, eye %.1f %.1f %.1f pivot %.1f %.1f %.1f tanV %.4f\n", ax, ay, bx, by, bv.eye[0], bv.eye[1], bv.eye[2], bv.pivot[0], bv.pivot[1], bv.pivot[2], bv.tanV);
                reply += buf;
            }
            bool ok = false;
            if (w > 0 && h > 0 && px.size() >= size_t(w) * h * 4)
            {
                alignas(16) char img[64], qs[16];
                qt.imageCtor(img, px.data(), w, h, w * 4, 4, nullptr, nullptr);
                qt.stringCtor(qs, out.c_str());
                ok = qt.imageSave(img, qs, "PNG", -1);
                qt.stringDtor(qs);
                qt.imageDtor(img);
            }
            reply += "fxpreview: " + std::to_string(w) + "x" + std::to_string(h) + (ok ? " saved " : " not saved ") + out + "\n";
            return true;
        }
        if (arg == "dbg resetstats") { g_boxFrames = g_boxShown = g_emptyFrames = g_boxMisses = 0; g_boxUs = g_showUs = 0; g_sliceMaxUs = 0; g_sumWorkUs = g_sumWallMs = g_sumSlices = g_sumStrips = 0; g_loadUs = 0; g_loads = 0; g_liveUs = 0; g_lives = 0; }
        if (arg.compare(4, 5, "hold ") == 0) g_holdStage = atoi(arg.c_str() + 9);
        if (arg.compare(4, 4, "seq ") == 0) { g_seqLeft = atoi(arg.c_str() + 8); g_seq.clear(); }
        if (arg.compare(4, 8, "restore ") == 0) g_restoreSteps = atoi(arg.c_str() + 12);
        if (arg.compare(4, 6, "steps ") == 0) g_boxSteps = atoi(arg.c_str() + 10);
        if (arg.compare(4, 8, "minstep ") == 0) g_minStepMs = atof(arg.c_str() + 12);
        if (arg.compare(4, 8, "droplog ") == 0) g_dropTraceLeft = atoi(arg.c_str() + 12);
        if (arg.compare(4, 6, "async ") == 0) { g_asyncBox = arg.back() == '1'; if (!g_asyncBox) { CollectBox(true); ReleaseSlots(); } }
        reply = "fxpreview: game view switch " + std::string(g_dbgNoGameView ? "off" : "on") + ", spawn cull " + (g_noCull ? "off" : "on") + (g_origSetCamera ? "" : " (camera hook missing)") + "\n";
        return true;
    }
    if (arg == "pause") { TogglePause(); reply = Status(); return true; }
    if (arg == "refresh") { OnRefresh(nullptr); reply = Status(); return true; }
    if (arg == "all") { ShowAll(); reply = Status(); return true; }
    if (arg.compare(0, 6, "orbit ") == 0)       // orbit <yaw> <pitch> [zoom], orbit reset
    {
        float y = g_orbitYaw, pt = g_orbitPitch, z = g_orbitZoom;
        if (arg.compare(6, 5, "reset") == 0) ResetOrbit();
        else if (sscanf(arg.c_str() + 6, "%f %f %f", &y, &pt, &z) >= 2) { g_orbitYaw = y; g_orbitPitch = pt < -89 ? -89 : pt > 89 ? 89 : pt; g_orbitZoom = z < 0.05f ? 0.05f : z > 20 ? 20 : z; g_boxDirty = true; }
        reply = Status();
        return true;
    }
    if (arg.compare(0, 9, "exposure ") == 0) { g_fixedEv = std::max(0.0f, float(atof(arg.c_str() + 9))); g_boxDirty = true; reply = Status(); return true; }
    if (arg == "animate 0" || arg == "animate 1") { SetAnimate(arg.back() == '1'); reply = Status(); return true; }
    if (arg == "grid 0" || arg == "grid 1") { SetShowGrid(arg.back() == '1'); reply = Status(); return true; }
    if (arg.compare(0, 4, "fps ") == 0) { const int f = atoi(arg.c_str() + 4); g_boxFps = unsigned(f < 5 ? 5 : f > 60 ? 60 : f); reply = Status(); return true; }
    if (arg.compare(0, 6, "scope ") == 0)       // scope 0|1|2: All, All In Use, All In Use Top
    {
        const int v = atoi(arg.c_str() + 6);
        void* radio = v == 2 ? g_scopeTop : v == 1 ? g_scopeUse : g_scopeAll;
        if (radio) rp::host->ui->SetChecked(radio, 1);
        if (g_scope != v) OnScope(reinterpret_cast<void*>(intptr_t(v)), 1);
        reply = Status();
        return true;
    }
    if (arg == "favonly 0" || arg == "favonly 1") { if (g_favBox) rp::host->ui->SetChecked(g_favBox, arg.back() == '1'); g_favOnly = arg.back() == '1'; Refilter(g_lastFilter); reply = Status(); return true; }
    if (arg == "slider")     // what the play slider and button show, and whether a thumbnail is being filmed
    {
        char b[200]; snprintf(b, sizeof b, "fxpreview: slider %d, shown %.2f s, %s, job %d, grid state %d\n", g_slider ? rp::host->ui->SliderValue(g_slider) : -1, ShownElapsed(), ShownPaused() ? "paused" : "playing", g_job.active ? 1 : 0,
                             g_grid && qt.viewState ? qt.viewState(g_grid) : -1);
        reply = b; return true;
    }
    if (arg.compare(0, 9, "favorite ") == 0) { const bool on = ToggleFavorite(CleanName(arg.substr(9))); reply = std::string("fxpreview: ") + (on ? "added " : "removed ") + arg.substr(9) + "\n"; return true; }
    if (arg.compare(0, 9, "iconsize ") == 0) { const int n = std::max(kIconMin, std::min(kIconMax, atoi(arg.c_str() + 9))); if (g_iconSlider) rp::host->ui->SliderSetValue(g_iconSlider, n); OnIconSlider(nullptr, n); reply = Status(); return true; }
    if (arg == "inuse")
    {
        const std::vector<std::string> v = UsedList(true);
        reply = "fxpreview: depth " + std::to_string(Depth()) + ", " + std::to_string(v.size()) + " in use (top):";
        for (const std::string& n : v) reply += " " + n;
        reply += "\n";
        return true;
    }
    if (arg == "makeall")
    {
        std::vector<int> all;
        for (size_t i = 0; i < g_all.size(); ++i) all.push_back(int(i));
        Enqueue(all);
        reply = Status();
        return true;
    }
    if (arg == "loop") { OnLoopBox(nullptr, g_loop ? 0 : 1); if (g_loopBox) rp::host->ui->SetChecked(g_loopBox, g_loop); reply = Status(); return true; }
    if (arg == "playall") { SetAnimate(!g_animate); reply = Status(); return true; }
    if (arg.compare(0, 7, "search ") == 0) { if (g_search) rp::host->ui->SetEditText(g_search, arg.c_str() + 7); ShowSearch(arg.substr(7)); reply = Status(); return true; }
    if (arg.compare(0, 5, "drop ") == 0)      // drop <x> <y> <name>: the release of a drag at pixel (x, y) of the 3D view
    {
        int x = 0, y = 0, used = 0;
        if (sscanf(arg.c_str() + 5, "%d %d %n", &x, &y, &used) < 2 || !used) { reply = "fxpreview: drop <x> <y> <name>\n"; return true; }
        reply = DropOnCamera(CleanName(arg.substr(5 + size_t(used))), x, y) ? "fxpreview: dropped\n" : "fxpreview: drop refused\n";
        return true;
    }
    if (arg.compare(0, 6, "place ") == 0) { reply = PlaceAtCamera(CleanName(arg.substr(6))) ? "fxpreview: placed\n" : "fxpreview: FX Browser not found\n"; return true; }
    if (arg.compare(0, 5, "seek ") == 0)
    {
        std::string why;
        reply = Start(g_name, -atoi(arg.c_str() + 5), why) ? Status() : "fxpreview: " + why + "\n";
        return true;
    }
    // "fxpreview thumb <dir/name> [out.jpg]": a hidden-frame filmstrip (the cache path by default)
    if (arg.compare(0, 6, "thumb ") == 0)
    {
        char name[256] = {}, out[512] = {};
        if (sscanf_s(arg.c_str() + 6, "%255s %511s", name, unsigned(sizeof name), out, unsigned(sizeof out)) < 1) { reply = "fxpreview thumb <dir/name> [out.jpg|out.png]\n"; return true; }
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
        const Fit* fit = FitOf(clean);
        snprintf(buf, sizeof buf, "fxpreview thumb: %s%s (%llu ms, strip %.1f ms, fit %s centre %.2f %.2f size %.2f)\n", ok ? "written " : "failed: ", ok ? Utf8(path.c_str(), int(path.size())).c_str() : why.c_str(),
                 (unsigned long long)(GetTickCount64() - t0), g_stripUs / 1000.0, fit && fit->ok ? "ok" : "none", fit ? fit->cx : 0.0f, fit ? fit->cy : 0.0f, fit ? fit->size : 0.0f);
        reply = buf;
        return true;
    }
    if (arg.compare(0, 5, "play ") == 0)
    {
        const std::string name = CleanName(arg.substr(5));
        for (size_t r = 0; r < g_rows.size() && qt.setCurrentItem; ++r)     // as a click would: the tile selected
            if (FullName(g_all[size_t(g_rows[r])]) == name) { if (void* it = qt.item(g_grid, int(r))) qt.setCurrentItem(g_grid, it); break; }
        reply = (name == g_name && g_effect) || Preview(name) ? Status() : "fxpreview: " + g_lastStatus + "\n";
        return true;
    }
    reply = "fxpreview: status | play <dir/name> | seek <ms> | stop | pause | loop | playall | refresh | all | search <words> | place <dir/name> | thumb <dir/name> [out.jpg] | makeall | animate 0|1\n";
    return true;
}
}
