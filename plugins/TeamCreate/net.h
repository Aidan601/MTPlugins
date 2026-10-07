#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

// Team Create transport: TCP, star topology. One Radiant hosts (listens), the others connect by IP, the host relays every client message to the other clients.
// Wire frame:  u32 payloadLength | u16 type | u32 fromPeer | payload (the host fills in fromPeer, clients send 0, peer 1 is always the host).
namespace net
{
    enum MsgType : uint16_t
    {
        // The trailing u32 color (ARGB, 0 = automatic) on HELLO, WELCOME and PEER_JOINED reads as 0 when missing.
        // `map` is the scope key of the sender's root map ("zm/zm_escape.map", see mapsync, empty = none loaded).
        MSG_HELLO        = 1,   // client -> host: u16 protocol, str name, str map, u32 color
        MSG_WELCOME      = 2,   // host -> client: u32 yourPeerId, str hostName, str map, u32 hostColor
        MSG_PEER_JOINED  = 3,   // host -> all:    u32 peerId, str name, u32 color, str map
        MSG_PEER_LEFT    = 4,   // host -> all:    u32 peerId
        MSG_BYE          = 5,   // host -> all:    (empty) the host ended the session, clients stop instead of reconnecting
        MSG_CAMERA       = 10,  // any -> all:     f32 origin[3], f32 angles[3]
        MSG_SELECTION    = 11,  // any -> all:     u16 count, str guid...
        MSG_CHAT         = 12,  // any -> all:     str text
        MSG_PEER_COLOR   = 13,  // any -> all:     u32 color (0 = automatic)
        MSG_MAP_INFO     = 14,  // any -> all:     str map (the sender's root map changed)
        MSG_MAP_REQUEST  = 15,  // client -> host: str map (client has no copy: the host answers with MSG_FULL_SYNC)
        // Prefab mirror (prefabs.cpp)
        MSG_INVENTORY_REQUEST = 16,  // client -> host: str map
        MSG_INVENTORY         = 17,  // host -> client: str map, u16 count, { str key, u32 size, str hashHex, u8 missing }... (every prefab the map uses, nested included)
        MSG_FILE_REQUEST      = 18,  // client -> host: u16 count, str key... (answered with MSG_FULL_SYNC per prefab file)
        // Object sync, see mapsync.cpp. `scope` is the file, relative to map_source, '/' separators, lower case.
        MSG_ENTITY_UPSERT = 20,  // str scope, str entityGuid, u32 order, blob headerText
        MSG_BRUSH_UPSERT  = 21,  // str scope, str entityGuid, str brushGuid, blob brushText
        MSG_DELETE        = 22,  // str scope, u8 kind (0 entity, 1 brush), str guid
        MSG_LIGHT_BUILD   = 27,  // any -> all: str map, str builderName, u8 rebuildSun, u8 rebuildProbes (lightsync.cpp)
        MSG_SCOPE         = 26,  // any -> all: u8 depth, str prefabKey x depth (where the sender edits: the prefab chain from the root map, empty = the map)
        MSG_SAVE          = 25,  // any -> all: str map, str saverName (savesync.cpp)
        MSG_FULL_SYNC     = 23,  // str scope, blob mapText (host -> joining client, root map + the prefab being edited)
        // Diagnostics sharing, see diag.cpp
        MSG_LOG_LINES     = 30,  // blob text: whole lines of the sender's RadiantPlugins.log
        MSG_FILE          = 31,  // str kind (dump|prevlog), str name, u32 mtimeLo, u32 mtimeHi, blob data
        MSG_FILE_PART     = 32,  // str kind, str name, u32 mtimeLo, u32 mtimeHi, u32 total, u32 offset, blob data (256 KB), a dump in parts
        // MSG_LOG_LINES trailer: u64 sourceToken, u64 fileOffset of the blob's first byte
        MSG_ASSET_MANIFEST = 40,  // str map, asset metadata: client offer / host authoritative revision
        MSG_ASSET_REQUEST  = 41,  // str map, str id, str revision, u16 file, u32 offset
        MSG_ASSET_CHUNK    = 42,  // request fields followed by blob bytes (256 KB maximum)
        MSG_ASSET_CHUNK_Z  = 43,  // request fields, u32 raw size, blob XPRESS-Huffman compressed bytes
        MSG_ASSET_REQUEST_Z = 44, // as MSG_ASSET_REQUEST from a peer that accepts MSG_ASSET_CHUNK_Z replies
        MSG_ASSET_CAPS     = 45,  // str map, u8 caps (bit 0: sends and accepts compressed chunks)
        MSG_ASSET_CONVERTED = 46, // str map, str id, str revision, u16 n, n x (str ToolsGfx path, str hash, u32 size): Radiant's converted files for the asset
                                  // requested as MSG_ASSET_REQUEST(_Z) with file 0x8000|k and a trailing str path, chunks echo that file index
        // Zone sync (zonesync.cpp): a client fetches the host's built usermap for Play Test's Join Host, not relayed
        MSG_ZONE_REQUEST   = 50,  // client -> host: str map
        MSG_ZONE_MANIFEST  = 51,  // host -> client: str map, u8 ok, str error, u16 n, n x { str rel, u64 size, u64 writeTime } (u64 = two u32, low first)
        MSG_ZONE_GET       = 52,  // client -> host: str map, str rel, u64 writeTime, u64 offset, u32 length (1 MB at most)
        MSG_ZONE_CHUNK     = 53,  // host -> client: str map, str rel, u8 status (0 ok), u64 offset, blob data
        MSG_ZONE_REQUEST2  = 54,  // client -> host: str map (50/51: whole files, no hashes)
        MSG_ZONE_MANIFEST2 = 55,  // host -> client: str map, u8 ok, str error, u32 blockSize, u16 n, n x { str rel, u64 size, u64 writeTime, blob 16-byte SHA-256 prefix per block }
        MSG_ZONE_ANNOUNCE  = 56,  // host -> clients: str map (a new build is complete)
        MSG_PLAY_STATE     = 57,  // host -> clients: str map, u8 playing (the host's Play Test game is up)
        // Appended fields: MSG_ZONE_GET u8 flags (bit 0: status 2 = XPRESS-Huffman blob accepted), MSG_ZONE_REQUEST2 u8 caps (bit 0: dials a direct link),
        // MSG_ZONE_MANIFEST2 u8 busy (the host is linking: ask again). MSG_ZONE_CHUNK status: 0 raw, 1 the file changed, 2 compressed.
        // Direct link (direct.cpp): bulk blocks straight between two PCs, beside the session. Not relayed.
        MSG_DIRECT_OFFER   = 58,  // host -> client: str ip, u16 port, blob token, u8 mode (0 = a port the router opened, 1 = the listener's public endpoint for hole punching)
        MSG_DIRECT_HELLO   = 59,  // first frame on a direct link: blob token, answered with u8 1. Then MSG_ZONE_GET and MSG_ZONE_CHUNK as on the session
        MSG_DIRECT_PUNCH   = 60,  // client -> host: str ip, u16 port (the public endpoint the client dials from, the host dials it too)
    };

    constexpr uint16_t kProtocolVersion = 2;   // 2: scope field on the sync messages
    constexpr uint32_t kHostPeerId = 1;

    struct Message
    {
        uint16_t             type = 0;
        uint32_t             from = 0;
        std::vector<uint8_t> payload;
    };

    // Helpers for building and reading payloads
    struct Writer
    {
        std::vector<uint8_t> buf;
        void u8(uint8_t v)          { buf.push_back(v); }
        void u16(uint16_t v)        { buf.push_back(uint8_t(v)); buf.push_back(uint8_t(v >> 8)); }
        void u32(uint32_t v)        { for (int i = 0; i < 4; ++i) buf.push_back(uint8_t(v >> (8 * i))); }
        void f32(float v)           { uint32_t u; memcpy(&u, &v, 4); u32(u); }
        void str(const std::string& s) { u16(uint16_t(s.size())); buf.insert(buf.end(), s.begin(), s.end()); }
        void blob(const std::string& s) { u32(uint32_t(s.size())); buf.insert(buf.end(), s.begin(), s.end()); }   // no 64 KB limit
    };
    struct Reader
    {
        const std::vector<uint8_t>& buf; size_t pos = 0; bool ok = true;
        explicit Reader(const std::vector<uint8_t>& b) : buf(b) {}
        bool     need(size_t n)     { if (pos + n > buf.size()) { ok = false; return false; } return true; }
        uint8_t  u8()               { if (!need(1)) return 0; return buf[pos++]; }
        uint16_t u16()              { if (!need(2)) return 0; uint16_t v = buf[pos] | (buf[pos + 1] << 8); pos += 2; return v; }
        uint32_t u32()              { if (!need(4)) return 0; uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(buf[pos + i]) << (8 * i); pos += 4; return v; }
        float    f32()              { uint32_t u = u32(); float f; memcpy(&f, &u, 4); return f; }
        std::string str()           { uint16_t n = u16(); if (!need(n)) return {}; std::string s(buf.begin() + pos, buf.begin() + pos + n); pos += n; return s; }
        std::string blob()          { uint32_t n = u32(); if (!need(n)) return {}; std::string s(buf.begin() + pos, buf.begin() + pos + n); pos += n; return s; }
    };

    using Handler = std::function<void(const Message&)>;   // called on the network thread

    bool StartHost(uint16_t port, const std::string& name, const std::string& mapName, const std::string& bindAddr = "0.0.0.0");
    bool Connect(const std::string& host, uint16_t port, const std::string& name, const std::string& mapName);
    // Connects on a worker thread (a dead address blocks ~20 s), dropped if Stop() or another Connect follows
    void ConnectAsync(const std::string& host, uint16_t port, const std::string& name, const std::string& mapName);
    bool IsConnecting();
    void Stop();

    // Send to everyone else (host: all clients, client: the host, which relays)
    void Send(uint16_t type, const std::vector<uint8_t>& payload);
    // Host only: send to one client.
    void SendTo(uint32_t peerId, uint16_t type, const std::vector<uint8_t>& payload);
    // Bytes waiting in the send queues of tier `tier` and above (0 all, 1 asset chunks + diagnostics, 2 diagnostics)
    // for that peer's connection, peerId 0 = the fullest connection. A client has one connection, the host's.
    size_t QueuedBytes(uint32_t peerId, int tier);
    void SetHandler(Handler h);
    // Camera colour this side announces (ARGB, 0 = automatic), any thread
    void SetLocalColor(uint32_t argb);
    uint32_t LocalColor();
    // Root map this side has open (scope key), any thread
    void SetLocalMap(const std::string& key);
    std::string LocalMap();

    bool     IsRunning();
    bool     IsHost();
    bool     IsConnected();    // client: handshake complete, host: listening
    std::string HostAddress(); // client: the host's IPv4 as connected, empty for the host or never connected
    std::string PeerAddresses();   // IPv4s this side plays with, comma separated (host: clients, client: the host)
    uint32_t LocalPeerId();
    std::string Status();      // one line for the UI/log
}
