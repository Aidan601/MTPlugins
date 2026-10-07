#pragma once
// The Theme editor shared by Radiant Plus (Preferences > Color, "Theme" tab) and APE Plus (View > Preferences, "Customize..."): eight core colors (and the icons) make a theme, the rest are mixed from them.
// The sheet is <themes>\template\theme.qss.in (tools/make_themes.py) with the palette filled in. Save As writes <themes>\<name>.qss (plus <name>.colors when the host asks). Each plugin compiles this file and supplies the Host hooks.
#include <functional>
#include <string>
#include <vector>

namespace themeedit
{
struct ThemeRef { std::string name, label; std::wstring file; };   // a listed theme (built-ins have no file)
struct Host
{
    std::wstring themesDir;                                         // <root>\bin\themes
    std::string product;                                            // "Radiant Plus" / "APE Plus", in the saved sheet's note
    std::string where;                                              // where the editor is, in the saved sheet's note
    bool viewColors = false;                                        // offer "Also save view colors" (Radiant)
    bool iconRecolor = true;                                        // the Icons color and "Recolor" box (off in APE: its icons keep their colours)
    int columns = 2;                                                // swatch columns: 2 on a wide page, 1 in a narrow group (APE)
    std::function<std::vector<ThemeRef>()> themes;                  // every listed theme, the editor keeps those with a palette
    std::function<std::string()> current;                           // the theme in effect (what the swatches show first)
    std::function<void(const std::string& sheet)> preview;          // apply this edited sheet now (base-style comment inside) and
                                                                    // show "Edited (not saved)" in the Theme combo
    std::function<void(const std::string& lines)> previewIcons;     // only the icons changed: apply this icon-pack/icon-recolor
                                                                    // comment as the icon pack, keep the sheet, mark it edited
    std::function<void(const std::string& name)> saved;             // a new theme file exists: rescan and select it
};
// Builds the editor under `parent` (a page or group) for the Preferences dialog `dialog`, returns the editor widget, or
// null with Error() set. The swatches show the theme in effect (or the unsaved edit), every change is applied at once
// through Host::preview and stays until a theme is applied again (Save As keeps it). One editor at a time.
void* Build(void* parent, void* dialog, const Host& host);
void Reset();                                                       // the dialog is gone
void ShowTheme(const std::string& name);                            // the Theme combo now names this theme: show its colours
bool Edited();                                                      // an unsaved edit is the look in effect
void ClearEdited();                                                 // a theme was applied
std::string Error();
// tests on an open dialog: "show <theme>", "set <0-8> <#rrggbb>", "from <index>", "recolor 0|1", "saveas", "" = state
std::string Command(const std::string& args);
}
