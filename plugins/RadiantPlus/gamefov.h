#pragma once
// Game view field of view: Radiant's GAME VIEW has no FOV setting (it uses cg_fov 65), so Radiant's FOV getter is
// detoured to a value in cg_fov convention (horizontal FOV of a 4:3 view). themes.cpp adds the field.
#include <string>

namespace br::gamefov
{
// Reads [camera] game_fov from RadiantPlus.ini and detours Radiant's FOV getter. Radiant main thread, OnLoad.
void Setup();
// The game view FOV in degrees (cg_fov convention, 65 = Radiant's own).
float Fov();
// Clamps (kMin..kMax), saves it to the ini and redraws the 3D view.
void Set(float degrees);
constexpr float kMin = 30.0f, kMax = 150.0f, kDefault = 65.0f;
// Handles "gamefov [degrees]", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
