#pragma once
// Action log: selections and edits printed to the Output pane ("Log actions" box), recorded either way.
#include <string>

namespace br::actionlog
{
// Reads [actionlog] enabled from RadiantPlus.ini and resolves Qt, hooks only when enabled. Radiant main thread, OnLoad.
void Setup();
// Adds the Output pane check box once the pane exists, polls the selection, prints queued lines. Plugin OnTick.
void Tick();
// Removes the check box. OnUnload (the host removes the detours).
void Shutdown();
bool Enabled();
void SetEnabled(bool on);
// "actionlog [0|1|status|tail [n]|since <seq>|clear|probe|reports 0|1]", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
