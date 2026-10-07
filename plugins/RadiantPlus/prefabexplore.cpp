// Prefab Browser "Show in Explorer": appends a separator and the entry to the browser's right-click QMenu. The slot
// runs inside the menu's exec while the toolbox still holds the clicked item, resolves it under map_source\_prefabs
// and opens Explorer there.
#include "modtools_plugin.hpp"
#include "prefabexplore.h"
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace br::prefabexplore
{
namespace
{
    // IDA addresses (no ASLR), resolved through host->RadiantAddress
    const uintptr_t kPrefabBrowserVtbl = 0x142548668;   // QtPrefabBrowser's vtable (COL ??_R4QtPrefabBrowser@@6B@ at -8)

    // QtAbstractToolbox fields: OpenContextMenu(QStandardItem*, bool) 0x14081C0E0 (vtable +496) stores the item at +544,
    // execs the menu at +536, then clears +544
    const size_t off_Box_Model    = 424;  // QStandardItemModel* (source)
    const size_t off_Box_RootPath = 624;  // QString: the folder the prefab tree is scanned from (QtPrefabBrowser 0x14081FB30)
    const size_t off_Box_Menu     = 536;  // QMenu*: the item context menu
    const size_t off_Box_MenuItem = 544;  // QStandardItem*: the right-clicked item, only while the menu is open
    const int    kNameRole        = 256;  // Qt::UserRole: the name a double-click places

    struct QListDataD { int ref; int alloc; int begin; int end; void* array[1]; };
    struct QListRaw { QListDataD* d; };
    struct QArrayDataRaw { int ref; int size; unsigned alloc; ptrdiff_t offset; };
    struct QVariantRaw { uint64_t data, type; };

    using AllWidgetsFn    = QListRaw* (*)(QListRaw* ret);
    using ListDisposeFn   = void (*)(QListDataD* d);
    using ActionsFn       = QListRaw* (*)(const void* widget, QListRaw* ret);
    using RemoveActionFn  = void (*)(void* widget, void* action);
    using AddSeparatorFn  = void* (*)(void* menu);
    using DeleteLaterFn   = void (*)(void* obj);
    using ParentItemFn    = void* (*)(const void* item);
    using RootItemFn      = void* (*)(const void* model);
    using ChildFn         = void* (*)(const void* item, int row, int col);
    using RowCountFn      = int (*)(const void* item);
    using ItemDataFn      = QVariantRaw* (*)(const void* item, QVariantRaw* ret, int role);   // QStandardItem vtable +8
    using ToStringFn      = void** (*)(const void* variant, void** retQString);
    using VariantDtorFn   = void (*)(void* variant);
    using StringDtorFn    = void (*)(void** s);
    using MetacastFn      = void* (*)(void* obj, const char* cls);                             // QObject vtable slot 1

    struct Qt
    {
        AllWidgetsFn allWidgets; ListDisposeFn dispose; ActionsFn actions; RemoveActionFn removeAction;
        AddSeparatorFn addSeparator; DeleteLaterFn deleteLater; ParentItemFn parent; RootItemFn rootItem; ChildFn child;
        RowCountFn rowCount; ToStringFn toString; VariantDtorFn variantDtor; StringDtorFn stringDtor;
    } qt = {};

    bool  g_enabled = true;             // [prefabs] show_in_explorer
    bool  g_ready = false;
    void* g_vtbl = nullptr;

    char* g_box = nullptr;              // the QtPrefabBrowser
    void* g_menu = nullptr;             // its context menu, holding our two actions
    void* g_sep = nullptr;
    void* g_action = nullptr;
    uint64_t g_nextScanMs = 0;
    unsigned g_shown = 0;
    std::string g_last;                 // the last path opened, or why nothing was

    std::string Utf8(const wchar_t* w, int n)
    {
        int len = n > 0 ? WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr) : 0;
        std::string out(len > 0 ? len : 0, '\0');
        if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w, n, &out[0], len, nullptr, nullptr);
        return out;
    }
    std::string Utf8(const std::wstring& w) { return Utf8(w.c_str(), int(w.size())); }
    std::wstring Wide(const std::string& s)
    {
        int len = s.empty() ? 0 : MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
        std::wstring out(len > 0 ? len : 0, L'\0');
        if (len > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), &out[0], len);
        return out;
    }
    template <class T> bool Get(T& fn, HMODULE m, const char* name)
    {
        fn = m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
        return fn != nullptr;
    }
    template <class T> T At(uintptr_t ida) { return reinterpret_cast<T>(rp::host->RadiantAddress(rp::self, ida)); }

    std::string QStringText(const void* d)
    {
        const QArrayDataRaw* a = static_cast<const QArrayDataRaw*>(d);
        if (!a || a->size <= 0 || a->size > 32768) return std::string();
        return Utf8(reinterpret_cast<const wchar_t*>(reinterpret_cast<const char*>(a) + a->offset), a->size);
    }
    std::string TakeQString(void*& qs)
    {
        std::string s = QStringText(qs);
        qt.stringDtor(&qs);
        return s;
    }
    void ListRelease(QListRaw& l)
    {
        if (!l.d || l.d->ref == -1) return;
        if (InterlockedDecrement(reinterpret_cast<volatile long*>(&l.d->ref)) == 0 && qt.dispose) qt.dispose(l.d);
        l.d = nullptr;
    }
    void* Metacast(void* obj, const char* cls)
    {
        return obj ? (*reinterpret_cast<MetacastFn**>(obj))[1](obj, cls) : nullptr;
    }
    std::string ItemData(const void* item, int role)
    {
        QVariantRaw v = {};
        (*reinterpret_cast<ItemDataFn* const*>(item))[1](item, &v, role);
        void* qs = nullptr;
        qt.toString(&v, &qs);
        std::string s = TakeQString(qs);
        qt.variantDtor(&v);
        return s;
    }
    bool MenuHas(void* menu, void* action)
    {
        if (!menu || !action) return false;
        QListRaw list = {};
        qt.actions(menu, &list);
        bool found = false;
        if (list.d)
            for (int i = list.d->begin; i < list.d->end && !found; ++i) found = list.d->array[i] == action;
        ListRelease(list);
        return found;
    }

    // ---- paths ----
    std::wstring Normalize(std::wstring p)
    {
        for (wchar_t& c : p) if (c == L'/') c = L'\\';
        wchar_t full[MAX_PATH * 4];
        const DWORD n = GetFullPathNameW(p.c_str(), DWORD(sizeof full / sizeof full[0]), full, nullptr);
        if (n && n < sizeof full / sizeof full[0]) p = full;
        while (p.size() > 3 && p.back() == L'\\') p.pop_back();
        return p;
    }
    int Kind(const std::wstring& p)     // 0 missing, 1 file, 2 folder
    {
        const DWORD a = GetFileAttributesW(p.c_str());
        return a == INVALID_FILE_ATTRIBUTES ? 0 : (a & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 1;
    }
    bool IsAbsolute(const std::string& s) { return (s.size() > 1 && s[1] == ':') || s.compare(0, 2, "\\\\") == 0 || s.compare(0, 2, "//") == 0; }
    std::string TrimSlashes(std::string s)
    {
        const size_t a = s.find_first_not_of("/\\");
        if (a == std::string::npos) return std::string();
        const size_t b = s.find_last_not_of("/\\");
        return s.substr(a, b - a + 1);
    }

    // the browser's scan root, else <TA_GAME_PATH>\map_source\_prefabs, else from the exe
    std::wstring PrefabRoot()
    {
        if (g_box)
        {
            const std::string own = QStringText(*reinterpret_cast<void* const*>(g_box + off_Box_RootPath));
            if (!own.empty())
            {
                wchar_t exp[MAX_PATH * 2];
                const std::wstring w = Wide(own);
                const DWORD n = ExpandEnvironmentStringsW(w.c_str(), exp, DWORD(sizeof exp / sizeof exp[0]));
                const std::wstring p = Normalize(n && n <= sizeof exp / sizeof exp[0] ? exp : w);
                if (Kind(p) == 2) return p;
            }
        }
        wchar_t env[MAX_PATH * 2];
        DWORD n = GetEnvironmentVariableW(L"TA_GAME_PATH", env, DWORD(sizeof env / sizeof env[0]));
        if (n && n < sizeof env / sizeof env[0])
        {
            const std::wstring p = Normalize(std::wstring(env) + L"\\map_source\\_prefabs");
            if (Kind(p) == 2) return p;
        }
        wchar_t exe[MAX_PATH * 2];
        n = GetModuleFileNameW(nullptr, exe, DWORD(sizeof exe / sizeof exe[0]));     // <root>\bin\Radiant_modtools.exe
        std::wstring p(exe, n);
        p = p.substr(0, p.find_last_of(L'\\'));
        return Normalize(p + L"\\..\\map_source\\_prefabs");
    }

    struct Target { std::wstring path; bool file = false; std::string how; };

    // The clicked item as a file or folder on disk: its UserRole name and row texts are tried under the root, else the
    // nearest existing folder of the first candidate, else the root.
    Target Resolve(void* item)
    {
        Target t;
        const std::wstring root = PrefabRoot();
        const std::string name = ItemData(item, kNameRole), text = ItemData(item, 0);
        std::vector<std::string> chain;                 // row texts from the top level down to the item
        for (void* it = item; it && chain.size() < 32; it = qt.parent(it)) chain.insert(chain.begin(), ItemData(it, 0));
        std::string joined;
        for (const std::string& c : chain) if (!TrimSlashes(c).empty()) joined += (joined.empty() ? "" : "\\") + TrimSlashes(c);

        std::vector<std::pair<std::wstring, std::string>> tries;
        auto addFile = [&](std::wstring p, const char* how)
        {
            tries.emplace_back(p, how);
            const size_t slash = p.find_last_of(L"\\/"), dot = p.find_last_of(L'.');
            if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) tries.emplace_back(p + L".map", how);
        };
        const std::string n = TrimSlashes(name);
        if (!n.empty())
        {
            if (IsAbsolute(name)) addFile(Wide(name), "name");
            else
            {
                addFile(root + L"\\..\\" + Wide(n), "name under map_source");   // Radiant's names: "_prefabs/<dir>/<file>.map"
                addFile(root + L"\\" + Wide(n), "name under root");
            }
        }
        if (!joined.empty()) addFile(root + L"\\" + Wide(joined), "tree path");
        if (!TrimSlashes(text).empty() && TrimSlashes(text) != joined) addFile(root + L"\\" + Wide(TrimSlashes(text)), "row text");

        for (auto& c : tries)
        {
            const std::wstring p = Normalize(c.first);
            const int k = Kind(p);
            if (k) { t.path = p; t.file = k == 1; t.how = c.second; return t; }
        }
        // nothing exists: the nearest existing folder of the first candidate (kept inside the root), else the root
        if (!tries.empty())
        {
            std::wstring p = Normalize(tries.front().first);
            while (!p.empty() && p.size() > root.size() && Kind(p) != 2)
            {
                const size_t s = p.find_last_of(L'\\');
                if (s == std::wstring::npos) break;
                p.resize(s);
            }
            if (Kind(p) == 2 && p.size() >= root.size() && _wcsnicmp(p.c_str(), root.c_str(), root.size()) == 0)
            { t.path = p; t.how = "nearest folder (not found: '" + (n.empty() ? joined : n) + "')"; return t; }
        }
        t.path = root;
        t.how = "prefab root (not found: '" + (n.empty() ? joined : n) + "')";
        return t;
    }

    // A file: its folder with the file selected. A separate explorer.exe returns at once, SHOpenFolderAndSelectItems on
    // the UI thread sits in a COM modal loop for about 70 s.
    bool Reveal(const Target& t)
    {
        if (t.path.empty() || !Kind(t.path)) return false;
        wchar_t win[MAX_PATH];
        const UINT n = GetWindowsDirectoryW(win, MAX_PATH);
        const std::wstring exe = (n && n < MAX_PATH ? std::wstring(win, n) : std::wstring(L"C:\\Windows")) + L"\\explorer.exe";
        std::wstring cmd = L"\"" + exe + L"\" " + (t.file ? L"/select,\"" + t.path + L"\"" : L"\"" + t.path + L"\"");
        STARTUPINFOW si = { sizeof si };
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
        AllowSetForegroundWindow(pi.dwProcessId);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }

    void ShowItem(void* item)
    {
        if (!item) { g_last = "no item under the menu"; rp::Log("prefabexplore: no right-clicked item"); return; }
        const Target t = Resolve(item);
        const bool ok = Reveal(t);
        ++g_shown;
        g_last = Utf8(t.path) + (t.file ? " (file, " : " (folder, ") + t.how + ")";
        rp::Log("prefabexplore: '%s' -> %s%s", ItemData(item, 0).c_str(), g_last.c_str(), ok ? "" : ", Explorer did not open");
    }

    void OnShowInExplorer(void*)
    {
        if (!g_box) return;
        ShowItem(*reinterpret_cast<void* const*>(g_box + off_Box_MenuItem));   // set by OpenContextMenu for the exec
    }

    void Detach()
    {
        if (g_menu && qt.removeAction)
            for (void* a : { g_action, g_sep })
                if (a && MenuHas(g_menu, a)) { qt.removeAction(g_menu, a); qt.deleteLater(a); }
        g_menu = g_sep = g_action = nullptr;
    }

    void Attach(char* box)
    {
        if (box != g_box) { g_menu = g_sep = g_action = nullptr; g_box = box; }   // a new browser: the old menu went with the old one
        if (!box || !g_enabled) return;
        void* menu = *reinterpret_cast<void* const*>(box + off_Box_Menu);
        if (!menu || !Metacast(menu, "QMenu")) { rp::Log("prefabexplore: Prefab Browser 0x%p has no context menu", box); return; }
        if (menu == g_menu && MenuHas(menu, g_action)) return;
        if (menu == g_menu && g_sep && MenuHas(menu, g_sep)) { qt.removeAction(menu, g_sep); qt.deleteLater(g_sep); }   // our entry went missing: no second separator
        g_menu = menu;
        g_sep = qt.addSeparator(menu);
        g_action = rp::host->AddMenuItem(rp::self, menu, "Show in Explorer", OnShowInExplorer, nullptr);
        rp::Log("prefabexplore: \"Show in Explorer\" added to the Prefab Browser 0x%p menu 0x%p (root %s)", box, menu,
                Utf8(PrefabRoot()).c_str());
    }

    char* FindBrowser()
    {
        QListRaw list = {};
        qt.allWidgets(&list);
        char* found = nullptr;
        if (list.d)
            for (int i = list.d->begin; i < list.d->end && !found; ++i)
            {
                void* w = list.d->array[i];
                if (w && *static_cast<void**>(w) == g_vtbl) found = static_cast<char*>(w);
            }
        ListRelease(list);
        return found;
    }

    // item n of a depth-first walk (shown == n), for the test command
    void* NthItem(void* item, int n, int& shown, int depth, std::string* dump)
    {
        const int rows = qt.rowCount(item);
        for (int r = 0; r < rows; ++r)
        {
            void* c = qt.child(item, r, 0);
            if (!c) continue;
            const int kids = qt.rowCount(c);
            if (dump) *dump += std::to_string(shown) + ": " + std::string(size_t(depth) * 2, ' ') + "'" + ItemData(c, 0) + "' name='"
                              + ItemData(c, kNameRole) + "'" + (kids ? " (" + std::to_string(kids) + " children)" : "") + "\n";
            if (shown++ == n) return c;
            if (kids && depth < 4)
                if (void* f = NthItem(c, n, shown, depth + 1, dump)) return f;
        }
        return nullptr;
    }
}

void Setup()
{
    g_enabled = rp::host->ConfigGetInt(rp::self, "prefabs", "show_in_explorer", 1) != 0;
    HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll"), c = GetModuleHandleW(L"Qt5Core64r.dll"), g = GetModuleHandleW(L"Qt5Gui64r.dll");
    bool ok = Get(qt.allWidgets, w, "?allWidgets@QApplication@@SA?AV?$QList@PEAVQWidget@@@@XZ")
           && Get(qt.actions, w, "?actions@QWidget@@QEBA?AV?$QList@PEAVQAction@@@@XZ")
           && Get(qt.removeAction, w, "?removeAction@QWidget@@QEAAXPEAVQAction@@@Z")
           && Get(qt.addSeparator, w, "?addSeparator@QMenu@@QEAAPEAVQAction@@XZ")
           && Get(qt.dispose, c, "?dispose@QListData@@SAXPEAUData@1@@Z")
           && Get(qt.deleteLater, c, "?deleteLater@QObject@@QEAAXXZ")
           && Get(qt.toString, c, "?toString@QVariant@@QEBA?AVQString@@XZ")
           && Get(qt.variantDtor, c, "??1QVariant@@QEAA@XZ")
           && Get(qt.stringDtor, c, "??1QString@@QEAA@XZ")
           && Get(qt.parent, g, "?parent@QStandardItem@@QEBAPEAV1@XZ")
           && Get(qt.rootItem, g, "?invisibleRootItem@QStandardItemModel@@QEBAPEAVQStandardItem@@XZ")
           && Get(qt.child, g, "?child@QStandardItem@@QEBAPEAV1@HH@Z")
           && Get(qt.rowCount, g, "?rowCount@QStandardItem@@QEBAHXZ");
    g_vtbl = At<void*>(kPrefabBrowserVtbl);
    g_ready = ok && g_vtbl && rp::host->AddMenuItem;
    rp::Log("prefabexplore: Prefab Browser \"Show in Explorer\": %s",
            !g_ready ? "unavailable (Qt or Radiant mismatch)" : g_enabled ? "on" : "off ([prefabs] show_in_explorer=0)");
}

void Tick()
{
    if (!g_ready || !g_enabled) return;
    const uint64_t now = GetTickCount64();
    if (now < g_nextScanMs) return;
    // the browser is created with the panes, look until it exists, then re-check now and then
    char* box = FindBrowser();
    g_nextScanMs = now + (box ? 10000 : 2000);
    Attach(box);
}

void Shutdown()
{
    if (g_ready) Detach();
    g_box = nullptr;
}

bool Command(const std::string& command, std::string& reply)
{
    if (command.compare(0, 13, "prefabexplore") != 0) return false;
    const std::string arg = command.size() > 14 ? command.substr(14) : std::string();
    if (!g_ready) { reply = "prefabexplore unavailable\n"; return true; }
    if (arg == "on" || arg == "off")
    {
        g_enabled = arg == "on";
        rp::host->ConfigSet(rp::self, "prefabs", "show_in_explorer", g_enabled ? "1" : "0");
        if (g_enabled) { g_nextScanMs = 0; Tick(); } else Detach();
    }
    void* model = g_box ? *reinterpret_cast<void* const*>(g_box + off_Box_Model) : nullptr;
    void* root = model ? qt.rootItem(model) : nullptr;
    if (arg.compare(0, 5, "items") == 0)        // items [n]: the browser's rows, numbered for "show"
    {
        if (!root) { reply = "no Prefab Browser\n"; return true; }
        int shown = 0;
        std::string out;
        NthItem(root, arg.size() > 6 ? atoi(arg.c_str() + 6) : 60, shown, 0, &out);
        reply = out;
        return true;
    }
    if (arg.compare(0, 8, "resolve ") == 0 || arg.compare(0, 5, "show ") == 0)   // the row "items" numbered n
    {
        const bool show = arg[0] == 's';
        int shown = 0;
        void* item = root ? NthItem(root, atoi(arg.c_str() + (show ? 5 : 8)), shown, 0, nullptr) : nullptr;
        if (!item) { reply = "no such item\n"; return true; }
        if (show) { ShowItem(item); reply = g_last + "\n"; return true; }
        const Target t = Resolve(item);
        reply = Utf8(t.path) + (t.file ? " (file, " : " (folder, ") + t.how + ")\n";
        return true;
    }
    char buf[1024];
    snprintf(buf, sizeof buf,
             "prefabexplore: %s; browser 0x%p menu 0x%p action 0x%p (%s); root %s; %u shown, last '%s'\n"
             "  prefabexplore on|off, prefabexplore items [n], prefabexplore resolve <n>, prefabexplore show <n>\n",
             g_enabled ? "on" : "off", static_cast<void*>(g_box), g_menu, g_action,
             MenuHas(g_menu, g_action) ? "in the menu" : "not in the menu", Utf8(PrefabRoot()).c_str(), g_shown, g_last.c_str());
    reply = buf;
    return true;
}
}
