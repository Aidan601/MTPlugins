// PlayTest: play the open map in Black Ops III from Radiant and see brush edits live in the running game. Radiant only.
// Builds only what changed on a worker thread, docks the game in a "BO3 Live" pane, Live Edit goes through BO3LiveBridge.dll.
#define MODTOOLS_PLUGIN_IMPL
#include "modtools_plugin.hpp"
#include "BO3LiveShared.h"
#include "ffread.h"
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <commctrl.h>      // TaskDialogIndirect (Live Edit needs BO3 Enhanced box)
#include <objbase.h>
#include <mmdeviceapi.h>    // mutes the parked BO3 (Core Audio)
#include <audiopolicy.h>
#undef interface        // objbase.h macros clash with names in this file
#undef small
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
// Radiant has no Common Controls 6 manifest: TaskDialogIndirect uses an activation context from this DLL's manifest (resource 2).
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <cctype>
#include <utility>
#include <map>
#include <set>
#include <unordered_map>
#include <cmath>
#include "../common/physprim.h"

namespace
{
    bool PlayRadiantForeground()
    {
        HWND fg = GetForegroundWindow();
        if (!fg) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        return pid == GetCurrentProcessId();
    }

    // ---- Qt action lookup (no headers, MS x64 ABI: QList/QString returned through a hidden pointer, QList<T*> keeps the pointers in
    // QListData::Data::array) ----
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
        activate(hit, 0);   // QAction::Trigger, like the user's click
        return true;
    }



    // ---- Play Test (BO3 test launcher) ----
    // Native Mod Launcher command sequence on a worker thread, started from a Radiant Qt dock button.
    enum PlayState
    {
        PLAY_IDLE = 0,
        PLAY_SAVING,
        PLAY_PREPARING,
        PLAY_COMPILING,
        PLAY_LINKING,
        PLAY_LAUNCHING,
        PLAY_RUNNING,
        PLAY_FAILED,
        PLAY_STOPPED
    };

    struct PlayMapInfo
    {
        std::wstring gameRoot;
        std::wstring toolsRoot;
        std::wstring mapPath;
        std::wstring mapName;
        std::wstring prefix;
        std::wstring bspPath;
        std::wstring usermapRoot;
        std::wstring stampPath;
        std::wstring statePath;
    };

    std::atomic<int>  g_playState{PLAY_IDLE};
    std::atomic<bool> g_playCancel{false};
    std::atomic<bool> g_playThreadDone{false};
    std::atomic<bool> g_playFullMode{false};
    std::thread       g_playThread;
    std::mutex        g_playMutex;
    HANDLE            g_playToolProcess = nullptr;
    HANDLE            g_playGameProcess = nullptr;
    HANDLE            g_playGameJob = nullptr; // kill-on-close job: BO3 dies automatically when Radiant exits
    DWORD             g_playGamePid = 0;
    bool              g_playGameOwnedByRadiant = false; // engine writes only for editor-created local test processes
    std::vector<std::string> g_playPendingLog;
    std::string       g_playLogText;
    std::string       g_playLastError;
    std::string       g_playCurrentMap;
    std::atomic<uint64_t> g_playStartedMs{0};
    bool              g_playEnabled = true;
    bool              g_playAutoSave = true;
    bool              g_playKillPrevious = true;
    bool              g_playKeepWarm = true;
    bool              g_playPrewarmOnMapOpen = false; // legacy background prewarm is optional
    bool              g_playGamePresented = false;   // false = runtime stays hidden/warm, true = user has entered Play
    // Parked: End Session hides the game with its level loaded (pane closed, audio muted, AI off, god on) so the next Play is a reload,
    // then Close BO3 quits. prewarm_on_map_open starts a parked game. Cleared by any Play, a stop or the game's exit.
    std::atomic<bool> g_playParked{false};
    bool              g_playParkMuted = false;       // the game's audio session is muted (retried until the session exists)
    bool              g_playParkCmdsSent = false;    // ai_disableSpawn 1 reached the zone module's mailbox
    uint64_t          g_playParkMuteNextMs = 0;
    constexpr int     kPlayParkFps = 10;             // com_maxfps while parked: 0.78 -> 0.20 cores, GPU 8.5 -> 3.3 % (zm_testingmap)
    std::wstring      g_playParkGameRoot;            // where players\config.ini's MaxFPS is guarded against the parked cap
    // Early launch ([playtest] early_launch): a cold Play starts BO3 at the click while it builds. The bridge holds the first main
    // frame (launchHold) until PlayEarlyLaunchTick releases it.
    bool              g_playEarlyLaunch = true;
    std::atomic<bool> g_playLaunchHeld{false};     // the hold is (to be) set in the game started for this build
    std::atomic<int>  g_playEarlyBuild{0};         // 1 building, 2 built (release when ready)
    std::atomic<bool> g_playEarlyAborted{false};   // the hold never engaged: the game was closed, the build launches after
    uint64_t          g_playEarlyLaunchMs = 0;
    uint64_t          g_playEarlyLaunchFt = 0;     // the same moment as a FILETIME (console_mp.log written by this game or not)
    std::wstring      g_playEarlyGameRoot;
    uint64_t          g_playEarlyMinHoldMs = 0;    // dev ini only, [debug] early_min_hold_s: test a long build's hold on a quick one
    bool              g_playEarlyReloadPending = false;   // the level had opened its files before the hold: reload once up
    bool              g_playPrewarmed = false;       // runtime was started by the background prewarmer
    bool              g_playPrewarmWindowHidden = false;
    uint64_t          g_playLastPrewarmPoll = 0;
    uint64_t          g_playLastPrewarmAttempt = 0;
    std::string       g_playPrewarmMap;
    std::string       g_playPrewarmSuppressedMap;     // manual Close BO3 suppresses auto-relaunch for that map
    bool              g_playEmbedGame = true;      // BO3 docks into its own "BO3 Live" pane
    bool              g_playRebuildNav = false;
    bool              g_playExportLighting = true;    // Play exports Radiant's built, unexported lighting (.led) first
    bool              g_playAutoRestart = true;
    bool              g_playInstantStart = true;   // startup black/fade bypass (per player, no level-wide fade call)
    bool              g_playFromCamera = true;     // Play From Here: spawn where Radiant's camera is, through br_play_* dvars and a marked GSC receiver
    bool              g_playFly = false;           // "Fly": noclip through PlayCheatsTick, and Play From Here keeps the exact camera point
    bool              g_playGod = false;           // "God": the god console command through PlayCheatsTick
    bool              g_playDisableAi = false;     // "Disable AI": ai_disableSpawn 1 at launch and on every toggle while the game runs
    // Join Host: this Play builds as usual, then launches BO3 to its menu with net_broadcast_ip = the Team Create host, and the
    // bridge's join module (bridge/join.cpp) joins the host's LAN game.
    bool              g_playJoinMode = false;
    std::string       g_playJoinHost;
    // The Team Create host's game is up on the map open here ("TeamCreate hostplaying", polled by PlayHostPlayingTick): the Play
    // button reads Join Host.
    bool              g_playHostPlaying = false;
    bool              g_playJoinUsermapEntry = true;   // [playtest] join_usermap_entry: the bridge puts the map into the usermap list
    std::string       g_playJoinStatusSeen;
    std::string       g_playJoinSeq;               // the request this launch wrote: only its answers are relayed (BO3LiveJoin.status)
    bool              g_playLanWanted = false;     // a Team Create session: the bridge opens BO3's LAN socket (bridge/join.cpp LanTick)
    bool              g_playFromStartOnce = false; // command "PlayTest start": ignore the camera for the next Play only
    std::string       g_playCamDvar;               // "x y z pitch yaw" (game angles) captured on the main thread by PlayStart, empty = stock spawn
    int               g_playCamGen = 0;            // bumped per Play so the receiver re-applies on a hot Play (nothing rebuilt)
    // automatic reload after a live bake (g_playLiveBakeReload)
    bool              g_playKeepPosNext = false, g_playKeepPos = false;   // this Play leaves br_play_cam / br_play_mode to the place the game kept
    std::atomic<bool> g_playForceReloadOnce{false};                       // this Play reloads the level although nothing was built
    std::string       g_playLevelProbeKey;         // PlayReadLedProbes "structure" of the .led the running level was linked from
    bool              g_playLevelProbeKeySet = false;   // cleared by every Play (the level loads again)
    bool              g_playLivePreview = true;    // selected classic brush -> live in-game wireframe proxy, no compile/link
    bool              g_playInputCaptured = false;
    std::string       g_playGameMap;
    HWND              g_playEmbeddedHwnd = nullptr;
    HWND              g_playEmbeddedOriginalParent = nullptr;
    LONG_PTR          g_playEmbeddedStyle = 0;
    LONG_PTR          g_playEmbeddedExStyle = 0;
    RECT              g_playEmbeddedRect = {};
    std::string       g_playLanguage = "english";
    std::string       g_playExtraArgs;
    // The Dvars window ([playtest] dvar_* keys). Written on the UI thread, read by the worker: copy under g_playMutex.
    struct PlayDvars
    {
        int         developer = 1;        // 1 or 2
        bool        splitscreen = false;  // splitscreen 1 with 2 players
        int         logfile = 0;          // 0 to 2
        std::string custom;               // "name value; name value"
    };
    PlayDvars         g_playDvars;
    std::string       g_playLaunchColdKey;    // the start-only dvars the running game was launched with
    uint64_t          g_playLastUiRefresh = 0;
    int               g_playLastUiState = PLAY_IDLE;
    size_t            g_playLastUiLogSize = (size_t)-1;
    std::string       g_playPreflightSummary = "Map setup has not been checked yet.";
    uint64_t          g_playLastLivePoll = 0;
    std::string       g_playLastLiveKey;
    bool              g_playLiveSentDisabled = false;
    uint32_t          g_playLiveUpdates = 0;
    bool              g_playLiveBackgroundWarned = false;
    HANDLE            g_playLiveMapHandle = nullptr;
    bool              g_playLiveNavMesh = true;         // [playtest] live_navmesh (published as SharedState::liveNavEnabled)
    bool              g_playLiveOcclusion = true;       // [playtest] live_occlusion (published as SharedState::liveOccEnabled)
    brlive::SharedState* g_playLiveShared = nullptr;
    DWORD             g_playLiveBridgePid = 0;
    bool              g_playLiveBridgeInjected = false;
    bool              g_playLiveZone = false;         // live zone: scripts and BSP linked as <map>_patch, the engine loads it after the base zone
    bool              g_playLiveZoneInjected = false;  // the bridge's zone module answered ZONE_READY (mailbox live)
    bool              g_playLiveZoneArmed = false;     // zoneArm set in the shared state for this game
    bool              g_playLiveZoneWarned = false;
    bool              g_playAutoLinkMaterials = true;  // [playtest] auto_link_materials: a live material the level lacks is linked into the patch zone and the game reloaded
    std::set<std::string> g_playLiveMaterialsQueued;   // "<map>|<material>" already written to the patch zone's live material list this session
    uint64_t          g_playLiveMaterialsDueMs = 0;    // when the automatic Smart Play for queued materials fires (debounced)
    bool              g_playLiveMaterialsNoZoneWarned = false;
    bool              g_playLiveMaterialLoad = true;    // [playtest] live_material_load: also load the carrier zone into the RUNNING level (off: any mid-level zone load flushes every streamed texture, a multi-second freeze)
    std::vector<std::string> g_playPreloadMaterials;    // [playtest] preload_materials: name prefixes whose carriers go into the patch zone at every Play
    // live zone loads: new materials are linked as usermaps\<map>_lm<stamp> on this thread, the bridge loads the fastfile into the
    // running level (no map reload)
    std::vector<std::string> g_playLiveMatPending;
    // live zones stay loaded side by side (freeing one cost the main thread 300+ ms) and carry only their new materials. Every
    // kPlayLiveKeepZones-th is a FULL zone (<map>_lmf<stamp>) with all live materials of this game session, loaded with freeing so
    // the engine's 64-zone limit is never met
    constexpr int     kPlayLiveKeepZones = 16;
    int               g_playLiveKeptZones = 0;
    DWORD             g_playLiveKeptPid = 0;
    std::vector<std::string> g_playLiveSessionNames;   // materials live-loaded into the current game process
    std::thread       g_playLiveMatThread;
    std::atomic<bool> g_playLiveMatThreadDone{false};
    std::atomic<bool> g_playLiveMatRunning{false};
    std::atomic<bool> g_playLiveMatCancel{false};     // Play starts: its patch zone takes the names, the link in flight is killed
    std::mutex        g_playLiveMatMutex;
    struct PlayLiveMatResult { bool ok = false; std::string zoneName, error; int count = 0; uint64_t seconds = 0; };
    PlayLiveMatResult g_playLiveMatResult;
    LONG              g_playLiveMatLastDoneSeq = 0;
    LONG              g_playLiveMatZoneSeq = 0;         // the zoneLoadSeq of the live material zone being loaded (0: none), for the simple log
    // Nav mesh refresh: after settled brush edits (pane button, `PlayTest nav`) the map is compiled again, the fresh `navmesh` is
    // linked as a nav-only live zone, loaded by the bridge and swapped into the running level's Havok world (no reload).
    bool              g_playLiveNavRefresh = true;      // [playtest] live_nav_refresh: automatic after settled edits
    std::thread       g_playNavThread;
    std::atomic<bool> g_playNavThreadDone{false};
    std::atomic<bool> g_playNavRunning{false};
    std::atomic<bool> g_playNavCancel{false};         // Play starts: the refresh's result would be stale, its build is killed
    std::atomic<bool> g_playNavForPlay{false};        // the nav build runs beside Play's compile: its zone goes into the next level, not this one
    std::atomic<LONG> g_playNavForPlayInitBase{-1};   // the bridge's G_InitGame count when that build started (-1: no game), the next level moves it
    std::mutex        g_playNavMutex;
    std::wstring      g_playNavKeepStem;                // (g_playNavMutex) zone built beside Play, kept by the zone sweeps until it went in or failed
    struct PlayNavResult { bool ok = false; std::string zoneName, assetName, error; uint64_t seconds = 0; };
    PlayNavResult     g_playNavResult;
    uint64_t          g_playNavDueMs = 0;               // a refresh is wanted at this tick time (0 = none)
    std::string       g_playNavDueWhy;
    LONG              g_playNavWaitZoneSeq = 0;         // the zoneLoadSeq of our zone, waited for
    LONG              g_playNavWaitSwapSeq = 0;         // the navSwapSeq of our request, waited for
    int               g_playNavPhase = 0;               // 0 idle, 1 building, 2 reverting (live_nav_revert), 3 zone loading, 4 swapping, 5 waiting for Play's level
    uint64_t          g_playNavPhaseMs = 0;
    std::string       g_playNavZoneName;
    bool              g_playNavWorldWasActive = false;
    bool              g_playNavDumpTaken = false;
    bool              g_playLiveNavRevert = true;       // [playtest] live_nav_revert: back to the base mesh before the next nav zone loads
    std::string       g_playNavPendingAsset;
    LONG              g_playNavGeometrySeen = 0;        // create + update + delete + hide counters at the last refresh
    int               g_playNavCount = 0, g_playNavFailures = 0;
    LONG              g_playNavLevelEpoch = 0;          // bumped whenever the level goes down or comes up
    LONG              g_playNavStartEpoch = 0;          // the epoch a refresh started in: its zone is for that level only
    // Live zones (nav refresh, live materials) loaded into the running level: a warm devmap after one hung the game, so the
    // bridge relabels them at G_ShutdownGame. [playtest] warm_after_live_zone=0 relaunches BO3 instead.
    std::atomic<int>  g_playMidLevelZoneLoads{0};
    bool              g_playWarmAfterLiveZone = true;
    bool              g_playLiveZonePending = false;  // a patch zone was placed before the game could load it (cold launch): reload once in the level
    uint64_t          g_playLiveZoneLaunchMs = 0;
    std::wstring      g_playLiveZoneConsoleLog;       // <root>\console_mp.log of the launch a pending patch waits for (the game rewrites it per launch)
    uint64_t          g_playLiveZoneLogCheckMs = 0;
    bool              g_playLiveBridgeLoggedReady = false;
    bool              g_playLiveDiagLogged = false;
    bool              g_playLiveDiagWarned = false;
    LONG              g_playLiveLastFrameSeq = -1;
    LONG              g_playLiveLastDiagInputSeq = -1;
    LONG              g_playLiveLastDiagProjectionSeq = -1;
    std::string       g_playLiveBridgeStatus;
    bool              g_playEngineLoggedReady = false;
    bool              g_playEngineLoggedDiscovery = false;
    bool              g_playEngineLoggedCbuf = false;
    bool              g_playEngineLoggedGameState = false;
    bool              g_playEngineLoggedNative = false;
    std::string       g_playEngineLastTexturedStatus;
    LONG              g_playEngineLastTexturedResolved = -1, g_playEngineLastTexturedMissing = -1;
    bool              g_playEngineLoggedTriangleGuardDiagnostics = false;
    LONG              g_playEngineLastState = -1;
    LONG              g_playEngineLastServerRunning = -999;
    LONG              g_playEngineLastWorldActive = -1;
    std::string       g_playEngineLastActiveMap;
    LONG              g_playEngineLastHeartbeat = -1;
    LONG              g_playEngineLastCommandAck = -1;
    LONG              g_playEngineUserCommandSeq = 0;
    LONG              g_playEngineUserCommand = brlive::ENGINE_CMD_NONE;
    uint64_t          g_playEngineLastQueryMs = 0;
    LONG              g_playEngineLastLifecycleOrder = -1;
    bool              g_playEngineLoggedLifecycleTrace = false;
    uint32_t          g_playLiveMoveEvents = 0;
    uint32_t          g_playLiveSelectMoveEvents = 0;
    uint32_t          g_playLiveSnapshotFailures = 0;
    bool              g_playLiveMoveLogged = false;
    bool              g_playLiveSnapshotWarned = false;
    bool              g_playLiveMouseWasDown = false;
    bool              g_playLiveDragArmed = false;
    POINT             g_playLiveMouseDownPt = {0,0};
    bool              g_playLiveReleaseSnapshotPending = false;
    bool              g_playLiveMaterialCheckPending = false;   // a click without a drag (texture browser, surface inspector): re-read materials
    uint64_t          g_playLiveMaterialCheckDue = 0;
    uint64_t          g_playLiveReleaseSnapshotDue = 0;
    bool              g_playLiveDeleteFollowupPending = false;  // a clip or replace: Select_Delete ran, publish what is selected once it settled
    uint64_t          g_playLiveDeleteFollowupDue = 0;
    uint32_t          g_playLiveReleaseEvents = 0;
    uint64_t          g_playLiveLastRegistryReconcile = 0;
    std::atomic<bool> g_playLiveClearAfterBuild{false};

    const char* PlayEngineStateName(LONG state);
    void PlayEngineSetRequestedMap();
    bool PlayEngineSendCommand(LONG command, const char* text = nullptr);

    // Host-owned Qt controls: real Radiant widgets and dock panes, not Win32 overlays.
    ModToolsPane       g_playPane = nullptr;
    ModToolsPane       g_playGamePane = nullptr;
    ModToolsWidget     g_playRoot = nullptr;
    ModToolsWidget     g_playGameRoot = nullptr;
    ModToolsWidget     g_playGameView = nullptr;
    ModToolsWidget     g_playGameStatus = nullptr;
    uintptr_t       (*g_playQtWinId)(void*) = nullptr;   // QWidget::winId for the dedicated BO3 pane
    DWORD             g_playUiThreadId = 0;              // the thread that owns the Qt widgets (set with the panes)
    RECT              g_playPaneRectCache = {};  // BO3 Live view screen rect, refreshed by the UI tick for the worker
    int               g_playInputProp = -1;              // last BRLiveInput value set on the game window (-1 = none)
    ModToolsWidget     g_playPrimaryButton = nullptr;
    ModToolsWidget     g_playCheckButton = nullptr;
    ModToolsWidget     g_playFullCheck = nullptr;
    ModToolsWidget     g_playWarmCheck = nullptr;
    ModToolsWidget     g_playPrewarmCheck = nullptr;
    ModToolsWidget     g_playEmbedCheck = nullptr;
    ModToolsWidget     g_playNavCheck = nullptr;
    ModToolsWidget     g_playAutoRestartCheck = nullptr;
    ModToolsWidget     g_playInstantStartCheck = nullptr;
    ModToolsWidget     g_playFromCameraCheck = nullptr;
    ModToolsWidget     g_playFlyCheck = nullptr;
    ModToolsWidget     g_playDisableAiCheck = nullptr;
    ModToolsWidget     g_playGodCheck = nullptr;
    ModToolsWidget     g_playLiveCheck = nullptr;
    ModToolsWidget     g_playEnhancedLabel = nullptr;   // "get BO3 Enhanced", shown while BlackOps3.exe is another build
    int               g_playEnhancedState = -1;        // the build the label last followed: 1 Enhanced (hidden), 0 other (shown), -1 not set
    uint64_t          g_playHideNewPaneUntil = 0;      // first start: keep the never-seen pane hidden until this tick count
    ModToolsWidget     g_playCloseButton = nullptr;
    ModToolsWidget     g_playEngineLoadButton = nullptr;
    ModToolsWidget     g_playEngineReloadButton = nullptr;
    ModToolsWidget     g_playEngineFastRestartButton = nullptr;
    ModToolsWidget     g_playStatusLabel = nullptr;
    ModToolsWidget     g_playPreflightLabel = nullptr;
    ModToolsWidget     g_playLogEdit = nullptr;
    ModToolsWidget     g_playVerboseCheck = nullptr;   // "Verbose log" under the log

    std::wstring PlayUtf8ToWide(const std::string& s)
    {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring out(n > 0 ? n : 0, L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
        return out;
    }

    std::string PlayWideToUtf8(const std::wstring& s)
    {
        if (s.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        std::string out(n > 0 ? n : 0, '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
        return out;
    }

    std::wstring PlayJoin(const std::wstring& a, const std::wstring& b)
    {
        if (a.empty()) return b;
        if (b.empty()) return a;
        if (a.back() == L'\\' || a.back() == L'/') return a + b;
        return a + L"\\" + b;
    }

    std::wstring PlayParent(const std::wstring& path)
    {
        size_t p = path.find_last_of(L"\\/");
        return p == std::wstring::npos ? std::wstring() : path.substr(0, p);
    }

    std::wstring PlayBaseName(const std::wstring& path)
    {
        size_t p = path.find_last_of(L"\\/");
        return p == std::wstring::npos ? path : path.substr(p + 1);
    }

    bool PlayFileExists(const std::wstring& path)
    {
        DWORD a = GetFileAttributesW(path.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }

    bool PlayDirExists(const std::wstring& path)
    {
        DWORD a = GetFileAttributesW(path.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    }

    uint64_t PlayFileTime(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA d = {};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0;
        ULARGE_INTEGER u = {}; u.LowPart = d.ftLastWriteTime.dwLowDateTime; u.HighPart = d.ftLastWriteTime.dwHighDateTime;
        return u.QuadPart;
    }

    std::wstring PlayGetEnv(const wchar_t* name)
    {
        DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
        if (!n) return {};
        std::wstring out(n, L'\0');
        DWORD got = GetEnvironmentVariableW(name, &out[0], n);
        if (!got) return {};
        out.resize(got);
        while (!out.empty() && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
        return out;
    }

    std::wstring PlayExeRoot()
    {
        wchar_t path[MAX_PATH * 4] = {};
        DWORD n = GetModuleFileNameW(nullptr, path, (DWORD)(sizeof(path) / sizeof(path[0])));
        if (!n || n >= sizeof(path) / sizeof(path[0])) return {};
        std::wstring bin = PlayParent(std::wstring(path, n));
        return PlayParent(bin);
    }

    std::wstring PlayGameRoot()
    {
        std::wstring root = PlayGetEnv(L"TA_GAME_PATH");
        if (root.empty()) root = PlayGetEnv(L"TA_TOOLS_PATH");
        if (root.empty()) root = PlayExeRoot();
        if (!root.empty() && !PlayFileExists(PlayJoin(root, L"BlackOps3.exe")))
        {
            std::wstring parent = PlayParent(root);
            if (PlayFileExists(PlayJoin(parent, L"BlackOps3.exe"))) root = parent;
        }
        return root;
    }

    // ---- Which BlackOps3.exe ----
    // Enhanced = full mode (bridge hooks it), Steam = basic mode (patch zones, Play From Here, console, outlines through the map script),
    // any other build only docks.
    enum PlayGameBuild { PLAY_BUILD_NONE = 0, PLAY_BUILD_UNKNOWN, PLAY_BUILD_STEAM, PLAY_BUILD_ENHANCED };
    PlayGameBuild PlayReadExeBuild(const std::wstring& exe, DWORD& stamp)
    {
        stamp = 0;
        HANDLE f = CreateFileW(exe.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return PLAY_BUILD_NONE;
        unsigned char head[4096] = {}; DWORD got = 0;
        ReadFile(f, head, sizeof head, &got, nullptr);
        CloseHandle(f);
        if (got < sizeof(IMAGE_DOS_HEADER)) return PLAY_BUILD_UNKNOWN;
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(head);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || (DWORD)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > got) return PLAY_BUILD_UNKNOWN;
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(head + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return PLAY_BUILD_UNKNOWN;
        stamp = nt->FileHeader.TimeDateStamp;
        const DWORD size = nt->OptionalHeader.SizeOfImage;
        if (stamp == 0x67363F2Au && size == 0x1A53F000u) return PLAY_BUILD_ENHANCED;
        if (stamp == 0x6A7B6355u && size == 0x1D75BC00u) return PLAY_BUILD_STEAM;
        return PLAY_BUILD_UNKNOWN;
    }
    PlayGameBuild g_playExeBuild = PLAY_BUILD_NONE;
    DWORD g_playExeStamp = 0;
    uint64_t g_playExeCheckMs = 0;
    // <root>\BlackOps3.exe, re-read at most every 5 s (a rename between two Plays is picked up).
    PlayGameBuild PlayCurrentExeBuild(bool force = false)
    {
        const uint64_t now = GetTickCount64();
        if (force || !g_playExeCheckMs || now - g_playExeCheckMs > 5000)
        {
            g_playExeCheckMs = now;
            g_playExeBuild = PlayReadExeBuild(PlayJoin(PlayGameRoot(), L"BlackOps3.exe"), g_playExeStamp);
        }
        return g_playExeBuild;
    }
    std::string PlayHex32(DWORD v) { char b[16]; sprintf_s(b, "0x%08lX", (unsigned long)v); return b; }
    std::string PlayBuildModeShort(PlayGameBuild b)
    {
        switch (b)
        {
            case PLAY_BUILD_ENHANCED: return "BO3 Enhanced";
            case PLAY_BUILD_STEAM: return "Regular BO3 (no Live Edit, get Black Ops 3 Enhanced)";
            case PLAY_BUILD_UNKNOWN: return "Unknown BO3 build (no Live Edit, get Black Ops 3 Enhanced)";
            default: return "BlackOps3.exe not found";
        }
    }
    // Live Edit (anything that changes the map in the running game) is BO3 Enhanced only, the pane says to get it.
    const char* const kPlayGetEnhanced = "Live Edit needs Black Ops 3 Enhanced. Get Black Ops 3 Enhanced to see map edits in the running game.";
    const char* const kPlayTipPlay = "Build what changed and play the map. A running game reloads the map instead of starting again.";
    const char* const kPlayTipJoin = "The Team Create host is playing this map: get the host's build, start BO3 at its menu and join the host's game (LAN play; same network or a VPN).";
    bool PlayLiveEditBuild() { return PlayCurrentExeBuild() == PLAY_BUILD_ENHANCED; }

    // Live Edit ticked without BO3 Enhanced: say it is required, with the repo as a clickable link (TaskDialogIndirect under our
    // Common Controls 6 context, a Yes/No box when unavailable).
    const wchar_t* const kPlayEnhancedUrl = L"https://github.com/shiversoftdev/BO3Enhanced";
    HRESULT CALLBACK PlayEnhancedBoxProc(HWND, UINT msg, WPARAM, LPARAM lParam, LONG_PTR)
    {
        if (msg == TDN_HYPERLINK_CLICKED && lParam) ShellExecuteW(nullptr, L"open", reinterpret_cast<const wchar_t*>(lParam), nullptr, nullptr, SW_SHOWNORMAL);
        return S_OK;
    }
    void PlayShowEnhancedRequiredBox()
    {
        HWND owner = static_cast<HWND>(rp::host && rp::host->MainWindow ? rp::host->MainWindow(rp::self) : nullptr);
        bool shown = false;
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&PlayShowEnhancedRequiredBox), &self);
        ACTCTXW ctx = { sizeof ctx }; ctx.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID; ctx.hModule = self; ctx.lpResourceName = MAKEINTRESOURCEW(2);
        HANDLE act = self ? CreateActCtxW(&ctx) : INVALID_HANDLE_VALUE; ULONG_PTR cookie = 0;
        if (act != INVALID_HANDLE_VALUE && ActivateActCtx(act, &cookie))
        {
            TASKDIALOGCONFIG td = { sizeof td };
            td.hwndParent = owner; td.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
            td.dwCommonButtons = TDCBF_OK_BUTTON; td.pszWindowTitle = L"Play Test"; td.pszMainIcon = TD_INFORMATION_ICON;
            td.pszMainInstruction = L"Live Edit needs BO3 Enhanced";
            td.pszContent = L"Live Edit shows your map edits in the running game. It works only with the BO3 Enhanced build of BlackOps3.exe, which this PC does not have.\n\n"
                            L"Get it here: <a href=\"https://github.com/shiversoftdev/BO3Enhanced\">github.com/shiversoftdev/BO3Enhanced</a>";
            td.pfCallback = PlayEnhancedBoxProc;
            shown = SUCCEEDED(TaskDialogIndirect(&td, nullptr, nullptr, nullptr));
            DeactivateActCtx(0, cookie);
        }
        if (act != INVALID_HANDLE_VALUE) ReleaseActCtx(act);
        if (shown) return;
        int r = MessageBoxW(owner, L"Live Edit needs BO3 Enhanced.\n\nLive Edit shows your map edits in the running game. It works only with the BO3 Enhanced build of BlackOps3.exe, which this PC does not have.\n\n"
                                   L"Open github.com/shiversoftdev/BO3Enhanced in your browser?", L"Play Test", MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND);
        if (r == IDYES) ShellExecuteW(nullptr, L"open", kPlayEnhancedUrl, nullptr, nullptr, SW_SHOWNORMAL);
    }
    std::string PlayBuildModeLog(PlayGameBuild b)
    {
        switch (b)
        {
            case PLAY_BUILD_ENHANCED: return "[BUILD] BO3 Enhanced (0x67363F2A): full mode. Native solid Live Edit, engine state, patch zones, Play From Here and console reloads.";
            case PLAY_BUILD_STEAM: return std::string("[BUILD] Regular BlackOps3.exe (0x6A7B6355): Play, patch zones, Play From Here and console reloads; nothing is changed in the running game. ") + kPlayGetEnhanced;
            case PLAY_BUILD_UNKNOWN: return "[BUILD WARN] BlackOps3.exe build " + PlayHex32(g_playExeStamp) + " is not known to Play Test: the game docks, but live zone, Play From Here, Live Edit and console commands are off. " + kPlayGetEnhanced;
            default: return "[BUILD WARN] BlackOps3.exe was not found or could not be read.";
        }
    }

    std::wstring PlayToolsRoot(const std::wstring& gameRoot)
    {
        std::wstring root = PlayGetEnv(L"TA_TOOLS_PATH");
        if (root.empty()) root = gameRoot;
        return root;
    }

    std::wstring PlayLower(std::wstring s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c){ return (wchar_t)towlower(c); });
        return s;
    }

    struct PlayWindowFind { DWORD pid = 0; HWND hwnd = nullptr; std::wstring title; };
    BOOL CALLBACK PlayEnumWindowsProc(HWND hwnd, LPARAM lp)
    {
        auto* f = reinterpret_cast<PlayWindowFind*>(lp);
        DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
        if (pid != f->pid || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
        wchar_t title[4096] = {}; GetWindowTextW(hwnd, title, (int)(sizeof(title) / sizeof(title[0])));
        std::wstring t = title;
        std::wstring low = PlayLower(t);
        if (low.find(L".map") != std::wstring::npos)
        {
            f->hwnd = hwnd; f->title = t; return FALSE;
        }
        if (!f->hwnd && IsWindowVisible(hwnd) && !t.empty()) { f->hwnd = hwnd; f->title = t; }
        return TRUE;
    }

    HWND PlayFindRadiantWindow(std::wstring* titleOut = nullptr)
    {
        PlayWindowFind f; f.pid = GetCurrentProcessId();
        EnumWindows(PlayEnumWindowsProc, reinterpret_cast<LPARAM>(&f));
        if (titleOut) *titleOut = f.title;
        return f.hwnd;
    }


    DWORD PlayFindBo3ProcessId()
    {
        DWORD found = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        PROCESSENTRY32W pe = {}; pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe))
        {
            do
            {
                if (_wcsicmp(pe.szExeFile, L"BlackOps3.exe") == 0) { found = pe.th32ProcessID; break; }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        return found;
    }

    struct PlayGameWindowFind { DWORD pid = 0; HWND hwnd = nullptr; };
    BOOL CALLBACK PlayEnumGameWindowsProc(HWND hwnd, LPARAM lp)
    {
        auto* f = reinterpret_cast<PlayGameWindowFind*>(lp);
        DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
        // The instant-prewarm path hides BO3 while it warms: keep hidden top-level game windows discoverable for Smart Play.
        if (pid != f->pid) return TRUE;
        // The game's window class is CoDBlackOps. A windowed (docked) game has an owner window, borderless fullscreen does not: only
        // unknown classes are held to the "no owner" rule.
        wchar_t cls[64] = {}; GetClassNameW(hwnd, cls, 64);
        const bool gameClass = _wcsicmp(cls, L"CoDBlackOps") == 0;
        if (!gameClass && GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
        RECT r = {}; if (!GetWindowRect(hwnd, &r) || r.right - r.left < 320 || r.bottom - r.top < 200) return TRUE;
        f->hwnd = hwnd; return FALSE;
    }

    HWND PlayFindGameWindow(DWORD pid)
    {
        if (!pid) return nullptr;
        PlayGameWindowFind f; f.pid = pid;
        EnumWindows(PlayEnumGameWindowsProc, reinterpret_cast<LPARAM>(&f));
        return f.hwnd;
    }

    HWND PlayWaitForGameWindow(DWORD pid, DWORD timeoutMs)
    {
        const uint64_t end = GetTickCount64() + timeoutMs;
        HWND w = nullptr;
        do
        {
            w = PlayFindGameWindow(pid);
            if (w) return w;
            Sleep(100);
        } while (GetTickCount64() < end && !g_playCancel);
        return nullptr;
    }

    // Dock requests go through the bridge's window subclass on the game's own thread: a synchronous call into a docked window
    // (shared input queue) deadlocked both editors.
    bool PlayGameTakesDockRequests(HWND game) { return game && IsWindow(game) && GetPropW(game, BRLIVE_DOCK_PROP) != nullptr; }
    ULONG_PTR PlayPack(int lo, int hi) { return (static_cast<ULONG_PTR>(static_cast<DWORD>(lo))) | (static_cast<ULONG_PTR>(static_cast<DWORD>(hi)) << 32); }

    void PlayDetachEmbeddedGame()
    {
        g_playInputCaptured = false;
        ClipCursor(nullptr);
        HWND game = g_playEmbeddedHwnd;
        g_playEmbeddedHwnd = nullptr;
        g_playEmbeddedOriginalParent = nullptr;
        if (!game || !IsWindow(game)) return;
        if (PlayGameTakesDockRequests(game)) PostMessageW(game, brlive::WM_RESTORE, 0, 0);
    }

    void PlayAppendLog(const std::string& line);
    void PlayNote(const std::string& text);

    // UI thread only: QWidget::winId creates the native window on the CALLING thread, so a worker-made HWND died with the worker
    // and Qt kept the stale handle. Off the UI thread this returns null and callers use g_playPaneRectCache.
    HWND PlayGamePaneHostHwnd()
    {
        if (!g_playGameView || !g_playQtWinId) return nullptr;
        if (g_playUiThreadId && GetCurrentThreadId() != g_playUiThreadId) return nullptr;
        HWND host = reinterpret_cast<HWND>(g_playQtWinId(g_playGameView));
        if (host && !IsWindow(host))
        {
            static bool warned = false;
            if (!warned) { warned = true; PlayAppendLog("[EMBED WARN] The BO3 Live view's native window is gone (Qt still holds the handle); reload the PlayTest plugin to get a fresh view."); }
            return nullptr;
        }
        return host;
    }

    RECT PlayGamePaneScreenRect()
    {
        if (g_playUiThreadId && GetCurrentThreadId() != g_playUiThreadId) return g_playPaneRectCache;
        RECT out = {};
        HWND host = PlayGamePaneHostHwnd();
        if (host) GetWindowRect(host, &out);
        g_playPaneRectCache = out;
        return out;
    }

    void PlayUpdateEmbeddedBounds()
    {
        if (!g_playEmbeddedHwnd || !IsWindow(g_playEmbeddedHwnd)) return;
        HWND host = PlayGamePaneHostHwnd();
        if (!host) return;
        RECT r = {}; if (!GetClientRect(host, &r)) return;
        int w = (std::max)(64L, r.right - r.left);
        int h = (std::max)(64L, r.bottom - r.top);
        // Cross-process window: a plain SetWindowPos waits on the game's window thread (DXGI waits on it too, a hang). Async, only on
        // a change, and not while a mouse button is down: the final size goes out when the drag ends.
        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) return;
        // Compare the game's ACTUAL placement, not what we sent: after a docked launch BO3 applies its own video mode again (config
        // WindowX/Y taken as child coordinates).
        RECT g = {}; if (!GetWindowRect(g_playEmbeddedHwnd, &g)) return;
        MapWindowPoints(HWND_DESKTOP, host, reinterpret_cast<POINT*>(&g), 2);
        if (g.left == 0 && g.top == 0 && g.right - g.left == w && g.bottom - g.top == h) return;
        static uint64_t lastPost = 0;  // posted and rate limited: the game may still be applying its mode
        const uint64_t now = GetTickCount64();
        if (now - lastPost < 250) return;
        lastPost = now;
        // Never SetWindowPos from here, even async: a docked child shares our input queue, so the call waits on the game's window
        // thread (the command channel unloaded this plugin under that frame). The bridge re-applies the dock geometry itself.
        if (PlayGameTakesDockRequests(g_playEmbeddedHwnd)) PostMessageW(g_playEmbeddedHwnd, brlive::WM_DOCK_INTO, reinterpret_cast<WPARAM>(host), 0);
    }

    void PlayTellGameInactive(HWND game);
    void PlayAppendLog(const std::string& line);
    std::wstring PlayThisModuleDir();
    bool PlayBusy();
    void PlayNavRefreshSchedule(const char* why, uint64_t delayMs);
    void PlayNavRefreshTick();
    std::string PlayLiveGuidKey(const char* guid);
    bool PlayCaptureGameControls();

    bool PlayEmbedGameWindow(HWND game)
    {
        if (!game || !IsWindow(game) || !rp::host) return false;
        if (g_playGamePane && rp::host->ShowPane) rp::host->ShowPane(rp::self, g_playGamePane, 1);
        HWND host = PlayGamePaneHostHwnd();
        if (!host)
        {
            static uint64_t lastHostLog = 0;
            const uint64_t now = GetTickCount64();
            if (now - lastHostLog > 5000) { lastHostLog = now; PlayAppendLog(std::string("[EMBED] No BO3 Live pane host window yet (pane ") + (g_playGamePane ? "exists" : "missing") + ", view " + (g_playGameView ? "exists" : "missing") + "); the game stays a top-level window."); }
            return false;
        }
        if (g_playEmbeddedHwnd == game) { PlayUpdateEmbeddedBounds(); return true; }
        if (!PlayGameTakesDockRequests(game))
        {
            static uint64_t lastLog = 0;
            const uint64_t now = GetTickCount64();
            if (now - lastLog > 5000) { lastLog = now; PlayAppendLog("[EMBED] Waiting for BO3LiveBridge.dll to take dock requests (it docks the game from inside)."); }
            return false;
        }
        PlayDetachEmbeddedGame();
        g_playEmbeddedOriginalParent = GetParent(game);
        g_playEmbeddedStyle = GetWindowLongPtrW(game, GWL_STYLE);
        g_playEmbeddedExStyle = GetWindowLongPtrW(game, GWL_EXSTYLE);
        GetWindowRect(game, &g_playEmbeddedRect);
        g_playEmbeddedHwnd = game;
        g_playInputProp = -1;              // a new game window: publish the input flag again
        PostMessageW(game, brlive::WM_DOCK_INTO, reinterpret_cast<WPARAM>(host), 0);
        PlayTellGameInactive(game);
        return true;
    }

    void PlayAppendLog(const std::string& line);
    bool PlayFocusWindow(HWND hwnd);

    void PlayApplyEmbeddedChildPresentation(HWND game)
    {
        if (!game || !IsWindow(game) || !rp::host) return;
        HWND host = PlayGamePaneHostHwnd();
        if (!host) return;
        g_playInputCaptured = false;
        if (PlayGameTakesDockRequests(game)) PostMessageW(game, brlive::WM_DOCK_INTO, reinterpret_cast<WPARAM>(host), 0);
        PlayTellGameInactive(game);
    }

    // BO3 rewrites its window style after docking (WS_CHILD dropped while it applies its video mode on map load) and becomes a
    // borderless top-level window. Dock it again, but not while Control BO3 has it top-level on purpose.
    int g_playRedocks = 0;
    void PlayRedockIfEscaped()
    {
        HWND game = g_playEmbeddedHwnd;
        HWND host = PlayGamePaneHostHwnd();
        if (!game || !IsWindow(game) || !host) return;
        if (GetParent(game) == host && (GetWindowLongPtrW(game, GWL_STYLE) & WS_CHILD)) return;
        static uint64_t lastPost = 0;  // the bridge docks asynchronously, do not flood it
        const uint64_t now = GetTickCount64();
        if (now - lastPost < 250) return;
        lastPost = now;
        PlayApplyEmbeddedChildPresentation(game);
        ++g_playRedocks;
        if (g_playRedocks <= 3 || g_playRedocks % 50 == 0)
            PlayAppendLog("[EMBED] BO3 left the BO3 Live pane (it rewrote its own window style); docked it again (" +
                          std::to_string(g_playRedocks) + ").");
    }

    // After docking BO3 must know it is not the active app (a child gets no WM_ACTIVATE(WA_INACTIVE)), so IW's IN_DeactivateMouse
    // runs: cursor shown, clip released. Hides from before the bridge's cursor hook are undone once here.
    uint64_t g_playInactiveDue = 0;  // a second pass after a dock, once the bridge knows the (now docked) window
    void PlayTellGameInactive(HWND game)
    {
        if (!game || !IsWindow(game)) return;
        if (!g_playInactiveDue) g_playInactiveDue = GetTickCount64() + 1200;
        PostMessageW(game, WM_ACTIVATEAPP, FALSE, 0);
        PostMessageW(game, WM_ACTIVATE, WA_INACTIVE, 0);
        PostMessageW(game, WM_KILLFOCUS, 0, 0);
        CURSORINFO ci = { sizeof(ci) };
        if (GetCursorInfo(&ci) && !(ci.flags & CURSOR_SHOWING))
            for (int n = 0; n < 64 && ShowCursor(TRUE) < 0; ++n) {}
        ClipCursor(nullptr);
    }

    // Hang watchdog: once a second the game window gets a WM_NULL with a short timeout, three misses write a minidump of the game
    // next to PlayTest.dll (once per game process). Stays under Windows' own 5 s hang verdict.
    uint64_t g_playHangLastProbe = 0;
    int      g_playHangMisses = 0;
    DWORD    g_playHangDumpedPid = 0;
    // The watchdog runs on its own thread (a hung docked window blocks Radiant's UI thread too). It dumps after 3 misses and 6
    // and terminates the game after [playtest] hang_kill_seconds (default 12, 0 = never).
    int         g_playHangKillSeconds = 12;
    std::thread g_playHangThread;
    std::atomic<bool> g_playHangThreadStop{false};
    // Radiant exits through ExitProcess without unloading plugins: a static std::thread must not be joinable when its destructor
    // runs (fast-fail). This guard is destroyed before the worker threads and detaches them, g_playNavThread included.
    struct PlayExitThreadGuard
    {
        ~PlayExitThreadGuard()
        {
            if (g_playHangThread.joinable()) g_playHangThread.detach();
            if (g_playNavThread.joinable()) g_playNavThread.detach();
            if (g_playLiveMatThread.joinable()) g_playLiveMatThread.detach();
            if (g_playThread.joinable()) g_playThread.detach();
        }
    } g_playExitThreadGuard;
    void PlayWriteHangDump(DWORD pid, HWND game, const wchar_t* suffix = L"", bool force = false)
    {
        if (!force && g_playHangDumpedPid == pid) return;
        g_playHangDumpedPid = pid;
        HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
        using WriteFn = BOOL (WINAPI*)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
        WriteFn write = dbghelp ? reinterpret_cast<WriteFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
        HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, pid);
        if (!write || !proc)
        {
            PlayAppendLog("[HANG] BO3 window stopped answering for 3 s; no dump (dbghelp or OpenProcess failed).");
            if (proc) CloseHandle(proc);
            return;
        }
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t name[64];
        swprintf_s(name, L"hang-%04u%02u%02u-%02u%02u%02u%s.dmp", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, suffix);
        const std::wstring path = PlayJoin(PlayThisModuleDir(), name);
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        BOOL ok = FALSE;
        if (file != INVALID_HANDLE_VALUE)
        {
            // MiniDumpWithIndirectlyReferencedMemory | WithDataSegs | WithHandleData | WithUnloadedModules | WithThreadInfo
            const int type = 0x40 | 0x1 | 0x4 | 0x20 | 0x1000;
            ok = write(proc, pid, file, type, nullptr, nullptr, nullptr);
            CloseHandle(file);
        }
        CloseHandle(proc);
        PlayAppendLog(std::string("[HANG] BO3 window (") + (game ? "found" : "missing") + ") stopped answering for 3 s; " +
                      (ok ? "dump written: " + PlayWideToUtf8(path) : "dump FAILED (Win32 " + std::to_string(GetLastError()) + ")"));
    }
    void PlayHangWatchdogThread()
    {
        DWORD lastPid = 0;
        while (!g_playHangThreadStop)
        {
            Sleep(1000);
            if (g_playHangThreadStop) break;
            const DWORD pid = g_playGamePid;
            if (!pid) { g_playHangMisses = 0; continue; }
            if (pid != lastPid) { lastPid = pid; g_playHangMisses = 0; }
            HWND game = g_playEmbeddedHwnd;
            if (!game || !IsWindow(game)) game = PlayFindGameWindow(pid);
            if (!game || g_playLaunchHeld) { g_playHangMisses = 0; continue; }   // early launch: the bridge holds the map file opens on purpose
            DWORD_PTR result = 0;
            const bool answered = SendMessageTimeoutW(game, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 300, &result) != 0;
            // the simple log: a long freeze is often a level or zone load, so say so and when it is over
            if (answered && g_playHangMisses >= 5) PlayNote("BO3 is responding again (after " + std::to_string(g_playHangMisses) + " s).");
            g_playHangMisses = answered ? 0 : g_playHangMisses + 1;
            if (g_playHangMisses == 5) PlayNote("BO3 is not responding (it may still be loading). Waiting...");
            const bool building = PlayBusy() || g_playNavRunning || g_playLiveMatRunning;   // a starved game is not a hung one
            if (g_playHangMisses == 3 && building) PlayAppendLog("[HANG] BO3 window stopped answering for 3 s while a build runs; no dump.");
            else if (g_playHangMisses == 3) PlayWriteHangDump(pid, game, L"-a");
            else if (g_playHangMisses == 6 && !building) PlayWriteHangDump(pid, game, L"-b", true);
            if (g_playHangKillSeconds > 0 && g_playHangMisses >= g_playHangKillSeconds)
            {
                HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
                PlayAppendLog("[HANG] BO3 stopped answering for " + std::to_string(g_playHangMisses) + " s: terminating it (hang_kill_seconds=" +
                              std::to_string(g_playHangKillSeconds) + "); a docked hung window freezes Radiant's UI, so this brings Radiant back." +
                              (h ? "" : " OpenProcess failed."));
                PlayNote("BO3 stopped responding for " + std::to_string(g_playHangMisses) + " s and was closed, so Radiant does not freeze with it (hang_kill_seconds in PlayTest.ini).");
                if (h) { TerminateProcess(h, 0xC0000194); WaitForSingleObject(h, 5000); CloseHandle(h); }
                g_playHangMisses = 0;
            }
        }
    }
    void PlayHangWatchdog(uint64_t)
    {
        if (!g_playHangThread.joinable()) { g_playHangThreadStop = false; g_playHangThread = std::thread(PlayHangWatchdogThread); }
    }

    bool PlayCaptureGameControls()
    {
        DWORD pid = 0;
        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            pid = g_playGamePid;
        }
        if (!pid)
        {
            PlayAppendLog("[INPUT] BO3 is not running yet.");
            return false;
        }
        HWND game = (g_playEmbeddedHwnd && IsWindow(g_playEmbeddedHwnd)) ? g_playEmbeddedHwnd : PlayFindGameWindow(pid);
        if (!game) game = PlayWaitForGameWindow(pid, 1500);
        if (!game)
        {
            PlayAppendLog("[INPUT] BO3 window was not found.");
            return false;
        }

        // In-place play: the game stays docked, the bridge gives it keyboard focus from its own thread, and PlayMirrorGameCursor hides
        // and clips the cursor from here. No pop-out, no foreground handoff, no SetWindowPos.
        if (!PlayFocusWindow(game))
            PlayAppendLog("[INPUT WARN] BO3 focus request was not confirmed; click inside the BO3 Live pane instead.");
        else
            PlayAppendLog("[INPUT] BO3 has the keyboard focus. Esc opens its menu and frees the cursor; click Radiant to edit.");
        return true;
    }

    void PlayReleaseGameControls(bool logLine = true)
    {
        if (!g_playInputCaptured && !(g_playEmbeddedHwnd && IsWindow(g_playEmbeddedHwnd))) return;
        HWND game = g_playEmbeddedHwnd;
        // The cursor itself is returned by PlayMirrorGameCursor once the focus has left the game (next tick).

        // Never a synchronous SetForegroundWindow: deactivating the game shows its cursor, which asks Radiant's window for the shape
        // while this thread waits for the deactivation (hang). Usually the user's click already made Radiant foreground, else the
        // game hands the foreground over from its own thread.
        HWND main = (HWND)(rp::host && rp::host->MainWindow ? rp::host->MainWindow(rp::self) : nullptr);
        HWND cam = (HWND)(rp::host && rp::host->CamWindow ? rp::host->CamWindow(rp::self) : nullptr);
        if (main && IsWindow(main))
        {
            if (IsIconic(main)) ShowWindowAsync(main, SW_RESTORE);
            DWORD fgPid = 0;
            if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &fgPid);
            if (fgPid == GetCurrentProcessId())
            {
                if (cam && IsWindow(cam)) SetFocus(cam); else SetFocus(main);
            }
            else if (game && IsWindow(game) && fgPid == g_playGamePid && PlayGameTakesDockRequests(game))
                PostMessageW(game, brlive::WM_GIVE_FOREGROUND, reinterpret_cast<WPARAM>(main), 0);
        }
        if (logLine) PlayAppendLog("[INPUT] Returned controls to Radiant.");
    }

    // Cursor for in-place play, applied from Radiant's thread (the docked game shares our input queue and show count, the bridge
    // swallows its own cursor calls): hidden and clipped while IW wants it hidden AND the game has the keyboard focus AND Radiant is
    // foreground, shown and free otherwise. Radiant never touches the show count itself.
    RECT g_playCursorClip = {};

    // The cursor stays visible while the game lacks the input: the docked game hides it through the input queue's show count it shares with Radiant,
    // and nothing gives it back when the game exits. After 250 ms without focus (or no game) and a hidden cursor over OUR windows the count is raised to 0.
    // Only after a docked game has run, never while a button is held. force (unload): no grace period. Silent: no mouse lines in the play log.
    void PlayEnsureCursorVisible(bool force)
    {
        static uint64_t freeSince = 0;
        static bool gaveUp = false;   // a raise found the count already >= 0: the hide is not ours, wait for a show
        static bool owed = false;     // a docked game ran since the cursor was last visible
        HWND game = g_playEmbeddedHwnd;
        const bool gameAlive = game && IsWindow(game);
        if (gameAlive) owed = true;
        const uint64_t now = GetTickCount64();
        if (!force && gameAlive && GetFocus() == game) { freeSince = 0; return; }
        if (!freeSince) freeSince = now;
        if (!force && now - freeSince < 250) return;
        CURSORINFO ci = { sizeof(ci) };
        if (!GetCursorInfo(&ci)) return;
        if (ci.flags & (CURSOR_SHOWING | CURSOR_SUPPRESSED)) { gaveUp = false; if (!gameAlive) owed = false; return; }
        if (gaveUp || !owed) return;
        if (!force && ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000)) return;
        HWND under = WindowFromPoint(ci.ptScreenPos);
        HWND root = under ? GetAncestor(under, GA_ROOT) : nullptr;
        DWORD pid = 0;
        if (!root || !GetWindowThreadProcessId(root, &pid) || pid != GetCurrentProcessId()) return;
        int count = ShowCursor(TRUE), raised = 1;
        if (count > 0) { ShowCursor(FALSE); gaveUp = true; return; }
        while (count < 0 && raised < 64) { count = ShowCursor(TRUE); ++raised; }
        if (!gameAlive) owed = false;
    }

    void PlayMirrorGameCursor()
    {
        HWND game = g_playEmbeddedHwnd;
        bool want = false, input = false;
        RECT clip = {};
        if (game && IsWindow(game) && g_playGamePresented && !PlayBusy() && (GetWindowLongPtrW(game, GWL_STYLE) & WS_CHILD))
        {
            const LONG_PTR intent = reinterpret_cast<LONG_PTR>(GetPropW(game, BRLIVE_CURSOR_PROP));
            const bool v2 = (intent & BRLIVE_CURSOR_V2) != 0;   // a bridge without BRLIVE_CURSOR_V2 has no input state
            const bool radiantFront = PlayRadiantForeground();
            input = GetFocus() == game && radiantFront && (!v2 || (intent & BRLIVE_CURSOR_ARMED));
            const uint64_t now = GetTickCount64();
            // Alt+Tab: the game dropped the input but Radiant's queue focus came back to it, so Radiant's keys would go to the game. Hand the
            // focus to the camera view (a Radiant window: the game sees a WM_KILLFOCUS and needs a click, which it does not take back).
            static uint64_t lastNudge = 0;
            if (v2 && (intent & BRLIVE_CURSOR_NEEDCLICK) && radiantFront && GetFocus() == game && now - lastNudge > 500)
            {
                lastNudge = now;
                HWND cam = (HWND)(rp::host && rp::host->CamWindow ? rp::host->CamWindow(rp::self) : nullptr);
                HWND main = (HWND)(rp::host && rp::host->MainWindow ? rp::host->MainWindow(rp::self) : nullptr);
                if (cam && IsWindow(cam)) SetFocus(cam); else if (main && IsWindow(main)) SetFocus(main);
            }
            // The game's hidden intent pulses while it plays (the count passes through 0), its menu (Esc) keeps the cursor visible.
            // v2: release once it has been visible for 400 ms, capture once hidden.
            static uint64_t hiddenUntil = 0;
            if (v2)
            {
                const bool hidden = (intent & BRLIVE_CURSOR_HIDDEN) != 0;
                const DWORD since = GetTickCount() - static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(GetPropW(game, hidden ? BRLIVE_HIDDEN_AT_PROP : BRLIVE_SHOWN_AT_PROP)));
                hiddenUntil = (input && (hidden || (g_playInputCaptured && since < 400))) ? now + 1 : 0;
            }
            else if (input && (intent & BRLIVE_CURSOR_HIDDEN)) hiddenUntil = now + 3000;
            want = input && now < hiddenUntil;
            if (want && (!GetWindowRect(game, &clip) || clip.right - clip.left < 8 || clip.bottom - clip.top < 8)) want = false;
        }
        if (game && IsWindow(game) && g_playInputProp != (input ? 1 : 0))
        {
            g_playInputProp = input ? 1 : 0;
            SetPropW(game, BRLIVE_INPUT_PROP, reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(g_playInputProp)));
        }
        if (want)
        {
            if (!g_playInputCaptured)
            {
                g_playInputCaptured = true;
                g_playCursorClip = RECT{};
            }
            if (memcmp(&clip, &g_playCursorClip, sizeof(clip)) != 0) { ClipCursor(&clip); g_playCursorClip = clip; }
        }
        else if (g_playInputCaptured)
        {
            ClipCursor(nullptr);
            g_playCursorClip = RECT{};
            g_playInputCaptured = false;
        }
        PlayEnsureCursorVisible(false);
    }

    bool PlayFocusWindow(HWND hwnd)
    {
        if (!hwnd || !IsWindow(hwnd)) return false;
        HWND root = GetAncestor(hwnd, GA_ROOT);
        if (!root) root = hwnd;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid && pid != GetCurrentProcessId())
        {
            // The game sets itself foreground from its own thread (no AttachThreadInput: shared-queue deadlocks).
            if (!PlayGameTakesDockRequests(hwnd)) return false;
            AllowSetForegroundWindow(pid);
            PostMessageW(hwnd, brlive::WM_FOCUS, 0, 0);
            for (int i = 0; i < 25 && GetForegroundWindow() != root; ++i) Sleep(20);
            return GetForegroundWindow() == root;
        }
        ShowWindow(hwnd, SW_RESTORE);
        BringWindowToTop(hwnd);
        BOOL ok = SetForegroundWindow(root);
        SetFocus(hwnd);
        return ok != FALSE || GetForegroundWindow() == root;
    }

    bool PlaySendVirtualKey(WORD vk)
    {
        INPUT in[2] = {};
        in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = vk;
        in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = vk; in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        return SendInput(2, in, sizeof(INPUT)) == 2;
    }

    bool PlaySendUnicodeText(const std::wstring& text)
    {
        for (wchar_t ch : text)
        {
            INPUT in[2] = {};
            in[0].type = INPUT_KEYBOARD; in[0].ki.wScan = ch; in[0].ki.dwFlags = KEYEVENTF_UNICODE;
            in[1].type = INPUT_KEYBOARD; in[1].ki.wScan = ch; in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            if (SendInput(2, in, sizeof(INPUT)) != 2) return false;
        }
        return true;
    }

    bool PlaySendConsoleCommand(HWND game, const std::wstring& command)
    {
        if (!PlayFocusWindow(game)) return false;
        Sleep(80);
        if (!PlaySendVirtualKey(VK_OEM_3)) return false; // ` / ~ on the standard US layout
        Sleep(100);
        if (!PlaySendUnicodeText(command)) return false;
        Sleep(40);
        if (!PlaySendVirtualKey(VK_RETURN)) return false;
        return true;
    }

    bool PlaySendWindowMessage(HWND game, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (!game || !IsWindow(game)) return false;
        DWORD_PTR result = 0;
        return SendMessageTimeoutW(game, msg, wp, lp, SMTO_ABORTIFHUNG | SMTO_NORMAL, 75, &result) != 0;  // never SMTO_BLOCK: the game may be sending to us
    }

    // Live Preview must never steal focus from Radiant while a brush is dragged: BO3 accepts keyboard window messages in the
    // background on many setups. No focus/SendInput fallback, a build that ignores them leaves Smart Play unaffected.
    bool PlaySendConsoleCommandBackground(HWND game, const std::wstring& command)
    {
        if (!game || !IsWindow(game)) return false;
        PlaySendWindowMessage(game, WM_KEYDOWN, VK_OEM_3, 0);
        PlaySendWindowMessage(game, WM_KEYUP, VK_OEM_3, 0);
        for (wchar_t ch : command)
            if (!PlaySendWindowMessage(game, WM_CHAR, (WPARAM)ch, 0)) return false;
        if (!PlaySendWindowMessage(game, WM_KEYDOWN, VK_RETURN, 0)) return false;
        if (!PlaySendWindowMessage(game, WM_KEYUP, VK_RETURN, 0)) return false;
        return true;
    }

    bool PlayResolveCurrentMap(PlayMapInfo& out, std::string& error)
    {
        std::wstring mapPath;
        if (rp::host && rp::Has(&rp::host->CurrentMapPath) && rp::host->CurrentMapPath)
        {
            wchar_t small[2048] = {};
            size_t n = rp::host->CurrentMapPath(rp::self, small, sizeof(small) / sizeof(small[0]));
            if (n && n < sizeof(small) / sizeof(small[0])) mapPath.assign(small, n);
            else if (n)
            {
                std::vector<wchar_t> big(n + 1, L'\0');
                rp::host->CurrentMapPath(rp::self, big.data(), big.size());
                mapPath.assign(big.data(), n);
            }
        }

        // fallback for older hosts without CurrentMapPath
        if (mapPath.empty())
        {
            std::wstring title;
            PlayFindRadiantWindow(&title);
            std::wstring low = PlayLower(title);
            size_t dotMap = low.find(L".map");
            if (dotMap != std::wstring::npos) mapPath = title.substr(0, dotMap + 4);
            while (!mapPath.empty() && iswspace(mapPath.front())) mapPath.erase(mapPath.begin());
            while (!mapPath.empty() && iswspace(mapPath.back())) mapPath.pop_back();
        }

        if (mapPath.empty())
        {
            error = "No map is open in Radiant. Open/save a map before Play Test.";
            return false;
        }
        for (wchar_t& c : mapPath) if (c == L'/') c = L'\\';
        std::wstring base = PlayBaseName(mapPath);
        if (base.size() < 5 || PlayLower(base) == L"unnamed.map")
        {
            error = "The current map is unnamed. Save it first.";
            return false;
        }
        if (PlayLower(base.substr(base.size() - 4)) != L".map")
        {
            error = "The current Radiant document is not a .map file.";
            return false;
        }
        std::wstring mapName = base.substr(0, base.size() - 4);
        if (mapName.size() < 2)
        {
            error = "The current map name is too short for BO3's map_source prefix convention.";
            return false;
        }
        std::wstring game = PlayGameRoot();
        if (game.empty() || !PlayFileExists(PlayJoin(game, L"BlackOps3.exe")))
        {
            error = "Could not locate the Black Ops III root. Set TA_GAME_PATH/TA_TOOLS_PATH or launch Radiant from the BO3 bin folder.";
            return false;
        }
        if (!PlayFileExists(mapPath))
        {
            // a root map path from the host should be absolute, BO3's conventional path is the last fallback
            std::wstring prefixTry = mapName.substr(0, 2);
            std::wstring derived = PlayJoin(PlayJoin(PlayJoin(game, L"map_source"), prefixTry), mapName + L".map");
            if (PlayFileExists(derived)) mapPath = derived;
        }
        if (!PlayFileExists(mapPath))
        {
            error = "Current map source was not found at " + PlayWideToUtf8(mapPath);
            return false;
        }

        std::wstring tools = PlayToolsRoot(game);
        // The BSP goes to share\raw\maps\<folder>, the map source's folder under map_source (zm, mp, cp), not the first two letters of the name.
        std::wstring prefix = mapName.substr(0, 2);
        {
            size_t slash = mapPath.find_last_of(L"\\/");
            if (slash != std::wstring::npos && slash > 0)
            {
                size_t prev = mapPath.find_last_of(L"\\/", slash - 1);
                std::wstring folder = mapPath.substr(prev == std::wstring::npos ? 0 : prev + 1, slash - (prev == std::wstring::npos ? 0 : prev + 1));
                if (!folder.empty() && PlayLower(folder) != L"map_source") prefix = folder;
            }
        }
        out.gameRoot = game;
        out.toolsRoot = tools;
        out.mapPath = mapPath;
        out.mapName = mapName;
        out.prefix = prefix;
        out.bspPath = PlayJoin(PlayJoin(PlayJoin(PlayJoin(game, L"share"), L"raw"), L"maps"), prefix + L"\\" + mapName + L".d3dbsp");
        out.usermapRoot = PlayJoin(PlayJoin(game, L"usermaps"), mapName);
        std::wstring stampDir = PlayJoin(PlayJoin(game, L"radiant"), L"betterradiant_playtest");
        CreateDirectoryW(PlayJoin(game, L"radiant").c_str(), nullptr);
        CreateDirectoryW(stampDir.c_str(), nullptr);
        out.stampPath = PlayJoin(stampDir, mapName + L".stamp");
        out.statePath = PlayJoin(stampDir, mapName + L".state");
        return true;
    }

    // Unnamed map: File/Save opens Radiant's Save As box, where picking an existing map overwrites it.
    std::wstring PlayCurrentMapPathW();
    bool PlayMapUnnamed()
    {
        if (!rp::host || !rp::Has(&rp::host->CurrentMapPath) || !rp::host->CurrentMapPath) return false;
        const std::wstring base = PlayLower(PlayBaseName(PlayCurrentMapPathW()));
        return base.empty() || base == L"unnamed.map";
    }

    bool PlaySaveCurrentMap(std::string& action)
    {
        if (PlayMapUnnamed()) { action = "none (the map is unnamed)"; return false; }
        if (rp::host && rp::Has(&rp::host->TriggerMenuAction) && rp::host->TriggerMenuAction)
        {
            int r = rp::host->TriggerMenuAction(rp::self, "File/Save");
            if (r == 1) { action = "File/Save"; return true; }
            if (r == -1) { action = "File/Save (map already clean)"; return true; }
        }
        const char* candidates[] = { "Save", "Save Map" };
        for (const char* c : candidates)
            if (TriggerActionByText(c, action)) return true;
        return false;
    }

    bool PlayIgnoredOutputExt(const std::wstring& path)
    {
        std::wstring l = PlayLower(path);
        const wchar_t* exts[] = { L".ff", L".xpak", L".pak", L".sabl", L".sabs", L".tmp", L".bak", L".log" };
        for (auto* e : exts)
            if (l.size() >= wcslen(e) && l.compare(l.size() - wcslen(e), wcslen(e), e) == 0) return true;
        return false;
    }

    uint64_t PlayNewestSourceRecursive(const std::wstring& dir, int depth = 0)
    {
        if (depth > 16 || !PlayDirExists(dir)) return 0;
        uint64_t newest = 0;
        WIN32_FIND_DATAW fd = {};
        HANDLE h = FindFirstFileW(PlayJoin(dir, L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;
        do
        {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring p = PlayJoin(dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                std::wstring n = PlayLower(fd.cFileName);
                if (n == L"zone" || n == L".git" || n == L"logs") continue;
                newest = (std::max)(newest, PlayNewestSourceRecursive(p, depth + 1));
            }
            else if (!PlayIgnoredOutputExt(p))
            {
                ULARGE_INTEGER u = {}; u.LowPart = fd.ftLastWriteTime.dwLowDateTime; u.HighPart = fd.ftLastWriteTime.dwHighDateTime;
                newest = (std::max)(newest, u.QuadPart);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return newest;
    }


    // Smart Play fingerprints: timestamps alone are not enough (Radiant can save an unchanged .map and bump its write time).
    // FNV-1a is change detection, fast enough to hash a map on every Play.
    static const uint64_t kPlayFnvOffset = 1469598103934665603ULL;
    static const uint64_t kPlayFnvPrime  = 1099511628211ULL;

    uint64_t PlayHashBytes(uint64_t h, const void* data, size_t n)
    {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= kPlayFnvPrime; }
        return h;
    }

    uint64_t PlayHashString(uint64_t h, const std::string& s)
    {
        return PlayHashBytes(h, s.data(), s.size());
    }

    bool PlayReadFileForHash(const std::wstring& path, std::string& out, size_t maxBytes = 256 * 1024 * 1024)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER li = {};
        if (!GetFileSizeEx(h, &li) || li.QuadPart < 0 || (uint64_t)li.QuadPart > maxBytes)
        { CloseHandle(h); return false; }
        out.assign((size_t)li.QuadPart, '\0');
        size_t off = 0;
        while (off < out.size())
        {
            DWORD want = (DWORD)(std::min<size_t>)(out.size() - off, 1u << 20);
            DWORD got = 0;
            if (!ReadFile(h, &out[off], want, &got, nullptr)) { CloseHandle(h); return false; }
            if (!got) break;
            off += got;
        }
        CloseHandle(h);
        out.resize(off);
        return true;
    }

    uint64_t PlayHashWholeFile(const std::wstring& path)
    {
        std::string data;
        if (!PlayReadFileForHash(path, data)) return 0;
        return PlayHashBytes(kPlayFnvOffset, data.data(), data.size());
    }

    uint64_t PlayHashGeometryText(const std::string& data)
    {
        uint64_t h = kPlayFnvOffset;
        size_t pos = 0;
        while (pos <= data.size())
        {
            size_t end = data.find('\n', pos);
            if (end == std::string::npos) end = data.size();
            size_t first = pos;
            while (first < end && (data[first] == ' ' || data[first] == '\t' || data[first] == '\r')) ++first;
            // Entity keys (origin, target, script_*) are quoted: leaving them out gives a stable geometry signature, so moving a point
            // entity is -onlyents while brush planes, patches and material lines force full.
            if (first < end && data[first] != '"')
            {
                h = PlayHashBytes(h, data.data() + first, end - first);
                const char nl = '\n'; h = PlayHashBytes(h, &nl, 1);
            }
            if (end == data.size()) break;
            pos = end + 1;
        }
        return h;
    }

    // Folds every misc_prefab entity's keys into h (a moved prefab moves compiled geometry) and lists the model paths.
    uint64_t PlayHashPrefabPlacements(const std::string& text, uint64_t h, std::vector<std::string>& models)
    {
        int depth = 0;
        bool prefab = false;
        std::string keys, model;
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            size_t first = pos, last = end;
            while (first < last && (text[first] == ' ' || text[first] == '\t' || text[first] == '\r')) ++first;
            while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t' || text[last - 1] == '\r')) --last;
            const size_t n = last - first;
            pos = end + 1;
            if (n == 1 && text[first] == '{')
            {
                if (depth++ == 0) { keys.clear(); model.clear(); prefab = false; }
                continue;
            }
            if (n == 1 && text[first] == '}')
            {
                if (depth > 0 && --depth == 0 && prefab)
                {
                    h = PlayHashString(h, keys);
                    if (!model.empty()) models.push_back(model);
                }
                continue;
            }
            if (depth != 1 || !n || text[first] != '"') continue;
            const std::string line = text.substr(first, n);
            keys += line; keys += '\n';
            const size_t keyEnd = line.find('"', 1);
            const size_t valueStart = keyEnd == std::string::npos ? keyEnd : line.find('"', keyEnd + 1);
            const size_t valueEnd = line.rfind('"');
            if (valueStart == std::string::npos || valueEnd <= valueStart) continue;
            const std::string key = line.substr(1, keyEnd - 1), value = line.substr(valueStart + 1, valueEnd - valueStart - 1);
            if (!_stricmp(key.c_str(), "classname")) prefab = !_stricmp(value.c_str(), "misc_prefab");
            else if (!_stricmp(key.c_str(), "model")) model = value;
        }
        return h;
    }

    // The prefab files a map places, nested ones included.
    struct PlayPrefabScan
    {
        uint64_t all = kPlayFnvOffset;        // every prefab file's bytes
        uint64_t geometry = kPlayFnvOffset;   // their brush lines and every misc_prefab's keys
        uint64_t newest = 0;                  // newest write time
        int files = 0, missing = 0, placements = 0;
        std::map<std::string, std::pair<uint64_t, uint64_t>> perFile;   // model path -> bytes hash, geometry hash
    };

    void PlayScanPrefabFile(const std::wstring& mapSource, const std::string& model, int depth, PlayPrefabScan& scan)
    {
        std::string key = model;
        for (char& c : key) c = c == '\\' ? '/' : (char)tolower((unsigned char)c);
        if (key.empty() || depth > 8 || scan.perFile.count(key)) return;
        const std::wstring file = PlayJoin(mapSource, PlayUtf8ToWide(key));
        std::string text;
        if (!PlayReadFileForHash(file, text)) { scan.perFile[key] = { 0, 0 }; ++scan.missing; return; }
        ++scan.files;
        scan.newest = (std::max)(scan.newest, PlayFileTime(file));
        std::vector<std::string> nested;
        const uint64_t geometry = PlayHashPrefabPlacements(text, PlayHashGeometryText(text), nested);
        scan.perFile[key] = { PlayHashBytes(kPlayFnvOffset, text.data(), text.size()), geometry };
        text = std::string();
        for (const std::string& n : nested) PlayScanPrefabFile(mapSource, n, depth + 1, scan);
    }

    PlayPrefabScan PlayScanPrefabs(const std::wstring& mapSource, const std::string& mapText)
    {
        PlayPrefabScan scan;
        std::vector<std::string> models;
        scan.geometry = PlayHashPrefabPlacements(mapText, scan.geometry, models);
        scan.placements = (int)models.size();
        for (const std::string& model : models) PlayScanPrefabFile(mapSource, model, 0, scan);
        for (const auto& f : scan.perFile)
        {
            scan.all = PlayHashString(scan.all, f.first);
            scan.all = PlayHashBytes(scan.all, &f.second.first, sizeof(f.second.first));
            scan.geometry = PlayHashString(scan.geometry, f.first);
            scan.geometry = PlayHashBytes(scan.geometry, &f.second.second, sizeof(f.second.second));
        }
        return scan;
    }

    bool PlayHashTreeFileAllowed(const std::wstring& path)
    {
        if (PlayIgnoredOutputExt(path)) return false;
        std::wstring low = PlayLower(path);
        const wchar_t* exts[] = {
            L".gsc", L".csc", L".gsh", L".zone", L".csv", L".zpkg", L".lua", L".cfg",
            L".txt", L".json", L".szc", L".alias", L".menu", L".inc", L".h", L".hpp"
        };
        for (const wchar_t* ext : exts)
        {
            size_t n = wcslen(ext);
            if (low.size() >= n && low.compare(low.size() - n, n, ext) == 0) return true;
        }
        return false;
    }

    // all: every source file of the usermap. headers: its .gsh files alone (a header edit recompiles every script).
    struct PlayTreeHash { uint64_t all = kPlayFnvOffset, headers = kPlayFnvOffset; };

    PlayTreeHash PlayHashSourceTreeRecursive(const std::wstring& dir, const std::wstring& relative = L"", int depth = 0)
    {
        if (depth > 16 || !PlayDirExists(dir)) return {};
        struct Item { std::wstring name; WIN32_FIND_DATAW fd; };
        std::vector<Item> items;
        WIN32_FIND_DATAW fd = {};
        HANDLE f = FindFirstFileW(PlayJoin(dir, L"*").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) return {};
        do
        {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            items.push_back({fd.cFileName, fd});
        } while (FindNextFileW(f, &fd));
        FindClose(f);
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b){ return PlayLower(a.name) < PlayLower(b.name); });

        PlayTreeHash h;
        for (const Item& item : items)
        {
            std::wstring lowerName = PlayLower(item.name);
            std::wstring rel = relative.empty() ? item.name : PlayJoin(relative, item.name);
            std::wstring full = PlayJoin(dir, item.name);
            if (item.fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (lowerName == L"zone" || lowerName == L".git" || lowerName == L"logs" || lowerName == L"assetinfo") continue;
                const PlayTreeHash child = PlayHashSourceTreeRecursive(full, rel, depth + 1);
                std::string key = PlayWideToUtf8(PlayLower(rel));
                h.all = PlayHashString(h.all, key);
                h.all = PlayHashBytes(h.all, &child.all, sizeof(child.all));
                h.headers = PlayHashString(h.headers, key);
                h.headers = PlayHashBytes(h.headers, &child.headers, sizeof(child.headers));
            }
            else if (PlayHashTreeFileAllowed(full))
            {
                std::string key = PlayWideToUtf8(PlayLower(rel));
                ULARGE_INTEGER sz = {}; sz.LowPart = item.fd.nFileSizeLow; sz.HighPart = item.fd.nFileSizeHigh;
                // Source files are usually tiny: content hashing is immune to copied files with preserved timestamps. Very large ones use metadata.
                uint64_t fileHash = 0;
                if (sz.QuadPart <= 64ULL * 1024ULL * 1024ULL) fileHash = PlayHashWholeFile(full);
                else
                {
                    ULARGE_INTEGER ft = {}; ft.LowPart = item.fd.ftLastWriteTime.dwLowDateTime; ft.HighPart = item.fd.ftLastWriteTime.dwHighDateTime;
                    fileHash = ft.QuadPart;
                }
                const bool header = lowerName.size() >= 4 && lowerName.compare(lowerName.size() - 4, 4, L".gsh") == 0;
                for (uint64_t* part : { &h.all, header ? &h.headers : nullptr })
                {
                    if (!part) continue;
                    *part = PlayHashString(*part, key);
                    *part = PlayHashBytes(*part, &sz.QuadPart, sizeof(sz.QuadPart));
                    *part = PlayHashBytes(*part, &fileHash, sizeof(fileHash));
                }
            }
        }
        return h;
    }

    // Size and write time of every file under dir with one of the extensions (lower case, with the dot).
    uint64_t PlayHashMetadataRecursive(const std::wstring& dir, const std::vector<std::wstring>& exts, uint64_t* newest = nullptr,
                                       const std::wstring& relative = L"", int depth = 0)
    {
        if (depth > 12 || !PlayDirExists(dir)) return kPlayFnvOffset;
        struct Item { std::wstring name; WIN32_FIND_DATAW fd; };
        std::vector<Item> items;
        WIN32_FIND_DATAW fd = {};
        HANDLE f = FindFirstFileW(PlayJoin(dir, L"*").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) return kPlayFnvOffset;
        do
        {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            items.push_back({fd.cFileName, fd});
        } while (FindNextFileW(f, &fd));
        FindClose(f);
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b){ return PlayLower(a.name) < PlayLower(b.name); });
        uint64_t h = kPlayFnvOffset;
        for (const Item& item : items)
        {
            std::wstring rel = relative.empty() ? item.name : PlayJoin(relative, item.name);
            std::wstring full = PlayJoin(dir, item.name);
            if (item.fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                uint64_t child = PlayHashMetadataRecursive(full, exts, newest, rel, depth + 1);
                h = PlayHashBytes(h, &child, sizeof(child));
                continue;
            }
            std::wstring low = PlayLower(item.name);
            bool wanted = false;
            for (const std::wstring& ext : exts)
                if (low.size() >= ext.size() && low.compare(low.size() - ext.size(), ext.size(), ext) == 0) { wanted = true; break; }
            if (!wanted) continue;
            std::string key = PlayWideToUtf8(PlayLower(rel));
            h = PlayHashString(h, key);
            ULARGE_INTEGER sz = {}; sz.LowPart = item.fd.nFileSizeLow; sz.HighPart = item.fd.nFileSizeHigh;
            ULARGE_INTEGER ft = {}; ft.LowPart = item.fd.ftLastWriteTime.dwLowDateTime; ft.HighPart = item.fd.ftLastWriteTime.dwHighDateTime;
            if (newest && ft.QuadPart > *newest) *newest = ft.QuadPart;
            h = PlayHashBytes(h, &sz.QuadPart, sizeof(sz.QuadPart));
            h = PlayHashBytes(h, &ft.QuadPart, sizeof(ft.QuadPart));
        }
        return h;
    }

    struct PlayBuildFingerprint
    {
        bool valid = false;
        uint64_t mapAll = 0;
        uint64_t mapGeometry = 0;
        uint64_t userSources = 0;
        uint64_t gdtMetadata = 0;
        bool extended = false;         // the state file had the four keys below
        uint64_t prefabsAll = 0;
        uint64_t prefabsGeometry = 0;
        uint64_t userHeaders = 0;
        uint64_t sharedScripts = 0;
    };

    bool PlayLoadFingerprint(const std::wstring& path, PlayBuildFingerprint& fp)
    {
        fp = {};
        std::string text;
        if (!PlayReadFileForHash(path, text, 64 * 1024)) return false;
        unsigned long long a=0,b=0,c=0,d=0;
        if (sscanf(text.c_str(), "map_all=%llx map_geometry=%llx user_sources=%llx gdt_metadata=%llx", &a,&b,&c,&d) != 4)
            return false;
        fp.valid = true; fp.mapAll = (uint64_t)a; fp.mapGeometry = (uint64_t)b; fp.userSources = (uint64_t)c; fp.gdtMetadata = (uint64_t)d;
        auto value = [&](const char* key, uint64_t& out)
        {
            const size_t at = text.find(key);
            unsigned long long v = 0;
            if (at == std::string::npos || sscanf(text.c_str() + at + strlen(key), "%llx", &v) != 1) return false;
            out = (uint64_t)v;
            return true;
        };
        const bool prefabs = value("prefabs_all=", fp.prefabsAll) && value("prefabs_geometry=", fp.prefabsGeometry);
        fp.extended = prefabs && value("user_headers=", fp.userHeaders) && value("shared_scripts=", fp.sharedScripts);
        return true;
    }

    bool PlaySaveFingerprint(const std::wstring& path, const PlayBuildFingerprint& fp)
    {
        char buf[512] = {};
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "map_all=%016llx\r\nmap_geometry=%016llx\r\nuser_sources=%016llx\r\ngdt_metadata=%016llx\r\n"
                    "prefabs_all=%016llx\r\nprefabs_geometry=%016llx\r\nuser_headers=%016llx\r\nshared_scripts=%016llx\r\n",
                    (unsigned long long)fp.mapAll, (unsigned long long)fp.mapGeometry,
                    (unsigned long long)fp.userSources, (unsigned long long)fp.gdtMetadata,
                    (unsigned long long)fp.prefabsAll, (unsigned long long)fp.prefabsGeometry,
                    (unsigned long long)fp.userHeaders, (unsigned long long)fp.sharedScripts);
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD wrote = 0; BOOL ok = WriteFile(h, buf, (DWORD)strlen(buf), &wrote, nullptr); CloseHandle(h);
        return ok != FALSE;
    }

    // Team Create's Hide box: while ticked the host's address never shows in the play log. PlayHideIpTick (UI tick) keeps the list
    // of addresses to hide, g_playMutex guards it.
    bool g_playHideIp = false;
    std::vector<std::string> g_playHiddenIps;
    std::string PlayMaskIpsLocked(std::string text)
    {
        if (!g_playHideIp) return text;
        for (const std::string& ip : g_playHiddenIps)
        {
            if (ip.empty()) continue;
            for (size_t at = text.find(ip); at != std::string::npos; at = text.find(ip, at))
            {
                text.replace(at, ip.size(), "(host IP hidden)");
                at += 16;
            }
        }
        return text;
    }

    // Simple and verbose log: PlayNote lines are the plain story of a Play (timestamped, in both texts), PlayAppendLog lines are the
    // detail (tool output, engine state, RVAs, verbose only), PlaySetError reaches both. The pane's "Verbose log" box ([playtest]
    // verbose_log) picks the one shown, "PlayTest log" returns the verbose text, "PlayTest log simple" the other. RadiantPlugins.log gets both.
    std::atomic<bool> g_playVerboseLog{false};
    std::string       g_playSimpleText;   // under g_playMutex

    void PlayTrimLogLocked(std::string& text)
    {
        if (text.size() <= 400000) return;
        size_t cut = text.find('\n', text.size() - 300000);
        text.erase(0, cut == std::string::npos ? text.size() - 300000 : cut + 1);
    }

    void PlayClearLogLocked()
    {
        g_playLogText.clear(); g_playSimpleText.clear(); g_playPendingLog.clear();
    }

    // "[01:17:33.005] " for RadiantPlugins.log, which stamps a line when the UI tick flushes it
    std::string PlayLogStamp()
    {
        SYSTEMTIME st; GetLocalTime(&st);
        char stamp[24]; snprintf(stamp, sizeof stamp, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        return stamp;
    }

    void PlayAppendLog(const std::string& rawLine)
    {
        const std::string stamp = PlayLogStamp();
        std::lock_guard<std::mutex> lock(g_playMutex);
        const std::string line = PlayMaskIpsLocked(rawLine);
        g_playPendingLog.push_back(stamp + line);
        g_playLogText += line;
        if (g_playLogText.empty() || g_playLogText.back() != '\n') g_playLogText += '\n';
        PlayTrimLogLocked(g_playLogText);
    }

    // One plain-language line for the simple log (and the verbose one, marked ">>").
    void PlayNote(const std::string& text)
    {
        SYSTEMTIME st; GetLocalTime(&st);
        char stamp[16]; snprintf(stamp, sizeof stamp, "%02u:%02u:%02u  ", st.wHour, st.wMinute, st.wSecond);
        std::lock_guard<std::mutex> lock(g_playMutex);
        const std::string line = stamp + PlayMaskIpsLocked(text);
        g_playPendingLog.push_back(PlayLogStamp() + ">> " + line);
        g_playSimpleText += line + "\n";
        g_playLogText += ">> " + line + "\n";
        PlayTrimLogLocked(g_playSimpleText);
        PlayTrimLogLocked(g_playLogText);
    }

    // "[JOIN] Could not ..." -> "Could not ..." for the simple log
    std::string PlayStripTag(const std::string& text)
    {
        if (text.empty() || text[0] != '[') return text;
        const size_t close = text.find("] ");
        return close == std::string::npos || close > 24 ? text : text.substr(close + 2);
    }

    // "8 s", "2 min 05 s"
    std::string PlayDuration(uint64_t ms)
    {
        const uint64_t s = (ms + 500) / 1000;
        char buf[32];
        if (s < 60) snprintf(buf, sizeof buf, "%llu s", (unsigned long long)s);
        else snprintf(buf, sizeof buf, "%llu min %02llu s", (unsigned long long)(s / 60), (unsigned long long)(s % 60));
        return buf;
    }

    // Long waits: a step that can take a while (compile, link, light build, BO3 loading, a live zone) is opened with PlayWaitBegin and
    // closed with PlayWaitEnd. PlayWaitTick (UI tick) notes "still ..." after 15 s, every 15 s up to a minute, then every 30 s.
    // giveUpMs: a wait on the game is dropped with a note after that. Any thread.
    struct PlayWait { std::string what; uint64_t since = 0, next = 0, giveUp = 0; };
    std::map<std::string, PlayWait> g_playWaits;   // key -> wait, under g_playMutex

    void PlayWaitBegin(const std::string& key, const std::string& what, uint64_t giveUpMs = 0)
    {
        const uint64_t now = GetTickCount64();
        std::lock_guard<std::mutex> lock(g_playMutex);
        PlayWait& w = g_playWaits[key];
        w.what = what; w.since = now; w.next = now + 15000; w.giveUp = giveUpMs;
    }

    // Elapsed ms of the wait it closes, 0 when none was open under that key.
    uint64_t PlayWaitEnd(const std::string& key)
    {
        std::lock_guard<std::mutex> lock(g_playMutex);
        auto it = g_playWaits.find(key);
        if (it == g_playWaits.end()) return 0;
        const uint64_t ms = (std::max)(1ull, GetTickCount64() - it->second.since);
        g_playWaits.erase(it);
        return ms;
    }

    // New text for an open wait, its start time kept (the duration reported at the end stays whole).
    void PlayWaitRename(const std::string& key, const std::string& what)
    {
        std::lock_guard<std::mutex> lock(g_playMutex);
        auto it = g_playWaits.find(key);
        if (it != g_playWaits.end()) it->second.what = what;
    }

    bool PlayWaitOpen(const std::string& key)
    {
        std::lock_guard<std::mutex> lock(g_playMutex);
        return g_playWaits.count(key) != 0;
    }

    void PlayWaitTick(uint64_t now)
    {
        std::vector<std::string> due;
        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            for (auto it = g_playWaits.begin(); it != g_playWaits.end(); )
            {
                PlayWait& w = it->second;
                if (w.giveUp && now - w.since >= w.giveUp)
                {
                    due.push_back("Gave up " + w.what + " after " + PlayDuration(now - w.since) + ". Verbose log has the detail.");
                    it = g_playWaits.erase(it);
                    continue;
                }
                if (now >= w.next)
                {
                    due.push_back("Still " + w.what + " (" + PlayDuration(now - w.since) + ")...");
                    w.next = now + (now - w.since < 59000 ? 15000 : 30000);
                }
                ++it;
            }
        }
        for (const std::string& line : due) PlayNote(line);
    }

    void PlaySetError(const std::string& error)
    {
        { std::lock_guard<std::mutex> lock(g_playMutex); g_playLastError = error; }
        PlayNote("ERROR: " + PlayStripTag(error));
        g_playState = PLAY_FAILED;
    }

    std::wstring PlayQuote(const std::wstring& a)
    {
        if (a.empty()) return L"\"\"";
        if (a.find_first_of(L" \t\"") == std::wstring::npos) return a;
        std::wstring out = L"\"";
        size_t slashes = 0;
        for (wchar_t ch : a)
        {
            if (ch == L'\\') { ++slashes; continue; }
            if (ch == L'\"') { out.append(slashes * 2 + 1, L'\\'); out += L'\"'; slashes = 0; continue; }
            out.append(slashes, L'\\'); slashes = 0; out += ch;
        }
        out.append(slashes * 2, L'\\'); out += L'\"';
        return out;
    }

    // The linker's exit code is useless (2001000 on a good base link, 0x80000003 on a fatal one) but it prints "UNRECOVERABLE ERROR"
    // before terminating: the last captured run remembers whether that line went by (a failed link's fastfiles froze the game).
    std::atomic<bool> g_playRunSawFatal{false};
    thread_local bool g_playRunBelowNormal = false;   // live material links run while the game plays: below-normal priority keeps the game's frames
    thread_local std::atomic<bool>* g_playRunJobCancel = nullptr;   // a background job's own cancel flag, checked beside the Play cancel
    bool PlayRunCancelled() { return g_playCancel || (g_playRunJobCancel && g_playRunJobCancel->load()); }
    bool PlayLastRunFatal() { return g_playRunSawFatal.load(); }
    // The last captured run's error lines: the simple log hides tool output, so a failed step names them.
    thread_local std::vector<std::string> g_playRunErrors;
    thread_local bool g_playRunQuiet = false;   // no [RUN], output or [EXIT] lines (errors are still kept)
    // The last captured run's linker problems for the simple log, by kind (names), and the repeated lines left out.
    struct PlayRunIssues { std::set<std::string> weapons, materials, effects, images; size_t other = 0, repeats = 0; };
    thread_local PlayRunIssues g_playRunIssues;
    // "^1x" -> "ERROR: x" (or x when it already says ERROR), "^3x" -> "WARN: x", other color codes dropped.
    std::string PlayToolLine(const std::string& line, int& color)
    {
        color = 0;
        if (line.size() < 2 || line[0] != '^' || line[1] < '0' || line[1] > '9') return line;
        color = line[1] - '0';
        const std::string rest = line.substr(2);
        if (color == 1) return rest.rfind("ERROR", 0) == 0 ? rest : "ERROR: " + rest;
        if (color == 3) return rest.rfind("WARN", 0) == 0 ? rest : "WARN: " + rest;
        return rest;
    }
    std::string PlayQuotedAfter(const std::string& line, const std::string& lead, char close)
    {
        const size_t a = line.find(lead);
        if (a == std::string::npos) return std::string();
        const size_t b = line.find(close, a + lead.size());
        return b == std::string::npos ? std::string() : line.substr(a + lead.size(), b - a - lead.size());
    }
    void PlayRunIssueLine(const std::string& line)
    {
        PlayRunIssues& r = g_playRunIssues;
        if (line.find("no techset for surface") != std::string::npos) return;   // follows from a missing material
        if (line.find("Unable to load weapon '") != std::string::npos) { r.weapons.insert(PlayQuotedAfter(line, "Unable to load weapon '", '\'')); return; }
        if (line.find(" was not found in gdtDB") != std::string::npos && line.find("Material ") != std::string::npos)
        { const std::string n = PlayQuotedAfter(line, "Material ", ' '); r.materials.insert(n.empty() ? "(no name)" : n); return; }
        if (line.find("Effect '") != std::string::npos && line.find("' not found") != std::string::npos) { r.effects.insert(PlayQuotedAfter(line, "Effect '", '\'')); return; }
        if (line.find("image '") != std::string::npos && line.find("' is missing") != std::string::npos) { r.images.insert(PlayQuotedAfter(line, "image '", '\'')); return; }
        ++r.other;
    }
    bool PlayRunErrorLine(const std::string& line)
    {
        if (line.find("ERROR") != std::string::npos || line.find("Error:") != std::string::npos || line.find("error:") != std::string::npos) return true;
        return line.find("UNRECOVERABLE") != std::string::npos || line.find("failed to compile") != std::string::npos;
    }
    void PlayNote(const std::string& text);
    // One simple-log line for the linker problems of a link that still wrote its fastfiles.
    void PlayNoteRunIssues()
    {
        const PlayRunIssues& r = g_playRunIssues;
        std::string text;
        auto add = [&](const std::set<std::string>& names, const char* one, const char* many)
        {
            if (names.empty()) return;
            if (!text.empty()) text += ", ";
            text += std::to_string(names.size()) + " " + (names.size() == 1 ? one : many);
            if (names.size() > 3) return;
            std::string list;
            for (const std::string& n : names) list += (list.empty() ? "" : ", ") + n;
            text += " (" + list + ")";
        };
        add(r.weapons, "weapon did not load", "weapons did not load");
        add(r.materials, "material missing", "materials missing");
        add(r.effects, "effect missing", "effects missing");
        add(r.images, "image missing", "images missing");
        if (r.other) text += (text.empty() ? "" : ", ") + std::to_string(r.other) + " other error(s)";
        if (!text.empty()) PlayNote("The link reported problems: " + text + ". Verbose log lists them.");
    }
    void PlayNoteRunErrors(const char* tool)
    {
        if (g_playRunErrors.empty()) { PlayNote(std::string(tool) + " printed no error line. Turn on Verbose log to see its full output."); return; }
        PlayNote(std::string(tool) + " reported:");
        for (const std::string& e : g_playRunErrors) PlayNote("    " + e);
        PlayNote("Turn on Verbose log to see the full output.");
    }
    std::wstring PlayCommandLine(const std::wstring& exe, const std::vector<std::wstring>& args)
    {
        std::wstring cmd = PlayQuote(exe);
        for (const auto& a : args) { cmd += L" "; cmd += PlayQuote(a); }
        return cmd;
    }

    std::wstring PlayThisModuleDir();

    // Launcher Plus only hooks modlauncher.exe: Play Test builds use its wrappers directly under the same switches its launcher bridge
    // reads (LauncherPlus row in plugins.ini, [launcherplus] enabled, the tool's enabled= in LauncherPlus.ini, wrappers BuildPlus*.exe,
    // the old plugins\BetterBuild install as fallback). Contract: "<wrapper>" --real "<tool>" -- <args>, the tool's exit code and output, kill-on-close job.
    bool PlayBetterBuildRoute(std::wstring& exe, std::vector<std::wstring>& args, const wchar_t* section, const wchar_t* wrapperName)
    {
        const std::wstring self = PlayThisModuleDir();
        const size_t slash = self.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return false;
        const std::wstring pluginsDir = self.substr(0, slash);
        const std::wstring pluginsIni = pluginsDir + L"\\plugins.ini";
        auto usable = [&](const wchar_t* row, const wchar_t* folder, const wchar_t* master, const std::wstring& file, std::wstring& out) {
            const std::wstring dir = pluginsDir + L"\\" + folder;
            const std::wstring ini = dir + L"\\" + folder + L".ini";
            if (GetPrivateProfileIntW(row, L"enabled", 0, pluginsIni.c_str()) == 0) return false;
            if (GetPrivateProfileIntW(master, L"enabled", 1, ini.c_str()) == 0) return false;
            if (GetPrivateProfileIntW(section, L"enabled", 1, ini.c_str()) == 0) return false;
            out = dir + L"\\" + file;
            return PlayFileExists(out);
        };
        std::wstring renamed = wrapperName;   // BetterBuildWorker.exe -> BuildPlusWorker.exe, BetterBuildLinker.exe -> BuildPlusLinker.exe
        if (renamed.rfind(L"BetterBuild", 0) == 0) renamed = L"BuildPlus" + renamed.substr(11);
        std::wstring wrapper;
        if (!usable(L"LauncherPlus", L"LauncherPlus", L"launcherplus", renamed, wrapper) &&
            !usable(L"BetterBuild", L"BetterBuild", L"betterbuild", wrapperName, wrapper))
            return false;
        std::vector<std::wstring> wrapped = { L"--real", exe, L"--" };
        wrapped.insert(wrapped.end(), args.begin(), args.end());
        exe = wrapper;
        args.swap(wrapped);
        return true;
    }

    int PlayRunCaptured(const std::wstring& exe, const std::vector<std::wstring>& args, const std::wstring& cwd, const char* label)
    {
        g_playRunSawFatal = false;
        g_playRunErrors.clear();
        g_playRunIssues = PlayRunIssues();
        if (PlayRunCancelled()) return ERROR_CANCELLED;
        if (!g_playRunQuiet) PlayAppendLog(std::string("[RUN] ") + label + ": " + PlayWideToUtf8(PlayCommandLine(exe, args)));
        SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
        HANDLE readPipe = nullptr, writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return (int)GetLastError();
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
        // FORCEOFFFEEDBACK: no "app starting" cursor over the game for each tool the live material link starts
        STARTUPINFOW si = {}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES | STARTF_FORCEOFFFEEDBACK;
        si.hStdOutput = writePipe; si.hStdError = writePipe; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi = {};
        std::wstring cmd = PlayCommandLine(exe, args);
        std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end()); mutableCmd.push_back(L'\0');
        BOOL ok = CreateProcessW(exe.c_str(), mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | (g_playRunBelowNormal ? BELOW_NORMAL_PRIORITY_CLASS : 0), nullptr,
                                 cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
        CloseHandle(writePipe);
        if (!ok)
        {
            DWORD e = GetLastError(); CloseHandle(readPipe);
            PlayAppendLog(std::string("[FAIL] ") + label + " could not start (Win32 " + std::to_string(e) + ")");
            return (int)e;
        }
        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            g_playToolProcess = pi.hProcess;
        }
        CloseHandle(pi.hThread);
        std::string carry;
        bool exited = false;
        // A tool's message seen before (numbers aside) is left out with its indented context lines (the linker repeats one per use).
        std::set<std::string> seen;
        bool skipContext = false;
        while (!exited)
        {
            if (PlayRunCancelled()) TerminateProcess(pi.hProcess, ERROR_CANCELLED);
            DWORD available = 0;
            if (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available)
            {
                char buf[4096]; DWORD got = 0;
                DWORD want = (std::min)(available, (DWORD)(sizeof(buf) - 1));
                if (ReadFile(readPipe, buf, want, &got, nullptr) && got)
                {
                    carry.append(buf, buf + got);
                    size_t pos = 0;
                    while ((pos = carry.find_first_of("\r\n")) != std::string::npos)
                    {
                        std::string line = carry.substr(0, pos);
                        size_t eat = 1;
                        if (pos + 1 < carry.size() && carry[pos] == '\r' && carry[pos + 1] == '\n') eat = 2;
                        carry.erase(0, pos + eat);
                        int color = 0;
                        if (!line.empty()) line = PlayToolLine(line, color);
                        if (!line.empty() && line[0] == ' ' && skipContext) continue;
                        if (!line.empty() && line[0] != ' ') skipContext = false;
                        if (color)
                        {
                            std::string key = line;
                            for (char& ch : key) if (ch >= '0' && ch <= '9') ch = '#';
                            if (!seen.insert(key).second) { ++g_playRunIssues.repeats; skipContext = true; continue; }
                            if (color == 1) PlayRunIssueLine(line);
                        }
                        if (!line.empty())
                        {
                            if (!g_playRunQuiet) PlayAppendLog(line);
                            if (PlayRunErrorLine(line) && g_playRunErrors.size() < 6)   // the first ones name the cause
                                g_playRunErrors.push_back(line.size() > 300 ? line.substr(0, 300) + "..." : line);
                            if (line.find("UNRECOVERABLE ERROR") != std::string::npos || line.find("shaders failed to compile") != std::string::npos) g_playRunSawFatal = true;
                        }
                    }
                }
            }
            exited = WaitForSingleObject(pi.hProcess, 25) == WAIT_OBJECT_0;
        }
        for (;;)
        {
            DWORD available = 0;
            if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) || !available) break;
            char buf[4096]; DWORD got = 0;
            DWORD want = (std::min)(available, (DWORD)sizeof(buf));
            if (!ReadFile(readPipe, buf, want, &got, nullptr) || !got) break;
            carry.append(buf, buf + got);
        }
        if (!carry.empty() && !g_playRunQuiet) PlayAppendLog(carry);
        CloseHandle(readPipe);
        DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            if (g_playToolProcess == pi.hProcess) g_playToolProcess = nullptr;
        }
        CloseHandle(pi.hProcess);
        if (g_playRunIssues.repeats) PlayAppendLog("[" + std::string(label) + "] " + std::to_string(g_playRunIssues.repeats) + " repeated message(s) left out (same text as an earlier one, numbers aside).");
        if (!g_playRunQuiet) PlayAppendLog(std::string("[EXIT] ") + label + " -> " + std::to_string(code));
        return (int)code;
    }

    bool PlayTouchStamp(const std::wstring& path, const std::wstring& mapName)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        std::string text = "Play Test link stamp: " + PlayWideToUtf8(mapName) + "\r\n";
        DWORD wrote = 0; WriteFile(h, text.data(), (DWORD)text.size(), &wrote, nullptr); CloseHandle(h);
        return true;
    }


    bool PlayEnsureGameJob()
    {
        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            if (g_playGameJob) return true;
        }

        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (!job)
        {
            PlayAppendLog("[PROCESS WARN] CreateJobObject failed (" + std::to_string(GetLastError()) + "); BO3 exit will use the normal process-handle fallback.");
            return false;
        }

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info)))
        {
            const DWORD err = GetLastError();
            CloseHandle(job);
            PlayAppendLog("[PROCESS WARN] Could not enable JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE (" + std::to_string(err) + ").");
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            if (!g_playGameJob) g_playGameJob = job;
            else CloseHandle(job);
        }
        return true;
    }

    bool PlayAssignGameToJob(HANDLE process, DWORD pid)
    {
        if (!process || !pid || !PlayEnsureGameJob()) return false;
        HANDLE job = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            job = g_playGameJob;
        }
        if (!job) return false;
        if (AssignProcessToJobObject(job, process))
        {
            PlayAppendLog("[PROCESS] BO3 pid " + std::to_string(pid) + " attached to Radiant kill-on-close job.");
            g_playLiveZoneLaunchMs = GetTickCount64();   // the live zone DLL waits for the window plus 20 s (an injection during the startup stub kills the game)
            return true;
        }
        const DWORD err = GetLastError();
        // ERROR_ACCESS_DENIED can mean the process is already in an incompatible job: keep the existing tracking handle and shutdown fallback.
        PlayAppendLog("[PROCESS WARN] Could not attach BO3 pid " + std::to_string(pid) +
                      " to Radiant kill-on-close job (Win32 " + std::to_string(err) + ").");
        return false;
    }

    void PlayCloseGameJob()
    {
        HANDLE job = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            job = g_playGameJob;
            g_playGameJob = nullptr;
        }
        if (job) CloseHandle(job); // KILL_ON_JOB_CLOSE terminates every process still assigned.
    }

    bool PlayTrackedGameRunning(DWORD* pidOut = nullptr)
    {
        std::lock_guard<std::mutex> lock(g_playMutex);
        if (g_playGameProcess && WaitForSingleObject(g_playGameProcess, 0) == WAIT_TIMEOUT)
        {
            if (pidOut) *pidOut = g_playGamePid;
            return true;
        }
        return false;
    }

    bool PlayAdoptExistingBo3(const PlayMapInfo& m, bool forceReload, bool presentGame = true)
    {
        if (!g_playKeepWarm) return false;

        DWORD existingPid = PlayFindBo3ProcessId();
        if (!existingPid) return false;

        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            if (g_playGameProcess && g_playGamePid == existingPid &&
                WaitForSingleObject(g_playGameProcess, 0) == WAIT_TIMEOUT)
                return true;
        }

        bool canAssignJob = true;
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, existingPid);
        if (!process)
        {
            // keep the warm-session behavior even if Windows denies the extra job-assignment rights
            canAssignJob = false;
            process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, existingPid);
        }
        if (!process)
        {
            PlayAppendLog("[WARM WARN] Found an existing BlackOps3.exe process but could not open a tracking handle. Refusing to launch a second BO3 instance.");
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            if (g_playGameProcess) CloseHandle(g_playGameProcess);
            g_playGameProcess = process;
            g_playGamePid = existingPid;
            g_playGameOwnedByRadiant = false;
        }
        if (canAssignJob) PlayAssignGameToJob(process, existingPid);

        HWND game = PlayFindGameWindow(existingPid);
        if (!game) game = PlayWaitForGameWindow(existingPid, 3000);
        if (!game)
        {
            PlayAppendLog("[WARM WARN] Adopted the already-running BO3 process, but its game window is not ready yet. No second process will be started.");
            g_playGamePresented = presentGame;
            g_playPrewarmed = !presentGame;
            g_playPrewarmWindowHidden = !presentGame;
            g_playState = presentGame ? PLAY_RUNNING : PLAY_IDLE;
            return true;
        }

        const std::string mapUtf8 = PlayWideToUtf8(m.mapName);
        PlayAppendLog("[WARM] Found an already-running BlackOps3.exe (pid " + std::to_string(existingPid) + "). Adopting it instead of starting a second Steam session.");
        PlayAppendLog(PlayBuildModeLog(PlayCurrentExeBuild(true)) + " (read from the file on disk; a rename after the launch is not seen)");

        // A restarted Radiant forgets which map the warm BO3 owns: re-issue devmap once when adopting.
        if (forceReload || g_playGameMap.empty() || _stricmp(g_playGameMap.c_str(), mapUtf8.c_str()) != 0)
        {
            const std::wstring command = L"/devmap " + m.mapName;
            PlayAppendLog("[WARM] Synchronizing adopted BO3 session: " + PlayWideToUtf8(command));
            const bool delivered = presentGame
                ? PlaySendConsoleCommand(game, command)
                : PlaySendConsoleCommandBackground(game, command);
            if (!delivered)
            {
                PlayAppendLog("[WARM WARN] Could not deliver the automatic devmap command to the adopted BO3 window. Keep the existing game open and launch the map from its menu/console; Play Test will not start a duplicate process.");
                if (presentGame) PlayFocusWindow(game); else ShowWindow(game, SW_HIDE);
                g_playGamePresented = presentGame;
                g_playPrewarmed = !presentGame;
                g_playPrewarmWindowHidden = !presentGame;
                g_playState = presentGame ? PLAY_RUNNING : PLAY_IDLE;
                return true;
            }
            g_playGameMap = mapUtf8;
        }

        if (presentGame) PlayFocusWindow(game); else ShowWindow(game, SW_HIDE);
        g_playGamePresented = presentGame;
        g_playPrewarmed = !presentGame;
        g_playPrewarmWindowHidden = !presentGame;
        g_playState = presentGame ? PLAY_RUNNING : PLAY_IDLE;
        return true;
    }

    // ---- Docked play video settings ----
    // config.ini holds FullScreenMode, AutoRestoreFullscreen and WindowX/Y/Size ("+set" does not change them). A docked launch writes the pane's geometry and
    // windowed mode there, the originals go back after the game exits, at shutdown, or before the next launch (state in radiant\betterradiant_playtest).
    std::string PlayReadTextFile(const std::wstring& path, size_t maxBytes);
    bool PlayWriteTextFile(const std::wstring& path, const std::string& text);
    std::wstring g_playVideoGameRoot;
    std::wstring PlayVideoStatePath(const std::wstring& gameRoot) { return PlayJoin(PlayJoin(PlayJoin(gameRoot, L"radiant"), L"betterradiant_playtest"), L"video_restore.ini"); }
    std::wstring PlayVideoConfigPath(const std::wstring& gameRoot) { return PlayJoin(PlayJoin(gameRoot, L"players"), L"config.ini"); }
    // Sets `Key = "value"` lines in place, returns the values they had. Lines keep their CRLF and comments.
    std::map<std::string, std::string> PlayVideoConfigSet(const std::wstring& ini, const std::map<std::string, std::string>& values)
    {
        std::map<std::string, std::string> originals;
        std::string text = PlayReadTextFile(ini, 8 * 1024 * 1024);
        if (text.empty()) return originals;
        std::string out; out.reserve(text.size() + 64);
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t eol = text.find('\n', pos);
            std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos + 1);
            pos = eol == std::string::npos ? text.size() : eol + 1;
            for (auto& kv : values)
            {
                const std::string head = kv.first + " = \"";
                if (line.compare(0, head.size(), head) != 0) continue;
                size_t q = line.find('"', head.size());
                if (q == std::string::npos) break;
                originals[kv.first] = line.substr(head.size(), q - head.size());
                line = head + kv.second + line.substr(q);
                break;
            }
            out += line;
        }
        if (originals.size() == values.size()) PlayWriteTextFile(ini, out);
        return originals;
    }
    void PlayVideoConfigRestore(const std::wstring& gameRoot, const char* why)
    {
        if (gameRoot.empty()) return;
        const std::wstring state = PlayVideoStatePath(gameRoot);
        if (!PlayFileExists(state)) return;
        std::map<std::string, std::string> saved;
        std::istringstream in(PlayReadTextFile(state, 65536));
        std::string line;
        while (std::getline(in, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            size_t eq = line.find('=');
            if (eq != std::string::npos && eq > 0) saved[line.substr(0, eq)] = line.substr(eq + 1);
        }
        if (!saved.empty()) PlayVideoConfigSet(PlayVideoConfigPath(gameRoot), saved);
        DeleteFileW(state.c_str());
        PlayAppendLog(std::string("[EMBED] players\\config.ini video settings restored (") + why + ").");
    }
    void PlayVideoConfigApply(const PlayMapInfo& m, const RECT& pane)
    {
        PlayVideoConfigRestore(m.gameRoot, "left over from an earlier session");
        const long pw = pane.right - pane.left, ph = pane.bottom - pane.top;
        if (pw < 64 || ph < 64) return;
        std::map<std::string, std::string> values = {
            { "FullScreenMode", "0" }, { "AutoRestoreFullscreen", "0" },
            { "WindowX", std::to_string(pane.left) }, { "WindowY", std::to_string(pane.top) },
            { "WindowSize", std::to_string(pw) + "x" + std::to_string(ph) } };
        auto originals = PlayVideoConfigSet(PlayVideoConfigPath(m.gameRoot), values);
        if (originals.size() != values.size())
        { PlayAppendLog("[EMBED WARN] players\\config.ini could not be updated for docked play (keys not found); the game keeps its own video mode."); return; }
        std::string state;
        for (auto& kv : originals) state += kv.first + "=" + kv.second + "\r\n";
        CreateDirectoryW(PlayJoin(PlayJoin(m.gameRoot, L"radiant"), L"betterradiant_playtest").c_str(), nullptr);
        PlayWriteTextFile(PlayVideoStatePath(m.gameRoot), state);
        g_playVideoGameRoot = m.gameRoot;
        PlayAppendLog("[EMBED] players\\config.ini set for docked play: windowed, no fullscreen auto-restore, window " + std::to_string(pw) + "x" + std::to_string(ph) + " at " + std::to_string(pane.left) + "," + std::to_string(pane.top) + " (original values restored when the game exits).");
    }

    // Play From Here: the Radiant camera reaches the game as dvars br_play_cam "x y z pitch yaw" (game angles), br_play_mode walk|fly|off and
    // br_play_gen, on a cold launch line or through the zone module mailbox. The marked GSC receiver (PlayConfigurePlayHere) moves the
    // player whenever br_play_gen changes, so the map GSC stays byte-identical between Plays.
    const char* PlayHereMode() { return g_playCamDvar.empty() ? "off" : (g_playFly ? "fly" : "walk"); }
    std::vector<std::string> PlayHereConsoleSets()
    {
        std::vector<std::string> sets;
        // an automatic reload (live bake) leaves the place to the game (it wrote the player's own into br_play_cam)
        if (!g_playKeepPos) { sets.push_back("set br_play_cam \"" + g_playCamDvar + "\""); sets.push_back(std::string("set br_play_mode ") + PlayHereMode()); }
        sets.push_back("set br_play_gen " + std::to_string(g_playCamGen));
        return sets;
    }
    void PlayHereLaunchArgs(std::vector<std::wstring>& args)
    {
        args.insert(args.end(), { L"+set", L"br_play_cam", PlayUtf8ToWide(g_playCamDvar),
                                  L"+set", L"br_play_mode", PlayUtf8ToWide(PlayHereMode()),
                                  L"+set", L"br_play_gen", std::to_wstring(g_playCamGen) });
    }

    // ---- Dvars window: what Play passes to the game ----
    std::string PlayTrim(std::string s);
    PlayDvars PlayDvarsCopy() { std::lock_guard<std::mutex> lock(g_playMutex); return g_playDvars; }
    struct PlayDvarSet { std::string name, value; bool coldOnly; };
    std::vector<PlayDvarSet> PlayDvarSets(const PlayDvars& d)
    {
        std::vector<PlayDvarSet> out;
        out.push_back({ "developer", std::to_string(d.developer), false });
        if (d.splitscreen) { out.push_back({ "splitscreen", "1", true }); out.push_back({ "splitscreen_playerCount", "2", true }); }
        out.push_back({ "logfile", std::to_string(d.logfile), false });
        size_t i = 0;
        while (i < d.custom.size())
        {
            size_t e = d.custom.find(';', i);
            if (e == std::string::npos) e = d.custom.size();
            std::string part = PlayTrim(d.custom.substr(i, e - i));
            i = e + 1;
            if (part.empty()) continue;
            const size_t sp = part.find_first_of(" \t");
            std::string value = sp == std::string::npos ? std::string() : PlayTrim(part.substr(sp + 1));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
            out.push_back({ part.substr(0, sp), value, false });
        }
        return out;
    }
    // Split screen takes effect only at game start: a change makes the next Play relaunch BO3.
    std::string PlayDvarColdKey(const PlayDvars& d)
    {
        std::string key;
        for (const auto& s : PlayDvarSets(d)) if (s.coldOnly) key += s.name + "=" + s.value + ";";
        return key;
    }
    // Cold launch: developer replaces the fixed one, the rest go before +devmap.
    void PlayDvarLaunchArgs(std::vector<std::wstring>& args, const PlayDvars& d)
    {
        std::vector<std::wstring> add;
        for (const auto& s : PlayDvarSets(d))
        {
            if (s.name == "developer")
            {
                for (size_t i = 0; i + 2 < args.size(); ++i)
                    if (args[i] == L"+set" && args[i + 1] == L"developer") { args[i + 2] = PlayUtf8ToWide(s.value); break; }
                continue;
            }
            add.insert(add.end(), { L"+set", PlayUtf8ToWide(s.name), PlayUtf8ToWide(s.value) });
        }
        size_t at = args.size();
        for (size_t i = 0; i < args.size(); ++i) if (args[i] == L"+devmap") { at = i; break; }
        args.insert(args.begin() + at, add.begin(), add.end());
    }
    // Warm reload: the same values as console lines, sent before the devmap or map_restart.
    std::vector<std::string> PlayDvarConsoleLines(const PlayDvars& d)
    {
        std::vector<std::string> lines;
        for (const auto& s : PlayDvarSets(d))
            if (!s.coldOnly) lines.push_back("set " + s.name + " \"" + s.value + "\"");
        return lines;
    }

    int PlayLiveMaterialsRemoveZones(const PlayMapInfo& m);
    std::wstring PlayThisModuleDir();
    std::wstring PlayJoinFile(const wchar_t* name) { std::wstring dir = PlayThisModuleDir(); return dir.empty() ? std::wstring() : PlayJoin(dir, name); }

    // BO3 loading the map: from a launch or level reload until the bridge reports the level running (engineWorldActive). A reload must show
    // the old level going first: engineWorldActive at 0, or its ShutdownGame hook count moving past shutdownBase (read before the reload).
    // Only BO3 Enhanced with the bridge reports it, elsewhere one note stands in. PlayLoadWaitTick (UI tick) closes the wait.
    std::atomic<bool> g_playLoadWatch{false};
    std::atomic<bool> g_playLoadSawInactive{false};
    std::atomic<LONG> g_playLoadShutdownBase{-1};
    bool PlayLoadProgressAvailable()
    {
        return PlayCurrentExeBuild() == PLAY_BUILD_ENHANCED && (g_playLivePreview || g_playLiveZone || g_playFromCamera) &&
               PlayFileExists(PlayJoin(PlayThisModuleDir(), L"BO3LiveBridge.dll"));
    }
    // The bridge's ShutdownGame count, -1 when its lifecycle hooks are not in (no bridge, another build).
    LONG PlayLoadShutdownCount()
    {
        brlive::SharedState* sh = g_playLiveShared;
        if (!sh || g_playLiveBridgePid != g_playGamePid || !sh->lifecycleTraceInstalled) return -1;
        return sh->lifecycleShutdownCallCount;
    }
    LONG PlayLoadInitCount()
    {
        brlive::SharedState* sh = g_playLiveShared;
        if (!sh || g_playLiveBridgePid != g_playGamePid || !sh->lifecycleTraceInstalled) return -1;
        return sh->lifecycleInitCallCount;
    }
    void PlayLoadWaitBegin(const std::string& what, LONG shutdownBase = -2)   // -2: a launch, no old level to see go
    {
        if (!PlayLoadProgressAvailable() || shutdownBase == -1)   // a reload the bridge cannot follow
        {
            g_playLoadWatch = false;
            PlayWaitEnd("load");
            PlayNote(what + ". The game shows the map when it is ready.");
            return;
        }
        g_playLoadSawInactive = shutdownBase == -2;   // a launch has no old level, the first worldActive is the new map
        g_playLoadShutdownBase = shutdownBase;
        g_playLoadWatch = true;
        PlayWaitBegin("load", "waiting for BO3 to load the map", 6 * 60000);
        PlayNote(what + "...");
    }

    void PlayParkRememberFps();
    bool PlayLaunchGame(const PlayMapInfo& m, bool prewarm = false)
    {
        if (g_playCancel) return false;
        // a join request is only for the game this launch starts in join mode (a stale one would make any game join)
        const std::wstring joinReq = PlayJoinFile(L"BO3LiveJoin.req");
        if (!joinReq.empty()) DeleteFileW(joinReq.c_str());
        { const std::wstring st = PlayJoinFile(L"BO3LiveJoin.status"); if (!st.empty()) DeleteFileW(st.c_str()); }   // the last session's answer
        g_playJoinSeq.clear();
        const bool join = g_playJoinMode && !prewarm;
        g_playMidLevelZoneLoads = 0;
        {
            const int removed = PlayLiveMaterialsRemoveZones(m);   // the last session's live material and nav refresh zones (their materials are in the patch zone now)
            if (removed) PlayAppendLog("[TEXTURED] " + std::to_string(removed) + " stale live material / nav zone item(s) removed.");
        }

        // Steam rejects a second BlackOps3.exe on the same account (e.g. Radiant restarted while Keep Warm left BO3 running): always
        // discover and adopt the resident process before CreateProcessW.
        DWORD existingPid = PlayFindBo3ProcessId();
        if (existingPid)
        {
            // Background prewarm must never take ownership of, focus, hide or /devmap a BO3 that appeared outside this session (this also closes
            // the race with PlayTickPrewarm's check). Explicit Play may adopt it.
            if (prewarm)
            {
                PlayAppendLog("[PREWARM] BlackOps3.exe is already running outside the tracked hot session; leaving it untouched and skipping background launch.");
                g_playState = PLAY_IDLE;
                return false;
            }
            if (join)
            {
                PlaySetError("BlackOps3.exe is already running. Close it (Close BO3) before Join Host: the joining game starts at its menu.");
                return false;
            }
            if (g_playKeepWarm && PlayAdoptExistingBo3(m, true, true))
            {
                PlayAppendLog("[WARM] Reused the resident BO3 process; duplicate Steam launch avoided.");
                PlayNote("Using the BO3 that is already running (a second copy cannot start under the same Steam account).");
                return true;
            }
            PlaySetError("BlackOps3.exe is already running. Play Test refused to start a second copy because Steam will reject the duplicate session. Close the existing game or enable Keep BO3 warm.");
            return false;
        }

        if (!g_playKeepWarm && g_playKillPrevious)
        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            if (g_playGameProcess)
            {
                if (WaitForSingleObject(g_playGameProcess, 0) == WAIT_TIMEOUT) TerminateProcess(g_playGameProcess, 0);
                CloseHandle(g_playGameProcess); g_playGameProcess = nullptr; g_playGamePid = 0; g_playGameOwnedByRadiant = false;
            }
        }
        std::wstring exe = PlayJoin(m.gameRoot, L"BlackOps3.exe");
        std::vector<std::wstring> args = {
            L"+set", L"con_enable", L"1",
            L"+set", L"developer", L"1",
            L"+set", L"developer_script", L"1",
            L"+set", L"scr_mod_enable_devblock", L"1",   // /# #/ blocks run in a usermap only with this: the live preview receiver draws with line()
            L"+set", L"fs_game", m.mapName,
            L"+devmap", m.mapName
        };
        {
            const PlayDvars dv = PlayDvarsCopy();
            PlayDvarLaunchArgs(args, dv);
            std::lock_guard<std::mutex> lock(g_playMutex);
            g_playLaunchColdKey = PlayDvarColdKey(dv);
        }
        // A Team Create session plays over LAN (bridge/join.cpp): lobby_forceOffline 1 makes "press any key" start LAN play (BeginLANPlay)
        // instead of signing in to the online services (BeginLivePlay).
        if (g_playLanWanted && !prewarm)   // before +devmap: set before the level (and any later menu) starts
        {
            size_t at = args.size();
            for (size_t i = 0; i < args.size(); ++i) if (args[i] == L"+devmap") { at = i; break; }
            args.insert(args.begin() + at, { L"+set", L"lobby_forceOffline", L"1" });
        }
        if (join)
        {
            // to the menu, discovery aimed at the host: the bridge's join module finds and joins its LAN game
            for (size_t i = 0; i + 1 < args.size(); ++i)
                if (args[i] == L"+devmap") { args.erase(args.begin() + i, args.begin() + i + 2); break; }
            args.push_back(L"+set"); args.push_back(L"net_broadcast_ip"); args.push_back(PlayUtf8ToWide(g_playJoinHost));
        }
        else PlayHereLaunchArgs(args);
        PlayAppendLog(PlayBuildModeLog(PlayCurrentExeBuild(true)));
        if (g_playEmbedGame)
        {
            // a background start (parked) may find the pane closed: windowed at a stand-in size, the dock fits it later
            RECT pane = PlayGamePaneScreenRect();
            if ((prewarm || g_playLaunchHeld) && (pane.right - pane.left < 64 || pane.bottom - pane.top < 64)) pane = RECT{ 100, 100, 1380, 820 };
            PlayVideoConfigApply(m, pane);
        }
        std::wstring cmd = PlayCommandLine(exe, args);
        if (!g_playExtraArgs.empty()) { cmd += L" "; cmd += PlayUtf8ToWide(g_playExtraArgs); }
        PlayAppendLog("[RUN] BlackOps3: " + PlayWideToUtf8(cmd));
        STARTUPINFOW si = {}; si.cb = sizeof(si); PROCESS_INFORMATION pi = {};
        if (prewarm)
        {
            // Hidden initial show state for BO3: some fullscreen paths ignore STARTUPINFO, so PlayTickPrewarm also hides the real HWND as soon as it appears.
            si.dwFlags |= STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
        }
        std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end()); mutableCmd.push_back(L'\0');
        // BlackOps3.start is a 0-byte marker the game creates at startup and deletes on a clean exit. A killed game leaves it and the next
        // launch sits in a "Safe mode" MessageBox before its window. No BO3 process exists here.
        DeleteFileW(PlayJoin(m.gameRoot, L"BlackOps3.start").c_str());
        // SteamAppId in the child's environment: without it the game exits after a few seconds and relaunches itself through Steam as a
        // process Play Test never owns (no docking, no kill-on-close).
        std::vector<wchar_t> env;
        if (wchar_t* block = GetEnvironmentStringsW())
        {
            for (wchar_t* p = block; *p; p += wcslen(p) + 1)
            {
                // Radiant runs inside Steam's mod tools session (app 455130): inherited Steam* variables describe it, and a game started with them
                // plus a different app id quit with code 0 at level start.
                if (_wcsnicmp(p, L"Steam", 5) == 0) continue;
                env.insert(env.end(), p, p + wcslen(p) + 1);
            }
            FreeEnvironmentStringsW(block);
            const wchar_t* extra[] = { L"SteamAppId=311210" };
            for (auto* e : extra) env.insert(env.end(), e, e + wcslen(e) + 1);
            env.push_back(L'\0');
        }
        BOOL ok = CreateProcessW(exe.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | (env.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT),
                                 env.empty() ? nullptr : env.data(), m.gameRoot.c_str(), &si, &pi);
        if (!ok)
        {
            const DWORD err = GetLastError();
            if (prewarm)
            {
                PlayAppendLog("[PREWARM WARN] BlackOps3.exe could not start in the background (Win32 " + std::to_string(err) + "). Smart Play can still launch it normally when requested.");
                g_playState = PLAY_IDLE;
            }
            else
                PlaySetError("BlackOps3.exe could not start (Win32 " + std::to_string(err) + ").");
            return false;
        }

        // Attach before the first game instruction runs: the kernel owns the job, so even a hard Radiant exit terminates BO3.
        PlayAssignGameToJob(pi.hProcess, pi.dwProcessId);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            if (g_playGameProcess && g_playGameProcess != pi.hProcess) CloseHandle(g_playGameProcess);
            g_playGameProcess = pi.hProcess; g_playGamePid = pi.dwProcessId; g_playGameOwnedByRadiant = true;
        }
        g_playGameMap = PlayWideToUtf8(m.mapName);
        g_playPrewarmMap = g_playGameMap;
        // A background start is a parked game: normal docking path, hidden and muted until Play opens the pane.
        g_playGamePresented = true;
        g_playPrewarmed = prewarm;
        g_playPrewarmWindowHidden = false;
        g_playParked = prewarm;
        g_playParkCmdsSent = false;
        g_playParkMuted = false;
        g_playParkMuteNextMs = 0;
        if (prewarm) { g_playParkGameRoot = m.gameRoot; PlayParkRememberFps(); }
        g_playState = PLAY_RUNNING;
        if (join)
        {
            // the request for the bridge's join module (read on BO3's main frame once a second)
            char seq[32]; snprintf(seq, sizeof seq, "%lu", (unsigned long)(GetTickCount64() / 1000 % 2000000000ull) + 1);
            const std::wstring wmap = m.mapName, whost = PlayUtf8ToWide(g_playJoinHost);
            bool ok = !joinReq.empty() &&
                      WritePrivateProfileStringW(L"join", L"host", whost.c_str(), joinReq.c_str()) &&
                      WritePrivateProfileStringW(L"join", L"map", wmap.c_str(), joinReq.c_str()) &&
                      WritePrivateProfileStringW(L"join", L"usermap_entry", g_playJoinUsermapEntry ? L"1" : L"0", joinReq.c_str()) &&
                      WritePrivateProfileStringW(L"join", L"seq", PlayUtf8ToWide(seq).c_str(), joinReq.c_str());
            g_playJoinStatusSeen.clear();
            if (ok) g_playJoinSeq = seq;
            PlayNote(PlayLiveEditBuild() && ok ? "BO3 is starting at its menu; it joins the host's game by itself once the menu is up."
                                                : "BO3 is starting at its menu. Join by hand: Play Local > Find LAN Game, then the host's game.");
            if (PlayLiveEditBuild())
                PlayAppendLog(std::string("[JOIN] BO3 launched to its menu; ") + (ok ? "the bridge joins the host's game at " + g_playJoinHost + " once the menu is up (BO3LiveJoin.log next to PlayTest.dll)."
                                                                             : "the join request could not be written: open Play Local > Find LAN Game and pick the host."));
            else
                PlayAppendLog("[JOIN] BO3 launched to its menu. This BlackOps3.exe is not BO3 Enhanced, so join by hand: Play Local > Find LAN Game, then the host's game (the search goes to " + g_playJoinHost + " only). On the Steam build the bridge adds the map to the usermap list for the join's mod check; no Live Edit in this game.");
            return true;
        }
        if (prewarm)
        {
            PlayAppendLog("[PREWARM] BO3 is starting " + PlayWideToUtf8(m.mapName) + " in the background, parked (hidden and muted). Play shows it and reloads only what changed.");
            PlayNote("BO3 is starting in the background (hidden and muted), so the next Play only has to show it.");
        }
        else
        {
            PlayAppendLog("[OK] BO3 launched directly into " + PlayWideToUtf8(m.mapName) + ". Keep Warm is " + (g_playKeepWarm ? std::string("ON") : std::string("OFF")) + ".");
            if (!g_playLaunchHeld) PlayLoadWaitBegin("Starting BO3 and loading " + PlayWideToUtf8(m.mapName));   // an early launch notes its own load after the build
        }
        return true;
    }

    // Live zone: a console command for the game through the zone module's mailbox (BO3LiveZone.cmd next to BO3LiveBridge.dll, queued
    // through Cbuf_InsertText inside the game, no keystrokes or focus change).
    std::wstring PlayThisModuleDir();
    bool PlayLiveZoneSendCommands(const std::vector<std::string>& commands)
    {
        if (!g_playLiveZoneInjected) return false;
        std::wstring dir = PlayThisModuleDir();
        if (dir.empty()) return false;
        // Appended: two sends inside one 250 ms poll of the module would keep only the second.
        FILE* f = _wfopen(PlayJoin(dir, L"BO3LiveZone.cmd").c_str(), L"a");
        if (!f) return false;
        for (const std::string& c : commands) { fputs(c.c_str(), f); fputs("\n", f); }
        fclose(f);
        return true;
    }
    bool PlayLiveZoneSendCommand(const std::string& command) { return PlayLiveZoneSendCommands({ command }); }
    // Worker thread only: the DLL deletes the mailbox once it queued the lines (polls every 250 ms). A file still there after 2 s means
    // the DLL is inert in this game (another build, "unexpected build: nothing done" in BO3LiveZone.log): take it back, use another path.
    bool PlayLiveZoneSendCommandsConfirmed(const std::vector<std::string>& commands)
    {
        if (!PlayLiveZoneSendCommands(commands)) return false;
        const std::wstring mailbox = PlayJoin(PlayThisModuleDir(), L"BO3LiveZone.cmd");
        for (int i = 0; i < 20; ++i)
        {
            Sleep(100);
            if (!PlayFileExists(mailbox)) return true;
        }
        DeleteFileW(mailbox.c_str());
        PlayAppendLog("[WARM WARN] the zone module did not take the command mailbox within 2 s (see BO3LiveZone.log).");
        return false;
    }

    std::vector<std::string> PlayUnparkCommands(DWORD pid, const std::wstring& gameRoot);
    bool PlayWarmUpdateGame(const PlayMapInfo& m, bool buildChanged)
    {
        DWORD pid = 0;
        // Keep Warm only decides whether Stop leaves BO3 resident and whether an untracked BO3 may be adopted. A game this session
        // launched and still tracks is reloaded in place either way.
        if (!PlayTrackedGameRunning(&pid))
        {
            // Radiant may have restarted while BO3 stayed warm: recover that session instead of CreateProcessW (Steam's duplicate-login fatal error).
            if (PlayAdoptExistingBo3(m, true, true)) return true;
            return false;
        }
        const std::string mapUtf8 = PlayWideToUtf8(m.mapName);
        if (!g_playGameMap.empty() && _stricmp(g_playGameMap.c_str(), mapUtf8.c_str()) != 0)
        {
            PlayAppendLog("[WARM] A different map is loaded in the tracked BO3 process; doing a clean relaunch for the new fs_game.");
            return false;
        }
        const PlayDvars dvars = PlayDvarsCopy();
        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            if (PlayDvarColdKey(dvars) != g_playLaunchColdKey)
            {
                PlayAppendLog("[WARM] The split screen settings changed (they apply only at game start): BO3 is relaunched.");
                return false;
            }
        }
        HWND game = (g_playEmbeddedHwnd && IsWindow(g_playEmbeddedHwnd)) ? g_playEmbeddedHwnd : PlayFindGameWindow(pid);
        if (!game) game = PlayWaitForGameWindow(pid, 3000);
        if (!game)
        {
            PlayAppendLog("[WARM] BO3 is running but its game window was not found; falling back to a clean relaunch.");
            return false;
        }
        if (g_playEmbedGame)
            PlayAppendLog("[EMBED] BO3 is warm; Radiant will attach its window to the dedicated BO3 Live pane on the main UI tick.");
        const bool wasParked = g_playParked;
        std::vector<std::string> unpark;
        if (wasParked) unpark = PlayUnparkCommands(pid, m.gameRoot);
        if (!buildChanged)
        {
            g_playGamePresented = true;
            g_playPrewarmed = false;
            g_playPrewarmWindowHidden = false;
            PlayFocusWindow(game);
            g_playState = PLAY_RUNNING;
            if (wasParked)
            {
                // a parked level ran on hidden (AI off, god on): a fresh start of it, no fastfile reload
                std::vector<std::string> cmds = unpark;
                for (auto& c : PlayDvarConsoleLines(dvars)) cmds.push_back(c);
                for (auto& c : PlayHereConsoleSets()) cmds.push_back(c);
                cmds.push_back("map_restart");
                if (PlayLiveZoneSendCommands(cmds))
                {
                    PlayAppendLog("[PARK] BO3 was parked and nothing changed: the map restarts in the running game.");
                    PlayNote("Nothing changed: BO3 comes back and the map restarts.");
                }
                else
                {
                    PlayAppendLog("[PARK] BO3 was parked and is still loading its level; it is shown as it is.");
                    PlayNote("BO3 is still loading the map; it is shown as it is.");
                }
                return true;
            }
            PlayAppendLog("[HOT PLAY] Nothing changed. The already-prewarmed BO3 runtime was presented immediately with zero compile/link/engine startup work.");
            PlayNote("Nothing changed: showing the running game.");
            PlayLiveZoneSendCommands(PlayDvarConsoleLines(dvars));   // the changed values, set live
            if (!g_playCamDvar.empty())
            {
                if (PlayLiveZoneSendCommands(PlayHereConsoleSets()))
                    PlayAppendLog(std::string("[PLAY HERE] the player is moved to the Radiant camera (") + PlayHereMode() + ").");
                else
                    PlayAppendLog("[PLAY HERE WARN] the zone module is not running in the game (it starts 20 s after a launch, and only on a build it knows); the player stays where they are.");
            }
            return true;
        }

        if (const int zones = g_playMidLevelZoneLoads.load())
        {
            if (!g_playWarmAfterLiveZone)
            {
                PlayAppendLog("[WARM] " + std::to_string(zones) + " live zone(s) were loaded into this level and warm_after_live_zone=0: BO3 is relaunched instead of reloaded.");
                return false;
            }
            PlayAppendLog("[WARM] " + std::to_string(zones) + " live zone(s) were loaded into this level; the bridge unloads them with the level (BO3LiveZone.log: 'relabelled').");
        }
        const std::wstring command = L"/devmap " + m.mapName;
        std::vector<std::string> reload = unpark;   // a parked game's settings back first (empty otherwise)
        for (auto& c : PlayDvarConsoleLines(dvars)) reload.push_back(c);
        for (auto& c : PlayHereConsoleSets()) reload.push_back(c);   // the dvars first, in the same mailbox file, then the reload
        reload.push_back("devmap " + PlayWideToUtf8(m.mapName));
        const LONG shutdownBase = PlayLoadShutdownCount();   // before the reload can run
        if (PlayLiveZoneSendCommandsConfirmed(reload))
            PlayAppendLog("[WARM] BO3 is already running; reload queued through the zone module: devmap " + PlayWideToUtf8(m.mapName) +
                          (g_playCamDvar.empty() ? "" : std::string(" (spawn at the Radiant camera, ") + PlayHereMode() + ")"));
        else if (!PlaySendConsoleCommand(game, command))
        {
            PlayAppendLog("[WARM WARN] Automatic console reload could not be delivered. A clean relaunch will be used instead.");
            return false;
        }
        g_playGameMap = mapUtf8;
        g_playGamePresented = true;
        g_playPrewarmed = false;
        g_playPrewarmWindowHidden = false;
        g_playState = PLAY_RUNNING;
        PlayAppendLog("[WARM] Reload command sent. BO3 stays open, so engine/Steam startup is skipped.");
        PlayLoadWaitBegin("Reloading the map in the running BO3 (no restart needed)", shutdownBase);
        return true;
    }

    enum PlayCompileMode { PLAY_COMPILE_NONE = 0, PLAY_COMPILE_ONLYENTS, PLAY_COMPILE_FULL };

    const char* PlayCompileModeName(PlayCompileMode m)
    {
        switch (m) { case PLAY_COMPILE_ONLYENTS: return "onlyents"; case PLAY_COMPILE_FULL: return "full"; default: return "none"; }
    }

    // ---- Live zone ([playtest] live_zone=1) ----
    // Every file-backed asset newer than the base fastfile is linked as the usermap "<map>_patch" and hardlinked into usermaps\<map>\zone, the bridge's zone
    // module makes the engine load it after the base zone. The world stays in the base, so geometry edits relink the base.
    std::string PlayReadTextFile(const std::wstring& path, size_t maxBytes);
    std::string PlayReadFileTail(const std::wstring& path, size_t maxBytes);
    bool PlayWriteTextFile(const std::wstring& path, const std::string& text);
    std::string PlayLowerAscii(std::string s);

    bool PlayEnsureJunction(const PlayMapInfo& m, const std::wstring& link, const std::wstring& target)
    {
        if (PlayDirExists(link)) return true;
        wchar_t sys[MAX_PATH] = {}; GetSystemDirectoryW(sys, MAX_PATH);
        std::vector<std::wstring> args = { L"/c", L"mklink", L"/J", link, target };
        g_playRunQuiet = true;
        PlayRunCaptured(PlayJoin(sys, L"cmd.exe"), args, m.toolsRoot, "Live zone junction");
        g_playRunQuiet = false;
        if (PlayDirExists(link)) return true;
        for (const std::string& e : g_playRunErrors) PlayAppendLog("[LIVE ZONE] mklink: " + e);
        return false;
    }

    std::wstring PlayBaseZonePath(const PlayMapInfo& m) { return PlayJoin(PlayJoin(m.usermapRoot, L"zone_source"), m.mapName + L".zone"); }

    int PlayLiveZoneRemovePatchFiles(const PlayMapInfo& m)
    {
        const std::wstring patchName = m.mapName + L"_patch", mapZoneDir = PlayJoin(m.usermapRoot, L"zone");
        int removed = 0;
        for (auto* ext : { L".ff", L".xpak" })
            for (auto& pre : { std::wstring(L""), std::wstring(L"en_") })
                if (DeleteFileW(PlayJoin(mapZoneDir, pre + patchName + ext).c_str())) ++removed;
        return removed;
    }

    // Live materials: materials applied in Radiant that the level's fastfiles lack. The bridge reports them (materialState 2), PlayTest appends
    // them to a list the patch zone links (material,<name>), kept until the file is deleted. The list and last patch zone source live in
    // radiant\betterradiant_playtest\, not usermaps (every usermaps\<map>_* folder showed up as a new map).
    std::wstring PlayStateDir(const PlayMapInfo& m) { return PlayJoin(PlayJoin(m.gameRoot, L"radiant"), L"betterradiant_playtest"); }
    std::wstring PlayLiveMaterialsPath(const PlayMapInfo& m)
    {
        const std::wstring path = PlayJoin(PlayStateDir(m), m.mapName + L"_live_materials.txt");
        if (!PlayFileExists(path))   // carried over from usermaps\<map>_patch
        {
            const std::wstring old = PlayJoin(PlayJoin(PlayJoin(m.gameRoot, L"usermaps"), m.mapName + L"_patch"), L"live_materials.txt");
            if (PlayFileExists(old)) { CreateDirectoryW(PlayStateDir(m).c_str(), nullptr); MoveFileW(old.c_str(), path.c_str()); }
        }
        return path;
    }
    std::wstring PlayPatchZoneCopyPath(const PlayMapInfo& m) { return PlayJoin(PlayStateDir(m), m.mapName + L"_patch.zone"); }
    std::vector<std::string> PlayReadLiveMaterials(const PlayMapInfo& m)
    {
        std::vector<std::string> out;
        std::istringstream in(PlayReadTextFile(PlayLiveMaterialsPath(m), 1024 * 1024));
        std::string line;
        while (std::getline(in, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            size_t s = line.find_first_not_of(" \t"); if (s == std::string::npos || line[s] == '#' || line[s] == ';') continue;
            line.erase(0, s);
            if (std::find(out.begin(), out.end(), line) == out.end()) out.push_back(line);
        }
        return out;
    }
    bool PlayAddLiveMaterial(const PlayMapInfo& m, const std::string& name)
    {
        std::vector<std::string> have = PlayReadLiveMaterials(m);
        if (std::find(have.begin(), have.end(), name) != have.end()) return true;
        CreateDirectoryW(PlayStateDir(m).c_str(), nullptr);
        std::string text;
        for (auto& h : have) text += h + "\r\n";
        text += name + "\r\n";
        return PlayWriteTextFile(PlayLiveMaterialsPath(m), text);
    }

    // A mod source folder under usermaps that links against the map's own sources: zone folders of its own, every other
    // folder of the map junctioned in (scripts, sound, gamedata, source_data, ...).
    void PlayPrepareModSource(const PlayMapInfo& m, const std::wstring& modRoot)
    {
        CreateDirectoryW(modRoot.c_str(), nullptr);
        for (auto* d : { L"zone", L"zone_source", L"zone_source\\all", L"zone_source\\english", L"zone_source\\loc" })
            CreateDirectoryW(PlayJoin(modRoot, d).c_str(), nullptr);
        WIN32_FIND_DATAW fd = {};
        HANDLE h = FindFirstFileW(PlayJoin(m.usermapRoot, L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        size_t made = 0;
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring n = PlayLower(fd.cFileName);
            if (n == L"zone" || n == L"zone_source") continue;
            const std::wstring link = PlayJoin(modRoot, fd.cFileName);
            const bool existed = PlayDirExists(link);
            if (!PlayEnsureJunction(m, link, PlayJoin(m.usermapRoot, fd.cFileName)))
                PlayAppendLog("[LIVE ZONE WARN] could not junction " + PlayWideToUtf8(fd.cFileName) + " into " + PlayWideToUtf8(modRoot) + "; sources from that folder may not resolve.");
            else if (!existed) ++made;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        if (made) PlayAppendLog("[LIVE ZONE] " + std::to_string(made) + " folder junction(s) from " + PlayWideToUtf8(modRoot) + " to the map's usermap folder.");
    }

    // Live material zones usermaps\<map>_lm<stamp> (one per batch, loaded by the bridge) and nav refresh zones usermaps\<map>_nav<stamp>.
    // Only what these functions created is deleted: junctions are removed as links (never followed), then the zone folders' own files,
    // then the folder. Folders go right after their link, the launch sweep catches leftovers.
    bool PlayIsLiveMatZoneName(const PlayMapInfo& m, const std::wstring& name)
    {
        std::wstring n = PlayLower(name);
        for (const wchar_t* kind : { L"_lm", L"_nav" })
        {
            const std::wstring pre = PlayLower(m.mapName) + kind;
            if (n.compare(0, pre.size(), pre) != 0 || n.size() == pre.size()) continue;
            size_t digits = pre.size();
            if (kind[1] == L'l' && n[digits] == L'f') ++digits;   // a full live zone
            if (digits == n.size()) continue;
            bool ok = true;
            for (size_t i = digits; i < n.size(); ++i) if (n[i] < L'0' || n[i] > L'9') { ok = false; break; }
            if (ok) return true;
        }
        return false;
    }
    void PlayRemoveLiveMatModDir(const std::wstring& root, int depth)
    {
        WIN32_FIND_DATAW fd = {};
        HANDLE h = FindFirstFileW(PlayJoin(root, L"*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                const std::wstring path = PlayJoin(root, fd.cFileName);
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { RemoveDirectoryW(path.c_str()); continue; }   // a junction: unlink only
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 6) PlayRemoveLiveMatModDir(path, depth + 1); continue; }
                DeleteFileW(path.c_str());
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(root.c_str());
    }
    int PlayLiveMaterialsRemoveZones(const PlayMapInfo& m)
    {
        int removed = 0;
        std::wstring keep; { std::lock_guard<std::mutex> lock(g_playNavMutex); keep = PlayLower(g_playNavKeepStem); }
        const std::wstring usermaps = PlayJoin(m.gameRoot, L"usermaps");
        WIN32_FIND_DATAW fd = {};
        HANDLE h = INVALID_HANDLE_VALUE;
        for (const wchar_t* kind : { L"_lm*", L"_nav*" })
        {
            h = FindFirstFileW(PlayJoin(usermaps, m.mapName + kind).c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do
            {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !PlayIsLiveMatZoneName(m, fd.cFileName)) continue;
                if (!keep.empty() && PlayLower(fd.cFileName) == keep) continue;
                PlayRemoveLiveMatModDir(PlayJoin(usermaps, fd.cFileName), 0); ++removed;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        {
            // the patch zone's old link folder: its live material list moves out first
            const std::wstring patchRoot = PlayJoin(usermaps, m.mapName + L"_patch");
            if (PlayDirExists(patchRoot))
            {
                const std::wstring list = PlayLiveMaterialsPath(m), oldParked = PlayJoin(patchRoot, L"live_materials.txt.failed");
                if (PlayFileExists(oldParked) && !PlayFileExists(list + L".failed")) MoveFileW(oldParked.c_str(), (list + L".failed").c_str());
                PlayRemoveLiveMatModDir(patchRoot, 0); ++removed;
            }
        }
        for (const std::wstring& zoneDir : { PlayJoin(m.usermapRoot, L"zone"), PlayJoin(m.gameRoot, L"zone") })
        for (const wchar_t* kind : { L"_lm*", L"_nav*" })
        {
            h = FindFirstFileW(PlayJoin(zoneDir, L"*" + m.mapName + kind).c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do
            {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::wstring n = fd.cFileName; size_t dot = n.find_last_of(L'.'); if (dot == std::wstring::npos) continue;
                std::wstring stem = n.substr(0, dot), ext = PlayLower(n.substr(dot));
                if (ext != L".ff" && ext != L".xpak") continue;
                if (PlayLower(stem).compare(0, 3, L"en_") == 0) stem = stem.substr(3);
                if (!PlayIsLiveMatZoneName(m, stem)) continue;
                if (!keep.empty() && PlayLower(stem) == keep) continue;
                if (DeleteFileW(PlayJoin(zoneDir, fd.cFileName).c_str())) ++removed;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        return removed;
    }
    // Every usermap's live zones, only while no BlackOps3.exe runs (a running game holds its loaded zones open).
    int PlayLiveMaterialsRemoveAllZones(const char* why)
    {
        if (PlayFindBo3ProcessId()) return 0;
        const std::wstring game = PlayGameRoot(), usermaps = PlayJoin(game, L"usermaps");
        if (game.empty()) return 0;
        std::vector<std::wstring> maps;
        WIN32_FIND_DATAW fd = {};
        HANDLE h = FindFirstFileW(PlayJoin(usermaps, L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;
        do
        {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) maps.push_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        int removed = 0;
        for (const std::wstring& name : maps)
        {
            PlayMapInfo m; m.gameRoot = game; m.mapName = name; m.usermapRoot = PlayJoin(usermaps, name);
            PlayLiveMaterialsPath(m);   // moves an old _patch folder's live list out before the folder goes
            removed += PlayLiveMaterialsRemoveZones(m);
        }
        if (removed) PlayAppendLog("[TEXTURED] " + std::to_string(removed) + " stale live material / nav zone item(s) removed (" + why + ").");
        return removed;
    }

    // Carrier models: a lit material cannot be linked by a bare material,<name> zone line (the linker builds every technique and dies on
    // shader sources the mod tools lack), but a model that uses it links through the shader cache and brings it in as mc/<name>.
    // Each live material gets a 32-unit cube: model_export\_playtest_live\<carrier>.xmodel_export -> export2bin, one GDT per map in source_data\_playtest_live_<map>.gdt, gdtdb /update.
    std::vector<std::string> PlayPreloadMaterialNames(const PlayMapInfo& m);
    // Carrier names are a FIXED POOL registered in the map's carrier GDT once: every gdt.db change made Radiant's idle tick reload its asset
    // database (about 9 s of sqlite on the UI thread, deafening the docked game). A live zone rewrites one pooled carrier model (up to
    // kPlayCarrierMaterials materials, one quad each) and never touches gdt.db again, the patch zone has its own pooled names.
    constexpr int kPlayCarrierMaterials = 16;        // materials per carrier model (one surface each)
    constexpr unsigned kPlayLiveCarrierSlots = 64;   // live zone carrier names, reused round robin (the engine holds 64 zones at most)
    constexpr unsigned kPlayPatchCarriers = 16;      // patch zone carrier names (16 x 16 = 256 live materials)
    std::atomic<unsigned> g_playLiveCarrierNext{0};
    std::string PlayCarrierSlotName(const PlayMapInfo& m, bool patch, unsigned index)
    {
        char buf[128]; snprintf(buf, sizeof buf, patch ? "lmc_%s_patch_%02u" : "lmc_%s_p%02u", PlayWideToUtf8(m.mapName).c_str(), index);
        return buf;
    }
    std::vector<std::string> PlayCarrierPoolNames(const PlayMapInfo& m)
    {
        std::vector<std::string> out;
        for (unsigned i = 0; i < kPlayLiveCarrierSlots; ++i) out.push_back(PlayCarrierSlotName(m, false, i));
        for (unsigned i = 0; i < kPlayPatchCarriers; ++i) out.push_back(PlayCarrierSlotName(m, true, i));
        return out;
    }
    std::wstring PlayLiveCarrierDir(const PlayMapInfo& m) { return PlayJoin(PlayJoin(m.gameRoot, L"model_export"), L"_playtest_live"); }
    std::wstring PlayLiveCarrierGdtPath(const PlayMapInfo& m) { return PlayJoin(PlayJoin(m.gameRoot, L"source_data"), L"_playtest_live_" + m.mapName + L".gdt"); }
    // One quad per material (32 x 32, stacked 2 units apart), rewritten and converted every time (the pooled name carries different materials per link).
    bool PlayWriteCarrierModel(const PlayMapInfo& m, const std::string& carrier, const std::vector<std::string>& materials, std::string& error)
    {
        const std::wstring dir = PlayLiveCarrierDir(m);
        CreateDirectoryW(PlayJoin(m.gameRoot, L"model_export").c_str(), nullptr);
        CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring wc = PlayUtf8ToWide(carrier);
        const int n = (int)materials.size();
        std::string t = "// PlayTest live material carrier (" + std::to_string(n) + " material(s))\n\nMODEL\nVERSION 6\n\nNUMBONES 1\nBONE 0 -1 \"tag_origin\"\n\nBONE 0\n"
                        "OFFSET 0.000000 0.000000 0.000000\nSCALE 1.000000 1.000000 1.000000\nX 1.000000 0.000000 0.000000\nY 0.000000 1.000000 0.000000\nZ 0.000000 0.000000 1.000000\n\n";
        char line[256];
        snprintf(line, sizeof line, "NUMVERTS %d\n\n", 4 * n); t += line;
        static const float Q[4][2] = { {-16,-16},{16,-16},{16,16},{-16,16} };
        for (int k = 0; k < n; ++k)
            for (int v = 0; v < 4; ++v)
            { snprintf(line, sizeof line, "VERT %d\nOFFSET %.6f %.6f %.6f\nBONES 1\nBONE 0 1.000000\n\n", 4 * k + v, Q[v][0], Q[v][1], 2.0f * k); t += line; }
        snprintf(line, sizeof line, "NUMFACES %d\n\n", 2 * n); t += line;
        static const float UV[4][2] = { {0,0},{1,0},{1,1},{0,1} };
        for (int k = 0; k < n; ++k)
            for (int tri = 0; tri < 2; ++tri)
            {
                const int idx[3] = { 0, tri ? 2 : 1, tri ? 3 : 2 };
                snprintf(line, sizeof line, "TRI 0 %d 0 0\n", k); t += line;
                for (int c = 0; c < 3; ++c)
                { snprintf(line, sizeof line, "VERT %d\nNORMAL 0.000000 0.000000 1.000000\nCOLOR 1.000000 1.000000 1.000000 1.000000\nUV 1 %.6f %.6f\n\n", 4 * k + idx[c], UV[idx[c]][0], UV[idx[c]][1]); t += line; }
            }
        snprintf(line, sizeof line, "NUMOBJECTS 1\nOBJECT 0 \"carrier\"\n\nNUMMATERIALS %d\n", n); t += line;
        for (int k = 0; k < n; ++k)
            t += "MATERIAL " + std::to_string(k) + " \"" + materials[k] + "\" \"Lambert\" \"\"\n"
                 "COLOR 0.000000 0.000000 0.000000 1.000000\nTRANSPARENCY 0.000000 0.000000 0.000000 1.000000\nAMBIENTCOLOR 1.000000 1.000000 1.000000 1.000000\n"
                 "INCANDESCENCE 0.000000 0.000000 0.000000 1.000000\nCOEFFS 0.800000 0.000000\nGLOW 0.000000 0\nREFRACTIVE 6 1.000000\n"
                 "SPECULARCOLOR 0.500000 0.500000 0.500000 1.000000\nREFLECTIVECOLOR 0.000000 0.000000 0.000000 1.000000\nREFLECTIVE 1 0.500000\nBLINN -1.000000 -1.000000\nPHONG -1.000000\n";
        if (!PlayWriteTextFile(PlayJoin(dir, wc + L".xmodel_export"), t)) { error = "could not write the carrier model export " + carrier; return false; }
        DeleteFileW(PlayJoin(dir, wc + L".xmodel_bin").c_str()); DeleteFileW(PlayJoin(dir, wc + L".XMODEL_BIN").c_str());
        const std::wstring export2bin = PlayJoin(PlayJoin(m.toolsRoot, L"bin"), L"export2bin.exe");
        if (!PlayFileExists(export2bin)) { error = "export2bin.exe was not found under TA_TOOLS_PATH\\bin"; return false; }
        PlayRunCaptured(export2bin, { L"/v", wc + L".xmodel_export" }, dir, "Carrier model convert");
        if (!PlayFileExists(PlayJoin(dir, wc + L".xmodel_bin")) && !PlayFileExists(PlayJoin(dir, wc + L".XMODEL_BIN")))
        { error = "export2bin produced no .xmodel_bin for the carrier " + carrier; return false; }
        return true;
    }
    // The map's carrier GDT holds the whole pool, true when the file changed (first use of a map, or a pool size change).
    bool PlayWriteCarrierGdt(const PlayMapInfo& m, std::string& error)
    {
        std::string gdt = "{\n";
        for (auto& c : PlayCarrierPoolNames(m))
            gdt += "\t\"" + c + "\" ( \"xmodel.gdf\" )\n\t{\n\t\t\"BulletCollisionLOD\" \"None\"\n\t\t\"filename\" \"_playtest_live\\\\" + c + ".xmodel_bin\"\n\t\t\"type\" \"rigid\"\n\t}\n";
        gdt += "}\n";
        const std::wstring path = PlayLiveCarrierGdtPath(m);
        if (PlayReadTextFile(path, 1024 * 1024) == gdt) return false;
        if (!PlayWriteTextFile(path, gdt)) { error = "could not write " + PlayWideToUtf8(path); return false; }
        return true;
    }
    bool PlayUpdateGdtDb(const PlayMapInfo& m, std::string& error)
    {
        const std::wstring gdtdb = PlayJoin(PlayJoin(m.toolsRoot, L"gdtdb"), L"gdtdb.exe");
        if (!PlayFileExists(gdtdb)) { error = "gdtdb.exe was not found under TA_TOOLS_PATH\\gdtdb"; return false; }
        if (PlayRunCaptured(gdtdb, { L"/update" }, m.toolsRoot, "GDT database update (live material carrier pool)") != 0) { error = "gdtdb /update failed"; return false; }
        return true;
    }
    // The pool is in the database (gdt.db changes only when the GDT text does: once per map).
    bool PlayEnsureCarrierPool(const PlayMapInfo& m, std::string& error)
    {
        const bool changed = PlayWriteCarrierGdt(m, error);
        if (!error.empty()) return false;
        if (changed && !PlayUpdateGdtDb(m, error)) return false;
        return true;
    }
    // Materials from the shipped GDTs whose names start with one of the preload prefixes (text scan of source_data\*.gdt).
    std::vector<std::string> PlayPreloadMaterialNames(const PlayMapInfo& m)
    {
        std::vector<std::string> out;
        if (g_playPreloadMaterials.empty()) return out;
        const std::wstring dir = PlayJoin(m.gameRoot, L"source_data");
        WIN32_FIND_DATAW fd = {};
        HANDLE h = FindFirstFileW(PlayJoin(dir, L"*.gdt").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return out;
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (_wcsnicmp(fd.cFileName, L"_playtest_live_", 15) == 0) continue;
            const std::string text = PlayReadTextFile(PlayJoin(dir, fd.cFileName), 64 * 1024 * 1024);
            size_t pos = 0;
            while ((pos = text.find("( \"material.gdf\" )", pos)) != std::string::npos)
            {
                size_t q2 = text.rfind('"', pos); if (q2 == std::string::npos) break;
                size_t q1 = text.rfind('"', q2 - 1); if (q1 == std::string::npos) break;
                const std::string name = text.substr(q1 + 1, q2 - q1 - 1);
                pos += 18;
                if (name.empty() || name[0] == '$' || name.find_first_of(" \t,\"/\\") != std::string::npos) continue;
                for (auto& pre : g_playPreloadMaterials)
                    if (!pre.empty() && _strnicmp(name.c_str(), pre.c_str(), pre.size()) == 0) { if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name); break; }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return out;
    }
    // Carriers for these materials exist on disk and in the database, zone lines to reference them are appended to `zone`.
    // patch: the patch zone's own pooled names, else the next live slots.
    // write = false (patch only): the zone lines alone, nothing written or converted (the "placed patch is current" test).
    bool PlayPrepareCarriers(const PlayMapInfo& m, const std::vector<std::string>& names, std::string& zone, std::string& error, bool patch, bool write = true)
    {
        if (!patch) write = true;   // live slots rotate: their names are only known by taking one
        if (write && !PlayEnsureCarrierPool(m, error)) return false;
        unsigned patchIndex = 0;
        for (size_t at = 0; at < names.size(); at += kPlayCarrierMaterials)
        {
            const std::vector<std::string> chunk(names.begin() + at, names.begin() + std::min(names.size(), at + kPlayCarrierMaterials));
            std::string carrier;
            if (patch)
            {
                if (patchIndex >= kPlayPatchCarriers) { error = "more live materials than the patch carrier pool holds (" + std::to_string(kPlayPatchCarriers * kPlayCarrierMaterials) + ")"; return false; }
                carrier = PlayCarrierSlotName(m, true, patchIndex++);
            }
            else carrier = PlayCarrierSlotName(m, false, g_playLiveCarrierNext.fetch_add(1) % kPlayLiveCarrierSlots);
            if (write && !PlayWriteCarrierModel(m, carrier, chunk, error)) return false;
            zone += "xmodel," + carrier + "\r\n";
        }
        return true;
    }

    // Links the carriers of these materials as the zone <zoneName> and places its fastfiles in the map's zone folder (worker thread).
    bool PlayLiveMaterialsLinkZone(const PlayMapInfo& m, const std::vector<std::string>& names, const std::wstring& zoneName, std::string& error, const std::string& extraZoneLines = std::string())
    {
        const std::wstring modRoot = PlayJoin(PlayJoin(m.gameRoot, L"usermaps"), zoneName);
        CreateDirectoryW(modRoot.c_str(), nullptr);
        struct DropSource { std::wstring root; ~DropSource() { PlayRemoveLiveMatModDir(root, 0); } } dropSource{ modRoot };   // only while the linker runs
        for (auto* d : { L"zone", L"zone_source" }) CreateDirectoryW(PlayJoin(modRoot, d).c_str(), nullptr);   // no junctions: carriers and materials live in the root's source_data
        // the engine opens a zone's image pack only for zones whose fastfile declares it (config "xpak_read" keyed by zone name,
        // XPak_AddPackfilesForZone 0x271C330): ours must carry the same two lines as the level's generated zone source
        const std::string zoneNameUtf8 = PlayWideToUtf8(zoneName);
        std::string zone = ">level.xpak_write," + zoneNameUtf8 + "\r\n>level.xpak_read," + zoneNameUtf8 + "\r\n";
        // live models travel in the same list as "xmodel,<name>" lines: zone lines, not carriers
        std::vector<std::string> materials; std::string modelLines;
        for (const auto& n : names) { if (n.rfind("xmodel,", 0) == 0 || n.rfind("fx,", 0) == 0 || n.rfind("material,", 0) == 0) modelLines += n + "\r\n"; else materials.push_back(n); }   // live fx too, decal materials "material,vd/<name>"
        if (!PlayPrepareCarriers(m, materials, zone, error, false)) return false;
        zone += modelLines;
        zone += extraZoneLines;
        if (!PlayWriteTextFile(PlayJoin(PlayJoin(modRoot, L"zone_source"), zoneName + L".zone"), zone))
        { error = "could not write the zone file under usermaps\\" + PlayWideToUtf8(zoneName); return false; }
        std::vector<std::wstring> args;
        if (!g_playLanguage.empty()) { args.push_back(L"-language"); args.push_back(PlayUtf8ToWide(g_playLanguage)); }
        args.push_back(L"-modsource"); args.push_back(zoneName);
        std::wstring linkerExe = PlayJoin(PlayJoin(m.toolsRoot, L"bin"), L"linker_modtools.exe");
        if (PlayBetterBuildRoute(linkerExe, args, L"linker", L"BetterBuildLinker.exe"))
            PlayAppendLog("[BETTERBUILD] Linking the live material zone through BetterBuild.");
        const int rc = PlayRunCaptured(linkerExe, args, m.toolsRoot, "Live materials link");
        const std::wstring outDir = PlayJoin(modRoot, L"zone");
        if (PlayLastRunFatal() || (rc & 0x80000000))
        {
            // the linker leaves fastfiles behind even when it dies: never let them near the game
            for (auto* ext : { L".ff", L".xpak" }) for (auto& pre : { std::wstring(L""), std::wstring(L"en_") }) DeleteFileW(PlayJoin(outDir, pre + zoneName + ext).c_str());
            error = "the live material link FAILED (exit " + std::to_string(rc) + "); see the linker output above";
            return false;
        }
        if (!PlayFileExists(PlayJoin(outDir, zoneName + L".ff")) || !PlayFileExists(PlayJoin(outDir, L"en_" + zoneName + L".ff")))
        { error = "the live material link (exit " + std::to_string(rc) + ") produced no fastfiles; see the linker output above"; return false; }
        // flags 0x10400: additive group + usermaps path, so the file lives in the usermap's zone folder (a zone from the install's own
        // zone folder counts as shipped and fails its signature check)
        const std::wstring mapZoneDir = PlayJoin(m.usermapRoot, L"zone");
        int placed = 0;
        for (auto* ext : { L".ff", L".xpak" })
            for (auto& pre : { std::wstring(L""), std::wstring(L"en_") })
            {
                std::wstring src = PlayJoin(outDir, pre + zoneName + ext), dst = PlayJoin(mapZoneDir, pre + zoneName + ext);
                DeleteFileW(dst.c_str());
                if (!PlayFileExists(src)) continue;
                if (CreateHardLinkW(dst.c_str(), src.c_str(), nullptr) || CopyFileW(src.c_str(), dst.c_str(), FALSE)) ++placed;
            }
        if (placed < 2) { error = "the live material fastfiles could not be placed in the map's zone folder"; return false; }
        return true;
    }

    // The saved map minus the baked brushes that are moved right now (the active hide records): the nav mesh is built as if they did not
    // exist, so their silhouettes own them (a second move needs no rebuild). Every "// brush N" block whose guid is in the set is dropped.
    bool PlayWriteNavTempMap(const std::wstring& src, const std::wstring& dst, const std::set<std::string>& stripKeys, int& stripped, std::string& error)
    {
        stripped = 0;
        std::string text = PlayReadTextFile(src, 256 * 1024 * 1024);
        if (text.empty()) { error = "the saved map could not be read"; return false; }
        std::string out; out.reserve(text.size());
        std::string pending; bool inBrush = false; int braces = 0; std::string guid;
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t eol = text.find('\n', pos);
            const std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos + 1);
            pos = eol == std::string::npos ? text.size() : eol + 1;
            size_t b = 0; while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
            const std::string t = line.substr(b);
            if (!inBrush)
            {
                if (t.rfind("// brush", 0) == 0) { inBrush = true; braces = 0; guid.clear(); pending = line; continue; }
                out += line; continue;
            }
            pending += line;
            if (!t.empty() && t[0] == '{') { ++braces; continue; }
            if (t.rfind("guid", 0) == 0)
            {
                const size_t q1 = t.find('{'), q2 = t.find('}');
                if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1) guid = t.substr(q1, q2 - q1 + 1);
                continue;
            }
            if (!t.empty() && t[0] == '}')
            {
                if (--braces <= 0)
                {
                    inBrush = false;
                    if (!guid.empty() && stripKeys.count(PlayLiveGuidKey(guid.c_str()))) ++stripped; else out += pending;
                    pending.clear();
                }
            }
        }
        if (inBrush) out += pending;
        if (!PlayWriteTextFile(dst, out)) { error = "the temporary map could not be written beside the map"; return false; }
        return true;
    }

    // The nav zone is linked from a compile of the temporary map into <xx>\<map>_n<stamp>.d3dbsp (the asset name of the zone line, unique
    // per refresh: the engine drops a same-named asset as redundant). The real BSP is untouched, temporary files go once the linker read them.
    void PlayNavWorker(PlayMapInfo m, std::wstring zoneName, std::wstring stamp, std::string assetName, std::wstring tempMap, std::wstring tempBsp)
    {
        g_playRunBelowNormal = true;
        g_playRunJobCancel = &g_playNavCancel;
        PlayNavResult r; r.zoneName = PlayWideToUtf8(zoneName); r.assetName = assetName;
        const uint64_t t0 = GetTickCount64();
        const std::wstring cod2map = PlayJoin(PlayJoin(m.toolsRoot, L"bin"), L"cod2map64.exe");
        if (!PlayFileExists(cod2map)) r.error = "cod2map64.exe was not found under TA_TOOLS_PATH\\bin";
        else
        {
            std::vector<std::wstring> args = { L"-platform", L"pc", L"-navmesh", L"-navvolume", L"-loadFrom", tempMap, tempBsp };
            std::wstring compiler = cod2map;
            if (PlayBetterBuildRoute(compiler, args, L"compile", L"BetterBuildWorker.exe")) PlayAppendLog("[NAV] Compiling through BetterBuild.");
            if (PlayRunCaptured(compiler, args, PlayJoin(m.toolsRoot, L"bin"), "Nav refresh compile") != 0) r.error = "the compile failed; see the cod2map output above";
            else r.ok = PlayLiveMaterialsLinkZone(m, std::vector<std::string>(), zoneName, r.error, "navmesh," + assetName + "\r\n");
        }
        const std::wstring base = tempBsp.substr(0, tempBsp.size() - 7);   // without ".d3dbsp"
        for (auto* suffix : { L".d3dbsp", L".d3dprt", L"_navmesh.hkt", L"_navvolume.hkt", L"_metadata.json", L"_navmesh_settings.json", L"_navmesh.hkdmp", L"_navvolume.hkdmp" }) DeleteFileW((base + suffix).c_str());
        DeleteFileW(tempMap.c_str());
        r.seconds = (GetTickCount64() - t0) / 1000;
        { std::lock_guard<std::mutex> lock(g_playNavMutex); g_playNavResult = r; }
        g_playNavThreadDone = true;
    }

    // Play worker: the nav mesh of the saved map compiled beside the main compile (cod2map is single-threaded, the two do not slow each
    // other) into a nav zone the tick loads and swaps once the new level is up. False: the serial -navmesh compile stays.
    bool PlayNavBuildBesidePlay(const PlayMapInfo& m)
    {
        for (int i = 0; i < 50 && g_playNavRunning; ++i) Sleep(100);   // a refresh cancelled by PlayStart is still winding down
        if (g_playNavRunning || g_playNavPhase != 0) { PlayAppendLog("[NAV] a nav refresh is still in flight; the map compile carries the nav mesh itself."); return false; }
        const std::wstring stamp = std::to_wstring((unsigned long long)((GetTickCount64() / 1000) % 1000000));
        const std::wstring zoneName = m.mapName + L"_nav" + stamp, tempName = m.mapName + L"_n" + stamp;
        const std::string assetName = "maps/" + PlayWideToUtf8(m.prefix) + "/" + PlayWideToUtf8(tempName) + "_navmesh";
        const std::wstring mapDir = m.mapPath.substr(0, m.mapPath.find_last_of(L"\\/") + 1), bspDir = m.bspPath.substr(0, m.bspPath.find_last_of(L"\\/") + 1);
        const std::wstring tempMap = PlayJoin(mapDir, tempName + L".map"), tempBsp = PlayJoin(bspDir, tempName + L".d3dbsp");
        if (!CopyFileW(m.mapPath.c_str(), tempMap.c_str(), FALSE)) { PlayAppendLog("[NAV] the map could not be copied for the nav build (" + std::to_string(GetLastError()) + "); the map compile carries the nav mesh itself."); return false; }
        { const std::wstring st = PlayJoin(mapDir, m.mapName + L"_navmesh_settings.json"); if (PlayFileExists(st)) CopyFileW(st.c_str(), PlayJoin(mapDir, tempName + L"_navmesh_settings.json").c_str(), FALSE); }
        if (g_playNavThread.joinable()) g_playNavThread.join();
        { std::lock_guard<std::mutex> lock(g_playNavMutex); g_playNavKeepStem = zoneName; }
        g_playNavForPlayInitBase = PlayLoadInitCount();
        g_playNavCancel = false; g_playNavThreadDone = false; g_playNavForPlay = true; g_playNavRunning = true;
        g_playNavThread = std::thread([m, zoneName, stamp, assetName, tempMap, tempBsp]() { PlayNavWorker(m, zoneName, stamp, assetName, tempMap, tempBsp); });
        PlayAppendLog("[NAV] nav mesh compiled beside the map as zone " + PlayWideToUtf8(zoneName) + "; it goes into the game once the level is up.");
        return true;
    }

    // true = fine (placed says whether a patch fastfile is now in usermaps\<map>\zone), false = error (message in error).
    bool PlayLiveZoneBuild(const PlayMapInfo& m, std::string& error, bool& placed)
    {
        placed = false;
        const uint64_t t0 = GetTickCount64();
        const std::wstring patchName = m.mapName + L"_patch";
        const std::wstring patchRoot = PlayJoin(PlayJoin(m.gameRoot, L"usermaps"), patchName);
        const std::wstring mapZoneDir = PlayJoin(m.usermapRoot, L"zone");
        const uint64_t baseTime = PlayFileTime(PlayJoin(mapZoneDir, m.mapName + L".ff"));
        if (!baseTime) { error = "the base fastfile is missing; a normal link is needed first"; return false; }

        // Zone lines: every file-backed asset of the base zone whose source is newer than the base fastfile.
        const std::string baseZone = PlayReadTextFile(PlayJoin(PlayJoin(m.usermapRoot, L"zone_source"), m.mapName + L".zone"), 4 * 1024 * 1024);
        std::string zone;
        int nScripts = 0, nFiles = 0;
        uint64_t newestSource = 0;
        std::istringstream in(baseZone);
        std::string raw;
        while (std::getline(in, raw))
        {
            size_t c = raw.find("//"); if (c != std::string::npos) raw.erase(c);
            while (!raw.empty() && (raw.back() == '\r' || raw.back() == ' ' || raw.back() == '\t')) raw.pop_back();
            size_t s = raw.find_first_not_of(" \t"); if (s == std::string::npos) continue; raw.erase(0, s);
            if (raw[0] == '>' || raw[0] == '#') continue;
            size_t comma = raw.find(','); if (comma == std::string::npos) continue;
            std::string type = PlayLowerAscii(raw.substr(0, comma)), name = raw.substr(comma + 1);
            while (!type.empty() && type.back() == ' ') type.pop_back();
            size_t ns = name.find_first_not_of(" \t"); if (ns == std::string::npos) continue; name.erase(0, ns);
            if (type == "col_map" || type == "gfx_map") continue;   // the world stays in the base:
            if (type != "scriptparsetree" && type != "rawfile" && type != "stringtable") continue;
            std::wstring rel = PlayUtf8ToWide(name);
            for (auto& ch : rel) if (ch == L'/') ch = L'\\';
            std::wstring src = PlayJoin(m.usermapRoot, rel);
            if (!PlayFileExists(src)) src = PlayJoin(PlayJoin(PlayJoin(m.gameRoot, L"share"), L"raw"), rel);
            const uint64_t srcTime = PlayFileTime(src);
            if (srcTime > baseTime)
            {
                zone += type + "," + name + "\r\n";
                if (type == "scriptparsetree") ++nScripts; else ++nFiles;
                if (srcTime > newestSource) newestSource = srcTime;
            }
        }

        // live materials: material,<name> for every name the game reported missing, the list file counts as a source. Composed twice: first
        // without writing anything for the "placed patch is current" test below (no export2bin or gdt.db check), then for real when a link is needed.
        int nMaterials = 0;
        std::vector<std::string> mats = PlayReadLiveMaterials(m);
        for (auto& n : PlayPreloadMaterialNames(m)) if (std::find(mats.begin(), mats.end(), n) == mats.end()) mats.push_back(n);
        mats.erase(std::remove_if(mats.begin(), mats.end(), [](const std::string& n) { return n.empty() || n[0] == '$' || n.find_first_of(" \t,\"") != std::string::npos; }), mats.end());
        const std::string scriptZone = zone;
        auto composeZone = [&](bool write) -> std::string
        {
            std::string out = scriptZone;
            nMaterials = 0;
            const uint64_t listTime = PlayFileTime(PlayLiveMaterialsPath(m));
            std::string carrierError;
            if (mats.empty()) { if (write && !PlayEnsureCarrierPool(m, carrierError)) PlayAppendLog("[LIVE ZONE WARN] live material carrier pool: " + carrierError); }   // registered at launch, so a later live link never touches gdt.db
            else
            {
                if (PlayPrepareCarriers(m, mats, out, carrierError, true, write)) nMaterials = (int)mats.size();
                else PlayAppendLog("[LIVE ZONE WARN] live material carriers could not be prepared (" + carrierError + "); the patch zone goes out without them.");
            }
            int nModels = 0;
            for (const auto& n : PlayReadLiveMaterials(m)) if ((n.rfind("xmodel,", 0) == 0 || n.rfind("fx,", 0) == 0) && n.find_first_of(" \t\"") == std::string::npos) { out += n + "\r\n"; ++nModels; }   // live models, live fx
            if ((nMaterials || nModels) && listTime > newestSource) newestSource = listTime;
            if (nMaterials)   // streamed carrier images: the patch zone must declare its own image pack (see PlayLiveMaterialsLinkZone)
                out = ">level.xpak_write," + PlayWideToUtf8(patchName) + "\r\n>level.xpak_read," + PlayWideToUtf8(patchName) + "\r\n" + out;
            return out;
        };
        zone = composeZone(false);

        const wchar_t* outputs[] = { L".ff", L".xpak" };
        if (zone.empty())
        {
            const int removed = PlayLiveZoneRemovePatchFiles(m);
            PlayAppendLog("[LIVE ZONE] nothing is newer than the base fastfile; no patch zone" + (removed ? " (stale patch fastfiles removed: " + std::to_string(removed) + ")" : std::string("")));
            return true;
        }
        // Same zone lines as the last patch and a patch fastfile newer than every source in it: the placed patch is current, no link.
        // A newer base does not force it: script lines are chosen against the CURRENT base, so a base relink that took a script in
        // changes the zone text anyway, and a materials-only patch does not depend on the base.
        {
            const std::wstring patchZonePath = PlayPatchZoneCopyPath(m);
            const uint64_t placedTime = PlayFileTime(PlayJoin(mapZoneDir, patchName + L".ff"));
            const uint64_t placedEnTime = PlayFileTime(PlayJoin(mapZoneDir, L"en_" + patchName + L".ff"));
            if (placedTime && placedEnTime && placedTime > newestSource &&
                PlayReadTextFile(patchZonePath, 4 * 1024 * 1024) == zone)
            {
                PlayAppendLog("[LIVE ZONE] the placed patch zone is current (" + std::to_string(nScripts) + " script(s), " + std::to_string(nFiles) + " other file(s), " + std::to_string(nMaterials) + " live material(s)); no link.");
                return true;
            }
        }
        zone = composeZone(true);
        // the link folder usermaps\<map>_patch exists only for this link (the launcher would list it as a map)
        PlayPrepareModSource(m, patchRoot);
        struct DropSource { std::wstring root; ~DropSource() { PlayRemoveLiveMatModDir(root, 0); } } dropSource{ patchRoot };
        DeleteFileW(PlayPatchZoneCopyPath(m).c_str());
        if (!PlayWriteTextFile(PlayJoin(PlayJoin(patchRoot, L"zone_source"), patchName + L".zone"), zone))
        { error = "could not write the patch zone file under usermaps\\" + PlayWideToUtf8(patchName); return false; }
        PlayAppendLog("[LIVE ZONE] patch zone: " + std::to_string(nScripts) + " script(s), " + std::to_string(nFiles) + " other file(s) newer than the base fastfile, " + std::to_string(nMaterials) + " live material(s)");
        PlayNote("Linking the patch zone (" + std::to_string(nScripts) + " changed script(s), " + std::to_string(nFiles) + " other file(s), " + std::to_string(nMaterials) + " asset(s) the level lacks)...");

        std::vector<std::wstring> args;
        if (!g_playLanguage.empty()) { args.push_back(L"-language"); args.push_back(PlayUtf8ToWide(g_playLanguage)); }
        args.push_back(L"-modsource"); args.push_back(patchName);
        std::wstring linkerExe = PlayJoin(PlayJoin(m.toolsRoot, L"bin"), L"linker_modtools.exe");
        if (PlayBetterBuildRoute(linkerExe, args, L"linker", L"BetterBuildLinker.exe"))
            PlayAppendLog("[BETTERBUILD] Linking the patch zone through BetterBuild.");
        const int rc = PlayRunCaptured(linkerExe, args, m.toolsRoot, "Live zone link");
        const std::wstring patchZoneDir = PlayJoin(patchRoot, L"zone");
        if (PlayLastRunFatal() || (rc & 0x80000000))
        {
            for (auto* ext : outputs) for (auto& pre : { std::wstring(L""), std::wstring(L"en_") }) DeleteFileW(PlayJoin(patchZoneDir, pre + patchName + ext).c_str());
            if (nMaterials)
            {
                // the live material lines broke the link: park the list and link the patch without them (scripts still patch)
                const std::wstring list = PlayLiveMaterialsPath(m), parked = list + L".failed";
                DeleteFileW(parked.c_str()); MoveFileW(list.c_str(), parked.c_str());
                PlayAppendLog("[LIVE ZONE] the patch link failed with " + std::to_string(nMaterials) + " live material line(s); the list was parked as " + PlayWideToUtf8(parked) + " and the patch is linked again without them.");
                PlayRemoveLiveMatModDir(patchRoot, 0);
                return PlayLiveZoneBuild(m, error, placed);
            }
            const int removed = PlayLiveZoneRemovePatchFiles(m);
            error = "the patch zone link FAILED (exit " + std::to_string(rc) + "); nothing was placed" + (removed ? " and the stale patch files were removed" : "") + "; see the linker output above";
            return false;
        }
        if (!PlayFileExists(PlayJoin(patchZoneDir, patchName + L".ff")) || !PlayFileExists(PlayJoin(patchZoneDir, L"en_" + patchName + L".ff")))
        {
            error = "the patch zone link (exit " + std::to_string(rc) + ") produced no fastfiles; see the linker output above";
            if (nMaterials) error += " (" + std::to_string(nMaterials) + " live material(s) are in the patch: a name the linker cannot convert breaks it; edit " + PlayWideToUtf8(PlayLiveMaterialsPath(m)) + ")";
            return false;
        }

        int linked = 0;
        for (auto* ext : outputs)
            for (auto& pre : { std::wstring(L""), std::wstring(L"en_") })
            {
                std::wstring src = PlayJoin(patchZoneDir, pre + patchName + ext), dst = PlayJoin(mapZoneDir, pre + patchName + ext);
                DeleteFileW(dst.c_str());
                if (!PlayFileExists(src)) continue;
                if (CreateHardLinkW(dst.c_str(), src.c_str(), nullptr) || CopyFileW(src.c_str(), dst.c_str(), FALSE)) ++linked;
                else PlayAppendLog("[LIVE ZONE WARN] could not place " + PlayWideToUtf8(pre + patchName + ext) + " into the map's zone folder (" + std::to_string(GetLastError()) + ")");
            }
        placed = linked >= 2;
        PlayAppendLog("[LIVE ZONE] " + std::to_string(linked) + " patch file(s) placed in usermaps\\" + PlayWideToUtf8(m.mapName) + "\\zone in " + std::to_string((GetTickCount64() - t0) / 1000) + " s" + (rc ? " (linker exit " + std::to_string(rc) + ")" : ""));
        PlayNote("Patch zone linked in " + PlayDuration(GetTickCount64() - t0) + ".");
        if (!placed) { error = "the patch fastfiles could not be placed in the map's zone folder"; return false; }
        CreateDirectoryW(PlayStateDir(m).c_str(), nullptr);
        PlayWriteTextFile(PlayPatchZoneCopyPath(m), zone);   // for the "placed patch is current" test of the next Play
        return true;
    }

    // ---- Built lighting: Radiant's in-editor bake reaches the game only through share\raw\maps\<dir>\<map>.led (Export Lighting), which
    // the link packs. Play exports it first whenever a built light state was never exported (Radiant's own "Lighting changed" test) with the
    // dialog's defaults, i.e. what Yes on Radiant's prompt does. Main thread only.
    struct PlayRadiantString { char buf[16]; size_t size; size_t cap; };   // Radiant's std::string (VS2012 x64)
    using PlayExportLightingFn = PlayRadiantString* (__fastcall*)(void* mainWin, PlayRadiantString* ret, uint8_t* opts, char quiet);

    std::wstring PlayLedPath(const PlayMapInfo& m)
    {
        std::wstring p = m.bspPath;
        const size_t dot = p.find_last_of(L'.');
        if (dot != std::wstring::npos) p.erase(dot);
        return p + L".led";
    }

    bool PlayCallExportLighting(PlayExportLightingFn fn, void* mainWin, PlayRadiantString* ret, uint8_t* opts)
    {
        __try { fn(mainWin, ret, opts, 1); return true; }   // quiet: warnings come back in the string, no box
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void PlayExportBuiltLighting(const PlayMapInfo& m)
    {
        if (!g_playExportLighting || !rp::host || !rp::host->RadiantAddress) return;
        void** mainWinPtr = static_cast<void**>(rp::host->RadiantAddress(rp::self, 0x1444C6560));   // RadiantMainWin*
        const int* prefabDepth = static_cast<const int*>(rp::host->RadiantAddress(rp::self, 0x14521EB9C));
        auto exportFn = reinterpret_cast<PlayExportLightingFn>(rp::host->RadiantAddress(rp::self, 0x14064E5C0));   // ExportLightingCmd
        if (!mainWinPtr || !prefabDepth || !exportFn) { PlayAppendLog("[LIGHT WARN] Radiant build not recognized; built lighting is not exported by Play."); return; }
        char* mainWin = static_cast<char*>(*mainWinPtr);
        char* qe = mainWin ? *reinterpret_cast<char**>(mainWin + 64) : nullptr;
        char* cam = qe ? *reinterpret_cast<char**>(qe + 240) : nullptr;   // CCamWnd, owner of the lighting volumes
        if (!cam) return;

        // Volumes: vector of 16-byte entries at cam+6408..6416, entry+0 = volume, per volume four light states, state int
        // at +1164+4i (3..6 baking, 7/8 built), exported byte at +1180+i.
        char** it = *reinterpret_cast<char***>(cam + 6408);
        char** end = *reinterpret_cast<char***>(cam + 6416);
        bool unexported = false, baking = false;
        for (int n = 0; it && it != end && n < 4096; it += 2, ++n)
        {
            const char* vol = *it;
            if (!vol) continue;
            for (int i = 0; i < 4; ++i)
            {
                const int state = *reinterpret_cast<const int*>(vol + 1164 + 4 * i);
                if (state >= 3 && state <= 6) baking = true;
                if ((state == 7 || state == 8) && !vol[1180 + i]) unexported = true;
            }
        }
        if (baking)
        {
            PlayAppendLog("[LIGHT] Radiant is still building lighting: this Play uses the last exported lighting. Play again when the build is done.");
            PlayNote("Radiant is still building the lighting: this Play uses the last finished lighting. Play again when the light build is done.");
            return;
        }
        if (!unexported) return;
        if (*prefabDepth != 0)
        {
            PlayAppendLog("[LIGHT WARN] Built lighting not exported: Radiant exports lighting only from the top map, leave the prefab first.");
            PlayNote("WARNING: the new lighting was not exported (Radiant exports it only from the top map). Leave the prefab and Play again.");
            return;
        }

        const std::wstring led = PlayLedPath(m);
        const uint64_t before = PlayFileTime(led);
        const uint64_t t0 = GetTickCount64();
        PlayAppendLog("[LIGHT] Exporting Radiant's built lighting to " + PlayWideToUtf8(led) + " ...");
        PlayNote("Exporting the new lighting from Radiant (Radiant pauses until it is done)...");
        uint8_t opts[8] = { 1, 1, 0, 0, 0 };   // the dialog's defaults: local probes, recompute cached data, no build, no background, no force clean
        PlayRadiantString ret = {};
        if (!PlayCallExportLighting(exportFn, mainWin, &ret, opts))
        {
            PlayAppendLog("[LIGHT WARN] Radiant's lighting export raised an exception; the game uses the last exported lighting.");
            PlayNote("WARNING: the lighting export failed; the game uses the last exported lighting.");
            return;
        }
        std::string msg;
        if (ret.size && ret.size < 64 * 1024)
            msg.assign(ret.cap >= 16 ? *reinterpret_cast<const char* const*>(ret.buf) : ret.buf, ret.size);
        // A long message's heap block belongs to Radiant's CRT, it is left allocated (a few bytes, warnings only).
        const uint64_t after = PlayFileTime(led);
        char secs[32]; snprintf(secs, sizeof secs, "%.1f", (GetTickCount64() - t0) / 1000.0);
        if (after && after != before)
        {
            PlayAppendLog(std::string("[LIGHT] Built lighting exported in ") + secs + " s" + (msg.empty() ? "" : "; Radiant reported: " + msg));
            PlayNote("Lighting exported in " + PlayDuration(GetTickCount64() - t0) + ".");
        }
        else
        {
            PlayAppendLog("[LIGHT WARN] Lighting export failed" + (msg.empty() ? std::string("") : ": " + msg) + ". The game uses the last exported lighting.");
            PlayNote("WARNING: the lighting export failed" + (msg.empty() ? std::string("") : " (" + msg + ")") + "; the game uses the last exported lighting.");
        }
    }

    // Early launch: only where the hold is known to work (BO3 Enhanced: the bridge's verified main-frame hook), the bridge goes in
    // (it carries the hold) and it is a cold start (no BO3 at all) that has something to build.
    bool PlayEarlyLaunchAllowed()
    {
        return g_playEarlyLaunch && !g_playJoinMode && g_playEmbedGame && (g_playLivePreview || g_playLiveZone || g_playFromCamera) &&
               PlayCurrentExeBuild(true) == PLAY_BUILD_ENHANCED && !PlayTrackedGameRunning() && !PlayFindBo3ProcessId() &&
               PlayFileExists(PlayJoin(PlayThisModuleDir(), L"BO3LiveBridge.dll"));
    }
    PlayMapInfo g_playEarlyMap;   // for the relaunch when the hold failed after the build had finished
    bool PlayEarlyLaunchStart(const PlayMapInfo& m)
    {
        g_playEarlyMap = m;
        g_playEarlyAborted = false;
        g_playEarlyReloadPending = false;
        g_playLaunchHeld = true;
        g_playEarlyBuild = 1;
        g_playEarlyLaunchMs = GetTickCount64();
        { FILETIME ft; GetSystemTimeAsFileTime(&ft); g_playEarlyLaunchFt = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }
        g_playEarlyGameRoot = m.gameRoot;
        const bool ok = PlayLaunchGame(m);
        g_playState = PLAY_PREPARING;   // PlayLaunchGame says RUNNING, the build is still to come
        if (!ok) { g_playLaunchHeld = false; g_playEarlyBuild = 0; return false; }
        PlayAppendLog("[EARLY] BO3 starts now, while the build runs: it opens the map's files only once the build is done.");
        return true;
    }
    // The build failed or was cancelled: the game started for it goes.
    void PlayEarlyLaunchFail()
    {
        g_playLaunchHeld = false;
        g_playEarlyBuild = 0;
        HANDLE game = nullptr;
        { std::lock_guard<std::mutex> lock(g_playMutex); game = g_playGameProcess; }
        if (game && WaitForSingleObject(game, 0) == WAIT_TIMEOUT)
        {
            TerminateProcess(game, 0);
            PlayAppendLog("[EARLY] the build did not finish: the BO3 started for it was closed.");
        }
    }

    void PlayWorker(PlayMapInfo m, bool full)
    {
        g_playThreadDone = false;
        g_playFullMode = full;
        g_playStartedMs = GetTickCount64();
        g_playState = PLAY_PREPARING;

        const std::wstring zoneDir = PlayJoin(m.usermapRoot, L"zone");
        const std::wstring mainFf = PlayJoin(zoneDir, m.mapName + L".ff");
        const std::wstring langFf = PlayJoin(zoneDir, L"en_" + m.mapName + L".ff");

        PlayBuildFingerprint previous, current;
        PlayLoadFingerprint(m.statePath, previous);
        current.valid = true;
        PlayPrefabScan prefabs;
        {
            std::string mapText;
            if (PlayReadFileForHash(m.mapPath, mapText))
            {
                current.mapAll = PlayHashBytes(kPlayFnvOffset, mapText.data(), mapText.size());
                current.mapGeometry = PlayHashGeometryText(mapText);
                prefabs = PlayScanPrefabs(PlayJoin(m.gameRoot, L"map_source"), mapText);
            }
        }
        const PlayTreeHash userTree = PlayHashSourceTreeRecursive(m.usermapRoot);
        uint64_t sharedNewest = 0;
        current.userSources = userTree.all;
        current.gdtMetadata = PlayHashMetadataRecursive(PlayJoin(m.gameRoot, L"source_data"), { L".gdt" });
        current.extended = true;
        current.prefabsAll = prefabs.all;
        current.prefabsGeometry = prefabs.geometry;
        current.userHeaders = userTree.headers;
        current.sharedScripts = PlayHashMetadataRecursive(PlayJoin(PlayJoin(PlayJoin(m.gameRoot, L"share"), L"raw"), L"scripts"),
                                                          { L".gsc", L".csc", L".gsh" }, &sharedNewest);

        // the "build" wait is closed on every way out of the worker
        struct WaitScope { ~WaitScope() { PlayWaitEnd("build"); } } waitScope;
        if (!current.mapAll || !current.mapGeometry)
        {
            PlaySetError("Smart Play could not fingerprint the current .map file.");
            g_playThreadDone = true;
            return;
        }

        const bool bspMissing = !PlayFileExists(m.bspPath);
        const bool mainFfMissing = !PlayFileExists(mainFf);
        const bool langFfMissing = !PlayFileExists(langFf);

        // An older successful-link stamp that is newer than the current map/usermap sources, with the BSP and fastfiles present, is
        // adopted as the fingerprint baseline instead of one redundant full build.
        bool adoptedBaseline = false;
        if (!previous.valid && !bspMissing && !mainFfMissing && !langFfMissing)
        {
            uint64_t stamp = PlayFileTime(m.stampPath);
            uint64_t mapTime = PlayFileTime(m.mapPath);
            uint64_t userNewest = PlayNewestSourceRecursive(m.usermapRoot);
            if (stamp && mapTime <= stamp && userNewest <= stamp && prefabs.newest <= stamp)
            {
                previous = current; previous.valid = true; adoptedBaseline = true;
                PlaySaveFingerprint(m.statePath, current);
                PlayAppendLog("[SMART] Adopted the existing successful v1.3 build as the fingerprint baseline; no redundant initialization build needed.");
            }
        }

        const bool firstSmartRun = !previous.valid;
        const bool mainMapChanged = firstSmartRun || current.mapAll != previous.mapAll;
        const bool mainGeometryChanged = firstSmartRun || current.mapGeometry != previous.mapGeometry;
        // A state file without the prefab and script keys: file times stand in for them once.
        const bool prefabsNewer = prefabs.newest > PlayFileTime(m.bspPath);
        const bool prefabGeometryChanged = firstSmartRun || (previous.extended ? current.prefabsGeometry != previous.prefabsGeometry
                                                                               : prefabsNewer || (mainMapChanged && prefabs.placements > 0));
        const bool prefabsChanged = prefabGeometryChanged || (previous.extended ? current.prefabsAll != previous.prefabsAll : prefabsNewer);
        const bool mapChanged = mainMapChanged || prefabsChanged;
        const bool geometryChanged = mainGeometryChanged || prefabGeometryChanged;
        const bool userChanged = firstSmartRun || current.userSources != previous.userSources;
        const bool headersChanged = firstSmartRun || (previous.extended ? current.userHeaders != previous.userHeaders : userChanged);
        const bool sharedChanged = !firstSmartRun && (previous.extended ? current.sharedScripts != previous.sharedScripts
                                                                        : sharedNewest > PlayFileTime(mainFf));
        const bool gdtChanged = firstSmartRun || current.gdtMetadata != previous.gdtMetadata;

        PlayCompileMode compileMode = PLAY_COMPILE_NONE;
        if (full || bspMissing) compileMode = PLAY_COMPILE_FULL;
        else if (mapChanged) compileMode = geometryChanged ? PLAY_COMPILE_FULL : PLAY_COMPILE_ONLYENTS;

        // Lighting: the link packs <map>.led, one exported after the last link (by Play, Radiant's prompt or the launcher's
        // Light step) relinks even when nothing else changed.
        const bool ledNewer = PlayFileTime(PlayLedPath(m)) > PlayFileTime(mainFf);
        bool link = full || compileMode != PLAY_COMPILE_NONE || userChanged || sharedChanged || gdtChanged || mainFfMissing || langFfMissing || ledNewer;
        // Live zone: scripts go to the patch zone, the base keeps the world, so a compile (geometry or entities), the GDT, a script
        // header, a missing fastfile or a newer zone file relink it. Other script changes relink it only when the patch took none.
        const bool liveZone = g_playLiveZone;
        const bool zoneFileNewer = PlayFileTime(PlayBaseZonePath(m)) > PlayFileTime(mainFf);
        if (liveZone) link = full || compileMode != PLAY_COMPILE_NONE || headersChanged || gdtChanged || mainFfMissing || langFfMissing || zoneFileNewer || ledNewer;
        bool liveZonePlaced = false;
        bool updateGdt = full || gdtChanged;
        bool anyBuild = compileMode != PLAY_COMPILE_NONE || link || updateGdt;

        std::ostringstream plan;
        plan << "[PLAN] " << (full ? "FORCE FULL" : "SMART")
             << " | compile=" << PlayCompileModeName(compileMode)
             << " link=" << (link ? "yes" : "no")
             << " gdtdb=" << (updateGdt ? "yes" : "no")
             << " warm=" << (g_playKeepWarm ? "yes" : "no")
             << " embed=" << (g_playEmbedGame ? "yes" : "no");
        PlayAppendLog(plan.str());

        if (firstSmartRun && !full)
            PlayAppendLog("[SMART] No v1.4 fingerprint exists yet; this first Smart Play establishes a safe baseline. Later unchanged plays skip the build completely.");
        if (prefabs.files || prefabs.missing)
            PlayAppendLog("[SMART] " + std::to_string(prefabs.files) + " prefab file(s) fingerprinted" +
                          (prefabs.missing ? ", " + std::to_string(prefabs.missing) + " missing" : std::string()) +
                          (firstSmartRun || !prefabsChanged ? "." : prefabGeometryChanged ? ": prefab geometry or a prefab placement changed." : ": prefab entities changed."));
        if (!firstSmartRun && mapChanged && !geometryChanged)
            PlayAppendLog("[SMART] Map changed but brush/patch/material geometry did not: using Treyarch's -onlyents compile path.");
        if (!anyBuild)
            PlayAppendLog("[SMART] Content fingerprints are unchanged. Skipping gdtdb, cod2map and linker entirely.");
        else
        {
            if (!mapChanged) PlayAppendLog("[SMART] .map and prefab content unchanged; no map compile needed.");
            if (sharedChanged && !liveZone) PlayAppendLog("[SMART] Scripts under share\\raw\\scripts changed; relinking.");
            if (headersChanged && liveZone && !firstSmartRun && compileMode == PLAY_COMPILE_NONE) PlayAppendLog("[SMART] A script header (.gsh) changed; relinking the base fastfile.");
            if (!userChanged && compileMode == PLAY_COMPILE_NONE && gdtChanged) PlayAppendLog("[SMART] GDT metadata changed; updating database and relinking without recompiling the BSP.");
            if (userChanged && compileMode == PLAY_COMPILE_NONE && !liveZone) PlayAppendLog("[SMART] Usermap scripts/zone source changed; link-only build.");
            if (zoneFileNewer && liveZone && compileMode == PLAY_COMPILE_NONE) PlayAppendLog("[SMART] The zone file is newer than the fastfile; relinking.");
            if (ledNewer && compileMode == PLAY_COMPILE_NONE) PlayAppendLog("[SMART] Exported lighting (.led) is newer than the fastfile; relinking.");
        }
        // Early launch: BO3's own startup (about half of a cold start) runs while the build runs. Any return before the
        // end of the build closes that game again (the guard).
        struct EarlyGuard { bool active = false, ok = false; ~EarlyGuard() { if (active && !ok) PlayEarlyLaunchFail(); } } early;
        if ((anyBuild || liveZone) && PlayEarlyLaunchAllowed()) early.active = PlayEarlyLaunchStart(m);
        const std::string earlyNote = early.active ? " BO3 starts meanwhile and loads the map when the build is done." : "";

        if (!anyBuild && !liveZone) PlayNote("Nothing changed since the last build: no compile or link needed." + earlyNote);
        else if (!anyBuild) PlayNote("The map and its assets are unchanged: no compile or link of the map needed." + earlyNote);
        else
        {
            std::string steps, why;
            auto add = [&](const char* step) { if (!steps.empty()) steps += ", then "; steps += step; };
            auto because = [&](bool on, const char* reason) { if (on) { if (!why.empty()) why += ", "; why += reason; } };
            if (updateGdt) add("update the asset database");
            if (compileMode == PLAY_COMPILE_FULL) add("compile the map");
            else if (compileMode == PLAY_COMPILE_ONLYENTS) add("compile the map's entities (quick: no geometry changed)");
            if (link) add("link the fastfile");
            because(full, "full build asked");
            because(firstSmartRun && !full, "first Play of this map");
            because(!firstSmartRun && bspMissing, "no compiled map");
            because(!firstSmartRun && mainGeometryChanged, "brushes changed");
            because(!firstSmartRun && prefabGeometryChanged, "a prefab changed");
            because(!firstSmartRun && mapChanged && !geometryChanged, mainMapChanged ? "entities changed" : "a prefab's entities changed");
            because(!firstSmartRun && gdtChanged, "assets (GDTs) changed");
            because(!firstSmartRun && userChanged && !liveZone, "scripts or zone changed");
            because(!firstSmartRun && sharedChanged && !liveZone, "shared scripts changed");
            because(!firstSmartRun && headersChanged && liveZone, "a script header changed");
            because(liveZone && zoneFileNewer, "zone file changed");
            because(ledNewer, "new lighting export");
            because(mainFfMissing || langFfMissing, "fastfile missing");
            PlayNote("Building" + (why.empty() ? std::string() : " (" + why + ")") + ": " + steps + "." + earlyNote);
        }

        const std::wstring gdtdb = PlayJoin(PlayJoin(m.toolsRoot, L"gdtdb"), L"gdtdb.exe");
        const std::wstring cod2map = PlayJoin(PlayJoin(m.toolsRoot, L"bin"), L"cod2map64.exe");
        const std::wstring linker = PlayJoin(PlayJoin(m.toolsRoot, L"bin"), L"linker_modtools.exe");
        if (updateGdt && !PlayFileExists(gdtdb)) { PlaySetError("gdtdb.exe was not found under TA_TOOLS_PATH."); g_playThreadDone = true; return; }
        if (compileMode != PLAY_COMPILE_NONE && !PlayFileExists(cod2map)) { PlaySetError("cod2map64.exe was not found under TA_TOOLS_PATH\\bin."); g_playThreadDone = true; return; }
        if (link && !PlayFileExists(linker)) { PlaySetError("linker_modtools.exe was not found under TA_TOOLS_PATH\\bin."); g_playThreadDone = true; return; }

        if (updateGdt)
        {
            PlayWaitBegin("build", "updating the asset database");
            if (PlayRunCaptured(gdtdb, { L"/update" }, m.toolsRoot, "GDT database update") != 0)
            { if (!g_playCancel) { PlayNoteRunErrors("gdtdb"); PlaySetError("GDT database update failed."); } g_playThreadDone = true; return; }
            PlayNote("Asset database updated in " + PlayDuration(PlayWaitEnd("build")) + ".");
        }
        if (g_playCancel) { g_playState = PLAY_STOPPED; g_playThreadDone = true; return; }

        if (compileMode != PLAY_COMPILE_NONE)
        {
            g_playState = PLAY_COMPILING;
            std::vector<std::wstring> args = { L"-platform", L"pc" };
            // nav beside the compile: Enhanced with the live zone and nav modules (the swap needs them), not a forced full build
            const bool navBeside = compileMode == PLAY_COMPILE_FULL && !full && g_playRebuildNav && g_playLiveZone && g_playLiveNavMesh &&
                                   PlayLiveEditBuild() && !g_playJoinMode && PlayNavBuildBesidePlay(m);
            if (compileMode == PLAY_COMPILE_ONLYENTS)
                args.push_back(L"-onlyents");
            else if ((full || g_playRebuildNav) && !navBeside)
            {
                args.push_back(L"-navmesh");
                args.push_back(L"-navvolume");
            }
            args.push_back(L"-loadFrom"); args.push_back(m.mapPath); args.push_back(m.bspPath);
            const char* label = compileMode == PLAY_COMPILE_ONLYENTS ? "Map compile (-onlyents)" : "Map compile (full geometry)";
            std::wstring compiler = cod2map;
            if (PlayBetterBuildRoute(compiler, args, L"compile", L"BetterBuildWorker.exe"))
                PlayAppendLog("[BETTERBUILD] Compiling through BetterBuild (its launcher settings apply).");
            // cwd = <root>\bin like the launcher: cod2map loads bin\default_navmesh_settings.json relative to it, elsewhere it aborts the nav
            // mesh ("Unable to load navigation mesh generation settings").
            const bool entsOnly = compileMode == PLAY_COMPILE_ONLYENTS;
            PlayNote(entsOnly ? "Compiling the map's entities..." : "Compiling the map (cod2map). A big map can take a few minutes...");
            PlayWaitBegin("build", entsOnly ? "compiling the map's entities" : "compiling the map");
            if (PlayRunCaptured(compiler, args, PlayJoin(m.toolsRoot, L"bin"), label) != 0)
            { if (!g_playCancel) { PlayNoteRunErrors("cod2map"); PlaySetError("Map compile failed. Turn on Verbose log to see cod2map's output."); } g_playThreadDone = true; return; }
            PlayNote("Map compiled in " + PlayDuration(PlayWaitEnd("build")) + ".");
        }
        if (g_playCancel) { g_playState = PLAY_STOPPED; g_playThreadDone = true; return; }

        // false: the link failed, the error is set and the worker is done
        auto runLink = [&]() -> bool
        {
            g_playState = PLAY_LINKING;
            std::vector<std::wstring> args;
            if (!g_playLanguage.empty()) { args.push_back(L"-language"); args.push_back(PlayUtf8ToWide(g_playLanguage)); }
            args.push_back(L"-modsource"); args.push_back(m.mapName);

            std::wstring linkerExe = linker;
            if (PlayBetterBuildRoute(linkerExe, args, L"linker", L"BetterBuildLinker.exe"))
                PlayAppendLog("[BETTERBUILD] Linking through BetterBuild (its launcher settings apply).");
            const uint64_t mainBefore = PlayFileTime(mainFf);
            PlayNote("Linking the fastfile (linker: images, models, sounds, scripts)...");
            PlayWaitBegin("build", "linking the fastfile");
            const int linkExit = PlayRunCaptured(linkerExe, args, m.toolsRoot, "Link");
            const uint64_t mainAfter = PlayFileTime(mainFf);
            const uint64_t langAfter = PlayFileTime(langFf);
            const bool mainWritten = mainAfter != 0 && (mainBefore == 0 || mainAfter > mainBefore);
            const bool mainAndLanguageExist = mainAfter != 0 && langAfter != 0;
            const bool outputsFresh = mainWritten && mainAndLanguageExist;

            if (linkExit != 0 && !outputsFresh)
            {
                if (!g_playCancel)
                {
                    PlayNoteRunErrors("The linker");
                    PlaySetError("Link failed (linker exit " + std::to_string(linkExit) +
                                 "): the map's fastfiles were not written. Turn on Verbose log to see the linker output.");
                }
                g_playThreadDone = true;
                return false;
            }
            if (linkExit != 0 && outputsFresh)
                PlayAppendLog("[LINK WARN] linker_modtools returned " + std::to_string(linkExit) +
                              ", but the main usermap fastfile was refreshed and both required fastfiles exist. Continuing.");
            else if (linkExit == 0 && !outputsFresh && (!PlayFileExists(mainFf) || !PlayFileExists(langFf)))
            {
                PlaySetError("Linker reported success but the expected usermap fastfiles are missing from usermaps/<map>/zone.");
                g_playThreadDone = true;
                return false;
            }
            else if (linkExit == 0 && !outputsFresh)
                PlayAppendLog("[LINK] Linker returned 0; existing usermap fastfiles are present.");

            PlayNote("Fastfile linked in " + PlayDuration(PlayWaitEnd("build")) + ".");
            PlayNoteRunIssues();
            PlayTouchStamp(m.stampPath, m.mapName);
            if (!liveZone)
            {
                // Live mode off: a running game with the DLL injected would still take patch files, so none may stay.
                const int removed = PlayLiveZoneRemovePatchFiles(m);
                if (removed) PlayAppendLog("[LIVE ZONE] " + std::to_string(removed) + " stale patch file(s) removed from the map's zone folder.");
            }
            return true;
        };
        if (link && !runLink()) return;
        if (liveZone && !g_playCancel)
        {
            g_playState = PLAY_LINKING;
            std::string lzError;
            PlayWaitBegin("build", "linking the patch zone (changed scripts and assets the level lacks)");
            if (!PlayLiveZoneBuild(m, lzError, liveZonePlaced))
            { if (!g_playCancel) { PlayNoteRunErrors("The linker"); PlaySetError("Patch zone build failed: " + lzError); } g_playThreadDone = true; return; }
            PlayWaitEnd("build");
            // A script change the patch zone took nothing of (not a zone line, an older file time): the base link takes it.
            if (!link && !liveZonePlaced && (userChanged || sharedChanged) && !g_playCancel)
            {
                PlayAppendLog(std::string("[SMART] Scripts changed (") + (userChanged ? "user_sources" : "") + (userChanged && sharedChanged ? ", " : "") + (sharedChanged ? "shared_scripts" : "") + ") but the patch zone took none of them; relinking the base fastfile.");
                PlayNote("Scripts changed: linking the fastfile.");
                if (!PlayFileExists(linker)) { PlaySetError("linker_modtools.exe was not found under TA_TOOLS_PATH\\bin."); g_playThreadDone = true; return; }
                link = anyBuild = true;
                if (!runLink()) return;
                if (!g_playCancel && !PlayLiveZoneBuild(m, lzError, liveZonePlaced))
                { if (!g_playCancel) { PlayNoteRunErrors("The linker"); PlaySetError("Patch zone build failed: " + lzError); } g_playThreadDone = true; return; }
            }
        }
        if (g_playCancel) { g_playState = PLAY_STOPPED; g_playThreadDone = true; return; }

        // Commit the fingerprint only after every requested stage succeeded: a failed link must not make the next Smart Play think the broken output is current.
        PlaySaveFingerprint(m.statePath, current);
        if (compileMode!=PLAY_COMPILE_NONE) g_playLiveClearAfterBuild=true;

        g_playState = PLAY_LAUNCHING;
        if (early.active && !g_playEarlyAborted && PlayTrackedGameRunning())
        {
            // the game waits on its map files: the UI tick lets it go (PlayEarlyLaunchTick), no reload needed
            early.ok = true;
            g_playForceReloadOnce = false;
            if (liveZonePlaced) { g_playLiveZonePending = true; g_playLiveZoneConsoleLog = PlayJoin(m.gameRoot, L"console_mp.log"); g_playLiveZoneLogCheckMs = 0; }
            g_playGamePresented = true;
            g_playState = PLAY_RUNNING;
            g_playEarlyBuild = 2;
            char secs[32]; snprintf(secs, sizeof secs, "%.1f", (GetTickCount64() - g_playEarlyLaunchMs) / 1000.0);
            PlayAppendLog(std::string("[EARLY] build done ") + secs + " s after BO3 started; it goes into the map as soon as its zone module is up.");
            PlayLoadWaitBegin("Build done. BO3 is loading the map");
            g_playThreadDone = true;
            return;
        }
        if (early.active) { early.ok = true; g_playLaunchHeld = false; g_playEarlyBuild = 0; }   // aborted: the normal launch below
        const bool forceReload = g_playForceReloadOnce.exchange(false);   // a live bake the swap could not take
        if (g_playJoinMode || !PlayWarmUpdateGame(m, anyBuild || liveZonePlaced || forceReload))   // a join always starts a fresh game at the menu
        {
            if (liveZonePlaced && !g_playJoinMode) { g_playLiveZonePending = true; g_playLiveZoneLaunchMs = GetTickCount64(); g_playLiveZoneConsoleLog = PlayJoin(m.gameRoot, L"console_mp.log"); g_playLiveZoneLogCheckMs = 0; }
            // If a tracked instance exists but the reload could not be delivered, close only that instance before relaunching,
            // and wait for it to go: PlayLaunchGame refuses while any BlackOps3.exe is still listed (Steam's duplicate session).
            HANDLE old = nullptr;
            g_playParked = false;
            {
                std::lock_guard<std::mutex> lock(g_playMutex);
                old = g_playGameProcess;
                g_playGameProcess = nullptr; g_playGamePid = 0; g_playGameOwnedByRadiant = false;
            }
            PlayDetachEmbeddedGame();
            if (old)
            {
                if (WaitForSingleObject(old, 0) == WAIT_TIMEOUT)
                {
                    PlayAppendLog("[WARM] Closing the running BO3 for a clean relaunch.");
                    PlayNote("Closing the running BO3 to start it fresh.");
                    TerminateProcess(old, 0);
                    WaitForSingleObject(old, 10000);
                }
                CloseHandle(old);
            }
            PlayLaunchGame(m);
        }
        g_playThreadDone = true;
    }

    const char* PlayStateName(int state)
    {
        switch (state)
        {
            case PLAY_SAVING: return "Saving"; case PLAY_PREPARING: return "Preparing"; case PLAY_COMPILING: return "Compiling";
            case PLAY_LINKING: return "Linking"; case PLAY_LAUNCHING: return "Launching"; case PLAY_RUNNING: return "Running";
            case PLAY_FAILED: return "Failed"; case PLAY_STOPPED: return "Stopped"; default: return "Idle";
        }
    }

    bool PlayBusy()
    {
        int s = g_playState.load();
        // A resident BO3 session is not "busy": Keep Warm exists so Play can be pressed again to build the delta and reload that process.
        return s == PLAY_SAVING || s == PLAY_PREPARING || s == PLAY_COMPILING || s == PLAY_LINKING || s == PLAY_LAUNCHING;
    }

    void PlayStop(bool userRequested = true)
    {
        g_playCancel = true;
        PlayReleaseGameControls(false);
        HANDLE tool = nullptr, game = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            tool = g_playToolProcess; game = g_playGameProcess;
        }
        if (tool && WaitForSingleObject(tool, 0) == WAIT_TIMEOUT) TerminateProcess(tool, ERROR_CANCELLED);
        PlayDetachEmbeddedGame();
        if (game && WaitForSingleObject(game, 0) == WAIT_TIMEOUT) TerminateProcess(game, 0);
        PlayCloseGameJob();
        g_playParked = false;
        if (userRequested) PlayAppendLog("[STOP] BO3 test session closed from Radiant.");
        if (userRequested && PlayBusy()) PlayNote("Stopped.");
        PlayWaitEnd("build"); PlayWaitEnd("join");
        if (g_playState != PLAY_FAILED) g_playState = PLAY_STOPPED;
        g_playGameMap.clear();
        g_playPrewarmMap.clear();
        g_playGamePresented = false;
        g_playPrewarmed = false;
        g_playPrewarmWindowHidden = false;
    }

    struct PlayEntitySnapshot
    {
        std::string guid;
        std::vector<std::pair<std::string, std::string>> kv;
        bool hasBounds = false;
        float mins[3] = {0,0,0};
        float maxs[3] = {0,0,0};
        bool fromPrefab = false;  // read from a misc_prefab's .map: origin is prefab-local, no bounds
    };

    struct PlayPreflightReport
    {
        int blockers = 0;
        int warnings = 0;
        std::vector<std::string> lines;
    };

    std::string PlayLowerAscii(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)tolower(c); });
        return s;
    }

    std::string PlayTrim(std::string s)
    {
        size_t a = 0, b = s.size();
        while (a < b && isspace((unsigned char)s[a])) ++a;
        while (b > a && isspace((unsigned char)s[b - 1])) --b;
        return s.substr(a, b - a);
    }

    void PlayParseEntityKvs(const std::string& text, std::vector<std::pair<std::string, std::string>>& out)
    {
        size_t p = 0;
        while (p < text.size())
        {
            size_t eol = text.find_first_of("\r\n", p);
            std::string line = text.substr(p, eol == std::string::npos ? std::string::npos : eol - p);
            p = eol == std::string::npos ? text.size() : eol + 1;
            size_t q1 = line.find('"');
            if (q1 == std::string::npos) continue;
            size_t q2 = line.find('"', q1 + 1);
            if (q2 == std::string::npos) continue;
            size_t q3 = line.find('"', q2 + 1);
            if (q3 == std::string::npos) continue;
            size_t q4 = line.find('"', q3 + 1);
            if (q4 == std::string::npos) continue;
            std::string key = PlayLowerAscii(line.substr(q1 + 1, q2 - q1 - 1));
            std::string value = line.substr(q3 + 1, q4 - q3 - 1);
            out.push_back({key, value});
        }
    }

    std::string PlayKv(const PlayEntitySnapshot& e, const char* key)
    {
        std::string k = PlayLowerAscii(key ? key : "");
        for (const auto& it : e.kv) if (it.first == k) return it.second;
        return {};
    }

    bool PlayParseVec3(const std::string& s, float out[3])
    {
        return sscanf(s.c_str(), "%f %f %f", &out[0], &out[1], &out[2]) == 3;
    }

    bool PlayPointInside(const float p[3], const PlayEntitySnapshot& vol, float eps = 8.0f)
    {
        if (!vol.hasBounds) return false;
        return p[0] >= vol.mins[0] - eps && p[0] <= vol.maxs[0] + eps &&
               p[1] >= vol.mins[1] - eps && p[1] <= vol.maxs[1] + eps &&
               p[2] >= vol.mins[2] - eps && p[2] <= vol.maxs[2] + eps;
    }

    std::string PlayReadTextFile(const std::wstring& path, size_t maxBytes = 4 * 1024 * 1024)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return {};
        LARGE_INTEGER li = {};
        if (!GetFileSizeEx(h, &li) || li.QuadPart <= 0 || (uint64_t)li.QuadPart > maxBytes)
        { CloseHandle(h); return {}; }
        std::string out((size_t)li.QuadPart, '\0');
        DWORD got = 0;
        BOOL ok = ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr);
        CloseHandle(h);
        if (!ok) return {};
        out.resize(got);
        return out;
    }

    // The last maxBytes of a file another process keeps open for writing (the game's console log).
    std::string PlayReadFileTail(const std::wstring& path, size_t maxBytes)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return {};
        LARGE_INTEGER li = {};
        if (!GetFileSizeEx(h, &li) || li.QuadPart <= 0) { CloseHandle(h); return {}; }
        LARGE_INTEGER start = {}; start.QuadPart = (uint64_t)li.QuadPart > maxBytes ? li.QuadPart - (LONGLONG)maxBytes : 0;
        if (!SetFilePointerEx(h, start, nullptr, FILE_BEGIN)) { CloseHandle(h); return {}; }
        std::string out((size_t)(li.QuadPart - start.QuadPart), '\0');
        DWORD got = 0;
        BOOL ok = !out.empty() && ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr);
        CloseHandle(h);
        if (!ok) return {};
        out.resize(got);
        return out;
    }

    bool PlayWriteTextFile(const std::wstring& path, const std::string& text)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        size_t off = 0;
        bool ok = true;
        while (off < text.size())
        {
            DWORD want = (DWORD)(std::min<size_t>)(text.size() - off, 1u << 20);
            DWORD wrote = 0;
            if (!WriteFile(h, text.data() + off, want, &wrote, nullptr) || !wrote) { ok = false; break; }
            off += wrote;
        }
        FlushFileBuffers(h);
        CloseHandle(h);
        return ok;
    }

    bool PlayRemoveMarkedBlock(std::string& text, const char* beginMarker, const char* endMarker)
    {
        bool changed = false;
        for (;;)
        {
            size_t a = text.find(beginMarker);
            if (a == std::string::npos) break;
            size_t b = text.find(endMarker, a);
            if (b == std::string::npos) break;
            b += strlen(endMarker);
            while (b < text.size() && (text[b] == '\r' || text[b] == '\n')) ++b;
            size_t line = text.rfind('\n', a);
            if (line != std::string::npos) a = line + 1;
            text.erase(a, b - a);
            changed = true;
        }
        return changed;
    }

    std::wstring PlayMapGscPath(const PlayMapInfo& m)
    {
        std::wstring user = PlayJoin(PlayJoin(PlayJoin(m.usermapRoot, L"scripts"), L"zm"), m.mapName + L".gsc");
        if (PlayFileExists(user)) return user;
        std::wstring raw = PlayJoin(PlayJoin(PlayJoin(PlayJoin(PlayJoin(m.gameRoot, L"share"), L"raw"), L"scripts"), L"zm"), m.mapName + L".gsc");
        if (PlayFileExists(raw)) return raw;
        return {};
    }

    bool PlayConfigureAutoRestart(const PlayMapInfo& m, bool enable, std::string& detail)
    {
        static const char* kCallBegin = "// <BETTERRADIANT_PLAYTEST_AUTORESTART_CALL>";
        static const char* kCallEnd   = "// </BETTERRADIANT_PLAYTEST_AUTORESTART_CALL>";
        static const char* kFuncBegin = "// <BETTERRADIANT_PLAYTEST_AUTORESTART_FUNCTION>";
        static const char* kFuncEnd   = "// </BETTERRADIANT_PLAYTEST_AUTORESTART_FUNCTION>";
        std::wstring gscPath = PlayMapGscPath(m);
        if (gscPath.empty()) { detail = "map GSC not found"; return false; }
        std::string original = PlayReadTextFile(gscPath, 8 * 1024 * 1024);
        if (original.empty()) { detail = "map GSC could not be read"; return false; }
        std::string text = original;
        PlayRemoveMarkedBlock(text, kCallBegin, kCallEnd);
        PlayRemoveMarkedBlock(text, kFuncBegin, kFuncEnd);

        if (enable)
        {
            std::string low = PlayLowerAscii(text);
            if (low.find("function betterradiant_playtest_auto_restart") != std::string::npos)
            {
                detail = "map already defines betterradiant_playtest_auto_restart outside BetterRadiant markers";
                return false;
            }
            size_t fn = low.find("function main");
            size_t brace = fn == std::string::npos ? std::string::npos : text.find('{', fn);
            if (brace == std::string::npos) { detail = "function main() was not found in map GSC"; return false; }
            size_t insertAt = brace + 1;
            size_t userMain = low.find("zm_usermap::main", brace);
            if (userMain != std::string::npos)
            {
                size_t semi = text.find(';', userMain);
                if (semi != std::string::npos) insertAt = semi + 1;
            }
            const std::string callBlock =
                "\r\n\t// <BETTERRADIANT_PLAYTEST_AUTORESTART_CALL>\r\n"
                "\tlevel thread betterradiant_playtest_auto_restart();\r\n"
                "\t// </BETTERRADIANT_PLAYTEST_AUTORESTART_CALL>";
            text.insert(insertAt, callBlock);
            const std::string funcBlock =
                "\r\n\r\n// <BETTERRADIANT_PLAYTEST_AUTORESTART_FUNCTION>\r\n"
                "function betterradiant_playtest_auto_restart()\r\n"
                "{\r\n"
                "\tlevel waittill( \"end_game\" );\r\n"
                "\twait 0.05;\r\n"
                "\tmap_restart( true );\r\n"
                "}\r\n"
                "// </BETTERRADIANT_PLAYTEST_AUTORESTART_FUNCTION>\r\n";
            // Strip the trailing newlines first: the block's own two leading ones survive PlayRemoveMarkedBlock, so the file grew a blank line
            // per Play and the live zone patch relinked each time. Idempotent: with several hooks on, strip-and-append rotated their order
            // every Play, so the map GSC changed, the live zone relinked and the warm reload lost the hot path.
            if (original.find(callBlock) != std::string::npos && original.find(funcBlock) != std::string::npos)
            {
                detail = "already installed";
                return true;
            }
            while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '\t')) text.pop_back();
            text += "\r\n";
            text += funcBlock;
        }

        if (text == original)
        {
            detail = enable ? "already installed" : "already disabled";
            return true;
        }
        if (enable)
        {
            std::wstring backup = gscPath + L".betterradiant_pre_autorestart.bak";
            CopyFileW(gscPath.c_str(), backup.c_str(), TRUE); // safety backup once, marker removal is the normal revert path
        }
        if (!PlayWriteTextFile(gscPath, text)) { detail = "could not write map GSC"; return false; }
        detail = enable ? "installed reversible end_game -> map_restart test hook" : "removed Play Test auto-restart hook";
        return true;
    }

    bool PlayConfigureLivePreview(const PlayMapInfo& m, bool enable, std::string& detail)
    {
        static const char* kImportBegin = "// <BETTERRADIANT_PLAYTEST_LIVEPREVIEW_IMPORT>";
        static const char* kImportEnd   = "// </BETTERRADIANT_PLAYTEST_LIVEPREVIEW_IMPORT>";
        static const char* kCallBegin   = "// <BETTERRADIANT_PLAYTEST_LIVEPREVIEW_CALL>";
        static const char* kCallEnd     = "// </BETTERRADIANT_PLAYTEST_LIVEPREVIEW_CALL>";
        static const char* kFuncBegin   = "// <BETTERRADIANT_PLAYTEST_LIVEPREVIEW_FUNCTION>";
        static const char* kFuncEnd     = "// </BETTERRADIANT_PLAYTEST_LIVEPREVIEW_FUNCTION>";
        std::wstring gscPath = PlayMapGscPath(m);
        if (gscPath.empty()) { detail = "map GSC not found"; return false; }
        std::string original = PlayReadTextFile(gscPath, 8 * 1024 * 1024);
        if (original.empty()) { detail = "map GSC could not be read"; return false; }
        std::string text = original;
        PlayRemoveMarkedBlock(text, kImportBegin, kImportEnd);
        PlayRemoveMarkedBlock(text, kCallBegin, kCallEnd);
        PlayRemoveMarkedBlock(text, kFuncBegin, kFuncEnd);

        if (enable)
        {
            std::string low = PlayLowerAscii(text);
            if (low.find("function betterradiant_playtest_live_preview") != std::string::npos)
            {
                detail = "map already defines betterradiant_playtest_live_preview outside BetterRadiant markers";
                return false;
            }
            size_t fn = low.find("function main");
            size_t brace = fn == std::string::npos ? std::string::npos : text.find('{', fn);
            if (brace == std::string::npos) { detail = "function main() was not found in map GSC"; return false; }
            size_t insertAt = brace + 1;
            size_t userMain = low.find("zm_usermap::main", brace);
            if (userMain != std::string::npos)
            {
                size_t semi = text.find(';', userMain);
                if (semi != std::string::npos) insertAt = semi + 1;
            }
            const std::string callBlock =
                "\r\n\t// <BETTERRADIANT_PLAYTEST_LIVEPREVIEW_CALL>\r\n"
                "\tlevel thread betterradiant_playtest_live_preview();\r\n"
                "\t// </BETTERRADIANT_PLAYTEST_LIVEPREVIEW_CALL>";
            text.insert(insertAt, callBlock);

            // Preview proxy, not fake BSP: Play Test streams the selected Radiant brush bounds through developer dvars and the warm map redraws
            // them every server frame interval. Smart Play is the commit step for real BSP, materials, collision, Umbra, lighting and nav.
            const std::string funcBlock =
                "\r\n"
                "// <BETTERRADIANT_PLAYTEST_LIVEPREVIEW_FUNCTION>\r\n"
                "function betterradiant_playtest_live_preview()\r\n"
                "{\r\n"
                "\tlevel.br_live_gen = -1;\r\n"
                "\tlevel.br_live_edges = [];\r\n"
                "\tlevel.br_live_mgen = -1;\r\n"
                "\tlevel.br_live_models = [];\r\n"
                "\tlevel.br_live_mcount = 0;\r\n"
                "\tlevel.br_live_sgen = -1;\r\n"
                "\tlevel.br_live_spots = [];\r\n"
                "\tlevel.br_live_scount = 0;\r\n"
                "\tlevel.br_live_spot_retry = 0;\r\n"
                "\tlevel.br_live_spot_until = 0;\r\n"
                "\tfor ( ;; )\r\n"
                "\t{\r\n"
                // live spots: br_live_s<i> = "from origin|from angles|to origin|to angles" per script_struct of a moved prefab, applied when
                // br_live_sgen changes and once a second for a minute while a spot has no entity yet (the scripts spawn their things after this thread starts)
                "\t\tsgen = GetDvarInt( \"br_live_sgen\", 0 );\r\n"
                "\t\tif ( sgen != level.br_live_sgen || ( level.br_live_spot_retry > 0 && GetTime() >= level.br_live_spot_retry ) )\r\n"
                "\t\t{\r\n"
                "\t\t\tlevel.br_live_sgen = sgen;\r\n"
                "\t\t\tbetterradiant_playtest_live_spots();\r\n"
                "\t\t}\r\n"
                // live models: br_live_m<i> = "model|x y z|pitch yaw roll|scale", a script_model per index, spawned, re-posed or deleted when
                // br_live_mgen changes (independent of br_live_enable, which the Enhanced build keeps off because it draws brushes natively)
                "\t\tmgen = GetDvarInt( \"br_live_mgen\", 0 );\r\n"
                "\t\tif ( mgen != level.br_live_mgen )\r\n"
                "\t\t{\r\n"
                "\t\t\tlevel.br_live_mgen = mgen;\r\n"
                "\t\t\tmn = GetDvarInt( \"br_live_mn\", 0 );\r\n"
                // debug prints go through IPrintLn: it is the one script print that lands in console_mp.log ("[msg]"), println never does
                "\t\t\tlevel.br_live_debug = GetDvarInt( \"br_live_debug\", 0 ) != 0;\r\n"
                "\t\t\tif ( level.br_live_debug )\r\n"
                "\t\t\t\tIPrintLn( \"[BR LIVE] models generation \" + mgen + \": \" + mn + \" record(s), \" + level.br_live_mcount + \" before\" );\r\n"
                "\t\t\tfor ( i = 0; i < mn || i < level.br_live_mcount; i++ )\r\n"
                "\t\t\t{\r\n"
                "\t\t\t\tf = [];\r\n"
                "\t\t\t\tif ( i < mn )\r\n"
                "\t\t\t\t{\r\n"
                "\t\t\t\t\ts = GetDvarString( \"br_live_m\" + i );\r\n"
                "\t\t\t\t\tif ( isdefined( s ) && s != \"\" )\r\n"
                "\t\t\t\t\t\tf = StrTok( s, \"|\" );\r\n"
                "\t\t\t\t}\r\n"
                "\t\t\t\tif ( f.size < 4 )\r\n"
                "\t\t\t\t{\r\n"
                "\t\t\t\t\tif ( isdefined( level.br_live_models[ i ] ) )\r\n"
                "\t\t\t\t\t{\r\n"
                "\t\t\t\t\t\tlevel.br_live_models[ i ] Delete();\r\n"
                "\t\t\t\t\t\tlevel.br_live_models[ i ] = undefined;\r\n"
                "\t\t\t\t\t\tif ( level.br_live_debug )\r\n"
                "\t\t\t\t\t\t\tIPrintLn( \"[BR LIVE] model \" + i + \" deleted\" );\r\n"
                "\t\t\t\t\t}\r\n"
                "\t\t\t\t\tcontinue;\r\n"
                "\t\t\t\t}\r\n"
                "\t\t\t\to = StrTok( f[ 1 ], \" \" );\r\n"
                "\t\t\t\ta = StrTok( f[ 2 ], \" \" );\r\n"
                "\t\t\t\tif ( o.size < 3 || a.size < 3 )\r\n"
                "\t\t\t\t\tcontinue;\r\n"
                "\t\t\t\torigin = ( float( o[ 0 ] ), float( o[ 1 ] ), float( o[ 2 ] ) );\r\n"
                "\t\t\t\tangles = ( float( a[ 0 ] ), float( a[ 1 ] ), float( a[ 2 ] ) );\r\n"
                "\t\t\t\tif ( !isdefined( level.br_live_models[ i ] ) )\r\n"
                "\t\t\t\t{\r\n"
                "\t\t\t\t\tlevel.br_live_models[ i ] = Spawn( \"script_model\", origin );\r\n"
                "\t\t\t\t\tlevel.br_live_models[ i ].br_live_model = \"\";\r\n"
                "\t\t\t\t\tlevel.br_live_models[ i ].br_live_scale = 1;\r\n"
                "\t\t\t\t\tif ( level.br_live_debug )\r\n"
                "\t\t\t\t\t\tIPrintLn( \"[BR LIVE] model \" + i + \" spawned: \" + f[ 0 ] + \" at \" + origin );\r\n"
                "\t\t\t\t}\r\n"
                "\t\t\t\te = level.br_live_models[ i ];\r\n"
                "\t\t\t\tif ( e.br_live_model != f[ 0 ] )\r\n"
                "\t\t\t\t{\r\n"
                "\t\t\t\t\te SetModel( f[ 0 ] );\r\n"
                "\t\t\t\t\te.br_live_model = f[ 0 ];\r\n"
                "\t\t\t\t}\r\n"
                // DontInterpolate: the client would slide the model from its old spot over a server frame, a Radiant move is a teleport like the brushes
                "\t\t\t\te DontInterpolate();\r\n"
                "\t\t\t\te.origin = origin;\r\n"
                "\t\t\t\te.angles = angles;\r\n"
                "\t\t\t\tif ( level.br_live_debug )\r\n"
                "\t\t\t\t\tIPrintLn( \"[BR LIVE] model \" + i + \" posed: \" + e.model + \" at \" + e.origin + \" ent \" + e GetEntityNumber() );\r\n"
                "\t\t\t\tsc = float( f[ 3 ] );\r\n"
                "\t\t\t\tif ( sc > 0 && sc != e.br_live_scale )\r\n"
                "\t\t\t\t{\r\n"
                "\t\t\t\t\te SetScale( sc );\r\n"
                "\t\t\t\t\te.br_live_scale = sc;\r\n"
                "\t\t\t\t}\r\n"
                "\t\t\t}\r\n"
                "\t\t\tlevel.br_live_mcount = mn;\r\n"
                "\t\t}\r\n"
                "\t\tif ( GetDvarInt( \"br_live_enable\", 0 ) != 0 )\r\n"
                "\t\t{\r\n"
                "\t\t\tif ( !isdefined( level.br_live_receiver_seen ) )\r\n"
                "\t\t\t{\r\n"
                "\t\t\t\tlevel.br_live_receiver_seen = true;\r\n"
                "\t\t\t\tIPrintLnBold( \"BetterRadiant live preview on\" );\r\n"
                "\t\t\t}\r\n"
                "\t\t\tgen = GetDvarInt( \"br_live_gen\", 0 );\r\n"
                "\t\t\tif ( gen != level.br_live_gen )\r\n"
                "\t\t\t{\r\n"
                "\t\t\t\tlevel.br_live_gen = gen;\r\n"
                "\t\t\t\tlevel.br_live_edges = [];\r\n"
                "\t\t\t\tn = GetDvarInt( \"br_live_n\", 0 );\r\n"
                "\t\t\t\tfor ( i = 0; i < n; i++ )\r\n"
                "\t\t\t\t{\r\n"
                "\t\t\t\t\ts = GetDvarString( \"br_live_e\" + i );\r\n"
                "\t\t\t\t\tif ( !isdefined( s ) || s == \"\" )\r\n"
                "\t\t\t\t\t\tcontinue;\r\n"
                "\t\t\t\t\tsegs = StrTok( s, \"|\" );\r\n"
                "\t\t\t\t\tfor ( j = 0; j < segs.size; j++ )\r\n"
                "\t\t\t\t\t{\r\n"
                "\t\t\t\t\t\tc = StrTok( segs[ j ], \" \" );\r\n"
                "\t\t\t\t\t\tif ( c.size < 6 )\r\n"
                "\t\t\t\t\t\t\tcontinue;\r\n"
                "\t\t\t\t\t\tlevel.br_live_edges[ level.br_live_edges.size ] = ( int( c[ 0 ] ), int( c[ 1 ] ), int( c[ 2 ] ) );\r\n"
                "\t\t\t\t\t\tlevel.br_live_edges[ level.br_live_edges.size ] = ( int( c[ 3 ] ), int( c[ 4 ] ), int( c[ 5 ] ) );\r\n"
                "\t\t\t\t\t}\r\n"
                "\t\t\t\t}\r\n"
                "\t\t\t}\r\n"
                "\t\t\t/#\r\n"
                "\t\t\tfor ( i = 0; i + 1 < level.br_live_edges.size; i += 2 )\r\n"
                "\t\t\t{\r\n"
                "\t\t\t\tline( level.br_live_edges[ i ], level.br_live_edges[ i + 1 ], ( 0.10, 1.00, 0.20 ), 1.0, false, 2 );\r\n"
                "\t\t\t}\r\n"
                "\t\t\t#/\r\n"
                "\t\t}\r\n"
                "\t\twait 0.05;\r\n"
                "\t}\r\n"
                "}\r\n"
                "\r\n"
                "function betterradiant_playtest_live_vec( text )\r\n"
                "{\r\n"
                "\tc = StrTok( text, \" \" );\r\n"
                "\tif ( c.size < 3 )\r\n"
                "\t\treturn ( 0, 0, 0 );\r\n"
                "\treturn ( float( c[ 0 ] ), float( c[ 1 ] ), float( c[ 2 ] ) );\r\n"
                "}\r\n"
                "\r\n"
                "function betterradiant_playtest_live_spots()\r\n"
                "{\r\n"
                "\tsn = GetDvarInt( \"br_live_sn\", 0 );\r\n"
                "\twaiting = false;\r\n"
                "\tfor ( i = 0; i < sn || i < level.br_live_scount; i++ )\r\n"
                "\t{\r\n"
                "\t\tf = [];\r\n"
                "\t\tif ( i < sn )\r\n"
                "\t\t{\r\n"
                "\t\t\ts = GetDvarString( \"br_live_s\" + i );\r\n"
                "\t\t\tif ( isdefined( s ) && s != \"\" )\r\n"
                "\t\t\t\tf = StrTok( s, \"|\" );\r\n"
                "\t\t}\r\n"
                "\t\tspot = level.br_live_spots[ i ];\r\n"
                "\t\tif ( f.size < 4 )\r\n"
                "\t\t{\r\n"
                "\t\t\tif ( isdefined( spot ) )\r\n"
                "\t\t\t{\r\n"
                "\t\t\t\tbetterradiant_playtest_live_spot_place( spot, spot.from_origin, spot.from_angles );\r\n"
                "\t\t\t\tlevel.br_live_spots[ i ] = undefined;\r\n"
                "\t\t\t}\r\n"
                "\t\t\tcontinue;\r\n"
                "\t\t}\r\n"
                "\t\tfrom_origin = betterradiant_playtest_live_vec( f[ 0 ] );\r\n"
                "\t\tfrom_angles = betterradiant_playtest_live_vec( f[ 1 ] );\r\n"
                "\t\tto_origin = betterradiant_playtest_live_vec( f[ 2 ] );\r\n"
                "\t\tto_angles = betterradiant_playtest_live_vec( f[ 3 ] );\r\n"
                "\t\tif ( isdefined( spot ) && DistanceSquared( spot.from_origin, from_origin ) > 0.25 )\r\n"
                "\t\t{\r\n"
                "\t\t\tbetterradiant_playtest_live_spot_place( spot, spot.from_origin, spot.from_angles );\r\n"
                "\t\t\tspot = undefined;\r\n"
                "\t\t}\r\n"
                "\t\tif ( !isdefined( spot ) )\r\n"
                "\t\t{\r\n"
                "\t\t\tspot = SpawnStruct();\r\n"
                "\t\t\tspot.from_origin = from_origin;\r\n"
                "\t\t\tspot.from_angles = from_angles;\r\n"
                "\t\t\tspot.items = [];\r\n"
                "\t\t\tspot.ents = 0;\r\n"
                "\t\t\tspot.structs_done = false;\r\n"
                "\t\t\tlevel.br_live_spots[ i ] = spot;\r\n"
                "\t\t}\r\n"
                "\t\tif ( spot.ents == 0 )\r\n"
                "\t\t\tbetterradiant_playtest_live_spot_collect( spot );\r\n"
                "\t\tif ( spot.ents == 0 )\r\n"
                "\t\t\twaiting = true;\r\n"
                "\t\tbetterradiant_playtest_live_spot_place( spot, to_origin, to_angles );\r\n"
                "\t}\r\n"
                "\tlevel.br_live_scount = sn;\r\n"
                "\tlevel.br_live_spot_retry = 0;\r\n"
                "\tif ( !waiting )\r\n"
                "\t\tlevel.br_live_spot_until = 0;\r\n"
                "\telse\r\n"
                "\t{\r\n"
                "\t\tif ( level.br_live_spot_until == 0 )\r\n"
                "\t\t\tlevel.br_live_spot_until = GetTime() + 60000;\r\n"
                "\t\tif ( GetTime() < level.br_live_spot_until )\r\n"
                "\t\t\tlevel.br_live_spot_retry = GetTime() + 1000;\r\n"
                "\t}\r\n"
                "}\r\n"
                "\r\n"
                // the struct itself (once) and every script-spawned thing standing on it: same x and y within a unit, from
                // 8 below to 128 above (a perk machine and its clip stand on the struct, its triggers 20 and 60 above)
                "function betterradiant_playtest_live_spot_collect( spot )\r\n"
                "{\r\n"
                "\tif ( !spot.structs_done )\r\n"
                "\t{\r\n"
                "\t\tspot.structs_done = true;\r\n"
                "\t\tif ( isdefined( level.struct ) )\r\n"
                "\t\t{\r\n"
                "\t\t\tforeach ( st in level.struct )\r\n"
                "\t\t\t{\r\n"
                "\t\t\t\tif ( !isdefined( st ) || !isdefined( st.origin ) || DistanceSquared( st.origin, spot.from_origin ) > 1 )\r\n"
                "\t\t\t\t\tcontinue;\r\n"
                "\t\t\t\titem = SpawnStruct();\r\n"
                "\t\t\t\titem.ref = st;\r\n"
                "\t\t\t\titem.is_struct = true;\r\n"
                "\t\t\t\titem.offset = ( 0, 0, 0 );\r\n"
                "\t\t\t\titem.angles = ( 0, 0, 0 );\r\n"
                "\t\t\t\tif ( isdefined( st.angles ) )\r\n"
                "\t\t\t\t\titem.angles = st.angles;\r\n"
                "\t\t\t\tspot.items[ spot.items.size ] = item;\r\n"
                "\t\t\t}\r\n"
                "\t\t}\r\n"
                "\t}\r\n"
                "\tents = GetEntArray();\r\n"
                "\tforeach ( e in ents )\r\n"
                "\t{\r\n"
                "\t\tif ( !isdefined( e ) || !isdefined( e.classname ) || !isdefined( e.origin ) || isdefined( e.br_live_model ) )\r\n"
                "\t\t\tcontinue;\r\n"
                "\t\tc = e.classname;\r\n"
                "\t\tif ( c != \"script_model\" && c != \"script_origin\" && c != \"script_brushmodel\" && !IsSubStr( c, \"trigger_\" ) )\r\n"
                "\t\t\tcontinue;\r\n"
                "\t\td = e.origin - spot.from_origin;\r\n"
                "\t\tif ( Abs( d[ 0 ] ) > 1 || Abs( d[ 1 ] ) > 1 || d[ 2 ] < -8 || d[ 2 ] > 128 )\r\n"
                "\t\t\tcontinue;\r\n"
                "\t\titem = SpawnStruct();\r\n"
                "\t\titem.ref = e;\r\n"
                "\t\titem.is_struct = false;\r\n"
                "\t\titem.offset = d;\r\n"
                "\t\titem.angles = e.angles;\r\n"
                "\t\tspot.items[ spot.items.size ] = item;\r\n"
                "\t\tspot.ents++;\r\n"
                "\t}\r\n"
                "}\r\n"
                "\r\n"
                "function betterradiant_playtest_live_spot_place( spot, origin, angles )\r\n"
                "{\r\n"
                "\tf0 = AnglesToForward( spot.from_angles );\r\n"
                "\tr0 = AnglesToRight( spot.from_angles );\r\n"
                "\tu0 = AnglesToUp( spot.from_angles );\r\n"
                "\tf1 = AnglesToForward( angles );\r\n"
                "\tr1 = AnglesToRight( angles );\r\n"
                "\tu1 = AnglesToUp( angles );\r\n"
                "\tturn = angles - spot.from_angles;\r\n"
                "\tforeach ( item in spot.items )\r\n"
                "\t{\r\n"
                "\t\tif ( !isdefined( item.ref ) )\r\n"
                "\t\t\tcontinue;\r\n"
                "\t\to = item.offset;\r\n"
                "\t\tpos = origin + f1 * VectorDot( o, f0 ) + r1 * VectorDot( o, r0 ) + u1 * VectorDot( o, u0 );\r\n"
                "\t\tif ( !item.is_struct && item.ref.classname == \"script_model\" )\r\n"
                "\t\t\titem.ref DontInterpolate();\r\n"
                "\t\titem.ref.origin = pos;\r\n"
                "\t\titem.ref.angles = item.angles + turn;\r\n"
                "\t}\r\n"
                "}\r\n"
                "// </BETTERRADIANT_PLAYTEST_LIVEPREVIEW_FUNCTION>\r\n";
            // Strip the trailing newlines first: the block's own two leading ones survive PlayRemoveMarkedBlock, so the file grew a blank line
            // per Play and the live zone patch relinked each time. Idempotent: with several hooks on, strip-and-append rotated their order
            // every Play, so the map GSC changed, the live zone relinked and the warm reload lost the hot path.
            if (original.find(callBlock) != std::string::npos && original.find(funcBlock) != std::string::npos)
            {
                detail = "already installed";
                return true;
            }
            while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '\t')) text.pop_back();
            text += "\r\n";
            text += funcBlock;
        }

        if (text == original)
        {
            detail = enable ? "already installed" : "already disabled";
            return true;
        }
        if (enable)
        {
            std::wstring backup = gscPath + L".betterradiant_pre_livepreview.bak";
            CopyFileW(gscPath.c_str(), backup.c_str(), TRUE);
        }
        if (!PlayWriteTextFile(gscPath, text)) { detail = "could not write map GSC"; return false; }
        detail = enable ? "installed reversible no-build brush wireframe hook" : "removed Play Test live-preview hook";
        return true;
    }

    // Startup black/fade bypass: stock _zm.gsc holds every player behind the InitialBlack menu for about 8 s. CloseMenu plus a fade-in every 20 ms flooded
    // the reliable buffer (EXE_ERR_RELIABLE_CYCLED_OUT), so this releases each player three times in the first second and fades in at most six times,
    // only while a stock fade menu exists, until passed_introscreen.
    bool PlayConfigureInstantStart(const PlayMapInfo& m, bool enable, std::string& detail)
    {
        static const char* kImportBegin = "// <BETTERRADIANT_PLAYTEST_INSTANTSTART_IMPORT>";
        static const char* kImportEnd   = "// </BETTERRADIANT_PLAYTEST_INSTANTSTART_IMPORT>";
        static const char* kCallBegin   = "// <BETTERRADIANT_PLAYTEST_INSTANTSTART_CALL>";
        static const char* kCallEnd     = "// </BETTERRADIANT_PLAYTEST_INSTANTSTART_CALL>";
        static const char* kFuncBegin   = "// <BETTERRADIANT_PLAYTEST_INSTANTSTART_FUNCTION>";
        static const char* kFuncEnd     = "// </BETTERRADIANT_PLAYTEST_INSTANTSTART_FUNCTION>";
        std::wstring gscPath = PlayMapGscPath(m);
        if (gscPath.empty()) { detail = "map GSC not found"; return false; }
        std::string original = PlayReadTextFile(gscPath, 8 * 1024 * 1024);
        if (original.empty()) { detail = "map GSC could not be read"; return false; }
        std::string text = original;
        PlayRemoveMarkedBlock(text, kImportBegin, kImportEnd);
        PlayRemoveMarkedBlock(text, kCallBegin, kCallEnd);
        PlayRemoveMarkedBlock(text, kFuncBegin, kFuncEnd);

        if (enable)
        {
            std::string low = PlayLowerAscii(text);
            if (low.find("function betterradiant_playtest_instant_start") != std::string::npos)
            {
                detail = "map already defines betterradiant_playtest_instant_start outside BetterRadiant markers";
                return false;
            }

            // lui::screen_fade_in needs the shared LUI namespace. Match an existing import with either slash, any case and spacing (a duplicate
            // #using is the one way this block could break a map's script compile).
            std::string usings = low;
            std::replace(usings.begin(), usings.end(), '/', '\\');
            usings.erase(std::remove_if(usings.begin(), usings.end(), [](char c) { return c == ' ' || c == '\t'; }), usings.end());
            bool importInserted = false;
            if (usings.find("#usingscripts\\shared\\lui_shared;") == std::string::npos)
            {
                importInserted = true;
                const std::string importBlock =
                    "// <BETTERRADIANT_PLAYTEST_INSTANTSTART_IMPORT>\r\n"
                    "#using scripts\\shared\\lui_shared;\r\n"
                    "// </BETTERRADIANT_PLAYTEST_INSTANTSTART_IMPORT>\r\n";
                size_t insertImport = 0;
                while (insertImport < text.size() && (text[insertImport] == '\r' || text[insertImport] == '\n')) ++insertImport;
                text.insert(insertImport, importBlock);
                low = PlayLowerAscii(text);
            }

            size_t fn = low.find("function main");
            size_t brace = fn == std::string::npos ? std::string::npos : text.find('{', fn);
            if (brace == std::string::npos) { detail = "function main() was not found in map GSC"; return false; }
            size_t insertAt = brace + 1;
            size_t userMain = low.find("zm_usermap::main", brace);
            if (userMain != std::string::npos)
            {
                size_t semi = text.find(';', userMain);
                if (semi != std::string::npos) insertAt = semi + 1;
            }
            const std::string callBlock =
                "\r\n\t// <BETTERRADIANT_PLAYTEST_INSTANTSTART_CALL>\r\n"
                "\tlevel thread betterradiant_playtest_instant_start();\r\n"
                "\t// </BETTERRADIANT_PLAYTEST_INSTANTSTART_CALL>";
            text.insert(insertAt, callBlock);

            const std::string funcBlock =
                "\r\n\r\n// <BETTERRADIANT_PLAYTEST_INSTANTSTART_FUNCTION>\r\n"
                "function betterradiant_playtest_instant_start()\r\n"
                "{\r\n"
                "\tlevel waittill( \"initial_players_connected\" );\r\n"
                "\tplayers = GetPlayers();\r\n"
                "\tfor ( i = 0; i < players.size; i++ )\r\n"
                "\t\tplayers[ i ] thread betterradiant_playtest_instant_start_player();\r\n"
                "}\r\n"
                "\r\n"
                "function betterradiant_playtest_instant_start_player()\r\n"
                "{\r\n"
                "\tself endon( \"disconnect\" );\r\n"
                "\tend_time = GetTime() + 15000;\r\n"
                "\tnext_release = 0;\r\n"
                "\treleases = 0;\r\n"
                "\tfades = 0;\r\n"
                "\twhile ( GetTime() < end_time )\r\n"
                "\t{\r\n"
                "\t\tif ( isdefined( level.passed_introscreen ) && level.passed_introscreen )\r\n"
                "\t\t\tbreak;\r\n"
                "\t\tif ( releases < 3 && GetTime() >= next_release )\r\n"
                "\t\t{\r\n"
                "\t\t\treleases++;\r\n"
                "\t\t\tnext_release = GetTime() + 500;\r\n"
                "\t\t\tself CloseMenu( \"InitialBlack\" );\r\n"
                "\t\t\tself FreezeControls( false );\r\n"
                "\t\t}\r\n"
                "\t\tif ( fades < 6 && isdefined( self.screen_fade_menus ) && isdefined( self.screen_fade_menus[ \"default\" ] ) )\r\n"
                "\t\t{\r\n"
                "\t\t\tfades++;\r\n"
                "\t\t\tself lui::screen_fade_in( 0, undefined );\r\n"
                "\t\t\twait 0.3;\r\n"
                "\t\t\tcontinue;\r\n"
                "\t\t}\r\n"
                "\t\twait 0.05;\r\n"
                "\t}\r\n"
                "\tself setClientUIVisibilityFlag( \"hud_visible\", 1 );\r\n"
                "\tself setClientUIVisibilityFlag( \"weapon_hud_visible\", 1 );\r\n"
                "}\r\n"
                "// </BETTERRADIANT_PLAYTEST_INSTANTSTART_FUNCTION>\r\n";
            // Strip the trailing newlines first: the block's own two leading ones survive PlayRemoveMarkedBlock, so the file grew a blank line
            // per Play and the live zone patch relinked each time. Idempotent: with several hooks on, strip-and-append rotated their order
            // every Play, so the map GSC changed, the live zone relinked and the warm reload lost the hot path.
            if (original.find(callBlock) != std::string::npos && original.find(funcBlock) != std::string::npos &&
                (!importInserted || original.find(kImportBegin) != std::string::npos))
            {
                detail = "already installed";
                return true;
            }
            while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '\t')) text.pop_back();
            text += "\r\n";
            text += funcBlock;
        }

        if (text == original)
        {
            detail = enable ? "already installed" : "already disabled";
            return true;
        }
        if (enable)
        {
            std::wstring backup = gscPath + L".betterradiant_pre_instantstart.bak";
            CopyFileW(gscPath.c_str(), backup.c_str(), TRUE);
        }
        if (!PlayWriteTextFile(gscPath, text)) { detail = "could not write map GSC"; return false; }
        detail = enable ? "installed reversible startup-black/fade bypass" : "removed Play Test instant-start hook";
        return true;
    }

    // Play From Here receiver: a level thread polls br_play_gen and once every player has spawned applies br_play_cam ("x y z pitch yaw", eye point) and
    // br_play_mode (walk|fly|off) three times, 0.25 s apart. walk = feet on the floor from a downward BulletTrace (eye minus 60 when nothing is below).
    bool PlayConfigurePlayHere(const PlayMapInfo& m, bool enable, std::string& detail)
    {
        static const char* kCallBegin = "// <BETTERRADIANT_PLAYTEST_PLAYHERE_CALL>";
        static const char* kCallEnd   = "// </BETTERRADIANT_PLAYTEST_PLAYHERE_CALL>";
        static const char* kFuncBegin = "// <BETTERRADIANT_PLAYTEST_PLAYHERE_FUNCTION>";
        static const char* kFuncEnd   = "// </BETTERRADIANT_PLAYTEST_PLAYHERE_FUNCTION>";
        std::wstring gscPath = PlayMapGscPath(m);
        if (gscPath.empty()) { detail = "map GSC not found"; return false; }
        std::string original = PlayReadTextFile(gscPath, 8 * 1024 * 1024);
        if (original.empty()) { detail = "map GSC could not be read"; return false; }
        std::string text = original;
        PlayRemoveMarkedBlock(text, kCallBegin, kCallEnd);
        PlayRemoveMarkedBlock(text, kFuncBegin, kFuncEnd);

        if (enable)
        {
            std::string low = PlayLowerAscii(text);
            if (low.find("function betterradiant_playtest_play_here") != std::string::npos)
            {
                detail = "map already defines betterradiant_playtest_play_here outside BetterRadiant markers";
                return false;
            }
            size_t fn = low.find("function main");
            size_t brace = fn == std::string::npos ? std::string::npos : text.find('{', fn);
            if (brace == std::string::npos) { detail = "function main() was not found in map GSC"; return false; }
            size_t insertAt = brace + 1;
            size_t userMain = low.find("zm_usermap::main", brace);
            if (userMain != std::string::npos)
            {
                size_t semi = text.find(';', userMain);
                if (semi != std::string::npos) insertAt = semi + 1;
            }
            const std::string callBlock =
                "\r\n\t// <BETTERRADIANT_PLAYTEST_PLAYHERE_CALL>\r\n"
                "\tlevel thread betterradiant_playtest_play_here();\r\n"
                "\t// </BETTERRADIANT_PLAYTEST_PLAYHERE_CALL>";
            text.insert(insertAt, callBlock);

            const std::string funcBlock =
                "\r\n\r\n// <BETTERRADIANT_PLAYTEST_PLAYHERE_FUNCTION>\r\n"
                "function betterradiant_playtest_play_here()\r\n"
                "{\r\n"
                "\tlevel.br_play_gen = -1;\r\n"
                "\tfor ( ;; )\r\n"
                "\t{\r\n"
                "\t\twait 0.05;\r\n"
                "\t\tkeep = GetDvarInt( \"br_play_keep\", 0 );\r\n"
                "\t\tif ( !isdefined( level.br_play_keep ) )\r\n"
                "\t\t\tlevel.br_play_keep = keep;\r\n"
                "\t\telse if ( keep !== level.br_play_keep )\r\n"
                "\t\t{\r\n"
                "\t\t\tlevel.br_play_keep = keep;\r\n"
                "\t\t\tkeeper = GetPlayers()[ 0 ];\r\n"
                "\t\t\tif ( isdefined( keeper ) && isdefined( keeper.sessionstate ) && keeper.sessionstate == \"playing\" && IsAlive( keeper ) )\r\n"
                "\t\t\t{\r\n"
                "\t\t\t\tkeep_eye = keeper GetEye();\r\n"
                "\t\t\t\tkeep_angles = keeper GetPlayerAngles();\r\n"
                "\t\t\t\tSetDvar( \"br_play_cam\", keep_eye[ 0 ] + \" \" + keep_eye[ 1 ] + \" \" + keep_eye[ 2 ] + \" \" + keep_angles[ 0 ] + \" \" + keep_angles[ 1 ] );\r\n"
                "\t\t\t\tif ( GetDvarString( \"br_play_mode\" ) != \"fly\" )\r\n"
                "\t\t\t\t\tSetDvar( \"br_play_mode\", \"walk\" );\r\n"
                "\t\t\t}\r\n"
                "\t\t}\r\n"
                "\t\tgen = GetDvarInt( \"br_play_gen\", 0 );\r\n"
                "\t\tif ( !isdefined( gen ) || gen === level.br_play_gen )\r\n"
                "\t\t\tcontinue;\r\n"
                "\t\tplayers = GetPlayers();\r\n"
                "\t\tif ( players.size == 0 )\r\n"
                "\t\t\tcontinue;\r\n"
                "\t\tready = true;\r\n"
                "\t\tfor ( i = 0; i < players.size; i++ )\r\n"
                "\t\t{\r\n"
                "\t\t\tif ( !isdefined( players[ i ].sessionstate ) || players[ i ].sessionstate != \"playing\" || !IsAlive( players[ i ] ) )\r\n"
                "\t\t\t\tready = false;\r\n"
                "\t\t}\r\n"
                "\t\tif ( !ready )\r\n"
                "\t\t\tcontinue;\r\n"
                "\t\tlevel.br_play_gen = gen;\r\n"
                "\t\tmode = GetDvarString( \"br_play_mode\" );\r\n"
                "\t\tcam = GetDvarString( \"br_play_cam\" );\r\n"
                "\t\tif ( !isdefined( mode ) || !isdefined( cam ) || mode == \"off\" || cam == \"\" )\r\n"
                "\t\t\tcontinue;\r\n"
                "\t\theld = false;\r\n"
                "\t\tend_time = GetTime() + 20000;\r\n"
                "\t\twhile ( isdefined( level.passed_introscreen ) && !level.passed_introscreen && GetTime() < end_time )\r\n"
                "\t\t{\r\n"
                "\t\t\tplayers = GetPlayers();\r\n"
                "\t\t\tfor ( i = 0; i < players.size; i++ )\r\n"
                "\t\t\t\tplayers[ i ] betterradiant_playtest_play_here_apply( cam, mode, true, !held );\r\n"
                "\t\t\theld = true;\r\n"
                "\t\t\twait 0.05;\r\n"
                "\t\t}\r\n"
                "\t\tif ( !held )\r\n"
                "\t\t{\r\n"
                "\t\t\tfor ( pass = 0; pass < 3; pass++ )\r\n"
                "\t\t\t{\r\n"
                "\t\t\t\tplayers = GetPlayers();\r\n"
                "\t\t\t\tfor ( i = 0; i < players.size; i++ )\r\n"
                "\t\t\t\t\tplayers[ i ] betterradiant_playtest_play_here_apply( cam, mode, false, true );\r\n"
                "\t\t\t\twait 0.25;\r\n"
                "\t\t\t}\r\n"
                "\t\t}\r\n"
                "\t}\r\n"
                "}\r\n"
                "\r\n"
                "function betterradiant_playtest_play_here_apply( cam, mode, hold, turn )\r\n"
                "{\r\n"
                "\tc = StrTok( cam, \" \" );\r\n"
                "\tif ( c.size < 5 )\r\n"
                "\t\treturn;\r\n"
                "\teye = ( Float( c[ 0 ] ), Float( c[ 1 ] ), Float( c[ 2 ] ) );\r\n"
                "\tangles = ( Float( c[ 3 ] ), Float( c[ 4 ] ), 0 );\r\n"
                "\torigin = eye - ( 0, 0, 60 );\r\n"
                "\tif ( mode == \"walk\" && !hold )\r\n"
                "\t{\r\n"
                "\t\ttrace = BulletTrace( eye, eye - ( 0, 0, 4096 ), false, self );\r\n"
                "\t\tif ( isdefined( trace[ \"fraction\" ] ) && trace[ \"fraction\" ] < 1 && isdefined( trace[ \"position\" ] ) )\r\n"
                "\t\t\torigin = trace[ \"position\" ] + ( 0, 0, 2 );\r\n"
                "\t}\r\n"
                "\tself SetVelocity( ( 0, 0, 0 ) );\r\n"
                "\tself SetOrigin( origin );\r\n"
                "\tif ( turn )\r\n"
                "\t\tself SetPlayerAngles( angles );\r\n"
                "}\r\n"
                "// </BETTERRADIANT_PLAYTEST_PLAYHERE_FUNCTION>\r\n";
            // Idempotent: with several hooks on, strip-and-append rotated their order every Play, so the map GSC changed, the live zone
            // relinked and the warm reload lost the hot path.
            if (original.find(callBlock) != std::string::npos && original.find(funcBlock) != std::string::npos)
            {
                detail = "already installed";
                return true;
            }
            while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '\t')) text.pop_back();
            text += "\r\n";
            text += funcBlock;
        }

        if (text == original)
        {
            detail = enable ? "already installed" : "already disabled";
            return true;
        }
        if (enable)
        {
            std::wstring backup = gscPath + L".betterradiant_pre_playhere.bak";
            CopyFileW(gscPath.c_str(), backup.c_str(), TRUE);
        }
        if (!PlayWriteTextFile(gscPath, text)) { detail = "could not write map GSC"; return false; }
        detail = enable ? "installed reversible spawn-at-camera receiver" : "removed Play Test spawn-at-camera receiver";
        return true;
    }

    // Disable AI receiver (retired): script SetDvar("ai_disableSpawn", 1) did not stop the spawns, so the bridge's zone module does
    // it (SharedState::disableAi). Play calls this with enable = false to take the block out of maps that got it.
    bool PlayConfigureDisableAi(const PlayMapInfo& m, bool enable, std::string& detail)
    {
        static const char* kCallBegin = "// <BETTERRADIANT_PLAYTEST_DISABLEAI_CALL>";
        static const char* kCallEnd   = "// </BETTERRADIANT_PLAYTEST_DISABLEAI_CALL>";
        static const char* kFuncBegin = "// <BETTERRADIANT_PLAYTEST_DISABLEAI_FUNCTION>";
        static const char* kFuncEnd   = "// </BETTERRADIANT_PLAYTEST_DISABLEAI_FUNCTION>";
        std::wstring gscPath = PlayMapGscPath(m);
        if (gscPath.empty()) { detail = "map GSC not found"; return false; }
        std::string original = PlayReadTextFile(gscPath, 8 * 1024 * 1024);
        if (original.empty()) { detail = "map GSC could not be read"; return false; }
        std::string text = original;
        PlayRemoveMarkedBlock(text, kCallBegin, kCallEnd);
        PlayRemoveMarkedBlock(text, kFuncBegin, kFuncEnd);

        if (enable)
        {
            std::string low = PlayLowerAscii(text);
            if (low.find("function betterradiant_playtest_disable_ai") != std::string::npos)
            {
                detail = "map already defines betterradiant_playtest_disable_ai outside BetterRadiant markers";
                return false;
            }
            size_t fn = low.find("function main");
            size_t brace = fn == std::string::npos ? std::string::npos : text.find('{', fn);
            if (brace == std::string::npos) { detail = "function main() was not found in map GSC"; return false; }
            size_t insertAt = brace + 1;
            size_t userMain = low.find("zm_usermap::main", brace);
            if (userMain != std::string::npos)
            {
                size_t semi = text.find(';', userMain);
                if (semi != std::string::npos) insertAt = semi + 1;
            }
            const std::string callBlock =
                "\r\n\t// <BETTERRADIANT_PLAYTEST_DISABLEAI_CALL>\r\n"
                "\tlevel thread betterradiant_playtest_disable_ai();\r\n"
                "\t// </BETTERRADIANT_PLAYTEST_DISABLEAI_CALL>";
            text.insert(insertAt, callBlock);

            const std::string funcBlock =
                "\r\n\r\n// <BETTERRADIANT_PLAYTEST_DISABLEAI_FUNCTION>\r\n"
                "function betterradiant_playtest_disable_ai()\r\n"
                "{\r\n"
                "\tfor ( ;; )\r\n"
                "\t{\r\n"
                "\t\tif ( GetDvarInt( \"br_disable_ai\", 0 ) == 1 && GetDvarInt( \"ai_disableSpawn\", 0 ) != 1 )\r\n"
                "\t\t\tSetDvar( \"ai_disableSpawn\", 1 );\r\n"
                "\t\twait 0.25;\r\n"
                "\t}\r\n"
                "}\r\n"
                "// </BETTERRADIANT_PLAYTEST_DISABLEAI_FUNCTION>\r\n";
            // Idempotent, like the other hooks: an unchanged file keeps its write time, so Smart Play does not relink.
            if (original.find(callBlock) != std::string::npos && original.find(funcBlock) != std::string::npos)
            {
                detail = "already installed";
                return true;
            }
            while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '\t')) text.pop_back();
            text += "\r\n";
            text += funcBlock;
        }

        if (text == original)
        {
            detail = enable ? "already installed" : "already disabled";
            return true;
        }
        if (enable)
        {
            std::wstring backup = gscPath + L".betterradiant_pre_disableai.bak";
            CopyFileW(gscPath.c_str(), backup.c_str(), TRUE);
        }
        if (!PlayWriteTextFile(gscPath, text)) { detail = "could not write map GSC"; return false; }
        detail = enable ? "installed reversible ai_disableSpawn receiver" : "removed Play Test ai_disableSpawn receiver";
        return true;
    }

    std::string PlayExtractDefaultStartLocation(const std::string& gsc)
    {
        std::string low = PlayLowerAscii(gsc);
        size_t p = low.find("default_start_location");
        if (p == std::string::npos)
        {
            // Older BO3 usermap templates identify the initially-active zone with init_zones[0] instead.
            p = low.find("init_zones");
            while (p != std::string::npos)
            {
                size_t lb = low.find('[', p);
                size_t rb = lb == std::string::npos ? std::string::npos : low.find(']', lb + 1);
                if (lb == std::string::npos || rb == std::string::npos) break;
                std::string index = PlayTrim(low.substr(lb + 1, rb - lb - 1));
                if (index == "0") { p = rb; break; }
                p = low.find("init_zones", rb + 1);
            }
        }
        if (p == std::string::npos) return {};
        size_t q1 = gsc.find('"', p);
        if (q1 == std::string::npos) return {};
        size_t q2 = gsc.find('"', q1 + 1);
        if (q2 == std::string::npos) return {};
        return gsc.substr(q1 + 1, q2 - q1 - 1);
    }

    void PlayPreflightLine(PlayPreflightReport& r, const char* level, const std::string& message)
    {
        std::string tag = level ? level : "INFO";
        if (tag == "BLOCK") ++r.blockers;
        else if (tag == "WARN") ++r.warnings;
        r.lines.push_back("[" + tag + "] " + message);
    }

    bool PlaySnapshotEntities(std::vector<PlayEntitySnapshot>& out, PlayPreflightReport& report)
    {
        if (!rp::host || !rp::host->ForEachEntity || !rp::host->WriteEntityText || !rp::host->FreeString)
        {
            PlayPreflightLine(report, "BLOCK", "The Radiant host does not expose entity inspection; map setup cannot be verified safely.");
            return false;
        }

        const bool canBrushBounds = rp::Has(&rp::host->BrushBounds) && rp::host->BrushBounds && rp::host->ForEachEntityBrush;
        rp::ForEachEntity([&](ModToolsEntity ent)
        {
            PlayEntitySnapshot s;
            if (rp::host->EntityGuid) { char guid[40] = {}; if (rp::host->EntityGuid(rp::self, ent, guid)) s.guid = guid; }
            char* raw = rp::host->WriteEntityText(rp::self, ent);
            if (raw) { PlayParseEntityKvs(raw, s.kv); rp::host->FreeString(rp::self, raw); }
            if (canBrushBounds)
            {
                rp::ForEachEntityBrush(ent, [&](ModToolsBrush b)
                {
                    float mn[3] = {}, mx[3] = {};
                    if (!rp::host->BrushBounds(rp::self, b, mn, mx)) return;
                    if (!s.hasBounds)
                    {
                        for (int i = 0; i < 3; ++i) { s.mins[i] = mn[i]; s.maxs[i] = mx[i]; }
                        s.hasBounds = true;
                    }
                    else
                    {
                        for (int i = 0; i < 3; ++i) { s.mins[i] = (std::min)(s.mins[i], mn[i]); s.maxs[i] = (std::max)(s.maxs[i], mx[i]); }
                    }
                });
            }
            out.push_back(std::move(s));
        });

        // Radiant temporarily unlinks selected brushes from their entity brush lists: fold the selection's bounds back into the entity so
        // selecting a zone cannot create a false failure.
        if (rp::host->GetSelection)
        {
            size_t count = rp::host->GetSelection(rp::self, nullptr, 0);
            if (count && count < 100000)
            {
                std::vector<ModToolsSelectedBrush> sel(count);
                size_t got = rp::host->GetSelection(rp::self, sel.data(), sel.size());
                if (got < sel.size()) sel.resize(got);
                for (const auto& b : sel)
                {
                    if (!b.entityGuid[0]) continue;
                    for (auto& s : out)
                    {
                        if (s.guid != b.entityGuid) continue;
                        if (!s.hasBounds)
                        {
                            for (int i = 0; i < 3; ++i) { s.mins[i] = b.mins[i]; s.maxs[i] = b.maxs[i]; }
                            s.hasBounds = true;
                        }
                        else
                        {
                            for (int i = 0; i < 3; ++i) { s.mins[i] = (std::min)(s.mins[i], b.mins[i]); s.maxs[i] = (std::max)(s.maxs[i], b.maxs[i]); }
                        }
                        break;
                    }
                }
            }
        }
        if (!canBrushBounds)
            PlayPreflightLine(report, "WARN", "This host cannot report brush bounds, so player-inside-zone validation is unavailable.");
        return true;
    }

    // Zones, spawners and spawn structs often live inside prefabs (zm_escape keeps them all in one), which the
    // entity walk does not see. Read each misc_prefab's .map (nested ones too) for the key/value checks only.
    void PlayAddMapTextEntities(const std::wstring& mapSource, const std::string& text, bool fromPrefab, int depth,
                                std::set<std::string>& seen, std::vector<PlayEntitySnapshot>& out, int& files);
    void PlayAddPrefabEntities(const std::wstring& mapSource, const std::string& model, int depth,
                               std::set<std::string>& seen, std::vector<PlayEntitySnapshot>& out, int& files)
    {
        std::string key = PlayLowerAscii(model);
        std::replace(key.begin(), key.end(), '\\', '/');
        if (key.empty() || depth > 8 || !seen.insert(key).second) return;
        std::string text = PlayReadTextFile(PlayJoin(mapSource, PlayUtf8ToWide(key)), 256u * 1024 * 1024);
        if (text.empty()) return;
        ++files;
        PlayAddMapTextEntities(mapSource, text, true, depth, seen, out, files);
    }

    // Top-level entities of a .map text, misc_prefab files followed (their entities only, not the misc_prefab itself).
    void PlayAddMapTextEntities(const std::wstring& mapSource, const std::string& text, bool fromPrefab, int depth,
                                std::set<std::string>& seen, std::vector<PlayEntitySnapshot>& out, int& files)
    {
        int braces = 0;
        std::string entityText;
        size_t p = 0;
        while (p < text.size())
        {
            size_t eol = text.find_first_of("\r\n", p);
            std::string line = PlayTrim(text.substr(p, eol == std::string::npos ? std::string::npos : eol - p));
            p = eol == std::string::npos ? text.size() : eol + 1;
            if (line == "{") { ++braces; continue; }
            if (line == "}")
            {
                if (--braces == 0 && !entityText.empty())
                {
                    PlayEntitySnapshot e;
                    e.fromPrefab = fromPrefab;
                    PlayParseEntityKvs(entityText, e.kv);
                    entityText.clear();
                    if (PlayLowerAscii(PlayKv(e, "classname")) == "misc_prefab")
                        PlayAddPrefabEntities(mapSource, PlayKv(e, "model"), depth + 1, seen, out, files);
                    else
                        out.push_back(std::move(e));
                }
                if (braces < 0) braces = 0;
                continue;
            }
            if (braces == 1 && !line.empty() && line[0] == '"') { entityText += line; entityText += '\n'; }
        }
    }

    int PlayPrefabDepth();
    PlayPreflightReport PlayPreflightMap(const PlayMapInfo& m)
    {
        PlayPreflightReport r;
        PlayPreflightLine(r, "OK", "Checking Zombies setup for " + PlayWideToUtf8(m.mapName) + ".");

        std::vector<PlayEntitySnapshot> entities;
        if (PlayPrefabDepth() > 0)
        {
            // Inside a prefab the entity list is the prefab's: check the saved top map, which is what gets compiled.
            const std::string text = PlayReadTextFile(m.mapPath, 256u * 1024 * 1024);
            if (text.empty())
            {
                PlayPreflightLine(r, "BLOCK", "Could not read " + PlayWideToUtf8(m.mapPath) + " to check it while a prefab is open.");
                return r;
            }
            std::set<std::string> seen;
            int files = 0;
            PlayAddMapTextEntities(PlayJoin(m.gameRoot, L"map_source"), text, false, -1, seen, entities, files);
            PlayPreflightLine(r, "OK", "A prefab is open: checked the saved " + PlayWideToUtf8(PlayBaseName(m.mapPath)) + " and " +
                              std::to_string(files) + " prefab file(s) (" + std::to_string(entities.size()) + " entities).");
        }
        else
        {
            if (!PlaySnapshotEntities(entities, r)) return r;
            const std::wstring mapSource = PlayJoin(m.gameRoot, L"map_source");
            std::set<std::string> seen;
            std::vector<PlayEntitySnapshot> prefabEntities;
            int files = 0;
            for (const auto& e : entities)
                if (PlayLowerAscii(PlayKv(e, "classname")) == "misc_prefab")
                    PlayAddPrefabEntities(mapSource, PlayKv(e, "model"), 0, seen, prefabEntities, files);
            if (files)
                PlayPreflightLine(r, "OK", "Read " + std::to_string(files) + " prefab file(s) for zones, spawners and spawn structs (" +
                                  std::to_string(prefabEntities.size()) + " entities).");
            for (auto& e : prefabEntities) entities.push_back(std::move(e));
        }

        std::vector<const PlayEntitySnapshot*> infoStarts;
        std::vector<const PlayEntitySnapshot*> initialSpawns;
        std::vector<const PlayEntitySnapshot*> playerVolumes;
        std::vector<const PlayEntitySnapshot*> zombieFactories;
        std::vector<const PlayEntitySnapshot*> zombieLocations;
        std::map<std::string, int> playerSlots;

        const std::set<std::string> locationTypes = { "riser_location", "spawn_location", "faller_location", "custom_spawner_entry" };
        for (const auto& e : entities)
        {
            std::string cls = PlayLowerAscii(PlayKv(e, "classname"));
            std::string targetname = PlayLowerAscii(PlayKv(e, "targetname"));
            std::string noteworthy = PlayLowerAscii(PlayKv(e, "script_noteworthy"));
            if (cls == "info_player_start" || cls == "info_player_deathmatch") infoStarts.push_back(&e);
            if (cls == "script_struct" && targetname == "initial_spawn_points")
            {
                initialSpawns.push_back(&e);
                if (noteworthy.rfind("player_", 0) == 0) ++playerSlots[noteworthy];
            }
            if (noteworthy == "player_volume") playerVolumes.push_back(&e);
            if (noteworthy == "zombie_spawner" || cls.find("spawner_zm_factory_zombie") != std::string::npos)
                zombieFactories.push_back(&e);
            if (locationTypes.count(noteworthy)) zombieLocations.push_back(&e);
        }

        if (infoStarts.empty())
            PlayPreflightLine(r, "BLOCK", "No info_player_start/info_player_deathmatch entity was found.");
        else
            PlayPreflightLine(r, "OK", "Found a Radiant/game start marker.");

        if (playerSlots["player_1"] == 0)
        {
            // Several valid or older BO3 Zombies maps use info_player_start or script/prefab-driven spawn setup instead of an authored
            // initial_spawn_points/player_1 struct: a warning, not a block, when a normal game start marker exists.
            if (!infoStarts.empty())
                PlayPreflightLine(r, "WARN", "No initial_spawn_points / player_1 script_struct was found. A Radiant/game start marker exists, so Play Test will allow this legacy/script-driven spawn layout.");
            else
                PlayPreflightLine(r, "BLOCK", "No usable player start path was found (no initial_spawn_points/player_1 and no info_player_start/info_player_deathmatch).");
        }
        else
            PlayPreflightLine(r, "OK", "Found initial player_1 spawn.");
        for (int i = 1; i <= 4; ++i)
        {
            std::string slot = "player_" + std::to_string(i);
            if (playerSlots[slot] > 1)
                PlayPreflightLine(r, "WARN", "Duplicate " + slot + " initial spawn points found (" + std::to_string(playerSlots[slot]) + ").");
        }
        if (playerSlots["player_2"] == 0 || playerSlots["player_3"] == 0 || playerSlots["player_4"] == 0)
            PlayPreflightLine(r, "WARN", "One or more co-op initial spawns (player_2..player_4) are missing; solo can still be tested.");

        if (playerVolumes.empty())
            PlayPreflightLine(r, "BLOCK", "No info_volume/script_noteworthy=player_volume zone exists. Players can die immediately outside a playable zone.");
        else
            PlayPreflightLine(r, "OK", "Found " + std::to_string(playerVolumes.size()) + " player-volume zone(s).");

        // Usermap-owned map scripts live under usermaps/<map>/scripts/zm (custom-map layout), share/raw/scripts/zm is still accepted as a fallback.
        const std::wstring userScriptsRoot = PlayJoin(PlayJoin(m.usermapRoot, L"scripts"), L"zm");
        const std::wstring sharedScriptsRoot = PlayJoin(PlayJoin(PlayJoin(PlayJoin(m.gameRoot, L"share"), L"raw"), L"scripts"), L"zm");
        const std::wstring userGscPath = PlayJoin(userScriptsRoot, m.mapName + L".gsc");
        const std::wstring userCscPath = PlayJoin(userScriptsRoot, m.mapName + L".csc");
        const std::wstring sharedGscPath = PlayJoin(sharedScriptsRoot, m.mapName + L".gsc");
        const std::wstring sharedCscPath = PlayJoin(sharedScriptsRoot, m.mapName + L".csc");
        const std::wstring gscPath = PlayFileExists(userGscPath) ? userGscPath : sharedGscPath;
        const std::wstring cscPath = PlayFileExists(userCscPath) ? userCscPath : sharedCscPath;
        const std::wstring zonePath = PlayJoin(PlayJoin(m.usermapRoot, L"zone_source"), m.mapName + L".zone");
        std::string gsc = PlayReadTextFile(gscPath);
        std::string csc = PlayReadTextFile(cscPath);
        std::string gscLow = PlayLowerAscii(gsc), cscLow = PlayLowerAscii(csc);
        std::string defaultStart = PlayLowerAscii(PlayExtractDefaultStartLocation(gsc));

        if (!PlayFileExists(gscPath))
            PlayPreflightLine(r, "WARN", "Map GSC source was not found in usermaps/<map>/scripts/zm or share/raw/scripts/zm. Existing compiled script assets may still be runnable, but source-level zone checks are unavailable.");
        else if (gscLow.find("zm_usermap::main") == std::string::npos)
            PlayPreflightLine(r, "WARN", "Map GSC does not call zm_usermap::main(); this can be valid for a custom/legacy startup path, but verify Zombies initialization is intentional.");
        else
        {
            PlayPreflightLine(r, "OK", "Map GSC found: " + PlayWideToUtf8(gscPath));
            PlayPreflightLine(r, "OK", "Map GSC has zm_usermap::main().");
        }
        if (PlayFileExists(gscPath) && gscLow.find("manage_zones") == std::string::npos)
            PlayPreflightLine(r, "WARN", "Map GSC does not call zm_zonemgr::manage_zones(...). This may be intentional for a custom zone system; entity wiring will still be checked.");
        else if (PlayFileExists(gscPath))
            PlayPreflightLine(r, "OK", "Map GSC starts the Zombies zone manager.");

        if (!PlayFileExists(cscPath))
            PlayPreflightLine(r, "WARN", "Map CSC source was not found in usermaps/<map>/scripts/zm or share/raw/scripts/zm. Existing compiled client script assets may still be runnable.");
        else if (cscLow.find("zm_usermap::main") == std::string::npos)
            PlayPreflightLine(r, "WARN", "Map CSC does not appear to call zm_usermap::main(); client-side usermap setup may use a custom/legacy path.");
        else
        {
            PlayPreflightLine(r, "OK", "Map CSC found: " + PlayWideToUtf8(cscPath));
            PlayPreflightLine(r, "OK", "Map CSC has zm_usermap::main().");
        }

        if (!PlayFileExists(zonePath))
            PlayPreflightLine(r, "BLOCK", "Missing zone manifest: usermaps/" + PlayWideToUtf8(m.mapName) + "/zone_source/" + PlayWideToUtf8(m.mapName) + ".zone");
        else PlayPreflightLine(r, "OK", "Zone manifest exists.");

        const PlayEntitySnapshot* startVolume = nullptr;
        if (!playerVolumes.empty())
        {
            // Prefer the volume named by level.default_start_location, then the volume containing player_1,
            // then an obvious start-named volume, finally the only/first volume.
            if (!defaultStart.empty())
            {
                for (auto* v : playerVolumes)
                {
                    std::string tn = PlayLowerAscii(PlayKv(*v, "targetname"));
                    if (tn == defaultStart || tn == defaultStart + "_zone" || (tn.size() > 5 && tn.substr(tn.size()-5) == "_zone" && tn.substr(0, tn.size()-5) == defaultStart))
                    { startVolume = v; break; }
                }
            }
            float p1[3] = {};
            bool haveP1 = false;
            for (auto* p : initialSpawns)
            {
                if (PlayLowerAscii(PlayKv(*p, "script_noteworthy")) == "player_1" && PlayParseVec3(PlayKv(*p, "origin"), p1))
                { haveP1 = true; break; }
            }
            if (!startVolume && haveP1)
                for (auto* v : playerVolumes) if (PlayPointInside(p1, *v)) { startVolume = v; break; }
            if (!startVolume)
                for (auto* v : playerVolumes) if (PlayLowerAscii(PlayKv(*v, "targetname")).find("start") != std::string::npos) { startVolume = v; break; }
            if (!startVolume) startVolume = playerVolumes.front();

            if (startVolume)
            {
                std::string zoneName = PlayKv(*startVolume, "targetname");
                std::string spawnTarget = PlayKv(*startVolume, "target");
                if (zoneName.empty()) PlayPreflightLine(r, "WARN", "Starting player_volume has no targetname; zone scripting cannot identify it cleanly.");
                else
                {
                    PlayPreflightLine(r, "OK", "Starting zone candidate: " + zoneName + ".");
                    if (!gscLow.empty() && gscLow.find(PlayLowerAscii(zoneName)) == std::string::npos)
                        PlayPreflightLine(r, "WARN", "Starting zone '" + zoneName + "' was not found by name in the map GSC; verify it is registered in init_zones/default_start_location.");
                }
                if (spawnTarget.empty())
                    PlayPreflightLine(r, "BLOCK", "Starting player_volume has no target linking it to its zombie spawn-location set.");
                else
                {
                    int matches = 0;
                    int weakLocations = 0;
                    for (auto* loc : zombieLocations)
                    {
                        if (PlayLowerAscii(PlayKv(*loc, "targetname")) != PlayLowerAscii(spawnTarget)) continue;
                        ++matches;
                        std::string ss = PlayLowerAscii(PlayKv(*loc, "script_string"));
                        std::string target = PlayKv(*loc, "target");
                        if (ss.empty() && target.empty()) ++weakLocations;
                    }
                    if (!matches)
                        PlayPreflightLine(r, "BLOCK", "Starting zone targets '" + spawnTarget + "' but no zombie_location script_struct uses that targetname.");
                    else
                    {
                        PlayPreflightLine(r, "OK", "Starting zone has " + std::to_string(matches) + " linked zombie spawn location(s).");
                        if (weakLocations)
                            PlayPreflightLine(r, "WARN", std::to_string(weakLocations) + " linked zombie location(s) have neither script_string (usually find_flesh) nor a target/barricade link.");
                    }
                }

                bool p1InPrefab = false;
                for (auto* p : initialSpawns)
                    if (p->fromPrefab && PlayLowerAscii(PlayKv(*p, "script_noteworthy")) == "player_1") p1InPrefab = true;
                if (startVolume->hasBounds && !p1InPrefab)
                {
                    bool p1Inside = false;
                    for (auto* p : initialSpawns)
                    {
                        if (PlayLowerAscii(PlayKv(*p, "script_noteworthy")) != "player_1") continue;
                        float pos[3] = {};
                        if (PlayParseVec3(PlayKv(*p, "origin"), pos) && PlayPointInside(pos, *startVolume)) { p1Inside = true; break; }
                    }
                    if (playerSlots["player_1"] && !p1Inside)
                        PlayPreflightLine(r, "BLOCK", "player_1 is outside the detected starting player_volume. BO3 can immediately kill/fail players outside active playable space.");
                    else if (p1Inside)
                        PlayPreflightLine(r, "OK", "player_1 is inside the starting player_volume.");
                }
            }
        }

        if (zombieFactories.empty())
            PlayPreflightLine(r, "BLOCK", "No Zombies factory spawner was found (script_noteworthy=zombie_spawner / *_spawner_zm_factory_zombie).");
        else
        {
            int usable = 0;
            for (auto* z : zombieFactories)
            {
                std::string count = PlayTrim(PlayKv(*z, "count"));
                std::string force = PlayTrim(PlayKv(*z, "script_forcespawn"));
                int n = count.empty() ? 1 : atoi(count.c_str());
                if (n > 0) ++usable;
                else PlayPreflightLine(r, "BLOCK", "A zombie factory has count=0 and is inactive.");
                if (!force.empty() && force != "1")
                    PlayPreflightLine(r, "WARN", "A zombie factory has script_forcespawn=" + force + " instead of 1.");
                else if (force.empty())
                    PlayPreflightLine(r, "WARN", "A zombie factory is missing script_forcespawn=1.");
            }
            if (!usable) PlayPreflightLine(r, "BLOCK", "Zombie factory entities exist, but none has a usable positive count.");
            else PlayPreflightLine(r, "OK", "Found " + std::to_string(usable) + " usable zombie factory spawner(s).");

            bool factoryInPrefab = false;
            for (auto* z : zombieFactories) if (z->fromPrefab) factoryInPrefab = true;
            if (startVolume && startVolume->hasBounds && !factoryInPrefab)
            {
                int factoriesInside = 0;
                for (auto* z : zombieFactories)
                {
                    float pos[3] = {};
                    if (PlayParseVec3(PlayKv(*z, "origin"), pos) && PlayPointInside(pos, *startVolume)) ++factoriesInside;
                }
                if (!factoriesInside)
                    PlayPreflightLine(r, "WARN", "No zombie factory origin is inside the detected starting player_volume. BO3 templates commonly keep the active factory in playable zone space; verify this is intentional.");
                else
                    PlayPreflightLine(r, "OK", "At least one zombie factory is inside the starting player_volume.");
            }
        }

        if (zombieLocations.empty())
            PlayPreflightLine(r, "BLOCK", "No zombie spawn-location script_structs were found (riser_location/spawn_location/faller_location/custom_spawner_entry).");

        if (!defaultStart.empty())
            PlayPreflightLine(r, "OK", "GSC starting zone is '" + defaultStart + "'.");
        else
            PlayPreflightLine(r, "WARN", "Could not find level.default_start_location or init_zones[0] in the map GSC; starting zone was inferred from map entities.");

        return r;
    }

    std::string PlayPreflightSummary(const PlayPreflightReport& r)
    {
        std::ostringstream o;
        if (r.blockers == 0) o << "READY"; else o << "NOT READY";
        o << " - " << r.blockers << " blocker" << (r.blockers == 1 ? "" : "s")
          << ", " << r.warnings << " warning" << (r.warnings == 1 ? "" : "s");
        return o.str();
    }

    // listWarnings: every warning goes to the simple log (an explicit check), a Play lists a warning once per Radiant session, then counts it.
    std::set<std::string> g_playPreflightWarningsShown;
    void PlayPublishPreflight(const PlayPreflightReport& r, bool listWarnings = false)
    {
        g_playPreflightSummary = PlayPreflightSummary(r);
        PlayAppendLog("=== Map Setup Check ===");
        size_t hidden = 0;
        for (const auto& line : r.lines)
        {
            PlayAppendLog(line);
            // the problems are for the simple log too, the [INFO] lines are not
            if (line.rfind("[BLOCK] ", 0) == 0) PlayNote("Map setup problem: " + line.substr(8));
            else if (line.rfind("[WARN] ", 0) == 0)
            {
                if (g_playPreflightWarningsShown.insert(g_playCurrentMap + "|" + line).second || listWarnings) PlayNote("Map setup warning: " + line.substr(7));
                else ++hidden;
            }
        }
        PlayAppendLog("[CHECK] " + g_playPreflightSummary);
        if (r.blockers == 0 && !hidden) PlayNote(r.warnings ? "Map setup check: nothing blocking." : "Map setup check passed.");
        else if (r.blockers == 0)
            PlayNote("Map setup check: nothing blocking, " + std::to_string(hidden) + " warning(s) shown before (Verbose log lists them).");
    }

    void PlayAnglesToAxis(const float angles[3], float ax[3][3]);
    // Main thread: Radiant's camera into g_playCamDvar (empty when not wanted or the 3D view is not up), generation bumped.
    void PlayCaptureCamera(bool want)
    {
        g_playCamDvar.clear();
        ++g_playCamGen;
        if (!want || !rp::host || !rp::host->GetCamera) return;
        float o[3] = {}, a[3] = {};
        if (!rp::host->GetCamera(rp::self, o, a)) return;
        // Inside a prefab the camera is in the prefab's space: carry it out through each entered placement.
        const int depth = PlayPrefabDepth();
        if (depth > 0)
        {
            if (!rp::Has(&rp::host->PrefabStackPlacement) || !rp::host->PrefabStackPlacement) return;
            const float d2r = 3.14159265358979f / 180.0f;
            float f[3] = { std::cos(a[0] * d2r) * std::cos(a[1] * d2r), std::cos(a[0] * d2r) * std::sin(a[1] * d2r), std::sin(a[0] * d2r) };
            for (int level = depth; level >= 1; --level)
            {
                float po[3] = {}, pa[3] = {}, ax[3][3];
                if (!rp::host->PrefabStackPlacement(rp::self, level, po, pa)) return;
                PlayAnglesToAxis(pa, ax);
                float no[3], nf[3];
                for (int i = 0; i < 3; ++i)
                {
                    no[i] = po[i] + o[0] * ax[0][i] + o[1] * ax[1][i] + o[2] * ax[2][i];
                    nf[i] = f[0] * ax[0][i] + f[1] * ax[1][i] + f[2] * ax[2][i];
                }
                for (int i = 0; i < 3; ++i) { o[i] = no[i]; f[i] = nf[i]; }
            }
            a[0] = std::asin((std::max)(-1.0f, (std::min)(1.0f, f[2]))) / d2r;
            a[1] = std::atan2(f[1], f[0]) / d2r;
        }
        char b[160];
        snprintf(b, sizeof b, "%.1f %.1f %.1f %.1f %.1f", o[0], o[1], o[2], -a[0], a[1]);
        g_playCamDvar = b;
    }

    void PlayJoinEnter();   // below the settings they override
    void PlayJoinLeave();
    bool PlayJoinBeginFetch(const PlayMapInfo& m);
    std::string PlayTeamCreate(const char* cmd);
    // BO3's LAN play needs a socket the PC game never opens: the bridge opens it while BO3LiveLan.ini says so (polled once a second),
    // on for any Team Create session. It takes packets only from the session's addresses (bridge/join.cpp PeerAllowed): the host's
    // clients or a client's host (TeamCreate peeraddrs) plus the Join Host address, rewritten on change so a peer who joins mid-game is let in.
    void PlayLanPeersTick(bool now)
    {
        static ULONGLONG last = 0;
        static std::string written = "\x01";
        const ULONGLONG t = GetTickCount64();
        if (!now && t - last < 1000) return;
        last = t;
        if (!g_playLanWanted) return;
        std::string peers = PlayTeamCreate("peeraddrs");
        if (!g_playJoinHost.empty() && ("," + peers + ",").find("," + g_playJoinHost + ",") == std::string::npos)
            peers += (peers.empty() ? "" : ",") + g_playJoinHost;
        if (peers == written) return;
        const std::wstring ini = PlayJoinFile(L"BO3LiveLan.ini");
        if (ini.empty()) return;
        WritePrivateProfileStringW(L"lan", L"peers", PlayUtf8ToWide(peers).c_str(), ini.c_str());
        written = peers;
    }
    void PlayWriteLanFlag(bool join)
    {
        g_playLanWanted = join || !PlayTeamCreate("hostaddr").empty();
        PlayLanPeersTick(true);   // the list first: the bridge opens the socket as soon as enabled=1
        const std::wstring ini = PlayJoinFile(L"BO3LiveLan.ini");
        if (!ini.empty()) WritePrivateProfileStringW(L"lan", L"enabled", g_playLanWanted ? L"1" : L"0", ini.c_str());
        if (g_playLanWanted) PlayAppendLog("[LAN] Team Create session: the bridge opens BO3's LAN socket in the game (UDP 27017 to 27026) for the session's peers only, so they can find and join it.");
    }
    // A closed BO3 Live pane opens as a tab over the Camera pane, one already open stays where the user put it (raised when behind
    // another tab). Main thread. 1 = moved, 2 = raised, 0 = shown the plain way (no Camera pane, or an older host), -1 = already on screen.
    int PlayOpenGamePane()
    {
        if (!g_playGamePane || !rp::host || !rp::host->PaneVisible) return -1;
        if (rp::host->PaneVisible(rp::self, g_playGamePane)) return -1;
        const int placed = rp::Has(&rp::host->TabPaneWith) && rp::host->TabPaneWith ? rp::host->TabPaneWith(rp::self, g_playGamePane, "Camera") : 0;
        if (placed == 1) PlayAppendLog("[EMBED] BO3 Live pane opened over the Camera pane.");
        else if (placed == 0) rp::host->ShowPane(rp::self, g_playGamePane, 1);
        return placed;
    }

    // ---- Windows Defender ----
    // Defender's ML heuristic takes BO3LiveBridge.dll (false positive), without it the game never docks: Play with the DLL missing offers the repair
    // (defender_playtest.ps1 repair), the first Play with no recorded exclusion offers it once. PlayTest.ini [defender]: exclusion=1, asked=1.
    bool PlayStart(bool full, bool join);
    std::atomic<int> g_playDefenderActive{ -1 };   // -1 not known yet, 0 Defender is the active antivirus, else not
    LONG g_playDefenderBusy = 0;

    HWND PlayMainHwnd() { return rp::Has(&rp::host->MainWindow) ? static_cast<HWND>(rp::host->MainWindow(rp::self)) : nullptr; }

    int PlayRunHidden(const std::wstring& args, DWORD timeoutMs)
    {
        std::wstring line = L"powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden " + args;
        std::vector<wchar_t> buffer(line.begin(), line.end()); buffer.push_back(0);
        STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
        CloseHandle(pi.hThread);
        DWORD code = static_cast<DWORD>(-1);
        if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
        else TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        return static_cast<int>(code);
    }

    // defender_playtest.ps1 <mode> "<this folder>" with administrator rights (one UAC prompt): its exit code, 1223 when
    // the prompt was declined, -2 when the script is missing
    int PlayRunDefenderScript(const wchar_t* mode)
    {
        const std::wstring dir = PlayThisModuleDir();
        const std::wstring script = PlayJoin(dir, L"defender_playtest.ps1");
        if (dir.empty() || !PlayFileExists(script)) return -2;
        std::wstring params = L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + script + L"\" " + mode + L" \"" + dir + L"\"";
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        SHELLEXECUTEINFOW info = { sizeof info };
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        info.hwnd = PlayMainHwnd();
        info.lpVerb = L"runas";
        info.lpFile = L"powershell.exe";
        info.lpParameters = params.c_str();
        info.nShow = SW_HIDE;
        int result = -1;
        if (ShellExecuteExW(&info))
        {
            DWORD code = static_cast<DWORD>(-1);
            if (info.hProcess)
            {
                if (WaitForSingleObject(info.hProcess, 180000) == WAIT_OBJECT_0) GetExitCodeProcess(info.hProcess, &code);
                else TerminateProcess(info.hProcess, 1);
                CloseHandle(info.hProcess);
            }
            result = static_cast<int>(code);
        }
        else result = static_cast<int>(GetLastError());
        if (SUCCEEDED(com)) CoUninitialize();
        return result;
    }

    // a detached thread that pins this DLL, so an unload while PowerShell runs never unmaps its code
    void PlayDetached(void (*fn)(void*), void* arg)
    {
        struct Job { void (*fn)(void*); void* arg; HMODULE self; };
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&PlayDetached), &self);
        Job* job = new Job{ fn, arg, self };
        HANDLE h = CreateThread(nullptr, 0, [](void* p) -> DWORD {
            Job j = *static_cast<Job*>(p); delete static_cast<Job*>(p);
            try { j.fn(j.arg); } catch (...) {}
            if (j.self) FreeLibraryAndExitThread(j.self, 0);
            return 0;
        }, job, 0, nullptr);
        if (h) CloseHandle(h);
        else { delete job; if (self) FreeLibrary(self); }
    }

    // OnLoad: whether Defender is the active antivirus (no admin needed, about a second of PowerShell)
    void PlayCheckDefenderLater()
    {
        if (rp::ConfigGetInt("defender", "exclusion", 0) || rp::ConfigGetInt("defender", "asked", 0)) return;
        PlayDetached([](void*) {
            g_playDefenderActive = PlayRunHidden(L"-Command \"$s = Get-MpComputerStatus; if ($s.RealTimeProtectionEnabled -and "
                                                 L"$s.AMRunningMode -eq 'Normal') { exit 0 } else { exit 3 }\"", 30000);
        }, nullptr);
    }

    struct PlayRepairJob { bool repair; bool full; bool join; };

    void PlayDefenderResult(const PlayRepairJob& job, int code)
    {
        InterlockedExchange(&g_playDefenderBusy, 0);
        const bool present = PlayFileExists(PlayJoin(PlayThisModuleDir(), L"BO3LiveBridge.dll"));
        PlayAppendLog("[DEFENDER] " + std::string(job.repair ? "repair" : "exclusion") + ": result " + std::to_string(code) + (present ? "" : ", BO3LiveBridge.dll still missing"));
        if (code == 0 || code == 5) rp::ConfigSet("defender", "exclusion", "1");
        if (code == 1223) return;                        // the UAC prompt was declined: nothing changed, nothing to say
        const char* text = nullptr;
        if (code == 0 && job.repair && present) { PlayAppendLog("[DEFENDER] BO3LiveBridge.dll restored; playing."); PlayStart(job.full, job.join); PlayNote("BO3LiveBridge.dll was restored from Windows Defender's quarantine."); return; }
        if (code == 0) return;                           // the offer: done quietly, the log says so
        if (code == 5 || (code == 3 && job.repair))
            text = "BO3LiveBridge.dll could not be restored: it is no longer in Windows Defender's quarantine (or another antivirus removed it).\n\n"
                   "Run the MTPlugins installer again (Repair keeps your settings), or restore the file in your antivirus and allow it.";
        else if (code == 3) text = "Windows Defender is not the active antivirus on this PC, so there is nothing to exclude. If another antivirus removes "
                                   "BO3LiveBridge.dll, add the Play Test folder to its exceptions.";
        else if (code == -2) text = "defender_playtest.ps1 is missing from the Play Test folder. Run the MTPlugins installer again.";
        else text = "Windows Defender did not accept the change (its settings may be managed by an organization).\n\n"
                    "By hand: Windows Security > Virus & threat protection > Protection history: restore BO3LiveBridge.dll; then "
                    "Manage settings > Exclusions > Add an exclusion > Folder: the Play Test folder (bin\\plugins\\PlayTest).";
        MessageBoxA(PlayMainHwnd(), text, "Play Test", MB_OK | MB_ICONWARNING);
    }

    void PlayStartDefenderScript(bool repair, bool full, bool join)
    {
        if (InterlockedCompareExchange(&g_playDefenderBusy, 1, 0) != 0) return;
        PlayDetached([](void* p) {
            PlayRepairJob job = *static_cast<PlayRepairJob*>(p); delete static_cast<PlayRepairJob*>(p);
            int code = -1;
            try { code = PlayRunDefenderScript(job.repair ? L"repair" : L"add"); } catch (...) { code = -1; }
            rp::PostToMainThread([job, code] { PlayDefenderResult(job, code); });
        }, new PlayRepairJob{ repair, full, join });
    }

    // Play's first step: false = do not play now (the repair runs and plays on its own once the DLL is back, or cancelled)
    bool PlayCheckBridgeFile(bool full, bool join)
    {
        const std::wstring dll = PlayJoin(PlayThisModuleDir(), L"BO3LiveBridge.dll");
        if (!PlayFileExists(dll))
        {
            if (g_playDefenderBusy) return false;
            PlayAppendLog("[DEFENDER] BO3LiveBridge.dll is missing from the Play Test folder (Windows Defender usually takes it).");
            int r = MessageBoxA(PlayMainHwnd(),
                "BO3LiveBridge.dll is missing from the Play Test folder. Without it the game does not dock and nothing is live.\n\n"
                "Windows Defender most likely removed it: it reports the file as Trojan:Win32/Wacatac, a false alarm on the DLL Play Test "
                "loads into BO3.\n\n"
                "Yes: repair it. Windows asks for administrator permission once; the Play Test folder is excluded from Defender's scans "
                "and the file is restored from quarantine, then the play test starts.\n"
                "No: play without it (undocked, no live edits).\n"
                "Cancel: do nothing.", "Play Test", MB_YESNOCANCEL | MB_ICONWARNING);
            if (r == IDYES) PlayStartDefenderScript(true, full, join);
            return r == IDNO;
        }
        // the one-time offer, on a PC where Defender is active and no exclusion is recorded (per-user install, dev build)
        if (g_playDefenderActive == 0 && !g_playDefenderBusy && !rp::ConfigGetInt("defender", "exclusion", 0) && !rp::ConfigGetInt("defender", "asked", 0))
        {
            rp::ConfigSet("defender", "asked", "1");
            int r = MessageBoxA(PlayMainHwnd(),
                "Windows Defender sometimes removes BO3LiveBridge.dll, the DLL Play Test loads into BO3 (a false alarm), and the game then "
                "stops docking.\n\nExclude the Play Test folder from Defender's scans? Windows asks for administrator permission once. "
                "Nothing else is excluded.", "Play Test", MB_YESNO | MB_ICONQUESTION);
            if (r == IDYES) PlayStartDefenderScript(false, full, join);
        }
        return true;
    }

    bool PlayStart(bool full, bool join = false)
    {
        if (!g_playEnabled) { PlayAppendLog("Play Test is disabled in [playtest] enabled=0."); PlayNote("Play Test is turned off (enabled=0 in PlayTest.ini)."); return false; }
        if (!PlayCheckBridgeFile(full, join)) return false;
        if (!join) PlayJoinLeave();   // a plain Play hosts again: the settings the join switched off come back first
        if (rp::host && rp::host->ui && rp::host->ui->IsChecked)
        {
            if (g_playWarmCheck) g_playKeepWarm = rp::host->ui->IsChecked(g_playWarmCheck) != 0;
            if (g_playPrewarmCheck) g_playPrewarmOnMapOpen = rp::host->ui->IsChecked(g_playPrewarmCheck) != 0;
            if (g_playEmbedCheck) g_playEmbedGame = rp::host->ui->IsChecked(g_playEmbedCheck) != 0;
            if (g_playNavCheck) g_playRebuildNav = rp::host->ui->IsChecked(g_playNavCheck) != 0;
            if (g_playAutoRestartCheck) g_playAutoRestart = rp::host->ui->IsChecked(g_playAutoRestartCheck) != 0;
            if (g_playInstantStartCheck) g_playInstantStart = rp::host->ui->IsChecked(g_playInstantStartCheck) != 0;
            if (g_playFromCameraCheck) g_playFromCamera = rp::host->ui->IsChecked(g_playFromCameraCheck) != 0;
            if (g_playFlyCheck) g_playFly = rp::host->ui->IsChecked(g_playFlyCheck) != 0;
            if (g_playGodCheck) g_playGod = rp::host->ui->IsChecked(g_playGodCheck) != 0;
            if (g_playLiveCheck && PlayLiveEditBuild()) g_playLivePreview = rp::host->ui->IsChecked(g_playLiveCheck) != 0;   // greyed without Enhanced: shown unchecked, preference kept
            rp::ConfigSet("playtest", "keep_warm", g_playKeepWarm ? "1" : "0");
            rp::ConfigSet("playtest", "prewarm_on_map_open", g_playPrewarmOnMapOpen ? "1" : "0");
            rp::ConfigSet("playtest", "embed_game", g_playEmbedGame ? "1" : "0");
            rp::ConfigSet("playtest", "smart_rebuild_nav", g_playRebuildNav ? "1" : "0");
            rp::ConfigSet("playtest", "auto_restart_gameover", g_playAutoRestart ? "1" : "0");
            rp::ConfigSet("playtest", "instant_start", g_playInstantStart ? "1" : "0");
            if (g_playFromCameraCheck) rp::ConfigSet("playtest", "play_from_camera", g_playFromCamera ? "1" : "0");
            rp::ConfigSet("playtest", "play_fly", g_playFly ? "1" : "0");
            rp::ConfigSet("playtest", "live_preview", g_playLivePreview ? "1" : "0");
        }
        if (join) PlayJoinEnter();
        // Play From Here: the camera is read here, on the main thread, before anything else runs. Radiant's pitch is
        // positive upwards (CCamWnd+220), the game's is positive downwards.
        const bool fromStart = g_playFromStartOnce;
        g_playFromStartOnce = false;
        PlayCaptureCamera(g_playFromCamera && !fromStart);
        g_playKeepPos = g_playKeepPosNext; g_playKeepPosNext = false;
        { std::lock_guard<std::mutex> lock(g_playMutex); g_playLevelProbeKeySet = false; }   // the level loads again: its probe layout is read at the next live bake
        // An explicit Play always overrides a prior manual Close suppression for the current map.
        g_playPrewarmSuppressedMap.clear();
        if (!g_playEmbedGame) PlayDetachEmbeddedGame();
        if (g_playThread.joinable())
        {
            if (!g_playThreadDone) PlayStop(false);
            g_playThread.join();
        }
        if (PlayBusy()) PlayStop(false);
        g_playCancel = false; g_playThreadDone = false;
        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            g_playLastError.clear();
            if (!g_playForceReloadOnce) PlayClearLogLocked();   // a live bake's own reload keeps the story that led to it
        }
        g_playLastUiLogSize = (size_t)-1;
        // background builds would run beside this one: the nav result is stale once the level reloads, the patch zone takes the materials
        if (g_playNavRunning) { g_playNavCancel = true; PlayAppendLog("[NAV] refresh in flight cancelled: Play builds the map itself."); }
        if (g_playNavPhase == 5) { g_playNavPhase = 0; g_playNavForPlay = false; { std::lock_guard<std::mutex> lock(g_playNavMutex); g_playNavKeepStem.clear(); } PlayWaitEnd("nav"); PlayAppendLog("[NAV] the nav zone of the last build is dropped: this Play builds again."); }
        if (g_playLiveMatRunning) { g_playLiveMatCancel = true; PlayAppendLog("[TEXTURED] live zone link in flight cancelled: Play's patch zone takes the names."); }
        if (g_playEmbedGame) PlayOpenGamePane();
        g_playState = PLAY_SAVING;
        if (!join && PlayMapUnnamed())
        {
            PlaySetError("The map is unnamed. Save it under its own name first (File > Save As), then press Play.");
            g_playThreadDone = true;
            return false;
        }
        if (g_playAutoSave && !join)   // a join builds nothing here: it runs the host's build
        {
            std::string action;
            if (PlaySaveCurrentMap(action)) PlayAppendLog("[SAVE] Radiant action: " + action);
            else
            {
                PlayAppendLog("[SAVE] Warning: couldn't invoke Radiant File/Save; live entity checks still use the open map, but build files may be stale.");
                PlayNote("WARNING: the map could not be saved, so the build uses the last saved version of it.");
            }
        }
        PlayMapInfo m; std::string error;
        if (!PlayResolveCurrentMap(m, error)) { PlaySetError(error); g_playThreadDone = true; return false; }
        g_playCurrentMap = PlayWideToUtf8(m.mapName);
        PlayNote((join ? "Join Host: " : full ? "Play (full build): " : "Play: ") + g_playCurrentMap);
        PlayAppendLog(std::string("=== Play Test: ") + (full ? "FORCE FULL" : "SMART") + (join ? ", JOIN HOST " + g_playJoinHost : std::string()) + " ===");
        PlayWriteLanFlag(join);
        if (join)
        {
            // no build here: a peer usually has only Team Create's temporary copy of the .map, and the game must load exactly the host's
            // fastfiles anyway (map, GSC/CSC, weapons, sounds, every compiled asset)
            PlayAppendLog("[JOIN] Gets the host's build of " + PlayWideToUtf8(m.mapName) + " through Team Create (usermaps\\" + PlayWideToUtf8(m.mapName) + "\\zone: map, scripts, weapons, sounds, images; only files that changed since the last join), then BO3 starts at its menu and joins the host's game. Off while joined: nav mesh cuts and refresh, live models, Disable AI, God, Fly, spawn at camera (the host's game runs those).");
            return PlayJoinBeginFetch(m);
        }
        PlayAppendLog("Map: " + PlayWideToUtf8(m.mapPath));
        PlayExportBuiltLighting(m);

        std::string restartDetail;
        if (PlayConfigureAutoRestart(m, g_playAutoRestart, restartDetail))
            PlayAppendLog(std::string("[TEST MODE] Auto-restart on game over ") + (g_playAutoRestart ? "ON: " : "OFF: ") + restartDetail);
        else if (g_playAutoRestart)
            PlayAppendLog("[TEST MODE WARN] Auto-restart could not be installed: " + restartDetail + ". The map will still test normally.");

        std::string instantDetail;
        if (PlayConfigureInstantStart(m, g_playInstantStart, instantDetail))
            PlayAppendLog(std::string("[TEST MODE] Skip startup black/fade ") + (g_playInstantStart ? "ON: " : "OFF: ") + instantDetail);
        else if (g_playInstantStart)
            PlayAppendLog("[TEST MODE WARN] Instant start could not be installed: " + instantDetail + ". The map will still test normally.");

        std::string hereDetail;
        if (PlayConfigurePlayHere(m, true, hereDetail))
        {
            if (g_playFromCamera)
                PlayAppendLog("[PLAY HERE] " + hereDetail + (g_playCamDvar.empty() ? (fromStart ? "; this Play starts at the map's spawn" : "; the 3D view is not up, stock spawn")
                                                                                    : "; spawn at " + g_playCamDvar + " (" + PlayHereMode() + ")"));
        }
        else
            PlayAppendLog("[PLAY HERE WARN] spawn-at-camera / fly receiver could not be installed: " + hereDetail + ". The map spawns at its own start and the Fly box does nothing.");

        std::string aiDetail;
        if (PlayConfigureDisableAi(m, false, aiDetail) && aiDetail.find("removed") != std::string::npos)
            PlayAppendLog("[TEST MODE] Removed the retired Disable AI GSC receiver; the bridge applies ai_disableSpawn now.");
        if (g_playDisableAi) PlayAppendLog("[TEST MODE] Disable AI ON: ai_disableSpawn 1 is re-applied every second by the bridge's zone module.");

        // The native bridge replaces the old GSC/dvar debug-line receiver: remove any old marked block once, then live editing no longer
        // touches or relinks the map GSC.
        std::string liveDetail;
        // The script receiver (developer line() calls fed by br_live_* dvars) comes back under live mode: the native bridge path is bound
        // to a 2024 build and inert here, and the DLL mailbox reaches the console without hooks.
        if (!PlayConfigureLivePreview(m, g_playLiveZone, liveDetail))
            PlayAppendLog("[LIVE WARN] Could not clean the retired v1.7 GSC live-preview block: " + liveDetail + ". Native bridge can still run.");
        else if (liveDetail.find("removed") != std::string::npos)
            PlayAppendLog("[LIVE] Removed retired GSC live-preview receiver; v1.8 uses BO3LiveBridge.dll instead.");

        // Entity/zone/spawner inspection must run on Radiant's main thread. Block obviously broken Zombies maps before compiling and linking.
        PlayPreflightReport check = PlayPreflightMap(m);
        PlayPublishPreflight(check);
        if (check.blockers > 0)
        {
            PlaySetError("Map setup check found " + std::to_string(check.blockers) + " blocking issue(s). Fix the items marked [BLOCK] before Play Test.");
            g_playThreadDone = true;
            if (g_playPane && rp::host && rp::host->ShowPane) rp::host->ShowPane(rp::self, g_playPane, 1);
            return false;
        }

        g_playThread = std::thread([m, full](){ PlayWorker(m, full); });
        return true;
    }

    // ---- LIVE BAKE: a finished in-editor light bake reaches the running game without a level reload ----
    // The baked lighting streams from <map>.xpak as named blobs and every link appends new ones under new keys: export the bake, link the base zone, read which blobs
    // the link added and hand them to the bridge (bridge/livebake.cpp). Works while blob sizes are unchanged, else the next Play reloads.
    bool g_playLiveBake = true;                  // [playtest] live_bake
    bool g_playBakeWasBaking = false;
    // Level reload for a bake the swap cannot take: the probes' count, places and volumes are compiled into the level (GfxReflectionProbe
    // configs, blends, Umbra ids, the probe image's layer count), so the swap keeps such a group until the next Play. The worker asks for it,
    // the main tick has the game note the player's place (br_play_keep, into br_play_cam) and starts a Smart Play with a forced warm reload. [playtest] live_bake_reload=0 turns it off.
    bool g_playLiveBakeReload = true;
    std::atomic<int> g_playBakeReloadWant{0};    // 1: asked by the worker, 2: keep request sent, Play at g_playBakeReloadDueMs
    uint64_t g_playBakeReloadDueMs = 0;
    std::string g_playBakeReloadWhy;             // under g_playMutex
    uint64_t g_playBakeLastCheck = 0;
    LONG g_playBakeSeq = 0;
    bool PlayBusy();

    struct PlayXpakLighting { uint64_t data[3] = {}, index[3] = {}; std::vector<brlive::BakeBlob> blobs; };
    bool PlayReadXpakLighting(const std::wstring& path, PlayXpakLighting& out, std::string& error)
    {
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) { error = "the map's .xpak could not be opened"; return false; }
        uint64_t h[16] = {};
        bool ok = fread(h, 8, 16, f) == 16 && (h[0] & 0xFFFFFFFFull) == 0x4950414Bull;   // "KAPI"
        std::vector<unsigned char> index, meta;
        if (ok)
        {
            for (int i = 0; i < 3; ++i) { out.data[i] = h[3 + i]; out.index[i] = h[6 + i]; }
            ok = h[8] == h[6] * 24 && h[8] < (1ull << 30) && h[14] < (1ull << 30);
            if (ok) { index.resize((size_t)h[8]); ok = _fseeki64(f, (long long)h[7], SEEK_SET) == 0 && (index.empty() || fread(index.data(), 1, index.size(), f) == index.size()); }
            if (ok) { meta.resize((size_t)h[14]); ok = _fseeki64(f, (long long)h[13], SEEK_SET) == 0 && (meta.empty() || fread(meta.data(), 1, meta.size(), f) == meta.size()); }
        }
        fclose(f);
        if (!ok) { error = "the map's .xpak has an unexpected layout"; return false; }
        std::map<uint64_t, std::pair<uint64_t, uint64_t>> entries;   // key -> offset, size
        for (size_t i = 0; i + 24 <= index.size(); i += 24)
        {
            uint64_t e[3]; std::memcpy(e, index.data() + i, 24);
            entries[e[0]] = { e[1], e[2] };
        }
        size_t o = 0;
        for (uint64_t n = 0; n < h[12] && o + 16 <= meta.size(); ++n)
        {
            uint64_t key = 0, len = 0;
            std::memcpy(&key, meta.data() + o, 8); std::memcpy(&len, meta.data() + o + 8, 8);
            if (len > meta.size() - o - 16) break;
            const std::string text(reinterpret_cast<const char*>(meta.data() + o + 16), (size_t)len);
            o += 16 + (size_t)len;
            auto field = [&](const char* name) -> std::string
            {
                const std::string tag = std::string(name) + ": ";
                for (size_t at = 0; (at = text.find(tag, at)) != std::string::npos; ++at)
                {
                    if (at && text[at - 1] != '\n') continue;
                    at += tag.size();
                    const size_t end = text.find('\n', at);
                    return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
                }
                return std::string();
            };
            const std::string type = field("type");
            LONG t = -1;
            if (type == "reflectionProbes") t = 0; else if (type == "probeVolume") t = 1; else if (type == "SST") t = 2; else if (type == "skybox") t = 4;
            if (t < 0) continue;
            auto e = entries.find(key);
            if (e == entries.end()) continue;
            brlive::BakeBlob b; std::memset(&b, 0, sizeof b);
            b.key = key; b.offset = e->second.first; b.size = e->second.second; b.type = t;
            strncpy_s(b.name, field("name").c_str(), _TRUNCATE);
            uint64_t declared = 0;
            for (size_t at = 0; (at = text.find("\nsize", at)) != std::string::npos; )
            {
                at += 5;
                const size_t colon = text.find(": ", at);
                if (colon == std::string::npos) break;
                declared += _strtoui64(text.c_str() + colon + 2, nullptr, 10);
            }
            b.declared = declared > 0xFFFFFFFFull ? 0 : (uint32_t)declared;
            out.blobs.push_back(b);
        }
        return true;
    }

    // The exported .led's "volumes" section: every probe's exposure and average cube colour per volume and state.
    // "structure" (optional) takes what a level load compiles from the probes and the swap cannot rebuild (probe count, origin, rotation, size, blend and extension
    // volume counts). A bake whose structure differs from the level's needs a level reload (PlayLiveBakeWorker).
    bool PlayReadLedProbes(const std::wstring& path, std::vector<brlive::BakeProbe>& out, std::vector<brlive::BakeSst>& ssts, std::string* structure = nullptr)
    {
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) return false;
        std::vector<unsigned char> d;
        _fseeki64(f, 0, SEEK_END); const long long size = _ftelli64(f); _fseeki64(f, 0, SEEK_SET);
        if (size < 32 || size > (3ll << 30)) { fclose(f); return false; }
        d.resize((size_t)size);
        const bool got = fread(d.data(), 1, d.size(), f) == d.size();
        fclose(f);
        if (!got || std::memcmp(d.data(), "LED", 4) != 0) return false;
        size_t o = 0, end = d.size();
        bool ok = true;
        auto u32 = [&]() -> uint32_t { if (o + 4 > end) { ok = false; return 0; } uint32_t v; std::memcpy(&v, d.data() + o, 4); o += 4; return v; };
        auto skip = [&](uint64_t n) { if (n > end - o) { ok = false; o = end; } else o += (size_t)n; };
        auto vec = [&](uint64_t element) { const uint32_t n = u32(); skip(uint64_t(n) * element); };
        o = 12; const uint32_t headerText = u32(); skip(headerText);
        const uint32_t sections = u32();
        uint32_t version = 0; size_t at = 0, len = 0;
        for (uint32_t i = 0; i < sections && ok; ++i)
        {
            const uint32_t nameLen = u32();
            if (!ok || nameLen > end - o) return false;
            const std::string name(reinterpret_cast<const char*>(d.data() + o), nameLen); o += nameLen;
            const uint32_t ver = u32(), sz = u32();
            if (!ok || sz > end - o) return false;
            if (name == "volumes") { version = ver; at = o; len = sz; }
            o += sz;
        }
        if (!len || version != 3) return false;
        o = at; end = at + len;
        const uint32_t volumes = u32();
        for (uint32_t v = 0; v < volumes && ok; ++v)
        {
            u32(); const uint32_t mask = u32();
            float sun[3] = {}; unsigned char streamLighting = 0;
            if (o + 13 > end) { ok = false; break; }
            std::memcpy(sun, d.data() + o, 12); streamLighting = d[o + 12];
            skip(13); vec(16);
            for (int ls = 0; ls < 4; ++ls)
            {
                if (!(mask >> ls & 1)) continue;
                vec(1);
                brlive::BakeSst t; std::memset(&t, 0, sizeof t);
                t.volume = (LONG)v; t.state = ls;
                if (o + 88 > end) { ok = false; break; }
                std::memcpy(t.config, d.data() + o, 88);
                skip(64 + 8 + 16);
                const uint32_t mips = u32();
                t.mipCount = mips;
                t.mipsFileOffset = o;
                for (uint32_t m = 0; m < mips && ok; ++m)
                {
                    const uint32_t n = u32();
                    if (m < 4) t.mipElements[m] = n;
                    skip(uint64_t(n) * 4);
                }
                vec(1);
                if (o + 8 > end) { ok = false; break; }
                std::memcpy(&t.minMaxWidth, d.data() + o, 4); std::memcpy(&t.minMaxHeight, d.data() + o + 4, 4);
                skip(8); vec(4);
                const uint32_t maxs = u32();
                t.maxsFileOffset = o;
                skip(uint64_t(maxs) * 4);
                if (uint64_t(t.minMaxWidth) * t.minMaxHeight != maxs) { t.minMaxWidth = 0; t.minMaxHeight = 0; }
                t.shadowSplitDistance = sun[0]; t.shadowBiasScale = sun[1]; t.transitionTime = sun[2]; t.streamLighting = streamLighting;
                if (ok && mips >= 1 && mips <= 3) ssts.push_back(t);
            }
            const uint32_t infos = u32();
            for (uint32_t i = 0; i < infos && ok; ++i) { vec(1); skip(4 + 16); }
            const uint32_t probes = u32();
            if (structure) { structure->append(reinterpret_cast<const char*>(&v), 4); structure->append(reinterpret_cast<const char*>(&mask), 4); structure->append(reinterpret_cast<const char*>(&probes), 4); }
            for (uint32_t p = 0; p < probes && ok; ++p)
            {
                float origin[3] = {};
                if (o + 188 > end) { ok = false; break; }
                std::memcpy(origin, d.data() + o, 12);
                if (structure) structure->append(reinterpret_cast<const char*>(d.data() + o), 72);
                skip(188);
                for (int ls = 0; ls < 4 && ok; ++ls)
                {
                    if (!(mask >> ls & 1)) continue;
                    vec(6); vec(6); vec(6); vec(1); vec(1); vec(1); vec(2); vec(2); vec(2); skip(12);
                    const uint32_t mipCount = u32(), mipCount2 = u32();
                    if (mipCount != mipCount2 || mipCount > 32) { ok = false; break; }
                    for (uint32_t m = 0; m < mipCount * 6 && ok; ++m) vec(1);
                    if (o + 16 > end) { ok = false; break; }
                    brlive::BakeProbe b; std::memset(&b, 0, sizeof b);
                    b.volume = (LONG)v; b.state = ls;
                    std::memcpy(b.origin, origin, 12);
                    std::memcpy(&b.exposure, d.data() + o, 4); std::memcpy(b.avgCubeColor, d.data() + o + 4, 12);
                    skip(16);
                    out.push_back(b);
                }
            }
            if (structure && o + 4 <= end) structure->append(reinterpret_cast<const char*>(d.data() + o), 4);   // probe blend count
            vec(584); vec(16);
            const uint32_t extensions = u32();
            if (structure) structure->append(reinterpret_cast<const char*>(&extensions), 4);
            for (uint32_t e = 0; e < extensions && ok; ++e) vec(16);
        }
        return ok && o == end;
    }

    // Radiant's lighting volumes (see PlayExportBuiltLighting): is a bake running, is there a built state not yet exported.
    bool PlayLightBakeStates(bool& baking, bool& unexported)
    {
        baking = false; unexported = false;
        if (!rp::host || !rp::host->RadiantAddress) return false;
        void** mainWinPtr = static_cast<void**>(rp::host->RadiantAddress(rp::self, 0x1444C6560));   // RadiantMainWin*
        if (!mainWinPtr) return false;
        __try
        {
            char* mainWin = static_cast<char*>(*mainWinPtr);
            char* qe = mainWin ? *reinterpret_cast<char**>(mainWin + 64) : nullptr;
            char* cam = qe ? *reinterpret_cast<char**>(qe + 240) : nullptr;
            if (!cam) return false;
            char** it = *reinterpret_cast<char***>(cam + 6408);
            char** end = *reinterpret_cast<char***>(cam + 6416);
            for (int n = 0; it && it != end && n < 4096; it += 2, ++n)
            {
                const char* vol = *it;
                if (!vol) continue;
                for (int i = 0; i < 4; ++i)
                {
                    const int state = *reinterpret_cast<const int*>(vol + 1164 + 4 * i);
                    if (state >= 3 && state <= 6) baking = true;
                    if ((state == 7 || state == 8) && !vol[1180 + i]) unexported = true;
                }
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // The probe layout, live: what the linker compiled from the probes is read back out of the relinked fastfile (ffread.h) and written to BO3LiveBakeProbes.bin
    // for the bridge, which installs it with the new .xpak's probe data (bridge/livebake.cpp THE PROBE LAYOUT).
    // The level reload is the fallback when the file cannot be built or the bridge leaves a group out. [playtest] live_bake_layout=0 turns it off.
    bool g_playLiveBakeLayout = true;
    bool PlayBuildProbeFile(const PlayMapInfo& m, const std::wstring& fastFile, std::string& pathUtf8, std::string& how)
    {
        try
        {
            const uint64_t t0 = GetTickCount64();
            std::vector<uint8_t> zone; std::string error;
            if (!brff::UnpackFastFile(fastFile, zone, error)) { how = error; return false; }
            std::vector<brff::LedVolume> led;
            if (!brff::ReadLedProbeLayout(PlayLedPath(m), led)) { how = "the .led's probes could not be read"; return false; }
            std::vector<brff::ProbeGroup> groups; std::string notes;
            if (!brff::FindProbeGroups(zone, led, groups, notes)) { how = notes.empty() ? "the fastfile holds no probe group the .led describes" : notes; return false; }
            const std::wstring file = PlayJoinFile(L"BO3LiveBakeProbes.bin");
            if (file.empty() || !brff::WriteProbeFile(file, groups)) { how = "the probe layout file could not be written"; return false; }
            pathUtf8 = PlayWideToUtf8(file);
            size_t probes = 0; for (const brff::ProbeGroup& g : groups) probes += g.probes;
            char t[240];
            snprintf(t, sizeof t, "%u probe group(s) with %u probe record(s) read back from the linked fastfile (%.1f MB) in %llu ms%s%s", (unsigned)groups.size(), (unsigned)probes,
                     zone.size() / 1048576.0, (unsigned long long)(GetTickCount64() - t0), notes.empty() ? "" : "; not found: ", notes.c_str());
            how = t;
            return true;
        }
        catch (...) { how = "out of memory reading the fastfile"; return false; }
    }
    void PlayLiveBakeAskReload(const std::string& why)
    {
        { std::lock_guard<std::mutex> lock(g_playMutex); g_playBakeReloadWhy = why; }
        g_playBakeReloadWant = 1;
    }
    void PlayLiveBakeWorker(PlayMapInfo m, std::vector<uint64_t> before, std::string levelProbeKey, bool levelProbeKeyKnown)
    {
        const uint64_t t0 = GetTickCount64();
        const std::wstring zoneDir = PlayJoin(m.usermapRoot, L"zone");
        const std::wstring mainFf = PlayJoin(zoneDir, m.mapName + L".ff");
        const std::wstring xpak = PlayJoin(zoneDir, m.mapName + L".xpak");
        // every way out notes the outcome in plain words too (the [LIVE BAKE] line stays the detail)
        auto finish = [&](const std::string& line)
        {
            PlayWaitEnd("bake");
            PlayAppendLog(line);
            const bool ok = line.rfind("[LIVE BAKE] new baked lighting", 0) == 0;
            const bool reload = line.find("reloaded") != std::string::npos;
            if (ok) PlayNote("New lighting is in the game (" + PlayDuration(GetTickCount64() - t0) + ").");
            else if (line.rfind("[LIVE BAKE] cancelled", 0) == 0) PlayNote("Sending the lighting was cancelled.");
            else if (reload) {}   // PlayLiveBakeReloadTick notes the reload
            else PlayNote("The new lighting could not be sent to the running game; the next Play loads it. Verbose log has the reason.");
            g_playState = PLAY_RUNNING; g_playThreadDone = true;
        };
        std::vector<std::wstring> args;
        if (!g_playLanguage.empty()) { args.push_back(L"-language"); args.push_back(PlayUtf8ToWide(g_playLanguage)); }
        args.push_back(L"-modsource"); args.push_back(m.mapName);
        std::wstring linkerExe = PlayJoin(PlayJoin(m.toolsRoot, L"bin"), L"linker_modtools.exe");
        if (PlayBetterBuildRoute(linkerExe, args, L"linker", L"BetterBuildLinker.exe"))
            PlayAppendLog("[BETTERBUILD] Linking through BetterBuild (its launcher settings apply).");
        const uint64_t ffBefore = PlayFileTime(mainFf);
        const int linkExit = PlayRunCaptured(linkerExe, args, m.toolsRoot, "Link (new baked lighting)");
        const uint64_t ffAfter = PlayFileTime(mainFf);
        if (g_playCancel) { finish("[LIVE BAKE] cancelled."); return; }
        if (!ffAfter || ffAfter == ffBefore) { finish("[LIVE BAKE WARN] the link (exit " + std::to_string(linkExit) + ") did not refresh the fastfile; the game keeps its lighting. See the linker output above."); return; }
        PlayTouchStamp(m.stampPath, m.mapName);
        PlayXpakLighting now; std::string error;
        if (!PlayReadXpakLighting(xpak, now, error)) { finish("[LIVE BAKE WARN] " + error + "; the next Play loads the new lighting."); return; }
        // Every lighting blob of the new file goes to the bridge, which compares keys with what the level reads now (a key follows the blob's
        // content, so sending only new keys missed a bake equal to an earlier one).
        (void)before;
        std::vector<brlive::BakeBlob> fresh = now.blobs;
        if (fresh.empty()) { finish("[LIVE BAKE] the linked .xpak holds no lighting data."); return; }
        if (fresh.size() > (size_t)brlive::kMaxBakeBlobs) { finish("[LIVE BAKE WARN] the link added " + std::to_string(fresh.size()) + " lighting blobs, more than the bridge takes; the next Play loads the new lighting."); return; }
        brlive::SharedState* sh = g_playLiveShared;
        if (!sh || sh->bakeSwapState != 1) { finish("[LIVE BAKE WARN] the game's bridge is not ready for a lighting swap; the next Play loads the new lighting."); return; }
        std::vector<brlive::BakeProbe> probes;
        std::vector<brlive::BakeSst> ssts;
        std::string probeKey;
        if (!PlayReadLedProbes(PlayLedPath(m), probes, ssts, &probeKey)) { probes.clear(); ssts.clear(); probeKey.clear(); levelProbeKeyKnown = false; PlayAppendLog("[LIVE BAKE WARN] the .led's probe values could not be read; probe exposure keeps the old bake's values until the next level load."); }
        // a changed probe layout (or one that cannot be compared): the linker's records go to the bridge with the swap
        const bool layoutChanged = !probeKey.empty() && (!levelProbeKeyKnown || probeKey != levelProbeKey);
        std::string probeFile;
        if (layoutChanged)
        {
            std::string how;
            if (g_playLiveBakeLayout && PlayBuildProbeFile(m, mainFf, probeFile, how))
                PlayAppendLog(std::string("[LIVE BAKE] ") + (levelProbeKeyKnown ? "the bake changes the reflection probe layout" : "the level's probe layout is not known") + ": " + how + ".");
            else if (levelProbeKeyKnown && g_playLiveBakeReload)
            {
                // nothing is swapped: half of a bake under the old probe layout would only show for the seconds before the reload
                PlayLiveBakeAskReload("reflection probes were added, removed, moved or resized and their layout could not be sent live (" + (g_playLiveBakeLayout ? how : std::string("live_bake_layout=0")) + ")");
                finish("[LIVE BAKE] the bake changes the reflection probe layout: the level is reloaded with the new lighting.");
                return;
            }
            else if (levelProbeKeyKnown) PlayAppendLog("[LIVE BAKE WARN] the bake changes the reflection probe layout and it could not be sent (" + how + "); the probes keep the old layout until the next Play.");
        }
        if (probes.size() > (size_t)brlive::kMaxBakeProbes) probes.resize((size_t)brlive::kMaxBakeProbes);
        for (size_t i = 0; i < probes.size(); ++i) sh->bakeProbe[i] = probes[i];
        sh->bakeProbeCount = (LONG)probes.size();
        if (ssts.size() > (size_t)brlive::kMaxBakeSsts) ssts.resize((size_t)brlive::kMaxBakeSsts);
        for (size_t i = 0; i < ssts.size(); ++i) sh->bakeSst[i] = ssts[i];
        sh->bakeSstCount = (LONG)ssts.size();
        for (size_t i = 0; i < fresh.size(); ++i) sh->bakeBlob[i] = fresh[i];
        sh->bakeBlobCount = (LONG)fresh.size();
        for (int i = 0; i < 3; ++i) { sh->bakePakData[i] = now.data[i]; sh->bakePakIndex[i] = now.index[i]; }
        strncpy_s(sh->bakePakName, PlayWideToUtf8(m.mapName).c_str(), _TRUNCATE);
        { std::string path = PlayWideToUtf8(PlayLedPath(m)); strncpy_s(sh->bakeLedPath, path.c_str(), _TRUNCATE); }
        strncpy_s(sh->bakeProbePath, probeFile.c_str(), _TRUNCATE);
        { std::string path = PlayWideToUtf8(xpak); for (char& c : path) if (c == '\\') c = '/'; strncpy_s(sh->bakePakPath, path.c_str(), _TRUNCATE); }
        MemoryBarrier();
        const LONG seq = InterlockedIncrement(&sh->bakeSwapSeq);
        const uint64_t waitFrom = GetTickCount64();
        PlayWaitBegin("bake", "waiting for BO3 to swap in the new lighting");
        while (sh->bakeSwapDoneSeq != seq && GetTickCount64() - waitFrom < 20000 && !g_playCancel && g_playLiveShared == sh) Sleep(50);
        char secs[32]; snprintf(secs, sizeof secs, "%.1f", (GetTickCount64() - t0) / 1000.0);
        if (g_playLiveShared != sh || sh->bakeSwapDoneSeq != seq) { finish(std::string("[LIVE BAKE WARN] the game did not answer the lighting swap (") + std::to_string(fresh.size()) + " blob(s) sent); the next Play loads the new lighting."); return; }
        char text[512]; strncpy_s(text, sh->bakeSwapText, _TRUNCATE);
        const bool partial = sh->bakeSwapResult == 1 && std::strstr(text, "not swapped:") != nullptr;
        if (sh->bakeSwapResult == 1 && !partial && !probeKey.empty())
        {
            // the level now holds this bake's probe layout
            std::lock_guard<std::mutex> lock(g_playMutex);
            g_playLevelProbeKey = probeKey; g_playLevelProbeKeySet = true;
        }
        if (g_playLiveBakeReload && (partial || sh->bakeSwapResult != 1))
            PlayLiveBakeAskReload(partial ? "part of the bake could not be swapped live" : "the game could not take the bake live");
        if (sh->bakeSwapResult == 1) finish(std::string("[LIVE BAKE] new baked lighting is in the running game after ") + secs + " s: " + text + (partial && g_playLiveBakeReload ? ". The level is reloaded for the rest." : ""));
        else finish(std::string("[LIVE BAKE WARN] the game kept its lighting: ") + text + (g_playLiveBakeReload ? ". The level is reloaded with the new lighting." : ". Press Play to reload the level with the new lighting."));
    }

    // Main thread. Exports the built lighting when Radiant has one that was never exported, then links and swaps.
    bool PlayLiveBakeStart(std::string& detail)
    {
        if (!g_playLiveShared || !g_playLiveBridgeInjected) { detail = "no running game with the live bridge"; return false; }
        if (g_playLiveShared->bakeSwapState != 1) { detail = std::string("the bridge cannot swap lighting: ") + g_playLiveShared->bakeSwapText; return false; }
        if (PlayBusy()) { detail = "a build is running"; return false; }
        if (g_playThread.joinable())
        {
            if (!g_playThreadDone) { detail = "a build is running"; return false; }
            g_playThread.join();
        }
        PlayMapInfo m; std::string error;
        if (!PlayResolveCurrentMap(m, error)) { detail = error; return false; }
        if (!g_playGameMap.empty() && _stricmp(g_playGameMap.c_str(), PlayWideToUtf8(m.mapName).c_str()) != 0) { detail = "the game runs another map"; return false; }
        const std::wstring zoneDir = PlayJoin(m.usermapRoot, L"zone");
        PlayXpakLighting had;
        if (!PlayReadXpakLighting(PlayJoin(zoneDir, m.mapName + L".xpak"), had, error)) { detail = error; return false; }
        PlayAppendLog("=== Live bake: new baked lighting into the running game ===");
        PlayNote("Sending the new lighting to the running game.");
        bool keySet = false;
        { std::lock_guard<std::mutex> lock(g_playMutex); keySet = g_playLevelProbeKeySet; }
        if (!keySet)
        {
            // before the export overwrites it: the .led on disk is the one the running level was linked from
            std::vector<brlive::BakeProbe> p; std::vector<brlive::BakeSst> t; std::string key;
            if (PlayFileTime(PlayLedPath(m)) <= PlayFileTime(PlayJoin(zoneDir, m.mapName + L".ff")) && PlayReadLedProbes(PlayLedPath(m), p, t, &key))
            { std::lock_guard<std::mutex> lock(g_playMutex); g_playLevelProbeKey.swap(key); g_playLevelProbeKeySet = true; }
        }
        PlayExportBuiltLighting(m);
        if (PlayFileTime(PlayLedPath(m)) <= PlayFileTime(PlayJoin(zoneDir, m.mapName + L".ff")))
        {
            detail = "the exported lighting (.led) is not newer than the fastfile: nothing to send";
            PlayAppendLog("[LIVE BAKE] " + detail);
            PlayNote("The exported lighting is not newer than the game's: nothing to send.");
            return false;
        }
        std::vector<uint64_t> before;
        for (const brlive::BakeBlob& b : had.blobs) before.push_back(b.key);
        g_playCancel = false; g_playThreadDone = false;
        g_playState = PLAY_LINKING;
        PlayAppendLog("[LIVE BAKE] linking the base zone with the new lighting; the game keeps running.");
        PlayNote("Linking the new lighting into the fastfile (the game keeps running)...");
        PlayWaitBegin("bake", "linking the new lighting");
        std::string levelKey; bool levelKeyKnown = false;
        { std::lock_guard<std::mutex> lock(g_playMutex); levelKey = g_playLevelProbeKey; levelKeyKnown = g_playLevelProbeKeySet; }
        g_playThread = std::thread([m, before, levelKey, levelKeyKnown]() { PlayLiveBakeWorker(m, before, levelKey, levelKeyKnown); });
        detail = "linking";
        return true;
    }

    // Main thread, every second: a bake that just finished while a game runs starts the live bake by itself.
    void PlayLiveBakeReloadTick(uint64_t now)
    {
        const int want = g_playBakeReloadWant.load();
        if (!want) return;
        if (PlayBusy() || !g_playThreadDone) return;   // the bake's own link is still finishing
        if (!g_playLiveShared || !g_playLiveBridgeInjected || g_playState != PLAY_RUNNING) { g_playBakeReloadWant = 0; return; }   // the game is gone: the next Play loads the bake anyway
        if (g_playJoinMode) { g_playBakeReloadWant = 0; PlayAppendLog("[LIVE BAKE] joined to a host's game: no reload from here; the host's next Play loads the new lighting."); return; }   // a Play would leave the host
        if (want == 1)
        {
            std::string why; { std::lock_guard<std::mutex> lock(g_playMutex); why = g_playBakeReloadWhy; }
            PlayAppendLog("[LIVE BAKE] " + why + ": reloading the level in the running game (live_bake_reload=0 turns this off).");
            PlayNote("Part of the new lighting needs a level reload: reloading the map in the running game (you keep your place).");
            // the game notes where the player stands (the receiver of this Play's GSC hook, an older hook ignores it)
            const bool asked = PlayLiveZoneSendCommands({ "set br_play_keep " + std::to_string((long)((now & 0x3FFFFFFF) | 1)) });
            g_playKeepPosNext = asked;
            g_playBakeReloadDueMs = now + (asked ? 900 : 0);
            g_playBakeReloadWant = 2;
            return;
        }
        if (now < g_playBakeReloadDueMs) return;
        g_playBakeReloadWant = 0;
        g_playForceReloadOnce = true;
        if (!PlayStart(false, false)) { g_playForceReloadOnce = false; g_playKeepPosNext = false; PlayAppendLog("[LIVE BAKE WARN] the reload could not be started; press Play to load the new lighting."); }
    }
    void PlayLiveBakeTick(uint64_t now)
    {
        PlayLiveBakeReloadTick(now);
        if (now - g_playBakeLastCheck < 1000) return;
        g_playBakeLastCheck = now;
        bool baking = false, unexported = false;
        if (!PlayLightBakeStates(baking, unexported)) return;
        const bool finished = g_playBakeWasBaking && !baking;
        const bool started = !g_playBakeWasBaking && baking;
        g_playBakeWasBaking = baking;
        // Radiant's light build runs for minutes: the simple log says it started and nudges while it runs
        const bool liveTarget = g_playLiveBake && g_playExportLighting && g_playLiveShared && g_playLiveBridgeInjected && g_playState == PLAY_RUNNING;
        if (started)
        {
            PlayWaitBegin("lightbuild", "waiting for Radiant's light build");
            PlayNote(liveTarget ? "Radiant is building the lighting. When it finishes, the new lighting goes into the running game."
                                : "Radiant is building the lighting. The next Play uses it once it finishes.");
        }
        if (finished)
        {
            const uint64_t ms = PlayWaitEnd("lightbuild");
            PlayNote("Radiant's light build finished" + (ms ? " (" + PlayDuration(ms) + ")" : std::string()) + ".");
        }
        if (!finished || !unexported || !g_playLiveBake || !g_playExportLighting) return;
        if (!g_playLiveShared || !g_playLiveBridgeInjected || g_playState != PLAY_RUNNING) return;
        std::string detail;
        if (!PlayLiveBakeStart(detail))
        {
            PlayAppendLog("[LIVE BAKE] bake finished, not sent to the game: " + detail);
            if (detail.find("nothing to send") == std::string::npos) PlayNote("The new lighting was not sent to the running game (" + detail + "). The next Play loads it.");
        }
    }

    std::string PlayStatusText()
    {
        std::ostringstream o;
        o << "state=" << PlayStateName(g_playState.load()) << " mode=" << (g_playFullMode ? "full" : "smart")
          << " map=" << (g_playCurrentMap.empty() ? "(none)" : g_playCurrentMap);
        DWORD pid = 0;
        { std::lock_guard<std::mutex> lock(g_playMutex); pid = g_playGamePid; }
        o << " build=" << (g_playExeBuild == PLAY_BUILD_ENHANCED ? "enhanced" : g_playExeBuild == PLAY_BUILD_STEAM ? "steam" : g_playExeBuild == PLAY_BUILD_UNKNOWN ? "unknown" : "none");
        if (pid)
        {
            o << " game_pid=" << pid;
            if (!g_playGamePresented) o << " runtime=resident"; if (g_playParked) o << " parked=1";
        }
        uint64_t started = g_playStartedMs.load();
        if (started && PlayBusy()) o << " elapsed=" << ((GetTickCount64() - started) / 1000.0) << "s";
        std::string e; { std::lock_guard<std::mutex> lock(g_playMutex); e = g_playLastError; }
        if (!e.empty()) o << " error=" << e;
        return o.str();
    }

    // The pane's status line while the verbose log is off: the longest open wait, else the state in plain words.
    std::string PlaySimpleStatusText()
    {
        const uint64_t now = GetTickCount64();
        std::string wait; uint64_t waitSince = 0; std::string error;
        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            for (const auto& kv : g_playWaits)
                if (!waitSince || kv.second.since < waitSince) { waitSince = kv.second.since; wait = kv.second.what; }
            error = g_playLastError;
        }
        if (!wait.empty())
        {
            wait[0] = (char)toupper((unsigned char)wait[0]);
            return wait + " (" + PlayDuration(now - waitSince) + ")";
        }
        switch (g_playState.load())
        {
            case PLAY_SAVING: return "Saving the map";
            case PLAY_PREPARING: return "Preparing";
            case PLAY_COMPILING: return "Compiling the map";
            case PLAY_LINKING: return "Linking";
            case PLAY_LAUNCHING: return "Starting BO3";
            case PLAY_RUNNING: return g_playParked ? "BO3 is parked" : g_playGamePid ? "Playing " + (g_playGameMap.empty() ? std::string("the map") : g_playGameMap) : "Ready";
            case PLAY_FAILED: return "Failed: " + PlayStripTag(error);
            case PLAY_STOPPED: return "Stopped";
            default: return "Ready";
        }
    }

    // Radiant's saved dock layout (HKCU, the string the host's dock code reads too) has a "d|f|p <name>" entry per pane
    // it has seen, a pane without one is new to this user.
    bool PlayLayoutKnowsPane(const wchar_t* name)
    {
        DWORD size = 0;
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Treyarch\\Radiant", L"QRadDockWidgetState", RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size) return false;
        std::wstring buf(size / sizeof(wchar_t) + 1, L'\0');
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Treyarch\\Radiant", L"QRadDockWidgetState", RRF_RT_REG_SZ, nullptr, &buf[0], &size) != ERROR_SUCCESS) return false;
        const std::wstring needle = std::wstring(L" ") + name;
        for (size_t pos = 0; (pos = buf.find(needle, pos)) != std::wstring::npos; pos += needle.size())
        {
            const wchar_t after = pos + needle.size() < buf.size() ? buf[pos + needle.size()] : L'\0';
            const wchar_t kind = pos ? buf[pos - 1] : L'\0';
            if ((kind == L'd' || kind == L'f' || kind == L'p') && (after == L'\n' || after == L'\r' || after == L')' || after == L'\0')) return true;
        }
        return false;
    }

    void PlayShowPane()
    {
        g_playHideNewPaneUntil = 0;
        if (g_playPane && rp::host && rp::host->ShowPane)
            rp::host->ShowPane(rp::self, g_playPane, 1);
    }

    void PlayCheckMapOnly()
    {
        if (PlayBusy()) return;
        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            g_playLastError.clear(); PlayClearLogLocked();
        }
        g_playLastUiLogSize = (size_t)-1;
        if (g_playAutoSave)
        {
            std::string action;
            if (PlaySaveCurrentMap(action)) PlayAppendLog("[SAVE] Radiant action: " + action);
        }
        PlayMapInfo m; std::string error;
        if (!PlayResolveCurrentMap(m, error))
        {
            PlaySetError(error);
            g_playThreadDone = true;
            PlayShowPane();
            return;
        }
        g_playCurrentMap = PlayWideToUtf8(m.mapName);
        PlayPreflightReport report = PlayPreflightMap(m);
        PlayPublishPreflight(report, true);
        if (report.blockers > 0)
        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            g_playLastError = "Map setup has blocking issues.";
            g_playState = PLAY_FAILED;
        }
        else g_playState = PLAY_IDLE;
        PlayShowPane();
    }

    bool PlayJoinStart(std::string host);
    void PlayPrimaryClicked(void*)
    {
        if (PlayBusy()) { PlayStop(); return; }
        if (g_playHostPlaying) { PlayJoinStart(""); return; }   // the button reads Join Host
        bool full = g_playFullCheck && rp::host && rp::host->ui && rp::host->ui->IsChecked && rp::host->ui->IsChecked(g_playFullCheck) != 0;
        PlayStart(full);
    }

    // Engine buttons: plain console commands through the zone module's mailbox, which works on every build it knows (Steam and Enhanced).
    void PlayConsoleButton(const std::string& command, const char* what)
    {
        if (PlayLiveZoneSendCommands({ command }))
        {
            PlayAppendLog(std::string("[CONSOLE] ") + what + ": " + command);
            PlayNote(std::string(what) + ".");
        }
        else
        {
            PlayAppendLog(std::string("[CONSOLE] ") + what + " not sent: the zone module is not running in the game (it starts 20 s after a launch, only on a build it knows, and only with Live zone or Play From Here on).");
            PlayNote(std::string(what) + ": not sent, the game is not ready for commands yet (about 20 s after it starts).");
        }
    }
    // Disable AI: the same as typing "ai_disableSpawn 1" in the console, saved at once. The engine resets the dvar on every level load and script
    // SetDvar does not reach it, so the box is published as SharedState::disableAi and the bridge's zone module queues the command once a second.
    // The untick sends "ai_disableSpawn 0" through the mailbox.
    void PlayDisableAiClicked(void*)
    {
        if (!g_playDisableAiCheck || !rp::host || !rp::host->ui || !rp::host->ui->IsChecked) return;
        g_playDisableAi = rp::host->ui->IsChecked(g_playDisableAiCheck) != 0;
        rp::ConfigSet("playtest", "disable_ai", g_playDisableAi ? "1" : "0");
        if (g_playLiveShared) InterlockedExchange(&g_playLiveShared->disableAi, g_playDisableAi ? 1 : 0);
        if (!g_playGamePid) return;
        const std::string v = g_playDisableAi ? "1" : "0";
        if (PlayLiveZoneSendCommands({ "ai_disableSpawn " + v }))
            PlayAppendLog(std::string("[CONSOLE] AI spawning ") + (g_playDisableAi ? "disabled" : "enabled") + ": ai_disableSpawn " + v);
        else
            PlayAppendLog("[CONSOLE] ai_disableSpawn not sent yet: the zone module is not running in the game (it starts 20 s after a launch, only on a build it knows). It applies the box as soon as it is up.");
    }
    // ---- Fly and God ----
    // BO3's noclip and god console commands only toggle, so PlayCheatsTick reads their replies from console_mp.log and toggles until
    // the state matches the box. It acts after a click or a level start only.
    struct PlayCheat { const char* command; const char* reply; bool on; bool sync; int sends; uint64_t pendingUntil; };
    PlayCheat         g_playCheatFly = { "noclip", "[msg]noclip ", false, false, 0, 0 };
    PlayCheat         g_playCheatGod = { "god", "[msg]godmode ", false, false, 0, 0 };
    DWORD             g_playCheatPid = 0;
    uint64_t          g_playCheatGameStartFt = 0;  // the game's creation time: an older console_mp.log is the last game's
    uint64_t          g_playCheatLogPos = 0;
    uint64_t          g_playCheatNextMs = 0;
    uint64_t          g_playCheatLevelMs = 0;      // a level started, waiting for the player's Connected line
    uint64_t          g_playCheatReadyMs = 0;      // 0 = the player is not in the level yet

    void PlayCheatsResync(bool fly, bool god)
    {
        if (fly) { g_playCheatFly.sync = true; g_playCheatFly.sends = 0; }
        if (god) { g_playCheatGod.sync = true; g_playCheatGod.sends = 0; }
    }

    void PlayCheatsLine(const std::string& line, uint64_t now)
    {
        if (line.find("------- Game Initialization -------") != std::string::npos)
        {
            g_playCheatLevelMs = now; g_playCheatReadyMs = 0;
            for (PlayCheat* c : { &g_playCheatFly, &g_playCheatGod }) { c->on = false; c->pendingUntil = 0; }
            PlayCheatsResync(true, true);
            return;
        }
        if (g_playCheatLevelMs && line.find("[msg]") != std::string::npos && line.find(" Connected") != std::string::npos)
        {
            if (!g_playCheatReadyMs) g_playCheatReadyMs = now + 1000;   // the spawn follows the connect
            return;
        }
        for (PlayCheat* c : { &g_playCheatFly, &g_playCheatGod })
        {
            size_t at = line.find(c->reply);
            if (at == std::string::npos) continue;
            c->on = line.compare(at + strlen(c->reply), 2, "ON") == 0;
            c->pendingUntil = 0;
        }
    }

    void PlayCheatsTick(uint64_t now)
    {
        if (now < g_playCheatNextMs) return;
        g_playCheatNextMs = now + 250;
        DWORD pid = g_playGamePid;
        if (pid != g_playCheatPid)
        {
            g_playCheatPid = pid; g_playCheatLogPos = 0; g_playCheatLevelMs = g_playCheatReadyMs = 0; g_playCheatGameStartFt = 0;
            for (PlayCheat* c : { &g_playCheatFly, &g_playCheatGod }) { c->on = false; c->pendingUntil = 0; }
            HANDLE game = nullptr;
            { std::lock_guard<std::mutex> lock(g_playMutex); game = g_playGameProcess; }
            FILETIME created, exited, kernel, user;
            if (game && GetProcessTimes(game, &created, &exited, &kernel, &user))
                g_playCheatGameStartFt = ((uint64_t)created.dwHighDateTime << 32) | created.dwLowDateTime;
        }
        if (!pid || g_playJoinMode) return;

        const std::wstring path = PlayJoin(PlayGameRoot(), L"console_mp.log");
        if (PlayFileTime(path) < g_playCheatGameStartFt) return;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER size = {};
            if (GetFileSizeEx(h, &size))
            {
                if ((uint64_t)size.QuadPart < g_playCheatLogPos)   // rewritten by a new launch
                {
                    g_playCheatLogPos = 0; g_playCheatLevelMs = g_playCheatReadyMs = 0;
                    for (PlayCheat* c : { &g_playCheatFly, &g_playCheatGod }) { c->on = false; c->pendingUntil = 0; }
                }
                const uint64_t avail = (std::min<uint64_t>)((uint64_t)size.QuadPart - g_playCheatLogPos, 4u << 20);
                LARGE_INTEGER at = {}; at.QuadPart = (LONGLONG)g_playCheatLogPos;
                std::string text((size_t)avail, '\0');
                DWORD got = 0;
                if (avail && SetFilePointerEx(h, at, nullptr, FILE_BEGIN) && ReadFile(h, &text[0], (DWORD)avail, &got, nullptr))
                {
                    text.resize(got);
                    size_t end = text.rfind('\n');
                    if (end != std::string::npos)
                    {
                        size_t from = 0;
                        while (from <= end)
                        {
                            size_t nl = text.find('\n', from);
                            PlayCheatsLine(text.substr(from, nl - from), now);
                            from = nl + 1;
                        }
                        g_playCheatLogPos += end + 1;
                    }
                }
            }
            CloseHandle(h);
        }

        if (g_playCheatLevelMs && !g_playCheatReadyMs && now - g_playCheatLevelMs > 90000) g_playCheatReadyMs = now;   // no Connected line on this map
        if (!g_playCheatReadyMs || now < g_playCheatReadyMs || !g_playLiveZoneInjected) return;
        for (PlayCheat* c : { &g_playCheatFly, &g_playCheatGod })
        {
            const bool want = c == &g_playCheatFly ? g_playFly : (g_playGod || g_playParked);
            if (!c->sync || now < c->pendingUntil) continue;
            const char* name = c == &g_playCheatFly ? "Fly" : "God mode";
            if (c->on == want)
            {
                c->sync = false;
                if (c->sends && !g_playParked) PlayNote(std::string(name) + (want ? " on." : " off."));   // the game's reply confirmed it
                continue;
            }
            if (c->sends >= 6)
            {
                c->sync = false;
                PlayAppendLog(std::string("[CONSOLE WARN] BO3 did not answer '") + c->command + "' (no reply in console_mp.log): the box is not applied in this game.");
                if (!g_playParked) PlayNote(std::string(name) + " could not be switched: BO3 did not answer.");
                continue;
            }
            if (!PlayLiveZoneSendCommand(c->command)) continue;
            c->sends++;
            c->pendingUntil = now + 3000;
        }
    }

    void PlayVerboseClicked(void*)
    {
        if (!g_playVerboseCheck || !rp::host || !rp::host->ui || !rp::host->ui->IsChecked) return;
        g_playVerboseLog = rp::host->ui->IsChecked(g_playVerboseCheck) != 0;
        rp::ConfigSet("playtest", "verbose_log", g_playVerboseLog ? "1" : "0");
        g_playLastUiLogSize = (size_t)-1;   // redraw with the other text
    }
    void PlayFlyClicked(void*)
    {
        if (!g_playFlyCheck || !rp::host || !rp::host->ui || !rp::host->ui->IsChecked) return;
        g_playFly = rp::host->ui->IsChecked(g_playFlyCheck) != 0;
        rp::ConfigSet("playtest", "play_fly", g_playFly ? "1" : "0");
        PlayCheatsResync(true, false);
        if (!g_playGamePid) return;
        if (g_playLiveZoneInjected)
            PlayAppendLog(std::string("[CONSOLE] Fly (noclip) ") + (g_playFly ? "on" : "off"));
        else
            PlayAppendLog("[CONSOLE] Fly not sent yet: the zone module is not running in the game (it starts 20 s after a launch, only on a build it knows). It applies the box as soon as it is up.");
    }
    void PlayGodClicked(void*)
    {
        if (!g_playGodCheck || !rp::host || !rp::host->ui || !rp::host->ui->IsChecked) return;
        g_playGod = rp::host->ui->IsChecked(g_playGodCheck) != 0;
        rp::ConfigSet("playtest", "play_god", g_playGod ? "1" : "0");
        PlayCheatsResync(false, true);
        if (!g_playGamePid) return;
        if (g_playLiveZoneInjected)
            PlayAppendLog(std::string("[CONSOLE] God mode ") + (g_playGod ? "on" : "off"));
        else
            PlayAppendLog("[CONSOLE] God not sent yet: the zone module is not running in the game (it starts 20 s after a launch, only on a build it knows). It applies the box as soon as it is up.");
    }
    // Spawn at camera: saved at once, and ticking it while the game runs moves the player to the Radiant camera right away (the same sets a hot Play sends).
    void PlayFromCameraClicked(void*)
    {
        if (!g_playFromCameraCheck || !rp::host || !rp::host->ui || !rp::host->ui->IsChecked) return;
        g_playFromCamera = rp::host->ui->IsChecked(g_playFromCameraCheck) != 0;
        rp::ConfigSet("playtest", "play_from_camera", g_playFromCamera ? "1" : "0");
        if (!g_playFromCamera || !g_playGamePid || PlayBusy()) return;
        PlayCaptureCamera(true);
        if (g_playCamDvar.empty()) { PlayAppendLog("[PLAY HERE] the 3D view is not up; the player stays where they are."); return; }
        if (PlayLiveZoneSendCommands(PlayHereConsoleSets()))
            PlayAppendLog("[PLAY HERE] the player is moved to the Radiant camera: " + g_playCamDvar + " (" + PlayHereMode() + ").");
        else
            PlayAppendLog("[PLAY HERE] not sent: the zone module is not running in the game (it starts 20 s after a launch, only on a build it knows). The next Play spawns at the camera.");
    }
    void PlayEngineReloadClicked(void*) { PlayConsoleButton("map_restart", "Restarting the map"); }
    void PlayEngineFastRestartClicked(void*) { PlayConsoleButton("fast_restart", "Fast restart: the round restarts, the assets stay loaded"); }
    // Mutes or unmutes every audio session of the process on every active render device (Core Audio, any thread).
    // Returns the number of sessions changed, 0 while the game has not opened its audio yet.
    int PlaySetProcessMuted(DWORD pid, bool mute)
    {
        if (!pid) return 0;
        bool uninit = false;
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) uninit = true;   // S_FALSE too: balanced below
        int changed = 0;
        IMMDeviceEnumerator* en = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en))) && en)
        {
            IMMDeviceCollection* devs = nullptr;
            UINT nDev = 0;
            if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devs)) && devs && SUCCEEDED(devs->GetCount(&nDev)))
                for (UINT d = 0; d < nDev; ++d)
                {
                    IMMDevice* dev = nullptr;
                    if (FAILED(devs->Item(d, &dev)) || !dev) continue;
                    IAudioSessionManager2* mgr = nullptr;
                    if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&mgr))) && mgr)
                    {
                        IAudioSessionEnumerator* sessions = nullptr;
                        int n = 0;
                        if (SUCCEEDED(mgr->GetSessionEnumerator(&sessions)) && sessions && SUCCEEDED(sessions->GetCount(&n)))
                            for (int i = 0; i < n; ++i)
                            {
                                IAudioSessionControl* ctl = nullptr;
                                if (FAILED(sessions->GetSession(i, &ctl)) || !ctl) continue;
                                IAudioSessionControl2* ctl2 = nullptr;
                                DWORD spid = 0;
                                if (SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&ctl2))) && ctl2 &&
                                    SUCCEEDED(ctl2->GetProcessId(&spid)) && spid == pid)
                                {
                                    ISimpleAudioVolume* vol = nullptr;
                                    if (SUCCEEDED(ctl->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&vol))) && vol)
                                    {
                                        if (SUCCEEDED(vol->SetMute(mute ? TRUE : FALSE, nullptr))) ++changed;
                                        vol->Release();
                                    }
                                }
                                if (ctl2) ctl2->Release();
                                ctl->Release();
                            }
                        if (sessions) sessions->Release();
                        mgr->Release();
                    }
                    dev->Release();
                }
            if (devs) devs->Release();
            en->Release();
        }
        if (uninit) CoUninitialize();
        return changed;
    }

    // Free physical memory in GB. A parked game holds 4 to 6 GB and big bakes run Radiant out of memory: the background start needs
    // [playtest] prewarm_min_free_gb (12) free, a parked game closes itself below park_min_free_gb (3).
    double PlayFreeMemoryGb()
    {
        MEMORYSTATUSEX ms = { sizeof(ms) };
        return GlobalMemoryStatusEx(&ms) ? ms.ullAvailPhys / (1024.0 * 1024.0 * 1024.0) : 1e9;
    }

    // The player's own frame cap, remembered before the parked cap goes in (the ini survives a Radiant crash).
    int PlayConfigMaxFps(const std::wstring& gameRoot);
    void PlayParkRememberFps()
    {
        PlayMapInfo m; std::string error;
        if (g_playParkGameRoot.empty() && PlayResolveCurrentMap(m, error)) g_playParkGameRoot = m.gameRoot;
        if (g_playParkGameRoot.empty()) return;
        const int fps = PlayConfigMaxFps(g_playParkGameRoot);
        if (fps >= 0 && fps != kPlayParkFps) rp::ConfigSet("playtest", "park_maxfps", std::to_string(fps));
    }

    // UI tick: a parked game gets muted (its audio session may appear seconds after a background launch) and gets the
    // park commands once the zone module takes its mailbox (20 s after a launch).
    void PlayParkTick(uint64_t now)
    {
        // Windows may keep an app's mute for its next run: a game muted while parked whose Radiant died before Play is
        // unmuted at the next launch ([playtest] park_muted is set while a mute is in force).
        static DWORD checkedPid = 0;
        static uint64_t nextUnmute = 0;
        if (!g_playParked && g_playGamePid && checkedPid != g_playGamePid && now >= nextUnmute)
        {
            if (!rp::ConfigGetInt("playtest", "park_muted", 0)) checkedPid = g_playGamePid;
            else if (PlaySetProcessMuted(g_playGamePid, false) > 0) { checkedPid = g_playGamePid; rp::ConfigSet("playtest", "park_muted", "0"); }
            else nextUnmute = now + 2000;
        }
        if (!g_playParked || !g_playGamePid) return;
        static uint64_t nextMemCheck = 0;
        if (now >= nextMemCheck && !PlayBusy())
        {
            nextMemCheck = now + 2000;
            const double freeGb = PlayFreeMemoryGb();
            if (freeGb < (std::max)(1, rp::ConfigGetInt("playtest", "park_min_free_gb", 3)))
            {
                char line[200]; snprintf(line, sizeof line, "[PARK] only %.1f GB of memory free: the parked BO3 was closed so Radiant keeps its memory (the next Play starts it again).", freeGb);
                if (g_playParkMuted && PlaySetProcessMuted(g_playGamePid, false) > 0) rp::ConfigSet("playtest", "park_muted", "0");
                if (!g_playCurrentMap.empty()) g_playPrewarmSuppressedMap = g_playCurrentMap;
                PlayStop(false);
                PlayAppendLog(line);
                return;
            }
        }
        if (!g_playParkCmdsSent && g_playLiveZoneInjected &&
            PlayLiveZoneSendCommands({ "ai_disableSpawn 1", "com_maxfps " + std::to_string(kPlayParkFps) }))
        {
            g_playParkCmdsSent = true;
            PlayCheatsResync(false, true);
        }
        if (g_playParkMuted || now < g_playParkMuteNextMs) return;
        g_playParkMuteNextMs = now + 2000;
        if (PlaySetProcessMuted(g_playGamePid, true) > 0) { g_playParkMuted = true; rp::ConfigSet("playtest", "park_muted", "1"); }
    }

    // players\config.ini MaxFPS (com_maxfps is an archived dvar): what Play gives back after the parked cap, -1 unknown.
    int PlayConfigMaxFps(const std::wstring& gameRoot)
    {
        std::istringstream in(PlayReadTextFile(PlayVideoConfigPath(gameRoot), 1024 * 1024));
        std::string line;
        while (std::getline(in, line))
        {
            if (line.compare(0, 6, "MaxFPS") != 0) continue;
            const size_t q = line.find('"');
            if (q != std::string::npos) return atoi(line.c_str() + q + 1);
        }
        return -1;
    }
    // The game exited: if it wrote the parked cap into its config (an archived dvar), the remembered value goes back.
    void PlayParkGuardConfigOnExit()
    {
        if (g_playParkGameRoot.empty()) return;
        const int keep = rp::ConfigGetInt("playtest", "park_maxfps", -1);
        const std::wstring cfg = PlayVideoConfigPath(g_playParkGameRoot);
        if (keep >= 0 && PlayConfigMaxFps(g_playParkGameRoot) == kPlayParkFps && keep != kPlayParkFps)
        {
            std::string text = PlayReadTextFile(cfg, 1024 * 1024);
            const size_t at = text.find("MaxFPS = \"" + std::to_string(kPlayParkFps) + "\"");
            if (at != std::string::npos)
            {
                text.replace(at, 12 + std::to_string(kPlayParkFps).size(), "MaxFPS = \"" + std::to_string(keep) + "\"");
                if (PlayWriteTextFile(cfg, text)) PlayAppendLog("[PARK] players\\config.ini MaxFPS put back to " + std::to_string(keep) + " (the parked game had saved its cap).");
            }
        }
    }

    // UI tick: a parked game's window stays hidden (also if BO3 shows it itself), so only Play or Close BO3 gets back to it.
    // Async calls only: a docked child shares the UI thread's input queue. Shown again by a dock once the park ends.
    bool g_playParkHidden = false;
    void PlayParkHideTick()
    {
        HWND game = g_playEmbeddedHwnd;
        if (!game || !IsWindow(game)) { if (!g_playParked) g_playParkHidden = false; return; }
        if (g_playParked)
        {
            g_playParkHidden = true;
            static uint64_t lastHide = 0;
            const uint64_t now = GetTickCount64();
            if ((GetWindowLongPtrW(game, GWL_STYLE) & WS_VISIBLE) && now - lastHide >= 250)
            {
                lastHide = now;
                ShowWindowAsync(game, SW_HIDE);
            }
            return;
        }
        if (g_playParkHidden && PlayGamePaneHostHwnd() && PlayGameTakesDockRequests(game))
        {
            g_playParkHidden = false;
            PlayApplyEmbeddedChildPresentation(game);   // the bridge's dock shows the window again
        }
    }

    // End Session: the game keeps its level and hides, Play then reloads or restarts it in seconds.
    bool PlayParkGame()
    {
        DWORD pid = 0;
        if (g_playParked || g_playJoinMode || PlayBusy() || !PlayTrackedGameRunning(&pid)) return false;
        PlayReleaseGameControls(false);
        g_playParked = true;
        g_playParkCmdsSent = false;
        g_playParkMuted = false;
        g_playParkMuteNextMs = 0;
        PlayParkRememberFps();
        PlayParkTick(GetTickCount64());
        PlayParkHideTick();   // the pane stays, the game window inside it is hidden until Play or Close BO3
        PlayAppendLog("[PARK] Session ended, BO3 kept warm: hidden and muted with the level still loaded (AI off, players invulnerable). Play brings it back in seconds; Close BO3 closes it.");
        PlayNote("Session ended, BO3 kept warm: hidden and muted with the map still loaded. Play brings it back in seconds; Close BO3 closes it.");
        return true;
    }

    // Play on a parked game (worker or UI thread): sound back, the box settings back. The caller reloads or restarts the level.
    std::vector<std::string> PlayUnparkCommands(DWORD pid, const std::wstring& gameRoot)
    {
        g_playParked = false;
        if ((g_playParkMuted || g_playParkMuteNextMs) && PlaySetProcessMuted(pid, false) > 0) rp::ConfigSet("playtest", "park_muted", "0");
        g_playParkMuted = false;
        g_playParkMuteNextMs = 0;
        g_playParkCmdsSent = false;
        int fps = rp::ConfigGetInt("playtest", "park_maxfps", -1);
        if (fps < 0) fps = PlayConfigMaxFps(gameRoot);
        if (fps < 0 || fps == kPlayParkFps) fps = 0;   // unknown: no cap rather than the parked one
        PlayCheatsResync(false, true);
        return { std::string("ai_disableSpawn ") + (g_playDisableAi ? "1" : "0"),
                 "com_maxfps " + std::to_string(fps) };
    }

    void PlayCloseClicked(void*)
    {
        // End Session parks the game, a parked game (button "Close BO3") or a joined one is really closed.
        if (!g_playParked && PlayParkGame()) return;
        // Close BO3: sound back first (Windows may keep the app's mute for a BO3 started from Steam later)
        if (g_playParked && g_playParkMuted && PlaySetProcessMuted(g_playGamePid, false) > 0) rp::ConfigSet("playtest", "park_muted", "0");
        g_playParked = false;
        // "Close BO3" is stronger than Stop Test: no immediate auto-prewarm of the same map. Opening a different map or pressing Play clears this.
        if (!g_playCurrentMap.empty()) g_playPrewarmSuppressedMap = g_playCurrentMap;
        else if (!g_playGameMap.empty()) g_playPrewarmSuppressedMap = g_playGameMap;
        PlayStop(true);
        PlayJoinLeave();
    }

    // ---- Dvars window (the Mod Tools Launcher's list without ai_disableSpawn, connect, g_password and set_gametype) ----
    ModToolsWidget g_playDvarsButton = nullptr;
    ModToolsWidget g_playDvarDlg = nullptr, g_playDvarDev = nullptr, g_playDvarSplit = nullptr, g_playDvarLogfile = nullptr, g_playDvarCustom = nullptr;

    // QSpinBox and a fixed-size dialog layout from Qt's exports (the host API has neither). Allocated with Qt's own operator new
    // because the parent deletes it.
    struct PlayQtSpin
    {
        bool tried = false, ok = false;
        void* (*alloc)(size_t) = nullptr;
        void (*ctor)(void*, void*) = nullptr;
        void (*setRange)(void*, int, int) = nullptr;
        void (*setValue)(void*, int) = nullptr;
        int  (*value)(const void*) = nullptr;
        void* (*layout)(const void*) = nullptr;
        void (*setSizeConstraint)(void*, int) = nullptr;
    } g_playQtSpin;
    bool PlayQtSpinBind()
    {
        PlayQtSpin& q = g_playQtSpin;
        if (q.tried) return q.ok;
        q.tried = true;
        HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll"), crt = GetModuleHandleW(L"msvcr110.dll");
        if (!w || !crt) return false;
        auto get = [](HMODULE m, const char* name, auto& fn) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(m, name)); return fn != nullptr; };
        q.ok = get(crt, "??2@YAPEAX_K@Z", q.alloc) && get(w, "??0QSpinBox@@QEAA@PEAVQWidget@@@Z", q.ctor) &&
               get(w, "?setRange@QSpinBox@@QEAAXHH@Z", q.setRange) && get(w, "?setValue@QSpinBox@@QEAAXH@Z", q.setValue) &&
               get(w, "?value@QSpinBox@@QEBAHXZ", q.value) && get(w, "?layout@QWidget@@QEBAPEAVQLayout@@XZ", q.layout) &&
               get(w, "?setSizeConstraint@QLayout@@QEAAXW4SizeConstraint@1@@Z", q.setSizeConstraint);
        if (!q.ok) PlayAppendLog("[DVARS WARN] Qt spin box exports missing: the Dvars window is unavailable.");
        return q.ok;
    }
    ModToolsWidget PlayQtSpinBox(int minimum, int maximum, ModToolsWidget parent)
    {
        void* w = g_playQtSpin.alloc(128);   // the host's widget allocation size
        if (!w) return nullptr;
        memset(w, 0, 128);
        g_playQtSpin.ctor(w, parent);
        g_playQtSpin.setRange(w, minimum, maximum);
        return w;
    }

    void PlayDvarsLoad()
    {
        PlayDvars d;
        d.developer = (std::max)(1, (std::min)(2, rp::ConfigGetInt("playtest", "dvar_developer", 2)));
        d.splitscreen = rp::ConfigGetBool("playtest", "dvar_splitscreen", false);
        d.logfile = (std::max)(0, (std::min)(2, rp::ConfigGetInt("playtest", "dvar_logfile", 0)));
        d.custom = PlayTrim(rp::ConfigGet("playtest", "dvar_custom", ""));
        std::lock_guard<std::mutex> lock(g_playMutex);
        g_playDvars = d;
    }

    std::string PlayEditText(ModToolsWidget edit)
    {
        const auto* ui = rp::host->ui;
        std::string s(ui->EditText(edit, nullptr, 0), '\0');
        if (!s.empty()) ui->EditText(edit, &s[0], s.size() + 1);
        return s;
    }

    void PlayDvarsOk(void*)
    {
        const auto* ui = rp::host->ui;
        PlayDvars d;
        d.developer = (std::max)(1, (std::min)(2, g_playQtSpin.value(g_playDvarDev)));
        d.splitscreen = ui->IsChecked(g_playDvarSplit) != 0;
        d.logfile = (std::max)(0, (std::min)(2, g_playQtSpin.value(g_playDvarLogfile)));
        d.custom = PlayTrim(PlayEditText(g_playDvarCustom));
        rp::ConfigSet("playtest", "dvar_developer", std::to_string(d.developer));
        rp::ConfigSet("playtest", "dvar_splitscreen", d.splitscreen ? "1" : "0");
        rp::ConfigSet("playtest", "dvar_logfile", std::to_string(d.logfile));
        rp::ConfigSet("playtest", "dvar_custom", d.custom);
        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            g_playDvars = d;
        }
        PlayNote("Dvars saved: they apply at the next Play.");
        ui->SetVisible(g_playDvarDlg, 0);
    }

    void PlayDvarsCancel(void*) { if (g_playDvarDlg) rp::host->ui->SetVisible(g_playDvarDlg, 0); }

    void PlayDvarsClicked(void*)
    {
        const auto* ui = rp::host ? rp::host->ui : nullptr;
        if (!ui || !rp::Has(&rp::host->CreateDialogWindow) || !PlayQtSpinBind()) return;
        if (!g_playDvarDlg)
        {
            ModToolsWidget content = ui->Widget(nullptr);
            ModToolsWidget box = ui->VBox(content);
            ui->LayoutMargins(box, 10, 10, 10, 10);
            ModToolsWidget grid = ui->Grid();
            int row = 0;
            auto addRow = [&](const char* name, ModToolsWidget w, const char* tip)
            {
                ModToolsWidget label = ui->Label(name, content);
                ui->SetToolTip(label, tip);
                ui->SetToolTip(w, tip);
                ui->GridAddWidget(grid, label, row, 0);
                ui->GridAddWidget(grid, w, row, 1);
                ++row;
            };
            g_playDvarDev = PlayQtSpinBox(1, 2, content);
            addRow("developer", g_playDvarDev, "developer 1 or 2.");
            g_playDvarSplit = ui->CheckBox("", content);
            addRow("splitscreen", g_playDvarSplit, "Split screen with 2 players (splitscreen 1, splitscreen_playerCount 2). A change restarts BO3 on the next Play.");
            g_playDvarLogfile = PlayQtSpinBox(0, 2, content);
            addRow("logfile", g_playDvarLogfile, "Console log in the mod's folder: 0 off, 1 buffered, 2 every line.");
            g_playDvarCustom = ui->LineEdit("", content);
            ui->SetPlaceholder(g_playDvarCustom, "timescale 2; g_speed 300");
            addRow("Other dvars", g_playDvarCustom, "Any other dvars as name value, separated by semicolons.");
            ui->GridColumnStretch(grid, 1, 1);
            ui->BoxAddLayout(box, grid, 0);
            ModToolsWidget buttons = ui->HBox();
            ModToolsWidget ok = ui->PushButton("OK", content);
            ModToolsWidget cancel = ui->PushButton("Cancel", content);
            ui->BoxAddStretch(buttons, 1);
            ui->BoxAddWidget(buttons, ok, 0);
            ui->BoxAddWidget(buttons, cancel, 0);
            ui->BoxAddLayout(box, buttons, 0);
            g_playDvarDlg = rp::host->CreateDialogWindow(rp::self, "Play Test Dvars", content, 300, 160);
            if (!g_playDvarDlg) return;
            if (void* dl = g_playQtSpin.layout(g_playDvarDlg)) g_playQtSpin.setSizeConstraint(dl, 3);   // QLayout::SetFixedSize: the size hint, not resizable
            rp::host->OnClicked(rp::self, ok, &PlayDvarsOk, nullptr);
            rp::host->OnClicked(rp::self, cancel, &PlayDvarsCancel, nullptr);
        }
        const PlayDvars d = PlayDvarsCopy();
        g_playQtSpin.setValue(g_playDvarDev, d.developer);
        ui->SetChecked(g_playDvarSplit, d.splitscreen ? 1 : 0);
        g_playQtSpin.setValue(g_playDvarLogfile, d.logfile);
        ui->SetEditText(g_playDvarCustom, d.custom.c_str());
        ui->SetVisible(g_playDvarDlg, 1);
    }
    const char* PlaySessionStateName(LONG state);

    // The bridge's join module answers in BO3LiveJoin.status (one line): relayed into the play log when it changes.
    void PlayJoinFetchTick();   // with the Join Host block below
    void PlayHideIpTick(bool now);
    void PlayHostPlayingTick();
    void PlayJoinPollStatus()
    {
        static ULONGLONG last = 0;
        if (!g_playJoinMode || !g_playGamePid) return;
        const ULONGLONG now = GetTickCount64();
        if (now - last < 1000) return;
        last = now;
        FILE* f = _wfopen(PlayJoinFile(L"BO3LiveJoin.status").c_str(), L"r");
        if (!f) return;
        char line[600] = {};
        if (!fgets(line, sizeof line, f)) line[0] = 0;
        fclose(f);
        std::string text(line);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        size_t sp = text.find(' ');
        if (text.empty() || text == g_playJoinStatusSeen || sp == std::string::npos) return;
        if (g_playJoinSeq.empty() || text.compare(0, sp, g_playJoinSeq) != 0) return;   // an older request's answer
        g_playJoinStatusSeen = text;
        PlayAppendLog("[JOIN] " + text.substr(sp + 1));
        PlayNote("Join: " + text.substr(sp + 1));
    }

    void PlayRefreshUi()
    {
        const ModToolsUiApi* ui = rp::host ? rp::host->ui : nullptr;
        if (!ui) return;
        PlayHideIpTick(false);
        PlayJoinPollStatus();
        PlayJoinFetchTick();
        PlayHostPlayingTick();
        int state = g_playState.load();
        const char* primary = "Play";
        if (PlayBusy()) primary = "Cancel";
        else if (g_playHostPlaying) primary = "Join Host";
        else if (state == PLAY_FAILED) primary = "Try Again";
        else if (g_playParked) primary = "Play";
        else if (state == PLAY_RUNNING && g_playGamePresented) primary = "Update";
        else if (g_playGamePid && !g_playGamePresented) primary = "Play";
        if (g_playPrimaryButton && ui->SetButtonText) ui->SetButtonText(g_playPrimaryButton, primary);
        static int tipJoin = -1;
        const int wantTipJoin = g_playHostPlaying ? 1 : 0;
        if (g_playPrimaryButton && ui->SetToolTip && wantTipJoin != tipJoin)
        {
            tipJoin = wantTipJoin;
            ui->SetToolTip(g_playPrimaryButton, wantTipJoin ? kPlayTipJoin : kPlayTipPlay);
        }
        if (g_playFullCheck && ui->SetEnabled) ui->SetEnabled(g_playFullCheck, PlayBusy() ? 0 : 1);
        if (g_playWarmCheck && ui->SetEnabled) ui->SetEnabled(g_playWarmCheck, PlayBusy() ? 0 : 1);
        if (g_playPrewarmCheck && ui->SetEnabled) ui->SetEnabled(g_playPrewarmCheck, PlayBusy() ? 0 : 1);
        if (g_playEmbedCheck && ui->SetEnabled) ui->SetEnabled(g_playEmbedCheck, PlayBusy() ? 0 : 1);
        if (g_playNavCheck && ui->SetEnabled) ui->SetEnabled(g_playNavCheck, PlayBusy() ? 0 : 1);
        if (g_playAutoRestartCheck && ui->SetEnabled) ui->SetEnabled(g_playAutoRestartCheck, PlayBusy() ? 0 : 1);
        if (g_playInstantStartCheck && ui->SetEnabled) ui->SetEnabled(g_playInstantStartCheck, PlayBusy() ? 0 : 1);
        if (g_playFromCameraCheck && ui->SetEnabled) ui->SetEnabled(g_playFromCameraCheck, PlayBusy() ? 0 : 1);
        if (g_playFlyCheck && ui->SetEnabled) ui->SetEnabled(g_playFlyCheck, PlayBusy() ? 0 : 1);
        if (g_playHideNewPaneUntil)
        {
            if (GetTickCount64() > g_playHideNewPaneUntil) g_playHideNewPaneUntil = 0;
            else if (g_playPane && rp::host->PaneVisible && rp::host->PaneVisible(rp::self, g_playPane))
            {
                rp::host->ShowPane(rp::self, g_playPane, 0);
                if (g_playGamePane && !g_playGamePid) rp::host->ShowPane(rp::self, g_playGamePane, 0);
                g_playHideNewPaneUntil = 0;
            }
        }
        const bool enhanced = PlayLiveEditBuild();
        if (g_playLiveCheck && ui->SetEnabled) ui->SetEnabled(g_playLiveCheck, PlayBusy() ? 0 : 1);
        // Without BO3 Enhanced the box shows unchecked (a grey ticked box reads as "on but stuck") but stays clickable: a tick is undone at once and
        // explains that Enhanced is required, with the repo link. The saved preference comes back with Enhanced, the read-backs below skip the box meanwhile.
        static int liveShownEnhanced = -1; static bool liveBoxOpen = false;
        if (g_playLiveCheck && ui->SetChecked && (int)enhanced != liveShownEnhanced) { liveShownEnhanced = enhanced; ui->SetChecked(g_playLiveCheck, (enhanced && g_playLivePreview) ? 1 : 0); }
        if (g_playLiveCheck && !enhanced && !liveBoxOpen && ui->IsChecked && ui->SetChecked && ui->IsChecked(g_playLiveCheck))
        {
            ui->SetChecked(g_playLiveCheck, 0);   // before the modal box: its message pump runs this tick again
            liveBoxOpen = true; PlayShowEnhancedRequiredBox(); liveBoxOpen = false;
        }
        if (g_playEnhancedLabel && ui->SetVisible && (int)enhanced != g_playEnhancedState) { g_playEnhancedState = enhanced; ui->SetVisible(g_playEnhancedLabel, enhanced ? 0 : 1); }
        if (g_playCheckButton && ui->SetEnabled) ui->SetEnabled(g_playCheckButton, PlayBusy() ? 0 : 1);
        if (g_playCloseButton && ui->SetEnabled) ui->SetEnabled(g_playCloseButton, g_playGamePid ? 1 : 0);
        // "End Session" parks a running game, "Close BO3" quits (parked, joined, or a build running)
        static int closeEnds = -1;
        const int endsNow = (g_playGamePid && !g_playParked && !g_playJoinMode && !PlayBusy()) ? 1 : 0;
        if (g_playCloseButton && ui->SetButtonText && closeEnds != endsNow)
        {
            closeEnds = endsNow;
            ui->SetButtonText(g_playCloseButton, endsNow ? "End Session" : "Close BO3");
            if (ui->SetToolTip) ui->SetToolTip(g_playCloseButton, endsNow ? "End the test but keep BO3 warm: hidden and muted with the level loaded, the next Play is a reload."
                                                                          : "Close BlackOps3.exe.");
        }
        const bool engineCmdReady = !PlayBusy() && g_playGamePid && g_playLiveZoneInjected && !g_playJoinMode;   // a joined game runs no server: restarts are the host's
        if (g_playEngineLoadButton && ui->SetEnabled) ui->SetEnabled(g_playEngineLoadButton, engineCmdReady ? 1 : 0);
        if (g_playEngineReloadButton && ui->SetEnabled) ui->SetEnabled(g_playEngineReloadButton, engineCmdReady ? 1 : 0);
        // fast_restart only in a level: Enhanced proves it with the typed server state, Steam has no state reads.
        const bool typedState = g_playLiveShared && g_playLiveShared->serverRunningVerified && g_playLiveShared->mapnameVerified;
        const bool engineFastReady = engineCmdReady && (!typedState || g_playLiveShared->engineWorldActive);
        if (g_playEngineFastRestartButton && ui->SetEnabled) ui->SetEnabled(g_playEngineFastRestartButton, engineFastReady ? 1 : 0);
        if (g_playStatusLabel && ui->SetLabelText)
        {
            std::string s = "Status: " + (g_playVerboseLog ? PlayStatusText() : PlaySimpleStatusText());
            ui->SetLabelText(g_playStatusLabel, s.c_str());
        }
        if (g_playPreflightLabel && ui->SetLabelText)
        {
            std::string s = "Map setup: " + g_playPreflightSummary;
            ui->SetLabelText(g_playPreflightLabel, s.c_str());
        }
        if (g_playGameStatus && ui->SetLabelText)
        {
            std::string gs;
            if (g_playGamePid)
            {
                gs = PlayBuildModeShort(g_playExeBuild) + (g_playParked ? " | session ended, BO3 kept warm (hidden, muted, level loaded). Play brings it back in seconds, Close BO3 closes it."
                                                           : g_playGamePresented ? " | running. Click the game to play, Esc frees the mouse." : " | running hidden. Press Play to show it.");
            }
            else
                gs = PlayBuildModeShort(PlayCurrentExeBuild()) + " | not running";
            ui->SetLabelText(g_playGameStatus, gs.c_str());
        }
        if (g_playLogEdit && ui->PlainSetText)
        {
            std::string log;
            { std::lock_guard<std::mutex> lock(g_playMutex); log = g_playVerboseLog ? g_playLogText : g_playSimpleText; }
            if (log.size() != g_playLastUiLogSize)
            {
                g_playLastUiLogSize = log.size();
                ui->PlainSetText(g_playLogEdit, log.c_str());
                if (ui->PlainScrollToEnd) ui->PlainScrollToEnd(g_playLogEdit);
            }
        }
    }

    // Ctrl+C / Ctrl+A in the log: Qt does not claim ShortcutOverride for a read-only QPlainTextEdit, so Radiant's Edit > Copy took the key and copied brushes.
    // Our own event filter accepts the override while the log has focus (the host's filter API passes no event pointer: QObject with a copied vtable, slot 5 = eventFilter).
    // QEvent: type ushort at +16, m_accept is bit 2 of the bitfield ushort at +18.
    void* g_playLogFilter = nullptr;
    void* g_playLogFilterVt[11] = {};
    void (*g_playQtInstallFilter)(void*, void*) = nullptr;
    void (*g_playQtRemoveFilter)(void*, void*) = nullptr;
    bool PlayLogEventFilter(void* /*self*/, void* watched, void* ev)
    {
        if (!ev || !watched || watched != g_playLogEdit) return false;
        if (*reinterpret_cast<uint16_t*>(static_cast<char*>(ev) + 16) != 51 /* QEvent::ShortcutOverride */) return false;
        if (!(GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000) || (GetKeyState(VK_SHIFT) & 0x8000)) return false;
        if (!(GetKeyState('C') & 0x8000) && !(GetKeyState('A') & 0x8000) && !(GetKeyState(VK_INSERT) & 0x8000)) return false;
        *reinterpret_cast<uint8_t*>(static_cast<char*>(ev) + 18) |= 4;   // accept: no shortcut, deliver as KeyPress
        return true;
    }
    void PlayLogInstallCopyKeys()
    {
        if (!g_playLogEdit || g_playLogFilter) return;
        HMODULE core = GetModuleHandleW(L"Qt5Core64r.dll");
        if (!core) return;
        auto ctor = reinterpret_cast<void (*)(void*, void*)>(GetProcAddress(core, "??0QObject@@QEAA@PEAV0@@Z"));
        g_playQtInstallFilter = reinterpret_cast<void (*)(void*, void*)>(GetProcAddress(core, "?installEventFilter@QObject@@QEAAXPEAV1@@Z"));
        g_playQtRemoveFilter = reinterpret_cast<void (*)(void*, void*)>(GetProcAddress(core, "?removeEventFilter@QObject@@QEAAXPEAV1@@Z"));
        if (!ctor || !g_playQtInstallFilter || !g_playQtRemoveFilter) { PlayAppendLog("[UI WARN] Ctrl+C in the log is unavailable (Qt exports missing); right-click > Copy works."); return; }
        // Never freed and never parented: a parent's delete would call the dtor through a vtable in this DLL after a hot reload. Removed in PlayShutdown, leaks one small QObject per reload.
        void* fo = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 64);
        if (!fo) return;
        ctor(fo, nullptr);
        memcpy(g_playLogFilterVt, *reinterpret_cast<void***>(fo), sizeof g_playLogFilterVt);
        g_playLogFilterVt[5] = reinterpret_cast<void*>(&PlayLogEventFilter);
        *reinterpret_cast<void***>(fo) = g_playLogFilterVt;
        g_playLogFilter = fo;
        g_playQtInstallFilter(g_playLogEdit, fo);
    }
    void PlayLogRemoveCopyKeys()
    {
        if (g_playLogFilter && g_playLogEdit && g_playQtRemoveFilter) g_playQtRemoveFilter(g_playLogEdit, g_playLogFilter);
        g_playLogFilter = nullptr;
    }

    void PlayCreateUi()
    {
        if (!g_playEnabled || !rp::host || !rp::host->ui || !rp::host->CreatePane) return;
        const ModToolsUiApi* ui = rp::host->ui;
        g_playRoot = ui->Widget(nullptr);
        if (!g_playRoot) return;
        ModToolsWidget layout = ui->VBox(g_playRoot);
        if (ui->LayoutMargins) ui->LayoutMargins(layout, 8, 8, 8, 8);
        if (ui->LayoutSpacing) ui->LayoutSpacing(layout, 6);

        g_playPrimaryButton = ui->PushButton("Play", g_playRoot);
        if (ui->SetMinimumHeight) ui->SetMinimumHeight(g_playPrimaryButton, 42);
        if (ui->SetToolTip) ui->SetToolTip(g_playPrimaryButton, kPlayTipPlay);
        ui->BoxAddWidget(layout, g_playPrimaryButton, 0);

        // Shown only while BlackOps3.exe is not BO3 Enhanced (PlayRefreshUi).
        g_playEnhancedLabel = ui->Label(kPlayGetEnhanced, g_playRoot);
        if (ui->SetWordWrap) ui->SetWordWrap(g_playEnhancedLabel, 1);
        ui->BoxAddWidget(layout, g_playEnhancedLabel, 0);

        // The pane keeps four options, the rest (keep_warm, embed_game, smart_rebuild_nav, instant_start, auto_restart_gameover, prewarm_on_map_open) are PlayTest.ini only.
        ModToolsWidget options = ui->HBox();
        g_playFullCheck = ui->CheckBox("Full build", g_playRoot);
        if (ui->SetToolTip) ui->SetToolTip(g_playFullCheck, "Rebuild everything instead of only what changed.");
        // Spawn at camera is off and out of the pane for now: its code and the GSC receiver stay (the receiver also runs Fly and God).
        // g_playFromCameraCheck stays null, every use checks it.
        g_playFlyCheck = ui->CheckBox("Fly", g_playRoot);
        if (ui->SetToolTip) ui->SetToolTip(g_playFlyCheck, "Noclip. Applies to the running game at once.");
        if (ui->SetChecked) ui->SetChecked(g_playFlyCheck, g_playFly ? 1 : 0);
        g_playLiveCheck = ui->CheckBox("Live Edit", g_playRoot);
        if (ui->SetToolTip) ui->SetToolTip(g_playLiveCheck, "Show map edits in the running game. BO3 Enhanced only.");
        if (ui->SetChecked) ui->SetChecked(g_playLiveCheck, g_playLivePreview ? 1 : 0);
        g_playDisableAiCheck = ui->CheckBox("Disable AI", g_playRoot);
        if (ui->SetToolTip) ui->SetToolTip(g_playDisableAiCheck, "No AI spawns (ai_disableSpawn 1). Applies to the running game at once.");
        if (ui->SetChecked) ui->SetChecked(g_playDisableAiCheck, g_playDisableAi ? 1 : 0);
        g_playGodCheck = ui->CheckBox("God", g_playRoot);
        if (ui->SetToolTip) ui->SetToolTip(g_playGodCheck, "No damage to the player. Applies to the running game at once.");
        if (ui->SetChecked) ui->SetChecked(g_playGodCheck, g_playGod ? 1 : 0);
        ui->BoxAddWidget(options, g_playFullCheck, 0);
        ui->BoxAddWidget(options, g_playLiveCheck, 0);
        ui->BoxAddWidget(options, g_playDisableAiCheck, 0);
        ui->BoxAddWidget(options, g_playFlyCheck, 0);
        ui->BoxAddWidget(options, g_playGodCheck, 0);
        ui->BoxAddStretch(options, 1);
        ui->BoxAddLayout(layout, options, 0);

        ModToolsWidget actions = ui->HBox();
        g_playEngineReloadButton = ui->PushButton("Restart Map", g_playRoot);
        if (ui->SetToolTip) ui->SetToolTip(g_playEngineReloadButton, "map_restart in the running game.");
        g_playEngineFastRestartButton = ui->PushButton("Fast Restart", g_playRoot);
        if (ui->SetToolTip) ui->SetToolTip(g_playEngineFastRestartButton, "fast_restart in the running game: restarts the round, keeps the assets loaded.");
        g_playCloseButton = ui->PushButton("Close BO3", g_playRoot);
        g_playDvarsButton = ui->PushButton("Dvars...", g_playRoot);
        if (ui->SetToolTip) ui->SetToolTip(g_playDvarsButton, "Dvars passed to the game on Play (developer, split screen, log file, your own).");
        ui->BoxAddWidget(actions, g_playEngineReloadButton, 0);
        ui->BoxAddWidget(actions, g_playEngineFastRestartButton, 0);
        ui->BoxAddWidget(actions, g_playDvarsButton, 0);
        ui->BoxAddStretch(actions, 1);
        ui->BoxAddWidget(actions, g_playCloseButton, 0);
        ui->BoxAddLayout(layout, actions, 0);

        g_playPreflightLabel = ui->Label("Map setup: not checked yet", g_playRoot);
        g_playStatusLabel = ui->Label("Status: Idle", g_playRoot);
        if (ui->SetWordWrap) { ui->SetWordWrap(g_playPreflightLabel, 1); ui->SetWordWrap(g_playStatusLabel, 1); }
        ui->BoxAddWidget(layout, g_playPreflightLabel, 0);
        ui->BoxAddWidget(layout, g_playStatusLabel, 0);

        g_playLogEdit = ui->PlainTextEdit(g_playRoot);
        if (ui->SetReadOnly) ui->SetReadOnly(g_playLogEdit, 1);
        PlayLogInstallCopyKeys();
        if (ui->SetMinimumHeight) ui->SetMinimumHeight(g_playLogEdit, 150);
        ui->BoxAddWidget(layout, g_playLogEdit, 1);
        g_playVerboseCheck = ui->CheckBox("Verbose log", g_playRoot);
        if (ui->SetToolTip) ui->SetToolTip(g_playVerboseCheck, "Show every detail in the log: tool output, commands, engine state. Off: a short plain summary of what is happening.");
        if (ui->SetChecked) ui->SetChecked(g_playVerboseCheck, g_playVerboseLog ? 1 : 0);
        ui->BoxAddWidget(layout, g_playVerboseCheck, 0);
        ui->SetLayout(g_playRoot, layout);

        g_playPane = rp::host->CreatePane(rp::self, "Play Test", g_playRoot, 420, 360);
        // A pane Radiant's saved layout does not know yet starts hidden (no pop-up on the first start), the menu or the first Play opens it, then the layout decides.
        const bool newPane = g_playPane && !PlayLayoutKnowsPane(L"PlayTest");
        // The hide only works on a pane that is already on screen, so the tick repeats it for 3 s (PlayRefreshUi).
        if (newPane) { rp::host->ShowPane(rp::self, g_playPane, 0); g_playHideNewPaneUntil = GetTickCount64() + 3000; }

        // Dedicated BO3 surface: QWidget::winId() makes only this view native, BO3 is never parented to the Radiant Camera HWND.
        if (HMODULE widgets = GetModuleHandleW(L"Qt5Widgets64r.dll"))
            g_playQtWinId = reinterpret_cast<uintptr_t (*)(void*)>(GetProcAddress(widgets, "?winId@QWidget@@QEBA_KXZ"));
        if (HMODULE core = GetModuleHandleW(L"Qt5Core64r.dll"))
            if (auto setAttr = reinterpret_cast<void (*)(int, bool)>(GetProcAddress(core, "?setAttribute@QCoreApplication@@SAXW4ApplicationAttribute@Qt@@_N@Z")))
                setAttr(4 /* Qt::AA_DontCreateNativeWidgetSiblings */, true);

        g_playGameRoot = ui->Widget(nullptr);
        if (g_playGameRoot)
        {
            ModToolsWidget gameLayout = ui->VBox(g_playGameRoot);
            if (ui->LayoutMargins) ui->LayoutMargins(gameLayout, 4, 4, 4, 4);
            if (ui->LayoutSpacing) ui->LayoutSpacing(gameLayout, 4);
            g_playGameStatus = ui->Label("BO3 not running", g_playGameRoot);
            if (ui->SetWordWrap) ui->SetWordWrap(g_playGameStatus, 1);
            g_playGameView = ui->Widget(g_playGameRoot);
            if (ui->SetMinimumSize) ui->SetMinimumSize(g_playGameView, 640, 360);
            if (ui->SetToolTip) ui->SetToolTip(g_playGameView, "Click to play. Esc frees the cursor.");
            ui->BoxAddWidget(gameLayout, g_playGameStatus, 0);
            ui->BoxAddWidget(gameLayout, g_playGameView, 1);
            ui->SetLayout(g_playGameRoot, gameLayout);
            g_playGamePane = rp::host->CreatePane(rp::self, "BO3 Live", g_playGameRoot, 760, 500);
            if (g_playGamePane && !PlayLayoutKnowsPane(L"BO3Live")) rp::host->ShowPane(rp::self, g_playGamePane, 0);
        }
        g_playUiThreadId = GetCurrentThreadId();
        g_playPaneRectCache = RECT{};
        rp::host->OnClicked(rp::self, g_playPrimaryButton, &PlayPrimaryClicked, nullptr);
        rp::host->OnClicked(rp::self, g_playDisableAiCheck, &PlayDisableAiClicked, nullptr);
        rp::host->OnClicked(rp::self, g_playFlyCheck, &PlayFlyClicked, nullptr);
        rp::host->OnClicked(rp::self, g_playGodCheck, &PlayGodClicked, nullptr);
        rp::host->OnClicked(rp::self, g_playVerboseCheck, &PlayVerboseClicked, nullptr);
        rp::host->OnClicked(rp::self, g_playCloseButton, &PlayCloseClicked, nullptr);
        rp::host->OnClicked(rp::self, g_playDvarsButton, &PlayDvarsClicked, nullptr);
        rp::host->OnClicked(rp::self, g_playEngineReloadButton, &PlayEngineReloadClicked, nullptr);
        rp::host->OnClicked(rp::self, g_playEngineFastRestartButton, &PlayEngineFastRestartClicked, nullptr);

        // Same actions from a small menu, with no keyboard shortcut: F8 belongs to Radiant's Built Lighting command.
        if (rp::host->AddMenu && rp::host->AddMenuItem)
        {
            ModToolsWidget menu = rp::host->AddMenu(rp::self, "Play Test");
            if (menu)
            {
                if (rp::host->PaneToggleAction && rp::host->AddMenuAction)
                {
                    ModToolsWidget toggle = rp::host->PaneToggleAction(rp::self, g_playPane);
                    if (toggle) rp::host->AddMenuAction(rp::self, menu, toggle);
                    if (g_playGamePane)
                    {
                        ModToolsWidget gameToggle = rp::host->PaneToggleAction(rp::self, g_playGamePane);
                        if (gameToggle) rp::host->AddMenuAction(rp::self, menu, gameToggle);
                    }
                }
            }
        }
        PlayRefreshUi();
    }

    struct PlayLiveVec3 { float x, y, z; };
    struct PlayLivePlane { PlayLiveVec3 n; float d; };

    PlayLiveVec3 PlayLiveSub(const PlayLiveVec3& a, const PlayLiveVec3& b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
    PlayLiveVec3 PlayLiveAdd(const PlayLiveVec3& a, const PlayLiveVec3& b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
    PlayLiveVec3 PlayLiveMul(const PlayLiveVec3& a, float s) { return {a.x*s,a.y*s,a.z*s}; }
    float PlayLiveDot(const PlayLiveVec3& a, const PlayLiveVec3& b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
    PlayLiveVec3 PlayLiveCross(const PlayLiveVec3& a, const PlayLiveVec3& b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
    float PlayLiveLenSq(const PlayLiveVec3& a) { return PlayLiveDot(a,a); }

    struct PlayLiveGeometryStats
    {
        LONG geometryType = brlive::LIVE_GEOMETRY_NONE;
        LONG faceCount = 0;
        LONG planeCount = 0;
        LONG reconstructedVertexCount = 0;
        LONG uniqueEdgeCount = 0;
        LONG triangleCount = 0;
        LONG degenerateEdges = 0;
        bool reconstructionFailed = false;
        bool usedAabb = false;
    };

    // Textured Live Edit (BO3 Enhanced): a face's texdef from the brush's own .map text, and the face records sent with the object
    // (corners in winding order, uv per corner, material index into object.materials).
    struct PlayLiveFaceTex
    {
        PlayLiveVec3 pts[3] = {};
        std::string material;
        float hStretch = 64, vStretch = 64, hShift = 0, vShift = 0, rot = 0;
    };
    struct PlayLiveFace
    {
        LONG material = -1;
        std::vector<uint16_t> idx;
        std::vector<std::pair<float,float>> uv;
        uint16_t triFirst = 0, triCount = 0;
    };
    // A patch (mesh / curve brush) as Radiant's writer prints it: the control grid (rows = blocks, cols = points per block, RADIANT_FACTS
    // "Patch control grid"), one material, uv per point (printed as uv * 1024 after "t", writer 0x1407B9690).
    struct PlayPatchGrid
    {
        LONG rows = 0, cols = 0;
        bool curve = false;
        std::string material;
        std::vector<PlayLiveVec3> pts;              // rows * cols, row-major
        std::vector<std::pair<float,float>> uv;     // per point
    };

    struct PlayLiveLocalObject
    {
        uint64_t liveObjectId = 0;
        uint32_t generation = 0;
        LONG operation = brlive::LIVE_OBJECT_NONE;
        LONG geometryType = brlive::LIVE_GEOMETRY_NONE;
        LONG flags = brlive::LIVE_OBJECT_RENDER;
        std::string sourceGuid;
        std::vector<PlayLiveVec3> vertices;
        std::vector<brlive::Edge> edges;
        std::vector<brlive::Triangle> triangles;
        std::vector<PlayLiveFace> faces;          // textured path, empty for patches and the AABB fallback
        std::vector<std::string> materials;       // face.material indexes this
        PlayLiveGeometryStats stats;
        LONG patchRows = 0;
        LONG patchColumns = 0;
        LONG patchControlPointCount = 0;
        LONG patchWireEdgeCount = 0;
        LONG stalePasses = 0;
        bool fromPrefab = false;                  // built from the baked hull cache for a moved prefab (no Radiant brush behind it)
        std::string prefabBrushKey;               // fromPrefab: the brush's GUID key in its prefab file (selected inside the prefab)
        std::string ownerClass;                   // lower-case classname of the owning brush entity, empty for worldspawn
    };

    std::map<std::string,PlayLiveLocalObject> g_playLiveRegistry;
    void PlayPublishLiveHides();                 // live delete (below the registry code)
    void PlayClearLiveHides(const char* reason);
    void PlayLoadBakedHulls(const char* why);
    void PlayDropBakedHulls();
    // Edits inside an entered prefab: Radiant edits the shared instance in memory and writes nothing until Ctrl+S (RADIANT_FACTS "Prefab stack"). After each settled
    // edit the plugin captures the scope's content as the file's override text and re-reads the saved file with it seeded, so every instance (nested too) diffs its
    // compiled interior against what Radiant shows. Placements cannot change while inside. The override outlives leaving the prefab and goes with the map.
    std::map<std::string,std::string> g_playPrefabScopeText;   // normalized prefab key -> map text as Radiant holds it now
    std::map<std::string,uint64_t> g_playPrefabScopeHash;
    bool g_playPrefabUseOverrides = true;        // false while the COMPILED interiors are read (the BSP came from the files on disk)
    int g_playScopeDepth = 0;                    // PrefabDepth as reported by OnScopeChanged
    std::string g_playScopeKey;                  // normalized key of the entered prefab file ("" at the root)
    bool g_playScopeDirty = false; uint64_t g_playScopeDirtyAt = 0;
    size_t g_playScopeCaptures = 0;
    std::string PlayPrefabKeyNorm(const std::string& s);
    int PlayPrefabDepth();
    std::map<std::string,std::string> g_playLiveFills;   // hole fills (see PlayBakedHull): registry key -> "<hidden guid>|<neighbour guid>"
    LONG g_playFillsHideGeneration = -1;
    void PlayReconcileLiveHides(uint64_t now, bool force);
    uint64_t g_playLiveNextObjectId = 1;
    LONG g_playLiveRegistryGeneration = 0;
    LONG g_playLiveCreateCount = 0;
    LONG g_playLiveUpdateCount = 0;
    LONG g_playLiveDeleteCount = 0;
    LONG g_playLiveClearCount = 0;
    LONG g_playLiveReconstructionFailures = 0;
    LONG g_playLivePatchReconstructionFailures = 0;
    LONG g_playLiveLastOperation = brlive::LIVE_OBJECT_NONE;
    uint64_t g_playLiveLastOperationId = 0;
    LONG g_playLiveLastOperationGeneration = 0;

    uint64_t PlayAllocateLiveObjectId()
    {
        const uint64_t processPart=static_cast<uint64_t>(GetCurrentProcessId())<<32;
        return processPart|g_playLiveNextObjectId++;
    }

    bool PlayBuildBoundsProxy(const ModToolsSelectedBrush& b, std::vector<PlayLiveVec3>& verts, std::vector<brlive::Edge>& edges)
    {
        verts = {
            {b.mins[0],b.mins[1],b.mins[2]}, {b.maxs[0],b.mins[1],b.mins[2]},
            {b.maxs[0],b.maxs[1],b.mins[2]}, {b.mins[0],b.maxs[1],b.mins[2]},
            {b.mins[0],b.mins[1],b.maxs[2]}, {b.maxs[0],b.mins[1],b.maxs[2]},
            {b.maxs[0],b.maxs[1],b.maxs[2]}, {b.mins[0],b.maxs[1],b.maxs[2]}
        };
        const uint16_t pairs[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
        edges.clear(); for (auto& e : pairs) edges.push_back({e[0],e[1]});
        return true;
    }

    bool PlayTryFillSettledSelection(ModToolsSelectedBrush* out, size_t cap, size_t& count)
    {
        count=0;
        if (!rp::host || !rp::Has(&rp::host->GetSelection) || !rp::host->GetSelection) return false;
        __try
        {
            count=rp::host->GetSelection(rp::self,out,cap);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            count=0;
            return false;
        }
    }

    bool PlayTryFillSettledBrushInsts(ModToolsBrushInst* out, size_t cap, size_t& count)
    {
        count=0;
        if (!rp::host || !rp::Has(&rp::host->SelectedBrushInsts) || !rp::host->SelectedBrushInsts) return false;
        __try
        {
            count=rp::host->SelectedBrushInsts(rp::self,out,cap);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            count=0;
            return false;
        }
    }

    bool PlayTrySettledBrushGuid(ModToolsBrushInst inst, char out[40])
    {
        if (out) out[0]=0;
        if (!inst || !out || !rp::host ||
            !rp::Has(&rp::host->BrushOfInst) || !rp::host->BrushOfInst ||
            !rp::Has(&rp::host->BrushGuid) || !rp::host->BrushGuid)
            return false;
        __try
        {
            ModToolsBrush brush=rp::host->BrushOfInst(rp::self,inst);
            return brush && rp::host->BrushGuid(rp::self,brush,out)!=0 && out[0]!=0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out[0]=0;
            return false;
        }
    }

    // The brush's face lines from Radiant's own writer: "( x y z ) ( x y z ) ( x y z ) <material> <hStretch> <vStretch> <hShift> <vShift> <rot> <flags>
    // <lightmap> ..." (iwmap 4). Faces match BrushPlanes by plane points. The writer emits the whole entity, so only the block with this brush's
    // `guid "{...}"` line is read (else an identical, differently textured duplicate matches). An empty guid reads every face line.
    void PlayParseBrushFaceTexdefs(const std::string& text, const char* brushGuid, std::vector<PlayLiveFaceTex>& out)
    {
        size_t pos = 0, end = text.size();
        if (brushGuid && *brushGuid)
        {
            std::string needle = std::string("guid \"") + brushGuid + "\"";
            std::string lowerText = text, lowerNeedle = needle;
            std::transform(lowerText.begin(), lowerText.end(), lowerText.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            std::transform(lowerNeedle.begin(), lowerNeedle.end(), lowerNeedle.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            size_t g = lowerText.find(lowerNeedle);
            if (g == std::string::npos) return;
            pos = g;
            // the block ends at the first line that is just "}"
            size_t p = pos;
            while (p < text.size())
            {
                size_t eol = text.find('\n', p);
                std::string line = text.substr(p, eol == std::string::npos ? std::string::npos : eol - p);
                size_t b = line.find_first_not_of(" \t\r");
                if (b != std::string::npos && line[b] == '}') { end = p; break; }
                p = eol == std::string::npos ? text.size() : eol + 1;
            }
        }
        while (pos < end)
        {
            size_t eol = text.find('\n', pos);
            if (eol != std::string::npos && eol > end) eol = end;
            std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
            pos = eol == std::string::npos ? end : eol + 1;
            size_t b = line.find_first_not_of(" \t\r");
            if (b == std::string::npos || line[b] != '(') continue;
            PlayLiveFaceTex f;
            const char* c = line.c_str() + b;
            int used = 0;
            if (sscanf(c, " ( %f %f %f ) ( %f %f %f ) ( %f %f %f ) %n", &f.pts[0].x, &f.pts[0].y, &f.pts[0].z,
                       &f.pts[1].x, &f.pts[1].y, &f.pts[1].z, &f.pts[2].x, &f.pts[2].y, &f.pts[2].z, &used) < 9 || used <= 0)
                continue;
            char mat[128] = {};
            int used2 = 0;
            if (sscanf(c + used, "%127s %f %f %f %f %f%n", mat, &f.hStretch, &f.vStretch, &f.hShift, &f.vShift, &f.rot, &used2) < 6)
                continue;
            f.material = mat;
            out.push_back(f);
        }
    }

    // Texture coordinates the way Radiant and cod2map project a face (Q3 lineage): axes from the plane normal's dominant axis, rotated by <rot>
    // degrees, one repeat spans <stretch> world units, shifts in world units along the axes.
    void PlayFaceTextureAxes(const PlayLiveVec3& n, const PlayLiveFaceTex& t, PlayLiveVec3& u, PlayLiveVec3& v)
    {
        static const float baseaxis[18][3] = {
            {0,0,1},{1,0,0},{0,-1,0},   {0,0,-1},{1,0,0},{0,-1,0},
            {1,0,0},{0,1,0},{0,0,-1},   {-1,0,0},{0,1,0},{0,0,-1},
            {0,1,0},{1,0,0},{0,0,-1},   {0,-1,0},{1,0,0},{0,0,-1} };
        int best = 0; float bestDot = -1e30f;
        for (int i = 0; i < 6; ++i)
        {
            const float d = n.x*baseaxis[i*3][0] + n.y*baseaxis[i*3][1] + n.z*baseaxis[i*3][2];
            if (d > bestDot) { bestDot = d; best = i; }
        }
        float vecs[2][3];
        for (int j = 0; j < 3; ++j) { vecs[0][j] = baseaxis[best*3+1][j]; vecs[1][j] = baseaxis[best*3+2][j]; }
        if (t.rot != 0.0f)
        {
            const float ang = t.rot * 3.14159265358979f / 180.0f;
            const float sinv = std::sin(ang), cosv = std::cos(ang);
            const int sv = vecs[0][0] != 0.0f ? 0 : vecs[0][1] != 0.0f ? 1 : 2;
            const int tv = vecs[1][0] != 0.0f ? 0 : vecs[1][1] != 0.0f ? 1 : 2;
            for (int i = 0; i < 2; ++i)
            {
                const float ns = cosv*vecs[i][sv] - sinv*vecs[i][tv];
                const float nt = sinv*vecs[i][sv] + cosv*vecs[i][tv];
                vecs[i][sv] = ns; vecs[i][tv] = nt;
            }
        }
        u = { vecs[0][0], vecs[0][1], vecs[0][2] };
        v = { vecs[1][0], vecs[1][1], vecs[1][2] };
    }
    std::pair<float,float> PlayFaceUV(const PlayLiveVec3& p, const PlayLiveVec3& u, const PlayLiveVec3& v, const PlayLiveFaceTex& t)
    {
        const float hs = std::fabs(t.hStretch) < 1e-6f ? 64.0f : t.hStretch;
        const float vs = std::fabs(t.vStretch) < 1e-6f ? 64.0f : t.vStretch;
        return { (PlayLiveDot(p, u) + t.hShift) / hs, (PlayLiveDot(p, v) + t.vShift) / vs };
    }

    // The face texdefs of a settled brush through Radiant's writer (main thread, settled snapshot only, like the planes).
    char* PlayReadSettledBrushTextSeh(ModToolsBrushInst inst)   // no C++ objects here: SEH frame
    {
        __try
        {
            ModToolsBrush brush = rp::host->BrushOfInst(rp::self, inst);
            if (!brush) return nullptr;
            ModToolsEntity owner = rp::host->BrushOwner(rp::self, brush);
            if (!owner) return nullptr;
            return rp::host->WriteEntityTextWith(rp::self, owner, &brush, 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    void PlayFreeHostStringSeh(char* text)
    {
        __try { rp::host->FreeString(rp::self, text); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    bool PlayTryReadSettledBrushTexdefs(ModToolsBrushInst inst, const char* brushGuid, std::vector<PlayLiveFaceTex>& out)
    {
        out.clear();
        if (!inst || !rp::host || !rp::Has(&rp::host->BrushOfInst) || !rp::host->BrushOfInst ||
            !rp::Has(&rp::host->BrushOwner) || !rp::host->BrushOwner ||
            !rp::Has(&rp::host->WriteEntityTextWith) || !rp::host->WriteEntityTextWith ||
            !rp::Has(&rp::host->FreeString) || !rp::host->FreeString)
            return false;
        char* text = PlayReadSettledBrushTextSeh(inst);
        if (!text) return false;
        std::string copy(text);
        PlayFreeHostStringSeh(text);
        if (physprim::Contains(copy)) copy = physprim::Convert(copy);   // a physics box / cylinder as the faces the compile gets
        PlayParseBrushFaceTexdefs(copy, brushGuid, out);
        return !out.empty();
    }

    // The mesh / curve block of the brush with this GUID inside entity text: it starts at its guid line, then "mesh" or "curve", "{", optional
    // "toolFlags ...;", the material, the lightmap, optional "smoothing ...", the header "rows cols <f> <i>", then one parenthesized block of
    // "v x y z [c r g b a] t s t ..." lines per row. False when the brush is not a patch or the text is not what the writer prints.
    // Where each `guid "{...}"` line of .map text starts, by lower-case GUID (the first one wins, like a search would).
    void PlayIndexGuidLines(const std::string& text, std::unordered_map<std::string, size_t>& out)
    {
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t eol = text.find('\n', pos);
            if (eol == std::string::npos) eol = text.size();
            size_t b = pos;
            while (b < eol && (text[b] == ' ' || text[b] == '\t')) ++b;
            if (eol - b > 7 && text.compare(b, 6, "guid \"") == 0)
            {
                const size_t q = text.find('"', b + 6);
                if (q != std::string::npos && q < eol)
                {
                    std::string key = text.substr(b + 6, q - b - 6);
                    for (char& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    out.emplace(std::move(key), b);
                }
            }
            pos = eol + 1;
        }
    }
    bool PlayParsePatchAt(const std::string& text, size_t pos, PlayPatchGrid& out);
    bool PlayParsePatchText(const std::string& text, const char* brushGuid, PlayPatchGrid& out)
    {
        out = PlayPatchGrid();
        if (!brushGuid || !*brushGuid) return false;
        std::string needle = std::string("guid \"") + brushGuid + "\"";
        std::string lowerText = text, lowerNeedle = needle;
        std::transform(lowerText.begin(), lowerText.end(), lowerText.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        std::transform(lowerNeedle.begin(), lowerNeedle.end(), lowerNeedle.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        size_t pos = lowerText.find(lowerNeedle);
        if (pos == std::string::npos) return false;
        return PlayParsePatchAt(text, pos, out);
    }
    // The patch block whose guid line starts at pos.
    bool PlayParsePatchAt(const std::string& text, size_t pos, PlayPatchGrid& out)
    {
        out = PlayPatchGrid();
        int depth = 0; bool inPatch = false, header = false;
        std::string lightmap;
        while (pos < text.size())
        {
            size_t eol = text.find('\n', pos);
            std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
            pos = eol == std::string::npos ? text.size() : eol + 1;
            size_t b = line.find_first_not_of(" \t\r");
            if (b == std::string::npos) continue;
            line.erase(0, b);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            if (line.rfind("guid", 0) == 0) continue;
            if (!inPatch)
            {
                if (line == "mesh" || line == "curve") { inPatch = true; out.curve = line == "curve"; continue; }
                return false;   // a face line or the block's end: not a patch
            }
            if (line[0] == '{') { ++depth; continue; }
            if (line[0] == '}') break;
            if (line[0] == '(' || line[0] == ')') continue;
            if (line.rfind("toolFlags", 0) == 0 || line.rfind("smoothing", 0) == 0) continue;
            if (line[0] == 'v' && line.size() > 1 && (line[1] == ' ' || line[1] == '\t'))
            {
                PlayLiveVec3 v; float s0 = 0, t0 = 0;
                if (sscanf(line.c_str(), "v %f %f %f", &v.x, &v.y, &v.z) != 3) return false;
                const size_t tp = line.find(" t ");
                if (tp != std::string::npos) sscanf(line.c_str() + tp, " t %f %f", &s0, &t0);
                out.pts.push_back(v);
                out.uv.push_back({ s0 / 1024.0f, t0 / 1024.0f });
                continue;
            }
            if (!header)
            {
                if (out.material.empty()) { out.material = line; continue; }
                if (lightmap.empty()) { lightmap = line; continue; }
                int r = 0, c = 0;
                if (sscanf(line.c_str(), "%d %d", &r, &c) == 2 && r >= 2 && c >= 2 && r <= 1024 && c <= 1024) { out.rows = r; out.cols = c; header = true; continue; }
                return false;
            }
        }
        if (!header || out.pts.size() != static_cast<size_t>(out.rows) * static_cast<size_t>(out.cols)) return false;
        for (char& ch : out.material) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return true;
    }
    // The settled patch behind a selection instance (main thread): its grid through the writer, like the texdefs.
    bool PlayTryReadSettledPatch(ModToolsBrushInst inst, const char* brushGuid, PlayPatchGrid& out)
    {
        if (!inst || !brushGuid || !*brushGuid || !rp::host || !rp::Has(&rp::host->BrushOfInst) || !rp::host->BrushOfInst ||
            !rp::Has(&rp::host->BrushOwner) || !rp::host->BrushOwner ||
            !rp::Has(&rp::host->WriteEntityTextWith) || !rp::host->WriteEntityTextWith ||
            !rp::Has(&rp::host->FreeString) || !rp::host->FreeString)
            return false;
        char* text = PlayReadSettledBrushTextSeh(inst);
        if (!text) return false;
        std::string copy(text);
        PlayFreeHostStringSeh(text);
        return PlayParsePatchText(copy, brushGuid, out);
    }

    bool PlayTryReadSettledBrushPlanes(ModToolsBrushInst inst, float* out, size_t capFaces,
        size_t& faceCount, bool& brushResolved)
    {
        faceCount = 0;
        brushResolved = false;
        if (!inst || !out || !capFaces || !rp::host ||
            !rp::Has(&rp::host->BrushOfInst) || !rp::host->BrushOfInst ||
            !rp::Has(&rp::host->BrushPlanes) || !rp::host->BrushPlanes)
            return false;

        __try
        {
            ModToolsBrush brush = rp::host->BrushOfInst(rp::self, inst);
            if (!brush) return false;
            brushResolved = true;
            faceCount = rp::host->BrushPlanes(rp::self, brush, out, capFaces);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            faceCount = 0;
            brushResolved = false;
            return false;
        }
    }

    bool PlayBuildExactBrushProxy(const ModToolsSelectedBrush& sel, const float* raw, size_t nFaces,
        std::vector<PlayLiveVec3>& verts, std::vector<brlive::Edge>& edges,
        std::vector<brlive::Triangle>& triangles, PlayLiveGeometryStats& stats,
        const std::vector<PlayLiveFaceTex>* texdefs, std::vector<PlayLiveFace>* facesOut, std::vector<std::string>* materialsOut,
        std::vector<PlayLivePlane>* orientedOut=nullptr)
    {
        if (!raw || nFaces < 4) return false;

        std::vector<PlayLivePlane> planes;
        std::vector<size_t> planeRaw;   // raw face index of each kept plane (its texdef)
        planes.reserve(nFaces);
        for (size_t i=0;i<nFaces;++i)
        {
            PlayLiveVec3 a={raw[i*9+0],raw[i*9+1],raw[i*9+2]};
            PlayLiveVec3 b={raw[i*9+3],raw[i*9+4],raw[i*9+5]};
            PlayLiveVec3 c={raw[i*9+6],raw[i*9+7],raw[i*9+8]};
            if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(a.z) ||
                !std::isfinite(b.x) || !std::isfinite(b.y) || !std::isfinite(b.z) ||
                !std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z))
                return false;
            PlayLiveVec3 n=PlayLiveCross(PlayLiveSub(b,a),PlayLiveSub(c,a));
            float ls=PlayLiveLenSq(n); if (ls<1e-10f) continue;
            float inv=1.0f/std::sqrt(ls); n=PlayLiveMul(n,inv);
            float d=PlayLiveDot(n,a);
            bool duplicate=false;
            for (const auto& existing:planes)
            {
                const float alignment=PlayLiveDot(existing.n,n);
                if ((alignment>0.99999f && std::fabs(existing.d-d)<0.03125f) ||
                    (alignment<-0.99999f && std::fabs(existing.d+d)<0.03125f))
                {
                    duplicate=true;
                    break;
                }
            }
            if (duplicate) continue;
            planes.push_back({n,d});
            planeRaw.push_back(i);
        }
        stats.planeCount=static_cast<LONG>(planes.size());
        if (planes.size()<4) return false;

        // Radiant emits a consistent winding for all classic-brush plane triples, but the host contract does not say whether it denotes the <= or
        // >= half-space: try both global orientations. Do not orient planes around the AABB center (not guaranteed inside a skewed convex body).
        struct CandidateFace { size_t plane; std::vector<size_t> verts; uint16_t triFirst, triCount; };
        auto reconstruct=[&](const std::vector<PlayLivePlane>& oriented,
            std::vector<PlayLiveVec3>& candidateVerts,
            std::vector<brlive::Edge>& candidateEdges,
            std::vector<brlive::Triangle>& candidateTriangles,
            std::vector<CandidateFace>& candidateFaces,
            LONG& candidateDegenerate)->bool
        {
            candidateVerts.clear();
            candidateEdges.clear();
            candidateTriangles.clear();
            candidateFaces.clear();
            candidateDegenerate=0;
            const float insideEps=0.125f;
            const float vertexEps=0.03125f;
            const float dedupeSq=vertexEps*vertexEps;
            const float boundsEps=0.5f;
            for (size_t i=0;i<oriented.size();++i)
            for (size_t j=i+1;j<oriented.size();++j)
            for (size_t k=j+1;k<oriented.size();++k)
            {
                const auto& p1=oriented[i]; const auto& p2=oriented[j]; const auto& p3=oriented[k];
                PlayLiveVec3 c23=PlayLiveCross(p2.n,p3.n);
                float den=PlayLiveDot(p1.n,c23); if (std::fabs(den)<1e-5f) continue;
                PlayLiveVec3 x=PlayLiveMul(PlayLiveAdd(PlayLiveAdd(PlayLiveMul(c23,p1.d),PlayLiveMul(PlayLiveCross(p3.n,p1.n),p2.d)),PlayLiveMul(PlayLiveCross(p1.n,p2.n),p3.d)),1.0f/den);
                if (!std::isfinite(x.x) || !std::isfinite(x.y) || !std::isfinite(x.z)) continue;
                bool inside=true;
                for (const auto& p:oriented) if (PlayLiveDot(p.n,x)>p.d+insideEps){inside=false;break;}
                if (!inside) continue;
                if (x.x<sel.mins[0]-boundsEps || x.x>sel.maxs[0]+boundsEps ||
                    x.y<sel.mins[1]-boundsEps || x.y>sel.maxs[1]+boundsEps ||
                    x.z<sel.mins[2]-boundsEps || x.z>sel.maxs[2]+boundsEps)
                    continue;
                bool duplicate=false;
                for (const auto& v:candidateVerts)
                    if (PlayLiveLenSq(PlayLiveSub(v,x))<=dedupeSq){duplicate=true;break;}
                if (!duplicate)
                {
                    if (candidateVerts.size()>=brlive::kMaxVertices) return false;
                    candidateVerts.push_back(x);
                }
            }
            if (candidateVerts.size()<4) return false;

            std::set<uint32_t> uniqueEdges;
            std::map<uint32_t,LONG> edgeUseCount;
            LONG reconstructedFaces=0;
            const float onPlaneEps=0.15625f;
            const float edgeEpsSq=0.015625f*0.015625f;
            for (size_t planeIndex=0;planeIndex<oriented.size();++planeIndex)
            {
                const auto& plane=oriented[planeIndex];
                std::vector<size_t> faceVertices;
                for (size_t i=0;i<candidateVerts.size();++i)
                    if (std::fabs(PlayLiveDot(plane.n,candidateVerts[i])-plane.d)<=onPlaneEps)
                        faceVertices.push_back(i);
                if (faceVertices.size()<3) continue;
                ++reconstructedFaces;

                PlayLiveVec3 faceCenter={0,0,0};
                for (size_t index:faceVertices) faceCenter=PlayLiveAdd(faceCenter,candidateVerts[index]);
                faceCenter=PlayLiveMul(faceCenter,1.0f/static_cast<float>(faceVertices.size()));

                PlayLiveVec3 axisU={0,0,0};
                for (size_t index:faceVertices)
                {
                    axisU=PlayLiveSub(candidateVerts[index],faceCenter);
                    const float axisLenSq=PlayLiveLenSq(axisU);
                    if (axisLenSq>edgeEpsSq)
                    {
                        axisU=PlayLiveMul(axisU,1.0f/std::sqrt(axisLenSq));
                        break;
                    }
                }
                if (PlayLiveLenSq(axisU)<=edgeEpsSq) continue;
                PlayLiveVec3 axisV=PlayLiveCross(plane.n,axisU);

                std::sort(faceVertices.begin(),faceVertices.end(),[&](size_t lhs,size_t rhs)
                {
                    const PlayLiveVec3 a=PlayLiveSub(candidateVerts[lhs],faceCenter);
                    const PlayLiveVec3 b=PlayLiveSub(candidateVerts[rhs],faceCenter);
                    return std::atan2(PlayLiveDot(a,axisV),PlayLiveDot(a,axisU)) <
                           std::atan2(PlayLiveDot(b,axisV),PlayLiveDot(b,axisU));
                });

                // axisV = normal x axisU makes increasing atan2 order face the accepted half-space normal (the exterior normal for n.x<=d).
                // Fan triangles keep that winding, the dot check stays so numerical ordering can never invert a face.
                CandidateFace face{planeIndex,faceVertices,static_cast<uint16_t>(candidateTriangles.size()),0};
                {
                    // The face record keeps the same orientation as its triangles (exterior normal = plane.n).
                    PlayLiveVec3 area={0,0,0};
                    for (size_t i=1;i+1<faceVertices.size();++i)
                        area=PlayLiveAdd(area,PlayLiveCross(PlayLiveSub(candidateVerts[faceVertices[i]],candidateVerts[faceVertices[0]]),
                                                            PlayLiveSub(candidateVerts[faceVertices[i+1]],candidateVerts[faceVertices[0]])));
                    if (PlayLiveDot(area,plane.n)<0.0f) std::reverse(face.verts.begin(),face.verts.end());
                }
                for (size_t i=1;i+1<faceVertices.size();++i)
                {
                    uint16_t a=static_cast<uint16_t>(faceVertices[0]);
                    uint16_t b=static_cast<uint16_t>(faceVertices[i]);
                    uint16_t c=static_cast<uint16_t>(faceVertices[i+1]);
                    const PlayLiveVec3 ab=PlayLiveSub(candidateVerts[b],candidateVerts[a]);
                    const PlayLiveVec3 ac=PlayLiveSub(candidateVerts[c],candidateVerts[a]);
                    const PlayLiveVec3 cross=PlayLiveCross(ab,ac);
                    if (PlayLiveLenSq(cross)<=edgeEpsSq)
                    {
                        ++candidateDegenerate;
                        continue;
                    }
                    if (PlayLiveDot(cross,plane.n)<0.0f) (std::swap)(b,c);
                    if (candidateTriangles.size()>=brlive::kMaxTriangles) return false;
                    candidateTriangles.push_back({a,b,c});
                    ++face.triCount;
                }
                candidateFaces.push_back(face);

                for (size_t i=0;i<faceVertices.size();++i)
                {
                    const size_t a=faceVertices[i];
                    const size_t b=faceVertices[(i+1)%faceVertices.size()];
                    if (a==b || PlayLiveLenSq(PlayLiveSub(candidateVerts[a],candidateVerts[b]))<=edgeEpsSq)
                    {
                        ++candidateDegenerate;
                        continue;
                    }
                    const uint16_t lo=static_cast<uint16_t>((std::min)(a,b));
                    const uint16_t hi=static_cast<uint16_t>((std::max)(a,b));
                    const uint32_t key=(static_cast<uint32_t>(lo)<<16)|hi;
                    ++edgeUseCount[key];
                    if (!uniqueEdges.insert(key).second) continue;
                    if (candidateEdges.size()>=brlive::kMaxEdges) return false;
                    candidateEdges.push_back({lo,hi});
                }
            }
            if (reconstructedFaces<4 || candidateEdges.size()<6 || candidateTriangles.size()<4 ||
                candidateTriangles.size()!=2*candidateVerts.size()-4 ||
                static_cast<LONG>(candidateVerts.size())-static_cast<LONG>(candidateEdges.size())+reconstructedFaces!=2)
                return false;
            std::vector<LONG> vertexDegree(candidateVerts.size(),0);
            for (const auto& edge:candidateEdges)
            {
                ++vertexDegree[edge.a];
                ++vertexDegree[edge.b];
                const uint32_t key=(static_cast<uint32_t>((std::min)(edge.a,edge.b))<<16)|(std::max)(edge.a,edge.b);
                const auto use=edgeUseCount.find(key);
                if (use==edgeUseCount.end() || use->second!=2) return false;
            }
            for (LONG degree:vertexDegree) if (degree<3) return false;
            return true;
        };

        LONG maxCandidateVertices=0;
        LONG maxCandidateEdges=0;
        LONG totalDegenerate=0;
        for (int orientation=0;orientation<2;++orientation)
        {
            std::vector<PlayLivePlane> oriented=planes;
            if (orientation)
                for (auto& plane:oriented) { plane.n=PlayLiveMul(plane.n,-1.0f); plane.d=-plane.d; }
            std::vector<PlayLiveVec3> candidateVerts;
            std::vector<brlive::Edge> candidateEdges;
            std::vector<brlive::Triangle> candidateTriangles;
            std::vector<CandidateFace> candidateFaces;
            LONG candidateDegenerate=0;
            const bool valid=reconstruct(oriented,candidateVerts,candidateEdges,candidateTriangles,candidateFaces,candidateDegenerate);
            maxCandidateVertices=(std::max)(maxCandidateVertices,static_cast<LONG>(candidateVerts.size()));
            maxCandidateEdges=(std::max)(maxCandidateEdges,static_cast<LONG>(candidateEdges.size()));
            totalDegenerate+=candidateDegenerate;
            if (!valid) continue;
            if (orientedOut) *orientedOut=oriented;   // live delete: the hull planes, outward (n.x <= d inside)
            // Face records for the textured path: material and uv from the texdef whose plane points match this plane's
            // raw face (the writer rounds, so compare with a tolerance), a face without a texdef stays untextured.
            if (facesOut && materialsOut)
            {
                facesOut->clear(); materialsOut->clear();
                for (const CandidateFace& cf:candidateFaces)
                {
                    PlayLiveFace f;
                    const size_t rawIndex=planeRaw[cf.plane];
                    const PlayLiveFaceTex* tex=nullptr;
                    if (texdefs)
                    {
                        const PlayLiveVec3 ra={raw[rawIndex*9+0],raw[rawIndex*9+1],raw[rawIndex*9+2]};
                        const PlayLiveVec3 rb={raw[rawIndex*9+3],raw[rawIndex*9+4],raw[rawIndex*9+5]};
                        const PlayLiveVec3 rc={raw[rawIndex*9+6],raw[rawIndex*9+7],raw[rawIndex*9+8]};
                        for (const PlayLiveFaceTex& t:*texdefs)
                            if (PlayLiveLenSq(PlayLiveSub(t.pts[0],ra))<0.01f && PlayLiveLenSq(PlayLiveSub(t.pts[1],rb))<0.01f && PlayLiveLenSq(PlayLiveSub(t.pts[2],rc))<0.01f)
                            { tex=&t; break; }
                    }
                    if (tex && !tex->material.empty() && cf.verts.size()<=static_cast<size_t>(brlive::kMaxFaceVerts))
                    {
                        size_t mi=0;
                        for (;mi<materialsOut->size();++mi) if (_stricmp((*materialsOut)[mi].c_str(),tex->material.c_str())==0) break;
                        if (mi==materialsOut->size()) materialsOut->push_back(tex->material);
                        f.material=static_cast<LONG>(mi);
                        // the texture axes come from the raw (Radiant) plane normal, the same one the writer and cod2map see
                        const PlayLiveVec3 ra={raw[rawIndex*9+0],raw[rawIndex*9+1],raw[rawIndex*9+2]};
                        const PlayLiveVec3 rb={raw[rawIndex*9+3],raw[rawIndex*9+4],raw[rawIndex*9+5]};
                        const PlayLiveVec3 rc={raw[rawIndex*9+6],raw[rawIndex*9+7],raw[rawIndex*9+8]};
                        PlayLiveVec3 n=PlayLiveCross(PlayLiveSub(rb,ra),PlayLiveSub(rc,ra));
                        const float ls=PlayLiveLenSq(n); if (ls>1e-10f) n=PlayLiveMul(n,1.0f/std::sqrt(ls));
                        PlayLiveVec3 axisU,axisV; PlayFaceTextureAxes(n,*tex,axisU,axisV);
                        for (size_t vi:cf.verts)
                        {
                            f.idx.push_back(static_cast<uint16_t>(vi));
                            f.uv.push_back(PlayFaceUV(candidateVerts[vi],axisU,axisV,*tex));
                        }
                    }
                    else
                        for (size_t vi:cf.verts) { f.idx.push_back(static_cast<uint16_t>(vi)); f.uv.push_back({0.0f,0.0f}); }
                    f.triFirst=cf.triFirst; f.triCount=cf.triCount;
                    facesOut->push_back(f);
                }
            }
            verts.swap(candidateVerts);
            edges.swap(candidateEdges);
            triangles.swap(candidateTriangles);
            stats.reconstructedVertexCount=static_cast<LONG>(verts.size());
            stats.uniqueEdgeCount=static_cast<LONG>(edges.size());
            stats.triangleCount=static_cast<LONG>(triangles.size());
            stats.degenerateEdges=candidateDegenerate;
            return true;
        }
        stats.reconstructedVertexCount=maxCandidateVertices;
        stats.uniqueEdgeCount=maxCandidateEdges;
        stats.degenerateEdges=totalDegenerate;
        return false;
    }

    void PlayCloseLiveShared()
    {
        if (g_playLiveShared) { UnmapViewOfFile(g_playLiveShared); g_playLiveShared=nullptr; }
        if (g_playLiveMapHandle) { CloseHandle(g_playLiveMapHandle); g_playLiveMapHandle=nullptr; }
        g_playLiveBridgePid=0; g_playLiveBridgeInjected=false; g_playLiveZoneInjected=false; g_playLiveZoneArmed=false; g_playLiveZoneWarned=false; g_playLiveBridgeLoggedReady=false; g_playLiveDiagLogged=false; g_playLiveDiagWarned=false; g_playLiveLastFrameSeq=-1; g_playLiveLastDiagInputSeq=-1; g_playLiveLastDiagProjectionSeq=-1; g_playLiveBackgroundWarned=false; g_playLiveBridgeStatus.clear();
        g_playLiveSentDisabled=false; g_playLiveMoveEvents=0; g_playLiveSelectMoveEvents=0; g_playLiveSnapshotFailures=0; g_playLiveMoveLogged=false; g_playLiveSnapshotWarned=false;
        g_playEngineLoggedReady=false; g_playEngineLoggedDiscovery=false; g_playEngineLoggedCbuf=false; g_playEngineLoggedGameState=false; g_playEngineLoggedNative=false; g_playEngineLastTexturedStatus.clear(); g_playEngineLastTexturedResolved=-1; g_playEngineLastTexturedMissing=-1; g_playEngineLoggedTriangleGuardDiagnostics=false; g_playEngineLastState=-1; g_playEngineLastServerRunning=-999; g_playEngineLastWorldActive=-1; g_playEngineLastActiveMap.clear(); g_playEngineLastHeartbeat=-1; g_playEngineLastCommandAck=-1; g_playEngineUserCommandSeq=0; g_playEngineUserCommand=brlive::ENGINE_CMD_NONE; g_playEngineLastQueryMs=0; g_playEngineLastLifecycleOrder=-1; g_playEngineLoggedLifecycleTrace=false;
    }

    bool PlayEnsureLiveShared(DWORD pid)
    {
        if (!pid) return false;
        if (g_playLiveShared && g_playLiveBridgePid==pid) return true;
        PlayCloseLiveShared();
        wchar_t name[96]={}; brlive::MappingName(pid,name);
        g_playLiveMapHandle=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(brlive::SharedState),name);
        if (!g_playLiveMapHandle) return false;
        bool existed=GetLastError()==ERROR_ALREADY_EXISTS;
        g_playLiveShared=static_cast<brlive::SharedState*>(MapViewOfFile(g_playLiveMapHandle,FILE_MAP_ALL_ACCESS,0,0,sizeof(brlive::SharedState)));
        if (!g_playLiveShared){CloseHandle(g_playLiveMapHandle);g_playLiveMapHandle=nullptr;return false;}
        if (!existed || g_playLiveShared->magic!=brlive::kMagic || g_playLiveShared->version!=brlive::kVersion)
        {
            std::memset(g_playLiveShared,0,sizeof(*g_playLiveShared));
            g_playLiveShared->magic=brlive::kMagic; g_playLiveShared->version=brlive::kVersion; g_playLiveShared->gamePid=pid;
            g_playLiveShared->status=brlive::STATUS_STARTING;
            g_playLiveShared->engineState=brlive::ENGINE_BOOTING;
            g_playLiveShared->engineWorldActive=0;
            strncpy_s(g_playLiveShared->statusText,"Waiting for BO3LiveBridge.dll...",_TRUNCATE);
            strncpy_s(g_playLiveShared->engineStatusText,"Waiting for BO3 editor runtime companion...",_TRUNCATE);
        }
        InterlockedExchange(&g_playLiveShared->liveNavEnabled, g_playLiveNavMesh ? 1 : 0);
        InterlockedExchange(&g_playLiveShared->liveOccEnabled, g_playLiveOcclusion ? 1 : 0);
        InterlockedExchange(&g_playLiveShared->disableAi, g_playDisableAi ? 1 : 0);
        InterlockedExchange(&g_playLiveShared->launchHold, g_playLaunchHeld ? 1 : 0);   // before the bridge is injected
        g_playLiveBridgePid=pid;
        return true;
    }

    std::wstring PlayThisModuleDir()
    {
        HMODULE mod=nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&PlayThisModuleDir),&mod) || !mod) return {};
        wchar_t path[4096]={}; DWORD n=GetModuleFileNameW(mod,path,(DWORD)(sizeof(path)/sizeof(path[0])));
        if (!n || n>=sizeof(path)/sizeof(path[0])) return {};
        std::wstring s(path,n); size_t slash=s.find_last_of(L"\\/"); return slash==std::wstring::npos?std::wstring():s.substr(0,slash);
    }

    bool PlayRemoteHasModule(DWORD pid,const wchar_t* baseName)
    {
        for(int attempt=0;attempt<3;++attempt)
        {
            HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);
            if (snap==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_BAD_LENGTH){Sleep(20);continue;}return false;}
            MODULEENTRY32W me={}; me.dwSize=sizeof(me); bool found=false;
            if(Module32FirstW(snap,&me)) do { if(_wcsicmp(me.szModule,baseName)==0){found=true;break;} } while(Module32NextW(snap,&me));
            CloseHandle(snap); return found;
        }
        return false;
    }

    bool PlayInjectLiveBridge(DWORD pid,std::string& error)
    {
        if (PlayRemoteHasModule(pid,L"BO3LiveBridge.dll")) return true;
        std::wstring dir=PlayThisModuleDir();
        std::wstring dll=dir.empty()?L"":PlayJoin(dir,L"BO3LiveBridge.dll");
        if (dll.empty() || !PlayFileExists(dll)){error="BO3LiveBridge.dll was not found next to PlayTest.dll; rebuild or reinstall the PlayTest plugin";return false;}
        HANDLE proc=OpenProcess(PROCESS_CREATE_THREAD|PROCESS_QUERY_INFORMATION|PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ,FALSE,pid);
        if(!proc){error="OpenProcess failed ("+std::to_string(GetLastError())+")";return false;}
        SIZE_T bytes=(dll.size()+1)*sizeof(wchar_t); void* remote=VirtualAllocEx(proc,nullptr,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
        if(!remote){error="VirtualAllocEx failed";CloseHandle(proc);return false;}
        SIZE_T wrote=0; bool ok=WriteProcessMemory(proc,remote,dll.c_str(),bytes,&wrote)!=FALSE && wrote==bytes;
        auto load=reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW"));
        HANDLE th=(ok&&load)?CreateRemoteThread(proc,nullptr,0,load,remote,0,nullptr):nullptr;
        if(!th){error="CreateRemoteThread/LoadLibraryW failed ("+std::to_string(GetLastError())+")";VirtualFreeEx(proc,remote,0,MEM_RELEASE);CloseHandle(proc);return false;}
        DWORD wait=WaitForSingleObject(th,5000); DWORD code=0; GetExitCodeThread(th,&code);
        CloseHandle(th); VirtualFreeEx(proc,remote,0,MEM_RELEASE); CloseHandle(proc);
        if(wait!=WAIT_OBJECT_0 || code==0){error="BO3LiveBridge.dll did not report a successful LoadLibrary";return false;}
        for(int i=0;i<20;++i){if(PlayRemoteHasModule(pid,L"BO3LiveBridge.dll")) return true;Sleep(50);}
        error="bridge injection returned but module enumeration did not find BO3LiveBridge.dll"; return false;
    }

    // Live preview through the game's own script: every settled Live Edit object's edges go to the game as dvar strings ("x y z x y z|..." world
    // units, rounded) through the zone module mailbox. The receiver PlayConfigureLivePreview puts in the map GSC re-parses them when br_live_gen
    // changes and draws them with line() every server frame (no engine hook: /# #/ runs with scr_mod_enable_devblock 1).
    LONG g_playLiveZonePreviewGen = 0;
    void PlayLiveZonePublishPreview()
    {
        if (!g_playLiveZone || !g_playLiveZoneInjected) return;
        // Regular BO3 gets no in-game map changes at all (Live Edit is Enhanced only).
        if (!PlayLiveEditBuild()) return;
        // Enhanced draws Live Edit natively (solid faces + outlines): the script outlines stay off there, or every edit
        // would show twice.
        if (g_playLiveShared && (g_playLiveShared->engineReadyFlags & brlive::ENGINE_READY_NATIVE_WORLD_DRAW))
        {
            if (g_playLiveZonePreviewGen > 0) { g_playLiveZonePreviewGen = 0; PlayLiveZoneSendCommands({ "set br_live_n 0", "set br_live_gen 0", "set br_live_enable 0" }); }
            return;
        }
        const size_t kMaxDvars = 24, kMaxChars = 800;
        std::vector<std::string> cmds;
        std::string cur;
        size_t nDvars = 0, nEdges = 0, nObjects = 0;
        auto flush = [&]() { if (cur.empty()) return; cmds.push_back("set br_live_e" + std::to_string(nDvars++) + " \"" + cur + "\""); cur.clear(); };
        for (const auto& pair : g_playLiveRegistry)
        {
            const PlayLiveLocalObject& o = pair.second;
            if (o.edges.empty()) continue;
            ++nObjects;
            for (const brlive::Edge& e : o.edges)
            {
                if (e.a >= o.vertices.size() || e.b >= o.vertices.size()) continue;
                const PlayLiveVec3& a = o.vertices[e.a]; const PlayLiveVec3& b = o.vertices[e.b];
                char buf[96];
                snprintf(buf, sizeof buf, "%d %d %d %d %d %d", (int)lround(a.x), (int)lround(a.y), (int)lround(a.z), (int)lround(b.x), (int)lround(b.y), (int)lround(b.z));
                if (cur.size() + strlen(buf) + 1 > kMaxChars) flush();
                if (nDvars >= kMaxDvars) break;
                if (!cur.empty()) cur += '|';
                cur += buf; ++nEdges;
            }
            if (nDvars >= kMaxDvars) break;
        }
        if (nDvars < kMaxDvars) flush();
        cmds.push_back("set br_live_n " + std::to_string(nDvars));
        cmds.push_back("set br_live_gen " + std::to_string(++g_playLiveZonePreviewGen));
        cmds.push_back("set br_live_enable 1");
        if (PlayLiveZoneSendCommands(cmds))
            PlayAppendLog("[LIVE ZONE] preview " + std::to_string(g_playLiveZonePreviewGen) + ": " + std::to_string(nEdges) + " edge(s) of " + std::to_string(nObjects) + " object(s) in " + std::to_string(nDvars) + " dvar string(s) sent to the game.");
    }

    void PlayPublishLiveClear()
    {
        if(!g_playLiveShared) return;
        InterlockedIncrement(&g_playLiveShared->nativeGeometrySeq);
        MemoryBarrier();
        g_playLiveShared->vertexCount=0; g_playLiveShared->edgeCount=0; g_playLiveShared->brushGuid[0]=0;
        g_playLiveShared->geometryType=brlive::LIVE_GEOMETRY_NONE;
        g_playLiveShared->faceCount=0;
        g_playLiveShared->planeCount=0;
        g_playLiveShared->reconstructedVertexCount=0;
        g_playLiveShared->uniqueEdgeCount=0;
        g_playLiveShared->degenerateEdges=0;
        g_playLiveShared->fallbackToAabb=0;
        MemoryBarrier();
        InterlockedIncrement(&g_playLiveShared->nativeGeometrySeq);
    }

    std::string PlayLiveGuidKey(const char* guid)
    {
        std::string key=guid ? guid : "";
        std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return static_cast<char>(std::toupper(c));});
        return key;
    }

    void PlayWriteLegacySnapshot(const PlayLiveLocalObject& object)
    {
        if (!g_playLiveShared) return;
        const LONG vc=static_cast<LONG>((std::min)(object.vertices.size(),static_cast<size_t>(brlive::kMaxVertices)));
        const LONG ec=static_cast<LONG>((std::min)(object.edges.size(),static_cast<size_t>(brlive::kMaxEdges)));
        InterlockedIncrement(&g_playLiveShared->nativeGeometrySeq);
        MemoryBarrier();
        g_playLiveShared->vertexCount=vc;
        g_playLiveShared->edgeCount=ec;
        strncpy_s(g_playLiveShared->brushGuid,object.sourceGuid.c_str(),_TRUNCATE);
        g_playLiveShared->geometryType=object.geometryType;
        g_playLiveShared->faceCount=object.stats.faceCount;
        g_playLiveShared->planeCount=object.stats.planeCount;
        g_playLiveShared->reconstructedVertexCount=object.stats.reconstructedVertexCount;
        g_playLiveShared->uniqueEdgeCount=object.stats.uniqueEdgeCount;
        g_playLiveShared->degenerateEdges=object.stats.degenerateEdges;
        g_playLiveShared->reconstructionFailures=g_playLiveReconstructionFailures;
        g_playLiveShared->fallbackToAabb=object.stats.usedAabb ? 1 : 0;
        for (LONG i=0;i<vc;++i)
        {
            g_playLiveShared->world[i][0]=object.vertices[i].x;
            g_playLiveShared->world[i][1]=object.vertices[i].y;
            g_playLiveShared->world[i][2]=object.vertices[i].z;
        }
        for (LONG i=0;i<ec;++i) g_playLiveShared->edges[i]=object.edges[i];
        MemoryBarrier();
        InterlockedIncrement(&g_playLiveShared->nativeGeometrySeq);
    }

    // ---- Material visibility and collision: a Live Edit brush keeps what its materials do in the compiled game. A noDraw face (caulk, clip, trigger, hint,
    // volumes, sky, portal ...) is not drawn and the brush collides with the contents cod2map would give it. The material row comes from gdt.db through
    // Radiant's own sqlite64r.dll (indexed by _name), the gdt keys turn into contents with the linker's key table: start at CONTENTS_SOLID, clearSolid removes it, every set key ORs its bits. ----
    struct PlayMaterialProps { bool known=false; bool noDraw=false; bool caulk=false; uint32_t contents=1; };
    constexpr uint32_t kPlayContentsSolid=1u;
    struct PlayGdtSql
    {
        bool tried=false;
        void* db=nullptr;
        void* stmt=nullptr;
        int (*open_v2)(const char*,void**,int,const char*)=nullptr;
        int (*prepare_v2)(void*,const char*,int,void**,const char**)=nullptr;
        int (*bind_text)(void*,int,const char*,int,void(*)(void*))=nullptr;
        int (*step)(void*)=nullptr;
        int (*reset)(void*)=nullptr;
        const unsigned char* (*column_text)(void*,int)=nullptr;
        int (*finalize)(void*)=nullptr;
        int (*close)(void*)=nullptr;
    };
    PlayGdtSql g_playGdtSql;
    std::map<std::string,PlayMaterialProps> g_playMaterialProps;   // lower-case material name -> properties
    // the gdt keys the query reads, in column order (the linker's table: clearSolid, contents)
    struct PlayGdtContentsKey { const char* key; bool clearSolid; uint32_t contents; };
    const PlayGdtContentsKey kPlayGdtContentsKeys[]={
        {"missileClip",true,0x80u},{"bulletClip",true,0x2000u},{"playerClip",true,0x10000u},{"aiClip",true,0x20000u},
        {"vehicleClip",true,0x200u},{"itemClip",true,0x400u},{"canShootClip",false,0x40u},{"aiSightClip",true,0x1000u},
        {"utilityClip",true,0x400000u},{"playerVehicleClip",true,0x40000u},{"noDrop",true,0x80000000u},{"mount",false,0x1000000u},
        {"portal",true,0u},{"origin",true,0u},{"sky",true,0x800u},{"nonColliding",true,0x4u},{"nonSolid",true,0u},
    };
    constexpr int kPlayGdtContentsKeyCount=static_cast<int>(sizeof(kPlayGdtContentsKeys)/sizeof(kPlayGdtContentsKeys[0]));

    bool PlayGdtSqlReady()
    {
        PlayGdtSql& q=g_playGdtSql;
        if (q.tried) return q.stmt!=nullptr;
        q.tried=true;
        HMODULE dll=GetModuleHandleW(L"sqlite64r.dll");
        if (!dll) dll=LoadLibraryW(PlayJoin(PlayJoin(PlayToolsRoot(PlayGameRoot()),L"bin"),L"sqlite64r.dll").c_str());
        if (!dll) { PlayAppendLog("[MATERIALS] sqlite64r.dll not found: tool materials are recognised by name only"); return false; }
        q.open_v2=reinterpret_cast<decltype(q.open_v2)>(GetProcAddress(dll,"sqlite3_open_v2"));
        q.prepare_v2=reinterpret_cast<decltype(q.prepare_v2)>(GetProcAddress(dll,"sqlite3_prepare_v2"));
        q.bind_text=reinterpret_cast<decltype(q.bind_text)>(GetProcAddress(dll,"sqlite3_bind_text"));
        q.step=reinterpret_cast<decltype(q.step)>(GetProcAddress(dll,"sqlite3_step"));
        q.reset=reinterpret_cast<decltype(q.reset)>(GetProcAddress(dll,"sqlite3_reset"));
        q.column_text=reinterpret_cast<decltype(q.column_text)>(GetProcAddress(dll,"sqlite3_column_text"));
        q.finalize=reinterpret_cast<decltype(q.finalize)>(GetProcAddress(dll,"sqlite3_finalize"));
        q.close=reinterpret_cast<decltype(q.close)>(GetProcAddress(dll,"sqlite3_close"));
        if (!q.open_v2 || !q.prepare_v2 || !q.bind_text || !q.step || !q.reset || !q.column_text || !q.finalize || !q.close)
        { PlayAppendLog("[MATERIALS] sqlite64r.dll lacks the expected exports: tool materials are recognised by name only"); return false; }
        const std::string path=PlayWideToUtf8(PlayJoin(PlayJoin(PlayToolsRoot(PlayGameRoot()),L"gdtdb"),L"gdt.db"));
        if (q.open_v2(path.c_str(),&q.db,1 /* SQLITE_OPEN_READONLY */,nullptr)!=0 || !q.db)
        {
            if (q.db) { q.close(q.db); q.db=nullptr; }
            PlayAppendLog("[MATERIALS] gdt.db could not be opened ("+path+"): tool materials are recognised by name only");
            return false;
        }
        std::string sql="SELECT noDraw, caulk, surfaceType";
        for (const PlayGdtContentsKey& k:kPlayGdtContentsKeys) { sql+=", "; sql+=k.key; }
        sql+=" FROM material WHERE _name=?1 LIMIT 1";
        if (q.prepare_v2(q.db,sql.c_str(),-1,&q.stmt,nullptr)!=0 || !q.stmt)
        {
            q.stmt=nullptr; q.close(q.db); q.db=nullptr;
            PlayAppendLog("[MATERIALS] gdt.db material query failed: tool materials are recognised by name only");
            return false;
        }
        PlayAppendLog("[MATERIALS] material visibility and collision read from "+path);
        return true;
    }

    void PlayGdtSqlClose()
    {
        PlayGdtSql& q=g_playGdtSql;
        if (q.stmt) q.finalize(q.stmt);
        if (q.db) q.close(q.db);
        q.stmt=nullptr; q.db=nullptr; q.tried=false;
        g_playMaterialProps.clear();
    }

    // false when gdt.db has no row of that exact name
    bool PlayQueryMaterialProps(const std::string& name, PlayMaterialProps& out)
    {
        if (!PlayGdtSqlReady()) return false;
        PlayGdtSql& q=g_playGdtSql;
        q.reset(q.stmt);
        if (q.bind_text(q.stmt,1,name.c_str(),-1,reinterpret_cast<void(*)(void*)>(-1) /* SQLITE_TRANSIENT */)!=0) return false;
        if (q.step(q.stmt)!=100 /* SQLITE_ROW */) { q.reset(q.stmt); return false; }
        auto on=[&](int col){ const unsigned char* t=q.column_text(q.stmt,col); return t && t[0] && !(t[0]=='0' && !t[1]); };
        const unsigned char* st=q.column_text(q.stmt,2);
        const std::string surface=st ? PlayLowerAscii(reinterpret_cast<const char*>(st)) : std::string();
        out=PlayMaterialProps();
        out.known=true;
        out.caulk=on(1);
        bool clear=false; uint32_t bits=0;
        for (int i=0;i<kPlayGdtContentsKeyCount;++i)
            if (on(3+i)) { clear=clear || kPlayGdtContentsKeys[i].clearSolid; bits|=kPlayGdtContentsKeys[i].contents; }
        // surface types that change contents (the linker's surface table): glass 0x10, water 0x20, foliage 0x2
        if (surface=="glass" || surface=="glasscar" || surface=="glassbulletproof") { clear=true; bits|=0x10u; }
        else if (surface=="water" || surface=="watershallow") { clear=true; bits|=0x20u; }
        else if (surface=="foliage") { clear=true; bits|=0x2u; }
        out.contents=bits|(clear ? 0u : kPlayContentsSolid);
        // sky faces show the level's sky and portals nothing: neither shows its tool texture
        out.noDraw=on(0) || out.caulk || on(3+12) || on(3+14);   // keys 12 and 14: portal, sky
        q.reset(q.stmt);
        return true;
    }

    // by name when gdt.db cannot answer (no sqlite, no row): the stock tool families
    PlayMaterialProps PlayGuessMaterialProps(const std::string& lower)
    {
        PlayMaterialProps p;
        auto starts=[&](const char* pf){ return lower.rfind(pf,0)==0; };
        if (starts("caulk") || lower=="nodraw") { p.noDraw=true; p.caulk=starts("caulk"); return p; }
        if (starts("clip") || lower.find("_clip")!=std::string::npos) { p.noDraw=true; p.contents=0x10000u|0x20000u|0x200u; return p; }
        static const char* const none[]={"trigger","hint","skip","nodraw","portal","lightgrid","occluder","shadowcaster","sky","aitrig","mantle","mount","origin","region","physics"};
        for (const char* pf:none) if (starts(pf)) { p.noDraw=true; p.contents=0; return p; }
        if (lower.find("volume")!=std::string::npos) { p.noDraw=true; p.contents=0; }
        return p;
    }

    const PlayMaterialProps& PlayMaterialPropsFor(const std::string& name)
    {
        const std::string lower=PlayLowerAscii(name);
        auto it=g_playMaterialProps.find(lower);
        if (it!=g_playMaterialProps.end()) return it->second;
        PlayMaterialProps p;
        if (!PlayQueryMaterialProps(name,p) && (lower==name || !PlayQueryMaterialProps(lower,p))) p=PlayGuessMaterialProps(lower);
        char line[256];
        snprintf(line,sizeof line,"[MATERIALS] %s: %s, contents 0x%08x%s%s",name.c_str(),p.noDraw ? "not drawn" : "drawn",p.contents,
            p.caulk ? ", caulk" : "",p.known ? "" : " (guessed from the name)");
        PlayAppendLog(line);
        return g_playMaterialProps.emplace(lower,p).first->second;
    }

    // Brush entities that neither draw nor collide in the game whatever their materials: triggers and volumes (trigger_*,
    // info_volume, volume_*, umbra_volume, nav_volume*, lut_volume: the brush entity classes found in map_source besides
    // func_group, script_brushmodel and glass, which follow their materials like worldspawn).
    bool PlayOwnerClassInert(const std::string& cls)
    {
        return cls.rfind("trigger_",0)==0 || cls.find("volume")!=std::string::npos;
    }

    // lower-case classname of the entity that owns the brush of this selection instance, empty for worldspawn
    std::string PlayInstOwnerClass(ModToolsBrushInst inst)
    {
        if (!inst || !rp::Has(&rp::host->BrushOfInst) || !rp::host->BrushOfInst || !rp::Has(&rp::host->BrushOwner) || !rp::host->BrushOwner) return {};
        ModToolsBrush b=rp::host->BrushOfInst(rp::self,inst);
        ModToolsEntity owner=b ? rp::host->BrushOwner(rp::self,b) : nullptr;
        if (!owner) return {};
        if (rp::Has(&rp::host->WorldEntity) && rp::host->WorldEntity && owner==rp::host->WorldEntity(rp::self)) return {};
        if (!rp::Has(&rp::host->WriteEntityText) || !rp::host->WriteEntityText || !rp::Has(&rp::host->FreeString) || !rp::host->FreeString) return {};
        char* text=rp::host->WriteEntityText(rp::self,owner);
        if (!text) return {};
        std::vector<std::pair<std::string,std::string>> kvs; PlayParseEntityKvs(text,kvs); rp::host->FreeString(rp::self,text);
        for (const auto& kv:kvs) if (kv.first=="classname") return PlayLowerAscii(kv.second);
        return {};
    }

    void PlayPublishLiveRegistry()
    {
        PlayLiveZonePublishPreview();
        if (!g_playLiveShared) return;
        InterlockedIncrement(&g_playLiveShared->liveRegistrySeq);
        MemoryBarrier();
        std::memset(g_playLiveShared->liveObjects,0,sizeof(g_playLiveShared->liveObjects));
        std::memset(g_playLiveShared->materialNames,0,sizeof(g_playLiveShared->materialNames));
        std::vector<std::string> materialTable;   // shared across the objects of this publish
        LONG objectCount=0;
        LONG brushCount=0;
        LONG patchCount=0;
        LONG staleCount=0;
        LONG latestPatchRows=0;
        LONG latestPatchColumns=0;
        LONG latestPatchPoints=0;
        LONG latestPatchEdges=0;
        for (const auto& pair:g_playLiveRegistry)
        {
            if (objectCount>=brlive::kMaxLiveObjects) break;
            const PlayLiveLocalObject& source=pair.second;
            brlive::LiveObjectRecord& target=g_playLiveShared->liveObjects[objectCount++];
            target.liveObjectId=source.liveObjectId;
            target.generation=source.generation;
            target.operation=source.operation;
            target.type=source.geometryType;
            target.flags=source.flags|brlive::LIVE_OBJECT_ACTIVE|brlive::LIVE_OBJECT_RENDER;
            target.vertexCount=static_cast<LONG>(source.vertices.size());
            target.edgeCount=static_cast<LONG>(source.edges.size());
            target.triangleCount=static_cast<LONG>(source.triangles.size());
            target.patchRows=source.patchRows;
            target.patchColumns=source.patchColumns;
            target.patchControlPointCount=source.patchControlPointCount;
            target.patchWireEdgeCount=source.patchWireEdgeCount;
            for (int i=0;i<16;++i) target.transform[i]=(i%5)==0 ? 1.0f : 0.0f;
            strncpy_s(target.sourceGuid,source.sourceGuid.c_str(),_TRUNCATE);
            for (LONG i=0;i<target.vertexCount;++i)
            {
                target.world[i][0]=source.vertices[i].x;
                target.world[i][1]=source.vertices[i].y;
                target.world[i][2]=source.vertices[i].z;
            }
            for (LONG i=0;i<target.edgeCount;++i) target.edges[i]=source.edges[i];
            for (LONG i=0;i<target.triangleCount;++i) target.triangles[i]=source.triangles[i];
            target.faceCount=0;
            // material visibility and collision: noDraw faces carry no material (never linked, never drawn), the object's
            // contents are the OR of its faces' contents with caulk faces left out while another face exists (a clip
            // brush with caulk sides stays clip), an inert owner (trigger, volume) neither draws nor collides
            const bool inert=PlayOwnerClassInert(source.ownerClass);
            uint32_t contentsAll=0, contentsNoCaulk=0;
            bool anyFaceMaterial=false, anyDrawn=false, anyNonCaulk=false;
            for (const PlayLiveFace& f:source.faces)
            {
                if (target.faceCount>=brlive::kMaxFaces) break;
                if (f.idx.size()<3 || f.idx.size()>static_cast<size_t>(brlive::kMaxFaceVerts)) continue;
                brlive::LiveFace& lf=target.faces[target.faceCount++];
                lf.material=-1;
                lf.flags=inert ? brlive::FACE_NO_DRAW : 0;
                if (f.material>=0 && static_cast<size_t>(f.material)<source.materials.size())
                {
                    const std::string& name=source.materials[f.material];
                    const PlayMaterialProps& props=PlayMaterialPropsFor(name);
                    anyFaceMaterial=true;
                    contentsAll|=props.contents;
                    if (!props.caulk) { contentsNoCaulk|=props.contents; anyNonCaulk=true; }
                    if (props.noDraw) lf.flags|=brlive::FACE_NO_DRAW;
                    if (!(lf.flags&brlive::FACE_NO_DRAW))
                    {
                        anyDrawn=true;
                        size_t mi=0;
                        for (;mi<materialTable.size();++mi) if (_stricmp(materialTable[mi].c_str(),name.c_str())==0) break;
                        if (mi==materialTable.size() && materialTable.size()<static_cast<size_t>(brlive::kMaxMaterials) && name.size()<static_cast<size_t>(brlive::kMaterialNameLen))
                            materialTable.push_back(name);
                        if (mi<materialTable.size()) lf.material=static_cast<LONG>(mi);
                    }
                }
                else if (!inert) anyDrawn=true;   // no material: the colored fill
                lf.vertCount=static_cast<LONG>(f.idx.size());
                lf.triFirst=f.triFirst; lf.triCount=f.triCount;
                for (size_t k=0;k<f.idx.size();++k) { lf.idx[k]=f.idx[k]; lf.uv[k][0]=f.uv[k].first; lf.uv[k][1]=f.uv[k].second; }
            }
            target.collisionContents=0;
            if (inert || anyFaceMaterial)
            {
                target.flags|=brlive::LIVE_OBJECT_CONTENTS;
                target.collisionContents=inert ? 0u : (anyNonCaulk ? contentsNoCaulk : contentsAll);
            }
            if (inert || (target.faceCount>0 && !anyDrawn)) target.flags|=brlive::LIVE_OBJECT_NO_DRAW;
            {
                // one line per change of what the game gets for this object: who owns it, its contents, drawn or not
                static std::map<std::string,std::string> lastSaid;
                char line[256];
                snprintf(line,sizeof line,"owner %s%s, %s, %s",source.ownerClass.empty() ? "worldspawn" : source.ownerClass.c_str(),
                    inert ? " (a trigger or volume entity: the game never draws it or collides with it, whatever its material)" : "",
                    !(target.flags&brlive::LIVE_OBJECT_CONTENTS) ? "solid (no face material known)" : target.collisionContents ? "collides" : "no collision",
                    (target.flags&brlive::LIVE_OBJECT_NO_DRAW) ? "not drawn" : "drawn");
                char contents[32]; snprintf(contents,sizeof contents,", contents 0x%08x",target.collisionContents);
                const std::string said=std::string(line)+contents;
                std::string& last=lastSaid[source.sourceGuid];
                if (last!=said) { last=said; PlayAppendLog("[MATERIALS] "+source.sourceGuid+": "+said); }
            }
            if (source.geometryType==brlive::LIVE_GEOMETRY_BRUSH) ++brushCount;
            if (source.geometryType==brlive::LIVE_GEOMETRY_PATCH)
            {
                ++patchCount;
                latestPatchRows=source.patchRows;
                latestPatchColumns=source.patchColumns;
                latestPatchPoints=source.patchControlPointCount;
                latestPatchEdges=source.patchWireEdgeCount;
            }
            if (source.stalePasses>0) ++staleCount;
        }
        for (size_t mi=0;mi<materialTable.size();++mi) strncpy_s(g_playLiveShared->materialNames[mi],materialTable[mi].c_str(),_TRUNCATE);
        g_playLiveShared->materialCount=static_cast<LONG>(materialTable.size());
        g_playLiveShared->enabled=objectCount>0 ? 1 : 0;
        g_playLiveShared->liveRegistryGeneration=g_playLiveRegistryGeneration;
        g_playLiveShared->liveObjectCount=objectCount;
        g_playLiveShared->liveBrushCount=brushCount;
        g_playLiveShared->livePatchCount=patchCount;
        g_playLiveShared->liveCreateCount=g_playLiveCreateCount;
        g_playLiveShared->liveUpdateCount=g_playLiveUpdateCount;
        g_playLiveShared->liveDeleteCount=g_playLiveDeleteCount;
        g_playLiveShared->liveClearCount=g_playLiveClearCount;
        g_playLiveShared->staleObjectCount=staleCount;
        g_playLiveShared->lastRegistryOperation=g_playLiveLastOperation;
        g_playLiveShared->lastRegistryObjectId=g_playLiveLastOperationId;
        g_playLiveShared->lastRegistryObjectGeneration=g_playLiveLastOperationGeneration;
        g_playLiveShared->patchRows=latestPatchRows;
        g_playLiveShared->patchColumns=latestPatchColumns;
        g_playLiveShared->patchControlPointCount=latestPatchPoints;
        g_playLiveShared->patchWireEdgeCount=latestPatchEdges;
        g_playLiveShared->patchReconstructionFailures=g_playLivePatchReconstructionFailures;
        g_playLiveShared->patchFallbackToAabb=patchCount>0 && latestPatchPoints==0 ? 1 : 0;
        MemoryBarrier();
        InterlockedIncrement(&g_playLiveShared->liveRegistrySeq);
        InterlockedIncrement(&g_playLiveShared->inputSeq);
    }

    void PlayClearLiveRegistry(const char* reason)
    {
        const LONG removed=static_cast<LONG>(g_playLiveRegistry.size());
        g_playLiveRegistry.clear();
        g_playLiveFills.clear(); g_playFillsHideGeneration=-1;
        ++g_playLiveRegistryGeneration;
        ++g_playLiveClearCount;
        g_playLiveLastOperation=brlive::LIVE_OBJECT_CLEAR_ALL;
        g_playLiveLastOperationId=0;
        g_playLiveLastOperationGeneration=g_playLiveRegistryGeneration;
        PlayPublishLiveClear();
        PlayPublishLiveRegistry();
        PlayClearLiveHides(reason);
        if (reason && strncmp(reason,"successful BSP rebuild",22)==0) PlayLoadBakedHulls("rebuild");
        else if (reason && strncmp(reason,"Radiant map loaded",18)==0) PlayDropBakedHulls();
        if (reason && *reason)
        {
            char line[320]={};
            snprintf(line,sizeof line,"[LIVE REGISTRY] CLEAR_ALL reason=%s removed=%ld registryGeneration=%ld",
                reason,(long)removed,(long)g_playLiveRegistryGeneration);
            PlayAppendLog(line);
        }
    }

    // A misc_prefab entity carries a bounds brush of its own in Radiant (the box around the instance). It is not geometry: published as a live
    // object it became a room-sized solid around the player (the game died in the client's ground-surface lookup). A misc_model's bounds brush is
    // the same thing (the model itself is a live script_model). The owner's classname is read once per entity GUID.
    std::map<std::string,bool> g_playEntityIsPrefab;
    bool PlayEntityIsPrefab(const char* entityGuid)
    {
        if (!entityGuid || !entityGuid[0]) return false;
        const std::string key=PlayLiveGuidKey(entityGuid);
        auto cached=g_playEntityIsPrefab.find(key);
        if (cached!=g_playEntityIsPrefab.end()) return cached->second;
        bool prefab=false;
        if (rp::Has(&rp::host->FindEntitiesByGuid) && rp::host->FindEntitiesByGuid && rp::Has(&rp::host->WriteEntityText) && rp::host->WriteEntityText && rp::Has(&rp::host->FreeString) && rp::host->FreeString)
        {
            const char* g=entityGuid; ModToolsEntity e=nullptr;
            if (rp::host->FindEntitiesByGuid(rp::self,&g,1,&e) && e)
            {
                char* text=rp::host->WriteEntityText(rp::self,e);
                if (text)
                {
                    std::vector<std::pair<std::string,std::string>> kvs; PlayParseEntityKvs(text,kvs); rp::host->FreeString(rp::self,text);
                    for (const auto& kv:kvs) if (kv.first=="classname") { const std::string cls=PlayLowerAscii(kv.second); prefab=cls=="misc_prefab" || cls=="misc_model" || cls=="misc_volume_decal"; break; }
                }
                else return false;   // unreadable this pass: not cached
            }
            else return false;
        }
        g_playEntityIsPrefab[key]=prefab;
        if (prefab) PlayAppendLog(std::string("[LIVE REGISTRY] entity ")+entityGuid+" is a misc_prefab, misc_model or misc_volume_decal: its bounds brush is not published (the prefab rule, a live script_model or a live decal stands in)");
        return prefab;
    }

    // The same rule from the selection instance itself, without the GUID registry: right after an undo the restored entity was not findable by
    // GUID yet and a volume decal's bounds brush went out as a solid box. A fixed-size owner (point entity, model, prefab) carries only an invisible
    // stand-in brush, a decal, model or prefab owner by class. Worldspawn is never serialized for this.
    bool PlayInstIsStandIn(ModToolsBrushInst inst)
    {
        if (!inst || !rp::Has(&rp::host->BrushOfInst) || !rp::host->BrushOfInst || !rp::Has(&rp::host->BrushOwner) || !rp::host->BrushOwner) return false;
        ModToolsBrush b=rp::host->BrushOfInst(rp::self,inst);
        ModToolsEntity owner=b ? rp::host->BrushOwner(rp::self,b) : nullptr;
        if (!owner) return false;
        if (rp::Has(&rp::host->WorldEntity) && rp::host->WorldEntity && owner==rp::host->WorldEntity(rp::self)) return false;
        if (rp::Has(&rp::host->EntityFixedSize) && rp::host->EntityFixedSize && rp::host->EntityFixedSize(rp::self,owner)) return true;
        if (!rp::Has(&rp::host->WriteEntityText) || !rp::host->WriteEntityText || !rp::Has(&rp::host->FreeString) || !rp::host->FreeString) return false;
        char* text=rp::host->WriteEntityText(rp::self,owner);
        if (!text) return false;
        std::vector<std::pair<std::string,std::string>> kvs; PlayParseEntityKvs(text,kvs); rp::host->FreeString(rp::self,text);
        for (const auto& kv:kvs) if (kv.first=="classname") { const std::string cls=PlayLowerAscii(kv.second); return cls=="misc_prefab" || cls=="misc_model" || cls=="misc_volume_decal"; }
        return false;
    }

    bool PlayBuildSettledLiveObject(const ModToolsSelectedBrush& sel, ModToolsBrushInst inst,
        PlayLiveLocalObject& object, bool& exactOut)
    {
        if (!inst || !sel.brushGuid[0]) return false;
        if (PlayEntityIsPrefab(sel.entityGuid) || PlayInstIsStandIn(inst)) return false;

        // Called only by PlayTryPublishReleaseSnapshot after the mouse has been up for 300 ms. BrushInst/brush_t internals are transient during a
        // drag and must never be chased from the 30 Hz polling path.
        constexpr size_t kMaxSettledFaces=128;
        float rawPlanes[kMaxSettledFaces*9]={};
        size_t faceCount=0;
        bool brushResolved=false;
        const bool planeReadOk=PlayTryReadSettledBrushPlanes(inst,rawPlanes,kMaxSettledFaces,faceCount,brushResolved);

        std::vector<PlayLiveVec3> verts;
        std::vector<brlive::Edge> edges;
        std::vector<brlive::Triangle> triangles;
        std::vector<PlayLiveFace> faces;
        std::vector<std::string> materials;
        std::vector<PlayLiveFaceTex> texdefs;
        if (planeReadOk && brushResolved && faceCount>0) PlayTryReadSettledBrushTexdefs(inst,sel.brushGuid,texdefs);
        else if (planeReadOk && brushResolved && PlayTryReadSettledBrushTexdefs(inst,sel.brushGuid,texdefs))
        {
            // a physics box or cylinder: the host reports no faces for one (brush_t +232 is 1 or 2), the writer saves it as parameters, and physprim.h
            // turns those into the faces the compile wrapper gives cod2map
            faceCount=(std::min)(texdefs.size(),kMaxSettledFaces);
            for (size_t i=0;i<faceCount;++i)
                for (int k=0;k<3;++k) { rawPlanes[i*9+k*3]=texdefs[i].pts[k].x; rawPlanes[i*9+k*3+1]=texdefs[i].pts[k].y; rawPlanes[i*9+k*3+2]=texdefs[i].pts[k].z; }
        }
        PlayLiveGeometryStats stats;
        bool exact=false;
        if (planeReadOk && brushResolved && faceCount==0)
        {
            // The host contract reports zero faces for mesh/curve brushes: keep the patch behavior on the diagnostic AABB path.
            stats.geometryType=brlive::LIVE_GEOMETRY_PATCH;
            stats.usedAabb=true;
        }
        else if (planeReadOk && brushResolved)
        {
            stats.geometryType=brlive::LIVE_GEOMETRY_BRUSH;
            stats.faceCount=static_cast<LONG>((std::min)(faceCount,static_cast<size_t>(0x7FFFFFFF)));
            if (faceCount<=kMaxSettledFaces)
                exact=PlayBuildExactBrushProxy(sel,rawPlanes,faceCount,verts,edges,triangles,stats,texdefs.empty()?nullptr:&texdefs,&faces,&materials);
            if (!exact)
            {
                stats.reconstructionFailed=true;
                stats.usedAabb=true;
            }
        }
        else
        {
            stats.geometryType=brlive::LIVE_GEOMETRY_BRUSH;
            stats.reconstructionFailed=true;
            stats.usedAabb=true;
        }

        if (stats.usedAabb && !PlayBuildBoundsProxy(sel,verts,edges)) return false;
        if (verts.empty() || edges.empty()) return false;
        object.sourceGuid=sel.brushGuid;
        object.geometryType=stats.geometryType;
        object.vertices.swap(verts);
        object.edges.swap(edges);
        object.triangles.swap(triangles);
        if (exact)
        {
            std::string names; for (const std::string& m:materials) { if (!names.empty()) names+=", "; names+=m; }
            PlayAppendLog("[TEXTURED] brush "+std::string(sel.brushGuid)+": "+std::to_string(faces.size())+" face(s), "+std::to_string(texdefs.size())+
                          " face line(s) from the writer, materials: "+(names.empty() ? std::string("(none matched)") : names));
            object.faces.swap(faces); object.materials.swap(materials);
        }
        object.stats=stats;
        object.ownerClass=PlayInstOwnerClass(inst);
        object.flags=brlive::LIVE_OBJECT_ACTIVE|brlive::LIVE_OBJECT_SELECTED|brlive::LIVE_OBJECT_RENDER;
        if (stats.usedAabb) object.flags|=brlive::LIVE_OBJECT_AABB_FALLBACK;
        exactOut=exact;
        return true;
    }

    // ---- Patches: a mesh / curve brush is published as its control grid, tiled into objects of at most 9 x 9 points (64 quads = kMaxFaces) keyed "<guid>#<tile>".
    // Every quad is a textured face, corners counter-clockwise from the visible side (RADIANT_FACTS "Patch control grid": order a, d, c, b for a=[i][j], b=[i][j+1], c=[i+1][j+1], d=[i+1][j]),
    // two fill triangles per quad, grid lines as outline edges. Coincident points merge per tile and degenerate edges / triangles drop: the bridge rejects a whole snapshot over one zero-length edge. ----
    constexpr LONG kPlayPatchTileSide = 9;
    std::string PlayRegistryKeyGuid(const std::string& key)   // registry key -> brush GUID key (tiles carry "#k")
    {
        const size_t hash=key.find('#');
        return hash==std::string::npos ? key : key.substr(0,hash);
    }
    bool PlayRegistryKeyIsTile(const std::string& key) { return key.find('#')!=std::string::npos; }
    std::string PlayPatchTileKey(const std::string& guidKey, LONG tile) { return guidKey+"#"+std::to_string(tile); }

    // CURVES: a `curve` is a sheet of biquadratic Bezier pieces, not its control grid: rows and cols are odd, piece (i, j) owns [2i..2i+2][2j..2j+2] and shares its border points, so only corner points lie on the surface.
    // It is evaluated into an ordinary grid (positions and uv, same weights) that goes through the mesh path. Segments per piece and direction: the smallest of 2, 4, 8 that keeps chords within 0.25 units
    // (a quadratic piece leaves its chord by |p0 - 2 p1 + p2| / 4, a split into n divides that by n * n), halved while the sheet would pass 512 quads (the registry and collision prism pool are shared).
    bool PlayTessellateCurve(const PlayPatchGrid& g, PlayPatchGrid& out)
    {
        if (!g.curve || g.rows<3 || g.cols<3 || !(g.rows&1) || !(g.cols&1) ||
            g.pts.size()!=static_cast<size_t>(g.rows)*static_cast<size_t>(g.cols) || g.uv.size()!=g.pts.size()) return false;
        const LONG pr=(g.rows-1)/2, pc=(g.cols-1)/2;
        auto P=[&](LONG r,LONG c)->const PlayLiveVec3& { return g.pts[static_cast<size_t>(r*g.cols+c)]; };
        auto bend=[](const PlayLiveVec3& a,const PlayLiveVec3& b,const PlayLiveVec3& c)
        { return PlayLiveLenSq(PlayLiveSub(PlayLiveAdd(a,c),PlayLiveMul(b,2.0f))); };
        float bendR=0, bendC=0;   // squared second differences along the rows' direction and along the cols' direction
        for (LONG i=0;i<pr;++i) for (LONG c=0;c<g.cols;++c) bendR=(std::max)(bendR,bend(P(2*i,c),P(2*i+1,c),P(2*i+2,c)));
        for (LONG j=0;j<pc;++j) for (LONG r=0;r<g.rows;++r) bendC=(std::max)(bendC,bend(P(r,2*j),P(r,2*j+1),P(r,2*j+2)));
        auto segments=[](float bendSq)
        {
            const float off=sqrtf(bendSq)*0.25f;
            LONG n=2; while (n<8 && off/static_cast<float>(n*n)>0.25f) n*=2;
            return n;
        };
        LONG nr=segments(bendR), nc=segments(bendC);
        while (pr*nr*pc*nc>512 && (nr>1 || nc>1)) { if (pr*nr>=pc*nc && nr>1) nr/=2; else if (nc>1) nc/=2; else nr/=2; }
        out=PlayPatchGrid();
        out.curve=false; out.material=g.material;
        out.rows=pr*nr+1; out.cols=pc*nc+1;
        out.pts.resize(static_cast<size_t>(out.rows)*static_cast<size_t>(out.cols)); out.uv.resize(out.pts.size());
        for (LONG R=0;R<out.rows;++R)
        {
            const LONG i=(std::min)(R/nr,pr-1); const float t=static_cast<float>(R-i*nr)/static_cast<float>(nr);
            const float wr[3]={(1-t)*(1-t),2*t*(1-t),t*t};
            for (LONG C=0;C<out.cols;++C)
            {
                const LONG j=(std::min)(C/nc,pc-1); const float u=static_cast<float>(C-j*nc)/static_cast<float>(nc);
                const float wc[3]={(1-u)*(1-u),2*u*(1-u),u*u};
                PlayLiveVec3 p={0,0,0}; float s0=0, t0=0;
                for (int a=0;a<3;++a)
                    for (int b=0;b<3;++b)
                    {
                        const float w=wr[a]*wc[b]; const size_t src=static_cast<size_t>((2*i+a)*g.cols+(2*j+b));
                        p=PlayLiveAdd(p,PlayLiveMul(g.pts[src],w)); s0+=g.uv[src].first*w; t0+=g.uv[src].second*w;
                    }
                const size_t dst=static_cast<size_t>(R*out.cols+C);
                out.pts[dst]=p; out.uv[dst]={s0,t0};
            }
        }
        return true;
    }

    void PlayBuildPatchLiveObjects(const ModToolsSelectedBrush& sel, const PlayPatchGrid& g, std::vector<PlayLiveLocalObject>& out)
    {
        out.clear();
        if (g.curve) { PlayPatchGrid flat; if (PlayTessellateCurve(g,flat)) { PlayBuildPatchLiveObjects(sel,flat,out); return; } }
        if (g.rows<2 || g.cols<2 || g.pts.size()!=static_cast<size_t>(g.rows)*static_cast<size_t>(g.cols)) return;
        const LONG tilesR=(g.rows-2)/(kPlayPatchTileSide-1)+1, tilesC=(g.cols-2)/(kPlayPatchTileSide-1)+1;
        for (LONG tr=0;tr<tilesR;++tr)
            for (LONG tc=0;tc<tilesC;++tc)
            {
                const LONG r0=tr*(kPlayPatchTileSide-1), c0=tc*(kPlayPatchTileSide-1);
                const LONG r1=(std::min)(g.rows-1,r0+kPlayPatchTileSide-1), c1=(std::min)(g.cols-1,c0+kPlayPatchTileSide-1);
                if (r1<=r0 || c1<=c0) continue;
                PlayLiveLocalObject obj;
                std::vector<uint16_t> local((r1-r0+1)*(c1-c0+1),0);
                auto localIndex=[&](LONG r,LONG c){ return static_cast<size_t>((r-r0)*(c1-c0+1)+(c-c0)); };
                for (LONG r=r0;r<=r1;++r)
                    for (LONG c=c0;c<=c1;++c)
                    {
                        const size_t src=static_cast<size_t>(r*g.cols+c);
                        const PlayLiveVec3& p=g.pts[src];
                        size_t found=obj.vertices.size();
                        for (size_t k=0;k<obj.vertices.size();++k)
                            if (PlayLiveLenSq(PlayLiveSub(obj.vertices[k],p))<1e-6f) { found=k; break; }
                        if (found==obj.vertices.size()) obj.vertices.push_back(p);
                        local[localIndex(r,c)]=static_cast<uint16_t>(found);
                    }
                std::set<std::pair<uint16_t,uint16_t>> edgeSet;
                auto addEdge=[&](uint16_t a,uint16_t b)
                {
                    if (a==b) return;
                    const auto e=std::make_pair((std::min)(a,b),(std::max)(a,b));
                    if (edgeSet.insert(e).second) obj.edges.push_back(brlive::Edge{e.first,e.second});
                };
                auto triArea2=[&](uint16_t a,uint16_t b,uint16_t c)
                {
                    const PlayLiveVec3 n=PlayLiveCross(PlayLiveSub(obj.vertices[b],obj.vertices[a]),PlayLiveSub(obj.vertices[c],obj.vertices[a]));
                    return PlayLiveLenSq(n);
                };
                obj.materials.push_back(g.material);
                for (LONG r=r0;r<r1;++r)
                    for (LONG c=c0;c<c1;++c)
                    {
                        const uint16_t a=local[localIndex(r,c)], b=local[localIndex(r,c+1)], cc=local[localIndex(r+1,c+1)], d=local[localIndex(r+1,c)];
                        const uint16_t order[4]={a,d,cc,b};   // counter-clockwise from the visible side
                        std::vector<uint16_t> corners; std::vector<std::pair<float,float>> uvs;
                        const size_t srcIdx[4]={static_cast<size_t>(r*g.cols+c),static_cast<size_t>((r+1)*g.cols+c),static_cast<size_t>((r+1)*g.cols+c+1),static_cast<size_t>(r*g.cols+c+1)};
                        for (int k=0;k<4;++k)
                        {
                            bool dup=false; for (uint16_t q:corners) if (q==order[k]) dup=true;
                            if (dup) continue;
                            corners.push_back(order[k]); uvs.push_back(g.uv[srcIdx[k]]);
                        }
                        if (corners.size()<3) continue;
                        PlayLiveFace f; f.material=0; f.idx=corners; f.uv=uvs;
                        f.triFirst=static_cast<uint16_t>(obj.triangles.size()); f.triCount=0;
                        for (size_t k=2;k<corners.size();++k)
                            if (triArea2(corners[0],corners[k-1],corners[k])>1e-6f)
                            { obj.triangles.push_back(brlive::Triangle{corners[0],corners[k-1],corners[k]}); ++f.triCount; }
                        if (!f.triCount) continue;
                        obj.faces.push_back(f);
                        addEdge(a,b); addEdge(b,cc); addEdge(cc,d); addEdge(d,a);
                    }
                if (obj.vertices.empty() || obj.edges.empty() || obj.faces.empty()) continue;
                if (obj.vertices.size()>static_cast<size_t>(brlive::kMaxVertices) || obj.edges.size()>static_cast<size_t>(brlive::kMaxEdges) ||
                    obj.triangles.size()>static_cast<size_t>(brlive::kMaxTriangles) || obj.faces.size()>static_cast<size_t>(brlive::kMaxFaces)) continue;
                obj.sourceGuid=sel.brushGuid;
                obj.geometryType=brlive::LIVE_GEOMETRY_PATCH;
                obj.stats.geometryType=brlive::LIVE_GEOMETRY_PATCH;
                obj.stats.faceCount=static_cast<LONG>(obj.faces.size());
                obj.patchRows=r1-r0+1; obj.patchColumns=c1-c0+1;
                obj.patchControlPointCount=static_cast<LONG>(obj.vertices.size());
                obj.patchWireEdgeCount=static_cast<LONG>(obj.edges.size());
                obj.flags=brlive::LIVE_OBJECT_ACTIVE|brlive::LIVE_OBJECT_SELECTED|brlive::LIVE_OBJECT_RENDER;
                out.push_back(std::move(obj));
            }
    }

    // Every registry tile of this patch leaves (returns how many).
    size_t PlayEraseRegistryTiles(const std::string& guidKey)
    {
        size_t n=0;
        const std::string prefix=guidKey+"#";
        for (auto it=g_playLiveRegistry.lower_bound(prefix); it!=g_playLiveRegistry.end() && it->first.compare(0,prefix.size(),prefix)==0;)
        {
            g_playLiveLastOperation=brlive::LIVE_OBJECT_DELETE;
            g_playLiveLastOperationId=it->second.liveObjectId;
            g_playLiveLastOperationGeneration=static_cast<LONG>(it->second.generation+1);
            ++g_playLiveDeleteCount;
            it=g_playLiveRegistry.erase(it); ++n;
        }
        return n;
    }

    bool PlayUpsertSettledPatch(const ModToolsSelectedBrush& sel, const PlayPatchGrid& grid)
    {
        std::vector<PlayLiveLocalObject> tiles;
        PlayBuildPatchLiveObjects(sel,grid,tiles);
        const std::string guidKey=PlayLiveGuidKey(sel.brushGuid);
        if (tiles.empty()) { PlayEraseRegistryTiles(guidKey); return false; }
        LONG created=0, updated=0;
        for (size_t t=0;t<tiles.size();++t)
        {
            const std::string key=PlayPatchTileKey(guidKey,static_cast<LONG>(t));
            auto found=g_playLiveRegistry.find(key);
            if (found==g_playLiveRegistry.end())
            {
                if (g_playLiveRegistry.size()>=brlive::kMaxLiveObjects) { PlayAppendLog("[LIVE REGISTRY WARN] Registry capacity reached; a patch tile was not published."); break; }
                tiles[t].liveObjectId=PlayAllocateLiveObjectId(); tiles[t].generation=1; tiles[t].operation=brlive::LIVE_OBJECT_CREATE;
                found=g_playLiveRegistry.emplace(key,std::move(tiles[t])).first; ++g_playLiveCreateCount; ++created;
            }
            else
            {
                tiles[t].liveObjectId=found->second.liveObjectId; tiles[t].generation=found->second.generation+1; tiles[t].operation=brlive::LIVE_OBJECT_UPDATE;
                found->second=std::move(tiles[t]); ++g_playLiveUpdateCount; ++updated;
            }
            g_playLiveLastOperation=found->second.operation;
            g_playLiveLastOperationId=found->second.liveObjectId;
            g_playLiveLastOperationGeneration=static_cast<LONG>(found->second.generation);
        }
        // tiles beyond the current count (the grid shrank) leave
        size_t stale=0;
        const std::string prefix=guidKey+"#";
        for (auto it=g_playLiveRegistry.lower_bound(prefix); it!=g_playLiveRegistry.end() && it->first.compare(0,prefix.size(),prefix)==0;)
        {
            const LONG idx=atol(it->first.c_str()+prefix.size());
            if (idx>=static_cast<LONG>(tiles.size())) { ++g_playLiveDeleteCount; it=g_playLiveRegistry.erase(it); ++stale; }
            else ++it;
        }
        ++g_playLiveRegistryGeneration;
        char kind[96]="mesh";
        if (grid.curve)
        {
            PlayPatchGrid flat;
            if (PlayTessellateCurve(grid,flat)) snprintf(kind,sizeof kind,"curve (drawn as its surface, %ld x %ld points)",(long)flat.rows,(long)flat.cols);
            else snprintf(kind,sizeof kind,"curve (even grid: control net drawn)");
        }
        char line[400]={};
        snprintf(line,sizeof line,"[LIVE PATCH] %s: %ld x %ld %s, material %s, %u tile(s) (%ld new, %ld updated, %u dropped)",
            sel.brushGuid,(long)grid.rows,(long)grid.cols,kind,grid.material.c_str(),
            (unsigned)tiles.size(),(long)created,(long)updated,(unsigned)stale);
        PlayAppendLog(line);
        return created+updated>0;
    }

    int PlayPatchGridSeh(const char* guid, int* r, int* c, float* xyz, int cap);   // below, with the patch hide records
    bool PlayIsPatchBrush(const char* brushGuid)
    {
        // the host's BrushPlanes reports a patch's bounding box (6 planes), so the patch test is PatchGrid: rows * cols or 0
        if (!brushGuid || !*brushGuid || !rp::host || !rp::Has(&rp::host->PatchGrid) || !rp::host->PatchGrid) return false;
        int r=0, c=0;
        return PlayPatchGridSeh(brushGuid,&r,&c,nullptr,0)>0;
    }

    bool PlayUpsertSettledLiveObject(const ModToolsSelectedBrush& sel, ModToolsBrushInst inst, bool& exactOut)
    {
        // a mesh / curve brush goes through the patch path
        if (inst && sel.brushGuid[0] && !PlayEntityIsPrefab(sel.entityGuid) && PlayIsPatchBrush(sel.brushGuid))
        {
            PlayPatchGrid grid;
            if (PlayTryReadSettledPatch(inst,sel.brushGuid,grid)) { exactOut=true; return PlayUpsertSettledPatch(sel,grid); }
            PlayAppendLog(std::string("[LIVE PATCH] ")+sel.brushGuid+": the writer's mesh block could not be read; the patch keeps its bounds proxy");
        }
        PlayLiveLocalObject candidate;
        if (!PlayBuildSettledLiveObject(sel,inst,candidate,exactOut)) return false;
        const std::string key=PlayLiveGuidKey(sel.brushGuid);
        auto found=g_playLiveRegistry.find(key);
        if (found==g_playLiveRegistry.end())
        {
            if (g_playLiveRegistry.size()>=brlive::kMaxLiveObjects)
            {
                PlayAppendLog("[LIVE REGISTRY WARN] Registry capacity reached; the settled object was not published.");
                return false;
            }
            candidate.liveObjectId=PlayAllocateLiveObjectId();
            candidate.generation=1;
            candidate.operation=brlive::LIVE_OBJECT_CREATE;
            found=g_playLiveRegistry.emplace(key,std::move(candidate)).first;
            ++g_playLiveCreateCount;
        }
        else
        {
            candidate.liveObjectId=found->second.liveObjectId;
            candidate.generation=found->second.generation+1;
            candidate.operation=brlive::LIVE_OBJECT_UPDATE;
            found->second=std::move(candidate);
            ++g_playLiveUpdateCount;
        }
        ++g_playLiveRegistryGeneration;
        if (found->second.stats.reconstructionFailed) ++g_playLiveReconstructionFailures;
        if (found->second.geometryType==brlive::LIVE_GEOMETRY_PATCH && found->second.stats.usedAabb)
            ++g_playLivePatchReconstructionFailures;
        g_playLiveLastOperation=found->second.operation;
        g_playLiveLastOperationId=found->second.liveObjectId;
        g_playLiveLastOperationGeneration=static_cast<LONG>(found->second.generation);
        PlayWriteLegacySnapshot(found->second);
        return true;
    }

    bool PlayAnyEditorMouseButtonDown()
    {
        return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
               (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 ||
               (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0 ||
               (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0 ||
               (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0;
    }

    void PlayTrackReleaseSnapshot(uint64_t now)
    {
        // A plain selection click must never publish geometry: arm only after the cursor actually moves while a button is held, then sample once well after release.
        bool down = PlayAnyEditorMouseButtonDown();
        POINT pt = {}; GetCursorPos(&pt);
        if (!g_playLiveMouseWasDown && down)
        {
            g_playLiveMouseDownPt = pt;
            g_playLiveDragArmed = false;
            g_playLiveReleaseSnapshotPending = false;
        }
        else if (g_playLiveMouseWasDown && down && !g_playLiveDragArmed)
        {
            LONG dx = pt.x - g_playLiveMouseDownPt.x; if (dx < 0) dx = -dx;
            LONG dy = pt.y - g_playLiveMouseDownPt.y; if (dy < 0) dy = -dy;
            if (dx >= 4 || dy >= 4) g_playLiveDragArmed = true;
        }
        if (g_playLiveMouseWasDown && !down)
        {
            if (g_playLiveDragArmed && g_playLivePreview && PlayLiveEditBuild() && !g_playInputCaptured && PlayRadiantForeground())
            {
                g_playLiveReleaseSnapshotPending = true;
                g_playLiveReleaseSnapshotDue = now + 300;
                ++g_playLiveReleaseEvents;
            }
            else if (!g_playLiveDragArmed && g_playLivePreview && PlayLiveEditBuild() && !g_playInputCaptured && PlayRadiantForeground() && !g_playLiveRegistry.empty())
            {
                // a texture applied from the texture window is a click, not a drag: check the published objects' materials shortly after and republish the ones whose faces changed material
                g_playLiveMaterialCheckPending = true;
                g_playLiveMaterialCheckDue = now + 300;
            }
            g_playLiveDragArmed = false;
        }
        g_playLiveMouseWasDown = down;
    }

    bool PlayBakedHullUnchanged(const char* brushGuid, ModToolsBrushInst inst);   // below the baked hull cache
    bool PlayBakedPatchUnchanged(const std::string& guidKey);                    // same, for a mesh patch
    void PlayPublishSettledSelection(uint64_t now, bool afterDelete);

    void PlayTryPublishReleaseSnapshot(uint64_t now)
    {
        if (!g_playLiveReleaseSnapshotPending || now < g_playLiveReleaseSnapshotDue) return;
        if (PlayAnyEditorMouseButtonDown() || g_playInputCaptured || !PlayRadiantForeground()) return;
        g_playLiveReleaseSnapshotPending = false;
        PlayPublishSettledSelection(now,false);
    }

    // Clip: Radiant's clipper deletes the original brush through Select_Delete and only then moves the split pieces into the selection (split clip 0x140356B90: ProduceSplitLists,
    // Select_Delete, Brush_CopyList of both lists into selected_brushes, plain clip 0x140356D20 the same with one list). Publishing ran on mouse-drag releases only and a clip ends with Enter,
    // so after every captured delete the selection is read once it settled and published. Brushes the BSP contains with their compiled hull are skipped (an unchanged baked brush must not get a live twin).
    void PlayTryPublishDeleteFollowup(uint64_t now)
    {
        if (!g_playLiveDeleteFollowupPending || now < g_playLiveDeleteFollowupDue) return;
        if (PlayAnyEditorMouseButtonDown() || g_playInputCaptured) return;
        g_playLiveDeleteFollowupPending = false;
        PlayPublishSettledSelection(now,true);
    }

    void PlayPublishSettledSelection(uint64_t now, bool afterDelete)
    {
        if (PlayPrefabDepth()>0) { g_playScopeDirty=true; g_playScopeDirtyAt=now; return; }   // inside a prefab: the scope capture, not brush objects in the file's space
        size_t selectionCount=0;
        size_t instanceCount=0;
        if (!PlayTryFillSettledSelection(nullptr,0,selectionCount) ||
            !PlayTryFillSettledBrushInsts(nullptr,0,instanceCount) ||
            !selectionCount || !instanceCount || selectionCount>4096 || instanceCount>4096)
        {
            if (afterDelete) return;   // a plain delete: nothing is selected afterwards
            ++g_playLiveSnapshotFailures;
            if (!g_playLiveSnapshotWarned)
            {
                g_playLiveSnapshotWarned = true;
                PlayAppendLog("[LIVE WARN] Mouse-release snapshot found no selected brush; no payload was published.");
            }
            return;
        }

        std::vector<ModToolsSelectedBrush> selections(selectionCount);
        std::vector<ModToolsBrushInst> instances(instanceCount,nullptr);
        size_t selectedRead=0;
        size_t instancesRead=0;
        if (!PlayTryFillSettledSelection(selections.data(),selections.size(),selectedRead) ||
            !PlayTryFillSettledBrushInsts(instances.data(),instances.size(),instancesRead))
        {
            ++g_playLiveSnapshotFailures;
            PlayAppendLog("[LIVE WARN] Settled multi-selection snapshot could not be read; the prior registry remains active.");
            return;
        }
        selections.resize((std::min)(selectedRead,selections.size()));
        instances.resize((std::min)(instancesRead,instances.size()));
        std::vector<std::string> instanceGuids(instances.size());
        for (size_t i=0;i<instances.size();++i)
        {
            char guid[40]={};
            if (PlayTrySettledBrushGuid(instances[i],guid)) instanceGuids[i]=PlayLiveGuidKey(guid);
        }

        for (auto& pair:g_playLiveRegistry) pair.second.flags&=~brlive::LIVE_OBJECT_SELECTED;
        std::vector<bool> used(instances.size(),false);
        LONG published=0;
        LONG exactCount=0;
        LONG fallbackCount=0;
        LONG unchangedBaked=0, restoredLive=0;
        for (const ModToolsSelectedBrush& sel:selections)
        {
            const std::string key=PlayLiveGuidKey(sel.brushGuid);
            size_t match=instances.size();
            for (size_t i=0;i<instances.size();++i)
                if (!used[i] && !key.empty() && instanceGuids[i]==key) {match=i;break;}
            if (match==instances.size() && selections.size()==instances.size())
            {
                const size_t index=static_cast<size_t>(&sel-selections.data());
                if (index<instances.size() && !used[index]) match=index;
            }
            if (match==instances.size()) {++g_playLiveSnapshotFailures;continue;}
            used[match]=true;
            if (afterDelete)
            {
                const bool patch=PlayIsPatchBrush(sel.brushGuid);
                if (patch ? PlayBakedPatchUnchanged(key) : PlayBakedHullUnchanged(sel.brushGuid,instances[match]))
                {
                    // back at its compiled shape (undo): the compiled copy shows again, a live twin must not stay
                    ++unchangedBaked;
                    if (patch) restoredLive+=static_cast<LONG>(PlayEraseRegistryTiles(key));
                    else { auto live=g_playLiveRegistry.find(key); if (live!=g_playLiveRegistry.end()) { ++g_playLiveDeleteCount; g_playLiveRegistry.erase(live); ++restoredLive; } }
                    continue;
                }
            }
            bool exact=false;
            if (!PlayUpsertSettledLiveObject(sel,instances[match],exact))
            {
                ++g_playLiveSnapshotFailures;
                continue;
            }
            ++published;
            if (exact) ++exactCount; else ++fallbackCount;
        }

        if (restoredLive>0) ++g_playLiveRegistryGeneration;
        if (published>0 || restoredLive>0)
        {
            if (g_playLiveRegistry.empty()) PlayPublishLiveClear();
            PlayPublishLiveRegistry();
            PlayReconcileLiveHides(now,true);   // live delete: an edited brush with a baked hull hides its original now
            g_playLiveUpdates+=static_cast<uint32_t>(published);
            ++g_playLiveMoveEvents;
            if (afterDelete)
            {
                char line[240]={};
                snprintf(line,sizeof line,"[LIVE REGISTRY] after delete / undo / redo / peer edit: %ld brush(es) published live, %ld back at their compiled shape (%ld live twin(s) dropped)",
                    (long)published,(long)unchangedBaked,(long)restoredLive);
                PlayAppendLog(line);
            }
            char geometryDiag[1024]={};
            snprintf(geometryDiag,sizeof geometryDiag,
                "[LIVE REGISTRY] settled=%ld exact=%ld fallback=%ld liveObjectCount=%ld liveBrushCount=%ld livePatchCount=%ld createCount=%ld updateCount=%ld deleteCount=%ld staleObjectCount=%ld registryGeneration=%ld nativeEdgeSubmits=%ld",
                (long)published,(long)exactCount,(long)fallbackCount,
                (long)g_playLiveShared->liveObjectCount,(long)g_playLiveShared->liveBrushCount,
                (long)g_playLiveShared->livePatchCount,(long)g_playLiveShared->liveCreateCount,
                (long)g_playLiveShared->liveUpdateCount,(long)g_playLiveShared->liveDeleteCount,
                (long)g_playLiveShared->staleObjectCount,(long)g_playLiveShared->liveRegistryGeneration,
                (long)g_playLiveShared->nativeEdgeSubmits);
            PlayAppendLog(geometryDiag);
            if (!g_playLiveMoveLogged)
            {
                g_playLiveMoveLogged=true;
                PlayAppendLog("[LIVE] v1.9.8 native solid registry is active. Exact settled convex objects coexist after deselection; BO3 renders face triangles from its guarded renderer-context stage and green outlines from the verified main-frame pump while typed worldActive=1.");
            }
        }
    }

    // Republish selected live objects whose face materials changed since they were published (a click in the texture
    // window applies a material without any drag, so the release snapshot never runs for it).
    void PlayTryPublishMaterialChange(uint64_t now)
    {
        if (!g_playLiveMaterialCheckPending || now < g_playLiveMaterialCheckDue) return;
        if (PlayAnyEditorMouseButtonDown() || g_playInputCaptured || !PlayRadiantForeground()) return;
        g_playLiveMaterialCheckPending = false;
        if (PlayPrefabDepth()>0) { g_playScopeDirty=true; g_playScopeDirtyAt=now; return; }
        if (g_playLiveRegistry.empty()) return;
        size_t selectionCount=0, instanceCount=0;
        if (!PlayTryFillSettledSelection(nullptr,0,selectionCount) || !PlayTryFillSettledBrushInsts(nullptr,0,instanceCount) ||
            !selectionCount || !instanceCount || selectionCount>4096 || instanceCount>4096) return;
        std::vector<ModToolsSelectedBrush> selections(selectionCount);
        std::vector<ModToolsBrushInst> instances(instanceCount,nullptr);
        size_t selectedRead=0, instancesRead=0;
        if (!PlayTryFillSettledSelection(selections.data(),selections.size(),selectedRead) ||
            !PlayTryFillSettledBrushInsts(instances.data(),instances.size(),instancesRead)) return;
        selections.resize((std::min)(selectedRead,selections.size()));
        instances.resize((std::min)(instancesRead,instances.size()));
        std::vector<std::string> instanceGuids(instances.size());
        for (size_t i=0;i<instances.size();++i)
        {
            char guid[40]={};
            if (PlayTrySettledBrushGuid(instances[i],guid)) instanceGuids[i]=PlayLiveGuidKey(guid);
        }
        LONG published=0;
        for (const ModToolsSelectedBrush& sel:selections)
        {
            const std::string key=PlayLiveGuidKey(sel.brushGuid);
            auto found=g_playLiveRegistry.find(key);
            if (found==g_playLiveRegistry.end()) found=g_playLiveRegistry.find(PlayPatchTileKey(key,0));   // a patch's first tile
            if (found==g_playLiveRegistry.end()) continue;
            size_t match=instances.size();
            for (size_t i=0;i<instances.size();++i) if (!key.empty() && instanceGuids[i]==key) {match=i;break;}
            if (match==instances.size()) continue;
            if (found->second.geometryType==brlive::LIVE_GEOMETRY_PATCH)
            {
                PlayPatchGrid grid;
                if (!PlayTryReadSettledPatch(instances[match],sel.brushGuid,grid)) continue;
                if (!found->second.materials.empty() && _stricmp(found->second.materials[0].c_str(),grid.material.c_str())==0) continue;
                if (PlayUpsertSettledPatch(sel,grid)) ++published;
                continue;
            }
            std::vector<PlayLiveFaceTex> texdefs;
            if (!PlayTryReadSettledBrushTexdefs(instances[match],sel.brushGuid,texdefs)) continue;
            std::set<std::string> now_, before;
            for (const PlayLiveFaceTex& t:texdefs) { std::string m=t.material; std::transform(m.begin(),m.end(),m.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));}); now_.insert(m); }
            for (const std::string& m0:found->second.materials) { std::string m=m0; std::transform(m.begin(),m.end(),m.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));}); before.insert(m); }
            if (now_==before) continue;
            bool exact=false;
            if (PlayUpsertSettledLiveObject(sel,instances[match],exact)) ++published;
        }
        if (published>0)
        {
            PlayPublishLiveRegistry();
            g_playLiveUpdates+=static_cast<uint32_t>(published);
            PlayAppendLog("[LIVE REGISTRY] material change: "+std::to_string(published)+" object(s) republished");
        }
    }

    bool PlayTryFindLiveBrushes(const char* const* guids,size_t count,ModToolsBrush* out)
    {
        if (!guids || !out || !count || !rp::host ||
            !rp::Has(&rp::host->FindBrushesByGuid) || !rp::host->FindBrushesByGuid)
            return false;
        __try
        {
            rp::host->FindBrushesByGuid(rp::self,guids,count,out);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // ---- Live delete: hide the baked geometry and collision of brushes that changed while the game runs ----
    // The BAKED HULL CACHE (GUID -> outward planes and bounds, PlayLoadBakedHulls) comes from the .map Play saved. A cached brush that is absent or has another hull gets a hide
    // record (the bridge blanks its world triangles and clipmap brush), undo deactivates it, redo re-arms it (PlayReconcileLiveHides). Deletes come from a Select_Delete detour (0x140D02320), edits from the Live Edit registry.
    struct PlayLiveHide
    {
        std::string guid;                    // as written in the .map
        std::vector<PlayLivePlane> planes;   // outward, n.x <= d inside (the baked hull)
        float mins[3] = {0.0f,0.0f,0.0f};
        float maxs[3] = {0.0f,0.0f,0.0f};
        bool active = true;
        uint64_t since = 0;
    };
    struct PlayBakedHull
    {
        std::vector<PlayLivePlane> planes;
        float mins[3] = {0.0f,0.0f,0.0f};
        float maxs[3] = {0.0f,0.0f,0.0f};
        std::vector<PlayLiveFaceTex> tex;    // the face lines' texdefs (world space): hole fills draw a neighbour's face with them
    };
    // HOLE FILLS: cod2map drops a face that lies inside or flush against another structural brush, so the floor under a sunk brush was never in the BSP.
    // For every hidden hull H and baked brush N that stays and touches it, the convex slab N (inset kPlayFillInset) intersected with H (grown kPlayFillExpand) is drawn
    // as a live object with N's face texdefs: its outer faces are N's faces where H was, a quarter unit below N's surface so a face that IS still there wins.
    bool g_playLiveFillHoles = true;             // [playtest] live_fill_holes
    constexpr float kPlayFillInset = 0.25f, kPlayFillExpand = 0.5f;
    constexpr size_t kPlayMaxFills = 192;

    std::map<std::string,PlayLiveHide> g_playLiveHides;   // key = PlayLiveGuidKey
    std::map<std::string,PlayBakedHull> g_playBakedHulls; // key = PlayLiveGuidKey, from the saved .map
    // PATCHES: every `mesh` block of the saved map as its control grid (curves are tessellated by the compiler and cannot be matched point for point: not cached).
    // A record is the grid the game was compiled with, the bridge blanks world triangles on its surface and zeroes the clipmap partitions whose vertices lie on it.
    struct PlayBakedPatch
    {
        LONG rows = 0, cols = 0;
        std::vector<PlayLiveVec3> pts;
        float mins[3] = {0.0f,0.0f,0.0f};
        float maxs[3] = {0.0f,0.0f,0.0f};
    };
    struct PlayLivePatchHide
    {
        std::string guid;
        PlayBakedPatch grid;
        bool active = true;
        uint64_t since = 0;
    };
    std::map<std::string,PlayBakedPatch> g_playBakedPatches;      // key = PlayLiveGuidKey
    std::map<std::string,PlayLivePatchHide> g_playLivePatchHides; // key = PlayLiveGuidKey
    std::vector<PlayLivePatchHide> g_playDeletePatchCandidates;    // main thread only (the Select_Delete detour)
    bool PlaySameGrid(const PlayBakedPatch& a, LONG rows, LONG cols, const float* xyz)
    {
        if (a.rows!=rows || a.cols!=cols || a.pts.size()!=static_cast<size_t>(rows)*static_cast<size_t>(cols)) return false;
        for (size_t i=0;i<a.pts.size();++i)
        {
            const float dx=a.pts[i].x-xyz[i*3], dy=a.pts[i].y-xyz[i*3+1], dz=a.pts[i].z-xyz[i*3+2];
            if (dx*dx+dy*dy+dz*dz>0.125f*0.125f) return false;
        }
        return true;
    }
    void PlayPatchBounds(PlayBakedPatch& g)
    {
        for (size_t i=0;i<g.pts.size();++i)
        {
            const float v[3]={g.pts[i].x,g.pts[i].y,g.pts[i].z};
            for (int a=0;a<3;++a) { if (i==0) { g.mins[a]=g.maxs[a]=v[a]; } else { g.mins[a]=(std::min)(g.mins[a],v[a]); g.maxs[a]=(std::max)(g.maxs[a],v[a]); } }
        }
    }
    bool PlayPatchHideFromBaked(const std::string& key, PlayLivePatchHide& out)
    {
        auto found=g_playBakedPatches.find(key);
        if (found==g_playBakedPatches.end()) return false;
        out.guid=key; out.grid=found->second; out.active=true; out.since=GetTickCount64();
        return true;
    }
    // The current grid of a patch in the map through the host (selected or not), false when it is not a patch.
    int PlayPatchGridSeh(const char* guid, int* r, int* c, float* xyz, int cap)   // no C++ objects here: SEH frame
    {
        __try { return rp::host->PatchGrid(rp::self,guid,r,c,xyz,cap); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }
    bool PlayCurrentPatchGrid(const std::string& guidKey, LONG& rows, LONG& cols, std::vector<float>& xyz)
    {
        if (!rp::host || !rp::Has(&rp::host->PatchGrid) || !rp::host->PatchGrid) return false;
        int r=0, c=0;
        xyz.assign(1024*3,0.0f);
        int n=PlayPatchGridSeh(guidKey.c_str(),&r,&c,xyz.data(),1024);
        if (n<=0 || r<2 || c<2 || static_cast<size_t>(r)*static_cast<size_t>(c)!=static_cast<size_t>(n)) return false;
        if (n>1024)
        {
            xyz.assign(static_cast<size_t>(n)*3,0.0f);
            n=PlayPatchGridSeh(guidKey.c_str(),&r,&c,xyz.data(),n);
            if (n<=0 || static_cast<size_t>(r)*static_cast<size_t>(c)!=static_cast<size_t>(n)) return false;
        }
        rows=r; cols=c; xyz.resize(static_cast<size_t>(n)*3);
        return true;
    }
    // PREFAB INTERIORS: one entry per misc_prefab instance of the saved map (nested prefabs folded in), its compiled placement and every interior brush's hull in world space.
    // The reconcile compares the live entity's origin / angles / model with the compiled ones (WriteEntityText), absent or moved -> every interior hull becomes a hide record
    // (sourceGuid = a synthetic GUID from the instance and the brush), back -> shown.
    struct PlayBakedModel { std::string entityGuid; std::string model; float origin[3] = {0,0,0}; float angles[3] = {0,0,0}; float scale = 1.0f; bool hidden = false; bool isNew = false; };
    struct PlayBakedPrefabModel
    {
        std::string recordGuid; std::string model; float origin[3] = {0,0,0}; float axis[9] = {1,0,0,0,1,0,0,0,1}; float scale = 1.0f;
        std::string fileKey;                     // normalized prefab file + entity guid (or model name): matches compiled with current
        float localOrigin[3] = {0,0,0}; float localAxis[9] = {1,0,0,0,1,0,0,0,1};   // placement in the outermost file's space (re-placed at a live placement)
        bool stale = false, changed = false;     // compiled: hide it, current: draw it (see PlayDiffPrefabGeometry)
    };
    struct PlayPrefabPlacement { float origin[3] = {0,0,0}; float angles[3] = {0,0,0}; std::string model; };
    // LIVE SPOTS: a perk machine, wallbuy or box is not in the BSP: the prefab holds a script_struct and the level's scripts spawn the model, triggers and clip at that struct.
    // Every script_struct inside a prefab instance is kept with its placement in the outermost file's space. When the instance moves, PlayPublishLiveSpots sends compiled place -> live place
    // (dvars br_live_s<i>, br_live_sn, br_live_sgen) and the GSC receiver moves the struct and what stands on it.
    struct PlayBakedPrefabSpot { float localOrigin[3] = {0,0,0}; float localAxis[9] = {1,0,0,0,1,0,0,0,1}; };
    struct PlayBakedPrefabHull
    {
        std::string recordGuid;                 // synthetic, unique per (instance, brush): hide record and live object key
        PlayBakedHull hull;                     // world space at the compiled placement
        std::vector<float> localRaw;            // 3 points per face in the prefab file's space (nested: the outermost file's)
        std::vector<PlayLiveFaceTex> localTex;  // the face lines' texdefs (points in the same space)
        std::vector<PlayLivePlane> localPlanes; // the hull in that space (compares a re-read file with the compiled one)
        std::string fileKey;                    // normalized prefab file + brush guid: matches a compiled hull with the current one
        std::string ownerClass;                 // the owning entity's classname in the file, empty for worldspawn
        bool stale = false, changed = false;    // compiled: hide it (gone or different now), current: draw it (new or different)
    };
    // PATCHES INSIDE PREFABS: mesh patches of the prefab files, like the hulls: the compiled grid in world space (hidden by point-on-surface records when the instance moves or the patch changed)
    // and the file's grid with uvs and material in the outermost file's space (drawn as live tiles at the live placement).
    struct PlayBakedPrefabPatch
    {
        std::string recordGuid, fileKey;
        PlayBakedPatch grid;                    // world space at the compiled placement
        PlayPatchGrid local;                    // outermost file space, uv and material from the mesh block
        bool stale = false, changed = false;
    };
    struct PlayBakedPrefab
    {
        std::string entityGuid;                  // as written in the .map
        PlayPrefabPlacement compiled;
        std::vector<PlayBakedPrefabHull> hulls;
        bool hidden = false;
        uint64_t since = 0;
        bool liveObjects = false;                // its hulls are in the live registry at `live` (a moved prefab draws there)
        PlayPrefabPlacement live;
        std::vector<PlayBakedPrefabModel> models; // misc_models inside the instance, placed (hidden with the prefab)
        std::vector<PlayBakedPrefabSpot> spots;   // script_structs inside the instance as compiled (LIVE SPOTS)
        // The file as it is on disk now (re-read when a prefab file's write time changes): the live objects come from here
        // when it differs from the compiled interior (fileChanged) or the instance was placed after the Play (isNew).
        std::vector<PlayBakedPrefabHull> current;
        bool fileChanged = false;
        bool isNew = false;
        uint32_t geomGeneration = 0, liveGeomGeneration = 0;
        std::vector<std::string> liveKeys;       // registry keys of its live objects
        std::vector<PlayBakedPrefabPatch> patches, currentPatches;   // compiled / as the file (or Radiant's memory) has it now
        std::vector<PlayBakedPrefabModel> currentModels;             // the file's models now (bp.models = compiled)
        uint32_t reconciledGeneration = 0; bool reconciledFileChanged = false;   // what the last hide publish saw
        bool liveFull = false;                   // the live objects are the whole interior (moved or new), not the changed subset
    };
    std::map<std::string,PlayBakedPrefab> g_playBakedPrefabs;   // key = PlayLiveGuidKey(entity guid)
    bool g_playLiveDeletePrefabs = true;         // [playtest] live_delete_prefabs
    std::map<std::string,uint64_t> g_playPrefabFileTimes;   // prefab file (map_source-relative, lower case) -> write time when read
    std::set<std::string> g_playSeenEntityKeys;             // entity GUIDs known since the cache load (new ones are looked at once)
    uint64_t g_playLastPrefabFileCheck = 0;
    // STATIC MODELS: every misc_model of the saved map (and those inside prefab instances, placed) with its compiled placement. Deleted or moved -> a protocol v23 ModelHideRecord
    // (the bridge hides the baked instance and disables its collision model), back -> shown again.
    void PlayAnglesToAxis(const float angles[3], float ax[3][3]);
    std::map<std::string,PlayBakedModel> g_playBakedModels;     // key = PlayLiveGuidKey(entity guid)
    bool g_playLiveDeleteModels = true;          // [playtest] live_delete_models
    LONG g_playModelHideGeneration = 0;
    void PlayPublishModelHides();
    // LIVE MODELS: a misc_model moved from its compiled placement, or placed after the Play, is spawned in the running game as a script_model by the GSC receiver (PlayConfigureLivePreview:
    // dvars br_live_m<i>, br_live_mn, br_live_mgen through the zone module mailbox). The bridge answers per xmodel name whether the level has it (protocol v25 liveModelState),
    // a missing one is linked as a live zone like a missing material ("xmodel,<name>").
    struct PlayLiveModel { std::string entityGuid; std::string model; float origin[3] = {0,0,0}; float angles[3] = {0,0,0}; float scale = 1.0f; };
    bool g_playLiveModels = true;                // [playtest] live_models
    bool g_playLiveModelsDebug = false;          // [playtest] live_models_debug: the receiver prints every spawn / pose / delete (IPrintLn, in console_mp.log)
    std::vector<PlayLiveModel> g_playLiveModelWanted;   // rebuilt by every model reconcile
    std::vector<PlayLiveModel> g_playLiveModelWantedTop;        // the map's own misc_models (the prefab interiors' are added by PlayRebuildLiveModelWanted)
    std::vector<std::string> g_playLiveModelSlotNames;  // names written to SharedState::liveModelNames, per slot (stable: a name keeps its slot while wanted)
    std::vector<std::string> g_playLiveModelRecordSlots; // entity GUID key per dvar record index (stable: an entity keeps its script_model)
    std::string g_playLiveModelSent;             // the dvar set last sent to the game
    size_t g_playLiveModelSentCount = 0;
    DWORD g_playLiveModelSentPid = 0;
    LONG g_playLiveModelGen = 0;
    std::set<std::string> g_playLiveModelQueued; // "<map>|<model>" already queued for a live zone this session
    bool g_playBakedHullsLoaded = false;
    std::wstring g_playBakedHullsPath;
    LONG g_playLiveHideGeneration = 0;
    uint64_t g_playLiveLastHideReconcile = 0;
    LONG g_playLiveHideAdded = 0;
    bool g_playLiveDelete = true;              // [playtest] live_delete
    bool g_playLiveDeleteCollision = true;     // [playtest] live_delete_collision

    void PlayNavRefreshSchedule(const char* why, uint64_t delayMs)
    {
        g_playNavDueMs = GetTickCount64() + delayMs;
        g_playNavDueWhy = why ? why : "";
    }

    // Main thread, every engine tick while the shared state is open.
    void PlayNavRefreshTick()
    {
        if (!g_playLiveShared) return;
        const uint64_t now = GetTickCount64();
        // automatic: a BAKED brush moved, scaled, was deleted or came back (the hide record generation): the compiled mesh
        // is wrong under it. New brushes and their moves are the live silhouettes' job (no recompile).
        const LONG geometry = g_playLiveHideGeneration;
        const bool worldActive = g_playLiveShared->engineWorldActive != 0;
        if (worldActive != g_playNavWorldWasActive)
        {
            // a level came up (or went): what was edited before it does not count, the level was built from the saved map
            g_playNavWorldWasActive = worldActive; g_playNavGeometrySeen = geometry; g_playNavDueMs = 0; ++g_playNavLevelEpoch;
        }
        if (g_playNavForPlay && g_playNavRunning && g_playNavPhase == 0)
        {
            g_playNavPhase = 1; g_playNavPhaseMs = now; g_playNavStartEpoch = g_playNavLevelEpoch;
            PlayWaitBegin("nav", "building the nav mesh beside the map");
        }
        if (g_playLiveNavRefresh && geometry != g_playNavGeometrySeen && worldActive && g_playLiveShared->liveNavState == 2)
        {
            g_playNavGeometrySeen = geometry;
            PlayNavRefreshSchedule("baked brush edited", 500);   // the brush is left out of the build, so its final spot does not matter: start early
        }
        // start: the clicks settled, nothing else builds, the game runs with the bridge's nav module up
        if (g_playNavDueMs && now >= g_playNavDueMs && !PlayBusy() && !g_playLoadWatch && !g_playLiveMatRunning && !g_playNavRunning && g_playNavPhase == 0 &&
            g_playLiveShared->engineWorldActive && g_playLiveShared->mainFrameHookInstalled)
        {
            g_playNavDueMs = 0;
            if (g_playLiveShared->liveNavState != 2) { PlayAppendLog("[NAV] refresh skipped: the bridge's nav module is not up (state " + std::to_string((long)g_playLiveShared->liveNavState) + ")"); }
            else
            {
                PlayMapInfo m; std::string error;
                std::string action;
                if (!PlaySaveCurrentMap(action)) PlayAppendLog("[NAV] Warning: couldn't invoke Radiant File/Save; the compile reads the map file as it is on disk.");
                if (!PlayResolveCurrentMap(m, error)) PlayAppendLog("[NAV] refresh skipped: " + error);
                else
                {
                    g_playNavGeometrySeen = g_playLiveHideGeneration;
                    const std::wstring stamp = std::to_wstring((unsigned long long)((GetTickCount64() / 1000) % 1000000));
                    const std::wstring zoneName = m.mapName + L"_nav" + stamp;   // a plain live zone (the bridge frees nothing for it)
                    const std::wstring tempName = m.mapName + L"_n" + stamp;
                    const std::string assetName = "maps/" + PlayWideToUtf8(m.prefix) + "/" + PlayWideToUtf8(tempName) + "_navmesh";
                    const std::wstring mapDir = m.mapPath.substr(0, m.mapPath.find_last_of(L"\\/") + 1), bspDir = m.bspPath.substr(0, m.bspPath.find_last_of(L"\\/") + 1);
                    const std::wstring tempMap = PlayJoin(mapDir, tempName + L".map"), tempBsp = PlayJoin(bspDir, tempName + L".d3dbsp");
                    std::set<std::string> strip; for (const auto& kv : g_playLiveHides) if (kv.second.active) strip.insert(kv.first);
                    int stripped = 0; std::string werror;
                    if (!PlayWriteNavTempMap(m.mapPath, tempMap, strip, stripped, werror)) { PlayAppendLog("[NAV] refresh skipped: " + werror); return; }
                    // a custom <map>_navmesh_settings.json beside the map applies to the copy too
                    { const std::wstring st = PlayJoin(mapDir, m.mapName + L"_navmesh_settings.json"); if (PlayFileExists(st)) CopyFileW(st.c_str(), PlayJoin(mapDir, tempName + L"_navmesh_settings.json").c_str(), FALSE); }
                    PlayAppendLog("[NAV] refresh (" + g_playNavDueWhy + "): map saved, " + std::to_string(stripped) + " moved baked brush(es) left out of the nav build, compiling and linking nav zone " + PlayWideToUtf8(zoneName) + " ...");
                    PlayNote("Updating the zombies' nav mesh for your edits (compile, link, then load into the game; the game keeps running)...");
                    PlayWaitBegin("nav", "updating the nav mesh");
                    if (g_playNavThread.joinable()) g_playNavThread.join();
                    g_playNavCancel = false; g_playNavThreadDone = false; g_playNavRunning = true; g_playNavPhase = 1; g_playNavPhaseMs = now; g_playNavStartEpoch = g_playNavLevelEpoch;
                    g_playNavThread = std::thread([m, zoneName, stamp, assetName, tempMap, tempBsp]() { PlayNavWorker(m, zoneName, stamp, assetName, tempMap, tempBsp); });
                }
            }
        }
        auto fail = [&](const std::string& why)
        {
            ++g_playNavFailures; g_playNavPhase = 0; g_playNavWaitZoneSeq = 0; g_playNavWaitSwapSeq = 0; g_playNavForPlay = false; PlayAppendLog("[NAV] refresh failed: " + why);
            { std::lock_guard<std::mutex> lock(g_playNavMutex); g_playNavKeepStem.clear(); }
            PlayWaitEnd("nav");
            PlayNote("The nav mesh update failed (" + why + "). Zombies keep the old paths until the next Play.");
        };
        if (g_playNavPhase && g_playNavPhase != 5 && !g_playNavRunning && now - g_playNavPhaseMs > 90000) { fail("phase " + std::to_string(g_playNavPhase) + " timed out"); return; }
        // 5: built beside Play's compile, waiting for the level of that build with the bridge's nav module up
        if (g_playNavPhase == 5)
        {
            if (g_playState.load() != PLAY_RUNNING && !PlayBusy()) { fail("Play did not reach the game"); return; }
            if (now - g_playNavPhaseMs > 5 * 60000) { fail("the level did not come up within 5 min"); return; }
            // a new level: the world flipped (cold start, relaunch) or the bridge's G_InitGame count moved (a warm reload can flip inside one tick)
            const LONG initBase = g_playNavForPlayInitBase.load(), initNow = PlayLoadInitCount();
            const bool newLevel = g_playNavLevelEpoch != g_playNavStartEpoch || (initBase >= 0 && initNow > initBase);
            if (!g_playLiveShared->engineWorldActive || !newLevel || g_playLiveShared->liveNavState != 2 ||
                !g_playLiveShared->mainFrameHookInstalled || !g_playLiveZoneInjected) return;
            g_playNavForPlay = false;
            PlayAppendLog("[NAV] the level is up; the bridge is loading nav zone " + g_playNavZoneName + " ...");
            strncpy_s(g_playLiveShared->zoneLoadName, g_playNavZoneName.c_str(), _TRUNCATE);
            g_playNavWaitZoneSeq = InterlockedIncrement(&g_playLiveShared->zoneLoadSeq);
            ++g_playMidLevelZoneLoads;
            g_playNavPhase = 3; g_playNavPhaseMs = now;
            return;
        }
        if (!g_playLiveShared->engineWorldActive && g_playNavPhase >= 2 && g_playNavPhase != 5) { fail("the level ended"); return; }
        // 1 -> 2: the build finished, the bridge first goes back to the base zone's nav mesh (the full live zone frees the earlier ones)
        if (g_playNavPhase == 1 && g_playNavThreadDone)
        {
            if (g_playNavThread.joinable()) g_playNavThread.join();
            g_playNavRunning = false;
            PlayNavResult r; { std::lock_guard<std::mutex> lock(g_playNavMutex); r = g_playNavResult; }
            if (g_playNavCancel) { g_playNavCancel = false; g_playNavForPlay = false; { std::lock_guard<std::mutex> lock(g_playNavMutex); g_playNavKeepStem.clear(); } g_playNavPhase = 0; g_playNavWaitZoneSeq = 0; g_playNavWaitSwapSeq = 0; PlayWaitEnd("nav"); PlayAppendLog("[NAV] refresh cancelled: Play builds the map itself."); return; }
            if (!r.ok) { fail(r.error); return; }
            if (g_playNavForPlay)
            {
                g_playNavZoneName = r.zoneName; g_playNavPendingAsset = r.assetName;
                PlayAppendLog("[NAV] zone " + r.zoneName + " built beside the map in " + std::to_string(r.seconds) + " s; it goes into the game once the level is up.");
                PlayWaitRename("nav", "waiting for the level to take the new nav mesh");
                g_playNavPhase = 5; g_playNavPhaseMs = now;
                return;
            }
            if (!g_playLiveShared->engineWorldActive || g_playNavStartEpoch != g_playNavLevelEpoch) { fail("the level ended or was reloaded while it built"); return; }
            g_playNavZoneName = r.zoneName; g_playNavPendingAsset = r.assetName;
            if (g_playLiveNavRevert)
            {
                // a zone load while the world runs on a live zone's mesh froze the game: the base zone's mesh comes back first, then the zone loads, then the swap
                PlayAppendLog("[NAV] zone " + r.zoneName + " built in " + std::to_string(r.seconds) + " s; the bridge goes back to the base nav mesh, then loads it ...");
                InterlockedExchange(&g_playLiveShared->navSwapOp, 1);
                g_playNavWaitSwapSeq = InterlockedIncrement(&g_playLiveShared->navSwapSeq);
                g_playNavPhase = 2; g_playNavPhaseMs = now;
            }
            else
            {
                PlayAppendLog("[NAV] zone " + r.zoneName + " built in " + std::to_string(r.seconds) + " s; the bridge is loading it ...");
                strncpy_s(g_playLiveShared->zoneLoadName, g_playNavZoneName.c_str(), _TRUNCATE);
                g_playNavWaitZoneSeq = InterlockedIncrement(&g_playLiveShared->zoneLoadSeq);
                ++g_playMidLevelZoneLoads;
                g_playNavPhase = 3; g_playNavPhaseMs = now;
            }
        }
        // 2 -> 3: reverted, load the zone
        if (g_playNavPhase == 2 && g_playLiveShared->navSwapDoneSeq == g_playNavWaitSwapSeq)
        {
            const std::string text(g_playLiveShared->navSwapText, strnlen(g_playLiveShared->navSwapText, sizeof g_playLiveShared->navSwapText));
            PlayAppendLog("[NAV] " + text);
            if (!g_playLiveShared->navSwapResult) { fail("the revert failed"); return; }
            strncpy_s(g_playLiveShared->zoneLoadName, g_playNavZoneName.c_str(), _TRUNCATE);
            g_playNavWaitZoneSeq = InterlockedIncrement(&g_playLiveShared->zoneLoadSeq);
            ++g_playMidLevelZoneLoads;
            g_playNavPhase = 3; g_playNavPhaseMs = now;
        }
        // 3 -> 4: the zone is in and its DB_Sync ran (or 10 s passed): ask for the swap
        if (g_playNavPhase == 3 && g_playLiveShared->zoneLoadDoneSeq == g_playNavWaitZoneSeq)
        {
            if (!g_playLiveShared->zoneLoadResult) { fail(std::string("the zone did not load: ") + std::string(g_playLiveShared->zoneLoadText, strnlen(g_playLiveShared->zoneLoadText, sizeof g_playLiveShared->zoneLoadText))); return; }
            const bool synced = g_playLiveShared->zoneLoadSyncedSeq == g_playNavWaitZoneSeq;
            if (synced || now - g_playNavPhaseMs > 10000)
            {
                strncpy_s(g_playLiveShared->navSwapAssetName, g_playNavPendingAsset.c_str(), _TRUNCATE);
                InterlockedExchange(&g_playLiveShared->navSwapOp, 2);
                g_playNavWaitSwapSeq = InterlockedIncrement(&g_playLiveShared->navSwapSeq);
                PlayAppendLog(std::string("[NAV] zone loaded") + (synced ? " and synced" : " (no sync signal within 10 s)") + "; swapping the nav mesh in the running level ...");
                g_playNavWaitZoneSeq = 0; g_playNavPhase = 4; g_playNavPhaseMs = now; g_playNavDumpTaken = false;
            }
        }
        // a swap that does not answer within 8 s has the game thread stuck in the engine: one dump of the game for the stack
        if (g_playNavPhase == 4 && g_playLiveShared->navSwapDoneSeq != g_playNavWaitSwapSeq && now - g_playNavPhaseMs > 8000 && !g_playNavDumpTaken)
        {
            g_playNavDumpTaken = true;
            const std::string step(g_playLiveShared->navSwapText, strnlen(g_playLiveShared->navSwapText, sizeof g_playLiveShared->navSwapText));
            PlayAppendLog("[NAV] the swap has not answered in 8 s (last: " + step + "): writing a dump of the game");
            PlayWriteHangDump(g_playGamePid, g_playEmbeddedHwnd, L"-navswap", true);
        }
        // 4 -> idle: the swap answered
        if (g_playNavPhase == 4 && g_playLiveShared->navSwapDoneSeq == g_playNavWaitSwapSeq)
        {
            const std::string text(g_playLiveShared->navSwapText, strnlen(g_playLiveShared->navSwapText, sizeof g_playLiveShared->navSwapText));
            const uint64_t ms = PlayWaitEnd("nav");
            if (g_playLiveShared->navSwapResult == 1) { ++g_playNavCount; PlayAppendLog("[NAV] " + text); PlayNote("Nav mesh updated in the game" + (ms ? " (" + PlayDuration(ms) + ")" : std::string()) + "."); }
            else { ++g_playNavFailures; PlayAppendLog("[NAV] swap did not happen: " + text); PlayNote("The nav mesh update did not go in (" + text + "). Zombies keep the old paths until the next Play."); }
            g_playNavPhase = 0; g_playNavWaitSwapSeq = 0;
            { std::lock_guard<std::mutex> lock(g_playNavMutex); g_playNavKeepStem.clear(); }
        }
    }

    using PlaySelectDeleteFn = void (*)();
    PlaySelectDeleteFn g_playOrigSelectDelete = nullptr;
    bool g_playSelectDeleteHooked = false;
    int g_playSelectDeleteDepth = 0;
    std::vector<PlayLiveHide> g_playDeleteCandidates;   // main thread only

    LONG g_playLiveHidePublishes = 0;   // every publish attempt, game or not (the hole fills follow it)
    void PlayPublishLiveHides()
    {
        ++g_playLiveHidePublishes;
        if (!g_playLiveShared) return;
        InterlockedIncrement(&g_playLiveShared->hideSeq);
        MemoryBarrier();
        LONG n=0;
        for (const auto& pair:g_playLiveHides)
        {
            const PlayLiveHide& h=pair.second;
            if (!h.active) continue;
            if (n>=brlive::kMaxHideRecords) break;
            brlive::HideRecord& r=g_playLiveShared->hides[n++];
            std::memset(&r,0,sizeof r);
            strncpy_s(r.sourceGuid,h.guid.c_str(),_TRUNCATE);
            r.flags=brlive::HIDE_RENDER|(g_playLiveDeleteCollision ? brlive::HIDE_COLLISION : 0);
            for (int a=0;a<3;++a) { r.mins[a]=h.mins[a]; r.maxs[a]=h.maxs[a]; }
            for (const PlayLivePlane& p:h.planes)
            {
                if (r.planeCount>=brlive::kMaxHidePlanes) break;
                float* pl=r.planes[r.planeCount++];
                pl[0]=p.n.x; pl[1]=p.n.y; pl[2]=p.n.z; pl[3]=p.d;
            }
        }
        for (const auto& pair:g_playBakedPrefabs)
        {
            const PlayBakedPrefab& bp=pair.second;
            if (!bp.hidden && !bp.fileChanged) continue;
            for (const PlayBakedPrefabHull& h:bp.hulls)
            {
                if (!bp.hidden && !h.stale) continue;   // the interior changed: only the brushes that differ now
                if (n>=brlive::kMaxHideRecords) break;
                brlive::HideRecord& r=g_playLiveShared->hides[n++];
                std::memset(&r,0,sizeof r);
                strncpy_s(r.sourceGuid,h.recordGuid.c_str(),_TRUNCATE);
                r.flags=brlive::HIDE_RENDER|(g_playLiveDeleteCollision ? brlive::HIDE_COLLISION : 0);
                for (int a=0;a<3;++a) { r.mins[a]=h.hull.mins[a]; r.maxs[a]=h.hull.maxs[a]; }
                for (const PlayLivePlane& p:h.hull.planes)
                {
                    if (r.planeCount>=brlive::kMaxHidePlanes) break;
                    float* pl=r.planes[r.planeCount++];
                    pl[0]=p.n.x; pl[1]=p.n.y; pl[2]=p.n.z; pl[3]=p.d;
                }
            }
        }
        g_playLiveShared->hideCount=n;
        // patches: one record per tile of at most 16 x 16 points, "<guid>#k"
        LONG np=0;
        auto putPatch=[&](const std::string& hguid, const PlayBakedPatch& g)
        {
            if (g.rows<2 || g.cols<2 || g.pts.size()!=static_cast<size_t>(g.rows)*static_cast<size_t>(g.cols)) return;
            const LONG side=brlive::kMaxPatchHideSide;
            const LONG tilesR=(g.rows-2)/(side-1)+1, tilesC=(g.cols-2)/(side-1)+1;
            LONG tile=0;
            for (LONG tr=0;tr<tilesR;++tr)
                for (LONG tc=0;tc<tilesC;++tc,++tile)
                {
                    if (np>=brlive::kMaxPatchHides) break;
                    const LONG r0=tr*(side-1), c0=tc*(side-1);
                    const LONG r1=(std::min)(g.rows-1,r0+side-1), c1=(std::min)(g.cols-1,c0+side-1);
                    if (r1<=r0 || c1<=c0) continue;
                    brlive::PatchHideRecord& r=g_playLiveShared->patchHides[np++];
                    std::memset(&r,0,sizeof r);
                    const std::string name=tilesR*tilesC>1 ? hguid+"#"+std::to_string(tile) : hguid;
                    strncpy_s(r.sourceGuid,name.c_str(),_TRUNCATE);
                    r.flags=brlive::HIDE_RENDER|(g_playLiveDeleteCollision ? brlive::HIDE_COLLISION : 0);
                    r.rows=r1-r0+1; r.cols=c1-c0+1;
                    LONG k=0;
                    for (LONG rr=r0;rr<=r1;++rr)
                        for (LONG cc=c0;cc<=c1;++cc,++k)
                        {
                            const PlayLiveVec3& p=g.pts[static_cast<size_t>(rr*g.cols+cc)];
                            r.points[k][0]=p.x; r.points[k][1]=p.y; r.points[k][2]=p.z;
                            for (int a=0;a<3;++a) { const float v=a==0?p.x:a==1?p.y:p.z; if (k==0) { r.mins[a]=r.maxs[a]=v; } else { r.mins[a]=(std::min)(r.mins[a],v); r.maxs[a]=(std::max)(r.maxs[a],v); } }
                        }
                }
        };
        for (const auto& pair:g_playLivePatchHides) { const PlayLivePatchHide& h=pair.second; if (h.active) putPatch(h.guid,h.grid); }
        for (const auto& pair:g_playBakedPrefabs)   // patches inside prefab instances: the whole interior when the instance moved, else the changed ones
        {
            const PlayBakedPrefab& bp=pair.second;
            if (!bp.hidden && !bp.fileChanged) continue;
            for (const PlayBakedPrefabPatch& pp:bp.patches) if (bp.hidden || pp.stale) putPatch(pp.recordGuid,pp.grid);
        }
        g_playLiveShared->patchHideCount=np;
        g_playLiveShared->hideGeneration=++g_playLiveHideGeneration;
        MemoryBarrier();
        InterlockedIncrement(&g_playLiveShared->hideSeq);
    }

    void PlayPublishModelHides()
    {
        if (!g_playLiveShared) return;
        InterlockedIncrement(&g_playLiveShared->modelHideSeq);
        MemoryBarrier();
        LONG n=0;
        auto put=[&](const char* guid, const std::string& model, const float origin[3], const float axis[9], float scale)
        {
            if (n>=brlive::kMaxModelHides) return;
            brlive::ModelHideRecord& r=g_playLiveShared->modelHides[n++];
            std::memset(&r,0,sizeof r);
            strncpy_s(r.entityGuid,guid,_TRUNCATE);
            strncpy_s(r.model,model.c_str(),_TRUNCATE);
            for (int a=0;a<3;++a) r.origin[a]=origin[a];
            for (int i=0;i<9;++i) r.axis[i]=axis[i];
            r.scale=scale;
            r.flags=brlive::HIDE_RENDER|(g_playLiveDeleteCollision ? brlive::HIDE_COLLISION : 0);
        };
        for (const auto& pair:g_playBakedModels)
        {
            const PlayBakedModel& m=pair.second;
            if (!m.hidden) continue;
            float ax[3][3]; PlayAnglesToAxis(m.angles,ax);
            put(m.entityGuid.c_str(),m.model,m.origin,&ax[0][0],m.scale);
        }
        for (const auto& pair:g_playBakedPrefabs)
        {
            const PlayBakedPrefab& bp=pair.second;
            if (!bp.hidden && !bp.fileChanged) continue;
            for (const PlayBakedPrefabModel& pm:bp.models) if (bp.hidden || pm.stale) put(pm.recordGuid.c_str(),pm.model,pm.origin,pm.axis,pm.scale);
        }
        g_playLiveShared->modelHideCount=n;
        g_playLiveShared->modelHideGeneration=++g_playModelHideGeneration;
        MemoryBarrier();
        InterlockedIncrement(&g_playLiveShared->modelHideSeq);
    }

    // VOLUME DECALS: a misc_volume_decal is drawn by the game from the level's GfxVolumeDecal records, so Live Edit's Radiant bounds brush is not it. Every decal of the saved map is cached with its compiled values:
    // one deleted or changed sends a protocol v26 VolDecalCompiled record with VOLDECAL_HIDE, every decal moved, changed or placed after the Play a VolDecalLive record the bridge draws with R_AddVolDecalDrawSurf.
    // A new decal borrows the reveal uv and runtime sort of a compiled revealAll decal with the same decalLayerSort (VOLDECAL_REF).
    struct PlayDecalParams
    {
        std::string material, sortName;
        float origin[3]={0,0,0}, angles[3]={0,0,0}, size[3]={0,0,0}, feather[3]={0,0,0}, uv[4]={0,0,1,1};
        bool revealAll=true;
    };
    struct PlayBakedDecal { std::string entityGuid; PlayDecalParams compiled; bool isNew=false; bool hidden=false; };
    std::map<std::string,PlayBakedDecal> g_playBakedDecals;     // key = PlayLiveGuidKey(entity guid)
    bool g_playLiveDecals = true;                // [playtest] live_decals
    LONG g_playDecalGeneration = 0;
    std::string g_playDecalPublished;            // the record bytes last published (republished when they change)
    brlive::SharedState* g_playDecalPublishedTo = nullptr;
    struct PlayLiveDecalWanted { std::string entityGuid; PlayDecalParams p; int ref=-1; };
    // Live decal materials: the game draws a volume decal only with its vd/<name> material, which the linker builds just for the decals of the last Play.
    // A material of the live list last published (read by PlayQueueDecalMaterials) that the compiled decals do not use is linked as a live zone line
    // "material,vd/<name>" (the linker builds the decal variant from the prefixed name).
    std::set<std::string> g_playDecalLiveMaterials;

    bool PlayDecalFromKvs(const std::vector<std::pair<std::string,std::string>>& kvs, PlayDecalParams& out, bool* decalClass=nullptr)
    {
        // keys compared without case: PlayParseEntityKvs lowercases them ("uvBaseAndScale" never matched otherwise)
        bool decal=false;
        float fx=0, fy=0, fz=0;
        for (const auto& kv:kvs)
        {
            const char* k=kv.first.c_str();
            if (!_stricmp(k,"classname")) decal=PlayLowerAscii(kv.second)=="misc_volume_decal";
            else if (!_stricmp(k,"modeloverridematerial")) out.material=kv.second;
            else if (!_stricmp(k,"decalLayerSort")) out.sortName=kv.second;
            else if (!_stricmp(k,"origin")) PlayParseVec3(kv.second,out.origin);
            else if (!_stricmp(k,"angles")) PlayParseVec3(kv.second,out.angles);
            else if (!_stricmp(k,"decalsize")) PlayParseVec3(kv.second,out.size);
            else if (!_stricmp(k,"edgeFeatherX")) fx=static_cast<float>(atof(kv.second.c_str()));
            else if (!_stricmp(k,"edgeFeatherY")) fy=static_cast<float>(atof(kv.second.c_str()));
            else if (!_stricmp(k,"edgeFeatherZ")) fz=static_cast<float>(atof(kv.second.c_str()));
            else if (!_stricmp(k,"uvBaseAndScale")) { float v[4]={0,0,0,0}; if (sscanf(kv.second.c_str(),"%f %f %f %f",&v[0],&v[1],&v[2],&v[3])==4) for (int i=0;i<4;++i) out.uv[i]=v[i]; }
            else if (!_stricmp(k,"revealDataSize")) out.revealAll=kv.second.empty() || _stricmp(kv.second.c_str(),"revealAll")==0;
        }
        auto clamp01=[](float v){ return v<=0.0f ? 0.0f : (v>=1.0f ? 1.0f : v); };
        out.feather[0]=1.0f-clamp01(fx); out.feather[1]=1.0f-clamp01(fy); out.feather[2]=1.0f-clamp01(fz);
        if (out.uv[0]==0.0f && out.uv[1]==0.0f && out.uv[2]==0.0f && out.uv[3]==0.0f) { out.uv[2]=1.0f; out.uv[3]=1.0f; }
        if (decalClass) *decalClass=decal;
        return decal && !out.material.empty();
    }
    bool PlayDecalSame(const PlayDecalParams& a, const PlayDecalParams& b)
    {
        if (_stricmp(a.material.c_str(),b.material.c_str())!=0 || a.sortName!=b.sortName || a.revealAll!=b.revealAll) return false;
        for (int i=0;i<3;++i)
        {
            if (std::fabs(a.origin[i]-b.origin[i])>0.05f || std::fabs(a.size[i]-b.size[i])>0.05f || std::fabs(a.feather[i]-b.feather[i])>0.001f) return false;
            float d=std::fmod(std::fabs(a.angles[i]-b.angles[i]),360.0f);
            if (d>180.0f) d=360.0f-d;
            if (d>0.05f) return false;
        }
        for (int i=0;i<4;++i) if (std::fabs(a.uv[i]-b.uv[i])>0.0001f) return false;
        return true;
    }

    // Every pass (500 ms, from the hide reconcile): compares each cached decal with Radiant's entity and publishes
    // the hide / reference / live lists when their bytes changed. Returns true when it published.
    bool PlayReconcileDecals(size_t& hiddenOut, size_t& liveOut)
    {
        hiddenOut=0; liveOut=0;
        if (!g_playLiveShared || !g_playBakedHullsLoaded) return false;
        if (!rp::Has(&rp::host->FindEntitiesByGuid) || !rp::host->FindEntitiesByGuid || !rp::Has(&rp::host->WriteEntityText) || !rp::host->WriteEntityText || !rp::Has(&rp::host->FreeString) || !rp::host->FreeString) return false;
        if (rp::Has(&rp::host->PrefabDepth) && rp::host->PrefabDepth && rp::host->PrefabDepth(rp::self)>0) return false;
        std::vector<PlayBakedDecal*> list;
        std::vector<const char*> guids;
        for (auto& pair:g_playBakedDecals) { list.push_back(&pair.second); guids.push_back(pair.second.entityGuid.c_str()); }
        std::vector<ModToolsEntity> entities(list.size(),nullptr);
        if (!list.empty()) rp::host->FindEntitiesByGuid(rp::self,guids.data(),guids.size(),entities.data());
        std::vector<PlayLiveDecalWanted> live;
        for (size_t i=0;i<list.size();++i)
        {
            PlayBakedDecal& d=*list[i];
            bool hide=false;
            if (!entities[i]) hide=true;
            else
            {
                char* text=rp::host->WriteEntityText(rp::self,entities[i]);
                if (!text) return false;   // unreadable this pass: keep what was published
                std::vector<std::pair<std::string,std::string>> kvs;
                PlayParseEntityKvs(text,kvs);
                rp::host->FreeString(rp::self,text);
                PlayDecalParams now;
                if (!PlayDecalFromKvs(kvs,now)) hide=true;
                else if (d.isNew || !PlayDecalSame(now,d.compiled))
                {
                    hide=true;
                    PlayLiveDecalWanted w; w.entityGuid=d.entityGuid; w.p=now;
                    live.push_back(std::move(w));
                }
            }
            if (d.isNew) hide=false;   // nothing compiled to hide
            d.hidden=hide;
            if (hide) ++hiddenOut;
        }
        // The compiled list: hidden decals first, then the references the live ones borrow from (a decal's own compiled
        // record when it was moved, else a compiled revealAll decal with the same layer sort, preferring the material).
        std::vector<std::pair<const PlayBakedDecal*,LONG>> compiled;   // decal, flags
        std::map<const PlayBakedDecal*,int> indexOf;
        auto addCompiled=[&](const PlayBakedDecal* d, LONG flags)->int
        {
            auto it=indexOf.find(d);
            if (it!=indexOf.end()) { compiled[it->second].second|=flags; return it->second; }
            if (compiled.size()>=(size_t)brlive::kMaxVolDecalCompiled) return -1;
            indexOf[d]=(int)compiled.size(); compiled.push_back({d,flags});
            return (int)compiled.size()-1;
        };
        for (const auto& pair:g_playBakedDecals) if (!pair.second.isNew && pair.second.hidden) addCompiled(&pair.second,brlive::VOLDECAL_HIDE);
        for (auto& w:live)
        {
            const PlayBakedDecal* own=nullptr;
            auto self=g_playBakedDecals.find(PlayLiveGuidKey(w.entityGuid.c_str()));
            if (self!=g_playBakedDecals.end() && !self->second.isNew && self->second.compiled.revealAll==w.p.revealAll) own=&self->second;
            const PlayBakedDecal* pick=own;
            if (!pick && w.p.revealAll)
            {
                for (const auto& pair:g_playBakedDecals)
                {
                    const PlayBakedDecal& c=pair.second;
                    if (c.isNew || !c.compiled.revealAll || c.compiled.sortName!=w.p.sortName) continue;
                    if (!pick || (_stricmp(c.compiled.material.c_str(),w.p.material.c_str())==0 && _stricmp(pick->compiled.material.c_str(),w.p.material.c_str())!=0)) pick=&c;
                }
            }
            if (pick) w.ref=addCompiled(pick,brlive::VOLDECAL_REF);
        }
        // the bytes, compared with what was published last
        std::vector<brlive::VolDecalCompiled> recC(compiled.size());
        for (size_t k=0;k<compiled.size();++k)
        {
            brlive::VolDecalCompiled& r=recC[k];
            std::memset(&r,0,sizeof r);
            const PlayBakedDecal& d=*compiled[k].first;
            strncpy_s(r.entityGuid,d.entityGuid.c_str(),_TRUNCATE);
            strncpy_s(r.material,d.compiled.material.c_str(),_TRUNCATE);
            float ax[3][3]; PlayAnglesToAxis(d.compiled.angles,ax);
            for (int a=0;a<3;++a) { r.origin[a]=d.compiled.origin[a]; r.halfExtents[a]=d.compiled.size[a]; }
            std::memcpy(r.axis,&ax[0][0],sizeof r.axis);
            r.flags=compiled[k].second|(d.compiled.revealAll ? brlive::VOLDECAL_REVEAL_ALL : 0);
        }
        std::vector<brlive::VolDecalLive> recL;
        for (const auto& w:live)
        {
            if (recL.size()>=(size_t)brlive::kMaxVolDecalLive) break;
            brlive::VolDecalLive r; std::memset(&r,0,sizeof r);
            strncpy_s(r.entityGuid,w.entityGuid.c_str(),_TRUNCATE);
            strncpy_s(r.material,w.p.material.c_str(),_TRUNCATE);
            float ax[3][3]; PlayAnglesToAxis(w.p.angles,ax);
            std::memcpy(r.axis,&ax[0][0],sizeof r.axis);
            for (int a=0;a<3;++a) { r.origin[a]=w.p.origin[a]; r.halfExtents[a]=w.p.size[a]; r.edgeFeather[a]=w.p.feather[a]; }
            for (int i=0;i<4;++i) r.uvBaseAndScale[i]=w.p.uv[i];
            r.refIndex=w.ref;
            r.flags=w.p.revealAll ? brlive::VOLDECAL_REVEAL_ALL : 0;
            recL.push_back(r);
        }
        liveOut=recL.size();
        std::string bytes;
        bytes.append(g_playLiveDecals ? "1" : "0");
        if (!recC.empty()) bytes.append(reinterpret_cast<const char*>(recC.data()),recC.size()*sizeof(brlive::VolDecalCompiled));
        bytes.append("|");
        if (!recL.empty()) bytes.append(reinterpret_cast<const char*>(recL.data()),recL.size()*sizeof(brlive::VolDecalLive));
        if (bytes==g_playDecalPublished && g_playDecalPublishedTo==g_playLiveShared) return false;
        brlive::SharedState* sh=g_playLiveShared;
        InterlockedIncrement(&sh->volDecalSeq);
        MemoryBarrier();
        for (size_t k=0;k<recC.size();++k) sh->volDecalCompiled[k]=recC[k];
        sh->volDecalCompiledCount=(LONG)recC.size();
        for (size_t k=0;k<recL.size();++k) sh->volDecalLive[k]=recL[k];
        sh->volDecalLiveCount=(LONG)recL.size();
        g_playDecalLiveMaterials.clear();
        for (const auto& w:live) if (!w.p.material.empty()) g_playDecalLiveMaterials.insert(PlayLowerAscii(w.p.material));
        sh->volDecalEnabled=g_playLiveDecals ? 1 : 0;
        sh->volDecalGeneration=++g_playDecalGeneration;
        MemoryBarrier();
        InterlockedIncrement(&sh->volDecalSeq);
        const bool firstToThisGame=g_playDecalPublishedTo!=g_playLiveShared;
        g_playDecalPublished.swap(bytes);
        g_playDecalPublishedTo=g_playLiveShared;
        if (!firstToThisGame || !recC.empty() || !recL.empty())
        {
            size_t refs=0; for (const auto& c:compiled) if (!(c.second&brlive::VOLDECAL_HIDE)) ++refs;
            char line[320]={};
            snprintf(line,sizeof line,"[LIVE DECAL] generation %ld: %u compiled decal(s) hidden in the game, %u drawn live (moved, changed or new), %u reference(s) for new decals",
                (long)g_playDecalGeneration,(unsigned)(compiled.size()-refs),(unsigned)recL.size(),(unsigned)refs);
            PlayAppendLog(line);
        }
        return true;
    }

    // From the live material tick: once the bridge has applied the current live decal list and still reports decals
    // without a vd/ material, every material of that list the compiled decals do not use goes into the next live zone
    // as "material,vd/<name>" (each once per map and session, like the brush materials).
    void PlayQueueDecalMaterials(int& added, std::string& addedNames)
    {
        if (!g_playLiveShared || !g_playLiveDecals || !g_playLiveMaterialLoad || g_playDecalLiveMaterials.empty()) return;
        if (g_playLiveShared->volDecalMissing <= 0 || g_playLiveShared->volDecalAppliedGeneration != g_playDecalGeneration) return;
        std::set<std::string> compiled;
        for (const auto& pair:g_playBakedDecals) if (!pair.second.isNew) compiled.insert(PlayLowerAscii(pair.second.compiled.material));
        for (const std::string& name:g_playDecalLiveMaterials)
        {
            if (compiled.count(name) || name[0]=='$' || name.find_first_of(" \t,\"/")!=std::string::npos) continue;
            const std::string key=g_playCurrentMap+"|vd/"+name;
            if (g_playLiveMaterialsQueued.count(key)) continue;
            g_playLiveMaterialsQueued.insert(key);
            g_playLiveMatPending.push_back("material,vd/"+name);
            ++added; if (!addedNames.empty()) addedNames+=", "; addedNames+="vd/"+name;
        }
    }

    // LIVE FX: an "fx" entity is a client RadiantFX (cg_fx.cpp: spawned from the BSP's entity string at level start, played with CG_CreateFX, guid = first 8 hex digits of the entity GUID like lights).
    // Every fx of the saved map is cached with its compiled values, one moved, changed, deleted or placed after the Play goes out as a LiveFx record (BO3LiveShared.h). The bridge (bridge/livefx.cpp) stops the compiled effect and plays the record's with CG_CreateFX.
    // A missing effect (LIVEFX_MISSING) is linked as a live zone ("fx,<name>") like a missing xmodel and goes into the patch zone for the next Play. Values as CG_GetFXSpawnParams reads them.
    struct PlayFxParams
    {
        std::string fxdef;
        float origin[3]={0,0,0}, angles[3]={0,0,0};
        long delay=0;
        LONG stateMask=0xF;
        bool outdoor=false, exploder=false;
    };
    struct PlayBakedFx { std::string entityGuid; PlayFxParams compiled; bool isNew=false; };
    std::map<std::string,PlayBakedFx> g_playBakedFx;            // key = PlayLiveGuidKey(entity guid)
    bool g_playLiveFx = true;                    // [playtest] live_fx
    LONG g_playFxGeneration = 0;
    std::string g_playFxPublished;               // the record bytes last published (republished when they change)
    brlive::SharedState* g_playFxPublishedTo = nullptr;
    std::vector<std::string> g_playFxPublishedNames;   // fxdef per published record (for the bridge's answers)
    std::set<std::string> g_playFxQueued;        // "<map>|fx,<name>" already queued for a live zone this session
    size_t g_playFxChanged = 0, g_playFxNew = 0, g_playFxRemoved = 0;

    bool PlayFxFromKvs(const std::vector<std::pair<std::string,std::string>>& kvs, PlayFxParams& out, bool* fxClass=nullptr)
    {
        bool fx=false;
        LONG mask=0xF;
        for (const auto& kv:kvs)
        {
            const char* k=kv.first.c_str();
            if (!_stricmp(k,"classname")) fx=PlayLowerAscii(kv.second)=="fx";
            else if (!_stricmp(k,"fxdef")) out.fxdef=kv.second;
            else if (!_stricmp(k,"origin")) PlayParseVec3(kv.second,out.origin);
            else if (!_stricmp(k,"angles")) PlayParseVec3(kv.second,out.angles);
            else if (!_stricmp(k,"delay")) out.delay=atol(kv.second.c_str());
            else if (!_stricmp(k,"outdoor")) out.outdoor=atoi(kv.second.c_str())!=0;
            else if (!_stricmp(k,"exploder_count")) out.exploder=atoi(kv.second.c_str())>0;
            else if (!_strnicmp(k,"fxstate",7) && k[7]>='1' && k[7]<='4' && !k[8]) { if (atoi(kv.second.c_str())) mask|=1<<(k[7]-'1'); else mask&=~(1<<(k[7]-'1')); }
        }
        out.stateMask=mask;
        if (fxClass) *fxClass=fx;
        return fx && !out.fxdef.empty();
    }
    bool PlayFxSame(const PlayFxParams& a, const PlayFxParams& b)
    {
        if (_stricmp(a.fxdef.c_str(),b.fxdef.c_str())!=0 || a.delay!=b.delay || a.stateMask!=b.stateMask || a.outdoor!=b.outdoor || a.exploder!=b.exploder) return false;
        for (int i=0;i<3;++i)
        {
            if (std::fabs(a.origin[i]-b.origin[i])>0.05f) return false;
            float d=std::fmod(std::fabs(a.angles[i]-b.angles[i]),360.0f);
            if (d>180.0f) d=360.0f-d;
            if (d>0.05f) return false;
        }
        return true;
    }
    // RadiantFX.guid: CG_GetFXSpawnParams scans the BSP's "guid" key ("6E514C08", the GUID's first 8 hex digits) with %X
    uint32_t PlayFxGuid32(const std::string& entityGuid)
    {
        const char* p=entityGuid.c_str();
        while (*p=='{' || *p==' ') ++p;
        char hex[9]={}; size_t n=0;
        for (;n<8 && isxdigit(static_cast<unsigned char>(p[n]));++n) hex[n]=p[n];
        return n==8 ? static_cast<uint32_t>(strtoul(hex,nullptr,16)) : 0u;
    }

    // Every pass (500 ms, from the hide reconcile): compares each cached fx with Radiant's entity, publishes the list
    // when its bytes changed, and queues the effects the bridge reports missing. Returns true when it published.
    bool PlayReconcileFx(uint64_t now)
    {
        if (!g_playLiveShared || !g_playBakedHullsLoaded) return false;
        if (!rp::Has(&rp::host->FindEntitiesByGuid) || !rp::host->FindEntitiesByGuid || !rp::Has(&rp::host->WriteEntityText) || !rp::host->WriteEntityText || !rp::Has(&rp::host->FreeString) || !rp::host->FreeString) return false;
        brlive::SharedState* sh=g_playLiveShared;
        // 1. the bridge's answers for what was published last: a missing effect is linked as a live zone
        if (g_playFxPublishedTo==sh && sh->liveFxAppliedGeneration==g_playFxGeneration)
        {
            for (size_t i=0;i<g_playFxPublishedNames.size() && i<(size_t)brlive::kMaxLiveFx;++i)
            {
                if (sh->liveFxResult[i]!=brlive::LIVEFX_MISSING) continue;
                const std::string& name=g_playFxPublishedNames[i];
                if (name.empty() || name.find_first_of(" \t\"")!=std::string::npos) continue;
                const std::string key=g_playCurrentMap+"|fx,"+name;
                if (g_playFxQueued.count(key)) continue;
                g_playFxQueued.insert(key);
                PlayMapInfo m; std::string error;
                if (!PlayResolveCurrentMap(m,error)) { PlayAppendLog("[LIVE FX] "+name+" is not in the level and the map could not be resolved: "+error); continue; }
                if (g_playLiveZone) PlayAddLiveMaterial(m,"fx,"+name);
                if (g_playLiveMaterialLoad && g_playAutoLinkMaterials)
                {
                    g_playLiveMatPending.push_back("fx,"+name);
                    if (!g_playLiveMaterialsDueMs) g_playLiveMaterialsDueMs=now+3000;
                    PlayAppendLog("[LIVE FX] the level has no effect "+name+": linking it as a live zone in a few seconds (and into the patch zone for the next Play).");
                }
                else PlayAppendLog("[LIVE FX] the level has no effect "+name+": in the patch zone from the next Play on (live_material_load=0 or auto_link_materials=0).");
            }
        }
        if (rp::Has(&rp::host->PrefabDepth) && rp::host->PrefabDepth && rp::host->PrefabDepth(rp::self)>0) return false;
        // 2. the records
        std::vector<PlayBakedFx*> list;
        std::vector<const char*> guids;
        for (auto& pair:g_playBakedFx) { list.push_back(&pair.second); guids.push_back(pair.second.entityGuid.c_str()); }
        std::vector<ModToolsEntity> entities(list.size(),nullptr);
        if (!list.empty()) rp::host->FindEntitiesByGuid(rp::self,guids.data(),guids.size(),entities.data());
        std::vector<brlive::LiveFx> recs;
        std::vector<std::string> names;
        size_t changed=0, added=0, removed=0;
        for (size_t i=0;i<list.size();++i)
        {
            const PlayBakedFx& f=*list[i];
            PlayFxParams cur; bool gone=false;
            if (!entities[i]) gone=true;
            else
            {
                char* text=rp::host->WriteEntityText(rp::self,entities[i]);
                if (!text) return false;   // unreadable this pass: keep what was published
                std::vector<std::pair<std::string,std::string>> kvs;
                PlayParseEntityKvs(text,kvs);
                rp::host->FreeString(rp::self,text);
                bool fxClass=false;
                const bool ok=PlayFxFromKvs(kvs,cur,&fxClass);
                if (!fxClass) gone=true;                          // no longer an fx entity
                else if (!ok) { if (f.isNew) continue; gone=true; }   // no fxdef (yet): nothing to play, the compiled one stops
            }
            if (gone && f.isNew) continue;
            if (!gone && !f.isNew && PlayFxSame(cur,f.compiled)) continue;
            if (recs.size()>=(size_t)brlive::kMaxLiveFx) break;
            brlive::LiveFx r; std::memset(&r,0,sizeof r);
            strncpy_s(r.entityGuid,f.entityGuid.c_str(),_TRUNCATE);
            r.guid=PlayFxGuid32(f.entityGuid);
            const PlayFxParams& p=gone ? f.compiled : cur;
            strncpy_s(r.fxdef,p.fxdef.c_str(),_TRUNCATE);
            for (int a=0;a<3;++a) { r.origin[a]=p.origin[a]; r.angles[a]=p.angles[a]; }
            r.delay=(LONG)p.delay; r.stateMask=p.stateMask; r.outdoor=p.outdoor ? 1 : 0;
            r.flags=(gone ? brlive::LIVEFX_REMOVED : 0)|(f.isNew ? brlive::LIVEFX_NEW : 0)|(p.exploder ? brlive::LIVEFX_EXPLODER : 0);
            if (gone) ++removed; else if (f.isNew) ++added; else ++changed;
            recs.push_back(r);
            names.push_back(gone ? std::string() : p.fxdef);
        }
        g_playFxChanged=changed; g_playFxNew=added; g_playFxRemoved=removed;
        std::string bytes;
        bytes.append(g_playLiveFx ? "1" : "0");
        if (!recs.empty()) bytes.append(reinterpret_cast<const char*>(recs.data()),recs.size()*sizeof(brlive::LiveFx));
        if (bytes==g_playFxPublished && g_playFxPublishedTo==sh) return false;
        InterlockedIncrement(&sh->liveFxSeq);
        MemoryBarrier();
        for (size_t k=0;k<recs.size();++k) sh->liveFx[k]=recs[k];
        sh->liveFxCount=(LONG)recs.size();
        sh->liveFxEnabled=g_playLiveFx ? 1 : 0;
        sh->liveFxGeneration=++g_playFxGeneration;
        MemoryBarrier();
        InterlockedIncrement(&sh->liveFxSeq);
        const bool firstToThisGame=g_playFxPublishedTo!=sh;
        g_playFxPublished.swap(bytes);
        g_playFxPublishedTo=sh;
        g_playFxPublishedNames.swap(names);
        if (!firstToThisGame || !recs.empty())
        {
            char line[320]={};
            snprintf(line,sizeof line,"[LIVE FX] generation %ld: %u fx moved or changed, %u placed after the Play, %u deleted (of %u cached)",
                (long)g_playFxGeneration,(unsigned)changed,(unsigned)added,(unsigned)removed,(unsigned)g_playBakedFx.size());
            PlayAppendLog(line);
        }
        return true;
    }

    // LIVE SKY: the level draws the sky of the ssi each sun volume names per lighting state (volume_sun "ssi1".."ssi4" or "ssi", worldspawn's "ssi" without a volume_sun), rotation and size from the sky material's skyRotation / skySize (the ssi's skyboxmodel, its one material).
    // When every sun key resolves to one sky the bridge draws it whatever the level compiled ("" = any), with several skies each (entity, state) whose sky changed since the Play sends compiled -> new.
    // A sky the level has not loaded is linked as a live zone ("xmodel,<sky>").
    struct PlaySunKeys { std::string entityGuid; bool volume=false; std::map<std::string,std::string> ssi; };   // lower-case key -> ssi name
    std::vector<PlaySunKeys> g_playBakedSuns;
    bool g_playLiveSky = true;                   // [playtest] live_sky
    LONG g_playSkyGeneration = 0;
    std::string g_playSkyPublished;
    brlive::SharedState* g_playSkyPublishedTo = nullptr;
    std::vector<std::string> g_playSkyPublishedNames;   // "to" per published record
    std::set<std::string> g_playSkyQueued;       // "<map>|xmodel,<sky>" already queued this session
    // an ssi's values as the linker turns them into GfxGlobalLightSettings (linker 0x7FF64C164DF0): color through the sRGB
    // curve, intensity = 2^stops (0 with enablesun 0), specScale = 2^spec_comp clamped to [0, 65504]
    struct PlaySsiValues
    {
        bool known=false; std::string sky;
        float pitch=0, yaw=0, color[3]={1,1,1}, intensity=0, specScale=1, penumbra=0, evcmp=0, evmin=0, evmax=0;
        bool shadow=true, dynamicShadow=true;
    };
    std::map<std::string,PlaySsiValues> g_playSsiSky;   // lower-case ssi -> its values (known=false: no such ssi)
    bool g_playLiveSkyBake = true;               // [playtest] live_sky_bake: Rebuild All in Radiant after an ssi change, live bake sends it
    std::string g_playSkyBakedKey;               // the sun keys the game's (or the last started) bake used
    bool g_playSkyBakedKeySet = false;
    std::string g_playSkyBakeWantKey;            // the sun keys a pending bake is for
    uint64_t g_playSkyBakeDueMs = 0;             // start the bake at (0: none pending)
    std::string g_playSkyBakeWait;               // why the pending bake waits (logged once per reason)
    struct PlaySkyValues { bool known=false; float rotation=0, size=0; };
    std::map<std::string,PlaySkyValues> g_playSkyValues;   // lower-case sky xmodel -> its material's values
    std::string g_playSkyLine;                   // what the last publish asked for (live status)

    bool PlaySunEntityFromKvs(const std::vector<std::pair<std::string,std::string>>& kvs, PlaySunKeys& out)
    {
        bool world=false, volume=false;
        for (const auto& kv:kvs)
        {
            const char* k=kv.first.c_str();
            if (!_stricmp(k,"classname")) { const std::string c=PlayLowerAscii(kv.second); world=c=="worldspawn"; volume=c=="volume_sun"; }
            else if ((!_stricmp(k,"ssi") || (!_strnicmp(k,"ssi",3) && k[3]>='1' && k[3]<='4' && !k[4])) && !kv.second.empty()) out.ssi[PlayLowerAscii(kv.first)]=kv.second;
        }
        out.volume=volume;
        return world || volume;
    }
    // one-shot query on gdt.db (Radiant's sqlite64r.dll, opened by the material lookup): the first row's columns
    bool PlayGdtQueryRow(const char* sql, const std::string& name, int columns, std::vector<std::string>& out)
    {
        out.clear();
        if (!PlayGdtSqlReady()) return false;
        PlayGdtSql& q=g_playGdtSql;
        void* st=nullptr;
        if (q.prepare_v2(q.db,sql,-1,&st,nullptr)!=0 || !st) return false;
        bool row=false;
        if (q.bind_text(st,1,name.c_str(),-1,reinterpret_cast<void(*)(void*)>(-1) /* SQLITE_TRANSIENT */)==0 && q.step(st)==100 /* SQLITE_ROW */)
        {
            row=true;
            for (int i=0;i<columns;++i) { const unsigned char* t=q.column_text(st,i); out.push_back(t ? reinterpret_cast<const char*>(t) : ""); }
        }
        q.finalize(st);
        return row;
    }
    float PlaySrgbToLinear(float v) { return v>0.040449999f ? std::pow((v+0.055f)/1.0549999f,2.4000001f) : v/12.92f; }
    const PlaySsiValues& PlaySsiValuesOf(const std::string& ssi)
    {
        static const PlaySsiValues none;
        if (ssi.empty()) return none;
        const std::string key=PlayLowerAscii(ssi);
        auto found=g_playSsiSky.find(key);
        if (found!=g_playSsiSky.end()) return found->second;
        std::vector<std::string> row;
        PlaySsiValues v;
        if (PlayGdtQueryRow("SELECT skyboxmodel, pitch, yaw, colorSRGB, stops, enablesun, spec_comp, penumbra_inches, dynamicShadow, evcmp, evmin, evmax FROM ssi WHERE _name=?1 LIMIT 1",ssi,12,row))
        {
            v.known=true;
            v.sky=row[0];
            while (!v.sky.empty() && isspace(static_cast<unsigned char>(v.sky.back()))) v.sky.pop_back();
            if (v.sky.find_first_of(" \t\"")!=std::string::npos) v.sky.clear();
            auto f=[&](int i){ return static_cast<float>(atof(row[i].c_str())); };
            auto on=[&](int i){ return row[i].empty() || row[i][0]=='1'; };   // the linker: absent = 1, else "1"
            v.pitch=f(1); v.yaw=f(2);
            float srgb[3]={1,1,1};
            if (!row[3].empty()) sscanf_s(row[3].c_str(),"%f %f %f",&srgb[0],&srgb[1],&srgb[2]);
            for (int i=0;i<3;++i) v.color[i]=PlaySrgbToLinear(srgb[i]);
            v.shadow=on(5);
            v.intensity=v.shadow ? std::pow(2.0f,f(4)) : 0.0f;
            v.specScale=std::pow(2.0f,f(6)); if (v.specScale>=65504.0f) v.specScale=65504.0f; else if (v.specScale<0.0f) v.specScale=0.0f;
            v.penumbra=f(7); v.dynamicShadow=on(8);
            v.evcmp=f(9); v.evmin=f(10); v.evmax=f(11);
        }
        if (!v.known || v.sky.empty()) PlayAppendLog("[LIVE SKY] ssi "+ssi+" is not in gdt.db or names no skybox model");
        return g_playSsiSky[key]=v;
    }
    std::string PlaySkyOfSsi(const std::string& ssi) { return PlaySsiValuesOf(ssi).sky; }

    // skyRotation / skySize of the sky's material: skinOverride's replacement of its one exported material (stock skies
    // export mtl_skybox_default), else "mtl_<sky>"
    PlaySkyValues PlaySkyValuesOf(const std::string& sky)
    {
        const std::string key=PlayLowerAscii(sky);
        auto found=g_playSkyValues.find(key);
        if (found!=g_playSkyValues.end()) return found->second;
        std::vector<std::string> candidates, row;
        if (PlayGdtQueryRow("SELECT skinOverride FROM xmodel WHERE _name=?1 LIMIT 1",sky,1,row))
        {
            std::string s=row[0];
            for (size_t p; (p=s.find("\\r"))!=std::string::npos || (p=s.find("\\n"))!=std::string::npos;) s.replace(p,2," ");
            std::istringstream in(s); std::string a, b;
            if (in>>a>>b) candidates.push_back(b);
        }
        candidates.push_back("mtl_"+sky);
        PlaySkyValues v;
        for (const auto& m:candidates)
        {
            if (!PlayGdtQueryRow("SELECT skyRotation, skySize FROM material WHERE _name=?1 LIMIT 1",m,2,row)) continue;
            v.known=true;
            v.rotation=row[0].empty() ? 0.0f : static_cast<float>(atof(row[0].c_str()));
            v.size=row[1].empty() ? 8000.0f : static_cast<float>(atof(row[1].c_str()));   // the linker's default
            break;
        }
        g_playSkyValues[key]=v;
        if (!v.known) PlayAppendLog("[LIVE SKY] no material values for "+sky+": the level keeps its compiled sky rotation and size");
        return v;
    }
    // the ssi per lighting state (1..4) of a sun entity: ssiN, else "ssi" for state 1
    std::map<int,std::string> PlaySunStates(const std::map<std::string,std::string>& ssi)
    {
        std::map<int,std::string> states;
        for (int n=1;n<=4;++n) { auto f=ssi.find(std::string("ssi")+char('0'+n)); if (f!=ssi.end()) states[n]=f->second; }
        if (states.empty()) { auto f=ssi.find("ssi"); if (f!=ssi.end()) states[1]=f->second; }
        return states;
    }

    // An entity's text keys straight from its key map (entity_t +304, std::map<std::string,QVariant>, MSVC 2012 layout: head +0, size +8, node left +0, parent +8, right +16, isnil +25, key +32,
    // value +64), the host's hooks::EntityKeyValue walk. Entity_Write would serialize every world brush of worldspawn on each pass.
    // Only QString values (type 10), only the keys "want" accepts.
    constexpr size_t kPlayEntityKeyValues = 304, kPlayEntityClass = 240;
    bool PlayReadStdString(const unsigned char* s, std::string& out)
    {
        const size_t size=*reinterpret_cast<const size_t*>(s+16), cap=*reinterpret_cast<const size_t*>(s+24);
        if (size>8192 || cap<size) return false;
        const char* p=cap<16 ? reinterpret_cast<const char*>(s) : *reinterpret_cast<const char* const*>(s);
        if (!p) return false;
        out.assign(p,size);
        return true;
    }
    bool PlayReadQStringVariant(const unsigned char* var, std::string& out)
    {
        const uint32_t flags=*reinterpret_cast<const uint32_t*>(var+8);
        if ((flags&0x3fffffffu)!=10u) return false;
        const unsigned char* d=*reinterpret_cast<const unsigned char* const*>(var);
        if (flags&0x40000000u) { if (!d) return false; d=*reinterpret_cast<const unsigned char* const*>(d); if (!d) return false; d=*reinterpret_cast<const unsigned char* const*>(d); }
        if (!d) return false;
        const int size=*reinterpret_cast<const int*>(d+4); const ptrdiff_t off=*reinterpret_cast<const ptrdiff_t*>(d+16);
        if (size<0 || size>65536 || off<16 || off>4096) return false;
        const wchar_t* w=reinterpret_cast<const wchar_t*>(d+off);
        const int n=WideCharToMultiByte(CP_UTF8,0,w,size,nullptr,0,nullptr,nullptr);
        out.assign(size_t(n>0 ? n : 0),'\0');
        if (n>0) WideCharToMultiByte(CP_UTF8,0,w,size,&out[0],n,nullptr,nullptr);
        return true;
    }
    bool PlayEntityKeysWalk(const unsigned char* ent, bool (*want)(const std::string&), std::vector<std::pair<std::string,std::string>>* out)
    {
        const unsigned char* head=*reinterpret_cast<const unsigned char* const*>(ent+kPlayEntityKeyValues);
        const size_t size=*reinterpret_cast<const size_t*>(ent+kPlayEntityKeyValues+8);
        if (!head || size>4096 || head[25]!=1) return false;
        // "classname" is not a key: the writer takes it from the entity class (entity_t +240, EClass name std::string +0).
        if (want("classname"))
        {
            const unsigned char* eclass=*reinterpret_cast<const unsigned char* const*>(ent+kPlayEntityClass);
            std::string name;
            if (eclass && PlayReadStdString(eclass,name) && !name.empty()) out->push_back({"classname",name});
        }
        const unsigned char* node=*reinterpret_cast<const unsigned char* const*>(head);   // leftmost
        for (size_t n=0;node && node!=head && node[25]==0 && n<=size;++n)
        {
            std::string k, v;
            if (PlayReadStdString(node+32,k) && want(k) && PlayReadQStringVariant(node+64,v)) out->push_back({k,v});
            const unsigned char* right=*reinterpret_cast<const unsigned char* const*>(node+16);
            if (right && right[25]==0)
            {
                node=right;
                for (const unsigned char* l=*reinterpret_cast<const unsigned char* const*>(node); l && l[25]==0; l=*reinterpret_cast<const unsigned char* const*>(node)) node=l;
            }
            else
            {
                const unsigned char* parent=*reinterpret_cast<const unsigned char* const*>(node+8);
                while (parent && parent[25]==0 && node==*reinterpret_cast<const unsigned char* const*>(parent+16)) { node=parent; parent=*reinterpret_cast<const unsigned char* const*>(node+8); }
                node=parent;
            }
        }
        return true;
    }
    bool PlayEntityKeysSeh(const unsigned char* ent, bool (*want)(const std::string&), std::vector<std::pair<std::string,std::string>>* out)
    {
        __try { return PlayEntityKeysWalk(ent,want,out); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool PlaySunKeyWanted(const std::string& k) { return !_stricmp(k.c_str(),"classname") || !_strnicmp(k.c_str(),"ssi",3); }

    // Radiant's light build (the toolbar bolt's Rebuild All, CCamWnd 0x1402F31D0(cam, sun, probes)), started for an ssi change while the game runs.
    // It waits for a running or paused bake (cancel then rebuild within seconds is a suspected crash trap) and for real lighting in the 3D view (the bake only steps there). PlayLiveBakeTick sends the finished bake to the game.
    void PlaySkyBakeWaiting(const std::string& why)
    {
        if (g_playSkyBakeWait==why) return;
        g_playSkyBakeWait=why;
        PlayAppendLog("[LIVE SKY] the light bake for the new ssi waits: "+why);
    }
    int PlayCallBuildLightingSeh(void* fn, void* cam)
    {
        __try { reinterpret_cast<void (__fastcall*)(void*, uint8_t, uint8_t)>(fn)(cam,1,1); return 1; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    void PlaySkyBakeTick(uint64_t now)
    {
        if (!g_playSkyBakeDueMs || now<g_playSkyBakeDueMs) return;
        if (!g_playLiveBake || !g_playExportLighting) { g_playSkyBakeDueMs=0; PlayAppendLog("[LIVE SKY] ssi changed: no light bake started (live_bake=0 or export_lighting=0); the game keeps its baked lighting"); return; }
        if (g_playState!=PLAY_RUNNING) { PlaySkyBakeWaiting("Play Test is busy"); return; }
        bool baking=false, unexported=false;
        if (!PlayLightBakeStates(baking,unexported)) { PlaySkyBakeWaiting("Radiant's light state is not readable"); return; }
        if (baking) { PlaySkyBakeWaiting("a light bake is running or paused in Radiant (the sky bake starts after it)"); return; }
        void** mainWinPtr=static_cast<void**>(rp::host->RadiantAddress(rp::self,0x1444C6560));   // RadiantMainWin*
        void* build=rp::host->RadiantAddress(rp::self,0x1402F31D0);                              // CCamWnd build lighting
        char* cam=nullptr;
        if (mainWinPtr && *mainWinPtr) { char* qe=*reinterpret_cast<char**>(static_cast<char*>(*mainWinPtr)+64); cam=qe ? *reinterpret_cast<char**>(qe+240) : nullptr; }
        if (!build || !cam) { g_playSkyBakeDueMs=0; PlayAppendLog("[LIVE SKY] ssi changed: Radiant's light build is not available; bake by hand to update the game's baked lighting"); return; }
        if (*reinterpret_cast<int*>(cam+232)!=4) { PlaySkyBakeWaiting("turn on real lighting in the 3D view (the bake only runs there)"); return; }
        if (!PlayCallBuildLightingSeh(build,cam)) { g_playSkyBakeDueMs=0; PlayAppendLog("[LIVE SKY] Radiant's light build raised an exception; not retried"); return; }
        g_playSkyBakeDueMs=0; g_playSkyBakeWait.clear();
        g_playSkyBakedKey=g_playSkyBakeWantKey;
        PlayAppendLog("[LIVE SKY] ssi changed: started Rebuild All in Radiant; the live bake sends the new probes, volumes and sun shadows to the game when it finishes");
    }

    // Every pass (500 ms, from the hide reconcile): reads the sun entities' ssi keys, publishes the sky records when
    // their bytes changed, and queues the skies the bridge reports missing. Returns true when it published.
    bool PlayReconcileSky(uint64_t now)
    {
        if (!g_playLiveShared || !g_playBakedHullsLoaded) return false;
        if (!rp::Has(&rp::host->FindEntitiesByGuid) || !rp::host->FindEntitiesByGuid) return false;
        brlive::SharedState* sh=g_playLiveShared;
        // 1. the bridge's answers: a sky the level lacks is linked as a live zone
        if (g_playSkyPublishedTo==sh && sh->liveSkyAppliedGeneration==g_playSkyGeneration)
        {
            for (size_t i=0;i<g_playSkyPublishedNames.size() && i<(size_t)brlive::kMaxLiveSkies;++i)
            {
                if (sh->liveSkyResult[i]!=brlive::LIVESKY_MISSING) continue;
                const std::string& name=g_playSkyPublishedNames[i];
                if (name.empty() || name.find_first_of(" \t\"")!=std::string::npos) continue;
                const std::string key=g_playCurrentMap+"|xmodel,"+name;
                if (g_playSkyQueued.count(key)) continue;
                g_playSkyQueued.insert(key);
                PlayMapInfo m; std::string error;
                if (!PlayResolveCurrentMap(m,error)) { PlayAppendLog("[LIVE SKY] "+name+" is not in the level and the map could not be resolved: "+error); continue; }
                if (g_playLiveZone) PlayAddLiveMaterial(m,"xmodel,"+name);
                if (g_playLiveMaterialLoad && g_playAutoLinkMaterials)
                {
                    g_playLiveMatPending.push_back("xmodel,"+name);
                    if (!g_playLiveMaterialsDueMs) g_playLiveMaterialsDueMs=now+3000;
                    PlayAppendLog("[LIVE SKY] the level has no sky model "+name+": linking it as a live zone in a few seconds (and into the patch zone for the next Play).");
                }
                else PlayAppendLog("[LIVE SKY] the level has no sky model "+name+": in the patch zone from the next Play on (live_material_load=0 or auto_link_materials=0).");
            }
        }
        if (rp::Has(&rp::host->PrefabDepth) && rp::host->PrefabDepth && rp::host->PrefabDepth(rp::self)>0) return false;
        // 2. the sun entities now (a map with a volume_sun ignores worldspawn's ssi)
        bool anyVolume=false;
        for (const auto& s:g_playBakedSuns) anyVolume=anyVolume || s.volume;
        std::vector<const PlaySunKeys*> suns;
        std::vector<const char*> guids;
        for (const auto& s:g_playBakedSuns) if (s.volume || !anyVolume) { suns.push_back(&s); guids.push_back(s.entityGuid.c_str()); }
        std::vector<ModToolsEntity> entities(suns.size(),nullptr);
        if (!suns.empty()) rp::host->FindEntitiesByGuid(rp::self,guids.data(),guids.size(),entities.data());
        std::set<std::string> ssisNow;                   // lower case, only ssis that name a sky
        std::map<std::string,std::string> ssiName;       // lower case -> as written
        std::vector<std::pair<std::string,std::string>> changes;   // (compiled ssi, new ssi) per (entity, state) whose ssi changed
        std::string baseKey, curKey;                     // the sun keys at the Play and now (for the bake)
        for (size_t i=0;i<suns.size();++i)
        {
            const std::map<int,std::string> base=PlaySunStates(suns[i]->ssi);
            for (const auto& st:base) baseKey+=suns[i]->entityGuid+"|"+std::to_string(st.first)+"|"+PlayLowerAscii(st.second)+";";
            if (!entities[i]) continue;   // deleted: its sky no longer counts
            std::vector<std::pair<std::string,std::string>> kvs;
            if (!PlayEntityKeysSeh(static_cast<const unsigned char*>(entities[i]),&PlaySunKeyWanted,&kvs)) return false;   // unreadable this pass: keep what was published
            PlaySunKeys cur;
            if (!PlaySunEntityFromKvs(kvs,cur)) continue;
            const std::map<int,std::string> now_=PlaySunStates(cur.ssi);
            for (const auto& st:now_)
            {
                curKey+=suns[i]->entityGuid+"|"+std::to_string(st.first)+"|"+PlayLowerAscii(st.second)+";";
                if (PlaySkyOfSsi(st.second).empty()) continue;
                ssisNow.insert(PlayLowerAscii(st.second)); ssiName[PlayLowerAscii(st.second)]=st.second;
                auto b=base.find(st.first);
                if (b!=base.end() && _stricmp(b->second.c_str(),st.second.c_str())!=0 && !PlaySkyOfSsi(b->second).empty()) changes.push_back({b->second,st.second});
            }
        }
        // the bake: the game's probes, probe volumes and sun shadow tree were baked with the Play's ssis, so after a change Radiant rebuilds its lighting (3 s after the last change) and the live bake sends the result
        if (!g_playSkyBakedKeySet) { g_playSkyBakedKey=baseKey; g_playSkyBakedKeySet=true; }
        if (curKey!=g_playSkyBakeWantKey)
        {
            g_playSkyBakeWantKey=curKey;
            g_playSkyBakeDueMs=(g_playLiveSkyBake && curKey!=g_playSkyBakedKey) ? now+3000 : 0;
            g_playSkyBakeWait.clear();
        }
        PlaySkyBakeTick(now);
        // 3. the records
        std::vector<brlive::LiveSky> recs;
        std::vector<std::string> names;
        auto add=[&](const std::string& fromSky, const std::string& ssi, bool force)
        {
            if (recs.size()>=(size_t)brlive::kMaxLiveSkies) return;
            for (const auto& r:recs) if (!_stricmp(r.from,fromSky.c_str())) return;   // the first change of a compiled sky wins
            const PlaySsiValues& v=PlaySsiValuesOf(ssi);
            brlive::LiveSky r; std::memset(&r,0,sizeof r);
            strncpy_s(r.from,fromSky.c_str(),_TRUNCATE); strncpy_s(r.to,v.sky.c_str(),_TRUNCATE); strncpy_s(r.ssi,ssi.c_str(),_TRUNCATE);
            const PlaySkyValues sv=PlaySkyValuesOf(v.sky);
            if (sv.known) { r.rotation=sv.rotation; r.size=sv.size; r.flags|=brlive::LIVESKY_ROTATION|brlive::LIVESKY_SIZE; }
            r.pitch=v.pitch; r.yaw=v.yaw; for (int i=0;i<3;++i) r.color[i]=v.color[i];
            r.intensity=v.intensity; r.specScale=v.specScale; r.penumbra=v.penumbra;
            r.evcmp=v.evcmp; r.evmin=v.evmin; r.evmax=v.evmax; r.shadow=v.shadow ? 1 : 0; r.dynamicShadow=v.dynamicShadow ? 1 : 0;
            r.flags|=brlive::LIVESKY_SUN|(force ? brlive::LIVESKY_FORCE : 0);
            recs.push_back(r); names.push_back(v.sky);
        };
        std::string line;
        if (ssisNow.size()==1)
        {
            // one ssi everywhere: draw its sky whatever the level compiled, its sun where the ssi changed since the Play
            // (or where the level compiled another sky: lighting built before the ssi changed)
            const std::string& ssi=ssiName[*ssisNow.begin()];
            add(std::string(),ssi,!changes.empty());
            line="every sun volume: ssi "+ssi+" (sky "+PlaySkyOfSsi(ssi)+(changes.empty() ? "" : ", sun and exposure")+")";
        }
        else
        {
            for (const auto& c:changes) { add(PlaySkyOfSsi(c.first),c.second,true); line+=(line.empty() ? "" : ", ")+c.first+" -> "+c.second; }
            if (line.empty()) line=ssisNow.empty() ? "no ssi with a sky in the sun keys" : std::to_string(ssisNow.size())+" ssis, none changed";
        }
        std::string bytes;
        bytes.append(g_playLiveSky ? "1" : "0");
        if (!recs.empty()) bytes.append(reinterpret_cast<const char*>(recs.data()),recs.size()*sizeof(brlive::LiveSky));
        if (bytes==g_playSkyPublished && g_playSkyPublishedTo==sh) return false;
        InterlockedIncrement(&sh->liveSkySeq);
        MemoryBarrier();
        for (size_t k=0;k<recs.size();++k) sh->liveSky[k]=recs[k];
        sh->liveSkyCount=(LONG)recs.size();
        sh->liveSkyEnabled=g_playLiveSky ? 1 : 0;
        sh->liveSkyGeneration=++g_playSkyGeneration;
        MemoryBarrier();
        InterlockedIncrement(&sh->liveSkySeq);
        g_playSkyPublished.swap(bytes);
        g_playSkyPublishedTo=sh;
        g_playSkyPublishedNames.swap(names);
        g_playSkyLine=line;
        char msg[400]={};
        snprintf(msg,sizeof msg,"[LIVE SKY] generation %ld: %s (%u record(s))",(long)g_playSkyGeneration,line.c_str(),(unsigned)recs.size());
        PlayAppendLog(msg);
        return true;
    }

    // LIVE LIGHTS: a "light" entity is a primary light of the level. Radiant keeps the engine's own GfxConfig_Light (0x230 bytes) per light entity for its 3D view: EntityInst+552 points at the light object,
    // whose first 0x230 bytes are that config, rebuilt by EntityInst::UpdateLight on every move or key change. The configs of all lights are remembered when the baked cache loads (= what the game compiled),
    // every pass (50 ms, also during a drag) a changed, new or gone light goes out as a protocol v27 LiveLight record, and the bridge rewrites the compiled light with the same guid in place or adds a dynamic scene light (bridge/livelight.cpp).
    constexpr size_t kPlayLightConfigSize = brlive::kLightConfigSize;
    constexpr size_t kPlayEInstLight = 552, kPlayEInstNext = 768, kPlayEntityGuid = 352;
    constexpr size_t kPlayLightType = 64, kPlayLightGuid = 68, kPlayLightProbeOnly = 0x88;
    struct PlayLightConfig { unsigned char c[kPlayLightConfigSize]; };
    std::map<uint32_t,PlayLightConfig> g_playBakedLights;       // key = GfxConfig_Light.guid
    unsigned g_playBakedLoadSerial = 0;          // bumped by PlayLoadBakedHulls / PlayDropBakedHulls
    unsigned g_playBakedLightsSerial = ~0u;      // the load the light baseline was taken at
    bool g_playLiveLights = true;                // [playtest] live_lights
    LONG g_playLightGeneration = 0;
    std::string g_playLightPublished;
    brlive::SharedState* g_playLightPublishedTo = nullptr;
    uint64_t g_playLightLastPass = 0;
    size_t g_playLightsSeen = 0, g_playLightsChanged = 0, g_playLightsNew = 0, g_playLightsRemoved = 0;

    // Walks the world's EntityInst list and copies every light's config (spot and omni, not probe-only). No C++
    // objects in here: a stale pointer must not take Radiant down. Returns the number copied, -1 on a fault.
    using PlayInstListFn = unsigned char** (__fastcall*)(void* world, unsigned char** out);
    int PlayCollectLightConfigsSeh(void* world, PlayInstListFn beginFn, PlayInstListFn endFn, unsigned char* out, int cap)
    {
        int n=0;
        __try
        {
            unsigned char* first=nullptr; unsigned char* endTmp=nullptr;
            beginFn(world,&first);
            unsigned char** endPtr=endFn(world,&endTmp);
            unsigned char* end=endPtr ? *endPtr : nullptr;
            size_t guard=0;
            for (unsigned char* inst=first; inst && inst!=end && guard++<200000; inst=*reinterpret_cast<unsigned char**>(inst+kPlayEInstNext))
            {
                const unsigned char* light=*reinterpret_cast<unsigned char**>(inst+kPlayEInstLight);
                if (!light) continue;
                // Radiant leaves the config's guid 0, the game's is the entity GUID's first 8 hex digits (Data1, entity+352)
                const unsigned char* ent=*reinterpret_cast<unsigned char**>(inst);
                if (!ent) continue;
                uint32_t type=0, guid=0;
                std::memcpy(&type,light+kPlayLightType,4); std::memcpy(&guid,ent+kPlayEntityGuid,4);
                if ((type!=2 && type!=4) || !guid || guid==0xFFFFFFFFu || light[kPlayLightProbeOnly]) continue;
                if (n>=cap) break;
                std::memcpy(out+size_t(n)*kPlayLightConfigSize,light,kPlayLightConfigSize);
                std::memcpy(out+size_t(n)*kPlayLightConfigSize+kPlayLightGuid,&guid,4);
                ++n;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
        return n;
    }
    bool PlayCollectLightConfigs(std::map<uint32_t,PlayLightConfig>& out)
    {
        if (!rp::host || !rp::host->RadiantAddress) return false;
        void* world=rp::host->RadiantAddress(rp::self,0x14522B130);                                             // world object
        auto beginFn=reinterpret_cast<PlayInstListFn>(rp::host->RadiantAddress(rp::self,0x1400186F6));        // EntityInst list begin
        auto endFn=reinterpret_cast<PlayInstListFn>(rp::host->RadiantAddress(rp::self,0x14004E1C0));          // EntityInst list end
        if (!world || !beginFn || !endFn) return false;
        static std::vector<unsigned char> buf;
        const int cap=4096;
        if (buf.empty()) buf.resize(size_t(cap)*kPlayLightConfigSize);
        const int n=PlayCollectLightConfigsSeh(world,beginFn,endFn,buf.data(),cap);
        if (n<0) return false;
        for (int i=0;i<n;++i)
        {
            PlayLightConfig c; std::memcpy(c.c,buf.data()+size_t(i)*kPlayLightConfigSize,kPlayLightConfigSize);
            uint32_t guid=0; std::memcpy(&guid,c.c+kPlayLightGuid,4);
            out[guid]=c;
        }
        return true;
    }

    // Main thread, from the live tick. Returns true when it published.
    bool PlayReconcileLights(uint64_t now)
    {
        if (!g_playLiveShared || !g_playBakedHullsLoaded) return false;
        if (now-g_playLightLastPass<50) return false;
        g_playLightLastPass=now;
        if (rp::Has(&rp::host->PrefabDepth) && rp::host->PrefabDepth && rp::host->PrefabDepth(rp::self)>0) return false;   // the root map's lights are not listed in there
        if (rp::Has(&rp::host->IsReloading) && rp::host->IsReloading && rp::host->IsReloading(rp::self)) return false;
        std::map<uint32_t,PlayLightConfig> lights;
        if (g_playLiveLights && !PlayCollectLightConfigs(lights)) return false;
        if (g_playBakedLightsSerial!=g_playBakedLoadSerial)
        {
            g_playBakedLightsSerial=g_playBakedLoadSerial;
            g_playBakedLights=lights;
            char line[200]={};
            snprintf(line,sizeof line,"[LIVE LIGHT] %u light(s) of the map remembered as compiled",(unsigned)lights.size());
            PlayAppendLog(line);
        }
        std::vector<brlive::LiveLight> recs;
        size_t changed=0, fresh=0, removed=0, dropped=0;
        auto add=[&](uint32_t guid, LONG flags, const unsigned char* config)
        {
            if (recs.size()>=(size_t)brlive::kMaxLiveLights) { ++dropped; return; }
            brlive::LiveLight r; std::memset(&r,0,sizeof r);
            r.guid=guid; r.flags=flags;
            if (config) std::memcpy(r.config,config,kPlayLightConfigSize);
            recs.push_back(r);
        };
        if (g_playLiveLights)
        {
            for (const auto& pair:lights)
            {
                auto base=g_playBakedLights.find(pair.first);
                if (base==g_playBakedLights.end()) { add(pair.first,brlive::LIVELIGHT_NEW,pair.second.c); ++fresh; }
                else if (std::memcmp(base->second.c,pair.second.c,kPlayLightConfigSize)!=0) { add(pair.first,0,pair.second.c); ++changed; }
            }
            for (const auto& pair:g_playBakedLights) if (!lights.count(pair.first)) { add(pair.first,brlive::LIVELIGHT_REMOVED,nullptr); ++removed; }
        }
        g_playLightsSeen=lights.size(); g_playLightsChanged=changed; g_playLightsNew=fresh; g_playLightsRemoved=removed;
        std::string bytes;
        bytes.append(g_playLiveLights ? "1" : "0");
        if (!recs.empty()) bytes.append(reinterpret_cast<const char*>(recs.data()),recs.size()*sizeof(brlive::LiveLight));
        if (bytes==g_playLightPublished && g_playLightPublishedTo==g_playLiveShared) return false;
        const bool countsChanged=g_playLightPublishedTo!=g_playLiveShared || bytes.size()!=g_playLightPublished.size();
        brlive::SharedState* sh=g_playLiveShared;
        InterlockedIncrement(&sh->liveLightSeq);
        MemoryBarrier();
        for (size_t k=0;k<recs.size();++k) sh->liveLight[k]=recs[k];
        sh->liveLightCount=(LONG)recs.size();
        sh->liveLightEnabled=g_playLiveLights ? 1 : 0;
        sh->liveLightGeneration=++g_playLightGeneration;
        MemoryBarrier();
        InterlockedIncrement(&sh->liveLightSeq);
        const bool firstToThisGame=g_playLightPublishedTo!=g_playLiveShared;
        g_playLightPublished.swap(bytes);
        g_playLightPublishedTo=g_playLiveShared;
        if (countsChanged && (!firstToThisGame || !recs.empty()))   // a drag republishes every pass: only a new set is logged
        {
            char line[320]={};
            snprintf(line,sizeof line,"[LIVE LIGHT] generation %ld: %u light(s) moved or changed, %u new, %u deleted%s",(long)g_playLightGeneration,(unsigned)changed,(unsigned)fresh,(unsigned)removed,
                dropped ? " (more than 128 records: the rest is left out)" : "");
            PlayAppendLog(line);
        }
        return true;
    }

    void PlayClearLiveHides(const char* reason)
    {
        size_t prefabsShown=0, modelsShown=0;
        for (auto& pair:g_playBakedPrefabs) { if (pair.second.hidden) { pair.second.hidden=false; ++prefabsShown; } pair.second.liveObjects=false; pair.second.liveKeys.clear(); }
        for (auto& pair:g_playBakedModels) if (pair.second.hidden) { pair.second.hidden=false; ++modelsShown; }
        if (prefabsShown || modelsShown) PlayPublishModelHides();
        for (auto& pair:g_playBakedDecals) pair.second.hidden=false;
        g_playDecalPublished.clear();   // the next decal reconcile publishes again
        if (g_playLiveHides.empty() && g_playLivePatchHides.empty() && !prefabsShown) return;
        const size_t removed=g_playLiveHides.size(), removedPatches=g_playLivePatchHides.size();
        g_playLiveHides.clear();
        g_playLivePatchHides.clear();
        PlayPublishLiveHides();
        char line[320]={};
        snprintf(line,sizeof line,"[LIVE DELETE] %u hidden brush record(s) and %u patch record(s) dropped: %s",(unsigned)removed,(unsigned)removedPatches,reason ? reason : "");
        PlayAppendLog(line);
    }

    // Outward planes and bounds of a classic brush given as 3 points per face (the proxy picks the orientation).
    bool PlayOrientHull(const float* raw, size_t nFaces, std::vector<PlayLivePlane>& planes, float mins[3], float maxs[3])
    {
        if (!raw || nFaces<4 || nFaces>128) return false;
        ModToolsSelectedBrush sel={};
        for (int a=0;a<3;++a) { sel.mins[a]=-1.0e6f; sel.maxs[a]=1.0e6f; }
        std::vector<PlayLiveVec3> verts; std::vector<brlive::Edge> edges; std::vector<brlive::Triangle> triangles;
        PlayLiveGeometryStats stats;
        planes.clear();
        if (!PlayBuildExactBrushProxy(sel,raw,nFaces,verts,edges,triangles,stats,nullptr,nullptr,nullptr,&planes) || planes.size()<4 || verts.empty()) return false;
        mins[0]=maxs[0]=verts[0].x; mins[1]=maxs[1]=verts[0].y; mins[2]=maxs[2]=verts[0].z;
        for (const PlayLiveVec3& v:verts)
        {
            mins[0]=(std::min)(mins[0],v.x); maxs[0]=(std::max)(maxs[0],v.x);
            mins[1]=(std::min)(mins[1],v.y); maxs[1]=(std::max)(maxs[1],v.y);
            mins[2]=(std::min)(mins[2],v.z); maxs[2]=(std::max)(maxs[2],v.z);
        }
        return true;
    }

    bool PlaySameHull(const std::vector<PlayLivePlane>& a, const std::vector<PlayLivePlane>& b)
    {
        if (a.size()!=b.size()) return false;
        for (const PlayLivePlane& pa:a)
        {
            bool found=false;
            for (const PlayLivePlane& pb:b)
                if (PlayLiveDot(pa.n,pb.n)>0.9999f && std::fabs(pa.d-pb.d)<0.125f) { found=true; break; }
            if (!found) return false;
        }
        return true;
    }

    std::wstring PlayCurrentMapPathW()
    {
        std::wstring mapPath;
        if (!rp::host || !rp::Has(&rp::host->CurrentMapPath) || !rp::host->CurrentMapPath) return mapPath;
        wchar_t small[2048]={};
        const size_t n=rp::host->CurrentMapPath(rp::self,small,sizeof(small)/sizeof(small[0]));
        if (n && n<sizeof(small)/sizeof(small[0])) mapPath.assign(small,n);
        else if (n)
        {
            std::vector<wchar_t> big(n+1,L'\0');
            rp::host->CurrentMapPath(rp::self,big.data(),big.size());
            mapPath.assign(big.data(),n);
        }
        return mapPath;
    }

    // The baked hull cache from the saved .map (the file Play compiled): classic brush blocks of every entity, no patches or prefab interiors.
    // One pass over .map text: brush blocks (depth 2) go to onBrush(guid, 9 floats per face), entity blocks to onEntity(guid, kvs), `mesh` blocks to onPatch, `curve` blocks are skipped.
    // g_playParseOwnerClass: the owning entity's classname (lower case) for brush callbacks. A plain buffer: a thread_local with a destructor would run after a hot unload.
    thread_local char g_playParseOwnerClass[64];
    template <class OnBrush, class OnEntity, class OnPatch>
    void PlayParseMapTextP(const std::string& textIn, OnBrush onBrush, OnEntity onEntity, OnPatch onPatch, size_t& brushes, size_t& failed)
    {
        // physics primitives: their faces as the compile wrapper writes them for cod2map (physprim.h)
        const std::string converted=physprim::Contains(textIn) ? physprim::Convert(textIn) : std::string();
        const std::string& text=converted.empty() ? textIn : converted;
        size_t pos=0;
        bool inBrush=false, inPatch=false;
        int depth=0, patchRows=0, patchCols=0;
        std::string guid, entityGuid, block;
        std::vector<float> raw;
        std::vector<PlayLiveVec3> patchPts;
        std::vector<std::pair<std::string,std::string>> kvs;
        while (pos<text.size())
        {
            size_t eol=text.find('\n',pos);
            if (eol==std::string::npos) eol=text.size();
            std::string line=text.substr(pos,eol-pos);
            pos=eol+1;
            size_t b=0; while (b<line.size() && (line[b]==' ' || line[b]=='\t' || line[b]=='\r')) ++b;
            if (b) line.erase(0,b);
            while (!line.empty() && (line.back()=='\r' || line.back()==' ')) line.pop_back();
            if (line.empty()) continue;
            if (line[0]=='{') { ++depth; if (depth==1) { entityGuid.clear(); kvs.clear(); g_playParseOwnerClass[0]=0; } continue; }
            if (line[0]=='}')
            {
                if (inBrush && depth==2)
                {
                    inBrush=false;
                    if (!guid.empty() && raw.size()>=4*9) { if (onBrush(guid,raw,block)) ++brushes; else ++failed; }
                    if (inPatch && !guid.empty() && patchRows>=2 && patchCols>=2 && patchPts.size()==static_cast<size_t>(patchRows)*static_cast<size_t>(patchCols))
                        onPatch(guid,patchRows,patchCols,patchPts);
                    inPatch=false;
                }
                if (depth==1) onEntity(entityGuid,kvs);
                --depth;
                if (depth<0) depth=0;
                continue;
            }
            if (line.rfind("// brush",0)==0) { inBrush=true; inPatch=false; guid.clear(); raw.clear(); block.clear(); patchPts.clear(); patchRows=patchCols=0; continue; }
            if (inBrush && inPatch && depth==3)
            {
                if (line[0]=='v' && line.size()>1 && (line[1]==' ' || line[1]=='\t'))
                {
                    PlayLiveVec3 v;
                    if (sscanf_s(line.c_str(),"v %f %f %f",&v.x,&v.y,&v.z)==3) patchPts.push_back(v); else inPatch=false;
                }
                else if (!patchRows && line[0]>='0' && line[0]<='9')
                {
                    int r=0, c=0;
                    if (sscanf_s(line.c_str(),"%d %d",&r,&c)==2 && r>=2 && c>=2 && r<=1024 && c<=1024) { patchRows=r; patchCols=c; }
                }
                continue;
            }
            if (depth==1 && !inBrush)
            {
                if (line.rfind("guid",0)==0)
                {
                    const size_t q1=line.find('{'), q2=line.find('}');
                    if (q1!=std::string::npos && q2!=std::string::npos && q2>q1) entityGuid=line.substr(q1,q2-q1+1);
                }
                else if (line[0]=='"')
                {
                    PlayParseEntityKvs(line,kvs);
                    if (!kvs.empty() && kvs.back().first=="classname") strncpy_s(g_playParseOwnerClass,PlayLowerAscii(kvs.back().second).c_str(),_TRUNCATE);
                }
                continue;
            }
            if (!inBrush || depth!=2) continue;
            if (line.rfind("guid",0)==0)
            {
                const size_t q1=line.find('{'), q2=line.find('}');
                if (q1!=std::string::npos && q2!=std::string::npos && q2>q1) guid=line.substr(q1,q2-q1+1);
                continue;
            }
            if (line[0]=='(')
            {
                block+=line; block+='\n';
                float v[9];
                if (sscanf_s(line.c_str(),"( %f %f %f ) ( %f %f %f ) ( %f %f %f )",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7],&v[8])==9)
                    raw.insert(raw.end(),v,v+9);
                else { raw.clear(); guid.clear(); }   // not a classic brush face line: skip this block
                continue;
            }
            if (line=="mesh") { raw.clear(); inPatch=true; patchPts.clear(); patchRows=patchCols=0; continue; }   // patch block: onPatch at the close
            if (line.rfind("mesh",0)==0 || line.rfind("curve",0)==0) { raw.clear(); guid.clear(); }   // curve block: skipped
        }
    }
    template <class OnBrush, class OnEntity>
    void PlayParseMapText(const std::string& text, OnBrush onBrush, OnEntity onEntity, size_t& brushes, size_t& failed)
    {
        PlayParseMapTextP(text,onBrush,onEntity,[](const std::string&, int, int, const std::vector<PlayLiveVec3>&) {},brushes,failed);
    }

    // id's AnglesToAxis on a misc_prefab's "angles" key (pitch yaw roll, degrees): a point p of the prefab file sits at
    // origin + p.x * axis[0] + p.y * axis[1] + p.z * axis[2] in the outer scope (RADIANT_FACTS "Prefab stack").
    struct PlayXform { float o[3] = {0,0,0}; float ax[3][3] = {{1,0,0},{0,1,0},{0,0,1}}; };
    void PlayAnglesToAxis(const float angles[3], float ax[3][3])
    {
        const float d2r=3.14159265358979f/180.0f;
        const float sp=std::sin(angles[0]*d2r), cp=std::cos(angles[0]*d2r);
        const float sy=std::sin(angles[1]*d2r), cy=std::cos(angles[1]*d2r);
        const float sr=std::sin(angles[2]*d2r), cr=std::cos(angles[2]*d2r);
        ax[0][0]=cp*cy;                 ax[0][1]=cp*sy;                 ax[0][2]=-sp;          // forward
        ax[1][0]=-(-sr*sp*cy+cr*sy);    ax[1][1]=-(-sr*sp*sy-cr*cy);    ax[1][2]=-(-sr*cp);    // -right
        ax[2][0]=cr*sp*cy+sr*sy;        ax[2][1]=cr*sp*sy-sr*cy;        ax[2][2]=cr*cp;        // up
    }
    void PlayXformPoint(const PlayXform& x, const float p[3], float out[3])
    {
        for (int a=0;a<3;++a) out[a]=x.o[a]+p[0]*x.ax[0][a]+p[1]*x.ax[1][a]+p[2]*x.ax[2][a];
    }
    PlayXform PlayXformCompose(const PlayXform& outer, const float origin[3], const float angles[3])
    {
        PlayXform inner; PlayAnglesToAxis(angles,inner.ax);
        PlayXform r;
        PlayXformPoint(outer,origin,r.o);
        for (int i=0;i<3;++i)
            for (int a=0;a<3;++a) r.ax[i][a]=inner.ax[i][0]*outer.ax[0][a]+inner.ax[i][1]*outer.ax[1][a]+inner.ax[i][2]*outer.ax[2][a];
        return r;
    }
    int PlayPrefabDepth();
    void PlayRefreshPrefabGeometry(const char* why);
    void PlayRebuildLiveModelWanted();
    bool PlayInsidePrefabPass(uint64_t now, bool& registryChanged, size_t& objectsSkipped);
    // classname misc_model: model (xmodel name), origin, angles, modelscale (1 when absent).
    bool PlayModelPlacementFromKvs(const std::vector<std::pair<std::string,std::string>>& kvs, PlayBakedModel& out)
    {
        bool model=false;
        for (const auto& kv:kvs)
        {
            if (kv.first=="classname") model=PlayLowerAscii(kv.second)=="misc_model";
            else if (kv.first=="model") out.model=PlayLowerAscii(kv.second);
            else if (kv.first=="origin") { float v[3]; if (PlayParseVec3(kv.second,v)) for (int a=0;a<3;++a) out.origin[a]=v[a]; }
            else if (kv.first=="angles") { float v[3]; if (PlayParseVec3(kv.second,v)) for (int a=0;a<3;++a) out.angles[a]=v[a]; }
            else if (kv.first=="modelscale") { float v=static_cast<float>(atof(kv.second.c_str())); if (v>0.0f) out.scale=v; }
        }
        return model && !out.model.empty();
    }
    bool PlayPrefabPlacementFromKvs(const std::vector<std::pair<std::string,std::string>>& kvs, PlayPrefabPlacement& out)
    {
        bool prefab=false;
        for (const auto& kv:kvs)
        {
            if (kv.first=="classname") prefab=PlayLowerAscii(kv.second)=="misc_prefab";
            else if (kv.first=="model") { out.model=PlayLowerAscii(kv.second); std::replace(out.model.begin(),out.model.end(),'\\','/'); }
            else if (kv.first=="origin") { float v[3]; if (PlayParseVec3(kv.second,v)) for (int a=0;a<3;++a) out.origin[a]=v[a]; }
            else if (kv.first=="angles") { float v[3]; if (PlayParseVec3(kv.second,v)) for (int a=0;a<3;++a) out.angles[a]=v[a]; }
        }
        return prefab && !out.model.empty();
    }
    // A GUID-shaped text unique to (instance, interior brush): the bridge keys its baked twins by sourceGuid.
    std::string PlayPrefabRecordGuid(const std::string& entityKey, const std::string& brushGuid, size_t index)
    {
        uint64_t h=1469598103934665603ull;
        auto mix=[&](const std::string& t){ for (unsigned char c:t) { h^=c; h*=1099511628211ull; } h^=0x7C; h*=1099511628211ull; };
        mix(entityKey); mix(brushGuid); mix(std::to_string(index));
        char out[40]={};
        snprintf(out,sizeof out,"{7B%06X-%04X-4%03X-8%03X-%012llX}",(unsigned)(h&0xFFFFFF),(unsigned)((h>>24)&0xFFFF),(unsigned)((h>>40)&0xFFF),(unsigned)((h>>52)&0xFFF),(unsigned long long)((h*0x9E3779B97F4A7C15ull)&0xFFFFFFFFFFFFull));
        return out;
    }
    // Every interior brush of a prefab file placed by `x` (nested prefabs recursively), as world-space hulls.
    void PlayCollectPrefabHulls(const std::wstring& mapSource, const std::string& model, const PlayXform& x, const PlayXform& toOuter, int depth,
                                std::vector<std::string>& stack, std::map<std::string,std::string>& fileCache,
                                const std::string& entityKey, PlayBakedPrefab& out, size_t& failed, size_t& files, size_t& missing)
    {
        if (depth>8 || model.empty()) return;
        if (std::find(stack.begin(),stack.end(),model)!=stack.end()) return;   // a prefab that contains itself
        const std::string nkey=PlayPrefabKeyNorm(model);
        auto cached=fileCache.find(nkey);
        if (cached==fileCache.end())
        {
            const std::wstring file=PlayJoin(mapSource,PlayUtf8ToWide(model));
            std::string text;
            auto ov=g_playPrefabUseOverrides ? g_playPrefabScopeText.find(nkey) : g_playPrefabScopeText.end();
            if (ov!=g_playPrefabScopeText.end()) text=ov->second;   // Radiant's memory of an entered (edited) prefab beats the disk
            else text=PlayReadTextFile(file,256u*1024*1024);
            if (text.empty()) { ++missing; return; }
            ++files;
            g_playPrefabFileTimes[model]=PlayFileTime(file);
            cached=fileCache.emplace(nkey,std::move(text)).first;
        }
        stack.push_back(model);
        std::vector<std::pair<PlayPrefabPlacement,int>> nested;
        size_t brushes=0;
        const std::string& fileText=cached->second;
        std::unordered_map<std::string,size_t> guidLines;   // built on the first patch
        PlayParseMapTextP(cached->second,
            [&](const std::string& brushGuid, const std::vector<float>& raw, const std::string& block)
            {
                // The file's points are brought into the OUTERMOST prefab file's space (`local`: the instance placement is
                // applied at hide / draw time), then into the world for the compiled hull.
                std::vector<float> local(raw.size()), world(raw.size());
                for (size_t i=0;i+2<raw.size();i+=3) { PlayXformPoint(toOuter,&raw[i],&local[i]); PlayXformPoint(x,&raw[i],&world[i]); }
                PlayBakedPrefabHull h;
                if (!PlayOrientHull(world.data(),world.size()/9,h.hull.planes,h.hull.mins,h.hull.maxs)) return false;
                h.recordGuid=PlayPrefabRecordGuid(entityKey,brushGuid,out.hulls.size());
                h.fileKey=nkey+"|"+PlayLowerAscii(brushGuid);
                if (strcmp(g_playParseOwnerClass,"worldspawn")!=0) h.ownerClass=g_playParseOwnerClass;
                { float mn[3],mx[3]; PlayOrientHull(local.data(),local.size()/9,h.localPlanes,mn,mx); }
                h.localRaw=std::move(local);
                PlayParseBrushFaceTexdefs(block,nullptr,h.localTex);
                for (PlayLiveFaceTex& t:h.localTex) for (int k=0;k<3;++k) { float p[3]={t.pts[k].x,t.pts[k].y,t.pts[k].z}, q[3]; PlayXformPoint(toOuter,p,q); t.pts[k]={q[0],q[1],q[2]}; }
                out.hulls.push_back(std::move(h));
                return true;
            },
            [&](const std::string& entGuid, const std::vector<std::pair<std::string,std::string>>& kvs)
            {
                PlayPrefabPlacement p;
                if (PlayPrefabPlacementFromKvs(kvs,p)) { nested.push_back({p,0}); return; }
                {
                    bool spot=false; float so[3]={0,0,0}, sa[3]={0,0,0};
                    for (const auto& kv:kvs)
                    {
                        if (kv.first=="classname") spot=PlayLowerAscii(kv.second)=="script_struct";
                        else if (kv.first=="origin") { float v[3]; if (PlayParseVec3(kv.second,v)) for (int a=0;a<3;++a) so[a]=v[a]; }
                        else if (kv.first=="angles") { float v[3]; if (PlayParseVec3(kv.second,v)) for (int a=0;a<3;++a) sa[a]=v[a]; }
                    }
                    if (spot)
                    {
                        const PlayXform local=PlayXformCompose(toOuter,so,sa);
                        PlayBakedPrefabSpot ps;
                        for (int a=0;a<3;++a) ps.localOrigin[a]=local.o[a];
                        for (int i=0;i<3;++i) for (int a=0;a<3;++a) ps.localAxis[i*3+a]=local.ax[i][a];
                        out.spots.push_back(ps);
                        return;
                    }
                }
                PlayBakedModel m;
                if (!PlayModelPlacementFromKvs(kvs,m)) return;
                PlayBakedPrefabModel pm;
                pm.model=m.model; pm.scale=m.scale;
                PlayXformPoint(x,m.origin,pm.origin);
                const PlayXform placed=PlayXformCompose(x,m.origin,m.angles);
                for (int i=0;i<3;++i) for (int a=0;a<3;++a) pm.axis[i*3+a]=placed.ax[i][a];
                pm.recordGuid=PlayPrefabRecordGuid(entityKey,entGuid.empty() ? m.model : entGuid,out.models.size()+100000);
                pm.fileKey=nkey+"|"+PlayLowerAscii(entGuid.empty() ? m.model : entGuid);
                const PlayXform local=PlayXformCompose(toOuter,m.origin,m.angles);
                for (int a=0;a<3;++a) pm.localOrigin[a]=local.o[a];
                for (int i=0;i<3;++i) for (int a=0;a<3;++a) pm.localAxis[i*3+a]=local.ax[i][a];
                out.models.push_back(std::move(pm));
            },
            [&](const std::string& guid, int rows, int cols, const std::vector<PlayLiveVec3>& pts)
            {
                PlayPatchGrid g;
                if (guidLines.empty()) PlayIndexGuidLines(fileText,guidLines);
                std::string lowerGuid=guid;
                for (char& ch:lowerGuid) ch=static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                const auto at=guidLines.find(lowerGuid);
                if (at==guidLines.end() || !PlayParsePatchAt(fileText,at->second,g) || g.curve || g.rows!=rows || g.cols!=cols || g.pts.size()!=pts.size() || g.uv.size()!=pts.size()) return;
                PlayBakedPrefabPatch pp;
                pp.recordGuid=PlayPrefabRecordGuid(entityKey,guid,out.patches.size()+200000);
                pp.fileKey=nkey+"|"+PlayLowerAscii(guid);
                pp.grid.rows=rows; pp.grid.cols=cols; pp.grid.pts.resize(pts.size());
                pp.local=g;
                for (size_t i=0;i<pts.size();++i)
                {
                    const float in[3]={pts[i].x,pts[i].y,pts[i].z}; float w[3], l[3];
                    PlayXformPoint(x,in,w); PlayXformPoint(toOuter,in,l);
                    pp.grid.pts[i]={w[0],w[1],w[2]}; pp.local.pts[i]={l[0],l[1],l[2]};
                }
                PlayPatchBounds(pp.grid);
                out.patches.push_back(std::move(pp));
            }, brushes, failed);
        for (const auto& n:nested)
            PlayCollectPrefabHulls(mapSource,n.first.model,PlayXformCompose(x,n.first.origin,n.first.angles),PlayXformCompose(toOuter,n.first.origin,n.first.angles),depth+1,stack,fileCache,entityKey,out,failed,files,missing);
        stack.pop_back();
    }

    void PlayLoadBakedHulls(const char* why)
    {
        g_playBakedHulls.clear();
        g_playBakedPatches.clear();
        g_playBakedPrefabs.clear();
        g_playBakedModels.clear();
        g_playBakedDecals.clear();
        g_playBakedFx.clear();
        g_playBakedSuns.clear(); g_playSsiSky.clear(); g_playSkyValues.clear();
        g_playSkyBakedKeySet=false; g_playSkyBakeWantKey.clear(); g_playSkyBakeDueMs=0; g_playSkyBakeWait.clear();
        g_playPrefabFileTimes.clear();
        g_playSeenEntityKeys.clear();
        g_playBakedHullsLoaded=false;
        const uint64_t t0=GetTickCount64();
        const std::wstring path=PlayCurrentMapPathW();
        if (path.empty()) { PlayAppendLog("[LIVE DELETE] baked hulls: no map path"); return; }
        FILE* f=_wfopen(path.c_str(),L"rb");
        if (!f) { PlayAppendLog("[LIVE DELETE] baked hulls: the map file could not be read"); return; }
        std::string text;
        {
            char chunk[65536];
            size_t got;
            while ((got=fread(chunk,1,sizeof chunk,f))>0) text.append(chunk,got);
            fclose(f);
        }
        size_t brushes=0, failed=0;
        std::vector<std::pair<std::string,PlayPrefabPlacement>> prefabs;   // entity guid, placement
        PlayParseMapTextP(text,
            [&](const std::string& guid, const std::vector<float>& raw, const std::string& block)
            {
                PlayBakedHull hull;
                if (!PlayOrientHull(raw.data(),raw.size()/9,hull.planes,hull.mins,hull.maxs)) return false;
                if (g_playLiveFillHoles) PlayParseBrushFaceTexdefs(block,nullptr,hull.tex);
                g_playBakedHulls[PlayLiveGuidKey(guid.c_str())]=std::move(hull);
                return true;
            },
            [&](const std::string& entityGuid, const std::vector<std::pair<std::string,std::string>>& kvs)
            {
                if (entityGuid.empty()) return;
                g_playSeenEntityKeys.insert(PlayLiveGuidKey(entityGuid.c_str()));
                { PlaySunKeys sun; if (PlaySunEntityFromKvs(kvs,sun)) { sun.entityGuid=entityGuid; g_playBakedSuns.push_back(std::move(sun)); return; } }
                PlayPrefabPlacement p;
                if (PlayPrefabPlacementFromKvs(kvs,p)) { prefabs.push_back({entityGuid,p}); return; }
                PlayFxParams fp;
                if (PlayFxFromKvs(kvs,fp)) { PlayBakedFx bf; bf.entityGuid=entityGuid; bf.compiled=fp; g_playBakedFx[PlayLiveGuidKey(entityGuid.c_str())]=std::move(bf); return; }
                PlayDecalParams dp;
                if (PlayDecalFromKvs(kvs,dp)) { PlayBakedDecal d; d.entityGuid=entityGuid; d.compiled=dp; g_playBakedDecals[PlayLiveGuidKey(entityGuid.c_str())]=std::move(d); return; }
                PlayBakedModel m;
                if (g_playLiveDeleteModels && PlayModelPlacementFromKvs(kvs,m)) { m.entityGuid=entityGuid; g_playBakedModels[PlayLiveGuidKey(entityGuid.c_str())]=std::move(m); }
            },
            [&](const std::string& guid, int rows, int cols, const std::vector<PlayLiveVec3>& pts)
            {
                PlayBakedPatch g; g.rows=rows; g.cols=cols; g.pts=pts; PlayPatchBounds(g);
                g_playBakedPatches[PlayLiveGuidKey(guid.c_str())]=std::move(g);
            }, brushes, failed);
        size_t prefabHulls=0, prefabModels=0, prefabPatches=0, prefabFailed=0, files=0, missing=0;
        if (g_playLiveDeletePrefabs && !prefabs.empty())
        {
            const std::wstring mapSource=PlayJoin(PlayGameRoot(),L"map_source");
            std::map<std::string,std::string> fileCache;
            g_playPrefabUseOverrides=false;   // the BSP was compiled from the files on disk
            for (const auto& pr:prefabs)
            {
                PlayBakedPrefab bp;
                bp.entityGuid=pr.first;
                bp.compiled=pr.second;
                PlayXform x; PlayAnglesToAxis(pr.second.angles,x.ax); for (int a=0;a<3;++a) x.o[a]=pr.second.origin[a];
                std::vector<std::string> stack;
                const std::string key=PlayLiveGuidKey(pr.first.c_str());
                PlayCollectPrefabHulls(mapSource,pr.second.model,x,PlayXform(),0,stack,fileCache,key,bp,prefabFailed,files,missing);
                if (bp.hulls.empty() && bp.models.empty() && bp.patches.empty()) continue;
                prefabHulls+=bp.hulls.size(); prefabModels+=bp.models.size(); prefabPatches+=bp.patches.size();
                g_playBakedPrefabs[key]=std::move(bp);
            }
            g_playPrefabUseOverrides=true;
            if (!g_playPrefabScopeText.empty()) PlayRefreshPrefabGeometry("prefab content edited in Radiant before this Play");   // unsaved prefab edits are not in the BSP
        }
        // Live Edit pauses during a build: edits made inside the entered prefab meanwhile are captured on the next pass
        if (PlayPrefabDepth()>0) { g_playScopeDirty=true; g_playScopeDirtyAt=GetTickCount64(); }
        g_playBakedHullsLoaded=true;
        ++g_playBakedLoadSerial;   // the light baseline is taken at the next light pass
        g_playBakedHullsPath=path;
        char msg[800]={};
        snprintf(msg,sizeof msg,"[LIVE DELETE] baked hulls: %u brush(es), %u mesh patch(es) and %u static model(s) from the saved map (%u not closed), %u prefab instance(s) with %u interior brush(es), %u patch(es) and %u model(s) from %u file(s) (%u not closed, %u file(s) missing), %s, %u ms",
            (unsigned)brushes,(unsigned)g_playBakedPatches.size(),(unsigned)g_playBakedModels.size(),(unsigned)failed,(unsigned)g_playBakedPrefabs.size(),(unsigned)prefabHulls,(unsigned)prefabPatches,(unsigned)prefabModels,(unsigned)files,(unsigned)prefabFailed,(unsigned)missing,why ? why : "",
            (unsigned)(GetTickCount64()-t0));
        PlayAppendLog(msg);
    }

    void PlayDropBakedHulls() { g_playBakedHulls.clear(); g_playBakedPatches.clear(); g_playBakedPrefabs.clear(); g_playBakedModels.clear(); g_playBakedDecals.clear(); g_playBakedFx.clear(); g_playBakedSuns.clear(); g_playLiveModelWanted.clear(); g_playLiveModelWantedTop.clear(); g_playPrefabFileTimes.clear(); g_playSeenEntityKeys.clear(); g_playBakedHullsLoaded=false; }

    bool PlayHideFromBaked(const std::string& key, PlayLiveHide& out)
    {
        auto found=g_playBakedHulls.find(key);
        if (found==g_playBakedHulls.end()) return false;
        out.guid=key;
        out.planes=found->second.planes;
        for (int a=0;a<3;++a) { out.mins[a]=found->second.mins[a]; out.maxs[a]=found->second.maxs[a]; }
        out.active=true;
        out.since=GetTickCount64();
        return true;
    }

    // Fallback when the cache is unavailable: the hull of one selected brush as it is now (outward planes from the exact
    // proxy, bounds from the selection). False for patches, fixed-size entities and brushes the proxy cannot close.
    bool PlayBuildHideRecord(const ModToolsSelectedBrush& sel, ModToolsBrushInst inst, PlayLiveHide& out)
    {
        if (!inst || !sel.brushGuid[0]) return false;
        if (PlayEntityIsPrefab(sel.entityGuid) || PlayInstIsStandIn(inst)) return false;
        if (sel.entityGuid[0] && rp::Has(&rp::host->FindEntitiesByGuid) && rp::host->FindEntitiesByGuid &&
            rp::Has(&rp::host->EntityFixedSize) && rp::host->EntityFixedSize)
        {
            const char* entityGuid=sel.entityGuid;
            ModToolsEntity entity=nullptr;
            if (rp::host->FindEntitiesByGuid(rp::self,&entityGuid,1,&entity) && entity && rp::host->EntityFixedSize(rp::self,entity)) return false;
        }
        constexpr size_t kCap=128;
        float raw[kCap*9]={};
        size_t faceCount=0;
        bool resolved=false;
        if (!PlayTryReadSettledBrushPlanes(inst,raw,kCap,faceCount,resolved) || !resolved || faceCount<4 || faceCount>kCap) return false;
        std::vector<PlayLivePlane> planes;
        float mins[3], maxs[3];
        if (!PlayOrientHull(raw,faceCount,planes,mins,maxs)) return false;
        out.guid=sel.brushGuid;
        out.planes.swap(planes);
        for (int a=0;a<3;++a) { out.mins[a]=sel.mins[a]; out.maxs[a]=sel.maxs[a]; }
        out.active=true;
        out.since=GetTickCount64();
        return true;
    }

    // Before Select_Delete runs: a hide record for every selected brush that the compiled BSP contains (the baked hull,
    // the current hull only while the cache is unavailable).
    void PlayCommitDeletePatchCandidates();
    void PlayCaptureDeleteCandidates(std::vector<PlayLiveHide>& out)
    {
        out.clear();
        g_playDeletePatchCandidates.clear();
        size_t selectionCount=0, instanceCount=0;
        if (!PlayTryFillSettledSelection(nullptr,0,selectionCount) || !PlayTryFillSettledBrushInsts(nullptr,0,instanceCount) ||
            !selectionCount || !instanceCount || selectionCount>4096 || instanceCount>4096) return;
        std::vector<ModToolsSelectedBrush> selections(selectionCount);
        std::vector<ModToolsBrushInst> instances(instanceCount,nullptr);
        size_t selectedRead=0, instancesRead=0;
        if (!PlayTryFillSettledSelection(selections.data(),selections.size(),selectedRead) ||
            !PlayTryFillSettledBrushInsts(instances.data(),instances.size(),instancesRead)) return;
        selections.resize((std::min)(selectedRead,selections.size()));
        instances.resize((std::min)(instancesRead,instances.size()));
        if (g_playBakedHullsLoaded)
        {
            for (const ModToolsSelectedBrush& sel:selections)
            {
                PlayLiveHide hide;
                if (sel.brushGuid[0] && PlayHideFromBaked(PlayLiveGuidKey(sel.brushGuid),hide)) out.push_back(std::move(hide));
                PlayLivePatchHide ph;
                if (sel.brushGuid[0] && PlayPatchHideFromBaked(PlayLiveGuidKey(sel.brushGuid),ph)) g_playDeletePatchCandidates.push_back(std::move(ph));
            }
            return;
        }
        std::vector<std::string> instanceGuids(instances.size());
        for (size_t i=0;i<instances.size();++i)
        {
            char guid[40]={};
            if (PlayTrySettledBrushGuid(instances[i],guid)) instanceGuids[i]=PlayLiveGuidKey(guid);
        }
        std::vector<bool> used(instances.size(),false);
        for (const ModToolsSelectedBrush& sel:selections)
        {
            const std::string key=PlayLiveGuidKey(sel.brushGuid);
            size_t match=instances.size();
            for (size_t i=0;i<instances.size();++i)
                if (!used[i] && !key.empty() && instanceGuids[i]==key) { match=i; break; }
            if (match==instances.size() && selections.size()==instances.size())
            {
                const size_t index=static_cast<size_t>(&sel-selections.data());
                if (index<instances.size() && !used[index]) match=index;
            }
            if (match==instances.size()) continue;
            used[match]=true;
            PlayLiveHide hide;
            if (PlayBuildHideRecord(sel,instances[match],hide)) out.push_back(std::move(hide));
        }
    }

    // After Select_Delete ran: every candidate that is gone becomes (or re-arms) a hide record.
    void PlayCommitDeleteCandidates(std::vector<PlayLiveHide>& candidates)
    {
        std::vector<const char*> guids; guids.reserve(candidates.size());
        for (const PlayLiveHide& h:candidates) guids.push_back(h.guid.c_str());
        std::vector<ModToolsBrush> brushes(candidates.size(),nullptr);
        if (!PlayTryFindLiveBrushes(guids.data(),guids.size(),brushes.data())) return;
        size_t added=0, rearmed=0, skipped=0, liveRemoved=0;
        std::string names;
        const uint64_t now=GetTickCount64();
        for (size_t i=0;i<candidates.size();++i)
        {
            if (brushes[i]) { ++skipped; continue; }   // still in the map: not deleted after all
            PlayLiveHide& h=candidates[i];
            h.active=true; h.since=now;
            const std::string key=PlayLiveGuidKey(h.guid.c_str());
            // A deleted brush that was also a Live Edit object leaves the registry now: the 500 ms reconcile is paused while the game has the controls, and its collision slot would stay solid until then.
            auto live=g_playLiveRegistry.find(key);
            if (live!=g_playLiveRegistry.end())
            {
                g_playLiveLastOperation=brlive::LIVE_OBJECT_DELETE;
                g_playLiveLastOperationId=live->second.liveObjectId;
                g_playLiveLastOperationGeneration=static_cast<LONG>(live->second.generation+1);
                ++g_playLiveDeleteCount;
                g_playLiveRegistry.erase(live);
                ++liveRemoved;
            }
            if (names.size()<240) { if (!names.empty()) names+=", "; names+=h.guid; }
            auto found=g_playLiveHides.find(key);
            if (found!=g_playLiveHides.end()) { found->second=std::move(h); ++rearmed; continue; }
            if (g_playLiveHides.size()>=static_cast<size_t>(brlive::kMaxHideRecords)) { ++skipped; continue; }
            g_playLiveHides.emplace(key,std::move(h));
            ++added;
        }
        if (liveRemoved)
        {
            ++g_playLiveRegistryGeneration;
            if (g_playLiveRegistry.empty()) PlayPublishLiveClear();
            PlayPublishLiveRegistry();
        }
        if (!added && !rearmed) return;
        g_playLiveHideAdded+=static_cast<LONG>(added);
        PlayPublishLiveHides();
        char line[640]={};
        snprintf(line,sizeof line,"[LIVE DELETE] %u deleted brush(es) hidden in the game (%u hidden again, %u skipped, %u live object(s) removed), %u record(s): %s",
            (unsigned)added,(unsigned)rearmed,(unsigned)skipped,(unsigned)liveRemoved,(unsigned)g_playLiveHides.size(),names.c_str());
        PlayAppendLog(line);
    }

    // The patch candidates of the same delete: every patch that is gone becomes (or re-arms) a patch record, its tiles
    // leave the registry.
    void PlayCommitDeletePatchCandidates()
    {
        std::vector<PlayLivePatchHide> candidates; candidates.swap(g_playDeletePatchCandidates);
        if (candidates.empty()) return;
        std::vector<const char*> guids; guids.reserve(candidates.size());
        for (const PlayLivePatchHide& h:candidates) guids.push_back(h.guid.c_str());
        std::vector<ModToolsBrush> brushes(candidates.size(),nullptr);
        if (!PlayTryFindLiveBrushes(guids.data(),guids.size(),brushes.data())) return;
        size_t added=0, rearmed=0, skipped=0, tilesRemoved=0;
        const uint64_t now=GetTickCount64();
        for (size_t i=0;i<candidates.size();++i)
        {
            if (brushes[i]) { ++skipped; continue; }
            PlayLivePatchHide& h=candidates[i];
            h.active=true; h.since=now;
            const std::string key=PlayLiveGuidKey(h.guid.c_str());
            tilesRemoved+=PlayEraseRegistryTiles(key);
            auto found=g_playLivePatchHides.find(key);
            if (found!=g_playLivePatchHides.end()) { found->second=std::move(h); ++rearmed; continue; }
            if (g_playLivePatchHides.size()>=static_cast<size_t>(brlive::kMaxPatchHides)) { ++skipped; continue; }
            g_playLivePatchHides.emplace(key,std::move(h)); ++added;
        }
        if (tilesRemoved)
        {
            ++g_playLiveRegistryGeneration;
            if (g_playLiveRegistry.empty()) PlayPublishLiveClear();
            PlayPublishLiveRegistry();
        }
        if (!added && !rearmed) return;
        PlayPublishLiveHides();
        char line[400]={};
        snprintf(line,sizeof line,"[LIVE PATCH] %u deleted patch(es) hidden in the game (%u hidden again, %u skipped, %u live tile(s) removed), %u patch record(s)",
            (unsigned)added,(unsigned)rearmed,(unsigned)skipped,(unsigned)tilesRemoved,(unsigned)g_playLivePatchHides.size());
        PlayAppendLog(line);
    }

    void PlayLogHideException(const char* where)
    {
        char line[160]={};
        snprintf(line,sizeof line,"[LIVE DELETE WARN] %s raised an exception; no hide record was made.",where);
        PlayAppendLog(line);
    }
    void PlayCaptureDeleteCandidatesSafe(std::vector<PlayLiveHide>* out)
    {
        __try { PlayCaptureDeleteCandidates(*out); }
        __except (EXCEPTION_EXECUTE_HANDLER) { out->clear(); PlayLogHideException("the delete capture"); }
    }
    void PlayCommitDeleteCandidatesSafe(std::vector<PlayLiveHide>* candidates)
    {
        __try { PlayCommitDeleteCandidates(*candidates); PlayCommitDeletePatchCandidates(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { PlayLogHideException("the delete commit"); }
    }

    // Select_Delete detour (main thread). Nested calls (the host's DeleteBrushes selects, then deletes) capture once.
    void __fastcall PlayHookSelectDelete()
    {
        const bool capture=g_playSelectDeleteDepth==0 && g_playLivePreview && PlayLiveEditBuild() && g_playLiveDelete &&
                           g_playLiveShared!=nullptr && g_playLiveBridgeInjected;
        if (capture) PlayCaptureDeleteCandidatesSafe(&g_playDeleteCandidates);
        ++g_playSelectDeleteDepth;
        if (g_playOrigSelectDelete) g_playOrigSelectDelete();
        --g_playSelectDeleteDepth;
        if (capture && (!g_playDeleteCandidates.empty() || !g_playDeletePatchCandidates.empty()))
        {
            PlayCommitDeleteCandidatesSafe(&g_playDeleteCandidates);
            g_playDeleteCandidates.clear();
            g_playDeletePatchCandidates.clear();
        }
        if (capture)
        {
            // a clip puts its pieces into the selection right after this returns: publish them once they settled
            g_playLiveDeleteFollowupPending=true;
            g_playLiveDeleteFollowupDue=GetTickCount64()+350;
        }
    }

    void PlayInstallDeleteHook()
    {
        if (!rp::host || !rp::host->RadiantAddress || !rp::host->Detour) return;
        void* target=rp::host->RadiantAddress(rp::self,0x140D02320);   // Select_Delete
        if (!target) { rp::Log("play: Select_Delete is not at its address in this Radiant build; live delete is off"); return; }
        if (!rp::host->Detour(rp::self,target,reinterpret_cast<void*>(&PlayHookSelectDelete),reinterpret_cast<void**>(&g_playOrigSelectDelete)))
        {
            rp::Log("play: the Select_Delete detour failed; live delete is off");
            return;
        }
        g_playSelectDeleteHooked=true;
        rp::Log("play: live delete armed (Select_Delete detour): deleting a brush hides its baked geometry in the running game");
    }

    // Undo / redo: Radiant's shared undo function 0x140C181D0 (Undo_Undo(undoList, redoList, undoId, redoId), the Redo wrapper passes the lists swapped) restores brushes and selects them (RADIANT_FACTS "Undo identity").
    // A restored brush without a compiled copy (created after the Play, or a clip's piece on redo) is invisible in the game until something publishes it, so the settled follow-up of a delete runs 350 ms later:
    // restored live brushes publish, brushes back at their compiled shape drop their live twin and their hide record goes inactive through the reconcile.
    using PlayUndoFn = __int64 (__fastcall*)(void*, void*, void*, void*);
    PlayUndoFn g_playOrigUndo = nullptr;
    __int64 __fastcall PlayHookUndo(void* undoList, void* redoList, void* undoId, void* redoId)
    {
        const __int64 r = g_playOrigUndo ? g_playOrigUndo(undoList,redoList,undoId,redoId) : 0;
        if (g_playLivePreview && PlayLiveEditBuild() && g_playLiveShared && g_playLiveBridgeInjected)
        {
            g_playLiveDeleteFollowupPending=true;
            g_playLiveDeleteFollowupDue=GetTickCount64()+350;
        }
        return r;
    }
    void PlayInstallUndoHook()
    {
        if (!rp::host || !rp::host->RadiantAddress || !rp::host->Detour) return;
        void* target=rp::host->RadiantAddress(rp::self,0x140C181D0);   // Undo_Undo
        if (!target) { rp::Log("play: Undo_Undo is not at its address in this Radiant build; undo / redo do not republish"); return; }
        if (!rp::host->Detour(rp::self,target,reinterpret_cast<void*>(&PlayHookUndo),reinterpret_cast<void**>(&g_playOrigUndo)))
        { rp::Log("play: the Undo_Undo detour failed; undo / redo do not republish"); return; }
        rp::Log("play: undo / redo follow-up armed (Undo_Undo detour): restored brushes publish, undone edits drop their live twin");
    }

    // The current hull of a brush that exists: unselected through its handle, selected through its instance.
    bool PlayCurrentHull(ModToolsBrush brush, ModToolsBrushInst inst, std::vector<PlayLivePlane>& planes)
    {
        constexpr size_t kCap=128;
        float raw[kCap*9]={};
        size_t faceCount=0;
        if (brush)
        {
            if (!rp::Has(&rp::host->BrushPlanes) || !rp::host->BrushPlanes) return false;
            __try { faceCount=rp::host->BrushPlanes(rp::self,brush,raw,kCap); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        else if (inst)
        {
            bool resolved=false;
            if (!PlayTryReadSettledBrushPlanes(inst,raw,kCap,faceCount,resolved) || !resolved) return false;
        }
        else return false;
        if (faceCount<4 || faceCount>kCap) return false;
        float mins[3], maxs[3];
        return PlayOrientHull(raw,faceCount,planes,mins,maxs);
    }

    bool PlayBakedPatchUnchanged(const std::string& guidKey)
    {
        if (!g_playBakedHullsLoaded) return false;
        auto baked=g_playBakedPatches.find(guidKey);
        if (baked==g_playBakedPatches.end()) return false;
        LONG rows=0, cols=0; std::vector<float> xyz;
        return PlayCurrentPatchGrid(guidKey,rows,cols,xyz) && PlaySameGrid(baked->second,rows,cols,xyz.data());
    }
    // A selected brush the compiled BSP contains with the hull it was compiled with: nothing to publish for it.
    bool PlayBakedHullUnchanged(const char* brushGuid, ModToolsBrushInst inst)
    {
        if (!g_playBakedHullsLoaded || !brushGuid || !brushGuid[0]) return false;
        auto baked=g_playBakedHulls.find(PlayLiveGuidKey(brushGuid));
        if (baked==g_playBakedHulls.end()) return false;
        std::vector<PlayLivePlane> current;
        if (!PlayCurrentHull(nullptr,inst,current)) return false;
        return PlaySameHull(baked->second.planes,current);
    }

    // Every 500 ms (at once after a settled edit): records and Live Edit objects with a baked hull are checked against the map. Absent or another hull -> hidden,
    // the baked hull again (undo) -> shown. Selected brushes count as present (FindBrushesByGuid reports them absent).
    // A moved prefab's interior brush as a live object at the live placement: the cached file-space faces and texdefs transformed, then the exact proxy. Texture shifts stay the file's.
    bool PlayBuildPrefabLiveObject(const PlayBakedPrefabHull& h, const PlayXform& x, PlayLiveLocalObject& out)
    {
        const size_t nFaces=h.localRaw.size()/9;
        if (nFaces<4 || nFaces>128) return false;
        std::vector<float> raw(h.localRaw.size());
        for (size_t i=0;i+2<raw.size();i+=3) PlayXformPoint(x,&h.localRaw[i],&raw[i]);
        std::vector<PlayLiveFaceTex> tex=h.localTex;
        for (PlayLiveFaceTex& t:tex) for (int k=0;k<3;++k) { float p[3]={t.pts[k].x,t.pts[k].y,t.pts[k].z}, q[3]; PlayXformPoint(x,p,q); t.pts[k]={q[0],q[1],q[2]}; }
        ModToolsSelectedBrush sel={};
        for (int a=0;a<3;++a) { sel.mins[a]=-1.0e6f; sel.maxs[a]=1.0e6f; }
        std::vector<PlayLiveVec3> verts; std::vector<brlive::Edge> edges; std::vector<brlive::Triangle> triangles;
        std::vector<PlayLiveFace> faces; std::vector<std::string> materials;
        PlayLiveGeometryStats stats;
        stats.geometryType=brlive::LIVE_GEOMETRY_BRUSH;
        stats.faceCount=static_cast<LONG>(nFaces);
        if (!PlayBuildExactBrushProxy(sel,raw.data(),nFaces,verts,edges,triangles,stats,tex.empty()?nullptr:&tex,&faces,&materials) || verts.empty() || edges.empty()) return false;
        out.sourceGuid=h.recordGuid;
        out.geometryType=brlive::LIVE_GEOMETRY_BRUSH;
        out.vertices.swap(verts); out.edges.swap(edges); out.triangles.swap(triangles);
        out.faces.swap(faces); out.materials.swap(materials);
        out.stats=stats;
        out.ownerClass=h.ownerClass;
        out.flags=brlive::LIVE_OBJECT_ACTIVE|brlive::LIVE_OBJECT_RENDER;
        out.fromPrefab=true;
        const size_t bar=h.fileKey.rfind('|');
        if (bar!=std::string::npos) out.prefabBrushKey=PlayLiveGuidKey(h.fileKey.c_str()+bar+1);
        return true;
    }
    // Puts the prefab's hulls into the registry at `live` (create or update), or takes them out. True when it changed.
    bool PlaySyncPrefabLiveObjects(PlayBakedPrefab& bp, bool wanted, bool moved, const PlayPrefabPlacement& live, size_t& skipped)
    {
        bool changed=false;
        if (!wanted)
        {
            if (!bp.liveObjects) return false;
            for (const std::string& key:bp.liveKeys)
            {
                auto found=g_playLiveRegistry.find(key);
                if (found==g_playLiveRegistry.end()) continue;
                g_playLiveLastOperation=brlive::LIVE_OBJECT_DELETE;
                g_playLiveLastOperationId=found->second.liveObjectId;
                g_playLiveLastOperationGeneration=static_cast<LONG>(found->second.generation+1);
                ++g_playLiveDeleteCount;
                g_playLiveRegistry.erase(found);
                changed=true;
            }
            bp.liveKeys.clear();
            bp.liveObjects=false;
            return changed;
        }
        const bool full=bp.isNew || moved;   // the whole interior at the live placement, else only what differs from the compiled interior
        bool same=bp.liveObjects && live.model==bp.live.model && bp.liveGeomGeneration==bp.geomGeneration && bp.liveFull==full;
        for (int a=0;a<3 && same;++a) if (std::fabs(live.origin[a]-bp.live.origin[a])>0.01f || std::fabs(live.angles[a]-bp.live.angles[a])>0.01f) same=false;
        if (same) return false;
        const bool fromCurrent=bp.isNew || bp.fileChanged;
        std::vector<const PlayBakedPrefabHull*> geom;
        for (const PlayBakedPrefabHull& h:(fromCurrent ? bp.current : bp.hulls)) if (full || (fromCurrent && h.changed)) geom.push_back(&h);
        std::vector<const PlayBakedPrefabPatch*> pgeom;
        for (const PlayBakedPrefabPatch& pp:(fromCurrent ? bp.currentPatches : bp.patches)) if (full || (fromCurrent && pp.changed)) pgeom.push_back(&pp);
        PlayXform x; PlayAnglesToAxis(live.angles,x.ax); for (int a=0;a<3;++a) x.o[a]=live.origin[a];
        std::vector<std::string> keys; keys.reserve(geom.size());
        for (const PlayBakedPrefabHull* h:geom) keys.push_back(PlayLiveGuidKey(h->recordGuid.c_str()));
        // patch tiles (built first so their keys join the set the old objects are checked against)
        std::vector<std::pair<std::string,PlayLiveLocalObject>> patchTiles;
        for (const PlayBakedPrefabPatch* pp:pgeom)
        {
            PlayPatchGrid g=pp->local;
            ModToolsSelectedBrush sel={};
            strncpy_s(sel.brushGuid,pp->recordGuid.c_str(),_TRUNCATE);
            for (size_t i=0;i<g.pts.size();++i)
            {
                const float in[3]={g.pts[i].x,g.pts[i].y,g.pts[i].z}; float w[3]; PlayXformPoint(x,in,w);
                g.pts[i]={w[0],w[1],w[2]};
                for (int a=0;a<3;++a) { if (i==0) { sel.mins[a]=sel.maxs[a]=w[a]; } else { sel.mins[a]=(std::min)(sel.mins[a],w[a]); sel.maxs[a]=(std::max)(sel.maxs[a],w[a]); } }
            }
            std::vector<PlayLiveLocalObject> tiles;
            PlayBuildPatchLiveObjects(sel,g,tiles);
            const std::string guidKey=PlayLiveGuidKey(pp->recordGuid.c_str());
            for (size_t t=0;t<tiles.size();++t) { tiles[t].fromPrefab=true; const std::string key=PlayPatchTileKey(guidKey,static_cast<LONG>(t)); keys.push_back(key); patchTiles.emplace_back(key,std::move(tiles[t])); }
        }
        for (const std::string& old:bp.liveKeys)   // objects of a geometry set that is no longer the one drawn
        {
            if (std::find(keys.begin(),keys.end(),old)!=keys.end()) continue;
            auto found=g_playLiveRegistry.find(old);
            if (found==g_playLiveRegistry.end()) continue;
            ++g_playLiveDeleteCount; g_playLiveRegistry.erase(found); changed=true;
        }
        auto upsert=[&](const std::string& key, PlayLiveLocalObject& candidate)
        {
            auto found=g_playLiveRegistry.find(key);
            if (found==g_playLiveRegistry.end())
            {
                if (g_playLiveRegistry.size()>=static_cast<size_t>(brlive::kMaxLiveObjects)) { ++skipped; return; }
                candidate.liveObjectId=PlayAllocateLiveObjectId();
                candidate.generation=1;
                candidate.operation=brlive::LIVE_OBJECT_CREATE;
                found=g_playLiveRegistry.emplace(key,std::move(candidate)).first;
                ++g_playLiveCreateCount;
            }
            else
            {
                candidate.liveObjectId=found->second.liveObjectId;
                candidate.generation=found->second.generation+1;
                candidate.operation=brlive::LIVE_OBJECT_UPDATE;
                found->second=std::move(candidate);
                ++g_playLiveUpdateCount;
            }
            g_playLiveLastOperation=found->second.operation;
            g_playLiveLastOperationId=found->second.liveObjectId;
            g_playLiveLastOperationGeneration=static_cast<LONG>(found->second.generation);
            changed=true;
        };
        for (const PlayBakedPrefabHull* h:geom)
        {
            PlayLiveLocalObject candidate;
            if (!PlayBuildPrefabLiveObject(*h,x,candidate)) { ++skipped; continue; }
            upsert(PlayLiveGuidKey(h->recordGuid.c_str()),candidate);
        }
        for (auto& pt:patchTiles) upsert(pt.first,pt.second);
        bp.live=live;
        bp.liveObjects=true;
        bp.liveFull=full;
        bp.liveGeomGeneration=bp.geomGeneration;
        bp.liveKeys.swap(keys);
        return changed;
    }

    // The prefab files as they are on disk now: every instance's `current` geometry is re-read (nested files included) and
    // compared with the compiled interior (hull count, planes, materials), a changed instance hides its baked interior and
    // draws the file's. Runs when a watched prefab file's write time changed (Ctrl+S inside an entered prefab writes it).
    bool PlaySamePrefabTex(const std::vector<PlayLiveFaceTex>& a, const std::vector<PlayLiveFaceTex>& b)
    {
        if (a.size()!=b.size()) return false;
        for (size_t k=0;k<a.size();++k)
        {
            const PlayLiveFaceTex& ta=a[k]; const PlayLiveFaceTex& tb=b[k];
            if (_stricmp(ta.material.c_str(),tb.material.c_str())!=0 || std::fabs(ta.hShift-tb.hShift)>0.01f || std::fabs(ta.vShift-tb.vShift)>0.01f || std::fabs(ta.rot-tb.rot)>0.01f || std::fabs(ta.hStretch-tb.hStretch)>0.01f || std::fabs(ta.vStretch-tb.vStretch)>0.01f) return false;
        }
        return true;
    }
    // Compiled against current, matched by file key (brush guid, patch guid, model entity within the prefab file, a file placed twice inside one instance matches in order). A compiled item with no current twin
    // or a different one is `stale` (hidden), a current item with no compiled twin or a different one is `changed` (drawn live). True when anything differs.
    bool PlayDiffPrefabGeometry(PlayBakedPrefab& bp)
    {
        bool differs=false;
        {
            std::multimap<std::string,size_t> cur; std::vector<bool> used(bp.current.size(),false);
            for (size_t i=0;i<bp.current.size();++i) { cur.emplace(bp.current[i].fileKey,i); bp.current[i].changed=true; }
            for (PlayBakedPrefabHull& a:bp.hulls)
            {
                a.stale=true;
                for (auto it=cur.lower_bound(a.fileKey); it!=cur.end() && it->first==a.fileKey; ++it)
                {
                    if (used[it->second]) continue;
                    PlayBakedPrefabHull& b=bp.current[it->second];
                    used[it->second]=true;
                    if (PlaySameHull(a.localPlanes,b.localPlanes) && PlaySamePrefabTex(a.localTex,b.localTex)) { a.stale=false; b.changed=false; }
                    break;
                }
                if (a.stale) differs=true;
            }
            for (const PlayBakedPrefabHull& b:bp.current) if (b.changed) differs=true;
        }
        {
            std::multimap<std::string,size_t> cur; std::vector<bool> used(bp.currentPatches.size(),false);
            for (size_t i=0;i<bp.currentPatches.size();++i) { cur.emplace(bp.currentPatches[i].fileKey,i); bp.currentPatches[i].changed=true; }
            for (PlayBakedPrefabPatch& a:bp.patches)
            {
                a.stale=true;
                for (auto it=cur.lower_bound(a.fileKey); it!=cur.end() && it->first==a.fileKey; ++it)
                {
                    if (used[it->second]) continue;
                    PlayBakedPrefabPatch& b=bp.currentPatches[it->second];
                    used[it->second]=true;
                    bool same=a.local.rows==b.local.rows && a.local.cols==b.local.cols && a.local.pts.size()==b.local.pts.size() && _stricmp(a.local.material.c_str(),b.local.material.c_str())==0;
                    for (size_t k=0;same && k<a.local.pts.size();++k)
                        if (PlayLiveLenSq(PlayLiveSub(a.local.pts[k],b.local.pts[k]))>0.01f || std::fabs(a.local.uv[k].first-b.local.uv[k].first)>1e-4f || std::fabs(a.local.uv[k].second-b.local.uv[k].second)>1e-4f) same=false;
                    if (same) { a.stale=false; b.changed=false; }
                    break;
                }
                if (a.stale) differs=true;
            }
            for (const PlayBakedPrefabPatch& b:bp.currentPatches) if (b.changed) differs=true;
        }
        {
            std::multimap<std::string,size_t> cur; std::vector<bool> used(bp.currentModels.size(),false);
            for (size_t i=0;i<bp.currentModels.size();++i) { cur.emplace(bp.currentModels[i].fileKey,i); bp.currentModels[i].changed=true; }
            for (PlayBakedPrefabModel& a:bp.models)
            {
                a.stale=true;
                for (auto it=cur.lower_bound(a.fileKey); it!=cur.end() && it->first==a.fileKey; ++it)
                {
                    if (used[it->second]) continue;
                    PlayBakedPrefabModel& b=bp.currentModels[it->second];
                    used[it->second]=true;
                    bool same=a.model==b.model && std::fabs(a.scale-b.scale)<0.001f;
                    for (int k=0;same && k<3;++k) if (std::fabs(a.localOrigin[k]-b.localOrigin[k])>0.05f) same=false;
                    for (int k=0;same && k<9;++k) if (std::fabs(a.localAxis[k]-b.localAxis[k])>0.001f) same=false;
                    if (same) { a.stale=false; b.changed=false; }
                    break;
                }
                if (a.stale) differs=true;
            }
            for (const PlayBakedPrefabModel& b:bp.currentModels) if (b.changed) differs=true;
        }
        return differs;
    }
    void PlayRefreshPrefabGeometry(const char* why)
    {
        const std::wstring mapSource=PlayJoin(PlayGameRoot(),L"map_source");
        std::map<std::string,std::string> fileCache;
        size_t changed=0, failed=0, files=0, missing=0;
        for (auto& pair:g_playBakedPrefabs)
        {
            PlayBakedPrefab& bp=pair.second;
            PlayBakedPrefab fresh; fresh.entityGuid=bp.entityGuid;
            std::vector<std::string> stack;
            PlayCollectPrefabHulls(mapSource,bp.compiled.model,PlayXform(),PlayXform(),0,stack,fileCache,pair.first,fresh,failed,files,missing);
            bp.current=std::move(fresh.hulls); bp.currentPatches=std::move(fresh.patches); bp.currentModels=std::move(fresh.models);
            const bool differs=bp.isNew ? false : PlayDiffPrefabGeometry(bp);
            if (differs!=bp.fileChanged || bp.isNew) ++changed;
            bp.fileChanged=differs;
            ++bp.geomGeneration;
        }
        char line[320]={};
        snprintf(line,sizeof line,"[LIVE DELETE] prefab files re-read (%s): %u file(s), %u instance(s) changed state, %u differ from the compiled map",why ? why : "",(unsigned)files,(unsigned)changed,(unsigned)std::count_if(g_playBakedPrefabs.begin(),g_playBakedPrefabs.end(),[](const std::pair<const std::string,PlayBakedPrefab>& p){ return p.second.fileChanged; }));
        PlayAppendLog(line);
    }
    bool PlayCheckPrefabFiles(uint64_t now)
    {
        if (g_playPrefabFileTimes.empty() || now-g_playLastPrefabFileCheck<500) return false;
        g_playLastPrefabFileCheck=now;
        const std::wstring mapSource=PlayJoin(PlayGameRoot(),L"map_source");
        bool changed=false;
        for (auto& pair:g_playPrefabFileTimes)
        {
            const uint64_t t=PlayFileTime(PlayJoin(mapSource,PlayUtf8ToWide(pair.first)));
            if (t && t!=pair.second) { pair.second=t; changed=true; }
        }
        if (!changed) return false;
        PlayRefreshPrefabGeometry("a prefab file was written");
        return true;
    }
    // misc_prefab entities placed after the Play (dragged in from the prefab browser): every entity GUID not seen since the
    // cache load is looked at once, a prefab gets an entry with no baked interior (isNew) and draws from its file.
    struct PlayEntityScan { std::vector<std::pair<ModToolsEntity,std::string>> fresh; };
    void PlayEntityScanFn(ModToolsEntity e, void* user)
    {
        char guid[40]={};
        if (!rp::host->EntityGuid(rp::self,e,guid) || !guid[0]) return;
        const std::string key=PlayLiveGuidKey(guid);
        if (g_playSeenEntityKeys.count(key)) return;
        static_cast<PlayEntityScan*>(user)->fresh.push_back({e,key});
    }
    bool PlayDiscoverNewPrefabs()
    {
        if (!rp::Has(&rp::host->ForEachEntity) || !rp::host->ForEachEntity || !rp::Has(&rp::host->EntityGuid) || !rp::host->EntityGuid) return false;
        PlayEntityScan scan;
        rp::host->ForEachEntity(rp::self,&PlayEntityScanFn,&scan);
        if (scan.fresh.empty()) return false;
        const std::wstring mapSource=PlayJoin(PlayGameRoot(),L"map_source");
        std::map<std::string,std::string> fileCache;
        size_t added=0;
        for (const auto& f:scan.fresh)
        {
            g_playSeenEntityKeys.insert(f.second);
            char* text=rp::host->WriteEntityText(rp::self,f.first);
            if (!text) continue;
            std::vector<std::pair<std::string,std::string>> kvs;
            PlayParseEntityKvs(text,kvs);
            rp::host->FreeString(rp::self,text);
            PlayFxParams fp; bool fxClass=false;
            if (PlayFxFromKvs(kvs,fp,&fxClass) || fxClass)   // without an fxdef yet it plays once it has one
            {
                // an fx placed after the Play: nothing compiled to stop, played live by the bridge
                char fguid[40]={}; rp::host->EntityGuid(rp::self,f.first,fguid);
                PlayBakedFx bf; bf.entityGuid=fguid; bf.compiled=fp; bf.isNew=true;
                g_playBakedFx[f.second]=std::move(bf);
                PlayAppendLog(std::string("[LIVE FX] new fx ")+fguid+" ("+fp.fxdef+") placed after the Play: played live");
                ++added;
                continue;
            }
            PlayDecalParams dp; bool decalClass=false;
            if (PlayDecalFromKvs(kvs,dp,&decalClass) || decalClass)   // without a material yet it is drawn once it has one
            {
                // a misc_volume_decal placed after the Play: nothing compiled to hide, drawn live by the bridge
                char dguid[40]={}; rp::host->EntityGuid(rp::self,f.first,dguid);
                PlayBakedDecal d; d.entityGuid=dguid; d.compiled=dp; d.isNew=true;
                g_playBakedDecals[f.second]=std::move(d);
                char line[320]={};
                snprintf(line,sizeof line,"[LIVE DECAL] new misc_volume_decal %s (%s) placed after the Play: drawn live",dguid,dp.material.c_str());
                PlayAppendLog(line);
                ++added;
                continue;
            }
            PlayBakedModel nm;
            if ((g_playLiveModels || g_playLiveDeleteModels) && PlayModelPlacementFromKvs(kvs,nm))
            {
                // a misc_model placed after the Play: nothing baked to hide, drawn live as a script_model
                char mguid[40]={}; rp::host->EntityGuid(rp::self,f.first,mguid);
                nm.entityGuid=mguid; nm.isNew=true;
                g_playBakedModels[f.second]=std::move(nm);
                char line[320]={};
                snprintf(line,sizeof line,"[LIVE MODEL] new misc_model %s (%s) placed after the Play: spawned live as a script_model",mguid,g_playBakedModels[f.second].model.c_str());
                PlayAppendLog(line);
                ++added;
                continue;
            }
            PlayPrefabPlacement p;
            if (!PlayPrefabPlacementFromKvs(kvs,p)) continue;
            PlayBakedPrefab bp;
            bp.isNew=true; bp.compiled=p;
            char guid[40]={}; rp::host->EntityGuid(rp::self,f.first,guid); bp.entityGuid=guid;
            std::vector<std::string> stack; size_t failed=0, files=0, missing=0;
            PlayCollectPrefabHulls(mapSource,p.model,PlayXform(),PlayXform(),0,stack,fileCache,f.second,bp,failed,files,missing);
            bp.current=std::move(bp.hulls); bp.hulls.clear(); bp.currentPatches=std::move(bp.patches); bp.patches.clear(); bp.currentModels=std::move(bp.models); bp.models.clear();
            for (auto& h:bp.current) h.changed=true; for (auto& pp:bp.currentPatches) pp.changed=true; for (auto& pm:bp.currentModels) pm.changed=true;
            bp.geomGeneration=1;
            const size_t n=bp.current.size();
            g_playBakedPrefabs[f.second]=std::move(bp);
            ++added;
            char line[320]={};
            snprintf(line,sizeof line,"[LIVE DELETE] new prefab instance %s (%s): %u interior brush(es) from its file, drawn live at its placement",guid,p.model.c_str(),(unsigned)n);
            PlayAppendLog(line);
        }
        return added!=0;
    }

    // misc_model entities: absent or moved (origin, angles, model, modelscale against the compiled values) -> hidden.
    bool PlayReconcileModelHides(uint64_t now, size_t& hiddenNow, size_t& shownAgain)
    {
        (void)now;
        if ((!g_playLiveDeleteModels && !g_playLiveModels) || !g_playBakedHullsLoaded) return false;
        if (!rp::Has(&rp::host->FindEntitiesByGuid) || !rp::host->FindEntitiesByGuid || !rp::Has(&rp::host->WriteEntityText) || !rp::host->WriteEntityText || !rp::Has(&rp::host->FreeString) || !rp::host->FreeString) return false;
        if (rp::Has(&rp::host->PrefabDepth) && rp::host->PrefabDepth && rp::host->PrefabDepth(rp::self)>0) return false;
        if (!g_playLiveDeletePrefabs) PlayDiscoverNewPrefabs();   // the prefab reconcile runs the new-entity scan otherwise
        g_playLiveModelWantedTop.clear();
        if (g_playBakedModels.empty()) { PlayRebuildLiveModelWanted(); return false; }
        std::vector<PlayBakedModel*> list; list.reserve(g_playBakedModels.size());
        std::vector<const char*> guids; guids.reserve(g_playBakedModels.size());
        for (auto& pair:g_playBakedModels) { list.push_back(&pair.second); guids.push_back(pair.second.entityGuid.c_str()); }
        std::vector<ModToolsEntity> entities(list.size(),nullptr);
        rp::host->FindEntitiesByGuid(rp::self,guids.data(),guids.size(),entities.data());
        bool changed=false;
        for (size_t i=0;i<list.size();++i)
        {
            PlayBakedModel& m=*list[i];
            bool want=false;
            if (!entities[i]) want=true;
            else
            {
                char* text=rp::host->WriteEntityText(rp::self,entities[i]);
                if (!text) continue;
                std::vector<std::pair<std::string,std::string>> kvs;
                PlayParseEntityKvs(text,kvs);
                rp::host->FreeString(rp::self,text);
                PlayBakedModel live;
                if (!PlayModelPlacementFromKvs(kvs,live)) want=true;
                else
                {
                    if (live.model!=m.model || std::fabs(live.scale-m.scale)>0.001f) want=true;
                    for (int a=0;a<3 && !want;++a)
                    {
                        if (std::fabs(live.origin[a]-m.origin[a])>0.05f) want=true;
                        float d=std::fmod(std::fabs(live.angles[a]-m.angles[a]),360.0f);
                        if (d>180.0f) d=360.0f-d;
                        if (d>0.05f) want=true;
                    }
                    if (g_playLiveModels && (want || m.isNew))   // moved, rescaled, another model, or placed after the Play: a live script_model
                    {
                        PlayLiveModel lm; lm.entityGuid=m.entityGuid; lm.model=live.model; lm.scale=live.scale;
                        for (int a=0;a<3;++a) { lm.origin[a]=live.origin[a]; lm.angles[a]=live.angles[a]; }
                        g_playLiveModelWantedTop.push_back(std::move(lm));
                    }
                }
            }
            if (m.isNew || !g_playLiveDeleteModels) want=false;   // nothing baked to hide
            if (m.hidden==want) continue;
            m.hidden=want; changed=true;
            if (want) ++hiddenNow; else ++shownAgain;
        }
        PlayRebuildLiveModelWanted();
        return changed;
    }

    // Live models: the wanted script_models (from the model reconcile) go to the game as dvars once the bridge confirms the level has each xmodel, a name it lacks is queued for a live zone like a missing material
    // (the "xmodel," prefix keeps it out of the carriers). The dvar set is resent when it changes or the game process changed. The mailbox is one file: a publish waits while another command is still in it.
    void PlayPublishLiveModels(uint64_t now)
    {
        if (!g_playLiveModels || !PlayLiveEditBuild() || !g_playLiveShared || !g_playLiveZoneInjected) return;
        brlive::SharedState* sh=g_playLiveShared;
        // 1. the distinct names, to the bridge. Slots are stable (rebuilding them in map order shifted the other names, reset their answers and made every live model blink): a name keeps its slot while wanted,
        // a freed slot takes the next new name and is reset to "not looked up" first.
        const size_t kNameSlots=(size_t)brlive::kMaxLiveModelNames;
        if (g_playLiveModelSlotNames.size()!=kNameSlots) g_playLiveModelSlotNames.assign(kNameSlots,std::string());
        std::vector<std::string> names;
        for (const auto& w:g_playLiveModelWanted) if (std::find(names.begin(),names.end(),w.model)==names.end()) names.push_back(w.model);
        bool slotsChanged=false;
        auto writeSlot=[&](size_t i, const std::string& name)
        {
            InterlockedExchange(&sh->liveModelState[i],0);
            std::memset(sh->liveModelNames[i],0,sizeof sh->liveModelNames[i]);
            strncpy_s(sh->liveModelNames[i],name.c_str(),_TRUNCATE);
            g_playLiveModelSlotNames[i]=name; slotsChanged=true;
        };
        for (size_t i=0;i<kNameSlots;++i)
            if (!g_playLiveModelSlotNames[i].empty() && std::find(names.begin(),names.end(),g_playLiveModelSlotNames[i])==names.end()) writeSlot(i,std::string());
        for (const auto& name:names)
        {
            if (std::find(g_playLiveModelSlotNames.begin(),g_playLiveModelSlotNames.end(),name)!=g_playLiveModelSlotNames.end()) continue;
            size_t free=0; while (free<kNameSlots && !g_playLiveModelSlotNames[free].empty()) ++free;
            if (free>=kNameSlots) break;   // more distinct models than slots: the rest wait
            writeSlot(free,name);
        }
        if (slotsChanged)
        {
            LONG count=0; for (size_t i=0;i<kNameSlots;++i) if (!g_playLiveModelSlotNames[i].empty()) count=(LONG)i+1;
            MemoryBarrier();
            sh->liveModelNameCount=count;
        }
        // 2. the answers, a missing model is queued for a live zone (and the patch zone list for the next Play)
        std::map<std::string,LONG> state;
        for (size_t i=0;i<kNameSlots;++i)
        {
            const std::string& name=g_playLiveModelSlotNames[i];
            if (name.empty()) continue;
            const LONG st=sh->liveModelState[i];
            state[name]=st;
            if (st!=2) continue;
            const std::string key=g_playCurrentMap+"|xmodel,"+name;
            if (g_playLiveModelQueued.count(key)) continue;
            g_playLiveModelQueued.insert(key);
            PlayMapInfo m; std::string error;
            if (!PlayResolveCurrentMap(m,error)) { PlayAppendLog("[LIVE MODEL] "+name+" is not in the level and the map could not be resolved: "+error); continue; }
            if (g_playLiveZone) PlayAddLiveMaterial(m,"xmodel,"+name);
            if (g_playLiveMaterialLoad && g_playAutoLinkMaterials)
            {
                g_playLiveMatPending.push_back("xmodel,"+name);
                if (!g_playLiveMaterialsDueMs) g_playLiveMaterialsDueMs=now+3000;
                PlayAppendLog("[LIVE MODEL] the level has no xmodel "+name+": linking it as a live zone in a few seconds (and into the patch zone for the next Play).");
            }
            else PlayAppendLog("[LIVE MODEL] the level has no xmodel "+name+": in the patch zone from the next Play on (live_material_load=0 or auto_link_materials=0).");
        }
        // 3. the dvars. Record slots are stable too: an entity keeps its index (and so its script_model) while it is
        // wanted and its model is usable, a freed index goes to the next entity, an empty index is sent as "" so the
        // receiver deletes that script_model.
        const size_t kRecordSlots=64;
        if (g_playLiveModelRecordSlots.size()!=kRecordSlots) g_playLiveModelRecordSlots.assign(kRecordSlots,std::string());
        std::map<std::string,const PlayLiveModel*> usable; size_t waiting=0, unavailable=0;
        for (const auto& w:g_playLiveModelWanted)
        {
            const LONG st=state.count(w.model) ? state[w.model] : 0;
            if (st==1) usable[PlayLiveGuidKey(w.entityGuid.c_str())]=&w;
            else if (st==3) ++unavailable;
            else ++waiting;
        }
        for (auto& slot:g_playLiveModelRecordSlots) if (!slot.empty() && !usable.count(slot)) slot.clear();
        for (const auto& u:usable)
        {
            if (std::find(g_playLiveModelRecordSlots.begin(),g_playLiveModelRecordSlots.end(),u.first)!=g_playLiveModelRecordSlots.end()) continue;
            size_t free=0; while (free<kRecordSlots && !g_playLiveModelRecordSlots[free].empty()) ++free;
            if (free>=kRecordSlots) break;
            g_playLiveModelRecordSlots[free]=u.first;
        }
        std::vector<std::string> cmds; size_t n=0, sent=0;
        for (size_t i=0;i<kRecordSlots;++i) if (!g_playLiveModelRecordSlots[i].empty()) n=i+1;
        for (size_t i=0;i<n;++i)
        {
            const std::string& guidKey=g_playLiveModelRecordSlots[i];
            if (guidKey.empty()) { cmds.push_back("set br_live_m"+std::to_string(i)+" \"\""); continue; }
            const PlayLiveModel& w=*usable[guidKey];
            char buf[320]={};
            snprintf(buf,sizeof buf,"set br_live_m%u \"%s|%.2f %.2f %.2f|%.2f %.2f %.2f|%.3f\"",(unsigned)i,w.model.c_str(),
                     w.origin[0],w.origin[1],w.origin[2],w.angles[0],w.angles[1],w.angles[2],w.scale);
            cmds.push_back(buf); ++sent;
        }
        std::string key; for (const auto& c:cmds) { key+=c; key+='\n'; }
        if (key==g_playLiveModelSent && g_playLiveModelSentPid==g_playGamePid) return;
        if (PlayFileExists(PlayJoin(PlayThisModuleDir(),L"BO3LiveZone.cmd"))) return;   // another command is still in the mailbox: next tick
        cmds.push_back("set br_live_mn "+std::to_string(n));
        cmds.push_back(std::string("set br_live_debug ")+(g_playLiveModelsDebug ? "1" : "0"));
        cmds.push_back("set br_live_mgen "+std::to_string(++g_playLiveModelGen));
        if (!PlayLiveZoneSendCommands(cmds)) return;
        g_playLiveModelSent=key; g_playLiveModelSentPid=g_playGamePid; g_playLiveModelSentCount=sent;
        char line[400]={};
        snprintf(line,sizeof line,"[LIVE MODEL] generation %ld: %u script_model(s) in the game (moved or placed misc_models), %u waiting for the level to load their model, %u with no lookup",(long)g_playLiveModelGen,(unsigned)sent,(unsigned)waiting,(unsigned)unavailable);
        PlayAppendLog(line);
    }

    // ---- Edits inside an entered prefab, see the globals above ----
    std::string PlayPrefabKeyNorm(const std::string& in)
    {
        std::string s=PlayLowerAscii(in);
        std::replace(s.begin(),s.end(),'\\','/');
        const size_t ms=s.find("/map_source/");
        if (ms!=std::string::npos) s=s.substr(ms+12);
        while (s.rfind("./",0)==0) s.erase(0,2);
        return s;
    }
    int PlayPrefabDepth()
    {
        if (!rp::host || !rp::Has(&rp::host->PrefabDepth) || !rp::host->PrefabDepth) return 0;
        return rp::host->PrefabDepth(rp::self);
    }
    // id's AxisToAngles: the inverse of PlayAnglesToAxis (yaw about z, pitch about y, roll about x).
    void PlayAxisToAngles(const float ax[9], float angles[3])
    {
        const float r2d=180.0f/3.14159265358979f;
        const float fx=ax[0], fy=ax[1], fz=ax[2];
        const float yaw=std::atan2(fy,fx);
        float sp=-fz; if (sp>1.0f) sp=1.0f; if (sp<-1.0f) sp=-1.0f;
        const float pitch=std::asin(sp);
        const float sy=std::sin(yaw), cy=std::cos(yaw), cp=std::cos(pitch);
        const float l0[3]={-sy,cy,0.0f}, u0[3]={sp*cy,sp*sy,cp};     // left and up with no roll
        const float lx=ax[3], ly=ax[4], lz=ax[5];
        const float roll=std::atan2(lx*u0[0]+ly*u0[1]+lz*u0[2], lx*l0[0]+ly*l0[1]+lz*l0[2]);
        angles[0]=pitch*r2d; angles[1]=yaw*r2d; angles[2]=roll*r2d;
    }
    PlayXform PlayXformMul(const PlayXform& outer, const float o[3], const float ax[9])
    {
        PlayXform r;
        PlayXformPoint(outer,o,r.o);
        for (int i=0;i<3;++i)
            for (int a=0;a<3;++a) r.ax[i][a]=ax[i*3+0]*outer.ax[0][a]+ax[i*3+1]*outer.ax[1][a]+ax[i*3+2]*outer.ax[2][a];
        return r;
    }
    // The wanted live script_models: the map's own moved or new misc_models plus the interior models of moved, new or
    // edited prefab instances (the whole interior at the live placement, else the models that differ from the compiled ones).
    void PlayRebuildLiveModelWanted()
    {
        g_playLiveModelWanted=g_playLiveModelWantedTop;
        if (!g_playLiveModels) return;
        for (auto& pair:g_playBakedPrefabs)
        {
            const PlayBakedPrefab& bp=pair.second;
            const bool full=bp.isNew || bp.hidden;
            if (!full && !bp.fileChanged) continue;
            const PlayPrefabPlacement& pl=bp.liveObjects ? bp.live : bp.compiled;
            if (pl.model!=bp.compiled.model && !bp.isNew) continue;   // swapped to another file: nothing of this interior is drawn
            PlayXform x; PlayAnglesToAxis(pl.angles,x.ax); for (int a=0;a<3;++a) x.o[a]=pl.origin[a];
            const bool fromCurrent=bp.isNew || bp.fileChanged;
            for (const PlayBakedPrefabModel& pm:(fromCurrent ? bp.currentModels : bp.models))
            {
                if (!full && !(fromCurrent && pm.changed)) continue;
                const PlayXform w=PlayXformMul(x,pm.localOrigin,pm.localAxis);
                PlayLiveModel lm; lm.entityGuid=pm.recordGuid; lm.model=pm.model; lm.scale=pm.scale;
                for (int a=0;a<3;++a) lm.origin[a]=w.o[a];
                float ax9[9]; for (int i=0;i<3;++i) for (int a=0;a<3;++a) ax9[i*3+a]=w.ax[i][a];
                PlayAxisToAngles(ax9,lm.angles);
                g_playLiveModelWanted.push_back(std::move(lm));
            }
        }
    }
    // LIVE SPOTS: compiled place -> live place of every script_struct inside a moved prefab instance, as dvars for the
    // GSC receiver (PlayConfigureLivePreview). Sent when the set changes or the game process changed, a record that
    // leaves (the prefab is back, or a Play compiled the new place) makes the receiver put its things back.
    bool g_playLiveSpots = true;                 // [playtest] live_spots
    std::string g_playLiveSpotSent;
    DWORD g_playLiveSpotSentPid = 0;
    LONG g_playLiveSpotGen = 0;
    void PlayPublishLiveSpots()
    {
        if (!g_playLiveSpots || !PlayLiveEditBuild() || !g_playLiveShared || !g_playLiveZoneInjected) return;
        const size_t kMaxSpots=48;
        std::vector<std::string> cmds; size_t prefabs=0;
        for (const auto& pair:g_playBakedPrefabs)
        {
            const PlayBakedPrefab& bp=pair.second;
            if (bp.isNew || !bp.hidden || !bp.liveObjects || bp.live.model!=bp.compiled.model || bp.spots.empty()) continue;
            PlayXform from, to;
            PlayAnglesToAxis(bp.compiled.angles,from.ax); PlayAnglesToAxis(bp.live.angles,to.ax);
            for (int a=0;a<3;++a) { from.o[a]=bp.compiled.origin[a]; to.o[a]=bp.live.origin[a]; }
            bool any=false;
            for (const PlayBakedPrefabSpot& ps:bp.spots)
            {
                if (cmds.size()>=kMaxSpots) break;
                const PlayXform a=PlayXformMul(from,ps.localOrigin,ps.localAxis), b=PlayXformMul(to,ps.localOrigin,ps.localAxis);
                float a9[9], b9[9], aa[3], ba[3];
                for (int i=0;i<3;++i) for (int k=0;k<3;++k) { a9[i*3+k]=a.ax[i][k]; b9[i*3+k]=b.ax[i][k]; }
                PlayAxisToAngles(a9,aa); PlayAxisToAngles(b9,ba);
                char buf[320]={};
                snprintf(buf,sizeof buf,"set br_live_s%u \"%.2f %.2f %.2f|%.2f %.2f %.2f|%.2f %.2f %.2f|%.2f %.2f %.2f\"",(unsigned)cmds.size(),
                         a.o[0],a.o[1],a.o[2],aa[0],aa[1],aa[2],b.o[0],b.o[1],b.o[2],ba[0],ba[1],ba[2]);
                cmds.push_back(buf); any=true;
            }
            if (any) ++prefabs;
        }
        std::string key; for (const auto& c:cmds) { key+=c; key+='\n'; }
        if (key==g_playLiveSpotSent && g_playLiveSpotSentPid==g_playGamePid) return;
        if (PlayFileExists(PlayJoin(PlayThisModuleDir(),L"BO3LiveZone.cmd"))) return;   // another command is still in the mailbox: next pass
        const size_t n=cmds.size();
        cmds.push_back("set br_live_sn "+std::to_string(n));
        cmds.push_back("set br_live_sgen "+std::to_string(++g_playLiveSpotGen));
        if (!PlayLiveZoneSendCommands(cmds)) return;
        const bool first=g_playLiveSpotSent.empty() && n==0;
        g_playLiveSpotSent=key; g_playLiveSpotSentPid=g_playGamePid;
        if (first) return;   // nothing moved yet: no line
        char line[320]={};
        snprintf(line,sizeof line,"[LIVE SPOT] generation %ld: %u script struct(s) of %u moved prefab(s): what the level's scripts spawned on them (perk machines, their triggers and clip) follows",(long)g_playLiveSpotGen,(unsigned)n,(unsigned)prefabs);
        PlayAppendLog(line);
    }
    struct PlayScopeCaptureCtx { std::string text; size_t entities=0; };
    void PlayScopeCaptureFn(ModToolsEntity e, void* user)
    {
        auto* ctx=static_cast<PlayScopeCaptureCtx*>(user);
        char* text=rp::host->WriteEntityText(rp::self,e);
        if (!text) return;
        ctx->text+=text; if (ctx->text.empty() || ctx->text.back()!='\n') ctx->text+='\n';
        rp::host->FreeString(rp::self,text);
        ++ctx->entities;
    }
    // The entered prefab's content from Radiant's entity list (its own list while inside). True when it differs from the
    // override kept so far (the instances are refreshed then).
    bool PlayScopeCapture(const char* why)
    {
        if (g_playScopeDepth<=0 || g_playScopeKey.empty() || !rp::host || !rp::Has(&rp::host->ForEachEntity) || !rp::host->ForEachEntity ||
            !rp::Has(&rp::host->WriteEntityText) || !rp::host->WriteEntityText || !rp::Has(&rp::host->FreeString) || !rp::host->FreeString) return false;
        PlayScopeCaptureCtx ctx;
        rp::host->ForEachEntity(rp::self,&PlayScopeCaptureFn,&ctx);
        if (ctx.text.empty()) return false;
        uint64_t h=1469598103934665603ull;
        for (unsigned char c:ctx.text) { h^=c; h*=1099511628211ull; }
        ++g_playScopeCaptures;
        auto old=g_playPrefabScopeHash.find(g_playScopeKey);
        if (old!=g_playPrefabScopeHash.end() && old->second==h) return false;
        g_playPrefabScopeHash[g_playScopeKey]=h;
        g_playPrefabScopeText[g_playScopeKey]=std::move(ctx.text);
        char line[400]={};
        snprintf(line,sizeof line,"[LIVE PREFAB] %s: %s captured from Radiant (%u entities, %u bytes); the instances of the file are refreshed from it",why,g_playScopeKey.c_str(),(unsigned)ctx.entities,(unsigned)g_playPrefabScopeText[g_playScopeKey].size());
        PlayAppendLog(line);
        if (g_playBakedHullsLoaded) PlayRefreshPrefabGeometry("edited inside the prefab");
        return true;
    }
    // Inside a prefab: capture the scope once the edits settled, then keep every instance's live objects and hide sets in
    // step (placements are the last known ones: nothing outside can move while inside).
    bool PlayInsidePrefabPass(uint64_t now, bool& registryChanged, size_t& objectsSkipped)
    {
        if (g_playScopeDirty && now-g_playScopeDirtyAt>=300 && !PlayAnyEditorMouseButtonDown() && !g_playInputCaptured)
        {
            g_playScopeDirty=false;
            PlayScopeCapture("settled edit");
        }
        bool changed=false;
        for (auto& pair:g_playBakedPrefabs)
        {
            PlayBakedPrefab& bp=pair.second;
            const PlayPrefabPlacement& pl=bp.liveObjects ? bp.live : bp.compiled;
            const bool drawLive=(pl.model==bp.compiled.model || bp.isNew) && (bp.hidden || bp.isNew || bp.fileChanged);
            if (PlaySyncPrefabLiveObjects(bp,drawLive,bp.hidden,pl,objectsSkipped)) registryChanged=true;
            if (bp.reconciledGeneration!=bp.geomGeneration || bp.reconciledFileChanged!=bp.fileChanged)
            { bp.reconciledGeneration=bp.geomGeneration; bp.reconciledFileChanged=bp.fileChanged; changed=true; }
        }
        if (changed || registryChanged) { PlayRebuildLiveModelWanted(); PlayPublishModelHides(); }
        return changed;
    }
    // Prefab instances: the live entity's placement (WriteEntityText: origin, angles, model) against the compiled one.
    // Absent or moved -> its interior hulls are hidden in the game, back (undo) -> shown. Skipped inside a prefab scope
    // (the entity list and the writer belong to the entered prefab there). Returns true when the record set changed.
    bool PlayReconcilePrefabHides(uint64_t now, size_t& hiddenNow, size_t& shownAgain, bool& registryChanged, size_t& objectsSkipped)
    {
        if (!g_playLiveDeletePrefabs || !g_playBakedHullsLoaded) return false;
        if (!rp::Has(&rp::host->FindEntitiesByGuid) || !rp::host->FindEntitiesByGuid || !rp::Has(&rp::host->WriteEntityText) || !rp::host->WriteEntityText || !rp::Has(&rp::host->FreeString) || !rp::host->FreeString) return false;
        if (PlayPrefabDepth()>0) return PlayInsidePrefabPass(now,registryChanged,objectsSkipped);   // the entity list is the entered prefab's: its content, not the instances
        PlayDiscoverNewPrefabs();
        PlayCheckPrefabFiles(now);
        if (g_playBakedPrefabs.empty()) return false;
        std::vector<PlayBakedPrefab*> list; list.reserve(g_playBakedPrefabs.size());
        std::vector<const char*> guids; guids.reserve(g_playBakedPrefabs.size());
        for (auto& pair:g_playBakedPrefabs) { list.push_back(&pair.second); guids.push_back(pair.second.entityGuid.c_str()); }
        std::vector<ModToolsEntity> entities(list.size(),nullptr);
        rp::host->FindEntitiesByGuid(rp::self,guids.data(),guids.size(),entities.data());
        bool changed=false;
        for (size_t i=0;i<list.size();++i)
        {
            PlayBakedPrefab& bp=*list[i];
            bool want=false, drawLive=false;
            PlayPrefabPlacement live;
            if (!entities[i]) want=true;   // deleted
            else
            {
                char* text=rp::host->WriteEntityText(rp::self,entities[i]);
                if (!text) continue;       // unreadable this pass: keep the state
                std::vector<std::pair<std::string,std::string>> kvs;
                PlayParseEntityKvs(text,kvs);
                rp::host->FreeString(rp::self,text);
                if (!PlayPrefabPlacementFromKvs(kvs,live)) want=true;   // not a prefab any more
                else
                {
                    if (live.model!=bp.compiled.model) want=true;
                    for (int a=0;a<3 && !want;++a)
                    {
                        if (std::fabs(live.origin[a]-bp.compiled.origin[a])>0.05f) want=true;
                        float d=std::fmod(std::fabs(live.angles[a]-bp.compiled.angles[a]),360.0f);
                        if (d>180.0f) d=360.0f-d;
                        if (d>0.05f) want=true;
                    }
                    drawLive=live.model==bp.compiled.model && (want || bp.isNew || bp.fileChanged);   // moved or new: the whole interior at the live placement, edited: the changed part
                }
            }
            if (bp.isNew) want=false;                                    // nothing baked to hide
            if (PlaySyncPrefabLiveObjects(bp,drawLive,want,live,objectsSkipped)) registryChanged=true;
            if (bp.reconciledGeneration!=bp.geomGeneration || bp.reconciledFileChanged!=bp.fileChanged)
            { bp.reconciledGeneration=bp.geomGeneration; bp.reconciledFileChanged=bp.fileChanged; changed=true; }   // the per-brush hide set changed
            if (bp.hidden==want) continue;
            bp.hidden=want; changed=true;
            if (want) { bp.since=now; ++hiddenNow; } else ++shownAgain;
        }
        return changed;
    }

    // Patches: every patch record and every registry tile whose patch the compiled map contains is checked like a brush: gone from the map -> hidden,
    // present with another grid (sculpted, moved, rows added) -> hidden, present with the compiled grid (undo) -> shown again. Records for edited patches come from the cache on demand.
    void PlayReconcilePatchHides(uint64_t now)
    {
        std::set<std::string> keys;
        for (const auto& pair:g_playLivePatchHides) keys.insert(pair.first);
        if (g_playBakedHullsLoaded)
            for (const auto& pair:g_playLiveRegistry)
                if (pair.second.geometryType==brlive::LIVE_GEOMETRY_PATCH && PlayRegistryKeyIsTile(pair.first))
                {
                    const std::string guidKey=PlayRegistryKeyGuid(pair.first);
                    if (g_playBakedPatches.count(guidKey)) keys.insert(guidKey);
                }
        if (keys.empty()) return;
        size_t selectionCount=0;
        std::set<std::string> selectedKeys;
        if (PlayTryFillSettledSelection(nullptr,0,selectionCount) && selectionCount && selectionCount<=4096)
        {
            std::vector<ModToolsSelectedBrush> selections(selectionCount);
            size_t selectedRead=0;
            if (PlayTryFillSettledSelection(selections.data(),selections.size(),selectedRead))
                for (size_t i=0;i<(std::min)(selectedRead,selections.size());++i) selectedKeys.insert(PlayLiveGuidKey(selections[i].brushGuid));
        }
        std::vector<std::string> keyList(keys.begin(),keys.end());
        std::vector<const char*> guids; guids.reserve(keyList.size());
        for (const std::string& key:keyList) guids.push_back(key.c_str());
        std::vector<ModToolsBrush> brushes(keyList.size(),nullptr);
        if (!PlayTryFindLiveBrushes(guids.data(),guids.size(),brushes.data())) return;
        size_t hiddenNow=0, shownAgain=0, created=0;
        bool changed=false;
        std::vector<float> xyz;
        for (size_t i=0;i<keyList.size();++i)
        {
            const std::string& key=keyList[i];
            const bool present=brushes[i]!=nullptr || selectedKeys.count(key)!=0;
            auto record=g_playLivePatchHides.find(key);
            auto baked=g_playBakedPatches.find(key);
            bool want=false;
            if (!present) want=true;
            else
            {
                const PlayBakedPatch* reference=nullptr;
                if (baked!=g_playBakedPatches.end()) reference=&baked->second;
                else if (record!=g_playLivePatchHides.end()) reference=&record->second.grid;
                LONG rows=0, cols=0;
                if (reference && PlayCurrentPatchGrid(key,rows,cols,xyz)) want=!PlaySameGrid(*reference,rows,cols,xyz.data());
                else if (record!=g_playLivePatchHides.end()) want=record->second.active;
                else want=false;
            }
            if (record==g_playLivePatchHides.end())
            {
                if (!want || baked==g_playBakedPatches.end() || g_playLivePatchHides.size()>=static_cast<size_t>(brlive::kMaxPatchHides)) continue;
                PlayLivePatchHide hide;
                if (!PlayPatchHideFromBaked(key,hide)) continue;
                g_playLivePatchHides.emplace(key,std::move(hide));
                ++created; changed=true;
                continue;
            }
            if (record->second.active!=want)
            {
                record->second.active=want;
                if (want) { ++hiddenNow; record->second.since=now; } else ++shownAgain;
                changed=true;
            }
        }
        if (!changed) return;
        PlayPublishLiveHides();
        char line[320]={};
        snprintf(line,sizeof line,"[LIVE PATCH] %u edited patch(es) hidden in the game, %u hidden again, %u shown again (back to their compiled grid)",
            (unsigned)created,(unsigned)hiddenNow,(unsigned)shownAgain);
        PlayAppendLog(line);
    }


    // ---- Hole fills, see the globals by PlayBakedHull ----
    struct PlayFillHull { std::string guid; const std::vector<PlayLivePlane>* planes; const float* mins; const float* maxs; std::vector<PlayLiveFaceTex> tex; };
    // Tool materials draw nothing in the game: a brush made only of them (a volume, a clip, a trigger) never had faces to
    // lose, and a tool face of a real brush is given the brush's first drawn material instead.
    bool PlayToolMaterial(const std::string& m)
    {
        return PlayMaterialPropsFor(m).noDraw;   // gdt.db's noDraw (sky, portal), see "Material visibility and collision"
    }
    bool PlayBuildFillObject(const PlayFillHull& n, const PlayFillHull& h, const std::string& sourceGuid, PlayLiveLocalObject& out, const char** why)
    {
        std::vector<float> raw; std::vector<PlayLiveFaceTex> tex; *why="";
        // three points on the plane near the hidden hull (the builder orients planes from the points of the other faces,
        // so they must sit by the brush, not at the plane's closest point to the origin)
        const PlayLiveVec3 centre={(h.mins[0]+h.maxs[0])*0.5f,(h.mins[1]+h.maxs[1])*0.5f,(h.mins[2]+h.maxs[2])*0.5f};
        float wind=1.0f;   // Radiant's face winding against the outward normal, learned from N's own faces (the builder flips all planes together)
        auto plane3=[&](const PlayLivePlane& p, float d)
        {
            const PlayLiveVec3 nn=p.n;
            PlayLiveVec3 t1=std::fabs(nn.z)<0.9f ? PlayLiveVec3{0,0,1} : PlayLiveVec3{1,0,0};
            t1=PlayLiveCross(nn,t1); const float l=std::sqrt(PlayLiveLenSq(t1)); if (l<1e-6f) return false; t1=PlayLiveMul(t1,1.0f/l);
            const PlayLiveVec3 t2=PlayLiveCross(nn,t1);
            const PlayLiveVec3 p0=PlayLiveSub(centre,PlayLiveMul(nn,PlayLiveDot(nn,centre)-d));
            const PlayLiveVec3 pa=wind>0.0f ? PlayLiveVec3{p0.x+t1.x*8,p0.y+t1.y*8,p0.z+t1.z*8} : PlayLiveVec3{p0.x+t2.x*8,p0.y+t2.y*8,p0.z+t2.z*8};
            const PlayLiveVec3 pb=wind>0.0f ? PlayLiveVec3{p0.x+t2.x*8,p0.y+t2.y*8,p0.z+t2.z*8} : PlayLiveVec3{p0.x+t1.x*8,p0.y+t1.y*8,p0.z+t1.z*8};
            const float pts[9]={p0.x,p0.y,p0.z, pa.x,pa.y,pa.z, pb.x,pb.y,pb.z};   // cross(pa-p0, pb-p0) = wind * n
            raw.insert(raw.end(),pts,pts+9);
            return true;
        };
        if (n.tex.empty()) { *why="no texdefs"; return false; }
        const PlayLiveFaceTex* drawn=nullptr;
        for (const PlayLiveFaceTex& t:n.tex) if (!PlayToolMaterial(t.material)) { drawn=&t; break; }
        if (!drawn) { *why="tool brush"; return false; }
        bool windKnown=false; std::vector<const PlayLivePlane*> generated;
        for (const PlayLivePlane& p:*n.planes)
        {
            // the face's own texdef points (the proxy matches a texdef by its three points and takes the texture axes
            // from their winding), moved kPlayFillInset into N along the outward normal
            const PlayLiveFaceTex* own=nullptr;
            for (const PlayLiveFaceTex& t:n.tex)
            {
                bool on=true;
                for (int k=0;k<3 && on;++k) { const PlayLiveVec3 q={t.pts[k].x,t.pts[k].y,t.pts[k].z}; if (std::fabs(PlayLiveDot(p.n,q)-p.d)>0.5f) on=false; }
                if (on) { own=&t; break; }
            }
            PlayLiveFaceTex t=own ? *own : n.tex[0];
            if (own)
            {
                if (!windKnown)
                {
                    const PlayLiveVec3 a={own->pts[0].x,own->pts[0].y,own->pts[0].z}, b={own->pts[1].x,own->pts[1].y,own->pts[1].z}, c={own->pts[2].x,own->pts[2].y,own->pts[2].z};
                    const float f=PlayLiveDot(PlayLiveCross(PlayLiveSub(b,a),PlayLiveSub(c,a)),p.n);
                    if (std::fabs(f)>1e-6f) { wind=f>0.0f ? 1.0f : -1.0f; windKnown=true; }
                }
                for (int k=0;k<3;++k) { t.pts[k]={t.pts[k].x-p.n.x*kPlayFillInset, t.pts[k].y-p.n.y*kPlayFillInset, t.pts[k].z-p.n.z*kPlayFillInset}; const float q[3]={t.pts[k].x,t.pts[k].y,t.pts[k].z}; raw.insert(raw.end(),q,q+3); }
                if (PlayToolMaterial(t.material)) t.material=drawn->material;
                tex.push_back(t);
            }
            else generated.push_back(&p);   // no texdef on this plane: generated after the winding is known
        }
        if (!windKnown) { *why="no texdef on any plane"; return false; }
        for (const PlayLivePlane* p:generated)
        {
            if (!plane3(*p,p->d-kPlayFillInset)) { *why="degenerate plane"; return false; }
            PlayLiveFaceTex t=*drawn;
            for (int k=0;k<3;++k) t.pts[k]={raw[raw.size()-9+k*3],raw[raw.size()-9+k*3+1],raw[raw.size()-9+k*3+2]};
            tex.push_back(t);
        }
        for (const PlayLivePlane& p:*h.planes)   // H's grown planes close the slab inside N: never visible, N's first material
        {
            if (!plane3(p,p.d+kPlayFillExpand)) { *why="degenerate plane"; return false; }
            PlayLiveFaceTex t=*drawn;
            for (int k=0;k<3;++k) t.pts[k]={raw[raw.size()-9+k*3],raw[raw.size()-9+k*3+1],raw[raw.size()-9+k*3+2]};
            tex.push_back(t);
        }
        ModToolsSelectedBrush sel={};
        strncpy_s(sel.brushGuid,sourceGuid.c_str(),_TRUNCATE);
        for (int a=0;a<3;++a) { sel.mins[a]=-1.0e6f; sel.maxs[a]=1.0e6f; }
        std::vector<PlayLiveVec3> verts; std::vector<brlive::Edge> edges; std::vector<brlive::Triangle> triangles;
        std::vector<PlayLiveFace> faces; std::vector<std::string> materials;
        PlayLiveGeometryStats stats;
        stats.geometryType=brlive::LIVE_GEOMETRY_BRUSH;
        stats.faceCount=static_cast<LONG>(raw.size()/9);
        if (!PlayBuildExactBrushProxy(sel,raw.data(),raw.size()/9,verts,edges,triangles,stats,&tex,&faces,&materials) || verts.size()<4 || triangles.empty()) { *why="no volume"; return false; }
        // a sliver where N only touches H along an edge or corner has no face worth drawing: two extents of at least 2 units
        float mn[3]={verts[0].x,verts[0].y,verts[0].z}, mx[3]={verts[0].x,verts[0].y,verts[0].z};
        for (const PlayLiveVec3& v:verts) { mn[0]=(std::min)(mn[0],v.x); mx[0]=(std::max)(mx[0],v.x); mn[1]=(std::min)(mn[1],v.y); mx[1]=(std::max)(mx[1],v.y); mn[2]=(std::min)(mn[2],v.z); mx[2]=(std::max)(mx[2],v.z); }
        int wide=0; for (int a=0;a<3;++a) if (mx[a]-mn[a]>=2.0f) ++wide;
        if (wide<2) { *why="sliver"; return false; }
        out.sourceGuid=sourceGuid;
        out.geometryType=brlive::LIVE_GEOMETRY_BRUSH;
        out.vertices.swap(verts); out.edges.swap(edges); out.triangles.swap(triangles);
        out.faces.swap(faces); out.materials.swap(materials);
        out.stats=stats;
        out.flags=brlive::LIVE_OBJECT_ACTIVE|brlive::LIVE_OBJECT_RENDER;
        out.fromPrefab=true;   // no Radiant brush behind it
        return true;
    }
    void PlayReconcileFills()
    {
        std::vector<PlayFillHull> hidden, stay;
        if (g_playLiveFillHoles && g_playBakedHullsLoaded)
        {
            for (const auto& pair:g_playLiveHides)
            {
                const PlayLiveHide& h=pair.second;
                if (!h.active) continue;
                hidden.push_back({h.guid,&h.planes,h.mins,h.maxs,{}});
            }
            for (const auto& pair:g_playBakedHulls)
            {
                auto hide=g_playLiveHides.find(pair.first);
                if (hide!=g_playLiveHides.end() && hide->second.active) continue;
                stay.push_back({pair.first,&pair.second.planes,pair.second.mins,pair.second.maxs,pair.second.tex});
            }
            for (const auto& pair:g_playBakedPrefabs)
            {
                const PlayBakedPrefab& bp=pair.second;
                PlayXform x; PlayAnglesToAxis(bp.compiled.angles,x.ax); for (int a=0;a<3;++a) x.o[a]=bp.compiled.origin[a];
                for (const PlayBakedPrefabHull& h:bp.hulls)
                {
                    const bool hid=bp.hidden || (bp.fileChanged && h.stale);
                    if (hid) { hidden.push_back({h.recordGuid,&h.hull.planes,h.hull.mins,h.hull.maxs,{}}); continue; }
                    PlayFillHull f={h.recordGuid,&h.hull.planes,h.hull.mins,h.hull.maxs,h.localTex};
                    for (PlayLiveFaceTex& t:f.tex) for (int k=0;k<3;++k) { float p[3]={t.pts[k].x,t.pts[k].y,t.pts[k].z}, q[3]; PlayXformPoint(x,p,q); t.pts[k]={q[0],q[1],q[2]}; }
                    stay.push_back(std::move(f));
                }
            }
        }
        std::map<std::string,std::pair<size_t,size_t>> desired;   // key -> (hidden index, stay index)
        for (size_t hi=0;hi<hidden.size() && desired.size()<kPlayMaxFills;++hi)
        {
            const PlayFillHull& h=hidden[hi];
            for (size_t si=0;si<stay.size() && desired.size()<kPlayMaxFills;++si)
            {
                const PlayFillHull& n=stay[si];
                bool touch=true;
                for (int a=0;a<3 && touch;++a) if (n.maxs[a]<h.mins[a]-kPlayFillExpand-0.5f || n.mins[a]>h.maxs[a]+kPlayFillExpand+0.5f) touch=false;
                if (!touch) continue;
                desired[PlayLiveGuidKey(PlayPrefabRecordGuid(h.guid,n.guid,777).c_str())]={hi,si};
            }
        }
        size_t created=0, removed=0, failed=0;
        for (auto it=g_playLiveFills.begin(); it!=g_playLiveFills.end();)
        {
            if (desired.count(it->first)) { ++it; continue; }
            auto found=g_playLiveRegistry.find(it->first);
            if (found!=g_playLiveRegistry.end()) { ++g_playLiveDeleteCount; g_playLiveRegistry.erase(found); ++removed; }
            it=g_playLiveFills.erase(it);
        }
        for (const auto& d:desired)
        {
            if (g_playLiveFills.count(d.first)) continue;
            if (g_playLiveRegistry.size()>=static_cast<size_t>(brlive::kMaxLiveObjects)) break;
            const PlayFillHull& h=hidden[d.second.first]; const PlayFillHull& n=stay[d.second.second];
            PlayLiveLocalObject obj;
            const std::string guid=PlayPrefabRecordGuid(h.guid,n.guid,777);
            const char* why="";
            if (!PlayBuildFillObject(n,h,guid,obj,&why))
            {
                if (failed<4) { char line[300]={}; snprintf(line,sizeof line,"[LIVE FILL] %s in %s: no fill (%s, %u texdef(s), %u planes)",h.guid.c_str(),n.guid.c_str(),why,(unsigned)n.tex.size(),(unsigned)n.planes->size()); PlayAppendLog(line); }
                ++failed; g_playLiveFills[d.first]="-"; continue;   // no volume or a sliver: N only touches H at an edge, or no texdefs
            }
            if (created<8)
            {
                float mn[3]={obj.vertices[0].x,obj.vertices[0].y,obj.vertices[0].z}, mx[3]={mn[0],mn[1],mn[2]};
                for (const PlayLiveVec3& v:obj.vertices) { mn[0]=(std::min)(mn[0],v.x); mx[0]=(std::max)(mx[0],v.x); mn[1]=(std::min)(mn[1],v.y); mx[1]=(std::max)(mx[1],v.y); mn[2]=(std::min)(mn[2],v.z); mx[2]=(std::max)(mx[2],v.z); }
                char line[400]={};
                snprintf(line,sizeof line,"[LIVE FILL] %s in %s: %.1f x %.1f x %.1f at %.0f %.0f %.0f, %s",h.guid.c_str(),n.guid.c_str(),mx[0]-mn[0],mx[1]-mn[1],mx[2]-mn[2],mn[0],mn[1],mn[2],obj.materials.empty() ? "?" : obj.materials[0].c_str());
                PlayAppendLog(line);
            }
            obj.liveObjectId=PlayAllocateLiveObjectId(); obj.generation=1; obj.operation=brlive::LIVE_OBJECT_CREATE;
            g_playLiveRegistry.emplace(d.first,std::move(obj)); ++g_playLiveCreateCount; ++created;
            g_playLiveFills[d.first]=h.guid+"|"+n.guid;
        }
        if (!created && !removed) { if (failed) { char line[200]={}; snprintf(line,sizeof line,"[LIVE FILL] no fill drawn: %u pair(s) rejected",(unsigned)failed); PlayAppendLog(line); } return; }
        ++g_playLiveRegistryGeneration;
        if (g_playLiveRegistry.empty()) PlayPublishLiveClear();
        PlayPublishLiveRegistry();
        size_t drawn=0; for (const auto& f:g_playLiveFills) if (f.second!="-") ++drawn;
        char line[320]={};
        snprintf(line,sizeof line,"[LIVE FILL] %u hole fill(s) drawn where %u hidden brush(es) met a baked neighbour (%u new, %u removed, %u pairs without volume)",(unsigned)drawn,(unsigned)hidden.size(),(unsigned)created,(unsigned)removed,(unsigned)failed);
        PlayAppendLog(line);
    }
    void PlayReconcileLiveHidesInner(uint64_t now, bool force);
    void PlayReconcileLiveHides(uint64_t now, bool force)
    {
        PlayReconcileLiveHidesInner(now,force);
        if (g_playFillsHideGeneration!=g_playLiveHidePublishes) { g_playFillsHideGeneration=g_playLiveHidePublishes; PlayReconcileFills(); }
    }
    void PlayReconcileLiveHidesInner(uint64_t now, bool force)
    {
        if (!force && now-g_playLiveLastHideReconcile<500) return;
        if (PlayAnyEditorMouseButtonDown() || g_playInputCaptured || (!force && !PlayRadiantForeground())) return;
        g_playLiveLastHideReconcile=now;
        {
            size_t prefabsHidden=0, prefabsShown=0, objectsSkipped=0;
            bool registryChanged=false;
            const bool hidesChanged=PlayReconcilePrefabHides(now,prefabsHidden,prefabsShown,registryChanged,objectsSkipped);
            if (registryChanged)
            {
                ++g_playLiveRegistryGeneration;
                if (g_playLiveRegistry.empty()) PlayPublishLiveClear();
                PlayPublishLiveRegistry();
            }
            size_t modelsHidden=0, modelsShown=0;
            const bool modelsChanged=PlayReconcileModelHides(now,modelsHidden,modelsShown);
            if (modelsChanged || hidesChanged) PlayPublishModelHides();   // prefab models travel with their prefab's state
            PlayPublishLiveModels(now);                                   // every pass: the bridge's model answers arrive asynchronously
            PlayPublishLiveSpots();
            if (!g_playLiveDeletePrefabs && !g_playLiveDeleteModels && !g_playLiveModels) PlayDiscoverNewPrefabs();   // new decals come from the same scan
            { size_t decalsHidden=0, decalsLive=0; PlayReconcileDecals(decalsHidden,decalsLive); }
            PlayReconcileFx(now);
            PlayReconcileSky(now);
            if (modelsChanged)
            {
                size_t hiddenModels=0;
                for (const auto& pair:g_playBakedModels) if (pair.second.hidden) ++hiddenModels;
                char line[320]={};
                snprintf(line,sizeof line,"[LIVE DELETE] static models: %u hidden in the game (deleted or moved; a moved one is spawned live as a script_model), %u shown again, %u of %u hidden",(unsigned)modelsHidden,(unsigned)modelsShown,(unsigned)hiddenModels,(unsigned)g_playBakedModels.size());
                PlayAppendLog(line);
            }
            if (hidesChanged || registryChanged)
            {
                if (hidesChanged) PlayPublishLiveHides();
                size_t records=0, objects=0;
                for (const auto& pair:g_playBakedPrefabs) { if (pair.second.hidden) records+=pair.second.hulls.size(); if (pair.second.liveObjects) objects+=pair.second.liveKeys.size(); }
                char line[400]={};
                snprintf(line,sizeof line,"[LIVE DELETE] prefab instances: %u hidden in the game, %u shown again, %u interior brush record(s) active; %u interior brush(es) drawn live, moved or edited (%u skipped: registry full or hull failed)",
                    (unsigned)prefabsHidden,(unsigned)prefabsShown,(unsigned)records,(unsigned)objects,(unsigned)objectsSkipped);
                PlayAppendLog(line);
            }
        }
        PlayReconcilePatchHides(now);
        std::set<std::string> keys;
        for (const auto& pair:g_playLiveHides) keys.insert(pair.first);
        if (g_playBakedHullsLoaded)
            for (const auto& pair:g_playLiveRegistry)
                if (g_playBakedHulls.count(pair.first)) keys.insert(pair.first);
        if (keys.empty()) return;

        size_t selectionCount=0, instanceCount=0;
        if (!PlayTryFillSettledSelection(nullptr,0,selectionCount) || selectionCount>4096) return;
        std::vector<ModToolsSelectedBrush> selections(selectionCount);
        size_t selectedRead=0;
        if (selectionCount && !PlayTryFillSettledSelection(selections.data(),selections.size(),selectedRead)) return;
        selections.resize((std::min)(selectedRead,selections.size()));
        std::set<std::string> selectedKeys;
        for (const auto& selected:selections) selectedKeys.insert(PlayLiveGuidKey(selected.brushGuid));
        std::map<std::string,ModToolsBrushInst> instByKey;
        if (!selectedKeys.empty() && PlayTryFillSettledBrushInsts(nullptr,0,instanceCount) && instanceCount && instanceCount<=4096)
        {
            std::vector<ModToolsBrushInst> instances(instanceCount,nullptr);
            size_t instancesRead=0;
            if (PlayTryFillSettledBrushInsts(instances.data(),instances.size(),instancesRead))
                for (size_t i=0;i<(std::min)(instancesRead,instances.size());++i)
                {
                    char guid[40]={};
                    if (PlayTrySettledBrushGuid(instances[i],guid)) instByKey[PlayLiveGuidKey(guid)]=instances[i];
                }
        }

        std::vector<std::string> keyList(keys.begin(),keys.end());
        std::vector<const char*> guids; guids.reserve(keyList.size());
        for (const std::string& key:keyList) guids.push_back(key.c_str());
        std::vector<ModToolsBrush> brushes(keyList.size(),nullptr);
        if (!PlayTryFindLiveBrushes(guids.data(),guids.size(),brushes.data())) return;

        size_t hiddenNow=0, shownAgain=0, created=0;
        bool changed=false;
        for (size_t i=0;i<keyList.size();++i)
        {
            const std::string& key=keyList[i];
            const bool selected=selectedKeys.count(key)!=0;
            const bool present=brushes[i]!=nullptr || selected;
            auto record=g_playLiveHides.find(key);
            auto baked=g_playBakedHulls.find(key);
            bool want=false;
            if (!present) want=true;   // deleted (a record made by the detour, or a Live Edit object whose brush vanished)
            else
            {
                const std::vector<PlayLivePlane>* reference=nullptr;
                if (baked!=g_playBakedHulls.end()) reference=&baked->second.planes;
                else if (record!=g_playLiveHides.end()) reference=&record->second.planes;
                std::vector<PlayLivePlane> current;
                if (reference && PlayCurrentHull(brushes[i],selected ? instByKey[key] : nullptr,current)) want=!PlaySameHull(*reference,current);
                else if (record!=g_playLiveHides.end()) want=record->second.active;   // unreadable this pass: keep the record as it is
                else want=false;
            }
            if (record==g_playLiveHides.end())
            {
                if (!want || baked==g_playBakedHulls.end() || g_playLiveHides.size()>=static_cast<size_t>(brlive::kMaxHideRecords)) continue;
                PlayLiveHide hide;
                if (!PlayHideFromBaked(key,hide)) continue;
                g_playLiveHides.emplace(key,std::move(hide));
                ++created; ++g_playLiveHideAdded; changed=true;
                continue;
            }
            if (record->second.active!=want)
            {
                record->second.active=want;
                if (want) { ++hiddenNow; record->second.since=now; } else ++shownAgain;
                changed=true;
            }
        }
        if (!changed) return;
        PlayPublishLiveHides();
        char line[320]={};
        snprintf(line,sizeof line,"[LIVE DELETE] %u edited brush(es) hidden in the game, %u hidden again, %u shown again (back to their baked shape)",
            (unsigned)created,(unsigned)hiddenNow,(unsigned)shownAgain);
        PlayAppendLog(line);
    }

    void PlayReconcileLiveRegistryDeletes(uint64_t now)
    {
        if (now-g_playLiveLastRegistryReconcile<500 || g_playLiveRegistry.empty() ||
            PlayAnyEditorMouseButtonDown() || g_playLiveReleaseSnapshotPending || g_playLiveDeleteFollowupPending ||
            g_playInputCaptured || !PlayRadiantForeground()) return;
        g_playLiveLastRegistryReconcile=now;

        size_t selectionCount=0;
        if (!PlayTryFillSettledSelection(nullptr,0,selectionCount) || selectionCount>4096) return;
        std::vector<ModToolsSelectedBrush> selections(selectionCount);
        size_t selectedRead=0;
        if (selectionCount && !PlayTryFillSettledSelection(selections.data(),selections.size(),selectedRead)) return;
        selections.resize((std::min)(selectedRead,selections.size()));
        std::set<std::string> selectedKeys;
        for (const auto& selected:selections) selectedKeys.insert(PlayLiveGuidKey(selected.brushGuid));

        std::vector<std::string> keys, lookups;   // registry keys and the brush GUID each one names (tiles carry "#k")
        for (const auto& pair:g_playLiveRegistry)
            if (!selectedKeys.count(PlayRegistryKeyGuid(pair.first)) && !pair.second.fromPrefab) { keys.push_back(pair.first); lookups.push_back(PlayRegistryKeyGuid(pair.first)); }
        if (keys.empty()) return;
        std::vector<const char*> keyPointers;
        keyPointers.reserve(keys.size());
        for (const std::string& key:lookups) keyPointers.push_back(key.c_str());
        std::vector<ModToolsBrush> brushes(keys.size(),nullptr);
        if (!PlayTryFindLiveBrushes(keyPointers.data(),keyPointers.size(),brushes.data())) return;

        bool changed=false;
        std::vector<std::string> eraseKeys;
        for (size_t i=0;i<keys.size();++i)
        {
            auto found=g_playLiveRegistry.find(keys[i]);
            if (found==g_playLiveRegistry.end()) continue;
            if (brushes[i])
            {
                if (found->second.stalePasses) {found->second.stalePasses=0;changed=true;}
                continue;
            }
            ++found->second.stalePasses;
            changed=true;
            if (found->second.stalePasses<2) continue;
            g_playLiveLastOperation=brlive::LIVE_OBJECT_DELETE;
            g_playLiveLastOperationId=found->second.liveObjectId;
            g_playLiveLastOperationGeneration=static_cast<LONG>(found->second.generation+1);
            ++g_playLiveDeleteCount;
            ++g_playLiveRegistryGeneration;
            eraseKeys.push_back(keys[i]);
        }
        for (const std::string& key:eraseKeys) g_playLiveRegistry.erase(key);
        if (!changed) return;
        if (g_playLiveRegistry.empty()) PlayPublishLiveClear();
        PlayPublishLiveRegistry();
        if (!eraseKeys.empty())
        {
            char line[384]={};
            snprintf(line,sizeof line,
                "[LIVE REGISTRY] DELETE count=%u liveObjectCount=%ld deleteCount=%ld staleObjectCount=%ld registryGeneration=%ld",
                (unsigned)eraseKeys.size(),(long)g_playLiveShared->liveObjectCount,
                (long)g_playLiveShared->liveDeleteCount,(long)g_playLiveShared->staleObjectCount,
                (long)g_playLiveShared->liveRegistryGeneration);
            PlayAppendLog(line);
        }
    }

    const char* PlayEngineStateName(LONG state)
    {
        switch (state)
        {
        case brlive::ENGINE_OFFLINE: return "OFFLINE";
        case brlive::ENGINE_BOOTING: return "BOOTING";
        case brlive::ENGINE_DISCOVERY: return "DISCOVERY";
        case brlive::ENGINE_SYMBOLS_UNRESOLVED: return "SYMBOLS_UNRESOLVED";
        case brlive::ENGINE_FRONTEND: return "FRONTEND";
        case brlive::ENGINE_LOADING: return "LOADING";
        case brlive::ENGINE_MAP_ACTIVE: return "MAP_ACTIVE";
        case brlive::ENGINE_PLAYING: return "PLAYING";
        case brlive::ENGINE_STOPPED: return "STOPPED";
        default: return "UNKNOWN";
        }
    }

    const char* PlaySessionStateName(LONG state)
    {
        switch (state)
        {
        case brlive::PLAY_SESSION_UNKNOWN: return "UNKNOWN";
        case brlive::PLAY_SESSION_MAP_READY: return "MAP_READY";
        case brlive::PLAY_SESSION_STARTING: return "STARTING";
        case brlive::PLAY_SESSION_PLAYING: return "PLAYING";
        case brlive::PLAY_SESSION_STOPPING: return "STOPPING";
        case brlive::PLAY_SESSION_STOPPED: return "STOPPED";
        case brlive::PLAY_SESSION_ERROR: return "ERROR";
        default: return "INVALID";
        }
    }

    void PlayEngineSetRequestedMap()
    {
        if (!g_playLiveShared) return;
        std::string map = g_playGameMap;
        if (map.empty()) map = g_playCurrentMap;
        if (!map.empty()) strncpy_s(g_playLiveShared->requestedMap, map.c_str(), _TRUNCATE);
    }

    const char* PlayEngineCommandName(LONG command)
    {
        switch (command)
        {
        case brlive::ENGINE_CMD_PING: return "PING";
        case brlive::ENGINE_CMD_QUERY_STATE: return "QUERY_STATE";
        case brlive::ENGINE_CMD_LOAD_MAP: return "LOAD_MAP";
        case brlive::ENGINE_CMD_BEGIN_PLAY: return "BEGIN_PLAY";
        case brlive::ENGINE_CMD_END_PLAY: return "END_PLAY";
        case brlive::ENGINE_CMD_RESTART_PLAYER: return "RESTART_PLAYER";
        case brlive::ENGINE_CMD_RELOAD_MAP: return "RELOAD_MAP";
        case brlive::ENGINE_CMD_CBUF_PROBE: return "CBUF_PROBE";
        case brlive::ENGINE_CMD_FAST_RESTART: return "FAST_RESTART";
        default: return "NONE";
        }
    }

    const char* PlayEngineResultName(LONG result)
    {
        switch (result)
        {
        case brlive::ENGINE_RESULT_OK: return "OK";
        case brlive::ENGINE_RESULT_NOT_READY: return "NOT_READY";
        case brlive::ENGINE_RESULT_UNRESOLVED: return "UNRESOLVED";
        case brlive::ENGINE_RESULT_BAD_COMMAND: return "BAD_COMMAND";
        case brlive::ENGINE_RESULT_UNSUPPORTED_BUILD: return "UNSUPPORTED_BUILD";
        case brlive::ENGINE_RESULT_BAD_ARGUMENT: return "BAD_ARGUMENT";
        case brlive::ENGINE_RESULT_WRONG_THREAD: return "WRONG_THREAD";
        default: return "NONE";
        }
    }

    bool PlayEngineSendCommand(LONG command, const char* text)
    {
        if (!g_playLiveShared || !g_playGamePid || !g_playGameOwnedByRadiant) return false;
        if (!g_playLiveShared->cbufVerified || !g_playLiveShared->mainFrameHookInstalled ||
            !(g_playLiveShared->engineReadyFlags & brlive::ENGINE_READY_FRAME_PUMP) ||
            !(g_playLiveShared->engineReadyFlags & brlive::ENGINE_READY_COMMAND_EXEC)) return false;
        if (g_playLiveShared->engineCommandSeq != g_playLiveShared->engineCommandAck) return false;
        g_playLiveShared->engineCommand = command;
        g_playLiveShared->engineCommandResult = brlive::ENGINE_RESULT_NONE;
        if (text && *text) strncpy_s(g_playLiveShared->commandText, text, _TRUNCATE);
        else g_playLiveShared->commandText[0] = 0;
        MemoryBarrier();
        const LONG seq = InterlockedIncrement(&g_playLiveShared->engineCommandSeq);
        g_playEngineUserCommandSeq = seq;
        g_playEngineUserCommand = command;
        return true;
    }

    void PlayEngineConsumeAck()
    {
        if (!g_playLiveShared) return;
        const LONG ack = g_playLiveShared->engineCommandAck;
        if (ack == g_playEngineLastCommandAck) return;
        g_playEngineLastCommandAck = ack;
        if (g_playEngineUserCommandSeq > 0 && ack == g_playEngineUserCommandSeq)
        {
            const LONG result = g_playLiveShared->engineCommandResult;
            char line[768] = {};
            snprintf(line, sizeof line,
                "[ENGINE CMD ACK] seq=%ld command=%s result=%s cbufRva=0x%08lX verified=%ld execCount=%ld failures=%ld callTid=%ld frameTid=%ld pumps=%ld client=%ld queued='%s'",
                (long)ack, PlayEngineCommandName(g_playEngineUserCommand), PlayEngineResultName(result),
                (unsigned long)(uint32_t)g_playLiveShared->cbufAddTextRva, (long)g_playLiveShared->cbufVerified,
                (long)g_playLiveShared->engineCommandExecCount, (long)g_playLiveShared->engineCommandExecFailures,
                (long)g_playLiveShared->engineCommandThreadId, (long)g_playLiveShared->mainFrameThreadId,
                (long)g_playLiveShared->mainFramePumpCount, (long)g_playLiveShared->engineCommandClientIndex,
                g_playLiveShared->lastQueuedCommand);
            PlayAppendLog(line);
            g_playEngineUserCommandSeq = 0;
            g_playEngineUserCommand = brlive::ENGINE_CMD_NONE;
        }
    }

    void PlayEngineSendQuery(uint64_t now)
    {
        if (!g_playLiveShared || now - g_playEngineLastQueryMs < 1000) return;
        if (!g_playLiveShared->mainFrameHookInstalled ||
            !(g_playLiveShared->engineReadyFlags & brlive::ENGINE_READY_FRAME_PUMP)) return;
        if (g_playLiveShared->engineCommandSeq != g_playLiveShared->engineCommandAck) return;
        if (g_playEngineUserCommandSeq > 0) return;
        g_playEngineLastQueryMs = now;
        g_playLiveShared->engineCommand = brlive::ENGINE_CMD_QUERY_STATE;
        g_playLiveShared->engineCommandResult = brlive::ENGINE_RESULT_NONE;
        g_playLiveShared->commandText[0] = 0;
        MemoryBarrier();
        InterlockedIncrement(&g_playLiveShared->engineCommandSeq);
    }

    void PlayTickEngineRuntime()
    {
        DWORD pid=0;
        { std::lock_guard<std::mutex> lock(g_playMutex); pid=g_playGamePid; }
        if (!pid) return;
        if (!PlayEnsureLiveShared(pid)) return;

        std::string injectError;
        // live_preview=0: no bridge in the game at all, unless the live zone or Play From Here needs its zone module.
        if (!g_playLiveBridgeInjected && (g_playLivePreview || g_playLiveZone || g_playFromCamera || g_playJoinMode))   // Join Host: the bridge joins the host
        {
            if (PlayInjectLiveBridge(pid,injectError))
            {
                g_playLiveBridgeInjected=true;
                PlayAppendLog("[ENGINE] BO3LiveBridge.dll injected into BO3 (pid " + std::to_string(pid) + ").");
            }
            else if(!g_playLiveBackgroundWarned)
            {
                g_playLiveBackgroundWarned=true;
                PlayAppendLog("[ENGINE WARN] Editor-runtime injection failed: "+injectError);
                PlayNote("WARNING: Play Test could not connect to the game (" + injectError + "). The game runs, but Live Edit, docking and live loads are off.");
            }
        }
        // Never start the zone module during the game's startup stub (an injection 4 s after launch failed fast 3 s later): wait for the game window and 20 s after our own launch,
        // an adopted game (no launch time) only needs its window. A docked game is a child window, which EnumWindows never lists: take the docked handle first.
        // The module lives in BO3LiveBridge.dll: Radiant arms it through the shared state and waits for its answer, so a build without a zone table entry is known.
        const HWND liveZoneGameWindow = (g_playEmbeddedHwnd && IsWindow(g_playEmbeddedHwnd)) ? g_playEmbeddedHwnd : PlayFindGameWindow(pid);
        const bool liveZoneInjectWindowOpen = liveZoneGameWindow != nullptr &&
            (g_playLiveZoneLaunchMs == 0 || GetTickCount64() - g_playLiveZoneLaunchMs >= 20000);
        // Play From Here needs the mailbox too: the module leaves the table alone without patch fastfiles (the slot is only written while both exist), so it may run with live mode off.
        if ((g_playLiveZone || g_playFromCamera || g_playDisableAi || g_playFly || g_playGod || g_playJoinMode || g_playLanWanted) && g_playLiveBridgeInjected && !g_playLiveZoneArmed && liveZoneInjectWindowOpen)
        {
            // The module reads the usermap name from its ini (it writes that name into the engine's patchable-map table).
            const std::wstring ini = PlayJoin(PlayThisModuleDir(), L"BO3LiveZone.ini");
            WritePrivateProfileStringW(L"livezone", L"map", PlayUtf8ToWide(g_playCurrentMap).c_str(), ini.c_str());
            WritePrivateProfileStringW(L"livezone", L"command", L"", ini.c_str());
            MemoryBarrier();
            InterlockedExchange(&g_playLiveShared->zoneArm, 1);
            g_playLiveZoneArmed = true;
        }
        if (g_playLiveZoneArmed && !g_playLiveZoneInjected && !g_playLiveZoneWarned)
        {
            const LONG zs = g_playLiveShared->zoneState;
            const std::string build(g_playLiveShared->zoneBuildName, strnlen(g_playLiveShared->zoneBuildName, sizeof g_playLiveShared->zoneBuildName));
            if (zs == brlive::ZONE_READY)
            {
                g_playLiveZoneInjected = true;
                // Spawn at camera: the same sets again through the mailbox, same generation, so a receiver that already moved the player does nothing and one that missed the launch-line values moves them now.
                if (!g_playCamDvar.empty() && !g_playJoinMode) PlayLiveZoneSendCommands(PlayHereConsoleSets());
                if (g_playLiveZone)
                {
                    PlayAppendLog("[LIVE ZONE] zone module ready (" + build + ") for " + g_playCurrentMap + ": its _patch fastfiles load after the base zone on every map load from now on.");
                    PlayLiveZonePublishPreview();
                }
                else
                    PlayAppendLog("[PLAY HERE] zone module ready (" + build + "): console commands (spawn at the camera, warm reloads) go through its mailbox from now on.");
            }
            else if (zs == brlive::ZONE_NO_BUILD || zs == brlive::ZONE_NO_CBUF)
            {
                g_playLiveZoneWarned = true;
                const std::string text(g_playLiveShared->zoneStatusText, strnlen(g_playLiveShared->zoneStatusText, sizeof g_playLiveShared->zoneStatusText));
                PlayAppendLog("[LIVE ZONE WARN] " + text + ". Live zone, Play From Here and mailbox reloads are off for this game (see BO3LiveZone.log).");
                PlayNote("WARNING: this BlackOps3.exe is not one Play Test knows, so it cannot send commands into it (reloads, spawn at camera, patch zone).");
            }
        }
        // A patch zone placed before a cold launch: the DLL is injected before the level zones expand, so the launch itself normally takes the patch. The console log decides:
        // "Setting <map>_patch to XZONE_LOADED" means done, "ModLoad done: <map>" without it means the level is up without the patch, and only then is a devmap safe
        // (from the menu it resolves the patch under <root>\zone and fails with "Could not find zone").
        if (g_playLiveZone && g_playLiveZoneInjected && g_playLiveZonePending && g_playLiveZoneLaunchMs &&
            GetTickCount64() - g_playLiveZoneLaunchMs > 30000 && GetTickCount64() - g_playLiveZoneLogCheckMs > 3000 && !PlayBusy())
        {
            g_playLiveZoneLogCheckMs = GetTickCount64();
            const std::string tail = PlayReadFileTail(g_playLiveZoneConsoleLog, 8u << 20);
            const bool patchLoaded = tail.find("Setting " + g_playCurrentMap + "_patch to XZONE_LOADED") != std::string::npos;
            const bool levelUp = tail.find("ModLoad done: " + g_playCurrentMap) != std::string::npos;
            if (patchLoaded)
            {
                g_playLiveZonePending = false;
                PlayAppendLog("[LIVE ZONE] the launch loaded the patch zone itself (console log); no reload needed.");
            }
            else if (levelUp)
            {
                g_playLiveZonePending = false;
                if (PlayLiveZoneSendCommand("devmap " + g_playCurrentMap))
                    PlayAppendLog("[LIVE ZONE] the level is up without the patch zone (console log): reloading it so the patch built before the launch is applied.");
            }
            else if (GetTickCount64() - g_playLiveZoneLaunchMs > 300000)
            {
                g_playLiveZonePending = false;
                PlayAppendLog("[LIVE ZONE WARN] the console log showed neither the patch zone nor the level within 5 minutes; no reload sent. Press Play again once in the level.");
            }
        }
        if (!g_playLiveShared) return;

        PlayEngineSetRequestedMap();
        g_playLiveShared->editorOwnedRuntime = g_playGameOwnedByRadiant ? 1 : 0;
        const uint64_t now = GetTickCount64();
        PlayEngineConsumeAck();
        PlayEngineSendQuery(now);

        const LONG hb = g_playLiveShared->engineHeartbeat;
        const LONG state = g_playLiveShared->engineState;
        const LONG flags = g_playLiveShared->engineReadyFlags;
        if (hb != g_playEngineLastHeartbeat) g_playEngineLastHeartbeat = hb;
        if (state != g_playEngineLastState)
        {
            g_playEngineLastState = state;
            char flagHex[16]={};
            snprintf(flagHex,sizeof flagHex,"%08lX",(unsigned long)(uint32_t)flags);
            // the status text can carry a per-bit guard table: kept only when a bit failed
            std::string status(g_playLiveShared->engineStatusText);
            const size_t nl = status.find('\n');
            if (nl != std::string::npos && status.find("=FAIL") == std::string::npos) status = status.substr(0, nl) + " (guard table: every bit PASS)";
            PlayAppendLog(std::string("[ENGINE] state=") + PlayEngineStateName(state) + " flags=0x" + flagHex + " status=" + status);
        }

        if (!g_playEngineLoggedReady && hb > 0 && (flags & brlive::ENGINE_READY_COMPANION))
        {
            g_playEngineLoggedReady = true;
            PlayAppendLog("[ENGINE] Bridge heartbeat alive; engine calls wait for the main-frame hook.");
        }

        if (!g_playEngineLoggedTriangleGuardDiagnostics &&
            g_playLiveShared->nativeTriangleGuardDiagnosticSeq > 0 &&
            g_playLiveShared->nativeTriangleGuardDiagnostics[0])
        {
            MemoryBarrier();
            g_playEngineLoggedTriangleGuardDiagnostics = true;
            PlayAppendLog(g_playLiveShared->nativeTriangleGuardDiagnostics);
        }

        if (!g_playEngineLoggedCbuf && g_playLiveShared->cbufVerified && (flags & brlive::ENGINE_READY_COMMAND_EXEC))
        {
            g_playEngineLoggedCbuf = true;
            char cbuf[512] = {};
            snprintf(cbuf, sizeof cbuf,
                "[ENGINE] VERIFIED main-frame IPC: frameRva=0x%08lX guard=0x%02lX hook=%ld pumps=%ld frameTid=%ld; Cbuf_AddText RVA=0x%08lX guard=0x%02lX. Engine calls are accepted only from that frame thread.",
                (unsigned long)(uint32_t)g_playLiveShared->mainFrameRva,
                (unsigned long)(uint32_t)g_playLiveShared->mainFrameGuardMask,
                (long)g_playLiveShared->mainFrameHookInstalled, (long)g_playLiveShared->mainFramePumpCount,
                (long)g_playLiveShared->mainFrameThreadId,
                (unsigned long)(uint32_t)g_playLiveShared->cbufAddTextRva,
                (unsigned long)(uint32_t)g_playLiveShared->cbufGuardMask);
            PlayAppendLog(cbuf);
        }

        PlayNavRefreshTick();
        {
            // Textured Live Edit: the bridge's guard result once, then every change in what resolved.
            const std::string tex(g_playLiveShared->texturedStatus, strnlen(g_playLiveShared->texturedStatus, sizeof g_playLiveShared->texturedStatus));
            if (!tex.empty() && tex != g_playEngineLastTexturedStatus)
            {
                g_playEngineLastTexturedStatus = tex;
                PlayAppendLog("[TEXTURED] " + tex);
            }
            const LONG resolved = g_playLiveShared->texturedMaterialsResolved, missing = g_playLiveShared->texturedMaterialsMissing;
            if (g_playLiveShared->texturedVerified && (resolved != g_playEngineLastTexturedResolved || missing != g_playEngineLastTexturedMissing))
            {
                g_playEngineLastTexturedResolved = resolved; g_playEngineLastTexturedMissing = missing;
                std::string names;
                for (LONG i = 0; i < g_playLiveShared->materialCount && i < brlive::kMaxMaterials; ++i)
                { if (!names.empty()) names += ", "; names += std::string(g_playLiveShared->materialNames[i], strnlen(g_playLiveShared->materialNames[i], brlive::kMaterialNameLen)); }
                PlayAppendLog("[TEXTURED] materials resolved=" + std::to_string(resolved) + " missing=" + std::to_string(missing) +
                              (names.empty() ? std::string() : ": " + names) + (missing ? " (a missing one keeps the colored fill)" : ""));
            }
            // Materials the level lacks: remembered for the patch zone (next Play) and linked as a small zone that the bridge loads into the running level once the clicks settle (no map reload).
            if (g_playLiveShared->texturedVerified && g_playAutoLinkMaterials)
            {
                const uint64_t now = GetTickCount64();
                int added = 0; std::string addedNames;
                // a name must be reported missing for a full second: the state slot of a freshly published name still carries the previous name's answer until the bridge's next pass
                static std::map<std::string, uint64_t> missingSince;
                std::set<std::string> seenMissing;
                for (LONG i = 0; i < g_playLiveShared->materialCount && i < brlive::kMaxMaterials; ++i)
                {
                    if (g_playLiveShared->materialState[i] != 2) continue;
                    const std::string name(g_playLiveShared->materialNames[i], strnlen(g_playLiveShared->materialNames[i], brlive::kMaterialNameLen));
                    if (name.empty() || name[0] == '$' || name.find_first_of(" \t,\"") != std::string::npos) continue;   // $default etc. are Radiant placeholders
                    seenMissing.insert(name);
                    auto ms = missingSince.find(name);
                    if (ms == missingSince.end()) { missingSince[name] = now; continue; }
                    if (now - ms->second < 1000) continue;
                    const std::string key = g_playCurrentMap + "|" + name;
                    if (g_playLiveMaterialsQueued.count(key)) continue;
                    g_playLiveMaterialsQueued.insert(key);
                    PlayMapInfo m; std::string error;
                    if (!PlayResolveCurrentMap(m, error)) { PlayAppendLog("[TEXTURED] " + name + " is not in the level and the map could not be resolved: " + error); continue; }
                    if (g_playLiveZone) PlayAddLiveMaterial(m, name);   // persists it through the patch zone on the next Play
                    else if (!g_playLiveMaterialsNoZoneWarned)
                    {
                        g_playLiveMaterialsNoZoneWarned = true;
                        PlayAppendLog("[TEXTURED] live_zone=0: materials the level lacks cannot be added through the patch zone (set live_zone=1).");
                        PlayNote("Some materials are not in the level yet and Live Edit cannot add them (live_zone=0 in PlayTest.ini). A Play builds them in.");
                    }
                    if (g_playLiveMaterialLoad) g_playLiveMatPending.push_back(name);
                    ++added; if (!addedNames.empty()) addedNames += ", "; addedNames += name;
                }
                for (auto it = missingSince.begin(); it != missingSince.end(); ) { if (!seenMissing.count(it->first)) it = missingSince.erase(it); else ++it; }
                PlayQueueDecalMaterials(added, addedNames);
                if (added)
                {
                    if (g_playLiveMaterialLoad)
                    {
                        g_playLiveMaterialsDueMs = now + 3000;
                        PlayAppendLog("[TEXTURED] " + std::to_string(added) + " material(s) the level lacks queued (" + addedNames + "): linking a live zone in a few seconds.");
                    }
                    else
                    {
                        PlayAppendLog("[TEXTURED] " + std::to_string(added) + " material(s) the level lacks (" + addedNames + "): carrier queued for the patch zone, in the game from the next UPDATE / RELOAD on (preload_materials=<prefix,...> in PlayTest.ini makes a whole material set available from the start).");
                        PlayNote(std::to_string(added) + " material(s) are not in the level yet (" + addedNames + "): they show in the game from the next Play.");
                    }
                }
                // start the link once the clicks settle, one at a time, never beside a build
                if (!g_playLiveMatPending.empty() && g_playLiveMaterialsDueMs && now >= g_playLiveMaterialsDueMs && !PlayBusy() && !g_playLoadWatch && !g_playLiveMatRunning &&
                    g_playLiveShared->engineWorldActive && g_playLiveShared->mainFrameHookInstalled)
                {
                    PlayMapInfo m; std::string error;
                    if (!PlayResolveCurrentMap(m, error)) { g_playLiveMaterialsDueMs = now + 5000; }
                    else
                    {
                        std::vector<std::string> names; names.swap(g_playLiveMatPending);
                        if (g_playLiveKeptPid != g_playGamePid) { g_playLiveKeptPid = g_playGamePid; g_playLiveKeptZones = 0; g_playLiveSessionNames.clear(); }
                        for (auto& n : names) if (std::find(g_playLiveSessionNames.begin(), g_playLiveSessionNames.end(), n) == g_playLiveSessionNames.end()) g_playLiveSessionNames.push_back(n);
                        // earlier live zones stay loaded, so a zone carries only the new names, a FULL zone replaces them all
                        const bool full = g_playLiveKeptZones >= kPlayLiveKeepZones;
                        if (full) { names = g_playLiveSessionNames; g_playLiveKeptZones = 0; }
                        else ++g_playLiveKeptZones;
                        g_playLiveMaterialsDueMs = 0;
                        const std::wstring zoneName = m.mapName + (full ? L"_lmf" : L"_lm") + std::to_wstring((unsigned long long)((GetTickCount64() / 1000) % 1000000));
                        PlayAppendLog("[TEXTURED] linking " + std::to_string(names.size()) + " material(s) as live zone " + PlayWideToUtf8(zoneName) + " ...");
                        {
                            // the batch mixes materials with models, effects and sky models ("xmodel,<name>", "fx,<name>")
                            int mats = 0, models = 0, fx = 0;
                            for (const std::string& n : names) { if (n.rfind("xmodel,", 0) == 0) ++models; else if (n.rfind("fx,", 0) == 0) ++fx; else ++mats; }
                            std::string what;
                            auto add = [&](int k, const char* one, const char* many) { if (!k) return; if (!what.empty()) what += ", "; what += std::to_string(k) + " " + (k == 1 ? one : many); };
                            add(mats, "material", "materials"); add(models, "model", "models"); add(fx, "effect", "effects");
                            PlayNote("Loading " + what + " the level does not have into the running game. Linking them first (the game keeps running)...");
                        }
                        PlayWaitBegin("assets", "linking assets the level does not have");
                        if (g_playLiveMatThread.joinable()) g_playLiveMatThread.join();
                        g_playLiveMatCancel = false; g_playLiveMatThreadDone = false; g_playLiveMatRunning = true;
                        g_playLiveMatThread = std::thread([m, names, zoneName]()
                        {
                            PlayLiveMatResult r; r.zoneName = PlayWideToUtf8(zoneName); r.count = (int)names.size();
                            const uint64_t t0 = GetTickCount64();
                            g_playRunBelowNormal = true;   // export2bin, gdtdb and the linker yield to the running game
                            g_playRunJobCancel = &g_playLiveMatCancel;
                            r.ok = PlayLiveMaterialsLinkZone(m, names, zoneName, r.error);
                            r.seconds = (GetTickCount64() - t0) / 1000;
                            { std::lock_guard<std::mutex> lock(g_playLiveMatMutex); g_playLiveMatResult = r; }
                            g_playLiveMatThreadDone = true;
                        });
                    }
                }
                if (g_playLiveMatRunning && g_playLiveMatThreadDone)
                {
                    if (g_playLiveMatThread.joinable()) g_playLiveMatThread.join();
                    g_playLiveMatRunning = false;
                    PlayLiveMatResult r; { std::lock_guard<std::mutex> lock(g_playLiveMatMutex); r = g_playLiveMatResult; }
                    PlayWaitEnd("assets");
                    if (r.ok)
                    {
                        strncpy_s(g_playLiveShared->zoneLoadName, r.zoneName.c_str(), _TRUNCATE);
                        g_playLiveMatZoneSeq = InterlockedIncrement(&g_playLiveShared->zoneLoadSeq);
                        ++g_playMidLevelZoneLoads;
                        PlayAppendLog("[TEXTURED] live zone " + r.zoneName + " linked in " + std::to_string(r.seconds) + " s (" + std::to_string(r.count) + " material(s)); the bridge is loading it into the running level.");
                        PlayNote("Linked in " + std::to_string(r.seconds) + " s. BO3 is loading them (images can take a moment to stream in)...");
                        PlayWaitBegin("assets", "waiting for BO3 to load the new assets", 3 * 60000);
                    }
                    else if (g_playLiveMatCancel) { g_playLiveMatCancel = false; PlayAppendLog("[TEXTURED] live zone " + r.zoneName + " cancelled: Play's patch zone takes the names."); }
                    else
                    {
                        PlayAppendLog("[TEXTURED] live zone " + r.zoneName + " failed: " + r.error + ". The names stay in the patch zone list for the next Play.");
                        PlayNote("Could not load the new assets into the running game (" + r.error + "). The next Play builds them in.");
                    }
                }
                const LONG done = g_playLiveShared->zoneLoadDoneSeq;
                if (done != g_playLiveMatLastDoneSeq)
                {
                    g_playLiveMatLastDoneSeq = done;
                    const std::string text(g_playLiveShared->zoneLoadText, strnlen(g_playLiveShared->zoneLoadText, sizeof g_playLiveShared->zoneLoadText));
                    PlayAppendLog(std::string("[TEXTURED] live zone load ") + (g_playLiveShared->zoneLoadResult ? "OK" : "FAILED") + ": " + text);
                    if (g_playLiveMatZoneSeq && done == g_playLiveMatZoneSeq)   // ours, not a nav refresh's zone
                    {
                        g_playLiveMatZoneSeq = 0;
                        const uint64_t ms = PlayWaitEnd("assets");
                        if (g_playLiveShared->zoneLoadResult) PlayNote("The new assets are in the game" + (ms ? " (" + PlayDuration(ms) + ")" : std::string()) + ".");
                        else PlayNote("BO3 could not load the new assets (" + text + "). The next Play builds them in.");
                    }
                }
            }
        }

        if (!g_playEngineLoggedNative && g_playLiveShared->nativeLineVerified &&
            g_playLiveShared->mainFramePumpCount > 0 && g_playLiveShared->mainFrameThreadId != 0 &&
            (flags & brlive::ENGINE_READY_NATIVE_WORLD_DRAW))
        {
            g_playEngineLoggedNative = true;
            char native[896] = {};
            snprintf(native, sizeof native,
                "[ENGINE] VERIFIED native world draw: lineRva=0x%08lX guard=0x%02lX/0x0F mode=%ld frameTid=%ld; triangleRva=0x%08lX guard=0x%02lX/0x3F verified=%ld hook=%ld. Protocol-10 exact face triangles use BO3's renderer context and depth-tested solid material; green world-line outlines remain enabled.",
                (unsigned long)(uint32_t)g_playLiveShared->nativeLineRva,
                (unsigned long)(uint32_t)g_playLiveShared->nativeLineGuardMask,
                (long)g_playLiveShared->nativeDrawMode,
                (long)g_playLiveShared->mainFrameThreadId,
                (unsigned long)(uint32_t)g_playLiveShared->nativeTriangleRva,
                (unsigned long)(uint32_t)g_playLiveShared->nativeTriangleGuardMask,
                (long)g_playLiveShared->nativeTriangleVerified,
                (long)g_playLiveShared->nativeTriangleHookInstalled);
            PlayAppendLog(native);
        }

        if (!g_playEngineLoggedDiscovery && (flags & brlive::ENGINE_READY_PE_SCANNED))
        {
            g_playEngineLoggedDiscovery = true;
            char d[1024]={};
            snprintf(d,sizeof d,
                "[ENGINE DISCOVERY] windowTid=%ld | mapname=%08lX/x%ld frontend=%08lX/x%ld devmap=%08lX/x%ld map_restart=%08lX/x%ld disconnect=%08lX/x%ld cl_ingame=%08lX/x%ld sv_running=%08lX/x%ld fs_game=%08lX/x%ld",
                (long)g_playLiveShared->gameWindowThreadId,
                (unsigned long)(uint32_t)g_playLiveShared->anchorMapname.firstXrefRva,(long)g_playLiveShared->anchorMapname.xrefCount,
                (unsigned long)(uint32_t)g_playLiveShared->anchorFrontend.firstXrefRva,(long)g_playLiveShared->anchorFrontend.xrefCount,
                (unsigned long)(uint32_t)g_playLiveShared->anchorDevmap.firstXrefRva,(long)g_playLiveShared->anchorDevmap.xrefCount,
                (unsigned long)(uint32_t)g_playLiveShared->anchorMapRestart.firstXrefRva,(long)g_playLiveShared->anchorMapRestart.xrefCount,
                (unsigned long)(uint32_t)g_playLiveShared->anchorDisconnect.firstXrefRva,(long)g_playLiveShared->anchorDisconnect.xrefCount,
                (unsigned long)(uint32_t)g_playLiveShared->anchorClIngame.firstXrefRva,(long)g_playLiveShared->anchorClIngame.xrefCount,
                (unsigned long)(uint32_t)g_playLiveShared->anchorSvRunning.firstXrefRva,(long)g_playLiveShared->anchorSvRunning.xrefCount,
                (unsigned long)(uint32_t)g_playLiveShared->anchorFsGame.firstXrefRva,(long)g_playLiveShared->anchorFsGame.xrefCount);
            const bool allGuards = g_playLiveShared->cbufVerified && g_playLiveShared->mainFrameVerified && g_playLiveShared->serverRunningVerified &&
                                   g_playLiveShared->mapnameVerified && g_playLiveShared->nativeLineVerified && g_playLiveShared->nativeTriangleVerified &&
                                   g_playLiveShared->lifecycleVerified;
            if (allGuards) PlayAppendLog("[ENGINE] exact-build guards verified (Cbuf, main frame, typed state, native line and triangle, lifecycle); hooks go in when the level runs.");
            if (!allGuards) PlayAppendLog(d);
            char guards[1024] = {};
            snprintf(guards, sizeof guards,
                "[ENGINE GUARDS] Cbuf=0x%02lX/0x1F verified=%ld; mainFrame=0x%02lX/0x07 verified=%ld hook=%ld failures=%ld; typedState=0x%02lX/0x03 verified=%ld/%ld; nativeLine=0x%02lX/0x0F verified=%ld rva=0x%08lX mode=%ld; nativeTriangle=0x%02lX/0x3F verified=%ld hook=%ld rva=0x%08lX; lifecycle=0x%02lX/0x07 verified=%ld.",
                (unsigned long)(uint32_t)g_playLiveShared->cbufGuardMask, (long)g_playLiveShared->cbufVerified,
                (unsigned long)(uint32_t)g_playLiveShared->mainFrameGuardMask, (long)g_playLiveShared->mainFrameVerified,
                (long)g_playLiveShared->mainFrameHookInstalled, (long)g_playLiveShared->mainFrameInstallFailures,
                (unsigned long)(uint32_t)g_playLiveShared->typedStateGuardMask,
                (long)g_playLiveShared->serverRunningVerified, (long)g_playLiveShared->mapnameVerified,
                (unsigned long)(uint32_t)g_playLiveShared->nativeLineGuardMask,
                (long)g_playLiveShared->nativeLineVerified,
                (unsigned long)(uint32_t)g_playLiveShared->nativeLineRva,
                (long)g_playLiveShared->nativeDrawMode,
                (unsigned long)(uint32_t)g_playLiveShared->nativeTriangleGuardMask,
                (long)g_playLiveShared->nativeTriangleVerified,
                (long)g_playLiveShared->nativeTriangleHookInstalled,
                (unsigned long)(uint32_t)g_playLiveShared->nativeTriangleRva,
                (unsigned long)(uint32_t)g_playLiveShared->lifecycleGuardMask,
                (long)g_playLiveShared->lifecycleVerified);
            if (!allGuards) PlayAppendLog(guards);
            // the hooks are installed later (main frame), so only the guards decide here
            if (!g_playLiveShared->cbufVerified || !g_playLiveShared->mainFrameVerified)
                PlayAppendLog("[ENGINE] Exact-build Cbuf/main-frame guards did not promote command execution. This process remains fail-closed; no guessed BO3 function will be called.");
            if (!g_playLiveShared->nativeLineVerified)
                PlayAppendLog("[ENGINE] Exact-build native-line guards did not promote world drawing. Native rendering remains off; set BETTERRADIANT_BO3_D3D_FALLBACK=1 before launching Radiant only when the legacy developer overlay is needed.");
            if (!g_playLiveShared->nativeTriangleVerified)
                PlayAppendLog("[ENGINE] Exact-build native-triangle guards/hook did not promote filled drawing. The independent native green outline remains available.");
        }

        if (!g_playEngineLoggedLifecycleTrace && g_playLiveShared->lifecycleTraceInstalled)
        {
            g_playEngineLoggedLifecycleTrace=true;
            char lifecycle[768] = {};
            snprintf(lifecycle, sizeof lifecycle,
                "[ENGINE] VERIFIED lifecycle observation: restartGuard=0x%02lX/0x07 phaseGuard=0x%02lX/0x0F trace=%ld restartCore=0x%08lX ShutdownGame=0x%08lX InitGameProgs=0x%08lX Finalize=0x%08lX. Hooks observe BO3-owned calls only; Play Test BeginPlay/EndPlay remains disabled.",
                (unsigned long)(uint32_t)g_playLiveShared->lifecycleGuardMask,
                (unsigned long)(uint32_t)g_playLiveShared->lifecyclePhaseGuardMask,
                (long)g_playLiveShared->lifecycleTraceInstalled,
                (unsigned long)(uint32_t)g_playLiveShared->lifecycleRestartCoreRva,
                (unsigned long)(uint32_t)g_playLiveShared->lifecycleShutdownGameRva,
                (unsigned long)(uint32_t)g_playLiveShared->lifecycleInitGameProgsRva,
                (unsigned long)(uint32_t)g_playLiveShared->lifecycleFinalizeRva);
            PlayAppendLog(lifecycle);
        }

        if (g_playLiveShared->lifecycleCallOrder!=g_playEngineLastLifecycleOrder)
        {
            g_playEngineLastLifecycleOrder=g_playLiveShared->lifecycleCallOrder;
            if (g_playEngineLastLifecycleOrder>0)
            {
                char lifecycle[1024]={};
                snprintf(lifecycle,sizeof lifecycle,
                    "[LIFECYCLE OBSERVE] session=%s order=%ld shutdown=%ld@%ld arg=%ld endMs=%llu init=%ld@%ld args=(%ld,%ld,%ld) beginMs=%llu finalize=%ld@%ld callTid=%ld frameTid=%ld wrongThread=%ld | pid=%lu mapGeneration=%ld gameplayGeneration=%ld serverInitGeneration=%ld scriptVmGeneration=%ld verified=%ld playerSpawnGeneration=%ld verified=%ld mapLoadCount=%ld verified=%ld dbZoneLoadCount=%ld verified=%ld transientEntities=%ld verified=%ld",
                    PlaySessionStateName(g_playLiveShared->playSessionState),
                    (long)g_playLiveShared->lifecycleCallOrder,
                    (long)g_playLiveShared->lifecycleShutdownCallCount,(long)g_playLiveShared->lifecycleShutdownOrder,
                    (long)g_playLiveShared->lifecycleLastShutdownArg,
                    (unsigned long long)g_playLiveShared->lifecycleLastEndDurationMs,
                    (long)g_playLiveShared->lifecycleInitCallCount,(long)g_playLiveShared->lifecycleInitOrder,
                    (long)g_playLiveShared->lifecycleLastInitArg0,(long)g_playLiveShared->lifecycleLastInitArg1,
                    (long)g_playLiveShared->lifecycleLastInitArg2,
                    (unsigned long long)g_playLiveShared->lifecycleLastBeginDurationMs,
                    (long)g_playLiveShared->lifecycleFinalizeCallCount,(long)g_playLiveShared->lifecycleFinalizeOrder,
                    (long)g_playLiveShared->lifecycleLastCallThreadId,(long)g_playLiveShared->mainFrameThreadId,
                    (long)g_playLiveShared->lifecycleWrongThreadObservations,
                    (unsigned long)g_playLiveShared->gamePid,
                    (long)g_playLiveShared->mapGeneration,(long)g_playLiveShared->gameplayGeneration,
                    (long)g_playLiveShared->serverInitializationGeneration,
                    (long)g_playLiveShared->scriptVmGeneration,(long)g_playLiveShared->scriptVmGenerationVerified,
                    (long)g_playLiveShared->playerSpawnGeneration,(long)g_playLiveShared->playerSpawnGenerationVerified,
                    (long)g_playLiveShared->mapLoadCount,(long)g_playLiveShared->mapLoadCountVerified,
                    (long)g_playLiveShared->dbZoneLoadCount,(long)g_playLiveShared->dbZoneLoadCountVerified,
                    (long)g_playLiveShared->transientEntityCount,(long)g_playLiveShared->transientEntityCountVerified);
                PlayAppendLog(lifecycle);
            }
        }

        if (!g_playEngineLoggedGameState && g_playLiveShared->serverRunningVerified &&
            g_playLiveShared->mapnameVerified &&
            (flags & brlive::ENGINE_READY_GAMESTATE))
        {
            g_playEngineLoggedGameState = true;
            char gs[512] = {};
            snprintf(gs, sizeof gs,
                "[ENGINE] VERIFIED typed state: sv_running global RVA=0x%08lX, mapname global RVA=0x%08lX. MAP_ACTIVE requires sv_running plus an exact BO3/Radiant map match; state 9 is ignored.",
                (unsigned long)(uint32_t)g_playLiveShared->serverRunningDvarRva,
                (unsigned long)(uint32_t)g_playLiveShared->mapnameDvarRva);
            PlayAppendLog(gs);
        }

        if (g_playLiveShared->serverRunningVerified && g_playLiveShared->mapnameVerified)
        {
            const LONG server = g_playLiveShared->serverRunning;
            const LONG wa = g_playLiveShared->engineWorldActive;
            const std::string am = g_playLiveShared->activeMap;
            if (server != g_playEngineLastServerRunning || wa != g_playEngineLastWorldActive || am != g_playEngineLastActiveMap)
            {
                g_playEngineLastServerRunning = server;
                g_playEngineLastWorldActive = wa;
                g_playEngineLastActiveMap = am;
                char gs[768] = {};
                snprintf(gs, sizeof gs,
                    "[ENGINE GAMESTATE] sv_running=%ld mapname='%s' dvarType=%ld requested='%s' match=%ld worldActive=%ld samples=%ld failures=%ld lastFailure=%ld fastRestarts=%ld",
                    (long)server, g_playLiveShared->activeMap, (long)g_playLiveShared->mapnameDvarType,
                    g_playLiveShared->requestedMap,
                    (long)g_playLiveShared->activeMapMatchesRequested, (long)wa,
                    (long)g_playLiveShared->runtimeStateSampleCount,
                    (long)g_playLiveShared->runtimeStateSampleFailures,
                    (long)g_playLiveShared->runtimeStateLastFailure, (long)g_playLiveShared->fastRestartCount);
                PlayAppendLog(gs);
            }
        }

        // Registry ownership is independent of BO3 gameplay state: the bridge's typed worldActive gate decides whether native lines may be submitted.
        // Keeping the registry enabled lets objects reappear after frontend or loading transitions without a new Radiant selection.
        g_playLiveShared->enabled = g_playLivePreview && PlayLiveEditBuild() && !g_playLiveRegistry.empty() ? 1 : 0;
    }

    // Outlines follow the selection: the bridge draws green edges only for objects flagged SELECTED, so the flag tracks Radiant's selection (brush GUIDs, patch tiles by their GUID,
    // a selected misc_prefab marks its interior live objects). Flags are rewritten in place under the registry seqlock without bumping inputSeq: every bridge consumer snapshots the
    // registry per frame and a selection click wants no geometry, collision or textured rebuild.
    uint64_t g_playLiveSelectionPoll = 0;
    void PlaySyncLiveSelectionFlags(uint64_t now)
    {
        if (now-g_playLiveSelectionPoll<100 || g_playLiveRegistry.empty() || !g_playLiveShared) return;
        g_playLiveSelectionPoll=now;
        size_t selectionCount=0;
        if (!PlayTryFillSettledSelection(nullptr,0,selectionCount) || selectionCount>4096) return;
        std::vector<ModToolsSelectedBrush> selections(selectionCount);
        size_t selectedRead=0;
        if (selectionCount && !PlayTryFillSettledSelection(selections.data(),selections.size(),selectedRead)) return;
        std::set<std::string> selectedKeys, selectedEntities;
        for (size_t i=0;i<(std::min)(selectedRead,selections.size());++i)
        {
            selectedKeys.insert(PlayLiveGuidKey(selections[i].brushGuid));
            selectedEntities.insert(PlayLiveGuidKey(selections[i].entityGuid));
        }
        selectedKeys.erase(std::string());
        std::set<std::string> prefabKeys;
        for (const auto& pair:g_playBakedPrefabs)
            if (selectedEntities.count(pair.first)) prefabKeys.insert(pair.second.liveKeys.begin(),pair.second.liveKeys.end());
        bool changed=false;
        std::map<uint64_t,bool> selectedById;
        const bool insidePrefab=PlayPrefabDepth()>0;   // a selected brush of the entered file outlines its copy in every instance
        for (auto& pair:g_playLiveRegistry)
        {
            const bool selected=pair.second.fromPrefab ? (prefabKeys.count(pair.first)!=0 ||
                                                          (insidePrefab && !pair.second.prefabBrushKey.empty() && selectedKeys.count(pair.second.prefabBrushKey)!=0))
                                                       : selectedKeys.count(PlayRegistryKeyGuid(pair.first))!=0;
            const LONG flags=selected ? (pair.second.flags|brlive::LIVE_OBJECT_SELECTED) : (pair.second.flags&~brlive::LIVE_OBJECT_SELECTED);
            if (flags!=pair.second.flags) { pair.second.flags=flags; changed=true; }
            selectedById[pair.second.liveObjectId]=selected;
        }
        if (!changed) return;
        InterlockedIncrement(&g_playLiveShared->liveRegistrySeq);
        MemoryBarrier();
        const LONG count=(std::min)(static_cast<LONG>(g_playLiveShared->liveObjectCount),static_cast<LONG>(brlive::kMaxLiveObjects));
        for (LONG i=0;i<count;++i)
        {
            brlive::LiveObjectRecord& target=g_playLiveShared->liveObjects[i];
            const auto found=selectedById.find(target.liveObjectId);
            if (found==selectedById.end()) continue;
            target.flags=found->second ? (target.flags|brlive::LIVE_OBJECT_SELECTED) : (target.flags&~brlive::LIVE_OBJECT_SELECTED);
        }
        MemoryBarrier();
        InterlockedIncrement(&g_playLiveShared->liveRegistrySeq);
    }

    void PlayTickLivePreview()
    {
        if(!g_playLivePreview || !PlayLiveEditBuild() || g_playInputCaptured || PlayBusy()) return;
        uint64_t now=GetTickCount64(); if(now-g_playLastLivePoll<33) return; g_playLastLivePoll=now;
        DWORD pid=0; {std::lock_guard<std::mutex> lock(g_playMutex);pid=g_playGamePid;} if(!pid) return;
        if(!PlayEnsureLiveShared(pid) || !g_playLiveBridgeInjected) return;
        // Geometry internals stay release-only: a plain selection click does not sample BrushInst/brush_t data, a real edit drag arms one settled snapshot 300 ms after release.
        // Registry deletion reconciliation uses stable GUIDs only and never retains a Radiant pointer.
        PlayTryPublishDeleteFollowup(now);   // clip pieces (Select_Delete ran, the pieces are the new selection)
        PlayTryPublishReleaseSnapshot(now);
        PlayTryPublishMaterialChange(now);
        PlayReconcileLiveRegistryDeletes(now);
        PlayReconcileLiveHides(now,false);
        PlayReconcileLights(now);
        PlaySyncLiveSelectionFlags(now);
        if(g_playLiveShared && !g_playLiveSentDisabled)
        {
            PlayPublishLiveClear();
            PlayPublishLiveRegistry();
            if (!g_playBakedHullsLoaded) PlayLoadBakedHulls("bridge connected");
            PlayPublishLiveHides();
            g_playLiveSentDisabled=true;
            g_playLastLiveKey.clear();
            g_playLiveUpdates=1;
            PlayAppendLog("[LIVE] Persistent Live Edit registry synchronized. Plain selection clicks are ignored; settled edits CREATE or UPDATE independent objects, while typed worldActive remains the sole native drawing gate.");
        }
        if(g_playLiveShared)
        {
            LONG frame=g_playLiveShared->frameSeq; if(frame!=g_playLiveLastFrameSeq) g_playLiveLastFrameSeq=frame;
            LONG st=g_playLiveShared->status;
            std::string statusText=g_playLiveShared->statusText;
            if(statusText!=g_playLiveBridgeStatus && !statusText.empty())
            {
                g_playLiveBridgeStatus=statusText;
                if(st==brlive::STATUS_UNSUPPORTED_BUILD||st==brlive::STATUS_PROJECTION_FAULT) PlayAppendLog("[LIVE ERROR] "+statusText);
                else PlayAppendLog("[LIVE] Native bridge status: "+statusText);
            }
            if((g_playLiveShared->nativeEdgeSubmits>0 || g_playLiveShared->nativeTriangleSubmitCount>0 ||
                g_playLiveShared->renderProbeCount>0) &&
               (g_playLiveShared->inputSeq!=g_playLiveLastDiagInputSeq ||
                g_playLiveShared->nativeLastInputSeq!=g_playLiveLastDiagProjectionSeq))
            {
                g_playLiveLastDiagInputSeq=g_playLiveShared->inputSeq;
                g_playLiveLastDiagProjectionSeq=g_playLiveShared->nativeLastInputSeq;
                const char* geometryType = g_playLiveShared->geometryType==brlive::LIVE_GEOMETRY_BRUSH ? "brush" :
                    (g_playLiveShared->geometryType==brlive::LIVE_GEOMETRY_PATCH ? "patch" : "none");
                char diag[2048]={};
                snprintf(diag,sizeof diag,
                    "[LIVE DIAG] geometryType=%s faceCount=%ld planeCount=%ld reconstructedVertexCount=%ld uniqueEdgeCount=%ld degenerateEdges=%ld reconstructionFailures=%ld fallbackToAabb=%ld nativeEdgeSubmits=%ld | liveObjectCount=%ld liveBrushCount=%ld livePatchCount=%ld createCount=%ld updateCount=%ld deleteCount=%ld staleObjectCount=%ld registryGeneration=%ld clearCount=%ld lastOp=%ld lastObjectId=%llu lastObjectGeneration=%ld | patchRows=%ld patchColumns=%ld patchControlPointCount=%ld patchWireEdgeCount=%ld patchReconstructionFailures=%ld patchFallbackToAabb=%ld | inputSeq=%ld geometrySeq=%ld registrySeq=%ld verts=%ld edges=%ld dragReleaseEvents=%u snapshotFailures=%u | nativeMode=%ld lineRva=0x%08lX verified=%ld guard=0x%02lX aabbSubmits=%ld lineSubmits=%ld failures=%ld drawTid=%ld frameTid=%ld nativeSeq=%ld nativeVerts=%ld nativeEdges=%ld inactiveSkips=%ld | solidRva=0x%08lX verified=%ld guard=0x%02lX hook=%ld submits=%ld failures=%ld drawTid=%ld lastTriangles=%ld | D3DfallbackProbe=%ld Present=%ld accepted=%ld probeDraws=%ld frameSeq=%ld",
                    geometryType,
                    (long)g_playLiveShared->faceCount,
                    (long)g_playLiveShared->planeCount,
                    (long)g_playLiveShared->reconstructedVertexCount,
                    (long)g_playLiveShared->uniqueEdgeCount,
                    (long)g_playLiveShared->degenerateEdges,
                    (long)g_playLiveShared->reconstructionFailures,
                    (long)g_playLiveShared->fallbackToAabb,
                    (long)g_playLiveShared->nativeEdgeSubmits,
                    (long)g_playLiveShared->liveObjectCount,
                    (long)g_playLiveShared->liveBrushCount,
                    (long)g_playLiveShared->livePatchCount,
                    (long)g_playLiveShared->liveCreateCount,
                    (long)g_playLiveShared->liveUpdateCount,
                    (long)g_playLiveShared->liveDeleteCount,
                    (long)g_playLiveShared->staleObjectCount,
                    (long)g_playLiveShared->liveRegistryGeneration,
                    (long)g_playLiveShared->liveClearCount,
                    (long)g_playLiveShared->lastRegistryOperation,
                    (unsigned long long)g_playLiveShared->lastRegistryObjectId,
                    (long)g_playLiveShared->lastRegistryObjectGeneration,
                    (long)g_playLiveShared->patchRows,
                    (long)g_playLiveShared->patchColumns,
                    (long)g_playLiveShared->patchControlPointCount,
                    (long)g_playLiveShared->patchWireEdgeCount,
                    (long)g_playLiveShared->patchReconstructionFailures,
                    (long)g_playLiveShared->patchFallbackToAabb,
                    (long)g_playLiveShared->inputSeq,(long)g_playLiveShared->nativeGeometrySeq,
                    (long)g_playLiveShared->liveRegistrySeq,
                    (long)g_playLiveShared->vertexCount,(long)g_playLiveShared->edgeCount,
                    (unsigned)g_playLiveReleaseEvents,(unsigned)g_playLiveSnapshotFailures,
                    (long)g_playLiveShared->nativeDrawMode,
                    (unsigned long)(uint32_t)g_playLiveShared->nativeLineRva,
                    (long)g_playLiveShared->nativeLineVerified,
                    (unsigned long)(uint32_t)g_playLiveShared->nativeLineGuardMask,
                    (long)g_playLiveShared->nativeAabbSubmitCount,
                    (long)g_playLiveShared->nativeLineSubmitCount,
                    (long)g_playLiveShared->nativeDrawFailures,
                    (long)g_playLiveShared->nativeDrawThreadId,
                    (long)g_playLiveShared->mainFrameThreadId,
                    (long)g_playLiveShared->nativeLastInputSeq,
                    (long)g_playLiveShared->nativeLastVertexCount,
                    (long)g_playLiveShared->nativeLastEdgeCount,
                    (long)g_playLiveShared->nativeWorldInactiveSkips,
                    (unsigned long)(uint32_t)g_playLiveShared->nativeTriangleRva,
                    (long)g_playLiveShared->nativeTriangleVerified,
                    (unsigned long)(uint32_t)g_playLiveShared->nativeTriangleGuardMask,
                    (long)g_playLiveShared->nativeTriangleHookInstalled,
                    (long)g_playLiveShared->nativeTriangleSubmitCount,
                    (long)g_playLiveShared->nativeTriangleDrawFailures,
                    (long)g_playLiveShared->nativeTriangleThreadId,
                    (long)g_playLiveShared->nativeLastTriangleCount,
                    (long)g_playLiveShared->matrixProbeActive,
                    (long)g_playLiveShared->presentCount,(long)g_playLiveShared->acceptedPresentCount,
                    (long)g_playLiveShared->renderProbeCount,(long)g_playLiveShared->frameSeq);
                PlayAppendLog(diag);
            }
            if(g_playLiveShared->nativeDrawMode==brlive::LIVE_RENDER_D3D_FALLBACK &&
               g_playLiveShared->enabled && g_playLiveShared->engineWorldActive && !g_playLiveDiagWarned)
            {
                if(g_playLiveShared->presentCount>120 && g_playLiveShared->acceptedPresentCount==0)
                {
                    g_playLiveDiagWarned=true;
                    PlayAppendLog("[LIVE DIAG] Present hook is firing, but no BO3-owned visible swap chain matched. Send this line back; the next fix is swap-chain selection.");
                }
                else if(g_playLiveShared->acceptedPresentCount>120 && g_playLiveShared->renderProbeCount==0)
                {
                    g_playLiveDiagWarned=true;
                    PlayAppendLog("[LIVE DIAG] BO3 swap chain is matched, but the D3D11 probe draw never completed. Send this line back; the next fix is render-resource/backbuffer state.");
                }
            }
        }
    }


    bool PlayPrewarmOutputsReady(const PlayMapInfo& m)
    {
        const std::wstring zoneDir = PlayJoin(m.usermapRoot, L"zone");
        const std::wstring mainFf = PlayJoin(zoneDir, m.mapName + L".ff");
        const std::wstring langFf = PlayJoin(zoneDir, L"en_" + m.mapName + L".ff");
        return PlayFileExists(mainFf) && PlayFileExists(langFf);
    }

    // Early launch, UI tick: the bridge must gate the map file opens (else the game is closed and the
    // build launches it afterwards), once the build is done and the zone module answered (it loads the patch zone with the
    // level, armed 20 s after the launch), the map file opens are released.
    void PlayEarlyLaunchTick(uint64_t now)
    {
        if (g_playEarlyReloadPending && g_playLiveZoneInjected && g_playLiveShared && g_playLiveShared->engineWorldActive && !PlayBusy())
        {
            g_playEarlyReloadPending = false;
            if (PlayLiveZoneSendCommand("devmap " + g_playCurrentMap))
                PlayAppendLog("[EARLY] the level had opened its files before the hold: reloaded once so it runs the new build.");
        }
        // a hold that failed after the build had finished: the game was closed, launch it now like a normal cold Play
        static bool relaunch = false;
        if (relaunch && !g_playGamePid && g_playThreadDone && !PlayBusy())
        {
            relaunch = false;
            if (g_playThread.joinable()) g_playThread.join();
            g_playThreadDone = false;
            g_playCancel = false;
            g_playState = PLAY_LAUNCHING;
            PlayMapInfo m = g_playEarlyMap;
            g_playThread = std::thread([m]() { try { PlayLaunchGame(m); } catch (...) {} g_playThreadDone = true; });
            return;
        }
        if (!g_playLaunchHeld || !g_playGamePid) return;
        brlive::SharedState* sh = (g_playLiveShared && g_playLiveBridgePid == g_playGamePid) ? g_playLiveShared : nullptr;
        const LONG state = sh ? sh->launchHoldState : 0;
        // The bridge gates the map's file opens (state 5 armed, 1 holding). A failure is: no gate soon after the bridge
        // connected, or the level running (main frame pumping, which starts after the level load) without one held open.
        const bool noGate = sh ? (state == 0 && now - g_playEarlyLaunchMs > 15000) : now - g_playEarlyLaunchMs > 30000;
        const bool missed = sh && state == 5 && sh->mainFramePumpCount > 0;
        if (noGate || missed)
        {
            g_playLaunchHeld = false;
            g_playEarlyAborted = true;
            relaunch = g_playEarlyBuild == 2;   // the worker is done: nobody else launches it
            g_playEarlyBuild = 0;
            HANDLE game = nullptr;
            { std::lock_guard<std::mutex> lock(g_playMutex); game = g_playGameProcess; }
            if (game && WaitForSingleObject(game, 0) == WAIT_TIMEOUT) TerminateProcess(game, 0);
            PlayNote("BO3 could not wait for the build, so it was closed; it starts again once the build is done.");
            PlayAppendLog(std::string("[EARLY WARN] the bridge could not hold BO3's map files (") +
                          (!sh ? "no bridge" : missed ? "the level loaded without a held open" : "its gate did not go in") +
                          "): that game was closed, BO3 starts after the build as before. early_launch=0 in PlayTest.ini turns this off.");
            return;
        }
        if (g_playEarlyBuild != 2 || !sh || now - g_playEarlyLaunchMs < g_playEarlyMinHoldMs) return;
        const bool zoneReady = !g_playLiveZone || g_playLiveZoneInjected || g_playLiveZoneWarned || now - g_playEarlyLaunchMs > 45000;
        if (!zoneReady) return;
        // the console log shows whether a level file was already open (the hold came too late): reload once it is up
        const std::wstring consoleLog = PlayJoin(g_playEarlyGameRoot, L"console_mp.log");
        if (!g_playCurrentMap.empty() && PlayFileTime(consoleLog) >= g_playEarlyLaunchFt &&
            PlayReadFileTail(consoleLog, 4u << 20).find("Loading fastfile '" + g_playCurrentMap + "'") != std::string::npos)
            g_playEarlyReloadPending = true;
        InterlockedExchange(&sh->launchHold, 0);
        g_playLaunchHeld = false;
        g_playEarlyBuild = 0;
        char secs[32]; snprintf(secs, sizeof secs, "%.1f", (now - g_playEarlyLaunchMs) / 1000.0);
        PlayAppendLog(std::string("[EARLY] BO3 goes into the map now (") + secs + " s after it started)" + (g_playEarlyReloadPending ? "; it had opened the old level files, so it reloads once in the level." : "."));
    }

    void PlayTickPrewarm(uint64_t now)
    {
        if (!g_playEnabled || PlayBusy()) return;
        if (now - g_playLastPrewarmPoll < 500) return;
        g_playLastPrewarmPoll = now;

        // Read the two runtime-life options continuously so merely checking the box starts warming without a Play.
        if (rp::host && rp::host->ui && rp::host->ui->IsChecked)
        {
            const bool oldWarm = g_playKeepWarm;
            const bool oldPrewarm = g_playPrewarmOnMapOpen;
            if (g_playWarmCheck) g_playKeepWarm = rp::host->ui->IsChecked(g_playWarmCheck) != 0;
            if (g_playPrewarmCheck) g_playPrewarmOnMapOpen = rp::host->ui->IsChecked(g_playPrewarmCheck) != 0;
            if (oldWarm != g_playKeepWarm) rp::ConfigSet("playtest", "keep_warm", g_playKeepWarm ? "1" : "0");
            if (oldPrewarm != g_playPrewarmOnMapOpen) rp::ConfigSet("playtest", "prewarm_on_map_open", g_playPrewarmOnMapOpen ? "1" : "0");
        }
        if (!g_playPrewarmOnMapOpen || !g_playEmbedGame) return;   // a parked game needs the BO3 Live pane to come back in

        PlayMapInfo m; std::string error;
        if (!PlayResolveCurrentMap(m, error)) return;
        const std::string mapUtf8 = PlayWideToUtf8(m.mapName);
        g_playCurrentMap = mapUtf8;

        // Explicit Close BO3 means "leave it closed" for this map. A different map clears that one-map suppression.
        if (!g_playPrewarmSuppressedMap.empty())
        {
            if (_stricmp(g_playPrewarmSuppressedMap.c_str(), mapUtf8.c_str()) == 0) return;
            g_playPrewarmSuppressedMap.clear();
        }

        DWORD pid = 0;
        if (PlayTrackedGameRunning(&pid))
        {
            // If a hidden hot runtime belongs to the old editor map, retire it now: the next tick prewarms the new map with the correct fs_game instead of making its first Play pay a cold start.
            if ((g_playParked || !g_playGamePresented) && !g_playGameMap.empty() && _stricmp(g_playGameMap.c_str(), mapUtf8.c_str()) != 0)
            {
                HANDLE game = nullptr;
                { std::lock_guard<std::mutex> lock(g_playMutex); game = g_playGameProcess; }
                if (game && WaitForSingleObject(game, 0) == WAIT_TIMEOUT)
                {
                    PlayAppendLog("[PREWARM] Radiant changed maps from " + g_playGameMap + " to " + mapUtf8 + "; retiring the hidden old runtime so the new map can prewarm.");
                    TerminateProcess(game, 0);
                    g_playLastPrewarmAttempt = now;
                }
                return;
            }

            // While not in Play keep BO3 invisible: this catches window creation that ignores STARTUPINFO::SW_HIDE, so the frontend/loading window never steals focus.
            if (!g_playGamePresented)
            {
                HWND game = g_playEmbeddedHwnd && IsWindow(g_playEmbeddedHwnd)
                    ? g_playEmbeddedHwnd : PlayFindGameWindow(pid);
                if (g_playEmbeddedHwnd) PlayDetachEmbeddedGame();
                if (!game || !IsWindow(game)) game = PlayFindGameWindow(pid);
                if (game && IsWindow(game))
                {
                    if (IsWindowVisible(game)) ShowWindow(game, SW_HIDE);
                    g_playPrewarmWindowHidden = true;
                    g_playPrewarmed = true;
                }
            }
            return;
        }

        // Never hijack/hide a BO3 instance the user launched manually. Smart Play can still adopt it explicitly.
        if (PlayFindBo3ProcessId()) return;
        if (!PlayPrewarmOutputsReady(m)) return; // first-ever build still has to happen once

        // Small backoff prevents Steam/process churn if BO3 is still shutting down or a map just crashed.
        if (now - g_playLastPrewarmAttempt < 2500) return;
        g_playLastPrewarmAttempt = now;
        {
            const double freeGb = PlayFreeMemoryGb();
            const int needGb = (std::max)(1, rp::ConfigGetInt("playtest", "prewarm_min_free_gb", 12));
            if (freeGb < needGb)
            {
                static uint64_t lastLog = 0;
                if (now - lastLog > 600000) { lastLog = now; char line[200]; snprintf(line, sizeof line, "[PREWARM] %.1f GB of memory free (needs %d): no background start for now.", freeGb, needGb); PlayAppendLog(line); }
                g_playLastPrewarmAttempt = now + 27500;   // look again in 30 s
                return;
            }
        }
        g_playCancel = false;
        PlayAppendLog("[PREWARM] Compiled fastfiles already exist for " + mapUtf8 + ". Starting BO3 hidden now so engine/Steam/map startup happens while you edit.");
        if (!PlayLaunchGame(m, true))
        {
            // PlayLaunchGame reports the real reason. Do not hammer Steam every tick after a failure.
            g_playLastPrewarmAttempt = now + 7500;
        }
    }


    // UI tick: closes the "load" wait once the bridge reports the level running (PlayLoadWaitBegin).
    void PlayLoadWaitTick()
    {
        if (!g_playLoadWatch) return;
        DWORD pid = 0;
        { std::lock_guard<std::mutex> lock(g_playMutex); pid = g_playGamePid; }
        if (!pid || !PlayWaitOpen("load")) { g_playLoadWatch = false; PlayWaitEnd("load"); return; }   // the game went (its exit note says so) or the wait gave up
        brlive::SharedState* sh = (g_playLiveShared && g_playLiveBridgePid == pid) ? g_playLiveShared : nullptr;
        if (!sh || !sh->engineWorldActive) { g_playLoadSawInactive = true; return; }
        const LONG base = g_playLoadShutdownBase;
        if (!g_playLoadSawInactive && !(base >= 0 && sh->lifecycleShutdownCallCount != base)) return;   // still the level from before the reload
        g_playLoadWatch = false;
        const uint64_t ms = PlayWaitEnd("load");
        const uint64_t started = g_playStartedMs.load();
        std::string took;
        if (ms) took = " (load " + PlayDuration(ms) + (started ? ", " + PlayDuration(GetTickCount64() - started) + " since Play" : std::string()) + ")";
        PlayNote("In game: " + (g_playGameMap.empty() ? std::string("the map") : g_playGameMap) + " is loaded" + took +
                 ". Click the BO3 Live view to play; Esc frees the mouse.");
    }

    void PlayTick()
    {
        uint64_t now = GetTickCount64();
        std::vector<std::string> pending;
        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            pending.swap(g_playPendingLog);
        }
        PlayLanPeersTick(false);
        for (const auto& line : pending) rp::Log("play: %s", line.c_str());

        // Refresh the BO3 Live view rect here, on the UI thread, so the worker's windowed launch reads a live cache.
        if (g_playGameView && g_playEmbedGame) PlayGamePaneScreenRect();

        HANDLE game = nullptr;
        { std::lock_guard<std::mutex> lock(g_playMutex); game = g_playGameProcess; }
        if (game && WaitForSingleObject(game, 0) == WAIT_OBJECT_0)
        {
            const bool hiddenPrewarmExit = g_playParked || (g_playPrewarmed && !g_playGamePresented);
            const std::string exitedMap = g_playGameMap;
            DWORD code = 0; GetExitCodeProcess(game, &code);
            PlayDetachEmbeddedGame();
            {
                std::lock_guard<std::mutex> lock(g_playMutex);
                if (g_playGameProcess == game) { CloseHandle(g_playGameProcess); g_playGameProcess = nullptr; g_playGamePid = 0; g_playGameOwnedByRadiant = false; }
            }
            g_playGameMap.clear();
            g_playGamePresented = false;
            g_playPrewarmed = false;
            g_playPrewarmWindowHidden = false;
            g_playParked = false;
            PlayAppendLog("[EXIT] BlackOps3 -> " + std::to_string(code));
            PlayWaitEnd("load"); PlayWaitEnd("assets"); PlayWaitEnd("nav");
            g_playLoadWatch = false;
            if (code == 0) PlayNote("BO3 closed.");
            else if (code != 0xC0000194) { char hex[16]; snprintf(hex, sizeof hex, "0x%08lX", (unsigned long)code); PlayNote(std::string("BO3 closed unexpectedly (exit code ") + hex + "), it may have crashed. BO3 Enhanced writes crashes.log next to BlackOps3.exe."); }
            PlayVideoConfigRestore(g_playVideoGameRoot, "game exited");
            PlayParkGuardConfigOnExit();
            if (!PlayBusy() && !g_playLiveMatRunning) PlayLiveMaterialsRemoveAllZones("game exited");
            if (hiddenPrewarmExit && !exitedMap.empty())
            {
                g_playPrewarmSuppressedMap = exitedMap;
                PlayAppendLog("[PREWARM WARN] The hidden runtime exited before Play. Automatic prewarm is suppressed for this map until you press Play or switch maps, preventing a launch/crash loop.");
                PlayNote("The BO3 started in the background closed before Play; it is not started in the background again for this map until you press Play.");
            }
            if (g_playState == PLAY_RUNNING) g_playState = PLAY_IDLE;
        }
        if (g_playThread.joinable() && g_playThreadDone) g_playThread.join();

        PlayTickPrewarm(now);
        PlayParkTick(now);
        PlayCheatsTick(now);
        PlayEarlyLaunchTick(now);
        PlayTickEngineRuntime();
        if (g_playLiveClearAfterBuild.exchange(false))
        {
            PlayClearLiveRegistry("successful BSP rebuild superseded Live Edit overrides");
            // the clear restores the baked brushes of a level that is being replaced: no nav refresh for it
            g_playNavGeometrySeen = g_playLiveHideGeneration; g_playNavDueMs = 0;
        }
        PlayTrackReleaseSnapshot(now);
        PlayTickLivePreview();
        PlayLiveBakeTick(now);

        // Input handoff for embedded PIE-style testing: BO3 stays over its dedicated BO3 Live pane. No escape chord is reserved: pause with Esc so BO3 releases the cursor, then click Radiant.
        // In-place play: the game never leaves the pane, the cursor follows what the game wants (see the function).
        PlayMirrorGameCursor();

        // Let the embed checkbox be toggled while a warm game sits idle: the BO3 HWND is resized to the BO3 Live pane each tick, so docking or resizing Radiant keeps it aligned.
        if (!PlayBusy() && rp::host && rp::host->ui && rp::host->ui->IsChecked)
        {
            if (g_playWarmCheck) g_playKeepWarm = rp::host->ui->IsChecked(g_playWarmCheck) != 0;
            if (g_playPrewarmCheck) g_playPrewarmOnMapOpen = rp::host->ui->IsChecked(g_playPrewarmCheck) != 0;
            if (g_playLiveCheck && PlayLiveEditBuild()) g_playLivePreview = rp::host->ui->IsChecked(g_playLiveCheck) != 0;   // greyed without Enhanced: shown unchecked, preference kept
            // Follow the pane's checkbox only while it exists: after a hot reload the pane can lag behind and a missing checkbox must not turn docking off.
            if (g_playEmbedCheck) g_playEmbedGame = rp::host->ui->IsChecked(g_playEmbedCheck) != 0;
            const bool embedNow = g_playEmbedGame;
            if (!embedNow || !g_playGamePresented) PlayDetachEmbeddedGame();
            else if (g_playGamePid)
            {
                HWND gameWindow = g_playEmbeddedHwnd ? g_playEmbeddedHwnd : PlayFindGameWindow(g_playGamePid);
                // No flash of a top-level game window over the editor: hide it until the bridge docks it (DockInto shows it). Before the dock the threads share no input queue, so the request is really asynchronous.
                if (gameWindow && !g_playEmbeddedHwnd && !GetParent(gameWindow) && IsWindowVisible(gameWindow))
                    SetWindowPos(gameWindow, nullptr, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
                // a parked game that was never docked (background start) stays a hidden top-level window until Play opens the pane
                // a held early launch is docked after the release (the dock refresh needs the game running)
                if (gameWindow && !g_playEmbeddedHwnd && !g_playParked && !g_playLaunchHeld && PlayEmbedGameWindow(gameWindow))
                    PlayAppendLog("[EMBED] BO3 docked in the BO3 Live pane.");
                PlayParkHideTick();
                if (g_playEmbeddedHwnd && !g_playParked && !g_playParkHidden) { PlayRedockIfEscaped(); PlayUpdateEmbeddedBounds(); }
            }
        }
        else if (g_playLaunchHeld && g_playGamePid && !g_playEmbeddedHwnd)
        {
            // early launch: the game waits for the build, its window stays hidden (docked once the build is done)
            HWND gameWindow = PlayFindGameWindow(g_playGamePid);
            if (gameWindow && !GetParent(gameWindow) && IsWindowVisible(gameWindow))
                SetWindowPos(gameWindow, nullptr, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        }
        PlayHangWatchdog(now);
        PlayLoadWaitTick();
        PlayWaitTick(now);
        if (g_playInactiveDue && now >= g_playInactiveDue)
        {
            g_playInactiveDue = 0;
            if (!g_playInputCaptured && g_playEmbeddedHwnd && IsWindow(g_playEmbeddedHwnd))
            {
                PlayTellGameInactive(g_playEmbeddedHwnd);
                g_playInactiveDue = 0;
            }
        }

        int stateNow = g_playState.load();
        if (stateNow == PLAY_FAILED && g_playLastUiState != PLAY_FAILED) PlayShowPane();
        g_playLastUiState = stateNow;
        if (now - g_playLastUiRefresh >= 100)
        {
            g_playLastUiRefresh = now;
            PlayRefreshUi();
        }
    }

    void PlayInit()
    {
        g_playEnabled = rp::ConfigGetBool("playtest", "enabled", true);
        g_playAutoSave = rp::ConfigGetBool("playtest", "auto_save", true);
        g_playVerboseLog = rp::ConfigGetBool("playtest", "verbose_log", false);
        g_playKillPrevious = rp::ConfigGetBool("playtest", "kill_previous", true);
        g_playKeepWarm = rp::ConfigGetBool("playtest", "keep_warm", true);
        g_playEarlyLaunch = rp::ConfigGetBool("playtest", "early_launch", true);
        g_playEarlyMinHoldMs = 1000ull * (uint64_t)(std::max)(0, rp::ConfigGetInt("debug", "early_min_hold_s", 0));
        // One-time migration: the default is no longer hiding a conventional cold launch, the checkbox stays as a legacy fallback.
        if (!rp::ConfigGetBool("playtest", "v19_engine_runtime_migrated", false))
        {
            g_playPrewarmOnMapOpen = false;
            rp::ConfigSet("playtest", "prewarm_on_map_open", "0");
            rp::ConfigSet("playtest", "v19_engine_runtime_migrated", "1");
        }
        else g_playPrewarmOnMapOpen = rp::ConfigGetBool("playtest", "prewarm_on_map_open", false);
        // One-time migration of the old camera-overlay preference to the dedicated BO3 Live pane.
        if (!rp::ConfigGetBool("playtest", "v17_game_pane_migrated", false))
        {
            g_playEmbedGame = true;
            rp::ConfigSet("playtest", "embed_game", "1");
            rp::ConfigSet("playtest", "v17_game_pane_migrated", "1");
        }
        else g_playEmbedGame = rp::ConfigGetBool("playtest", "embed_game", true);
        g_playExportLighting = rp::ConfigGetBool("playtest", "export_lighting", true);
        g_playLiveBake = rp::ConfigGetBool("playtest", "live_bake", true);
        g_playLiveBakeReload = rp::ConfigGetBool("playtest", "live_bake_reload", true);
        g_playLiveBakeLayout = rp::ConfigGetBool("playtest", "live_bake_layout", true);
        g_playRebuildNav = rp::ConfigGetBool("playtest", "smart_rebuild_nav", true);   // off left the game on the compile's old nav mesh
        g_playLiveZone = rp::ConfigGetBool("playtest", "live_zone", false);
        g_playAutoLinkMaterials = rp::ConfigGetBool("playtest", "auto_link_materials", true);
        g_playHangKillSeconds = rp::ConfigGetInt("playtest", "hang_kill_seconds", 12);
        g_playLiveMaterialLoad = rp::ConfigGetBool("playtest", "live_material_load", true);
        {
            g_playPreloadMaterials.clear();
            std::string list = rp::ConfigGet("playtest", "preload_materials", "");
            size_t start = 0;
            while (start <= list.size())
            {
                size_t comma = list.find_first_of(",;", start); if (comma == std::string::npos) comma = list.size();
                std::string item = list.substr(start, comma - start);
                while (!item.empty() && isspace((unsigned char)item.front())) item.erase(0, 1);
                while (!item.empty() && isspace((unsigned char)item.back())) item.pop_back();
                if (!item.empty()) g_playPreloadMaterials.push_back(item);
                start = comma + 1;
            }
        }
        g_playAutoRestart = rp::ConfigGetBool("playtest", "auto_restart_gameover", true);
        // One-time migration of the old startup-fade bypass setting (it could send otherwise-good maps to the frontend).
        if (!rp::ConfigGetBool("playtest", "v17_safe_start_migrated", false))
        {
            g_playInstantStart = false;
            rp::ConfigSet("playtest", "instant_start", "0");
            rp::ConfigSet("playtest", "v17_safe_start_migrated", "1");
        }
        else g_playInstantStart = rp::ConfigGetBool("playtest", "instant_start", false);
        // the bypass is per player now (no level-wide fade call): on by default, once
        if (!rp::ConfigGetBool("playtest", "v20_instant_start_on", false))
        {
            g_playInstantStart = true;
            rp::ConfigSet("playtest", "instant_start", "1");
            rp::ConfigSet("playtest", "v20_instant_start_on", "1");
        }
        g_playFromCamera = false;   // Spawn at camera off for now, play_from_camera is not read
        g_playFly = rp::ConfigGetBool("playtest", "play_fly", false);
        g_playDisableAi = rp::ConfigGetBool("playtest", "disable_ai", false);
        g_playGod = rp::ConfigGetBool("playtest", "play_god", false);
        g_playLivePreview = rp::ConfigGetBool("playtest", "live_preview", true);
        g_playLiveDelete = rp::ConfigGetBool("playtest", "live_delete", true);
        g_playLiveDeleteCollision = rp::ConfigGetBool("playtest", "live_delete_collision", true);
        g_playLiveDeletePrefabs = rp::ConfigGetBool("playtest", "live_delete_prefabs", true);
        g_playLiveDeleteModels = rp::ConfigGetBool("playtest", "live_delete_models", true);
        g_playLiveModels = rp::ConfigGetBool("playtest", "live_models", true);
        g_playLiveSpots = rp::ConfigGetBool("playtest", "live_spots", true);
        g_playLiveDecals = rp::ConfigGetBool("playtest", "live_decals", true);
        g_playLiveFx = rp::ConfigGetBool("playtest", "live_fx", true);
        g_playLiveSky = rp::ConfigGetBool("playtest", "live_sky", true);
        g_playLiveSkyBake = rp::ConfigGetBool("playtest", "live_sky_bake", true);
        g_playLiveLights = rp::ConfigGetBool("playtest", "live_lights", true);
        g_playLiveFillHoles = rp::ConfigGetBool("playtest", "live_fill_holes", true);
        g_playLiveModelsDebug = rp::ConfigGetBool("playtest", "live_models_debug", false);
        g_playLiveNavMesh = rp::ConfigGetBool("playtest", "live_navmesh", true);
        g_playJoinUsermapEntry = rp::ConfigGetBool("playtest", "join_usermap_entry", true);
        g_playLiveNavRefresh = rp::ConfigGetBool("playtest", "live_nav_refresh", true);
        g_playLiveNavRevert = rp::ConfigGetBool("playtest", "live_nav_revert", true);
        g_playWarmAfterLiveZone = rp::ConfigGetBool("playtest", "warm_after_live_zone", true);
        g_playLiveOcclusion = rp::ConfigGetBool("playtest", "live_occlusion", true);
        if (g_playLiveShared) InterlockedExchange(&g_playLiveShared->liveNavEnabled, g_playLiveNavMesh ? 1 : 0);
        if (g_playLiveShared) InterlockedExchange(&g_playLiveShared->liveOccEnabled, g_playLiveOcclusion ? 1 : 0);
        if (g_playLivePreview && g_playLiveDelete) PlayInstallDeleteHook();
        if (g_playLivePreview) PlayInstallUndoHook();
        g_playLanguage = rp::ConfigGet("playtest", "language", "english");
        g_playExtraArgs = rp::ConfigGet("playtest", "extra_args", "");
        PlayDvarsLoad();
        PlayLiveMaterialsRemoveAllZones("Radiant started");
        PlayCreateUi();
        rp::Log("play: v1.9.8 Native Solid Live Edit ready; v1.9.7 persistent registry semantics, verified main-frame IPC, typed sv_running/mapname state, resident runtime, and green native outlines are preserved; protocol-10 carries outward-wound exact convex face triangles to BO3's guarded renderer-context solid path; patches remain diagnostic wire/AABB only; BeginPlay/EndPlay remain fail-closed pending verified phase contracts (F8 remains Radiant Built Lighting)");
    }

    void PlayShutdown()
    {
        PlayVideoConfigRestore(g_playVideoGameRoot, "plugin shutdown");
        // Radiant owns the Play Test session: never leave a warm BO3 behind at editor exit. PlayStop normally terminates the tracked process,
        // the PID fallback covers adopted/warm sessions whose handle was already closed or replaced.
        DWORD shutdownPid = 0;
        HANDLE shutdownGame = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            shutdownPid = g_playGamePid;
            shutdownGame = g_playGameProcess;
        }
        if (g_playParked && g_playParkMuted && PlaySetProcessMuted(shutdownPid, false) > 0) rp::ConfigSet("playtest", "park_muted", "0");   // no mute left for the app's next run
        PlayStop(false);
        // PlayStop closes the kill-on-close job: close it again in case shutdown comes through an unusual host lifecycle path (idempotent).
        PlayCloseGameJob();
        if (g_playThread.joinable()) g_playThread.join();
        if (g_playLiveMatThread.joinable()) g_playLiveMatThread.join();   // a linker run in flight: wait for it (never joinable at exit)
        g_playHangThreadStop = true;
        if (g_playHangThread.joinable()) g_playHangThread.join();
        if (shutdownGame && WaitForSingleObject(shutdownGame, 0) == WAIT_TIMEOUT)
        {
            TerminateProcess(shutdownGame, 0);
            WaitForSingleObject(shutdownGame, 2000);
        }
        else if (shutdownPid)
        {
            HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, shutdownPid);
            if (h)
            {
                if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT)
                {
                    TerminateProcess(h, 0);
                    WaitForSingleObject(h, 2000);
                }
                CloseHandle(h);
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            if (g_playGameProcess) { CloseHandle(g_playGameProcess); g_playGameProcess = nullptr; }
            g_playGamePid = 0;
            g_playGameOwnedByRadiant = false;
        }
        g_playGamePresented = false;
        g_playPrewarmed = false;
        g_playPrewarmWindowHidden = false;
        PlayReleaseGameControls(false);
        PlayDetachEmbeddedGame();
        PlayEnsureCursorVisible(true);
        PlayLogRemoveCopyKeys();
        PlayLiveMaterialsRemoveAllZones("Radiant closing");
        PlayCloseLiveShared();
        // Qt widgets, menu callbacks and panes belong to the host and are destroyed on unload.
        g_playPane = nullptr;
        g_playGamePane = nullptr;
        g_playRoot = g_playPrimaryButton = g_playCheckButton = g_playFullCheck = nullptr;
        g_playFromCameraCheck = g_playFlyCheck = g_playDisableAiCheck = g_playGodCheck = nullptr;
        g_playWarmCheck = g_playPrewarmCheck = g_playEmbedCheck = g_playNavCheck = g_playAutoRestartCheck = g_playInstantStartCheck = g_playLiveCheck = nullptr;
        g_playCloseButton = g_playDvarsButton = nullptr;
        g_playDvarDlg = g_playDvarDev = g_playDvarSplit = g_playDvarLogfile = g_playDvarCustom = nullptr;
        g_playEngineLoadButton = g_playEngineReloadButton = g_playEngineFastRestartButton = nullptr;
        g_playStatusLabel = g_playPreflightLabel = g_playLogEdit = g_playEnhancedLabel = g_playVerboseCheck = nullptr;
        g_playEnhancedState = -1;
        g_playGameRoot = g_playGameView = g_playGameStatus = nullptr;
        g_playQtWinId = nullptr;
        g_playUiThreadId = 0;
        g_playPaneRectCache = RECT{};
        g_playLiveReleaseSnapshotPending = false;
        g_playLiveMouseWasDown = false;
        g_playLiveDragArmed = false;
    }


    int OnLoad(void)
    {
        PlayInit();
        PlayCheckDefenderLater();
        return 1;
    }

    void OnUnload(void)
    {
        PlayShutdown();
        PlayGdtSqlClose();
    }

    void OnMapLoaded(const wchar_t*)
    {
        PlayGdtSqlClose();   // material properties are read again (a material edited in APE meanwhile)
        g_playPrefabScopeText.clear(); g_playPrefabScopeHash.clear();   // Radiant re-read every prefab file
        g_playScopeDepth=0; g_playScopeKey.clear(); g_playScopeDirty=false;
        PlayClearLiveRegistry("Radiant map loaded or reloaded");
    }
    void OnBeforeScopeChange(void)
    {
        // leaving a prefab swaps the entity list back: capture its content now while the list is still the prefab's
        if (g_playScopeDepth>0 && g_playScopeDirty) { g_playScopeDirty=false; PlayScopeCapture("leaving the prefab"); }
    }
    void OnScopeChanged(const wchar_t* path, int depth)
    {
        g_playScopeDepth=depth;
        g_playScopeKey=depth>0 && path ? PlayPrefabKeyNorm(PlayWideToUtf8(path)) : std::string();
        if (depth>0) { g_playScopeDirty=true; g_playScopeDirtyAt=GetTickCount64(); }
        char line[400]={};
        snprintf(line,sizeof line,"[LIVE PREFAB] scope depth %d%s%s",depth,depth>0 ? ": editing " : "",depth>0 ? g_playScopeKey.c_str() : "");
        PlayAppendLog(line);
    }

    void OnTick(void)
    {
        PlayTick();
    }

    // ---- JOIN HOST: play the Team Create host's game together ----
    // While joined, what only the game's server can do stays with the host: its Play Test spawns the live models, cuts and swaps the nav mesh, runs Disable AI, God, Fly and the spawn point.
    // The joiner's bridge keeps what each game draws and collides with by itself (Live Edit brushes and patches, hides, decals, lights, fx, sky, lighting).
    struct PlayJoinSaved { bool valid = false, navRefresh = true, navMesh = true, models = true, disableAi = false, god = false, fly = false, fromCamera = true; };
    PlayJoinSaved g_playJoinSaved;
    void PlayJoinEnter()
    {
        if (!g_playJoinSaved.valid)
            g_playJoinSaved = { true, g_playLiveNavRefresh, g_playLiveNavMesh, g_playLiveModels, g_playDisableAi, g_playGod, g_playFly, g_playFromCamera };
        g_playLiveNavRefresh = g_playLiveNavMesh = g_playLiveModels = false;
        g_playDisableAi = g_playGod = g_playFly = g_playFromCamera = false;
        g_playJoinMode = true;
    }
    void PlayJoinLeave()
    {
        if (g_playJoinSaved.valid)
        {
            g_playLiveNavRefresh = g_playJoinSaved.navRefresh; g_playLiveNavMesh = g_playJoinSaved.navMesh; g_playLiveModels = g_playJoinSaved.models;
            g_playDisableAi = g_playJoinSaved.disableAi; g_playGod = g_playJoinSaved.god; g_playFly = g_playJoinSaved.fly; g_playFromCamera = g_playJoinSaved.fromCamera;
            g_playJoinSaved.valid = false;
        }
        g_playJoinMode = false;
    }
    // The host's build arrives through Team Create's zone sync ("TeamCreate zonesync fetch <map>"), the UI tick polls it
    // and launches once it is complete (PlayJoinFetchTick, called from PlayRefreshUi).
    bool g_playJoinFetching = false;
    PlayMapInfo g_playJoinMap;
    ULONGLONG g_playJoinPollAt = 0, g_playJoinProgressAt = 0;
    std::string PlayTeamCreate(const char* cmd)
    {
        const char* r = rp::host && rp::host->Command ? rp::host->Command(rp::self, "TeamCreate", cmd) : nullptr;
        return r ? r : "";
    }
    // Once a second (or now): Team Create's Hide box and the addresses to hide (the Join Host address, the Team Create
    // host's). Ticking the box also scrubs the lines already in the log.
    void PlayHideIpTick(bool now)
    {
        static ULONGLONG last = 0;
        const ULONGLONG t = GetTickCount64();
        if (!now && t - last < 1000) return;
        last = t;
        const bool hide = PlayTeamCreate("hideip").rfind("hideip=1", 0) == 0;
        std::vector<std::string> ips;
        if (!g_playJoinHost.empty()) ips.push_back(g_playJoinHost);
        const std::string tcHost = PlayTeamCreate("hostaddr");
        if (!tcHost.empty() && tcHost != "host" && tcHost != g_playJoinHost) ips.push_back(tcHost);
        std::lock_guard<std::mutex> lock(g_playMutex);
        const bool changed = hide != g_playHideIp || ips != g_playHiddenIps;
        g_playHideIp = hide; g_playHiddenIps = ips;
        if (changed && hide)
        {
            g_playLogText = PlayMaskIpsLocked(g_playLogText);
            g_playSimpleText = PlayMaskIpsLocked(g_playSimpleText);
            for (auto& l : g_playPendingLog) l = PlayMaskIpsLocked(l);
            g_playLastUiLogSize = (size_t)-1;   // redraw the log
        }
    }

    // Once a second: is the Team Create host's Play Test game up on this map? Then Play reads Join Host. A Team Create
    // without the "hostplaying" command (older build) or no session answers something else: plain Play.
    void PlayHostPlayingTick()
    {
        static ULONGLONG last = 0;
        const ULONGLONG t = GetTickCount64();
        if (t - last < 1000) return;
        last = t;
        const std::string r = PlayTeamCreate("hostplaying");
        const bool playing = r == "1" || r.rfind("1 ", 0) == 0;
        if (playing != g_playHostPlaying)
            PlayAppendLog(playing ? "[JOIN] The Team Create host is playing this map: Play now joins the host's game (Join Host)."
                                  : "[JOIN] The Team Create host's game has closed: Play plays here again.");
        g_playHostPlaying = playing;
    }

    bool PlayJoinBeginFetch(const PlayMapInfo& m)
    {
        const std::string map = PlayWideToUtf8(m.mapName);
        const std::string r = PlayTeamCreate(("zonesync fetch " + map).c_str());
        if (r.rfind("ASKING", 0) != 0 && r.rfind("FETCHING", 0) != 0)   // FETCHING: a background copy of this build is already under way
        {
            PlaySetError("[JOIN] Team Create could not ask the host for its build: " + (r.empty() ? std::string("Team Create is not loaded or too old (update it on both PCs)") : r));
            g_playThreadDone = true;
            return false;
        }
        g_playJoinMap = m;
        g_playJoinFetching = true;
        g_playJoinPollAt = g_playJoinProgressAt = GetTickCount64();
        g_playState = PLAY_PREPARING;
        g_playThreadDone = true;   // no worker until the files are here
        PlayAppendLog("[JOIN] Asking the host for its build of " + map + "...");
        PlayNote("Getting the host's build of " + map + " through Team Create (only files that changed since the last join)...");
        PlayWaitBegin("join", "getting the host's build");
        return true;
    }
    void PlayJoinLaunchWorker(PlayMapInfo m)
    {
        g_playState = PLAY_LAUNCHING;
        HANDLE old = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            old = g_playGameProcess;
            g_playGameProcess = nullptr; g_playGamePid = 0; g_playGameOwnedByRadiant = false;
        }
        PlayDetachEmbeddedGame();
        if (old)
        {
            if (WaitForSingleObject(old, 0) == WAIT_TIMEOUT)
            {
                PlayAppendLog("[JOIN] Closing the running BO3: the joining game starts at its menu.");
                TerminateProcess(old, 0);
                WaitForSingleObject(old, 10000);
            }
            CloseHandle(old);
        }
        PlayLaunchGame(m);
        g_playThreadDone = true;
    }
    void PlayJoinFetchTick()
    {
        if (!g_playJoinFetching) return;
        const ULONGLONG now = GetTickCount64();
        if (g_playCancel)
        {
            g_playJoinFetching = false;
            PlayTeamCreate("zonesync cancel");
            PlayWaitEnd("join");
            PlayAppendLog("[JOIN] Cancelled while getting the host's build.");
            PlayNote("Cancelled while getting the host's build.");
            return;
        }
        if (now - g_playJoinPollAt < 500) return;
        g_playJoinPollAt = now;
        const std::string r = PlayTeamCreate("zonesync status");
        if (r.rfind("FETCHING", 0) == 0 || r.rfind("ASKING", 0) == 0)
        {
            if (now - g_playJoinProgressAt >= 5000) { g_playJoinProgressAt = now; PlayAppendLog("[JOIN] " + r); }
            return;
        }
        g_playJoinFetching = false;
        if (r.rfind("DONE", 0) == 0)
        {
            PlayAppendLog("[JOIN] The host's build is here: " + r.substr(5));
            PlayNote("The host's build is here (" + PlayDuration(PlayWaitEnd("join")) + ").");
            if (g_playThread.joinable()) g_playThread.join();
            g_playThreadDone = false;
            PlayMapInfo m = g_playJoinMap;
            g_playThread = std::thread([m]() { PlayJoinLaunchWorker(m); });
            return;
        }
        PlayWaitEnd("join");
        PlaySetError("[JOIN] Could not get the host's build: " + (r.size() > 7 && r.rfind("FAILED", 0) == 0 ? r.substr(7) : r));
    }

    // host: an IPv4 or host name, empty = the Team Create host this Radiant is connected to
    bool PlayJoinStart(std::string host)
    {
        while (!host.empty() && host.back() == ' ') host.pop_back();
        if (host.empty() && rp::host && rp::host->Command)
        {
            const char* r = rp::host->Command(rp::self, "TeamCreate", "hostaddr");
            host = r ? r : "";
        }
        if (host == "host") { PlayAppendLog("[JOIN] You are the Team Create host: press Play, and your peers' Play button turns into Join Host."); PlayShowPane(); return false; }
        if (host.empty()) { PlayAppendLog("[JOIN] Not connected to a Team Create host. Join the session first (Ctrl+Shift+T), or use the command \"PlayTest join <host IP>\"."); PlayShowPane(); return false; }
        for (char ch : host)
            if (!(isalnum((unsigned char)ch) || ch == '.' || ch == '-'))
            { PlayAppendLog("[JOIN] '" + host + "' is not an IP address or host name."); PlayShowPane(); return false; }
        g_playJoinHost = host;
        PlayHideIpTick(true);   // before the first line that names it
        const bool ok = PlayStart(false, true);
        if (!ok) PlayJoinLeave();
        return ok;
    }

    const char* OnCommand(const char* cmd)
    {
        static std::string reply;
        std::string c = cmd;
        if (c.rfind("play ", 0) == 0) c = c.substr(5);   // "play status" and "status" both work
        char buf[4096];
        if (c == "play" || c == "smart" || c == "fast")
        {
            PlayStart(false); reply = PlayStatusText(); return reply.c_str();
        }
        if (c == "full")
        {
            PlayStart(true); reply = PlayStatusText(); return reply.c_str();
        }
        if (c == "join" || c.rfind("join ", 0) == 0)   // join [host]: Join Host (the Team Create host when no address is given)
        {
            if (PlayBusy()) { reply = "busy: " + PlayStatusText(); return reply.c_str(); }
            PlayJoinStart(c.size() > 5 ? c.substr(5) : "");
            reply = PlayStatusText(); return reply.c_str();
        }
        if (c == "start" || c == "from start")
        {
            g_playFromStartOnce = true; PlayStart(false); reply = PlayStatusText(); return reply.c_str();
        }
        if (c == "stop" || c == "close")
        {
            PlayStop(); reply = PlayStatusText(); return reply.c_str();
        }
        if (c == "closebo3")   // the pane's End Session / Close BO3 button: parks a running game, closes a parked one
        {
            PlayCloseClicked(nullptr); reply = g_playParked ? "parked" : PlayStatusText(); return reply.c_str();
        }
        if (c == "park")   // what End Session does: hide and mute the running game with its level loaded
        {
            reply = PlayParkGame() ? "parked" : (g_playParked ? "already parked" : "nothing to park (no game, a joined game, or a build running)"); return reply.c_str();
        }
        if (c == "status")
        {
            reply = PlayStatusText(); return reply.c_str();
        }
        if (c == "camera") { PlayCaptureCamera(true); reply = g_playCamDvar.empty() ? "no camera" : g_playCamDvar; return reply.c_str(); }   // what Play from camera would send, in world space
        if (c == "enhancedbox") { PlayShowEnhancedRequiredBox(); reply = "shown"; return reply.c_str(); }   // the "Live Edit needs BO3 Enhanced" box, for a look without a regular BO3
        if (c == "hosting")   // Team Create's host asks once a second: "1" while this Radiant's own game is up (not a joined one)
        {
            reply = (g_playGamePid && g_playGamePresented && !g_playParked && !g_playJoinMode) ? "1" : "0"; return reply.c_str();
        }
        if (c == "livepane")   // what Play does with a closed BO3 Live pane: open it over the Camera pane
        {
            const int r = PlayOpenGamePane();
            reply = r == 1 ? "moved over the Camera pane" : r == 2 ? "raised where it was" : r == 0 ? "shown (no Camera pane to join)" : "already on screen";
            return reply.c_str();
        }
        if (c == "ui" || c == "console")
        {
            PlayShowPane(); reply = PlayStatusText(); return reply.c_str();
        }
        if (c == "check")
        {
            PlayCheckMapOnly(); reply = g_playPreflightSummary; return reply.c_str();
        }
        if (c == "control" || c == "input")
        {
            PlayCaptureGameControls(); reply = PlayStatusText(); return reply.c_str();
        }
        if (c == "release")
        {
            PlayReleaseGameControls(); reply = PlayStatusText(); return reply.c_str();
        }
        if (c == "log" || c == "log verbose")
        {
            std::lock_guard<std::mutex> lock(g_playMutex); reply = g_playLogText.empty() ? "(play log empty)" : g_playLogText; return reply.c_str();
        }
        if (c == "log simple")
        {
            std::lock_guard<std::mutex> lock(g_playMutex); reply = g_playSimpleText.empty() ? "(play log empty)" : g_playSimpleText; return reply.c_str();
        }
        if (c == "live publish")
        {
            // publish the settled selection now (an edit made by a plugin or a script has no mouse release)
            PlayPublishSettledSelection(GetTickCount64(),false);
            reply = "Live Edit: the settled selection was published.";
            return reply.c_str();
        }
        if (c == "live publish peer")
        {
            // Team Create: a peer's edit arrives as an in-place import that is selected for a moment. It is published like the follow-up after a delete:
            // brushes back at their compiled hull drop their live twin instead of getting one. No game, no Live Edit: nothing to do.
            if (!g_playLivePreview || !PlayLiveEditBuild() || !g_playLiveShared || !g_playLiveBridgeInjected) { reply = "Live Edit: no running game"; return reply.c_str(); }
            PlayPublishSettledSelection(GetTickCount64(),true);
            reply = "Live Edit: the peer's import was published.";
            return reply.c_str();
        }
        if (c == "live bake")   // export Radiant's built lighting, link, swap it into the running game
        {
            std::string detail;
            reply = PlayLiveBakeStart(detail) ? "Live bake started: " + detail : "Live bake not started: " + detail;
            return reply.c_str();
        }
        if (c == "live clear")
        {
            PlayClearLiveRegistry("developer CLEAR_ALL request");
            reply = "Live Edit registry cleared.";
            return reply.c_str();
        }
        if (c == "live status")
        {
            unsigned hideActive = 0, prefabsHidden = 0, modelsHidden = 0;
            for (const auto& pair : g_playLiveHides) if (pair.second.active) ++hideActive;
            for (const auto& pair : g_playBakedPrefabs) if (pair.second.hidden) ++prefabsHidden;
            for (const auto& pair : g_playBakedModels) if (pair.second.hidden) ++modelsHidden;
            snprintf(buf, sizeof buf,
                "liveObjectCount=%u liveBrushCount=%ld livePatchCount=%ld createCount=%ld updateCount=%ld deleteCount=%ld staleObjectCount=%ld registryGeneration=%ld"
                " | live delete: hook=%d bakedHulls=%u prefabs=%u hiddenPrefabs=%u records=%u active=%u generation=%ld bridge: state=%ld triangles=%ld unmatched=%ld brushes=%ld %s"
                " | hidden models: cached=%u hidden=%u bridge: state=%ld instances=%ld unmatched=%ld collision=%ld %s"
                " | live models: enabled=%d wanted=%u sent=%u generation=%ld names=%u"
                " | live collision: state=%ld brushes=%ld appended=%ld %s"
                " | live nav mesh: enabled=%d state=%ld obstacles=%ld generators=%ld %s"
                " | live occlusion: enabled=%d state=%ld surfaces=%ld models=%ld %s"
                " | nav refresh: auto=%d phase=%d running=%d swaps=%d failures=%d swapSeq=%ld doneSeq=%ld result=%ld %s",
                (unsigned)g_playLiveRegistry.size(),
                g_playLiveShared ? (long)g_playLiveShared->liveBrushCount : 0L,
                g_playLiveShared ? (long)g_playLiveShared->livePatchCount : 0L,
                (long)g_playLiveCreateCount, (long)g_playLiveUpdateCount, (long)g_playLiveDeleteCount,
                g_playLiveShared ? (long)g_playLiveShared->staleObjectCount : 0L,
                (long)g_playLiveRegistryGeneration,
                g_playSelectDeleteHooked ? 1 : 0, (unsigned)g_playBakedHulls.size(), (unsigned)g_playBakedPrefabs.size(), prefabsHidden, (unsigned)g_playLiveHides.size(), hideActive, (long)g_playLiveHideGeneration,
                g_playLiveShared ? (long)g_playLiveShared->hideState : 0L,
                g_playLiveShared ? (long)g_playLiveShared->hideTriangles : 0L,
                g_playLiveShared ? (long)g_playLiveShared->hideUnmatched : 0L,
                g_playLiveShared ? (long)g_playLiveShared->hideBrushes : 0L,
                g_playLiveShared ? g_playLiveShared->hideStatus : "",
                (unsigned)g_playBakedModels.size(), modelsHidden,
                g_playLiveShared ? (long)g_playLiveShared->modelHideState : 0L,
                g_playLiveShared ? (long)g_playLiveShared->modelHideMatched : 0L,
                g_playLiveShared ? (long)g_playLiveShared->modelHideUnmatched : 0L,
                g_playLiveShared ? (long)g_playLiveShared->modelHideCollision : 0L,
                g_playLiveShared ? g_playLiveShared->modelHideStatus : "",
                g_playLiveModels ? 1 : 0, (unsigned)g_playLiveModelWanted.size(), (unsigned)g_playLiveModelSentCount, (long)g_playLiveModelGen, (unsigned)g_playLiveModelSlotNames.size(),
                g_playLiveShared ? (long)g_playLiveShared->liveCollisionState : 0L,
                g_playLiveShared ? (long)g_playLiveShared->liveCollisionBrushes : 0L,
                g_playLiveShared ? (long)g_playLiveShared->liveCollisionAppended : 0L,
                g_playLiveShared ? g_playLiveShared->liveCollisionStatus : "",
                g_playLiveNavMesh ? 1 : 0,
                g_playLiveShared ? (long)g_playLiveShared->liveNavState : 0L,
                g_playLiveShared ? (long)g_playLiveShared->liveNavObstacles : 0L,
                g_playLiveShared ? (long)g_playLiveShared->liveNavGenerators : 0L,
                g_playLiveShared ? g_playLiveShared->liveNavStatus : "",
                g_playLiveOcclusion ? 1 : 0,
                g_playLiveShared ? (long)g_playLiveShared->liveOccState : 0L,
                g_playLiveShared ? (long)g_playLiveShared->liveOccSurfaces : 0L,
                g_playLiveShared ? (long)g_playLiveShared->liveOccModels : 0L,
                g_playLiveShared ? g_playLiveShared->liveOccStatus : "",
                g_playLiveNavRefresh ? 1 : 0, g_playNavPhase, g_playNavRunning ? 1 : 0, g_playNavCount, g_playNavFailures,
                g_playLiveShared ? (long)g_playLiveShared->navSwapSeq : 0L,
                g_playLiveShared ? (long)g_playLiveShared->navSwapDoneSeq : 0L,
                g_playLiveShared ? (long)g_playLiveShared->navSwapResult : 0L,
                g_playLiveShared ? g_playLiveShared->navSwapText : "");
            {
                unsigned decalsHidden = 0, decalsNew = 0;
                for (const auto& pair : g_playBakedDecals) { if (pair.second.hidden) ++decalsHidden; if (pair.second.isNew) ++decalsNew; }
                const size_t used = strlen(buf);
                snprintf(buf + used, sizeof buf - used,
                    " | live decals: enabled=%d cached=%u new=%u hidden=%u generation=%ld bridge: state=%ld hidden=%ld unmatched=%ld drawn=%ld missing=%ld %s",
                    g_playLiveDecals ? 1 : 0, (unsigned)g_playBakedDecals.size(), decalsNew, decalsHidden, (long)g_playDecalGeneration,
                    g_playLiveShared ? (long)g_playLiveShared->volDecalState : 0L,
                    g_playLiveShared ? (long)g_playLiveShared->volDecalHidden : 0L,
                    g_playLiveShared ? (long)g_playLiveShared->volDecalUnmatched : 0L,
                    g_playLiveShared ? (long)g_playLiveShared->volDecalDrawn : 0L,
                    g_playLiveShared ? (long)g_playLiveShared->volDecalMissing : 0L,
                    g_playLiveShared ? g_playLiveShared->volDecalStatus : "");
            }
            {
                const size_t used = strlen(buf);
                snprintf(buf + used, sizeof buf - used,
                    " | live lights: enabled=%d compiled=%u seen=%u changed=%u new=%u deleted=%u generation=%ld bridge: state=%ld applied=%ld rewritten=%ld added=%ld unmatched=%ld %s",
                    g_playLiveLights ? 1 : 0, (unsigned)g_playBakedLights.size(), (unsigned)g_playLightsSeen, (unsigned)g_playLightsChanged, (unsigned)g_playLightsNew, (unsigned)g_playLightsRemoved, (long)g_playLightGeneration,
                    g_playLiveShared ? (long)g_playLiveShared->liveLightState : 0L,
                    g_playLiveShared ? (long)g_playLiveShared->liveLightAppliedGeneration : 0L,
                    g_playLiveShared ? (long)g_playLiveShared->liveLightOverridden : 0L,
                    g_playLiveShared ? (long)g_playLiveShared->liveLightAdded : 0L,
                    g_playLiveShared ? (long)g_playLiveShared->liveLightUnmatched : 0L,
                    g_playLiveShared ? g_playLiveShared->liveLightStatus : "");
            }
            reply = buf;
            {
                unsigned fxNew = 0; for (const auto& pair : g_playBakedFx) if (pair.second.isNew) ++fxNew;
                char fx[640];
                snprintf(fx, sizeof fx, " | live fx: enabled=%d cached=%u new=%u changed=%u placed=%u deleted=%u generation=%ld bridge: state=%ld applied=%ld playing=%ld stopped=%ld missing=%ld %s",
                    g_playLiveFx ? 1 : 0, (unsigned)g_playBakedFx.size(), fxNew, (unsigned)g_playFxChanged, (unsigned)g_playFxNew, (unsigned)g_playFxRemoved, (long)g_playFxGeneration,
                    g_playLiveShared ? (long)g_playLiveShared->liveFxState : 0L,
                    g_playLiveShared ? (long)g_playLiveShared->liveFxAppliedGeneration : 0L,
                    g_playLiveShared ? (long)g_playLiveShared->liveFxPlaying : 0L,
                    g_playLiveShared ? (long)g_playLiveShared->liveFxStopped : 0L,
                    g_playLiveShared ? (long)g_playLiveShared->liveFxMissing : 0L,
                    g_playLiveShared ? g_playLiveShared->liveFxStatus : "");
                reply += fx;
                char sky[640];
                snprintf(sky, sizeof sky, " | live sky: enabled=%d bake=%d%s suns=%u generation=%ld asked: %s; bridge: state=%ld applied=%ld rewritten=%ld sun=%ld drawn=%s %s",
                    g_playLiveSky ? 1 : 0, g_playLiveSkyBake ? 1 : 0, g_playSkyBakeDueMs ? (g_playSkyBakeWait.empty() ? " (pending)" : " (waiting)") : "",
                    (unsigned)g_playBakedSuns.size(), (long)g_playSkyGeneration, g_playSkyLine.empty() ? "-" : g_playSkyLine.c_str(),
                    g_playLiveShared ? (long)g_playLiveShared->liveSkyState : 0L,
                    g_playLiveShared ? (long)g_playLiveShared->liveSkyAppliedGeneration : 0L,
                    g_playLiveShared ? (long)g_playLiveShared->liveSkyRewritten : 0L,
                    g_playLiveShared ? (long)g_playLiveShared->liveSkySunRewritten : 0L,
                    g_playLiveShared && g_playLiveShared->liveSkyDrawn[0] ? g_playLiveShared->liveSkyDrawn : "-",
                    g_playLiveShared ? g_playLiveShared->liveSkyStatus : "");
                reply += sky;
            }
            return reply.c_str();
        }
        if (c == "nav")
        {
            PlayNavRefreshSchedule("command", 0);
            reply = "nav mesh refresh scheduled (save, compile, nav zone, swap); watch the play log for [NAV] lines";
            return reply.c_str();
        }
        if (c.rfind("live enter ", 0) == 0)   // live enter <entityGuid>: select that misc_prefab and Enter Prefab (test loops, no keyboard)
        {
            const std::string guid = c.substr(11);
            if (!rp::Has(&rp::host->SelectEntityByGuid) || !rp::host->SelectEntityByGuid || !rp::Has(&rp::host->EnterSelectedPrefab) || !rp::host->EnterSelectedPrefab) { reply = "host too old"; return reply.c_str(); }
            if (!rp::host->SelectEntityByGuid(rp::self, guid.c_str())) { reply = "entity not found: " + guid; return reply.c_str(); }
            reply = rp::host->EnterSelectedPrefab(rp::self) ? "entered, depth=" + std::to_string(PlayPrefabDepth()) : "Enter Prefab refused";
            return reply.c_str();
        }
        if (c == "live reconcile")   // run the hide / prefab / model / fill reconcile now (test loops, the tick needs a running game)
        {
            PlayReconcileLiveHides(GetTickCount64(), true);
            size_t hides=0; for (const auto& h:g_playLiveHides) if (h.second.active) ++hides;
            size_t prefabsHidden=0; for (const auto& b:g_playBakedPrefabs) if (b.second.hidden) ++prefabsHidden;
            size_t fills=0; for (const auto& f:g_playLiveFills) if (f.second!="-") ++fills;
            snprintf(buf, sizeof buf, "reconciled: %u brush hide(s), %u prefab instance(s) hidden, %u hole fill(s), %u registry object(s)", (unsigned)hides, (unsigned)prefabsHidden, (unsigned)fills, (unsigned)g_playLiveRegistry.size());
            reply = buf; return reply.c_str();
        }
        if (c == "live leave")
        {
            if (!rp::Has(&rp::host->LeavePrefab) || !rp::host->LeavePrefab) { reply = "host too old"; return reply.c_str(); }
            reply = rp::host->LeavePrefab(rp::self) ? "left, depth=" + std::to_string(PlayPrefabDepth()) : "Leave Prefab refused";
            return reply.c_str();
        }
        if (c == "live probefile")
        {
            // dev: builds the probe layout file from the map's linked fastfile and .led as a live bake would (no game needed)
            PlayMapInfo m; std::string error, path, how;
            if (!PlayResolveCurrentMap(m, error)) { reply = error; return reply.c_str(); }
            const bool ok = PlayBuildProbeFile(m, PlayJoin(PlayJoin(m.usermapRoot, L"zone"), m.mapName + L".ff"), path, how);
            reply = std::string(ok ? "ok: " : "failed: ") + how + (ok ? " -> " + path : std::string());
            return reply.c_str();
        }
        if (c == "live hulls")
        {
            PlayLoadBakedHulls("live hulls command");
            snprintf(buf, sizeof buf, "baked hulls reloaded from the saved map: %u brush(es)", (unsigned)g_playBakedHulls.size());
            reply = buf;
            return reply.c_str();
        }
        if (c == "live unhide")
        {
            PlayClearLiveHides("live unhide command");
            reply = "Hidden brush records dropped; the game shows the baked geometry again.";
            return reply.c_str();
        }
        return nullptr;
    }
}

extern "C" MODTOOLS_PLUGIN_EXPORT const ModToolsPluginApi* ModToolsPluginEntry(ModToolsPlugin* self, const ModToolsHostApi* host)
{
    rp::Bind(self, host);
    static ModToolsPluginApi api = {};
    api.size = sizeof api;
    api.apiVersion = MODTOOLS_PLUGIN_API_VERSION;
    api.name = "PlayTest";
    api.displayName = "Play Test";
    api.version = "1.0.0";
    api.author = "Figglebottom and SaintVertigo";
    api.description = "Play test your maps within Radiant, see edits live while playing, and join a Team Create host's game.";
    api.url = "https://github.com/Aidan601/MTPlugins/tree/main/plugins/PlayTest";
    api.OnLoad = OnLoad;
    api.OnUnload = OnUnload;
    api.OnTick = OnTick;
    api.OnMapLoaded = OnMapLoaded;
    api.OnBeforeScopeChange = OnBeforeScopeChange;
    api.OnScopeChanged = OnScopeChanged;
    api.OnCommand = OnCommand;
    api.apps = MODTOOLS_APP_RADIANT;
    return &api;
}
