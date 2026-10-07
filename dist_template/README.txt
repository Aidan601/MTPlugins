Radiant Plugins (Black Ops III mod tools: Radiant and APE), test build
=====================================================================

What this is
------------
A plugin system for the Black Ops III mod tools editors: Radiant (the map editor) and APE (the Asset
Property Editor). A plugin host is loaded into the editor; it adds a "Plugins" window (Ctrl+Shift+P) and
loads the plugins found in <game>\bin\plugins. Nothing in the mod tools is replaced. Two ways to get it in:
the "Launch ... with Plugins" .bat files start an editor with the host loaded, or run "Install plugins for
the normal launcher.bat" once and the normal mod tools launcher (Mod Tools > Radiant / Asset Editor),
Steam and shortcuts all start the editors with plugins. The plugins in this package:

  Team Create   several mappers in the same map at the same time: you see each other's cameras and
                selections in the 3D view, brush and entity edits are copied to everyone within a
                fraction of a second, and there is a chat.
  Radiant+      fixes to Radiant itself; for now it removes the hidden 60 FPS cap so Preferences > Camera >
                Max FPS is honoured.
  Launcher+     (was Build Plus; its build part is BetterBuild)
                faster Compile and Link from the Mod Tools Launcher, and themes for it (Edit > Options). The launcher runs the stock cod2map64 and
                linker, but with faster versions of their slowest steps loaded in memory; the shipped tools
                are never modified and the compiled map is the same. A large map (The Giant) compiles in
                about 45 s instead of 160 s. The Launcher+ menu in the launcher switches Compile and Link;
                Edit > Options has a Theme list in place of "Use Treyarch Theme" (the themes of Radiant+ and APE+).
                Publish Mod (Ctrl+P) checks the thumbnail against Steam's rules (PNG, JPG or GIF under 1 MB) and has
                a Map Description (shown in game) apart from the Workshop Description (Steam page, formatting
                buttons and a preview).

Requirements
------------
- Windows 10 or 11, 64-bit.
- Call of Duty: Black Ops III Mod Tools installed from Steam (the current Steam version; the host checks
  the exact Radiant build and stays inactive on any other one).
- For Team Create: a network connection to the other mapper (same LAN, or a VPN such as Radmin VPN,
  Hamachi or ZeroTier), and everyone needs the SAME map file at the SAME place inside the mod tools
  folder, e.g. ...\Call of Duty Black Ops III\map_source\zm\zm_mymap.map, plus any custom prefabs
  the map uses. Copy those files from the host before the session.

Setup (2 minutes)
-----------------
0. If Windows or your browser warns about the download: the launcher is a small unsigned program
   that starts Radiant and loads code into it, which is exactly what virus heuristics look for.
   Windows Defender on the build machine reports the files as clean. Keep the file in the browser
   ("Keep" / "Keep anyway"), and if SmartScreen shows "Windows protected your PC" when you start the
   .bat or the .exe, click "More info" and then "Run anyway". If Defender quarantines a file, restore
   it in Windows Security > Virus & threat protection > Protection history and add the folder under
   "Exclusions". Only do this for a copy you got directly from the author.
1. Unzip this folder anywhere, for example C:\RadiantPlugins. Keep the files together.
2. Double-click "Launch Radiant with Plugins.bat". It first copies the plugins folder into
   ...\Call of Duty Black Ops III\bin\plugins (only files that are new or newer; your settings are kept),
   then starts Radiant. After the map loads the 3D view shows a small "TEAMCREATE OFF" hint.
   To open a map right away, drag a .map file onto that .bat file. "Launch APE with Plugins.bat" does the
   same for the Asset Property Editor.
   Prefer the normal mod tools launcher? Run "Install plugins for the normal launcher.bat" once instead: it
   adds a small d3d9.dll next to the editors that loads the plugin host from this folder (keep the folder
   where it is). Undo with "RadiantPlugins_Launch.exe --uninstall-loader".
   If a message says Radiant was not found, open RadiantPlugins.ini in Notepad and set
       [radiant]
       root=C:\Program Files (x86)\Steam\steamapps\common\Call of Duty Black Ops III
   to your game folder.
3. Press Ctrl+Shift+P (or use the "Plugins" menu) to open the Plugins window: every plugin in the
   folder with a check box (tick = loaded), its status, and Reload / Refresh / Open folder / Settings.
   A plugin made for the other editor shows as "Radiant only" or "APE only" there.

Team Create: hosting a session (one person)
-------------------------------------------
1. In Radiant open the map everybody agreed on.
2. Press Ctrl+Shift+T (or use the "Team Create" menu). In the pane pick "Host a session on this PC",
   type your name, leave the port at 27017, click Host.
3. Windows may ask whether Radiant may accept connections: choose Allow (private and public networks).
4. Tell the others your IP address on the LAN / VPN (Radmin VPN shows it next to your name).

Team Create: joining (everyone else)
------------------------------------
1. Open the same map file (same path) in Radiant.
2. Ctrl+Shift+T, pick "Join a session", type the host's IP address and your name, click Join.
3. Within a few seconds the pane lists everyone connected, the host's copy of the map is applied to
   yours, and the other mappers' cameras appear as coloured frustums with their names in the 3D view.

Team Create + Play Test: playing together
-----------------------------------------
With Play Test installed on every PC, the host presses Play in the Play Test pane. Everyone else's
Play button turns into Join Host: it fetches the host's build and takes them into the host's game.
Edits made in Radiant show up in the running game for everyone (Live Edit, BO3 Enhanced only).

What to try
-----------
- Move around: the others see your camera. Select something: they see it boxed in your colour.
- Create, move, copy, paste or delete brushes and entities. The change appears on the other
  machines as soon as you deselect it (click in empty space or press Escape). Undo works the same way.
- Select something another person is holding: it is boxed in red with their name, and the status
  line says LOCKED BY. Nothing is blocked, last change wins, it is only a warning.
- Chat: type in the pane and press Enter. Messages also show in the 3D view for a few seconds.
- Diagnostics: while you are connected, the plugin's log lines (RadiantPlugins.log: what the plugins did,
  never map content or anything outside the editor) are streamed to the other mappers, and when you join,
  any crash dump Radiant wrote in the last three days is sent to them too. That way a crash on one machine
  can be looked at from another one without anybody collecting files. The received files sit in
  bin\plugins\TeamCreate\peerlogs\<name>\. To switch it off set share_logs=0 under [diag] in TeamCreate.ini.
- Enter a prefab (Prefab menu): edits inside it reach the others too.
- Save as usual with Ctrl+S. Each person saves their own copy; the copies stay identical while
  everyone is connected.
- Untick a plugin in the Plugins pane to switch it off; tick it again to load it.

Known limits of this build
--------------------------
- A change is sent when the object is deselected, not while you are dragging it.
- Edits to worldspawn key/values (Map Info) and a few rarer cases reload the map on the other
  machines instead of applying in place; the camera is kept, the reload takes a few seconds.
- If the host clicks Stop or closes Radiant, everyone else gets "The host ended the session" and is
  back to Join; they keep their copy of the map and can save it.
- Textures, models or GDT assets that only one person has are NOT sent. Stick to assets both
  installs have (stock assets, or copy custom ones over first).
- Do not run this on a map you have not backed up. It has been tested, but it is a test build.

Writing your own plugins
------------------------
The SDK folder has everything needed to write plugins for Radiant and APE in C or C++ with Visual Studio 2022
or newer (the free Community edition, "Desktop development with C++"). Start with SDK\GETTING_STARTED.md: it
builds the HelloPlugin example, shows it running, and explains how plugins work. SDK\NewPlugin.bat makes a new
plugin project under the name you choose; building it copies the DLL straight into bin\plugins.

If something goes wrong
-----------------------
- The Team Create pane says "Reconnecting": the host is unreachable (VPN dropped, host closed Radiant, firewall).
- Nothing arrives on the other machine: the status line in the pane counts edits sent and received.
  If it says "The other side has '...' open, you have '...'", the two of you are not in the same map
  file: open the same one. Also check that the host allowed Radiant through the Windows Firewall.
- A plugin shows "Failed" in the Plugins pane: it crashed and was switched off; Radiant keeps running.
  Send the crash-*.dmp from that plugin's folder (bin\plugins\<Name>\) with the log.
- Radiant crashes, freezes or nothing happens: tell the host what you did (a screenshot helps) and
  rejoin the session once: your log and Radiant's crash dump reach the host automatically (see
  Diagnostics above). Without a session, send RadiantPlugins.log from this folder and the newest
  radiant_modtools.exe*.dmp from ...\Call of Duty Black Ops III\bin\ yourself.
- To use Radiant without plugins, start it from Steam or the mod tools launcher as usual.

Files in this folder
--------------------
Launch Radiant with Plugins.bat   double-click this (or drop a .map on it)
RadiantPlugins_Launch.exe         installs the plugins folder, starts Radiant and loads the host ("--where" prints where Radiant was found)
RadiantPlugins.dll                the plugin host
RadiantPlugins.ini                host settings
RadiantPlugins.log                appears after the first run; send it with bug reports
plugins\TeamCreate\               the Team Create plugin and its settings (name, port, last session)
plugins\RadiantPlus\            the Radiant+ plugin (removes Radiant's hidden 60 FPS cap) and its settings
SDK\                              for writing your own plugins: start with SDK\GETTING_STARTED.md
plugins\APEPlus\                the APE+ plugin (fast asset tree filter, no freeze after a committed value, themes in View > Preferences) and its themes folder

CHANGES
-------
2026-09-25
- Light bakes at Low and Medium now come out as bright as a High bake. Radiant reads blurrier probe images at the
  lower qualities, which made Low up to about 1.4 times too bright on our test maps (and single probes on a big
  map up to 1.6 times too bright or too dark). Radiant Plus now samples one voxel in eight a second time at
  High's sharpness and corrects each probe by the difference. It costs about 5 % more bake time at Low; High bakes
  are unchanged. In the editor, cone=0 in RadiantPlus.ini goes back to Radiant's own brightness.
- Build Plus: the launcher's Light step now runs through Build Plus too (the new "Light with BuildPlus" switch
  in its menu, on by default). It applies the same correction, bakes a bit faster (the same result, less repeated
  work between bounces) and ends right after the lighting file is written instead of spending seconds shutting
  down. Untick the switch for Radiant's own Light step.
- Clicking Build Lighting again right after cancelling a bake could crash Radiant; that click now waits until the
  cancelled bake has really stopped.
2026-09-24
- New plugin Build Plus (Mod Tools Launcher): Compile runs 3 to 4 times faster on large maps, Link about a
  fifth faster (much more right after a Windows Defender update, when it re-scans every file). It only
  works with the current Steam cod2map64 and linker builds; on any other build it quietly runs the stock code and
  says so in the launcher log. Every build starts with a "[BuildPlus] ..." line in the log showing what is on.
  To use the stock compiler or linker while keeping the plugin, untick "Compile with BuildPlus" or "Link with
  BuildPlus" in the launcher's BuildPlus menu.
  Windows Defender scans every file a build opens, which adds tens of seconds to a link right after each
  Defender update. The first time the launcher starts with the plugin it offers to tell Defender to skip the
  three build programs (cod2map64.exe, linker_modtools.exe and snd_convert.exe in the game's bin folder);
  Windows asks for administrator permission once. Nothing else is excluded, nothing happens without that click,
  and "Skip Windows Defender scans while building" in the launcher's BuildPlus menu undoes it.
- Fixed a crash that could take Radiant down with no warning while you worked in a map (the 3D view froze and
  Radiant closed). Direct3D sometimes swaps the way it dispatches draw calls partway through a session, and the
  plugin host then watched that draw call twice, which made the two watchers call each other until Radiant ran
  out of stack. The host now keeps a single watcher per draw call, and stops any nested draw before it can
  repeat. Nothing to change on your side.
- New SDK folder for writing your own plugins without the plugin system's source code: the plugin headers, the
  HelloPlugin example as a project that builds on its own with Visual Studio 2022 or newer (or CMake) and copies
  the DLL into bin\plugins, NewPlugin.bat to start a new plugin, and a step by step guide (GETTING_STARTED.md).
2026-09-17
- Team Create: property edits now show up for the others while you still have the object selected (a light's
  colour, a model's name, any key in the entity window), about a second after you change them. Before, they
  only arrived when you deselected it. [sync] live_keys=0 in TeamCreate.ini goes back to the old behaviour.
2026-09-14
- Team Create: streamed models and images now show up much sooner. Along with the files, the host also sends
  the preview copies its Radiant already made (Radiant's "Converting image" / "Converting xmodel mesh" step),
  so your Radiant skips that step for them. Big textures gain the most. [assets] send_converted=0 in
  TeamCreate.ini turns it off.
- Team Create: fixed downloads slowing to a crawl in bigger sessions. The host kept resending the whole asset list
  every 10 seconds (a single material description is about 16 KB), which filled its upload. Each list is now sent
  once, and a file that Radiant or gdtdb had open for a moment no longer cancels an asset.
- Team Create: "Sync light builds", a new check box in the Team Create pane. When you build lighting (the
  lightning bolt, or Rebuild Sun / Probes / All under it) on the shared map, everyone else who has it ticked
  starts the same build on their PC. Each PC bakes with its own CPU. Pausing and cancelling are not shared,
  and a mapper with real lighting off, another map open or inside a prefab gets a chat note instead.
- Team Create: your camera now shows for the others right after you join. Before, the crash logs sent on join
  could hold it back until you left and joined again.
- APE: fixed a crash a few seconds after starting APE (before its window appears). APE saves its GDT file
  cache with several threads at once and they could free the same memory twice; it only happens after a GDT
  changed, which Team Create's streamed assets make common. The plugin host now prepares that list safely
  before APE saves it. If APE already crashed this way, just start it again.

2026-09-13 (late night)
- Team Create: a "Hide" box next to the host's IP address in the pane, ticked by default. The address shows as
  dots and the status line says "the host" instead, so it stays off a stream or screen share. Pasting into the
  hidden field still works; untick it to see what you typed.

2026-09-13 (night)
- Team Create 1.3: source asset streaming (the M4 milestone). Custom models, materials and images that the
  host's map uses now reach every mapper during the session: they land in <root>\__streamed_assets (registered
  once in bin\converter_gdt_dirs_0.txt, backed up first) and in one shared streamed_assets.gdt, so Radiant
  converts and shows them without a restart. When an asset differs between mappers the host's copy wins; any
  file replaced on your PC is backed up first under %LOCALAPPDATA%\RadiantPlugins\TeamCreate\asset_backups.
  Saving your map keeps the streamed assets it uses: they move into the normal folders (model_export,
  texture_assets, ...) and their entries into the GDT file they came from on the other mapper's PC. Assets no
  saved map uses are removed when the session ends. Identical assets you already have are never touched.
  The session pane shows the asset status; "TeamCreate assets" on the command channel prints it.
  First tester build: please report anything odd in __streamed_assets or your GDTs (log lines start with
  "assets:").
- Fix from the first tester run (same night): a streamed model arrived without its materials. The material
  names inside most .xmodel_bin files were skipped by the reader (a padding detail in the file format), so
  the materials and their images were never sent. Both the host and the joiner need this build.
- Fix (same night): after a crash or an editor closed mid-session, the next session refused to stream
  ("unfinished asset session: restore files listed in __streamed_assets/recovery.txt") until someone
  cleaned up by hand, so the host offered nothing. It now cleans up on its own at the next session start:
  temporary definitions removed, your own GDT entries put back, anything edited copied to the backups.
- Fix (same night): a model whose file sits outside model_export (a GDT filename like
  ..\_custom\...\door.xmodel_bin) was sent without its file, and Radiant then reported "Failed loading
  xmodel file". Such references are now resolved and mirrored like the rest; a model whose file really
  is missing on the sender is reported in the sender's log instead of being sent empty.
- Fix (same night): a model's "skinOverride" in its GDT is now honoured when working out what it needs, so an
  overridden material (and its images) is no longer sent. Before, the stock skybox pulled the mapper's
  mtl_skybox_default with a 201 MB image into every session. A material a model names that the sender has no
  definition for is left out (and logged on the sender) instead of blocking that model's save on the other side.
  Asset names that start with a digit are now recognised in map text too.
- Faster streaming (same night): the smallest assets arrive first (a model no longer waits behind a big
  texture), several pieces of a file travel at once (several times the speed over a VPN), the database
  refresh no longer pauses transfers, and the Team Create pane shows what is being received or sent with a
  percentage and speed. Anything over 100 MB of source files is called out in the log when it really starts
  moving (a stock file both PCs already have is never sent, so it gets no warning).
- Fix (same night): an image or model whose file is missing on the sender's own PC (ported packs often point at
  an _images\$identitynormalmap.png that never existed) was refused with "cannot stream ...: missing source
  file", which also stopped the material using it from being kept on save. The definition is now sent without
  the file, exactly as it is on the sender, and the sender's log says so once.
- Faster still (2026-09-14): files you have received before are kept in a cache outside the game folder
  (%LOCALAPPDATA%\RadiantPlugins\TeamCreate\asset_cache, 4 GB, oldest dropped), so rejoining a session or
  joining a second one never downloads the same file twice. Textures now travel compressed between two PCs on
  this build (two to four times fewer bytes for TGA, TIF and EXR); an older build on the other side still works,
  just uncompressed.
- Streamed models now appear while their textures are still arriving: the asset database is refreshed every
  25 seconds during a long transfer instead of only at the end (2026-09-14).
- Crash dumps and logs shared between mappers no longer slow a session down (2026-09-14): they are sent in small
  pieces behind camera updates, edits and asset transfers, each dump goes to each mapper only once, the large
  Windows copies of editor crashes (over 10 MB) stay on your PC, and the shared log no longer repeats itself
  after a rejoin.
- The session status and the asset transfer progress are now shown in the top-left of the 3D camera view,
  under the session line and above chat, instead of in the Team Create window (2026-09-14). The window only
  shows a message when one of its own buttons needs attention.
- Joining is quicker (2026-09-14): the host used to wait up to 10 seconds before telling a new mapper which
  assets exist; it now tells them at once.
2026-09-13 (afternoon)
- Prefabs, three fixes: the "outside prefab" note under another mapper's name shows only while you are inside
  a prefab and they are not (no more notes under everyone at the map level); a mapper who is inside a prefab is
  now drawn where that prefab instance really sits (its position and rotation were ignored before, so their
  camera and selection looked misplaced); and a prefab someone drags into the map during the session now
  appears for everyone even when the prefab file was not on your PC yet (Team Create fetches the file from the
  mapper who placed it and drops it into your _prefabs folder, or next to a temporary map copy).
- New plugin APE Plus (loads into the Asset Property Editor): the asset tree's name/type/GDT filter boxes no
  longer take seconds per keystroke on a big database, the pause after every committed value is gone, and a
  Theme list in View > Preferences (in place of "Use Treyarch Theme") offers Classic, Treyarch, Dark (when
  bin\t7x_themes exists) and any .qss you drop into plugins\APEPlus\themes (see the README there).
- APE Plus: closing the Asset Property Editor no longer ends in a crash report (its disk watcher was still
  running while APE shut down; nothing was lost, but Windows logged a crash on every close).
- Joining takes you to the host's map: if you have another map open, Team Create opens the host's map from the same
  place under map_source on your PC (it asks first only when your map has unsaved changes). If you do not have
  that map at all, it gets it from the host and opens it as a temporary copy in map_source\_teamcreate_temp\.
  When the session ends or you close Radiant with such a copy open, it asks whether to save it on your PC
  (Save Map As, pick a place under map_source) or to discard it.
- Team Create no longer starts a session by itself when Radiant opens; host or join from the pane (Ctrl+Shift+T).
- Prefabs come along: when you join, the prefabs the host's map uses are compared with yours; any you lack or that
  differ are fetched from the host into map_source\_teamcreate_temp\_prefabs\ (prefabs inside prefabs included) and
  your temporary copy of the map uses them from there, so you see what the host sees. Your own _prefabs files are not
  touched. If you save the temporary map on your PC, those prefabs move into _prefabs (a file they replace is first
  copied to radiant\teamcreate_backup). Models, materials and images are not sent yet.
- You can see where everyone is editing: when a mapper is inside a prefab, the Team Create pane shows the prefab's
  path next to their name (nested prefabs as "outer > inner"), and in the 3D and 2D views their camera label says
  "in prefab <file>" or "outside prefab". A camera inside a prefab is placed in that prefab's own space, so it may
  not line up with the map you are looking at. Both sides need this package for it to show.
- New option in the Team Create pane: "Sync saves". When a mapper who has it ticked saves the shared map (Ctrl+S or
  File > Save), everyone else who has it ticked saves their copy too, and a line in the chat says so. It waits
  until the latest edits have arrived, and until you are out of a prefab if you are inside one. Prefab files,
  Save As to another file, autosaves and a temporary copy of the host's map are not affected. Off by default.
- Fixed: deleting many objects at once (Shift+A on a model, then Delete) removed them only for you. Team Create took
  a delete of more than a quarter of the map for a sync error and held it back, and then held back every later
  delete too. Anything you had selected is now always shared when you delete it. If Team Create ever does hold
  deletes back, the status line says "N DELETES HELD" and TeamCreate's "deletes send" command releases them.
- Fixed: Ctrl+Z could delete an object another mapper had just changed (you move a model, they move it, you press
  Ctrl+Z, it vanished for everyone). Changes that arrive from other mappers are no longer steps of your own undo:
  Ctrl+Z undoes your own edits only (your undo of that move now puts the model back where it was before you
  moved it, for everyone), never removes something another mapper created, and incoming edits no longer clear
  your redo.

2026-09-12 late
- When another mapper drags or nudges something, you now see the real brush, model or prefab move on your
  side as they go (boxed in their colour), instead of a box that only snaps into place when they let go.
  Resizes, rotations and vertex edits of brushes follow too, and a brush someone is still dragging out or an
  entity they just placed appears on your side right away (models and prefabs: moves only). [sync] live_move=0
  turns the live part off; the finished edit still arrives when they deselect.
- Fixed a crash after a plugin reload (Radiant re-read an undo label that belonged to the unloaded plugin).
- Different maps are caught when you join: if the host has another map open than you, Team Create asks
  "Open <the host's map> now?". Yes opens it (your unsaved changes would be lost, so it tells you to save first
  when there are any); No keeps your map, cameras and chat still work, edits are not shared until the maps
  match. The same question comes up when the host opens another map mid-session. The host's pane says which
  players are on another map.
- Before Team Create rewrites your map file on disk for the first time in a session (that happens when a
  change from another mapper needs a reload, or when someone edits a prefab you are not inside), it copies
  the file to <Black Ops III>\radiant\teamcreate_backup\<map>_<date_time>.map. The newest 20 copies per map
  are kept ([backup] keep in TeamCreate.ini, 0 = keep all). Radiant's own autosaves in radiant\autosave_backup
  are not touched (Radiant never deletes those itself; clear that folder by hand if it has grown).

2026-09-12 night (this package)
- New plugin, Radiant Plus: Radiant no longer stops at 60 FPS. Preferences > Camera > Max FPS is honoured now
  (set it to your monitor's refresh rate; 0 = no limit, uses more CPU). Untick it in the Plugins window to go back.
- Pick your player colour: click the colour next to "Player color" in the Team Create pane and Radiant's own
  colour picker opens. The others see your camera, name and selection boxes in it, and
  every name in the Connected list has a dot in that player's colour. Both sides need this package for a
  chosen colour to show; with an older package the colours stay automatic.
- Team Create's text in the 3D view (status line, chat, names, "HAS IT") now uses Radiant's own HUD font,
  the one of "RUNTIME LIGHTING" and the FPS counter. engine_text=0 under [render] in RadiantPlugins.ini
  brings back the old line font.

2026-09-12 evening
- No more "Lighting changed. Do you want to export?" boxes in the middle of a session (the plugin's own map
  reloads answer it for you; File > Open still asks), and no more vanishing mouse pointer after one.
- Fixes Radiant crashing a few seconds after the other mapper added or rebuilt entities and then deleted
  them (for example a batch of misc_models placed and removed, or volumes being moved). The synced entity
  stayed behind in Radiant's Entity List as a dead row and Radiant crashed the next time the selection
  changed. Everyone in a session should update, the fix is in RadiantPlugins.dll.
- Fixes a crash report every time Radiant was closed while hosting a Team Create session (the same kind
  of shutdown crash is also prevented in Map Reload). Nothing was lost, but Windows logged a crash.
- Team Create 1.2: diagnostics sharing (logs and crash dumps travel between the connected mappers, see
  "What to try"; share_logs=0 in TeamCreate.ini switches it off).
- Host update: menu and undo helpers for plugins, APE property access (used by developer tools; no change
  in how Team Create or Map Reload behave).

2026-09-11
- Map Reload 1.1: fixes a crash of Radiant on Ctrl+S (the file watcher could abort right after
  Radiant's own save). Everyone on an older package should update.
- Host: plugins also load into APE (the Asset Property Editor) through the same d3d9.dll loader.
