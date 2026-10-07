#pragma once
#include "../sdk/modtools_plugin.h"

struct ModToolsPlugin;

// The ModToolsHostApi table handed to every plugin, plus per-plugin resource cleanup.
namespace hostapi
{
    const ModToolsHostApi* Table();
    void ReleasePanes(ModToolsPlugin* p);    // hide the plugin's panes (kept for a reload)
    void ReleaseMenus(ModToolsPlugin* p);    // clear + hide its menus (kept for a reload)
    void ReleaseDialogs(ModToolsPlugin* p);  // hide + delete its dialogs (the content widgets go with them)
    void BeginDetourBatch();                // plugin detours made on the main thread are queued (nests)...
    void EndDetourBatch();                  // ...and enabled with one thread freeze when the outermost batch ends
    struct DetourBatch { DetourBatch() { BeginDetourBatch(); } ~DetourBatch() { EndDetourBatch(); } };
}
