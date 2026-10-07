#pragma once
#include "assetfiles.h"

// Radiant's converted-preview cache (<root>\share\assetconvert\ToolsGfx): what Radiant builds from a source image or
// .xmodel_bin before drawing it. File names depend only on the source bytes and the conversion settings, so a copy
// from another PC is used as is and Radiant skips its own conversion.
namespace assetfiles
{
    constexpr uint64_t ConvertedMax = 64ull * 1024 * 1024;   // one converted file
    constexpr size_t ConvertedCount = 64;                     // converted files offered per asset
    // Radiant's file hash: MD4 of four zero bytes followed by the file (16 raw bytes).
    std::string RadiantHash(const std::string& bytes);
    // A cache key as Radiant prints it: the 16 bytes as four little-endian dwords, %08x each.
    std::string KeyHex(const std::string& hash16);
    // Key of the pct100 mesh Radiant converts for drawing an .xmodel_bin, from that file's RadiantHash.
    std::string MeshKey(const std::string& fileHash16);
    // Wire check: "ToolsGfx/images/v<N>/<settings>/<file>.lz4" or "ToolsGfx/xmeshes/v<N>/<mesh>/<file>.lz4".
    bool ConvertedPath(const std::string& path);

    class ConvertedCache
    {
        fs::path root_;   // <root>\share\assetconvert
        struct Seen { uint64_t size = 0; int64_t time = 0; std::string hash; };
        std::map<std::wstring, Seen> radiantHashes_, fileHashes_;
        struct Dir { int64_t time = 0; std::vector<std::pair<std::string, std::string>> files; };   // key hex, file name
        std::map<std::string, Dir> dirs_;                     // settings folder -> its files
        std::multimap<std::string, std::string> byKey_;       // key hex -> "<settings>/<file>"
        std::string imageVersion_;
        uint64_t refreshed_ = 0;
        void RefreshImages();
        bool Stat(const fs::path& path, uint64_t& size, int64_t& time);
    public:
        explicit ConvertedCache(const fs::path& convertRoot);
        const fs::path& Root() const { return root_; }
        // The converted files Radiant made from this source file, as "ToolsGfx/..." paths (empty for other file types).
        std::vector<std::string> For(const fs::path& source);
        // Size and Hash() of a converted file, false when it is gone or too big.
        bool Describe(const std::string& path, File& out);
    };
}
