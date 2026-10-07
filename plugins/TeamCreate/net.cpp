#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mutex>
#include <thread>
#include <atomic>
#include <map>
#include <deque>
#include <condition_variable>
#include <algorithm>
#include "net.h"
#include "compat.h"

#pragma comment(lib, "ws2_32.lib")

namespace
{
    struct Conn
    {
        SOCKET               s = INVALID_SOCKET;
        uint32_t             peerId = 0;
        std::string          name;
        uint32_t             color = 0;           // host: the camera colour this client announced (0 = automatic)
        std::string          map;                 // host: the root map this client has open (HELLO, then MAP_INFO)
        std::string          ip;                  // host: the client's IPv4 as accepted (Play Test's LAN peer list)
        std::vector<uint8_t> inbuf;
        std::thread          thread;
        std::atomic<bool>    alive{ true };
        // outgoing frames are queued and written by a sender thread, so a slow reader never blocks the caller
        std::mutex                       qmutex;
        std::condition_variable          qcv;
        // Priority tiers: 0 session traffic (cameras, edits, chat, asset manifests and requests), 1 asset chunks,
        // 2 diagnostics. A lower tier is always drained first.
        std::deque<std::vector<uint8_t>> queue[3];
        size_t                           queuedBytes[3] = { 0, 0, 0 };
    };
    int Tier(uint16_t type)
    {
        // Asset definitions and converted-file lists are bulk too (a material definition is about 16 KB)
        if (type == net::MSG_ASSET_CHUNK || type == net::MSG_ASSET_CHUNK_Z || type == net::MSG_ASSET_MANIFEST || type == net::MSG_ASSET_CONVERTED) return 1;
        if (type == net::MSG_ZONE_CHUNK || type == net::MSG_ZONE_MANIFEST || type == net::MSG_ZONE_MANIFEST2) return 1;   // Join Host download
        if (type == net::MSG_LOG_LINES || type == net::MSG_FILE || type == net::MSG_FILE_PART) return 2;
        return 0;
    }
    constexpr size_t kQueueSoftLimit = 1 << 20;     // above this, camera frames are dropped
    constexpr size_t kQueueHardLimit = 256 << 20;   // above this, the connection is dead

    std::mutex                          g_mutex;
    net::Handler                        g_handler;
    std::atomic<bool>                   g_running{ false };
    bool                                g_isHost = false;
    std::atomic<bool>                   g_connected{ false };
    uint32_t                            g_localId = 0;
    uint32_t                            g_nextPeerId = 2;
    std::string                         g_name, g_mapName, g_status = "off";
    std::string                         g_hostAddr;              // client: the host's IPv4 as connected
    SOCKET                              g_listen = INVALID_SOCKET;
    std::thread                         g_acceptThread;
    std::map<uint32_t, std::shared_ptr<Conn>> g_conns;      // host: clients, client: {1: host}
    bool                                g_wsa = false;
    std::atomic<bool>                   g_connecting{ false };   // a ConnectAsync dial is in flight
    std::atomic<uint32_t>               g_gen{ 0 };              // bumped by Stop/ConnectAsync, a late dial is discarded
    std::atomic<uint32_t>               g_color{ 0 };            // our camera colour (ARGB, 0 = automatic)

    // The host does not call OnUnload when Radiant exits, so g_acceptThread is still joinable at static destruction
    // and ~thread calls std::terminate (0xC0000409). Windows has already ended the thread: detach the handle.
    // Declared after g_acceptThread so it is destroyed first.
    struct ExitThreadGuard { ~ExitThreadGuard() { if (g_acceptThread.joinable()) g_acceptThread.detach(); } } g_exitThreadGuard;

    void SetStatus(const std::string& s) { std::lock_guard<std::mutex> lock(g_mutex); g_status = s; LOG("net: %s", s.c_str()); }

    bool SendAll(SOCKET s, const uint8_t* data, size_t len)
    {
        while (len)
        {
            int n = send(s, reinterpret_cast<const char*>(data), int(len), 0);
            if (n <= 0) return false;
            data += n; len -= n;
        }
        return true;
    }

    std::vector<uint8_t> Frame(uint16_t type, uint32_t from, const std::vector<uint8_t>& payload)
    {
        std::vector<uint8_t> f;
        f.reserve(10 + payload.size());
        uint32_t len = uint32_t(payload.size());
        for (int i = 0; i < 4; ++i) f.push_back(uint8_t(len >> (8 * i)));
        f.push_back(uint8_t(type)); f.push_back(uint8_t(type >> 8));
        for (int i = 0; i < 4; ++i) f.push_back(uint8_t(from >> (8 * i)));
        f.insert(f.end(), payload.begin(), payload.end());
        return f;
    }

    void SenderLoop(std::shared_ptr<Conn> c)
    {
        for (;;)
        {
            std::vector<uint8_t> f;
            {
                std::unique_lock<std::mutex> lk(c->qmutex);
                auto any = [&] { return !c->queue[0].empty() || !c->queue[1].empty() || !c->queue[2].empty(); };
                c->qcv.wait(lk, [&] { return any() || !c->alive || !g_running; });
                if (!any()) break;
                int t = !c->queue[0].empty() ? 0 : !c->queue[1].empty() ? 1 : 2;
                f = std::move(c->queue[t].front()); c->queue[t].pop_front(); c->queuedBytes[t] -= f.size();
            }
            if (!SendAll(c->s, f.data(), f.size())) { c->alive = false; break; }
        }
    }

    void StartSender(const std::shared_ptr<Conn>& c)
    {
        std::thread(SenderLoop, c).detach();
    }

    void SendFrameTo(const std::shared_ptr<Conn>& c, const std::vector<uint8_t>& frame)
    {
        if (!c->alive) return;
        std::lock_guard<std::mutex> lk(c->qmutex);
        uint16_t type = frame.size() >= 6 ? uint16_t(frame[4] | (frame[5] << 8)) : 0;
        if (type == net::MSG_CAMERA && c->queuedBytes[0] > kQueueSoftLimit) return;   // stale by the time it goes out
        size_t total = c->queuedBytes[0] + c->queuedBytes[1] + c->queuedBytes[2];
        if (total > kQueueHardLimit) { LOG("net: peer %u is not reading (%zu bytes queued); dropping it", c->peerId, total); c->alive = false; c->qcv.notify_all(); return; }
        int t = Tier(type);
        c->queue[t].push_back(frame); c->queuedBytes[t] += frame.size();
        c->qcv.notify_all();
    }

    void BroadcastFrame(const std::vector<uint8_t>& frame, uint32_t except)
    {
        std::vector<std::shared_ptr<Conn>> targets;
        { std::lock_guard<std::mutex> lock(g_mutex); for (auto& kv : g_conns) if (kv.first != except) targets.push_back(kv.second); }
        for (auto& c : targets) SendFrameTo(c, frame);
    }

    void Dispatch(const net::Message& m)
    {
        net::Handler h;
        { std::lock_guard<std::mutex> lock(g_mutex); h = g_handler; }
        if (h) h(m);
    }

    void RemoveConn(uint32_t peerId)
    {
        std::shared_ptr<Conn> c;
        { std::lock_guard<std::mutex> lock(g_mutex); auto it = g_conns.find(peerId); if (it != g_conns.end()) { c = it->second; g_conns.erase(it); } }
        if (!c) return;
        c->alive = false; c->qcv.notify_all();
        if (c->s != INVALID_SOCKET) { shutdown(c->s, SD_BOTH); closesocket(c->s); c->s = INVALID_SOCKET; }
        if (g_isHost)
        {
            net::Writer w; w.u32(peerId);
            BroadcastFrame(Frame(net::MSG_PEER_LEFT, net::kHostPeerId, w.buf), 0);
            net::Message m; m.type = net::MSG_PEER_LEFT; m.from = net::kHostPeerId; m.payload = w.buf;
            Dispatch(m);
            SetStatus("host: peer " + std::to_string(peerId) + " (" + c->name + ") left");
        }
        else
        {
            g_connected = false;
            SetStatus("client: disconnected from host");
        }
    }

    // Reads frames from one socket until it closes. Host: per client. Client: the host link.
    void ReadLoop(std::shared_ptr<Conn> c)
    {
        uint8_t chunk[16384];
        while (g_running && c->alive)
        {
            int n = recv(c->s, reinterpret_cast<char*>(chunk), sizeof chunk, 0);
            if (n <= 0) break;
            c->inbuf.insert(c->inbuf.end(), chunk, chunk + n);
            for (;;)
            {
                if (c->inbuf.size() < 10) break;
                uint32_t len = 0; for (int i = 0; i < 4; ++i) len |= uint32_t(c->inbuf[i]) << (8 * i);
                if (len > 64 * 1024 * 1024) { LOG("net: absurd frame length %u, dropping connection", len); c->alive = false; break; }
                if (c->inbuf.size() < 10 + len) break;
                net::Message m;
                m.type = c->inbuf[4] | (c->inbuf[5] << 8);
                m.from = 0; for (int i = 0; i < 4; ++i) m.from |= uint32_t(c->inbuf[6 + i]) << (8 * i);
                m.payload.assign(c->inbuf.begin() + 10, c->inbuf.begin() + 10 + len);
                c->inbuf.erase(c->inbuf.begin(), c->inbuf.begin() + 10 + len);

                if (g_isHost)
                {
                    m.from = c->peerId;                       // never trust the client's from field
                    if (m.type == net::MSG_HELLO)
                    {
                        net::Reader r(m.payload);
                        uint16_t proto = r.u16(); std::string name = r.str(); std::string map = r.str();
                        uint32_t color = r.u32(); if (!r.ok) color = 0;     // older peer without a colour
                        c->name = name.empty() ? "peer" + std::to_string(c->peerId) : name;
                        c->color = color;
                        c->map = map;
                        LOG("net: hello from peer %u '%s' proto=%u map='%s' color=%08X", c->peerId, c->name.c_str(), proto, map.c_str(), color);
                        std::string myMap; { std::lock_guard<std::mutex> lock(g_mutex); myMap = g_mapName; }
                        net::Writer w; w.u32(c->peerId); w.str(g_name); w.str(myMap); w.u32(g_color);
                        SendFrameTo(c, Frame(net::MSG_WELCOME, net::kHostPeerId, w.buf));
                        // tell the newcomer about everyone here, and everyone about the newcomer
                        struct Other { uint32_t id; std::string name; uint32_t color; std::string map; };
                        std::vector<Other> others;
                        { std::lock_guard<std::mutex> lock(g_mutex); for (auto& kv : g_conns) if (kv.first != c->peerId && !kv.second->name.empty()) others.push_back({ kv.first, kv.second->name, kv.second->color, kv.second->map }); }
                        for (auto& o : others) { net::Writer w2; w2.u32(o.id); w2.str(o.name); w2.u32(o.color); w2.str(o.map); SendFrameTo(c, Frame(net::MSG_PEER_JOINED, net::kHostPeerId, w2.buf)); }
                        { net::Writer w3; w3.u32(net::kHostPeerId); w3.str(g_name); w3.u32(g_color); w3.str(myMap); SendFrameTo(c, Frame(net::MSG_PEER_JOINED, net::kHostPeerId, w3.buf)); }
                        net::Writer w4; w4.u32(c->peerId); w4.str(c->name); w4.u32(c->color); w4.str(c->map);
                        BroadcastFrame(Frame(net::MSG_PEER_JOINED, net::kHostPeerId, w4.buf), c->peerId);
                        net::Message joined; joined.type = net::MSG_PEER_JOINED; joined.from = net::kHostPeerId; joined.payload = w4.buf;
                        Dispatch(joined);
                        SetStatus("host: " + std::to_string(g_conns.size()) + " peer(s) connected");
                        continue;
                    }
                    if (m.type == net::MSG_PEER_COLOR) { net::Reader r(m.payload); uint32_t color = r.u32(); if (r.ok) c->color = color; }   // for later joiners
                    if (m.type == net::MSG_MAP_INFO) { net::Reader r(m.payload); std::string map = r.str(); if (r.ok) c->map = map; }
                    // Asset offers and pulls, zone sync requests (zonesync.cpp) and direct link endpoints end at the host
                    if ((m.type < net::MSG_ASSET_MANIFEST || m.type > net::MSG_ASSET_CONVERTED) && (m.type < net::MSG_ZONE_REQUEST || m.type > net::MSG_ZONE_ANNOUNCE) &&
                        (m.type < net::MSG_DIRECT_OFFER || m.type > net::MSG_DIRECT_PUNCH))
                        BroadcastFrame(Frame(m.type, m.from, m.payload), c->peerId);
                }
                else if (m.type == net::MSG_BYE)
                {
                    LOG("net: the host ended the session");
                }
                else if (m.type == net::MSG_WELCOME)
                {
                    net::Reader r(m.payload);
                    g_localId = r.u32(); std::string hostName = r.str(); std::string map = r.str();
                    c->name = hostName;
                    g_connected = true;
                    SetStatus("client: connected as peer " + std::to_string(g_localId) + " to '" + hostName + "' (map '" + map + "')");
                }
                Dispatch(m);
            }
        }
        RemoveConn(c->peerId);
    }

    void AcceptLoop()
    {
        while (g_running)
        {
            sockaddr_in addr{}; int alen = sizeof addr;
            SOCKET s = accept(g_listen, reinterpret_cast<sockaddr*>(&addr), &alen);
            if (s == INVALID_SOCKET) break;
            BOOL nd = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof nd);
            auto c = std::make_shared<Conn>();
            c->s = s;
            char ip[64] = {}; inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof ip);
            c->ip = ip;
            { std::lock_guard<std::mutex> lock(g_mutex); c->peerId = g_nextPeerId++; g_conns[c->peerId] = c; }
            LOG("net: accepted %s as peer %u", ip, c->peerId);
            StartSender(c);
            c->thread = std::thread(ReadLoop, c);
            c->thread.detach();
        }
    }

    bool EnsureWsa()
    {
        if (g_wsa) return true;
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) { LOG("net: WSAStartup failed"); return false; }
        g_wsa = true;
        return true;
    }
}

namespace net
{
    bool StartHost(uint16_t port, const std::string& name, const std::string& mapName, const std::string& bindAddr)
    {
        if (g_running) Stop();
        if (!EnsureWsa()) return false;
        g_isHost = true; g_name = name; g_localId = kHostPeerId; g_nextPeerId = 2;
        { std::lock_guard<std::mutex> lock(g_mutex); g_mapName = mapName; }
        g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOL yes = TRUE; setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
        if (inet_pton(AF_INET, bindAddr.c_str(), &addr.sin_addr) != 1) addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(g_listen, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(g_listen, 8) != 0)
        {
            SetStatus("host: bind/listen on port " + std::to_string(port) + " failed (err " + std::to_string(WSAGetLastError()) + ")");
            closesocket(g_listen); g_listen = INVALID_SOCKET; return false;
        }
        g_running = true; g_connected = true;
        g_acceptThread = std::thread(AcceptLoop);
        SetStatus("host: listening on " + bindAddr + ":" + std::to_string(port));
        return true;
    }

    // Blocking part of a client connect (resolve + connect), INVALID_SOCKET after logging
    static SOCKET DialHost(const std::string& host, uint16_t port)
    {
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
        { SetStatus("client: cannot resolve '" + host + "'"); return INVALID_SOCKET; }
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        int rc = connect(s, res->ai_addr, int(res->ai_addrlen));
        freeaddrinfo(res);
        if (rc != 0) { SetStatus("client: connect to " + host + ":" + std::to_string(port) + " failed (err " + std::to_string(WSAGetLastError()) + ")"); closesocket(s); return INVALID_SOCKET; }
        return s;
    }

    // Adopt a connected socket as the host link and start the handshake.
    static void AdoptHostSocket(SOCKET s, const std::string& host, uint16_t port, const std::string& name, const std::string& mapName)
    {
        BOOL nd = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof nd);
        g_isHost = false; g_name = name; g_localId = 0;
        {
            sockaddr_in peer{}; int len = sizeof peer; char ip[64] = {};
            std::string addr = getpeername(s, reinterpret_cast<sockaddr*>(&peer), &len) == 0 && inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip) ? ip : host;
            std::lock_guard<std::mutex> lock(g_mutex); g_hostAddr = addr;
        }
        // the map may have loaded during the dial: a name given at dial time wins, an empty one keeps the current key
        std::string myMap;
        { std::lock_guard<std::mutex> lock(g_mutex); if (!mapName.empty()) g_mapName = mapName; myMap = g_mapName; }
        auto c = std::make_shared<Conn>(); c->s = s; c->peerId = kHostPeerId;
        { std::lock_guard<std::mutex> lock(g_mutex); g_conns.clear(); g_conns[kHostPeerId] = c; }
        g_running = true;
        StartSender(c);
        Writer w; w.u16(kProtocolVersion); w.str(name); w.str(myMap); w.u32(g_color);
        SendFrameTo(c, Frame(MSG_HELLO, 0, w.buf));
        c->thread = std::thread(ReadLoop, c);
        c->thread.detach();
        SetStatus("client: connected to " + host + ":" + std::to_string(port) + ", waiting for welcome");
    }

    bool Connect(const std::string& host, uint16_t port, const std::string& name, const std::string& mapName)
    {
        if (g_running) Stop();
        if (!EnsureWsa()) return false;
        g_isHost = false;
        SOCKET s = DialHost(host, port);
        if (s == INVALID_SOCKET) return false;
        AdoptHostSocket(s, host, port, name, mapName);
        return true;
    }

    void ConnectAsync(const std::string& host, uint16_t port, const std::string& name, const std::string& mapName)
    {
        if (g_running) Stop();
        if (!EnsureWsa()) return;
        if (g_connecting.exchange(true)) return;          // one dial at a time
        g_isHost = false;
        uint32_t gen = ++g_gen;
        SetStatus("client: connecting to " + host + ":" + std::to_string(port) + " ...");
        std::thread([host, port, name, mapName, gen]
        {
            SOCKET s = DialHost(host, port);
            bool stale = false;
            { std::lock_guard<std::mutex> lock(g_mutex); stale = gen != g_gen || g_running; }
            if (s != INVALID_SOCKET && stale) { closesocket(s); LOG("net: late connect to %s:%u discarded", host.c_str(), port); }
            else if (s != INVALID_SOCKET) AdoptHostSocket(s, host, port, name, mapName);
            g_connecting = false;
        }).detach();
    }

    bool IsConnecting() { return g_connecting; }

    void Stop()
    {
        ++g_gen;
        if (!g_running) return;
        if (g_isHost)
        {
            // tell the clients the session is over so they stop, give the senders a moment to flush
            std::vector<uint8_t> bye = Frame(MSG_BYE, kHostPeerId, {});
            BroadcastFrame(bye, 0);
            for (int i = 0; i < 50; ++i)
            {
                bool pending = false;
                { std::lock_guard<std::mutex> lock(g_mutex); for (auto& kv : g_conns) { std::lock_guard<std::mutex> lk(kv.second->qmutex); if (!kv.second->queue[0].empty() && kv.second->alive) pending = true; } }
                if (!pending) break;
                Sleep(10);
            }
        }
        g_running = false; g_connected = false;
        if (g_listen != INVALID_SOCKET) { closesocket(g_listen); g_listen = INVALID_SOCKET; }
        if (g_acceptThread.joinable()) g_acceptThread.join();
        std::vector<uint32_t> ids;
        { std::lock_guard<std::mutex> lock(g_mutex); for (auto& kv : g_conns) ids.push_back(kv.first); }
        for (uint32_t id : ids)
        {
            std::shared_ptr<Conn> c;
            { std::lock_guard<std::mutex> lock(g_mutex); auto it = g_conns.find(id); if (it != g_conns.end()) { c = it->second; g_conns.erase(it); } }
            if (c) { c->alive = false; c->qcv.notify_all(); }
            if (c && c->s != INVALID_SOCKET) { shutdown(c->s, SD_BOTH); closesocket(c->s); c->s = INVALID_SOCKET; }
        }
        SetStatus("off");
    }

    void Send(uint16_t type, const std::vector<uint8_t>& payload)
    {
        if (!g_running) return;
        BroadcastFrame(Frame(type, g_localId, payload), 0);
    }

    size_t QueuedBytes(uint32_t peerId, int tier)
    {
        std::vector<std::shared_ptr<Conn>> conns;
        { std::lock_guard<std::mutex> lock(g_mutex); for (auto& kv : g_conns) if (!peerId || kv.first == peerId || !g_isHost) conns.push_back(kv.second); }
        size_t most = 0;
        for (auto& c : conns) { std::lock_guard<std::mutex> lk(c->qmutex); size_t n = 0; for (int t = tier; t < 3; ++t) n += c->queuedBytes[t]; most = std::max(most, n); }
        return most;
    }

    void SendTo(uint32_t peerId, uint16_t type, const std::vector<uint8_t>& payload)
    {
        if (!g_running) return;
        std::shared_ptr<Conn> c;
        { std::lock_guard<std::mutex> lock(g_mutex); auto it = g_conns.find(peerId); if (it != g_conns.end()) c = it->second; }
        if (c) SendFrameTo(c, Frame(type, g_localId, payload));
    }

    void SetLocalColor(uint32_t argb)
    {
        if (g_color.exchange(argb) == argb) return;
        // client: the HELLO is already queued, this goes out after it
        if (g_running) { Writer w; w.u32(argb); Send(MSG_PEER_COLOR, w.buf); }
    }
    uint32_t LocalColor() { return g_color; }

    void SetLocalMap(const std::string& key)
    {
        { std::lock_guard<std::mutex> lock(g_mutex); if (g_mapName == key) return; g_mapName = key; }
        if (g_running && g_connected) { Writer w; w.str(key); Send(MSG_MAP_INFO, w.buf); LOG("net: announced map '%s'", key.c_str()); }
    }
    std::string LocalMap() { std::lock_guard<std::mutex> lock(g_mutex); return g_mapName; }

    void SetHandler(Handler h) { std::lock_guard<std::mutex> lock(g_mutex); g_handler = std::move(h); }
    bool IsRunning()   { return g_running; }
    bool IsHost()      { return g_isHost; }
    std::string HostAddress() { std::lock_guard<std::mutex> lock(g_mutex); return g_isHost ? std::string() : g_hostAddr; }
    bool IsConnected() { return g_connected; }
    std::string PeerAddresses()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_running) return std::string();
        if (!g_isHost) return g_connected ? g_hostAddr : std::string();
        std::string out;
        for (const auto& kv : g_conns)
            if (kv.second && kv.second->alive && !kv.second->ip.empty() && out.find(kv.second->ip) == std::string::npos)
                out += (out.empty() ? "" : ",") + kv.second->ip;
        return out;
    }
    uint32_t LocalPeerId() { return g_localId; }
    std::string Status() { std::lock_guard<std::mutex> lock(g_mutex); return g_status; }
}
