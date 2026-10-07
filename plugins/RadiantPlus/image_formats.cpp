// Extra image source formats for Radiant (JPEG), as APEPlus's Feature 6. The converter (0x141058860) accepts only .tif/.tiff/.png/.exr by extension,
// so the std::string ends-with helper is detoured: the converter's ".png" test (the call returning to kPngCheckRet) answers yes for an extra extension.
// [images] extra_formats, default jpg,jpeg.
#include "modtools_plugin.hpp"
#include "image_formats.h"
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace br::formats
{
namespace
{
    // IDA addresses (no ASLR), resolved through host->RadiantAddress: a different build gets nothing
    const uintptr_t kEndsWith = 0x140F93C40;      // bool ends_with(const std::string* s, const std::string* suffix), thunk 0x14008908B
    const uintptr_t kPngCheckRet = 0x14105940C;   // return address of the converter's ".png" ends-with call (E8 at 0x141059407)

    typedef bool (__fastcall* EndsWithFn)(const void*, const void*);
    EndsWithFn g_origEndsWith = nullptr;
    std::vector<std::string> g_exts;              // lowercase, no dot
    uintptr_t g_pngRet = 0;
    bool g_converter = false;
    volatile LONG g_accepted = 0;

    void Configure(const std::string& list)
    {
        g_exts.clear();
        std::string cur;
        auto flush = [&]() {
            size_t a = cur.find_first_not_of(" \t."), b = cur.find_last_not_of(" \t");
            std::string e = a == std::string::npos ? std::string() : cur.substr(a, b - a + 1);
            for (char& ch : e) ch = char(tolower(static_cast<unsigned char>(ch)));
            if (!e.empty() && e != "tif" && e != "tiff" && e != "png" && e != "exr" && std::find(g_exts.begin(), g_exts.end(), e) == g_exts.end()) g_exts.push_back(e);
            cur.clear();
        };
        for (char ch : list) { if (ch == ',' || ch == ';') flush(); else cur += ch; }
        flush();
    }

    // MSVC 2012 std::string: 16-byte buffer at +0 (a pointer there when the capacity is 16 or more), size +16, capacity +24
    const char* StdStringData(const void* s, size_t& n)
    {
        const char* p = static_cast<const char*>(s);
        n = *reinterpret_cast<const size_t*>(p + 16);
        return *reinterpret_cast<const size_t*>(p + 24) >= 16 ? *reinterpret_cast<const char* const*>(p) : p;
    }
    bool HasExtraExt(const char* s, size_t n)   // ".<ext>" at the end, any case
    {
        for (const std::string& e : g_exts)
            if (n > e.size() && s[n - e.size() - 1] == '.' && _strnicmp(s + n - e.size(), e.c_str(), e.size()) == 0) return true;
        return false;
    }
    bool __fastcall hk_EndsWith(const void* s, const void* suffix)
    {
        bool r = g_origEndsWith(s, suffix);
        if (r || reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_pngRet) return r;
        size_t n = 0; const char* p = StdStringData(s, n);
        if (!p || !HasExtraExt(p, n)) return false;
        if (InterlockedIncrement(&g_accepted) <= 20) rp::Log("formats: '%.*s' takes the PNG (WIC) path", int(n), p);
        return true;
    }

    std::string List()
    {
        std::string list; for (const std::string& e : g_exts) list += (list.empty() ? "" : ",") + e;
        return list;
    }
}

void Setup()
{
    Configure(rp::ConfigGet("images", "extra_formats", "jpg,jpeg"));
    if (g_exts.empty()) { rp::Log("formats: no extra image formats configured ([images] extra_formats)"); return; }
    // the helper: sub rsp,28h, mov r9,[rcx+10h], mov r10,[rdx+10h] (the same code as APE's)
    uint8_t* target = static_cast<uint8_t*>(rp::host->RadiantAddress(rp::self, kEndsWith));
    const uint8_t* call = static_cast<const uint8_t*>(rp::host->RadiantAddress(rp::self, kPngCheckRet - 5));
    static const uint8_t kHead[12] = { 0x48, 0x83, 0xEC, 0x28, 0x4C, 0x8B, 0x49, 0x10, 0x4C, 0x8B, 0x52, 0x10 };
    DWORD64 base = 0; PRUNTIME_FUNCTION fn = target ? RtlLookupFunctionEntry(DWORD64(target), &base, nullptr) : nullptr;
    if (!target || !call || memcmp(target, kHead, sizeof kHead) != 0 || call[0] != 0xE8 || !fn || DWORD64(fn->BeginAddress) + base != DWORD64(target))
        rp::Log("formats: converter helper not recognised, the converter keeps Radiant's list");
    else if (!rp::host->Detour(rp::self, target, reinterpret_cast<void*>(&hk_EndsWith), reinterpret_cast<void**>(&g_origEndsWith)))
        rp::Log("formats: converter detour failed");
    else { g_pngRet = uintptr_t(call) + 5; g_converter = true; }
    rp::Log("formats: extra image formats %s (converter %s)", List().c_str(), g_converter ? "on" : "off");
}

bool Command(const std::string& command, std::string& reply)
{
    if (command != "formats" && command != "formats status") return false;
    char buf[256];
    snprintf(buf, sizeof buf, "formats: extra=%s converter=%d (accepted %ld)", g_exts.empty() ? "(none)" : List().c_str(), g_converter ? 1 : 0, g_accepted);
    reply = buf;
    return true;
}
}
