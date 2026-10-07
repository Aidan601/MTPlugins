#include <windows.h>
#include <cstring>
#include <mutex>
#include <deque>
#include <string>
#include <unordered_set>
#include "ape.h"
#include "app.h"
#include "qt.h"
#include "mainthread.h"
#include "plugins.h"
#include "pluginmgr.h"
#include "ui.h"
#include "dock.h"
#include "log.h"
#include "radiant_addrs.h"
#include "MinHook.h"

namespace
{
    void* g_mainWin = nullptr;         // apeMainWindow (QMainWindow*)
    void* g_tabs = nullptr;            // apeAssetTabWidget (QTabWidget*): the editor tabs
    void* g_tree = nullptr;            // apeAssetTreeWidget (QTreeWidget*): the asset tree
    void* g_output = nullptr;          // the Output pane's text box
    bool  g_outputIsPlain = false;
    bool  g_lookedUp = false;
    ULONGLONG g_lastLookup = 0;
    ape::Asset g_lastCurrent; bool g_haveCurrent = false;
    ULONGLONG g_lastAssetPoll = 0;
    std::mutex g_printMutex;
    std::deque<std::string> g_printQueue;

    // Depth-first search of the QObject tree for the first object of that class (QMetaObject chain).
    void* FindByClass(void* root, const char* cls, int depth = 0)
    {
        if (!root || depth > 40) return nullptr;
        for (void* c : qt::Children(root))
        {
            if (qt::IsA(c, cls)) return c;
            if (void* r = FindByClass(c, cls, depth + 1)) return r;
        }
        return nullptr;
    }

    void* FindTextBox(void* root, int depth = 0)
    {
        if (!root || depth > 40) return nullptr;
        for (void* c : qt::Children(root))
        {
            if (qt::IsA(c, "QTextEdit") || qt::IsA(c, "QPlainTextEdit")) return c;
            if (void* r = FindTextBox(c, depth + 1)) return r;
        }
        return nullptr;
    }

    // The Output pane: the text edit inside the dock whose object name says "Output" (QDockWidget 'OutputToolBar' > QTextEdit).
    void* FindOutput(void* root)
    {
        for (void* c : qt::Children(root))
        {
            if (!qt::IsA(c, "QDockWidget")) continue;
            std::string name = qt::ObjectName(c);
            for (auto& ch : name) ch = char(tolower(static_cast<unsigned char>(ch)));
            if (name.find("output") == std::string::npos) continue;
            if (void* t = FindTextBox(c)) return t;
        }
        return nullptr;
    }

    // The pages of the editor QTabWidget (its QStackedWidget's apeTabWidget children), in tab order.
    std::vector<void*> TabPages()
    {
        std::vector<void*> pages;
        if (!g_tabs) return pages;
        void* stack = FindByClass(g_tabs, "QStackedWidget");
        if (!stack) return pages;
        int n = qt::TabCount(g_tabs);
        pages.assign(size_t(n > 0 ? n : 0), nullptr);
        for (void* c : qt::Children(stack))
        {
            if (!qt::IsA(c, "QWidget")) continue;
            int i = qt::TabIndexOf(g_tabs, c);
            if (i >= 0 && i < n) pages[size_t(i)] = c;
        }
        return pages;
    }

    // A tab page's asset: the apeAssetLineEditWidget in its toolbar shows "<name>.<type>" (the same text Copy Asset Link uses).
    bool PageAsset(void* page, ape::Asset& out)
    {
        if (!page) return false;
        void* edit = FindByClass(page, "apeAssetLineEditWidget");
        if (!edit || !qt::IsA(edit, "QLineEdit")) edit = FindByClass(page, "QLineEdit");
        if (!edit) return false;
        std::string text = qt::EditText(edit);
        size_t dot = text.rfind('.');
        if (text.empty() || dot == std::string::npos || dot == 0) return false;
        out.name = text.substr(0, dot); out.type = text.substr(dot + 1);
        return true;
    }

    // ---- gdt.db through the sqlite the mod tools ship (sqlite64r.dll, already loaded by APE) ----
    struct Sqlite
    {
        void* db = nullptr;
        int  (*open_v2)(const char*, void**, int, const char*) = nullptr;
        int  (*prepare_v2)(void*, const char*, int, void**, const char**) = nullptr;
        int  (*bind_text)(void*, int, const char*, int, void*) = nullptr;
        int  (*step)(void*) = nullptr;
        const unsigned char* (*column_text)(void*, int) = nullptr;
        int  (*finalize)(void*) = nullptr;
        bool tried = false;
    } g_sql;

    bool OpenDb()
    {
        if (g_sql.db) return true;
        if (g_sql.tried) return false;
        g_sql.tried = true;
        HMODULE m = GetModuleHandleW(L"sqlite64r.dll");
        if (!m) m = LoadLibraryW((plugins::RadiantRoot() + L"\\bin\\sqlite64r.dll").c_str());
        if (!m) { LOG("ape: sqlite64r.dll not available; asset type lookups disabled"); return false; }
        g_sql.open_v2 = reinterpret_cast<decltype(g_sql.open_v2)>(GetProcAddress(m, "sqlite3_open_v2"));
        g_sql.prepare_v2 = reinterpret_cast<decltype(g_sql.prepare_v2)>(GetProcAddress(m, "sqlite3_prepare_v2"));
        g_sql.bind_text = reinterpret_cast<decltype(g_sql.bind_text)>(GetProcAddress(m, "sqlite3_bind_text"));
        g_sql.step = reinterpret_cast<decltype(g_sql.step)>(GetProcAddress(m, "sqlite3_step"));
        g_sql.column_text = reinterpret_cast<decltype(g_sql.column_text)>(GetProcAddress(m, "sqlite3_column_text"));
        g_sql.finalize = reinterpret_cast<decltype(g_sql.finalize)>(GetProcAddress(m, "sqlite3_finalize"));
        if (!g_sql.open_v2 || !g_sql.prepare_v2 || !g_sql.bind_text || !g_sql.step || !g_sql.column_text || !g_sql.finalize) { LOG("ape: sqlite exports missing"); return false; }
        std::wstring wpath = ape::GdtDbPath();
        int n = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string path(n ? n - 1 : 0, '\0'); if (n) WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, &path[0], n, nullptr, nullptr);
        int rc = g_sql.open_v2(path.c_str(), &g_sql.db, 0x00000001 /* READONLY */ | 0x00010000 /* FULLMUTEX */, nullptr);
        if (rc != 0) { LOG("ape: could not open %s (sqlite rc %d)", path.c_str(), rc); g_sql.db = nullptr; return false; }
        LOG("ape: gdt.db opened read-only: %s", path.c_str());
        return true;
    }

    // Every type an asset of that name exists as (a name is unique per type, not globally).
    std::vector<std::string> TypesOf(const std::string& name)
    {
        std::vector<std::string> out;
        if (!OpenDb()) return out;
        void* stmt = nullptr;
        if (g_sql.prepare_v2(g_sql.db, "SELECT g.name FROM _entity e INNER JOIN _gdf g ON e.FK_gdf = g.PK_id WHERE e.name = ? ORDER BY g.name;", -1, &stmt, nullptr) != 0) return out;
        g_sql.bind_text(stmt, 1, name.c_str(), int(name.size()), reinterpret_cast<void*>(-1) /* SQLITE_TRANSIENT */);
        while (g_sql.step(stmt) == 100 /* SQLITE_ROW */) { const unsigned char* t = g_sql.column_text(stmt, 0); if (t) out.push_back(reinterpret_cast<const char*>(t)); }
        g_sql.finalize(stmt);
        return out;
    }

    // Assets whose name contains `namePart` (case-insensitive, empty = any), optionally of one type.
    // No ORDER BY: gdt.db holds hundreds of thousands of rows and the caller only wants the first `limit`.
    std::vector<ape::Asset> SearchAssets(const std::string& namePart, const std::string& type, size_t limit)
    {
        std::vector<ape::Asset> out;
        if (!OpenDb() || !limit) return out;
        std::string pattern = "%";
        for (char c : namePart) { if (c == '%' || c == '_' || c == '\\') pattern += '\\'; pattern += c; }
        pattern += '%';
        void* stmt = nullptr;
        const char* sql =
            "SELECT e.name, g.name FROM _entity e INNER JOIN _gdf g ON e.FK_gdf = g.PK_id "
            "WHERE (?1 = '' OR e.name LIKE ?2 ESCAPE '\\') AND (?3 = '' OR g.name LIKE ?3);";
        if (g_sql.prepare_v2(g_sql.db, sql, -1, &stmt, nullptr) != 0) return out;
        void* transient = reinterpret_cast<void*>(-1);   // SQLITE_TRANSIENT
        g_sql.bind_text(stmt, 1, namePart.c_str(), int(namePart.size()), transient);
        g_sql.bind_text(stmt, 2, pattern.c_str(), int(pattern.size()), transient);
        g_sql.bind_text(stmt, 3, type.c_str(), int(type.size()), transient);
        while (out.size() < limit && g_sql.step(stmt) == 100 /* SQLITE_ROW */)
        {
            const unsigned char* n = g_sql.column_text(stmt, 0);
            const unsigned char* t = g_sql.column_text(stmt, 1);
            if (!n) continue;
            ape::Asset a; a.name = reinterpret_cast<const char*>(n); a.type = t ? reinterpret_cast<const char*>(t) : "";
            out.push_back(a);
        }
        g_sql.finalize(stmt);
        return out;
    }

    // Every asset type (GDF name) the database knows: the vocabulary for a type filter.
    std::vector<std::string> TypeNames()
    {
        std::vector<std::string> out;
        if (!OpenDb()) return out;
        void* stmt = nullptr;
        if (g_sql.prepare_v2(g_sql.db, "SELECT name FROM _gdf ORDER BY name;", -1, &stmt, nullptr) != 0) return out;
        while (g_sql.step(stmt) == 100 /* SQLITE_ROW */) { const unsigned char* t = g_sql.column_text(stmt, 0); if (t) out.push_back(reinterpret_cast<const char*>(t)); }
        g_sql.finalize(stmt);
        return out;
    }

    std::wstring Widen(const std::string& utf8)
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
        std::wstring w(n ? n - 1 : 0, L'\0');
        if (n) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &w[0], n);
        return w;
    }
    // Where an asset lives: its .gdt file and line, and its parent asset when it is derived ("child" [ "parent" ]).
    bool QueryAsset(const std::string& name, const std::string& type, ape::AssetInfo& out)
    {
        if (!OpenDb()) return false;
        void* stmt = nullptr;
        const char* sql =
            "SELECT t.name, e._gdt_linenum, p.name, pg.name FROM _entity e "
            "INNER JOIN _gdf g ON e.FK_gdf = g.PK_id INNER JOIN _gdt t ON e.FK_gdt = t.PK_id "
            "LEFT JOIN _entity p ON e.FK_parent_id = p.PK_id LEFT JOIN _gdf pg ON p.FK_gdf = pg.PK_id "
            "WHERE e.name = ?1 AND (?2 = '' OR g.name LIKE ?2) LIMIT 1;";
        if (g_sql.prepare_v2(g_sql.db, sql, -1, &stmt, nullptr) != 0) return false;
        void* transient = reinterpret_cast<void*>(-1);
        g_sql.bind_text(stmt, 1, name.c_str(), int(name.size()), transient);
        g_sql.bind_text(stmt, 2, type.c_str(), int(type.size()), transient);
        bool found = false;
        if (g_sql.step(stmt) == 100)
        {
            const unsigned char* f = g_sql.column_text(stmt, 0);
            const unsigned char* l = g_sql.column_text(stmt, 1);
            const unsigned char* pn = g_sql.column_text(stmt, 2);
            const unsigned char* pt = g_sql.column_text(stmt, 3);
            out.gdtPath = f ? Widen(reinterpret_cast<const char*>(f)) : L"";
            out.line = l ? atoi(reinterpret_cast<const char*>(l)) : 0;
            out.parentName = pn ? reinterpret_cast<const char*>(pn) : "";
            out.parentType = pt ? reinterpret_cast<const char*>(pt) : "";
            found = true;
        }
        g_sql.finalize(stmt);
        return found;
    }
    // The property schema of a type (gdt.db _meta): one "key<TAB>type<TAB>default<TAB>enums" line per property.
    std::string QueryTypeProperties(const std::string& type)
    {
        std::string out;
        if (!OpenDb()) return out;
        void* stmt = nullptr;
        if (g_sql.prepare_v2(g_sql.db, "SELECT _key, _type, _default, _enums FROM _meta WHERE _name = ? ORDER BY _index, _key;", -1, &stmt, nullptr) != 0) return out;
        g_sql.bind_text(stmt, 1, type.c_str(), int(type.size()), reinterpret_cast<void*>(-1));
        while (g_sql.step(stmt) == 100)
        {
            for (int c = 0; c < 4; ++c)
            {
                const unsigned char* t = g_sql.column_text(stmt, c);
                std::string v = t ? reinterpret_cast<const char*>(t) : "";
                for (char& ch : v) if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
                out += v; out += c < 3 ? '\t' : '\n';
            }
        }
        g_sql.finalize(stmt);
        return out;
    }

    void LookupWidgets()
    {
        ULONGLONG now = GetTickCount64();
        if (g_lookedUp || now - g_lastLookup < 1000) return;
        g_lastLookup = now;
        void* mw = ape::MainWindowWidget();
        if (!mw) return;
        if (!g_tabs) g_tabs = FindByClass(mw, "apeAssetTabWidget");
        if (!g_tabs) g_tabs = FindByClass(mw, "QTabWidget");
        if (!g_tree) g_tree = FindByClass(mw, "apeAssetTreeWidget");
        if (!g_tree) g_tree = FindByClass(mw, "QTreeWidget");
        if (!g_output) { g_output = FindOutput(mw); if (g_output) g_outputIsPlain = qt::IsA(g_output, "QPlainTextEdit"); }
        if (g_tabs && g_tree && g_output)
        {
            g_lookedUp = true;
            LOG("ape: widgets bound: tabs 0x%p (%s) tree 0x%p (%s) output 0x%p (%s '%s')", g_tabs, qt::ClassName(g_tabs), g_tree, qt::ClassName(g_tree), g_output, qt::ClassName(g_output), qt::ObjectName(g_output).c_str());
        }
    }

    // Tab text is "<name>", "<name> (<type>)" or "<name>.<type>", else the raw text is the name.
    ape::Asset ParseTabText(const std::string& text)
    {
        ape::Asset a;
        std::string t = text;
        while (!t.empty() && (t.back() == ' ' || t.back() == '*')) t.pop_back();   // a modified marker, if APE adds one
        size_t open = t.rfind(" (");
        if (open != std::string::npos && !t.empty() && t.back() == ')') { a.name = t.substr(0, open); a.type = t.substr(open + 2, t.size() - open - 3); return a; }
        size_t dot = t.rfind('.');
        if (dot != std::string::npos && dot > 0) { a.name = t.substr(0, dot); a.type = t.substr(dot + 1); return a; }
        a.name = t;
        return a;
    }

    void FlushPrints()
    {
        std::deque<std::string> q;
        { std::lock_guard<std::mutex> lock(g_printMutex); q.swap(g_printQueue); }
        if (q.empty()) return;
        if (!g_output) { for (auto& l : q) LOG("ape: output (no Output pane found): %s", l.c_str()); return; }
        for (auto& l : q)
        {
            if (g_outputIsPlain) { qt::PlainAppend(g_output, l.c_str()); continue; }
            // append keeps the last line's format (APE's red errors), so give plugin lines their own colour
            std::string html = "<span style=\"color:#3a7bd5\">";
            for (char c : l) { if (c == '<') html += "&lt;"; else if (c == '>') html += "&gt;"; else if (c == '&') html += "&amp;"; else html += c; }
            html += "</span>";
            qt::TextEditAppend(g_output, html.c_str());
        }
    }

    void WatchCurrentAsset()
    {
        ULONGLONG now = GetTickCount64();
        if (now - g_lastAssetPoll < 100) return;
        g_lastAssetPoll = now;
        ape::Asset a;
        bool have = ape::CurrentAsset(a);
        if (have == g_haveCurrent && a.name == g_lastCurrent.name && a.type == g_lastCurrent.type) return;
        g_haveCurrent = have; g_lastCurrent = a;
        LOG("ape: current asset now '%s' (%s)", a.name.c_str(), a.type.c_str());
        plugins::OnAssetChanged(a.name, a.type);
    }

    // SaveCache's parallel jobs detach the shared entry list at once (double free). Detach it once on the main thread
    // first, with APE's own reallocData, so the jobs find it unshared.
    namespace filecache
    {
        using SaveFn = uintptr_t(*)(void*, void*, void*, void*);
        using ReallocFn = void(*)(void*, int, int, int);
        SaveFn g_origSave = nullptr;
        ReallocFn g_realloc = nullptr;

        uintptr_t hk_SaveCache(void* self, void* a2, void* a3, void* a4)
        {
            __try
            {
                auto** d = reinterpret_cast<int32_t**>(static_cast<char*>(self) + apeaddr::off_FileCache_Entries);
                int32_t* hdr = *d;                           // QArrayData: ref, size, alloc:31|capacityReserved:1
                uint32_t ref = uint32_t(hdr[0]);
                int size = hdr[1], alloc = hdr[2] & 0x7fffffff;
                if (ref > 1 && ref != 0xFFFFFFFFu && alloc > 0)   // shared, not the static empty block
                {
                    g_realloc(d, size, alloc, 0);
                    LOG("ape: file cache save: entry list (%d entries) was shared (ref %u), detached before the parallel save", size, ref);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { LOG("ape: file cache save guard faulted; calling SaveCache unchanged"); }
            return g_origSave(self, a2, a3, a4);
        }

        bool StartsFunction(void* target)
        {
            DWORD64 base = 0; PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(DWORD64(target), &base, nullptr);
            return fn && DWORD64(fn->BeginAddress) + base == DWORD64(target);
        }
    }

    // A .gdt rewritten by another program leaves the deleted assets pointing at freed records (entry+0x30). Clear those
    // before ReloadDeleteAssetDialog runs, so APE builds the gone ones from the type defaults.
    namespace reloadguard
    {
        using RdadFn = void(*)(void*, void*, void*);
        RdadFn g_orig = nullptr;
        struct QAD { int ref; int size; unsigned alloc; ptrdiff_t offset; };
        template <class T> T* Data(QAD* d) { return reinterpret_cast<T*>(reinterpret_cast<char*>(d) + d->offset); }

        // lower case, '/' -> '\\', runs of '\\' collapsed: what QFileInfo equality amounts to for these paths
        std::wstring Norm(const std::wstring& in)
        {
            std::wstring o; o.reserve(in.size());
            for (wchar_t c : in)
            {
                if (c == L'/') c = L'\\';
                if (c == L'\\' && !o.empty() && o.back() == L'\\') continue;
                o += (c < 128) ? wchar_t(towlower(c)) : c;
            }
            return o;
        }
        std::wstring QStr(QAD* d) { return (d && d->size > 0) ? std::wstring(Data<wchar_t>(d), size_t(d->size)) : std::wstring(); }

        int Clear(void* tree, void* changed, std::string& files)
        {
            QAD* cd = changed ? *static_cast<QAD**>(changed) : nullptr;
            if (!cd || cd->size <= 0) return 0;
            QAD* rows = *reinterpret_cast<QAD**>(static_cast<char*>(tree) + apeaddr::off_Tree_GdtRows);
            if (!rows || rows->size <= 0) return 0;
            char game[1024] = {}; GetEnvironmentVariableA("TA_GAME_PATH", game, sizeof game);   // APE's GetGamePathStr = getenv("TA_GAME_PATH")
            int cleared = 0;
            for (int i = 0; i < cd->size; ++i)
            {
                char* ce = Data<char*>(cd)[i];
                if (!ce) continue;
                std::string rel(ce, strnlen(ce, 0x104));
                std::string full = std::string(game) + "\\" + rel;                            // APE: "%1\\%2".arg(GetGamePathStr(), path)
                std::wstring want = Norm(std::wstring(full.begin(), full.end()));
                QAD* recs = *reinterpret_cast<QAD**>(ce + apeaddr::off_CacheEntry_Records);
                std::unordered_set<void*> live;
                if (recs) for (int r = 0; r < recs->size; ++r) live.insert(Data<void*>(recs)[r]);
                int here = 0;
                for (int k = 0; k < rows->size; ++k)
                {
                    char* row = reinterpret_cast<char*>(Data<char>(rows)) + size_t(k) * 24;
                    if (Norm(QStr(*reinterpret_cast<QAD**>(row + 8))) != want) continue;
                    QAD* ents = *reinterpret_cast<QAD**>(row + 16);
                    if (ents) for (int e = 0; e < ents->size; ++e)
                    {
                        char* ent = Data<char*>(ents)[e];
                        if (!ent || *reinterpret_cast<void**>(ent + apeaddr::off_Entry_Pairs)) continue;
                        void*& rec = *reinterpret_cast<void**>(ent + apeaddr::off_Entry_Record);
                        if (rec && !live.count(rec)) { rec = nullptr; ++here; }
                    }
                    break;
                }
                if (here) { cleared += here; if (files.size() < 300) files += (files.empty() ? "" : ", ") + rel + " (" + std::to_string(here) + ")"; }
            }
            return cleared;
        }
        int ClearGuarded(void* tree, void* changed, std::string& files)
        {
            __try { return Clear(tree, changed, files); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
        }
        void hk_ReloadDeleteAssetDialog(void* tree, void* changed, void* deletedFiles)
        {
            std::string files;
            int n = ClearGuarded(tree, changed, files);
            if (n < 0) LOG("ape: reload guard faulted; ReloadDeleteAssetDialog runs unchanged");
            else if (n > 0) LOG("ape: reload guard: %d assets of changed GDTs pointed at records APE had just freed; cleared before the reload (it re-points the ones still in the file, drops the rest) (%s)", n, files.c_str());
            g_orig(tree, changed, deletedFiles);
        }
    }
}

namespace ape
{
    static void InstallSaveCacheGuard()
    {
        void* save = reinterpret_cast<void*>(app::ModuleBase() + (apeaddr::apeFileCache_SaveCache - 0x140000000ULL));
        void* realloc = reinterpret_cast<void*>(app::ModuleBase() + (apeaddr::QVectorPtr_reallocData - 0x140000000ULL));
        if (!filecache::StartsFunction(save) || !filecache::StartsFunction(realloc)) { LOG("ape: file cache save guard off (functions not where expected)"); return; }
        filecache::g_realloc = static_cast<filecache::ReallocFn>(realloc);
        if (MH_CreateHook(save, reinterpret_cast<void*>(&filecache::hk_SaveCache), reinterpret_cast<void**>(&filecache::g_origSave)) != MH_OK
            || MH_EnableHook(save) != MH_OK) { LOG("ape: file cache save guard off (detour failed)"); return; }
        LOG("ape: file cache save guard installed at 0x%p", save);
    }
    static void InstallReloadGuard()
    {
        void* rdad = reinterpret_cast<void*>(app::ModuleBase() + (apeaddr::ReloadDeleteAssetDialog - 0x140000000ULL));
        if (!filecache::StartsFunction(rdad)) { LOG("ape: reload guard off (function not where expected)"); return; }
        if (MH_CreateHook(rdad, reinterpret_cast<void*>(&reloadguard::hk_ReloadDeleteAssetDialog), reinterpret_cast<void**>(&reloadguard::g_orig)) != MH_OK
            || MH_EnableHook(rdad) != MH_OK) { LOG("ape: reload guard off (detour failed)"); return; }
        LOG("ape: reload guard installed at 0x%p", rdad);
    }
    void InstallStartupFixes()
    {
        if (!app::VerifyApeBinary()) return;
        InstallSaveCacheGuard();
        InstallReloadGuard();
        InstallGdtScan();
    }

    void* MainWindowWidget()
    {
        if (g_mainWin) return g_mainWin;
        if (!qt::Bind()) return nullptr;
        HWND h = mt::MainWindow();
        if (!h) return nullptr;
        void* w = qt::FindWidgetByHwnd(h);
        if (!w || !qt::IsMainWindow(w)) return nullptr;
        g_mainWin = w;
        LOG("ape: main window widget 0x%p (%s '%s') for hwnd 0x%p", w, qt::ClassName(w), qt::ObjectName(w).c_str(), h);
        return g_mainWin;
    }

    bool Ready() { return MainWindowWidget() != nullptr; }

    void Start() { EndGdtScan(); mt::StartTimerTick(Tick, 16); }

    void Tick()
    {
        mt::DrainNow();
        ui::Tick();
        pluginmgr::Tick();
        plugins::OnTick();
        if (!plugins::Initialized()) return;
        LookupWidgets();
        FlushPrints();
        WatchCurrentAsset();
        static ULONGLONG lastPanes = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastPanes > 500) { lastPanes = now; dock::RememberVisibility(); }
    }

    bool CurrentAsset(Asset& out)
    {
        LookupWidgets();
        if (!g_tabs) return false;
        int i = qt::TabCurrent(g_tabs);
        return TabAsset(i, out);
    }

    int TabCount() { LookupWidgets(); return g_tabs ? qt::TabCount(g_tabs) : 0; }

    bool TabAsset(int index, Asset& out)
    {
        LookupWidgets();
        if (!g_tabs || index < 0 || index >= qt::TabCount(g_tabs)) return false;
        std::vector<void*> pages = TabPages();
        if (index < int(pages.size()) && PageAsset(pages[size_t(index)], out)) return true;
        // fallback: the tab title is the asset name, the type comes from gdt.db (first match)
        std::string text = qt::TabText(g_tabs, index);
        if (text.empty() || text == "New Tab") return false;
        out = ParseTabText(text);
        if (out.type.empty()) { std::vector<std::string> t = TypesOf(out.name); if (!t.empty()) out.type = t[0]; }
        return !out.name.empty();
    }

    std::vector<std::string> AssetTypes(const std::string& name) { return TypesOf(name); }

    std::vector<Asset> FindAssets(const std::string& namePart, const std::string& type, size_t limit)
    {
        return SearchAssets(namePart, type, limit);
    }

    std::vector<std::string> AssetTypeNames() { return TypeNames(); }
    bool LookupAsset(const std::string& name, const std::string& type, AssetInfo& out) { return QueryAsset(name, type, out); }
    std::string TypeProperties(const std::string& type) { return QueryTypeProperties(type); }
    // ---- the property grid ----
    static bool IsControl(void* c)
    {
        return qt::IsA(c, "QLineEdit") || qt::IsA(c, "QDoubleSpinBox") || qt::IsA(c, "QSpinBox") || qt::IsA(c, "QComboBox")
            || qt::IsA(c, "QCheckBox") || qt::IsA(c, "QPlainTextEdit") || qt::IsA(c, "QTextEdit");
    }
    static void CollectParts(void* w, std::vector<void*>& parts, int depth)
    {
        if (depth > 6) return;
        for (void* c : qt::Children(w))
        {
            if (qt::IsA(c, "QLineEdit")) { if (qt::ObjectName(c) != "qt_spinbox_lineedit") parts.push_back(c); continue; }
            if (IsControl(c)) { parts.push_back(c); continue; }
            if (qt::IsA(c, "QWidget")) CollectParts(c, parts, depth + 1);
        }
    }
    static std::string Num(double v) { char b[64]; if (v == floor(v) && fabs(v) < 1e12) snprintf(b, sizeof b, "%lld", (long long)v); else snprintf(b, sizeof b, "%g", v); return b; }
    static std::string PartValue(void* c)
    {
        if (qt::IsA(c, "QLineEdit")) return qt::EditText(c);
        if (qt::IsA(c, "QDoubleSpinBox")) return Num(qt::DoubleSpinValue(c));
        if (qt::IsA(c, "QSpinBox")) return std::to_string(qt::SpinValue(c));
        if (qt::IsA(c, "QComboBox")) return qt::ComboCurrentText(c);
        if (qt::IsA(c, "QCheckBox")) return qt::IsChecked(c) ? "1" : "0";
        if (qt::IsA(c, "QPlainTextEdit")) return qt::PlainText(c);
        if (qt::IsA(c, "QTextEdit")) return qt::TextEditText(c);
        return std::string();
    }
    static bool SetPart(void* c, const std::string& v, std::string& why)
    {
        if (qt::IsA(c, "QLineEdit")) { qt::SetEditText(c, v.c_str()); qt::EmitEditingFinished(c); return true; }
        if (qt::IsA(c, "QDoubleSpinBox")) { qt::DoubleSpinSetValue(c, atof(v.c_str())); return true; }
        if (qt::IsA(c, "QSpinBox")) { qt::SpinSetValue(c, atoi(v.c_str())); return true; }
        if (qt::IsA(c, "QComboBox"))
        {
            int i = qt::ComboFindText(c, v.c_str(), 8 | 16);   // fixed string, case sensitive
            if (i < 0) i = qt::ComboFindText(c, v.c_str(), 8);
            if (i < 0)
            {
                std::string opts; int n = qt::ComboCount(c);
                for (int k = 0; k < n && k < 60; ++k) opts += (k ? " | " : "") + qt::ComboItemText(c, k);
                why = "'" + v + "' is not one of the combo's choices: " + opts;
                return false;
            }
            qt::ComboSetCurrentIndex(c, i); return true;
        }
        if (qt::IsA(c, "QCheckBox"))
        {
            std::string l = v; for (auto& ch : l) ch = char(tolower(static_cast<unsigned char>(ch)));
            bool want = l == "1" || l == "true" || l == "yes" || l == "on";
            if (qt::IsChecked(c) != want) qt::Click(c);
            return true;
        }
        if (qt::IsA(c, "QPlainTextEdit")) { qt::PlainSetTextEmit(c, v.c_str()); return true; }
        why = "unsupported control"; return false;
    }
    static void* CurrentGrid()
    {
        LookupWidgets();
        void* page = g_tabs ? qt::TabCurrentWidget(g_tabs) : nullptr;
        return page ? FindByClass(page, "apeAssetWidget") : nullptr;
    }

    std::vector<Prop> Properties()
    {
        std::vector<Prop> out;
        void* grid = CurrentGrid();
        if (!grid) return out;
        std::vector<void*> kids = qt::Children(grid);
        std::string category;
        for (size_t i = 0; i < kids.size(); ++i)
        {
            void* c = kids[i];
            const char* cls = qt::ClassName(c);
            if (!cls || !qt::IsA(c, "QWidget")) continue;
            if (strcmp(cls, "apeGroupWidget") == 0)
            {
                // the header text is in the group or the tool button next to it
                category.clear();
                std::vector<void*> cands = { c }; if (i + 1 < kids.size()) cands.push_back(kids[i + 1]);
                for (void* cand : cands)
                {
                    if (!category.empty()) break;
                    std::vector<void*> stack = { cand };
                    for (size_t k = 0; k < stack.size() && k < 64 && category.empty(); ++k)
                    {
                        void* o = stack[k];
                        if (qt::IsA(o, "QAbstractButton")) category = qt::ButtonText(o);
                        else if (qt::IsA(o, "QLabel")) category = qt::LabelText(o);
                        if (category.empty() && qt::IsA(o, "QWidget")) category = qt::ToolTip(o);
                        for (void* gc : qt::Children(o)) stack.push_back(gc);
                    }
                }
                continue;
            }
            if (strncmp(cls, "ape", 3) != 0) continue;
            Prop p; p.widget = c; p.kind = cls; p.category = category;
            if (i + 1 < kids.size() && qt::IsA(kids[i + 1], "QLabel")) { p.title = qt::LabelText(kids[i + 1]); p.tooltip = qt::ToolTip(kids[i + 1]); }
            if (qt::IsA(c, "QAbstractButton")) { std::string t = qt::ButtonText(c); if (!t.empty()) p.title = t; }   // check boxes carry their title
            if (p.tooltip.empty()) p.tooltip = qt::ToolTip(c);
            p.visible = qt::IsVisible(c); p.enabled = qt::IsEnabled(c);
            if (IsControl(c)) p.parts.push_back(c); else CollectParts(c, p.parts, 0);
            for (size_t j = 0; j < p.parts.size(); ++j) p.value += (j ? " " : "") + PartValue(p.parts[j]);
            if (p.parts.empty() && qt::IsA(c, "QLabel")) p.value = qt::LabelText(c);   // apeLabelWidget: a read-only value shown as a label
            out.push_back(p);
        }
        return out;
    }

    bool SetProperty(const std::string& titleOrIndex, const std::string& value, std::string& why)
    {
        std::vector<Prop> props = Properties();
        if (props.empty()) { why = "no asset tab is current"; return false; }
        const Prop* hit = nullptr;
        if (!titleOrIndex.empty() && isdigit(static_cast<unsigned char>(titleOrIndex[0])))
        {
            size_t i = size_t(atoi(titleOrIndex.c_str()));
            if (i < props.size()) hit = &props[i];
        }
        if (!hit)
        {
            std::string want = titleOrIndex; for (auto& ch : want) ch = char(tolower(static_cast<unsigned char>(ch)));
            for (const Prop& p : props) { std::string t = p.title; for (auto& ch : t) ch = char(tolower(static_cast<unsigned char>(ch))); if (t == want) { hit = &p; break; } }
            if (!hit) for (const Prop& p : props) { std::string t = p.category + "/" + p.title; for (auto& ch : t) ch = char(tolower(static_cast<unsigned char>(ch))); if (t == want) { hit = &p; break; } }
        }
        if (!hit) { why = "no property titled '" + titleOrIndex + "'"; return false; }
        if (hit->parts.empty()) { why = "'" + hit->title + "' (" + hit->kind + ") has no editable control"; return false; }
        if (hit->parts.size() == 1) return SetPart(hit->parts[0], value, why);
        // a multi-part entry (vector, button group): one whitespace-separated value per control
        std::vector<std::string> vals; size_t st = 0;
        while (st < value.size()) { size_t sp = value.find_first_of(" \t", st); vals.push_back(value.substr(st, sp == std::string::npos ? std::string::npos : sp - st)); if (sp == std::string::npos) break; st = value.find_first_not_of(" \t", sp); if (st == std::string::npos) break; }
        if (vals.size() != hit->parts.size()) { why = "'" + hit->title + "' has " + std::to_string(hit->parts.size()) + " components; give that many values separated by spaces"; return false; }
        for (size_t j = 0; j < vals.size(); ++j) if (!SetPart(hit->parts[j], vals[j], why)) return false;
        return true;
    }

    bool OpenAsset(const std::string& name, const std::string& type, std::string& why)
    {
        LookupWidgets();
        void* page = g_tabs ? qt::TabCurrentWidget(g_tabs) : nullptr;
        void* edit = page ? FindByClass(page, "apeAssetLineEditWidget") : nullptr;
        if (!edit || !qt::IsA(edit, "QLineEdit")) { why = "the current tab has no asset line edit (open any asset tab first)"; return false; }
        std::string link = name + "." + type;
        qt::SetEditText(edit, link.c_str());
        qt::EmitReturnPressed(edit);
        qt::EmitEditingFinished(edit);
        return true;
    }

    std::string OutputText()
    {
        LookupWidgets();
        if (!g_output) return std::string();
        return g_outputIsPlain ? qt::PlainText(g_output) : qt::TextEditText(g_output);
    }

    std::vector<Asset> SelectedAssets()
    {
        std::vector<Asset> out;
        LookupWidgets();
        if (!g_tree) return out;
        for (void* item : qt::TreeSelectedItems(g_tree))
        {
            if (!qt::TreeItemParent(item)) continue;        // a GDT row
            Asset a; a.name = qt::TreeItemText(item, 0); a.type = qt::TreeItemText(item, 1);
            if (a.type.empty()) { std::vector<std::string> t = TypesOf(a.name); if (!t.empty()) a.type = t[0]; }
            out.push_back(a);
        }
        return out;
    }

    void Print(const std::string& line)
    {
        { std::lock_guard<std::mutex> lock(g_printMutex); g_printQueue.push_back(line); }
        if (mt::IsMainThread()) FlushPrints();
    }

    std::wstring GdtDbPath()
    {
        wchar_t env[MAX_PATH * 2] = {};
        std::wstring root;
        if (GetEnvironmentVariableW(L"TA_TOOLS_PATH", env, MAX_PATH * 2) && env[0]) root = env;
        else root = plugins::RadiantRoot();
        while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
        return root + L"\\gdtdb\\gdt.db";
    }

    static void Dump(void* obj, int depth, int maxDepth, std::string& out, int& count)
    {
        if (!obj || depth > maxDepth || count > 4000) return;
        for (void* c : qt::Children(obj))
        {
            ++count;
            out += std::string(size_t(depth) * 2, ' ') + qt::ClassName(c);
            std::string n = qt::ObjectName(c); if (!n.empty()) out += " '" + n + "'";
            if (qt::IsA(c, "QTabWidget")) out += " tabs=" + std::to_string(qt::TabCount(c)) + " current='" + (qt::TabCount(c) ? qt::TabText(c, qt::TabCurrent(c)) : "") + "'";
            out += "\n";
            Dump(c, depth + 1, maxDepth, out, count);
        }
    }

    std::string DumpWidgets(int maxDepth)
    {
        void* mw = MainWindowWidget();
        if (!mw) return "no main window";
        std::string out = std::string(qt::ClassName(mw)) + " '" + qt::ObjectName(mw) + "'\n";
        int count = 0;
        Dump(mw, 1, maxDepth, out, count);
        return out;
    }

    std::string Command(const std::string& cmd)
    {
        if (cmd == "widgets" || cmd.rfind("widgets ", 0) == 0)
        {
            int depth = cmd.size() > 8 ? atoi(cmd.c_str() + 8) : 6;
            std::string d = DumpWidgets(depth > 0 ? depth : 6);
            LOG("ape: widget tree:\n%s", d.c_str());
            return d;
        }
        if (cmd == "current") { Asset a; return CurrentAsset(a) ? a.name + " (" + a.type + ")" : "no current asset"; }
        if (cmd == "tabs")
        {
            std::string out;
            for (int i = 0; i < TabCount(); ++i) { Asset a; out += std::to_string(i) + ": '" + (g_tabs ? qt::TabText(g_tabs, i) : "") + "'" + (TabAsset(i, a) ? " -> " + a.name + " (" + a.type + ")" : "") + "\n"; }
            return out.empty() ? "no tabs" : out;
        }
        if (cmd == "selected")
        {
            std::string out;
            for (const Asset& a : SelectedAssets()) out += a.name + " (" + a.type + ")\n";
            if (g_tree) for (void* item : qt::TreeSelectedItems(g_tree)) out += "  raw: '" + qt::TreeItemText(item, 0) + "' | '" + qt::TreeItemText(item, 1) + "' | '" + qt::TreeItemText(item, 2) + "' tip '" + qt::TreeItemToolTip(item, 0) + "' parent=" + (qt::TreeItemParent(item) ? "yes" : "no") + "\n";
            return out.empty() ? "nothing selected" : out;
        }
        if (cmd.rfind("print ", 0) == 0) { Print(cmd.substr(6)); return "printed"; }
        if (cmd == "props" || cmd == "props all")
        {
            std::string out; size_t i = 0;
            for (const Prop& p : Properties())
            {
                if (cmd == "props" && !p.visible) { ++i; continue; }
                std::string v = p.value; if (v.size() > 80) v = v.substr(0, 80) + "...";
                for (auto& ch : v) if (ch == '\n' || ch == '\t') ch = ' ';
                out += std::to_string(i++) + "  [" + p.category + "] " + p.title + "  <" + p.kind + " x" + std::to_string(p.parts.size()) + (p.visible ? "" : " hidden") + (p.enabled ? "" : " disabled") + ">  = '" + v + "'\n";
            }
            return out.empty() ? "no property grid (no asset tab current?)" : out;
        }
        if (cmd == "propnames")
        {
            std::string out; int n = 0;
            for (const Prop& p : Properties())
            {
                if (n++ > 6) break;
                out += p.title + " (" + p.kind + ") objectName='" + qt::ObjectName(p.widget) + "' props:";
                for (const std::string& nm : qt::DynamicPropertyNames(p.widget)) out += " " + nm + "='" + qt::ObjectProperty(p.widget, nm.c_str()) + "'";
                for (const char* guess : { "ID", "id", "entry", "key", "name", "Title" }) { std::string v = qt::ObjectProperty(p.widget, guess); if (!v.empty()) out += " " + std::string(guess) + "?='" + v + "'"; }
                out += "\n";
            }
            return out.empty() ? "no properties" : out;
        }
        if (cmd.rfind("set ", 0) == 0)
        {
            std::string rest = cmd.substr(4); size_t eq = rest.find('=');
            if (eq == std::string::npos) return "usage: ape set <title|index>=<value>";
            std::string why;
            return SetProperty(rest.substr(0, eq), rest.substr(eq + 1), why) ? "set " + rest.substr(0, eq) : "failed: " + why;
        }
        if (cmd.rfind("open ", 0) == 0)
        {
            std::string link = cmd.substr(5); size_t dot = link.rfind('.');
            if (dot == std::string::npos) return "usage: ape open <name>.<type>";
            std::string why;
            return OpenAsset(link.substr(0, dot), link.substr(dot + 1), why) ? "opening " + link : "failed: " + why;
        }
        if (cmd == "itemdata")
        {
            LookupWidgets();
            void* item = g_tree ? qt::TreeCurrentItem(g_tree) : nullptr;
            if (!item) return "no current tree item";
            std::string out = "item '" + qt::TreeItemText(item, 0) + "'\n";
            for (int role : { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0x100, 0x101, 0x102, 0x103, 0x104, 0x105 })
                out += "  role " + std::to_string(role) + " = " + qt::TreeItemData(item, 0, role, true) + "\n";
            return out;
        }
        if (cmd.rfind("types ", 0) == 0)
        {
            std::string out;
            for (const std::string& t : TypesOf(cmd.substr(6))) out += t + "\n";
            return out.empty() ? "no asset of that name in gdt.db" : out;
        }
        return "";
    }
}
