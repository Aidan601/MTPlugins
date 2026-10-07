#pragma once
// Interactive 2D detail: small objects are left out of the 2D frames while the user interacts, a full frame follows at rest.
#include <string>

namespace br::xylod
{
// Reads [performance] xy_lod* and installs the selection-pass hook. Main thread, OnLoad.
void Setup();
// Around one view's QtXYRender::OnRender, ms = the frame's wall time.
void RenderBegin();
void RenderEnd(double ms);
// Around one CXYWnd::XY_ProcessAllBrushes call of that frame (xy = the CXYWnd).
void PassBegin(const void* xy);
void PassEnd();
// true = leave this BrushInst out of the 2D frame (outside the view or too small). xform = float[12]
// (origin, three axes) of the holding prefab instance, null for the map itself.
bool Skip(__int64 brushInst, const float* xform);
// Requests the full-detail frame once the interaction stopped. OnTick.
void Tick();
// true while the cursor is over a 2D view.
bool CursorOn2DView();
// Handles "xylod ...", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
