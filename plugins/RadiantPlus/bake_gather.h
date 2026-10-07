#pragma once
// Fast per-voxel gather replacing 0x141131670, same arithmetic in the same order (bit-identical, `verify` checks).
#include "bake_guiding.h"
#include <cstdint>

namespace br::bakegather
{
using GatherFn = int64_t(__fastcall*)(int64_t* ctx, int64_t voxel);
void Setup(const br::bakeguide::Env& env);   // [config] gather_fast = 1 (default), verify, 0
bool Enabled();
// The fast gather when enabled and the voxel qualifies, otherwise `original`.
int64_t Gather(GatherFn original, int64_t* ctx, int64_t voxel);
std::string Status();
void ClearStats();
bool Command(const std::string& command, std::string& reply);   // "bake gatherfast 0|1|verify|status"
}
