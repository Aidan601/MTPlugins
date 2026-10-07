#pragma once
#include <string>

namespace net { struct Message; }

// "Sync saves" ([sync] sync_saves): when a mapper with it on saves the shared map, every other mapper with it on saves their copy too,
// through Radiant's File > Save. The sender waits one capture pass so its last edits arrive first. A receiver waits until incoming
// edits are applied, it is at the top level of the map, no reload runs and no mouse button is held. Other files and temporary copies are never saved.
namespace savesync
{
    bool Enabled();
    void SetEnabled(bool on);                          // writes [sync] sync_saves
    void OnLocalSave(const std::wstring& path);        // main thread, from OnMapSaved
    void OnMessage(const net::Message& m);             // net thread: MSG_SAVE
    void Tick();                                       // main thread
    std::string Status();                              // "TeamCreate syncsaves"
}
