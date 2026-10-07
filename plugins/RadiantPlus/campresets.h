#pragma once
// Camera movement presets: tooltips, own presets and an "Edit Camera Presets..." window in the 3D view's drop-down.
#include <string>

namespace br::campresets
{
// Reads [camera_presets] from RadiantPlus.ini and detours Radiant's mouse binding lookup. Radiant main thread, OnLoad.
void Setup();
// Takes the menu entries out again. OnUnload.
void Shutdown();
// Attaches to the drop-down once it exists, follows a switch to a built-in mode, refreshes tooltips.
void Tick();
// Handles "campreset ...", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
