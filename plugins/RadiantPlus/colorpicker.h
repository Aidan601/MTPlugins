#pragma once
// Color picker extras: hex, 0-1 float and Kelvin rows, copy buttons, an eyedropper icon and saved recent colors
// in Radiant's color dialog.
#include <string>

namespace br::colorpicker
{
// Reads [colorpicker] from RadiantPlus.ini, resolves Qt and detours the dialog. Radiant main thread, OnLoad.
void Setup();
// Takes the extra rows out of an open dialog and deletes them. Plugin OnUnload.
void Shutdown();
// Handles "colorpicker ...", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
