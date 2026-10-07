#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <set>

// Team Create presence: who is connected, where their camera is, what they have selected.
// Camera samples are taken on Radiant's main thread (Tick, called from the RoutineProcessing
// detour) and broadcast over net, incoming messages update the peer table on the net thread.
namespace presence
{
    // Placement of an entered prefab instance in the scope outside it: the misc_prefab's "origin" and "angles"
    // (pitch yaw roll, degrees). Inside a prefab Radiant shows the prefab file's own space (its origin at 0 0 0,
    // unrotated), so a peer's camera and selection there are in that space, not the map's.
    struct PrefabXf { float origin[3] = { 0, 0, 0 }; float angles[3] = { 0, 0, 0 }; };
    // A rigid transform: out = m * in + t.
    struct Xform { float m[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }; float t[3] = { 0, 0, 0 }; };

    struct Peer
    {
        uint32_t    id = 0;
        std::string name;
        float       origin[3] = { 0, 0, 0 };     // latest sample
        float       angles[3] = { 0, 0, 0 };     // pitch, yaw, roll (degrees, Radiant convention)
        float       prevOrigin[3] = { 0, 0, 0 }; // previous sample, for interpolation on the drawing side
        float       prevAngles[3] = { 0, 0, 0 };
        uint64_t    sampleTick = 0;              // when `origin/angles` arrived
        uint64_t    prevSampleTick = 0;
        bool        hasCamera = false;
        uint64_t    lastSeenTick = 0;
        struct SelBox { std::string brushGuid, entityGuid; float mins[3], maxs[3]; std::vector<float> planes; };   // planes: 9 floats per face when the brush changed shape
        std::vector<SelBox> selection;           // brushes they hold selected (GUID + bounds, so we can draw without a lookup)
        uint32_t    selectionSeq = 0;            // bumped per selection message, the live-move tick applies each new one once
        uint32_t    liveMoveSeq = 0;             // the last selectionSeq the live-move tick handled
        uint32_t    liveMoveRetry = 0;           // ticks left to re-apply the last selection while a brush is missing here
        uint32_t    color = 0xFFFFFFFF;          // ARGB: the colour the peer picked, else the palette colour of its id
        std::string mapKey;                      // the root map they have open (scope key), empty = not loaded yet
        bool        scopeKnown = false;          // they sent MSG_SCOPE
        std::vector<std::string> prefabChain;    // the prefabs they are inside, outermost first (scope keys), empty = the map itself
        std::vector<PrefabXf>    prefabXf;       // per chain level, the instance's placement in the outer scope (shorter than the chain = identity)
    };

    // Soft locks: objects you have selected that a peer also holds selected (computed every 100 ms
    // in TickSelection, from the peers' selection messages and Radiant's own selection list).
    struct Conflict { std::string brushGuid; float mins[3], maxs[3]; uint32_t peerId; std::string peerName; uint32_t color; };
    std::vector<Conflict> Conflicts();          // any thread
    // Live peer drags (main thread, from Tick): what the tick did with the peers' selection bounds so far.
    std::string LiveMoveStatus();
    // Brush and entity GUIDs some peer holds selected right now (any thread). While a peer drags an object our copy
    // of it is moved live and differs from the sync model, the capture must not treat that as a local edit.
    std::set<std::string> HeldByPeers();

    // Chat (MSG_CHAT): plain text, host relays, kept as a short history for the dialog and the 3D view.
    struct ChatLine { uint64_t tick; uint32_t from; std::string name; std::string text; };
    void SendChat(const std::string& text);      // any thread
    std::vector<ChatLine> ChatHistory(size_t maxLines = 50);
    void Notice(const std::string& text);        // a local line in the chat (pane and 3D view) from "Team Create", not sent

    void Start();                 // starts hosting/connecting per config ([net] role), or nothing when role=off
    bool StartHost(uint16_t port, const std::string& name, const std::string& bindAddr);   // any thread, replaces a running session
    bool StartClient(const std::string& host, uint16_t port, const std::string& name);
    void Stop();
    bool IsActive();              // a session was started (hosting, connected or reconnecting)
    std::string EndReason();      // why the last session ended on its own ("The host ended the session."), empty otherwise
    std::string SessionText(bool hideAddress = false);    // one line for the UI ("Hosting on port 27017"), hideAddress: "the host" instead of the address
    void Tick();                  // main thread, every RoutineProcessing: sample + broadcast camera at ~10 Hz
    std::vector<Peer> Snapshot(); // copy of the peer table (any thread)

    // Map match (main thread). The host's map is compared with ours at the handshake and whenever either side loads another map (MSG_MAP_INFO).
    // A client on another map is asked once whether to open the host's (ReloadMap, retried until Radiant can), until the maps match its sync is off (cameras and chat keep working).
    // The host only warns about peers elsewhere and re-sends the full map to a client that switched to its map.
    void OnMapLoaded();           // after mapsync::OnMapLoaded: re-check, then announce the new map to the peers
    void OnMapSaved(const wchar_t* path);   // Radiant wrote a map (Save / Save As): remembered to tell a Save As apart from a cancelled one
    std::string MapStatus();      // "map" command: keys, blocked state, temporary copy, every peer's map
    // Temporary copy of the host's map (a client that had no file at that path got the map from the host and opened
    // it from map_source\_teamcreate_temp\...). When the session ends or Radiant closes while it is open, the user is
    // asked to save it on this PC (Save Map As) or to discard it (the file is deleted, an empty map is opened).
    void InstallCloseWatch();     // OnLoad: event filter on Radiant's main window for the close (QEvent::Close)
    bool LeaveTempMap(const char* reason, bool closing);   // main thread, closing = Radiant is closing (never blocked, a cancelled Save As leaves the file on disk). false = the map stays open
    bool OnTempMap();             // the open map is such a copy
    // Camera pose to draw right now: glides from the previous sample to the latest one over the
    // observed sample interval, so 10-20 Hz updates look continuous.
    void InterpolatedPose(const Peer& p, float origin[3], float angles[3]);
    constexpr unsigned kSendIntervalMs = 50;   // 20 Hz camera updates
    std::string LocalName();

    // Where each mapper is editing (MSG_SCOPE): the prefab chain from the root map. Sent when it changes, when a peer
    // joins or the link comes up, and every 15 s. OnScopeChanged: main thread, from the Map_EnterPrefab/LeavePrefab events.
    void OnScopeChanged(const std::wstring& fullPath, int depth);
    std::vector<std::string> LocalPrefabChain();
    std::vector<PrefabXf>    LocalPrefabXf();       // one per level of LocalPrefabChain (identity when the host cannot tell)
    // The transform from the space a peer's camera/selection is in (its prefab chain) into the space this Radiant
    // shows (ours). false = the same space, nothing to do. The common leading part of both chains is skipped (the same
    // prefab file is the same shared instance content, wherever each side entered it from).
    bool PeerToLocal(const Peer& p, Xform& out);
    void XfPoint(const Xform& x, const float in[3], float out[3]);
    void XfCameraAngles(const Xform& x, const float in[3], float out[3]);   // camera angles (Radiant: pitch up positive, yaw, roll) through the rotation part
    void XfBox(const Xform& x, const float mins[3], const float maxs[3], float outMins[3], float outMaxs[3]);   // bounds of the transformed box
    std::string PrefabChainText(const std::vector<std::string>& chain, bool fileNamesOnly);   // "a.map > b.map", "" for the map itself

    // Camera colours. Each mapper picks theirs ([net] color in TeamCreate.ini, the swatches in the pane),
    // 0 = automatic, which is the palette colour of the peer id, so every participant resolves it the same.
    constexpr uint32_t kPalette[] = { 0xFF00E5FF, 0xFFFF9800, 0xFF76FF03, 0xFFE040FB, 0xFFFFEB3B, 0xFFFF5252, 0xFF40C4FF, 0xFFB2FF59 };
    uint32_t AutoColor(uint32_t peerId);
    uint32_t LocalColor();                       // what the others see for us right now (resolved, never 0)
    uint32_t ChosenColor();                      // 0 = automatic
    void SetLocalColor(uint32_t argb);           // main thread, 0 = automatic. Saves the ini and tells the peers
    uint32_t ParseColor(const std::string& s);   // "#RRGGBB", "RRGGBB" -> 0xFFRRGGBB, "", "auto" or junk -> 0
    std::string ColorHex(uint32_t argb);         // "#RRGGBB"
}
