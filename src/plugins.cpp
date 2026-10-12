#include <windows.h>
#include <dbghelp.h>
#include <algorithm>
#include <cstring>
#include <functional>
#include "MinHook.h"
#include "plugins.h"
#include "hostapi.h"
#include "pluginmgr.h"
#include "browse.h"
#include "update.h"
#include "support.h"
#include "hooks.h"
#include "mainthread.h"
#include "dock.h"
#include "qt.h"
#include "ui.h"
#include "config.h"
#include "app.h"
#include "ape.h"
#include "log.h"

namespace
{
    std::vector<ModToolsPlugin*> g_plugins;
    std::wstring g_root, g_radiantRoot, g_iniPath;
    bool     g_initialized = false;
    unsigned g_version = 1;

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
    std::string Lower(std::string s) { for (auto& c : s) c = char(tolower(static_cast<unsigned char>(c))); return s; }
    bool IEquals(const std::string& a, const std::string& b) { return Lower(a) == Lower(b); }
    // the Website link goes to ShellExecute: keep only a plain web address
    std::string SafeUrl(const char* u)
    {
        std::string s = u ? u : "";
        std::string l = Lower(s);
        if (l.rfind("https://", 0) != 0 && l.rfind("http://", 0) != 0) return "";
        for (char c : s) if (static_cast<unsigned char>(c) <= ' ' || static_cast<unsigned char>(c) > '~' || c == '"') return "";   // printable ASCII only
        return s;
    }

    void ResolveRoots()
    {
        wchar_t exe[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
        std::wstring p = exe;
        size_t s1 = p.find_last_of(L"\\/");
        std::wstring bin = s1 == std::wstring::npos ? L"." : p.substr(0, s1);
        size_t s2 = bin.find_last_of(L"\\/");
        g_radiantRoot = s2 == std::wstring::npos ? bin : bin.substr(0, s2);

        wchar_t env[MAX_PATH * 2] = {};
        if (GetEnvironmentVariableW(L"RADIANT_PLUGINS_DIR", env, MAX_PATH * 2) && env[0]) g_root = env;
        else if (!Cfg().pluginsDir.empty()) g_root = Cfg().pluginsDir;
        else g_root = bin + L"\\plugins";
        while (!g_root.empty() && (g_root.back() == L'\\' || g_root.back() == L'/')) g_root.pop_back();
        g_iniPath = g_root + L"\\plugins.ini";
    }

    bool ValidName(const std::string& n)
    {
        if (n.empty() || n.size() > 64) return false;
        for (char c : n) if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') return false;
        return true;
    }

    bool ReadEnabled(const std::string& name)
    {
        int v = GetPrivateProfileIntW(Widen(name).c_str(), L"enabled", -1, g_iniPath.c_str());
        if (v < 0) return Cfg().autoloadMissing;
        return v != 0;
    }
    bool WriteEnabled(const std::string& name, bool on)
    {
        CreateDirectoryW(g_root.c_str(), nullptr);
        return WritePrivateProfileStringW(Widen(name).c_str(), L"enabled", on ? L"1" : L"0", g_iniPath.c_str()) != FALSE;
    }

    // The apps mask is cached in plugins.ini (apps=<mask> <DLL write time>), a rebuilt DLL invalidates it
    std::wstring DllStamp(const std::wstring& dll)
    {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExW(dll.c_str(), GetFileExInfoStandard, &fa)) return L"";
        wchar_t buf[32]; _snwprintf_s(buf, _TRUNCATE, L"%08X%08X", fa.ftLastWriteTime.dwHighDateTime, fa.ftLastWriteTime.dwLowDateTime);
        return buf;
    }
    uint32_t ReadCachedApps(const ModToolsPlugin* p)
    {
        wchar_t buf[64] = {};
        GetPrivateProfileStringW(Widen(p->name).c_str(), L"apps", L"", buf, 64, g_iniPath.c_str());
        unsigned mask = 0; wchar_t stamp[33] = {};
        if (swscanf_s(buf, L"%u %32s", &mask, stamp, 33u) != 2) return 0;
        return DllStamp(p->dllPath) == stamp ? mask : 0;
    }
    void WriteCachedApps(ModToolsPlugin* p, uint32_t mask)
    {
        p->apps = mask;
        std::wstring v = std::to_wstring(mask) + L" " + DllStamp(p->dllPath);
        wchar_t cur[64] = {};
        GetPrivateProfileStringW(Widen(p->name).c_str(), L"apps", L"", cur, 64, g_iniPath.c_str());
        if (v != cur) WritePrivateProfileStringW(Widen(p->name).c_str(), L"apps", v.c_str(), g_iniPath.c_str());
    }
    // Same for the version (version=<DLL write time> <version>)
    std::string CachedVersion(const std::wstring& name, const std::wstring& dll)
    {
        wchar_t buf[128] = {};
        GetPrivateProfileStringW(name.c_str(), L"version", L"", buf, 128, g_iniPath.c_str());
        std::wstring v = buf, stamp = DllStamp(dll);
        size_t sp = v.find(L' ');
        if (stamp.empty() || sp == std::wstring::npos || v.substr(0, sp) != stamp) return "";
        return Narrow(v.substr(sp + 1));
    }
    void WriteCachedVersion(const ModToolsPlugin* p)
    {
        if (p->version.empty() || p->version.size() > 64) return;
        std::wstring v = DllStamp(p->dllPath) + L" " + Widen(p->version);
        wchar_t cur[128] = {};
        GetPrivateProfileStringW(Widen(p->name).c_str(), L"version", L"", cur, 128, g_iniPath.c_str());
        if (v != cur) WritePrivateProfileStringW(Widen(p->name).c_str(), L"version", v.c_str(), g_iniPath.c_str());
    }
    void ApplyCachedApps(ModToolsPlugin* p)
    {
        if (p->module) return;
        std::string ver = CachedVersion(Widen(p->name), p->dllPath);
        if (!ver.empty()) p->version = ver;
        p->apps = ReadCachedApps(p);
        if (p->apps && !(p->apps & uint32_t(app::ApiId())))
        {
            p->state = ModToolsPlugin::OtherApp;
            p->error = "Runs in " + plugins::AppNames(p->apps) + "; shared toggle applies there.";
        }
        else if (p->state == ModToolsPlugin::OtherApp) { p->state = p->enabled ? ModToolsPlugin::Discovered : ModToolsPlugin::Disabled; p->error.clear(); }
    }

    ModToolsPlugin* Register(const std::string& name, const std::wstring& dir, const std::wstring& dll)
    {
        for (ModToolsPlugin* p : g_plugins) if (IEquals(p->name, name)) { p->dir = dir; p->dllPath = dll; ApplyCachedApps(p); return p; }
        auto* p = new ModToolsPlugin;
        p->name = name; p->prefix = "[" + name + "]"; p->dir = dir; p->dllPath = dll;
        p->iniPath = dir + L"\\" + Widen(name) + L".ini";
        p->enabled = ReadEnabled(name);
        p->state = p->enabled ? ModToolsPlugin::Discovered : ModToolsPlugin::Disabled;
        ApplyCachedApps(p);
        g_plugins.push_back(p);
        return p;
    }

    // Renamed plugins: the old folder is never loaded when the new one exists, its settings are carried over once
    // (ini with the [section] renamed, themes, DXVK copy, enabled state)
    struct Renamed { const wchar_t* from; const wchar_t* to; };
    const Renamed kRenamed[] = { { L"BetterRadiant", L"RadiantPlus" }, { L"BetterAPE", L"APEPlus" }, { L"BetterBuild", L"LauncherPlus" },
                                 { L"BuildPlus", L"LauncherPlus" } };

    // Plugin builds made for another host table layout: refused so they cannot call the wrong host functions
    struct Stale { const char* name; const char* version; };
    const Stale kStale[] = { { "VisionEditor", "1.0.1" } };
    bool StaleBuild(const std::string& name, const char* version)
    {
        for (const Stale& s : kStale) if (version && IEquals(name, s.name) && strcmp(version, s.version) == 0) return true;
        return false;
    }

    bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
    const Renamed* ReplacedBy(const std::wstring& name)          // non-null when `name` is an old name whose new DLL is here
    {
        for (const Renamed& r : kRenamed)
            if (_wcsicmp(name.c_str(), r.from) == 0 && Exists(g_root + L"\\" + r.to + L"\\" + r.to + L".dll")) return &r;
        return nullptr;
    }
    void CopyMissing(const std::wstring& from, const std::wstring& to)   // recursive, never overwrites
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((from + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        CreateDirectoryW(to.c_str(), nullptr);
        do
        {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) CopyMissing(from + L"\\" + n, to + L"\\" + n);
            else CopyFileW((from + L"\\" + n).c_str(), (to + L"\\" + n).c_str(), TRUE);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    void MigrateRenamed()
    {
        for (const Renamed& r : kRenamed)
        {
            std::wstring oldDir = g_root + L"\\" + r.from, newDir = g_root + L"\\" + r.to;
            std::wstring marker = newDir + L"\\.migrated-from-" + r.from;
            if (!Exists(oldDir) || !Exists(newDir + L"\\" + r.to + L".dll") || Exists(marker)) continue;
            // the old ini replaces the new template, section renamed
            std::string ini;
            HANDLE f = CreateFileW((oldDir + L"\\" + r.from + L".ini").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE)
            {
                LARGE_INTEGER size{}; GetFileSizeEx(f, &size);
                ini.resize(size_t(size.QuadPart < (1 << 20) ? size.QuadPart : 0));
                DWORD got = 0;
                if (!ini.empty() && (!ReadFile(f, &ini[0], DWORD(ini.size()), &got, nullptr) || got != ini.size())) ini.clear();
                CloseHandle(f);
            }
            if (!ini.empty())
            {
                std::string from = "[" + Lower(Narrow(r.from)) + "]", to = "[" + Lower(Narrow(r.to)) + "]";
                for (size_t at = 0; (at = Lower(ini).find(from, at)) != std::string::npos; at += to.size()) ini.replace(at, from.size(), to);
                HANDLE o = CreateFileW((newDir + L"\\" + r.to + L".ini").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                DWORD wrote = 0;
                if (o != INVALID_HANDLE_VALUE) { WriteFile(o, ini.data(), DWORD(ini.size()), &wrote, nullptr); CloseHandle(o); }
            }
            CopyMissing(oldDir + L"\\themes", newDir + L"\\themes");   // custom .qss files
            CopyMissing(oldDir + L"\\dxvk", newDir + L"\\dxvk");       // RadiantPlus's DXVK copy
            // enabled state, when the new name has none
            if (int(GetPrivateProfileIntW(r.to, L"enabled", -1, g_iniPath.c_str())) < 0)   // returns UINT: -1 only through int
            {
                int old = int(GetPrivateProfileIntW(r.from, L"enabled", -1, g_iniPath.c_str()));
                if (old >= 0) WritePrivateProfileStringW(r.to, L"enabled", old ? L"1" : L"0", g_iniPath.c_str());
            }
            HANDLE m = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
            if (m != INVALID_HANDLE_VALUE) CloseHandle(m);
            LOGW(L"plugins: %s was renamed %s: settings carried over, the old folder is no longer loaded (delete it when no editor runs)", r.from, r.to);
        }
    }

    void Discover()
    {
        MigrateRenamed();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((g_root + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) { LOGW(L"plugins: folder '%s' not found (nothing to load)", g_root.c_str()); return; }
        do
        {
            std::wstring fn = fd.cFileName;
            if (fn == L"." || fn == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                std::wstring dll = g_root + L"\\" + fn + L"\\" + fn + L".dll";
                if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
                std::string name = Narrow(fn);
                if (const Renamed* r = ReplacedBy(fn))
                {
                    // never registered, or unloaded for good if registered before its successor appeared
                    for (ModToolsPlugin* p : g_plugins)
                        if (IEquals(p->name, name) && !p->replaced)
                        {
                            p->replaced = true; plugins::Unload(p);
                            p->state = ModToolsPlugin::Disabled; p->error = "Replaced by " + Narrow(r->to); ++g_version;
                        }
                    continue;
                }
                if (ValidName(name)) Register(name, g_root + L"\\" + fn, dll);
            }
            else if (fn.size() > 4 && _wcsicmp(fn.c_str() + fn.size() - 4, L".dll") == 0)
            {
                std::string name = Narrow(fn.substr(0, fn.size() - 4));
                if (ValidName(name) && !IEquals(name, "RadiantPlugins")) Register(name, g_root, g_root + L"\\" + fn);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // ---- crash isolation ------------------------------------------------------------------
    struct SehCtx { void (*fn)(void*); void* arg; DWORD code; wchar_t dumpPath[MAX_PATH]; bool dumped; DWORD dumpError; };

    void WriteDump(EXCEPTION_POINTERS* ep, SehCtx* c)
    {
        HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
        if (!dbg) return;
        using Fn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
        auto write = reinterpret_cast<Fn>(GetProcAddress(dbg, "MiniDumpWriteDump"));
        if (!write) return;
        HANDLE f = CreateFileW(c->dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        MINIDUMP_EXCEPTION_INFORMATION mei{ GetCurrentThreadId(), ep, FALSE };
        c->dumped = write(GetCurrentProcess(), GetCurrentProcessId(), f, MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs), &mei, nullptr, nullptr) != FALSE;
        if (!c->dumped) c->dumpError = GetLastError();
        CloseHandle(f);
        // a failed MiniDumpWriteDump can leave a large unreadable file (no stream directory): remove it
        if (!c->dumped) DeleteFileW(c->dumpPath);
    }

    LONG Filter(EXCEPTION_POINTERS* ep, SehCtx* c)
    {
        if (!Cfg().isolate) return EXCEPTION_CONTINUE_SEARCH;      // let Radiant crash normally (its own .dmp is more useful)
        c->code = ep->ExceptionRecord->ExceptionCode;
        WriteDump(ep, c);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // no C++ objects with destructors in here (C2712)
    bool RunSeh(SehCtx* c)
    {
        __try { c->fn(c->arg); return true; }
        __except (Filter(GetExceptionInformation(), c)) { return false; }
    }

    // ---- unload helpers -----------------------------------------------------------------------
    void ReleaseResources(ModToolsPlugin* p)
    {
        ui::UnregisterTag(p);
        mt::DiscardTag(p);
        for (void* f : p->filters) qt::DisarmEventFilter(f);
        for (void* s : p->slots) qt::DisarmSlot(s);
        // one thread freeze for all of them (MH_DisableHook freezes every thread per call), removing a disabled hook needs none
        for (void* t : p->detours) MH_QueueDisableHook(t);
        if (!p->detours.empty()) MH_ApplyQueued();
        for (void* t : p->detours) MH_RemoveHook(t);
        p->filters.clear(); p->slots.clear(); p->detours.clear();
        for (void* c : p->callbacks) free(c);
        p->callbacks.clear();
        hostapi::ReleasePanes(p);
        hostapi::ReleaseMenus(p);
        hostapi::ReleaseDialogs(p);
        p->inhibitReload = false;
    }

    // A plugin detour entered from Radiant is not counted by p->calls. If Radiant's original pumps messages a
    // disable can arrive under it and FreeLibrary would unmap the hook's return address.
    // Walks this thread's return addresses: is any inside the module?
    bool PluginCodeOnStack(HMODULE m)
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m);
        if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(m) + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        const ULONG_PTR imageLo = reinterpret_cast<ULONG_PTR>(m);
        const ULONG_PTR imageHi = imageLo + nt->OptionalHeader.SizeOfImage;
        ULONG_PTR stackLo = 0, stackHi = 0;
        GetCurrentThreadStackLimits(&stackLo, &stackHi);

        CONTEXT ctx;
        RtlCaptureContext(&ctx);
        for (int depth = 0; depth < 512; ++depth)
        {
            if (ctx.Rip >= imageLo && ctx.Rip < imageHi) return true;
            DWORD64 base = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
            if (!fn)
            {
                // no unwind data (a stub): scan the rest of the stack for a pointer into the module
                // (a stale match only defers the unload to the next tick)
                for (const ULONG_PTR* slot = reinterpret_cast<const ULONG_PTR*>(ctx.Rsp);
                     slot < reinterpret_cast<const ULONG_PTR*>(stackHi); ++slot)
                    if (*slot >= imageLo && *slot < imageHi) return true;
                return false;
            }
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
            if (!ctx.Rip || ctx.Rsp < stackLo || ctx.Rsp >= stackHi) break;
        }
        return false;
    }

    ModToolsPluginEntryFn FindEntry(HMODULE m)
    {
        return reinterpret_cast<ModToolsPluginEntryFn>(GetProcAddress(m, "ModToolsPluginEntry"));
    }

    // A plugin built against the old SDK exports RadiantPluginEntry: not loaded, the Plugins window says why
    std::string NoEntryReason(HMODULE m)
    {
        return GetProcAddress(m, "RadiantPluginEntry") ? "built with the old SDK (RadiantPluginEntry): rebuild it against modtools_plugin.h"
                                                       : "no ModToolsPluginEntry export";
    }

    struct EntryCall { ModToolsPluginEntryFn entry; ModToolsPlugin* self; const ModToolsPluginApi* result; };
    void CallEntry(void* a) { auto* c = static_cast<EntryCall*>(a); c->result = c->entry(c->self, hostapi::Table()); }
    struct IntCall { int (*fn)(void); int result; };
    void CallInt(void* a) { auto* c = static_cast<IntCall*>(a); c->result = c->fn(); }
    void CallVoid(void* a) { reinterpret_cast<void (*)(void)>(a)(); }
    struct WstrCall { void (*fn)(const wchar_t*); const wchar_t* s; };
    void CallWstr(void* a) { auto* c = static_cast<WstrCall*>(a); c->fn(c->s); }
    struct SavedCall { void (*fn)(const wchar_t*, int, int); const wchar_t* s; int a, b; };
    void CallSaved(void* a) { auto* c = static_cast<SavedCall*>(a); c->fn(c->s, c->a, c->b); }
    struct ScopeCall { void (*fn)(const wchar_t*, int); const wchar_t* s; int d; };
    void CallScope(void* a) { auto* c = static_cast<ScopeCall*>(a); c->fn(c->s, c->d); }
    struct DrawCall { void (*fn)(const ModToolsDrawContext*); const ModToolsDrawContext* ctx; };
    void CallDraw(void* a) { auto* c = static_cast<DrawCall*>(a); c->fn(c->ctx); }
    struct CmdCall { const char* (*fn)(const char*); const char* cmd; const char* result; };
    void CallCmd(void* a) { auto* c = static_cast<CmdCall*>(a); c->result = c->fn(c->cmd); }

    std::vector<ModToolsPlugin*> LoadedSnapshot()
    {
        std::vector<ModToolsPlugin*> v;
        for (ModToolsPlugin* p : g_plugins) if (p->state == ModToolsPlugin::Loaded) v.push_back(p);
        return v;
    }
}

namespace plugins
{
    const std::wstring& Root() { return g_root; }
    const std::wstring& RadiantRoot() { return g_radiantRoot; }
    bool Initialized() { return g_initialized; }
    unsigned Version() { return g_version; }

    std::string InstalledVersion(const std::string& name)
    {
        if (ModToolsPlugin* p = Find(name)) if (p->module && !p->version.empty()) return p->version;
        std::wstring w = Widen(name);
        return CachedVersion(w, g_root + L"\\" + w + L"\\" + w + L".dll");
    }
    std::vector<ModToolsPlugin*> All() { return g_plugins; }

    const char* StateName(ModToolsPlugin::State s)
    {
        switch (s)
        {
        case ModToolsPlugin::Discovered: return "Not loaded";
        case ModToolsPlugin::Disabled: return "Disabled";
        case ModToolsPlugin::Loaded: return "Loaded";
        case ModToolsPlugin::Failed: return "Failed";
        case ModToolsPlugin::Incompatible: return "Incompatible";
        case ModToolsPlugin::OtherApp: return "Other application";
        }
        return "?";
    }

    bool ForThisEditor(const ModToolsPlugin* p)
    {
        return p->state != ModToolsPlugin::OtherApp && (!p->apps || (p->apps & uint32_t(app::ApiId())));
    }

    ModToolsPlugin* Find(const std::string& name)
    {
        for (ModToolsPlugin* p : g_plugins) if (IEquals(p->name, name)) return p;
        return nullptr;
    }

    std::string AppNames(uint32_t apps)
    {
        if (!apps) return "Unknown (not probed)";
        std::string s;
        if (apps & MODTOOLS_APP_LAUNCHER) s = "Launcher";
        if (apps & MODTOOLS_APP_APE) { if (!s.empty()) s += ", "; s += "APE"; }
        if (apps & MODTOOLS_APP_RADIANT) { if (!s.empty()) s += ", "; s += "Radiant"; }
        return s.empty() ? "Unsupported application" : s;
    }

    // Deferred by Unload: the plugin's frame is off the stack now
    static void FinishPendingUnload(ModToolsPlugin* p)
    {
        if (!p->unloadPending) return;
        p->unloadPending = false;
        Unload(p);
        if (p->enabled) Load(p);           // a 'plugins reload' from inside the call
    }

    // Load() and Unload() call into the plugin (entry, OnLoad, OnUnload). A disable arriving under one must not be
    // finished by Call() (FreeLibrary mid-frame): they mark the plugin busy and finish the unload themselves.
    struct Lifecycle
    {
        ModToolsPlugin* p;
        explicit Lifecycle(ModToolsPlugin* q) : p(q) { p->lifecycle = true; }
        ~Lifecycle() { p->lifecycle = false; if (p->calls == 0) FinishPendingUnload(p); }
    };

    bool Call(ModToolsPlugin* p, const char* what, void (*fn)(void*), void* arg)
    {
        SehCtx c{ fn, arg, 0, {}, false, 0 };
        SYSTEMTIME st; GetLocalTime(&st);
        _snwprintf_s(c.dumpPath, _TRUNCATE, L"%s\\crash-%04d%02d%02d-%02d%02d%02d.dmp", p->dir.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        ++p->calls;
        const bool ok = RunSeh(&c);
        --p->calls;
        if (p->calls == 0 && p->unloadPending && !p->lifecycle)
        {
            FinishPendingUnload(p);
            if (!ok) LOG("plugins: %s faulted in %s while its unload was pending (exception 0x%08X)", p->name.c_str(), what, c.code);
            return ok;
        }
        if (ok) return true;
        char err[256];
        _snprintf_s(err, _TRUNCATE, "%s in %s (exception 0x%08X)%s", c.code == 0xC0000005 ? "access violation" : c.code == 0xE06D7363 ? "C++ exception" : "fault", what, c.code, c.dumped ? "" : ", no minidump");
        if (!c.dumped && c.dumpError) LOG("plugins: MiniDumpWriteDump failed (error 0x%08X), partial dump removed", c.dumpError);
        p->error = err;
        p->state = ModToolsPlugin::Failed;
        ++g_version;
        hooks::ResetUntracked();     // it may have faulted between BeginUntracked and EndUntracked
        LOG("plugins: %s DISABLED: %s", p->name.c_str(), err);
        if (c.dumped) LOGW(L"plugins: minidump written to %s", c.dumpPath);
        return false;
    }

    bool Load(ModToolsPlugin* p)
    {
        if (p->state == ModToolsPlugin::Loaded) return true;
        if (p->replaced) return false;
        ApplyCachedApps(p);
        if (p->state == ModToolsPlugin::OtherApp) { ++g_version; LOG("plugins: %s: %s (cached)", p->name.c_str(), p->error.c_str()); return false; }
        Lifecycle lc{ p };                 // a disable arriving inside the entry point or OnLoad waits for us
        hostapi::DetourBatch batch;        // its detours go live together when OnLoad has returned (nested in Init's batch)
        p->error.clear();
        LOGW(L"plugins: loading %S from %s", p->name.c_str(), p->dllPath.c_str());
        // the plugin's own folder first, so a plugin can ship helper DLLs next to itself
        HMODULE m = LoadLibraryExW(p->dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m) { p->error = "LoadLibrary failed (" + std::to_string(GetLastError()) + ")"; p->state = ModToolsPlugin::Failed; ++g_version; LOG("plugins: %s: %s", p->name.c_str(), p->error.c_str()); return false; }
        ModToolsPluginEntryFn entry = FindEntry(m);
        if (!entry) { p->error = NoEntryReason(m); FreeLibrary(m); p->state = ModToolsPlugin::Incompatible; ++g_version; LOG("plugins: %s: %s", p->name.c_str(), p->error.c_str()); return false; }
        p->module = m;
        p->state = ModToolsPlugin::Loaded;          // so host calls made from the entry point are accepted
        EntryCall ec{ entry, p, nullptr };
        if (!Call(p, "ModToolsPluginEntry", CallEntry, &ec)) { ReleaseResources(p); FreeLibrary(m); p->module = nullptr; return false; }
        const ModToolsPluginApi* api = ec.result;
        std::string why;
        if (!api) why = "ModToolsPluginEntry returned NULL";
        else if (api->apiVersion != MODTOOLS_PLUGIN_API_VERSION) why = "built for plugin API version " + std::to_string(api->apiVersion) + ", this host has " + std::to_string(MODTOOLS_PLUGIN_API_VERSION);
        else if (api->size < offsetof(ModToolsPluginApi, OnLoad)) why = "plugin table too small";
        else if (!api->name || !IEquals(api->name, p->name)) why = std::string("plugin name '") + (api->name ? api->name : "") + "' does not match the DLL name";
        else if (StaleBuild(p->name, api->version)) why = std::string("version ") + api->version + " needs an update for this MTPlugins version (Browse plugins)";
        if (!why.empty())
        {
            ReleaseResources(p); FreeLibrary(m); p->module = nullptr;
            p->error = why; p->state = ModToolsPlugin::Incompatible; ++g_version;
            LOG("plugins: %s: %s", p->name.c_str(), why.c_str());
            return false;
        }
        memset(&p->api, 0, sizeof p->api);
        memcpy(&p->api, api, api->size < sizeof p->api ? api->size : sizeof p->api);
        // a plugin built before APE support has no `apps` field (0): it is a Radiant plugin
        uint32_t apps = p->api.apps ? p->api.apps : MODTOOLS_APP_RADIANT;
        WriteCachedApps(p, apps);
        if (!(apps & uint32_t(app::ApiId())))
        {
            p->displayName = api->displayName ? api->displayName : p->name;
            p->version = api->version ? api->version : "";
            p->author = api->author ? api->author : "";
            p->description = api->description ? api->description : "";
            p->url = SafeUrl(p->api.url);
            WriteCachedVersion(p);
            ReleaseResources(p); FreeLibrary(m); p->module = nullptr;
            memset(&p->api, 0, sizeof p->api);
            p->error = "Runs in " + AppNames(apps) + "; shared toggle applies there.";
            p->state = ModToolsPlugin::OtherApp; ++g_version;
            LOG("plugins: %s: %s", p->name.c_str(), p->error.c_str());
            return false;
        }
        p->displayName = api->displayName ? api->displayName : p->name;
        p->version = api->version ? api->version : "";
        p->author = api->author ? api->author : "";
        p->description = api->description ? api->description : "";
        p->url = SafeUrl(p->api.url);
        WriteCachedVersion(p);
        if (p->api.OnLoad)
        {
            IntCall ic{ p->api.OnLoad, 1 };
            if (!Call(p, "OnLoad", CallInt, &ic)) { ReleaseResources(p); return false; }
            if (!ic.result)
            {
                ReleaseResources(p); FreeLibrary(m); p->module = nullptr;
                p->error = "OnLoad refused (see the log)"; p->state = ModToolsPlugin::Failed; ++g_version;
                LOG("plugins: %s: OnLoad returned 0", p->name.c_str());
                return false;
            }
        }
        ++g_version;
        LOG("plugins: %s %s loaded (%s)", p->displayName.c_str(), p->version.c_str(), p->description.c_str());
        // a map may already be open: replay the load so the plugin starts in the right state
        std::wstring cur = hooks::CurrentMapPath();
        if (!cur.empty() && p->api.OnMapLoaded) { WstrCall wc{ p->api.OnMapLoaded, cur.c_str() }; Call(p, "OnMapLoaded", CallWstr, &wc); }
        return p->state == ModToolsPlugin::Loaded;
    }

    bool Unload(ModToolsPlugin* p)
    {
        if (!p->module) { if (p->state != ModToolsPlugin::OtherApp) p->state = p->enabled ? ModToolsPlugin::Discovered : ModToolsPlugin::Disabled; ++g_version; return true; }
        // The command channel (WM_COPYDATA) is dispatched inside the plugin's own synchronous user32 calls, so this
        // can run under the plugin's frame: defer, Call() finishes it. A live detour without a Call() frame is the
        // same hazard (PluginCodeOnStack), OnTick retries it.
        const bool hookOnStack = p->calls == 0 && PluginCodeOnStack(p->module);
        if (p->calls > 0 || hookOnStack)
        {
            if (!p->unloadPending)
                LOG("plugins: unload of %s deferred (%s)", p->name.c_str(),
                    hookOnStack ? "Radiant is inside one of its hooks" : "its current call has not returned");
            p->unloadPending = true;
            return true;
        }
        LOG("plugins: unloading %s", p->name.c_str());
        Lifecycle lc{ p };                 // a disable arriving inside OnUnload must not start a second unload
        if ((p->state == ModToolsPlugin::Loaded || p->state == ModToolsPlugin::Failed) && p->api.OnUnload)
        {
            ModToolsPlugin::State keep = p->state;
            p->state = ModToolsPlugin::Loaded;      // Call refuses nothing, but keep Failed's error text below
            std::string err = p->error;
            Call(p, "OnUnload", CallVoid, reinterpret_cast<void*>(p->api.OnUnload));
            if (keep == ModToolsPlugin::Failed) p->error = err;
        }
        ReleaseResources(p);
        FreeLibrary(p->module);
        p->module = nullptr;
        memset(&p->api, 0, sizeof p->api);
        p->state = p->enabled ? ModToolsPlugin::Discovered : ModToolsPlugin::Disabled;
        ++g_version;
        return true;
    }

    bool Reload(ModToolsPlugin* p)
    {
        Unload(p);
        return Load(p);
    }

    void SetEnabled(ModToolsPlugin* p, bool on)
    {
        if (!WriteEnabled(p->name, on))
        {
            p->error = "Could not save shared enabled state (Windows error " + std::to_string(GetLastError()) + ")";
            ++g_version;
            return;
        }
        p->enabled = on;
        if (on) Load(p); else Unload(p);
        ++g_version;
    }

    // A disabled plugin's editors are unknown: call its entry point for the table, unload without OnLoad. Cached per DLL build.
    static void ProbeApps(ModToolsPlugin* p)
    {
        if (p->module || p->apps || p->enabled) return;
        HMODULE m = LoadLibraryExW(p->dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m) return;
        ModToolsPluginEntryFn entry = FindEntry(m);
        if (!entry) { FreeLibrary(m); return; }
        ModToolsPlugin::State keep = p->state;
        std::string err = p->error;
        p->module = m;
        p->state = ModToolsPlugin::Loaded;          // so host calls made from the entry point are accepted
        EntryCall ec{ entry, p, nullptr };
        bool ok = Call(p, "ModToolsPluginEntry (probe)", CallEntry, &ec);
        const ModToolsPluginApi* api = ok ? ec.result : nullptr;
        uint32_t apps = 0;
        if (api && api->apiVersion == MODTOOLS_PLUGIN_API_VERSION && api->size >= offsetof(ModToolsPluginApi, OnLoad) && api->name && IEquals(api->name, p->name))
            apps = api->size >= offsetof(ModToolsPluginApi, apps) + sizeof api->apps && api->apps ? api->apps : MODTOOLS_APP_RADIANT;
        ReleaseResources(p); FreeLibrary(m); p->module = nullptr;
        memset(&p->api, 0, sizeof p->api);
        if (!ok) return;                           // Call already marked it Failed, with the fault
        p->state = keep; p->error = err;
        if (!apps) return;
        WriteCachedApps(p, apps);
        ApplyCachedApps(p);
        ++g_version;
        LOG("plugins: %s (disabled) is for %s", p->name.c_str(), AppNames(apps).c_str());
    }

    // Entries stay allocated: a pointer kept by a pane or callback must never dangle
    void Forget(ModToolsPlugin* p)
    {
        if (!p || p->module) return;
        g_plugins.erase(std::remove(g_plugins.begin(), g_plugins.end(), p), g_plugins.end());
        WritePrivateProfileStringW(Widen(p->name).c_str(), nullptr, nullptr, g_iniPath.c_str());
        ++g_version;
    }

    void Rescan()
    {
        // a plugin uninstalled by another application: its DLL is gone and nothing here holds it
        for (size_t i = g_plugins.size(); i-- > 0;)
        {
            ModToolsPlugin* p = g_plugins[i];
            if (!p->module && !p->unloadPending && GetFileAttributesW(p->dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) g_plugins.erase(g_plugins.begin() + i);
        }
        Discover();
        for (ModToolsPlugin* p : g_plugins) ProbeApps(p);
        for (ModToolsPlugin* p : g_plugins)
            if (p->state == ModToolsPlugin::Discovered && p->enabled && g_initialized && !support::SafeStart()) Load(p);
        ++g_version;
    }

    void Init()
    {
        if (g_initialized) return;
        g_initialized = true;
        ResolveRoots();
        LOGW(L"plugins: folder '%s' (mod tools root '%s', editor %S)", g_root.c_str(), g_radiantRoot.c_str(), app::Name());
        Discover();
        if (support::SafeStart())      // the start guard's question was answered Yes: nothing loads, nothing is probed
        {
            for (ModToolsPlugin* p : g_plugins)
                if (p->enabled && p->state == ModToolsPlugin::Discovered) p->error = "not loaded: started without plugins (Reload loads it)";
            LOG("plugins: started without plugins (start guard), %zu plugin(s) found", g_plugins.size());
            return;
        }
        hostapi::BeginDetourBatch();
        for (ModToolsPlugin* p : g_plugins) if (p->enabled) Load(p); else { LOG("plugins: %s is disabled in plugins.ini", p->name.c_str()); ProbeApps(p); }
        hostapi::EndDetourBatch();
        LOG("plugins: %zu plugin(s) found", g_plugins.size());
    }

    void OnTick()
    {
        // retry an unload deferred without a Call() frame (Radiant was inside a plugin detour):
        // the outermost tick runs before RoutineProcessing, so no hook is on the stack
        for (ModToolsPlugin* p : g_plugins)
            if (p->unloadPending && p->calls == 0 && !p->lifecycle) FinishPendingUnload(p);

        // Global preference, local loaded state. Never write during reconciliation.
        // A fault is retried only on a new enable transition or explicit Reload.
        static ULONGLONG lastSync = 0;
        ULONGLONG now = GetTickCount64();
        if (g_initialized && now - lastSync >= 500)
        {
            lastSync = now;
            for (ModToolsPlugin* p : g_plugins)
            {
                bool on = ReadEnabled(p->name);
                if (on == p->enabled || p->replaced) continue;
                p->enabled = on;
                if (on) Load(p); else Unload(p);
                ++g_version;
                LOG("plugins: synchronized %s enabled=%d", p->name.c_str(), int(on));
            }
        }
        for (ModToolsPlugin* p : g_plugins)
            if (p->state == ModToolsPlugin::Loaded && p->api.OnTick) Call(p, "OnTick", CallVoid, reinterpret_cast<void*>(p->api.OnTick));
        support::Tick();
    }

    void OnMapLoading(const std::wstring& path)
    {
        for (ModToolsPlugin* p : LoadedSnapshot())
            if (p->state == ModToolsPlugin::Loaded && p->api.OnMapLoading) { WstrCall c{ p->api.OnMapLoading, path.c_str() }; Call(p, "OnMapLoading", CallWstr, &c); }
    }

    void OnMapLoaded(const std::wstring& path)
    {
        support::NoteMapLoaded();
        for (ModToolsPlugin* p : LoadedSnapshot())
            if (p->state == ModToolsPlugin::Loaded && p->api.OnMapLoaded) { WstrCall c{ p->api.OnMapLoaded, path.c_str() }; Call(p, "OnMapLoaded", CallWstr, &c); }
    }

    void OnMapSaved(const std::wstring& path, bool autosave, bool selectedOnly)
    {
        for (ModToolsPlugin* p : LoadedSnapshot())
            if (p->state == ModToolsPlugin::Loaded && p->api.OnMapSaved) { SavedCall c{ p->api.OnMapSaved, path.c_str(), autosave, selectedOnly }; Call(p, "OnMapSaved", CallSaved, &c); }
    }

    void OnBeforeScopeChange()
    {
        for (ModToolsPlugin* p : LoadedSnapshot())
            if (p->state == ModToolsPlugin::Loaded && p->api.OnBeforeScopeChange) Call(p, "OnBeforeScopeChange", CallVoid, reinterpret_cast<void*>(p->api.OnBeforeScopeChange));
    }

    void OnScopeChanged(const std::wstring& path, int depth)
    {
        for (ModToolsPlugin* p : LoadedSnapshot())
            if (p->state == ModToolsPlugin::Loaded && p->api.OnScopeChanged) { ScopeCall c{ p->api.OnScopeChanged, path.c_str(), depth }; Call(p, "OnScopeChanged", CallScope, &c); }
    }

    void OnAssetChanged(const std::string& name, const std::string& type)
    {
        struct AssetCall { void (*fn)(const char*, const char*); const char* n; const char* t; };
        for (ModToolsPlugin* p : LoadedSnapshot())
            if (p->state == ModToolsPlugin::Loaded && p->api.OnAssetChanged)
            {
                AssetCall c{ p->api.OnAssetChanged, name.c_str(), type.c_str() };
                Call(p, "OnAssetChanged", [](void* a) { auto* c = static_cast<AssetCall*>(a); c->fn(c->n, c->t); }, &c);
            }
    }

    void DispatchDraw(const ModToolsDrawContext& ctx)
    {
        for (ModToolsPlugin* p : g_plugins)
            if (p->state == ModToolsPlugin::Loaded && p->api.OnDraw) { DrawCall c{ p->api.OnDraw, &ctx }; Call(p, "OnDraw", CallDraw, &c); }
    }

    const char* CommandTo(ModToolsPlugin* p, const char* cmd)
    {
        if (!p || p->state != ModToolsPlugin::Loaded || !p->api.OnCommand) return nullptr;
        CmdCall c{ p->api.OnCommand, cmd, nullptr };
        if (!Call(p, "OnCommand", CallCmd, &c) || !c.result) return nullptr;
        p->lastReply = c.result;
        return p->lastReply.c_str();
    }

    std::string Command(const std::string& raw)
    {
        std::string cmd = raw;
        while (!cmd.empty() && isspace(static_cast<unsigned char>(cmd.back()))) cmd.pop_back();
        size_t a = 0; while (a < cmd.size() && isspace(static_cast<unsigned char>(cmd[a]))) ++a;
        cmd = cmd.substr(a);
        std::string first = cmd, rest;
        size_t sp = cmd.find(' ');
        if (sp != std::string::npos) { first = cmd.substr(0, sp); rest = cmd.substr(sp + 1); size_t b = 0; while (b < rest.size() && rest[b] == ' ') ++b; rest = rest.substr(b); }

        if (first == "plugins")
        {
            std::string sub = rest, arg;
            size_t s2 = rest.find(' ');
            if (s2 != std::string::npos) { sub = rest.substr(0, s2); arg = rest.substr(s2 + 1); }
            if (sub.empty() || sub == "list")
            {
                std::string out;
                for (ModToolsPlugin* p : g_plugins) out += p->name + " " + StateName(p->state) + (p->version.empty() ? "" : " " + p->version) + (p->error.empty() ? "" : " (" + p->error + ")") + "\n";
                return out.empty() ? "no plugins" : out;
            }
            if (sub == "rescan") { Rescan(); return "rescanned"; }
            if (sub == "window")
            {
                if (arg == "search" || arg.rfind("search ", 0) == 0)
                {
                    int n = pluginmgr::Search(arg.size() > 7 ? arg.substr(7) : "");
                    return n < 0 ? "no window" : std::to_string(n) + " plugin(s) shown";
                }
                if (arg == "browse" || arg == "installed") pluginmgr::ShowMode(arg == "browse");
                else if (arg != "status") pluginmgr::Toggle();
                return pluginmgr::Visible() ? "window shown" : "window hidden";
            }
            if (sub == "browse")    // plugins browse [refresh]
            {
                // a failed list is asked for again only with "refresh"
                if (arg == "refresh" || (!browse::Loaded() && !browse::Loading() && browse::Message().empty())) browse::Refresh();
                if (browse::Loading()) return "loading: " + browse::Message();
                std::string out;
                for (const browse::Entry& e : browse::Entries()) out += e.name + " " + e.version + " " + browse::StatusName(browse::StatusOf(e)) + (e.bundled || e.official ? " (comes with MTPlugins)" : "") + "\n";
                return out + browse::Message() + (browse::Installing() ? " (install running)" : "");
            }
            // plugins install|uninstall <Name>: local automation, the window asks first
            if (sub == "install") return browse::Install(arg) ? "installing " + arg : browse::Message();
            if (sub == "uninstall") return browse::Uninstall(arg) ? "uninstalling " + arg : browse::UninstallNote();
            if (sub == "pane")      // plugins pane <title> show|hide|status (tests)
            {
                size_t sp = arg.find_last_of(' ');
                std::string title = sp == std::string::npos ? arg : arg.substr(0, sp), what = sp == std::string::npos ? "status" : arg.substr(sp + 1);
                dock::Pane* pane = dock::Find(title.c_str());
                if (!pane) return "no pane '" + title + "'";
                if (what == "show") dock::Show(pane, true); else if (what == "hide") dock::Show(pane, false);
                return "pane '" + title + "' " + (dock::Visible(pane) ? "shown" : "hidden");
            }
            ModToolsPlugin* p = Find(arg);
            if (!p) return "unknown plugin '" + arg + "'";
            if (sub == "enable") { SetEnabled(p, true); return p->name + " " + StateName(p->state) + (p->error.empty() ? "" : " (" + p->error + ")"); }
            if (sub == "disable") { SetEnabled(p, false); return p->name + " " + (p->unloadPending ? "Disabling (unload deferred until its code is off the stack)" : StateName(p->state)); }
            if (sub == "reload") { Reload(p); return p->name + " " + StateName(p->state) + (p->error.empty() ? "" : " (" + p->error + ")"); }
            return "unknown plugins command '" + sub + "'";
        }
        if (first == "update")      // update [check|on|off|plugins [install|on|off]] (update.cpp)
        {
            if (rest == "plugins") return update::PluginStatus();
            if (rest == "plugins on" || rest == "plugins off") { update::SetPluginsEnabled(rest == "plugins on"); return update::Status(); }
            if (rest == "plugins install") { update::InstallPluginUpdates(); return update::PluginStatus(); }
            if (rest == "check") update::CheckNow();
            else if (rest == "on" || rest == "off") update::SetEnabled(rest == "on");
            else if (!rest.empty()) return "unknown update command '" + rest + "' (check, on, off, plugins, plugins install, plugins on, plugins off)";
            return update::Status();
        }
        if (first == "ape" && app::IsApe())
        {
            std::string r = ape::Command(rest);
            return r.empty() ? "unknown ape command '" + rest + "' (widgets [depth], current, tabs, selected, print <text>)" : r;
        }
        if (first == "report" && rest == "ask") { mt::Post([]() { support::Report(true); }); return "asked"; }   // the menu's flow, boxes included
        if (first == "report") { if (rest != "status") support::Report(false); return support::ReportStatus(); }   // report [status]
        if (first == "startguard") return support::StartGuardStatus();
        if (first == "whatsnew") { support::ShowWhatsNew(); return "shown"; }
        if (first == "about") { support::ShowAbout(); return "shown"; }
        if (first == "host") return std::string("RadiantPlugins host inside ") + app::Name() + " (pid " + std::to_string(GetCurrentProcessId()) + ")";
        if (first == "menus")       // menus [Edit]
        {
            void* mw = qt::Bind() ? dock::MainWindowWidget() : nullptr;
            std::string d = mw ? qt::DescribeMenu(mw, rest) : "";
            return d.empty() ? "no menu '" + rest + "'" : d;
        }
        if (first == "widgets" && !app::IsApe())   // widgets <objectName> [depth]
        {
            std::string name = rest, depthText; size_t sp = rest.find(' ');
            if (sp != std::string::npos) { name = rest.substr(0, sp); depthText = rest.substr(sp + 1); }
            int depth = depthText.empty() ? 8 : atoi(depthText.c_str());
            void* mw = qt::Bind() ? dock::MainWindowWidget() : nullptr;
            void* root = mw && !name.empty() ? qt::FindChildByName(mw, name.c_str()) : nullptr;
            if (!root) return "no widget named '" + name + "'";
            std::string out; int lines = 0;
            std::function<void(void*, int)> walk = [&](void* o, int level) {
                if (level > depth || ++lines > 2000) return;
                out += std::string(size_t(level) * 2, ' ') + qt::ClassName(o);
                std::string on = qt::ObjectName(o); if (!on.empty()) out += " '" + on + "'";
                if (qt::IsA(o, "QWidget") && !qt::IsVisible(o)) out += " hidden";
                if (qt::IsA(o, "QLabel")) out += " text=\"" + qt::LabelText(o) + "\"";
                else if (qt::IsA(o, "QAbstractButton")) out += " text=\"" + qt::ButtonText(o) + "\"" + (qt::IsChecked(o) ? " checked" : "");
                else if (qt::IsA(o, "QAbstractSlider") && qt::PanelBound()) out += " value=" + std::to_string(qt::SliderValue(o));
                else if (qt::IsA(o, "QDoubleSpinBox")) out += " value=" + std::to_string(qt::DoubleSpinValue(o));
                else if (qt::IsA(o, "QSpinBox")) out += " value=" + std::to_string(qt::SpinValue(o));
                else if (qt::IsA(o, "QLineEdit")) out += " text=\"" + qt::EditText(o) + "\"";
                out += "\n";
                for (void* c : qt::Children(o)) walk(c, level + 1);
            };
            walk(root, 0);
            return out;
        }
        if (first == "click" && !app::IsApe())     // click <objectName>
        {
            void* mw = qt::Bind() ? dock::MainWindowWidget() : nullptr;
            void* b = mw && !rest.empty() ? qt::FindChildByName(mw, rest.c_str()) : nullptr;
            if (!b || !qt::IsA(b, "QAbstractButton")) return "no button named '" + rest + "'";
            qt::Click(b);
            return "clicked '" + rest + "'" + (qt::IsChecked(b) ? " (checked)" : "");
        }
        if (first == "menu")        // menu Edit/Undo
        {
            void* mw = qt::Bind() ? dock::MainWindowWidget() : nullptr;
            void* a = mw ? qt::FindMenuAction(mw, rest) : nullptr;
            if (!a) return "no menu entry '" + rest + "'";
            if (!qt::ActionEnabled(a)) return "'" + rest + "' is disabled";
            qt::TriggerAction(a);
            return "triggered '" + rest + "'";
        }
        if (ModToolsPlugin* p = Find(first))
        {
            const char* r = CommandTo(p, rest.c_str());
            return r ? r : (p->state == ModToolsPlugin::Loaded ? "unknown command '" + rest + "'" : p->name + " is not loaded");
        }
        for (ModToolsPlugin* p : LoadedSnapshot())
            if (const char* r = CommandTo(p, cmd.c_str())) return r;
        return "unknown command '" + cmd + "'";
    }

    bool ReloadPromptsInhibited()
    {
        for (ModToolsPlugin* p : g_plugins) if (p->state == ModToolsPlugin::Loaded && p->inhibitReload) return true;
        return false;
    }

    void Shutdown()
    {
        for (ModToolsPlugin* p : g_plugins) if (p->module) Unload(p);
        for (ModToolsPlugin* p : g_plugins) delete p;
        g_plugins.clear();
    }
}
