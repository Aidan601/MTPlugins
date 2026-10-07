#pragma once
// Maps the host's internal module names (hooks, mt, fw, render, qt, dock, log, config) onto the plugin API (sdk/modtools_plugin.h).
#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include "modtools_plugin.hpp"

#define LOG(...)  ::rp::Log(__VA_ARGS__)
#define LOGW(...) ::rp::LogW(__VA_ARGS__)

// Plugin settings (TeamCreate.ini next to the DLL).
struct Config
{
    // [net]
    std::wstring role       = L"off";  // off | host | client: the pane default (sessions start from the pane unless autostart=1)
    bool         autostart  = false;   // 1 = start the [net] role when Radiant opens (test loops)
    std::wstring hostAddr   = L"127.0.0.1";
    std::wstring bindAddr   = L"0.0.0.0";  // host listen address, 127.0.0.1 = local tests only (no firewall prompt)
    unsigned     port       = 27017;   // TCP, same number as BO3's LAN game (UDP), so one router rule covers both
    std::wstring userName   = L"";
    bool         hideIp     = true;    // pane "Hide": host address shown as dots and left out of the status line
    uint32_t     cameraColor = 0;       // ARGB the peers draw our camera in, 0 = automatic (palette colour of our peer id)

    // [window]

    // [sync]
    bool     syncEnabled    = true;    // M3 object sync
    unsigned syncApplyDelayMs = 700;   // quiet time before remote changes are applied
    bool     syncInPlace    = true;    // apply remote blocks through Radiant's paste/delete path (no reload)
    bool     keepCamera     = true;    // keep the 3D camera across the reload path
    bool     syncSaves      = false;   // "Sync saves" (savesync.cpp)
    bool     syncLights     = false;   // "Sync light builds" (lightsync.cpp)
    bool     syncLiveMove   = true;    // follow a peer's drag live (translation only)
    bool     syncLiveKeys   = true;    // send key/value edits of selected entities once they settle, not on deselect
    // Comma separated classes whose runtime instance the clipboard import does not build (reflection_probe asserts in
    // the lighting pass), so they arrive through the reload path
    std::wstring syncReloadClasses = L"reflection_probe";

    // [backup] safety copies of the files the sync rewrites on disk (backup.cpp)
    bool     backupEnabled  = true;    // copy a map file to <root>\radiant\teamcreate_backup before it is rewritten
    unsigned backupKeep     = 20;      // newest copies kept per map, 0 = keep everything
    unsigned assetBackupDays = 30;     // asset backup sessions older than this are removed, 0 = never
    unsigned assetBackupKeep = 10;     // newest sessions kept whatever their age

    // [log]
    bool     verbose        = false;

    // [diag] diagnostics sharing over the session (diag.cpp)
    bool     diagShare      = true;    // stream my log lines and ship my recent crash dumps
    bool     diagKeep       = true;    // store what peers send under <PluginDir>\peerlogs\<name>
    unsigned diagDumpMaxMb  = 80;      // bigger dumps are not shipped (Radiant's own ~0.7 MB, isolation dump ~60 MB, LocalDumps ~100 MB)
    unsigned diagDumpMaxAgeDays = 3;   // only dumps this recent are shipped on join

    // [debug] scripted prefab navigation for unattended tests (all off by default)
    std::string debugEnterPrefabGuid;   // GUID of a misc_prefab entity to enter after the map loads
    unsigned    debugEnterAfterMs = 0;  // delay after the load before entering (0 = never)
    unsigned    debugLeaveAfterMs = 0;  // delay after entering before leaving (0 = stay inside)
    std::string debugSelectGuid;        // GUID of an entity to select after the load (soft-lock test)
    unsigned    debugSelectAfterMs = 0;
};
Config& Cfg();
void    LoadConfig();
void    SaveNetConfig();               // writes the [net] keys back

namespace mt
{
    template <typename F> void Post(F fn) { rp::PostToMainThread(std::move(fn)); }
    inline bool IsMainThread() { return rp::host->IsMainThread(rp::self) != 0; }
    inline HWND MainWindow() { return static_cast<HWND>(rp::host->MainWindow(rp::self)); }
}

namespace fw
{
    inline void NoteSelfWriteBegin() { rp::host->BeginOwnWrite(rp::self); }
    inline void NoteSelfWriteEnd() { rp::host->EndOwnWrite(rp::self); }
}

namespace hooks
{
    using RadPtr = unsigned char*;
    struct SelectedBrush { std::string brushGuid; std::string entityGuid; float mins[3]; float maxs[3]; };

    inline std::wstring CurrentMapPath() { return rp::CurrentMapPath(); }
    inline bool GetCamera(float o[3], float a[3]) { return rp::host->GetCamera(rp::self, o, a) != 0; }
    inline bool SetCamera(const float o[3], const float a[3]) { return rp::host->SetCamera(rp::self, o, a) != 0; }
    inline bool GameView() { return rp::host->GameView(rp::self) != 0; }
    inline void SetGameView(bool on) { rp::host->SetGameView(rp::self, on ? 1 : 0); }
    inline bool GetSelection(std::vector<SelectedBrush>& out, size_t = 4096)
    {
        out.clear();
        for (const ModToolsSelectedBrush& s : rp::GetSelection())
        {
            SelectedBrush b; b.brushGuid = s.brushGuid; b.entityGuid = s.entityGuid;
            memcpy(b.mins, s.mins, sizeof b.mins); memcpy(b.maxs, s.maxs, sizeof b.maxs);
            out.push_back(std::move(b));
        }
        return true;
    }
    inline void ForEachEntity(const std::function<void(RadPtr)>& fn) { rp::ForEachEntity([&](ModToolsEntity e) { fn(static_cast<RadPtr>(e)); }); }
    inline void ForEachEntityBrush(RadPtr ent, const std::function<void(RadPtr)>& fn) { rp::ForEachEntityBrush(ent, [&](ModToolsBrush b) { fn(static_cast<RadPtr>(b)); }); }
    inline std::string EntityGuid(RadPtr e) { return rp::EntityGuid(e); }
    inline RadPtr WorldEntity() { return static_cast<RadPtr>(rp::host->WorldEntity(rp::self)); }
    inline std::string BrushGuid(RadPtr b) { return rp::BrushGuid(b); }
    inline RadPtr BrushOwner(RadPtr b) { return static_cast<RadPtr>(rp::host->BrushOwner(rp::self, b)); }
    inline bool WriteEntityText(RadPtr e, std::string& out) { return rp::WriteEntityText(e, out); }
    inline bool WriteEntityTextWith(RadPtr e, const std::vector<RadPtr>& brushes, std::string& out) { return rp::WriteEntityTextWith(e, std::vector<ModToolsBrush>(brushes.begin(), brushes.end()), out); }
    inline bool EntityFixedSize(RadPtr e) { return rp::EntityFixedSize(e); }
    inline RadPtr BrushOfInst(RadPtr inst) { return static_cast<RadPtr>(rp::BrushOfInst(inst)); }
    inline std::vector<float> BrushPlanes(RadPtr b) { return rp::BrushPlanes(b); }
    inline bool SetBrushPlanes(RadPtr b, const std::vector<float>& planes) { return rp::SetBrushPlanes(b, planes); }
    inline bool BrushBounds(RadPtr b, float mins[3], float maxs[3]) { return rp::BrushBounds(b, mins, maxs); }
    inline bool HasUntracked() { return rp::HasUntracked(); }
    inline void BeginUntracked() { rp::BeginUntracked(); }
    inline void EndUntracked() { rp::EndUntracked(); }
    inline bool GetUndoTags(RadPtr o, bool entity, int tags[3]) { return rp::GetUndoTags(o, entity, tags); }
    inline bool SetUndoTags(RadPtr o, bool entity, const int tags[3]) { return rp::SetUndoTags(o, entity, tags); }
    inline bool ReloadMapNow(const std::wstring& path) { return rp::host->ReloadMap(rp::self, path.c_str(), Cfg().keepCamera ? 1 : 0) != 0; }
    inline bool IsReloading() { return rp::host->IsReloading(rp::self) != 0; }
    inline bool MouseButtonHeld() { return ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000) != 0; }
    inline bool ImportMapText(const std::string& text, const char* label) { return rp::host->ImportMapText(rp::self, text.c_str(), label) != 0; }
    // Hands a peer's import to Play Test's Live Edit while still selected, no-op without PlayTest or a game
    inline void PublishToPlayTest() { if (rp::host->Command) rp::host->Command(rp::self, "PlayTest", "live publish peer"); }
    inline bool DeleteBrushes(const std::vector<RadPtr>& brushes, const char* label)
    {
        std::vector<ModToolsBrush> v(brushes.begin(), brushes.end());
        return rp::host->DeleteBrushes(rp::self, v.data(), v.size(), label) != 0;
    }
    inline std::vector<RadPtr> SelectedBrushPtrs()
    {
        std::vector<ModToolsBrushInst> v = rp::SelectedBrushInsts();
        return std::vector<RadPtr>(reinterpret_cast<RadPtr*>(v.data()), reinterpret_cast<RadPtr*>(v.data()) + v.size());
    }
    inline bool RestampImportedGuids(const std::string& entityGuid, const std::vector<std::string>& brushGuids)
    {
        std::vector<const char*> v; for (const auto& g : brushGuids) v.push_back(g.c_str());
        return rp::host->RestampImportedGuids(rp::self, entityGuid.c_str(), v.data(), v.size()) != 0;
    }
    // absent on an older host (then every entity is imported on its own)
    inline bool CanRestampImportedEntities() { return rp::Has(&rp::host->RestampImportedEntities) && rp::host->RestampImportedEntities; }
    inline size_t RestampImportedEntities(const std::vector<std::string>& entityGuids, const std::vector<std::vector<std::string>>& brushGuids)
    {
        if (!CanRestampImportedEntities() || entityGuids.size() != brushGuids.size()) return 0;
        std::vector<const char*> e, b; std::vector<size_t> counts;
        for (size_t i = 0; i < entityGuids.size(); ++i)
        {
            e.push_back(entityGuids[i].c_str()); counts.push_back(brushGuids[i].size());
            for (const auto& g : brushGuids[i]) b.push_back(g.c_str());
        }
        return rp::host->RestampImportedEntities(rp::self, e.data(), e.size(), b.data(), counts.data());
    }
    inline void SelectBrushes(const std::vector<RadPtr>& insts)
    {
        std::vector<ModToolsBrushInst> v(insts.begin(), insts.end());
        rp::host->SelectBrushInsts(rp::self, v.data(), v.size());
    }
    inline void DeselectAll() { rp::host->DeselectAll(rp::self); }
    inline int  PrefabDepth() { return rp::host->PrefabDepth(rp::self); }
    inline bool PrefabStackPlacement(int level, float origin[3], float angles[3]) { return rp::Has(&rp::host->PrefabStackPlacement) && rp::host->PrefabStackPlacement && rp::host->PrefabStackPlacement(rp::self, level, origin, angles) != 0; }
    inline bool SelectEntityByGuid(const std::string& g) { return rp::host->SelectEntityByGuid(rp::self, g.c_str()) != 0; }
    inline bool EnterSelectedPrefab() { return rp::host->EnterSelectedPrefab(rp::self) != 0; }
    inline bool LeavePrefab() { return rp::host->LeavePrefab(rp::self) != 0; }
}

namespace render
{
    struct CamParams { float origin[3]; float angles[3]; int width, height; bool gameView; };
    inline CamParams LastCam()
    {
        ModToolsCamParams c{}; rp::host->LastCamParams(rp::self, &c);
        CamParams r; memcpy(r.origin, c.origin, sizeof r.origin); memcpy(r.angles, c.angles, sizeof r.angles); r.width = c.width; r.height = c.height; r.gameView = c.gameView != 0;
        return r;
    }
    inline void* CamWindow() { return rp::host->CamWindow(rp::self); }
}

// Qt widget helpers, same names as the host's qt.h, over host->ui.
namespace qt
{
    inline bool Bind() { return rp::host && rp::host->ui != nullptr; }
    inline void* Widget(void* parent) { return rp::host->ui->Widget(parent); }
    inline void* Label(const char* t, void* p) { return rp::host->ui->Label(t, p); }
    inline void* LineEdit(const char* t, void* p) { return rp::host->ui->LineEdit(t, p); }
    inline void* PushButton(const char* t, void* p) { return rp::host->ui->PushButton(t, p); }
    inline void* CheckBox(const char* t, void* p) { return rp::host->ui->CheckBox(t, p); }
    inline void* RadioButton(const char* t, void* p) { return rp::host->ui->RadioButton(t, p); }
    inline void* ListWidget(void* p) { return rp::host->ui->ListWidget(p); }
    inline void* PlainTextEdit(void* p) { return rp::host->ui->PlainTextEdit(p); }
    inline void* VBox(void* p) { return rp::host->ui->VBox(p); }
    inline void* HBox() { return rp::host->ui->HBox(); }
    inline void* Grid() { return rp::host->ui->Grid(); }
    inline void  BoxAddWidget(void* b, void* w, int s = 0) { rp::host->ui->BoxAddWidget(b, w, s); }
    inline void  BoxAddLayout(void* b, void* l, int s = 0) { rp::host->ui->BoxAddLayout(b, l, s); }
    inline void  BoxAddStretch(void* b, int s = 1) { rp::host->ui->BoxAddStretch(b, s); }
    inline void  GridAddWidget(void* g, void* w, int r, int c) { rp::host->ui->GridAddWidget(g, w, r, c); }
    inline void  GridColumnStretch(void* g, int c, int s) { rp::host->ui->GridColumnStretch(g, c, s); }
    inline void  LayoutMargins(void* l, int a, int b, int c, int d) { rp::host->ui->LayoutMargins(l, a, b, c, d); }
    inline void  LayoutSpacing(void* l, int s) { rp::host->ui->LayoutSpacing(l, s); }
    inline void  SetLabelText(void* w, const char* t) { rp::host->ui->SetLabelText(w, t); }
    inline void  SetWordWrap(void* w, bool on) { rp::host->ui->SetWordWrap(w, on); }
    inline void  SetVisible(void* w, bool on) { rp::host->ui->SetVisible(w, on); }
    inline void  SetButtonText(void* w, const char* t) { rp::host->ui->SetButtonText(w, t); }
    inline void  SetEditText(void* w, const char* t) { rp::host->ui->SetEditText(w, t); }
    inline std::string EditText(void* w) { return rp::EditText(w); }
    inline void  SetPlaceholder(void* w, const char* t) { rp::host->ui->SetPlaceholder(w, t); }
    inline void  SetChecked(void* w, bool on) { rp::host->ui->SetChecked(w, on); }
    inline bool  IsChecked(void* w) { return rp::host->ui->IsChecked(w) != 0; }
    inline bool  IsDown(void* w) { return rp::host->ui->IsDown(w) != 0; }
    inline void  SetEnabled(void* w, bool on) { rp::host->ui->SetEnabled(w, on); }
    inline void  SetReadOnly(void* w, bool on) { rp::host->ui->SetReadOnly(w, on); }
    inline void  ListClear(void* w) { rp::host->ui->ListClear(w); }
    inline void  ListAdd(void* w, const char* t) { rp::host->ui->ListAdd(w, t); }
    inline void  PlainSetText(void* w, const char* t) { rp::host->ui->PlainSetText(w, t); }
    inline void  PlainScrollToEnd(void* w) { rp::host->ui->PlainScrollToEnd(w); }
    inline void  SetMinimumHeight(void* w, int h) { rp::host->ui->SetMinimumHeight(w, h); }
    inline void  SetMaximumHeight(void* w, int h) { rp::host->ui->SetMaximumHeight(w, h); }
    inline void  SetMaximumWidth(void* w, int x) { rp::host->ui->SetMaximumWidth(w, x); }
    inline void  SetToolTip(void* w, const char* t) { rp::host->ui->SetToolTip(w, t); }
    inline void  SetFocus(void* w) { rp::host->ui->SetFocus(w); }
    inline bool  CursorInside(void* w) { return rp::host->ui->CursorInside(w) != 0; }
    inline void  InstallFilter(void* o, void* f) { rp::host->ui->InstallFilter(o, f); }
    inline void  RemoveFilter(void* o, void* f) { rp::host->ui->RemoveFilter(o, f); }
    // appended to the table: an older host has none (false, text stays visible)
    inline bool  SetEchoMode(void* w, int mode)
    {
        const ModToolsUiApi* ui = rp::host->ui;
        if (reinterpret_cast<const char*>(&ui->SetEchoMode) >= reinterpret_cast<const char*>(ui) + ui->size || !ui->SetEchoMode) return false;
        return ui->SetEchoMode(w, mode) != 0;
    }
    using FilterFn = bool (*)(void* watched, int type);
    void* MakeEventFilter(FilterFn fn);           // in teamcreate.cpp (adapts the host's signature)
}

namespace dock
{
    bool Ready();
    bool Host(void* contentWidget, const char* title, int w, int h);
    bool Hosted();
    bool Visible();
    void Show(bool show);
}
