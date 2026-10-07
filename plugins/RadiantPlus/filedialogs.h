#pragma once
// Windows file dialogs: Radiant's Open/Save/Import dialogs as the Windows (Explorer) dialog instead of Qt's own.
#include <string>

namespace br::filedialogs
{
// Reads [files] windows_dialogs from RadiantPlus.ini and installs the two detours. Radiant main thread, OnLoad.
void Setup();
// The setting (true = the Windows dialog).
bool Enabled();
// Sets it and saves it to the ini, applies from the next dialog opened.
void Set(bool on);
// Handles "filedialogs [on|off]", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
