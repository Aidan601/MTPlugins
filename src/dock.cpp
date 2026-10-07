#include <windows.h>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include "dock.h"
#include "qt.h"
#include "radiant_addrs.h"
#include "app.h"
#include "ape.h"
#include "mainthread.h"
#include "config.h"
#include "log.h"

namespace dock
{
    struct Pane
    {
        std::string title;
        BYTE* dock = nullptr;        // QRadDockWidget* (Radiant) or QDockWidget* (APE)
        void* container = nullptr;   // our own QWidget inside the dock (made native so its HWND tells us whether it is on screen)
        void* layout = nullptr;      // its QVBoxLayout, the plugin's content widget is its only item
        void* content = nullptr;     // the hosted QWidget (swapped on a plugin reload)
        HWND  contentHwnd = nullptr;
        std::string name;            // object name (title without spaces), the [panes] ini key in APE
        bool  lastVisible = true;    // last recorded shown/hidden state (the toggle action's check mark, APE's ini)
    };
}

namespace
{
    template <typename Fn> Fn Func(uintptr_t idaAddr) { return reinterpret_cast<Fn>(rt::Rva(idaAddr)); }
    template <typename T> T* Global(uintptr_t idaAddr) { return reinterpret_cast<T*>(rt::Rva(idaAddr)); }

    using QRadDock_ctor = void* (*)(void* self, const qt::QStr* title, void* bridge);
    using TADock_setWidget = void (*)(void* self, void* widget);
    using TADock_toggleView = void (*)(void* self, bool show);
    using MainWin_addDock = void (*)(void* mainWin, int area, void* dock);
    using MainWin_container = void* (*)(void* mainWin);
    using TADock_container = void* (*)(void* dock);
    using Container_remove = void (*)(void* container, void* dock);
    using Container_add = void (*)(void* container, void* dock, int fromRestore, int orientation, int side, void* relativeTo, int tabify);
    using Container_relayout = void (*)(void* container);

    std::vector<dock::Pane*> g_panes;

    void* MainWindowObject() { return *Global<void*>(rt::g_pMainWin); }

    // Radiant's own pane titled `title`: a TADockWidget whose toggle action carries the title. Floating and hidden panes
    // live in TADockWindows parented to the main window, so a walk of its object tree finds every one of them.
    void* FindRadiantDock(void* obj, const char* title, int depth)
    {
        if (depth > 24) return nullptr;
        for (void* child : qt::Children(obj))
        {
            if (qt::IsA(child, "TADockWidget"))
            {
                void* action = *reinterpret_cast<void**>(static_cast<BYTE*>(child) + rt::off_TADock_ToggleAction);
                if (action && qt::ActionText(action) == title) return child;
                continue;                                   // a pane's content holds no further panes
            }
            if (void* found = FindRadiantDock(child, title, depth + 1)) return found;
        }
        return nullptr;
    }

    // Radiant's saved dock layout (HKCU\Software\Treyarch\Radiant\QRadDockWidgetState): one line per pane, "p <name>" = hidden,
    // "d <name>" = behind another tab, "f <name>" = in front. A pane put into its empty slot is always shown, so hidden is re-applied by hand.
    enum class Saved { Unknown, Hidden, Shown };
    Saved SavedPaneState(const char* name)
    {
        DWORD size = 0;
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Treyarch\\Radiant", L"QRadDockWidgetState", RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size) return Saved::Unknown;
        std::wstring buf(size / sizeof(wchar_t) + 1, L'\0');
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Treyarch\\Radiant", L"QRadDockWidgetState", RRF_RT_REG_SZ, nullptr, &buf[0], &size) != ERROR_SUCCESS) return Saved::Unknown;
        std::wstring wname(name, name + strlen(name));
        auto lineHas = [&](const std::wstring& needle) {
            size_t pos = 0;
            while ((pos = buf.find(needle, pos)) != std::wstring::npos)
            {
                size_t end = pos + needle.size();
                bool lineStart = pos == 0 || buf[pos - 1] == L' ' || buf[pos - 1] == L'\n';
                bool lineEnd = end >= buf.size() || buf[end] == L'\n' || buf[end] == L'\r' || buf[end] == L')' || buf[end] == L'\0';
                if (lineStart && lineEnd) return true;
                pos = end;
            }
            return false;
        };
        if (lineHas(L"p " + wname)) { LOG("dock: saved layout has '%s' hidden", name); return Saved::Hidden; }
        if (lineHas(L"d " + wname) || lineHas(L"f " + wname)) { LOG("dock: saved layout has '%s' shown", name); return Saved::Shown; }
        LOG("dock: saved layout does not know '%s' yet (Radiant picks a spot)", name);
        return Saved::Unknown;
    }
}

namespace dock
{
    bool Ready()
    {
        if (app::IsLauncher()) return MainWindowWidget() != nullptr;
        if (app::IsApe()) return ape::Ready();
        if (!rt::VerifyBinary()) return false;
        void* mw = MainWindowObject();
        if (!mw) return false;
        return Func<MainWin_container>(rt::RadiantMainWin_DockContainer)(mw) != nullptr;
    }

    void* MainWindowWidget()
    {
        if (app::IsLauncher())
        {
            void* w = qt::Bind() ? qt::FindWidgetByHwnd(mt::MainWindow()) : nullptr;
            return w && qt::IsMainWindow(w) ? w : nullptr;
        }
        if (app::IsApe()) return ape::MainWindowWidget();
        return rt::VerifyBinary() ? MainWindowObject() : nullptr;
    }

    Pane* Find(const char* title)
    {
        for (Pane* p : g_panes) if (p->title == title) return p;
        return nullptr;
    }

    Pane* Host(void* content, const char* title, int w, int h)
    {
        if (!qt::Bind() || !Ready() || !content) return nullptr;
        if (Pane* existing = Find(title))
        {
            // plugin reload: swap the content inside our container, keep Radiant's dock widget (setWidget takes only one widget)
            if (existing->content && existing->content != content)
            {
                qt::SetVisible(existing->content, false);
                qt::SetParent(existing->content, nullptr);     // out of the layout now, freed when the event loop gets to it
                qt::DeleteLater(existing->content);
            }
            qt::BoxAddWidget(existing->layout, content, 1);
            qt::SetVisible(content, true);
            existing->content = content;
            LOG("dock: pane '%s' content replaced (content 0x%p)", title, content);
            return existing;
        }
        if (app::IsApe() || app::IsLauncher())
        {
            void* mw = MainWindowWidget();
            if (!mw) return nullptr;
            auto* dockObj = static_cast<BYTE*>(qt::DockWidget(title, mw));
            std::string dockName; for (const char* c = title; *c; ++c) if (*c != ' ') dockName += *c;
            qt::SetObjectName(dockObj, (dockName + "Pane").c_str());     // QMainWindow::saveState skips nameless docks
            void* container = qt::Widget(nullptr);
            void* layout = qt::VBox(container);
            qt::LayoutMargins(layout, 0, 0, 0, 0);
            qt::LayoutSpacing(layout, 0);
            qt::BoxAddWidget(layout, content, 1);
            qt::SetAttribute(container, 100 /* Qt::WA_NativeWindow */, true);
            qt::SetMinimumSize(container, 200, 120);
            qt::Resize(container, w, h);
            qt::DockSetWidget(dockObj, container);
            qt::AddDockWidget(mw, 2 /* Qt::RightDockWidgetArea */, dockObj);
            // APE restored its layout before we existed. The return value does not say whether the dock ends up shown, so the
            // shown/hidden state is kept in the host ini ([panes] <name>=, recorded by ape::Tick, unknown = shown).
            bool restored = qt::RestoreDockWidget(mw, dockObj);
            int remembered = GetPrivateProfileIntW(L"panes", std::wstring(dockName.begin(), dockName.end()).c_str(), -1, Cfg().iniPath.c_str());
            qt::SetVisible(dockObj, remembered != 0);
            Pane* pane = new Pane;
            pane->title = title; pane->dock = dockObj; pane->container = container; pane->layout = layout; pane->content = content;
            pane->contentHwnd = reinterpret_cast<HWND>(qt::WinId(container));
            pane->name = dockName; pane->lastVisible = remembered != 0;
            g_panes.push_back(pane);
            LOG("dock: APE pane '%s' created: QDockWidget 0x%p content 0x%p (hwnd 0x%p), %s, %s", title, dockObj, content, pane->contentHwnd, restored ? "placed from the saved layout" : "new (right area)", remembered < 0 ? "shown (first time)" : remembered ? "shown (remembered)" : "hidden (remembered)");
            return pane;
        }
        BYTE* mw = static_cast<BYTE*>(MainWindowObject());
        void* bridge = *reinterpret_cast<void**>(mw + rt::off_MainWin_QEChildren);
        if (!bridge || !qt::MetaCast(Global<void>(rt::RadiantQtBridge_staticMetaObject), bridge))
        {
            LOG("dock: main window 0x%p has no RadiantQtBridge at +%zu (0x%p)", mw, rt::off_MainWin_QEChildren, bridge);
            return nullptr;
        }

        // the pane itself: Radiant's QRadDockWidget, constructed by Radiant's own code
        auto* dockObj = static_cast<BYTE*>(qt::New(rt::kQRadDockWidgetAlloc));
        if (!dockObj) return nullptr;
        { qt::QStr qtitle(title); Func<QRadDock_ctor>(rt::QRadDockWidget_ctor)(dockObj, &qtitle, bridge); }
        std::string dockName; for (const char* c = title; *c; ++c) if (*c != ' ') dockName += *c;
        // Radiant persists the layout through QMainWindow::saveState too, which skips widgets without an objectName
        qt::SetObjectName(dockObj, (dockName + "Pane").c_str());
        qt::Resize(dockObj, w, h);          // the container reads the geometry as the initial size

        void* container = qt::Widget(nullptr);
        void* layout = qt::VBox(container);
        qt::LayoutMargins(layout, 0, 0, 0, 0);
        qt::LayoutSpacing(layout, 0);
        qt::BoxAddWidget(layout, content, 1);
        qt::SetAttribute(container, 100 /* Qt::WA_NativeWindow */, true);
        qt::SetMinimumSize(container, 200, 120);
        Func<TADock_setWidget>(rt::TADockWidget_setWidget)(dockObj, container);
        // addDockWidget passes fromRestore=0, which drops the pane's saved slot and tabs it beside a random pane, so a pane the
        // saved layout knows goes into its slot the way the restore does it.
        Saved saved = SavedPaneState(dockName.c_str());
        void* dockContainer = saved != Saved::Unknown ? Func<MainWin_container>(rt::RadiantMainWin_DockContainer)(mw) : nullptr;
        if (dockContainer)
        {
            dockObj[rt::off_TADock_Undocked] = 0;
            Func<Container_add>(rt::TADockContainer_add)(dockContainer, dockObj, 1, 1, 0, nullptr, 0);
        }
        else Func<MainWin_addDock>(rt::RadiantMainWin_addDockWidget)(mw, 8, dockObj);
        if (saved == Saved::Hidden) Func<TADock_toggleView>(rt::TADockWidget_toggleView)(dockObj, false);

        Pane* pane = new Pane;
        pane->title = title; pane->dock = dockObj; pane->container = container; pane->layout = layout; pane->content = content;
        pane->contentHwnd = reinterpret_cast<HWND>(qt::WinId(container));
        g_panes.push_back(pane);
        LOG("dock: pane '%s' created: dock 0x%p content 0x%p (hwnd 0x%p), main window 0x%p, %s", title, dockObj, content, pane->contentHwnd, mw,
            dockContainer ? "put in its saved slot" : "placed by Radiant");
        return pane;
    }

    bool Visible(Pane* pane)
    {
        if (!pane || !pane->container) return false;
        HWND h = reinterpret_cast<HWND>(qt::WinId(pane->container));   // Qt may recreate the native window on a re-dock
        if (h != pane->contentHwnd) pane->contentHwnd = h;
        return h && IsWindowVisible(h);
    }

    void Show(Pane* pane, bool show)
    {
        if (!pane || !pane->dock) return;
        bool vis = Visible(pane);
        if (app::IsApe() || app::IsLauncher())
        {
            if (show) { if (!vis) qt::SetVisible(pane->dock, true); qt::Raise(pane->dock); }
            else if (vis) qt::SetVisible(pane->dock, false);
            return;
        }
        if (show)
        {
            if (!vis) Func<TADock_toggleView>(rt::TADockWidget_toggleView)(pane->dock, true);
            qt::Raise(pane->dock);
        }
        else if (vis) Func<TADock_toggleView>(rt::TADockWidget_toggleView)(pane->dock, false);
    }

    int TabWith(Pane* pane, const char* beside)
    {
        if (!pane || !pane->dock || !beside || app::IsApe() || app::IsLauncher() || !rt::VerifyBinary()) return 0;
        // open but behind another tab (its container is on screen, a closed pane sits in a hidden TADockWindow): front only
        void* from = Func<TADock_container>(rt::TADockWidget_container)(pane->dock);
        if (from && qt::IsVisible(from)) { Show(pane, true); return 2; }
        void* mw = MainWindowObject();
        void* other = mw ? FindRadiantDock(mw, beside, 0) : nullptr;
        if (!other || other == pane->dock) { LOG("dock: '%s' not placed beside '%s' (no such pane)", pane->title.c_str(), beside); return 0; }
        if (!qt::IsVisible(other)) { LOG("dock: '%s' not placed beside '%s' (that pane is hidden)", pane->title.c_str(), beside); return 0; }
        void* target = Func<TADock_container>(rt::TADockWidget_container)(other);
        if (!target) { LOG("dock: '%s' not placed beside '%s' (it has no dock container)", pane->title.c_str(), beside); return 0; }
        if (from) Func<Container_remove>(rt::TADockContainer_remove)(from, pane->dock);
        Func<Container_add>(rt::TADockContainer_add)(target, pane->dock, 0, 1, 0, other, 1);
        if (from && from != target) Func<Container_relayout>(rt::TADockContainer_relayout)(from);
        Func<Container_relayout>(rt::TADockContainer_relayout)(target);
        Func<TADock_toggleView>(rt::TADockWidget_toggleView)(pane->dock, true);
        qt::Raise(pane->dock);
        LOG("dock: pane '%s' tabbed with '%s' (container 0x%p, was 0x%p)", pane->title.c_str(), beside, target, from);
        return 1;
    }

    void* ToggleAction(Pane* pane)
    {
        if (!pane || !pane->dock) return nullptr;
        if (app::IsApe() || app::IsLauncher()) return qt::DockToggleAction(pane->dock);
        return *reinterpret_cast<void**>(pane->dock + rt::off_TADock_ToggleAction);
    }

    // Every tick: a pane's shown or hidden state changed (its X button, a plugin's Show, a toggle): the toggle action's check mark follows
    // (Radiant's toggleView does not set it), APE and the launcher also save the state to the host ini.
    void RememberVisibility()
    {
        for (Pane* p : g_panes)
        {
            bool vis = Visible(p);
            if (vis == p->lastVisible) continue;
            p->lastVisible = vis;
            if (void* action = ToggleAction(p)) qt::SetActionChecked(action, true, vis);
            if (app::IsApe() || app::IsLauncher())
                WritePrivateProfileStringW(L"panes", std::wstring(p->name.begin(), p->name.end()).c_str(), vis ? L"1" : L"0", Cfg().iniPath.c_str());
        }
    }

    void Shutdown()
    {
        for (Pane* p : g_panes) delete p;     // the widgets belong to Radiant's dock now
        g_panes.clear();
    }
}
