#pragma once
// Crash guard: small hooks that turn known Radiant crashes into log lines (or a choice) instead of a dead editor.
#include <string>

namespace br::crashguard
{
// Reads [crashguard] from RadiantPlus.ini and installs every guard that is switched on. Radiant main thread, OnLoad.
void Setup();
// Undoes the IAT patch (the host removes the detours itself). OnUnload.
void Shutdown();
// Clears the load flags a failed Map_LoadFile leaves set. Main thread, after every root map load.
void AfterMapLoad();
// Handles "crashguard ..." commands, false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
