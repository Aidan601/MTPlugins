# Writing plugins for the Black Ops III mod tools

This folder is the SDK for MTPlugins. With it you can write your own plugins for the three Black Ops III mod tools
applications: **Radiant** (the map editor), **APE** (the Asset Property Editor) and the **Mod Tools Launcher**. A
plugin is a normal Windows DLL written in C or C++. The plugin host loads it into the application, and from there
it can add menus, hotkeys, dockable panes and dialogs, and then do what that application is about: read and change
the open map and draw in the views in Radiant, or work with assets and their properties in APE.

There is one SDK and one kind of plugin for all three. A plugin says which applications it runs in, and one DLL
can run in several. Sections 1 to 6 apply to every plugin; sections 7, 8 and 9 cover what is particular to Radiant,
APE and the launcher.

This guide takes you from nothing to your own plugin running in Radiant, then covers what you need to know to
write real ones. Allow about 30 minutes, most of it for installing Visual Studio.


## What is in this folder

| File | What it is |
| ---- | ---------- |
| `modtools_plugin.h` | The plugin interface. Every function the host offers is declared and commented here, tagged [all], [Radiant] or [APE]. This is the reference. |
| `modtools_plugin.hpp` | Optional C++ helpers on top of it (`rp::Log`, `rp::ConfigGet`, `rp::ForEachEntity`, ...). |
| `example\HelloPlugin\` | A complete small plugin: a menu, a hotkey, a pane with a button, text in the 3D view, APE support. |
| `example\MapEditExample\` | A Radiant plugin that edits the map: follows the selection, shows and changes an entity's keys with undo, adds an entity. |
| `NewPlugin.bat` | Makes a new plugin project from HelloPlugin under the name you choose. |
| `ADVANCED.md` | For experts: hooking and calling the editors' own code (`RadiantAddress`, `ApeAddress`, `Detour`) and how to find more of it. |
| `MODTOOLS_REFERENCE.md` | What is known about the insides of Radiant, APE and the Mod Tools Launcher: functions, globals, layouts, traps. Goes with `ADVANCED.md`. |
| `MODTOOLS_ADDRESSES.md` | The full list: every Radiant and APE function and global MTPlugins knows, with its address. |


## 1. What you need

- **Windows 10 or 11, 64-bit**, with the Black Ops III Mod Tools installed from Steam.
- **Radiant Plugins installed and working**: Radiant shows a "Plugins" menu and Ctrl+Shift+P opens the Plugins
  window. See the README next to this folder if it does not.
- **Visual Studio 2022 or newer.** The free Community edition is fine. In the Visual Studio Installer, tick the
  **"Desktop development with C++"** workload. Nothing else is required.

You do not need the plugin system's source code, Qt, or any other library.


## 2. Build the example

1. Open `example\HelloPlugin\HelloPlugin.vcxproj` in Visual Studio (double-click it, or File > Open > Project/Solution).
2. In the toolbar, set the configuration to **Release** and the platform to **x64**.
3. Build > Build Solution (Ctrl+Shift+B).

The Output window should end with a line like:

```
HelloPlugin copied to E:\SteamLibrary\steamapps\common\Call of Duty Black Ops III\bin\plugins\HelloPlugin\
```

That folder, `<game>\bin\plugins\`, is where the editors load plugins from. Each plugin lives in its own
subfolder, and the folder name, the DLL name and the plugin's name in its code must all be the same.

**If the build says "Not copied to the game"**: the project finds your game folder through the `TA_TOOLS_PATH`
environment variable, which the mod tools normally set. If it is missing on your PC, right-click the project in
Solution Explorer, choose Unload Project, then Edit Project File, set `<Bo3Root>` near the top to your Black
Ops III folder (the one containing `bin\Radiant_modtools.exe`), save, and choose Reload Project.

**If the build says "could not copy"**: the editor is running with the plugin loaded, and Windows does not let
you replace a DLL that is in use. See step 4.


## 3. See it run

1. Start Radiant with plugins (the "Launch Radiant with Plugins" shortcut, or the normal launcher if you
   installed the loader).
2. Press **Ctrl+Shift+P** to open the Plugins window. HelloPlugin is in the list; tick it if it is not ticked.
3. A "Hello" menu appears in Radiant's menu bar, along with a small "Hello Plugin" pane. Click its button, or
   press Ctrl+Shift+H. The 3D view shows "HELLO PLUGIN: n SELECTED" and updates as you select brushes.

It works the same in APE, where the pane shows the asset you have open.


## 4. Change something and reload

You do not need to restart the editor between builds:

1. In the Plugins window, **untick** the plugin. The host unloads it and removes its menu, pane and hotkey.
2. Change the code and build. The new DLL is copied into place.
3. **Tick** the plugin again. The new version loads.

Try it: in `HelloPlugin.cpp`, change `"Say hello"` to something else, and repeat those three steps.


## 5. Make your own plugin

Run `NewPlugin.bat` (double-click it, or run `NewPlugin.bat MyTool` in a command prompt). It asks for a name,
then creates `MyPlugins\<Name>\` with a project, a `.cpp` and a `CMakeLists.txt`, all renamed from HelloPlugin.

- The name may contain letters, digits and `_`. It becomes the DLL name and the folder under `bin\plugins`.
- Open `MyPlugins\<Name>\<Name>.vcxproj` and build it like the example.
- It starts as a copy of HelloPlugin, including its Ctrl+Shift+H hotkey. Delete what you do not need.
- Keep the project somewhere inside this SDK folder: it finds `modtools_plugin.h` by looking in the folders
  above it. If you move it elsewhere, set `ModToolsSdkDir` at the top of the `.vcxproj` to this folder.

Prefer CMake? Every project comes with a `CMakeLists.txt` that builds the same DLL:

```
cmake -S . -B out -A x64
cmake --build out --config Release
```


## 6. How a plugin works

Everything in this section applies to every plugin, whichever application it is for.

### The entry point

The host calls one exported function, `ModToolsPluginEntry`, right after it loads your DLL. You give back a
table describing your plugin and which of its functions the host should call:

```cpp
#define MODTOOLS_PLUGIN_IMPL            // in exactly one .cpp of your plugin
#include "modtools_plugin.hpp"

static int OnLoad(void)
{
    rp::Log("hello from my plugin");
    return 1;                          // 0 refuses loading (log why)
}

extern "C" MODTOOLS_PLUGIN_EXPORT const ModToolsPluginApi* ModToolsPluginEntry(ModToolsPlugin* self, const ModToolsHostApi* host)
{
    rp::Bind(self, host);              // remember our handle and the host's function table
    static ModToolsPluginApi api = {};
    api.size = sizeof api;
    api.apiVersion = MODTOOLS_PLUGIN_API_VERSION;
    api.name = "MyTool";               // must equal the DLL name
    api.displayName = "My Tool";
    api.version = "1.0.0";
    api.author = "you";
    api.description = "One or two sentences for the Plugins window.";
    api.url = "https://github.com/you/MyTool";   // optional: the Plugins window's Website button opens it
    api.apps = MODTOOLS_APP_RADIANT;    // or MODTOOLS_APP_APE, MODTOOLS_APP_LAUNCHER, or several with |
    api.OnLoad = OnLoad;
    return &api;
}
```

Two values matter everywhere:

- `self` is your plugin's handle. Almost every host function takes it as the first argument
  (`rp::self` after `rp::Bind`).
- `host` is the table of everything the host can do for you (`rp::host`). Look through `modtools_plugin.h` to
  see what is there: it is grouped by topic, every function has a comment, and the index at its top lists what
  works in which application.

### Which applications

Set `api.apps` to the applications your plugin supports, one or more of `MODTOOLS_APP_RADIANT`,
`MODTOOLS_APP_APE` and `MODTOOLS_APP_LAUNCHER` joined with `|`. The host only loads it there; in the others it is
listed as "Other application". Inside, ask where you are with `rp::InRadiant()`, `rp::InApe()` or
`rp::InLauncher()` (or `rp::App()`).

What each application offers:

| | Radiant | APE | Launcher |
| --- | --- | --- | --- |
| Log, settings, threads, menus, hotkeys, panes, dialogs, widgets, other plugins | yes | yes | yes |
| The open map, camera, entities and brushes, undo, drawing in the views (section 7) | yes | no | no |
| `host->ape`: assets, properties, gdt.db (section 8) | no | yes | no |
| Callbacks of its own | `OnMapLoading`, `OnMapLoaded`, `OnMapSaved`, `OnBeforeScopeChange`, `OnScopeChanged`, `OnDraw` | `OnAssetChanged` | none |

A function that belongs to another application does not crash: the Radiant ones return 0 or NULL (or do nothing)
in APE and the launcher, and `host->ape` is NULL outside APE. In `modtools_plugin.h` every function is tagged
[all], [Radiant] or [APE].

### Callbacks

These are the functions you can fill in `ModToolsPluginApi`. Leave the ones you do not need empty; a callback of
another application is simply never called.

| Callback | Applications | When it is called |
| -------- | ------------ | ----------------- |
| `OnLoad` | all | Once, after your plugin is loaded. Create your menus, panes and hotkeys here. |
| `OnUnload` | all | Once, before your plugin is unloaded. Stop your own threads. Menus, panes and hotkeys are removed for you. |
| `OnTick` | all | Continuously, hundreds of times per second, on the main thread. Keep it very cheap. |
| `OnSettings` | all | The user clicked Settings for your plugin in the Plugins window. |
| `OnCommand` | all | Optional text commands from other tools; most plugins leave it empty. |
| `OnMapLoading` | Radiant | Radiant is about to load a map (every root load, the map Radiant was started with included). `OnMapLoaded` follows. |
| `OnMapLoaded` | Radiant | Radiant loaded a map (also called once if your plugin loads after a map is already open). |
| `OnMapSaved` | Radiant | Radiant saved the map. |
| `OnBeforeScopeChange`, `OnScopeChanged` | Radiant | Before and after Enter Prefab / Leave Prefab swaps what is being edited. |
| `OnDraw` | Radiant | A view is being drawn. The only place where you can draw lines and text into Radiant's views. |
| `OnAssetChanged` | APE | The user switched to another asset (another tab, or a new asset in the current one). |

### The rules that keep the application alive

A plugin runs inside the application's process, so a bug in it can take the application down. These rules avoid that:

1. **All callbacks run on the application's main thread**, and most host functions must be called from it (the header
   marks them MAIN THREAD). Never block there: the whole application freezes while your callback runs.
2. **Slow work goes on your own thread** (loading files, network, big computations). When it has a result, hand
   it back with `rp::PostToMainThread([=]{ ... })`, which runs your code on the main thread at the next tick.
3. **Never let an exception leave one of your threads.** Wrap the thread's body in `try { ... } catch (...) {}`.
   An uncaught exception on any thread closes the application immediately.
4. **Stop and join your threads in `OnUnload`**, and never leave a `std::thread` running when the application exits.

### User interface

Build panels with `host->ui`: labels, buttons, check boxes, text fields, sliders, combo boxes, tabs, trees and
layouts. Put them in a dockable pane with `host->CreatePane` (in Radiant it behaves like Radiant's own panes; every
application remembers where the user put it) or a dialog with `host->CreateDialogWindow`. React to clicks with
`host->OnClicked` and to value changes with `host->OnChanged`. HelloPlugin shows the whole pattern.

A label takes rich text. An `<a href="https://...">` link in it opens in the browser when clicked; the first 1.0.0
builds of the host showed the link without opening it, so keep the address readable in the text.

Menus: `host->AddMenu` and `host->AddMenuItem` (a menu in the application's menu bar). Hotkeys:
`host->RegisterHotkey` (they fire while the application is in the foreground). All of this works the same in
Radiant, APE and the launcher.

What `OnChanged` reports depends on the widget: a slider its value, a combo box its index, a tab widget the page, a list the row, check boxes, radio buttons and checkable buttons 0 or 1. A line edit reports 0 when editing finishes. A tree reports 0 (current item changed), 1 (text or check box changed) or 2 (items added, removed or moved).

`host->FindEditorWidget` finds a QObject by name anywhere under the application's main window. The host command `widgets <name>` prints that subtree.

### Settings

Every plugin can keep settings in `<Name>.ini` next to its DLL. Read them with `rp::ConfigGet`,
`rp::ConfigGetInt` or `rp::ConfigGetBool` (each takes a default for when the key is missing) and write them with
`rp::ConfigSet`. Ship an `.ini` with your defaults and comments if users are meant to edit it.


## 7. Radiant plugins

`api.apps` includes `MODTOOLS_APP_RADIANT`. Radiant is where most of the host's functions are: the open map, the
camera, entities and brushes, undo and drawing in the 3D and 2D views.

### Reading and changing the map

`example\MapEditExample\` is a complete Radiant plugin that does everything in this part: it follows the
selection, shows the selected entity's keys, changes them with undo and adds a new entity. Read it next to this.

- `rp::ForEachEntity([](ModToolsEntity e){ ... })` visits every entity (worldspawn first), and
  `rp::ForEachEntityBrush` every brush of one entity. `rp::GetSelection()` gives the selected brushes, and
  `rp::SelectedEntityGuids()` the entities they belong to.
- `rp::WriteEntityText(e, text)` gives an entity exactly as Radiant would save it in the `.map` file, keys and
  brushes included. `rp::GetKeys(e)` and `rp::GetKey(e, "targetname")` read its keys from that text.
- To change the map in a way the user can undo, use `host->ImportMapText` (adds objects, in the same text
  format as Radiant's clipboard) and `host->DeleteBrushes`. Both show up in Radiant's undo history under the
  label you give them. The `rp::` helpers below are built on these two.
- Group several imports and deletes into one Ctrl+Z with `host->BeginUndo(label)` ... `host->EndUndo()`.
  Always pair them.
- **Pointers to Radiant's objects** (`ModToolsEntity`, `ModToolsBrush`) are only valid until the map changes.
  Do not keep them across `OnMapLoaded` or `OnScopeChanged`; use GUIDs (`rp::EntityGuid`) to find objects again
  (`rp::FindEntitiesByGuid`, `rp::FindBrushesByGuid`). Every edit below also replaces the entity with a new one.
- Worldspawn is the exception: its GUID changes whenever anything is pasted. Recognize it with
  `host->WorldEntity`, never by GUID.

### The .map text

Everything the host reads and writes is Radiant's own `.map` text. One entity looks like this:

```
{
guid "{735B04E6-A236-4B5F-9EBF-1445C689864D}"
layer "Zones"
"classname" "info_volume"
"targetname" "start_zone"
// brush 0
{
 guid "{3B105DB8-72C9-431F-9BF0-B15F8F5E9804}"
 ( 1536 512 0 ) ( 1536 -1536 0 ) ( 1536 -1536 320 ) volume 64 64 1800 -136 0 0 lightmap_gray 16384 16384 -2044.5 -291.5 0 0
 ...one line per face...
}
}
```

- Braces stand on lines of their own. The entity's own lines come first: `guid` (unquoted), an optional
  unquoted `layer` line, then one `"key" "value"` line per key. Values never contain `"` or line breaks.
- Then its brushes, each `{ guid ... }` with one line per face (three points, then the material and its
  texture settings), or a `mesh { ... }` / `curve { ... }` block for patches and terrain.
- A point entity (a model, a light, a struct, a spawner) has no brushes in the text, only keys.
- The easiest way to learn the format: select something in Radiant, press Ctrl+C and paste into a text editor.
  That is the text `host->ImportMapText` takes, with a worldspawn block first.

### Editing entities

Radiant has no "set key" function. An entity is changed by replacing it: delete it, import its edited text,
then write its old GUIDs back so that other tools still find it. The `rp::` helpers do all of that as one undo step:

```cpp
ModToolsEntity e = rp::FindEntitiesByGuid({ guid })[0];
std::string cls = rp::GetKey(e, "classname");
std::string after = rp::SetKey(e, "script_noteworthy", "zone_a", "Set noteworthy");   // "" = refused, see the log
// e is gone now; look the entity up again by `after` (the same GUID, except for prefabs)

rp::ImportEntities("{\n\"classname\" \"script_struct\"\n\"origin\" \"0 0 64\"\n}\n", "Add struct");
std::string added = rp::SelectedEntityGuids()[0];                                     // the import selects it
```

`rp::SetKeys` sets and removes several keys at once, and `rp::ReplaceEntity` takes the whole new text when you
change more than keys. Their limits come from Radiant's paste, which they use:

- **worldspawn** and **reflection_probe** cannot be rebuilt; edit the `.map` file and reload it.
- **target and linkto keys** are kept as written since host 1.1.0 (`host->hostVersion >= 0x10100`). Radiant's own
  paste drops them, so on an older host `SetKeys`/`ReplaceEntity` refuse an entity that has one and
  `ImportEntities` creates entities without them.
- **misc_prefab** entities come back with a new GUID (the one returned).
- **Ctrl+Z** restores a point entity exactly. For an entity that owns brushes (a trigger, a volume) it brings
  the brushes back as world brushes without the entity, so keep such edits deliberate.
- Radiant adds a class's default keys (`_color` and the like) to what you import.
- All of this runs on the main thread, and the replaced entity is left selected.

### Reacting to changes

There is no callback for "the selection changed" or "an entity was added". Poll for it in `OnTick`, a few times
a second rather than every tick, and compare with what you saw last:

```cpp
static void OnTick(void)
{
    static ULONGLONG next = 0; static std::vector<std::string> last;
    if (GetTickCount64() < next || rp::host->IsReloading(rp::self)) return;
    next = GetTickCount64() + 250;
    std::vector<std::string> now = rp::SelectedEntityGuids();
    if (now != last) { last = now; /* the selection changed */ }
}
```

`host->GetSelection(self, nullptr, 0)` returns just the count and is the cheapest test. `host->IsMapModified` says
whether the map has unsaved changes, and `OnMapLoaded`, `OnMapSaved` and `OnScopeChanged` do fire as callbacks.

`host->BeginOwnWrite` and `host->EndOwnWrite` bracket your own writes to map files. `host->OwnWriteRecent` is true during one and for 1.5 s after, so a file watcher can tell your writes from external edits. `host->InhibitReloadPrompts` is for a plugin (live collaboration) that keeps the map in sync itself.

### The map file and prefabs

- `host->CurrentMapPath` is the open `.map`; `OnMapLoaded` and `OnMapSaved` tell you when it changes.
- `host->PrefabDepth()` is 0 while the user edits the map and more than 0 inside a prefab (Enter Prefab). The
  entity list is then the prefab's; `OnScopeChanged` fires on every switch.

### Camera and views

- `host->GetCamera` / `host->SetCamera` read and move the 3D camera.
- Draw in `OnDraw` only: `host->DrawWorldLine`, `host->DrawWorldTriangle` and `host->DrawLabel` (text), with
  `host->Project` to turn a world point into pixels. `ctx->view` says whether the 3D or a 2D view is drawing.
- The 3D view redraws all the time; the 2D views only when something changed. A 2D overlay that moves on its own
  calls `host->RequestRedraw(MODTOOLS_VIEW_2D)` from `OnTick`.
- `host->CamXYUpdateOnDrag` is Radiant's "Update XY on drag" preference. When it is off, the user asked the 2D views not to follow camera-like motion.


## 8. APE plugins

`api.apps` includes `MODTOOLS_APP_APE`. Everything APE-specific is in `host->ape` (`rp::Ape()`), which is NULL in
the other applications, so check it before use:

```cpp
static void OnAssetChanged(const char* name, const char* type)      // api.OnAssetChanged = OnAssetChanged;
{
    if (!name || !*name) return;                                    // no asset tab is current
    rp::Log("now editing %s (%s)", name, type);
    char props[65536];
    if (rp::Ape() && rp::Ape()->Properties(rp::self, props, sizeof props) < sizeof props)
        rp::Log("its property grid:\n%s", props);
}
```

- **The asset on screen**: `CurrentAsset`, the tree selection with `SelectedAssets`, the open tabs with `TabCount`
  and `TabAsset`. `OnAssetChanged` fires when the current one changes.
- **Its properties, live**: `Properties` lists the property grid of the current tab (one line per property:
  category, title, kind, value, ...). `SetProperty` writes one through its control, so APE treats it exactly like
  the user's edit (override state, unsaved tab, undo). `OpenAsset` opens another asset in the current tab.
- **The asset database** (gdt.db): `FindAssets` searches by name and type, `AssetTypes` and `AssetTypeNames` list
  types, `AssetInfo` says which `.gdt` file and line an asset is defined in, `TypeProperties` gives a type's
  property schema. These may be called from any thread; the database is APE's saved state, so assets created and
  not yet saved are not in it.
- **APE's Output pane**: `Print` adds a line (any thread), `OutputText` reads it back.
- The map, camera and drawing functions of section 7 do nothing in APE.


## 9. Launcher plugins

`api.apps` includes `MODTOOLS_APP_LAUNCHER`. The host runs inside the Mod Tools Launcher (`modlauncher.exe`) too,
where it adds the same "Plugins" menu. A launcher plugin has everything from section 6 and nothing more: there is
no map and no asset, and `host->ape` is NULL. That is enough for tools around the build and the mod: add a menu
beside "Plugins" with `host->AddMenu`, open settings with `host->CreateDialogWindow`, do the slow work on your own
thread and report back with `rp::PostToMainThread`. MTPlugins' own Launcher+ is a launcher plugin.

- Its log is `RadiantPlugins.launcher.log`; to debug, attach to `modlauncher.exe`.
- The launcher's own controls are plain Qt widgets: `host->MainWindow`, `host->MenuEntries` and
  `host->TriggerMenuAction` reach its window and menus without knowing its code.


## 10. When something goes wrong

### The log

Everything the host and the plugins log goes to `RadiantPlugins.log` (APE: `RadiantPlugins.ape.log`, launcher:
`RadiantPlugins.launcher.log`), in the folder that contains `RadiantPlugins.dll` (`<game>\bin\plugins` when MTPlugins
was installed with its Setup). Lines from your
plugin come from `rp::Log(...)` and start with its name. Look here first when a plugin does not load.

### The Plugins window

Select your plugin to see its state and, when it failed, why. The common ones:

| State | Meaning |
| ----- | ------- |
| Incompatible | `api.name` does not match the DLL name, or the plugin was built for another API version. The reason is shown. |
| Failed | `OnLoad` returned 0, or the plugin crashed (see below). |
| Incompatible: built with the old SDK | the DLL exports `RadiantPluginEntry` (the SDK was `radiant_plugin.h` until 2026-10-04). Rebuild it with this SDK. |
| Other application | `api.apps` does not include this application. Such plugins are only listed when "Show all plugins" is ticked. |

### Crashes

If your plugin crashes inside a callback, the host catches it: the editor keeps running, your plugin is marked
Failed, and a crash dump (`crash-<date>.dmp`) is written to your plugin's folder. Open the dump in Visual
Studio (File > Open > File, then "Debug with Native Only") with your `.pdb` next to the DLL to see the line
that crashed. Fix it, rebuild, and click Reload in the Plugins window.

Crashes on your own threads cannot be caught this way and close the application. Rule 3 in section 6 prevents most of them.

### Debugging with breakpoints

- **Attach** (always works): start the editor with plugins, then in Visual Studio choose Debug > Attach to
  Process and pick `Radiant_modtools.exe` (or `asseteditor_modtools.exe`, `modlauncher.exe`). Breakpoints in your code are hit
  once your plugin is loaded.
- **F5** (needs the loader, "Install plugins for the normal launcher.bat"): the project is set up to start
  Radiant under the debugger.
- While debugging you may want `isolate=0` under `[plugins]` in `RadiantPlugins.ini`. Then a crash is not
  caught by the host and the debugger stops right on the faulting line (the editor closes afterwards).


## 11. Sharing your plugin

Build in **Release**, then give people your plugin's folder: `<Name>\<Name>.dll`, plus your `.ini` if you have
one. They put it into `<game>\bin\plugins\` and tick it in the Plugins window. The project uses the static C++
runtime, so nobody needs a Visual C++ redistributable.

To let everyone find it, add it to the public plugin list: it is what **Browse plugins** in the Plugins window
shows, and people install and update your plugin from there with one click. Open a pull request on
https://github.com/Aidan601/MTPlugins-Index with your files in it, or with links to a GitHub release of them (its
README walks through it; a script there writes the entry with the file hashes for you). Your source does not have
to be public. Set `api.url` to your plugin's page, if it has one, so the Website button leads there.


## 12. Compatibility

- The interface only grows: new functions are added at the end of the tables and existing ones keep working.
  A plugin built with this SDK keeps working with newer versions of the host.
- A plugin built with a **newer** SDK can meet an **older** host that lacks a function. Before calling a function
  that is newer than what your users may have installed, check it with `rp::Has(&rp::host->FunctionName)` (and
  `rp::HasUi(&rp::host->ui->FunctionName)` for the widget helpers). The header notes when functions were added.
- `MODTOOLS_PLUGIN_API_VERSION` changes only for a breaking change. The host then lists older plugins as
  Incompatible until they are rebuilt; this is announced with the release.
- **Expert functions**: `RadiantAddress`, `Detour` and `host->ape->ApeAddress` give raw access to the editor's
  machine code. They depend on the exact editor build and on reverse engineering; they are not needed for
  normal plugins and nothing about them is guaranteed to stay the same. `ADVANCED.md` explains how to use them
  safely and how to find more, and `MODTOOLS_REFERENCE.md` holds what is already known.
