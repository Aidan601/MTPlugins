#pragma once
#include <string>

// RadiantPlugins.ini next to the host DLL: host settings only (each plugin has its own ini). Missing keys use the defaults.
struct Config
{
    // [plugins]
    std::wstring pluginsDir;           // empty = <root>\bin\plugins (root = the folder above Radiant's bin)
    bool     isolate        = true;    // catch faults inside plugin callbacks, write a minidump, disable the plugin
    bool     autoloadMissing = true;   // a plugin with no entry in plugins.ini is enabled

    // [window]
    bool     maximize       = true;    // maximize Radiant's main window after the first map load

    // [render]
    float    fovDegrees     = 100.0f;  // editor camera FOV: horizontal FOV of a 16:9 view (Radiant's Prefs\Fov, default 100)
    float    gameFov        = 65.0f;   // "GAME VIEW" mode: horizontal FOV of a 4:3 view (the game's cg_fov default)
    float    pitchSign      = 1.0f;    // Radiant: positive pitch looks UP
    bool     markerEnabled  = false;   // draw a calibration cross at `marker`
    float    marker[3]      = { 0, 0, 0 };
    bool     debugCamEnabled = false;  // place the camera after load: x y z pitch yaw (calibration)
    float    debugCam[5]    = { 0, 0, 0, 0, 0 };
    bool     engineText     = true;    // plugin labels in the 3D view use Radiant's own HUD font (0 = the stroke font)
    int      textBackground = 128;     // alpha of the dark box Radiant draws behind HUD text (0 = none)

    // [ape]
    bool     apeFastGdtScan = true;    // APE's startup GDT walk opens only the folders that led to a GDT last time (src/ape_gdtscan.cpp)

    // [log]
    bool     verbose        = false;

    std::wstring dllDir;               // directory the host DLL was loaded from
    std::wstring iniPath;
};

Config& Cfg();
void    LoadConfig(const std::wstring& dllDir);
