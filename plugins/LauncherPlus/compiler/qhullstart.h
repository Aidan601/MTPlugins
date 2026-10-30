#pragma once
// C14: qhull per-hull startup. qh_option formats every option into qh.qhull_options with sprintf and strncat
// (nothing reads it in a compile); the text is built with to_chars in MSVCR110's format and the same strncat
// rule writes it. qh_initqhull_globals draws 1000 random numbers per hull
// for a self-test warning and reseeds; the loop and the check are skipped in place (the RNG state is the same
// fresh seed).
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <charconv>
#include <cmath>
#include <vector>
#include "../../../src/radiant_addrs.h"
#include "../../../src/third_party/minhook/include/MinHook.h"

namespace qhullstart
{
    using OptionFn = char* (*)(const char* option, int* i, double* r);
    using SprintfFn = int (__cdecl*)(char* buffer, const char* format, ...);

    inline OptionFn g_option = nullptr;   // trampoline
    inline SprintfFn g_sprintf = nullptr; // MSVCR110's sprintf, the one stock uses ("%2.2g" prints 3-digit exponents)
    inline char* g_options = nullptr;     // qh.qhull_options[512]
    inline int* g_optionLen = nullptr;    // qh.qhull_optionlen
    inline bool g_verify = false;
    inline bool g_rngPatched = false;
    inline unsigned long long g_calls = 0, g_formats = 0, g_mismatches = 0;  // g_formats: writes the CRT had to format

    inline void Format(char* buffer, const char* option, int* i, double* r)
    {
        g_sprintf(buffer, "  %s", option);
        if (i) g_sprintf(buffer + strlen(buffer), " %d", *i);
        if (r) g_sprintf(buffer + strlen(buffer), " %2.2g", *r);
    }

    // "  %s[ %d][ %2.2g]" without the CRT: to_chars is correctly rounded like MSVCR110 except on exact decimal ties
    // (old CRT rounds them away from zero), and prints 2 exponent digits where MSVCR110 prints 3. Ties and
    // non-finite values return false and go to the real sprintf.
    inline bool FormatFast(char* out, const char* option, int* i, double* r)
    {
        char* const end = out + 200;
        char* p = out;
        *p++ = ' '; *p++ = ' ';
        const size_t n = strlen(option);
        if (n > 150) return false;
        memcpy(p, option, n); p += n;
        if (i)
        {
            *p++ = ' ';
            p = std::to_chars(p, end, *i).ptr;
        }
        if (r)
        {
            const double v = *r;
            if (!std::isfinite(v)) return false;
            char sci[64];
            const char* q = sci;
            const auto t = std::to_chars(sci, sci + sizeof sci, v, std::chars_format::scientific, 20);
            if (*q == '-') ++q;
            if (q[1] == '.' && q[3] == '5')
            {
                bool tie = true;
                for (int k = 4; k < 22 && q + k < t.ptr; ++k) if (q[k] != '0') { tie = false; break; }
                if (tie) return false;
            }
            *p++ = ' ';
            char* num = p;
            auto g = std::to_chars(p, end, v, std::chars_format::general, 2);
            if (char* e = static_cast<char*>(memchr(num, 'e', size_t(g.ptr - num))))
            {
                char* d = e + 2;  // past 'e' and the sign
                if (g.ptr - d == 2) { memmove(d + 1, d, 2); *d = '0'; ++g.ptr; }
            }
            if (g.ptr - num < 2) { memmove(num + 1, num, size_t(g.ptr - num)); *num = ' '; ++g.ptr; }  // width 2
            p = g.ptr;
        }
        *p = 0;
        return true;
    }

    inline const char* Text(const char* option, int* i, double* r, char* scratch)
    {
        if (FormatFast(scratch, option, i, r)) return scratch;
        ++g_formats;
        Format(scratch, option, i, r);
        return scratch;
    }

    inline char* Option(const char* option, int* i, double* r)
    {
        ++g_calls;
        char scratch[208];
        const char* text = Text(option, i, r, scratch);
        if (g_verify)
        {
            char buffer[208];
            Format(buffer, option, i, r);
            if (strcmp(text, buffer)) ++g_mismatches;
        }
        const int len = int(strlen(text));
        const int total = len + *g_optionLen;
        int maxlen = 511 - len;
        if (maxlen < 0) maxlen = 0;
        *g_optionLen += len;
        if (total >= 80 && maxlen > 0)
        {
            *g_optionLen = len;
            strncat(g_options, "\n", size_t(maxlen--));
        }
        return strncat(g_options, text, size_t(maxlen));
    }

    // Replaces "mov edi,1000 / mov r15d,1 / movsd xmm6,[qh_RANDOMmax]" with the two register effects the code after
    // the loop needs (xmm6 = qh_RANDOMmax, r15d = r12d) and a jump over the loop, the reseed and the average check.
    inline bool PatchRngTest(uintptr_t base)
    {
        using namespace cod2mapaddr;
        uint8_t* at = reinterpret_cast<uint8_t*>(base + QhRngTestRva);
        DWORD old = 0;
        if (!VirtualProtect(at, 19, PAGE_EXECUTE_READWRITE, &old)) return false;
        uint8_t code[19] = { 0xF2, 0x0F, 0x10, 0x35, 0, 0, 0, 0,   // movsd xmm6, [rip + disp]
                             0x45, 0x8B, 0xFC,                     // mov r15d, r12d
                             0xE9, 0, 0, 0, 0,                     // jmp QhRngTestEnd
                             0xCC, 0xCC, 0xCC };
        const int32_t disp = int32_t(QhRandomMaxRva - (QhRngTestRva + 8));
        const int32_t rel = int32_t(QhRngTestEndRva - (QhRngTestRva + 16));
        memcpy(code + 4, &disp, 4);
        memcpy(code + 12, &rel, 4);
        memcpy(at, code, sizeof code);
        VirtualProtect(at, 19, old, &old);
        FlushInstructionCache(GetCurrentProcess(), at, 19);
        return true;
    }

    inline bool Verify(uintptr_t base)
    {
        using namespace cod2mapaddr;
        return !memcmp(reinterpret_cast<void*>(base + QhOptionRva), QhOptionSig, sizeof QhOptionSig) &&
               !memcmp(reinterpret_cast<void*>(base + QhRngRegionRva), QhRngRegionSig, sizeof QhRngRegionSig);
    }

    inline bool Install(uintptr_t base, bool verify)
    {
        using namespace cod2mapaddr;
        g_verify = verify;
        g_sprintf = reinterpret_cast<SprintfFn>(GetProcAddress(GetModuleHandleW(L"MSVCR110.dll"), "sprintf"));
        if (!g_sprintf) return false;
        g_options = reinterpret_cast<char*>(base + QhOptionsRva);
        g_optionLen = reinterpret_cast<int*>(base + QhOptionLenRva);
        g_rngPatched = PatchRngTest(base);
        return g_rngPatched &&
               MH_CreateHook(reinterpret_cast<void*>(base + QhOptionRva), reinterpret_cast<void*>(&Option), reinterpret_cast<void**>(&g_option)) == MH_OK;
    }

    inline void Report(FILE* out)
    {
        fprintf(out, "[BetterBuild] Qhull Startup: %llu option writes, %llu through the CRT, RNG self-test %s", g_calls, g_formats, g_rngPatched ? "skipped" : "stock");
        if (g_verify) fprintf(out, ", %llu verify mismatches", g_mismatches);
        fprintf(out, ".\n");
    }
}
