// MAP: wire validation/serialization, worker (host arbitration, pull transfers, save promotion), public lifecycle.
#include "compat.h"
#include "assets.h"
#include "assetfiles.h"
#include "assetresolver.h"
#include "assetconverted.h"
#include "mapsync.h"
#include "presence.h"
#include "net.h"
#include <algorithm>
#include <atomic>
#include <functional>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <compressapi.h>
#include <commctrl.h>
#include <shellapi.h>
#pragma comment(lib, "cabinet.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
    using namespace assetfiles;
    constexpr uint32_t Chunk = 256 * 1024;
    constexpr size_t Window = 6;           // chunk requests in flight per file (1.5 MB)
    constexpr uint64_t LargeAsset = 100ull * 1024 * 1024;   // warn once about a source set this big
    constexpr uint64_t CacheMax = 4ull * 1024 * 1024 * 1024; // received-file cache, oldest trimmed at session start
    constexpr size_t MaxBundle = 512ull * 1024 * 1024;
    constexpr size_t GraphMax = 65536;     // assets one scan follows (map references, then their materials and images, stock ones included)
    constexpr size_t QueueMax = 65536;     // manifests waiting for a transfer
    constexpr size_t InboxMessages = 131072, InboxBytes = 256ull * 1024 * 1024;   // undelivered messages (a host announces 2 per asset in one burst)
    std::mutex g_mutex;
    std::condition_variable g_wake;
    std::deque<net::Message> g_messages;
    struct Saved { std::wstring path; std::string text; };
    std::deque<Saved> g_saves;
    uint64_t g_statusTick = 0;   // when g_status last changed
    std::string g_map, g_text, g_status = "asset streaming off", g_progress;   // g_progress: live transfer line, shown instead of g_status while set
    size_t g_queueBytes = 0;
    bool g_host = false;
    std::atomic<bool> g_sendConverted{true};   // [assets] send_converted
    std::atomic<unsigned> g_backupDays{30}, g_backupKeep{10};   // [backup] asset_days / asset_keep
    std::atomic<bool> g_stop{false}, g_running{false};
    std::atomic<bool> g_dropped{false};   // a frame was dropped (queue full): ask senders to resend manifests
    struct WorkerThread
    {
        std::thread thread;
        ~WorkerThread() { g_stop = true; g_wake.notify_all(); if (thread.joinable()) thread.join(); }
    } g_worker;
    uint64_t g_lastSnapshot = 0;
    // Session end (EndSession): the worker reports which streamed assets the saved maps use, g_keep is saved when it stops
    struct EndSummary { std::vector<std::string> ids, replaces, missing; std::string backup; };
    EndSummary g_summary;                         // under g_mutex
    uint64_t g_summaryWanted = 0, g_summaryDone = 0;
    std::set<std::string> g_keep;
    bool g_ending = false;                        // EndSession's box is up (its modal loop runs the tick)
    int g_endAnswer = -1;                         // SetEndAnswer: -1 ask, 0 discard, 1 keep
    std::atomic<size_t> g_streamed{0};            // temporary assets the worker holds (HasStreamed)

    void Event(const std::string& status)
    {
        { std::lock_guard<std::mutex> lock(g_mutex); g_status = status; g_statusTick = GetTickCount64(); }
        LOG("assets: %s", status.c_str());
    }
    // Log only: sender-side housekeeping (stock models naming fastfile-only materials, placeholder files)
    void Note(const std::string& text) { LOG("assets: %s", text.c_str()); }
    void Check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
    std::string Size(uint64_t bytes)
    {
        char buf[32];
        if (bytes >= 1024ull * 1024 * 1024) snprintf(buf, sizeof buf, "%.1f GB", bytes / (1024.0 * 1024 * 1024));
        else if (bytes >= 1024 * 1024) snprintf(buf, sizeof buf, "%.1f MB", bytes / (1024.0 * 1024));
        else if (bytes >= 1024) snprintf(buf, sizeof buf, "%llu KB", (unsigned long long)(bytes / 1024));
        else snprintf(buf, sizeof buf, "%llu B", (unsigned long long)bytes);
        return buf;
    }
    uint64_t TotalBytes(const Asset& a) { uint64_t n = 0; for (const auto& f : a.files) n += f.size; return n; }
    // Chunk compression through Windows' XPRESS Huffman codec (cabinet.dll): 2 to 4 times smaller for uncompressed
    // textures, no gain (sent raw) for LZ4 models or PNG
    struct Codec
    {
        COMPRESSOR_HANDLE c = nullptr; DECOMPRESSOR_HANDLE d = nullptr;
        ~Codec() { if (c) CloseCompressor(c); if (d) CloseDecompressor(d); }
        bool Compress(const std::string& in, std::string& out)
        {
            if (in.empty() || (!c && !CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &c))) return false;
            out.assign(in.size(), '\0'); SIZE_T n = 0;
            if (!::Compress(c, in.data(), in.size(), &out[0], out.size(), &n) || n == 0 || n >= in.size()) { out.clear(); return false; }
            out.resize(n); return true;
        }
        bool Decompress(const std::string& in, size_t rawSize, std::string& out)
        {
            if (!rawSize || in.empty() || (!d && !CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &d))) return false;
            out.assign(rawSize, '\0'); SIZE_T n = 0;
            return ::Decompress(d, in.data(), in.size(), &out[0], rawSize, &n) && n == rawSize;
        }
        bool SelfTest() { std::string z, back, sample(64 * 1024, 'a'); for (size_t i = 0; i < sample.size(); i += 7) sample[i] = char('b' + i % 20); return Compress(sample, z) && Decompress(z, sample.size(), back) && back == sample; }
    };
    // Received-file cache: <hash>-<size> under %LOCALAPPDATA%\RadiantPlugins\TeamCreate\asset_cache (RTC_ASSET_CACHE
    // overrides the folder, empty disables)
    std::string CacheName(const File& f) { return f.hash + "-" + std::to_string(f.size); }
    bool FromCache(const fs::path& cache, const File& f, std::string& bytes)
    {
        if (cache.empty()) return false;
        std::error_code ec; fs::path p = cache / CacheName(f);
        if (!fs::is_regular_file(p, ec) || fs::file_size(p, ec) != f.size) return false;
        try { bytes = Read(p); } catch (...) { return false; }
        if (Hash(bytes) != f.hash) { fs::remove(p, ec); bytes.clear(); return false; }
        fs::last_write_time(p, fs::file_time_type::clock::now(), ec);   // recently used, trimmed last
        return true;
    }
    void ToCache(const fs::path& cache, const File& f, const std::string& bytes)
    {
        if (cache.empty() || bytes.size() != f.size) return;
        std::error_code ec; fs::path p = cache / CacheName(f); if (fs::exists(p, ec)) return;
        try { fs::create_directories(cache); Write(p, bytes); } catch (...) {}
    }
    void TrimCache(const fs::path& cache, uint64_t maxBytes)
    {
        if (cache.empty()) return; std::error_code ec; if (!fs::is_directory(cache, ec)) return;
        struct Entry { fs::path path; fs::file_time_type time; uint64_t size; }; std::vector<Entry> entries; uint64_t total = 0;
        for (const auto& e : fs::directory_iterator(cache, ec)) { if (!e.is_regular_file(ec)) continue; uint64_t s = e.file_size(ec); entries.push_back({e.path(), e.last_write_time(ec), s}); total += s; }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.time < b.time; });
        for (const auto& e : entries) { if (total <= maxBytes) break; fs::remove(e.path, ec); total -= e.size; }
    }
    bool DataPath(const std::string& p, bool gdt = false)
    {
        if (!SafeRelative(p) || p != Lower(p)) return false;
        auto slash = p.find('/'); if (slash == std::string::npos) return false;
        // any top-level source folder (mappers add their own to converter_gdt_dirs), never the engine's or ours
        static const std::set<std::string> engine = {"bin", "share", "zone", "zone_source", "raw", "video", "players", "radiant", "sound", "lpc", "gdtdb", "deffiles", "win64", "pc", "docs_modtools", "crash_reports", "__streamed_assets"};
        if (engine.count(p.substr(0, slash))) return false;
        auto ext = fs::u8path(p).extension().u8string();
        if (gdt) return ext == ".gdt";
        static const std::set<std::string> exts = {".xmodel_bin", ".xmodel_export", ".xanim_bin", ".xanim_export", ".tga", ".tif", ".tiff", ".dds", ".png", ".jpg", ".jpeg", ".exr", ".hdr", ".wav", ".flac", ".fbx", ".obj", ".smd", ".xmodel_json"};
        return exts.count(ext) != 0;
    }
    bool IdValid(const std::string& id)
    {
        auto p = id.find(':'); if (p == std::string::npos) return false;
        std::string type = id.substr(0, p), name = id.substr(p + 1);
        return (type == "xmodel" || type == "material" || type == "image") && name.size() < 128 && SafeRelative(name) && name == Lower(name);
    }
    std::vector<uint8_t> Encode(const Asset& a)
    {
        net::Writer w; w.str(a.Id()); w.str(a.gdt); w.blob("{\r\n" + Emit(a.block) + "}\r\n");
        w.u16(uint16_t(a.files.size()));
        for (const auto& f : a.files) { w.str(f.path); w.str(f.hash); w.u32(f.size); }
        w.u16(uint16_t(a.paths.size())); for (const auto& p : a.paths) { w.str(p.first); w.str(p.second); }
        w.u16(uint16_t(a.dependencies.size())); for (const auto& d : a.dependencies) w.str(d);
        return w.buf;
    }
    std::string Fingerprint(const Asset& a)
    {
        // Storage location is provenance, not content: identical assets in other GDTs stay local
        Asset content = a; content.gdt.clear();
        for (const auto& p : content.paths) content.block.values[p.first] = p.second;
        auto b = Encode(content); return Hash(std::string(b.begin(), b.end()));
    }
    Asset Decode(net::Reader& r)
    {
        Asset a; std::string id = r.str(); a.gdt = r.str(); std::string definition = r.blob();
        Check(r.ok && IdValid(id) && DataPath(a.gdt, true) && definition.size() <= 1024 * 1024, "invalid asset manifest");
        auto blocks = Parse(definition); Check(blocks.size() == 1, "asset manifest must contain one definition"); a.block = blocks.front();
        Check(a.Id() == id && a.block.parent.empty(), "asset definition identity mismatch");
        size_t total = 0; uint16_t count = r.u16(); Check(count <= 128, "too many asset source files");
        std::set<std::string> paths;
        for (unsigned i = 0; i < count; ++i)
        {
            File f; f.path = r.str(); f.hash = r.str(); f.size = r.u32();
            Check(r.ok && DataPath(f.path) && f.hash.size() == 16 && paths.insert(f.path).second, "invalid asset source path/hash");
            total += f.size; Check(total <= MaxBundle, "asset bundle size limit"); a.files.push_back(std::move(f));
        }
        count = r.u16(); Check(count <= 128, "too many asset file properties");
        for (unsigned i = 0; i < count; ++i)
        {
            std::string k = r.str(), p = r.str(); Check(r.ok && paths.count(p) && a.block.values.count(k), "invalid asset file property");
            Check(a.paths.emplace(k, p).second, "duplicate asset file property");
        }
        count = r.u16(); Check(count <= 1024, "too many asset dependencies");
        for (unsigned i = 0; i < count; ++i) { std::string id2 = r.str(); Check(IdValid(id2), "invalid dependency id"); a.dependencies.push_back(id2); }
        Check(r.ok && r.pos == r.buf.size(), "truncated or trailing asset manifest"); return a;
    }
    // MSG_ASSET_CONVERTED body after the map: id, revision, then Radiant's converted files made from the sources
    void EncodeConverted(net::Writer& w, const std::string& id, const std::string& signature, const std::vector<File>& files)
    {
        w.str(id); w.str(signature); w.u16(uint16_t(files.size()));
        for (const auto& f : files) { w.str(f.path); w.str(f.hash); w.u32(f.size); }
    }
    std::vector<File> DecodeConverted(net::Reader& r)
    {
        uint16_t count = r.u16(); Check(r.ok && count <= ConvertedCount, "too many converted files");
        std::vector<File> files; std::set<std::string> paths;
        for (unsigned i = 0; i < count; ++i)
        {
            File f; f.path = r.str(); f.hash = r.str(); f.size = r.u32();
            Check(r.ok && ConvertedPath(f.path) && f.hash.size() == 16 && f.size <= ConvertedMax && paths.insert(Lower(f.path)).second, "invalid converted file");
            files.push_back(std::move(f));
        }
        Check(r.ok && r.pos == r.buf.size(), "truncated or trailing converted file list"); return files;
    }
    bool SameFiles(const std::vector<File>& a, const std::vector<File>& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) if (a[i].path != b[i].path || a[i].hash != b[i].hash || a[i].size != b[i].size) return false;
        return true;
    }
    void Send(uint32_t peer, uint16_t type, const net::Writer& w)
    {
        if (g_host && peer) net::SendTo(peer, type, w.buf); else net::Send(type, w.buf);
    }
    // mirror: a client's copy that matched the host's manifest, never offered back
    struct Record { Asset asset; std::string signature; bool local = true, mirror = false; };
    struct Download
    {
        uint32_t peer = 0; Record record;
        size_t index = 0; std::vector<std::string> bytes;   // completed files
        std::string current;                                // file being received, preallocated
        std::set<uint32_t> pending; size_t received = 0; uint32_t next = 0;   // offsets requested and unanswered, chunks stored, next offset
        uint64_t transferred = 0, started = GetTickCount64(), asked = 0, progressed = GetTickCount64();
        size_t fromCache = 0, zipped = 0;                   // files from the cache, compressed chunks accepted
        // Radiant's converted files, pulled after the sources (wire index 0x8000 | k), installed into share\assetconvert before the refresh
        std::vector<File> converted; size_t convertedInstalled = 0; bool convertedOnly = false;
        std::vector<File> convertedWritten;                 // written by this download (root-relative), so a discard removes them
        size_t Sources() const { return record.asset.files.size(); }
        size_t Count() const { return Sources() + converted.size(); }
        const File& At(size_t i) const { return i < Sources() ? record.asset.files[i] : converted[i - Sources()]; }
        uint16_t Wire() const { return index < Sources() ? uint16_t(index) : uint16_t(0x8000 | (index - Sources())); }
        uint64_t Total() const { uint64_t n = 0; for (size_t i = convertedOnly ? Sources() : 0; i < Count(); ++i) n += At(i).size; return n; }
    };
    // Prefabs can reference more assets, including ones never entered in this editor
    std::string WithPrefabs(const fs::path& root, std::string text)
    {
        std::set<std::string> visited;
        std::string pending = text;
        for (unsigned depth = 0; depth < 32 && !pending.empty(); ++depth)
        {
            std::string next;
            // Map prefab paths contain backslashes
            std::string normalized = Lower(pending);
            for (size_t p = 0; (p = normalized.find("_prefabs/", p)) != std::string::npos;)
            {
                size_t end = normalized.find(".map", p); if (end == std::string::npos) break;
                std::string key = normalized.substr(p, end + 4 - p); p = end + 4;
                if (!SafeRelative(key) || !visited.insert(key).second || visited.size() > 512) continue;
                fs::path file = Under(root, "map_source/" + key);
                auto mirror = Under(root, "map_source/_teamcreate_temp/" + key); if (fs::exists(mirror)) file = mirror;
                if (fs::exists(file)) { auto bytes = Read(file, 32 * 1024 * 1024); if (text.size() + next.size() + bytes.size() < 64 * 1024 * 1024) next += bytes; }
            }
            text += next; pending = std::move(next);
        }
        return text;
    }
    // gdtdb /update as a child process, started asynchronously (the worker keeps answering chunk requests),
    // waited for at exit, never killed because it writes the database
    struct Refresh
    {
        HANDLE process = nullptr;
        ~Refresh() { if (process) CloseHandle(process); }
        bool Running() const { return process != nullptr; }
        bool Start(const fs::path& root)   // false: no gdtdb.exe
        {
            fs::path exe = root / L"gdtdb/gdtdb.exe"; if (!fs::exists(exe)) return false;
            std::wstring cmd = L"\"" + exe.wstring() + L"\" /update";
            STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};
            if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, root.c_str(), &si, &pi))
                throw std::runtime_error("cannot start gdtdb /update");
            CloseHandle(pi.hThread); process = pi.hProcess; return true;
        }
        int Poll()   // 0 still running, 1 finished, -1 failed
        {
            if (!process || WaitForSingleObject(process, 0) == WAIT_TIMEOUT) return process ? 0 : -1;
            DWORD code = 1; GetExitCodeProcess(process, &code); CloseHandle(process); process = nullptr; return code == 0 ? 1 : -1;
        }
        int Wait(DWORD ms) { if (process) WaitForSingleObject(process, ms); return Poll(); }
    };
    void RefreshDatabase(const fs::path& root)
    {
        Refresh r; if (!r.Start(root)) return;
        int result = r.Wait(30000);
        if (result == 0) throw std::runtime_error("gdtdb /update is still running; asset refresh pending");
        Check(result == 1, "gdtdb /update failed");
    }
    void Run(const fs::path& root, const fs::path& backup, const fs::path& cache)
    {
        std::unique_ptr<Store> store;
        try
        {
            store.reset(new Store(root, backup)); Resolver resolver(root);
            if (!store->Recovered().empty()) Event(store->Recovered());
            try
            {
                // after the recovery above, which may need an earlier session's copies
                Pruned p = PruneBackups(backup.parent_path(), backup, g_backupDays, g_backupKeep);
                if (p.sessions) Note("backups of " + std::to_string(p.sessions) + " session(s) older than " + std::to_string(g_backupDays.load()) + " days removed (" + std::to_string(p.bytes / (1024 * 1024)) + " MB), " + std::to_string(p.kept) + " kept");
            }
            catch (const std::exception& e) { Note(std::string("old backups not removed: ") + e.what()); }
            TrimCache(cache, CacheMax);
            Codec codec; const bool zip = codec.SelfTest();   // caps bit 0: sends and accepts compressed chunks
            std::set<uint32_t> zipPeers, seenCaps;            // peers with bit 0, peers whose caps arrived
            std::map<std::string, Record> records;
            std::map<std::string, std::pair<uint32_t, Record>> waiting;
            std::unique_ptr<Download> download;
            struct SavePlan { std::set<std::string> tokens, processed; };
            std::vector<SavePlan> savedTexts;
            std::string map, text, lastMap;
            uint64_t scanned = 0, announced = 0, refreshAt = 0, receivedAt = 0, progressAt = 0, sendingAt = 0, refreshSince = 0;
            bool refresh = false; Refresh refreshing; std::string sending;
            auto source = [&](const Record& r, const File& f)
            {
                return Under(root, (!r.local && store->Has(r.asset.Id()) ? "__streamed_assets/" : "") + f.path);
            };
            // Converted files: what this PC offers (recomputed at most every 8 s), what peers offer (by id), installed records still missing some
            ConvertedCache convertedCache(root / L"share" / L"assetconvert");
            struct Offer { uint32_t peer = 0; std::string signature; uint64_t at = 0; std::vector<File> files; };
            std::map<std::string, Offer> offered, offers;
            std::set<std::string> convertQueue;
            // Converted lists go out once per receiver and change (key: receiver, 0 = everyone), all again every 2 minutes
            std::map<std::pair<uint32_t, std::string>, std::string> convertedSent; uint64_t convertedSentReset = GetTickCount64();
            // Manifests go out once per receiver and revision (0 = everyone): periodic re-sends filled the host's upload.
            // A receiver that dropped frames asks for a resend (caps bit 1).
            std::map<std::pair<uint32_t, std::string>, std::string> manifestSent;
            std::map<uint32_t, uint64_t> resyncServed; uint64_t resyncAsked = 0; bool wasConnected = false;
            auto offer = [&](const Record& r) -> const std::vector<File>&
            {
                Offer& o = offered[r.asset.Id()];
                if (o.signature == r.signature && o.at && GetTickCount64() - o.at < 8000) return o.files;
                o.signature = r.signature; o.at = GetTickCount64(); o.files.clear();
                if (!g_sendConverted) return o.files;
                uint64_t total = 0; std::set<std::string> seen;
                for (const auto& f : r.asset.files)
                {
                    fs::path src; try { src = source(r, f); } catch (...) { continue; }
                    bool mesh = Lower(src.extension().u8string()) == ".xmodel_bin";
                    for (const auto& path : convertedCache.For(src))
                    {
                        File c; if (!seen.insert(Lower(path)).second || !convertedCache.Describe(path, c)) continue;
                        // A converted PNG can be bigger than its source: send an image's only when not bigger or the source is big
                        if (!mesh && c.size > f.size && f.size < 2 * 1024 * 1024) continue;
                        if (o.files.size() >= ConvertedCount || total + c.size > MaxBundle) break;
                        total += c.size; o.files.push_back(std::move(c));
                    }
                }
                return o.files;
            };
            std::function<void(const Record&, uint32_t, bool)> announce = [&](const Record& record, uint32_t peer, bool force)
            {
                // The host addresses each client that sent caps on its own (one ledger per client), a client's peer 0 is the host
                if (g_host && peer == 0) { for (uint32_t client : seenCaps) announce(record, client, force); return; }
                std::string& sentManifest = manifestSent[{peer, record.asset.Id()}];
                if (!force && sentManifest == record.signature) return;
                sentManifest = record.signature;
                if (force) convertedSent.erase({peer, record.asset.Id()});
                net::Writer w; w.str(map); auto b = Encode(record.asset); w.buf.insert(w.buf.end(), b.begin(), b.end()); Send(peer, net::MSG_ASSET_MANIFEST, w);
                const auto& files = offer(record); if (files.empty()) return;
                net::Writer c; c.str(map); EncodeConverted(c, record.asset.Id(), record.signature, files);
                std::string& sent = convertedSent[{peer, record.asset.Id()}], tag = Hash(std::string(c.buf.begin(), c.buf.end()));
                if (sent != tag) { sent = tag; Send(peer, net::MSG_ASSET_CONVERTED, c); }
            };
            auto absent = [&](const std::vector<File>& files)
            {
                std::vector<File> out; std::error_code ec;
                for (const auto& f : files) { try { if (!fs::exists(Under(convertedCache.Root(), f.path), ec)) out.push_back(f); } catch (...) {} }
                return out;
            };
            auto request = [&](uint32_t offset)
            {
                net::Writer w; w.str(map); w.str(download->record.asset.Id()); w.str(download->record.signature);
                w.u16(download->Wire()); w.u32(offset);
                if (download->index >= download->Sources()) w.str(download->At(download->index).path);   // 0x8000 index
                Send(download->peer, zip && zipPeers.count(download->peer) ? net::MSG_ASSET_REQUEST_Z : net::MSG_ASSET_REQUEST, w);
            };
            // Keep up to Window chunk requests in flight, resend repeats the unanswered ones
            auto ask = [&](bool resend = false)
            {
                if (!download) return;
                const auto& f = download->At(download->index);
                if (resend) for (uint32_t o : download->pending) request(o);
                while (download->pending.size() < Window && download->next < f.size)
                { request(download->next); download->pending.insert(download->next); download->next += Chunk; }
                download->asked = GetTickCount64();
            };
            auto finish = [&]()
            {
                if (download->convertedOnly)
                {
                    if (download->convertedInstalled) Event("received " + std::to_string(download->convertedInstalled) + " converted file(s) for " + download->record.asset.Id() + ": Radiant skips converting them");
                    store->NoteConverted(download->record.asset.Id(), download->convertedWritten);   // ignored for an identical local asset
                    download.reset(); return;
                }
                if (g_host)
                {
                    // host files win collisions between assets sharing a source path
                    for (size_t i = 0; i < download->record.asset.files.size(); ++i)
                    {
                        auto& f = download->record.asset.files[i]; fs::path p = Under(root, f.path);
                        if (!fs::exists(p)) p = Under(root, "__streamed_assets/" + f.path);
                        if (fs::exists(p)) { download->bytes[i] = Read(p); f.hash = Hash(download->bytes[i]); f.size = uint32_t(download->bytes[i].size()); }
                    }
                    download->record.signature = Fingerprint(download->record.asset);
                }
                Asset local; std::string localGdt;
                if (resolver.Find(download->record.asset.Id(), local)) localGdt = local.gdt;
                store->Stage(download->record.asset, download->bytes, localGdt);
                store->NoteConverted(download->record.asset.Id(), download->convertedWritten);
                for (size_t i = 0; i < download->bytes.size(); ++i) ToCache(cache, download->record.asset.files[i], download->bytes[i]);
                Record record = download->record; record.local = false; records[record.asset.Id()] = record;
                if (g_host) announce(record, 0, false);
                std::string how = download->fromCache ? std::to_string(download->fromCache) + " file(s) from the cache" : "";
                if (download->zipped) how += (how.empty() ? "" : ", ") + std::to_string(download->zipped) + " compressed chunk(s)";
                if (download->convertedInstalled) how += (how.empty() ? "" : ", ") + std::to_string(download->convertedInstalled) + " converted file(s)";
                Event("received " + record.asset.Id() + (how.empty() ? "" : " (" + how + ")") + "; " + std::to_string(store->Count()) + " temporary assets");
                download.reset(); refresh = true; refreshAt = GetTickCount64() + 1500;
            };
            // Prepare the current file, from the cache when present. True when chunks must be requested
            auto prepare = [&]()
            {
                const auto& f = download->At(download->index);
                download->pending.clear(); download->received = 0; download->next = 0; download->progressed = GetTickCount64();
                if (FromCache(cache, f, download->current)) { download->transferred += f.size; ++download->fromCache; return false; }
                download->current.assign(f.size, '\0'); return f.size != 0;
            };
            // The current file is complete: verify and keep a source, or install a converted file
            auto complete = [&]()
            {
                const auto& f = download->At(download->index);
                if (download->index < download->Sources())
                {
                    Check(Hash(download->current) == f.hash, "received asset checksum failed");
                    download->bytes.push_back(std::move(download->current));
                }
                else if (Hash(download->current) == f.hash)
                {
                    // named by content: an existing file has the same bytes
                    std::error_code ec; fs::path p = Under(convertedCache.Root(), f.path);
                    try { if (!fs::exists(p, ec)) { Write(p, download->current); ++download->convertedInstalled; download->convertedWritten.push_back({"share/assetconvert/" + f.path, f.hash, f.size}); } }
                    catch (const std::exception& e) { Note("could not install converted file " + f.path + ": " + e.what()); }
                    ToCache(cache, f, download->current); download->current.clear();
                }
                else { Note("converted file " + f.path + " changed on the sender; Radiant converts it here"); download->current.clear(); }
                ++download->index;
            };
            // Continue at the index: skip converted files already here, use the cache, request the rest, install when all is in
            std::function<void()> pump = [&]()
            {
                for (;;)
                {
                    std::error_code ec;
                    while (download->index >= download->Sources() && download->index < download->Count() && fs::exists(Under(convertedCache.Root(), download->At(download->index).path), ec)) ++download->index;
                    if (download->index == download->Count()) { finish(); return; }
                    if (prepare()) { ask(); return; }
                    complete();
                }
            };
            std::set<std::string> missing;   // dependency ids reported as undefined on this PC (once per session)
            std::set<std::string> returnedNoted; uint64_t hiddenChecked = 0;   // hidden local entries found back in their GDT (once per asset)
            auto scan = [&]()
            {
                std::string all = WithPrefabs(root, text); auto refs = resolver.References(all);
                std::deque<std::string> todo(refs.begin(), refs.end()); std::set<std::string> visited;
                while (!todo.empty() && !g_stop)
                {
                    std::string id = todo.front(); todo.pop_front(); if (!visited.insert(id).second) continue;
                    // past the cap the rest stays unstreamed, the worker lives on (a throw here ended every session on real maps)
                    if (visited.size() > GraphMax) { if (missing.insert("graph limit").second) Event("asset scan stopped at " + std::to_string(GraphMax) + " assets: the rest of the map's assets are not streamed"); break; }
                    auto old = records.find(id);
                    if (old != records.end() && !old->second.local) continue; // the host's accepted revision is authoritative
                    try
                    {
                        Asset a;
                        if (!resolver.Find(id, a))
                        {
                            // a dependency named inside a model export (map references are pre-filtered by gdt.db)
                            if (missing.insert(id).second) Note("cannot stream " + id + ": no GDT definition on this PC");
                            continue;
                        }
                        for (const auto& u : a.undefined)
                            if (missing.insert(id + " " + u).second) Note("cannot stream " + u + " for " + id + ": no GDT definition on this PC; " + id + " is sent without it");
                        for (const auto& f : a.missingFiles)
                            if (missing.insert(id + " file " + f).second) Note("streaming " + id + " without " + f + " (no such file on this PC)");
                        Record r{a, Fingerprint(a), true};
                        // same path/count checks on our own exports as on received metadata
                        auto encoded = Encode(a); net::Reader validate(encoded); Decode(validate);
                        if (old == records.end() || old->second.signature != r.signature) { records[id] = r; announce(r, 0, false); }
                        for (const auto& d : a.dependencies) todo.push_back(d);
                    }
                    catch (const std::exception& e) { if (missing.insert(id + " err " + e.what()).second) Event("cannot stream " + id + ": " + e.what()); }
                }
            };
            LOG("assets: external backup directory (made when a local asset is first replaced): %s", backup.u8string().c_str());
            Event(std::string("asset streaming ready (compressed chunks: ") + (zip ? "yes" : "no") + (cache.empty() ? ", no cache)" : ")"));
            uint64_t summaryWanted = 0;
            while (true)
            {
                std::deque<net::Message> incoming; std::deque<Saved> saves;
                {
                    std::unique_lock<std::mutex> lock(g_mutex);
                    g_wake.wait_for(lock, std::chrono::milliseconds(100), [] { return g_stop || !g_messages.empty() || !g_saves.empty() || g_summaryWanted > g_summaryDone; });
                    incoming.swap(g_messages); saves.swap(g_saves); g_queueBytes = 0; map = g_map; text = g_text;
                    summaryWanted = g_summaryWanted;   // read with the saves
                }
                g_streamed = store->Count();
                if (map != lastMap)
                {
                    // switching maps does not end the session's ownership of unsaved assets
                    waiting.clear(); download.reset(); offered.clear(); offers.clear(); convertQueue.clear(); convertedSent.clear(); manifestSent.clear();
                    lastMap = map; scanned = announced = 0;
                }
                for (const auto& saved : saves)
                {
                    auto tokens = Tokens(WithPrefabs(root, saved.text)); bool found = false;
                    for (auto& plan : savedTexts) if (plan.tokens == tokens) { plan.processed.clear(); found = true; break; }
                    if (!found) savedTexts.push_back({std::move(tokens), {}});
                }
                // A local GDT entry written again while hidden (APE Save All): gdtdb sees a duplicate until the session ends, said once per asset
                if (!g_stop && GetTickCount64() - hiddenChecked >= 5000)
                {
                    hiddenChecked = GetTickCount64();
                    for (const auto& id : store->HiddenReturned()) if (returnedNoted.insert(id).second)
                        Event("your GDT defines " + id + " again while the streamed one is active (saved from APE?): the asset database sees two until the session ends; yours stays as it is");
                }
                if (!g_stop && net::IsConnected() && !map.empty() && GetTickCount64() - scanned >= 5000)
                { scanned = GetTickCount64(); scan(); }
                for (const auto& message : incoming)
                {
                    try
                    {
                        net::Reader r(message.payload); std::string scope = r.str(); if (!r.ok || !mapsync::SameMap(scope, map)) continue;
                        if (message.type == net::MSG_ASSET_MANIFEST)
                        {
                            if (!g_host && message.from != net::kHostPeerId) continue;
                            Asset a = Decode(r); std::string id = a.Id(); Record candidate{a, Fingerprint(a), false};
                            receivedAt = GetTickCount64();
                            auto current = records.find(id);
                            // an offer can add an asset but never replace a host asset: an offering peer gets the host's revision when its offer differs
                            if (g_host && current != records.end()) { if (current->second.signature != candidate.signature) announce(current->second, message.from, true); continue; }
                            if (current != records.end() && current->second.signature == candidate.signature) continue;
                            if (download && download->record.asset.Id() == id && download->record.signature == candidate.signature) continue;
                            // a full queue drops the manifest, the sender repeats it within 15 minutes
                            if (waiting.size() >= QueueMax) { if (missing.insert("queue limit").second) Event("asset queue full (" + std::to_string(QueueMax) + "): later assets wait for the sender's next announce"); continue; }
                            // this PC's own copy is compared when the asset is picked (hashing its files): the inbox drains at network speed
                            waiting[id] = {message.from, candidate};
                            if (TotalBytes(a) > LargeAsset && missing.insert(id + " large").second) Event("queued " + id + " (" + Size(TotalBytes(a)) + " of source files): a long transfer");
                        }
                        else if (message.type == net::MSG_ASSET_CONVERTED)
                        {
                            if (!g_host && message.from != net::kHostPeerId) continue;
                            std::string id = r.str(), signature = r.str(); auto files = DecodeConverted(r); Check(IdValid(id), "invalid converted file asset id");
                            auto record = records.find(id);
                            if (g_host && record != records.end() && record->second.local) continue;   // the host converts its own assets
                            Offer& o = offers[id];
                            if (o.peer == message.from && o.signature == signature && SameFiles(o.files, files)) continue;
                            bool sameRevision = download && download->record.asset.Id() == id && download->record.signature == signature && download->peer == message.from;
                            // already pulling this asset's converted files: keep that list
                            if (sameRevision && download->index >= download->Sources()) continue;
                            o.peer = message.from; o.signature = signature; o.files = files;
                            if (sameRevision) download->converted = absent(files);
                            else if (record != records.end() && record->second.signature == signature && !absent(files).empty()) convertQueue.insert(id);
                        }
                        else if (message.type == net::MSG_ASSET_CAPS)
                        {
                            if (!g_host && message.from != net::kHostPeerId) continue;
                            uint8_t caps = r.u8(); if (!r.ok) continue;
                            if (caps & 1) zipPeers.insert(message.from); else zipPeers.erase(message.from);
                            // a peer's first caps is its hello: give it every manifest now, not at the next 10 s announce
                            if (g_host && seenCaps.insert(message.from).second) for (const auto& record : records) announce(record.second, message.from, true);
                            // bit 1: the peer dropped frames and asks for everything again (at most every 30 s per peer)
                            else if ((caps & 2) && GetTickCount64() - resyncServed[message.from] >= 30000)
                            {
                                resyncServed[message.from] = GetTickCount64();
                                for (const auto& record : records) if (g_host || (record.second.local && !record.second.mirror)) announce(record.second, message.from, true);
                                Note("resent manifests to peer " + std::to_string(message.from) + " (it dropped messages)");
                            }
                        }
                        else if (message.type == net::MSG_ASSET_REQUEST || message.type == net::MSG_ASSET_REQUEST_Z)
                        {
                            if (!g_host && message.from != net::kHostPeerId) continue;
                            std::string id = r.str(), signature = r.str(); unsigned index = r.u16(); uint32_t offset = r.u32();
                            std::string convertedPath; if (index & 0x8000) convertedPath = r.str();
                            auto it = records.find(id); if (!r.ok || r.pos != r.buf.size() || it == records.end() || it->second.signature != signature) continue;
                            if (index & 0x8000)
                            {
                                // a converted file this PC offered, looked up by path, a vanished one is left unanswered (the peer times out)
                                auto o = offered.find(id); if (o == offered.end() || o->second.signature != signature) continue;
                                auto c = std::find_if(o->second.files.begin(), o->second.files.end(), [&](const File& x) { return x.path == convertedPath; });
                                if (c == o->second.files.end() || offset > c->size) continue;
                                std::string bytes(std::min(Chunk, c->size - offset), '\0');
                                {
                                    std::ifstream file(Under(convertedCache.Root(), c->path), std::ios::binary); if (!file) continue;
                                    file.seekg(offset); if (!bytes.empty()) file.read(&bytes[0], std::streamsize(bytes.size())); if (!file) continue;
                                }
                                net::Writer w; w.str(map); w.str(id); w.str(signature); w.u16(uint16_t(index)); w.u32(offset);
                                std::string z;
                                if (message.type == net::MSG_ASSET_REQUEST_Z && zip && codec.Compress(bytes, z)) { w.u32(uint32_t(bytes.size())); w.blob(z); Send(message.from, net::MSG_ASSET_CHUNK_Z, w); }
                                else { w.blob(bytes); Send(message.from, net::MSG_ASSET_CHUNK, w); }
                                sending = "sending converted files for " + id; sendingAt = GetTickCount64();
                                continue;
                            }
                            if (index >= it->second.asset.files.size()) continue;
                            const auto& f = it->second.asset.files[index]; if (offset > f.size) continue;
                            // log a big one only once a peer really asks (identical stock files are never requested)
                            if (index == 0 && offset == 0 && TotalBytes(it->second.asset) > LargeAsset && missing.insert(id + " sending").second)
                                Event("sending " + id + " (" + Size(TotalBytes(it->second.asset)) + " of source files) to a peer: a long transfer");
                            auto path = source(it->second, f); std::ifstream file(path, std::ios::binary); Check(bool(file), "asset source no longer exists"); file.seekg(offset);
                            std::string bytes(std::min(Chunk, f.size - offset), '\0'); if (!bytes.empty()) file.read(&bytes[0], std::streamsize(bytes.size())); Check(bool(file), "short streamed source read");
                            net::Writer w; w.str(map); w.str(id); w.str(signature); w.u16(uint16_t(index)); w.u32(offset);
                            std::string z;
                            if (message.type == net::MSG_ASSET_REQUEST_Z && zip && codec.Compress(bytes, z)) { w.u32(uint32_t(bytes.size())); w.blob(z); Send(message.from, net::MSG_ASSET_CHUNK_Z, w); }
                            else { w.blob(bytes); Send(message.from, net::MSG_ASSET_CHUNK, w); }
                            uint64_t total = TotalBytes(it->second.asset), before = 0; for (unsigned i = 0; i < index; ++i) before += it->second.asset.files[i].size;
                            sending = "sending " + id + " (" + Size(before + offset + bytes.size()) + " of " + Size(total) + ")"; sendingAt = GetTickCount64();
                        }
                        else if (message.type == net::MSG_ASSET_CHUNK || message.type == net::MSG_ASSET_CHUNK_Z)
                        {
                            std::string id = r.str(), signature = r.str(); unsigned index = r.u16(); uint32_t offset = r.u32();
                            uint32_t raw = message.type == net::MSG_ASSET_CHUNK_Z ? r.u32() : 0; std::string bytes = r.blob();
                            if (!r.ok || r.pos != r.buf.size() || !download || message.from != download->peer || id != download->record.asset.Id() || signature != download->record.signature || index != download->Wire() || !download->pending.count(offset)) continue;
                            const auto& f = download->At(download->index);
                            if (message.type == net::MSG_ASSET_CHUNK_Z)
                            {
                                Check(raw == std::min<size_t>(Chunk, f.size - offset) && bytes.size() < raw, "invalid compressed asset chunk");
                                std::string plain; Check(codec.Decompress(bytes, raw, plain), "asset chunk decompression failed"); bytes.swap(plain); ++download->zipped;
                            }
                            Check(bytes.size() == std::min<size_t>(Chunk, f.size - offset), "invalid asset chunk size");
                            download->current.replace(offset, bytes.size(), bytes); download->pending.erase(offset); ++download->received;
                            download->transferred += bytes.size(); download->progressed = GetTickCount64();
                            if (download->received == (f.size + Chunk - 1) / Chunk) { complete(); pump(); } else ask();
                        }
                    }
                    catch (const std::exception& e) { Event(std::string("asset transfer rejected: ") + e.what()); download.reset(); }
                }
                // Picks run for at most half a second per pass so chunk requests from peers keep being answered
                for (uint64_t picking = GetTickCount64(); !g_stop && !download && !waiting.empty() && GetTickCount64() - picking < 500;)
                {
                    // Smallest first: definitions and small models arrive before a big texture.
                    auto it = waiting.begin();
                    for (auto w = waiting.begin(); w != waiting.end(); ++w) if (TotalBytes(w->second.second.asset) < TotalBytes(it->second.second.asset)) it = w;
                    uint32_t from = it->second.first; Record candidate = it->second.second; waiting.erase(it);
                    const std::string id = candidate.asset.Id();
                    try
                    {
                        // Compared with this PC's own definition now, not when the manifest arrived: hashing the files is the slow part
                        Asset local;
                        if (resolver.Find(id, local))
                        {
                            Record own{local, Fingerprint(local), true};
                            if (g_host) { records[id] = own; announce(own, 0, false); if (own.signature != candidate.signature) announce(own, from, true); continue; }
                            if (own.signature == candidate.signature) { candidate.local = true; candidate.mirror = true; records[id] = candidate; continue; }
                        }
                    }
                    catch (const std::exception& e) { if (missing.insert(id + " cmp " + e.what()).second) Event("cannot compare " + id + " with this PC's copy: " + e.what()); continue; }
                    auto current = records.find(id); if (current != records.end() && current->second.signature == candidate.signature) continue;
                    download.reset(new Download); download->peer = from; download->record = candidate;
                    auto o = offers.find(id);
                    if (o != offers.end() && o->second.peer == download->peer && o->second.signature == download->record.signature) download->converted = absent(o->second.files);
                    try { pump(); }
                    catch (const std::exception& e) { Event(std::string("asset install failed: ") + e.what()); download.reset(); }
                }
                // converted files offered for an asset this PC already has
                while (!g_stop && !download && waiting.empty() && !convertQueue.empty())
                {
                    std::string id = *convertQueue.begin(); convertQueue.erase(convertQueue.begin());
                    auto o = offers.find(id); auto record = records.find(id);
                    if (o == offers.end() || record == records.end() || record->second.signature != o->second.signature) continue;
                    auto files = absent(o->second.files); if (files.empty()) continue;
                    download.reset(new Download); download->peer = o->second.peer; download->record = record->second;
                    download->index = download->Sources(); download->converted = std::move(files); download->convertedOnly = true;
                    try { pump(); }
                    catch (const std::exception& e) { Note(std::string("converted file install failed: ") + e.what()); download.reset(); }
                }
                if (!g_stop && download && download->index >= download->Sources() && GetTickCount64() - download->progressed > 8000)
                {
                    // converted files are an extra: a missing one must not cost the asset
                    Note("converted files for " + download->record.asset.Id() + " did not arrive; Radiant converts the rest here");
                    try { finish(); } catch (const std::exception& e) { Event(std::string("asset install failed: ") + e.what()); download.reset(); }
                }
                if (!g_stop && download && GetTickCount64() - download->progressed > 30000)
                { Event("asset transfer timed out: " + download->record.asset.Id()); download.reset(); }
                if (!g_stop && download && GetTickCount64() - download->asked > 3000) ask(true);
                if (GetTickCount64() - progressAt >= 500)
                {
                    // live line for the pane and the `assets` command
                    progressAt = GetTickCount64(); std::string line;
                    if (download)
                    {
                        uint64_t total = download->Total(), done = download->transferred;
                        double secs = std::max(0.001, double(GetTickCount64() - download->started) / 1000.0);
                        char pct[16]; snprintf(pct, sizeof pct, "%.0f%%", total ? 100.0 * double(done) / double(total) : 100.0);
                        line = "receiving " + download->record.asset.Id() + ": " + Size(done) + " of " + Size(total) + " (" + pct + ", " + Size(uint64_t(double(done) / secs)) + "/s)";
                        if (!waiting.empty()) line += ", " + std::to_string(waiting.size()) + " queued";
                    }
                    else if (!sending.empty() && GetTickCount64() - sendingAt < 3000) line = sending;
                    std::lock_guard<std::mutex> lock(g_mutex); g_progress = line;
                }
                // what the saved maps use: the transitive dependency closure of their names, kept only when the user agrees at session end
                if (summaryWanted && summaryWanted > g_summaryDone)
                {
                    std::set<std::string> keep, missing;
                    for (const auto& saved : savedTexts)
                    {
                        std::set<std::string> ids; std::deque<std::string> todo;
                        for (const auto& record : records) if (saved.tokens.count(record.second.asset.block.name)) todo.push_back(record.first);
                        while (!todo.empty())
                        {
                            auto id = todo.front(); todo.pop_front(); if (!ids.insert(id).second) continue;
                            auto i = records.find(id);
                            if (i == records.end()) { if (!resolver.Exists(id)) missing.insert(id); continue; }
                            for (const auto& d : i->second.asset.dependencies) todo.push_back(d);
                        }
                        for (const auto& id : ids)
                        {
                            if (store->Has(id)) { keep.insert(id); continue; }
                            auto i = records.find(id);
                            if (i != records.end() && !i->second.local && !resolver.Exists(id)) missing.insert(id);   // still on its way
                        }
                    }
                    EndSummary summary; summary.backup = store->Backup().u8string();
                    for (const auto& id : keep) { summary.ids.push_back(id); std::string gdt = store->HiddenIn(id); if (!gdt.empty()) summary.replaces.push_back(id + "  (" + gdt + ")"); }
                    summary.missing.assign(missing.begin(), missing.end());
                    std::lock_guard<std::mutex> lock(g_mutex); g_summary = std::move(summary); g_summaryDone = summaryWanted;
                }
                if (g_stop) break;
                bool connected = net::IsConnected();
                if (connected && !wasConnected) { manifestSent.clear(); convertedSent.clear(); seenCaps.clear(); }   // a new link: tell it everything
                wasConnected = connected;
                if (connected && g_dropped && GetTickCount64() - resyncAsked >= 30000)
                {
                    g_dropped = false; resyncAsked = GetTickCount64(); Note("asset messages were dropped (queue full); asking for the manifests again");
                    net::Writer w; w.str(map); w.u8(uint8_t((zip ? 1 : 0) | 2)); Send(0, net::MSG_ASSET_CAPS, w);
                }
                if (connected && GetTickCount64() - announced > 10000)
                {
                    announced = GetTickCount64();
                    // only new or changed revisions go out, everything again every 15 minutes
                    if (GetTickCount64() - convertedSentReset >= 900000) { manifestSent.clear(); convertedSent.clear(); convertedSentReset = GetTickCount64(); }
                    { net::Writer w; w.str(map); w.u8(zip ? 1 : 0); Send(0, net::MSG_ASSET_CAPS, w); }
                    for (const auto& record : records) if (g_host || (record.second.local && !record.second.mirror)) announce(record.second, 0, false);
                }
                // Refresh once the queue is empty, and every 25 s during a long batch so models show up before the last texture
                if (refresh && !refreshSince) refreshSince = GetTickCount64();
                if (refresh && !refreshing.Running() && ((!download && waiting.empty() && GetTickCount64() >= refreshAt) || GetTickCount64() - refreshSince >= 25000))
                {
                    refresh = false; refreshSince = 0;
                    try { if (!refreshing.Start(root)) Event("no gdtdb.exe; " + std::to_string(store->Count()) + " temporary assets"); }
                    catch (const std::exception& e) { Event(e.what()); }
                }
                if (refreshing.Running())
                {
                    int r = refreshing.Poll();
                    if (r == 1) Event("asset database updated; " + std::to_string(store->Count()) + " temporary assets");
                    else if (r == -1) Event("gdtdb /update failed");
                }
            }
            { std::lock_guard<std::mutex> lock(g_mutex); g_progress.clear(); }
            if (refreshing.Running() && refreshing.Wait(30000) == 0) Event("gdtdb /update still running at session end; waiting no longer");
            std::set<std::string> keep; { std::lock_guard<std::mutex> lock(g_mutex); keep.swap(g_keep); }
            if (!keep.empty())
            {
                try { store->Save(keep); refresh = true; Event("saved " + std::to_string(keep.size()) + " streamed asset(s) to " + SavedGdt); }
                catch (const std::exception& e) { Event(std::string("could not save the streamed assets (") + e.what() + "); nothing was changed, they stay in __streamed_assets until the next session start removes them"); throw; }
            }
            bool cleanup = store->Count() != 0; std::string finished = store->Finish();
            if (!finished.empty()) Event(finished);
            if (refresh || cleanup) RefreshDatabase(root);
            Event("asset session ended; unsaved streamed assets removed");
        }
        catch (const std::exception& e) { Event(std::string("asset streaming stopped: ") + e.what() + "; backups: " + backup.u8string()); }
        catch (...) { Event("asset streaming stopped after an unexpected error; recovery files retained"); }
        g_running = false;
    }
    std::wstring Wide(const std::string& text)
    {
        std::wstring w(text.size() + 1, L'\0'); int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &w[0], int(w.size()));
        w.resize(n > 0 ? size_t(n - 1) : 0); return w;
    }
    // "40 images, 35 materials, 23 models" from ids like "image:name"
    std::string CountByType(const std::vector<std::string>& ids)
    {
        std::map<std::string, size_t> n; for (const auto& id : ids) n[id.substr(0, id.find(':'))]++;
        std::string out;
        for (const auto& t : n)
        {
            std::string name = t.first == "xmodel" ? "model" : t.first;
            out += (out.empty() ? "" : ", ") + std::to_string(t.second) + " " + name + (t.second == 1 ? "" : "s");
        }
        return out;
    }
    // Every list in full, for the dialog's link. Empty when it cannot be written
    fs::path WriteEndList(const EndSummary& s, const std::string& reason, const std::string& saveTo)
    {
        try
        {
            fs::path file = fs::path(s.backup).parent_path().parent_path() / L"session_end_assets.txt";
            std::string t = "Team Create session over (" + reason + ")\r\n\r\nStreamed assets the saved map uses (" + std::to_string(s.ids.size()) + "), saved to " + saveTo + " on Yes:\r\n";
            for (const auto& x : s.ids) t += "  " + x + "\r\n";
            if (!s.replaces.empty())
            {
                t += "\r\nSame name as an asset you already have (" + std::to_string(s.replaces.size()) + "): saving takes yours out of its GDT, a copy is in " + s.backup + "\r\n";
                for (const auto& x : s.replaces) t += "  " + x + "\r\n";
            }
            if (!s.missing.empty())
            {
                t += "\r\nUsed by the map but never arrived, cannot be saved (" + std::to_string(s.missing.size()) + "):\r\n";
                for (const auto& x : s.missing) t += "  " + x + "\r\n";
            }
            fs::create_directories(file.parent_path()); Write(file, t); return file;
        }
        catch (...) { return {}; }
    }
    HRESULT CALLBACK EndDialogProc(HWND, UINT note, WPARAM, LPARAM lParam, LONG_PTR data)
    {
        if (note == TDN_HYPERLINK_CLICKED && data) ShellExecuteW(nullptr, L"open", reinterpret_cast<const wchar_t*>(data), nullptr, nullptr, SW_SHOWNORMAL);
        (void)lParam; return S_OK;
    }
    // IDYES keep, IDNO discard. A task dialog under this DLL's Common Controls 6 context (see pane.cpp ShowNotice), a message box without it
    int AskKeep(const EndSummary& s, const std::string& reason, const std::string& saveTo)
    {
        fs::path list = WriteEndList(s, reason, saveTo); std::wstring listPath = list.wstring();
        std::string why = reason; if (!why.empty()) why[0] = char(tolower(static_cast<unsigned char>(why[0])));
        std::string body = "The session is over (" + why + ").\n\nThe map you saved uses " + std::to_string(s.ids.size()) + " streamed asset(s): " + CountByType(s.ids) + ".";
        if (!s.replaces.empty()) body += "\n\n" + std::to_string(s.replaces.size()) + " have the same name as an asset you already have. Saving replaces yours (a backup is kept).";
        if (!s.missing.empty()) body += "\n\n" + std::to_string(s.missing.size()) + " never arrived and cannot be saved.";
        std::wstring content = Wide(body) + (listPath.empty() ? L"" : L"\n\n<a href=\"list\">Show every asset</a>");
        std::wstring instruction = L"Keep the " + std::to_wstring(s.ids.size()) + L" streamed asset(s) your map uses?";
        std::wstring keep = L"Save them\nInto " + Wide(saveTo) + L", with their own GDT";
        std::wstring drop = L"Discard them\nThe saved map then refers to assets this PC does not have";
        HWND owner = mt::MainWindow(); int answer = 0; HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&AskKeep), &self);
        ACTCTXW ctx = { sizeof ctx }; ctx.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID; ctx.hModule = self; ctx.lpResourceName = MAKEINTRESOURCEW(2);
        HANDLE act = self ? CreateActCtxW(&ctx) : INVALID_HANDLE_VALUE; ULONG_PTR cookie = 0;
        const bool activated = act != INVALID_HANDLE_VALUE && ActivateActCtx(act, &cookie);   // an exe with its own manifest needs none
        {
            TASKDIALOG_BUTTON buttons[] = { { IDYES, keep.c_str() }, { IDNO, drop.c_str() } };
            TASKDIALOGCONFIG td = { sizeof td };
            td.hwndParent = owner; td.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ENABLE_HYPERLINKS | TDF_POSITION_RELATIVE_TO_WINDOW;
            td.pszWindowTitle = L"Team Create"; td.pszMainIcon = s.replaces.empty() ? TD_INFORMATION_ICON : TD_WARNING_ICON;
            td.pszMainInstruction = instruction.c_str(); td.pszContent = content.c_str();
            td.pButtons = buttons; td.cButtons = 2; td.nDefaultButton = IDYES;
            td.pfCallback = EndDialogProc; td.lpCallbackData = reinterpret_cast<LONG_PTR>(listPath.empty() ? nullptr : listPath.c_str());
            if (FAILED(TaskDialogIndirect(&td, &answer, nullptr, nullptr))) answer = 0;
            if (activated) DeactivateActCtx(0, cookie);
        }
        if (act != INVALID_HANDLE_VALUE) ReleaseActCtx(act);
        if (answer == IDYES || answer == IDNO) return answer;
        std::string text = body + "\n\nYes: save them into " + saveTo + ".\nNo: discard them; the saved map then refers to assets this PC does not have.";
        if (!listPath.empty()) text += "\n\nEvery asset: " + list.u8string();
        return MessageBoxW(owner, Wide(text).c_str(), (L"Team Create: " + instruction).c_str(), MB_YESNO | (s.replaces.empty() ? MB_ICONQUESTION : MB_ICONWARNING) | MB_SETFOREGROUND);
    }
}
namespace assets
{
    void Start(bool host)
    {
        Stop(); if (!rp::host->RadiantRoot) return;
        fs::path root(rp::host->RadiantRoot(rp::self)); if (root.empty()) return;
        wchar_t local[32768] = {}; DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
        if (!n || n >= 32768) { Event("asset streaming unavailable: LOCALAPPDATA missing"); return; }
        SYSTEMTIME t; GetLocalTime(&t); wchar_t stamp[96];
        swprintf_s(stamp, L"%04u%02u%02u-%02u%02u%02u-%u-%llu", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId(), GetTickCount64());
        fs::path backup = fs::path(local) / L"RadiantPlugins/TeamCreate/asset_backups" / stamp;
        fs::path cache = fs::path(local) / L"RadiantPlugins/TeamCreate/asset_cache";
        wchar_t override[32768] = {}; DWORD m = GetEnvironmentVariableW(L"RTC_ASSET_CACHE", override, 32768);
        if (m < 32768 && (m || GetLastError() != ERROR_ENVVAR_NOT_FOUND)) cache = m ? fs::path(override) : fs::path();   // set empty: no cache
        { std::lock_guard<std::mutex> lock(g_mutex); g_messages.clear(); g_saves.clear(); g_queueBytes = 0; g_map.clear(); g_text.clear(); g_keep.clear(); g_summaryWanted = g_summaryDone = 0; }
        g_host = host; g_stop = false; g_running = true; g_lastSnapshot = 0; g_streamed = 0;
        try
        {
            g_worker.thread = std::thread([root, backup, cache]() noexcept
            {
                try { Run(root, backup, cache); }
                catch (...) { g_running = false; OutputDebugStringA("TeamCreate asset worker failed while reporting an error; recovery files retained\n"); }
            });
        }
        catch (...) { g_running = false; Event("could not start asset worker"); }
    }
    void Stop()
    {
        g_stop = true; g_wake.notify_all(); if (g_worker.thread.joinable()) g_worker.thread.join(); g_running = false;
        std::lock_guard<std::mutex> lock(g_mutex); g_progress.clear();
    }
    void EndSession(const char* why)
    {
        if (!g_running || g_ending) return;
        g_ending = true;
        uint64_t want;
        { std::lock_guard<std::mutex> lock(g_mutex); want = ++g_summaryWanted; }
        g_wake.notify_all();
        // the worker answers within one pass (100 ms) unless it is writing a big file
        bool answered = false; uint64_t start = GetTickCount64();
        while (g_running && GetTickCount64() - start < 15000) { { std::lock_guard<std::mutex> lock(g_mutex); answered = g_summaryDone >= want; } if (answered) break; Sleep(20); }
        EndSummary s; { std::lock_guard<std::mutex> lock(g_mutex); s = g_summary; }
        if (!answered) Note("no answer from the asset worker within 15 s; streamed assets are not saved");
        else if (s.ids.empty()) Note(std::string("session over (") + why + "): no saved map uses a streamed asset");
        else
        {
            std::string root = rp::host->RadiantRoot ? fs::path(rp::host->RadiantRoot(rp::self)).u8string() : std::string();
            std::string reason = why; while (!reason.empty() && reason.back() == '.') reason.pop_back();
            int r = g_endAnswer >= 0 ? (g_endAnswer ? IDYES : IDNO) : AskKeep(s, reason, root + "\\" + SavedFolder + "\\");
            if (r == IDYES) { std::lock_guard<std::mutex> lock(g_mutex); g_keep.insert(s.ids.begin(), s.ids.end()); }
            Note(std::string("session over (") + why + "): " + std::to_string(s.ids.size()) + " streamed asset(s) used by the saved map " + (r == IDYES ? "kept" : "discarded by the user"));
        }
        Stop();
        g_ending = false;
    }
    void Tick()
    {
        if (!g_running || g_ending) return;
        if (!presence::IsActive() && !presence::OnTempMap()) { EndSession(presence::EndReason().empty() ? "stopped" : presence::EndReason().c_str()); return; }
        if (GetTickCount64() - g_lastSnapshot < 2000 || mapsync::Blocked()) return;
        g_lastSnapshot = GetTickCount64(); std::string text = mapsync::AssetText(), map = mapsync::RootKey();
        { std::lock_guard<std::mutex> lock(g_mutex); g_text = std::move(text); g_map = std::move(map); }
    }
    void OnMessage(const net::Message& message)
    {
        if (!g_running || g_stop || message.payload.size() > 2 * 1024 * 1024) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_queueBytes + message.payload.size() > InboxBytes || g_messages.size() >= InboxMessages) { g_dropped = true; return; }
        g_queueBytes += message.payload.size(); g_messages.push_back(message); g_wake.notify_all();
    }
    void OnSaved(const std::wstring& path)
    {
        if (!g_running || path.empty()) return;
        try
        {
            Saved save{path, Read(fs::path(path), 64 * 1024 * 1024)};
            { std::lock_guard<std::mutex> lock(g_mutex); if (g_saves.size() < 32) g_saves.push_back(std::move(save)); }
            g_wake.notify_all();
        }
        catch (const std::exception& e) { Event(std::string("could not capture saved map's assets: ") + e.what()); }
    }
    std::string Status() { std::lock_guard<std::mutex> lock(g_mutex); return g_progress.empty() ? g_status : g_progress; }
    void SetSendConverted(bool on) { g_sendConverted = on; }
    void SetBackupRetention(unsigned days, unsigned keep) { g_backupDays = days; g_backupKeep = keep; }
    void SetEndAnswer(int answer) { g_endAnswer = answer; }
    bool HasStreamed() { return g_running && g_streamed > 0; }
    std::string HudLine()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_progress.empty()) return g_progress;
        return g_running && GetTickCount64() - g_statusTick < 10000 ? g_status : std::string();
    }
}
