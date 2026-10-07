#pragma once
// Loads BuildPlusCompiler.dll into a suspended cod2map64 and runs its start export before the
// compiler's main thread ever executes. The caller owns the child inside a kill-on-close job and
// must terminate it when this fails: a half-installed patch set is never resumed.
#include <windows.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <string>
#pragma comment(lib, "bcrypt.lib")

namespace inject
{
    // SHA-256 of the one cod2map64.exe build the patches were written against (T7, changelist 3421987).
    constexpr unsigned char kCompilerSha256[32] = {
        0x20,0x1a,0x6b,0x31,0x07,0x2f,0x74,0xcf,0xac,0xab,0x94,0x40,0x14,0xd4,0x46,0x18,
        0x55,0x70,0x60,0x8e,0x37,0xe5,0x56,0x13,0x2c,0xc0,0x0d,0xb3,0xf9,0xdb,0x8e,0xfd};

    inline bool Sha256Equals(const std::wstring& file, const unsigned char (&expected)[32])
    {
        HANDLE input = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (input == INVALID_HANDLE_VALUE) return false;
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
                  BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
        static unsigned char buffer[1 << 20];
        unsigned char digest[32] = {};
        while (ok)
        {
            DWORD bytes = 0;
            if (!ReadFile(input, buffer, sizeof buffer, &bytes, nullptr)) { ok = false; break; }
            if (!bytes) break;
            ok = BCryptHashData(hash, buffer, bytes, 0) >= 0;
        }
        ok = ok && BCryptFinishHash(hash, digest, sizeof digest, 0) >= 0 && !memcmp(digest, expected, sizeof digest);
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        CloseHandle(input);
        return ok;
    }

    inline uintptr_t RemoteModule(DWORD pid, const std::wstring& path)
    {
        HANDLE snapshot = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 8 && snapshot == INVALID_HANDLE_VALUE; ++attempt)
        {
            snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
            if (snapshot == INVALID_HANDLE_VALUE && GetLastError() != ERROR_BAD_LENGTH) break;
        }
        if (snapshot == INVALID_HANDLE_VALUE) return 0;
        MODULEENTRY32W entry = {};
        entry.dwSize = sizeof entry;
        uintptr_t base = 0;
        if (Module32FirstW(snapshot, &entry)) do
        {
            if (!_wcsicmp(entry.szExePath, path.c_str())) { base = reinterpret_cast<uintptr_t>(entry.modBaseAddr); break; }
        } while (Module32NextW(snapshot, &entry));
        CloseHandle(snapshot);
        return base;
    }

    inline bool RemoteCall(HANDLE process, LPTHREAD_START_ROUTINE entry, void* argument, DWORD& result)
    {
        HANDLE thread = CreateRemoteThread(process, nullptr, 0, entry, argument, 0, nullptr);
        if (!thread) return false;
        const bool ok = WaitForSingleObject(thread, 15000) == WAIT_OBJECT_0 && GetExitCodeThread(thread, &result);
        CloseHandle(thread);
        return ok;
    }

    // Returns the start export's result (1 = every requested patch installed), 0 on any failure.
    inline DWORD Install(const PROCESS_INFORMATION& child, const std::wstring& dll, const char* exportName, uintptr_t mode)
    {
        // Export offset from a local image mapping (DllMain does not run with this flag).
        HMODULE local = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (!local) return 0;
        const FARPROC start = GetProcAddress(local, exportName);
        const uintptr_t offset = start ? reinterpret_cast<uintptr_t>(start) - reinterpret_cast<uintptr_t>(local) : 0;
        FreeLibrary(local);
        if (!offset) return 0;

        const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
        void* remote = VirtualAllocEx(child.hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remote) return 0;
        SIZE_T written = 0;
        // kernel32 sits at the same per-boot address in every process, so our LoadLibraryW is theirs.
        auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        DWORD result = 0;
        const bool loaded = WriteProcessMemory(child.hProcess, remote, dll.c_str(), bytes, &written) && written == bytes &&
                            load && RemoteCall(child.hProcess, load, remote, result);
        VirtualFreeEx(child.hProcess, remote, 0, MEM_RELEASE);
        if (!loaded) return 0;
        // The thread exit code is only the low half of the HMODULE, so look the module up instead.
        const uintptr_t base = RemoteModule(child.dwProcessId, dll);
        if (!base) return 0;
        result = 0;
        if (!RemoteCall(child.hProcess, reinterpret_cast<LPTHREAD_START_ROUTINE>(base + offset),
                        reinterpret_cast<void*>(mode), result)) return 0;
        return result;
    }
}
