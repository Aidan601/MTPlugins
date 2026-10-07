Themes for Radiant and APE (MTPlugins)
======================================

This folder (<game>\bin\themes) is shared by Radiant and APE. Every <name>.qss here is a theme; pick one in
Radiant under Settings > Preferences, Color page, "Theme:", or in APE under View > Preferences > Theme. The list is
read each time Preferences opens, and the "Themes Folder" button next to it opens this folder. Radiant and APE each
remember their own choice (RadiantPlus.ini and APEPlus.ini, [theme] name=).

Themes that come with MTPlugins, each with its own background, accent and icon colour. Dark ones: Ember (near
black, amber), Dark (grey, blue), Midnight (navy, teal), Plum (purple, orchid), Forest (deep green, lime),
Figglebottom (the dog's browns and tan, and the dog as every icon) and Slate (medium blue-grey, coral). Light ones:
Light (white, blue), Sage (green-grey, green), Paper (sepia, terracotta) and Rose (pink, crimson). Steam Classic
is the olive green of Steam in 2004 and Counter-Strike 1.6's menus, with bevelled square buttons and gold highlights.
In the look of other programs: Steam Modern (today's Steam client: slate greys, grey buttons that light up, its blue,
the green Play button), Blender and Maya.
Each redraws the editors' icons in its own icon colour (see Icons below). Graphite, Fog and Frost were replaced by
Ember, Sage and Light, Photoshop was dropped for Dark (a copy you saved yourself under those names
stays).
Treyarch is the stock Treyarch look with modern controls: every colour of Radiant's own sheet, the editors' own icons.
The list also has "Classic Treyarch" (Radiant's own radiant\stylesheet.qss, called Treyarch before) and "Classic"
(Qt's plain Windows look, no stylesheet). The list starts Light, Dark, Treyarch, Classic, Classic Treyarch.
The first time Radiant, APE or the Mod Tools Launcher opens with MTPlugins, it picks Treyarch where it showed the
Treyarch look and Light where it showed Classic; after that your own choice stays.

MTPlugins puts its themes back when it updates, unless you changed the file. To make your own, copy one under a new
name (for example My Dark.qss) and edit that.

Make your own: in Radiant, Settings > Preferences > Color, the "Theme" page tab (APE: View > Preferences, Customize...).
It shows the theme in effect; click a swatch and the change shows at once ("Edited (not saved)" under Theme; picking a
theme there and pressing OK puts it back). Save As... keeps your colors as a theme.
It writes <name>.qss (and <name>.colors for the views) here, images\user-<name>\, and APE lists it too.

Icons: on the same Theme page, "Icons" is the color the editors' grey icons are redrawn in (Radiant's white toolbar
glyphs, APE's grey ones and the grey AssetWorks icons; colored icons keep their colors). "Icons from" picks the
editor's own icons or a folder of your own under icons\, and "Recolor in the Icons color" redraws the grey ones of
either (your folder is not changed; redrawn sets go to icons\_generated\, which Radiant Plus and APE Plus
manage themselves). "Icons Folder" opens icons\ and puts every original icon of
both editors into icons\_editor_defaults\ the first time: copy that folder under a new name, change what you like
(only changed files matter), then pick it under "Icons from".

View colors: Radiant's own view colors (grid, camera background, brushes...) have presets too, the <name>.colors
files here. Pick one in Settings > Preferences, Color page, "Preset:" (OK keeps it, Cancel drops it); "Save As..." writes
the colors you see as a new .colors file, and with "Follow theme" ticked picking a theme also picks the preset of the
same name. Format: one KEY=r, g, b[, a] per line, 0..1 (Radiant's COLOR_* names); keys left out stay as they are.

Format: a normal Qt 5 stylesheet (https://doc.qt.io/qt-5/stylesheet-reference.html). Two extras:

  /* base-style: fusion */     a comment near the top picks the widget style the sheet is drawn over:
                               fusion, plastique (Radiant's own), WindowsVista, Windows, cleanlooks, motif.
  url(images/dark/check.png)   relative paths are resolved against this folder.

Icons: a theme can also replace the editors' icons. Put PNGs in icons\<theme name>\ here (for example
icons\My Dark\ for My Dark.qss), in the same folders the editor reads them from, or name another folder in the
sheet with a comment near the top:

  /* icon-pack: icons/Shared */   relative to this folder; "none" turns the pack off for this theme.
  /* logo: images/mytheme/logo.png */     the editor's window and taskbar icon while the theme is applied (relative
                                        to this folder); without the line the editors keep their own logo.
  /* logo-ape: ... */  /* logo-launcher: ... */   the same for APE and the Mod Tools Launcher only (they use logo:
                                        when the theme has no line of their own).

Title bar: the editors' and the launcher's title bars take the theme's colours (Windows 11; Windows 10 only turns
them dark): the palette line's panel, text and border colours, or a line of their own near the top:

  /* titlebar: #2b2b2b #d9d9d9 #454545 */   caption, text (optional) and border (optional) colour; "none" keeps
                                        the Windows title bar. Treyarch and Classic have colours of their own (their menu
                                        bars'); any other theme without a palette line keeps the Windows title bar.

To start, run "RadiantPlus theme icons export" (or "APEPlus theme icons export" in APE) on the command channel: it
copies every icon the editor has into icons\_editor_defaults\ with the right folders, for example
toolbars\toolbars\Copy_Standard.png (Radiant's toolbar), AEMainWindow\Resources\filter.png (APE) and
AssetWorks\icon_asset_xmodel.png (APE's asset types). Copy the files you change into your theme's folder; only
those are replaced, the rest stay the editor's own. One folder can hold icons for both editors. The icons change
as soon as the theme is applied; "theme icons use <folder>" tries a folder without a theme.

  /* icon-recolor: #28292d #f5f5f5 */   makes the icon folder instead: the first time the theme is applied, Radiant's
                                        white icons are redrawn in the first colour, dark button faces in the second.
                                        Files already in the folder are kept; delete the folder to make it again.

A broken sheet shows as Qt's default look; RadiantPlugins.log ("theme:" lines) says what was applied. Command
channel: "RadiantPlus theme list" and "RadiantPlus theme <name>" (APEPlus the same in APE).
