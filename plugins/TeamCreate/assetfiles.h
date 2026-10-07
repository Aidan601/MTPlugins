#pragma once
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

// Disk-only asset operations, also exercised by tools/assettest.cpp.
namespace assetfiles
{
    namespace fs = std::filesystem;
    constexpr size_t MaxFile = 512ull * 1024 * 1024;
    struct Block
    {
        std::string name, type, parent;
        std::map<std::string, std::string> values;
        size_t begin = 0, end = 0;
    };
    struct File { std::string path, hash; uint32_t size = 0; };
    struct Asset
    {
        Block block;
        std::string gdt;
        std::vector<File> files;
        std::vector<std::string> dependencies;
        // Sender-side only (not on the wire): dependency ids this PC cannot define, left out of `dependencies`,
        // and file references whose file is not on this PC (the definition is streamed without them, as it is here).
        std::vector<std::string> undefined, missingFiles;
        // Property -> root-relative file path. Wire definitions retain original property values.
        std::map<std::string, std::string> paths;
        std::string Id() const { return block.type + ":" + block.name; }
    };
    std::string Lower(std::string s);
    std::string Hash(const std::string& bytes);
    std::string Read(const fs::path& path, size_t limit = MaxFile);
    void Write(const fs::path& path, const std::string& bytes);
    bool SafeRelative(const std::string& path);
    // Lexical join of a root-relative base ("model_export/" or "") and a relative reference that may use
    // "." and ".." ("../_custom/x/door.xmodel_bin"), empty when the reference escapes the root.
    std::string LexicalJoin(const std::string& base, const std::string& relative);
    // Base directories the converter resolves a file-valued property against, most specific first
    // (xmodel filenames live under model_export, xanims under xanim_export, images at the root).
    std::vector<std::string> ReferenceBases(const std::string& type);
    fs::path Under(const fs::path& root, const std::string& relative);
    std::vector<Block> Parse(const std::string& text);
    std::string Emit(const Block& block);
    std::string Merge(const std::string& text, const Block& block, bool remove = false);
    std::set<std::string> Tokens(const std::string& text);
    // Decoder for the model export's *LZ4* container, bounded before allocation.
    std::string ModelText(const std::string& bytes);
    // Retention of the external backups: session folders under `all` (asset_backups) older than `days` are removed,
    // except the newest `keep`, `current` and one an unrecovered session still points at. days 0 = never, keep 0 = no floor.
    // Only folders named like a session stamp are ever removed, links inside are not followed.
    struct Pruned { size_t sessions = 0, kept = 0; uint64_t bytes = 0; };
    Pruned PruneBackups(const fs::path& all, const fs::path& current, unsigned days, unsigned keep);
    // Adds each of `folders` (root-relative) to bin/converter_gdt_dirs_0.txt unless listed (backed up first).
    void RegisterConverter(const fs::path& root, const fs::path& backup, std::set<std::string> folders);
    // Saved streamed assets: their own folder and GDT, never the user's folders or GDTs. Files keep their root-relative path under it
    // ("teamcreate_assets/model_export/x.xmodel_bin").
    const std::string SavedFolder = "teamcreate_assets";
    const std::string SavedGdt = "teamcreate_assets/teamcreate_assets.gdt";
    std::string SavedPath(const std::string& path);   // where a streamed file goes when saved (a path already under SavedFolder stays)
    // A local definition a saved streamed asset replaced for good is also written here, GDT syntax under a non-.gdt name, next to the
    // asset that replaced it. Nothing prunes this file.
    const std::string ReplacedRecord = "teamcreate_assets/replaced_assets.txt";

    class Store
    {
        fs::path root_, backup_;
        void* lock_ = nullptr;
        std::map<std::string, Asset> staged_;
        struct Hidden { std::string path; Block block; };
        std::map<std::string, Hidden> hidden_;
        std::map<std::string, std::string> owned_; // path -> last bytes hash, never remove unowned files
        // Radiant's converted preview files this session installed for a staged asset (root-relative, under share/assetconvert):
        // removed with the asset when it is discarded, kept when it is saved.
        std::map<std::string, std::vector<File>> converted_;
        std::map<std::string, fs::file_time_type> hiddenSeen_;   // local GDT with hidden entries -> write time last looked at (HiddenReturned)
        // A local GDT with hidden entries: hash of its bytes before the session (the one copy in the backups) and as this
        // session last wrote it, clean while nothing else changed it, which is when those earlier bytes can go back as a whole.
        struct Local { std::string pristine, written; bool clean = true; };
        std::map<std::string, Local> local_;
        std::string recovered_;
        void Replace(const std::string& path, const std::string& bytes);
        void Publish();
        void Journal();
        void Recover();
    public:
        Store(const fs::path& root, const fs::path& outsideBackup);
        ~Store();
        Store(const Store&) = delete;
        Store& operator=(const Store&) = delete;
        const fs::path& Root() const { return root_; }
        const fs::path& Backup() const { return backup_; }
        // What the constructor cleaned up from an interrupted previous session (empty when there was none).
        const std::string& Recovered() const { return recovered_; }
        // incoming files have already been hash-checked. localGdt is the duplicate definition, if any.
        void Stage(const Asset& asset, const std::vector<std::string>& bytes, const std::string& localGdt);
        void Save(const std::set<std::string>& ids);   // into SavedFolder / SavedGdt, the rest stay temporary
        // Returns what is worth telling the user ("" = nothing): converted previews removed, a local entry found back in its GDT.
        std::string Finish();
        // Converted preview files the download wrote for `id` (root-relative, with hashes), ignored for an asset not staged.
        void NoteConverted(const std::string& id, const std::vector<File>& files);
        // Local GDTs with hidden entries, looked at when their write time moved: ids whose entry is back in the file
        // (APE's Save All of an asset held open across the hide), each reported once. gdtdb sees a duplicate until session end.
        std::vector<std::string> HiddenReturned();
        bool Has(const std::string& id) const { return staged_.count(id) != 0; }
        // The local GDT whose definition of `id` the streamed one temporarily replaced ("" = none): saving it replaces that one for good.
        std::string HiddenIn(const std::string& id) const { auto h = hidden_.find(id); return h == hidden_.end() ? std::string() : h->second.path; }
        size_t Count() const { return staged_.size(); }
    };
}
