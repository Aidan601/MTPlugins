#pragma once
// Color picker extras in APE's Select Color dialog: hex and 0-1 fields, a Kelvin row and Copy buttons.
#include <string>

namespace ba::colorpicker
{
// Reads [colorpicker] extras from APEPlus.ini, resolves Qt and detours the dialog. APE main thread, OnLoad.
void Install();
// Takes the extras out of an open dialog and deletes them. Plugin OnUnload.
void Shutdown();
// Handles "colorpicker ...", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
