// HelloPlugin: the smallest useful plugin, it runs in both editors (apps = Radiant | APE). NewPlugin.bat in the SDK folder starts a new one from it.
// Adds a "Hello" menu item, a Ctrl+Shift+H hotkey and a pane with a button, and draws the selected brush count (Radiant) or shows the current asset (APE).
// The DLL name must match ModToolsPluginApi::name. It goes to <game>\bin\plugins\HelloPlugin\ (tick it in the Plugins window, Ctrl+Shift+P).
#define MODTOOLS_PLUGIN_IMPL
#include "modtools_plugin.hpp"
#include <windows.h>
#include <string>

namespace
{
    ModToolsPane   g_pane = nullptr;
    ModToolsWidget g_label = nullptr;
    int           g_clicks = 0;

    void SayHello(void*)
    {
        ++g_clicks;
        rp::Log("hello from the plugin (%d)", g_clicks);
        std::string what;
        if (rp::InApe())
        {
            ModToolsApeAsset a{};
            what = rp::Ape() && rp::Ape()->CurrentAsset(rp::self, &a) ? std::string("Asset: ") + a.name + " (" + a.type + ")" : "no asset open";
            if (rp::Ape()) rp::Ape()->Print(rp::self, ("HelloPlugin: hello " + std::to_string(g_clicks)).c_str());
        }
        else
        {
            char map[512] = {}; WideCharToMultiByte(CP_UTF8, 0, rp::CurrentMapPath().c_str(), -1, map, sizeof map, nullptr, nullptr);
            what = std::string("Map: ") + map;
        }
        if (g_label) rp::host->ui->SetLabelText(g_label, ("Clicked " + std::to_string(g_clicks) + " time(s). " + what).c_str());
        if (g_pane) rp::host->ShowPane(rp::self, g_pane, 1);
    }

    int OnLoad(void)
    {
        rp::Log("loaded inside %s; plugin folder: %S", rp::InApe() ? "APE" : "Radiant", rp::PluginDir().c_str());
        if (rp::host->ui)
        {
            const ModToolsUiApi* ui = rp::host->ui;
            ModToolsWidget root = ui->Widget(nullptr);
            ModToolsWidget box = ui->VBox(root);
            ui->LayoutMargins(box, 8, 8, 8, 8);
            g_label = ui->Label(rp::InApe() ? "Hello from an APE plugin." : "Hello from a Radiant plugin.", root);
            ui->SetWordWrap(g_label, 1);
            ui->BoxAddWidget(box, g_label, 0);
            ModToolsWidget button = ui->PushButton("Say hello", root);
            ui->BoxAddWidget(box, button, 0);
            ui->BoxAddStretch(box, 1);
            rp::host->OnClicked(rp::self, button, SayHello, nullptr);
            g_pane = rp::host->CreatePane(rp::self, "Hello Plugin", root, 300, 200);
            ModToolsWidget menu = rp::host->AddMenu(rp::self, "Hello");
            if (menu)
            {
                if (g_pane) rp::host->AddMenuAction(rp::self, menu, rp::host->PaneToggleAction(rp::self, g_pane));
                rp::host->AddMenuItem(rp::self, menu, "Say hello (Ctrl+Shift+H)", SayHello, nullptr);
            }
        }
        rp::host->RegisterHotkey(rp::self, 'H', MODTOOLS_MOD_CTRL | MODTOOLS_MOD_SHIFT, SayHello, nullptr);
        return 1;
    }

    void OnDraw(const ModToolsDrawContext* ctx)
    {
        if (ctx->view != 1) return;    // 3D view only
        size_t n = rp::host->GetSelection(rp::self, nullptr, 0);
        std::string text = "HELLO PLUGIN: " + std::to_string(n) + " SELECTED";
        // In the 3D view labels use Radiant's HUD font, its own two status lines sit at the bottom, so go above them
        float w = 0, lineH = 12.0f;
        if (rp::Has(&rp::host->MeasureLabel) && rp::host->MeasureLabel) rp::host->MeasureLabel(rp::self, text.c_str(), 2.0f, &w, &lineH);
        rp::host->DrawLabel(rp::self, 12.0f, float(ctx->height) - 3.0f * (lineH + 4.0f) - 8.0f, text.c_str(), 0xFFFFD040, 2.0f);
    }

    void OnSettings(void) { if (g_pane) rp::host->ShowPane(rp::self, g_pane, 1); }

    // APE: the current editor tab changed
    void OnAssetChanged(const char* name, const char* type)
    {
        rp::Log("current asset: '%s' (%s)", name, type);
        if (g_label) rp::host->ui->SetLabelText(g_label, (std::string("Current asset: ") + name + " (" + type + ")").c_str());
    }

    // test channel: "HelloPlugin crash" faults on purpose so the host's crash isolation can be exercised
    const char* OnCommand(const char* cmd)
    {
        if (std::string(cmd) == "crash") { volatile int* p = nullptr; *p = 1; }
        if (std::string(cmd) == "hello") { SayHello(nullptr); return "hello"; }
        return nullptr;
    }
}

extern "C" MODTOOLS_PLUGIN_EXPORT const ModToolsPluginApi* ModToolsPluginEntry(ModToolsPlugin* self, const ModToolsHostApi* host)
{
    rp::Bind(self, host);
    static ModToolsPluginApi api = {};
    api.size = sizeof api;
    api.apiVersion = MODTOOLS_PLUGIN_API_VERSION;
    api.name = "HelloPlugin";
    api.displayName = "Hello Plugin";
    api.version = "1.0.0";
    api.author = "you";
    api.description = "Example plugin for both editors: a menu, a hotkey, a pane with a button, text in Radiant's 3D view, the current asset in APE.";
    api.OnLoad = OnLoad;
    api.OnDraw = OnDraw;
    api.OnSettings = OnSettings;
    api.OnCommand = OnCommand;
    api.apps = MODTOOLS_APP_RADIANT | MODTOOLS_APP_APE;
    api.OnAssetChanged = OnAssetChanged;
    return &api;
}
