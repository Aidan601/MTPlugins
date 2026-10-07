#pragma once
#include <string>

// New-release check: in the launcher a worker asks GitHub for the latest Aidan601/MTPlugins release and, when newer than MTPLUGINS_VERSION, asks to run its Setup exe
// (SHA-256 checked when listed, [update] check=0 switches it off). Installed plugins with a newer version in the public list (browse.h) are asked about in a later start.
// Main thread only, except the workers inside update.cpp.
namespace update
{
    bool Enabled();                  // [update] check (default 1)
    void SetEnabled(bool on);
    void StartupCheck();             // once, from the first tick: the launcher checks the release and asks about plugin
                                     // updates, the editors do nothing. Nothing when switched off or a dev build
                                     // (unless [update] url= / [browse] index= is set)
    void CheckNow();                 // command channel: check and ask even when switched off
    void Tick();                     // main thread: ask, then run the downloaded installer or install the plugin updates
    std::string Status();            // one line for the command channel
    std::string PluginStatus();      // command channel: the plugin updates, one per line
    void InstallPluginUpdates();     // command channel: install every pending plugin update (skipped ones too), no question
}
