#pragma once
// Techset include search cache for APE's startup techsetdef scan.
#include <string>

namespace ba::filesearch
{
// Reads [startup] techset_search_cache, detours the finder, opens the cache while the scan thread still runs. OnLoad.
void Install();
// Closes the cache and logs the counts once the scan thread has finished. APE main thread, OnTick.
void Tick();
// Handles "filesearch [status|0|1|verify]", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
