// Action log for APE: diffs the current tab's property grid after input (event filter, QAction::activate, idle check).
// Lines go to the Output pane and a ring buffer for ape_action_log.
#include "modtools_plugin.hpp"
#include "actionlog.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace ba::actionlog
{
namespace
{
    struct QListDataD { int ref; int alloc; int begin; int end; void* array[1]; };
    struct QListRaw { QListDataD* d; };
    struct QArrayDataRaw { int ref; int size; unsigned alloc; ptrdiff_t offset; };
    struct QPointRaw { int x, y; };
    struct QRectRaw { int x1, y1, x2, y2; };
    struct QSizeRaw { int w, h; };

    using ActivateFn      = void (*)(void* action, int event);
    using StringRetFn     = void** (*)(const void* obj, void** ret);
    using ListRetFn       = QListRaw* (*)(const void* obj, QListRaw* ret);
    using ListDisposeFn   = void (*)(QListDataD* d);
    using StringDtorFn    = void (*)(void** s);
    using KeySeqRetFn     = void** (*)(const void* action, void** ret);
    using KeySeqStringFn  = void** (*)(const void* seq, void** ret, int format);
    using KeySeqDtorFn    = void (*)(void** seq);
    using PtrFn           = void* (*)(const void* obj);
    using StaticPtrFn     = void* (*)();
    using WidgetAtFn      = void* (*)(const QPointRaw* pos);
    using ClassNameFn     = const char* (*)(const void* metaObject);
    using GeometryFn      = const QRectRaw* (*)(const void* widget);
    using MoveFn          = void (*)(void* widget, const QPointRaw* p);
    using ResizeFn        = void (*)(void* widget, const QSizeRaw* s);
    using SizeHintFn      = QSizeRaw* (*)(const void* widget, QSizeRaw* ret);
    using VoidFn          = void (*)(void* widget);
    using VisibleToFn     = bool (*)(const void* widget, const void* ancestor);

    struct Qt
    {
        StringRetFn text = nullptr, windowTitle = nullptr, lineEditText = nullptr;
        ListRetFn associatedWidgets = nullptr;
        ListDisposeFn dispose = nullptr;
        StringDtorFn stringDtor = nullptr;
        KeySeqRetFn shortcut = nullptr;
        KeySeqStringFn keySeqString = nullptr;
        KeySeqDtorFn keySeqDtor = nullptr;
        PtrFn menuAction = nullptr, parentWidget = nullptr;
        StaticPtrFn focusWidget = nullptr, appInstance = nullptr;
        WidgetAtFn widgetAt = nullptr;
        ClassNameFn className = nullptr;
        GeometryFn geometry = nullptr;
        MoveFn move = nullptr;
        ResizeFn resize = nullptr;
        SizeHintFn checkBoxSizeHint = nullptr;
        VoidFn raise = nullptr;
        VisibleToFn visibleTo = nullptr;
    } qt;

    ActivateFn g_oActivate = nullptr;
    bool g_hooked = false, g_hookFailed = false, g_faulted = false;
    bool g_on = false;                   // recording: shown, or kept for problem reports
    bool g_display = false;              // [actionlog] enabled, the Output pane check box: printed to the Output pane
    ModToolsWidget g_filter = nullptr;

    // ---- ring buffer and output -----------------------------------------------------------------------------------
    struct Entry { uint64_t seq; std::string text; std::string when; uint64_t tick; };
    std::deque<Entry> g_ring;
    uint64_t g_seq = 0;
    size_t g_ringMax = 2000;

    // in_reports: the last entries also go to <plugin dir>\actionlog.log for problem reports (written to a file because the editor may be gone)
    bool g_inReports = false, g_reportDirty = false;
    uint64_t g_nextReportWrite = 0;
    const uint64_t kReportWindowMs = 15 * 60 * 1000;
    const size_t kReportMax = 300;
    std::string Stamp()
    {
        SYSTEMTIME t; GetLocalTime(&t);
        char b[32]; snprintf(b, sizeof b, "%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        return b;
    }
    std::wstring ReportPath()
    {
        std::wstring dir = rp::PluginDir();
        if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
        return dir + L"actionlog.log";
    }
    void Emit(const std::string& text)
    {
        if (!g_on || text.empty()) return;
        g_ring.push_back({ ++g_seq, text, Stamp(), GetTickCount64() });
        g_reportDirty = true;
        while (g_ring.size() > g_ringMax) g_ring.pop_front();
        if (!g_display) return;
        const ModToolsApeApi* ape = rp::Ape();
        if (!ape || !ape->Print) return;
        // one Print per line (the pane is HTML, so "- " marks a continuation line)
        size_t pos = 0;
        while (pos <= text.size())
        {
            size_t nl = text.find('\n', pos);
            std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            if (line.compare(0, 2, "  ") == 0) line = "    - " + line.substr(2);
            ape->Print(rp::self, line.c_str());
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    }
    // a file older than this process belongs to the previous APE session (maybe a crash): kept as actionlog.prev.log
    void KeepPreviousSession()
    {
        std::wstring path = ReportPath();
        WIN32_FILE_ATTRIBUTE_DATA fa;
        FILETIME created, exited, kernel, user;
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return;
        if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return;
        if (CompareFileTime(&fa.ftLastWriteTime, &created) >= 0) return;   // this session's own file (a hot reload)
        std::wstring prev = path.substr(0, path.size() - 4) + L".prev.log";
        if (MoveFileExW(path.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING)) rp::Log("actionlog: the previous session's log kept as actionlog.prev.log");
    }
    void WriteReportFile()
    {
        g_reportDirty = false;
        uint64_t now = GetTickCount64();
        size_t first = g_ring.size();
        while (first > 0 && g_ring.size() - first < kReportMax && now - g_ring[first - 1].tick <= kReportWindowMs) --first;
        std::string text = "APEPlus action log, written " + Stamp() + ": the entries of the last 15 minutes (at most 300), kept for problem "
                           "reports because [actionlog] in_reports=1 in APEPlus.ini\r\n";
        for (size_t i = first; i < g_ring.size(); ++i)
        {
            text += "#" + std::to_string(g_ring[i].seq) + " " + g_ring[i].when + " ";
            for (char c : g_ring[i].text) { if (c == '\n') text += "\r\n"; else text += c; }
            text += "\r\n";
        }
        std::wstring path = ReportPath(), tmp = path + L".tmp";
        FILE* f = _wfopen(tmp.c_str(), L"wb");
        if (!f) return;
        bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
        ok = fclose(f) == 0 && ok;
        if (ok) MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
        else DeleteFileW(tmp.c_str());
    }

    // ---- small text helpers ---------------------------------------------------------------------------------------
    std::string JsonStr(const std::string& s)
    {
        std::string o = "\"";
        for (char c : s)
        {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if (static_cast<unsigned char>(c) < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", static_cast<unsigned char>(c)); o += b; }
            else o += c;
        }
        return o + "\"";
    }
    std::string Quote(const std::string& s) { return "\"" + (s.size() > 200 ? s.substr(0, 200) + "..." : s) + "\""; }

    template <class T> bool Get(T& fn, HMODULE m, const char* name)
    {
        fn = m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
        return fn != nullptr;
    }

    // ---- Qt reads -------------------------------------------------------------------------------------------------
    std::string Utf8(const wchar_t* w, int n)
    {
        int len = n > 0 ? WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr) : 0;
        std::string out(len > 0 ? len : 0, '\0');
        if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w, n, &out[0], len, nullptr, nullptr);
        return out;
    }
    std::string TakeQString(void*& qs)
    {
        const QArrayDataRaw* a = static_cast<const QArrayDataRaw*>(qs);
        std::string s;
        if (a && a->size > 0) s = Utf8(reinterpret_cast<const wchar_t*>(reinterpret_cast<const char*>(a) + a->offset), a->size);
        if (qt.stringDtor) qt.stringDtor(&qs);
        return s;
    }
    void ListRelease(QListRaw& l)
    {
        if (!l.d || l.d->ref == -1) return;
        if (InterlockedDecrement(reinterpret_cast<volatile long*>(&l.d->ref)) == 0 && qt.dispose) qt.dispose(l.d);
        l.d = nullptr;
    }
    const char* ClassNameSeh(const void* obj)
    {
        __try
        {
            auto vt = *reinterpret_cast<void* const* const*>(obj);
            const void* mo = reinterpret_cast<const void* (*)(const void*)>(vt[0])(obj);
            return mo && qt.className ? qt.className(mo) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    std::string ClassName(const void* obj) { const char* c = obj ? ClassNameSeh(obj) : nullptr; return c ? c : ""; }
    bool IsA(void* obj, const char* cls)
    {
        __try
        {
            auto vt = *reinterpret_cast<void* const* const*>(obj);
            return reinterpret_cast<void* (*)(void*, const char*)>(vt[1])(obj, cls) != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    std::string ObjText(StringRetFn fn, const void* obj)
    {
        if (!fn || !obj) return std::string();
        void* qs = nullptr; fn(obj, &qs);
        return TakeQString(qs);
    }
    std::string CleanLabel(std::string s)
    {
        size_t tab = s.find('\t'); if (tab != std::string::npos) s.resize(tab);
        std::string o;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '&') { if (i + 1 < s.size() && s[i + 1] == '&') { o += '&'; ++i; } continue; }
            o += s[i];
        }
        while (o.size() >= 3 && o.compare(o.size() - 3, 3, "...") == 0) o.resize(o.size() - 3);
        while (!o.empty() && o.back() == ' ') o.pop_back();
        return o;
    }
    std::string ActionPath(void* action, int depth = 0)
    {
        std::string t = CleanLabel(ObjText(qt.text, action));
        if (depth > 5 || !qt.associatedWidgets || !qt.menuAction) return t;
        QListRaw list = {}; qt.associatedWidgets(action, &list);
        std::string parent;
        if (list.d)
            for (int i = list.d->begin; i < list.d->end && parent.empty(); ++i)
            {
                void* w = list.d->array[i];
                if (!w || !IsA(w, "QMenu")) continue;
                void* ma = qt.menuAction(w);
                if (ma && ma != action) parent = ActionPath(ma, depth + 1);
            }
        ListRelease(list);
        return parent.empty() ? t : parent + "/" + t;
    }
    std::string ActionShortcut(void* action)
    {
        if (!qt.shortcut || !qt.keySeqString || !qt.keySeqDtor) return std::string();
        void* seq = nullptr; qt.shortcut(action, &seq);
        void* qs = nullptr; qt.keySeqString(&seq, &qs, 0);
        std::string s = TakeQString(qs);
        qt.keySeqDtor(&seq);
        return s;
    }
    // The control or pane a widget belongs to: the nearest ape* class, named for people, and the dock's title.
    std::string Where(void* w)
    {
        std::string control, area, title;
        for (int i = 0; w && i < 20; ++i)
        {
            std::string c = ClassName(w);
            if (c.compare(0, 3, "ape") == 0)
            {
                if (c == "apeAssetWidget") { if (area.empty()) area = "property grid"; }
                else if (c == "apeAssetTreeWidget") { if (area.empty()) area = "asset tree"; }
                else if (c == "apePreviewWidget") { if (area.empty()) area = "preview"; }
                else if (c == "apeAssetGraphWidget") { if (area.empty()) area = "graph"; }
                else if (c == "apeAssetLineEditWidget" && control.empty()) control = "the tab's asset box";
                else if (control.empty() && c != "apeMainWindow" && c != "apeTabWidget") control = c;
            }
            else if (control.empty() && area.empty() && (c == "QTabBar")) control = "tab bar";
            void* parent = qt.parentWidget ? qt.parentWidget(w) : nullptr;
            if (title.empty() && parent && IsA(w, "QDockWidget") && qt.windowTitle) title = ObjText(qt.windowTitle, w);
            w = parent;
        }
        std::string s = control;
        if (!area.empty()) s += (s.empty() ? "" : " in the ") + area;
        else if (!title.empty()) s += (s.empty() ? "" : " in ") + title + " pane";
        return s;
    }

    // ---- input state ----------------------------------------------------------------------------------------------
    // GetKeyState: the state when the message being handled was posted (a quick tap is already up for GetAsyncKeyState)
    bool Down(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }
    std::string Mods()
    {
        std::string m;
        if (Down(VK_CONTROL)) m += "Ctrl+";
        if (Down(VK_SHIFT)) m += "Shift+";
        if (Down(VK_MENU)) m += "Alt+";
        return m;
    }
    int HeldVk()
    {
        for (int vk = 0x08; vk <= 0xFE; ++vk)
        {
            if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || (vk >= VK_LSHIFT && vk <= VK_RMENU) || vk <= 0x06 ||
                vk == VK_LWIN || vk == VK_RWIN || vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL) continue;
            if (Down(vk)) return vk;
        }
        return 0;
    }
    std::string KeyName(int vk)
    {
        char name[64] = {};
        LONG sc = static_cast<LONG>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)) << 16;
        if (GetKeyNameTextA(sc, name, sizeof name) > 0) return name;
        snprintf(name, sizeof name, "vk 0x%02X", vk);
        return name;
    }

    // ---- what the user last did (event filter) and the scheduled check -----------------------------------------
    std::string g_input;                 // "key Enter on apeStringWidget in the property grid"
    uint64_t g_inputAt = 0;
    bool g_commitKey = false;            // the last input commits a typed value (Enter, Tab, focus out, a click)
    uint64_t g_checkAt = 0;              // a grid check is due at this time (0 = none)
    std::string g_checkSource;
    bool g_checkWeak = false;            // a click outside the property grid: skipped while grid reads are slow
    const char* g_pressButton = nullptr;
    uint64_t g_readMs = 0;               // what the last grid read cost
    const uint64_t kSlowReadMs = 150;    // above this only commits, menu actions and grid clicks are checked, no idle check

    void Schedule(const std::string& source, bool commit, uint64_t delayMs, bool weak = false)
    {
        if (weak && g_readMs > kSlowReadMs) return;
        g_checkSource = source;
        g_checkWeak = weak;
        g_commitKey = commit;
        uint64_t at = GetTickCount64() + delayMs;
        if (!g_checkAt || at < g_checkAt) g_checkAt = at;
    }

    int OnEvent(void*, ModToolsWidget watched, int type)
    {
        if (!g_on || !watched) return 0;
        switch (type)
        {
        case 2:   // MouseButtonPress
            g_pressButton = Down(VK_LBUTTON) ? "left mouse" : Down(VK_RBUTTON) ? "right mouse" : Down(VK_MBUTTON) ? "middle mouse" : "mouse";
            break;
        case 3:   // MouseButtonRelease
            if (IsA(watched, "QWidget"))
            {
                std::string where = Where(watched);
                Schedule(Mods() + (g_pressButton ? g_pressButton : "mouse") + " click on " + where, true, 150, where.find("property grid") == std::string::npos);
            }
            break;
        case 6:   // KeyPress
        {
            int vk = HeldVk();
            if (!vk) break;
            bool commit = vk == VK_RETURN || vk == VK_TAB || vk == VK_SEPARATOR;
            bool undoRedo = (vk == 'Z' || vk == 'Y') && Down(VK_CONTROL);
            if (commit || undoRedo || vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_SPACE)
                Schedule("key " + Mods() + KeyName(vk) + " on " + Where(watched), true, 150);
            break;
        }
        case 9:   // FocusOut
            if (IsA(watched, "QLineEdit") || IsA(watched, "QAbstractSpinBox") || IsA(watched, "QPlainTextEdit") || IsA(watched, "QTextEdit"))
                Schedule("focus left " + Where(watched), true, 100);
            break;
        case 31:  // Wheel
            Schedule("mouse wheel on " + Where(watched), true, 300);
            break;
        default: break;
        }
        return 0;
    }

    // ---- the running menu action (QAction::activate) --------------------------------------------------------------
    struct ActionCtx { std::string path, keys; bool keyHeld = false; };
    ActionCtx g_act;

    std::string ActionSource(const ActionCtx& a)
    {
        std::string s = (a.keyHeld ? "shortcut " : "menu ") + JsonStr(a.path);
        if (!a.keys.empty()) s += " (" + a.keys + ")";
        return s;
    }
    void ActionEnter(void* action)
    {
        g_act = ActionCtx();
        g_act.path = ActionPath(action);
        g_act.keys = ActionShortcut(action);
        g_act.keyHeld = HeldVk() != 0;
    }
    void ActionLeave()
    {
        if (g_act.path.empty()) return;
        std::string text = "[action] " + ActionSource(g_act);
        Emit(text);
        Schedule(ActionSource(g_act), true, 200);   // a value it changed (Undo) is reported with it as the source
    }
    void ActionEnterGuarded(void* a) { try { ActionEnter(a); } catch (...) { g_act = ActionCtx(); } }
    void ActionLeaveGuarded() { try { ActionLeave(); } catch (...) {} }
    void ResetAct() { g_act = ActionCtx(); }
    void SehActionEnter(void* a) { __try { ActionEnterGuarded(a); } __except (EXCEPTION_EXECUTE_HANDLER) { ResetAct(); } }
    void SehActionLeave() { __try { ActionLeaveGuarded(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    void hk_Activate(void* action, int event)
    {
        if (event != 0 || !g_on || !action) { g_oActivate(action, event); return; }   // 0 = QAction::Trigger
        ActionCtx outer = g_act;
        SehActionEnter(action);
        ActionCtx mine = g_act;
        g_oActivate(action, event);
        if (g_on) { g_act = mine; SehActionLeave(); }
        g_act = outer;
    }

    // ---- the property grid ----------------------------------------------------------------------------------------
    struct Row { std::string category, title, kind, value; bool visible = true; };
    struct Grid
    {
        std::string name, type;
        std::vector<Row> rows;
        std::vector<std::string> keys;   // category/title#n per row
        bool valid = false;
        std::string Asset() const { return name.empty() ? std::string() : name + "." + type; }
    };
    Grid g_grid;                          // the last grid read
    uint64_t g_nextIdle = 0;

    std::vector<std::string> SplitTabs(const std::string& line)
    {
        std::vector<std::string> v; size_t st = 0;
        while (true) { size_t t = line.find('\t', st); v.push_back(line.substr(st, t == std::string::npos ? std::string::npos : t - st)); if (t == std::string::npos) break; st = t + 1; }
        return v;
    }
    Grid ReadGrid()
    {
        Grid g;
        const ModToolsApeApi* ape = rp::Ape();
        if (!ape || !ape->CurrentAsset || !ape->Properties) return g;
        uint64_t t0 = GetTickCount64();
        ModToolsApeAsset a = {};
        if (!ape->CurrentAsset(rp::self, &a)) { g.valid = true; g_readMs = GetTickCount64() - t0; return g; }
        g.name = a.name; g.type = a.type;
        size_t n = ape->Properties(rp::self, nullptr, 0);
        std::string text(n + 1, '\0');
        if (n) ape->Properties(rp::self, &text[0], text.size());
        text.resize(strlen(text.c_str()));
        std::map<std::string, int> seen;
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t nl = text.find('\n', pos);
            std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = nl == std::string::npos ? text.size() : nl + 1;
            std::vector<std::string> f = SplitTabs(line);
            if (f.size() < 7) continue;
            Row r; r.category = f[1]; r.title = f[2]; r.kind = f[3]; r.value = f[4]; r.visible = f[6] == "1";
            std::string key = r.category + "/" + r.title;
            key += "#" + std::to_string(seen[key]++);
            g.rows.push_back(r); g.keys.push_back(key);
        }
        g.valid = true;
        g_readMs = GetTickCount64() - t0;
        return g;
    }

    // the text in the control that has the keyboard focus, when it is a text editor (a value still being typed)
    bool FocusedEditText(std::string& out)
    {
        void* f = qt.focusWidget ? qt.focusWidget() : nullptr;
        if (!f || !qt.lineEditText || !IsA(f, "QLineEdit")) return false;
        out = ObjText(qt.lineEditText, f);
        return true;
    }

    std::string KindName(const std::string& k)
    {
        if (k == "apeStringWidget") return "text";
        if (k == "apeCheckBoxWidget") return "check box";
        if (k == "apeComboBoxWidget") return "combo box";
        if (k == "apeSpinBoxArrayWidget" || k == "apeSpinBoxWidget") return "number";
        if (k == "apeTextureWidget") return "image file";
        if (k == "apePathWidget") return "file path";
        if (k == "apeXModelWidget" || k == "apeAssetListWidget") return "asset field";
        if (k == "apeTextWidget") return "text box";
        return k;
    }

    void Check(const std::string& source, bool commit)
    {
        Grid now = ReadGrid();
        if (!now.valid) return;
        if (now.Asset() != g_grid.Asset())
        {
            if (!now.name.empty())
            {
                std::string text = "[action] asset " + now.Asset() + " shown (" + std::to_string(now.rows.size()) + " entries), via " + source;
                Emit(text);
            }
            g_grid = std::move(now);
            return;
        }
        std::unordered_map<std::string, size_t> was;
        for (size_t i = 0; i < g_grid.keys.size(); ++i) was[g_grid.keys[i]] = i;
        std::string focused;
        bool typing = !commit && FocusedEditText(focused);
        std::vector<std::string> lines;
        size_t added = 0;
        for (size_t i = 0; i < now.rows.size(); ++i)
        {
            auto it = was.find(now.keys[i]);
            if (it == was.end()) { ++added; continue; }
            const Row& a = g_grid.rows[it->second];
            const Row& b = now.rows[i];
            if (a.value == b.value) continue;
            if (typing && b.value == focused) { now.rows[i].value = a.value; continue; }   // still being typed: wait for the commit
            std::string title = b.title.empty() ? "entry " + std::to_string(i) : b.title;
            if (!b.category.empty()) title = b.category + " / " + title;
            lines.push_back("  " + title + " (" + KindName(b.kind) + "): " + Quote(a.value) + " -> " + Quote(b.value));
        }
        size_t removed = 0;
        {
            std::unordered_map<std::string, bool> has;
            for (const auto& k : now.keys) has[k] = true;
            for (const auto& k : g_grid.keys) if (!has.count(k)) ++removed;
        }
        g_grid = std::move(now);
        if (lines.empty() && !added && !removed) return;
        std::string text = "[action] edit on " + g_grid.Asset() + " via " + source;
        for (const auto& l : lines) text += "\n" + l;
        if (added || removed) text += "\n  the grid now shows " + std::to_string(added) + " new and " + std::to_string(removed) + " fewer entries (APE rebuilt it for the change)";
        Emit(text);
    }
    void CheckGuarded(const std::string& s, bool commit) { try { Check(s, commit); } catch (...) {} }
    void FaultOff(const char* where)
    {
        g_faulted = true; g_on = false;
        rp::Log("actionlog: a fault in %s; the log is off until it is switched on again", where);
    }
    void SehCheck(const std::string& s, bool commit)
    {
        __try { CheckGuarded(s, commit); }
        __except (EXCEPTION_EXECUTE_HANDLER) { FaultOff("the grid check"); }
    }

    // ---- asset tree selection -------------------------------------------------------------------------------------
    std::vector<std::string> g_treeSel;
    uint64_t g_nextTree = 0;
    void PollTree()
    {
        const ModToolsApeApi* ape = rp::Ape();
        if (!ape || !ape->SelectedAssets) return;
        ModToolsApeAsset buf[16];
        size_t n = ape->SelectedAssets(rp::self, buf, 16);
        std::vector<std::string> sel;
        for (size_t i = 0; i < n && i < 16; ++i) sel.push_back(std::string(buf[i].name) + "." + buf[i].type);
        if (sel == g_treeSel) return;
        g_treeSel = sel;
        if (sel.empty()) return;
        std::string text = "[action] selected in the asset tree: ";
        for (size_t i = 0; i < sel.size() && i < 4; ++i) text += (i ? ", " : "") + sel[i];
        if (n > 4) text += " (+" + std::to_string(n - 4) + " more)";
        Emit(text);
    }
    void PollTreeGuarded() { try { PollTree(); } catch (...) {} }
    void SehPollTree() { __try { PollTreeGuarded(); } __except (EXCEPTION_EXECUTE_HANDLER) { FaultOff("the tree poll"); } }

    // ---- hooks ----------------------------------------------------------------------------------------------------
    bool InstallHooks()
    {
        if (g_hooked) return true;
        if (g_hookFailed) return false;
        void* app = qt.appInstance ? qt.appInstance() : nullptr;
        if (!app || !rp::host->MakeEventFilter || !rp::host->ui || !rp::host->ui->InstallFilter || !rp::Ape() || !rp::Ape()->Properties)
        { g_hookFailed = true; rp::Log("actionlog: Qt or the host's APE services were not found; the log stays off"); return false; }
        g_filter = rp::host->MakeEventFilter(rp::self, OnEvent, nullptr);
        if (!g_filter) { g_hookFailed = true; rp::Log("actionlog: no event filter; the log stays off"); return false; }
        rp::host->ui->InstallFilter(app, g_filter);
        HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll");
        void* activate = w ? reinterpret_cast<void*>(GetProcAddress(w, "?activate@QAction@@QEAAXW4ActionEvent@1@@Z")) : nullptr;
        if (!activate || !rp::host->Detour(rp::self, activate, reinterpret_cast<void*>(&hk_Activate), reinterpret_cast<void**>(&g_oActivate)))
        { g_oActivate = nullptr; rp::Log("actionlog: QAction::activate not hooked; menu entries and shortcuts are not named"); }
        g_hooked = true;
        rp::Log("actionlog: hooks installed (application event filter%s)", g_oActivate ? ", QAction::activate" : "");
        return true;
    }

    // ---- the check box in the Output pane's title bar -------------------------------------------------------------
    void* g_box = nullptr;
    void* g_dock = nullptr;
    QRectRaw g_placedFor = {};
    uint64_t g_nextBox = 0;
    int g_boxTries = 0;

    void SyncBox() { if (g_box && rp::host->ui) rp::host->ui->SetChecked(g_box, g_display ? 1 : 0); }
    void OnBoxClicked(void*) { SetEnabled(rp::host->ui->IsChecked(g_box) != 0); }

    // Left of the dock's float button (or close button), redone when the title buttons move.
    void PlaceBox()
    {
        if (!g_box || !g_dock || !qt.geometry || !qt.move || !qt.resize || !qt.checkBoxSizeHint) return;
        void* anchor = nullptr;
        for (int i = 0; i < 2 && !anchor; ++i)
        {
            void* b = rp::host->ui->FindChild(g_dock, "QDockWidgetTitleButton", i);
            if (b && (!qt.visibleTo || qt.visibleTo(b, g_dock))) anchor = b;
        }
        QRectRaw ref = anchor ? *qt.geometry(anchor) : *qt.geometry(g_dock);
        if (memcmp(&ref, &g_placedFor, sizeof ref) == 0) return;
        g_placedFor = ref;
        QSizeRaw hint = {}; qt.checkBoxSizeHint(g_box, &hint);
        if (hint.w <= 0 || hint.h <= 0) hint = { 90, 16 };
        qt.resize(g_box, &hint);
        QPointRaw p;
        if (anchor) p = { ref.x1 - hint.w - 8, ref.y1 + ((ref.y2 - ref.y1 + 1) - hint.h) / 2 };
        else p = { (ref.x2 - ref.x1) - hint.w - 48, 2 };
        if (p.x < 60) p.x = 60;
        qt.move(g_box, &p);
        if (qt.raise) qt.raise(g_box);
    }
    void TryAddBox()
    {
        const ModToolsUiApi* ui = rp::host->ui;
        if (g_box || !ui || !ui->FindEditorWidget || !ui->FindChild || !ui->CheckBox) return;
        void* dock = ui->FindEditorWidget("OutputToolBar");
        if (!dock || !IsA(dock, "QDockWidget")) return;
        void* cb = ui->CheckBox("Log actions", dock);
        if (!cb) return;
        ui->SetToolTip(cb, "APE+: print every property edit, asset switch and menu action here: the old and new value, "
                           "how it was done (key, mouse, menu)"
                           );
        g_box = cb; g_dock = dock; g_placedFor = {};
        if (rp::host->OnClicked) rp::host->OnClicked(rp::self, cb, OnBoxClicked, nullptr);
        SyncBox();
        PlaceBox();
        ui->SetVisible(cb, 1);
        rp::Log("actionlog: check box added to the Output pane's title bar");
    }
    bool DeleteNowSeh(void* obj)
    {
        __try { (*reinterpret_cast<void (__fastcall***)(void*, unsigned)>(obj))[3](obj, 1); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    std::string Probe()
    {
        Grid g = ReadGrid();
        std::string s = "asset '" + g.Asset() + "', " + std::to_string(g.rows.size()) + " entries, read in " + std::to_string(g_readMs) + " ms";
        for (size_t i = 0; i < g.rows.size() && i < 8; ++i)
            s += "\n" + std::to_string(i) + " " + g.rows[i].category + " / " + g.rows[i].title + " (" + g.rows[i].kind + ") = " + Quote(g.rows[i].value);
        return s;
    }
}

// Recording runs while the log is shown or kept for reports, the hooks go in the first time either is on.
static void UpdateActive()
{
    bool want = (g_display || g_inReports) && InstallHooks();
    if (want && !g_on) { g_faulted = false; g_checkAt = 0; g_grid = ReadGrid(); g_treeSel.clear(); g_nextTree = GetTickCount64() + 500; }
    g_on = want;
}

void Setup()
{
    HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll"), c = GetModuleHandleW(L"Qt5Core64r.dll"), g = GetModuleHandleW(L"Qt5Gui64r.dll");
    Get(qt.text, w, "?text@QAction@@QEBA?AVQString@@XZ");
    Get(qt.associatedWidgets, w, "?associatedWidgets@QAction@@QEBA?AV?$QList@PEAVQWidget@@@@XZ");
    Get(qt.shortcut, w, "?shortcut@QAction@@QEBA?AVQKeySequence@@XZ");
    Get(qt.menuAction, w, "?menuAction@QMenu@@QEBAPEAVQAction@@XZ");
    Get(qt.parentWidget, w, "?parentWidget@QWidget@@QEBAPEAV1@XZ");
    Get(qt.windowTitle, w, "?windowTitle@QWidget@@QEBA?AVQString@@XZ");
    Get(qt.lineEditText, w, "?text@QLineEdit@@QEBA?AVQString@@XZ");
    Get(qt.focusWidget, w, "?focusWidget@QApplication@@SAPEAVQWidget@@XZ");
    Get(qt.widgetAt, w, "?widgetAt@QApplication@@SAPEAVQWidget@@AEBVQPoint@@@Z");
    Get(qt.geometry, w, "?geometry@QWidget@@QEBAAEBVQRect@@XZ");
    Get(qt.move, w, "?move@QWidget@@QEAAXAEBVQPoint@@@Z");
    Get(qt.resize, w, "?resize@QWidget@@QEAAXAEBVQSize@@@Z");
    Get(qt.checkBoxSizeHint, w, "?sizeHint@QCheckBox@@UEBA?AVQSize@@XZ");
    Get(qt.raise, w, "?raise@QWidget@@QEAAXXZ");
    Get(qt.visibleTo, w, "?isVisibleTo@QWidget@@QEBA_NPEBV1@@Z");
    Get(qt.dispose, c, "?dispose@QListData@@SAXPEAUData@1@@Z");
    Get(qt.stringDtor, c, "??1QString@@QEAA@XZ");
    Get(qt.className, c, "?className@QMetaObject@@QEBAPEBDXZ");
    Get(qt.appInstance, c, "?instance@QCoreApplication@@SAPEAV1@XZ");
    Get(qt.keySeqString, g, "?toString@QKeySequence@@QEBA?AVQString@@W4SequenceFormat@1@@Z");
    Get(qt.keySeqDtor, g, "??1QKeySequence@@QEAA@XZ");
    int maxLines = rp::ConfigGetInt("actionlog", "max_lines", 2000);
    g_ringMax = size_t(maxLines < 100 ? 100 : maxLines > 100000 ? 100000 : maxLines);
    g_inReports = rp::ConfigGetBool("actionlog", "in_reports", true);
    KeepPreviousSession();
    g_display = rp::ConfigGetBool("actionlog", "enabled", false);
    UpdateActive();
    if (g_display && !g_on) g_display = false;
}

bool Enabled() { return g_display; }

void SetEnabled(bool on)
{
    bool was = g_display;
    g_display = on;
    UpdateActive();
    if (on && !g_on) g_display = false;   // the hooks could not be installed
    if (g_display != was)
    {
        rp::ConfigSet("actionlog", "enabled", g_display ? "1" : "0");
        const char* line = g_display ? "[action] action log shown (APE+): property edits, asset switches and menu actions are printed here"
                         : g_on ? "[action] action log hidden (still kept for problem reports; [actionlog] in_reports=0 stops that)"
                                : "[action] action log off";
        if (const ModToolsApeApi* ape = rp::Ape()) if (ape->Print) ape->Print(rp::self, line);
    }
    SyncBox();
}

void Tick()
{
    uint64_t now = GetTickCount64();
    if (g_inReports && g_reportDirty && now >= g_nextReportWrite) { g_nextReportWrite = now + 2000; WriteReportFile(); }
    if (now >= g_nextBox)
    {
        g_nextBox = now + 500;
        if (!g_box && g_boxTries < 120) { ++g_boxTries; TryAddBox(); }
        else if (g_box) PlaceBox();
    }
    if (!g_on) return;
    if (g_checkAt && now >= g_checkAt)
    {
        g_checkAt = 0;
        SehCheck(g_checkSource, g_commitKey);
        g_nextIdle = now + 1500;
    }
    else if (now >= g_nextIdle)
    {
        // code edits (a plugin) come without input: a big grid is read less often, a slow one only after input
        g_nextIdle = now + (g_readMs > 25 ? 6000 : 1500);
        if (g_readMs <= kSlowReadMs) SehCheck("code, no key or mouse input seen (a plugin)", false);
    }
    if (now >= g_nextTree) { g_nextTree = now + 400; SehPollTree(); }
}

void Shutdown()
{
    if (g_inReports && g_reportDirty) WriteReportFile();
    g_on = false;
    if (g_box)
    {
        if (!DeleteNowSeh(g_box) && rp::host->ui) rp::host->ui->SetVisible(g_box, 0);
        g_box = nullptr;
    }
    g_dock = nullptr;
    g_hooked = false; g_filter = nullptr;   // the host removes the detour and the filter
}

bool Command(const std::string& command, std::string& reply)
{
    if (command.compare(0, 9, "actionlog") != 0) return false;
    std::string arg = command.size() > 9 ? command.substr(10) : std::string();
    if (arg.empty() || arg == "status")
    {
        reply = std::string("actionlog ") + (g_display ? "shown" : "hidden") + (g_on ? ", recording" : ", not recording") +
                (g_on && g_inReports ? " (kept for problem reports)" : "") + (g_hooked ? ", hooked" : "") + (g_faulted ? ", faulted" : "") +
                ", " + std::to_string(g_ring.size()) + " entries, last seq " + std::to_string(g_seq) + ", last grid read " +
                std::to_string(g_readMs) + " ms" + (g_box ? ", check box in the Output pane" : "");
        if (g_box && qt.geometry)
        {
            const QRectRaw* r = qt.geometry(g_box);
            char b[160]; snprintf(b, sizeof b, " at %d,%d %dx%d beside %d,%d..%d,%d", r->x1, r->y1, r->x2 - r->x1 + 1, r->y2 - r->y1 + 1,
                                  g_placedFor.x1, g_placedFor.y1, g_placedFor.x2, g_placedFor.y2);
            reply += b;
        }
        return true;
    }
    if (arg == "1" || arg == "on") { SetEnabled(true); reply = g_display ? "actionlog shown" : "actionlog could not be switched on (see the log)"; return true; }
    if (arg == "0" || arg == "off") { SetEnabled(false); reply = g_on ? "actionlog hidden, still recording for problem reports" : "actionlog off"; return true; }
    if (arg == "reports 1" || arg == "reports 0")
    {
        g_inReports = arg.back() == '1';
        rp::ConfigSet("actionlog", "in_reports", g_inReports ? "1" : "0");
        if (g_inReports) { g_reportDirty = true; g_nextReportWrite = 0; }
        else { std::wstring path = ReportPath(); DeleteFileW(path.c_str()); DeleteFileW((path.substr(0, path.size() - 4) + L".prev.log").c_str()); }
        UpdateActive();
        reply = g_inReports ? "actionlog entries kept for problem reports (actionlog.log in the plugin folder)" : "actionlog entries no longer kept for problem reports (actionlog.log deleted)";
        return true;
    }
    if (arg == "clear") { g_ring.clear(); reply = "actionlog cleared, last seq " + std::to_string(g_seq); return true; }
    if (arg == "probe") { reply = Probe(); return true; }
    bool since = arg.compare(0, 5, "since") == 0;
    if (since || arg.compare(0, 4, "tail") == 0)
    {
        unsigned long long n = strtoull(arg.c_str() + (since ? 5 : 4), nullptr, 10);
        if (!since && n == 0) n = 20;
        reply = "seq " + std::to_string(g_seq) + (g_on ? "" : " (not recording: actionlog 1 or actionlog reports 1)") + "\n";
        size_t first = 0;
        if (since) { while (first < g_ring.size() && g_ring[first].seq <= n) ++first; }
        else first = g_ring.size() > n ? g_ring.size() - size_t(n) : 0;
        for (size_t i = first; i < g_ring.size(); ++i) reply += "#" + std::to_string(g_ring[i].seq) + " " + g_ring[i].text + "\n";
        return true;
    }
    reply = "actionlog [0|1|status|tail [n]|since <seq>|clear|probe|reports 0|1]";
    return true;
}
}
