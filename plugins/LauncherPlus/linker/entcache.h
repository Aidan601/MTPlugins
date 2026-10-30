// cache_map_entities: the linker parses the map source every link (Com_ParseMapEntities, linker+0x2776D0: map_source\<map>.map
// and every prefab through the map parser, then the entity string is rebuilt from the result). The string depends only on those
// files, so it is kept beside the skip states and reused while none of them changed.
#pragma once
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <unordered_set>
#include "../../../src/third_party/minhook/include/MinHook.h"

namespace entcache
{
    constexpr uintptr_t kParseRva = 0x2776D0;      // Com_ParseMapEntities(const char* bspName)
    constexpr uintptr_t kSearchRva = 0x276B80;     // search_dir(std::string dir, std::string* out[2]): finds <name>.map under map_source
    constexpr uintptr_t kDepRecordRva = 0x31CF20;  // dependency recorder (path, hash flag): the .deps "file," lines
    constexpr uintptr_t kAssignRva = 0x4DB0;       // std::string::assign(const char*, size_t), MSVC 2012
    constexpr uintptr_t kStringRva = 0xC80330;     // the global std::string the entity string is built into
    const unsigned char kParseSig[] = { 0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0xA8, 0xFE, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x30, 0x02 };
    const unsigned char kSearchSig[] = { 0x40, 0x55, 0x56, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0xC0, 0xFE, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x40 };
    const unsigned char kDepRecordSig[] = { 0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0x28 };
    const unsigned char kAssignSig[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x49 };

    // MSVC 2012 std::string: 16-byte buffer or heap pointer, size, capacity (15 = in the buffer)
    struct MsString
    {
        union { char buf[16]; char* ptr; };
        size_t size, res;
        const char* data() const { return res >= 16 ? ptr : buf; }
    };
    using ParseFn = __int64 (__fastcall*)(char*);
    using SearchFn = void (__fastcall*)(MsString*, MsString**);
    using DepRecordFn = void (__fastcall*)(char*, char);
    using AssignFn = void* (__fastcall*)(MsString*, const void*, size_t);
    using FreeFn = void (__cdecl*)(void*);
    inline ParseFn g_parse = nullptr;
    inline SearchFn g_search = nullptr;
    inline DepRecordFn g_depRecord = nullptr;
    inline AssignFn g_assign = nullptr;
    inline FreeFn g_delete = nullptr;   // msvcr110 operator delete, what the linker's strings free with
    inline unsigned char* g_base = nullptr;
    inline std::wstring g_stateDir;
    inline bool g_verify = false;
    inline volatile LONG g_verifyRuns = 0, g_verifyMismatches = 0;

    // read opens of the current thread while the stock parse runs (filled by the CreateFile hooks' Record): lower-case path, found
    inline thread_local std::vector<std::pair<std::wstring, bool>>* t_capture = nullptr;
    // the journal and prefetch list get the reused files as reads (set by the runtime DLL when those lists exist)
    inline void (*g_note)(const std::wstring& path, bool found) = nullptr;

    inline std::wstring Widen(const std::string& s)
    {
        if (s.empty()) return L"";
        const int n = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring out(n, L'\0');
        MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), &out[0], n);
        return out;
    }
    inline std::string Narrow(const std::wstring& w)
    {
        if (w.empty()) return "";
        const int n = WideCharToMultiByte(CP_ACP, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string out(n, '\0');
        WideCharToMultiByte(CP_ACP, 0, w.data(), (int)w.size(), &out[0], n, nullptr, nullptr);
        return out;
    }
    inline std::wstring LowerW(std::wstring s) { if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size()); return s; }

    inline std::string Identity(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA fa = {};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return "";
        char out[64];
        snprintf(out, sizeof out, "%llu,%llu", ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow,
                 ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime);
        return out;
    }
    inline bool ReadAll(const std::wstring& path, std::string& out)
    {
        const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size = {};
        bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (1ll << 31);
        if (ok)
        {
            out.resize((size_t)size.QuadPart);
            size_t done = 0;
            while (ok && done < out.size())
            {
                DWORD got = 0;
                ok = ReadFile(h, &out[done], (DWORD)(out.size() - done), &got, nullptr) && got;
                done += got;
            }
        }
        CloseHandle(h);
        return ok;
    }
    inline bool WriteAll(const std::wstring& path, const char* data, size_t size)
    {
        const std::wstring tmp = path + L".tmp";
        const HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        bool ok = true;
        for (size_t done = 0; ok && done < size;)
        {
            DWORD wrote = 0;
            ok = WriteFile(h, data + done, (DWORD)(size - done), &wrote, nullptr) && wrote;
            done += wrote;
        }
        CloseHandle(h);
        if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp.c_str()); return false; }
        return true;
    }

    // the bsp name the linker passes ("usermaps/<map>/<map>.d3dbsp" or a bare name): what the map file is searched by
    inline std::string BaseName(const char* src)
    {
        std::string s(src ? src : "");
        const size_t slash = s.find_last_of("/\\");
        if (slash != std::string::npos) s = s.substr(slash + 1);
        const size_t dot = s.find('.');
        if (dot != std::string::npos) s = s.substr(0, dot);
        return s;
    }
    inline std::string GameMapSource()
    {
        char game[MAX_PATH * 2] = {};
        if (!GetEnvironmentVariableA("TA_GAME_PATH", game, sizeof game)) return "";
        return std::string(game) + "\\map_source\\";
    }

    // what stock's search_dir finds for "<base>.map" now (the same walk, the same order, an exact name compare), empty when nothing is found
    inline std::wstring FoundNow(const std::string& dir, const std::string& base, bool& ok)
    {
        ok = false;
        if (dir.empty()) return L"";
        const std::string name = base + ".map";
        MsString dirStr = {}, nameStr = {}, foundFile = {};
        dirStr.res = nameStr.res = foundFile.res = 15;
        g_assign(&dirStr, dir.data(), dir.size());
        g_assign(&nameStr, name.data(), name.size());
        MsString* out[2] = { &nameStr, &foundFile };   // in: the file name to find, out: its full path
        g_search(&dirStr, out);   // destroys dirStr itself
        std::wstring found = LowerW(Widen(std::string(foundFile.data(), foundFile.size)));
        if (nameStr.res >= 16) g_delete(nameStr.ptr);
        if (foundFile.res >= 16) g_delete(foundFile.ptr);
        ok = true;
        return found;
    }

    struct State
    {
        std::map<std::string, std::string> kv;      // arg, game, found, size
        std::vector<std::pair<std::wstring, bool>> files;   // lower-case path, existed
        bool Load(const std::wstring& path)
        {
            std::string text;
            if (!ReadAll(path, text)) return false;
            size_t pos = 0;
            bool first = true;
            while (pos < text.size())
            {
                size_t end = text.find('\n', pos);
                if (end == std::string::npos) end = text.size();
                std::string line = text.substr(pos, end - pos);
                pos = end + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (first) { first = false; if (line != "v1") return false; continue; }
                if (line.size() > 2 && (line[0] == 'f' || line[0] == 'x') && line[1] == ' ')
                {
                    const size_t bar = line.rfind('|');
                    if (line[0] == 'f' && bar == std::string::npos) return false;
                    const std::string p = line.substr(2, line[0] == 'f' ? bar - 2 : std::string::npos);
                    files.push_back({ Widen(p), line[0] == 'f' });
                    if (line[0] == 'f') kv["id:" + p] = line.substr(bar + 1);
                    continue;
                }
                const size_t eq = line.find('=');
                if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
            }
            return kv.count("arg") && kv.count("found") && kv.count("size");
        }
    };

    inline std::wstring StatePath(const std::string& base, const wchar_t* ext) { return g_stateDir + L"\\" + Widen(base) + L"_ents" + ext; }

    // "" = the recorded string was loaded, else why the parse must run
    inline std::string TryReuse(char* src, const std::string& base, size_t& checked)
    {
        checked = 0;
        State st;
        if (!st.Load(StatePath(base, L".txt"))) return "first link recorded by BetterBuild";
        const std::string dir = GameMapSource();
        if (st.kv["arg"] != (src ? src : "") || st.kv["game"] != dir) return "its arguments changed";
        bool ok = false;
        const std::wstring found = FoundNow(dir, base, ok);
        if (!ok || found.empty() || Narrow(found) != st.kv["found"]) return "map_source\\" + base + ".map is not where it was";
        for (const auto& [path, existed] : st.files)
        {
            ++checked;
            const std::string id = Identity(path);
            if (existed && id != st.kv["id:" + Narrow(path)]) return id.empty() ? Narrow(path) + " is missing" : Narrow(path) + " changed";
            if (!existed && !id.empty()) return Narrow(path) + " appeared";
        }
        std::string text;
        if (!ReadAll(StatePath(base, L".bin"), text) || std::to_string(text.size()) != st.kv["size"]) return "the recorded entity string is missing";
        auto* str = reinterpret_cast<MsString*>(g_base + kStringRva);
        g_assign(str, text.data(), text.size());
        // the same side effects as the parse: the map files in the .deps, the journal and the prefetch list
        for (const auto& [path, existed] : st.files)
        {
            if (existed) { std::string p = Narrow(path); g_depRecord(&p[0], 0); }
            if (g_note) g_note(path, existed);
        }
        return "";
    }

    inline void Record(char* src, const std::string& base, const std::vector<std::pair<std::wstring, bool>>& captured)
    {
        const auto* str = reinterpret_cast<const MsString*>(g_base + kStringRva);
        if (!str->size || captured.empty()) { DeleteFileW(StatePath(base, L".txt").c_str()); return; }
        const std::wstring want = L"\\" + LowerW(Widen(base)) + L".map";
        std::wstring found;
        for (const auto& [path, existed] : captured)
            if (existed && path.size() > want.size() && !path.compare(path.size() - want.size(), want.size(), want)) { found = path; break; }
        if (found.empty()) { DeleteFileW(StatePath(base, L".txt").c_str()); return; }
        std::string text = "v1\narg=" + std::string(src ? src : "") + "\ngame=" + GameMapSource() + "\nfound=" + Narrow(found) +
                           "\nsize=" + std::to_string(str->size) + "\n";
        for (const auto& [path, existed] : captured)
        {
            if (existed)
            {
                const std::string id = Identity(path);
                if (id.empty()) { DeleteFileW(StatePath(base, L".txt").c_str()); return; }   // gone already: record nothing
                text += "f " + Narrow(path) + "|" + id + "\n";
            }
            else text += "x " + Narrow(path) + "\n";
        }
        CreateDirectoryW(g_stateDir.c_str(), nullptr);
        if (!WriteAll(StatePath(base, L".bin"), str->data(), str->size) || !WriteAll(StatePath(base, L".txt"), text.data(), text.size()))
            DeleteFileW(StatePath(base, L".txt").c_str());
    }

    inline __int64 __fastcall HookParse(char* src)
    {
        const ULONGLONG t0 = GetTickCount64();
        const std::string base = BaseName(src);
        size_t checked = 0;
        const std::string why = base.empty() ? "no map name" : TryReuse(src, base, checked);
        if (why.empty())
        {
            fprintf(stdout, "[BetterBuild] Map entities reused: none of the %zu map source files changed since the last link (checked in %llu ms; cache_map_entities=0 in LauncherPlus.ini always parses them).\n",
                    checked, GetTickCount64() - t0);
            fflush(stdout);
            if (!g_verify) return 0;
            // verify: the stock parse runs too and its string must equal the reused one (stock's is kept)
            const auto* str = reinterpret_cast<const MsString*>(g_base + kStringRva);
            const std::string reused(str->data(), str->size);
            const __int64 r = g_parse(src);
            InterlockedIncrement(&g_verifyRuns);
            if (reused.size() != str->size || memcmp(reused.data(), str->data(), str->size) != 0) InterlockedIncrement(&g_verifyMismatches);
            return r;
        }
        fprintf(stdout, "[BetterBuild] Map entities parsed: %s.\n", why.c_str());
        fflush(stdout);
        std::vector<std::pair<std::wstring, bool>> captured;
        t_capture = &captured;
        const __int64 r = g_parse(src);
        t_capture = nullptr;
        // a prefab is opened once per placement: keep the first open of each path
        std::vector<std::pair<std::wstring, bool>> unique;
        std::unordered_set<std::wstring> seen;
        for (auto& e : captured) if (seen.insert(e.first).second) unique.push_back(std::move(e));
        Record(src, base, unique);
        return r;
    }

    inline bool Install(unsigned char* base, const std::wstring& stateDir, bool verify)
    {
        g_base = base;
        g_stateDir = stateDir;
        g_verify = verify;
        if (memcmp(base + kParseRva, kParseSig, sizeof kParseSig) || memcmp(base + kSearchRva, kSearchSig, sizeof kSearchSig) ||
            memcmp(base + kDepRecordRva, kDepRecordSig, sizeof kDepRecordSig) || memcmp(base + kAssignRva, kAssignSig, sizeof kAssignSig))
            return false;
        HMODULE crt = GetModuleHandleW(L"msvcr110.dll");
        if (!crt) return false;
        g_delete = reinterpret_cast<FreeFn>(GetProcAddress(crt, "??3@YAXPEAX@Z"));
        if (!g_delete) return false;
        g_search = reinterpret_cast<SearchFn>(base + kSearchRva);
        g_depRecord = reinterpret_cast<DepRecordFn>(base + kDepRecordRva);
        g_assign = reinterpret_cast<AssignFn>(base + kAssignRva);
        return MH_CreateHook(base + kParseRva, reinterpret_cast<LPVOID>(&HookParse), reinterpret_cast<LPVOID*>(&g_parse)) == MH_OK &&
               MH_EnableHook(base + kParseRva) == MH_OK;
    }
}
