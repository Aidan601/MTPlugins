// compact_xpak: zone\<map>.xpak is append-only (the linker never compacts it), so it grows on every link while a fresh one
// holds only what the map streams now. Before a link that runs anyway, the file is deleted when it has outgrown the last
// recorded fresh size by far; the link then writes a fresh one. The fresh size is the xpak's size after a link that started
// without one, so a map gets its baseline on its first link or after the user deletes the xpak once. Without a baseline nothing
// is deleted: the index cannot tell live data from stale. State: linker_state\<map>_xpak.txt.
#pragma once
#include <windows.h>
#include <cstdio>
#include <string>

namespace xpakclean
{
    constexpr unsigned long long kRatio = 3, kSlack = 256ull << 20;

    inline unsigned long long Size(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA fa = {};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return 0;
        return ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    }
    inline std::wstring StatePath(const std::wstring& stateDir, const std::wstring& map) { return stateDir + L"\\" + map + L"_xpak.txt"; }
    inline unsigned long long Fresh(const std::wstring& stateDir, const std::wstring& map)
    {
        FILE* f = _wfopen(StatePath(stateDir, map).c_str(), L"rb");
        if (!f) return 0;
        char line[64] = {};
        unsigned long long fresh = 0;
        if (fgets(line, sizeof line, f) && !strncmp(line, "fresh=", 6)) fresh = _strtoui64(line + 6, nullptr, 10);
        fclose(f);
        return fresh;
    }
    inline double Gb(unsigned long long bytes) { return (double)bytes / (1024.0 * 1024.0 * 1024.0); }

    // before a link that runs: deletes an outgrown xpak, returns the launcher line (empty when nothing was done)
    inline std::string Before(const std::wstring& mapDir, const std::wstring& map, const std::wstring& stateDir, bool& absent)
    {
        const std::wstring xpak = mapDir + L"\\zone\\" + map + L".xpak";
        const unsigned long long size = Size(xpak);
        absent = size == 0 && GetFileAttributesW(xpak.c_str()) == INVALID_FILE_ATTRIBUTES;
        if (absent) return "";
        const unsigned long long fresh = Fresh(stateDir, map);
        if (!fresh || size <= fresh * kRatio || size <= fresh + kSlack) return "";
        if (!DeleteFileW(xpak.c_str())) return "";
        absent = true;
        char line[320];
        snprintf(line, sizeof line, "Xpak cleaned: zone\\%ls.xpak had grown to %.2f GB (a fresh one was %.2f GB); this link writes a fresh one. compact_xpak=0 in LauncherPlus.ini keeps it growing.", map.c_str(), Gb(size), Gb(fresh));
        return line;
    }

    // after a link that started without an xpak: its size is the fresh size
    inline void After(const std::wstring& mapDir, const std::wstring& map, const std::wstring& stateDir, bool wasAbsent)
    {
        if (!wasAbsent) return;
        const unsigned long long size = Size(mapDir + L"\\zone\\" + map + L".xpak");
        if (!size) return;
        CreateDirectoryW(stateDir.c_str(), nullptr);
        FILE* f = _wfopen(StatePath(stateDir, map).c_str(), L"wb");
        if (!f) return;
        fprintf(f, "fresh=%llu\n", size);
        fclose(f);
    }
}
