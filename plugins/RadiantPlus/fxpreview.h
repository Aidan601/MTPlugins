#pragma once
// FX Preview pane: plays one effect through Radiant's own fx system at a stage point in front of the camera, nothing is
// added to the map. Off unless [fxpreview] enabled=1.
#include <string>

namespace br::fxpreview
{
// Reads [fxpreview] enabled, resolves Radiant and builds the pane and its menu. Radiant main thread, OnLoad.
void Setup();
// Elapsed label, loop restarts, stale effect handles. Plugin OnTick.
void Tick();
// Kills the preview effect and shows the map again. OnUnload.
void Shutdown();
// Handles "fxpreview ...", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
// True while a preview hides the map: Cam_ProcessAllBrushes is skipped for that frame.
bool HidingMap();
// True during the same frames: the brush collection walks (entity helpers, the selection) are skipped too.
bool HidingMapHelpers();
// Enter/Leave Prefab: the levels above keep their fx list for "All In Use Top". Plugin OnBeforeScopeChange, OnScopeChanged.
void BeforeScopeChange();
void ScopeChanged(int depth);
// A map was loaded: the level lists are dropped. Plugin OnMapLoaded.
void MapLoaded();
}
