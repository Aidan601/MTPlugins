#pragma once
// Draft bake tier "LL": rewrites Low's ray count constant (ten places, two are the table builders 0x14112A090,
// 0x141129C20) from 256 to 64. Setup: once per process (Light step). SetupEditor: at a build's start to match the LL radio.
#include "bake_guiding.h"
#include <cstdint>
#include <string>

namespace br::bakell
{
void Setup(const br::bakeguide::Env& env);   // [config] low_rays = 256 (Radiant's own, default) or 64
// wantDraft: the LL radio is selected (main thread, at build start), busy: a bake is running or paused.
void SetupEditor(const br::bakeguide::Env& env, bool (*wantDraft)(), bool (*busy)());
bool Switchable();                           // SetupEditor's hooks are in
std::uint32_t LowRays();                     // rays per voxel of quality index 0 (Low) right now
inline bool Active() { return LowRays() != 256; }
// A trace made with the other Low ray count: gathering it would misread the hit store.
bool Stale(const std::uint8_t* probe);
// Mixed into the trace cache key of a trace pass at this quality (0 = nothing to mix in).
std::uint32_t KeySalt(int quality);
std::string Status();
}
