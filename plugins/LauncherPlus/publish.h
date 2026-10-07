#pragma once
// The launcher's "Publish Mod" dialog, extended.
#include <string>

namespace publish
{
void Setup();                             // OnLoad: binds Qt, narrows the thumbnail file dialog
void Tick();                              // finds the dialog when it opens, keeps the preview and the checks current
void Shutdown();                          // OnUnload: forgets the dialog (the host removes filters and detours)
bool Command(const std::string& cmd, std::string& reply);   // "publish", "publish html <text>", "publish thumb <path>", ...
}
