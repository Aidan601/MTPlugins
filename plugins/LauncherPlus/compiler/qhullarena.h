#pragma once
// C12: qhull memory arena (one hull per collision partition). qh_memalloc, qh_memfree and qh_memfreeshort
// are exact copies whose malloc/free are a bump arena reset per hull. The tables stay on the CRT heap.
// Past 1 GB in one hull it falls back to CRT blocks. Tracing prints (qhull 'T5', never set) are dropped.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../../../src/radiant_addrs.h"
#include "../../../src/third_party/minhook/include/MinHook.h"

namespace qhullarena
{
    // qhmemT as laid out in this build (0x88 bytes at QhMemRva).
    struct QhMem
    {
        int BUFsize, BUFinit, TABLEsize, NUMsizes, LASTsize, ALIGNmask;   // +0x00
        void** freelists;    // +0x18
        int* sizetable;      // +0x20
        int* indextable;     // +0x28
        void* curbuffer;     // +0x30
        char* freemem;       // +0x38
        int freesize;        // +0x40
        int pad44;
        void* tempstack;     // +0x48
        void* ferr;          // +0x50
        int IStracing;       // +0x58
        int cntquick;        // +0x5C
        int cntshort;        // +0x60
        int cntlong;         // +0x64
        int freeshort;       // +0x68 (incremented on the long path in this build)
        int freeshort2;      // +0x6C (incremented by the short free)
        int freelong;        // +0x70
        int totbuffer;       // +0x74
        int totlong;         // +0x78
        int maxlong;         // +0x7C
        int totshort, totunused;  // +0x80
    };
    static_assert(sizeof(QhMem) == 0x88, "qhmem layout");

    using MemAllocFn = void* (*)(int size);
    using MemFreeFn = void (*)(void* block, int size);
    using MemFreeShortFn = void (*)(int* curlong, int* totlong);
    using CrtFreeFn = void (__cdecl*)(void*);
    inline MemAllocFn g_memalloc = nullptr;          // trampolines
    inline MemFreeFn g_memfree = nullptr;
    inline MemFreeShortFn g_memfreeshort = nullptr;
    inline CrtFreeFn g_crtFree = nullptr;            // msvcr110!free, for the tables and fallback blocks
    inline void* (__cdecl* g_crtMalloc)(size_t) = nullptr;
    inline QhMem* g_qh = nullptr;
    inline bool g_verify = false;
    inline unsigned long long g_hulls = 0, g_chunks = 0, g_longs = 0, g_fallbacks = 0, g_anomalies = 0;

    struct Chunk { uint8_t* base; size_t size, used; };
    inline std::vector<Chunk> g_chunksList;
    inline size_t g_current = 0, g_total = 0;
    constexpr size_t kFirstChunk = 8u << 20, kMaxChunk = 128u << 20, kRetain = 256u << 20, kCap = 1u << 30;

    inline bool InArena(const void* p)
    {
        for (const Chunk& c : g_chunksList)
            if (p >= c.base && p < c.base + c.size) return true;
        return false;
    }

    inline void* ArenaAlloc(size_t size)
    {
        size = (size + 15) & ~size_t(15);
        if (g_total + size > kCap) return nullptr;
        while (g_current < g_chunksList.size() && g_chunksList[g_current].used + size > g_chunksList[g_current].size) ++g_current;
        if (g_current == g_chunksList.size())
        {
            size_t chunk = g_chunksList.empty() ? kFirstChunk : g_chunksList.back().size * 2;
            if (chunk > kMaxChunk) chunk = kMaxChunk;
            if (chunk < size) chunk = (size + 0xFFFF) & ~size_t(0xFFFF);
            void* mem = VirtualAlloc(nullptr, chunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!mem) return nullptr;
            g_chunksList.push_back({ static_cast<uint8_t*>(mem), chunk, 0 });
        }
        Chunk& c = g_chunksList[g_current];
        void* p = c.base + c.used;
        c.used += size;
        g_total += size;
        return p;
    }

    inline void ArenaReset()
    {
        size_t kept = 0;
        for (size_t i = 0; i < g_chunksList.size(); ++i)
        {
            g_chunksList[i].used = 0;
            kept += g_chunksList[i].size;
            if (kept > kRetain) { VirtualFree(g_chunksList[i].base, 0, MEM_RELEASE); g_chunksList.erase(g_chunksList.begin() + i); --i; kept -= 0; }
        }
        g_current = 0;
        g_total = 0;
    }

    inline void* Alloc(size_t size)
    {
        void* p = ArenaAlloc(size);
        if (p) return p;
        ++g_fallbacks;
        return g_crtMalloc(size);
    }

    // qh_memalloc: freelist hit, else carve from the current buffer (new one when full), else a long block
    inline void* MemAlloc(int size)
    {
        QhMem& qh = *g_qh;
        if (size > qh.LASTsize)
        {
            if (!qh.indextable) return g_memalloc(size);   // stock prints "qhmem has not been initialized" and exits
            ++qh.cntlong;
            ++qh.freeshort;
            qh.totlong += size;
            if (qh.maxlong < qh.totlong) qh.maxlong = qh.totlong;
            void* p = Alloc(size_t(size));
            if (!p) return g_memalloc(size);              // stock reports the failure
            ++g_longs;
            return p;
        }
        const int index = qh.indextable[size];
        void** head = &qh.freelists[index];
        if (*head)
        {
            ++qh.cntquick;
            void* p = *head;
            *head = *static_cast<void**>(p);
            return p;
        }
        const int rounded = qh.sizetable[index];
        ++qh.cntshort;
        char* mem;
        int freesize;
        if (rounded <= qh.freesize)
        {
            mem = qh.freemem;
            freesize = qh.freesize;
        }
        else
        {
            int bufsize = qh.BUFsize;
            if (!qh.curbuffer) bufsize = qh.BUFinit;
            qh.totbuffer += bufsize;
            void** buffer = static_cast<void**>(Alloc(size_t(bufsize)));
            if (!buffer) return g_memalloc(size);
            ++g_chunks;
            *buffer = qh.curbuffer;
            qh.curbuffer = buffer;
            const int header = ~qh.ALIGNmask & (qh.ALIGNmask + 8);
            mem = reinterpret_cast<char*>(buffer) + header;
            freesize = bufsize - header;
        }
        qh.freesize = freesize - rounded;
        qh.freemem = mem + rounded;
        return mem;
    }

    // qh_memfree: short blocks back on their freelist, long blocks counted (arena) or freed (fallback)
    inline void MemFree(void* block, int size)
    {
        if (!block) return;
        QhMem& qh = *g_qh;
        if (size > qh.LASTsize)
        {
            ++qh.freelong;
            qh.totlong -= size;
            if (!InArena(block)) { if (g_verify && g_chunksList.empty()) ++g_anomalies; g_crtFree(block); }
            return;
        }
        ++qh.freeshort2;
        void** head = &qh.freelists[qh.indextable[size]];
        *static_cast<void**>(block) = *head;
        *head = block;
    }

    // qh_memfreeshort: report counts, drop every chunk (arena reset, fallback chunks freed), free the tables, clear qhmem
    inline void MemFreeShort(int* curlong, int* totlong)
    {
        QhMem& qh = *g_qh;
        ++g_hulls;
        *curlong = qh.cntlong - qh.freelong;
        *totlong = qh.totlong;
        for (void** buffer = static_cast<void**>(qh.curbuffer); buffer;)
        {
            void** next = static_cast<void**>(*buffer);
            if (!InArena(buffer)) g_crtFree(buffer);
            buffer = next;
        }
        qh.curbuffer = nullptr;
        if (qh.LASTsize)
        {
            g_crtFree(qh.indextable);
            g_crtFree(qh.freelists);
            g_crtFree(qh.sizetable);
        }
        memset(&qh, 0, sizeof qh);
        ArenaReset();
    }

    inline bool Verify(uintptr_t base)
    {
        using namespace cod2mapaddr;
        auto match = [base](uintptr_t rva, const void* sig, size_t size) { return !memcmp(reinterpret_cast<void*>(base + rva), sig, size); };
        HMODULE crt = GetModuleHandleW(L"MSVCR110.dll");
        return match(QhMemAllocRva, QhMemAllocSig, sizeof QhMemAllocSig) && match(QhMemFreeRva, QhMemFreeSig, sizeof QhMemFreeSig) &&
               match(QhMemFreeShortRva, QhMemFreeShortSig, sizeof QhMemFreeShortSig) &&
               crt && GetProcAddress(crt, "free") && GetProcAddress(crt, "malloc");
    }

    inline bool Install(uintptr_t base, bool verify)
    {
        using namespace cod2mapaddr;
        g_verify = verify;
        g_qh = reinterpret_cast<QhMem*>(base + QhMemRva);
        HMODULE crt = GetModuleHandleW(L"MSVCR110.dll");
        g_crtFree = reinterpret_cast<CrtFreeFn>(GetProcAddress(crt, "free"));
        g_crtMalloc = reinterpret_cast<void* (__cdecl*)(size_t)>(GetProcAddress(crt, "malloc"));
        auto hook = [base](uintptr_t rva, void* detour, void* original) {
            return MH_CreateHook(reinterpret_cast<void*>(base + rva), detour, reinterpret_cast<void**>(original)) == MH_OK;
        };
        return hook(QhMemAllocRva, reinterpret_cast<void*>(&MemAlloc), &g_memalloc) &&
               hook(QhMemFreeRva, reinterpret_cast<void*>(&MemFree), &g_memfree) &&
               hook(QhMemFreeShortRva, reinterpret_cast<void*>(&MemFreeShort), &g_memfreeshort);
    }

    inline void Report(FILE* out)
    {
        fprintf(out, "[BetterBuild] Qhull Arena: %llu hulls, %llu buffer chunks, %llu long blocks, %llu CRT fallbacks, %zu chunks retained",
                g_hulls, g_chunks, g_longs, g_fallbacks, g_chunksList.size());
        if (g_verify) fprintf(out, ", %llu anomalies", g_anomalies);
        fprintf(out, ".\n");
    }
}
