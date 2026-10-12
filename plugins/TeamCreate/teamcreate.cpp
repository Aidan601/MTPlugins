// TeamCreate plugin: several mappers in the same map at once (presence, selection sharing, object sync, chat).
// Plugin entry point and glue between the host events and the modules (presence, mapsync, net, pane).
#define MODTOOLS_PLUGIN_IMPL
#include "compat.h"
#include "presence.h"
#include "mapsync.h"
#include "net.h"
#include "pane.h"
#include "diag.h"
#include "zonesync.h"
#include "backup.h"
#include "prefabs.h"
#include "savesync.h"
#include "lightsync.h"
#include "assets.h"
#include <cmath>

namespace
{
    Config g_cfg;
    ModToolsPane g_pane = nullptr;
    void* g_menu = nullptr;
    uint64_t g_loadTick = 0;
    bool g_wasConnected = false;

    std::wstring Widen(const std::string& s)
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n ? n - 1 : 0, L'\0');
        if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
        return w;
    }
    std::string Narrow(const std::wstring& w)
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n ? n - 1 : 0, '\0');
        if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
        return s;
    }

    // ---- overlay: peer frustums, labels, selections, soft locks, status line, chat -------------
    constexpr float kDeg = 3.14159265f / 180.0f;
    void AngleVectors(const float angles[3], float fwd[3], float right[3], float up[3])
    {
        float p = angles[0] * kDeg, y = angles[1] * kDeg;      // positive pitch looks up (Radiant, measured)
        float cp = cosf(p), sp = sinf(p), cy = cosf(y), sy = sinf(y);
        fwd[0] = cp * cy; fwd[1] = cp * sy; fwd[2] = sp;
        right[0] = fwd[1]; right[1] = -fwd[0]; right[2] = 0;
        float rl = sqrtf(right[0] * right[0] + right[1] * right[1]);
        if (rl < 1e-5f) { right[0] = 0; right[1] = -1; } else { right[0] /= rl; right[1] /= rl; }
        up[0] = right[1] * fwd[2] - right[2] * fwd[1];
        up[1] = right[2] * fwd[0] - right[0] * fwd[2];
        up[2] = right[0] * fwd[1] - right[1] * fwd[0];
    }
    void Line(const float a[3], const float b[3], uint32_t argb) { rp::host->DrawWorldLine(rp::self, a, b, argb); }
    void Glyph(const float origin[3], const float angles[3], uint32_t argb)
    {
        float f[3], r[3], u[3];
        AngleVectors(angles, f, r, u);
        const float L = 64.0f, W = 32.0f, H = 20.0f;
        float base[4][3];
        for (int i = 0; i < 4; ++i)
        {
            float sx = (i == 0 || i == 3) ? -1.0f : 1.0f, sy = (i < 2) ? 1.0f : -1.0f;
            for (int k = 0; k < 3; ++k) base[i][k] = origin[k] + f[k] * L + r[k] * W * sx + u[k] * H * sy;
        }
        for (int i = 0; i < 4; ++i) { Line(origin, base[i], argb); Line(base[i], base[(i + 1) % 4], argb); }
        float t0[3], t1[3];
        for (int k = 0; k < 3; ++k) { t0[k] = (base[0][k] + base[1][k]) * 0.5f; t1[k] = t0[k] + u[k] * H; }
        Line(t0, t1, argb);
    }
    void Box(const float mins[3], const float maxs[3], uint32_t argb)
    {
        auto corner = [&](int i, float* out) { out[0] = (i & 1) ? maxs[0] : mins[0]; out[1] = (i & 2) ? maxs[1] : mins[1]; out[2] = (i & 4) ? maxs[2] : mins[2]; };
        static const int edges[12][2] = { {0,1},{1,3},{3,2},{2,0}, {4,5},{5,7},{7,6},{6,4}, {0,4},{1,5},{2,6},{3,7} };
        for (auto& e : edges) { float a[3], b[3]; corner(e[0], a); corner(e[1], b); Line(a, b, argb); }
    }
    void Text(float x, float y, const std::string& s, uint32_t argb, float px = 2.0f) { rp::host->DrawLabel(rp::self, x, y, s.c_str(), argb, px); }
    // size of a label: Radiant's HUD font in the 3D view (host MeasureLabel), the host's 4x6 stroke font otherwise
    void Measure(const std::string& s, float& w, float& h, float px = 2.0f)
    {
        if (rp::Has(&rp::host->MeasureLabel) && rp::host->MeasureLabel) { rp::host->MeasureLabel(rp::self, s.c_str(), px, &w, &h); return; }
        w = 6.0f * px * float(s.size()); h = 6.0f * px;
    }

    void OnDraw(const ModToolsDrawContext* ctx)
    {
        const std::vector<std::string> localChain = presence::LocalPrefabChain();
        for (const presence::Peer& peer : presence::Snapshot())
        {
            if (!peer.hasCamera) continue;
            // A peer inside a prefab sends its camera and selection in that prefab's own space (Radiant shows an entered
            // prefab at its file's origin, unrotated). Bring them into our space through the placement of the entered instances on both sides.
            presence::Xform xf; bool moved = presence::PeerToLocal(peer, xf);
            float po[3], pa[3];
            presence::InterpolatedPose(peer, po, pa);
            if (moved) { float o[3], a[3]; presence::XfPoint(xf, po, o); presence::XfCameraAngles(xf, pa, a); memcpy(po, o, sizeof po); memcpy(pa, a, sizeof pa); }
            Glyph(po, pa, peer.color);
            float lx, ly;
            if (rp::host->Project(rp::self, po, &lx, &ly))
            {
                float tw, th; Measure(peer.name, tw, th);
                Text(lx + 10, ly - th - 6, peer.name, peer.color);
                // where they are editing, under the name, only when it differs from where we are: a peer inside a prefab
                // we are not in (amber), or a peer outside while we are inside one (grey)
                if (peer.scopeKnown && peer.prefabChain != localChain)
                {
                    if (!peer.prefabChain.empty()) Text(lx + 10, ly - 2, "in prefab " + presence::PrefabChainText(peer.prefabChain, true), 0xFFFFD040, 1.6f);
                    else if (!localChain.empty()) Text(lx + 10, ly - 2, "outside prefab", 0xFFB0B0B0, 1.6f);
                }
            }
            // what they have selected: a box per brush in their colour (inflated slightly so it does not z-fight the brush edges)
            for (const auto& box : peer.selection)
            {
                float mn[3], mx[3];
                if (moved) presence::XfBox(xf, box.mins, box.maxs, mn, mx); else { memcpy(mn, box.mins, sizeof mn); memcpy(mx, box.maxs, sizeof mx); }
                for (int k = 0; k < 3; ++k) { mn[k] -= 1.0f; mx[k] += 1.0f; }
                Box(mn, mx, peer.color);
            }
        }
        // soft locks: our selected objects that a peer also holds, boxed in red with the peer's name
        std::vector<presence::Conflict> conflicts = presence::Conflicts();
        for (const auto& c : conflicts)
        {
            float mn[3], mx[3];
            for (int k = 0; k < 3; ++k) { mn[k] = c.mins[k] - 2.0f; mx[k] = c.maxs[k] + 2.0f; }
            Box(mn, mx, 0xFFFF3030);
            for (int k = 0; k < 3; ++k) { mn[k] -= 1.0f; mx[k] += 1.0f; }
            Box(mn, mx, 0xFFFF3030);
            float top[3] = { (c.mins[0] + c.maxs[0]) * 0.5f, (c.mins[1] + c.maxs[1]) * 0.5f, c.maxs[2] + 8.0f };
            float lx, ly;
            std::string label = c.peerName + " HAS IT";
            float tw, th; Measure(label, tw, th);
            if (rp::host->Project(rp::self, top, &lx, &ly)) Text(lx - tw * 0.5f, ly - th - 2, label, 0xFFFF3030);
        }
        if (ctx->view != 1) return;
        if (net::IsRunning())
        {
            // Session status, top-left of the camera pane under Radiant's own lighting notes: the session line, a sync warning,
            // the asset transfer line, then recent chat. The host address follows the pane's Hide box (this view ends up on streams).
            const float left = 12.0f; float cy = 64.0f, tw, lineH;
            std::string line = "Team Create: " + presence::SessionText(Cfg().hideIp);
            size_t peers = presence::Snapshot().size();
            line += "  |  " + std::to_string(peers) + (peers == 1 ? " peer" : " peers");
            std::string sync = net::IsConnected() ? mapsync::Status() : "";
            if (!sync.empty()) line += "  |  edits " + sync;
            if (!conflicts.empty()) line += "  |  LOCKED BY " + conflicts[0].peerName;
            Text(left, cy, line, !conflicts.empty() ? 0xFFFF5050 : net::IsConnected() ? 0xFF80FF80 : 0xFFFFC060);
            Measure(line, tw, lineH); cy += lineH + 4.0f;
            std::string warn = mapsync::Warning();
            if (!warn.empty()) { Text(left, cy, warn, 0xFFFFC060); cy += lineH + 4.0f; }
            std::string transfer = assets::HudLine();
            if (!transfer.empty()) { Text(left, cy, "Assets: " + transfer, transfer.rfind("receiving", 0) == 0 || transfer.rfind("sending", 0) == 0 ? 0xFF80C8FF : 0xFFC0C0C0); cy += lineH + 4.0f; }
            cy += 2.0f;
            // recent chat, fading out after 12 s
            uint64_t now = GetTickCount64();
            for (const auto& cl : presence::ChatHistory(4))
            {
                if (now - cl.tick > 12000) continue;
                Text(left, cy, cl.name + ": " + cl.text, 0xFFE0E0E0);
                cy += lineH + 4.0f;
            }
        }
        else if (!presence::IsActive())
        {
            std::string ended = presence::EndReason();
            float y = 64.0f;
            if (!ended.empty()) Text(12.0f, y, "Team Create: " + ended, 0xFFFFC060);
        }
    }

    // ---- [debug] scripted prefab navigation for unattended tests --------------------------------
    void DebugPrefabScript()
    {
        static bool selected = false;
        if (!selected && g_loadTick && !Cfg().debugSelectGuid.empty() && Cfg().debugSelectAfterMs && GetTickCount64() - g_loadTick >= Cfg().debugSelectAfterMs)
        {
            selected = true;
            LOG("debug: select %s -> %d", Cfg().debugSelectGuid.c_str(), hooks::SelectEntityByGuid(Cfg().debugSelectGuid));
        }
        static int stage = 0;
        if (stage >= 2 || g_loadTick == 0 || Cfg().debugEnterPrefabGuid.empty() || Cfg().debugEnterAfterMs == 0) return;
        static uint64_t enteredAt = 0;
        uint64_t now = GetTickCount64();
        if (stage == 0 && now - g_loadTick >= Cfg().debugEnterAfterMs)
        {
            stage = 1;
            bool sel = hooks::SelectEntityByGuid(Cfg().debugEnterPrefabGuid);
            bool ok = sel && hooks::EnterSelectedPrefab();
            LOG("debug: enter prefab %s: selected=%d entered=%d depth=%d", Cfg().debugEnterPrefabGuid.c_str(), sel, ok, hooks::PrefabDepth());
            enteredAt = now;
            if (!ok || Cfg().debugLeaveAfterMs == 0) stage = 2;
        }
        else if (stage == 1 && now - enteredAt >= Cfg().debugLeaveAfterMs)
        {
            stage = 2;
            bool ok = hooks::LeavePrefab();
            LOG("debug: leave prefab: %d depth=%d", ok, hooks::PrefabDepth());
        }
    }

    // ---- plugin callbacks ------------------------------------------------------------------------
    void Hotkey(void*) { pane::Toggle(); }

    int OnLoad(void)
    {
        LoadConfig();
        mapsync::Init();
        diag::Init();
        lightsync::Install();
        if (qt::Bind())
        {
            g_menu = rp::host->AddMenu(rp::self, "Team Create");
            if (pane::Create() && g_menu)
                if (void* toggle = rp::host->PaneToggleAction(rp::self, g_pane)) rp::host->AddMenuAction(rp::self, g_menu, toggle);
            if (g_menu && rp::host->AddMenuItem)
            {
                if (rp::host->AddMenuSeparator) rp::host->AddMenuSeparator(rp::self, g_menu);
                rp::host->AddMenuItem(rp::self, g_menu, "Experimental notice...", [](void*) { pane::ShowNotice(); }, nullptr);   // read the first-open box again
            }
        }
        else LOG("Qt unavailable: no Team Create pane (sessions can still be started from TeamCreate.ini)");
        rp::host->RegisterHotkey(rp::self, 'T', MODTOOLS_MOD_CTRL | MODTOOLS_MOD_SHIFT, Hotkey, nullptr);
        presence::InstallCloseWatch();
        presence::Start();
        return 1;
    }

    void OnUnload(void)
    {
        presence::Stop();
        assets::Stop();
        diag::Shutdown();
        zonesync::Shutdown();
        pane::Shutdown();
        rp::host->InhibitReloadPrompts(rp::self, 0);
    }

    void OnTick(void)
    {
        presence::Tick();
        diag::Tick();
        pane::Tick();
        savesync::Tick();
        lightsync::Tick();
        assets::Tick();
        zonesync::Tick();
        DebugPrefabScript();
        // Peer cameras and selection boxes are drawn in the 2D views too, but Radiant only repaints those when something marks them dirty.
        // Refresh the 2D views each tick while a peer has a camera, only when "Update XY on drag" is on (Preferences > Camera).
        if (rp::Has(&rp::host->CamXYUpdateOnDrag) && rp::host->RequestRedraw && rp::host->CamXYUpdateOnDrag &&
            rp::host->CamXYUpdateOnDrag(rp::self))
        {
            for (const presence::Peer& peer : presence::Snapshot())
                if (peer.hasCamera) { rp::host->RequestRedraw(rp::self, MODTOOLS_VIEW_2D); break; }
        }
        // while a session is connected the map file is owned by the sync: an external-change prompt would be wrong
        bool connected = Cfg().syncEnabled && net::IsConnected();
        if (connected != g_wasConnected) { g_wasConnected = connected; rp::host->InhibitReloadPrompts(rp::self, connected ? 1 : 0); }
    }

    void OnMapLoaded(const wchar_t* path)
    {
        if (!g_loadTick) g_loadTick = GetTickCount64();
        mapsync::OnMapLoaded(path);
        presence::OnScopeChanged(path ? path : L"", 0);      // a (re)load always lands at the top level of the map
        presence::OnMapLoaded();
    }
    void OnMapSaved(const wchar_t* path, int autosave, int selectedOnly)
    {
        if (!autosave && !selectedOnly && hooks::PrefabDepth() > 0) mapsync::OnScopeSaved();
        if (!autosave && !selectedOnly) presence::OnMapSaved(path ? path : L"");
        if (!autosave && !selectedOnly) savesync::OnLocalSave(path ? path : L"");
        if (!autosave && !selectedOnly) assets::OnSaved(path ? path : L"");
    }
    void OnBeforeScopeChange(void) { mapsync::BeforeScopeChange(); }
    void OnScopeChanged(const wchar_t* path, int depth) { mapsync::OnScopeChanged(path, depth); presence::OnScopeChanged(path ? path : L"", depth); }
    const char* OnCommand(const char* cmd)
    {
        static std::string reply;
        if (strcmp(cmd, "diag") == 0 || strcmp(cmd, "diag status") == 0) { reply = diag::Status(); return reply.c_str(); }
        if (strcmp(cmd, "diag ship") == 0) { diag::ShipNow(); reply = "queued: previous log tail + recent dumps to everyone"; return reply.c_str(); }
        if (strcmp(cmd, "backup") == 0 || strcmp(cmd, "backup status") == 0) { reply = backup::Status(); return reply.c_str(); }
        if (strcmp(cmd, "map") == 0) { reply = presence::MapStatus(); return reply.c_str(); }
        if (strcmp(cmd, "prefabs") == 0) { reply = prefabs::Status(); return reply.c_str(); }
        if (strcmp(cmd, "assets") == 0) { reply = assets::Status(); return reply.c_str(); }
        if (!pane::Created()) return nullptr;
        reply = pane::Command(cmd);
        if (reply.rfind("unknown command", 0) == 0) return nullptr;
        return reply.c_str();
    }
    void OnSettings(void) { if (pane::Created()) { dock::Show(true); } }

    // event filter adapter: the pane's filter has no user pointer
    struct FilterAdapter { qt::FilterFn fn; };
    int FilterThunk(void* user, ModToolsWidget watched, int type) { return static_cast<FilterAdapter*>(user)->fn(watched, type) ? 1 : 0; }
}

// ---- Config -------------------------------------------------------------------------------------
Config& Cfg() { return g_cfg; }

void LoadConfig()
{
    g_cfg.role = Widen(rp::ConfigGet("net", "role", "off"));
    g_cfg.autostart = rp::ConfigGetBool("net", "autostart", false);
    g_cfg.hostAddr = Widen(rp::ConfigGet("net", "host", "127.0.0.1"));
    g_cfg.bindAddr = Widen(rp::ConfigGet("net", "bind", "0.0.0.0"));
    g_cfg.port = unsigned(rp::ConfigGetInt("net", "port", 27017));
    g_cfg.userName = Widen(rp::ConfigGet("net", "name", ""));
    g_cfg.hideIp = rp::ConfigGetBool("net", "hide_ip", true);
    g_cfg.cameraColor = presence::ParseColor(rp::ConfigGet("net", "color", ""));
    g_cfg.syncEnabled = rp::ConfigGetBool("sync", "enabled", true);
    g_cfg.syncApplyDelayMs = unsigned(rp::ConfigGetInt("sync", "apply_delay_ms", 700));
    g_cfg.syncInPlace = rp::ConfigGetBool("sync", "in_place", true);
    g_cfg.keepCamera = rp::ConfigGetBool("sync", "keep_camera", true);
    g_cfg.syncLiveMove = rp::ConfigGetBool("sync", "live_move", true);
    g_cfg.syncLiveKeys = rp::ConfigGetBool("sync", "live_keys", true);
    g_cfg.syncSaves = rp::ConfigGetBool("sync", "sync_saves", false);
    g_cfg.syncLights = rp::ConfigGetBool("sync", "sync_lights", false);
    g_cfg.syncReloadClasses = Widen(rp::ConfigGet("sync", "reload_classes", "reflection_probe"));
    g_cfg.backupEnabled = rp::ConfigGetBool("backup", "enabled", true);
    g_cfg.backupKeep = unsigned(rp::ConfigGetInt("backup", "keep", 20));
    { int days = rp::ConfigGetInt("backup", "asset_days", 30), keep = rp::ConfigGetInt("backup", "asset_keep", 10);
      g_cfg.assetBackupDays = days > 0 ? unsigned(days) : 0; g_cfg.assetBackupKeep = keep > 0 ? unsigned(keep) : 0; }
    assets::SetBackupRetention(g_cfg.assetBackupDays, g_cfg.assetBackupKeep);
    g_cfg.verbose = rp::ConfigGetBool("log", "verbose", false);
    g_cfg.diagShare = rp::ConfigGetBool("diag", "share_logs", true);
    g_cfg.diagKeep = rp::ConfigGetBool("diag", "keep_peer_logs", true);
    g_cfg.diagDumpMaxMb = unsigned(rp::ConfigGetInt("diag", "dump_max_mb", 80));
    g_cfg.diagDumpMaxAgeDays = unsigned(rp::ConfigGetInt("diag", "dump_max_age_days", 3));
    assets::SetSendConverted(rp::ConfigGetBool("assets", "send_converted", true));
    g_cfg.debugEnterPrefabGuid = rp::ConfigGet("debug", "enter_prefab_guid", "");
    g_cfg.debugEnterAfterMs = unsigned(rp::ConfigGetInt("debug", "enter_after_ms", 0));
    g_cfg.debugLeaveAfterMs = unsigned(rp::ConfigGetInt("debug", "leave_after_ms", 0));
    g_cfg.debugSelectGuid = rp::ConfigGet("debug", "select_guid", "");
    g_cfg.debugSelectAfterMs = unsigned(rp::ConfigGetInt("debug", "select_after_ms", 0));
    LOGW(L"cfg: role=%s host=%s:%u bind=%s name='%s' sync=%d in_place=%d", g_cfg.role.c_str(), g_cfg.hostAddr.c_str(), g_cfg.port, g_cfg.bindAddr.c_str(), g_cfg.userName.c_str(), g_cfg.syncEnabled, g_cfg.syncInPlace);
}

void SaveNetConfig()
{
    rp::ConfigSet("net", "role", Narrow(g_cfg.role));
    rp::ConfigSet("net", "host", Narrow(g_cfg.hostAddr));
    rp::ConfigSet("net", "port", std::to_string(g_cfg.port));
    rp::ConfigSet("net", "name", Narrow(g_cfg.userName));
    rp::ConfigSet("net", "bind", Narrow(g_cfg.bindAddr));
    rp::ConfigSet("net", "hide_ip", g_cfg.hideIp ? "1" : "0");
}

// ---- qt / dock adapters ----------------------------------------------------------------------------
namespace qt
{
    void* MakeEventFilter(FilterFn fn)
    {
        auto* a = new FilterAdapter{ fn };
        return rp::host->MakeEventFilter(rp::self, FilterThunk, a);
    }
}

namespace dock
{
    bool Ready() { return qt::Bind(); }
    bool Host(void* content, const char* title, int w, int h)
    {
        g_pane = rp::host->CreatePane(rp::self, title, content, w, h);
        return g_pane != nullptr;
    }
    bool Hosted() { return g_pane != nullptr; }
    bool Visible() { return g_pane && rp::host->PaneVisible(rp::self, g_pane) != 0; }
    void Show(bool show) { if (g_pane) rp::host->ShowPane(rp::self, g_pane, show ? 1 : 0); }
}

// ---- entry point ------------------------------------------------------------------------------------
extern "C" MODTOOLS_PLUGIN_EXPORT const ModToolsPluginApi* ModToolsPluginEntry(ModToolsPlugin* self, const ModToolsHostApi* host)
{
    rp::Bind(self, host);
    static ModToolsPluginApi api = {};
    api.size = sizeof api;
    api.apiVersion = MODTOOLS_PLUGIN_API_VERSION;
    api.name = "TeamCreate";
    api.displayName = "Team Create";
    api.version = "0.9.1";   // experimental
    api.author = "Figglebottom";
    api.description = "Allows for multiple users to edit a map in Radiant at the same time, and to play it together with Play Test.";
    api.url = "https://github.com/Aidan601/MTPlugins/tree/main/plugins/TeamCreate";
    api.OnLoad = OnLoad;
    api.OnUnload = OnUnload;
    api.OnTick = OnTick;
    api.OnMapLoaded = OnMapLoaded;
    api.OnMapSaved = OnMapSaved;
    api.OnBeforeScopeChange = OnBeforeScopeChange;
    api.OnScopeChanged = OnScopeChanged;
    api.OnDraw = OnDraw;
    api.OnCommand = OnCommand;
    api.OnSettings = OnSettings;
    return &api;
}
