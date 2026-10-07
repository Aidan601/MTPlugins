#pragma once
// Dynamic buffer twins: Radiant's big dynamic vertex and index buffers are drawn from default-usage twins that get
// only the written ranges, so Direct3D 11 stops stalling when the camera shows new geometry.
#include <string>

namespace br::dynbuffers
{
// Reads [performance] dynamic_buffers from RadiantPlus.ini. Radiant main thread, OnLoad.
void Setup();
// Hooks the device and its context once the renderer is up. Plugin OnTick.
void Tick();
// Writes every twin back into Radiant's own buffer and frees it. Plugin OnUnload.
void Shutdown();
// Handles "dynbuf ...", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
