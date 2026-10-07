#pragma once
#include <string>

// Hosts plugin QWidgets in the editor's dock panes. Radiant: TADockWidget (see radiant_addrs.h "docking").
// APE: stock QDockWidgets placed from QMainWindow::saveState. Main thread only.
namespace dock
{
    struct Pane;

    bool  Ready();                                              // dock system exists (main window built)
    void* MainWindowWidget();                                   // the QMainWindow, null before it exists
    // Create the pane `title` around `content`, or replace the content of the pane with that title. Null if it cannot be hosted.
    Pane* Host(void* contentWidget, const char* title, int w, int h);
    Pane* Find(const char* title);
    bool  Visible(Pane* pane);                                  // the pane content is on screen
    void  Show(Pane* pane, bool show);                          // show + raise the pane / hide it
    // Radiant only: a closed pane joins the tab group of the pane titled `beside` (1), an open one is raised in place (2), 0 = nothing done.
    int   TabWith(Pane* pane, const char* beside);
    void* ToggleAction(Pane* pane);                             // the pane's checkable QAction (for a menu)
    void  RememberVisibility();                                 // tick: toggle action check marks follow the panes, APE saves the states to the host ini
    void  Shutdown();
}
