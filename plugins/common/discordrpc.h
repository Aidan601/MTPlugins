#pragma once
// Discord Rich Presence for the mod tools: shows which tool is in use and, in detail mode, what is open in it.
// Shared by RadiantPlus, APEPlus and LauncherPlus, the tool focused last owns the presence.
#include <string>

namespace discordrpc
{
// UTF-8. details is the first line under the application name, state the second (may be empty).
struct Activity { std::string details, state; };

// Off = nothing shown, Tool = only which tool is open, Detail = also the map, asset or build step.
enum Mode { Off = 0, Tool = 1, Detail = 2 };
// The labels the three-way switches show, in Mode order.
extern const char* const kModeLabels[3];

// art = the tool's image key in the Discord application ("radiant", "ape", "launcher"), label = the tool's name.
// Reads [discord] from the plugin's ini and starts the worker. Main thread, OnLoad.
void Setup(const char* art, const char* label);
// True about once a second in Detail mode: the plugin then builds its Activity and calls Update. Main thread.
bool Due();
void Update(const Activity& activity);
Mode GetMode();
// Sets [discord] mode and saves it to the ini.
void SetMode(Mode mode);
// Stops the worker and gives up the presence. Main thread, OnUnload.
void Shutdown();
// Handles "discord [off|tool|detail|appid <id>]", false when the command is not ours.
bool Command(const std::string& command, std::string& reply);
}
