#pragma once
// Map load file prefetch ([performance] load_prefetch): the read-only opens of a load are recorded per map, the next load opens and closes them
// on worker threads first so Defender's scans run in parallel.
#include <string>

namespace br::loadpf
{
// Reads the ini key and installs the kernelbase CreateFileW/A recorders. Radiant main thread, OnLoad.
void Setup(const std::wstring& pluginDir);
// Around every root map load (the host's OnMapLoading / OnMapLoaded, main thread).
void MapLoadBegin(const wchar_t* path);
void MapLoadEnd();
// Saves the list once the load's tail (opens after Map_LoadFile, first frame) is over. Main thread.
void Tick();
// Stops the prefetch threads and waits for them (they run this DLL's code). OnUnload.
void Shutdown();
// Handles "loadprefetch ...", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
