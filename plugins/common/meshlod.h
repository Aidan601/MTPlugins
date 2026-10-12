// meshopt_autolod: the auto LOD simplifier (linker+0x7AE170, APE+0x217930) replaced by meshoptimizer's simplifyWithUpdate.
// Only what the reducer reads back is produced: the face vector shrunk in place, corner positions, an attribute block per face.
#pragma once
#include <windows.h>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "third_party/meshoptimizer/meshoptimizer.h"

namespace meshlod
{
    // the linker's std::vector: begin, end, capacity
    struct Vec { unsigned char* begin; unsigned char* end; unsigned char* cap; };

    // face record (40 bytes): +0 u64 ids, +8 u64 and +16 u32 the stock simplifier's own, +20 three u32 corner indices,
    // +32 float* attributes, three corners of `stride` floats. Corner record (16 bytes): x, y, z, u32 weight hash.
    // Corner attributes: normal 0..2, color 3..6, uv 7..8, uv again 9..10, then one weight per bone.
    constexpr size_t kFace = 40, kCorner = 16, kNormal = 0, kColor = 3, kUv = 7, kUv2 = 9;
    // cache tag of these LODs ("mop2"): bump it and the linker's "_mo2" name suffix whenever the output changes
    constexpr uint32_t kCacheTag = 0x6D6F7032;

    using SimplifyFn = double (*)(void*, Vec*, Vec*, double, unsigned, float, Vec*);
    inline SimplifyFn g_simplify = nullptr;
    inline thread_local int t_stride = 0;                   // bones + 11 while the hooked reducer runs on this thread, else 0
    inline thread_local std::vector<float> t_attributes;    // what the reduced faces point at, freed when the reducer returns

    struct Welded { uint64_t ids; uint32_t corner; };

    inline uint64_t Hash(const void* p, size_t n, uint64_t h)
    {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001B3ull;
        return h;
    }

    inline double HookSimplify(void* params, Vec* faces, Vec* corners, double a4, unsigned target, float a6, Vec* out)
    {
        const int stride = t_stride;
        const size_t nf = faces && faces->begin ? size_t(faces->end - faces->begin) / kFace : 0;
        if (stride < 11 || nf < 2 || target >= nf || !corners || !corners->begin)
            return g_simplify(params, faces, corners, a4, target, a6, out);

        // weld corners that are the same vertex: same ids, position and every attribute
        const size_t nc = nf * 3;
        std::vector<Welded> welded;
        std::vector<float> pos, attr, full;   // per welded vertex: xyz, the 9 simplifier attributes, all `stride` floats
        std::vector<uint32_t> index(nc);
        welded.reserve(nc / 2); pos.reserve(nc / 2 * 3); attr.reserve(nc / 2 * 9); full.reserve(nc / 2 * stride);
        size_t buckets = 1;
        while (buckets < nc * 2) buckets <<= 1;
        std::vector<uint32_t> table(buckets, UINT32_MAX);
        const size_t attrBytes = sizeof(float) * stride;
        for (size_t f = 0; f < nf; ++f)
        {
            const unsigned char* rec = faces->begin + kFace * f;
            const uint64_t ids = *reinterpret_cast<const uint64_t*>(rec);
            const float* block = *reinterpret_cast<float* const*>(rec + 32);
            for (int k = 0; k < 3; ++k)
            {
                const uint32_t c = *reinterpret_cast<const uint32_t*>(rec + 20 + 4 * k);
                const float* p = reinterpret_cast<const float*>(corners->begin + kCorner * c);
                const float* a = block + size_t(stride) * k;
                uint64_t h = Hash(&ids, 8, 0xCBF29CE484222325ull);
                h = Hash(p, 16, h);           // position and the corner's weight hash
                h = Hash(a, 11 * sizeof(float), h);
                size_t slot = size_t(h) & (buckets - 1);
                uint32_t v;
                for (;; slot = (slot + 1) & (buckets - 1))
                {
                    v = table[slot];
                    if (v == UINT32_MAX)
                    {
                        v = uint32_t(welded.size());
                        table[slot] = v;
                        welded.push_back({ ids, c });
                        pos.insert(pos.end(), p, p + 3);
                        attr.insert(attr.end(), { a[kNormal], a[kNormal + 1], a[kNormal + 2], a[kUv], a[kUv + 1], a[kColor], a[kColor + 1], a[kColor + 2], a[kColor + 3] });
                        full.insert(full.end(), a, a + stride);
                        break;
                    }
                    if (welded[v].ids == ids && !memcmp(&pos[3 * size_t(v)], p, 12) && !memcmp(&full[size_t(stride) * v], a, attrBytes)) break;
                }
                index[3 * f + k] = v;
            }
        }
        table = {};

        // permissive: collapses may cross attribute splits (hard edges, uv seams) like stock's, which welds by position only.
        // low attribute weights keep shape first: higher ones let hard edges steer collapses into visible gaps
    static const float kWeights[9] = { 0.25f, 0.25f, 0.25f, 0.5f, 0.5f, 0.25f, 0.25f, 0.25f, 0.25f };   // normal, uv, color as in `attr`
        const size_t count = meshopt_simplifyWithUpdate(index.data(), index.size(), pos.data(), welded.size(), 12, attr.data(), 36, kWeights, 9, nullptr,
                                                        size_t(target) * 3, FLT_MAX, meshopt_SimplifyPermissive, nullptr);
        const size_t out_faces = count / 3;

        // write back: moved positions into each welded vertex's own corner record, faces in place
        std::vector<float>& store = t_attributes;
        store.assign(out_faces * 3 * size_t(stride), 0.0f);
        for (size_t t = 0; t < out_faces; ++t)
        {
            unsigned char* rec = faces->begin + kFace * t;
            const uint32_t* tri = &index[3 * t];
            *reinterpret_cast<uint64_t*>(rec) = welded[tri[0]].ids;
            *reinterpret_cast<uint64_t*>(rec + 8) = 0;
            *reinterpret_cast<uint32_t*>(rec + 16) = 0;
            float* block = &store[t * 3 * size_t(stride)];
            for (int k = 0; k < 3; ++k)
            {
                const uint32_t v = tri[k];
                *reinterpret_cast<uint32_t*>(rec + 20 + 4 * k) = welded[v].corner;
                memcpy(reinterpret_cast<float*>(corners->begin + kCorner * welded[v].corner), &pos[3 * size_t(v)], 12);
                float* a = block + size_t(stride) * k;
                memcpy(a, &full[size_t(stride) * v], attrBytes);
                const float* s = &attr[9 * size_t(v)];
                a[kNormal] = s[0]; a[kNormal + 1] = s[1]; a[kNormal + 2] = s[2];
                a[kUv] = a[kUv2] = s[3]; a[kUv + 1] = a[kUv2 + 1] = s[4];
                a[kColor] = s[5]; a[kColor + 1] = s[6]; a[kColor + 2] = s[7]; a[kColor + 3] = s[8];
            }
            *reinterpret_cast<float**>(rec + 32) = block;
        }
        faces->end = faces->begin + kFace * out_faces;
        return 0.0;
    }
}
