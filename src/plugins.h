#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "../sdk/modtools_plugin.h"

// The plugin registry: discovery, load/unload, event dispatch with crash isolation. Main thread unless noted.

struct ModToolsPlugin        // the handle plugins get (opaque to them)
{
    enum State { Discovered, Disabled, Loaded, Failed, Incompatible, OtherApp };

    std::string   name;             // identifier (= DLL base name)
    std::string   prefix;           // "[Name]" for the log
    std::wstring  dir, dllPath, iniPath;
    HMODULE       module = nullptr;
    ModToolsPluginApi api{};         // copy of the plugin's table
    State         state = Discovered;
    bool          enabled = true;   // plugins.ini
    uint32_t      apps = 0;         // MODTOOLS_APP_* mask, 0 = not known yet
    std::string   error;            // why Failed/Incompatible
    std::string   displayName, version, author, description, url;   // url: http(s) only, else empty
    bool          replaced = false; // old name of a renamed plugin, never loaded
    bool          inhibitReload = false;
    int           calls = 0;        // depth of Call() frames (an unload inside one is deferred)
    bool          unloadPending = false;
    bool          lifecycle = false; // inside Load()/Unload(), which finish the deferred unload
    std::string   lastReply;        // last OnCommand result
    // resources the host cleans up on unload
    std::vector<void*> panes;       // dock::Pane*
    std::vector<void*> menus;       // QMenu*
    std::vector<void*> dialogs;     // QDialog* from CreateDialog
    std::vector<void*> filters;     // qt event filter objects
    std::vector<void*> slots;       // qt slot objects
    std::vector<void*> callbacks;   // hostapi heap contexts (freed on unload)
    std::vector<void*> detours;     // MinHook targets
};

namespace plugins
{
    void Init();                                  // once the dock exists: discover + load the enabled ones
    bool Initialized();
    void Rescan();                                // re-read the folder (new plugins appear, unloaded ones whose DLL is gone drop out)
    void Forget(ModToolsPlugin* p);               // drop an unloaded plugin from the list and plugins.ini (uninstall)
    const std::wstring& Root();                   // the plugins folder
    const std::wstring& RadiantRoot();            // the BO3 install root
    std::vector<ModToolsPlugin*> All();
    ModToolsPlugin* Find(const std::string& name); // case-insensitive
    bool Load(ModToolsPlugin* p);
    bool Unload(ModToolsPlugin* p);
    bool Reload(ModToolsPlugin* p);
    void SetEnabled(ModToolsPlugin* p, bool on);   // persists to plugins.ini and loads/unloads
    unsigned Version();                           // bumps on every state change (UI refresh)
    std::string InstalledVersion(const std::string& name);   // the version the installed DLL reports, "" if unknown
    const char* StateName(ModToolsPlugin::State s);
    bool ForThisEditor(const ModToolsPlugin* p);   // runtime callbacks can run in this editor
    std::string AppNames(uint32_t apps);

    // Run `fn(arg)` under crash isolation: a fault disables the plugin and writes a minidump
    bool Call(ModToolsPlugin* p, const char* what, void (*fn)(void*), void* arg);

    // events from hooks.cpp / render.cpp
    void OnTick();
    void OnMapLoading(const std::wstring& path);
    void OnMapLoaded(const std::wstring& path);
    void OnMapSaved(const std::wstring& path, bool autosave, bool selectedOnly);
    void OnBeforeScopeChange();
    void OnScopeChanged(const std::wstring& path, int depth);
    void DispatchDraw(const ModToolsDrawContext& ctx);
    void OnAssetChanged(const std::string& name, const std::string& type);   // APE

    // command channel: "plugins ..." or "<name> <cmd>" (or the first plugin that answers)
    std::string Command(const std::string& cmd);
    const char* CommandTo(ModToolsPlugin* p, const char* cmd);   // NULL = not handled
    bool ReloadPromptsInhibited();

    void Shutdown();                              // unload everything (DLL detach)
}
