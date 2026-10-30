#pragma once
// gdt.db reload without the main-thread freeze: the old in-memory copies are freed on a worker thread and
// a save that rewrites gdt.db several times reloads it once.
#include <string>

namespace br::gdtreload
{
// Reads [gdtdb] from RadiantPlus.ini, resolves sqlite64r.dll and installs the detours. Radiant main thread, OnLoad.
void Setup();
// Frees whatever the worker still holds and stops it. Plugin OnUnload.
void Shutdown();
// Handles "gdtreload ...", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
