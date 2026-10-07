#pragma once
// Theme icon packs, shared by Radiant Plus and APE Plus: a theme can replace the editor's icons with PNGs from a folder that mirrors where the editor reads them
// (<pack>\toolbars\toolbars\Copy_Standard.png for ":/toolbars/toolbars/Copy_Standard.png", <pack>\AssetWorks\icon_asset_xmodel.png for <root>\AssetWorks\... in APE).
// A theme's pack is <bin\themes>\icons\<theme name>, or the folder a '/* icon-pack: <folder> */' comment in the first 2 KB of the sheet names (relative to the sheet's folder, "none" = no pack).
#include <string>
#include <string.h>

namespace themeicons
{
// Binds Qt and registers the redirect (idle until Use names a pack). root = the game folder. Main thread, OnLoad.
bool Setup(const std::wstring& root);
// Whether a sheet's 'icon-recolor' line counts in this program (default on). APE Plus and the launcher turn it off,
// a theme's 'icon-pack' folder still applies there.
void SetRecolor(bool on);
// The pack folder for a theme, "" when it has none.
std::wstring PackFor(const std::wstring& themesDir, const std::string& themeName, const std::string& sheet, const std::wstring& sheetDir);
// The logo file a '/* logo-<app>: <file> */' line in the sheet names for this program (app "ape", "launcher", Radiant
// passes none), else its '/* logo: <file> */' line (relative to the sheet's folder), "" for the program's own logo.
std::wstring LogoFor(const std::string& sheet, const std::wstring& sheetDir, const char* app = nullptr);
// Switches to pack ("" = the editor's own icons) and logo ("" = the editor's own), refreshes the icons already on screen
// and, when the logo changed, sets every window's icon again so the title and task bars follow. Main thread.
void Use(const std::wstring& pack, const std::wstring& logo = L"");
// The native title bar of every window of this program in the theme's colours: the sheet's '/* titlebar: #caption [#text [#border]] */'
// line, else its palette line (panel, text, border), else built-in colours for "Classic Treyarch" and "classic", "titlebar: none" or
// anything else = Windows' own. Windows 11 paints the colours, Windows 10 only turns dark for a dark caption. Later windows get it too. Main thread.
void UseTitleBar(const std::string& sheet, const std::string& themeName = "");
// Puts the editor's icons back and removes the redirect. OnUnload. Also puts the title bars back.
void Shutdown();
// The editors' own stylesheet look (radiant\stylesheet.qss) is the built-in "Classic Treyarch", "Treyarch" is the
// generated theme in its colours. An ini 'treyarch' or 'default' means the generated one.
constexpr const char* kClassicTreyarch = "Classic Treyarch";
inline bool IsClassicTreyarch(const std::string& name) { return _stricmp(name.c_str(), kClassicTreyarch) == 0; }
// First run (no [theme] name key yet): the generated Treyarch where the program showed its Treyarch look, else Light. Saved, so once.
inline const char* FirstRunTheme(bool treyarchLook) { return treyarchLook ? "Treyarch" : "Light"; }
constexpr const char* kUnset = "\x01unset";   // ConfigGet default that tells "no key" from "name=" (the program's own look)
// Theme list order: Light, Dark, Treyarch, Classic, Classic Treyarch first, the rest by name.
inline int ListRank(const std::string& name)
{
    static const char* const pinned[] = { "Light", "Dark", "Treyarch", "Classic", kClassicTreyarch };
    for (int i = 0; i < 5; ++i) if (_stricmp(name.c_str(), pinned[i]) == 0) return i;
    return 5;
}
inline bool ListBefore(const std::string& a, const std::string& b)
{
    int ra = ListRank(a), rb = ListRank(b);
    return ra != rb ? ra < rb : _stricmp(a.c_str(), b.c_str()) < 0;
}
// Shipped themes renamed or dropped: the theme that took an old name's place (the name itself otherwise).
std::string Renamed(const std::string& name);
// Removes those old shipped themes from dir (bin\themes or a plugin's own themes folder): the .qss and .colors only while
// they are as shipped (their generated header, a theme the user saved under that name stays), their images and recoloured icons.
void RetireShipped(const std::wstring& dir);
// "theme icons [status|refresh|use <folder>|none|export [dir]|trace on|off|show]", themesDir = <root>\bin\themes.
std::string Command(const std::string& args, const std::wstring& themesDir);
}
