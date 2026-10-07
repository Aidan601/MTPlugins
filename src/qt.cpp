#include "qt.h"
#include <cstring>
#include "log.h"

namespace
{
    struct QPoint { int x, y; };
    template <typename T> bool Res(T& out, const wchar_t* dll, const char* name, bool& ok)
    {
        HMODULE m = GetModuleHandleW(dll);
        out = m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
        if (!out) { LOG("qt: missing %S!%s", dll, name); ok = false; }
        return out != nullptr;
    }
    const wchar_t* kCore = L"Qt5Core64r.dll";
    const wchar_t* kWidgets = L"Qt5Widgets64r.dll";
    const wchar_t* kGui = L"Qt5Gui64r.dll";

    // --- resolved exports (MS x64 ABI: this in rcx, by-value QString/QPoint returns through a hidden pointer after it) ---
    void* (*p_new)(size_t);
    void* (*p_QString_ctor)(void*, const char*);
    void  (*p_QString_dtor)(void*);
    void* (*p_QObject_ctor)(void*, void*);
    void  (*p_QObject_setObjectName)(void*, const void*);
    void  (*p_QObject_installEventFilter)(void*, void*);
    void  (*p_QObject_removeEventFilter)(void*, void*);
    void  (*p_QObject_deleteLater)(void*);
    void* (*p_QObject_connectImpl)(void* retConnection, const void* sender, void** signal, const void* receiver, void** slot, void* slotObj, int type, const int* types, const void* senderMetaObject);
    void  (*p_Connection_dtor)(void*);
    void* (*p_QMetaObject_cast)(const void*, void*);
    void* (*p_QWidget_ctor)(void*, void*, int);
    void* (*p_QLabel_ctor)(void*, const void*, void*, int);
    void  (*p_QLabel_setText)(void*, const void*);
    void  (*p_QLabel_setWordWrap)(void*, bool);
    void  (*p_QLabel_setOpenExternalLinks)(void*, bool);   // optional
    void  (*p_QLayout_setSizeConstraint)(void*, int);       // optional
    void* (*p_QCoreApplication_instance)();                 // optional
    void* (*p_QApplication_styleSheet)(const void*, void*); // optional
    void* (*p_QLineEdit_ctor)(void*, const void*, void*);
    void* (*p_QLineEdit_text)(void*, void*);
    void  (*p_QLineEdit_setText)(void*, const void*);
    void  (*p_QLineEdit_setPlaceholderText)(void*, const void*);
    void* (*p_QPushButton_ctor)(void*, const void*, void*);
    void* (*p_QCheckBox_ctor)(void*, const void*, void*);
    void* (*p_QRadioButton_ctor)(void*, const void*, void*);
    void  (*p_QAbstractButton_setText)(void*, const void*);
    void  (*p_QAbstractButton_setChecked)(void*, bool);
    bool  (*p_QAbstractButton_isChecked)(void*);
    bool  (*p_QAbstractButton_isDown)(void*);
    void* (*p_QListWidget_ctor)(void*, void*);
    void  (*p_QListWidget_addItem)(void*, const void*);
    void  (*p_QListWidget_clear)(void*);
    int   (*p_QListWidget_count)(void*);
    int   (*p_QListWidget_currentRow)(void*);
    void  (*p_QListWidget_setCurrentRow)(void*, int);
    void* (*p_QListWidget_item)(void*, int);
    void  (*p_QListWidgetItem_setText)(void*, const void*);
    void  (*p_QListWidgetItem_setToolTip)(void*, const void*);
    void* (*p_QListWidgetItem_flags)(void*, int* out);   // QFlags: returned through a hidden pointer (RDX)
    void  (*p_QListWidgetItem_setFlags)(void*, int);
    int   (*p_QListWidgetItem_checkState)(void*);
    void  (*p_QListWidgetItem_setCheckState)(void*, int);
    void* (*p_QPlainTextEdit_ctor)(void*, void*);
    void  (*p_QPlainTextEdit_setPlainText)(void*, const void*);
    void  (*p_QPlainTextEdit_setReadOnly)(void*, bool);
    void  (*p_QPlainTextEdit_moveCursor)(void*, int, int);
    void* (*p_QVBoxLayout_ctorP)(void*, void*);
    void* (*p_QVBoxLayout_ctor)(void*);
    void* (*p_QHBoxLayout_ctor)(void*);
    void* (*p_QGridLayout_ctor)(void*);
    void  (*p_QBoxLayout_addWidget)(void*, void*, int, int);
    void  (*p_QBoxLayout_addLayout)(void*, void*, int);
    void  (*p_QBoxLayout_addStretch)(void*, int);
    void  (*p_QGridLayout_addWidget)(void*, void*, int, int, int);
    void  (*p_QGridLayout_addLayout)(void*, void*, int, int, int);
    void  (*p_QGridLayout_setColumnStretch)(void*, int, int);
    void  (*p_QLayout_setContentsMargins)(void*, int, int, int, int);
    void  (*p_QLayout_setSpacing)(void*, int);
    void  (*p_QWidget_setLayout)(void*, void*);
    void  (*p_QWidget_setEnabled)(void*, bool);
    void  (*p_QWidget_setVisible)(void*, bool);
    void  (*p_QWidget_setParent)(void*, void*);
    void  (*p_QWidget_setMinimumHeight)(void*, int);
    void  (*p_QWidget_setMaximumHeight)(void*, int);
    void  (*p_QWidget_setMaximumWidth)(void*, int);
    void  (*p_QWidget_setToolTip)(void*, const void*);
    void  (*p_QWidget_setMinimumSize)(void*, int, int);
    void  (*p_QWidget_resize)(void*, int, int);
    void  (*p_QWidget_raise)(void*);
    void  (*p_QWidget_setAttribute)(void*, int, bool);
    uintptr_t (*p_QWidget_winId)(void*);
    void  (*p_QWidget_setFocus)(void*, int);
    void* (*p_QWidget_mapFromGlobal)(void*, QPoint*, const QPoint*);
    void  (*p_QWidget_addAction)(void*, void*);
    void* (*p_QToolBar_addWidget)(void*, void*);
    void  (*p_QWidget_removeAction)(void*, void*);
    void* (*p_QMainWindow_menuBar)(void*);
    void* (*p_QWidget_find)(uintptr_t);
    const void* p_QMainWindow_meta;
    void* (*p_QDockWidget_ctor)(void*, const void*, void*, int);
    void  (*p_QDockWidget_setWidget)(void*, void*);
    void* (*p_QDockWidget_toggleViewAction)(void*);
    void  (*p_QMainWindow_addDockWidget)(void*, int, void*);
    bool  (*p_QMainWindow_restoreDockWidget)(void*, void*);
    void* (*p_QObject_objectName)(void*, void*);
    const char* (*p_QMetaObject_className)(const void*);
    void* (*p_QObject_children)(void*);
    void* (*p_qFindChild_helper)(const void*, const void*, const void*, int);
    const void* p_QObject_meta;
    int   (*p_QTabWidget_count)(void*);
    int   (*p_QTabWidget_currentIndex)(void*);
    void* (*p_QTabWidget_tabText)(void*, void*, int);
    void* (*p_QTabWidget_currentWidget)(void*);
    void* (*p_QTreeWidget_selectedItems)(void*, void*);
    void* (*p_QTreeWidget_currentItem)(void*);
    void* (*p_QTreeWidgetItem_text)(void*, void*, int);
    void* (*p_QTreeWidgetItem_toolTip)(void*, void*, int);
    void* (*p_QTreeWidgetItem_parent)(void*);
    void  (*p_QTextEdit_append)(void*, const void*);
    void  (*p_QPlainTextEdit_appendPlainText)(void*, const void*);
    void* (*p_QTextEdit_toPlainText)(void*, void*);
    void* (*p_QPlainTextEdit_toPlainText)(void*, void*);
    void  (*p_QListData_dispose)(void*);
    const void* (*p_QMetaObject_superClass)(const void*);
    int   (*p_QTabWidget_indexOf)(void*, void*);
    void* (*p_QTreeWidgetItem_data)(void*, void*, int, int);
    const char* (*p_QVariant_typeName)(void*);
    void* (*p_QVariant_toString)(void*, void*);
    void  (*p_QVariant_dtor)(void*);
    void* (*p_QDialog_ctor)(void*, void*, int);
    void  (*p_QWidget_setWindowTitle)(void*, const void*);
    void  (*p_QWidget_move)(void*, int, int);
    void  (*p_QWidget_activateWindow)(void*);
    void* (*p_QMenuBar_addMenu)(void*, const void*);
    void* (*p_QWidget_actions)(void*, void*);          // QList<QAction*> through the hidden pointer
    void* (*p_QComboBox_currentText)(void*, void*);
    int   (*p_QComboBox_currentIndex)(void*);
    void  (*p_QComboBox_setCurrentIndex)(void*, int);
    int   (*p_QComboBox_findText)(void*, const void*, int);
    int   (*p_QComboBox_count)(void*);
    void* (*p_QComboBox_itemText)(void*, void*, int);
    double (*p_QDoubleSpinBox_value)(void*);
    void  (*p_QDoubleSpinBox_setValue)(void*, double);
    int   (*p_QSpinBox_value)(void*);
    void  (*p_QSpinBox_setValue)(void*, int);
    void* (*p_QLabel_text)(void*, void*);
    void* (*p_QAbstractButton_text)(void*, void*);
    void  (*p_QAbstractButton_click)(void*);
    void* (*p_QWidget_toolTip)(void*, void*);
    bool  (*p_QWidget_isVisible)(void*);
    bool  (*p_QWidget_isEnabled)(void*);
    void  (*p_QLineEdit_editingFinished)(void*);
    void  (*p_QLineEdit_returnPressed)(void*);
    void* (*p_QObject_property)(void*, void*, const char*);
    void* (*p_QObject_dynamicPropertyNames)(void*, void*);
    void* (*p_QAction_text)(void*, void*);
    void* (*p_QAction_menu)(void*);
    bool  (*p_QAction_isEnabled)(void*);
    bool  (*p_QAction_isSeparator)(void*);
    void  (*p_QAction_trigger)(void*);
    void* (*p_QMenu_menuAction)(void*);
    void  (*p_QMenu_clear)(void*);
    void* (*p_QMenu_addSeparator)(void*);
    void* (*p_QAction_ctor)(void*, const void*, void*);
    void  (*p_QAction_setEnabled)(void*, bool);
    void  (*p_QAction_setCheckable)(void*, bool);
    void  (*p_QAction_setChecked)(void*, bool);
    void  (*p_QAction_setText)(void*, const void*);
    void* p_QAction_triggered; const void* p_QAction_meta;
    void* p_QAbstractButton_clicked; const void* p_QAbstractButton_meta;
    void* p_QListWidget_currentRowChanged; void* p_QListWidget_itemChanged; const void* p_QListWidget_meta;

    bool g_bound = false, g_ok = false;

    // QColorDialog::getColor (resolved on first use). QColor is 16 bytes, spec 0 = invalid. Static: hidden return pointer in rcx.
    void* (*p_QColorDialog_getColor)(void* ret, const void* initial, void* parent, const void* title, int options);
    void  (*p_QColor_setRgba)(void*, unsigned);
    unsigned (*p_QColor_rgba)(const void*);
    bool  (*p_QColor_isValid)(const void*);
    void  (*p_QLineEdit_setEchoMode)(void*, int);   // enum EchoMode as int, resolved on first use

    // one vtable copy shared by every filter object, fn stored behind the QObject
    void* g_filterVt[16] = {};
    struct FilterObj { char qobject[32]; qt::FilterFn fn; void* user; };
    bool __fastcall FilterThunk(void* self, void* watched, void* ev)
    {
        auto* fo = static_cast<FilterObj*>(self);
        int type = *reinterpret_cast<unsigned short*>(static_cast<char*>(ev) + 16);   // QEvent::t
        return fo->fn && fo->fn(fo->user, watched, type);
    }

    // QtPrivate::QSlotObjectBase: { QAtomicInt ref, ImplFn impl } and our payload behind it.
    // impl(which, this, receiver, args, ret): 0 Destroy, 1 Call, 2 Compare.
    struct SlotObj { int ref; int pad; void (*impl)(int, SlotObj*, void*, void**, bool*); qt::SlotFn fn; void* user; bool dead; };
    void SlotImpl(int which, SlotObj* self, void* /*receiver*/, void** args, bool* ret)
    {
        if (which == 1) { if (self->fn && !self->dead) self->fn(self->user, args); }
        else if (which == 2) { if (ret) *ret = false; }
        else if (which == 0) { self->dead = true; }     // Qt dropped its reference, the memory is kept
    }
}

namespace
{
    // panel widgets, resolved by qt::PanelBound
    void* (*p_QSlider_ctor)(void*, int, void*);
    void  (*p_QAbstractSlider_setRange)(void*, int, int);
    void  (*p_QAbstractSlider_setValue)(void*, int);
    int   (*p_QAbstractSlider_value)(void*);
    void* p_QAbstractSlider_valueChanged; const void* p_QAbstractSlider_meta;
    void* (*p_QComboBox_ctor)(void*, void*);
    void  (*p_QComboBox_insertItem)(void*, int, const void*, const void*);
    void  (*p_QComboBox_clear)(void*);
    void* p_QComboBox_currentIndexChanged; const void* p_QComboBox_meta;
    void* (*p_QVariant_ctorInt)(void*, int);
    void* (*p_QVariant_ctorLongLong)(void*, long long);
    long long (*p_QVariant_toLongLong)(const void*, bool*);
    void* (*p_QTabWidget_ctor)(void*, void*);
    int   (*p_QTabWidget_addTab)(void*, void*, const void*);
    void  (*p_QTabWidget_setCurrentIndex)(void*, int);
    void* p_QTabWidget_currentChanged; const void* p_QTabWidget_meta;
    void* (*p_QScrollArea_ctor)(void*, void*);
    void  (*p_QScrollArea_setWidget)(void*, void*);
    void  (*p_QScrollArea_setWidgetResizable)(void*, bool);
    void  (*p_QFrame_setFrameShape)(void*, int);
    void  (*p_QAbstractScrollArea_setHorizontalScrollBarPolicy)(void*, int);
    void* (*p_QSplitter_ctor)(void*, int, void*);
    void  (*p_QSplitter_addWidget)(void*, void*);
    void  (*p_QSplitter_setStretchFactor)(void*, int, int);
    void  (*p_QSplitter_setChildrenCollapsible)(void*, bool);
    int   (*p_QSplitter_indexOf)(void*, void*);
    void* (*p_QTreeWidget_ctor)(void*, void*);
    void  (*p_QTreeWidget_setColumnCount)(void*, int);
    void  (*p_QTreeView_setHeaderHidden)(void*, bool);
    void  (*p_QTreeWidget_clear)(void*);
    void  (*p_QTreeWidget_addTopLevelItem)(void*, void*);
    int   (*p_QTreeWidget_topLevelItemCount)(void*);
    void* (*p_QTreeWidget_topLevelItem)(void*, int);
    int   (*p_QTreeWidget_indexOfTopLevelItem)(void*, void*);
    void* (*p_QTreeWidget_takeTopLevelItem)(void*, int);
    void  (*p_QTreeWidget_setCurrentItem)(void*, void*);
    void* p_QTreeWidget_currentItemChanged; void* p_QTreeWidget_itemChanged; const void* p_QTreeWidget_meta;
    void* (*p_QTreeWidgetItem_ctor)(void*, int);
    void  (*p_QTreeWidgetItem_addChild)(void*, void*);
    int   (*p_QTreeWidgetItem_childCount)(void*);
    void* (*p_QTreeWidgetItem_child)(void*, int);
    int   (*p_QTreeWidgetItem_indexOfChild)(void*, void*);
    void* (*p_QTreeWidgetItem_takeChild)(void*, int);
    void  (*p_QTreeWidgetItem_setText)(void*, int, const void*);
    void  (*p_QTreeWidgetItem_setData)(void*, int, int, const void*);
    void* (*p_QTreeWidgetItem_flags)(void*, int* out);   // QFlags, hidden pointer
    void  (*p_QTreeWidgetItem_setFlags)(void*, int);
    int   (*p_QTreeWidgetItem_checkState)(void*, int);
    void  (*p_QTreeWidgetItem_setCheckState)(void*, int, int);
    void  (*p_QTreeWidgetItem_setExpanded)(void*, bool);
    void  (*p_QAbstractItemView_setDragDropMode)(void*, int);
    void  (*p_QAbstractItemView_setDefaultDropAction)(void*, int);
    void* (*p_QAbstractItemView_model)(void*);
    void* p_QAbstractItemModel_rowsInserted; void* p_QAbstractItemModel_rowsRemoved; const void* p_QAbstractItemModel_meta;
    const void* p_QLineEdit_meta;   // editingFinished is bound in Bind (EmitEditingFinished)
    void* p_QAbstractButton_toggled;
}

namespace qt
{
    QStr::QStr(const char* utf8) { p_QString_ctor(this, utf8 ? utf8 : ""); }
    QStr::QStr(const std::string& utf8) { p_QString_ctor(this, utf8.c_str()); }
    QStr::~QStr() { if (d) p_QString_dtor(this); }
    std::string QStr::Utf8() const
    {
        if (!d) return {};
        // QArrayData: int ref, int size, uint alloc bits, qptrdiff offset (data at d + offset)
        int n = *reinterpret_cast<int*>(static_cast<char*>(d) + 4);
        ptrdiff_t off = *reinterpret_cast<ptrdiff_t*>(static_cast<char*>(d) + 16);
        auto* w = reinterpret_cast<const wchar_t*>(static_cast<char*>(d) + off);
        if (n <= 0) return {};
        int bytes = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
        std::string s(bytes, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, n, &s[0], bytes, nullptr, nullptr);
        return s;
    }

    bool Bind()
    {
        if (g_bound) return g_ok;
        g_bound = true;
        bool ok = true;
        Res(p_new, L"msvcr110.dll", "??2@YAPEAX_K@Z", ok);
        Res(p_QString_ctor, kCore, "??0QString@@QEAA@PEBD@Z", ok);
        Res(p_QString_dtor, kCore, "??1QString@@QEAA@XZ", ok);
        Res(p_QObject_ctor, kCore, "??0QObject@@QEAA@PEAV0@@Z", ok);
        Res(p_QObject_setObjectName, kCore, "?setObjectName@QObject@@QEAAXAEBVQString@@@Z", ok);
        Res(p_QObject_installEventFilter, kCore, "?installEventFilter@QObject@@QEAAXPEAV1@@Z", ok);
        Res(p_QObject_removeEventFilter, kCore, "?removeEventFilter@QObject@@QEAAXPEAV1@@Z", ok);
        Res(p_QObject_deleteLater, kCore, "?deleteLater@QObject@@QEAAXXZ", ok);
        Res(p_QObject_connectImpl, kCore, "?connectImpl@QObject@@CA?AVConnection@QMetaObject@@PEBV1@PEAPEAX01PEAVQSlotObjectBase@QtPrivate@@W4ConnectionType@Qt@@PEBHPEBU3@@Z", ok);
        Res(p_Connection_dtor, kCore, "??1Connection@QMetaObject@@QEAA@XZ", ok);
        Res(p_QMetaObject_cast, kCore, "?cast@QMetaObject@@QEBAPEAVQObject@@PEAV2@@Z", ok);
        Res(p_QWidget_ctor, kWidgets, "??0QWidget@@QEAA@PEAV0@V?$QFlags@W4WindowType@Qt@@@@@Z", ok);
        Res(p_QLabel_ctor, kWidgets, "??0QLabel@@QEAA@AEBVQString@@PEAVQWidget@@V?$QFlags@W4WindowType@Qt@@@@@Z", ok);
        Res(p_QLabel_setText, kWidgets, "?setText@QLabel@@QEAAXAEBVQString@@@Z", ok);
        Res(p_QLabel_setWordWrap, kWidgets, "?setWordWrap@QLabel@@QEAAX_N@Z", ok);
        { bool opt = true; Res(p_QLabel_setOpenExternalLinks, kWidgets, "?setOpenExternalLinks@QLabel@@QEAAX_N@Z", opt); }
        { bool opt = true; Res(p_QLayout_setSizeConstraint, kWidgets, "?setSizeConstraint@QLayout@@QEAAXW4SizeConstraint@1@@Z", opt); }
        { bool opt = true; Res(p_QCoreApplication_instance, kCore, "?instance@QCoreApplication@@SAPEAV1@XZ", opt); }
        { bool opt = true; Res(p_QApplication_styleSheet, kWidgets, "?styleSheet@QApplication@@QEBA?AVQString@@XZ", opt); }
        Res(p_QLineEdit_ctor, kWidgets, "??0QLineEdit@@QEAA@AEBVQString@@PEAVQWidget@@@Z", ok);
        Res(p_QLineEdit_text, kWidgets, "?text@QLineEdit@@QEBA?AVQString@@XZ", ok);
        Res(p_QLineEdit_setText, kWidgets, "?setText@QLineEdit@@QEAAXAEBVQString@@@Z", ok);
        Res(p_QLineEdit_setPlaceholderText, kWidgets, "?setPlaceholderText@QLineEdit@@QEAAXAEBVQString@@@Z", ok);
        Res(p_QPushButton_ctor, kWidgets, "??0QPushButton@@QEAA@AEBVQString@@PEAVQWidget@@@Z", ok);
        Res(p_QCheckBox_ctor, kWidgets, "??0QCheckBox@@QEAA@AEBVQString@@PEAVQWidget@@@Z", ok);
        Res(p_QRadioButton_ctor, kWidgets, "??0QRadioButton@@QEAA@AEBVQString@@PEAVQWidget@@@Z", ok);
        Res(p_QAbstractButton_setText, kWidgets, "?setText@QAbstractButton@@QEAAXAEBVQString@@@Z", ok);
        Res(p_QAbstractButton_setChecked, kWidgets, "?setChecked@QAbstractButton@@QEAAX_N@Z", ok);
        Res(p_QAbstractButton_isChecked, kWidgets, "?isChecked@QAbstractButton@@QEBA_NXZ", ok);
        Res(p_QAbstractButton_isDown, kWidgets, "?isDown@QAbstractButton@@QEBA_NXZ", ok);
        Res(p_QListWidget_ctor, kWidgets, "??0QListWidget@@QEAA@PEAVQWidget@@@Z", ok);
        Res(p_QListWidget_addItem, kWidgets, "?addItem@QListWidget@@QEAAXAEBVQString@@@Z", ok);
        Res(p_QListWidget_clear, kWidgets, "?clear@QListWidget@@QEAAXXZ", ok);
        Res(p_QListWidget_count, kWidgets, "?count@QListWidget@@QEBAHXZ", ok);
        Res(p_QListWidget_currentRow, kWidgets, "?currentRow@QListWidget@@QEBAHXZ", ok);
        Res(p_QListWidget_setCurrentRow, kWidgets, "?setCurrentRow@QListWidget@@QEAAXH@Z", ok);
        Res(p_QListWidget_item, kWidgets, "?item@QListWidget@@QEBAPEAVQListWidgetItem@@H@Z", ok);
        Res(p_QListWidgetItem_setText, kWidgets, "?setText@QListWidgetItem@@QEAAXAEBVQString@@@Z", ok);
        Res(p_QListWidgetItem_setToolTip, kWidgets, "?setToolTip@QListWidgetItem@@QEAAXAEBVQString@@@Z", ok);
        Res(p_QListWidgetItem_flags, kWidgets, "?flags@QListWidgetItem@@QEBA?AV?$QFlags@W4ItemFlag@Qt@@@@XZ", ok);
        Res(p_QListWidgetItem_setFlags, kWidgets, "?setFlags@QListWidgetItem@@QEAAXV?$QFlags@W4ItemFlag@Qt@@@@@Z", ok);
        Res(p_QListWidgetItem_checkState, kWidgets, "?checkState@QListWidgetItem@@QEBA?AW4CheckState@Qt@@XZ", ok);
        Res(p_QListWidgetItem_setCheckState, kWidgets, "?setCheckState@QListWidgetItem@@QEAAXW4CheckState@Qt@@@Z", ok);
        Res(p_QPlainTextEdit_ctor, kWidgets, "??0QPlainTextEdit@@QEAA@PEAVQWidget@@@Z", ok);
        Res(p_QPlainTextEdit_setPlainText, kWidgets, "?setPlainText@QPlainTextEdit@@QEAAXAEBVQString@@@Z", ok);
        Res(p_QPlainTextEdit_setReadOnly, kWidgets, "?setReadOnly@QPlainTextEdit@@QEAAX_N@Z", ok);
        Res(p_QPlainTextEdit_moveCursor, kWidgets, "?moveCursor@QPlainTextEdit@@QEAAXW4MoveOperation@QTextCursor@@W4MoveMode@3@@Z", ok);
        Res(p_QVBoxLayout_ctorP, kWidgets, "??0QVBoxLayout@@QEAA@PEAVQWidget@@@Z", ok);
        Res(p_QVBoxLayout_ctor, kWidgets, "??0QVBoxLayout@@QEAA@XZ", ok);
        Res(p_QHBoxLayout_ctor, kWidgets, "??0QHBoxLayout@@QEAA@XZ", ok);
        Res(p_QGridLayout_ctor, kWidgets, "??0QGridLayout@@QEAA@XZ", ok);
        Res(p_QBoxLayout_addWidget, kWidgets, "?addWidget@QBoxLayout@@QEAAXPEAVQWidget@@HV?$QFlags@W4AlignmentFlag@Qt@@@@@Z", ok);
        Res(p_QBoxLayout_addLayout, kWidgets, "?addLayout@QBoxLayout@@QEAAXPEAVQLayout@@H@Z", ok);
        Res(p_QBoxLayout_addStretch, kWidgets, "?addStretch@QBoxLayout@@QEAAXH@Z", ok);
        Res(p_QGridLayout_addWidget, kWidgets, "?addWidget@QGridLayout@@QEAAXPEAVQWidget@@HHV?$QFlags@W4AlignmentFlag@Qt@@@@@Z", ok);
        Res(p_QGridLayout_addLayout, kWidgets, "?addLayout@QGridLayout@@QEAAXPEAVQLayout@@HHV?$QFlags@W4AlignmentFlag@Qt@@@@@Z", ok);
        Res(p_QGridLayout_setColumnStretch, kWidgets, "?setColumnStretch@QGridLayout@@QEAAXHH@Z", ok);
        Res(p_QLayout_setContentsMargins, kWidgets, "?setContentsMargins@QLayout@@QEAAXHHHH@Z", ok);
        Res(p_QLayout_setSpacing, kWidgets, "?setSpacing@QLayout@@QEAAXH@Z", ok);
        Res(p_QWidget_setLayout, kWidgets, "?setLayout@QWidget@@QEAAXPEAVQLayout@@@Z", ok);
        Res(p_QWidget_setEnabled, kWidgets, "?setEnabled@QWidget@@QEAAX_N@Z", ok);
        Res(p_QWidget_setVisible, kWidgets, "?setVisible@QWidget@@UEAAX_N@Z", ok);
        Res(p_QWidget_setParent, kWidgets, "?setParent@QWidget@@QEAAXPEAV1@@Z", ok);
        Res(p_QWidget_setMinimumHeight, kWidgets, "?setMinimumHeight@QWidget@@QEAAXH@Z", ok);
        Res(p_QWidget_setMaximumHeight, kWidgets, "?setMaximumHeight@QWidget@@QEAAXH@Z", ok);
        Res(p_QWidget_setMaximumWidth, kWidgets, "?setMaximumWidth@QWidget@@QEAAXH@Z", ok);
        Res(p_QWidget_setToolTip, kWidgets, "?setToolTip@QWidget@@QEAAXAEBVQString@@@Z", ok);
        Res(p_QWidget_setMinimumSize, kWidgets, "?setMinimumSize@QWidget@@QEAAXHH@Z", ok);
        Res(p_QWidget_resize, kWidgets, "?resize@QWidget@@QEAAXHH@Z", ok);
        Res(p_QWidget_raise, kWidgets, "?raise@QWidget@@QEAAXXZ", ok);
        Res(p_QWidget_setAttribute, kWidgets, "?setAttribute@QWidget@@QEAAXW4WidgetAttribute@Qt@@_N@Z", ok);
        Res(p_QWidget_winId, kWidgets, "?winId@QWidget@@QEBA_KXZ", ok);
        Res(p_QWidget_setFocus, kWidgets, "?setFocus@QWidget@@QEAAXW4FocusReason@Qt@@@Z", ok);
        Res(p_QWidget_mapFromGlobal, kWidgets, "?mapFromGlobal@QWidget@@QEBA?AVQPoint@@AEBV2@@Z", ok);
        Res(p_QWidget_addAction, kWidgets, "?addAction@QWidget@@QEAAXPEAVQAction@@@Z", ok);
        Res(p_QToolBar_addWidget, kWidgets, "?addWidget@QToolBar@@QEAAPEAVQAction@@PEAVQWidget@@@Z", ok);
        Res(p_QWidget_removeAction, kWidgets, "?removeAction@QWidget@@QEAAXPEAVQAction@@@Z", ok);
        Res(p_QMainWindow_menuBar, kWidgets, "?menuBar@QMainWindow@@QEBAPEAVQMenuBar@@XZ", ok);
        Res(p_QWidget_find, kWidgets, "?find@QWidget@@SAPEAV1@_K@Z", ok);
        Res(p_QMainWindow_meta, kWidgets, "?staticMetaObject@QMainWindow@@2UQMetaObject@@B", ok);
        Res(p_QDockWidget_ctor, kWidgets, "??0QDockWidget@@QEAA@AEBVQString@@PEAVQWidget@@V?$QFlags@W4WindowType@Qt@@@@@Z", ok);
        Res(p_QDockWidget_setWidget, kWidgets, "?setWidget@QDockWidget@@QEAAXPEAVQWidget@@@Z", ok);
        Res(p_QDockWidget_toggleViewAction, kWidgets, "?toggleViewAction@QDockWidget@@QEBAPEAVQAction@@XZ", ok);
        Res(p_QMainWindow_addDockWidget, kWidgets, "?addDockWidget@QMainWindow@@QEAAXW4DockWidgetArea@Qt@@PEAVQDockWidget@@@Z", ok);
        Res(p_QMainWindow_restoreDockWidget, kWidgets, "?restoreDockWidget@QMainWindow@@QEAA_NPEAVQDockWidget@@@Z", ok);
        Res(p_QObject_objectName, kCore, "?objectName@QObject@@QEBA?AVQString@@XZ", ok);
        Res(p_QMetaObject_className, kCore, "?className@QMetaObject@@QEBAPEBDXZ", ok);
        Res(p_QObject_children, kCore, "?children@QObject@@QEBAAEBV?$QList@PEAVQObject@@@@XZ", ok);
        Res(p_qFindChild_helper, kCore, "?qt_qFindChild_helper@@YAPEAVQObject@@PEBV1@AEBVQString@@AEBUQMetaObject@@V?$QFlags@W4FindChildOption@Qt@@@@@Z", ok);
        Res(p_QObject_meta, kCore, "?staticMetaObject@QObject@@2UQMetaObject@@B", ok);
        Res(p_QTabWidget_count, kWidgets, "?count@QTabWidget@@QEBAHXZ", ok);
        Res(p_QTabWidget_currentIndex, kWidgets, "?currentIndex@QTabWidget@@QEBAHXZ", ok);
        Res(p_QTabWidget_tabText, kWidgets, "?tabText@QTabWidget@@QEBA?AVQString@@H@Z", ok);
        Res(p_QTabWidget_currentWidget, kWidgets, "?currentWidget@QTabWidget@@QEBAPEAVQWidget@@XZ", ok);
        Res(p_QTreeWidget_selectedItems, kWidgets, "?selectedItems@QTreeWidget@@QEBA?AV?$QList@PEAVQTreeWidgetItem@@@@XZ", ok);
        Res(p_QTreeWidget_currentItem, kWidgets, "?currentItem@QTreeWidget@@QEBAPEAVQTreeWidgetItem@@XZ", ok);
        Res(p_QTreeWidgetItem_text, kWidgets, "?text@QTreeWidgetItem@@QEBA?AVQString@@H@Z", ok);
        Res(p_QTreeWidgetItem_toolTip, kWidgets, "?toolTip@QTreeWidgetItem@@QEBA?AVQString@@H@Z", ok);
        Res(p_QTreeWidgetItem_parent, kWidgets, "?parent@QTreeWidgetItem@@QEBAPEAV1@XZ", ok);
        Res(p_QTextEdit_append, kWidgets, "?append@QTextEdit@@QEAAXAEBVQString@@@Z", ok);
        Res(p_QPlainTextEdit_appendPlainText, kWidgets, "?appendPlainText@QPlainTextEdit@@QEAAXAEBVQString@@@Z", ok);
        Res(p_QTextEdit_toPlainText, kWidgets, "?toPlainText@QTextEdit@@QEBA?AVQString@@XZ", ok);
        Res(p_QPlainTextEdit_toPlainText, kWidgets, "?toPlainText@QPlainTextEdit@@QEBA?AVQString@@XZ", ok);
        Res(p_QListData_dispose, kCore, "?dispose@QListData@@QEAAXXZ", ok);
        Res(p_QMetaObject_superClass, kCore, "?superClass@QMetaObject@@QEBAPEBU1@XZ", ok);
        Res(p_QTabWidget_indexOf, kWidgets, "?indexOf@QTabWidget@@QEBAHPEAVQWidget@@@Z", ok);
        Res(p_QTreeWidgetItem_data, kWidgets, "?data@QTreeWidgetItem@@UEBA?AVQVariant@@HH@Z", ok);
        Res(p_QVariant_typeName, kCore, "?typeName@QVariant@@QEBAPEBDXZ", ok);
        Res(p_QVariant_toString, kCore, "?toString@QVariant@@QEBA?AVQString@@XZ", ok);
        Res(p_QVariant_dtor, kCore, "??1QVariant@@QEAA@XZ", ok);
        Res(p_QDialog_ctor, kWidgets, "??0QDialog@@QEAA@PEAVQWidget@@V?$QFlags@W4WindowType@Qt@@@@@Z", ok);
        Res(p_QWidget_setWindowTitle, kWidgets, "?setWindowTitle@QWidget@@QEAAXAEBVQString@@@Z", ok);
        Res(p_QWidget_move, kWidgets, "?move@QWidget@@QEAAXHH@Z", ok);
        Res(p_QWidget_activateWindow, kWidgets, "?activateWindow@QWidget@@QEAAXXZ", ok);
        Res(p_QMenuBar_addMenu, kWidgets, "?addMenu@QMenuBar@@QEAAPEAVQMenu@@AEBVQString@@@Z", ok);
        Res(p_QWidget_actions, kWidgets, "?actions@QWidget@@QEBA?AV?$QList@PEAVQAction@@@@XZ", ok);
        Res(p_QComboBox_currentText, kWidgets, "?currentText@QComboBox@@QEBA?AVQString@@XZ", ok);
        Res(p_QComboBox_currentIndex, kWidgets, "?currentIndex@QComboBox@@QEBAHXZ", ok);
        Res(p_QComboBox_setCurrentIndex, kWidgets, "?setCurrentIndex@QComboBox@@QEAAXH@Z", ok);
        Res(p_QComboBox_findText, kWidgets, "?findText@QComboBox@@QEBAHAEBVQString@@V?$QFlags@W4MatchFlag@Qt@@@@@Z", ok);
        Res(p_QComboBox_count, kWidgets, "?count@QComboBox@@QEBAHXZ", ok);
        Res(p_QComboBox_itemText, kWidgets, "?itemText@QComboBox@@QEBA?AVQString@@H@Z", ok);
        Res(p_QDoubleSpinBox_value, kWidgets, "?value@QDoubleSpinBox@@QEBANXZ", ok);
        Res(p_QDoubleSpinBox_setValue, kWidgets, "?setValue@QDoubleSpinBox@@QEAAXN@Z", ok);
        Res(p_QSpinBox_value, kWidgets, "?value@QSpinBox@@QEBAHXZ", ok);
        Res(p_QSpinBox_setValue, kWidgets, "?setValue@QSpinBox@@QEAAXH@Z", ok);
        Res(p_QLabel_text, kWidgets, "?text@QLabel@@QEBA?AVQString@@XZ", ok);
        Res(p_QAbstractButton_text, kWidgets, "?text@QAbstractButton@@QEBA?AVQString@@XZ", ok);
        Res(p_QAbstractButton_click, kWidgets, "?click@QAbstractButton@@QEAAXXZ", ok);
        Res(p_QWidget_toolTip, kWidgets, "?toolTip@QWidget@@QEBA?AVQString@@XZ", ok);
        Res(p_QWidget_isVisible, kWidgets, "?isVisible@QWidget@@QEBA_NXZ", ok);
        Res(p_QWidget_isEnabled, kWidgets, "?isEnabled@QWidget@@QEBA_NXZ", ok);
        Res(p_QLineEdit_editingFinished, kWidgets, "?editingFinished@QLineEdit@@QEAAXXZ", ok);
        Res(p_QLineEdit_returnPressed, kWidgets, "?returnPressed@QLineEdit@@QEAAXXZ", ok);
        Res(p_QObject_property, kCore, "?property@QObject@@QEBA?AVQVariant@@PEBD@Z", ok);
        Res(p_QObject_dynamicPropertyNames, kCore, "?dynamicPropertyNames@QObject@@QEBA?AV?$QList@VQByteArray@@@@XZ", ok);
        Res(p_QAction_text, kWidgets, "?text@QAction@@QEBA?AVQString@@XZ", ok);
        Res(p_QAction_menu, kWidgets, "?menu@QAction@@QEBAPEAVQMenu@@XZ", ok);
        Res(p_QAction_isEnabled, kWidgets, "?isEnabled@QAction@@QEBA_NXZ", ok);
        Res(p_QAction_isSeparator, kWidgets, "?isSeparator@QAction@@QEBA_NXZ", ok);
        Res(p_QAction_trigger, kWidgets, "?trigger@QAction@@QEAAXXZ", ok);
        Res(p_QMenu_menuAction, kWidgets, "?menuAction@QMenu@@QEBAPEAVQAction@@XZ", ok);
        Res(p_QMenu_clear, kWidgets, "?clear@QMenu@@QEAAXXZ", ok);
        Res(p_QMenu_addSeparator, kWidgets, "?addSeparator@QMenu@@QEAAPEAVQAction@@XZ", ok);
        Res(p_QAction_ctor, kWidgets, "??0QAction@@QEAA@AEBVQString@@PEAVQObject@@@Z", ok);
        Res(p_QAction_setEnabled, kWidgets, "?setEnabled@QAction@@QEAAX_N@Z", ok);
        Res(p_QAction_setCheckable, kWidgets, "?setCheckable@QAction@@QEAAX_N@Z", ok);
        Res(p_QAction_setChecked, kWidgets, "?setChecked@QAction@@QEAAX_N@Z", ok);
        Res(p_QAction_setText, kWidgets, "?setText@QAction@@QEAAXAEBVQString@@@Z", ok);
        Res(p_QAction_triggered, kWidgets, "?triggered@QAction@@QEAAX_N@Z", ok);
        Res(p_QAction_meta, kWidgets, "?staticMetaObject@QAction@@2UQMetaObject@@B", ok);
        Res(p_QAbstractButton_clicked, kWidgets, "?clicked@QAbstractButton@@QEAAX_N@Z", ok);
        Res(p_QAbstractButton_meta, kWidgets, "?staticMetaObject@QAbstractButton@@2UQMetaObject@@B", ok);
        Res(p_QListWidget_currentRowChanged, kWidgets, "?currentRowChanged@QListWidget@@QEAAXH@Z", ok);
        Res(p_QListWidget_itemChanged, kWidgets, "?itemChanged@QListWidget@@QEAAXPEAVQListWidgetItem@@@Z", ok);
        Res(p_QListWidget_meta, kWidgets, "?staticMetaObject@QListWidget@@2UQMetaObject@@B", ok);
        g_ok = ok;
        if (!ok) LOG("qt: binding incomplete, Qt widgets unavailable");
        return ok;
    }

    bool  Bound() { return g_bound && g_ok; }

    bool PickColor(void* parent, uint32_t initialArgb, const char* title, uint32_t& outArgb)
    {
        static int resolved = -1;
        if (resolved < 0)
        {
            bool ok = Bound();
            Res(p_QColorDialog_getColor, kWidgets, "?getColor@QColorDialog@@SA?AVQColor@@AEBV2@PEAVQWidget@@AEBVQString@@V?$QFlags@W4ColorDialogOption@QColorDialog@@@@@Z", ok);
            Res(p_QColor_setRgba, kGui, "?setRgba@QColor@@QEAAXI@Z", ok);
            Res(p_QColor_rgba, kGui, "?rgba@QColor@@QEBAIXZ", ok);
            Res(p_QColor_isValid, kGui, "?isValid@QColor@@QEBA_NXZ", ok);
            resolved = ok ? 1 : 0;
        }
        if (!resolved) return false;
        struct QColorBuf { unsigned char b[16]; } initial{}, result{};
        p_QColor_setRgba(&initial, initialArgb | 0xFF000000u);
        QStr t(title ? title : "");                 // empty = Qt's own "Select Color" title
        p_QColorDialog_getColor(&result, &initial, parent, &t, 0);   // modal, options 0 like Radiant
        if (!p_QColor_isValid(&result)) return false;                // cancelled
        outArgb = p_QColor_rgba(&result);
        return true;
    }

    bool SetEchoMode(void* lineEdit, int mode)
    {
        static int resolved = -1;
        if (resolved < 0)
        {
            bool ok = Bound();
            Res(p_QLineEdit_setEchoMode, kWidgets, "?setEchoMode@QLineEdit@@QEAAXW4EchoMode@1@@Z", ok);
            resolved = ok ? 1 : 0;
        }
        if (!resolved || !lineEdit) return false;
        p_QLineEdit_setEchoMode(lineEdit, mode);
        return true;
    }
    void* New(size_t bytes) { void* p = p_new(bytes); if (p) memset(p, 0, bytes); return p; }
    void  DeleteLater(void* obj) { if (obj) p_QObject_deleteLater(obj); }

    // every widget class used here is sizeof 48, 128 leaves room
    constexpr size_t kWidgetAlloc = 128, kLayoutAlloc = 64;

    void* Widget(void* parent) { void* w = New(kWidgetAlloc); p_QWidget_ctor(w, parent, 0); return w; }
    // <a href> links in label text open in the browser
    void* Label(const char* text, void* parent)
    {
        void* w = New(kWidgetAlloc); QStr s(text); p_QLabel_ctor(w, &s, parent, 0);
        if (p_QLabel_setOpenExternalLinks) p_QLabel_setOpenExternalLinks(w, true);
        return w;
    }
    void* LineEdit(const char* text, void* parent) { void* w = New(kWidgetAlloc); QStr s(text); p_QLineEdit_ctor(w, &s, parent); return w; }
    void* PushButton(const char* text, void* parent) { void* w = New(kWidgetAlloc); QStr s(text); p_QPushButton_ctor(w, &s, parent); return w; }
    void* CheckBox(const char* text, void* parent) { void* w = New(kWidgetAlloc); QStr s(text); p_QCheckBox_ctor(w, &s, parent); return w; }
    void* RadioButton(const char* text, void* parent) { void* w = New(kWidgetAlloc); QStr s(text); p_QRadioButton_ctor(w, &s, parent); return w; }
    void* ListWidget(void* parent) { void* w = New(kWidgetAlloc); p_QListWidget_ctor(w, parent); return w; }
    void* PlainTextEdit(void* parent) { void* w = New(kWidgetAlloc); p_QPlainTextEdit_ctor(w, parent); return w; }
    void* Action(const char* text, void* parent) { void* a = New(kWidgetAlloc); QStr s(text); p_QAction_ctor(a, &s, parent); return a; }

    void* VBox(void* parent) { void* l = New(kLayoutAlloc); if (parent) p_QVBoxLayout_ctorP(l, parent); else p_QVBoxLayout_ctor(l); return l; }
    void* HBox() { void* l = New(kLayoutAlloc); p_QHBoxLayout_ctor(l); return l; }
    void* Grid() { void* l = New(kLayoutAlloc); p_QGridLayout_ctor(l); return l; }
    void  BoxAddWidget(void* box, void* w, int stretch) { p_QBoxLayout_addWidget(box, w, stretch, 0); }
    void  BoxAddLayout(void* box, void* layout, int stretch) { p_QBoxLayout_addLayout(box, layout, stretch); }
    void  BoxAddStretch(void* box, int stretch) { p_QBoxLayout_addStretch(box, stretch); }
    void  GridAddWidget(void* grid, void* w, int row, int col) { p_QGridLayout_addWidget(grid, w, row, col, 0); }
    void  GridAddLayout(void* grid, void* layout, int row, int col) { p_QGridLayout_addLayout(grid, layout, row, col, 0); }
    void  GridColumnStretch(void* grid, int col, int stretch) { p_QGridLayout_setColumnStretch(grid, col, stretch); }
    void  LayoutMargins(void* layout, int l, int t, int r, int b) { p_QLayout_setContentsMargins(layout, l, t, r, b); }
    bool  LayoutFixedSize(void* layout) { if (!p_QLayout_setSizeConstraint) return false; p_QLayout_setSizeConstraint(layout, 3 /* QLayout::SetFixedSize */); return true; }
    std::string AppStyleSheet()
    {
        void* app = p_QCoreApplication_instance ? p_QCoreApplication_instance() : nullptr;
        if (!app || !p_QApplication_styleSheet) return {};
        QStr s; p_QApplication_styleSheet(app, &s); return s.Utf8();
    }
    void  LayoutSpacing(void* layout, int s) { p_QLayout_setSpacing(layout, s); }
    void  SetLayout(void* widget, void* layout) { p_QWidget_setLayout(widget, layout); }

    void  SetLabelText(void* label, const char* text) { QStr s(text); p_QLabel_setText(label, &s); }
    void  SetWordWrap(void* label, bool on) { p_QLabel_setWordWrap(label, on); }
    void  SetButtonText(void* button, const char* text) { QStr s(text); p_QAbstractButton_setText(button, &s); }
    void  SetEditText(void* edit, const char* text) { QStr s(text); p_QLineEdit_setText(edit, &s); }
    std::string EditText(void* edit) { QStr s; p_QLineEdit_text(edit, &s); return s.Utf8(); }
    void  SetPlaceholder(void* edit, const char* text) { QStr s(text); p_QLineEdit_setPlaceholderText(edit, &s); }
    void  SetChecked(void* button, bool on) { p_QAbstractButton_setChecked(button, on); }
    bool  IsChecked(void* button) { return p_QAbstractButton_isChecked(button); }
    bool  IsDown(void* button) { return p_QAbstractButton_isDown(button); }
    void  SetEnabled(void* widget, bool on) { p_QWidget_setEnabled(widget, on); }
    void  SetVisible(void* widget, bool on) { p_QWidget_setVisible(widget, on); }
    void  SetParent(void* widget, void* parent) { p_QWidget_setParent(widget, parent); }
    void  SetReadOnly(void* edit, bool on) { p_QPlainTextEdit_setReadOnly(edit, on); }
    void  ListClear(void* list) { p_QListWidget_clear(list); }
    void  ListAdd(void* list, const char* text) { QStr s(text); p_QListWidget_addItem(list, &s); }
    int   ListCount(void* list) { return p_QListWidget_count(list); }
    int   ListCurrentRow(void* list) { return p_QListWidget_currentRow(list); }
    void  ListSetCurrentRow(void* list, int row) { p_QListWidget_setCurrentRow(list, row); }
    void  ListSetItemText(void* list, int row, const char* text) { if (void* it = p_QListWidget_item(list, row)) { QStr s(text); p_QListWidgetItem_setText(it, &s); } }
    void  ListSetItemCheckable(void* list, int row, bool on)
    {
        void* it = p_QListWidget_item(list, row); if (!it) return;
        int f = 0; p_QListWidgetItem_flags(it, &f);
        p_QListWidgetItem_setFlags(it, on ? (f | 16 /* Qt::ItemIsUserCheckable */) : (f & ~16));
        if (on) p_QListWidgetItem_setCheckState(it, 0);
    }
    void  ListSetItemChecked(void* list, int row, bool on) { if (void* it = p_QListWidget_item(list, row)) p_QListWidgetItem_setCheckState(it, on ? 2 : 0); }
    bool  ListItemChecked(void* list, int row) { void* it = p_QListWidget_item(list, row); return it && p_QListWidgetItem_checkState(it) == 2; }
    void  ListSetItemToolTip(void* list, int row, const char* text) { if (void* it = p_QListWidget_item(list, row)) { QStr s(text); p_QListWidgetItem_setToolTip(it, &s); } }
    void  PlainSetText(void* edit, const char* text) { QStr s(text); p_QPlainTextEdit_setPlainText(edit, &s); }
    void  PlainScrollToEnd(void* edit) { p_QPlainTextEdit_moveCursor(edit, 11 /* QTextCursor::End */, 0); }
    void  SetMinimumHeight(void* widget, int h) { p_QWidget_setMinimumHeight(widget, h); }
    void  SetMaximumHeight(void* widget, int h) { p_QWidget_setMaximumHeight(widget, h); }
    void  SetMaximumWidth(void* widget, int w) { p_QWidget_setMaximumWidth(widget, w); }
    void  SetToolTip(void* widget, const char* text) { QStr s(text); p_QWidget_setToolTip(widget, &s); }
    void  SetMinimumSize(void* widget, int w, int h) { p_QWidget_setMinimumSize(widget, w, h); }
    void  Resize(void* widget, int w, int h) { p_QWidget_resize(widget, w, h); }
    void  Raise(void* widget) { p_QWidget_raise(widget); }
    void  SetAttribute(void* widget, int attribute, bool on) { p_QWidget_setAttribute(widget, attribute, on); }
    void  SetObjectName(void* obj, const char* name) { QStr s(name); p_QObject_setObjectName(obj, &s); }
    uintptr_t WinId(void* widget) { return p_QWidget_winId(widget); }
    void* MetaCast(const void* metaObject, void* obj) { return p_QMetaObject_cast(metaObject, obj); }
    void  SetFocus(void* widget) { p_QWidget_setFocus(widget, 7 /* Qt::OtherFocusReason */); }
    void  SetActionEnabled(void* action, bool on) { p_QAction_setEnabled(action, on); }
    void  SetActionChecked(void* action, bool checkable, bool on)
    {
        if (!action || !p_QAction_setCheckable || !p_QAction_setChecked) return;
        p_QAction_setCheckable(action, checkable);
        p_QAction_setChecked(action, on);
    }
    void  SetActionText(void* action, const char* text) { QStr s(text); p_QAction_setText(action, &s); }

    void* FindWidgetByHwnd(void* hwnd) { return hwnd ? p_QWidget_find(reinterpret_cast<uintptr_t>(hwnd)) : nullptr; }
    bool  IsMainWindow(void* widget) { return widget && p_QMetaObject_cast(p_QMainWindow_meta, widget) != nullptr; }
    void* DockWidget(const char* title, void* parent) { void* d = New(kWidgetAlloc); QStr s(title); p_QDockWidget_ctor(d, &s, parent, 0); return d; }
    void  DockSetWidget(void* dock, void* widget) { p_QDockWidget_setWidget(dock, widget); }
    void* DockToggleAction(void* dock) { return p_QDockWidget_toggleViewAction(dock); }
    void  AddDockWidget(void* mainWindow, int area, void* dock) { p_QMainWindow_addDockWidget(mainWindow, area, dock); }
    bool  RestoreDockWidget(void* mainWindow, void* dock) { return p_QMainWindow_restoreDockWidget(mainWindow, dock); }

    std::string ObjectName(void* obj) { QStr s; p_QObject_objectName(obj, &s); return s.Utf8(); }
    void* MetaObjectOf(void* obj)
    {
        // vtable slot 0 of every QObject is metaObject()
        using Fn = void* (*)(void*);
        return obj ? (*reinterpret_cast<Fn**>(obj))[0](obj) : nullptr;
    }
    const char* ClassName(void* obj) { void* m = MetaObjectOf(obj); return m ? p_QMetaObject_className(m) : ""; }
    std::vector<void*> Children(void* obj)
    {
        std::vector<void*> out;
        if (!obj) return out;
        // QList<QObject*>: d -> { int ref, alloc, begin, end, void* array[] }
        auto* d = *reinterpret_cast<char**>(p_QObject_children(obj));
        if (!d) return out;
        int begin = *reinterpret_cast<int*>(d + 8), end = *reinterpret_cast<int*>(d + 12);
        auto** arr = reinterpret_cast<void**>(d + 16);
        for (int i = begin; i < end && i - begin < 100000; ++i) out.push_back(arr[i]);
        return out;
    }
    void* FindChildByName(void* parent, const char* name) { QStr s(name); return parent ? p_qFindChild_helper(parent, &s, p_QObject_meta, 1 /* FindChildrenRecursively */) : nullptr; }

    namespace
    {
        // QList<T*> by value: one pointer to QListData::Data { ref, alloc, begin, end, array[] }, elements inline.
        // Copy out, then drop our reference (shared_null has ref -1).
        std::vector<void*> TakePointerList(void* d)
        {
            std::vector<void*> out;
            if (!d) return out;
            char* p = static_cast<char*>(d);
            int begin = *reinterpret_cast<int*>(p + 8), end = *reinterpret_cast<int*>(p + 12);
            auto** arr = reinterpret_cast<void**>(p + 16);
            for (int i = begin; i < end && i - begin < 100000; ++i) out.push_back(arr[i]);
            auto* ref = reinterpret_cast<volatile long*>(p);
            if (*ref != -1 && InterlockedDecrement(ref) == 0) { void* list = d; p_QListData_dispose(&list); }
            return out;
        }
    }
    int   TabCount(void* tabs) { return tabs ? p_QTabWidget_count(tabs) : 0; }
    int   TabCurrent(void* tabs) { return tabs ? p_QTabWidget_currentIndex(tabs) : -1; }
    std::string TabText(void* tabs, int index) { QStr s; p_QTabWidget_tabText(tabs, &s, index); return s.Utf8(); }
    void* TabCurrentWidget(void* tabs) { return tabs ? p_QTabWidget_currentWidget(tabs) : nullptr; }
    std::vector<void*> TreeSelectedItems(void* tree) { void* d = nullptr; if (!tree) return {}; p_QTreeWidget_selectedItems(tree, &d); return TakePointerList(d); }
    void* TreeCurrentItem(void* tree) { return tree ? p_QTreeWidget_currentItem(tree) : nullptr; }
    std::string TreeItemText(void* item, int column) { QStr s; p_QTreeWidgetItem_text(item, &s, column); return s.Utf8(); }
    std::string TreeItemToolTip(void* item, int column) { QStr s; p_QTreeWidgetItem_toolTip(item, &s, column); return s.Utf8(); }
    void* TreeItemParent(void* item) { return item ? p_QTreeWidgetItem_parent(item) : nullptr; }
    void  TextEditAppend(void* te, const char* text) { QStr s(text); p_QTextEdit_append(te, &s); }
    void  PlainAppend(void* pte, const char* text) { QStr s(text); p_QPlainTextEdit_appendPlainText(pte, &s); }
    std::string TextEditText(void* te) { QStr s; p_QTextEdit_toPlainText(te, &s); return s.Utf8(); }
    std::string PlainText(void* pte) { QStr s; p_QPlainTextEdit_toPlainText(pte, &s); return s.Utf8(); }
    int   TabIndexOf(void* tabs, void* page) { return tabs && page ? p_QTabWidget_indexOf(tabs, page) : -1; }
    std::string TreeItemData(void* item, int column, int role, bool describe)
    {
        // QVariant is 16 bytes: { union data, uint type:30, is_shared:1, is_null:1 }, hidden return pointer
        alignas(8) char var[16] = {};
        // data() is virtual (vtable slot 1): the vtable finds a subclass override
        using DataFn = void* (*)(void*, void*, int, int);
        DataFn fn = (*reinterpret_cast<DataFn**>(item))[1];
        if (!fn) fn = reinterpret_cast<DataFn>(p_QTreeWidgetItem_data);
        fn(item, var, column, role);
        const char* tn = p_QVariant_typeName(var);
        std::string out;
        if (describe) out = std::string(tn ? tn : "invalid") + ": ";
        { QStr s; p_QVariant_toString(var, &s); out += s.Utf8(); }
        p_QVariant_dtor(var);
        return out;
    }
    bool  IsA(void* obj, const char* className)
    {
        for (const void* m = MetaObjectOf(obj); m; m = p_QMetaObject_superClass(m))
            if (strcmp(p_QMetaObject_className(m), className) == 0) return true;
        return false;
    }

    void* Dialog(const char* title, void* parent)
    {
        void* d = New(kWidgetAlloc);
        // Dialog | title | system menu | close hints: explicit hints keep Qt from adding the "?" button
        p_QDialog_ctor(d, parent, 0x00000003 | 0x00001000 | 0x00002000 | 0x08000000);
        QStr s(title); p_QWidget_setWindowTitle(d, &s);
        return d;
    }
    void  Move(void* widget, int x, int y) { p_QWidget_move(widget, x, y); }
    void  ActivateWindow(void* widget) { p_QWidget_activateWindow(widget); }
    void  Geometry(void* widget, int& x, int& y, int& w, int& h)
    {
        auto* data = *reinterpret_cast<char**>(static_cast<char*>(widget) + 40);
        const int* r = reinterpret_cast<const int*>(data + 20);
        x = r[0]; y = r[1]; w = r[2] - r[0] + 1; h = r[3] - r[1] + 1;
    }
    bool  IsShown(void* topLevel)
    {
        HWND h = reinterpret_cast<HWND>(p_QWidget_winId(topLevel));
        return h && IsWindowVisible(h);
    }

    void* MenuBar(void* mainWindow) { return p_QMainWindow_menuBar(mainWindow); }
    std::vector<void*> WidgetActions(void* w) { void* d = nullptr; if (!w) return {}; p_QWidget_actions(w, &d); return TakePointerList(d); }
    std::string ComboCurrentText(void* c) { QStr s; p_QComboBox_currentText(c, &s); return s.Utf8(); }
    int   ComboCurrentIndex(void* c) { return p_QComboBox_currentIndex(c); }
    void  ComboSetCurrentIndex(void* c, int i) { p_QComboBox_setCurrentIndex(c, i); }
    int   ComboFindText(void* c, const char* text, int matchFlags) { QStr s(text); return p_QComboBox_findText(c, &s, matchFlags); }
    int   ComboCount(void* c) { return p_QComboBox_count(c); }
    std::string ComboItemText(void* c, int i) { QStr s; p_QComboBox_itemText(c, &s, i); return s.Utf8(); }
    double DoubleSpinValue(void* w) { return p_QDoubleSpinBox_value(w); }
    void  DoubleSpinSetValue(void* w, double v) { p_QDoubleSpinBox_setValue(w, v); }
    int   SpinValue(void* w) { return p_QSpinBox_value(w); }
    void  SpinSetValue(void* w, int v) { p_QSpinBox_setValue(w, v); }
    std::string LabelText(void* l) { QStr s; p_QLabel_text(l, &s); return s.Utf8(); }
    std::string ButtonText(void* b) { QStr s; p_QAbstractButton_text(b, &s); return s.Utf8(); }
    void  Click(void* b) { p_QAbstractButton_click(b); }
    std::string ToolTip(void* w) { QStr s; p_QWidget_toolTip(w, &s); return s.Utf8(); }
    bool  IsVisible(void* w) { return w && p_QWidget_isVisible(w); }
    bool  IsEnabled(void* w) { return w && p_QWidget_isEnabled(w); }
    void  EmitEditingFinished(void* edit) { p_QLineEdit_editingFinished(edit); }
    void  EmitReturnPressed(void* edit) { p_QLineEdit_returnPressed(edit); }
    void  PlainSetTextEmit(void* pte, const char* text) { QStr s(text); p_QPlainTextEdit_setPlainText(pte, &s); }
    // QObject::property(name) as text ("" when invalid), dynamic property names
    std::string ObjectProperty(void* obj, const char* name)
    {
        alignas(8) char var[16] = {};
        p_QObject_property(obj, var, name);
        const char* tn = p_QVariant_typeName(var);
        std::string out;
        if (tn) { QStr s; p_QVariant_toString(var, &s); out = s.Utf8(); }
        p_QVariant_dtor(var);
        return out;
    }
    std::vector<std::string> DynamicPropertyNames(void* obj)
    {
        std::vector<std::string> out;
        void* d = nullptr; p_QObject_dynamicPropertyNames(obj, &d);
        // QList<QByteArray>: each element is the QByteArray's QArrayData*
        for (void* e : TakePointerList(d))
        {
            if (!e) continue;
            const char* base = static_cast<const char*>(e);
            int size = *reinterpret_cast<const int*>(base + 4);
            ptrdiff_t off = *reinterpret_cast<const ptrdiff_t*>(base + 16);
            if (size > 0 && size < 4096) out.push_back(std::string(base + off, size_t(size)));
        }
        return out;
    }
    std::string ActionText(void* a) { QStr s; p_QAction_text(a, &s); return s.Utf8(); }
    void* ActionMenu(void* a) { return a ? p_QAction_menu(a) : nullptr; }
    bool  ActionEnabled(void* a) { return a && p_QAction_isEnabled(a); }
    bool  ActionIsSeparator(void* a) { return a && p_QAction_isSeparator(a); }
    void  TriggerAction(void* a) { if (a) p_QAction_trigger(a); }
    // menu labels compare without '&', a trailing "..." and case
    static std::string MenuKey(const std::string& text)
    {
        std::string k;
        for (char c : text) if (c != '&') k += char(tolower(static_cast<unsigned char>(c)));
        while (!k.empty() && (k.back() == '.' || k.back() == ' ')) k.pop_back();
        size_t tab = k.find('\t'); if (tab != std::string::npos) k = k.substr(0, tab);   // "Undo\tCtrl+Z"
        return k;
    }
    // "File/Save" paths from the menu bar down, returns the action
    void* FindMenuAction(void* mainWindow, const std::string& path)
    {
        void* holder = MenuBar(mainWindow);
        void* found = nullptr;
        size_t pos = 0;
        while (holder && pos <= path.size())
        {
            size_t slash = path.find('/', pos);
            std::string want = MenuKey(path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos));
            found = nullptr;
            std::vector<void*> acts = WidgetActions(holder);
            for (void* a : acts)
                if (!ActionIsSeparator(a) && MenuKey(ActionText(a)) == want) { found = a; break; }
            // Radiant appends the argument to entries ("Undo Paste"): accept a word prefix
            if (!found) for (void* a : acts)
                if (!ActionIsSeparator(a)) { std::string k = MenuKey(ActionText(a)); if (k.size() > want.size() && k.compare(0, want.size(), want) == 0 && k[want.size()] == ' ') { found = a; break; } }
            if (!found || slash == std::string::npos) break;
            holder = ActionMenu(found);
            pos = slash + 1;
        }
        return found;
    }
    // one "text<TAB>enabled<TAB>submenu" line per entry at `path` ("" = the menu bar)
    std::string DescribeMenu(void* mainWindow, const std::string& path)
    {
        void* holder = path.empty() ? MenuBar(mainWindow) : ActionMenu(FindMenuAction(mainWindow, path));
        std::string out;
        if (!holder) return out;
        for (void* a : WidgetActions(holder))
        {
            if (ActionIsSeparator(a)) { out += "---\n"; continue; }
            out += ActionText(a) + "\t" + (ActionEnabled(a) ? "1" : "0") + "\t" + (ActionMenu(a) ? "1" : "0") + "\n";
        }
        return out;
    }
    void* AddMenu(void* menuBar, const char* title) { QStr s(title); return p_QMenuBar_addMenu(menuBar, &s); }
    void  AddAction(void* widget, void* action) { p_QWidget_addAction(widget, action); }
    void* ToolBarAddWidget(void* toolbar, void* widget) { return p_QToolBar_addWidget(toolbar, widget); }
    void  RemoveAction(void* widget, void* action) { p_QWidget_removeAction(widget, action); }
    void* MenuAction(void* menu) { return p_QMenu_menuAction(menu); }
    void  MenuClear(void* menu) { p_QMenu_clear(menu); }
    void  MenuAddSeparator(void* menu) { p_QMenu_addSeparator(menu); }

    void* MakeEventFilter(FilterFn fn, void* user)
    {
        auto* fo = static_cast<FilterObj*>(New(sizeof(FilterObj)));
        p_QObject_ctor(fo, nullptr);
        if (!g_filterVt[0])
        {
            // QObject vtable: 0 metaObject, 1 qt_metacast, 2 qt_metacall, 3 dtor, 4 event, 5 eventFilter, 6..10 notifies
            memcpy(g_filterVt, *reinterpret_cast<void***>(fo), sizeof g_filterVt);
            g_filterVt[5] = reinterpret_cast<void*>(&FilterThunk);
        }
        *reinterpret_cast<void***>(fo) = g_filterVt;
        fo->fn = fn; fo->user = user;
        return fo;
    }
    void DisarmEventFilter(void* filter) { if (filter) static_cast<FilterObj*>(filter)->fn = nullptr; }
    void InstallFilter(void* obj, void* filter) { p_QObject_installEventFilter(obj, filter); }
    void RemoveFilter(void* obj, void* filter) { p_QObject_removeEventFilter(obj, filter); }

    bool CursorInside(void* widget)
    {
        POINT p{}; GetCursorPos(&p);
        QPoint g{ p.x, p.y }, l{};
        p_QWidget_mapFromGlobal(widget, &l, &g);
        // QWidget+40 -> QWidgetData, whose QRect crect (x1, y1, x2, y2) sits at +20
        auto* data = *reinterpret_cast<char**>(static_cast<char*>(widget) + 40);
        const int* r = reinterpret_cast<const int*>(data + 20);
        return l.x >= 0 && l.y >= 0 && l.x <= r[2] - r[0] && l.y <= r[3] - r[1];
    }

    // QObject::connect without templates: the signal is a pointer to the member-function pointer, the receiver is
    // the sender itself, the slot object is our SlotObj. Null when Qt rejected it.
    void* Connect(void* sender, Signal sig, SlotFn fn, void* user)
    {
        void* signalFn = nullptr; const void* meta = nullptr;
        switch (sig)
        {
        case Signal::ActionTriggered:        signalFn = p_QAction_triggered; meta = p_QAction_meta; break;
        case Signal::ButtonClicked:          signalFn = p_QAbstractButton_clicked; meta = p_QAbstractButton_meta; break;
        case Signal::ListCurrentRowChanged:  signalFn = p_QListWidget_currentRowChanged; meta = p_QListWidget_meta; break;
        case Signal::ListItemChanged:        signalFn = p_QListWidget_itemChanged; meta = p_QListWidget_meta; break;
        default:
            if (!PanelBound()) return nullptr;
            switch (sig)
            {
            case Signal::SliderValueChanged:     signalFn = p_QAbstractSlider_valueChanged; meta = p_QAbstractSlider_meta; break;
            case Signal::ComboIndexChanged:      signalFn = p_QComboBox_currentIndexChanged; meta = p_QComboBox_meta; break;
            case Signal::TabCurrentChanged:      signalFn = p_QTabWidget_currentChanged; meta = p_QTabWidget_meta; break;
            case Signal::ButtonToggled:          signalFn = p_QAbstractButton_toggled; meta = p_QAbstractButton_meta; break;
            case Signal::LineEditingFinished:    signalFn = reinterpret_cast<void*>(p_QLineEdit_editingFinished); meta = p_QLineEdit_meta; break;
            case Signal::TreeCurrentItemChanged: signalFn = p_QTreeWidget_currentItemChanged; meta = p_QTreeWidget_meta; break;
            case Signal::TreeItemChanged:        signalFn = p_QTreeWidget_itemChanged; meta = p_QTreeWidget_meta; break;
            case Signal::ModelRowsInserted:      signalFn = p_QAbstractItemModel_rowsInserted; meta = p_QAbstractItemModel_meta; break;
            case Signal::ModelRowsRemoved:       signalFn = p_QAbstractItemModel_rowsRemoved; meta = p_QAbstractItemModel_meta; break;
            default: break;
            }
        }
        if (!sender || !signalFn || !meta) return nullptr;
        auto* so = static_cast<SlotObj*>(New(sizeof(SlotObj)));
        so->ref = 1; so->impl = &SlotImpl; so->fn = fn; so->user = user; so->dead = false;
        // Qt compares the whole member-function pointer: 8 bytes (QAction), 16 for every QWidget (code address + zero adjustment)
        void* signalPtr[3] = { signalFn, nullptr, nullptr };
        void* conn = nullptr;                       // QMetaObject::Connection is one pointer (d_ptr)
        p_QObject_connectImpl(&conn, sender, signalPtr, sender, nullptr, so, 0 /* AutoConnection */, nullptr, meta);
        bool ok = conn != nullptr;
        p_Connection_dtor(&conn);
        if (!ok) { LOG("qt: connectImpl failed for signal %d on 0x%p", int(sig), sender); so->dead = true; return nullptr; }
        return so;
    }
    void DisarmSlot(void* slotObject) { if (slotObject) static_cast<SlotObj*>(slotObject)->fn = nullptr; }

    // ---- panel widgets (see qt.h) ----
    bool PanelBound()
    {
        static int resolved = -1;
        if (resolved >= 0) return resolved == 1;
        bool ok = Bound();
        Res(p_QSlider_ctor, kWidgets, "??0QSlider@@QEAA@W4Orientation@Qt@@PEAVQWidget@@@Z", ok);
        Res(p_QAbstractSlider_setRange, kWidgets, "?setRange@QAbstractSlider@@QEAAXHH@Z", ok);
        Res(p_QAbstractSlider_setValue, kWidgets, "?setValue@QAbstractSlider@@QEAAXH@Z", ok);
        Res(p_QAbstractSlider_value, kWidgets, "?value@QAbstractSlider@@QEBAHXZ", ok);
        Res(p_QAbstractSlider_valueChanged, kWidgets, "?valueChanged@QAbstractSlider@@QEAAXH@Z", ok);
        Res(p_QAbstractSlider_meta, kWidgets, "?staticMetaObject@QAbstractSlider@@2UQMetaObject@@B", ok);
        Res(p_QComboBox_ctor, kWidgets, "??0QComboBox@@QEAA@PEAVQWidget@@@Z", ok);
        Res(p_QComboBox_insertItem, kWidgets, "?insertItem@QComboBox@@QEAAXHAEBVQString@@AEBVQVariant@@@Z", ok);
        Res(p_QComboBox_clear, kWidgets, "?clear@QComboBox@@QEAAXXZ", ok);
        Res(p_QComboBox_currentIndexChanged, kWidgets, "?currentIndexChanged@QComboBox@@QEAAXH@Z", ok);
        Res(p_QComboBox_meta, kWidgets, "?staticMetaObject@QComboBox@@2UQMetaObject@@B", ok);
        Res(p_QVariant_ctorInt, kCore, "??0QVariant@@QEAA@H@Z", ok);
        Res(p_QVariant_ctorLongLong, kCore, "??0QVariant@@QEAA@_J@Z", ok);
        Res(p_QVariant_toLongLong, kCore, "?toLongLong@QVariant@@QEBA_JPEA_N@Z", ok);
        Res(p_QTabWidget_ctor, kWidgets, "??0QTabWidget@@QEAA@PEAVQWidget@@@Z", ok);
        Res(p_QTabWidget_addTab, kWidgets, "?addTab@QTabWidget@@QEAAHPEAVQWidget@@AEBVQString@@@Z", ok);
        Res(p_QTabWidget_setCurrentIndex, kWidgets, "?setCurrentIndex@QTabWidget@@QEAAXH@Z", ok);
        Res(p_QTabWidget_currentChanged, kWidgets, "?currentChanged@QTabWidget@@QEAAXH@Z", ok);
        Res(p_QTabWidget_meta, kWidgets, "?staticMetaObject@QTabWidget@@2UQMetaObject@@B", ok);
        Res(p_QScrollArea_ctor, kWidgets, "??0QScrollArea@@QEAA@PEAVQWidget@@@Z", ok);
        Res(p_QScrollArea_setWidget, kWidgets, "?setWidget@QScrollArea@@QEAAXPEAVQWidget@@@Z", ok);
        Res(p_QScrollArea_setWidgetResizable, kWidgets, "?setWidgetResizable@QScrollArea@@QEAAX_N@Z", ok);
        Res(p_QFrame_setFrameShape, kWidgets, "?setFrameShape@QFrame@@QEAAXW4Shape@1@@Z", ok);
        Res(p_QAbstractScrollArea_setHorizontalScrollBarPolicy, kWidgets, "?setHorizontalScrollBarPolicy@QAbstractScrollArea@@QEAAXW4ScrollBarPolicy@Qt@@@Z", ok);
        Res(p_QSplitter_ctor, kWidgets, "??0QSplitter@@QEAA@W4Orientation@Qt@@PEAVQWidget@@@Z", ok);
        Res(p_QSplitter_addWidget, kWidgets, "?addWidget@QSplitter@@QEAAXPEAVQWidget@@@Z", ok);
        Res(p_QSplitter_setStretchFactor, kWidgets, "?setStretchFactor@QSplitter@@QEAAXHH@Z", ok);
        Res(p_QSplitter_setChildrenCollapsible, kWidgets, "?setChildrenCollapsible@QSplitter@@QEAAX_N@Z", ok);
        Res(p_QSplitter_indexOf, kWidgets, "?indexOf@QSplitter@@QEBAHPEAVQWidget@@@Z", ok);
        Res(p_QTreeWidget_ctor, kWidgets, "??0QTreeWidget@@QEAA@PEAVQWidget@@@Z", ok);
        Res(p_QTreeWidget_setColumnCount, kWidgets, "?setColumnCount@QTreeWidget@@QEAAXH@Z", ok);
        Res(p_QTreeView_setHeaderHidden, kWidgets, "?setHeaderHidden@QTreeView@@QEAAX_N@Z", ok);
        Res(p_QTreeWidget_clear, kWidgets, "?clear@QTreeWidget@@QEAAXXZ", ok);
        Res(p_QTreeWidget_addTopLevelItem, kWidgets, "?addTopLevelItem@QTreeWidget@@QEAAXPEAVQTreeWidgetItem@@@Z", ok);
        Res(p_QTreeWidget_topLevelItemCount, kWidgets, "?topLevelItemCount@QTreeWidget@@QEBAHXZ", ok);
        Res(p_QTreeWidget_topLevelItem, kWidgets, "?topLevelItem@QTreeWidget@@QEBAPEAVQTreeWidgetItem@@H@Z", ok);
        Res(p_QTreeWidget_indexOfTopLevelItem, kWidgets, "?indexOfTopLevelItem@QTreeWidget@@QEBAHPEAVQTreeWidgetItem@@@Z", ok);
        Res(p_QTreeWidget_takeTopLevelItem, kWidgets, "?takeTopLevelItem@QTreeWidget@@QEAAPEAVQTreeWidgetItem@@H@Z", ok);
        Res(p_QTreeWidget_setCurrentItem, kWidgets, "?setCurrentItem@QTreeWidget@@QEAAXPEAVQTreeWidgetItem@@@Z", ok);
        Res(p_QTreeWidget_currentItemChanged, kWidgets, "?currentItemChanged@QTreeWidget@@QEAAXPEAVQTreeWidgetItem@@0@Z", ok);
        Res(p_QTreeWidget_itemChanged, kWidgets, "?itemChanged@QTreeWidget@@QEAAXPEAVQTreeWidgetItem@@H@Z", ok);
        Res(p_QTreeWidget_meta, kWidgets, "?staticMetaObject@QTreeWidget@@2UQMetaObject@@B", ok);
        Res(p_QTreeWidgetItem_ctor, kWidgets, "??0QTreeWidgetItem@@QEAA@H@Z", ok);
        Res(p_QTreeWidgetItem_addChild, kWidgets, "?addChild@QTreeWidgetItem@@QEAAXPEAV1@@Z", ok);
        Res(p_QTreeWidgetItem_childCount, kWidgets, "?childCount@QTreeWidgetItem@@QEBAHXZ", ok);
        Res(p_QTreeWidgetItem_child, kWidgets, "?child@QTreeWidgetItem@@QEBAPEAV1@H@Z", ok);
        Res(p_QTreeWidgetItem_indexOfChild, kWidgets, "?indexOfChild@QTreeWidgetItem@@QEBAHPEAV1@@Z", ok);
        Res(p_QTreeWidgetItem_takeChild, kWidgets, "?takeChild@QTreeWidgetItem@@QEAAPEAV1@H@Z", ok);
        Res(p_QTreeWidgetItem_setText, kWidgets, "?setText@QTreeWidgetItem@@QEAAXHAEBVQString@@@Z", ok);
        Res(p_QTreeWidgetItem_setData, kWidgets, "?setData@QTreeWidgetItem@@UEAAXHHAEBVQVariant@@@Z", ok);
        Res(p_QTreeWidgetItem_flags, kWidgets, "?flags@QTreeWidgetItem@@QEBA?AV?$QFlags@W4ItemFlag@Qt@@@@XZ", ok);
        Res(p_QTreeWidgetItem_setFlags, kWidgets, "?setFlags@QTreeWidgetItem@@QEAAXV?$QFlags@W4ItemFlag@Qt@@@@@Z", ok);
        Res(p_QTreeWidgetItem_checkState, kWidgets, "?checkState@QTreeWidgetItem@@QEBA?AW4CheckState@Qt@@H@Z", ok);
        Res(p_QTreeWidgetItem_setCheckState, kWidgets, "?setCheckState@QTreeWidgetItem@@QEAAXHW4CheckState@Qt@@@Z", ok);
        Res(p_QTreeWidgetItem_setExpanded, kWidgets, "?setExpanded@QTreeWidgetItem@@QEAAX_N@Z", ok);
        Res(p_QAbstractItemView_setDragDropMode, kWidgets, "?setDragDropMode@QAbstractItemView@@QEAAXW4DragDropMode@1@@Z", ok);
        Res(p_QAbstractItemView_setDefaultDropAction, kWidgets, "?setDefaultDropAction@QAbstractItemView@@QEAAXW4DropAction@Qt@@@Z", ok);
        Res(p_QAbstractItemView_model, kWidgets, "?model@QAbstractItemView@@QEBAPEAVQAbstractItemModel@@XZ", ok);
        Res(p_QAbstractItemModel_rowsInserted, kCore, "?rowsInserted@QAbstractItemModel@@QEAAXAEBVQModelIndex@@HHUQPrivateSignal@1@@Z", ok);
        Res(p_QAbstractItemModel_rowsRemoved, kCore, "?rowsRemoved@QAbstractItemModel@@QEAAXAEBVQModelIndex@@HHUQPrivateSignal@1@@Z", ok);
        Res(p_QAbstractItemModel_meta, kCore, "?staticMetaObject@QAbstractItemModel@@2UQMetaObject@@B", ok);
        Res(p_QLineEdit_meta, kWidgets, "?staticMetaObject@QLineEdit@@2UQMetaObject@@B", ok);
        Res(p_QAbstractButton_toggled, kWidgets, "?toggled@QAbstractButton@@QEAAX_N@Z", ok);
        resolved = ok ? 1 : 0;
        if (!ok) LOG("qt: panel widgets unavailable (exports above are missing)");
        return ok;
    }
    void* Slider(bool horizontal, int minimum, int maximum, void* parent)
    {
        if (!PanelBound()) return nullptr;
        void* w = New(kWidgetAlloc); p_QSlider_ctor(w, horizontal ? 1 : 2, parent);
        p_QAbstractSlider_setRange(w, minimum, maximum);
        return w;
    }
    void  SliderSetRange(void* w, int minimum, int maximum) { if (w && PanelBound()) p_QAbstractSlider_setRange(w, minimum, maximum); }
    void  SliderSetValue(void* w, int value) { if (w && PanelBound()) p_QAbstractSlider_setValue(w, value); }
    int   SliderValue(void* w) { return w && PanelBound() ? p_QAbstractSlider_value(w) : 0; }
    void* ComboBox(void* parent) { if (!PanelBound()) return nullptr; void* w = New(kWidgetAlloc); p_QComboBox_ctor(w, parent); return w; }
    void  ComboAdd(void* c, const char* text)
    {
        if (!c || !PanelBound()) return;
        QStr s(text ? text : ""); alignas(8) char var[16] = {};
        p_QVariant_ctorInt(var, p_QComboBox_count(c));
        p_QComboBox_insertItem(c, p_QComboBox_count(c), &s, var);
        p_QVariant_dtor(var);
    }
    void  ComboClear(void* c) { if (c && PanelBound()) p_QComboBox_clear(c); }
    void* TabWidget(void* parent) { if (!PanelBound()) return nullptr; void* w = New(kWidgetAlloc); p_QTabWidget_ctor(w, parent); return w; }
    int   TabAdd(void* tabs, void* page, const char* title) { if (!tabs || !page || !PanelBound()) return -1; QStr s(title ? title : ""); return p_QTabWidget_addTab(tabs, page, &s); }
    void  TabSetCurrent(void* tabs, int index) { if (tabs && PanelBound()) p_QTabWidget_setCurrentIndex(tabs, index); }
    void* ScrollArea(void* content, void* parent)
    {
        if (!PanelBound()) return nullptr;
        void* w = New(kWidgetAlloc); p_QScrollArea_ctor(w, parent);
        p_QFrame_setFrameShape(w, 0 /* NoFrame */);
        p_QAbstractScrollArea_setHorizontalScrollBarPolicy(w, 1 /* ScrollBarAlwaysOff */);
        p_QScrollArea_setWidgetResizable(w, true);
        if (content) p_QScrollArea_setWidget(w, content);
        return w;
    }
    void* Splitter(bool horizontal, void* parent)
    {
        if (!PanelBound()) return nullptr;
        void* w = New(kWidgetAlloc); p_QSplitter_ctor(w, horizontal ? 1 : 2, parent);
        p_QSplitter_setChildrenCollapsible(w, false);
        return w;
    }
    void  SplitterAdd(void* sp, void* widget, int stretch)
    {
        if (!sp || !widget || !PanelBound()) return;
        p_QSplitter_addWidget(sp, widget);
        int index = p_QSplitter_indexOf(sp, widget);
        if (index >= 0) p_QSplitter_setStretchFactor(sp, index, stretch);
    }
    void* TreeWidget(int columns, void* parent)
    {
        if (!PanelBound()) return nullptr;
        void* w = New(kWidgetAlloc); p_QTreeWidget_ctor(w, parent);
        p_QTreeWidget_setColumnCount(w, columns < 1 ? 1 : columns);
        p_QTreeView_setHeaderHidden(w, true);
        return w;
    }
    void  TreeClear(void* tree) { if (tree && PanelBound()) p_QTreeWidget_clear(tree); }
    void* TreeAddItem(void* tree, void* parentItem, const char* columns)
    {
        if (!tree || !PanelBound()) return nullptr;
        void* item = New(128); p_QTreeWidgetItem_ctor(item, 0 /* QTreeWidgetItem::Type */);
        std::string text = columns ? columns : "";
        for (int col = 0;; ++col)
        {
            size_t tab = text.find('\t');
            QStr s(text.substr(0, tab)); p_QTreeWidgetItem_setText(item, col, &s);
            if (tab == std::string::npos) break;
            text.erase(0, tab + 1);
        }
        if (parentItem) p_QTreeWidgetItem_addChild(parentItem, item); else p_QTreeWidget_addTopLevelItem(tree, item);
        return item;
    }
    void  TreeRemoveItem(void* tree, void* item)
    {
        if (!tree || !item || !PanelBound()) return;
        void* parent = p_QTreeWidgetItem_parent(item);
        void* taken = parent ? p_QTreeWidgetItem_takeChild(parent, p_QTreeWidgetItem_indexOfChild(parent, item))
                             : p_QTreeWidget_takeTopLevelItem(tree, p_QTreeWidget_indexOfTopLevelItem(tree, item));
        using DeletingDtor = void* (*)(void*, unsigned);   // vtable slot 0: the scalar deleting destructor
        if (taken) (*reinterpret_cast<DeletingDtor**>(taken))[0](taken, 1);
    }
    int   TreeChildCount(void* tree, void* item)
    {
        if (!PanelBound()) return 0;
        return item ? p_QTreeWidgetItem_childCount(item) : tree ? p_QTreeWidget_topLevelItemCount(tree) : 0;
    }
    void* TreeChild(void* tree, void* item, int index)
    {
        if (!PanelBound() || index < 0 || index >= TreeChildCount(tree, item)) return nullptr;
        return item ? p_QTreeWidgetItem_child(item, index) : p_QTreeWidget_topLevelItem(tree, index);
    }
    void  TreeSetCurrent(void* tree, void* item) { if (tree && PanelBound()) p_QTreeWidget_setCurrentItem(tree, item); }
    void  TreeSetItemText(void* item, int column, const char* text) { if (item && PanelBound()) { QStr s(text ? text : ""); p_QTreeWidgetItem_setText(item, column, &s); } }
    void  TreeSetItemId(void* item, int64_t id)
    {
        if (!item || !PanelBound()) return;
        alignas(8) char var[16] = {};
        p_QVariant_ctorLongLong(var, id);
        p_QTreeWidgetItem_setData(item, 0, 0x0100 /* Qt::UserRole */, var);
        p_QVariant_dtor(var);
    }
    int64_t TreeItemId(void* item)
    {
        if (!item || !PanelBound()) return 0;
        alignas(8) char var[16] = {};
        p_QTreeWidgetItem_data(item, var, 0, 0x0100);
        long long id = p_QVariant_toLongLong(var, nullptr);
        p_QVariant_dtor(var);
        return id;
    }
    void  TreeSetItemCheckable(void* item, bool on)
    {
        if (!item || !PanelBound()) return;
        int flags = 0; p_QTreeWidgetItem_flags(item, &flags);
        p_QTreeWidgetItem_setFlags(item, on ? (flags | 16) : (flags & ~16));   // Qt::ItemIsUserCheckable
        if (on) p_QTreeWidgetItem_setCheckState(item, 0, p_QTreeWidgetItem_checkState(item, 0));
    }
    void  TreeSetItemChecked(void* item, bool on) { if (item && PanelBound()) p_QTreeWidgetItem_setCheckState(item, 0, on ? 2 : 0); }
    bool  TreeItemChecked(void* item) { return item && PanelBound() && p_QTreeWidgetItem_checkState(item, 0) == 2; }
    void  TreeSetExpanded(void* item, bool on) { if (item && PanelBound()) p_QTreeWidgetItem_setExpanded(item, on); }
    void  TreeSetDragDrop(void* tree, bool on)
    {
        if (!tree || !PanelBound()) return;
        p_QAbstractItemView_setDragDropMode(tree, on ? 4 /* InternalMove */ : 0 /* NoDragDrop */);
        p_QAbstractItemView_setDefaultDropAction(tree, 2 /* Qt::MoveAction */);
    }
    void* ItemViewModel(void* view) { return view && PanelBound() ? p_QAbstractItemView_model(view) : nullptr; }
}
