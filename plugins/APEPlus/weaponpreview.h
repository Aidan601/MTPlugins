#pragma once
// Weapon preview: a weapon selected in the asset tree plays in APE's preview, first person (arms + view gun) or third
// person (a body + world gun), with action buttons, an anim list and the weapon's sounds.
#include <string>

namespace ba::weaponpreview
{
// Reads [weaponpreview] from APEPlus.ini and detours the preview's type dispatch and the gdt entry field read. OnLoad.
void Install();
// Runs the action sequence. APE main thread, OnTick.
void Tick();
// Removes the preview bar. Plugin OnUnload.
void Shutdown();
// Bracket APE's preview frame function: a one-shot action anim holds its last frame instead of wrapping, the fx advance.
void BeforeFrame(void* widget);
void AfterFrame();
// The fx player with its camera detour and material resolver, shared with the notetrack fx. True once it runs. APE main thread.
bool EnsureFx();
// Test harness: the preview's pause flag (the tool bar icon is not updated). False without a preview.
bool SetPaused(bool paused);
// Handles "weaponpreview [status|on|off|mode 1p|3p|action <name>|anim <field>|view|set <key> <value>|trace|fxslow <rate>|fxdump|raise]", false when not ours.
bool Command(const std::string& command, std::string& reply);
}
