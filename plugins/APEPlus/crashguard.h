#pragma once
// APE's assert box made survivable, and the preview's NaN rotations caught before they reach it.
#include <string>

namespace ba::crashguard
{
// Reads [crashguard] from APEPlus.ini and detours APE's assert handler and the CRT exit. APE main thread, OnLoad.
void Install();
// Puts a NaN or infinite play position back to 0. APE main thread, before the preview frame function.
void BeforeFrame();
// Plugin OnUnload (the host removes the detours).
void Shutdown();
// Handles "crashguard [status|ask|continue|ape|test|slerp on|off]", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
