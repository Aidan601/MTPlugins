#include <windows.h>
#include <commdlg.h>
#include <string>
#include <cstdio>
#include <vector>
#include "pane.h"
#include "compat.h"
#include "presence.h"
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#include "mapsync.h"
#include "savesync.h"
#include "lightsync.h"
#include "assets.h"
#include "net.h"
#include "zonesync.h"

#pragma comment(lib, "comdlg32.lib")

namespace
{
    void* g_root = nullptr;
    void* g_radioHost = nullptr; void* g_radioJoin = nullptr;
    void* g_addr = nullptr; void* g_port = nullptr; void* g_name = nullptr;
    void* g_hideIp = nullptr; bool g_lastHideIp = true;
    void* g_syncSaves = nullptr; bool g_lastSyncSaves = false;
    void* g_syncLights = nullptr; bool g_lastSyncLights = false;
    void* g_btnStart = nullptr; void* g_status = nullptr;
    void* g_peers = nullptr; void* g_hint = nullptr; void* g_chatLog = nullptr; void* g_chatEdit = nullptr; void* g_btnSend = nullptr;
    void* g_filter = nullptr;
    // player colour: one label (dot + hex code) that opens Radiant's colour picker
    void* g_colorButton = nullptr;
    uint32_t g_colorShown = 0;           // colour the label was last drawn for (0 = never)
    bool  g_inColorDialog = false;
    void* g_pendingClick = nullptr;
    bool  g_pendingSend = false;
    bool  g_lastJoin = false;
    uint64_t g_lastRefresh = 0;
    std::vector<std::string> g_rows;
    bool g_peersEmptyShown = false;
    std::string g_chatShown, g_statusText, g_buttonText;

    std::wstring Widen(const std::string& s)
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n ? n - 1 : 0, L'\0');
        if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
        return w;
    }
    std::string Narrow(const std::wstring& w)
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n ? n - 1 : 0, '\0');
        if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
        return s;
    }
    std::string Trim(std::string s)
    {
        while (!s.empty() && isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        size_t a = 0; while (a < s.size() && isspace(static_cast<unsigned char>(s[a]))) ++a;
        return s.substr(a);
    }
    std::string Escape(const std::string& s)
    {
        std::string o;
        for (char c : s) { if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '&') o += "&amp;"; else o += c; }
        return o;
    }
    enum class Tone { Plain, Good, Warn };
    // status shows only messages from the pane's own buttons (bad port, missing address) for 15 s
    uint64_t g_noticeTick = 0;
    void SetStatus(const std::string& text, Tone tone, bool notice = true)
    {
        g_statusText = text;                      // `read` / `start` on the command channel still print the session line
        if (!notice) return;
        const char* col = tone == Tone::Good ? "#7fd67f" : tone == Tone::Warn ? "#e6c878" : "#d0d0d0";
        qt::SetLabelText(g_status, ("<span style=\"color:" + std::string(col) + "\">" + Escape(text) + "</span>").c_str());
        qt::SetVisible(g_status, !text.empty()); g_noticeTick = GetTickCount64();
    }

    // clicks are queued and handled from the idle tick (outside Qt's event delivery)
    bool OnEvent(void* w, int type)
    {
        if (type == 3 /* MouseButtonRelease */ && (w == g_btnStart || w == g_btnSend) && qt::IsDown(w) && qt::CursorInside(w))
            g_pendingClick = w;
        if (type == 2 /* MouseButtonPress: a QLabel ignores it, but the filter sees it first */ && w == g_colorButton)
            g_pendingClick = w;
        if (type == 6 /* KeyPress */ && w == g_chatEdit && (GetKeyState(VK_RETURN) & 0x8000)) { g_pendingSend = true; return true; }
        return false;
    }

    void SaveIni(const wchar_t* role)
    {
        Config& c = Cfg();
        c.role = role;
        c.hostAddr = Widen(Trim(qt::EditText(g_addr)));
        c.port = static_cast<unsigned>(atoi(Trim(qt::EditText(g_port)).c_str()));
        c.userName = Widen(Trim(qt::EditText(g_name)));
        c.hideIp = qt::IsChecked(g_hideIp);
        SaveNetConfig();
    }

    // a filled circle in the colour, as rich text for a QLabel
    std::string Dot(uint32_t argb, int pt)
    {
        return "<span style=\"color:" + presence::ColorHex(argb) + "; font-size:" + std::to_string(pt) + "pt\">&#9679;</span>";
    }

    void UpdateColorButton()
    {
        uint32_t local = presence::LocalColor();      // automatic until picked, and that follows our peer id
        if (local == g_colorShown) return;
        g_colorShown = local;
        std::string html = Dot(local, 14) + "&nbsp;<span style=\"color:#d0d0d0\">" + presence::ColorHex(local) + "</span>";
        qt::SetLabelText(g_colorButton, html.c_str());
    }

    void PickColor(uint32_t argb)
    {
        presence::SetLocalColor(argb);
        UpdateColorButton();
        g_rows.clear();                  // redraw the peer list with our new dot
    }

    void OnColorClicked()
    {
        uint32_t cur = presence::LocalColor();
        const ModToolsUiApi* ui = rp::host->ui;
        if (ui && reinterpret_cast<const char*>(&ui->PickColor) < reinterpret_cast<const char*>(ui) + ui->size && ui->PickColor)
        {
            // Radiant's own colour picker (QColorDialog)
            uint32_t picked = 0;
            g_inColorDialog = true;      // modal: the idle tick keeps running meanwhile
            int ok = ui->PickColor(g_root, cur, "Player Color", &picked);
            g_inColorDialog = false;
            if (ok) PickColor(0xFF000000 | picked);
            return;
        }
        // older host: the Windows colour dialog
        static COLORREF custom[16] = {};
        CHOOSECOLORW cc = { sizeof cc };
        cc.hwndOwner = mt::MainWindow();
        cc.rgbResult = RGB((cur >> 16) & 0xFF, (cur >> 8) & 0xFF, cur & 0xFF);
        cc.lpCustColors = custom;
        cc.Flags = CC_RGBINIT | CC_FULLOPEN;
        g_inColorDialog = true;          // the dialog pumps messages, so the idle tick keeps running meanwhile
        BOOL ok = ChooseColorW(&cc);
        g_inColorDialog = false;
        if (ok) PickColor(0xFF000000 | (uint32_t(GetRValue(cc.rgbResult)) << 16) | (uint32_t(GetGValue(cc.rgbResult)) << 8) | GetBValue(cc.rgbResult));
    }

    void UpdateModeControls()
    {
        bool active = presence::IsActive();
        bool join = qt::IsChecked(g_radioJoin);
        g_lastJoin = join;
        qt::SetEnabled(g_addr, join && !active);
        for (void* w : { g_radioHost, g_radioJoin, g_port, g_name }) qt::SetEnabled(w, !active);
        g_buttonText = active ? "Stop" : (join ? "Join" : "Host");
        qt::SetButtonText(g_btnStart, g_buttonText.c_str());
    }

    void Refresh()
    {
        if (!g_root) return;
        bool active = presence::IsActive();
        UpdateModeControls();
        static bool wasActive = false;
        if (wasActive && !active) SaveIni(L"off");       // ended on its own (host said goodbye): do not auto-join next time
        wasActive = active;
        std::string warn = active ? mapsync::Warning() : "";
        std::string edits = active && net::IsConnected() ? mapsync::Status() : "";     // "SENT n  RECV n [PENDING] [PREFAB]"
        std::string session = presence::SessionText(g_lastHideIp);
        if (g_noticeTick && (active || GetTickCount64() - g_noticeTick > 15000)) { qt::SetLabelText(g_status, ""); qt::SetVisible(g_status, false); g_noticeTick = 0; }
        if (!warn.empty()) SetStatus(session + " " + warn, Tone::Warn, false);
        else SetStatus(session + (edits.empty() ? "" : "  (edits " + edits + ")"), Tone::Plain, false);

        UpdateColorButton();
        // one row per player: a dot in their camera colour, then the name
        std::vector<std::string> rows;       // "#RRGGBB name  (you)", also what the `read` command prints
        std::string html;
        auto addRow = [&](uint32_t color, const std::string& name, const char* tag, const std::string& prefab)
        {
            rows.push_back(presence::ColorHex(color) + " " + name + tag + (prefab.empty() ? "" : "  [in prefab " + prefab + "]"));
            html += (html.empty() ? "" : "<br>") + Dot(color, 13) + "&nbsp;&nbsp;" + Escape(name) + "<span style=\"color:#909090\">" + tag + "</span>";
            if (!prefab.empty()) html += "&nbsp;&nbsp;<span style=\"color:#E0B040\">in prefab " + Escape(prefab) + "</span>";
        };
        if (active)
        {
            addRow(presence::LocalColor(), presence::LocalName(), net::IsHost() ? "  (you, host)" : "  (you)", presence::PrefabChainText(presence::LocalPrefabChain(), false));
            for (const auto& p : presence::Snapshot()) addRow(p.color, p.name, p.id == net::kHostPeerId ? "  (host)" : "", presence::PrefabChainText(p.prefabChain, false));
        }
        if (rows != g_rows || rows.empty() != g_peersEmptyShown)
        {
            g_rows = rows;
            g_peersEmptyShown = rows.empty();
            qt::SetLabelText(g_peers, rows.empty() ? "<span style=\"color:#909090\">Nobody yet. Host or join a session.</span>" : html.c_str());
        }
        std::string chat;
        for (const auto& cl : presence::ChatHistory(50)) chat += cl.name + ": " + cl.text + "\n";
        if (chat != g_chatShown)
        {
            g_chatShown = chat;
            qt::PlainSetText(g_chatLog, chat.c_str());
            qt::PlainScrollToEnd(g_chatLog);
        }
    }

    void OnStartStop()
    {
        if (presence::IsActive())
        {
            presence::Stop();
            SaveIni(L"off");
            presence::LeaveTempMap("you stopped the session", false);   // a temporary copy of the host's map: save it or discard it
            Refresh();
            return;
        }
        bool join = qt::IsChecked(g_radioJoin);
        std::string addr = Trim(qt::EditText(g_addr)), name = Trim(qt::EditText(g_name));
        int port = atoi(Trim(qt::EditText(g_port)).c_str());
        if (port <= 0 || port > 65535) { SetStatus("Port must be between 1 and 65535.", Tone::Warn); return; }
        if (join && addr.empty()) { SetStatus("Enter the host's IP address (their Radmin / Hamachi / LAN address).", Tone::Warn); return; }
        SaveIni(join ? L"client" : L"host");
        if (join) presence::StartClient(addr, uint16_t(port), name);
        else
        {
            bool localOnly = Cfg().bindAddr == L"127.0.0.1";   // [net] bind, 127.0.0.1 only in test loops
            bool ok = presence::StartHost(uint16_t(port), name, localOnly ? "127.0.0.1" : "0.0.0.0");
            if (!ok) { SetStatus("Could not listen on that port (is another Radiant already hosting on it?).", Tone::Warn); return; }
            if (!localOnly) { qt::SetLabelText(g_hint, "If Windows asks whether Radiant may accept connections, choose Allow."); qt::SetVisible(g_hint, true); }
        }
        Refresh();
    }

    void SendChat()
    {
        std::string t = Trim(qt::EditText(g_chatEdit));
        if (t.empty()) return;
        if (!presence::IsActive()) { SetStatus("Start or join a session first.", Tone::Warn); return; }
        presence::SendChat(t);
        qt::SetEditText(g_chatEdit, "");
    }

    void Build()
    {
        const Config& c = Cfg();
        g_root = qt::Widget(nullptr);
        void* main = qt::VBox(g_root);
        qt::LayoutMargins(main, 8, 8, 8, 8);
        qt::LayoutSpacing(main, 6);

        void* modes = qt::HBox();
        g_radioHost = qt::RadioButton("Host a session on this PC", g_root);
        g_radioJoin = qt::RadioButton("Join a session", g_root);
        qt::BoxAddWidget(modes, g_radioHost); qt::BoxAddWidget(modes, g_radioJoin); qt::BoxAddStretch(modes);
        qt::BoxAddLayout(main, modes);

        void* grid = qt::Grid();
        qt::LayoutSpacing(grid, 6);
        g_addr = qt::LineEdit(Narrow(c.hostAddr).c_str(), g_root);
        qt::SetPlaceholder(g_addr, "host's Radmin / Hamachi / LAN IP");
        g_port = qt::LineEdit(std::to_string(c.port).c_str(), g_root);
        qt::SetMaximumWidth(g_port, 90);
        g_name = qt::LineEdit(Narrow(c.userName).c_str(), g_root);
        qt::SetPlaceholder(g_name, "shown to the other mappers");
        g_hideIp = qt::CheckBox("Hide", g_root);
        qt::SetToolTip(g_hideIp, "Shows the host's IP address as dots and leaves it out of the status line, so it stays off a stream or screen share. Pasting into the field still works.");
        qt::GridAddWidget(grid, qt::Label("Host IP address", g_root), 0, 0); qt::GridAddWidget(grid, g_addr, 0, 1); qt::GridAddWidget(grid, g_hideIp, 0, 2);
        qt::GridAddWidget(grid, qt::Label("Port", g_root), 1, 0);            qt::GridAddWidget(grid, g_port, 1, 1);
        qt::GridAddWidget(grid, qt::Label("Your name", g_root), 2, 0);       qt::GridAddWidget(grid, g_name, 2, 1);
        qt::GridColumnStretch(grid, 1, 1);
        qt::BoxAddLayout(main, grid);

        g_syncSaves = qt::CheckBox("Sync saves", g_root);
        qt::SetToolTip(g_syncSaves, "Your Ctrl+S of the shared map also saves the map for the other mappers who have this ticked, and their saves save your copy. Prefab files, Save As to another file and autosaves are not shared.");
        qt::BoxAddWidget(main, g_syncSaves);

        g_syncLights = qt::CheckBox("Sync light builds", g_root);
        qt::SetToolTip(g_syncLights, "Your lightning bolt (or Rebuild Sun / Probes / All) on the shared map also starts the same light build for the other mappers who have this ticked, and their builds start yours. Each PC bakes on its own CPU. Pausing and cancelling are not shared; a PC with real lighting off, another map open or inside a prefab skips it.");
        qt::BoxAddWidget(main, g_syncLights);

        void* buttons = qt::HBox();
        g_btnStart = qt::PushButton("Host", g_root);
        qt::BoxAddWidget(buttons, g_btnStart); qt::BoxAddStretch(buttons);
        qt::BoxAddLayout(main, buttons);

        g_status = qt::Label("", g_root);
        qt::SetWordWrap(g_status, true);
        qt::BoxAddWidget(main, g_status);
        qt::SetVisible(g_status, false);

        void* colorRow = qt::HBox();
        qt::LayoutSpacing(colorRow, 10);
        qt::BoxAddWidget(colorRow, qt::Label("Player color", g_root));
        g_colorButton = qt::Label("", g_root);
        qt::SetToolTip(g_colorButton, "The colour the other mappers see your camera, name and selection in. Click to change it.");
        qt::BoxAddWidget(colorRow, g_colorButton);
        qt::BoxAddStretch(colorRow);
        qt::BoxAddLayout(main, colorRow);

        qt::BoxAddWidget(main, qt::Label("Connected", g_root));
        g_peers = qt::Label("", g_root);
        qt::SetWordWrap(g_peers, true);
        qt::BoxAddWidget(main, g_peers);

        // no standing hint line, the label only carries the firewall note after a host start on all interfaces
        g_hint = qt::Label("", g_root);
        qt::SetWordWrap(g_hint, true);
        qt::SetVisible(g_hint, false);
        qt::BoxAddWidget(main, g_hint);

        qt::BoxAddWidget(main, qt::Label("Chat", g_root));
        g_chatLog = qt::PlainTextEdit(g_root);
        qt::SetReadOnly(g_chatLog, true);
        qt::SetMinimumHeight(g_chatLog, 60);
        qt::BoxAddWidget(main, g_chatLog, 1);

        void* chatRow = qt::HBox();
        g_chatEdit = qt::LineEdit("", g_root);
        qt::SetPlaceholder(g_chatEdit, "message, Enter sends");
        g_btnSend = qt::PushButton("Send", g_root);
        qt::BoxAddWidget(chatRow, g_chatEdit, 1); qt::BoxAddWidget(chatRow, g_btnSend);
        qt::BoxAddLayout(main, chatRow);

        qt::SetChecked(c.role == L"client" ? g_radioJoin : g_radioHost, true);
        g_lastHideIp = c.hideIp;
        qt::SetChecked(g_hideIp, g_lastHideIp);
        qt::SetEchoMode(g_addr, g_lastHideIp ? 2 /* Password */ : 0 /* Normal */);
        g_lastSyncSaves = savesync::Enabled();
        qt::SetChecked(g_syncSaves, g_lastSyncSaves);
        g_lastSyncLights = lightsync::Enabled();
        qt::SetChecked(g_syncLights, g_lastSyncLights);

        g_filter = qt::MakeEventFilter(&OnEvent);
        for (void* w : { g_btnStart, g_btnSend, g_chatEdit, g_colorButton }) qt::InstallFilter(w, g_filter);
    }
}

namespace pane
{
    bool Create()
    {
        if (g_root) return true;
        if (!qt::Bind() || !dock::Ready()) return false;
        Build();
        if (!dock::Host(g_root, "Team Create", 420, 590)) { LOG("pane: dock host failed, widgets left unparented"); return false; }
        Refresh();
        LOG("pane: Team Create pane built from Qt widgets (root 0x%p)", g_root);
        return true;
    }

    bool Created() { return g_root != nullptr; }

    void Tick()
    {
        if (!g_root || g_inColorDialog) return;
        if (g_pendingClick)
        {
            void* w = g_pendingClick; g_pendingClick = nullptr;
            if (w == g_btnStart) OnStartStop();
            else if (w == g_btnSend) SendChat();
            else if (w == g_colorButton) { OnColorClicked(); Refresh(); }
        }
        if (g_pendingSend) { g_pendingSend = false; SendChat(); }
        if (qt::IsChecked(g_radioJoin) != g_lastJoin) UpdateModeControls();
        if (qt::IsChecked(g_hideIp) != g_lastHideIp)
        {
            g_lastHideIp = !g_lastHideIp;
            qt::SetEchoMode(g_addr, g_lastHideIp ? 2 : 0);
            Cfg().hideIp = g_lastHideIp; SaveNetConfig();   // only this key changed, the rest is what was loaded or last saved
            Refresh();
        }
        if (qt::IsChecked(g_syncSaves) != g_lastSyncSaves) { g_lastSyncSaves = !g_lastSyncSaves; savesync::SetEnabled(g_lastSyncSaves); }
        if (qt::IsChecked(g_syncLights) != g_lastSyncLights) { g_lastSyncLights = !g_lastSyncLights; lightsync::SetEnabled(g_lastSyncLights); }
        uint64_t now = GetTickCount64();
        if (now - g_lastRefresh >= 1000) { g_lastRefresh = now; Refresh(); }
    }

    // experimental notice as a task dialog: TaskDialogIndirect needs Common Controls 6, which Radiant_modtools.exe does not request,
    // so it runs under an activation context from this DLL's manifest (/manifestdependency pragma below, resource id 2),
    // a plain MessageBox carries the same text when that is unavailable
    void ShowNotice()
    {
        // remembered in TeamCreate.ini [window] experimental_notice=1
        rp::ConfigSet("window", "experimental_notice", "1");
        const wchar_t* const content =
            L"Some things may not work as expected.\n\n"
            L"It shares the host's map, prefabs and custom assets with everyone in the session, live, and rewrites them on disk. "
            L"Replaced files are backed up (maps in radiant\\teamcreate_backup, GDTs in %LOCALAPPDATA%\\RadiantPlugins\\TeamCreate). "
            L"Your model and texture files are never written.\n\n"
            L"Before you start:\n"
            L"  - Make a map with the host's map name in the Mod Tools Launcher (New Map), so it can load, build and play here.\n"
            L"  - Use the host's MTPlugins version.\n"
            L"  - Enter your mapper name and the host's address (Radmin, Hamachi, LAN, or public IP if they forwarded the port).\n\n"
            L"Hosting (the others can reach you either way):\n"
            L"  - Port forwarding (recommended): forward port 27017 (TCP and UDP) on your router and give out your public IP.\n"
            L"  - Radmin or Hamachi: no router setup, and editing together works just as smoothly. Only file transfers are slower: "
            L"custom assets take far longer to stream, and joining the host's game through Play Test may not be workable "
            L"because of how long it takes to join.\n"
            L"  - Never share your IP with anyone else. Keep the Hide box ticked so it stays off streams and screenshots.\n"
            L"  - When Windows asks whether Radiant may accept connections, choose Allow.\n\n"
            L"Playing together: with Play Test on every PC, the host presses Play and the others' Play button becomes "
            L"Join Host, which takes them into the host's game. Radiant edits show up in the game for everyone.\n\n"
            L"You can read this again from the Team Create menu.";
        HWND owner = mt::MainWindow(); bool shown = false; HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&ShowNotice), &self);
        ACTCTXW ctx = { sizeof ctx }; ctx.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID; ctx.hModule = self; ctx.lpResourceName = MAKEINTRESOURCEW(2);
        HANDLE act = self ? CreateActCtxW(&ctx) : INVALID_HANDLE_VALUE; ULONG_PTR cookie = 0;
        if (act != INVALID_HANDLE_VALUE && ActivateActCtx(act, &cookie))
        {
            TASKDIALOGCONFIG td = { sizeof td };
            td.hwndParent = owner; td.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
            td.dwCommonButtons = TDCBF_OK_BUTTON; td.pszWindowTitle = L"Team Create"; td.pszMainIcon = TD_INFORMATION_ICON;
            td.pszMainInstruction = L"Team Create is experimental"; td.pszContent = content;
            shown = SUCCEEDED(TaskDialogIndirect(&td, nullptr, nullptr, nullptr));
            DeactivateActCtx(0, cookie);
        }
        if (act != INVALID_HANDLE_VALUE) ReleaseActCtx(act);
        if (!shown) MessageBoxW(owner, (std::wstring(L"Team Create is experimental. ") + content).c_str(), L"Team Create", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
    }

    void Toggle()
    {
        if (!g_root) return;
        if (dock::Visible()) { dock::Show(false); return; }
        // first opening on this install: the experimental notice
        if (!rp::ConfigGetBool("window", "experimental_notice", false)) ShowNotice();
        Refresh();
        dock::Show(true);
        qt::SetFocus(presence::IsActive() ? g_btnStart : (qt::IsChecked(g_radioJoin) ? g_addr : g_btnStart));
    }

    bool Visible() { return g_root && dock::Visible(); }

    std::string Command(const std::string& raw)
    {
        if (!g_root) return "no pane";
        std::string cmd = Trim(raw), arg;
        size_t sp = cmd.find(' ');
        if (sp != std::string::npos) { arg = Trim(cmd.substr(sp + 1)); cmd = cmd.substr(0, sp); }
        if (cmd == "state") return std::string("visible=") + (dock::Visible() ? "True" : "False");
        if (cmd == "livemove") return presence::LiveMoveStatus();
        if (cmd == "zonesync") return zonesync::Command(arg);   // fetch <map> | status | cancel (Play Test's Join Host)
        if (cmd == "hostplaying") return zonesync::Command(cmd);   // Play Test's Play button: "1 <map>" while the host's game is up (it becomes Join Host)
        if (cmd == "hostaddr")   // Play Test's Join Host: the host's IP while connected as a client, "host" on the host, else empty
            return net::IsHost() ? (net::IsRunning() ? "host" : "") : (net::IsConnected() ? net::HostAddress() : "");
        if (cmd == "peeraddrs") return net::PeerAddresses();   // Play Test's LAN filter: the only addresses BO3's LAN socket accepts
        if (cmd == "deletes") return mapsync::DeletesCommand(arg);
        if (cmd == "syncsaves")
        {
            if (arg == "1" || arg == "0") { savesync::SetEnabled(arg == "1"); g_lastSyncSaves = arg == "1"; qt::SetChecked(g_syncSaves, g_lastSyncSaves); }
            return savesync::Status();
        }
        if (cmd == "synclights")
        {
            if (arg.rfind("build", 0) == 0) return lightsync::BuildLikeBolt(arg.find("sun") != std::string::npos || arg.find("all") != std::string::npos, arg.find("probes") != std::string::npos || arg.find("all") != std::string::npos);
            if (arg == "1" || arg == "0") { lightsync::SetEnabled(arg == "1"); g_lastSyncLights = arg == "1"; qt::SetChecked(g_syncLights, g_lastSyncLights); }
            return lightsync::Status();
        }
        if (cmd == "show") { dock::Show(true); Refresh(); return "shown"; }
        if (cmd == "hide") { dock::Show(false); return "hidden"; }
        if (cmd == "local") { Cfg().bindAddr = arg == "1" ? L"127.0.0.1" : L"0.0.0.0"; return "ok"; }   // test loops: host on 127.0.0.1
        if (cmd == "hideip")    // hideip [0|1]: the Hide box (Tick applies it), prints the box and the status line
        {
            if (arg == "1" || arg == "0") qt::SetChecked(g_hideIp, arg == "1");
            return std::string("hideip=") + (qt::IsChecked(g_hideIp) ? "1" : "0") + " status: " + g_statusText;
        }
        if (cmd == "join") { qt::SetChecked(g_radioJoin, true); qt::SetEditText(g_addr, arg.c_str()); UpdateModeControls(); return "ok"; }
        if (cmd == "host") { qt::SetChecked(g_radioHost, true); UpdateModeControls(); return "ok"; }
        if (cmd == "start") { OnStartStop(); return "status: " + g_statusText; }
        if (cmd == "chat") { qt::SetEditText(g_chatEdit, arg.c_str()); SendChat(); return "sent"; }
        if (cmd == "chatlog") return g_chatShown;
        if (cmd == "color")
        {
            // color               -> current, color auto | #RRGGBB -> set, color pick -> opens the dialog (interactive)
            if (arg == "pick") { g_pendingClick = g_colorButton; return "dialog queued"; }
            if (!arg.empty())
            {
                uint32_t c = presence::ParseColor(arg);
                if (!c && arg != "auto") return "usage: color [auto | #RRGGBB | pick]";
                PickColor(c);
                Refresh();
            }
            return "color=" + presence::ColorHex(presence::LocalColor()) + (presence::ChosenColor() ? "" : " (automatic)");
        }
        if (cmd == "cam")
        {
            float o[3] = {}, a[3] = {};
            if (sscanf_s(arg.c_str(), "%f %f %f %f %f", &o[0], &o[1], &o[2], &a[0], &a[1]) != 5) return "usage: cam x y z pitch yaw";
            return hooks::SetCamera(o, a) ? "ok" : "no camera";
        }
        if (cmd == "gameview") { hooks::SetGameView(arg == "1"); return hooks::GameView() ? "gameview=1" : "gameview=0"; }
        if (cmd == "enter")     // enter <entityGuid>: select that prefab and Enter Prefab (test loops, no keyboard)
        {
            if (!hooks::SelectEntityByGuid(arg)) return "no entity " + arg;
            return hooks::EnterSelectedPrefab() ? "depth=" + std::to_string(hooks::PrefabDepth()) : "Enter Prefab refused";
        }
        if (cmd == "leave") return hooks::LeavePrefab() ? "depth=" + std::to_string(hooks::PrefabDepth()) : "Leave Prefab refused";
        if (cmd == "scope")     // where this Radiant edits and how each entered instance is placed
        {
            std::vector<std::string> chain = presence::LocalPrefabChain(); std::vector<presence::PrefabXf> xf = presence::LocalPrefabXf();
            std::string out = chain.empty() ? "in the map" : "in prefab " + presence::PrefabChainText(chain, false);
            for (size_t i = 0; i < xf.size(); ++i) { char b[128]; _snprintf_s(b, _TRUNCATE, "\n  level %zu: origin %g %g %g angles %g %g %g", i + 1, xf[i].origin[0], xf[i].origin[1], xf[i].origin[2], xf[i].angles[0], xf[i].angles[1], xf[i].angles[2]); out += b; }
            return out;
        }
        if (cmd == "caminfo")
        {
            render::CamParams c = render::LastCam();
            char buf[240];
            RECT rc{}; HWND hw = static_cast<HWND>(render::CamWindow()); if (hw) GetWindowRect(hw, &rc);
            _snprintf_s(buf, _TRUNCATE, "origin=%.1f %.1f %.1f angles=%.1f %.1f %.1f pane=%dx%d gameview=%d rect=%ld %ld %ld %ld", c.origin[0], c.origin[1], c.origin[2], c.angles[0], c.angles[1], c.angles[2], c.width, c.height, c.gameView ? 1 : 0, rc.left, rc.top, rc.right, rc.bottom);
            return buf;
        }
        if (cmd == "read")
        {
            Refresh();
            std::string out = "button: " + g_buttonText + "\nstatus: " + g_statusText + "\n";
            for (const auto& r : g_rows) out += "peer: " + r + "\n";
            return out;
        }
        return "unknown command '" + cmd + "'";
    }

    void Shutdown()
    {
        if (!g_root) return;
        for (void* w : { g_btnStart, g_btnSend, g_chatEdit, g_colorButton }) if (w && g_filter) qt::RemoveFilter(w, g_filter);
        g_root = nullptr;   // the widgets belong to Radiant's dock now
    }
}
