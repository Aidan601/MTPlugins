#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>
#include "pluginmgr.h"
#include "plugins.h"
#include "dock.h"
#include "qt.h"
#include "mainthread.h"
#include "ui.h"
#include "radiant_addrs.h"
#include "config.h"
#include "log.h"
#include "app.h"
#include "browse.h"
#include "update.h"
#include "support.h"

#pragma comment(lib, "shell32.lib")

namespace
{
    bool  g_tried = false;
    void* g_dialog = nullptr;        // QDialog (the Plugins manager window), hidden between uses
    void* g_root = nullptr;
    void* g_btnClose = nullptr;
    void* g_chkShowAll = nullptr;
    bool  g_showAll = false;         // view filter only
    void* g_list = nullptr;
    void* g_detail = nullptr;
    void* g_btnReload = nullptr; void* g_btnRefresh = nullptr; void* g_btnFolder = nullptr; void* g_btnSettings = nullptr; void* g_btnWebsite = nullptr;
    void* g_btnUninstall = nullptr;
    void* g_filter = nullptr;
    void* g_search = nullptr;        // QLineEdit above the list, both views
    std::string g_searchShown;
    void* g_pendingClick = nullptr;
    unsigned g_shownVersion = 0;
    int   g_shownRow = -1;
    std::vector<std::string> g_rowNames;
    std::vector<bool> g_rowChecked;
    // Browse mode: the public plugin index (browse.cpp)
    void* g_radInstalled = nullptr; void* g_radBrowse = nullptr; void* g_btnInstall = nullptr;
    bool  g_browse = false;
    unsigned g_browseShown = 0;
    std::vector<std::string> g_browseNames;
    uint64_t g_lastPoll = 0;
    bool  g_rebuilding = false;
    bool  g_useFilter = false;
    void* g_chkUpdates = nullptr;    // "Ask about MTPlugins updates" ([update] check)
    bool  g_updatesShown = true;
    void* g_chkPluginUpdates = nullptr;   // "Ask about plugin updates" ([update] plugins)
    bool  g_pluginUpdatesShown = true;

    std::string Escape(const std::string& s)
    {
        std::string o;
        for (char c : s) { if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '&') o += "&amp;"; else if (c == '\n') o += "<br>"; else o += c; }
        return o;
    }
    // light blue: Qt's default link colour is unreadable on the dark window
    std::string Anchor(const std::string& url, const std::string& text) { return "<a style=\"color:#6cb4ff\" href=\"" + url + "\">" + text + "</a>"; }

    // http(s) URLs in already escaped text become links
    std::string Linkify(const std::string& html)
    {
        std::string o;
        size_t i = 0;
        while (i < html.size())
        {
            size_t a = (std::min)(html.find("https://", i), html.find("http://", i));
            if (a == std::string::npos) { o += html.substr(i); break; }
            size_t b = html.find_first_of(" \t\r\n<\"'", a);
            if (b == std::string::npos) b = html.size();
            while (b > a && strchr(".,;:!?)", html[b - 1])) --b;
            const std::string url = html.substr(a, b - a);
            o += html.substr(i, a - i) + Anchor(url, url);
            i = b;
        }
        return o;
    }
    std::string Link(const std::string& url) { return Linkify(Escape(url)); }

    // the list is shown A-Z by the name the user sees, load order is left alone
    bool NameLess(const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; }
    const std::string& Shown(const ModToolsPlugin* p) { return p->displayName.empty() ? p->name : p->displayName; }
    const std::string& Shown(const browse::Entry& e) { return e.displayName.empty() ? e.name : e.displayName; }

    std::string Narrow(const std::wstring& w)
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n ? n - 1 : 0, '\0');
        if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
        return s;
    }

    std::string Lower(std::string s)
    {
        for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        return s;
    }

    // every word of the search text appears somewhere in the fields, case ignored
    bool Matches(std::initializer_list<std::string> fields)
    {
        std::string hay;
        for (const std::string& f : fields) hay += Lower(f) + '\n';
        std::string q = Lower(g_searchShown);
        size_t i = 0;
        while (i < q.size())
        {
            size_t a = q.find_first_not_of(" \t", i);
            if (a == std::string::npos) break;
            size_t b = q.find_first_of(" \t", a);
            if (b == std::string::npos) b = q.size();
            if (hay.find(q.substr(a, b - a)) == std::string::npos) return false;
            i = b;
        }
        return true;
    }

    std::string SearchNote()
    {
        if (g_searchShown.find_first_not_of(" \t") == std::string::npos) return "";
        return " matching \"" + Escape(g_searchShown) + "\"";
    }

    ModToolsPlugin* Selected()
    {
        if (g_browse) return nullptr;
        int row = g_list ? qt::ListCurrentRow(g_list) : -1;
        if (row < 0 || row >= int(g_rowNames.size())) return nullptr;
        return plugins::Find(g_rowNames[size_t(row)]);
    }

    void UpdateDetail()
    {
        ModToolsPlugin* p = Selected();
        if (!p)
        {
            std::string note = browse::UninstallNote("");
            qt::SetLabelText(g_detail, ("<b>" + std::to_string(g_rowNames.size()) + " plugin(s) shown</b>" + SearchNote() + " in " + Escape(Narrow(plugins::Root())) + "<br>Use Show all plugins to manage other applications. Enabled states are shared." +
                                        (note.empty() ? "" : "<br><span style=\"color:#e0c060\">" + Escape(note) + "</span>")).c_str());
            qt::SetEnabled(g_btnReload, false); qt::SetEnabled(g_btnSettings, false); qt::SetEnabled(g_btnWebsite, false); qt::SetEnabled(g_btnUninstall, false);
            return;
        }
        std::string col = p->state == ModToolsPlugin::Loaded ? "#7fd67f" : (p->state == ModToolsPlugin::Failed || p->state == ModToolsPlugin::Incompatible) ? "#ff7070" : "#d0d0d0";
        if (p->state == ModToolsPlugin::OtherApp) col = "#a0a0a0";
        std::string t = "<b>" + Escape(p->displayName.empty() ? p->name : p->displayName) + "</b>" + (p->version.empty() ? "" : " " + Escape(p->version)) + (p->author.empty() ? "" : " by " + Escape(p->author));
        t += "<br><span style=\"color:" + col + "\">" + plugins::StateName(p->state) + (p->error.empty() ? "" : ": " + Escape(p->error)) + "</span>";
        if (!p->description.empty()) t += "<br>" + Link(p->description);
        t += "<br>Runs in: " + Escape(plugins::AppNames(p->apps)) + ". Shared: " + (p->enabled ? "enabled" : "disabled");
        if (!p->url.empty()) t += "<br>Website: " + Link(p->url);
        t += "<br><span style=\"color:#909090\">" + Escape(Narrow(p->dllPath)) + "</span>";
        std::string note = browse::UninstallNote(p->name);
        if (!note.empty()) t += "<br><span style=\"color:#e0c060\">" + Escape(note) + "</span>";
        qt::SetLabelText(g_detail, t.c_str());
        qt::SetEnabled(g_btnUninstall, !browse::Installing());
        qt::SetEnabled(g_btnReload, p->enabled && plugins::ForThisEditor(p));
        qt::SetEnabled(g_btnSettings, p->state == ModToolsPlugin::Loaded && p->api.OnSettings != nullptr);
        qt::SetEnabled(g_btnWebsite, !p->url.empty());
    }

    bool SelectedEntry(browse::Entry& out)
    {
        int row = g_list ? qt::ListCurrentRow(g_list) : -1;
        if (!g_browse || row < 0 || row >= int(g_browseNames.size())) return false;
        for (const browse::Entry& e : browse::Entries()) if (e.name == g_browseNames[size_t(row)]) { out = e; return true; }
        return false;
    }

    std::string BrowseMessageHtml()
    {
        std::string m = browse::Message();
        return m.empty() ? "" : "<br><span style=\"color:#e0c060\">" + Escape(m) + "</span>";
    }

    void UpdateDetailBrowse()
    {
        browse::Entry e;
        qt::SetEnabled(g_btnRefresh, !browse::Loading());
        if (!SelectedEntry(e))
        {
            std::string t = "<b>" + std::to_string(g_browseNames.size()) + " plugin(s) shown</b>" + SearchNote() + ": the public plugin list, and MTPlugins' own plugins "
                            "that were left out in Setup. Anyone can add theirs to the list: see "
                            + Anchor("https://github.com/Aidan601/MTPlugins-Index", "github.com/Aidan601/MTPlugins-Index") + "." + BrowseMessageHtml();
            qt::SetLabelText(g_detail, t.c_str());
            qt::SetEnabled(g_btnInstall, false); qt::SetEnabled(g_btnWebsite, false);
            qt::SetButtonText(g_btnInstall, "Install");
            return;
        }
        browse::Status st = browse::StatusOf(e);
        std::string col = st == browse::Status::Installed ? "#7fd67f" : st == browse::Status::UpdateAvailable ? "#e0c060" : st == browse::Status::Incompatible ? "#ff7070" : "#d0d0d0";
        std::string t = "<b>" + Escape(e.displayName) + "</b>" + (e.version.empty() ? "" : " " + Escape(e.version)) + (e.author.empty() ? "" : " by " + Escape(e.author));
        t += "<br><span style=\"color:" + col + "\">" + browse::StatusName(st) + "</span>";
        std::string mine = plugins::InstalledVersion(e.name);
        if (st == browse::Status::UpdateAvailable && !mine.empty()) t += " (you have " + Escape(mine) + ")";
        if (st == browse::Status::Modified) t += "<br>Your copy" + (mine.empty() ? std::string() : " (" + Escape(mine) + ")") + " differs from the list but is not older, so it is not offered as an update. Reinstall replaces it with the list's version.";
        if (!e.changes.empty() && st != browse::Status::Installed) t += "<br>Changes: " + Link(e.changes);
        if (!e.description.empty()) t += "<br>" + Link(e.description);
        t += "<br>Runs in: " + Escape(plugins::AppNames(e.apps)) + (e.bundled ? "." : ". Files: " + std::to_string(e.files.size()));
        if (!e.url.empty()) t += "<br>Website: " + Link(e.url);
        if (e.bundled) t += "<br><span style=\"color:#909090\">Comes with MTPlugins: Install has MTPlugins Setup add it, nothing is downloaded.</span>";
        else t += "<br><span style=\"color:#909090\">Plugins run inside the editor with full access to your PC. Only install plugins from authors you trust.</span>";
        t += BrowseMessageHtml();
        qt::SetLabelText(g_detail, t.c_str());
        qt::SetButtonText(g_btnInstall, st == browse::Status::UpdateAvailable ? "Update" : st == browse::Status::Installed || st == browse::Status::Modified ? "Reinstall" : "Install");
        qt::SetEnabled(g_btnInstall, st != browse::Status::Incompatible && !browse::Installing());
        qt::SetEnabled(g_btnWebsite, !e.url.empty());
    }

    void RebuildBrowse()
    {
        g_rebuilding = true;
        browse::Entry sel;
        std::string keep = SelectedEntry(sel) ? sel.name : "";
        qt::ListClear(g_list);
        g_browseNames.clear();
        int row = 0, keepRow = -1;
        std::vector<browse::Entry> entries = browse::Entries();
        // MTPlugins' own entries stay ahead of the community list
        std::stable_sort(entries.begin(), entries.end(), [](const browse::Entry& a, const browse::Entry& b) {
            bool ao = a.bundled || a.official, bo = b.bundled || b.official;
            return ao != bo ? ao : NameLess(Shown(a), Shown(b));
        });
        for (const browse::Entry& e : entries)
        {
            if (!g_showAll && !(e.apps & uint32_t(app::ApiId()))) continue;
            if (!Matches({ e.name, e.displayName, e.author, e.description, plugins::AppNames(e.apps), browse::StatusName(browse::StatusOf(e)) })) continue;
            std::string label = e.displayName + (e.version.empty() ? "" : "  " + e.version) + (e.author.empty() ? "" : "  by " + e.author) +
                                "    [" + plugins::AppNames(e.apps) + "]    " + browse::StatusName(browse::StatusOf(e)) + (e.bundled || e.official ? " (comes with MTPlugins)" : "");
            qt::ListAdd(g_list, label.c_str());
            if (!e.description.empty()) qt::ListSetItemToolTip(g_list, row, e.description.c_str());
            g_browseNames.push_back(e.name);
            if (e.name == keep) keepRow = row;
            ++row;
        }
        qt::ListSetCurrentRow(g_list, keepRow);
        g_browseShown = browse::Version();
        g_shownVersion = plugins::Version();
        g_shownRow = qt::ListCurrentRow(g_list);
        g_rebuilding = false;
        UpdateDetailBrowse();
    }

    void ApplyMode()
    {
        for (void* b : { g_btnReload, g_btnFolder, g_btnSettings, g_btnUninstall }) qt::SetVisible(b, !g_browse);
        qt::SetVisible(g_btnInstall, g_browse);
        qt::SetEnabled(g_btnRefresh, true);
        qt::SetToolTip(g_btnRefresh, g_browse ? "Download the plugin list again" : "Look for new plugins in the plugins folder");
        qt::SetToolTip(g_chkShowAll, g_browse ? "Include plugins for the other editors and the launcher" : "Include plugins for Launcher, APE and Radiant. This changes only the list, not enabled states.");
    }

    void Rebuild()
    {
        if (g_browse) { RebuildBrowse(); return; }
        g_rebuilding = true;
        ModToolsPlugin* selected = Selected();
        std::string keep = selected ? selected->name : "";
        qt::ListClear(g_list);
        g_rowNames.clear(); g_rowChecked.clear();
        int row = 0;
        int keepRow = -1;
        std::vector<ModToolsPlugin*> all = plugins::All();
        std::stable_sort(all.begin(), all.end(), [](const ModToolsPlugin* a, const ModToolsPlugin* b) { return NameLess(Shown(a), Shown(b)); });
        for (ModToolsPlugin* p : all)
        {
            // keep unknown/failed probes visible, hide known foreign apps
            if (!g_showAll && !plugins::ForThisEditor(p)) continue;
            if (!Matches({ p->name, p->displayName, p->author, p->description, plugins::AppNames(p->apps), plugins::StateName(p->state) })) continue;
            std::string label = Shown(p) + (p->version.empty() ? "" : "  " + p->version) + "    [" + plugins::AppNames(p->apps) + "]";
            // the checkbox already says Loaded/Disabled
            if (p->state != ModToolsPlugin::Loaded && p->state != ModToolsPlugin::Disabled) label += std::string("    ") + plugins::StateName(p->state);
            qt::ListAdd(g_list, label.c_str());
            qt::ListSetItemCheckable(g_list, row, true);
            qt::ListSetItemChecked(g_list, row, p->enabled);
            if (!p->error.empty()) qt::ListSetItemToolTip(g_list, row, p->error.c_str());
            g_rowNames.push_back(p->name); g_rowChecked.push_back(p->enabled);
            if (p->name == keep) keepRow = row;
            ++row;
        }
        qt::ListSetCurrentRow(g_list, keepRow);
        g_shownVersion = plugins::Version();
        g_shownRow = qt::ListCurrentRow(g_list);
        g_rebuilding = false;
        UpdateDetail();
    }

    void SaveGeometry()
    {
        if (!g_dialog) return;
        int x, y, w, h; qt::Geometry(g_dialog, x, y, w, h);
        if (w < 100 || h < 100) return;
        wchar_t buf[128]; _snwprintf_s(buf, _TRUNCATE, L"%d %d %d %d", x, y, w, h);
        WritePrivateProfileStringW(L"window", L"plugins_dialog", buf, Cfg().iniPath.c_str());
    }
    bool LoadGeometry(int& x, int& y, int& w, int& h)
    {
        wchar_t buf[128] = {};
        GetPrivateProfileStringW(L"window", L"plugins_dialog", L"", buf, 128, Cfg().iniPath.c_str());
        if (swscanf_s(buf, L"%d %d %d %d", &x, &y, &w, &h) != 4 || w < 100 || h < 100) return false;
        // keep it on a monitor that exists
        RECT r{ x, y, x + w, y + h };
        return MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != nullptr;
    }
    void OnClose(void*) { pluginmgr::Toggle(); }
    void OnReload(void*) { if (ModToolsPlugin* p = Selected()) { plugins::Reload(p); Rebuild(); } }

    // Browse always shows the newest plugin list: downloaded again on every switch to it and every open in it
    void RefreshList()
    {
        if (!browse::Loading() && !browse::Installing()) browse::Refresh();
    }

    void OnRefresh(void*)
    {
        if (g_browse) browse::Refresh(); else plugins::Rescan();
        Rebuild();
    }
    void OnInstall(void*)
    {
        browse::Entry e;
        if (!SelectedEntry(e) || browse::Installing()) return;
        const browse::File* dll = nullptr;
        for (const browse::File& f : e.files) if (_stricmp(f.path.c_str(), (e.name + ".dll").c_str()) == 0) dll = &f;
        std::string from = dll ? dll->url : "";
        size_t cut = from.find('/', from.find("://") == std::string::npos ? 0 : from.find("://") + 3);
        if (cut != std::string::npos) from = from.substr(0, cut);
        std::string q = "Install " + e.displayName + (e.version.empty() ? "" : " " + e.version) + (e.author.empty() ? "" : " by " + e.author) + "?\n\n"
                        "Downloaded from: " + from + " (" + std::to_string(e.files.size()) + " file(s), each checked against the plugin list's SHA-256)\n\n"
                        "Plugins run inside the editor with full access to your PC. Only install plugins from authors you trust.";
        if (e.official)
            q = "Update " + e.displayName + " to " + e.version + "?\n\nIt comes with MTPlugins. Downloaded from: " + from + " (" + std::to_string(e.files.size()) +
                " file(s), each checked against its SHA-256). Your settings are kept.";
        // one of ours that Setup left out: the kept Setup adds it (browse::StartSetup)
        if (e.bundled)
            q = "Install " + e.displayName + "?\n\nIt comes with MTPlugins. MTPlugins Setup adds it: Windows may ask for permission, and the editors can stay open." +
                (e.note.empty() ? "" : "\n\n" + e.note);
        int wlen = MultiByteToWideChar(CP_UTF8, 0, q.c_str(), -1, nullptr, 0);
        std::wstring wq(wlen ? wlen - 1 : 0, L'\0');
        if (wlen) MultiByteToWideChar(CP_UTF8, 0, q.c_str(), -1, &wq[0], wlen);
        HWND owner = reinterpret_cast<HWND>(qt::WinId(g_dialog));
        if (MessageBoxW(owner, wq.c_str(), e.official ? L"Update plugin" : L"Install plugin", e.bundled || e.official ? MB_OKCANCEL | MB_ICONQUESTION : MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return;
        browse::Install(e.name);
        Rebuild();
    }
    void OnUninstall(void*)
    {
        ModToolsPlugin* p = Selected();
        if (!p) return;
        std::string name = p->displayName.empty() ? p->name : p->displayName;
        bool loose = _wcsicmp(p->dir.c_str(), plugins::Root().c_str()) == 0;
        std::string q = "Uninstall " + name + (p->version.empty() ? "" : " " + p->version) + "?\n\n" +
                        (loose ? "Its file " + Narrow(p->dllPath) + " and its settings are deleted."
                               : "Its folder " + Narrow(p->dir) + " is deleted, settings included.") +
                        "\n\nRadiant, APE or the launcher may still have it loaded: they keep it until they restart.";
        int wlen = MultiByteToWideChar(CP_UTF8, 0, q.c_str(), -1, nullptr, 0);
        std::wstring wq(wlen ? wlen - 1 : 0, L'\0');
        if (wlen) MultiByteToWideChar(CP_UTF8, 0, q.c_str(), -1, &wq[0], wlen);
        HWND owner = reinterpret_cast<HWND>(qt::WinId(g_dialog));
        if (MessageBoxW(owner, wq.c_str(), L"Uninstall plugin", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return;
        browse::Uninstall(p->name);
        Rebuild();
    }
    void OnFolder(void*)
    {
        ModToolsPlugin* p = Selected();
        std::wstring dir = p ? p->dir : plugins::Root();
        CreateDirectoryW(plugins::Root().c_str(), nullptr);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    void OnSettings(void*)
    {
        ModToolsPlugin* p = Selected();
        if (p && p->state == ModToolsPlugin::Loaded && p->api.OnSettings) plugins::Call(p, "OnSettings", [](void* a) { reinterpret_cast<void (*)(void)>(a)(); }, reinterpret_cast<void*>(p->api.OnSettings));
    }
    void OnWebsite(void*)
    {
        std::string url;
        browse::Entry e;
        if (g_browse) { if (SelectedEntry(e)) url = e.url; }       // browse.cpp keeps only plain https addresses
        else if (ModToolsPlugin* p = Selected()) url = p->url;     // SafeUrl keeps it to a plain http(s) address
        if (url.empty()) return;
        std::wstring u(url.begin(), url.end());
        ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    void SlotThunk(void* user, void**) { reinterpret_cast<void (*)(void*)>(user)(nullptr); }

    bool OnEvent(void*, void* w, int type)
    {
        if (type == 3 /* MouseButtonRelease */ && (w == g_btnReload || w == g_btnRefresh || w == g_btnFolder || w == g_btnSettings || w == g_btnWebsite || w == g_btnInstall || w == g_btnClose) && qt::IsDown(w) && qt::CursorInside(w))
            g_pendingClick = w;
        if (type == 18 /* Hide */ && w == g_dialog) SaveGeometry();     // closed with the title bar, Esc or Toggle
        return false;
    }

    void Wire(void* button, void (*fn)(void*))
    {
        if (!g_useFilter && qt::Connect(button, qt::Signal::ButtonClicked, SlotThunk, reinterpret_cast<void*>(fn))) return;
        if (!g_useFilter) { g_useFilter = true; LOG("pluginmgr: signal connection unavailable, using the event filter for clicks"); }
        if (!g_filter) g_filter = qt::MakeEventFilter(OnEvent, nullptr);
        qt::InstallFilter(button, g_filter);
    }

    void Build()
    {
        void* mw = dock::MainWindowWidget();
        g_dialog = qt::Dialog((std::string("Plugins - ") + app::Name()).c_str(), mw);
        void* outer = qt::VBox(g_dialog);
        qt::LayoutMargins(outer, 0, 0, 0, 0);
        g_root = qt::Widget(g_dialog);
        qt::BoxAddWidget(outer, g_root, 1);
        void* main = qt::VBox(g_root);
        qt::LayoutMargins(main, 8, 8, 8, 8);
        qt::LayoutSpacing(main, 6);

        void* top = qt::HBox();
        g_radInstalled = qt::RadioButton("Installed", g_root);
        g_radBrowse = qt::RadioButton("Browse plugins", g_root);
        qt::SetToolTip(g_radBrowse, "Every plugin in the public plugin list, including other people's; install or update with one click");
        qt::SetChecked(g_radInstalled, true);
        g_chkShowAll = qt::CheckBox("Show all plugins", g_root);
        qt::SetObjectName(g_chkShowAll, "PluginsShowAll");
        qt::SetChecked(g_chkShowAll, false);
        qt::BoxAddWidget(top, g_radInstalled);
        qt::BoxAddWidget(top, g_radBrowse);
        g_search = qt::LineEdit("", g_root);
        qt::SetObjectName(g_search, "PluginsSearch");
        qt::SetPlaceholder(g_search, "Search plugins");
        qt::SetToolTip(g_search, "Show only plugins whose name, author, description, editors or state contain every word typed here");
        qt::BoxAddWidget(top, g_search, 1);
        qt::BoxAddWidget(top, g_chkShowAll);
        qt::BoxAddLayout(main, top);

        g_list = qt::ListWidget(g_root);
        qt::SetObjectName(g_list, "PluginsList");
        qt::SetMinimumHeight(g_list, 80);
        qt::BoxAddWidget(main, g_list, 1);

        g_detail = qt::Label("", g_root);
        qt::SetWordWrap(g_detail, true);
        qt::BoxAddWidget(main, g_detail);

        g_chkUpdates = qt::CheckBox("Ask about MTPlugins updates", g_root);
        qt::SetObjectName(g_chkUpdates, "PluginsAskUpdates");
        qt::SetToolTip(g_chkUpdates, "When the Mod Tools Launcher starts, look for a newer MTPlugins release on GitHub and offer to install it. "
                                     "Shared by the launcher and both editors.");
        g_updatesShown = update::Enabled();
        qt::SetChecked(g_chkUpdates, g_updatesShown);
        g_chkPluginUpdates = qt::CheckBox("Ask about plugin updates", g_root);
        qt::SetObjectName(g_chkPluginUpdates, "PluginsAskPluginUpdates");
        qt::SetToolTip(g_chkPluginUpdates, "When the launcher or an editor starts, offer to update the plugins you installed from Browse plugins "
                                           "when the plugin list has a newer version. Shared by the launcher and both editors.");
        g_pluginUpdatesShown = update::PluginsEnabled();
        qt::SetChecked(g_chkPluginUpdates, g_pluginUpdatesShown);
        void* asks = qt::HBox();
        qt::BoxAddWidget(asks, g_chkUpdates);
        qt::BoxAddWidget(asks, g_chkPluginUpdates);
        qt::BoxAddStretch(asks);
        qt::BoxAddLayout(main, asks);

        void* buttons = qt::HBox();
        g_btnReload = qt::PushButton("Reload", g_root);
        qt::SetToolTip(g_btnReload, "Unload and load the selected plugin again (after replacing its DLL)");
        g_btnRefresh = qt::PushButton("Refresh", g_root);
        qt::SetToolTip(g_btnRefresh, "Look for new plugins in the plugins folder");
        g_btnFolder = qt::PushButton("Open folder", g_root);
        g_btnSettings = qt::PushButton("Settings", g_root);
        qt::SetToolTip(g_btnSettings, "Open the selected plugin's own window");
        g_btnUninstall = qt::PushButton("Uninstall", g_root);
        qt::SetToolTip(g_btnUninstall, "Unload the selected plugin and delete its folder, settings included");
        g_btnInstall = qt::PushButton("Install", g_root);
        qt::SetToolTip(g_btnInstall, "Download the selected plugin, check every file against the plugin list, and load it");
        g_btnWebsite = qt::PushButton("Website", g_root);
        qt::SetToolTip(g_btnWebsite, "Open the selected plugin's project page (its GitHub page) in the browser");
        g_btnClose = qt::PushButton("Close", g_root);
        for (void* b : { g_btnInstall, g_btnReload, g_btnRefresh, g_btnFolder, g_btnSettings, g_btnUninstall, g_btnWebsite }) qt::BoxAddWidget(buttons, b);
        qt::BoxAddStretch(buttons);
        qt::BoxAddWidget(buttons, g_btnClose);
        qt::BoxAddLayout(main, buttons);

        Wire(g_btnReload, OnReload); Wire(g_btnRefresh, OnRefresh); Wire(g_btnFolder, OnFolder); Wire(g_btnSettings, OnSettings); Wire(g_btnWebsite, OnWebsite); Wire(g_btnInstall, OnInstall); Wire(g_btnUninstall, OnUninstall); Wire(g_btnClose, OnClose);
        ApplyMode();
        if (!g_filter) g_filter = qt::MakeEventFilter(OnEvent, nullptr);
        qt::InstallFilter(g_dialog, g_filter);      // remembers the geometry on close

        int x, y, w, h;
        if (LoadGeometry(x, y, w, h)) { qt::Resize(g_dialog, w, h); qt::Move(g_dialog, x, y); }
        else
        {
            w = 520; h = 360;
            RECT r{}; HWND main = mt::MainWindow();
            if (main && GetWindowRect(main, &r)) qt::Move(g_dialog, (r.left + r.right - w) / 2, (r.top + r.bottom - h) / 2);
            qt::Resize(g_dialog, w, h);
        }
    }

    void HotkeyToggle(void*) { pluginmgr::Toggle(); }
    void MenuShow(void*) { pluginmgr::Show(); }
    void MenuFolder(void*) { OnFolder(nullptr); }
    void MenuWhatsNew(void*) { support::ShowWhatsNew(); }
    void MenuCheckUpdates(void*) { update::CheckNow(); }
    void MenuReport(void*) { support::Report(true); }
    void MenuAbout(void*) { support::ShowAbout(); }
    void MenuThunk(void* user, void**) { reinterpret_cast<void (*)(void*)>(user)(nullptr); }

    void BuildPluginsMenu()
    {
        void* mw = dock::MainWindowWidget();
        // Launcher: the "Plugins" menu below only, no toolbar buttons
        void* bar = mw ? qt::MenuBar(mw) : nullptr;
        if (!bar) return;
        void* menu = qt::AddMenu(bar, "Plugins");
        if (!menu) return;
        void* a0 = qt::Action("Plugins... (Ctrl+Shift+P)", menu); qt::AddAction(menu, a0); qt::Connect(a0, qt::Signal::ActionTriggered, MenuThunk, reinterpret_cast<void*>(MenuShow));
        void* a1 = qt::Action("Open plugins folder", menu); qt::AddAction(menu, a1); qt::Connect(a1, qt::Signal::ActionTriggered, MenuThunk, reinterpret_cast<void*>(MenuFolder));
        qt::MenuAddSeparator(menu);
        void* a3 = qt::Action("What's new...", menu); qt::AddAction(menu, a3); qt::Connect(a3, qt::Signal::ActionTriggered, MenuThunk, reinterpret_cast<void*>(MenuWhatsNew));
        void* a6 = qt::Action("Check for updates...", menu); qt::AddAction(menu, a6); qt::Connect(a6, qt::Signal::ActionTriggered, MenuThunk, reinterpret_cast<void*>(MenuCheckUpdates));
        void* a4 = qt::Action("Report a problem...", menu); qt::AddAction(menu, a4); qt::Connect(a4, qt::Signal::ActionTriggered, MenuThunk, reinterpret_cast<void*>(MenuReport));
        qt::MenuAddSeparator(menu);
        void* a5 = qt::Action("About MTPlugins...", menu); qt::AddAction(menu, a5); qt::Connect(a5, qt::Signal::ActionTriggered, MenuThunk, reinterpret_cast<void*>(MenuAbout));
    }
}

namespace pluginmgr
{
    void Tick()
    {
        if (!g_tried)
        {
            if (!dock::Ready() || !support::StartDecided()) return;
            g_tried = true;
            if (qt::Bind())
            {
                Build();
                BuildPluginsMenu();
                LOG("pluginmgr: Plugins dialog created (0x%p)", g_dialog);
            }
            else LOG("pluginmgr: Qt unavailable; no Plugins window (plugins still load)");
            ui::RegisterHotkey(nullptr, 'P', MODTOOLS_MOD_CTRL | MODTOOLS_MOD_SHIFT, HotkeyToggle, nullptr);
            // plugins are loaded once the main window exists, so their panes join the layout in a stable order
            plugins::Init();
            if (g_dialog) Rebuild();
            update::StartupCheck();      // plugin updates in every app, the release in the launcher
            return;
        }
        update::Tick();              // update question and installer start, window or not
        if (!g_dialog) return;
        if (g_pendingClick)
        {
            void* w = g_pendingClick; g_pendingClick = nullptr;
            if (w == g_btnReload) OnReload(nullptr); else if (w == g_btnRefresh) OnRefresh(nullptr); else if (w == g_btnFolder) OnFolder(nullptr); else if (w == g_btnSettings) OnSettings(nullptr); else if (w == g_btnWebsite) OnWebsite(nullptr); else if (w == g_btnInstall) OnInstall(nullptr); else if (w == g_btnClose) OnClose(nullptr);
        }
        browse::Tick();              // moves a verified download into place, window or not
        if (!Visible()) return;      // nothing to keep current while hidden
        uint64_t now = GetTickCount64();
        if (now - g_lastPoll < 200) return;
        g_lastPoll = now;
        bool browseMode = qt::IsChecked(g_radBrowse);
        std::string search = qt::EditText(g_search);
        if (search != g_searchShown) { g_searchShown = search; Rebuild(); return; }      // the 200 ms poll debounces typing
        bool askUpdates = qt::IsChecked(g_chkUpdates);
        if (askUpdates != g_updatesShown) { g_updatesShown = askUpdates; update::SetEnabled(askUpdates); }
        bool askPlugins = qt::IsChecked(g_chkPluginUpdates);
        if (askPlugins != g_pluginUpdatesShown) { g_pluginUpdatesShown = askPlugins; update::SetPluginsEnabled(askPlugins); }
        if (browseMode != g_browse)
        {
            g_browse = browseMode;
            ApplyMode();
            if (g_browse) RefreshList(); else plugins::Rescan();
            Rebuild();
            return;
        }
        if (g_browse)
        {
            bool showAll = qt::IsChecked(g_chkShowAll);
            if (showAll != g_showAll || browse::Version() != g_browseShown || plugins::Version() != g_shownVersion) { g_showAll = showAll; Rebuild(); return; }
            int row = qt::ListCurrentRow(g_list);
            if (row != g_shownRow) { g_shownRow = row; UpdateDetailBrowse(); }
            return;
        }
        // check boxes toggled by the user
        for (size_t i = 0; i < g_rowNames.size(); ++i)
        {
            bool checked = qt::ListItemChecked(g_list, int(i));
            if (checked != g_rowChecked[i])
            {
                g_rowChecked[i] = checked;
                if (ModToolsPlugin* p = plugins::Find(g_rowNames[i])) { plugins::SetEnabled(p, checked); Rebuild(); return; }
            }
        }
        bool showAll = qt::IsChecked(g_chkShowAll);
        if (showAll != g_showAll || plugins::Version() != g_shownVersion)
        {
            g_showAll = showAll;
            Rebuild();
            return;
        }
        int row = qt::ListCurrentRow(g_list);
        if (row != g_shownRow) { g_shownRow = row; UpdateDetail(); }
    }

    bool Visible() { return g_dialog && qt::IsShown(g_dialog); }

    void Show()
    {
        if (!g_dialog) return;
        g_updatesShown = update::Enabled();       // another editor may have changed it
        qt::SetChecked(g_chkUpdates, g_updatesShown);
        g_pluginUpdatesShown = update::PluginsEnabled();
        qt::SetChecked(g_chkPluginUpdates, g_pluginUpdatesShown);
        plugins::Rescan();                        // new plugin folders show (and load) on open
        if (g_browse) RefreshList();
        Rebuild();
        qt::SetVisible(g_dialog, true);
        qt::Raise(g_dialog);
        qt::ActivateWindow(g_dialog);
    }

    void ShowMode(bool browseMode)
    {
        if (!g_dialog) return;
        qt::SetChecked(browseMode ? g_radBrowse : g_radInstalled, true);   // Tick sees the change and switches
        if (!Visible()) Show();
    }

    int Search(const std::string& text)
    {
        if (!g_dialog) return -1;
        qt::SetEditText(g_search, text.c_str());
        g_searchShown = text;
        if (!Visible()) Show(); else Rebuild();
        return int(g_browse ? g_browseNames.size() : g_rowNames.size());
    }

    void Toggle()
    {
        if (!g_dialog) return;
        if (Visible()) { SaveGeometry(); qt::SetVisible(g_dialog, false); return; }
        Show();
    }

    void Shutdown()
    {
        if (g_filter) { for (void* b : { g_btnReload, g_btnRefresh, g_btnFolder, g_btnSettings, g_btnWebsite, g_btnInstall, g_btnClose, g_dialog }) if (b) qt::RemoveFilter(b, g_filter); }
        if (g_dialog) qt::SetVisible(g_dialog, false);
        g_root = nullptr; g_dialog = nullptr;
    }
}
