#pragma once
#include <string>
#include <vector>
#include <cstdint>

// Prefab mirror: a client missing a prefab the host's map uses (or holding another version) gets the host's file in map_source\_teamcreate_temp\_prefabs\<same relative path>,
// and its temporary map refers to that copy (nested prefabs too). The wire carries the host's real paths ("_prefabs/..."): text is localized on the way in, delocalized on the way out.
// Saving the temporary map moves the mirror onto _prefabs (an existing file is backed up first), discarding deletes _teamcreate_temp.
namespace prefabs
{
    constexpr const char* kMirrorPrefix = "_teamcreate_temp/_prefabs/";   // key prefix of a mirrored prefab
    constexpr const char* kRealPrefix   = "_prefabs/";

    std::wstring MapSourceDir();                 // <root>\map_source from the open map, else the mod tools root ("" = unknown)
    std::wstring MirrorDir();                    // <map_source>\_teamcreate_temp
    std::wstring RealPath(const std::string& key);     // <map_source>\<key>
    std::wstring MirrorPath(const std::string& key);   // <map_source>\_teamcreate_temp\<key>

    // Which prefab keys ("_prefabs/zm/x.map", lower case) are mirrored right now (main thread).
    bool   IsMirrored(const std::string& key);
    void   MarkMirrored(const std::string& key);
    void   ClearMirror();                        // forget (files untouched)
    size_t RescanMirror();                       // rebuild the set from the files under _teamcreate_temp\_prefabs
    bool   MirrorEmpty();

    // Text translation (no-ops while nothing is mirrored).
    std::string Localize(const std::string& text);      // "_prefabs/x" -> "_teamcreate_temp/_prefabs/x" for mirrored x
    std::string Delocalize(const std::string& text);    // "_teamcreate_temp/_prefabs/" -> "_prefabs/" everywhere
    std::string DelocalizeKey(const std::string& key);  // scope key of a mirrored file -> the real key

    // Inventory: every prefab a map text refers to, recursively, with size and hash of the file on this side.
    struct Item { std::string key; uint32_t size = 0; uint64_t hash = 0; bool missing = false; };
    std::vector<std::string> Refs(const std::string& text);      // "_prefabs/..." keys referenced by the text (also mirrored ones, delocalized)
    uint64_t Hash(const std::string& bytes);                     // FNV-1a 64
    bool ReadBytes(const std::wstring& path, std::string& out);
    // walk from `rootText`, files read on this side
    std::vector<Item> Inventory(const std::string& rootText, const std::vector<std::wstring>* dummy = nullptr);

    // Leaving a temporary map: move the mirror onto _prefabs (existing files backed up through backup.cpp, references rewritten) or just delete it.
    struct MoveResult { size_t moved = 0, overwritten = 0, failed = 0; };
    MoveResult MoveMirrorToReal();
    void DeleteMirrorFolder();
    void ResetMirrorPrefabs();                   // delete the mirrored prefab files (a new fetch starts) and forget them, the temporary map stays
    std::string Status();
}
