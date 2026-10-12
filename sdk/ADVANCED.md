# Advanced Plugin Guide

Everything in `GETTING_STARTED.md` works through the host's functions, which are stable and safe. This file is
for when you need something the host does not offer: changing how Radiant or APE behaves inside, calling one of
their internal functions, or reading their data structures. MTPlugins' own RadiantPlus, APEPlus and Play Test work
this way.

You need to be comfortable with C++, x64 calling conventions and a disassembler. Nothing here is guaranteed to stay
the same, and a mistake here crashes the editor instead of being caught by the host. `MODTOOLS_REFERENCE.md` next
to this file holds the layouts, addresses and traps that are already known.


## 1. The three functions

| Function | What it does |
| -------- | ------------ |
| `host->RadiantAddress(self, idaAddress)` | Turns an address of `Radiant_modtools.exe` as a disassembler shows it into a pointer you can call or read. Returns NULL when the running Radiant is not the build the addresses belong to. |
| `host->ape->ApeAddress(self, idaAddress)` | The same for `asseteditor_modtools.exe`. APE loads at a random address (ASLR), so this one really rebases. NULL on another build. |
| `host->Detour(self, target, myFunction, &original)` | Hooks `target` (MinHook): every call goes to `myFunction`, which calls `original` to run the real code. Removed automatically when your plugin unloads. Works in every application. |

A typical hook:

```cpp
using SaveFile_t = char(__fastcall*)(char* filename, char silentAutosave, char saveSelected);
static SaveFile_t g_origSave = nullptr;

static char __fastcall MySave(char* filename, char silentAutosave, char saveSelected)
{
    rp::Log("Radiant is saving %s", filename ? filename : "(dialog)");
    return g_origSave(filename, silentAutosave, saveSelected);       // always run the real one
}

static int OnLoad(void)
{
    void* target = rp::host->RadiantAddress(rp::self, 0x140CD2890);   // Map_SaveFile, see MODTOOLS_REFERENCE.md
    if (!target) { rp::Log("unknown Radiant build: hook skipped"); return 1; }
    if (!rp::host->Detour(rp::self, target, (void*)MySave, (void**)&g_origSave)) rp::Log("detour failed");
    return 1;
}
```

(This one only shows the mechanism: for saves you would use `OnMapSaved`.)


## 2. Safety rules

1. **Never guess an address.** Every address you use must come from the exact build you target (section 4). If
   `RadiantAddress` or `ApeAddress` returns NULL, skip the feature and log it; never fall back to a hard-coded pointer.
2. **Match the prototype exactly**: argument count, types and return type. A wrong one corrupts registers or the
   stack, often far from the hook. If you do not know a function's arguments yet, hook it with four pointer-sized
   arguments and pass them all through: x64 passes the first four in registers, so that is safe for reading.
3. **A crash in a detour is not caught.** The host's crash isolation covers your callbacks only. A fault inside a
   hook takes the editor down, at every start if it happens every time (the host then offers to start without
   plugins). Check every pointer you read.
4. **Detours run on whatever thread calls the function**, often not the main thread. Do not call host functions
   marked MAIN THREAD from there; post to the main thread with `rp::PostToMainThread`.
5. **The editor's own code can pump messages** (load dialogs, asset reloads, message boxes). While your hook waits for
   `original` to return, the editor may run your other callbacks, or even unload your plugin; the host delays the
   unload until your code is off the stack, but your own state may have changed under you.
6. **Keep hooks cheap.** Some functions run thousands of times per frame. Measure before and after.
7. **APE in particular**: before hooking, check that the address is a real function start. `RtlLookupFunctionEntry`
   must return an entry whose `BeginAddress` is exactly your target's offset from the module base.
8. **Clean-room.** Write your own code from what you learn. Do not paste decompiler output into your plugin, and do
   not ship anything copied out of the editors (code, images, data).


## 3. Calling the editors' functions and reading their data

The same address works for calling: cast the pointer to the right function type and call it, on the main thread.

```cpp
using SetTitle_t = void(__fastcall*)(const char* name);
if (auto setTitle = (SetTitle_t)rp::host->RadiantAddress(rp::self, 0x140D33530))   // Sys_SetTitle
    setTitle("my title");
```

Globals work the same way: `int* modified = (int*)rp::host->RadiantAddress(rp::self, 0x14521EB94);`.

Things that bite when calling or reading:

- **Calling convention**: x64 fastcall. The first four arguments in RCX, RDX, R8, R9 (floats in XMM0 to XMM3), the
  rest on the stack, the caller reserves 32 bytes of shadow space. The compiler does all of this when the
  prototype is right.
- **Objects returned by value** (a `QString`, a `std::string`, any class with a constructor) do not come back in
  RAX: the caller passes a hidden pointer to the result as the first argument, or the second one after `this`.
- **The editors are built with Visual C++ 2012** (runtime `MSVCR110`/`MSVCP110`). Their `std::string`, `std::vector`
  and `std::map` have the 2012 layouts, which differ from what your compiler uses: read them by offset, never
  through your own STL types, and never free their memory with your runtime.
- **Files and memory belong to their runtime.** A `FILE*` a Radiant function writes to must come from MSVCR110's
  `_wfopen`, and memory Radiant frees must come from its allocator.
- **Qt**: the editors use Qt 5.3.2 built with MSVC 2012, from DLLs named `Qt5Core64r.dll`, `Qt5Widgets64r.dll` and so
  on. No headers or import libraries ship for those names; call Qt with `GetProcAddress` on the decorated export
  names (for example `?setText@QLineEdit@@QEAAXAEBVQString@@@Z`). `host->ui` already wraps the common widgets.
  `MODTOOLS_REFERENCE.md` lists the Qt traps (object sizes, signals without moc, by-value returns).


## 4. Finding functions yourself

You need a disassembler: Ghidra (free) or IDA. Load the editor from your own install,
`<game>\bin\Radiant_modtools.exe` or `<game>\bin\asseteditor_modtools.exe`, and check it is the build the
reference describes (section "Binary identity" there). Radiant has no ASLR and loads at `0x140000000`, so every
address the disassembler shows is the real one. APE is analysed at the same base and rebased by `ApeAddress`.

Ways in, fastest first:

1. **Strings.** The editors log a lot (`"Map_LoadFile: %s\n"`, `"Map_SaveFile: %s\n"`, `"Nothing left to undo.\n"`),
   and their assertions carry the failed expression and the source file name. Search the string, follow its
   cross-references, and you are in the function that uses it. Most known functions were found this way.
2. **id Software's names.** Radiant descends from id Software's Q3Radiant, whose source is public (GPL). Treyarch
   kept many of its names and much of its structure: `Map_LoadFile`, `Brush_Move`, `Select_Delete`, `Undo_Start`,
   `entity_t`, `brush_t`, `face_t`, epairs. Read the Q3Radiant sources to know what a function is meant to do and
   what its structures hold, then find the Treyarch version by its strings. Treyarch extended the structures
   (GUIDs, layers, patches, an instance layer for the selection), so check every offset against the binary.
3. **Profile labels.** Many functions announce themselves to Radiant's profiler with their own name
   (`"RadiantMainWin::RoutineProcessing"`, `"CCamWnd::Cam_Draw"`), which is a string to search too.
4. **Qt.** The editors' own classes have meta objects (`apeAssetTreeWidget`, `QtEntityInfo`, ...): class names and
   signal names appear as strings, and their virtual tables lead to their methods. The Qt DLL exports show what Qt
   functions exist in this build.
5. **Watch it run.** Attach Visual Studio or WinDbg to the editor, break on a function you know, and look at the call
   stack and the arguments. Read memory to confirm a structure offset before you write to it.
6. **The known map.** About 600 addresses are already named in MTPlugins' source on GitHub: `src/radiant_addrs.h`
   (the host's, with comments) and the plugins under `plugins/` (RadiantPlus, APEPlus, Play Test). Start next to a
   known function and you rarely have to search the whole binary.

When you find something, write down the evidence (which string, which caller), the prototype and the offsets you
verified. Future you will need it when something crashes.


## 5. When the editors are updated

All of this is tied to one build of each editor, the July 2024 one. If Treyarch ships a new build, `RadiantAddress`
and `ApeAddress` return NULL and your hooks simply stay off; the host's own features keep working where they do
not need addresses. Re-find the functions in the new build (strings again) before turning them back on.


## 6. Advanced Radiant functions of the host

For plugins that apply other people's edits or preview geometry, the host has a few functions beyond the everyday ones in `GETTING_STARTED.md`. All of them are Radiant only and MAIN THREAD.

### Untracked edits and undo tags

- Between `BeginUntracked` and `EndUntracked` (paired, inside one callback) `ImportMapText` and `DeleteBrushes` record no undo, and Radiant's redo list survives.
- Radiant's undo finds live objects by tag: an object carries the `undoId` of the operation that last recorded it, or the `redoId` of the redo that restores it. Saved brush copies find their owner through the entity's `entityId`.
- `GetUndoTags` and `SetUndoTags` copy these (entity: `undoId`, `redoId`, `entityId`; brush: `undoId`, `redoId`, 0).
- When an untracked rebuild replaces an object, copy the old object's tags onto the new one. The user's earlier undo step then still applies, instead of deleting the new object or leaving a duplicate.

### Previews that move geometry

- `MoveBrush` and `SetBrushPlanes` rebuild in place with no undo record, no selection change and the map not marked modified.
- They are meant for drag previews that a later `ImportMapText` / `DeleteBrushes` makes permanent. Call `MarkMapModified` after a real edit.
- `FindBrushesByGuid` treats brushes the local user has selected as absent, because Radiant unlinks them from their entity. `WriteEntityTextWith` links the given brushes back for the write.

### Native model preview

- `SetNativeModelPreview`, `SetNativeModelPreviewSet` and `SetNativeModelCullBounds` are editor-only. The host's guarded D3D model-pass adapter draws a deformed copy of one selected model with its real materials, only after a strict vertex-bounds match (optional `sourcePositions` prove the draw identity). It never writes an asset or entity memory.
- Coordinates are in source-model space, before origin, angles and modelscale. `anchor` is the rest local-X minimum endpoint, centred on the local Y/Z spans. Mode 0 is stretch, 1 curve, 2 skirt, `curveSegments` is 2 to 16.
- The status is UNAVAILABLE (no usable D3D adapter), WAITING (no verified draw yet) or ACTIVE.
- The cull-bounds envelope is applied only around the model-dispatch call and then restored.

### Patch vertices

- `PatchGrid`: rows are the blocks in the `.map` text, columns are the points per block, xyz triples in `row * cols + col` order.
- `PatchSelectVerts` needs every brush selected. It returns -1 for a bad argument or more than 1024 vertices, -2 when a GUID is not a selected patch, -3 when a row or column is out of range.
- `PatchWeldVerts` goes through Radiant's own "connect vertices" (one undo step, seams over several patches stay closed). It returns -1 when no patch is selected or the vertex selection is stale.
- Undo, redo and inserts re-create patch objects, so read them again afterwards.

### Brush and patch commits

- `CommitBrushGeometry` writes a move (`delta`) or a classic brush's plane set (`pts`, 9 floats per face, wins over `delta`) as one undo step and marks the map modified. Inside an open undo step it joins it.
- `PatchSetGrid` replaces a patch's control points by GUID. When `rows` or `cols` differ it resizes the net and resamples the UVs and vertex colors from the old one.
- `PatchSetGrid` records no undo: call it between `BeginUndo` and `EndUndo`. It refuses non-finite points, coordinates past 131072, more than 5450 points and collapsed segments.
- On hosts that have these two, `MoveBrush` on a patch also re-tessellates its render mesh. On older hosts call `Patch_RebuildMesh` after it yourself.

### Running on an older host

A host function added after 1.0.0 is missing on a 1.0.0 host (`rp::Has` is false). When the plugin must still work there, keep a fallback built from 1.0.0 functions and `RadiantAddress`:

- Brush commit: `BeginUndo`, `SetBrushPlanes` or `MoveBrush`, `MarkMapModified`, `EndUndo`. Bracket only when `Undo_IsOpen` is false, because `EndUndo` closes whatever step is open.
- Patch grid: clear the patch vertex selection, `Patch_Resize` if the size changes, write the control records, `Brush_Build(brush, 0, 1, 0)`, `Patch_RebuildMesh(patch)`, then copy the brush's padded bounds into its `BrushInst` cull bounds. Layouts in `MODTOOLS_REFERENCE.md` "Patch vertices".
- Call the host function whenever `rp::Has` says it exists, so a host update fixes the plugin without a new release.

### Prefab scopes

- `PrefabStackPlacement`: an entered prefab is shown in its file's own space, so a point `p` there sits at `origin + R(angles) * p` in the outer scope.
- Do not `WriteEntityText` an entity of an outer scope while inside a prefab: Radiant's writer crashes on it.
