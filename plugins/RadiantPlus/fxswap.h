#pragma once
// FX Browser click applies the effect: a left click on an effect sets the selected fx entities' "fxdef" (one undo step).
#include <string>
#include <vector>

namespace br::fxswap
{
// Reads [fx] browser_apply from RadiantPlus.ini and resolves Radiant and Qt. Radiant main thread, OnLoad.
void Setup();
// Finds the FX Browser (once, then re-checked now and then) and applies a pending click. Plugin OnTick.
void Tick();
// Handles "fxswap ...", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
// Sets fxdef on the selected fx entities (one undo step). Entities changed, -1 while an undo is open, -2 when unavailable.
int ApplyToSelection(const std::string& name);
// The fxdef of every fx entity at the current editing level ("dir/name", repeats kept). Main thread.
void InUse(std::vector<std::string>& out);
// Radiant's FX Browser (QtEffectsBrowser), NULL until it exists.
void* Browser();
}
