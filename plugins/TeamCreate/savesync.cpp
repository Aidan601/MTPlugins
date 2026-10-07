#include <windows.h>
#include <mutex>
#include <string>
#include <cwctype>
#include "savesync.h"
#include "compat.h"
#include "net.h"
#include "mapsync.h"
#include "presence.h"

// See savesync.h. Wire format: MSG_SAVE = str mapKey, str saverName (any -> all, the host relays, older peers ignore it).
namespace
{
    constexpr uint64_t kBroadcastDelayMs = 600;    // one capture pass (500 ms) so the last edits go out before the save request
    constexpr uint64_t kEchoGuardMs = 3000;        // a save we made for a peer must not be announced back
    constexpr uint64_t kSettleTimeoutMs = 10000;   // edits held back by our own selection: save anyway after this long

    std::mutex  g_mutex;
    // sender (main thread)
    bool        g_broadcastPending = false;
    uint64_t    g_broadcastAt = 0;
    std::string g_broadcastKey;
    // receiver
    bool        g_requestPending = false;          // under g_mutex (set on the net thread)
    std::string g_requestKey, g_requestName;       // under g_mutex
    uint64_t    g_requestTick = 0;                 // under g_mutex
    bool        g_savingForPeer = false;           // main thread: inside our own File > Save
    uint64_t    g_lastPeerSaveTick = 0;            // main thread
    unsigned    g_sent = 0, g_saved = 0, g_ignored = 0;
    std::string g_lastEvent;
    uint64_t    g_lastWaitLog = 0;

    bool SamePath(std::wstring a, std::wstring b)
    {
        for (auto* s : { &a, &b }) for (auto& c : *s) { c = wchar_t(towlower(c)); if (c == L'/') c = L'\\'; }
        return !a.empty() && a == b;
    }

    void Event(const std::string& text) { g_lastEvent = text; LOG("savesync: %s", text.c_str()); }
}

namespace savesync
{
    bool Enabled() { return Cfg().syncSaves; }

    void SetEnabled(bool on)
    {
        if (Cfg().syncSaves == on) return;
        Cfg().syncSaves = on;
        rp::ConfigSet("sync", "sync_saves", on ? "1" : "0");
        Event(on ? "sync saves ON: your saves are shared, and saves by mappers who have it on save your copy too" : "sync saves OFF");
        if (!on) { std::lock_guard<std::mutex> lock(g_mutex); g_requestPending = false; }
    }

    void OnLocalSave(const std::wstring& path)
    {
        if (!Enabled() || !net::IsConnected()) return;
        if (g_savingForPeer || GetTickCount64() - g_lastPeerSaveTick < kEchoGuardMs) return;   // our copy of a peer's save
        if (hooks::PrefabDepth() > 0) return;                                                 // a prefab file, not the shared map
        std::string key = mapsync::RootKey();
        if (key.empty() || !SamePath(path, hooks::CurrentMapPath())) return;                   // Save As elsewhere, an autosave copy
        g_broadcastPending = true;
        g_broadcastAt = GetTickCount64() + kBroadcastDelayMs;
        g_broadcastKey = key;
    }

    void OnMessage(const net::Message& m)
    {
        if (m.from == net::LocalPeerId()) return;
        net::Reader r(m.payload);
        std::string key = r.str(); std::string name = r.str();
        if (!r.ok) return;
        if (!Enabled())
        {
            ++g_ignored;
            LOG("savesync: %s saved '%s'; not saving here (sync saves is off on this PC)", name.c_str(), key.c_str());
            return;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_requestPending = true; g_requestKey = key; g_requestName = name; g_requestTick = GetTickCount64();
    }

    void Tick()
    {
        uint64_t now = GetTickCount64();
        if (g_broadcastPending && now >= g_broadcastAt)
        {
            g_broadcastPending = false;
            if (Enabled() && net::IsConnected())
            {
                net::Writer w; w.str(g_broadcastKey); w.str(presence::LocalName());
                net::Send(net::MSG_SAVE, w.buf);
                ++g_sent;
                Event("you saved '" + g_broadcastKey + "': asked the other mappers with sync saves on to save too");
            }
        }

        std::string key, name; uint64_t since;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_requestPending) return;
            key = g_requestKey; name = g_requestName; since = g_requestTick;
        }
        auto drop = [&](const std::string& why)
        {
            { std::lock_guard<std::mutex> lock(g_mutex); if (g_requestTick == since) g_requestPending = false; }
            ++g_ignored;
            Event(name + " saved '" + key + "'; not saved here: " + why);
        };
        if (!Enabled() || !net::IsConnected()) { drop("sync saves is off or the session ended"); return; }
        std::string ours = mapsync::RootKey();
        if (ours.empty() || !mapsync::SameMap(key, ours)) { drop("you have another map open ('" + ours + "')"); return; }
        if (presence::OnTempMap()) { drop("your map is a temporary copy of the host's; it is saved when the session ends"); return; }

        // wait for a moment where Radiant's File > Save writes the shared map with every edit that came before the request
        const char* wait = nullptr;
        if (hooks::PrefabDepth() > 0) wait = "you are inside a prefab (File > Save would save the prefab)";
        else if (hooks::IsReloading()) wait = "a reload is running";
        else if (hooks::MouseButtonHeld()) wait = "a mouse button is held";
        else if (!mapsync::Settled() && now - since < kSettleTimeoutMs) wait = "incoming edits are still being applied";
        if (wait)
        {
            if (now - g_lastWaitLog > 5000) { g_lastWaitLog = now; LOG("savesync: %s saved; waiting to save here: %s", name.c_str(), wait); }
            return;
        }
        { std::lock_guard<std::mutex> lock(g_mutex); if (g_requestTick == since) g_requestPending = false; }
        g_savingForPeer = true;
        int r = rp::host->TriggerMenuAction(rp::self, "File/Save");
        g_savingForPeer = false;
        g_lastPeerSaveTick = GetTickCount64();
        if (r == 1)
        {
            ++g_saved;
            Event("saved '" + ours + "' because " + name + " saved");
            presence::Notice("Map saved (" + name + " saved)");
        }
        else Event(std::string("could not run File > Save for ") + name + "'s save (" + (r == 0 ? "no such menu entry" : "entry disabled") + ")");
    }

    std::string Status()
    {
        bool pending; { std::lock_guard<std::mutex> lock(g_mutex); pending = g_requestPending; }
        return std::string("sync_saves=") + (Enabled() ? "1" : "0") + " sent=" + std::to_string(g_sent) + " saved_for_peers=" + std::to_string(g_saved) +
            " ignored=" + std::to_string(g_ignored) + (pending ? " (a peer's save is waiting)" : "") + (g_lastEvent.empty() ? "" : "\nlast: " + g_lastEvent);
    }
}
