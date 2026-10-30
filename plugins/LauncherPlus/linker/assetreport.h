// Fastfile content line after a recorded link: the linker's own assetinfo\<map>.csv (type, name, resident and streamed
// bytes per asset) against the copy kept from the last link in linker_state\<map>_<language>_assets.csv.
#pragma once
#include <windows.h>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace assetreport
{
    using Key = std::pair<std::string, std::string>;      // type, name
    using Sizes = std::pair<long long, long long>;        // resident, streamed

    inline bool ReadAll(const std::wstring& path, std::string& out)
    {
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) return false;
        char buf[1 << 16];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        fclose(f);
        return true;
    }

    // index,type,name,resident,streamed,parentStack
    inline bool Parse(const std::string& text, std::map<Key, Sizes>& out)
    {
        size_t pos = text.find('\n');
        if (pos == std::string::npos || text.compare(0, 5, "index") != 0) return false;
        ++pos;
        while (pos < text.size())
        {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::vector<std::string> f;
            size_t a = 0;
            for (int i = 0; i < 5; ++i)
            {
                const size_t c = line.find(',', a);
                if (c == std::string::npos) { f.push_back(line.substr(a)); a = std::string::npos; break; }
                f.push_back(line.substr(a, c - a));
                a = c + 1;
            }
            if (f.size() < 5) continue;
            out[{ f[1], f[2] }] = { _atoi64(f[3].c_str()), _atoi64(f[4].c_str()) };
        }
        return true;
    }

    inline std::string Line(const std::wstring& mapDir, const std::wstring& map, const std::wstring& language, const std::wstring& stateDir)
    {
        const std::wstring cur = mapDir + L"\\zone_source\\all\\assetinfo\\" + map + L".csv";
        const std::wstring prev = stateDir + L"\\" + map + L"_" + language + L"_assets.csv";
        std::string now, before;
        std::map<Key, Sizes> a, b;
        if (!ReadAll(cur, now) || !Parse(now, b)) return "";
        const bool had = ReadAll(prev, before) && Parse(before, a);
        CreateDirectoryW(stateDir.c_str(), nullptr);
        CopyFileW(cur.c_str(), prev.c_str(), FALSE);
        char line[512];
        if (!had)
        {
            snprintf(line, sizeof line, "Fastfile content: %zu assets, kept for the next link's comparison.", b.size());
            return line;
        }
        size_t added = 0, removed = 0, resized = 0;
        std::string example;
        for (const auto& [k, v] : b)
        {
            const auto it = a.find(k);
            if (it == a.end()) { ++added; if (example.empty()) example = "new " + k.first + " " + k.second; }
            else if (it->second != v)
            {
                ++resized;
                if (example.empty())
                {
                    char e[256];
                    snprintf(e, sizeof e, "%s %s %lld/%lld -> %lld/%lld bytes", k.first.c_str(), k.second.c_str(), it->second.first, it->second.second, v.first, v.second);
                    example = e;
                }
            }
        }
        for (const auto& [k, v] : a) if (!b.count(k)) { ++removed; if (example.empty()) example = "gone " + k.first + " " + k.second; }
        if (!added && !removed && !resized) snprintf(line, sizeof line, "Fastfile content: %zu assets, the same as the last link.", b.size());
        else snprintf(line, sizeof line, "Fastfile content: %zu assets, %zu new, %zu gone, %zu with another size (%s).", b.size(), added, removed, resized, example.c_str());
        return line;
    }
}
