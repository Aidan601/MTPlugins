#pragma once
// C16: small-block allocator in front of MSVCR110's heap. Blocks up to 2 KB come from size-class pages (64 KB, one
// class per page, header-less, 16-byte aligned) with a free list per class; the requested size is kept per block so
// _msize, realloc, _recalloc and _expand keep the CRT's exact semantics. Larger requests and pointers that are not ours
// (allocated before the hooks, or by the CRT itself) go to the original functions. One spinlock, the compiler's hot
// phases are single-threaded.
#include <windows.h>
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../../../src/third_party/minhook/include/MinHook.h"

namespace fastheap
{
    using MallocFn = void* (__cdecl*)(size_t);
    using FreeFn = void (__cdecl*)(void*);
    using ReallocFn = void* (__cdecl*)(void*, size_t);
    using CallocFn = void* (__cdecl*)(size_t, size_t);
    using MsizeFn = size_t (__cdecl*)(void*);
    using RecallocFn = void* (__cdecl*)(void*, size_t, size_t);
    using ExpandFn = void* (__cdecl*)(void*, size_t);

    inline MallocFn g_malloc = nullptr;   // trampolines
    inline FreeFn g_free = nullptr;
    inline ReallocFn g_realloc = nullptr;
    inline CallocFn g_calloc = nullptr;
    inline MsizeFn g_msize = nullptr;
    inline RecallocFn g_recalloc = nullptr;
    inline ExpandFn g_expand = nullptr;

    constexpr size_t kMaxSmall = 2048, kPage = 65536, kChunk = size_t(64) << 20, kPagesPerChunk = kChunk / kPage;
    constexpr unsigned kClasses = 24;
    constexpr uint16_t kSizes[kClasses] = { 16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512,
                                            640, 768, 896, 1024, 1280, 1536, 1792, 2048 };
    struct Page { uint8_t cls; bool used; uint16_t* sizes; };  // sizes: requested bytes per block
    inline uint32_t g_recip[kClasses] = {};  // block index = (offset in page * recip) >> 32, checked exact at install
    struct Chunk { uint8_t* base; uint32_t nextPage; Page pages[kPagesPerChunk]; };
    struct Class { void* freelist; uint8_t* bump; uint8_t* bumpEnd; Page* bumpPage; uint8_t* bumpPageBase; };

    constexpr size_t kMaxChunks = 1024;
    inline Chunk* g_chunks[kMaxChunks] = {};  // append-only, count published after the entry: lock-free readers
    inline volatile long g_chunkCount = 0;
    inline Class g_class[kClasses] = {};
    inline uint8_t g_classOf[kMaxSmall / 16 + 1] = {};
    inline volatile long g_lock = 0;
    inline unsigned long long g_allocs = 0, g_frees = 0, g_foreignFrees = 0, g_large = 0, g_reallocs = 0, g_pages = 0;
    inline unsigned long long g_msizes = 0, g_recallocs = 0, g_expands = 0;  // on our blocks

    inline void Lock() { while (_InterlockedExchange(&g_lock, 1)) { while (g_lock) _mm_pause(); } }
    inline void Unlock() { _InterlockedExchange(&g_lock, 0); }

    // the chunk holding p, or null (a few dozen chunks at most)
    inline Chunk* ChunkOf(const void* p)
    {
        const uint8_t* q = static_cast<const uint8_t*>(p);
        const long count = g_chunkCount;
        for (long i = 0; i < count; ++i)
            if (q >= g_chunks[i]->base && q < g_chunks[i]->base + kChunk) return g_chunks[i];
        return nullptr;
    }

    inline Chunk* NewChunk()
    {
        if (g_chunkCount == kMaxChunks) return nullptr;
        void* mem = VirtualAlloc(nullptr, kChunk, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!mem) return nullptr;
        auto* c = new Chunk();
        c->base = static_cast<uint8_t*>(mem);
        c->nextPage = 0;
        g_chunks[g_chunkCount] = c;
        _InterlockedIncrement(&g_chunkCount);
        return c;
    }

    // a fresh page for the class, false when the OS refuses memory
    inline bool NewPage(unsigned cls)
    {
        Chunk* c = g_chunkCount ? g_chunks[g_chunkCount - 1] : nullptr;
        if (c && c->nextPage == kPagesPerChunk) c = nullptr;
        if (!c) c = NewChunk();
        if (!c) return false;
        Page& pg = c->pages[c->nextPage];
        pg.cls = uint8_t(cls);
        pg.used = true;
        pg.sizes = new uint16_t[kPage / kSizes[cls]]();
        Class& k = g_class[cls];
        k.bumpPageBase = c->base + size_t(c->nextPage) * kPage;
        k.bump = k.bumpPageBase;
        k.bumpEnd = k.bumpPageBase + (kPage / kSizes[cls]) * kSizes[cls];
        k.bumpPage = &pg;
        ++c->nextPage;
        ++g_pages;
        return true;
    }

    inline void Record(Chunk* c, void* p, size_t requested)
    {
        const size_t offset = size_t(static_cast<uint8_t*>(p) - c->base);
        Page& pg = c->pages[offset / kPage];
        pg.sizes[(uint64_t(offset % kPage) * g_recip[pg.cls]) >> 32] = uint16_t(requested);
    }

    inline void* Alloc(size_t n)
    {
        const unsigned cls = g_classOf[(n + 15) >> 4];
        Lock();
        Class& k = g_class[cls];
        void* p = k.freelist;
        if (p) k.freelist = *static_cast<void**>(p);
        else
        {
            if (k.bump == k.bumpEnd && !NewPage(cls)) { Unlock(); return nullptr; }
            p = k.bump;
            k.bump += kSizes[cls];
        }
        Chunk* c = ChunkOf(p);
        Record(c, p, n);
        ++g_allocs;
        Unlock();
        return p;
    }

    // class size and requested size of one of our blocks (caller holds no lock: the page tables never move)
    inline bool Ours(const void* p, size_t& classSize, size_t& requested, Chunk*& chunk)
    {
        chunk = ChunkOf(p);
        if (!chunk) return false;
        const size_t offset = size_t(static_cast<const uint8_t*>(p) - chunk->base);
        const Page& pg = chunk->pages[offset / kPage];
        if (!pg.used) return false;
        classSize = kSizes[pg.cls];
        requested = pg.sizes[(uint64_t(offset % kPage) * g_recip[pg.cls]) >> 32];
        return true;
    }

    inline void Release(void* p, Chunk* c)
    {
        const size_t offset = size_t(static_cast<uint8_t*>(p) - c->base);
        Lock();
        Class& k = g_class[c->pages[offset / kPage].cls];
        *static_cast<void**>(p) = k.freelist;
        k.freelist = p;
        ++g_frees;
        Unlock();
    }

    inline void* __cdecl Malloc(size_t n)
    {
        if (n > kMaxSmall) { ++g_large; return g_malloc(n); }
        void* p = Alloc(n);
        return p ? p : g_malloc(n);
    }

    inline void __cdecl Free(void* p)
    {
        size_t cs, req; Chunk* c;
        if (p && Ours(p, cs, req, c)) { Release(p, c); return; }
        if (p) ++g_foreignFrees;
        g_free(p);
    }

    inline void* __cdecl Realloc(void* p, size_t n)
    {
        size_t cs, req; Chunk* c;
        if (!p || !Ours(p, cs, req, c)) return g_realloc(p, n);
        ++g_reallocs;
        if (n == 0) { Release(p, c); return nullptr; }
        if (n <= cs) { Lock(); Record(c, p, n); Unlock(); return p; }
        void* q = Malloc(n);
        if (!q) return nullptr;
        memcpy(q, p, req < n ? req : n);
        Release(p, c);
        return q;
    }

    inline void* __cdecl Calloc(size_t count, size_t size)
    {
        if (size && count > SIZE_MAX / size) return g_calloc(count, size);
        const size_t n = count * size;
        if (n > kMaxSmall) { ++g_large; return g_calloc(count, size); }
        void* p = Alloc(n);
        if (!p) return g_calloc(count, size);
        memset(p, 0, n);
        return p;
    }

    inline size_t __cdecl Msize(void* p)
    {
        size_t cs, req; Chunk* c;
        if (p && Ours(p, cs, req, c)) { ++g_msizes; return req; }
        return g_msize(p);
    }

    inline void* __cdecl Recalloc(void* p, size_t count, size_t size)
    {
        size_t cs, req; Chunk* c;
        if (!p || !Ours(p, cs, req, c)) return g_recalloc(p, count, size);
        ++g_recallocs;
        if (size && count > SIZE_MAX / size) return nullptr;
        const size_t n = count * size;
        void* q = Realloc(p, n);
        if (q && n > req) memset(static_cast<uint8_t*>(q) + req, 0, n - req);
        return q;
    }

    inline void* __cdecl Expand(void* p, size_t n)
    {
        size_t cs, req; Chunk* c;
        if (!p || !Ours(p, cs, req, c)) return g_expand(p, n);
        ++g_expands;
        if (n > cs) return nullptr;
        Lock(); Record(c, p, n); Unlock();
        return p;
    }

    inline bool Verify(uintptr_t)
    {
        HMODULE crt = GetModuleHandleW(L"MSVCR110.dll");
        if (!crt) return false;
        for (const char* name : { "malloc", "free", "realloc", "calloc", "_msize", "_recalloc", "_expand" })
            if (!GetProcAddress(crt, name)) return false;
        return true;
    }

    inline bool Install(uintptr_t, bool)
    {
        for (size_t i = 0, cls = 0; i <= kMaxSmall / 16; ++i)
        {
            while (cls + 1 < kClasses && kSizes[cls] < i * 16) ++cls;
            g_classOf[i] = uint8_t(cls);
        }
        for (unsigned c = 0; c < kClasses; ++c)
        {
            g_recip[c] = uint32_t((uint64_t(1) << 32) / kSizes[c] + 1);
            for (uint32_t off = 0; off < kPage; ++off)
                if ((uint64_t(off) * g_recip[c]) >> 32 != off / kSizes[c]) return false;
        }
        HMODULE crt = GetModuleHandleW(L"MSVCR110.dll");
        auto hook = [&](const char* name, void* detour, void** original) {
            return MH_CreateHook(reinterpret_cast<void*>(GetProcAddress(crt, name)), detour, original) == MH_OK;
        };
        return hook("malloc", reinterpret_cast<void*>(&Malloc), reinterpret_cast<void**>(&g_malloc)) &&
               hook("free", reinterpret_cast<void*>(&Free), reinterpret_cast<void**>(&g_free)) &&
               hook("realloc", reinterpret_cast<void*>(&Realloc), reinterpret_cast<void**>(&g_realloc)) &&
               hook("calloc", reinterpret_cast<void*>(&Calloc), reinterpret_cast<void**>(&g_calloc)) &&
               hook("_msize", reinterpret_cast<void*>(&Msize), reinterpret_cast<void**>(&g_msize)) &&
               hook("_recalloc", reinterpret_cast<void*>(&Recalloc), reinterpret_cast<void**>(&g_recalloc)) &&
               hook("_expand", reinterpret_cast<void*>(&Expand), reinterpret_cast<void**>(&g_expand));
    }

    inline void Report(FILE* out)
    {
        fprintf(out, "[BetterBuild] Fast Heap: %llu small allocations, %llu frees, %llu reallocs, %llu _msize, %llu _recalloc, %llu _expand, %llu large (CRT), %llu foreign frees, %llu pages in %zu chunks.\n",
                g_allocs, g_frees, g_reallocs, g_msizes, g_recallocs, g_expands, g_large, g_foreignFrees, g_pages, size_t(g_chunkCount));
    }
}
