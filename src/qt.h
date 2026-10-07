#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

// Runtime binding to the editor's Qt 5.3 (MSVC 2012 x64) DLLs: function pointers resolved by mangled export
// name, no Qt headers. Objects come from MSVCR110's operator new (Qt deletes them with their parents).
// Main thread only.
namespace qt
{
    // QString is one pointer (QArrayData*)
    struct QStr
    {
        void* d = nullptr;
        QStr() = default;
        explicit QStr(const char* utf8);
        explicit QStr(const std::string& utf8);
        ~QStr();
        QStr(const QStr&) = delete;
        QStr& operator=(const QStr&) = delete;
        std::string Utf8() const;
    };

    bool  Bind();                              // resolve every export below (logs what is missing), false = do not use qt
    bool  Bound();
    void* New(size_t bytes);                   // zeroed, from Qt's CRT heap
    void  DeleteLater(void* qobject);          // QObject::deleteLater

    void* Widget(void* parent);
    void* Label(const char* text, void* parent);
    void* LineEdit(const char* text, void* parent);
    void* PushButton(const char* text, void* parent);
    void* CheckBox(const char* text, void* parent);
    void* RadioButton(const char* text, void* parent);
    void* ListWidget(void* parent);
    void* PlainTextEdit(void* parent);
    void* Action(const char* text, void* parent);

    void* VBox(void* parentWidgetOrNull);
    void* HBox();
    void* Grid();
    void  BoxAddWidget(void* box, void* w, int stretch = 0);
    void  BoxAddLayout(void* box, void* layout, int stretch = 0);
    void  BoxAddStretch(void* box, int stretch = 1);
    void  GridAddWidget(void* grid, void* w, int row, int col);
    void  GridAddLayout(void* grid, void* layout, int row, int col);
    void  GridColumnStretch(void* grid, int col, int stretch);
    void  LayoutMargins(void* layout, int l, int t, int r, int b);
    bool  LayoutFixedSize(void* layout);                // the widget keeps its layout's size hint, the user cannot resize it
    std::string AppStyleSheet();                        // QApplication's style sheet, empty when none
    void  LayoutSpacing(void* layout, int s);
    void  SetLayout(void* widget, void* layout);

    void  SetLabelText(void* label, const char* text);
    void  SetWordWrap(void* label, bool on);
    void  SetButtonText(void* button, const char* text);
    void  SetEditText(void* edit, const char* text);
    std::string EditText(void* edit);
    void  SetPlaceholder(void* edit, const char* text);
    void  SetChecked(void* button, bool on);
    bool  IsChecked(void* button);
    bool  IsDown(void* button);
    void  SetEnabled(void* widget, bool on);
    void  SetVisible(void* widget, bool on);
    void  SetParent(void* widget, void* parentOrNull);   // null = detach from its layout/parent (hidden top-level)
    void  SetReadOnly(void* plainTextEdit, bool on);
    void  ListClear(void* list);
    void  ListAdd(void* list, const char* text);
    int   ListCount(void* list);
    int   ListCurrentRow(void* list);
    void  ListSetCurrentRow(void* list, int row);
    void  ListSetItemText(void* list, int row, const char* text);
    void  ListSetItemCheckable(void* list, int row, bool on);
    void  ListSetItemChecked(void* list, int row, bool on);
    bool  ListItemChecked(void* list, int row);
    void  ListSetItemToolTip(void* list, int row, const char* text);
    void  PlainSetText(void* edit, const char* text);
    void  PlainScrollToEnd(void* edit);
    void  SetMinimumHeight(void* widget, int h);
    void  SetMaximumHeight(void* widget, int h);
    void  SetMaximumWidth(void* widget, int w);
    void  SetToolTip(void* widget, const char* text);
    void  SetMinimumSize(void* widget, int w, int h);
    void  Resize(void* widget, int w, int h);
    void  Raise(void* widget);
    void  SetAttribute(void* widget, int attribute, bool on);
    void  SetObjectName(void* obj, const char* name);
    uintptr_t WinId(void* widget);              // forces a native window
    void* MetaCast(const void* metaObject, void* obj);
    void  SetFocus(void* widget);
    void  SetActionEnabled(void* action, bool on);
    void  SetActionText(void* action, const char* text);
    void  SetActionChecked(void* action, bool checkable, bool on);   /* QAction::setCheckable + setChecked */

    // Stock Qt docking (APE's QMainWindow)
    void* FindWidgetByHwnd(void* hwnd);          // QWidget::find(WId), null if the HWND is not a Qt widget
    bool  IsMainWindow(void* widget);            // QMainWindow (or subclass)
    void* DockWidget(const char* title, void* parent);
    void  DockSetWidget(void* dock, void* widget);
    void* DockToggleAction(void* dock);
    void  AddDockWidget(void* mainWindow, int area, void* dock);   // area: 1 left, 2 right, 4 top, 8 bottom
    bool  RestoreDockWidget(void* mainWindow, void* dock);         // place it from the saved QMainWindow layout
    // introspection
    std::string ObjectName(void* obj);
    const char* ClassName(void* obj);            // QObject::metaObject()->className()
    std::vector<void*> Children(void* obj);
    void* FindChildByName(void* parent, const char* objectName);   // recursive, QObject::findChild<QObject*>
    void* MetaObjectOf(void* obj);
    // QTabWidget / QTreeWidget / text edits (APE's asset tabs, asset tree and Output pane)
    int   TabCount(void* tabs);
    int   TabCurrent(void* tabs);
    std::string TabText(void* tabs, int index);
    void* TabCurrentWidget(void* tabs);
    std::vector<void*> TreeSelectedItems(void* tree);
    void* TreeCurrentItem(void* tree);
    std::string TreeItemText(void* item, int column);
    std::string TreeItemToolTip(void* item, int column);
    void* TreeItemParent(void* item);
    void  TextEditAppend(void* textEdit, const char* text);        // QTextEdit::append
    void  PlainAppend(void* plainTextEdit, const char* text);      // QPlainTextEdit::appendPlainText
    std::string TextEditText(void* textEdit);                      // QTextEdit::toPlainText
    std::string PlainText(void* plainTextEdit);                    // QPlainTextEdit::toPlainText
    bool  IsA(void* obj, const char* className);                   // walks the QMetaObject chain
    int   TabIndexOf(void* tabs, void* page);                      // QTabWidget::indexOf
    // QTreeWidgetItem::data(column, role) as text: "<typeName>: <toString>" (debug), or the string value only
    std::string TreeItemData(void* item, int column, int role, bool describe);

    // top-level dialogs, parented to the main window
    void* Dialog(const char* title, void* parent);          // QDialog, hidden, not deleted on close
    void  Move(void* widget, int x, int y);
    void  ActivateWindow(void* widget);
    void  Geometry(void* widget, int& x, int& y, int& w, int& h);   // QWidgetData::crect (screen coordinates for top levels)
    bool  IsShown(void* topLevel);                           // the native window exists and is visible

    void* MenuBar(void* mainWindow);
    void* AddMenu(void* menuBar, const char* title);
    void  AddAction(void* widget, void* action);
    void* ToolBarAddWidget(void* toolbar, void* widget);
    void  RemoveAction(void* widget, void* action);
    void* MenuAction(void* menu);               // QMenu::menuAction (the entry in the menu bar)
    // Qt's colour dialog, modal. false = cancelled or unavailable.
    bool  PickColor(void* parent, uint32_t initialArgb, const char* title, uint32_t& outArgb);
    // QLineEdit::setEchoMode: 0 Normal, 1 NoEcho, 2 Password, 3 PasswordEchoOnEdit. false = export unavailable.
    bool  SetEchoMode(void* lineEdit, int mode);
    std::vector<void*> WidgetActions(void* widget);   // QWidget::actions (a menu's or the menu bar's entries)
    // property-widget reads and writes (APE): setters emit the widgets' own signals
    std::string ComboCurrentText(void* combo);
    int   ComboCurrentIndex(void* combo);
    void  ComboSetCurrentIndex(void* combo, int index);
    int   ComboFindText(void* combo, const char* text, int matchFlags);   // Qt::MatchFlags as int: 8 = fixed string, 16 = case sensitive
    int   ComboCount(void* combo);
    std::string ComboItemText(void* combo, int index);
    double DoubleSpinValue(void* spin);
    void  DoubleSpinSetValue(void* spin, double v);
    int   SpinValue(void* spin);
    void  SpinSetValue(void* spin, int v);
    std::string LabelText(void* label);
    std::string ButtonText(void* button);
    void  Click(void* button);                   // QAbstractButton::click: toggles and emits like a real click
    std::string ToolTip(void* widget);
    bool  IsVisible(void* widget);
    bool  IsEnabled(void* widget);
    void  EmitEditingFinished(void* lineEdit);   // fires QLineEdit::editingFinished (what Enter/focus-out does)
    void  EmitReturnPressed(void* lineEdit);
    void  PlainSetTextEmit(void* plainTextEdit, const char* text);
    std::string ObjectProperty(void* obj, const char* name);
    std::vector<std::string> DynamicPropertyNames(void* obj);
    std::string ActionText(void* action);
    void* ActionMenu(void* action);              // the submenu an action opens, or null
    bool  ActionEnabled(void* action);
    bool  ActionIsSeparator(void* action);
    void  TriggerAction(void* action);           // QAction::trigger: exactly what the user's click or shortcut does
    void* FindMenuAction(void* mainWindow, const std::string& path);   // "Edit/Undo" ('&', "...", case ignored)
    std::string DescribeMenu(void* mainWindow, const std::string& path); // entries of a menu, "" = the menu bar
    void  MenuClear(void* menu);
    void  MenuAddSeparator(void* menu);

    // Event filter without moc (eventFilter vtable slot redirected to `fn`). fn returns true to swallow the event.
    // Types used: 3 MouseButtonRelease, 6 KeyPress.
    using FilterFn = bool (*)(void* user, void* watched, int type);
    void* MakeEventFilter(FilterFn fn, void* user);
    void  DisarmEventFilter(void* filter);      // fn is never called again (plugin unload)
    void  InstallFilter(void* obj, void* filter);
    void  RemoveFilter(void* obj, void* filter);
    bool  CursorInside(void* widget);           // the Win32 cursor is over the widget's rectangle

    // Panel widgets: resolved on first use, creators return null when an export is missing.
    // Tree items are QTreeWidgetItem (not QObjects), the tree deletes them on TreeClear.
    bool  PanelBound();
    void* Slider(bool horizontal, int minimum, int maximum, void* parent);
    void  SliderSetRange(void* slider, int minimum, int maximum);
    void  SliderSetValue(void* slider, int value);   // emits valueChanged when it changes
    int   SliderValue(void* slider);
    void* ComboBox(void* parent);
    void  ComboAdd(void* combo, const char* text);
    void  ComboClear(void* combo);
    void* TabWidget(void* parent);
    int   TabAdd(void* tabs, void* page, const char* title);
    void  TabSetCurrent(void* tabs, int index);
    void* ScrollArea(void* content, void* parent);   // frameless, content resizes with the area, vertical scrolling only
    void* Splitter(bool horizontal, void* parent);
    void  SplitterAdd(void* splitter, void* widget, int stretch);
    void* TreeWidget(int columns, void* parent);   // header hidden
    void  TreeClear(void* tree);
    void* TreeAddItem(void* tree, void* parentItemOrNull, const char* tabSeparatedColumns);
    void  TreeRemoveItem(void* tree, void* item);   // removes and deletes it with its children
    int   TreeChildCount(void* tree, void* itemOrNull);   // null = top-level items
    void* TreeChild(void* tree, void* itemOrNull, int index);
    void  TreeSetCurrent(void* tree, void* item);
    void  TreeSetItemText(void* item, int column, const char* text);
    void  TreeSetItemId(void* item, int64_t id);  // Qt::UserRole data on column 0
    int64_t TreeItemId(void* item);
    void  TreeSetItemCheckable(void* item, bool on);   // column 0 check box
    void  TreeSetItemChecked(void* item, bool on);
    bool  TreeItemChecked(void* item);
    void  TreeSetExpanded(void* item, bool on);
    void  TreeSetDragDrop(void* tree, bool on);   // InternalMove: the user reorders and re-parents items by dragging
    void* ItemViewModel(void* view);              // QAbstractItemView::model (sender of the rows signals)

    // Signal connections without moc (QObject::connectImpl with a hand-made slot object).
    // fn(user, args): args[0] is the return slot, args[1..] the signal arguments.
    using SlotFn = void (*)(void* user, void** args);
    // args per signal: SliderValueChanged/ComboIndexChanged/TabCurrentChanged int*, ButtonToggled bool*,
    // TreeCurrentItemChanged item** previous**, TreeItemChanged item** int* column, ModelRows* (model signals) index* int* int*.
    enum class Signal { ActionTriggered, ButtonClicked, ListCurrentRowChanged, ListItemChanged,
                        SliderValueChanged, ComboIndexChanged, TabCurrentChanged, ButtonToggled, LineEditingFinished,
                        TreeCurrentItemChanged, TreeItemChanged, ModelRowsInserted, ModelRowsRemoved };
    void* Connect(void* sender, Signal sig, SlotFn fn, void* user);   // returns the slot object (for Disarm), null on failure
    void  DisarmSlot(void* slotObject);         // fn is never called again
}
