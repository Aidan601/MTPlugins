#include <windows.h>
#include "radiant_addrs.h"
#include "app.h"
#include "log.h"

namespace rt
{
    uintptr_t Rva(uintptr_t idaAddress)
    {
        static uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        return base + (idaAddress - kImageBase);
    }

    bool VerifyBinary()
    {
        static int cached = -1;
        if (cached >= 0) return cached != 0;
        if (!app::IsRadiant()) { LOG("rt: running inside %s, Radiant addresses are unavailable", app::Name()); cached = 0; return false; }
        auto base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        uint32_t ts = nt->FileHeader.TimeDateStamp;
        uint32_t size = nt->OptionalHeader.SizeOfImage;
        bool ok = ts == kExpectedTimestamp && size == kExpectedSizeOfImage;
        LOG("rt: exe base=0x%p timestamp=0x%08X sizeOfImage=0x%X -> %s", base, ts, size, ok ? "MATCH" : "MISMATCH");
        if (!ok) LOG("rt: expected timestamp 0x%08X sizeOfImage 0x%X", kExpectedTimestamp, kExpectedSizeOfImage);
        cached = ok ? 1 : 0;
        return ok;
    }
}
