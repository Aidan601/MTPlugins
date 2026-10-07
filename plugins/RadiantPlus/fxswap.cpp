// FX Browser applies to the selection: watches the QtEffectsBrowser tree's viewport for a plain left click and sets the selected fx entities' "fxdef"
// through Radiant's Entity Info key edit path (one Ctrl+Z). It waits out the double-click time so a double-click (place a new fx) changes nothing.
#include "modtools_plugin.hpp"
#include "fxswap.h"
#include "fxpreview.h"
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace br::fxswap
{
namespace
{
    // IDA addresses (no ASLR), resolved through host->RadiantAddress
    const uintptr_t kEffectsBrowserVtbl = 0x142508438;  // QtEffectsBrowser's vtable (RTTI .?AVQtEffectsBrowser@@)
    const uintptr_t kUndoIsOpen         = 0x140C16ED0;  // () -> bool
    const uintptr_t kUndoBegin          = 0x140C164D0;  // (const char* label), kept by pointer
    const uintptr_t kUndoAddEntity      = 0x140C168D0;  // (entity_t*)
    const uintptr_t kUndoEnd            = 0x140C16520;  // (): marks the map modified, relabels Edit > Undo
    const uintptr_t kSetKeyValue        = 0x140204A80;  // (entity_t*, const std::string& key, const std::string& value)
    const uintptr_t kEntityInfoRefresh  = 0x140858AB0;  // QtEntityInfo::EntitySelectionChanged(this)
    const uintptr_t kMainWin            = 0x1444C6560;  // RadiantMainWin*, +64 = RadiantQtBridge, bridge+312 = QtEntityInfo

    // QtAbstractToolbox fields (ItemDoubleClicked slot 0x14081BFD0)
    const size_t off_Box_Model  = 424;  // QStandardItemModel* (source)
    const size_t off_Box_Proxy  = 432;  // QSortFilterProxyModel* (the tree's model), mapToSource = vtable +368
    const size_t off_Box_Tree   = 440;  // the tree view
    const size_t off_Proxy_MapToSource = 368;
    const int    kNameRole      = 256;  // Qt::UserRole: the name a double-click places

    const size_t off_BInst_EntityInst = 16;
    const size_t off_Entity_EClass    = 240;
    const size_t off_Entity_Keys      = 304;
    const size_t off_Bridge_EntityInfo = 312;

    struct QListDataD { int ref; int alloc; int begin; int end; void* array[1]; };
    struct QListRaw { QListDataD* d; };
    struct QArrayDataRaw { int ref; int size; unsigned alloc; ptrdiff_t offset; };
    struct QPointRaw { int x, y; };
    struct QModelIndexRaw { int r, c; void* i; const void* m; };
    struct QVariantRaw { uint64_t data, type; };
    // MSVC 2012 std::string: 16-byte buffer or pointer, size, capacity (15 = inline)
    struct MsvcString { union { char buf[16]; const char* ptr; }; size_t size; size_t cap; };

    using AllWidgetsFn    = QListRaw* (*)(QListRaw* ret);
    using ListDisposeFn   = void (*)(QListDataD* d);
    using ObjectNameFn    = void** (*)(const void* obj, void** retQString);
    using MapFromGlobalFn = QPointRaw* (*)(const void* widget, QPointRaw* ret, const QPointRaw* pos);
    using IndexAtFn       = QModelIndexRaw* (*)(const void* view, QModelIndexRaw* ret, const QPointRaw* pos);
    using MapToSourceFn   = QModelIndexRaw* (*)(const void* proxy, QModelIndexRaw* ret, const QModelIndexRaw* idx);
    using ItemFromIndexFn = void* (*)(const void* model, const QModelIndexRaw* idx);
    using RootItemFn      = void* (*)(const void* model);
    using ChildFn         = void* (*)(const void* item, int row, int col);
    using RowCountFn      = int (*)(const void* item);
    using ItemDataFn      = QVariantRaw* (*)(const void* item, QVariantRaw* ret, int role);   // QStandardItem vtable +8
    using ToStringFn      = void** (*)(const void* variant, void** retQString);
    using VariantDtorFn   = void (*)(void* variant);
    using StringDtorFn    = void (*)(void** s);
    using MetacastFn      = void* (*)(void* obj, const char* cls);                             // QObject vtable slot 1
    using UndoIsOpenFn    = bool (*)();
    using UndoBeginFn     = void (*)(const char* label);
    using UndoEntityFn    = void (*)(void* entity);
    using UndoEndFn       = void (*)();
    using SetKeyValueFn   = bool (*)(void* entity, const MsvcString* key, const MsvcString* value);
    using RefreshFn       = void (*)(void* entityInfo);

    struct Qt
    {
        AllWidgetsFn allWidgets; ListDisposeFn dispose; ObjectNameFn objectName; MapFromGlobalFn mapFromGlobal;
        IndexAtFn indexAt; ItemFromIndexFn itemFromIndex; RootItemFn rootItem; ChildFn child; RowCountFn rowCount;
        ToStringFn toString; VariantDtorFn variantDtor; StringDtorFn stringDtor;
    } qt = {};

    bool  g_enabled = true;             // [fx] browser_apply
    bool  g_ready = false;
    void* g_vtbl = nullptr;
    UndoIsOpenFn  g_undoIsOpen = nullptr;
    UndoBeginFn   g_undoBegin = nullptr;
    UndoEntityFn  g_undoAddEntity = nullptr;
    UndoEndFn     g_undoEnd = nullptr;
    SetKeyValueFn g_setKeyValue = nullptr;
    RefreshFn     g_entityInfoRefresh = nullptr;
    void** g_mainWin = nullptr;

    char*  g_box = nullptr;             // the QtEffectsBrowser
    void*  g_tree = nullptr;
    void*  g_viewport = nullptr;        // filtered
    ModToolsWidget g_filter = nullptr;
    uint64_t g_nextScanMs = 0;

    bool g_armed = false;               // a plain left press on the viewport, not yet released
    int  g_pressX = 0, g_pressY = 0;
    std::string g_pending;              // the effect clicked, applied at g_dueMs
    uint64_t g_dueMs = 0;
    std::string g_last;                 // the last effect applied
    unsigned g_applied = 0, g_clicks = 0;
    bool g_debug = false;               // "fxswap debug": log the viewport's mouse events

    template <class T> bool Get(T& fn, HMODULE m, const char* name)
    {
        fn = m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
        return fn != nullptr;
    }
    template <class T> T At(uintptr_t ida) { return reinterpret_cast<T>(rp::host->RadiantAddress(rp::self, ida)); }

    std::string Utf8(const wchar_t* w, int n)
    {
        int len = n > 0 ? WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr) : 0;
        std::string out(len > 0 ? len : 0, '\0');
        if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w, n, &out[0], len, nullptr, nullptr);
        return out;
    }
    std::string QStringText(void* d)
    {
        const QArrayDataRaw* a = static_cast<const QArrayDataRaw*>(d);
        if (!a || a->size <= 0) return std::string();
        return Utf8(reinterpret_cast<const wchar_t*>(reinterpret_cast<const char*>(a) + a->offset), a->size);
    }
    // takes the QString returned by a Qt call and releases it
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
    std::string ItemName(const void* item, int role)
    {
        QVariantRaw v = {};
        (*reinterpret_cast<ItemDataFn* const*>(item))[1](item, &v, role);
        void* qs = nullptr;
        qt.toString(&v, &qs);
        std::string s = TakeQString(qs);
        qt.variantDtor(&v);
        return s;
    }

    // ---- MSVC 2012 std::string and std::map<std::string, QVariant> as Radiant lays them out ----
    std::string RadString(const char* s)
    {
        const size_t size = *reinterpret_cast<const size_t*>(s + 16), cap = *reinterpret_cast<const size_t*>(s + 24);
        const char* text = cap >= 16 ? *reinterpret_cast<const char* const*>(s) : s;
        return size < 4096 && text ? std::string(text, size) : std::string();
    }
    MsvcString MakeRadString(const std::string& s)    // s must outlive the call
    {
        MsvcString m = {};
        m.size = s.size();
        if (s.size() < 16) { memcpy(m.buf, s.c_str(), s.size() + 1); m.cap = 15; }
        else { m.ptr = s.c_str(); m.cap = s.size(); }
        return m;
    }
    // the entity's own value of `key` (QString values only), false when it has none
    bool EntityKey(const char* entity, const char* key, std::string& out)
    {
        const char* head = *reinterpret_cast<const char* const*>(entity + off_Entity_Keys);
        if (!head) return false;
        const char* node = *reinterpret_cast<const char* const*>(head + 8);      // root
        for (int guard = 0; node && !node[25] && guard < 64; ++guard)
        {
            const int cmp = RadString(node + 32).compare(key);
            if (cmp == 0)
            {
                const QVariantRaw* v = reinterpret_cast<const QVariantRaw*>(node + 64);
                if ((v->type & 0x3FFFFFFF) != 10) return false;
                out = QStringText(reinterpret_cast<void*>(v->data));
                return true;
            }
            node = *reinterpret_cast<const char* const*>(node + (cmp < 0 ? 16 : 0));
        }
        return false;
    }
    bool IsFx(const char* entity)
    {
        const char* eclass = *reinterpret_cast<const char* const*>(entity + off_Entity_EClass);
        return eclass && RadString(eclass) == "fx";
    }

    // Radiant keeps the undo label by pointer: a copy that outlives
    // this DLL across a hot reload.
    const char* UndoLabel()
    {
        static const char* label = nullptr;
        if (!label)
        {
            static const char text[] = "Change FX";
            if (char* m = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, sizeof text))) { memcpy(m, text, sizeof text); label = m; }
        }
        return label;
    }

    std::vector<char*> SelectedFx()
    {
        std::vector<char*> out;
        size_t n = rp::host->SelectedBrushInsts(rp::self, nullptr, 0);
        if (!n) return out;
        std::vector<ModToolsBrushInst> insts(n + 16);
        n = rp::host->SelectedBrushInsts(rp::self, insts.data(), insts.size());
        if (n > insts.size()) n = insts.size();
        for (size_t i = 0; i < n; ++i)
        {
            const char* inst = static_cast<const char*>(insts[i]);
            const char* ei = inst ? *reinterpret_cast<const char* const*>(inst + off_BInst_EntityInst) : nullptr;
            char* e = ei ? *reinterpret_cast<char* const*>(ei) : nullptr;
            if (!e || !IsFx(e)) continue;
            bool dup = false;
            for (char* x : out) if (x == e) { dup = true; break; }
            if (!dup) out.push_back(e);
        }
        return out;
    }

    bool RefreshEntityInfo()
    {
        char* main = g_mainWin ? static_cast<char*>(*g_mainWin) : nullptr;
        char* bridge = main ? *reinterpret_cast<char**>(main + 64) : nullptr;
        void* info = bridge ? *reinterpret_cast<void**>(bridge + off_Bridge_EntityInfo) : nullptr;
        if (!info || !Metacast(info, "QtEntityInfo")) return false;
        g_entityInfoRefresh(info);
        return true;
    }

    // Sets fxdef on every selected fx entity that has another one. Returns the count changed, -1 while an undo is open.
    int Apply(std::string fx)
    {
        // the browser names items "/dir/name", fxdef has no leading slash
        for (char& ch : fx) if (ch == '\\') ch = '/';
        fx.erase(0, fx.find_first_not_of('/') == std::string::npos ? fx.size() : fx.find_first_not_of('/'));
        if (fx.empty()) return 0;
        std::vector<char*> all = SelectedFx(), change;
        for (char* e : all)
        {
            std::string cur;
            if (!EntityKey(e, "fxdef", cur) || _stricmp(cur.c_str(), fx.c_str()) != 0) change.push_back(e);
        }
        if (change.empty()) return 0;
        if (g_undoIsOpen()) return -1;
        const std::string key = "fxdef";
        const MsvcString k = MakeRadString(key), v = MakeRadString(fx);
        g_undoBegin(UndoLabel());
        for (char* e : change)
        {
            g_undoAddEntity(e);
            g_setKeyValue(e, &k, &v);
        }
        g_undoEnd();
        const bool info = RefreshEntityInfo();
        if (rp::host->RequestRedraw) rp::host->RequestRedraw(rp::self, MODTOOLS_VIEW_3D | MODTOOLS_VIEW_2D);
        g_last = fx;
        g_applied += unsigned(change.size());
        rp::Log("fxswap: fxdef '%s' on %zu fx entit%s (%zu selected)%s", fx.c_str(), change.size(), change.size() == 1 ? "y" : "ies", all.size(),
                info ? "" : ", Entity Info not found");
        return int(change.size());
    }

    // the browser's item name under a screen point, "" when there is none
    std::string NameAt(int sx, int sy)
    {
        if (!g_box || !g_tree || !g_viewport) return std::string();
        QPointRaw global = { sx, sy }, local = {};
        qt.mapFromGlobal(g_viewport, &local, &global);
        QModelIndexRaw vi = { -1, -1, nullptr, nullptr };
        qt.indexAt(g_tree, &vi, &local);
        if (vi.r < 0 || vi.c < 0 || !vi.m) return std::string();
        void* proxy = *reinterpret_cast<void**>(g_box + off_Box_Proxy);
        void* model = *reinterpret_cast<void**>(g_box + off_Box_Model);
        if (!model) return std::string();
        QModelIndexRaw si = vi;
        if (proxy && vi.m == proxy)
        {
            si = { -1, -1, nullptr, nullptr };
            (*reinterpret_cast<MapToSourceFn* const*>(proxy))[off_Proxy_MapToSource / 8](proxy, &si, &vi);
        }
        if (si.r < 0 || si.m != model) return std::string();
        void* item = qt.itemFromIndex(model, &si);
        if (!item || qt.rowCount(item) > 0) return std::string();      // a category, not an effect
        return ItemName(item, kNameRole);
    }

    int OnViewportEvent(void*, ModToolsWidget watched, int type)
    {
        if (!g_enabled || watched != g_viewport) return 0;
        const DWORD pos = GetMessagePos();
        const int x = short(LOWORD(pos)), y = short(HIWORD(pos));
        if (g_debug && type >= 2 && type <= 4)
            rp::Log("fxswap: event %d at %d,%d armed=%d lbutton=%d name='%s'", type, x, y, g_armed ? 1 : 0,
                    (GetKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0, NameAt(x, y).c_str());
        if (type == 2)          // MouseButtonPress
        {
            g_armed = (GetKeyState(VK_LBUTTON) & 0x8000) && !(GetKeyState(VK_RBUTTON) & 0x8000) && !(GetKeyState(VK_MBUTTON) & 0x8000)
                   && !(GetKeyState(VK_MENU) & 0x8000);
            g_pressX = x; g_pressY = y;
        }
        else if (type == 4)     // MouseButtonDblClick: Radiant places a new fx, leave the selection alone
        {
            g_armed = false;
            g_pending.clear();
        }
        else if (type == 3 && g_armed)  // MouseButtonRelease
        {
            g_armed = false;
            if (abs(x - g_pressX) > GetSystemMetrics(SM_CXDRAG) || abs(y - g_pressY) > GetSystemMetrics(SM_CYDRAG)) return 0;   // a drag
            std::string name = NameAt(x, y);
            if (name.empty()) return 0;
            ++g_clicks;
            g_pending = name;
            g_dueMs = GetTickCount64() + GetDoubleClickTime();
        }
        return 0;
    }

    void* FindViewport(void* tree)
    {
        if (!rp::host->ui || !rp::host->ui->FindChild) return nullptr;
        for (int i = 0; i < 64; ++i)
        {
            void* w = rp::host->ui->FindChild(tree, "QWidget", i);
            if (!w) break;
            void* qs = nullptr;
            qt.objectName(w, &qs);
            if (TakeQString(qs) == "qt_scrollarea_viewport") return w;   // depth first: the tree's own comes before its header's
        }
        return nullptr;
    }

    void Attach(char* box)
    {
        g_box = box; g_tree = nullptr; g_viewport = nullptr; g_armed = false;
        if (!box) return;
        void* tree = *reinterpret_cast<void**>(box + off_Box_Tree);
        if (!tree || !Metacast(tree, "QTreeView")) { rp::Log("fxswap: FX Browser 0x%p has no tree view", box); return; }
        void* vp = FindViewport(tree);
        if (!vp) { rp::Log("fxswap: FX Browser tree 0x%p has no viewport", tree); return; }
        if (!g_filter) g_filter = rp::host->MakeEventFilter(rp::self, OnViewportEvent, nullptr);
        if (!g_filter) return;
        rp::host->ui->InstallFilter(vp, g_filter);
        g_tree = tree; g_viewport = vp;
        rp::Log("fxswap: watching the FX Browser 0x%p (tree 0x%p, viewport 0x%p)", box, tree, vp);
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

    std::string DumpItems(int limit)
    {
        if (!g_box) return "no FX Browser\n";
        void* model = *reinterpret_cast<void**>(g_box + off_Box_Model);
        void* root = model ? qt.rootItem(model) : nullptr;
        if (!root) return "no model\n";
        std::string out;
        int shown = 0;
        struct Walk { static void Run(void* item, int depth, int limit, int& shown, std::string& out)
        {
            const int rows = qt.rowCount(item);
            for (int r = 0; r < rows && shown < limit; ++r)
            {
                void* c = qt.child(item, r, 0);
                if (!c) continue;
                const int kids = qt.rowCount(c);
                out += std::string(size_t(depth) * 2, ' ') + "'" + ItemName(c, 0) + "' name='" + ItemName(c, kNameRole) + "'"
                     + (kids ? " (" + std::to_string(kids) + " children)" : "") + "\n";
                ++shown;
                if (kids && depth < 3) Run(c, depth + 1, limit, shown, out);
            }
        } };
        Walk::Run(root, 0, limit, shown, out);
        return out;
    }
}

void Setup()
{
    g_enabled = rp::host->ConfigGetInt(rp::self, "fx", "browser_apply", 1) != 0;
    HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll"), c = GetModuleHandleW(L"Qt5Core64r.dll"), g = GetModuleHandleW(L"Qt5Gui64r.dll");
    bool ok = Get(qt.allWidgets, w, "?allWidgets@QApplication@@SA?AV?$QList@PEAVQWidget@@@@XZ")
           && Get(qt.mapFromGlobal, w, "?mapFromGlobal@QWidget@@QEBA?AVQPoint@@AEBV2@@Z")
           && Get(qt.indexAt, w, "?indexAt@QTreeView@@UEBA?AVQModelIndex@@AEBVQPoint@@@Z")
           && Get(qt.dispose, c, "?dispose@QListData@@SAXPEAUData@1@@Z")
           && Get(qt.objectName, c, "?objectName@QObject@@QEBA?AVQString@@XZ")
           && Get(qt.toString, c, "?toString@QVariant@@QEBA?AVQString@@XZ")
           && Get(qt.variantDtor, c, "??1QVariant@@QEAA@XZ")
           && Get(qt.stringDtor, c, "??1QString@@QEAA@XZ")
           && Get(qt.itemFromIndex, g, "?itemFromIndex@QStandardItemModel@@QEBAPEAVQStandardItem@@AEBVQModelIndex@@@Z")
           && Get(qt.rootItem, g, "?invisibleRootItem@QStandardItemModel@@QEBAPEAVQStandardItem@@XZ")
           && Get(qt.child, g, "?child@QStandardItem@@QEBAPEAV1@HH@Z")
           && Get(qt.rowCount, g, "?rowCount@QStandardItem@@QEBAHXZ");
    g_vtbl              = At<void*>(kEffectsBrowserVtbl);
    g_undoIsOpen        = At<UndoIsOpenFn>(kUndoIsOpen);
    g_undoBegin         = At<UndoBeginFn>(kUndoBegin);
    g_undoAddEntity     = At<UndoEntityFn>(kUndoAddEntity);
    g_undoEnd           = At<UndoEndFn>(kUndoEnd);
    g_setKeyValue       = At<SetKeyValueFn>(kSetKeyValue);
    g_entityInfoRefresh = At<RefreshFn>(kEntityInfoRefresh);
    g_mainWin           = At<void**>(kMainWin);
    ok = ok && g_vtbl && g_undoIsOpen && g_undoBegin && g_undoAddEntity && g_undoEnd && g_setKeyValue && g_entityInfoRefresh && g_mainWin
            && rp::host->ui && rp::host->ui->FindChild && rp::host->ui->InstallFilter && rp::host->MakeEventFilter && rp::host->SelectedBrushInsts;
    g_ready = ok;
    rp::Log("fxswap: FX Browser click applies to selected fx: %s", !ok ? "unavailable (Qt or Radiant mismatch)" : g_enabled ? "on" : "off ([fx] browser_apply=0)");
}

void Tick()
{
    if (!g_ready || !g_enabled) return;
    const uint64_t now = GetTickCount64();
    if (!g_pending.empty() && now >= g_dueMs)
    {
        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) g_dueMs = now + 50;            // a new press may still be a double-click
        else if (Apply(g_pending) < 0) g_dueMs = now + 100;                         // an undo operation is open: wait
        else { br::fxpreview::OnBrowserClick(g_pending); g_pending.clear(); }
    }
    if (now < g_nextScanMs) return;
    // the browser is created with the panes, look until it exists, then re-check now and then
    char* box = FindBrowser();
    g_nextScanMs = now + (box ? 10000 : 2000);
    if (box != g_box || (box && !g_viewport)) Attach(box);
}

int ApplyToSelection(const std::string& name)
{
    return g_ready ? Apply(name) : -2;
}

void* Browser()
{
    if (!g_ready) return nullptr;
    if (!g_box) g_box = FindBrowser();
    return g_box;
}

bool Command(const std::string& command, std::string& reply)
{
    if (command.compare(0, 6, "fxswap") != 0) return false;
    std::string arg = command.size() > 7 ? command.substr(7) : std::string();
    char buf[1024];
    if (arg.compare(0, 6, "apply ") == 0)
    {
        if (!g_ready) { reply = "fxswap unavailable\n"; return true; }
        const int n = Apply(arg.substr(6));
        snprintf(buf, sizeof buf, "fxswap apply: %d entit%s changed\n", n, n == 1 ? "y" : "ies");
        reply = buf;
        return true;
    }
    if (arg.compare(0, 5, "items") == 0) { reply = DumpItems(arg.size() > 6 ? atoi(arg.c_str() + 6) : 40); return true; }
    if (arg.compare(0, 7, "select ") == 0)     // selects these entities (space separated GUIDs), for tests
    {
        if (!rp::host->SelectEntityByGuid || !rp::host->SelectBrushInsts) { reply = "unavailable\n"; return true; }
        std::vector<ModToolsBrushInst> all;
        size_t found = 0, start = 7;
        while (start < arg.size())
        {
            size_t end = arg.find(' ', start);
            if (end == std::string::npos) end = arg.size();
            const std::string guid = arg.substr(start, end - start);
            start = end + 1;
            if (guid.empty() || !rp::host->SelectEntityByGuid(rp::self, guid.c_str())) continue;
            ++found;
            ModToolsBrushInst insts[256];
            const size_t n = rp::host->SelectedBrushInsts(rp::self, insts, 256);
            all.insert(all.end(), insts, insts + (n < 256 ? n : 256));
        }
        rp::host->DeselectAll(rp::self);
        if (!all.empty()) rp::host->SelectBrushInsts(rp::self, all.data(), all.size());
        snprintf(buf, sizeof buf, "selected %zu entities (%zu objects)\n", found, all.size());
        reply = buf;
        return true;
    }
    if (arg == "debug") { g_debug = !g_debug; reply = g_debug ? "fxswap debug on\n" : "fxswap debug off\n"; return true; }
    if (arg == "on" || arg == "off")
    {
        g_enabled = arg == "on";
        rp::host->ConfigSet(rp::self, "fx", "browser_apply", g_enabled ? "1" : "0");
    }
    const std::vector<char*> sel = g_ready ? SelectedFx() : std::vector<char*>();
    std::string fxdefs;
    for (size_t i = 0; i < sel.size() && i < 4; ++i)
    {
        std::string v;
        fxdefs += (i ? ", '" : " ('") + (EntityKey(sel[i], "fxdef", v) ? v : std::string("<none>")) + "'";
    }
    if (!fxdefs.empty()) fxdefs += sel.size() > 4 ? ", ...)" : ")";
    snprintf(buf, sizeof buf,
             "fxswap: %s, %s; browser 0x%p tree 0x%p viewport 0x%p; %zu fx selected%s; %u clicks, %u entities changed, last '%s'\n"
             "  fxswap on|off, fxswap apply <fxdef>, fxswap items [n], fxswap select <guid>\n",
             g_ready ? "ready" : "unavailable", g_enabled ? "on" : "off", static_cast<void*>(g_box), g_tree, g_viewport,
             sel.size(), fxdefs.c_str(), g_clicks, g_applied, g_last.c_str());
    reply = buf;
    return true;
}
}
