#pragma once
// Directed-edge table for C4 and C11: exact endpoint positions as the key (-0 folded into +0, NaN never
// inserted), triangles chained per edge. Open addressing over reused arrays, so no heap traffic per soup.
// Chains are newest first, callers sort the candidates.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace edgetable
{
    struct Key
    {
        uint32_t v[6];
        bool operator==(const Key& o) const { return !memcmp(v, o.v, sizeof v); }
    };

    inline uint64_t Hash(const Key& k)
    {
        uint64_t h = 0x9E3779B97F4A7C15ull;
        for (uint32_t x : k.v) { h ^= x; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 31; }
        return h;
    }

    // false when a coordinate is NaN
    inline bool MakeKey(const float* from, const float* to, Key& key)
    {
        for (int c = 0; c < 3; ++c)
        {
            if (std::isnan(from[c]) || std::isnan(to[c])) return false;
            const float a = from[c] + 0.0f, b = to[c] + 0.0f;  // -0 + 0 = +0
            memcpy(&key.v[c], &a, 4);
            memcpy(&key.v[3 + c], &b, 4);
        }
        return true;
    }

    struct Table
    {
        struct Slot { Key key; uint32_t head; };      // head: entry index, kEmpty for a free slot
        struct Entry { uint32_t tri, next; };
        static constexpr uint32_t kEmpty = 0xFFFFFFFFu;
        std::vector<Slot> slots;
        std::vector<Entry> entries;
        size_t mask = 0;

        void Reset(size_t edges)
        {
            size_t size = 1024;
            while (size < edges * 2) size <<= 1;
            if (slots.size() != size) slots.resize(size);
            for (Slot& s : slots) s.head = kEmpty;
            mask = size - 1;
            entries.clear();
            entries.reserve(edges);
        }
        Slot& Find(const Key& key, bool create)
        {
            for (size_t i = Hash(key) & mask;; i = (i + 1) & mask)
            {
                Slot& s = slots[i];
                if (s.head == kEmpty) { if (create) s.key = key; return s; }
                if (s.key == key) return s;
            }
        }
        // once per edge, even for a degenerate triangle listing it twice
        void Insert(const Key& key, uint32_t tri)
        {
            Slot& s = Find(key, true);
            if (s.head != kEmpty && entries[s.head].tri == tri) return;
            entries.push_back({ tri, s.head });
            s.head = uint32_t(entries.size() - 1);
        }
        void Gather(const Key& key, std::vector<uint32_t>& out) const
        {
            for (size_t i = Hash(key) & mask;; i = (i + 1) & mask)
            {
                const Slot& s = slots[i];
                if (s.head == kEmpty) return;
                if (s.key == key)
                {
                    for (uint32_t e = s.head; e != kEmpty; e = entries[e].next) out.push_back(entries[e].tri);
                    return;
                }
            }
        }
    };

    // ascending, duplicates removed
    inline void SortUnique(std::vector<uint32_t>& v)
    {
        if (v.size() > 24) std::sort(v.begin(), v.end());
        else
            for (size_t i = 1; i < v.size(); ++i)
            {
                const uint32_t x = v[i];
                size_t j = i;
                while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; --j; }
                v[j] = x;
            }
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }
}
