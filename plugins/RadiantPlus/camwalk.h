#pragma once
// Prefetch for the 3D view's editor-graphics walk (game view off). Same calls, same order, same picture.
#include <string>

namespace br::camwalk
{
// Reads [performance] cam_prefetch from RadiantPlus.ini. Radiant main thread, OnLoad.
void Setup();
// Around one BrushCache_ProcessRadBrushes call of a 3D frame (mask = its first argument). Returns a token for End.
int  IterBegin(int mask);
void IterEnd(int token, double ms);
// Per callback of that walk (camera callback 0x1402AEE10), entry = its argument (pointer to the record pointer).
void IterCallback(const void* entry);
// Handles "camwalk ...", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
