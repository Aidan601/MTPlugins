#pragma once
#include <string>

namespace net { struct Message; }

// "Sync light builds" ([sync] sync_lights, pane check box): a build started by one mapper (bolt or Rebuild Sun / Probes / All) starts on every other mapper who has it on,
// each PC bakes on its own CPU. Caught at CCamWnd's build function (0x1402F31D0), pause, resume and cancel are not shared. The sender waits one capture pass,
// a receiver waits for incoming edits and skips the build (chat notice) with another map open, inside a prefab, real lighting off or no bake state.
namespace lightsync
{
    void Install();                                    // OnLoad: detour Radiant's build function (unavailable = no-op)
    bool Available();
    bool Enabled();
    void SetEnabled(bool on);                          // writes [sync] sync_lights
    void OnMessage(const net::Message& m);             // net thread: MSG_LIGHT_BUILD
    void Tick();                                       // main thread
    std::string Status();                              // "TeamCreate synclights"
    std::string BuildLikeBolt(bool rebuildSun, bool rebuildProbes);   // "TeamCreate synclights build [sun] [probes]", through the hook
}
