#pragma once
#include <string>
#include <vector>
#include <functional>
#include "modtools_plugin.h"

// Detours into Radiant (MinHook) and the editor-state accessors behind the host API. A hook is only
// installed when its address in radiant_addrs.h is non-zero and the binary check passed.
namespace hooks
{
    bool Install();
    void Remove();

    // Full path of the open map (empty until a load was seen)
    std::wstring CurrentMapPath();
    // File being edited now (the prefab after "Enter Prefab", else the map)
    std::wstring CurrentScopePath();
    int  Modified();                      // Radiant's unsaved-changes flag

    // Brackets our own map writes so a file watcher can tell them from external edits. Any thread.
    void BeginOwnWrite();
    void EndOwnWrite();
    bool OwnWriteRecent();                // in progress, or ended less than 1.5 s ago

    // 3D camera (CCamWnd origin/angles), false before the window exists
    bool GetCamera(float origin[3], float angles[3]);
    // Place the 3D camera (main thread). Angles: pitch, yaw, roll.
    bool SetCamera(const float origin[3], const float angles[3]);
    // "GAME VIEW" flag (16:9 letterboxed 3D view), setter for tests
    bool GameView();
    void SetGameView(bool on);

    // Maximize the main window (any thread)
    void MaximizeMainWindow();

    // Mark views dirty for the next idle tick. bits: rt::kRedraw3D | rt::kRedraw2D. Main thread, no-op in APE.
    // The 3D view redraws every tick anyway, this is for animating 2D overlays.
    void RequestRedraw(unsigned bits);
    // "Update XY on drag" preference, false in APE or on an unknown build
    bool CamXYUpdateOnDrag();

    // Replacement cull envelopes keyed by the model's fixed-size brush, substituted only during the model dispatcher call.
    void SetNativeModelCullBounds(const ModToolsNativeModelCullBounds* bounds, size_t count);

    // One selected brush (or the empty brush of a point entity)
    struct SelectedBrush
    {
        std::string brushGuid;    // "{XXXXXXXX-...}" as written to the .map
        std::string entityGuid;   // owner entity's GUID
        float       mins[3];
        float       maxs[3];
    };
    // Main thread: walks the selected-brush list (bounded), false if unavailable
    bool GetSelection(std::vector<SelectedBrush>& out, size_t maxItems = 4096);

    // ---- raw entity/brush access (MAIN THREAD ONLY) -------------------------------------
    using RadPtr = unsigned char*;
    void        ForEachEntity(const std::function<void(RadPtr ent)>& fn);            // global list order
    void        ForEachEntityBrush(RadPtr ent, const std::function<void(RadPtr brush)>& fn);
    std::string EntityGuid(RadPtr ent);
    RadPtr      WorldEntity();                 // worldspawn entity_t, null before a map is loaded
    std::string BrushGuid(RadPtr brush);
    RadPtr      BrushOwner(RadPtr brush);
    // Serialize one entity (keys + linked brushes) with Radiant's writer
    bool        WriteEntityText(RadPtr ent, std::string& out);
    // Reload the map file without prompting
    bool        ReloadMapNow(const std::wstring& fullPath, bool keepCamera);
    bool        IsReloading();
    bool        InMapLoad();        // main thread: Map_LoadFile is running (a modal box would land mid-load)
    bool MouseButtonHeld();          // any mouse button down (a reload must wait: freelook hides the cursor)

    // ---- in-place editing through Radiant's own paste/delete pipeline (MAIN THREAD ONLY) --
    // Import .map entity blocks (worldspawn block = brushes merged into the world), GUIDs kept, objects left selected
    bool        ImportMapText(const std::string& text, const char* undoLabel);
    void        BeginUndo(const char* label);   // one undo operation, DeleteBrushes/ImportMapText inside it join it
    void        EndUndo();
    bool        DeleteBrushes(const std::vector<RadPtr>& brushes, const char* undoLabel);   // one undoable operation
    // Untracked edits: above depth 0, ImportMapText and DeleteBrushes record no undo (a peer's change must not join the
    // local Ctrl+Z). Undo tags: entity (undoId, redoId, entityId) or brush (undoId, redoId, 0), so a replacement keeps the old identity.
    void        BeginUntracked();
    void        EndUntracked();
    void        ResetUntracked();                      // a plugin faulted between Begin and End
    bool        GetUndoTags(RadPtr object, bool entity, int tags[3]);
    bool        SetUndoTags(RadPtr object, bool entity, const int tags[3]);
    // Selection as BrushInst pointers, SelectBrushes re-selects them
    std::vector<RadPtr> SelectedBrushPtrs();
    // brush_t -> its BrushInst, nullptr if not found
    RadPtr      FindBrushInst(RadPtr brush);
    // After ImportMapText: write our GUIDs back over the fresh ones. entityGuid empty = world brushes only (selection order),
    // else the last entity in the global list is the imported one.
    bool        RestampImportedGuids(const std::string& entityGuid, const std::vector<std::string>& brushGuids);
    // Same after one ImportMapText of several entity blocks: the last entityGuids.size() entities (import order). Returns the count.
    size_t      RestampImportedEntities(const std::vector<std::string>& entityGuids, const std::vector<std::vector<std::string>>& brushGuids);
    void        SelectBrushes(const std::vector<RadPtr>& brushes);
    void        DeselectAll();

    // ---- moving geometry in place (MAIN THREAD ONLY) -------------------------------------
    // Translate one brush (and a fixed-size entity's origin) with Brush_Move. No undo, no selection change, map not marked modified.
    bool        MoveBrush(RadPtr brush, const float delta[3]);
    // Set the "unsaved changes" flag (g_modified). MoveBrush and SetBrushPlanes leave it alone (previews). No-op in APE.
    void        MarkMapModified();
    // Brush GUIDs -> brush_t in one walk (selected brushes are unlinked, not found). out[i] = nullptr when missing. Returns the found count.
    size_t      FindBrushesByGuid(const std::vector<std::string>& guids, std::vector<RadPtr>& out);
    bool        BrushBounds(RadPtr brush, float mins[3], float maxs[3]);
    size_t      FindEntitiesByGuid(const std::vector<std::string>& guids, std::vector<RadPtr>& out);   // same, over the entity list
    // Three plane points per face with a winding (classic brushes only, 0 for mesh/curve), 9 floats per face. Returns the face
    // count (may exceed capFaces). SetBrushPlanes writes them back (same count) and rebuilds the brush like Brush_Move (no undo).
    size_t      BrushPlanes(RadPtr brush, float* out, size_t capFaces);
    bool        SetBrushPlanes(RadPtr brush, const float* pts, size_t nFaces);
    // A translation or classic-brush plane set as one undoable edit that marks the map modified
    bool        CommitBrushGeometry(RadPtr brush, const float* pts, size_t nFaces, const float delta[3], const char* undoLabel);
    bool        EntityFixedSize(RadPtr ent);              // eclass fixed-size flag: point entities, models, prefabs (the one brush is never written)
    RadPtr      BrushOfInst(RadPtr inst);                 // BrushInst -> brush_t
    // The writer skips unlinked (selected) brushes: this links `extraBrushes` for one Entity_Write call, then restores them.
    bool        WriteEntityTextWith(RadPtr ent, const std::vector<RadPtr>& extraBrushes, std::string& out);

    // ---- prefabs (MAIN THREAD ONLY) ---------------------------------------------------
    int  PrefabDepth();                                   // 0 = editing the top-level map
    // A key's value read straight from entity_t+304 (Entity_Write on an outer-scope entity crashes inside a prefab)
    bool EntityKeyValue(RadPtr ent, const char* key, std::string& out);
    bool PrefabStackPlacement(int level, float origin[3], float angles[3]);   // level 1..PrefabDepth(): origin/angles of the misc_prefab entered at that level (missing keys 0)
    // timings from host hooks a plugin must not detour again
    int  PerformanceProfile(int action, ModToolsHostPerfSnapshot* out);
    bool SelectEntityByGuid(const std::string& entityGuid);   // selects all brush instances (point entities: the empty brush)
    bool EnterSelectedPrefab();                           // "Enter Prefab" on the selection, true if the depth grew
    bool LeavePrefab();                                   // "Leave Prefab", true if the depth shrank

    // ---- patch vertices (MAIN THREAD ONLY, contract: ModToolsHostApi::PatchGrid) ----
    RadPtr PatchBrushByGuid(const std::string& guid, bool selectedOnly);   // selected list first, then the entity lists
    int  PatchGrid(RadPtr brush, int* rows, int* cols, float* xyz, int cap);   // rows*cols, 0 = not a patch
    bool PatchSetGrid(RadPtr brush, int rows, int cols, const float* xyz, int count);   // replace control points, resize if needed
    int  PatchSelectVerts(const std::vector<std::pair<RadPtr, int>>& refs);  // (brush, index), empty clears
    int  PatchSelectedVertCount();
    int  PatchWeldSelected(const float xyz[3]);
    int  DragMode();
}
