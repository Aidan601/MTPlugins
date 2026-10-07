// Light step file prefetch (see light_prefetch.h), adapted from the linker's prefetch_files. The list key is the map path.
#include "light_prefetch.h"
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "../../../src/third_party/minhook/include/MinHook.h"

namespace lightpf
{
namespace
{
using CreateFileWFn = HANDLE (WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileAFn = HANDLE (WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileWFn g_createFileW = nullptr;
CreateFileAFn g_createFileA = nullptr;
thread_local bool t_prefetcher = false;
SRWLOCK g_recordLock = SRWLOCK_INIT;
std::unordered_set<std::wstring>* g_readSeen = nullptr;
std::vector<std::wstring>* g_readOrder = nullptr;
std::unordered_set<std::wstring>* g_written = nullptr;
std::vector<std::wstring>* g_list = nullptr;
volatile LONG g_next = 0, g_opened = 0, g_live = 0, g_saved = 0;
ULONGLONG g_prefetchStart = 0, g_prefetchEnd = 0;
std::wstring g_listFile, g_tempDir;
void (*g_log)(const char*) = nullptr;
constexpr size_t kMaxListed = 200000;
// open timing of Radiant's own opens (all threads): total, failures, slowest few
volatile LONG64 g_openTicks = 0, g_opens = 0, g_failTicks = 0, g_fails = 0;
struct Slow { LONG64 ticks; std::wstring name; };
Slow g_slow[6];
SRWLOCK g_slowLock = SRWLOCK_INIT;
LONG64 g_freq = 1;

std::wstring Lower(const wchar_t* s);
std::unordered_map<std::wstring, std::pair<long, LONG64>>* g_perPath = nullptr;   // BB_PREFETCH_STATS=1: opens and ticks per path
void Timed(const wchar_t* name, LONG64 ticks, bool ok)
{
    if (g_perPath)
    {
        AcquireSRWLockExclusive(&g_slowLock);
        auto& e = (*g_perPath)[Lower(name)];
        ++e.first; e.second += ticks;
        ReleaseSRWLockExclusive(&g_slowLock);
    }
    InterlockedAdd64(ok ? &g_openTicks : &g_failTicks, ticks);
    InterlockedIncrement64(ok ? &g_opens : &g_fails);
    if (ticks < g_slow[5].ticks || ticks * 1000 < g_freq * 20) return;   // keep only opens over 20 ms
    AcquireSRWLockExclusive(&g_slowLock);
    int at = 5;
    while (at > 0 && g_slow[at - 1].ticks < ticks) --at;
    if (at < 6 && g_slow[5].ticks < ticks)
    {
        for (int i = 5; i > at; --i) g_slow[i] = g_slow[i - 1];
        g_slow[at].ticks = ticks; g_slow[at].name = name;
    }
    ReleaseSRWLockExclusive(&g_slowLock);
}

std::wstring Lower(const wchar_t* s)
{
    std::wstring out(s);
    if (!out.empty()) CharLowerBuffW(&out[0], (DWORD)out.size());
    return out;
}

void Record(const wchar_t* name, DWORD access, DWORD disposition, DWORD flags)
{
    if ((flags & FILE_FLAG_BACKUP_SEMANTICS) || !wcsncmp(name, L"\\\\.\\", 4)) return;
    const bool writes = (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE)) || disposition != OPEN_EXISTING;
    const bool reads = (access & (GENERIC_READ | FILE_READ_DATA)) != 0;
    if (!writes && !reads) return;
    std::wstring key = Lower(name);
    const auto endsWith = [&](const wchar_t* tail) { const size_t n = wcslen(tail); return key.size() >= n && !key.compare(key.size() - n, n, tail); };
    // never listed: temp files and SQLite databases (gdt.db is written by other tools)
    if (!writes && (endsWith(L".tmp") || endsWith(L".db") ||
                    (!g_tempDir.empty() && key.size() > g_tempDir.size() && !key.compare(0, g_tempDir.size(), g_tempDir))))
        return;
    AcquireSRWLockExclusive(&g_recordLock);
    if (writes) g_written->insert(std::move(key));
    else if (g_readOrder->size() < kMaxListed && g_readSeen->insert(key).second) g_readOrder->push_back(std::move(key));
    ReleaseSRWLockExclusive(&g_recordLock);
}

HANDLE WINAPI HookCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE templ)
{
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    const HANDLE h = g_createFileW(name, access, share, sa, disposition, flags, templ);
    if (t_prefetcher || !name || g_saved) return h;
    const DWORD error = GetLastError();
    QueryPerformanceCounter(&t1);
    Timed(name, t1.QuadPart - t0.QuadPart, h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) Record(name, access, disposition, flags);
    SetLastError(error);
    return h;
}

// kernelbase!CreateFileA does not go through CreateFileW's entry.
HANDLE WINAPI HookCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE templ)
{
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    const HANDLE h = g_createFileA(name, access, share, sa, disposition, flags, templ);
    if (t_prefetcher || !name || g_saved) return h;
    const DWORD error = GetLastError();
    QueryPerformanceCounter(&t1);
    wchar_t wide[MAX_PATH * 2];
    if (MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH * 2) > 0)
    {
        Timed(wide, t1.QuadPart - t0.QuadPart, h != INVALID_HANDLE_VALUE);
        if (h != INVALID_HANDLE_VALUE) Record(wide, access, disposition, flags);
    }
    SetLastError(error);
    return h;
}

DWORD WINAPI PrefetchMain(void*)
{
    t_prefetcher = true;
    const std::vector<std::wstring>& list = *g_list;
    for (;;)
    {
        const LONG i = InterlockedIncrement(&g_next) - 1;
        if (i >= (LONG)list.size()) break;
        const HANDLE h = CreateFileW(list[i].c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); InterlockedIncrement(&g_opened); }
    }
    if (InterlockedDecrement(&g_live) == 0) g_prefetchEnd = GetTickCount64();
    return 0;
}

// list name: the map's file name plus a hash of its full lower-case path
std::wstring ListFileName()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring map;
    for (int i = 1; argv && i < argc; ++i)
    {
        const std::wstring a = Lower(argv[i]);
        if (a.size() > 4 && !a.compare(a.size() - 4, 4, L".map")) map = a;
    }
    if (argv) LocalFree(argv);
    if (map.empty()) return L"";
    for (wchar_t& c : map) if (c == L'/') c = L'\\';
    unsigned hash = 2166136261u;
    for (wchar_t c : map) hash = (hash ^ c) * 16777619u;
    std::wstring name = map.substr(map.find_last_of(L'\\') + 1);
    name.resize(name.size() - 4);
    for (wchar_t& c : name) if (!iswalnum(c) && c != L'_' && c != L'-') c = L'_';
    wchar_t tail[32];
    swprintf_s(tail, L"_%08x.txt", hash);
    return name.substr(0, 64) + tail;
}

bool Hook(LPCWSTR module, LPCSTR proc, void* detour, void** original)
{
    HMODULE m = GetModuleHandleW(module);
    void* target = m ? reinterpret_cast<void*>(GetProcAddress(m, proc)) : nullptr;
    if (!target || MH_CreateHook(target, detour, original) != MH_OK) return false;
    if (MH_EnableHook(target) != MH_OK) { MH_RemoveHook(target); return false; }
    return true;
}
}

bool Start(const std::wstring& dir, void (*log)(const char*))
{
    t_prefetcher = true;   // this thread's own opens are not Radiant's
    g_log = log;
    if (GetEnvironmentVariableW(L"BB_PREFETCH_STATS", nullptr, 0) > 0) g_perPath = new std::unordered_map<std::wstring, std::pair<long, LONG64>>();
    LARGE_INTEGER qpf; QueryPerformanceFrequency(&qpf); g_freq = qpf.QuadPart ? qpf.QuadPart : 1;
    const std::wstring name = ListFileName();
    if (name.empty()) return false;
    g_listFile = dir + L"\\light_prefetch\\" + name;
    wchar_t temp[MAX_PATH + 1] = {};
    const DWORD tn = GetTempPathW(MAX_PATH + 1, temp);
    if (tn && tn <= MAX_PATH)
    {
        wchar_t longTemp[MAX_PATH * 2] = {};
        const DWORD ln = GetLongPathNameW(temp, longTemp, MAX_PATH * 2);
        g_tempDir = Lower(ln && ln < MAX_PATH * 2 ? longTemp : temp);
    }
    g_readSeen = new std::unordered_set<std::wstring>();
    g_readOrder = new std::vector<std::wstring>();
    g_written = new std::unordered_set<std::wstring>();
    g_list = new std::vector<std::wstring>();
    if (!Hook(L"kernelbase.dll", "CreateFileW", reinterpret_cast<void*>(&HookCreateFileW), reinterpret_cast<void**>(&g_createFileW)) ||
        !Hook(L"kernelbase.dll", "CreateFileA", reinterpret_cast<void*>(&HookCreateFileA), reinterpret_cast<void**>(&g_createFileA)))
    {
        if (g_log) g_log("prefetch: CreateFile hooks failed, off");
        g_listFile.clear();
        return false;
    }

    const HANDLE f = CreateFileW(g_listFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    std::string text;   // empty when there is no list yet: this run only records
    if (f != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER size = {};
        if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (64 << 20))
        {
            text.resize((size_t)size.QuadPart);
            DWORD got = 0;
            if (!ReadFile(f, &text[0], (DWORD)text.size(), &got, nullptr)) got = 0;
            text.resize(got);
        }
        CloseHandle(f);
    }
    for (size_t at = 0; at < text.size();)
    {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const int len = (int)(end - at);
        if (len > 0)
        {
            std::wstring w(len, L'\0');
            w.resize(MultiByteToWideChar(CP_UTF8, 0, text.data() + at, len, &w[0], len));
            if (!w.empty()) g_list->push_back(std::move(w));
        }
        at = end + 1;
    }
    char line[512];
    snprintf(line, sizeof line, "prefetch: %zu files listed from the last Light step of this map (%ls)", g_list->size(), name.c_str());
    if (g_log) g_log(line);
    if (g_list->empty()) return true;
    SYSTEM_INFO info = {};
    GetSystemInfo(&info);
    const int cores = (int)info.dwNumberOfProcessors;
    const int threads = cores < 4 ? 4 : cores > 16 ? 16 : cores;
    g_prefetchStart = GetTickCount64();
    g_live = threads;
    for (int i = 0; i < threads; ++i)
    {
        const HANDLE t = CreateThread(nullptr, 0, &PrefetchMain, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
        else if (InterlockedDecrement(&g_live) == 0) g_prefetchEnd = GetTickCount64();
    }
    return true;
}

std::string Save()
{
    if (!g_readOrder || g_listFile.empty() || InterlockedExchange(&g_saved, 1)) return "";
    t_prefetcher = true;   // the list file is not part of the run
    char report[512] = "";
    if (!g_list->empty())
    {
        const ULONGLONG end = g_prefetchEnd ? g_prefetchEnd : GetTickCount64();
        snprintf(report, sizeof report, "Prefetch: opened %ld of %zu files ahead of Radiant in %.1f s%s", g_opened, g_list->size(),
                 (end - g_prefetchStart) / 1000.0, g_prefetchEnd ? "" : " (still running at exit)");
    }
    if (g_log)
    {
        char line[512];
        snprintf(line, sizeof line, "prefetch: Radiant's load opened %lld files in %.2f s and failed %lld opens in %.2f s (all threads)",
                 (long long)g_opens, double(g_openTicks) / double(g_freq), (long long)g_fails, double(g_failTicks) / double(g_freq));
        g_log(line);
        AcquireSRWLockShared(&g_slowLock);
        for (const Slow& w : g_slow)
            if (w.ticks) { snprintf(line, sizeof line, "prefetch:   %.0f ms %ls", 1000.0 * double(w.ticks) / double(g_freq), w.name.c_str()); g_log(line); }
        if (g_perPath)
        {
            std::vector<std::pair<long, std::wstring>> top;
            for (auto& kv : *g_perPath) top.push_back({ kv.second.first, kv.first });
            std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
            for (size_t i = 0; i < top.size() && i < 25; ++i)
            { snprintf(line, sizeof line, "prefetch:   %ld opens %ls", top[i].first, top[i].second.c_str()); g_log(line); }
        }
        ReleaseSRWLockShared(&g_slowLock);
    }
    // at DLL_PROCESS_DETACH a thread may have died holding the lock: never wait, the list is just not saved
    if (!TryAcquireSRWLockExclusive(&g_recordLock)) return report;
    std::string text;
    size_t count = 0;
    for (const std::wstring& p : *g_readOrder)
    {
        if (g_written->count(p)) continue;
        const int need = WideCharToMultiByte(CP_UTF8, 0, p.data(), (int)p.size(), nullptr, 0, nullptr, nullptr);
        const size_t at = text.size();
        text.resize(at + need);
        WideCharToMultiByte(CP_UTF8, 0, p.data(), (int)p.size(), &text[at], need, nullptr, nullptr);
        text += '\n';
        ++count;
    }
    ReleaseSRWLockExclusive(&g_recordLock);
    if (text.empty()) return report;
    const std::wstring dir = g_listFile.substr(0, g_listFile.find_last_of(L'\\'));
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring tmp = g_listFile + L".tmp";
    const HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return report;
    DWORD wrote = 0;
    const bool ok = WriteFile(f, text.data(), (DWORD)text.size(), &wrote, nullptr) && wrote == text.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), g_listFile.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
    else if (g_log)
    {
        char line[128];
        snprintf(line, sizeof line, "prefetch: saved %zu files for the next Light step", count);
        g_log(line);
    }
    return report;
}
}
