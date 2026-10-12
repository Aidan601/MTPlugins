#pragma once
// Radiant's auto LOD previews reduced by meshoptimizer while Launcher+ links with it (the Radiant half of APEPlus's).

namespace br::autolod
{
// Installs the reducer, simplifier and mesh cache name detours. Radiant main thread, OnLoad.
void Setup();
}
