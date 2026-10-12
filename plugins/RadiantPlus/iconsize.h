#pragma once
// Models and Textures Icon Size sliders resize the view live: the full refresh waits until the slider rests.
#include <string>

namespace br::iconsize
{
// Reads [browsers] live_icon_size and installs the detours. Radiant main thread, OnLoad.
void Setup();
// Runs the refreshes whose slider has rested. Plugin OnTick.
void Tick();
// Runs any refresh still waiting. Plugin OnUnload.
void Shutdown();
// Handles "iconsize ...", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
