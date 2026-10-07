// MapEditExample: reading and changing the open map from a Radiant plugin (GETTING_STARTED.md section 7).
// A pane shows the selected entity's keys. "Set key" / "Remove key" edit it as one undo step (rp::SetKey), "Add script_struct" places an entity in front of the camera (rp::ImportEntities).
// Radiant has no selection event: poll in OnTick and compare. Keep entities by GUID, never by pointer (an edit replaces the entity).
#define MODTOOLS_PLUGIN_IMPL
#include "modtools_plugin.hpp"
#include <windows.h>
#include <cmath>
#include <string>

namespace
{
    ModToolsPane   g_pane = nullptr;
    ModToolsWidget g_title = nullptr, g_keys = nullptr, g_keyEdit = nullptr, g_valueEdit = nullptr, g_status = nullptr;
    std::string    g_guid;           // the entity shown in the pane ("" = none)
    std::string    g_shown;          // what the key list shows, to update it only when it changed
    ULONGLONG      g_nextPoll = 0;

    // GUID -> entity handle (one walk of the entity list, FindEntitiesByGuid is the fast path on current hosts)
    ModToolsEntity FindEntity(const std::string& guid)
    {
        if (guid.empty()) return nullptr;
        if (rp::Has(&rp::host->FindEntitiesByGuid) && rp::host->FindEntitiesByGuid) return rp::FindEntitiesByGuid({ guid })[0];
        ModToolsEntity found = nullptr;
        rp::ForEachEntity([&](ModToolsEntity e) { if (!found && rp::EntityGuid(e) == guid) found = e; });
        return found;
    }

    void SetStatus(const std::string& text) { if (g_status) rp::host->ui->SetLabelText(g_status, text.c_str()); }

    // Show the first selected entity and its keys. Called from OnTick a few times a second and after every edit.
    void Refresh()
    {
        std::vector<std::string> sel = rp::SelectedEntityGuids();
        g_guid = sel.empty() ? std::string() : sel.front();
        std::string title = "Select an entity (a model, a light, a struct, ...).", keys;
        if (ModToolsEntity e = FindEntity(g_guid))
        {
            rp::KeyValues kv = rp::GetKeys(e);
            title = rp::FindKey(kv, "classname", "?") + "  " + g_guid;
            if (sel.size() > 1) title += "  (+" + std::to_string(sel.size() - 1) + " more selected)";
            for (const auto& p : kv) keys += p.first + " = " + p.second + "\n";
        }
        if (title + keys == g_shown) return;    // nothing changed: leave the widgets alone
        g_shown = title + keys;
        rp::host->ui->SetLabelText(g_title, title.c_str());
        rp::host->ui->PlainSetText(g_keys, keys.c_str());   // SetEditText is for line edits only
    }

    void OnSetKey(void*)
    {
        std::string key = rp::EditText(g_keyEdit), value = rp::EditText(g_valueEdit);
        ModToolsEntity e = FindEntity(g_guid);
        if (!e) { SetStatus("Nothing selected."); return; }
        if (key.empty()) { SetStatus("Type a key first."); return; }
        std::string guid = rp::SetKey(e, key, value, "MapEditExample set key");   // e is gone after this
        SetStatus(guid.empty() ? "Not changed, see RadiantPlugins.log." : "Set " + key + ". Ctrl+Z undoes it.");
        g_shown.clear(); Refresh();
    }

    void OnRemoveKey(void*)
    {
        std::string key = rp::EditText(g_keyEdit);
        ModToolsEntity e = FindEntity(g_guid);
        if (!e || key.empty()) { SetStatus("Select an entity and type the key to remove."); return; }
        if (rp::GetKey(e, key, "\x01") == "\x01") { SetStatus("It has no key '" + key + "'."); return; }
        std::string guid = rp::SetKeys(e, {}, { key }, "MapEditExample remove key");
        SetStatus(guid.empty() ? "Not changed, see RadiantPlugins.log." : "Removed " + key + ".");
        g_shown.clear(); Refresh();
    }

    // A script_struct 128 units in front of the camera. Radiant's angles are pitch (positive looks down), yaw, roll.
    void OnAddStruct(void*)
    {
        float o[3], a[3];
        if (!rp::host->GetCamera(rp::self, o, a)) { SetStatus("No camera yet: open a map first."); return; }
        const float d2r = 3.14159265f / 180.0f;
        float p = a[0] * d2r, y = a[1] * d2r;
        char origin[96];
        snprintf(origin, sizeof origin, "%.0f %.0f %.0f", o[0] + 128 * cosf(p) * cosf(y), o[1] + 128 * cosf(p) * sinf(y), o[2] - 128 * sinf(p));
        // one entity block in .map text (the worldspawn block Radiant's paste wants is added for us)
        std::string text = "{\n\"classname\" \"script_struct\"\n\"origin\" \"" + std::string(origin) + "\"\n\"targetname\" \"mapedit_example\"\n}\n";
        if (!rp::ImportEntities(text, "MapEditExample add struct")) { SetStatus("Radiant refused the import, see RadiantPlugins.log."); return; }
        std::vector<std::string> added = rp::SelectedEntityGuids();    // the import leaves the new entity selected
        SetStatus(added.empty() ? "Added." : "Added " + added.front() + ".");
        g_shown.clear(); Refresh();
    }

    int OnLoad(void)
    {
        if (!rp::host->ui || !rp::HasUi(&rp::host->ui->PlainSetText)) { rp::Log("this host is too old for the example's widgets"); return 0; }
        const ModToolsUiApi* ui = rp::host->ui;
        ModToolsWidget root = ui->Widget(nullptr);
        ModToolsWidget box = ui->VBox(root);
        ui->LayoutMargins(box, 8, 8, 8, 8);
        g_title = ui->Label("", root);
        ui->SetWordWrap(g_title, 1);
        ui->BoxAddWidget(box, g_title, 0);
        g_keys = ui->PlainTextEdit(root);
        ui->SetReadOnly(g_keys, 1);
        ui->BoxAddWidget(box, g_keys, 1);

        ModToolsWidget grid = ui->Grid();
        g_keyEdit = ui->LineEdit("", root);    ui->SetPlaceholder(g_keyEdit, "key, e.g. targetname");
        g_valueEdit = ui->LineEdit("", root);  ui->SetPlaceholder(g_valueEdit, "value");
        ModToolsWidget setButton = ui->PushButton("Set key", root);
        ModToolsWidget removeButton = ui->PushButton("Remove key", root);
        ui->GridAddWidget(grid, g_keyEdit, 0, 0);   ui->GridAddWidget(grid, setButton, 0, 1);
        ui->GridAddWidget(grid, g_valueEdit, 1, 0); ui->GridAddWidget(grid, removeButton, 1, 1);
        ui->GridColumnStretch(grid, 0, 1);
        ui->BoxAddLayout(box, grid, 0);
        ModToolsWidget addButton = ui->PushButton("Add script_struct in front of the camera", root);
        ui->BoxAddWidget(box, addButton, 0);
        g_status = ui->Label("", root);
        ui->SetWordWrap(g_status, 1);
        ui->BoxAddWidget(box, g_status, 0);

        rp::host->OnClicked(rp::self, setButton, OnSetKey, nullptr);
        rp::host->OnClicked(rp::self, removeButton, OnRemoveKey, nullptr);
        rp::host->OnClicked(rp::self, addButton, OnAddStruct, nullptr);
        g_pane = rp::host->CreatePane(rp::self, "Map Edit Example", root, 320, 360);
        if (ModToolsWidget menu = rp::host->AddMenu(rp::self, "Map Edit Example"))
            if (g_pane) rp::host->AddMenuAction(rp::self, menu, rp::host->PaneToggleAction(rp::self, g_pane));
        Refresh();
        return 1;
    }

    // Radiant has no selection event: look four times a second, and only while the pane is open and nothing is loading
    void OnTick(void)
    {
        ULONGLONG now = GetTickCount64();
        if (now < g_nextPoll) return;
        g_nextPoll = now + 250;
        if (!g_pane || !rp::host->PaneVisible(rp::self, g_pane) || rp::host->IsReloading(rp::self)) return;
        Refresh();
    }

    // handles from the old map are gone, GUIDs are not, but the selection is empty after a load anyway
    void OnMapLoaded(const wchar_t*) { g_guid.clear(); g_shown.clear(); SetStatus(""); }

    void OnSettings(void) { if (g_pane) rp::host->ShowPane(rp::self, g_pane, 1); }
}

extern "C" MODTOOLS_PLUGIN_EXPORT const ModToolsPluginApi* ModToolsPluginEntry(ModToolsPlugin* self, const ModToolsHostApi* host)
{
    rp::Bind(self, host);
    static ModToolsPluginApi api = {};
    api.size = sizeof api;
    api.apiVersion = MODTOOLS_PLUGIN_API_VERSION;
    api.name = "MapEditExample";
    api.displayName = "Map Edit Example";
    api.version = "1.0.0";
    api.author = "you";
    api.description = "Example Radiant plugin: shows the selected entity's keys, changes them with undo, and adds an entity in front of the camera.";
    api.OnLoad = OnLoad;
    api.OnTick = OnTick;
    api.OnMapLoaded = OnMapLoaded;
    api.OnSettings = OnSettings;
    api.apps = MODTOOLS_APP_RADIANT;
    return &api;
}
