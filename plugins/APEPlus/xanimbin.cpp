// Source xanim export reader. (ADS export tracks).
#include "xanimbin.h"
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>

namespace ba::xanimbin
{
namespace
{
    bool Lz4Block(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t size)
    {
        out.clear();
        out.reserve(size);
        size_t i = 0;
        while (i < n)
        {
            const uint8_t tok = src[i++];
            size_t lit = tok >> 4;
            if (lit == 15) { uint8_t b; do { if (i >= n) return false; b = src[i++]; lit += b; } while (b == 255); }
            if (i + lit > n) return false;
            out.insert(out.end(), src + i, src + i + lit);
            i += lit;
            if (i >= n || out.size() >= size) break;
            if (i + 2 > n) return false;
            const size_t off = size_t(src[i]) | (size_t(src[i + 1]) << 8);
            i += 2;
            size_t ml = tok & 15;
            if (ml == 15) { uint8_t b; do { if (i >= n) return false; b = src[i++]; ml += b; } while (b == 255); }
            ml += 4;
            if (!off || off > out.size()) return false;
            const size_t st = out.size() - off;
            for (size_t k = 0; k < ml; ++k) out.push_back(out[st + k]);
        }
        return out.size() == size;
    }

    template <class T> T At(const std::vector<uint8_t>& d, size_t i) { T v; memcpy(&v, d.data() + i, sizeof v); return v; }

    // record tags (u16 at 4-byte aligned offsets)
    enum : uint16_t
    {
        kString = 0xC355, kBoneName = 0x360B, kFrame = 0xC723, kBone = 0x745A, kOffset = 0x9383, kScale = 0x1C56,
        kAxisX = 0xDCFD, kAxisY = 0xCCDC, kAxisZ = 0xFCBF, kFrameCount = 0xB917,
    };
}

bool Load(const std::string& path, Export& out, std::string& err)
{
    out = Export{};
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<uint8_t> d;
    if (file.size() > 9 && !memcmp(file.data(), "*LZ4*", 5))
    {
        const uint32_t size = At<uint32_t>(file, 5);
        if (size > (64u << 20) || !Lz4Block(file.data() + 9, file.size() - 9, d, size)) { err = "bad LZ4 data in " + path; return false; }
    }
    else d = std::move(file);
    std::vector<Pose>* frame = nullptr;
    Pose* bone = nullptr;
    size_t i = 0;
    auto skipString = [&](size_t from) { const void* z = memchr(d.data() + from, 0, d.size() - from); if (!z) return d.size(); return ((static_cast<const uint8_t*>(z) - d.data()) + 4) & ~size_t(3); };
    while (i + 4 <= d.size())
    {
        const uint16_t tag = At<uint16_t>(d, i);
        if (tag == kString) { i = skipString(i + 4); continue; }
        if (tag == kBoneName)
        {
            const uint16_t idx = At<uint16_t>(d, i + 2);
            const void* z = memchr(d.data() + i + 4, 0, d.size() - i - 4);
            if (!z) break;
            std::string name(reinterpret_cast<const char*>(d.data() + i + 4), static_cast<const uint8_t*>(z) - d.data() - i - 4);
            for (char& c : name) c = char(tolower(static_cast<unsigned char>(c)));
            if (idx >= out.bones.size()) out.bones.resize(size_t(idx) + 1);
            out.bones[idx] = name;
            i = skipString(i + 4);
            continue;
        }
        if (tag == kFrame && i + 8 <= d.size())
        {
            out.frames.emplace_back(out.bones.size());
            if (out.frames.size() > 1)   // a bone a frame leaves out keeps its last pose
                for (size_t b = 0; b < out.bones.size(); ++b) out.frames.back()[b] = out.frames[out.frames.size() - 2][b];
            frame = &out.frames.back();
            bone = nullptr;
            i += 8;
            continue;
        }
        if (tag == kBone)
        {
            const uint16_t idx = At<uint16_t>(d, i + 2);
            bone = frame && idx < frame->size() ? &(*frame)[idx] : nullptr;
            i += 4;
            continue;
        }
        if (tag == kOffset && i + 16 <= d.size()) { if (bone) { memcpy(bone->o, d.data() + i + 4, 12); bone->set = true; } i += 16; continue; }
        if (tag == kScale && i + 16 <= d.size()) { i += 16; continue; }
        if ((tag == kAxisX || tag == kAxisY || tag == kAxisZ) && i + 8 <= d.size())
        {
            if (bone)
            {
                const int row = tag == kAxisX ? 0 : tag == kAxisY ? 1 : 2;
                for (int c = 0; c < 3; ++c) bone->axis[row][c] = float(At<int16_t>(d, i + 2 + size_t(c) * 2)) / 32767.0f;
            }
            i += 8;
            continue;
        }
        if (tag == kFrameCount) { i += 8; continue; }
        i += 4;
    }
    if (out.bones.empty() || out.frames.empty()) { err = "no bones or frames in " + path; return false; }
    return true;
}
}
