#pragma once
// Settings > Preferences without blocking the main window.
#include <string>

namespace br::prefswin
{
// Reads [preferences] modeless, resolves Qt, hooks the Preferences slot and QDialog::exec. OnLoad.
void Setup();
// Handles "prefswin ...", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
