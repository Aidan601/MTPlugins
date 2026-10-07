#include <windows.h>
#include "assetconverted.h"
#include <algorithm>
#include <cstring>

namespace assetfiles
{
    namespace
    {
        struct Md4
        {
            uint32_t s[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
            uint64_t length = 0; unsigned char buf[64] = {}; size_t used = 0;
            static uint32_t Rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
            void Block(const unsigned char* p)
            {
                uint32_t x[16]; for (int i = 0; i < 16; ++i) x[i] = uint32_t(p[i * 4]) | uint32_t(p[i * 4 + 1]) << 8 | uint32_t(p[i * 4 + 2]) << 16 | uint32_t(p[i * 4 + 3]) << 24;
                uint32_t a = s[0], b = s[1], c = s[2], d = s[3], t;
                static const int r1[4] = {3, 7, 11, 19}, r2[4] = {3, 5, 9, 13}, r3[4] = {3, 9, 11, 15};
                static const int k2[16] = {0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15}, k3[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
                for (int i = 0; i < 16; ++i) { t = Rol(a + ((b & c) | (~b & d)) + x[i], r1[i % 4]); a = d; d = c; c = b; b = t; }
                for (int i = 0; i < 16; ++i) { t = Rol(a + ((b & c) | (b & d) | (c & d)) + x[k2[i]] + 0x5a827999, r2[i % 4]); a = d; d = c; c = b; b = t; }
                for (int i = 0; i < 16; ++i) { t = Rol(a + (b ^ c ^ d) + x[k3[i]] + 0x6ed9eba1, r3[i % 4]); a = d; d = c; c = b; b = t; }
                s[0] += a; s[1] += b; s[2] += c; s[3] += d;
            }
            void Update(const unsigned char* p, size_t n)
            {
                length += n;
                if (used) { size_t take = std::min(n, 64 - used); memcpy(buf + used, p, take); used += take; p += take; n -= take; if (used == 64) { Block(buf); used = 0; } }
                for (; n >= 64; p += 64, n -= 64) Block(p);
                if (n) { memcpy(buf, p, n); used = n; }
            }
            void Update(const std::string& bytes) { Update(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()); }
            std::string Final()
            {
                uint64_t bits = length * 8; unsigned char pad[72] = {0x80}; size_t n = (used < 56 ? 56 : 120) - used;
                Update(pad, n); unsigned char len[8]; for (int i = 0; i < 8; ++i) len[i] = unsigned char(bits >> (8 * i)); Update(len, 8);
                std::string out(16, '\0'); for (int i = 0; i < 16; ++i) out[i] = char(s[i / 4] >> (8 * (i % 4))); return out;
            }
        };
        bool IsImage(const std::string& ext)
        {
            static const std::set<std::string> exts = {".tga", ".tif", ".tiff", ".dds", ".png", ".jpg", ".jpeg", ".exr", ".hdr"};
            return exts.count(ext) != 0;
        }
        int64_t FileTime(const FILETIME& t) { return int64_t(uint64_t(t.dwHighDateTime) << 32 | t.dwLowDateTime); }
        bool IsHex(const std::string& s) { return s.size() == 32 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }); }
        // "<anything>_<32 hex>.lz4" -> the hex, else empty.
        std::string KeyOf(const std::string& name)
        {
            if (name.size() < 38 || Lower(name.substr(name.size() - 4)) != ".lz4" || name[name.size() - 37] != '_') return {};
            std::string hex = name.substr(name.size() - 36, 32); return IsHex(hex) ? hex : std::string();
        }
        // Calls fn(name, isDirectory, lastWrite) for each entry of dir.
        template<class Fn> void List(const fs::path& dir, Fn fn)
        {
            WIN32_FIND_DATAW d; HANDLE h = FindFirstFileExW((dir / L"*").c_str(), FindExInfoBasic, &d, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE) return;
            do
            {
                if (!wcscmp(d.cFileName, L".") || !wcscmp(d.cFileName, L"..") || (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
                fn(fs::path(d.cFileName).u8string(), (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, FileTime(d.ftLastWriteTime));
            } while (FindNextFileW(h, &d));
            FindClose(h);
        }
        // Radiant only reads the newest cache format of its build: the highest v<N> folder.
        std::string NewestVersion(const fs::path& dir)
        {
            std::string best; long bestN = -1;
            List(dir, [&](const std::string& name, bool isDir, int64_t)
            {
                if (!isDir || name.size() < 2 || name[0] != 'v' || name.find_first_not_of("0123456789", 1) != std::string::npos || name.size() > 6) return;
                long n = strtol(name.c_str() + 1, nullptr, 10); if (n > bestN) { bestN = n; best = name; }
            });
            return best;
        }
    }

    std::string RadiantHash(const std::string& bytes)
    {
        Md4 m; unsigned char seed[4] = {}; m.Update(seed, 4); m.Update(bytes); return m.Final();
    }
    std::string KeyHex(const std::string& hash16)
    {
        if (hash16.size() != 16) return {};
        char out[33];
        for (int i = 0; i < 4; ++i)
        {
            uint32_t v = 0; for (int j = 0; j < 4; ++j) v |= uint32_t(uint8_t(hash16[i * 4 + j])) << (8 * j);
            snprintf(out + i * 8, 9, "%08x", v);
        }
        return std::string(out, 32);
    }
    std::string MeshKey(const std::string& fileHash16)
    {
        // The settings block Radiant hashes after the file for its in-editor mesh (0x1412BAB60 -> 0x1412CD720):
        // -1, 1.0f, then five floats (32, 1/32, 1/16, 1/32, 1) and the percent scale 1.0f, all as little-endian dwords.
        static const uint32_t block[8] = {0xffffffff, 0x3f800000, 0x42000000, 0x3d000000, 0x3d800000, 0x3d000000, 0x3f800000, 0x3f800000};
        unsigned char bytes[32]; for (int i = 0; i < 32; ++i) bytes[i] = unsigned char(block[i / 4] >> (8 * (i % 4)));
        Md4 m; m.Update(fileHash16); m.Update(bytes, 32); return m.Final();
    }
    bool ConvertedPath(const std::string& path)
    {
        if (!SafeRelative(path)) return false;
        std::vector<std::string> parts; size_t start = 0;
        for (size_t slash; (slash = path.find('/', start)) != std::string::npos; start = slash + 1) parts.push_back(path.substr(start, slash - start));
        parts.push_back(path.substr(start));
        if (parts.size() != 5 || parts[0] != "ToolsGfx" || (parts[1] != "images" && parts[1] != "xmeshes")) return false;
        if (parts[2].size() < 2 || parts[2].size() > 6 || parts[2][0] != 'v' || parts[2].find_first_not_of("0123456789", 1) != std::string::npos) return false;
        return !KeyOf(parts[4]).empty();
    }

    ConvertedCache::ConvertedCache(const fs::path& convertRoot) : root_(convertRoot) {}

    bool ConvertedCache::Stat(const fs::path& path, uint64_t& size, int64_t& time)
    {
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
        size = uint64_t(a.nFileSizeHigh) << 32 | a.nFileSizeLow; time = FileTime(a.ftLastWriteTime); return true;
    }

    // Index of every converted image by key. The first call lists them all (73k files in 187 settings folders took
    // well under a second), later calls, at most every 5 s, re-list only folders whose write time moved.
    void ConvertedCache::RefreshImages()
    {
        if (refreshed_ && GetTickCount64() - refreshed_ < 5000) return;
        refreshed_ = GetTickCount64();
        fs::path images = root_ / L"ToolsGfx" / L"images";
        std::string version = NewestVersion(images);
        if (version != imageVersion_) { dirs_.clear(); byKey_.clear(); imageVersion_ = version; }
        if (version.empty()) return;
        std::map<std::string, int64_t> now;
        List(images / fs::u8path(version), [&](const std::string& name, bool isDir, int64_t time) { if (isDir) now[name] = time; });
        for (auto it = dirs_.begin(); it != dirs_.end();)
        {
            auto seen = now.find(it->first);
            if (seen != now.end() && seen->second == it->second.time) { ++it; continue; }
            for (const auto& f : it->second.files)
            {
                auto range = byKey_.equal_range(f.first);
                for (auto k = range.first; k != range.second; ++k) if (k->second == it->first + "/" + f.second) { byKey_.erase(k); break; }
            }
            it = dirs_.erase(it);
        }
        for (const auto& d : now)
        {
            if (dirs_.count(d.first)) continue;
            Dir& dir = dirs_[d.first]; dir.time = d.second;
            List(images / fs::u8path(version) / fs::u8path(d.first), [&](const std::string& name, bool isDir, int64_t)
            {
                std::string key = isDir ? std::string() : KeyOf(name); if (key.empty()) return;
                dir.files.push_back({key, name}); byKey_.insert({key, d.first + "/" + name});
            });
        }
    }

    std::vector<std::string> ConvertedCache::For(const fs::path& source)
    {
        std::vector<std::string> out;
        std::string ext = Lower(source.extension().u8string()), name = source.filename().u8string();
        bool mesh = ext == ".xmodel_bin";
        if (!mesh && !IsImage(ext)) return out;
        uint64_t size = 0; int64_t time = 0; if (!Stat(source, size, time) || size > MaxFile) return out;
        std::string meshVersion = mesh ? NewestVersion(root_ / L"ToolsGfx" / L"xmeshes") : std::string();
        if (mesh && (meshVersion.empty() || GetFileAttributesW((root_ / L"ToolsGfx" / L"xmeshes" / fs::u8path(meshVersion) / source.stem()).c_str()) == INVALID_FILE_ATTRIBUTES)) return out;
        if (!mesh) { RefreshImages(); if (byKey_.empty()) return out; }
        Seen& seen = radiantHashes_[source.wstring()];
        if (seen.hash.empty() || seen.size != size || seen.time != time)
        {
            try { seen.hash = RadiantHash(Read(source)); seen.size = size; seen.time = time; }
            catch (...) { radiantHashes_.erase(source.wstring()); return out; }
        }
        if (!mesh)
        {
            // Radiant names the file after the source file (a cube or lat-long source gives one file per face).
            std::string prefix = Lower(name) + "_";
            auto range = byKey_.equal_range(KeyHex(seen.hash));
            for (auto k = range.first; k != range.second; ++k)
            {
                std::string file = k->second.substr(k->second.find('/') + 1);
                if (file.size() > prefix.size() && Lower(file.substr(0, prefix.size())) == prefix) out.push_back("ToolsGfx/images/" + imageVersion_ + "/" + k->second);
            }
        }
        else
        {
            // One folder per .xmodel_bin (named after it), one file per LOD percent. The editor's own pct100 key is
            // computed exactly, other percents and settings (auto LODs) take the newest file of that percent.
            const std::string& version = meshVersion; std::string stem = source.stem().u8string(), exact = KeyHex(MeshKey(seen.hash));
            struct Pick { std::string name; int64_t time = 0; bool exact = false; };
            std::map<std::string, Pick> picks;
            List(root_ / L"ToolsGfx" / L"xmeshes" / fs::u8path(version) / source.stem(), [&](const std::string& file, bool isDir, int64_t t)
            {
                std::string key = isDir ? std::string() : KeyOf(file); if (key.empty()) return;
                Pick& p = picks[Lower(file.substr(0, file.size() - 37))];
                bool isExact = key == exact;
                if (p.name.empty() || (isExact && !p.exact) || (!p.exact && t > p.time)) { p.name = file; p.time = t; p.exact = isExact; }
            });
            for (const auto& p : picks) out.push_back("ToolsGfx/xmeshes/" + version + "/" + stem + "/" + p.second.name);
        }
        out.erase(std::remove_if(out.begin(), out.end(), [](const std::string& p) { return !ConvertedPath(p); }), out.end());
        std::sort(out.begin(), out.end()); return out;
    }

    bool ConvertedCache::Describe(const std::string& path, File& out)
    {
        if (!ConvertedPath(path)) return false;
        fs::path file = root_ / fs::u8path(path);
        uint64_t size = 0; int64_t time = 0; if (!Stat(file, size, time) || size > ConvertedMax) return false;
        Seen& seen = fileHashes_[file.wstring()];
        if (seen.hash.empty() || seen.size != size || seen.time != time)
        {
            try { std::string bytes = Read(file, size_t(ConvertedMax)); seen.hash = Hash(bytes); seen.size = bytes.size(); seen.time = time; }
            catch (...) { fileHashes_.erase(file.wstring()); return false; }
        }
        out.path = path; out.hash = seen.hash; out.size = uint32_t(seen.size); return true;
    }
}
