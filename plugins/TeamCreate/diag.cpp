#include <windows.h>
#include <shlobj.h>
#include <mutex>
#include <set>
#include <vector>
#include <deque>
#include <cstdio>
#include <algorithm>
#include <cctype>
#include "diag.h"
#include "compat.h"
#include "presence.h"
#include <map>

#pragma comment(lib, "shell32.lib")

// See diag.h. Wire: MSG_LOG_LINES = blob of whole log lines + trailer (u32 tokenLo, tokenHi, offsetLo, offsetHi) so a rejoin drops lines already held,
// MSG_FILE = str kind ("dump" | "prevlog"), str name, u32 mtimeLo, mtimeHi, blob. MSG_FILE_PART = same header + u32 total, u32 offset, blob (max 256 KB,
// fed only while the diagnostics queue is short). A dump goes to each peer once (peerlogs\sent_dumps.txt).
namespace
{
    std::mutex   g_mutex;
    std::wstring g_logPath;                 // the host's RadiantPlugins.log
    uint64_t     g_logStartOffset = 0;      // log size at plugin load (this process' lines follow)
    uint64_t     g_streamOffset = 0;        // next byte to stream, 0 = not streaming
    bool         g_streaming = false;
    uint64_t     g_lastStreamTick = 0, g_lastScanTick = 0;
    std::set<std::wstring> g_knownDumps;    // dumps present at load or already shipped (path)
    bool         g_clientShipped = false;   // client: one shipment per process
    bool         g_shipAll = false;         // queued: ship to everyone
    std::deque<uint32_t> g_shipTo;          // queued: ship to these peers (host, newcomers)
    std::set<std::string> g_markedPeers;    // peers whose live log got a session marker line
    size_t g_linesSent = 0, g_filesSent = 0, g_linesRecv = 0, g_filesRecv = 0;
    std::string g_lastFile;

    constexpr size_t kBatchMax = 64 * 1024;       // one MSG_LOG_LINES frame at most
    constexpr size_t kPartMax = 256 * 1024;       // one MSG_FILE_PART frame at most
    constexpr size_t kFeedBelow = 512 * 1024;     // feed the next part only while the diagnostics queue is shorter
    constexpr uint64_t kLocalDumpMax = 10ull << 20;   // LocalDumps copies above this are not shipped
    constexpr uint64_t kFileMax = 512ull << 20;   // receiver bound on one incoming file
    uint64_t g_logToken = 0;                     // identifies this log stream (changes on truncation)
    struct Outgoing { uint32_t to; std::string kind, name, peerKey, data; std::wstring path; FILETIME mtime; uint64_t size, offset; };
    std::deque<Outgoing> g_out;                  // files being sent in parts, oldest first
    std::set<std::string> g_sent;                // "peerKey\tname\tsize": dumps already shipped to that peer (persisted)
    bool g_sentLoaded = false;
    struct LogPos { uint64_t token = 0, end = 0; };
    std::map<std::string, LogPos> g_logPos;      // receiver: per peer, the sender stream and where the stored copy ends
    constexpr size_t kPrevTailMax = 256 * 1024;   // previous session tail
    constexpr size_t kStartBacklogMax = 256 * 1024;   // this process' lines before the session started

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

    // Test hooks: RTC_DIAG_LOG names the log to stream, RTC_DIAG_CRASHDUMPS the LocalDumps folder
    std::wstring EnvPath(const wchar_t* name)
    {
        wchar_t buf[MAX_PATH * 2] = {}; DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
        return n && n < MAX_PATH * 2 ? std::wstring(buf) : std::wstring();
    }
    std::wstring HostLogPath()
    {
        std::wstring forced = EnvPath(L"RTC_DIAG_LOG"); if (!forced.empty()) return forced;
        HMODULE h = GetModuleHandleW(L"RadiantPlugins.dll");
        wchar_t buf[MAX_PATH * 2] = {};
        if (!h || !GetModuleFileNameW(h, buf, MAX_PATH * 2)) return L"";
        std::wstring p = buf;
        size_t slash = p.find_last_of(L"\\/");
        return (slash == std::wstring::npos ? L"" : p.substr(0, slash + 1)) + L"RadiantPlugins.log";
    }

    uint64_t FileSize(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return 0;
        return (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    }

    // Read [from, from+max) of a file opened for shared reading (the log stays open for appending)
    std::string ReadRange(const std::wstring& path, uint64_t from, size_t max)
    {
        FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
        if (!f) return {};
        std::string out;
        if (_fseeki64(f, int64_t(from), SEEK_SET) == 0)
        {
            out.resize(max);
            size_t n = fread(&out[0], 1, max, f);
            out.resize(n);
        }
        fclose(f);
        return out;
    }

    std::string ReadWhole(const std::wstring& path, size_t max)
    {
        uint64_t size = FileSize(path);
        if (size == 0 || size > max) return {};
        return ReadRange(path, 0, size_t(size));
    }

    bool WriteFileBytes(const std::wstring& path, const std::string& data, bool append)
    {
        FILE* f = _wfsopen(path.c_str(), append ? L"ab" : L"wb", _SH_DENYNO);
        if (!f) return false;
        bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
        fclose(f);
        return ok;
    }

    void EnsureDir(const std::wstring& dir) { SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr); }

    std::string SafeName(const std::string& name)
    {
        std::string s;
        for (char c : name) s += (isalnum(uint8_t(c)) || c == '_' || c == '-' || c == '.') ? c : '_';
        while (!s.empty() && s.back() == '.') s.pop_back();
        return s.empty() ? "peer" : s.substr(0, 48);
    }

    struct DumpFile { std::wstring path; std::string name; uint64_t size; FILETIME mtime; };

    void ScanDir(const std::wstring& dir, const wchar_t* pattern, const std::string& prefix, std::vector<DumpFile>& out)
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            DumpFile d; d.path = dir + L"\\" + fd.cFileName; d.name = prefix + Narrow(fd.cFileName);
            d.size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow; d.mtime = fd.ftLastWriteTime;
            out.push_back(std::move(d));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // Every crash dump this machine could have: Radiant's own (<root>\bin\*_modtools.exe*.dmp), plugin isolation
    // dumps (<plugins>\<Name>\crash-*.dmp) and Windows LocalDumps (%LOCALAPPDATA%\CrashDumps, usually skipped by size)
    std::vector<DumpFile> ScanDumps()
    {
        std::vector<DumpFile> out;
        std::wstring root = rp::host->RadiantRoot(rp::self) ? rp::host->RadiantRoot(rp::self) : L"";
        if (!root.empty()) ScanDir(root + L"\\bin", L"*_modtools.exe*.dmp", "", out);
        std::wstring plugins = rp::host->PluginsRoot(rp::self) ? rp::host->PluginsRoot(rp::self) : L"";
        if (!plugins.empty())
        {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((plugins + L"\\*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
                    ScanDir(plugins + L"\\" + fd.cFileName, L"crash-*.dmp", Narrow(fd.cFileName) + "-", out);
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
        wchar_t local[MAX_PATH] = {};
        std::wstring crash = EnvPath(L"RTC_DIAG_CRASHDUMPS");
        if (crash.empty() && SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local))) crash = std::wstring(local) + L"\\CrashDumps";
        if (!crash.empty()) ScanDir(crash, L"*_modtools.exe.*.dmp", "LocalDumps-", out);
        return out;
    }

    bool Recent(const FILETIME& ft, unsigned maxAgeDays)
    {
        FILETIME now; GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER a, b; a.LowPart = ft.dwLowDateTime; a.HighPart = ft.dwHighDateTime; b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
        if (a.QuadPart > b.QuadPart) return true;
        return (b.QuadPart - a.QuadPart) / 10000000ULL <= uint64_t(maxAgeDays) * 86400ULL;
    }

    std::wstring SentPath() { return std::wstring(rp::host->PluginDir(rp::self)) + L"\\peerlogs\\sent_dumps.txt"; }
    void LoadSent()
    {
        if (g_sentLoaded) return; g_sentLoaded = true;
        std::string text = ReadWhole(SentPath(), 4 << 20); size_t at = 0;
        while (at < text.size()) { size_t nl = text.find('\n', at); if (nl == std::string::npos) nl = text.size(); std::string line = text.substr(at, nl - at); if (!line.empty() && line.back() == '\r') line.pop_back(); if (!line.empty()) g_sent.insert(line); at = nl + 1; }
    }
    // Who a shipment goes to, by name: the host keys each client, a client keys the host
    std::string PeerKey(uint32_t to)
    {
        uint32_t id = to ? to : net::kHostPeerId;
        for (const auto& p : presence::Snapshot()) if (p.id == id && !p.name.empty()) { std::string k = p.name; for (auto& c : k) c = char(tolower(uint8_t(c))); return k; }
        return "peer" + std::to_string(id);
    }
    // Queue a file (from disk, or memory for the log tail), Pump sends it in parts
    void Enqueue(uint32_t to, const char* kind, const std::string& name, const FILETIME& mtime, const std::wstring& path, std::string data, uint64_t size, bool force)
    {
        std::string key = PeerKey(to), record = key + "\t" + name + "\t" + std::to_string(size);
        if (!strcmp(kind, "dump"))
        {
            LoadSent();
            if (!force && g_sent.count(record)) return;            // peer has it from an earlier session
            for (const auto& o : g_out) if (o.to == to && o.name == name) return;
        }
        g_out.push_back({ to, kind, name, key, std::move(data), path, mtime, size, 0 });
    }
    void Pump()
    {
        for (int sent = 0; sent < 16 && !g_out.empty(); ++sent)
        {
            Outgoing& o = g_out.front();
            bool gone = !net::IsConnected();
            if (!gone && o.to && net::IsHost()) { gone = true; for (const auto& p : presence::Snapshot()) if (p.id == o.to) gone = false; }
            if (gone) { g_out.pop_front(); continue; }             // session or peer gone
            if (net::QueuedBytes(o.to, 2) >= kFeedBelow) return;
            size_t n = size_t(std::min<uint64_t>(kPartMax, o.size - o.offset));
            std::string part = o.path.empty() ? o.data.substr(size_t(o.offset), n) : ReadRange(o.path, o.offset, n);
            if (part.size() != n) { LOG("diag: '%s' changed or vanished while sending; dropped", o.name.c_str()); g_out.pop_front(); continue; }
            net::Writer w; w.str(o.kind); w.str(o.name); w.u32(o.mtime.dwLowDateTime); w.u32(o.mtime.dwHighDateTime);
            w.u32(uint32_t(o.size)); w.u32(uint32_t(o.offset)); w.blob(part);
            if (o.to && net::IsHost()) net::SendTo(o.to, net::MSG_FILE_PART, w.buf); else net::Send(net::MSG_FILE_PART, w.buf);
            o.offset += n;
            if (o.offset >= o.size)
            {
                g_filesSent++;
                LOG("diag: sent %s '%s' (%llu bytes) to %s", o.kind.c_str(), o.name.c_str(), (unsigned long long)o.size, o.peerKey.c_str());
                if (o.kind == "dump")
                {
                    std::string record = o.peerKey + "\t" + o.name + "\t" + std::to_string(o.size);
                    if (g_sent.insert(record).second) { EnsureDir(diag::PeerLogsDir()); WriteFileBytes(SentPath(), record + "\n", true); }
                }
                g_out.pop_front();
            }
        }
    }
    // Targets for "everyone": the host sends to each client by id, a client to the host
    std::vector<uint32_t> Everyone()
    {
        std::vector<uint32_t> ids;
        if (!net::IsHost()) { ids.push_back(0); return ids; }
        for (const auto& p : presence::Snapshot()) if (p.id != net::LocalPeerId() && p.id != 0) ids.push_back(p.id);
        return ids;
    }
    bool Shippable(const DumpFile& d)
    {
        if (d.size == 0 || d.size > uint64_t(Cfg().diagDumpMaxMb) << 20) return false;
        if (d.name.rfind("LocalDumps-", 0) == 0 && d.size > kLocalDumpMax) return false;   // WER copies are huge
        return true;
    }

    // The previous session's log tail and the recent dumps
    void Ship(uint32_t to, bool force)
    {
        if (!Cfg().diagShare || !net::IsRunning()) return;
        std::vector<uint32_t> targets; if (to) targets.push_back(to); else targets = Everyone();
        std::string tail;
        if (g_logStartOffset > 0)
        {
            uint64_t from = g_logStartOffset > kPrevTailMax ? g_logStartOffset - kPrevTailMax : 0;
            tail = ReadRange(g_logPath, from, size_t(g_logStartOffset - from));
            size_t nl = tail.find('\n');
            if (from > 0 && nl != std::string::npos) tail.erase(0, nl + 1);   // start on a whole line
        }
        std::vector<DumpFile> dumps = ScanDumps();
        dumps.erase(std::remove_if(dumps.begin(), dumps.end(), [](const DumpFile& d) { return !Recent(d.mtime, Cfg().diagDumpMaxAgeDays) || !Shippable(d); }), dumps.end());
        std::sort(dumps.begin(), dumps.end(), [](const DumpFile& a, const DumpFile& b) { return CompareFileTime(&a.mtime, &b.mtime) > 0; });
        if (dumps.size() > 5) dumps.resize(5);
        FILETIME now; GetSystemTimeAsFileTime(&now);
        for (uint32_t t : targets)
        {
            if (!tail.empty()) Enqueue(t, "prevlog", "RadiantPlugins.prev.log", now, L"", tail, tail.size(), true);
            for (const DumpFile& d : dumps) Enqueue(t, "dump", d.name, d.mtime, d.path, std::string(), d.size, force);
        }
        for (const DumpFile& d : dumps) g_knownDumps.insert(d.path);
    }

    // A dump that appeared during the session (a plugin fault under crash isolation): ship it once
    void ShipNewDumps()
    {
        if (!Cfg().diagShare || !net::IsConnected()) return;
        for (const DumpFile& d : ScanDumps())
        {
            if (g_knownDumps.count(d.path)) continue;
            g_knownDumps.insert(d.path);
            if (!Shippable(d)) continue;
            for (uint32_t t : Everyone()) Enqueue(t, "dump", d.name, d.mtime, d.path, std::string(), d.size, false);
        }
    }

    void StreamLog()
    {
        if (!Cfg().diagShare || !net::IsConnected() || g_logPath.empty()) return;
        uint64_t size = FileSize(g_logPath);
        if (!g_streaming)
        {
            g_streaming = true;
            g_streamOffset = size - g_logStartOffset > kStartBacklogMax ? size - kStartBacklogMax : g_logStartOffset;
        }
        if (size < g_streamOffset) { g_streamOffset = 0; ++g_logToken; }    // log truncated or rotated: new stream
        if (size == g_streamOffset) return;
        for (int batch = 0; batch < 4 && size > g_streamOffset; ++batch)
        {
            std::string chunk = ReadRange(g_logPath, g_streamOffset, size_t(std::min<uint64_t>(size - g_streamOffset, kBatchMax)));
            if (chunk.empty()) return;
            size_t cut = chunk.find_last_of('\n');
            if (cut == std::string::npos) { if (chunk.size() < kBatchMax) return; cut = chunk.size() - 1; }   // wait for the line to finish
            chunk.resize(cut + 1);
            net::Writer w; w.blob(chunk);
            w.u32(uint32_t(g_logToken)); w.u32(uint32_t(g_logToken >> 32)); w.u32(uint32_t(g_streamOffset)); w.u32(uint32_t(g_streamOffset >> 32));
            net::Send(net::MSG_LOG_LINES, w.buf);
            g_streamOffset += chunk.size();
            g_linesSent += size_t(std::count(chunk.begin(), chunk.end(), '\n'));
        }
    }

    std::wstring PeerDir(const std::string& peerName)
    {
        std::wstring dir = diag::PeerLogsDir() + L"\\" + Widen(SafeName(peerName));
        EnsureDir(dir);
        return dir;
    }
}

namespace diag
{
    std::wstring PeerLogsDir() { return std::wstring(rp::host->PluginDir(rp::self)) + L"\\peerlogs"; }

    void Init()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_logPath = HostLogPath();
        g_logStartOffset = FileSize(g_logPath);
        g_logToken = (uint64_t(GetCurrentProcessId()) << 32) ^ GetTickCount64() ^ (uint64_t(g_logStartOffset) << 8);
        for (const DumpFile& d : ScanDumps()) g_knownDumps.insert(d.path);
        LOG("diag: host log '%s' (%llu bytes before this start), %zu dump(s) on this machine, share=%d keep=%d",
            Narrow(g_logPath).c_str(), (unsigned long long)g_logStartOffset, g_knownDumps.size(), Cfg().diagShare, Cfg().diagKeep);
    }

    void Shutdown()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_streaming = false; g_shipAll = false; g_shipTo.clear(); g_markedPeers.clear(); g_out.clear();
    }

    void OnConnected()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_streaming = false;                        // restart the stream at the backlog window
        if (!g_clientShipped) { g_clientShipped = true; g_shipAll = true; }
    }

    void OnPeerJoined(uint32_t peerId)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_streaming = false;
        g_shipTo.push_back(peerId);
    }

    bool g_forceShip = false;
    void ShipNow() { std::lock_guard<std::mutex> lock(g_mutex); g_shipAll = true; g_forceShip = true; }

    void Tick()
    {
        if (!net::IsRunning()) { std::lock_guard<std::mutex> lock(g_mutex); g_streaming = false; g_out.clear(); return; }
        uint64_t now = GetTickCount64();
        bool shipAll = false, force = false; std::deque<uint32_t> shipTo;
        { std::lock_guard<std::mutex> lock(g_mutex); shipAll = g_shipAll; g_shipAll = false; force = g_forceShip; g_forceShip = false; shipTo.swap(g_shipTo); }
        if (shipAll && net::IsConnected()) Ship(0, force);
        for (uint32_t id : shipTo) if (net::IsHost()) Ship(id, false);
        Pump();
        if (now - g_lastStreamTick >= 1000) { g_lastStreamTick = now; StreamLog(); }
        if (now - g_lastScanTick >= 30000) { g_lastScanTick = now; ShipNewDumps(); }
    }

    void OnMessage(const net::Message& m, const std::string& peerName)
    {
        if (!Cfg().diagKeep || m.from == net::LocalPeerId() || m.from == 0) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        std::string who = peerName.empty() || peerName.rfind("peer", 0) == 0 ? "peer" + std::to_string(m.from) : peerName;
        if (m.type == net::MSG_LOG_LINES)
        {
            net::Reader r(m.payload);
            std::string text = r.blob();
            if (!r.ok || text.empty()) return;
            std::wstring file = PeerDir(who) + L"\\RadiantPlugins.log";
            uint32_t tl = r.u32(), th = r.u32(), ol = r.u32(), oh = r.u32();
            if (r.ok)
            {
                // Drop what this file already holds from the same stream (a rejoin restarts it up to 256 KB back).
                // The position is kept next to the copy.
                uint64_t token = (uint64_t(th) << 32) | tl, offset = (uint64_t(oh) << 32) | ol;
                std::wstring posFile = file + L".pos";
                auto it = g_logPos.find(who);
                if (it == g_logPos.end())
                {
                    LogPos p; std::string saved = ReadWhole(posFile, 256);
                    if (!saved.empty()) sscanf_s(saved.c_str(), "%llu %llu", (unsigned long long*)&p.token, (unsigned long long*)&p.end);
                    it = g_logPos.emplace(who, p).first;
                }
                if (it->second.token == token && offset < it->second.end)
                {
                    uint64_t skip = it->second.end - offset;
                    if (skip >= text.size()) return;
                    text.erase(0, size_t(skip)); offset += skip;
                }
                it->second.token = token; it->second.end = offset + text.size();
                char pos[64]; _snprintf_s(pos, _TRUNCATE, "%llu %llu", (unsigned long long)token, (unsigned long long)it->second.end);
                WriteFileBytes(posFile, pos, false);
            }
            if (!g_markedPeers.count(who))
            {
                g_markedPeers.insert(who);
                SYSTEMTIME st; GetLocalTime(&st);
                char marker[256];
                _snprintf_s(marker, _TRUNCATE, "==== %04d-%02d-%02d %02d:%02d:%02d local: log lines from '%s' (peer %u) ====\n",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, who.c_str(), m.from);
                WriteFileBytes(file, marker, true);
            }
            WriteFileBytes(file, text, true);
            g_linesRecv += size_t(std::count(text.begin(), text.end(), '\n'));
        }
        else if (m.type == net::MSG_FILE_PART)
        {
            net::Reader r(m.payload);
            std::string kind = r.str(), name = SafeName(r.str());
            FILETIME mtime; mtime.dwLowDateTime = r.u32(); mtime.dwHighDateTime = r.u32();
            uint32_t total = r.u32(), offset = r.u32(); std::string data = r.blob();
            if (!r.ok || name.empty() || total == 0 || total > kFileMax || data.size() > kPartMax || uint64_t(offset) + data.size() > total) return;
            std::wstring path = PeerDir(who) + L"\\" + Widen(name), part = path + L".part";
            if (kind == "dump" && FileSize(path) == total) return;       // already here
            if (offset == 0) { if (!WriteFileBytes(part, data, false)) { LOG("diag: cannot write '%s'", Narrow(part).c_str()); return; } }
            else if (FileSize(part) == offset) WriteFileBytes(part, data, true);
            else return;                                                  // a part went missing: wait for the next shipment
            if (uint64_t(offset) + data.size() < total) return;
            if (!MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) { LOG("diag: cannot finish '%s'", Narrow(path).c_str()); return; }
            HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) { if (kind == "dump") SetFileTime(h, nullptr, nullptr, &mtime); CloseHandle(h); }
            g_filesRecv++; g_lastFile = who + "/" + name;
            LOG("diag: received %s '%s' (%u bytes) from '%s' -> %s", kind.c_str(), name.c_str(), total, who.c_str(), Narrow(path).c_str());
        }
        else if (m.type == net::MSG_FILE)
        {
            net::Reader r(m.payload);
            std::string kind = r.str(), name = SafeName(r.str());
            FILETIME mtime; mtime.dwLowDateTime = r.u32(); mtime.dwHighDateTime = r.u32();
            std::string data = r.blob();
            if (!r.ok || name.empty() || data.empty()) return;
            std::wstring path = PeerDir(who) + L"\\" + Widen(name);
            if (kind == "dump" && FileSize(path) == data.size()) return;   // already here
            if (!WriteFileBytes(path, data, false)) { LOG("diag: cannot write '%s'", Narrow(path).c_str()); return; }
            HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) { if (kind == "dump") SetFileTime(h, nullptr, nullptr, &mtime); CloseHandle(h); }
            g_filesRecv++; g_lastFile = who + "/" + name;
            LOG("diag: received %s '%s' (%zu bytes) from '%s' -> %s", kind.c_str(), name.c_str(), data.size(), who.c_str(), Narrow(path).c_str());
        }
    }

    std::string Status()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        char buf[512];
        _snprintf_s(buf, _TRUNCATE, "diag: share=%d keep=%d streaming=%d offset=%llu sent %zu lines / %zu files, received %zu lines / %zu files (last '%s'), peerlogs=%s",
            Cfg().diagShare, Cfg().diagKeep, g_streaming, (unsigned long long)g_streamOffset, g_linesSent, g_filesSent, g_linesRecv, g_filesRecv, g_lastFile.c_str(), Narrow(PeerLogsDir()).c_str());
        return buf;
    }
}
