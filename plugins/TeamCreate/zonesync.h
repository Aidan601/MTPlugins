// Zone sync: a client fetches the host's built usermap (usermaps\<map>\zone: fastfiles, image packs, sound banks) so Play Test's
// Join Host runs the host's build without the sources. Only the 1 MB pieces this PC lacks travel.
#pragma once
#include <string>
#include "net.h"

namespace zonesync
{
    void OnMessage(const net::Message& m);          // network thread: MSG_ZONE_*
    void OnPeerJoined(uint32_t peerId);             // host: tell a newcomer that a build of our map exists
    void Tick();                                    // main thread: host watches its build and announces a new one
    void Shutdown();                                // OnUnload: stop and join the worker threads
    // "fetch <map>" (client: start or keep a running fetch), "status" (first word IDLE|ASKING|FETCHING|DONE|FAILED,
    // then details), "cancel", "hostplaying" (client: "1 <map>" while the host's Play Test game of the map open here is up)
    std::string Command(const std::string& arg);
}
