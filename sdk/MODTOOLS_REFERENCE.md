# Mod tools reference

What is known about the insides of Radiant, APE and the Mod Tools Launcher: the builds, the main functions and
globals, the data layouts and the traps. It goes with `ADVANCED.md` (how to use `RadiantAddress`, `ApeAddress` and
`Detour` safely). Every address is as a disassembler shows it for the build below, and every layout was checked
against the running program. The complete list of addresses the host uses, with comments, is `src/radiant_addrs.h` in MTPlugins' source.


## Radiant

### Binary identity

| What | Value |
| ---- | ----- |
| File | `<game>\bin\Radiant_modtools.exe`, the current Steam build, x64 |
| `TimeDateStamp` | `0x5DF45023` |
| `SizeOfImage` | `0x6D95000` |
| Image base | `0x140000000`, no ASLR: every disassembler address is the runtime address |
| md5 of the untouched file | `483f3acd5157b0f56a6c8177fafb8762` |
| Compiler and runtime | Visual C++ 2012, `MSVCR110.dll` / `MSVCP110.dll` |
| Qt | 5.3.2 (MSVC 2012 build) as `Qt5Core64r.dll`, `Qt5Gui64r.dll`, `Qt5Widgets64r.dll`, ... loaded at run time |
| Rendering | Direct3D 11, one swap chain per view (the `d3d9.dll` import is not used for drawing) |

The host checks the timestamp and size before it installs anything; on another build `RadiantAddress` returns NULL.
Assertion texts carry Treyarch's source paths (`q:\t7\pc\code\tools\radiant\*.cpp`), which tell you which file a
function came from.

### Lineage

Radiant is id Software's Q3Radiant, extended by Treyarch. id's names survive as log and profiler labels
(`Map_LoadFile`, `Map_SaveFile: %s`, `Map_EnterPrefab`, `Undo_*`, `Select_*`, `RadiantMainWin::RoutineProcessing`,
`CCamWnd::Cam_Draw`, `CXYWnd::XY_Draw`), and the public Q3Radiant sources are the best guide to `entity_t`,
`brush_t`, `face_t`, epairs and the load, save and undo flow. Treyarch added GUIDs, layers, patches and meshes, an
instance layer for the selection, and its own docking. Main classes: `RadiantMainWin` (main window), `CCamWnd`
(3D view), `CXYWnd` (2D views), `QtEntityInfo` (Entity Info), `EntityInst`, `BrushCollection`.

### Main thread and idle tick

- Everything that touches the map runs on the main thread.
- `RadiantMainWin::RoutineProcessing` `0x14064BFB0` `(RadiantMainWin* this)` is Radiant's idle tick: it runs
  continuously and is where the host runs plugins' `OnTick` and their posted jobs.
- Radiant's own long operations (asset reloads, message boxes, load dialogs) run nested message loops, so the idle
  tick, and your callbacks, can run while one of Radiant's functions is still on the stack.
- The tick starts with `CCamWnd::ReloadAssets` `0x140238300` `(cam, bool force)`, Radiant's check for changed asset
  files (images, materials, models, FX, ...). `Map_LoadFile` calls it with `force = 1`.

### Main functions

The most useful ones with how to use them. Every function and global MTPlugins knows is in `MODTOOLS_ADDRESSES.md`.

| Function | Address | Prototype (x64) | Notes |
| -------- | ------- | --------------- | ----- |
| `Map_LoadFile` | `0x140CD1760` | `__int64 (char* filename)` | no unsaved-changes prompt (the callers ask); leaves `<map>.bak` next to the map |
| `Map_SaveFile` | `0x140CD2890` | `char (char* filename, char silentAutosave, char saveSelected)` | NULL or empty name opens the Save dialog |
| `Map_New` | `0x140CD2EA0` | | |
| `Map_Parse` | `0x140CDB0C0` | `int (const char* file, void* entityList, uchar flag)` | returns the entity count |
| `Map_Write` | `0x140CDBEB0` | `__int64 (const char* file)` | the `.map` writer behind `Map_SaveFile` |
| `Map_ImportBuffer` | `0x140CD3E60` | `__int64 (char** text, uint flags, uchar regenerateGuids)` | the paste importer, see "Importing" |
| `Map_EnterPrefab` | `0x140CDF5A0` | `(EntityInst* prefabOrNull, char showAll, char flag)` | NULL = the single selected `misc_prefab` |
| `Map_LeavePrefab` | `0x140CD5E30` | `(char updateTitle)` | |
| `Entity_Parse` | `0x140C8C830` | `(parseSession, ...)` | allocates an `entity_t` |
| `Entity_Write` | `0x140C8FCF0` | `(entity_t*, FILE*, char selectedOnly)` | `{ guid ... keys ... brushes }`; the `FILE*` must come from MSVCR110 |
| `Entity_AddToList` | `0x140CA5690` | `(list sentinel, entity_t*)` | |
| `Brush_Parse` | `0x1401E3FE0` | `(parseSession, int, char)` | |
| `Brush_Build` | `0x1401DEB00` | `(brush_t*, bool snap, bool markModified, uint flags)` | rebuilds windings and bounds |
| `Brush_Move` | `0x1401E1540` | `(brush_t*, const float delta[3], uchar snap, uchar markModified)` | also moves a fixed-size entity's origin |
| `Select_Move` | `0x140D02700` | `(const float delta[3], bool snap)` | `Brush_Move` on every selected brush |
| `Select_Brush` | `0x140B01DB0` | `(BrushCollection*, BrushInst*)` | takes a `BrushInst`, never a `brush_t` (crashes) |
| `Select_Deselect` | `0x140D023E0` | `(char clearDrag)` | |
| `Select_Delete` | `0x140D02320` | `()` | deletes everything selected |
| `Undo_Begin` | `0x140C164D0` | `(const char* label)` | keeps the label pointer: it must stay valid |
| `Undo_End` | `0x140C16520` | `()` | |
| `Undo_IsOpen` | `0x140C16ED0` | `bool ()` | the top operation is still open |
| `Undo_AddBrushList` | `0x140B00FA0` | `(BrushCollection*)` | records the selection in the open operation |
| `Undo_Undo` | `0x140C181D0` | `(undoList, redoList, undoId, redoId)` | Edit > Undo and Redo both end here |
| `BeginParseSession` / `EndParseSession` | `0x1406F6A80` / `0x140D3C9F0` | `(const char* name)` / `()` | around an import |
| `Sys_SetTitle` | `0x140D33530` | `void (const char* name)` | window title |
| `CCamWnd::Cam_Draw` | `0x140244120` | `(CCamWnd* this, ...)` | 3D view frame |
| `CXYWnd::XY_Draw` | `0x140C2E3E0` | `(CXYWnd* this, __int64* viewport)` | 2D view frame |
| `EntityRegistry_Add` / `_Remove` | `0x140CD81B0` / `0x140CD82F0` | `(EntityInst*)` | see "Entity registry" |

### Globals

| Global | Address | Type | Meaning |
| ------ | ------- | ---- | ------- |
| current map name | `0x14521ED20` | `char[]` | path of the open map, `"unnamed.map"` when none |
| modified | `0x14521EB94` | `int` | 0 clean, 1 unsaved changes, 2 after save-selected |
| prefab depth | `0x14521EB9C` | `int` | 0 = the map itself, up to 16 levels of Enter Prefab |
| prefab stack | `0x14521F320` | 16 x 2936 bytes | one entry per level entered |
| main window | `0x1444C6560` | `RadiantMainWin*` | |
| entity list | `0x14522B440` | list sentinel | every entity, worldspawn first |
| worldspawn | `0x14522B438` | `EntityInst*` | worldspawn's instance; its `entity_t` at `+0` |
| world object | `0x14522B130` | | `+776` = worldspawn's `EntityInst*`; owns the instance list |
| selected brushes | `0x1453A2278` | `BrushCollection` | the selection |
| active brushes | `0x1453A22D8` | `BrushCollection` | everything not selected |
| entity registry | `0x14522B6D0` | map head pointer | GUID -> `EntityInst*`, size at `0x14522B6D8` |
| redraw flags | `0x1444C656C` | `int` | OR in bits to mark views dirty for the next tick |
| saving | `0x14521EB90` | `char` | set while `Map_SaveFile` runs |

### entity_t (632 bytes)

| Offset | Type | Meaning |
| ------ | ---- | ------- |
| +0 / +8 | `brush_t*` | prev / next of the entity's own brush list (the sentinel is the entity itself) |
| +240 | `EClass*` | entity class: `+0` name (`std::string`), `+72` fixed-size flag (point entity, model, prefab), `+368` class default keys |
| +280 | `float[3]` | origin (the `origin` key is written from this) |
| +292 | `float[3]` | angles (pitch yaw roll) |
| +304 | `std::map<std::string, QVariant>` | the keys (MSVC 2012 map: head +0, size +8; node key at +32, value at +64) |
| +340 / +344 | `int` | undo id / redo id (see "Undo") |
| +348 | `int` | entity id, sequential per session |
| +352 | `GUID` (16 bytes) | the entity's GUID |
| +440 / +448 | `entity_t*` | prev / next in the global entity list |

- Values in the key map are `QVariant`s: text keys are `QString` (type 10), `origin` and `angles` a shared `QVector3D`
  (type 83). Reading text is easier through the host's `WriteEntityText`.
- Selected brushes are unlinked from their entity's brush list while selected.
- Each entity also has an `EntityInst` (776 bytes): `+0` its `entity_t*`, `+16` the sentinel of its `BrushInst`
  list, `+768` the next `EntityInst` in the world's list.

### brush_t and face_t

| Offset | Type | Meaning |
| ------ | ---- | ------- |
| +0 / +8 | `brush_t*` | prev / next in the owner's list |
| +16 | `entity_t*` | owner |
| +32 / +36 | `int` | undo id / redo id |
| +48 | `GUID` | brush GUID |
| +64 / +76 | `float[3]` | mins / maxs |
| +96 | flags | bit `0x80` = selected |
| +144 / +152 / +160 | `face_t*` | `std::vector<face_t>` begin / end / capacity |
| +168 | `std::string` | layer name |
| +224 | pointer | patch data (`mesh` / `curve`); NULL for a classic brush |
| +232 | `int` | 1 physics box, 2 physics cylinder, else classic |

- `face_t` is 864 bytes: `+0` the three plane points `float[3][3]`, `+784` plane distance, `+792` normal, `+808` winding
  pointer (NULL = degenerate face, skipped), `+824` texture name.
- A point entity has one invisible brush whose GUID is made at load time and differs on every PC: find point
  entities by their entity GUID.
- A brush with 64 or more faces overruns a fixed array in the renderer: keep generated brushes well below that.

### Selection: BrushInst and BrushCollection

The selection is a list of `BrushInst` nodes, not of `brush_t`.

| BrushInst offset | Meaning |
| ---------------- | ------- |
| +0 / +8 | links in the collection |
| +16 | owner `EntityInst*` |
| +24 / +32 | links in the owner's instance list |
| +48 | the `brush_t*` |
| +96 | flags (`0x80` = selected) |

- `BrushCollection` (96 bytes): `+56` points at the list sentinel; first node `*(sentinel + 8)`, next `*(node + 8)`,
  stop at the sentinel.
- Selection changes are batched: `0x140B01060 (BrushCollection*)` begins a batch, `0x140B01070 (collection, 3, 0, 1)`
  ends it and fires the change notification that Entity Info and the panes listen to.

### Entity registry

- A `std::multimap<GUID, EntityInst*>` keyed by the GUID an entity had when it was added. The Entity List pane and
  the exploder manager read it.
- **Never write an entity's GUID while it is registered.** Remove it, change the GUID, add it again (Radiant's own
  duplicate-GUID repair does exactly this). A GUID changed in place leaves a stale entry, and the next selection
  change reads freed memory. `host->RestampImportedGuids` does it correctly.

### Undo

- Q3Radiant's design. An operation is opened by `Undo_Begin(label)`, records brushes and entities, and is closed by
  `Undo_End()`. Recording stamps the live object with the operation's id (`brush+32`, `entity+340`).
- `Undo_Undo` finds live objects by those ids, not by pointer, removes them and re-links the saved copies. A saved
  brush goes back to the live entity whose entity id (`+348`) matches the one it was saved with, else to the world.
- Consequence: undoing a delete-and-import of an entity that owns brushes brings the brushes back to the world, not
  the entity. Point entities are recorded whole and come back intact.
- The label pointer passed to `Undo_Begin` is kept and read later: never pass memory that goes away (a string inside a
  DLL that can be unloaded, a temporary). The host's own undo functions copy the label for you.

### Importing (paste)

- `Map_ImportBuffer(&text, 4, 0)` imports clipboard-format text: the first block is merged into worldspawn, the
  others become new entities, everything imported is selected.
- `flags` must be 4: it selects the modern `mesh`/`curve` syntax. With 0, a mesh brush ends in a fatal error box
  and Radiant exits.
- New objects always get fresh GUIDs, whatever the last argument says.
- Worldspawn's GUID changes on every paste: recognize worldspawn by its class, never by GUID.
- The paste deletes every `target` and `linkto` key unless the byte at `0x14521EB91` (Radiant's "copy the
  selection into the next map" flag) is set. The host sets it around its own imports since host 1.1.0.
- `reflection_probe` entities need a runtime object that only a map load builds; an imported one makes the lighting
  pass assert. Add those through the `.map` file and a reload.

### Patch vertices

- `PatchGrid`: rows are the blocks in the `.map` text, columns are the points per block.
- `PatchSelectVerts` returns the count, -1 for a bad argument or over 1024, -2 for not a selected patch, -3 for a row or column out of range.
- `PatchWeldVerts` returns the vertices moved, 0 when none are selected, -1 when no patch is selected or the selection is stale.

### Loading and saving

- `Map_LoadFile` clears the map, asks about an autosave if one exists, parses, places the camera at the player
  start (or the last bookmark), clears the modified flag and sets the title. It does not ask about unsaved changes.
- Never start a map load from inside the code that reacts to a load finishing; post it to the next tick.
- `Map_SaveFile` writes `iwmap 4`, the layer list, then every entity. A plain save writes no backup; every load
  leaves `<map>.bak`.

### The .map format

- Text, header `iwmap 4`, then a layer list, then `// entity N` blocks.
- Entity: `{`, `guid "{...}"`, optional unquoted `layer "Name"`, `"key" "value"` lines, then `// brush N` blocks.
- Classic brush: `{`, ` guid "{...}"`, one line per face: three points `( x y z )`, material, texture settings, the
  lightmap material and its settings.
- Patches: `mesh { ... }` or `curve { ... }` with the material, `lightmap_gray`, the grid size, then one parenthesized
  vertex list per column (`v x y z [c r g b a] t u v lu lv`).
- Physics primitives are saved as parameters: `physics_box { 15 floats }`, `physics_cylinder { 8 floats }`. The map
  compiler refuses both in a level map; they belong in physics models.

### Qt without headers

- Qt objects are allocated by Radiant's runtime; widgets you create must come from MSVCR110's `operator new` if Qt
  will delete them. The host's `ui` functions handle this.
- `sizeof(QWidget)` is 48 in this build (`QWidgetData*` at `+40`), and the widget subclasses used keep that size.
- `QObject` vtable: `metaObject, qt_metacast, qt_metacall, ~QObject, event, eventFilter, timerEvent, childEvent,
  customEvent, connectNotify, disconnectNotify`; `QWidget::setVisible` is slot 11.
- `QEvent::type()` is the `ushort` at `+16`.
- **By-value returns** (`QString`, `QPoint`, `QFlags`, anything with a constructor) come back through a hidden pointer
  (after `this` for member functions). Declaring `QListWidgetItem::flags()` as `int f(this)` makes Qt write the result
  to a random address: silent memory corruption. Plain enums and ints come back in RAX.
- `QString` and `QByteArray` are one `QArrayData*`: `int ref, int size, uint alloc, qptrdiff offset`, data at
  `d + offset`.
- `QApplication::allWidgets()` returns a `QWidgetList` by value; release it with an atomic decrement of `ref` and
  `QListData::dispose` at 0.
- **Signals without moc**: `QObject::connectImpl` (exported from `Qt5Core64r.dll`) is the back end of the functor
  `connect`. The `signal` argument points at the bytes of a pointer-to-member: 8 bytes for single-inheritance
  classes (`QAction`), 16 for every `QWidget` subclass (address + zero adjustment). A 24-byte zero-padded buffer
  serves both. The slot object is `{ int ref (starts at 1); impl(int which, self, receiver, void** args, bool* ret) }`
  with `which` 0 destroy, 1 call (`args[1..]` are the signal's arguments), 2 compare.
- `QWidget::winId()` on a widget without a native window creates one on the calling thread; call it on the main
  thread only.
- Never return 1 for `QEvent::Close` from an event filter: Radiant asserts at exit.
- Radiant keeps its own copies of Qt vtables whose entries are jump stubs into the Qt DLLs: follow the `E9` / `FF 25`
  jumps before comparing an entry with a Qt export.

### Traps

- **Assertions are fatal.** Radiant's assert box offers OK (exits) and Cancel (breaks into a debugger, which also
  ends the process without one). Any state your hook leaves that Radiant asserts on ends the session.
- **Memory**: every camera draw with meshes needs one contiguous block of about 1.4 GB for its geometry cache. On a
  PC short of free memory Radiant dies there, in its own renderer; it is not your plugin.
- **Patches are recreated** by undo, redo and row or column inserts: same GUID, new address. Look them up again by
  GUID every time.
- `Select_Brush` with a `brush_t*` instead of a `BrushInst*` crashes.
- Radiant deletes the selection on Backspace, not Delete.
- The splash screen is also a `Qt5QWindowIcon` titled "Radiant" and stays alive hidden: find the main window by its
  size or through `host->MainWindow`, not by title alone.


## APE

### Binary identity

| What | Value |
| ---- | ----- |
| File | `<game>\bin\asseteditor_modtools.exe`, the current Steam build, 19,324,928 bytes |
| `TimeDateStamp` | `0x5DF44F21` |
| `SizeOfImage` | `0x264A000` |
| Image base | linked at `0x140000000`, **ASLR on**: use `ApeAddress` |
| Toolchain | the same as Radiant: Visual C++ 2012, Qt 5.3.2 `64r` DLLs |
| Main window | class `apeMainWindow`, a stock `QMainWindow` with `QDockWidget`s |
| Other DLLs | `sqlite64r.dll` (gdt.db), `AssetScript64r.dll` and `AngelScript64r.dll` (the `.awi` scripts) |

APE allows one instance: a second `asseteditor_modtools.exe` exits at once while one runs.

### Finding code under ASLR

- `ApeAddress(ida)` returns `module base + (ida - 0x140000000)`, NULL on another build.
- Before a detour, `RtlLookupFunctionEntry(target)` must return an entry whose `BeginAddress` is exactly the target.
- Telling callers apart: compare `_ReturnAddress()` with the address range of the known call site. This works for
  real calls, not for tail calls.
- Most of APE is reachable without any address: its widgets are ordinary Qt objects. Walk `QObject::children` from
  the main window, test classes with `QMetaObject::className` up the `superClass` chain, and use object names.

### Widget tree

```
apeMainWindow
  QMenuBar / QStatusBar / QToolBar 'MainToolBar'
  QDockWidget 'PreviewToolBar'   > QMainWindow > apePreviewWidget
  QDockWidget 'AssetToolBar'     > apeAssetTreeWidget (QTreeWidget)
  QDockWidget 'FilterToolBar'    > QLineEdits: name, type, GDT and value filters
  QDockWidget 'GraphToolBar'     > apeAssetGraphWidget
  QDockWidget 'OutputToolBar'    > QTextEdit (the Output pane)
  QWidget > QTabWidget (the editor tabs)
    QStackedWidget > apeTabWidget per tab
      QToolBar > ... apeAssetLineEditWidget (text "<name>.<type>") ...
      QScrollArea > apeAssetWidget (the property grid)
```

- The current asset is the text of the tab's `apeAssetLineEditWidget` (`name.type`); the tab title alone is the
  bare name.
- Asset tree items carry no data roles; the type of an asset comes from gdt.db.
- The filter boxes are told apart by their tool tips ("asset names", "asset types", "GDT files", "keys and values").
- The Output pane keeps the previous line's format when appending (red after an APE error): append HTML with an
  explicit colour.

### gdt.db

- `<game>\gdtdb\gdt.db`, its size depends on the assets in `source_data` (hundreds of MB is common). Only Treyarch's `sqlite64r.dll` opens it (other SQLite builds stop at "too
  many columns"). Open it read-only. `host->ape` already answers the common questions.

| Table | Columns |
| ----- | ------- |
| `_gdf` | `PK_id`, `name` (the asset type) |
| `_gdt` | `PK_id`, `name` (lower-case path of the `.gdt`), `bOpenForEdit`, `timestamp` |
| `_entity` | `PK_id`, `name`, `FK_parent_id` (derived assets), `FK_gdf`, `FK_gdt`, `_gdt_linenum`, ... |
| `_meta` | `_name` (type), `_key`, `_type`, `_default`, `_min`, `_max`, `_enums` (`|`-separated), `_index` |
| one table per type | `PK_id` (= `_entity.PK_id`), `_name`, one column per property |

- An asset name is unique per type, not overall: `default` exists as several types.
- Saved values live in the `.gdt` files: `"name" ( "type.gdf" ) { "key" "value" ... }`, a derived asset is
  `"child" [ "parent" ]`.

### Traps

- A `.gdt` rewritten by another program while APE has it open can crash APE on its next refresh. Close APE before
  rewriting a `.gdt` it has loaded.
- `QTabWidget::widget(int)` is not exported by this Qt build: take the pages from the stacked widget's children.
- APE's tree filter re-runs over every asset on each change; anything you hook there runs hundreds of thousands of
  times.
- A `.awi` script and everything it includes are recompiled when any of their modification times change.

## Mod Tools Launcher

### Binary identity

| What | Value |
| ---- | ----- |
| File | `<game>\bin\modlauncher.exe`, the current Steam build, 256,072 bytes |
| `TimeDateStamp` | `0x5F08BBD1` |
| `SizeOfImage` | `0x40000` |
| Image base | linked at `0x140000000`, ASLR on |
| md5 of the untouched file | `cf9cada2b8f893debf99f6bdfb684bd1` |
| Toolchain | Visual C++ 2012, Qt 5 `64r` DLLs (`Qt5Core64r`, `Qt5Gui64r`, `Qt5Widgets64r`), `steam_api64.dll` |
| Main window | class `mlMainWindow` |
| Source | Treyarch publishes it: https://github.com/TreyarchGames/ModLauncher (`mlMainWindow.cpp` holds almost everything) |

The host has no address helper for the launcher. You rarely need one: the source shows every widget and step, and
the window is plain Qt (`host->MainWindow`, `host->MenuEntries`, `host->TriggerMenuAction`).

### How a build runs

- Each ticked step runs as a child process started through `QProcess`, which ends in `CreateProcessW`. The
  launcher shows the child's stdout in its output pane, and its Cancel kills the child it started.
- The steps' command lines (from the source), with `<game>` from `TA_TOOLS_PATH`:

| Step | Command line |
| ---- | ------------ |
| Compile | `bin\cod2map64.exe -platform pc [-onlyents | -navmesh -navvolume] -loadFrom <game>\map_source\<xx>\<map>.map <game>\share\raw\maps\<xx>\<map>.d3dbsp` |
| Light | `bin\Radiant_modtools.exe -ledSilent +low|+medium|+high +localprobes +forceclean +recompute <map>` |
| Link | `bin\linker_modtools.exe -language <language> -modsource <map>`, a mod: `-fs_game <mod> -modsource <zone>` |
| Run | `BlackOps3.exe +set fs_game <map> +devmap <map>` |

- `<xx>` is the map folder (usually the first two letters of the name, `zm`, `mp`).
- Light runs Radiant itself without a window. Plugins are never loaded in a `-ledSilent` Radiant.
- `cod2map64.exe` with no arguments prints its option list.
- The linker's exit code is not an error flag: good links end with codes like 2000. Judge a link by
  `UNRECOVERABLE ERROR` in its output.
- `linker_modtools.exe` is packed: its code is decrypted by the entry stub at start, so it cannot be read or
  patched on disk. Patch it in memory after the C runtime has started.
- Build Language "All" links 12 languages. Each writes `zone\<prefix>_<zone>.ff` after the main `zone\<zone>.ff`:
  english `en`, french `fr`, italian `it`, spanish `es`, german `ge`, portuguese `bp`, russian `ru`, polish `po`,
  japanese `ja`, traditionalchinese `tc`, simplifiedchinese `sc`, englisharabic `ea`.

### Widgets and dialogs

- The Maps and Mods tree: the first checked row is the item every step and Publish work on.
- Light quality: `mLightQualityWidget`, a `QComboBox` with Low, Medium and High. It starts at Medium and is not
  saved.
- Edit > Options: a `QDialog` titled "Options" with a `QVBoxLayout` of the "Use Treyarch Theme" check box, the
  "Build Language:" row and a `QDialogButtonBox`. The check box is stored in QSettings as `UseDarkTheme`.
  Ticked means the plastique style with `<game>\radiant\stylesheet.qss`, unticked means Qt's Windows style with no
  sheet.
- Publish Mod: a `QDialog` titled "Publish Mod" with three line edits, a "Description:" label and a
  `QDialogButtonBox`. The one description goes to both `zone\workshop.json` (what the game lists) and Steam's
  `SetItemDescription`. For an item already published it is first replaced by Steam's text.
- The thumbnail field's "..." button opens `QFileDialog::getOpenFileName` with the caption "Open Thumbnail".
- Steam: the launcher gets `ISteamUGC` as `"STEAMUGC_INTERFACE_VERSION008"`. When the launcher was not started
  through Steam, Steam's interfaces are not available and Publish cannot upload.

### Traps

- Options and Publish Mod are stack objects: the dialog has the same address every time it opens. Track them by
  their show and hide events, never by pointer.
- The host's tick in the launcher is a 50 ms timer that keeps running while a modal dialog such as Options is open,
  so `OnTick` runs under it. A dialog opened from `OnTick` then nests inside the open one: check
  `QApplication::activeModalWidget` first.
- The launcher's Qt has no image format plugins: it reads only PNG and BMP. Convert other formats before handing
  them to a `QLabel` or `QPixmap`.
- Toggling "Use Treyarch Theme" redraws the launcher's own look over any sheet a plugin applied: apply yours again
  after the dialog closes.
- A `CreateProcessW` detour sees every launch, including the editors (the launcher starts Radiant and APE with
  `QProcess` too). Match the program by its full path in `<game>\bin`.
