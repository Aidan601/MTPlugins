#include <windows.h>
#include <vector>
#include <cstring>
#include <cstdint>
#include "config.h"
#include "log.h"

namespace
{
    // Radiant stores its preferences with QSettings: Prefs\Fov is a REG_BINARY holding the UTF-16 text
    // "@Variant(" + one char per byte of the QDataStream-serialized QVariant (type 135 = float, big-endian) + ")".
    float RadiantFovPref(float fallback)
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Treyarch\\Radiant\\Prefs", 0, KEY_READ, &key) != ERROR_SUCCESS) return fallback;
        BYTE buf[256] = {}; DWORD size = sizeof buf, type = 0;
        LONG rc = RegQueryValueExW(key, L"Fov", nullptr, &type, buf, &size);
        RegCloseKey(key);
        if (rc != ERROR_SUCCESS || size < 4) return fallback;
        std::vector<unsigned char> bytes;                 // the "characters" after "@Variant(" as raw bytes
        const wchar_t* w = reinterpret_cast<const wchar_t*>(buf); size_t n = size / 2;
        size_t start = 0;
        for (size_t i = 0; i + 1 < n; ++i) if (w[i] == L'(') { start = i + 1; break; }
        if (!start) return fallback;
        for (size_t i = start; i < n && w[i] != L')'; ++i) bytes.push_back(static_cast<unsigned char>(w[i] & 0xFF));
        if (bytes.size() < 4) return fallback;
        uint32_t u = (uint32_t(bytes[bytes.size() - 4]) << 24) | (uint32_t(bytes[bytes.size() - 3]) << 16) | (uint32_t(bytes[bytes.size() - 2]) << 8) | bytes[bytes.size() - 1];
        float f; memcpy(&f, &u, 4);
        if (!(f >= 10.0f && f <= 170.0f)) return fallback;
        return f;
    }
    Config g_cfg;

    std::wstring GetStr(const wchar_t* sec, const wchar_t* key, const std::wstring& def)
    {
        wchar_t buf[1024];
        GetPrivateProfileStringW(sec, key, def.c_str(), buf, 1024, g_cfg.iniPath.c_str());
        return buf;
    }
    unsigned GetUInt(const wchar_t* sec, const wchar_t* key, unsigned def)
    {
        return static_cast<unsigned>(GetPrivateProfileIntW(sec, key, static_cast<int>(def), g_cfg.iniPath.c_str()));
    }
    bool GetBool(const wchar_t* sec, const wchar_t* key, bool def) { return GetUInt(sec, key, def ? 1 : 0) != 0; }
}

Config& Cfg() { return g_cfg; }

void LoadConfig(const std::wstring& dllDir)
{
    g_cfg.dllDir = dllDir;
    g_cfg.iniPath = dllDir + L"\\RadiantPlugins.ini";

    g_cfg.pluginsDir = GetStr(L"plugins", L"dir", L"");
    while (!g_cfg.pluginsDir.empty() && (g_cfg.pluginsDir.back() == L'\\' || g_cfg.pluginsDir.back() == L'/')) g_cfg.pluginsDir.pop_back();
    g_cfg.isolate = GetBool(L"plugins", L"isolate", g_cfg.isolate);
    g_cfg.autoloadMissing = GetBool(L"plugins", L"autoload_new", g_cfg.autoloadMissing);

    g_cfg.maximize = GetBool(L"window", L"maximize", g_cfg.maximize);

    // fov: empty = Radiant's own camera FOV preference (HKCU\Software\Treyarch\Radiant\Prefs\Fov), else a number
    std::wstring fov = GetStr(L"render", L"fov", L"");
    g_cfg.fovDegrees = fov.empty() ? RadiantFovPref(100.0f) : static_cast<float>(_wtof(fov.c_str()));
    if (g_cfg.fovDegrees < 10 || g_cfg.fovDegrees > 170) g_cfg.fovDegrees = 100.0f;
    g_cfg.gameFov = static_cast<float>(_wtof(GetStr(L"render", L"game_fov", L"65").c_str()));
    if (g_cfg.gameFov < 10 || g_cfg.gameFov > 170) g_cfg.gameFov = 65.0f;
    g_cfg.pitchSign = _wtof(GetStr(L"render", L"pitch_sign", L"1").c_str()) < 0 ? -1.0f : 1.0f;
    std::wstring marker = GetStr(L"render", L"marker", L"");
    g_cfg.markerEnabled = swscanf_s(marker.c_str(), L"%f %f %f", &g_cfg.marker[0], &g_cfg.marker[1], &g_cfg.marker[2]) == 3;
    std::wstring dc = GetStr(L"render", L"camera", L"");
    g_cfg.debugCamEnabled = swscanf_s(dc.c_str(), L"%f %f %f %f %f", &g_cfg.debugCam[0], &g_cfg.debugCam[1], &g_cfg.debugCam[2], &g_cfg.debugCam[3], &g_cfg.debugCam[4]) == 5;
    g_cfg.engineText = GetBool(L"render", L"engine_text", g_cfg.engineText);
    g_cfg.textBackground = _wtoi(GetStr(L"render", L"text_background", L"128").c_str());
    if (g_cfg.textBackground < 0) g_cfg.textBackground = 0; if (g_cfg.textBackground > 255) g_cfg.textBackground = 255;
    g_cfg.apeFastGdtScan = GetBool(L"ape", L"fast_gdt_scan", g_cfg.apeFastGdtScan);
    g_cfg.verbose = GetBool(L"log", L"verbose", g_cfg.verbose);

    LOGW(L"cfg: ini='%s' plugins_dir='%s' isolate=%d maximize=%d fov=%.0f", g_cfg.iniPath.c_str(), g_cfg.pluginsDir.c_str(), g_cfg.isolate, g_cfg.maximize, g_cfg.fovDegrees);
}
