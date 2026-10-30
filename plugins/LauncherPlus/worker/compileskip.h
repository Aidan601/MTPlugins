// skip_unchanged for compiles: cod2map is not run when the map, its prefabs, everything the last compile read and its
// outputs are as the last compile left them. State: compile_state\<output hash>_full.txt.
#pragma once
#include "smartents.h"
#include "unchanged.h"

namespace compileskip
{
    struct Job
    {
        bool applicable = false;
        std::string reason;   // why the check does not apply
        std::wstring output, gdtDb, statePath, journal, pluginDirLower, tempLower;
        std::vector<std::wstring> outputs;   // the BSP and the two nav files
        std::string key;
    };

    inline std::wstring Parent(const std::wstring& p) { return p.substr(0, p.find_last_of(L"\\/")); }

    inline Job Prepare(const std::wstring& real, const std::wstring& pluginDir, const smartents::Plan& plan)
    {
        Job job;
        if (plan.callerOnlyEnts) { job.reason = "the caller asked for -onlyents"; return job; }
        if (plan.output.empty() || plan.fullSignature.empty()) { job.reason = plan.reason; return job; }
        job.output = plan.output;
        job.outputs = { plan.output, plan.navmesh, plan.navvolume };
        job.gdtDb = Parent(Parent(real)) + L"\\gdtdb\\gdt.db";
        const std::wstring base = plan.statePath.substr(0, plan.statePath.size() - 4);   // compile_state\<hash>
        job.statePath = base + L"_full.txt";
        job.journal = base + L"_reads.txt";
        job.pluginDirLower = unchanged::NormalPath(pluginDir) + L"\\";
        wchar_t temp[MAX_PATH + 1];
        const DWORD n = GetTempPathW(MAX_PATH + 1, temp);
        if (n && n <= MAX_PATH) job.tempLower = unchanged::NormalPath(temp);
        unchanged::Sha256 sha;
        sha.Add("buildplus compile skip v1");
        sha.Add("map " + plan.fullSignature);
        std::vector<wchar_t> self(32768);
        GetModuleFileNameW(nullptr, self.data(), (DWORD)self.size());
        for (const std::wstring& f : { real, pluginDir + L"\\BuildPlusCompiler.dll", std::wstring(self.data()) })
            sha.Add("bin " + unchanged::Utf8(unchanged::LowerW(f)) + " " + unchanged::Identity(f));
        job.key = sha.Hex();
        if (job.key.empty()) { job.reason = "SHA-256 unavailable"; return job; }
        job.applicable = true;
        return job;
    }

    // left out of the journal: our own files, temp files, the physics primitive copy
    inline bool Filtered(const Job& job, const std::wstring& p)
    {
        using unchanged::StartsWith;
        using unchanged::EndsWith;
        return StartsWith(p, job.pluginDirLower) || (!job.tempLower.empty() && StartsWith(p, job.tempLower)) ||
               EndsWith(p, L".tmp") || EndsWith(p, L".db") || p.find(L"\\__buildplus_primitives\\") != std::wstring::npos;
    }

    // "" when the compile can be skipped, else why it runs
    inline std::string Decide(const Job& job, size_t& checked)
    {
        checked = 0;
        unchanged::State st;
        if (!st.Load(job.statePath)) return "first compile of this map recorded by BetterBuild";
        if (st.kv["key"] != job.key) return "the map, a prefab, the compiler options or BetterBuild changed";
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
        if (outs.size() < job.outputs.size()) return "no recorded output";
        for (const auto& [path, id] : outs)
            if (unchanged::Identity(path) != id) return unchanged::Utf8(path) + " was rewritten or removed after the last compile";
        checked += outs.size();
        size_t at = unchanged::FirstChanged(reads.size(), [&](size_t i) { return unchanged::Identity(reads[i].first) != reads[i].second; });
        if (at < reads.size()) return unchanged::Utf8(reads[at].first) + " changed";
        checked += reads.size();
        at = unchanged::FirstChanged(missing.size(), [&](size_t i) { return GetFileAttributesW(missing[i].c_str()) != INVALID_FILE_ATTRIBUTES; });
        if (at < missing.size()) return unchanged::Utf8(missing[at]) + " was added (the last compile looked for it)";
        checked += missing.size();
        return "";
    }

    inline unsigned long long Now()
    {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        return ((unsigned long long)now.dwHighDateTime << 32) | now.dwLowDateTime;
    }

    // identity, or a value no later check matches when the file was written after the compile started
    inline std::string IdentityBefore(const std::wstring& path, unsigned long long start)
    {
        const std::string id = unchanged::Identity(path);
        if (id.empty()) return id;
        return _strtoui64(id.c_str() + id.find(',') + 1, nullptr, 10) >= start ? "changed during the compile" : id;
    }

    // After a successful compile (exit 0, a new BSP): record what it read and wrote, else forget the state.
    inline std::string Remember(const Job& job, const std::string& bspBefore, unsigned long long start, int code)
    {
        auto forget = [&](const std::string& why) { DeleteFileW(job.statePath.c_str()); return why; };
        if (code != 0) return forget("the compile failed");
        const std::string bsp = unchanged::Identity(job.output);
        if (bsp.empty() || bsp == bspBefore) return forget("no new BSP");
        unchanged::Journal journal;
        if (!journal.Load(job.journal)) return forget("no complete read journal");
        unchanged::State st;
        st.kv["key"] = job.key;
        st.kv["gdt"] = IdentityBefore(job.gdtDb, start);
        char k[16];
        size_t n = 0;
        auto put = [&](char kind, const std::string& value) { snprintf(k, sizeof k, "%c%06zu", kind, n++); st.kv[k] = value; };
        std::unordered_set<std::wstring> outs;
        for (const std::wstring& o : job.outputs)
        {
            const std::string id = unchanged::Identity(o);
            if (id.empty()) return forget("the nav mesh files are missing");
            outs.insert(unchanged::NormalPath(o));
            put('o', id + "|" + unchanged::Utf8(o));
        }
        // outputs beside the BSP with its name (.d3dprt, _metadata.json, ...), not shared files other compiles rewrite
        const std::wstring stem = unchanged::NormalPath(smartents::StripExt(job.output));
        for (const std::wstring& w : journal.writes)
        {
            if (Filtered(job, w) || !unchanged::StartsWith(w, stem) || !outs.insert(w).second) continue;
            const std::string id = unchanged::Identity(w);
            if (!id.empty()) put('o', id + "|" + unchanged::Utf8(w));
        }
        // a file read and rewritten during the compile is one of its outputs, left out (the map and prefabs are in the key)
        for (const std::wstring& r : journal.reads)
        {
            if (Filtered(job, r)) continue;
            const std::string id = IdentityBefore(r, start);
            if (id != "changed during the compile") put('r', id + "|" + unchanged::Utf8(r));
        }
        for (const std::wstring& m : journal.missing)
            if (!Filtered(job, m) && GetFileAttributesW(m.c_str()) == INVALID_FILE_ATTRIBUTES) put('m', unchanged::Utf8(m));
        if (!st.Save(job.statePath)) return forget("the state could not be written");
        return "";
    }

    inline std::string Compare(const Job& job, const unchanged::State& old, unsigned long long start)
    {
        unchanged::Journal journal;
        if (!journal.Load(job.journal)) return "no journal to compare";
        return unchanged::CompareReads(old, journal, [&](const std::wstring& p) {
            return !Filtered(job, p) && IdentityBefore(p, start) != "changed during the compile";
        });
    }
}
