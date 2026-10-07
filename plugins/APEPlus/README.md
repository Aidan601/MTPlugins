# APE+

Fixes and improvements for APE, the Black Ops III Asset Property Editor. Nothing in APE's own files is changed, and turning APE+ off in the Plugins window (Ctrl+Shift+P) puts APE back the way it was.

## Speed

- Searching the asset tree is much faster, even with a large GDT library (milliseconds instead of seconds per key).
- APE no longer rescans every folder after each value you change, unless something on disk changed.
- Assets open faster (a weapon camo in about a second instead of thirteen), and changing a value does not lag.

## Editing

- More values and cleaner property layouts for bullet, projectile and dual wield weapons and for weapon camos.
- JPEG images can be used as image sources.
- The Log actions box in the Output pane's title bar lists what you changed: each property with its old and new value.

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
- `[preview]` the Preview window (frame rate, Custom Sky)
- `[images]` extra image formats
- `[open]` faster asset opening
- `[deffiles]` APE+'s own property layouts in `plugins\APEPlus\deffiles` instead of the installed ones
- `[actionlog]` the action log and how much of it goes into problem reports
- `[discord]` Discord presence

## Problems

Plugins > Report a problem... collects the logs and crash dumps into a zip on your Desktop. Attach it to a new issue at https://github.com/Aidan601/MTPlugins/issues.
