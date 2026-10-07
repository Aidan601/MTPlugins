#pragma once
// Action log for APE: property edits, asset switches, tree selections and menu entries with old and new value.
// "Log actions" in the Output pane's title bar prints them, recorded either way.
#include <string>

namespace ba::actionlog
{
// Reads [actionlog] from APEPlus.ini and resolves Qt, hooks only when enabled (APE main thread, OnLoad).
void Setup();
// Places the title bar check box, runs the scheduled and idle grid checks, polls the tree selection. Plugin OnTick.
void Tick();
// Removes the check box. OnUnload (the host removes the detour and the event filter).
void Shutdown();
bool Enabled();
void SetEnabled(bool on);
// "actionlog [0|1|status|tail [n]|since <seq>|clear|probe|reports 0|1]", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
