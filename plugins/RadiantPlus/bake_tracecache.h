#pragma once
// Traced-hit cache: a probe's trace pass (0x1411237B0) depends only on scene geometry, placement and quality, so its
// outputs are saved under a key of those and loaded instead of tracing.
#include "bake_guiding.h"
#include <string>

namespace br::baketrace
{
// [config] trace_cache = 1 (default), verify (trace and compare), diag (log keys and hashes only), 0.
// cacheDir: folder for the cache files (created on demand).
void Setup(const br::bakeguide::Env& env, const std::wstring& cacheDir);
std::string Status();
void ClearStats();
bool Command(const std::string& command, std::string& reply);   // "bake tracecache 0|1|verify|diag|status"
}
