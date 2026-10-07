#pragma once
// Rotation gizmo for the 3D view: while Mouse Rotate is on, three axis rings are drawn around the pivot and a drag
// on a ring turns the selection about that axis.
#include "modtools_plugin.hpp"
#include <string>

namespace br::rotgizmo
{
// Reads [gizmo] from RadiantPlus.ini and resolves Radiant's rotate-mode state. Radiant main thread, OnLoad.
void Setup();
// Ends a drag in progress and detaches from the 3D view's window. OnUnload.
void Shutdown();
// Attaches to the 3D view's window and applies the pending rotation of a drag. Plugin OnTick.
void Tick();
// Draws the rings into the 3D view and caches their screen positions for the mouse. Plugin OnDraw.
void Draw(const ModToolsDrawContext* ctx);
// The two switches in Preferences > Selection (themes.cpp): our rings and Radiant's own handles. Set saves to the ini.
bool Rings();
bool RadiantHandles();
void Set(bool rings, bool radiantHandles);
// The ring radius in pixels (30 to 400), set by the slider or the wheel over a ring (ini size_wheel). SetSize clamps and saves.
int  SizePx();
bool SizeWheel();
void SetSize(int px);
// Handles "gizmo ...", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
