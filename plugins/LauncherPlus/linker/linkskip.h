// skip_unchanged for links: the whole link is skipped when nothing it reads changed since the last link BetterBuild recorded.
// Inputs: the linker's .deps, the read journal of BuildPlusLinkerRT.dll, the map folder and share\raw\scripts, gdt.db,
// the localized and sound states. State: linker_state\<map>_<language>_link.txt.
#pragma once
#define LOCALIZED_DECIDE_ONLY
#define SOUNDSKIP_DECIDE_ONLY
#include "localized.h"
#include "soundskip.h"
#include "../worker/unchanged.h"

namespace linkskip
{
    constexpr unsigned kVerifyBit = 1u << 8, kSkipLocalizedBit = 1u << 9, kSkipSoundBit = 1u << 10, kJournalBit = 1u << 14;

    struct Job
    {
        bool applicable = false;
        std::string reason;   // why the check does not apply
        unsigned mode = 0;
        std::wstring real, root, map, language, mapDir, deps, ff, xpak, gdtDb, stateDir, statePath, journal;
        std::wstring mapDirLower, pluginDirLower, tempLower;
        std::string key;
    };

    inline std::wstring Parent(const std::wstring& p) { return p.substr(0, p.find_last_of(L"\\/")); }

    inline Job Prepare(const std::wstring& real, const std::vector<std::wstring>& args, const std::wstring& pluginDir, unsigned mode)
    {
        Job job;
        job.real = real;
        job.mode = mode;
        for (size_t i = 0; i < args.size(); ++i)
        {
            if (!_wcsicmp(args[i].c_str(), L"-spawnedchild")) { job.reason = "a spawned child link"; return job; }
            if (!_wcsicmp(args[i].c_str(), L"-language") && i + 1 < args.size()) job.language = args[++i];
            else if (!_wcsicmp(args[i].c_str(), L"-modsource"))
            {
                size_t k = i + 1;
                while (k < args.size() && args[k][0] == L'-') ++k;
                if (k < args.size()) { job.map = args[k]; i = k; }
            }
        }
        if (job.map.empty() || job.language.empty() || job.map.find_first_of(L"\\/:") != std::wstring::npos)
        {
            job.reason = "not a -language <language> -modsource <map> link";
            return job;
        }
        job.root = Parent(Parent(real));
        job.mapDir = job.root + L"\\usermaps\\" + job.map;
        if (unchanged::Identity(job.mapDir + L"\\zone_source\\" + job.map + L".zone").empty()) { job.reason = "not a usermap"; return job; }
        if (!(mode & kSkipLocalizedBit)) { job.reason = "skip_localized=0"; return job; }
        if (!(mode & kSkipSoundBit)) { job.reason = "skip_sound_convert=0"; return job; }
        if (!localized::g_stat)
        {
            if (HMODULE crt = LoadLibraryW(L"msvcr110.dll"))
                localized::g_stat = reinterpret_cast<localized::StatFn>(GetProcAddress(crt, "_stat64i32"));
            if (!localized::g_stat) { job.reason = "msvcr110.dll not found"; return job; }
        }
        job.deps = job.mapDir + L"\\zone_source\\all\\assetinfo\\" + job.map + L".deps";
        job.ff = job.mapDir + L"\\zone\\" + job.map + L".ff";
        job.xpak = job.mapDir + L"\\zone\\" + job.map + L".xpak";
        job.gdtDb = job.root + L"\\gdtdb\\gdt.db";
        job.stateDir = pluginDir + L"\\linker_state";
        job.statePath = job.stateDir + L"\\" + job.map + L"_" + job.language + L"_link.txt";
        job.journal = job.stateDir + L"\\" + job.map + L"_" + job.language + L"_reads.txt";
        job.mapDirLower = unchanged::NormalPath(job.mapDir) + L"\\";
        job.pluginDirLower = unchanged::NormalPath(pluginDir) + L"\\";
        wchar_t temp[MAX_PATH + 1];
        const DWORD n = GetTempPathW(MAX_PATH + 1, temp);
        if (n && n <= MAX_PATH) job.tempLower = unchanged::NormalPath(temp);
        // anything that changes what the linker or BetterBuild does: the arguments, the binaries, the patch bits
        unchanged::Sha256 sha;
        sha.Add("buildplus link skip v1");
        for (const std::wstring& a : args) sha.Add("arg " + unchanged::Utf8(unchanged::LowerW(a)));
        std::vector<wchar_t> self(32768);
        GetModuleFileNameW(nullptr, self.data(), (DWORD)self.size());
        for (const std::wstring& f : { real, job.root + L"\\bin\\libtiff64r.dll", pluginDir + L"\\BuildPlusLinkerRT.dll", std::wstring(self.data()) })
            sha.Add("bin " + unchanged::Utf8(unchanged::LowerW(f)) + " " + unchanged::Identity(f));
        sha.Add("mode " + std::to_string(mode & ~(kVerifyBit | kJournalBit)));
        job.key = sha.Hex();
        if (job.key.empty()) { job.reason = "SHA-256 unavailable"; return job; }
        job.applicable = true;
        return job;
    }

    // what the link reads besides its .deps and journal: the map's own folder (scripts, gsh, lua, localized strings) and
    // share\raw\scripts. Output folders are left out. A file written at or after notAfter poisons the signature.
    inline std::string TreeSignature(const Job& job, size_t& files, unsigned long long notAfter = 0)
    {
        unchanged::Sha256 sha;
        sha.Add("buildplus link tree v2");
        // outputs: zone\, sound\zone (soundskip's), zone_source\<dir> (the linker's lists), <language>\sound\zone (the localized child's .sz)
        std::vector<std::wstring> skip{ L"zone", L"sound\\zone" };
        const auto subdirs = [](const std::wstring& dir, const auto& each) {
            WIN32_FIND_DATAW fd = {};
            HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) return;
            do
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L".."))
                    each(std::wstring(fd.cFileName));
            while (FindNextFileW(h, &fd));
            FindClose(h);
        };
        subdirs(job.mapDir + L"\\zone_source", [&](const std::wstring& d) { skip.push_back(unchanged::LowerW(L"zone_source\\" + d)); });
        subdirs(job.mapDir, [&](const std::wstring& d) { if (_wcsicmp(d.c_str(), L"zone") && _wcsicmp(d.c_str(), L"zone_source") && _wcsicmp(d.c_str(), L"sound")) skip.push_back(unchanged::LowerW(d + L"\\sound\\zone")); });
        std::vector<std::pair<std::wstring, std::string>> listed;
        files = unchanged::HashTree(sha, job.mapDir, skip, notAfter ? &listed : nullptr);
        files += unchanged::HashTree(sha, job.root + L"\\share\\raw\\scripts", {}, notAfter ? &listed : nullptr);
        for (const auto& [name, id] : listed)
            if (_strtoui64(id.c_str() + id.find(',') + 1, nullptr, 10) >= notAfter) return "changed during the link";
        return sha.Hex();
    }

    inline bool Filtered(const Job& job, const std::wstring& p)
    {
        using unchanged::StartsWith;
        using unchanged::EndsWith;
        return StartsWith(p, job.pluginDirLower) || (!job.tempLower.empty() && StartsWith(p, job.tempLower)) ||
               EndsWith(p, L".tmp") || EndsWith(p, L".db") || EndsWith(p, L".partial");
    }

    // "" when the link can be skipped, else why it runs (checked: how many files were compared)
    inline std::string Decide(const Job& job, size_t& checked)
    {
        checked = 0;
        unchanged::State st;
        if (!st.Load(job.statePath)) return "first link of this map recorded by BetterBuild";
        if (st.kv["key"] != job.key) return "the linker, BetterBuild or the link options changed";
        if (unchanged::Identity(job.deps) != st.kv["deps"]) return "another link rewrote the dependency list";
        if (unchanged::Identity(job.gdtDb) != st.kv["gdt"]) return "gdt.db changed (an asset was edited or a GDT was updated)";
        std::vector<std::pair<std::wstring, std::string>> outs, reads;
        std::vector<std::wstring> missing;
        for (const auto& [k, v] : st.kv)
        {
            if (k.size() < 2 || !isdigit((unsigned char)k[1])) continue;
            const size_t bar = v.find('|');
            if (k[0] == 'm') missing.push_back(unchanged::FromUtf8(v));
            else if (bar != std::string::npos && (k[0] == 'o' || k[0] == 'r'))
                (k[0] == 'o' ? outs : reads).emplace_back(unchanged::FromUtf8(v.substr(bar + 1)), v.substr(0, bar));
        }
        if (outs.empty()) return "no recorded fastfile";
        for (const auto& [path, id] : outs)
            if (unchanged::Identity(path) != id) return unchanged::Utf8(path) + " was rewritten or removed after the last link";
        checked += outs.size();

        localized::Job loc;
        loc.map = localized::Narrow(job.map);
        loc.language = localized::Narrow(job.language);
        loc.root = job.root;
        loc.deps = job.mapDir + L"\\zone_source\\" + job.language + L"\\assetinfo\\" + job.map + L".deps";
        loc.zoneDir = job.mapDir + L"\\zone";
        loc.mapDirLower = job.mapDirLower;
        loc.gdtDb = job.gdtDb;
        loc.statePath = job.stateDir + L"\\" + job.map + L"_" + job.language + L".txt";
        size_t locChecked = 0;
        const std::string locWhy = localized::Decide(loc, locChecked);
        if (!locWhy.empty()) return "the localized fastfile needs a link (" + locWhy + ")";
        checked += locChecked;

        const localized::Deps deps = localized::ParseDeps(job.deps);
        if (!deps.ok) return "no usable dependency list";
        size_t at = unchanged::FirstChanged(deps.files.size(), [&](size_t i) {
            struct _stat64i32 s = {};
            return localized::g_stat(deps.files[i].path.c_str(), &s) != 0 || (long long)s.st_mtime != deps.files[i].mtime;
        });
        if (at < deps.files.size()) return deps.files[at].path + " changed";
        checked += deps.files.size();
        at = unchanged::FirstChanged(reads.size(), [&](size_t i) { return unchanged::Identity(reads[i].first) != reads[i].second; });
        if (at < reads.size()) return unchanged::Utf8(reads[at].first) + " changed";
        checked += reads.size();
        at = unchanged::FirstChanged(missing.size(), [&](size_t i) { return GetFileAttributesW(missing[i].c_str()) != INVALID_FILE_ATTRIBUTES; });
        if (at < missing.size()) return unchanged::Utf8(missing[at]) + " was added (the last link looked for it)";
        checked += missing.size();
        size_t treeFiles = 0;
        if (TreeSignature(job, treeFiles) != st.kv["tree"]) return "a file in the map folder or share\\raw\\scripts changed";
        checked += treeFiles;

        soundskip::g_stateDir = job.stateDir;
        // the tool's language argument is "all" in every observed link, the link's own language is tried first
        soundskip::State snd;
        if (!snd.Load(job.stateDir + L"\\" + job.map + L"_snd_" + job.language + L".txt") &&
            !snd.Load(job.stateDir + L"\\" + job.map + L"_snd_all.txt"))
            return "no sound conversion recorded by BetterBuild yet";
        if (snd.kv["cmd"].empty()) return "no sound conversion recorded by BetterBuild yet";
        soundskip::Job sj;
        const std::string app = localized::Narrow(job.root + L"\\sound\\snd_convert.exe");
        if (!soundskip::Parse(app.c_str(), snd.kv["cmd"].c_str(), sj)) return "the recorded sound conversion is unreadable";
        size_t sndFiles = 0, sndOutputs = 0;
        const std::string sig = soundskip::Signature(sj, sndFiles);
        const std::string sndWhy = sig.empty() ? "SHA-256 unavailable" : soundskip::Decide(sj, sig, sndOutputs);
        if (!sndWhy.empty()) return "the sound conversion has to run (" + sndWhy + ")";
        checked += sndFiles;
        return "";
    }

    struct Before
    {
        std::string deps, ff;
        unsigned long long start = 0;   // FILETIME of the link start
    };

    inline Before Snapshot(const Job& job)
    {
        Before b;
        b.deps = unchanged::Identity(job.deps);
        b.ff = unchanged::Identity(job.ff);
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        b.start = ((unsigned long long)now.dwHighDateTime << 32) | now.dwLowDateTime;
        return b;
    }

    // identity, or a value no later check matches when the file was written after the link started
    inline std::string IdentityBefore(const std::wstring& path, unsigned long long start)
    {
        const std::string id = unchanged::Identity(path);
        if (id.empty()) return id;
        return _strtoui64(id.c_str() + id.find(',') + 1, nullptr, 10) >= start ? "changed during the link" : id;
    }

    // After the link: record what it read when it produced a fastfile without a fatal error, else forget the state.
    inline std::string Remember(const Job& job, const Before& before, const unchanged::Tap& tap, bool tapComplete, DWORD code)
    {
        auto forget = [&](const std::string& why) { DeleteFileW(job.statePath.c_str()); return why; };
        if (!tapComplete) return forget("the linker output did not end");
        if (tap.fatal || (code & 0x80000000u)) return forget("the link failed");
        const std::string ff = unchanged::Identity(job.ff);
        if (ff.empty() || ff == before.ff) return forget("no new fastfile");
        if (unchanged::Identity(job.deps) == before.deps) return forget("no new dependency list");
        unchanged::Journal journal;
        if (!journal.Load(job.journal)) return forget("no complete read journal");
        const localized::Deps deps = localized::ParseDeps(job.deps);
        if (!deps.ok) return forget("no usable dependency list");

        unchanged::State st;
        st.kv["key"] = job.key;
        st.kv["deps"] = unchanged::Identity(job.deps);
        st.kv["gdt"] = IdentityBefore(job.gdtDb, before.start);
        size_t files = 0;
        st.kv["tree"] = TreeSignature(job, files, before.start);
        st.kv["errorcount"] = std::to_string(deps.errorcount);
        st.kv["errorlines"] = std::to_string(tap.errorLines);
        char k[16];
        size_t n = 0;
        auto put = [&](char kind, const std::string& value) { snprintf(k, sizeof k, "%c%06zu", kind, n++); st.kv[k] = value; };
        put('o', ff + "|" + unchanged::Utf8(job.ff));
        if (!unchanged::Identity(job.xpak).empty()) put('o', unchanged::Identity(job.xpak) + "|" + unchanged::Utf8(job.xpak));
        std::unordered_set<std::wstring> outs{ unchanged::NormalPath(job.ff), unchanged::NormalPath(job.xpak) };
        for (const std::wstring& w : journal.writes)
        {
            if (!unchanged::StartsWith(w, job.mapDirLower) || Filtered(job, w) || !outs.insert(w).second) continue;
            const std::string id = unchanged::Identity(w);
            if (!id.empty()) put('o', id + "|" + unchanged::Utf8(w));
        }
        // a file read and rewritten during the link is one of its outputs (Umbra's c:\umbradebug scene, sound banks), left out.
        // Sources edited during the link are still caught by the .deps mtimes and the tree signature.
        for (const std::wstring& r : journal.reads)
        {
            if (Filtered(job, r)) continue;
            const std::string id = IdentityBefore(r, before.start);
            if (id != "changed during the link") put('r', id + "|" + unchanged::Utf8(r));
        }
        // a lookup that finds its file now was answered by the link itself (or its children) after the miss
        for (const std::wstring& m : journal.missing)
            if (!Filtered(job, m) && GetFileAttributesW(m.c_str()) == INVALID_FILE_ATTRIBUTES) put('m', unchanged::Utf8(m));
        for (const std::string& e : tap.errors) put('e', e);
        if (!st.Save(job.statePath)) return forget("the state could not be written");
        return "";
    }

    // the last real link's error lines, so a skipped link still shows them
    inline void Replay(const Job& job)
    {
        unchanged::State st;
        if (!st.Load(job.statePath)) return;
        const long long errors = _atoi64(st.kv["errorcount"].c_str());
        const long long lines = _atoi64(st.kv["errorlines"].c_str());
        if (errors <= 0 && lines <= 0) return;
        fprintf(stdout, "[BetterBuild] The last link of %s reported %lld error(s); its inputs are unchanged, so they still apply:\n",
                localized::Narrow(job.map).c_str(), errors > 0 ? errors : lines);
        long long shown = 0;
        for (const auto& [key, value] : st.kv)
            if (key.size() > 1 && key[0] == 'e' && isdigit((unsigned char)key[1])) { fprintf(stdout, "%s\n", value.c_str()); ++shown; }
        if (lines > shown) fprintf(stdout, "[BetterBuild] (%lld more error lines, see the last full link)\n", lines - shown);
        fflush(stdout);
    }

    // verify mode: after a link the check would have skipped, did it read anything the last one did not?
    inline std::string Compare(const Job& job, const unchanged::State& old, const Before& before)
    {
        unchanged::Journal journal;
        if (!journal.Load(job.journal)) return "no journal to compare";
        return unchanged::CompareReads(old, journal, [&](const std::wstring& p) {
            return !Filtered(job, p) && IdentityBefore(p, before.start) != "changed during the link";
        });
    }
}
