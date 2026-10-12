// browse.cpp: the plugin index and one-click install (see browse.h).

// MAP
//   Index source      IndexSource (ini [browse] index=, https URL or a local file for tests)
//   Download          HttpGet (WinHTTP, HTTPS only, size cap), ReadLocal
//   Hashing           Sha256Hex, InstalledHash (cached by size + write time)
//   Index parsing     ParseIndex, ValidName, ValidPath, ValidUrl, RepoFileUrl (files stored in the index repo)
//   Worker threads    StartWorker (pins the DLL, never lets an exception out), FetchIndex, DownloadAndVerify
//   Main thread       StatusOf, Updates, Install, InstallAll (queue), Tick -> FinishInstall (unload, move into place, rescan, load)
//   Uninstall         Uninstall, Tick -> FinishUninstall (unload, files deleted or moved to .mtplugins-removed, Forget)
//   Bundled plugins   LoadBundled (bundled.ini beside the kept Setup), StartSetup (/ADDPLUGIN), Tick -> FinishSetup
//   Official feed     UpdatesSource, FetchIndex (updates.json, only for installed bundled plugins), OfficialFor
//   Shared            browse::Fetch, Sha256, StartWorker, ParseVersion, Newer (update.cpp)
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <atomic>
#include <map>
#include <mutex>
#include "browse.h"
#include "plugins.h"
#include "config.h"
#include "log.h"
#include "version.h"
#include "../plugins/common/json.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

namespace
{
    const wchar_t* kDefaultIndex = L"https://raw.githubusercontent.com/Aidan601/MTPlugins-Index/main/index.json";
    const wchar_t* kDefaultUpdates = L"https://raw.githubusercontent.com/Aidan601/MTPlugins/main/updates.json";   // tools/publish/release_plugin.py
    const size_t kMaxIndexBytes = 4u << 20;
    const size_t kMaxFileBytes = 256u << 20;
    const wchar_t* kStaging = L".mtplugins-download";      // under the plugins folder (no DLL of that name: never a plugin)
    const wchar_t* kOldSuffix = L".mtp-old";                 // a file another editor still had mapped, deleted when free
    const wchar_t* kRemoved = L".mtplugins-removed";        // under the plugins folder: uninstalled files another editor still held

    std::mutex g_mu;                      // guards everything below that a worker touches
    std::vector<browse::Entry> g_entries;
    bool g_loaded = false;
    std::string g_message;
    std::atomic<bool> g_loading{ false };
    std::atomic<bool> g_installing{ false };
    std::atomic<unsigned> g_version{ 1 };
    bool g_localIndex = false;            // the index came from a file: file entries may be local paths too

    struct Staged { bool ready = false; browse::Entry entry; std::wstring dir; bool wasInstalled = false; };
    Staged g_staged;                      // a verified download waiting for the main thread

    // Bundled plugins: not in the index, Setup installs them. Ones left unticked are offered from the Setup kept beside the
    // uninstaller (bundled.ini lists them), Install runs it with /ADDPLUGIN so Setup stays the only thing that writes these files.
    const wchar_t* kSetupDir = L"uninstall";                 // under the plugins folder (Setup's UninstallFilesDir)
    const wchar_t* kSetupExe = L"MTPlugins-Setup.exe";
    const wchar_t* kBundledIni = L"bundled.ini";
    std::vector<browse::Entry> g_bundled; // every plugin the kept Setup holds, installed or not
    std::vector<browse::Entry> g_official;   // updates.json entries for bundled plugins (official = true)
    bool g_localUpdates = false;          // the feed came from a file: its file entries may be local paths
    HANDLE g_setupProcess = nullptr;      // the running Setup, main thread only
    browse::Entry g_setupEntry;

    std::vector<std::string> g_queue;     // InstallAll, main thread only
    std::string g_queueCurrent;           // the queued plugin being installed now
    std::vector<std::string> g_queueReport;

    std::wstring Widen(const std::string& s)
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n ? n - 1 : 0, L'\0');
        if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
        return w;
    }
    std::string Narrow(const std::wstring& w)
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n ? n - 1 : 0, '\0');
        if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
        return s;
    }
    void SetMessage(const std::string& m)
    {
        { std::lock_guard<std::mutex> lk(g_mu); g_message = m; }
        ++g_version;
        LOG("browse: %s", m.c_str());
    }
    bool StartsWithI(const std::wstring& s, const wchar_t* p) { return _wcsnicmp(s.c_str(), p, wcslen(p)) == 0; }

    std::wstring BrowseKey(const wchar_t* key, const wchar_t* def)
    {
        wchar_t buf[2048] = {};
        GetPrivateProfileStringW(L"browse", key, L"", buf, 2048, Cfg().iniPath.c_str());
        std::wstring s = buf;
        while (!s.empty() && iswspace(s.back())) s.pop_back();
        return s.empty() ? def : s;
    }
    std::wstring IndexSource() { return BrowseKey(L"index", kDefaultIndex); }
    std::wstring UpdatesSource() { return BrowseKey(L"updates", kDefaultUpdates); }

    // ---- download ---------------------------------------------------------------------------------------------
    struct Handle { HINTERNET h = nullptr; ~Handle() { if (h) WinHttpCloseHandle(h); } };

    bool HttpGet(const std::wstring& url, size_t cap, std::string& body, std::string& err)
    {
        body.clear();
        wchar_t host[256] = {}, path[4096] = {}, extra[4096] = {};
        URL_COMPONENTS uc{}; uc.dwStructSize = sizeof uc;
        uc.lpszHostName = host; uc.dwHostNameLength = 256;
        uc.lpszUrlPath = path; uc.dwUrlPathLength = 4096;
        uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 4096;
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { err = "bad URL"; return false; }
        if (uc.nScheme != INTERNET_SCHEME_HTTPS) { err = "only https:// downloads are allowed"; return false; }
        Handle s, c, r;
        s.h = WinHttpOpen(L"MTPlugins/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!s.h) { err = "WinHttpOpen failed (" + std::to_string(GetLastError()) + ")"; return false; }
        WinHttpSetTimeouts(s.h, 10000, 10000, 15000, 30000);
        c.h = WinHttpConnect(s.h, host, uc.nPort, 0);
        if (!c.h) { err = "cannot reach " + Narrow(host); return false; }
        std::wstring object = std::wstring(path) + extra;
        r.h = WinHttpOpenRequest(c.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!r.h) { err = "WinHttpOpenRequest failed (" + std::to_string(GetLastError()) + ")"; return false; }
        // redirects (release assets go to a CDN) are followed, https -> http is refused by WinHTTP's default policy
        if (!WinHttpSendRequest(r.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(r.h, nullptr))
        { err = "download failed from " + Narrow(host) + " (Windows error " + std::to_string(GetLastError()) + ")"; return false; }
        DWORD code = 0, len = sizeof code;
        WinHttpQueryHeaders(r.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
        if (code != 200) { err = "HTTP " + std::to_string(code) + " from " + Narrow(host); return false; }
        for (;;)
        {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(r.h, &avail)) { err = "download interrupted"; return false; }
            if (!avail) break;
            if (body.size() + avail > cap) { err = "file larger than allowed"; return false; }
            size_t at = body.size();
            body.resize(at + avail);
            DWORD got = 0;
            if (!WinHttpReadData(r.h, &body[at], avail, &got)) { err = "download interrupted"; return false; }
            body.resize(at + got);
        }
        return true;
    }

    bool ReadLocal(const std::wstring& path, size_t cap, std::string& body, std::string& err)
    {
        body.clear();
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) { err = "cannot open " + Narrow(path); return false; }
        LARGE_INTEGER size{};
        GetFileSizeEx(f, &size);
        if (size.QuadPart > LONGLONG(cap)) { CloseHandle(f); err = "file larger than allowed"; return false; }
        body.resize(size_t(size.QuadPart));
        DWORD got = 0;
        bool ok = body.empty() || (ReadFile(f, &body[0], DWORD(body.size()), &got, nullptr) && got == body.size());
        CloseHandle(f);
        if (!ok) err = "cannot read " + Narrow(path);
        return ok;
    }

    bool Fetch(const std::string& src, bool localAllowed, size_t cap, std::string& body, std::string& err)
    {
        std::wstring w = Widen(src);
        if (StartsWithI(w, L"https://")) return HttpGet(w, cap, body, err);
        if (!localAllowed) { err = "only https:// downloads are allowed"; return false; }
        return ReadLocal(w, cap, body, err);
    }

    // ---- hashing ----------------------------------------------------------------------------------------------
    std::string Sha256Hex(const void* data, size_t size)
    {
        unsigned char digest[32] = {};
        if (!BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)data, ULONG(size), digest, 32))) return "";
        static const char* hex = "0123456789abcdef";
        std::string out;
        for (unsigned char b : digest) { out += hex[b >> 4]; out += hex[b & 15]; }
        return out;
    }

    struct HashCache { ULONGLONG size = 0; FILETIME written{}; std::string hash; };
    std::map<std::wstring, HashCache> g_hashes;      // main thread only

    std::string InstalledHash(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return "";
        ULONGLONG size = (ULONGLONG(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        HashCache& c = g_hashes[path];
        if (!c.hash.empty() && c.size == size && CompareFileTime(&c.written, &fa.ftLastWriteTime) == 0) return c.hash;
        std::string body, err;
        if (!ReadLocal(path, kMaxFileBytes, body, err)) return "";
        c.size = size; c.written = fa.ftLastWriteTime; c.hash = Sha256Hex(body.data(), body.size());
        return c.hash;
    }

    // ---- index ------------------------------------------------------------------------------------------------
    bool ValidName(const std::string& n)
    {
        if (n.empty() || n.size() > 64 || _stricmp(n.c_str(), "RadiantPlugins") == 0) return false;
        for (char c : n) if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') return false;
        return true;
    }
    // a relative path inside the plugin's folder: "Name.dll", "themes/dark.qss", no drive, no "..", no odd characters
    bool ValidPath(const std::string& p)
    {
        if (p.empty() || p.size() > 200) return false;
        size_t start = 0;
        for (;;)
        {
            size_t slash = p.find('/', start);
            std::string seg = p.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            if (seg.empty() || seg == "." || seg == ".." || seg.back() == '.' || seg.back() == ' ') return false;
            for (char c : seg) if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.' && c != ' ') return false;
            if (slash == std::string::npos) return true;
            start = slash + 1;
        }
    }
    bool ValidUrl(const std::string& u)
    {
        if (_strnicmp(u.c_str(), "https://", 8) != 0 || u.size() > 2000) return false;
        for (char c : u) if (static_cast<unsigned char>(c) <= ' ' || static_cast<unsigned char>(c) > '~' || c == '"') return false;
        return true;
    }
    bool ValidHash(const std::string& h)
    {
        if (h.size() != 64) return false;
        for (char c : h) if (!isxdigit(static_cast<unsigned char>(c))) return false;
        return true;
    }
    std::string Lower(std::string s) { for (auto& c : s) c = char(tolower(static_cast<unsigned char>(c))); return s; }

    // a file without "url" is stored in the index repo: <folder of index.json>/plugins/<name>/<version>/<path>
    std::string RepoFileUrl(const std::string& indexSrc, bool local, const browse::Entry& e, const std::string& path)
    {
        const std::string& v = e.version;
        if (v.empty() || v.size() > 32 || v.front() == '.' || v.back() == '.' || v.find("..") != std::string::npos) return "";
        for (char c : v) if (!isdigit(static_cast<unsigned char>(c)) && c != '.') return "";
        size_t cut = indexSrc.find_last_of("/\\");
        if (cut == std::string::npos) return "";
        std::string u = indexSrc.substr(0, cut + 1) + "plugins/" + e.name + "/" + v + "/";
        for (char c : path) { if (c == ' ' && !local) u += "%20"; else u += c; }
        return u;
    }

    bool ParseIndex(const std::string& text, const std::string& indexSrc, bool local, std::vector<browse::Entry>& out, std::string& err)
    {
        Json root;
        if (!Json::parse(text, root, err)) { err = "the plugin list is not valid JSON (" + err + ")"; return false; }
        int format = root.integer("format", 0);
        if (format != 1) { err = format > 1 ? "the plugin list needs a newer MTPlugins" : "the plugin list has no format 1"; return false; }
        const Json* list = root.get("plugins");
        if (!list || !list->isArray()) { err = "the plugin list has no \"plugins\" array"; return false; }
        for (const Json& j : list->a)
        {
            browse::Entry e;
            e.name = j.str("name");
            if (!ValidName(e.name)) { LOG("browse: skipped an entry with name '%s'", e.name.c_str()); continue; }
            e.displayName = j.str("displayName", e.name);
            e.author = j.str("author");
            e.description = j.str("description");
            e.version = j.str("version");
            e.changes = j.str("changes");
            e.minHost = j.str("minHost");
            for (auto& c : e.changes) if (c == '\r' || c == '\n' || c == '\t') c = ' ';
            if (e.changes.size() > 200) e.changes = e.changes.substr(0, 197) + "...";
            e.url = j.str("url");
            if (!ValidUrl(e.url)) e.url.clear();
            e.apiVersion = uint32_t(j.integer("apiVersion", 0));
            if (const Json* apps = j.get("apps"))
                for (const Json& a : apps->a)
                {
                    std::string s = Lower(a.s);
                    if (s == "radiant") e.apps |= MODTOOLS_APP_RADIANT; else if (s == "ape") e.apps |= MODTOOLS_APP_APE; else if (s == "launcher") e.apps |= MODTOOLS_APP_LAUNCHER;
                }
            bool ok = true, hasDll = false;
            if (const Json* files = j.get("files"))
                for (const Json& f : files->a)
                {
                    browse::File bf;
                    bf.path = f.str("path"); bf.url = f.str("url"); bf.sha256 = Lower(f.str("sha256")); bf.config = f.boolean("config", false);
                    if (bf.url.empty() && ValidPath(bf.path)) bf.url = RepoFileUrl(indexSrc, local, e, bf.path);
                    bool urlOk = ValidUrl(bf.url) || (local && !bf.url.empty());
                    if (!ValidPath(bf.path) || !urlOk || !ValidHash(bf.sha256)) { ok = false; break; }
                    if (_stricmp(bf.path.c_str(), (e.name + ".dll").c_str()) == 0) hasDll = true;
                    e.files.push_back(bf);
                }
            if (!ok || !hasDll) { LOG("browse: skipped %s (bad or missing files; it must list %s.dll with sha256, and a url or a numeric version)", e.name.c_str(), e.name.c_str()); continue; }
            out.push_back(e);
        }
        return true;
    }

    // ---- bundled plugins --------------------------------------------------------------------------------------
    bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
    std::wstring PluginDll(const std::string& name) { std::wstring w = Widen(name); return plugins::Root() + L"\\" + w + L"\\" + w + L".dll"; }
    std::wstring SetupPath() { return plugins::Root() + L"\\" + kSetupDir + L"\\" + kSetupExe; }

    // <game>\bin\plugins -> <game>, empty when the plugins folder is elsewhere (a dev build): Setup installs only into a game folder
    std::wstring GameRoot()
    {
        const std::wstring& r = plugins::Root();
        const wchar_t* tail = L"\\bin\\plugins";
        size_t n = wcslen(tail);
        if (r.size() <= n || _wcsicmp(r.c_str() + r.size() - n, tail) != 0) return L"";
        return r.substr(0, r.size() - n);
    }

    std::string IniText(const wchar_t* section, const wchar_t* key, const std::wstring& ini)
    {
        wchar_t buf[1024] = {};
        GetPrivateProfileStringW(section, key, L"", buf, 1024, ini.c_str());
        return Narrow(buf);
    }

    // bundled.ini: one [Name] per plugin with component= (Setup's name for it), displayName=, author=, description=,
    // apps= (radiant, ape, launcher), url=, note= (said in the install question)
    void LoadBundled()
    {
        std::vector<browse::Entry> list;
        std::wstring ini = plugins::Root() + L"\\" + kSetupDir + L"\\" + kBundledIni;
        if (!GameRoot().empty() && Exists(SetupPath()) && Exists(ini))
        {
            std::vector<wchar_t> names(8192);
            DWORD n = GetPrivateProfileSectionNamesW(names.data(), DWORD(names.size()), ini.c_str());
            for (const wchar_t* sec = names.data(); sec < names.data() + n && *sec; sec += wcslen(sec) + 1)
            {
                browse::Entry e;
                e.name = Narrow(sec);
                e.component = IniText(sec, L"component", ini);
                bool ok = ValidName(e.name) && !e.component.empty() && e.component.size() <= 32;
                for (char c : e.component) if (!islower(static_cast<unsigned char>(c)) && !isdigit(static_cast<unsigned char>(c))) ok = false;
                if (!ok) { LOG("browse: skipped [%s] of bundled.ini (bad name or component)", e.name.c_str()); continue; }
                e.bundled = true;
                e.apiVersion = MODTOOLS_PLUGIN_API_VERSION;
                e.displayName = IniText(sec, L"displayName", ini);
                if (e.displayName.empty()) e.displayName = e.name;
                e.author = IniText(sec, L"author", ini);
                e.description = IniText(sec, L"description", ini);
                e.note = IniText(sec, L"note", ini);
                e.url = IniText(sec, L"url", ini);
                if (!ValidUrl(e.url)) e.url.clear();
                std::string apps = Lower(IniText(sec, L"apps", ini));
                if (apps.find("radiant") != std::string::npos) e.apps |= MODTOOLS_APP_RADIANT;
                if (apps.find("ape") != std::string::npos) e.apps |= MODTOOLS_APP_APE;
                if (apps.find("launcher") != std::string::npos) e.apps |= MODTOOLS_APP_LAUNCHER;
                list.push_back(e);
            }
        }
        std::lock_guard<std::mutex> lk(g_mu);
        g_bundled.swap(list);
    }

    // Whether MTPlugins was installed into `root` for this user only (Setup's /CURRENTUSER), else a rerun would ask for
    // administrator rights and install a second copy.
    bool InstalledAt(HKEY hive, const std::wstring& root)
    {
        wchar_t path[1024] = {};
        DWORD size = sizeof path - sizeof(wchar_t);
        if (RegGetValueW(hive, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{6C2D8F4E-3B1A-4E7C-9A52-8D0F1B7E6A31}_is1",   // AppId in MTPlugins.iss
                         L"Inno Setup: App Path", RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS) return false;
        std::wstring p = path;
        while (!p.empty() && p.back() == L'\\') p.pop_back();
        return _wcsicmp(p.c_str(), root.c_str()) == 0;
    }
    bool PerUserInstall(const std::wstring& root) { return InstalledAt(HKEY_CURRENT_USER, root) && !InstalledAt(HKEY_LOCAL_MACHINE, root); }

    // /COMPONENTS replaces the whole selection and removes unticked plugins, so name every plugin of ours on disk plus the one
    // to add. /ADDPLUGIN copies only that plugin's files, nothing has to close. An administrator install asks for UAC itself.
    bool StartSetup(const browse::Entry& e)
    {
        std::wstring root = GameRoot(), setup = SetupPath();
        if (root.empty() || !Exists(setup))
        { SetMessage(e.displayName + ": MTPlugins Setup is not beside the uninstaller any more. Run the MTPlugins installer and tick it there."); return false; }
        std::vector<browse::Entry> all;
        { std::lock_guard<std::mutex> lk(g_mu); all = g_bundled; }
        std::string comps = "core";
        for (const browse::Entry& b : all)
            if (_stricmp(b.name.c_str(), e.name.c_str()) == 0 || Exists(PluginDll(b.name))) comps += "," + b.component;
        std::wstring params = L"/SILENT /SUPPRESSMSGBOXES /NOCANCEL /NORESTART /NOCLOSEAPPLICATIONS /NORESTARTAPPLICATIONS /DIR=\"" + root +
                              L"\" /COMPONENTS=\"" + Widen(comps) + L"\" /ADDPLUGIN=" + Widen(e.name);
        if (PerUserInstall(root)) params += L" /CURRENTUSER";
        SHELLEXECUTEINFOW sei{ sizeof sei };
        sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS;
        sei.lpFile = setup.c_str();
        sei.lpParameters = params.c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&sei) || !sei.hProcess)
        {
            DWORD err = GetLastError();
            SetMessage(err == ERROR_CANCELLED ? e.displayName + " was not added (the permission prompt was cancelled)"
                                              : "MTPlugins Setup did not start (Windows error " + std::to_string(err) + ")");
            return false;
        }
        LOG("browse: %s", Narrow(setup + L" " + params).c_str());
        g_setupProcess = sei.hProcess;
        g_setupEntry = e;
        g_installing = true;
        SetMessage("MTPlugins Setup is adding " + e.displayName + "...");
        return true;
    }

    void FinishSetup()
    {
        DWORD code = 1;
        GetExitCodeProcess(g_setupProcess, &code);
        CloseHandle(g_setupProcess);
        g_setupProcess = nullptr;
        const browse::Entry e = g_setupEntry;
        plugins::Rescan();                              // registers the new plugin and loads it when it is for this editor
        ModToolsPlugin* p = plugins::Find(e.name);
        if (Exists(PluginDll(e.name)))
        {
            if (p && !p->enabled) plugins::SetEnabled(p, true);      // asked for by name: it starts on
            SetMessage("Installed " + e.displayName + "." + (p && p->apps && !plugins::ForThisEditor(p) ? " It runs in: " + plugins::AppNames(p->apps) + "." : ""));
        }
        else SetMessage("MTPlugins Setup did not add " + e.displayName + " (exit code " + std::to_string(code) +
                        (code == 2 || code == 5 ? ", cancelled" : "") + "; its log is \"Setup Log\" in the temp folder)");
        g_installing = false;
        ++g_version;
    }

    // ---- workers ----------------------------------------------------------------------------------------------
    struct Job { void (*fn)(void*); void* arg; HMODULE self; };

    DWORD WINAPI JobProc(void* p)
    {
        Job job = *static_cast<Job*>(p);
        delete static_cast<Job*>(p);
        try { job.fn(job.arg); }
        catch (...) { SetMessage("internal error in the plugin browser"); }
        FreeLibraryAndExitThread(job.self, 0);    // the thread holds a reference, so the host never unloads under it
    }

    bool StartWorker(void (*fn)(void*), void* arg)
    {
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&JobProc), &self)) return false;
        Job* job = new Job{ fn, arg, self };
        HANDLE t = CreateThread(nullptr, 0, JobProc, job, 0, nullptr);
        if (!t) { delete job; FreeLibrary(self); return false; }
        CloseHandle(t);
        return true;
    }

    // the official feed, same format as the index: entries for plugins Setup does not hold are dropped
    void FetchUpdates(const std::wstring& src)
    {
        std::vector<browse::Entry> bundled;
        { std::lock_guard<std::mutex> lk(g_mu); bundled = g_bundled; }
        std::vector<browse::Entry> list, keep;
        bool local = !StartsWithI(src, L"https://");
        std::string body, err;
        if (!bundled.empty() && (!Fetch(Narrow(src), true, kMaxIndexBytes, body, err) || !ParseIndex(body, Narrow(src), local, list, err)))
            LOG("browse: no official updates: %s", err.c_str());
        for (browse::Entry& e : list)
        {
            bool ours = false;
            for (const browse::Entry& b : bundled) if (_stricmp(b.name.c_str(), e.name.c_str()) == 0) ours = true;
            if (!ours) { LOG("browse: skipped %s in the official updates (not a plugin Setup holds)", e.name.c_str()); continue; }
            e.official = true;
            keep.push_back(e);
        }
        std::lock_guard<std::mutex> lk(g_mu);
        g_official.swap(keep);
        g_localUpdates = local;
    }

    struct Sources { std::wstring index, updates; };

    void FetchIndex(void* arg)
    {
        Sources* srcs = static_cast<Sources*>(arg);
        std::wstring* src = &srcs->index;
        bool local = !StartsWithI(*src, L"https://");
        std::string body, err;
        std::vector<browse::Entry> list;
        bool ok = Fetch(Narrow(*src), true, kMaxIndexBytes, body, err) && ParseIndex(body, Narrow(*src), local, list, err);
        FetchUpdates(srcs->updates);
        delete srcs;
        if (ok)
        {
            { std::lock_guard<std::mutex> lk(g_mu); g_entries = list; g_loaded = true; g_localIndex = local; }
            SetMessage(std::to_string(list.size()) + " plugin(s) in the list");
        }
        else SetMessage("Could not load the plugin list: " + err);
        g_loading = false;
        ++g_version;
    }

    void RemoveTree(const std::wstring& dir)
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                std::wstring n = fd.cFileName;
                if (n == L"." || n == L"..") continue;
                std::wstring p = dir + L"\\" + n;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(p); else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(p.c_str()); }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(dir.c_str());
    }

    bool MakeDirs(const std::wstring& dir)
    {
        if (dir.empty() || GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
        size_t cut = dir.find_last_of(L'\\');
        if (cut != std::wstring::npos && !MakeDirs(dir.substr(0, cut))) return false;
        return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
    }
    std::wstring WinPath(const std::string& rel) { std::wstring w = Widen(rel); for (auto& c : w) if (c == L'/') c = L'\\'; return w; }

    struct InstallJob { browse::Entry entry; std::wstring root; bool local; bool wasInstalled; };

    void DownloadAndVerify(void* arg)
    {
        InstallJob* job = static_cast<InstallJob*>(arg);
        std::wstring dir = job->root + L"\\" + kStaging + L"\\" + Widen(job->entry.name);
        RemoveTree(dir);
        bool ok = true;
        for (size_t i = 0; ok && i < job->entry.files.size(); ++i)
        {
            const browse::File& f = job->entry.files[i];
            SetMessage("Downloading " + job->entry.displayName + " (" + std::to_string(i + 1) + " of " + std::to_string(job->entry.files.size()) + "): " + f.path);
            std::string body, err;
            if (!Fetch(f.url, job->local, kMaxFileBytes, body, err)) { SetMessage("Install failed: " + f.path + ": " + err); ok = false; break; }
            if (Sha256Hex(body.data(), body.size()) != f.sha256)
            { SetMessage("Install failed: " + f.path + " does not match its SHA-256 in the plugin list (nothing was changed)"); ok = false; break; }
            std::wstring dst = dir + L"\\" + WinPath(f.path);
            if (!MakeDirs(dst.substr(0, dst.find_last_of(L'\\')))) { SetMessage("Install failed: cannot create the download folder"); ok = false; break; }
            HANDLE h = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD wrote = 0;
            bool w = h != INVALID_HANDLE_VALUE && (body.empty() || WriteFile(h, body.data(), DWORD(body.size()), &wrote, nullptr)) && wrote == body.size();
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            if (!w) { SetMessage("Install failed: cannot write " + f.path + " to the download folder"); ok = false; }
        }
        if (ok)
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_staged.ready = true; g_staged.entry = job->entry; g_staged.dir = dir; g_staged.wasInstalled = job->wasInstalled;
        }
        else { RemoveTree(dir); g_installing = false; }
        delete job;
        ++g_version;
    }

    // ---- main thread: move the verified files into place ------------------------------------------------------
    void DeleteOldFiles(const std::wstring& dir)      // *.mtp-old left by an update while another editor used the file
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do
        {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteOldFiles(p);
            else if (n.size() > wcslen(kOldSuffix) && _wcsicmp(n.c_str() + n.size() - wcslen(kOldSuffix), kOldSuffix) == 0) DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // true = in place, *renamed = the old file was still in use elsewhere and was renamed out of the way
    bool PlaceFile(const std::wstring& src, const std::wstring& dst, bool* renamed)
    {
        if (MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) return true;
        if (GetFileAttributesW(dst.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
        // a DLL or exe mapped by another running editor cannot be replaced, but it can be renamed
        std::wstring old = dst + kOldSuffix;
        DeleteFileW(old.c_str());
        if (GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES) old = dst + L"." + std::to_wstring(GetTickCount64()) + kOldSuffix;
        if (!MoveFileExW(dst.c_str(), old.c_str(), 0)) return false;
        if (MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) { *renamed = true; return true; }
        MoveFileExW(old.c_str(), dst.c_str(), 0);      // put the old one back
        return false;
    }

    void FinishInstall()
    {
        Staged st;
        { std::lock_guard<std::mutex> lk(g_mu); st = g_staged; }
        ModToolsPlugin* p = plugins::Find(st.entry.name);
        if (p && p->module)
        {
            plugins::Unload(p);
            if (p->module) return;                     // unload deferred until the plugin's current call returns: next tick
        }
        { std::lock_guard<std::mutex> lk(g_mu); g_staged.ready = false; }
        std::wstring dir = plugins::Root() + L"\\" + Widen(st.entry.name);
        bool ok = MakeDirs(dir), renamed = false;
        std::string failed;
        for (const browse::File& f : st.entry.files)
        {
            if (!ok) break;
            std::wstring dst = dir + L"\\" + WinPath(f.path), src = st.dir + L"\\" + WinPath(f.path);
            if (f.config && GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) continue;   // keep the user's settings
            if (!MakeDirs(dst.substr(0, dst.find_last_of(L'\\'))) || !PlaceFile(src, dst, &renamed)) { ok = false; failed = f.path; }
        }
        RemoveTree(st.dir);
        RemoveDirectoryW((plugins::Root() + L"\\" + kStaging).c_str());
        plugins::Rescan();                              // registers a new plugin, loads an enabled one that is not loaded
        p = plugins::Find(st.entry.name);
        if (p && !st.wasInstalled && !p->enabled) plugins::SetEnabled(p, true);   // a fresh install starts on
        else if (p && p->enabled && !p->module && plugins::ForThisEditor(p)) plugins::Load(p);
        std::string name = st.entry.displayName + (st.entry.version.empty() ? "" : " " + st.entry.version);
        if (!ok) SetMessage("Install of " + st.entry.displayName + " stopped at " + failed + " (file in use? close the other editors and try again)");
        else SetMessage(std::string(st.wasInstalled ? "Updated " : "Installed ") + name + "." +
                        (renamed ? " Another running editor keeps the old version until it restarts." : ""));
        g_installing = false;
        ++g_version;
    }

    // ---- uninstall --------------------------------------------------------------------------------------------
    std::string g_uninstall;             // main thread: the plugin waiting for its unload
    std::string g_uninstallNote, g_uninstallNoteFor;

    // deleted, or moved into the trash when another editor still has it open (a mapped DLL can be moved, not deleted)
    void MoveOut(const std::wstring& src, const std::wstring& dst, std::vector<std::wstring>& stuck, int& moved)
    {
        SetFileAttributesW(src.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(src.c_str())) return;
        MakeDirs(dst.substr(0, dst.find_last_of(L'\\')));
        if (MoveFileExW(src.c_str(), dst.c_str(), 0)) ++moved; else stuck.push_back(src);
    }
    void MoveTreeOut(const std::wstring& dir, const std::wstring& trash, std::vector<std::wstring>& stuck, int& moved)
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                std::wstring n = fd.cFileName;
                if (n == L"." || n == L"..") continue;
                std::wstring p = dir + L"\\" + n;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { if (!RemoveDirectoryW(p.c_str()) && !DeleteFileW(p.c_str())) stuck.push_back(p); }   // the link only, never its target
                else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) MoveTreeOut(p, trash + L"\\" + n, stuck, moved);
                else MoveOut(p, trash + L"\\" + n, stuck, moved);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(dir.c_str());
    }

    void FinishUninstall()
    {
        ModToolsPlugin* p = plugins::Find(g_uninstall);
        if (!p) { g_uninstall.clear(); return; }
        if (p->module)
        {
            plugins::Unload(p);
            if (p->module) return;                     // unload deferred until the plugin's current call returns: next tick
        }
        g_uninstall.clear();
        std::string name = p->displayName.empty() ? p->name : p->displayName;
        std::wstring trash = plugins::Root() + L"\\" + kRemoved;
        RemoveTree(trash);                              // what earlier uninstalls left, once nothing holds it
        std::wstring dest = trash + L"\\" + Widen(p->name) + L"-" + std::to_wstring(GetTickCount64());
        std::vector<std::wstring> stuck;
        int moved = 0;
        DWORD attrs = GetFileAttributesW(p->dir.c_str());
        if (_wcsicmp(p->dir.c_str(), plugins::Root().c_str()) == 0)     // a loose <plugins>\<Name>.dll
        {
            MoveOut(p->dllPath, dest + L"\\" + Widen(p->name) + L".dll", stuck, moved);
            if (Exists(p->iniPath)) MoveOut(p->iniPath, dest + L"\\" + Widen(p->name) + L".ini", stuck, moved);
        }
        else if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) RemoveDirectoryW(p->dir.c_str());
        else MoveTreeOut(p->dir, dest, stuck, moved);
        RemoveDirectoryW(trash.c_str());

        g_uninstallNoteFor = p->name;
        if (Exists(p->dllPath))
        {
            g_uninstallNote = "Could not uninstall " + name + ": " + Narrow(p->dllPath) + " is in use. Close the other editors and try again.";
            plugins::Rescan();                          // loads it again when it is enabled
        }
        else
        {
            plugins::Forget(p);
            g_uninstallNote = "Uninstalled " + name + ".";
            if (moved) g_uninstallNote += " Editors that still have it loaded keep it until they restart.";
            if (!stuck.empty()) g_uninstallNote += " " + std::to_string(stuck.size()) + " file(s) were in use and stay in " + Narrow(p->dir) + ".";
        }
        LOG("browse: %s", g_uninstallNote.c_str());
        ++g_version;
    }
}

namespace browse
{
    void Refresh()
    {
        if (g_loading.exchange(true)) return;
        SetMessage("Loading the plugin list...");
        DeleteOldFiles(plugins::Root());
        RemoveTree(plugins::Root() + L"\\" + kRemoved);
        LoadBundled();
        Sources* srcs = new Sources{ IndexSource(), UpdatesSource() };
        if (!::StartWorker(FetchIndex, srcs)) { delete srcs; g_loading = false; SetMessage("Could not start the download thread"); }
    }
    bool Loading() { return g_loading; }
    bool Loaded() { std::lock_guard<std::mutex> lk(g_mu); return g_loaded; }
    std::vector<Entry> Entries()
    {
        std::vector<Entry> out, official;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            for (const Entry& b : g_bundled) if (!Exists(PluginDll(b.name))) out.push_back(b);
            for (const Entry& o : g_official) if (Exists(PluginDll(o.name))) official.push_back(o);
        }
        // an official entry shows only while it is an update: the Installed view already lists our plugins
        for (const Entry& o : official) if (StatusOf(o) == Status::UpdateAvailable) out.push_back(o);
        std::lock_guard<std::mutex> lk(g_mu);
        // a list entry named like a plugin Setup holds is left out: Setup's copy is the one made for this MTPlugins
        for (const Entry& e : g_entries)
        {
            bool ours = false;
            for (const Entry& b : g_bundled) if (_stricmp(b.name.c_str(), e.name.c_str()) == 0) ours = true;
            if (!ours) out.push_back(e);
        }
        return out;
    }
    std::string Message() { std::lock_guard<std::mutex> lk(g_mu); return g_message; }
    unsigned Version() { return g_version; }
    bool Installing() { return g_installing; }

    Status StatusOf(const Entry& e)
    {
        if (e.bundled) return Exists(PluginDll(e.name)) ? Status::Installed : Status::NotInstalled;   // never an update: Setup brings those
        if (e.apiVersion != MODTOOLS_PLUGIN_API_VERSION) return Status::Incompatible;
        if (!e.minHost.empty())
        {
            std::vector<int> need = ParseVersion(e.minHost);
            if (need.empty() || Newer(need, ParseVersion(MTPLUGINS_VERSION))) return Status::Incompatible;
        }
        std::wstring dir = plugins::Root() + L"\\" + Widen(e.name);
        if (GetFileAttributesW((dir + L"\\" + Widen(e.name) + L".dll").c_str()) == INVALID_FILE_ATTRIBUTES) return Status::NotInstalled;
        bool same = true;
        for (const File& f : e.files)
            if (!f.config && InstalledHash(dir + L"\\" + WinPath(f.path)) != f.sha256) { same = false; break; }
        if (same) return Status::Installed;
        // files differ: an update only when the list's version is newer (unknown: assume so)
        std::vector<int> listed = ParseVersion(e.version), mine = ParseVersion(plugins::InstalledVersion(e.name));
        if (listed.empty() || mine.empty()) return Status::UpdateAvailable;
        return Newer(listed, mine) ? Status::UpdateAvailable : Status::Modified;
    }

    std::vector<Entry> Updates()
    {
        std::vector<Entry> out;
        for (const Entry& e : Entries()) if (StatusOf(e) == Status::UpdateAvailable) out.push_back(e);
        return out;
    }

    std::string ChangesOf(const std::string& name, const std::string& version)
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (const std::vector<Entry>* list : { &g_official, &g_entries })
            for (const Entry& e : *list)
                if (_stricmp(e.name.c_str(), name.c_str()) == 0 && e.version == version) return e.changes;
        return "";
    }

    const char* StatusName(Status s)
    {
        switch (s)
        {
        case Status::NotInstalled: return "Not installed";
        case Status::Installed: return "Installed";
        case Status::UpdateAvailable: return "Update available";
        case Status::Incompatible: return "Needs a newer MTPlugins";
        case Status::Modified: return "Modified (yours differs, not older)";
        }
        return "?";
    }

    bool Install(const std::string& name)
    {
        if (g_installing) { SetMessage("Another install is still running"); return false; }
        Entry e;
        bool found = false, local = false;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            for (const Entry& x : g_entries) if (_stricmp(x.name.c_str(), name.c_str()) == 0) { e = x; found = true; }
            for (const Entry& x : g_bundled) if (_stricmp(x.name.c_str(), name.c_str()) == 0) { e = x; found = true; }
            local = g_localIndex;
            // an installed plugin of ours updates from the official feed, a missing one still comes from Setup
            if (found && e.bundled && Exists(PluginDll(e.name)))
                for (const Entry& x : g_official) if (_stricmp(x.name.c_str(), name.c_str()) == 0) { e = x; local = g_localUpdates; }
        }
        if (!found) { SetMessage("That plugin is not in the list any more"); return false; }
        if (e.bundled)
        {
            if (Exists(PluginDll(e.name))) { SetMessage(e.displayName + " is already installed"); return false; }
            return StartSetup(e);
        }
        Status s = StatusOf(e);
        if (s == Status::Incompatible) { SetMessage(e.displayName + " needs a newer MTPlugins"); return false; }
        g_installing = true;
        InstallJob* job = new InstallJob{ e, plugins::Root(), local, s != Status::NotInstalled };
        if (!::StartWorker(DownloadAndVerify, job)) { delete job; g_installing = false; SetMessage("Could not start the download thread"); return false; }
        return true;
    }

    void InstallAll(const std::vector<std::string>& names)
    {
        for (const std::string& n : names) g_queue.push_back(n);
    }
    bool QueueBusy() { return !g_queue.empty() || !g_queueCurrent.empty(); }
    std::vector<std::string> TakeQueueReport() { std::vector<std::string> r; r.swap(g_queueReport); return r; }

    void Tick()
    {
        bool ready;
        { std::lock_guard<std::mutex> lk(g_mu); ready = g_staged.ready; }
        if (ready) FinishInstall();
        if (!g_uninstall.empty()) FinishUninstall();
        if (g_setupProcess && WaitForSingleObject(g_setupProcess, 0) != WAIT_TIMEOUT) FinishSetup();
        if (!g_queueCurrent.empty() && !g_installing) { g_queueReport.push_back(Message()); g_queueCurrent.clear(); }
        while (g_queueCurrent.empty() && !g_installing && !g_queue.empty())
        {
            std::string next = g_queue.front();
            g_queue.erase(g_queue.begin());
            if (Install(next)) g_queueCurrent = next;
            else g_queueReport.push_back(Message());
        }
    }

    bool Uninstall(const std::string& name)
    {
        ModToolsPlugin* p = plugins::Find(name);
        g_uninstallNoteFor = name;
        if (!p) { g_uninstallNote = "No plugin named " + name; ++g_version; return false; }
        if (g_installing || !g_uninstall.empty()) { g_uninstallNote = "An install or uninstall is still running"; ++g_version; return false; }
        g_uninstall = p->name;
        g_uninstallNote = "Uninstalling " + (p->displayName.empty() ? p->name : p->displayName) + "...";
        ++g_version;
        FinishUninstall();
        return true;
    }
    std::string UninstallNote(const std::string& name) { return name.empty() || _stricmp(name.c_str(), g_uninstallNoteFor.c_str()) == 0 ? g_uninstallNote : ""; }
    std::string UninstallNote() { return g_uninstallNote; }

    bool Fetch(const std::string& src, bool localAllowed, size_t cap, std::string& body, std::string& err) { return ::Fetch(src, localAllowed, cap, body, err); }
    std::string Sha256(const std::string& data) { return Sha256Hex(data.data(), data.size()); }

    std::vector<int> ParseVersion(std::string v)
    {
        while (!v.empty() && isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
        if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
        std::vector<int> out;
        int n = 0; bool digit = false;
        for (char c : v)
        {
            if (isdigit(static_cast<unsigned char>(c)) && n < 100000) { n = n * 10 + (c - '0'); digit = true; }
            else if (c == '.' && digit) { out.push_back(n); n = 0; digit = false; }
            else return {};
        }
        if (!digit || out.size() >= 4) return {};
        out.push_back(n);
        return out;
    }
    bool Newer(const std::vector<int>& a, const std::vector<int>& b)
    {
        for (size_t i = 0; i < 4; ++i)
        {
            int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
            if (x != y) return x > y;
        }
        return false;
    }
    bool StartWorker(void (*fn)(void*), void* arg) { return ::StartWorker(fn, arg); }
}
