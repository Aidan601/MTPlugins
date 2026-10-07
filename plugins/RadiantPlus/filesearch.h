#pragma once
// Source file search cache during map loads ([performance] file_search_cache). Radiant's file finder 0x140F28F90
// walks whole directory trees on a miss, so inside one load the answers are kept per (subdir, name, recurse).
#include <string>

namespace br::filesearch
{
// Reads the ini key and installs the hook. Radiant main thread, OnLoad.
void Setup();
// The cache lives from the start of a root map load to the end of its tail (main thread).
void LoadBegin();
void LoadEnd();
// Handles "filesearch ...", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
