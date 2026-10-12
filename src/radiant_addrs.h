#pragma once
// Radiant_modtools.exe (BO3 mod tools) function and global addresses. The exe has no ASLR (base 0x140000000), rt::Rva() rebases.
// 0 = not located: hooks on a 0 address are skipped.
#include <cstdint>

namespace rt
{
    // Identity of the binary these addresses belong to.
    constexpr uint32_t  kExpectedTimestamp = 0x5DF45023;   // IMAGE_FILE_HEADER.TimeDateStamp
    constexpr uint32_t  kExpectedSizeOfImage = 0x6D95000;
    constexpr uintptr_t kImageBase = 0x140000000ULL;

    // --- probe gather (ray guiding research, observation targets only) ---
    constexpr uintptr_t ProbeImportanceSample = 0x141174B20;
    constexpr uintptr_t ProbeSampleDirection = 0x141129F60;
    constexpr uintptr_t ProbeAccumulateRadiance = 0x141119DD0;
    constexpr uintptr_t ProbeGatherVoxel = 0x141131670;
    constexpr uintptr_t ProbeGatherAdaptiveVoxel = 0x141131D20;
    constexpr uintptr_t ProbeGatherCachedSample = 0x1411334F0;
    constexpr uintptr_t ProbeAccumulatorInit = 0x141119CC0;
    constexpr uintptr_t ProbeAccumulatorFinalize = 0x14111A6F0;
    constexpr uintptr_t TraceRayPackets8 = 0x1411A5420;
    constexpr unsigned char sig_ProbeGatherVoxel[] = {0x89,0x54,0x24,0x10,0x55,0x56,0x41,0x55,0x41,0x56,0x48,0x8d,0xac,0x24,0x58,0xfa,0xff,0xff};
    constexpr size_t off_GatherContext_Probe = 8;
    constexpr size_t off_Probe_VoxelFlags = 1648;
    constexpr size_t off_Probe_VoxelPositions = 1672;
    constexpr size_t off_Probe_VoxelCoefficients = 1752; // float[18] per voxel after gather
    constexpr size_t off_Probe_ImportanceSampling = 360; // static branch condition
    constexpr unsigned char sig_ProbeImportanceSample[] = {0x4c,0x89,0x4c,0x24,0x20,0x4c,0x89,0x44,0x24,0x18,0x48,0x89,0x4c,0x24,0x08};
    constexpr unsigned char sig_ProbeSampleDirection[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x48,0x89,0x7c,0x24,0x18};
    constexpr unsigned char sig_ProbeAccumulateRadiance[] = {0x40,0x55,0x48,0x8b,0xec,0x48,0x83,0xec,0x60,0xf2,0x0f,0x10,0x15,0x1f,0xdc,0x18,0x01};

    // --- map load / save (map.cpp) ------------------------------------------------------
    // __int64 Map_LoadFile(char* filename): no unsaved-changes prompt
    constexpr uintptr_t Map_LoadFile = 0x140CD1760;
    // char Map_SaveFile(char* filename, char silentAutosave, char saveSelected): empty filename opens the Save dialog
    constexpr uintptr_t Map_SaveFile = 0x140CD2890;
    constexpr uintptr_t Map_New      = 0x140CD2EA0;
    // Map_LeavePrefab(char updateTitle): pops the prefab stack (g_prefabStack), does not write the prefab file
    constexpr uintptr_t Map_LeavePrefab = 0x140CD5E30;
    // Map_EnterPrefab(EntityInst* prefabOrNull, char showAll, char flag): null = the selected misc_prefab.
    // Swaps in the prefab's in-memory entity list (shared by every reference), the file is not re-read.
    constexpr uintptr_t Map_EnterPrefab = 0x140CDF5A0;

    // --- main window idle tick (mainfrm.cpp) --------------------------------------------
    constexpr uintptr_t RadiantMainWin_RoutineProcessing = 0x14064BFB0;   // (RadiantMainWin* this)
    // Pending view-redraw bitmask: 0x1 = 3D camera view, 0x2 = 2D grid views. RoutineProcessing sets 0x101 every frame,
    // so the 3D view redraws continuously and the 2D views only when marked dirty.
    constexpr uintptr_t g_redrawFlags = 0x1444C656C;   // int
    constexpr unsigned  kRedraw3D     = 0x1;
    constexpr unsigned  kRedraw2D     = 0x2;
    // "Update XY on drag" pref (CamXYUpdate, prefs block 0x1453A1C20 + 0x8C): camera drags also mark the 2D views dirty (0x3)
    constexpr uintptr_t g_camXYUpdate = 0x1453A1CAC;   // int (0/1)

    // --- external-modification detection that already exists in Radiant ----------------
    // FilesModifiedCheck(this): "Files Modified" Yes/No box, Yes reloads the map. Only runs when Perforce is enabled.
    constexpr uintptr_t FilesModifiedCheck = 0x14065C450;

    // --- view drawing (camwnd.cpp / xywnd.cpp) -----------------------------------------
    // __int64 CCamWnd::Cam_Draw(CCamWnd* this, ...)
    constexpr uintptr_t CCamWnd_Cam_Draw = 0x140244120;
    // __int64 CXYWnd::XY_Draw(CXYWnd* this, __int64* viewport)
    constexpr uintptr_t CXYWnd_XY_Draw = 0x140C2E3E0;
    constexpr uintptr_t CCamWnd_OnRender = 0x14024F080;
    // Cam view HUD text, called from CCamWnd::OnRender after the 3D scene: (CCamWnd* this, ?, int w, int h)
    constexpr uintptr_t CCamWnd_DrawHud = 0x1402488A0;
    constexpr size_t    off_CamWnd_HudW = 200;          // int: pane size the HUD lays out in
    constexpr size_t    off_CamWnd_HudH = 204;
    // void DrawText2D(const char* text, Font** font, const float pos[3], const float xAdvance[3], const float yDir[3],
    //                 const float rgba[4], float outEnd[3] (nullable), uint8 backgroundAlpha)   (thunk sub_14008A265)
    // pos = baseline start. backgroundAlpha > 0 draws a black box first. Refuses fonts whose material (+0x48) is null.
    constexpr uintptr_t DrawText2D = 0x1411E1FE0;
    constexpr uintptr_t Font_GetGlyph = 0x14004AAAC;    // glyph* (Font** font, int ch): +2 s8 xOff, +3 s8 yOff, +4 u8 advance, +5 u8 w, +6 u8 h
    constexpr uintptr_t g_debugFont = 0x145573AA0;      // Font*: +0x40 int line height, +0x48 material
    constexpr uintptr_t g_gameView  = 0x1453A1EE9;      // char: 1 = "GAME VIEW" (scene letterboxed to 16:9 inside the cam pane)
    constexpr uintptr_t g_probeView = 0x1453A1EEA;      // char: 1 = "STATIC PROBE VIEW"
    // float Cam_GetFov() (thunk sub_14000F182): horizontal FOV of a 16:9 view, used by every projection and pick.
    // Editor view: Fov pref (0x1453A1D88), game view 80.67 (cg_fov 65, 4:3), probe view 90. RadiantPlus detours it.
    constexpr uintptr_t Cam_GetFov  = 0x1402316D0;

    // --- entities / brushes / selection (entity.cpp, brush.cpp, BrushCollection) --------
    constexpr uintptr_t Entity_Parse      = 0x140C8C830;   // (parseSession, ..., flag): allocates entity_t (632 bytes)
    constexpr uintptr_t Entity_Write      = 0x140C8FCF0;   // (entity_t*, FILE*, char): "{ guid ... }" + brushes
    constexpr uintptr_t Entity_GuidString = 0x140C911B0;   // (entity_t*, std::string* out): StringFromGUID2 of entity+352
    constexpr uintptr_t Entity_AddToList  = 0x140CA5690;   // (list sentinel, entity_t*)
    constexpr uintptr_t Brush_Parse       = 0x1401E3FE0;   // (parseSession, int, char): reads guid, mesh/curve/patch bodies
    constexpr uintptr_t Brush_Create      = 0x1401DFEF0;
    constexpr uintptr_t Brush_BuildWindings = 0x1401DED20; // recomputes brush+64 mins / +76 maxs (+88/+100 padded copy)
    constexpr uintptr_t BrushCollection_ForEach = 0x140B00BB0;   // (collection, std::function<bool(BrushInst*)>*)
    // Fixed-size misc_model category callback (generic and model-specific cull checks)
    constexpr uintptr_t BrushCache_ModelDispatcher = 0x1401DC540;
    // Early generic brush culler: tests brush_t's padded bounds (+88/+100) before the model renderer
    constexpr uintptr_t BrushCache_GenericCull = 0x140231930;
    // Alternate fixed-size brush culler for the normal camera context, also reads the padded bounds
    constexpr uintptr_t BrushCache_AlternateCull = 0x140C2F4D0;
    // Nested XMODEL BrushInst visibility test
    constexpr uintptr_t BrushInst_VisibilityCull = 0x1401E8FE0;
    // ProcessRadBrushes walks six vectors of 16-byte category entries after Prepare, an entry's first pointer = the BrushInst
    constexpr uintptr_t BrushCache_Prepare = 0x140213380;
    constexpr uintptr_t BrushCache_ProcessRadBrushes = 0x1402131D0;
    // Default camera pass cache: arena of 208-byte BrushInst records + vector of { arenaIndex, passMask } pairs
    // (ProcessRadBrushes(1, ...) iterates it, not the six typed vectors)
    constexpr uintptr_t g_BrushCacheArena = 0x1444877C0;
    constexpr uintptr_t g_BrushCacheRootEntries = 0x1444877E0;
    constexpr size_t    kBrushCacheArenaRecordOffset = 32;
    constexpr size_t    kBrushCacheArenaRecordStride = 208;
    constexpr uintptr_t g_BrushCacheCategories = 0x144487830;
    constexpr size_t    kBrushCacheCategoryCount = 6;
    constexpr uintptr_t g_selectedBrushes = 0x1453A2278;   // BrushCollection "%s - Selected Brushes" (96 bytes, +56 = list sentinel)
    constexpr uintptr_t g_activeBrushes   = 0x1453A22D8;   // BrushCollection "%s - Active Brushes"

    // entity_t layout (632 bytes)
    constexpr size_t    off_Entity_BrushPrev = 0;          // brush list sentinel inside the entity (prev/next of its brushes)
    constexpr size_t    off_Entity_BrushNext = 8;
    constexpr size_t    off_Entity_EClass    = 240;
    constexpr size_t    off_Entity_Origin    = 280;        // float[3]
    constexpr size_t    off_Entity_Angles    = 292;        // float[3] pitch yaw roll
    constexpr size_t    off_Entity_KeyValues = 304;        // std::map<std::string, QVariant> (MSVC 2012 layout, QString or QVector3D values, see hooks::EntityKeyValue)
    constexpr size_t    off_Entity_Guid      = 352;        // GUID (16 bytes)
    constexpr size_t    off_Entity_ListPrev  = 440;        // global entity list links (sentinel g_entityList)
    constexpr size_t    off_Entity_ListNext  = 448;
    // undo identity (Q3Radiant's undoId/redoId/entityId): Undo_AddBrush (0x140C17220 entity copy) stamps live objects,
    // Undo_Undo (0x140C181D0) removes them and re-links saved copies, matching a brush to its owner by entityId
    constexpr size_t    off_Entity_UndoId    = 340;        // int
    constexpr size_t    off_Entity_RedoId    = 344;        // int
    constexpr size_t    off_Entity_Id        = 348;        // int, per session (saved brush copies keep their owner's id at +40)
    // brush_t layout (nodes of an entity_t's brush list)
    constexpr size_t    off_Brush_Prev       = 0;          // links in the owning entity's list (sentinel = the entity)
    constexpr size_t    off_Brush_Next       = 8;
    constexpr size_t    off_Brush_Owner      = 16;         // entity_t*
    constexpr size_t    off_Brush_UndoId     = 32;         // int (see off_Entity_UndoId)
    constexpr size_t    off_Brush_RedoId     = 36;         // int
    constexpr size_t    off_Brush_Guid       = 48;         // GUID (16 bytes)
    constexpr size_t    off_Brush_Mins       = 64;         // float[3]
    constexpr size_t    off_Brush_Maxs       = 76;         // float[3]
    constexpr size_t    off_Brush_PaddedMins = 88;         // float[3], fixed-size renderer cull input
    constexpr size_t    off_Brush_PaddedMaxs = 100;        // float[3], fixed-size renderer cull input
    constexpr size_t    off_Brush_Layer      = 168;        // std::string layer name
    constexpr size_t    off_Brush_Type       = 232;        // int: 1 physics_box, 2 physics_cylinder, else classic (patches: off_Brush_Patch != 0)
    // BrushInst layout (nodes of the selected/active BrushCollections)
    constexpr size_t    off_BInst_Prev       = 0;          // links in the collection list (selected / active)
    constexpr size_t    off_BInst_Next       = 8;
    constexpr size_t    off_BInst_EntityInst = 16;         // EntityInst*
    constexpr size_t    off_BInst_ListPrev   = 24;         // links in the owning EntityInst's instance list
    constexpr size_t    off_BInst_ListNext   = 32;
    constexpr size_t    off_BInst_Brush      = 48;         // brush_t*
    constexpr size_t    off_BInst_Flags      = 96;         // 0x80 = selected
    // BrushCache_Add stores its arena record reference here (the default camera pass iterates records, not BrushInst pointers)
    constexpr size_t    off_BInst_RootCacheRecord = 176;   // cache arena record pointer
    constexpr size_t    off_BInst_CullMins   = 480;        // float[3], rebuilt by BrushCache_Add
    constexpr size_t    off_BInst_CullMaxs   = 492;        // float[3], rebuilt by BrushCache_Add
    // EntityInst layout (776 bytes)
    constexpr size_t    off_EInst_Entity     = 0;          // entity_t*
    constexpr size_t    off_EInst_ListSentinel = 16;       // pseudo BrushInst whose ListPrev/ListNext live at +40/+48
    constexpr size_t    off_EInst_Next       = 768;        // next EntityInst in the world list
    constexpr uintptr_t g_world              = 0x14522B130; // world object; +776 = worldspawn EntityInst*
    constexpr uintptr_t EntityInst_Begin     = 0x1400186F6; // thunk: (world, EntityInst** out) -> writes first inst
    constexpr uintptr_t EntityInst_End       = 0x14004E1C0; // thunk: (world, EntityInst** out) -> returns EntityInst** end
    // BrushCollection layout (partial)
    constexpr size_t    off_Collection_List  = 56;         // pointer to the list sentinel, first = *(sentinel+8)

    // --- paste / delete pipeline ---
    constexpr uintptr_t Map_ImportBuffer   = 0x140CD3E60;   // __int64 (char** text, uint flags, uchar regenerateGuids)
    constexpr uintptr_t BeginParseSession  = 0x1406F6A80;   // (const char* name)
    constexpr uintptr_t EndParseSession    = 0x140D3C9F0;   // ()
    constexpr uintptr_t Undo_Begin         = 0x140C164D0;   // (const char* label)  wrapper around Undo_Start
    constexpr uintptr_t Undo_End           = 0x140C16520;   // ()
    constexpr uintptr_t Undo_AddBrushList  = 0x140B00FA0;   // (BrushCollection*)  records the selection for undo
    constexpr uintptr_t Selection_BeginBatch = 0x140B01060; // (BrushCollection*)  ++batch depth
    constexpr uintptr_t Selection_EndBatch = 0x140B01070;   // (BrushCollection*, int 3, __int64 0, char 1)  fires the change event
    constexpr uintptr_t Selection_CloseUndo = 0x140B00FF0;  // (BrushCollection*)  after Undo_AddBrushList
    constexpr uintptr_t Select_Brush       = 0x140B01DB0;   // (BrushCollection*, BrushInst*)  NOT brush_t*: it reads the inst's GUID bytes at +56 and crashes on a bare brush
    constexpr uintptr_t Select_Deselect    = 0x140D023E0;   // (char clearDrag)
    constexpr uintptr_t Select_Delete      = 0x140D02320;   // ()  deletes everything in the selected collection
    constexpr uintptr_t Undo_IsOpen        = 0x140C16ED0;   // () -> bool: the top undo operation is still open (paste then joins it instead of starting one)
    // --- moving geometry in place ---
    // Brush_Move(brush_t*, const float delta[3], bool snapToGrid, bool markModified): moves the face plane points, rebuilds,
    // moves mesh/curve data and, for a fixed-size entity, its origin. Radiant's drag runs it per brush. Works unselected.
    constexpr uintptr_t Brush_Move         = 0x1401E1540;
    constexpr uintptr_t Brush_Build        = 0x1401DEB00;   // (brush_t*, bool snap, bool markModified, uint flag)
    constexpr uintptr_t Select_Move        = 0x140D02700;   // (const float delta[3], bool snap): Brush_Move on every selected brush + status text
    constexpr uintptr_t MoveSelection      = 0x140C5CCC0;   // the drag handler that calls Select_Move for a plain translate
    constexpr uintptr_t Entity_UpdateOrigin = 0x140C92110;  // (entity_t*): "origin" key from entity+280, targetname bookkeeping
    constexpr uintptr_t Brush_RebuildBrush = 0x1401E8800;   // (brush_t*, mins, maxs): six faces from a box
    constexpr uintptr_t Brush_SnapToGrid   = 0x1401E6590;
    constexpr size_t    off_Brush_FaceBegin = 144;         // face_t array begin (brush_t+144), end at +152, 864 bytes per face
    constexpr size_t    off_Brush_FaceEnd   = 152;
    constexpr size_t    sz_Face             = 864;         // face_t: +0 planepts float[3][3], +40 texdef, +808 winding pointer
    constexpr size_t    off_Face_PlanePts   = 0;
    constexpr size_t    off_Face_Winding    = 808;   // winding pointer (face_t+808)
    // Entity registry: std::multimap<GUID, EntityInst*> keyed by entity_t+352 (Entity List pane, exploder manager).
    // Remove looks up by the CURRENT GUID: never rewrite a GUID while registered (Radiant's duplicate fixer 0x140CD3300 removes, re-GUIDs, inserts).
    constexpr uintptr_t g_entityRegistry   = 0x14522B6D0;   // _Tree head pointer, node +25 isnil, +32 GUID key, +48 EntityInst*
    constexpr uintptr_t EntityRegistry_Add = 0x140CD81B0;   // (EntityInst*)  insert under the entity's GUID + add its Entity List row
    constexpr uintptr_t EntityRegistry_Remove = 0x140CD82F0; // (EntityInst*)  find by the entity's GUID and this instance, drop the row, erase

    // ---- QuestionBox: __int64 (void* parent, const char* text, const char* title, int buttons, int defaultButton) ----
    // buttons 0x14000 = Yes|No, returns 0x4000 Yes, 0x10000 No. Map_LoadFile and Map_New ask "Lighting changed..." through it (thunk 0x14006CA76),
    // the host answers No on its own reloads.
    constexpr uintptr_t QuestionBox         = 0x140D337C0;
    constexpr uintptr_t str_LightingChanged = 0x14264DA20;   // the text of that question (.rdata)

    // --- undo (undo.cpp) --------------------------------------------------------------
    constexpr uintptr_t Undo_Start    = 0x140C17B40;
    constexpr uintptr_t Undo_AddBrush = 0x140C16660;

    // --- helpers used by Radiant's reload path --------------
    // void Sys_SetTitle(const char* mapName): sets the main window title (+ " :: DEV SHADERS" / " (Experimental)")
    constexpr uintptr_t Sys_SetTitle = 0x140D33530;
    // char Bookmarks_Save(void* bookmarks, const char* mapName): writes Bookmark0..9 + LastUsed to QSettings
    constexpr uintptr_t Bookmarks_Save = 0x140D46D40;
    // int Bookmarks_Count(void* bookmarks): number of valid camera bookmarks
    constexpr uintptr_t Bookmarks_Count = 0x140D47010;
    // char PrepareForMapLoad(): leaves any open prefab (Map_LeavePrefab(1)) after a modified check, 0 = abort
    constexpr uintptr_t PrepareForMapLoad = 0x140D35340;
    // int Map_Parse(const char* filename, void* entityList, uchar flag): reads the file, checks "iwmap", parses entities
    constexpr uintptr_t Map_Parse = 0x140CDB0C0;
    // __int64 Map_Write(const char* filename): the .map writer called by Map_SaveFile
    constexpr uintptr_t Map_Write = 0x140CDBEB0;

    // --- globals ----------------------------------------------------------------------
    constexpr uintptr_t g_currentMapName = 0x14521ED20;   // char[] "Str1": path of the open map ("unnamed.map" when none)
    constexpr uintptr_t g_modified       = 0x14521EB94;   // int: 0 clean, 1 dirty, 2 after save-selected
    constexpr uintptr_t g_prefabDepth    = 0x14521EB9C;   // int "gPrefabStackLevel": 0 = editing the top-level map, max 16
    constexpr uintptr_t g_prefabStack    = 0x14521F320;   // 16 entries x 2936 bytes: +0 int modified, +4 char name[] of the map that was left, +1096 its entity list, +1728 its world object
    constexpr size_t    kPrefabStackEntry = 2936;
    constexpr size_t    off_PrefabStack_Inst = 1752;   // the entered prefab's own instance/world object, NOT the misc_prefab entity
    constexpr uintptr_t g_prefabModified = 0x14521EB98;   // int: set when the prefab being edited changed (instances rebuilt on leave)
    constexpr uintptr_t g_saving         = 0x14521EB90;   // char: re-entrancy guard inside Map_SaveFile
    // char: "copy the selection into the next map" flag (New/Open path), cleared after its paste. While set, Map_ImportBuffer
    // skips its target/linkto remap, which otherwise deletes every target and linkto key of the imported text.
    constexpr uintptr_t g_importKeepLinks = 0x14521EB91;
    constexpr uintptr_t g_pMainWin       = 0x1444C6560;   // RadiantMainWin*  (mainwin+64 -> QE children, children+240 -> CCamWnd*)
    constexpr uintptr_t g_bookmarks      = 0x14521F200;   // camera bookmark table (10 x 28 bytes from +16, LastUsed int at +280)
    constexpr uintptr_t g_lastBookmark   = 0x14521F318;   // int: bookmark index Map_LoadFile moves the camera to (-1 = none)
    constexpr uintptr_t g_entityList     = 0x14522B440;   // global entity list filled by Map_Parse
    constexpr uintptr_t g_worldEntity    = 0x14522B438;   // EntityInst* of worldspawn (its entity_t is at +0), set by Map_LoadFile
    constexpr uintptr_t g_brushCount     = 0x1453A1810;   // int, counted during load
    constexpr uintptr_t g_entityCount    = 0x145268194;   // int, Map_Parse result

    // lighting build (TeamCreate lightsync.cpp)
    constexpr uintptr_t CCamWnd_BuildLighting = 0x1402F31D0;   // void (CCamWnd*, bool rebuildSun, bool rebuildProbes): the bolt and its Rebuild menu
    constexpr size_t    off_CamWnd_ViewMode    = 232;          // int: 4 = real lighting
    constexpr size_t    off_CamWnd_BakeVolume  = 6456;         // LightingVolume*, state = vol+1164+4*(cam+3960), or cam+6500 without one
    constexpr size_t    off_CamWnd_BakePaused  = 6656;         // bool

    // CCamWnd layout fragments
    constexpr size_t    off_MainWin_QEChildren = 64;
    constexpr size_t    off_QEChildren_CamWnd  = 240;
    constexpr size_t    off_CamWnd_Origin      = 208;      // float[3]
    constexpr size_t    off_CamWnd_Angles      = 220;      // float[3]
    constexpr size_t    off_CamWnd_Width       = 3964;     // int (from Cam_Draw: passed to the projection setup)
    constexpr size_t    off_CamWnd_Height      = 3968;     // int

    // --- patch control grid and vertex selection (Drag Vertices mode, V) ---
    // Undo/redo/insert re-create patch objects (same GUID, new address): never cache a patch or vertex pointer.
    constexpr size_t    off_Brush_Patch      = 224;        // patch data (0 for classic brushes)
    constexpr size_t    off_Patch_Rows       = 660;        // int: blocks in the .map text (Radiant's menus call one a "column")
    constexpr size_t    off_Patch_Cols       = 664;        // int: points per block
    constexpr size_t    off_Patch_Ctrl       = 672;        // control vertex array, index row*cols+col, first block first
    constexpr size_t    sz_PatchCtrl         = 92;         // record: +0 float[3] xyz
    constexpr uintptr_t g_patchVertSelCount  = 0x1452711F4; // int
    constexpr uintptr_t g_patchVertSel       = 0x1452711F8; // float*[]: control record addresses (max 4097 for model welds, 1025 for patch connect)
    constexpr uintptr_t g_modelVertCand      = 0x1452E1240; // float[3][]: selected model verts, weld candidates
    constexpr uintptr_t g_modelVertCandCount = 0x145341240; // int
    constexpr uintptr_t g_tolerantWeldFlag   = 0x1453A1D50; // int: 1 routes Patch_ConnectVerts to the XY-only tolerant welds (read 0 even with the pref on)
    constexpr uintptr_t g_dragMode           = 0x1453A17D4; // int: 4 = Drag Vertices
    constexpr int       kDragModeVertices    = 4;
    // void Patch_ConnectVerts(char): the W handler's body in Drag Vertices mode (0x140650860 checks the drag mode first).
    // Flag 0, candidates > 0: every selected vertex copies candidate[0], undo "connect vertices", both counts cleared.
    constexpr uintptr_t Patch_ConnectVerts   = 0x14079C450;
    constexpr uintptr_t Patch_ConnectToCandidate = 0x1407BF640; // what the dispatcher calls in that case (not called directly)
    constexpr uintptr_t Patch_InsertDelete   = 0x1407956A0; // (insert, column, first) over the selected brushes (menu wrappers 0x140655DB0..EA0)
    constexpr uintptr_t Patch_Resize         = 0x14078E1D0; // (patch, rows, cols): remaps the vertex selection by index
    constexpr uintptr_t Patch_RebuildMesh    = 0x1407A49C0; // (patch): re-tessellates the render mesh from the control net

    // CXYWnd layout fragments (from XY_Draw)
    constexpr size_t    off_XYWnd_Scale        = 132;      // float pixels per unit
    constexpr size_t    off_XYWnd_Origin       = 136;      // float[3] view centre
    constexpr size_t    off_XYWnd_Widget       = 256;      // QWidget*
    constexpr size_t    off_XYWnd_ViewType     = 264;      // int: 2 = XY (top), 1 = XZ (front), 0 = YZ (side)

    // --- docking (TADockWidget (QFrame) -> QRadDockWidget -> every pane) ---
    // QRadDockWidget::QRadDockWidget(this, const QString& title, QObject* bridge): TADockWidget ctor (toggle QAction at +64,
    // unique name at +72, content slot at +80), then +104 = bridge->mainWindow (bridge+568), +112 = bridge.
    constexpr uintptr_t QRadDockWidget_ctor          = 0x1408186D0;
    constexpr size_t    kQRadDockWidgetAlloc         = 256;         // real size is >= 120 and < 184 (smallest derived pane); over-allocate
    constexpr uintptr_t TADockWidget_setWidget       = 0x140E88F80; // (this, QWidget* orNull): adds it to the dock's layout, null = first child widget
    constexpr uintptr_t TADockWidget_toggleView      = 0x14003BCB9; // slot toggleView(bool) (thunk), what the pane's menu action triggers
    constexpr uintptr_t RadiantMainWin_addDockWidget = 0x14064B4C0; // (QMainWindow* this, Qt::DockWidgetArea (unused), QRadDockWidget*): rejects QDialogs, reparents into the container
    constexpr uintptr_t RadiantMainWin_DockContainer = 0x140E8AEC0; // (QMainWindow*) -> the TADockContainer central widget, null until the UI exists
    constexpr uintptr_t RadiantQtBridge_staticMetaObject = 0x14331FA58;
    constexpr size_t    off_Bridge_MainWindow        = 568;         // RadiantQtBridge+568 = the QMainWindow (RadiantMainWin)
    constexpr size_t    off_TADock_ToggleAction      = 64;          // QAction* (checkable, text = title)
    constexpr size_t    off_TADock_Content           = 80;          // QWidget* set by setWidget
    // Moving a dock (as Radiant's own float/hide 0x140E7FCB0 and toggleView 0x140E89040): detach from its container, add to the target, relayout.
    // A hidden pane sits in a hidden floating TADockWindow's container, so the same steps bring it back.
    constexpr uintptr_t TADockWidget_container       = 0x140044F62; // thunk -> 0x140E88750 (dock): nearest TADockContainer ancestor or null
    constexpr uintptr_t TADockContainer_remove       = 0x14000763A; // thunk -> 0x140E849D0 (container, dock): empties its slot (name kept), disconnects dock->container, relayouts
    constexpr uintptr_t TADockContainer_add          = 0x14000860C; // thunk -> 0x140E7E760 (container, dock, fromRestore, orientation, side, relativeTo, tabify): tabify = into relativeTo's tab group, fromRestore=0 drops the dock's old named slots
                                                                    // fromRestore=1 fills the empty slot of the same name in the restored layout and returns
    constexpr size_t    off_TADock_Undocked          = 88;          // byte, addDockWidget zeroes it (setter 0x140E88E40) before the add
    constexpr uintptr_t TADockContainer_relayout     = 0x1400A44D0; // thunk -> 0x140E82C50 (container): posts the layout rebuild
    constexpr size_t    vt_QWidget_setVisible        = 11;          // vtable slot (QObject 0..10, then setVisible)

    // Convert an IDA absolute address into the live address (identity unless relocated).
    uintptr_t Rva(uintptr_t idaAddress);
    // Timestamp/size check of the loaded exe. Always false inside APE, which keeps every Radiant hook off there.
    bool      VerifyBinary();
}

// asseteditor_modtools.exe (APE) code addresses the host needs. ASLR is on: these are IDA addresses rebased with app::ModuleBase(),
// used only when app::VerifyApeBinary() matches.
namespace apeaddr
{
    constexpr uintptr_t apeFileCache_SaveCache   = 0x1400FD330; // (apeFileCache* this): runs once from apeFileCache::Start on the main thread
    constexpr uintptr_t QVectorPtr_reallocData   = 0x14008AFA0; // QVector<T*>::reallocData(QVector* v, int size, int alloc, int options); options 0
    constexpr size_t    off_FileCache_Entries    = 0x18;        // QVector<apeGdtFileCacheEntry*> (one QArrayData* pointer)

    // External GDT change: the post-commit refresh frees each changed file's old records BEFORE calling ReloadDeleteAssetDialog,
    // whose deleted-asset loop still reads them through entry+0x30.
    constexpr uintptr_t ReloadDeleteAssetDialog  = 0x14007A040; // (tree*, QVector<apeGdtFileCacheEntry*>* changed, QVector<char*>* deleted files)
    constexpr size_t    off_Tree_GdtRows         = 0x28;        // tree: QVector of 24-byte rows { ?, QString path @+8, QVector<entry*> @+16 }
    constexpr size_t    off_CacheEntry_Records   = 0x118;       // apeGdtFileCacheEntry: char path[] @+0 (relative to TA_GAME_PATH), QVector<record*> @+0x118
    constexpr size_t    off_Entry_Pairs          = 0x28;        // asset entry: QVector<pair*>* once materialized, else null
    constexpr size_t    off_Entry_Record         = 0x30;        // asset entry: parse record (values +0x18, flags +0x20) until materialized

    // GDT folder walk: Start runs it once before the main window, the refresh runs it again when the dirty byte is set.
    constexpr uintptr_t apeFileCache_Instance    = 0x14118D4D0; // the one apeFileCache
    constexpr uintptr_t apeFileCache_Walk        = 0x1400F85F0; // QVector<entry*>* (apeFileCache*, QVector<entry*>* out)
    constexpr uintptr_t apeFileCache_Refresh     = 0x1400F8A70; // void (apeFileCache*): walk + re-parse + ReloadDeleteAssetDialog, only while the dirty byte is set
    constexpr size_t    off_FileCache_Roots      = 0x20;        // QList<QByteArray>: the walk's roots (bin\converter_gdt_dirs_0.txt), relative to TA_GAME_PATH
    constexpr size_t    off_FileCache_Dirty      = 0x50;        // bool: set by APE's ReadDirectoryChangesW thread on any change under TA_GAME_PATH
}

// cod2map64.exe (T7 changelist 3421987, SHA-256 201A6B31...8EFD), verified by BuildPlusCompiler.dll.
namespace cod2mapaddr
{
    constexpr uint32_t Timestamp = 0x5DF4499B;
    constexpr uint32_t ImageSize = 0x75317000;

    // C1, coalesced surface props lookup (tris_coalesce.cpp). The bucket table is written only by FindOrCreate and Clear.
    constexpr uintptr_t CoalesceFindOrCreateRva = 0xE9170; // props* (props* a, props* b, ctx*): the record for the merged chain
    constexpr uintptr_t CoalesceClearRva        = 0xE8810; // void (): memset of the 256-bucket table, from FreeSurfaceProperties
    constexpr uintptr_t CoalesceFlattenRva      = 0xE8C60; // int (props*, props** out, int start, ctx*): sorted leaf props, returns count
    constexpr uintptr_t CoalesceSkipRva         = 0x1ACCF0; // bool (props* a, props* b, ctx*): true = FindOrCreate returns a
    constexpr uintptr_t MaterialFlagsRva        = 0xB5790; // unsigned (mtlTex*): (flags & 0x70) == 0x10 marks a hidden surface
    constexpr uintptr_t CoalesceBucketsRva      = 0x498F6440; // props* [256], bucket = low byte of h = byte[k] + (k + 118) * h over the key bytes
    constexpr size_t    CoalesceRecordChain     = 0xD8;  // record + 27*8: chain list {props*, next}
    constexpr size_t    CoalesceRecordNext      = 0xE0;  // record + 28*8: next record in the bucket (new records are pushed at the head)

    // C2, grid tree (tris_gridtree.cpp): 2,097,151 nodes {float lo, float hi, item* head}, children 2i+2 (lo side
    // test) and 2i+1. Items: +24 mins[3], +36 maxs[3] (floats), +72 prev, +80 next in the node list.
    constexpr uintptr_t GridNodesRva      = 0x498F6C70; // node* (pointer variable)
    constexpr uintptr_t GridSmallRva      = 0x498F6C90; // bool: extent < 100, everything lives in the root
    constexpr uintptr_t GridAxesRva       = 0xA0ABE0;   // int[21] split axis per depth
    constexpr uintptr_t GridInsertRva     = 0xEB800;    // void (item*): push at the head of Descend(item bounds)
    constexpr uintptr_t GridRemoveRva     = 0xEB880;    // void (item*): unlink, no-op unless it is linked where stock looks
    constexpr uintptr_t GridResetRva      = 0xEB6D0;    // (int mins[3], int maxs[3]): re-initialise, all lists empty
    constexpr uintptr_t GridRebuildRva    = 0xEB7C0;    // void (): gather every item into the root and redistribute
    constexpr uintptr_t GridFreeRva       = 0xEBA80;    // void (): free the node array
    constexpr uintptr_t GridAllocRva      = 0xEBA20;    // void* (): operator new(0x1FFFFF0) into the node pointer, asserts if set
    constexpr uintptr_t GridDescendRva    = 0xEBD10;    // node* (float mins[3], float maxs[3]): the node an item belongs to
    constexpr uintptr_t GridQueryRva      = 0xEC310;    // (float mins[3], float maxs[3], node, depth, cb(item, depth+1, node, axes))
    constexpr uintptr_t GridCountRva      = 0xEC8B0;    // int (float mins[3], float maxs[3], node, depth, limit): overlaps, stops above limit
    constexpr uintptr_t BoundsOverlapRva  = 0x1226A0;   // bool (amins, amaxs, bmins, bmaxs)
    constexpr unsigned char GridInsertSig[]    = {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x30,0x48,0x8d,0x51,0x24,0x48,0x8b};
    constexpr unsigned char GridRemoveSig[]    = {0x40,0x53,0x48,0x83,0xec,0x30,0x48,0x83,0x3d,0xe2,0xb3,0x80,0x49,0x00,0x48,0x8b};
    constexpr unsigned char GridResetSig[]     = {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x30,0x48,0x83,0x3d,0x8e,0xb5,0x80};
    constexpr unsigned char GridRebuildSig[]   = {0x48,0x83,0xec,0x28,0x80,0x3d,0xc5,0xb4,0x80,0x49,0x00,0x75,0x21,0xe8,0x1e,0x0a};
    constexpr unsigned char GridAllocSig[]     = {0x48,0x83,0xec,0x38,0x48,0x83,0x3d,0x44,0xb2,0x80,0x49,0x00,0x74,0x2c,0x48,0x8d};
    constexpr unsigned char GridFreeSig[]      = {0x48,0x83,0xec,0x38,0x48,0x83,0x3d,0xe4,0xb1,0x80,0x49,0x00,0x75,0x2c,0x48,0x8d};
    constexpr unsigned char GridDescendSig[]   = {0x40,0x55,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x30,0x48,0x83,0x3d,0x4e,0xaf,0x80};
    constexpr unsigned char GridQuerySig[]     = {0x48,0x89,0x5c,0x24,0x08,0x44,0x89,0x44,0x24,0x18,0x55,0x56,0x57,0x48,0x83,0xec};
    constexpr unsigned char GridCountSig[]     = {0x48,0x89,0x5c,0x24,0x10,0x44,0x89,0x4c,0x24,0x20,0x44,0x89,0x44,0x24,0x18,0x48};
    constexpr unsigned char BoundsOverlapSig[] = {0xf3,0x0f,0x10,0x01,0x41,0x0f,0x2f,0x01,0x77,0x40,0xf3,0x41,0x0f,0x10,0x00,0x0f};
    constexpr int GridAxes[21] = {0,1,0,1,0,1,2,0,1,0,1,0,1,2,0,1,0,1,0,1,2};
    // C6, vertex weld grid (tris_mergeverts.cpp:105): int* (const float* base, int coordCount, int stride, int count,
    // float eps). Allocates map then next (4*count each) with the exe's operator new, frees next, returns map.
    constexpr uintptr_t WeldRva         = 0xF1DF0;
    constexpr uintptr_t WeldBucketsRva  = 0x498F6D20; // int[16384], memset to -1 on entry
    constexpr uintptr_t EpsilonEqualRva = 0x1264A0;   // bool (const float* a, const float* b, float eps, int n): (a-b)^2 <= eps^2 per coord
    constexpr uintptr_t ExeNewRva       = 0x534DC4;   // the exe's operator new (callers free with the exe's delete)
    constexpr uintptr_t ExeDeleteRva    = 0x53532A;
    constexpr unsigned char WeldSig[]         = {0x48,0x8b,0xc4,0x44,0x89,0x48,0x20,0x48,0x89,0x48,0x08,0x53,0x55,0x56,0x57,0x41};
    constexpr unsigned char EpsilonEqualSig[] = {0x33,0xc0,0x45,0x85,0xc9,0x7e,0x26,0xf3,0x0f,0x59,0xd2,0x48,0x2b,0xca,0x66,0x90};

    // C4, per-soup triangle edge neighbours: (ctx*, u32 firstVert, unused, const u16* idx (6 per triangle, first 3 used),
    // u32 triCount, int* out[3*triCount]) -> 3*triCount (if > 1, else out), caller ignores the result.
    constexpr uintptr_t EdgeNeighboursRva = 0x1BA270;
    constexpr unsigned char EdgeNeighboursSig[] = {0x4c,0x89,0x4c,0x24,0x20,0x53,0x55,0x56,0x57,0x41,0x55,0x48,0x83,0xec,0x30,0x8b};

    // C5, triangle subgroups (tris.cpp): u16 (tri** surfTris, int* surfCounts, int surfCount, const int* vertMap).
    // Triangles are 120 bytes: u16 label at +2 (output), int vertex ids at +24/+28/+32. Entries 24 bytes {3 sorted ids,
    // pad, tri*} from exe new, CRT qsort with EntryCompare, window/relabel loop, exe delete[], then per-surface qsort by label.
    constexpr uintptr_t SubgroupsRva       = 0x1AD2E0;
    constexpr uintptr_t SortIdsRva         = 0x1B8240;  // void (const int src[3], int dst[3]): ascending
    constexpr uintptr_t EntryCompareRva    = 0x1B83D0;  // qsort comparator on d0, d1, d2 (int differences)
    constexpr uintptr_t TriLabelCompareRva = 0x1B8220;  // qsort comparator on the u16 label at +2
    constexpr uintptr_t SubgroupErrorRva   = 0x83930;   // printf-style error ("MAX_TRI_SUBGROUPS")
    constexpr unsigned char SubgroupsSig[]       = {0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x54,0x24,0x10,0x48,0x89,0x4c,0x24,0x08,0x55};
    constexpr unsigned char SortIdsSig[]         = {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xda,0x48,0x8b,0xf9};
    constexpr unsigned char EntryCompareSig[]    = {0x8b,0x01,0x2b,0x02,0x75,0x0e,0x8b,0x41,0x04,0x2b,0x42,0x04,0x75,0x06,0x8b,0x41};
    constexpr unsigned char TriLabelCompareSig[] = {0x0f,0xb7,0x41,0x02,0x0f,0xb7,0x4a,0x02,0x2b,0xc1,0xc3,0xcc,0xcc,0xcc,0xcc,0xcc};

    // C7, full grid initialisation: (int index, float mins[3], float maxs[3], int depth), recursive, the root call
    // (index 0, depth 0) writes all 2,097,151 nodes {split, split, null head}.
    constexpr uintptr_t GridInitRva = 0xEBB50;
    constexpr unsigned char GridInitSig[] = {0x40,0x53,0x56,0x57,0x48,0x83,0xec,0x50,0x48,0x8b,0x05,0x21,0xbd,0x34,0x01,
                                             0x48,0x33,0xc4,0x48,0x89,0x44,0x24,0x40,0x49,0x63,0xf9,0x48,0x63,0xd9,0x48,0x8d,0x0d};

    // C3, per-soup duplicate vertex merge (EmitTrianglesForContext). Vertices: 68-byte records at *(ctx+16), position
    // = first 3 floats, layer data 72-byte records at *(ctx+24) (may be null), triangle records 120 bytes, vertex slot
    // k belongs to triangle k/3.
    constexpr uintptr_t VertexMergeRva = 0x1BB2A0;  // u32 (ctx*, int first, u32 n, u16* idx, u32 triCount, u8* tris, int flags): new n
    constexpr uintptr_t VertexMatchRva = 0x1B9930;  // bool (ctx*, u32 a, u32 b, u8* triA, u8* triB, int flags): |pos diff| <= 0.001 per axis first
    constexpr uintptr_t VertexMatchRejectsRva = 0x74D31A2C; // int: VertexMatch increments it for pairs rejected by its last (tangent sign) test
    constexpr unsigned char VertexMergeSig[] = {0x4c,0x89,0x4c,0x24,0x20,0x89,0x54,0x24,0x10,0x48,0x89,0x4c,0x24,0x08,0x55,0x53};
    constexpr unsigned char VertexMatchSig[] = {0x40,0x53,0x56,0x41,0x56,0x48,0x83,0xec,0x40,0x48,0x8b,0xb4,0x24,0x80,0x00,0x00};
    constexpr unsigned char CoalesceFindOrCreateSig[] = {0x40,0x53,0x41,0x54,0x41,0x55,0x48,0x81,0xec,0x50,0x08,0x00,0x00,0x49,0x8b,0xd8};
    constexpr unsigned char CoalesceClearSig[]        = {0x48,0x8d,0x0d,0x29,0xdc,0x80,0x49,0x33,0xd2,0x41,0xb8,0x00,0x08,0x00,0x00,0xe9};
    constexpr unsigned char CoalesceFlattenSig[]      = {0x40,0x53,0x55,0x56,0x48,0x83,0xec,0x70,0x48,0x8b,0x05,0x11,0xec,0x34,0x01,0x48};
    constexpr unsigned char CoalesceSkipSig[]         = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48};
    constexpr unsigned char MaterialFlagsSig[]        = {0x83,0x79,0x18,0x01,0x75,0x04,0x8b,0x41,0x20,0xc3,0x48,0x8b,0x01,0x0f,0xb7,0x40};

    // C8, collision partition flood fill (collision.cpp, BuildCollision via 0x869E0, which then qsorts the tris).
    // Tris: 120-byte records, partitionId dword at +0, vertex indices dwords 26..28, verts: 16-byte records,
    // partitionId dword at +0, range [first, end) of tris, counter = last partition id handed out.
    constexpr uintptr_t CollisionFloodRva  = 0x85C40;    // void (): labels [first, end) by shared vertex index
    constexpr uintptr_t CollisionTrisRva   = 0x4413F3C0; // u8* tris
    constexpr uintptr_t CollisionVertsRva  = 0x4413F418; // u8* verts
    constexpr uintptr_t CollisionFirstRva  = 0x4413F3B4; // int first tri
    constexpr uintptr_t CollisionEndRva    = 0x4413F3BC; // int end tri (high dword of the qword at 0x4413F3B8)
    constexpr uintptr_t CollisionCounterRva = 0x4417F450; // int partition counter
    constexpr unsigned char CollisionFloodSig[] = {0x48,0x89,0x5c,0x24,0x18,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,
                                                   0x48,0x83,0xec,0x30,0x48,0x63,0x0d,0x59,0x97,0x0b,0x44,0x8b,0x3d,0xef,0x97,0x0f,
                                                   0x44,0x89,0x4c,0x24,0x70,0x4c,0x8b,0xf9,0x48,0x8b,0x0d,0xa9,0x97,0x0b,0x44};

    // C9, material tessSize (only caller: SplitSparseSoups 0x1B13A6). float (material*): type dword at +24, type 1 =
    // source material, looked up by the std::string name at +48 ("material" asset, "tessSize" field, atof, 0 if
    // absent) through the asset database, other types return *(float*)(*material + 32).
    constexpr uintptr_t TessSizeRva = 0xB5810;
    constexpr unsigned char TessSizeSig[] = {0x48,0x8b,0xc4,0x55,0x48,0x8d,0x68,0xb8,0x48,0x81,0xec,0x40,0x01,0x00,0x00,0x48,
                                             0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48,0x89,0x58,0x10,0x48,0x89,0x78,0x18};

    // C10, SplitSparseSoups group merge (inside 0x1AD8E0), three loops patched in place. Soup record: +0x10 first vertex (u32), +0x14 count (u16),
    // labels u16 per vertex in r13, 68-byte vertices at *SoupVerts. A scan: A in r14w, start in di, bounds rbp+8 / rbp+0x18. B scan: B in di, bounds rbp+0x38 / rbp+0x28.
    // Relabel: == B becomes A (r14w), > B drops by one. Only the loop entries jump in.
    constexpr uintptr_t GroupScanARva   = 0x1AE130;   // to 0x1AE184
    constexpr uintptr_t GroupScanBRva   = 0x1AE241;   // to 0x1AE294
    constexpr uintptr_t GroupRelabelRva = 0x1AE3AF;   // to 0x1AE3EC
    constexpr uintptr_t SoupVertsRva    = 0x74D31960; // u8* vertex records
    constexpr uintptr_t BoundsClearRva  = 0x127020;   // void (float* mins, float* maxs): +-131072
    constexpr uintptr_t BoundsAddRva    = 0x122780;   // void (const float* p, float* mins, float* maxs): ordered compares
    constexpr unsigned char GroupScanASig[] = {0x48,0x8d,0x55,0x18,0x48,0x8d,0x4d,0x08,0xe8,0xe3,0x8e,0xf7,0xff,0x0f,0xb7,0xdf,0x66,0x3b,0x7e,0x14,0x73,0x3e,0x66,0x66,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x0f,0xb7,0xc3,0x66,0x45,0x39,0x74,0x45,0x00,0x75,0x20,0x0f,0xb7,0xc3,0x8b,0x4e,0x10,0x03,0xc8,0x48,0x6b,0xc9,0x44,0x48,0x03,0x0d,0xf2,0x37,0xb8,0x74,0x4c,0x8d,0x45,0x18,0x48,0x8d,0x55,0x08,0xe8,0x05,0x46,0xf7,0xff,0x66,0xff,0xc3,0x66,0x3b,0x5e,0x14,0x72,0xcc};
    constexpr unsigned char GroupScanBSig[] = {0x48,0x8d,0x55,0x28,0x48,0x8d,0x4d,0x38,0xe8,0xd2,0x8d,0xf7,0xff,0x33,0xc0,0x0f,0xb7,0xd8,0x66,0x3b,0x46,0x14,0x73,0x3b,0x0f,0x1f,0x80,0x00,0x00,0x00,0x00,0x0f,0xb7,0xc3,0x66,0x41,0x39,0x7c,0x45,0x00,0x75,0x20,0x0f,0xb7,0xc3,0x8b,0x4e,0x10,0x03,0xc8,0x48,0x6b,0xc9,0x44,0x48,0x03,0x0d,0xe2,0x36,0xb8,0x74,0x4c,0x8d,0x45,0x28,0x48,0x8d,0x55,0x38,0xe8,0xf5,0x44,0xf7,0xff,0x66,0xff,0xc3,0x66,0x3b,0x5e,0x14,0x72,0xcc};
    constexpr unsigned char GroupRelabelSig[] = {0x41,0xbc,0xff,0xff,0x00,0x00,0x66,0x66,0x66,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x0f,0xb7,0xc2,0x41,0x0f,0xb7,0x4c,0x45,0x00,0x66,0x3b,0xcf,0x75,0x0d,0x41,0x0f,0xb7,0xce,0x66,0x45,0x89,0x74,0x45,0x00,0x66,0x3b,0xcf,0x76,0x06,0x66,0x45,0x01,0x64,0x45,0x00,0x66,0xff,0xc2,0x66,0x3b,0x56,0x14,0x72,0xd4};
    constexpr unsigned char BoundsClearSig[] = {0xc7,0x01,0x00,0x00,0x00,0x48,0xc7,0x41,0x04,0x00,0x00,0x00,0x48,0xc7,0x41,0x08};
    constexpr unsigned char BoundsAddSig[]   = {0xf3,0x0f,0x10,0x09,0xf3,0x0f,0x10,0x02,0x0f,0x2f,0xc1,0x76,0x04,0xf3,0x0f,0x11};

    // C11, group labelling of a large soup being split (0x1BBD60 -> 0x1BBC30). Soup record: [0] first vertex, [1] vertex count, [2] triangle count, [3] first triangle,
    // vertices 68-byte at *(ctx+16), u16 indices at *(ctx+96) (12-byte stride per triangle).
    // SharesEdge(ctx, soup, t, k): t has an edge equal to one of k's reversed (exact float ==), the C4 test.
    constexpr uintptr_t SoupLabelRva  = 0x1BBC30;   // u32 (ctx*, const u32* soup, u16* labels): group count
    constexpr uintptr_t SharesEdgeRva = 0x1BBAC0;   // char (ctx*, const u32* soup, u32 t, u32 k)
    constexpr unsigned char SoupLabelSig[]  = {0x4c,0x8b,0xdc,0x49,0x89,0x53,0x10,0x49,0x89,0x4b,0x08,0x41,0x55,0x41,0x56,0x41};
    constexpr unsigned char SharesEdgeSig[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48};

    // C12, qhull mem.c in this build (qhull 2003-era, non-reentrant): qhmemT global at QhMemRva (0x88 bytes),
    // qh_memalloc(int size), qh_memfree(void*, int size), qh_memfreeshort(int* curlong, int* totlong), only
    // qh_memfreeshort caller is the per-partition hull routine 0x160C20.
    constexpr uintptr_t QhMemRva          = 0x7503B9A0;
    constexpr uintptr_t QhMemAllocRva     = 0x4C6C10;
    constexpr uintptr_t QhMemFreeRva      = 0x4C6DF0;
    constexpr uintptr_t QhMemFreeShortRva = 0x4C6E90;
    constexpr unsigned char QhMemAllocSig[]     = {0x48,0x89,0x5c,0x24,0x18,0x57,0x48,0x83,0xec,0x20,0x48,0x63,0xd9,0x3b,0x1d,0x8d};
    constexpr unsigned char QhMemFreeSig[]      = {0x48,0x85,0xc9,0x0f,0x84,0x88,0x00,0x00,0x00,0x48,0x89,0x5c,0x24,0x08,0x57,0x48};
    constexpr unsigned char QhMemFreeShortSig[] = {0x48,0x83,0xec,0x28,0x8b,0x05,0x6a,0x4b,0xb7,0x74,0x2b,0x05,0x70,0x4b,0xb7,0x74};
}
