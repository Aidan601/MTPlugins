// Radiant's auto LOD previews from meshoptimizer while LauncherPlus.ini has meshopt_autolod on, so the editor shows what the linker builds.
// Same tfbDecimate code and layout as the linker and APE: plugins/common/meshlod.h does the reduction.
#include "modtools_plugin.hpp"
#include "autolod.h"
#include "meshlod.h"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>

namespace br::autolod
{
namespace
{
    // IDA addresses (no ASLR), resolved through host->RadiantAddress: a different build gets nothing
    const uintptr_t kReduce = 0x1412CAE30;     // (out, model**, lodSpec*, ?, error string*): one auto LOD
    const uintptr_t kSimplify = 0x140EB52F0;   // tfbDecimate simplifier, the linker's 0x7AE170
    const uintptr_t kCacheName = 0x1412CBF70, kCacheRead = 0x1412CBFE0, kCacheWrite = 0x1412CC720;   // "ToolsGfx/xmeshes/v39/<mesh>/pct%d_<key>"
    const uint8_t kSimplifyPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40 };

    using ReduceFn = void* (__fastcall*)(void*, uint8_t**, float*, void*, void*);
    using CacheNameFn = int (__fastcall*)(char*, size_t, const void*, int, const uint32_t*);
    using CacheReadFn = uint64_t (__fastcall*)(void*, const void*, int, const uint32_t*);
    using CacheWriteFn = void (__fastcall*)(const void*, int, const uint32_t*, void*);
    ReduceFn g_reduce = nullptr;
    CacheNameFn g_name = nullptr;
    CacheReadFn g_read = nullptr;
    CacheWriteFn g_write = nullptr;
    std::wstring g_launcherIni;
    bool g_active = false;   // every detour live: keys and reduction switch together

    // the linker reduces with meshoptimizer: same keys BuildPlusLinker and the launcher bridge read
    bool Meshopt()
    {
        const wchar_t* ini = g_launcherIni.c_str();
        return g_active && GetPrivateProfileIntW(L"launcherplus", L"enabled", 1, ini) && GetPrivateProfileIntW(L"linker", L"enabled", 1, ini) &&
               GetPrivateProfileIntW(L"linker", L"patches", 1, ini) && GetPrivateProfileIntW(L"linker", L"meshopt_autolod", 1, ini);
    }

    // meshoptimizer LODs get their own cache files (APEPlus uses the same key), so switching never shows the other simplifier's
    struct Key
    {
        uint32_t k[4];
        const uint32_t* For(int pct, const uint32_t* key)
        {
            if (pct >= 100 || !key || !Meshopt()) return key;
            memcpy(k, key, sizeof k);
            k[3] ^= meshlod::kCacheTag;
            return k;
        }
    };
    int __fastcall hk_CacheName(char* dest, size_t size, const void* mesh, int pct, const uint32_t* key) { Key k; return g_name(dest, size, mesh, pct, k.For(pct, key)); }
    uint64_t __fastcall hk_CacheRead(void* reader, const void* mesh, int pct, const uint32_t* key) { Key k; return g_read(reader, mesh, pct, k.For(pct, key)); }
    void __fastcall hk_CacheWrite(const void* mesh, int pct, const uint32_t* key, void* writer) { Key k; g_write(mesh, pct, k.For(pct, key), writer); }

    // model +8 bones (192 bytes each) as a std::vector begin/end pair, called on worker threads
    void* __fastcall hk_Reduce(void* out, uint8_t** model, float* spec, void* a4, void* err)
    {
        const uint8_t* m = model ? *model : nullptr;
        struct Simplifier
        {
            explicit Simplifier(int stride) { meshlod::t_stride = stride; }
            ~Simplifier() { meshlod::t_stride = 0; meshlod::t_attributes = {}; }
        } simplifier(m && Meshopt() ? int((*reinterpret_cast<const uintptr_t*>(m + 16) - *reinterpret_cast<const uintptr_t*>(m + 8)) / 192) + 11 : 0);
        return g_reduce(out, model, spec, a4, err);
    }

    bool IsFunction(void* at)
    {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fn = at ? RtlLookupFunctionEntry(DWORD64(at), &base, nullptr) : nullptr;
        return fn && DWORD64(fn->BeginAddress) + base == DWORD64(at);
    }
}

void Setup()
{
    if (g_reduce || !rp::host->RadiantAddress || !rp::host->Detour) return;
    void* reduce = rp::host->RadiantAddress(rp::self, kReduce);
    void* simplify = rp::host->RadiantAddress(rp::self, kSimplify);
    void* name = rp::host->RadiantAddress(rp::self, kCacheName);
    void* read = rp::host->RadiantAddress(rp::self, kCacheRead);
    void* write = rp::host->RadiantAddress(rp::self, kCacheWrite);
    if (!IsFunction(reduce) || !IsFunction(simplify) || memcmp(simplify, kSimplifyPrologue, sizeof kSimplifyPrologue) || !IsFunction(name) || !IsFunction(read) || !IsFunction(write))
    {
        rp::Log("autolod: reducer, simplifier or cache names not found, preview LODs stay stock");
        return;
    }
    g_launcherIni = rp::PluginDir();
    g_launcherIni = g_launcherIni.substr(0, g_launcherIni.find_last_of(L"\\/")) + L"\\LauncherPlus\\LauncherPlus.ini";
    // the cache names go first: a reduction is never cached under the other simplifier's key
    const bool ok = rp::host->Detour(rp::self, name, reinterpret_cast<void*>(&hk_CacheName), reinterpret_cast<void**>(&g_name)) &&
                    rp::host->Detour(rp::self, write, reinterpret_cast<void*>(&hk_CacheWrite), reinterpret_cast<void**>(&g_write)) &&
                    rp::host->Detour(rp::self, read, reinterpret_cast<void*>(&hk_CacheRead), reinterpret_cast<void**>(&g_read)) &&
                    rp::host->Detour(rp::self, simplify, reinterpret_cast<void*>(&meshlod::HookSimplify), reinterpret_cast<void**>(&meshlod::g_simplify)) &&
                    rp::host->Detour(rp::self, reduce, reinterpret_cast<void*>(&hk_Reduce), reinterpret_cast<void**>(&g_reduce));
    if (!ok) { rp::Log("autolod: detours failed, preview LODs stay stock"); return; }
    g_active = true;
    rp::Log("autolod: meshopt preview LODs installed (%s now)", Meshopt() ? "on" : "off");
}
}
