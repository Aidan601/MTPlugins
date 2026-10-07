// RadiantPlugins_Launch.exe: starts or attaches to Radiant or APE and injects RadiantPlugins.dll (CreateRemoteThread + LoadLibraryW).
// Flags: --ape --attach --root --dll --plugins --install-plugins --install-loader --uninstall-loader --where, the rest goes to Radiant.
// Game folder: --root, else TA_TOOLS_PATH (process, then HKCU/HKLM), else [radiant] root= in RadiantPlugins.ini. Errors also show a message box.
#include <windows.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "version.lib")

namespace
{
    const wchar_t* kRadiantExe = L"Radiant_modtools.exe";
    const wchar_t* kApeExe = L"asseteditor_modtools.exe";
    const wchar_t* kLauncherExe = L"modlauncher.exe";
    const wchar_t* kExeName = kRadiantExe;                          // switched to APE by --ape
    const wchar_t* kAppName = L"Radiant";
    const wchar_t* kDllName = L"RadiantPlugins.dll";
    const wchar_t* kIniName = L"RadiantPlugins.ini";
    const wchar_t* kLoaderName = L"d3d9.dll";                       // the proxy Radiant imports (see loader\d3d9_proxy.cpp)
    const wchar_t* kLoaderIni = L"RadiantLoader.ini";
    const wchar_t* kLoaderTag = L"RadiantPlugins_Loader";           // InternalName in its VERSIONINFO
    const wchar_t* kTitle = L"Radiant Plugins launcher";

    std::wstring ExeDir()
    {
        wchar_t p[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, p, MAX_PATH * 2);
        PathRemoveFileSpecW(p);
        return p;
    }

    bool HasRadiant(const std::wstring& root)
    {
        return !root.empty() && PathFileExistsW((root + L"\\bin\\" + kRadiantExe).c_str());
    }

    std::wstring Trimmed(std::wstring s)
    {
        while (!s.empty() && (s.back() == L'\\' || s.back() == L'/' || iswspace(s.back()))) s.pop_back();
        size_t a = 0; while (a < s.size() && iswspace(s[a])) ++a;
        return s.substr(a);
    }

    // TA_TOOLS_PATH as saved by the mod tools installer (per user, else machine wide)
    std::wstring SavedToolsPath()
    {
        const struct { HKEY key; const wchar_t* sub; } places[] = {
            { HKEY_CURRENT_USER, L"Environment" },
            { HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment" } };
        for (const auto& pl : places)
        {
            wchar_t buf[MAX_PATH * 2] = {}; DWORD size = sizeof buf;
            if (RegGetValueW(pl.key, pl.sub, L"TA_TOOLS_PATH", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf, &size) == ERROR_SUCCESS && buf[0])
                return buf;
        }
        return L"";
    }

    std::wstring ResolveRoot(const std::wstring& override)
    {
        if (!override.empty()) return override;
        wchar_t env[MAX_PATH * 2] = {};
        if (GetEnvironmentVariableW(L"TA_TOOLS_PATH", env, MAX_PATH * 2) && env[0] && HasRadiant(Trimmed(env))) return Trimmed(env);
        std::wstring saved = Trimmed(SavedToolsPath());
        if (HasRadiant(saved)) return saved;
        wchar_t ini[MAX_PATH * 2] = {};
        GetPrivateProfileStringW(L"radiant", L"root", L"", ini, MAX_PATH * 2, (ExeDir() + L"\\" + kIniName).c_str());
        if (ini[0] && HasRadiant(Trimmed(ini))) return Trimmed(ini);
        return L"";
    }

    // Why ResolveRoot came back without Radiant, for every "not found" message
    std::wstring NotFoundHelp(const std::wstring& root)
    {
        return (root.empty() ? std::wstring(L"The Black Ops III mod tools were not found: the TA_TOOLS_PATH environment variable is not set.")
                             : L"Radiant was not found in " + root + L"\\bin.")
             + L"\n\nInstall the Black Ops III Mod Tools from Steam (their installer sets TA_TOOLS_PATH), or put the game folder in "
             + kIniName + L":\n[radiant]\nroot=C:\\...\\Call of Duty Black Ops III";
    }

    int Fail(int code, const std::wstring& msg)
    {
        wprintf(L"%s\n", msg.c_str());
        MessageBoxW(nullptr, msg.c_str(), kTitle, MB_OK | MB_ICONERROR);
        return code;
    }

    DWORD FindRadiantPid()
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        PROCESSENTRY32W pe = { sizeof(pe) };
        DWORD pid = 0;
        if (Process32FirstW(snap, &pe))
            do { if (_wcsicmp(pe.szExeFile, kExeName) == 0) { pid = pe.th32ProcessID; break; } } while (Process32NextW(snap, &pe));
        CloseHandle(snap);
        return pid;
    }

    // Path of a module with this file name already loaded in the process, or empty. Matches by name so a host
    // the d3d9 loader pulled in from elsewhere is found too (two hosts would fight over the hooks).
    std::wstring LoadedModulePath(DWORD pid, const wchar_t* fileName)
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE) return L"";
        MODULEENTRY32W me = { sizeof(me) };
        std::wstring found;
        if (Module32FirstW(snap, &me))
            do { if (_wcsicmp(me.szModule, fileName) == 0) { found = me.szExePath; break; } } while (Module32NextW(snap, &me));
        CloseHandle(snap);
        return found;
    }

    // Is this d3d9.dll ours? (never delete or overwrite someone else's: DXVK, ReShade, a wrapper)
    bool IsOurLoader(const std::wstring& path)
    {
        DWORD ignored = 0;
        DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
        if (!size) return false;
        std::vector<BYTE> buf(size);
        if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return false;
        void* value = nullptr; UINT len = 0;
        if (!VerQueryValueW(buf.data(), L"\\StringFileInfo\\040904b0\\InternalName", &value, &len) || !value) return false;
        return _wcsicmp(static_cast<const wchar_t*>(value), kLoaderTag) == 0;
    }

    bool Inject(HANDLE hProc, const std::wstring& dllPath)
    {
        SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
        void* remote = VirtualAllocEx(hProc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remote) { wprintf(L"VirtualAllocEx failed (%lu)\n", GetLastError()); return false; }
        if (!WriteProcessMemory(hProc, remote, dllPath.c_str(), bytes, nullptr)) { wprintf(L"WriteProcessMemory failed (%lu)\n", GetLastError()); return false; }

        auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        HANDLE th = CreateRemoteThread(hProc, nullptr, 0, loadLibrary, remote, 0, nullptr);
        if (!th) { wprintf(L"CreateRemoteThread failed (%lu)\n", GetLastError()); return false; }
        WaitForSingleObject(th, 30000);
        DWORD exitCode = 0; GetExitCodeThread(th, &exitCode);
        CloseHandle(th);
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        if (exitCode == 0) { wprintf(L"LoadLibraryW returned NULL inside the editor (missing dependency or bad path?)\n"); return false; }
        wprintf(L"Injected. Module handle: 0x%08X\n", exitCode);
        return true;
    }

    // Copy <src>\* into <dst>\ recursively, only missing or older files.
    int CopyNewer(const std::wstring& src, const std::wstring& dst, int& copied)
    {
        CreateDirectoryW(dst.c_str(), nullptr);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;
        int errors = 0;
        do
        {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring from = src + L"\\" + name, to = dst + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { errors += CopyNewer(from, to, copied); continue; }
            WIN32_FILE_ATTRIBUTE_DATA existing{};
            bool have = GetFileAttributesExW(to.c_str(), GetFileExInfoStandard, &existing) != 0;
            if (have && CompareFileTime(&existing.ftLastWriteTime, &fd.ftLastWriteTime) >= 0) continue;
            // never overwrite an existing plugin ini (user settings)
            if (have && name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".ini") == 0) continue;
            if (CopyFileW(from.c_str(), to.c_str(), FALSE)) { ++copied; wprintf(L"  %s\n", to.c_str()); }
            else { ++errors; wprintf(L"  FAILED %s (%lu)\n", to.c_str(), GetLastError()); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return errors;
    }
}

int wmain(int argc, wchar_t** argv)
{
    bool attach = false, where = false, installLoader = false, uninstallLoader = false, ape = false;
    std::wstring rootOverride, dllOverride, pluginsDir, installDir, forwarded;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring a = argv[i];
        if (a == L"--attach") attach = true;
        else if (a == L"--ape") { ape = true; kExeName = kApeExe; kAppName = L"APE"; }
        else if (a == L"--modlauncher") { ape = false; kExeName = kLauncherExe; kAppName = L"Mod Tools Launcher"; }
        else if (a == L"--where") where = true;
        else if (a == L"--root" && i + 1 < argc) rootOverride = argv[++i];
        else if (a == L"--dll" && i + 1 < argc) dllOverride = argv[++i];
        else if (a == L"--plugins" && i + 1 < argc) pluginsDir = argv[++i];
        else if (a == L"--install-plugins" && i + 1 < argc) installDir = argv[++i];
        else if (a == L"--install-loader") installLoader = true;
        else if (a == L"--uninstall-loader") uninstallLoader = true;
        else { if (!forwarded.empty()) forwarded += L" "; forwarded += L"\"" + a + L"\""; }
    }

    if (where)
    {
        std::wstring root = ResolveRoot(rootOverride);
        std::wstring loader = root + L"\\bin\\" + kLoaderName;
        std::wstring loaderState = !PathFileExistsW(loader.c_str()) ? L"not installed (run --install-loader)"
                                 : IsOurLoader(loader) ? L"installed: " + loader
                                 : L"a d3d9.dll that is NOT ours is in " + root + L"\\bin (DXVK or a wrapper?); --install-loader will refuse";
        std::wstring msg = HasRadiant(root) ? L"Radiant found: " + root + L"\\bin\\" + kRadiantExe + L"\nAPE found: " + (PathFileExistsW((root + L"\\bin\\" + kApeExe).c_str()) ? root + L"\\bin\\" + kApeExe : std::wstring(L"no")) + L"\nPlugins folder: " + root + L"\\bin\\plugins" + L"\nLoader: " + loaderState : NotFoundHelp(root);
        wprintf(L"%s\n", msg.c_str());
        return HasRadiant(root) ? 0 : 5;
    }
    if (installLoader || uninstallLoader)
    {
        std::wstring root = ResolveRoot(rootOverride);
        if (!HasRadiant(root)) return Fail(5, NotFoundHelp(root));
        std::wstring bin = root + L"\\bin";
        std::wstring dst = bin + L"\\" + kLoaderName;
        bool present = PathFileExistsW(dst.c_str()) != 0;
        if (present && !IsOurLoader(dst))
            return Fail(8, std::wstring(L"There is already a ") + kLoaderName + L" in " + bin + L" that is not ours\n(DXVK, ReShade or another wrapper?).\n\nLeaving it alone. Use the launcher instead.");

        if (uninstallLoader)
        {
            if (!present) { wprintf(L"Loader is not installed.\n"); return 0; }
            if (!DeleteFileW(dst.c_str()))
                return Fail(9, L"Could not delete " + dst + L" (" + std::to_wstring(GetLastError()) + L").\nClose Radiant and try again.");
            wprintf(L"Removed %s\nRadiant now loads plugins only when started from this launcher.\n", dst.c_str());
            return 0;
        }

        std::wstring src = ExeDir() + L"\\" + kLoaderName;
        if (!PathFileExistsW(src.c_str())) return Fail(2, std::wstring(L"Loader not found next to the launcher: ") + src);
        std::wstring host = dllOverride.empty() ? ExeDir() + L"\\" + kDllName : dllOverride;
        if (!PathFileExistsW(host.c_str())) return Fail(2, L"Plugin host not found: " + host);

        if (!CopyFileW(src.c_str(), dst.c_str(), FALSE))
            return Fail(7, L"Could not copy the loader to " + dst + L" (" + std::to_wstring(GetLastError()) + L").\nClose Radiant and try again.");
        std::wstring ini = bin + L"\\" + kLoaderIni;
        if (!PathFileExistsW(ini.c_str())) CopyFileW((ExeDir() + L"\\" + kLoaderIni).c_str(), ini.c_str(), FALSE);
        WritePrivateProfileStringW(L"loader", L"dll", host.c_str(), ini.c_str());
        WritePrivateProfileStringW(L"loader", L"enabled", L"1", ini.c_str());
        WritePrivateProfileStringW(L"loader", L"process", (std::wstring(kRadiantExe) + L"," + kApeExe + L"," + kLauncherExe).c_str(), ini.c_str());
        wprintf(L"Installed %s\n  host: %s\n  settings: %s\nRadiant and APE now load the plugin host however they are started (mod tools launcher included).\nRemove it again with --uninstall-loader.\n",
                dst.c_str(), host.c_str(), ini.c_str());
        return 0;
    }
    if (!installDir.empty())
    {
        std::wstring root = ResolveRoot(rootOverride);
        if (!HasRadiant(root)) return Fail(5, NotFoundHelp(root));
        wchar_t full[MAX_PATH * 2]; GetFullPathNameW(installDir.c_str(), MAX_PATH * 2, full, nullptr);
        std::wstring dst = root + L"\\bin\\plugins";
        wprintf(L"Installing plugins from %s to %s\n", full, dst.c_str());
        int copied = 0;
        int errors = CopyNewer(Trimmed(full), dst, copied);
        wprintf(L"%d file(s) copied, %d error(s)\n", copied, errors);
        if (errors) return Fail(7, L"Some plugin files could not be copied to " + dst + L".\nClose Radiant and try again, or copy the plugins folder there by hand.");
        return 0;
    }
    std::wstring dllPath = dllOverride.empty() ? ExeDir() + L"\\" + kDllName : dllOverride;
    if (!PathFileExistsW(dllPath.c_str())) return Fail(2, L"DLL not found: " + dllPath);
    if (!pluginsDir.empty())
    {
        wchar_t full[MAX_PATH * 2]; GetFullPathNameW(pluginsDir.c_str(), MAX_PATH * 2, full, nullptr);
        SetEnvironmentVariableW(L"RADIANT_PLUGINS_DIR", Trimmed(full).c_str());   // inherited by Radiant, read by the host
    }

    HANDLE hProc = nullptr;
    DWORD pid = 0;
    if (attach)
    {
        pid = FindRadiantPid();
        if (!pid) return Fail(3, std::wstring(kExeName) + L" is not running.");
        hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
        if (!hProc) return Fail(4, L"OpenProcess failed (" + std::to_wstring(GetLastError()) + L")");
    }
    else
    {
        std::wstring root = ResolveRoot(rootOverride);
        std::wstring bin = root + L"\\bin";
        std::wstring exe = bin + L"\\" + kExeName;
        if (!PathFileExistsW(exe.c_str())) return Fail(5, root.empty() ? NotFoundHelp(root) : std::wstring(kAppName) + L" not found: " + exe);

        // Radiant needs these to find its data
        wchar_t env[MAX_PATH * 2] = {};
        if (!GetEnvironmentVariableW(L"TA_TOOLS_PATH", env, MAX_PATH * 2) || !env[0]) SetEnvironmentVariableW(L"TA_TOOLS_PATH", (root + L"\\").c_str());
        if (!GetEnvironmentVariableW(L"TA_GAME_PATH", env, MAX_PATH * 2) || !env[0]) SetEnvironmentVariableW(L"TA_GAME_PATH", (root + L"\\").c_str());
        std::wstring cmd = L"\"" + exe + L"\"" + (forwarded.empty() ? L"" : L" " + forwarded);
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end()); cmdBuf.push_back(0);
        if (!CreateProcessW(exe.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, bin.c_str(), &si, &pi))
            return Fail(6, std::wstring(L"Could not start ") + kAppName + L" (" + std::to_wstring(GetLastError()) + L")");
        wprintf(L"Started %s (pid %lu), waiting for it to initialise...\n", kExeName, pi.dwProcessId);
        // Inject once the loader has run and the process is pumping messages (splash screen up).
        WaitForInputIdle(pi.hProcess, 60000);
        Sleep(500);
        hProc = pi.hProcess; pid = pi.dwProcessId;
        CloseHandle(pi.hThread);
    }

    // the d3d9 loader may have pulled the host in at process start already
    std::wstring loadedHost = LoadedModulePath(pid, kDllName);
    if (!loadedHost.empty())
    {
        wprintf(L"%s is already loaded in pid %lu (%s). Nothing to inject.\n", kDllName, pid, loadedHost.c_str());
        CloseHandle(hProc);
        return 0;
    }

    wprintf(L"Injecting %s into pid %lu\n", dllPath.c_str(), pid);
    bool ok = Inject(hProc, dllPath);
    CloseHandle(hProc);
    if (!ok) return Fail(1, std::wstring(L"Could not load ") + kDllName + L" into " + kAppName + L". It is running without plugins.\nSend RadiantPlugins.log (next to the launcher) to the developer.");
    return 0;
}
