#pragma once
// Plays the fx notetracks of the xanim in APE's preview on the posed bone they name, through the fx player.
#include <string>

namespace ba::notefx
{
// Reads [notefx] from APEPlus.ini and sets the fx player up. APE main thread, OnLoad, after weaponpreview::Install.
void Install();
bool Enabled();
// A fx note the sound scanner passed: the action, its fx path (relative to share\raw\fx) and the bone. APE main thread.
void Fire(const std::string& action, const std::string& fx, const std::string& tag);
// True for the note actions this module plays or stops.
bool IsFxAction(const std::string& action);
// Stops every fx this module started (an anim change). APE main thread.
void StopAll();
// Pause stops the fx sounds and the particles keep their place, resume starts the loops again.
void Pause(bool paused);
// Moves each live anchor onto its bone as the anim poses it this frame. Inside APE's frame function, before the fx update.
void AfterFrame();
void Shutdown();
// Handles "notefx [status|on|off|stop|play <fx> [bone]]", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
