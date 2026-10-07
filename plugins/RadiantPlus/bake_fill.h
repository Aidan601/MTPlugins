#pragma once
// Free-space fill list cache (0x14117C860): serves a byte-identical neighbour list built before for the same mask.
#include "bake_guiding.h"

namespace br::bakefill
{
void Setup(const br::bakeguide::Env& env);   // [config] fill_cache 1 (default), verify or 0, fill_cache_mb caps it (512)
std::string Status();
void Clear();                                 // drop every cached list (bake end)
bool Command(const std::string& command, std::string& reply);   // "bake fillcache 0|1|verify|status"
}
