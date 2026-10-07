// Zone sync: the host's built usermap for Play Test's Join Host (zonesync.h). The client fetches usermaps\<map>\zone in 1 MB blocks,
// only the blocks it cannot find among its own files, compressed, over the direct link (direct.cpp) when the host offers one. Message layouts are in net.h.
// Threads: the host hashes on its own worker, the client's worker plans, fills <file>.tcpart (kept when a fetch stops, the next one carries on) and renames.
#include <windows.h>
#include <bcrypt.h>
#include <compressapi.h>
#include <tlhelp32.h>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <deque>
#include <array>
#include <algorithm>
#include <chrono>
#include <vector>
#include <map>
#include <unordered_map>
#include <string>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include "zonesync.h"
#include "direct.h"
#include "compat.h"
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "cabinet.lib")

namespace
{
    namespace fs = std::filesystem;
    constexpr uint32_t kBlock = 1u << 20;
    constexpr size_t kWindow = 4, kDirectWindow = 8;        // requests in flight on the session, on the direct link
    constexpr ULONGLONG kStallMs = 45000, kDirectStallMs = 15000, kManifestWaitMs = 90000, kRebuildWaitMs = 600000;
    constexpr size_t kHash = 16;
    using Hash = std::array<uint8_t, kHash>;

    struct Entry { std::string rel; uint64_t size = 0, time = 0; std::vector<Hash> blocks; };
    enum State { IDLE, ASKING, FETCHING, DONE, FAILED };
    const char* StateName(State s) { switch (s) { case ASKING: return "ASKING"; case FETCHING: return "FETCHING"; case DONE: return "DONE"; case FAILED: return "FAILED"; default: return "IDLE"; } }

    // ---- helpers ----
    std::wstring Widen(const std::string& s)
    {
        if (s.empty()) return L"";
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
        std::wstring w(size_t(n), L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
        return w;
    }
    std::string Narrow(const std::wstring& w)
    {
        if (w.empty()) return "";
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(size_t(n), '\0'); WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), &s[0], n, nullptr, nullptr);
        return s;
    }
    std::string Lower(std::string s) { for (auto& c : s) c = char(tolower(uint8_t(c))); return s; }
    bool ValidMap(const std::string& m)
    {
        if (m.empty() || m.size() > 63) return false;
        for (char c : m) if (!(isalnum(uint8_t(c)) || c == '_' || c == '-')) return false;
        return true;
    }
    bool ValidRel(const std::string& r)
    {
        if (r.empty() || r.size() > 200 || r[0] == '/' || r[0] == '\\' || r.find("..") != std::string::npos || r.find(':') != std::string::npos) return false;
        for (char c : r) if (uint8_t(c) < 0x20) return false;
        return true;
    }
    std::wstring ZoneDir(const std::string& map)
    {
        std::wstring root;
        if (rp::host->RadiantRoot) { const wchar_t* r = rp::host->RadiantRoot(rp::self); if (r) root = r; }
        while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
        return root.empty() ? L"" : root + L"\\usermaps\\" + Widen(map) + L"\\zone";
    }
    std::wstring PathOf(const std::wstring& dir, const std::string& rel)
    {
        std::wstring p = dir + L"\\" + Widen(rel);
        for (auto& c : p) if (c == L'/') c = L'\\';
        return p;
    }
    // Files a joining game needs: everything under the zone folder except Play Test's live material and nav zones
    // (<map>_lm*, <map>_nav*: loaded by the host's bridge into its running level), partial downloads and the cache file.
    bool Wanted(const std::string& rel, const std::string& map)
    {
        std::string lower = Lower(rel), m = Lower(map);
        if (lower.size() > 7 && lower.compare(lower.size() - 7, 7, ".tcpart") == 0) return false;
        if (lower == "teamcreate_host.txt") return false;
        if (lower.find(m + "_lm") != std::string::npos || lower.find(m + "_nav") != std::string::npos) return false;   // the host's live zones (Play Test removes them at every launch)
        return true;
    }
    bool FileInfo(const std::wstring& path, uint64_t& size, uint64_t& time)
    {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
        size = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
        time = (uint64_t(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
        return true;
    }
    void U64(net::Writer& w, uint64_t v) { w.u32(uint32_t(v)); w.u32(uint32_t(v >> 32)); }
    uint64_t U64(net::Reader& r) { uint64_t lo = r.u32(); uint64_t hi = r.u32(); return lo | (hi << 32); }
    std::string MapBaseOf(const std::string& key)
    {
        std::string k = key;
        size_t s = k.find_last_of("/\\"); if (s != std::string::npos) k = k.substr(s + 1);
        size_t d = k.rfind('.'); if (d != std::string::npos) k = k.substr(0, d);
        return k;
    }
    bool ProcessRunning(const wchar_t* exe)
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return false;
        PROCESSENTRY32W pe{ sizeof pe };
        bool found = false;
        for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) found = _wcsicmp(pe.szExeFile, exe) == 0;
        CloseHandle(snap);
        return found;
    }

    // SHA-256 (first 16 bytes) of a buffer, one algorithm handle for the process
    struct Hasher
    {
        BCRYPT_ALG_HANDLE alg = nullptr;
        Hasher() { BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0); }
        ~Hasher() { if (alg) BCryptCloseAlgorithmProvider(alg, 0); }
        bool Of(const uint8_t* p, size_t n, Hash& out) const
        {
            uint8_t full[32];
            if (!alg || BCryptHash(alg, nullptr, 0, const_cast<PUCHAR>(p), ULONG(n), full, sizeof full) != 0) return false;
            memcpy(out.data(), full, kHash);
            return true;
        }
    };
    Hasher& Sha() { static Hasher h; return h; }

    // Block hashes of a file, cached by path, size and write time (both sides hash the same files again and again).
    std::mutex g_hashMutex;
    struct HashCacheItem { uint64_t size = 0, time = 0; std::vector<Hash> blocks; };
    std::unordered_map<std::wstring, HashCacheItem> g_hashCache;
    bool HashFile(const std::wstring& path, uint64_t& size, uint64_t& time, std::vector<Hash>& out, const std::atomic<bool>* cancel)
    {
        if (!FileInfo(path, size, time)) return false;
        {
            std::lock_guard<std::mutex> lock(g_hashMutex);
            auto it = g_hashCache.find(path);
            if (it != g_hashCache.end() && it->second.size == size && it->second.time == time) { out = it->second.blocks; return true; }
        }
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        std::vector<uint8_t> buf(kBlock);
        out.clear();
        uint64_t done = 0;
        bool ok = true;
        while (done < size)
        {
            if (cancel && *cancel) { ok = false; break; }
            DWORD want = DWORD(std::min<uint64_t>(kBlock, size - done)), got = 0;
            if (!ReadFile(h, buf.data(), want, &got, nullptr) || got != want) { ok = false; break; }
            Hash hh{}; if (!Sha().Of(buf.data(), got, hh)) { ok = false; break; }
            out.push_back(hh); done += got;
        }
        CloseHandle(h);
        if (!ok) return false;
        uint64_t s2 = 0, t2 = 0;
        if (!FileInfo(path, s2, t2) || s2 != size || t2 != time) return false;   // written meanwhile
        std::lock_guard<std::mutex> lock(g_hashMutex);
        g_hashCache[path] = { size, time, out };
        return true;
    }
    void RememberHashes(const std::wstring& path, const std::vector<Hash>& blocks)
    {
        uint64_t s = 0, t = 0;
        if (!FileInfo(path, s, t)) return;
        std::lock_guard<std::mutex> lock(g_hashMutex);
        g_hashCache[path] = { s, t, blocks };
    }

    // The files of a build (host side), without hashes.
    std::vector<Entry> ListBuild(const std::string& map, std::string& error)
    {
        std::vector<Entry> files;
        std::wstring dir = ValidMap(map) ? ZoneDir(map) : L"";
        uint64_t s = 0, t = 0;
        if (dir.empty()) { error = "bad map name"; return files; }
        if (!FileInfo(PathOf(dir, map + ".ff"), s, t)) { error = "the host has no build of " + map + " (usermaps\\" + map + "\\zone\\" + map + ".ff): press Play on the host first"; return files; }
        std::error_code ec;
        for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        {
            if (!it->is_regular_file(ec)) continue;
            std::string rel = Narrow(fs::relative(it->path(), dir, ec).wstring());
            for (auto& c : rel) if (c == '\\') c = '/';
            if (ec || !ValidRel(rel) || !Wanted(rel, map)) continue;
            Entry e; e.rel = rel;
            if (FileInfo(it->path().wstring(), e.size, e.time)) files.push_back(e);
            if (files.size() >= 4000) break;
        }
        return files;
    }

    // ---- host ----
    void ServeManifestV1(uint32_t peer, const std::string& map)   // request without hashes (first protocol version)
    {
        std::string error;
        std::vector<Entry> files = ListBuild(map, error);
        net::Writer w; w.str(map); w.u8(error.empty() ? 1 : 0); w.str(error);
        w.u16(uint16_t(files.size()));
        for (const auto& e : files) { w.str(e.rel); U64(w, e.size); U64(w, e.time); }
        net::SendTo(peer, net::MSG_ZONE_MANIFEST, w.buf);
    }

    // XPRESS Huffman through cabinet.dll, a handle per call (handles are not shared between threads).
    bool Pack(const std::string& in, std::string& out)
    {
        COMPRESSOR_HANDLE c = nullptr;
        if (in.empty() || !CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &c)) return false;
        out.resize(in.size());
        SIZE_T n = 0;
        const bool ok = Compress(c, in.data(), in.size(), &out[0], out.size(), &n) != 0;
        CloseCompressor(c);
        if (ok) out.resize(n);
        return ok;
    }
    bool Unpack(const std::string& in, size_t rawSize, std::string& out)
    {
        DECOMPRESSOR_HANDLE d = nullptr;
        if (in.empty() || !rawSize || !CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &d)) return false;
        out.resize(rawSize);
        SIZE_T n = 0;
        const bool ok = Decompress(d, in.data(), in.size(), &out[0], rawSize, &n) != 0 && n == rawSize;
        CloseDecompressor(d);
        return ok;
    }

    // One block for a MSG_ZONE_GET as a MSG_ZONE_CHUNK payload, the session and the direct link both serve it. False = malformed request.
    bool BuildChunk(net::Reader& r, const std::string& map, net::Writer& w)
    {
        std::string rel = r.str(); uint64_t time = U64(r); uint64_t off = U64(r); uint32_t len = r.u32();
        if (!r.ok) return false;
        const uint8_t flags = r.u8();
        const bool packs = r.ok && (flags & 1);
        w.str(map); w.str(rel);
        std::string data;
        uint8_t status = 1;
        if (ValidMap(map) && ValidRel(rel) && Wanted(rel, map) && len <= kBlock)
        {
            std::wstring path = PathOf(ZoneDir(map), rel);
            uint64_t size = 0, t = 0;
            if (FileInfo(path, size, t) && t == time && off <= size)
            {
                // the host's running game keeps its fastfiles open: share everything
                HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
                if (h != INVALID_HANDLE_VALUE)
                {
                    LARGE_INTEGER li; li.QuadPart = LONGLONG(off);
                    DWORD want = DWORD(std::min<uint64_t>(len, size - off)), got = 0;
                    data.resize(want);
                    if (SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && (want == 0 || ReadFile(h, &data[0], want, &got, nullptr)) && got == want) status = 0;
                    CloseHandle(h);
                }
            }
        }
        if (status) data.clear();
        else if (packs)
        {
            std::string z;
            if (Pack(data, z) && z.size() + data.size() / 32 < data.size()) { data.swap(z); status = 2; }
        }
        w.u8(status); U64(w, off); w.blob(data);
        return true;
    }
    void ServeChunk(uint32_t peer, net::Reader& r, const std::string& map)
    {
        net::Writer w;
        if (BuildChunk(r, map, w)) net::SendTo(peer, net::MSG_ZONE_CHUNK, w.buf);
    }
    bool ServeDirect(uint16_t type, const std::vector<uint8_t>& payload, uint16_t& replyType, std::vector<uint8_t>& reply)
    {
        if (type != net::MSG_ZONE_GET) return false;
        net::Reader r(payload);
        const std::string map = r.str();
        net::Writer w;
        if (!r.ok || !BuildChunk(r, map, w)) return false;
        replyType = net::MSG_ZONE_CHUNK; reply = std::move(w.buf);
        return true;
    }
    void SendOffer(uint32_t peer, const direct::Offer& o)
    {
        net::Writer w; w.str(o.ip); w.u16(o.port); w.blob(o.token); w.u8(o.mode);
        net::SendTo(peer, net::MSG_DIRECT_OFFER, w.buf);
    }

    // Host worker: manifests with block hashes, one request at a time.
    std::mutex g_hostMutex;
    std::condition_variable g_hostCv;
    std::deque<std::pair<uint32_t, std::string>> g_hostJobs;
    std::thread g_hostThread;
    std::atomic<bool> g_quit{ false };

    void ServeManifest2(uint32_t peer, const std::string& map)
    {
        std::string error;
        // a build is not complete while the linker runs: the client asks again
        const bool busy = peer && ProcessRunning(L"linker_modtools.exe");
        if (busy) error = "the host is linking the map right now";
        std::vector<Entry> files;
        if (!busy) files = ListBuild(map, error);
        uint64_t total = 0;
        if (error.empty())
            for (auto& e : files)
            {
                uint64_t s = 0, t = 0;
                if (!HashFile(PathOf(ZoneDir(map), e.rel), s, t, e.blocks, &g_quit)) { error = "the host could not read " + e.rel + " (is it being linked right now?)"; break; }
                e.size = s; e.time = t; total += s;
            }
        if (!error.empty()) files.clear();
        if (!peer) { if (error.empty()) LOG("zonesync: build of %s hashed for the peers (%.1f MB)", map.c_str(), total / 1048576.0); return; }   // a warm-up job
        net::Writer w; w.str(map); w.u8(error.empty() ? 1 : 0); w.str(error); w.u32(kBlock);
        w.u16(uint16_t(files.size()));
        for (const auto& e : files)
        {
            w.str(e.rel); U64(w, e.size); U64(w, e.time);
            std::string hashes(e.blocks.size() * kHash, '\0');
            for (size_t i = 0; i < e.blocks.size(); ++i) memcpy(&hashes[i * kHash], e.blocks[i].data(), kHash);
            w.blob(hashes);
        }
        w.u8(busy ? 1 : 0);
        net::SendTo(peer, net::MSG_ZONE_MANIFEST2, w.buf);
        if (error.empty()) LOG("zonesync: peer %u asked for %s: %zu file(s), %.1f MB", peer, map.c_str(), files.size(), total / 1048576.0);
        else LOG("zonesync: peer %u asked for %s: %s", peer, map.c_str(), error.c_str());
    }
    void HostWorker()
    {
        for (;;)
        {
            std::pair<uint32_t, std::string> job;
            {
                std::unique_lock<std::mutex> lock(g_hostMutex);
                g_hostCv.wait(lock, [] { return g_quit || !g_hostJobs.empty(); });
                if (g_quit) return;
                job = g_hostJobs.front(); g_hostJobs.pop_front();
            }
            try { ServeManifest2(job.first, job.second); } catch (...) { LOG("zonesync: the manifest for %s failed (exception)", job.second.c_str()); }
        }
    }
    void QueueManifest(uint32_t peer, const std::string& map)
    {
        std::lock_guard<std::mutex> lock(g_hostMutex);
        for (const auto& j : g_hostJobs) if (j.first == peer && j.second == map) return;
        g_hostJobs.push_back({ peer, map });
        if (!g_hostThread.joinable()) g_hostThread = std::thread(HostWorker);
        g_hostCv.notify_all();
    }

    // Host build watcher (main thread): a stable new build of our map is announced to the clients.
    std::string g_watchMap, g_watchSig, g_pendingSig, g_announcedSig;
    ULONGLONG g_watchAt = 0, g_pendingSince = 0;
    std::string BuildSignature(const std::string& map)
    {
        std::string error;
        std::vector<Entry> files = ListBuild(map, error);
        if (!error.empty()) return "";
        std::string sig;
        for (const auto& e : files) sig += e.rel + ":" + std::to_string(e.size) + ":" + std::to_string(e.time) + ";";
        return sig;
    }

    // ---- client ----
    std::mutex g_m;
    std::condition_variable g_cv;
    State g_state = IDLE;
    bool g_background = false;
    std::string g_map, g_msg;
    std::thread g_worker;
    std::atomic<bool> g_cancel{ false };
    bool g_haveManifest = false, g_manifestBusy = false;
    uint8_t g_manifestOk = 0;
    std::string g_manifestError;
    uint32_t g_manifestBlock = 0;
    std::vector<Entry> g_manifest;
    struct Piece { std::string rel; uint8_t status = 0; uint64_t off = 0; std::string data; };
    std::deque<Piece> g_pieces;
    ULONGLONG g_lastActivity = 0, g_startedAt = 0, g_rateAt = 0;
    uint64_t g_fetchTotal = 0, g_fetched = 0, g_reused = 0, g_wire = 0, g_directBytes = 0, g_rateBase = 0;
    double g_rate = 0;                     // MB per second, last two seconds
    size_t g_fileNo = 0, g_fileCount = 0;
    // The direct link of the running fetch, all under g_m. The worker opens and closes it, g_dreader feeds g_pieces.
    direct::Offer g_offer;
    bool g_haveOffer = false, g_offerTried = false, g_dDead = false, g_dialing = false;
    direct::Sock g_ds = direct::kNoSock;
    std::thread g_dreader, g_dialer;       // g_dialer makes the link beside the running transfer and starts g_dreader
    std::atomic<bool> g_dialCancel{ false };

    // Radiant exits without OnUnload: joinable threads at static destruction would terminate the process ( Traps).
    struct ExitThreadGuard
    {
        ~ExitThreadGuard()
        {
            if (g_dialer.joinable()) g_dialer.detach();
            if (g_dreader.joinable()) g_dreader.detach();
            if (g_worker.joinable()) g_worker.detach();
            if (g_hostThread.joinable()) g_hostThread.detach();
        }
    } g_exitThreadGuard;

    void Finish(State st, const std::string& msg)
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_state = st; g_msg = msg;
        LOG("zonesync: %s %s: %s", g_map.c_str(), st == DONE ? "done" : "FAILED", msg.c_str());
    }
    std::string HashKey(const Hash& h) { return std::string(reinterpret_cast<const char*>(h.data()), kHash); }

    struct Source { std::wstring path; uint64_t off = 0; };
    struct Plan
    {
        Entry e;
        std::wstring final, part;
        std::vector<uint32_t> fetch;                          // block indices to pull from the host
        std::vector<std::pair<uint32_t, Source>> copy;        // block index, where this PC has it
        bool resume = false;                                  // the part file of a stopped fetch holds finished blocks
    };
    enum class Got { Ok, Failed, Rebuilt };

    bool ReadAt(HANDLE h, uint64_t off, void* dst, DWORD n)
    {
        LARGE_INTEGER li; li.QuadPart = LONGLONG(off); DWORD got = 0;
        return SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && (n == 0 || (ReadFile(h, dst, n, &got, nullptr) && got == n));
    }
    bool WriteAt(HANDLE h, uint64_t off, const void* src, DWORD n)
    {
        LARGE_INTEGER li; li.QuadPart = LONGLONG(off); DWORD wrote = 0;
        return SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && (n == 0 || (WriteFile(h, src, n, &wrote, nullptr) && wrote == n));
    }
    DWORD BlockLen(const Entry& e, uint32_t i) { return DWORD(std::min<uint64_t>(kBlock, e.size - uint64_t(i) * kBlock)); }

    void OnChunk(net::Reader& r, const std::string& map)
    {
        Piece p; p.rel = r.str(); p.status = r.u8(); p.off = U64(r); p.data = r.blob();
        if (!r.ok) return;
        std::lock_guard<std::mutex> lock(g_m);
        if (g_state != FETCHING || map != g_map) return;
        g_lastActivity = GetTickCount64();
        g_pieces.push_back(std::move(p));
        g_cv.notify_all();
    }

    // ---- client: direct link ----
    void DirectReader(direct::Sock s)
    {
        try
        {
            uint16_t type = 0; std::vector<uint8_t> payload;
            while (direct::ReadFrame(s, type, payload, kBlock + 4096))
            {
                if (type != net::MSG_ZONE_CHUNK) continue;
                net::Reader r(payload);
                const std::string map = r.str();
                if (r.ok) OnChunk(r, map);
            }
        }
        catch (...) {}
        std::lock_guard<std::mutex> lock(g_m);
        g_dDead = true; g_cv.notify_all();
    }
    void DialMain(direct::Offer o)
    {
        direct::Sock s = direct::kNoSock;
        std::string why;
        const ULONGLONG t0 = GetTickCount64();
        try
        {
            s = direct::Dial(o, 3000, g_dialCancel, [](const std::string& ip, uint16_t port) { net::Writer w; w.str(ip); w.u16(port); net::Send(net::MSG_DIRECT_PUNCH, w.buf); }, why);
        }
        catch (...) { s = direct::kNoSock; }
        const unsigned long long ms = (unsigned long long)(GetTickCount64() - t0);
        std::lock_guard<std::mutex> lock(g_m);
        g_dialing = false;
        if (s == direct::kNoSock) { if (!g_dialCancel) LOG("zonesync: no direct link to the host (%s, %llu ms), the session link carries the build", why.c_str(), ms); return; }
        if (g_dialCancel) { direct::Close(s); return; }
        g_ds = s; g_dDead = false; g_lastActivity = GetTickCount64();
        g_dreader = std::thread(DirectReader, s);
        LOG("zonesync: direct link to the host up (%s, %llu ms)", o.mode ? "hole punched" : "open port", ms);
        g_cv.notify_all();
    }
    void DirectClose()   // worker
    {
        g_dialCancel = true;
        if (g_dialer.joinable()) g_dialer.join();
        direct::Sock s;
        { std::lock_guard<std::mutex> lock(g_m); s = g_ds; }
        direct::Close(s);
        if (g_dreader.joinable()) g_dreader.join();
        std::lock_guard<std::mutex> lock(g_m);
        g_ds = direct::kNoSock; g_dDead = false;
    }
    // Starts the dial of an offer not tried yet, the transfer runs on meanwhile. Skipped when the session already runs on the LAN or on that address.
    void DirectOpen()    // worker
    {
        direct::Offer o;
        {
            std::lock_guard<std::mutex> lock(g_m);
            if (!g_haveOffer || g_offerTried || g_dialing || g_ds != direct::kNoSock) return;
            g_offerTried = true; o = g_offer;
        }
        if (!rp::ConfigGetBool("direct", "enabled", true)) return;
        const std::string host = net::HostAddress();
        if (direct::IsRoutable(o.ip) && (direct::IsLan(host) || host == o.ip)) return;
        if (g_dialer.joinable()) g_dialer.join();
        g_dialCancel = false;
        { std::lock_guard<std::mutex> lock(g_m); g_dialing = true; }
        g_dialer = std::thread(DialMain, o);
    }
    // Asks for one block, over the direct link while it lives. A dead link sends nothing: the caller asks again over the session.
    void Request(const std::string& map, const Plan& p, uint32_t i)
    {
        net::Writer w; w.str(map); w.str(p.e.rel); U64(w, p.e.time); U64(w, uint64_t(i) * kBlock); w.u32(BlockLen(p.e, i)); w.u8(1);
        direct::Sock s; bool dead;
        { std::lock_guard<std::mutex> lock(g_m); s = g_ds; dead = g_dDead; }
        if (s == direct::kNoSock) net::Send(net::MSG_ZONE_GET, w.buf);
        else if (!dead && !direct::SendFrame(s, net::MSG_ZONE_GET, w.buf)) { std::lock_guard<std::mutex> lock(g_m); g_dDead = true; g_cv.notify_all(); }
    }

    // One file into its part file: blocks a stopped fetch left there stay, then the blocks found on this PC (checked again on read), then the host's.
    // The part file is kept on every failure, the next fetch carries on from it.
    Got Assemble(const std::string& map, Plan& p)
    {
        std::error_code ec; fs::create_directories(fs::path(p.final).parent_path(), ec);
        HANDLE out = CreateFileW(p.part.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, p.resume ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE) { Finish(FAILED, "cannot write " + Narrow(p.part) + " (error " + std::to_string(GetLastError()) + ")"); return Got::Failed; }
        std::vector<uint8_t> buf(kBlock);
        std::map<std::wstring, HANDLE> sources;
        for (const auto& c : p.copy)
        {
            if (g_cancel) break;
            HANDLE& src = sources[c.second.path];
            if (!src) src = CreateFileW(c.second.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            const DWORD n = BlockLen(p.e, c.first);
            Hash h{};
            if (src != INVALID_HANDLE_VALUE && ReadAt(src, c.second.off, buf.data(), n) && Sha().Of(buf.data(), n, h) && h == p.e.blocks[c.first] &&
                WriteAt(out, uint64_t(c.first) * kBlock, buf.data(), n))
            { std::lock_guard<std::mutex> lock(g_m); g_reused += n; }
            else { p.fetch.push_back(c.first); std::lock_guard<std::mutex> lock(g_m); g_fetchTotal += n; }   // changed meanwhile: from the host after all
        }
        for (auto& s : sources) if (s.second && s.second != INVALID_HANDLE_VALUE) CloseHandle(s.second);
        std::sort(p.fetch.begin(), p.fetch.end());

        // the host's blocks: a window of requests in flight, answers matched by offset
        size_t next = 0, done = 0;
        int damaged = 0, stalls = 0;
        std::deque<uint32_t> inflight;
        std::string plain;
        Got res = g_cancel ? Got::Failed : Got::Ok;
        while (res == Got::Ok && done < p.fetch.size())
        {
            DirectOpen();
            bool onDirect;
            { std::lock_guard<std::mutex> lock(g_m); onDirect = g_ds != direct::kNoSock && !g_dDead; }
            const size_t window = onDirect ? kDirectWindow : kWindow;
            while (inflight.size() < window && next < p.fetch.size()) { const uint32_t i = p.fetch[next++]; inflight.push_back(i); Request(map, p, i); }
            Piece piece;
            bool have = false, lost = false, stalled = false;
            {
                std::unique_lock<std::mutex> lock(g_m);
                g_cv.wait_for(lock, std::chrono::milliseconds(500), [] { return !g_pieces.empty() || g_cancel || g_quit || (g_ds != direct::kNoSock && g_dDead); });
                if (g_cancel || g_quit) { res = Got::Failed; break; }
                const ULONGLONG idle = GetTickCount64() - g_lastActivity;
                if (!g_pieces.empty()) { piece = std::move(g_pieces.front()); g_pieces.pop_front(); have = true; }
                else if (g_ds != direct::kNoSock && (g_dDead || idle > kDirectStallMs)) lost = true;
                else if (idle > kStallMs) stalled = true;
            }
            if (lost)
            {
                DirectClose();
                LOG("zonesync: the direct link dropped, the session link carries on");
                { std::lock_guard<std::mutex> lock(g_m); g_lastActivity = GetTickCount64(); }
                for (uint32_t i : inflight) Request(map, p, i);
                continue;
            }
            if (!have && !net::IsConnected()) { Finish(FAILED, "the session with the host ended: Join Host again carries on where it stopped"); res = Got::Failed; break; }
            if (stalled && ++stalls <= 2)   // answers lost on the way: ask again before giving up
            {
                { std::lock_guard<std::mutex> lock(g_m); g_lastActivity = GetTickCount64(); }
                for (uint32_t i : inflight) Request(map, p, i);
                continue;
            }
            if (stalled) { Finish(FAILED, "the transfer stalled (the host stopped answering): Join Host again carries on where it stopped"); res = Got::Failed; break; }
            if (!have || piece.rel != p.e.rel || piece.off % kBlock) continue;
            auto it = std::find(inflight.begin(), inflight.end(), uint32_t(piece.off / kBlock));
            if (it == inflight.end()) continue;                     // a second answer after a link change
            const uint32_t want = *it;
            if (piece.status != 0 && piece.status != 2) { res = Got::Rebuilt; break; }
            const DWORD n = BlockLen(p.e, want);
            bool good = piece.status == 0 ? piece.data.size() == n : Unpack(piece.data, n, plain);
            const std::string& data = piece.status == 0 ? piece.data : plain;
            Hash h{};
            good = good && Sha().Of(reinterpret_cast<const uint8_t*>(data.data()), n, h) && h == p.e.blocks[want];
            if (!good)
            {
                // an answer from before a rebuild can still arrive: ask again, give up when it repeats
                if (++damaged > 8) { Finish(FAILED, "pieces of " + p.e.rel + " arrived damaged"); res = Got::Failed; break; }
                Request(map, p, want);
                continue;
            }
            if (!WriteAt(out, piece.off, data.data(), n)) { Finish(FAILED, "cannot write " + p.e.rel + " (error " + std::to_string(GetLastError()) + "; disk full?)"); res = Got::Failed; break; }
            inflight.erase(it); ++done;
            std::lock_guard<std::mutex> lock(g_m);
            g_fetched += n; g_wire += piece.data.size();
            if (onDirect) g_directBytes += n;
            const ULONGLONG now = GetTickCount64();
            if (now - g_rateAt >= 2000) { g_rate = (g_fetched - g_rateBase) / 1048576.0 * 1000.0 / double(now - g_rateAt); g_rateBase = g_fetched; g_rateAt = now; }
        }
        if (res == Got::Ok)
        {
            LARGE_INTEGER li; li.QuadPart = LONGLONG(p.e.size);
            if (!SetFilePointerEx(out, li, nullptr, FILE_BEGIN) || !SetEndOfFile(out)) { Finish(FAILED, "cannot size " + Narrow(p.part)); res = Got::Failed; }
        }
        CloseHandle(out);
        return res == Got::Ok && g_cancel ? Got::Failed : res;
    }

    bool IsPart(std::string& rel)   // strips the suffix
    {
        if (rel.size() <= 7 || _stricmp(rel.c_str() + rel.size() - 7, ".tcpart") != 0) return false;
        rel.resize(rel.size() - 7);
        return true;
    }

    // One round: the host's list, the plan, every part file, then the renames. Rebuilt = the host is linking or rewrote a file, ask again.
    Got FetchOnce(const std::string& map)
    {
        // 1. the host's file list with block hashes
        {
            net::Writer w; w.str(map); w.u8(rp::ConfigGetBool("direct", "enabled", true) ? 1 : 0);
            net::Send(net::MSG_ZONE_REQUEST2, w.buf);
        }
        {
            std::unique_lock<std::mutex> lock(g_m);
            const ULONGLONG start = GetTickCount64();
            while (!g_haveManifest && !g_cancel && !g_quit)
            {
                if (GetTickCount64() - start > kManifestWaitMs)
                {
                    lock.unlock();
                    Finish(FAILED, "the host did not answer: its Team Create is older than this one (both PCs need the same update), or it left");
                    return Got::Failed;
                }
                g_cv.wait_for(lock, std::chrono::milliseconds(500));
            }
            if (g_cancel || g_quit) return Got::Failed;
            if (!g_manifestOk && g_manifestBusy) return Got::Rebuilt;
        }
        if (!g_manifestOk) { Finish(FAILED, g_manifestError.empty() ? "the host refused" : g_manifestError); return Got::Failed; }
        if (g_manifestBlock != kBlock) { Finish(FAILED, "the host uses another piece size: both PCs need the same Team Create"); return Got::Failed; }

        // 2. what this PC has: every block of every file in its copy of the zone folder, and of the part files a stopped fetch left
        const std::wstring dir = ZoneDir(map);
        std::unordered_map<std::string, Source> have;
        std::map<std::string, std::vector<Hash>> local, parts;   // rel -> blocks of the file, of its part file
        {
            std::error_code ec;
            if (fs::exists(dir, ec))
                for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end && !g_cancel; it.increment(ec))
                {
                    if (!it->is_regular_file(ec)) continue;
                    std::string rel = Narrow(fs::relative(it->path(), dir, ec).wstring());
                    for (auto& c : rel) if (c == '\\') c = '/';
                    const bool part = IsPart(rel);
                    if (ec || !ValidRel(rel) || !Wanted(rel, map)) continue;
                    uint64_t s = 0, t = 0; std::vector<Hash> blocks;
                    if (!HashFile(it->path().wstring(), s, t, blocks, &g_cancel)) continue;
                    if (part) { parts[rel] = std::move(blocks); continue; }
                    for (size_t i = 0; i < blocks.size(); ++i) have.emplace(HashKey(blocks[i]), Source{ it->path().wstring(), uint64_t(i) * kBlock });
                    local[rel] = std::move(blocks);
                }
        }
        if (g_cancel) return Got::Failed;

        // 3. the plan: identical files are skipped, every other block stays in its part file, is copied from here or fetched
        std::vector<Plan> plans;
        uint64_t fetchTotal = 0, kept = 0;
        for (const auto& e : g_manifest)
        {
            auto l = local.find(e.rel);
            uint64_t s = 0, t = 0;
            if (l != local.end() && l->second == e.blocks && FileInfo(PathOf(dir, e.rel), s, t) && s == e.size) continue;
            Plan p; p.e = e; p.final = PathOf(dir, e.rel); p.part = p.final + L".tcpart";
            auto pt = parts.find(e.rel);
            for (uint32_t i = 0; i < uint32_t(e.blocks.size()); ++i)
            {
                if (pt != parts.end() && i < pt->second.size() && pt->second[i] == e.blocks[i]) { p.resume = true; kept += BlockLen(e, i); continue; }
                auto h = have.find(HashKey(e.blocks[i]));
                if (h != have.end()) p.copy.push_back({ i, h->second });
                else { p.fetch.push_back(i); fetchTotal += BlockLen(e, i); }
            }
            plans.push_back(std::move(p));
        }
        for (const auto& pt : parts)   // part files nothing waits for
        {
            bool planned = false;
            for (const auto& p : plans) if (p.e.rel == pt.first) { planned = true; break; }
            if (!planned) DeleteFileW((PathOf(dir, pt.first) + L".tcpart").c_str());
        }
        {
            std::lock_guard<std::mutex> lock(g_m);
            g_fetchTotal = fetchTotal; g_fetched = g_wire = g_directBytes = 0; g_reused = kept; g_fileCount = plans.size(); g_fileNo = 0;
            g_state = FETCHING; g_lastActivity = g_rateAt = GetTickCount64(); g_rateBase = 0; g_rate = 0;
        }
        LOG("zonesync: %s: %zu of %zu file(s) differ from the host's; %.1f MB to fetch, %.1f MB kept from a stopped fetch, the rest from this PC", map.c_str(), plans.size(), g_manifest.size(), fetchTotal / 1048576.0, kept / 1048576.0);

        // 4. every part file first (the old files stay readable as sources), then all renames
        for (size_t i = 0; i < plans.size(); ++i)
        {
            { std::lock_guard<std::mutex> lock(g_m); g_fileNo = i + 1; }
            const Got r = Assemble(map, plans[i]);
            if (r == Got::Rebuilt) LOG("zonesync: the host rebuilt %s during the transfer, asking for the new build", plans[i].e.rel.c_str());
            if (r == Got::Failed && g_cancel) Finish(FAILED, "cancelled");
            if (r != Got::Ok) return r;
        }
        for (size_t i = 0; i < plans.size(); ++i)
        {
            if (!MoveFileExW(plans[i].part.c_str(), plans[i].final.c_str(), MOVEFILE_REPLACE_EXISTING))
            {
                Finish(FAILED, "cannot replace " + Narrow(plans[i].final) + " (error " + std::to_string(GetLastError()) + "): close BO3 on this PC and press Join Host again (nothing is fetched twice)");
                return Got::Failed;
            }
            RememberHashes(plans[i].final, plans[i].e.blocks);
        }
        DeleteFileW((dir + L"\\teamcreate_host.txt").c_str());   // record of the first protocol version
        char msg[320];
        const unsigned long long secs = (unsigned long long)((GetTickCount64() - g_startedAt) / 1000);
        if (plans.empty()) snprintf(msg, sizeof msg, "already up to date (%zu file(s), checked in %llu s)", g_manifest.size(), secs);
        else snprintf(msg, sizeof msg, "%zu file(s) updated: %.1f MB from the host (%.1f MB on the wire%s), %.1f MB already on this PC, in %llu s", plans.size(), g_fetched / 1048576.0, g_wire / 1048576.0,
                      g_directBytes ? ", direct link" : "", g_reused / 1048576.0, secs);
        Finish(DONE, msg);
        return Got::Ok;
    }

    void ClientWork(std::string map)
    {
        ULONGLONG waitingSince = GetTickCount64();
        for (;;)
        {
            const Got r = FetchOnce(map);
            if (r != Got::Rebuilt) return;
            std::unique_lock<std::mutex> lock(g_m);
            if (g_fetched) waitingSince = GetTickCount64();
            if (GetTickCount64() - waitingSince > kRebuildWaitMs)
            {
                lock.unlock();
                Finish(FAILED, "the host kept linking the map: press Join Host again when its game is up");
                return;
            }
            g_state = ASKING; g_haveManifest = false; g_manifest.clear(); g_pieces.clear(); g_fetched = 0;
            if (g_cv.wait_for(lock, std::chrono::seconds(5), [] { return g_cancel || g_quit; })) return;
        }
    }

    void WorkerMain(std::string map)
    {
        try { ClientWork(map); }
        catch (...) { Finish(FAILED, "internal error (exception)"); }
        try { DirectClose(); } catch (...) {}
    }

    // Start a fetch unless one for this map runs (then a foreground request only clears the background flag).
    // Never joins a running worker from the network thread: an announce during a fetch is ignored there.
    std::string StartFetch(const std::string& map, bool background, bool mayJoinRunning)
    {
        std::unique_lock<std::mutex> lock(g_m);
        const bool running = g_state == ASKING || g_state == FETCHING;
        if (running && g_map == map) { if (!background) g_background = false; return std::string(StateName(g_state)) + " " + map; }
        if (running && !mayJoinRunning) return "BUSY";
        if (g_worker.joinable())
        {
            g_cancel = true; g_cv.notify_all();
            lock.unlock();
            g_worker.join();
            lock.lock();
        }
        g_cancel = false;
        g_map = map; g_msg.clear(); g_background = background;
        g_haveManifest = false; g_manifestBusy = false; g_manifestOk = 0; g_manifestError.clear(); g_manifest.clear(); g_pieces.clear();
        g_fetchTotal = g_fetched = g_reused = g_wire = g_directBytes = 0; g_fileNo = g_fileCount = 0; g_rate = 0;
        g_offerTried = false;
        if (g_offer.mode) g_haveOffer = false;   // the host sends a fresh endpoint with its file list
        g_state = ASKING; g_startedAt = g_lastActivity = GetTickCount64();
        g_worker = std::thread(WorkerMain, map);
        LOG("zonesync: %s the host's build of %s", background ? "background fetch of" : "asking for", map.c_str());
        return "ASKING " + map;
    }

    void OnManifest2(net::Reader& r, const std::string& map)
    {
        uint8_t ok = r.u8(); std::string error = r.str(); uint32_t block = r.u32(); uint16_t n = r.u16();
        std::vector<Entry> files;
        for (uint16_t i = 0; i < n && r.ok; ++i)
        {
            Entry e; e.rel = r.str(); e.size = U64(r); e.time = U64(r); std::string hashes = r.blob();
            if (!r.ok || !ValidRel(e.rel) || !Wanted(e.rel, map) || hashes.size() % kHash) continue;
            e.blocks.resize(hashes.size() / kHash);
            for (size_t k = 0; k < e.blocks.size(); ++k) memcpy(e.blocks[k].data(), &hashes[k * kHash], kHash);
            if (e.blocks.size() != size_t((e.size + kBlock - 1) / kBlock)) continue;
            files.push_back(std::move(e));
        }
        const bool parsed = r.ok;
        const uint8_t busy = r.u8();
        std::lock_guard<std::mutex> lock(g_m);
        if (g_state != ASKING || map != g_map || g_haveManifest) return;
        g_lastActivity = GetTickCount64();
        g_manifestOk = parsed ? ok : 0; g_manifestError = parsed ? error : "the host's file list was damaged";
        g_manifestBusy = parsed && r.ok && busy;
        g_manifestBlock = block; g_manifest = std::move(files); g_haveManifest = true;
        g_cv.notify_all();
    }

    void OnOffer(net::Reader& r, const std::string& ip)
    {
        direct::Offer o; o.ip = ip; o.port = r.u16(); o.token = r.blob();
        if (!r.ok || !o.port || o.token.empty()) return;
        const uint8_t mode = r.u8();
        o.mode = r.ok && mode == 1 ? 1 : 0;
        std::lock_guard<std::mutex> lock(g_m);
        // a punched endpoint is fresh with every offer, an open port stays
        if (!o.mode && g_haveOffer && !g_offer.mode && g_offer.ip == o.ip && g_offer.port == o.port && g_offer.token == o.token) return;
        g_offer = o; g_haveOffer = true; g_offerTried = false;
        LOG("zonesync: the host offers a direct link for builds (%s)", o.mode ? "hole punching" : "open port");
    }

    // A new host build: a client on that map fetches it in the background (not while its BO3 runs: the files are in use).
    void OnAnnounce(const std::string& map)
    {
        if (!rp::ConfigGetBool("zonesync", "prefetch", true)) return;
        if (!ValidMap(map) || _stricmp(MapBaseOf(net::LocalMap()).c_str(), map.c_str()) != 0) return;
        if (ProcessRunning(L"BlackOps3.exe")) { LOG("zonesync: the host has a new build of %s; not fetched now (BO3 is running here), Join Host gets it", map.c_str()); return; }
        StartFetch(map, true, false);
    }

    // Host play state: the host asks its Play Test once a second whether its game is up ("PlayTest hosting")
    // and tells the peers on a change, their Play button turns into Join Host while it is ("TeamCreate hostplaying").
    std::mutex g_playMutex;                // guards the four below (main and network threads)
    bool g_hostPlaying = false;            // host: last state sent
    std::string g_hostPlayMap;
    bool g_peerHostPlaying = false;        // client: what the host said
    std::string g_peerHostPlayMap;
    ULONGLONG g_playStateAt = 0;

    void SendPlayState(uint32_t peer)      // peer 0 = every client
    {
        net::Writer w;
        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            w.str(g_hostPlayMap); w.u8(g_hostPlaying ? 1 : 0);
        }
        if (peer) net::SendTo(peer, net::MSG_PLAY_STATE, w.buf);
        else net::Send(net::MSG_PLAY_STATE, w.buf);
    }
    void PlayStateTick()
    {
        const ULONGLONG now = GetTickCount64();
        if (now - g_playStateAt < 1000) return;
        g_playStateAt = now;
        bool playing = false;
        if (net::IsHost() && net::IsRunning() && rp::host->Command)
        {
            const char* r = rp::host->Command(rp::self, "PlayTest", "hosting");
            playing = r && r[0] == '1' && (r[1] == 0 || r[1] == ' ');
        }
        const std::string map = playing ? MapBaseOf(net::LocalMap()) : std::string();
        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            if (playing == g_hostPlaying && map == g_hostPlayMap) return;
            g_hostPlaying = playing; g_hostPlayMap = map;
        }
        if (net::IsHost() && net::IsRunning()) SendPlayState(0);
        LOG("zonesync: host Play Test game %s%s", playing ? "up, map " : "down", map.c_str());
    }
}

namespace zonesync
{
    void OnMessage(const net::Message& m)
    {
        if (m.from == net::LocalPeerId()) return;
        net::Reader r(m.payload);
        std::string map = r.str();
        if (!r.ok) return;
        if (net::IsHost())
        {
            if (m.type == net::MSG_ZONE_REQUEST) ServeManifestV1(m.from, map);
            else if (m.type == net::MSG_ZONE_REQUEST2)
            {
                if (!ValidMap(map)) return;
                // a client that dials direct links gets the port opened and the offer before its file list
                const uint8_t caps = r.u8();
                if (r.ok && (caps & 1) && rp::ConfigGetBool("direct", "enabled", true)) { direct::HostEnsure(ServeDirect, SendOffer); direct::HostAsk(m.from); }
                QueueManifest(m.from, map);
            }
            else if (m.type == net::MSG_ZONE_GET) ServeChunk(m.from, r, map);
            else if (m.type == net::MSG_DIRECT_PUNCH) { const uint16_t port = r.u16(); if (r.ok) direct::HostPunch(map, port); }   // the first string is the address
            return;
        }
        if (m.from != net::kHostPeerId) return;
        if (m.type == net::MSG_ZONE_MANIFEST2) OnManifest2(r, map);
        else if (m.type == net::MSG_ZONE_CHUNK) OnChunk(r, map);
        else if (m.type == net::MSG_ZONE_ANNOUNCE) OnAnnounce(map);
        else if (m.type == net::MSG_DIRECT_OFFER) OnOffer(r, map);
        else if (m.type == net::MSG_PLAY_STATE)
        {
            const bool playing = r.u8() != 0;
            if (!r.ok) return;
            std::lock_guard<std::mutex> lock(g_playMutex);
            g_peerHostPlaying = playing; g_peerHostPlayMap = playing ? map : std::string();
        }
    }

    void OnPeerJoined(uint32_t peerId)
    {
        if (!net::IsHost()) return;
        SendPlayState(peerId);   // always, so a reconnecting peer drops a stale "playing"
        if (g_watchMap.empty() || g_announcedSig.empty()) return;
        net::Writer w; w.str(g_watchMap);
        net::SendTo(peerId, net::MSG_ZONE_ANNOUNCE, w.buf);
    }

    void Tick()
    {
        PlayStateTick();
        const ULONGLONG now = GetTickCount64();
        if (now - g_watchAt < 3000) return;
        g_watchAt = now;
        if (!net::IsHost() || !net::IsRunning())
        {
            g_watchMap.clear();
            direct::HostStop(false);
            if (!net::IsConnected()) { std::lock_guard<std::mutex> lock(g_m); g_haveOffer = false; }
            return;
        }
        const std::string map = MapBaseOf(net::LocalMap());
        if (!ValidMap(map)) return;
        if (map != g_watchMap) { g_watchMap = map; g_pendingSig.clear(); g_announcedSig.clear(); }
        const std::string sig = BuildSignature(map);
        if (sig.empty()) return;
        if (sig != g_pendingSig) { g_pendingSig = sig; g_pendingSince = now; return; }
        if (sig == g_announcedSig) return;
        if (ProcessRunning(L"linker_modtools.exe")) { g_pendingSince = now; return; }   // the linker still writes
        if (now - g_pendingSince < 15000) return;
        g_announcedSig = sig;
        QueueManifest(0, map);                                               // hash it now, so the peers' requests are answered at once
        net::Writer w; w.str(map);
        net::Send(net::MSG_ZONE_ANNOUNCE, w.buf);
        LOG("zonesync: build of %s ready; told the peers", map.c_str());
    }

    void Shutdown()
    {
        g_quit = true; g_cancel = true;
        { std::lock_guard<std::mutex> lock(g_hostMutex); g_hostCv.notify_all(); }
        { std::lock_guard<std::mutex> lock(g_m); g_cv.notify_all(); }
        if (g_hostThread.joinable()) g_hostThread.join();
        if (g_worker.joinable()) g_worker.join();
        direct::HostStop(true);
    }

    std::string Command(const std::string& raw)
    {
        std::string arg = raw;
        while (!arg.empty() && arg.back() == ' ') arg.pop_back();
        if (arg.rfind("fetch ", 0) == 0)
        {
            std::string map = arg.substr(6);
            if (!ValidMap(map)) return "FAILED bad map name '" + map + "'";
            if (net::IsHost() || !net::IsConnected()) return "FAILED not connected to a Team Create host";
            if (ZoneDir(map).empty()) return "FAILED the Black Ops III folder is unknown";
            return StartFetch(map, false, true);
        }
        if (arg == "hostplaying")   // Play Test's Play button: "1 <map>" while the Team Create host's Play Test game is up, else "0"
        {
            std::lock_guard<std::mutex> lock(g_playMutex);
            // only on the host's map: Join Host fetches and plays the build of the map open here
            if (net::IsHost() || !net::IsConnected() || !g_peerHostPlaying) return "0";
            if (_stricmp(MapBaseOf(net::LocalMap()).c_str(), g_peerHostPlayMap.c_str()) != 0) return "0 other map " + g_peerHostPlayMap;
            return "1 " + g_peerHostPlayMap;
        }
        if (arg == "direct") return direct::Status();   // host: the router port, no addresses
        std::lock_guard<std::mutex> lock(g_m);
        if (arg == "cancel")
        {
            if (g_state == ASKING || g_state == FETCHING) { g_cancel = true; g_cv.notify_all(); g_state = FAILED; g_msg = "cancelled"; }
            return std::string(StateName(g_state)) + " " + g_msg;
        }
        char buf[512];
        const unsigned long long waited = (unsigned long long)((GetTickCount64() - g_startedAt) / 1000);
        if (g_state == ASKING)
            snprintf(buf, sizeof buf, "ASKING the host for its build of %s (%llu s; %s)", g_map.c_str(), waited, g_manifestBusy ? "the host is linking, waiting for it" : "it reads its files once per build");
        else if (g_state == FETCHING)
            snprintf(buf, sizeof buf, "FETCHING%s file %zu/%zu, %.1f/%.1f MB from the host, %.1f MB already on this PC, %.1f MB/s%s", g_background ? " (background)" : "",
                     g_fileNo, g_fileCount, g_fetched / 1048576.0, g_fetchTotal / 1048576.0, g_reused / 1048576.0, g_rate, g_ds != direct::kNoSock && !g_dDead ? " over the direct link" : "");
        else
            snprintf(buf, sizeof buf, "%s %s", StateName(g_state), g_msg.c_str());
        return buf;
    }
}
