// Safety copies of the files Team Create rewrites. See backup.h for the why.
#include "compat.h"
#include "backup.h"
#include "presence.h"
#include <shlobj.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace
{
    std::mutex             g_mutex;
    // File state (size + write time) when last copied and when last written by us. A rewrite copies unless it still matches one.
    struct Known { uint64_t size = 0, mtime = 0; bool operator==(const Known& o) const { return size == o.size && mtime == o.mtime; } };
    std::map<std::wstring, Known> g_copied, g_written;   // lower-case path -> state
    std::set<std::wstring> g_allowed;   // lower-case paths the user allowed to be written without a copy
    std::vector<std::wstring> g_made;   // backup files written this session
    unsigned               g_pruned = 0;
    enum class Answer { Asking, Yes, No };
    std::map<std::wstring, Answer> g_answers;   // lower-case path -> answer after a failed copy

    std::wstring Lower(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }

    bool Stat(const std::wstring& path, Known& k)
    {
        WIN32_FILE_ATTRIBUTE_DATA d;
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d) || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
        k.size = (uint64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
        k.mtime = (uint64_t(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
        return true;
    }

    std::string Narrow(const std::wstring& w)
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n ? n - 1 : 0, '\0');
        if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
        return s;
    }

    // <root>\radiant\teamcreate_backup, root from the host, else the folder above map_source, else <PluginDir>\backup
    std::wstring DirFor(const std::wstring& mapPath)
    {
        std::wstring root;
        if (rp::host->RadiantRoot) { const wchar_t* r = rp::host->RadiantRoot(rp::self); if (r && *r) root = r; }
        if (root.empty())
        {
            std::wstring lower = Lower(mapPath);
            size_t at = lower.find(L"\\map_source\\");
            if (at != std::wstring::npos) root = mapPath.substr(0, at);
        }
        while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
        if (root.empty()) return rp::PluginDir() + L"\\backup";
        return root + L"\\radiant\\teamcreate_backup";
    }

    std::wstring Stem(const std::wstring& path)
    {
        size_t slash = path.find_last_of(L"\\/");
        std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
        size_t dot = name.find_last_of(L'.');
        return dot == std::wstring::npos ? name : name.substr(0, dot);
    }

    std::wstring Stamp()
    {
        SYSTEMTIME t; GetLocalTime(&t);
        wchar_t buf[32];
        swprintf_s(buf, L"%04u_%02u_%02u_%02u_%02u_%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        return buf;
    }

    // "<stem>_YYYY_MM_DD_HH_MM_SS.map" only: the only files ever deleted
    bool IsOurs(const std::wstring& name, const std::wstring& stem)
    {
        const std::wstring lname = Lower(name), lstem = Lower(stem);
        if (lname.size() != lstem.size() + 1 + 19 + 4) return false;
        if (lname.compare(0, lstem.size(), lstem) != 0 || lname[lstem.size()] != L'_') return false;
        if (lname.compare(lname.size() - 4, 4, L".map") != 0) return false;
        const wchar_t* s = lname.c_str() + lstem.size() + 1;    // 19 chars: dddd_dd_dd_dd_dd_dd
        for (int i = 0; i < 19; ++i)
        {
            bool sep = (i == 4 || i == 7 || i == 10 || i == 13 || i == 16);
            if (sep ? s[i] != L'_' : !iswdigit(s[i])) return false;
        }
        return true;
    }

    // Keep the newest `keep` copies of this map (the stamp sorts by time)
    void Prune(const std::wstring& dir, const std::wstring& stem, unsigned keep)
    {
        if (keep == 0) return;
        std::vector<std::wstring> names;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\" + stem + L"_*.map").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (IsOurs(fd.cFileName, stem)) names.push_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        if (names.size() <= keep) return;
        std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return Lower(a) < Lower(b); });
        size_t drop = names.size() - keep;
        for (size_t i = 0; i < drop; ++i)
        {
            std::wstring p = dir + L"\\" + names[i];
            if (DeleteFileW(p.c_str())) { ++g_pruned; if (Cfg().verbose) LOG("backup: pruned %S", p.c_str()); }
            else LOG("backup: could not delete %S (error %lu)", p.c_str(), GetLastError());
        }
        LOG("backup: %zu older cop%s of %S removed, %u kept", drop, drop == 1 ? "y" : "ies", stem.c_str(), keep);
    }
}

namespace backup
{
    void NewSession()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_copied.clear(); g_written.clear(); g_allowed.clear(); g_made.clear(); g_pruned = 0; g_answers.clear();
    }

    void NoteWritten(const std::wstring& path)
    {
        if (path.empty()) return;
        Known k; if (!Stat(path, k)) return;
        std::lock_guard<std::mutex> lock(g_mutex); g_written[Lower(path)] = k;
    }

    std::wstring Dir()
    {
        std::wstring map = hooks::CurrentMapPath();
        return map.empty() ? std::wstring() : DirFor(map);
    }
}

namespace
{
    enum class CopyResult { Copied, Missing, Failed };

    // Copy `path` into the backup folder, `err` describes a failure
    CopyResult Copy(const std::wstring& path, std::wstring& err)
    {
        DWORD attr = GetFileAttributesW(path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY))
        {
            if (Cfg().verbose) LOG("backup: %S does not exist yet, nothing to copy", path.c_str());
            return CopyResult::Missing;
        }
        std::wstring dir = DirFor(path), stem = Stem(path);
        if (SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr) != ERROR_SUCCESS && GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS)
        {
            DWORD e = GetLastError();
            LOG("backup: WARNING cannot create %S (error %lu)", dir.c_str(), e);
            err = L"The folder " + dir + L" could not be created (error " + std::to_wstring(e) + L").";
            return CopyResult::Failed;
        }
        std::wstring dst = dir + L"\\" + stem + L"_" + Stamp() + L".map";
        if (!CopyFileW(path.c_str(), dst.c_str(), TRUE))
        {
            DWORD e = GetLastError();
            if (e == ERROR_FILE_EXISTS) { LOG("backup: %S already exists (same second), kept", dst.c_str()); }
            else
            {
                LOG("backup: WARNING copy of %S to %S failed (error %lu)", path.c_str(), dst.c_str(), e);
                err = L"Copying it to " + dst + L" failed (error " + std::to_wstring(e) + L").";
                return CopyResult::Failed;
            }
        }
        else LOG("backup: %S copied to %S", path.c_str(), dst.c_str());
        { std::lock_guard<std::mutex> lock(g_mutex); g_made.push_back(dst); }
        Prune(dir, stem, Cfg().backupKeep);
        return CopyResult::Copied;
    }

    std::wstring Question(const std::wstring& path, const std::wstring& err)
    {
        return L"Team Create is about to overwrite\n" + path + L"\nwith the other mappers' changes, but could not back it up first.\n\n" + err +
               L"\n\nYes: overwrite it anyway, without a backup.\nNo: leave the file as it is for this session. Changes from the others that need it "
               L"are not applied here, so your copy will differ from theirs until you rejoin.";
    }

    void Record(const std::wstring& key, const std::wstring& path, bool yes)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_answers[key] = yes ? Answer::Yes : Answer::No;
        if (yes) g_allowed.insert(key);
        LOG("backup: no backup of %S; the user chose %s", path.c_str(), yes ? "to overwrite it anyway" : "to leave it alone this session");
    }

    // Shared start of Snapshot/BeforeWrite/BeforeWriteLater: Go/Skip when settled, Wait when the copy failed
    // (`err` set) or a posted question is still open (`err` empty)
    backup::Gate Try(const std::wstring& path, std::wstring& err)
    {
        if (!Cfg().backupEnabled || path.empty()) return backup::Gate::Go;
        const std::wstring key = Lower(path);
        Known now; const bool exists = Stat(path, now);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_allowed.count(key)) return backup::Gate::Go;  // user chose to write without a copy
            auto it = g_answers.find(key);
            if (it != g_answers.end()) return it->second == Answer::No ? backup::Gate::Skip : backup::Gate::Wait;
            if (exists)
            {
                auto c = g_copied.find(key), w = g_written.find(key);
                if ((c != g_copied.end() && c->second == now) || (w != g_written.end() && w->second == now))
                {
                    if (Cfg().verbose) LOG("backup: %S unchanged since %s, no new copy", path.c_str(), c != g_copied.end() && c->second == now ? "the last copy" : "Team Create wrote it");
                    return backup::Gate::Go;
                }
            }
        }
        CopyResult r = Copy(path, err);
        if (r == CopyResult::Failed) return backup::Gate::Wait;
        if (r == CopyResult::Copied) { std::lock_guard<std::mutex> lock(g_mutex); g_copied[key] = now; }
        return backup::Gate::Go;
    }
}

namespace backup
{
    void Snapshot(const std::wstring& path)
    {
        std::wstring err;
        Gate g = Try(path, err);
        if (g == Gate::Wait && !err.empty())    // no answer recorded: the first rewrite tries again and asks
            presence::Notice("Could not back up your map before the session: " + Narrow(err) + " Your saves overwrite it without a copy.");
    }

    bool BeforeWrite(const std::wstring& path)
    {
        std::wstring err;
        Gate g = Try(path, err);
        if (g != Gate::Wait) return g == Gate::Go;
        if (err.empty()) return false;                          // a posted question for this file is still open
        int r = MessageBoxW(mt::MainWindow(), Question(path, err).c_str(), L"Team Create", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND);
        Record(Lower(path), path, r == IDYES);
        return r == IDYES;
    }

    Gate BeforeWriteLater(const std::wstring& path)
    {
        std::wstring err;
        Gate g = Try(path, err);
        if (g != Gate::Wait || err.empty()) return g;
        const std::wstring key = Lower(path);
        { std::lock_guard<std::mutex> lock(g_mutex); g_answers[key] = Answer::Asking; }
        mt::Post([key, path, err] {
            int r = MessageBoxW(mt::MainWindow(), Question(path, err).c_str(), L"Team Create", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND);
            bool stale;
            { std::lock_guard<std::mutex> lock(g_mutex); auto it = g_answers.find(key); stale = it == g_answers.end() || it->second != Answer::Asking; }
            if (stale) { LOG("backup: answer for %S dropped (a new session started)", path.c_str()); return; }
            Record(key, path, r == IDYES);
        });
        return Gate::Wait;
    }

    std::string Status()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::string s = "backup: " + std::string(Cfg().backupEnabled ? "enabled" : "disabled") + ", keep " + std::to_string(Cfg().backupKeep) + " per map (0 = all)";
        std::wstring dir = Dir();
        if (!dir.empty()) s += "\nfolder: " + Narrow(dir);
        s += "\nasset backups: %LOCALAPPDATA%\\RadiantPlugins\\TeamCreate\\asset_backups, " +
             (Cfg().assetBackupDays ? "sessions older than " + std::to_string(Cfg().assetBackupDays) + " days removed, the newest " + std::to_string(Cfg().assetBackupKeep) + " kept" : std::string("never removed"));
        s += "\nthis session: " + std::to_string(g_made.size()) + " file(s) copied, " + std::to_string(g_pruned) + " old cop" + (g_pruned == 1 ? "y" : "ies") + " removed";
        for (const auto& m : g_made) s += "\n  " + Narrow(m);
        for (const auto& a : g_answers)
            s += "\n  no backup of " + Narrow(a.first) + ": " + (a.second == Answer::Yes ? "overwritten anyway" : a.second == Answer::No ? "left alone this session" : "asking");
        return s;
    }
}
