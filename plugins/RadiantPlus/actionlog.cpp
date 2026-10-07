// Action log: selections and edits printed to Radiant's Output pane (Undo_Begin/End snapshots, QAction triggers, a poll).
// Output goes through Sys_Printf from OnTick (never inside a hook) and a ring buffer. Nothing is hooked until first on.
#include "modtools_plugin.hpp"
#include "actionlog.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iterator>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace br::actionlog
{
namespace
{
    // IDA addresses (Radiant_modtools.exe, no ASLR), resolved through host->RadiantAddress.
    const uintptr_t kSysPrintf = 0x140D33210;   // (const char* fmt, ...): "HH:MM:SS: " + text to Radiant's log file and the Output pane
    const uintptr_t kUndoBegin = 0x140C164D0;   // (const char* label)
    const uintptr_t kUndoEnd   = 0x140C16520;   // ()

    // brush_t / face_t / patch fields, three texdefs
    // per face, 184 bytes apart from face+40 (material, lightmap, smoothing), name = std::string at texdef+0x20
    const size_t off_Brush_Mins = 64, off_Brush_Maxs = 76, off_Brush_FacesBegin = 144, off_Brush_FacesEnd = 152;
    const size_t off_Brush_Layer = 168, off_Brush_Patch = 224, off_Brush_Type = 232;
    const size_t sz_Face = 864, off_Face_Material = 40 + 32, off_Face_Lightmap = 40 + 184 + 32;
    const size_t off_Patch_Flags = 8, off_Patch_Material = 24, off_Patch_Lightmap = 160;
    const size_t off_Patch_Rows = 660, off_Patch_Cols = 664, off_Patch_Ctrl = 672, sz_PatchCtrl = 92;
    const int kMaxFaces = 64;
    const size_t kMaxObjects = 400;     // objects compared per step, a bigger selection is counted only
    const size_t kMaxKeyBrushes = 64;   // a brush entity with more brushes than this is not written out for its keys

    struct QListDataD { int ref; int alloc; int begin; int end; void* array[1]; };
    struct QListRaw { QListDataD* d; };
    struct QArrayDataRaw { int ref; int size; unsigned alloc; ptrdiff_t offset; };
    struct QPointRaw { int x, y; };

    using SysPrintfFn     = int (*)(const char* fmt, ...);
    using UndoBeginFn     = void (*)(const char* label);
    using UndoEndFn       = void (*)();
    using ActivateFn      = void (*)(void* action, int event);
    using StringRetFn     = void** (*)(const void* obj, void** ret);                // QString f() const
    using ListRetFn       = QListRaw* (*)(const void* obj, QListRaw* ret);          // QList<QWidget*> f() const
    using ListDisposeFn   = void (*)(QListDataD* d);
    using StringDtorFn    = void (*)(void** s);
    using KeySeqRetFn     = void** (*)(const void* action, void** ret);
    using KeySeqStringFn  = void** (*)(const void* seq, void** ret, int format);
    using KeySeqDtorFn    = void (*)(void** seq);
    using PtrFn           = void* (*)(const void* obj);
    using StaticPtrFn     = void* (*)();
    using WidgetAtFn      = void* (*)(const QPointRaw* pos);
    using ClassNameFn     = const char* (*)(const void* metaObject);
    using InsertWidgetFn  = void (*)(void* box, int index, void* widget, int stretch, int alignment);

    struct Qt
    {
        StringRetFn text = nullptr, windowTitle = nullptr;
        ListRetFn associatedWidgets = nullptr;
        ListDisposeFn dispose = nullptr;
        StringDtorFn stringDtor = nullptr;
        KeySeqRetFn shortcut = nullptr;
        KeySeqStringFn keySeqString = nullptr;
        KeySeqDtorFn keySeqDtor = nullptr;
        PtrFn menuAction = nullptr, parentWidget = nullptr, layout = nullptr;
        StaticPtrFn focusWidget = nullptr;
        WidgetAtFn widgetAt = nullptr;
        ClassNameFn className = nullptr;
        InsertWidgetFn insertWidget = nullptr;
    } qt;

    SysPrintfFn g_sysPrintf = nullptr;
    UndoBeginFn g_oUndoBegin = nullptr;
    UndoEndFn   g_oUndoEnd = nullptr;
    ActivateFn  g_oActivate = nullptr;
    bool g_hooked = false, g_hookFailed = false;
    bool g_on = false;                   // recording: shown, or kept for problem reports
    bool g_display = false;              // [actionlog] enabled, the Output pane check box: printed to the Output pane

    // ---- ring buffer and output -----------------------------------------------------------------------------------
    struct Entry { uint64_t seq; std::string text; std::string when; uint64_t tick; };
    std::deque<Entry> g_ring;           // the last entries, for "actionlog tail/since"
    std::deque<std::string> g_print;    // waiting for OnTick
    uint64_t g_seq = 0;
    size_t g_ringMax = 2000;

    // in_reports: the last entries also go to <plugin dir>\actionlog.log for the problem report (made after a crash too)
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
        g_print.push_back(text);
        while (g_print.size() > 200) g_print.pop_front();
    }
    void WriteReportFile()
    {
        g_reportDirty = false;
        uint64_t now = GetTickCount64();
        size_t first = g_ring.size();
        while (first > 0 && g_ring.size() - first < kReportMax && now - g_ring[first - 1].tick <= kReportWindowMs) --first;
        std::string text = "RadiantPlus action log, written " + Stamp() + ": the entries of the last 15 minutes (at most 300), kept for problem "
                           "reports because [actionlog] in_reports=1 in RadiantPlus.ini\r\n";
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
    std::string Num(float v)
    {
        if (std::fabs(v) < 0.0005f) v = 0.0f;
        char b[32]; snprintf(b, sizeof b, "%.3f", v);
        std::string s = b;
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s;
    }
    std::string Vec(const float v[3], const char* sep = " ") { return Num(v[0]) + sep + Num(v[1]) + sep + Num(v[2]); }
    std::string JsonStr(const std::string& s)
    {
        std::string o = "\"";
        for (char c : s)
        {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if (static_cast<unsigned char>(c) < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o += c;
        }
        return o + "\"";
    }
    std::string JsonList(const std::vector<std::string>& v)
    {
        std::string o = "[";
        for (size_t i = 0; i < v.size(); ++i) { if (i) o += ","; o += JsonStr(v[i]); }
        return o + "]";
    }
    std::string JsonVec(const float v[3]) { return "[" + Vec(v, ",") + "]"; }
    std::string ShortGuid(const std::string& g) { return g.size() > 10 ? g.substr(0, 9) + "..}" : g; }

    template <class T> bool Get(T& fn, HMODULE m, const char* name)
    {
        fn = m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
        return fn != nullptr;
    }
    template <class T> T At(uintptr_t ida) { return reinterpret_cast<T>(rp::host->RadiantAddress(rp::self, ida)); }

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
    // QObject vtable: slot 0 metaObject(), slot 1 qt_metacast(const char*)
    const char* ClassNameSeh(const void* obj)
    {
        __try
        {
            auto vt = *reinterpret_cast<void* const* const*>(obj);
            auto metaObject = reinterpret_cast<const void* (*)(const void*)>(vt[0]);
            const void* mo = metaObject(obj);
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
    // "&Edit" -> "Edit", "Save As..." -> "Save As", "Undo\tCtrl+Z" -> "Undo"
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
    // "Edit/Undo": the action's text under the titles of the menus that hold it
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
        void* qs = nullptr; qt.keySeqString(&seq, &qs, 0);   // NativeText
        std::string s = TakeQString(qs);
        qt.keySeqDtor(&seq);
        return s;
    }
    // The view or pane a widget belongs to: Radiant's own class nearest to it (Qt's classes and the dock frames are
    // skipped), and the title of the pane around it.
    std::string Where(void* w)
    {
        std::string cls, title;
        for (int i = 0; w && i < 16; ++i)
        {
            std::string c = ClassName(w);
            bool qtOwn = c.size() > 1 && c[0] == 'Q' && isupper(static_cast<unsigned char>(c[1]));
            if (cls.empty() && !c.empty() && !qtOwn && c.compare(0, 6, "TADock") != 0) cls = c;
            void* parent = qt.parentWidget ? qt.parentWidget(w) : nullptr;
            if (title.empty() && parent && qt.windowTitle) title = ObjText(qt.windowTitle, w);
            w = parent;
        }
        if (cls == "QtCamRender" || cls == "CCamWnd") return "3D view";
        if (cls == "QtXYRender" || cls == "CXYWnd") return "2D view";
        if (cls == "QtZWndRender" || cls == "QtZWnd") return "Z view";
        if (!title.empty() && !cls.empty()) return title + " pane (" + cls + ")";
        if (!title.empty()) return title + " pane";
        return cls;
    }
    void* WidgetUnderCursor()
    {
        POINT p; if (!qt.widgetAt || !GetCursorPos(&p)) return nullptr;
        QPointRaw q = { p.x, p.y };
        return qt.widgetAt(&q);
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
    // the first non-modifier key held, by name ("Space", "M"), empty when none
    std::string HeldKey()
    {
        for (int vk = 0x08; vk <= 0xFE; ++vk)
        {
            if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || (vk >= VK_LSHIFT && vk <= VK_RMENU) ||
                vk == VK_LWIN || vk == VK_RWIN || vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL) continue;
            if (!Down(vk)) continue;
            char name[64] = {};
            LONG sc = static_cast<LONG>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)) << 16;
            if (GetKeyNameTextA(sc, name, sizeof name) > 0) return name;
            snprintf(name, sizeof name, "vk 0x%02X", vk);
            return name;
        }
        return std::string();
    }
    // How a step or selection change was started, when no menu action is running: the mouse button or key held and
    // the view or pane under the cursor (mouse) or with the keyboard focus (key).
    std::string InputSource()
    {
        const char* button = Down(VK_LBUTTON) ? "left mouse" : Down(VK_RBUTTON) ? "right mouse" : Down(VK_MBUTTON) ? "middle mouse" : nullptr;
        std::string where, how;
        if (button) { how = Mods() + button; where = Where(WidgetUnderCursor()); }
        else
        {
            std::string key = HeldKey();
            how = key.empty() ? std::string() : "key " + Mods() + key;
            where = Where(qt.focusWidget ? qt.focusWidget() : nullptr);
            if (how.empty())
            {
                // nothing held: a click handled on release (the Texture Browser), or code (a plugin)
                std::string under = Where(WidgetUnderCursor());
                return under.empty() ? std::string("code, no mouse button or key held (a plugin)")
                                     : "a mouse release or code, cursor over " + under;
            }
        }
        return where.empty() ? how : how + " in " + where;
    }

    // ---- the running menu action (QAction::activate) --------------------------------------------------------------
    struct ActionCtx { std::string path, keys; uint64_t start = 0; bool step = false; bool keyHeld = false; };
    ActionCtx g_act;
    int g_actDepth = 0;

    std::string ActionSource(const ActionCtx& a)
    {
        std::string s = (a.keyHeld ? "shortcut " : "menu ") + JsonStr(a.path);
        if (!a.keys.empty()) s += " (" + a.keys + ")";
        return s;
    }

    // ---- object snapshots -----------------------------------------------------------------------------------------
    struct RawFace { char mat[96]; char lm[96]; };
    struct RawBrush
    {
        int faces; int type; int patch; int mesh; int rows, cols;
        float mins[3], maxs[3];
        uint64_t shape;
        char pmat[96], plm[96], layer[96];
        RawFace f[kMaxFaces];
    };
    RawBrush g_raw;                      // main thread only

    void ReadStdString(const BYTE* s, char* out, size_t cap)
    {
        // MSVC 2012 std::string: 16-byte buffer or pointer, size at +16, capacity at +24 (15 = inline)
        size_t size = *reinterpret_cast<const size_t*>(s + 16), capacity = *reinterpret_cast<const size_t*>(s + 24);
        out[0] = 0;
        if (size > 4096 || capacity < size) return;
        const char* p = capacity >= 16 ? *reinterpret_cast<const char* const*>(s) : reinterpret_cast<const char*>(s);
        size_t n = size < cap - 1 ? size : cap - 1;
        memcpy(out, p, n); out[n] = 0;
    }
    uint64_t Fnv(uint64_t h, const void* data, size_t n)
    {
        const BYTE* p = static_cast<const BYTE*>(data);
        for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
        return h;
    }
    bool ReadBrushSeh(const BYTE* b, RawBrush* r, bool faces)
    {
        __try
        {
            memset(r, 0, offsetof(RawBrush, f));
            memcpy(r->mins, b + off_Brush_Mins, 12); memcpy(r->maxs, b + off_Brush_Maxs, 12);
            r->type = *reinterpret_cast<const int*>(b + off_Brush_Type);
            ReadStdString(b + off_Brush_Layer, r->layer, sizeof r->layer);
            r->shape = 1469598103934665603ull;
            if (!faces) return true;
            const BYTE* patch = *reinterpret_cast<const BYTE* const*>(b + off_Brush_Patch);
            if (patch)
            {
                r->patch = 1;
                r->mesh = (*reinterpret_cast<const unsigned*>(patch + off_Patch_Flags) & 0x40) ? 1 : 0;
                r->rows = *reinterpret_cast<const int*>(patch + off_Patch_Rows);
                r->cols = *reinterpret_cast<const int*>(patch + off_Patch_Cols);
                ReadStdString(patch + off_Patch_Material, r->pmat, sizeof r->pmat);
                ReadStdString(patch + off_Patch_Lightmap, r->plm, sizeof r->plm);
                const BYTE* ctrl = *reinterpret_cast<const BYTE* const*>(patch + off_Patch_Ctrl);
                if (ctrl && r->rows > 0 && r->cols > 0 && r->rows <= 256 && r->cols <= 256)
                    for (int i = 0; i < r->rows * r->cols; ++i) r->shape = Fnv(r->shape, ctrl + size_t(i) * sz_PatchCtrl, 12);
                return true;
            }
            const BYTE* fb = *reinterpret_cast<const BYTE* const*>(b + off_Brush_FacesBegin);
            const BYTE* fe = *reinterpret_cast<const BYTE* const*>(b + off_Brush_FacesEnd);
            size_t n = fb && fe > fb ? size_t(fe - fb) / sz_Face : 0;
            r->faces = int(n > size_t(kMaxFaces) ? kMaxFaces : n);
            for (int i = 0; i < r->faces; ++i)
            {
                const BYTE* f = fb + size_t(i) * sz_Face;
                ReadStdString(f + off_Face_Material, r->f[i].mat, sizeof r->f[i].mat);
                ReadStdString(f + off_Face_Lightmap, r->f[i].lm, sizeof r->f[i].lm);
                r->shape = Fnv(r->shape, f, 36);   // plane points
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    enum Kind { kBrush, kPatch, kPoint };
    struct Obj
    {
        std::string key, brushGuid, entGuid, cls;
        int kind = kBrush;
        float mins[3] = {}, maxs[3] = {};
        std::vector<std::string> mats, lms;
        std::string pmat, plm, layer;
        bool mesh = false; int rows = 0, cols = 0, prim = 0;
        uint64_t shape = 0;
        bool haveKeys = false; rp::KeyValues keys;
        std::string Guid() const { return kind == kPoint ? entGuid : brushGuid; }
    };
    struct KeyCache { std::unordered_map<void*, std::pair<bool, rp::KeyValues>> map; };

    const rp::KeyValues* EntityKeys(void* e, bool fixed, KeyCache& cache)
    {
        auto it = cache.map.find(e);
        if (it != cache.map.end()) return it->second.first ? &it->second.second : nullptr;
        auto& slot = cache.map[e];
        size_t brushes = 0;
        if (!fixed) rp::ForEachEntityBrush(e, [&](ModToolsBrush) { ++brushes; });
        std::string text;
        if (brushes <= kMaxKeyBrushes && rp::WriteEntityText(e, text)) { slot.first = true; slot.second = rp::ParseKeys(text); }
        return slot.first ? &slot.second : nullptr;
    }

    bool ReadObj(ModToolsBrushInst inst, KeyCache& cache, Obj& o)
    {
        ModToolsBrush b = rp::BrushOfInst(inst);
        if (!b) return false;
        ModToolsEntity e = rp::host->BrushOwner(rp::self, b);
        ModToolsEntity world = rp::host->WorldEntity(rp::self);
        bool fixed = e && rp::EntityFixedSize(e);
        if (!ReadBrushSeh(static_cast<const BYTE*>(b), &g_raw, !fixed)) return false;
        o.brushGuid = rp::BrushGuid(b);
        o.entGuid = e ? rp::EntityGuid(e) : std::string();
        o.kind = fixed ? kPoint : g_raw.patch ? kPatch : kBrush;
        o.key = fixed ? "E" + o.entGuid : o.brushGuid;
        memcpy(o.mins, g_raw.mins, 12); memcpy(o.maxs, g_raw.maxs, 12);
        o.layer = g_raw.layer; o.prim = g_raw.type; o.shape = g_raw.shape;
        o.mesh = g_raw.mesh != 0; o.rows = g_raw.rows; o.cols = g_raw.cols;
        o.pmat = g_raw.pmat; o.plm = g_raw.plm;
        for (int i = 0; i < g_raw.faces; ++i) { o.mats.push_back(g_raw.f[i].mat); o.lms.push_back(g_raw.f[i].lm); }
        if (e && e != world)
        {
            if (const rp::KeyValues* kv = EntityKeys(e, fixed, cache)) { o.haveKeys = true; o.keys = *kv; o.cls = rp::FindKey(*kv, "classname"); }
        }
        else o.cls = "worldspawn";
        return true;
    }

    struct Snap { std::vector<Obj> objs; size_t selected = 0; size_t mapObjects = 0; };

    size_t CountMapObjects()
    {
        size_t n = 0;
        rp::ForEachEntity([&](ModToolsEntity e) { ++n; rp::ForEachEntityBrush(e, [&](ModToolsBrush) { ++n; }); });
        return n;
    }
    Snap TakeSnap(bool countMap = true)
    {
        Snap s;
        std::vector<ModToolsBrushInst> insts = rp::SelectedBrushInsts();
        s.selected = insts.size();
        if (countMap) s.mapObjects = CountMapObjects() + insts.size();   // selected brushes are unlinked from their entity's list
        KeyCache cache;
        std::unordered_set<std::string> seen;
        for (size_t i = 0; i < insts.size() && s.objs.size() < kMaxObjects; ++i)
        {
            Obj o;
            if (!ReadObj(insts[i], cache, o) || !seen.insert(o.key).second) continue;
            s.objs.push_back(std::move(o));
        }
        return s;
    }

    // "brush {GUID} material m" / "patch {GUID} (mesh 3x5) material m" / "script_model {GUID} model=x at (0 0 0)"
    std::string MaterialSummary(const std::vector<std::string>& mats)
    {
        std::map<std::string, int> count;
        for (const auto& m : mats) ++count[m];
        if (count.size() == 1) return count.begin()->first;
        std::string s;
        for (const auto& c : count) { if (!s.empty()) s += ", "; s += c.first + " x" + std::to_string(c.second); }
        return s;
    }
    std::string Describe(const Obj& o)
    {
        std::string d;
        if (o.kind == kPoint)
        {
            d = (o.cls.empty() ? std::string("entity") : o.cls) + " " + o.entGuid;
            std::string model = rp::FindKey(o.keys, "model"), origin = rp::FindKey(o.keys, "origin");
            if (!model.empty()) d += " model=" + model;
            if (!origin.empty()) d += " at (" + origin + ")";
            return d;
        }
        if (o.kind == kPatch)
            d = "patch " + o.brushGuid + " (" + (o.mesh ? "mesh " : "curve ") + std::to_string(o.rows) + "x" + std::to_string(o.cols) +
                ") material " + o.pmat + (o.plm.empty() ? "" : ", lightmap " + o.plm);
        else
        {
            const char* what = o.prim == 1 ? "physics box " : o.prim == 2 ? "physics cylinder " : "brush ";
            d = what + o.brushGuid + " (" + std::to_string(o.mats.size()) + " faces) material " + MaterialSummary(o.mats);
            std::string lm = MaterialSummary(o.lms);
            if (!lm.empty()) d += ", lightmap " + lm;
        }
        if (o.cls != "worldspawn") d += ", in " + (o.cls.empty() ? std::string("entity") : o.cls) + " " + o.entGuid;
        return d;
    }
    std::string ShortDescribe(const Obj& o)
    {
        if (o.kind == kPoint) return (o.cls.empty() ? std::string("entity") : o.cls) + " " + ShortGuid(o.entGuid);
        return std::string(o.kind == kPatch ? "patch " : "brush ") + ShortGuid(o.brushGuid);
    }

    // per-index changes of one face slot: "material a -> b on 2 of 6 faces"
    void FaceSlotDiff(const char* slot, const std::vector<std::string>& a, const std::vector<std::string>& b, std::vector<std::string>& out)
    {
        std::map<std::pair<std::string, std::string>, int> changes;
        size_t n = std::min(a.size(), b.size());
        for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) ++changes[{ a[i], b[i] }];
        for (const auto& c : changes)
            out.push_back(std::string(slot) + " " + (c.first.first.empty() ? "(none)" : c.first.first) + " -> " +
                          (c.first.second.empty() ? "(none)" : c.first.second) + " on " + std::to_string(c.second) + " of " + std::to_string(n) + " faces");
    }

    struct Hints
    {
        std::map<std::string, std::vector<std::string>> moves;    // offset text -> guids
        std::map<std::string, std::vector<std::string>> layers;   // layer -> guids
        std::vector<std::string> keyEdits, deleted, created;
        bool material = false, shape = false, owner = false;
    };

    bool Same(float a, float b) { return std::fabs(a - b) < 0.01f; }

    void DiffObj(const Obj& a, const Obj& b, std::vector<std::string>& out, Hints& h)
    {
        float size0[3], size1[3], d[3];
        for (int i = 0; i < 3; ++i) { size0[i] = a.maxs[i] - a.mins[i]; size1[i] = b.maxs[i] - b.mins[i]; d[i] = b.mins[i] - a.mins[i]; }
        bool sameSize = Same(size0[0], size1[0]) && Same(size0[1], size1[1]) && Same(size0[2], size1[2]);
        bool moved = sameSize && !(Same(d[0], 0) && Same(d[1], 0) && Same(d[2], 0));
        if (moved)
        {
            out.push_back("moved by (" + Vec(d) + ")");
            h.moves[Vec(d, ",")].push_back(b.Guid());
        }
        else if (!sameSize || !(Same(d[0], 0) && Same(d[1], 0) && Same(d[2], 0)))
        {
            out.push_back("bounds (" + Vec(a.mins) + ")..(" + Vec(a.maxs) + ") -> (" + Vec(b.mins) + ")..(" + Vec(b.maxs) + ")");
            h.shape = true;
        }
        else if (a.kind != kPoint && a.shape != b.shape) { out.push_back("shape changed inside the same bounds"); h.shape = true; }
        if (a.kind == kBrush && b.kind == kBrush)
        {
            if (a.mats.size() != b.mats.size()) { out.push_back("faces " + std::to_string(a.mats.size()) + " -> " + std::to_string(b.mats.size())); h.shape = true; }
            size_t before = out.size();
            FaceSlotDiff("material", a.mats, b.mats, out);
            FaceSlotDiff("lightmap", a.lms, b.lms, out);
            if (out.size() != before) h.material = true;
        }
        if (a.kind == kPatch && b.kind == kPatch)
        {
            if (a.pmat != b.pmat) { out.push_back("material " + a.pmat + " -> " + b.pmat); h.material = true; }
            if (a.plm != b.plm) { out.push_back("lightmap " + a.plm + " -> " + b.plm); h.material = true; }
            if (a.mesh != b.mesh) { out.push_back(std::string("type ") + (a.mesh ? "mesh" : "curve") + " -> " + (b.mesh ? "mesh" : "curve")); h.shape = true; }
            if (a.rows != b.rows || a.cols != b.cols)
            { out.push_back("size " + std::to_string(a.rows) + "x" + std::to_string(a.cols) + " -> " + std::to_string(b.rows) + "x" + std::to_string(b.cols)); h.shape = true; }
        }
        if (a.kind != b.kind) { out.push_back("kind changed"); h.shape = true; }
        if (a.layer != b.layer)
        {
            out.push_back("layer " + (a.layer.empty() ? std::string("(default)") : a.layer) + " -> " + (b.layer.empty() ? std::string("(default)") : b.layer));
            h.layers[b.layer].push_back(b.Guid());
        }
        if (a.kind != kPoint && a.entGuid != b.entGuid)
        {
            out.push_back("owner " + a.cls + " " + ShortGuid(a.entGuid) + " -> " + b.cls + " " + ShortGuid(b.entGuid));
            h.owner = true;
        }
        if (a.haveKeys && b.haveKeys && a.entGuid == b.entGuid)
        {
            std::string set;
            std::vector<std::string> removedKeys;
            for (const auto& kv : b.keys)
            {
                if (kv.first == "origin" && moved && b.kind == kPoint) continue;
                std::string old = rp::FindKey(a.keys, kv.first, "\x01");
                if (old == kv.second) continue;
                out.push_back(old == "\x01" ? "key " + kv.first + " = \"" + kv.second + "\" (new)" : "key " + kv.first + " \"" + old + "\" -> \"" + kv.second + "\"");
                if (!set.empty()) set += ",";
                set += JsonStr(kv.first) + ":" + JsonStr(kv.second);
            }
            for (const auto& kv : a.keys)
                if (rp::FindKey(b.keys, kv.first, "\x01") == "\x01") { out.push_back("key " + kv.first + " removed"); removedKeys.push_back(kv.first); }
        }
    }


    // ---- the open undo step ---------------------------------------------------------------------------------------
    struct Step { bool open = false, fromLast = false; std::string label, source; Snap before; uint64_t start = 0; };
    Step g_step;
    std::vector<void*> g_sel;            // the selection the poll last saw (sorted BrushInst pointers: the cheap change test)
    Snap g_last;                         // its objects, compared by GUID (an undo or a re-select makes new BrushInsts)
    uint64_t g_nextPoll = 0;

    std::vector<void*> SortedSelection()
    {
        std::vector<ModToolsBrushInst> insts = rp::SelectedBrushInsts();
        std::vector<void*> v(insts.begin(), insts.end());
        std::sort(v.begin(), v.end());
        return v;
    }

    void StepBegin(const char* label)
    {
        g_step = Step();
        g_step.open = true;
        g_step.label = label ? label : "";
        g_step.start = GetTickCount64();
        if (g_actDepth > 0 && GetTickCount64() - g_act.start < 1500) { g_act.step = true; g_step.source = ActionSource(g_act); }
        else g_step.source = InputSource();
        g_step.before = TakeSnap();
        // code that clears the selection first (a plugin) still compares against the last selection
        if (g_step.before.objs.empty() && !g_last.objs.empty()) { g_step.before.objs = g_last.objs; g_step.fromLast = true; }
    }

    void StepEnd()
    {
        Step step = std::move(g_step);
        g_step = Step();
        Snap after = TakeSnap();
        g_sel = SortedSelection();
        g_last.objs = after.objs; g_last.selected = after.selected;

        std::unordered_map<std::string, const Obj*> was, now;
        for (const Obj& o : step.before.objs) was[o.key] = &o;
        for (const Obj& o : after.objs) now[o.key] = &o;

        std::vector<std::string> body;
        Hints h;
        auto addObj = [&](const std::string& head, const std::vector<std::string>& changes)
        {
            std::string line = "  " + head;
            for (size_t i = 0; i < changes.size(); ++i) line += (i ? "; " : ": ") + changes[i];
            body.push_back(line);
        };
        // changed
        for (const Obj& b : after.objs)
        {
            auto it = was.find(b.key);
            if (it == was.end()) continue;
            std::vector<std::string> changes;
            DiffObj(*it->second, b, changes, h);
            if (!changes.empty()) addObj(ShortDescribe(b), changes);
        }
        // deleted, or only deselected
        std::vector<const Obj*> gone;
        for (const Obj& a : step.before.objs) if (!now.count(a.key)) gone.push_back(&a);
        size_t deselected = 0;
        if (!gone.empty())
        {
            std::vector<std::string> brushGuids, entGuids;
            for (const Obj* o : gone) (o->kind == kPoint ? entGuids : brushGuids).push_back(o->kind == kPoint ? o->entGuid : o->brushGuid);
            std::vector<ModToolsBrush> fb = brushGuids.empty() ? std::vector<ModToolsBrush>() : rp::FindBrushesByGuid(brushGuids);
            std::vector<ModToolsEntity> fe = entGuids.empty() ? std::vector<ModToolsEntity>() : rp::FindEntitiesByGuid(entGuids);
            size_t bi = 0, ei = 0;
            for (const Obj* o : gone)
            {
                bool exists = o->kind == kPoint ? (ei < fe.size() && fe[ei++] != nullptr) : (bi < fb.size() && fb[bi++] != nullptr);
                if (exists) { ++deselected; continue; }
                body.push_back("  deleted " + Describe(*o));
                h.deleted.push_back(o->Guid());
            }
        }
        // created, or only selected
        size_t newlySelected = 0;
        bool grew = after.mapObjects > step.before.mapObjects;
        for (const Obj& b : after.objs)
        {
            if (was.count(b.key)) continue;
            if (!grew) { ++newlySelected; continue; }
            body.push_back("  new " + Describe(b));
        }
        if (deselected && !step.fromLast) body.push_back("  " + std::to_string(deselected) + " deselected");
        if (newlySelected) body.push_back("  " + std::to_string(newlySelected) + " now selected (existed before)");
        if (step.before.selected > kMaxObjects || after.selected > kMaxObjects)
            body.push_back("  (only the first " + std::to_string(kMaxObjects) + " selected objects were compared)");


        size_t changed = body.size();
        std::string head = "[action] edit \"" + step.label + "\" via " + step.source + ", " + std::to_string(after.selected) + " selected";
        std::string text = head;
        if (!changed) text += "\n  no change seen in the selected objects (face selections and settings are not compared)";
        for (const auto& l : body) text += "\n" + l;
        Emit(text);
    }

    // ---- hooks ----------------------------------------------------------------------------------------------------
    void StepBeginGuarded(const char* label)
    {
        try { StepBegin(label); }
        catch (...) { g_step = Step(); }
    }
    void StepEndGuarded()
    {
        try { StepEnd(); }
        catch (...) { g_step = Step(); }
    }
    bool g_faulted = false;
    void FaultOff(const char* where)
    {
        g_faulted = true; g_on = false; g_step = Step();
        rp::Log("actionlog: a fault in %s; the log is off until it is switched on again", where);
    }
    void SehBegin(const char* label)
    {
        __try { StepBeginGuarded(label); }
        __except (EXCEPTION_EXECUTE_HANDLER) { FaultOff("Undo_Begin"); }
    }
    void SehEnd()
    {
        __try { StepEndGuarded(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { FaultOff("Undo_End"); }
    }

    bool Busy() { return rp::host->IsReloading && rp::host->IsReloading(rp::self); }

    void hk_UndoBegin(const char* label)
    {
        if (g_on && !g_step.open && !Busy()) SehBegin(label);
        g_oUndoBegin(label);
    }
    void hk_UndoEnd()
    {
        g_oUndoEnd();
        if (g_on && g_step.open && !Busy()) SehEnd();
        else g_step.open = false;
    }

    void ActionEnter(void* action)
    {
        g_act = ActionCtx();
        g_act.path = ActionPath(action);
        g_act.keys = ActionShortcut(action);
        g_act.start = GetTickCount64();
        g_act.keyHeld = !HeldKey().empty();
    }
    void ActionLeave()
    {
        if (g_act.step || g_act.path.empty()) return;
        std::string text = "[action] " + ActionSource(g_act) + ", no undo step";
        Emit(text);
    }
    void ActionEnterGuarded(void* action) { try { ActionEnter(action); } catch (...) { g_act = ActionCtx(); } }
    void ActionLeaveGuarded() { try { ActionLeave(); } catch (...) {} }
    void SehActionEnter(void* action)
    {
        __try { ActionEnterGuarded(action); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_act.path.clear(); g_act.step = true; }
    }
    void SehActionLeave()
    {
        __try { ActionLeaveGuarded(); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    void hk_Activate(void* action, int event)
    {
        if (event != 0 || !g_on || !action) { g_oActivate(action, event); return; }   // 0 = QAction::Trigger
        ActionCtx outer = g_act;
        SehActionEnter(action);
        ++g_actDepth;
        struct Restore { ActionCtx outer; ~Restore() { --g_actDepth; } } restore{ outer };
        g_oActivate(action, event);
        if (g_on) SehActionLeave();
        g_act = restore.outer;
    }

    bool InstallHooks()
    {
        if (g_hooked) return true;
        if (g_hookFailed) return false;
        void* begin = rp::host->RadiantAddress(rp::self, kUndoBegin);
        void* end = rp::host->RadiantAddress(rp::self, kUndoEnd);
        HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll");
        void* activate = w ? reinterpret_cast<void*>(GetProcAddress(w, "?activate@QAction@@QEAAXW4ActionEvent@1@@Z")) : nullptr;
        if (!begin || !end || !g_sysPrintf || !rp::host->Detour)
        { g_hookFailed = true; rp::Log("actionlog: Radiant's undo or print functions were not found; the log stays off"); return false; }
        if (!rp::host->Detour(rp::self, begin, reinterpret_cast<void*>(&hk_UndoBegin), reinterpret_cast<void**>(&g_oUndoBegin)) ||
            !rp::host->Detour(rp::self, end, reinterpret_cast<void*>(&hk_UndoEnd), reinterpret_cast<void**>(&g_oUndoEnd)))
        { g_hookFailed = true; rp::Log("actionlog: the undo detours failed (another plugin hooks Undo_Begin/Undo_End?); the log stays off"); return false; }
        if (!activate || !rp::host->Detour(rp::self, activate, reinterpret_cast<void*>(&hk_Activate), reinterpret_cast<void**>(&g_oActivate)))
            rp::Log("actionlog: QAction::activate not hooked; menu entries and shortcuts are not named");
        g_hooked = true;
        rp::Log("actionlog: hooks installed (Undo_Begin, Undo_End%s)", g_oActivate ? ", QAction::activate" : "");
        return true;
    }

    // ---- the Output pane check box and the menu entry -------------------------------------------------------------
    void* g_box = nullptr;               // our QCheckBox beside Clear
    uint64_t g_nextBoxTry = 0;
    int g_boxTries = 0;

    void SyncControls()
    {
        if (g_box && rp::host->ui) rp::host->ui->SetChecked(g_box, g_display ? 1 : 0);
    }
    void OnBoxClicked(void*) { SetEnabled(rp::host->ui->IsChecked(g_box) != 0); }

    void TryAddBox()
    {
        const ModToolsUiApi* ui = rp::host->ui;
        if (g_box || !ui || !ui->FindEditorWidget || !ui->FindChild || !qt.parentWidget || !qt.layout || !qt.insertWidget) return;
        void* pane = ui->FindEditorWidget("Output Window");
        void* clear = pane ? ui->FindChild(pane, "QPushButton", 0) : nullptr;
        void* row = clear ? qt.parentWidget(clear) : nullptr;
        void* box = row ? qt.layout(row) : nullptr;
        if (!box || !IsA(box, "QBoxLayout")) return;
        void* cb = ui->CheckBox("Log actions", row);
        if (!cb) return;
        ui->SetToolTip(cb, "Radiant+: print every selection and edit here: what changed, how it was done "
                           "(menu, shortcut, mouse in which view)"
                           );
        qt.insertWidget(box, 1, cb, 0, 0);   // after the stretch, before Clear
        g_box = cb;
        if (rp::host->OnClicked) rp::host->OnClicked(rp::self, cb, OnBoxClicked, nullptr);
        SyncControls();
        rp::Log("actionlog: check box added to the Output pane");
    }

    bool DeleteNowSeh(void* obj)
    {
        __try { (*reinterpret_cast<void (__fastcall***)(void*, unsigned)>(obj))[3](obj, 1); return true; }   // scalar deleting destructor
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }


    // ---- selection poll -------------------------------------------------------------------------------------------
    void PollSelection()
    {
        std::vector<void*> sel = SortedSelection();
        if (sel == g_sel) return;
        g_sel = sel;
        Snap now = TakeSnap(false);
        std::unordered_set<std::string> before, after;
        for (const Obj& o : g_last.objs) before.insert(o.key);
        for (const Obj& o : now.objs) after.insert(o.key);
        std::vector<const Obj*> added;
        for (const Obj& o : now.objs) if (!before.count(o.key)) added.push_back(&o);
        size_t removed = 0;
        for (const std::string& k : before) if (!after.count(k)) ++removed;
        size_t lastCount = g_last.selected;
        g_last = std::move(now);
        if (added.empty() && removed == 0 && g_last.selected == lastCount) return;   // the same objects, new BrushInsts
        std::string where = Where(WidgetUnderCursor());
        std::string tail = where.empty() ? std::string() : " (cursor over " + where + ")";
        if (sel.empty()) { Emit("[action] selection cleared" + tail); return; }
        if (added.empty()) { Emit("[action] deselected " + std::to_string(removed) + ", " + std::to_string(sel.size()) + " still selected" + tail); return; }
        std::vector<std::string> lines, guids;
        for (const Obj* o : added)
        {
            if (guids.size() < 64) guids.push_back(o->Guid());
            if (lines.size() < 4) lines.push_back("  " + Describe(*o));
        }
        if (added.size() > lines.size()) lines.push_back("  ... and " + std::to_string(added.size() - lines.size()) + " more");
        std::string text = "[action] selected " + std::to_string(added.size()) + (removed ? " (replacing " + std::to_string(removed) + ")" : "") +
                           ", " + std::to_string(sel.size()) + " selected" + tail;
        for (const auto& l : lines) text += "\n" + l;
        Emit(text);
    }
    void PollGuarded() { try { PollSelection(); } catch (...) {} }
    void SehPoll()
    {
        __try { PollGuarded(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { FaultOff("the selection poll"); }
    }

    void Flush()
    {
        while (!g_print.empty())
        {
            std::string t = std::move(g_print.front());
            g_print.pop_front();
            if (g_sysPrintf) g_sysPrintf("%s\n", t.c_str());
        }
    }

    // "probe": the raw strings of the first selected brush, to check the field offsets against a Ctrl+C of it
    std::string Probe()
    {
        std::vector<ModToolsBrushInst> insts = rp::SelectedBrushInsts();
        if (insts.empty()) return "select a brush or patch first";
        KeyCache cache; Obj o;
        if (!ReadObj(insts[0], cache, o)) return "could not read the first selected object";
        std::string s = Describe(o) + "\nlayer '" + o.layer + "' prim " + std::to_string(o.prim) + " bounds (" + Vec(o.mins) + ")..(" + Vec(o.maxs) + ")";
        for (size_t i = 0; i < o.mats.size(); ++i) s += "\nface " + std::to_string(i) + ": '" + o.mats[i] + "' / '" + o.lms[i] + "'";
        return s;
    }
}

// Recording runs while the log is shown or kept for problem reports, the hooks go in the first time either is on.
static void UpdateActive()
{
    bool want = (g_display || g_inReports) && InstallHooks();
    if (want && !g_on) { g_faulted = false; g_step = Step(); g_sel = SortedSelection(); g_last = TakeSnap(false); }
    if (!want) { g_step = Step(); g_print.clear(); }
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
    Get(qt.layout, w, "?layout@QWidget@@QEBAPEAVQLayout@@XZ");
    Get(qt.insertWidget, w, "?insertWidget@QBoxLayout@@QEAAXHPEAVQWidget@@HV?$QFlags@W4AlignmentFlag@Qt@@@@@Z");
    Get(qt.focusWidget, w, "?focusWidget@QApplication@@SAPEAVQWidget@@XZ");
    Get(qt.widgetAt, w, "?widgetAt@QApplication@@SAPEAVQWidget@@AEBVQPoint@@@Z");
    Get(qt.dispose, c, "?dispose@QListData@@SAXPEAUData@1@@Z");
    Get(qt.stringDtor, c, "??1QString@@QEAA@XZ");
    Get(qt.className, c, "?className@QMetaObject@@QEBAPEBDXZ");
    Get(qt.keySeqString, g, "?toString@QKeySequence@@QEBA?AVQString@@W4SequenceFormat@1@@Z");
    Get(qt.keySeqDtor, g, "??1QKeySequence@@QEAA@XZ");
    g_sysPrintf = At<SysPrintfFn>(kSysPrintf);
    int maxLines = rp::ConfigGetInt("actionlog", "max_lines", 2000);
    g_ringMax = size_t(maxLines < 100 ? 100 : maxLines > 100000 ? 100000 : maxLines);
    g_inReports = rp::ConfigGetBool("actionlog", "in_reports", true);
    if (!qt.text || !qt.stringDtor || !qt.className) rp::Log("actionlog: some Qt functions were not found (text %d, QString dtor %d, className %d)",
                                                             qt.text != nullptr, qt.stringDtor != nullptr, qt.className != nullptr);
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
        if (!g_display) g_print.clear();
        const char* line = g_display ? "[action] action log shown (Radiant+): selections and edits are printed here"
                         : g_on ? "[action] action log hidden (still kept for problem reports; [actionlog] in_reports=0 stops that)"
                                : "[action] action log off";
        if (g_sysPrintf) g_sysPrintf("%s\n", line);
    }
    SyncControls();
}

void Tick()
{
    uint64_t now = GetTickCount64();
    if (g_inReports && g_reportDirty && now >= g_nextReportWrite) { g_nextReportWrite = now + 2000; WriteReportFile(); }
    if (!g_box && g_boxTries < 120 && now >= g_nextBoxTry) { ++g_boxTries; g_nextBoxTry = now + 1000; TryAddBox(); }
    if (!g_on) return;
    if (!g_step.open && !Busy() && now >= g_nextPoll) { g_nextPoll = now + 150; SehPoll(); }
    if (!g_step.open) Flush();
}

void Shutdown()
{
    if (g_inReports && g_reportDirty) WriteReportFile();
    g_on = false;
    g_step = Step();
    g_print.clear();
    if (g_box)
    {
        if (!DeleteNowSeh(g_box) && rp::host->ui) rp::host->ui->SetVisible(g_box, 0);
        g_box = nullptr;
    }
    g_hooked = false;                   // the host removes the detours
}

bool Command(const std::string& command, std::string& reply)
{
    if (command.compare(0, 9, "actionlog") != 0) return false;
    std::string arg = command.size() > 9 ? command.substr(10) : std::string();
    if (arg.empty() || arg == "status")
    {
        reply = std::string("actionlog ") + (g_display ? "shown" : "hidden") + (g_on ? ", recording" : ", not recording") +
                (g_on && g_inReports ? " (kept for problem reports)" : "") + (g_hooked ? ", hooked" : "") + (g_faulted ? ", faulted" : "") +
                ", " + std::to_string(g_ring.size()) + " entries, last seq " + std::to_string(g_seq) + (g_box ? ", check box in the Output pane" : "");
        return true;
    }
    if (arg == "1" || arg == "on") { SetEnabled(true); reply = g_display ? "actionlog shown" : "actionlog could not be switched on (see the log)"; return true; }
    if (arg == "0" || arg == "off") { SetEnabled(false); reply = g_on ? "actionlog hidden, still recording for problem reports" : "actionlog off"; return true; }
    if (arg == "reports 1" || arg == "reports 0")
    {
        g_inReports = arg.back() == '1';
        rp::ConfigSet("actionlog", "in_reports", g_inReports ? "1" : "0");
        if (g_inReports) { g_reportDirty = true; g_nextReportWrite = 0; }
        else DeleteFileW(ReportPath().c_str());
        UpdateActive();
        reply = g_inReports ? "actionlog entries kept for problem reports (actionlog.log in the plugin folder)" : "actionlog entries no longer kept for problem reports (actionlog.log deleted)";
        return true;
    }
    if (arg == "clear") { g_ring.clear(); reply = "actionlog cleared, last seq " + std::to_string(g_seq); return true; }
    if (arg == "probe") { reply = Probe(); return true; }
    if (arg.compare(0, 8, "trigger ") == 0)   // fire a menu entry the way a click does (tests the "via menu" path)
    {
        int r = rp::Has(&rp::host->TriggerMenuAction) && rp::host->TriggerMenuAction ? rp::host->TriggerMenuAction(rp::self, arg.c_str() + 8) : 0;
        reply = r == 1 ? "triggered" : r == -1 ? "that menu entry is disabled" : "no such menu entry";
        return true;
    }
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
    reply = "actionlog [0|1|status|tail [n]|since <seq>|clear|probe|reports 0|1|trigger <menu path>]";
    return true;
}
}
