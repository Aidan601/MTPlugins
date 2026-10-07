// Theme icon packs, compiled into Radiant Plus and APE Plus. Qt 5.3.2 asks every QAbstractFileEngineHandler first for any file or resource path it opens, so one handler sends the paths a pack replaces to the pack's files (no exe is patched).
// Use() refreshes icons built before a switch: it drops the pixmaps of redirected files inside Qt's private QPixmapIconEngine (layout learned from a probe icon, the refresh is off when it does not match).
#include "modtools_plugin.hpp"
#include "themeicons.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace themeicons
{
namespace
{
    // ---- Qt 5.3.2 exports, no headers (MS x64: a class returned by value goes through a hidden pointer) ----
    struct QArrayDataRaw { int ref; int size; unsigned alloc; ptrdiff_t offset; };
    struct QListDataD { int ref; int alloc; int begin; int end; void* array[1]; };
    struct QModelIndexRaw { int r; int c; uintptr_t i; const void* m; };
    struct QMetaObjectRaw;
    struct Qt
    {
        bool bound = false;
        // core
        void* (*qsFromWChar)(void*, const wchar_t*, int) = nullptr;
        void (*qsDtor)(void*) = nullptr;
        void* (*qsAssign)(void*, const void*) = nullptr;
        void* (*engineCreate)(const void*) = nullptr;                   // static QAbstractFileEngine::create(const QString&)
        void* (*handlerCtor)(void*) = nullptr;
        void (*handlerDtor)(void*) = nullptr;
        void** handlerVtbl = nullptr;
        void (*dispose)(void*) = nullptr;                               // QListData::dispose
        const char* (*className)(const QMetaObjectRaw*) = nullptr;
        const QMetaObjectRaw* (*superClass)(const QMetaObjectRaw*) = nullptr;
        void (*variantDtor)(void*) = nullptr;
        int (*variantUserType)(const void*) = nullptr;
        const void* (*variantConstData)(const void*) = nullptr;
        void** modelVtbl = nullptr;                                     // QAbstractItemModel's own vtable (layout check)
        void* modelSibling = nullptr;
        void* modelHasChildren = nullptr;
        void* (*dirIterCtor)(void*, const void*, int) = nullptr;
        bool (*dirIterHasNext)(const void*) = nullptr;
        void* (*dirIterNext)(void*, void*) = nullptr;
        void (*dirIterDtor)(void*) = nullptr;
        bool (*fileCopy)(const void*, const void*) = nullptr;
        // gui
        void* (*iconCtor)(void*) = nullptr;
        void (*iconDtor)(void*) = nullptr;
        void (*iconAddFile)(void*, const void*, const void*, int, int) = nullptr;
        void* (*pixmapCtor)(void*) = nullptr;
        void (*pixmapDtor)(void*) = nullptr;
        void* (*pixmapAssign)(void*, const void*) = nullptr;
        void* (*imageFromFile)(void*, const void*, const char*) = nullptr;
        void (*imageDtor)(void*) = nullptr;
        void* (*imageConvert)(const void*, void*, int, int) = nullptr;   // convertToFormat(Format, Qt::ImageConversionFlags)
        unsigned char* (*imageBits)(void*) = nullptr;
        int (*imageWidth)(const void*) = nullptr;
        int (*imageHeight)(const void*) = nullptr;
        int (*imageBytesPerLine)(const void*) = nullptr;
        bool (*imageIsNull)(const void*) = nullptr;
        bool (*imageSave)(const void*, const void*, const char*, int) = nullptr;
        // widgets
        void* (*allWidgets)(void*) = nullptr;
        void* (*widgetActions)(const void*, void*) = nullptr;
        void* (*windowIcon)(const void*, void*) = nullptr;
        void (*widgetUpdate)(void*) = nullptr;
        void* (*actionIcon)(const void*, void*) = nullptr;
        void* (*buttonIcon)(const void*, void*) = nullptr;
        int (*tabCount)(const void*) = nullptr;
        void* (*tabIcon)(const void*, void*, int) = nullptr;
        void* (*viewModel)(const void*) = nullptr;
        void* (*viewRootIndex)(const void*, void*) = nullptr;
        bool (*treeIsExpanded)(const void*, const void*) = nullptr;
        // optional, for a theme's logo on the title and task bars
        void (*setWindowIcon)(void*, const void*) = nullptr;
        void* (*widgetWindow)(const void*) = nullptr;
        void (*appSetWindowIcon)(const void*) = nullptr;                // static QApplication::setWindowIcon
        void* (*appWindowIcon)(void*) = nullptr;                        // static QApplication::windowIcon
        void* (*iconFromFile)(void*, const void*) = nullptr;            // QIcon(const QString&)
    } qt;

    template <class T> bool Get(T& fn, HMODULE m, const char* name) { fn = reinterpret_cast<T>(GetProcAddress(m, name)); return fn != nullptr; }

    bool BindQt()
    {
        if (qt.bound) return true;
        HMODULE c = GetModuleHandleW(L"Qt5Core64r.dll"), g = GetModuleHandleW(L"Qt5Gui64r.dll"), w = GetModuleHandleW(L"Qt5Widgets64r.dll");
        if (!c || !g || !w) return false;
        bool ok = true;
        ok &= Get(qt.qsFromWChar, c, "?fromWCharArray@QString@@SA?AV1@PEB_WH@Z");
        ok &= Get(qt.qsDtor, c, "??1QString@@QEAA@XZ");
        ok &= Get(qt.qsAssign, c, "??4QString@@QEAAAEAV0@AEBV0@@Z");
        ok &= Get(qt.engineCreate, c, "?create@QAbstractFileEngine@@SAPEAV1@AEBVQString@@@Z");
        ok &= Get(qt.handlerCtor, c, "??0QAbstractFileEngineHandler@@QEAA@XZ");
        ok &= Get(qt.handlerDtor, c, "??1QAbstractFileEngineHandler@@UEAA@XZ");
        ok &= Get(qt.handlerVtbl, c, "??_7QAbstractFileEngineHandler@@6B@");
        ok &= Get(qt.dispose, c, "?dispose@QListData@@SAXPEAUData@1@@Z");
        ok &= Get(qt.className, c, "?className@QMetaObject@@QEBAPEBDXZ");
        ok &= Get(qt.superClass, c, "?superClass@QMetaObject@@QEBAPEBU1@XZ");
        ok &= Get(qt.variantDtor, c, "??1QVariant@@QEAA@XZ");
        ok &= Get(qt.variantUserType, c, "?userType@QVariant@@QEBAHXZ");
        ok &= Get(qt.variantConstData, c, "?constData@QVariant@@QEBAPEBXXZ");
        ok &= Get(qt.modelVtbl, c, "??_7QAbstractItemModel@@6B@");
        ok &= Get(qt.modelSibling, c, "?sibling@QAbstractItemModel@@UEBA?AVQModelIndex@@HHAEBV2@@Z");
        ok &= Get(qt.modelHasChildren, c, "?hasChildren@QAbstractItemModel@@UEBA_NAEBVQModelIndex@@@Z");
        ok &= Get(qt.dirIterCtor, c, "??0QDirIterator@@QEAA@AEBVQString@@V?$QFlags@W4IteratorFlag@QDirIterator@@@@@Z");
        ok &= Get(qt.dirIterHasNext, c, "?hasNext@QDirIterator@@QEBA_NXZ");
        ok &= Get(qt.dirIterNext, c, "?next@QDirIterator@@QEAA?AVQString@@XZ");
        if (!Get(qt.dirIterDtor, c, "??1QDirIterator@@UEAA@XZ")) ok &= Get(qt.dirIterDtor, c, "??1QDirIterator@@QEAA@XZ");
        ok &= Get(qt.fileCopy, c, "?copy@QFile@@SA_NAEBVQString@@0@Z");
        ok &= Get(qt.iconCtor, g, "??0QIcon@@QEAA@XZ");
        ok &= Get(qt.iconDtor, g, "??1QIcon@@QEAA@XZ");
        ok &= Get(qt.iconAddFile, g, "?addFile@QIcon@@QEAAXAEBVQString@@AEBVQSize@@W4Mode@1@W4State@1@@Z");
        ok &= Get(qt.pixmapCtor, g, "??0QPixmap@@QEAA@XZ");
        ok &= Get(qt.pixmapDtor, g, "??1QPixmap@@UEAA@XZ");
        ok &= Get(qt.pixmapAssign, g, "??4QPixmap@@QEAAAEAV0@AEBV0@@Z");
        ok &= Get(qt.imageFromFile, g, "??0QImage@@QEAA@AEBVQString@@PEBD@Z");
        ok &= Get(qt.imageDtor, g, "??1QImage@@UEAA@XZ");
        ok &= Get(qt.imageConvert, g, "?convertToFormat@QImage@@QEBA?AV1@W4Format@1@V?$QFlags@W4ImageConversionFlag@Qt@@@@@Z");
        ok &= Get(qt.imageBits, g, "?bits@QImage@@QEAAPEAEXZ");
        ok &= Get(qt.imageWidth, g, "?width@QImage@@QEBAHXZ");
        ok &= Get(qt.imageHeight, g, "?height@QImage@@QEBAHXZ");
        ok &= Get(qt.imageBytesPerLine, g, "?bytesPerLine@QImage@@QEBAHXZ");
        ok &= Get(qt.imageIsNull, g, "?isNull@QImage@@QEBA_NXZ");
        ok &= Get(qt.imageSave, g, "?save@QImage@@QEBA_NAEBVQString@@PEBDH@Z");
        ok &= Get(qt.allWidgets, w, "?allWidgets@QApplication@@SA?AV?$QList@PEAVQWidget@@@@XZ");
        ok &= Get(qt.widgetActions, w, "?actions@QWidget@@QEBA?AV?$QList@PEAVQAction@@@@XZ");
        ok &= Get(qt.windowIcon, w, "?windowIcon@QWidget@@QEBA?AVQIcon@@XZ");
        ok &= Get(qt.widgetUpdate, w, "?update@QWidget@@QEAAXXZ");
        ok &= Get(qt.actionIcon, w, "?icon@QAction@@QEBA?AVQIcon@@XZ");
        ok &= Get(qt.buttonIcon, w, "?icon@QAbstractButton@@QEBA?AVQIcon@@XZ");
        ok &= Get(qt.tabCount, w, "?count@QTabBar@@QEBAHXZ");
        ok &= Get(qt.tabIcon, w, "?tabIcon@QTabBar@@QEBA?AVQIcon@@H@Z");
        ok &= Get(qt.viewModel, w, "?model@QAbstractItemView@@QEBAPEAVQAbstractItemModel@@XZ");
        ok &= Get(qt.viewRootIndex, w, "?rootIndex@QAbstractItemView@@QEBA?AVQModelIndex@@XZ");
        ok &= Get(qt.treeIsExpanded, w, "?isExpanded@QTreeView@@QEBA_NAEBVQModelIndex@@@Z");
        Get(qt.setWindowIcon, w, "?setWindowIcon@QWidget@@QEAAXAEBVQIcon@@@Z");
        Get(qt.widgetWindow, w, "?window@QWidget@@QEBAPEAV1@XZ");
        Get(qt.appSetWindowIcon, w, "?setWindowIcon@QApplication@@SAXAEBVQIcon@@@Z");
        Get(qt.appWindowIcon, w, "?windowIcon@QApplication@@SA?AVQIcon@@XZ");
        Get(qt.iconFromFile, g, "??0QIcon@@QEAA@AEBVQString@@@Z");
        if (!ok) rp::Log("icons: some Qt exports are missing, theme icon packs are off");
        qt.bound = ok;
        return ok;
    }

    struct QStr   // a QString built from UTF-16, released with Qt
    {
        void* d = nullptr;
        explicit QStr(const std::wstring& s) { qt.qsFromWChar(&d, s.c_str(), int(s.size())); }
        ~QStr() { qt.qsDtor(&d); }
        QStr(const QStr&) = delete; QStr& operator=(const QStr&) = delete;
    };
    const wchar_t* QsData(const void* qs, int& n)
    {
        const QArrayDataRaw* d = *static_cast<const QArrayDataRaw* const*>(qs);
        n = d ? d->size : 0;
        return d && n > 0 ? reinterpret_cast<const wchar_t*>(reinterpret_cast<const char*>(d) + d->offset) : nullptr;
    }
    std::wstring QsToW(const void* qs) { int n = 0; const wchar_t* w = QsData(qs, n); return w ? std::wstring(w, size_t(n)) : std::wstring(); }
    void ReleaseList(void* d)
    {
        if (!d) return;
        volatile long* ref = static_cast<volatile long*>(d);
        if (*ref == -1) return;                                         // the static shared_null
        if (*ref == 0 || InterlockedDecrement(ref) == 0) qt.dispose(d);
    }
    template <class F> void ForList(void* d, F f)
    {
        if (!d) return;
        QListDataD* l = static_cast<QListDataD*>(d);
        for (int i = l->begin; i < l->end; ++i) f(l->array[i]);
    }
    const QMetaObjectRaw* MetaOf(void* obj)
    {
        auto metaObject = (*reinterpret_cast<const QMetaObjectRaw* (__fastcall***)(void*)>(obj))[0];   // QObject vtable slot 0
        return metaObject(obj);
    }
    bool IsA(void* obj, const char* cls)
    {
        for (const QMetaObjectRaw* m = MetaOf(obj); m; m = qt.superClass(m))
            if (const char* c = qt.className(m)) if (strcmp(c, cls) == 0) return true;
        return false;
    }

    std::string Narrow(const std::wstring& w)
    {
        if (w.empty()) return "";
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(size_t(n), '\0'); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), &s[0], n, nullptr, nullptr); return s;
    }
    std::wstring Widen(const std::string& s)
    {
        if (s.empty()) return L"";
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
        std::wstring w(size_t(n), L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), &w[0], n); return w;
    }
    bool DirExists(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
    void MakeDirs(const std::wstring& dir)
    {
        if (dir.empty() || DirExists(dir)) return;
        size_t cut = dir.find_last_of(L"\\/");
        if (cut != std::wstring::npos && cut > 2) MakeDirs(dir.substr(0, cut));
        CreateDirectoryW(dir.c_str(), nullptr);
    }

    bool IsImage(const wchar_t* w, int n)
    {
        static const wchar_t* const kExt[] = { L".png", L".jpg", L".jpeg", L".bmp", L".ico", L".gif", L".svg" };
        for (const wchar_t* e : kExt)
        {
            int k = int(wcslen(e));
            if (n <= k) continue;
            int j = 0;
            while (j < k && towlower(w[n - k + j]) == e[j]) ++j;
            if (j == k) return true;
        }
        return false;
    }
    // the lookup key of a path: lower case, '/' separators, no doubled separators
    std::wstring Key(const wchar_t* w, int n)
    {
        std::wstring k; k.reserve(size_t(n));
        for (int i = 0; i < n; ++i)
        {
            wchar_t ch = w[i] == L'\\' ? L'/' : w[i];
            if (ch >= L'A' && ch <= L'Z') ch = wchar_t(ch - L'A' + L'a');
            if (ch == L'/' && !k.empty() && k.back() == L'/' && k.size() > 2) continue;
            k += ch;
        }
        return k;
    }
    std::wstring Key(const std::wstring& s) { return Key(s.c_str(), int(s.size())); }

    // ---- the pack and the handler ----
    struct Pack
    {
        std::wstring dir;
        std::unordered_map<std::wstring, std::wstring> files;   // key of the editor's path -> the pack's file
    };
    SRWLOCK g_lock = SRWLOCK_INIT;
    Pack* g_pack = nullptr;                    // under g_lock, replaced on the main thread only
    std::atomic<bool> g_live{ false };         // handler registered, a pack in use, not exporting, not exiting
    bool g_registered = false;
    bool g_exporting = false;
    std::wstring g_root;
    // every pack file handed out this session -> the editor's own path (QFileInfo::absoluteFilePath of a redirected disk
    // path answers with the pack's file, and QPixmapIconEngine keeps that name), main thread only
    std::unordered_map<std::wstring, std::wstring>* g_reverse = new std::unordered_map<std::wstring, std::wstring>();

    SRWLOCK g_traceLock = SRWLOCK_INIT;
    bool g_trace = false;
    std::unordered_set<std::wstring>* g_traceSeen = new std::unordered_set<std::wstring>();   // leaked on purpose (exit)
    std::vector<std::string>* g_traceLines = new std::vector<std::string>();

    void UpdateLive() { g_live.store(g_registered && g_pack && !g_exporting, std::memory_order_release); }

    // QAbstractFileEngineHandler::create(const QString&) const, Qt holds its recursive handler lock around it.
    void* __fastcall H_Create(const void*, const void* name) noexcept
    {
        try
        {
            if (!g_live.load(std::memory_order_acquire) && !g_trace) return nullptr;
            int n = 0; const wchar_t* w = QsData(name, n);
            if (!w || n < 5 || n > 2048 || !IsImage(w, n)) return nullptr;
            std::wstring key = Key(w, n), hit;
            if (g_live.load(std::memory_order_acquire))
            {
                AcquireSRWLockShared(&g_lock);
                if (g_pack) { auto it = g_pack->files.find(key); if (it != g_pack->files.end()) hit = it->second; }
                ReleaseSRWLockShared(&g_lock);
            }
            if (g_trace)
            {
                AcquireSRWLockExclusive(&g_traceLock);
                if (g_traceSeen->size() < 5000 && g_traceSeen->insert(key).second)
                    g_traceLines->push_back(Narrow(std::wstring(w, size_t(n))) + (hit.empty() ? "" : "  -> " + Narrow(hit)));
                ReleaseSRWLockExclusive(&g_traceLock);
            }
            if (hit.empty()) return nullptr;
            QStr q(hit);
            return qt.engineCreate(&q.d);       // asks the handlers again with the pack's path: none answers, Qt's file engine
        }
        catch (...) { return nullptr; }
    }
    void* __fastcall H_Dtor(void* self, unsigned) noexcept { return self; }   // Qt never deletes a handler

    void* g_vt[3] = { nullptr, reinterpret_cast<void*>(&H_Dtor), reinterpret_cast<void*>(&H_Create) };   // [-1] = Qt's RTTI locator
    alignas(16) unsigned char g_handler[64];

    struct ExitGuard { ~ExitGuard() { g_registered = false; g_trace = false; g_live.store(false); } } g_exitGuard;   // CRT teardown at quit: answer nothing

    // the editors' logos (window and taskbar icon) stay the editor's own: never recoloured, never taken from a pack,
    // only a sheet's own 'logo:' line replaces them (Use)
    const wchar_t* const kLogos[] = { L":/icons/RadiantIcon.png", L":/icons/icon_experimental.png",
                                      L":/AEMainWindow/Resources/icon.png", L":/AEMainWindow/Resources/icon_experimental.png" };
    std::wstring g_logo;                       // the logo file in use, "" = the editor's own, main thread only
    bool IsLogo(const std::wstring& path)
    {
        for (const wchar_t* k : kLogos) if (_wcsicmp(path.c_str(), k) == 0) return true;
        return false;
    }
    Pack* LoadPack(const std::wstring& dir)
    {
        Pack* p = new Pack();
        p->dir = dir;
        std::vector<std::wstring> stack{ L"" };
        while (!stack.empty() && p->files.size() < 20000)
        {
            std::wstring rel = stack.back(); stack.pop_back();
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((dir + L"\\" + rel + L"*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do
            {
                std::wstring n = fd.cFileName;
                if (n == L"." || n == L"..") continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { stack.push_back(rel + n + L"\\"); continue; }
                if (!IsImage(n.c_str(), int(n.size()))) continue;
                std::wstring r = rel + n, file = dir + L"\\" + r, orig;
                if (_wcsnicmp(r.c_str(), L"AssetWorks\\", 11) == 0) orig = g_root + L"\\" + r;
                else { orig = L":/" + r; for (wchar_t& ch : orig) if (ch == L'\\') ch = L'/'; }
                if (IsLogo(orig)) continue;    // a pack's copy of a logo is left out
                p->files[Key(orig)] = file;
                (*g_reverse)[Key(file)] = orig;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        return p;
    }

    // ---- refreshing icons that already loaded their pixmap ----
    // Qt 5.3.2 private layout, checked by Probe(): QIcon = { QIconPrivate* d }, QIconPrivate = { QIconEngine* engine, ... },
    // QPixmapIconEngine = { vptr, QVector<entry> }, entry = { QPixmap pixmap, QString fileName, QSize size, QIcon::Mode mode, QIcon::State state }.
    struct Layout { bool ok = false; void* engineVtbl = nullptr; size_t offName = 0, stride = 0; std::string why; } g_layout;

    QArrayDataRaw* Entries(void* engine) { return *reinterpret_cast<QArrayDataRaw**>(static_cast<char*>(engine) + 8); }
    char* EntryBase(QArrayDataRaw* v) { return reinterpret_cast<char*>(v) + v->offset; }

    bool WriteProbe(const std::wstring& path)
    {
        static const unsigned char kPng[] = {   // a 1x1 transparent PNG
            0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41,
            0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49,
            0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82 };
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD put = 0; bool ok = WriteFile(h, kPng, DWORD(sizeof kPng), &put, nullptr) && put == sizeof kPng;
        CloseHandle(h);
        return ok;
    }

    // Builds an icon from two real files with known sizes, modes and states, then finds them in the engine's entries.
    void Probe()
    {
        g_layout = Layout();
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        std::wstring a = std::wstring(tmp) + L"mtplugins_iconprobe_a.png", b = std::wstring(tmp) + L"mtplugins_iconprobe_b.png";
        if (!WriteProbe(a) || !WriteProbe(b)) { g_layout.why = "cannot write the probe images"; return; }
        alignas(16) unsigned char icon[16] = {};
        qt.iconCtor(icon);
        {
            QStr qa(a), qb(b);
            int sa[2] = { 16, 16 }, sb[2] = { 32, 32 };
            qt.iconAddFile(icon, &qa.d, sa, 0, 1);   // Normal, Off
            qt.iconAddFile(icon, &qb.d, sb, 2, 0);   // Active, On
        }
        void* d = *reinterpret_cast<void**>(icon);
        void* engine = d ? *static_cast<void**>(d) : nullptr;
        QArrayDataRaw* v = engine ? Entries(engine) : nullptr;
        std::wstring ka = Key(a), kb = Key(b);
        if (!v || v->size != 2) g_layout.why = "the probe icon does not hold two entries";
        else
        {
            // expected: QPixmap (32 bytes) first, the name at +32, size at +40, mode +48, state +52, 56 bytes per entry
            char* base = EntryBase(v);
            const size_t kName = 32, kStride = 56;
            auto nameAt = [&](size_t off)
            {
                const QArrayDataRaw* s = *reinterpret_cast<QArrayDataRaw**>(base + off);
                return s && !IsBadReadPtr(s, sizeof *s) && s->size > 0 && s->size < 1024 ? Key(QsToW(base + off)) : std::wstring();
            };
            const int* ea = reinterpret_cast<const int*>(base + kName + 8);
            const int* eb = reinterpret_cast<const int*>(base + kStride + kName + 8);
            if (nameAt(kName) != ka || nameAt(kStride + kName) != kb) g_layout.why = "file names not where expected";
            else if (ea[0] != 16 || ea[1] != 16 || ea[2] != 0 || ea[3] != 1 || eb[0] != 32 || eb[1] != 32 || eb[2] != 2 || eb[3] != 0)
                g_layout.why = "size, mode or state not where expected";
            else { g_layout.ok = true; g_layout.engineVtbl = *static_cast<void**>(engine); g_layout.offName = kName; g_layout.stride = kStride; }
        }
        qt.iconDtor(icon);
        DeleteFileW(a.c_str()); DeleteFileW(b.c_str());
        // the item model calls go through QAbstractItemModel's vtable: index 11, rowCount 14, columnCount 15, data 17
        if (qt.modelVtbl[13] != qt.modelSibling || qt.modelVtbl[16] != qt.modelHasChildren) { g_layout.ok = false; g_layout.why = "QAbstractItemModel vtable differs"; }
        if (g_layout.ok) rp::Log("icons: icon refresh ready (QPixmapIconEngine entries %zu bytes, file name at +%zu)", g_layout.stride, g_layout.offName);
        else rp::Log("icons: %s; icons already on screen change after a restart", g_layout.why.c_str());
    }

    struct RefreshStats { int icons = 0, entries = 0, reset = 0, renamed = 0, indexes = 0; };

    // drops the pixmaps of the entries whose file the switch changed, the engine reloads them from the name when painted
    void ResetIcon(void* icon, const std::unordered_set<std::wstring>& changed, std::unordered_set<void*>& seen, RefreshStats& st)
    {
        void* d = *static_cast<void**>(icon);
        if (!d || !seen.insert(d).second) return;
        void* engine = *static_cast<void**>(d);
        if (!engine || *static_cast<void**>(engine) != g_layout.engineVtbl) return;
        QArrayDataRaw* v = Entries(engine);
        if (!v || v->size <= 0 || v->size > 64) return;
        ++st.icons;
        char* base = EntryBase(v);
        for (int i = 0; i < v->size; ++i)
        {
            char* e = base + size_t(i) * g_layout.stride;
            void* name = e + g_layout.offName;
            int n = 0; const wchar_t* w = QsData(name, n);
            if (!w) continue;
            ++st.entries;
            std::wstring key = Key(w, n);
            if (w[0] != L':')
            {
                auto r = g_reverse->find(key);
                if (r != g_reverse->end()) { QStr orig(r->second); qt.qsAssign(name, &orig.d); key = Key(r->second); ++st.renamed; }
            }
            if (!changed.count(key)) continue;
            alignas(16) unsigned char empty[64] = {};
            qt.pixmapCtor(empty);
            qt.pixmapAssign(e, empty);
            qt.pixmapDtor(empty);
            ++st.reset;
        }
    }
    template <class Getter> void ResetVia(Getter get, const std::unordered_set<std::wstring>& changed, std::unordered_set<void*>& seen, RefreshStats& st)
    {
        alignas(16) unsigned char icon[16] = {};
        get(icon);
        ResetIcon(icon, changed, seen, st);
        qt.iconDtor(icon);
    }

    // the decoration icons of an item view: top level rows, and the children of expanded rows in a tree
    void WalkModel(void* view, const std::unordered_set<std::wstring>& changed, std::unordered_set<void*>& seen, RefreshStats& st)
    {
        void* model = qt.viewModel(view);
        if (!model) return;
        void** vt = *static_cast<void***>(model);
        auto index = reinterpret_cast<void* (*)(const void*, void*, int, int, const void*)>(vt[11]);
        auto rowCount = reinterpret_cast<int (*)(const void*, const void*)>(vt[14]);
        auto columnCount = reinterpret_cast<int (*)(const void*, const void*)>(vt[15]);
        auto data = reinterpret_cast<void* (*)(const void*, void*, const void*, int)>(vt[17]);
        bool tree = IsA(view, "QTreeView");
        QModelIndexRaw root{ -1, -1, 0, nullptr };
        qt.viewRootIndex(view, &root);
        std::vector<QModelIndexRaw> todo{ root };
        const int stop = st.indexes + 5000;     // per view: one long flat list must not use up the rest
        while (!todo.empty() && st.indexes < stop)
        {
            QModelIndexRaw parent = todo.back(); todo.pop_back();
            int rows = rowCount(model, &parent), cols = columnCount(model, &parent);
            if (cols > 4) cols = 4;
            for (int r = 0; r < rows && st.indexes < stop; ++r)
                for (int c = 0; c < cols; ++c)
                {
                    QModelIndexRaw idx{ -1, -1, 0, nullptr };
                    index(model, &idx, r, c, &parent);
                    if (idx.r < 0 || !idx.m) continue;
                    ++st.indexes;
                    alignas(16) unsigned char var[32] = {};
                    data(model, var, &idx, 1);                                    // Qt::DecorationRole
                    if (qt.variantUserType(var) == 69)                             // QMetaType::QIcon
                        if (const void* ic = qt.variantConstData(var)) ResetIcon(const_cast<void*>(ic), changed, seen, st);
                    qt.variantDtor(var);
                    if (c == 0 && tree && qt.treeIsExpanded(view, &idx)) todo.push_back(idx);
                }
        }
    }

    RefreshStats Refresh(const std::unordered_set<std::wstring>& changed)
    {
        RefreshStats st;
        void* list = nullptr; qt.allWidgets(&list);
        std::unordered_set<void*> seen;
        ForList(list, [&](void* w)
        {
            if (g_layout.ok && !changed.empty())
            {
                ResetVia([&](void* out) { qt.windowIcon(w, out); }, changed, seen, st);
                void* acts = nullptr; qt.widgetActions(w, &acts);
                ForList(acts, [&](void* a) { ResetVia([&](void* out) { qt.actionIcon(a, out); }, changed, seen, st); });
                ReleaseList(acts);
                if (IsA(w, "QAbstractButton")) ResetVia([&](void* out) { qt.buttonIcon(w, out); }, changed, seen, st);
                else if (IsA(w, "QTabBar")) { int n = qt.tabCount(w); for (int i = 0; i < n; ++i) ResetVia([&](void* out) { qt.tabIcon(w, out, i); }, changed, seen, st); }
                else if (IsA(w, "QAbstractItemView")) WalkModel(w, changed, seen, st);
            }
            qt.widgetUpdate(w);
        });
        ReleaseList(list);
        return st;
    }

    // the last switch, for status
    std::string g_lastSwitch = "none yet";

    std::string Status()
    {
        std::wstring dir; size_t n = 0;
        AcquireSRWLockShared(&g_lock);
        if (g_pack) { dir = g_pack->dir; n = g_pack->files.size(); }
        ReleaseSRWLockShared(&g_lock);
        char buf[1024];
        snprintf(buf, sizeof buf, "icons: redirect %s, pack %s (%zu files), refresh %s%s%s, trace %s\nlast switch: %s",
                 g_registered ? "on" : "off", dir.empty() ? "none (the editor's own icons)" : Narrow(dir).c_str(), n,
                 g_layout.ok ? "ready" : "off", g_layout.ok ? "" : ": ", g_layout.ok ? "" : g_layout.why.c_str(),
                 g_trace ? "on" : "off", g_lastSwitch.c_str());
        return buf;
    }

    // every image the editor has compiled in (":/...", Qt's own included) and <root>\AssetWorks\*.png, under their pack paths
    std::string Export(const std::wstring& out)
    {
        g_exporting = true; UpdateLive();       // the originals, not the pack's files
        int copied = 0, failed = 0;
        alignas(16) unsigned char it[64] = {};
        {
            QStr rootPath(L":/");
            qt.dirIterCtor(it, &rootPath.d, 0x2);   // QDirIterator::Subdirectories
        }
        while (qt.dirIterHasNext(it))
        {
            void* q = nullptr; qt.dirIterNext(it, &q);
            std::wstring src = QsToW(&q);
            qt.qsDtor(&q);
            if (src.size() < 3 || !IsImage(src.c_str(), int(src.size()))) continue;
            std::wstring rel = src.substr(2);
            for (wchar_t& ch : rel) if (ch == L'/') ch = L'\\';
            std::wstring dst = out + L"\\" + rel;
            MakeDirs(dst.substr(0, dst.find_last_of(L'\\')));
            SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(dst.c_str());
            QStr qs(src), qd(dst);
            if (qt.fileCopy(&qs.d, &qd.d)) { ++copied; SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL); }   // Qt copies the resource's read-only bits
            else ++failed;
        }
        qt.dirIterDtor(it);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((g_root + L"\\AssetWorks\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            MakeDirs(out + L"\\AssetWorks");
            do
            {
                std::wstring n = fd.cFileName;
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !IsImage(n.c_str(), int(n.size()))) continue;
                if (CopyFileW((g_root + L"\\AssetWorks\\" + n).c_str(), (out + L"\\AssetWorks\\" + n).c_str(), FALSE)) ++copied; else ++failed;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        g_exporting = false; UpdateLive();
        char buf[512];
        snprintf(buf, sizeof buf, "icons: %d images exported to %s%s", copied, Narrow(out).c_str(), failed ? (" (" + std::to_string(failed) + " failed)").c_str() : "");
        rp::Log("%s", buf);
        return buf;
    }

    // ---- recoloured packs: a light theme's dark icons, made on the user's PC from the editor's own images ----
    // An icon with white in it (over 3 % of its opaque pixels) or a dark button face is redrawn: grey pixels get their lightness turned over (white -> glyph colour,
    // black -> face colour), coloured pixels keep their colour a little darker. Icons drawn dark already stay. Nothing of the editors' art ships, the pack is written when first applied.
    struct Rgb { int r, g, b; };
    bool ParseHex(const std::string& t, Rgb& c)
    {
        size_t i = t[0] == '#' ? 1 : 0;
        if (t.size() != i + 6) return false;
        unsigned v = 0;
        for (size_t k = i; k < t.size(); ++k) { if (!isxdigit(static_cast<unsigned char>(t[k]))) return false; v = v * 16 + unsigned(isdigit(static_cast<unsigned char>(t[k])) ? t[k] - '0' : (tolower(t[k]) - 'a' + 10)); }
        c = { int(v >> 16 & 255), int(v >> 8 & 255), int(v & 255) };
        return true;
    }
    // 0 = leave the icon alone, 1 = a light glyph (white -> glyph colour, black -> face colour), 2 = a dark glyph (black -> glyph colour, white -> face colour).
    // Radiant's art (white glyphs for a dark UI): mode 1 when over 3 % of opaque pixels are near-white or it is a dark button face, mode 2 for almost wholly dark grey icons.
    // APE's art and AssetWorks (colour pictograms for a light UI): only almost wholly grey icons (mean lightness) change, anything with real colour keeps its look.
    int RecolorMode(const uint32_t* px, int w, int h, int stride, bool pictograms)
    {
        int n = 0, white = 0, dark = 0, grey = 0;
        double light = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                uint32_t p = px[size_t(y) * stride + x];
                int a = p >> 24, r = p >> 16 & 255, g = p >> 8 & 255, b = p & 255;
                if (a < 128) continue;
                ++n;
                int mx = (std::max)({ r, g, b }), mn = (std::min)({ r, g, b });
                if (mx - mn < 30) { ++grey; light += (0.299 * r + 0.587 * g + 0.114 * b) / 255.0; }
                if (mn > 200 && mx - mn < 40) ++white;
                else if (mx < 110 && mx - mn < 30) ++dark;
            }
        if (!n) return 0;
        const bool mono = grey > 0.9 * n;
        const double meanLight = grey ? light / grey : 0.5;
        if (pictograms) return mono ? (meanLight > 0.5 ? 1 : 2) : 0;
        bool face = n > 0.6 * w * h && dark > 0.5 * n;   // a dark button face with a glyph on it
        if (white > 0.03 * n || face) return 1;
        return mono && meanLight < 0.45 ? 2 : 0;
    }
    void Recolor(uint32_t* px, int w, int h, int stride, Rgb glyph, Rgb face, int mode)
    {
        const bool darkFace = face.r + face.g + face.b < 3 * 128;   // a dark theme: colour stays as bright as it is
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                uint32_t& p = px[size_t(y) * stride + x];
                int a = p >> 24, r = p >> 16 & 255, g = p >> 8 & 255, b = p & 255;
                if (!a) continue;
                int mx = (std::max)({ r, g, b }), mn = (std::min)({ r, g, b });
                double sat = mx ? double(mx - mn) / mx : 0.0, l = (0.299 * r + 0.587 * g + 0.114 * b) / 255.0;
                double t = (sat - 0.15) / 0.25; t = t < 0 ? 0 : t > 1 ? 1 : t;   // 0 = grey, 1 = coloured
                double k = mx > 200 && !darkFace ? 0.8 : 1.0;
                double ll = mode == 2 ? 1.0 - l : l;   // dark glyphs: black is the glyph
                // a light glyph on a light theme keeps the icon as drawn (its dark outline and shading) and only its light
                // parts take the glyph colour
                double keep = mode == 1 && !darkFace ? 1.0 - (std::min)(1.0, (std::max)(0.0, (l - 0.55) / 0.3)) : 0.0;
                auto mix = [&](int c, int gl, int fa) {
                    double grey = keep > 0 ? c * keep + gl * (1 - keep) : fa + (gl - fa) * ll;
                    double v = t * c * k + (1 - t) * grey;
                    return uint32_t(v < 0 ? 0 : v > 255 ? 255 : v + 0.5); };
                p = uint32_t(a) << 24 | mix(r, glyph.r, face.r) << 16 | mix(g, glyph.g, face.g) << 8 | mix(b, glyph.b, face.b);
            }
    }
    bool SkipForRecolor(const std::wstring& src)   // Qt's own images, sheet images, the splash, the editors' logos
    {
        if (IsLogo(src)) return true;
        static const wchar_t* const kSkip[] = { L":/qt-project.org/", L":/stylesheet/", L":/SplashScreen/" };
        for (const wchar_t* k : kSkip) if (_wcsnicmp(src.c_str(), k, wcslen(k)) == 0) return true;
        return false;
    }
    // the newest write time of any file under dir (0 when there is none): a source folder's icons changed
    uint64_t NewestFile(const std::wstring& dir)
    {
        uint64_t newest = 0;
        std::vector<std::wstring> stack{ dir };
        int seen = 0;
        while (!stack.empty() && seen < 20000)
        {
            std::wstring d = stack.back(); stack.pop_back();
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do
            {
                std::wstring n = fd.cFileName;
                if (n == L"." || n == L".." || n[0] == L'.') continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { stack.push_back(d + L"\\" + n); continue; }
                ++seen;
                uint64_t t = (uint64_t(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
                if (t > newest) newest = t;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        return newest;
    }
    // Writes this editor's recoloured icons into dir (the compiled-in ones and <root>\AssetWorks\*.png under dir\AssetWorks). With a source folder (a theme's own icon set) its images stand in for the editor's.
    // A stamp per editor (".generated-<exe>-v2": colours, source and its newest file time, then every file written) marks it done: other colours or a changed source replace the files it wrote (unless changed since), a user's file stays.
    void Generate(const std::wstring& dir, Rgb glyph, Rgb face, const std::wstring& source)
    {
        wchar_t exe[MAX_PATH] = {}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring name = exe; size_t cut = name.find_last_of(L"\\/"); if (cut != std::wstring::npos) name = name.substr(cut + 1);
        std::wstring stamp = dir + L"\\.generated-" + name + L"-v2";
        char buf[64];
        snprintf(buf, sizeof buf, "r3 %02x%02x%02x %02x%02x%02x | ", glyph.r, glyph.g, glyph.b, face.r, face.g, face.b);   // r3: the drawing rule
        std::string colours = buf + Narrow(source);
        snprintf(buf, sizeof buf, " | %llx", static_cast<unsigned long long>(source.empty() ? 0 : NewestFile(source)));
        colours += buf;
        std::string old;
        {
            HANDLE f = CreateFileW(stamp.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE)
            {
                LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz);
                old.resize(size_t(sz.QuadPart < (4 << 20) ? sz.QuadPart : 0));
                DWORD got = 0; if (!old.empty() && !ReadFile(f, &old[0], DWORD(old.size()), &got, nullptr)) old.clear();
                CloseHandle(f);
            }
        }
        if (!old.empty() && old.substr(0, old.find('\n')) == colours) return;   // made like this already
        uint64_t t0 = GetTickCount64();
        // pack from the first version (".generated-<exe>-v1", no file list): what it wrote is everything not changed since
        std::wstring v1 = dir + L"\\.generated-" + name + L"-v1";
        WIN32_FILE_ATTRIBUTE_DATA v1a{};
        if (GetFileAttributesExW(v1.c_str(), GetFileExInfoStandard, &v1a))
        {
            ULARGE_INTEGER limit; limit.LowPart = v1a.ftLastWriteTime.dwLowDateTime; limit.HighPart = v1a.ftLastWriteTime.dwHighDateTime;
            limit.QuadPart += 5ull * 10000000ull;
            std::vector<std::wstring> stack{ dir };
            int gone = 0;
            while (!stack.empty())
            {
                std::wstring d = stack.back(); stack.pop_back();
                WIN32_FIND_DATAW fd;
                HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
                if (h == INVALID_HANDLE_VALUE) continue;
                do
                {
                    std::wstring n = fd.cFileName;
                    if (n == L"." || n == L".." || n[0] == L'.') continue;
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { stack.push_back(d + L"\\" + n); continue; }
                    ULARGE_INTEGER t; t.LowPart = fd.ftLastWriteTime.dwLowDateTime; t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
                    if (t.QuadPart <= limit.QuadPart && IsImage(n.c_str(), int(n.size())) && DeleteFileW((d + L"\\" + n).c_str())) ++gone;
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            DeleteFileW(v1.c_str());
            rp::Log("icons: %d icons of a first-version pack removed from %s (redrawn now)", gone, Narrow(dir).c_str());
        }
        if (!old.empty())                       // other colours: the files the last run wrote go, unless changed since
        {
            WIN32_FILE_ATTRIBUTE_DATA sa{};
            GetFileAttributesExW(stamp.c_str(), GetFileExInfoStandard, &sa);
            ULARGE_INTEGER limit; limit.LowPart = sa.ftLastWriteTime.dwLowDateTime; limit.HighPart = sa.ftLastWriteTime.dwHighDateTime;
            limit.QuadPart += 5ull * 10000000ull;
            size_t at = old.find('\n');
            while (at != std::string::npos && at + 1 < old.size())
            {
                size_t end = old.find('\n', at + 1);
                std::wstring f = dir + L"\\" + Widen(old.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1));
                at = end;
                WIN32_FILE_ATTRIBUTE_DATA fa{};
                if (!GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &fa)) continue;
                ULARGE_INTEGER t; t.LowPart = fa.ftLastWriteTime.dwLowDateTime; t.HighPart = fa.ftLastWriteTime.dwHighDateTime;
                if (t.QuadPart <= limit.QuadPart) DeleteFileW(f.c_str());
            }
        }
        MakeDirs(dir);
        bool was = g_exporting; g_exporting = true; UpdateLive();   // read the editor's own images
        int made = 0, kept = 0, failed = 0;
        std::string list = colours + "\n";
        auto one = [&](const std::wstring& original, const std::wstring& rel, bool pictograms)
        {
            std::wstring dst = dir + L"\\" + rel;
            if (GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) { ++kept; return; }   // the user's own file stays
            std::wstring custom = source.empty() ? L"" : source + L"\\" + rel;
            const bool fromSource = !custom.empty() && GetFileAttributesW(custom.c_str()) != INVALID_FILE_ATTRIBUTES;
            const std::wstring& src = fromSource ? custom : original;
            alignas(16) unsigned char img[64] = {}, argb[64] = {};
            { QStr qs(src); qt.imageFromFile(img, &qs.d, nullptr); }
            if (!qt.imageIsNull(img))
            {
                qt.imageConvert(img, argb, 5, 0);                    // QImage::Format_ARGB32
                int w = qt.imageWidth(argb), h = qt.imageHeight(argb), bpl = qt.imageBytesPerLine(argb);
                uint32_t* px = reinterpret_cast<uint32_t*>(qt.imageBits(argb));
                int mode = px && w > 0 && h > 0 ? RecolorMode(px, w, h, bpl / 4, pictograms) : 0;
                if (mode)
                {
                    Recolor(px, w, h, bpl / 4, glyph, face, mode);
                    MakeDirs(dst.substr(0, dst.find_last_of(L'\\')));
                    QStr qd(dst);
                    if (qt.imageSave(argb, &qd.d, "PNG", -1)) { ++made; list += Narrow(rel) + "\n"; } else ++failed;
                }
                else if (fromSource)            // a coloured icon of the source set: in the pack as it is
                {
                    MakeDirs(dst.substr(0, dst.find_last_of(L'\\')));
                    if (CopyFileW(custom.c_str(), dst.c_str(), FALSE)) { ++made; list += Narrow(rel) + "\n"; } else ++failed;
                }
                qt.imageDtor(argb);
            }
            qt.imageDtor(img);
        };
        alignas(16) unsigned char it[64] = {};
        { QStr rootPath(L":/"); qt.dirIterCtor(it, &rootPath.d, 0x2); }
        while (qt.dirIterHasNext(it))
        {
            void* q = nullptr; qt.dirIterNext(it, &q);
            std::wstring src = QsToW(&q);
            qt.qsDtor(&q);
            if (src.size() < 3 || !IsImage(src.c_str(), int(src.size())) || SkipForRecolor(src)) continue;
            std::wstring rel = src.substr(2);
            for (wchar_t& ch : rel) if (ch == L'/') ch = L'\\';
            one(src, rel, _wcsnicmp(src.c_str(), L":/AEMainWindow/", 15) == 0);
        }
        qt.dirIterDtor(it);
        WIN32_FIND_DATAW fd;                    // APE's asset type icons on disk
        HANDLE fh = FindFirstFileW((g_root + L"\\AssetWorks\\*").c_str(), &fd);
        if (fh != INVALID_HANDLE_VALUE)
        {
            do
            {
                std::wstring n = fd.cFileName;
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !IsImage(n.c_str(), int(n.size()))) continue;
                one(g_root + L"\\AssetWorks\\" + n, L"AssetWorks\\" + n, true);
            } while (FindNextFileW(fh, &fd));
            FindClose(fh);
        }
        g_exporting = was; UpdateLive();
        HANDLE h = CreateFileW(stamp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (h != INVALID_HANDLE_VALUE) { DWORD wr = 0; WriteFile(h, list.data(), DWORD(list.size()), &wr, nullptr); CloseHandle(h); }
        rp::Log("icons: %d recoloured icons written to %s (%d already there, %d failed) in %llu ms", made, Narrow(dir).c_str(), kept, failed,
                static_cast<unsigned long long>(GetTickCount64() - t0));
    }

    // the value of '<key>: value' in the sheet's first 2 KB, up to the end of the comment or line
    bool Directive(const std::string& sheet, const char* key, std::string& v)
    {
        size_t p = sheet.find(key);
        if (p == std::string::npos || p > 2048) return false;
        p += strlen(key);
        while (p < sheet.size() && (sheet[p] == ' ' || sheet[p] == '\t')) ++p;
        size_t e = sheet.find("*/", p);
        size_t nl = sheet.find_first_of("\r\n", p);
        if (e == std::string::npos || (nl != std::string::npos && nl < e)) e = nl;
        v = sheet.substr(p, e == std::string::npos ? std::string::npos : e - p);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '"')) v.pop_back();
        if (!v.empty() && v[0] == '"') v.erase(0, 1);
        return true;
    }
}

bool Setup(const std::wstring& root)
{
    if (g_registered) return true;
    g_root = root;
    while (!g_root.empty() && (g_root.back() == L'\\' || g_root.back() == L'/')) g_root.pop_back();
    if (!BindQt()) return false;
    Probe();
    g_vt[0] = qt.handlerVtbl[-1];
    qt.handlerCtor(g_handler);                 // registers it (first in Qt's list) with Qt's abstract vtable
    *reinterpret_cast<void**>(g_handler) = &g_vt[1];
    g_registered = true;
    UpdateLive();
    rp::Log("icons: redirect registered");
    return true;
}

namespace
{
    // Recoloured packs live in <themes>\icons\_generated\<theme> (folders starting with "_" are not offered as icon sets).
    // Once per run, in every older folder holding a ".generated-*" stamp: the images not newer than the newest stamp, the stamps
    // and the then empty folders go, a file the user added or changed later stays, and with it the folder.
    void RemoveEmptyDirs(const std::wstring& dir)
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                std::wstring n = fd.cFileName;
                if (n != L"." && n != L".." && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) RemoveEmptyDirs(dir + L"\\" + n);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(dir.c_str());   // fails while anything is left in it
    }
    void RemoveOldGenerated(const std::wstring& themesDir)
    {
        static bool done = false;
        if (done) return;
        done = true;
        const std::wstring icons = themesDir + L"\\icons";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((icons + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        std::vector<std::wstring> dirs;
        do
        {
            std::wstring n = fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n != L"." && n != L".." && n[0] != L'_') dirs.push_back(icons + L"\\" + n);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        for (const std::wstring& dir : dirs)
        {
            uint64_t newest = 0;
            std::vector<std::wstring> stamps;
            HANDLE sh = FindFirstFileW((dir + L"\\.generated-*").c_str(), &fd);
            if (sh == INVALID_HANDLE_VALUE) continue;   // the user's own folder
            do
            {
                stamps.push_back(dir + L"\\" + fd.cFileName);
                uint64_t t = (uint64_t(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
                if (t > newest) newest = t;
            } while (FindNextFileW(sh, &fd));
            FindClose(sh);
            newest += 5ull * 10000000ull;
            int gone = 0, kept = 0;
            std::vector<std::wstring> stack{ dir };
            while (!stack.empty())
            {
                std::wstring d = stack.back(); stack.pop_back();
                HANDLE fh = FindFirstFileW((d + L"\\*").c_str(), &fd);
                if (fh == INVALID_HANDLE_VALUE) continue;
                do
                {
                    std::wstring n = fd.cFileName;
                    if (n == L"." || n == L".." || n[0] == L'.') continue;
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { stack.push_back(d + L"\\" + n); continue; }
                    uint64_t t = (uint64_t(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
                    if (t <= newest && IsImage(n.c_str(), int(n.size())) && DeleteFileW((d + L"\\" + n).c_str())) ++gone; else ++kept;
                } while (FindNextFileW(fh, &fd));
                FindClose(fh);
            }
            for (const std::wstring& st : stamps) DeleteFileW(st.c_str());
            RemoveEmptyDirs(dir);
            rp::Log("icons: old recoloured pack %s removed (%d images%s)", Narrow(dir).c_str(), gone,
                    kept ? (", " + std::to_string(kept) + " of the user's files kept").c_str() : "");
        }
    }

    // Shipped themes renamed or dropped: old name -> its successor
    const struct { const char* from; const char* to; } kRetired[] = { { "Graphite", "Ember" }, { "Fog", "Sage" }, { "Frost", "Light" },
                                                                         { "Photoshop", "Dark" } };
    // a file whose first 1 KB holds marker (the header of the shipped copy, the Theme editor writes another)
    bool HasMarker(const std::wstring& path, const char* marker)
    {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        char buf[1025] = {}; DWORD got = 0;
        ReadFile(f, buf, 1024, &got, nullptr);
        CloseHandle(f);
        return strstr(buf, marker) != nullptr;
    }
    int DeleteTree(const std::wstring& dir)
    {
        int gone = 0;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;
        do
        {
            std::wstring n = fd.cFileName, path = dir + L"\\" + n;
            if (n == L"." || n == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) gone += DeleteTree(path);
            else if (DeleteFileW(path.c_str())) ++gone;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        RemoveDirectoryW(dir.c_str());
        return gone;
    }
}

std::string Renamed(const std::string& name)
{
    for (const auto& r : kRetired) if (_stricmp(r.from, name.c_str()) == 0) return r.to;
    return name;
}

void RetireShipped(const std::wstring& dir)
{
    for (const auto& r : kRetired)
    {
        std::wstring n = Widen(r.from), lower = n;
        for (wchar_t& ch : lower) ch = wchar_t(towlower(ch));
        int gone = 0;
        const std::wstring qss = dir + L"\\" + n + L".qss", colors = dir + L"\\" + n + L".colors";
        if (HasMarker(qss, "Generated by tools/make_themes.py") && DeleteFileW(qss.c_str())) ++gone;
        if (HasMarker(colors, "; MTPlugins view colors") && DeleteFileW(colors.c_str())) ++gone;
        if (GetFileAttributesW(qss.c_str()) == INVALID_FILE_ATTRIBUTES) { gone += DeleteTree(dir + L"\\images\\" + lower); gone += DeleteTree(dir + L"\\icons\\" + n); }   // its images and icon folder, once no sheet of that name uses them
        gone += DeleteTree(dir + L"\\icons\\_generated\\" + n);
        if (gone) rp::Log("theme: retired shipped theme '%s' (now '%s'): %d files removed from %s", r.from, r.to, gone, Narrow(dir).c_str());
    }
}

namespace { bool g_recolor = true; }
void SetRecolor(bool on) { g_recolor = on; }

std::wstring LogoFor(const std::string& sheet, const std::wstring& sheetDir, const char* app)
{
    std::string v;
    const std::string own = app && *app ? std::string("logo-") + app + ":" : std::string();   // 'logo-ape:', 'logo-launcher:'
    if ((own.empty() || !Directive(sheet, own.c_str(), v) || v.empty()) && (!Directive(sheet, "logo:", v) || v.empty())) return L"";
    std::wstring file = Widen(v);
    for (wchar_t& ch : file) if (ch == L'/') ch = L'\\';
    if (file.find(L':') == std::wstring::npos) file = sheetDir + L"\\" + file;
    DWORD a = GetFileAttributesW(file.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) { rp::Log("icons: the theme's logo %s does not exist", Narrow(file).c_str()); return L""; }
    return file;
}

std::wstring PackFor(const std::wstring& themesDir, const std::string& themeName, const std::string& sheet, const std::wstring& sheetDir)
{
    RemoveOldGenerated(themesDir);
    const std::wstring own = themesDir + L"\\icons\\" + Widen(themeName);              // the user's icons for this theme
    const std::wstring generated = themesDir + L"\\icons\\_generated\\" + Widen(themeName);   // its recoloured pack
    std::wstring d = own, folder;
    std::string v, rc;
    const bool none = Directive(sheet, "icon-pack:", v) && _stricmp(v.c_str(), "none") == 0;
    if (!none && !v.empty())
    {
        folder = Widen(v);
        for (wchar_t& ch : folder) if (ch == L'/') ch = L'\\';
        if (folder.find(L':') == std::wstring::npos) folder = sheetDir + L"\\" + folder;
    }
    // '/* icon-recolor: #2b2d31 #f2f2f2 */': the pack is made from the editor's icons, or from the icon-pack folder when the
    // sheet names one (glyph colour, face colour), into the theme's own icon folder
    if (g_recolor && g_registered && Directive(sheet, "icon-recolor:", rc))
    {
        Rgb glyph{}, face{ 242, 242, 242 };
        size_t sp = rc.find(' ');
        if (ParseHex(rc.substr(0, sp), glyph) && (sp == std::string::npos || ParseHex(rc.substr(rc.find_first_not_of(' ', sp)), face)))
        {
            // the source: the icon-pack folder, else the user's own folder for this theme, else the editor's icons
            std::wstring source = !folder.empty() ? folder : DirExists(own) ? own : L"";
            if (!source.empty() && !DirExists(source)) { rp::Log("icons: the theme's icon folder %s does not exist", Narrow(source).c_str()); source.clear(); }
            MakeDirs(themesDir + L"\\icons\\_generated");
            Generate(generated, glyph, face, source);
            return DirExists(generated) ? generated : L"";
        }
        rp::Log("icons: icon-recolor '%s' is not '#rrggbb [#rrggbb]'", rc.c_str());
    }
    if (none) return L"";
    if (!folder.empty()) d = folder;
    if (DirExists(d)) return d;
    if (!v.empty()) rp::Log("icons: the theme's icon folder %s does not exist", Narrow(d).c_str());
    return L"";
}

namespace
{
    // The editors' window icon is not one Qt reloads from a file (the redirect above does not reach it), so a logo is set
    // outright: on the application (windows made later) and on every top-level window, whose own icon is kept to be put
    // back when a theme without a logo follows. Main thread.
    struct SavedIcon { alignas(16) unsigned char icon[16]; };
    std::unordered_map<void*, SavedIcon>* g_savedIcons = new std::unordered_map<void*, SavedIcon>();   // leaked on purpose (exit)
    alignas(16) unsigned char g_appIcon[16] = {};
    bool g_appIconSaved = false;

    int SetLogo(const std::wstring& logo)
    {
        if (!qt.setWindowIcon || !qt.widgetWindow || !qt.appSetWindowIcon || !qt.appWindowIcon || !qt.iconFromFile) return 0;
        alignas(16) unsigned char li[16] = {};
        int n = 0;
        void* list = nullptr; qt.allWidgets(&list);
        if (!logo.empty())
        {
            // Save every window's icon BEFORE the application icon changes: a window without an icon of its own
            // answers windowIcon() with the application's, so a copy taken after would be the logo itself and "put back" would keep the logo.
            ForList(list, [&](void* w)
            {
                if (qt.widgetWindow(w) != w || g_savedIcons->count(w)) return;
                SavedIcon& sv = (*g_savedIcons)[w]; memset(sv.icon, 0, sizeof sv.icon); qt.windowIcon(w, sv.icon);
            });
            QStr path(logo);
            qt.iconFromFile(li, &path.d);
            if (!g_appIconSaved) { qt.appWindowIcon(g_appIcon); g_appIconSaved = true; }
            qt.appSetWindowIcon(li);
        }
        else if (g_appIconSaved) qt.appSetWindowIcon(g_appIcon);
        ForList(list, [&](void* w)
        {
            if (qt.widgetWindow(w) != w) return;
            auto it = g_savedIcons->find(w);
            if (!logo.empty())
            {
                qt.setWindowIcon(w, li);
                ++n;
            }
            else if (it != g_savedIcons->end())
            {
                qt.setWindowIcon(w, it->second.icon);
                qt.iconDtor(it->second.icon);
                g_savedIcons->erase(it);
                ++n;
            }
        });
        ReleaseList(list);
        if (logo.empty())                      // windows closed meanwhile: only their saved copies are left
        {
            for (auto& kv : *g_savedIcons) qt.iconDtor(kv.second.icon);
            g_savedIcons->clear();
        }
        else qt.iconDtor(li);
        return n;
    }

    // The saved icons are held by no widget while the logo is up, so Refresh does not reach them: one whose file the pack
    // replaced would keep the pack's pixmap and be put back with it, leaving the theme's logo on the title and task bars. Their entries are dropped here too.
    void RefreshSaved(const std::unordered_set<std::wstring>& changed, RefreshStats& st)
    {
        if (!g_layout.ok || changed.empty()) return;
        std::unordered_set<void*> seen;
        for (auto& kv : *g_savedIcons) ResetIcon(kv.second.icon, changed, seen, st);
        if (g_appIconSaved) ResetIcon(g_appIcon, changed, seen, st);
    }
}

void Use(const std::wstring& pack, const std::wstring& logo)
{
    if (!g_registered) return;
    std::wstring cur;
    AcquireSRWLockShared(&g_lock);
    if (g_pack) cur = g_pack->dir;
    ReleaseSRWLockShared(&g_lock);
    if (cur.empty() && pack.empty() && logo.empty()) return;
    uint64_t t0 = GetTickCount64();
    Pack* next = pack.empty() ? nullptr : LoadPack(pack);
    if (next && next->files.empty()) { rp::Log("icons: %s holds no images, using the editor's own icons", Narrow(pack).c_str()); delete next; next = nullptr; }
    if (!logo.empty())                         // the sheet's 'logo:' file in place of every editor logo
    {
        if (!next) { next = new Pack(); next->dir = L"(logo only)"; }
        for (const wchar_t* k : kLogos) next->files[Key(std::wstring(k))] = logo;
        (*g_reverse)[Key(logo)] = kLogos[0];
    }
    const bool logoChanged = _wcsicmp(g_logo.c_str(), logo.c_str()) != 0;
    g_logo = logo;
    std::unordered_set<std::wstring> changed;
    AcquireSRWLockExclusive(&g_lock);
    Pack* old = g_pack;
    g_pack = next;
    ReleaseSRWLockExclusive(&g_lock);
    UpdateLive();
    if (old) for (auto& kv : old->files) changed.insert(kv.first);
    if (next) for (auto& kv : next->files) changed.insert(kv.first);
    delete old;                                // no reader left: they hold g_lock while they look
    RefreshStats st = Refresh(changed);
    RefreshSaved(changed, st);
    int windows = logoChanged ? SetLogo(logo) : 0;
    char buf[512];
    snprintf(buf, sizeof buf, "%s (%zu files) in %llu ms: %d icons looked at, %d pixmaps dropped, %d names restored, %d view items%s%s",
             next ? Narrow(next->dir).c_str() : "the editor's own icons", next ? next->files.size() : size_t(0),
             static_cast<unsigned long long>(GetTickCount64() - t0), st.icons, st.reset, st.renamed, st.indexes,
             logo.empty() ? "" : ", logo ", logo.empty() ? "" : Narrow(logo).c_str());
    if (windows) snprintf(buf + strlen(buf), sizeof buf - strlen(buf), ", %d window icons %s", windows, logo.empty() ? "put back" : "set");
    g_lastSwitch = buf;
    rp::Log("icons: %s", buf);
}

namespace
{
    // ---- title bars: Windows paints the native caption in the theme's colours ----
    // DwmSetWindowAttribute per window: CAPTION_COLOR 35, TEXT_COLOR 36, BORDER_COLOR 34 (Windows 11 build 22000+, 0xFFFFFFFF = Windows' own), USE_IMMERSIVE_DARK_MODE 20 (19 before Windows 10 20H1).
    // Only top-level windows of this process with a caption: other programs' windows (BO3 docked in a pane too) are never touched.
    struct TitleColors { bool on = false; COLORREF caption = 0, text = 0, border = 0; BOOL dark = FALSE; };
    TitleColors g_title;
    HWINEVENTHOOK g_titleHook = nullptr;
    using DwmSetFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    DwmSetFn g_dwmSet = nullptr;
    bool g_dwmTried = false;
    int g_titled = 0;

    bool TitleCandidate(HWND h)
    {
        DWORD pid = 0;
        if (!h || !GetWindowThreadProcessId(h, &pid) || pid != GetCurrentProcessId()) return false;
        LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
        return !(st & WS_CHILD) && (st & WS_CAPTION) == WS_CAPTION;
    }
    void PaintTitle(HWND h)
    {
        if (!g_dwmSet || !TitleCandidate(h)) return;
        const TitleColors& t = g_title;
        const COLORREF def = 0xFFFFFFFF;
        BOOL dark = t.on && t.dark;
        if (FAILED(g_dwmSet(h, 20, &dark, sizeof dark))) g_dwmSet(h, 19, &dark, sizeof dark);
        COLORREF caption = t.on ? t.caption : def, text = t.on ? t.text : def, border = t.on ? t.border : def;
        g_dwmSet(h, 34, &border, sizeof border);
        g_dwmSet(h, 36, &text, sizeof text);
        if (FAILED(g_dwmSet(h, 35, &caption, sizeof caption)) && IsWindowVisible(h))   // Windows 10: redraw the frame for the dark switch
            SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        ++g_titled;
    }
    BOOL CALLBACK PaintTitleEnum(HWND h, LPARAM) { PaintTitle(h); return TRUE; }
    // windows made or shown after the theme: before their first paint where Windows tells us at creation
    void CALLBACK TitleEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD)
    {
        if (ev != EVENT_OBJECT_DESTROY && obj == OBJID_WINDOW && child == CHILDID_SELF && g_title.on) PaintTitle(h);
    }
    // '#rrggbb' -> COLORREF
    bool TitleHex(const std::string& t, COLORREF& c)
    {
        Rgb v;
        if (t.empty() || !ParseHex(t, v)) return false;
        c = RGB(v.r, v.g, v.b);
        return true;
    }
    // the palette line's value for key ('bg=#323232 panel=#2b2b2b ...')
    std::string PaletteKey(const std::string& palette, const char* key)
    {
        const std::string k = std::string(key) + "=";
        for (size_t p = 0; p < palette.size();)
        {
            size_t e = palette.find(' ', p);
            if (e == std::string::npos) e = palette.size();
            if (palette.compare(p, k.size(), k) == 0) return palette.substr(p + k.size(), e - p - k.size());
            p = e + 1;
        }
        return "";
    }
}

void UseTitleBar(const std::string& sheet, const std::string& themeName)
{
    if (!g_dwmTried)
    {
        g_dwmTried = true;
        if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) g_dwmSet = reinterpret_cast<DwmSetFn>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    }
    if (!g_dwmSet) return;
    TitleColors t;
    std::string v, pal;
    if (!Directive(sheet, "titlebar:", v) && !Directive(sheet, "palette:", pal))   // the editors' own two looks
    {
        if (IsClassicTreyarch(themeName)) v = "#323232 #b1b1b1 #4a4a4a";   // radiant\stylesheet.qss: QWidget #323232 on #b1b1b1
        else if (_stricmp(themeName.c_str(), "classic") == 0) v = "#ffffff #000000 #c2c2c2";   // Qt's WindowsVista menu bar is white
    }
    if (!v.empty() && _stricmp(v.c_str(), "none") != 0)
    {
        std::string c[3];
        size_t n = 0;
        for (size_t p = 0; p < v.size() && n < 3;)
        {
            size_t e = v.find(' ', p);
            if (e == std::string::npos) e = v.size();
            if (e > p) c[n++] = v.substr(p, e - p);
            p = e + 1;
        }
        t.on = TitleHex(c[0], t.caption);
        if (!TitleHex(c[1], t.text)) t.text = (GetRValue(t.caption) * 299 + GetGValue(t.caption) * 587 + GetBValue(t.caption) * 114) / 1000 < 128 ? RGB(255, 255, 255) : RGB(0, 0, 0);
        if (!TitleHex(c[2], t.border)) t.border = t.caption;
    }
    else if (v.empty() && !pal.empty())        // the menu bar's colour, so the caption runs into it
    {
        t.on = TitleHex(PaletteKey(pal, "panel"), t.caption) || TitleHex(PaletteKey(pal, "bg"), t.caption);
        if (!TitleHex(PaletteKey(pal, "text"), t.text)) t.text = RGB(255, 255, 255);
        if (!TitleHex(PaletteKey(pal, "border"), t.border)) t.border = t.caption;
    }
    t.dark = (GetRValue(t.caption) * 299 + GetGValue(t.caption) * 587 + GetBValue(t.caption) * 114) / 1000 < 128;
    if (!t.on && !g_title.on) return;          // Windows' own already
    if (t.on && g_title.on && t.caption == g_title.caption && t.text == g_title.text && t.border == g_title.border) return;
    g_title = t;
    g_titled = 0;
    EnumWindows(PaintTitleEnum, 0);            // every top-level window, PaintTitle keeps to this process's
    if (t.on && !g_titleHook)
        g_titleHook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, nullptr, TitleEvent, GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
    else if (!t.on && g_titleHook) { UnhookWinEvent(g_titleHook); g_titleHook = nullptr; }
    if (t.on) rp::Log("titlebar: caption #%06x text #%06x border #%06x on %d windows", unsigned(RGB(GetBValue(t.caption), GetGValue(t.caption), GetRValue(t.caption))),
                      unsigned(RGB(GetBValue(t.text), GetGValue(t.text), GetRValue(t.text))), unsigned(RGB(GetBValue(t.border), GetGValue(t.border), GetRValue(t.border))), g_titled);
    else rp::Log("titlebar: Windows' own put back on %d windows", g_titled);
}

void Shutdown()
{
    UseTitleBar("");                          // before the DLL goes: the hook calls into it
    if (!g_registered) return;
    Use(L"");
    qt.handlerDtor(g_handler);                 // unregisters it under Qt's write lock: no create() runs after this
    g_registered = false;
    UpdateLive();
}

std::string Command(const std::string& args, const std::wstring& themesDir)
{
    if (!g_registered) return "icons: the redirect is not registered (see the log)";
    if (args.empty() || args == "status") return Status();
    if (args == "refresh")
    {
        std::wstring cur;
        AcquireSRWLockShared(&g_lock); if (g_pack) cur = g_pack->dir; ReleaseSRWLockShared(&g_lock);
        Use(L""); if (!cur.empty()) Use(cur);
        return Status();
    }
    if (args == "export" || args.rfind("export ", 0) == 0)
    {
        std::wstring out = args.size() > 7 ? Widen(args.substr(7)) : themesDir + L"\\icons\\_editor_defaults";
        return Export(out);
    }
    if (args.rfind("use ", 0) == 0)            // try a pack without a theme (not saved, the next theme switch replaces it)
    {
        std::wstring dir = Widen(args.substr(4));
        if (_wcsicmp(dir.c_str(), L"none") == 0) dir.clear();
        else if (!DirExists(dir)) return "icons: no folder " + args.substr(4);
        Use(dir);
        return Status();
    }
    if (args == "trace on") { g_trace = true; return "icons: tracing the image paths the editor opens ('theme icons trace show')"; }
    if (args == "trace off") { g_trace = false; return "icons: trace off"; }
    if (args == "trace show")
    {
        std::string s;
        AcquireSRWLockShared(&g_traceLock);
        for (const std::string& l : *g_traceLines) s += l + "\n";
        ReleaseSRWLockShared(&g_traceLock);
        return s.empty() ? "icons: nothing traced yet" : s;
    }
    return "icons: theme icons [status|refresh|use <folder>|none|export [dir]|trace on|off|show]";
}
}
