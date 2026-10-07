#include <windows.h>
#include <mutex>
#include <string>
#include "lightsync.h"
#include "compat.h"
#include "net.h"
#include "mapsync.h"
#include "presence.h"

// See lightsync.h. MSG_LIGHT_BUILD = str mapKey, str builderName, u8 rebuildSun, u8 rebuildProbes (the host relays)
namespace
{
    // Radiant addresses (resolved through host->RadiantAddress)
    constexpr uintptr_t kAddr_g_pMainWin = 0x1444C6560;   // RadiantMainWin*: +64 -> QE children, +240 -> CCamWnd*
    constexpr uintptr_t kAddr_CamBuildLighting = 0x1402F31D0;   // void (CCamWnd*, bool rebuildSun, bool rebuildProbes)
    constexpr size_t off_MainWin_QEChildren = 64;
    constexpr size_t off_QEChildren_CamWnd = 240;
    constexpr size_t off_Cam_ViewMode = 232;          // int: 4 = real lighting (bake only steps in this mode)
    constexpr size_t off_Cam_LightState = 3960;       // int: active light state index, -1 = none
    constexpr size_t off_Cam_Volume = 6456;           // LightingVolume* the camera is in (null = none)
    constexpr size_t off_Cam_StateNoVolume = 6500;    // int: bake state when there is no volume or light state
    constexpr size_t off_Cam_Paused = 6656;           // bool
    constexpr size_t off_Volume_State = 1164;         // int[4]: 0 no bake state, 1 unbuilt, 3..6 baking, 7/8 built

    constexpr uint64_t kBroadcastDelayMs = 600;    // one capture pass (500 ms), last edits go out first
    constexpr uint64_t kSettleTimeoutMs = 10000;   // build anyway after this long

    using BuildFn = void (__fastcall*)(void* cam, uint8_t rebuildSun, uint8_t rebuildProbes);
    BuildFn     g_original = nullptr;
    BuildFn     g_target = nullptr;                // the hooked entry (calls land in hk_Build)
    void**      g_mainWin = nullptr;
    bool        g_available = false;
    bool        g_buildingForPeer = false;         // main thread: inside our own call of the build function

    std::mutex  g_mutex;
    // sender (main thread)
    bool        g_broadcastPending = false;
    uint64_t    g_broadcastAt = 0;
    uint8_t     g_broadcastSun = 0, g_broadcastProbes = 0;
    // receiver
    bool        g_requestPending = false;          // under g_mutex (set on the net thread)
    std::string g_requestKey, g_requestName;       // under g_mutex
    uint8_t     g_requestSun = 0, g_requestProbes = 0;   // under g_mutex
    uint64_t    g_requestTick = 0;                 // under g_mutex
    unsigned    g_sent = 0, g_built = 0, g_ignored = 0;
    std::string g_lastEvent;
    uint64_t    g_lastWaitLog = 0;

    void Event(const std::string& text) { g_lastEvent = text; LOG("lightsync: %s", text.c_str()); }

    std::string Kind(uint8_t sun, uint8_t probes)
    {
        if (sun && probes) return "Rebuild All";
        if (sun) return "Rebuild Sun";
        if (probes) return "Rebuild Probes";
        return "build lighting";
    }

    void* CamWnd()
    {
        if (!g_mainWin || !*g_mainWin) return nullptr;
        void* qe = *static_cast<void**>(static_cast<void*>(static_cast<char*>(*g_mainWin) + off_MainWin_QEChildren));
        if (!qe) return nullptr;
        return *static_cast<void**>(static_cast<void*>(static_cast<char*>(qe) + off_QEChildren_CamWnd));
    }

    template <class T> T Field(void* obj, size_t off) { T v; memcpy(&v, static_cast<char*>(obj) + off, sizeof v); return v; }

    // bake state of the active light state of the volume the camera is in
    int BakeState(void* cam)
    {
        void* volume = Field<void*>(cam, off_Cam_Volume);
        int index = Field<int>(cam, off_Cam_LightState);
        if (!volume || index < 0 || index > 3) return Field<int>(cam, off_Cam_StateNoVolume);
        return Field<int>(volume, off_Volume_State + 4 * size_t(index));
    }
    bool Paused(void* cam) { return Field<uint8_t>(cam, off_Cam_Paused) != 0; }
    bool Baking(int state) { return unsigned(state - 3) <= 3; }

    // nothing without a bake state, rebuild for Sun/Probes, else resume when paused, pause when baking, or incremental build
    bool StartsBuild(void* cam, uint8_t sun, uint8_t probes)
    {
        int state = BakeState(cam);
        if (state == 0) return false;
        if (sun || probes) return true;
        return !Paused(cam) && !Baking(state);
    }

    void __fastcall hk_Build(void* cam, uint8_t sun, uint8_t probes)
    {
        bool share = false;
        if (!g_buildingForPeer && cam && lightsync::Enabled() && net::IsConnected())
            share = StartsBuild(cam, sun, probes);
        g_original(cam, sun, probes);
        if (!share) return;
        if (hooks::PrefabDepth() > 0) { Event("you built lighting inside a prefab; not shared"); return; }
        g_broadcastPending = true;
        g_broadcastAt = GetTickCount64() + kBroadcastDelayMs;
        g_broadcastSun = sun; g_broadcastProbes = probes;
    }
}

namespace lightsync
{
    void Install()
    {
        if (!rp::Has(&rp::host->Detour) || !rp::host->Detour || !rp::host->RadiantAddress) { LOG("lightsync: host has no detours; sync light builds unavailable"); return; }
        g_mainWin = static_cast<void**>(rp::host->RadiantAddress(rp::self, kAddr_g_pMainWin));
        void* target = rp::host->RadiantAddress(rp::self, kAddr_CamBuildLighting);
        if (!g_mainWin || !target) { LOG("lightsync: Radiant build not recognised; sync light builds unavailable"); return; }
        void* original = nullptr;
        if (!rp::host->Detour(rp::self, target, reinterpret_cast<void*>(&hk_Build), &original) || !original)
        { LOG("lightsync: detour on the lighting build failed; sync light builds unavailable"); return; }
        g_original = static_cast<BuildFn>(original);
        g_target = static_cast<BuildFn>(target);
        g_available = true;
    }

    bool Available() { return g_available; }
    bool Enabled() { return Cfg().syncLights; }

    void SetEnabled(bool on)
    {
        if (Cfg().syncLights == on) return;
        Cfg().syncLights = on;
        rp::ConfigSet("sync", "sync_lights", on ? "1" : "0");
        Event(on ? "sync light builds ON: your light builds are shared, and builds by mappers who have it on build here too" : "sync light builds OFF");
        if (!on) { std::lock_guard<std::mutex> lock(g_mutex); g_requestPending = false; }
    }

    void OnMessage(const net::Message& m)
    {
        if (m.from == net::LocalPeerId()) return;
        net::Reader r(m.payload);
        std::string key = r.str(); std::string name = r.str();
        uint8_t sun = r.u8(), probes = r.u8();
        if (!r.ok) return;
        if (!Enabled())
        {
            ++g_ignored;
            LOG("lightsync: %s built lighting on '%s'; not building here (sync light builds is off on this PC)", name.c_str(), key.c_str());
            return;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_requestPending = true; g_requestKey = key; g_requestName = name; g_requestSun = sun; g_requestProbes = probes; g_requestTick = GetTickCount64();
    }

    void Tick()
    {
        uint64_t now = GetTickCount64();
        if (g_broadcastPending && now >= g_broadcastAt)
        {
            g_broadcastPending = false;
            std::string key = mapsync::RootKey();
            if (Enabled() && net::IsConnected() && !key.empty())
            {
                net::Writer w; w.str(key); w.str(presence::LocalName()); w.u8(g_broadcastSun); w.u8(g_broadcastProbes);
                net::Send(net::MSG_LIGHT_BUILD, w.buf);
                ++g_sent;
                Event("you started " + Kind(g_broadcastSun, g_broadcastProbes) + " on '" + key + "': asked the other mappers with sync light builds on to build too");
            }
        }

        std::string key, name; uint8_t sun, probes; uint64_t since;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_requestPending) return;
            key = g_requestKey; name = g_requestName; sun = g_requestSun; probes = g_requestProbes; since = g_requestTick;
        }
        auto finish = [&] { std::lock_guard<std::mutex> lock(g_mutex); if (g_requestTick == since) g_requestPending = false; };
        auto drop = [&](const std::string& why, bool notice)
        {
            finish();
            ++g_ignored;
            Event(name + " started " + Kind(sun, probes) + "; not built here: " + why);
            if (notice) presence::Notice(name + " built lighting; not built here: " + why);
        };
        if (!Enabled() || !net::IsConnected()) { drop("sync light builds is off or the session ended", false); return; }
        if (!g_available) { drop("this Radiant build is not supported", true); return; }
        std::string ours = mapsync::RootKey();
        if (ours.empty() || !mapsync::SameMap(key, ours)) { drop("you have another map open", true); return; }
        if (hooks::PrefabDepth() > 0) { drop("you are inside a prefab", true); return; }

        // let earlier edits land first so both PCs bake the same geometry
        const char* wait = nullptr;
        if (hooks::IsReloading()) wait = "a reload is running";
        else if (hooks::MouseButtonHeld()) wait = "a mouse button is held";
        else if (!mapsync::Settled() && now - since < kSettleTimeoutMs) wait = "incoming edits are still being applied";
        if (wait)
        {
            if (now - g_lastWaitLog > 5000) { g_lastWaitLog = now; LOG("lightsync: %s built lighting; waiting to build here: %s", name.c_str(), wait); }
            return;
        }

        void* cam = CamWnd();
        if (!cam) { drop("no 3D view", false); return; }
        int state = BakeState(cam);
        if (Field<int>(cam, off_Cam_ViewMode) != 4) { drop("real lighting is off in your 3D view", true); return; }
        if (state == 0) { drop("no bake state here (no sun volume, or a no-bake light state)", true); return; }
        if (!sun && !probes && !Paused(cam) && Baking(state)) { drop("a light build is already running", false); return; }

        finish();
        g_buildingForPeer = true;
        g_original(cam, sun, probes);
        g_buildingForPeer = false;
        ++g_built;
        Event("started " + Kind(sun, probes) + " because " + name + " did");
        presence::Notice("Building lighting (" + name + " built)");
    }

    std::string BuildLikeBolt(bool rebuildSun, bool rebuildProbes)
    {
        if (!g_available) return "unavailable";
        void* cam = CamWnd();
        if (!cam) return "no 3D view";
        int before = BakeState(cam);
        g_target(cam, rebuildSun ? 1 : 0, rebuildProbes ? 1 : 0);
        return "state " + std::to_string(before) + " -> " + std::to_string(BakeState(cam)) + (Paused(cam) ? " (paused)" : "") + (g_broadcastPending ? ", broadcast pending" : ", not shared");
    }

    std::string Status()
    {
        bool pending; { std::lock_guard<std::mutex> lock(g_mutex); pending = g_requestPending; }
        std::string s = std::string("sync_lights=") + (Enabled() ? "1" : "0") + " available=" + (g_available ? "1" : "0") +
            " sent=" + std::to_string(g_sent) + " built_for_peers=" + std::to_string(g_built) + " ignored=" + std::to_string(g_ignored) +
            (pending ? " (a peer's build is waiting)" : "");
        if (void* cam = g_available ? CamWnd() : nullptr)
            s += " state=" + std::to_string(BakeState(cam)) + " paused=" + (Paused(cam) ? "1" : "0") + " view_mode=" + std::to_string(Field<int>(cam, off_Cam_ViewMode));
        return s + (g_lastEvent.empty() ? "" : "\nlast: " + g_lastEvent);
    }
}
