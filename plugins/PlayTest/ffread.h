// ffread.h: what a level load compiles from the reflection probes, read back out of the linked fastfile.
// Header only (Radiant side of the live bake, also built into build\lb_probe\fftest.cpp).

// MAP
//   Inflate ................. zlib stream -> bytes (tables as in stb_image's zlib)
//   UnpackFastFile .......... <map>.ff -> its zone stream (blocks inflated on several threads)
//   ReadLedProbeLayout ...... the .led's probe configs and blend counts per sun volume
//   FindProbeGroups ......... per (sun volume, lighting state): the linker's GfxReflectionProbe records, probe blends,
//                             the probe image and the three probe volume images, the two streamed buffers' keys
//   WriteProbeFile .......... the handoff file the bridge reads (BO3LiveBridge livebake.cpp, ReadProbeFile)
//
// (blocks from 0x248, zlib, pointer -1 = data follows).
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace brff
{
    // ---- inflate ------------------------------------------------------------------------------------------------
    namespace detail
    {
        constexpr int kFastBits = 9, kFastMask = (1 << kFastBits) - 1;
        struct Huffman
        {
            uint16_t fast[1 << kFastBits];
            uint16_t firstCode[16];
            int maxCode[17];
            uint16_t firstSymbol[16];
            uint8_t size[288];
            uint16_t value[288];
        };
        inline int BitReverse(int v, int bits)
        {
            int n = v;
            n = ((n & 0xAAAA) >> 1) | ((n & 0x5555) << 1);
            n = ((n & 0xCCCC) >> 2) | ((n & 0x3333) << 2);
            n = ((n & 0xF0F0) >> 4) | ((n & 0x0F0F) << 4);
            n = ((n & 0xFF00) >> 8) | ((n & 0x00FF) << 8);
            return n >> (16 - bits);
        }
        inline bool Build(Huffman& z, const uint8_t* sizes, int num)
        {
            int count[17] = {}, nextCode[16] = {};
            std::memset(z.fast, 0, sizeof z.fast);
            for (int i = 0; i < num; ++i) ++count[sizes[i]];
            count[0] = 0;
            for (int i = 1; i < 16; ++i) if (count[i] > (1 << i)) return false;
            int code = 0, k = 0;
            for (int i = 1; i < 16; ++i)
            {
                nextCode[i] = code;
                z.firstCode[i] = static_cast<uint16_t>(code);
                z.firstSymbol[i] = static_cast<uint16_t>(k);
                code += count[i];
                if (count[i] && code - 1 >= (1 << i)) return false;
                z.maxCode[i] = code << (16 - i);
                code <<= 1;
                k += count[i];
            }
            z.maxCode[16] = 0x10000;
            for (int i = 0; i < num; ++i)
            {
                const int s = sizes[i];
                if (!s) continue;
                const int c = nextCode[s] - z.firstCode[s] + z.firstSymbol[s];
                z.size[c] = static_cast<uint8_t>(s);
                z.value[c] = static_cast<uint16_t>(i);
                if (s <= kFastBits)
                    for (int j = BitReverse(nextCode[s], s); j < (1 << kFastBits); j += 1 << s) z.fast[j] = static_cast<uint16_t>((s << 9) | i);
                ++nextCode[s];
            }
            return true;
        }
        struct Reader
        {
            const uint8_t* p; const uint8_t* end;
            uint32_t bits = 0; int count = 0;
            void Fill() { while (count <= 24) { bits |= static_cast<uint32_t>(p < end ? *p++ : 0) << count; count += 8; } }
            uint32_t Take(int n) { if (count < n) Fill(); const uint32_t v = bits & ((1u << n) - 1); bits >>= n; count -= n; return v; }
            int Decode(const Huffman& z)
            {
                if (count < 16) Fill();
                const int b = z.fast[bits & kFastMask];
                if (b) { const int s = b >> 9; bits >>= s; count -= s; return b & 511; }
                const int k = BitReverse(static_cast<int>(bits & 0xFFFF), 16);
                int s = kFastBits + 1;
                for (;; ++s) if (k < z.maxCode[s]) break;
                if (s >= 16) return -1;
                const int at = (k >> (16 - s)) - z.firstCode[s] + z.firstSymbol[s];
                if (at < 0 || at >= 288 || z.size[at] != s) return -1;
                bits >>= s; count -= s;
                return z.value[at];
            }
        };
        const int kLengthBase[31] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258,0,0 };
        const int kLengthExtra[31] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0,0,0 };
        const int kDistBase[32] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577,0,0 };
        const int kDistExtra[32] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13,0,0 };
    }

    // A zlib stream into exactly outLen bytes (the fastfile's blocks say their decompressed size).
    inline bool Inflate(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen)
    {
        using namespace detail;
        if (inLen < 2 || (in[0] & 15) != 8 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 32)) return false;
        Reader r{ in + 2, in + inLen };
        size_t o = 0;
        Huffman lengths, dists;
        for (;;)
        {
            const uint32_t last = r.Take(1), type = r.Take(2);
            if (type == 0)
            {
                r.Take(r.count & 7);   // to the byte boundary
                uint8_t h[4]; int k = 0;
                while (r.count > 0 && k < 4) { h[k++] = static_cast<uint8_t>(r.bits & 255); r.bits >>= 8; r.count -= 8; }
                while (k < 4) { if (r.p >= r.end) return false; h[k++] = *r.p++; }
                const uint32_t len = h[0] | (h[1] << 8), nlen = h[2] | (h[3] << 8);
                if ((len ^ 0xFFFF) != nlen || len > static_cast<size_t>(r.end - r.p) || len > outLen - o) return false;
                std::memcpy(out + o, r.p, len); r.p += len; o += len;
            }
            else if (type == 3) return false;
            else
            {
                if (type == 1)
                {
                    uint8_t s[288 + 32];
                    for (int i = 0; i < 144; ++i) s[i] = 8;
                    for (int i = 144; i < 256; ++i) s[i] = 9;
                    for (int i = 256; i < 280; ++i) s[i] = 7;
                    for (int i = 280; i < 288; ++i) s[i] = 8;
                    uint8_t d[32]; std::memset(d, 5, sizeof d);
                    if (!Build(lengths, s, 288) || !Build(dists, d, 32)) return false;
                }
                else
                {
                    static const uint8_t order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
                    const int hlit = static_cast<int>(r.Take(5)) + 257, hdist = static_cast<int>(r.Take(5)) + 1, hclen = static_cast<int>(r.Take(4)) + 4;
                    uint8_t codeSizes[19] = {};
                    for (int i = 0; i < hclen; ++i) codeSizes[order[i]] = static_cast<uint8_t>(r.Take(3));
                    Huffman codes;
                    if (!Build(codes, codeSizes, 19)) return false;
                    uint8_t all[286 + 32 + 137] = {};
                    int n = 0;
                    while (n < hlit + hdist)
                    {
                        const int c = r.Decode(codes);
                        if (c < 0 || c >= 19) return false;
                        if (c < 16) { all[n++] = static_cast<uint8_t>(c); continue; }
                        uint8_t fill = 0; int rep = 0;
                        if (c == 16) { if (!n) return false; rep = static_cast<int>(r.Take(2)) + 3; fill = all[n - 1]; }
                        else if (c == 17) rep = static_cast<int>(r.Take(3)) + 3;
                        else rep = static_cast<int>(r.Take(7)) + 11;
                        if (hlit + hdist - n < rep) return false;
                        std::memset(all + n, fill, static_cast<size_t>(rep)); n += rep;
                    }
                    if (n != hlit + hdist || !Build(lengths, all, hlit) || !Build(dists, all + hlit, hdist)) return false;
                }
                for (;;)
                {
                    int z = r.Decode(lengths);
                    if (z < 0) return false;
                    if (z < 256) { if (o >= outLen) return false; out[o++] = static_cast<uint8_t>(z); continue; }
                    if (z == 256) break;
                    z -= 257;
                    if (z >= 29) return false;
                    size_t len = static_cast<size_t>(kLengthBase[z]);
                    if (kLengthExtra[z]) len += r.Take(kLengthExtra[z]);
                    z = r.Decode(dists);
                    if (z < 0 || z >= 30) return false;
                    size_t dist = static_cast<size_t>(kDistBase[z]);
                    if (kDistExtra[z]) dist += r.Take(kDistExtra[z]);
                    if (dist > o || len > outLen - o) return false;
                    const uint8_t* from = out + o - dist;
                    for (size_t i = 0; i < len; ++i) out[o + i] = from[i];   // may overlap: byte by byte
                    o += len;
                }
            }
            if (last) break;
        }
        return o == outLen;
    }

    // ---- fastfile -----------------------------------------------------------------------------------------------
    inline bool UnpackFastFile(const std::wstring& path, std::vector<uint8_t>& zone, std::string& error)
    {
        zone.clear();
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) { error = "the fastfile could not be opened"; return false; }
        std::vector<uint8_t> d;
        _fseeki64(f, 0, SEEK_END); const long long size = _ftelli64(f); _fseeki64(f, 0, SEEK_SET);
        if (size < 0x260 || size > (6ll << 30)) { fclose(f); error = "the fastfile has an unexpected size"; return false; }
        d.resize(static_cast<size_t>(size));
        const bool got = fread(d.data(), 1, d.size(), f) == d.size();
        fclose(f);
        uint32_t version = 0; uint64_t total = 0;
        if (got) { std::memcpy(&version, d.data() + 8, 4); std::memcpy(&total, d.data() + 0x90, 8); }
        if (!got || std::memcmp(d.data(), "TAff0000", 8) != 0 || version != 0x251 || d[0x0F] != 0 || !total || total > (16ull << 30))
        { error = "not a mod tools fastfile this reader knows (TAff0000, version 0x251, not encrypted)"; return false; }
        struct Block { size_t in, inLen, out, outLen; };
        std::vector<Block> blocks;
        size_t o = 0x248; uint64_t outAt = 0;
        while (o + 16 <= d.size() && outAt < total)
        {
            uint32_t h[4]; std::memcpy(h, d.data() + o, 16);
            if (!h[0] || h[2] < h[0] || h[3] != o || h[0] > d.size() - o - 16) break;
            if (!h[1]) { o = (o + 0x800000) & ~static_cast<size_t>(0x7FFFFF); continue; }   // filler up to the next 8 MB boundary
            if (h[1] > total - outAt) break;
            blocks.push_back({ o + 16, h[0], static_cast<size_t>(outAt), h[1] });
            outAt += h[1];
            o += 16 + static_cast<size_t>(h[2]);
        }
        if (outAt != total) { error = "the fastfile's blocks do not add up to its size"; return false; }
        zone.resize(static_cast<size_t>(total));
        std::atomic<size_t> next{ 0 };
        std::atomic<bool> ok{ true };
        auto work = [&]()
        {
            for (size_t i; ok && (i = next.fetch_add(1)) < blocks.size();)
                if (!Inflate(d.data() + blocks[i].in, blocks[i].inLen, zone.data() + blocks[i].out, blocks[i].outLen)) ok = false;
        };
        unsigned threads = std::thread::hardware_concurrency();
        if (threads < 1) threads = 1;
        if (threads > 8) threads = 8;
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work);
        work();
        for (std::thread& t : pool) t.join();
        if (!ok) { zone.clear(); error = "a fastfile block did not inflate"; return false; }
        return true;
    }

    // ---- the .led's probe layout ----------------------------------------------------------------------------------
    struct LedVolume
    {
        uint32_t mask = 0;                               // lighting states the volume has data for
        std::vector<std::array<uint8_t, 188>> probes;    // GfxConfig_Probe as exported (origin first)
        uint32_t blends = 0;
    };
    inline bool ReadLedProbeLayout(const std::wstring& path, std::vector<LedVolume>& out)
    {
        out.clear();
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) return false;
        std::vector<uint8_t> d;
        _fseeki64(f, 0, SEEK_END); const long long size = _ftelli64(f); _fseeki64(f, 0, SEEK_SET);
        if (size < 32 || size > (3ll << 30)) { fclose(f); return false; }
        d.resize(static_cast<size_t>(size));
        const bool got = fread(d.data(), 1, d.size(), f) == d.size();
        fclose(f);
        if (!got || std::memcmp(d.data(), "LED", 4) != 0) return false;
        size_t o = 0, end = d.size();
        bool ok = true;
        auto u32 = [&]() -> uint32_t { if (o + 4 > end) { ok = false; return 0; } uint32_t v; std::memcpy(&v, d.data() + o, 4); o += 4; return v; };
        auto skip = [&](uint64_t n) { if (n > end - o) { ok = false; o = end; } else o += static_cast<size_t>(n); };
        auto vec = [&](uint64_t element) { const uint32_t n = u32(); skip(uint64_t(n) * element); return n; };
        o = 12; const uint32_t headerText = u32(); skip(headerText);
        const uint32_t sections = u32();
        uint32_t version = 0; size_t at = 0, len = 0;
        for (uint32_t i = 0; i < sections && ok; ++i)
        {
            const uint32_t nameLen = u32();
            if (!ok || nameLen > end - o) return false;
            const std::string name(reinterpret_cast<const char*>(d.data() + o), nameLen); o += nameLen;
            const uint32_t ver = u32(), sz = u32();
            if (!ok || sz > end - o) return false;
            if (name == "volumes") { version = ver; at = o; len = sz; }
            o += sz;
        }
        if (!len || version != 3) return false;
        o = at; end = at + len;
        const uint32_t volumes = u32();
        for (uint32_t v = 0; v < volumes && ok; ++v)
        {
            LedVolume lv;
            u32(); lv.mask = u32();
            skip(13); vec(16);
            for (int ls = 0; ls < 4; ++ls)
            {
                if (!(lv.mask >> ls & 1)) continue;
                vec(1); skip(64 + 8 + 16);
                const uint32_t mips = u32();
                for (uint32_t m = 0; m < mips && ok; ++m) vec(4);
                vec(1); skip(8); vec(4); vec(4);
            }
            const uint32_t infos = u32();
            for (uint32_t i = 0; i < infos && ok; ++i) { vec(1); skip(4 + 16); }
            const uint32_t probes = u32();
            for (uint32_t p = 0; p < probes && ok; ++p)
            {
                if (o + 188 > end) { ok = false; break; }
                std::array<uint8_t, 188> c; std::memcpy(c.data(), d.data() + o, 188);
                lv.probes.push_back(c);
                skip(188);
                for (int ls = 0; ls < 4 && ok; ++ls)
                {
                    if (!(lv.mask >> ls & 1)) continue;
                    vec(6); vec(6); vec(6); vec(1); vec(1); vec(1); vec(2); vec(2); vec(2); skip(12);
                    const uint32_t mipCount = u32(), mipCount2 = u32();
                    if (mipCount != mipCount2 || mipCount > 32) { ok = false; break; }
                    for (uint32_t m = 0; m < mipCount * 6 && ok; ++m) vec(1);
                    skip(16);
                }
            }
            lv.blends = vec(584);
            vec(16);
            const uint32_t extensions = u32();
            for (uint32_t e = 0; e < extensions && ok; ++e) vec(16);
            if (ok) out.push_back(std::move(lv));
        }
        return ok && o == end;
    }

    // ---- the linker's probe data of one sun volume and lighting state -------------------------------------------------
    constexpr size_t kImageSize = 264, kProbeSize = 328, kBlendSize = 604, kProbeConfigAt = 0x2C, kBufferSize = 0x40;
    struct ProbeGroup
    {
        int32_t volume = 0, state = 0;
        uint32_t probes = 0, blends = 0;                 // records in the array (the level's local count is probes - 1)
        uint64_t probeKey = 0, gridKey = 0;              // the two StreamWrappedBuffers (reflection probes, probe volume)
        uint32_t probeDataSize = 0, gridDataSize = 0;
        uint8_t probeImage[kImageSize] = {};
        uint8_t gridImage[3][kImageSize] = {};           // probe_x / y / z textures
        std::vector<uint8_t> configs, blendData;
    };
    namespace detail
    {
        // every place the text stands as an inline asset name: 264 bytes of GfxImage before it, whose name pointer reads -1
        inline std::vector<size_t> FindImageNames(const std::vector<uint8_t>& zone, const std::string& prefix)
        {
            std::vector<size_t> hits;
            if (zone.size() < prefix.size() + kImageSize) return hits;
            const uint8_t first = static_cast<uint8_t>(prefix[0]);
            const uint8_t* base = zone.data();
            const size_t last = zone.size() - prefix.size();
            for (size_t i = kImageSize; i <= last; ++i)
            {
                const void* at = std::memchr(base + i, first, last - i + 1);
                if (!at) break;
                i = static_cast<size_t>(static_cast<const uint8_t*>(at) - base);
                if (std::memcmp(base + i, prefix.data(), prefix.size()) != 0) continue;
                uint64_t namePointer = 0; std::memcpy(&namePointer, base + i - 16, 8);
                if (namePointer == ~0ull) hits.push_back(i);
            }
            return hits;
        }
        inline size_t NameEnd(const std::vector<uint8_t>& zone, size_t at)
        {
            while (at < zone.size() && zone[at]) ++at;
            return at + 1;
        }
        inline bool Buffer(const std::vector<uint8_t>& zone, size_t at, uint8_t type, uint64_t& key, uint32_t& dataSize)
        {
            if (at + kBufferSize > zone.size()) return false;
            uint32_t handle = 0; std::memcpy(&handle, zone.data() + at, 4);
            if (handle != 0xFFFFFFFFu || zone[at + 0x30] != type) return false;
            std::memcpy(&key, zone.data() + at + 8, 8); std::memcpy(&dataSize, zone.data() + at + 0x34, 4);
            return key != 0 && dataSize != 0;
        }
    }
    inline bool FindProbeGroups(const std::vector<uint8_t>& zone, const std::vector<LedVolume>& led, std::vector<ProbeGroup>& out, std::string& notes)
    {
        using namespace detail;
        out.clear();
        for (size_t v = 0; v < led.size(); ++v)
        {
            const LedVolume& lv = led[v];
            if (lv.probes.empty()) continue;
            for (int s = 0; s < 4; ++s)
            {
                if (!(lv.mask >> s & 1)) continue;
                char tag[96];
                auto fail = [&](const char* what) { char t[200]; snprintf(t, sizeof t, "volume %u state %d: %s; ", static_cast<unsigned>(v), s, what); notes += t; };
                ProbeGroup g; g.volume = static_cast<int32_t>(v); g.state = s;
                g.probes = static_cast<uint32_t>(lv.probes.size()); g.blends = lv.blends;
                // the probe array: image, configs, blends, buffer
                snprintf(tag, sizeof tag, "volume%u_state%d_reflection_probes_", static_cast<unsigned>(v), s);
                bool found = false;
                for (size_t name : FindImageNames(zone, tag))
                {
                    const size_t configs = NameEnd(zone, name), blends = configs + size_t(g.probes) * kProbeSize, buffer = blends + size_t(g.blends) * kBlendSize;
                    if (buffer + kBufferSize > zone.size()) continue;
                    uint16_t depth = 0; std::memcpy(&depth, zone.data() + name - kImageSize + 0xC8, 2);
                    if (depth != g.probes) continue;
                    bool same = true;
                    for (uint32_t p = 0; p < g.probes && same; ++p)
                        same = std::memcmp(zone.data() + configs + size_t(p) * kProbeSize + kProbeConfigAt, lv.probes[p].data(), 48) == 0;   // origin and rotation, in the .led's order
                    if (!same || !Buffer(zone, buffer, 0, g.probeKey, g.probeDataSize)) continue;
                    std::memcpy(g.probeImage, zone.data() + name - kImageSize, kImageSize);
                    g.configs.assign(zone.begin() + configs, zone.begin() + blends);
                    g.blendData.assign(zone.begin() + blends, zone.begin() + buffer);
                    found = true;
                    break;
                }
                if (!found) { fail("the probe array was not found in the fastfile as the .led describes it"); continue; }
                // the probe volume: three images one after the other, then the buffer
                size_t after = 0; bool grid = true;
                static const char axis[3] = { 'x', 'y', 'z' };
                for (int a = 0; a < 3 && grid; ++a)
                {
                    snprintf(tag, sizeof tag, "volume%u_state%d_probe_%c_texture_", static_cast<unsigned>(v), s, axis[a]);
                    const std::vector<size_t> names = FindImageNames(zone, tag);
                    if (names.size() != 1 || (a && names[0] - kImageSize != after)) { grid = false; break; }
                    std::memcpy(g.gridImage[a], zone.data() + names[0] - kImageSize, kImageSize);
                    after = NameEnd(zone, names[0]);
                }
                if (!grid || !Buffer(zone, after, 1, g.gridKey, g.gridDataSize)) { fail("the probe volume textures were not found in the fastfile"); continue; }
                uint32_t sizes = 0;
                for (int a = 0; a < 3; ++a) { uint32_t t = 0; std::memcpy(&t, g.gridImage[a] + 0xE8, 4); sizes += t; }
                if (sizes != g.gridDataSize) { fail("the probe volume textures do not add up to their buffer"); continue; }
                out.push_back(std::move(g));
            }
        }
        return !out.empty();
    }

    // "BRPL", version 1, group count, per group the fixed part, then configs and blends.
    inline bool WriteProbeFile(const std::wstring& path, const std::vector<ProbeGroup>& groups)
    {
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f) return false;
        bool ok = true;
        auto put = [&](const void* p, size_t n) { if (n && fwrite(p, 1, n, f) != n) ok = false; };
        const uint32_t head[3] = { 0x4C505242u, 1u, static_cast<uint32_t>(groups.size()) };
        put(head, sizeof head);
        for (const ProbeGroup& g : groups)
        {
            put(&g.volume, 4); put(&g.state, 4); put(&g.probes, 4); put(&g.blends, 4);
            put(&g.probeKey, 8); put(&g.gridKey, 8); put(&g.probeDataSize, 4); put(&g.gridDataSize, 4);
            put(g.probeImage, kImageSize); put(g.gridImage, sizeof g.gridImage);
            put(g.configs.data(), g.configs.size()); put(g.blendData.data(), g.blendData.size());
        }
        if (fclose(f) != 0) ok = false;
        return ok;
    }
}
