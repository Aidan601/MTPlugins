#pragma once
// Plays the sound notetracks of the xanim in APE's preview.
#include <string>

namespace ba::notesounds
{
// Reads [notesounds] from APEPlus.ini and resolves the preview globals. APE main thread, OnLoad.
void Install();
// Follows the preview's play position and starts the sounds of the notes it passed. APE main thread, OnTick.
void Tick();
// Plays a sound alias; keep = an anim change does not stop it (the weapon preview's fire and reload sounds). APE main thread.
void PlayAlias(const std::string& alias, bool keep = true);
// Starts an alias as a loop (one voice per alias) and stops every voice of an alias: the fx player's follow sounds.
void PlayAliasLoop(const std::string& alias);
void StopAliasVoices(const std::string& alias);
// Sound notes of the anim the preview plays now (GDT sound notes and exported sndnt# notetracks), -1 before one is read.
int SoundNoteCount();
// Stops every voice, joins the loader thread and releases XAudio2. Plugin OnUnload.
void Shutdown();
// Handles "notesounds [status|on|off|dump|reindex|play <alias>|stop]", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
