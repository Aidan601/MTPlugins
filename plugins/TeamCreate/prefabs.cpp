// Prefab mirror: see prefabs.h.
#include "compat.h"
#include "prefabs.h"
#include "backup.h"
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include <set>
#include <algorithm>

namespace
{
    std::set<std::string> g_mirrored;    // lower-case real keys ("_prefabs/zm/x.map")

    std::string Narrow(const std::wstring& w)
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n ? n - 1 : 0, '\0');
        if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
        return s;
    }
    std::wstring Widen(const std::string& s)
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n ? n - 1 : 0, L'\0');
        if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
        return w;
    }
    std::string Lower(std::string s) { for (auto& c : s) c = char(tolower(uint8_t(c))); return s; }
    std::wstring LowerW(std::wstring s) { for (auto& c : s) { if (c == L'/') c = L'\\'; c = wchar_t(towlower(c)); } return s; }

    std::wstring KeyToRel(const std::string& key) { std::wstring r = Widen(key); for (auto& c : r) if (c == L'/') c = L'\\'; return r; }

    void EnsureDirOf(const std::wstring& file)
    {
        size_t s = file.find_last_of(L"\\/");
        if (s != std::wstring::npos) SHCreateDirectoryExW(nullptr, file.substr(0, s).c_str(), nullptr);
    }

    bool WriteBytes(const std::wstring& path, const std::string& data)
    {
        EnsureDirOf(path);
        std::wstring tmp = path + L".rtc_tmp";
        { std::ofstream f(tmp, std::ios::binary); if (!f) return false; f.write(data.data(), std::streamsize(data.size())); }
        return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }

    // every "<prefix><path>.map" occurrence, returns the lower-case keys (with the prefix given)
    void Scan(const std::string& text, const char* prefix, std::vector<std::string>& out)
    {
        std::string lower = Lower(text); std::string pfx = prefix;
        size_t pos = 0;
        while ((pos = lower.find(pfx, pos)) != std::string::npos)
        {
            size_t end = lower.find_first_of("\"\r\n", pos);
            if (end == std::string::npos) end = lower.size();
            std::string key = lower.substr(pos, end - pos);
            for (auto& c : key) if (c == '\\') c = '/';
            if (key.size() > 4 && key.compare(key.size() - 4, 4, ".map") == 0) out.push_back(key);
            pos = end;
        }
    }

    void ListFiles(const std::wstring& dir, std::vector<std::wstring>& out)
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do
        {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ListFiles(p, out); else out.push_back(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    void RemoveTree(const std::wstring& dir)
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                std::wstring p = dir + L"\\" + fd.cFileName;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(p); else DeleteFileW(p.c_str());
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(dir.c_str());
    }
}

namespace prefabs
{
    std::wstring MapSourceDir()
    {
        std::wstring map = hooks::CurrentMapPath();
        std::wstring lower = LowerW(map);
        size_t at = lower.find(L"\\map_source\\");
        if (at != std::wstring::npos) return map.substr(0, at + 11);
        std::wstring root;
        if (rp::host->RadiantRoot) { const wchar_t* r = rp::host->RadiantRoot(rp::self); if (r && *r) root = r; }
        while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
        return root.empty() ? L"" : root + L"\\map_source";
    }
    std::wstring MirrorDir() { std::wstring m = MapSourceDir(); return m.empty() ? L"" : m + L"\\_teamcreate_temp"; }
    std::wstring RealPath(const std::string& key) { std::wstring m = MapSourceDir(); return m.empty() ? L"" : m + L"\\" + KeyToRel(key); }
    std::wstring MirrorPath(const std::string& key) { std::wstring m = MirrorDir(); return m.empty() ? L"" : m + L"\\" + KeyToRel(key); }

    bool   IsMirrored(const std::string& key) { return g_mirrored.count(Lower(key)) != 0; }
    void   MarkMirrored(const std::string& key) { g_mirrored.insert(Lower(key)); }
    void   ClearMirror() { g_mirrored.clear(); }
    bool   MirrorEmpty() { return g_mirrored.empty(); }
    size_t RescanMirror()
    {
        g_mirrored.clear();
        std::wstring base = MirrorDir();
        if (base.empty()) return 0;
        std::vector<std::wstring> files; ListFiles(base + L"\\_prefabs", files);
        for (const auto& f : files)
        {
            std::wstring rel = f.substr(base.size() + 1);                 // "_prefabs\zm\x.map"
            std::string key = Lower(Narrow(rel)); for (auto& c : key) if (c == '\\') c = '/';
            if (key.size() > 4 && key.compare(key.size() - 4, 4, ".map") == 0) g_mirrored.insert(key);
        }
        return g_mirrored.size();
    }

    std::string Localize(const std::string& text)
    {
        if (g_mirrored.empty()) return text;
        std::string out; out.reserve(text.size() + 64);
        std::string lower = Lower(text);
        size_t pos = 0;
        for (;;)
        {
            size_t at = lower.find(kRealPrefix, pos);
            if (at == std::string::npos) { out.append(text, pos, std::string::npos); break; }
            // not already inside a mirror prefix
            size_t end = lower.find_first_of("\"\r\n", at); if (end == std::string::npos) end = lower.size();
            std::string key = lower.substr(at, end - at); for (auto& c : key) if (c == '\\') c = '/';
            bool inMirror = at >= 17 && lower.compare(at - 17, 17, "_teamcreate_temp/") == 0;
            out.append(text, pos, at - pos);
            if (!inMirror && g_mirrored.count(key)) out += "_teamcreate_temp/";
            out.append(text, at, end - at);
            pos = end;
        }
        return out;
    }

    std::string Delocalize(const std::string& text)
    {
        std::string out; out.reserve(text.size());
        std::string lower = Lower(text);
        const std::string pfx = "_teamcreate_temp/_prefabs/";
        size_t pos = 0;
        for (;;)
        {
            size_t at = lower.find(pfx, pos);
            if (at == std::string::npos) { out.append(text, pos, std::string::npos); break; }
            out.append(text, pos, at - pos);
            out.append(text, at + 17, 9);                 // the "_prefabs/" part as written
            pos = at + pfx.size();
        }
        return out;
    }

    std::string DelocalizeKey(const std::string& key)
    {
        std::string l = Lower(key);
        if (l.rfind(kMirrorPrefix, 0) == 0) return key.substr(17);
        return key;
    }

    std::vector<std::string> Refs(const std::string& text)
    {
        std::vector<std::string> out; Scan(text, "_prefabs/", out);
        for (auto& k : out) k = DelocalizeKey(k);
        std::sort(out.begin(), out.end()); out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    uint64_t Hash(const std::string& b)
    {
        uint64_t h = 1469598103934665603ULL;
        for (unsigned char c : b) { h ^= c; h *= 1099511628211ULL; }
        return h;
    }

    bool ReadBytes(const std::wstring& path, std::string& out)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::ostringstream ss; ss << f.rdbuf(); out = ss.str();
        return true;
    }

    std::vector<Item> Inventory(const std::string& rootText, const std::vector<std::wstring>*)
    {
        std::vector<Item> items; std::set<std::string> seen;
        std::vector<std::string> todo = Refs(rootText);
        while (!todo.empty())
        {
            std::string key = todo.back(); todo.pop_back();
            if (!seen.insert(key).second) continue;
            Item it; it.key = key;
            std::wstring path = IsMirrored(key) ? MirrorPath(key) : RealPath(key);
            std::string bytes;
            if (!path.empty() && ReadBytes(path, bytes))
            {
                std::string real = Delocalize(bytes);          // what the file means to everybody else
                it.size = uint32_t(real.size()); it.hash = Hash(real);
                for (const auto& r : Refs(bytes)) todo.push_back(r);
            }
            else it.missing = true;
            items.push_back(it);
        }
        return items;
    }

    MoveResult MoveMirrorToReal()
    {
        MoveResult r;
        std::wstring base = MirrorDir();
        if (base.empty()) return r;
        std::vector<std::wstring> files; ListFiles(base + L"\\_prefabs", files);
        for (const auto& f : files)
        {
            std::wstring rel = f.substr(base.size() + 1);
            std::wstring lower = LowerW(rel);
            if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, L".map") != 0) { DeleteFileW(f.c_str()); continue; }   // .bak and the like
            std::wstring dest = MapSourceDir() + L"\\" + rel;
            std::string bytes;
            if (!ReadBytes(f, bytes)) { ++r.failed; LOG("prefabs: cannot read %S", f.c_str()); continue; }
            bool existed = GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES;
            if (existed && !backup::BeforeWrite(dest)) { ++r.failed; LOG("prefabs: %S kept as it is (no backup, the user said No)", dest.c_str()); continue; }
            fw::NoteSelfWriteBegin();
            bool ok = WriteBytes(dest, Delocalize(bytes));
            fw::NoteSelfWriteEnd();
            if (!ok) { ++r.failed; LOG("prefabs: cannot write %S", dest.c_str()); continue; }
            ++r.moved; if (existed) ++r.overwritten;
            LOG("prefabs: %S -> %S%s", f.c_str(), dest.c_str(), existed ? " (replaced, backup made)" : "");
            DeleteFileW(f.c_str());
        }
        return r;
    }

    void DeleteMirrorFolder()
    {
        std::wstring base = MirrorDir();
        if (base.empty()) return;
        RemoveTree(base);
        g_mirrored.clear();
        LOG("prefabs: %S removed", base.c_str());
    }

    void ResetMirrorPrefabs()
    {
        std::wstring base = MirrorDir();
        if (!base.empty()) RemoveTree(base + L"\\_prefabs");
        g_mirrored.clear();
    }

    std::string Status()
    {
        std::string s = "mirror: " + Narrow(MirrorDir()) + ", " + std::to_string(g_mirrored.size()) + " prefab(s)";
        for (const auto& k : g_mirrored) s += "\n  " + k;
        return s;
    }
}
