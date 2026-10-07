#pragma once
// Themes for the Mod Tools Launcher: the sheets of <root>\bin\themes, picked in Edit > Options.
#include <string>

namespace launchertheme
{
void Setup(const std::wstring& ini);      // OnLoad: binds Qt, lists the themes
void Tick();                              // applies the ini theme once the main window is up
void Shutdown();                          // OnUnload: the launcher's own look back
bool Command(const std::string& cmd, std::string& reply);   // "theme", "theme list", "theme <name>", "theme options ..."
}
