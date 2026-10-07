#pragma once
#include <string>

// Start guard (two failed starts in a row ask to start without plugins, [startguard] radiant|ape|launcher = <failed starts> <pid>),
// problem report (logs, settings, plugin list and crash dumps zipped onto the Desktop, nothing is sent),
// What's new (once per MTPlugins version and application).
namespace support
{
    // start guard
    void StartGuardBegin();            // init thread, after LoadConfig: count this start, ask when the last two failed
    bool StartDecided();               // the question (if any) is answered: plugins may load
    bool SafeStart();                  // this session starts with plugins off (the user's answer)
    void NoteMapLoaded();              // Radiant: a map finished loading (the start is settled a little later)
    void Tick();                       // main thread, every host tick: marks the start settled
    std::string StartGuardStatus();    // command channel

    // problem report (main thread)
    void Report(bool ask);             // ask = the confirmation box first (menu), false = command channel
    std::string ReportStatus();

    // what's new (main thread)
    void ShowWhatsNew();               // the window, any time (Plugins menu)
    void ShowAbout();                  // logo, version, author, license, links and the loaded plugins (Plugins menu)
    void MaybeShowWhatsNew();          // after the plugins loaded: once per MTPlugins version and application, and whenever a
                                       // loaded plugin (ours or a community one) is new or has another version since last time
}
