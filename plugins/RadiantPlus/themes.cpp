// Radiant themes and view color presets, in Radiant's Preferences dialog. Radiant has one look (InitInstance 0x140BA8970 sets plastique + stylesheet.qss once),
// this applies another style + sheet to the whole application and adds a "Theme:" row and a "Theme" editor tab to Preferences (ctor detoured).
// Color presets fill Radiant's 48 view colors (float[48][4]) from bin\themes\<name>.colors.
#include "modtools_plugin.hpp"
#include "themes.h"
#include "rotgizmo.h"
#include "gamefov.h"
#include "dxvk.h"
#include "discordrpc.h"
#include "filedialogs.h"
#include "themeicons.h"
#include "themeeditor.h"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#pragma comment(lib, "shell32.lib")

namespace br::theme
{
namespace
{
    // IDA addresses (Radiant_modtools.exe, no ASLR), resolved through host->RadiantAddress
    const uintptr_t kPrefsCtor = 0x1409576C0;          // QtRadiantPreferences::QtRadiantPreferences(this, QWidget* parent), Ui at +0x38
    const size_t off_Prefs_Ui = 0x38;                  // Ui::QtRadiantPreferences* (0xA68 bytes, setupUi 0x14096ADD0)
    const size_t off_Ui_Reset = 0x128;                 // QPushButton* "Reset Preferences to default" (System page)
    const size_t off_Ui_Ok = 0xA50;                    // QPushButton* OK (-> OkPressed)
    const size_t off_Ui_Apply = 0xA60;                 // QPushButton* Apply (-> ApplyPressed)
    // view colors
    const uintptr_t kColorTable = 0x1453A14A8;         // float[48][4], index order = the names below (SavedInfo block 0x1453A14A0 + 8)
    const uintptr_t kPrefsBlock = 0x1453A1C20;         // the prefs block every tab's load function gets as its 2nd argument
    const uintptr_t kPrefsInit = 0x1409578E0;          // QtRadiantPreferences::Init(this): every tab's widgets from the prefs
    const uintptr_t kColorTabLoad = 0x140958B60;       // (dialog, prefs): each Color swatch <- table[index] (8 bits per channel)
    const uintptr_t kColorTabStore = 0x140959D10;      // (dialog): table[index] <- each Color swatch (OK/Apply)
    const size_t off_Ui_SwatchCameraBack = 0x980;      // the COLOR_CAMERABACK swatch (any swatch finds the Color tab)
    const uintptr_t kDockToggleView = 0x140E89040;     // TADockWidget::toggleView(this, bool show): show/raise a pane (tests)

    // ---- Qt 5.3 exports, no headers (MS x64 ABI: QString returned through a hidden pointer) ----
    struct QListDataD { int ref; int alloc; int begin; int end; void* array[1]; };
    struct QArrayDataRaw { int ref; int size; unsigned alloc; ptrdiff_t offset; };
    struct QMetaObjectRaw;
    struct Qt
    {
        bool bound = false;
        const char* (*className)(const QMetaObjectRaw*) = nullptr;
        const QMetaObjectRaw* (*superClass)(const QMetaObjectRaw*) = nullptr;
        void (*qsDtor)(void**) = nullptr;
        void* (*qstringFromUtf8)(void*, const char*, int) = nullptr;
        void* (*appInstance)() = nullptr;
        void* (*appSetStyle)(const void*) = nullptr;                    // QApplication::setStyle(const QString&) -> QStyle*
        void (*appSetStyleSheet)(void*, const void*) = nullptr;
        void* (*opNew)(size_t) = nullptr;                               // msvcr110 operator new (Qt deletes with it)
        void* (*comboCtor)(void*, void*) = nullptr;
        void (*comboAddItem)(void*, const void*, const void*) = nullptr; // addItem(const QString&, const QVariant&)
        void (*comboSetCurrentIndex)(void*, int) = nullptr;
        int (*comboCurrentIndex)(const void*) = nullptr;
        int (*comboCount)(const void*) = nullptr;
        void** (*comboItemText)(const void*, void**, int) = nullptr;
        void* (*variantCtor)(void*) = nullptr;
        void (*variantDtor)(void*) = nullptr;
        void** (*buttonText)(const void*, void**) = nullptr;
        void* (*parentWidget)(const void*) = nullptr;
        void* (*widgetLayout)(const void*) = nullptr;
        int (*layoutIndexOf)(const void*, void*) = nullptr;
        void (*boxInsertLayout)(void*, int, void*, int) = nullptr;
        void (*gridAddLayout)(void*, void*, int, int, int, int, int) = nullptr;   // addLayout(QLayout*, row, col, rowSpan, colSpan, Qt::Alignment)
        int (*gridRowCount)(const void*) = nullptr;
        void* (*layoutReplaceWidget)(void*, void*, void*, int) = nullptr;   // QLayout::replaceWidget(from, to, Qt::FindChildOptions) -> QLayoutItem* (the caller's)
        int (*gridColumnCount)(const void*) = nullptr;
        bool (*isVisibleTo)(const void*, const void*) = nullptr;
        void (*deleteLater)(void*) = nullptr;
        const int* (*geometry)(const void*) = nullptr;                  // const QRect& (x1, y1, x2, y2)
        void (*setGeometry)(void*, int, int, int, int) = nullptr;
        void* (*children)(const void*) = nullptr;                       // const QList<QObject*>&
        void** (*objectName)(const void*, void**) = nullptr;
        // self test only (optional)
        void (*adjustSize)(void*) = nullptr;
        void* (*grab)(void*, void*, const int*) = nullptr;               // QPixmap grab(const QRect&), hidden return pointer
        bool (*pixmapSave)(const void*, const void*, const char*, int) = nullptr;
        void (*pixmapDtor)(void*) = nullptr;
        // view color presets (optional): QInputDialog::getText(parent, title, label, echo, text, bool* ok, flags, hints), static
        void** (*getText)(void**, void*, const void*, const void*, int, const void*, bool*, int, int) = nullptr;
        // relayout after a theme change (optional)
        void** (*allWidgets)(void**) = nullptr;                       // static QWidgetList QApplication::allWidgets(), hidden return
        void (*listDispose)(void*) = nullptr;                          // QListData::dispose(Data*)
        void (*layoutInvalidate)(void*) = nullptr;                    // only to verify the vtable slot (see LayoutInvalidate)
        void* boxInvalidate = nullptr;
        bool (*layoutActivate)(void*) = nullptr;
        void (*updateGeometry)(void*) = nullptr;
        // Theme editor (optional)
        void (*setStyleSheet)(void*, const void*) = nullptr;           // QWidget::setStyleSheet(const QString&)
        void (*comboClear)(void*) = nullptr;
        int (*tabInsert)(void*, int, void*, const void*) = nullptr;     // QTabWidget::insertTab(int, QWidget*, const QString&)
        void (*tabSetCurrent)(void*, int) = nullptr;
        int (*tabIndexOf)(const void*, void*) = nullptr;                // QTabWidget::indexOf(QWidget*)
        void** (*tabText)(const void*, void**, int) = nullptr;          // QTabWidget::tabText(int), QString by hidden pointer
        // fast base-style change (optional, see RestyleOnce)
        void* (*widgetStyle)(const void*) = nullptr;                    // QWidget::style(): its own, else the application's
        void* (*appStyle)() = nullptr;
        bool (*testAttribute)(const void*, int) = nullptr;              // testAttribute_helper (attributes from 32 up)
        void (*setAttribute)(void*, int, bool) = nullptr;
    } qt;

    template <class T> bool Get(T& fn, HMODULE m, const char* name) { fn = reinterpret_cast<T>(GetProcAddress(m, name)); return fn != nullptr; }

    bool BindQt()
    {
        if (qt.bound) return true;
        HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll"), c = GetModuleHandleW(L"Qt5Core64r.dll");
        if (!w || !c) return false;
        bool ok = true;
        ok &= Get(qt.className, c, "?className@QMetaObject@@QEBAPEBDXZ");
        ok &= Get(qt.superClass, c, "?superClass@QMetaObject@@QEBAPEBU1@XZ");
        ok &= Get(qt.qsDtor, c, "??1QString@@QEAA@XZ");
        ok &= Get(qt.qstringFromUtf8, c, "?fromUtf8@QString@@SA?AV1@PEBDH@Z");
        ok &= Get(qt.appInstance, c, "?instance@QCoreApplication@@SAPEAV1@XZ");
        ok &= Get(qt.appSetStyle, w, "?setStyle@QApplication@@SAPEAVQStyle@@AEBVQString@@@Z");
        ok &= Get(qt.appSetStyleSheet, w, "?setStyleSheet@QApplication@@QEAAXAEBVQString@@@Z");
        if (HMODULE crt = GetModuleHandleW(L"msvcr110.dll")) ok &= Get(qt.opNew, crt, "??2@YAPEAX_K@Z"); else ok = false;
        ok &= Get(qt.comboCtor, w, "??0QComboBox@@QEAA@PEAVQWidget@@@Z");
        ok &= Get(qt.comboAddItem, w, "?addItem@QComboBox@@QEAAXAEBVQString@@AEBVQVariant@@@Z");
        ok &= Get(qt.comboSetCurrentIndex, w, "?setCurrentIndex@QComboBox@@QEAAXH@Z");
        ok &= Get(qt.comboCurrentIndex, w, "?currentIndex@QComboBox@@QEBAHXZ");
        ok &= Get(qt.comboCount, w, "?count@QComboBox@@QEBAHXZ");
        ok &= Get(qt.comboItemText, w, "?itemText@QComboBox@@QEBA?AVQString@@H@Z");
        ok &= Get(qt.variantCtor, c, "??0QVariant@@QEAA@XZ");
        ok &= Get(qt.variantDtor, c, "??1QVariant@@QEAA@XZ");
        ok &= Get(qt.buttonText, w, "?text@QAbstractButton@@QEBA?AVQString@@XZ");
        ok &= Get(qt.parentWidget, w, "?parentWidget@QWidget@@QEBAPEAV1@XZ");
        ok &= Get(qt.widgetLayout, w, "?layout@QWidget@@QEBAPEAVQLayout@@XZ");
        ok &= Get(qt.layoutIndexOf, w, "?indexOf@QLayout@@UEBAHPEAVQWidget@@@Z");
        ok &= Get(qt.boxInsertLayout, w, "?insertLayout@QBoxLayout@@QEAAXHPEAVQLayout@@H@Z");
        ok &= Get(qt.gridAddLayout, w, "?addLayout@QGridLayout@@QEAAXPEAVQLayout@@HHHHV?$QFlags@W4AlignmentFlag@Qt@@@@@Z");
        ok &= Get(qt.gridRowCount, w, "?rowCount@QGridLayout@@QEBAHXZ");
        ok &= Get(qt.gridColumnCount, w, "?columnCount@QGridLayout@@QEBAHXZ");
        ok &= Get(qt.isVisibleTo, w, "?isVisibleTo@QWidget@@QEBA_NPEBV1@@Z");
        ok &= Get(qt.deleteLater, c, "?deleteLater@QObject@@QEAAXXZ");
        ok &= Get(qt.geometry, w, "?geometry@QWidget@@QEBAAEBVQRect@@XZ");
        ok &= Get(qt.setGeometry, w, "?setGeometry@QWidget@@QEAAXHHHH@Z");
        ok &= Get(qt.children, c, "?children@QObject@@QEBAAEBV?$QList@PEAVQObject@@@@XZ");
        ok &= Get(qt.objectName, c, "?objectName@QObject@@QEBA?AVQString@@XZ");
        Get(qt.adjustSize, w, "?adjustSize@QWidget@@QEAAXXZ");
        Get(qt.layoutReplaceWidget, w, "?replaceWidget@QLayout@@QEAAPEAVQLayoutItem@@PEAVQWidget@@0V?$QFlags@W4FindChildOption@Qt@@@@@Z");
        Get(qt.grab, w, "?grab@QWidget@@QEAA?AVQPixmap@@AEBVQRect@@@Z");
        if (HMODULE g = GetModuleHandleW(L"Qt5Gui64r.dll")) { Get(qt.pixmapSave, g, "?save@QPixmap@@QEBA_NAEBVQString@@PEBDH@Z"); Get(qt.pixmapDtor, g, "??1QPixmap@@UEAA@XZ"); }
        Get(qt.getText, w, "?getText@QInputDialog@@SA?AVQString@@PEAVQWidget@@AEBV2@1W4EchoMode@QLineEdit@@1PEA_NV?$QFlags@W4WindowType@Qt@@@@V?$QFlags@W4InputMethodHint@Qt@@@@@Z");
        Get(qt.setStyleSheet, w, "?setStyleSheet@QWidget@@QEAAXAEBVQString@@@Z");
        Get(qt.comboClear, w, "?clear@QComboBox@@QEAAXXZ");
        Get(qt.tabInsert, w, "?insertTab@QTabWidget@@QEAAHHPEAVQWidget@@AEBVQString@@@Z");
        Get(qt.tabSetCurrent, w, "?setCurrentIndex@QTabWidget@@QEAAXH@Z");
        Get(qt.tabIndexOf, w, "?indexOf@QTabWidget@@QEBAHPEAVQWidget@@@Z");
        Get(qt.tabText, w, "?tabText@QTabWidget@@QEBA?AVQString@@H@Z");
        Get(qt.allWidgets, w, "?allWidgets@QApplication@@SA?AV?$QList@PEAVQWidget@@@@XZ");
        Get(qt.listDispose, c, "?dispose@QListData@@SAXPEAUData@1@@Z");
        Get(qt.layoutInvalidate, w, "?invalidate@QLayout@@UEAAXXZ");
        qt.boxInvalidate = reinterpret_cast<void*>(GetProcAddress(w, "?invalidate@QBoxLayout@@UEAAXXZ"));
        Get(qt.layoutActivate, w, "?activate@QLayout@@QEAA_NXZ");
        Get(qt.updateGeometry, w, "?updateGeometry@QWidget@@QEAAXXZ");
        Get(qt.widgetStyle, w, "?style@QWidget@@QEBAPEAVQStyle@@XZ");
        Get(qt.appStyle, w, "?style@QApplication@@SAPEAVQStyle@@XZ");
        Get(qt.testAttribute, w, "?testAttribute_helper@QWidget@@AEBA_NW4WidgetAttribute@Qt@@@Z");
        Get(qt.setAttribute, w, "?setAttribute@QWidget@@QEAAXW4WidgetAttribute@Qt@@_N@Z");
        if (!ok) rp::Log("theme: some Qt exports are missing, themes are off");
        qt.bound = ok;
        return ok;
    }

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
    const QMetaObjectRaw* MetaOf(void* obj)
    {
        auto metaObject = (*reinterpret_cast<const QMetaObjectRaw* (__fastcall ***)(void*)>(obj))[0];   // QObject vtable slot 0
        return metaObject(obj);
    }
    const char* ClassName(void* obj) { const char* c = obj ? qt.className(MetaOf(obj)) : nullptr; return c ? c : "(null)"; }
    bool IsA(void* obj, const char* cls)
    {
        for (const QMetaObjectRaw* m = MetaOf(obj); m; m = qt.superClass(m))
            if (const char* c = qt.className(m)) if (strcmp(c, cls) == 0) return true;
        return false;
    }
    void* DockOf(void* w)
    {
        for (void* p = w; p; p = qt.parentWidget(p)) if (IsA(p, "TADockWidget")) return p;
        return nullptr;
    }
    // self test dialogs: deleteLater never ran from the command channel, so run the scalar deleting destructor (vtable slot 3)
    bool DeleteNow(void* obj)
    {
        __try { (*reinterpret_cast<void (__fastcall***)(void*, unsigned)>(obj))[3](obj, 1); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }


    // ---- theme list and apply ----
    struct Entry { std::string name; std::string label; std::wstring file; std::string style; bool classic; bool supplement; };
    std::vector<Entry> g_list;
    std::string g_current;                 // the ini choice ("" or "default" = Radiant's own look, nothing applied)
    std::string g_applied;                 // the look in effect
    std::string g_pending;                 // picked in the Theme combo (or OK/Apply), applied on the next tick
    bool g_started = false;
    int g_relayoutTicks = 0;               // ticks until the layouts are redone after a theme change (0 = none pending)
    bool g_reapply = false;                // the Theme editor re-saved a theme: OK applies it again
    bool g_fillingCombo = false;           // the Theme combo is being refilled from code: not a pick
    const char* const kEditedName = "*edited*";   // the Theme combo's "Edited (not saved)" entry
    double g_lastMs = 0;
    std::string g_lastError;

    using PrefsCtor = void* (__fastcall*)(void* self, void* parent);
    PrefsCtor g_origCtor = nullptr;
    void* g_ctorLive = nullptr;            // live address of the constructor (calling it runs the detour)
    bool g_hooked = false;
    int g_patched = 0;
    std::string g_patchError;
    std::string g_where;                   // where the row went (layout class, index), for status and the self test

    struct Dialog { void* dialog = nullptr; void* reset = nullptr; void* page = nullptr; void* combo = nullptr; int hooked = 0; std::vector<std::string> names; };
    Dialog g_dlg;

    uint64_t Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return uint64_t(t.QuadPart); }
    double Ms(uint64_t dt) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return double(dt) * 1000.0 / double(f.QuadPart); }
    std::wstring Root() { return rp::host->RadiantRoot(rp::self); }
    std::wstring Dir() { return Root() + L"\\bin\\themes"; }   // shared by Radiant+ and APE+
    bool FileExists(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
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
    std::string Slashes(const std::wstring& p) { std::string s = Narrow(p); for (char& ch : s) if (ch == '\\') ch = '/'; return s; }
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

    // '/* base-style: fusion */' within the first 2 KB of the sheet
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

    // url(...) references: relative paths against baseDir, ':/stylesheet/<file>' to themes\images\<file> when shipped
    std::string Resolve(const std::string& sheet, const std::wstring& baseDir)
    {
        std::string out; out.reserve(sheet.size() + 512);
        std::wstring images = Dir() + L"\\images\\";
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
            std::string repl;
            if (inner.rfind(":/stylesheet/", 0) == 0)
            {
                std::wstring f = images + Widen(inner.substr(inner.find_last_of('/') + 1));
                if (FileExists(f)) repl = Slashes(f);
            }
            else if (!inner.empty() && inner[0] != ':' && inner[0] != '/' && inner[0] != '\\' && inner.find(':') == std::string::npos)
            {
                std::wstring f = baseDir + L"\\" + Widen(inner);
                for (wchar_t& ch : f) if (ch == L'/') ch = L'\\';
                repl = Slashes(f);
            }
            if (repl.empty()) out.append(sheet, p, close + 1 - p);
            else { out += "url(\""; out += repl; out += "\")"; }
            i = close + 1;
        }
        return out;
    }

    std::vector<void*> AllWidgets();
    bool g_sheetSet = true;                // an application sheet is set (Radiant's InitInstance leaves its own)
    bool g_fastRestyle = true;             // 'theme fast 0|1' (a test switch, not saved)
    int g_skipped = 0;                     // widgets RestyleOnce left out of Qt's loop in the last style change

    // A style change restyles each polished widget's whole subtree, so a subtree is redone once per ancestor. Qt's loop skips widgets without
    // WA_WState_Polished but the subtree walk does not: the flag is cleared on every widget a polished ancestor reaches anyway (unbroken chain of
    // QStyleSheetStyle proxies) and put back after the call, so each subtree is restyled once.
    const int kWA_WState_Polished = 66;
    bool OwnProxy(void* w, void* appStyle)
    {
        void* st = qt.widgetStyle(w);
        return st && st != appStyle && strcmp(ClassName(st), "QStyleSheetStyle") == 0;
    }
    std::vector<void*> RestyleOnce()
    {
        std::vector<void*> cleared;
        if (!g_fastRestyle || !qt.widgetStyle || !qt.appStyle || !qt.testAttribute || !qt.setAttribute || !qt.allWidgets) return cleared;
        void* app = qt.appStyle();
        std::vector<void*> all = AllWidgets();
        std::sort(all.begin(), all.end());
        // per widget: bit 0 = has a proxy of its own, bit 1 = polished, bit 2 = reached from a polished ancestor
        std::vector<unsigned char> flag(all.size(), 0);
        std::vector<int> parent(all.size(), -1);
        for (size_t i = 0; i < all.size(); ++i)
        {
            void* w = all[i];
            if (OwnProxy(w, app)) flag[i] |= 1;
            if (qt.testAttribute(w, kWA_WState_Polished)) flag[i] |= 2;
            void* p = qt.parentWidget(w);
            auto it = p ? std::lower_bound(all.begin(), all.end(), p) : all.end();
            if (it != all.end() && *it == p) parent[i] = int(it - all.begin());
        }
        std::vector<signed char> reached(all.size(), -1);
        for (size_t i = 0; i < all.size(); ++i)
        {
            // up the chain of proxies until a widget whose answer is known, or the chain's top
            std::vector<int> chain; int k = int(i); signed char r = 0;
            for (;;)
            {
                if (reached[k] >= 0) { r = reached[k]; break; }
                int p = parent[k];
                if (!(flag[k] & 1) || p < 0 || !(flag[p] & 1) || chain.size() > 256) { reached[k] = 0; r = 0; break; }
                chain.push_back(k);
                if (flag[p] & 2) { r = 1; break; }      // Qt touches the parent itself
                k = p;
            }
            for (int c : chain) reached[c] = r;
        }
        for (size_t i = 0; i < all.size(); ++i)
            if (reached[i] == 1 && (flag[i] & 2)) { qt.setAttribute(all[i], kWA_WState_Polished, false); cleared.push_back(all[i]); }
        g_skipped = int(cleared.size());
        return cleared;
    }
    void RestyleDone(const std::vector<void*>& cleared)
    {
        if (cleared.empty()) return;
        std::vector<void*> all = AllWidgets();           // a widget deleted during the change is not touched again
        std::sort(all.begin(), all.end());
        for (void* w : cleared) if (std::binary_search(all.begin(), all.end(), w)) qt.setAttribute(w, kWA_WState_Polished, true);
    }

    bool SetStyle(const std::string& key)
    {
        void* qs = nullptr; qt.qstringFromUtf8(&qs, key.c_str(), int(key.size()));
        std::vector<void*> cleared = RestyleOnce();
        void* style = qt.appSetStyle(&qs);
        RestyleDone(cleared);
        qt.qsDtor(&qs);
        return style != nullptr;
    }
    void SetSheet(const std::string& sheet)
    {
        void* qs = nullptr; qt.qstringFromUtf8(&qs, sheet.c_str(), int(sheet.size()));
        // from or to no sheet at all Qt changes the application style (the slow loop), sheet to sheet is a repolish
        std::vector<void*> cleared;
        if (sheet.empty() || !g_sheetSet) cleared = RestyleOnce();
        qt.appSetStyleSheet(qt.appInstance(), &qs);
        RestyleDone(cleared);
        qt.qsDtor(&qs);
    }

    const Entry* Find(const std::string& name)
    {
        std::string l = Lower(name);
        if (l == "default") l = Lower(themeicons::kClassicTreyarch);
        for (const Entry& e : g_list) if (Lower(e.name) == l) return &e;
        return nullptr;
    }

    // the base style and sheet in effect: Radiant's InitInstance leaves plastique + its stylesheet
    std::string g_style = "plastique";

    // QApplication::setStyle re-applies the style to every widget (about 5800): about 4 s, against 0.4 s for swapping the sheet
    // on the same style. So the style is only set when it changes, with the old sheet still set (clearing first cost much more).
    // Going to Classic (no sheet) the clear comes first, since setStyle is cheaper without a sheet.
    bool Apply(const std::string& name, std::string& why)
    {
        if (!qt.bound || !qt.appInstance()) { why = "Qt is not bound"; return false; }
        const Entry* e = Find(name);
        if (!e) { why = "no theme named '" + name + "' (see 'theme list')"; return false; }
        uint64_t t0 = Now();
        std::string style = "WindowsVista", sheet, titleSheet;   // classic: Qt's own Windows look, no sheet
        std::wstring pack, logo;
        if (!e->classic)
        {
            if (!ReadWhole(e->file, sheet)) { why = "cannot read " + Narrow(e->file); return false; }
            style = e->style.empty() ? StyleDirective(sheet) : e->style;
            if (style.empty()) style = "plastique";   // what Radiant draws its own sheet over
            size_t cut = e->file.find_last_of(L"\\/");
            std::wstring sheetDir = cut == std::wstring::npos ? Dir() : e->file.substr(0, cut);
            pack = themeicons::PackFor(Dir(), e->name, sheet, sheetDir);
            logo = themeicons::LogoFor(sheet, sheetDir);
            titleSheet = sheet;
            sheet = Resolve(sheet, sheetDir);
        }
        else   // Classic is Qt's light grey Windows look: Radiant's white icons are redrawn dark for it, as in the light sheets
            pack = themeicons::PackFor(Dir(), e->name, "/* icon-recolor: #262626 #f0f0f0 */", Dir());
        uint64_t t1 = Now();
        themeicons::Use(pack, logo);                  // before the sheet, so its url(:/...) images come from the pack too
        themeicons::UseTitleBar(titleSheet, e->name);   // Treyarch and Classic: built-in colours
        uint64_t t2 = Now(), t3 = t2;
        g_skipped = 0;
        if (Lower(style) != Lower(g_style))
        {
            SetCursor(LoadCursorW(nullptr, IDC_WAIT));   // seconds without events, Qt sets its own cursor on the next mouse move
            if (sheet.empty() && g_sheetSet) { SetSheet(""); g_sheetSet = false; }   // to Classic: no sheet, and setStyle is cheaper without one
            if (SetStyle(style)) g_style = style;
            else rp::Log("theme: base style '%s' is not available, keeping %s", style.c_str(), g_style.c_str());
            t3 = Now();
        }
        if (!sheet.empty() || g_sheetSet) { SetSheet(sheet); g_sheetSet = !sheet.empty(); }
        g_applied = e->name;
        themeedit::ClearEdited();                     // a theme replaces the editor's unsaved edit
        g_relayoutTicks = 2;                          // after Qt has delivered the style change events
        g_lastMs = Ms(Now() - t0);
        rp::Log("theme: '%s' applied (style %s) in %.0f ms (read %.0f, icons %.0f, style %.0f, sheet %.0f; %d widgets restyled through their parent)",
                e->name.c_str(), g_style.c_str(), g_lastMs, Ms(t1 - t0), Ms(t2 - t1), Ms(t3 - t2), Ms(Now() - t3), g_skipped);
        return true;
    }

    // The first apply of a theme with 'icon-recolor' redraws about 400 icons (0.4 s). After start the packs of the other
    // themes are made ahead, one per tick. A rescan (new or saved theme) starts it again.
    size_t g_warmNext = 0;
    void WarmPacks()
    {
        while (g_warmNext < g_list.size())
        {
            const Entry& e = g_list[g_warmNext++];
            std::string sheet;
            if (e.classic || e.file.empty() || Lower(e.name) == Lower(g_applied) || !ReadWhole(e.file, sheet)) continue;
            if (sheet.find("icon-recolor:") == std::string::npos || sheet.find("icon-recolor:") > 2048) continue;
            size_t cut = e.file.find_last_of(L"\\/");
            uint64_t t0 = Now();
            themeicons::PackFor(Dir(), e.name, sheet, cut == std::wstring::npos ? Dir() : e.file.substr(0, cut));
            if (Ms(Now() - t0) > 20) return;          // one that had to be drawn: the next theme on the next tick
        }
    }

    // Copies the plugin's shipped themes\ folder to the shared <root>\bin\themes (again when the shipped copy is newer, a user-edited
    // theme stays) and carries custom sheets over from old per-plugin folders. Runs before every scan.
    void CopyNewer(const std::wstring& from, const std::wstring& to)
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((from + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        CreateDirectoryW(to.c_str(), nullptr);
        do
        {
            std::wstring n = fd.cFileName, src = from + L"\\" + n, dst = to + L"\\" + n;
            if (n == L"." || n == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { CopyNewer(src, dst); continue; }
            WIN32_FILE_ATTRIBUTE_DATA d{};
            if (GetFileAttributesExW(dst.c_str(), GetFileExInfoStandard, &d) && CompareFileTime(&d.ftLastWriteTime, &fd.ftLastWriteTime) >= 0) continue;
            CopyFileW(src.c_str(), dst.c_str(), FALSE);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    void Seed()
    {
        static bool retired = false;                   // once per run: the old names out of both folders before the copy
        if (!retired) { retired = true; themeicons::RetireShipped(rp::PluginDir() + L"\\themes"); themeicons::RetireShipped(Dir()); }
        CopyNewer(rp::PluginDir() + L"\\themes", Dir());
    }

    void Scan()
    {
        Seed();
        g_list.clear();
        g_warmNext = 0;
        std::wstring rad = Root() + L"\\radiant\\stylesheet.qss";
        g_list.push_back({ themeicons::kClassicTreyarch, themeicons::kClassicTreyarch, rad, "plastique", false, false });
        g_list.push_back({ "classic", "Classic", L"", "", true, false });
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((Dir() + L"\\*.qss").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::wstring f = fd.cFileName;
                if (f.size() <= 4) continue;
                std::string name = Narrow(f.substr(0, f.size() - 4));
                if (name.empty() || Lower(name) == "default" || Find(name)) continue;   // built-in names win
                g_list.push_back({ name, name, Dir() + L"\\" + f, "", false, false });
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::stable_sort(g_list.begin(), g_list.end(), [](const Entry& a, const Entry& b) { return themeicons::ListBefore(a.name, b.name); });
    }

    void OnOpenFolder(void*)
    {
        CreateDirectoryW(Dir().c_str(), nullptr);
        ShellExecuteW(nullptr, L"open", Dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // OK or Apply in Preferences: Radiant's own slot has run, apply on the next tick (re-applies a theme the editor re-saved)
    void OnPrefsCommit(void* dialog)
    {
        if (!dialog || dialog != g_dlg.dialog || !g_dlg.combo) return;
        int i = qt.comboCurrentIndex(g_dlg.combo);
        if (i < 0 || size_t(i) >= g_dlg.names.size()) return;
        g_pending = g_dlg.names[size_t(i)];
    }

    void* QNew(size_t bytes) { void* p = qt.opNew(bytes); if (p) memset(p, 0, bytes); return p; }
    void* UiField(void* dialog, size_t off)
    {
        char* ui = *reinterpret_cast<char**>(static_cast<char*>(dialog) + off_Prefs_Ui);
        return ui ? *reinterpret_cast<void**>(ui + off) : nullptr;
    }

    std::vector<void*> Children(void* obj)
    {
        std::vector<void*> out;
        auto* d = *reinterpret_cast<QListDataD**>(qt.children(obj));
        if (!d) return out;
        for (int i = d->begin; i < d->end && i - d->begin < 10000; ++i) out.push_back(d->array[i]);
        return out;
    }
    std::string Describe(void* w)
    {
        char buf[256];
        std::string text;
        if (IsA(w, "QAbstractButton")) text = TakeString(qt.buttonText, w);
        const int* g = IsA(w, "QWidget") ? qt.geometry(w) : nullptr;
        snprintf(buf, sizeof buf, "%s '%s' \"%s\" %d,%d..%d,%d%s", ClassName(w), TakeString(qt.objectName, w).c_str(), text.c_str(),
                 g ? g[0] : 0, g ? g[1] : 0, g ? g[2] : 0, g ? g[3] : 0, IsA(w, "QWidget") && qt.widgetLayout(w) ? " (layout)" : "");
        return buf;
    }
    void Dump(void* w, int depth, std::string& out)
    {
        out += std::string(size_t(depth) * 2, ' ') + Describe(w) + "\n";
        if (depth < 4) for (void* c : Children(w)) if (c && IsA(c, "QWidget")) Dump(c, depth + 1, out);
    }
    std::string Probe(void* dialog)
    {
        std::string out;
        void* reset = UiField(dialog, off_Ui_Reset);
        if (!reset) return "no reset button\n";
        out += "chain:\n";
        for (void* p = reset; p; p = qt.parentWidget(p)) out += "  " + Describe(p) + "\n";
        void* page = qt.parentWidget(reset);
        while (page && qt.parentWidget(page) && !IsA(qt.parentWidget(page), "QStackedWidget")) page = qt.parentWidget(page);
        out += "page tree:\n";
        if (page) Dump(page, 1, out);
        return out;
    }

    // ---- view color presets (the Color tab) ----
    // Radiant's names for the table rows, in table order (its own name table at 0x14337FEB0, registry Treyarch\Radiant\Colors)
    const char* const kColorNames[48] = {
        "COLOR_TEXTUREBACK", "COLOR_GRIDBACK", "COLOR_GRIDMINOR", "COLOR_GRIDMAJOR", "COLOR_CAMERABACK", "COLOR_ENTITY",
        "COLOR_LIGHT", "COLOR_GRIDBLOCK", "COLOR_GRIDTEXT", "COLOR_BRUSHES", "COLOR_SELBRUSHES", "COLOR_SELBRUSHESCAMERA",
        "COLOR_CLIPPER", "COLOR_VIEWNAME", "COLOR_DETAIL", "COLOR_PORTAL", "COLOR_FACESELECTION", "COLOR_FUNCGROUP",
        "COLOR_FUNCCULLGROUP", "COLOR_WEAPONCLIP", "COLOR_SIZEINFO", "COLOR_MODEL", "COLOR_SCRIPTMODEL", "COLOR_DYNMODEL",
        "COLOR_NONCOLLIDING", "COLOR_WIREFRAME", "COLOR_FROZEN", "COLOR_PREFABLEVEL", "COLOR_ROPE", "COLOR_LYRMTL_SELECTEDBACK",
        "COLOR_LYRMTL_SELECTEDTEXT", "COLOR_TERRAIN", "COLOR_TEXTURE_IMAGEBACK", "COLOR_TEXTURE_IMAGEBACK_INUSE",
        "COLOR_TERRAINPATCH", "COLOR_CURVEPATCH", "COLOR_SELTEXTURETINT", "COLOR_PREFAB_WIREFRAME", "COLOR_CAMWND_GRID",
        "COLOR_CAMWND_GRID_TEXT", "COLOR_CAMWND_GRID_AXIS", "COLOR_CAMWND_GRID_SUBDIVISION", "COLOR_TRIANGULATED_COLLISION",
        "COLOR_XYWND_CAMERA_ICON", "COLOR_XAXIS_MANIP_HANDLE", "COLOR_YAXIS_MANIP_HANDLE", "COLOR_ZAXIS_MANIP_HANDLE",
        "COLOR_SCRIPT_BRUSHMODEL" };
    // rows without a swatch on the Color tab (ENTITY, the two layer-material list colors, TERRAIN, TRIANGULATED_COLLISION):
    // a preset sets them straight in the table on OK/Apply
    bool OnColorTab(int i) { return i != 5 && i != 29 && i != 30 && i != 31 && i != 42; }
    // the Color tab's Shared page (object, selection and texture browser colors) and the object rows without a swatch (ENTITY,
    // TERRAIN, TRIANGULATED_COLLISION), the rest are Grid View, Camera View and the layer-material list (29, 30)
    bool IsShared(int i)
    {
        static const int rows[] = { 0, 5, 6, 9, 11, 12, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 31, 32, 33, 34, 35, 36, 37, 42 };
        for (int r : rows) if (r == i) return true;
        return false;
    }
    // Radiant's built-in values (what it starts with when nothing is saved), COLOR_LIGHT is (0, 8, 0, 1) there, which
    // no swatch can hold, so the preset uses black
    const float kRadiantDefault[48][4] = {
        { .63f, .63f, .63f, 1 }, { .63f, .63f, .63f, 1 }, { .57f, .57f, .57f, 1 }, { .5f, .5f, .5f, 1 }, { .63f, .63f, .63f, 1 },
        { 0, 0, 0, 0 }, { 0, 0, 0, 1 }, { .75f, .75f, .75f, 1 }, { 0, .27f, .1f, 1 }, { 0, .02f, .38f, 1 }, { .26f, 1, .64f, 1 },
        { .26f, 1, .64f, .25f }, { 0, 0, 1, 1 }, { 0, .27f, .1f, 1 }, { .66f, .67f, 1, 1 }, { .1f, .4f, 1, 1 }, { .26f, 1, .64f, .25f },
        { .75f, .75f, .75f, 1 }, { 0, .5f, .5f, 1 }, { .79f, .8f, .1f, 1 }, { 0, .27f, .1f, 1 }, { .88f, .89f, 1, 1 }, { 0, .5f, .8f, 1 },
        { .84f, .03f, .84f, 1 }, { .8f, .6f, 0, 1 }, { .2f, .17f, .14f, 1 }, { .75f, 0, 0, 1 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 },
        { 0, .27f, .1f, 1 }, { .63f, .63f, .63f, 1 }, { .2f, 1, .2f, 1 }, { .49f, .49f, .49f, 1 }, { .49f, .49f, .49f, 1 },
        { .2f, 1, .2f, 1 }, { .2f, 1, .2f, 1 }, { 0, .89f, .89f, .25f }, { 1, 0, 0, 1 }, { .9f, .9f, .9f, .95f }, { 1, 1, 1, 1 },
        { 0, 0, 0, .95f }, { .9f, .9f, .9f, .9f }, { 0, 0, 1, 1 }, { 0, 0, 1, 1 }, { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 },
        { 0, .5f, .8f, 1 } };
    const char* const kDefaultPreset = "Radiant Default";
    const wchar_t* const kMyColorsFile = L"My Colors.colors";   // in the plugin folder: the user's colors before the first preset

    struct ColorValue { int index; float rgba[4]; bool hasAlpha; };
    struct Preset { std::string name; std::wstring file; std::vector<ColorValue> values; };
    std::vector<Preset> g_presets;

    using TabLoadFn = void (__fastcall*)(void* dialog, void* prefs);
    using TabStoreFn = void (__fastcall*)(void* dialog);
    using PrefsInitFn = void (__fastcall*)(void* dialog);
    float (*g_table)[4] = nullptr;
    void* g_prefsBlock = nullptr;
    TabLoadFn g_tabLoad = nullptr;
    TabStoreFn g_tabStore = nullptr;
    PrefsInitFn g_prefsInit = nullptr;
    bool g_colorsOn = false;
    bool g_follow = true;                  // [colors] follow_theme
    bool g_shared = true;                  // [colors] shared: presets set the Shared tab's colors too (0 = Radiant's defaults there)
    std::string g_colorsWhere, g_colorsError, g_lastPreset;

    struct ColorRow
    {
        void* dialog = nullptr; void* page = nullptr; void* combo = nullptr; void* follow = nullptr; void* save = nullptr; void* shared = nullptr;
        std::vector<std::string> names;    // combo index -> preset name ("" = the "choose" line)
        bool used = false;                 // a preset went into the swatches: commit the rows without a swatch, save
        bool busy = false;                 // setting the combo from code
        bool fromTheme = false;            // the pick comes from the Theme combo (Follow theme): start from the defaults
        float extra[48][4] = {}; bool hasExtra[48] = {};
    };
    ColorRow g_row;

    bool LookupFunction(void* target)
    {
        DWORD64 base = 0; PRUNTIME_FUNCTION fn = target ? RtlLookupFunctionEntry(DWORD64(target), &base, nullptr) : nullptr;
        return fn && DWORD64(fn->BeginAddress) + base == DWORD64(target);
    }
    void SetupColors()
    {
        g_follow = rp::ConfigGetBool("colors", "follow_theme", true);
        g_shared = rp::ConfigGetBool("colors", "shared", true);
        g_table = static_cast<float (*)[4]>(rp::host->RadiantAddress(rp::self, kColorTable));
        g_prefsBlock = rp::host->RadiantAddress(rp::self, kPrefsBlock);
        void* load = rp::host->RadiantAddress(rp::self, kColorTabLoad);
        void* store = rp::host->RadiantAddress(rp::self, kColorTabStore);
        void* init = rp::host->RadiantAddress(rp::self, kPrefsInit);
        if (!g_table || !g_prefsBlock || !LookupFunction(load) || !LookupFunction(store) || !LookupFunction(init))
        { rp::Log("colors: Radiant's color table or Color tab functions not found, view color presets are off"); return; }
        g_tabLoad = reinterpret_cast<TabLoadFn>(load);
        g_tabStore = reinterpret_cast<TabStoreFn>(store);
        g_prefsInit = reinterpret_cast<PrefsInitFn>(init);
        g_colorsOn = true;
    }

    int ColorIndex(const std::string& key)
    {
        std::string k = Lower(key);
        for (int i = 0; i < 48; ++i) if (Lower(kColorNames[i]) == k) return i;
        return -1;
    }
    float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
    // "KEY=r, g, b[, a]" per line, 0..1, semicolon and hash start comments, three values keep the current alpha
    bool ParsePreset(const std::wstring& path, std::vector<ColorValue>& out)
    {
        std::string text;
        if (!ReadWhole(path, text)) return false;
        size_t at = 0;
        while (at < text.size())
        {
            size_t end = text.find('\n', at);
            std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
            at = end == std::string::npos ? text.size() : end + 1;
            size_t eq = line.find('=');
            if (line.empty() || line[0] == ';' || line[0] == '#' || eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            while (!key.empty() && isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
            while (!key.empty() && isspace(static_cast<unsigned char>(key.front()))) key.erase(key.begin());
            int index = ColorIndex(key);
            if (index < 0) continue;
            ColorValue v{ index, { 0, 0, 0, 1 }, false };
            int n = sscanf(line.c_str() + eq + 1, " %f , %f , %f , %f", &v.rgba[0], &v.rgba[1], &v.rgba[2], &v.rgba[3]);
            if (n < 3) continue;
            v.hasAlpha = n >= 4;
            for (float& c : v.rgba) c = Clamp01(c);
            out.push_back(v);
        }
        return true;
    }
    bool WritePreset(const std::wstring& path, const float (*table)[4], const std::string& title)
    {
        std::string text = "; " + title + "\r\n; Radiant view colors (Radiant+, Settings > Preferences > Color > Preset). KEY=r, g, b, a in 0..1;\r\n"
                           "; a preset may list only some keys, the others stay as they are.\r\n";
        char line[160];
        for (int i = 0; i < 48; ++i)
        {
            snprintf(line, sizeof line, "%s=%g, %g, %g, %g\r\n", kColorNames[i], table[i][0], table[i][1], table[i][2], table[i][3]);
            text += line;
        }
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        DWORD wrote = 0;
        bool ok = WriteFile(f, text.data(), DWORD(text.size()), &wrote, nullptr) && wrote == text.size();
        CloseHandle(f);
        return ok;
    }

    const Preset* FindPreset(const std::string& name)
    {
        std::string l = Lower(name);
        for (const Preset& p : g_presets) if (Lower(p.name) == l) return &p;
        return nullptr;
    }
    void ScanPresets()
    {
        Seed();
        g_presets.clear();
        Preset def{ kDefaultPreset, L"", {} };
        for (int i = 0; i < 48; ++i) def.values.push_back({ i, { kRadiantDefault[i][0], kRadiantDefault[i][1], kRadiantDefault[i][2], kRadiantDefault[i][3] }, true });
        g_presets.push_back(def);
        Preset mine{ "My Colors", rp::PluginDir() + L"\\" + kMyColorsFile, {} };
        if (ParsePreset(mine.file, mine.values) && !mine.values.empty()) g_presets.push_back(mine);
        size_t builtins = g_presets.size();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((Dir() + L"\\*.colors").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::wstring f = fd.cFileName;
                if (f.size() <= 7) continue;
                Preset p{ Narrow(f.substr(0, f.size() - 7)), Dir() + L"\\" + f, {} };
                if (p.name.empty() || FindPreset(p.name)) continue;   // built-in names win
                if (ParsePreset(p.file, p.values) && !p.values.empty()) g_presets.push_back(p);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(g_presets.begin() + builtins, g_presets.end(), [](const Preset& a, const Preset& b) { return Lower(a.name) < Lower(b.name); });
    }
    // the preset that goes with a theme: the same name (Classic has Classic.colors), Treyarch and Classic Treyarch -> Radiant's defaults
    const Preset* PresetForTheme(const std::string& theme)
    {
        std::string l = Lower(theme);
        if (l.empty() || l == "default" || l == "treyarch" || themeicons::IsClassicTreyarch(l)) return FindPreset(kDefaultPreset);   // both Treyarch looks
        const Preset* p = FindPreset(theme);
        return p ? p : l == "classic" ? FindPreset(kDefaultPreset) : nullptr;
    }

    // Radiant keeps the colors in its registry key only at exit, a preset also writes them right away (same text format)
    void SaveColorsToRegistry()
    {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Treyarch\\Radiant\\Colors", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
        for (int i = 0; i < 48; ++i)
        {
            wchar_t value[96];
            swprintf(value, 96, L"%g, %g, %g, %g", g_table[i][0], g_table[i][1], g_table[i][2], g_table[i][3]);
            RegSetValueExW(key, Widen(kColorNames[i]).c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(value), DWORD((wcslen(value) + 1) * sizeof(wchar_t)));
        }
        RegCloseKey(key);
    }
    // once, before the first preset replaces anything: the user's own colors become the "My Colors" preset
    void KeepMyColors()
    {
        std::wstring path = rp::PluginDir() + L"\\" + kMyColorsFile;
        if (FileExists(path)) return;
        if (WritePreset(path, g_table, "My Colors: your view colors from before the first preset (Radiant+ wrote this once)"))
            rp::Log("colors: your current colors saved as the 'My Colors' preset");
    }
    void RedrawViews() { if (rp::Has(&rp::host->RequestRedraw)) rp::host->RequestRedraw(rp::self, MODTOOLS_VIEW_3D | MODTOOLS_VIEW_2D); }

    // Fills an open dialog's swatches with a preset: the table is borrowed for Radiant's copy functions (swatches -> table,
    // preset over it, table -> swatches) and put back, so nothing changes until OK/Apply. Rows without a swatch wait in
    // g_row.extra. fromDefaults (a theme picked it): rows the preset leaves out become Radiant's defaults.
    void PreviewPreset(void* dialog, const Preset& p, bool fromDefaults)
    {
        static float saved[48][4];
        memcpy(saved, g_table, sizeof saved);
        g_tabStore(dialog);                                    // the swatches as the user left them (partial presets keep the rest)
        if (fromDefaults)
            for (int i = 0; i < 48; ++i)
            {
                memcpy(OnColorTab(i) ? g_table[i] : g_row.extra[i], kRadiantDefault[i], sizeof kRadiantDefault[0]);
                if (!OnColorTab(i)) g_row.hasExtra[i] = true;
            }
        for (const ColorValue& v : p.values)
        {
            if (!g_shared && IsShared(v.index)) continue;      // the Shared tab stays Radiant's (below)
            float* row = OnColorTab(v.index) ? g_table[v.index] : g_row.extra[v.index];
            if (!OnColorTab(v.index) && !g_row.hasExtra[v.index]) memcpy(row, saved[v.index], sizeof saved[0]);
            for (int c = 0; c < (v.hasAlpha ? 4 : 3); ++c) row[c] = v.rgba[c];
            if (!OnColorTab(v.index)) g_row.hasExtra[v.index] = true;
        }
        if (!g_shared)
            for (int i = 0; i < 48; ++i)
                if (IsShared(i))
                {
                    memcpy(OnColorTab(i) ? g_table[i] : g_row.extra[i], kRadiantDefault[i], sizeof kRadiantDefault[0]);
                    if (!OnColorTab(i)) g_row.hasExtra[i] = true;
                }
        g_tabLoad(dialog, g_prefsBlock);
        memcpy(g_table, saved, sizeof saved);
        g_row.used = true;
        g_lastPreset = p.name;
    }
    void PreviewGuarded(void* dialog, const Preset& p, bool fromDefaults)
    {
        __try { PreviewPreset(dialog, p, fromDefaults); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_colorsError = "filling the Color tab faulted"; }
    }
    // a preset straight into the table (commands, a theme applied by command with Follow theme on)
    void ApplyPresetNow(const Preset& p, bool fromDefaults)
    {
        KeepMyColors();
        if (fromDefaults) memcpy(g_table, kRadiantDefault, sizeof kRadiantDefault);
        for (const ColorValue& v : p.values)
            for (int c = 0; c < (v.hasAlpha ? 4 : 3); ++c) g_table[v.index][c] = v.rgba[c];
        if (!g_shared) for (int i = 0; i < 48; ++i) if (IsShared(i)) memcpy(g_table[i], kRadiantDefault[i], sizeof kRadiantDefault[0]);
        SaveColorsToRegistry();
        RedrawViews();
        g_lastPreset = p.name;
        rp::Log("colors: preset '%s' applied", p.name.c_str());
    }

    void FillPresetCombo(const std::string& select)
    {
        const ModToolsUiApi* ui = rp::host->ui;
        g_row.busy = true;
        ui->ComboClear(g_row.combo);
        g_row.names.clear();
        ui->ComboAdd(g_row.combo, "Choose a preset...");
        g_row.names.push_back("");
        int current = 0;
        for (const Preset& p : g_presets)
        {
            ui->ComboAdd(g_row.combo, p.name.c_str());
            if (!select.empty() && Lower(p.name) == Lower(select)) current = int(g_row.names.size());
            g_row.names.push_back(p.name);
        }
        ui->ComboSetCurrent(g_row.combo, current);
        g_row.busy = false;
    }
    void OnPresetPicked(void* dialog, int index)
    {
        if (g_row.busy || dialog != g_row.dialog || index <= 0 || size_t(index) >= g_row.names.size()) return;
        const Preset* p = FindPreset(g_row.names[size_t(index)]);
        if (!p) return;
        if (!g_row.used) KeepMyColors();
        PreviewGuarded(dialog, *p, g_row.fromTheme);
    }
    // the Theme combo changed (Follow theme ticked): its preset goes into the swatches too
    void OnThemePicked(void* dialog, const std::string& theme)
    {
        if (!g_row.dialog || dialog != g_row.dialog || !rp::host->ui->IsChecked(g_row.follow)) return;
        const Preset* p = PresetForTheme(theme);
        if (!p) return;
        for (size_t i = 0; i < g_row.names.size(); ++i)
        {
            if (g_row.names[i] != p->name) continue;
            g_row.fromTheme = true;
            if (rp::host->ui->ComboCurrent(g_row.combo) == int(i)) OnPresetPicked(dialog, int(i));   // no change signal then
            else rp::host->ui->ComboSetCurrent(g_row.combo, int(i));                                // OnChanged previews it
            g_row.fromTheme = false;
            return;
        }
    }
    // "Shared colors": a preset in the swatches is filled again with the new choice
    void OnSharedToggled(void* dialog, int on)
    {
        g_shared = on != 0;
        rp::ConfigSet("colors", "shared", g_shared ? "1" : "0");
        if (!g_row.dialog || dialog != g_row.dialog) return;
        int i = rp::host->ui->ComboCurrent(g_row.combo);
        if (i > 0 && size_t(i) < g_row.names.size()) if (const Preset* p = FindPreset(g_row.names[size_t(i)])) PreviewGuarded(dialog, *p, false);
    }
    void OnFollowToggled(void*, int on)
    {
        g_follow = on != 0;
        rp::ConfigSet("colors", "follow_theme", g_follow ? "1" : "0");
    }
    std::string SafeFileName(std::string s)
    {
        for (char& ch : s) if (strchr("\\/:*?\"<>|", ch) || static_cast<unsigned char>(ch) < 32) ch = '_';
        while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
        return s;
    }
    // what the dialog's swatches show, through Radiant's own copy (swatches -> table) with the table put back
    bool ReadSwatches(void* dialog, float (*out)[4])
    {
        static float saved[48][4];
        memcpy(saved, g_table, sizeof saved);
        bool ok = true;
        __try { g_tabStore(dialog); memcpy(out, g_table, sizeof saved); }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        memcpy(g_table, saved, sizeof saved);
        return ok;
    }
    // Save As: the dialog's swatches (plus the table's rows without one) as bin\themes\<name>.colors
    void OnSaveAs(void* dialog)
    {
        if (dialog != g_row.dialog || !qt.getText) return;
        void* title = nullptr; qt.qstringFromUtf8(&title, "Save View Colors", -1);
        void* label = nullptr; qt.qstringFromUtf8(&label, "Preset name (saved in bin\\themes, shared with your other Radiants):", -1);
        void* text = nullptr; qt.qstringFromUtf8(&text, "", 0);
        void* result = nullptr; bool ok = false;
        qt.getText(&result, dialog, &title, &label, 0, &text, &ok, 0, 0);
        std::string name = SafeFileName(QStringToUtf8(&result));
        qt.qsDtor(&result); qt.qsDtor(&text); qt.qsDtor(&label); qt.qsDtor(&title);
        if (!ok || name.empty()) return;
        static float shown[48][4];
        if (!ReadSwatches(dialog, shown)) { g_colorsError = "reading the Color tab faulted"; return; }
        for (int i = 0; i < 48; ++i) if (!OnColorTab(i) && g_row.hasExtra[i]) memcpy(shown[i], g_row.extra[i], sizeof shown[0]);
        CreateDirectoryW(Dir().c_str(), nullptr);
        std::wstring path = Dir() + L"\\" + Widen(name) + L".colors";
        if (!WritePreset(path, shown, name + ": saved from Radiant's Color tab")) { g_colorsError = "cannot write " + Narrow(path); return; }
        rp::Log("colors: preset '%s' saved to %s", name.c_str(), Narrow(path).c_str());
        ScanPresets();
        FillPresetCombo(name);
    }

    // OK or Apply: Radiant's slot has copied the swatches into the table, the rows without a swatch follow, then the
    // registry and the views
    void OnColorsCommit(void* dialog)
    {
        if (dialog != g_row.dialog || !g_row.used) return;
        for (int i = 0; i < 48; ++i) if (g_row.hasExtra[i]) memcpy(g_table[i], g_row.extra[i], sizeof g_row.extra[0]);
        SaveColorsToRegistry();
        RedrawViews();
        rp::Log("colors: Preferences committed with preset '%s'", g_lastPreset.c_str());
    }

    // the Color page: its page tabs (Shared / Camera View / Grid View), the page and the page's layout
    bool ColorPage(void* dialog, void*& tabs, void*& page, void*& lay)
    {
        tabs = page = lay = nullptr;
        void* swatch = UiField(dialog, off_Ui_SwatchCameraBack);
        if (!swatch || !IsA(swatch, "QWidget")) return false;
        tabs = qt.parentWidget(swatch);
        while (tabs && !IsA(tabs, "QTabWidget")) tabs = qt.parentWidget(tabs);
        page = tabs ? qt.parentWidget(tabs) : nullptr;
        lay = page ? qt.widgetLayout(page) : nullptr;
        return page != nullptr;
    }

    // the row goes under the Color tab's inner tab widget (Shared / Camera View / Grid View)
    void PatchColors(void* dialog)
    {
        const ModToolsUiApi* ui = rp::host->ui;
        g_row = ColorRow{};
        if (!g_colorsOn) return;
        if (!ui || !rp::HasUi(&ui->ComboClear) || !rp::Has(&rp::host->OnChanged)) { g_colorsError = "the host is too old for the preset row"; return; }
        void* swatch = UiField(dialog, off_Ui_SwatchCameraBack);
        if (!swatch || !IsA(swatch, "QWidget")) { g_colorsError = "the Color tab's swatches are not where expected"; return; }
        void* tabs = qt.parentWidget(swatch);
        while (tabs && !IsA(tabs, "QTabWidget")) tabs = qt.parentWidget(tabs);
        void* page = tabs ? qt.parentWidget(tabs) : nullptr;
        if (!page) { g_colorsError = "the Color tab's page tabs were not found"; return; }
        void* lay = qt.widgetLayout(page);
        int index = lay ? qt.layoutIndexOf(lay, tabs) : -1;
        if (lay && (index < 0 || !(IsA(lay, "QBoxLayout") || IsA(lay, "QGridLayout")))) { g_colorsError = std::string("the Color tab's layout is a ") + ClassName(lay); return; }

        ScanPresets();
        void* holder = lay ? page : ui->Widget(page);
        void* row = ui->HBox();
        ui->LayoutSpacing(row, 6);
        void* label = ui->Label("Preset:", holder);
        void* combo = ui->ComboBox(holder);
        void* save = ui->PushButton("Save As...", holder);
        void* follow = ui->CheckBox("Follow theme", holder);
        ui->SetChecked(follow, g_follow ? 1 : 0);
        void* shared = ui->CheckBox("Shared colors", holder);
        ui->SetChecked(shared, g_shared ? 1 : 0);
        ui->SetToolTip(shared, "On: a preset (and Follow theme) also sets the Shared tab: brushes, models, clip, detail, selection and "
                               "texture browser colors. Off: the Shared tab keeps Radiant's default colors; Grid View and Camera View still follow.");
        ui->SetToolTip(combo, "Radiant+: fills the color swatches from a preset (bin\\themes\\<name>.colors, or Radiant's own "
                              "defaults). The views update at once, no OK needed. 'My Colors' are your colors from before the first preset.");
        ui->SetToolTip(save, "Save the swatches as a preset in bin\\themes (shared with APE's themes folder and other mappers)");
        ui->SetToolTip(follow, "Picking a theme above also picks the view color preset of the same name (Classic Treyarch: Radiant Default)");
        ui->BoxAddWidget(row, label, 0);
        ui->BoxAddWidget(row, combo, 1);
        ui->BoxAddWidget(row, save, 0);
        ui->BoxAddWidget(row, follow, 0);
        ui->BoxAddWidget(row, shared, 0);
        if (!qt.getText) ui->SetEnabled(save, 0);

        char where[160];
        if (!lay)
        {
            const int* g = qt.geometry(tabs);
            ui->LayoutMargins(row, 0, 0, 0, 0);
            ui->SetLayout(holder, row);
            qt.setGeometry(tabs, g[0], g[1], g[2] - g[0] + 1, g[3] - g[1] + 1 - 32);   // make room at the bottom
            qt.setGeometry(holder, g[0], g[3] - 26, g[2] - g[0] + 1, 26);
            snprintf(where, sizeof where, "fixed %d,%d", g[0], g[3] - 26);
        }
        else if (IsA(lay, "QBoxLayout"))
        {
            qt.boxInsertLayout(lay, index + 1, row, 0);   // just under the page tabs
            snprintf(where, sizeof where, "%s index %d", ClassName(lay), index + 1);
        }
        else
        {
            int r = qt.gridRowCount(lay), cols = qt.gridColumnCount(lay);
            qt.gridAddLayout(lay, row, r, 0, 1, cols > 0 ? cols : 1, 0);
            snprintf(where, sizeof where, "%s row %d", ClassName(lay), r);
        }
        g_colorsWhere = where;

        g_row.dialog = dialog; g_row.page = page; g_row.combo = combo; g_row.follow = follow; g_row.save = save; g_row.shared = shared;
        FillPresetCombo("");
        rp::host->OnChanged(rp::self, combo, [](void* d, int i) { OnPresetPicked(d, i); }, dialog);
        rp::host->OnChanged(rp::self, follow, OnFollowToggled, nullptr);
        rp::host->OnChanged(rp::self, shared, OnSharedToggled, dialog);
        rp::host->OnClicked(rp::self, save, OnSaveAs, dialog);
        for (size_t off : { off_Ui_Ok, off_Ui_Apply })
            if (void* b = UiField(dialog, off)) if (IsA(b, "QAbstractButton")) rp::host->OnClicked(rp::self, b, OnColorsCommit, dialog);
        g_colorsError.clear();
    }
    bool PatchColorsGuarded(void* dialog)
    {
        __try { PatchColors(dialog); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void ResetRow() { g_row = ColorRow{}; }

    // ---- Theme editor (plugins/common/themeeditor.cpp): the Color page's first page tab ----
    void* g_editorPage = nullptr;
    std::string g_editorError;
    // the Theme row's combo, refilled after Save As (changing it is a user's pick: Follow theme takes the colors along)
    void FillThemeCombo(const std::string& select)
    {
        if (!g_dlg.combo || !qt.comboClear) return;
        g_fillingCombo = true;
        qt.comboClear(g_dlg.combo);
        g_dlg.names.clear();
        int current = 0;
        for (const Entry& e : g_list)
        {
            void* text = nullptr; qt.qstringFromUtf8(&text, e.label.c_str(), int(e.label.size()));
            char variant[16] = {}; qt.variantCtor(variant);
            qt.comboAddItem(g_dlg.combo, &text, variant);
            qt.variantDtor(variant); qt.qsDtor(&text);
            if (Lower(e.name) == Lower(select)) current = int(g_dlg.names.size());
            g_dlg.names.push_back(e.name);
        }
        qt.comboSetCurrentIndex(g_dlg.combo, current);
        g_fillingCombo = false;
    }
    // the style and sheet without a theme entry (the editor's Preview)
    void ApplyLook(const std::string& sheetText)
    {
        std::string style = StyleDirective(sheetText);
        if (style.empty()) style = "plastique";
        themeicons::Use(themeicons::PackFor(Dir(), "_preview", sheetText, Dir()));
        themeicons::UseTitleBar(sheetText);
        if (Lower(style) != Lower(g_style))
        {
            SetCursor(LoadCursorW(nullptr, IDC_WAIT));
            if (SetStyle(style)) g_style = style;     // with the old sheet still set (see Apply)
        }
        SetSheet(Resolve(sheetText, Dir()));
        g_sheetSet = true;
        g_relayoutTicks = 2;
    }
    // the Theme combo shows "Edited (not saved)" while the editor's change is the look (OK keeps it, a theme reverts it)
    void MarkEdited()
    {
        if (!g_dlg.combo) return;
        for (size_t i = 0; i < g_dlg.names.size(); ++i)
            if (g_dlg.names[i] == kEditedName) { if (qt.comboCurrentIndex(g_dlg.combo) != int(i)) qt.comboSetCurrentIndex(g_dlg.combo, int(i)); return; }
        void* text = nullptr; qt.qstringFromUtf8(&text, "Edited (not saved)", -1);
        char variant[16] = {}; qt.variantCtor(variant);
        qt.comboAddItem(g_dlg.combo, &text, variant);
        qt.variantDtor(variant); qt.qsDtor(&text);
        g_dlg.names.push_back(kEditedName);
        qt.comboSetCurrentIndex(g_dlg.combo, int(g_dlg.names.size()) - 1);
    }
    void PatchEditor(void* dialog)
    {
        g_editorPage = nullptr;
        void* tabs = nullptr; void* page = nullptr; void* lay = nullptr;
        if (!ColorPage(dialog, tabs, page, lay) || !tabs) { g_editorError = "the Color page tabs were not found"; return; }
        if (g_list.empty()) Scan();
        themeedit::Host h;
        h.themesDir = Dir();
        h.product = "Radiant+";
        h.where = "Radiant: Settings > Preferences > Color > Theme";
        h.viewColors = true;
        h.themes = [] { std::vector<themeedit::ThemeRef> v; for (const Entry& e : g_list) v.push_back({ e.name, e.label, e.file }); return v; };
        h.current = [] { return g_applied; };
        h.preview = [](const std::string& sheet) { ApplyLook(sheet); MarkEdited(); };
        h.previewIcons = [](const std::string& lines) { themeicons::Use(themeicons::PackFor(Dir(), "_preview", lines, Dir())); MarkEdited(); };
        h.saved = [](const std::string& name)
        {
            Scan();
            ScanPresets();
            if (g_row.combo) { int i = rp::host->ui->ComboCurrent(g_row.combo); FillPresetCombo(i > 0 && size_t(i) < g_row.names.size() ? g_row.names[size_t(i)] : ""); }
            g_reapply = true;                                    // a re-saved theme in effect changed on disk
            FillThemeCombo(name);                                // OK then applies it (and Follow theme its colors)
        };
        void* ed = themeedit::Build(nullptr, dialog, h);
        if (!ed) { g_editorError = themeedit::Error(); return; }
        void* title = nullptr; qt.qstringFromUtf8(&title, "Theme", 5);
        if (qt.tabInsert && qt.tabSetCurrent) { qt.tabInsert(tabs, 0, ed, &title); qt.tabSetCurrent(tabs, 0); }   // the first page tab
        else rp::host->ui->TabAdd(tabs, ed, "Theme");
        qt.qsDtor(&title);
        g_editorPage = ed;
        g_editorError.clear();
    }
    bool PatchEditorGuarded(void* dialog)
    {
        __try { PatchEditor(dialog); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void ResetEditor() { themeedit::Reset(); g_editorPage = nullptr; }

    void OnThemeComboChanged(void* dialog, int index)
    {
        if (dialog != g_dlg.dialog || index < 0 || size_t(index) >= g_dlg.names.size()) return;
        const std::string name = g_dlg.names[size_t(index)];
        themeedit::ShowTheme(name);                           // the editor shows the picked theme's colours
        OnThemePicked(dialog, name);
        if (!g_fillingCombo && name != kEditedName) g_pending = name;   // a pick applies at once (next tick), no OK needed
    }

    void Patch(void* dialog)
    {
        const ModToolsUiApi* ui = rp::host->ui;
        if (!ui) { g_patchError = "no host ui"; return; }
        // The row goes on the Color page (with the view color presets and the Theme editor), under its page tabs, the
        // System tab next to Reset Preferences is the fallback when the Color page is not where expected.
        void* cpTabs = nullptr; void* cpPage = nullptr; void* cpLay = nullptr;
        const bool onColor = ColorPage(dialog, cpTabs, cpPage, cpLay) && cpLay && IsA(cpLay, "QGridLayout");
        void* reset = UiField(dialog, off_Ui_Reset);
        if (!onColor && (!reset || !IsA(reset, "QPushButton") || TakeString(qt.buttonText, reset) != "Reset Preferences to default"))
        { g_patchError = "neither the Color page nor the Reset Preferences button is where expected"; return; }
        // Radiant's System tab (m_SystemTab) has no layout: uic placed every child at a fixed position (640 x 480 page, the
        // Reset/Nuke buttons at 300..490 x 350..402, nothing below). A layout, if another build has one, is used instead.
        void* page = onColor ? cpPage : qt.parentWidget(reset);
        if (!page) { g_patchError = "the Reset button has no parent"; return; }
        void* lay = onColor ? cpLay : qt.widgetLayout(page);
        int index = lay && !onColor ? qt.layoutIndexOf(lay, reset) : -1;
        if (!onColor && lay && (index < 0 || !(IsA(lay, "QBoxLayout") || IsA(lay, "QGridLayout")))) { g_patchError = std::string("the Reset button's layout is a ") + ClassName(lay) + ", not a box or grid layout"; return; }
        int left = 1 << 30, bottom = 0;
        if (!lay)
            for (void* c : Children(page))
                if (c && IsA(c, "QWidget")) { const int* g = qt.geometry(c); left = std::min(left, g[0]); bottom = std::max(bottom, g[3]); }
        const int* rg = reset ? qt.geometry(reset) : nullptr;
        if (!lay && (!rg || left > rg[0] || bottom < rg[3])) { g_patchError = "the System tab's children have no usable geometry"; return; }

        Scan();
        std::string shown = g_applied.empty() ? std::string(themeicons::kClassicTreyarch) : g_applied;
        void* holder = lay ? page : ui->Widget(page);            // fixed layout: one child widget holding the row
        void* row = ui->HBox();
        ui->LayoutSpacing(row, 6);
        void* label = ui->Label("Theme:", holder);
        void* combo = QNew(128);
        qt.comboCtor(combo, holder);
        std::vector<std::string> names;
        int current = 0;
        for (const Entry& e : g_list)
        {
            void* text = nullptr; qt.qstringFromUtf8(&text, e.label.c_str(), int(e.label.size()));
            char variant[16] = {}; qt.variantCtor(variant);
            qt.comboAddItem(combo, &text, variant);
            qt.variantDtor(variant); qt.qsDtor(&text);
            if (Lower(e.name) == Lower(shown)) current = int(names.size());
            names.push_back(e.name);
        }
        if (themeedit::Edited())                  // the editor's unsaved edit is the look in effect
        {
            void* text = nullptr; qt.qstringFromUtf8(&text, "Edited (not saved)", -1);
            char variant[16] = {}; qt.variantCtor(variant);
            qt.comboAddItem(combo, &text, variant);
            qt.variantDtor(variant); qt.qsDtor(&text);
            current = int(names.size());
            names.push_back(kEditedName);
        }
        qt.comboSetCurrentIndex(combo, current);
        ui->SetToolTip(combo, "The look of Radiant (Radiant+ plugin). Classic Treyarch is Radiant's own stylesheet, Treyarch a modern theme in its colours, Classic is Qt's "
                              "plain Windows look; more themes are .qss files in bin\\themes. Picking one applies it.");
        void* folder = ui->PushButton("Themes Folder", holder);
        ui->SetToolTip(folder, "Open bin\\themes (shared with APE): drop a .qss file there and reopen Preferences");
        rp::host->OnClicked(rp::self, folder, OnOpenFolder, nullptr);
        ui->BoxAddWidget(row, label, 0);
        ui->BoxAddWidget(row, combo, 1);
        ui->BoxAddWidget(row, folder, 0);

        char where[160];
        if (!lay)
        {
            ui->LayoutMargins(row, 0, 0, 0, 0);
            ui->SetLayout(holder, row);
            int x = left + 10, y = bottom + 14, w = rg[2] - x;   // under the lowest child, from the left column to the Reset button's right edge
            qt.setGeometry(holder, x, y, w, 26);
            snprintf(where, sizeof where, "fixed %d,%d %dx26", x, y, w);
        }
        else if (onColor)
        {
            int r = qt.gridRowCount(lay), cols = qt.gridColumnCount(lay);
            qt.gridAddLayout(lay, row, r, 0, 1, cols > 0 ? cols : 1, 0);   // a new last row, under the page tabs
            snprintf(where, sizeof where, "Color page row %d", r);
        }
        else if (IsA(lay, "QBoxLayout"))
        {
            qt.boxInsertLayout(lay, index, row, 0);   // just above the Reset button
            snprintf(where, sizeof where, "%s index %d", ClassName(lay), index);
        }
        else
        {
            int r = qt.gridRowCount(lay), cols = qt.gridColumnCount(lay);
            qt.gridAddLayout(lay, row, r, 0, 1, cols > 0 ? cols : 1, 0);   // a new last row across every column
            snprintf(where, sizeof where, "%s row %d span %d", ClassName(lay), r, cols);
        }
        g_where = std::string(where) + " in " + TakeString(qt.objectName, page);

        int hooked = 0;
        for (size_t off : { off_Ui_Ok, off_Ui_Apply })
        {
            void* b = UiField(dialog, off);
            if (b && IsA(b, "QAbstractButton") && rp::host->OnClicked(rp::self, b, OnPrefsCommit, dialog)) ++hooked;
        }
        if (!hooked) g_patchError = "no OK or Apply button found; the choice cannot be applied";
        else g_patchError.clear();

        g_dlg.dialog = dialog; g_dlg.reset = reset; g_dlg.page = page; g_dlg.combo = combo; g_dlg.hooked = hooked; g_dlg.names = std::move(names);
        if (rp::Has(&rp::host->OnChanged)) rp::host->OnChanged(rp::self, combo, OnThemeComboChanged, dialog);   // Follow theme
        ++g_patched;
    }
    bool PatchGuarded(void* dialog)
    {
        __try { Patch(dialog); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool PatchGizmoGuarded(void* dialog);
    extern std::string g_gizmoError;
    bool PatchGameFovGuarded(void* dialog);
    extern std::string g_gameFovError;
    bool PatchDxvkGuarded(void* dialog);
    extern std::string g_dxvkError;
    bool PatchFileDialogsGuarded(void* dialog);
    extern std::string g_fileDialogsError;
    bool PatchDiscordGuarded(void* dialog);
    extern std::string g_discordError;
    void* __fastcall hk_PrefsCtor(void* self, void* parent)
    {
        void* r = g_origCtor(self, parent);
        g_dlg = Dialog{};
        if (self && !PatchGuarded(self)) { g_patchError = "the Preferences patch faulted; no theme row this time"; rp::Log("theme: %s", g_patchError.c_str()); }
        else if (!g_patchError.empty()) rp::Log("theme: Preferences: %s", g_patchError.c_str());
        if (self && !PatchColorsGuarded(self)) { ResetRow(); g_colorsError = "the Color tab patch faulted; no preset row this time"; }
        if (!g_colorsError.empty()) rp::Log("colors: Preferences: %s", g_colorsError.c_str());
        if (self && !PatchEditorGuarded(self)) { ResetEditor(); g_editorError = "the Theme editor patch faulted; no Theme tab this time"; }
        if (!g_editorError.empty()) rp::Log("theme: Preferences: editor: %s", g_editorError.c_str());
        if (self && !PatchGizmoGuarded(self)) g_gizmoError = "the Selection tab patch faulted; no gizmo boxes this time";
        if (!g_gizmoError.empty()) rp::Log("gizmo: Preferences: %s", g_gizmoError.c_str());
        if (self && !PatchGameFovGuarded(self)) g_gameFovError = "the Camera tab patch faulted; no game view FOV field this time";
        if (!g_gameFovError.empty()) rp::Log("gamefov: Preferences: %s", g_gameFovError.c_str());
        if (self && !PatchDxvkGuarded(self)) g_dxvkError = "the Camera tab patch faulted; no DXVK box this time";
        if (!g_dxvkError.empty()) rp::Log("dxvk: Preferences: %s", g_dxvkError.c_str());
        if (self && !PatchFileDialogsGuarded(self)) g_fileDialogsError = "the System tab patch faulted; no file dialog box this time";
        if (!g_fileDialogsError.empty()) rp::Log("filedialogs: Preferences: %s", g_fileDialogsError.c_str());
        if (self && !PatchDiscordGuarded(self)) g_discordError = "the System tab patch faulted; no Discord box this time";
        if (!g_discordError.empty()) rp::Log("discord: Preferences: %s", g_discordError.c_str());
        return r;
    }

    // builds a Preferences dialog that is never shown (through the detour), reports the patch, renders it to a PNG and
    // deletes it: verifies the row inside a running Radiant without a window on screen
    std::string SelfTest(const std::string& png)
    {
        if (!g_hooked || !g_ctorLive) return "the Preferences hook is not installed";
        void* dialog = QNew(0x400);
        g_patchError.clear();
        reinterpret_cast<PrefsCtor>(g_ctorLive)(dialog, nullptr);
        std::string out;
        char buf[512];
        if (g_dlg.dialog != dialog) out += "patch did not run: " + g_patchError + "\n" + Probe(dialog);
        else
        {
            snprintf(buf, sizeof buf, "patched: row at %s, combo visible on its page=%d, items=%d current=%d, OK/Apply hooked=%d\n",
                     g_where.c_str(), qt.isVisibleTo(g_dlg.combo, g_dlg.page) ? 1 : 0,
                     qt.comboCount(g_dlg.combo), qt.comboCurrentIndex(g_dlg.combo), g_dlg.hooked);
            out += buf;
            for (int i = 0; i < qt.comboCount(g_dlg.combo); ++i)
            {
                void* qs = nullptr; qt.comboItemText(g_dlg.combo, &qs, i);
                out += "  item " + std::to_string(i) + ": '" + QStringToUtf8(&qs) + "' -> " + (size_t(i) < g_dlg.names.size() ? g_dlg.names[size_t(i)] : "?") + "\n";
                qt.qsDtor(&qs);
            }
            if (!g_patchError.empty()) out += "note: " + g_patchError + "\n";
        }
        if (!png.empty() && qt.grab && qt.pixmapSave && qt.pixmapDtor)
        {
            // the page that holds the row (the Color page, or the System page in the fallback)
            void* target = g_dlg.page ? g_dlg.page : dialog;
            qt.adjustSize(dialog);
            int rect[4] = { 0, 0, -2, -2 };                   // QRect(QPoint(0, 0), QSize(-1, -1)): the whole widget
            char pixmap[64] = {};
            qt.grab(target ? target : dialog, pixmap, rect);
            void* path = nullptr; qt.qstringFromUtf8(&path, png.c_str(), int(png.size()));
            bool saved = qt.pixmapSave(pixmap, &path, "PNG", -1);
            qt.qsDtor(&path);
            qt.pixmapDtor(pixmap);
            out += saved ? "rendered to " + png + "\n" : "render to " + png + " failed\n";
            if (g_editorPage)                                    // the Theme editor tab, beside it
            {
                std::string png2 = png.substr(0, png.size() > 4 ? png.size() - 4 : png.size()) + "_editor.png";
                char pm2[64] = {};
                qt.adjustSize(g_editorPage);
                qt.grab(g_editorPage, pm2, rect);
                void* p2 = nullptr; qt.qstringFromUtf8(&p2, png2.c_str(), int(png2.size()));
                bool saved2 = qt.pixmapSave(pm2, &p2, "PNG", -1);
                qt.qsDtor(&p2); qt.pixmapDtor(pm2);
                out += saved2 ? "editor rendered to " + png2 + "\n" : "editor render failed\n";
            }
            else out += "no editor tab: " + g_editorError + "\n";
        }
        g_dlg = Dialog{};
        ResetRow();
        ResetEditor();
        DeleteNow(dialog);
        return out;
    }

    // the first widget under `root` (depth first) that is a button with this text
    void* FindButton(void* root, const std::string& text, int depth = 0)
    {
        for (void* c : Children(root))
        {
            if (!c || !IsA(c, "QWidget")) continue;
            if (IsA(c, "QAbstractButton") && TakeString(qt.buttonText, c) == text) return c;
            if (depth < 12) if (void* hit = FindButton(c, text, depth + 1)) return hit;
        }
        return nullptr;
    }
    // ---- rotation gizmo switches (Selection tab) ----
    // Two check boxes and a size slider under Radiant's rotation options. m_SelectionTab has no layout (fixed geometry, lowest child at y 229),
    // so the rows are one child widget under the lowest child. OK or Apply stores them (rotgizmo.cpp keeps the state and ini).
    struct GizmoRow { void* dialog = nullptr; void* rings = nullptr; void* handles = nullptr; void* size = nullptr; void* sizeText = nullptr; } g_gizmoRow;
    void OnGizmoSizeMoved(void*, int value)
    {
        const ModToolsUiApi* ui = rp::host->ui;
        if (!ui || !g_gizmoRow.sizeText) return;
        char text[24];
        snprintf(text, sizeof(text), "%d px", value);
        ui->SetLabelText(g_gizmoRow.sizeText, text);
    }
    std::string g_gizmoError;   // declared above hk_PrefsCtor
    void OnGizmoCommit(void* dialog)
    {
        const ModToolsUiApi* ui = rp::host->ui;
        if (!ui || !dialog || dialog != g_gizmoRow.dialog || !g_gizmoRow.rings || !g_gizmoRow.handles) return;
        br::rotgizmo::Set(ui->IsChecked(g_gizmoRow.rings) != 0, ui->IsChecked(g_gizmoRow.handles) != 0);
        if (g_gizmoRow.size) br::rotgizmo::SetSize(ui->SliderValue(g_gizmoRow.size));
    }
    void PatchGizmo(void* dialog)
    {
        g_gizmoRow = GizmoRow{};
        const ModToolsUiApi* ui = rp::host->ui;
        if (!ui) { g_gizmoError = "no host ui"; return; }
        void* anchor = FindButton(dialog, "Use grid size for rotation");
        void* form = anchor ? qt.parentWidget(anchor) : nullptr;
        void* page = form ? qt.parentWidget(form) : nullptr;
        if (!page || qt.widgetLayout(page)) { g_gizmoError = "the Selection tab is not where expected"; return; }
        int left = qt.geometry(form)[0], bottom = 0, right = 0;
        for (void* c : Children(page))
            if (c && IsA(c, "QWidget")) { const int* g = qt.geometry(c); bottom = std::max(bottom, g[3]); right = std::max(right, g[2]); }
        if (bottom <= 0 || right <= left) { g_gizmoError = "the Selection tab's children have no usable geometry"; return; }
        void* holder = ui->Widget(page);
        void* row = ui->HBox();
        ui->LayoutMargins(row, 0, 0, 0, 0);
        ui->LayoutSpacing(row, 18);
        void* rings = ui->CheckBox("Rotation gizmo rings", holder);
        void* handles = ui->CheckBox("Manipulator handles in Mouse Rotate", holder);
        ui->SetChecked(rings, br::rotgizmo::Rings() ? 1 : 0);
        ui->SetChecked(handles, br::rotgizmo::RadiantHandles() ? 1 : 0);
        ui->SetToolTip(rings, "Radiant+: while Mouse Rotate is on, the 3D view shows three axis rings around the pivot; drag a ring to "
                              "turn the selection about that axis. Follows snap to grid (Radiant's rotation step for the grid size), Shift snaps to 15 degrees.");
        ui->SetToolTip(handles, "Radiant+: Radiant's own straight handles at the pivot while Mouse Rotate is on. Off = only the rings there; "
                                "outside Mouse Rotate the handles follow \"Manipulator Handles\" above.");
        ui->BoxAddWidget(row, rings, 0);
        ui->BoxAddWidget(row, handles, 0);
        ui->BoxAddStretch(row, 1);
        // second row: the ring radius
        void* sizeRow = ui->HBox();
        ui->LayoutMargins(sizeRow, 0, 0, 0, 0);
        ui->LayoutSpacing(sizeRow, 8);
        void* sizeLabel = ui->Label("Rotation gizmo size", holder);
        void* size = ui->Slider(1, 30, 400, holder);
        void* sizeText = ui->Label("", holder);
        ui->SetMinimumSize(size, 220, 0);
        ui->SetMinimumSize(sizeText, 48, 0);
        const std::string sizeTip = std::string("Radiant+: the radius of the rotation rings in pixels.") + (br::rotgizmo::SizeWheel() ? " Also the mouse wheel with the cursor on a ring." : "");
        ui->SetToolTip(sizeLabel, sizeTip.c_str());
        ui->SetToolTip(size, sizeTip.c_str());
        ui->BoxAddWidget(sizeRow, sizeLabel, 0);
        ui->BoxAddWidget(sizeRow, size, 0);
        ui->BoxAddWidget(sizeRow, sizeText, 0);
        ui->BoxAddStretch(sizeRow, 1);
        void* rows = ui->VBox(holder);
        ui->LayoutMargins(rows, 0, 0, 0, 0);
        ui->LayoutSpacing(rows, 6);
        ui->BoxAddLayout(rows, row, 0);
        ui->BoxAddLayout(rows, sizeRow, 0);
        qt.setGeometry(holder, left, bottom + 12, right - left, 54);
        g_gizmoRow.sizeText = sizeText;      // before the value is set: the slider reports it
        rp::host->OnChanged(rp::self, size, OnGizmoSizeMoved, nullptr);
        ui->SliderSetValue(size, br::rotgizmo::SizePx());
        OnGizmoSizeMoved(nullptr, br::rotgizmo::SizePx());
        int hooked = 0;
        for (size_t off : { off_Ui_Ok, off_Ui_Apply })
        {
            void* b = UiField(dialog, off);
            if (b && IsA(b, "QAbstractButton") && rp::host->OnClicked(rp::self, b, OnGizmoCommit, dialog)) ++hooked;
        }
        g_gizmoError = hooked ? "" : "no OK or Apply button found";
        g_gizmoRow.dialog = dialog; g_gizmoRow.rings = rings; g_gizmoRow.handles = handles; g_gizmoRow.size = size;
    }
    bool PatchGizmoGuarded(void* dialog)
    {
        __try { PatchGizmo(dialog); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // ---- game view FOV (Camera tab) ----
    // A "GameView FOV" box on the same row as Radiant's "Field of View (non GameView)" line edit (m_FOVTbx): that edit is swapped in the
    // layout for a holder widget that takes it, our label and our box. OK or Apply stores the value (gamefov.cpp keeps it and the ini).
    struct GameFovRow { void* dialog = nullptr; void* edit = nullptr; } g_gameFovRow;
    std::string g_gameFovError;   // declared above hk_PrefsCtor
    void* FindByName(void* obj, const char* name, int depth = 0)
    {
        for (void* c : Children(obj))
        {
            if (!c) continue;
            if (TakeString(qt.objectName, c) == name) return c;
            if (depth < 12) if (void* hit = FindByName(c, name, depth + 1)) return hit;
        }
        return nullptr;
    }
    void OnGameFovCommit(void* dialog)
    {
        const ModToolsUiApi* ui = rp::host->ui;
        if (!ui || !dialog || dialog != g_gameFovRow.dialog || !g_gameFovRow.edit) return;
        char text[64] = {};
        ui->EditText(g_gameFovRow.edit, text, sizeof text);
        char* end = nullptr;
        const float v = strtof(text, &end);
        if (end != text && v > 0) br::gamefov::Set(v);
        char shown[32];
        snprintf(shown, sizeof shown, "%g", br::gamefov::Fov());   // the clamped value, or the old one after bad input
        ui->SetEditText(g_gameFovRow.edit, shown);
    }
    void PatchGameFov(void* dialog)
    {
        g_gameFovRow = GameFovRow{};
        const ModToolsUiApi* ui = rp::host->ui;
        if (!ui) { g_gameFovError = "no host ui"; return; }
        if (!qt.layoutReplaceWidget) { g_gameFovError = "QLayout::replaceWidget is not exported by this Qt"; return; }
        void* fov = FindByName(dialog, "m_FOVTbx");
        void* page = fov && IsA(fov, "QLineEdit") ? qt.parentWidget(fov) : nullptr;
        void* lay = page ? qt.widgetLayout(page) : nullptr;
        if (!lay) { g_gameFovError = "the Camera tab's Field of View box is not where expected"; return; }
        void* holder = ui->Widget(page);
        // 1 = Qt::FindChildrenRecursively: the box sits in a nested layout of the tab
        if (!qt.layoutReplaceWidget(lay, fov, holder, 1)) { DeleteNow(holder); g_gameFovError = "the Field of View box could not be swapped"; return; }
        // the returned QLayoutItem (the old cell's wrapper) is left alone: a few bytes per dialog
        void* row = ui->HBox();
        ui->LayoutMargins(row, 0, 0, 0, 0);
        ui->LayoutSpacing(row, 8);
        void* label = ui->Label("GameView FOV:", holder);
        char text[32];
        snprintf(text, sizeof text, "%g", br::gamefov::Fov());
        void* edit = ui->LineEdit(text, holder);
        ui->SetMaximumWidth(edit, 80);
        char tip[320];
        snprintf(tip, sizeof tip, "Radiant+: the field of view of GAME VIEW, as the game's FOV setting (cg_fov: horizontal degrees of a 4:3 view). "
                                  "65 = Radiant's own (the game's default), %g to %g. The box on the left only applies outside game view.",
                 br::gamefov::kMin, br::gamefov::kMax);
        ui->SetToolTip(label, tip);
        ui->SetToolTip(edit, tip);
        ui->BoxAddWidget(row, fov, 1);   // reparents Radiant's box into the holder, Radiant's own pointer to it stays valid
        ui->BoxAddWidget(row, label, 0);
        ui->BoxAddWidget(row, edit, 0);
        ui->SetLayout(holder, row);
        int hooked = 0;
        for (size_t off : { off_Ui_Ok, off_Ui_Apply })
        {
            void* b = UiField(dialog, off);
            if (b && IsA(b, "QAbstractButton") && rp::host->OnClicked(rp::self, b, OnGameFovCommit, dialog)) ++hooked;
        }
        g_gameFovError = hooked ? "" : "no OK or Apply button found";
        g_gameFovRow.dialog = dialog; g_gameFovRow.edit = edit;
    }
    bool PatchGameFovGuarded(void* dialog)
    {
        __try { PatchGameFov(dialog); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // ---- DXVK (Vulkan) renderer (Camera tab) ----
    // A check box on the row of Radiant's "Always start in fullbright" (m_FullbrightCbx), swapped into a holder like the GameView FOV box.
    // Ticked = DXVK's files are in bin (shared with APE). OK or Apply hands a change to radiantplus.cpp (applies after Preferences closes).
    struct DxvkRow { void* dialog = nullptr; void* box = nullptr; } g_dxvkRow;
    std::string g_dxvkError;   // declared above hk_PrefsCtor
    void OnDxvkCommit(void* dialog)
    {
        const ModToolsUiApi* ui = rp::host->ui;
        if (!ui || !dialog || dialog != g_dxvkRow.dialog || !g_dxvkRow.box) return;
        const bool on = ui->IsChecked(g_dxvkRow.box) != 0;
        if (on != br::dxvk::Setting()) br::dxvk::Request(on);
    }
    void PatchDxvk(void* dialog)
    {
        g_dxvkRow = DxvkRow{};
        const ModToolsUiApi* ui = rp::host->ui;
        if (!ui) { g_dxvkError = "no host ui"; return; }
        if (!qt.layoutReplaceWidget) { g_dxvkError = "QLayout::replaceWidget is not exported by this Qt"; return; }
        void* full = FindByName(dialog, "m_FullbrightCbx");
        void* page = full && IsA(full, "QCheckBox") ? qt.parentWidget(full) : nullptr;
        void* lay = page ? qt.widgetLayout(page) : nullptr;
        if (!lay) { g_dxvkError = "the Camera tab's fullbright box is not where expected"; return; }
        void* holder = ui->Widget(page);
        if (!qt.layoutReplaceWidget(lay, full, holder, 1)) { DeleteNow(holder); g_dxvkError = "the fullbright box could not be swapped"; return; }
        void* row = ui->HBox();
        ui->LayoutMargins(row, 0, 0, 0, 0);
        ui->LayoutSpacing(row, 18);
        void* box = ui->CheckBox("Use DXVK (Vulkan) renderer", holder);
        ui->SetChecked(box, br::dxvk::Setting() ? 1 : 0);
        ui->SetToolTip(box, "Radiant+: draw through Vulkan with DXVK instead of Direct3D 11 (often a higher frame rate). "
                            "Takes effect when Radiant restarts (you are asked after OK or Apply). Shared with APE, which "
                            "uses it too while on.");
        ui->BoxAddWidget(row, full, 0);   // reparents Radiant's box into the holder, Radiant's own pointer to it stays valid
        ui->BoxAddWidget(row, box, 0);
        ui->BoxAddStretch(row, 1);
        ui->SetLayout(holder, row);
        int hooked = 0;
        for (size_t off : { off_Ui_Ok, off_Ui_Apply })
        {
            void* b = UiField(dialog, off);
            if (b && IsA(b, "QAbstractButton") && rp::host->OnClicked(rp::self, b, OnDxvkCommit, dialog)) ++hooked;
        }
        g_dxvkError = hooked ? "" : "no OK or Apply button found";
        g_dxvkRow.dialog = dialog; g_dxvkRow.box = box;
    }
    bool PatchDxvkGuarded(void* dialog)
    {
        __try { PatchDxvk(dialog); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // ---- Windows file dialogs (System tab) ----
    // One check box under the lowest child of m_SystemTab (no layout, fixed geometry). OK or Apply stores it, the next dialog follows it.
    struct FileDialogsRow { void* dialog = nullptr; void* box = nullptr; } g_fileDialogsRow;
    std::string g_fileDialogsError;   // declared above hk_PrefsCtor
    void OnFileDialogsCommit(void* dialog)
    {
        const ModToolsUiApi* ui = rp::host->ui;
        if (!ui || !dialog || dialog != g_fileDialogsRow.dialog || !g_fileDialogsRow.box) return;
        br::filedialogs::Set(ui->IsChecked(g_fileDialogsRow.box) != 0);
    }
    void PatchFileDialogs(void* dialog)
    {
        g_fileDialogsRow = FileDialogsRow{};
        const ModToolsUiApi* ui = rp::host->ui;
        if (!ui) { g_fileDialogsError = "no host ui"; return; }
        void* reset = UiField(dialog, off_Ui_Reset);
        void* page = reset && IsA(reset, "QPushButton") ? qt.parentWidget(reset) : nullptr;
        if (!page || qt.widgetLayout(page)) { g_fileDialogsError = "the System tab is not where expected"; return; }
        int left = 1 << 30, bottom = 0, right = 0;
        for (void* c : Children(page))
            if (c && IsA(c, "QWidget")) { const int* g = qt.geometry(c); left = std::min(left, g[0]); bottom = std::max(bottom, g[3]); right = std::max(right, g[2]); }
        if (bottom <= 0 || right <= left) { g_fileDialogsError = "the System tab's children have no usable geometry"; return; }
        void* box = ui->CheckBox("Use Windows file dialogs", page);
        ui->SetChecked(box, br::filedialogs::Enabled() ? 1 : 0);
        ui->SetToolTip(box, "Radiant+: Open, Save and Import use the Windows (Explorer) file dialog instead of Radiant's own. "
                            "Applies from the next dialog opened. Radiant's shortcut list on the left of its own dialog is not shown there.");
        qt.setGeometry(box, left + 10, bottom + 14, right - left - 10, 22);
        int hooked = 0;
        for (size_t off : { off_Ui_Ok, off_Ui_Apply })
        {
            void* b = UiField(dialog, off);
            if (b && IsA(b, "QAbstractButton") && rp::host->OnClicked(rp::self, b, OnFileDialogsCommit, dialog)) ++hooked;
        }
        g_fileDialogsError = hooked ? "" : "no OK or Apply button found";
        g_fileDialogsRow.dialog = dialog; g_fileDialogsRow.box = box;
    }
    bool PatchFileDialogsGuarded(void* dialog)
    {
        __try { PatchFileDialogs(dialog); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // ---- Discord presence (System tab) ----
    // A label and a combo under the file dialogs box (runs after PatchFileDialogs). OK or Apply stores it.
    struct DiscordRow { void* dialog = nullptr; void* box = nullptr; } g_discordRow;
    std::string g_discordError;   // declared above hk_PrefsCtor
    void OnDiscordCommit(void* dialog)
    {
        const ModToolsUiApi* ui = rp::host->ui;
        if (!ui || !dialog || dialog != g_discordRow.dialog || !g_discordRow.box) return;
        const int mode = ui->ComboCurrent(g_discordRow.box);
        if (mode >= 0 && mode != discordrpc::GetMode()) discordrpc::SetMode(discordrpc::Mode(mode));
    }
    void PatchDiscord(void* dialog)
    {
        g_discordRow = DiscordRow{};
        const ModToolsUiApi* ui = rp::host->ui;
        if (!ui) { g_discordError = "no host ui"; return; }
        void* reset = UiField(dialog, off_Ui_Reset);
        void* page = reset && IsA(reset, "QPushButton") ? qt.parentWidget(reset) : nullptr;
        if (!page || qt.widgetLayout(page)) { g_discordError = "the System tab is not where expected"; return; }
        void* above = g_fileDialogsRow.dialog == dialog ? g_fileDialogsRow.box : nullptr;
        int left = 1 << 30, bottom = 0, right = 0;
        for (void* c : Children(page))
            if (c && IsA(c, "QWidget")) { const int* g = qt.geometry(c); if (c != above) left = std::min(left, g[0]); bottom = std::max(bottom, g[3]); right = std::max(right, g[2]); }
        if (bottom <= 0 || right <= left) { g_discordError = "the System tab's children have no usable geometry"; return; }
        void* box = ui->ComboBox(page);
        if (!box) { g_discordError = "no combo box"; return; }
        for (const char* label : discordrpc::kModeLabels) ui->ComboAdd(box, label);
        ui->ComboSetCurrent(box, discordrpc::GetMode());
        ui->SetToolTip(box, "Radiant+: what your Discord profile shows while Radiant is in use. Off: nothing. Show the tool: only which tool is open. Show the tool and what is open: also the map, asset or build step. Each tool has its own setting.");
        void* label = ui->Label("Discord presence:", page);
        const int top = bottom + (above ? 6 : 14);
        qt.setGeometry(label, left + 10, top, 120, 24);
        qt.setGeometry(box, left + 132, top, 240, 24);
        int hooked = 0;
        for (size_t off : { off_Ui_Ok, off_Ui_Apply })
        {
            void* b = UiField(dialog, off);
            if (b && IsA(b, "QAbstractButton") && rp::host->OnClicked(rp::self, b, OnDiscordCommit, dialog)) ++hooked;
        }
        g_discordError = hooked ? "" : "no OK or Apply button found";
        g_discordRow.dialog = dialog; g_discordRow.box = box;
    }
    bool PatchDiscordGuarded(void* dialog)
    {
        __try { PatchDiscord(dialog); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool PrefsWindowOpen();
    // "theme prefstree <button text>[|<png>]": a Preferences dialog that is never shown, prints the page that holds that button
    std::string PrefsTree(const std::string& textAndPng)
    {
        const size_t bar = textAndPng.find('|');
        const std::string text = textAndPng.substr(0, bar), png = bar == std::string::npos ? "" : textAndPng.substr(bar + 1);
        if (!g_hooked || !g_ctorLive) return "the Preferences hook is not installed";
        if (PrefsWindowOpen()) return "close Preferences first (the probe builds its own dialog)";
        void* dialog = QNew(0x400);
        reinterpret_cast<PrefsCtor>(g_ctorLive)(dialog, nullptr);
        std::string out;
        void* b = FindButton(dialog, text);
        if (!b) out = "no button \"" + text + "\"";
        else { out = "chain:\n"; for (void* p = b; p; p = qt.parentWidget(p)) out += "  " + Describe(p) + "\n"; out += "page tree:\n"; void* page = qt.parentWidget(b); while (page && qt.parentWidget(page) && !IsA(qt.parentWidget(page), "QStackedWidget")) page = qt.parentWidget(page); if (page) Dump(page, 1, out);
            if (page && !png.empty() && qt.grab && qt.pixmapSave && qt.pixmapDtor)
            {
                int rect[4] = { 0, 0, -2, -2 }; char pm[64] = {};
                qt.grab(page, pm, rect);
                void* path = nullptr; qt.qstringFromUtf8(&path, png.c_str(), int(png.size()));
                out += qt.pixmapSave(pm, &path, "PNG", -1) ? "rendered to " + png + "\n" : "render failed\n";
                qt.qsDtor(&path); qt.pixmapDtor(pm);
            }
            if (!g_gizmoError.empty()) out += "gizmo boxes: " + g_gizmoError + "\n";
            if (!g_gameFovError.empty()) out += "game view FOV: " + g_gameFovError + "\n";
        }
        g_dlg = Dialog{};
        g_gizmoRow = GizmoRow{};
        g_gameFovRow = GameFovRow{};
        ResetRow();
        ResetEditor();
        DeleteNow(dialog);
        return out;
    }

    bool InitGuarded(void* dialog)
    {
        __try { g_prefsInit(dialog); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    float MaxDiff(const float (*a)[4], const float (*b)[4], bool tabOnly)
    {
        float m = 0;
        for (int i = 0; i < 48; ++i)
            if (!tabOnly || OnColorTab(i))
                for (int c = 0; c < 4; ++c) m = std::max(m, fabsf(a[i][c] - b[i][c]));
        return m;
    }
    // "colors selftest [<preset>][|<png>]": a Preferences dialog that is never shown, loaded like Radiant loads it, the
    // preset goes into its swatches through the row, the table must stay as it was, the Color tab is rendered to a PNG
    bool PrefsWindowOpen();
    std::string ColorsSelfTest(const std::string& arg)
    {
        if (!g_hooked || !g_ctorLive || !g_colorsOn) return "colors: the Preferences hook or the color table is not available";
        if (PrefsWindowOpen()) return "colors: close Preferences first (the self test builds its own dialog)";
        size_t bar = arg.find('|');
        std::string presetName = arg.substr(0, bar), png = bar == std::string::npos ? "" : arg.substr(bar + 1);
        static float before[48][4], shown[48][4], after[48][4];
        memcpy(before, g_table, sizeof before);
        void* dialog = QNew(0x400);
        reinterpret_cast<PrefsCtor>(g_ctorLive)(dialog, nullptr);
        std::string out;
        char buf[512];
        if (g_row.dialog != dialog) out += "row not patched: " + g_colorsError + "\n";
        else
        {
            snprintf(buf, sizeof buf, "row at %s, presets=%zu\n", g_colorsWhere.c_str(), g_row.names.size() - 1);
            out += buf;
            if (!InitGuarded(dialog)) out += "Preferences Init faulted\n";
            else if (!ReadSwatches(dialog, shown)) out += "reading the swatches faulted\n";
            else
            {
                snprintf(buf, sizeof buf, "loaded swatches vs table: max diff %.4f (8-bit swatches: <= 0.004 expected)\n", MaxDiff(shown, g_table, true));
                out += buf;
                const Preset* p = presetName.empty() ? nullptr : FindPreset(presetName);
                if (!presetName.empty() && !p) out += "no preset '" + presetName + "'\n";
                if (p)
                {
                    int idx = -1;
                    for (size_t i = 0; i < g_row.names.size(); ++i) if (g_row.names[i] == p->name) idx = int(i);
                    rp::host->ui->ComboSetCurrent(g_row.combo, idx);   // as a user's pick: OnChanged -> preview
                    ReadSwatches(dialog, shown);
                    float worst = 0, sharedWorst = 0; int tabRows = 0, extraRows = 0, sharedRows = 0;
                    for (const ColorValue& v : p->values)
                    {
                        if (!OnColorTab(v.index)) { extraRows += g_row.hasExtra[v.index] ? 1 : 0; continue; }
                        if (!g_shared && IsShared(v.index)) continue;
                        ++tabRows;
                        for (int c = 0; c < (v.hasAlpha ? 4 : 3); ++c) worst = std::max(worst, fabsf(shown[v.index][c] - v.rgba[c]));
                    }
                    if (!g_shared)   // the Shared tab shows Radiant's defaults
                        for (int i = 0; i < 48; ++i)
                            if (IsShared(i) && OnColorTab(i) && i != 6)    // COLOR_LIGHT's default (0, 8, 0) does not fit a swatch
                            {
                                ++sharedRows;
                                for (int c = 0; c < 3; ++c) sharedWorst = std::max(sharedWorst, fabsf(shown[i][c] - kRadiantDefault[i][c]));
                            }
                    snprintf(buf, sizeof buf, "preset '%s': %d swatch rows, max diff %.4f; %d rows wait for OK; table untouched=%d\n",
                             p->name.c_str(), tabRows, worst, extraRows, MaxDiff(before, g_table, false) == 0 ? 1 : 0);
                    out += buf;
                    if (!g_shared) { snprintf(buf, sizeof buf, "shared off: %d Shared swatches vs Radiant's defaults, max diff %.4f\n", sharedRows, sharedWorst); out += buf; }
                }
            }
            if (!png.empty() && qt.grab && qt.pixmapSave && qt.pixmapDtor && g_row.page)
            {
                qt.adjustSize(dialog);
                int rect[4] = { 0, 0, -2, -2 };
                char pixmap[64] = {};
                qt.grab(g_row.page, pixmap, rect);
                void* path = nullptr; qt.qstringFromUtf8(&path, png.c_str(), int(png.size()));
                bool saved = qt.pixmapSave(pixmap, &path, "PNG", -1);
                qt.qsDtor(&path);
                qt.pixmapDtor(pixmap);
                out += saved ? "rendered to " + png + "\n" : "render to " + png + " failed\n";
            }
        }
        ResetRow();
        ResetEditor();
        g_dlg = Dialog{};
        DeleteNow(dialog);
        memcpy(after, g_table, sizeof after);
        out += MaxDiff(before, after, false) == 0 ? "table unchanged after the test\n" : "TABLE CHANGED by the test\n";
        return out;
    }
    // the Color tab's swatches: table row -> Ui field (the Color load 0x140958B60 calls 0x14095A130(dialog, row, swatch))
    const struct { int row; int off; } kSwatches[43] = {
        { 4, 2432 }, { 0, 2088 }, { 1, 2528 }, { 3, 2568 }, { 2, 2504 }, { 6, 2104 }, { 8, 2480 }, { 7, 2536 }, { 10, 2600 },
        { 14, 2120 }, { 19, 2136 }, { 9, 2152 }, { 17, 2168 }, { 18, 2216 }, { 21, 2008 }, { 22, 2280 }, { 23, 2296 },
        { 25, 1896 }, { 37, 1952 }, { 27, 1904 }, { 26, 1936 }, { 11, 1920 }, { 16, 1984 }, { 12, 1992 }, { 13, 2584 },
        { 15, 2024 }, { 20, 2056 }, { 28, 2064 }, { 24, 2184 }, { 32, 2232 }, { 34, 2200 }, { 35, 2072 }, { 33, 2248 },
        { 36, 2264 }, { 38, 2408 }, { 39, 2440 }, { 40, 2424 }, { 41, 2400 }, { 43, 2520 }, { 44, 2376 }, { 45, 2368 },
        { 46, 2344 }, { 47, 2616 } };
    // "colors tabs": which inner tab (Shared / Camera View / Grid View) each swatch sits on, from an unshown dialog
    std::string ColorsTabs()
    {
        if (!g_hooked || !g_ctorLive || !qt.tabIndexOf || !qt.tabText) return "colors: the Preferences hook or QTabWidget exports are missing";
        if (PrefsWindowOpen()) return "colors: close Preferences first (the probe builds its own dialog)";
        void* dialog = QNew(0x400);
        reinterpret_cast<PrefsCtor>(g_ctorLive)(dialog, nullptr);
        std::string out;
        void* tabs = nullptr; void* page = nullptr; void* lay = nullptr;
        if (!ColorPage(dialog, tabs, page, lay) || !tabs) out = "the Color page tabs were not found";
        else
            for (const auto& s : kSwatches)
            {
                void* w = UiField(dialog, size_t(s.off));
                std::string tab = "?";
                for (void* x = w; x; x = qt.parentWidget(x))
                {
                    void* up = qt.parentWidget(x);
                    if (up && IsA(up, "QStackedWidget") && qt.parentWidget(up) == tabs)
                    {
                        int i = qt.tabIndexOf(tabs, x);
                        void* qs = nullptr; qt.tabText(tabs, &qs, i);
                        tab = QStringToUtf8(&qs); qt.qsDtor(&qs);
                        break;
                    }
                }
                out += std::to_string(s.row) + " " + kColorNames[s.row] + " (" + ClassName(w) + "): " + tab + "\n";
            }
        ResetRow();
        ResetEditor();
        g_dlg = Dialog{};
        DeleteNow(dialog);
        return out;
    }
    // the Preferences dialog is on screen in this process (g_row.dialog is only valid while it is)
    bool PrefsWindowOpen()
    {
        struct Find { static BOOL CALLBACK Cb(HWND h, LPARAM lp) {
            DWORD pid = 0; GetWindowThreadProcessId(h, &pid); wchar_t t[64] = {};
            if (pid == GetCurrentProcessId() && IsWindowVisible(h) && GetWindowTextW(h, t, 64) && wcscmp(t, L"Preferences") == 0) { *reinterpret_cast<bool*>(lp) = true; return FALSE; }
            return TRUE; } };
        bool open = false;
        EnumWindows(Find::Cb, reinterpret_cast<LPARAM>(&open));
        return open;
    }
    // Live view colors: while Preferences is on screen the swatches go into Radiant's table as soon as they change (preset, Follow
    // theme, Shared box, a picked swatch), so no OK or Apply is needed. Every 150 ms Radiant's Color store runs on a borrowed table and
    // only a changed result is kept. Cancel keeps the change, like a theme pick.
    struct Live { void* dialog = nullptr; float last[48][4] = {}; uint64_t next = 0; } g_live;
    bool StoreGuarded(void* dialog)
    {
        __try { g_tabStore(dialog); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void LiveColors()
    {
        if (!g_colorsOn || !g_tabStore || !g_row.dialog) return;
        const uint64_t now = GetTickCount64();
        if (now < g_live.next) return;
        g_live.next = now + 150;
        if (!PrefsWindowOpen()) { g_live.dialog = nullptr; return; }
        static float saved[48][4], shown[48][4];
        memcpy(saved, g_table, sizeof saved);
        if (!StoreGuarded(g_row.dialog)) { memcpy(g_table, saved, sizeof saved); g_colorsError = "live colors: reading the swatches faulted"; g_row.dialog = nullptr; return; }
        memcpy(shown, g_table, sizeof shown);
        if (g_live.dialog != g_row.dialog)                    // just opened: the baseline
        {
            g_live.dialog = g_row.dialog;
            memcpy(g_live.last, shown, sizeof shown);
            memcpy(g_table, saved, sizeof saved);
            return;
        }
        if (memcmp(shown, g_live.last, sizeof shown) == 0) { memcpy(g_table, saved, sizeof saved); return; }
        memcpy(g_live.last, shown, sizeof shown);
        for (int i = 0; i < 48; ++i) if (g_row.hasExtra[i]) memcpy(g_table[i], g_row.extra[i], sizeof g_row.extra[0]);
        SaveColorsToRegistry();
        RedrawViews();
    }

    // test driver for an open dialog, through the same signals a user's clicks send: "colors dialog <preset>",
    // "colors dialog theme <name>" (the System tab's Theme combo), "colors dialog ok|apply|cancel|saveas"
    std::string ColorsDialog(const std::string& arg)
    {
        const ModToolsUiApi* ui = rp::host->ui;
        if (!g_row.dialog || !PrefsWindowOpen()) return "colors: no Preferences dialog open";
        void* d = g_row.dialog;
        if (arg == "ok" || arg == "apply" || arg == "cancel" || arg == "saveas")
        {
            void* b = arg == "saveas" ? g_row.save : UiField(d, arg == "ok" ? off_Ui_Ok : arg == "apply" ? off_Ui_Apply : 0xA58);
            if (!b || !rp::HasUi(&ui->ClickButton)) return "colors: button not found";
            ui->ClickButton(b);
            return "clicked " + arg;
        }
        if (arg.rfind("editor ", 0) == 0)                        // the Theme tab: base <theme> | set <0-7> <#rrggbb> | preview | saveas | grab <png>
        {
            std::string e = arg.substr(7);
            if (e.rfind("grab ", 0) == 0 && qt.grab)
            {
                std::string png = e.substr(5);
                int rect[4] = { 0, 0, -2, -2 }; char pm[64] = {};
                qt.grab(d, pm, rect);
                void* path = nullptr; qt.qstringFromUtf8(&path, png.c_str(), int(png.size()));
                bool saved = qt.pixmapSave(pm, &path, "PNG", -1);
                qt.qsDtor(&path); qt.pixmapDtor(pm);
                return saved ? "dialog rendered to " + png : "render failed";
            }
            return themeedit::Command(e);
        }
        if (arg.rfind("theme ", 0) == 0)
        {
            std::string want = Lower(arg.substr(6));
            for (size_t i = 0; i < g_dlg.names.size(); ++i)
                if (Lower(g_dlg.names[i]) == want) { qt.comboSetCurrentIndex(g_dlg.combo, int(i)); break; }
        }
        else
        {
            const Preset* p = FindPreset(arg);
            if (!p) return "colors: no preset '" + arg + "'";
            for (size_t i = 0; i < g_row.names.size(); ++i) if (g_row.names[i] == p->name) ui->ComboSetCurrent(g_row.combo, int(i));
        }
        static float shown[48][4];
        if (!ReadSwatches(d, shown)) return "colors: reading the swatches faulted";
        int i = ui->ComboCurrent(g_row.combo);
        char buf[384];
        snprintf(buf, sizeof buf, "preset combo '%s'; swatches GRIDBACK %.3f %.3f %.3f, LIGHT %.3f %.3f %.3f, SELBRUSHES %.3f %.3f %.3f; table GRIDBACK %.3f %.3f %.3f",
                 i > 0 && size_t(i) < g_row.names.size() ? g_row.names[size_t(i)].c_str() : "-", shown[1][0], shown[1][1], shown[1][2],
                 shown[6][0], shown[6][1], shown[6][2], shown[10][0], shown[10][1], shown[10][2], g_table[1][0], g_table[1][1], g_table[1][2]);
        return buf;
    }
    std::string ColorsList()
    {
        ScanPresets();
        std::string s;
        for (const Preset& p : g_presets)
            s += p.name + " (" + std::to_string(p.values.size()) + " colors)" + (p.file.empty() ? "" : "  " + Narrow(p.file)) + "\n";
        return s;
    }
    std::string ColorsStatus()
    {
        char buf[512];
        snprintf(buf, sizeof buf, "colors: %s, follow_theme=%d, shared=%d, row at %s, last preset '%s'%s%s\nGRIDBACK now %.3f %.3f %.3f, CAMERABACK %.3f %.3f %.3f",
                 g_colorsOn ? "on" : "off", g_follow ? 1 : 0, g_shared ? 1 : 0, g_colorsWhere.empty() ? "-" : g_colorsWhere.c_str(), g_lastPreset.c_str(),
                 g_colorsError.empty() ? "" : ", last problem: ", g_colorsError.c_str(),
                 g_table ? g_table[1][0] : 0.f, g_table ? g_table[1][1] : 0.f, g_table ? g_table[1][2] : 0.f,
                 g_table ? g_table[4][0] : 0.f, g_table ? g_table[4][1] : 0.f, g_table ? g_table[4][2] : 0.f);
        return buf;
    }

    // ---- relayout after a style change ----
    // A style change gives buttons new sizes, but a layout that is not active (a pane behind another tab, a toolbar) keeps its old
    // item positions and buttons overlap until the pane is reopened.
    std::vector<void*> AllWidgets()
    {
        std::vector<void*> out;
        if (!qt.allWidgets || !qt.listDispose) return out;
        void* list = nullptr;
        qt.allWidgets(&list);
        auto* d = static_cast<QListDataD*>(list);
        if (!d) return out;
        for (int i = d->begin; i < d->end; ++i) out.push_back(d->array[i]);
        if (d->ref != -1 && InterlockedDecrement(reinterpret_cast<volatile LONG*>(&d->ref)) == 0) qt.listDispose(d);   // ~QList
        return out;
    }
    // QLayout::invalidate is QLayoutItem's virtual (QLayoutItem is QLayout's second base, after QObject's 16 bytes), slot 11
    // after the destructor, sizeHint ... minimumHeightForWidth. Called through that vtable so the box, grid, tool bar and
    // main window layouts clear their own caches too, the slot is checked against QBoxLayout::invalidate before use.
    const size_t kLayoutItemOffset = 16, kInvalidateSlot = 11;
    int g_invalidateOk = -1;                   // -1 not checked yet, 0 the slot does not match (relayout off), 1 checked
    void InvalidateVirtual(void* lay)
    {
        char* item = static_cast<char*>(lay) + kLayoutItemOffset;
        (*reinterpret_cast<void (__fastcall***)(void*)>(item))[kInvalidateSlot](item);
    }
    // Radiant's own copies of Qt vtables hold jump stubs into the Qt DLLs (E9 rel32, FF 25 [rip+rel32]): follow them
    const void* FollowJumps(const void* fn)
    {
        for (int hop = 0; hop < 4 && fn; ++hop)
        {
            const unsigned char* b = static_cast<const unsigned char*>(fn);
            if (b[0] == 0xE9) fn = b + 5 + *reinterpret_cast<const int32_t*>(b + 1);
            else if (b[0] == 0xFF && b[1] == 0x25) fn = *reinterpret_cast<void* const*>(b + 6 + *reinterpret_cast<const int32_t*>(b + 2));
            else if (b[0] == 0x48 && b[1] == 0xFF && b[2] == 0x25) fn = *reinterpret_cast<void* const*>(b + 7 + *reinterpret_cast<const int32_t*>(b + 3));
            else break;
        }
        return fn;
    }
    bool CheckInvalidateSlot(const std::vector<void*>& all)
    {
        if (g_invalidateOk >= 0) return g_invalidateOk == 1;
        if (!qt.boxInvalidate) return false;
        for (void* w : all)
        {
            void* lay = qt.widgetLayout(w);
            if (!lay || strcmp(ClassName(lay), "QVBoxLayout") != 0) continue;
            void* slot = (*reinterpret_cast<void***>(static_cast<char*>(lay) + kLayoutItemOffset))[kInvalidateSlot];
            g_invalidateOk = FollowJumps(slot) == qt.boxInvalidate ? 1 : 0;
            if (!g_invalidateOk) rp::Log("theme: QLayoutItem vtable slot %zu is not QBoxLayout::invalidate here; relayout after theme changes is off", kInvalidateSlot);
            return g_invalidateOk == 1;
        }
        return false;
    }
    // phase 0: invalidate the widget's layout, 1: activate it again, -1 = faulted, 0 = no layout, 1 = done
    int RelayoutOne(void* w, int phase)
    {
        __try
        {
            void* lay = qt.widgetLayout(w);
            if (!lay) return 0;
            if (phase == 0) InvalidateVirtual(lay); else qt.layoutActivate(lay);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }
    std::string Relayout(bool visibleOnly)
    {
        if (!qt.layoutInvalidate || !qt.layoutActivate) return "relayout: Qt exports missing";
        std::vector<void*> all = AllWidgets(), used;
        if (!CheckInvalidateSlot(all)) return "relayout: off (the layout vtable is not as expected)";
        // visibleOnly (a test): what is on screen, else the hidden widgets of docked panes, the only ones Qt leaves stale
        // (visible ones are redone by Qt, hidden dialogs and closed panes when they open)
        for (void* w : all)
            if (visibleOnly ? qt.isVisibleTo(w, nullptr) : !qt.isVisibleTo(w, nullptr) && qt.widgetLayout(w) && DockOf(w)) used.push_back(w);
        uint64_t t0 = Now();
        int done = 0, faults[2] = {};
        for (int phase = 0; phase < 2; ++phase)
            for (void* w : used) { int r = RelayoutOne(w, phase); if (r < 0) ++faults[phase]; else if (phase == 1) done += r; }
        char ms[32]; snprintf(ms, sizeof ms, " in %.0f ms", Ms(Now() - t0));
        return "relaid " + std::to_string(done) + " layouts of " + std::to_string(used.size()) + " widgets (faults: invalidate " +
               std::to_string(faults[0]) + ", activate " + std::to_string(faults[1]) + ")" + ms;
    }
    // visible buttons that overlap a visible sibling button by more than a third of the smaller one
    std::string Docks()
    {
        std::string out;
        for (void* w : AllWidgets()) if (IsA(w, "TADockWidget")) out += std::string(qt.isVisibleTo(w, nullptr) ? "shown  " : "hidden ") + Describe(w) + "\n";
        return out;
    }
    std::string RaiseDock(const std::string& name)
    {
        auto toggle = reinterpret_cast<void (__fastcall*)(void*, bool)>(rp::host->RadiantAddress(rp::self, kDockToggleView));
        if (!toggle) return "toggleView not found";
        for (void* w : AllWidgets())
            if (IsA(w, "TADockWidget") && Lower(TakeString(qt.objectName, w)) == Lower(name)) { toggle(w, true); return "raised " + Describe(w); }
        return "no pane '" + name + "'";
    }
    // docks = every docked pane, shown or behind another tab: a button counts when it is visible within its pane
    std::string Overlaps(const std::string& png, bool docks = false)
    {
        std::vector<void*> all = AllWidgets();
        std::vector<void*> parents;
        int pairs = 0;
        std::string out;
        for (void* p : all)
        {
            std::vector<void*> buttons;
            for (void* c : Children(p))
                if (c && IsA(c, "QAbstractButton"))
                {
                    void* dock = docks ? DockOf(c) : nullptr;
                    if (docks ? dock && qt.isVisibleTo(c, dock) : qt.isVisibleTo(c, nullptr)) buttons.push_back(c);   // on screen / in its pane
                }
            int here = 0;
            for (size_t i = 0; i < buttons.size(); ++i)
                for (size_t j = i + 1; j < buttons.size(); ++j)
                {
                    const int* a = qt.geometry(buttons[i]); const int* b = qt.geometry(buttons[j]);
                    int w = std::min(a[2], b[2]) - std::max(a[0], b[0]) + 1, h = std::min(a[3], b[3]) - std::max(a[1], b[1]) + 1;
                    int minArea = std::min((a[2] - a[0] + 1) * (a[3] - a[1] + 1), (b[2] - b[0] + 1) * (b[3] - b[1] + 1));
                    if (w > 0 && h > 0 && minArea > 0 && w * h * 3 > minArea) ++here;
                }
            if (!here) continue;
            pairs += here;
            parents.push_back(p);
            if (parents.size() <= 8)
            {
                void* dock = DockOf(p);
                out += "  " + std::to_string(here) + " in " + Describe(p) + (dock ? " (pane " + Describe(dock) + (qt.isVisibleTo(dock, nullptr) ? ", shown)" : ", hidden)") : "") + "\n";
            }
        }
        out = std::to_string(pairs) + " overlapping button pairs in " + std::to_string(parents.size()) + " widgets\n" + out;
        if (!png.empty() && !parents.empty() && qt.grab && qt.pixmapSave && qt.pixmapDtor)
        {
            void* target = qt.parentWidget(parents[0]) ? qt.parentWidget(parents[0]) : parents[0];
            int rect[4] = { 0, 0, -2, -2 };
            char pixmap[64] = {};
            qt.grab(target, pixmap, rect);
            void* path = nullptr; qt.qstringFromUtf8(&path, png.c_str(), int(png.size()));
            bool saved = qt.pixmapSave(pixmap, &path, "PNG", -1);
            qt.qsDtor(&path); qt.pixmapDtor(pixmap);
            out += saved ? "rendered " + Describe(target) + " to " + png + "\n" : "render failed\n";
        }
        return out;
    }

    std::string List()
    {
        std::string s;
        for (const Entry& e : g_list)
        {
            s += e.name;
            if (Lower(e.name) == Lower(g_applied.empty() ? std::string(themeicons::kClassicTreyarch) : g_applied)) s += " (applied)";
            if (!e.file.empty()) s += "  " + Narrow(e.file);
            s += "\n";
        }
        return s.empty() ? "no themes scanned yet" : s;
    }
    std::string Status()
    {
        char buf[1024];
        snprintf(buf, sizeof buf, "theme: ini='%s' applied='%s' themes=%zu preferences hook=%s (dialogs patched %d, row at %s%s%s) last apply %.0f ms%s%s\nfolder: %s",
                 g_current.c_str(), g_applied.empty() ? themeicons::kClassicTreyarch : g_applied.c_str(), g_list.size(), g_hooked ? "on" : "off", g_patched,
                 g_where.empty() ? "-" : g_where.c_str(), g_patchError.empty() ? "" : ", last problem: ", g_patchError.c_str(), g_lastMs,
                 g_lastError.empty() ? "" : " last error: ", g_lastError.c_str(), Narrow(Dir()).c_str());
        return buf;
    }
}

void Setup()
{
    g_current = rp::ConfigGet("theme", "name", themeicons::kUnset);
    if (g_current == themeicons::kUnset)   // first run with the plugin: Radiant always shows its own Treyarch sheet
    {
        g_current = themeicons::FirstRunTheme(true);
        rp::ConfigSet("theme", "name", g_current);
        rp::Log("theme: first run, '%s' picked", g_current.c_str());
    }
    g_current = themeicons::Renamed(g_current);   // a retired shipped theme -> its successor
    if (!BindQt()) return;
    SetupColors();
    themeicons::Setup(Root());
    void* target = rp::host->RadiantAddress(rp::self, kPrefsCtor);
    DWORD64 base = 0; PRUNTIME_FUNCTION fn = target ? RtlLookupFunctionEntry(DWORD64(target), &base, nullptr) : nullptr;
    if (!fn || DWORD64(fn->BeginAddress) + base != DWORD64(target)) { rp::Log("theme: Preferences dialog not found, the theme row is off (command 'theme <name>' still works)"); return; }
    if (!rp::host->Detour(rp::self, target, reinterpret_cast<void*>(&hk_PrefsCtor), reinterpret_cast<void**>(&g_origCtor))) { rp::Log("theme: detour of the Preferences dialog failed"); return; }
    g_ctorLive = target;
    g_hooked = true;
    rp::Log("theme: Preferences theme row installed (ini theme '%s')", g_current.c_str());
}

// first tick with a main window, shown or still hidden under a command-line map load (InitInstance sets Radiant's own
// sheet before the window exists, so the first paint is themed), later ticks apply a choice made in Preferences
void Tick()
{
    if (!qt.bound) return;
    LiveColors();
    // Qt does not redo layouts of hidden panes after a style or sheet change (buttons overlap), so every layout is redone once, hidden too.
    if (g_relayoutTicks > 0 && --g_relayoutTicks == 0)
    {
        std::string r = Relayout(false);
        rp::Log("theme: %s", r.c_str());
    }
    if (!g_pending.empty())
    {
        std::string name = g_pending, why; g_pending.clear();
        if (name == kEditedName) return;            // OK with "Edited (not saved)": the edit stays
        // the theme in effect again: nothing to do, unless an edit is on (then this reverts it) or it was re-saved
        if (Lower(name) == Lower(g_applied.empty() ? std::string(themeicons::kClassicTreyarch) : g_applied) && !g_reapply && !themeedit::Edited()) return;
        g_reapply = false;
        if (Apply(name, why)) { g_current = g_applied; rp::ConfigSet("theme", "name", g_current); }
        else { g_lastError = why; rp::Log("theme: %s", why.c_str()); }
        return;
    }
    if (g_started) { WarmPacks(); return; }
    if (!rp::host->MainWindow(rp::self)) return;
    g_started = true;
    Scan();
    std::string l = Lower(g_current);
    if (l.empty() || l == "default" || themeicons::IsClassicTreyarch(l)) { g_applied = themeicons::kClassicTreyarch; themeicons::Use(themeicons::PackFor(Dir(), g_applied, "", L"")); themeicons::UseTitleBar("", g_applied); return; }
    std::string why;
    if (!Apply(g_current, why)) { g_lastError = why; rp::Log("theme: %s", why.c_str()); g_applied = themeicons::kClassicTreyarch; }
}

void Shutdown()
{
    themeicons::Shutdown();
}

bool PrefsOpen() { return PrefsWindowOpen(); }

bool Command(const std::string& c, std::string& reply)
{
    if (c == "colors" || c.rfind("colors ", 0) == 0)
    {
        std::string arg = c.size() > 7 ? c.substr(7) : "";
        if (arg.empty() || arg == "status") { reply = ColorsStatus(); return true; }
        if (!qt.bound || !g_colorsOn) { reply = "colors: off (" + std::string(qt.bound ? "color table not found" : "Qt is not bound") + ")"; return true; }
        if (arg == "list") { reply = ColorsList(); return true; }
        if (arg == "tabs") { reply = ColorsTabs(); return true; }
        if (arg == "shared 0" || arg == "shared 1")              // as the "Shared colors" box (no dialog needed)
        {
            g_shared = arg.back() == '1';
            rp::ConfigSet("colors", "shared", g_shared ? "1" : "0");
            reply = std::string("colors: shared=") + (g_shared ? "1" : "0");
            return true;
        }
        if (arg.rfind("dialog ", 0) == 0) { reply = ColorsDialog(arg.substr(7)); return true; }
        if (arg == "selftest" || arg.rfind("selftest ", 0) == 0) { ScanPresets(); reply = ColorsSelfTest(arg.size() > 9 ? arg.substr(9) : ""); return true; }
        if (arg.rfind("save ", 0) == 0)
        {
            std::string name = SafeFileName(arg.substr(5));
            std::wstring path = Dir() + L"\\" + Widen(name) + L".colors";
            CreateDirectoryW(Dir().c_str(), nullptr);
            reply = !name.empty() && WritePreset(path, g_table, name + ": saved from Radiant's current colors") ? "saved " + Narrow(path) : "colors: cannot save '" + name + "'";
            return true;
        }
        ScanPresets();
        const Preset* p = FindPreset(arg);
        if (!p) { reply = "colors: no preset '" + arg + "' (see 'colors list')"; return true; }
        ApplyPresetNow(*p, false);
        reply = "preset '" + p->name + "' applied and saved to Radiant's registry colors";
        return true;
    }
    if (c != "theme" && c.rfind("theme ", 0) != 0) return false;
    std::string arg = c.size() > 6 ? c.substr(6) : "";
    if (arg.empty() || arg == "status") { reply = Status(); return true; }
    if (!qt.bound) { reply = "theme: Qt is not bound"; return true; }
    if (arg == "list" || arg == "rescan") { Scan(); reply = List(); return true; }
    if (arg == "docks") { reply = Docks(); return true; }
    if (arg.rfind("grab ", 0) == 0 && qt.grab)                  // the main window rendered to a PNG (tests)
    {
        void* main = nullptr;
        for (void* w : AllWidgets()) if (!qt.parentWidget(w) && IsA(w, "QMainWindow") && qt.isVisibleTo(w, nullptr)) { main = w; break; }
        if (!main) { reply = "no main window"; return true; }
        std::string png = arg.substr(5);
        int rect[4] = { 0, 0, -2, -2 }; char pm[64] = {};
        qt.grab(main, pm, rect);
        void* path = nullptr; qt.qstringFromUtf8(&path, png.c_str(), int(png.size()));
        bool saved = qt.pixmapSave(pm, &path, "PNG", -1);
        qt.qsDtor(&path); qt.pixmapDtor(pm);
        reply = saved ? "main window rendered to " + png : "render failed";
        return true;
    }
    if (arg.rfind("count ", 0) == 0)
    {
        int n = 0, top = 0;
        for (void* w : AllWidgets()) if (IsA(w, arg.substr(6).c_str())) { ++n; if (!qt.parentWidget(w)) ++top; }
        reply = std::to_string(n) + " " + arg.substr(6) + " (" + std::to_string(top) + " without a parent)";
        return true;
    }
    if (arg.rfind("tree ", 0) == 0)
    {
        std::string want = Lower(arg.substr(5));
        for (void* w : AllWidgets()) if (Lower(TakeString(qt.objectName, w)) == want) { std::string t; Dump(w, 0, t); reply = t; return true; }
        reply = "no widget '" + arg.substr(5) + "'";
        return true;
    }
    if (arg.rfind("raise ", 0) == 0) { reply = RaiseDock(arg.substr(6)); return true; }
    if (arg.rfind("hide ", 0) == 0)
    {
        auto toggle = reinterpret_cast<void (__fastcall*)(void*, bool)>(rp::host->RadiantAddress(rp::self, kDockToggleView));
        std::string want = Lower(arg.substr(5));
        reply = "no pane";
        for (void* w : AllWidgets()) if (toggle && IsA(w, "TADockWidget") && Lower(TakeString(qt.objectName, w)) == want) { toggle(w, false); reply = "hid " + Describe(w); }
        return true;
    }
    if (arg == "overlaps docks" || arg.rfind("overlaps docks ", 0) == 0) { reply = Overlaps(arg.size() > 15 ? arg.substr(15) : "", true); return true; }
    if (arg == "overlaps" || arg.rfind("overlaps ", 0) == 0) { reply = Overlaps(arg.size() > 9 ? arg.substr(9) : ""); return true; }
    if (arg == "fast 0" || arg == "fast 1") { g_fastRestyle = arg.back() == '1'; reply = std::string("theme: fast restyle ") + (g_fastRestyle ? "on" : "off"); return true; }
    if (arg == "relayout" || arg == "relayout all") { reply = Relayout(arg == "relayout"); return true; }
    if (arg == "folder") { OnOpenFolder(nullptr); reply = "opened " + Narrow(Dir()); return true; }
    if (arg == "icons" || arg.rfind("icons ", 0) == 0) { reply = themeicons::Command(arg.size() > 6 ? arg.substr(6) : "", Dir()); return true; }
    if (arg.rfind("prefstree ", 0) == 0) { reply = PrefsTree(arg.substr(10)); return true; }
    if (arg == "selftest" || arg.rfind("selftest ", 0) == 0) { reply = SelfTest(arg.size() > 9 ? arg.substr(9) : ""); return true; }
    Scan();
    std::string why;
    if (Apply(arg, why))
    {
        g_current = g_applied; rp::ConfigSet("theme", "name", g_current); reply = "theme '" + g_applied + "' applied and saved";
        if (g_colorsOn && g_follow)
        {
            ScanPresets();
            if (const Preset* p = PresetForTheme(g_applied)) { ApplyPresetNow(*p, true); reply += ", view colors '" + p->name + "'"; }
        }
    }
    else reply = "theme: " + why;
    return true;
}
}
