#pragma once
// Radiant themes (a "Theme:" row in Preferences) and view color presets (bin\themes\*.colors).
#include <string>

namespace br::theme
{
// Reads [theme] name, detours the Preferences dialog constructor. Main thread, OnLoad.
void Setup();
// Applies the ini theme once the main window is up, and a Preferences choice on the next tick. OnTick.
void Tick();
// Restores Radiant's icons and removes the icon redirect. OnUnload.
void Shutdown();
// True while Radiant's Preferences window is on screen.
bool PrefsOpen();
// Handles "theme ..." and "colors ..." commands, false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
