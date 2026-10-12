#include <windows.h>
#include <cstdarg>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "MinHook.h"
#include "app.h"
#include "ape.h"
#include "hostapi.h"
#include "plugins.h"
#include "hooks.h"
#include "render.h"
#include "mainthread.h"
#include "dock.h"
#include "qt.h"
#include "ui.h"
#include "radiant_addrs.h"
#include "config.h"
#include "log.h"

// MAP (grep the names, line numbers drift):
//   Marshalling helpers ....... Narrow, Widen, CopyOut, CopyOutW, Ok (plugin is Loaded), Cb + NewCb + RunCb
//                               (callbacks run through plugins::Call), FilterCb + FilterThunk (event filters)
//   ModToolsHostApi entries (one plain function per table slot, in table order):
//     where things are ........ PluginDir, PluginsRoot, RadiantRoot
//     log + config ............ Log, LogW, ConfigGet, ConfigGetInt, ConfigSet
//     threads ................. PostToMainThread (mt::PostTagged), IsMainThread, MainWindow
//     map ..................... CurrentMapPath, PrefabDepth, IsMapModified, ReloadMap, IsReloading, BeginOwnWrite,
//                               EndOwnWrite, OwnWriteRecent, InhibitReloadPrompts, ReloadPromptsInhibited
//     camera .................. GetCamera, SetCamera, GameView, SetGameView, LastCamParams, CamWindow, H_XYWindow
//     patch vertices .......... H_PatchGrid, H_PatchSetGrid, H_PatchSelectVerts, H_PatchWeldVerts, H_DragMode
//     objects ................. ForEachEntity, ForEachEntityBrush, EntityGuid, BrushGuid, BrushOwner, WorldEntity,
//                               GetSelection, WriteEntityText, FreeString, ImportMapText, DeleteBrushes,
//                               SelectedBrushInsts, SelectBrushInsts, DeselectAll, RestampImportedGuids, H_RestampImportedEntities,
//                               SelectEntityByGuid, EnterSelectedPrefab, LeavePrefab
//     overlay ................. Project, DrawWorldLine, DrawScreenLine, DrawLabel
//     UI ...................... g_menus, g_menuOwner, PaneRec + g_panes, CreatePane, PaneVisible, ShowPane, TabPaneWith,
//                               PaneToggleAction, AddMenu, AddMenuItem, AddMenuAction, AddMenuSeparator,
//                               RegisterHotkey, MakeEventFilter, OnClicked
//     other plugins / raw ..... PluginLoaded, Command, RadiantAddress, Detour (MinHook, p->detours), BrushInstOf, App
//     later additions (H_*) ... H_CreateDialogWindow, H_TriggerMenuAction, H_MenuEntries, H_BeginUndo, H_EndUndo,
//                               H_MeasureLabel, H_RequestRedraw, H_CamXYUpdateOnDrag, H_MoveBrush,
//                               H_FindBrushesByGuid, H_BrushBounds, H_FindEntitiesByGuid, H_BrushPlanes,
//                               H_SetBrushPlanes, H_EntityFixedSize, H_BrushOfInst, H_WriteEntityTextWith, H_PrefabStackPlacement,
//                               H_BeginUntracked, H_EndUntracked, H_GetUndoTags, H_SetUndoTags, H_CommitBrushGeometry
//   ModToolsApeApi (A_*) ....... CopyAsset, A_CurrentAsset, A_SelectedAssets, A_TabCount, A_TabAsset, A_Print,
//                               A_GdtDbPath, A_AssetTypes, A_ApeAddress, A_FindAssets, A_AssetTypeNames,
//                               A_AssetInfo, A_TypeProperties, A_OutputText, A_Properties, A_SetProperty,
//                               A_OpenAsset (see ape.cpp)
//   ModToolsUiApi (W_*) ........ plain wrappers over qt::* (W_Widget ... W_SetEchoMode, panel widgets W_Slider ... W_WidgetNumber, W_SetActionChecked), tables g_api, g_ui, g_ape
//   OnChanged ................. value-change signals by widget class (ValueCb, FireValue)
//   namespace hostapi ......... Table (fills g_api.ui, g_api.ape), ReleasePanes, ReleaseDialogs, ReleaseMenus (unload)

namespace
{
    std::string Narrow(const std::wstring& w)
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n ? n - 1 : 0, '\0');
        if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
        return s;
    }
    std::wstring Widen(const char* s)
    {
        if (!s) return L"";
        int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
        std::wstring w(n ? n - 1 : 0, L'\0');
        if (n) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
        return w;
    }
    size_t CopyOut(const std::string& s, char* out, size_t cap)
    {
        if (out && cap) { size_t n = s.size() < cap - 1 ? s.size() : cap - 1; memcpy(out, s.data(), n); out[n] = 0; }
        return s.size();
    }
    size_t CopyOutW(const std::wstring& s, wchar_t* out, size_t cap)
    {
        if (out && cap) { size_t n = s.size() < cap - 1 ? s.size() : cap - 1; memcpy(out, s.data(), n * sizeof(wchar_t)); out[n] = 0; }
        return s.size();
    }
    bool Ok(ModToolsPlugin* p) { return p && (p->state == ModToolsPlugin::Loaded); }

    // heap contexts for callbacks that outlive the call
    struct Cb { ModToolsCallback fn; void* user; ModToolsPlugin* p; const char* what; };
    Cb* NewCb(ModToolsPlugin* p, ModToolsCallback fn, void* user, const char* what)
    {
        Cb* c = static_cast<Cb*>(malloc(sizeof(Cb)));
        c->fn = fn; c->user = user; c->p = p; c->what = what;
        p->callbacks.push_back(c);
        return c;
    }
    void RunCb(void* a) { Cb* c = static_cast<Cb*>(a); c->fn(c->user); }
    void FireCb(void* user) { Cb* c = static_cast<Cb*>(user); if (c->p->state == ModToolsPlugin::Loaded) plugins::Call(c->p, c->what, RunCb, c); }
    void FireSlot(void* user, void** /*args*/) { FireCb(user); }

    struct FilterCb { ModToolsEventFilterFn fn; void* user; ModToolsPlugin* p; void* watched; int type; int result; };
    void RunFilter(void* a) { auto* c = static_cast<FilterCb*>(a); c->result = c->fn(c->user, c->watched, c->type); }
    bool FilterThunk(void* user, void* watched, int type)
    {
        auto* c = static_cast<FilterCb*>(user);
        if (c->p->state != ModToolsPlugin::Loaded) return false;
        c->watched = watched; c->type = type; c->result = 0;
        plugins::Call(c->p, "event filter", RunFilter, c);
        return c->result != 0;
    }

    // ---- where things are ----
    const wchar_t* PluginDir(ModToolsPlugin* p) { return p->dir.c_str(); }
    const wchar_t* PluginsRoot(ModToolsPlugin*) { return plugins::Root().c_str(); }
    const wchar_t* RadiantRoot(ModToolsPlugin*) { return plugins::RadiantRoot().c_str(); }

    // ---- log + config ----
    void Log(ModToolsPlugin* p, const char* fmt, ...) { va_list ap; va_start(ap, fmt); rtclog::VPrintfPrefixed(p->prefix.c_str(), fmt, ap); va_end(ap); }
    void LogW(ModToolsPlugin* p, const wchar_t* fmt, ...) { va_list ap; va_start(ap, fmt); rtclog::VPrintfPrefixedW(p->prefix.c_str(), fmt, ap); va_end(ap); }
    size_t ConfigGet(ModToolsPlugin* p, const char* section, const char* key, const char* def, char* out, size_t cap)
    {
        wchar_t buf[2048];
        GetPrivateProfileStringW(Widen(section).c_str(), Widen(key).c_str(), Widen(def ? def : "").c_str(), buf, 2048, p->iniPath.c_str());
        return CopyOut(Narrow(buf), out, cap);
    }
    int ConfigGetInt(ModToolsPlugin* p, const char* section, const char* key, int def)
    {
        return GetPrivateProfileIntW(Widen(section).c_str(), Widen(key).c_str(), def, p->iniPath.c_str());
    }
    void ConfigSet(ModToolsPlugin* p, const char* section, const char* key, const char* value)
    {
        WritePrivateProfileStringW(Widen(section).c_str(), Widen(key).c_str(), value ? Widen(value).c_str() : nullptr, p->iniPath.c_str());
    }

    // ---- threads ----
    void PostToMainThread(ModToolsPlugin* p, ModToolsCallback fn, void* user)
    {
        mt::PostTagged(p, [p, fn, user] { if (p->state == ModToolsPlugin::Loaded) { Cb c{ fn, user, p, "PostToMainThread" }; plugins::Call(p, "posted job", RunCb, &c); } });
    }
    int IsMainThread(ModToolsPlugin*) { return mt::IsMainThread() ? 1 : 0; }
    void* MainWindow(ModToolsPlugin*) { return mt::MainWindow(); }

    // ---- map ----
    size_t CurrentMapPath(ModToolsPlugin*, wchar_t* out, size_t cap) { return CopyOutW(hooks::CurrentMapPath(), out, cap); }
    int  PrefabDepth(ModToolsPlugin*) { return hooks::PrefabDepth(); }
    int  IsMapModified(ModToolsPlugin*) { return hooks::Modified(); }
    int  ReloadMap(ModToolsPlugin*, const wchar_t* path, int keepCamera) { return path && hooks::ReloadMapNow(path, keepCamera != 0) ? 1 : 0; }
    int  IsReloading(ModToolsPlugin*) { return hooks::IsReloading() ? 1 : 0; }
    void BeginOwnWrite(ModToolsPlugin*) { hooks::BeginOwnWrite(); }
    void EndOwnWrite(ModToolsPlugin*) { hooks::EndOwnWrite(); }
    int  OwnWriteRecent(ModToolsPlugin*) { return hooks::OwnWriteRecent() ? 1 : 0; }
    void InhibitReloadPrompts(ModToolsPlugin* p, int on) { p->inhibitReload = on != 0; }
    int  ReloadPromptsInhibited(ModToolsPlugin*) { return plugins::ReloadPromptsInhibited() ? 1 : 0; }

    // ---- camera ----
    int  GetCamera(ModToolsPlugin*, float o[3], float a[3]) { return hooks::GetCamera(o, a) ? 1 : 0; }
    int  SetCamera(ModToolsPlugin*, const float o[3], const float a[3]) { return hooks::SetCamera(o, a) ? 1 : 0; }
    int  GameView(ModToolsPlugin*) { return hooks::GameView() ? 1 : 0; }
    void SetGameView(ModToolsPlugin*, int on) { hooks::SetGameView(on != 0); }
    int  LastCamParams(ModToolsPlugin*, ModToolsCamParams* out)
    {
        if (!out) return 0;
        render::CamParams c = render::LastCam();
        memcpy(out->origin, c.origin, sizeof out->origin); memcpy(out->angles, c.angles, sizeof out->angles);
        out->width = c.width; out->height = c.height; out->gameView = c.gameView ? 1 : 0;
        return c.width > 0 ? 1 : 0;
    }
    void* CamWindow(ModToolsPlugin*) { return render::CamWindow(); }
    void* H_XYWindow(ModToolsPlugin*, int type) { return render::XYWindow(type); }
    void  H_MarkMapModified(ModToolsPlugin* p) { if (Ok(p) && rt::VerifyBinary()) hooks::MarkMapModified(); }

    // ---- patch vertices ----
    int H_PatchGrid(ModToolsPlugin* p, const char* guid, int* rows, int* cols, float* xyz, int cap)
    {
        if (!Ok(p) || !guid || !rt::VerifyBinary()) return 0;
        return hooks::PatchGrid(hooks::PatchBrushByGuid(guid, false), rows, cols, xyz, cap);
    }
    int H_PatchSetGrid(ModToolsPlugin* p, const char* guid, int rows, int cols, const float* xyz, int count)
    {
        if (!Ok(p) || !guid || !*guid || !rt::VerifyBinary()) return 0;
        return hooks::PatchSetGrid(hooks::PatchBrushByGuid(guid, false), rows, cols, xyz, count) ? 1 : 0;
    }
    int H_PatchSelectVerts(ModToolsPlugin* p, const ModToolsPatchVert* verts, int count)
    {
        if (!Ok(p) || !rt::VerifyBinary()) return -1;
        if (count < 0) return hooks::PatchSelectedVertCount();
        if (count > 0 && !verts) return -1;
        std::vector<std::pair<hooks::RadPtr, int>> refs;
        for (int i = 0; i < count; ++i)
        {
            if (!verts[i].brushGuid) return -1;
            hooks::RadPtr b = hooks::PatchBrushByGuid(verts[i].brushGuid, true);
            int rows = 0, cols = 0;
            if (!b || !hooks::PatchGrid(b, &rows, &cols, nullptr, 0)) return -2;
            if (verts[i].row < 0 || verts[i].row >= rows || verts[i].col < 0 || verts[i].col >= cols) return -3;
            refs.push_back({ b, verts[i].row * cols + verts[i].col });
        }
        return hooks::PatchSelectVerts(refs);
    }
    int H_PatchWeldVerts(ModToolsPlugin* p, const float xyz[3])
    {
        if (!Ok(p) || !rt::VerifyBinary()) return -1;
        LOG("host: %s welds %d patch vertices", p->name.c_str(), hooks::PatchSelectedVertCount());
        return hooks::PatchWeldSelected(xyz);
    }
    int H_DragMode(ModToolsPlugin* p) { return Ok(p) ? hooks::DragMode() : -1; }

    // ---- objects ----
    void ForEachEntity(ModToolsPlugin*, ModToolsEntityFn fn, void* user) { if (fn) hooks::ForEachEntity([&](hooks::RadPtr e) { fn(e, user); }); }
    void ForEachEntityBrush(ModToolsPlugin*, ModToolsEntity e, ModToolsBrushFn fn, void* user) { if (fn) hooks::ForEachEntityBrush(static_cast<hooks::RadPtr>(e), [&](hooks::RadPtr b) { fn(b, user); }); }
    int  EntityGuid(ModToolsPlugin*, ModToolsEntity e, char out[40]) { std::string g = hooks::EntityGuid(static_cast<hooks::RadPtr>(e)); if (g.empty() || g.size() >= 40) return 0; strcpy_s(out, 40, g.c_str()); return 1; }
    int  BrushGuid(ModToolsPlugin*, ModToolsBrush b, char out[40]) { std::string g = hooks::BrushGuid(static_cast<hooks::RadPtr>(b)); if (g.empty() || g.size() >= 40) return 0; strcpy_s(out, 40, g.c_str()); return 1; }
    ModToolsEntity BrushOwner(ModToolsPlugin*, ModToolsBrush b) { return hooks::BrushOwner(static_cast<hooks::RadPtr>(b)); }
    ModToolsEntity WorldEntity(ModToolsPlugin*) { return hooks::WorldEntity(); }
    size_t GetSelection(ModToolsPlugin*, ModToolsSelectedBrush* out, size_t cap)
    {
        std::vector<hooks::SelectedBrush> sel;
        hooks::GetSelection(sel);
        for (size_t i = 0; i < sel.size() && i < cap && out; ++i)
        {
            ModToolsSelectedBrush& o = out[i];
            strcpy_s(o.brushGuid, sizeof o.brushGuid, sel[i].brushGuid.substr(0, 39).c_str());
            strcpy_s(o.entityGuid, sizeof o.entityGuid, sel[i].entityGuid.substr(0, 39).c_str());
            memcpy(o.mins, sel[i].mins, sizeof o.mins); memcpy(o.maxs, sel[i].maxs, sizeof o.maxs);
        }
        return sel.size();
    }
    char* WriteEntityText(ModToolsPlugin*, ModToolsEntity e)
    {
        std::string text;
        if (!hooks::WriteEntityText(static_cast<hooks::RadPtr>(e), text)) return nullptr;
        char* s = static_cast<char*>(malloc(text.size() + 1));
        memcpy(s, text.data(), text.size()); s[text.size()] = 0;
        return s;
    }
    void FreeString(ModToolsPlugin*, char* s) { free(s); }
    int  ImportMapText(ModToolsPlugin*, const char* text, const char* label) { return text && hooks::ImportMapText(text, label ? label : "plugin") ? 1 : 0; }
    int  DeleteBrushes(ModToolsPlugin*, const ModToolsBrush* brushes, size_t n, const char* label)
    {
        std::vector<hooks::RadPtr> v; for (size_t i = 0; i < n; ++i) v.push_back(static_cast<hooks::RadPtr>(brushes[i]));
        return hooks::DeleteBrushes(v, label ? label : "plugin") ? 1 : 0;
    }
    size_t SelectedBrushInsts(ModToolsPlugin*, ModToolsBrushInst* out, size_t cap)
    {
        std::vector<hooks::RadPtr> v = hooks::SelectedBrushPtrs();
        for (size_t i = 0; i < v.size() && i < cap && out; ++i) out[i] = v[i];
        return v.size();
    }
    void SelectBrushInsts(ModToolsPlugin*, const ModToolsBrushInst* insts, size_t n)
    {
        std::vector<hooks::RadPtr> v; for (size_t i = 0; i < n; ++i) v.push_back(static_cast<hooks::RadPtr>(insts[i]));
        hooks::SelectBrushes(v);
    }
    void DeselectAll(ModToolsPlugin*) { hooks::DeselectAll(); }
    int  RestampImportedGuids(ModToolsPlugin*, const char* entityGuid, const char* const* brushGuids, size_t n)
    {
        std::vector<std::string> v; for (size_t i = 0; i < n; ++i) v.push_back(brushGuids[i] ? brushGuids[i] : "");
        return hooks::RestampImportedGuids(entityGuid ? entityGuid : "", v) ? 1 : 0;
    }
    size_t H_RestampImportedEntities(ModToolsPlugin* p, const char* const* entityGuids, size_t n, const char* const* brushGuids, const size_t* brushCounts)
    {
        if (!Ok(p) || !entityGuids || !brushCounts || !rt::VerifyBinary()) return 0;
        std::vector<std::string> ents;
        std::vector<std::vector<std::string>> brushes(n);
        size_t k = 0;
        for (size_t i = 0; i < n; ++i)
        {
            ents.push_back(entityGuids[i] ? entityGuids[i] : "");
            if (brushCounts[i] && !brushGuids) return 0;
            for (size_t j = 0; j < brushCounts[i]; ++j, ++k) brushes[i].push_back(brushGuids[k] ? brushGuids[k] : "");
        }
        return hooks::RestampImportedEntities(ents, brushes);
    }
    int  SelectEntityByGuid(ModToolsPlugin*, const char* guid) { return guid && hooks::SelectEntityByGuid(guid) ? 1 : 0; }
    int  EnterSelectedPrefab(ModToolsPlugin*) { return hooks::EnterSelectedPrefab() ? 1 : 0; }
    int  LeavePrefab(ModToolsPlugin*) { return hooks::LeavePrefab() ? 1 : 0; }

    // ---- overlay ----
    int  Project(ModToolsPlugin*, const float w[3], float* sx, float* sy) { float x, y; if (!render::ProjectCurrent(w, x, y)) return 0; if (sx) *sx = x; if (sy) *sy = y; return 1; }
    void DrawWorldLine(ModToolsPlugin*, const float a[3], const float b[3], uint32_t argb) { render::LineWorld(a, b, argb); }
    void DrawScreenLine(ModToolsPlugin*, float x0, float y0, float x1, float y1, uint32_t argb) { render::LineScreen(x0, y0, x1, y1, argb); }
    void DrawLabel(ModToolsPlugin*, float x, float y, const char* text, uint32_t argb, float px) { if (text) render::Text(x, y, text, argb, px); }
    void DrawWorldTriangle(ModToolsPlugin*, const float a[3], const float b[3], const float c[3], uint32_t argb) { render::TriangleWorld(a, b, c, argb); }
    void SetNativeModelPreview(ModToolsPlugin*, const ModToolsNativeModelPreview* preview) { render::SetNativeModelPreview(preview); }
    int NativeModelPreviewStatus(ModToolsPlugin*) { return render::NativeModelPreviewStatus(); }
    unsigned NativeModelPreviewDrawCount(ModToolsPlugin*) { return render::NativeModelPreviewDrawCount(); }
    void SetNativeModelPreviewSet(ModToolsPlugin*, const ModToolsNativeModelPreview* previews, size_t count) { render::SetNativeModelPreviewSet(previews, count); }
    int NativeModelPreviewStatusForId(ModToolsPlugin*, const char* requestId) { return render::NativeModelPreviewStatusForId(requestId); }
    unsigned NativeModelPreviewDrawCountForId(ModToolsPlugin*, const char* requestId) { return render::NativeModelPreviewDrawCountForId(requestId); }
    void SetNativeModelCullBounds(ModToolsPlugin*, const ModToolsNativeModelCullBounds* bounds, size_t count) { hooks::SetNativeModelCullBounds(bounds, count); }

    // ---- UI ----
    std::map<std::string, void*> g_menus;                 // title -> QMenu* (menus outlive plugin reloads)
    std::map<void*, ModToolsPlugin*> g_menuOwner;
    struct PaneRec { dock::Pane* pane; ModToolsPlugin* owner; bool wasVisible; };
    std::map<std::string, PaneRec> g_panes;               // title -> pane

    ModToolsPane CreatePane(ModToolsPlugin* p, const char* title, ModToolsWidget content, int w, int h)
    {
        if (!Ok(p) || !title || !content || !qt::Bind() || !dock::Ready()) return nullptr;
        dock::Pane* pane = dock::Host(content, title, w > 0 ? w : 400, h > 0 ? h : 500);
        if (!pane) return nullptr;
        auto it = g_panes.find(title);
        bool restore = it != g_panes.end() && it->second.wasVisible;
        g_panes[title] = PaneRec{ pane, p, false };
        p->panes.push_back(pane);
        if (restore) dock::Show(pane, true);
        return pane;
    }
    int  PaneVisible(ModToolsPlugin*, ModToolsPane pane) { return dock::Visible(static_cast<dock::Pane*>(pane)) ? 1 : 0; }
    void ShowPane(ModToolsPlugin*, ModToolsPane pane, int show) { dock::Show(static_cast<dock::Pane*>(pane), show != 0); }
    int  TabPaneWith(ModToolsPlugin*, ModToolsPane pane, const char* beside) { return dock::TabWith(static_cast<dock::Pane*>(pane), beside); }
    ModToolsWidget PaneToggleAction(ModToolsPlugin*, ModToolsPane pane) { return dock::ToggleAction(static_cast<dock::Pane*>(pane)); }

    ModToolsWidget AddMenu(ModToolsPlugin* p, const char* title)
    {
        if (!Ok(p) || !title || !qt::Bind()) return nullptr;
        void* mw = dock::MainWindowWidget();
        void* bar = mw ? qt::MenuBar(mw) : nullptr;
        if (!bar) return nullptr;
        void* menu = nullptr;
        auto it = g_menus.find(title);
        // A reload of the same plugin reuses its menu (one bar entry). A title owned by a DIFFERENT loaded plugin
        // gets its own menu: sharing one QMenu is fatal, ReleaseMenus clears it and deletes the other plugin's actions.
        auto owner = it != g_menus.end() ? g_menuOwner.find(it->second) : g_menuOwner.end();
        if (it != g_menus.end() && (owner == g_menuOwner.end() || owner->second == p))
        { menu = it->second; qt::AddAction(bar, qt::MenuAction(menu)); }
        else
        {
            if (it != g_menus.end())
                LOG("hostapi: %s: menu '%s' belongs to %s, adding a second one", p->name.c_str(), title,
                    owner != g_menuOwner.end() ? owner->second->name.c_str() : "another plugin");
            menu = qt::AddMenu(bar, title); if (!menu) return nullptr; g_menus[title] = menu;
        }
        g_menuOwner[menu] = p;
        p->menus.push_back(menu);
        return menu;
    }
    ModToolsWidget AddMenuItem(ModToolsPlugin* p, ModToolsWidget menu, const char* text, ModToolsCallback fn, void* user)
    {
        if (!Ok(p) || !menu || !text) return nullptr;
        void* action = qt::Action(text, menu);
        qt::AddAction(menu, action);
        if (fn)
        {
            Cb* c = NewCb(p, fn, user, "menu item");
            if (void* so = qt::Connect(action, qt::Signal::ActionTriggered, FireSlot, c)) p->slots.push_back(so);
            else LOG("hostapi: %s: could not connect menu item '%s'", p->name.c_str(), text);
        }
        return action;
    }
    void AddMenuAction(ModToolsPlugin*, ModToolsWidget menu, ModToolsWidget action) { if (menu && action) qt::AddAction(menu, action); }
    void AddMenuSeparator(ModToolsPlugin*, ModToolsWidget menu) { if (menu) qt::MenuAddSeparator(menu); }
    int  RegisterHotkey(ModToolsPlugin* p, int vk, int mods, ModToolsCallback fn, void* user)
    {
        if (!Ok(p) || !fn) return 0;
        Cb* c = NewCb(p, fn, user, "hotkey");
        return ui::RegisterHotkey(p, vk, mods, FireCb, c);
    }
    ModToolsWidget MakeEventFilter(ModToolsPlugin* p, ModToolsEventFilterFn fn, void* user)
    {
        if (!Ok(p) || !fn || !qt::Bind()) return nullptr;
        auto* c = static_cast<FilterCb*>(malloc(sizeof(FilterCb)));
        c->fn = fn; c->user = user; c->p = p; c->watched = nullptr; c->type = 0; c->result = 0;
        p->callbacks.push_back(c);
        void* f = qt::MakeEventFilter(FilterThunk, c);
        if (f) p->filters.push_back(f);
        return f;
    }
    // OnChanged: one ValueCb per connection, code >= 0 is a fixed value (tree events), -1 reads int* args[1], -2 bool* args[1]
    struct ValueCb { ModToolsValueFn fn; void* user; ModToolsPlugin* p; int code; int value; };
    void RunValue(void* a) { auto* c = static_cast<ValueCb*>(a); c->fn(c->user, c->value); }
    void FireValue(void* user, void** args)
    {
        auto* c = static_cast<ValueCb*>(user);
        if (c->p->state != ModToolsPlugin::Loaded) return;
        c->value = c->code >= 0 ? c->code : !args || !args[1] ? 0 : c->code == -1 ? *static_cast<int*>(args[1]) : (*static_cast<bool*>(args[1]) ? 1 : 0);
        plugins::Call(c->p, "value change", RunValue, c);
    }
    int  OnChanged(ModToolsPlugin* p, ModToolsWidget w, ModToolsValueFn fn, void* user)
    {
        if (!Ok(p) || !w || !fn || !qt::PanelBound()) return 0;
        auto connect = [&](void* sender, qt::Signal sig, int code) {
            auto* c = static_cast<ValueCb*>(malloc(sizeof(ValueCb)));
            c->fn = fn; c->user = user; c->p = p; c->code = code; c->value = 0;
            p->callbacks.push_back(c);
            void* so = sender ? qt::Connect(sender, sig, FireValue, c) : nullptr;
            if (so) p->slots.push_back(so);
            return so != nullptr;
        };
        if (qt::IsA(w, "QAbstractSlider")) return connect(w, qt::Signal::SliderValueChanged, -1) ? 1 : 0;
        if (qt::IsA(w, "QComboBox")) return connect(w, qt::Signal::ComboIndexChanged, -1) ? 1 : 0;
        if (qt::IsA(w, "QTabWidget")) return connect(w, qt::Signal::TabCurrentChanged, -1) ? 1 : 0;
        if (qt::IsA(w, "QListWidget")) return connect(w, qt::Signal::ListCurrentRowChanged, -1) ? 1 : 0;
        if (qt::IsA(w, "QAbstractButton")) return connect(w, qt::Signal::ButtonToggled, -2) ? 1 : 0;
        if (qt::IsA(w, "QLineEdit")) return connect(w, qt::Signal::LineEditingFinished, 0) ? 1 : 0;
        if (qt::IsA(w, "QTreeWidget"))
        {
            void* model = qt::ItemViewModel(w);
            bool ok = connect(w, qt::Signal::TreeCurrentItemChanged, 0) & connect(w, qt::Signal::TreeItemChanged, 1)
                    & connect(model, qt::Signal::ModelRowsInserted, 2) & connect(model, qt::Signal::ModelRowsRemoved, 2);
            return ok ? 1 : 0;
        }
        return 0;
    }
    int  OnClicked(ModToolsPlugin* p, ModToolsWidget button, ModToolsCallback fn, void* user)
    {
        if (!Ok(p) || !button || !fn) return 0;
        Cb* c = NewCb(p, fn, user, "button click");
        void* so = qt::Connect(button, qt::Signal::ButtonClicked, FireSlot, c);
        if (so) p->slots.push_back(so);
        return so ? 1 : 0;
    }

    // ---- other plugins ----
    int PluginLoaded(ModToolsPlugin*, const char* name) { ModToolsPlugin* q = name ? plugins::Find(name) : nullptr; return q && q->state == ModToolsPlugin::Loaded ? 1 : 0; }
    const char* Command(ModToolsPlugin*, const char* name, const char* cmd) { return name && cmd ? plugins::CommandTo(plugins::Find(name), cmd) : nullptr; }

    // ---- raw ----
    void* RadiantAddress(ModToolsPlugin*, uintptr_t ida) { return rt::VerifyBinary() ? reinterpret_cast<void*>(rt::Rva(ida)) : nullptr; }
    // Every MH_EnableHook suspends all editor threads (about 50 ms in Radiant): detours made while a plugin loads are queued and enabled together
    int  g_detourBatch = 0;         // nesting depth, main thread
    int  g_detoursQueued = 0;
    int   Detour(ModToolsPlugin* p, void* target, void* detour, void** original)
    {
        if (!Ok(p) || !target || !detour) return 0;
        if (MH_CreateHook(target, detour, original) != MH_OK) return 0;
        bool queue = g_detourBatch > 0 && mt::IsMainThread();
        if ((queue ? MH_QueueEnableHook(target) : MH_EnableHook(target)) != MH_OK) { MH_RemoveHook(target); return 0; }
        if (queue) ++g_detoursQueued;
        p->detours.push_back(target);
        return 1;
    }
    ModToolsBrushInst BrushInstOf(ModToolsPlugin*, ModToolsBrush b) { return hooks::FindBrushInst(static_cast<hooks::RadPtr>(b)); }
    int App(ModToolsPlugin*) { return app::ApiId(); }
    int H_TriggerMenuAction(ModToolsPlugin* p, const char* path)
    {
        if (!Ok(p) || !path || !qt::Bind()) return 0;
        void* mw = dock::MainWindowWidget();
        void* a = mw ? qt::FindMenuAction(mw, path) : nullptr;
        if (!a) return 0;
        if (!qt::ActionEnabled(a)) return -1;
        LOG("host: %s triggers menu action '%s'", p->name.c_str(), path);
        qt::TriggerAction(a);
        return 1;
    }
    size_t H_MenuEntries(ModToolsPlugin* p, const char* path, char* out, size_t cap)
    {
        if (!Ok(p) || !qt::Bind()) return 0;
        void* mw = dock::MainWindowWidget();
        return CopyOut(mw ? qt::DescribeMenu(mw, path ? path : "") : std::string(), out, cap);
    }
    int  H_MeasureLabel(ModToolsPlugin*, const char* text, float px, float* width, float* height)
    {
        float w = 0, h = 0;
        bool engine = rt::VerifyBinary() && render::MeasureText(text ? text : "", px, 1, w, h);
        if (width) *width = w;
        if (height) *height = h;
        return engine ? 1 : 0;
    }
    void H_BeginUndo(ModToolsPlugin* p, const char* label) { if (Ok(p) && rt::VerifyBinary()) hooks::BeginUndo(label); }
    void H_EndUndo(ModToolsPlugin* p) { if (Ok(p) && rt::VerifyBinary()) hooks::EndUndo(); }
    void H_RequestRedraw(ModToolsPlugin* p, unsigned views)
    {
        if (!Ok(p)) return;
        unsigned bits = 0;
        if (views & MODTOOLS_VIEW_3D) bits |= rt::kRedraw3D;
        if (views & MODTOOLS_VIEW_2D) bits |= rt::kRedraw2D;
        hooks::RequestRedraw(bits);
    }
    int  H_CamXYUpdateOnDrag(ModToolsPlugin* p) { return Ok(p) && hooks::CamXYUpdateOnDrag() ? 1 : 0; }
    void H_SetCameraLutPreview(ModToolsPlugin* p, const ModToolsCameraLutPreview* preview)
    {
        if (Ok(p)) render::SetCameraLutPreview(preview);
    }
    int H_CameraLutPreviewStatus(ModToolsPlugin* p)
    {
        return Ok(p) ? render::CameraLutPreviewStatus() : MODTOOLS_CAMERA_LUT_PREVIEW_OFF;
    }
    int  H_MoveBrush(ModToolsPlugin* p, ModToolsBrush b, const float delta[3])
    {
        return Ok(p) && rt::VerifyBinary() && hooks::MoveBrush(static_cast<hooks::RadPtr>(b), delta) ? 1 : 0;
    }
    size_t H_FindBrushesByGuid(ModToolsPlugin* p, const char* const* guids, size_t n, ModToolsBrush* out)
    {
        if (!Ok(p) || !guids || !out || !rt::VerifyBinary()) return 0;
        std::vector<std::string> keys; keys.reserve(n);
        for (size_t i = 0; i < n; ++i) keys.push_back(guids[i] ? guids[i] : "");
        std::vector<hooks::RadPtr> found;
        size_t count = hooks::FindBrushesByGuid(keys, found);
        for (size_t i = 0; i < n; ++i) out[i] = i < found.size() ? found[i] : nullptr;
        return count;
    }
    size_t H_FindEntitiesByGuid(ModToolsPlugin* p, const char* const* guids, size_t n, ModToolsEntity* out)
    {
        if (!Ok(p) || !guids || !out || !rt::VerifyBinary()) return 0;
        std::vector<std::string> keys; keys.reserve(n);
        for (size_t i = 0; i < n; ++i) keys.push_back(guids[i] ? guids[i] : "");
        std::vector<hooks::RadPtr> found;
        size_t count = hooks::FindEntitiesByGuid(keys, found);
        for (size_t i = 0; i < n; ++i) out[i] = i < found.size() ? found[i] : nullptr;
        return count;
    }
    size_t H_BrushPlanes(ModToolsPlugin* p, ModToolsBrush b, float* out, size_t capFaces)
    {
        return Ok(p) && rt::VerifyBinary() ? hooks::BrushPlanes(static_cast<hooks::RadPtr>(b), out, capFaces) : 0;
    }
    int  H_SetBrushPlanes(ModToolsPlugin* p, ModToolsBrush b, const float* pts, size_t nFaces)
    {
        return Ok(p) && rt::VerifyBinary() && hooks::SetBrushPlanes(static_cast<hooks::RadPtr>(b), pts, nFaces) ? 1 : 0;
    }
    int  H_CommitBrushGeometry(ModToolsPlugin* p, ModToolsBrush b, const float* pts, size_t nFaces, const float delta[3], const char* label)
    {
        return Ok(p) && rt::VerifyBinary() && hooks::CommitBrushGeometry(static_cast<hooks::RadPtr>(b), pts, nFaces, delta, label ? label : "Place brush") ? 1 : 0;
    }
    int  H_EntityFixedSize(ModToolsPlugin* p, ModToolsEntity e)
    {
        return Ok(p) && rt::VerifyBinary() && hooks::EntityFixedSize(static_cast<hooks::RadPtr>(e)) ? 1 : 0;
    }
    int H_PrefabStackPlacement(ModToolsPlugin* p, int level, float origin[3], float angles[3])
    {
        return Ok(p) && rt::VerifyBinary() && origin && angles && hooks::PrefabStackPlacement(level, origin, angles) ? 1 : 0;
    }
    int H_PerformanceProfile(ModToolsPlugin* p, int action, ModToolsHostPerfSnapshot* out)
    {
        return Ok(p) && rt::VerifyBinary() ? hooks::PerformanceProfile(action, out) : 0;
    }
    ModToolsBrush H_BrushOfInst(ModToolsPlugin* p, ModToolsBrushInst inst)
    {
        return Ok(p) && rt::VerifyBinary() ? hooks::BrushOfInst(static_cast<hooks::RadPtr>(inst)) : nullptr;
    }
    char* H_WriteEntityTextWith(ModToolsPlugin* p, ModToolsEntity e, const ModToolsBrush* brushes, size_t n)
    {
        if (!Ok(p) || !rt::VerifyBinary()) return nullptr;
        std::vector<hooks::RadPtr> extra; for (size_t i = 0; i < n && brushes; ++i) extra.push_back(static_cast<hooks::RadPtr>(brushes[i]));
        std::string text;
        if (!hooks::WriteEntityTextWith(static_cast<hooks::RadPtr>(e), extra, text)) return nullptr;
        char* s = static_cast<char*>(malloc(text.size() + 1));
        memcpy(s, text.data(), text.size()); s[text.size()] = 0;
        return s;
    }
    void H_BeginUntracked(ModToolsPlugin* p) { if (Ok(p) && rt::VerifyBinary()) hooks::BeginUntracked(); }
    void H_EndUntracked(ModToolsPlugin* p) { if (Ok(p) && rt::VerifyBinary()) hooks::EndUntracked(); }
    int  H_GetUndoTags(ModToolsPlugin* p, void* o, int isEntity, int tags[3])
    {
        return Ok(p) && rt::VerifyBinary() && hooks::GetUndoTags(static_cast<hooks::RadPtr>(o), isEntity != 0, tags) ? 1 : 0;
    }
    int  H_SetUndoTags(ModToolsPlugin* p, void* o, int isEntity, const int tags[3])
    {
        return Ok(p) && rt::VerifyBinary() && hooks::SetUndoTags(static_cast<hooks::RadPtr>(o), isEntity != 0, tags) ? 1 : 0;
    }
    int  H_BrushBounds(ModToolsPlugin* p, ModToolsBrush b, float mins[3], float maxs[3])
    {
        return Ok(p) && rt::VerifyBinary() && hooks::BrushBounds(static_cast<hooks::RadPtr>(b), mins, maxs) ? 1 : 0;
    }
    ModToolsWidget H_CreateDialogWindow(ModToolsPlugin* p, const char* title, ModToolsWidget content, int w, int h)
    {
        if (!Ok(p) || !title || !content || !qt::Bind()) return nullptr;
        void* mw = dock::MainWindowWidget();
        void* d = qt::Dialog(title, mw);
        void* box = qt::VBox(d);
        qt::LayoutMargins(box, 0, 0, 0, 0);
        qt::BoxAddWidget(box, content, 1);
        qt::Resize(d, w > 0 ? w : 480, h > 0 ? h : 320);
        RECT r{}; HWND main = mt::MainWindow();
        if (main && GetWindowRect(main, &r)) qt::Move(d, (r.left + r.right - (w > 0 ? w : 480)) / 2, (r.top + r.bottom - (h > 0 ? h : 320)) / 2);
        p->dialogs.push_back(d);
        return d;
    }

    // ---- APE table (see ape.cpp) ----
    void CopyAsset(const ape::Asset& a, ModToolsApeAsset* out)
    {
        if (!out) return;
        strncpy_s(out->name, sizeof out->name, a.name.c_str(), _TRUNCATE);
        strncpy_s(out->type, sizeof out->type, a.type.c_str(), _TRUNCATE);
    }
    int    A_CurrentAsset(ModToolsPlugin*, ModToolsApeAsset* out) { ape::Asset a; if (!ape::CurrentAsset(a)) return 0; CopyAsset(a, out); return 1; }
    size_t A_SelectedAssets(ModToolsPlugin*, ModToolsApeAsset* out, size_t cap)
    {
        std::vector<ape::Asset> v = ape::SelectedAssets();
        for (size_t i = 0; i < v.size() && i < cap && out; ++i) CopyAsset(v[i], &out[i]);
        return v.size();
    }
    int    A_TabCount(ModToolsPlugin*) { return ape::TabCount(); }
    int    A_TabAsset(ModToolsPlugin*, int index, ModToolsApeAsset* out) { ape::Asset a; if (!ape::TabAsset(index, a)) return 0; CopyAsset(a, out); return 1; }
    void   A_Print(ModToolsPlugin* p, const char* text) { if (Ok(p) && text) ape::Print(text); }
    size_t A_GdtDbPath(ModToolsPlugin*, wchar_t* out, size_t cap) { return CopyOutW(ape::GdtDbPath(), out, cap); }
    size_t A_AssetTypes(ModToolsPlugin*, const char* name, char* out, size_t cap)
    {
        if (!name) return 0;
        std::vector<std::string> t = ape::AssetTypes(name);
        std::string joined; for (size_t i = 0; i < t.size(); ++i) joined += (i ? "," : "") + t[i];
        CopyOut(joined, out, cap);
        return t.size();
    }
    size_t A_FindAssets(ModToolsPlugin*, const char* namePart, const char* type, ModToolsApeAsset* out, size_t cap)
    {
        if (!out || !cap) return 0;
        std::vector<ape::Asset> v = ape::FindAssets(namePart ? namePart : "", type ? type : "", cap);
        for (size_t i = 0; i < v.size(); ++i) CopyAsset(v[i], out + i);
        return v.size();
    }
    size_t A_AssetTypeNames(ModToolsPlugin*, char* out, size_t cap)
    {
        std::vector<std::string> t = ape::AssetTypeNames();
        std::string joined; for (size_t i = 0; i < t.size(); ++i) joined += (i ? "," : "") + t[i];
        CopyOut(joined, out, cap);
        return t.size();
    }
    int    A_AssetInfo(ModToolsPlugin*, const char* name, const char* type, ModToolsApeAssetInfo* out)
    {
        if (!name || !out) return 0;
        ape::AssetInfo info;
        if (!ape::LookupAsset(name, type ? type : "", info)) return 0;
        memset(out, 0, sizeof *out);
        CopyOutW(info.gdtPath, out->gdtPath, sizeof out->gdtPath / sizeof(wchar_t));
        out->line = info.line;
        strncpy_s(out->parentName, sizeof out->parentName, info.parentName.c_str(), _TRUNCATE);
        strncpy_s(out->parentType, sizeof out->parentType, info.parentType.c_str(), _TRUNCATE);
        return 1;
    }
    size_t A_TypeProperties(ModToolsPlugin*, const char* type, char* out, size_t cap) { return CopyOut(type ? ape::TypeProperties(type) : std::string(), out, cap); }
    size_t A_OutputText(ModToolsPlugin*, char* out, size_t cap) { return CopyOut(ape::OutputText(), out, cap); }
    size_t A_Properties(ModToolsPlugin*, char* out, size_t cap)
    {
        std::string text; size_t i = 0;
        for (const ape::Prop& p : ape::Properties())
        {
            auto clean = [](std::string s) { for (auto& ch : s) if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' '; return s; };
            text += std::to_string(i++) + "\t" + clean(p.category) + "\t" + clean(p.title) + "\t" + p.kind + "\t" + clean(p.value) + "\t" + clean(p.tooltip) + "\t" + (p.visible ? "1" : "0") + "\t" + (p.enabled ? "1" : "0") + "\n";
        }
        return CopyOut(text, out, cap);
    }
    int    A_SetProperty(ModToolsPlugin* p, const char* key, const char* value, char* why, size_t whyCap)
    {
        if (!Ok(p) || !key) return 0;
        std::string reason;
        bool ok = ape::SetProperty(key, value ? value : "", reason);
        if (!ok) CopyOut(reason, why, whyCap);
        LOG("host: %s set APE property '%s' = '%s' -> %d %s", p->name.c_str(), key, value ? value : "", ok, reason.c_str());
        return ok ? 1 : 0;
    }
    int    A_OpenAsset(ModToolsPlugin* p, const char* name, const char* type, char* why, size_t whyCap)
    {
        if (!Ok(p) || !name || !type) return 0;
        std::string reason;
        bool ok = ape::OpenAsset(name, type, reason);
        if (!ok) CopyOut(reason, why, whyCap);
        return ok ? 1 : 0;
    }
    void*  A_ApeAddress(ModToolsPlugin*, uintptr_t ida) { return app::VerifyApeBinary() ? reinterpret_cast<void*>(app::ModuleBase() + (ida - 0x140000000ULL)) : nullptr; }

    ModToolsApeApi g_ape = {
        sizeof(ModToolsApeApi),
        A_CurrentAsset, A_SelectedAssets, A_TabCount, A_TabAsset, A_Print, A_GdtDbPath, A_AssetTypes, A_ApeAddress,
        A_FindAssets, A_AssetTypeNames, A_AssetInfo, A_TypeProperties, A_OutputText, A_Properties, A_SetProperty, A_OpenAsset,
    };

    // ---- widget helpers (plain wrappers) ----
    ModToolsWidget W_Widget(ModToolsWidget parent) { return qt::Widget(parent); }
    ModToolsWidget W_Label(const char* t, ModToolsWidget parent) { return qt::Label(t ? t : "", parent); }
    ModToolsWidget W_LineEdit(const char* t, ModToolsWidget parent) { return qt::LineEdit(t ? t : "", parent); }
    ModToolsWidget W_PushButton(const char* t, ModToolsWidget parent) { return qt::PushButton(t ? t : "", parent); }
    ModToolsWidget W_CheckBox(const char* t, ModToolsWidget parent) { return qt::CheckBox(t ? t : "", parent); }
    ModToolsWidget W_RadioButton(const char* t, ModToolsWidget parent) { return qt::RadioButton(t ? t : "", parent); }
    ModToolsWidget W_ListWidget(ModToolsWidget parent) { return qt::ListWidget(parent); }
    ModToolsWidget W_PlainTextEdit(ModToolsWidget parent) { return qt::PlainTextEdit(parent); }
    ModToolsWidget W_VBox(ModToolsWidget parent) { return qt::VBox(parent); }
    ModToolsWidget W_HBox(void) { return qt::HBox(); }
    ModToolsWidget W_Grid(void) { return qt::Grid(); }
    void W_BoxAddWidget(ModToolsWidget b, ModToolsWidget w, int s) { qt::BoxAddWidget(b, w, s); }
    void W_BoxAddLayout(ModToolsWidget b, ModToolsWidget l, int s) { qt::BoxAddLayout(b, l, s); }
    void W_BoxAddStretch(ModToolsWidget b, int s) { qt::BoxAddStretch(b, s); }
    void W_GridAddWidget(ModToolsWidget g, ModToolsWidget w, int r, int c) { qt::GridAddWidget(g, w, r, c); }
    void W_GridAddLayout(ModToolsWidget g, ModToolsWidget l, int r, int c) { qt::GridAddLayout(g, l, r, c); }
    void W_GridColumnStretch(ModToolsWidget g, int c, int s) { qt::GridColumnStretch(g, c, s); }
    void W_LayoutMargins(ModToolsWidget l, int a, int b, int c, int d) { qt::LayoutMargins(l, a, b, c, d); }
    void W_LayoutSpacing(ModToolsWidget l, int s) { qt::LayoutSpacing(l, s); }
    void W_SetLayout(ModToolsWidget w, ModToolsWidget l) { qt::SetLayout(w, l); }
    void W_SetLabelText(ModToolsWidget w, const char* t) { qt::SetLabelText(w, t ? t : ""); }
    void W_SetWordWrap(ModToolsWidget w, int on) { qt::SetWordWrap(w, on != 0); }
    void W_SetButtonText(ModToolsWidget w, const char* t) { qt::SetButtonText(w, t ? t : ""); }
    void W_SetEditText(ModToolsWidget w, const char* t) { qt::SetEditText(w, t ? t : ""); }
    size_t W_EditText(ModToolsWidget w, char* out, size_t cap) { return CopyOut(qt::EditText(w), out, cap); }
    void W_SetPlaceholder(ModToolsWidget w, const char* t) { qt::SetPlaceholder(w, t ? t : ""); }
    void W_SetChecked(ModToolsWidget w, int on) { qt::SetChecked(w, on != 0); }
    int  W_IsChecked(ModToolsWidget w) { return qt::IsChecked(w) ? 1 : 0; }
    int  W_IsDown(ModToolsWidget w) { return qt::IsDown(w) ? 1 : 0; }
    void W_SetEnabled(ModToolsWidget w, int on) { qt::SetEnabled(w, on != 0); }
    void W_SetVisible(ModToolsWidget w, int on) { qt::SetVisible(w, on != 0); }
    void W_SetReadOnly(ModToolsWidget w, int on) { qt::SetReadOnly(w, on != 0); }
    void W_ListClear(ModToolsWidget w) { qt::ListClear(w); }
    void W_ListAdd(ModToolsWidget w, const char* t) { qt::ListAdd(w, t ? t : ""); }
    int  W_ListCount(ModToolsWidget w) { return qt::ListCount(w); }
    int  W_ListCurrentRow(ModToolsWidget w) { return qt::ListCurrentRow(w); }
    void W_ListSetCurrentRow(ModToolsWidget w, int r) { qt::ListSetCurrentRow(w, r); }
    void W_ListSetItemText(ModToolsWidget w, int r, const char* t) { qt::ListSetItemText(w, r, t ? t : ""); }
    void W_ListSetItemCheckable(ModToolsWidget w, int r, int on) { qt::ListSetItemCheckable(w, r, on != 0); }
    void W_ListSetItemChecked(ModToolsWidget w, int r, int on) { qt::ListSetItemChecked(w, r, on != 0); }
    int  W_ListItemChecked(ModToolsWidget w, int r) { return qt::ListItemChecked(w, r) ? 1 : 0; }
    void W_ListSetItemToolTip(ModToolsWidget w, int r, const char* t) { qt::ListSetItemToolTip(w, r, t ? t : ""); }
    void W_PlainSetText(ModToolsWidget w, const char* t) { qt::PlainSetText(w, t ? t : ""); }
    void W_PlainScrollToEnd(ModToolsWidget w) { qt::PlainScrollToEnd(w); }
    void W_SetMinimumHeight(ModToolsWidget w, int h) { qt::SetMinimumHeight(w, h); }
    void W_SetMaximumHeight(ModToolsWidget w, int h) { qt::SetMaximumHeight(w, h); }
    void W_SetMaximumWidth(ModToolsWidget w, int x) { qt::SetMaximumWidth(w, x); }
    void W_SetMinimumSize(ModToolsWidget w, int x, int y) { qt::SetMinimumSize(w, x, y); }
    void W_SetToolTip(ModToolsWidget w, const char* t) { qt::SetToolTip(w, t ? t : ""); }
    void W_SetFocus(ModToolsWidget w) { qt::SetFocus(w); }
    int  W_CursorInside(ModToolsWidget w) { return qt::CursorInside(w) ? 1 : 0; }
    void W_InstallFilter(ModToolsWidget o, ModToolsWidget f) { if (o && f) qt::InstallFilter(o, f); }
    void W_RemoveFilter(ModToolsWidget o, ModToolsWidget f) { if (o && f) qt::RemoveFilter(o, f); }
    void W_SetActionEnabled(ModToolsWidget a, int on) { qt::SetActionEnabled(a, on != 0); }
    void W_SetActionText(ModToolsWidget a, const char* t) { qt::SetActionText(a, t ? t : ""); }
    int  W_PickColor(ModToolsWidget parent, uint32_t initialArgb, const char* title, uint32_t* outArgb)
    {
        uint32_t c = 0;
        if (!qt::PickColor(parent, initialArgb, title, c)) return 0;
        if (outArgb) *outArgb = c;
        return 1;
    }
    int  W_SetEchoMode(ModToolsWidget w, int mode) { return qt::SetEchoMode(w, mode) ? 1 : 0; }
    // panel widgets
    ModToolsWidget W_Slider(int horizontal, int mn, int mx, ModToolsWidget parent) { return qt::Slider(horizontal != 0, mn, mx, parent); }
    void W_SliderSetRange(ModToolsWidget w, int mn, int mx) { qt::SliderSetRange(w, mn, mx); }
    void W_SliderSetValue(ModToolsWidget w, int v) { qt::SliderSetValue(w, v); }
    int  W_SliderValue(ModToolsWidget w) { return qt::SliderValue(w); }
    ModToolsWidget W_ComboBox(ModToolsWidget parent) { return qt::ComboBox(parent); }
    void W_ComboAdd(ModToolsWidget w, const char* t) { qt::ComboAdd(w, t); }
    void W_ComboClear(ModToolsWidget w) { qt::ComboClear(w); }
    int  W_ComboCurrent(ModToolsWidget w) { return w && qt::PanelBound() ? qt::ComboCurrentIndex(w) : -1; }
    void W_ComboSetCurrent(ModToolsWidget w, int i) { if (w && qt::PanelBound()) qt::ComboSetCurrentIndex(w, i); }
    ModToolsWidget W_TabWidget(ModToolsWidget parent) { return qt::TabWidget(parent); }
    int  W_TabAdd(ModToolsWidget tabs, ModToolsWidget page, const char* title) { return qt::TabAdd(tabs, page, title); }
    int  W_TabCurrent(ModToolsWidget tabs) { return qt::TabCurrent(tabs); }
    void W_TabSetCurrent(ModToolsWidget tabs, int i) { qt::TabSetCurrent(tabs, i); }
    ModToolsWidget W_ScrollArea(ModToolsWidget content, ModToolsWidget parent) { return qt::ScrollArea(content, parent); }
    void W_ClickButton(ModToolsWidget b) { if (b && qt::Bound() && qt::IsA(b, "QAbstractButton")) qt::Click(b); }
    void W_SetActionChecked(ModToolsWidget a, int checkable, int on) { if (a && qt::Bound()) qt::SetActionChecked(a, checkable != 0, on != 0); }
    ModToolsWidget W_Splitter(int horizontal, ModToolsWidget parent) { return qt::Splitter(horizontal != 0, parent); }
    void W_SplitterAdd(ModToolsWidget sp, ModToolsWidget w, int stretch) { qt::SplitterAdd(sp, w, stretch); }
    ModToolsWidget W_TreeWidget(int columns, ModToolsWidget parent) { return qt::TreeWidget(columns, parent); }
    void W_TreeClear(ModToolsWidget t) { qt::TreeClear(t); }
    ModToolsTreeItem W_TreeAdd(ModToolsWidget t, ModToolsTreeItem parent, const char* cols) { return qt::TreeAddItem(t, parent, cols); }
    void W_TreeRemove(ModToolsWidget t, ModToolsTreeItem item) { qt::TreeRemoveItem(t, item); }
    int  W_TreeChildCount(ModToolsWidget t, ModToolsTreeItem item) { return qt::TreeChildCount(t, item); }
    ModToolsTreeItem W_TreeChild(ModToolsWidget t, ModToolsTreeItem item, int i) { return qt::TreeChild(t, item, i); }
    ModToolsTreeItem W_TreeParent(ModToolsTreeItem item) { return item && qt::PanelBound() ? qt::TreeItemParent(item) : nullptr; }
    ModToolsTreeItem W_TreeCurrent(ModToolsWidget t) { return t && qt::PanelBound() ? qt::TreeCurrentItem(t) : nullptr; }
    void W_TreeSetCurrent(ModToolsWidget t, ModToolsTreeItem item) { qt::TreeSetCurrent(t, item); }
    size_t W_TreeItemText(ModToolsTreeItem item, int column, char* out, size_t cap)
    {
        std::string s = item && qt::PanelBound() ? qt::TreeItemText(item, column) : std::string();
        if (out && cap) { size_t n = s.size() < cap - 1 ? s.size() : cap - 1; memcpy(out, s.data(), n); out[n] = 0; }
        return s.size();
    }
    void W_TreeSetItemText(ModToolsTreeItem item, int column, const char* t) { qt::TreeSetItemText(item, column, t); }
    void W_TreeSetItemId(ModToolsTreeItem item, int64_t id) { qt::TreeSetItemId(item, id); }
    int64_t W_TreeItemId(ModToolsTreeItem item) { return qt::TreeItemId(item); }
    void W_TreeSetItemCheckable(ModToolsTreeItem item, int on) { qt::TreeSetItemCheckable(item, on != 0); }
    void W_TreeSetItemChecked(ModToolsTreeItem item, int on) { qt::TreeSetItemChecked(item, on != 0); }
    int  W_TreeItemChecked(ModToolsTreeItem item) { return qt::TreeItemChecked(item) ? 1 : 0; }
    void W_TreeSetExpanded(ModToolsTreeItem item, int on) { qt::TreeSetExpanded(item, on != 0); }
    void W_TreeSetDragDrop(ModToolsWidget t, int on) { qt::TreeSetDragDrop(t, on != 0); }
    ModToolsWidget W_FindEditorWidget(const char* name)
    {
        void* main = name && *name && qt::Bound() ? dock::MainWindowWidget() : nullptr;
        return main ? qt::FindChildByName(main, name) : nullptr;
    }
    void* FindChildDepthFirst(void* parent, const char* cls, int& index)
    {
        for (void* c : qt::Children(parent))
        {
            if (qt::IsA(c, cls) && index-- == 0) return c;
            if (void* found = FindChildDepthFirst(c, cls, index)) return found;
        }
        return nullptr;
    }
    ModToolsWidget W_FindChild(ModToolsWidget parent, const char* cls, int index)
    {
        if (!parent || !cls || index < 0 || !qt::Bound()) return nullptr;
        return FindChildDepthFirst(parent, cls, index);
    }
    double W_WidgetNumber(ModToolsWidget w, int* ok)
    {
        if (ok) *ok = 0;
        if (!w || !qt::PanelBound()) return 0;
        double v = 0; bool have = true;
        if (qt::IsA(w, "QAbstractSlider")) v = qt::SliderValue(w);
        else if (qt::IsA(w, "QDoubleSpinBox")) v = qt::DoubleSpinValue(w);
        else if (qt::IsA(w, "QSpinBox")) v = qt::SpinValue(w);
        else if (qt::IsA(w, "QComboBox")) v = qt::ComboCurrentIndex(w);
        else if (qt::IsA(w, "QAbstractButton")) v = qt::IsChecked(w) ? 1 : 0;
        else if (qt::IsA(w, "QLineEdit")) { std::string t = qt::EditText(w); char* end = nullptr; v = strtod(t.c_str(), &end); have = end && end != t.c_str(); }
        else have = false;
        if (ok) *ok = have ? 1 : 0;
        return have ? v : 0;
    }

    ModToolsUiApi g_ui = {
        sizeof(ModToolsUiApi),
        W_Widget, W_Label, W_LineEdit, W_PushButton, W_CheckBox, W_RadioButton, W_ListWidget, W_PlainTextEdit,
        W_VBox, W_HBox, W_Grid, W_BoxAddWidget, W_BoxAddLayout, W_BoxAddStretch, W_GridAddWidget, W_GridAddLayout, W_GridColumnStretch,
        W_LayoutMargins, W_LayoutSpacing, W_SetLayout,
        W_SetLabelText, W_SetWordWrap, W_SetButtonText, W_SetEditText, W_EditText, W_SetPlaceholder, W_SetChecked, W_IsChecked, W_IsDown,
        W_SetEnabled, W_SetVisible, W_SetReadOnly, W_ListClear, W_ListAdd, W_ListCount, W_ListCurrentRow, W_ListSetCurrentRow, W_ListSetItemText,
        W_ListSetItemCheckable, W_ListSetItemChecked, W_ListItemChecked, W_ListSetItemToolTip, W_PlainSetText, W_PlainScrollToEnd,
        W_SetMinimumHeight, W_SetMaximumHeight, W_SetMaximumWidth, W_SetMinimumSize, W_SetToolTip, W_SetFocus, W_CursorInside,
        W_InstallFilter, W_RemoveFilter, W_SetActionEnabled, W_SetActionText,
        W_PickColor,
        W_SetEchoMode,
        W_Slider, W_SliderSetRange, W_SliderSetValue, W_SliderValue, W_ComboBox, W_ComboAdd, W_ComboClear, W_ComboCurrent, W_ComboSetCurrent,
        W_TabWidget, W_TabAdd, W_TabCurrent, W_TabSetCurrent, W_Splitter, W_SplitterAdd,
        W_TreeWidget, W_TreeClear, W_TreeAdd, W_TreeRemove, W_TreeChildCount, W_TreeChild, W_TreeParent, W_TreeCurrent, W_TreeSetCurrent,
        W_TreeItemText, W_TreeSetItemText, W_TreeSetItemId, W_TreeItemId, W_TreeSetItemCheckable, W_TreeSetItemChecked, W_TreeItemChecked,
        W_TreeSetExpanded, W_TreeSetDragDrop, W_FindEditorWidget, W_FindChild, W_WidgetNumber, W_ScrollArea, W_ClickButton,
        W_SetActionChecked,
    };

    ModToolsHostApi g_api = {
        sizeof(ModToolsHostApi), MODTOOLS_PLUGIN_API_VERSION, (1u << 16) | (2u << 8) | 0u,   // 1.2.0
        PluginDir, PluginsRoot, RadiantRoot,
        Log, LogW,
        ConfigGet, ConfigGetInt, ConfigSet,
        PostToMainThread, IsMainThread, MainWindow,
        CurrentMapPath, PrefabDepth, IsMapModified, ReloadMap, IsReloading, BeginOwnWrite, EndOwnWrite, OwnWriteRecent, InhibitReloadPrompts, ReloadPromptsInhibited,
        GetCamera, SetCamera, GameView, SetGameView, LastCamParams, CamWindow,
        ForEachEntity, ForEachEntityBrush, EntityGuid, BrushGuid, BrushOwner, WorldEntity, GetSelection, WriteEntityText, FreeString,
        ImportMapText, DeleteBrushes, SelectedBrushInsts, SelectBrushInsts, DeselectAll, RestampImportedGuids, SelectEntityByGuid, EnterSelectedPrefab, LeavePrefab,
        Project, DrawWorldLine, DrawScreenLine, DrawLabel,
        nullptr /* ui, filled in Table() */, CreatePane, PaneVisible, ShowPane, PaneToggleAction, AddMenu, AddMenuItem, AddMenuAction, AddMenuSeparator, RegisterHotkey, MakeEventFilter, OnClicked,
        PluginLoaded, Command,
        RadiantAddress, Detour,
        BrushInstOf,
        App, nullptr /* ape, filled in Table() */,
        H_CreateDialogWindow,
        H_TriggerMenuAction, H_MenuEntries,
        H_BeginUndo, H_EndUndo,
        H_MeasureLabel,
        H_RequestRedraw, H_CamXYUpdateOnDrag,
        H_MoveBrush, H_FindBrushesByGuid, H_BrushBounds,
        H_FindEntitiesByGuid,
        H_BrushPlanes, H_SetBrushPlanes, H_EntityFixedSize, H_BrushOfInst, H_WriteEntityTextWith,
        H_BeginUntracked, H_EndUntracked, H_GetUndoTags, H_SetUndoTags,
        H_PrefabStackPlacement,
        OnChanged,
        H_PerformanceProfile,
        H_XYWindow, H_MarkMapModified,
        H_PatchGrid, H_PatchSelectVerts, H_PatchWeldVerts, H_DragMode,
        H_RestampImportedEntities,
        DrawWorldTriangle,
        SetNativeModelPreview,
        NativeModelPreviewStatus,
        NativeModelPreviewDrawCount,
        SetNativeModelPreviewSet,
        NativeModelPreviewStatusForId,
        NativeModelPreviewDrawCountForId,
        SetNativeModelCullBounds,
        TabPaneWith,
        H_PatchSetGrid,
        H_CommitBrushGeometry,
        H_SetCameraLutPreview,
        H_CameraLutPreviewStatus,
    };
}

namespace hostapi
{
    void BeginDetourBatch() { if (g_detourBatch++ == 0) g_detoursQueued = 0; }

    void EndDetourBatch()
    {
        if (g_detourBatch == 0 || --g_detourBatch > 0) return;
        if (!g_detoursQueued) return;
        ULONGLONG t0 = GetTickCount64();
        MH_STATUS st = MH_ApplyQueued();
        LOG("host: %d plugin detours enabled together in %llu ms (%s)", g_detoursQueued, GetTickCount64() - t0, MH_StatusToString(st));
        g_detoursQueued = 0;
    }

    const ModToolsHostApi* Table()
    {
        g_api.ui = qt::Bind() ? &g_ui : nullptr;
        g_api.ape = app::IsApe() ? &g_ape : nullptr;
        return &g_api;
    }

    void ReleasePanes(ModToolsPlugin* p)
    {
        for (auto& kv : g_panes)
            if (kv.second.owner == p) { kv.second.wasVisible = dock::Visible(kv.second.pane); dock::Show(kv.second.pane, false); }
        p->panes.clear();
    }

    void ReleaseDialogs(ModToolsPlugin* p)
    {
        if (qt::Bound()) for (void* d : p->dialogs) { qt::SetVisible(d, false); qt::DeleteLater(d); }
        p->dialogs.clear();
    }

    void ReleaseMenus(ModToolsPlugin* p)
    {
        if (!qt::Bound()) { p->menus.clear(); return; }
        void* mw = dock::MainWindowWidget();
        void* bar = mw ? qt::MenuBar(mw) : nullptr;
        for (void* menu : p->menus)
        {
            qt::MenuClear(menu);
            if (bar) qt::RemoveAction(bar, qt::MenuAction(menu));
            g_menuOwner.erase(menu);
        }
        p->menus.clear();
    }
}
