/* modtools_plugin.h: C interface of RadiantPlugins: a plugin exports ModToolsPluginEntry(self, host), called once on the main thread. */
/* Callbacks run on the main thread, host functions on any thread unless tagged MAIN THREAD. Tags: [all], [Radiant] (0/NULL elsewhere), [APE] (host->ape only). */
/* Tables only grow at the end (size, apiVersion), strings are UTF-8 or UTF-16 as declared, GUIDs "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}". See GETTING_STARTED.md. */
#ifndef MODTOOLS_PLUGIN_H
#define MODTOOLS_PLUGIN_H

#include <stdint.h>
#include <stddef.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODTOOLS_PLUGIN_API_VERSION 1

/* The programs the host runs in: host->App() returns one, ModToolsPluginApi::apps is a mask of them */
#define MODTOOLS_APP_RADIANT 1
#define MODTOOLS_APP_APE     2
#define MODTOOLS_APP_LAUNCHER 4

/* Views for RequestRedraw (a bitmask), matches ModToolsDrawContext::view */
#define MODTOOLS_VIEW_3D 1   /* the 3D camera view */
#define MODTOOLS_VIEW_2D 2   /* the 2D grid views (XY / XZ / YZ) */

#ifdef _WIN32
#define MODTOOLS_PLUGIN_EXPORT __declspec(dllexport)
#else
#define MODTOOLS_PLUGIN_EXPORT
#endif

typedef struct ModToolsPlugin ModToolsPlugin;   /* opaque: your handle, given to ModToolsPluginEntry */
typedef struct ModToolsApeApi ModToolsApeApi;   /* APE services, declared further down */
typedef void* ModToolsEntity;                  /* Radiant's entity_t*, opaque: only pass it back to the host (valid until the map changes) */
typedef void* ModToolsBrush;                   /* Radiant's brush_t*, opaque (a node of its owner's brush list) */
typedef void* ModToolsBrushInst;               /* a BrushInst: the selection list holds these, not brush_t */
typedef void* ModToolsWidget;                  /* QWidget*, QLayout*, QMenu*, QAction* or QObject* */
typedef void* ModToolsTreeItem;                /* QTreeWidgetItem* (not a QObject, owned by its tree) */
typedef void* ModToolsPane;                    /* a dock pane created with CreatePane */

/* ---- data carried by events -------------------------------------------------------------- */

typedef struct ModToolsCamParams
{
    float origin[3];
    float angles[3];      /* pitch, yaw, roll in degrees, positive pitch looks up, yaw 90 = +y */
    int   width, height;  /* pane size in pixels (0 until the first frame was presented) */
    int   gameView;       /* 1 = Radiant's "GAME VIEW" mode (16:9 letterbox) */
} ModToolsCamParams;

typedef struct ModToolsXYParams
{
    int   type;           /* 2 = XY (top), 1 = XZ (front), 0 = YZ (side) */
    float origin[3];      /* view centre */
    float scale;          /* pixels per unit */
} ModToolsXYParams;

typedef struct ModToolsDrawContext
{
    int              view;    /* 1 = the 3D camera view, 2 = a 2D view */
    int              width, height;
    ModToolsCamParams cam;     /* valid when view == 1 */
    ModToolsXYParams  xy;      /* valid when view == 2 */
} ModToolsDrawContext;

/* ---- optional native-material model preview ----------------------------------------------- */

/* Experimental (see SetNativeModelPreview): a deformation request for one selected model, applied when a draw belongs to its source mesh.
 * Coordinates are source-model space, before origin, angles and modelscale. `anchor` is the rest local-X minimum endpoint,
 * centred on the local Y/Z spans. `mode`: 0 = stretch, 1 = curve, 2 = skirt. */
typedef struct ModToolsNativeModelPreview
{
    uint32_t size;
    uint32_t enabled;
    float    anchor[3];
    float    axis[3];
    float    sideAxis[3];
    float    upAxis[3];
    float    restLength;
    float    minSide, maxSide;
    float    minUp, maxUp;
    float    scaleX, scaleY, scaleZ;
    int      anchorXAtMax, anchorYAtMax, anchorZAtMax;
    float    offsetX, offsetY, offsetZ;
    int      mode;
    float    curveSide, curveUp;
    float    skirtDepth, skirtFalloff;
    /* Optional source-local float3 positions for the host's draw identity check (copied, not retained).
     * sourcePositionStrideFloats is normally 3. */
    const float* sourcePositions;
    uint32_t sourcePositionCount;
    uint32_t sourcePositionStrideFloats;
    /* Non-zero: also draw the original model before the deformed one */
    uint32_t showOriginal;
    /* Curve lattice segments, 2 through 16 (0 = host default) */
    uint32_t curveSegments;
    /* GUID of this member of a preview set, used for per-request status (empty only with the single-preview API) */
    char     requestId[40];
} ModToolsNativeModelPreview;

/* A temporary world-space visibility envelope for a fixed-size misc_model, applied around the model-dispatch call only.
 * Editor-only, safe to publish from OnTick. */
typedef struct ModToolsNativeModelCullBounds
{
    ModToolsBrush brush;     /* the fixed-size model's brush_t */
    float mins[3];
    float maxs[3];
} ModToolsNativeModelCullBounds;

enum
{
    MODTOOLS_NATIVE_PREVIEW_UNAVAILABLE = 0,
    MODTOOLS_NATIVE_PREVIEW_WAITING = 1,
    MODTOOLS_NATIVE_PREVIEW_ACTIVE = 2,
};

typedef struct ModToolsSelectedBrush
{
    char  brushGuid[40];      /* GUID text of the brush (or the empty brush of a point entity) */
    char  entityGuid[40];     /* GUID text of the owning entity */
    float mins[3], maxs[3];
} ModToolsSelectedBrush;

typedef void (*ModToolsCallback)(void* user);
typedef void (*ModToolsEntityFn)(ModToolsEntity e, void* user);
typedef void (*ModToolsBrushFn)(ModToolsBrush b, void* user);
/* Qt event filter: watched widget + QEvent::Type (2 MouseButtonPress, 3 MouseButtonRelease, 6 KeyPress...). Return 1 to swallow. */
typedef int  (*ModToolsEventFilterFn)(void* user, ModToolsWidget watched, int eventType);
typedef void (*ModToolsValueFn)(void* user, int value);   /* host->OnChanged */

/* Hotkey modifiers for RegisterHotkey */
#define MODTOOLS_MOD_CTRL  1
#define MODTOOLS_MOD_SHIFT 2
#define MODTOOLS_MOD_ALT   4

/* ---- [all] Qt widget helpers (MAIN THREAD only) -------------------------------------------------- */
/* Wrappers over Radiant's Qt 5.3 (no Qt headers needed), widgets get Radiant's stylesheet, parents own children, label text is rich text.
 * Layouts go to a widget with SetLayout or to a box with BoxAddLayout. host->ui is NULL when Qt could not be bound (CreatePane fails too). */
typedef struct ModToolsUiApi
{
    uint32_t size;

    ModToolsWidget (*Widget)(ModToolsWidget parent);
    ModToolsWidget (*Label)(const char* text, ModToolsWidget parent);   /* rich text: <a href="https://..."> links open in the browser */
    ModToolsWidget (*LineEdit)(const char* text, ModToolsWidget parent);
    ModToolsWidget (*PushButton)(const char* text, ModToolsWidget parent);
    ModToolsWidget (*CheckBox)(const char* text, ModToolsWidget parent);
    ModToolsWidget (*RadioButton)(const char* text, ModToolsWidget parent);
    ModToolsWidget (*ListWidget)(ModToolsWidget parent);
    ModToolsWidget (*PlainTextEdit)(ModToolsWidget parent);

    ModToolsWidget (*VBox)(ModToolsWidget parentWidgetOrNull);
    ModToolsWidget (*HBox)(void);
    ModToolsWidget (*Grid)(void);
    void (*BoxAddWidget)(ModToolsWidget box, ModToolsWidget w, int stretch);
    void (*BoxAddLayout)(ModToolsWidget box, ModToolsWidget layout, int stretch);
    void (*BoxAddStretch)(ModToolsWidget box, int stretch);
    void (*GridAddWidget)(ModToolsWidget grid, ModToolsWidget w, int row, int col);
    void (*GridAddLayout)(ModToolsWidget grid, ModToolsWidget layout, int row, int col);
    void (*GridColumnStretch)(ModToolsWidget grid, int col, int stretch);
    void (*LayoutMargins)(ModToolsWidget layout, int l, int t, int r, int b);
    void (*LayoutSpacing)(ModToolsWidget layout, int s);
    void (*SetLayout)(ModToolsWidget widget, ModToolsWidget layout);

    void (*SetLabelText)(ModToolsWidget label, const char* text);
    void (*SetWordWrap)(ModToolsWidget label, int on);
    void (*SetButtonText)(ModToolsWidget button, const char* text);
    void (*SetEditText)(ModToolsWidget edit, const char* text);
    size_t (*EditText)(ModToolsWidget edit, char* out, size_t cap);       /* returns the length needed (without NUL) */
    void (*SetPlaceholder)(ModToolsWidget edit, const char* text);
    void (*SetChecked)(ModToolsWidget button, int on);
    int  (*IsChecked)(ModToolsWidget button);
    int  (*IsDown)(ModToolsWidget button);
    void (*SetEnabled)(ModToolsWidget widget, int on);
    void (*SetVisible)(ModToolsWidget widget, int on);
    void (*SetReadOnly)(ModToolsWidget plainTextEdit, int on);
    void (*ListClear)(ModToolsWidget list);
    void (*ListAdd)(ModToolsWidget list, const char* text);
    int  (*ListCount)(ModToolsWidget list);
    int  (*ListCurrentRow)(ModToolsWidget list);
    void (*ListSetCurrentRow)(ModToolsWidget list, int row);
    void (*ListSetItemText)(ModToolsWidget list, int row, const char* text);
    void (*ListSetItemCheckable)(ModToolsWidget list, int row, int checkable);   /* shows a check box on the row */
    void (*ListSetItemChecked)(ModToolsWidget list, int row, int checked);
    int  (*ListItemChecked)(ModToolsWidget list, int row);
    void (*ListSetItemToolTip)(ModToolsWidget list, int row, const char* text);
    void (*PlainSetText)(ModToolsWidget edit, const char* text);
    void (*PlainScrollToEnd)(ModToolsWidget edit);
    void (*SetMinimumHeight)(ModToolsWidget widget, int h);
    void (*SetMaximumHeight)(ModToolsWidget widget, int h);
    void (*SetMaximumWidth)(ModToolsWidget widget, int w);
    void (*SetMinimumSize)(ModToolsWidget widget, int w, int h);
    void (*SetToolTip)(ModToolsWidget widget, const char* text);
    void (*SetFocus)(ModToolsWidget widget);
    int  (*CursorInside)(ModToolsWidget widget);      /* the mouse cursor is over the widget's rectangle */
    void (*InstallFilter)(ModToolsWidget obj, ModToolsWidget filter);   /* filter from host->MakeEventFilter */
    void (*RemoveFilter)(ModToolsWidget obj, ModToolsWidget filter);
    void (*SetActionEnabled)(ModToolsWidget action, int on);
    void (*SetActionText)(ModToolsWidget action, const char* text);
    /* Modal colour dialog parented to `parent` (your pane content, or NULL), title NULL = default. Returns 1 and writes
     * the colour as 0xAARRGGBB, 0 when cancelled. Check ui->size before calling. */
    int  (*PickColor)(ModToolsWidget parent, uint32_t initialArgb, const char* title, uint32_t* outArgb);
    /* QLineEdit echo mode: 0 Normal, 1 NoEcho, 2 Password, 3 PasswordEchoOnEdit. Returns 0 when unavailable. Check ui->size. */
    int  (*SetEchoMode)(ModToolsWidget lineEdit, int mode);
    /* Panel widgets (check ui->size, rp::HasUi). Creators return NULL when Qt lacks an export. Report changes with host->OnChanged.
     * Orientation: horizontal 1, vertical 0. */
    ModToolsWidget (*Slider)(int horizontal, int minimum, int maximum, ModToolsWidget parent);
    void (*SliderSetRange)(ModToolsWidget slider, int minimum, int maximum);
    void (*SliderSetValue)(ModToolsWidget slider, int value);          /* emits OnChanged when the value changes */
    int  (*SliderValue)(ModToolsWidget slider);
    ModToolsWidget (*ComboBox)(ModToolsWidget parent);
    void (*ComboAdd)(ModToolsWidget combo, const char* text);
    void (*ComboClear)(ModToolsWidget combo);
    int  (*ComboCurrent)(ModToolsWidget combo);                        /* -1 when empty */
    void (*ComboSetCurrent)(ModToolsWidget combo, int index);
    ModToolsWidget (*TabWidget)(ModToolsWidget parent);
    int  (*TabAdd)(ModToolsWidget tabs, ModToolsWidget page, const char* title);   /* returns the page index */
    int  (*TabCurrent)(ModToolsWidget tabs);
    void (*TabSetCurrent)(ModToolsWidget tabs, int index);
    ModToolsWidget (*Splitter)(int horizontal, ModToolsWidget parent);   /* the user drags the handles between children */
    void (*SplitterAdd)(ModToolsWidget splitter, ModToolsWidget w, int stretch);
    /* Tree (header hidden). Items carry tab-separated column texts, an int64 id of your own and an optional check box in
     * column 0. TreeSetDragDrop lets the user reorder items: read the structure back with TreeChildCount/TreeChild
     * (item NULL = top level) when OnChanged reports 2. */
    ModToolsWidget (*TreeWidget)(int columns, ModToolsWidget parent);
    void (*TreeClear)(ModToolsWidget tree);
    ModToolsTreeItem (*TreeAdd)(ModToolsWidget tree, ModToolsTreeItem parentOrNull, const char* tabSeparatedColumns);
    void (*TreeRemove)(ModToolsWidget tree, ModToolsTreeItem item);     /* deletes the item and its children */
    int  (*TreeChildCount)(ModToolsWidget tree, ModToolsTreeItem itemOrNull);
    ModToolsTreeItem (*TreeChild)(ModToolsWidget tree, ModToolsTreeItem itemOrNull, int index);
    ModToolsTreeItem (*TreeParent)(ModToolsTreeItem item);               /* NULL for a top-level item */
    ModToolsTreeItem (*TreeCurrent)(ModToolsWidget tree);
    void (*TreeSetCurrent)(ModToolsWidget tree, ModToolsTreeItem item);
    size_t (*TreeItemText)(ModToolsTreeItem item, int column, char* out, size_t cap);   /* returns the length needed */
    void (*TreeSetItemText)(ModToolsTreeItem item, int column, const char* text);
    void (*TreeSetItemId)(ModToolsTreeItem item, int64_t id);
    int64_t (*TreeItemId)(ModToolsTreeItem item);                       /* 0 when never set */
    void (*TreeSetItemCheckable)(ModToolsTreeItem item, int on);
    void (*TreeSetItemChecked)(ModToolsTreeItem item, int on);
    int  (*TreeItemChecked)(ModToolsTreeItem item);
    void (*TreeSetExpanded)(ModToolsTreeItem item, int on);
    void (*TreeSetDragDrop)(ModToolsWidget tree, int on);
    /* The editor's own widgets (read them, do not delete them). FindEditorWidget: a QObject name anywhere under the main
     * window. FindChild: the index-th descendant (depth first) of class className or a subclass ("QSlider").
     * WidgetNumber: a slider, spin box, line edit, combo index or check box value, *ok = 0 when the widget has none. */
    ModToolsWidget (*FindEditorWidget)(const char* objectName);
    ModToolsWidget (*FindChild)(ModToolsWidget parent, const char* className, int index);
    double (*WidgetNumber)(ModToolsWidget w, int* ok);
    /* Frameless vertical scroll area around content */
    ModToolsWidget (*ScrollArea)(ModToolsWidget content, ModToolsWidget parent);
    /* QAbstractButton::click, exactly a user's click, also on the editor's own buttons */
    void (*ClickButton)(ModToolsWidget button);
    /* Make a menu action (AddMenuItem's return) checkable and set its check mark (check ui->size). Its callback fires on every click. */
    void (*SetActionChecked)(ModToolsWidget action, int checkable, int on);
} ModToolsUiApi;

/* Aggregate main-thread timing counters for profiling, in QueryPerformanceCounter ticks. Optional, check host->size
 * before calling PerformanceProfile. */
typedef struct ModToolsPerfCounter
{
    uint64_t calls;
    uint64_t totalTicks;
    uint64_t minTicks;
    uint64_t maxTicks;
} ModToolsPerfCounter;

enum ModToolsHostPerfMetric
{
    MODTOOLS_HOST_PERF_ROUTINE_HOOK = 0,
    MODTOOLS_HOST_PERF_RADIANT_ROUTINE_PROCESSING,
    MODTOOLS_HOST_PERF_PLUGINS_ON_TICK,
    MODTOOLS_HOST_PERF_CAM_DRAW,
    MODTOOLS_HOST_PERF_DRAW_HUD,
    MODTOOLS_HOST_PERF_XY_DRAW,
    MODTOOLS_HOST_PERF_METRIC_COUNT
};

enum ModToolsHostPerfAction
{
    MODTOOLS_HOST_PERF_RESET = 0,
    MODTOOLS_HOST_PERF_START,
    MODTOOLS_HOST_PERF_STOP,
    MODTOOLS_HOST_PERF_SNAPSHOT
};

typedef struct ModToolsHostPerfSnapshot
{
    uint32_t size;
    uint32_t active;
    uint64_t qpcFrequency;
    uint64_t startTick;
    uint64_t endTick;
    ModToolsPerfCounter metrics[MODTOOLS_HOST_PERF_METRIC_COUNT];
    uint64_t xyFrames[3];       /* XY, XZ, YZ (CXYWnd view types 2, 1, 0) */
} ModToolsHostPerfSnapshot;

/* One patch control vertex for ModToolsHostApi::PatchSelectVerts: brush GUID text, block (row) and point (col) index. */
typedef struct ModToolsPatchVert
{
    const char* brushGuid;
    int row;
    int col;
} ModToolsPatchVert;

/* In-memory replacement for the LUT texture used by Radiant's camera preview. Pixel data is
 * tightly packed RGB uint16 values in row-major order. The host copies the buffers during the call.
 * Pass referenceRgb only when setting or changing the camera's native base texture. Pass NULL on later
 * wheel updates to keep the prior reference. */
typedef struct ModToolsCameraLutPreview
{
    uint32_t size;
    uint32_t width;
    uint32_t height;
    const uint16_t* referenceRgb;
    const uint16_t* previewRgb;
    int enabled;
} ModToolsCameraLutPreview;

enum
{
    MODTOOLS_CAMERA_LUT_PREVIEW_OFF = 0,
    MODTOOLS_CAMERA_LUT_PREVIEW_WAITING = 1,
    MODTOOLS_CAMERA_LUT_PREVIEW_ACTIVE = 2,
    MODTOOLS_CAMERA_LUT_PREVIEW_UNSUPPORTED = 3,
    MODTOOLS_CAMERA_LUT_PREVIEW_NO_MATCH = 4,
    MODTOOLS_CAMERA_LUT_PREVIEW_MATCHED = 5
};

/* ---- host services: the first argument is always your plugin handle ------------------------ */
typedef struct ModToolsHostApi
{
    uint32_t size;
    uint32_t apiVersion;       /* MODTOOLS_PLUGIN_API_VERSION of the host */
    uint32_t hostVersion;      /* host build: major << 16 | minor << 8 | patch */

    /* --- [all] where things are --- */
    const wchar_t* (*PluginDir)(ModToolsPlugin* p);      /* folder of your DLL, keep your ini/data there */
    const wchar_t* (*PluginsRoot)(ModToolsPlugin* p);    /* the plugins folder */
    const wchar_t* (*RadiantRoot)(ModToolsPlugin* p);    /* the Black Ops III install (bin\Radiant_modtools.exe and bin\asseteditor_modtools.exe live under it) */

    /* --- [all] log: one shared RadiantPlugins.log, every line prefixed with your plugin name --- */
    void (*Log)(ModToolsPlugin* p, const char* fmt, ...);
    void (*LogW)(ModToolsPlugin* p, const wchar_t* fmt, ...);

    /* --- [all] config: <PluginDir>\<Name>.ini, sections/keys like a Windows ini file. UTF-8 in and out. --- */
    size_t (*ConfigGet)(ModToolsPlugin* p, const char* section, const char* key, const char* def, char* out, size_t cap);   /* returns the length needed */
    int    (*ConfigGetInt)(ModToolsPlugin* p, const char* section, const char* key, int def);
    void   (*ConfigSet)(ModToolsPlugin* p, const char* section, const char* key, const char* value);   /* NULL value deletes the key */

    /* --- [all] threads --- */
    void  (*PostToMainThread)(ModToolsPlugin* p, ModToolsCallback fn, void* user);   /* runs on the next idle tick, dropped if you unload first */
    int   (*IsMainThread)(ModToolsPlugin* p);
    void* (*MainWindow)(ModToolsPlugin* p);              /* HWND of the application's main window (may change during startup) */

    /* --- [Radiant] the open map --- */
    size_t (*CurrentMapPath)(ModToolsPlugin* p, wchar_t* out, size_t cap);   /* full path of the root map, 0 = none loaded yet */
    int  (*PrefabDepth)(ModToolsPlugin* p);              /* 0 = editing the map, >0 = inside a prefab (Enter Prefab) */
    int  (*IsMapModified)(ModToolsPlugin* p);            /* Radiant's unsaved-changes flag */
    int  (*ReloadMap)(ModToolsPlugin* p, const wchar_t* path, int keepCamera);   /* MAIN THREAD: load a map like File > Open without any prompt (unsaved changes are lost), refuses inside a prefab */
    int  (*IsReloading)(ModToolsPlugin* p);
    /* Bracket your own writes to map files with these so a file watcher can tell them from external edits. */
    void (*BeginOwnWrite)(ModToolsPlugin* p);
    void (*EndOwnWrite)(ModToolsPlugin* p);
    int  (*OwnWriteRecent)(ModToolsPlugin* p);           /* a bracketed write is in progress or ended less than 1.5 s ago */
    /* Raise this to keep the user from being asked about external map changes. A plugin that would ask checks it. */
    void (*InhibitReloadPrompts)(ModToolsPlugin* p, int on);
    int  (*ReloadPromptsInhibited)(ModToolsPlugin* p);

    /* --- [Radiant] camera --- */
    int  (*GetCamera)(ModToolsPlugin* p, float origin[3], float angles[3]);
    int  (*SetCamera)(ModToolsPlugin* p, const float origin[3], const float angles[3]);   /* MAIN THREAD */
    int  (*GameView)(ModToolsPlugin* p);
    void (*SetGameView)(ModToolsPlugin* p, int on);
    int  (*LastCamParams)(ModToolsPlugin* p, ModToolsCamParams* out);   /* parameters of the last 3D frame (pane size included) */
    void* (*CamWindow)(ModToolsPlugin* p);               /* HWND of the 3D view's swap chain, NULL before the first frame */

    /* --- [Radiant] entities and brushes (MAIN THREAD only, pointers are Radiant's own objects) --- */
    void (*ForEachEntity)(ModToolsPlugin* p, ModToolsEntityFn fn, void* user);               /* global list order, worldspawn first */
    void (*ForEachEntityBrush)(ModToolsPlugin* p, ModToolsEntity e, ModToolsBrushFn fn, void* user);   /* selected brushes are unlinked from this list */
    int  (*EntityGuid)(ModToolsPlugin* p, ModToolsEntity e, char out[40]);
    int  (*BrushGuid)(ModToolsPlugin* p, ModToolsBrush b, char out[40]);
    ModToolsEntity (*BrushOwner)(ModToolsPlugin* p, ModToolsBrush b);
    ModToolsEntity (*WorldEntity)(ModToolsPlugin* p);                                         /* worldspawn, NULL before a map is loaded */
    size_t (*GetSelection)(ModToolsPlugin* p, ModToolsSelectedBrush* out, size_t cap);        /* returns the count (may exceed cap) */
    /* Serialize one entity with Radiant's own .map writer ("{ guid ... }" + its brushes). Free with FreeString. */
    char* (*WriteEntityText)(ModToolsPlugin* p, ModToolsEntity e);
    void  (*FreeString)(ModToolsPlugin* p, char* s);
    /* In-place editing through Radiant's paste/delete pipeline (undoable, shows as `undoLabel` in Radiant's undo). */
    int  (*ImportMapText)(ModToolsPlugin* p, const char* mapText, const char* undoLabel);   /* clipboard format, worldspawn block first, leaves the objects selected, keeps target/linkto (Radiant's own paste drops them) */
    int  (*DeleteBrushes)(ModToolsPlugin* p, const ModToolsBrush* brushes, size_t n, const char* undoLabel);
    size_t (*SelectedBrushInsts)(ModToolsPlugin* p, ModToolsBrushInst* out, size_t cap);      /* the selection as instance pointers, to restore it later */
    void (*SelectBrushInsts)(ModToolsPlugin* p, const ModToolsBrushInst* insts, size_t n);
    void (*DeselectAll)(ModToolsPlugin* p);
    int  (*RestampImportedGuids)(ModToolsPlugin* p, const char* entityGuid, const char* const* brushGuids, size_t n);   /* after ImportMapText: write these GUIDs over the fresh ones */
    int  (*SelectEntityByGuid)(ModToolsPlugin* p, const char* entityGuid);
    int  (*EnterSelectedPrefab)(ModToolsPlugin* p);      /* Radiant's "Enter Prefab" on the selection */
    int  (*LeavePrefab)(ModToolsPlugin* p);

    /* --- [Radiant] overlay drawing: only from OnDraw, into the view named by the ModToolsDrawContext --- */
    int  (*Project)(ModToolsPlugin* p, const float world[3], float* sx, float* sy);   /* world -> pixels, 0 when behind the camera */
    void (*DrawWorldLine)(ModToolsPlugin* p, const float a[3], const float b[3], uint32_t argb);
    void (*DrawScreenLine)(ModToolsPlugin* p, float x0, float y0, float x1, float y1, uint32_t argb);
    /* Text with its top-left at pixel (x, y), px 2 = normal size. 3D view: Radiant's HUD font on a dark box. 2D views, or
     * [render] engine_text=0 in RadiantPlugins.ini: the host's 4x6 stroke font (upper case only). MeasureLabel gives the size. */
    void (*DrawLabel)(ModToolsPlugin* p, float x, float y, const char* text, uint32_t argb, float px);

    /* --- [all] UI (MAIN THREAD) --- */
    const ModToolsUiApi* ui;                             /* widget helpers, NULL if Qt is unavailable */
    /* A dock pane around your content widget (a real pane in Radiant, a dock of the main window in APE and the launcher).
     * Place and hidden state are restored from the saved layout. One pane per title, creating it again replaces the content. */
    ModToolsPane   (*CreatePane)(ModToolsPlugin* p, const char* title, ModToolsWidget content, int w, int h);
    int           (*PaneVisible)(ModToolsPlugin* p, ModToolsPane pane);
    void          (*ShowPane)(ModToolsPlugin* p, ModToolsPane pane, int show);
    ModToolsWidget (*PaneToggleAction)(ModToolsPlugin* p, ModToolsPane pane);   /* its checkable QAction, for a menu */
    ModToolsWidget (*AddMenu)(ModToolsPlugin* p, const char* title);           /* top-level menu in the application's menu bar (reused by title) */
    ModToolsWidget (*AddMenuItem)(ModToolsPlugin* p, ModToolsWidget menu, const char* text, ModToolsCallback fn, void* user);
    void          (*AddMenuAction)(ModToolsPlugin* p, ModToolsWidget menu, ModToolsWidget action);
    void          (*AddMenuSeparator)(ModToolsPlugin* p, ModToolsWidget menu);
    int           (*RegisterHotkey)(ModToolsPlugin* p, int vk, int modifiers, ModToolsCallback fn, void* user);   /* fires while the application is in the foreground */
    ModToolsWidget (*MakeEventFilter)(ModToolsPlugin* p, ModToolsEventFilterFn fn, void* user);   /* install with ui->InstallFilter */
    int           (*OnClicked)(ModToolsPlugin* p, ModToolsWidget button, ModToolsCallback fn, void* user);   /* QAbstractButton::clicked */

    /* --- [all] other plugins --- */
    int         (*PluginLoaded)(ModToolsPlugin* p, const char* name);
    const char* (*Command)(ModToolsPlugin* p, const char* pluginName, const char* cmd);   /* that plugin's OnCommand, NULL if absent */

    /* --- raw access for plugins that know the binary --- */
    /* [Radiant] IDA address of Radiant_modtools.exe -> live address, NULL when the binary does not match (APE: ape->ApeAddress) */
    void* (*RadiantAddress)(ModToolsPlugin* p, uintptr_t idaAddress);
    int   (*Detour)(ModToolsPlugin* p, void* target, void* detour, void** original);   /* [all] MinHook, removed when you unload */

    /* --- later additions (check rp::Has or `size` before calling) --- */
    /* [Radiant] MAIN THREAD: the BrushInst of a brush_t (what SelectBrushInsts wants), NULL if the brush has no instance. */
    ModToolsBrushInst (*BrushInstOf)(ModToolsPlugin* p, ModToolsBrush b);

    /* --- [all] which application --- */
    int (*App)(ModToolsPlugin* p);                       /* MODTOOLS_APP_RADIANT, MODTOOLS_APP_APE or MODTOOLS_APP_LAUNCHER */
    const ModToolsApeApi* ape;                           /* APE services, NULL inside Radiant and the launcher */
    /* [all] MAIN THREAD: a top-level dialog window around your content widget, parented to the main window, hidden until
     * you ui->SetVisible it. Closing hides it, the host destroys it on unload. Use it for settings screens, CreatePane for tool panes. */
    ModToolsWidget (*CreateDialogWindow)(ModToolsPlugin* p, const char* title, ModToolsWidget content, int w, int h);   /* "CreateDialog" is a Win32 macro */
    /* [all] MAIN THREAD. The application's own menu actions by label ("Edit/Undo"), ignoring '&', a trailing "..." and case.
     * TriggerMenuAction: 1 = triggered, 0 = no such entry, -1 = found but disabled. MenuEntries lists a menu ("" = the menu
     * bar), one "label<TAB>enabled<TAB>has_submenu" line per entry ("---" = separator), returns the length needed. */
    int    (*TriggerMenuAction)(ModToolsPlugin* p, const char* path);
    size_t (*MenuEntries)(ModToolsPlugin* p, const char* path, char* out, size_t cap);
    /* [Radiant] MAIN THREAD: one undo operation across several DeleteBrushes/ImportMapText calls, so a rebuild is a single Ctrl+Z.
     * Always pair them, an operation left open swallows the user's next edit. */
    void   (*BeginUndo)(ModToolsPlugin* p, const char* label);
    void   (*EndUndo)(ModToolsPlugin* p);
    /* [Radiant] width and line height in pixels of DrawLabel(text, px) in the view being drawn (inside OnDraw) or the 3D view.
     * Returns 1 for Radiant's HUD font, 0 for the stroke font. */
    int    (*MeasureLabel)(ModToolsPlugin* p, const char* text, float px, float* width, float* height);
    /* [Radiant] MAIN THREAD: redraw the given views (MODTOOLS_VIEW_* bitmask) on the next idle tick. The 2D views only redraw when
     * dirty, so an animated 2D overlay must call this each tick (from OnTick). No-op in APE. CamXYUpdateOnDrag returns Radiant's
     * "Update XY on drag" preference (Preferences > Camera), respect it before forcing 2D redraws. */
    void   (*RequestRedraw)(ModToolsPlugin* p, unsigned views);
    int    (*CamXYUpdateOnDrag)(ModToolsPlugin* p);
    /* [Radiant] MAIN THREAD: move geometry in place like Radiant's own drag, for previews (no undo record, selection or modified flag).
     * MoveBrush adds delta to one brush and, for a fixed-size entity, to its owner's origin. FindBrushesByGuid: GUIDs -> handles
     * in one walk (NULL when absent or selected by the user), returns the number found. BrushBounds copies mins/maxs. */
    int    (*MoveBrush)(ModToolsPlugin* p, ModToolsBrush b, const float delta[3]);
    size_t (*FindBrushesByGuid)(ModToolsPlugin* p, const char* const* guids, size_t n, ModToolsBrush* out);
    int    (*BrushBounds)(ModToolsPlugin* p, ModToolsBrush b, float mins[3], float maxs[3]);
    /* [Radiant] MAIN THREAD: entity GUIDs -> entity handles in one walk (out[i] = NULL when absent) */
    size_t (*FindEntitiesByGuid)(ModToolsPlugin* p, const char* const* guids, size_t n, ModToolsEntity* out);
    /* [Radiant] MAIN THREAD: a classic brush as three plane points per face (9 floats, face order), for resize previews. BrushPlanes returns
     * the face count (0 for mesh/curve brushes, may exceed capFaces), SetBrushPlanes rebuilds like MoveBrush. EntityFixedSize: point entity,
     * model or prefab. BrushOfInst: brush behind a selection instance. WriteEntityTextWith links the given selected brushes back in (FreeString). */
    size_t (*BrushPlanes)(ModToolsPlugin* p, ModToolsBrush b, float* out, size_t capFaces);
    int    (*SetBrushPlanes)(ModToolsPlugin* p, ModToolsBrush b, const float* pts, size_t nFaces);
    int    (*EntityFixedSize)(ModToolsPlugin* p, ModToolsEntity e);
    ModToolsBrush (*BrushOfInst)(ModToolsPlugin* p, ModToolsBrushInst inst);
    char*  (*WriteEntityTextWith)(ModToolsPlugin* p, ModToolsEntity e, const ModToolsBrush* brushes, size_t n);
    /* [Radiant] MAIN THREAD. Advanced, for plugins that apply other people's edits. Between BeginUntracked and EndUntracked (paired,
     * one callback) ImportMapText and DeleteBrushes record no undo. Get/SetUndoTags copy an object's undo tags (entity: undoId,
     * redoId, entityId, brush: undoId, redoId, 0): put the old object's tags on its untracked replacement so earlier undo steps still apply. */
    void   (*BeginUntracked)(ModToolsPlugin* p);
    void   (*EndUntracked)(ModToolsPlugin* p);
    int    (*GetUndoTags)(ModToolsPlugin* p, void* object, int isEntity, int tags[3]);
    int    (*SetUndoTags)(ModToolsPlugin* p, void* object, int isEntity, const int tags[3]);
    /* [Radiant] MAIN THREAD: inside a prefab, the "origin" and "angles" keys (missing = 0) of the misc_prefab entity entered at `level`
     * (1 = first Enter Prefab, up to PrefabDepth()). A point p in the prefab's own space sits at origin + R(angles) * p outside it.
     * 0 when there is no such level. Do not WriteEntityText an outer-scope entity while inside a prefab (Radiant's writer crashes). */
    int (*PrefabStackPlacement)(ModToolsPlugin* p, int level, float origin[3], float angles[3]);
    /* [all] MAIN THREAD: fn(user, value) when a widget's value changes. Slider: value, combo box: index, tab widget: page, list: row,
     * check box / radio / checkable button: 0 or 1, line edit: editingFinished (0), tree: 0 current item changed, 1 text or check box
     * changed, 2 items added, removed or moved. Returns 1 when connected, 0 for other widget classes. */
    int (*OnChanged)(ModToolsPlugin* p, ModToolsWidget w, ModToolsValueFn fn, void* user);
    /* [Radiant] MAIN THREAD: timing of the host's hooks on the idle tick, 3D draw, HUD draw and 2D draw. Inactive until START, never logs per call. */
    int (*PerformanceProfile)(ModToolsPlugin* p, int action, ModToolsHostPerfSnapshot* out);
    /* [Radiant] HWND of a 2D view's swap chain (2 = XY top, 1 = XZ front, 0 = YZ side), NULL until it has drawn once. Any thread. */
    void* (*XYWindow)(ModToolsPlugin* p, int type);
    /* [Radiant] MAIN THREAD: raise the unsaved-changes flag. MoveBrush and SetBrushPlanes leave it down, call this after a real edit. */
    void (*MarkMapModified)(ModToolsPlugin* p);
    /* [Radiant] MAIN THREAD: patch vertices via Drag Vertices (undo, redo and inserts re-create patches). PatchGrid: control grid by GUID, up to cap xyz triples in row*cols+col order (xyz may be NULL), returns rows*cols (0 = not a patch).
     * PatchSelectVerts: replace the selection (0 clears, < 0 counts), returns the count or -1 bad/over 1024, -2 not selected, -3 out of range.
     * PatchWeldVerts: move the selected vertices to xyz (one undo step), returns the number moved (0 none, -1 no patch). DragMode: 4 = vertices. */
    int (*PatchGrid)(ModToolsPlugin* p, const char* brushGuid, int* rows, int* cols, float* xyz, int cap);
    int (*PatchSelectVerts)(ModToolsPlugin* p, const ModToolsPatchVert* verts, int count);
    int (*PatchWeldVerts)(ModToolsPlugin* p, const float xyz[3]);
    int (*DragMode)(ModToolsPlugin* p);
    /* [Radiant] MAIN THREAD: RestampImportedGuids for one ImportMapText of n entity blocks (after the worldspawn block). The last n entities
     * of the global list, in import order, get entityGuids[i], entity i's imported brushes get the next brushCounts[i] texts of brushGuids.
     * Returns n, or 0 when the list is shorter. */
    size_t (*RestampImportedEntities)(ModToolsPlugin* p, const char* const* entityGuids, size_t n, const char* const* brushGuids, const size_t* brushCounts);
    /* [Radiant] only from OnDraw: an alpha-blended triangle in the overlay pass (a preview primitive, not Radiant's material renderer) */
    void (*DrawWorldTriangle)(ModToolsPlugin* p, const float a[3], const float b[3], const float c[3], uint32_t argb);
    /* [Radiant] MAIN THREAD. Advanced and experimental, built for a model-bending tool: SetNativeModelPreview through
     * SetNativeModelCullBounds draw a deformed copy of the one selected model with its real materials, editor-only. Most plugins want
     * DrawWorldLine/DrawWorldTriangle. NULL clears the request. */
    void (*SetNativeModelPreview)(ModToolsPlugin* p, const ModToolsNativeModelPreview* preview);
    /* [Radiant] UNAVAILABLE without a usable D3D adapter, WAITING until a matching draw was seen, ACTIVE while one is rendered */
    int  (*NativeModelPreviewStatus)(ModToolsPlugin* p);
    /* [Radiant] verified native draw substitutions in the current camera frame (0 when none) */
    unsigned (*NativeModelPreviewDrawCount)(ModToolsPlugin* p);
    /* [Radiant] MAIN THREAD: replaces the set of preview requests, each with a unique requestId. NULL or zero clears it. */
    void (*SetNativeModelPreviewSet)(ModToolsPlugin* p, const ModToolsNativeModelPreview* previews, size_t count);
    /* [Radiant] per-request status and draw count for the current camera frame */
    int  (*NativeModelPreviewStatusForId)(ModToolsPlugin* p, const char* requestId);
    unsigned (*NativeModelPreviewDrawCountForId)(ModToolsPlugin* p, const char* requestId);
    /* [Radiant] MAIN THREAD: replaces the temporary model visibility envelopes (copied). NULL or zero clears them. */
    void (*SetNativeModelCullBounds)(ModToolsPlugin* p, const ModToolsNativeModelCullBounds* bounds, size_t count);
    /* [Radiant] MAIN THREAD (check rp::Has): move your closed pane into the tab group of Radiant's pane titled `beside` ("Camera", "Entity Info")
     * and raise it (returns 1), or raise one that is already open behind another tab (2). 0 = left alone, `beside` is missing or hidden. */
    int  (*TabPaneWith)(ModToolsPlugin* p, ModToolsPane pane, const char* beside);
    /* [Radiant] MAIN THREAD (check rp::Has): replace a patch's control grid by GUID, resizing its control net if needed.
     * xyz holds exactly rows*cols triples (both odd, 3 or more). No undo record: wrap it in BeginUndo/EndUndo.
     * Returns 1 on success, 0 for a missing patch or an unsafe grid. */
    int  (*PatchSetGrid)(ModToolsPlugin* p, const char* brushGuid, int rows, int cols, const float* xyz, int count);
    /* [Radiant] MAIN THREAD (check rp::Has): commit a brush translation (delta) or a classic-brush plane set (pts/nFaces, wins over delta)
     * as one undoable edit that marks the map modified, joining an open undo step. */
    int  (*CommitBrushGeometry)(ModToolsPlugin* p, ModToolsBrush brush, const float* pts, size_t nFaces, const float delta[3], const char* undoLabel);
    /* [Radiant] MAIN THREAD (check rp::Has): replace the matching camera LUT texture in memory only; the host copies both buffers. */
    void (*SetCameraLutPreview)(ModToolsPlugin* p, const ModToolsCameraLutPreview* preview);
    /* [Radiant] (check rp::Has): OFF, WAITING, ACTIVE, UNSUPPORTED, NO_MATCH or MATCHED for the selected camera LUT. */
    int  (*CameraLutPreviewStatus)(ModToolsPlugin* p);
} ModToolsHostApi;

/* ---- [APE] Asset Property Editor services: host->ape (NULL in Radiant and the launcher), MAIN THREAD unless noted ------------- */
/* An asset is identified by its name and type (the GDF name, e.g. "xmodel", "material"). Strings are UTF-8.
 * Functions that copy text out return the length needed without the NUL, like ConfigGet. */
typedef struct ModToolsApeAsset
{
    char name[256];
    char type[64];
} ModToolsApeAsset;

typedef struct ModToolsApeAssetInfo
{
    wchar_t gdtPath[520];     /* the .gdt file that defines the asset (as gdt.db spells it, lower case) */
    int     line;             /* 1-based line of the asset's header in that file */
    char    parentName[256];  /* empty when the asset is not derived */
    char    parentType[64];
} ModToolsApeAssetInfo;

typedef struct ModToolsApeApi
{
    uint32_t size;
    /* The asset shown in the current editor tab (0 = no asset tab is current). */
    int    (*CurrentAsset)(ModToolsPlugin* p, ModToolsApeAsset* out);
    /* The assets selected in the asset tree (GDT rows are skipped). Returns the count, which may exceed cap. */
    size_t (*SelectedAssets)(ModToolsPlugin* p, ModToolsApeAsset* out, size_t cap);
    /* Number of open editor tabs and the asset of tab `index` (0 when that tab holds no asset). */
    int    (*TabCount)(ModToolsPlugin* p);
    int    (*TabAsset)(ModToolsPlugin* p, int index, ModToolsApeAsset* out);
    /* Append a line to APE's Output pane (any thread, queued to the main thread). */
    void   (*Print)(ModToolsPlugin* p, const char* text);
    /* Full path of APE's asset database (<root>\gdtdb\gdt.db). */
    size_t (*GdtDbPath)(ModToolsPlugin* p, wchar_t* out, size_t cap);
    /* Every type an asset of that name exists as, comma separated (names are unique per type). Read from gdt.db, so unsaved new
     * assets are not known. Returns the number of types. Any thread. */
    size_t (*AssetTypes)(ModToolsPlugin* p, const char* name, char* out, size_t cap);
    /* Raw access for plugins that know the binary: IDA address -> live address (APE is relocated by ASLR), NULL when the build does not match. */
    void*  (*ApeAddress)(ModToolsPlugin* p, uintptr_t idaAddress);
    /* Search gdt.db: assets whose name contains `namePart` (case-insensitive, NULL/empty = any) of type `type` (NULL/empty = any).
     * Writes at most `cap` entries in the database's order and returns how many. Any thread. */
    size_t (*FindAssets)(ModToolsPlugin* p, const char* namePart, const char* type, ModToolsApeAsset* out, size_t cap);
    /* Every asset type gdt.db knows (the GDF names FindAssets takes), comma separated. Returns the count, or the length needed when `out` is too small. Any thread. */
    size_t (*AssetTypeNames)(ModToolsPlugin* p, char* out, size_t cap);
    /* Where an asset lives, from gdt.db: its .gdt file and line, and its parent when it is derived ("child" [ "parent" ] in the .gdt).
     * `type` may be empty (first match). Returns 0 when unknown. Any thread. */
    int    (*AssetInfo)(ModToolsPlugin* p, const char* name, const char* type, ModToolsApeAssetInfo* out);
    /* The property schema of a type from gdt.db (_meta): one "key\ttype\tdefault\tenums\n" line per property. Length needed like ConfigGet. Any thread. */
    size_t (*TypeProperties)(ModToolsPlugin* p, const char* type, char* out, size_t cap);
    /* The plain text of APE's Output pane (its errors and warnings). MAIN THREAD. Returns the length needed. */
    size_t (*OutputText)(ModToolsPlugin* p, char* out, size_t cap);
    /* The current tab's property grid, live, one line per entry "index<TAB>category<TAB>title<TAB>kind<TAB>value<TAB>tooltip<TAB>visible<TAB>enabled\n"
     * (kind = the ape*Widget class, multi-component values are space separated). MAIN THREAD. Length needed like ConfigGet. */
    size_t (*Properties)(ModToolsPlugin* p, char* out, size_t cap);
    /* Write one property through its control, so APE treats it as the user's edit (override state, dirty tab, undo).
     * `titleOrIndex` = the title, "category/title" or the index from Properties. MAIN THREAD. Returns 1, or 0 with the reason in `why`. */
    int    (*SetProperty)(ModToolsPlugin* p, const char* titleOrIndex, const char* value, char* why, size_t whyCap);
    /* Open an asset in the current tab (the tab's asset line edit + Enter, i.e. following an asset link). MAIN THREAD. */
    int    (*OpenAsset)(ModToolsPlugin* p, const char* name, const char* type, char* why, size_t whyCap);
} ModToolsApeApi;

/* ---- what a plugin gives the host --------------------------------------------------------- */
typedef struct ModToolsPluginApi
{
    uint32_t size;
    uint32_t apiVersion;       /* MODTOOLS_PLUGIN_API_VERSION the plugin was built against */
    const char* name;          /* identifier: letters, digits, underscore, must equal the DLL name */
    const char* displayName;
    const char* version;       /* free text, e.g. "1.0.0" */
    const char* author;
    const char* description;   /* one or two sentences for the Plugins window */

    /* all callbacks run on the application's main thread, any may be NULL. Tags as in the index at the top. */
    int  (*OnLoad)(void);                                   /* [all] return 0 to refuse loading (say why with Log) */
    void (*OnUnload)(void);                                 /* [all] stop threads, forget host objects (panes/menus are cleaned by the host) */
    void (*OnTick)(void);                                   /* [all] the main thread's idle tick, continuous (hundreds of times per second) */
    void (*OnMapLoaded)(const wchar_t* path);               /* [Radiant] a map was loaded (also replayed once when the plugin loads after the map) */
    void (*OnMapSaved)(const wchar_t* path, int autosave, int selectedOnly);   /* [Radiant] */
    void (*OnBeforeScopeChange)(void);                      /* [Radiant] Enter/Leave Prefab is about to swap the entity list */
    void (*OnScopeChanged)(const wchar_t* path, int depth); /* [Radiant] the file Radiant edits now (prefab or root map) */
    void (*OnDraw)(const ModToolsDrawContext* ctx);          /* [Radiant] a view is being presented: draw with host->Draw* */
    const char* (*OnCommand)(const char* cmd);              /* [all] test/automation channel, return a static reply or NULL for "not mine" */
    void (*OnSettings)(void);                               /* [all] the Plugins window's Settings button (e.g. show your pane) */

        uint32_t apps;                                          /* MODTOOLS_APP_* mask: where this plugin runs (0 = Radiant only) */
    /* [APE] the current editor tab changed (name/type empty when no asset is shown) */
    void (*OnAssetChanged)(const char* name, const char* type);

    const char* url;                                        /* project page, e.g. "https://github.com/you/MyTool", http(s) only, may be NULL */

    /* [Radiant] a root map load is about to start (File > Open, reloads, the command-line map, which loads before the main
     * window shows). OnMapLoaded follows when it is done. */
    void (*OnMapLoading)(const wchar_t* path);
} ModToolsPluginApi;

typedef const ModToolsPluginApi* (*ModToolsPluginEntryFn)(ModToolsPlugin* self, const ModToolsHostApi* host);

#ifdef __cplusplus
}
#endif
#endif /* MODTOOLS_PLUGIN_H */
