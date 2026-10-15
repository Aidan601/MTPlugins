// update.cpp: the "new MTPlugins release" question in the launcher and the "plugin updates" question in every app (see update.h).

// MAP
//   Settings     Enabled/SetEnabled ([update] check=), PluginsEnabled ([update] plugins=), Source ([update] url=), Skipped
//   Workers      CheckWorker (GitHub's latest release JSON -> setup asset), DownloadWorker (setup exe into %TEMP%)
//   Main thread  StartupCheck, CheckNow, Tick -> Ask, Launch, DownloadFailed
//   Plugins      PendingPlugins, AskPlugins (renamed MessageBox buttons), ReportPlugins
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <atomic>
#include <mutex>
#include <vector>
#include "update.h"
#include "version.h"
#include "browse.h"
#include "config.h"
#include "mainthread.h"
#include "app.h"
#include "plugins.h"
#include "hooks.h"
#include "log.h"
#include "../plugins/common/json.h"

#pragma comment(lib, "shell32.lib")

namespace
{
    const char* kLatest = "https://api.github.com/repos/Aidan601/MTPlugins/releases/latest";
    const char* kReleases = "https://github.com/Aidan601/MTPlugins/releases";
    const size_t kMaxJson = 4u << 20;
    const size_t kMaxSetup = 256u << 20;

    enum Phase { Idle, Checking, Found, Asking, Downloading, Downloaded, DownloadError };
    std::atomic<int> g_phase{ Idle };
    std::mutex g_mu;                       // guards everything below that a worker touches
    struct Release { std::string version, page, setupUrl, sha256; bool local = false; };
    Release g_rel;
    std::string g_status = "not checked";
    std::wstring g_setupPath;
    bool g_startupDone = false;
    bool g_inTick = false;                 // MessageBox's modal loop runs the tick again
    // plugin updates: the index is fetched at start, asked once it is in, never in the same start as the release question
    bool g_pluginsPending = false;
    bool g_releaseAsked = false;
    bool g_manual = false;                 // Check for updates: ask even when switched off, answer even when nothing is new
    std::string g_releaseLine;             // one line for the manual answer (guarded by g_mu)
    bool g_pluginsInstalling = false;      // InstallAll started from the question: report when it is done
    std::string g_pluginStatus = "not checked";

    std::wstring Widen(const std::string& s)
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n ? n - 1 : 0, L'\0');
        if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
        return w;
    }
    void SetStatus(const std::string& s)
    {
        { std::lock_guard<std::mutex> lk(g_mu); g_status = s; }
        LOG("update: %s", s.c_str());
    }

    std::string Source()
    {
        wchar_t buf[2048] = {};
        GetPrivateProfileStringW(L"update", L"url", L"", buf, 2048, Cfg().iniPath.c_str());
        int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
        std::string s(n ? n - 1 : 0, '\0');
        if (n) WideCharToMultiByte(CP_UTF8, 0, buf, -1, &s[0], n, nullptr, nullptr);
        while (!s.empty() && isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        return s;
    }

    bool ValidHttps(const std::string& u)
    {
        if (_strnicmp(u.c_str(), "https://", 8) != 0 || u.size() > 2000) return false;
        for (char c : u) if (static_cast<unsigned char>(c) <= ' ' || static_cast<unsigned char>(c) > '~' || c == '"') return false;
        return true;
    }

    // ---- workers ----------------------------------------------------------------------------------------------
    void CheckWorker(void* arg)
    {
        std::string* src = static_cast<std::string*>(arg);
        bool local = _strnicmp(src->c_str(), "https://", 8) != 0;
        std::string body, err;
        bool ok = browse::Fetch(*src, true, kMaxJson, body, err);
        delete src;
        if (!ok)
        {
            // 404: no published release yet (drafts do not count, a private repo answers 404 too)
            SetStatus("could not ask GitHub for the latest release: " + err);
            { std::lock_guard<std::mutex> lk(g_mu); g_releaseLine = "Could not check for a new MTPlugins release (" + err + ")."; }
            g_phase = Idle;
            return;
        }
        Json root;
        if (!Json::parse(body, root, err)) { SetStatus("the release answer is not JSON (" + err + ")"); g_phase = Idle; return; }
        Release r;
        r.local = local;
        r.version = root.str("tag_name");
        if (!r.version.empty() && (r.version[0] == 'v' || r.version[0] == 'V')) r.version.erase(0, 1);
        std::vector<int> latest = browse::ParseVersion(r.version), mine = browse::ParseVersion(MTPLUGINS_VERSION);
        if (latest.empty()) { SetStatus("the latest release has no version tag ('" + root.str("tag_name") + "')"); g_phase = Idle; return; }
        if (!browse::Newer(latest, mine))
        {
            SetStatus("up to date (" MTPLUGINS_VERSION ", latest release " + r.version + ")");
            { std::lock_guard<std::mutex> lk(g_mu); g_releaseLine = "MTPlugins " MTPLUGINS_VERSION " is up to date."; }
            g_phase = Idle;
            return;
        }
        r.page = root.str("html_url");
        if (!ValidHttps(r.page)) r.page = kReleases;
        if (const Json* assets = root.get("assets"))
            for (const Json& a : assets->a)
            {
                std::string name = a.str("name"), url = a.str("browser_download_url"), digest = a.str("digest");
                if (_strnicmp(name.c_str(), "MTPlugins-Setup", 15) != 0 || name.size() < 5 || _stricmp(name.c_str() + name.size() - 4, ".exe") != 0) continue;
                if (!ValidHttps(url) && !(local && !url.empty())) continue;
                r.setupUrl = url;
                if (_strnicmp(digest.c_str(), "sha256:", 7) == 0 && digest.size() == 71) r.sha256 = digest.substr(7);
                for (auto& c : r.sha256) c = char(tolower(static_cast<unsigned char>(c)));
            }
        { std::lock_guard<std::mutex> lk(g_mu); g_rel = r; }
        SetStatus("MTPlugins " + r.version + " is out (this is " MTPLUGINS_VERSION ")" + (r.setupUrl.empty() ? ", the release has no installer" : ""));
        g_phase = Found;
    }

    void DownloadWorker(void*)
    {
        Release r;
        { std::lock_guard<std::mutex> lk(g_mu); r = g_rel; }
        std::string body, err;
        SetStatus("downloading " + r.setupUrl);
        bool ok = browse::Fetch(r.setupUrl, r.local, kMaxSetup, body, err);
        if (ok && !r.sha256.empty() && browse::Sha256(body) != r.sha256) { ok = false; err = "the download does not match the SHA-256 GitHub lists for it"; }
        if (ok && (body.size() < 2 || body[0] != 'M' || body[1] != 'Z')) { ok = false; err = "the download is not a Windows program"; }
        std::wstring path;
        if (ok)
        {
            wchar_t tmp[MAX_PATH + 1] = {};
            GetTempPathW(MAX_PATH, tmp);
            path = std::wstring(tmp) + L"MTPlugins-Setup-" + Widen(r.version) + L".exe";     // version: digits and dots only
            HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD wrote = 0;
            ok = h != INVALID_HANDLE_VALUE && WriteFile(h, body.data(), DWORD(body.size()), &wrote, nullptr) && wrote == body.size();
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            if (!ok) err = "cannot write the installer to the temp folder";
        }
        if (!ok) { SetStatus("update download failed: " + err); g_phase = DownloadError; return; }
        { std::lock_guard<std::mutex> lk(g_mu); g_setupPath = path; }
        SetStatus("downloaded " + std::to_string(body.size()) + " bytes" + (r.sha256.empty() ? " (GitHub listed no SHA-256)" : ", SHA-256 matches"));
        g_phase = Downloaded;
    }

    void Wrap(void (*fn)(void*), void* arg)        // a worker never lets an exception out
    {
        struct Call { void (*fn)(void*); void* arg; };
        auto run = [](void* p)
        {
            Call c = *static_cast<Call*>(p);
            delete static_cast<Call*>(p);
            try { c.fn(c.arg); }
            catch (...) { SetStatus("internal error in the update check"); g_phase = Idle; }
        };
        Call* c = new Call{ fn, arg };
        if (!browse::StartWorker(run, c)) { delete c; SetStatus("could not start the update thread"); g_phase = Idle; }
    }

    void StartCheck()
    {
        int idle = Idle;
        if (!g_phase.compare_exchange_strong(idle, Checking)) return;
        std::string src = Source();
        { std::lock_guard<std::mutex> lk(g_mu); g_releaseLine.clear(); }
        SetStatus("checking " + (src.empty() ? std::string(kLatest) : src));
        Wrap(CheckWorker, new std::string(src.empty() ? kLatest : src));
    }

    // ---- main thread ------------------------------------------------------------------------------------------
    int Box(const std::string& text, UINT flags, const wchar_t* title = L"MTPlugins update")
    {
        return MessageBoxW(mt::MainWindow(), Widen(text).c_str(), title, flags | MB_SETFOREGROUND);
    }
    void OpenPage(const std::string& url) { ShellExecuteW(nullptr, L"open", Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

    void Ask()
    {
        Release r;
        { std::lock_guard<std::mutex> lk(g_mu); r = g_rel; }
        g_phase = Asking;
        g_releaseAsked = true;
        std::string q = "MTPlugins " + r.version + " is out (you have " MTPLUGINS_VERSION ").\n\n";
        if (r.setupUrl.empty())
        {
            if (Box(q + "Open the release page?", MB_YESNO | MB_ICONINFORMATION) == IDYES) OpenPage(r.page);
            g_phase = Idle;
            return;
        }
        q += "Update now? It installs by itself with the plugins you have and keeps your settings. The launcher closes and opens again when it is done.";
        if (Box(q, MB_YESNO | MB_ICONINFORMATION) != IDYES) { SetStatus("update to " + r.version + " declined for now"); g_phase = Idle; return; }
        g_phase = Downloading;
        Wrap(DownloadWorker, nullptr);
    }

    // Radiant or APE running anywhere: they hold the plugin files, and Setup would close them unsaved
    bool EditorOpen()
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return false;
        PROCESSENTRY32W pe{ sizeof pe };
        bool found = false;
        for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
            found = !_wcsicmp(pe.szExeFile, L"Radiant_modtools.exe") || !_wcsicmp(pe.szExeFile, L"asseteditor_modtools.exe");
        CloseHandle(snap);
        return found;
    }

    void Launch()
    {
        std::wstring path;
        Release r;
        { std::lock_guard<std::mutex> lk(g_mu); path = g_setupPath; r = g_rel; }
        g_phase = Idle;
        while (EditorOpen())
            if (Box("Radiant or APE is open. Save your work and close them, then press Retry.", MB_RETRYCANCEL | MB_ICONWARNING) != IDRETRY)
            {
                SetStatus("update to " + r.version + " put off: an editor is open");
                return;
            }
        // silent: the previous folder, plugins and tasks, a progress window only, then the launcher again (Setup's RELAUNCH)
        SHELLEXECUTEINFOW sei{ sizeof sei };
        sei.fMask = SEE_MASK_NOASYNC;
        sei.hwnd = mt::MainWindow();
        sei.lpFile = path.c_str();
        sei.lpParameters = L"/SILENT /NORESTART /RELAUNCH=1";
        sei.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&sei)) { SetStatus("installer started"); return; }
        DWORD e = GetLastError();
        SetStatus("the installer did not start (error " + std::to_string(e) + ")");
        if (e == ERROR_CANCELLED) return;       // the user said no to the administrator prompt
        if (Box("The installer did not start (Windows error " + std::to_string(e) + ").\n\nOpen the release page to download it yourself?", MB_YESNO | MB_ICONWARNING) == IDYES)
            OpenPage(r.page);
    }

    void DownloadFailed()
    {
        std::string status, page;
        { std::lock_guard<std::mutex> lk(g_mu); status = g_status; page = g_rel.page; }
        g_phase = Idle;
        if (Box("The update could not be downloaded:\n" + status + "\n\nOpen the release page to download it yourself?", MB_YESNO | MB_ICONWARNING) == IDYES)
            OpenPage(page);
    }

    // ---- plugin updates (main thread) --------------------------------------------------------------------------
    std::wstring SkipKey(const std::string& name) { return L"skip_" + Widen(name); }
    bool Skipped(const browse::Entry& e)      // [update] skip_<Name>=<version>: "Skip" for that version only
    {
        wchar_t buf[128] = {};
        GetPrivateProfileStringW(L"update", SkipKey(e.name).c_str(), L"", buf, 128, Cfg().iniPath.c_str());
        return buf[0] && Widen(e.version) == buf;
    }
    void SetPluginStatus(const std::string& s) { g_pluginStatus = s; LOG("update: plugins: %s", s.c_str()); }

    // forQuestion: each app asks only about the plugins that run in it
    std::vector<browse::Entry> PendingPlugins(bool includeSkipped, bool forQuestion = false)
    {
        std::vector<browse::Entry> out;
        uint32_t mine = uint32_t(app::ApiId());
        for (const browse::Entry& e : browse::Updates())
            if ((includeSkipped || !Skipped(e)) && (!forQuestion || (e.apps & mine))) out.push_back(e);
        return out;
    }

    // an editor asks once its window is up, nothing else is modal, no map is loading and no drag is in progress
    bool ReadyToAsk()
    {
        if (app::IsLauncher()) return true;
        HWND w = mt::MainWindow();
        if (!w || !IsWindowVisible(w) || IsIconic(w) || !IsWindowEnabled(w)) return false;
        if (app::IsRadiant() && (hooks::InMapLoad() || hooks::IsReloading() || hooks::MouseButtonHeld())) return false;
        return true;
    }

    // MessageBox with our own button texts: a CBT hook renames Yes/No/Cancel when the box is created on this thread
    HHOOK g_boxHook = nullptr;
    LRESULT CALLBACK BoxHook(int code, WPARAM wp, LPARAM lp)
    {
        if (code == HCBT_ACTIVATE)
        {
            HWND box = reinterpret_cast<HWND>(wp);
            SetDlgItemTextW(box, IDYES, L"&Update all");
            SetDlgItemTextW(box, IDNO, L"&Skip");
            SetDlgItemTextW(box, IDCANCEL, L"&Later");
            UnhookWindowsHookEx(g_boxHook); g_boxHook = nullptr;
            return 0;
        }
        return CallNextHookEx(g_boxHook, code, wp, lp);
    }

    void AskPlugins(const std::vector<browse::Entry>& list)
    {
        std::string q = std::string("Plugin updates for ") + (app::IsLauncher() ? "the launcher" : app::Name()) + ":\n\n";
        for (const browse::Entry& e : list)
        {
            std::string mine = plugins::InstalledVersion(e.name);
            q += "    " + e.displayName + "  " + (mine.empty() ? "" : mine + " -> ") + e.version + "\n";
            if (!e.changes.empty()) q += "        " + e.changes + "\n";
        }
        q += "\nSkip ignores these versions. Turn these questions off in the Plugins window.";
        g_boxHook = SetWindowsHookExW(WH_CBT, BoxHook, nullptr, GetCurrentThreadId());
        int r = Box(q, MB_YESNOCANCEL | MB_ICONINFORMATION, L"Plugin updates");
        if (g_boxHook) { UnhookWindowsHookEx(g_boxHook); g_boxHook = nullptr; }
        std::vector<std::string> names;
        for (const browse::Entry& e : list) names.push_back(e.name);
        if (r == IDYES)
        {
            browse::InstallAll(names);
            g_pluginsInstalling = true;
            SetPluginStatus("updating " + std::to_string(names.size()) + " plugin(s)");
        }
        else if (r == IDNO)
        {
            for (const browse::Entry& e : list) WritePrivateProfileStringW(L"update", SkipKey(e.name).c_str(), Widen(e.version).c_str(), Cfg().iniPath.c_str());
            SetPluginStatus("skipped " + std::to_string(names.size()) + " plugin update(s)");
        }
        else SetPluginStatus("plugin updates declined for now");
    }

    void ReportPlugins()
    {
        g_pluginsInstalling = false;
        std::vector<std::string> lines = browse::TakeQueueReport();
        std::string t;
        for (const std::string& l : lines) t += l + "\n";
        SetPluginStatus("plugin updates done: " + std::to_string(lines.size()) + " result(s)");
        Box(t, MB_OK | MB_ICONINFORMATION, L"Plugin updates");
    }

    bool InstalledCopy()       // <game>\bin\plugins\RadiantPlugins.dll, a dev build elsewhere does not ask
    {
        std::wstring d = Cfg().dllDir;
        while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
        const wchar_t* tail = L"\\bin\\plugins";
        size_t n = wcslen(tail);
        return d.size() > n && _wcsicmp(d.c_str() + d.size() - n, tail) == 0;
    }
}

namespace update
{
    bool Enabled() { return GetPrivateProfileIntW(L"update", L"check", 1, Cfg().iniPath.c_str()) != 0; }
    void SetEnabled(bool on)
    {
        WritePrivateProfileStringW(L"update", L"check", on ? L"1" : L"0", Cfg().iniPath.c_str());
        LOG("update: release question %s", on ? "on" : "off");
    }
    // no key yet: follows [update] check, which switched both questions off before they were split
    bool PluginsEnabled() { return GetPrivateProfileIntW(L"update", L"plugins", Enabled() ? 1 : 0, Cfg().iniPath.c_str()) != 0; }
    void SetPluginsEnabled(bool on)
    {
        WritePrivateProfileStringW(L"update", L"plugins", on ? L"1" : L"0", Cfg().iniPath.c_str());
        LOG("update: plugin update question %s", on ? "on" : "off");
    }

    void StartupCheck()
    {
        if (g_startupDone) return;
        g_startupDone = true;
        // plugin updates: every app, dev builds too (only plugins installed from the list can be offered)
        if (!PluginsEnabled()) SetPluginStatus("switched off ([update] plugins=0)");
        else { g_pluginsPending = true; SetPluginStatus("loading the plugin list"); if (!browse::Loaded() && !browse::Loading()) browse::Refresh(); }
        // MTPlugins release: the launcher only, installed copies only unless [update] url= is set
        if (!app::IsLauncher()) { SetStatus("the release is checked in the launcher only"); return; }
        if (!Enabled()) { SetStatus("switched off ([update] check=0)"); return; }
        if (!InstalledCopy() && Source().empty()) { SetStatus("dev build (host not in <game>\\bin\\plugins): no check"); return; }
        StartCheck();
    }

    void CheckNow()
    {
        StartCheck();
        g_releaseAsked = false;
        g_manual = true;
        g_pluginsPending = true;
        browse::Refresh();
    }

    std::string PluginStatus()
    {
        std::string out = "plugins: " + g_pluginStatus;
        if (!browse::Loaded()) return out + (browse::Loading() ? " (list loading)" : " (no plugin list: " + browse::Message() + ")");
        for (const browse::Entry& e : PendingPlugins(true))
        {
            std::string mine = plugins::InstalledVersion(e.name);
            out += "\n" + e.name + " " + (mine.empty() ? "?" : mine) + " -> " + e.version + (Skipped(e) ? " (skipped)" : "");
        }
        return out;
    }

    void InstallPluginUpdates()
    {
        std::vector<std::string> names;
        for (const browse::Entry& e : PendingPlugins(true)) names.push_back(e.name);
        browse::InstallAll(names);
        SetPluginStatus("updating " + std::to_string(names.size()) + " plugin(s) (command channel)");
    }

    void Tick()
    {
        if (g_inTick) return;
        int ph = g_phase;
        g_inTick = true;
        try
        {
            if (ph == Found) Ask(); else if (ph == Downloaded) Launch(); else if (ph == DownloadError) DownloadFailed();
            else if (ph == Idle && g_pluginsPending && !browse::Loading() && ReadyToAsk())
            {
                g_pluginsPending = false;
                bool manual = g_manual;
                g_manual = false;
                std::vector<browse::Entry> list;
                if (browse::Loaded()) list = PendingPlugins(manual, true);
                if (g_releaseAsked && !manual) SetPluginStatus("not asked: the MTPlugins release question came first");
                else if (!list.empty()) AskPlugins(list);
                else
                {
                    SetPluginStatus(browse::Loaded() ? "up to date" : "no plugin list: " + browse::Message());
                    if (manual && !g_releaseAsked)
                    {
                        std::string rel;
                        { std::lock_guard<std::mutex> lk(g_mu); rel = g_releaseLine; }
                        std::string t = (rel.empty() ? "" : rel + "\n") +
                            (browse::Loaded() ? "Your plugins are up to date." : "Could not load the plugin list (" + browse::Message() + ").");
                        Box(t, MB_OK | MB_ICONINFORMATION, L"Check for updates");
                    }
                }
            }
            else if (g_pluginsInstalling && !browse::QueueBusy()) ReportPlugins();
        }
        catch (...) { g_phase = Idle; g_pluginsPending = false; g_pluginsInstalling = false; g_manual = false; }
        g_inTick = false;
    }

    std::string Status()
    {
        static const char* names[] = { "idle", "checking", "found", "asking", "downloading", "downloaded", "download failed" };
        std::lock_guard<std::mutex> lk(g_mu);
        return std::string("MTPlugins " MTPLUGINS_VERSION ", release question ") + (Enabled() ? "on" : "off") + ", plugin update question " + (PluginsEnabled() ? "on" : "off") + ", " + names[g_phase] + ": " + g_status;
    }
}
