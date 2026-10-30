#pragma once
// With BB_COUNT_FILES=<list path> set, writes every distinct read-only file cod2map opens at exit (diagnostic).
// With BB_COMPILE_JOURNAL=<path> set, writes the skip_unchanged journal there: reads, missed lookups, writes (worker/unchanged.h).
// CreateFileA and W are hooked separately (A does not route through W). Skips temp dir, .tmp, .db and written files.
// The journal also records stats and attribute queries: cod2map keys its convert cache on source stats, not opens.
// Nothing is hooked when neither variable is set.
#include <windows.h>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>
#include "../../../src/third_party/minhook/include/MinHook.h"

namespace filecount
{
    using CreateFileWFn = HANDLE (WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    using CreateFileAFn = HANDLE (WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    inline CreateFileWFn g_createFileW = nullptr;
    inline CreateFileAFn g_createFileA = nullptr;
    inline SRWLOCK g_lock = SRWLOCK_INIT;
    inline std::unordered_set<std::wstring>* g_seen = nullptr;
    inline std::vector<std::wstring>* g_order = nullptr;
    inline std::unordered_set<std::wstring>* g_written = nullptr;
    inline std::wstring g_listFile, g_tempDir, g_journalFile;
    inline std::unordered_set<std::wstring>* g_missing = nullptr;
    inline unsigned long long g_calls = 0, g_callsA = 0;
    inline thread_local bool t_bypass = false;  // Report's own open must not re-enter the lock

    inline std::wstring Lower(const wchar_t* s)
    {
        std::wstring out(s);
        if (!out.empty()) CharLowerBuffW(&out[0], (DWORD)out.size());
        return out;
    }

    inline void Record(const wchar_t* name, DWORD access, DWORD disposition, DWORD flags, bool found = true)
    {
        if ((flags & FILE_FLAG_BACKUP_SEMANTICS) || !wcsncmp(name, L"\\\\.\\", 4)) return;
        const bool writes = (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE)) || disposition != OPEN_EXISTING;
        const bool reads = (access & (GENERIC_READ | FILE_READ_DATA)) != 0;
        if (!writes && !reads) return;
        if (!found && (writes || !g_missing)) return;
        std::wstring key = Lower(name);
        const auto endsWith = [&](const wchar_t* tail) { const size_t n = wcslen(tail); return key.size() >= n && !key.compare(key.size() - n, n, tail); };
        if (!writes && (endsWith(L".tmp") || endsWith(L".db") ||
                        (!g_tempDir.empty() && key.size() > g_tempDir.size() && !key.compare(0, g_tempDir.size(), g_tempDir))))
            return;
        AcquireSRWLockExclusive(&g_lock);
        if (!found) g_missing->insert(std::move(key));
        else if (writes) g_written->insert(std::move(key));
        else if (g_seen->insert(key).second) g_order->push_back(std::move(key));
        ReleaseSRWLockExclusive(&g_lock);
    }

    inline HANDLE WINAPI HookW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE templ)
    {
        const HANDLE h = g_createFileW(name, access, share, sa, disposition, flags, templ);
        ++g_calls;
        if (t_bypass || !name) return h;
        const DWORD error = GetLastError();
        const bool missing = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        if (h != INVALID_HANDLE_VALUE || missing) Record(name, access, disposition, flags, h != INVALID_HANDLE_VALUE);
        SetLastError(error);
        return h;
    }

    inline HANDLE WINAPI HookA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE templ)
    {
        const HANDLE h = g_createFileA(name, access, share, sa, disposition, flags, templ);
        ++g_calls; ++g_callsA;
        if (t_bypass || !name) return h;
        const DWORD error = GetLastError();
        const bool missing = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        wchar_t wide[MAX_PATH * 4];
        if ((h != INVALID_HANDLE_VALUE || missing) && MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH * 4) > 0)
            Record(wide, access, disposition, flags, h != INVALID_HANDLE_VALUE);
        SetLastError(error);
        return h;
    }

    // journal only: a stat or attribute query counts as a read of that path (found) or a missed lookup
    inline void RecordQuery(const wchar_t* name, bool found)
    {
        if (t_bypass || !name || !*name || wcspbrk(name, L"*?") || !wcsncmp(name, L"\\\\.\\", 4)) return;
        const DWORD error = GetLastError();
        Record(name, GENERIC_READ, OPEN_EXISTING, 0, found);
        SetLastError(error);
    }
    inline void RecordQueryA(const char* name, bool found)
    {
        wchar_t wide[MAX_PATH * 4];
        if (name && MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH * 4) > 0) RecordQuery(wide, found);
    }
    inline bool Missing() { const DWORD e = GetLastError(); return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND; }

    using FindFirstFileExWFn = HANDLE (WINAPI*)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
    using FindFirstFileExAFn = HANDLE (WINAPI*)(LPCSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
    using GetAttrExWFn = BOOL (WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
    using GetAttrExAFn = BOOL (WINAPI*)(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
    using GetAttrWFn = DWORD (WINAPI*)(LPCWSTR);
    using GetAttrAFn = DWORD (WINAPI*)(LPCSTR);
    inline FindFirstFileExWFn g_findW = nullptr;
    inline FindFirstFileExAFn g_findA = nullptr;
    inline GetAttrExWFn g_attrExW = nullptr;
    inline GetAttrExAFn g_attrExA = nullptr;
    inline GetAttrWFn g_attrW = nullptr;
    inline GetAttrAFn g_attrA = nullptr;
    inline HANDLE WINAPI FindW(LPCWSTR n, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD fl)
    { const HANDLE h = g_findW(n, l, d, o, f, fl); if (h != INVALID_HANDLE_VALUE || Missing()) RecordQuery(n, h != INVALID_HANDLE_VALUE); return h; }
    inline HANDLE WINAPI FindA(LPCSTR n, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD fl)
    { const HANDLE h = g_findA(n, l, d, o, f, fl); if (h != INVALID_HANDLE_VALUE || Missing()) RecordQueryA(n, h != INVALID_HANDLE_VALUE); return h; }
    inline BOOL WINAPI AttrExW(LPCWSTR n, GET_FILEEX_INFO_LEVELS l, LPVOID d)
    { const BOOL ok = g_attrExW(n, l, d); if (ok || Missing()) RecordQuery(n, ok != FALSE); return ok; }
    inline BOOL WINAPI AttrExA(LPCSTR n, GET_FILEEX_INFO_LEVELS l, LPVOID d)
    { const BOOL ok = g_attrExA(n, l, d); if (ok || Missing()) RecordQueryA(n, ok != FALSE); return ok; }
    inline DWORD WINAPI AttrW(LPCWSTR n)
    { const DWORD a = g_attrW(n); if (a != INVALID_FILE_ATTRIBUTES || Missing()) RecordQuery(n, a != INVALID_FILE_ATTRIBUTES); return a; }
    inline DWORD WINAPI AttrA(LPCSTR n)
    { const DWORD a = g_attrA(n); if (a != INVALID_FILE_ATTRIBUTES || Missing()) RecordQueryA(n, a != INVALID_FILE_ATTRIBUTES); return a; }

    // after MH_Initialize, before MH_EnableHook(MH_ALL_HOOKS)
    inline bool Install()
    {
        wchar_t path[MAX_PATH * 2] = {}, journal[MAX_PATH * 2] = {};
        const DWORD list = GetEnvironmentVariableW(L"BB_COUNT_FILES", path, MAX_PATH * 2);
        const DWORD jn = GetEnvironmentVariableW(L"BB_COMPILE_JOURNAL", journal, MAX_PATH * 2);
        if ((!list || list >= MAX_PATH * 2) && (!jn || jn >= MAX_PATH * 2)) return false;
        if (list && list < MAX_PATH * 2) g_listFile = path;
        if (jn && jn < MAX_PATH * 2) { g_journalFile = journal; g_missing = new std::unordered_set<std::wstring>(); }
        wchar_t temp[MAX_PATH * 2];
        if (GetTempPathW(MAX_PATH * 2, temp)) { g_tempDir = Lower(temp); while (!g_tempDir.empty() && g_tempDir.back() == L'\\') g_tempDir.pop_back(); }
        g_seen = new std::unordered_set<std::wstring>();
        g_order = new std::vector<std::wstring>();
        g_written = new std::unordered_set<std::wstring>();
        HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
        void* w = kb ? reinterpret_cast<void*>(GetProcAddress(kb, "CreateFileW")) : nullptr;
        void* a = kb ? reinterpret_cast<void*>(GetProcAddress(kb, "CreateFileA")) : nullptr;
        if (!w || !a || MH_CreateHook(w, reinterpret_cast<void*>(&HookW), reinterpret_cast<void**>(&g_createFileW)) != MH_OK ||
            MH_CreateHook(a, reinterpret_cast<void*>(&HookA), reinterpret_cast<void**>(&g_createFileA)) != MH_OK)
            return false;
        if (!g_missing) return true;
        // without these the journal is incomplete: no journal at all then (no "end" line, the next compile runs)
        auto hook = [&](const char* name, void* detour, void** original) {
            void* target = reinterpret_cast<void*>(GetProcAddress(kb, name));
            return target && MH_CreateHook(target, detour, original) == MH_OK;
        };
        if (!hook("FindFirstFileExW", reinterpret_cast<void*>(&FindW), reinterpret_cast<void**>(&g_findW)) ||
            !hook("FindFirstFileExA", reinterpret_cast<void*>(&FindA), reinterpret_cast<void**>(&g_findA)) ||
            !hook("GetFileAttributesExW", reinterpret_cast<void*>(&AttrExW), reinterpret_cast<void**>(&g_attrExW)) ||
            !hook("GetFileAttributesExA", reinterpret_cast<void*>(&AttrExA), reinterpret_cast<void**>(&g_attrExA)) ||
            !hook("GetFileAttributesW", reinterpret_cast<void*>(&AttrW), reinterpret_cast<void**>(&g_attrW)) ||
            !hook("GetFileAttributesA", reinterpret_cast<void*>(&AttrA), reinterpret_cast<void**>(&g_attrA)))
            g_journalFile.clear();
        return true;
    }

    inline void WriteJournal()
    {
        std::string text = "v1\n";
        auto add = [&](char kind, const std::wstring& p) {
            const int need = WideCharToMultiByte(CP_UTF8, 0, p.data(), (int)p.size(), nullptr, 0, nullptr, nullptr);
            text += kind;
            text += ' ';
            const size_t at = text.size();
            text.resize(at + need);
            WideCharToMultiByte(CP_UTF8, 0, p.data(), (int)p.size(), &text[at], need, nullptr, nullptr);
            text += '\n';
        };
        AcquireSRWLockExclusive(&g_lock);
        for (const std::wstring& p : *g_order) if (!g_written->count(p)) add('r', p);
        for (const std::wstring& p : *g_missing) if (!g_written->count(p) && !g_seen->count(p)) add('m', p);
        for (const std::wstring& p : *g_written) add('w', p);
        ReleaseSRWLockExclusive(&g_lock);
        text += "end\n";
        t_bypass = true;
        const std::wstring tmp = g_journalFile + L".tmp";
        const HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE)
        {
            DWORD wrote = 0;
            const bool ok = WriteFile(f, text.data(), (DWORD)text.size(), &wrote, nullptr) && wrote == text.size();
            CloseHandle(f);
            if (!ok || !MoveFileExW(tmp.c_str(), g_journalFile.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
        }
        t_bypass = false;
    }

    inline void Report(FILE* out)
    {
        if (!g_journalFile.empty()) WriteJournal();
        if (g_listFile.empty()) return;
        // write outside the lock: it goes through the hooked CreateFileW
        std::wstring text;
        size_t listed = 0, seen = 0, written = 0;
        AcquireSRWLockExclusive(&g_lock);
        seen = g_order->size(); written = g_written->size();
        for (const std::wstring& p : *g_order)
            if (!g_written->count(p)) { text += p; text += L"\n"; ++listed; }
        ReleaseSRWLockExclusive(&g_lock);
        t_bypass = true;
        if (FILE* f = _wfopen(g_listFile.c_str(), L"w, ccs=UTF-8")) { fputws(text.c_str(), f); fclose(f); }
        t_bypass = false;
        fprintf(out, "[BetterBuild] file count: %llu CreateFile calls (%llu A), %zu distinct read-only files, %zu listed after dropping %zu written, list %ls.\n",
                g_calls, g_callsA, seen, listed, written, g_listFile.c_str());
    }
}
