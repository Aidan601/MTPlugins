// Camera movement presets, see campresets.h. A preset of our own is a code -> action map served from the second
// binding slot (30/31 only run there) by a detour of the lookup 0x1412F15D0. Radiant's own tables, CameraMode pref and
// Mouse Bindings are never written.

// MAP
//   addresses, action list ........ kLookup, kMainWin, offsets, kActions
//   Qt 5.3 exports ................ Qt, BindQt, QStr, TakeString
//   presets ....................... Preset, g_custom, ReadTable, BuiltIn, Describe, Load/Save
//   lookup detour ................. hk_Lookup
//   drop-down ..................... Attach, RebuildMenu, Activate, Deactivate, Follow, RefreshTips
//   editor window ................. ShowEditor and its On* handlers
//   lifecycle, command ............ Setup, Shutdown, Tick, Command
#include "modtools_plugin.hpp"
#include "campresets.h"
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace br::campresets
{
namespace
{
    // ---- Radiant (IDA addresses, Radiant_modtools.exe, no ASLR, resolved through host->RadiantAddress) ----
    const uintptr_t kLookup  = 0x1412F15D0;   // int (table*, Qt::MouseButton, Qt::KeyboardModifiers) -> action id, 0 = none
    const uintptr_t kMainWin = 0x1444C6560;   // RadiantMainWin*, +64 -> QE children, +240 -> CCamWnd*
    const size_t off_MainWin_Children = 64, off_Children_Cam = 240;
    const size_t off_Cam_Controller = 200;    // camera controller inside CCamWnd
    const size_t off_Ctl_Mode = 36;           // int: active mode index (0..4)
    const size_t off_Ctl_Tables = 48;         // five 40-byte binding tables
    const size_t kTableStride = 40;
    const int kSlot = 1;                      // the slot our presets are served from (30/31 only run there)

    struct BuiltInInfo { const char* label; const char* action; const char* table; };
    const BuiltInInfo kBuiltIn[5] = {
        { "Camera Movement Mode 1", "actionCamera_Movement_Mode_1", "CamMode1" },
        { "Camera Movement Mode 2", "actionCamera_Movement_Mode_2", "CamMode2" },
        { "Camera Movement Mode 3", "actionCamera_Movement_Mode_3", "CamMode3" },
        { "Maya Cam",               "actionMaya_Cam",               "MayaCamMode" },
        { "MotionBuilder Cam",      "actionMotionBuilder_Cam",      "MotionBuilderCamMode" },
    };

    // The camera actions the controller runs (0x140FAE7D0), Radiant's own names (0x1412F2BF0) with where each comes from
    struct ActionInfo { int id; const char* name; };
    const ActionInfo kActions[] = {
        { 30, "Rotate/Move Camera" },
        { 31, "Rotate/Move Camera (slow)" },
        { 32, "Move Camera Z" },
        { 33, "Move Camera Z (slow)" },
        { 34, "Move Camera Angles" },
        { 36, "Third Person Camera Turn" },
        { 37, "Drag Camera Position" },
        { 38, "Orbit (Maya)" },
        { 39, "Rotate (Maya)" },
        { 40, "Pan (Maya Drag)" },
        { 41, "Zoom (Maya)" },
        { 42, "Zoom (MotionBuilder)" },
        { 43, "Pan (MotionBuilder)" },
        { 44, "Orbit (MotionBuilder)" },
    };
    const int kActionCount = int(sizeof kActions / sizeof kActions[0]);

    std::string ActionName(int id)
    {
        for (const ActionInfo& a : kActions) if (a.id == id) return a.name;
        char b[32]; snprintf(b, sizeof b, "Action %d", id); return b;
    }
    int ActionIndex(int id) { for (int i = 0; i < kActionCount; ++i) if (kActions[i].id == id) return i; return -1; }

    // code: Left 8 / Right 16 / Middle 32 + Ctrl 1 + Alt 2 + Shift 4
    int ButtonOfCode(int code) { return code & 56; }
    std::string CodeText(int code)
    {
        std::string s;
        if (code & 1) s += "Ctrl + ";
        if (code & 2) s += "Alt + ";
        if (code & 4) s += "Shift + ";
        const int b = ButtonOfCode(code);
        s += b == 8 ? "Left drag" : b == 16 ? "Right drag" : b == 32 ? "Middle drag" : "?";
        return s;
    }
    // sort: by button (left, middle, right), then modifiers
    int CodeOrder(int code) { const int b = ButtonOfCode(code); return (b == 8 ? 0 : b == 32 ? 1 : 2) * 8 + (code & 7); }

    // ---- Qt 5.3 exports (Qt5Widgets64r / Qt5Core64r), no headers: MS x64 ABI, QString/QList returned through a hidden pointer ----
    struct QListDataD { int ref; int alloc; int begin; int end; void* array[1]; };
    struct QListRaw { QListDataD* d; };
    struct QArrayDataRaw { int ref; int size; unsigned alloc; ptrdiff_t offset; };
    struct QMetaObjectRaw;
    struct Qt
    {
        bool bound = false, tried = false;
        void** (*fromUtf8)(void**, const char*, int) = nullptr;
        void (*qsDtor)(void**) = nullptr;
        void (*dispose)(QListDataD*) = nullptr;
        const char* (*className)(const QMetaObjectRaw*) = nullptr;
        void (*deleteLater)(void*) = nullptr;
        void (*setToolTip)(void*, void**) = nullptr;
        void** (*toolTip)(const void*, void**) = nullptr;
        bool (*isChecked)(const void*) = nullptr;
        void (*setChecked)(void*, bool) = nullptr;
        void (*setCheckable)(void*, bool) = nullptr;
        void (*setSeparator)(void*, bool) = nullptr;
        void (*setVisible)(void*, bool) = nullptr;
        void* (*actionGroup)(const void*) = nullptr;
        void* (*groupAdd)(void*, void*) = nullptr;
        void (*activate)(void*, int) = nullptr;
        QListRaw* (*associatedWidgets)(const void*, QListRaw*) = nullptr;
        void (*insertAction)(void*, void*, void*) = nullptr;
        void (*removeAction)(void*, void*) = nullptr;
        void (*setToolTipsVisible)(void*, bool) = nullptr;
        void (*setObjectName)(void*, void**) = nullptr;
    } qt;

    template <class T> bool Get(T& fn, HMODULE m, const char* name) { fn = reinterpret_cast<T>(GetProcAddress(m, name)); return fn != nullptr; }

    bool BindQt()
    {
        if (qt.bound || qt.tried) return qt.bound;
        qt.tried = true;
        HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll"), c = GetModuleHandleW(L"Qt5Core64r.dll");
        if (!w || !c) return false;
        bool ok = true;
        ok &= Get(qt.fromUtf8, c, "?fromUtf8@QString@@SA?AV1@PEBDH@Z");
        ok &= Get(qt.qsDtor, c, "??1QString@@QEAA@XZ");
        ok &= Get(qt.dispose, c, "?dispose@QListData@@SAXPEAUData@1@@Z");
        ok &= Get(qt.className, c, "?className@QMetaObject@@QEBAPEBDXZ");
        ok &= Get(qt.deleteLater, c, "?deleteLater@QObject@@QEAAXXZ");
        ok &= Get(qt.setToolTip, w, "?setToolTip@QAction@@QEAAXAEBVQString@@@Z");
        ok &= Get(qt.toolTip, w, "?toolTip@QAction@@QEBA?AVQString@@XZ");
        ok &= Get(qt.isChecked, w, "?isChecked@QAction@@QEBA_NXZ");
        ok &= Get(qt.setChecked, w, "?setChecked@QAction@@QEAAX_N@Z");
        ok &= Get(qt.setCheckable, w, "?setCheckable@QAction@@QEAAX_N@Z");
        ok &= Get(qt.setSeparator, w, "?setSeparator@QAction@@QEAAX_N@Z");
        ok &= Get(qt.setVisible, w, "?setVisible@QAction@@QEAAX_N@Z");
        ok &= Get(qt.actionGroup, w, "?actionGroup@QAction@@QEBAPEAVQActionGroup@@XZ");
        ok &= Get(qt.groupAdd, w, "?addAction@QActionGroup@@QEAAPEAVQAction@@PEAV2@@Z");
        ok &= Get(qt.activate, w, "?activate@QAction@@QEAAXW4ActionEvent@1@@Z");
        ok &= Get(qt.associatedWidgets, w, "?associatedWidgets@QAction@@QEBA?AV?$QList@PEAVQWidget@@@@XZ");
        ok &= Get(qt.insertAction, w, "?insertAction@QWidget@@QEAAXPEAVQAction@@0@Z");
        ok &= Get(qt.removeAction, w, "?removeAction@QWidget@@QEAAXPEAVQAction@@@Z");
        Get(qt.setToolTipsVisible, w, "?setToolTipsVisible@QMenu@@QEAAX_N@Z");   // optional: no tooltips without it
        Get(qt.setObjectName, c, "?setObjectName@QObject@@QEAAXAEBVQString@@@Z");   // optional: names for `widgets` / `click`
        qt.bound = ok;
        if (!ok) rp::Log("campresets: Qt exports missing, no camera presets in the drop-down");
        return ok;
    }

    struct QStr
    {
        void* d = nullptr;
        explicit QStr(const std::string& s) { qt.fromUtf8(&d, s.c_str(), int(s.size())); }
        ~QStr() { qt.qsDtor(&d); }
        void** ptr() { return &d; }
    };
    std::string QStringToUtf8(void** qs)
    {
        QArrayDataRaw* d = static_cast<QArrayDataRaw*>(*qs);
        if (!d || d->size <= 0) return std::string();
        const wchar_t* w = reinterpret_cast<const wchar_t*>(reinterpret_cast<char*>(d) + d->offset);
        int n = WideCharToMultiByte(CP_UTF8, 0, w, d->size, nullptr, 0, nullptr, nullptr);
        std::string out(n > 0 ? n : 0, '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, d->size, &out[0], n, nullptr, nullptr);
        return out;
    }
    std::string TakeString(void** (*getter)(const void*, void**), const void* obj)
    {
        void* qs = nullptr; getter(obj, &qs);
        std::string s = QStringToUtf8(&qs);
        qt.qsDtor(&qs);
        return s;
    }
    void ListRelease(QListRaw& l)
    {
        if (!l.d || l.d->ref == -1) { l.d = nullptr; return; }
        if (InterlockedDecrement(reinterpret_cast<volatile long*>(&l.d->ref)) == 0) qt.dispose(l.d);
        l.d = nullptr;
    }
    const char* ClassName(void* obj)
    {
        auto metaObject = (*reinterpret_cast<const QMetaObjectRaw* (__fastcall ***)(void*)>(obj))[0];   // QObject vtable slot 0
        const char* c = qt.className(metaObject(obj));
        return c ? c : "";
    }

    // ---- presets ----
    struct Preset { std::string name; std::map<int, int> bind; };   // code -> action
    std::vector<Preset> g_custom;
    int g_savedCount = 0;          // preset keys written last time (to blank removed ones)

    uint8_t** g_mainWin = nullptr;
    uint8_t* g_ctl = nullptr;      // camera controller, CCamWnd+200
    bool g_tableOk = false;        // slot 1's table is named CamMode2 (the layout matches)

    uint8_t* Controller()
    {
        if (!g_mainWin || !*g_mainWin) return nullptr;
        uint8_t* children = *reinterpret_cast<uint8_t**>(*g_mainWin + off_MainWin_Children);
        uint8_t* cam = children ? *reinterpret_cast<uint8_t**>(children + off_Children_Cam) : nullptr;
        return cam ? cam + off_Cam_Controller : nullptr;
    }
    uint8_t* Table(int i) { return g_ctl ? g_ctl + off_Ctl_Tables + kTableStride * size_t(i) : nullptr; }
    int ModeIndex() { return g_ctl ? *reinterpret_cast<int*>(g_ctl + off_Ctl_Mode) : -1; }

    // Radiant's table: +0 QString name, +8 QHash<int code, int action> (QHashData: +8 buckets, +20 size, +32 numBuckets,
    // node: +0 next, +12 key, +16 value, a chain ends at the QHashData itself). Read only, main thread.
    bool ReadTable(int i, std::map<int, int>& out, std::string* name = nullptr)
    {
        out.clear();
        uint8_t* t = Table(i);
        if (!t) return false;
        if (name) *name = QStringToUtf8(reinterpret_cast<void**>(t));
        uint8_t* d = *reinterpret_cast<uint8_t**>(t + 8);
        if (!d) return false;
        const int size = *reinterpret_cast<int*>(d + 20);
        const unsigned buckets = *reinterpret_cast<unsigned*>(d + 32);
        if (size < 0 || size > 512 || buckets > 65536) return false;
        if (size == 0 || buckets == 0) return true;
        uint8_t** b = *reinterpret_cast<uint8_t***>(d + 8);
        if (!b) return false;
        int seen = 0;
        for (unsigned k = 0; k < buckets && seen <= size; ++k)
            for (uint8_t* n = b[k]; n && n != d && seen <= size; n = *reinterpret_cast<uint8_t**>(n), ++seen)
            {
                const int code = *reinterpret_cast<int*>(n + 12), action = *reinterpret_cast<int*>(n + 16);
                if (code >= 0 && code < 64 && ButtonOfCode(code) && action > 1) out[code] = action;   // each table also holds a button-less entry
            }
        return seen == size;
    }

    // the stock defaults (0x140FAD430), shown when the live table cannot be read
    void Defaults(int i, std::map<int, int>& m)
    {
        m.clear();
        switch (i)
        {
        case 0: case 1: m = { { 16, 30 }, { 17, 32 }, { 21, 34 } }; break;
        case 2: m = { { 16, 36 }, { 17, 37 } }; break;
        case 3: m = { { 10, 38 }, { 22, 39 }, { 34, 40 }, { 18, 41 } }; break;
        case 4: m = { { 9, 42 }, { 12, 43 }, { 13, 44 } }; break;
        }
    }
    void BuiltIn(int i, std::map<int, int>& m) { if (!ReadTable(i, m)) Defaults(i, m); }

    std::string Describe(const std::map<int, int>& bind)
    {
        std::vector<std::pair<int, int>> rows(bind.begin(), bind.end());
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return CodeOrder(a.first) < CodeOrder(b.first); });
        std::string s;
        for (const auto& r : rows) { if (!s.empty()) s += "\n"; s += CodeText(r.first) + ": " + ActionName(r.second); }
        return s.empty() ? "(no mouse bindings)" : s;
    }

    std::string Clean(std::string s)
    {
        for (char& c : s) if (c == '|' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == ':') c = ' ';
        while (!s.empty() && s.back() == ' ') s.pop_back();
        while (!s.empty() && s.front() == ' ') s.erase(s.begin());
        return s;
    }

    // [camera_presets] count=N, preset1=Name|16:30,17:32, active=Name
    void Load()
    {
        g_custom.clear();
        const int n = rp::ConfigGetInt("camera_presets", "count", 0);
        for (int i = 1; i <= n && i <= 200; ++i)
        {
            char key[32]; snprintf(key, sizeof key, "preset%d", i);
            const std::string v = rp::ConfigGet("camera_presets", key, "");
            const size_t bar = v.find('|');
            Preset p; p.name = Clean(v.substr(0, bar));
            if (p.name.empty()) continue;
            if (bar != std::string::npos)
            {
                const char* s = v.c_str() + bar + 1;
                while (*s)
                {
                    char* end = nullptr;
                    const long code = strtol(s, &end, 10);
                    if (end == s || *end != ':') break;
                    const long action = strtol(end + 1, &end, 10);
                    if (code >= 0 && code < 64 && ButtonOfCode(int(code)) && ActionIndex(int(action)) >= 0) p.bind[int(code)] = int(action);
                    s = end; while (*s == ',' || *s == ' ') ++s;
                }
            }
            g_custom.push_back(p);
        }
        g_savedCount = n;
    }
    void Save(const std::string& active)
    {
        int i = 0;
        for (const Preset& p : g_custom)
        {
            std::string v = p.name + "|";
            bool first = true;
            for (const auto& b : p.bind) { char e[24]; snprintf(e, sizeof e, "%s%d:%d", first ? "" : ",", b.first, b.second); v += e; first = false; }
            char key[32]; snprintf(key, sizeof key, "preset%d", ++i);
            rp::ConfigSet("camera_presets", key, v);
        }
        for (int k = i + 1; k <= g_savedCount; ++k) { char key[32]; snprintf(key, sizeof key, "preset%d", k); rp::ConfigSet("camera_presets", key, ""); }
        g_savedCount = i;
        rp::ConfigSet("camera_presets", "count", std::to_string(i));
        rp::ConfigSet("camera_presets", "active", active);
    }
    int FindCustom(const std::string& name)
    {
        for (size_t i = 0; i < g_custom.size(); ++i) if (_stricmp(g_custom[i].name.c_str(), name.c_str()) == 0) return int(i);
        return -1;
    }

    // ---- the lookup detour ----
    using Lookup_t = int(__fastcall*)(void* table, int button, int modifiers);
    Lookup_t g_origLookup = nullptr;
    int g_active = -1;             // index into g_custom of the preset in use, -1 = Radiant's own
    int g_live[64] = {};           // code -> action of the preset in use
    uint64_t g_hits = 0;

    void SetLive(int index)
    {
        memset(g_live, 0, sizeof g_live);
        if (index >= 0 && size_t(index) < g_custom.size()) for (const auto& b : g_custom[size_t(index)].bind) g_live[b.first & 63] = b.second;
    }

    int __fastcall hk_Lookup(void* table, int button, int modifiers)
    {
        if (g_active < 0 || !g_tableOk || table != Table(kSlot) || ModeIndex() != kSlot) return g_origLookup(table, button, modifiers);
        int code = button == 1 ? 8 : button == 2 ? 16 : button == 4 ? 32 : 0;   // as Radiant: one button exactly
        if (!code) return 0;
        if (modifiers & 0x4000000) code += 1;
        if (modifiers & 0x8000000) code += 2;
        if (modifiers & 0x2000000) code += 4;
        ++g_hits;
        return g_live[code & 63];
    }

    // ---- the drop-down ----
    void* g_stock[5] = {};               // Radiant's five QActions
    std::vector<void*> g_menus;          // every QMenu holding them
    void* g_group = nullptr;             // their QActionGroup (exclusive), if any
    void* g_sep1 = nullptr;              // separator above our presets
    void* g_sep2 = nullptr;              // separator above "Edit Camera Presets..."
    void* g_editAction = nullptr;
    std::vector<void*> g_presetActions;  // pool, one per custom preset (hidden when unused)
    int g_slotIds[200];                  // user data of the pool actions
    bool g_attached = false, g_startDone = false;
    uint64_t g_lastTips = 0;
    std::string g_tips[5];               // tooltip last set on each stock action

    void ShowEditor();
    void Activate(int index, bool userClick);

    void OnPresetAction(void* user)
    {
        const int i = *static_cast<int*>(user);
        if (i >= 0 && size_t(i) < g_custom.size()) Activate(i, true);
    }
    void OnEditAction(void*) { ShowEditor(); }

    void MenuInsert(void* action, void* before) { for (void* m : g_menus) { qt.removeAction(m, action); qt.insertAction(m, before, action); } }

    void* NewAction(const char* text, ModToolsCallback fn, void* user)
    {
        if (g_menus.empty()) return nullptr;
        void* a = rp::host->AddMenuItem(rp::self, g_menus[0], text, fn, user);
        return a;
    }

    void RebuildMenu()
    {
        if (!g_attached) return;
        const auto* ui = rp::host->ui;
        while (g_presetActions.size() < g_custom.size() && g_presetActions.size() < 200)
        {
            const size_t i = g_presetActions.size();
            g_slotIds[i] = int(i);
            void* a = NewAction("preset", OnPresetAction, &g_slotIds[i]);
            if (!a) break;
            qt.setCheckable(a, true);
            if (g_group) qt.groupAdd(g_group, a);
            MenuInsert(a, g_sep2);
            g_presetActions.push_back(a);
        }
        for (size_t i = 0; i < g_presetActions.size(); ++i)
        {
            void* a = g_presetActions[i];
            const bool used = i < g_custom.size();
            qt.setVisible(a, used);
            if (!used) { qt.setChecked(a, false); continue; }
            ui->SetActionText(a, g_custom[i].name.c_str());
            QStr tip(g_custom[i].name + " (your preset)\n" + Describe(g_custom[i].bind));
            qt.setToolTip(a, tip.ptr());
            qt.setChecked(a, int(i) == g_active);
        }
        qt.setVisible(g_sep1, !g_custom.empty());
    }

    void RefreshTips(bool force)
    {
        if (!g_attached) return;
        const uint64_t now = GetTickCount64();
        if (!force && now - g_lastTips < 2000) return;
        g_lastTips = now;
        for (int i = 0; i < 5; ++i)
        {
            if (!g_stock[i]) continue;
            std::map<int, int> m; BuiltIn(i, m);
            std::string t = std::string(kBuiltIn[i].label) + "\n" + Describe(m);
            if (i == kSlot) t += "\n(your own presets below run from this slot)";
            if (t == g_tips[i]) continue;
            g_tips[i] = t;
            QStr q(t);
            qt.setToolTip(g_stock[i], q.ptr());
        }
    }

    bool Attach()
    {
        if (g_attached) return true;
        const auto* ui = rp::host->ui;
        if (!ui || !ui->FindEditorWidget || !ui->SetActionText || !BindQt()) return false;
        g_ctl = Controller();
        if (!g_ctl) return false;
        for (int i = 0; i < 5; ++i) if (!(g_stock[i] = ui->FindEditorWidget(kBuiltIn[i].action))) return false;
        std::string name;
        std::map<int, int> tmp;
        g_tableOk = ReadTable(kSlot, tmp, &name) && name == kBuiltIn[kSlot].table;
        // every QMenu the mode entries are in (the toolbar button's drop-down and View > Camera > Camera Movement Mode)
        g_menus.clear();
        QListRaw l = {}; qt.associatedWidgets(g_stock[4], &l);
        if (l.d) for (int j = l.d->begin; j < l.d->end; ++j) { void* w = l.d->array[j]; if (w && strcmp(ClassName(w), "QMenu") == 0) g_menus.push_back(w); }
        ListRelease(l);
        if (g_menus.empty()) { rp::Log("campresets: the Camera Movement Mode menu was not found"); g_attached = true; return true; }
        g_group = qt.actionGroup(g_stock[0]);
        for (void* m : g_menus) if (qt.setToolTipsVisible) qt.setToolTipsVisible(m, true);
        g_sep1 = NewAction("", nullptr, nullptr);
        g_sep2 = NewAction("", nullptr, nullptr);
        g_editAction = NewAction("Edit Camera Presets...", OnEditAction, nullptr);
        if (!g_sep1 || !g_sep2 || !g_editAction) { rp::Log("campresets: could not add the menu entries"); g_attached = true; return true; }
        qt.setSeparator(g_sep1, true);
        qt.setSeparator(g_sep2, true);
        for (void* a : { g_sep1, g_sep2, g_editAction }) MenuInsert(a, nullptr);   // appended, in this order
        QStr tip("View the built-in camera presets and make your own");
        qt.setToolTip(g_editAction, tip.ptr());
        g_attached = true;
        RebuildMenu();
        RefreshTips(true);
        rp::Log("campresets: attached (%zu menus, group %s, slot table %s, mode %d, lookup detour %s, %zu presets)", g_menus.size(),
                g_group ? "yes" : "no", g_tableOk ? "CamMode2" : ("UNEXPECTED '" + name + "'").c_str(), ModeIndex(),
                g_origLookup ? "ok" : "FAILED", g_custom.size());
        return true;
    }

    void Deactivate(bool save)
    {
        if (g_active < 0) return;
        if (size_t(g_active) < g_presetActions.size()) qt.setChecked(g_presetActions[size_t(g_active)], false);
        rp::Log("campresets: back to Radiant's %s", ModeIndex() >= 0 && ModeIndex() < 5 ? kBuiltIn[ModeIndex()].label : "mode");
        g_active = -1;
        SetLive(-1);
        if (save) Save("");
    }

    // userClick: the entry was clicked (Qt has already checked it), false at start-up and from the editor/command
    void Activate(int index, bool userClick)
    {
        if (index < 0 || size_t(index) >= g_custom.size() || !g_attached || !g_tableOk || !g_origLookup) return;
        if (ModeIndex() != kSlot) qt.activate(g_stock[kSlot], 0);   // Radiant's own switch to its second slot (QAction::Trigger)
        if (ModeIndex() != kSlot) { rp::Log("campresets: Radiant did not switch to Camera Movement Mode 2, preset not used"); return; }
        g_active = index;
        SetLive(index);
        for (int i = 0; i < 5; ++i) qt.setChecked(g_stock[i], false);
        for (size_t i = 0; i < g_presetActions.size(); ++i) qt.setChecked(g_presetActions[i], int(i) == index);
        Save(g_custom[size_t(index)].name);
        rp::Log("campresets: using '%s'%s", g_custom[size_t(index)].name.c_str(), userClick ? "" : " (not from the menu)");
    }

    void UseBuiltIn(int i)
    {
        if (i < 0 || i >= 5 || !g_attached) return;
        Deactivate(true);
        qt.activate(g_stock[i], 0);
    }

    // a built-in entry was picked (or Radiant changed mode some other way): give the mouse back to Radiant's tables
    void Follow()
    {
        if (g_active < 0) return;
        bool stockChecked = false;
        for (int i = 0; i < 5; ++i) if (qt.isChecked(g_stock[i])) stockChecked = true;
        const bool oursChecked = size_t(g_active) < g_presetActions.size() && qt.isChecked(g_presetActions[size_t(g_active)]);
        if (stockChecked || !oursChecked || ModeIndex() != kSlot) Deactivate(true);
    }

    // ---- editor window ----
    void* g_dlg = nullptr;
    void *g_list = nullptr, *g_name = nullptr, *g_tree = nullptr, *g_note = nullptr, *g_status = nullptr;
    void *g_btnCombo = nullptr, *g_ctrl = nullptr, *g_alt = nullptr, *g_shift = nullptr, *g_actCombo = nullptr;
    void *g_add = nullptr, *g_remove = nullptr, *g_new = nullptr, *g_dup = nullptr, *g_del = nullptr, *g_use = nullptr;
    int g_sel = 0;                   // row: 0..4 built-in, 5.. custom
    bool g_filling = false;

    bool SelCustom() { return g_sel >= 5 && size_t(g_sel - 5) < g_custom.size(); }
    std::map<int, int> SelBindings()
    {
        std::map<int, int> m;
        if (g_sel >= 0 && g_sel < 5) BuiltIn(g_sel, m);
        else if (SelCustom()) m = g_custom[size_t(g_sel - 5)].bind;
        return m;
    }
    std::string SelName() { return g_sel >= 0 && g_sel < 5 ? kBuiltIn[g_sel].label : SelCustom() ? g_custom[size_t(g_sel - 5)].name : ""; }

    void FillStatus()
    {
        const auto* ui = rp::host->ui;
        const int mode = ModeIndex();
        std::string s = "In use: ";
        s += g_active >= 0 ? g_custom[size_t(g_active)].name : (mode >= 0 && mode < 5 ? kBuiltIn[mode].label : "?");
        ui->SetLabelText(g_status, s.c_str());
    }

    void FillTree()
    {
        const auto* ui = rp::host->ui;
        ui->TreeClear(g_tree);
        std::map<int, int> m = SelBindings();
        std::vector<std::pair<int, int>> rows(m.begin(), m.end());
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return CodeOrder(a.first) < CodeOrder(b.first); });
        for (const auto& r : rows)
            if (void* it = ui->TreeAdd(g_tree, nullptr, (CodeText(r.first) + "   ->   " + ActionName(r.second)).c_str())) ui->TreeSetItemId(it, r.first + 1);
    }

    void FillRight()
    {
        const auto* ui = rp::host->ui;
        g_filling = true;
        const bool custom = SelCustom();
        ui->SetEditText(g_name, SelName().c_str());
        ui->SetEnabled(g_name, custom);
        for (void* w : { g_btnCombo, g_ctrl, g_alt, g_shift, g_actCombo, g_add, g_remove, g_del }) ui->SetEnabled(w, custom);
        ui->SetEnabled(g_dup, g_sel >= 0);
        ui->SetLabelText(g_note, custom
            ? "Pick a mouse button, modifier keys and an action, then Add / Replace. Changes are saved at once."
            : "A built-in Radiant preset (read from Radiant; Settings > Mouse Bindings edits these). Duplicate it to make your own.");
        FillTree();
        FillStatus();
        g_filling = false;
    }

    void FillList(int select)
    {
        const auto* ui = rp::host->ui;
        g_filling = true;
        ui->ListClear(g_list);
        for (int i = 0; i < 5; ++i) ui->ListAdd(g_list, (std::string(kBuiltIn[i].label) + "  (built-in)").c_str());
        for (const Preset& p : g_custom) ui->ListAdd(g_list, p.name.c_str());
        const int count = 5 + int(g_custom.size());
        g_sel = select < 0 ? 0 : select >= count ? count - 1 : select;
        ui->ListSetCurrentRow(g_list, g_sel);
        g_filling = false;
        FillRight();
    }

    // after any change to g_custom: ini, menu, the live map of the preset in use
    void Changed()
    {
        Save(g_active >= 0 ? g_custom[size_t(g_active)].name : "");
        if (g_active >= 0) SetLive(g_active);
        RebuildMenu();
    }

    std::string UniqueName(const std::string& base)
    {
        std::string n = base;
        for (int k = 2; FindCustom(n) >= 0 || n.empty(); ++k) n = base + " " + std::to_string(k);
        return n;
    }

    void OnListRow(void*, int row) { if (g_filling || row < 0) return; g_sel = row; FillRight(); }

    void OnNew(void*)
    {
        Preset p; p.name = UniqueName("My Preset");
        g_custom.push_back(p);
        Changed();
        FillList(4 + int(g_custom.size()));
    }
    void OnDuplicate(void*)
    {
        Preset p; p.name = UniqueName(SelName() + " copy"); p.bind = SelBindings();
        g_custom.push_back(p);
        Changed();
        FillList(4 + int(g_custom.size()));
    }
    void OnDelete(void*)
    {
        if (!SelCustom()) return;
        const int i = g_sel - 5;
        if (g_active == i) Deactivate(false);
        else if (g_active > i) --g_active;
        g_custom.erase(g_custom.begin() + i);
        Changed();
        // the pool keeps its order: menu entry i now shows the next preset, its check mark follows g_active
        FillList(g_sel);
    }
    void OnRename(void*, int)
    {
        if (g_filling || !SelCustom()) return;
        std::string n = Clean(rp::EditText(g_name));
        Preset& p = g_custom[size_t(g_sel - 5)];
        if (n.empty() || n == p.name) { rp::host->ui->SetEditText(g_name, p.name.c_str()); return; }
        const int other = FindCustom(n);
        if (other >= 0 && other != g_sel - 5) n = UniqueName(n);
        p.name = n;
        Changed();
        FillList(g_sel);
    }
    int EditorCode()
    {
        const auto* ui = rp::host->ui;
        const int b = ui->ComboCurrent(g_btnCombo);
        int code = b == 0 ? 8 : b == 1 ? 32 : 16;
        if (ui->IsChecked(g_ctrl)) code += 1;
        if (ui->IsChecked(g_alt)) code += 2;
        if (ui->IsChecked(g_shift)) code += 4;
        return code;
    }
    void OnAdd(void*)
    {
        if (!SelCustom()) return;
        const int a = rp::host->ui->ComboCurrent(g_actCombo);
        if (a < 0 || a >= kActionCount) return;
        g_custom[size_t(g_sel - 5)].bind[EditorCode()] = kActions[a].id;
        Changed();
        FillTree();
    }
    void OnRemove(void*)
    {
        if (!SelCustom()) return;
        auto& bind = g_custom[size_t(g_sel - 5)].bind;
        void* it = rp::host->ui->TreeCurrent(g_tree);
        const int code = it ? int(rp::host->ui->TreeItemId(it)) - 1 : EditorCode();
        if (!bind.erase(code)) return;
        Changed();
        FillTree();
    }
    // a binding row picked: load it into the controls below
    void OnTreeRow(void*, int what)
    {
        if (g_filling || what != 0) return;
        const auto* ui = rp::host->ui;
        void* it = ui->TreeCurrent(g_tree);
        if (!it) return;
        const int code = int(ui->TreeItemId(it)) - 1;
        if (code < 0) return;
        const std::map<int, int> m = SelBindings();
        const auto f = m.find(code);
        g_filling = true;
        const int b = ButtonOfCode(code);
        ui->ComboSetCurrent(g_btnCombo, b == 8 ? 0 : b == 32 ? 1 : 2);
        ui->SetChecked(g_ctrl, (code & 1) != 0);
        ui->SetChecked(g_alt, (code & 2) != 0);
        ui->SetChecked(g_shift, (code & 4) != 0);
        if (f != m.end() && ActionIndex(f->second) >= 0) ui->ComboSetCurrent(g_actCombo, ActionIndex(f->second));
        g_filling = false;
    }
    void OnUse(void*)
    {
        if (g_sel >= 0 && g_sel < 5) UseBuiltIn(g_sel);
        else if (SelCustom()) Activate(g_sel - 5, false);
        FillStatus();
    }

    void ShowEditor()
    {
        const auto* ui = rp::host->ui;
        if (!ui || !rp::Has(&rp::host->CreateDialogWindow) || !rp::Has(&rp::host->OnChanged) || !rp::HasUi(&ui->TreeWidget)) return;
        if (!g_dlg)
        {
            void* content = ui->Widget(nullptr);
            void* outer = ui->HBox();
            ui->LayoutMargins(outer, 10, 10, 10, 10);
            ui->LayoutSpacing(outer, 10);

            void* left = ui->VBox(nullptr);
            ui->BoxAddWidget(left, ui->Label("Presets", content), 0);
            g_list = ui->ListWidget(content);
            ui->SetMinimumSize(g_list, 230, 200);
            ui->BoxAddWidget(left, g_list, 1);
            void* lb = ui->HBox();
            g_new = ui->PushButton("New", content);
            g_dup = ui->PushButton("Duplicate", content);
            g_del = ui->PushButton("Delete", content);
            ui->BoxAddWidget(lb, g_new, 0); ui->BoxAddWidget(lb, g_dup, 0); ui->BoxAddWidget(lb, g_del, 0);
            ui->BoxAddLayout(left, lb, 0);

            void* right = ui->VBox(nullptr);
            void* nameRow = ui->HBox();
            ui->BoxAddWidget(nameRow, ui->Label("Name:", content), 0);
            g_name = ui->LineEdit("", content);
            ui->BoxAddWidget(nameRow, g_name, 1);
            ui->BoxAddLayout(right, nameRow, 0);
            g_tree = ui->TreeWidget(1, content);
            ui->SetMinimumSize(g_tree, 380, 180);
            ui->BoxAddWidget(right, g_tree, 1);

            void* edit = ui->HBox();
            g_btnCombo = ui->ComboBox(content);
            ui->ComboAdd(g_btnCombo, "Left drag"); ui->ComboAdd(g_btnCombo, "Middle drag"); ui->ComboAdd(g_btnCombo, "Right drag");
            ui->ComboSetCurrent(g_btnCombo, 2);
            g_ctrl = ui->CheckBox("Ctrl", content);
            g_alt = ui->CheckBox("Alt", content);
            g_shift = ui->CheckBox("Shift", content);
            g_actCombo = ui->ComboBox(content);
            for (const ActionInfo& a : kActions) ui->ComboAdd(g_actCombo, a.name);
            ui->BoxAddWidget(edit, g_ctrl, 0); ui->BoxAddWidget(edit, g_alt, 0); ui->BoxAddWidget(edit, g_shift, 0);
            ui->BoxAddWidget(edit, g_btnCombo, 0);
            ui->BoxAddWidget(edit, ui->Label("->", content), 0);
            ui->BoxAddWidget(edit, g_actCombo, 1);
            ui->BoxAddLayout(right, edit, 0);
            void* eb = ui->HBox();
            g_add = ui->PushButton("Add / Replace", content);
            g_remove = ui->PushButton("Remove Binding", content);
            ui->BoxAddWidget(eb, g_add, 0); ui->BoxAddWidget(eb, g_remove, 0); ui->BoxAddStretch(eb, 1);
            ui->BoxAddLayout(right, eb, 0);
            g_note = ui->Label("", content);
            ui->SetWordWrap(g_note, 1);
            ui->BoxAddWidget(right, g_note, 0);
            void* bottom = ui->HBox();
            g_status = ui->Label("", content);
            g_use = ui->PushButton("Use This Preset", content);
            ui->BoxAddWidget(bottom, g_status, 1); ui->BoxAddWidget(bottom, g_use, 0);
            ui->BoxAddLayout(right, bottom, 0);

            ui->BoxAddLayout(outer, left, 0);
            ui->BoxAddLayout(outer, right, 1);
            ui->SetLayout(content, outer);
            g_dlg = rp::host->CreateDialogWindow(rp::self, "Camera Movement Presets", content, 720, 420);
            // object names, so the host's `widgets` and `click` commands can reach the window
            const std::pair<void*, const char*> names[] = { { content, "campresetEditor" }, { g_list, "campresetList" }, { g_name, "campresetName" },
                { g_tree, "campresetBindings" }, { g_btnCombo, "campresetButton" }, { g_ctrl, "campresetCtrl" }, { g_alt, "campresetAlt" },
                { g_shift, "campresetShift" }, { g_actCombo, "campresetAction" }, { g_add, "campresetAdd" }, { g_remove, "campresetRemove" },
                { g_new, "campresetNew" }, { g_dup, "campresetDuplicate" }, { g_del, "campresetDelete" }, { g_use, "campresetUse" },
                { g_status, "campresetStatus" }, { g_note, "campresetNote" } };
            if (qt.setObjectName) for (const auto& n : names) if (n.first) { QStr q(n.second); qt.setObjectName(n.first, q.ptr()); }
            rp::host->OnChanged(rp::self, g_list, OnListRow, nullptr);
            rp::host->OnChanged(rp::self, g_name, OnRename, nullptr);
            rp::host->OnChanged(rp::self, g_tree, OnTreeRow, nullptr);
            rp::host->OnClicked(rp::self, g_new, OnNew, nullptr);
            rp::host->OnClicked(rp::self, g_dup, OnDuplicate, nullptr);
            rp::host->OnClicked(rp::self, g_del, OnDelete, nullptr);
            rp::host->OnClicked(rp::self, g_add, OnAdd, nullptr);
            rp::host->OnClicked(rp::self, g_remove, OnRemove, nullptr);
            rp::host->OnClicked(rp::self, g_use, OnUse, nullptr);
        }
        if (!g_dlg) return;
        const int mode = ModeIndex();
        FillList(g_active >= 0 ? 5 + g_active : (mode >= 0 && mode < 5 ? mode : 0));
        ui->SetVisible(g_dlg, 1);
    }
}

void Setup()
{
    Load();
    g_mainWin = static_cast<uint8_t**>(rp::host->RadiantAddress(rp::self, kMainWin));
    void* lookup = rp::host->RadiantAddress(rp::self, kLookup);
    if (!lookup || !rp::host->Detour(rp::self, lookup, reinterpret_cast<void*>(&hk_Lookup), reinterpret_cast<void**>(&g_origLookup))) g_origLookup = nullptr;
    rp::Log("campresets: %zu presets in the ini, lookup detour %s", g_custom.size(), g_origLookup ? "ok" : "FAILED");
}

void Shutdown()
{
    const bool wasActive = g_active >= 0;
    g_active = -1;                  // the host removes the detour, the ini keeps the preset for the next load
    if (!g_attached || !qt.bound) return;
    if (wasActive && g_stock[kSlot]) qt.setChecked(g_stock[kSlot], true);   // Radiant is in its second slot: show that
    std::vector<void*> ours = g_presetActions;
    for (void* a : { g_sep1, g_sep2, g_editAction }) if (a) ours.push_back(a);
    for (void* a : ours) { for (void* m : g_menus) qt.removeAction(m, a); qt.deleteLater(a); }
    g_presetActions.clear();
    g_sep1 = g_sep2 = g_editAction = nullptr;
    g_attached = false;
    g_dlg = nullptr;                // the host destroys the window
}

void Tick()
{
    if (!g_attached)
    {
        static uint64_t lastTry = 0;
        const uint64_t now = GetTickCount64();
        if (now - lastTry < 500) return;
        lastTry = now;
        if (!Attach()) return;
    }
    if (!g_menus.empty() && g_sep2)
    {
        if (!g_startDone)
        {
            g_startDone = true;
            const std::string want = rp::ConfigGet("camera_presets", "active", "");
            const int i = want.empty() ? -1 : FindCustom(want);
            if (i >= 0 && ModeIndex() == kSlot) Activate(i, false);
            else if (i >= 0) { rp::Log("campresets: '%s' was in use, but Radiant starts in another mode; not used", want.c_str()); Save(""); }
        }
        Follow();
    }
    RefreshTips(false);
}

bool Command(const std::string& command, std::string& reply)
{
    if (command.compare(0, 9, "campreset") != 0) return false;
    const std::string arg = command.size() > 10 ? command.substr(10) : "";
    if (arg == "show") { ShowEditor(); reply = "editor shown\n"; return true; }
    if (arg.compare(0, 5, "show ") == 0)
    {
        ShowEditor();
        if (!g_dlg) { reply = "no editor\n"; return true; }
        const std::string n = arg.substr(5);
        int row = -1;
        for (int i = 0; i < 5; ++i) if (_stricmp(n.c_str(), kBuiltIn[i].label) == 0) row = i;
        if (row < 0 && FindCustom(n) >= 0) row = 5 + FindCustom(n);
        if (row < 0) { reply = "no preset named '" + n + "'\n"; return true; }
        FillList(row);
        reply = "editor on '" + SelName() + "'\n";
        return true;
    }
    if (arg == "probe")   // Radiant's own lookup entry (through the detour) for every code, slot 1 and slot 0
    {
        auto entry = reinterpret_cast<Lookup_t>(rp::host->RadiantAddress(rp::self, kLookup));
        if (!entry || !g_attached) { reply = "not attached\n"; return true; }
        const int buttons[3] = { 1, 4, 2 };
        reply = "code: slot1 (served) / slot0 (Radiant's)\n";
        for (int b : buttons)
            for (int m = 0; m < 8; ++m)
            {
                const int mods = (m & 1 ? 0x4000000 : 0) | (m & 2 ? 0x8000000 : 0) | (m & 4 ? 0x2000000 : 0);
                const int a1 = entry(Table(kSlot), b, mods), a0 = entry(Table(0), b, mods);
                if (!a1 && !a0) continue;
                const int code = (b == 1 ? 8 : b == 2 ? 16 : 32) + m;
                reply += CodeText(code) + ": " + (a1 ? ActionName(a1) : "-") + " / " + (a0 ? ActionName(a0) : "-") + "\n";
            }
        return true;
    }
    if (arg.compare(0, 4, "use ") == 0)
    {
        const std::string n = arg.substr(4);
        for (int i = 0; i < 5; ++i) if (_stricmp(n.c_str(), kBuiltIn[i].label) == 0 || n == std::to_string(i + 1)) { UseBuiltIn(i); reply = "using built-in\n"; return true; }
        const int i = FindCustom(n);
        if (i < 0) { reply = "no preset named '" + n + "'\n"; return true; }
        Activate(i, false);
        reply = g_active == i ? "using '" + n + "'\n" : "could not use it (see the log)\n";
        return true;
    }
    char buf[512];
    snprintf(buf, sizeof buf, "campreset: attached %d, menus %zu, group %s, mode %d, slot table %s, lookup detour %s, in use: %s, lookups answered %llu\n",
             g_attached ? 1 : 0, g_menus.size(), g_group ? "yes" : "no", ModeIndex(), g_tableOk ? "ok" : "unchecked",
             g_origLookup ? "ok" : "missing", g_active >= 0 ? g_custom[size_t(g_active)].name.c_str() : "Radiant's own", (unsigned long long)g_hits);
    reply = buf;
    for (int i = 0; i < 5; ++i)
    {
        std::map<int, int> m;
        const bool live = ReadTable(i, m);
        if (!live) Defaults(i, m);
        reply += std::string("[") + kBuiltIn[i].label + (live ? "" : ", defaults") + "]\n" + Describe(m) + "\n";
    }
    for (const Preset& p : g_custom) reply += "[" + p.name + "]\n" + Describe(p.bind) + "\n";
    return true;
}
}
