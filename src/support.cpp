// MAP (support.h has the contract)
//   start guard ........ GuardKey, Alive, ReadGuard/WriteGuard, StartGuardBegin, SafeStart, NoteMapLoaded, Tick
//   problem report ..... CollectDumps, TailCopy, Summary, ReportWorker, Report
//   what's new ......... kNotes (the text per release and plugin), VersionLess, BuildWhatsNewHtml, ShowWhatsNew, MaybeShowWhatsNew
//   about .............. LogoHtml, BuildAboutHtml, ShowAbout
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include "support.h"
#include "plugins.h"
#include "config.h"
#include "app.h"
#include "mainthread.h"
#include "dock.h"
#include "qt.h"
#include "log.h"
#include "version.h"
#include "browse.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
// Common Controls 6 in this DLL's manifest (resource 2): the report's task dialog runs under it
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace
{
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
    std::string Escape(const std::string& s)
    {
        std::string o;
        for (char c : s) { if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '&') o += "&amp;"; else o += c; }
        return o;
    }
    const char* FriendlyApp() { return app::IsLauncher() ? "The Mod Tools Launcher" : app::IsApe() ? "APE" : "Radiant"; }

    // <game>\bin\plugins\RadiantPlugins.dll, a dev build elsewhere is not guarded and shows no What's new by itself
    bool InstalledCopy()
    {
        std::wstring d = Cfg().dllDir;
        while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
        const wchar_t* tail = L"\\bin\\plugins";
        size_t n = wcslen(tail);
        return d.size() > n && _wcsicmp(d.c_str() + d.size() - n, tail) == 0;
    }

    // ---- start guard -------------------------------------------------------------------------------------------
    // The host ini keeps, per application, how many starts in a row never settled and the pid of the start that has
    // not settled yet. A start whose pid is gone when the next one begins died (crash, freeze killed, closed early).
    std::atomic<bool> g_decided{ false };
    bool      g_guarded = false;     // this start is counted (installed copy or [startguard] enabled=1, not headless)
    bool      g_safe = false;
    int       g_failures = 0;
    bool      g_settled = false;
    ULONGLONG g_initAt = 0, g_mapAt = 0;

    const wchar_t* GuardKey() { return app::IsLauncher() ? L"launcher" : app::IsApe() ? L"ape" : L"radiant"; }

    std::wstring ImageName(HANDLE process)
    {
        wchar_t buf[MAX_PATH * 2]; DWORD n = MAX_PATH * 2;
        if (!QueryFullProcessImageNameW(process, 0, buf, &n)) return L"";
        std::wstring p(buf, n);
        size_t s = p.find_last_of(L"\\/");
        return s == std::wstring::npos ? p : p.substr(s + 1);
    }

    // the pid still runs our own executable (a reused pid of another program counts as gone)
    bool Alive(DWORD pid)
    {
        if (!pid || pid == GetCurrentProcessId()) return false;
        HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return false;
        bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT && _wcsicmp(ImageName(h).c_str(), ImageName(GetCurrentProcess()).c_str()) == 0;
        CloseHandle(h);
        return alive;
    }

    void ReadGuard(int& failures, DWORD& pid)
    {
        wchar_t buf[64] = {};
        GetPrivateProfileStringW(L"startguard", GuardKey(), L"", buf, 64, Cfg().iniPath.c_str());
        failures = 0; pid = 0;
        unsigned long p = 0;
        if (swscanf_s(buf, L"%d %lu", &failures, &p) < 1) failures = 0;
        if (failures < 0) failures = 0;
        pid = p;
    }
    void WriteGuard(int failures, DWORD pid)
    {
        wchar_t buf[64];
        _snwprintf_s(buf, _TRUNCATE, L"%d %lu", failures, static_cast<unsigned long>(pid));
        WritePrivateProfileStringW(L"startguard", GuardKey(), buf, Cfg().iniPath.c_str());
    }

    bool Headless()
    {
        std::wstring cl = GetCommandLineW();
        for (auto& c : cl) c = static_cast<wchar_t>(towlower(c));
        return cl.find(L" -led") != std::wstring::npos;     // -led / -ledSilent: the launcher's light bake, no window
    }

    // ---- problem report ----------------------------------------------------------------------------------------
    std::mutex  g_reportMu;
    std::string g_reportStatus = "no report made yet";
    std::atomic<bool> g_reportBusy{ false };
    std::atomic<int>  g_reportCopied{ 0 }, g_reportTotal{ 0 };   // files copied so far, files in the job
    std::atomic<bool> g_reportZipping{ false };

    // the worker's result for the progress dialog, under g_reportMu
    struct ReportResult { bool ready = false, failed = false; std::wstring path; std::string mb; };
    ReportResult g_reportResult;
    bool g_reportDialog = false;   // the progress dialog is open and shows the result itself

    struct FileItem { std::wstring src, dest; bool tail = false; };   // dest relative to the report folder
    struct ReportJob
    {
        std::wstring desktop, name;    // name = MTPlugins-report-<stamp>
        std::vector<FileItem> files;
        std::string summary;
        bool interactive = false;
    };

    void SetReportStatus(const std::string& s) { std::lock_guard<std::mutex> lk(g_reportMu); g_reportStatus = s; LOG("report: %s", s.c_str()); }

    struct Found { ULONGLONG time; uint64_t size; std::wstring path, file; };

    std::vector<Found> List(const std::wstring& dir, const wchar_t* pattern)
    {
        std::vector<Found> out;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return out;
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            Found f;
            f.time = (ULONGLONG(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
            f.size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            f.path = dir + L"\\" + fd.cFileName;
            f.file = fd.cFileName;
            out.push_back(f);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return out;
    }

    ULONGLONG NowFileTime() { FILETIME ft; GetSystemTimeAsFileTime(&ft); return (ULONGLONG(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }

    bool StartsWithI(const std::wstring& s, const wchar_t* prefix) { size_t n = wcslen(prefix); return s.size() >= n && _wcsnicmp(s.c_str(), prefix, n) == 0; }

    // crash dumps of the last 14 days: Radiant's/APE's own next to the exe, Windows Error Reporting's, the host's per plugin
    // (crash-*), Play Test's of a hung game (hang-*)
    std::vector<Found> CollectDumps(std::string& notes)
    {
        const ULONGLONG maxAge = 14ull * 24 * 3600 * 10000000ull;
        const uint64_t  maxSize = 300ull << 20;
        ULONGLONG now = NowFileTime();
        std::vector<Found> all;
        for (Found& f : List(plugins::RadiantRoot() + L"\\bin", L"*.dmp")) all.push_back(f);
        wchar_t local[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH))
            for (Found& f : List(std::wstring(local) + L"\\CrashDumps", L"*.dmp"))
                for (const wchar_t* p : { L"Radiant_modtools", L"asseteditor_modtools", L"modlauncher", L"BlackOps3", L"cod2map64", L"linker_modtools" })
                    if (StartsWithI(f.file, p)) { all.push_back(f); break; }
        for (ModToolsPlugin* p : plugins::All())
            for (const wchar_t* pat : { L"crash-*.dmp", L"hang-*.dmp" })      // plugin faults, Play Test's game hangs
                for (Found& f : List(p->dir, pat)) { f.file = Widen(p->name) + L"-" + f.file; all.push_back(f); }
        std::vector<Found> keep;
        std::sort(all.begin(), all.end(), [](const Found& a, const Found& b) { return a.time > b.time; });
        for (const Found& f : all)
        {
            if (f.size == 0 || now - f.time > maxAge) continue;
            if (f.size > maxSize) { notes += "  not included (too big, " + std::to_string(f.size >> 20) + " MB, send it yourself if asked): " + Narrow(f.path) + "\n"; continue; }
            if (keep.size() < 6) keep.push_back(f);
        }
        return keep;
    }

    void MakeDirs(const std::wstring& path)
    {
        for (size_t i = 3; i <= path.size(); ++i)
            if (i == path.size() || path[i] == L'\\') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    }

    // the last `limit` bytes of a file another process may hold open for writing (the logs)
    bool TailCopy(const std::wstring& src, const std::wstring& dst, uint64_t limit)
    {
        HANDLE in = CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (in == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        GetFileSizeEx(in, &size);
        if (uint64_t(size.QuadPart) > limit) { LARGE_INTEGER off; off.QuadPart = size.QuadPart - LONGLONG(limit); SetFilePointerEx(in, off, nullptr, FILE_BEGIN); }
        HANDLE out = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (out == INVALID_HANDLE_VALUE) { CloseHandle(in); return false; }
        std::vector<char> buf(1 << 20);
        DWORD got = 0, put = 0;
        bool ok = true;
        while (ReadFile(in, buf.data(), DWORD(buf.size()), &got, nullptr) && got)
            if (!WriteFile(out, buf.data(), got, &put, nullptr) || put != got) { ok = false; break; }
        CloseHandle(out); CloseHandle(in);
        return ok;
    }

    void DeleteTree(const std::wstring& dir)
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
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(p);
                else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(p.c_str()); }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(dir.c_str());
    }

    // Windows 10 1803 and later ship bsdtar, which writes zip files with -a and a .zip name
    bool Zip(const std::wstring& parent, const std::wstring& folder, const std::wstring& zip)
    {
        wchar_t sys[MAX_PATH] = {};
        GetSystemDirectoryW(sys, MAX_PATH);
        std::wstring tar = std::wstring(sys) + L"\\tar.exe";
        if (GetFileAttributesW(tar.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
        std::wstring cmd = L"\"" + tar + L"\" -a -c -f \"" + zip + L"\" -C \"" + parent + L"\" \"" + folder + L"\"";
        STARTUPINFOW si{ sizeof si };
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, parent.c_str(), &si, &pi)) return false;
        CloseHandle(pi.hThread);
        DWORD code = 1;
        if (WaitForSingleObject(pi.hProcess, 10 * 60 * 1000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
        else TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        return code == 0 && GetFileAttributesW(zip.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    const wchar_t* kIssuesUrl = L"https://github.com/Aidan601/MTPlugins/issues";

    void ShowSavedBox(const std::wstring& path, const std::string& mb)
    {
        std::string t = std::string("Saved on your Desktop: ") + Narrow(path.substr(path.find_last_of(L'\\') + 1)) + " (" + mb + " MB).\n\n"
                        "Attach it to a new issue at " + Narrow(kIssuesUrl) + " and say what you did just before the problem.";
        std::wstring arg = L"/select,\"" + path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
        MessageBoxW(mt::MainWindow(), Widen(t).c_str(), L"MTPlugins", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
    }

    void ReportFinished(bool interactive, bool failed, const std::wstring& path, const std::string& mb)
    {
        if (!interactive) return;
        bool dialog;
        {
            std::lock_guard<std::mutex> lk(g_reportMu);
            g_reportResult.ready = true; g_reportResult.failed = failed; g_reportResult.path = path; g_reportResult.mb = mb;
            dialog = g_reportDialog;
        }
        if (dialog || failed) return;
        mt::Post([path, mb]() { ShowSavedBox(path, mb); });
    }

    void ReportDone(bool interactive, const std::wstring& result, bool zipped, uint64_t bytes)
    {
        std::string mb = std::to_string((bytes + (1 << 19)) >> 20);
        SetReportStatus(std::string(zipped ? "saved " : "saved (folder, zip failed) ") + Narrow(result) + ", " + mb + " MB");
        ReportFinished(interactive, false, result, mb);
    }

    DWORD WINAPI ReportWorker(void* arg)
    {
        ReportJob* job = static_cast<ReportJob*>(arg);
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&ReportWorker), &self);
        try
        {
            std::wstring dir = job->desktop + L"\\" + job->name;
            MakeDirs(dir);
            std::string done;
            uint64_t bytes = 0;
            for (const FileItem& f : job->files)
            {
                std::wstring dst = dir + L"\\" + f.dest;
                MakeDirs(dst.substr(0, dst.find_last_of(L'\\')));
                bool ok = f.tail ? TailCopy(f.src, dst, 16ull << 20) : CopyFileW(f.src.c_str(), dst.c_str(), FALSE) != FALSE;
                WIN32_FILE_ATTRIBUTE_DATA fa;
                if (ok && GetFileAttributesExW(dst.c_str(), GetFileExInfoStandard, &fa)) bytes += (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
                done += std::string(ok ? "  " : "  FAILED ") + Narrow(f.dest) + " <- " + Narrow(f.src) + "\n";
                ++g_reportCopied;
            }
            g_reportZipping = true;
            std::string text = job->summary + "\nFiles in this report:\n" + done;
            HANDLE h = CreateFileW((dir + L"\\report.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) { DWORD put; WriteFile(h, text.data(), DWORD(text.size()), &put, nullptr); CloseHandle(h); }
            std::wstring zip = dir + L".zip";
            if (Zip(job->desktop, job->name, zip))
            {
                DeleteTree(dir);
                WIN32_FILE_ATTRIBUTE_DATA fa{};
                GetFileAttributesExW(zip.c_str(), GetFileExInfoStandard, &fa);
                ReportDone(job->interactive, zip, true, (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow);
            }
            else ReportDone(job->interactive, dir, false, bytes);
        }
        catch (...) { SetReportStatus("failed (internal error)"); ReportFinished(job->interactive, true, L"", ""); }
        delete job;
        g_reportBusy = false;
        if (self) FreeLibraryAndExitThread(self, 0);
        return 0;
    }

    std::string FileStamp(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return "missing";
        FILETIME lt; SYSTEMTIME st;
        FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lt); FileTimeToSystemTime(&lt, &st);
        char b[96];
        _snprintf_s(b, _TRUNCATE, "%04d-%02d-%02d %02d:%02d, %llu bytes", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                    (static_cast<unsigned long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow);
        return b;
    }

    std::string WindowsVersion()
    {
        using RtlGetVersion_t = LONG(WINAPI*)(OSVERSIONINFOW*);
        auto fn = reinterpret_cast<RtlGetVersion_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        OSVERSIONINFOW v{ sizeof v };
        if (!fn || fn(&v) != 0) return "unknown";
        return std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + "." + std::to_string(v.dwBuildNumber);
    }

    std::string Summary(const std::string& dumpNotes)
    {
        SYSTEMTIME st; GetLocalTime(&st);
        char when[64];
        _snprintf_s(when, _TRUNCATE, "%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        wchar_t exe[MAX_PATH * 2] = {}; GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
        std::wstring host = Cfg().dllDir + L"\\RadiantPlugins.dll";
        std::string s = "MTPlugins problem report\n";
        s += std::string("Created: ") + when + "\n";
        s += "MTPlugins: " MTPLUGINS_VERSION + std::string(InstalledCopy() ? "" : " (dev build)") + "\n";
        s += "Host: " + Narrow(host) + " (" + FileStamp(host) + ")\n";
        s += std::string("Made in: ") + app::Name() + ", " + Narrow(exe) + " (" + FileStamp(exe) + ")\n";
        s += "Windows: " + WindowsVersion() + "\n";
        s += "Plugins folder: " + Narrow(plugins::Root()) + "\n";
        s += "Start guard: " + support::StartGuardStatus() + "\n";
        s += "\nPlugins (name, version, state here, enabled, editors, error):\n";
        for (ModToolsPlugin* p : plugins::All())
            s += "  " + p->name + (p->displayName.empty() || p->displayName == p->name ? "" : " (" + p->displayName + ")") + ", " + (p->version.empty() ? "?" : p->version) + ", "
                 + plugins::StateName(p->state) + ", " + (p->enabled ? "on" : "off") + ", " + plugins::AppNames(p->apps) + (p->error.empty() ? "" : ", " + p->error) + "\n";
        if (!dumpNotes.empty()) s += "\nCrash dumps:\n" + dumpNotes;
        return s;
    }

    // ---- what's new --------------------------------------------------------------------------------------------
    // Add a block per release, newest first: what is new in each plugin of ours, shown in this order when it is loaded here.
    // Every release newer than the one this application last showed is listed. A null plugin is the Plugins window line.
    // An empty entry hides a plugin with nothing new. Plugins with no entry follow, listed with their own description.
    struct Note { const char* release; const char* plugin; uint32_t apps; const char* html; };
    const uint32_t kAllApps = MODTOOLS_APP_RADIANT | MODTOOLS_APP_APE | MODTOOLS_APP_LAUNCHER;
    const Note kNotes[] = {
        // ---- 1.1.2 ----
        { "1.1.2", nullptr, MODTOOLS_APP_LAUNCHER,
          "<b>Updates</b><br>Updating MTPlugins now installs without the Setup wizard and reopens the launcher. Your plugin settings are kept and new ones are added." },
        { "1.1.2", "RadiantPlus", MODTOOLS_APP_RADIANT,
          "<b>Radiant+</b><ul>"
          "<li>Auto LODs from meshoptimizer are now off by default, as they can look worse on custom models. A fix is in the works. Until then, <code>meshopt_lods=1</code> under <code>[linker]</code> in LauncherPlus.ini turns them on.</li>"
          "<li>Light bakes on big maps are less likely to run out of memory.</li></ul>" },
        { "1.1.2", "APEPlus", MODTOOLS_APP_APE,
          "<b>APE+</b><ul>"
          "<li>APE's internal check boxes no longer close APE. Choose Yes to keep working and save your changes.</li>"
          "<li>Fixed a Preview window check failing on animations with bad rotations.</li>"
          "<li>Fixed the type filter showing the wrong assets when extra asset types are installed.</li>"
          "<li>Auto LODs from meshoptimizer are now off by default, matching Launcher+.</li></ul>" },
        { "1.1.2", "LauncherPlus", MODTOOLS_APP_LAUNCHER,
          "<b>Launcher+</b><ul>"
          "<li>Auto LODs from meshoptimizer are now off by default, as they can look worse on custom models. A fix is in the works. Until then, <code>meshopt_lods=1</code> under <code>[linker]</code> in LauncherPlus.ini turns them on.</li>"
          "<li>The Light step now says when Radiant closes during a bake, and when the PC ran out of memory.</li></ul>" },
        // ---- 1.1.1 ----
        { "1.1.1", nullptr, kAllApps,
          "<b>Plugins</b> (Ctrl+Shift+P)<br>MTPlugins' own plugins now update from here too, without running Setup again." },
        { "1.1.1", "RadiantPlus", MODTOOLS_APP_RADIANT,
          "<b>Radiant+</b><ul>"
          "<li>FX Browser previews are now experimental and off by default. Tick Previews (Experimental) at the bottom of the FX Browser to turn them on.</li>"
          "<li>Fixed auto LODs failing on some models with \"Produced invalid normal\" or \"Produced out of bounds vertex\".</li></ul>" },
        { "1.1.1", "APEPlus", MODTOOLS_APP_APE,
          "<b>APE+</b><ul>"
          "<li>Fixed auto LODs failing on some models with \"Produced invalid normal\" or \"Produced out of bounds vertex\".</li></ul>" },
        { "1.1.1", "LauncherPlus", MODTOOLS_APP_LAUNCHER,
          "<b>Launcher+</b><ul>"
          "<li>Fixed auto LODs failing on some models with \"Produced out of bounds vertex\".</li></ul>" },
        // ---- 1.1.0 ----
        { "1.1.0", nullptr, kAllApps,
          "<b>Plugins</b> (Ctrl+Shift+P)<br>Search your plugins, and get asked when one you installed from Browse plugins has an update." },
        { "1.1.0", "RadiantPlus", MODTOOLS_APP_RADIANT,
          "<b>Radiant+</b><ul>"
          "<li>FX Browser can display previews of each effect, mirroring the Models and Textures windows.</li>"
          "<li>Selected models now show as an outline instead of the wireframe. Can be toggled with Alt+J or View > Toggle Model Outline.</li>"
          "<li>Icon Size sliders no longer cause lag in the Models, Textures and FX Browser windows, and the icons resize as you drag.</li>"
          "<li>The color dialog takes hex, 0-1 and Kelvin values, copies the color and keeps your recent colors.</li>"
          "<li>Auto LODs match the ones Launcher+ builds.</li>"
          "<li>The rotation gizmo now lines up correctly in game view.</li>"
          "<li>Fixed issue where camera view would have artifacts on graphics cards with low video memory.</li></ul>" },
        { "1.1.0", "TeamCreate", MODTOOLS_APP_RADIANT,
          "<b>Team Create</b> (Ctrl+Shift+T)<ul>"
          "<li>Assets already in the map now stream to everyone who joins, even on big maps.</li>"
          "<li>Join Host waits while the host reads a big build for the first time instead of giving up.</li></ul>" },
        { "1.1.0", "PlayTest", MODTOOLS_APP_RADIANT, "" },
        { "1.1.0", "APEPlus", MODTOOLS_APP_APE,
          "<b>APE+</b><ul>"
          "<li>Speed<ul>"
          "<li>No more freeze when opening the first material after APE starts.</li>"
          "<li>XModel Info is much faster on big models.</li></ul></li>"
          "<li>Editing<ul>"
          "<li>Sound notetracks play in the Preview window, and alias file edits are picked up live.</li>"
          "<li>Viewmodel animations no longer look stretched in the Preview window.</li>"
          "<li>Select Color takes hex, 0-1 and Kelvin values and copies the color.</li>"
          "<li>Previewed auto LODs match the ones Launcher+ builds.</li></ul></li></ul>" },
        { "1.1.0", "LauncherPlus", MODTOOLS_APP_LAUNCHER,
          "<b>Launcher+</b><ul>"
          "<li>Better auto LODs, made much faster and keeping their shape.</li>"
          "<li>Auto LODs for models over the stock 220,000-face limit.</li>"
          "<li>Starting the mod tools from Steam no longer leaves a cmd window open.</li></ul>" },
    };

    void* g_whatsNew = nullptr;          // QDialog, built on first use, hidden between uses

    // Plugins whose version changed since this application last showed What's new (ours and community plugins alike;
    // seen versions in the host ini as [whatsnew] <app>.<Name>=<version>). Computed once per session.
    struct Change { std::string name, from, to, text; };
    std::vector<Change> g_changes;
    std::string g_since;                 // the release this application showed last, empty = only the current release

    // "1.1.0" < "1.1.1" < "1.2.0", missing parts count as 0
    bool VersionLess(const std::string& a, const std::string& b)
    {
        int x[3] = {}, y[3] = {};
        sscanf_s(a.c_str(), "%d.%d.%d", &x[0], &x[1], &x[2]);
        sscanf_s(b.c_str(), "%d.%d.%d", &y[0], &y[1], &y[2]);
        return std::lexicographical_compare(x, x + 3, y, y + 3);
    }

    std::string DisplayOf(const std::string& name)
    {
        ModToolsPlugin* p = plugins::Find(name);
        return p && !p->displayName.empty() ? p->displayName : name;
    }

    // MTPlugins-logo.png next to the host DLL (480 wide, shown unscaled), empty when it is missing
    std::string LogoHtml()
    {
        const std::wstring path = Cfg().dllDir + L"\\MTPlugins-logo.png";
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return "";
        std::string src = Narrow(path);
        for (char& c : src) if (c == '\\') c = '/';
        return "<p align=\"center\"><img src=\"" + Escape(src) + "\"></p>";
    }

    std::string BuildWhatsNewHtml()
    {
        const std::string logo = LogoHtml();
        std::string h = logo.empty() ? "<h3>MTPlugins " MTPLUGINS_VERSION "</h3>" : logo + "<h3 align=\"center\">Version " MTPLUGINS_VERSION "</h3>";
        const char* where = app::IsLauncher() ? "the Mod Tools Launcher" : app::IsApe() ? "APE" : "Radiant";
        if (!g_changes.empty())
        {
            h += std::string("<h4>New and updated since you last opened ") + (app::IsLauncher() ? "the launcher" : where) + "</h4><ul>";
            for (const Change& c : g_changes)
                h += "<li><b>" + Escape(DisplayOf(c.name)) + "</b> " + (c.from.empty() ? "new, " + Escape(c.to) : Escape(c.from) + " to " + Escape(c.to))
                     + (c.text.empty() ? "" : ": " + Escape(c.text)) + "</li>";
            h += "</ul>";
        }
        h += std::string("<p>What is new in the plugins for ") + where + ".</p>";
        auto ours = [](ModToolsPlugin* p) {
            for (const Note& n : kNotes) if (n.plugin && _stricmp(n.plugin, p->name.c_str()) == 0 && (n.apps & app::ApiId())) return true;
            return false;
        };
        auto listed = [](const char* release) {
            return strcmp(release, MTPLUGINS_VERSION) == 0 || (!g_since.empty() && VersionLess(g_since, release) && VersionLess(release, MTPLUGINS_VERSION));
        };
        int shown = 0;
        const char* release = nullptr;
        for (const Note& n : kNotes)              // ours first, newest release first, in kNotes order
        {
            if (!*n.html || !(n.apps & app::ApiId()) || !listed(n.release)) continue;
            bool loaded = !n.plugin;
            for (ModToolsPlugin* p : plugins::All())
                if (n.plugin && p->state == ModToolsPlugin::Loaded && _stricmp(n.plugin, p->name.c_str()) == 0) loaded = true;
            if (!loaded) continue;
            if (!release || strcmp(release, n.release) != 0)
            {
                if (release || strcmp(n.release, MTPLUGINS_VERSION) != 0) h += std::string("<h4>New in ") + n.release + "</h4>";
                release = n.release;
            }
            h += std::string("<p>") + n.html + "</p>";
            if (n.plugin) ++shown;
        }
        for (ModToolsPlugin* p : plugins::All())   // then everyone else's, with their own description
        {
            if (p->state != ModToolsPlugin::Loaded || ours(p)) continue;
            h += "<p><b>" + Escape(p->displayName.empty() ? p->name : p->displayName) + "</b>" + (p->version.empty() ? "" : " " + Escape(p->version))
                 + (p->description.empty() ? "" : ": " + Escape(p->description)) + "</p>";
            ++shown;
        }
        if (!shown) h += "<p>Nothing new in the plugins loaded here.</p>";
        h += "<p>Open this again from Plugins > What's new.</p>";
        return h;
    }

    void CloseWhatsNew(void*, void**) { if (g_whatsNew) qt::SetVisible(g_whatsNew, false); }

    void* g_about = nullptr;             // QDialog, built on first use, hidden between uses

    // ---- theme link colour: the same rule as APE+'s palette Link role, from the active sheet's "palette:" header ----
    bool SheetColor(const std::string& sheet, const char* key, int& rgb)
    {
        const std::string tag = std::string(" ") + key + "=#";
        const size_t at = sheet.find(tag);
        if (at == std::string::npos || at > 4096 || at + tag.size() + 6 > sheet.size()) return false;
        char* end = nullptr;
        const long v = strtol(sheet.c_str() + at + tag.size(), &end, 16);
        if (end != sheet.c_str() + at + tag.size() + 6) return false;
        rgb = int(v);
        return true;
    }
    double Luminance(int rgb)            // WCAG relative luminance
    {
        double l = 0; const double k[3] = { 0.2126, 0.7152, 0.0722 };
        for (int i = 0; i < 3; ++i) { double c = ((rgb >> (16 - 8 * i)) & 255) / 255.0; l += k[i] * (c <= 0.03928 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4)); }
        return l;
    }
    double Contrast(int a, int b) { double la = Luminance(a) + 0.05, lb = Luminance(b) + 0.05; return la > lb ? la / lb : lb / la; }
    int Mix(int rgb, int to, double t) { int out = 0; for (int s = 0; s < 24; s += 8) out |= int(((rgb >> s) & 255) * (1 - t) + ((to >> s) & 255) * t + 0.5) << s; return out; }
    // link= from the sheet, else the accent pulled toward white (dark themes) or black until it reads at 4.5:1, -1 = Qt's own
    int ThemeLinkColor()
    {
        const std::string sheet = qt::AppStyleSheet();
        int rgb, bg, accent, accent2;
        if (SheetColor(sheet, "link", rgb)) return rgb;
        if (!SheetColor(sheet, "bg", bg) || !SheetColor(sheet, "accent", accent)) return -1;
        if (!SheetColor(sheet, "accent2", accent2)) accent2 = accent;
        rgb = Luminance(bg) < 0.18 ? accent2 : accent;
        const int to = Luminance(bg) < 0.18 ? 0xffffff : 0x000000;
        for (int i = 0; i < 20 && Contrast(rgb, bg) < 4.5; ++i) rgb = Mix(rgb, to, 0.1);
        return rgb;
    }
    std::string Link(const char* url, const char* text, int rgb)
    {
        char color[48] = "";
        if (rgb >= 0) snprintf(color, sizeof color, "<span style=\"color:#%06x\">", rgb);
        return std::string("<a href=\"") + url + "\">" + color + text + (rgb >= 0 ? "</span>" : "") + "</a>";
    }

    std::string BuildAboutHtml()
    {
        std::string h = LogoHtml();
        h += "<p align=\"center\"><b>MTPlugins " MTPLUGINS_VERSION "</b><br>Plugins for the Black Ops III mod tools: Radiant, APE and the Mod Tools Launcher.</p>";
        h += "<p align=\"center\">By Figglebottom. Free software under the GNU General Public License v3.</p>";
        const int link = ThemeLinkColor();
        h += "<p align=\"center\">" + Link("https://github.com/Aidan601/MTPlugins", "GitHub", link) + " &nbsp;|&nbsp; "
             + Link("https://github.com/Aidan601/MTPlugins/releases", "Releases", link) + " &nbsp;|&nbsp; "
             + Link("https://github.com/Aidan601/MTPlugins/issues", "Report an issue", link) + " &nbsp;|&nbsp; "
             + Link("https://github.com/Aidan601/MTPlugins-Index", "Plugin list", link) + "</p>";
        std::string loaded;
        for (ModToolsPlugin* p : plugins::All())
        {
            if (p->state != ModToolsPlugin::Loaded) continue;
            loaded += std::string(loaded.empty() ? "" : "<br>") + Escape(p->displayName.empty() ? p->name : p->displayName)
                      + (p->version.empty() ? "" : " " + Escape(p->version));
        }
        const char* where = app::IsLauncher() ? "the Mod Tools Launcher" : app::IsApe() ? "APE" : "Radiant";
        h += std::string("<p align=\"center\"><b>Loaded in ") + where + "</b><br>" + (loaded.empty() ? "none" : loaded) + "</p>";
        return h;
    }

    void CloseAbout(void*, void**) { if (g_about) qt::SetVisible(g_about, false); }
}

namespace support
{
    // ---- start guard -------------------------------------------------------------------------------------------
    void StartGuardBegin()
    {
        int on = GetPrivateProfileIntW(L"startguard", L"enabled", -1, Cfg().iniPath.c_str());
        g_guarded = (on < 0 ? InstalledCopy() : on != 0) && !Headless() && app::Current() != app::Kind::Unknown;
        if (!g_guarded) { g_decided = true; return; }
        int failures; DWORD pid;
        ReadGuard(failures, pid);
        if (pid && Alive(pid)) LOG("startguard: the start of pid %lu is still running; not counted", pid);
        else if (pid) ++failures;
        g_failures = failures;
        WriteGuard(failures, GetCurrentProcessId());
        LOG("startguard: %d start(s) in a row did not settle", failures);
        if (failures >= 2)
        {
            std::string q = std::string(FriendlyApp()) + " stopped before it finished starting the last " + std::to_string(failures) + " times "
                            "(a crash, a freeze, or it was closed early). A plugin may be the cause.\n\n"
                            "Yes: start without plugins this time. Your plugin settings are kept. To find the plugin that causes it, open "
                            "Plugins (Ctrl+Shift+P), select a plugin and click Reload to load it, one at a time.\n\n"
                            "No: load the plugins as usual.\n\n"
                            "To report it: Plugins > Report a problem... collects the logs and crash dumps into a zip.";
            int r = MessageBoxW(nullptr, Widen(q).c_str(), L"MTPlugins", MB_YESNO | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
            g_safe = r == IDYES;
            LOG("startguard: asked; %s", g_safe ? "starting without plugins" : "loading the plugins as usual");
        }
        g_decided = true;
    }

    bool StartDecided() { return g_decided; }
    bool SafeStart() { return g_safe; }
    void NoteMapLoaded() { if (!g_mapAt) g_mapAt = GetTickCount64(); }

    void Tick()
    {
        if (!plugins::Initialized()) return;
        ULONGLONG now = GetTickCount64();
        if (!g_initAt) g_initAt = now;
        if (g_guarded && !g_settled)
        {
            ULONGLONG up = now - g_initAt;
            bool settled = app::IsLauncher() ? up > 15000 : app::IsApe() ? up > 45000 : (g_mapAt && now - g_mapAt > 10000) || up > 120000;
            if (settled)
            {
                g_settled = true;
                int f; DWORD pid;
                ReadGuard(f, pid);
                WriteGuard(0, pid == GetCurrentProcessId() ? 0 : pid);   // another start since ours keeps its pid
                LOG("startguard: start settled");
            }
        }
        if (now - g_initAt > 3000) MaybeShowWhatsNew();
    }

    std::string StartGuardStatus()
    {
        if (!g_guarded) return Headless() ? "off (headless)" : "off (dev build; [startguard] enabled=1 turns it on)";
        return std::to_string(g_failures) + " unsettled start(s) before this one, " + (g_settled ? "this start settled" : "this start not settled yet")
               + (g_safe ? ", started without plugins" : "");
    }

    // ---- problem report ----------------------------------------------------------------------------------------
    namespace
    {
        using TaskDialogIndirectFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);

        struct ProgressState { TASKDIALOGCONFIG done = { sizeof(TASKDIALOGCONFIG) }; std::wstring doneText, path; bool finished = false, zipping = false; int shown = -1; };

        HRESULT CALLBACK ProgressProc(HWND hwnd, UINT msg, WPARAM, LPARAM lp, LONG_PTR ref)
        {
            ProgressState* st = reinterpret_cast<ProgressState*>(ref);
            if (msg == TDN_HYPERLINK_CLICKED)
            {
                const wchar_t* href = reinterpret_cast<const wchar_t*>(lp);
                if (href && !wcscmp(href, L"file"))
                {
                    std::wstring arg = L"/select,\"" + st->path + L"\"";
                    ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
                }
                else if (href) ShellExecuteW(nullptr, L"open", href, nullptr, nullptr, SW_SHOWNORMAL);
                return S_OK;
            }
            if (msg != TDN_TIMER || st->finished) return S_OK;
            ReportResult r;
            { std::lock_guard<std::mutex> lk(g_reportMu); r = g_reportResult; }
            if (r.ready)
            {
                st->finished = true; st->path = r.path;
                TASKDIALOGCONFIG& d = st->done;
                d.hwndParent = mt::MainWindow(); d.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
                d.dwCommonButtons = TDCBF_OK_BUTTON; d.pszWindowTitle = L"MTPlugins";
                d.pfCallback = ProgressProc; d.lpCallbackData = ref;
                if (r.failed)
                {
                    d.pszMainIcon = TD_ERROR_ICON; d.pszMainInstruction = L"The problem report could not be saved";
                    std::lock_guard<std::mutex> lk(g_reportMu);
                    st->doneText = L"Report " + Widen(g_reportStatus) + L". RadiantPlugins.log has the details.";
                }
                else
                {
                    d.pszMainIcon = TD_INFORMATION_ICON; d.pszMainInstruction = L"Problem report saved";
                    st->doneText = L"Saved on your Desktop: <a href=\"file\">" + r.path.substr(r.path.find_last_of(L'\\') + 1) + L"</a> (" + Widen(r.mb) + L" MB)\n\n"
                                   L"Attach it to a new issue at <a href=\"" + std::wstring(kIssuesUrl) + L"\">github.com/Aidan601/MTPlugins/issues</a> "
                                   L"and say what you did just before the problem.";
                }
                d.pszContent = st->doneText.c_str();
                SendMessageW(hwnd, TDM_NAVIGATE_PAGE, 0, reinterpret_cast<LPARAM>(&d));
                return S_OK;
            }
            if (g_reportZipping && !st->zipping)
            {
                st->zipping = true;
                SendMessageW(hwnd, TDM_SET_MARQUEE_PROGRESS_BAR, TRUE, 0);
                SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 30);
                SendMessageW(hwnd, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, reinterpret_cast<LPARAM>(L"Zipping..."));
            }
            else if (!st->zipping && g_reportCopied != st->shown)
            {
                st->shown = g_reportCopied;
                const int total = (std::max)(1, int(g_reportTotal));
                SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_POS, st->shown * 100 / total, 0);
                std::wstring t = L"Copying files (" + std::to_wstring(st->shown) + L" of " + std::to_wstring(total) + L")";
                SendMessageW(hwnd, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, reinterpret_cast<LPARAM>(t.c_str()));
            }
            return S_OK;
        }

        // Calls show(TaskDialogIndirect) under this DLL's manifest context (comctl32 v6). False when no task dialog could be shown.
        template <class F> bool WithTaskDialog(F&& show)
        {
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&ProgressProc), &self);
            ACTCTXW ctx = { sizeof ctx }; ctx.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID; ctx.hModule = self; ctx.lpResourceName = MAKEINTRESOURCEW(2);
            HANDLE act = self ? CreateActCtxW(&ctx) : INVALID_HANDLE_VALUE; ULONG_PTR cookie = 0;
            bool shown = false;
            if (act != INVALID_HANDLE_VALUE && ActivateActCtx(act, &cookie))
            {
                HMODULE cc = LoadLibraryW(L"comctl32.dll");   // v6 under this context, kept loaded
                auto fn = cc ? reinterpret_cast<TaskDialogIndirectFn>(GetProcAddress(cc, "TaskDialogIndirect")) : nullptr;
                if (fn) shown = show(fn);
                DeactivateActCtx(0, cookie);
            }
            if (act != INVALID_HANDLE_VALUE) ReleaseActCtx(act);
            return shown;
        }

        // Progress, then the result with links. False when no task dialog could be shown.
        bool ShowProgressDialog()
        {
            ProgressState st;
            const bool shown = WithTaskDialog([&](TaskDialogIndirectFn fn)
                {
                    TASKDIALOGCONFIG td = { sizeof td };
                    td.hwndParent = mt::MainWindow();
                    td.dwFlags = TDF_SHOW_PROGRESS_BAR | TDF_CALLBACK_TIMER | TDF_ALLOW_DIALOG_CANCELLATION | TDF_ENABLE_HYPERLINKS;
                    td.dwCommonButtons = TDCBF_CLOSE_BUTTON; td.pszWindowTitle = L"MTPlugins"; td.pszMainIcon = TD_INFORMATION_ICON;
                    td.pszMainInstruction = L"Saving problem report"; td.pszContent = L"Copying files";
                    td.pszFooter = L"Closing this window does not stop the report.";
                    td.pfCallback = ProgressProc; td.lpCallbackData = reinterpret_cast<LONG_PTR>(&st);
                    return SUCCEEDED(fn(&td, nullptr, nullptr, nullptr));
                });
            // closed before the result, or no dialog: the plain box shows it when the worker ends
            std::lock_guard<std::mutex> lk(g_reportMu);
            g_reportDialog = false;
            if (!st.finished && g_reportResult.ready && !g_reportResult.failed)
            {
                std::wstring path = g_reportResult.path; std::string mb = g_reportResult.mb;
                mt::Post([path, mb]() { ShowSavedBox(path, mb); });
            }
            return shown;
        }
    }

    void Report(bool ask)
    {
        if (g_reportBusy) { if (ask) MessageBoxW(mt::MainWindow(), L"A report is being made already.", L"MTPlugins", MB_OK | MB_ICONINFORMATION); return; }
        if (ask)
        {
            const wchar_t* q = L"Logs, settings, crash dumps and your recent editor actions are saved as a zip on your Desktop.\n\n"
                               L"Nothing is sent. It may contain your Windows user name and map names, so check it before posting it publicly.";
            int choice = 0;
            const bool shown = WithTaskDialog([&](TaskDialogIndirectFn fn)
                {
                    TASKDIALOGCONFIG td = { sizeof td };
                    td.hwndParent = mt::MainWindow(); td.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
                    td.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON; td.pszWindowTitle = L"MTPlugins"; td.pszMainIcon = TD_INFORMATION_ICON;
                    td.pszMainInstruction = L"Save a problem report?"; td.pszContent = q;
                    return SUCCEEDED(fn(&td, &choice, nullptr, nullptr));
                });
            if (!shown)
            {
                const std::wstring box = std::wstring(L"Save a problem report?\n\n") + q;
                choice = MessageBoxW(mt::MainWindow(), box.c_str(), L"MTPlugins", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
            }
            if (choice != IDYES) return;
        }
        ReportJob* job = new ReportJob;
        job->interactive = ask;
        PWSTR desk = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk)) && desk) job->desktop = desk;
        if (desk) CoTaskMemFree(desk);
        if (job->desktop.empty()) { delete job; SetReportStatus("failed: no Desktop folder"); return; }
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t name[64];
        _snwprintf_s(name, _TRUNCATE, L"MTPlugins-report-%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        job->name = name;

        const std::wstring& host = Cfg().dllDir;
        for (const wchar_t* f : { L"RadiantPlugins.log", L"RadiantPlugins.ape.log", L"RadiantPlugins.launcher.log" })
            if (GetFileAttributesW((host + L"\\" + f).c_str()) != INVALID_FILE_ATTRIBUTES) job->files.push_back({ host + L"\\" + f, std::wstring(L"host\\") + f, true });
        job->files.push_back({ Cfg().iniPath, L"host\\RadiantPlugins.ini", false });
        std::wstring bin = plugins::RadiantRoot() + L"\\bin";
        for (const wchar_t* f : { L"RadiantLoader.log", L"RadiantLoader.ini" })
            if (GetFileAttributesW((bin + L"\\" + f).c_str()) != INVALID_FILE_ATTRIBUTES) job->files.push_back({ bin + L"\\" + f, std::wstring(L"loader\\") + f, true });
        if (GetFileAttributesW((plugins::Root() + L"\\plugins.ini").c_str()) != INVALID_FILE_ATTRIBUTES)
            job->files.push_back({ plugins::Root() + L"\\plugins.ini", L"plugins\\plugins.ini", false });
        for (ModToolsPlugin* p : plugins::All())
            for (const wchar_t* pat : { L"*.ini", L"*.log" })
                for (const Found& f : List(p->dir, pat))
                    job->files.push_back({ f.path, L"plugins\\" + Widen(p->name) + L"\\" + f.file, true });
        std::string notes;
        for (const Found& f : CollectDumps(notes)) job->files.push_back({ f.path, L"dumps\\" + f.file, false });
        job->summary = Summary(notes);

        g_reportBusy = true;
        g_reportCopied = 0; g_reportTotal = int(job->files.size()); g_reportZipping = false;
        { std::lock_guard<std::mutex> lk(g_reportMu); g_reportResult = ReportResult(); g_reportDialog = ask; }
        SetReportStatus("collecting " + std::to_string(job->files.size()) + " file(s)");
        HANDLE t = CreateThread(nullptr, 0, ReportWorker, job, 0, nullptr);
        if (t) CloseHandle(t);
        else { delete job; g_reportBusy = false; SetReportStatus("failed: no worker thread"); return; }
        if (ask) ShowProgressDialog();
    }

    std::string ReportStatus() { std::lock_guard<std::mutex> lk(g_reportMu); return std::string(g_reportBusy ? "busy: " : "") + g_reportStatus; }

    // ---- what's new --------------------------------------------------------------------------------------------
    void ShowWhatsNew()
    {
        if (!qt::Bind()) return;
        void* mw = dock::MainWindowWidget();
        if (!g_whatsNew)
        {
            g_whatsNew = qt::Dialog("What's new in MTPlugins", mw);
            void* outer = qt::VBox(g_whatsNew);
            qt::LayoutMargins(outer, 8, 8, 8, 8);
            void* content = qt::Widget(nullptr);
            void* box = qt::VBox(content);
            qt::LayoutMargins(box, 4, 4, 4, 4);
            void* label = qt::Label(BuildWhatsNewHtml().c_str(), content);
            qt::SetObjectName(label, "MTPluginsWhatsNewText");
            qt::SetWordWrap(label, true);
            qt::BoxAddWidget(box, label);
            qt::BoxAddStretch(box);
            void* scroll = qt::ScrollArea(content, g_whatsNew);
            if (scroll) qt::BoxAddWidget(outer, scroll, 1);
            else { qt::SetParent(content, g_whatsNew); qt::BoxAddWidget(outer, content, 1); }
            void* row = qt::HBox();
            qt::BoxAddStretch(row);
            void* close = qt::PushButton("Close", g_whatsNew);
            qt::Connect(close, qt::Signal::ButtonClicked, CloseWhatsNew, nullptr);
            qt::BoxAddWidget(row, close);
            qt::BoxAddLayout(outer, row);
            int w = 560, h = 520;
            RECT r{}; HWND main = mt::MainWindow();
            if (main && GetWindowRect(main, &r)) qt::Move(g_whatsNew, (r.left + r.right - w) / 2, (r.top + r.bottom - h) / 2);
            qt::Resize(g_whatsNew, w, h);
        }
        else if (void* label = qt::FindChildByName(g_whatsNew, "MTPluginsWhatsNewText")) qt::SetLabelText(label, BuildWhatsNewHtml().c_str());
        qt::SetVisible(g_whatsNew, true);
        qt::Raise(g_whatsNew);
        qt::ActivateWindow(g_whatsNew);
    }

    // ---- about -------------------------------------------------------------------------------------------------
    void ShowAbout()
    {
        if (!qt::Bind()) return;
        if (!g_about)
        {
            g_about = qt::Dialog("About MTPlugins", dock::MainWindowWidget());
            void* outer = qt::VBox(g_about);
            qt::LayoutMargins(outer, 12, 12, 12, 12);
            qt::LayoutFixedSize(outer);  // the smallest size that fits, not resizable
            void* label = qt::Label(BuildAboutHtml().c_str(), g_about);
            qt::SetObjectName(label, "MTPluginsAboutText");
            qt::BoxAddWidget(outer, label, 1);
            void* row = qt::HBox();
            qt::BoxAddStretch(row);
            void* close = qt::PushButton("Close", g_about);
            qt::Connect(close, qt::Signal::ButtonClicked, CloseAbout, nullptr);
            qt::BoxAddWidget(row, close);
            qt::BoxAddLayout(outer, row);
        }
        else if (void* label = qt::FindChildByName(g_about, "MTPluginsAboutText")) qt::SetLabelText(label, BuildAboutHtml().c_str());
        qt::SetVisible(g_about, true);
        // centred in the work area of the screen that holds the main window
        HWND self = reinterpret_cast<HWND>(qt::WinId(g_about));
        RECT r{};
        MONITORINFO mi{ sizeof mi };
        HWND main = mt::MainWindow();
        if (self && GetWindowRect(self, &r) && GetMonitorInfoW(MonitorFromWindow(main ? main : self, MONITOR_DEFAULTTONEAREST), &mi))
        {
            const RECT& wa = mi.rcWork;
            qt::Move(g_about, (wa.left + wa.right - (r.right - r.left)) / 2, (wa.top + wa.bottom - (r.bottom - r.top)) / 2);
        }
        qt::Raise(g_about);
        qt::ActivateWindow(g_about);
    }

    void MaybeShowWhatsNew()
    {
        static bool done = false;
        if (done) return;
        if (g_safe) { done = true; return; }
        const std::wstring& ini = Cfg().iniPath;
        int dev = GetPrivateProfileIntW(L"whatsnew", L"dev", 0, ini.c_str());
        if (!InstalledCopy() && !dev) { done = true; return; }
        // the plugin list (fetched at start) carries each version's one-line `changes`, wait for it up to 20 s
        if (browse::Loading() && g_initAt && GetTickCount64() - g_initAt < 20000) return;
        done = true;
        const wchar_t* key = GuardKey();
        wchar_t seen[64] = {};
        GetPrivateProfileStringW(L"whatsnew", key, L"", seen, 64, ini.c_str());
        bool firstRun = !seen[0];                  // the overview lists every plugin, nothing is "new" yet
        bool release = Narrow(seen) != MTPLUGINS_VERSION;
        if (release) g_since = Narrow(seen);
        for (ModToolsPlugin* p : plugins::All())
        {
            if (p->state != ModToolsPlugin::Loaded || p->version.empty()) continue;
            std::wstring pk = std::wstring(key) + L"." + Widen(p->name);
            wchar_t was[64] = {};
            GetPrivateProfileStringW(L"whatsnew", pk.c_str(), L"", was, 64, ini.c_str());
            if (Narrow(was) == p->version) continue;
            WritePrivateProfileStringW(L"whatsnew", pk.c_str(), Widen(p->version).c_str(), ini.c_str());
            if (firstRun) continue;
            Change c{ p->name, Narrow(was), p->version, "" };
            c.text = browse::ChangesOf(p->name, p->version);
            if (c.text.empty() && c.from.empty()) c.text = p->description;
            g_changes.push_back(c);
        }
        if (!release && g_changes.empty()) return;
        WritePrivateProfileStringW(L"whatsnew", key, Widen(MTPLUGINS_VERSION).c_str(), ini.c_str());   // before showing: never twice
        LOG("whatsnew: %s with " MTPLUGINS_VERSION " (last seen '%S'), %zu plugin(s) new or updated", app::Name(), seen, g_changes.size());
        ShowWhatsNew();
    }
}
