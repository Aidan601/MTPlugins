#pragma once
#include <string>

// Team Create pane built from real Qt widgets (qt.h) in a Radiant dock pane (dock.h). Main thread only.
namespace pane
{
    bool Create();                                   // build the widgets and host them, false = use the popup fallback
    bool Created();
    void Tick();                                     // pending clicks, 1 s refresh
    void Toggle();
    void ShowNotice();                               // the experimental notice box (first pane opening, Team Create menu)
    bool Visible();
    std::string Command(const std::string& cmd);     // test channel (see ui_dialog.ps1): state, show, hide, local N, join IP, host, start, read, chat TEXT, chatlog, color [auto|#RRGGBB|pick]
    void Shutdown();
}
