// skip_unchanged helpers shared by the compile and link wrappers: identities, a key=value state file, the read journal
// the in-process DLLs write, parallel checks and an output tap that keeps a run's error lines.
#pragma once
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace unchanged
{
    inline std::string Utf8(const std::wstring& w)
    {
        if (w.empty()) return "";
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string out(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), n, nullptr, nullptr);
        return out;
    }

    inline std::wstring FromUtf8(const std::string& s)
    {
        if (s.empty()) return L"";
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring out(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
        return out;
    }

    inline std::wstring LowerW(std::wstring s) { if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size()); return s; }

    inline bool StartsWith(const std::wstring& s, const std::wstring& prefix) { return s.size() >= prefix.size() && !s.compare(0, prefix.size(), prefix); }
    inline bool EndsWith(const std::wstring& s, const wchar_t* tail) { const size_t n = wcslen(tail); return s.size() >= n && !s.compare(s.size() - n, n, tail); }

    // size + last write time, "" when missing
    inline std::string Identity(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA fa = {};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return "";
        char out[64];
        snprintf(out, sizeof out, "%llu,%llu", ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow,
                 ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime);
        return out;
    }

    inline bool ReadAll(const std::wstring& path, std::string& out)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size = {};
        if (!GetFileSizeEx(h, &size) || size.QuadPart > (1LL << 30)) { CloseHandle(h); return false; }
        out.resize((size_t)size.QuadPart);
        size_t off = 0;
        while (off < out.size())
        {
            DWORD got = 0;
            const DWORD want = (DWORD)(out.size() - off > (1u << 22) ? (1u << 22) : out.size() - off);
            if (!ReadFile(h, out.data() + off, want, &got, nullptr) || !got) { CloseHandle(h); return false; }
            off += got;
        }
        CloseHandle(h);
        return true;
    }

    template <class F> inline void Lines(const std::string& text, F&& f)
    {
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            size_t stop = end;
            if (stop > pos && text[stop - 1] == '\r') --stop;
            f(text.substr(pos, stop - pos));
            pos = end + 1;
        }
    }

    struct Sha256
    {
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        Sha256()
        {
            if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) alg = nullptr;
            else if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) < 0) hash = nullptr;
        }
        ~Sha256() { if (hash) BCryptDestroyHash(hash); if (alg) BCryptCloseAlgorithmProvider(alg, 0); }
        Sha256(const Sha256&) = delete;
        Sha256& operator=(const Sha256&) = delete;
        // length-prefixed, so ("ab","c") and ("a","bc") differ
        void Add(const std::string& s)
        {
            if (!hash) return;
            const unsigned long long n = s.size();
            BCryptHashData(hash, (PUCHAR)&n, sizeof n, 0);
            if (n) BCryptHashData(hash, (PUCHAR)s.data(), (ULONG)n, 0);
        }
        std::string Hex()
        {
            unsigned char digest[32] = {};
            if (!hash || BCryptFinishHash(hash, digest, sizeof digest, 0) < 0) return "";
            BCryptDestroyHash(hash);
            hash = nullptr;
            char out[65];
            for (int i = 0; i < 32; ++i) snprintf(out + 2 * i, 3, "%02x", digest[i]);
            return out;
        }
    };

    // key=value lines, the value runs to the end of the line
    struct State
    {
        std::map<std::string, std::string> kv;
        bool Load(const std::wstring& path)
        {
            std::string text;
            if (!ReadAll(path, text)) return false;
            Lines(text, [&](const std::string& line) {
                const size_t eq = line.find('=');
                if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
            });
            return kv.count("version") && kv["version"] == "1";
        }
        bool Save(const std::wstring& path) const
        {
            CreateDirectoryW(path.substr(0, path.find_last_of(L'\\')).c_str(), nullptr);
            std::string text = "version=1\n";
            for (const auto& [k, v] : kv) if (k != "version") text += k + "=" + v + "\n";
            const std::wstring tmp = path + L".tmp";
            HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (f == INVALID_HANDLE_VALUE) return false;
            DWORD wrote = 0;
            const bool ok = WriteFile(f, text.data(), (DWORD)text.size(), &wrote, nullptr) && wrote == text.size();
            CloseHandle(f);
            if (ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
            DeleteFileW(tmp.c_str());
            return false;
        }
    };

    // Written by BuildPlusLinkerRT.dll / BuildPlusCompiler.dll at exit: "v1", then "r <path>" read, "m <path>" looked for
    // but missing, "w <path>" written, lower case UTF-8, and "end" last (no "end" = the list was cut short).
    // full path without "..", backslashes, no doubled separators, lower case (relative paths resolve against our cwd, the child's)
    inline std::wstring NormalPath(std::wstring p)
    {
        if (p.empty()) return p;
        for (wchar_t& c : p) if (c == L'/') c = L'\\';
        std::wstring out;
        for (size_t i = 0; i < p.size(); ++i)
            if (!(p[i] == L'\\' && i > 1 && p[i - 1] == L'\\')) out += p[i];
        std::vector<wchar_t> buf(32768);
        const DWORD n = GetFullPathNameW(out.c_str(), (DWORD)buf.size(), buf.data(), nullptr);
        if (n && n < buf.size()) out.assign(buf.data(), n);
        return LowerW(out);
    }

    struct Journal
    {
        std::vector<std::wstring> reads, missing, writes;
        bool Load(const std::wstring& path)
        {
            std::string text;
            if (!ReadAll(path, text)) return false;
            bool header = false, end = false;
            Lines(text, [&](const std::string& line) {
                if (line == "v1") { header = true; return; }
                if (line == "end") { end = true; return; }
                if (line.size() < 3 || line[1] != ' ') return;
                std::wstring p = NormalPath(FromUtf8(line.substr(2)));
                if (p.empty()) return;
                if (line[0] == 'r') reads.push_back(std::move(p));
                else if (line[0] == 'm') missing.push_back(std::move(p));
                else if (line[0] == 'w') writes.push_back(std::move(p));
            });
            return header && end;
        }
    };

    // The lowest i in [0, n) for which changed(i) is true, n when none: checked on up to 16 threads.
    template <class F> inline size_t FirstChanged(size_t n, F&& changed)
    {
        if (!n) return 0;
        std::atomic<size_t> next{0}, found{n};
        auto work = [&]() {
            for (;;)
            {
                const size_t i = next.fetch_add(1);
                if (i >= n || i > found.load()) return;
                if (changed(i))
                {
                    size_t cur = found.load();
                    while (i < cur && !found.compare_exchange_weak(cur, i)) {}
                }
            }
        };
        SYSTEM_INFO info = {};
        GetSystemInfo(&info);
        size_t threads = info.dwNumberOfProcessors < 2 ? 1 : info.dwNumberOfProcessors > 16 ? 16 : info.dwNumberOfProcessors;
        if (threads > n / 64 + 1) threads = n / 64 + 1;
        std::vector<std::thread> pool;
        try { for (size_t t = 1; t < threads; ++t) pool.emplace_back(work); } catch (...) {}
        work();
        for (std::thread& t : pool) t.join();
        return found.load();
    }

    // Files under dir (relative path, size and write time) into sha, sorted. skip: lower-case relative folders left out.
    inline size_t HashTree(Sha256& sha, const std::wstring& dir, const std::vector<std::wstring>& skip,
                           std::vector<std::pair<std::wstring, std::string>>* listed = nullptr)
    {
        std::vector<std::pair<std::wstring, std::string>> files;
        std::vector<std::wstring> pending{ L"" };
        while (!pending.empty())
        {
            const std::wstring rel = pending.back();
            pending.pop_back();
            WIN32_FIND_DATAW fd = {};
            HANDLE h = FindFirstFileExW((dir + L"\\" + rel + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE) continue;
            do
            {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                const std::wstring name = rel + fd.cFileName;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    bool left = false;
                    for (const std::wstring& s : skip) if (LowerW(name) == s) left = true;
                    if (!left && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) pending.push_back(name + L"\\");
                    continue;
                }
                char id[64];
                snprintf(id, sizeof id, "%llu,%llu", ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow,
                         ((unsigned long long)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime);
                files.emplace_back(LowerW(name), id);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(files.begin(), files.end());
        sha.Add("tree " + Utf8(LowerW(dir)));
        for (const auto& [name, id] : files) sha.Add(Utf8(name) + " " + id);
        if (listed) listed->insert(listed->end(), files.begin(), files.end());
        return files.size();
    }

    // verify mode: how this run's reads differ from the recorded ones ("r" and "m" entries), keep = the journal filter
    template <class Keep> inline std::string CompareReads(const State& old, const Journal& now, Keep&& keep)
    {
        std::unordered_set<std::wstring> was, wasMissing, reads;
        for (const auto& [k, v] : old.kv)
        {
            if (k.size() < 2 || !isdigit((unsigned char)k[1])) continue;
            if (k[0] == 'r') was.insert(FromUtf8(v.substr(v.find('|') + 1)));
            else if (k[0] == 'm') wasMissing.insert(FromUtf8(v));
        }
        size_t added = 0, gone = 0, missingAdded = 0;
        std::wstring first;
        for (const std::wstring& r : now.reads)
        {
            if (!keep(r)) continue;
            reads.insert(r);
            if (!was.count(r)) { ++added; if (first.empty()) first = r; }
        }
        for (const std::wstring& r : was) if (!reads.count(r)) ++gone;
        for (const std::wstring& m : now.missing) if (keep(m) && !wasMissing.count(m)) ++missingAdded;
        char buf[200];
        snprintf(buf, sizeof buf, "%zu files read that the recorded run did not read, %zu no longer read, %zu new missing lookups", added, gone, missingAdded);
        std::string out = buf;
        if (!first.empty()) out += ", first new read " + Utf8(first);
        return out;
    }

    // Forwards a child's stdout and stderr to ours and keeps what a skip has to repeat: error lines and fatal markers.
    struct Tap
    {
        struct Stream { HANDLE read = nullptr, write = nullptr, target = nullptr; std::thread thread; std::string partial; };
        Stream out, err;
        std::atomic<bool> fatal{false};
        SRWLOCK lock = SRWLOCK_INIT;
        std::vector<std::string> errors;
        size_t errorLines = 0;
        bool active = false;
        static constexpr size_t kKept = 400;

        void Line(const std::string& line)
        {
            if (line.find("UNRECOVERABLE") != std::string::npos || line.find("failed to compile") != std::string::npos) fatal = true;
            if (line.find("ERROR") == std::string::npos && line.find(" error") == std::string::npos) return;
            AcquireSRWLockExclusive(&lock);
            ++errorLines;
            if (errors.size() < kKept) errors.push_back(line.size() > 600 ? line.substr(0, 600) : line);
            ReleaseSRWLockExclusive(&lock);
        }
        void Pump(Stream& s)
        {
            try
            {
                char buf[8192];
                DWORD got = 0;
                while (ReadFile(s.read, buf, sizeof buf, &got, nullptr) && got)
                {
                    DWORD wrote = 0;
                    if (s.target) WriteFile(s.target, buf, got, &wrote, nullptr);
                    s.partial.append(buf, got);
                    size_t nl;
                    while ((nl = s.partial.find('\n')) != std::string::npos)
                    {
                        std::string line = s.partial.substr(0, nl);
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        Line(line);
                        s.partial.erase(0, nl + 1);
                    }
                }
                if (!s.partial.empty()) Line(s.partial);
            }
            catch (...) { fatal = true; }
        }
        static bool Open(Stream& s, DWORD which)
        {
            s.target = GetStdHandle(which);
            if (s.target == INVALID_HANDLE_VALUE) s.target = nullptr;
            SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
            if (!CreatePipe(&s.read, &s.write, &sa, 1 << 16)) return false;
            SetHandleInformation(s.read, HANDLE_FLAG_INHERIT, 0);
            return true;
        }
        // before CreateProcess: the child gets out.write / err.write
        bool Start()
        {
            fflush(stdout); fflush(stderr);
            if (!Open(out, STD_OUTPUT_HANDLE) || !Open(err, STD_ERROR_HANDLE)) { Close(); return false; }
            try
            {
                out.thread = std::thread([this] { Pump(out); });
                err.thread = std::thread([this] { Pump(err); });
            }
            catch (...) { Close(); return false; }
            active = true;
            return true;
        }
        // after CreateProcess: our copies of the write ends, so the readers see the end of the stream when the child exits
        void ChildStarted()
        {
            if (out.write) { CloseHandle(out.write); out.write = nullptr; }
            if (err.write) { CloseHandle(err.write); err.write = nullptr; }
        }
        // after the child exited: false when a grandchild still holds the pipes (the output may be incomplete)
        bool Finish(DWORD waitMs = 10000)
        {
            ChildStarted();
            bool complete = true;
            for (Stream* s : { &out, &err })
            {
                if (!s->thread.joinable()) continue;
                HANDLE th = s->thread.native_handle();
                if (WaitForSingleObject(th, waitMs) == WAIT_OBJECT_0) s->thread.join();
                else { complete = false; s->thread.detach(); }
            }
            return complete;
        }
        void Close()
        {
            for (Stream* s : { &out, &err })
            {
                if (s->write) { CloseHandle(s->write); s->write = nullptr; }
                if (s->thread.joinable()) s->thread.detach();
            }
        }
        ~Tap() { Close(); }
    };
}
