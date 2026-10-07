#pragma once
// [light] prefetch_files: the files Radiant opens while loading the map go to plugins\LauncherPlus\light_prefetch\<map>_<hash>.txt
// when the bake starts, and the next Light step opens them on worker threads from injection on (parallel Defender scans).
// Written, .db and .tmp files and %TEMP% are never listed.
#include <string>

namespace lightpf
{
// Installs the CreateFileW/A recorders (MinHook initialised) and starts the prefetch threads when a list exists.
// dir: the folder of BuildPlusLightRT.dll.
bool Start(const std::wstring& dir, void (*log)(const char*));
// Writes this run's list (tmp + rename), stops recording, returns a one-line report. Called once, when the export loop reaches the bake.
std::string Save();
}
