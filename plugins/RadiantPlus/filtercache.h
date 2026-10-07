#pragma once
// Faster Filters-pane evaluation while Radiant rebuilds its brush cache: entity key tests are answered once per entity
// and filter within one BrushCache_Update pass, and the name pattern test skips its string copy. Same answers.
#include <string>

namespace br::filtercache
{
// Reads [performance] filter_cache and installs the two detours. Radiant main thread, OnLoad.
void Setup();
// Around BrushCache_Update (0x1402124B0) on its thread: the key test cache lives only inside.
void PassBegin();
void PassEnd();
// Handles "filtercache ...", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
