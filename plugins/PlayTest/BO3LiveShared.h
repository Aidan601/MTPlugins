#pragma once
#include <windows.h>
#include <cstdint>

// Shared memory protocol between the PlayTest plugin and BO3LiveBridge.dll (injected into the test BlackOps3.exe).
// POD only: both sides map the same bytes.
// Window property the bridge sets once its dock subclass is installed.
#define BRLIVE_DOCK_PROP L"BRLiveDock"
// Cursor state wanted by the docked game (set by the bridge's user32 hooks, applied by Radiant's tick):
// 1 = hidden (play), 2 = clipped to the window.
#define BRLIVE_CURSOR_PROP L"BRLiveCursor"
#define BRLIVE_CURSOR_HIDDEN 1
#define BRLIVE_CURSOR_CLIPPED 2
// Input state: 4 = armed (the game holds the input), 8 = needs a click (input lost to another app).
// 16 = the bridge publishes 4/8 and the two GetTickCount stamps below (cursor last shown / hidden).
#define BRLIVE_CURSOR_ARMED 4
#define BRLIVE_CURSOR_NEEDCLICK 8
#define BRLIVE_CURSOR_V2 16
#define BRLIVE_SHOWN_AT_PROP L"BRLiveShownAt"
#define BRLIVE_HIDDEN_AT_PROP L"BRLiveHiddenAt"
// Diagnostic: ms of the last visible cursor span that ended hidden while armed.
#define BRLIVE_PULSE_PROP L"BRLivePulseMs"
// Set by Radiant (1) while the game has focus and Radiant is foreground: the bridge passes WM_INPUT to the game only then.
#define BRLIVE_INPUT_PROP L"BRLiveInput"

namespace brlive
{
    static constexpr uint32_t kMagic = 0x394C5242u; // "BRL9"
    static constexpr uint32_t kVersion = 40u;  // bump on every layout change
    static constexpr int kMaxVertices = 96;
    static constexpr int kMaxEdges = 256;
    // A closed convex polyhedron with V vertices needs 2V-4 face triangles, plus spare room for patches.
    static constexpr int kMaxTriangles = 256;
    static constexpr int kMaxLiveObjects = 512;  // moved prefabs publish their interiors
    // Textured Live Edit (BO3 Enhanced only): every brush face carries its material and one uv per corner.
    static constexpr int kMaxFaces = 64;          // per object
    static constexpr int kMaxFaceVerts = 16;      // corners per face (more: the face keeps the colored fill)
    static constexpr int kMaxMaterials = 96;      // distinct material names per registry publish
    static constexpr int kMaterialNameLen = 64;
    // Hidden baked brushes (BO3 Enhanced only): a deleted brush the compiled BSP still contains is sent as its convex hull
    // (outward planes, n.x <= d inside, plus bounds). The bridge blanks the world triangles on it and zeroes the collision brush contents.
    static constexpr int kMaxHideRecords = 1024;  // prefab interiors need many
    static constexpr int kMaxHidePlanes = 32;

    // The zone module (bridge/zone.cpp): patch zones through the patchable-map table and the console mailbox BO3LiveZone.cmd.
    // Radiant arms it once the game is past its startup.
    enum ZoneState : LONG
    {
        ZONE_OFF = 0,        // not armed yet
        ZONE_STARTING = 1,   // armed, checking the build
        ZONE_READY = 2,      // mailbox polled, table slot kept in step with the patch fastfiles
        ZONE_NO_BUILD = 3,   // this BlackOps3.exe has no zone table entry: nothing done
        ZONE_NO_CBUF = 4,    // build known but Cbuf_InsertText did not verify: table slot only, no mailbox
    };

    enum BridgeStatus : LONG
    {
        STATUS_OFFLINE = 0,
        STATUS_STARTING = 1,
        STATUS_READY = 2,
        STATUS_ACTIVE = 3,
        STATUS_UNSUPPORTED_BUILD = 4,
        STATUS_PROJECTION_FAULT = 5,
    };

    // Engine runtime states: "runtime injected" is kept apart from "real BO3 world state resolved", so MAP_ACTIVE is never faked
    // from a D3D matrix or a visible window.
    enum EngineRuntimeState : LONG
    {
        ENGINE_OFFLINE = 0,
        ENGINE_BOOTING = 1,
        ENGINE_DISCOVERY = 2,
        ENGINE_SYMBOLS_UNRESOLVED = 3,
        ENGINE_FRONTEND = 4,
        ENGINE_LOADING = 5,
        ENGINE_MAP_ACTIVE = 6,
        ENGINE_PLAYING = 7,
        ENGINE_STOPPED = 8,
    };

    enum EngineCommand : LONG
    {
        ENGINE_CMD_NONE = 0,
        ENGINE_CMD_PING = 1,
        ENGINE_CMD_QUERY_STATE = 2,
        ENGINE_CMD_LOAD_MAP = 3,       // reserved until the BO3 command path is proven
        ENGINE_CMD_BEGIN_PLAY = 4,     // reserved until gameplay lifecycle is proven
        ENGINE_CMD_END_PLAY = 5,       // reserved until gameplay lifecycle is proven
        ENGINE_CMD_RESTART_PLAYER = 6, // reserved until player restart is proven
        ENGINE_CMD_RELOAD_MAP = 7,  // queues a verified map_restart through BO3 Cbuf_AddText
        ENGINE_CMD_CBUF_PROBE = 8,  // queues a harmless newline through the verified Cbuf_AddText
        ENGINE_CMD_FAST_RESTART = 9,  // queues BO3 fast_restart without unloading the map assets
    };

    enum EngineCommandResult : LONG
    {
        ENGINE_RESULT_NONE = 0,
        ENGINE_RESULT_OK = 1,
        ENGINE_RESULT_NOT_READY = 2,
        ENGINE_RESULT_UNRESOLVED = 3,
        ENGINE_RESULT_BAD_COMMAND = 4,
        ENGINE_RESULT_UNSUPPORTED_BUILD = 5,
        ENGINE_RESULT_BAD_ARGUMENT = 6,
        ENGINE_RESULT_WRONG_THREAD = 7,
    };

    // BO3 map ownership and gameplay-session ownership are separate. Typed sv_running/mapname stays authoritative for the map,
    // this state describes only the observed or owned play layer.
    enum PlaySessionState : LONG
    {
        PLAY_SESSION_UNKNOWN = 0,
        PLAY_SESSION_MAP_READY = 1,
        PLAY_SESSION_STARTING = 2,
        PLAY_SESSION_PLAYING = 3,
        PLAY_SESSION_STOPPING = 4,
        PLAY_SESSION_STOPPED = 5,
        PLAY_SESSION_ERROR = 6,
    };

    enum EngineReadyFlags : LONG
    {
        ENGINE_READY_COMPANION = 1 << 0,
        ENGINE_READY_WINDOW = 1 << 1,
        ENGINE_READY_PE_SCANNED = 1 << 2,
        ENGINE_READY_COMMAND_MAILBOX = 1 << 3,
        ENGINE_READY_GAMESTATE = 1 << 4,
        ENGINE_READY_COMMAND_EXEC = 1 << 5,
        ENGINE_READY_PLAY_LIFECYCLE = 1 << 6,
        ENGINE_READY_WORLD_RENDER = 1 << 7,
        ENGINE_READY_FRAME_PUMP = 1 << 8,
        ENGINE_READY_MAP_IDENTITY = 1 << 9,
        ENGINE_READY_SERVER_STATE = 1 << 10,
        ENGINE_READY_NATIVE_WORLD_DRAW = 1 << 11,
    };

    enum LiveRenderMode : LONG
    {
        LIVE_RENDER_OFF = 0,
        LIVE_RENDER_NATIVE_GEOMETRY = 1,
        LIVE_RENDER_NATIVE_AABB = LIVE_RENDER_NATIVE_GEOMETRY,  // alias
        LIVE_RENDER_D3D_FALLBACK = 2,
    };

    enum LiveGeometryType : LONG
    {
        LIVE_GEOMETRY_NONE = 0,
        LIVE_GEOMETRY_BRUSH = 1,
        LIVE_GEOMETRY_PATCH = 2,
    };

    enum LiveObjectOperation : LONG
    {
        LIVE_OBJECT_NONE = 0,
        LIVE_OBJECT_CREATE = 1,
        LIVE_OBJECT_UPDATE = 2,
        LIVE_OBJECT_DELETE = 3,
        LIVE_OBJECT_CLEAR_ALL = 4,
    };

    enum LiveObjectFlags : LONG
    {
        LIVE_OBJECT_ACTIVE = 1 << 0,
        LIVE_OBJECT_SELECTED = 1 << 1,
        LIVE_OBJECT_AABB_FALLBACK = 1 << 2,
        LIVE_OBJECT_RENDER = 1 << 3,
        LIVE_OBJECT_COLLISION_REQUESTED = 1 << 4,
        // The object draws nothing in the game (every face is noDraw, or the owner is a trigger_* / info_volume entity),
        // the selection outline still shows it.
        LIVE_OBJECT_NO_DRAW = 1 << 5,
        // collisionContents holds the game contents (0 = no collision),
        LIVE_OBJECT_CONTENTS = 1 << 6,
    };

    enum LiveFaceFlags : LONG
    {
        FACE_NO_DRAW = 1 << 0,  // the face's material draws nothing (material is -1)
    };

    struct Edge
    {
        uint16_t a;
        uint16_t b;
    };

    struct Triangle
    {
        uint16_t a;
        uint16_t b;
        uint16_t c;
    };
    static_assert(sizeof(Triangle)==6,"Live triangle transport must remain tightly packed.");

    // One textured brush face: idx[] index the object's world[] in winding order, material indexes SharedState::materialNames
    // (-1 = none), uv per corner. triFirst/triCount name its fan triangles so the colored fill can skip it.
    struct LiveFace
    {
        LONG material;
        LONG flags;  // LiveFaceFlags
        LONG vertCount;
        uint16_t triFirst;
        uint16_t triCount;
        uint16_t idx[kMaxFaceVerts];
        float uv[kMaxFaceVerts][2];
    };
    static_assert(kMaxTriangles>=2*kMaxVertices-4,
        "Triangle capacity must cover every closed convex object at vertex capacity.");

    // Flags for the hide records below.
    enum HideFlags : LONG
    {
        HIDE_RENDER = 1 << 0,      // blank the world triangles on the hull
        HIDE_COLLISION = 1 << 1,   // zero the matching clipmap brush's contents
    };

    struct HideRecord
    {
        char sourceGuid[40];
        LONG planeCount;
        LONG flags;                // HideFlags
        float mins[3];
        float maxs[3];
        float planes[kMaxHidePlanes][4];   // n.x, n.y, n.z, d (unit normal pointing out of the brush)
    };

    // Hidden baked patches: a compiled mesh block Radiant deleted or reshaped, sent as a control grid tile (at most 16 x 16 points,
    // a bigger patch is several records "<guid>#k"). The bridge blanks the world triangles on the surface and,
    // with HIDE_COLLISION, the clipmap partitions on it.
    static constexpr int kMaxPatchHides = 128;
    static constexpr int kMaxPatchHideSide = 16;
    static constexpr int kMaxPatchHidePoints = kMaxPatchHideSide * kMaxPatchHideSide;
    struct PatchHideRecord
    {
        char sourceGuid[48];
        LONG rows;
        LONG cols;
        LONG flags;                // HideFlags
        float mins[3];
        float maxs[3];
        float points[kMaxPatchHidePoints][3];   // row-major, rows * cols used
    };

    // Hidden baked static models: the compiled placement of a deleted or moved misc_model. The bridge hides the matching
    // GfxStaticModelDrawInst and, with HIDE_COLLISION, the cStaticModel_s contents. axis rows follow AnglesToAxis.
    static constexpr int kMaxModelHides = 256;
    // Live models: xmodel names Radiant wants as script_models, the bridge answers per index.
    static constexpr int kMaxLiveModelNames = 32;
    static constexpr int kLiveModelNameLen = 64;
    struct ModelHideRecord
    {
        char entityGuid[40];
        char model[64];
        float origin[3];
        float axis[9];
        float scale;
        LONG flags;                // HideFlags
    };

    // Volume decals: a compiled misc_volume_decal is a VolDecalCompiled record (VOLDECAL_HIDE hides it, VOLDECAL_REF only looks it up).
    // One moved, changed or placed after the Play is a VolDecalLive record the bridge draws itself.
    static constexpr int kMaxVolDecalCompiled = 256;
    static constexpr int kMaxVolDecalLive = 128;
    enum VolDecalFlags : LONG
    {
        VOLDECAL_HIDE = 1 << 0,       // hide the compiled decal
        VOLDECAL_REF = 1 << 1,        // look it up only (a live record names it as refIndex)
        VOLDECAL_REVEAL_ALL = 1 << 2, // "revealDataSize" is revealAll (or absent)
    };
    struct VolDecalCompiled
    {
        char entityGuid[40];
        char material[64];
        float origin[3];
        float axis[9];
        float halfExtents[3];
        LONG flags;                // VolDecalFlags
    };
    struct VolDecalLive
    {
        char entityGuid[40];
        char material[64];
        float origin[3];
        float axis[9];
        float halfExtents[3];
        float edgeFeather[3];
        float uvBaseAndScale[4];
        LONG refIndex;             // VolDecalCompiled index whose reveal uv and sort to use, -1 none
        LONG flags;                // VolDecalFlags (REVEAL_ALL)
    };

    // Live lights: Radiant sends the GfxConfig_Light (0x230 bytes) of every light moved, changed, placed or deleted after the Play.
    // guid = first 8 hex digits of the entity GUID: a match is overwritten in place every frame (REMOVED: color zeroed),
    // else the light is added as a dynamic scene light.
    static constexpr int kMaxLiveLights = 128;
    static constexpr int kLightConfigSize = 0x230;
    enum LiveLightFlags : LONG
    {
        LIVELIGHT_REMOVED = 1 << 0,   // the entity is gone: the compiled light goes dark (config unused)
        LIVELIGHT_NEW = 1 << 1,       // placed after the Play (the level has no such light)
    };
    struct LiveLight
    {
        uint32_t guid;
        LONG flags;                // LiveLightFlags
        unsigned char config[kLightConfigSize];
    };

    // Live fx: Radiant sends every fx entity moved, changed, placed or deleted after the Play (guid = first 8 hex digits of the entity GUID).
    // The bridge stops the compiled effect (FX_ThroughWithEffect), plays the record's and answers in liveFxResult.
    // A missing effect is linked as a live zone ("fx,<name>").
    static constexpr int kMaxLiveFx = 256;
    enum LiveFxFlags : LONG
    {
        LIVEFX_REMOVED = 1 << 0,      // the entity is gone: the compiled effect stops
        LIVEFX_NEW = 1 << 1,          // placed after the Play (the level has no such fx)
        LIVEFX_EXPLODER = 1 << 2,     // the entity has exploder keys: a compiled one keeps the game's exploder state, a new one is not played
    };
    enum LiveFxResult : LONG
    {
        LIVEFX_PENDING = 0,
        LIVEFX_PLAYING = 1,           // playing (or stopped, for REMOVED / a state the level is not in)
        LIVEFX_MISSING = 2,           // the level has no such effect (Radiant links it as a live zone)
        LIVEFX_UNMATCHED = 3,         // no compiled fx with this guid (REMOVED or changed)
        LIVEFX_FAILED = 4,            // bad values or the engine call raised
    };
    struct LiveFx
    {
        char entityGuid[40];
        uint32_t guid;             // first 8 hex digits of the entity GUID (RadiantFX.guid)
        LONG flags;                // LiveFxFlags
        char fxdef[128];
        float origin[3];
        float angles[3];
        LONG delay;                // "delay" in ms (negative: started that long ago)
        LONG stateMask;            // fxstate1..4 -> bits 0..3 (absent = on)
        LONG outdoor;
        LONG reserved;
    };

    // Live sky: the bridge rewrites model, rotation and size of every sky entry g_skybox points at whose compiled model is "from"
    // ("" = any) and restores them when the record leaves. The record also carries the ssi's sun and exposure values (converted as
    // the linker does), written into the sun volume's settings[state]. A sky the level lacks is linked as a live zone ("xmodel,<to>").
    static constexpr int kMaxLiveSkies = 16;
    static constexpr int kLiveSkyNameLen = 64;
    enum LiveSkyFlags : LONG
    {
        LIVESKY_ROTATION = 1 << 0,    // rotation is valid (else the compiled entry keeps its own)
        LIVESKY_SIZE = 1 << 1,        // size is valid
        LIVESKY_SUN = 1 << 2,         // the sun and exposure values are valid
        LIVESKY_FORCE = 1 << 3,       // the ssi changed since the Play: write the sun even where the level compiled the record's sky
    };
    enum LiveSkyResult : LONG
    {
        LIVESKY_PENDING = 0,
        LIVESKY_APPLIED = 1,          // drawn (or the level already draws it)
        LIVESKY_MISSING = 2,          // the level has no xmodel of that name (Radiant links it as a live zone)
        LIVESKY_UNMATCHED = 3,        // no sky entry of the level has "from" as its compiled model (yet)
        LIVESKY_FAILED = 4,
    };
    struct LiveSky
    {
        char from[kLiveSkyNameLen];   // the compiled sky xmodel to replace, "" = whatever sky the level draws
        char to[kLiveSkyNameLen];     // the sky xmodel to draw
        float rotation;               // GfxSkyBox.rotation (skyRotation of the sky material, degrees)
        float size;                   // GfxSkyBox.size (skySize)
        LONG flags;                   // LiveSkyFlags
        LONG reserved;
        // ssi GfxGlobalLightSettings values (PC offsets: pitch +0, yaw +4, color +8, intensity +20, specScale +24, penumbra +28,
        // shadow +96, dynamicShadow +97, evcmp +100, evmin +104, evmax +108)
        char ssi[kLiveSkyNameLen];
        float pitch, yaw;
        float color[3];               // linear (colorSRGB through the sRGB curve)
        float intensity;              // 2^stops, 0 when enablesun is 0
        float specScale;              // 2^spec_comp, clamped to [0, 65504]
        float penumbra;
        float evcmp, evmin, evmax;
        LONG shadow;                  // enablesun
        LONG dynamicShadow;
    };

    // Live baked lighting: a link appends new lighting blobs to <map>.xpak under new keys. Radiant sends every blob the link added
    // (BakeBlob) plus the file's new data / index sections, the bridge points each StreamWrappedBuffer of the same type and name
    // at its new blob and lets the engine's streamer reload it.
    static constexpr int kMaxBakeBlobs = 256;
    struct BakeBlob
    {
        uint64_t key;
        uint64_t offset;           // from the index entry, relative to the data section
        uint64_t size;             // from the index entry (bit 63 = compressed)
        uint32_t declared;         // sum of the text record's sizeN
        LONG type;                 // StreamWrappedType: 0 reflectionProbes, 1 probeVolume, 2 SST, 4 skybox
        char name[64];
    };

    // Bake values that live in the fastfile: each reflection probe's exposure and average cube colour per sun volume and lighting state
    // (from the .led "volumes" section), written into the GfxReflectionProbe records matched by origin.
    static constexpr int kMaxBakeProbes = 1024;
    struct BakeProbe
    {
        LONG volume;
        LONG state;
        float origin[3];
        float exposure;
        float avgCubeColor[3];
    };

    // A bake sun shadow tree (.led lightState.sst): the level keeps its table next to the tree pointer (GfxMultiResSST: mipDrop = 3 - mipCount,
    // rootOffsets = running element counts, then 88 config bytes: world-to-tree transform, dimensionInTiles, inchesPerTexel, spanInches,
    // yaw, pitch). The xpak blob is the mips' u32 elements back to back.
    static constexpr int kMaxBakeSsts = 64;
    struct BakeSst
    {
        LONG volume;
        LONG state;
        uint32_t mipCount;
        uint32_t mipElements[4];
        unsigned char config[88];
        // the tree's min/max map (.led lightState.sstMinMax): size and the .led offset of the "maxs" floats (w * h),
        // the bridge builds the 512 x 512 ushort mip chain from them
        uint32_t minMaxWidth;
        uint32_t minMaxHeight;
        uint64_t maxsFileOffset;
        // the volume's sun values (.led volume.*), stored in the level's GfxConfig_Sun
        float shadowSplitDistance;
        float shadowBiasScale;
        float transitionTime;
        LONG streamLighting;
        // where the tree's mips start in the .led (per mip: u32 count, then the elements): the bridge reads the tree into its own
        // memory, a streamed tree cannot outgrow the address range the level reserved
        uint64_t mipsFileOffset;
    };

    struct LiveObjectRecord
    {
        uint64_t liveObjectId;
        uint32_t generation;
        uint32_t collisionContents;  // game contents when flags has LIVE_OBJECT_CONTENTS (0 = no collision)
        LONG operation; // LiveObjectOperation
        LONG type;      // LiveGeometryType
        LONG flags;     // LiveObjectFlags
        LONG vertexCount;
        LONG edgeCount;
        LONG triangleCount;
        LONG patchRows;
        LONG patchColumns;
        LONG patchControlPointCount;
        LONG patchWireEdgeCount;
        float transform[16];
        char sourceGuid[40];
        float world[kMaxVertices][3];
        Edge edges[kMaxEdges];
        Triangle triangles[kMaxTriangles];
        LONG faceCount;  // 0 when the producer had no face data (patches, AABB fallback)
        LiveFace faces[kMaxFaces];
    };

    struct EngineAnchor
    {
        volatile LONG stringRva;
        volatile LONG firstXrefRva;
        volatile LONG xrefCount;
    };

    struct SharedState
    {
        uint32_t magic;
        uint32_t version;
        DWORD gamePid;
        volatile LONG inputSeq;
        volatile LONG frameSeq;
        volatile LONG status;
        volatile LONG enabled;
        volatile LONG vertexCount;
        volatile LONG edgeCount;

        volatile LONG presentCount;
        volatile LONG acceptedPresentCount;
        volatile LONG renderProbeCount;

        // Executable diagnostics.
        volatile LONG gameBuildTimestamp;
        volatile LONG gameImageSize;

        // View-matrix probe diagnostics.
        volatile LONG matrixProbeActive;
        volatile LONG matrixCandidateCount;
        volatile LONG matrixReadCount;
        volatile LONG matrixReadFailures;
        volatile LONG matrixSource;            // 0 none, 1 legacy RVA direct, 2 legacy RVA pointer, 3 AOB-derived, 4 D3D11 constant buffer
        volatile LONG matrixMode;              // 0 none, 1 row-vector convention, 2 column-vector convention
        volatile LONG matrixCandidateRva;      // selected candidate RVA, source 4 = cbuffer byte offset
        volatile LONG projectionPasses;
        volatile LONG projectedVertexCount;
        volatile LONG projectedEdgeCount;
        volatile LONG lastProjectedInputSeq;
        volatile LONG lastProjectedFrameSeq;
        volatile LONG projectionFaultCount;
        volatile LONG matrixScore;
        float firstProjectedW;

        volatile LONG lastWidth;
        volatile LONG lastHeight;

        char brushGuid[40];
        float world[kMaxVertices][3];
        Edge edges[kMaxEdges];

        float screen[kMaxVertices][2];
        uint8_t visible[kMaxVertices];

        char statusText[256];

        // ---- in-engine editor-runtime foundation ----
        volatile LONG engineState;
        volatile LONG engineReadyFlags;
        volatile LONG engineHeartbeat;
        volatile LONG engineDiscoveryPasses;
        volatile LONG engineDiscoveryFailures;
        volatile LONG gameWindowThreadId;
        volatile LONG editorOwnedRuntime;  // 1 only for a BO3 process this plugin started
        volatile LONG engineWorldActive;  // 1 only on real engine-state proof

        // One-producer/one-consumer mailbox: Radiant writes the command data, a memory barrier, then bumps engineCommandSeq.
        // The bridge copies the processed sequence to engineCommandAck.
        volatile LONG engineCommandSeq;
        volatile LONG engineCommandAck;
        volatile LONG engineCommand;
        volatile LONG engineCommandResult;

        // Command-buffer bridge diagnostics, enabled only after exact byte/string guards match the build.
        volatile LONG cbufAddTextRva;
        volatile LONG cbufVerified;
        volatile LONG cbufGuardMask; // bits: AddText string, InsertText string, both xrefs, executable RVA
        volatile LONG engineCommandExecCount;
        volatile LONG engineCommandExecFailures;
        volatile LONG engineCommandThreadId;
        volatile LONG engineCommandClientIndex;

        // Deprecated: kept for layout continuity, they never authorize gameplay work.
        volatile LONG connectionStateRva;
        volatile LONG connectionStateVerified;
        volatile LONG connectionState;
        volatile LONG connectionStateTransitions;
        volatile LONG activeMapReads;
        volatile LONG activeMapReadFailures;
        volatile LONG activeMapMatchesRequested;
        volatile LONG fastRestartCount;

        // Guarded main-frame pump: every engine call from the mailbox must come from mainFrameThreadId.
        volatile LONG mainFrameRva;
        volatile LONG mainFrameVerified;
        volatile LONG mainFrameGuardMask; // bits: prologue, WinMain caller, executable RVA
        volatile LONG mainFrameHookInstalled;
        volatile LONG mainFramePumpCount;
        volatile LONG mainFrameThreadId;
        volatile LONG mainFrameInstallFailures;

        // Typed engine-state diagnostics, promoted only after restart-core references match the build.
        volatile LONG serverRunningDvarRva;
        volatile LONG serverRunningVerified;
        volatile LONG serverRunning;
        volatile LONG mapnameDvarRva;
        volatile LONG mapnameVerified;
        volatile LONG typedStateGuardMask; // bits: sv_running restart-core ref, mapname restart-core ref
        volatile LONG mapnameDvarType;
        volatile LONG runtimeStateSampleCount;
        volatile LONG runtimeStateSampleFailures;
        volatile LONG runtimeStateLastFailure; // 0 ok, 1 globals, 2 server type, 3 server value, 4 map type, 5 map pointer, 6 map text

        // Lifecycle boundary diagnostics: the restart core and its wrappers are traced, direct invocation stays disabled.
        volatile LONG lifecycleRestartCoreRva;
        volatile LONG lifecycleMapRestartRva;
        volatile LONG lifecycleFastRestartRva;
        volatile LONG lifecycleVerified;
        volatile LONG lifecycleGuardMask; // bits: core prologue/executable, map wrapper, fast wrapper
        volatile LONG beginPlayCount;
        volatile LONG endPlayCount;

        // Lifecycle observation: guarded hooks observe BO3's own restart sequence (arguments, order, thread, timing) without starting it.
        volatile LONG playSessionState; // PlaySessionState
        volatile LONG lifecycleTraceInstalled;
        volatile LONG lifecyclePhaseGuardMask; // build, ShutdownGame, InitGameProgs, restart finalizer
        volatile LONG lifecycleShutdownGameRva;
        volatile LONG lifecycleInitGameProgsRva;
        volatile LONG lifecycleFinalizeRva;
        volatile LONG lifecycleShutdownCallCount;
        volatile LONG lifecycleInitCallCount;
        volatile LONG lifecycleFinalizeCallCount;
        volatile LONG lifecycleLastShutdownArg;
        volatile LONG lifecycleLastInitArg0;
        volatile LONG lifecycleLastInitArg1;
        volatile LONG lifecycleLastInitArg2;
        volatile LONG lifecycleLastCallThreadId;
        volatile LONG lifecycleWrongThreadObservations;
        volatile LONG lifecycleCallOrder;
        volatile LONG lifecycleShutdownOrder;
        volatile LONG lifecycleInitOrder;
        volatile LONG lifecycleFinalizeOrder;
        volatile LONG lifecycleLastFailurePhase;
        volatile LONG mapGeneration;
        volatile LONG gameplayGeneration;
        volatile LONG scriptVmGeneration;
        volatile LONG scriptVmGenerationVerified;
        volatile LONG serverInitializationGeneration;
        volatile LONG playerSpawnGeneration;
        volatile LONG playerSpawnGenerationVerified;
        volatile LONG mapLoadCount;
        volatile LONG mapLoadCountVerified;
        volatile LONG dbZoneLoadCount;
        volatile LONG dbZoneLoadCountVerified;
        volatile LONG transientEntityCount;
        volatile LONG transientEntityCountVerified;
        uint64_t lifecycleLastBeginDurationMs;
        uint64_t lifecycleLastEndDurationMs;

        // Settled world-space geometry payload (developer fallback). nativeGeometrySeq is a seqlock: odd while Radiant writes,
        // even when a reader may snapshot. The line function is used only when nativeLineGuardMask is complete.
        volatile LONG nativeGeometrySeq;
        volatile LONG nativeLineRva;
        volatile LONG nativeLineVerified;
        volatile LONG nativeLineGuardMask; // bits: PE fingerprint, exact prologue, box-helper call, executable RVA
        volatile LONG nativeDrawMode; // LiveRenderMode, D3D mode is an explicit developer fallback
        volatile LONG nativeAabbSubmitCount;
        volatile LONG nativeLineSubmitCount;
        volatile LONG nativeDrawFailures;
        volatile LONG nativeDrawThreadId;
        volatile LONG nativeLastInputSeq;
        volatile LONG nativeLastVertexCount;
        volatile LONG nativeLastEdgeCount;
        volatile LONG nativeWorldInactiveSkips;

        // Native filled-surface path: the guarded render-stage hook submits the same world-space triangles as BO3's collision debug
        // renderer. The line path stays as an outline diagnostic.
        volatile LONG nativeTriangleRva;
        volatile LONG nativeTriangleVerified;
        volatile LONG nativeTriangleGuardMask; // build, stage prologue/caller, submit/init/flush prologues, executable RVAs
        volatile LONG nativeTriangleHookInstalled;
        volatile LONG nativeTriangleSubmitCount;
        volatile LONG nativeTriangleDrawFailures;
        volatile LONG nativeTriangleThreadId;
        volatile LONG nativeLastTriangleCount;

        // Settled geometry diagnostics. Current-shape values (fallbackToAabb too) reset per snapshot,
        // reconstructionFailures and nativeEdgeSubmits are cumulative.
        volatile LONG geometryType; // LiveGeometryType
        volatile LONG faceCount;
        volatile LONG planeCount;
        volatile LONG reconstructedVertexCount;
        volatile LONG uniqueEdgeCount;
        volatile LONG degenerateEdges;
        volatile LONG reconstructionFailures;
        volatile LONG fallbackToAabb;
        volatile LONG nativeEdgeSubmits;

        // Persistent multi-object Live Edit registry. Radiant is the sole writer, BO3 snapshots it from the frame pump.
        // Selection is a flag, never ownership.
        volatile LONG liveRegistrySeq;
        volatile LONG liveRegistryGeneration;
        volatile LONG liveObjectCount;
        volatile LONG liveBrushCount;
        volatile LONG livePatchCount;
        volatile LONG liveCreateCount;
        volatile LONG liveUpdateCount;
        volatile LONG liveDeleteCount;
        volatile LONG liveClearCount;
        volatile LONG staleObjectCount;
        volatile LONG lastRegistryOperation; // LiveObjectOperation
        uint64_t lastRegistryObjectId;
        volatile LONG lastRegistryObjectGeneration;
        volatile LONG patchRows;
        volatile LONG patchColumns;
        volatile LONG patchControlPointCount;
        volatile LONG patchWireEdgeCount;
        volatile LONG patchReconstructionFailures;
        volatile LONG patchFallbackToAabb;
        LiveObjectRecord liveObjects[kMaxLiveObjects];

        // Read-only PE discovery anchors (diagnostics, not callable addresses).
        EngineAnchor anchorMapname;
        EngineAnchor anchorFrontend;
        EngineAnchor anchorDevmap;
        EngineAnchor anchorMapRestart;
        EngineAnchor anchorDisconnect;
        EngineAnchor anchorClIngame;
        EngineAnchor anchorSvRunning;
        EngineAnchor anchorFsGame;

        char requestedMap[64];
        char activeMap[64];
        char commandText[256];
        char lastQueuedCommand[256];
        char engineStatusText[256];

        // Native-triangle guard diagnostics, appended after the registry fields. The writer publishes the text before incrementing seq.
        volatile LONG nativeTriangleGuardDiagnosticSeq;
        char nativeTriangleGuardDiagnostics[4096];

        // Zone module: Radiant writes BO3LiveZone.ini (map=), then sets zoneArm = 1. The bridge answers in zoneState (ZoneState)
        // and zoneStatusText, and names the matched build in zoneBuildName.
        volatile LONG zoneArm;
        volatile LONG zoneState;
        char zoneBuildName[32];
        char zoneStatusText[256];

        // Textured Live Edit: Radiant fills materialNames under the registry seqlock (indices used by LiveFace::material),
        // the bridge reports what it resolved and drew.
        volatile LONG materialCount;
        char materialNames[kMaxMaterials][kMaterialNameLen];
        volatile LONG texturedVerified;          // the Enhanced poly set / DB_FindXAssetHeader guards passed
        volatile LONG texturedPolysSubmitted;    // cumulative polygons appended to the engine's debug poly set
        volatile LONG texturedLastPolyCount;     // last frame
        volatile LONG texturedMaterialsResolved; // names that resolved to a Material with an unlit technique
        volatile LONG texturedMaterialsMissing;  // names the game does not have loaded (colored fill instead)
        volatile LONG texturedSetFull;           // frames where the poly set had no room left
        volatile LONG texturedGBufferState;  // 0 off, 1 the gbuffer draw path is installed, -1 it failed its guards
        volatile LONG texturedGBufferLastPolyCount;  // polygons drawn through the gbuffer (lit) path last frame
        // per materialNames index: 0 not looked up yet, 1 resolved, 2 the level has no such material (Radiant links it into the
        // patch zone and reloads), 3 present but unusable (no technique)
        volatile LONG materialState[kMaxMaterials];
        // Live zone load: Radiant names a fastfile it placed in usermaps\<map>\zone (no extension) and bumps zoneLoadSeq. The bridge loads it
        // on the main-frame thread (DB_LoadXAssets) and answers in zoneLoadDoneSeq / zoneLoadResult (1 ok) / zoneLoadText.
        char zoneLoadName[64];
        volatile LONG zoneLoadSeq;
        volatile LONG zoneLoadDoneSeq;
        volatile LONG zoneLoadResult;
        char zoneLoadText[256];
        char texturedStatus[1024];
        // Hidden baked brushes: Radiant publishes the records under the hideSeq seqlock and bumps hideGeneration. The bridge applies them on
        // the render thread (index buffer) and main-frame thread (collision). hideState: 0 off, 1 world matched, -1 guards failed (hideStatus).
        volatile LONG hideSeq;
        volatile LONG hideGeneration;
        volatile LONG hideCount;
        HideRecord hides[kMaxHideRecords];
        volatile LONG patchHideCount;  // under the same hideSeq seqlock and hideGeneration
        PatchHideRecord patchHides[kMaxPatchHides];
        volatile LONG hideState;
        volatile LONG hideAppliedGeneration;      // render side
        volatile LONG hideTriangles;              // world triangles currently blanked
        volatile LONG hideUnmatched;              // records whose hull matched no world triangle
        volatile LONG hideCollisionGeneration;    // collision side
        volatile LONG hideBrushes;                // collision brushes currently disabled
        char hideStatus[512];
        // Live collision: every exact Live Edit brush also collides (a cbrush_t in the bridge's copy of the clipmap brush array, registered
        // in the leafs it touches). liveCollisionState: 0 off, 1 baked twins only (trace thread info not found), 2 appended slots too.
        volatile LONG liveCollisionState;
        volatile LONG liveCollisionBrushes;
        volatile LONG liveCollisionAppended;
        char liveCollisionStatus[256];
        // Live nav mesh: every exact Live Edit brush gets a Havok silhouette generator in the level's hkaiWorld (the engine's DisconnectPaths).
        // liveNavEnabled comes from Radiant (live_navmesh). liveNavState: 0 off, 1 detoured (waiting for a level), 2 a level's world is in hand,
        // -1 guards failed (liveNavStatus).
        volatile LONG liveNavEnabled;
        volatile LONG liveNavState;
        volatile LONG liveNavObstacles;     // our enabled silhouettes
        volatile LONG liveNavGenerators;    // silhouette generators in the world (the engine's and ours)
        char liveNavStatus[256];
        // Nav mesh swap: Radiant links the fresh navmesh asset as a live zone, has the bridge load it (zoneLoad*), then names it here and bumps
        // navSwapSeq. The bridge swaps the level's hkaiWorld nav mesh instance on the game thread and answers in navSwapDoneSeq /
        // navSwapResult (1 ok, 0 failed, 2 unchanged) / navSwapText.
        char navSwapAssetName[128];
        volatile LONG navSwapSeq;
        volatile LONG navSwapDoneSeq;
        volatile LONG navSwapResult;
        char navSwapText[256];
        volatile LONG navSwapOp;  // 1 revert to the base zone's nav mesh (before a live zone that frees earlier ones loads), 2 swap to navSwapAssetName
        volatile LONG zoneLoadSyncedSeq;  // the zoneLoadDoneSeq whose DB_Sync has run (asset overrides apply from here on)
        // Live occlusion: the compiled Umbra data still occludes through a hidden baked brush, so after the engine's Umbra camera query the
        // bridge marks visible every world surface and static model in the shadow volume a hide record's box casts (and all light / fx bits).
        // liveOccEnabled comes from Radiant (live_occlusion). liveOccState: 0 off, 1 detoured, 2 records in hand, -1 guards failed.
        volatile LONG liveOccEnabled;
        volatile LONG liveOccState;
        volatile LONG liveOccSurfaces;      // world surfaces forced visible last frame
        volatile LONG liveOccModels;        // static models forced visible last frame
        char liveOccStatus[256];
        // Hidden baked static models: Radiant publishes the records under the modelHideSeq seqlock and bumps modelHideGeneration. The bridge
        // answers on the main-frame thread: modelHideState 0 off, 1 waiting, 2 applied, -1 guards failed. matched = instances hidden,
        // unmatched = records no instance fit, collision = models disabled.
        volatile LONG modelHideSeq;
        volatile LONG modelHideGeneration;
        volatile LONG modelHideCount;
        ModelHideRecord modelHides[kMaxModelHides];
        volatile LONG modelHideState;
        volatile LONG modelHideAppliedGeneration;
        volatile LONG modelHideMatched;
        volatile LONG modelHideUnmatched;
        volatile LONG modelHideCollision;
        char modelHideStatus[256];
        // Live models: Radiant writes the xmodel names its script_models need (names first, then the count). The bridge looks each up in the
        // asset DB on the main-frame thread (never creating a stub). liveModelState: 0 not looked up, 1 loaded, 2 the level lacks it
        // (Radiant links it as a live zone), 3 lookup unavailable in this build.
        volatile LONG liveModelNameCount;
        char liveModelNames[kMaxLiveModelNames][kLiveModelNameLen];
        volatile LONG liveModelState[kMaxLiveModelNames];
        // Volume decals: Radiant publishes both lists under the volDecalSeq seqlock and bumps volDecalGeneration. The bridge applies the hides
        // on the main-frame thread and draws the live ones from the frontend's decal pass. volDecalState: 0 off, 1 waiting for a level,
        // 2 applied, -1 guards failed. hidden / unmatched / drawn / missing count the compiled decals hidden, records no decal fit, live decals drawn, and live decals whose vd/ material is absent.
        volatile LONG volDecalEnabled;
        volatile LONG volDecalSeq;
        volatile LONG volDecalGeneration;
        volatile LONG volDecalCompiledCount;
        VolDecalCompiled volDecalCompiled[kMaxVolDecalCompiled];
        volatile LONG volDecalLiveCount;
        VolDecalLive volDecalLive[kMaxVolDecalLive];
        volatile LONG volDecalState;
        volatile LONG volDecalAppliedGeneration;
        volatile LONG volDecalHidden;
        volatile LONG volDecalUnmatched;
        volatile LONG volDecalDrawn;
        volatile LONG volDecalMissing;
        char volDecalStatus[256];
        // Live lights: Radiant publishes the list under the liveLightSeq seqlock and bumps liveLightGeneration. The bridge applies it from the
        // frontend's scene build. liveLightState: 0 off, 1 waiting, 2 applied, -1 guards failed. overridden = compiled lights rewritten,
        // added = dynamic lights added last frame, unmatched = REMOVED records with no light.
        volatile LONG liveLightEnabled;
        volatile LONG liveLightSeq;
        volatile LONG liveLightGeneration;
        volatile LONG liveLightCount;
        LiveLight liveLight[kMaxLiveLights];
        volatile LONG liveLightState;
        volatile LONG liveLightAppliedGeneration;
        volatile LONG liveLightOverridden;
        volatile LONG liveLightAdded;
        volatile LONG liveLightUnmatched;
        char liveLightStatus[256];
        // Live fx: Radiant publishes the list under the liveFxSeq seqlock and bumps liveFxGeneration. The bridge applies it on the main-frame
        // thread and writes liveFxResult[i] (LiveFxResult) for liveFxAppliedGeneration. liveFxState: 0 off, 1 waiting, 2 applied, -1 guards failed.
        volatile LONG liveFxEnabled;
        volatile LONG liveFxSeq;
        volatile LONG liveFxGeneration;
        volatile LONG liveFxCount;
        LiveFx liveFx[kMaxLiveFx];
        volatile LONG liveFxResult[kMaxLiveFx];
        volatile LONG liveFxState;
        volatile LONG liveFxAppliedGeneration;
        volatile LONG liveFxPlaying;       // records whose effect the bridge started
        volatile LONG liveFxStopped;       // compiled effects stopped because their entity was deleted
        volatile LONG liveFxMissing;
        char liveFxStatus[256];
        // Live baked lighting: Radiant fills the blobs and pak sections, then bumps bakeSwapSeq. The bridge answers bakeSwapDoneSeq with
        // bakeSwapResult (1 swapped, -1 failed, nothing changed) and bakeSwapText. bakeSwapState: 0 off, 1 ready, -1 guards failed.
        volatile LONG bakeSwapSeq;
        volatile LONG bakeSwapDoneSeq;
        volatile LONG bakeSwapResult;
        volatile LONG bakeSwapState;
        volatile LONG bakeBlobCount;
        BakeBlob bakeBlob[kMaxBakeBlobs];
        char bakePakName[64];  // the xpak's file name without extension (the map name)
        char bakeLedPath[260];  // the exported .led (the bridge reads the min/max floats from it)
        char bakePakPath[260];  // the xpak's full path (the link replaces the file, the game still holds the old one)
        char bakeProbePath[260];  // the probe layout file (ffread.h WriteProbeFile), empty when the bake keeps the level's probe layout
        uint64_t bakePakData[3];           // the file header's data section: itemCount, offset, size
        uint64_t bakePakIndex[3];          // the file header's index section: itemCount, offset, size
        volatile LONG bakeSwapped;         // wrappers pointed at new blobs by the last swap
        volatile LONG bakeProbeCount;
        // Disable AI: Radiant sets it from the pane's box. While 1 the zone module queues "ai_disableSpawn 1" once a second
        // (the engine resets the dvar on a level load and script SetDvar does not reach it).
        volatile LONG disableAi;
        BakeProbe bakeProbe[kMaxBakeProbes];
        volatile LONG bakeSstCount;
        BakeSst bakeSst[kMaxBakeSsts];
        char bakeSwapText[512];
        // Live sky: Radiant publishes under the liveSkySeq seqlock and bumps liveSkyGeneration. The bridge applies on the main-frame thread and
        // answers liveSkyResult[i] (LiveSkyResult) for liveSkyAppliedGeneration. liveSkyState: 0 off, 1 waiting, 2 applied, -1 guards failed.
        // liveSkyDrawn: the compiled model of the sky entry g_skybox points at now (what it draws without Live Edit).
        volatile LONG liveSkyEnabled;
        volatile LONG liveSkySeq;
        volatile LONG liveSkyGeneration;
        volatile LONG liveSkyCount;
        LiveSky liveSky[kMaxLiveSkies];
        volatile LONG liveSkyResult[kMaxLiveSkies];
        volatile LONG liveSkyState;
        volatile LONG liveSkyAppliedGeneration;
        volatile LONG liveSkyRewritten;    // sky entries of the level drawing a live sky now
        volatile LONG liveSkySunRewritten; // sun volume light states using a live ssi's sun and exposure now
        char liveSkyDrawn[kLiveSkyNameLen];
        char liveSkyStatus[256];
        // Launch hold: Play starts BO3 while it still compiles and links. Radiant sets launchHold = 1 before injection, the bridge hooks
        // kernelbase CreateFileW/A and makes every open of a file under usermaps\<map>\zone wait until Radiant clears it.
        // launchHoldState: 5 gate installed, 1 holding a map file open, 2 released, 3 timed out (15 min).
        volatile LONG launchHold;
        volatile LONG launchHoldState;
        volatile LONG launchHoldMs;        // how long the frame was held
    };

    inline void MappingName(DWORD pid, wchar_t out[96])
    {
        wsprintfW(out, L"Local\\BetterRadiant_BO3Live_v37_%lu", static_cast<unsigned long>(pid));
    }

    // Dock requests Radiant posts to the game window (PostMessage only), run on the window's own thread by the bridge's subclass.
    // Radiant must never call SetWindowLongPtr/SetParent/SetFocus on a docked window: a cross-process child shares Radiant's input queue
    // and one synchronous call deadlocked both editors.
    constexpr unsigned int WM_DOCK_INTO = 0x8000 + 0x51;  // wParam = host HWND, child of it, sized to its client area
    constexpr unsigned int WM_UNDOCK    = 0x8000 + 0x52;  // wParam = x | (y << 32), lParam = w | (h << 32): popup there, foreground
    constexpr unsigned int WM_RESTORE   = 0x8000 + 0x54;  // the window's original style and rect
    constexpr unsigned int WM_FOCUS     = 0x8000 + 0x55;  // foreground + focus (Radiant called AllowSetForegroundWindow first)
    constexpr unsigned int WM_GIVE_FOREGROUND = 0x8000 + 0x56;  // wParam = HWND: the game (foreground) makes that window foreground
    constexpr unsigned int WM_SYNC_CURSOR = 0x8000 + 0x57;     // bridge-internal: apply the game's cursor intent on the window thread

}
