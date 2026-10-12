# Mod tools address list

Every Radiant and APE function and global MTPlugins knows by name, for the current Steam build of each editor
(identity in `MODTOOLS_REFERENCE.md`). Addresses are as a disassembler shows them (image base `0x140000000`):
pass them to `RadiantAddress` or `ApeAddress`. How to hook one safely is in `ADVANCED.md`.

Names come from Treyarch's log texts and id's Q3Radiant where known, else from what the function does.
Every function address below is a real function start (listed in the exe's exception table). This file is
generated from the addresses the host and the shipped plugins use: do not edit it by hand.

## Radiant functions (240)

| Address | Name | Prototype (x64) | Notes |
| ------- | ---- | --------------- | ----- |
| `0x1400F8D90` | `FxLightIcon` | `(ctx, ...)` | the yellow icon model of a light elem, ctx+72 = the elem def |
| `0x1400FB5D0` | `FxDrawSprites` | `(sys, effect, ...10 args)` | FX_Draw's per-effect sprites, called before kFxDrawEffect |
| `0x1400FB8C0` | `FxDrawEffect` | `(sys, effect, ...12 args)` | FX_Draw's per-effect draw, effect +92 origin, +160 def handle |
| `0x1401035F0` | `MaterialProp` |  | property of a material handle (map lookup, null -> read at +336) |
| `0x14010A420` | `DrawIndexed` |  | Gfx DrawIndexed wrapper: (indexCount, startIndex, baseVertex) |
| `0x14013BF10` | `FxLoadEfx` | `(const char* "dir/name", def)` | parses fx/<name>.efx into the def, 0 when missing |
| `0x14013C020` | `FxRegisterDef` | `(int* out, def, char reload)` | compiles and registers the def, *out = handle (the name's string id), 0 on failure |
| `0x140159060` | `FxKillOne` | `(sys, effect)` | kills one effect and its children, the kill-all's per-effect call |
| `0x140160490` | `FxEffectPtr` | `(int client, unsigned effect)` | the live effect or NULL |
| `0x1401613A0` | `FxSetCamera` | `(int client, int which, const float* view)` | the camera a step culls spawns against, +148 = plane count |
| `0x140162E90` | `FxChildSpawns` | `(def, cap)` | spawn count of a child def: the step reads it without a null check |
| `0x140167F00` | `FxEffectUpdate` | `(sys, effect, float)` | one effect from effect+200 (last update ms) to sys+2292, origin at effect+92 |
| `0x1401A8A00` | `SceneEarlyGenericSceneCallback` |  |  |
| `0x1401D9CF0` | `AnimSliderCtor` | `(this, QWidget* parent)` | the Animation window's QSlider subclass (128 bytes, 10 px font) |
| `0x1401DC540` | `BrushCache_ModelDispatcher` |  | Fixed-size misc_model category callback (generic and model-specific cull checks) |
| `0x1401DEB00` | `Brush_Build` | `(brush_t*, bool snap, bool markModified, uint flags)` | rebuilds windings and bounds |
| `0x1401DED20` | `Brush_BuildWindings` |  | recomputes brush+64 mins / +76 maxs (+88/+100 padded copy) |
| `0x1401DFEF0` | `Brush_Create` |  |  |
| `0x1401E1540` | `Brush_Move` | `(brush_t*, const float delta[3], uchar snap, uchar markModified)` | also moves a fixed-size entity's origin |
| `0x1401E3FE0` | `Brush_Parse` | `(parseSession, int, char)` | reads guid, mesh/curve/patch bodies |
| `0x1401E6590` | `Brush_SnapToGrid` |  |  |
| `0x1401E8800` | `Brush_RebuildBrush` | `(brush_t*, mins, maxs)` | six faces from a box |
| `0x1401E8FE0` | `BrushInst_VisibilityCull` | `(BrushInst*, bool)` | Nested XMODEL BrushInst visibility test |
| `0x1401E9840` | `FaceSetTexture` |  | copies a texdef onto a face (+0x78 = material, +0x20 = name) |
| `0x1401EBF00` | `Brush_Deselect` | `Brush_Deselect(brush_t*)` | asserts (b->brushFlags & (1 << 7)) |
| `0x1401ED4D0` | `FaceGather` |  | point gather over face-plane triples, only caller Brush_BuildWindings 0x1401DED20 |
| `0x1401F3C50` | `PrefabDraw` |  | draws a prefab instance's interior: (mode, entity, float[12] parent, list, drawState, a6) |
| `0x1401F58D0` | `SceneEarlySpecialRender` |  |  |
| `0x1401F87E0` | `SceneEarlyGeneralRender` |  |  |
| `0x140204A80` | `SetKeyValue` | `(entity_t*, const std::string& key, const std::string& value)` |  |
| `0x1402124B0` | `BrushCacheUpd` |  | per-frame brush cache update |
| `0x1402131D0` | `BrushCache_ProcessRadBrushes` |  |  |
| `0x140213380` | `BrushCache_Prepare` |  | ProcessRadBrushes walks six vectors of 16-byte category entries after Prepare, an entry's first pointer = the BrushInst |
| `0x140230FF0` | `CamMouseMotion` |  |  |
| `0x1402316D0` | `Cam_GetFov` |  | float Cam_GetFov (thunk sub_14000F182): horizontal FOV of a 16:9 view, used by every projection and pick. Editor view: Fov pref (0x1453A1D88), game view 80.67 (cg_fov 65, 4:3), probe view 90. RadiantPlus detours it |
| `0x140231770` | `ViewSetup` | `(CCamWnd*, float out[14])` | the view the fx step and Cam_Draw use |
| `0x140231930` | `BrushCache_GenericCull` |  | Early generic brush culler: tests brush_t's padded bounds (+88/+100) before the model renderer |
| `0x140232270` | `CamRoutineProcessing` |  | steps the bake once when +232 == 4 |
| `0x140238300` | `CamReloadAssets` |  |  |
| `0x14023A540` | `CamOnPaint` |  |  |
| `0x14023ACF0` | `ExposureAdapt` | `(cam, ...)` | auto exposure, pops the readback list and smooths cam+3712..3720 |
| `0x140243120` | `CamProcessAllBrushes` |  |  |
| `0x140244120` | `CCamWnd::Cam_Draw` | `(CCamWnd* this, ...)` | 3D view frame |
| `0x1402488A0` | `CCamWnd_DrawHud` |  | Cam view HUD text, called from CCamWnd::OnRender after the 3D scene: (CCamWnd* this, ?, int w, int h) |
| `0x14024F080` | `CCamWnd_OnRender` |  | the whole 3D view frame incl. HUD |
| `0x14025E770` | `SceneEarlyBrushInstCallback` |  |  |
| `0x1402A95E0` | `ReadbackPopReady` |  | same, null when the oldest is not ready yet |
| `0x1402A9AD0` | `ReadbackPopWait` | `(list, shared_ptr* out)` | pops the oldest readback, waiting for the GPU |
| `0x1402AE780` | `OutlineLambda` |  | Cam_Draw selection pass, per selected BrushInst: (lambda*, BrushInst**) -> bool |
| `0x1402AEE10` | `SceneEarlyRadiantBrushCallback` |  |  |
| `0x1402AEF10` | `SceneEarlyVolumeDecalCallback` |  |  |
| `0x1402BC480` | `HandleMapClear` | `(manipulator + 144)` | empties the handle map |
| `0x1402E85B0` | `ManipulatorDraw` | `(manipulator*)` | draws Radiant's axis handles at the pivot, once per 3D frame |
| `0x1402E9560` | `HandleLabelDraw` | `(manipulator*)` | the X/Y/Z letters at the handle tips |
| `0x1402F31D0` | `CCamWnd_BuildLighting` | `void (CCamWnd*, bool rebuildSun, bool rebuildProbes)` | the bolt and its Rebuild menu |
| `0x1402F38B0` | `CCamWnd::UpdateGI` |  | CCamWnd::UpdateGI(CCamWnd*, char tickMode), the export loop calls it with 0 |
| `0x1402F69E0` | `BakeStart` | `void(CCamWnd*, char incremental, char sun, uchar probes, char)` | every build starts here |
| `0x140302410` | `SmoothTask` |  | state 5 "Probe Smooth Postprocess" std::async callable: char(capture*), 0 = skipped |
| `0x14035A9B0` | `LightStateUpdate` | `(const float* camPos, ...)` | sun volumes at camPos, a changed one starts a timed light state blend |
| `0x1403A91A0` | `DebugLists` | `(a, b, c)` | Cam_Draw's two persistent primitive lists (0x14448FC80, 0x14448FCD0) |
| `0x140420920` | `TreeModelClear` |  | asset tree source model: removeRows(0, rowCount) |
| `0x14042C300` | `TreeSetSelected` |  | asset tree proxy: store the selected set, then invalidateFilter |
| `0x1404305C0` | `TreePaneRefresh` |  | asset tree pane: selection broadcast -> rebuild the model, filter the proxy |
| `0x140432AE0` | `TreeSelectionSlot` |  | asset tree pane: tree selection changed -> select in the map, broadcast |
| `0x1404632C0` | `EdHolderCtor` | `(holder)` | the 599544-byte editor def holder an fx class keeps at +400: int handle, then the def at +8 |
| `0x140472F00` | `PlaybackStopAll` |  | FX playback's stop-all (Stop Effect, Retrigger FX): kill-all 0x14015A540 on client 0 |
| `0x1404732E0` | `PlaybackUpdate` | `(mgr)` | EffectPlaybackMgr::Update, advances the fx clock by now - mgr+40 |
| `0x1404DD740` | `ViewDrop` | `(bridge + 1960, view 0 = camera, x, y from the top left, const QMimeData*)` | bool: a browser drop, the camera traces the cursor ray |
| `0x1405944A0` | `KeyTest` |  | expression leaf 5: entity key/value test (brush_t*, node*) |
| `0x140594BF0` | `NameMatch` |  | pattern test (pattern*, const char* name) |
| `0x1405B28C0` | `FxRelease` | `(unsigned effect)` | drops the spawner's reference and the effect's own, the slot is freed |
| `0x1405B29C0` | `FxKill` | `(unsigned effect)` | kills the effect and its children, the references stay |
| `0x1405B2BD0` | `FxSpawn` | `(int def, const float origin[3], const float axis[9], int delayMs, float unused)` | effect handle, 0 = failed |
| `0x1405B3060` | `FxDrawBegin` |  | enters the fx critical section 0x1444B1480 and empties the icon map 0x1444A73E8 |
| `0x1405B3120` | `FxDrawEnd` |  | empties the fx light vectors 0x1444A73A0/B8/D0 and leaves the section |
| `0x1405B5740` | `FxLoadLights` | `(const char* name, holder)` | the dynamicLight blocks of the text into the holder |
| `0x1405B5830` | `FxFixSegments` | `(holder)` | segment light names after a load |
| `0x140620F90` | `RadMetaObject` |  | QRadFileDialog::metaObject |
| `0x14064B4C0` | `RadiantMainWin_addDockWidget` | `(QMainWindow* this, Qt::DockWidgetArea (unused), QRadDockWidget*)` | rejects QDialogs, reparents into the container |
| `0x14064BC40` | `MainStatusUpdater` |  |  |
| `0x14064BFB0` | `RadiantMainWin_RoutineProcessing` | `(RadiantMainWin* this)` |  |
| `0x14064C2B0` | `MainUpdateWindows` |  |  |
| `0x14064C540` | `LedHandler` |  |  |
| `0x14064E5C0` | `ExportLightingCmd` |  |  |
| `0x1406527C0` | `PrefsSlot` |  | Settings > Preferences... slot (ctor, load, exec, OK tail) |
| `0x14065C450` | `FilesModifiedCheck` | `FilesModifiedCheck(this)` | "Files Modified" Yes/No box, Yes reloads the map. Only runs when Perforce is enabled |
| `0x1406F6A80` | `BeginParseSession` | `(const char* name)` / `` | around an import |
| `0x14077F1E0` | `OutdoorUpdate` | `(scene, int, char)` | outdoor map for the camera's volumes, rebuilt whole when they change, reset when none |
| `0x14078E1D0` | `Patch_Resize` | `void (patch, int rows, int cols)` | the control array may move (read its pointer again), remaps the vertex selection by index |
| `0x1407956A0` | `Patch_InsertDelete` | `(insert, column, first)` | over the selected brushes (menu wrappers 0x140655DB0..EA0) |
| `0x14079C450` | `Patch_ConnectVerts` |  | void Patch_ConnectVerts(char): the W handler's body in Drag Vertices mode (0x140650860 checks the drag mode first). Flag 0, candidates > 0: every selected vertex copies candidate[0], undo "connect vertices", both counts cleared |
| `0x1407A49C0` | `Patch_RebuildMesh` | `__int64 (patch)` | re-tessellates the render mesh from the control net |
| `0x1407BF640` | `Patch_ConnectToCandidate` |  | what the dispatcher calls in that case (not called directly) |
| `0x1408097A0` | `EntityIsPropType` |  | Entity Info: "is_prop_type" of the selected entity's definition |
| `0x140815740` | `QRadColorSelectDialog` | `QRadColorSelectDialog(this, const QColor* initial, ...)` |  |
| `0x1408165F0` | `Dtor` |  | complete dtor, the deleting dtor 0x1408181F0 calls it too |
| `0x140816700` | `GetColor` |  | QColor* (QColor* ret, const QColor* initial, ...): construct, exec, destruct |
| `0x140816DD0` | `Refresh` |  | rebuilds the pixmaps, +304 and the spin boxes from h, s, v, a |
| `0x140817400` | `Accept` |  | accept slot: pushes +304 onto the recent row |
| `0x1408186D0` | `QRadDockWidget_ctor` |  | QRadDockWidget::QRadDockWidget(this, const QString& title, QObject* bridge): TADockWidget ctor (toggle QAction at +64, unique name at +72, content slot at +80), then +104 = bridge->mainWindow (bridge+568), +112 = bridge |
| `0x14081E5D0` | `ToolboxToggleFav` | `(toolbox, QStandardItem*)` | Toggle Favorite, the tree row and QSettings <name>/Favorites |
| `0x140821160` | `ToolboxFindItem` | `(QStandardItem* root, const QString& name)` | QStandardItem*, depth first by data(256) |
| `0x140858AB0` | `QtEntityInfo::EntitySelectionChanged` |  | QtEntityInfo::EntitySelectionChanged(this) |
| `0x1408964C0` | `ToggleGameView` | `(QtFilterWindow*)` | flips the box and both flags, refilters |
| `0x140930A20` | `ExportDialogCtor` |  | QtLightingExportDialog*(this, QWidget* parent) |
| `0x140930C40` | `ExportDialogOpts` | `bool* (dialog, bool out[5])` | after exec, on OK only |
| `0x1409576C0` | `QtRadiantPreferences::QtRadiantPreferences` |  | QtRadiantPreferences::QtRadiantPreferences(this, QWidget* parent), Ui at +0x38 |
| `0x1409578E0` | `QtRadiantPreferences::Init` |  | QtRadiantPreferences::Init(this): every tab's widgets from the prefs |
| `0x140958B60` | `ColorTabLoad` | `(dialog, prefs)` | each Color swatch <- table[index] (8 bits per channel) |
| `0x140959D10` | `ColorTabStore` | `(dialog)` | table[index] <- each Color swatch (OK/Apply) |
| `0x1409B5ED0` | `BeamReloadAll` |  | under the SRW lock below |
| `0x1409BBAC0` | `GetRenderOpt` | `bool(int index)` | bool, bit 1 = Fx > Fx Rendering Enabled |
| `0x1409BC2B0` | `SetRenderOpt` | `void(int index, char on)` | Radiant's render options bit array 0x145215AC0 (locked) |
| `0x140B00BB0` | `BrushCollection_ForEach` | `(collection, std::function<bool(BrushInst*)>*)` |  |
| `0x140B00FA0` | `Undo_AddBrushList` | `(BrushCollection*)` | records the selection in the open operation |
| `0x140B00FF0` | `Selection_CloseUndo` | `(BrushCollection*)` | after Undo_AddBrushList |
| `0x140B01070` | `Selection_EndBatch` | `(BrushCollection*, int 3, __int64 0, char 1)` | fires the change event |
| `0x140B01DB0` | `Select_Brush` | `(BrushCollection*, BrushInst*)` | takes a `BrushInst`, never a `brush_t` (crashes) |
| `0x140B4A6D0` | `ModelSetSize` |  | the browser's SQL model setIconSize(QSize): size at +32, drops the thumbnails (vtable +384), grey placeholder |
| `0x140B4C430` | `UpdateSearch` |  | ThumbnailBrowser::UpdateSearch (vtable +472) |
| `0x140B4C8A0` | `ThumbnailBrowser::setIconSize` |  | ThumbnailBrowser::setIconSize(int): view icon and grid size, QSettings, vtable +472 |
| `0x140B59CC0` | `CachedLines` | `(cam + 0xEF8)` | Cam_Draw's pre-built line sets, editor view only |
| `0x140B894B0` | `Malloc` | `void*(size_t)` | the exe's allocator |
| `0x140B894F0` | `Free` | `void(void*)` |  |
| `0x140B89520` | `BlockMalloc` |  | block data (the store clear frees it with 0x140B89560) |
| `0x140C164D0` | `Undo_Begin` | `(const char* label)` | keeps the label pointer: it must stay valid |
| `0x140C16520` | `Undo_End` |  | marks the map modified, relabels Edit > Undo |
| `0x140C16660` | `Undo_AddBrush` |  |  |
| `0x140C168D0` | `UndoAddEntity` | `(entity_t*)` |  |
| `0x140C16ED0` | `Undo_IsOpen` | `bool` | the top operation is still open |
| `0x140C17B40` | `Undo_Start` |  |  |
| `0x140C181D0` | `Undo_Undo` | `(undoList, redoList, undoId, redoId)` | Edit > Undo and Redo both end here |
| `0x140C2B300` | `QtXYOnRender` |  |  |
| `0x140C2D5F0` | `XYProcessAllBrushes` |  |  |
| `0x140C2E3E0` | `CXYWnd::XY_Draw` | `(CXYWnd* this, __int64* viewport)` | 2D view frame |
| `0x140C2F4D0` | `BrushCache_AlternateCull` |  | Alternate fixed-size brush culler for the normal camera context, also reads the padded bounds |
| `0x140C3F6B0` | `GridCullXYBrushCallback` |  |  |
| `0x140C5CCC0` | `MoveSelection` |  | the drag handler that calls Select_Move for a plain translate |
| `0x140C6C250` | `DeepPrefabLoop` |  |  |
| `0x140C8C830` | `Entity_Parse` | `(parseSession, ...)` | allocates an `entity_t` |
| `0x140C8ED90` | `ComposeTransform` | `(entity, float[12] parent, float[12] out)` | the instance's transform |
| `0x140C8FCF0` | `Entity_Write` | `(entity_t*, FILE*, char selectedOnly)` | `{ guid ... keys ... brushes }`; the `FILE*` must come from MSVCR110 |
| `0x140C911B0` | `Entity_GuidString` | `(entity_t*, std::string* out)` | StringFromGUID2 of entity+352 |
| `0x140C92110` | `Entity_UpdateOrigin` | `(entity_t*)` | "origin" key from entity+280, targetname bookkeeping |
| `0x140CA5690` | `Entity_AddToList` | `(list sentinel, entity_t*)` |  |
| `0x140CD1760` | `Map_LoadFile` | `__int64 (char* filename)` | no unsaved-changes prompt (the callers ask); leaves `<map>.bak` next to the map |
| `0x140CD2890` | `Map_SaveFile` | `char (char* filename, char silentAutosave, char saveSelected)` | NULL or empty name opens the Save dialog |
| `0x140CD2EA0` | `Map_New` |  |  |
| `0x140CD3E60` | `Map_ImportBuffer` | `__int64 (char** text, uint flags, uchar regenerateGuids)` | the paste importer, see "Importing" |
| `0x140CD5E30` | `Map_LeavePrefab` | `(char updateTitle)` | pops the prefab stack (g_prefabStack), does not write the prefab file |
| `0x140CD81B0` | `EntityRegistry_Add` | `(EntityInst*)` | see "Entity registry" |
| `0x140CD82F0` | `EntityRegistry_Remove` | `(EntityInst*)` | see "Entity registry" |
| `0x140CDB0C0` | `Map_Parse` | `int (const char* file, void* entityList, uchar flag)` | returns the entity count |
| `0x140CDBEB0` | `Map_Write` | `__int64 (const char* file)` | the `.map` writer behind `Map_SaveFile` |
| `0x140CDF5A0` | `Map_EnterPrefab` | `(EntityInst* prefabOrNull, char showAll, char flag)` | NULL = the single selected `misc_prefab` |
| `0x140D02320` | `Select_Delete` |  | deletes everything selected |
| `0x140D023E0` | `Select_Deselect` | `(char clearDrag)` |  |
| `0x140D02700` | `Select_Move` | `(const float delta[3], bool snap)` | `Brush_Move` on every selected brush |
| `0x140D03F70` | `SelectRotateAxis` | `(int axis, float deg, char paint, char mouse, uchar, char axisRelative)` |  |
| `0x140D33210` | `SysPrintf` | `(const char* fmt, ...)` | "HH:MM:SS: " + text to Radiant's log file and the Output pane |
| `0x140D33530` | `Sys_SetTitle` | `void (const char* name)` | window title |
| `0x140D337C0` | `QuestionBox` | `(parent, text, title, buttons, icon)` | buttons 0x14000 = Yes\|No, returns 0x4000 Yes, 0x10000 No. Map_LoadFile and Map_New ask "Lighting changed..." through it (thunk 0x14006CA76), the host answers No on its own reloads |
| `0x140D35340` | `PrepareForMapLoad` |  | char PrepareForMapLoad: leaves any open prefab (Map_LeavePrefab(1)) after a modified check, 0 = abort |
| `0x140D3C9F0` | `EndParseSession` | `(const char* name)` / `` | around an import |
| `0x140D46D40` | `Bookmarks_Save` |  | char Bookmarks_Save(void* bookmarks, const char* mapName): writes Bookmark0..9 + LastUsed to QSettings |
| `0x140E88F80` | `TADockWidget_setWidget` | `(this, QWidget* orNull)` | adds it to the dock's layout, null = first child widget |
| `0x140E89040` | `TADockWidget::toggleView` |  | TADockWidget::toggleView(this, bool show): show/raise a pane (tests) |
| `0x140E8AEC0` | `RadiantMainWin_DockContainer` | `(QMainWindow*)` | the TADockContainer central widget, null until the UI exists |
| `0x140E920C0` | `UnLz4` | `int(src, dst, compressed, maxOut)` | LZ4_decompress_safe |
| `0x140E95120` | `Lz4` | `int(src, dst, size)` | LZ4_compress, dst >= size + size/255 + 16 |
| `0x140EB52F0` | `Simplify` |  | tfbDecimate simplifier, the linker's 0x7AE170 |
| `0x140F28F90` | `Find` |  | the finder |
| `0x140F35CF0` | `AssertHandler` | `(file, line, kind, expr, fmt, ...)` | nonzero = continue, thunk 0x14000C1C6 |
| `0x140F427B0` | `Inverse` | `void(const float M[16], float Minv[16])` | row-major, no conditioning check |
| `0x140F93C40` | `EndsWith` |  | bool ends_with(const std::string* s, const std::string* suffix), thunk 0x14008908B |
| `0x140FA0300` | `DeepAssetCacheOpen` |  |  |
| `0x140FA2FE0` | `DeepAssetCacheDecomp` |  |  |
| `0x140FAEFE0` | `LensView` |  | view* (cam, view* out, float focal, float focusDist): game view projection |
| `0x14100E990` | `GpuStatsQuery` |  | expensive D3DKMTQueryStatistics walker used by DrawHud |
| `0x141042190` | `DeepGeoGrow` |  |  |
| `0x1410423C0` | `CensusGeoCopy` |  |  |
| `0x1410985D0` | `DeepInstanceCache` |  |  |
| `0x1410AD280` | `PoolB` |  |  |
| `0x141101FE0` | `RenderPostFx` |  | bloom/exposure/lens/tonemap/SMAA/etc |
| `0x141119CC0` | `ProbeAccumulatorInit` |  | acc*(acc*, const float ref[3]) |
| `0x141119DD0` | `ProbeAccumulateRadiance` | `(acc*, dir*, rgb*, basisA[6]*, basisB[6]*, float weight)` |  |
| `0x14111A6F0` | `ProbeAccumulatorFinalize` |  |  |
| `0x14111AB50` | `AccFinal` | `void(acc*, char debug)` |  |
| `0x14111AB80` | `Merge` | `float(acc* a, acc* b)` | blends b into a, returns the max relative difference |
| `0x1411202C0` | `StoreClear` | `void(store*)` | frees the blocks and the unpacked +1552/+1576 |
| `0x141121E30` | `RayParams` | `void(probe*)` | +10176..+10255 from the probe's quality and bounds |
| `0x141122D70` | `VoxelTrace` |  | per-voxel cache trace: (probe, uint voxel, scene*, float m[9]) |
| `0x1411232B0` | `Trace32` |  | trace 32 rays from index start: (probe, voxel, start, set, scene*, uint*, uint*, uint*, float acc[12]*, float m[9]) |
| `0x1411237B0` | `TracePass` | `void(probe*, SceneObj** scene, int quality, char grid, int* debugVoxel, char freeSpace)` |  |
| `0x141125CB0` | `ProbeGather` |  | per-probe bounce job: void(probe, dims*, volumes*, char, char, char, char init, scene*, char adaptive, qword, int*, int) |
| `0x141129C20` | `BuildBasis` | `void` | the two basis tables, from the directions |
| `0x141129EC0` | `DirLookup` | `const float* (int quality, uint set, uint index)` | gUniformDirs entry (probe-local) |
| `0x141129F60` | `ProbeSampleDirection` | `float* (float* out, uint quality, uint set, uint index, float m[9])` | rotated direction |
| `0x14112A090` | `BuildDirs` | `void` | fills gUniformDirs for the four ray counts (halves ordered for 32-ray packets) |
| `0x14112F630` | `HitFixup` |  | per-voxel pass after the trace: resolves each hit's lighting volume (flag bits 0-3) and |
| `0x141131670` | `ProbeGatherVoxel` |  | per-voxel gather from the hit cache: int64(ctx*, uint voxel), ctx[1] = probe |
| `0x141131D20` | `ProbeGatherAdaptiveVoxel` |  | adaptive re-gather of unconverged voxels (observed only) |
| `0x141132CC0` | `VoxelFinal` |  | last per-voxel stage of the gather job, after gather, adaptive and free-space fill |
| `0x1411334F0` | `ProbeGatherCachedSample` |  |  |
| `0x1411341D0` | `int64` | `int64(ctx*)` | one probe's trace task (pass 0x1411237B0 inside) |
| `0x141165750` | `BlockResize` |  | vector<Block>::resize(vec*, n), value-initialised |
| `0x14116C230` | `CaptureBegin` | `char(int w, int h)` | starts the probe debugger's voxel capture (global buffers, hMutex) |
| `0x141174B20` | `ProbeImportanceSample` |  |  |
| `0x14117C860` | `BuildList` |  |  |
| `0x14117C9A0` | `Fill` |  |  |
| `0x141183B30` | `ProbeReset` | `void(owner*, char full)` | per probe at a build's start, full drops its trace |
| `0x1411863C0` | `PoolA` |  | per-frame resource pool upkeep |
| `0x141191030` | `BasisA` | `void(float x, float y, float z, float out[6])` |  |
| `0x1411A19C0` | `EmbreeLoad` |  | loads (avx\\)embree64r.dll |
| `0x1411A5420` | `TraceRayPackets8` |  |  |
| `0x1411AED10` | `ViewSet` | `(int view)` | bool: makes `view` current when none is (the camera is view 0) |
| `0x1411AEDA0` | `DxgiPresent` |  |  |
| `0x1411AEEA0` | `ViewTarget` | `(int view)` | the colour target of the view |
| `0x1411AEFF0` | `ViewDepth` | `(int view)` | its depth target |
| `0x1411B8AE0` | `RenderGBuffer` |  | prepass + GBuffer opaque/semi/decal |
| `0x1411B8FC0` | `RenderMainPipeline` |  | deferred/forward lighting, transparency, volumetrics, etc |
| `0x1411BAF00` | `DeepRenderPassDispatch` |  |  |
| `0x1411BB0E0` | `V722SpecialStageSubmit` |  |  |
| `0x1411BB860` | `ModelWire` |  | camera model wireframe: (model, int, RGBA bytes*, drawState*, lod, -1, flags, flags) |
| `0x1411CDBB0` | `DeepSortA` |  |  |
| `0x1411CDDE0` | `DeepSortB` |  |  |
| `0x1411E1FE0` | `DrawText2D` | `(text, Font**, pos, xAdvance, yDir, rgba, outEnd, uint8 backgroundAlpha)` | void DrawText2D(const char* text, Font** font, const float pos[3], const float xAdvance[3], const float yDir[3], const float rgba[4], float outEnd[3] (nullable), uint8 backgroundAlpha) (thunk sub_14008A265) pos = baseline start. backgroundAlpha > 0 draws a black box first. Refuses fonts whose material (+0x48) is null |
| `0x1411E6670` | `DeepGeoSubmit` |  |  |
| `0x1411E7710` | `BeginFrame` |  | bool, 0 when a frame is already open |
| `0x1411E7760` | `EndFrame` |  |  |
| `0x1411E8920` | `CensusCoalesceRange` |  |  |
| `0x1411EC3A0` | `CensusCoalesceOne` |  |  |
| `0x1411EC7C0` | `V723GeoResolveMesh` |  |  |
| `0x141259200` | `SetTargets` | `(colour, depth, 0, 0)` |  |
| `0x141259590` | `Clear` | `(char flags 7, const float rgba[4], float depth)` |  |
| `0x1412CAE30` | `Reduce` | `(out, model**, lodSpec*, ?, error string*)` | one auto LOD |
| `0x1412CBF70` | `CacheName` |  | "ToolsGfx/xmeshes/v39/<mesh>/pct%d_<key>" |
| `0x1412CBFE0` | `CacheRead` |  | "ToolsGfx/xmeshes/v39/<mesh>/pct%d_<key>" |
| `0x1412CC720` | `CacheWrite` |  | "ToolsGfx/xmeshes/v39/<mesh>/pct%d_<key>" |
| `0x1412F15D0` | `Lookup` | `int (table*, Qt::MouseButton, Qt::KeyboardModifiers)` | action id, 0 = none |

## Radiant globals (98)

| Address | Name | Notes |
| ------- | ---- | ----- |
| `0x1422A5BE8` | `BrushFileInfo` | assert "file" record of the brush module, line 0x7E = more than 64 faces |
| `0x142310788` | `PlaybackMgrVtbl` | EffectPlaybackMgr's vtable, checked before the manager is trusted |
| `0x142466D00` | `ToolboxMeta` | QtAbstractToolbox::staticMetaObject: signals RecentItemsUpdated, ItemDoubleClicked, AddItemAtCameraLocation |
| `0x1424FA348` | `Vtbl` |  |
| `0x1424FB138` | `RadDialogVtbl` | QRadFileDialog's vtable, slot 0 = metaObject thunk 0x140011F4A |
| `0x142508438` | `EffectsBrowserVtbl` | QtEffectsBrowser's vtable (RTTI .?AVQtEffectsBrowser@@) |
| `0x142515CC8` | `FilterWindowVtable` | QtFilterWindow::`vftable' |
| `0x142548668` | `PrefabBrowserVtbl` | QtPrefabBrowser's vtable (COL ??_R4QtPrefabBrowser@@6B@ at -8) |
| `0x14264DA20` | `str_LightingChanged` | the text of that question (.rdata) |
| `0x1426B3C98` | `V6AssetBlobVtable` |  |
| `0x142724398` | `Thresholds` | float[]: adaptive thresholds, index probe+0x88 - 1 |
| `0x14272A220` | `Offsets` | int32[124][3]: 0..25 the 3x3x3 shell, 26..123 the radius-2 shell |
| `0x14331F720` | `RadStaticMeta` | QRadFileDialog::staticMetaObject (no methods, no properties) |
| `0x14331FA58` | `RadiantQtBridge_staticMetaObject` |  |
| `0x143370458` | `RotateStepTable` | int[10]: rotation step in degrees per grid index (0x140C27F70, 90 past the table) |
| `0x14338F558` | `V63AssetCacheLocalRoot` | std::string local asset-cache directory |
| `0x1433B5740` | `ViewIndex` | int, the current view, 7 = none (Present sets it back) |
| `0x1443900B0` | `FxSystemCount` | int, local fx clients |
| `0x1443900B8` | `FxSystemPtr` | client 0's fx system, +2292 = fx time ms |
| `0x1443B0E68` | `FpsDeepJqProfActive` | nonzero => built-in JQPROF event capture active |
| `0x1444877C0` | `g_BrushCacheArena` | Default camera pass cache: arena of 208-byte BrushInst records + vector of { arenaIndex, passMask } pairs (ProcessRadBrushes(1, ...) iterates it, not the six typed vectors) |
| `0x1444877E0` | `g_BrushCacheRootEntries` |  |
| `0x144487804` | `SpatialBrushDirtyFlag` |  |
| `0x144487830` | `g_BrushCacheCategories` | six { begin, end, capacity } vectors of 16-byte { record*, mask } |
| `0x1444A73E8` | `FxIconMap` | std::map<def, shared_ptr<Light>> head pointer, size at +8: last frame's fx lights |
| `0x1444C6560` | `main window` | RadiantMainWin* RadiantMainWin* (mainwin+64 -> QE children, children+240 -> CCamWnd*) |
| `0x1444C656C` | `redraw flags` | int OR in bits to mark views dirty for the next tick |
| `0x1444C6578` | `FrameBudget` | double, Radiant's limiter: frames it may still ask for |
| `0x145214C70` | `Recent` | static QColor[10], the recent row |
| `0x1452158C8` | `BeamLock` | SRWLOCK the reload holds while it walks the beam map |
| `0x145215AC0` | `FpsDeepInputMask` | 10-byte internal key/button state snapshot |
| `0x145216980` | `FpsDeepJqProfTrigger` | built-in short capture trigger |
| `0x145216990` | `IdleTimerPtr` | QTimer* driving RadiantMainWin::RoutineProcessing |
| `0x145216998` | `ExitCode` | int, written by 0x140B899A0 (255 on any failed map, else 0) |
| `0x145216CB1` | `RotateMode` | byte: Mouse Rotate mode is on (CXYWnd::RotateMode) |
| `0x145216CD8` | `RotateOrigin` | float[3]: the pivot (CXYWnd::RotateOrigin), set when the mode is entered |
| `0x14521EB90` | `saving` | char set while `Map_SaveFile` runs |
| `0x14521EB91` | `g_importKeepLinks` | char: "copy the selection into the next map" flag (New/Open path), cleared after its paste. While set, Map_ImportBuffer skips its target/linkto remap, which otherwise deletes every target and linkto key of the imported text |
| `0x14521EB94` | `modified` | int 0 clean, 1 unsaved changes, 2 after save-selected |
| `0x14521EB98` | `g_prefabModified` | int: set when the prefab being edited changed (instances rebuilt on leave) |
| `0x14521EB9C` | `prefab depth` | int 0 = the map itself, up to 16 levels of Enter Prefab |
| `0x14521ED20` | `current map name` | char[] path of the open map, `"unnamed.map"` when none |
| `0x14521F200` | `g_bookmarks` | camera bookmark table (10 x 28 bytes from +16, LastUsed int at +280) |
| `0x14521F318` | `g_lastBookmark` | int: bookmark index Map_LoadFile moves the camera to (-1 = none) |
| `0x14521F320` | `prefab stack` | 16 x 2936 bytes one entry per level entered |
| `0x14522B130` | `world object` | `+776` = worldspawn's `EntityInst*`; owns the instance list |
| `0x14522B438` | `worldspawn` | EntityInst* worldspawn's instance; its `entity_t` at `+0` |
| `0x14522B440` | `entity list` | list sentinel every entity, worldspawn first |
| `0x14522B6D0` | `entity registry` | map head pointer GUID -> `EntityInst*`, size at `0x14522B6D8` |
| `0x145268194` | `g_entityCount` | int, Map_Parse result |
| `0x1452711F4` | `g_patchVertSelCount` | int |
| `0x1452711F8` | `g_patchVertSel` | float*[]: control record addresses (max 4097 for model welds, 1025 for patch connect) |
| `0x1452E1240` | `g_modelVertCand` | float[3][]: selected model verts, weld candidates |
| `0x145341240` | `g_modelVertCandCount` | int |
| `0x145341248` | `CurrentTexdef` | g_qeglobals.d_texturewin.texdef, the current texture |
| `0x1453A14A8` | `ColorTable` | float[48][4], row 4 = COLOR_CAMERABACK (Cam_Draw clears with it) |
| `0x1453A1768` | `HandleColors` | float[3][4]: X, Y, Z handle colours (rgba, 0..1), view color table rows |
| `0x1453A17D4` | `g_dragMode` | int: 4 = Drag Vertices |
| `0x1453A1810` | `g_brushCount` | int, counted during load |
| `0x1453A1830` | `NoSelOutline` | int, nonzero = J turned the selection pass off |
| `0x1453A1848` | `NowSeconds` | double, the idle tick's clock the manager reads |
| `0x1453A1C20` | `GridIndex` | int: the grid size as an index (first field of the prefs block) |
| `0x1453A1CAC` | `g_camXYUpdate` | int (0/1) |
| `0x1453A1CC4` | `FpsDeepForceCameraA` | both nonzero force camera-dirty path |
| `0x1453A1CD4` | `FpsDeepForceCameraB` |  |
| `0x1453A1D10` | `MaxFpsPref` | float, prefs block + 0xF0, registry Prefs\MaxFPS (0 = no limit) |
| `0x1453A1D24` | `NoGridSnap` | int: nonzero = snap to grid is off |
| `0x1453A1D50` | `g_tolerantWeldFlag` | int: 1 routes Patch_ConnectVerts to the XY-only tolerant welds (read 0 even with the pref on) |
| `0x1453A1D88` | `FovPref` | float: Preferences > Camera > Field of View (non GameView) |
| `0x1453A1EE9` | `g_gameView` | char: 1 = "GAME VIEW" (scene letterboxed to 16:9 inside the cam pane) |
| `0x1453A1EEA` | `g_probeView` | char: 1 = "STATIC PROBE VIEW" |
| `0x1453A1F90` | `MapLoading` | byte: map loading, set and cleared by Map_LoadFile |
| `0x1453A2278` | `selected brushes` | BrushCollection the selection |
| `0x1453A22D8` | `active brushes` | BrushCollection everything not selected |
| `0x1453A23A4` | `BakeQuality` | int 0/1/2: the toolbar's L / M / H, copied to the volume when a build starts |
| `0x1453A23AA` | `LockSunVolume` | byte, the Exposure toolbar's "Lock Sun Volume": the camera keeps its sun volume and light state |
| `0x145413B30` | `FxMapLoading` | byte: FX s_mapIsLoading, set and cleared by Map_LoadFile |
| `0x145573AA0` | `g_debugFont` | Font*: +0x40 int line height, +0x48 material |
| `0x1455740C0` | `DxDevice` | ID3D11Device* |
| `0x145574210` | `DxContext` | ID3D11DeviceContext* (dx.context) |
| `0x14557FEE0` | `V6GeoCacheObject` |  |
| `0x14557FF50` | `V718GeoHashSentinel` |  |
| `0x14557FF60` | `V718GeoHashBuckets` |  |
| `0x14557FF78` | `V718GeoHashMask` |  |
| `0x14557FF80` | `V718GeoHashThreshold` |  |
| `0x145581E54` | `Inflight` | int: probe jobs in flight |
| `0x145581FA0` | `TableB` |  |
| `0x1455822A0` | `DirTables` | std::vector<float[3]>[4 qualities][8 sets] |
| `0x1455828A0` | `TableA` | std::vector<float[6]>[4][8], parallel to the directions |
| `0x145584A78` | `SwapChain0` | IDXGISwapChain* of view 0 (the camera) |
| `0x145585BB0` | `FpsDeepQueueBase` | 10 x 32-byte camera render-stage queues |
| `0x145B0CDE8` | `V8CurrentIndexBinding` |  |
| `0x145B0DBF0` | `DeepDipCounters` | low = DIP submissions, high = logical DIP work |
| `0x145B0DBF8` | `DeepDiipCounters` | low = DIIP submissions, high = instances |
| `0x145B0DC00` | `DeepTriCounter` |  |
| `0x145B0DC08` | `HudFps` | float, the HUD's FPS: smoothed 1000 / frame ms |
| `0x146D31B88` | `ImpEnterCriticalSection` | Radiant's import slot for EnterCriticalSection |
| `0x146D31BF0` | `ImpTryEnterCriticalSection` | TryEnterCriticalSection |

## APE functions (46)

| Address | Name | Prototype (x64) | Notes |
| ------- | ---- | --------------- | ----- |
| `0x140050060` | `std::string::assign` |  | std::string::assign(const char*, size_t) with APE's allocator // selection handler: cmp [child tag size], 0 then jz, no attach without a child tag |
| `0x14005FBC0` | `Intern` | `(uint64_t* out, const char*)` | the interned name handle |
| `0x140062190` | `Field` | `(record, const char** out, const char* key)` | the value, "" when unset |
| `0x14007A040` | `ReloadDeleteAssetDialog` | `(tree*, QVector<apeGdtFileCacheEntry*>* changed, QVector<char*>* deleted files)` |  |
| `0x14007B690` | `Lookup` | `(asset db, const uint64_t* name handle, int type, char 0)` | record or null |
| `0x14007CBC0` | `TypeIndex` | `(asset db, const uint64_t* type handle)` | type index, -1 unknown |
| `0x140084510` | `TimerSlotBegin` |  |  |
| `0x140084730` | `TimerSlotEnd` |  |  |
| `0x14008AFA0` | `QVectorPtr_reallocData` |  | QVector<T*>::reallocData(QVector* v, int size, int alloc, int options); options 0 |
| `0x1400B46C0` | `assetTreeUpdateFilters` | `assetTreeUpdateFilters(apeAssetTreeWidget*)` |  |
| `0x1400C1160` | `Clear` |  | apeAssetWidget clear / reset |
| `0x1400C1500` | `Reset` |  | apeAssetWidget clear / reset |
| `0x1400C9EA0` | `apeColorDialog` | `apeColorDialog(this, QWidget* parent, const QColor* initial, bool alpha)` |  |
| `0x1400CBF10` | `DeletingDtor` |  | vtable slot 3, inlines the complete dtor |
| `0x1400CC980` | `Refresh` | `(this, bool notify)` | pixmaps, +280, spin boxes, notify = live update of the property |
| `0x1400CD0B0` | `Accept` |  | accept slot: recent colors to QSettings, QDialog::accept |
| `0x1400F85F0` | `apeFileCache_Walk` |  | QVector<entry*>* (apeFileCache*, QVector<entry*>* out) |
| `0x1400F8A70` | `apeFileCache_Refresh` | `void (apeFileCache*)` | walk + re-parse + ReloadDeleteAssetDialog, only while the dirty byte is set |
| `0x1400FD330` | `apeFileCache_SaveCache` | `(apeFileCache* this)` | runs once from apeFileCache::Start on the main thread |
| `0x1401073B0` | `LoadImage` | `bool (QPixmap* out, const QString& path)` |  |
| `0x140120990` | `apePreferencesDialog::apePreferencesDialog` |  | apePreferencesDialog::apePreferencesDialog(QWidget* parent) |
| `0x14012A0D0` | `Frame` | `(preview widget*, ?)` | renders one frame, re-queues itself while playing |
| `0x14012C5E0` | `GdtAsset` | `(shared_ptr<GdtAsset>* out, const std::string& name, const std::string& type)` |  |
| `0x14012D3F0` | `RawGet` |  | apeGfxGdtEntry vtable[1]: (entry, std::string* out, const std::string* key) |
| `0x140131EA0` | `Dispatch` | `(apePreviewWidget*, asset record*)` | picks the preview by the asset's type |
| `0x140145740` | `Render` | `(widget data, ...)` | poses, placement and draw, the frame labels follow it |
| `0x140147250` | `Reload` | `(previewWindowOpts*)` | re-reads the lighting states (the Lighting slots call it) |
| `0x140217930` | `Simplify` |  | tfbDecimate simplifier, the linker's 0x7AE170 |
| `0x14021E2F0` | `Reorder` | `(indices, triangles, float weights[3], out, batch)` | called by the surface build |
| `0x140270AA0` | `ReadSsi` | `(const char* ssi name, Ssi* out)` | one ssi record from gdt.db |
| `0x140298180` | `Find` |  | the finder, called by the techset include loader only |
| `0x14031A530` | `EndsWith` |  | bool ends_with(const std::string* s, const std::string* suffix): memcmp of the tail |
| `0x140338A60` | `ViewOf` | `(camera, view out, float fov)` | origin, axes, +48 tan of the half height, +52 of the half width |
| `0x14047EA90` | `SetAnim` | `(holder, const std::string& anim name, int mode)` |  |
| `0x140481070` | `LocalPose` |  | ModelInst local pose from an anim, a8 = the model's bone vector |
| `0x140632170` | `Attach` | `(entries, parent entry, model, parent tag, child tag)` | the Preview Options attach |
| `0x1406328D0` | `Length` | `(entries)` | float: the longest anim of the preview entries in seconds |
| `0x1406331D0` | `Place` | `(entry, ?, bool)` | per-frame placement on the parent's tag, false = not drawn |
| `0x1406368B0` | `Convert` | `(shared_ptr<Anim>* out, const std::string& name, int mode)` | mode 1 = stub |
| `0x140647FD0` | `SetPlacement` | `(holder, const Xform*, float scale)` |  |
| `0x140679C70` | `BuildOne` | `(surfaces*, model**, material, lod)` | one material of one LOD |
| `0x14067AE20` | `BuildSurfaces` | `(surfaces*, model**)` | every LOD x material of the model |
| `0x14067C300` | `Reduce` | `(out, model**, lodSpec*, ?, error string*)` | one auto LOD |
| `0x14067D440` | `CacheName` |  | "ToolsGfx/xmeshes/v39/<mesh>/pct%d_<key>" |
| `0x14067D4B0` | `CacheRead` |  | "ToolsGfx/xmeshes/v39/<mesh>/pct%d_<key>" |
| `0x14067DBF0` | `CacheWrite` |  | "ToolsGfx/xmeshes/v39/<mesh>/pct%d_<key>" |

## APE globals (8)

| Address | Name | Notes |
| ------- | ---- | ----- |
| `0x140810E88` | `Vtbl` |  |
| `0x1408352F0` | `PickerFilter` | the picker's filter string (81 chars, one xref) |
| `0x14118D298` | `TypeSchemas` | global: object whose QList at +0x30 holds one schema per type index |
| `0x14118D4D0` | `apeFileCache_Instance` | the one apeFileCache |
| `0x14118D615` | `RadiantTheme` | bool radiantTheme, read by QT_SetTheme (0x14011BFC0) |
| `0x14118D620` | `Opts` | previewWindowOpts (pointer) |
| `0x14118D978` | `NoValue` | global: object whose char* at +0x18 is what a lookup of a missing key returns |
| `0x14118D9D8` | `ScanThread` | the techsetdef scan QThread (a global object), started by apeMainWindow::Init |
