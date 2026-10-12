// APEPlus: performance fixes and improvements to APE (the Asset Property Editor) itself. APE only.
// Diagnostics: a typing harness (posted key messages), a main-thread busy watch and a call stack sampler.
// Commands ("APEPlus <cmd>"): edits, type <n> <text>, profile <ms>, stall.

// MAP (grep the names, line numbers drift):
//   Qt 5.3 exports, no headers  Qt (the function table), BindQt, QStringToUtf8, TakeString, ListRelease, MetaOf,
//                               IsA, DockOf, LooksLikeCount, Children
//   Property grid controls .... EditRef, CurrentGrid, CollectEdits, Edits, g_lastEdits
//   Asset tree report ......... FindTree, CountItems, RawItem, TreeReport
//   Typing harness ............ Typing, PostKeys (posted WM_KEYDOWN/WM_KEYUP), g_typeResult
//   Main-thread sampler ....... Sampler (g_sampler, its own std::thread), ReadQword, Unwind, Symbolize,
//                               SamplerMain, StartSampler
//   namespace filt ............ Feature 1, the fast asset-tree filter (detour of assetTreeUpdateFilters)
//     grammar ................. Term, Group, Expr, Lower, QStringToW, Glob, TermMatch, ExprMatch, Parse
//     value filter ............ VPat, VTerm, VGroup, VExpr, ParseValue, SchemaView, ValueReady (g_vctx), View, SlotValue,
//                               Pairs, TermExact, TermWild, ValueMatch, g_fastValue
//     APE's filter widgets .... Ui (g_ui), FindUi, LoadTypeNames (g_typeNames)
//     tree cache .............. Node, Gdt, Cache (g_cache), AddNodes, CacheValid, BuildCache, ShowOurGdtRows
//     the run ................. Run, RunGuarded, FromTyping (debounce), hk_UpdateFilters (g_orig),
//                               Tick (g_pendingTree), SetLabel, Stats (g_stats), g_fast, g_debounceMs, g_disabled
//     install / status ........ Install, Uninstall, Audit, Status
//   namespace commit .......... Feature 2, the directory rescan after a committed value (detour of kRefresh)
//     detours ................. hk_Refresh (g_orig, g_minS, g_runs, g_skipped), hk_FindFirstFileExA (g_origFind,
//                               g_trace, g_traceDirs under g_traceMutex)
//     change watcher .......... WatchMain (own std::thread g_watchThread, ReadDirectoryChangesW), Ignored,
//                               StartWatcher, StopWatcher, g_changed, g_changeCount, g_watchRoot, g_ignore
//     install / status ........ Install, InstallTrace, Status, TraceReport
//   namespace theme ........... Feature 3, the theme picker in View > Preferences (.qss sheets, plugins\APEPlus\themes)
//     sheet loading ........... Entry (g_list), Widen, Narrow, ReadWhole, StyleDirective, Resolve, Scan
//     apply ................... SetStyle, SetSheet, HeaderColor, LinkColorFor, RefreshLinkLabels, ApplyLinkColor (g_link, the
//                               palette's Link role), FlagTheme, Find, SyncFlag (g_flag), Apply (g_current, g_applied,
//                               g_lastError), Tick (startup + g_pending)
//     preferences dialog ...... hk_PrefsCtor (detour, g_origCtor), Patch, PatchGuarded, OnPrefsOk, OnOpenFolder,
//                               Dialog (g_dlg), Install, SelfTest (hidden dialog + PNG), List, Status
//   namespace preview ......... Feature 5, the preview frame cap: hk_Frame (records the frame start), hk_SingleShot (detour of
//                               QTimer::singleShot, only the frame function's re-queue), g_maxFps, startup selection fix:
//                               HookGfxInitDone (runtime moc dispatcher detour, hk_Metacall), Tick (re-emits
//                               itemSelectionChanged once), Install, Status
//   namespace sky ............. Feature 7, Custom Sky in the preview's Rendering menu: hk_ReadSsi (detour of the ssi record
//                               read: the custom sky's own record for every state), LoadSsiMap, SsiOf (g_ssiOf), ListSkies,
//                               Reapply (APE's lighting reload + DrawSkybox()), Store, Activate, Deactivate, picker window
//                               (FillList, OnListRow, ShowPicker), menu (BindFns, ResolveIds, DecorateMenu, OnSlot via
//                               preview::g_slotHook), Tick, Install, Uninstall, Status
//   namespace convert ......... Feature 5, first-time model conversion: hk_BuildSurfaces (face indices bucketed per material x LOD,
//                               Ctx t_ctx), hk_BuildOne (the face vector pointed at one bucket's copy for the call), Install, Status,
//                               "convert lods [click <n>]" (test: the Number of LODs buttons), the vertex cache reorder in vcache.cpp
//   namespace autolod ......... an Output note when a preview auto LOD is past the stock linker's limits: hk_Reduce (g_told), Install;
//                               meshoptimizer preview LODs while Launcher+ uses them: Meshopt, Key, hk_Cache*, InstallMeshopt
//   namespace formats ......... Feature 6, extra image source formats (JPEG): hk_EndsWith (the converter's ".png" test, caller
//                               checked), hk_QsEq (the texture widget's "png" compare), PatchPicker (filter string in .rdata),
//                               Configure ([images] extra_formats), Install, Uninstall, Status
//   namespace fastopen ........ Feature 8, opening an asset: hk_Popup (APE's forced QCompleter::popup returns null, caller
//                               checked), hk_SetPopup (uniform item sizes when Qt makes that popup later), hk_Dtor (g_deferred),
//                               hk_SetWidget (the grid swap: queued PolishRequests dropped first), hk_Reset/hk_Clear/hk_DeleteLater
//                               (a dying grid's descendants skip deleteLater), hk_EnsurePolished (hidden widgets in the swap),
//                               thumbnails: hk_LoadImage, hk_Scaled (g_imgs, g_small, Trim, DropAll, Tick), InstallThumbs, CallsTo, IsFunction, InstallClear, InstallSwap,
//                               Install, Status, Count ("open count": posted events by type)
//   namespace dxvk ............ Feature 4, the DXVK (Vulkan) switch shared with RadiantPlus (the two DLLs in bin): SourceDir,
//                               IsDxvkFile, Live, InBin, Status, Set, Restart, Tick (g_pending from the Preferences checkbox)
//   namespace awi ............. Feature 9, the plugin's own .awi scripts ([deffiles] override): a QAbstractFileEngineHandler
//                               (H_Create, Lookup) answers <root>\deffiles\<file> with plugins\APEPlus\deffiles\<file>,
//                               hk_ShellExecuteA (Edit AWI opens the file APE uses, g_origShell, g_edits), Files, Install,
//                               Uninstall, Status, Command ("awi [on|off|path <file>]")
//   namespace iotrace ......... diagnostics: file calls of a click (KernelBase detours), Start, Tick (report to iotrace.txt)
//   Plugin callbacks .......... OnLoad, OnUnload, OnTick, OnCommand (command channel: edits, type, profile,
//                               stall, ...), ModToolsPluginEntry

#define MODTOOLS_PLUGIN_IMPL
#include "modtools_plugin.hpp"
#include "themeicons.h"
#include "themeeditor.h"
#include "actionlog.h"
#include "colorpicker.h"
#include "filesearch.h"
#include "notesounds.h"
#include "previewfix.h"
#include "weaponpreview.h"
#include "discordrpc.h"
#include "meshlod.h"
#include "vcache.h"
#include <windows.h>
#include <dbghelp.h>
#include <mmsystem.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <thread>
#include <mutex>
#include <map>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#pragma comment(lib, "version.lib")

namespace
{
    // ---- Qt 5.3 exports (Qt5Widgets64r / Qt5Core64r), no headers: MS x64 ABI, QString/QList returned through a hidden pointer ----
    struct QListDataD { int ref; int alloc; int begin; int end; void* array[1]; };
    struct QListRaw { QListDataD* d; };
    struct QArrayDataRaw { int ref; int size; unsigned alloc; ptrdiff_t offset; };
    struct QMetaObjectRaw;

    struct Qt
    {
        bool bound = false;
        QListRaw* (*allWidgets)(QListRaw*) = nullptr;
        void* (*focusWidget)() = nullptr;
        void (*setFocus)(void*, int) = nullptr;
        const char* (*className)(const QMetaObjectRaw*) = nullptr;
        const QMetaObjectRaw* (*superClass)(const QMetaObjectRaw*) = nullptr;
        void* (*children)(const void*) = nullptr;
        void** (*lineEditText)(const void*, void**) = nullptr;
        void** (*toolTip)(const void*, void**) = nullptr;
        void** (*objectName)(const void*, void**) = nullptr;
        bool (*isVisible)(const void*) = nullptr;
        bool (*isEnabled)(const void*) = nullptr;
        void* (*window)(const void*) = nullptr;
        uintptr_t (*effectiveWinId)(const void*) = nullptr;
        void (*dispose)(QListDataD*) = nullptr;
        void (*qsDtor)(void**) = nullptr;
        // tree widget
        int (*topLevelItemCount)(const void*) = nullptr;
        void* (*topLevelItem)(const void*, int) = nullptr;
        int (*childCount)(const void*) = nullptr;
        void* (*child)(const void*, int) = nullptr;
        void** (*itemText)(const void*, void**, int) = nullptr;
        int (*itemColumnCount)(const void*) = nullptr;
        void (*itemSetHidden)(void*, bool) = nullptr;
        bool (*itemIsHidden)(const void*) = nullptr;
        void* (*itemParent)(const void*) = nullptr;
        void (*setUpdatesEnabled)(void*, bool) = nullptr;
        bool (*blockSignals)(void*, bool) = nullptr;
        bool (*buttonChecked)(const void*) = nullptr;
        bool (*actionChecked)(const void*) = nullptr;
        void (*labelSetText)(void*, const void*) = nullptr;
        void** (*labelText)(const void*, void**) = nullptr;
        void** (*actionText)(const void*, void**) = nullptr;
        QListRaw* (*widgetActions)(const void*, QListRaw*) = nullptr;
        void* (*qstringCtor)(void*, const wchar_t*, int) = nullptr;   // QString(const QChar*, int)
        void (*lineEditSetText)(void*, const void*) = nullptr;
        void* (*widgetFocusWidget)(const void*) = nullptr;   // QWidget::focusWidget: the focus child of a window, active or not
        void (*lineEditTextEdited)(void*, const void*) = nullptr;      // the signal itself (moc bodies are exported)
        // themes
        void* (*appInstance)() = nullptr;
        void* (*appSetStyle)(const void*) = nullptr;                    // QApplication::setStyle(const QString&) -> QStyle*
        void (*appSetStyleSheet)(void*, const void*) = nullptr;
        void* (*qstringFromUtf8)(void*, const char*, int) = nullptr;
        // the Preferences theme picker
        void* (*opNew)(size_t) = nullptr;                               // msvcr110 operator new (Qt and APE delete with it)
        void* (*comboCtor)(void*, void*) = nullptr;
        void (*comboAddItem)(void*, const void*, const void*) = nullptr; // addItem(const QString&, const QVariant&)
        void (*comboSetCurrentIndex)(void*, int) = nullptr;
        void (*comboClear)(void*) = nullptr;                            // optional: the Theme editor's Save As refills the combo
        int (*comboCurrentIndex)(const void*) = nullptr;
        int (*comboCount)(const void*) = nullptr;
        void** (*comboItemText)(const void*, void**, int) = nullptr;
        void* (*variantCtor)(void*) = nullptr;
        void (*variantDtor)(void*) = nullptr;
        void** (*buttonText)(const void*, void**) = nullptr;
        void (*buttonSetChecked)(void*, bool) = nullptr;
        void* (*parentWidget)(const void*) = nullptr;
        void* (*widgetLayout)(const void*) = nullptr;
        int (*layoutIndexOf)(const void*, void*) = nullptr;
        void (*boxInsertLayout)(void*, int, void*, int) = nullptr;
        void* (*buttonBoxButton)(const void*, int) = nullptr;
        bool (*isVisibleTo)(const void*, const void*) = nullptr;
        void (*deleteLater)(void*) = nullptr;
        // self test only (optional)
        void (*adjustSize)(void*) = nullptr;
        void* (*grab)(void*, void*, const int*) = nullptr;               // QPixmap grab(const QRect&), hidden return pointer
        bool (*pixmapSave)(const void*, const void*, const char*, int) = nullptr;
        void (*pixmapDtor)(void*) = nullptr;
        // the value filter (optional: without them APE's engine keeps the value filter)
        void* (*qstringFromLatin1)(void*, const char*, int) = nullptr;
        void* (*rxCtor)(void*, const void*, int, int) = nullptr;         // QRegExp(const QString&, Qt::CaseSensitivity, PatternSyntax)
        bool (*rxExactMatch)(const void*, const void*) = nullptr;
        void (*rxDtor)(void*) = nullptr;
        const void* (*mapNextNode)(const void*) = nullptr;               // QMapNodeBase::nextNode
        // test harness: select a tree item the way a click does
        void (*timerSingleShot)(int, const void*, const char*) = nullptr;   // QTimer::singleShot(int, const QObject*, const char* member)
        void (*treeItemSelectionChanged)(void*) = nullptr;                   // QTreeWidget::itemSelectionChanged (the signal: calling it emits)
        int (*metaIndexOfSlot)(const QMetaObjectRaw*, const char*) = nullptr;
        int (*metaMethodOffset)(const QMetaObjectRaw*) = nullptr;
        void (*treeSetCurrentItem)(void*, void*) = nullptr;
        void (*treeScrollToItem)(void*, const void*, int) = nullptr;
        // theme link colour (optional): QPalette::Link, which no sheet can set
        void (*guiPalette)(void*) = nullptr;                                // QGuiApplication::palette(), hidden return pointer (QPalette, 16 bytes)
        void (*appSetPalette)(const void*, const char*) = nullptr;          // QApplication::setPalette(const QPalette&, const char* className)
        void (*paletteSetBrush)(void*, int, int, const void*) = nullptr;    // QPalette::setBrush(ColorGroup, ColorRole, const QBrush&)
        const void* (*paletteColor)(const void*, int, int) = nullptr;       // QPalette::color(ColorGroup, ColorRole) -> const QColor&
        void (*paletteDtor)(void*) = nullptr;
        void* (*brushCtorColor)(void*, const void*, int) = nullptr;         // QBrush(const QColor&, Qt::BrushStyle)
        void (*brushDtor)(void*) = nullptr;
        void* (*colorCtorRgb)(void*, int, int, int, int) = nullptr;         // QColor(int r, int g, int b, int a)
    } qt;

    uint64_t Now(); double Ms(uint64_t dt); bool ReadQword(uintptr_t addr, uintptr_t& out);   // defined further down
    template <class T> bool Get(T& fn, HMODULE m, const char* name) { fn = reinterpret_cast<T>(GetProcAddress(m, name)); return fn != nullptr; }

    bool BindQt()
    {
        if (qt.bound) return true;
        HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll"), c = GetModuleHandleW(L"Qt5Core64r.dll");
        if (!w || !c) return false;
        bool ok = true;
        ok &= Get(qt.allWidgets, w, "?allWidgets@QApplication@@SA?AV?$QList@PEAVQWidget@@@@XZ");
        ok &= Get(qt.focusWidget, w, "?focusWidget@QApplication@@SAPEAVQWidget@@XZ");
        ok &= Get(qt.setFocus, w, "?setFocus@QWidget@@QEAAXW4FocusReason@Qt@@@Z");
        ok &= Get(qt.className, c, "?className@QMetaObject@@QEBAPEBDXZ");
        ok &= Get(qt.superClass, c, "?superClass@QMetaObject@@QEBAPEBU1@XZ");
        ok &= Get(qt.children, c, "?children@QObject@@QEBAAEBV?$QList@PEAVQObject@@@@XZ");
        ok &= Get(qt.lineEditText, w, "?text@QLineEdit@@QEBA?AVQString@@XZ");
        ok &= Get(qt.toolTip, w, "?toolTip@QWidget@@QEBA?AVQString@@XZ");
        ok &= Get(qt.objectName, c, "?objectName@QObject@@QEBA?AVQString@@XZ");
        ok &= Get(qt.isVisible, w, "?isVisible@QWidget@@QEBA_NXZ");
        ok &= Get(qt.isEnabled, w, "?isEnabled@QWidget@@QEBA_NXZ");
        ok &= Get(qt.window, w, "?window@QWidget@@QEBAPEAV1@XZ");
        ok &= Get(qt.effectiveWinId, w, "?effectiveWinId@QWidget@@QEBA_KXZ");
        ok &= Get(qt.dispose, c, "?dispose@QListData@@SAXPEAUData@1@@Z");
        ok &= Get(qt.qsDtor, c, "??1QString@@QEAA@XZ");
        ok &= Get(qt.topLevelItemCount, w, "?topLevelItemCount@QTreeWidget@@QEBAHXZ");
        ok &= Get(qt.topLevelItem, w, "?topLevelItem@QTreeWidget@@QEBAPEAVQTreeWidgetItem@@H@Z");
        ok &= Get(qt.childCount, w, "?childCount@QTreeWidgetItem@@QEBAHXZ");
        ok &= Get(qt.child, w, "?child@QTreeWidgetItem@@QEBAPEAV1@H@Z");
        ok &= Get(qt.itemText, w, "?text@QTreeWidgetItem@@QEBA?AVQString@@H@Z");
        ok &= Get(qt.itemColumnCount, w, "?columnCount@QTreeWidgetItem@@QEBAHXZ");
        ok &= Get(qt.itemSetHidden, w, "?setHidden@QTreeWidgetItem@@QEAAX_N@Z");
        ok &= Get(qt.itemIsHidden, w, "?isHidden@QTreeWidgetItem@@QEBA_NXZ");
        ok &= Get(qt.itemParent, w, "?parent@QTreeWidgetItem@@QEBAPEAV1@XZ");
        ok &= Get(qt.setUpdatesEnabled, w, "?setUpdatesEnabled@QWidget@@QEAAX_N@Z");
        ok &= Get(qt.blockSignals, c, "?blockSignals@QObject@@QEAA_N_N@Z");
        ok &= Get(qt.buttonChecked, w, "?isChecked@QAbstractButton@@QEBA_NXZ");
        ok &= Get(qt.actionChecked, w, "?isChecked@QAction@@QEBA_NXZ");
        ok &= Get(qt.labelSetText, w, "?setText@QLabel@@QEAAXAEBVQString@@@Z");
        ok &= Get(qt.labelText, w, "?text@QLabel@@QEBA?AVQString@@XZ");
        ok &= Get(qt.actionText, w, "?text@QAction@@QEBA?AVQString@@XZ");
        ok &= Get(qt.widgetActions, w, "?actions@QWidget@@QEBA?AV?$QList@PEAVQAction@@@@XZ");
        ok &= Get(qt.qstringCtor, c, "??0QString@@QEAA@PEBVQChar@@H@Z");
        ok &= Get(qt.lineEditSetText, w, "?setText@QLineEdit@@QEAAXAEBVQString@@@Z");
        ok &= Get(qt.widgetFocusWidget, w, "?focusWidget@QWidget@@QEBAPEAV1@XZ");
        ok &= Get(qt.lineEditTextEdited, w, "?textEdited@QLineEdit@@QEAAXAEBVQString@@@Z");
        ok &= Get(qt.appInstance, c, "?instance@QCoreApplication@@SAPEAV1@XZ");
        ok &= Get(qt.appSetStyle, w, "?setStyle@QApplication@@SAPEAVQStyle@@AEBVQString@@@Z");
        ok &= Get(qt.appSetStyleSheet, w, "?setStyleSheet@QApplication@@QEAAXAEBVQString@@@Z");
        ok &= Get(qt.qstringFromUtf8, c, "?fromUtf8@QString@@SA?AV1@PEBDH@Z");
        if (HMODULE crt = GetModuleHandleW(L"msvcr110.dll")) ok &= Get(qt.opNew, crt, "??2@YAPEAX_K@Z"); else ok = false;
        ok &= Get(qt.comboCtor, w, "??0QComboBox@@QEAA@PEAVQWidget@@@Z");
        ok &= Get(qt.comboAddItem, w, "?addItem@QComboBox@@QEAAXAEBVQString@@AEBVQVariant@@@Z");
        ok &= Get(qt.comboSetCurrentIndex, w, "?setCurrentIndex@QComboBox@@QEAAXH@Z");
        Get(qt.comboClear, w, "?clear@QComboBox@@QEAAXXZ");
        ok &= Get(qt.comboCurrentIndex, w, "?currentIndex@QComboBox@@QEBAHXZ");
        ok &= Get(qt.comboCount, w, "?count@QComboBox@@QEBAHXZ");
        ok &= Get(qt.comboItemText, w, "?itemText@QComboBox@@QEBA?AVQString@@H@Z");
        ok &= Get(qt.variantCtor, c, "??0QVariant@@QEAA@XZ");
        ok &= Get(qt.variantDtor, c, "??1QVariant@@QEAA@XZ");
        ok &= Get(qt.buttonText, w, "?text@QAbstractButton@@QEBA?AVQString@@XZ");
        ok &= Get(qt.buttonSetChecked, w, "?setChecked@QAbstractButton@@QEAAX_N@Z");
        ok &= Get(qt.parentWidget, w, "?parentWidget@QWidget@@QEBAPEAV1@XZ");
        ok &= Get(qt.widgetLayout, w, "?layout@QWidget@@QEBAPEAVQLayout@@XZ");
        ok &= Get(qt.layoutIndexOf, w, "?indexOf@QLayout@@UEBAHPEAVQWidget@@@Z");
        ok &= Get(qt.boxInsertLayout, w, "?insertLayout@QBoxLayout@@QEAAXHPEAVQLayout@@H@Z");
        ok &= Get(qt.buttonBoxButton, w, "?button@QDialogButtonBox@@QEBAPEAVQPushButton@@W4StandardButton@1@@Z");
        ok &= Get(qt.isVisibleTo, w, "?isVisibleTo@QWidget@@QEBA_NPEBV1@@Z");
        ok &= Get(qt.deleteLater, c, "?deleteLater@QObject@@QEAAXXZ");
        Get(qt.adjustSize, w, "?adjustSize@QWidget@@QEAAXXZ");
        Get(qt.grab, w, "?grab@QWidget@@QEAA?AVQPixmap@@AEBVQRect@@@Z");
        if (HMODULE g = GetModuleHandleW(L"Qt5Gui64r.dll"))
        {
            Get(qt.pixmapSave, g, "?save@QPixmap@@QEBA_NAEBVQString@@PEBDH@Z"); Get(qt.pixmapDtor, g, "??1QPixmap@@UEAA@XZ");
            Get(qt.guiPalette, g, "?palette@QGuiApplication@@SA?AVQPalette@@XZ");
            Get(qt.paletteSetBrush, g, "?setBrush@QPalette@@QEAAXW4ColorGroup@1@W4ColorRole@1@AEBVQBrush@@@Z");
            Get(qt.paletteColor, g, "?color@QPalette@@QEBAAEBVQColor@@W4ColorGroup@1@W4ColorRole@1@@Z");
            Get(qt.paletteDtor, g, "??1QPalette@@QEAA@XZ");
            Get(qt.brushCtorColor, g, "??0QBrush@@QEAA@AEBVQColor@@W4BrushStyle@Qt@@@Z");
            Get(qt.brushDtor, g, "??1QBrush@@QEAA@XZ");
            Get(qt.colorCtorRgb, g, "??0QColor@@QEAA@HHHH@Z");
        }
        Get(qt.appSetPalette, w, "?setPalette@QApplication@@SAXAEBVQPalette@@PEBD@Z");
        Get(qt.qstringFromLatin1, c, "?fromLatin1@QString@@SA?AV1@PEBDH@Z");
        Get(qt.rxCtor, c, "??0QRegExp@@QEAA@AEBVQString@@W4CaseSensitivity@Qt@@W4PatternSyntax@0@@Z");
        Get(qt.rxExactMatch, c, "?exactMatch@QRegExp@@QEBA_NAEBVQString@@@Z");
        Get(qt.rxDtor, c, "??1QRegExp@@QEAA@XZ");
        Get(qt.mapNextNode, c, "?nextNode@QMapNodeBase@@QEBAPEBU1@XZ");
        Get(qt.timerSingleShot, c, "?singleShot@QTimer@@SAXHPEBVQObject@@PEBD@Z");
        Get(qt.treeItemSelectionChanged, w, "?itemSelectionChanged@QTreeWidget@@QEAAXXZ");
        Get(qt.metaIndexOfSlot, c, "?indexOfSlot@QMetaObject@@QEBAHPEBD@Z");
        Get(qt.metaMethodOffset, c, "?methodOffset@QMetaObject@@QEBAHXZ");
        Get(qt.treeSetCurrentItem, w, "?setCurrentItem@QTreeWidget@@QEAAXPEAVQTreeWidgetItem@@@Z");
        Get(qt.treeScrollToItem, w, "?scrollToItem@QTreeWidget@@QEAAXPEBVQTreeWidgetItem@@W4ScrollHint@QAbstractItemView@@@Z");
        if (!ok) rp::Log("qt: some exports are missing, widget walking disabled");
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
    void ListRelease(QListRaw& l)
    {
        if (!l.d || l.d->ref == -1) return;
        if (InterlockedDecrement(reinterpret_cast<volatile long*>(&l.d->ref)) == 0 && qt.dispose) qt.dispose(l.d);
        l.d = nullptr;
    }
    const QMetaObjectRaw* MetaOf(void* obj)
    {
        auto metaObject = (*reinterpret_cast<const QMetaObjectRaw* (__fastcall ***)(void*)>(obj))[0];   // QObject vtable slot 0
        return metaObject(obj);
    }
    const char* ClassName(void* obj) { const char* c = qt.className(MetaOf(obj)); return c ? c : ""; }
    bool IsA(void* obj, const char* cls)
    {
        for (const QMetaObjectRaw* m = MetaOf(obj); m; m = qt.superClass(m))
            if (const char* c = qt.className(m)) if (strcmp(c, cls) == 0) return true;
        return false;
    }
    void* ParentOf(void* obj) { char* d = *reinterpret_cast<char**>(static_cast<char*>(obj) + 8); return d ? *reinterpret_cast<void**>(d + 16) : nullptr; }   // QObjectData::parent
    std::string DockOf(void* obj)
    {
        for (void* p = obj; p; p = ParentOf(p)) if (IsA(p, "QDockWidget")) return TakeString(qt.objectName, p);
        return "";
    }
    bool LooksLikeCount(const std::string& t)
    {
        if (t.empty()) return false;
        for (char ch : t) if (!isdigit(static_cast<unsigned char>(ch)) && ch != ' ' && ch != '/') return false;
        return isdigit(static_cast<unsigned char>(t[0])) != 0;
    }
    std::vector<void*> Children(void* obj)
    {
        std::vector<void*> out;
        auto* d = *reinterpret_cast<QListDataD**>(qt.children(obj));
        if (!d) return out;
        for (int i = d->begin; i < d->end && i - d->begin < 100000; ++i) out.push_back(d->array[i]);
        return out;
    }
    template <class Fn> void ForEachWidget(Fn fn)
    {
        QListRaw widgets = {}; qt.allWidgets(&widgets);
        if (widgets.d)
            for (int i = widgets.d->begin; i < widgets.d->end; ++i)
                if (void* w = widgets.d->array[i]) if (fn(w)) break;
        ListRelease(widgets);
    }

    // ---- the property grid's text controls ----
    struct EditRef { void* edit; std::string kind; std::string tip; std::string text; bool visible; bool enabled; };

    void* CurrentGrid()
    {
        void* hit = nullptr;
        ForEachWidget([&](void* w) {
            if (strcmp(ClassName(w), "apeAssetWidget") == 0 && qt.isVisible(w)) { hit = w; return true; }
            return false;
        });
        return hit;
    }
    void CollectEdits(void* w, const std::string& kind, const std::string& tip, std::vector<EditRef>& out, int depth)
    {
        if (depth > 8) return;
        for (void* c : Children(w))
        {
            if (!IsA(c, "QWidget")) continue;
            const char* cls = ClassName(c);
            std::string k = kind, t = tip;
            if (strncmp(cls, "ape", 3) == 0) { k = cls; t = TakeString(qt.toolTip, c); }
            if (IsA(c, "QLineEdit"))
            {
                if (TakeString(qt.objectName, c) == "qt_spinbox_lineedit") continue;
                out.push_back({ c, k, t, TakeString(qt.lineEditText, c), qt.isVisible(c), qt.isEnabled(c) });
                continue;
            }
            CollectEdits(c, k, t, out, depth + 1);
        }
    }
    std::vector<EditRef> g_lastEdits;   // the last enumeration, so "type <n>" indexes what "edits" showed
    std::vector<EditRef> Edits(bool wholeApp)
    {
        std::vector<EditRef> out;
        if (!BindQt()) return out;
        if (!wholeApp) { if (void* grid = CurrentGrid()) CollectEdits(grid, "", "", out, 0); }
        else
        {
            // every visible line edit in the application (filter boxes, tab tool bars, dialogs), tagged with its class
            ForEachWidget([&](void* w) {
                if (!IsA(w, "QLineEdit") || !qt.isVisible(w)) return false;
                std::string name = TakeString(qt.objectName, w);
                if (name == "qt_spinbox_lineedit") return false;
                std::string kind = ClassName(w); if (!name.empty()) kind += " '" + name + "'";
                out.push_back({ w, kind, TakeString(qt.toolTip, w), TakeString(qt.lineEditText, w), true, qt.isEnabled(w) });
                return false;
            });
        }
        g_lastEdits = out;
        return out;
    }

    std::string ItemText(void* item, int col) { void* qs = nullptr; qt.itemText(item, &qs, col); std::string s = QStringToUtf8(&qs); qt.qsDtor(&qs); return s; }
    void* FindTree()
    {
        void* hit = nullptr;
        ForEachWidget([&](void* w) { if (strcmp(ClassName(w), "apeAssetTreeWidget") == 0) { hit = w; return true; } return false; });
        return hit;
    }
    int CountItems(void* item, int depth, int& maxDepth)
    {
        if (depth > maxDepth) maxDepth = depth;
        int n = qt.childCount(item), total = n;
        for (int i = 0; i < n; ++i) total += CountItems(qt.child(item, i), depth + 1, maxDepth);
        return total;
    }
    std::string RawItem(void* item)
    {
        char buf[512]; int n = 0;
        const uint64_t* q = static_cast<const uint64_t*>(item);
        n += snprintf(buf + n, sizeof buf - n, "raw+0x38..0x58:");
        for (int i = 7; i < 11; ++i) n += snprintf(buf + n, sizeof buf - n, " %llx", (unsigned long long)q[i]);
        // a pointer-looking field: show the int at +0x20 and bytes +0x24..+0x27 of what it points to (APE's entry record?)
        for (int i = 7; i < 11; ++i)
        {
            uintptr_t p = uintptr_t(q[i]); uintptr_t v = 0;
            if (p > 0x10000 && p < 0x7FFFFFFFFFFF && ReadQword(p + 0x20, v))
                n += snprintf(buf + n, sizeof buf - n, " [+0x%x -> +0x20 int %d bytes24..27 %02x %02x %02x %02x]", 0x38 + 8 * (i - 7), int(v & 0xffffffff), unsigned(v >> 32) & 0xff, unsigned(v >> 40) & 0xff, unsigned(v >> 48) & 0xff, unsigned(v >> 56) & 0xff);
        }
        return buf;
    }
    std::string TreeReport()
    {
        std::string out;
        void* tree = FindTree();
        if (!tree) return "no apeAssetTreeWidget";
        int tl = qt.topLevelItemCount(tree);
        uint64_t t0 = Now();
        int maxDepth = 0, total = 0;
        for (int i = 0; i < tl; ++i) total += 1 + CountItems(qt.topLevelItem(tree, i), 1, maxDepth);
        uint64_t t1 = Now();
        // a full text(0) pass
        size_t bytes = 0;
        std::vector<void*> stack;
        for (int i = 0; i < tl; ++i) stack.push_back(qt.topLevelItem(tree, i));
        while (!stack.empty()) { void* it = stack.back(); stack.pop_back(); bytes += ItemText(it, 0).size(); int n = qt.childCount(it); for (int k = 0; k < n; ++k) stack.push_back(qt.child(it, k)); }
        uint64_t t2 = Now();
        char buf[1024];
        snprintf(buf, sizeof buf, "tree %p: %d top-level items, %d items total, max depth %d; count pass %.1f ms, text(0) pass %.1f ms (%zu bytes)\n", tree, tl, total, maxDepth, Ms(t1 - t0), Ms(t2 - t1), bytes);
        out += buf;
        for (int i = 0; i < tl && i < 3; ++i)
        {
            void* g = qt.topLevelItem(tree, i);
            snprintf(buf, sizeof buf, "GDT %d %p '%s' cols=%d children=%d hidden=%d %s\n", i, g, ItemText(g, 0).c_str(), qt.itemColumnCount(g), qt.childCount(g), qt.itemIsHidden(g) ? 1 : 0, RawItem(g).c_str());
            out += buf;
            for (int k = 0; k < qt.childCount(g) && k < 3; ++k)
            {
                void* c = qt.child(g, k);
                snprintf(buf, sizeof buf, "   asset %p '%s' | '%s' | '%s' children=%d hidden=%d %s\n", c, ItemText(c, 0).c_str(), ItemText(c, 1).c_str(), ItemText(c, 2).c_str(), qt.childCount(c), qt.itemIsHidden(c) ? 1 : 0, RawItem(c).c_str());
                out += buf;
            }
        }
        // actions mentioning favorites
        ForEachWidget([&](void* w) {
            QListRaw acts = {}; qt.widgetActions(w, &acts);
            if (acts.d) for (int j = acts.d->begin; j < acts.d->end; ++j) { void* a = acts.d->array[j]; if (!a) continue; std::string t = TakeString(qt.actionText, a); if (t.find("avorite") != std::string::npos) { snprintf(buf, sizeof buf, "action %p '%s' checked=%d on %s\n", a, t.c_str(), qt.actionChecked(a) ? 1 : 0, ClassName(w)); out += buf; } }
            ListRelease(acts); return false; });
        return out;
    }

    // ---- typing: posted key messages, then watch the main thread ----
    struct Typing
    {
        bool active = false;
        void* edit = nullptr;
        std::string expect;       // the edit's text once every key was handled
        uint64_t start = 0;
        uint64_t lastTick = 0;
        uint64_t maxGap = 0;
        int      keys = 0;
    } g_typing;
    uint64_t Now() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return uint64_t(c.QuadPart); }
    double Ms(uint64_t dt) { static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }(); return 1000.0 * double(dt) / double(f.QuadPart); }

    // tick watchdog: the longest gap between two idle ticks in the last window
    uint64_t g_tickPrev = 0, g_tickMaxGap = 0, g_tickMaxAt = 0;
    std::string g_typeResult;

    bool PostKeys(HWND hwnd, const std::string& text, std::string& why)
    {
        for (size_t i = 0; i < text.size(); ++i)
        {
            char ch = text[i];
            SHORT vk = -1;
            if (ch == '{')
            {
                // special keys: {BS} {DEL} {END} {HOME} {ENTER} {TAB} {ESC} {LEFT} {RIGHT}
                size_t close = text.find('}', i);
                if (close == std::string::npos) { why = "unclosed {"; return false; }
                std::string name = text.substr(i + 1, close - i - 1); i = close;
                static const struct { const char* n; BYTE vk; } keys[] = { { "BS", VK_BACK }, { "DEL", VK_DELETE }, { "END", VK_END }, { "HOME", VK_HOME }, { "ENTER", VK_RETURN }, { "TAB", VK_TAB }, { "ESC", VK_ESCAPE }, { "LEFT", VK_LEFT }, { "RIGHT", VK_RIGHT } };
                for (const auto& k : keys) if (name == k.n) vk = k.vk;
                if (vk == -1) { why = "unknown key {" + name + "}"; return false; }
            }
            else vk = VkKeyScanA(ch);
            if (vk == -1) { why = std::string("no key for '") + ch + "'"; return false; }
            BYTE key = LOBYTE(vk); BYTE mods = HIBYTE(vk);
            UINT scan = MapVirtualKeyA(key, MAPVK_VK_TO_VSC);
            LPARAM down = LPARAM(1) | (LPARAM(scan) << 16), up = down | (LPARAM(1) << 30) | (LPARAM(1) << 31);
            if (mods & 1) PostMessageW(hwnd, WM_KEYDOWN, VK_SHIFT, LPARAM(1) | (LPARAM(MapVirtualKeyA(VK_SHIFT, MAPVK_VK_TO_VSC)) << 16));
            PostMessageW(hwnd, WM_KEYDOWN, key, down);
            PostMessageW(hwnd, WM_KEYUP, key, up);
            if (mods & 1) PostMessageW(hwnd, WM_KEYUP, VK_SHIFT, LPARAM(1) | (LPARAM(MapVirtualKeyA(VK_SHIFT, MAPVK_VK_TO_VSC)) << 16) | (LPARAM(1) << 30) | (LPARAM(1) << 31));
        }
        return true;
    }

    // ---- main-thread sampler ----
    struct Sampler
    {
        std::atomic<bool> running{ false };
        std::atomic<bool> done{ false };
        std::thread thread;
        DWORD mainTid = 0;
        int   ms = 0;
        int   intervalUs = 1000;
        // results
        std::unordered_map<uintptr_t, int> leaf, incl;
        std::vector<std::vector<uintptr_t>> stacks;   // every sample's frames (capped)
        std::vector<std::vector<uintptr_t>> wstacks;  // worker threads' frames (profile ... all)
        bool allThreads = false;
        int samples = 0, failed = 0, wsamples = 0;
        std::string report;
    } g_sampler;
    // APE's exit skips OnUnload, so a joinable thread at static destruction would std::terminate: detach it.
    // Declared after g_sampler, so it is destroyed first.
    struct SamplerExitGuard { ~SamplerExitGuard() { if (g_sampler.thread.joinable()) g_sampler.thread.detach(); } } g_samplerExitGuard;

    bool ReadQword(uintptr_t addr, uintptr_t& out)
    {
        __try { out = *reinterpret_cast<uintptr_t*>(addr); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    int Unwind(HANDLE thread, uintptr_t* frames, int cap)
    {
        CONTEXT ctx; memset(&ctx, 0, sizeof ctx); ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (!GetThreadContext(thread, &ctx)) return 0;
        int n = 0;
        for (int i = 0; i < cap; ++i)
        {
            if (!ctx.Rip) break;
            frames[n++] = uintptr_t(ctx.Rip);
            DWORD64 base = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
            if (!fn)
            {
                uintptr_t ret = 0;
                if (!ReadQword(uintptr_t(ctx.Rsp), ret)) break;   // leaf function: the return address sits at rsp
                ctx.Rip = ret; ctx.Rsp += 8;
            }
            else
            {
                void* handler = nullptr; DWORD64 est = 0;
                __try { RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &handler, &est, nullptr); }
                __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            }
        }
        return n;
    }

    std::string Symbolize(uintptr_t addr)
    {
        char buf[512];
        HMODULE m = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(addr), &m) && m)
        {
            wchar_t path[MAX_PATH] = {}; GetModuleFileNameW(m, path, MAX_PATH);
            const wchar_t* name = wcsrchr(path, L'\\'); name = name ? name + 1 : path;
            uintptr_t rva = addr - uintptr_t(m);
            // export names through dbghelp (no PDBs here, but exports resolve: Qt, sqlite, system DLLs)
            alignas(SYMBOL_INFO) char sbuf[sizeof(SYMBOL_INFO) + 256] = {};
            SYMBOL_INFO* si = reinterpret_cast<SYMBOL_INFO*>(sbuf); si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
            DWORD64 disp = 0;
            if (SymFromAddr(GetCurrentProcess(), addr, &disp, si) && si->NameLen)
                snprintf(buf, sizeof buf, "%S!%s+0x%llx", name, si->Name, (unsigned long long)disp);
            else
            {
                DWORD64 base = 0; PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(addr, &base, nullptr);   // collapse by function start (.pdata)
                if (fn) snprintf(buf, sizeof buf, "%S!fn_%llx+0x%llx", name, (unsigned long long)fn->BeginAddress, (unsigned long long)(rva - fn->BeginAddress));
                else snprintf(buf, sizeof buf, "%S+0x%llx", name, (unsigned long long)rva);
            }
            return buf;
        }
        snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)addr);
        return buf;
    }

    void SamplerMain()
    {
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_sampler.mainTid);
        if (!h) { g_sampler.report = "could not open the main thread"; g_sampler.done = true; return; }
        uintptr_t frames[64];
        timeBeginPeriod(1);
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);   // 1 ms pacing even when the process is in the background
        uint64_t end = Now() + uint64_t(double(g_sampler.ms) / 1000.0 * [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return double(f.QuadPart); }());
        std::vector<uintptr_t> seen; seen.reserve(64);
        std::unordered_map<DWORD, HANDLE> workers; uint64_t lastEnum = 0;
        DWORD selfTid = GetCurrentThreadId(), pid = GetCurrentProcessId();
        while (Now() < end && g_sampler.running)
        {
            if (g_sampler.allThreads)
            {
                if (!lastEnum || Ms(Now() - lastEnum) > 250.0)
                {
                    lastEnum = Now();
                    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                    if (snap != INVALID_HANDLE_VALUE)
                    {
                        THREADENTRY32 te; te.dwSize = sizeof te;
                        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
                            if (te.th32OwnerProcessID == pid && te.th32ThreadID != g_sampler.mainTid && te.th32ThreadID != selfTid && !workers.count(te.th32ThreadID))
                                if (HANDLE wh = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID)) workers[te.th32ThreadID] = wh;
                        CloseHandle(snap);
                    }
                }
                for (auto it = workers.begin(); it != workers.end();)
                {
                    DWORD code = 0;
                    if (!GetExitCodeThread(it->second, &code) || code != STILL_ACTIVE) { CloseHandle(it->second); it = workers.erase(it); continue; }
                    if (SuspendThread(it->second) != DWORD(-1))
                    {
                        int n = Unwind(it->second, frames, 64);
                        ResumeThread(it->second);
                        if (n > 0) { ++g_sampler.wsamples; if (g_sampler.wstacks.size() < 60000) g_sampler.wstacks.emplace_back(frames, frames + n); }
                    }
                    ++it;
                }
            }
            if (SuspendThread(h) != DWORD(-1))
            {
                int n = Unwind(h, frames, 64);
                ResumeThread(h);
                if (n > 0)
                {
                    ++g_sampler.samples;
                    g_sampler.leaf[frames[0]]++;
                    if (g_sampler.stacks.size() < 30000) g_sampler.stacks.emplace_back(frames, frames + n);
                    seen.clear();
                    for (int i = 0; i < n; ++i)
                    {
                        if (std::find(seen.begin(), seen.end(), frames[i]) != seen.end()) continue;   // recursion: count a frame once per sample
                        seen.push_back(frames[i]);
                        g_sampler.incl[frames[i]]++;
                    }
                }
                else ++g_sampler.failed;
            }
            if (timer) { LARGE_INTEGER due; due.QuadPart = -10000; SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE); WaitForSingleObject(timer, 5); }
            else Sleep(1);
        }
        CloseHandle(h);
        for (auto& kv : workers) CloseHandle(kv.second);
        if (timer) CloseHandle(timer);
        timeEndPeriod(1);
        // symbolize the top of each histogram (main thread runs again, dbghelp is safe now)
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_NO_PROMPTS | SYMOPT_FAIL_CRITICAL_ERRORS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);   // cleaned up below: dbghelp would keep our own PDB open (locks the next build)
        // collapse by function start where dbghelp knows the symbol, else by exact address, report both lists
        auto top = [&](const std::unordered_map<uintptr_t, int>& hist, const char* title, int count) {
            std::map<std::string, int> byName;
            for (const auto& kv : hist) byName[Symbolize(kv.first)] += kv.second;
            std::vector<std::pair<std::string, int>> v(byName.begin(), byName.end());
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            std::string s = std::string(title) + " (" + std::to_string(g_sampler.samples) + " samples, " + std::to_string(g_sampler.failed) + " failed):\n";
            char line[640];
            for (size_t i = 0; i < v.size() && int(i) < count; ++i)
            {
                snprintf(line, sizeof line, "  %5.1f%%  %s\n", 100.0 * v[i].second / std::max(1, g_sampler.samples), v[i].first.c_str());
                s += line;
            }
            return s;
        };
        // busy samples only: the main thread was not sitting in the message wait
        std::unordered_map<uintptr_t, int> busyIncl, busyLeaf; int busy = 0;
        std::string waitName = "win32u.dll!NtUserMsgWaitForMultipleObjectsEx";
        for (const auto& st : g_sampler.stacks)
        {
            if (st.empty()) continue;
            std::string leafName = Symbolize(st[0]);
            if (leafName.rfind(waitName, 0) == 0) continue;
            ++busy; busyLeaf[st[0]]++;
            seen.clear();
            for (uintptr_t f : st) { if (std::find(seen.begin(), seen.end(), f) != seen.end()) continue; seen.push_back(f); busyIncl[f]++; }
        }
        auto topOf = [&](const std::unordered_map<uintptr_t, int>& hist, const char* title, int count, int denom, bool exeOnly) {
            std::map<std::string, int> byName;
            for (const auto& kv : hist) { std::string n = Symbolize(kv.first); if (exeOnly && _strnicmp(n.c_str(), "asseteditor_modtools.exe", 24) != 0) continue; byName[n] += kv.second; }
            std::vector<std::pair<std::string, int>> v(byName.begin(), byName.end());
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            std::string s = std::string(title) + " (" + std::to_string(denom) + " samples):\n";
            char line[640];
            for (size_t i = 0; i < v.size() && int(i) < count; ++i) { snprintf(line, sizeof line, "  %5.1f%%  %s\n", 100.0 * v[i].second / std::max(1, denom), v[i].first.c_str()); s += line; }
            return s;
        };
        // worker threads: only the samples where the thread was not parked in a wait
        std::unordered_map<uintptr_t, int> wIncl, wLeaf; int wbusy = 0;
        auto idleLeaf = [](const std::string& n) { return n.find("Wait") != std::string::npos || n.find("NtDelayExecution") != std::string::npos || n.find("GetMessage") != std::string::npos || n.find("RemoveIoCompletion") != std::string::npos || n.find("NtUserGetQueueStatus") != std::string::npos; };
        for (const auto& st : g_sampler.wstacks)
        {
            if (st.empty() || idleLeaf(Symbolize(st[0]))) continue;
            ++wbusy; wLeaf[st[0]]++;
            seen.clear();
            for (uintptr_t f : st) { if (std::find(seen.begin(), seen.end(), f) != seen.end()) continue; seen.push_back(f); wIncl[f]++; }
        }
        g_sampler.report = top(g_sampler.leaf, "leaf frames", 12)
            + topOf(busyLeaf, "BUSY leaf frames", 25, busy, false)
            + topOf(busyIncl, "BUSY inclusive frames", 45, busy, false)
            + topOf(busyIncl, "BUSY inclusive frames in APE itself", 40, busy, true);
        if (g_sampler.allThreads)
            g_sampler.report += "\nWORKER THREADS: " + std::to_string(g_sampler.wsamples) + " samples, " + std::to_string(wbusy) + " busy\n"
                + topOf(wLeaf, "WORKER busy leaf frames", 30, wbusy, false)
                + topOf(wIncl, "WORKER busy inclusive frames", 60, wbusy, false)
                + topOf(wIncl, "WORKER busy inclusive frames in APE itself", 40, wbusy, true);
        // busy main-thread stacks, root first, one folded line per distinct stack ( reads it)
        {
            std::unordered_map<uintptr_t, std::string> names;
            std::map<std::string, int> folded;
            for (const auto& st : g_sampler.stacks)
            {
                if (st.empty()) continue;
                auto name = [&](uintptr_t a) -> const std::string& { auto it = names.find(a); if (it == names.end()) it = names.emplace(a, Symbolize(a)).first; return it->second; };
                if (name(st[0]).rfind(waitName, 0) == 0) continue;
                std::string line;
                for (size_t i = st.size(); i-- > 0;) { if (!line.empty()) line += ';'; line += name(st[i]); }
                folded[line]++;
            }
            std::wstring fpath = rp::PluginDir() + L"\\profile.folded";
            if (FILE* f = _wfopen(fpath.c_str(), L"wb")) { for (const auto& kv : folded) fprintf(f, "%s %d\n", kv.first.c_str(), kv.second); fclose(f); }
        }
        g_sampler.wstacks.clear(); g_sampler.wstacks.shrink_to_fit();
        g_sampler.stacks.clear(); g_sampler.stacks.shrink_to_fit();
        SymCleanup(GetCurrentProcess());
        // the full report goes to profile.txt next to the plugin (the log truncates long lines)
        std::wstring path = rp::PluginDir() + L"\\profile.txt";
        if (FILE* f = _wfopen(path.c_str(), L"wb")) { fwrite(g_sampler.report.data(), 1, g_sampler.report.size(), f); fclose(f); }
        g_sampler.report = g_sampler.report.substr(0, 1500) + "\n  ... full report in profile.txt";
        g_sampler.done = true;
    }

    bool StartSampler(int ms, bool allThreads, std::string& why)
    {
        if (g_sampler.running) { why = "a profile is already running"; return false; }
        if (g_sampler.thread.joinable()) g_sampler.thread.join();
        HWND main = static_cast<HWND>(rp::host->MainWindow(rp::self));
        g_sampler.mainTid = main ? GetWindowThreadProcessId(main, nullptr) : 0;
        if (!g_sampler.mainTid) { why = "no main window yet"; return false; }
        g_sampler.leaf.clear(); g_sampler.incl.clear(); g_sampler.stacks.clear(); g_sampler.samples = g_sampler.failed = 0; g_sampler.report.clear();
        g_sampler.ms = ms; g_sampler.done = false; g_sampler.running = true; g_sampler.allThreads = allThreads; g_sampler.wsamples = 0; g_sampler.wstacks.clear();
        g_sampler.thread = std::thread([] { __try { SamplerMain(); } __except (EXCEPTION_EXECUTE_HANDLER) { g_sampler.report = "sampler faulted"; g_sampler.done = true; } g_sampler.running = false; });
        return true;
    }

    // ---- Feature 1: the fast asset-tree filter, a detour of assetTreeUpdateFilters (0x1400B46C0), ----
    namespace filt
    {
        const uintptr_t kUpdateFilters = 0x1400B46C0;   // assetTreeUpdateFilters(apeAssetTreeWidget*)
        // the four slots that call it from a filter box edit (start, end): CheckGDTFilter, CheckNameFilter, CheckTypeFilter, CheckValueFilter
        const uintptr_t kTypingSlots[4][2] = { { 0x1400B2180, 0x1400B21F0 }, { 0x1400B2D00, 0x1400B2D70 }, { 0x1400B45D0, 0x1400B4640 }, { 0x1400B5DF0, 0x1400B5E60 } };
        const size_t off_Item_Entry = 0x40, off_Item_Hidden = 0x48, off_Entry_Type = 0x20, off_Entry_Favorite = 0x25;

        using UpdateFn = void (__fastcall*)(void* tree);
        UpdateFn g_orig = nullptr;
        bool g_hooked = false;
        bool g_fast = true;          // [filter] fast
        int  g_debounceMs = 150;     // [filter] debounce_ms
        bool g_disabled = false;     // set after a fault: always run the original
        uintptr_t g_slots[4][2] = {};

        // --- grammar ---
        struct Term { std::wstring text; bool negate = false; bool wildcard = false; std::wstring lowered; };   // lowered: for the wildcard, text is compared case-sensitively (APE: QString::contains)
        struct Group { std::vector<Term> terms; };
        struct Expr { std::vector<Group> groups; };

        std::wstring Lower(const wchar_t* s, size_t n)
        {
            std::wstring o(s, n);
            for (auto& ch : o) ch = ch < 128 ? wchar_t(tolower(int(ch))) : wchar_t(towlower(ch));
            return o;
        }
        std::wstring QStringToW(void** qs)
        {
            QArrayDataRaw* d = static_cast<QArrayDataRaw*>(*qs);
            if (!d || d->size <= 0) return std::wstring();
            return std::wstring(reinterpret_cast<const wchar_t*>(reinterpret_cast<char*>(d) + d->offset), size_t(d->size));
        }
        std::wstring ItemTextW(void* item) { void* qs = nullptr; qt.itemText(item, &qs, 0); std::wstring w = QStringToW(&qs); qt.qsDtor(&qs); return w; }
        std::wstring EditTextW(void* edit) { void* qs = nullptr; qt.lineEditText(edit, &qs); std::wstring w = QStringToW(&qs); qt.qsDtor(&qs); return w; }
        std::string Narrow(const std::wstring& w) { int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr); std::string o(n > 0 ? n : 0, '\0'); if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), &o[0], n, nullptr, nullptr); return o; }

        // anchored glob on lowered text: '*' any run, '?' one char (QRegExp::Wildcard, exactMatch)
        bool Glob(const wchar_t* p, const wchar_t* s)
        {
            const wchar_t* star = nullptr; const wchar_t* ss = nullptr;
            while (*s)
            {
                if (*p == L'*') { star = p++; ss = s; continue; }
                if (*p == L'?' || *p == *s) { ++p; ++s; continue; }
                if (star) { p = star + 1; s = ++ss; continue; }
                return false;
            }
            while (*p == L'*') ++p;
            return *p == 0;
        }
        // APE: a wildcard term is case-insensitive (QRegExp::Wildcard), a plain term is a case-SENSITIVE substring
        bool TermMatch(const Term& t, const std::wstring& name, const std::wstring& lowered)
        {
            bool m = t.wildcard ? Glob(t.lowered.c_str(), lowered.c_str()) : (t.text.empty() || name.find(t.text) != std::wstring::npos);
            return m != t.negate;
        }
        bool ExprMatch(const Expr& e, const std::wstring& name, const std::wstring& lowered)
        {
            if (e.groups.empty()) return true;
            for (const Group& g : e.groups)
            {
                bool all = true;
                for (const Term& t : g.terms) if (!TermMatch(t, name, lowered)) { all = false; break; }
                if (all) return true;
            }
            return false;
        }
        std::wstring Trim(const std::wstring& s) { size_t a = s.find_first_not_of(L" \t\r\n"); if (a == std::wstring::npos) return L""; size_t b = s.find_last_not_of(L" \t\r\n"); return s.substr(a, b - a + 1); }
        // spaceIsAnd: name/GDT grammar, false: the type filter (comma tokens only, every token an anchored wildcard)
        Expr Parse(const std::wstring& text, bool spaceIsAnd)
        {
            Expr e;
            size_t start = 0;
            for (;;)
            {
                size_t comma = text.find(L',', start);
                std::wstring grp = text.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
                std::vector<std::wstring> parts;
                if (spaceIsAnd) { size_t p = 0; while (p <= grp.size()) { size_t sp = grp.find(L' ', p); parts.push_back(grp.substr(p, sp == std::wstring::npos ? std::wstring::npos : sp - p)); if (sp == std::wstring::npos) break; p = sp + 1; } }
                else parts.push_back(grp);
                Group g;
                for (std::wstring part : parts)
                {
                    part = Trim(part);
                    if (part.empty()) continue;
                    Term t;
                    if (part[0] == L'-') { t.negate = true; part.erase(0, 1); part = Trim(part); }   // a lone '-' stays: it negates "everything", so nothing passes (APE shows 0)
                    t.wildcard = !spaceIsAnd || part.find(L'*') != std::wstring::npos;
                    t.text = part;
                    t.lowered = Lower(part.c_str(), part.size());
                    g.terms.push_back(t);
                }
                if (!g.terms.empty()) e.groups.push_back(g);
                if (comma == std::wstring::npos) break;
                start = comma + 1;
            }
            return e;
        }

        // ---- the value filter (key=value), APE's rules: parser 0x1400B2DA0, match 0x1400B63B0, lookup 0x140062190 ----
        // ',' = OR, ' ' = AND, leading '-' = NOT, a '*' anywhere = wildcard mode, else exact key and exact value.
        const uintptr_t kTypeSchemas = 0x14118D298;   // global: object whose QList at +0x30 holds one schema per type index
        const uintptr_t kNoValue = 0x14118D978;       // global: object whose char* at +0x18 is what a lookup of a missing key returns
        const size_t off_Entry_Pairs = 0x28, off_Entry_Values = 0x30, off_Values_Array = 0x18, off_Schema_Map = 0x10, off_Global_List = 0x30, off_NoValue_Str = 0x18;
        bool g_fastValue = true;   // [filter] fast_value

        inline unsigned char L1Lower(unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7) ? c + 32 : c; }
        bool StrIEq(const char* a, const std::string& lowered)   // qstricmp(a, b) == 0 with b already lowered
        {
            if (!a) return false;
            const unsigned char* s = reinterpret_cast<const unsigned char*>(a);
            size_t i = 0;
            for (; s[i] && i < lowered.size(); ++i) if (L1Lower(s[i]) != static_cast<unsigned char>(lowered[i])) return false;
            return s[i] == 0 && i == lowered.size();
        }
        // anchored glob, '*' and '?' only, pattern lowered, text lowered on the fly (QRegExp Wildcard, CaseInsensitive, exactMatch)
        bool GlobA(const char* pat, const char* str)
        {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(pat); const unsigned char* s = reinterpret_cast<const unsigned char*>(str ? str : "");
            const unsigned char* star = nullptr; const unsigned char* ss = nullptr;
            while (*s)
            {
                if (*p == '*') { star = p++; ss = s; continue; }
                if (*p == '?' || *p == L1Lower(*s)) { ++p; ++s; continue; }
                if (star) { p = star + 1; s = ++ss; continue; }
                return false;
            }
            while (*p == '*') ++p;
            return *p == 0;
        }

        struct VPat
        {
            std::string text, lowered; bool simple = true; void* rx = nullptr;   // rx: Qt's QRegExp when the pattern has a '[' set
            std::unordered_map<std::string, bool> memo;
            bool Match(const char* s)
            {
                if (simple) return GlobA(lowered.c_str(), s);
                std::string k = s ? s : "";
                auto it = memo.find(k); if (it != memo.end()) return it->second;
                bool m = false;
                if (rx)
                {
                    void* qs = nullptr; qt.qstringFromLatin1(&qs, k.c_str(), int(k.size()));
                    m = qt.rxExactMatch(&rx, &qs);
                    qt.qsDtor(&qs);
                }
                memo.emplace(std::move(k), m);
                return m;
            }
            void Prepare(bool wildcard)
            {
                lowered.clear(); for (char ch : text) lowered += char(L1Lower(static_cast<unsigned char>(ch)));
                simple = !wildcard || text.find('[') == std::string::npos;
                if (!simple && qt.qstringFromLatin1 && qt.rxCtor)
                {
                    void* qs = nullptr; qt.qstringFromLatin1(&qs, text.c_str(), int(text.size()));
                    qt.rxCtor(&rx, &qs, 0 /* Qt::CaseInsensitive */, 1 /* QRegExp::Wildcard */);
                    qt.qsDtor(&qs);
                }
            }
            void Free() { if (rx && qt.rxDtor) qt.rxDtor(&rx); rx = nullptr; }
        };
        struct VTerm
        {
            VPat key, value; bool wanted = true;
            std::vector<int> exactIdx;                 // exact mode, per type: schema index of the key, -1 none, -2 not resolved yet
            std::vector<std::vector<int>> wildIdx;     // wildcard mode, per type: the schema indices whose key matches
            std::vector<uint8_t> wildDone;
        };
        struct VGroup { std::vector<VTerm> terms; };
        struct VExpr { std::vector<VGroup> groups; bool wildcard = false; void Free() { for (auto& g : groups) for (auto& t : g.terms) { t.key.Free(); t.value.Free(); } } };

        std::vector<std::wstring> SplitSkipEmpty(const std::wstring& s, wchar_t sep)
        {
            std::vector<std::wstring> out; size_t p = 0;
            for (;;) { size_t q = s.find(sep, p); std::wstring part = s.substr(p, q == std::wstring::npos ? std::wstring::npos : q - p); if (!part.empty()) out.push_back(part); if (q == std::wstring::npos) break; p = q + 1; }
            return out;
        }
        VExpr ParseValue(const std::wstring& text)
        {
            VExpr e; e.wildcard = text.find(L'*') != std::wstring::npos;
            for (const std::wstring& grp : SplitSkipEmpty(text, L','))
            {
                VGroup g;
                for (std::wstring part : SplitSkipEmpty(grp, L' '))
                {
                    VTerm t;
                    if (part[0] == L'-') { t.wanted = false; part.erase(0, 1); }
                    std::string l1; l1.reserve(part.size()); for (wchar_t ch : part) l1 += char(ch < 256 ? ch : L'?');   // QString::toLatin1
                    size_t eq = l1.find('=');
                    if (eq == std::string::npos || eq == 0 || l1.find('=', eq + 1) != std::string::npos) continue;   // QByteArray::split('=') must give 2 parts, key non-empty
                    t.key.text = l1.substr(0, eq); t.value.text = l1.substr(eq + 1);
                    t.key.Prepare(e.wildcard); t.value.Prepare(e.wildcard);
                    g.terms.push_back(std::move(t));
                }
                e.groups.push_back(std::move(g));
            }
            return e;
        }

        // APE's property data for one run: the type schemas walked once each
        struct SchemaView { bool built = false, ok = false; std::vector<const char*> keys, defaults; };
        struct ValueCtx { uint8_t* schemasObj = nullptr; const char* noValue = ""; std::vector<SchemaView> views; } g_vctx;
        bool ValueReady()
        {
            if (!qt.mapNextNode || !qt.qstringFromLatin1 || !qt.rxCtor || !qt.rxExactMatch || !qt.rxDtor || !rp::Ape()) return false;
            uint8_t** gSchemas = static_cast<uint8_t**>(rp::Ape()->ApeAddress(rp::self, kTypeSchemas));
            uint8_t** gNoValue = static_cast<uint8_t**>(rp::Ape()->ApeAddress(rp::self, kNoValue));
            if (!gSchemas || !gNoValue || !*gSchemas) return false;
            g_vctx.schemasObj = *gSchemas;
            const char* nv = *gNoValue ? *reinterpret_cast<const char**>(*gNoValue + off_NoValue_Str) : nullptr;
            g_vctx.noValue = nv ? nv : "";
            g_vctx.views.clear();
            return true;
        }
        const SchemaView* View(int ti)
        {
            if (ti < 0) return nullptr;
            if (size_t(ti) >= g_vctx.views.size()) g_vctx.views.resize(size_t(ti) + 1);
            SchemaView& v = g_vctx.views[size_t(ti)];
            if (v.built) return v.ok ? &v : nullptr;
            v.built = true;
            QListDataD* d = *reinterpret_cast<QListDataD**>(g_vctx.schemasObj + off_Global_List);
            if (!d || ti >= d->end - d->begin) return nullptr;
            uint8_t* schema = static_cast<uint8_t*>(d->array[d->begin + ti]);
            uint8_t* md = schema ? *reinterpret_cast<uint8_t**>(schema + off_Schema_Map) : nullptr;   // QMapData: header at +8, root +0x10, mostLeftNode +0x20
            if (!md) return nullptr;
            const void* end = md + 8;
            const void* n = *reinterpret_cast<void**>(md + 0x10) ? *reinterpret_cast<void**>(md + 0x20) : end;
            while (n != end)
            {
                v.keys.push_back(*reinterpret_cast<const char* const*>(static_cast<const uint8_t*>(n) + 0x18));
                v.defaults.push_back(*reinterpret_cast<const char* const*>(static_cast<const uint8_t*>(n) + 0x20));
                n = qt.mapNextNode(n);
            }
            v.ok = true;
            return &v;
        }
        inline const char* SlotValue(uint8_t* entry, const SchemaView& v, int i)
        {
            uint8_t* vals = *reinterpret_cast<uint8_t**>(entry + off_Entry_Values);
            return vals ? (*reinterpret_cast<const char***>(vals + off_Values_Array))[i] : v.defaults[size_t(i)];
        }
        // the entry's own key/value list (QVector<pair*>, pair = { char* key, char* value }), or null
        inline void** Pairs(uint8_t* entry, int& n)
        {
            void* vec = *reinterpret_cast<void**>(entry + off_Entry_Pairs);
            if (!vec) { n = 0; return nullptr; }
            QArrayDataRaw* d = *static_cast<QArrayDataRaw**>(vec);
            n = d ? d->size : 0;
            return d ? reinterpret_cast<void**>(reinterpret_cast<char*>(d) + d->offset) : nullptr;
        }
        bool TermExact(uint8_t* entry, VTerm& t)
        {
            const char* v = g_vctx.noValue;
            int n = 0; void** pairs = Pairs(entry, n);
            if (pairs)
            {
                for (int i = 0; i < n; ++i) { const char* const* p = static_cast<const char* const*>(pairs[i]); if (StrIEq(p[0], t.key.lowered)) { v = p[1]; break; } }
            }
            else
            {
                int ti = *reinterpret_cast<int*>(entry + off_Entry_Type);
                if (const SchemaView* sv = View(ti))
                {
                    if (size_t(ti) >= t.exactIdx.size()) t.exactIdx.resize(size_t(ti) + 1, -2);
                    int& idx = t.exactIdx[size_t(ti)];
                    if (idx == -2) { idx = -1; for (size_t i = 0; i < sv->keys.size(); ++i) if (StrIEq(sv->keys[i], t.key.lowered)) { idx = int(i); break; } }
                    if (idx >= 0) v = SlotValue(entry, *sv, idx);
                }
            }
            bool r = (v && *v) ? strcmp(v, t.value.text.c_str()) == 0 : t.value.text.empty();
            return r == t.wanted;
        }
        bool TermWild(uint8_t* entry, VTerm& t)
        {
            int n = 0; void** pairs = Pairs(entry, n);
            if (pairs)
            {
                for (int i = 0; i < n; ++i)
                {
                    const char* const* p = static_cast<const char* const*>(pairs[i]);
                    if (t.key.Match(p[0]) && t.value.Match(p[1]) == t.wanted) return true;
                }
            }
            else
            {
                int ti = *reinterpret_cast<int*>(entry + off_Entry_Type);
                if (const SchemaView* sv = View(ti))
                {
                    if (size_t(ti) >= t.wildDone.size()) { t.wildDone.resize(size_t(ti) + 1, 0); t.wildIdx.resize(size_t(ti) + 1); }
                    if (!t.wildDone[size_t(ti)]) { t.wildDone[size_t(ti)] = 1; for (size_t i = 0; i < sv->keys.size(); ++i) if (t.key.Match(sv->keys[i])) t.wildIdx[size_t(ti)].push_back(int(i)); }
                    for (int i : t.wildIdx[size_t(ti)]) if (t.value.Match(SlotValue(entry, *sv, i)) == t.wanted) return true;
                }
            }
            return t.value.text.empty() == t.wanted;   // APE: no property decided it -> (value pattern empty) == wanted
        }
        bool ValueMatch(uint8_t* entry, VExpr& e)
        {
            if (!entry) return false;
            for (VGroup& g : e.groups)
            {
                bool all = true;
                for (VTerm& t : g.terms) if (!(e.wildcard ? TermWild(entry, t) : TermExact(entry, t))) { all = false; break; }
                if (all) return true;
            }
            return false;
        }

        // --- APE's filter widgets ---
        struct Ui { void* nameEdit = nullptr; void* nameCb = nullptr; void* typeEdit = nullptr; void* typeCb = nullptr; void* gdtEdit = nullptr; void* gdtCb = nullptr; void* valueEdit = nullptr; void* valueCb = nullptr; void* label = nullptr; void* favAction = nullptr; bool ok = false; } g_ui;
        bool FindUi()
        {
            if (g_ui.ok) return true;
            void* dock = nullptr;
            ForEachWidget([&](void* w) { if (IsA(w, "QDockWidget") && TakeString(qt.objectName, w) == "FilterToolBar") { dock = w; return true; } return false; });
            if (!dock) return false;
            Ui u;
            std::vector<void*> stack = { dock };
            void* lastCb = nullptr;
            for (size_t k = 0; k < stack.size() && k < 4096; ++k)
            {
                void* o = stack[k];
                if (IsA(o, "QCheckBox")) lastCb = o;
                else if (IsA(o, "QLineEdit"))
                {
                    std::string tip = TakeString(qt.toolTip, o);
                    if (tip.find("asset names") != std::string::npos) { u.nameEdit = o; u.nameCb = lastCb; }
                    else if (tip.find("asset types") != std::string::npos) { u.typeEdit = o; u.typeCb = lastCb; }
                    else if (tip.find("GDT files") != std::string::npos) { u.gdtEdit = o; u.gdtCb = lastCb; }
                    else if (tip.find("keys and values") != std::string::npos) { u.valueEdit = o; u.valueCb = lastCb; }
                }
                for (void* c : Children(o)) if (IsA(c, "QWidget")) stack.push_back(c);
            }
            // the asset counter (APE's labelNumAssets): the label showing "<n>" or "<shown> / <total>", in the asset or filter dock
            void* anyCount = nullptr;
            ForEachWidget([&](void* w) {
                if (!IsA(w, "QLabel")) return false;
                std::string t = TakeString(qt.labelText, w);
                if (!LooksLikeCount(t) || t.find('/') == std::string::npos && t.size() < 3) return false;
                // APE's counter sits in the main window's own tool bar (no dock ancestor, not in a property grid)
                bool inGrid = false; for (void* q = w; q; q = ParentOf(q)) if (strcmp(ClassName(q), "apeAssetWidget") == 0) { inGrid = true; break; }
                std::string dock = DockOf(w);
                rp::Log("filter: count-like label %p '%s' dock='%s' grid=%d", w, t.c_str(), dock.c_str(), inGrid ? 1 : 0);
                if (inGrid) return false;
                if (dock.empty()) { u.label = w; return true; }
                if (!anyCount) anyCount = w;
                return false; });
            if (!u.label) u.label = anyCount;
            ForEachWidget([&](void* w) {
                QListRaw acts = {}; qt.widgetActions(w, &acts);
                if (acts.d) for (int j = acts.d->begin; j < acts.d->end && !u.favAction; ++j) { void* a = acts.d->array[j]; if (a && TakeString(qt.actionText, a) == "Favorites Only") u.favAction = a; }
                ListRelease(acts); return u.favAction != nullptr; });
            u.ok = u.nameEdit && u.nameCb && u.typeEdit && u.typeCb && u.gdtEdit && u.gdtCb && u.valueEdit && u.valueCb && u.label;
            g_ui = u;
            rp::Log("filter: widgets %s (name %p/%p type %p/%p gdt %p/%p value %p/%p label %p favorites action %p)", u.ok ? "found" : "INCOMPLETE",
                    u.nameEdit, u.nameCb, u.typeEdit, u.typeCb, u.gdtEdit, u.gdtCb, u.valueEdit, u.valueCb, u.label, u.favAction);
            return u.ok;
        }

        // --- type names by APE's type index (gdt.db _gdf PK_id - 1) ---
        std::vector<std::wstring> g_typeNames;
        bool LoadTypeNames()
        {
            if (!g_typeNames.empty()) return true;
            HMODULE sq = GetModuleHandleW(L"sqlite64r.dll");
            if (!sq || !rp::Ape()) return false;
            using OpenFn = int (*)(const char*, void**, int, const char*); using PrepFn = int (*)(void*, const char*, int, void**, const char**);
            using StepFn = int (*)(void*); using ColIntFn = int (*)(void*, int); using ColTextFn = const unsigned char* (*)(void*, int); using FinFn = int (*)(void*); using CloseFn = int (*)(void*);
            auto open = reinterpret_cast<OpenFn>(GetProcAddress(sq, "sqlite3_open_v2")); auto prep = reinterpret_cast<PrepFn>(GetProcAddress(sq, "sqlite3_prepare_v2"));
            auto step = reinterpret_cast<StepFn>(GetProcAddress(sq, "sqlite3_step")); auto colInt = reinterpret_cast<ColIntFn>(GetProcAddress(sq, "sqlite3_column_int"));
            auto colText = reinterpret_cast<ColTextFn>(GetProcAddress(sq, "sqlite3_column_text")); auto fin = reinterpret_cast<FinFn>(GetProcAddress(sq, "sqlite3_finalize")); auto close = reinterpret_cast<CloseFn>(GetProcAddress(sq, "sqlite3_close"));
            if (!open || !prep || !step || !colInt || !colText || !fin || !close) return false;
            wchar_t pathW[1024] = {}; rp::Ape()->GdtDbPath(rp::self, pathW, 1024);
            char path[2048] = {}; WideCharToMultiByte(CP_UTF8, 0, pathW, -1, path, sizeof path, nullptr, nullptr);
            void* db = nullptr;
            if (open(path, &db, 0x00000001 | 0x00010000 /* READONLY | FULLMUTEX */, nullptr) != 0) { if (db) close(db); return false; }
            void* st = nullptr;
            std::vector<std::wstring> names; bool gaps = false;
            if (prep(db, "SELECT PK_id, name FROM _gdf ORDER BY PK_id", -1, &st, nullptr) == 0)
            {
                while (step(st) == 100)
                {
                    int id = colInt(st, 0); const char* nm = reinterpret_cast<const char*>(colText(st, 1));
                    if (id != int(names.size()) + 1) gaps = true;
                    while (int(names.size()) < id - 1) names.push_back(L"");
                    std::string n = nm ? nm : ""; std::wstring w(n.begin(), n.end());
                    names.push_back(Lower(w.c_str(), w.size()));
                }
                fin(st);
            }
            close(db);
            if (names.empty()) return false;
            g_typeNames = names;
            rp::Log("filter: %zu asset types from gdt.db%s", names.size(), gaps ? " (PK gaps: index = PK_id-1)" : "");
            return true;
        }

        // --- the cache of the tree ---
        struct Node { void* item; uint8_t* entry; std::wstring name, lowered; int type; int parent; };   // parent: index into nodes, -1 = the GDT row
        struct Gdt { void* item; std::wstring name, lowered; int first, count; int children; bool hidden; };
        struct Cache { void* tree = nullptr; std::vector<Gdt> gdts; std::vector<Node> nodes; uint64_t built = 0; double buildMs = 0; } g_cache;

        void AddNodes(void* item, int parent, Cache& c)
        {
            int n = qt.childCount(item);
            for (int i = 0; i < n; ++i)
            {
                void* ch = qt.child(item, i);
                Node nd; nd.item = ch; nd.parent = parent;
                nd.entry = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(ch) + off_Item_Entry);
                nd.type = nd.entry ? *reinterpret_cast<int*>(nd.entry + off_Entry_Type) : -1;
                if (nd.type < 0 || nd.type >= int(g_typeNames.size()) + 64) nd.type = -1;
                nd.name = ItemTextW(ch); nd.lowered = Lower(nd.name.c_str(), nd.name.size());
                int idx = int(c.nodes.size());
                c.nodes.push_back(std::move(nd));
                AddNodes(ch, idx, c);
            }
        }
        int CountBelow(void* item) { int n = qt.childCount(item), t = n; for (int i = 0; i < n; ++i) t += CountBelow(qt.child(item, i)); return t; }
        bool CacheValid(void* tree)
        {
            Cache& c = g_cache;
            if (c.tree != tree) return false;
            int tl = qt.topLevelItemCount(tree);
            if (tl != int(c.gdts.size())) return false;
            for (int i = 0; i < tl; ++i)
            {
                const Gdt& g = c.gdts[i];
                void* it = qt.topLevelItem(tree, i);
                if (it != g.item || qt.childCount(it) != g.children) return false;
                if (CountBelow(it) != g.count) return false;
            }
            return true;
        }
        void BuildCache(void* tree)
        {
            LoadTypeNames();   // AddNodes bounds the type index by the name count
            uint64_t t0 = Now();
            Cache c; c.tree = tree;
            int tl = qt.topLevelItemCount(tree);
            c.gdts.reserve(tl); c.nodes.reserve(320000);
            for (int i = 0; i < tl; ++i)
            {
                void* it = qt.topLevelItem(tree, i);
                Gdt g; g.item = it; g.children = qt.childCount(it); g.first = int(c.nodes.size()); g.hidden = qt.itemIsHidden(it);
                g.name = ItemTextW(it); g.lowered = Lower(g.name.c_str(), g.name.size());
                AddNodes(it, -1, c);
                g.count = int(c.nodes.size()) - g.first;
                c.gdts.push_back(g);
            }
            c.built = Now(); c.buildMs = Ms(c.built - t0);
            g_cache = std::move(c);
            rp::Log("filter: cache built: %zu GDT rows, %zu assets in %.0f ms", g_cache.gdts.size(), g_cache.nodes.size(), g_cache.buildMs);
        }

        // --- stats ---
        struct Stats { int runs = 0, fallbacks = 0, debounced = 0, rebuilds = 0; double lastMatchMs = 0, lastApplyMs = 0, lastTotalMs = 0, lastValueMs = 0; int lastChanges = 0, lastShown = 0, lastTotal = 0, lastGdtHidden = 0; std::string lastFilters; } g_stats;

        void ShowOurGdtRows()
        {
            // APE's engine never hides GDT rows: undo ours before it runs (or before we unload)
            for (Gdt& g : g_cache.gdts) if (g.hidden) { qt.itemSetHidden(g.item, false); g.hidden = false; }
        }
        void SetLabel(const std::wstring& text)
        {
            if (!g_ui.label) return;
            void* qs = nullptr; qt.qstringCtor(&qs, text.c_str(), int(text.size()));
            qt.labelSetText(g_ui.label, &qs);
            qt.qsDtor(&qs);
        }

        void Run(void* tree)
        {
            uint64_t t0 = Now();
            if (!FindUi()) { ++g_stats.fallbacks; g_orig(tree); return; }
            std::wstring nameF = qt.buttonChecked(g_ui.nameCb) ? EditTextW(g_ui.nameEdit) : L"";
            std::wstring typeF = qt.buttonChecked(g_ui.typeCb) ? EditTextW(g_ui.typeEdit) : L"";
            std::wstring gdtF = qt.buttonChecked(g_ui.gdtCb) ? EditTextW(g_ui.gdtEdit) : L"";
            std::wstring valueF = qt.buttonChecked(g_ui.valueCb) ? EditTextW(g_ui.valueEdit) : L"";
            bool favOnly = g_ui.favAction && qt.actionChecked(g_ui.favAction);
            g_stats.lastFilters = "name='" + Narrow(nameF) + "' type='" + Narrow(typeF) + "' gdt='" + Narrow(gdtF) + "' value='" + Narrow(valueF) + "'" + (favOnly ? " favorites" : "");
            VExpr valueE = ParseValue(valueF);
            bool valueOn = !valueE.groups.empty();   // APE: an empty group list = no value filter
            if ((valueOn && (!g_fastValue || !ValueReady())) || !LoadTypeNames())
            {
                // APE's engine does the value filter then (slowly), give it the tree as it expects it
                valueE.Free();
                ShowOurGdtRows();
                ++g_stats.fallbacks;
                g_orig(tree);
                return;
            }
            if (!CacheValid(tree)) { BuildCache(tree); ++g_stats.rebuilds; }
            Cache& c = g_cache;
            Expr nameE = Parse(nameF, true), gdtE = Parse(gdtF, true), typeE = Parse(typeF, false);
            std::vector<uint8_t> typePass(g_typeNames.size() + 64, 1);
            if (!typeE.groups.empty()) for (size_t i = 0; i < typePass.size(); ++i) typePass[i] = i < g_typeNames.size() && ExprMatch(typeE, g_typeNames[i], g_typeNames[i]) ? 1 : 0;
            bool anyNameOrFav = !nameE.groups.empty() || favOnly, anyType = !typeE.groups.empty();
            bool assetFilters = anyNameOrFav || anyType || valueOn;
            double valueMs = 0;

            std::vector<uint8_t> vis(c.nodes.size(), 1);
            uint64_t t1 = Now();
            int shown = 0, changes = 0, gdtHidden = 0;   // shown = matching assets (APE's counter leaves out the parents that are only shown for a matching derived child)
            qt.setUpdatesEnabled(tree, false);
            bool wasBlocked = qt.blockSignals(tree, true);
            for (Gdt& g : c.gdts)
            {
                bool gpass = ExprMatch(gdtE, g.name, g.lowered);
                int first = g.first, end = g.first + g.count;
                bool any = false;
                if (gpass)
                {
                    if (anyNameOrFav || anyType || valueOn)
                    {
                        uint64_t tv = valueOn ? Now() : 0;
                        for (int k = first; k < end; ++k)
                        {
                            const Node& n = c.nodes[k];
                            bool m = true;
                            if (anyType && n.type >= 0 && !typePass[n.type]) m = false;
                            if (m && favOnly && !(n.entry && n.entry[off_Entry_Favorite])) m = false;
                            if (m && !nameE.groups.empty()) m = ExprMatch(nameE, n.name, n.lowered);
                            if (m && valueOn) m = ValueMatch(*reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(n.item) + off_Item_Entry), valueE);   // the live entry: a reload can replace it
                            vis[k] = m ? 1 : 0;
                            if (m) ++shown;
                        }
                        for (int k = end - 1; k >= first; --k) if (vis[k] && c.nodes[k].parent >= 0) vis[c.nodes[k].parent] = 1;   // parents of a match stay visible
                        if (valueOn) valueMs += Ms(Now() - tv);
                    }
                    else shown += g.count;
                    for (int k = first; k < end && !any; ++k) if (vis[k]) any = true;
                    // the assets of a GDT row that is about to be hidden keep their state: that is what makes a keystroke cheap
                    if (any)
                        for (int k = first; k < end; ++k)
                        {
                            uint8_t hide = vis[k] ? 0 : 1;
                            uint8_t* flag = static_cast<uint8_t*>(c.nodes[k].item) + off_Item_Hidden;
                            if (*flag != hide) { *flag = hide; qt.itemSetHidden(c.nodes[k].item, hide != 0); ++changes; }
                        }
                }
                bool hideGdt = !gpass || (!any && (g.count > 0 || assetFilters));   // an empty GDT row shows unless an asset filter is on, so the GDT filter finds it
                if (g.hidden != hideGdt) { g.hidden = hideGdt; qt.itemSetHidden(g.item, hideGdt); ++changes; }
                if (hideGdt) ++gdtHidden;
            }
            qt.blockSignals(tree, wasBlocked);
            qt.setUpdatesEnabled(tree, true);
            valueE.Free();
            uint64_t t2 = Now();
            int total = int(c.nodes.size());
            SetLabel(shown == total ? std::to_wstring(total) : std::to_wstring(shown) + L" / " + std::to_wstring(total));
            ++g_stats.runs; g_stats.lastMatchMs = Ms(t1 - t0); g_stats.lastApplyMs = Ms(t2 - t1); g_stats.lastTotalMs = Ms(Now() - t0);
            g_stats.lastChanges = changes; g_stats.lastShown = shown; g_stats.lastTotal = total; g_stats.lastGdtHidden = gdtHidden; g_stats.lastValueMs = valueMs;
            rp::Log("filter: %s -> %d / %d shown, %d GDT rows hidden, %d changes, %.0f ms (%.0f match + %.0f apply; value filter %.0f ms of the loop)", g_stats.lastFilters.c_str(), shown, total, gdtHidden, changes, g_stats.lastTotalMs, g_stats.lastMatchMs, g_stats.lastApplyMs, valueMs);
        }

        void RunGuarded(void* tree)
        {
            __try { Run(tree); }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_disabled = true;
                qt.blockSignals(tree, false); qt.setUpdatesEnabled(tree, true);
                rp::Log("filter: the fast filter faulted (code 0x%08x); disabled for this session, APE's own engine runs from now on", unsigned(GetExceptionCode()));
            }
        }

        // debounce state
        void*    g_pendingTree = nullptr;
        uint64_t g_pendingAt = 0;

        std::vector<uintptr_t> g_callers;   // unique return addresses seen (RVA), for the log
        bool FromTyping(uintptr_t ret)
        {
            if (g_callers.size() < 16 && std::find(g_callers.begin(), g_callers.end(), ret) == g_callers.end())
            {
                g_callers.push_back(ret);
                HMODULE m = nullptr; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(ret), &m);
                wchar_t path[MAX_PATH] = {}; if (m) GetModuleFileNameW(m, path, MAX_PATH); const wchar_t* nm = wcsrchr(path, L'\\'); nm = nm ? nm + 1 : path;
                rp::Log("filter: called from %S+0x%llx", nm, (unsigned long long)(ret - uintptr_t(m)));
            }
            for (int i = 0; i < 4; ++i) if (g_slots[i][0] && ret >= g_slots[i][0] && ret < g_slots[i][1]) return true;
            // typing: the keyboard focus is in one of the filter boxes (the slots above are only the direct callers we know)
            if (g_ui.ok)
            {
                void* top = qt.window(g_ui.nameEdit);
                void* f = top ? qt.widgetFocusWidget(top) : nullptr;
                if (f && (f == g_ui.nameEdit || f == g_ui.typeEdit || f == g_ui.gdtEdit || f == g_ui.valueEdit)) return true;
            }
            return false;
        }
        void __fastcall hk_UpdateFilters(void* tree)
        {
            if (!g_fast || g_disabled || !g_orig) { if (g_orig) g_orig(tree); return; }
            uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
            if (g_debounceMs > 0 && FromTyping(ret))
            {
                if (g_pendingTree) ++g_stats.debounced;
                g_pendingTree = tree; g_pendingAt = Now();
                return;
            }
            g_pendingTree = nullptr;
            RunGuarded(tree);
        }
        void Tick()
        {
            if (g_pendingTree && Ms(Now() - g_pendingAt) >= double(g_debounceMs))
            {
                void* t = g_pendingTree; g_pendingTree = nullptr;
                if (g_disabled || !g_fast) { if (g_orig) g_orig(t); }
                else RunGuarded(t);
            }
        }

        bool Install()
        {
            if (g_hooked) return true;
            if (!rp::Ape() || !qt.bound) return false;
            void* target = rp::Ape()->ApeAddress(rp::self, kUpdateFilters);
            if (!target) { rp::Log("filter: APE binary not recognised, the fast filter is off"); return false; }
            DWORD64 base = 0; PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(DWORD64(target), &base, nullptr);
            if (!fn || DWORD64(fn->BeginAddress) + base != DWORD64(target)) { rp::Log("filter: no function starts at the expected address, the fast filter is off"); return false; }
            for (int i = 0; i < 4; ++i) { g_slots[i][0] = uintptr_t(rp::Ape()->ApeAddress(rp::self, kTypingSlots[i][0])); g_slots[i][1] = uintptr_t(rp::Ape()->ApeAddress(rp::self, kTypingSlots[i][1])); }
            if (!rp::host->Detour(rp::self, target, reinterpret_cast<void*>(&hk_UpdateFilters), reinterpret_cast<void**>(&g_orig))) { rp::Log("filter: detour failed"); return false; }
            g_hooked = true;
            rp::Log("filter: hooked the tree filter engine at %p (fast=%d, debounce %d ms)", target, g_fast ? 1 : 0, g_debounceMs);
            return true;
        }
        void Uninstall()
        {
            // the host removes the detour, leave the tree the way APE's engine expects it
            if (g_hooked && g_cache.tree) ShowOurGdtRows();
            g_hooked = false; g_pendingTree = nullptr;
        }
        // compare the tree's current state (APE's hidden bytes and GDT rows, e.g. after APE's own engine ran) with what our rules say
        std::string Audit(void* tree)
        {
            if (!FindUi()) return "no ui";
            if (!CacheValid(tree)) BuildCache(tree);
            Cache& c = g_cache;
            std::wstring nameF = qt.buttonChecked(g_ui.nameCb) ? EditTextW(g_ui.nameEdit) : L"";
            std::wstring typeF = qt.buttonChecked(g_ui.typeCb) ? EditTextW(g_ui.typeEdit) : L"";
            std::wstring gdtF = qt.buttonChecked(g_ui.gdtCb) ? EditTextW(g_ui.gdtEdit) : L"";
            bool favOnly = g_ui.favAction && qt.actionChecked(g_ui.favAction);
            std::wstring valueF = qt.buttonChecked(g_ui.valueCb) ? EditTextW(g_ui.valueEdit) : L"";
            VExpr valueE = ParseValue(valueF);
            bool valueOn = !valueE.groups.empty();
            if (valueOn && !ValueReady()) { valueE.Free(); return "audit: the value filter's APE data is not reachable"; }
            Expr nameE = Parse(nameF, true), gdtE = Parse(gdtF, true), typeE = Parse(typeF, false);
            std::vector<uint8_t> typePass(g_typeNames.size() + 64, 1);
            if (!typeE.groups.empty()) for (size_t i = 0; i < typePass.size(); ++i) typePass[i] = i < g_typeNames.size() && ExprMatch(typeE, g_typeNames[i], g_typeNames[i]) ? 1 : 0;
            int apeShown = 0, matches = 0, shownNotMatch = 0, matchHidden = 0, visibleUnderHiddenParent = 0, gdtRowsHidden = 0, gdtRowsHiddenWithVisible = 0;
            std::string ex;
            for (Gdt& g : c.gdts)
            {
                bool gHidden = qt.itemIsHidden(g.item); if (gHidden) ++gdtRowsHidden;
                bool anyVis = false;
                for (int k = g.first; k < g.first + g.count; ++k)
                {
                    const Node& n = c.nodes[k];
                    bool hidden = *(static_cast<uint8_t*>(n.item) + off_Item_Hidden) != 0;
                    bool m = ExprMatch(gdtE, g.name, g.lowered) && (n.type < 0 || typePass[n.type]) && (!favOnly || (n.entry && n.entry[off_Entry_Favorite])) && ExprMatch(nameE, n.name, n.lowered)
                             && (!valueOn || ValueMatch(*reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(n.item) + off_Item_Entry), valueE));
                    if (m) ++matches;
                    if (!hidden && !gHidden) { ++apeShown; anyVis = true; }
                    if (!hidden && !m) { ++shownNotMatch; if (ex.size() < 600) ex += " shown-not-match:'" + Narrow(n.name) + "'(parent " + (n.parent >= 0 ? Narrow(c.nodes[n.parent].name) : std::string("gdt")) + ")"; }
                    if (hidden && m) { ++matchHidden; if (ex.size() < 900) ex += " match-hidden:'" + Narrow(n.name) + "'(parent " + (n.parent >= 0 ? Narrow(c.nodes[n.parent].name) : std::string("gdt")) + (n.parent >= 0 && *(static_cast<uint8_t*>(c.nodes[n.parent].item) + off_Item_Hidden) ? " hidden" : "") + ")"; }
                    if (!hidden && n.parent >= 0 && *(static_cast<uint8_t*>(c.nodes[n.parent].item) + off_Item_Hidden)) ++visibleUnderHiddenParent;
                }
                if (gHidden && anyVis) ++gdtRowsHiddenWithVisible;
            }
            valueE.Free();
            char buf[1200];
            snprintf(buf, sizeof buf, "audit: %s: tree shows %d (bytes), our rules match %d; shown-but-no-match %d, match-but-hidden %d, visible-under-hidden-parent %d, GDT rows hidden %d (%d with visible assets); label '%s'; examples:%s",
                     g_stats.lastFilters.c_str(), apeShown, matches, shownNotMatch, matchHidden, visibleUnderHiddenParent, gdtRowsHidden, gdtRowsHiddenWithVisible, TakeString(qt.labelText, g_ui.label).c_str(), ex.c_str());
            return buf;
        }
        std::string Status()
        {
            char buf[1024];
            snprintf(buf, sizeof buf, "hooked=%d fast=%d fast_value=%d disabled=%d debounce_ms=%d runs=%d fallbacks=%d debounced=%d cache: %zu GDT rows, %zu assets, rebuilds=%d (%.0f ms) last: %s -> %d/%d shown, %d GDT rows hidden, %d changes, %.0f ms (%.0f match + %.0f apply, value %.0f)",
                     g_hooked ? 1 : 0, g_fast ? 1 : 0, g_fastValue ? 1 : 0, g_disabled ? 1 : 0, g_debounceMs, g_stats.runs, g_stats.fallbacks, g_stats.debounced, g_cache.gdts.size(), g_cache.nodes.size(), g_stats.rebuilds, g_cache.buildMs,
                     g_stats.lastFilters.c_str(), g_stats.lastShown, g_stats.lastTotal, g_stats.lastGdtHidden, g_stats.lastChanges, g_stats.lastTotalMs, g_stats.lastMatchMs, g_stats.lastApplyMs, g_stats.lastValueMs);
            return buf;
        }
    }

    // ---- Feature 2: the freeze after committing a value, the commit slot 0x140084510 ends in a ~2 s rescan (0x1400F8A70) ----
    namespace commit
    {
        using filt::Narrow;
        const uintptr_t kRefresh = 0x1400F8A70;
        const uintptr_t kTimerSlotBegin = 0x140084510, kTimerSlotEnd = 0x140084730;
        using RefreshFn = void (__fastcall*)(void* a, void* b, void* c, void* d);
        RefreshFn g_orig = nullptr;
        bool g_hooked = false;
        int  g_minS = 10;        // [commit] scan_min_s (0 = never skip)
        uintptr_t g_slotBegin = 0, g_slotEnd = 0;
        uint64_t g_lastRun = 0;
        int g_runs = 0, g_skipped = 0; double g_lastMs = 0;
        std::vector<uintptr_t> g_callers;

        // FindFirstFileExA trace (diagnostics): which directories the scan walks
        using FindFirstFn = HANDLE (WINAPI*)(LPCSTR, int, LPVOID, int, LPVOID, DWORD);
        FindFirstFn g_origFind = nullptr;
        std::atomic<bool> g_trace{ false };
        std::mutex g_traceMutex;
        std::map<std::string, int> g_traceDirs; std::atomic<int> g_traceCalls{ 0 };
        HANDLE WINAPI hk_FindFirstFileExA(LPCSTR pattern, int level, LPVOID data, int op, LPVOID filter, DWORD flags)
        {
            if (g_trace && pattern)
            {
                ++g_traceCalls;
                std::string p = pattern; size_t sl = p.find_last_of("\\/"); if (sl != std::string::npos) p.resize(sl);
                // keep the first 4 components under the drive so the map stays small
                std::string pat = pattern + (sl == std::string::npos ? 0 : sl + 1);
                int comps = 0; size_t cut = std::string::npos;
                for (size_t i = 0; i < p.size(); ++i) if (p[i] == '\\' || p[i] == '/') { if (++comps == 6) { cut = i; break; } }
                if (cut != std::string::npos) p.resize(cut);
                std::lock_guard<std::mutex> lock(g_traceMutex); if (g_traceDirs.size() < 2000) g_traceDirs[p + "  [" + pat + "]"]++;
            }
            return g_origFind(pattern, level, data, op, filter, flags);
        }

        // --- the change watcher: ReadDirectoryChangesW on the tools root, so the scan only runs when the disk changed ---
        std::thread g_watchThread;
        // Same exit trap for the watcher (detached at static destruction, else ~thread terminates). Destroyed before g_watchThread.
        struct ExitThreadGuard { ~ExitThreadGuard() { if (g_watchThread.joinable()) g_watchThread.detach(); } } g_exitThreadGuard;
        HANDLE g_watchDir = INVALID_HANDLE_VALUE;
        std::atomic<bool> g_watchAlive{ false }, g_watchStop{ false };
        std::atomic<bool> g_changed{ true };            // true until the first scan after a change completes
        std::atomic<int>  g_changeCount{ 0 };
        std::vector<std::wstring> g_ignore;             // top-level folders whose changes do not count ([commit] ignore_dirs)
        std::mutex g_lastChangeMutex; std::wstring g_lastChange;
        std::wstring g_watchRoot;

        bool Ignored(const wchar_t* rel, size_t n)
        {
            size_t sl = 0; while (sl < n && rel[sl] != L'\\' && rel[sl] != L'/') ++sl;
            for (const std::wstring& ig : g_ignore) if (ig.size() == sl && _wcsnicmp(ig.c_str(), rel, sl) == 0) return true;
            return false;
        }
        // The scan only looks for *.gdt files, so only a .gdt change or a folder appearing, going or renamed counts.
        bool g_gdtOnly = true;   // [commit] gdt_only
        bool Relevant(DWORD action, const wchar_t* rel, size_t n)
        {
            if (!g_gdtOnly) return true;
            if (n >= 4 && rel[n - 4] == L'.' && _wcsnicmp(rel + n - 3, L"gdt", 3) == 0) return true;
            if (action == FILE_ACTION_MODIFIED) return false;
            if (action == FILE_ACTION_ADDED || action == FILE_ACTION_RENAMED_NEW_NAME)
            {
                DWORD a = GetFileAttributesW((g_watchRoot + L"\\" + std::wstring(rel, n)).c_str());
                if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
            }
            // removed, renamed away or already gone again: it cannot be looked at, so a name without an extension counts as a folder
            size_t last = n; while (last > 0 && rel[last - 1] != L'\\' && rel[last - 1] != L'/') --last;
            for (size_t i = last; i < n; ++i) if (rel[i] == L'.') return false;
            return true;
        }
        void WatchMain()
        {
            std::vector<char> buf(256 * 1024);
            while (!g_watchStop)
            {
                DWORD got = 0;
                BOOL ok = ReadDirectoryChangesW(g_watchDir, buf.data(), DWORD(buf.size()), TRUE,
                    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION, &got, nullptr, nullptr);
                if (!ok) { if (g_watchStop) break; g_changed = true; g_watchAlive = false; rp::Log("commit: watcher stopped (error %lu); every commit scans again", GetLastError()); return; }
                if (got == 0) { g_changed = true; ++g_changeCount; continue; }   // buffer overflow: assume changes
                size_t off = 0;
                for (;;)
                {
                    const FILE_NOTIFY_INFORMATION* fi = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buf.data() + off);
                    size_t n = fi->FileNameLength / sizeof(wchar_t);
                    if (!Ignored(fi->FileName, n) && Relevant(fi->Action, fi->FileName, n))
                    {
                        g_changed = true; ++g_changeCount;
                        std::lock_guard<std::mutex> lock(g_lastChangeMutex); g_lastChange.assign(fi->FileName, n);
                    }
                    if (!fi->NextEntryOffset) break;
                    off += fi->NextEntryOffset;
                }
            }
            g_watchAlive = false;
        }
        bool StartWatcher()
        {
            const wchar_t* root = rp::host->RadiantRoot(rp::self);
            if (!root || !*root) return false;
            g_watchRoot = root;
            g_watchDir = CreateFileW(root, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (g_watchDir == INVALID_HANDLE_VALUE) { rp::Log("commit: cannot watch %S (error %lu)", root, GetLastError()); return false; }
            std::string ig = rp::ConfigGet("commit", "ignore_dirs", "radiant,bin,gdtdb,share");
            size_t st = 0; while (st <= ig.size()) { size_t c = ig.find(',', st); std::string part = ig.substr(st, c == std::string::npos ? std::string::npos : c - st); if (!part.empty()) g_ignore.emplace_back(part.begin(), part.end()); if (c == std::string::npos) break; st = c + 1; }
            // share\ never holds a GDT but a first model preview writes dozens of files there: always ignored
            if (std::none_of(g_ignore.begin(), g_ignore.end(), [](const std::wstring& d) { return _wcsicmp(d.c_str(), L"share") == 0; })) g_ignore.emplace_back(L"share");
            g_gdtOnly = rp::ConfigGet("commit", "gdt_only", "1") != "0";
            // The host's GDT check (src/ape_gdtscan.cpp) covers the gap before this watcher, so the first rescan has nothing to find.
            wchar_t hostCheck[8] = {};
            bool covered = GetEnvironmentVariableW(L"RADIANTPLUGINS_APE_GDT_CHECK", hostCheck, 8) && hostCheck[0] == L'1';
            g_watchStop = false; g_watchAlive = true; g_changed = !covered;
            g_watchThread = std::thread([] { __try { WatchMain(); } __except (EXCEPTION_EXECUTE_HANDLER) { g_watchAlive = false; g_changed = true; } });
            rp::Log("commit: watching %S for changes (%zu ignored top-level folders, %s%s)", root, g_ignore.size(), g_gdtOnly ? "only .gdt files and folders count" : "every change counts",
                    covered ? ", startup covered by the host's GDT check" : "");
            return true;
        }
        void StopWatcher()
        {
            g_watchStop = true;
            if (g_watchDir != INVALID_HANDLE_VALUE) { CancelIoEx(g_watchDir, nullptr); CloseHandle(g_watchDir); g_watchDir = INVALID_HANDLE_VALUE; }
            if (g_watchThread.joinable()) g_watchThread.join();
            g_watchAlive = false;
        }

        void __fastcall hk_Refresh(void* a, void* b, void* c, void* d)
        {
            uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
            bool fromTimer = g_slotBegin && ret >= g_slotBegin && ret < g_slotEnd;
            if (g_callers.size() < 16 && std::find(g_callers.begin(), g_callers.end(), ret) == g_callers.end())
            {
                g_callers.push_back(ret);
                HMODULE m = nullptr; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(ret), &m);
                rp::Log("commit: file scan called from +0x%llx%s", (unsigned long long)(ret - uintptr_t(m)), fromTimer ? " (the commit timer)" : "");
            }
            if (fromTimer)
            {
                // with the watcher: skip while nothing changed on disk, without it: a time throttle
                if (g_watchAlive) { if (!g_changed) { ++g_skipped; return; } }
                else if (g_minS > 0 && g_lastRun && Ms(Now() - g_lastRun) < 1000.0 * g_minS) { ++g_skipped; return; }
            }
            bool hadChange = g_changed.exchange(false);   // changes during the scan set it again
            // Name the file that triggered the scan (read before it, the watcher overwrites it): a .gdt rewritten while
            // APE has it open drives the scan into APE's reload path, which can crash on a stale entry.
            std::string change;
            if (hadChange) { std::lock_guard<std::mutex> lock(g_lastChangeMutex); change = ", disk changed: " + Narrow(g_lastChange); }
            uint64_t t0 = Now();
            g_orig(a, b, c, d);
            g_lastRun = Now(); g_lastMs = Ms(g_lastRun - t0); ++g_runs;
            if (g_lastMs > 50.0 || !fromTimer) rp::Log("commit: file scan ran in %.0f ms (%s%s)", g_lastMs, fromTimer ? "commit timer" : "other caller", change.c_str());
        }
        bool Install()
        {
            if (g_hooked || !rp::Ape()) return g_hooked;
            void* target = rp::Ape()->ApeAddress(rp::self, kRefresh);
            if (!target) return false;
            DWORD64 base = 0; PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(DWORD64(target), &base, nullptr);
            if (!fn || DWORD64(fn->BeginAddress) + base != DWORD64(target)) { rp::Log("commit: no function starts at the expected address, the scan throttle is off"); return false; }
            g_slotBegin = uintptr_t(rp::Ape()->ApeAddress(rp::self, kTimerSlotBegin)); g_slotEnd = uintptr_t(rp::Ape()->ApeAddress(rp::self, kTimerSlotEnd));
            if (!rp::host->Detour(rp::self, target, reinterpret_cast<void*>(&hk_Refresh), reinterpret_cast<void**>(&g_orig))) { rp::Log("commit: detour failed"); return false; }
            g_hooked = true;
            rp::Log("commit: hooked the file scan at %p (scan_min_s=%d)", target, g_minS);
            StartWatcher();
            return true;
        }
        bool InstallTrace()
        {
            if (g_origFind) return true;
            // the exe's import is bound to KernelBase (kernel32 forwards it), so hook the function there
            HMODULE k = GetModuleHandleW(L"kernelbase.dll");
            void* f = k ? reinterpret_cast<void*>(GetProcAddress(k, "FindFirstFileExA")) : nullptr;
            if (!f) { rp::Log("commit: no kernelbase!FindFirstFileExA"); return false; }
            if (!rp::host->Detour(rp::self, f, reinterpret_cast<void*>(&hk_FindFirstFileExA), reinterpret_cast<void**>(&g_origFind))) { rp::Log("commit: FindFirstFileExA detour failed"); return false; }
            return true;
        }
        std::string Status()
        {
            char buf[512];
            std::string last; { std::lock_guard<std::mutex> lock(g_lastChangeMutex); last = Narrow(g_lastChange); }
            snprintf(buf, sizeof buf, "hooked=%d watcher=%s changed=%d changes=%d last_change='%s' scan_min_s=%d runs=%d skipped=%d last_scan_ms=%.0f last_run_age_s=%.0f",
                     g_hooked ? 1 : 0, g_watchAlive ? "alive" : "off", g_changed ? 1 : 0, g_changeCount.load(), last.c_str(), g_minS, g_runs, g_skipped, g_lastMs, g_lastRun ? Ms(Now() - g_lastRun) / 1000.0 : -1.0);
            return buf;
        }
        std::string TraceReport()
        {
            std::lock_guard<std::mutex> lock(g_traceMutex);
            std::vector<std::pair<std::string, int>> v(g_traceDirs.begin(), g_traceDirs.end());
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            std::string out = "FindFirstFileExA calls: " + std::to_string(g_traceCalls.load()) + "\n";
            for (size_t i = 0; i < v.size() && i < 40; ++i) out += "  " + std::to_string(v[i].second) + "  " + v[i].first + "\n";
            return out;
        }
    }

    // ---- Feature 5: the preview frame rate cap, the frame function 0x14012A0D0 re-queues itself at ~1000 fps ----
    namespace preview
    {
        const uintptr_t kFrame = 0x14012A0D0;        // (preview widget*, ?): renders one frame, re-queues itself while playing
        const uintptr_t kRequeueRet = 0x14012A289;   // return address of its QTimer::singleShot(0, this, "update()") call
        using FrameFn = void (__fastcall*)(void*, void*);
        using SingleShotFn = void (__fastcall*)(int, const void*, const char*);
        FrameFn g_orig = nullptr;
        SingleShotFn g_origShot = nullptr;
        uintptr_t g_requeueRet = 0;
        bool g_hooked = false;
        int g_maxFps = 60;          // [preview] max_fps, 0 = APE's own (uncapped)
        uint64_t g_last = 0;        // Now() at the start of the last frame
        uint64_t g_frames = 0, g_paced = 0;
        int g_trace = 0;            // "preview trace <n>": log the next n re-queues

        double g_avgMs = 0;         // running average of the time between frames (Status shows it as fps)
        void __fastcall hk_Frame(void* widget, void* a2)
        {
            const uint64_t now = Now();
            if (g_last) { const double d = Ms(now - g_last); if (d < 500.0) g_avgMs = g_avgMs > 0 ? g_avgMs * 0.95 + d * 0.05 : d; }
            g_last = now; ++g_frames;
            ba::previewfix::BeforeFrame(widget);
            ba::weaponpreview::BeforeFrame();
            g_orig(widget, a2);
            ba::previewfix::AfterFrame();   // first: it puts back the attached gun's anim, the weapon preview reads its tags
            ba::weaponpreview::AfterFrame();
        }
        // every QTimer::singleShot in the process passes here, only the frame function's re-queue is changed
        void __fastcall hk_SingleShot(int msec, const void* receiver, const char* member)
        {
            if (msec == 0 && g_maxFps > 0 && reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_requeueRet)
            {
                // Qt's timers land on Windows' 15.6 ms tick (timeBeginPeriod does not change that), so ask 2 ms early: 60 -> ~64 fps
                const double interval = 1000.0 / double(g_maxFps), spent = Ms(Now() - g_last);
                const int wait = spent + 2.0 < interval ? int(interval - spent) - 2 : 0;
                if (wait > 0)
                {
                    ++g_paced;
                    if (g_trace > 0) { --g_trace; rp::Log("preview: frame took %.1f ms, next in %d ms", spent, wait); }
                    msec = wait;
                }
            }
            g_origShot(msec, receiver, member);
        }
        // The renderer starts on a background thread and queues GfxInitDone() on the preview widget when ready.
        // A tree selection made before that is dropped, so the widget's moc dispatcher is hooked and the tick
        // re-emits itemSelectionChanged() once after GfxInitDone() ran.
        using MetacallFn = void (__fastcall*)(void*, int, int, void**);
        MetacallFn g_origMeta = nullptr;
        int g_gfxDoneId = -1;                     // GfxInitDone()'s local method index in apePreviewWidget
        bool g_metaDone = false;                  // hook installed, or given up
        std::atomic<uint64_t> g_gfxDoneAt{ 0 };   // Now() when GfxInitDone() ran
        bool g_reselected = false;
        void (*g_slotHook)(void* widget, int id, bool after) = nullptr;   // sky: the lighting slots and the Rendering menu
        void __fastcall hk_Metacall(void* obj, int call, int id, void** args)
        {
            if (call == 0 && g_slotHook) g_slotHook(obj, id, false);
            g_origMeta(obj, call, id, args);
            if (call == 0 && g_slotHook) g_slotHook(obj, id, true);
            if (call == 0 /* QMetaObject::InvokeMetaMethod */ && id == g_gfxDoneId && !g_gfxDoneAt) g_gfxDoneAt = Now();
        }
        void HookGfxInitDone()
        {
            if (g_metaDone || !qt.metaIndexOfSlot || !qt.metaMethodOffset) return;
            void* widget = nullptr;
            ForEachWidget([&](void* w) { if (strcmp(ClassName(w), "apePreviewWidget") == 0) { widget = w; return true; } return false; });
            if (!widget) return;                  // not built yet: try again on a later tick
            g_metaDone = true;
            const QMetaObjectRaw* mo = MetaOf(widget);
            const int idx = qt.metaIndexOfSlot(mo, "GfxInitDone()"), off = qt.metaMethodOffset(mo);
            void* dispatch = *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(mo) + 0x18);   // QMetaObject::d.static_metacall
            HMODULE owner = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(dispatch), &owner);
            if (idx < off || !dispatch || owner != GetModuleHandleW(nullptr)) { rp::Log("preview: GfxInitDone() not found (index %d, offset %d), the startup selection fix is off", idx, off); return; }
            g_gfxDoneId = idx - off;
            if (!rp::host->Detour(rp::self, dispatch, reinterpret_cast<void*>(&hk_Metacall), reinterpret_cast<void**>(&g_origMeta))) { rp::Log("preview: dispatcher detour failed"); return; }
            rp::Log("preview: startup selection fix installed (GfxInitDone() = method %d)", g_gfxDoneId);
        }
        void Tick()
        {
            HookGfxInitDone();
            const uint64_t done = g_gfxDoneAt;
            if (g_reselected || !done || Ms(Now() - done) < 100.0) return;
            g_reselected = true;
            void* tree = FindTree();
            if (tree && qt.treeItemSelectionChanged) { qt.treeItemSelectionChanged(tree); rp::Log("preview: renderer ready, the current tree selection shown again"); }
        }

        void Install()
        {
            if (g_hooked || !rp::Ape()) return;
            void* target = rp::Ape()->ApeAddress(rp::self, kFrame);
            const uint8_t* call = static_cast<const uint8_t*>(rp::Ape()->ApeAddress(rp::self, kRequeueRet - 8));   // 33 C9 = xor ecx,ecx, FF 15 = call [rip+IAT]
            DWORD64 base = 0; PRUNTIME_FUNCTION fn = target ? RtlLookupFunctionEntry(DWORD64(target), &base, nullptr) : nullptr;
            if (!fn || DWORD64(fn->BeginAddress) + base != DWORD64(target) || !call || call[0] != 0x33 || call[1] != 0xC9 || call[2] != 0xFF || call[3] != 0x15)
            { rp::Log("preview: frame function not found, no frame cap"); return; }
            HMODULE core = GetModuleHandleW(L"Qt5Core64r.dll");
            void* shot = core ? reinterpret_cast<void*>(GetProcAddress(core, "?singleShot@QTimer@@SAXHPEBVQObject@@PEBD@Z")) : nullptr;
            if (!shot) { rp::Log("preview: QTimer::singleShot not found, no frame cap"); return; }
            g_requeueRet = reinterpret_cast<uintptr_t>(call) + 8;
            if (!rp::host->Detour(rp::self, target, reinterpret_cast<void*>(&hk_Frame), reinterpret_cast<void**>(&g_orig))
                || !rp::host->Detour(rp::self, shot, reinterpret_cast<void*>(&hk_SingleShot), reinterpret_cast<void**>(&g_origShot)))
            { rp::Log("preview: detour failed"); return; }
            g_hooked = true;
            rp::Log("preview: frame cap installed (max_fps=%d)", g_maxFps);
        }
        std::string Status()
        {
            char buf[256];
            snprintf(buf, sizeof buf, "preview: hooked=%d max_fps=%d frames=%llu paced=%llu, measured %.0f fps (%.1f ms)", g_hooked ? 1 : 0, g_maxFps, (unsigned long long)g_frames, (unsigned long long)g_paced,
                     g_avgMs > 0 ? 1000.0 / g_avgMs : 0.0, g_avgMs);
            return buf;
        }
    }

    // Feature 7: Custom Sky in the preview's Rendering menu.
    // While it is on, kReadSsi is wrapped to read the sky's own ssi record for all four lighting states.
    // APE rebuilds the menu on every opening, so the entry is re-inserted after that slot via preview::g_slotHook.
    namespace sky
    {
        const uintptr_t kReadSsi = 0x140270AA0;      // (const char* ssi name, Ssi* out): one ssi record from gdt.db
        const uintptr_t kReload = 0x140147250;       // (previewWindowOpts*): re-reads the lighting states (the Lighting slots call it)
        const uintptr_t kOpts = 0x14118D620;         // previewWindowOpts (pointer)
        const size_t kSsiSky = 32, kSkyCap = 64;     // Ssi: skyboxmodel
        const size_t kOptLit = 0x668, kOptWire = 0x669, kOptDrawSky = 0x66A, kOptState = 0x670;   // real lighting, wireframe, Draw Skybox, state 0..3
        const size_t kOptStates = 0x254, kSsiSize = 0xF4;                        // the four Ssi structs
        const char* const kNoRecordLight = "default_day";                        // the light of a sky no ssi record uses
        using ReadSsiFn = uintptr_t (__fastcall*)(const char*, void*);
        using ReloadFn = void (__fastcall*)(void*);
        ReadSsiFn g_orig = nullptr;
        ReloadFn g_reload = nullptr;
        void** g_opts = nullptr;
        bool g_hooked = false;
        std::mutex g_mutex;
        bool g_on = false;                           // the custom sky is the lighting (read on GfxInit's thread too, under g_mutex)
        char g_sky[kSkyCap] = {}, g_ssi[kSkyCap] = {};   // its xmodel and the ssi record read for it
        std::atomic<uint64_t> g_swaps{ 0 };
        std::string g_pick;                          // [preview] skybox: the last custom sky (kept while a preset is on)
        std::map<std::string, std::string> g_ssiOf;  // skybox xmodel (lower case) -> the ssi record that uses it (gdt.db)
        bool g_ssiLoaded = false;
        bool g_startupDone = false;
        uint64_t g_lastCheck = 0, g_loadedAt = 0;
        // the Rendering menu entry
        int g_idMenu = -1, g_idPreset[6] = { -1, -1, -1, -1, -1, -1 };   // apePreviewWidget local method indices
        void* g_action = nullptr;                    // "Custom Sky...", parented to the preview widget so the menu's clear() keeps it
        struct QtFns
        {
            void* (__fastcall* sender)(const void*) = nullptr;
            void (__fastcall* setParent)(void*, void*) = nullptr;
            void (__fastcall* setCheckable)(void*, bool) = nullptr;
            void (__fastcall* setChecked)(void*, bool) = nullptr;
            void* (__fastcall* actionGroup)(const void*) = nullptr;
            void* (__fastcall* groupAdd)(void*, void*) = nullptr;
            void (__fastcall* insertAction)(void*, void*, void*) = nullptr;
            void (__fastcall* removeAction)(void*, void*) = nullptr;
            void (__fastcall* update)(void*) = nullptr;
            bool ok = false;
        } f;
        // the picker
        void* g_dlg = nullptr; void* g_filter = nullptr; void* g_list = nullptr;
        std::vector<std::string> g_all, g_rows;      // every sky, the rows the filter shows
        std::string g_lastFilter = "\x01";
        bool g_filling = false;

        std::string Lower(std::string s) { for (char& ch : s) ch = char(tolower(static_cast<unsigned char>(ch))); return s; }

        uintptr_t __fastcall hk_ReadSsi(const char* name, void* out)
        {
            bool on; char ssi[kSkyCap] = {}, sky[kSkyCap] = {};
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                on = g_on && g_sky[0]; memcpy(ssi, g_ssi, kSkyCap); memcpy(sky, g_sky, kSkyCap);
            }
            if (!on || !out) return g_orig(name, out);
            const uintptr_t r = g_orig(ssi[0] ? ssi : kNoRecordLight, out);   // the sky's own light for every state
            strncpy_s(static_cast<char*>(out) + kSsiSky, kSkyCap, sky, _TRUNCATE);
            ++g_swaps;
            return r;
        }

        void* Opts() { return g_opts ? *g_opts : nullptr; }
        char* Opt(void* opts, size_t off) { return static_cast<char*>(opts) + off; }
        char* StateSky(void* opts, int state) { return Opt(opts, kOptStates + kSsiSize * size_t(state) + kSsiSky); }
        int State(void* opts) { const int s = *reinterpret_cast<int*>(Opt(opts, kOptState)); return s >= 0 && s < 4 ? s : 0; }

        void* PreviewWidget()
        {
            void* widget = nullptr;
            ForEachWidget([&](void* w) { if (strcmp(ClassName(w), "apePreviewWidget") == 0) { widget = w; return true; } return false; });
            return widget;
        }
        // a slot of apePreviewWidget by signature, through its moc dispatcher
        bool InvokeSlot(void* widget, const char* signature)
        {
            if (!widget || !qt.metaIndexOfSlot || !qt.metaMethodOffset) return false;
            const QMetaObjectRaw* mo = MetaOf(widget);
            const int idx = qt.metaIndexOfSlot(mo, signature), off = qt.metaMethodOffset(mo);
            auto dispatch = *reinterpret_cast<void (__fastcall* const*)(void*, int, int, void**)>(reinterpret_cast<const char*>(mo) + 0x18);
            if (idx < off || !dispatch) return false;
            void* args[1] = { nullptr };
            dispatch(widget, 0 /* InvokeMetaMethod */, idx - off, args);
            return true;
        }

        // skybox -> ssi record from gdt.db, when several records use one sky the one named after it wins, else the first by name
        void LoadSsiMap()
        {
            if (g_ssiLoaded) return;
            g_ssiLoaded = true;
            HMODULE sq = GetModuleHandleW(L"sqlite64r.dll");
            if (!sq || !rp::Ape()) return;
            using OpenFn = int (*)(const char*, void**, int, const char*); using PrepFn = int (*)(void*, const char*, int, void**, const char**);
            using StepFn = int (*)(void*); using ColTextFn = const unsigned char* (*)(void*, int); using FinFn = int (*)(void*); using CloseFn = int (*)(void*);
            auto open = reinterpret_cast<OpenFn>(GetProcAddress(sq, "sqlite3_open_v2")); auto prep = reinterpret_cast<PrepFn>(GetProcAddress(sq, "sqlite3_prepare_v2"));
            auto step = reinterpret_cast<StepFn>(GetProcAddress(sq, "sqlite3_step")); auto colText = reinterpret_cast<ColTextFn>(GetProcAddress(sq, "sqlite3_column_text"));
            auto fin = reinterpret_cast<FinFn>(GetProcAddress(sq, "sqlite3_finalize")); auto close = reinterpret_cast<CloseFn>(GetProcAddress(sq, "sqlite3_close"));
            if (!open || !prep || !step || !colText || !fin || !close) return;
            wchar_t pathW[1024] = {}; rp::Ape()->GdtDbPath(rp::self, pathW, 1024);
            char path[2048] = {}; WideCharToMultiByte(CP_UTF8, 0, pathW, -1, path, sizeof path, nullptr, nullptr);
            void* db = nullptr;
            if (open(path, &db, 0x00000001 | 0x00010000 /* READONLY | FULLMUTEX */, nullptr) != 0) { if (db) close(db); rp::Log("sky: gdt.db not opened, custom skies use %s's light", kNoRecordLight); return; }
            void* st = nullptr;
            if (prep(db, "SELECT _name, skyboxmodel FROM ssi ORDER BY _name", -1, &st, nullptr) == 0)
            {
                while (step(st) == 100)
                {
                    const char* n = reinterpret_cast<const char*>(colText(st, 0)); const char* m = reinterpret_cast<const char*>(colText(st, 1));
                    if (!n || !m || !*n || !*m) continue;
                    const std::string ssi = n, sky = Lower(m), l = Lower(ssi);
                    const bool named = sky == l + "_skybox" || sky == "skybox_" + l;
                    if (g_ssiOf.find(sky) == g_ssiOf.end() || named) g_ssiOf[sky] = ssi;
                }
                fin(st);
            }
            close(db);
            rp::Log("sky: %zu skyboxes have an ssi record (their own light)", g_ssiOf.size());
        }
        std::string SsiOf(const std::string& sky)
        {
            LoadSsiMap();
            auto it = g_ssiOf.find(Lower(sky));
            return it == g_ssiOf.end() ? std::string() : it->second;
        }
        bool LooksLikeSky(const std::string& n)
        {
            const std::string l = Lower(n);
            return l.find("skybox") != std::string::npos || l.rfind("sky_", 0) == 0 || (l.size() > 4 && l.compare(l.size() - 4, 4, "_sky") == 0);
        }
        // every xmodel in gdt.db named like a skybox, plus every sky an ssi record uses
        std::vector<std::string> ListSkies()
        {
            std::vector<std::string> out;
            const ModToolsApeApi* ape = rp::Ape();
            if (ape && reinterpret_cast<const char*>(&ape->FindAssets) < reinterpret_cast<const char*>(ape) + ape->size && ape->FindAssets)
            {
                std::vector<ModToolsApeAsset> rows(4000);
                const size_t n = ape->FindAssets(rp::self, "sky", "xmodel", rows.data(), rows.size());
                for (size_t i = 0; i < n && i < rows.size(); ++i) if (LooksLikeSky(rows[i].name)) out.push_back(rows[i].name);
            }
            LoadSsiMap();
            for (auto& kv : g_ssiOf) out.push_back(kv.first);
            std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
            out.erase(std::unique(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) == 0; }), out.end());
            return out;
        }

        // the lighting states read again (main thread, what a Lighting slot does after setting its state)
        bool Reapply(bool showSky)
        {
            void* opts = Opts();
            if (!g_hooked || !opts) return false;
            g_reload(opts);
            void* widget = PreviewWidget();
            if (showSky && !*Opt(opts, kOptDrawSky)) InvokeSlot(widget, "DrawSkybox()");   // a picked sky is meant to be seen
            if (widget && f.update) f.update(widget);
            return true;
        }
        void Store(bool on, const std::string& sky)
        {
            const std::string ssi = sky.empty() ? std::string() : SsiOf(sky);
            std::lock_guard<std::mutex> lock(g_mutex);
            g_on = on && !sky.empty();
            strncpy_s(g_sky, sky.c_str(), _TRUNCATE);
            strncpy_s(g_ssi, ssi.c_str(), _TRUNCATE);
        }
        std::string LightOf(const std::string& sky) { const std::string s = SsiOf(sky); return s.empty() ? std::string(kNoRecordLight) + " (no ssi record uses this sky)" : s; }
        // the custom sky becomes the lighting
        void Activate(const std::string& sky)
        {
            if (sky.empty()) return;
            g_pick = sky;
            Store(true, sky);
            rp::ConfigSet("preview", "skybox", sky);
            rp::ConfigSet("preview", "skybox_on", "1");
            if (void* opts = Opts()) { *Opt(opts, kOptLit) = 1; *Opt(opts, kOptWire) = 0; }   // real lighting, as the preset slots set it
            const bool live = Reapply(true);
            rp::Log("sky: custom sky %s, light from %s%s", sky.c_str(), LightOf(sky).c_str(), live ? "" : " (applies when the preview starts)");
        }
        // a preset (or Wireframe / No Lighting) was picked: its slot reloads the states, now without the custom sky
        void Deactivate(bool reload)
        {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (!g_on) return;
                g_on = false;
            }
            rp::ConfigSet("preview", "skybox_on", "0");
            if (reload) Reapply(false);
            rp::Log("sky: custom sky off, the preset lighting is back");
        }
        bool IsOn() { std::lock_guard<std::mutex> lock(g_mutex); return g_on; }

        // ---- the picker window: filter + list, a click applies the sky at once ----
        void FillList()
        {
            if (!g_list) return;
            const auto* ui = rp::host->ui;
            std::string want;
            if (g_filter) { char buf[256] = {}; ui->EditText(g_filter, buf, sizeof buf); want = Lower(buf); }
            g_lastFilter = want;
            g_filling = true;
            ui->ListClear(g_list);
            g_rows.clear();
            int current = -1;
            for (auto& s : g_all)
            {
                if (!want.empty() && Lower(s).find(want) == std::string::npos) continue;
                ui->ListAdd(g_list, s.c_str());
                if (s == g_pick) current = int(g_rows.size());
                g_rows.push_back(s);
            }
            if (current >= 0) ui->ListSetCurrentRow(g_list, current);
            g_filling = false;
        }
        void OnListRow(void*, int row)
        {
            if (g_filling || row < 0 || size_t(row) >= g_rows.size()) return;
            if (IsOn() && g_rows[size_t(row)] == g_pick) return;
            Activate(g_rows[size_t(row)]);
        }
        void ShowPicker()
        {
            const auto* ui = rp::host->ui;
            if (!ui || !rp::Has(&rp::host->CreateDialogWindow) || !rp::Has(&rp::host->OnChanged)) return;
            if (!g_dlg)
            {
                void* content = ui->Widget(nullptr);
                void* box = ui->VBox(content);
                ui->LayoutMargins(box, 8, 8, 8, 8);
                g_filter = ui->LineEdit("", content);
                ui->SetPlaceholder(g_filter, "Filter skyboxes");
                g_list = ui->ListWidget(content);
                ui->BoxAddWidget(box, g_filter, 0);
                ui->BoxAddWidget(box, g_list, 1);
                g_dlg = rp::host->CreateDialogWindow(rp::self, "Custom Sky (APE+)", content, 420, 560);
                rp::host->OnChanged(rp::self, g_list, OnListRow, nullptr);
            }
            if (!g_dlg) return;
            g_all = ListSkies();
            FillList();
            ui->SetVisible(g_dlg, 1);
        }
        void OnCustomSky(void*)
        {
            // the click checked the entry (exclusive group) before anything changed: a remembered sky turns on right away,
            // the window picks another
            if (!g_pick.empty() && !IsOn()) Activate(g_pick);
            ShowPicker();
        }

        // ---- the Rendering menu ----
        bool BindFns()
        {
            if (f.ok) return true;
            HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll"), c = GetModuleHandleW(L"Qt5Core64r.dll");
            if (!w || !c) return false;
            bool ok = Get(f.sender, c, "?sender@QObject@@IEBAPEAV1@XZ") && Get(f.setParent, c, "?setParent@QObject@@QEAAXPEAV1@@Z")
                   && Get(f.setCheckable, w, "?setCheckable@QAction@@QEAAX_N@Z") && Get(f.setChecked, w, "?setChecked@QAction@@QEAAX_N@Z")
                   && Get(f.actionGroup, w, "?actionGroup@QAction@@QEBAPEAVQActionGroup@@XZ") && Get(f.groupAdd, w, "?addAction@QActionGroup@@QEAAPEAVQAction@@PEAV2@@Z")
                   && Get(f.insertAction, w, "?insertAction@QWidget@@QEAAXPEAVQAction@@0@Z") && Get(f.removeAction, w, "?removeAction@QWidget@@QEAAXPEAVQAction@@@Z");
            Get(f.update, w, "?update@QWidget@@QEAAXXZ");
            f.ok = ok;
            if (!ok) rp::Log("sky: Qt exports for the menu entry missing, no Custom Sky entry (command 'sky <name>' still works)");
            return ok;
        }
        void ResolveIds(void* widget)
        {
            if (g_idMenu >= 0 || !qt.metaIndexOfSlot || !qt.metaMethodOffset) return;
            const QMetaObjectRaw* mo = MetaOf(widget);
            const int off = qt.metaMethodOffset(mo);
            auto id = [&](const char* sig) { const int i = qt.metaIndexOfSlot(mo, sig); return i >= off ? i - off : -1; };
            static const char* const presets[6] = { "LightingWireframe()", "LightingNone()", "LightingMorning()", "LightingDay()", "LightingSunset()", "LightingNight()" };
            for (int i = 0; i < 6; ++i) g_idPreset[i] = id(presets[i]);
            g_idMenu = id("LightingMenuAboutToShow()");
            rp::Log("sky: Rendering menu slot %d, lighting slots %d %d %d %d %d %d", g_idMenu, g_idPreset[0], g_idPreset[1], g_idPreset[2], g_idPreset[3], g_idPreset[4], g_idPreset[5]);
        }
        // APE has just rebuilt the menu (sender): Custom Sky goes in after Night, in the lightings' exclusive group
        void DecorateMenu(void* widget, void* menu)
        {
            if (!menu || !BindFns() || !rp::host->ui) return;
            QListRaw acts = {}; qt.widgetActions(menu, &acts);
            void* night = nullptr; void* after = nullptr;
            if (acts.d)
                for (int i = acts.d->begin; i < acts.d->end; ++i)
                {
                    void* a = acts.d->array[i];
                    if (night) { after = a; break; }
                    if (a && TakeString(qt.actionText, a) == "Night") night = a;
                }
            ListRelease(acts);
            if (!night) return;
            if (!g_action)
            {
                g_action = rp::host->AddMenuItem(rp::self, menu, "Custom Sky...", OnCustomSky, nullptr);   // appended, parented to the menu
                if (!g_action) return;
                f.setParent(g_action, widget);       // the menu's clear() on the next opening must not delete it
                f.setCheckable(g_action, true);
            }
            f.removeAction(menu, g_action);
            f.insertAction(menu, after, g_action);   // after == nullptr appends
            if (void* group = f.actionGroup(night)) f.groupAdd(group, g_action);
            const bool on = IsOn();
            rp::host->ui->SetActionText(g_action, on ? ("Custom Sky: " + g_pick + "...").c_str() : "Custom Sky...");
            f.setChecked(g_action, on);              // on: the group unchecks the preset APE checked
        }
        // every apePreviewWidget slot call passes here (preview::hk_Metacall), after = the slot has run
        void OnSlot(void* widget, int id, bool after)
        {
            ResolveIds(widget);
            if (!after)
            {
                for (int i = 0; i < 6; ++i) if (id == g_idPreset[i] && id >= 0) { Deactivate(false); return; }   // the slot reloads the states itself
                return;
            }
            if (id == g_idMenu && id >= 0 && BindFns()) DecorateMenu(widget, f.sender(widget));
        }

        void Tick()
        {
            if (!g_hooked) return;
            if (g_dlg && g_filter && qt.isVisible(g_dlg))     // live filter
            {
                char buf[256] = {}; rp::host->ui->EditText(g_filter, buf, sizeof buf);
                if (Lower(buf) != g_lastFilter) FillList();
            }
            // the states may have been read before the plugin loaded (a hot reload, possibly with an older custom sky in them):
            // once the preview is up, read them again
            if (g_startupDone) return;
            const uint64_t now = Now();
            if (g_lastCheck && Ms(now - g_lastCheck) < 500.0) return;
            g_lastCheck = now;
            void* opts = Opts();
            if (preview::g_frames == 0 && !(opts && *StateSky(opts, State(opts)) && Ms(now - g_loadedAt) > 3000.0)) return;
            if (!opts) return;
            g_startupDone = true;
            if (IsOn()) { *Opt(opts, kOptLit) = 1; *Opt(opts, kOptWire) = 0; }
            Reapply(false);
        }

        void Install()
        {
            if (g_hooked || !rp::Ape()) return;
            void* read = rp::Ape()->ApeAddress(rp::self, kReadSsi);
            void* reload = rp::Ape()->ApeAddress(rp::self, kReload);
            g_opts = static_cast<void**>(rp::Ape()->ApeAddress(rp::self, kOpts));
            auto isEntry = [](void* p) { DWORD64 base = 0; PRUNTIME_FUNCTION fn = p ? RtlLookupFunctionEntry(DWORD64(p), &base, nullptr) : nullptr; return fn && DWORD64(fn->BeginAddress) + base == DWORD64(p); };
            if (!isEntry(read) || !isEntry(reload) || !g_opts) { rp::Log("sky: APE's lighting code not found, no custom sky"); g_opts = nullptr; return; }
            g_reload = reinterpret_cast<ReloadFn>(reload);
            if (!rp::host->Detour(rp::self, read, reinterpret_cast<void*>(&hk_ReadSsi), reinterpret_cast<void**>(&g_orig))) { rp::Log("sky: detour failed"); return; }
            BindFns();
            preview::g_slotHook = &OnSlot;
            g_hooked = true;
            g_loadedAt = Now();
            rp::Log("sky: installed (custom sky %s%s)", g_pick.empty() ? "none" : g_pick.c_str(), IsOn() ? ", on" : ", off");
        }
        void Uninstall()
        {
            preview::g_slotHook = nullptr;
            if (g_action && qt.deleteLater) qt.deleteLater(g_action);   // only in a closed menu here, APE's next clear() drops it anyway
            g_action = nullptr;
            g_dlg = g_filter = g_list = nullptr;                // the host hides and deletes the window
        }
        std::string Status()
        {
            char buf[512];
            void* opts = Opts();
            float sun[2] = {};                        // the current state's Ssi +0 / +4 (pitch, yaw)
            if (opts) memcpy(sun, StateSky(opts, State(opts)) - kSsiSky, sizeof sun);
            std::string ssi; { std::lock_guard<std::mutex> lock(g_mutex); ssi = g_ssi; }
            snprintf(buf, sizeof buf, "sky: hooked=%d custom=%s pick='%s' ssi='%s' swaps=%llu menu=%s; preview %s, lighting %s wire %s state %d sky '%s' sun %.1f/%.1f, Draw Skybox %s",
                     g_hooked ? 1 : 0, IsOn() ? "on" : "off", g_pick.c_str(), ssi.c_str(), (unsigned long long)g_swaps.load(), g_idMenu >= 0 ? (g_action ? "entry" : "slot found") : "not yet",
                     opts ? "up" : "not started", opts ? (*Opt(opts, kOptLit) ? "on" : "off") : "-", opts ? (*Opt(opts, kOptWire) ? "on" : "off") : "-", opts ? State(opts) : -1,
                     opts ? StateSky(opts, State(opts)) : "", sun[0], sun[1], opts ? (*Opt(opts, kOptDrawSky) ? "on" : "off") : "-");
            return buf;
        }
    }

    // First-time model conversion: kBuildSurfaces buckets the face indices per (material, LOD)
    // once, kBuildOne gets a face array holding only its bucket (the face vector pointed at a copy for the call, then restored).
    namespace convert
    {
        const uintptr_t kBuildSurfaces = 0x14067AE20;   // (surfaces*, model**): every LOD x material of the model
        const uintptr_t kBuildOne = 0x140679C70;        // (surfaces*, model**, material, lod): one material of one LOD
        const size_t kFace = 116;                       // face record: dword [0] material, [1] LOD
        const size_t kFaces = 56;                       // model: std::vector of face records (begin, end, capacity)
        using SurfacesFn = char (__fastcall*)(void*, uint8_t**);
        using OneFn = uint8_t (__fastcall*)(void*, uint8_t**, unsigned, unsigned);
        SurfacesFn g_origSurfaces = nullptr;
        OneFn g_origOne = nullptr;
        bool g_hooked = false, g_on = true;     // [preview] fast_convert
        // the LODs of a model are built in parallel on worker threads (the main thread waits): per-thread context, atomic counters
        std::atomic<uint64_t> g_models{ 0 }, g_calls{ 0 }, g_moved{ 0 }, g_totalUs{ 0 }, g_maxUs{ 0 };
        void Count(size_t faces, size_t materials, uint64_t t0, const char* how)
        {
            const double ms = Ms(Now() - t0);
            const uint64_t us = uint64_t(ms * 1000.0);
            ++g_models; g_totalUs += us;
            for (uint64_t m = g_maxUs; us > m && !g_maxUs.compare_exchange_weak(m, us);) {}
            rp::Log("convert: %s surface build, %zu faces, %zu materials, %.0f ms", how, faces, materials, ms);
        }

        struct Ctx
        {
            uint8_t* model = nullptr; uint8_t* begin = nullptr; size_t count = 0;
            std::unordered_map<uint64_t, std::vector<uint32_t>> buckets;
        };
        thread_local Ctx* t_ctx = nullptr;
        uint64_t Key(uint32_t material, uint32_t lod) { return (uint64_t(material) << 32) | lod; }
        uint8_t** FaceVec(uint8_t* model) { return reinterpret_cast<uint8_t**>(model + kFaces); }

        char __fastcall hk_BuildSurfaces(void* surfaces, uint8_t** pmodel)
        {
            uint8_t* model = pmodel ? *pmodel : nullptr;
            if (!model || t_ctx) return g_origSurfaces(surfaces, pmodel);
            uint8_t** v = FaceVec(model);
            const size_t bytes = size_t(v[1] - v[0]);
            const uint64_t t0 = Now();
            if (!g_on || !v[0] || bytes % kFace)   // stock (timed, so "convert 0" gives the comparison)
            {
                const size_t materials = size_t(v[4] - v[3]) / 128;
                const char r = g_origSurfaces(surfaces, pmodel);
                Count(bytes / kFace, materials, t0, "stock");
                return r;
            }
            Ctx ctx; ctx.model = model; ctx.begin = v[0]; ctx.count = bytes / kFace;
            for (size_t i = 0; i < ctx.count; ++i)
            {
                const uint32_t* f = reinterpret_cast<const uint32_t*>(ctx.begin + i * kFace);
                ctx.buckets[Key(f[0], f[1])].push_back(uint32_t(i));
            }
            struct Scope { Scope(Ctx* c) { t_ctx = c; } ~Scope() { t_ctx = nullptr; } } scope(&ctx);
            const size_t materials = size_t(v[4] - v[3]) / 128;
            const char r = g_origSurfaces(surfaces, pmodel);
            Count(ctx.count, materials, t0, "bucketed");
            return r;
        }

        uint8_t __fastcall hk_BuildOne(void* surfaces, uint8_t** pmodel, unsigned material, unsigned lod)
        {
            Ctx* c = t_ctx;
            uint8_t* model = pmodel ? *pmodel : nullptr;
            if (!c || model != c->model) return g_origOne(surfaces, pmodel, material, lod);
            uint8_t** v = FaceVec(model);
            if (v[0] != c->begin || size_t(v[1] - v[0]) != c->count * kFace) return g_origOne(surfaces, pmodel, material, lod);   // the face array changed: stock path
            ++g_calls;
            const uint64_t key = Key(material, lod);
            std::vector<uint32_t> mine;
            auto it = c->buckets.find(key);
            if (it != c->buckets.end()) mine.swap(it->second);
            std::vector<uint8_t> copy(mine.size() * kFace);
            for (size_t k = 0; k < mine.size(); ++k) memcpy(&copy[k * kFace], c->begin + size_t(mine[k]) * kFace, kFace);
            // the face vector points at the copy for the call, restored (and the copy written back) even if it throws
            struct Swap
            {
                uint8_t** v; uint8_t* saved[3]; const std::vector<uint32_t>& idx; std::vector<uint8_t>& copy; uint8_t* begin;
                ~Swap()
                {
                    v[0] = saved[0]; v[1] = saved[1]; v[2] = saved[2];
                    for (size_t k = 0; k < idx.size(); ++k) memcpy(begin + size_t(idx[k]) * kFace, &copy[k * kFace], kFace);
                }
            } swap{ v, { v[0], v[1], v[2] }, mine, copy, c->begin };
            uint8_t* first = copy.empty() ? nullptr : copy.data();
            v[0] = first; v[1] = v[2] = first ? first + copy.size() : nullptr;
            const uint8_t r = g_origOne(surfaces, pmodel, material, lod);
            // faces the call moved to another material go to that bucket (still in face order)
            std::vector<uint32_t> kept;
            kept.reserve(mine.size());
            for (size_t k = 0; k < mine.size(); ++k)
            {
                const uint32_t* f = reinterpret_cast<const uint32_t*>(&copy[k * kFace]);
                if (Key(f[0], f[1]) == key) { kept.push_back(mine[k]); continue; }
                std::vector<uint32_t>& to = c->buckets[Key(f[0], f[1])];
                if (!to.empty() && to.back() > mine[k]) { to.push_back(mine[k]); std::sort(to.begin(), to.end()); }
                else to.push_back(mine[k]);
                ++g_moved;
            }
            c->buckets[key] = std::move(kept);
            return r;
        }

        bool IsStart(void* p) { DWORD64 base = 0; PRUNTIME_FUNCTION fn = p ? RtlLookupFunctionEntry(DWORD64(p), &base, nullptr) : nullptr; return fn && DWORD64(fn->BeginAddress) + base == DWORD64(p); }
        void Install()
        {
            if (g_hooked || !rp::Ape()) return;
            void* surfaces = rp::Ape()->ApeAddress(rp::self, kBuildSurfaces);
            void* one = rp::Ape()->ApeAddress(rp::self, kBuildOne);
            if (!IsStart(surfaces) || !IsStart(one)) { rp::Log("convert: surface build not found, stock conversion"); return; }
            if (!rp::host->Detour(rp::self, surfaces, reinterpret_cast<void*>(&hk_BuildSurfaces), reinterpret_cast<void**>(&g_origSurfaces))
                || !rp::host->Detour(rp::self, one, reinterpret_cast<void*>(&hk_BuildOne), reinterpret_cast<void**>(&g_origOne)))
            { rp::Log("convert: detour failed"); return; }
            g_hooked = true;
            rp::Log("convert: bucketed surface build installed (%s)", g_on ? "on" : "off, fast_convert=0");
        }
        std::string Status()
        {
            char buf[256];
            snprintf(buf, sizeof buf, "convert: hooked=%d on=%d surface builds=%llu (material x LOD calls %llu, faces moved by a split %llu), longest %.0f ms, total %.0f ms",
                     g_hooked ? 1 : 0, g_on ? 1 : 0, (unsigned long long)g_models.load(), (unsigned long long)g_calls.load(), (unsigned long long)g_moved.load(),
                     g_maxUs / 1000.0, g_totalUs / 1000.0);
            return buf;
        }
    }

    // ---- autolod: an Output note when a preview auto LOD is past the stock linker's limits ----
    namespace autolod
    {
        const uintptr_t kReduce = 0x14067C300;   // (out, model**, lodSpec*, ?, error string*): one auto LOD
        const uintptr_t kPrint = 0x14002A0D6;    // PrintOutput(channel, format, ...), a jmp thunk
        const uintptr_t kSimplify = 0x140217930; // tfbDecimate simplifier, the linker's 0x7AE170
        const uintptr_t kCacheName = 0x14067D440, kCacheRead = 0x14067D4B0, kCacheWrite = 0x14067DBF0;   // "ToolsGfx/xmeshes/v39/<mesh>/pct%d_<key>"
        const uint8_t kSimplifyPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40 };
        const size_t kStockFaces = 220000, kStockBones = 1000;
        using ReduceFn = void* (__fastcall*)(void*, uint8_t**, float*, void*, void*);
        using PrintFn = void (*)(int, const char*, ...);   // APE's msvcr110 printf: no %zu
        ReduceFn g_orig = nullptr;
        PrintFn g_print = nullptr;
        std::mutex g_lock;
        std::unordered_set<uint64_t> g_told;   // faces << 20 | bones, one note per model
        using CacheNameFn = int (__fastcall*)(char*, size_t, const void*, int, const uint32_t*);
        using CacheReadFn = uint64_t (__fastcall*)(void*, const void*, int, const uint32_t*);
        using CacheWriteFn = void (__fastcall*)(const void*, int, const uint32_t*, void*);
        CacheNameFn g_name = nullptr;
        CacheReadFn g_read = nullptr;
        CacheWriteFn g_write = nullptr;
        std::wstring g_launcherIni;
        bool g_active = false;   // every detour live: keys and reduction switch together

        // the linker reduces with meshoptimizer: same keys BuildPlusLinker and the launcher bridge read
        bool Meshopt()
        {
            const wchar_t* ini = g_launcherIni.c_str();
            return g_active && GetPrivateProfileIntW(L"launcherplus", L"enabled", 1, ini) && GetPrivateProfileIntW(L"linker", L"enabled", 1, ini) &&
                   GetPrivateProfileIntW(L"linker", L"patches", 1, ini) && GetPrivateProfileIntW(L"linker", L"meshopt_autolod", 1, ini);
        }

        // meshoptimizer LODs get their own cache files, so switching never shows the other simplifier's
        struct Key
        {
            uint32_t k[4];
            const uint32_t* For(int pct, const uint32_t* key)
            {
                if (pct >= 100 || !key || !Meshopt()) return key;
                memcpy(k, key, sizeof k);
                k[3] ^= meshlod::kCacheTag;
                return k;
            }
        };
        int __fastcall hk_CacheName(char* dest, size_t size, const void* mesh, int pct, const uint32_t* key) { Key k; return g_name(dest, size, mesh, pct, k.For(pct, key)); }
        uint64_t __fastcall hk_CacheRead(void* reader, const void* mesh, int pct, const uint32_t* key) { Key k; return g_read(reader, mesh, pct, k.For(pct, key)); }
        void __fastcall hk_CacheWrite(const void* mesh, int pct, const uint32_t* key, void* writer) { Key k; g_write(mesh, pct, k.For(pct, key), writer); }

        // model +8 bones (192 bytes each), +56 faces (116 bytes each) as std::vector begin/end pairs, called on worker threads
        void* __fastcall hk_Reduce(void* out, uint8_t** model, float* spec, void* a4, void* err)
        {
            const uint8_t* m = model ? *model : nullptr;
            if (m && g_print)
            {
                const auto span = [m](size_t at, size_t size) { return (*reinterpret_cast<const uintptr_t*>(m + at + 8) - *reinterpret_cast<const uintptr_t*>(m + at)) / size; };
                const size_t faces = span(56, 116), bones = span(8, 192);
                bool tell = false;
                if (faces > kStockFaces || bones > kStockBones)
                {
                    std::lock_guard<std::mutex> lock(g_lock);
                    tell = g_told.insert(uint64_t(faces) << 20 | bones).second;
                }
                if (tell && bones > kStockBones)
                    g_print(0, "APEPlus: this model has %u bones. The linker refuses auto LODs over %u bones (\"Model is too complex for auto LOD\"), give it LOD files instead.\n", unsigned(bones), unsigned(kStockBones));
                else if (tell)
                    g_print(0, "APEPlus: this model has %u faces. The stock linker refuses auto LODs over 220,000 faces (\"Model is too complex for auto LOD\"). "
                               "Launcher+ builds them (LauncherPlus.ini [linker] raise_autolod_limit).\n", unsigned(faces));
            }
            struct Simplifier
            {
                explicit Simplifier(int stride) { meshlod::t_stride = stride; }
                ~Simplifier() { meshlod::t_stride = 0; meshlod::t_attributes = {}; }
            } simplifier(m && Meshopt() ? int((*reinterpret_cast<const uintptr_t*>(m + 16) - *reinterpret_cast<const uintptr_t*>(m + 8)) / 192) + 11 : 0);
            return g_orig(out, model, spec, a4, err);
        }

        bool IsFunction(void* at)
        {
            DWORD64 base = 0;
            PRUNTIME_FUNCTION fn = at ? RtlLookupFunctionEntry(DWORD64(at), &base, nullptr) : nullptr;
            return fn && DWORD64(fn->BeginAddress) + base == DWORD64(at);
        }

        // preview auto LODs from meshoptimizer while Launcher+ links with it.
        void InstallMeshopt()
        {
            void* simplify = rp::Ape()->ApeAddress(rp::self, kSimplify);
            void* name = rp::Ape()->ApeAddress(rp::self, kCacheName);
            void* read = rp::Ape()->ApeAddress(rp::self, kCacheRead);
            void* write = rp::Ape()->ApeAddress(rp::self, kCacheWrite);
            if (!IsFunction(simplify) || memcmp(simplify, kSimplifyPrologue, sizeof kSimplifyPrologue) || !IsFunction(name) || !IsFunction(read) || !IsFunction(write))
            {
                rp::Log("autolod: simplifier or cache names not found, preview LODs stay stock");
                return;
            }
            g_launcherIni = rp::PluginDir();
            g_launcherIni = g_launcherIni.substr(0, g_launcherIni.find_last_of(L"\\/")) + L"\\LauncherPlus\\LauncherPlus.ini";
            const bool ok = rp::host->Detour(rp::self, simplify, reinterpret_cast<void*>(&meshlod::HookSimplify), reinterpret_cast<void**>(&meshlod::g_simplify)) &&
                            rp::host->Detour(rp::self, name, reinterpret_cast<void*>(&hk_CacheName), reinterpret_cast<void**>(&g_name)) &&
                            rp::host->Detour(rp::self, write, reinterpret_cast<void*>(&hk_CacheWrite), reinterpret_cast<void**>(&g_write)) &&
                            rp::host->Detour(rp::self, read, reinterpret_cast<void*>(&hk_CacheRead), reinterpret_cast<void**>(&g_read));
            if (!ok) { rp::Log("autolod: meshopt detours failed, preview LODs stay stock"); return; }
            g_active = true;
            rp::Log("autolod: meshopt preview LODs installed (%s now)", Meshopt() ? "on" : "off");
        }

        void Install()
        {
            if (g_orig || !rp::Ape()) return;
            void* target = rp::Ape()->ApeAddress(rp::self, kReduce);
            const uint8_t* print = static_cast<const uint8_t*>(rp::Ape()->ApeAddress(rp::self, kPrint));
            DWORD64 base = 0;
            PRUNTIME_FUNCTION fn = target ? RtlLookupFunctionEntry(DWORD64(target), &base, nullptr) : nullptr;
            if (!fn || DWORD64(fn->BeginAddress) + base != DWORD64(target) || !print || print[0] != 0xE9) { rp::Log("autolod: APE's auto LOD code not found, no note"); return; }
            g_print = reinterpret_cast<PrintFn>(const_cast<uint8_t*>(print));
            if (!rp::host->Detour(rp::self, target, reinterpret_cast<void*>(&hk_Reduce), reinterpret_cast<void**>(&g_orig))) { rp::Log("autolod: detour failed"); g_print = nullptr; return; }
            rp::Log("autolod: installed");
            InstallMeshopt();
        }
    }

    // ---- Feature 6: extra image source formats (JPEG), three gates patched: converter, widget, file picker filter ----
    namespace formats
    {
        constexpr uintptr_t kEndsWith = 0x14031A530;      // bool ends_with(const std::string* s, const std::string* suffix): memcmp of the tail
        constexpr uintptr_t kPngCheckRet = 0x1403EDD9C;   // return address of the converter's ".png" ends-with call (E8 at 0x1403EDD97)
        constexpr uintptr_t kWidgetPngRet = 0x14018A7B9;  // return address of the texture widget validator's QString == "png" (FF 15 at 0x14018A7B3)
        constexpr uintptr_t kPickerFilter = 0x1408352F0;  // the picker's filter string (81 chars, one xref)
        const char* const kOldFilter = "TIFF Files (*.tif *.tiff);;PNG Files (*.png);;EXR Files (*.exr);;All Files (*.*)";

        typedef bool (__fastcall* EndsWithFn)(const void*, const void*);
        typedef bool (__fastcall* QsEqFn)(const void*, const char*);
        EndsWithFn g_origEndsWith = nullptr;
        QsEqFn g_origQsEq = nullptr;
        std::vector<std::string> g_exts;            // lowercase, no dot
        uintptr_t g_pngRet = 0, g_widgetRet = 0;
        bool g_converter = false, g_widget = false, g_picker = false;
        char* g_filter = nullptr;                   // the patched string, for Uninstall
        std::string g_newFilter;
        volatile LONG g_accepted = 0, g_widgetOk = 0;

        void Configure(const std::string& list)
        {
            g_exts.clear();
            std::string cur;
            auto flush = [&]() {
                size_t a = cur.find_first_not_of(" \t."), b = cur.find_last_not_of(" \t");
                std::string e = a == std::string::npos ? std::string() : cur.substr(a, b - a + 1);
                for (char& ch : e) ch = char(tolower(static_cast<unsigned char>(ch)));
                if (!e.empty() && e != "tif" && e != "tiff" && e != "png" && e != "exr" && std::find(g_exts.begin(), g_exts.end(), e) == g_exts.end()) g_exts.push_back(e);
                cur.clear();
            };
            for (char ch : list) { if (ch == ',' || ch == ';') flush(); else cur += ch; }
            flush();
        }

        // MSVC 2012 std::string: 16-byte buffer at +0 (a pointer there when the capacity is 16 or more), size +16, capacity +24
        const char* StdStringData(const void* s, size_t& n)
        {
            const char* p = static_cast<const char*>(s);
            n = *reinterpret_cast<const size_t*>(p + 16);
            return *reinterpret_cast<const size_t*>(p + 24) >= 16 ? *reinterpret_cast<const char* const*>(p) : p;
        }
        bool HasExtraExt(const char* s, size_t n)   // ".<ext>" at the end, any case
        {
            for (const std::string& e : g_exts)
                if (n > e.size() && s[n - e.size() - 1] == '.' && _strnicmp(s + n - e.size(), e.c_str(), e.size()) == 0) return true;
            return false;
        }
        bool __fastcall hk_EndsWith(const void* s, const void* suffix)
        {
            bool r = g_origEndsWith(s, suffix);
            if (r || reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_pngRet) return r;
            size_t n = 0; const char* p = StdStringData(s, n);
            if (!p || !HasExtraExt(p, n)) return false;
            InterlockedIncrement(&g_accepted);
            rp::Log("formats: '%.*s' takes the PNG (WIC) path", int(n), p);
            return true;
        }
        bool __fastcall hk_QsEq(const void* qs, const char* rhs)
        {
            bool r = g_origQsEq(qs, rhs);
            if (r || reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_widgetRet || !rhs || strcmp(rhs, "png") != 0) return r;
            std::string u = QStringToUtf8(const_cast<void**>(reinterpret_cast<void* const*>(qs)));
            for (const std::string& e : g_exts)
                if (_stricmp(u.c_str(), e.c_str()) == 0) { InterlockedIncrement(&g_widgetOk); return true; }
            return false;
        }

        void PatchPicker()
        {
            char* p = static_cast<char*>(rp::Ape()->ApeAddress(rp::self, kPickerFilter));
            if (!p || strncmp(p, kOldFilter, strlen(kOldFilter) + 1) != 0) { rp::Log("formats: picker filter text not found, the picker keeps APE's list"); return; }
            std::string f = "Image Files (*.tif *.tiff *.png *.exr";
            for (const std::string& e : g_exts) f += " *." + e;
            f += ");;All Files (*.*)";
            if (f.size() > strlen(kOldFilter)) { rp::Log("formats: picker filter would not fit (%zu chars), the picker keeps APE's list", f.size()); return; }
            DWORD old = 0;
            if (!VirtualProtect(p, strlen(kOldFilter) + 1, PAGE_READWRITE, &old)) { rp::Log("formats: picker filter page is not writable"); return; }
            memset(p, 0, strlen(kOldFilter) + 1);
            memcpy(p, f.c_str(), f.size());
            DWORD tmp = 0; VirtualProtect(p, strlen(kOldFilter) + 1, old, &tmp);
            g_filter = p; g_newFilter = f; g_picker = true;
        }

        void Install()
        {
            if (!rp::Ape()) return;
            if (g_exts.empty()) { rp::Log("formats: no extra image formats configured ([images] extra_formats)"); return; }
            // the converter's ends-with helper: sub rsp,28h / mov r9,[rcx+10h] / mov r10,[rdx+10h]
            uint8_t* target = static_cast<uint8_t*>(rp::Ape()->ApeAddress(rp::self, kEndsWith));
            const uint8_t* call = static_cast<const uint8_t*>(rp::Ape()->ApeAddress(rp::self, kPngCheckRet - 5));
            static const uint8_t kHead[12] = { 0x48, 0x83, 0xEC, 0x28, 0x4C, 0x8B, 0x49, 0x10, 0x4C, 0x8B, 0x52, 0x10 };
            DWORD64 base = 0; PRUNTIME_FUNCTION fn = target ? RtlLookupFunctionEntry(DWORD64(target), &base, nullptr) : nullptr;
            if (!target || !call || memcmp(target, kHead, sizeof kHead) != 0 || call[0] != 0xE8 || !fn || DWORD64(fn->BeginAddress) + base != DWORD64(target))
                rp::Log("formats: converter helper not recognised, the converter keeps APE's list");
            else if (!rp::host->Detour(rp::self, target, reinterpret_cast<void*>(&hk_EndsWith), reinterpret_cast<void**>(&g_origEndsWith)))
                rp::Log("formats: converter detour failed");
            else { g_pngRet = uintptr_t(call) + 5; g_converter = true; }
            // the texture widget's validator: lea rdx,"png" / lea rcx,[rbp+28h] / call cs:QString::operator==(const char*)
            const uint8_t* site = static_cast<const uint8_t*>(rp::Ape()->ApeAddress(rp::self, kWidgetPngRet - 17));
            HMODULE core = GetModuleHandleW(L"Qt5Core64r.dll");
            void* eq = core ? reinterpret_cast<void*>(GetProcAddress(core, "??8QString@@QEBA_NPEBD@Z")) : nullptr;
            if (!site || !eq || site[0] != 0x48 || site[1] != 0x8D || site[2] != 0x15 || site[7] != 0x48 || site[8] != 0x8D || site[9] != 0x4D || site[11] != 0xFF || site[12] != 0x15)
                rp::Log("formats: texture widget validator not recognised, its warning icon keeps APE's list");
            else if (!rp::host->Detour(rp::self, eq, reinterpret_cast<void*>(&hk_QsEq), reinterpret_cast<void**>(&g_origQsEq)))
                rp::Log("formats: QString::operator== detour failed");
            else { g_widgetRet = uintptr_t(site) + 17; g_widget = true; }
            PatchPicker();
            std::string list; for (const std::string& e : g_exts) list += (list.empty() ? "" : ",") + e;
            rp::Log("formats: extra image formats %s (converter %s, widget %s, picker %s)", list.c_str(), g_converter ? "on" : "off", g_widget ? "on" : "off", g_picker ? "on" : "off");
        }
        void Uninstall()
        {
            if (!g_filter) return;
            DWORD old = 0;
            if (VirtualProtect(g_filter, strlen(kOldFilter) + 1, PAGE_READWRITE, &old))
            {
                memcpy(g_filter, kOldFilter, strlen(kOldFilter) + 1);
                DWORD tmp = 0; VirtualProtect(g_filter, strlen(kOldFilter) + 1, old, &tmp);
            }
            g_filter = nullptr; g_picker = false;
        }
        std::string Status()
        {
            std::string list; for (const std::string& e : g_exts) list += (list.empty() ? "" : ",") + e;
            char buf[512];
            snprintf(buf, sizeof buf, "formats: extra=%s converter=%d (accepted %ld) widget=%d (accepted %ld) picker=%d%s%s", list.empty() ? "(none)" : list.c_str(),
                     g_converter ? 1 : 0, g_accepted, g_widget ? 1 : 0, g_widgetOk, g_picker ? 1 : 0, g_picker ? " filter='" : "", g_picker ? (g_newFilter + "'").c_str() : "");
            return buf;
        }
    }

    // ---- Feature 8: opening an asset, lazy completer popups, cheap old-grid delete, cached thumbnails ----
    namespace fastopen
    {
        constexpr uintptr_t kPopupRet = 0x1400A2A41;   // return address of APE's call cs:QCompleter::popup (FF 15 at 0x1400A2A3B)
        constexpr uintptr_t kSwapRet[2] = { 0x140174186, 0x140173A1B };   // call cs:QScrollArea::setWidget in the load / the rebuild
        constexpr int kPolishRequest = 74;             // QEvent::PolishRequest
        constexpr uintptr_t kClear = 0x1400C1160, kReset = 0x1400C1500;                   // apeAssetWidget clear / reset
        constexpr uintptr_t kResetDyingRet[2] = { 0x14017404D, 0x1401738CF };             // E8 calls of Reset in the load / the rebuild
        constexpr uintptr_t kClearDtorRet = 0x1400BC3EA;                                  // E8 call of Clear in ~apeAssetWidget
        constexpr uintptr_t kDeleteLaterRet[3] = { 0x1400C123F, 0x1400C124E, 0x1400C13F1 };   // FF 15 calls of deleteLater in Clear

        typedef void* (__fastcall* PopupFn)(const void*);
        typedef void (__fastcall* SetPopupFn)(void*, void*);
        typedef void (__fastcall* DtorFn)(void*);
        typedef void (__fastcall* UniformFn)(void*, bool);
        typedef void (__fastcall* SetWidgetFn)(void*, void*);
        typedef void (__fastcall* RemovePostedFn)(void*, int);
        SetWidgetFn g_origSetWidget = nullptr;
        RemovePostedFn g_removePosted = nullptr;
        uintptr_t g_swapRet[2] = {};
        bool g_swapOn = true, g_swapInstalled = false;
        long long g_swaps = 0;
        typedef void (__fastcall* ObjFn)(void*);
        ObjFn g_origClear = nullptr, g_origReset = nullptr, g_origDeleteLater = nullptr;
        uintptr_t g_resetDyingRet[2] = {}, g_clearDtorRet = 0, g_deleteLaterRet[3] = {};
        void* g_resetDying = nullptr;                   // the grid Reset is clearing for the swap
        void* g_dyingRoot = nullptr;                    // the grid Clear is clearing while it is about to be deleted
        bool g_clearInstalled = false;
        long long g_lateSkipped = 0;
        typedef void (__fastcall* EnsurePolishedFn)(const void*);
        typedef bool (__fastcall* VisibleToFn)(const void*, const void*);
        EnsurePolishedFn g_origEnsurePolished = nullptr;
        VisibleToFn g_visibleTo = nullptr;
        bool g_inSwap = false, g_polishInstalled = false;
        long long g_polishSkipped = 0;
        DWORD g_mainTid = 0;
        PopupFn g_origPopup = nullptr;
        SetPopupFn g_origSetPopup = nullptr;
        DtorFn g_origDtor = nullptr;
        UniformFn g_uniform = nullptr;
        uintptr_t g_siteRet = 0;
        bool g_on = true, g_installed = false;
        std::unordered_set<const void*> g_deferred;     // completers whose popup APE asked for and did not get (main thread)
        long long g_skipped = 0, g_created = 0;

        void* __fastcall hk_Popup(const void* c)
        {
            if (g_on && reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_siteRet) { g_deferred.insert(c); ++g_skipped; return nullptr; }
            return g_origPopup(c);
        }
        void __fastcall hk_SetPopup(void* c, void* view)
        {
            if (!g_deferred.empty() && g_deferred.erase(c) && view && IsA(view, "QListView")) { g_uniform(view, true); ++g_created; }   // before the model reaches it, as APE did
            g_origSetPopup(c, view);
        }
        void __fastcall hk_Dtor(void* c)
        {
            if (!g_deferred.empty()) g_deferred.erase(c);
            g_origDtor(c);
        }
        // diagnostics ("open count"): posted events by type until the next "open count"
        typedef void (__fastcall* PostEventFn)(void*, void*, int);
        PostEventFn g_origPost = nullptr;
        bool g_counting = false;
        std::map<int, long long> g_posted;
        void __fastcall hk_PostEvent(void* receiver, void* ev, int prio)
        {
            if (g_counting && ev && GetCurrentThreadId() == g_mainTid) g_posted[*reinterpret_cast<const uint16_t*>(static_cast<char*>(ev) + 16)]++;   // QEvent::t
            g_origPost(receiver, ev, prio);
        }
        std::string Count()
        {
            if (!g_origPost)
            {
                HMODULE core = GetModuleHandleW(L"Qt5Core64r.dll");
                void* post = core ? reinterpret_cast<void*>(GetProcAddress(core, "?postEvent@QCoreApplication@@SAXPEAVQObject@@PEAVQEvent@@H@Z")) : nullptr;
                if (!post || !rp::host->Detour(rp::self, post, reinterpret_cast<void*>(&hk_PostEvent), reinterpret_cast<void**>(&g_origPost))) return "postEvent detour failed";
            }
            if (!g_counting) { g_posted.clear(); g_counting = true; return "counting posted events"; }
            g_counting = false;
            std::string out = "posted events by type:";
            for (const auto& kv : g_posted) out += " " + std::to_string(kv.first) + "=" + std::to_string(kv.second);
            return out;
        }
        void __fastcall hk_Reset(void* grid)
        {
            uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
            void* prev = g_resetDying;
            if (ret == g_resetDyingRet[0] || ret == g_resetDyingRet[1]) g_resetDying = grid;
            g_origReset(grid);
            g_resetDying = prev;
        }
        void __fastcall hk_Clear(void* grid)
        {
            void* prev = g_dyingRoot;
            if (g_swapOn && (reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_clearDtorRet || (grid && grid == g_resetDying))) g_dyingRoot = grid;
            g_origClear(grid);
            g_dyingRoot = prev;
        }
        void __fastcall hk_DeleteLater(void* obj)
        {
            if (g_dyingRoot && obj && obj != g_dyingRoot)
            {
                uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
                if (ret == g_deleteLaterRet[0] || ret == g_deleteLaterRet[1] || ret == g_deleteLaterRet[2])
                    for (void* p = ParentOf(obj); p; p = ParentOf(p))
                        if (p == g_dyingRoot) { ++g_lateSkipped; return; }   // deleted with the grid, synchronously
            }
            g_origDeleteLater(obj);
        }
        void __fastcall hk_EnsurePolished(const void* w)
        {
            if (g_inSwap)
                if (void* parent = ParentOf(const_cast<void*>(w)))
                    if (!g_visibleTo(w, parent)) { ++g_polishSkipped; return; }   // hidden from its parent: polished by setVisible if shown
            g_origEnsurePolished(w);
        }
        // ---- thumbnail cache (main thread only) ----
        constexpr uintptr_t kLoadImage = 0x1401073B0;      // bool (QPixmap* out, const QString& path)
        constexpr uintptr_t kLoadImageRet = 0x1400A88F7;   // its call (through an E9 thunk) in UpdateIcon
        constexpr uintptr_t kScaledRet = 0x1400A8926;      // UpdateIcon's call cs:QPixmap::scaled(64, 64, KeepAspectRatio, Smooth)
        struct Pix { alignas(16) char obj[32]; };          // a QPixmap (24 bytes) built by Qt's own constructors
        struct PixFns
        {
            void (__fastcall* ctorCopy)(void*, const void*) = nullptr;
            void* (__fastcall* assign)(void*, const void*) = nullptr;
            void (__fastcall* dtor)(void*) = nullptr;
            int (__fastcall* width)(const void*) = nullptr;
            int (__fastcall* height)(const void*) = nullptr;
            int (__fastcall* depth)(const void*) = nullptr;
            bool (__fastcall* isNull)(const void*) = nullptr;
            long long (__fastcall* cacheKey)(const void*) = nullptr;
        } px;
        typedef bool (__fastcall* LoadImageFn)(void*, const void*);
        typedef void* (__fastcall* ScaledFn)(const void*, void*, int, int, int, int);
        LoadImageFn g_origLoadImage = nullptr;
        ScaledFn g_origScaled = nullptr;
        uintptr_t g_loadRet = 0, g_scaledRet = 0;
        bool g_thumbOn = true, g_thumbInstalled = false;
        size_t g_thumbCap = 128u << 20;
        struct Img { Pix pix; unsigned long long size = 0, time = 0; size_t bytes = 0; uint64_t used = 0; };
        std::unordered_map<std::wstring, Img*> g_imgs;      // lower-case path -> decoded image
        std::unordered_map<std::string, Img*> g_small;      // "<cacheKey> <w> <h> <aspect> <mode>" -> scaled copy
        size_t g_imgBytes = 0;
        long long g_loadHits = 0, g_loadMisses = 0, g_scaleHits = 0, g_scaleMisses = 0;

        void Drop(Img* e) { px.dtor(e->pix.obj); delete e; }
        void DropAll()
        {
            for (auto& kv : g_imgs) Drop(kv.second);
            for (auto& kv : g_small) Drop(kv.second);
            g_imgs.clear(); g_small.clear(); g_imgBytes = 0;
        }
        void Trim(uint64_t now, uint64_t idleMs)
        {
            // idle entries first, then the least recently used decoded images until the cap holds
            for (auto it = g_imgs.begin(); it != g_imgs.end();)
                if (now - it->second->used > idleMs) { g_imgBytes -= it->second->bytes; Drop(it->second); it = g_imgs.erase(it); } else ++it;
            for (auto it = g_small.begin(); it != g_small.end();)
                if (now - it->second->used > idleMs || g_small.size() > 2048) { Drop(it->second); it = g_small.erase(it); } else ++it;
            while (g_imgBytes > g_thumbCap && !g_imgs.empty())
            {
                auto old = g_imgs.begin();
                for (auto it = g_imgs.begin(); it != g_imgs.end(); ++it) if (it->second->used < old->second->used) old = it;
                g_imgBytes -= old->second->bytes; Drop(old->second); g_imgs.erase(old);
            }
        }
        bool __fastcall hk_LoadImage(void* out, const void* path)
        {
            if (!g_thumbOn || reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_loadRet) return g_origLoadImage(out, path);
            std::string u = QStringToUtf8(const_cast<void**>(reinterpret_cast<void* const*>(path)));
            std::wstring key(u.size(), L'\0');
            key.resize(MultiByteToWideChar(CP_UTF8, 0, u.data(), int(u.size()), &key[0], int(key.size())));
            for (wchar_t& c : key) c = c == L'/' ? L'\\' : wchar_t(towlower(c));
            WIN32_FILE_ATTRIBUTE_DATA fa = {};
            if (key.empty() || !GetFileAttributesExW(key.c_str(), GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return g_origLoadImage(out, path);
            unsigned long long size = (unsigned long long)fa.nFileSizeHigh << 32 | fa.nFileSizeLow;
            unsigned long long time = (unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32 | fa.ftLastWriteTime.dwLowDateTime;
            uint64_t now = GetTickCount64();
            auto it = g_imgs.find(key);
            if (it != g_imgs.end())
            {
                if (it->second->size == size && it->second->time == time) { it->second->used = now; px.assign(out, it->second->pix.obj); ++g_loadHits; return true; }
                g_imgBytes -= it->second->bytes; Drop(it->second); g_imgs.erase(it);   // the file changed: decode it again
            }
            bool ok = g_origLoadImage(out, path);
            ++g_loadMisses;
            if (!ok || px.isNull(out)) return ok;
            Img* e = new Img();
            px.ctorCopy(e->pix.obj, out);                   // shares the decoded data (implicit sharing), no copy of pixels
            e->size = size; e->time = time; e->used = now;
            e->bytes = size_t(px.width(out)) * size_t(px.height(out)) * size_t(std::max(1, px.depth(out)) / 8);
            g_imgs[key] = e; g_imgBytes += e->bytes;
            Trim(now, 120000);
            return ok;
        }
        void* __fastcall hk_Scaled(const void* self, void* ret, int w, int h, int aspect, int mode)
        {
            if (!g_thumbOn || reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_scaledRet) return g_origScaled(self, ret, w, h, aspect, mode);
            char key[96];
            snprintf(key, sizeof key, "%lld %d %d %d %d", px.cacheKey(self), w, h, aspect, mode);
            auto it = g_small.find(key);
            if (it != g_small.end()) { it->second->used = GetTickCount64(); px.ctorCopy(ret, it->second->pix.obj); ++g_scaleHits; return ret; }
            g_origScaled(self, ret, w, h, aspect, mode);
            ++g_scaleMisses;
            Img* e = new Img();
            px.ctorCopy(e->pix.obj, ret); e->used = GetTickCount64();
            g_small[key] = e;
            return ret;
        }
        bool CallsTo(uintptr_t ret, const void* fn);
        bool IsFunction(const void* p);
        void InstallThumbs(HMODULE gui)
        {
            px.ctorCopy = reinterpret_cast<decltype(px.ctorCopy)>(GetProcAddress(gui, "??0QPixmap@@QEAA@AEBV0@@Z"));
            px.assign = reinterpret_cast<decltype(px.assign)>(GetProcAddress(gui, "??4QPixmap@@QEAAAEAV0@AEBV0@@Z"));
            px.dtor = reinterpret_cast<decltype(px.dtor)>(GetProcAddress(gui, "??1QPixmap@@UEAA@XZ"));
            px.width = reinterpret_cast<decltype(px.width)>(GetProcAddress(gui, "?width@QPixmap@@QEBAHXZ"));
            px.height = reinterpret_cast<decltype(px.height)>(GetProcAddress(gui, "?height@QPixmap@@QEBAHXZ"));
            px.depth = reinterpret_cast<decltype(px.depth)>(GetProcAddress(gui, "?depth@QPixmap@@QEBAHXZ"));
            px.isNull = reinterpret_cast<decltype(px.isNull)>(GetProcAddress(gui, "?isNull@QPixmap@@QEBA_NXZ"));
            px.cacheKey = reinterpret_cast<decltype(px.cacheKey)>(GetProcAddress(gui, "?cacheKey@QPixmap@@QEBA_JXZ"));
            void* scaled = GetProcAddress(gui, "?scaled@QPixmap@@QEBA?AV1@HHW4AspectRatioMode@Qt@@W4TransformationMode@3@@Z");
            void* load = rp::Ape()->ApeAddress(rp::self, kLoadImage);
            const uint8_t* scall = static_cast<const uint8_t*>(rp::Ape()->ApeAddress(rp::self, kScaledRet - 6));
            bool ok = px.ctorCopy && px.assign && px.dtor && px.width && px.height && px.depth && px.isNull && px.cacheKey && scaled &&
                      IsFunction(load) && CallsTo(kLoadImageRet, load) && scall && scall[0] == 0xFF && scall[1] == 0x15 &&
                      *reinterpret_cast<void* const*>(scall + 6 + *reinterpret_cast<const int32_t*>(scall + 2)) == scaled;
            if (!ok) { rp::Log("fastopen: image field thumbnail code not recognised, thumbnails load uncached"); return; }
            g_loadRet = uintptr_t(rp::Ape()->ApeAddress(rp::self, kLoadImageRet));
            g_scaledRet = uintptr_t(scall) + 6;
            if (!rp::host->Detour(rp::self, scaled, reinterpret_cast<void*>(&hk_Scaled), reinterpret_cast<void**>(&g_origScaled)) ||
                !rp::host->Detour(rp::self, load, reinterpret_cast<void*>(&hk_LoadImage), reinterpret_cast<void**>(&g_origLoadImage)))
            { rp::Log("fastopen: thumbnail detours failed"); return; }
            g_thumbInstalled = true;
            rp::Log("fastopen: image field thumbnails cached (%s, %zu MB)", g_thumbOn ? "on" : "off by [open] thumb_cache=0", g_thumbCap >> 20);
        }
        void Tick() { if (g_thumbInstalled && (!g_imgs.empty() || !g_small.empty())) { static uint64_t last = 0; uint64_t now = GetTickCount64(); if (now - last > 5000) { last = now; Trim(now, 120000); } } }

        void __fastcall hk_SetWidget(void* area, void* w)
        {
            uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
            if (!g_swapOn || (ret != g_swapRet[0] && ret != g_swapRet[1])) { g_origSetWidget(area, w); return; }
            g_removePosted(nullptr, kPolishRequest); ++g_swaps;
            g_inSwap = g_polishInstalled;
            g_origSetWidget(area, w);
            g_inSwap = false;
        }

        // an E8 call at ret - 5 whose target (through one E9 thunk) is fn
        bool CallsTo(uintptr_t ret, const void* fn)
        {
            const uint8_t* call = static_cast<const uint8_t*>(rp::Ape()->ApeAddress(rp::self, ret - 5));
            if (!call || call[0] != 0xE8) return false;
            const uint8_t* t = call + 5 + *reinterpret_cast<const int32_t*>(call + 1);
            if (t[0] == 0xE9) t = t + 5 + *reinterpret_cast<const int32_t*>(t + 1);
            return t == fn;
        }
        bool IsFunction(const void* p)
        {
            DWORD64 base = 0; PRUNTIME_FUNCTION fn = p ? RtlLookupFunctionEntry(DWORD64(p), &base, nullptr) : nullptr;
            return fn && DWORD64(fn->BeginAddress) + base == DWORD64(p);
        }
        void InstallClear()
        {
            void* clear = rp::Ape()->ApeAddress(rp::self, kClear);
            void* reset = rp::Ape()->ApeAddress(rp::self, kReset);
            HMODULE core = GetModuleHandleW(L"Qt5Core64r.dll");
            void* deleteLater = core ? reinterpret_cast<void*>(GetProcAddress(core, "?deleteLater@QObject@@QEAAXXZ")) : nullptr;
            bool ok = IsFunction(clear) && IsFunction(reset) && deleteLater && CallsTo(kResetDyingRet[0], reset) && CallsTo(kResetDyingRet[1], reset) && CallsTo(kClearDtorRet, clear);
            for (int i = 0; ok && i < 3; ++i)
            {
                const uint8_t* call = static_cast<const uint8_t*>(rp::Ape()->ApeAddress(rp::self, kDeleteLaterRet[i] - 6));
                ok = call && call[0] == 0xFF && call[1] == 0x15 && *reinterpret_cast<void* const*>(call + 6 + *reinterpret_cast<const int32_t*>(call + 2)) == deleteLater;
                if (ok) g_deleteLaterRet[i] = uintptr_t(call) + 6;
            }
            if (!ok) { rp::Log("fastopen: grid clear not recognised, the old grid's widgets keep their deleteLater"); return; }
            for (int i = 0; i < 2; ++i) g_resetDyingRet[i] = uintptr_t(rp::Ape()->ApeAddress(rp::self, kResetDyingRet[i]));
            g_clearDtorRet = uintptr_t(rp::Ape()->ApeAddress(rp::self, kClearDtorRet));
            if (!rp::host->Detour(rp::self, deleteLater, reinterpret_cast<void*>(&hk_DeleteLater), reinterpret_cast<void**>(&g_origDeleteLater)) ||
                !rp::host->Detour(rp::self, clear, reinterpret_cast<void*>(&hk_Clear), reinterpret_cast<void**>(&g_origClear)) ||
                !rp::host->Detour(rp::self, reset, reinterpret_cast<void*>(&hk_Reset), reinterpret_cast<void**>(&g_origReset)))
            { rp::Log("fastopen: grid clear detours failed"); return; }
            g_clearInstalled = true;
            rp::Log("fastopen: a dying grid's widgets are deleted with it instead of each getting a deleteLater");
        }
        void InstallSwap(HMODULE w)
        {
            HMODULE core = GetModuleHandleW(L"Qt5Core64r.dll");
            void* setWidget = w ? reinterpret_cast<void*>(GetProcAddress(w, "?setWidget@QScrollArea@@QEAAXPEAVQWidget@@@Z")) : nullptr;
            g_removePosted = core ? reinterpret_cast<RemovePostedFn>(GetProcAddress(core, "?removePostedEvents@QCoreApplication@@SAXPEAVQObject@@H@Z")) : nullptr;
            for (int i = 0; i < 2; ++i)
            {
                const uint8_t* call = static_cast<const uint8_t*>(rp::Ape()->ApeAddress(rp::self, kSwapRet[i] - 6));
                if (!call || call[0] != 0xFF || call[1] != 0x15 || !setWidget ||
                    *reinterpret_cast<void* const*>(call + 6 + *reinterpret_cast<const int32_t*>(call + 2)) != setWidget)
                { rp::Log("fastopen: grid swap %d not recognised, the old grid is deleted the slow way", i); return; }
                g_swapRet[i] = uintptr_t(call) + 6;
            }
            if (!g_removePosted || !rp::host->Detour(rp::self, setWidget, reinterpret_cast<void*>(&hk_SetWidget), reinterpret_cast<void**>(&g_origSetWidget)))
            { rp::Log("fastopen: QScrollArea::setWidget detour failed"); return; }
            g_swapInstalled = true;
            g_mainTid = GetCurrentThreadId();
            void* ensurePolished = reinterpret_cast<void*>(GetProcAddress(w, "?ensurePolished@QWidget@@QEBAXXZ"));
            g_visibleTo = reinterpret_cast<VisibleToFn>(GetProcAddress(w, "?isVisibleTo@QWidget@@QEBA_NPEBV1@@Z"));
            g_polishInstalled = ensurePolished && g_visibleTo &&
                                rp::host->Detour(rp::self, ensurePolished, reinterpret_cast<void*>(&hk_EnsurePolished), reinterpret_cast<void**>(&g_origEnsurePolished));
            rp::Log("fastopen: grid swap drops the new grid's polish requests first (%s)", g_swapOn ? "on" : "off by [open] fast_swap=0");
            InstallClear();
            if (HMODULE gui = GetModuleHandleW(L"Qt5Gui64r.dll")) InstallThumbs(gui);
        }

        void Install()
        {
            if (!rp::Ape() || g_installed) return;
            const uint8_t* call = static_cast<const uint8_t*>(rp::Ape()->ApeAddress(rp::self, kPopupRet - 6));
            HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll");
            void* popup = w ? reinterpret_cast<void*>(GetProcAddress(w, "?popup@QCompleter@@QEBAPEAVQAbstractItemView@@XZ")) : nullptr;
            void* setPopup = w ? reinterpret_cast<void*>(GetProcAddress(w, "?setPopup@QCompleter@@QEAAXPEAVQAbstractItemView@@@Z")) : nullptr;
            void* dtor = w ? reinterpret_cast<void*>(GetProcAddress(w, "??1QCompleter@@UEAA@XZ")) : nullptr;
            g_uniform = w ? reinterpret_cast<UniformFn>(GetProcAddress(w, "?setUniformItemSizes@QListView@@QEAAX_N@Z")) : nullptr;
            // the site must be an indirect call through the import slot of QCompleter::popup
            bool siteOk = call && call[0] == 0xFF && call[1] == 0x15 && popup &&
                          *reinterpret_cast<void* const*>(call + 6 + *reinterpret_cast<const int32_t*>(call + 2)) == popup;
            if (!siteOk || !setPopup || !dtor || !g_uniform) { rp::Log("fastopen: APE's completer setup not recognised, asset fields keep their eager popups"); return; }
            if (!rp::host->Detour(rp::self, dtor, reinterpret_cast<void*>(&hk_Dtor), reinterpret_cast<void**>(&g_origDtor)) ||
                !rp::host->Detour(rp::self, setPopup, reinterpret_cast<void*>(&hk_SetPopup), reinterpret_cast<void**>(&g_origSetPopup)) ||
                !rp::host->Detour(rp::self, popup, reinterpret_cast<void*>(&hk_Popup), reinterpret_cast<void**>(&g_origPopup)))
            { rp::Log("fastopen: QCompleter detours failed"); return; }
            g_siteRet = uintptr_t(call) + 6; g_installed = true;
            rp::Log("fastopen: asset field popups made lazy (%s)", g_on ? "on" : "off by [open] lazy_popups=0");
            InstallSwap(w);
        }
        std::string Status()
        {
            char buf[512];
            snprintf(buf, sizeof buf, "fastopen: installed=%d lazy_popups=%d skipped=%lld created_later=%lld pending=%zu; swap installed=%d fast_swap=%d swaps=%lld; clear installed=%d deleteLater skipped=%lld; polish installed=%d skipped=%lld",
                     g_installed ? 1 : 0, g_on ? 1 : 0, g_skipped, g_created, g_deferred.size(), g_swapInstalled ? 1 : 0, g_swapOn ? 1 : 0, g_swaps, g_clearInstalled ? 1 : 0, g_lateSkipped, g_polishInstalled ? 1 : 0, g_polishSkipped);
            char more[256];
            snprintf(more, sizeof more, "; thumbs installed=%d thumb_cache=%d images=%zu (%zu MB) loads hit/miss=%lld/%lld scaled hit/miss=%lld/%lld",
                     g_thumbInstalled ? 1 : 0, g_thumbOn ? 1 : 0, g_imgs.size(), g_imgBytes >> 20, g_loadHits, g_loadMisses, g_scaleHits, g_scaleMisses);
            return std::string(buf) + more;
        }
    }

    // ---- Feature 4: the DXVK (Vulkan) renderer switch, dxgi.dll and d3d11.dll in <root>\bin, applied at the next start ----
    namespace dxvk
    {
        std::wstring Parent(std::wstring p) { size_t k = p.find_last_of(L"\\/"); return k == std::wstring::npos ? p : p.substr(0, k); }
        std::wstring ExeDir() { wchar_t p[MAX_PATH * 2] = {}; GetModuleFileNameW(nullptr, p, MAX_PATH * 2); return Parent(p); }
        std::wstring PluginDir()
        {
            HMODULE m = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&PluginDir), &m);
            wchar_t p[MAX_PATH * 2] = {}; GetModuleFileNameW(m, p, MAX_PATH * 2); return Parent(p);
        }
        bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
        std::string N(const std::wstring& w) { int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr); std::string o(n > 0 ? n : 0, '\0'); if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), &o[0], n, nullptr, nullptr); return o; }
        // the bundled copy: ours, else RadiantPlus's (plugins\<Name>\ are siblings)
        std::wstring SourceDir()
        {
            const std::wstring own = PluginDir() + L"\\dxvk", br = Parent(PluginDir()) + L"\\RadiantPlus\\dxvk";
            for (const std::wstring& d : { own, br }) if (Exists(d + L"\\dxgi.dll") && Exists(d + L"\\d3d11.dll")) return d;
            return L"";
        }
        // CompanyName "DXVK" in the version resource (DXVK's DLLs carry fake Windows version numbers)
        bool IsDxvkFile(const std::wstring& path)
        {
            DWORD dummy = 0, size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
            if (!size) return false;
            std::vector<char> data(size);
            if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return false;
            struct Lang { WORD lang, cp; } * langs = nullptr; UINT len = 0;
            if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &len) || len < sizeof(Lang)) return false;
            for (UINT i = 0; i < len / sizeof(Lang); ++i)
            {
                wchar_t key[64]; swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\CompanyName", langs[i].lang, langs[i].cp);
                wchar_t* value = nullptr; UINT vlen = 0;
                if (VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&value), &vlen) && value && _wcsicmp(value, L"DXVK") == 0) return true;
            }
            return false;
        }
        std::string Version(const std::wstring& dir)
        {
            FILE* f = dir.empty() ? nullptr : _wfopen((dir + L"\\version.txt").c_str(), L"rb");
            if (!f) return "";
            char buf[32] = {}; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f);
            std::string v(buf, n); while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
            return v;
        }
        // active in this APE: dxgi.dll mapped from a DXVK file (or from one renamed to *.dxvk-off this session)
        bool Live(std::wstring* pathOut = nullptr)
        {
            HMODULE m = GetModuleHandleW(L"dxgi.dll");
            if (!m) m = GetModuleHandleW(L"d3d11.dll");
            if (!m) return false;
            wchar_t path[MAX_PATH * 2] = {}; GetModuleFileNameW(m, path, MAX_PATH * 2);
            if (pathOut) *pathOut = path;
            return IsDxvkFile(path) || (!Exists(path) && IsDxvkFile(std::wstring(path) + L".dxvk-off"));
        }
        bool InBin() { const std::wstring b = ExeDir(); return IsDxvkFile(b + L"\\dxgi.dll") && IsDxvkFile(b + L"\\d3d11.dll"); }
        void Cleanup() { for (const wchar_t* n : { L"dxgi.dll.dxvk-off", L"d3d11.dll.dxvk-off" }) DeleteFileW((ExeDir() + L"\\" + n).c_str()); }
        std::string Status()
        {
            Cleanup();
            std::wstring path; const bool live = Live(&path), on = InBin();
            const std::wstring src = SourceDir();
            std::string out = live ? "DXVK active in this APE (" + N(path) + ")" : "DXVK not active in this APE (native Direct3D 11)";
            out += "; setting: " + std::string(on ? "on (dxgi.dll + d3d11.dll in bin)" : "off");
            if (on != live) out += " (restart APE to apply)";
            out += "; shared with Radiant (RadiantPlus's switch is the same files), each editor applies it at its next start";
            if (src.empty()) out += "; no bundled copy: put x64 dxgi.dll and d3d11.dll from github.com/doitsujin/dxvk/releases into " + N(PluginDir() + L"\\dxvk");
            else { std::string v = Version(src); out += "; bundled DXVK " + (v.empty() ? std::string("?") : v) + " from " + N(src); }
            return out;
        }
        std::string Set(bool on)
        {
            const std::wstring bin = ExeDir(), src = SourceDir();
            std::string problem;
            for (const wchar_t* n : { L"dxgi.dll", L"d3d11.dll" })
            {
                const std::wstring dst = bin + L"\\" + n;
                if (on)
                {
                    if (src.empty()) { problem = "bundled DXVK files missing"; break; }
                    if (Exists(dst) && !IsDxvkFile(dst)) { problem = "bin\\" + N(n) + " exists and is not DXVK's, not touching it"; break; }
                    if (!CopyFileW((src + L"\\" + n).c_str(), dst.c_str(), FALSE))
                    {
                        const DWORD err = GetLastError();
                        if (err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED) continue;   // an editor maps the current copy: it stays
                        problem = "copy of " + N(n) + " into bin failed (error " + std::to_string(err) + ")"; break;
                    }
                }
                else
                {
                    if (!Exists(dst)) continue;
                    if (!IsDxvkFile(dst)) { problem = "bin\\" + N(n) + " is not DXVK's, not touching it"; break; }
                    if (DeleteFileW(dst.c_str())) continue;
                    const std::wstring off = dst + L".dxvk-off";
                    DeleteFileW(off.c_str());
                    if (!MoveFileExW(dst.c_str(), off.c_str(), MOVEFILE_REPLACE_EXISTING)) { problem = "could not remove bin\\" + N(n) + " (error " + std::to_string(GetLastError()) + ")"; break; }
                }
            }
            rp::Log("dxvk: set %s%s%s", on ? "on" : "off", problem.empty() ? "" : ": ", problem.c_str());
            std::string out = Status();
            if (!problem.empty()) out = "DXVK: " + problem + "; " + out;
            return out;
        }
        std::string PsQuote(const std::wstring& w) { std::string s = N(w), o; for (char c : s) { o += c; if (c == '\'') o += '\''; } return o; }
        // restart APE (single instance): a hidden PowerShell waits for this process to end (up to 10 minutes),
        void Restart()
        {
            wchar_t cwd[MAX_PATH * 2] = {}, exe[MAX_PATH * 2] = {};
            GetCurrentDirectoryW(MAX_PATH * 2, cwd); GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
            std::wstring cl = GetCommandLineW(); size_t i = 0;
            if (!cl.empty() && cl[0] == L'"') { i = cl.find(L'"', 1); i = (i == std::wstring::npos) ? cl.size() : i + 1; }
            else { i = cl.find(L' '); if (i == std::wstring::npos) i = cl.size(); }
            while (i < cl.size() && cl[i] == L' ') ++i;
            const std::wstring args = cl.substr(i);
            wchar_t tmp[MAX_PATH] = {}; GetTempPathW(MAX_PATH, tmp);
            const std::wstring ps1 = std::wstring(tmp) + L"apeplus_restart_" + std::to_wstring(GetCurrentProcessId()) + L".ps1";
            FILE* f = _wfopen(ps1.c_str(), L"wb");
            if (!f) { rp::Log("dxvk: cannot write %s", N(ps1).c_str()); return; }
            std::string text = "$p = Get-Process -Id " + std::to_string(GetCurrentProcessId()) + " -ErrorAction SilentlyContinue\r\n"
                               "if ($p) { $p.WaitForExit(600000) | Out-Null }\r\n"
                               "Start-Process -FilePath '" + PsQuote(exe) + "' -WorkingDirectory '" + PsQuote(cwd) + "'" + (args.empty() ? "" : " -ArgumentList '" + PsQuote(args) + "'") + "\r\n"
                               "Remove-Item -LiteralPath $MyInvocation.MyCommand.Path -ErrorAction SilentlyContinue\r\n";
            fwrite(text.data(), 1, text.size(), f); fclose(f);
            std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + ps1 + L"\"";
            STARTUPINFOW si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
            if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { rp::Log("dxvk: restart helper failed (%lu)", GetLastError()); return; }
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            HWND main = static_cast<HWND>(rp::host->MainWindow(rp::self));
            rp::Log("dxvk: restart requested (%s %s), closing the main window", N(exe).c_str(), N(args).c_str());
            if (main) PostMessageW(main, WM_CLOSE, 0, 0);
        }
        // after OK in Preferences (next tick, the dialog is gone): apply the checkbox, offer the restart when it changes this APE
        int g_pending = -1;   // -1 nothing, 0/1 the checkbox's value
        void Tick()
        {
            if (g_pending < 0) return;
            const bool want = g_pending == 1; g_pending = -1;
            if (want != InBin()) { std::string r = Set(want); rp::Log("dxvk: %s", r.c_str()); }
            if (InBin() == Live()) return;
            const char* text = want ? "DXVK (Vulkan) renderer turned on.\n\nWhile on, Radiant uses it too.\n\nRestart APE now to apply?"
                                    : "DXVK (Vulkan) renderer turned off.\n\nRestart APE now to apply?";
            HWND main = static_cast<HWND>(rp::host->MainWindow(rp::self));
            if (MessageBoxA(main, text, "APE+", MB_YESNO | MB_ICONQUESTION) == IDYES) Restart();
        }
    }

    // ---- Feature 3: themes, a Theme combo in View > Preferences in place of "Use Treyarch Theme" ----
    namespace theme
    {
        struct Entry { std::string name; std::string label; std::wstring file; std::string style; bool classic; bool supplement; };
        std::vector<Entry> g_list;
        std::string g_current;                 // the ini choice ("" or "default" = follow APE's own flag)
        std::string g_applied;                 // the look in effect ("classic"/"Classic Treyarch" from APE's flag until the plugin applies one)
        std::string g_pending;                 // picked in the Theme combo, applied on the next tick (outside the combo's signal)
        bool g_fillingCombo = false;           // the Theme combo is being refilled from code: not a pick
        const char* const kEditedName = "*edited*";   // the Theme combo's "Edited (not saved)" entry (the editor's live edit)
        bool g_started = false;
        double g_lastMs = 0;
        std::string g_lastError;

        // APE addresses (IDA), each checked before use
        const uintptr_t kPrefsCtor = 0x140120990;      // apePreferencesDialog::apePreferencesDialog(QWidget* parent)
        const uintptr_t kFlagLoad = 0x14011C35F;       // in the ViewPreferences slot: movzx edx, byte ptr [rip+X] (X = radiantTheme)
        const uintptr_t kRadiantTheme = 0x14118D615;   // bool radiantTheme, read by QT_SetTheme (0x14011BFC0)
        const size_t off_Prefs_TreyarchBox = 0x38;     // apePreferencesDialog: QCheckBox* "Use Treyarch Theme"
        using PrefsCtor = void* (__fastcall*)(void* self, void* parent);
        PrefsCtor g_origCtor = nullptr;
        void* g_ctorLive = nullptr;                    // live address of the constructor (calling it runs the detour)
        uint8_t* g_flag = nullptr;                     // radiantTheme, null when the code bytes did not match
        bool g_hooked = false;
        int g_patched = 0;
        std::string g_patchError;

        struct Dialog { void* dialog = nullptr; void* box = nullptr; void* combo = nullptr; void* dxvkBox = nullptr; void* dxvkNow = nullptr; void* discordBox = nullptr; bool okHooked = false; std::vector<std::string> names; };
        Dialog g_dlg;


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

        // rewrite url(...) references: relative paths against baseDir, ':/stylesheet/<file>' to themes\images\<file> when shipped
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

        // Tooltips: Qt word-wraps only rich text, so the text is wrapped in <p> with the HTML characters escaped.
        std::string Tip(const char* text)
        {
            std::string out = "<p>";
            for (const char* p = text; *p; ++p) { if (*p == '<') out += "&lt;"; else if (*p == '>') out += "&gt;"; else if (*p == '&') out += "&amp;"; else out += *p; }
            return out + "</p>";
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

        // Links: APE's asset references are rich-text <a href> labels coloured from the palette's Link role, which no sheet sets.
        // The colour comes from the sheet's palette line (link=#rrggbb, else the accent), Light and Classic keep Qt's default.
        // Labels already showing a link are set again (empty, then their text) so they re-parse.
        int g_defaultLink = -1;     // Qt's Link colour before the first change (0xRRGGBB), -1 = not read yet
        int g_link = -1;            // the colour in effect, -1 = untouched
        bool HeaderColor(const std::string& sheet, const char* key, int& rgb)
        {
            std::string tag = std::string(" ") + key + "=#";
            size_t at = sheet.find(tag);
            if (at == std::string::npos || at > 4096 || at + tag.size() + 6 > sheet.size()) return false;
            char* end = nullptr; long v = strtol(sheet.c_str() + at + tag.size(), &end, 16);
            if (end != sheet.c_str() + at + tag.size() + 6) return false;
            rgb = int(v); return true;
        }
        double Luminance(int rgb)       // WCAG relative luminance
        {
            double l = 0; const double k[3] = { 0.2126, 0.7152, 0.0722 };
            for (int i = 0; i < 3; ++i) { double c = ((rgb >> (16 - 8 * i)) & 255) / 255.0; l += k[i] * (c <= 0.03928 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4)); }
            return l;
        }
        double Contrast(int a, int b) { double la = Luminance(a) + 0.05, lb = Luminance(b) + 0.05; return la > lb ? la / lb : lb / la; }
        int Mix(int rgb, int to, double t) { int out = 0; for (int s = 0; s < 24; s += 8) out |= int(((rgb >> s) & 255) * (1 - t) + ((to >> s) & 255) * t + 0.5) << s; return out; }
        // the accent, pulled toward white on a dark bg (black on a light one) until the text reads (4.5:1)
        int Readable(int rgb, int bg)
        {
            int to = Luminance(bg) < 0.18 ? 0xffffff : 0x000000;
            for (int i = 0; i < 20 && Contrast(rgb, bg) < 4.5; ++i) rgb = Mix(rgb, to, 0.1);
            return rgb;
        }
        int LinkColorFor(const std::string& sheet)
        {
            int rgb, bg, accent, accent2;
            if (HeaderColor(sheet, "link", rgb)) return rgb;
            if (!HeaderColor(sheet, "bg", bg) || !HeaderColor(sheet, "accent", accent)) return -1;
            if (!HeaderColor(sheet, "accent2", accent2)) accent2 = accent;
            return Readable(Luminance(bg) < 0.18 ? accent2 : accent, bg);
        }
        int RefreshLinkLabels()
        {
            int n = 0;
            void* empty = nullptr; qt.qstringFromUtf8(&empty, "", 0);
            ForEachWidget([&](void* w) {
                if (!IsA(w, "QLabel")) return false;
                void* qs = nullptr; qt.labelText(w, &qs);
                std::string t = QStringToUtf8(&qs);
                if (t.find("href") != std::string::npos) { qt.labelSetText(w, &empty); qt.labelSetText(w, &qs); ++n; }
                qt.qsDtor(&qs);
                return false; });
            qt.qsDtor(&empty);
            return n;
        }
        void ApplyLinkColor(int rgb)
        {
            if (!qt.guiPalette || !qt.appSetPalette || !qt.paletteSetBrush || !qt.paletteColor || !qt.paletteDtor || !qt.brushCtorColor || !qt.brushDtor || !qt.colorCtorRgb) return;
            if (rgb < 0 && g_link < 0) return;      // Qt's default wanted and never changed
            enum { Active = 0, All = 5, Link = 14, LinkVisited = 15, SolidPattern = 1, SpecRgb = 1 };
            alignas(16) unsigned char pal[32] = {};
            qt.guiPalette(pal);
            if (g_defaultLink < 0)
            {
                const unsigned char* c = static_cast<const unsigned char*>(qt.paletteColor(pal, Active, Link));   // QColor: int cspec, ushort alpha, red, green, blue
                const unsigned short* ch = reinterpret_cast<const unsigned short*>(c + 4);
                g_defaultLink = *reinterpret_cast<const int*>(c) == SpecRgb ? ((ch[1] >> 8) << 16) | ((ch[2] >> 8) << 8) | (ch[3] >> 8) : 0x0000ff;
            }
            int want = rgb < 0 ? g_defaultLink : rgb;
            if (want == g_link) { qt.paletteDtor(pal); return; }
            alignas(16) unsigned char color[32] = {}; qt.colorCtorRgb(color, (want >> 16) & 255, (want >> 8) & 255, want & 255, 255);
            alignas(16) unsigned char brush[16] = {}; qt.brushCtorColor(brush, color, SolidPattern);
            qt.paletteSetBrush(pal, All, Link, brush);
            qt.paletteSetBrush(pal, All, LinkVisited, brush);
            qt.appSetPalette(pal, nullptr);
            qt.brushDtor(brush); qt.paletteDtor(pal);
            g_link = want;
            int refreshed = RefreshLinkLabels();
            rp::Log("theme: link colour #%06x%s, %d labels refreshed", want, rgb < 0 ? " (Qt's default)" : "", refreshed);
        }

        // "default" means APE's own flag
        std::string FlagTheme() { return g_flag && *g_flag ? themeicons::kClassicTreyarch : "classic"; }
        const Entry* Find(const std::string& name)
        {
            std::string l = Lower(name);
            if (l == "default") l = FlagTheme();
            for (const Entry& e : g_list) if (Lower(e.name) == l) return &e;
            return nullptr;
        }

        // keep APE's flag (and so its saved RadiantTheme) equal to "the look is Radiant's stylesheet" (Classic Treyarch)
        void SyncFlag(const std::string& name) { if (g_flag) *g_flag = themeicons::IsClassicTreyarch(name) ? 1 : 0; }

        // The base style and sheet in effect. setStyle restyles every widget (seconds), a sheet swap is much cheaper, so the style
        // is only set when it changes, with the old sheet still on (toward Classic the sheet is cleared first).
        std::string g_style;                   // "" = not known yet
        bool g_sheetSet = false;
        bool g_stateKnown = false;
        void KnowStartState()
        {
            if (g_stateKnown) return;
            g_stateKnown = true;
            if (!g_flag) return;                // unknown: the first apply sets the style
            g_style = *g_flag ? "plastique" : "WindowsVista";   // what APE's QT_SetTheme set at startup
            g_sheetSet = *g_flag != 0;
        }
        bool Apply(const std::string& name, std::string& why)
        {
            if (!qt.bound || !qt.appInstance()) { why = "Qt is not bound"; return false; }
            const Entry* e = Find(name);
            if (!e) { why = "no theme named '" + name + "' (see 'theme list')"; return false; }
            KnowStartState();
            uint64_t t0 = Now();
            std::string style;
            if (e->classic)                     // what QT_SetTheme does with the flag off
            {
                themeicons::Use(themeicons::PackFor(Dir(), e->name, "", L""));
                themeicons::UseTitleBar("", "classic");
                style = "WindowsVista";
                if (g_sheetSet) { SetSheet(""); g_sheetSet = false; }
                if (Lower(g_style) != Lower(style) && SetStyle(style)) g_style = style;
                ApplyLinkColor(-1);
            }
            else
            {
                std::string sheet;
                if (!ReadWhole(e->file, sheet)) { why = "cannot read " + Narrow(e->file); return false; }
                style = e->style.empty() ? StyleDirective(sheet) : e->style;
                if (style.empty()) style = "WindowsVista";
                size_t cut = e->file.find_last_of(L"\\/");
                std::wstring sheetDir = cut == std::wstring::npos ? Dir() : e->file.substr(0, cut);
                themeicons::Use(themeicons::PackFor(Dir(), e->name, sheet, sheetDir), themeicons::LogoFor(sheet, sheetDir, "ape"));   // before the sheet, so its url(:/...) images come from the pack too
                themeicons::UseTitleBar(sheet, e->name);   // Treyarch: built-in colours
                sheet = Resolve(sheet, sheetDir);
                if (Lower(g_style) != Lower(style))
                {
                    if (SetStyle(style)) g_style = style;
                    else rp::Log("theme: base style '%s' is not available, keeping the current style", style.c_str());
                }
                SetSheet(sheet);
                g_sheetSet = true;
                // Light, Classic and Classic Treyarch keep Qt's blue links, the rest take the accent
                bool asBefore = Lower(e->name) == "light" || themeicons::IsClassicTreyarch(e->name);
                ApplyLinkColor(asBefore ? -1 : LinkColorFor(sheet));
            }
            g_applied = e->name;
            themeedit::ClearEdited();                 // a theme replaces the editor's unsaved edit
            SyncFlag(e->name);
            g_lastMs = Ms(Now() - t0);
            rp::Log("theme: '%s' applied (style %s) in %.0f ms", e->name.c_str(), style.c_str(), g_lastMs);
            return true;
        }

        // The shipped themes go to the shared <root>\bin\themes once, and again when the shipped copy is newer (an edited one stays).
        // Also carries custom sheets from older per-plugin folders. Runs before every scan.
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
            static bool retired = false;               // once per run: the old names out of both folders before the copy
            if (!retired) { retired = true; themeicons::RetireShipped(rp::PluginDir() + L"\\themes"); themeicons::RetireShipped(Dir()); }
            CopyNewer(rp::PluginDir() + L"\\themes", Dir());
        }

        void Scan()
        {
            Seed();
            g_list.clear();
            g_list.push_back({ "classic", "Classic", L"", "", true, false });
            std::wstring rad = Root() + L"\\radiant\\stylesheet.qss";
            if (FileExists(rad)) g_list.push_back({ themeicons::kClassicTreyarch, themeicons::kClassicTreyarch, rad, "plastique", false, false });
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

        // a theme picked (the combo, or OK for a theme the editor re-saved): APE's flag and hidden checkbox follow it, applied next tick
        void PickTheme(const std::string& name)
        {
            if (name == kEditedName) return;           // the editor's unsaved edit stays the look
            if (g_dlg.box) qt.buttonSetChecked(g_dlg.box, themeicons::IsClassicTreyarch(name));
            SyncFlag(name);
            g_pending = name;
        }
        // OK in Preferences. APE re-runs QT_SetTheme only when the checkbox differs from radiantTheme, so both are set here.
        void OnPrefsOk(void* dialog)
        {
            if (!dialog || dialog != g_dlg.dialog) return;
            if (g_dlg.dxvkBox && rp::host->ui) dxvk::g_pending = rp::host->ui->IsChecked(g_dlg.dxvkBox) ? 1 : 0;   // applied on the next tick
            if (g_dlg.discordBox && rp::host->ui)
            {
                const int mode = rp::host->ui->ComboCurrent(g_dlg.discordBox);
                if (mode >= 0 && mode != discordrpc::GetMode()) discordrpc::SetMode(discordrpc::Mode(mode));
            }
            if (!g_dlg.combo) return;
            int i = qt.comboCurrentIndex(g_dlg.combo);
            if (i < 0 || size_t(i) >= g_dlg.names.size()) return;
            PickTheme(g_dlg.names[size_t(i)]);
        }

        void* QNew(size_t bytes) { void* p = qt.opNew(bytes); if (p) memset(p, 0, bytes); return p; }

        // ---- the Theme editor (plugins/common/themeeditor.cpp), under the Theme row behind "Customize..." ----
        void* g_editor = nullptr;
        bool g_editorShown = false;
        bool g_reapply = false;                // the Theme editor re-saved a theme: OK applies it even when its name is the one in effect
        void AddEditedItem(void* combo, std::vector<std::string>& names)
        {
            void* text = nullptr; qt.qstringFromUtf8(&text, "Edited (not saved)", -1);
            char variant[16] = {}; qt.variantCtor(variant);
            qt.comboAddItem(combo, &text, variant);
            qt.variantDtor(variant); qt.qsDtor(&text);
            names.push_back(kEditedName);
        }
        // the Theme combo shows "Edited (not saved)" while the editor's change is the look (OK keeps it, a theme reverts it)
        void MarkEdited()
        {
            if (!g_dlg.combo) return;
            for (size_t i = 0; i < g_dlg.names.size(); ++i)
                if (g_dlg.names[i] == kEditedName) { qt.comboSetCurrentIndex(g_dlg.combo, int(i)); return; }
            AddEditedItem(g_dlg.combo, g_dlg.names);
            qt.comboSetCurrentIndex(g_dlg.combo, int(g_dlg.names.size()) - 1);
        }
        // a pick in the Theme combo: the editor shows its colours and it is applied at once (next tick), no OK needed
        void OnThemeComboChanged(void* dialog, int index)
        {
            if (dialog != g_dlg.dialog || index < 0 || size_t(index) >= g_dlg.names.size()) return;
            themeedit::ShowTheme(g_dlg.names[size_t(index)]);
            if (!g_fillingCombo) PickTheme(g_dlg.names[size_t(index)]);
        }
        std::string g_editorError;
        // the editor's Preview: a sheet with no theme entry
        void ApplyLook(const std::string& sheetText)
        {
            KnowStartState();
            std::string style = StyleDirective(sheetText);
            if (style.empty()) style = "plastique";
            themeicons::Use(themeicons::PackFor(Dir(), "_preview", sheetText, Dir()));
            themeicons::UseTitleBar(sheetText);
            if (Lower(style) != Lower(g_style) && SetStyle(style)) g_style = style;
            SetSheet(Resolve(sheetText, Dir()));
            g_sheetSet = true;
            ApplyLinkColor(LinkColorFor(sheetText));
        }
        // the Theme combo, refilled after Save As with the new theme selected (not a pick, OK applies it)
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
        void OnCustomize(void*)
        {
            if (!g_editor || !rp::host->ui) return;
            g_editorShown = !g_editorShown;
            rp::host->ui->SetVisible(g_editor, g_editorShown ? 1 : 0);
        }
        void* BuildEditor(void* group, void* dialog)
        {
            themeedit::Host h;
            h.themesDir = Dir();
            h.product = "APE+";
            h.where = "APE: View > Preferences > Customize...";
            h.viewColors = false;
            h.iconRecolor = false;
            h.columns = 1;                                       // APE's Preferences group is narrow
            h.themes = [] { std::vector<themeedit::ThemeRef> v; for (const Entry& e : g_list) v.push_back({ e.name, e.label, e.file }); return v; };
            h.current = [] { return g_applied.empty() ? FlagTheme() : g_applied; };
            h.preview = [](const std::string& sheet) { ApplyLook(sheet); MarkEdited(); };
            h.previewIcons = [](const std::string& lines) { themeicons::Use(themeicons::PackFor(Dir(), "_preview", lines, Dir())); MarkEdited(); };
            h.saved = [](const std::string& name) { Scan(); g_reapply = true; FillThemeCombo(name); };
            void* ed = themeedit::Build(group, dialog, h);
            g_editorError = ed ? "" : themeedit::Error();
            if (!ed) rp::Log("theme: Preferences: editor: %s", g_editorError.c_str());
            return ed;
        }

        void Patch(void* dialog)
        {
            const ModToolsUiApi* ui = rp::host->ui;
            if (!ui) { g_patchError = "no host ui"; return; }
            void* box = *reinterpret_cast<void**>(static_cast<char*>(dialog) + off_Prefs_TreyarchBox);
            if (!box || !IsA(box, "QCheckBox") || TakeString(qt.buttonText, box) != "Use Treyarch Theme") { g_patchError = "the Treyarch checkbox is not where expected"; return; }
            void* group = qt.parentWidget(box);
            void* lay = group ? qt.widgetLayout(group) : nullptr;
            if (!lay || !IsA(lay, "QBoxLayout")) { g_patchError = "the checkbox's group has no box layout"; return; }
            int index = qt.layoutIndexOf(lay, box);
            if (index < 0) { g_patchError = "the checkbox is not in its group's layout"; return; }

            Scan();
            std::string shown = g_applied.empty() ? FlagTheme() : g_applied;
            void* row = ui->HBox();
            ui->LayoutSpacing(row, 6);
            void* label = ui->Label("Theme:", group);
            void* combo = QNew(128);
            qt.comboCtor(combo, group);
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
            if (themeedit::Edited()) { current = int(names.size()); AddEditedItem(combo, names); }   // the editor's unsaved edit in effect
            qt.comboSetCurrentIndex(combo, current);
            ui->SetToolTip(combo, Tip("The look of APE (APE+ plugin). Classic is APE's default, Classic Treyarch is Radiant's stylesheet, Treyarch a modern theme in its colours; "
                                  "more themes are .qss files in bin\\themes. Picking one applies it.").c_str());
            void* folder = ui->PushButton("Themes Folder", group);
            ui->SetToolTip(folder, Tip("Open bin\\themes (shared with Radiant): drop a .qss file there and reopen Preferences").c_str());
            rp::host->OnClicked(rp::self, folder, OnOpenFolder, nullptr);
            ui->BoxAddWidget(row, label, 0);
            ui->BoxAddWidget(row, combo, 1);
            ui->BoxAddWidget(row, folder, 0);
            // the Theme editor, hidden until "Customize..." (the dialog grows while it is shown)
            g_editor = nullptr; g_editorShown = false;
            void* rows = ui->VBox(nullptr);
            ui->BoxAddLayout(rows, row, 0);
            if (void* ed = BuildEditor(group, dialog))
            {
                void* custom = ui->PushButton("Customize...", group);
                ui->SetToolTip(custom, Tip("Make your own theme from eight colors (shows or hides the Theme editor)").c_str());
                rp::host->OnClicked(rp::self, custom, OnCustomize, nullptr);
                ui->BoxAddWidget(row, custom, 0);
                ui->BoxAddWidget(rows, ed, 0);
                ui->SetVisible(ed, 0);
                g_editor = ed;
            }
            qt.boxInsertLayout(lay, index, rows, 0);
            ui->SetVisible(box, 0);

            // Feature 4: the DXVK switch, one setting with RadiantPlus's (the two DLLs in bin)
            const bool dxOn = dxvk::InBin(), dxLive = dxvk::Live();
            void* dxvkBox = ui->CheckBox("Use DXVK (Vulkan) renderer (APE and Radiant, from their next start)", group);
            ui->SetChecked(dxvkBox, dxOn ? 1 : 0);   // read each time the dialog opens, whichever editor changed it
            std::string now = std::string("      This APE is running on ") + (dxLive ? "Vulkan (DXVK)" : "native Direct3D 11");
            if (dxOn != dxLive) now += dxOn ? "; the switch is on, restart APE to use Vulkan" : "; the switch is off, restart APE to go back to Direct3D 11";
            void* dxvkNow = ui->Label(now.c_str(), group);
            ui->SetToolTip(dxvkBox, Tip(dxvk::SourceDir().empty()
                ? "APE+: DXVK's files are missing (plugins\\APEPlus\\dxvk or plugins\\RadiantPlus\\dxvk)"
                : "APE+: Direct3D 11 through Vulkan (DXVK). The same switch as Radiant+'s: DXVK's dxgi.dll and d3d11.dll "
                  "in bin, used by both editors from their next start").c_str());
            if (dxvk::SourceDir().empty() && !dxvk::InBin()) ui->SetEnabled(dxvkBox, 0);
            ui->BoxAddWidget(lay, dxvkBox, 0);
            ui->BoxAddWidget(lay, dxvkNow, 0);
            g_dlg.dxvkBox = dxvkBox; g_dlg.dxvkNow = dxvkNow;

            void* discordRow = ui->HBox();
            void* discordBox = ui->ComboBox(group);
            if (discordBox)
            {
                for (const char* label : discordrpc::kModeLabels) ui->ComboAdd(discordBox, label);
                ui->ComboSetCurrent(discordBox, discordrpc::GetMode());
                ui->SetToolTip(discordBox, Tip("APE+: what your Discord profile shows while APE is in use. Off: nothing. Show the tool: only which tool is open. Show the tool and what is open: also the map, asset or build step. Each tool has its own setting.").c_str());
                ui->BoxAddWidget(discordRow, ui->Label("Discord presence:", group), 0);
                ui->BoxAddWidget(discordRow, discordBox, 0);
                ui->BoxAddStretch(discordRow, 1);
                ui->BoxAddLayout(lay, discordRow, 0);
            }

            bool okHooked = false;
            for (void* child : Children(dialog))
                if (IsA(child, "QDialogButtonBox"))
                    if (void* ok = qt.buttonBoxButton(child, 0x400 /* QDialogButtonBox::Ok */))
                        okHooked = rp::host->OnClicked(rp::self, ok, OnPrefsOk, dialog) != 0;
            if (!okHooked) g_patchError = "no OK button found; the choice cannot be applied";
            else g_patchError.clear();

            g_dlg.dialog = dialog; g_dlg.box = box; g_dlg.combo = combo; g_dlg.dxvkBox = dxvkBox; g_dlg.discordBox = discordBox; g_dlg.okHooked = okHooked; g_dlg.names = std::move(names);
            if (rp::Has(&rp::host->OnChanged)) rp::host->OnChanged(rp::self, combo, OnThemeComboChanged, dialog);   // the editor shows the pick
            ++g_patched;
        }
        bool PatchGuarded(void* dialog)
        {
            __try { Patch(dialog); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        void* __fastcall hk_PrefsCtor(void* self, void* parent)
        {
            void* r = g_origCtor(self, parent);
            g_dlg = Dialog{};
            if (self && !PatchGuarded(self)) { g_hooked = false; g_patchError = "the Preferences patch faulted; APE's own checkbox stays"; rp::Log("theme: %s", g_patchError.c_str()); }
            else if (!g_patchError.empty()) rp::Log("theme: Preferences: %s", g_patchError.c_str());
            return r;
        }

        void Install()
        {
            if (!rp::Ape() || !qt.bound) return;
            themeicons::SetRecolor(false);   // APE's icons keep their colours under every theme
            themeicons::Setup(Root());
            // the flag: only trusted when the slot's instruction still addresses it
            void* code = rp::Ape()->ApeAddress(rp::self, kFlagLoad);
            void* flag = rp::Ape()->ApeAddress(rp::self, kRadiantTheme);
            if (code && flag)
            {
                const uint8_t* b = static_cast<const uint8_t*>(code);
                int32_t disp = 0; memcpy(&disp, b + 3, 4);
                if (b[0] == 0x0F && b[1] == 0xB6 && b[2] == 0x15 && b + 7 + disp == static_cast<const uint8_t*>(flag)) g_flag = static_cast<uint8_t*>(flag);
            }
            if (!g_flag) rp::Log("theme: APE's theme flag not recognised; Treyarch will not be remembered by APE itself");
            void* target = rp::Ape()->ApeAddress(rp::self, kPrefsCtor);
            DWORD64 base = 0; PRUNTIME_FUNCTION fn = target ? RtlLookupFunctionEntry(DWORD64(target), &base, nullptr) : nullptr;
            if (!fn || DWORD64(fn->BeginAddress) + base != DWORD64(target)) { rp::Log("theme: Preferences dialog not found, the theme picker is off (command 'theme <name>' still works)"); return; }
            if (!rp::host->Detour(rp::self, target, reinterpret_cast<void*>(&hk_PrefsCtor), reinterpret_cast<void**>(&g_origCtor))) { rp::Log("theme: detour of the Preferences dialog failed"); return; }
            g_ctorLive = target;
            g_hooked = true;
            rp::Log("theme: Preferences theme picker installed (flag %s)", g_flag ? "found" : "missing");
        }

        // first tick with the main window on screen (APE's Init has run by then), later ticks apply a Preferences choice
        void Tick()
        {
            if (!qt.bound) return;
            if (!g_pending.empty())
            {
                std::string name = g_pending, why; g_pending.clear();
                if (name == kEditedName) return;    // OK with "Edited (not saved)": the edit stays
                // the theme in effect again: nothing to do, unless an edit is on (then this reverts it) or it was re-saved
                if (Lower(name) == Lower(g_applied) && !g_reapply && !themeedit::Edited()) return;
                g_reapply = false;
                if (Apply(name, why)) { g_current = g_applied; rp::ConfigSet("theme", "name", g_current); }
                else { g_lastError = why; rp::Log("theme: %s", why.c_str()); }
                return;
            }
            if (g_started) return;
            HWND main = static_cast<HWND>(rp::host->MainWindow(rp::self));
            if (!main || !IsWindowVisible(main)) return;
            g_started = true;
            Scan();
            if (g_current == themeicons::kUnset)   // first run with the plugin: Treyarch where APE showed its Treyarch look, else Light
            {
                g_current = themeicons::FirstRunTheme(g_flag && *g_flag);
                rp::ConfigSet("theme", "name", g_current);
                rp::Log("theme: first run, '%s' picked", g_current.c_str());
            }
            std::string l = Lower(g_current);
            if (l.empty() || l == "default") { g_applied = FlagTheme(); themeicons::Use(themeicons::PackFor(Dir(), g_applied, "", L"")); themeicons::UseTitleBar("", g_applied); return; }
            std::string why;
            if (!Apply(g_current, why)) { g_lastError = why; rp::Log("theme: %s", why.c_str()); g_applied = FlagTheme(); }
        }

        // builds a Preferences dialog that is never shown (through the detour), checks the patch, renders it to a PNG
        // and deletes it: verifies the picker inside a running APE without a window on screen
        std::string SelfTest(const std::string& png)
        {
            if (!g_hooked || !g_ctorLive) return "the Preferences hook is not installed";
            void* dialog = QNew(0x400);
            g_patchError.clear();
            reinterpret_cast<PrefsCtor>(g_ctorLive)(dialog, nullptr);
            std::string out;
            char buf[512];
            if (g_dlg.dialog != dialog) out += "patch did not run: " + g_patchError + "\n";
            else
            {
                snprintf(buf, sizeof buf, "patched: checkbox hidden=%d, combo in group=%d, items=%d current=%d, ok button hooked=%d, flag=%s\n",
                         qt.isVisibleTo(g_dlg.box, qt.parentWidget(g_dlg.box)) ? 0 : 1, qt.parentWidget(g_dlg.combo) == qt.parentWidget(g_dlg.box) ? 1 : 0,
                         qt.comboCount(g_dlg.combo), qt.comboCurrentIndex(g_dlg.combo), g_dlg.okHooked ? 1 : 0, g_flag ? (*g_flag ? "on" : "off") : "missing");
                out += buf;
                snprintf(buf, sizeof buf, "dxvk checkbox: present=%d in group=%d checked=%d (setting in bin %s, live %s); line: '%s'\n", g_dlg.dxvkBox ? 1 : 0,
                         g_dlg.dxvkBox && qt.parentWidget(g_dlg.dxvkBox) == qt.parentWidget(g_dlg.box) ? 1 : 0,
                         g_dlg.dxvkBox && rp::host->ui ? rp::host->ui->IsChecked(g_dlg.dxvkBox) : -1, dxvk::InBin() ? "on" : "off", dxvk::Live() ? "vulkan" : "d3d11",
                         g_dlg.dxvkNow ? TakeString(qt.labelText, g_dlg.dxvkNow).c_str() : "");
                out += buf;
                for (int i = 0; i < qt.comboCount(g_dlg.combo); ++i)
                {
                    void* qs = nullptr; qt.comboItemText(g_dlg.combo, &qs, i);
                    out += "  item " + std::to_string(i) + ": '" + QStringToUtf8(&qs) + "' -> " + (size_t(i) < g_dlg.names.size() ? g_dlg.names[size_t(i)] : "?") + "\n";
                    qt.qsDtor(&qs);
                }
                if (!g_patchError.empty()) out += "note: " + g_patchError + "\n";
            }
            out += g_editor ? "theme editor: present (shown for the render)\n" : "theme editor: missing (" + g_editorError + ")\n";
            if (g_editor && rp::host->ui) rp::host->ui->SetVisible(g_editor, 1);
            if (!png.empty() && qt.grab && qt.pixmapSave && qt.pixmapDtor)
            {
                qt.adjustSize(dialog);
                int rect[4] = { 0, 0, -2, -2 };                   // QRect(QPoint(0, 0), QSize(-1, -1)): the whole widget
                char pixmap[64] = {};
                qt.grab(dialog, pixmap, rect);
                void* path = nullptr; qt.qstringFromUtf8(&path, png.c_str(), int(png.size()));
                bool saved = qt.pixmapSave(pixmap, &path, "PNG", -1);
                qt.qsDtor(&path);
                qt.pixmapDtor(pixmap);
                out += saved ? "rendered to " + png + "\n" : "render to " + png + " failed\n";
            }
            g_dlg = Dialog{};
            g_editor = nullptr; g_editorShown = false;
            themeedit::Reset();
            qt.deleteLater(dialog);
            return out;
        }

        // test hook: a Preferences dialog that is never shown, <name> picked in its Theme combo (as a user's pick: no OK)
        std::string PickTest(const std::string& name)
        {
            if (!g_hooked || !g_ctorLive) return "the Preferences hook is not installed";
            void* dialog = QNew(0x400);
            reinterpret_cast<PrefsCtor>(g_ctorLive)(dialog, nullptr);
            std::string out = "patch did not run: " + g_patchError;
            if (g_dlg.dialog == dialog && g_dlg.combo)
            {
                int at = -1;
                for (size_t i = 0; i < g_dlg.names.size(); ++i) if (Lower(g_dlg.names[i]) == Lower(name)) at = int(i);
                if (at < 0) out = "no theme '" + name + "' in the combo";
                else { qt.comboSetCurrentIndex(g_dlg.combo, at); out = "picked '" + g_dlg.names[size_t(at)] + "': pending '" + g_pending + "' (applied on the next tick)\neditor: " + themeedit::Command(""); }
            }
            g_dlg = Dialog{};
            g_editor = nullptr; g_editorShown = false;
            themeedit::Reset();
            qt.deleteLater(dialog);
            return out;
        }
        // test hook: a Preferences dialog that is never shown, its DXVK box set, then the handler the OK button runs
        std::string PrefsOkTest(bool dxOn)
        {
            if (!g_hooked || !g_ctorLive || !rp::host->ui) return "the Preferences hook is not installed";
            void* dialog = QNew(0x400);
            reinterpret_cast<PrefsCtor>(g_ctorLive)(dialog, nullptr);
            std::string out;
            if (g_dlg.dialog != dialog || !g_dlg.dxvkBox) out = "patch did not run: " + g_patchError;
            else { rp::host->ui->SetChecked(g_dlg.dxvkBox, dxOn ? 1 : 0); OnPrefsOk(dialog); out = std::string("OK handled with the box ") + (dxOn ? "checked" : "unchecked") + "; applied on the next tick"; }
            g_dlg = Dialog{};
            qt.deleteLater(dialog);
            return out;
        }

        std::string List()
        {
            std::string s;
            for (const Entry& e : g_list)
            {
                s += e.name;
                if (Lower(e.name) == Lower(g_applied)) s += " (applied)";
                if (!e.file.empty()) s += "  " + Narrow(e.file);
                s += "\n";
            }
            return s.empty() ? "no themes scanned yet" : s;
        }
        std::string Status()
        {
            char buf[768];
            snprintf(buf, sizeof buf, "theme: ini='%s' applied='%s' themes=%zu preferences hook=%s (dialogs patched %d%s%s) flag=%s last apply %.0f ms%s%s\nfolder: %s",
                     g_current.c_str(), g_applied.c_str(), g_list.size(), g_hooked ? "on" : "off", g_patched,
                     g_patchError.empty() ? "" : ", last problem: ", g_patchError.c_str(),
                     g_flag ? (*g_flag ? "on" : "off") : "missing", g_lastMs,
                     g_lastError.empty() ? "" : " last error: ", g_lastError.c_str(), Narrow(Dir()).c_str());
            return buf;
        }
    }

    // Diagnostics: which files a click opens ("iotrace <ms>", report in the log and iotrace.txt). Detours KernelBase's
    // CreateFile, GetFileAttributes(Ex) and FindFirstFileEx (W and A), installed on first use.
    namespace iotrace
    {
        using CreateW = HANDLE (WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
        using CreateA = HANDLE (WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
        using AttrExW = BOOL (WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
        using AttrExA = BOOL (WINAPI*)(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
        using AttrW = DWORD (WINAPI*)(LPCWSTR);
        using AttrA = DWORD (WINAPI*)(LPCSTR);
        using FindW = HANDLE (WINAPI*)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
        using FindA = HANDLE (WINAPI*)(LPCSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
        CreateW g_cw = nullptr; CreateA g_ca = nullptr; AttrExW g_axw = nullptr; AttrExA g_axa = nullptr; AttrW g_aw = nullptr; AttrA g_aa = nullptr; FindW g_fw = nullptr; FindA g_fa = nullptr;
        bool g_installed = false;
        std::atomic<bool> g_on{ false };
        DWORD g_mainTid = 0;
        uint64_t g_until = 0;
        struct Row { int calls = 0, main = 0, failed = 0; double ms = 0; };
        std::mutex g_mutex;
        std::map<std::string, Row> g_rows;   // key: "<api> <path>"
        std::string W2U(LPCWSTR w) { if (!w) return "(null)"; int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr); std::string o(n > 0 ? n - 1 : 0, '\0'); if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &o[0], n, nullptr, nullptr); return o; }
        void Note(const char* api, const std::string& path, uint64_t t0, bool failed)
        {
            const double ms = Ms(Now() - t0);
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_rows.size() >= 20000) return;
            Row& r = g_rows[std::string(api) + " " + path];
            ++r.calls; r.ms += ms; if (failed) ++r.failed; if (GetCurrentThreadId() == g_mainTid) ++r.main;
        }
        HANDLE WINAPI hk_cw(LPCWSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t)
        { if (!g_on) return g_cw(p, a, s, sa, c, f, t); const uint64_t t0 = Now(); HANDLE h = g_cw(p, a, s, sa, c, f, t); const DWORD e = GetLastError(); Note("open", W2U(p), t0, h == INVALID_HANDLE_VALUE); SetLastError(e); return h; }
        HANDLE WINAPI hk_ca(LPCSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t)
        { if (!g_on) return g_ca(p, a, s, sa, c, f, t); const uint64_t t0 = Now(); HANDLE h = g_ca(p, a, s, sa, c, f, t); const DWORD e = GetLastError(); Note("openA", p ? p : "(null)", t0, h == INVALID_HANDLE_VALUE); SetLastError(e); return h; }
        BOOL WINAPI hk_axw(LPCWSTR p, GET_FILEEX_INFO_LEVELS l, LPVOID d)
        { if (!g_on) return g_axw(p, l, d); const uint64_t t0 = Now(); BOOL r = g_axw(p, l, d); const DWORD e = GetLastError(); Note("attrEx", W2U(p), t0, !r); SetLastError(e); return r; }
        BOOL WINAPI hk_axa(LPCSTR p, GET_FILEEX_INFO_LEVELS l, LPVOID d)
        { if (!g_on) return g_axa(p, l, d); const uint64_t t0 = Now(); BOOL r = g_axa(p, l, d); const DWORD e = GetLastError(); Note("attrExA", p ? p : "(null)", t0, !r); SetLastError(e); return r; }
        DWORD WINAPI hk_aw(LPCWSTR p)
        { if (!g_on) return g_aw(p); const uint64_t t0 = Now(); DWORD r = g_aw(p); const DWORD e = GetLastError(); Note("attr", W2U(p), t0, r == INVALID_FILE_ATTRIBUTES); SetLastError(e); return r; }
        DWORD WINAPI hk_aa(LPCSTR p)
        { if (!g_on) return g_aa(p); const uint64_t t0 = Now(); DWORD r = g_aa(p); const DWORD e = GetLastError(); Note("attrA", p ? p : "(null)", t0, r == INVALID_FILE_ATTRIBUTES); SetLastError(e); return r; }
        HANDLE WINAPI hk_fw(LPCWSTR p, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD fl)
        { if (!g_on) return g_fw(p, l, d, o, f, fl); const uint64_t t0 = Now(); HANDLE h = g_fw(p, l, d, o, f, fl); const DWORD e = GetLastError(); Note("find", W2U(p), t0, h == INVALID_HANDLE_VALUE); SetLastError(e); return h; }
        HANDLE WINAPI hk_fa(LPCSTR p, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD fl)
        { if (!g_on) return g_fa(p, l, d, o, f, fl); const uint64_t t0 = Now(); HANDLE h = g_fa(p, l, d, o, f, fl); const DWORD e = GetLastError(); Note("findA", p ? p : "(null)", t0, h == INVALID_HANDLE_VALUE); SetLastError(e); return h; }
        bool Install()
        {
            if (g_installed) return true;
            HMODULE k = GetModuleHandleW(L"kernelbase.dll");
            if (!k) return false;
            struct { const char* name; void* hook; void** orig; } list[] = {
                { "CreateFileW", reinterpret_cast<void*>(&hk_cw), reinterpret_cast<void**>(&g_cw) }, { "CreateFileA", reinterpret_cast<void*>(&hk_ca), reinterpret_cast<void**>(&g_ca) },
                { "GetFileAttributesExW", reinterpret_cast<void*>(&hk_axw), reinterpret_cast<void**>(&g_axw) }, { "GetFileAttributesExA", reinterpret_cast<void*>(&hk_axa), reinterpret_cast<void**>(&g_axa) },
                { "GetFileAttributesW", reinterpret_cast<void*>(&hk_aw), reinterpret_cast<void**>(&g_aw) }, { "GetFileAttributesA", reinterpret_cast<void*>(&hk_aa), reinterpret_cast<void**>(&g_aa) },
                { "FindFirstFileExW", reinterpret_cast<void*>(&hk_fw), reinterpret_cast<void**>(&g_fw) },
            };
            for (auto& e : list)
            {
                void* f = reinterpret_cast<void*>(GetProcAddress(k, e.name));
                if (!f || !rp::host->Detour(rp::self, f, e.hook, e.orig)) { rp::Log("iotrace: %s not hooked", e.name); return false; }
            }
            // FindFirstFileExA may already carry the commit trace's detour: chain through it instead of a second detour
            void* fa = reinterpret_cast<void*>(GetProcAddress(k, "FindFirstFileExA"));
            if (fa && !commit::g_origFind && rp::host->Detour(rp::self, fa, reinterpret_cast<void*>(&hk_fa), reinterpret_cast<void**>(&g_fa))) {}
            g_installed = true;
            return true;
        }
        std::string Start(int ms)
        {
            if (!Install()) return "iotrace: hooks failed";
            { std::lock_guard<std::mutex> lock(g_mutex); g_rows.clear(); }
            g_mainTid = GetCurrentThreadId();   // commands run on the main thread
            LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_until = Now() + uint64_t(ms) * uint64_t(f.QuadPart) / 1000;
            g_on = true;
            return "iotrace: recording " + std::to_string(ms) + " ms";
        }
        void Tick()
        {
            if (!g_on || Now() < g_until) return;
            g_on = false;
            std::vector<std::pair<std::string, Row>> rows;
            { std::lock_guard<std::mutex> lock(g_mutex); rows.assign(g_rows.begin(), g_rows.end()); }
            std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
            int calls = 0, main = 0, repeats = 0; double ms = 0, mainMs = 0;
            for (auto& r : rows) { calls += r.second.calls; main += r.second.main; ms += r.second.ms; if (r.second.main) mainMs += r.second.ms * r.second.main / r.second.calls; if (r.second.calls > 1) repeats += r.second.calls - 1; }
            std::string out;
            char line[1200];
            snprintf(line, sizeof line, "iotrace: %d calls on %zu paths (%d on the main thread), %d repeats of a path already seen, %.1f ms inside the calls (main thread ~%.1f ms)\n", calls, rows.size(), main, repeats, ms, mainMs);
            out += line;
            out += "  ms      calls main fail  api path\n";
            for (auto& r : rows) { snprintf(line, sizeof line, "  %7.2f %5d %4d %4d  %s\n", r.second.ms, r.second.calls, r.second.main, r.second.failed, r.first.c_str()); out += line; }
            std::wstring path = rp::PluginDir() + L"\\iotrace.txt";
            if (FILE* f = _wfopen(path.c_str(), L"wb")) { fwrite(out.data(), 1, out.size(), f); fclose(f); }
            rp::Log("%s", out.substr(0, 1500).c_str());
        }
    }

    // ---- Feature 9: the plugin's own .awi scripts ----
    // A QAbstractFileEngineHandler answers <any>\deffiles\<file> with plugins\APEPlus\deffiles\<file> when it exists.
    // The folder is checked on every read, and APE's compile cache sees the other file's time, so a change recompiles.
    namespace awi
    {
        struct Fns
        {
            bool bound = false;
            void* (*qsFromWChar)(void*, const wchar_t*, int) = nullptr;   // static QString::fromWCharArray, hidden return
            void (*qsDtor)(void*) = nullptr;
            void* (*engineCreate)(const void*) = nullptr;                   // static QAbstractFileEngine::create(const QString&)
            void* (*handlerCtor)(void*) = nullptr;
            void (*handlerDtor)(void*) = nullptr;
            void** handlerVtbl = nullptr;
        } q;
        std::atomic<bool> g_on{ false };
        bool g_registered = false;
        std::wstring g_dir, g_dirKey;                                       // plugins\APEPlus\deffiles, its key, set before registering
        std::atomic<int> g_hits{ 0 };

        std::wstring Key(const wchar_t* w, int n)   // lower case, '/' separators, no doubled separators
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
        // the plugin's file for an editor path, "" when the folder has none (checked on disk now: the installed file then)
        std::wstring Lookup(const wchar_t* w, int n)
        {
            if (!w || n < 14 || n > 2048 || g_dir.empty()) return L"";
            std::wstring k = Key(w, n);
            size_t at = k.rfind(L"/deffiles/");
            if (at == std::wstring::npos || at + 10 >= k.size() || k.find(L'/', at + 10) != std::wstring::npos) return L"";
            if (k.compare(0, g_dirKey.size(), g_dirKey) == 0) return L"";   // our own file: Qt's engine
            std::wstring file = g_dir + L"\\" + k.substr(at + 10);
            const DWORD a = GetFileAttributesW(file.c_str());
            return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY) ? file : L"";
        }
        // QAbstractFileEngineHandler::create(const QString&) const, Qt holds its recursive handler lock around it
        void* __fastcall H_Create(const void*, const void* name) noexcept
        {
            try
            {
                if (!g_on.load(std::memory_order_acquire)) return nullptr;
                const QArrayDataRaw* d = *static_cast<const QArrayDataRaw* const*>(name);
                if (!d || d->size <= 0) return nullptr;
                std::wstring hit = Lookup(reinterpret_cast<const wchar_t*>(reinterpret_cast<const char*>(d) + d->offset), d->size);
                if (hit.empty()) return nullptr;
                ++g_hits;
                void* qs = nullptr;
                q.qsFromWChar(&qs, hit.c_str(), int(hit.size()));
                void* e = q.engineCreate(&qs);      // asks the handlers again with our path: none answers, Qt's file engine
                q.qsDtor(&qs);
                return e;
            }
            catch (...) { return nullptr; }
        }
        void* __fastcall H_Dtor(void* self, unsigned) noexcept { return self; }   // Qt never deletes a handler
        void* g_vt[3] = { nullptr, reinterpret_cast<void*>(&H_Dtor), reinterpret_cast<void*>(&H_Create) };   // [-1] = Qt's RTTI locator
        alignas(16) unsigned char g_handler[64];
        struct ExitGuard { ~ExitGuard() { g_on.store(false); } } g_exitGuard;   // CRT teardown at quit: answer nothing

        // Edit AWI hands "<gamepath>\deffiles\<type>.awi" to ShellExecuteA. While the override is on, the file APE compiled is the
        // plugin's copy, so that one opens instead, every other ShellExecuteA call passes through.
        using ShellExecuteAFn = HINSTANCE (WINAPI*)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT);
        ShellExecuteAFn g_origShell = nullptr;
        std::atomic<int> g_edits{ 0 };
        static std::wstring WideAcp(const char* s)
        {
            std::wstring w;
            if (!s || !*s) return w;
            int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
            if (n > 1) { w.resize(size_t(n - 1)); MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n); }
            return w;
        }
        HINSTANCE WINAPI hk_ShellExecuteA(HWND hwnd, LPCSTR op, LPCSTR file, LPCSTR params, LPCSTR dir, INT show)
        {
            try
            {
                if (g_on.load(std::memory_order_acquire) && file && *file)
                {
                    std::wstring wfile = WideAcp(file);
                    std::wstring hit = Lookup(wfile.c_str(), int(wfile.size()));
                    if (!hit.empty())
                    {
                        ++g_edits;
                        rp::Log("awi: Edit AWI opens %ls (the file APE uses) instead of %s", hit.c_str(), file);
                        std::wstring wop = WideAcp(op), wparams = WideAcp(params), wdir = WideAcp(dir);
                        return ShellExecuteW(hwnd, op ? wop.c_str() : nullptr, hit.c_str(), params ? wparams.c_str() : nullptr, dir ? wdir.c_str() : nullptr, show);
                    }
                }
            }
            catch (...) {}
            return g_origShell(hwnd, op, file, params, dir, show);
        }
        void InstallEditHook()
        {
            if (g_origShell) return;
            HMODULE sh = GetModuleHandleW(L"shell32.dll");
            void* f = sh ? reinterpret_cast<void*>(GetProcAddress(sh, "ShellExecuteA")) : nullptr;   // the exe's import, bound to this address
            if (!f) { rp::Log("awi: no shell32!ShellExecuteA, Edit AWI opens the installed file"); return; }
            if (!rp::host->Detour(rp::self, f, reinterpret_cast<void*>(&hk_ShellExecuteA), reinterpret_cast<void**>(&g_origShell)))
                rp::Log("awi: ShellExecuteA detour failed, Edit AWI opens the installed file");
        }

        std::vector<std::string> Files()   // what the folder holds now
        {
            std::vector<std::string> names;
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((g_dir + L"\\*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) return names;
            do
            {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::string n; for (const wchar_t* c = fd.cFileName; *c; ++c) n += char(*c < 128 ? *c : '?');
                names.push_back(n);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
            std::sort(names.begin(), names.end());
            return names;
        }
        bool Install(bool on)
        {
            if (!g_registered)
            {
                HMODULE c = GetModuleHandleW(L"Qt5Core64r.dll");
                if (!c) { rp::Log("awi: Qt5Core64r.dll not loaded"); return false; }
                q.qsFromWChar = reinterpret_cast<decltype(q.qsFromWChar)>(GetProcAddress(c, "?fromWCharArray@QString@@SA?AV1@PEB_WH@Z"));
                q.qsDtor = reinterpret_cast<decltype(q.qsDtor)>(GetProcAddress(c, "??1QString@@QEAA@XZ"));
                q.engineCreate = reinterpret_cast<decltype(q.engineCreate)>(GetProcAddress(c, "?create@QAbstractFileEngine@@SAPEAV1@AEBVQString@@@Z"));
                q.handlerCtor = reinterpret_cast<decltype(q.handlerCtor)>(GetProcAddress(c, "??0QAbstractFileEngineHandler@@QEAA@XZ"));
                q.handlerDtor = reinterpret_cast<decltype(q.handlerDtor)>(GetProcAddress(c, "??1QAbstractFileEngineHandler@@UEAA@XZ"));
                q.handlerVtbl = reinterpret_cast<void**>(GetProcAddress(c, "??_7QAbstractFileEngineHandler@@6B@"));
                if (!q.qsFromWChar || !q.qsDtor || !q.engineCreate || !q.handlerCtor || !q.handlerDtor || !q.handlerVtbl) { rp::Log("awi: Qt exports missing"); return false; }
                g_dir = rp::PluginDir() + L"\\deffiles";
                g_dirKey = Key(g_dir.c_str(), int(g_dir.size())) + L"/";
                g_vt[0] = q.handlerVtbl[-1];
                q.handlerCtor(g_handler);           // registers it (first in Qt's list) with Qt's abstract vtable
                *reinterpret_cast<void**>(g_handler) = &g_vt[1];
                g_registered = true;
                InstallEditHook();
            }
            g_on.store(on, std::memory_order_release);
            rp::Log("awi: %s, %zu files in plugins\\APEPlus\\deffiles now", on ? "on" : "off", Files().size());
            return true;
        }
        void Uninstall()
        {
            g_on.store(false);
            if (!g_registered) return;
            q.handlerDtor(g_handler);               // unregisters it under Qt's write lock: no create() runs after this
            g_registered = false;
        }
        std::string Status()
        {
            std::string out = std::string("awi: ") + (g_on ? "on" : "off") + (g_registered ? "" : " (handler not registered)") + ", "
                + std::to_string(g_hits.load()) + " reads redirected, " + std::to_string(g_edits.load()) + " Edit AWI opens redirected"
                + (g_origShell ? "" : " (ShellExecuteA not hooked)") + "; files:";
            std::vector<std::string> names = Files();
            for (auto& n : names) out += " " + n;
            if (names.empty()) out += " (none)";
            return out;
        }
        // "awi [status|on|off|path <file>]", path answers the file APE reads for deffiles\<file>, "" = the installed one
        std::string Command(const std::string& args)
        {
            if (args == "on" || args == "off")
            {
                if (!Install(args == "on")) return "awi: could not register the redirect (see the log)";
                rp::ConfigSet("deffiles", "override", args == "on" ? "1" : "0");
                return Status() + " (an asset tab opened before the switch keeps its grid until it is reopened)";
            }
            if (args.rfind("path ", 0) == 0)
            {
                if (!g_on) return "";
                std::wstring w = L"x/deffiles/";
                for (char ch : args.substr(5)) w += wchar_t(static_cast<unsigned char>(ch));
                std::wstring hit = Lookup(w.c_str(), int(w.size()));
                std::string out;
                if (!hit.empty()) { int n = WideCharToMultiByte(CP_UTF8, 0, hit.c_str(), -1, nullptr, 0, nullptr, nullptr); out.resize(n > 0 ? n - 1 : 0); if (n > 1) WideCharToMultiByte(CP_UTF8, 0, hit.c_str(), -1, &out[0], n, nullptr, nullptr); }
                return out;
            }
            return Status();
        }
    }

    // ---- Discord presence ----
    // detail mode: "APE: editing <asset>" with the asset type underneath
    discordrpc::Activity DiscordActivity()
    {
        discordrpc::Activity a;
        const ModToolsApeApi* ape = rp::Ape();
        ModToolsApeAsset cur = {};
        if (!ape || !ape->CurrentAsset || !ape->CurrentAsset(rp::self, &cur) || !cur.name[0]) { a.details = "APE: browsing assets"; return a; }
        a.details = std::string("APE: editing ") + cur.name;
        a.state = cur.type;
        return a;
    }

    // ---- plugin callbacks ----
    int OnLoad(void)
    {
        if (!rp::InApe()) { rp::Log("APEPlus is for APE only"); return 0; }
        BindQt();
        filt::g_fast = rp::ConfigGetBool("filter", "fast", true);
        filt::g_debounceMs = rp::ConfigGetInt("filter", "debounce_ms", 150);
        filt::g_fastValue = rp::ConfigGetBool("filter", "fast_value", true);
        if (filt::g_debounceMs < 0) filt::g_debounceMs = 0; if (filt::g_debounceMs > 2000) filt::g_debounceMs = 2000;
        rp::Log("loaded; qt exports %s", qt.bound ? "bound" : "MISSING");
        ba::filesearch::Install();
        filt::Install();
        commit::g_minS = rp::ConfigGetInt("commit", "scan_min_s", 10); if (commit::g_minS < 0) commit::g_minS = 0; if (commit::g_minS > 3600) commit::g_minS = 3600;
        commit::Install();
        theme::g_current = rp::ConfigGet("theme", "name", themeicons::kUnset);   // kUnset: the first run, decided on the first tick
        if (theme::g_current != themeicons::kUnset) theme::g_current = themeicons::Renamed(theme::g_current);   // a retired shipped theme -> its successor
        theme::Install();
        preview::g_maxFps = rp::ConfigGetInt("preview", "max_fps", 60); if (preview::g_maxFps < 0) preview::g_maxFps = 0; if (preview::g_maxFps > 1000) preview::g_maxFps = 1000;
        preview::Install();
        sky::g_pick = rp::ConfigGet("preview", "skybox", "");
        sky::Store(rp::ConfigGetBool("preview", "skybox_on", false), sky::g_pick);
        sky::Install();
        convert::g_on = rp::ConfigGetBool("preview", "fast_convert", true);
        convert::Install();
        ba::vcache::Install();
        autolod::Install();
        formats::Configure(rp::ConfigGet("images", "extra_formats", "jpg,jpeg"));
        formats::Install();
        fastopen::g_on = rp::ConfigGetBool("open", "lazy_popups", true);
        fastopen::g_swapOn = rp::ConfigGetBool("open", "fast_swap", true);
        fastopen::g_thumbOn = rp::ConfigGetBool("open", "thumb_cache", true);
        { int mb = rp::ConfigGetInt("open", "thumb_cache_mb", 128); fastopen::g_thumbCap = size_t(mb < 16 ? 16 : (mb > 2048 ? 2048 : mb)) << 20; }
        fastopen::Install();
        awi::Install(rp::ConfigGetBool("deffiles", "override", true));
        ba::actionlog::Setup();
        ba::colorpicker::Install();
        ba::notesounds::Install();
        ba::previewfix::Install();
        ba::weaponpreview::Install();
        discordrpc::Setup("ape", "APE (Asset Property Editor)");
        return 1;
    }

    void OnUnload(void)
    {
        discordrpc::Shutdown();
        ba::actionlog::Shutdown();
        ba::colorpicker::Shutdown();
        ba::notesounds::Shutdown();
        ba::previewfix::Shutdown();
        ba::weaponpreview::Shutdown();
        themeicons::Shutdown();
        awi::Uninstall();
        sky::Uninstall();
        commit::StopWatcher();
        formats::Uninstall();
        fastopen::DropAll();
        filt::Uninstall();
        g_sampler.running = false;
        if (g_sampler.thread.joinable()) g_sampler.thread.join();
    }

    void OnTick(void)
    {
        filt::Tick();
        theme::Tick();
        dxvk::Tick();
        preview::Tick();
        sky::Tick();
        iotrace::Tick();
        fastopen::Tick();
        ba::actionlog::Tick();
        ba::filesearch::Tick();
        ba::notesounds::Tick();
        ba::weaponpreview::Tick();
        if (discordrpc::Due()) discordrpc::Update(DiscordActivity());
        uint64_t now = Now();
        if (g_tickPrev) { uint64_t gap = now - g_tickPrev; if (gap > g_tickMaxGap) { g_tickMaxGap = gap; g_tickMaxAt = now; } }
        g_tickPrev = now;
        if (g_typing.active)
        {
            uint64_t gap = now - g_typing.lastTick; if (gap > g_typing.maxGap) g_typing.maxGap = gap;
            g_typing.lastTick = now;
            std::string t = TakeString(qt.lineEditText, g_typing.edit);
            double elapsed = Ms(now - g_typing.start);
            bool special = g_typing.expect.find('{') != std::string::npos;   // special keys: no expected text, report after a fixed window
            bool done = special ? elapsed > 3000.0 : (t.size() >= g_typing.expect.size() && t.compare(t.size() - g_typing.expect.size(), g_typing.expect.size(), g_typing.expect) == 0);
            if (done || elapsed > 20000.0)
            {
                char buf[512];
                snprintf(buf, sizeof buf, "%s: %d keys in %.0f ms (%.0f ms/key), longest main-thread stall %.0f ms, text now '%s'",
                         done ? (special ? "keys sent" : "typed") : "TIMEOUT", g_typing.keys, elapsed, elapsed / std::max(1, g_typing.keys), Ms(g_typing.maxGap), t.c_str());
                g_typeResult = buf;
                rp::Log("%s", buf);
                g_typing.active = false;
            }
        }
        if (g_sampler.done && !g_sampler.report.empty())
        {
            rp::Log("profile:\n%s", g_sampler.report.c_str());
            g_sampler.report.clear();
        }
    }

    const char* OnCommand(const char* cmd)
    {
        static std::string reply;
        std::string c = cmd;
        char buf[512];
        if (ba::actionlog::Command(c, reply)) return reply.c_str();
        if (ba::colorpicker::Command(c, reply)) return reply.c_str();
        if (ba::filesearch::Command(c, reply)) return reply.c_str();
        if (ba::vcache::Command(c, reply)) return reply.c_str();
        if (ba::notesounds::Command(c, reply)) return reply.c_str();
        if (ba::previewfix::Command(c, reply)) return reply.c_str();
        if (ba::weaponpreview::Command(c, reply)) return reply.c_str();
        if (discordrpc::Command(c, reply)) return reply.c_str();
        if (c == "edits" || c == "edits all")
        {
            reply.clear();
            std::vector<EditRef> e = Edits(c == "edits all");
            for (size_t i = 0; i < e.size(); ++i)
            {
                snprintf(buf, sizeof buf, "%zu  %s%s%s  tip='%s'  = '%s'\n", i, e[i].kind.c_str(), e[i].visible ? "" : " hidden", e[i].enabled ? "" : " disabled", e[i].tip.c_str(), e[i].text.c_str());
                reply += buf;
            }
            if (reply.empty()) reply = "no text controls (no asset tab current?)";
            return reply.c_str();
        }
        if (c.rfind("type ", 0) == 0)
        {
            size_t sp = c.find(' ', 5);
            if (sp == std::string::npos) { reply = "usage: type <edit index> <text>"; return reply.c_str(); }
            int idx = atoi(c.substr(5, sp - 5).c_str());
            std::string text = c.substr(sp + 1);
            std::vector<EditRef>& e = g_lastEdits;
            if (idx < 0 || size_t(idx) >= e.size()) { reply = "no such edit (run 'edits' or 'edits all' first)"; return reply.c_str(); }
            void* edit = e[idx].edit;
            void* top = qt.window(edit);
            HWND hwnd = top ? reinterpret_cast<HWND>(qt.effectiveWinId(top)) : nullptr;
            if (!hwnd) { reply = "the edit has no native window"; return reply.c_str(); }
            qt.setFocus(edit, 7 /* Qt::OtherFocusReason */);
            std::string why;
            g_typing = Typing{};
            g_typing.active = true; g_typing.edit = edit; g_typing.expect = text; g_typing.start = g_typing.lastTick = Now(); g_typing.keys = int(text.size());
            g_typeResult.clear();
            if (!PostKeys(hwnd, text, why)) { g_typing.active = false; reply = "failed: " + why; return reply.c_str(); }
            snprintf(buf, sizeof buf, "posted %zu keys to edit %d (%s, focus %s) hwnd %p", text.size(), idx, e[idx].kind.c_str(), qt.focusWidget() == edit ? "ok" : "NOT on the edit", hwnd);
            reply = buf; return reply.c_str();
        }
        if (c == "tree") { reply = TreeReport(); return reply.c_str(); }
        if (c == "filter" || c == "filter status") { reply = filt::Status(); return reply.c_str(); }
        if (c == "filter fast 0" || c == "filter fast 1")
        {
            filt::g_fast = c.back() == '1'; rp::ConfigSet("filter", "fast", filt::g_fast ? "1" : "0");
            if (!filt::g_fast) filt::ShowOurGdtRows();
            reply = filt::g_fast ? "fast filter on" : "fast filter off (APE's own engine)"; return reply.c_str();
        }
        if (c == "filter value 0" || c == "filter value 1")
        {
            filt::g_fastValue = c.back() == '1'; rp::ConfigSet("filter", "fast_value", filt::g_fastValue ? "1" : "0");
            reply = filt::g_fastValue ? "value filter: the plugin's engine" : "value filter: APE's own engine"; return reply.c_str();
        }
        if (c == "filter name 0" || c == "filter name 1")   // test harness: the Name filter's check box
        {
            if (!filt::FindUi() || !filt::g_ui.nameCb) { reply = "no Name check box"; return reply.c_str(); }
            qt.buttonSetChecked(filt::g_ui.nameCb, c.back() == '1');
            reply = c.back() == '1' ? "Name filter checked" : "Name filter unchecked"; return reply.c_str();
        }
        if (c.rfind("filter debounce ", 0) == 0)
        {
            filt::g_debounceMs = atoi(c.substr(16).c_str()); if (filt::g_debounceMs < 0) filt::g_debounceMs = 0; if (filt::g_debounceMs > 2000) filt::g_debounceMs = 2000;
            rp::ConfigSet("filter", "debounce_ms", std::to_string(filt::g_debounceMs));
            snprintf(buf, sizeof buf, "debounce_ms=%d", filt::g_debounceMs); reply = buf; return reply.c_str();
        }
        if (c == "labels")
        {
            reply.clear();
            ForEachWidget([&](void* w) {
                if (!IsA(w, "QLabel") || !qt.isVisible(w)) return false;
                std::string t = TakeString(qt.labelText, w); if (t.empty() || t.size() > 60) return false;
                void* top = qt.window(w);
                snprintf(buf, sizeof buf, "%p '%s' objectName='%s' window=%s dock='%s'\n", w, t.c_str(), TakeString(qt.objectName, w).c_str(), top ? ClassName(top) : "?", DockOf(w).c_str()); reply += buf; return false; });
            if (reply.empty()) reply = "(no labels)";
            return reply.c_str();
        }
        if (c.rfind("select ", 0) == 0)
        {
            // test harness: make an asset the tree's current item (what a click does)
            void* tree = FindTree();
            if (!tree || !qt.treeSetCurrentItem) { reply = "no tree"; return reply.c_str(); }
            if (!filt::CacheValid(tree)) filt::BuildCache(tree);
            std::string want = c.substr(7);
            int skip = 0;   // "name#2" picks the second item with that name (an xmodel and a weapon can share one)
            if (size_t h = want.rfind('#'); h != std::string::npos) { skip = std::max(0, atoi(want.c_str() + h + 1) - 1); want.resize(h); }
            std::wstring w(want.begin(), want.end()); w = filt::Lower(w.c_str(), w.size());
            for (const filt::Node& n : filt::g_cache.nodes)
                if (n.lowered == w && skip-- == 0)
                {
                    uint64_t t0 = Now();
                    qt.treeSetCurrentItem(tree, n.item);
                    if (qt.treeScrollToItem) qt.treeScrollToItem(tree, n.item, 0);
                    snprintf(buf, sizeof buf, "selected '%s' (setCurrentItem returned after %.0f ms)", want.c_str(), Ms(Now() - t0));
                    reply = buf; return reply.c_str();
                }
            reply = "no asset named '" + want + "' in the tree"; return reply.c_str();
        }
        if (c == "filter audit") { void* tree = FindTree(); reply = tree ? filt::Audit(tree) : "no tree"; return reply.c_str(); }
        if (c == "commit" || c == "commit status") { reply = commit::Status(); return reply.c_str(); }
        if (c == "preview") { reply = preview::Status(); return reply.c_str(); }
        if (c.rfind("preview trace ", 0) == 0) { preview::g_trace = atoi(c.substr(14).c_str()); reply = "tracing"; return reply.c_str(); }
        if (c.rfind("preview fps ", 0) == 0)
        {
            preview::g_maxFps = atoi(c.substr(12).c_str()); if (preview::g_maxFps < 0) preview::g_maxFps = 0; if (preview::g_maxFps > 1000) preview::g_maxFps = 1000;
            rp::ConfigSet("preview", "max_fps", std::to_string(preview::g_maxFps));
            reply = preview::Status(); return reply.c_str();
        }
        if (c == "sky") { reply = sky::Status(); return reply.c_str(); }
        if (c == "sky list") { reply.clear(); for (auto& n : sky::ListSkies()) reply += n + "\n"; if (reply.empty()) reply = "no skyboxes found in gdt.db"; return reply.c_str(); }
        if (c == "sky refresh") { sky::g_ssiLoaded = false; sky::g_ssiOf.clear(); if (sky::IsOn()) sky::Activate(sky::g_pick); reply = sky::Status(); return reply.c_str(); }
        if (c == "sky pick") { sky::ShowPicker(); reply = "picker shown"; return reply.c_str(); }
        if (c == "sky menutest" || c == "sky menutest close")
        {
            // test only: open the preview's Rendering menu (the tool button whose menu holds Night) without the mouse, or close it
            reply.clear();
            if (c == "sky menutest close")
            {
                using PopupFn = void* (__fastcall*)(); auto popup = reinterpret_cast<PopupFn>(GetProcAddress(GetModuleHandleW(L"Qt5Widgets64r.dll"), "?activePopupWidget@QApplication@@SAPEAVQWidget@@XZ"));
                void* w = popup ? popup() : nullptr; if (w) rp::host->ui->SetVisible(w, 0);
                reply = w ? "closed" : "no popup"; return reply.c_str();
            }
            void* dock = rp::host->ui->FindEditorWidget("PreviewToolBar");
            void* b = nullptr;
            for (int i = 0; dock && i < 30 && !b; ++i)
            {
                void* t = rp::host->ui->FindChild(dock, "QToolButton", i);
                if (!t) break;
                if (TakeString(qt.toolTip, t) == "Rendering") b = t;
            }
            if (b && qt.timerSingleShot) { qt.timerSingleShot(0, b, "1showMenu()"); reply = "showMenu queued on the Rendering button"; }
            else reply = "no Rendering button";
            return reply.c_str();
        }
        if (c.rfind("sky slot ", 0) == 0) { reply = sky::InvokeSlot(sky::PreviewWidget(), c.substr(9).c_str()) ? sky::Status() : "no such slot"; return reply.c_str(); }   // test: LightingDay() ...
        if (c == "sky off") { sky::Deactivate(true); reply = sky::Status(); return reply.c_str(); }
        if (c.rfind("sky ", 0) == 0) { sky::Activate(c.substr(4)); reply = sky::Status(); return reply.c_str(); }
        if (c == "awi" || c.rfind("awi ", 0) == 0) { reply = awi::Command(c.size() > 4 ? c.substr(4) : ""); return reply.c_str(); }
        if (c.rfind("iotrace ", 0) == 0) { reply = iotrace::Start(atoi(c.substr(8).c_str())); return reply.c_str(); }
        if (c == "convert") { reply = convert::Status(); return reply.c_str(); }
        if (c == "convert lods" || c.rfind("convert lods click ", 0) == 0)
        {
            // test only: the Number of LODs buttons of the open xmodel, "click <n>" queues a click (XModel Info opens a modal box)
            void* grid = CurrentGrid();
            void* group = grid ? rp::host->ui->FindChild(grid, "apeButtonGroupWidget", 0) : nullptr;
            if (!group) { reply = "no Number of LODs group (open an xmodel)"; return reply.c_str(); }
            reply.clear();
            const int want = c.size() > 19 ? atoi(c.c_str() + 19) : -1;
            // A click queued while a modal box is open runs after the commands sent meanwhile swapped the grid's asset, and
            // APE's own handler then faults on the stale pointer.
            if (want >= 0)
            {
                using ModalFn = void* (__fastcall*)();
                auto activeModal = reinterpret_cast<ModalFn>(GetProcAddress(GetModuleHandleW(L"Qt5Widgets64r.dll"), "?activeModalWidget@QApplication@@SAPEAVQWidget@@XZ"));
                if (activeModal && activeModal()) { reply = "a modal window is open (close it first)"; return reply.c_str(); }
            }
            for (int i = 0; i < 32; ++i)
            {
                void* b = rp::host->ui->FindChild(group, "QAbstractButton", i);
                if (!b) break;
                if (i == want && qt.timerSingleShot) { qt.timerSingleShot(0, b, "1click()"); reply = "click queued on button " + std::to_string(i); return reply.c_str(); }
                reply += std::to_string(i) + ": '" + TakeString(qt.buttonText, b) + "' tip '" + TakeString(qt.toolTip, b) + "'\n";
            }
            if (reply.empty()) reply = "no buttons";
            return reply.c_str();
        }
        if (c == "convert 0" || c == "convert 1") { convert::g_on = c[8] == '1'; convert::g_models = 0; convert::g_totalUs = 0; convert::g_maxUs = 0; reply = convert::Status(); return reply.c_str(); }
        if (c == "formats" || c == "formats status") { reply = formats::Status(); return reply.c_str(); }
        if (c == "open" || c == "open status") { reply = fastopen::Status(); return reply.c_str(); }
        if (c == "open on" || c == "open off") { fastopen::g_on = c == "open on"; reply = fastopen::Status(); return reply.c_str(); }
        if (c == "open count") { reply = fastopen::Count(); return reply.c_str(); }
        if (c == "open thumbs on" || c == "open thumbs off") { fastopen::g_thumbOn = c == "open thumbs on"; if (!fastopen::g_thumbOn) fastopen::DropAll(); reply = fastopen::Status(); return reply.c_str(); }
        if (c == "open swap on" || c == "open swap off") { fastopen::g_swapOn = c == "open swap on"; reply = fastopen::Status(); return reply.c_str(); }
        if (c == "dxvk" || c == "dxvk status") { reply = dxvk::Status(); return reply.c_str(); }
        if (c == "dxvk 0" || c == "dxvk 1") { reply = dxvk::Set(c.back() == '1'); return reply.c_str(); }
        if (c == "dxvk prefs 0" || c == "dxvk prefs 1") { reply = theme::PrefsOkTest(c.back() == '1'); return reply.c_str(); }
        if (c == "dxvk restart") { dxvk::Restart(); reply = "restarting APE"; return reply.c_str(); }
        if (c == "theme" || c == "theme status") { reply = theme::Status(); return reply.c_str(); }
        if (c == "theme list") { reply = theme::List(); return reply.c_str(); }
        if (c == "theme rescan") { theme::Scan(); reply = theme::List(); return reply.c_str(); }
        if (c == "theme selftest" || c.rfind("theme selftest ", 0) == 0) { reply = theme::SelfTest(c.size() > 15 ? c.substr(15) : ""); return reply.c_str(); }
        if (c.rfind("theme pick ", 0) == 0) { reply = theme::PickTest(c.substr(11)); return reply.c_str(); }
        if (c == "theme folder") { theme::OnOpenFolder(nullptr); reply = "opened"; return reply.c_str(); }
        if (c == "theme customize") { theme::OnCustomize(nullptr); reply = theme::g_editor ? (theme::g_editorShown ? "editor shown" : "editor hidden") : "no editor: " + theme::g_editorError; return reply.c_str(); }
        if (c.rfind("theme editor", 0) == 0) { reply = themeedit::Command(c.size() > 13 ? c.substr(13) : ""); return reply.c_str(); }
        if (c == "theme icons" || c.rfind("theme icons ", 0) == 0) { reply = themeicons::Command(c.size() > 12 ? c.substr(12) : "", theme::Dir()); return reply.c_str(); }
        if (c.rfind("theme ", 0) == 0)
        {
            std::string name = c.substr(6), why;
            if (theme::g_list.empty()) theme::Scan();
            if (!theme::Apply(name, why)) { reply = "failed: " + why; return reply.c_str(); }
            theme::g_current = theme::g_applied; rp::ConfigSet("theme", "name", theme::g_current);
            snprintf(buf, sizeof buf, "theme '%s' applied in %.0f ms", theme::g_applied.c_str(), theme::g_lastMs);
            reply = buf; return reply.c_str();
        }
        if (c.rfind("commit min ", 0) == 0)
        {
            commit::g_minS = atoi(c.substr(11).c_str()); if (commit::g_minS < 0) commit::g_minS = 0; if (commit::g_minS > 3600) commit::g_minS = 3600;
            rp::ConfigSet("commit", "scan_min_s", std::to_string(commit::g_minS));
            reply = commit::Status(); return reply.c_str();
        }
        if (c == "files 1") { commit::InstallTrace(); { std::lock_guard<std::mutex> lock(commit::g_traceMutex); commit::g_traceDirs.clear(); } commit::g_traceCalls = 0; commit::g_trace = commit::g_origFind != nullptr; reply = commit::g_trace ? "file trace on" : "trace hook failed"; return reply.c_str(); }
        if (c == "files 0") { commit::g_trace = false; reply = "file trace off"; return reply.c_str(); }
        if (c == "files") { reply = commit::TraceReport(); return reply.c_str(); }
        if (c == "filter label") { reply = filt::g_ui.label ? "label: '" + TakeString(qt.labelText, filt::g_ui.label) + "'" : "no label"; return reply.c_str(); }
        if (c == "filter run")
        {
            void* tree = FindTree();
            if (!tree || !filt::g_orig) { reply = "no tree or not hooked"; return reply.c_str(); }
            uint64_t t0 = Now();
            filt::hk_UpdateFilters(tree);   // from here the return address is ours: runs synchronously
            snprintf(buf, sizeof buf, "run took %.0f ms (%s engine); ", Ms(Now() - t0), filt::g_fast ? "fast" : "APE's");
            reply = buf + filt::Status(); return reply.c_str();
        }
        if (c == "fn")
        {
            void* live = rp::Ape() ? rp::Ape()->ApeAddress(rp::self, 0x1400B46C0) : nullptr;
            DWORD64 base = 0; PRUNTIME_FUNCTION fn = live ? RtlLookupFunctionEntry(DWORD64(live), &base, nullptr) : nullptr;
            snprintf(buf, sizeof buf, "assetTreeUpdateFilters live=%p entry begin=0x%x end=0x%x (expect begin 0xb46c0)", live, fn ? fn->BeginAddress : 0, fn ? fn->EndAddress : 0);
            reply = buf; return reply.c_str();
        }
        if (c.rfind("set ", 0) == 0)
        {
            // set an edit's text the way typing ends up (setText + the textEdited signal), for filter tests with any characters
            size_t sp = c.find(' ', 4);
            int idx = atoi(c.substr(4, sp == std::string::npos ? std::string::npos : sp - 4).c_str());
            std::string text = sp == std::string::npos ? "" : c.substr(sp + 1);
            std::vector<EditRef>& e = g_lastEdits;
            if (idx < 0 || size_t(idx) >= e.size()) { reply = "no such edit (run 'edits' or 'edits all' first)"; return reply.c_str(); }
            std::wstring w(text.size() + 1, L'\0'); int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), &w[0], int(w.size())); w.resize(n > 0 ? n : 0);
            void* qs = nullptr; qt.qstringCtor(&qs, w.c_str(), int(w.size()));
            qt.lineEditSetText(e[idx].edit, &qs);
            qt.lineEditTextEdited(e[idx].edit, &qs);
            qt.qsDtor(&qs);
            reply = "set edit " + std::to_string(idx) + " to '" + text + "' (textEdited emitted)"; return reply.c_str();
        }
        if (c == "typed") { reply = g_typeResult.empty() ? (g_typing.active ? "still typing" : "no result") : g_typeResult; return reply.c_str(); }
        if (c.rfind("profile", 0) == 0)
        {
            int ms = c.size() > 8 ? atoi(c.substr(8).c_str()) : 3000;
            bool all = c.find(" all") != std::string::npos;
            if (ms < 100) ms = 100; if (ms > 60000) ms = 60000;
            std::string why;
            if (!StartSampler(ms, all, why)) { reply = "failed: " + why; return reply.c_str(); }
            snprintf(buf, sizeof buf, "sampling the main thread for %d ms; the report goes to the log", ms);
            reply = buf; return reply.c_str();
        }
        if (c == "report") { reply = g_sampler.done ? "(already logged)" : (g_sampler.running ? "still sampling" : "no profile"); return reply.c_str(); }
        if (c == "stall")
        {
            snprintf(buf, sizeof buf, "longest gap between idle ticks since last ask: %.0f ms (%.1f s ago)", Ms(g_tickMaxGap), g_tickMaxAt ? Ms(Now() - g_tickMaxAt) / 1000.0 : 0.0);
            g_tickMaxGap = 0; g_tickMaxAt = 0;
            reply = buf; return reply.c_str();
        }
        return nullptr;
    }
}

extern "C" MODTOOLS_PLUGIN_EXPORT const ModToolsPluginApi* ModToolsPluginEntry(ModToolsPlugin* self, const ModToolsHostApi* host)
{
    rp::Bind(self, host);
    static ModToolsPluginApi api = {};
    api.size = sizeof api;
    api.apiVersion = MODTOOLS_PLUGIN_API_VERSION;
    api.name = "APEPlus";
    api.displayName = "APE+";
    api.version = "1.1.1";
    api.author = "Figglebottom";
    api.description = "QOL fixes and improvements to APE.";
    api.url = "https://github.com/Aidan601/MTPlugins/tree/main/plugins/APEPlus";
    api.OnLoad = OnLoad;
    api.OnUnload = OnUnload;
    api.OnTick = OnTick;
    api.OnCommand = OnCommand;
    api.apps = MODTOOLS_APP_APE;
    return &api;
}
