#pragma once
#include <string>

// New-release check: in the launcher a worker asks GitHub for the latest Aidan601/MTPlugins release and, when newer than MTPLUGINS_VERSION, asks to run its Setup exe
// (SHA-256 checked when listed, [update] check=0 switches it off). Every app asks about its installed plugins with a newer version in the public list (browse.h).
// Main thread only, except the workers inside update.cpp.
namespace update
{
    bool Enabled();                  // [update] check (default 1): the MTPlugins release question
    void SetEnabled(bool on);
    bool PluginsEnabled();           // [update] plugins (default: the value of check): the plugin update question
    void SetPluginsEnabled(bool on);
    void StartupCheck();             // once, from the first tick: every app loads the plugin list and asks about its plugins,
                                     // the launcher also checks the release (installed copies or [update] url=)
    void CheckNow();                 // Check for updates (menu, command channel): release and plugins, asks even when switched off
                                     // and answers "up to date" when nothing is new
    void Tick();                     // main thread: ask, then run the downloaded installer or install the plugin updates
    std::string Status();            // one line for the command channel
    std::string PluginStatus();      // command channel: the plugin updates, one per line
    void InstallPluginUpdates();     // command channel: install every pending plugin update (skipped ones too), no question
}
