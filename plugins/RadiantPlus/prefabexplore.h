#pragma once
// Prefab Browser "Show in Explorer": a right-click menu entry that opens Explorer on the prefab or folder.
#include <string>

namespace br::prefabexplore
{
// Reads [prefabs] show_in_explorer from RadiantPlus.ini and resolves Radiant and Qt. Radiant main thread, OnLoad.
void Setup();
// Finds the Prefab Browser (once, then re-checked now and then) and adds the menu entry. Plugin OnTick.
void Tick();
// Takes the menu entry out of Radiant's menu. Plugin OnUnload.
void Shutdown();
// Handles "prefabexplore ...", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
