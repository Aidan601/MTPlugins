#pragma once
// Deferred asset-cache touch: Radiant reopens each converted .lz4 for write after a read (0x140FA0300) only to set
// its last-access time. During a load that is skipped and queued here, then a worker sets the times after the load.
#include <string>

namespace br::assettouch
{
// Any thread: remember one cache file and the current time as its last-access time.
void Queue(const std::string& path);
// Main thread: set the queued times on a worker thread (no-op when nothing is queued or a worker runs).
void Flush();
// Stops the worker and waits for it (it runs code of this DLL). OnUnload.
void Shutdown();
// One-line state for the status commands.
std::string Status();
}
