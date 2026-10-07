// Direct link: bulk blocks travel straight between two PCs instead of through the VPN tunnel of the session. The host opens a port on its
// router (UPnP), or both sides punch through their NATs.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace direct
{
    using Sock = uintptr_t;
    constexpr Sock kNoSock = ~Sock(0);
    struct Offer { std::string ip; uint16_t port = 0; std::string token; uint8_t mode = 0; };   // mode 0 = dial the open port, 1 = hole punching
    // Answers one request frame of an accepted link, false closes the link. Called on that link's thread.
    using Serve = std::function<bool(uint16_t type, const std::vector<uint8_t>& payload, uint16_t& replyType, std::vector<uint8_t>& reply)>;
    using OfferFn = std::function<void(uint32_t peer, const Offer&)>;   // sends an offer to one peer, called on the server thread

    void HostEnsure(Serve serve, OfferFn send);                    // host, any thread: listener plus router port or public endpoint, once per session
    void HostAsk(uint32_t peer);                                   // host: an offer for this peer as soon as there is one
    void HostPunch(const std::string& ip, uint16_t port);          // host: a client dials from that public endpoint, dial it too
    void HostStop(bool wait);                                      // session end (wait=false) or unload (wait=true)
    std::string Status();                                          // one line, no addresses

    // Client: a link the host accepted, or kNoSock with the reason. A mode 1 offer calls announce with our public endpoint, the host needs it.
    Sock Dial(const Offer& o, int timeoutMs, const std::atomic<bool>& cancel, const std::function<void(const std::string&, uint16_t)>& announce, std::string& why);
    void Close(Sock s);
    bool SendFrame(Sock s, uint16_t type, const std::vector<uint8_t>& payload);
    bool ReadFrame(Sock s, uint16_t& type, std::vector<uint8_t>& payload, size_t maxLen);
    bool IsLan(const std::string& ip);                             // loopback or a private LAN range
    bool IsRoutable(const std::string& ip);                        // a public IPv4 (not LAN, link-local or carrier NAT)
}
