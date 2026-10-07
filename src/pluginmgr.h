#pragma once
#include <string>

// The "Plugins" window, menu and Ctrl+Shift+P. Also starts the plugin registry once the main window
// exists. Main thread only. Size and position: host ini [window] plugins_dialog.
namespace pluginmgr
{
    void Tick();       // from the idle tick: create the pane when the dock is ready, then keep it current
    void Show();
    void Toggle();
    void ShowMode(bool browse);   // show the window in Installed or Browse mode (command channel, tests)
    bool Visible();
    void Shutdown();
}
