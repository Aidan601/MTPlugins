# Radiant+

Fixes and improvements for Radiant, the Black Ops III map editor. Nothing in Radiant's own files is changed: everything happens while Radiant runs, and turning Radiant+ off in the Plugins window (Ctrl+Shift+P) puts Radiant back the way it was.

## Speed and stability

- The 60 FPS cap is gone. Set your limit under Preferences > Camera > Max FPS (144 by default), and every view keeps to it.
- Maps load faster.
- Smoother camera movement in the 3D view, without the short stall about once a second.
- A faster 3D view when Game View is off.
- Smoother 2D views on big maps: small objects are left out while you pan, zoom or drag, and the full view comes back as soon as you stop.
- Guards against crashes Radiant is known for. An assert box lets you continue instead of closing Radiant.

## Lighting

- Light bakes are faster, and Low and Medium come out closer to High.
- The LL button in front of L on the lighting toolbar bakes a fast draft for quick checks.
- A rebake with unchanged geometry skips work the last bake already did.

## Camera

- Game View field of view under Preferences > Camera > GameView FOV.
- Hover a Camera Movement Mode to see its controls.
- Make your own camera movement presets with Edit Camera Presets... at the end of the same list.

## Editing

- A rotation gizmo for rotating objects, toggled under Preferences > Selection > Rotation gizmo rings.
- In the Legacy FX Browser, a click on an effect sets it on the selected fx entities.
- The Icon Size sliders of the Models, Textures and FX Browser panes resize the tiles while you drag them, without the lag.
- A selected model shows just its outline in the 3D view, seen through walls. Alt+J or View > Toggle Model Outline switches to Radiant's wireframe and back.
- Right-click a prefab or folder in the Prefab Browser > Show in Explorer.
- The color dialog (Entity Color and the other color prompts) takes hex and 0-1 colors, sets light colors by Kelvin, copies the color, shows its eyedropper with an icon and keeps the recent colors row between sessions.
- The Log actions box beside Clear in the Output pane lists what you did and how.
- Auto LODs shown in the views match the ones Launcher+ builds, so what you see is what the game gets.

## FX Browser

- Every effect shows as a thumbnail that plays on a loop, with a ground grid and X, Y and Z axes. Clips are filmed as effects come on screen and kept in `radiant\fx_thumbs`.
- Click an effect to play it in the preview box beside the grid. Drag in the box to orbit, the wheel zooms, a middle drag pans, a double-click resets the view. Loop restarts the preview when it ends.
- Double-click an effect to set it on the selected fx entities, or drag it into the 3D view to place a new one.
- The top bar works like the Models pane: All, All In Use, All In Use Top, Favorites (right-click an effect to add it, shared with the Legacy browser) and a name filter. The Icon Size slider is at the bottom.
- Options: Play All (every thumbnail plays, effects that load models are skipped), XYZ Grid, Reset Preview Camera, Refresh.
- Tick Legacy (bottom left) for Radiant's own FX Browser.

## Look and settings

- Themes and a theme editor under Preferences > Color > Theme, with matching view color presets.
- The Vulkan renderer (DXVK) under Preferences > Camera > Use DXVK (Vulkan) renderer, often a higher frame rate. It applies from the next start and is shared with APE.
- The Windows file dialogs for Open, Save and Import under Preferences > System > Use Windows file dialogs.
- Preferences stays open while you keep working in Radiant.
- Image formats beyond TIFF, PNG and EXR for textures (`[images] extra_formats`).

## Discord

- Show Radiant on your Discord profile under Preferences > System > Discord presence: Off, Show the tool, or Show the tool and what is open (the map name and kind). Show the tool is the default.

## Settings

Everything above is set in Radiant's own Preferences where possible. The rest lives in `RadiantPlus.ini` next to `RadiantPlus.dll`, with one comment line per key:

- `[fps]` the frame rate cap
- `[bake]` light bake quality and speed
- `[performance]` the speed-ups, each one can be switched off
- `[gizmo]`, `[camera]`, `[camera_presets]` the rotation gizmo and the camera
- `[fx]`, `[fxpreview]`, `[prefabs]`, `[selection]`, `[files]`, `[preferences]` the editing and dialog changes
- `[actionlog]` the action log and how much of it goes into problem reports
- `[crashguard]` each crash guard, on or off
- `[images]`, `[theme]`, `[colors]` image formats, theme and view colors
- `[discord]` Discord presence

## Problems

Plugins > Report a problem... collects the logs and crash dumps into a zip on your Desktop. Attach it to a new issue at https://github.com/Aidan601/MTPlugins/issues.
