// In-process cod2map64 optimizations, injected by BuildPlusWorker into the suspended compiler.
// Each one verifies its byte signatures before any hook is created. Addresses: src/radiant_addrs.h (cod2mapaddr).
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "../../../src/radiant_addrs.h"
#include "../../../src/third_party/minhook/include/MinHook.h"
#include "coalesce.h"
#include "gridtree.h"
#include "vertmerge.h"
#include "gridinit.h"
#include "weld.h"
#include "edges.h"
#include "subgroups.h"
#include "flood.h"
#include "tesssize.h"
#include "groupbounds.h"
#include "soupsplit.h"
#include "qhullarena.h"
#include "filecount.h"

namespace
{
    struct Optimization
    {
        const wchar_t* key;  // LauncherPlus.ini [compile] switch, default on
        const char* name;
        bool (*verify)(uintptr_t base);
        bool (*install)(uintptr_t base, bool verifyMode);
        void (*report)(FILE* out);
    };

    const Optimization kOptimizations[] = {
        { L"c1", "Coalesce Lookup", coalesce::Verify, coalesce::Install, coalesce::Report },
        { L"c2", "Grid Tree", gridtree::Verify, gridtree::Install, gridtree::Report },
        { L"c3", "Vertex Merge", vertmerge::Verify, vertmerge::Install, vertmerge::Report },
        { L"c4", "Edge Neighbours", edges::Verify, edges::Install, edges::Report },
        { L"c5", "Subgroups", subgroups::Verify, subgroups::Install, subgroups::Report },
        { L"c6", "Weld Grid", weld::Verify, weld::Install, weld::Report },
        { L"c7", "Grid Init", gridinit::Verify, gridinit::Install, gridinit::Report },
        { L"c8", "Collision Flood", flood::Verify, flood::Install, flood::Report },
        { L"c9", "TessSize Cache", tesssize::Verify, tesssize::Install, tesssize::Report },
        { L"c10", "Group Bounds", groupbounds::Verify, groupbounds::Install, groupbounds::Report },
        { L"c11", "Soup Split Groups", soupsplit::Verify, soupsplit::Install, soupsplit::Report },
        { L"c12", "Qhull Arena", qhullarena::Verify, qhullarena::Install, qhullarena::Report },
    };

    constexpr size_t kCount = sizeof kOptimizations / sizeof *kOptimizations;
    bool g_enabled[kCount] = {};
    bool g_active = false;

    std::wstring IniPath()
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&IniPath), &self);
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(self, path, MAX_PATH);
        std::wstring ini = path;
        return ini.substr(0, ini.find_last_of(L"\\/")) + L"\\LauncherPlus.ini";
    }

    bool VerifyImage(uintptr_t base)
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
               nt->FileHeader.TimeDateStamp == cod2mapaddr::Timestamp && nt->OptionalHeader.SizeOfImage == cod2mapaddr::ImageSize;
    }
}

// mode 1 = optimize, 2 = verify (also run stock and keep its answer). Returns 1 only when all installed.
extern "C" __declspec(dllexport) DWORD WINAPI BuildPlusCompilerStart(void* mode)
{
    static LONG started = 0;
    if (InterlockedExchange(&started, 1)) return 0;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!VerifyImage(base)) return 0;
    const bool verify = reinterpret_cast<uintptr_t>(mode) == 2;
    const std::wstring ini = IniPath();
    unsigned enabled = 0;
    for (size_t i = 0; i < kCount; ++i)
    {
        g_enabled[i] = GetPrivateProfileIntW(L"compile", kOptimizations[i].key, 1, ini.c_str()) != 0;
        if (g_enabled[i] && !kOptimizations[i].verify(base))
        { fprintf(stdout, "[BetterBuild] %s: signature mismatch.\n", kOptimizations[i].name); fflush(stdout); return 0; }
    }
    if (MH_Initialize() != MH_OK) return 0;
    filecount::Install();  // diagnostic only, BB_COUNT_FILES in the environment
    for (size_t i = 0; i < kCount; ++i)
    {
        if (!g_enabled[i]) continue;
        if (!kOptimizations[i].install(base, verify)) { fprintf(stdout, "[BetterBuild] %s: hook failed.\n", kOptimizations[i].name); fflush(stdout); return 0; }
        ++enabled;
    }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) return 0;
    g_active = true;
    fprintf(stdout, "[BetterBuild] %u of %zu compiler optimizations active%s:", enabled, kCount, verify ? " (verify mode)" : "");
    for (size_t i = 0, n = 0; i < kCount; ++i)
        if (g_enabled[i]) fprintf(stdout, "%s %s", n++ ? "," : "", kOptimizations[i].name);
    fprintf(stdout, "%s.\n", enabled ? "" : " none");
    if (enabled < kCount)
    {
        fprintf(stdout, "[BetterBuild] Off in LauncherPlus.ini:");
        for (size_t i = 0, n = 0; i < kCount; ++i)
            if (!g_enabled[i]) fprintf(stdout, "%s %s", n++ ? "," : "", kOptimizations[i].name);
        fprintf(stdout, ".\n");
    }
    fflush(stdout);
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    if (reason == DLL_PROCESS_DETACH && g_active)
    {
        for (size_t i = 0; i < kCount; ++i)
            if (g_enabled[i]) kOptimizations[i].report(stderr);
        filecount::Report(stderr);
        fflush(stderr);
    }
    return TRUE;
}
