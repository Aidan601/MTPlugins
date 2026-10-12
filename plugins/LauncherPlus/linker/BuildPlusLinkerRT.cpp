// In-process linker_modtools optimizations, injected by BuildPlusLinker.exe into the suspended linker.
// The linker's .text is Treyarch-packed, so code patches wait for the first msvcr110!__getmainargs call, API patches install at once.
// L3akMod patches RVAs 0x318781, 0x322AE0 and the getenv_s IAT slot: none here touch those.
#include <winsock2.h>
#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <cmath>
#include <cstdint>
#include <sys/stat.h>
#include <string>
#include <unordered_set>
#include <vector>
#include "../../../src/third_party/minhook/include/MinHook.h"
#include "mip.h"
#include "localized.h"
#include "soundskip.h"
#include "../../common/meshlod.h"

extern "C" LPVOID AllocateBuffer(LPVOID origin);   // MinHook buffer.c: a 64-byte executable slot within 1 GB

namespace
{
    // patch bits, the start routine's argument (built from the [linker] keys)
    enum : unsigned
    {
        kSkipTelemetry = 1u << 0,   // drop the per-asset UDP records sent to 127.0.0.1:6669 (no listener exists)
        kUmbraPoll = 1u << 1,       // poll the Umbra tome workers every 2 ms instead of every 1000 ms
        kReuseD3DDevice = 1u << 2,  // one D3D11 device per thread for image conversion instead of one per image
        kParallelMip = 1u << 3,     // build image mip levels on all cores (bit-identical)
        kReuseWic = 1u << 4,        // keep COM up on the converting thread and reuse one WIC factory
        kPrefetchFiles = 1u << 5,   // open the last link's files on worker threads first (parallel Defender scans)
        kFastStat = 1u << 6,        // the CRT's _stat64i32 with one syscall instead of a directory search (three)
        kFastRecenter = 1u << 7,    // world-load UV recentering without the quadratic scans (bit-identical)

        kVerify = 1u << 8,          // patches that support it also run stock and compare (diagnostic, slower)

        kSkipLocalized = 1u << 9,   // keep zone\en_<map>.ff when none of the localized child's inputs changed (localized.h)
        kSkipSound = 1u << 10,      // do not run sound\snd_convert.exe when none of its inputs changed (soundskip.h)
        kRaiseAutoLod = 1u << 11,   // auto LOD past the stock 220,000-face cap within a memory budget
        kRepairNormals = 1u << 12,  // auto LOD rebuilds zero or NaN normals instead of failing the LOD
        kMeshoptLod = 1u << 13,     // auto LOD reduced by meshoptimizer instead of the stock simplifier
    };

    // a patch on decrypted linker code: RVA, expected bytes, bytes to write
    struct CodePatch
    {
        unsigned bit;
        const char* name;
        uintptr_t rva;
        unsigned char expect[7];
        unsigned char write[7];
        size_t size;
    };

    const CodePatch kCodePatches[] = {
        // linker+0x355540 (Umbra tome conversion) polls its workers then Sleep(1000): only the argument changes (1000 -> 2)
        { kUmbraPoll, "umbra-poll", 0x355972, { 0xB9, 0xE8, 0x03, 0x00, 0x00, 0xFF, 0x15 }, { 0xB9, 0x02, 0x00, 0x00, 0x00, 0xFF, 0x15 }, 7 },
    };

    // linker+0x326B80 sends "\x01<text>" to 127.0.0.1:6669 per asset, constants in network byte order (no ws2_32 import)
    constexpr u_short kTelemetryPortN = 0x0D1A;          // htons(6669)
    constexpr ULONG kLoopbackN = 0x0100007F;             // htonl(INADDR_LOOPBACK)
    unsigned g_patches = 0;

    // names as in LauncherPlus.ini [linker], for the launcher log
    struct Named { unsigned bit; const char* name; };
    const Named kNames[] = {
        { kSkipTelemetry, "skip_telemetry" }, { kUmbraPoll, "umbra_poll" }, { kReuseD3DDevice, "reuse_d3d_device" },
        { kParallelMip, "parallel_mip" }, { kReuseWic, "reuse_wic" }, { kPrefetchFiles, "prefetch_files" },
        { kFastStat, "fast_stat" }, { kFastRecenter, "fast_recenter" }, { kSkipLocalized, "skip_localized" },
        { kSkipSound, "skip_sound_convert" }, { kRaiseAutoLod, "raise_autolod_limit" },
        { kRepairNormals, "repair_autolod_normals" }, { kMeshoptLod, "meshopt_autolod" },
    };
    unsigned g_failed = 0;   // requested code patches whose bytes did not match

    using SendTo = int (WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
    SendTo g_sendto = nullptr;

    int WSAAPI HookSendTo(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen)
    {
        if (to && tolen >= (int)sizeof(sockaddr_in) && to->sa_family == AF_INET)
        {
            const auto* in = reinterpret_cast<const sockaddr_in*>(to);
            if (in->sin_port == kTelemetryPortN && in->sin_addr.s_addr == kLoopbackN)
                return len;   // what a datagram to a closed loopback port returns anyway
        }
        return g_sendto(s, buf, len, flags, to, tolen);
    }

    // Image conversion (linker+0x77F260, call at 0x77F325) makes a D3D11 device per image (about 40% of a cold link's
    // main thread). Identical requests on a thread reuse the first device with its context reset. Requests naming an
    // adapter or software rasterizer, or no device, go straight through.
    using CreateDevice = HRESULT (WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                           ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    CreateDevice g_createDevice = nullptr;

    struct CachedDevice
    {
        D3D_DRIVER_TYPE type;
        UINT flags;
        D3D_FEATURE_LEVEL levels[8];
        UINT levelCount;
        UINT sdk;
        ID3D11Device* device;
        ID3D11DeviceContext* context;
        D3D_FEATURE_LEVEL level;
    };
    thread_local CachedDevice* t_device = nullptr;

    HRESULT WINAPI HookCreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
                                    const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdk,
                                    ID3D11Device** outDevice, D3D_FEATURE_LEVEL* outLevel, ID3D11DeviceContext** outContext)
    {
        if (adapter || software || !outDevice || levelCount > 8)
            return g_createDevice(adapter, type, software, flags, levels, levelCount, sdk, outDevice, outLevel, outContext);
        CachedDevice* c = t_device;
        const bool same = c && c->type == type && c->flags == flags && c->sdk == sdk && c->levelCount == levelCount &&
                          (!levelCount || !memcmp(c->levels, levels, levelCount * sizeof *levels));
        if (!same)
        {
            ID3D11Device* device = nullptr;
            ID3D11DeviceContext* context = nullptr;
            D3D_FEATURE_LEVEL level = {};
            const HRESULT hr = g_createDevice(nullptr, type, nullptr, flags, levels, levelCount, sdk, &device, &level, &context);
            if (FAILED(hr)) return g_createDevice(adapter, type, software, flags, levels, levelCount, sdk, outDevice, outLevel, outContext);
            if (c) { c->context->Release(); c->device->Release(); }
            else c = new CachedDevice();
            c->type = type; c->flags = flags; c->sdk = sdk; c->levelCount = levelCount;
            if (levelCount) memcpy(c->levels, levels, levelCount * sizeof *levels);
            c->device = device; c->context = context; c->level = level;
            t_device = c;
        }
        c->context->ClearState();
        c->context->Flush();
        c->device->AddRef();
        *outDevice = c->device;
        if (outContext) { c->context->AddRef(); *outContext = c->context; }
        if (outLevel) *outLevel = c->level;
        return S_OK;
    }

    // image mip levels (mip.h): 0x7897A0 is decrypted code, so the hook is placed from InstallCodePatches
    constexpr uintptr_t kMipRva = 0x7897A0;
    const unsigned char kMipSignature[] = { 0x44, 0x89, 0x4C, 0x24, 0x20, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
                                            0x48, 0x83, 0xEC, 0x60, 0x4C, 0x8B, 0xF2 };
    constexpr uintptr_t kMipHalfRva = 0x954AF0, kMipPiRva = 0xAEC370;   // the linker's own 0.5 and pi
    using MipFn = void (*)(const float*, const float*, int, int, int, int, int, float*);
    MipFn g_mip = nullptr;
    mip::Pool g_pool;
    double g_half = 0.5, g_pi = 0.0;
    volatile LONG g_mipLevels = 0, g_mipParallel = 0, g_mipMismatches = 0;

    void HookMip(const float* src, const float* weight, int srcW, int srcH, int levelW, int levelH, int mode, float* out)
    {
        InterlockedIncrement(&g_mipLevels);
        const bool shape = levelW > 0 && levelH > 1 && srcW / levelW * levelW == srcW && srcH / levelH * levelH == srcH;
        if (!shape || (long long)srcW * srcH < 65536 || !g_pool.Workers())
            return g_mip(src, weight, srcW, srcH, levelW, levelH, mode, out);
        InterlockedIncrement(&g_mipParallel);
        mip::Job j = {};
        j.src = src; j.weight = weight; j.srcW = srcW; j.mode = mode; j.out = out;
        j.xstep = srcW / levelW; j.ystep = srcH / levelH; j.cols = levelW; j.rows = levelH;
        // stock: ((max(xstep, ystep) * 0.5)^2 * pi) * 0.5, multiplied in this order
        double t = (double)(j.xstep >= j.ystep ? j.xstep : j.ystep) * g_half;
        t = t * t;
        t = t * g_pi;
        j.threshold = t * g_half;
        j.mxcsr = _mm_getcsr();
        g_pool.Run(j);
        if (g_patches & kVerify)
        {
            const size_t bytes = (size_t)levelW * levelH * 16;
            float* stock = static_cast<float*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            if (stock)
            {
                g_mip(src, weight, srcW, srcH, levelW, levelH, mode, stock);
                if (memcmp(stock, out, bytes))
                {
                    if (InterlockedIncrement(&g_mipMismatches) <= 5)
                        fprintf(stderr, "[BetterBuild] mip mismatch: %dx%d -> %dx%d mode %d\n", srcW, srcH, levelW, levelH, mode);
                    memcpy(out, stock, bytes);   // keep stock output
                }
                VirtualFree(stock, 0, MEM_RELEASE);
            }
        }
    }

    bool InstallMip(unsigned char* base)
    {
        if (memcmp(base + kMipRva, kMipSignature, sizeof kMipSignature) ||
            *reinterpret_cast<const double*>(base + kMipHalfRva) != 0.5)
            return false;
        g_half = *reinterpret_cast<const double*>(base + kMipHalfRva);
        g_pi = *reinterpret_cast<const double*>(base + kMipPiRva);
        SYSTEM_INFO si = {};
        GetSystemInfo(&si);
        g_pool.Start((int)si.dwNumberOfProcessors > 1 ? (int)si.dwNumberOfProcessors - 1 : 0);
        return MH_CreateHook(base + kMipRva, reinterpret_cast<LPVOID>(&HookMip), reinterpret_cast<LPVOID*>(&g_mip)) == MH_OK &&
               MH_EnableHook(base + kMipRva) == MH_OK;
    }

    // Image decode (linker+0x78BB70) does CoInitialize, CoCreateInstance(WICImagingFactory), decode, Release and
    // CoUninitialize per image. The first CoInitialize on a thread takes one extra permanent reference so the
    // apartment stays up, and the first factory is handed out again (AddRef'd).
    const GUID kClsidWicFactory = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
    const GUID kIidWicFactory = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
    using CoInit = HRESULT (WINAPI*)(LPVOID);
    using CoCreate = HRESULT (WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
    CoInit g_coInitialize = nullptr;
    CoCreate g_coCreateInstance = nullptr;
    thread_local bool t_comPinned = false;
    thread_local IUnknown* t_wicFactory = nullptr;
    thread_local DWORD t_wicContext = 0;

    HRESULT WINAPI HookCoInitialize(LPVOID reserved)
    {
        if (!t_comPinned && SUCCEEDED(g_coInitialize(nullptr))) t_comPinned = true;
        return g_coInitialize(reserved);
    }

    // CreateDecoderFromFilename tests every installed decoder against the file (about 17% of a cold link's main thread).
    // TIFF and PNG go straight to their container's decoder, anything else or any failure takes the stock path.
    // Verify mode also runs the stock search and compares the decoder CLSID.
    const GUID kContainerTiff = { 0x163bcc30, 0xe2e9, 0x4f0b, { 0x96, 0x1d, 0xa3, 0xe9, 0xfd, 0xb7, 0x88, 0xa3 } };
    const GUID kContainerPng = { 0x1b7cfaf4, 0x713f, 0x473c, { 0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf } };
    using CreateFromFilename = HRESULT (STDMETHODCALLTYPE*)(IWICImagingFactory*, LPCWSTR, const GUID*, DWORD, WICDecodeOptions, IWICBitmapDecoder**);
    CreateFromFilename g_createFromFilename = nullptr;
    volatile LONG g_wicFast = 0, g_wicStock = 0, g_wicMismatches = 0;

    bool DecoderClsid(IWICBitmapDecoder* decoder, CLSID& out)
    {
        IWICBitmapDecoderInfo* info = nullptr;
        if (FAILED(decoder->GetDecoderInfo(&info)) || !info) return false;
        const bool ok = SUCCEEDED(info->GetCLSID(&out));
        info->Release();
        return ok;
    }

    HRESULT STDMETHODCALLTYPE HookCreateFromFilename(IWICImagingFactory* factory, LPCWSTR name, const GUID* vendor, DWORD access,
                                                     WICDecodeOptions options, IWICBitmapDecoder** out)
    {
        IWICStream* stream = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        const GUID* container = nullptr;
        if (!vendor && out && SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(name, access)))
        {
            unsigned char magic[8] = {};
            ULONG got = 0;
            LARGE_INTEGER zero = {};
            if (SUCCEEDED(stream->Read(magic, sizeof magic, &got)) && got == sizeof magic && SUCCEEDED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)))
            {
                if ((magic[0] == 'I' && magic[1] == 'I' && magic[2] == 42 && magic[3] == 0) || (magic[0] == 'M' && magic[1] == 'M' && magic[2] == 0 && magic[3] == 42))
                    container = &kContainerTiff;
                else if (!memcmp(magic, "\x89PNG\r\n\x1a\n", 8))
                    container = &kContainerPng;
            }
            if (container && (FAILED(factory->CreateDecoder(*container, nullptr, &decoder)) || FAILED(decoder->Initialize(stream, options))))
            {
                if (decoder) decoder->Release();
                decoder = nullptr;
            }
        }
        if (stream) stream->Release();   // the decoder holds its own reference
        if (!decoder)
        {
            InterlockedIncrement(&g_wicStock);
            return g_createFromFilename(factory, name, vendor, access, options, out);
        }
        InterlockedIncrement(&g_wicFast);
        if (g_patches & kVerify)
        {
            IWICBitmapDecoder* stock = nullptr;
            CLSID a = {}, b = {};
            if (SUCCEEDED(g_createFromFilename(factory, name, vendor, access, options, &stock)) && stock)
            {
                if (!DecoderClsid(decoder, a) || !DecoderClsid(stock, b) || !IsEqualGUID(a, b))
                {
                    if (InterlockedIncrement(&g_wicMismatches) <= 5) fprintf(stderr, "[BetterBuild] WIC decoder mismatch: %ls\n", name);
                    decoder->Release();
                    decoder = stock;   // keep stock's choice
                    stock = nullptr;
                }
                if (stock) stock->Release();
            }
        }
        *out = decoder;
        return S_OK;
    }

    HRESULT WINAPI HookCoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID* out)
    {
        if (!t_comPinned || outer || !out || !IsEqualGUID(clsid, kClsidWicFactory) || !IsEqualGUID(iid, kIidWicFactory))
            return g_coCreateInstance(clsid, outer, context, iid, out);
        if (!t_wicFactory || t_wicContext != context)
        {
            IUnknown* factory = nullptr;
            const HRESULT hr = g_coCreateInstance(clsid, nullptr, context, iid, reinterpret_cast<LPVOID*>(&factory));
            if (FAILED(hr)) return hr;
            if (t_wicFactory) t_wicFactory->Release();
            t_wicFactory = factory;
            t_wicContext = context;
            // hook the decoder-by-filename method once (slot 3 of IWICImagingFactory, shared by all instances)
            static volatile LONG hooked = 0;
            if (!InterlockedExchange(&hooked, 1))
            {
                void* method = (*reinterpret_cast<void***>(factory))[3];
                if (MH_CreateHook(method, reinterpret_cast<LPVOID>(&HookCreateFromFilename), reinterpret_cast<LPVOID*>(&g_createFromFilename)) != MH_OK ||
                    MH_EnableHook(method) != MH_OK)
                    g_createFromFilename = nullptr;
            }
        }
        t_wicFactory->AddRef();
        *out = t_wicFactory;
        return S_OK;
    }

    // prefetch_files: the link's read-only opens are recorded to plugins\LauncherPlus\linker_prefetch\<map>_<hash>.txt
    // at exit, the next link opens them on worker threads from the start so Defender's scans run in parallel.
    // Handles are closed at once, written paths are dropped from the list.
    using CreateFileWFn = HANDLE (WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    CreateFileWFn g_createFileW = nullptr;
    using CreateFileAFn = HANDLE (WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    CreateFileAFn g_createFileA = nullptr;
    thread_local bool t_prefetcher = false;
    SRWLOCK g_recordLock = SRWLOCK_INIT;
    std::unordered_set<std::wstring>* g_readSeen = nullptr;
    std::vector<std::wstring>* g_readOrder = nullptr;
    std::unordered_set<std::wstring>* g_written = nullptr;
    std::vector<std::wstring>* g_prefetchList = nullptr;
    volatile LONG g_prefetchNext = 0, g_prefetchOpened = 0, g_prefetchLive = 0, g_prefetchDeps = 0;
    ULONGLONG g_prefetchStart = 0, g_prefetchEnd = 0;
    std::wstring g_listFile;
    std::wstring g_tempDir;   // lower case, from GetTempPathW
    constexpr size_t kMaxListed = 200000;

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
        // never listed: temp files (the packer stub reads %TEMP%\linker_modtools.exe-<hash>.stored.tmp first) and SQLite databases
        const auto endsWith = [&](const wchar_t* tail) { const size_t n = wcslen(tail); return key.size() >= n && !key.compare(key.size() - n, n, tail); };
        if (!writes && (endsWith(L".tmp") || endsWith(L".db") ||
                        (!g_tempDir.empty() && key.size() > g_tempDir.size() && !key.compare(0, g_tempDir.size(), g_tempDir) &&
                         (key[g_tempDir.size()] == L'\\' || key[g_tempDir.size()] == L'/'))))
            return;
        AcquireSRWLockExclusive(&g_recordLock);
        if (writes) g_written->insert(std::move(key));
        else if (g_readOrder->size() < kMaxListed && g_readSeen->insert(key).second) g_readOrder->push_back(std::move(key));
        ReleaseSRWLockExclusive(&g_recordLock);
    }

    HANDLE WINAPI HookCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE templ)
    {
        const HANDLE h = g_createFileW(name, access, share, sa, disposition, flags, templ);
        if (t_prefetcher || !name || h == INVALID_HANDLE_VALUE) return h;
        const DWORD error = GetLastError();
        Record(name, access, disposition, flags);
        SetLastError(error);
        return h;
    }

    // kernelbase!CreateFileA does not go through CreateFileW, and the linker opens most files with it
    HANDLE WINAPI HookCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE templ)
    {
        const HANDLE h = g_createFileA(name, access, share, sa, disposition, flags, templ);
        if (t_prefetcher || !name || h == INVALID_HANDLE_VALUE) return h;
        const DWORD error = GetLastError();
        wchar_t wide[MAX_PATH * 2];
        if (MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH * 2) > 0) Record(wide, access, disposition, flags);
        SetLastError(error);
        return h;
    }

    DWORD WINAPI PrefetchMain(void*)
    {
        t_prefetcher = true;
        const std::vector<std::wstring>& list = *g_prefetchList;
        for (;;)
        {
            const LONG i = InterlockedIncrement(&g_prefetchNext) - 1;
            if (i >= (LONG)list.size()) break;
            const HANDLE h = CreateFileW(list[i].c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); InterlockedIncrement(&g_prefetchOpened); }
        }
        if (InterlockedDecrement(&g_prefetchLive) == 0) g_prefetchEnd = GetTickCount64();
        return 0;
    }

    std::wstring g_mapName;   // the -modsource value, for the deps path

    // the linker's .deps list (usermaps\<map>\zone_source\all\assetinfo\<map>.deps, "file,<path>,<mtime>," lines) names every source
    // a link reads, including those a cold conversion opens: its entries join the prefetch list
    void AddDepsToPrefetch(std::unordered_set<std::wstring>& seen)
    {
        wchar_t game[MAX_PATH * 2] = {};
        if (g_mapName.empty() || !GetEnvironmentVariableW(L"TA_GAME_PATH", game, MAX_PATH * 2)) return;
        const std::wstring deps = std::wstring(game) + L"\\usermaps\\" + g_mapName + L"\\zone_source\\all\\assetinfo\\" + g_mapName + L".deps";
        const HANDLE f = CreateFileW(deps.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER size = {};
        std::string text;
        if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (64 << 20))
        {
            text.resize((size_t)size.QuadPart);
            DWORD got = 0;
            if (!ReadFile(f, &text[0], (DWORD)text.size(), &got, nullptr)) got = 0;
            text.resize(got);
        }
        CloseHandle(f);
        for (size_t at = 0; at < text.size();)
        {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            size_t p = at;
            while (p < end && (text[p] == ' ' || text[p] == '\t')) ++p;
            if (end - p > 5 && !text.compare(p, 5, "file,"))
            {
                p += 5;
                size_t q = text.find(',', p);
                if (q == std::string::npos || q > end) q = end;
                if (q > p && text[q - 1] == '\r') --q;
                std::wstring w(q - p, L'\0');
                w.resize(MultiByteToWideChar(CP_UTF8, 0, text.data() + p, (int)(q - p), &w[0], (int)(q - p)));
                if (!w.empty()) CharLowerBuffW(&w[0], (DWORD)w.size());
                const bool db = w.size() > 3 && !w.compare(w.size() - 3, 3, L".db");
                const bool tmp = w.size() > 4 && !w.compare(w.size() - 4, 4, L".tmp");
                // only sources outside the map's own folders (the link writes under usermaps\<map> and zone_source)
                const bool own = w.find(L"\\usermaps\\" + g_mapName + L"\\") != std::wstring::npos ||
                                 w.find(L"\\zone_source\\") != std::wstring::npos || w.find(L"\\zone\\") != std::wstring::npos;
                static const bool all = GetEnvironmentVariableW(L"BB_PREFETCH_ALL", nullptr, 0) > 0;   // diagnostics only
                if (!w.empty() && !db && !tmp && (!own || all) && seen.insert(w).second) g_prefetchList->push_back(std::move(w));
            }
            at = end + 1;
        }
    }

    // list name: the -modsource value plus a hash of all arguments
    std::wstring ListFileName()
    {
        const wchar_t* line = GetCommandLineW();
        const wchar_t* args = line;
        if (*args == L'"') { args = wcschr(args + 1, L'"'); args = args ? args + 1 : line + wcslen(line); }
        else while (*args && *args != L' ') ++args;
        const std::wstring lower = Lower(args);
        unsigned hash = 2166136261u;
        for (wchar_t c : lower) hash = (hash ^ c) * 16777619u;
        std::wstring map = L"link";
        const size_t flag = lower.find(L"-modsource ");
        if (flag != std::wstring::npos)
        {
            size_t at = lower.find_first_not_of(L' ', flag + 11);
            if (at != std::wstring::npos) map = lower.substr(at, lower.find(L' ', at) - at);
        }
        g_mapName = map;
        for (wchar_t& c : map) if (!iswalnum(c) && c != L'_' && c != L'-') c = L'_';
        wchar_t name[64];
        swprintf_s(name, L"_%08x.txt", hash);
        return map.substr(0, 64) + name;
    }

    void StartPrefetch()
    {
        t_prefetcher = true;   // this thread's own opens are not the link's
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&StartPrefetch), &self);
        wchar_t path[MAX_PATH * 2] = {};
        const DWORD n = GetModuleFileNameW(self, path, MAX_PATH * 2);
        if (!n || n >= MAX_PATH * 2) return;
        std::wstring dir(path);
        dir = dir.substr(0, dir.find_last_of(L"\\/")) + L"\\linker_prefetch";
        g_listFile = dir + L"\\" + ListFileName();

        g_prefetchList = new std::vector<std::wstring>();
        const HANDLE f = CreateFileW(g_listFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        LARGE_INTEGER size = {};
        std::string text;   // empty when there is no list yet
        if (f != INVALID_HANDLE_VALUE)
        {
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
                if (!w.empty()) g_prefetchList->push_back(std::move(w));
            }
            at = end + 1;
        }
        {
            std::unordered_set<std::wstring> seen(g_prefetchList->begin(), g_prefetchList->end());
            g_prefetchDeps = -(LONG)g_prefetchList->size();
            AddDepsToPrefetch(seen);
            g_prefetchDeps += (LONG)g_prefetchList->size();
        }
        if (g_prefetchList->empty()) return;
        SYSTEM_INFO info = {};
        GetSystemInfo(&info);
        const int cores = (int)info.dwNumberOfProcessors;
        const int threads = cores < 4 ? 4 : cores > 16 ? 16 : cores;
        g_prefetchStart = GetTickCount64();
        g_prefetchLive = threads;
        for (int i = 0; i < threads; ++i)
        {
            const HANDLE t = CreateThread(nullptr, 0, &PrefetchMain, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
            else if (InterlockedDecrement(&g_prefetchLive) == 0) g_prefetchEnd = GetTickCount64();
        }
    }

    // at exit() with every thread alive: report, then replace the list with this link's (tmp + rename)
    void SavePrefetchList()
    {
        if (!g_readOrder || g_listFile.empty()) return;
        t_prefetcher = true;   // the list file is not part of the link
        if (!g_prefetchList->empty())
        {
            const ULONGLONG end = g_prefetchEnd ? g_prefetchEnd : GetTickCount64();
            fprintf(stdout, "[BetterBuild] Prefetch: opened %ld of %zu files (%ld more from the map's .deps) ahead of the linker in %.1f s%s.\n",
                    g_prefetchOpened, g_prefetchList->size(), g_prefetchDeps, (end - g_prefetchStart) / 1000.0, g_prefetchEnd ? "" : " (still running at exit)");
            fflush(stdout);
        }
        std::string text;
        AcquireSRWLockExclusive(&g_recordLock);
        for (const std::wstring& p : *g_readOrder)
        {
            if (g_written->count(p)) continue;
            const int need = WideCharToMultiByte(CP_UTF8, 0, p.data(), (int)p.size(), nullptr, 0, nullptr, nullptr);
            const size_t at = text.size();
            text.resize(at + need);
            WideCharToMultiByte(CP_UTF8, 0, p.data(), (int)p.size(), &text[at], need, nullptr, nullptr);
            text += '\n';
        }
        ReleaseSRWLockExclusive(&g_recordLock);
        if (text.empty()) return;
        const std::wstring dir = g_listFile.substr(0, g_listFile.find_last_of(L'\\'));
        CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring tmp = g_listFile + L".tmp";
        const HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        DWORD wrote = 0;
        const bool ok = WriteFile(f, text.data(), (DWORD)text.size(), &wrote, nullptr) && wrote == text.size();
        CloseHandle(f);
        if (!ok || !MoveFileExW(tmp.c_str(), g_listFile.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
    }

    using ExitFn = void (__cdecl*)(int);
    ExitFn g_exit = nullptr;
    void __cdecl HookExit(int code)
    {
        static LONG once = 0;
        if (!InterlockedExchange(&once, 1)) SavePrefetchList();
        g_exit(code);
    }

    // fast_stat: the dependency recorder (linker+0x31CF20) stats every file. While _stat64i32 runs on this thread,
    // FindFirstFileExW for an exact name answers from GetFileAttributesExW (one syscall) with a fake handle FindClose
    // accepts. The CRT time conversion is unchanged, so st_mtime (the key of datasignature.db) is identical.
    using StatFn = int (__cdecl*)(const char*, struct _stat64i32*);
    using FindFirstFileExWFn = HANDLE (WINAPI*)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
    using FindCloseFn = BOOL (WINAPI*)(HANDLE);
    StatFn g_stat = nullptr;
    FindFirstFileExWFn g_findFirstFileExW = nullptr;
    FindCloseFn g_findClose = nullptr;
    thread_local bool t_inStat = false;
    char g_fakeFindTag = 0;
    volatile LONG g_statCalls = 0, g_statFast = 0, g_statMismatches = 0, g_statAtimeOnly = 0;
    volatile LONG64 g_statFastTicks = 0, g_statStockTicks = 0;

    HANDLE WINAPI HookFindFirstFileExW(LPCWSTR name, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags)
    {
        if (t_inStat && name && data && !filter && op == FindExSearchNameMatch && (level == FindExInfoStandard || level == FindExInfoBasic) && !wcspbrk(name, L"*?"))
        {
            const size_t n = wcslen(name);
            if (n && n < MAX_PATH && name[n - 1] != L'\\' && name[n - 1] != L'/' && name[n - 1] != L':')
            {
                WIN32_FILE_ATTRIBUTE_DATA a;
                if (!GetFileAttributesExW(name, GetFileExInfoStandard, &a)) return INVALID_HANDLE_VALUE;
                WIN32_FIND_DATAW* d = static_cast<WIN32_FIND_DATAW*>(data);
                d->dwFileAttributes = a.dwFileAttributes;
                d->ftCreationTime = a.ftCreationTime;
                d->ftLastAccessTime = a.ftLastAccessTime;
                d->ftLastWriteTime = a.ftLastWriteTime;
                d->nFileSizeHigh = a.nFileSizeHigh;
                d->nFileSizeLow = a.nFileSizeLow;
                d->dwReserved0 = 0;
                d->dwReserved1 = 0;
                size_t tail = n;
                while (tail && name[tail - 1] != L'\\' && name[tail - 1] != L'/' && name[tail - 1] != L':') --tail;
                wcscpy_s(d->cFileName, name + tail);
                d->cAlternateFileName[0] = 0;
                InterlockedIncrement(&g_statFast);
                return &g_fakeFindTag;
            }
        }
        return g_findFirstFileExW(name, level, data, op, filter, flags);
    }

    BOOL WINAPI HookFindClose(HANDLE h)
    {
        if (h == &g_fakeFindTag) return TRUE;
        return g_findClose(h);
    }

    int __cdecl HookStat(const char* path, struct _stat64i32* buf)
    {
        if (t_inStat) return g_stat(path, buf);
        InterlockedIncrement(&g_statCalls);
        LARGE_INTEGER t0, t1, t2;
        QueryPerformanceCounter(&t0);
        t_inStat = true;
        int r = g_stat(path, buf);
        t_inStat = false;
        QueryPerformanceCounter(&t1);
        if (g_patches & kVerify)
        {
            struct _stat64i32 stock = {};
            const int r2 = g_stat(path, &stock);
            QueryPerformanceCounter(&t2);
            InterlockedAdd64(&g_statFastTicks, t1.QuadPart - t0.QuadPart);
            InterlockedAdd64(&g_statStockTicks, t2.QuadPart - t1.QuadPart);
            // st_atime may differ (file record vs directory entry), no caller of the linker's stat wrapper uses it
            const bool same = r == r2 && (r != 0 ||
                (buf->st_size == stock.st_size && buf->st_mtime == stock.st_mtime && buf->st_ctime == stock.st_ctime &&
                 buf->st_mode == stock.st_mode && buf->st_dev == stock.st_dev &&
                 buf->st_rdev == stock.st_rdev && buf->st_nlink == stock.st_nlink));
            if (same && r == 0 && buf->st_atime != stock.st_atime) InterlockedIncrement(&g_statAtimeOnly);
            if (!same)
            {
                if (InterlockedIncrement(&g_statMismatches) <= 5)
                    fprintf(stderr, "[BetterBuild] stat verify mismatch: %s (fast r=%d size=%ld mtime=%lld atime=%lld ctime=%lld, stock r=%d size=%ld mtime=%lld atime=%lld ctime=%lld)\n",
                            path, r, (long)buf->st_size, (long long)buf->st_mtime, (long long)buf->st_atime, (long long)buf->st_ctime,
                            r2, (long)stock.st_size, (long long)stock.st_mtime, (long long)stock.st_atime, (long long)stock.st_ctime);
                *buf = stock;
                r = r2;
            }
        }
        return r;
    }

    // fast_recenter: R_LoadWorld (linker+0x203EA0) recenters UVs per connected triangle set (material field at +632).
    // Stock's set search (linker+0x1EB480) is quadratic. This visits triangles in stock's order with the same float
    // operations (identical bytes), an out-of-range vertex index goes to stock.
    constexpr uintptr_t kRecenterRva = 0x203EA0, kWorldPtrRva = 0xC862B8;   // the world, +672 = index buffer
    const unsigned char kRecenterSignature[] = { 0x4C, 0x8B, 0xDC, 0x55, 0x56, 0x48, 0x81, 0xEC, 0xC8, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05 };
    using RecenterFn = void (*)(unsigned char*, float*);
    RecenterFn g_recenter = nullptr;
    unsigned char* g_base = nullptr;
    volatile LONG g_recenterCalls = 0, g_recenterFast = 0, g_recenterMismatches = 0;
    volatile LONG64 g_recenterFastTicks = 0, g_recenterStockTicks = 0;   // verify mode: time of each version

    LONG64 Ticks() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
    double Ms(LONG64 ticks) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return 1000.0 * (double)ticks / (double)f.QuadPart; }

    struct RecenterWork
    {
        std::vector<int> start, list;        // vertex -> triangles (CSR, ascending)
        std::vector<int> stamp;              // vertex -> component that holds it
        std::vector<unsigned char> visited;  // triangle visited
        std::vector<int> compVerts;
        struct Frame { int tri; int p[3]; };
        std::vector<Frame> stack;
    };

    // false when an index is out of range (stock then handles the surface)
    bool RecenterFast(unsigned char* s, float* verts)
    {
        thread_local RecenterWork w;
        const int firstVert = *reinterpret_cast<int*>(s + 32);
        const int vertCount = *reinterpret_cast<unsigned short*>(s + 40), triCount = *reinterpret_cast<unsigned short*>(s + 42);
        const int firstIndex = *reinterpret_cast<int*>(s + 44);
        const unsigned short* idx = *reinterpret_cast<unsigned short**>(*reinterpret_cast<unsigned char**>(g_base + kWorldPtrRva) + 672) + firstIndex;
        const int n = 3 * triCount;
        for (int i = 0; i < n; ++i) if (idx[i] >= vertCount) return false;
        w.start.assign(vertCount + 1, 0);
        for (int i = 0; i < n; ++i) ++w.start[idx[i] + 1];
        for (int v = 0; v < vertCount; ++v) w.start[v + 1] += w.start[v];
        w.list.resize(n);
        {
            std::vector<int>& fill = w.stamp;   // reused as the fill cursor, reset below
            fill.assign(w.start.begin(), w.start.end() - 1);
            for (int t = 0; t < triCount; ++t)
                for (int k = 0; k < 3; ++k) w.list[fill[idx[3 * t + k]]++] = t;   // ascending triangle order per vertex
        }
        w.stamp.assign(vertCount, -1);
        w.visited.assign(triCount, 0);
        for (int seed = 0; seed < triCount; ++seed)
        {
            if (w.visited[seed]) continue;
            float minU = 3.4028235e38f, minV = 3.4028235e38f, maxU = -3.4028235e38f, maxV = -3.4028235e38f;
            w.compVerts.clear();
            const auto visit = [&](int t) {
                w.visited[t] = 1;
                for (int k = 0; k < 3; ++k)
                {
                    const int vi = idx[3 * t + k];
                    if (w.stamp[vi] == seed) continue;
                    w.stamp[vi] = seed;
                    w.compVerts.push_back(vi);
                    const float* uv = verts + 5 * (size_t)(firstVert + vi) + 1;
                    if (uv[0] <= minU) minU = uv[0];
                    if (uv[1] <= minV) minV = uv[1];
                    if (maxU <= uv[0]) maxU = uv[0];
                    if (maxV <= uv[1]) maxV = uv[1];
                }
                RecenterWork::Frame f;
                f.tri = t;
                for (int k = 0; k < 3; ++k) f.p[k] = w.start[idx[3 * t + k]];
                w.stack.push_back(f);
            };
            visit(seed);
            while (!w.stack.empty())
            {
                RecenterWork::Frame& f = w.stack.back();
                int next = -1;
                for (int k = 0; k < 3; ++k)
                {
                    const int end = w.start[idx[3 * f.tri + k] + 1];
                    if (f.p[k] < end && (next < 0 || w.list[f.p[k]] < next)) next = w.list[f.p[k]];
                }
                if (next < 0) { w.stack.pop_back(); continue; }
                for (int k = 0; k < 3; ++k)
                    if (f.p[k] < w.start[idx[3 * f.tri + k] + 1] && w.list[f.p[k]] == next) ++f.p[k];
                if (!w.visited[next]) visit(next);   // pushes a frame, f is re-fetched next loop
            }
            const float midV = floorf((maxV + minV) * 0.5f);
            const float midU = floorf((maxU + minU) * 0.5f);
            for (int vi : w.compVerts)
            {
                float* uv = verts + 5 * (size_t)(firstVert + vi) + 1;
                uv[0] = uv[0] - midU;
                uv[1] = uv[1] - midV;
            }
        }
        return true;
    }

    void HookRecenter(unsigned char* s, float* verts)
    {
        InterlockedIncrement(&g_recenterCalls);
        const unsigned char* material = *reinterpret_cast<unsigned char**>(s + 72);
        if (!material || !*reinterpret_cast<const uint64_t*>(material + 632)) return;   // stock does nothing either
        const int firstVert = *reinterpret_cast<int*>(s + 32);
        const int vertCount = *reinterpret_cast<unsigned short*>(s + 40);
        if (g_patches & kVerify)
        {
            // stock runs on a copy of the surface's vertices (shifted pointer), on any difference its bytes replace ours
            std::vector<unsigned char> copy(reinterpret_cast<unsigned char*>(verts + 5 * (size_t)firstVert), reinterpret_cast<unsigned char*>(verts + 5 * (size_t)(firstVert + vertCount)));
            const LONG64 t0 = Ticks();
            const bool fast = RecenterFast(s, verts);
            const LONG64 t1 = Ticks();
            if (!fast) { g_recenter(s, verts); return; }
            InterlockedIncrement(&g_recenterFast);
            InterlockedAdd64(&g_recenterFastTicks, t1 - t0);
            float* shifted = reinterpret_cast<float*>(copy.data() - 20 * (size_t)firstVert);
            const LONG64 t2 = Ticks();
            g_recenter(s, shifted);
            InterlockedAdd64(&g_recenterStockTicks, Ticks() - t2);
            unsigned char* real = reinterpret_cast<unsigned char*>(verts + 5 * (size_t)firstVert);
            if (memcmp(real, copy.data(), copy.size()) != 0)
            {
                InterlockedIncrement(&g_recenterMismatches);
                memcpy(real, copy.data(), copy.size());
            }
            return;
        }
        if (RecenterFast(s, verts)) InterlockedIncrement(&g_recenterFast);
        else g_recenter(s, verts);
    }

    bool InstallRecenter(unsigned char* base)
    {
        g_base = base;
        unsigned char* at = base + kRecenterRva;
        if (memcmp(at, kRecenterSignature, sizeof kRecenterSignature) != 0) return false;
        return MH_CreateHook(at, reinterpret_cast<LPVOID>(&HookRecenter), reinterpret_cast<LPVOID*>(&g_recenter)) == MH_OK && MH_EnableHook(at) == MH_OK;
    }

    // Auto LOD reducer (linker+0x76B800). raise_autolod_limit: past stock's 220,000 faces while the memory estimate fits
    // a shared budget. repair_autolod_normals: zero or NaN normals rebuilt instead of failing the LOD.
    constexpr uintptr_t kAutoLodRva = 0x76B800, kAutoLodFaceCapRva = 0x76B91E, kAutoLodNormalCheckRva = 0x76C122, kSimplifyRva = 0x7AE170;
    const unsigned char kSimplifySignature[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40 };
    const unsigned char kAutoLodSignature[] = { 0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
    const unsigned char kAutoLodFaceCap[] = { 0x81, 0xFB, 0x60, 0x5B, 0x03, 0x00, 0x76 };    // cmp ebx, 220000 / jbe
    const unsigned char kAutoLodFaceCapOff[] = { 0x81, 0xFB, 0xFF, 0xFF, 0xFF, 0xFF, 0x76 }; // cmp ebx, 0xFFFFFFFF / jbe
    const unsigned char kAutoLodNormalCheck[] = { 0x44, 0x0F, 0x2F, 0xD1, 0x77, 0x0F, 0x48, 0x8D, 0x15 };   // comiss / ja ok / lea msg
    const unsigned char kAutoLodNormalCheckOff[] = { 0x44, 0x0F, 0x2F, 0xD1, 0xEB, 0x0F, 0x48, 0x8D, 0x15 }; // ja -> jmp
    constexpr unsigned kStockAutoLodFaces = 220000;
    // xmesh cache name "_f%d_s%f_%s%s%s" (faces, scale, geo/collision, _rigid, _origuv): va() call in both converters
    constexpr uintptr_t kMeshNameCallRva[2] = { 0x7768BD, 0x7778E9 }, kVaRva = 0x2CF4B0;
    using VaFn = const char* (*)(const char*, ...);
    VaFn g_va = nullptr;

    // dev: BB_MESHOPT_STOCK_FACES=<n>,<n>,... keeps those target face counts on the stock simplifier (side-by-side tests)
    bool KeepStock(unsigned faces)
    {
        static const std::vector<unsigned> list = [] {
            std::vector<unsigned> r;
            char s[2048] = {};
            if (GetEnvironmentVariableA("BB_MESHOPT_STOCK_FACES", s, sizeof s))
                for (char* p = s; *p; ++p) if (unsigned n = strtoul(p, &p, 10)) { r.push_back(n); if (!*p) break; }
            return r;
        }();
        for (unsigned n : list) if (n == faces) return true;
        return false;
    }

    double SimplifyKeepStock(void* params, meshlod::Vec* faces, meshlod::Vec* corners, double a4, unsigned target, float a6, meshlod::Vec* out)
    {
        return (KeepStock(target) ? meshlod::g_simplify : meshlod::HookSimplify)(params, faces, corners, a4, target, a6, out);
    }

    // reduced LODs from meshoptimizer get their own cache entries: switching never reuses the other simplifier's
    const char* MeshCacheName(const char* fmt, int faces, double scale, const char* kind, const char* rigid, const char* origuv)
    {
        return g_va(faces && !KeepStock(unsigned(faces)) ? "_f%d_s%f_%s%s%s_mo3" : fmt, faces, scale, kind, rigid, origuv);
    }

    bool WriteCode(unsigned char* at, const unsigned char* bytes, size_t n);

    bool InstallMeshCacheName(unsigned char* base)
    {
        for (uintptr_t rva : kMeshNameCallRva)
        {
            unsigned char* call = base + rva;
            const unsigned char* fmt = call + *reinterpret_cast<const int32_t*>(call - 4);   // lea rcx, [rip + fmt] just before
            if (call[0] != 0xE8 || call + 5 + *reinterpret_cast<const int32_t*>(call + 1) != base + kVaRva ||
                call[-7] != 0x48 || call[-6] != 0x8D || call[-5] != 0x0D || strcmp(reinterpret_cast<const char*>(fmt), "_f%d_s%f_%s%s%s"))
                return false;
        }
        g_va = reinterpret_cast<VaFn>(base + kVaRva);
        for (uintptr_t rva : kMeshNameCallRva)
        {
            unsigned char* call = base + rva;
            unsigned char* stub = static_cast<unsigned char*>(AllocateBuffer(call));
            if (!stub) return false;
            const uint64_t target = reinterpret_cast<uint64_t>(&MeshCacheName);
            stub[0] = 0x48; stub[1] = 0xB8; memcpy(stub + 2, &target, 8); stub[10] = 0xFF; stub[11] = 0xE0;   // mov rax, imm64 / jmp rax
            FlushInstructionCache(GetCurrentProcess(), stub, 12);
            unsigned char rel[5] = { 0xE8 };
            const int32_t d = int32_t(stub - (call + 5));
            memcpy(rel + 1, &d, 4);
            if (!WriteCode(call, rel, 5)) return false;
        }
        return true;
    }
    constexpr int kStockAutoLodBones = 1000;   // stock's bone check stays in place
    using AutoLodFn = void (*)(unsigned char*, void*, float*);
    AutoLodFn g_autoLod = nullptr;
    unsigned long long g_autoLodBudget = 0;    // bytes for all reductions past the stock cap together, set at install
    bool g_autoLodRaise = false, g_autoLodRepair = false, g_autoLodMeshopt = false;
    SRWLOCK g_autoLodLock = SRWLOCK_INIT;
    CONDITION_VARIABLE g_autoLodFree = CONDITION_VARIABLE_INIT;
    unsigned long long g_autoLodInUse = 0;     // estimates of the running reductions past the stock cap, under g_autoLodLock
    volatile LONG g_autoLodNormalsFixed = 0;

    // measured peak per reduction: about 2 KB per face plus 48 bytes per face and weight slot (bones + 11)
    unsigned long long AutoLodBytes(unsigned faces, int bones) { return (unsigned long long)faces * (2048ull + 48ull * (unsigned)(bones + 11)); }

    bool UnitNormal(const float* n)
    {
        const float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        return fabsf(len - 1.0f) < 0.002f;   // stock's test, false for NaN
    }

    // model +76 faces, +144 vertices (288 bytes, position at +132), +160 faces (208 bytes, corner k at +56k: +8 vertex, +12 normal)
    int RepairAutoLodNormals(unsigned char* model)
    {
        const int faces = *reinterpret_cast<int*>(model + 76);
        unsigned char* faceBase = *reinterpret_cast<unsigned char**>(model + 160);
        unsigned char* vertBase = *reinterpret_cast<unsigned char**>(model + 144);
        if (faces <= 0 || !faceBase || !vertBase) return 0;
        const auto corner = [&](int f, int k) { return reinterpret_cast<float*>(faceBase + 208ull * f + 8 + 56ull * k + 4); };
        const auto geometric = [&](int f, float* out) {
            const float* p[3];
            for (int k = 0; k < 3; ++k) p[k] = reinterpret_cast<float*>(vertBase + 288ull * *reinterpret_cast<int*>(faceBase + 208ull * f + 8 + 56ull * k) + 132);
            const float a[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] }, b[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
            out[0] = a[1] * b[2] - a[2] * b[1]; out[1] = a[2] * b[0] - a[0] * b[2]; out[2] = a[0] * b[1] - a[1] * b[0];
        };
        int winding = 0;   // +1: the corner normals follow (p1-p0)x(p2-p0), -1: the opposite, 0: not measured yet
        int fixed = 0;
        for (int f = 0; f < faces; ++f)
        {
            float* n[3] = { corner(f, 0), corner(f, 1), corner(f, 2) };
            const bool ok[3] = { UnitNormal(n[0]), UnitNormal(n[1]), UnitNormal(n[2]) };
            if (ok[0] && ok[1] && ok[2]) continue;
            float avg[3] = { 0, 0, 0 };
            for (int k = 0; k < 3; ++k)
                if (ok[k]) { avg[0] += n[k][0]; avg[1] += n[k][1]; avg[2] += n[k][2]; }
            float len = sqrtf(avg[0] * avg[0] + avg[1] * avg[1] + avg[2] * avg[2]);
            if (!(len > 1e-6f))
            {
                if (!winding)
                {
                    long long votes = 0;   // sign agreement over faces whose normals are all valid
                    for (int g = 0; g < faces && (votes < 64 && votes > -64); ++g)
                    {
                        if (!UnitNormal(corner(g, 0)) || !UnitNormal(corner(g, 1)) || !UnitNormal(corner(g, 2))) continue;
                        float c[3]; geometric(g, c);
                        const float* m = corner(g, 0);
                        const float d = c[0] * m[0] + c[1] * m[1] + c[2] * m[2];
                        votes += d > 0 ? 1 : (d < 0 ? -1 : 0);
                    }
                    winding = votes < 0 ? -1 : 1;
                }
                geometric(f, avg);
                avg[0] *= (float)winding; avg[1] *= (float)winding; avg[2] *= (float)winding;
                len = sqrtf(avg[0] * avg[0] + avg[1] * avg[1] + avg[2] * avg[2]);
                if (!(len > 1e-12f)) { avg[0] = 0; avg[1] = 0; avg[2] = 1; len = 1; }   // zero-area face
            }
            for (int k = 0; k < 3; ++k)
                if (!ok[k]) { n[k][0] = avg[0] / len; n[k][1] = avg[1] / len; n[k][2] = avg[2] / len; ++fixed; }
        }
        return fixed;
    }

    // dev: BB_AUTOLOD_DUMP=<dir> writes every reducer input and output as <dir>\<input faces>_<output faces>_<in|out>_<n>.obj
    void DumpAutoLodModel(const unsigned char* model, unsigned inFaces, const char* tag)
    {
        char dir[MAX_PATH] = {};
        if (!GetEnvironmentVariableA("BB_AUTOLOD_DUMP", dir, sizeof dir)) return;
        static volatile LONG serial = 0;
        const int faces = *reinterpret_cast<const int*>(model + 76);
        char path[MAX_PATH + 64];
        snprintf(path, sizeof path, "%s\\%u_%d_%s_%ld.obj", dir, inFaces, faces, tag, InterlockedIncrement(&serial));
        FILE* f = fopen(path, "wb");
        if (!f) return;
        const unsigned char* faceBase = *reinterpret_cast<unsigned char* const*>(model + 160);
        const unsigned char* vertBase = *reinterpret_cast<unsigned char* const*>(model + 144);
        for (int i = 0; i < faces; ++i)
            for (int k = 0; k < 3; ++k)
            {
                const unsigned char* c = faceBase + 208ull * i + 8 + 56ull * k;
                const float* p = reinterpret_cast<const float*>(vertBase + 288ull * *reinterpret_cast<const int*>(c) + 132);
                const float* n = reinterpret_cast<const float*>(c + 4);
                const float* uv = reinterpret_cast<const float*>(c + 32);
                fprintf(f, "v %g %g %g\nvn %g %g %g\nvt %g %g\n", p[0], p[1], p[2], n[0], n[1], n[2], uv[0], uv[1]);
            }
        for (int i = 0; i < faces; ++i)
            fprintf(f, "f %d/%d/%d %d/%d/%d %d/%d/%d\n", 3 * i + 1, 3 * i + 1, 3 * i + 1, 3 * i + 2, 3 * i + 2, 3 * i + 2, 3 * i + 3, 3 * i + 3, 3 * i + 3);
        fclose(f);
    }

    void HookAutoLod(unsigned char* model, void* reader, float* spec)
    {
        const unsigned faces = *reinterpret_cast<unsigned*>(model + 76);
        const int bones = *reinterpret_cast<int*>(model + 88) + *reinterpret_cast<int*>(model + 124);
        const bool big = g_autoLodRaise && faces > kStockAutoLodFaces && bones >= 0 && bones <= kStockAutoLodBones;
        if (big)
        {
            const unsigned long long need = AutoLodBytes(faces, bones);
            if (need > g_autoLodBudget || 3ull * faces * (unsigned)(bones + 11) >= 0xFFFFFFFFull)
            {
                // the linker's xmodel converter catches its own char* messages like this one
                thread_local char msg[320];
                snprintf(msg, sizeof msg, "Model is too complex for auto LOD, %u faces with %d bones needs about %llu MB (budget %llu MB, LauncherPlus.ini [linker] autolod_budget_mb)\n",
                         faces, bones, need >> 20, g_autoLodBudget >> 20);
                throw static_cast<char*>(msg);
            }
            fprintf(stdout, "[BetterBuild] Auto LOD past the stock limit: %u faces, %d bones (stock max %u faces, about %llu MB of a %llu MB budget).\n",
                    faces, bones, kStockAutoLodFaces, need >> 20, g_autoLodBudget >> 20);
            fflush(stdout);
            AcquireSRWLockExclusive(&g_autoLodLock);
            while (g_autoLodInUse && g_autoLodInUse + need > g_autoLodBudget) SleepConditionVariableSRW(&g_autoLodFree, &g_autoLodLock, INFINITE, 0);
            g_autoLodInUse += need;
            ReleaseSRWLockExclusive(&g_autoLodLock);
        }
        struct Release
        {
            unsigned long long n;
            ~Release()
            {
                if (!n) return;
                AcquireSRWLockExclusive(&g_autoLodLock);
                g_autoLodInUse -= n;
                ReleaseSRWLockExclusive(&g_autoLodLock);
                WakeAllConditionVariable(&g_autoLodFree);
            }
        } release{ big ? AutoLodBytes(faces, bones) : 0 };
        struct Simplifier
        {
            explicit Simplifier(int stride) { meshlod::t_stride = stride; }
            ~Simplifier() { meshlod::t_stride = 0; meshlod::t_attributes = {}; }
        } simplifier(g_autoLodMeshopt && bones >= 0 ? bones + 11 : 0);
        DumpAutoLodModel(model, faces, "in");
        g_autoLod(model, reader, spec);
        DumpAutoLodModel(model, faces, "out");
        if (g_autoLodRepair)
            if (const int fixed = RepairAutoLodNormals(model))
            {
                InterlockedAdd(&g_autoLodNormalsFixed, fixed);
                fprintf(stdout, "[BetterBuild] Auto LOD to %d faces: rebuilt %d invalid vertex normals (stock fails this LOD with \"Produced invalid normal\").\n", *reinterpret_cast<int*>(model + 76), fixed);
                fflush(stdout);
            }
    }

    bool WriteCode(unsigned char* at, const unsigned char* bytes, size_t n)
    {
        DWORD old = 0;
        if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
        memcpy(at, bytes, n);
        VirtualProtect(at, n, old, &old);
        FlushInstructionCache(GetCurrentProcess(), at, n);
        return true;
    }

    // stock's checks are turned off only once the hook that replaces them is live
    bool InstallAutoLod(unsigned char* base, bool raise, bool repair, bool meshopt)
    {
        unsigned char* fn = base + kAutoLodRva;
        unsigned char* cap = base + kAutoLodFaceCapRva;
        unsigned char* normal = base + kAutoLodNormalCheckRva;
        unsigned char* simplify = base + kSimplifyRva;
        if (memcmp(fn, kAutoLodSignature, sizeof kAutoLodSignature) || memcmp(cap, kAutoLodFaceCap, sizeof kAutoLodFaceCap) ||
            memcmp(normal, kAutoLodNormalCheck, sizeof kAutoLodNormalCheck) || (meshopt && memcmp(simplify, kSimplifySignature, sizeof kSimplifySignature)))
            return false;
        MEMORYSTATUSEX mem = { sizeof mem };
        g_autoLodBudget = GlobalMemoryStatusEx(&mem) ? mem.ullTotalPhys / 5 * 2 : 4ull << 30;
        char env[32] = {};
        if (GetEnvironmentVariableA("BB_AUTOLOD_BUDGET_MB", env, sizeof env) && atoi(env) > 0) g_autoLodBudget = (unsigned long long)atoi(env) << 20;
        g_autoLodRaise = raise;
        g_autoLodRepair = repair;
        g_autoLodMeshopt = meshopt;
        if (meshopt && (!InstallMeshCacheName(base) || MH_CreateHook(simplify, GetEnvironmentVariableA("BB_MESHOPT_STOCK_FACES", nullptr, 0) ? reinterpret_cast<LPVOID>(&SimplifyKeepStock) : reinterpret_cast<LPVOID>(&meshlod::HookSimplify), reinterpret_cast<LPVOID*>(&meshlod::g_simplify)) != MH_OK || MH_EnableHook(simplify) != MH_OK)) return false;
        if (MH_CreateHook(fn, reinterpret_cast<LPVOID>(&HookAutoLod), reinterpret_cast<LPVOID*>(&g_autoLod)) != MH_OK || MH_EnableHook(fn) != MH_OK) return false;
        if (raise && !WriteCode(cap, kAutoLodFaceCapOff, sizeof kAutoLodFaceCapOff)) return false;
        if (repair && !WriteCode(normal, kAutoLodNormalCheckOff, sizeof kAutoLodNormalCheckOff)) return false;
        return true;
    }

    // diagnostics: an access violation prints module+offset, address, thread and stack to stderr
    LONG WINAPI OnException(EXCEPTION_POINTERS* e)
    {
        static LONG depth = 0;
        if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || InterlockedIncrement(&depth) > 1) return EXCEPTION_CONTINUE_SEARCH;
        const auto where = [](const void* a, char* out, size_t n) {
            HMODULE m = nullptr;
            wchar_t path[MAX_PATH] = {};
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(a), &m) && GetModuleFileNameW(m, path, MAX_PATH))
            {
                const wchar_t* base = wcsrchr(path, L'\\');
                snprintf(out, n, "%ls+0x%llx", base ? base + 1 : path, (unsigned long long)(reinterpret_cast<const unsigned char*>(a) - reinterpret_cast<const unsigned char*>(m)));
            }
            else snprintf(out, n, "0x%p", a);
        };
        char at[300];
        where(e->ExceptionRecord->ExceptionAddress, at, sizeof at);
        fprintf(stderr, "[BetterBuild] access violation at %s (%s address 0x%llx) on thread %lu\n", at,
                e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading", (unsigned long long)e->ExceptionRecord->ExceptionInformation[1], GetCurrentThreadId());
        void* frames[24];
        const USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
        for (USHORT i = 0; i < n; ++i) { where(frames[i], at, sizeof at); fprintf(stderr, "    %s\n", at); }
        fflush(stderr);
        InterlockedDecrement(&depth);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    using GetMainArgs = int (__cdecl*)(int*, char***, char***, int, void*);
    GetMainArgs g_getmainargs = nullptr;

    // stdout in the launcher: which optimizations run, which the ini turned off, which could not be applied
    void Announce()
    {
        const size_t count = sizeof kNames / sizeof kNames[0];
        unsigned active = 0;
        for (const Named& n : kNames) active += (g_patches & n.bit) && !(g_failed & n.bit);
        fprintf(stdout, "[BetterBuild] %u of %zu linker optimizations active%s:", active, count, (g_patches & kVerify) ? " (verify mode)" : "");
        for (size_t i = 0, k = 0; i < count; ++i)
            if ((g_patches & kNames[i].bit) && !(g_failed & kNames[i].bit)) fprintf(stdout, "%s %s", k++ ? "," : "", kNames[i].name);
        fprintf(stdout, "%s.\n", active ? "" : " none");
        const auto list = [&](const char* title, bool (*pick)(unsigned)) {
            bool any = false;
            for (const Named& n : kNames)
                if (pick(n.bit)) { fprintf(stdout, "%s %s", any ? "," : title, n.name); any = true; }
            if (any) fprintf(stdout, ".\n");
        };
        list("[BetterBuild] Off in LauncherPlus.ini:", [](unsigned bit) { return !(g_patches & bit); });
        list("[BetterBuild] Linker code did not match, running stock for:", [](unsigned bit) { return (g_failed & bit) != 0; });
        fflush(stdout);
    }

    // runs once on the main thread after the unpacker, a patch whose bytes do not match is skipped and reported
    void InstallCodePatches()
    {
        const auto base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
        for (const CodePatch& p : kCodePatches)
        {
            if (!(g_patches & p.bit)) continue;
            unsigned char* at = base + p.rva;
            DWORD old = 0;
            const bool ok = !memcmp(at, p.expect, p.size) && VirtualProtect(at, p.size, PAGE_EXECUTE_READWRITE, &old);
            if (ok)
            {
                memcpy(at, p.write, p.size);
                VirtualProtect(at, p.size, old, &old);
                FlushInstructionCache(GetCurrentProcess(), at, p.size);
            }
            if (!ok) g_failed |= p.bit;
        }
        if ((g_patches & kParallelMip) && !InstallMip(base)) g_failed |= kParallelMip;
        if ((g_patches & kFastRecenter) && !InstallRecenter(base)) g_failed |= kFastRecenter;
        constexpr unsigned kAutoLodBits = kRaiseAutoLod | kRepairNormals | kMeshoptLod;
        if ((g_patches & kAutoLodBits) && !InstallAutoLod(base, (g_patches & kRaiseAutoLod) != 0, (g_patches & kRepairNormals) != 0, (g_patches & kMeshoptLod) != 0))
            g_failed |= g_patches & kAutoLodBits;
        Announce();
    }

    int __cdecl HookGetMainArgs(int* argc, char*** argv, char*** env, int wildcard, void* startInfo)
    {
        static LONG once = 0;
        if (!InterlockedExchange(&once, 1)) InstallCodePatches();
        return g_getmainargs(argc, argv, env, wildcard, startInfo);
    }
}

// returns 1 only when every requested patch installed, the wrapper aborts the link otherwise
extern "C" __declspec(dllexport) DWORD WINAPI BuildPlusLinkerStart(void* arg)
{
    static LONG started = 0;
    if (InterlockedExchange(&started, 1)) return 0;
    g_patches = (unsigned)reinterpret_cast<uintptr_t>(arg);
    AddVectoredExceptionHandler(1, &OnException);
    if (MH_Initialize() != MH_OK) return 0;
    if ((g_patches & kSkipTelemetry) &&
        MH_CreateHookApi(L"ws2_32", "sendto", reinterpret_cast<LPVOID>(&HookSendTo), reinterpret_cast<LPVOID*>(&g_sendto)) != MH_OK)
        return 0;
    if ((g_patches & kReuseWic) &&
        (MH_CreateHookApi(L"ole32", "CoInitialize", reinterpret_cast<LPVOID>(&HookCoInitialize), reinterpret_cast<LPVOID*>(&g_coInitialize)) != MH_OK ||
         MH_CreateHookApi(L"combase", "CoCreateInstance", reinterpret_cast<LPVOID>(&HookCoCreateInstance), reinterpret_cast<LPVOID*>(&g_coCreateInstance)) != MH_OK))
        return 0;
    // the linker loads d3d11.dll on demand: load it now so the hook can be placed
    if ((g_patches & kReuseD3DDevice) &&
        (!LoadLibraryW(L"d3d11.dll") ||
         MH_CreateHookApi(L"d3d11", "D3D11CreateDevice", reinterpret_cast<LPVOID>(&HookCreateDevice), reinterpret_cast<LPVOID*>(&g_createDevice)) != MH_OK))
        return 0;
    if (g_patches & kPrefetchFiles)
    {
        g_readSeen = new std::unordered_set<std::wstring>();
        g_readOrder = new std::vector<std::wstring>();
        g_written = new std::unordered_set<std::wstring>();
        wchar_t temp[MAX_PATH + 1];
        const DWORD n = GetTempPathW(MAX_PATH + 1, temp);
        if (n && n <= MAX_PATH) g_tempDir = Lower(temp);
        while (!g_tempDir.empty() && (g_tempDir.back() == L'\\' || g_tempDir.back() == L'/')) g_tempDir.pop_back();
        if (MH_CreateHookApi(L"kernelbase", "CreateFileW", reinterpret_cast<LPVOID>(&HookCreateFileW), reinterpret_cast<LPVOID*>(&g_createFileW)) != MH_OK ||
            MH_CreateHookApi(L"kernelbase", "CreateFileA", reinterpret_cast<LPVOID>(&HookCreateFileA), reinterpret_cast<LPVOID*>(&g_createFileA)) != MH_OK ||
            MH_CreateHookApi(L"msvcr110", "exit", reinterpret_cast<LPVOID>(&HookExit), reinterpret_cast<LPVOID*>(&g_exit)) != MH_OK)
            return 0;
    }
    if ((g_patches & kFastStat) &&
        (MH_CreateHookApi(L"msvcr110", "_stat64i32", reinterpret_cast<LPVOID>(&HookStat), reinterpret_cast<LPVOID*>(&g_stat)) != MH_OK ||
         MH_CreateHookApi(L"kernelbase", "FindFirstFileExW", reinterpret_cast<LPVOID>(&HookFindFirstFileExW), reinterpret_cast<LPVOID*>(&g_findFirstFileExW)) != MH_OK ||
         MH_CreateHookApi(L"kernelbase", "FindClose", reinterpret_cast<LPVOID>(&HookFindClose), reinterpret_cast<LPVOID*>(&g_findClose)) != MH_OK))
        return 0;
    // the linker polls workers with Sleep(1)/Sleep(2) but never sets a timer resolution (up to 15.6 ms per poll): ask for 1 ms
    if (g_patches & kUmbraPoll)
        if (HMODULE winmm = LoadLibraryW(L"winmm.dll"))
            if (auto timeBeginPeriod = reinterpret_cast<UINT (WINAPI*)(UINT)>(GetProcAddress(winmm, "timeBeginPeriod"))) timeBeginPeriod(1);
    if (g_patches & (kSkipLocalized | kSkipSound))
    {
        HMODULE self = nullptr;
        wchar_t path[MAX_PATH * 2] = {};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&BuildPlusLinkerStart), &self);
        const DWORD n = GetModuleFileNameW(self, path, MAX_PATH * 2);
        if (!n || n >= MAX_PATH * 2) return 0;
        std::wstring dir(path);
        const std::wstring stateDir = dir.substr(0, dir.find_last_of(L"\\/")) + L"\\linker_state";
        if ((g_patches & kSkipLocalized) && !localized::Install(stateDir)) return 0;
        if ((g_patches & kSkipSound) && !soundskip::Install(stateDir)) return 0;
    }
    if (MH_CreateHookApi(L"msvcr110", "__getmainargs", reinterpret_cast<LPVOID>(&HookGetMainArgs), reinterpret_cast<LPVOID*>(&g_getmainargs)) != MH_OK)
        return 0;
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) return 0;
    if (g_patches & kPrefetchFiles) StartPrefetch();
    if (g_patches & kSkipSound) soundskip::StartPrefetch();   // the sound check's walks, ready when snd_convert is asked for
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_DETACH && (g_patches & kVerify))
    {
        if (g_mipLevels)
            fprintf(stderr, "[BetterBuild] mip verify: %ld levels, %ld parallel, %ld mismatches\n", g_mipLevels, g_mipParallel, g_mipMismatches);
        if (g_wicFast || g_wicStock)
            fprintf(stderr, "[BetterBuild] WIC verify: %ld direct, %ld stock search, %ld decoder mismatches\n", g_wicFast, g_wicStock, g_wicMismatches);
        if (g_recenterCalls)
            fprintf(stderr, "[BetterBuild] recenter verify: %ld surfaces, %ld fast, %ld mismatches; fast %.0f ms, stock %.0f ms\n", g_recenterCalls, g_recenterFast, g_recenterMismatches, Ms(g_recenterFastTicks), Ms(g_recenterStockTicks));
        if (g_statCalls)
        {
            LARGE_INTEGER f; QueryPerformanceFrequency(&f);
            fprintf(stderr, "[BetterBuild] stat verify: %ld calls, %ld one-syscall, %ld mismatches, %ld access-time-only differences; fast %.0f ms, stock %.0f ms\n",
                    g_statCalls, g_statFast, g_statMismatches, g_statAtimeOnly, 1000.0 * (double)g_statFastTicks / (double)f.QuadPart, 1000.0 * (double)g_statStockTicks / (double)f.QuadPart);
        }
        fflush(stderr);
    }
    return TRUE;
}
