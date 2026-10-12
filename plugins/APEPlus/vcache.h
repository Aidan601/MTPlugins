#pragma once
// Vertex cache triangle reorder of a model conversion, exact and faster.
#include <string>

namespace ba::vcache
{
// Reads [preview] fast_reorder, detours APE's reorder. OnLoad.
void Install();
// Handles "reorder [0|1|verify]" (APE's, ours, both compared), false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
