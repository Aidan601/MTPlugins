#pragma once
// Selected model outline: a selected model in the 3D view shows its silhouette in place of the selection wireframe,
// Alt+J (View > Toggle Model Outline) switches back to the wireframe.
#include <string>

namespace br::seloutline
{
// Reads [selection], installs the detours, the hotkey and the View menu entry. Radiant main thread, OnLoad.
void Setup();
// Draws an outline still waiting when the 3D view is presented. Plugin OnDraw.
void Draw(int view);
// Releases the GPU objects. Plugin OnUnload.
void Shutdown();
// Handles "seloutline ...", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
