#pragma once
// C9: material tessSize lookup (SplitSparseSoups). Stock queries the asset database per soup record for the
// same few materials. The answer depends only on the name, so it is cached per name (hashed in place, one
// copy per distinct name). A miss calls stock. Verify mode calls stock every time and compares.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../../../src/radiant_addrs.h"
#include "../../../src/third_party/minhook/include/MinHook.h"

namespace tesssize
{
    using TessSizeFn = float (*)(const uint8_t* material);
    inline TessSizeFn g_tessSize = nullptr;  // trampoline
    inline bool g_verify = false;
    inline unsigned long long g_calls = 0, g_hits = 0, g_mismatches = 0;

    struct Entry { uint64_t hash; std::string name; float value; bool used; };
    inline std::vector<Entry>* g_slots = nullptr;   // open addressing, grows at half full
    inline size_t g_count = 0;

    // the exe's VS2012 std::string: 16-byte buffer or pointer, size +16, capacity +24
    inline const char* Name(const uint8_t* material, size_t& size)
    {
        const uint8_t* s = material + 48;
        size = *reinterpret_cast<const size_t*>(s + 16);
        const size_t capacity = *reinterpret_cast<const size_t*>(s + 24);
        return capacity >= 16 ? *reinterpret_cast<const char* const*>(s) : reinterpret_cast<const char*>(s);
    }

    inline uint64_t Hash(const char* text, size_t size)
    {
        uint64_t h = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < size; ++i) { h ^= uint8_t(text[i]); h *= 0x100000001B3ull; }
        return h ? h : 1;
    }

    inline Entry& Slot(std::vector<Entry>& slots, uint64_t hash, const char* text, size_t size)
    {
        const size_t mask = slots.size() - 1;
        for (size_t i = size_t(hash) & mask;; i = (i + 1) & mask)
        {
            Entry& e = slots[i];
            if (!e.used || (e.hash == hash && e.name.size() == size && !memcmp(e.name.data(), text, size))) return e;
        }
    }

    inline Entry& Find(const char* text, size_t size, uint64_t hash)
    {
        if (!g_slots) g_slots = new std::vector<Entry>(1024);
        if (g_count * 2 >= g_slots->size())
        {
            auto* bigger = new std::vector<Entry>(g_slots->size() * 2);
            for (Entry& e : *g_slots)
                if (e.used) Slot(*bigger, e.hash, e.name.data(), e.name.size()) = std::move(e);
            delete g_slots;
            g_slots = bigger;
        }
        return Slot(*g_slots, hash, text, size);
    }

    inline float TessSize(const uint8_t* material)
    {
        ++g_calls;
        if (*reinterpret_cast<const int*>(material + 24) != 1) return g_tessSize(material);
        size_t size = 0;
        const char* text = Name(material, size);
        Entry& e = Find(text, size, Hash(text, size));
        if (!e.used)
        {
            const float value = g_tessSize(material);
            e.hash = Hash(text, size); e.name.assign(text, size); e.value = value; e.used = true;
            ++g_count;
            return value;
        }
        ++g_hits;
        if (!g_verify) return e.value;
        const float stock = g_tessSize(material);
        if (memcmp(&stock, &e.value, sizeof stock)) ++g_mismatches;
        return stock;
    }

    inline bool Verify(uintptr_t base)
    {
        using namespace cod2mapaddr;
        return !memcmp(reinterpret_cast<void*>(base + TessSizeRva), TessSizeSig, sizeof TessSizeSig);
    }

    inline bool Install(uintptr_t base, bool verify)
    {
        g_verify = verify;
        return MH_CreateHook(reinterpret_cast<void*>(base + cod2mapaddr::TessSizeRva), reinterpret_cast<void*>(&TessSize),
                             reinterpret_cast<void**>(&g_tessSize)) == MH_OK;
    }

    inline void Report(FILE* out)
    {
        fprintf(out, "[BetterBuild] TessSize Cache: %llu calls, %llu cached, %zu names", g_calls, g_hits, g_count);
        if (g_verify) fprintf(out, ", %llu verify mismatches (stock kept)", g_mismatches);
        fprintf(out, ".\n");
    }
}
