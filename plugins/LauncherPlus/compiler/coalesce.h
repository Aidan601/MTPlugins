#pragma once
// C1: coalesced surface props lookup. A side map from the leaf pointer sequence to the record replaces
// stock's 256-bucket walk. On a miss the bucket is detached while stock creates the record, then relinked
// behind it (the list stock would build). Unusual paths call stock. Verify mode checks the map against stock.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <unordered_set>
#include "../../../src/radiant_addrs.h"
#include "../../../src/third_party/minhook/include/MinHook.h"

namespace coalesce
{
    using FindOrCreateFn = void* (*)(void* a, void* b, void* ctx);
    using ClearFn = void (*)();
    using FlattenFn = int (*)(void* props, void** out, int start, void* ctx);
    using SkipFn = bool (*)(void* a, void* b, void* ctx);
    using MaterialFlagsFn = unsigned (*)(void* mtlTex);

    inline FindOrCreateFn g_findOrCreate = nullptr;  // trampoline
    inline ClearFn g_clear = nullptr;                // trampoline
    inline FlattenFn g_flatten = nullptr;
    inline SkipFn g_skip = nullptr;
    inline MaterialFlagsFn g_materialFlags = nullptr;
    inline uint8_t** g_buckets = nullptr;   // stock table, 256 record heads
    inline bool g_verify = false;
    inline unsigned long long g_hits = 0, g_misses = 0, g_passthrough = 0, g_mismatches = 0, g_relinkFailures = 0;
    inline std::unordered_set<void*> g_records;  // verify mode: every record the map knows

    // Stock's 8-bit bucket index over the key's bytes.
    inline unsigned StockBucket(const void* key, size_t bytes)
    {
        const unsigned char* p = static_cast<const unsigned char*>(key);
        uint32_t h = 0;
        for (size_t k = 0; k < bytes; ++k) h = p[k] + uint32_t(k + 118) * h;
        return h & 0xFF;
    }

    // Open-addressing map: pointer sequence -> record. The compiler is single-threaded here.
    class ChainMap
    {
        struct Slot { uint64_t hash; uint32_t offset; uint32_t length; void* record; };
        std::vector<Slot> slots_ = std::vector<Slot>(1024);
        std::vector<uintptr_t> keys_;
        size_t used_ = 0;

        bool Equal(const Slot& s, const uintptr_t* key, uint32_t length) const
        {
            return s.length == length && !memcmp(keys_.data() + s.offset, key, length * sizeof(uintptr_t));
        }

        void Grow()
        {
            std::vector<Slot> old(slots_.size() * 2);
            old.swap(slots_);
            const size_t mask = slots_.size() - 1;
            for (const Slot& s : old)
            {
                if (!s.record) continue;
                size_t i = s.hash & mask;
                while (slots_[i].record) i = (i + 1) & mask;
                slots_[i] = s;
            }
        }

    public:
        static uint64_t Hash(const uintptr_t* key, uint32_t length)
        {
            uint64_t h = 0x9E3779B97F4A7C15ull ^ length;
            for (uint32_t i = 0; i < length; ++i)
            {
                h ^= key[i];
                h *= 0xBF58476D1CE4E5B9ull;
                h ^= h >> 31;
            }
            return h ^ (h >> 29);
        }

        void* Find(const uintptr_t* key, uint32_t length, uint64_t hash) const
        {
            const size_t mask = slots_.size() - 1;
            for (size_t i = hash & mask; slots_[i].record; i = (i + 1) & mask)
                if (slots_[i].hash == hash && Equal(slots_[i], key, length)) return slots_[i].record;
            return nullptr;
        }

        void Insert(const uintptr_t* key, uint32_t length, uint64_t hash, void* record)
        {
            if ((used_ + 1) * 2 > slots_.size()) Grow();
            const size_t mask = slots_.size() - 1;
            size_t i = hash & mask;
            while (slots_[i].record) i = (i + 1) & mask;
            slots_[i] = { hash, (uint32_t)keys_.size(), length, record };
            keys_.insert(keys_.end(), key, key + length);
            ++used_;
        }

        void Clear()
        {
            if (!used_) return;
            // runs once per model: do not sweep a world-sized table for small submodels
            if (slots_.size() > 4096 && used_ * 8 < slots_.size()) slots_.assign(1024, Slot{});
            else std::fill(slots_.begin(), slots_.end(), Slot{});
            keys_.clear();
            used_ = 0;
        }
    };

    inline ChainMap g_map;
    constexpr int kStockCapacity = 256;         // stock stack array size
    inline void* g_leaves[4096];

    inline void* FindOrCreate(void* a, void* b, void* ctx)
    {
        if (g_skip(a, b, ctx)) return a;
        const int first = g_flatten(a, g_leaves, 0, ctx);
        const int count = g_flatten(b, g_leaves, first, ctx);
        if (count == 1) return g_leaves[0];
        if (count < 2 || count > kStockCapacity) { ++g_passthrough; return g_findOrCreate(a, b, ctx); }
        int hidden = 0;
        for (int i = 1; i < count; ++i)
            if ((g_materialFlags(**static_cast<void***>(g_leaves[i])) & 0x70) == 0x10) hidden = i;
        if (hidden == count - 1) { ++g_passthrough; return g_findOrCreate(a, b, ctx); }

        const uintptr_t* key = reinterpret_cast<const uintptr_t*>(g_leaves + hidden);
        const uint32_t length = uint32_t(count - hidden);
        const uint64_t hash = ChainMap::Hash(key, length);
        if (void* record = g_map.Find(key, length, hash))
        {
            ++g_hits;
            if (!g_verify) return record;
            void* stock = g_findOrCreate(a, b, ctx);
            if (stock != record) ++g_mismatches;
            return stock;
        }
        ++g_misses;
        std::vector<uintptr_t> saved(key, key + length);  // flatten may overwrite g_leaves
        void* record;
        if (g_verify)
        {
            record = g_findOrCreate(a, b, ctx);
            if (!g_records.insert(record).second) ++g_mismatches;
            // the new record must be its bucket's head
            if (g_buckets[StockBucket(saved.data(), length * sizeof(uintptr_t))] != record) ++g_relinkFailures;
        }
        else
        {
            const unsigned bucket = StockBucket(saved.data(), length * sizeof(uintptr_t));
            uint8_t* head = g_buckets[bucket];
            g_buckets[bucket] = nullptr;
            record = g_findOrCreate(a, b, ctx);
            if (g_buckets[bucket] == record && record &&
                !*reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(record) + cod2mapaddr::CoalesceRecordNext))
            {
                *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(record) + cod2mapaddr::CoalesceRecordNext) = head;
                g_buckets[bucket] = static_cast<uint8_t*>(record);
            }
            else
            {
                // unexpected push: undo our detach, keep what stock linked
                ++g_relinkFailures;
                if (!g_buckets[bucket]) g_buckets[bucket] = head;
                else
                {
                    uint8_t* tail = g_buckets[bucket];
                    while (*reinterpret_cast<uint8_t**>(tail + cod2mapaddr::CoalesceRecordNext))
                        tail = *reinterpret_cast<uint8_t**>(tail + cod2mapaddr::CoalesceRecordNext);
                    *reinterpret_cast<uint8_t**>(tail + cod2mapaddr::CoalesceRecordNext) = head;
                }
            }
        }
        g_map.Insert(saved.data(), length, hash, record);
        return record;
    }

    inline void Clear()
    {
        g_map.Clear();
        g_records.clear();
        g_clear();
    }

    inline bool Verify(uintptr_t base)
    {
        using namespace cod2mapaddr;
        auto match = [base](uintptr_t rva, const unsigned char* sig, size_t size) {
            return !memcmp(reinterpret_cast<void*>(base + rva), sig, size);
        };
        return match(CoalesceFindOrCreateRva, CoalesceFindOrCreateSig, sizeof CoalesceFindOrCreateSig) &&
               match(CoalesceClearRva, CoalesceClearSig, sizeof CoalesceClearSig) &&
               match(CoalesceFlattenRva, CoalesceFlattenSig, sizeof CoalesceFlattenSig) &&
               match(CoalesceSkipRva, CoalesceSkipSig, sizeof CoalesceSkipSig) &&
               match(MaterialFlagsRva, MaterialFlagsSig, sizeof MaterialFlagsSig);
    }

    inline bool Install(uintptr_t base, bool verify)
    {
        using namespace cod2mapaddr;
        g_verify = verify;
        g_flatten = reinterpret_cast<FlattenFn>(base + CoalesceFlattenRva);
        g_skip = reinterpret_cast<SkipFn>(base + CoalesceSkipRva);
        g_materialFlags = reinterpret_cast<MaterialFlagsFn>(base + MaterialFlagsRva);
        g_buckets = reinterpret_cast<uint8_t**>(base + CoalesceBucketsRva);
        return MH_CreateHook(reinterpret_cast<void*>(base + CoalesceFindOrCreateRva), reinterpret_cast<void*>(&FindOrCreate),
                             reinterpret_cast<void**>(&g_findOrCreate)) == MH_OK &&
               MH_CreateHook(reinterpret_cast<void*>(base + CoalesceClearRva), reinterpret_cast<void*>(&Clear),
                             reinterpret_cast<void**>(&g_clear)) == MH_OK;
    }

    inline void Report(FILE* out)
    {
        fprintf(out, "[BetterBuild] Coalesce Lookup: %llu hits, %llu created, %llu stock paths", g_hits, g_misses, g_passthrough);
        if (g_verify) fprintf(out, ", %llu verify mismatches (stock answer kept)", g_mismatches);
        if (g_relinkFailures) fprintf(out, ", %llu unexpected bucket states", g_relinkFailures);
        fprintf(out, ".\n");
    }
}
