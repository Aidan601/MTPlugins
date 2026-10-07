#pragma once
#include <cstdint>
#include <string>
#include "net.h"

// Diagnostics sharing: peers stream their host log to each other and ship recent crash dumps plus the
// previous session's log tail on join. Receivers store them under <PluginDir>\peerlogs\<peer name>\.
// Gated by [diag] share_logs / keep_peer_logs.
namespace diag
{
    void Init();                                    // main thread, plugin load: remember where the host log ends
    void Shutdown();
    void Tick();                                    // main thread: stream log lines, look for dumps, run shipments
    void OnConnected();                             // client: host link up (ship previous tail + dumps to everyone)
    void OnPeerJoined(uint32_t peerId);             // host: ship the same to the newcomer
    void OnMessage(const net::Message& m, const std::string& peerName);   // net thread: MSG_LOG_LINES / MSG_FILE
    void ShipNow();                                 // command channel: ship dumps + previous tail now
    std::string Status();                           // one line for the command channel
    std::wstring PeerLogsDir();                     // <PluginDir>\peerlogs
}
