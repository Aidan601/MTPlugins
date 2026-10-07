#pragma once
#include <string>
#include <vector>

// The APE (asseteditor_modtools.exe) side of the host: the main window, the timer tick that replaces Radiant's idle
// detour, and the asset reads behind host->ape. Built on Qt exports only. Main thread unless noted.
namespace ape
{
    void* MainWindowWidget();          // the apeMainWindow (QMainWindow*), null until it exists
    bool  Ready();                     // main window found and bound
    void  ArmGdtScan(void* self);      // host DllMain: the startup GDT walk's import filters (ape_gdtscan.cpp)
    void  InstallGdtScan();            // from InstallStartupFixes: logs the filter state, binds APE's refresh
    void  EndGdtScan();                // main window up: filters off if APE's watcher did not end them
    void  InstallStartupFixes();       // init thread, right after attach: guards for APE startup code
    void  Start();                     // arms the tick (call once from the init thread after mt::Init)
    void  Tick();                      // main thread: drains jobs, ticks the host and the plugins, watches the current tab

    struct Asset { std::string name, type; };
    bool  CurrentAsset(Asset& out);
    std::vector<Asset> SelectedAssets();
    int   TabCount();
    bool  TabAsset(int index, Asset& out);
    std::vector<std::string> AssetTypes(const std::string& name);   // every type an asset of that name exists as (gdt.db)
    std::vector<Asset> FindAssets(const std::string& namePart, const std::string& type, size_t limit);  // gdt.db search, any thread
    std::vector<std::string> AssetTypeNames();      // every asset type (GDF name) gdt.db knows, any thread
    struct AssetInfo { std::wstring gdtPath; int line = 0; std::string parentName, parentType; };
    bool  LookupAsset(const std::string& name, const std::string& type, AssetInfo& out);   // gdt.db: file, line, parent, any thread
    std::string TypeProperties(const std::string& type);   // gdt.db _meta rows of a type: "key\ttype\tdefault\tenums\n" per property, any thread
    std::string OutputText();                       // main thread: the Output pane's plain text
    // The property grid of the current tab (apeAssetWidget): one entry per ape*Widget, title from the QLabel that
    // follows it, category from the apeGroupWidget header above it, value read from the Qt control(s) inside.
    struct Prop { std::string category, title, kind, value, tooltip; bool visible = false, enabled = false; void* widget = nullptr; std::vector<void*> parts; };
    std::vector<Prop> Properties();                 // main thread
    bool  SetProperty(const std::string& titleOrIndex, const std::string& value, std::string& why);   // main thread, writes through the control and emits its signal
    bool  OpenAsset(const std::string& name, const std::string& type, std::string& why);           // main thread: the current tab's asset line edit + Enter
    void  Print(const std::string& line);           // any thread, appended to APE's Output pane
    std::wstring GdtDbPath();

    std::string DumpWidgets(int maxDepth);          // debug: the object tree under the main window (class, objectName)
    std::string Command(const std::string& cmd);    // "ape ..." commands of the control channel, empty = not handled
}
