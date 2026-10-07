#pragma once
#include <string>
#include <cstdint>
#include <functional>
#include <vector>
#include "prefabs.h"
#include <vector>

namespace net { struct Message; }
namespace hooks { struct SelectedBrush; }

// Object sync (text model per scope, in-place apply, reload fallback). Every participant keeps the open map as GUID-keyed text blocks (mapmodel): local edits are
// captured on the main thread by walking Radiant's entity/brush lists and the changed blocks broadcast (the host relays), remote blocks are merged into the model,
// then applied in place or, once the user is idle for apply_delay_ms, written and reloaded.
namespace mapsync
{
    void Init();
    void OnMapLoaded(const std::wstring& fullPath);   // main thread (hooks::OnMapLoaded): the root map
    // Prefab navigation (main thread, from the Map_EnterPrefab/Map_LeavePrefab detours)
    void BeforeScopeChange();                         // commit the selection before Radiant swaps the entity list
    void OnScopeChanged(const std::wstring& fullPath, int prefabDepth);   // the file Radiant edits now
    void OnScopeSaved();                              // the user saved the prefab file (Ctrl+S inside a prefab)
    void Tick();                                      // main thread (RoutineProcessing detour)
    // Main thread, selection changed: objects the model does not know yet (a brush being dragged out, a new entity)
    // are serialized now and sent as upserts so peers see them while shaped. The commit capture sends the final text.
    void AnnounceSelectedNew();
    // Main thread: no incoming edit is queued, deferred or waiting for a reload
    bool Settled();
    // Main thread: GUIDs the user has selected (presence's 100 ms poll). Deleting a selected object is always broadcast,
    // only unexplained mass deletions are held back as a suspected diff error
    void NoteSelected(const std::vector<hooks::SelectedBrush>& sel);
    std::string DeletesCommand(const std::string& arg);   // "TeamCreate deletes [send]"
    void OnSessionStarted(bool host);                 // any thread: a client holds its capture until the full sync arrived
    void OnMessage(const net::Message& m);            // net thread
    void OnPeerJoined(uint32_t peerId);               // net thread, host sends the full map
    std::string Status();
    std::string Warning();   // non-empty when edits cannot be shared (different map open), cleared on map load
    void Warn(const std::string& text);   // set that line ("" clears)

    // Map identity, for the handshake check in presence.cpp
    std::string  KeyOfPath(const std::wstring& fullPath);      // the scope key of any file ("_prefabs/zm/x.map")
    std::string  RootKey();                                    // scope key of the open root map ("" = none)
    bool         SameMap(const std::string& a, const std::string& b);   // root maps match by file name, prefabs by path
    std::wstring PathForKey(const std::string& key);          // where that map would live on this PC ("" when no root is known)
    // Client whose map is not the host's: nothing is captured, sent or applied until it is (remote ops arriving
    // meanwhile are dropped, the full sync after the switch is authoritative). Main thread.
    void SetBlocked(bool blocked);
    bool Blocked();
    // Client with no copy of the host's map: the next MSG_FULL_SYNC for `key` is written to `path` (folders created),
    // then `done(ok)` runs on the main thread. An empty key cancels.
    void SetTempTarget(const std::string& key, const std::wstring& path, std::function<void(bool ok)> done);
    void OnMapClosed();   // the map was replaced by an empty one: no root map until the next load
    // Prefab mirror (prefabs.cpp). Client: the next MSG_FULL_SYNCs for `keys` are written to the mirror, then `done()`
    // runs on the main thread (empty keys = cancel). Host: the map's prefab inventory and one prefab file for a peer.
    void ExpectFiles(const std::vector<std::string>& keys, std::function<void()> done);
    std::vector<prefabs::Item> HostInventory();
    std::string AssetText(); // main thread: snapshot of all scope models for the asset worker
    void SendPrefabFile(uint32_t peerId, const std::string& key);
}
