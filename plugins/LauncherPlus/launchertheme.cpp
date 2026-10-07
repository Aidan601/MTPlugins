// Themes for the Mod Tools Launcher: a sheet of <root>\bin\themes applied over the launcher's own two looks, picked
// in a "Theme:" combo in Edit > Options in place of "Use Treyarch Theme". Classic Treyarch and Classic are the
// launcher's own looks (name= stays empty).
#include "modtools_plugin.hpp"
#include "launchertheme.h"
#include "themeicons.h"
#include "discordrpc.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace launchertheme
{
namespace
{
    struct QArrayDataRaw { int ref; int size; unsigned alloc; ptrdiff_t offset; };
    struct Qt
    {
        bool bound = false;
        void (*qsDtor)(void**) = nullptr;
        void* (*qstringFromUtf8)(void*, const char*, int) = nullptr;
        void* (*appInstance)() = nullptr;
        void* (*appSetStyle)(const void*) = nullptr;                    // static QApplication::setStyle(const QString&)
        void (*appSetStyleSheet)(void*, const void*) = nullptr;
        void** (*appStyleSheet)(const void*, void**) = nullptr;         // QApplication::styleSheet(), QString by hidden pointer
        void* (*activeModal)() = nullptr;
        unsigned long long (*winId)(const void*) = nullptr;
        void* (*layout)(const void*) = nullptr;
        void (*boxInsertWidget)(void*, int, void*, int, int) = nullptr;
        void** (*buttonText)(const void*, void**) = nullptr;            // QAbstractButton::text()
        void* (*boxButton)(const void*, int) = nullptr;                 // QDialogButtonBox::button(StandardButton)
        void (*click)(void*) = nullptr;                                 // QAbstractButton::click() (test aid)
    } qt;
    template <class T> bool Get(T& fn, HMODULE m, const char* name) { fn = reinterpret_cast<T>(GetProcAddress(m, name)); return fn != nullptr; }
    bool BindQt()
    {
        HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll"), c = GetModuleHandleW(L"Qt5Core64r.dll");
        if (!w || !c) return false;
        bool ok = true;
        ok &= Get(qt.qsDtor, c, "??1QString@@QEAA@XZ");
        ok &= Get(qt.qstringFromUtf8, c, "?fromUtf8@QString@@SA?AV1@PEBDH@Z");
        ok &= Get(qt.appInstance, c, "?instance@QCoreApplication@@SAPEAV1@XZ");
        ok &= Get(qt.appSetStyle, w, "?setStyle@QApplication@@SAPEAVQStyle@@AEBVQString@@@Z");
        ok &= Get(qt.appSetStyleSheet, w, "?setStyleSheet@QApplication@@QEAAXAEBVQString@@@Z");
        ok &= Get(qt.appStyleSheet, w, "?styleSheet@QApplication@@QEBA?AVQString@@XZ");
        ok &= Get(qt.activeModal, w, "?activeModalWidget@QApplication@@SAPEAVQWidget@@XZ");
        ok &= Get(qt.winId, w, "?winId@QWidget@@QEBA_KXZ");
        ok &= Get(qt.layout, w, "?layout@QWidget@@QEBAPEAVQLayout@@XZ");
        ok &= Get(qt.boxInsertWidget, w, "?insertWidget@QBoxLayout@@QEAAXHPEAVQWidget@@HV?$QFlags@W4AlignmentFlag@Qt@@@@@Z");
        ok &= Get(qt.buttonText, w, "?text@QAbstractButton@@QEBA?AVQString@@XZ");
        ok &= Get(qt.boxButton, w, "?button@QDialogButtonBox@@QEBAPEAVQPushButton@@W4StandardButton@1@@Z");
        ok &= Get(qt.click, w, "?click@QAbstractButton@@QEAAXXZ");
        return qt.bound = ok;
    }

    struct Entry { std::string name, label; std::wstring file; bool classic = false; };
    std::vector<Entry> g_list;             // [0] = the launcher's own look
    std::wstring g_ini;
    std::string g_current;                 // the ini choice ("" = the launcher's own look, nothing applied)
    std::string g_applied;                 // the theme in effect ("" = none applied by us)
    std::string g_pending; bool g_hasPending = false;
    bool g_fromOptions = false;            // the pending pick came from Options: the launcher has just set its own look
    bool g_started = false, g_touched = false;
    std::string g_style;                   // the base style we set last ("" = unknown)
    std::string g_ownSheet; bool g_ownSaved = false;   // the launcher's sheet before our first apply
    std::string g_last;

    std::wstring Root() { return rp::host->RadiantRoot(rp::self); }
    std::wstring Dir() { return Root() + L"\\bin\\themes"; }
    std::string Lower(std::string s) { for (char& ch : s) ch = char(tolower(static_cast<unsigned char>(ch))); return s; }
    std::wstring Widen(const std::string& s)
    {
        if (s.empty()) return L"";
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
        std::wstring w(size_t(n), L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), &w[0], n); return w;
    }
    std::string Narrow(const std::wstring& w)
    {
        if (w.empty()) return "";
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(size_t(n), '\0'); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), &s[0], n, nullptr, nullptr); return s;
    }
    bool ReadWhole(const std::wstring& path, std::string& out)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size = {}; GetFileSizeEx(h, &size);
        if (size.QuadPart > (8 << 20)) { CloseHandle(h); return false; }
        out.assign(size_t(size.QuadPart), '\0');
        DWORD got = 0; bool ok = size.QuadPart == 0 || (::ReadFile(h, &out[0], DWORD(size.QuadPart), &got, nullptr) && got == DWORD(size.QuadPart));
        CloseHandle(h);
        if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF && static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF) out.erase(0, 3);
        return ok;
    }
    // '/* base-style: plastique */' within the first 2 KB of the sheet
    std::string StyleDirective(const std::string& sheet)
    {
        size_t p = sheet.find("base-style:");
        if (p == std::string::npos || p > 2048) return "";
        p += 11;
        while (p < sheet.size() && (sheet[p] == ' ' || sheet[p] == '\t')) ++p;
        size_t e = p;
        while (e < sheet.size() && (isalnum(static_cast<unsigned char>(sheet[e])) || sheet[e] == '_' || sheet[e] == '-')) ++e;
        return sheet.substr(p, e - p);
    }
    // relative url(...) references go against the sheet's folder
    std::string Resolve(const std::string& sheet, const std::wstring& baseDir)
    {
        std::string out; out.reserve(sheet.size() + 512);
        size_t i = 0;
        for (;;)
        {
            size_t p = sheet.find("url(", i);
            size_t close = p == std::string::npos ? std::string::npos : sheet.find(')', p);
            if (close == std::string::npos) { out.append(sheet, i, std::string::npos); break; }
            out.append(sheet, i, p - i);
            std::string inner = sheet.substr(p + 4, close - p - 4);
            while (!inner.empty() && (inner.back() == ' ' || inner.back() == '\t' || inner.back() == '"' || inner.back() == '\'')) inner.pop_back();
            size_t b = 0; while (b < inner.size() && (inner[b] == ' ' || inner[b] == '\t' || inner[b] == '"' || inner[b] == '\'')) ++b;
            inner.erase(0, b);
            if (!inner.empty() && inner[0] != ':' && inner[0] != '/' && inner[0] != '\\' && inner.find(':') == std::string::npos)
            {
                std::string f = Narrow(baseDir) + "/" + inner;
                for (char& ch : f) if (ch == '\\') ch = '/';
                out += "url(\"" + f + "\")";
            }
            else out.append(sheet, p, close + 1 - p);
            i = close + 1;
        }
        return out;
    }
    bool SetStyle(const std::string& key)
    {
        void* qs = nullptr; qt.qstringFromUtf8(&qs, key.c_str(), int(key.size()));
        void* style = qt.appSetStyle(&qs);
        qt.qsDtor(&qs);
        return style != nullptr;
    }
    void SetSheet(const std::string& sheet)
    {
        void* qs = nullptr; qt.qstringFromUtf8(&qs, sheet.c_str(), int(sheet.size()));
        qt.appSetStyleSheet(qt.appInstance(), &qs);
        qt.qsDtor(&qs);
    }
    std::string OwnSheet()
    {
        void* qs = nullptr; qt.appStyleSheet(qt.appInstance(), &qs);
        std::string out;
        if (auto* d = static_cast<QArrayDataRaw*>(qs); d && d->size > 0)
        {
            const wchar_t* w = reinterpret_cast<const wchar_t*>(reinterpret_cast<char*>(d) + d->offset);
            out = Narrow(std::wstring(w, size_t(d->size)));
        }
        qt.qsDtor(&qs);
        return out;
    }

    Entry* Find(const std::string& name)
    {
        std::string l = Lower(name);
        for (Entry& e : g_list) if (Lower(e.name) == l) return &e;
        return nullptr;
    }
    // name "" = the launcher's own look back (plastique under its sheet, Qt's Windows style without)
    bool Apply(const std::string& name, std::string& why)
    {
        if (!qt.bound || !qt.appInstance()) { why = "Qt is not bound"; return false; }
        Entry* e = Find(name);
        if (!e) { why = "no theme named '" + name + "' (see 'theme list')"; return false; }
        if (name.empty() && !g_touched) { g_applied.clear(); return true; }
        ULONGLONG t0 = GetTickCount64();
        if (!g_ownSaved) { g_ownSheet = OwnSheet(); g_ownSaved = true; }
        std::string style = "WindowsVista", sheet, titleSheet;
        std::wstring pack, logo;
        if (name.empty()) { sheet = g_ownSheet; if (!sheet.empty()) style = "plastique"; }
        else if (!e->classic)
        {
            if (!ReadWhole(e->file, sheet)) { why = "cannot read " + Narrow(e->file); return false; }
            style = StyleDirective(sheet);
            if (style.empty()) style = "plastique";
            size_t cut = e->file.find_last_of(L"\\/");
            std::wstring sheetDir = cut == std::wstring::npos ? Dir() : e->file.substr(0, cut);
            if (!themeicons::IsClassicTreyarch(e->name)) pack = themeicons::PackFor(Dir(), e->name, sheet, sheetDir);
            logo = themeicons::LogoFor(sheet, sheetDir, "launcher");
            titleSheet = sheet;
            sheet = Resolve(sheet, sheetDir);
        }
        themeicons::Use(pack, logo);                  // before the sheet, so its url(:/...) images come from the pack
        themeicons::UseTitleBar(titleSheet, name.empty() ? (sheet.empty() ? "classic" : themeicons::kClassicTreyarch) : e->classic ? "classic" : e->name);
        if (Lower(style) != Lower(g_style))           // unknown at first: the launcher's own
        {
            if (sheet.empty()) SetSheet("");          // setStyle is cheaper without a sheet
            if (SetStyle(style)) g_style = style; else rp::Log("theme: base style '%s' is not available", style.c_str());
        }
        SetSheet(sheet);
        g_touched = true;
        g_applied = e->name;
        char buf[160]; snprintf(buf, sizeof buf, "'%s' applied (style %s) in %llu ms", e->name.empty() ? "launcher" : e->name.c_str(), g_style.c_str(),
                                static_cast<unsigned long long>(GetTickCount64() - t0));
        g_last = buf;
        rp::Log("theme: %s", buf);
        return true;
    }

    void Scan()
    {
        g_list.clear();
        g_list.push_back({ "", "Launcher's own", L"", false });
        g_list.push_back({ themeicons::kClassicTreyarch, themeicons::kClassicTreyarch, Root() + L"\\radiant\\stylesheet.qss", false });
        g_list.push_back({ "classic", "Classic", L"", true });
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((Dir() + L"\\*.qss").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                std::wstring f = fd.cFileName;
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || f.size() <= 4) continue;
                std::string name = Narrow(f.substr(0, f.size() - 4));
                if (name.empty() || Lower(name) == "default" || Find(name)) continue;   // built-in names win
                g_list.push_back({ name, name, Dir() + L"\\" + f, false });
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::stable_sort(g_list.begin() + 1, g_list.end(), [](const Entry& a, const Entry& b) { return themeicons::ListBefore(a.name, b.name); });   // [0], the launcher's own, stays first
    }
    void Save(const std::string& name)
    {
        g_current = name;
        WritePrivateProfileStringW(L"theme", L"name", Widen(name).c_str(), g_ini.c_str());
    }

    // ---- Edit > Options: the "Theme:" combo in place of "Use Treyarch Theme", then "Discord presence:" ----
    struct Options
    {
        void* dialog = nullptr; void* check = nullptr; void* buttons = nullptr; void* row = nullptr; void* combo = nullptr;
        void* discord = nullptr;
        std::vector<size_t> rows;
        std::string before;                // g_applied when the dialog opened: put back unless OK
        bool previewed = false, ok = false;
    };
    Options g_opt;                         // dialog set and combo null = a dialog left alone
    std::string g_optLast;
    std::string g_preview; bool g_hasPreview = false;   // picked in the combo, shown on the next tick (not saved)
    std::string Text(void* button)
    {
        void* qs = nullptr; qt.buttonText(button, &qs);
        std::string out;
        if (auto* d = static_cast<QArrayDataRaw*>(qs); d && d->size > 0)
            out = Narrow(std::wstring(reinterpret_cast<const wchar_t*>(reinterpret_cast<char*>(d) + d->offset), size_t(d->size)));
        qt.qsDtor(&qs);
        return out;
    }
    bool IsOptionsDialog(void* w)
    {
        HWND h = reinterpret_cast<HWND>(qt.winId(w));
        wchar_t title[32] = {};
        return h && GetWindowTextW(h, title, 32) && wcscmp(title, L"Options") == 0;
    }
    // ini theme, else the checkbox (ticked = Classic Treyarch, else Classic)
    std::string ShownName(bool ticked) { return !g_applied.empty() ? g_applied : ticked ? themeicons::kClassicTreyarch : "classic"; }
    void OnOptionsPick(void*, int i)
    {
        if (!g_opt.combo || i < 0 || size_t(i) >= g_opt.rows.size()) return;
        g_preview = g_list[g_opt.rows[size_t(i)]].name; g_hasPreview = true;   // outside the combo's signal
    }
    void OnOptionsOk(void*)
    {
        if (!g_opt.combo) return;
        if (g_opt.discord)
            if (int d = rp::host->ui->ComboCurrent(g_opt.discord); d >= 0 && d < 3 && d != discordrpc::GetMode()) discordrpc::SetMode(discordrpc::Mode(d));
        int i = rp::host->ui->ComboCurrent(g_opt.combo);
        if (i < 0 || size_t(i) >= g_opt.rows.size()) return;
        const Entry& e = g_list[g_opt.rows[size_t(i)]];
        // the launcher reads the box after exec(), so it follows the pick
        rp::host->ui->SetChecked(g_opt.check, e.classic ? 0 : 1);
        g_pending = (e.classic || themeicons::IsClassicTreyarch(e.name)) ? "" : e.name;
        g_hasPending = true; g_fromOptions = true; g_hasPreview = false; g_opt.ok = true;   // applied next tick, after UpdateTheme
    }
    void EnhanceOptions(void* dialog)
    {
        const ModToolsUiApi* ui = rp::host->ui;
        g_opt = Options(); g_opt.dialog = dialog;
        void* check = ui->FindChild(dialog, "QCheckBox", 0);
        void* buttons = ui->FindChild(dialog, "QDialogButtonBox", 0);
        void* layout = qt.layout(dialog);
        void* ok = buttons ? qt.boxButton(buttons, 0x400 /*QDialogButtonBox::Ok*/) : nullptr;
        if (!check || Text(check) != "Use Treyarch Theme" || ui->FindChild(dialog, "QCheckBox", 1) || !layout || !ok)
        {
            g_optLast = "an Options dialog with another layout: left alone";
            rp::Log("theme: %s", g_optLast.c_str());
            return;
        }
        void* row = ui->Widget(dialog);
        void* h = ui->HBox();
        ui->LayoutMargins(h, 0, 0, 0, 0);
        void* label = ui->Label("Theme:", row);
        void* combo = ui->ComboBox(row);
        ui->SetToolTip(combo, "The launcher's look: Classic Treyarch and Classic are its own two (the old \"Use Treyarch Theme\" on and off), Treyarch a modern theme in its colours; "
                              "the others are the themes of bin\\themes, shared with Radiant and APE.");
        const std::string shown = Lower(ShownName(ui->IsChecked(check) != 0));
        int current = 0;
        for (size_t i = 1; i < g_list.size(); ++i)     // [0], the launcher's own, is Classic Treyarch or Classic here
        {
            if (Lower(g_list[i].name) == shown) current = int(g_opt.rows.size());
            g_opt.rows.push_back(i);
            ui->ComboAdd(combo, g_list[i].label.c_str());
        }
        ui->ComboSetCurrent(combo, current);
        ui->BoxAddWidget(h, label, 0);
        ui->BoxAddWidget(h, combo, 1);
        ui->SetLayout(row, h);
        qt.boxInsertWidget(layout, 0, row, 0, 0);
        ui->SetVisible(check, 0);

        // index 1: below Theme, above the Build Language row
        static const char* const kDiscord[3] = { "Off", "Show the launcher", "Show the launcher and the build" };
        void* drow = ui->Widget(dialog);
        void* dh = ui->HBox();
        ui->LayoutMargins(dh, 0, 0, 0, 0);
        void* dlabel = ui->Label("Discord presence:", drow);
        void* discord = ui->ComboBox(drow);
        for (const char* s : kDiscord) ui->ComboAdd(discord, s);
        ui->ComboSetCurrent(discord, discordrpc::GetMode());
        ui->SetToolTip(discord, "What your Discord profile shows while the launcher is in use. Off: nothing. Show the launcher: only that it is open. "
                                "Show the launcher and the build: also the checked map or mod and the build step. Radiant and APE have their own setting.");
        ui->BoxAddWidget(dh, dlabel, 0);
        ui->BoxAddWidget(dh, discord, 1);
        ui->SetLayout(drow, dh);
        qt.boxInsertWidget(layout, 1, drow, 0, 0);
        rp::host->OnClicked(rp::self, ok, OnOptionsOk, nullptr);
        if (rp::Has(&rp::host->OnChanged)) rp::host->OnChanged(rp::self, combo, OnOptionsPick, nullptr);   // after the fill: no pick yet
        g_opt.before = g_applied;
        g_opt.check = check; g_opt.buttons = buttons; g_opt.row = row; g_opt.combo = combo; g_opt.discord = discord;
        g_optLast = "open, showing " + shown;
        rp::Log("theme: Options dialog extended (%zu themes, showing %s)", g_opt.rows.size(), shown.c_str());
    }
    // test aid: opens Options from a timer (exec() blocks, the host tick keeps running)
    void CALLBACK OpenTimer(HWND, UINT, UINT_PTR id, DWORD)
    {
        KillTimer(nullptr, id);
        if (rp::host->TriggerMenuAction(rp::self, "Edit/Options") != 1) g_optLast = "Edit > Options not found";
    }
    std::string OptionsCommand(const std::string& arg)
    {
        const ModToolsUiApi* ui = rp::host->ui;
        if (arg == "open") { SetTimer(nullptr, 0, 10, OpenTimer); return "options: opening"; }
        if (arg.empty() || arg == "read")
        {
            std::string r = "options: " + g_optLast;
            if (g_opt.combo)
            {
                int i = ui->ComboCurrent(g_opt.combo);
                r += std::string(", combo='") + (i >= 0 && size_t(i) < g_opt.rows.size() ? g_list[g_opt.rows[size_t(i)]].name : "?")
                   + "' of " + std::to_string(g_opt.rows.size()) + ", hidden box " + (ui->IsChecked(g_opt.check) ? "ticked" : "unticked")
                   + ", discord " + std::to_string(ui->ComboCurrent(g_opt.discord));
            }
            return r;
        }
        if (!g_opt.combo) return "options: the dialog is not open";
        if (arg.rfind("pick ", 0) == 0)
        {
            const std::string want = Lower(arg.substr(5));
            for (size_t i = 0; i < g_opt.rows.size(); ++i)
                if (Lower(g_list[g_opt.rows[i]].name) == want) { ui->ComboSetCurrent(g_opt.combo, int(i)); return "options: picked " + want; }
            return "options: no theme '" + want + "' in the combo";
        }
        if (arg.rfind("discord ", 0) == 0)
        {
            const int d = atoi(arg.c_str() + 8);
            if (d < 0 || d > 2) return "options: discord 0|1|2";
            ui->ComboSetCurrent(g_opt.discord, d);
            return "options: discord " + std::to_string(d);
        }
        if (arg == "ok" || arg == "cancel")
        {
            void* b = qt.boxButton(g_opt.buttons, arg == "ok" ? 0x400 : 0x400000 /*Cancel*/);
            if (!b) return "options: no " + arg + " button";
            qt.click(b);
            return "options: " + arg + " clicked";
        }
        return "options: open | read | pick <name> | discord 0|1|2 | ok | cancel";
    }
}

void Setup(const std::wstring& ini)
{
    g_ini = ini;
    wchar_t buf[260] = {};
    GetPrivateProfileStringW(L"theme", L"name", L"\x01unset", buf, 260, ini.c_str());
    g_current = Narrow(buf);   // kUnset: first run, decided on the first tick
    if (g_current != themeicons::kUnset) g_current = themeicons::Renamed(g_current);
    if (!BindQt()) { rp::Log("theme: some Qt exports are missing, launcher themes are off"); return; }
    themeicons::SetRecolor(false);   // icon folders yes, recoloured icons no
    themeicons::Setup(Root());
    Scan();
}

void Tick()
{
    if (!qt.bound) return;
    void* modal = qt.activeModal();
    if (g_opt.dialog && modal != g_opt.dialog)                      // closed (a stack object: never compared later)
    {
        if (g_opt.previewed && !g_opt.ok) { g_pending = g_opt.before; g_hasPending = true; }   // Cancel or closed: the look back
        g_opt = Options(); g_hasPreview = false;
    }
    if (!g_opt.dialog && modal && IsOptionsDialog(modal)) EnhanceOptions(modal);
    if (g_hasPreview && g_opt.combo)
    {
        std::string why; g_hasPreview = false;
        if (Apply(g_preview, why)) g_opt.previewed = true; else rp::Log("theme: %s", why.c_str());
        return;
    }
    if (g_hasPending)
    {
        std::string name = g_pending, why; g_hasPending = false;
        if (g_fromOptions) { g_style.clear(); g_ownSaved = false; g_fromOptions = false; }   // UpdateTheme has run
        if (Apply(name, why)) Save(g_applied); else rp::Log("theme: %s", why.c_str());
        return;
    }
    if (g_started) return;
    HWND main = rp::Has(&rp::host->MainWindow) ? static_cast<HWND>(rp::host->MainWindow(rp::self)) : nullptr;
    if (!main || !IsWindowVisible(main)) return;        // the launcher has set its own look by then
    g_started = true;
    std::string why;
    if (g_current == themeicons::kUnset)   // first run: Treyarch if the launcher showed its Treyarch look, else Light
    {
        Save(themeicons::FirstRunTheme(!OwnSheet().empty()));
        rp::Log("theme: first run, '%s' picked", g_current.c_str());
    }
    if (!g_current.empty() && !Apply(g_current, why)) rp::Log("theme: %s", why.c_str());
    if (g_current.empty()) themeicons::UseTitleBar("", OwnSheet().empty() ? "classic" : themeicons::kClassicTreyarch);
}

void Shutdown()
{
    if (!qt.bound) return;
    std::string why;
    if (g_touched) Apply("", why);
    themeicons::Shutdown();
    if (g_opt.combo) { rp::host->ui->SetVisible(g_opt.row, 0); rp::host->ui->SetVisible(g_opt.check, 1); }
    g_opt = Options(); g_hasPending = false; g_fromOptions = false; g_hasPreview = false;
    g_started = false; g_touched = false; g_style.clear(); g_ownSaved = false;
}

bool Command(const std::string& c, std::string& reply)
{
    if (c != "theme" && c.rfind("theme ", 0) != 0) return false;
    std::string arg = c.size() > 6 ? c.substr(6) : "";
    if (!qt.bound) { reply = "theme: off (Qt is not bound)"; return true; }
    if (arg.empty()) { reply = "theme: ini='" + g_current + "' applied='" + g_applied + "' themes=" + std::to_string(g_list.size()) + " last: " + g_last; return true; }
    if (arg == "list") { for (const Entry& e : g_list) reply += (e.name.empty() ? "(launcher)" : e.name) + (Lower(e.name) == Lower(g_applied) ? " (applied)\n" : "\n"); return true; }
    if (arg == "options" || arg.rfind("options ", 0) == 0) { reply = OptionsCommand(arg.size() > 8 ? arg.substr(8) : ""); return true; }
    if (arg.rfind("icons", 0) == 0) { reply = themeicons::Command(arg.size() > 6 ? arg.substr(6) : "", Dir()); return true; }
    std::string why;
    if (arg == "launcher") arg.clear();
    if (Apply(arg, why)) { Save(g_applied); reply = "theme: " + g_last; } else reply = "theme: " + why;
    return true;
}
}
