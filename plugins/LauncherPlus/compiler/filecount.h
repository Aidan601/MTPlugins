#pragma once
// Diagnostic: with BB_COUNT_FILES=<list path> set, writes every distinct read-only file cod2map opens at exit.
// CreateFileA and W are hooked separately (A does not route through W). Skips temp dir, .tmp, .db and written files.
// Nothing is hooked when the variable is absent.
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
    inline std::wstring g_listFile, g_tempDir;
    inline unsigned long long g_calls = 0, g_callsA = 0;
    inline thread_local bool t_bypass = false;  // Report's own open must not re-enter the lock

    inline std::wstring Lower(const wchar_t* s)
    {
        std::wstring out(s);
        if (!out.empty()) CharLowerBuffW(&out[0], (DWORD)out.size());
        return out;
    }

    inline void Record(const wchar_t* name, DWORD access, DWORD disposition, DWORD flags)
    {
        if ((flags & FILE_FLAG_BACKUP_SEMANTICS) || !wcsncmp(name, L"\\\\.\\", 4)) return;
        const bool writes = (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE)) || disposition != OPEN_EXISTING;
        const bool reads = (access & (GENERIC_READ | FILE_READ_DATA)) != 0;
        if (!writes && !reads) return;
        std::wstring key = Lower(name);
        const auto endsWith = [&](const wchar_t* tail) { const size_t n = wcslen(tail); return key.size() >= n && !key.compare(key.size() - n, n, tail); };
        if (!writes && (endsWith(L".tmp") || endsWith(L".db") ||
                        (!g_tempDir.empty() && key.size() > g_tempDir.size() && !key.compare(0, g_tempDir.size(), g_tempDir))))
            return;
        AcquireSRWLockExclusive(&g_lock);
        if (writes) g_written->insert(std::move(key));
        else if (g_seen->insert(key).second) g_order->push_back(std::move(key));
        ReleaseSRWLockExclusive(&g_lock);
    }

    inline HANDLE WINAPI HookW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE templ)
    {
        const HANDLE h = g_createFileW(name, access, share, sa, disposition, flags, templ);
        ++g_calls;
        if (t_bypass || !name || h == INVALID_HANDLE_VALUE) return h;
        const DWORD error = GetLastError();
        Record(name, access, disposition, flags);
        SetLastError(error);
        return h;
    }

    inline HANDLE WINAPI HookA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE templ)
    {
        const HANDLE h = g_createFileA(name, access, share, sa, disposition, flags, templ);
        ++g_calls; ++g_callsA;
        if (t_bypass || !name || h == INVALID_HANDLE_VALUE) return h;
        const DWORD error = GetLastError();
        wchar_t wide[MAX_PATH * 4];
        if (MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH * 4) > 0) Record(wide, access, disposition, flags);
        SetLastError(error);
        return h;
    }

    // after MH_Initialize, before MH_EnableHook(MH_ALL_HOOKS)
    inline bool Install()
    {
        wchar_t path[MAX_PATH * 2];
        if (!GetEnvironmentVariableW(L"BB_COUNT_FILES", path, MAX_PATH * 2)) return false;
        g_listFile = path;
        wchar_t temp[MAX_PATH * 2];
        if (GetTempPathW(MAX_PATH * 2, temp)) { g_tempDir = Lower(temp); while (!g_tempDir.empty() && g_tempDir.back() == L'\\') g_tempDir.pop_back(); }
        g_seen = new std::unordered_set<std::wstring>();
        g_order = new std::vector<std::wstring>();
        g_written = new std::unordered_set<std::wstring>();
        HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
        void* w = kb ? reinterpret_cast<void*>(GetProcAddress(kb, "CreateFileW")) : nullptr;
        void* a = kb ? reinterpret_cast<void*>(GetProcAddress(kb, "CreateFileA")) : nullptr;
        return w && a && MH_CreateHook(w, reinterpret_cast<void*>(&HookW), reinterpret_cast<void**>(&g_createFileW)) == MH_OK &&
               MH_CreateHook(a, reinterpret_cast<void*>(&HookA), reinterpret_cast<void**>(&g_createFileA)) == MH_OK;
    }

    inline void Report(FILE* out)
    {
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
