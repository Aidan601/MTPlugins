#pragma once
// Extra image source formats (JPEG) for Radiant's image converter (the Radiant half of APEPlus's Feature 6).
#include <string>

namespace br::formats
{
// Reads [images] extra_formats from RadiantPlus.ini and installs the converter patch. Radiant main thread, OnLoad.
void Setup();
// Handles "formats" / "formats status", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
