# APE+

Fixes and improvements for APE, the Black Ops III Asset Property Editor. Nothing in APE's own files is changed, and turning APE+ off in the Plugins window (Ctrl+Shift+P) puts APE back the way it was.

## Speed

- Searching the asset tree is much faster, even with a large GDT library (milliseconds instead of seconds per key).
- APE no longer rescans every folder after each value you change, unless something on disk changed.
- Assets open faster (a weapon camo in about a second instead of thirteen), and changing a value does not lag.
- The first material after APE starts opens without the long freeze (about 23 seconds down to 6).
- XModel Info and the Preview window build the auto LODs of big models several times faster.
- Previewed auto LODs match the ones Launcher+ builds, so what you see is what the game gets.

## Editing

- More values and cleaner property layouts for bullet, projectile and dual wield weapons and for weapon camos.
- JPEG images can be used as image sources.
- The Log actions box in the Output pane's title bar lists what you changed: each property with its old and new value.
- The Select Color dialog takes hex and 0-1 colors, sets light colors by Kelvin and copies the color as hex or 0-1.
- The Preview window plays an animation's sound notetracks (GDT Sound notes and exported `sndnt#` notes) from your alias csv files and `sound_assets`. Edits to the alias files and sound files are picked up on the next play.
- APE's internal check boxes no longer close APE: choose Yes to keep working and save your changes (`[crashguard]` in APEPlus.ini). Bad rotations in the Preview window are caught before they fail a check.
- Viewmodel animations (Use Bones off) no longer look stretched in the Preview window, and the AssetViewer Preview Options attach works: the Child Tag is used, swapped tags are fixed with a note in the Output pane, and an attached gun follows the animation like in game.

## Look

- Themes under View > Preferences > Theme, with a theme editor (Customize...) and icon packs. Themes are shared with Radiant and the launcher (`bin\themes`).
- Custom Sky in the Preview window's Rendering menu: preview a model under any skybox with its own lighting.
- The Vulkan renderer (DXVK) under View > Preferences, shared with Radiant.

## Discord

- Show APE on your Discord profile under View > Preferences > Discord presence: Off, Show the tool, or Show the tool and what is open (the asset name and type). Show the tool is the default.

## Settings

Most of the above is set in View > Preferences. The rest lives in `APEPlus.ini` next to `APEPlus.dll`, with one comment line per key:

- `[filter]` the asset tree search
- `[commit]` the rescan after a changed value
- `[theme]` the theme
- `[preview]` the Preview window (frame rate, Custom Sky, faster model conversion)
- `[images]` extra image formats
- `[open]` faster asset opening
- `[startup]` the first material without the freeze
- `[deffiles]` APE+'s own property layouts in `plugins\APEPlus\deffiles` instead of the installed ones
- `[notesounds]` notetrack sounds in the Preview window
- `[xanimpreview]` the viewmodel and attach fixes in the Preview window
- `[actionlog]` the action log and how much of it goes into problem reports
- `[colorpicker]` the Select Color extras
- `[discord]` Discord presence

## Problems

Plugins > Report a problem... collects the logs and crash dumps into a zip on your Desktop. Attach it to a new issue at https://github.com/Aidan601/MTPlugins/issues.
