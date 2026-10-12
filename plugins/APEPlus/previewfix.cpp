// Preview fixes for xanims. APE adds each bone's bind offset to the anim's translation, right for useBones 1 anims
// (stored as deltas) and doubled for useBones 0 ones (full offsets, the viewmodel kind). The Preview Options attach
// ignores the child tag and hides the model for good when the parent tag is missing.
#include "modtools_plugin.hpp"
#include "previewfix.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace ba::previewfix
{
namespace
{
    // IDA addresses (base 0x140000000), rebased through ApeAddress
    const uintptr_t kConvert = 0x1406368B0;      // (shared_ptr<Anim>* out, const std::string& name, int mode), mode 1 = stub
    const uintptr_t kLocalPose = 0x140481070;    // ModelInst local pose from an anim, a8 = the model's bone vector
    const uintptr_t kAttach = 0x140632170;       // (entries, parent entry, model, parent tag, child tag): the Preview Options attach
    const uintptr_t kPlace = 0x1406331D0;        // (entry, ?, bool): per-frame placement on the parent's tag, false = not drawn
    const uintptr_t kSetPlacement = 0x140647FD0; // (holder, const Xform*, float scale)
    const uintptr_t kSetAnim = 0x14047EA90;      // (holder, const std::string& anim name, int mode)
    const uintptr_t kRender = 0x140145740;       // (widget data, ...): poses, placement and draw, the frame labels follow it
    const uintptr_t kGdtAsset = 0x14012C5E0;     // (shared_ptr<GdtAsset>* out, const std::string& name, const std::string& type)
    const uintptr_t kNeedChildTag = 0x14012F2D6;
    const uintptr_t kAssign = 0x140050060;       // std::string::assign(const char*, size_t) with APE's allocator // selection handler: cmp [child tag size], 0 then jz, no attach without a child tag

    // preview entry (32 x 1336 bytes from previewWindowOpts + 2440)
    const size_t kEntryFlags = 4;                // 1 shown, 2 parented, 4 tag missing (sticky in APE)
    const size_t kEntryParentTag = 264, kEntryChildTag = 520;   // char[256]
    const size_t kEntryInUse = 777;
    const size_t kEntryHolder = 792;             // shared_ptr<ModelInst>
    const size_t kEntryTagXform = 1240;          // the parent tag's world transform from the last placement
    const size_t kEntryParent = 1312;
    const size_t kEntryAnimNames = 1080, kEntryAnimIndex = 1304;   // std::string[], int: APE advances an entry's play time only when its current name is set
    const size_t kEntryStride = 1336;
    // ModelInst
    const size_t kInstOrigin = 16, kInstAxis = 28, kInstScale = 64, kInstAnim = 104;
    // model data (*(*ModelInst + 104)), anim
    const size_t kInstT = 224;                   // float play position 0..1
    const size_t kModelBones = 120;              // non-root bones, 64 bytes: std::string name, int parent, quat, vec3 bind offset
    const size_t kModelBind = 144;               // bind pose, 32 bytes per bone (roots first): quat, vec3, 2/|q|^2
    const size_t kModelNames = 216;              // bone names in bind pose order, std::string
    const size_t kBoneSize = 64, kBoneParent = 32, kBoneOffset = 52, kPoseSize = 32;   // bone: std::string name, int parent (-1 = the model root), quat, vec3 bind offset
    const size_t kAnimType = 64, kAnimFrames = 76, kAnimParts = 208, kPartSize = 104;
    const size_t kPartQuats = 32, kPartTrans = 56;   // vectors of quat and vec3 per frame
    const int kTypeAdditive = 4;

    struct Xform { float o[3]; float a[9]; };     // row vectors: world = p * a + o

    // an MSVC std::string over our own characters, read only by APE
    struct MsvcStr
    {
        union { char buf[16]; const char* ptr; };
        size_t size, cap;
    };
    MsvcStr Str(const char* s)
    {
        MsvcStr m; memset(&m, 0, sizeof m);
        m.size = strlen(s);
        if (m.size < 16) { memcpy(m.buf, s, m.size); m.cap = 15; } else { m.ptr = s; m.cap = m.size; }
        return m;
    }

    using ConvertFn = int64_t* (*)(int64_t* out, void* name, int mode);
    using LocalPoseFn = int64_t (*)(void*, void*, void*, void*, uint64_t, uint64_t, void*, void*);
    using AttachFn = char (*)(uint8_t* entries, uint8_t* parent, void* model, void* ptag, void* ctag);
    using PlaceFn = bool (*)(uint8_t* entry, void* a2, char a3);
    using SetPlacementFn = void (*)(void* holder, const Xform* x, float scale);
    using SetAnimFn = void* (*)(void* holder, const MsvcStr* name, unsigned mode);
    using RenderFn = int64_t (*)(void*, void*, void*);
    using GdtAssetFn = int64_t* (*)(int64_t* out, const MsvcStr* name, const MsvcStr* type);
    using GetIntFn = unsigned (*)(void* asset, const MsvcStr* key);

    ConvertFn g_origConvert = nullptr;
    LocalPoseFn g_origPose = nullptr;
    AttachFn g_origAttach = nullptr;
    PlaceFn g_origPlace = nullptr;
    RenderFn g_origRender = nullptr;
    SetPlacementFn g_setPlacement = nullptr;
    SetAnimFn g_setAnim = nullptr;
    GdtAssetFn g_gdtAsset = nullptr;
    using AssignFn = void* (*)(void* str, const char* s, size_t n);
    AssignFn g_assign = nullptr;
    bool g_ready = false;
    bool g_on = true;                 // [xanimpreview] enabled
    bool g_attachAnim = true;         // [xanimpreview] attach_anim

    // ---- useBones per anim object (converter, any thread) ----
    struct AnimInfo { bool fullOffsets; std::string name; };
    SRWLOCK g_lock = SRWLOCK_INIT;
    std::unordered_map<void*, AnimInfo> g_anims;
    unsigned g_fullCount = 0, g_poseCalls = 0;

    std::string ReadStr(const void* s)
    {
        const uint8_t* p = static_cast<const uint8_t*>(s);
        const size_t size = *reinterpret_cast<const size_t*>(p + 16), cap = *reinterpret_cast<const size_t*>(p + 24);
        const char* c = cap >= 16 ? *reinterpret_cast<const char* const*>(p) : reinterpret_cast<const char*>(p);
        return size < 4096 ? std::string(c, size) : std::string();
    }

    void Release(int64_t ctrl)
    {
        if (!ctrl) return;
        if (InterlockedDecrement(reinterpret_cast<volatile LONG*>(ctrl + 8)) == 0)
        {
            void** vt = *reinterpret_cast<void***>(ctrl);
            reinterpret_cast<void (*)(int64_t)>(vt[0])(ctrl);
            if (InterlockedDecrement(reinterpret_cast<volatile LONG*>(ctrl + 12)) == 0) reinterpret_cast<void (*)(int64_t)>(vt[1])(ctrl);
        }
    }

    // 1 = useBones on, 0 = off, -1 = unknown, from the same gdt accessor the converter uses
    int UseBones(const char* name)
    {
        __try
        {
            MsvcStr n = Str(name), type = Str("xanim"), key = Str("useBones");
            int64_t sp[2] = {0, 0};
            g_gdtAsset(sp, &n, &type);
            int r = -1;
            if (sp[0])
            {
                void** vt = *reinterpret_cast<void***>(sp[0]);
                r = reinterpret_cast<GetIntFn>(vt[3])(reinterpret_cast<void*>(sp[0]), &key) != 0 ? 1 : 0;
            }
            Release(sp[1]);
            return r;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }

    int64_t* hk_Convert(int64_t* out, void* name, int mode)
    {
        int64_t* r = g_origConvert(out, name, mode);
        void* anim = out ? reinterpret_cast<void*>(out[0]) : nullptr;
        if (!anim) return r;
        AnimInfo info{false, name ? ReadStr(name) : std::string()};
        if (mode != 1 && info.name.size() < 256)
            info.fullOffsets = UseBones(info.name.c_str()) == 0 && *reinterpret_cast<int*>(static_cast<uint8_t*>(anim) + kAnimType) != kTypeAdditive;
        AcquireSRWLockExclusive(&g_lock);
        if (g_anims.size() > 4096) g_anims.clear();
        g_anims[anim] = info;
        if (info.fullOffsets) ++g_fullCount;
        ReleaseSRWLockExclusive(&g_lock);
        return r;
    }

    bool FullOffsets(void* anim)
    {
        AcquireSRWLockShared(&g_lock);
        auto it = g_anims.find(anim);
        const bool r = it != g_anims.end() && it->second.fullOffsets;
        ReleaseSRWLockShared(&g_lock);
        return r;
    }

    std::string AnimName(void* anim)
    {
        AcquireSRWLockShared(&g_lock);
        auto it = g_anims.find(anim);
        std::string r = it != g_anims.end() ? it->second.name : std::string();
        ReleaseSRWLockShared(&g_lock);
        return r;
    }

    // ---- layers: a torso anim (pt_) over a base anim (pb_) for the bones it does not move ----
    struct Layer
    {
        std::string base;                    // base anim name
        void* baseAnim = nullptr; int64_t baseCtrl = 0;
        void* top = nullptr; uint8_t* bones = nullptr;   // the combined anim below was built for these
        std::vector<uint8_t> header, parts;  // combined anim: header copy, top parts then base parts (104 bytes each)
        std::vector<std::vector<float>> bufs;
        std::vector<int> map; int* mapVec[3] = {};
        void* headerPtr = nullptr;
    };
    std::unordered_map<std::string, Layer> g_layers;   // top anim name -> layer, main thread
    unsigned g_layerCalls = 0;
    std::vector<std::string> g_seen;     // anim names the local pose saw while layers were set (status)

    bool CopyBytes(void* dst, const void* src, size_t n)
    {
        __try { memcpy(dst, src, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    const char* NameAt(const uint8_t* s, size_t& n)
    {
        n = *reinterpret_cast<const size_t*>(s + 16);
        return *reinterpret_cast<const size_t*>(s + 24) >= 16 ? *reinterpret_cast<const char* const*>(s) : reinterpret_cast<const char*>(s);
    }
    // a vector<T> (begin, end, cap) over our own buffer
    void PutVec(uint8_t* at, const float* b, size_t floats)
    {
        const float* e = b + floats;
        memcpy(at, &b, 8); memcpy(at + 8, &e, 8); memcpy(at + 16, &e, 8);
    }

    bool BuildLayer(Layer& L, void* top, const int* topMap, uint8_t* const* bv)
    {
        const uint8_t* ta = static_cast<const uint8_t*>(top);
        const uint8_t* ba = static_cast<const uint8_t*>(L.baseAnim);
        L.header.assign(240, 0);
        if (!CopyBytes(L.header.data(), ta, 240)) return false;
        const int topFrames = std::max(1, *reinterpret_cast<const int*>(ta + kAnimFrames));
        const float topFps = *reinterpret_cast<const float*>(ta + 72), baseFps = *reinterpret_cast<const float*>(ba + 72);
        const int baseFrames = std::max(1, *reinterpret_cast<const int*>(ba + kAnimFrames));
        uint8_t* const* tp = reinterpret_cast<uint8_t* const*>(ta + kAnimParts);
        uint8_t* const* bp = reinterpret_cast<uint8_t* const*>(ba + kAnimParts);
        const size_t ntp = size_t(tp[1] - tp[0]) / kPartSize, nbp = size_t(bp[1] - bp[0]) / kPartSize;
        const size_t nb = size_t(bv[1] - bv[0]) / kBoneSize;
        L.parts.assign(tp[0], tp[0] + ntp * kPartSize);
        L.map.assign(nb, -1);
        L.bufs.clear();
        L.bufs.reserve(nb * 3);
        for (size_t i = 0; i < nb; ++i)
        {
            if (topMap[i] >= 0) { L.map[i] = topMap[i]; continue; }
            size_t bn; const char* bname = NameAt(bv[0] + i * kBoneSize, bn);
            for (size_t k = 0; k < nbp; ++k)
            {
                const uint8_t* part = bp[0] + k * kPartSize;
                size_t pn; const char* pname = NameAt(part, pn);
                if (pn != bn || memcmp(pname, bname, bn)) continue;
                const float* q = *reinterpret_cast<float* const*>(part + 32);
                const float* v = *reinterpret_cast<float* const*>(part + 56);
                const float* w = *reinterpret_cast<float* const*>(part + 80);
                const size_t nq = size_t(*reinterpret_cast<float* const*>(part + 40) - q) / 4, nv = size_t(*reinterpret_cast<float* const*>(part + 64) - v) / 3;
                const size_t nw = w ? size_t(*reinterpret_cast<float* const*>(part + 88) - w) / 4 : 0;
                if (!nq || !nv) break;
                std::vector<float> Q(size_t(topFrames) * 4), V(size_t(topFrames) * 3), W(size_t(topFrames) * 4, 0.0f);
                for (int f = 0; f < topFrames; ++f)
                {
                    // the base keeps its own speed under the top anim
                    const double sec = topFps > 0 ? f / double(topFps) : 0.0;
                    const size_t bf = size_t(llround(sec * (baseFps > 0 ? baseFps : 30.0))) % size_t(baseFrames);
                    memcpy(&Q[size_t(f) * 4], q + std::min(bf, nq - 1) * 4, 16);
                    memcpy(&V[size_t(f) * 3], v + std::min(bf, nv - 1) * 3, 12);
                    if (nw) memcpy(&W[size_t(f) * 4], w + std::min(bf, nw - 1) * 4, 16);
                }
                std::vector<uint8_t> np(part, part + kPartSize);
                L.bufs.push_back(std::move(Q)); PutVec(np.data() + 32, L.bufs.back().data(), L.bufs.back().size());
                L.bufs.push_back(std::move(V)); PutVec(np.data() + 56, L.bufs.back().data(), L.bufs.back().size());
                L.bufs.push_back(std::move(W)); PutVec(np.data() + 80, L.bufs.back().data(), L.bufs.back().size());
                L.map[i] = int(L.parts.size() / kPartSize);
                L.parts.insert(L.parts.end(), np.begin(), np.end());
                break;
            }
        }
        uint8_t* pb = L.parts.data(); uint8_t* pe = pb + L.parts.size();
        memcpy(L.header.data() + kAnimParts, &pb, 8); memcpy(L.header.data() + kAnimParts + 8, &pe, 8); memcpy(L.header.data() + kAnimParts + 16, &pe, 8);
        L.mapVec[0] = L.map.data(); L.mapVec[1] = L.map.data() + L.map.size(); L.mapVec[2] = L.mapVec[1];
        L.headerPtr = L.header.data();
        L.top = top; L.bones = bv[0];
        return true;
    }
    bool BuildLayerSafe(Layer& L, void* top, const int* topMap, uint8_t* const* bv)
    {
        __try { return BuildLayer(L, top, topMap, bv); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // ---- ADS nodes: ADS Up and Down blended 1:1 per bone with the playing anim, as the game's viewmodel tree does ----
    struct Overlaid
    {
        std::vector<uint8_t> header, parts;
        std::vector<std::vector<float>> bufs;
        std::vector<int> map; int* mapVec[3] = {};
        void* headerPtr = nullptr;
    };
    struct AdsNode
    {
        std::string name;
        void* anim = nullptr; int64_t ctrl = 0;
        float weight = 0, time = 0;          // time 0 to 1 over the anim, set each frame
        AdsTracks tracks; bool hasTracks = false;
    };
    struct AdsState
    {
        AdsNode node[2];                     // ADS Up, ADS Down
        Overlaid built;                      // rebuilt on every pose call, an anim address can be freed and reused
        unsigned calls = 0, hits = 0;
        std::string note;                    // how the playing anim's bones under the ADS anims counted (status)
    };
    AdsState g_ads;

    struct Track { const float* q = nullptr; const float* v = nullptr; const float* w = nullptr; size_t nq = 0, nv = 0, nw = 0; };
    Track TrackOf(const uint8_t* part)
    {
        Track t;
        t.q = *reinterpret_cast<float* const*>(part + 32);
        t.v = *reinterpret_cast<float* const*>(part + 56);
        t.w = *reinterpret_cast<float* const*>(part + 80);
        t.nq = t.q ? size_t(*reinterpret_cast<float* const*>(part + 40) - t.q) / 4 : 0;
        t.nv = t.v ? size_t(*reinterpret_cast<float* const*>(part + 64) - t.v) / 3 : 0;
        t.nw = t.w ? size_t(*reinterpret_cast<float* const*>(part + 88) - t.w) / 4 : 0;
        return t;
    }

    // the track at time 0 to 1 (first key to last), linear between keys
    void Sample(const Track& t, float time, float* q, float* v)
    {
        const float fq = std::clamp(time, 0.0f, 1.0f) * float(t.nq - 1), fv = std::clamp(time, 0.0f, 1.0f) * float(t.nv - 1);
        const size_t q0 = size_t(fq), q1 = std::min(q0 + 1, t.nq - 1), v0 = size_t(fv), v1 = std::min(v0 + 1, t.nv - 1);
        const float aq = fq - float(q0), av = fv - float(v0);
        const float* A = t.q + q0 * 4; const float* B = t.q + q1 * 4;
        const float sgn = A[0] * B[0] + A[1] * B[1] + A[2] * B[2] + A[3] * B[3] < 0 ? -1.0f : 1.0f;
        float len = 0;
        for (int c = 0; c < 4; ++c) { q[c] = A[c] * (1 - aq) + B[c] * sgn * aq; len += q[c] * q[c]; }
        len = len > 0 ? 1.0f / std::sqrt(len) : 1.0f;
        for (int c = 0; c < 4; ++c) q[c] *= len;
        for (int c = 0; c < 3; ++c) v[c] = t.v[v0 * 3 + c] * (1 - av) + t.v[v1 * 3 + c] * av;
    }

    void SampleKeys(const std::vector<AdsKey>& keys, float time, float* q, float* v)
    {
        const float f = std::clamp(time, 0.0f, 1.0f) * float(keys.size() - 1);
        const size_t k0 = size_t(f), k1 = std::min(k0 + 1, keys.size() - 1);
        const float a = f - float(k0);
        const AdsKey& A = keys[k0]; const AdsKey& B = keys[k1];
        const float sgn = A.q[0] * B.q[0] + A.q[1] * B.q[1] + A.q[2] * B.q[2] + A.q[3] * B.q[3] < 0 ? -1.0f : 1.0f;
        float len = 0;
        for (int c = 0; c < 4; ++c) { q[c] = A.q[c] * (1 - a) + B.q[c] * sgn * a; len += q[c] * q[c]; }
        len = len > 0 ? 1.0f / std::sqrt(len) : 1.0f;
        for (int c = 0; c < 4; ++c) q[c] *= len;
        for (int c = 0; c < 3; ++c) v[c] = A.v[c] * (1 - a) + B.v[c] * a;
    }

    std::string LowerName(const char* p, size_t n) { std::string s(p, n); for (char& c : s) c = char(tolower(static_cast<unsigned char>(c))); return s; }

    const uint8_t* FindPart(const void* anim, const char* name, size_t n)
    {
        uint8_t* const* pp = reinterpret_cast<uint8_t* const*>(static_cast<const uint8_t*>(anim) + kAnimParts);
        const size_t np = size_t(pp[1] - pp[0]) / kPartSize;
        for (size_t k = 0; k < np; ++k)
        {
            size_t pn; const char* pname = NameAt(pp[0] + k * kPartSize, pn);
            if (pn == n && !memcmp(pname, name, n)) return pp[0] + k * kPartSize;
        }
        return nullptr;
    }

    bool BuildAds(Overlaid& O, void* top, const int* topMap, uint8_t* const* bv, std::string* note)
    {
        const uint8_t* ta = static_cast<const uint8_t*>(top);
        O.header.assign(240, 0);
        if (!CopyBytes(O.header.data(), ta, 240)) return false;
        const size_t frames = size_t(std::max(1, *reinterpret_cast<const int*>(ta + kAnimFrames)));
        uint8_t* const* tp = reinterpret_cast<uint8_t* const*>(ta + kAnimParts);
        const size_t ntp = size_t(tp[1] - tp[0]) / kPartSize;
        const size_t nb = size_t(bv[1] - bv[0]) / kBoneSize;
        O.parts.assign(tp[0], tp[0] + ntp * kPartSize);
        O.map.assign(topMap, topMap + nb);
        O.bufs.clear();
        O.bufs.reserve(nb * 3);
        bool any = false;
        for (size_t i = 0; i < nb; ++i)
        {
            const uint8_t* bone = bv[0] + i * kBoneSize;
            size_t bn; const char* bname = NameAt(bone, bn);
            float nq[2][4], nv[2][3], nw[2] = {0, 0};
            const uint8_t* npart = nullptr;
            bool fromExport = false;
            for (int k = 0; k < 2; ++k)
            {
                const AdsNode& N = g_ads.node[k];
                if (!N.anim || N.weight <= 0.0001f) continue;
                const uint8_t* part = FindPart(N.anim, bname, bn);
                if (N.hasTracks)
                {
                    auto it = N.tracks.find(LowerName(bname, bn));
                    if (it == N.tracks.end() || it->second.empty()) continue;
                    SampleKeys(it->second, N.time, nq[k], nv[k]);
                    fromExport = true;
                }
                else
                {
                    if (!part) continue;
                    const Track t = TrackOf(part);
                    if (!t.nq || !t.nv) continue;
                    Sample(t, N.time, nq[k], nv[k]);
                }
                nw[k] = N.weight;
                if (!npart) npart = part;
            }
            const uint8_t* tpart = topMap[i] >= 0 && size_t(topMap[i]) < ntp ? tp[0] + size_t(topMap[i]) * kPartSize : nullptr;
            if (!npart && fromExport) npart = tpart;
            if (!npart) continue;
            const Track T = tpart ? TrackOf(tpart) : Track{};
            // the converter drops an export's root bones, a bone with no ancestor that has a track in the anim
            bool rooted = false;
            for (int p = *reinterpret_cast<const int*>(bone + kBoneParent), guard = 0; p >= 0 && size_t(p) < nb && guard < 64 && !rooted; ++guard)
            {
                rooted = topMap[p] >= 0;
                p = *reinterpret_cast<const int*>(bv[0] + size_t(p) * kBoneSize + kBoneParent);
            }
            const bool joins = T.nq && T.nv && rooted;
            if (note) *note += std::string(bname, bn) + (joins ? " in the playing anim (1:1 with ADS)" : tpart ? " a root of the playing anim, dropped by the converter (ADS alone)" : " not in the playing anim (ADS alone)")
                             + (fromExport ? ", ADS from the export; " : "; ");
            const size_t keys = std::max({frames + 1, T.nq, T.nv});
            std::vector<float> Q(keys * 4), V(keys * 3), Wt(keys * 4, 0.0f);
            for (size_t f = 0; f < keys; ++f)
            {
                float q[4] = {0, 0, 0, 0}, v[3] = {0, 0, 0}, wsum = 0;
                const float* ref = nullptr;
                auto add = [&](const float* aq, const float* av, float w)
                {
                    if (!ref) ref = aq;
                    const float sgn = aq[0] * ref[0] + aq[1] * ref[1] + aq[2] * ref[2] + aq[3] * ref[3] < 0 ? -1.0f : 1.0f;
                    for (int c = 0; c < 4; ++c) q[c] += aq[c] * sgn * w;
                    for (int c = 0; c < 3; ++c) v[c] += av[c] * w;
                    wsum += w;
                };
                if (joins) add(T.q + std::min(f, T.nq - 1) * 4, T.v + std::min(f, T.nv - 1) * 3, 1.0f);
                for (int k = 0; k < 2; ++k) if (nw[k] > 0) add(nq[k], nv[k], nw[k]);
                float len = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
                len = len > 0 ? 1.0f / std::sqrt(len) : 1.0f;
                for (int c = 0; c < 4; ++c) Q[f * 4 + c] = q[c] * len;
                for (int c = 0; c < 3; ++c) V[f * 3 + c] = wsum > 0 ? v[c] / wsum : 0.0f;
                if (T.nw) memcpy(&Wt[f * 4], T.w + std::min(f, T.nw - 1) * 4, 16);
            }
            std::vector<uint8_t> np(tpart ? tpart : npart, (tpart ? tpart : npart) + kPartSize);
            O.bufs.push_back(std::move(Q)); PutVec(np.data() + 32, O.bufs.back().data(), O.bufs.back().size());
            O.bufs.push_back(std::move(V)); PutVec(np.data() + 56, O.bufs.back().data(), O.bufs.back().size());
            O.bufs.push_back(std::move(Wt)); PutVec(np.data() + 80, O.bufs.back().data(), O.bufs.back().size());
            if (tpart) memcpy(O.parts.data() + size_t(topMap[i]) * kPartSize, np.data(), kPartSize);
            else { O.map[i] = int(O.parts.size() / kPartSize); O.parts.insert(O.parts.end(), np.begin(), np.end()); }
            any = true;
        }
        if (!any) return false;
        uint8_t* pb = O.parts.data(); uint8_t* pe = pb + O.parts.size();
        memcpy(O.header.data() + kAnimParts, &pb, 8); memcpy(O.header.data() + kAnimParts + 8, &pe, 8); memcpy(O.header.data() + kAnimParts + 16, &pe, 8);
        O.mapVec[0] = O.map.data(); O.mapVec[1] = O.map.data() + O.map.size(); O.mapVec[2] = O.mapVec[1];
        O.headerPtr = O.header.data();
        return true;
    }
    bool BuildAdsSafe(Overlaid& O, void* top, const int* topMap, uint8_t* const* bv, std::string* note)
    {
        __try { return BuildAds(O, top, topMap, bv, note); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    std::string AnimName(void* anim);

    // ---- local pose: a bone copy without the bind offset on animated bones ----
    thread_local std::vector<uint8_t> t_bones;

    int64_t hk_LocalPose(void* a1, void* a2, void* a3, void* animRef, uint64_t t, uint64_t flag, void* partMap, void* bones)
    {
        void* anim = animRef ? *static_cast<void**>(animRef) : nullptr;
        bool layered = false;
        if (g_on && anim && bones && partMap && !g_layers.empty())
        {
            const std::string nm = AnimName(anim);
            if (g_seen.size() < 8 && std::find(g_seen.begin(), g_seen.end(), nm) == g_seen.end()) g_seen.push_back(nm);
            auto it = g_layers.find(nm);
            uint8_t* const* bv = static_cast<uint8_t* const*>(bones);
            if (it != g_layers.end() && it->second.baseAnim)
            {
                Layer& L = it->second;
                if ((L.top != anim || L.bones != bv[0]) && !BuildLayerSafe(L, anim, *static_cast<int* const*>(partMap), bv)) L.top = nullptr;
                if (L.top == anim) { animRef = &L.headerPtr; partMap = L.mapVec; ++g_layerCalls; layered = true; }
            }
        }
        if (!layered && g_on && anim && bones && partMap && (g_ads.node[0].anim || g_ads.node[1].anim))
        {
            const std::string nm = AnimName(anim);
            if (nm != g_ads.node[0].name && nm != g_ads.node[1].name)
            {
                ++g_ads.calls;
                std::string note;
                if (BuildAdsSafe(g_ads.built, anim, *static_cast<int* const*>(partMap), static_cast<uint8_t* const*>(bones), &note))
                {
                    animRef = &g_ads.built.headerPtr; partMap = g_ads.built.mapVec; ++g_ads.hits;
                    g_ads.note = nm + ": " + note;
                }
            }
        }
        if (!g_on || !anim || !bones || !partMap || !FullOffsets(anim)) return g_origPose(a1, a2, a3, animRef, t, flag, partMap, bones);
        uint8_t* const* v = static_cast<uint8_t* const*>(bones);
        const int* map = *static_cast<int* const*>(partMap);
        const size_t n = v[0] && v[1] > v[0] ? size_t(v[1] - v[0]) / kBoneSize : 0;
        if (!n || !map || n > 4096) return g_origPose(a1, a2, a3, animRef, t, flag, partMap, bones);
        t_bones.assign(v[0], v[0] + n * kBoneSize);
        for (size_t i = 0; i < n; ++i)
            if (map[i] >= 0) memset(t_bones.data() + i * kBoneSize + kBoneOffset, 0, 12);
        uint8_t* copy[3] = {t_bones.data(), t_bones.data() + n * kBoneSize, t_bones.data() + n * kBoneSize};
        ++g_poseCalls;
        return g_origPose(a1, a2, a3, animRef, t, flag, partMap, copy);
    }

    // ---- attach: tag check, swapped tags, child tag offset, the anim on the attached model ----
    struct Att
    {
        uint8_t* entry = nullptr;
        uint8_t* parent = nullptr;
        std::string model, ptag, ctag;       // as typed in the asset
        std::string usePtag;                 // the parent tag the placement uses ("" = none)
        std::string animPart;                // the anim part that drives an anim-driven attach
        std::string useCtag;
        Xform child{};                       // the child tag in the attached model's bind pose
        bool haveChild = false, childIsRoot = false, animDriven = false;
        bool onParentBone = false;           // parent is itself attached: placed on its bone as the parent's anim poses it
        bool resolved = false, animated = false, swapped = false;
        ULONGLONG lastTry = 0;
        void* partAnim = nullptr;            // the anim partIndex was found in
        int partIndex = -1;
        std::string note;
    };
    std::vector<Att> g_atts;
    std::unordered_map<std::string, bool> g_told;

    void* Holder(uint8_t* entry) { return entry + kEntryHolder; }
    uint8_t* Inst(uint8_t* entry) { return *reinterpret_cast<uint8_t**>(entry + kEntryHolder); }

    void Mul(const float* x, const float* y, float* r)   // 3x3, r = x * y
    {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r[i * 3 + j] = x[i * 3] * y[j] + x[i * 3 + 1] * y[3 + j] + x[i * 3 + 2] * y[6 + j];
    }
    void Transpose(const float* x, float* r) { for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) r[i * 3 + j] = x[j * 3 + i]; }
    void VecMul(const float* v, const float* m, float* r) { for (int j = 0; j < 3; ++j) r[j] = v[0] * m[j] + v[1] * m[3 + j] + v[2] * m[6 + j]; }

    // quaternion (x, y, z, w) to axis rows, the same matrix as APE's com_math
    void QuatAxis(const float* q, float* m)
    {
        const float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
        const float s = n > 1e-12f ? 2.0f / n : 0.0f;
        const float xx = q[0] * q[0] * s, yy = q[1] * q[1] * s, zz = q[2] * q[2] * s;
        const float xy = q[0] * q[1] * s, xz = q[0] * q[2] * s, yz = q[1] * q[2] * s;
        const float wx = q[3] * q[0] * s, wy = q[3] * q[1] * s, wz = q[3] * q[2] * s;
        m[0] = 1 - (yy + zz); m[1] = xy + wz;       m[2] = xz - wy;
        m[3] = xy - wz;       m[4] = 1 - (xx + zz); m[5] = yz + wx;
        m[6] = xz + wy;       m[7] = yz - wx;       m[8] = 1 - (xx + yy);
    }

    bool StrAt(const uint8_t* s, const char*& p, size_t& n)
    {
        n = *reinterpret_cast<const size_t*>(s + 16);
        p = *reinterpret_cast<const size_t*>(s + 24) >= 16 ? *reinterpret_cast<const char* const*>(s) : reinterpret_cast<const char*>(s);
        return n < 512;
    }
    bool StrIs(const uint8_t* s, const char* want, size_t wn)
    {
        const char* p; size_t n;
        return StrAt(s, p, n) && n == wn && !memcmp(p, want, n);
    }

    uint8_t* ModelOf(uint8_t* entry)
    {
        __try
        {
            const uint8_t* inst = Inst(entry);
            const uint8_t* wrap = inst ? *reinterpret_cast<uint8_t* const*>(inst) : nullptr;
            return wrap ? *reinterpret_cast<uint8_t* const*>(wrap + 104) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    // the model's bone vector is filled once its conversion finished
    bool ModelReady(uint8_t* entry)
    {
        uint8_t* model = ModelOf(entry);
        __try
        {
            uint8_t* const* bv = model ? reinterpret_cast<uint8_t* const*>(model + kModelBones) : nullptr;
            return bv && bv[0] && bv[1] > bv[0];
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // index into the bind pose (roots first), -1 when the model has no such bone
    int BoneIndex(uint8_t* model, const std::string& name, bool* isRoot)
    {
        __try
        {
            uint8_t* const* nv = reinterpret_cast<uint8_t* const*>(model + kModelNames);
            uint8_t* const* bv = reinterpret_cast<uint8_t* const*>(model + kModelBones);
            uint8_t* const* pv = reinterpret_cast<uint8_t* const*>(model + kModelBind);
            const size_t nn = nv[1] > nv[0] ? size_t(nv[1] - nv[0]) / 32 : 0;
            const size_t nb = bv[1] > bv[0] ? size_t(bv[1] - bv[0]) / kBoneSize : 0;
            const size_t np = pv[1] > pv[0] ? size_t(pv[1] - pv[0]) / kPoseSize : 0;
            const size_t nr = np > nb ? np - nb : 0;
            for (size_t i = 0; i < nn && i < np && i < 1024; ++i)
                if (StrIs(nv[0] + i * 32, name.data(), name.size())) { if (isRoot) *isRoot = i < nr; return int(i); }
            for (size_t i = 0; i < nb && i < 1024; ++i)
                if (StrIs(bv[0] + i * kBoneSize, name.data(), name.size())) { if (isRoot) *isRoot = false; return int(nr + i); }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return -1;
    }
    bool RootNameRaw(uint8_t* model, const char*& p, size_t& n)
    {
        __try
        {
            uint8_t* const* nv = reinterpret_cast<uint8_t* const*>(model + kModelNames);
            return nv[1] > nv[0] && StrAt(nv[0], p, n);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    std::string RootName(uint8_t* model)
    {
        const char* p = nullptr; size_t n = 0;
        return model && RootNameRaw(model, p, n) ? std::string(p, n) : std::string();
    }

    bool HasTag(uint8_t* entry, const std::string& tag)
    {
        uint8_t* model = ModelOf(entry);
        return tag == "tag_origin" || (model && BoneIndex(model, tag, nullptr) >= 0);
    }

    bool BindXform(uint8_t* model, int index, Xform& x)
    {
        __try
        {
            uint8_t* const* pv = reinterpret_cast<uint8_t* const*>(model + kModelBind);
            if (index < 0 || size_t(pv[1] - pv[0]) / kPoseSize <= size_t(index)) return false;
            const float* e = reinterpret_cast<const float*>(pv[0] + size_t(index) * kPoseSize);
            QuatAxis(e, x.a);
            x.o[0] = e[4]; x.o[1] = e[5]; x.o[2] = e[6];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    int PartIndexRaw(void* anim, const char* name, size_t n)
    {
        uint8_t* const* pv = reinterpret_cast<uint8_t* const*>(static_cast<uint8_t*>(anim) + kAnimParts);
        const size_t np = pv[1] > pv[0] ? size_t(pv[1] - pv[0]) / kPartSize : 0;
        for (size_t k = 0; k < np && k < 1024; ++k)
            if (StrIs(pv[0] + k * kPartSize, name, n)) return int(k);
        return -1;
    }
    int PartIndex(void* anim, const std::string& name)
    {
        __try
        {
            uint8_t* const* pv = reinterpret_cast<uint8_t* const*>(static_cast<uint8_t*>(anim) + kAnimParts);
            const size_t np = pv[1] > pv[0] ? size_t(pv[1] - pv[0]) / kPartSize : 0;
            for (size_t k = 0; k < np && k < 1024; ++k)
                if (StrIs(pv[0] + k * kPartSize, name.data(), name.size())) return int(k);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return -1;
    }

    // the part's local transform at play position t (0..1), interpolated like ModelInst.cpp
    bool PartLocal(void* anim, int part, float t, Xform& x)
    {
        __try
        {
            const uint8_t* a = static_cast<const uint8_t*>(anim);
            const int frames = *reinterpret_cast<const int*>(a + kAnimFrames);
            const uint8_t* p = *reinterpret_cast<uint8_t* const*>(a + kAnimParts) + size_t(part) * kPartSize;
            const float* qv = *reinterpret_cast<float* const*>(p + kPartQuats);
            const float* qe = *reinterpret_cast<float* const*>(p + kPartQuats + 8);
            const float* tv = *reinterpret_cast<float* const*>(p + kPartTrans);
            const float* te = *reinterpret_cast<float* const*>(p + kPartTrans + 8);
            const size_t nq = size_t(qe - qv) / 4, nt = size_t(te - tv) / 3;
            if (frames <= 0 || !nq || !nt) return false;
            const float f = float(frames - 1) * (t < 0 ? 0 : t > 1 ? 1 : t);
            size_t i0 = size_t(f), i1 = i0 + 1 < size_t(frames) ? i0 + 1 : i0;
            const float u = f - float(i0);
            if (i0 >= nq || i0 >= nt) i0 = (nq < nt ? nq : nt) - 1;
            if (i1 >= nq || i1 >= nt) i1 = i0;
            const float* q0 = qv + i0 * 4; const float* q1 = qv + i1 * 4;
            const float sign = q0[0] * q1[0] + q0[1] * q1[1] + q0[2] * q1[2] + q0[3] * q1[3] < 0 ? -1.0f : 1.0f;
            float q[4];
            for (int k = 0; k < 4; ++k) q[k] = q0[k] * (1 - u) + q1[k] * sign * u;
            QuatAxis(q, x.a);
            for (int k = 0; k < 3; ++k) x.o[k] = tv[i0 * 3 + k] * (1 - u) + tv[i1 * 3 + k] * u;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool SharesBones(uint8_t* modelInst, void* anim)
    {
        __try
        {
            const uint8_t* model = *reinterpret_cast<uint8_t* const*>(*reinterpret_cast<uint8_t* const*>(modelInst) + 104);
            uint8_t* const* bv = reinterpret_cast<uint8_t* const*>(model + kModelBones);
            uint8_t* const* pv = reinterpret_cast<uint8_t* const*>(static_cast<uint8_t*>(anim) + kAnimParts);
            const size_t nb = bv[1] > bv[0] ? size_t(bv[1] - bv[0]) / kBoneSize : 0;
            const size_t np = pv[1] > pv[0] ? size_t(pv[1] - pv[0]) / kPartSize : 0;
            for (size_t i = 0; i < nb && i < 1024; ++i)
            {
                const char* b; size_t bn;
                if (!StrAt(bv[0] + i * kBoneSize, b, bn)) continue;
                for (size_t k = 0; k < np && k < 1024; ++k)
                    if (StrIs(pv[0] + k * kPartSize, b, bn)) return true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return false;
    }

    bool CallSetAnim(void* holder, const MsvcStr* name)
    {
        __try { g_setAnim(holder, name, 0); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // the entry's anim name, without which its play position stays at 0 (the joints of an attached gun never move)
    bool SetEntryAnimName(uint8_t* entry, const char* name, size_t n)
    {
        if (!g_assign) return false;
        __try
        {
            const int idx = *reinterpret_cast<int*>(entry + kEntryAnimIndex);
            if (idx < 0 || idx > 5) return false;
            g_assign(entry + kEntryAnimNames + size_t(idx) * 32, name, n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void Tell(const Att& a, const std::string& text)
    {
        const std::string key = a.model + "|" + a.ptag + "|" + a.ctag + "|" + text;
        if (g_told.count(key)) return;
        g_told[key] = true;
        rp::Log("xanimpreview: %s", text.c_str());
        if (rp::Ape()) rp::Ape()->Print(rp::self, ("Preview: " + text).c_str());
    }

    // a bone of an entry as the entry's anim poses it: anim locals up the bone chain (bind pose where the anim has no
    // track) on the entry's placement. APE's own tag transform is wrong for a model that is itself attached.
    bool AnimTagWorldRaw(uint8_t* entry, const char* tag, size_t tagLen, Xform& out)
    {
        __try
        {
            uint8_t* model = ModelOf(entry);
            uint8_t* inst = Inst(entry);
            if (!model || !inst) return false;
            uint8_t* const* bv = reinterpret_cast<uint8_t* const*>(model + kModelBones);
            uint8_t* const* pv = reinterpret_cast<uint8_t* const*>(model + kModelBind);
            const size_t nb = bv[1] > bv[0] ? size_t(bv[1] - bv[0]) / kBoneSize : 0;
            const size_t np = pv[1] > pv[0] ? size_t(pv[1] - pv[0]) / kPoseSize : 0;
            const size_t nr = np > nb ? np - nb : 0;
            int bi = -1;
            for (size_t i = 0; i < nb && i < 1024 && bi < 0; ++i) if (StrIs(bv[0] + i * kBoneSize, tag, tagLen)) bi = int(i);
            if (bi < 0) return false;
            void* anim = *reinterpret_cast<void**>(inst + kInstAnim);
            if (anim && !FullOffsets(anim)) anim = nullptr;
            const float t = *reinterpret_cast<const float*>(inst + kInstT);
            Xform acc{}; acc.a[0] = acc.a[4] = acc.a[8] = 1;
            for (int guard = 0; bi >= 0 && size_t(bi) < nb && guard < 64; ++guard)
            {
                const uint8_t* bone = bv[0] + size_t(bi) * kBoneSize;
                const char* bn; size_t bl;
                if (!StrAt(bone, bn, bl)) return false;
                const int parent = *reinterpret_cast<const int*>(bone + kBoneParent);
                Xform l{};
                const int part = anim ? PartIndexRaw(anim, bn, bl) : -1;
                if (part < 0 || !PartLocal(anim, part, t, l))
                {
                    Xform b{}, pb{};
                    if (!BindXform(model, int(nr) + bi, b) || !BindXform(model, parent >= 0 ? int(nr) + parent : 0, pb)) return false;
                    float pt[9]; Transpose(pb.a, pt);
                    Mul(b.a, pt, l.a);
                    const float d[3] = {b.o[0] - pb.o[0], b.o[1] - pb.o[1], b.o[2] - pb.o[2]};
                    VecMul(d, pt, l.o);
                }
                Xform n{};
                Mul(acc.a, l.a, n.a);
                VecMul(acc.o, l.a, n.o);
                for (int k = 0; k < 3; ++k) n.o[k] += l.o[k];
                acc = n;
                bi = parent;
            }
            Xform p{};
            memcpy(p.o, inst + kInstOrigin, 12); memcpy(p.a, inst + kInstAxis, 36);
            Mul(acc.a, p.a, out.a);
            VecMul(acc.o, p.a, out.o);
            for (int k = 0; k < 3; ++k) out.o[k] += p.o[k];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    Att* Find(uint8_t* entry);
    void Resolve(Att& a)
    {
        a.lastTry = GetTickCount64();
        if (!ModelReady(a.entry) || !ModelReady(a.parent)) return;
        std::string ptag = a.ptag, ctag = a.ctag;
        const bool pOk = ptag.empty() || HasTag(a.parent, ptag);
        const bool cOk = ctag.empty() || HasTag(a.entry, ctag);
        a.note.clear();
        if (!(pOk && cOk) && !ptag.empty() && !ctag.empty() && HasTag(a.parent, ctag) && HasTag(a.entry, ptag))
        {
            std::swap(ptag, ctag);
            a.swapped = true;
            a.note = "Align Parent Tag '" + a.ptag + "' is a tag of " + a.model + " and Align Child Tag '" + a.ctag
                   + "' one of the preview Model, so the preview uses them the other way round (Parent Tag: preview Model, Child Tag: Attach Model)";
        }
        else
        {
            if (!pOk)
            {
                a.note = "Align Parent Tag '" + a.ptag + "' is not a tag of the preview Model, " + a.model + " is shown at its origin";
                ptag = "tag_origin";
            }
            if (!cOk)
            {
                // a gun whose root bone has another name (j_gun in older ports) is held by that root, as in game
                const std::string root = RootName(ModelOf(a.entry));
                if (!root.empty() && root != "tag_origin")
                {
                    a.note += std::string(a.note.empty() ? "" : "; ") + "Align Child Tag '" + a.ctag + "' is not a tag of " + a.model + ", its root bone '" + root + "' is used";
                    a.animPart = ctag;
                    ctag = root;
                }
                else
                {
                    a.note += std::string(a.note.empty() ? "" : "; ") + "Align Child Tag '" + a.ctag + "' is not a tag of " + a.model + ", its origin is used";
                    ctag.clear();
                }
            }
        }
        // APE's tag lookup only knows non-root bones: a root of the preview Model is placed like its origin
        bool parentRoot = false;
        if (!ptag.empty() && ptag != "tag_origin" && BoneIndex(ModelOf(a.parent), ptag, &parentRoot) >= 0 && parentRoot) ptag = "tag_origin";
        a.usePtag = ptag;
        a.useCtag = ctag;
        bool childRoot = false;
        const int ci = ctag.empty() || ctag == "tag_origin" ? -1 : BoneIndex(ModelOf(a.entry), ctag, &childRoot);
        a.haveChild = ci >= 0 && BindXform(ModelOf(a.entry), ci, a.child);
        a.childIsRoot = a.haveChild && childRoot;
        a.onParentBone = Find(a.parent) != nullptr && !ptag.empty() && ptag != "tag_origin";
        strncpy_s(reinterpret_cast<char*>(a.entry + kEntryParentTag), 256, ptag.c_str(), _TRUNCATE);
        a.resolved = true;
        if (!a.note.empty()) Tell(a, a.note);

        void* anim = *reinterpret_cast<void**>(Inst(a.parent) + kInstAnim);
        if (!g_attachAnim || !anim) return;
        // the attached model's root follows the anim's bone of that name (a gun's tag_weapon in a viewmodel anim)
        // the anim's part of the child tag's name moves the attached model (the requested tag when the root stood in for it)
        if (a.animPart.empty() || PartIndex(anim, a.animPart) < 0) a.animPart = ctag;
        a.animDriven = a.childIsRoot && !ptag.empty() && FullOffsets(anim) && PartIndex(anim, a.animPart) >= 0;
        if (!*reinterpret_cast<void**>(Inst(a.entry) + kInstAnim) && SharesBones(Inst(a.entry), anim))
        {
            const std::string name = AnimName(anim);
            const MsvcStr s = Str(name.c_str());
            a.animated = !name.empty() && CallSetAnim(Holder(a.entry), &s);
            if (a.animated) SetEntryAnimName(a.entry, name.c_str(), name.size());
        }
    }

    Att* Find(uint8_t* entry)
    {
        for (Att& a : g_atts) if (a.entry == entry) return &a;
        return nullptr;
    }

    unsigned g_attachCalls = 0, g_attachRecorded = 0, g_freed = 0;
    char hk_Attach(uint8_t* entries, uint8_t* parent, void* model, void* ptag, void* ctag)
    {
        ++g_attachCalls;
        // APE's selection reset clears an entry's enabled byte and model but not its in-use byte, so 31 attaches use up every slot
        for (int i = 1; i < 32 && entries && g_on; ++i)
        {
            uint8_t* e = entries + i * kEntryStride;
            if (e[kEntryInUse] && !e[kEntryInUse - 1] && !*reinterpret_cast<void**>(e + kEntryHolder)) { e[kEntryInUse] = 0; ++g_freed; }
        }
        uint8_t* slot = nullptr;
        for (int i = 1; i < 32 && entries; ++i)
            if (!entries[i * kEntryStride + kEntryInUse]) { slot = entries + i * kEntryStride; break; }
        const char r = g_origAttach(entries, parent, model, ptag, ctag);
        if (!g_ready || !slot || !slot[kEntryInUse] || *reinterpret_cast<uint8_t**>(slot + kEntryParent) != parent) return r;
        ++g_attachRecorded;
        Att* a = Find(slot);
        if (!a) { g_atts.push_back(Att{}); a = &g_atts.back(); }
        *a = Att{};
        a->entry = slot; a->parent = parent;
        a->model = ReadStr(model); a->ptag = ReadStr(ptag); a->ctag = ReadStr(ctag);
        if (a->ptag.empty()) a->ptag = "tag_origin";   // APE's default, "clear" means no placement
        else if (a->ptag == "clear") a->ptag.clear();
        if (g_on) Resolve(*a);
        return r;
    }

    std::unordered_map<uint8_t*, Xform> g_free;   // entries placed by another feature (the weapon preview's shells)

    bool hk_Place(uint8_t* entry, void* a2, char a3)
    {
        auto fr = g_free.empty() ? g_free.end() : g_free.find(entry);
        if (fr != g_free.end())
        {
            g_origPlace(entry, a2, a3);
            if (!entry[kEntryInUse - 1] || !Inst(entry)) return false;
            *reinterpret_cast<uint32_t*>(entry + kEntryFlags) &= ~4u;
            g_setPlacement(Holder(entry), &fr->second, 1.0f);
            return true;
        }
        Att* a = g_on ? Find(entry) : nullptr;
        if (!a || *reinterpret_cast<uint8_t**>(entry + kEntryParent) != a->parent || !entry[kEntryInUse]) return g_origPlace(entry, a2, a3);
        if (!a->resolved && GetTickCount64() - a->lastTry >= 50) Resolve(*a);
        *reinterpret_cast<uint32_t*>(entry + kEntryFlags) &= ~4u;
        const bool r = g_origPlace(entry, a2, a3);
        *reinterpret_cast<uint32_t*>(entry + kEntryFlags) &= ~4u;
        if (!r || a->usePtag.empty() || !Inst(entry) || !Inst(a->parent)) return r;
        Xform t = *reinterpret_cast<const Xform*>(entry + kEntryTagXform);
        if (a->onParentBone && !AnimTagWorldRaw(a->parent, a->usePtag.data(), a->usePtag.size(), t)) return r;
        if (!a->onParentBone && !a->haveChild && !a->animDriven) return r;
        if (a->animDriven)
        {
            void* anim = *reinterpret_cast<void**>(Inst(a->parent) + kInstAnim);
            if (anim != a->partAnim) { a->partAnim = anim; a->partIndex = anim ? PartIndex(anim, a->animPart.empty() ? a->useCtag : a->animPart) : -1; }
            Xform l{};
            if (a->partIndex >= 0 && PartLocal(anim, a->partIndex, *reinterpret_cast<const float*>(Inst(a->parent) + kInstT), l))
            {
                Xform w{};
                Mul(l.a, t.a, w.a);
                VecMul(l.o, t.a, w.o);
                for (int i = 0; i < 3; ++i) w.o[i] += t.o[i];
                t = w;
            }
        }
        Xform p = t;
        if (a->haveChild)
        {
            float ct[9]; Transpose(a->child.a, ct);
            Mul(ct, t.a, p.a);
            float off[3]; VecMul(a->child.o, p.a, off);
            for (int i = 0; i < 3; ++i) p.o[i] = t.o[i] - off[i];
        }
        g_setPlacement(Holder(entry), &p, 1.0f);
        return r;
    }

    // ---- frame labels: APE draws "Frame: N" and the notes for every slot with an anim ----
    bool g_inFrame = false;
    void* g_widget = nullptr;                    // apePreviewWidget, from the frame function
    const uintptr_t kOpts = 0x14118D620;         // previewWindowOpts (pointer)
    const size_t kOptsTime = 0xB528, kOptsPaused = 0xB530, kSlot0 = 0xCA0;
    std::vector<std::pair<uint8_t*, void*>> g_hidden;

    int64_t hk_Render(void* a1, void* a2, void* a3)
    {
        const int64_t r = g_origRender(a1, a2, a3);
        if (!g_inFrame || !g_on) return r;
        for (Att& a : g_atts)
        {
            uint8_t* inst = a.animated && a.entry && a.entry[kEntryInUse] ? Inst(a.entry) : nullptr;
            void** anim = inst ? reinterpret_cast<void**>(inst + kInstAnim) : nullptr;
            if (!anim || !*anim) continue;
            g_hidden.emplace_back(inst, *anim);
            *anim = nullptr;
        }
        return r;
    }

    void* Ape(uintptr_t ida) { return rp::Ape() ? rp::Ape()->ApeAddress(rp::self, ida) : nullptr; }

    // an Attach Model without an Align Child Tag is attached too (its origin on the parent tag)
    uint8_t* g_jz = nullptr;
    bool WriteCode(uint8_t* at, const uint8_t* bytes, size_t n)
    {
        DWORD old = 0;
        if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
        memcpy(at, bytes, n);
        VirtualProtect(at, n, old, &old);
        FlushInstructionCache(GetCurrentProcess(), at, n);
        return true;
    }
    void PatchChildTagCheck(bool on)
    {
        static const uint8_t cmp[8] = {0x48, 0x83, 0xBD, 0x50, 0x01, 0x00, 0x00, 0x00};
        static const uint8_t jz[2] = {0x74, 0x37}, nop[2] = {0x66, 0x90};
        uint8_t* p = static_cast<uint8_t*>(Ape(kNeedChildTag));
        if (!p || memcmp(p, cmp, 8) != 0) return;
        if (on && !memcmp(p + 8, jz, 2) && WriteCode(p + 8, nop, 2)) g_jz = p + 8;
        else if (!on && g_jz == p + 8 && !memcmp(p + 8, nop, 2) && WriteCode(p + 8, jz, 2)) g_jz = nullptr;
    }
    bool StartsFunction(void* target)
    {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fn = target ? RtlLookupFunctionEntry(DWORD64(target), &base, nullptr) : nullptr;
        return fn && DWORD64(fn->BeginAddress) + base == DWORD64(target);
    }
}

void Install()
{
    g_on = rp::host->ConfigGetInt(rp::self, "xanimpreview", "enabled", 1) != 0;
    g_attachAnim = rp::host->ConfigGetInt(rp::self, "xanimpreview", "attach_anim", 1) != 0;
    void* convert = Ape(kConvert);
    void* pose = Ape(kLocalPose);
    void* attach = Ape(kAttach);
    void* place = Ape(kPlace);
    void* render = Ape(kRender);
    g_setPlacement = reinterpret_cast<SetPlacementFn>(Ape(kSetPlacement));
    g_setAnim = reinterpret_cast<SetAnimFn>(Ape(kSetAnim));
    g_gdtAsset = reinterpret_cast<GdtAssetFn>(Ape(kGdtAsset));
    g_assign = reinterpret_cast<AssignFn>(Ape(kAssign));
    if (!StartsFunction(reinterpret_cast<void*>(g_assign))) g_assign = nullptr;
    const bool found = StartsFunction(convert) && StartsFunction(pose) && StartsFunction(attach) && StartsFunction(place) && StartsFunction(render)
                    && StartsFunction(reinterpret_cast<void*>(g_setPlacement))
                    && StartsFunction(reinterpret_cast<void*>(g_setAnim)) && StartsFunction(reinterpret_cast<void*>(g_gdtAsset));
    if (found)
        g_ready = rp::host->Detour(rp::self, convert, reinterpret_cast<void*>(&hk_Convert), reinterpret_cast<void**>(&g_origConvert))
               && rp::host->Detour(rp::self, pose, reinterpret_cast<void*>(&hk_LocalPose), reinterpret_cast<void**>(&g_origPose))
               && rp::host->Detour(rp::self, attach, reinterpret_cast<void*>(&hk_Attach), reinterpret_cast<void**>(&g_origAttach))
               && rp::host->Detour(rp::self, place, reinterpret_cast<void*>(&hk_Place), reinterpret_cast<void**>(&g_origPlace))
               && rp::host->Detour(rp::self, render, reinterpret_cast<void*>(&hk_Render), reinterpret_cast<void**>(&g_origRender));
    if (g_ready && g_on) PatchChildTagCheck(true);
    rp::Log("xanimpreview: %s%s", !found ? "APE binary not recognised, off" : g_ready ? "detours ok" : "detour FAILED",
            g_ready && !g_on ? " (disabled in ini)" : "");
}

void Shutdown()
{
    PatchChildTagCheck(false);
    ClearLayers();
    ClearAds();
    g_free.clear();
}

void SetFree(void* entry, const float* xform12)
{
    uint8_t* e = static_cast<uint8_t*>(entry);
    if (!xform12) { g_free.erase(e); return; }
    Xform x; memcpy(&x, xform12, sizeof x);
    g_free[e] = x;
}
void ClearFree() { g_free.clear(); }

static size_t BoneListRaw(uint8_t* model, void* anim, char* out, size_t cap)
{
    size_t at = 0;
    __try
    {
        uint8_t* const* nv = reinterpret_cast<uint8_t* const*>(model + kModelNames);
        uint8_t* const* bv = reinterpret_cast<uint8_t* const*>(model + kModelBones);
        uint8_t* const* pv = reinterpret_cast<uint8_t* const*>(model + kModelBind);
        const size_t nn = size_t(nv[1] - nv[0]) / 32, nb = size_t(bv[1] - bv[0]) / kBoneSize, np = size_t(pv[1] - pv[0]) / kPoseSize;
        at += size_t(snprintf(out, cap, "%zu bones, %zu roots: ", nn, np - nb));
        for (size_t i = 0; i < nn && i < 60 && at + 80 < cap; ++i)
        {
            const char* p; size_t n;
            if (!StrAt(nv[0] + i * 32, p, n) || n > 60) continue;
            bool moved = false;
            if (anim)
            {
                uint8_t* const* av = reinterpret_cast<uint8_t* const*>(static_cast<uint8_t*>(anim) + kAnimParts);
                const size_t na = size_t(av[1] - av[0]) / kPartSize;
                for (size_t k = 0; k < na && !moved; ++k) moved = StrIs(av[0] + k * kPartSize, p, n);
            }
            at += size_t(snprintf(out + at, cap - at, "%.*s%s%s ", int(n), p, i < np - nb ? "(root)" : "", moved ? "*" : ""));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { at += size_t(snprintf(out + at, cap - at, "?")); }
    return at;
}

static size_t BoneParentsRaw(uint8_t* model, char* out, size_t cap)
{
    size_t at = 0;
    __try
    {
        uint8_t* const* bv = reinterpret_cast<uint8_t* const*>(model + kModelBones);
        const size_t nb = size_t(bv[1] - bv[0]) / kBoneSize;
        for (size_t i = 0; i < nb && i < 16 && at + 80 < cap; ++i)
        {
            const char* p; size_t n;
            if (!StrAt(bv[0] + i * kBoneSize, p, n) || n > 60) continue;
            const float* q = reinterpret_cast<const float*>(bv[0] + i * kBoneSize + 36);
            const float* o = reinterpret_cast<const float*>(bv[0] + i * kBoneSize + kBoneOffset);
            at += size_t(snprintf(out + at, cap - at, "%zu %.*s parent %d q %.3f %.3f %.3f %.3f o %.2f %.2f %.2f\n", i, int(n), p, *reinterpret_cast<const int*>(bv[0] + i * kBoneSize + kBoneParent),
                                  q[0], q[1], q[2], q[3], o[0], o[1], o[2]));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { at += size_t(snprintf(out + at, cap - at, "?")); }
    return at;
}

static bool HierarchyRaw(uint8_t* model, std::vector<std::pair<const char*, size_t>>& names, std::vector<int>& parents)
{
    __try
    {
        uint8_t* const* bv = reinterpret_cast<uint8_t* const*>(model + kModelBones);
        const size_t nb = bv[1] > bv[0] ? size_t(bv[1] - bv[0]) / kBoneSize : 0;
        for (size_t i = 0; i < nb && i < 1024; ++i)
        {
            const char* p; size_t n;
            if (!StrAt(bv[0] + i * kBoneSize, p, n) || n > 128) return false;
            names.push_back({p, n});
            parents.push_back(*reinterpret_cast<const int*>(bv[0] + i * kBoneSize + kBoneParent));
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool Hierarchy(void* entry, std::vector<std::pair<std::string, std::string>>& out)
{
    out.clear();
    uint8_t* model = entry ? ModelOf(static_cast<uint8_t*>(entry)) : nullptr;
    if (!model) return false;
    std::vector<std::pair<const char*, size_t>> names;
    std::vector<int> parents;
    if (!HierarchyRaw(model, names, parents)) return false;
    const std::string root = RootName(model);
    auto lower = [](std::string s) { for (char& c : s) c = char(tolower(static_cast<unsigned char>(c))); return s; };
    if (!root.empty()) out.push_back({lower(root), std::string()});
    for (size_t i = 0; i < names.size(); ++i)
    {
        const int p = parents[i];
        out.push_back({lower(std::string(names[i].first, names[i].second)),
                       p >= 0 && size_t(p) < names.size() ? lower(std::string(names[size_t(p)].first, names[size_t(p)].second)) : lower(root)});
    }
    return true;
}

std::string BoneParents(void* entry)
{
    uint8_t* model = entry ? ModelOf(static_cast<uint8_t*>(entry)) : nullptr;
    if (!model) return "no model";
    std::vector<char> buf(4096);
    BoneParentsRaw(model, buf.data(), buf.size());
    return buf.data();
}

std::string BoneList(void* entry, void* animOrNull)
{
    uint8_t* model = entry ? ModelOf(static_cast<uint8_t*>(entry)) : nullptr;
    if (!model) return "no model";
    std::vector<char> buf(4096);
    BoneListRaw(model, animOrNull, buf.data(), buf.size());
    return buf.data();
}

bool TagWorld(void* entry, const char* tag, float* out12)
{
    uint8_t* e = static_cast<uint8_t*>(entry);
    uint8_t* model = e ? ModelOf(e) : nullptr;
    uint8_t* inst = e ? Inst(e) : nullptr;
    if (!model || !inst) return false;
    const int idx = BoneIndex(model, tag, nullptr);
    Xform b{};
    if (idx < 0 || !BindXform(model, idx, b)) return false;
    Xform p{}, w{};
    if (!CopyBytes(p.o, inst + kInstOrigin, 12) || !CopyBytes(p.a, inst + kInstAxis, 36)) return false;
    Mul(b.a, p.a, w.a);
    VecMul(b.o, p.a, w.o);
    for (int i = 0; i < 3; ++i) w.o[i] += p.o[i];
    memcpy(out12, &w, sizeof w);
    return true;
}

bool ConvertSafe(int64_t* sp, const MsvcStr* name)
{
    __try { hk_Convert(sp, const_cast<MsvcStr*>(name), 0); return sp[0] != 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SetLayer(const std::string& top, const std::string& base)
{
    if (!g_ready || top.empty() || base.empty()) return false;
    auto it = g_layers.find(top);
    if (it != g_layers.end() && it->second.base == base && it->second.baseAnim) return true;
    if (it != g_layers.end()) { Release(it->second.baseCtrl); g_layers.erase(it); }
    int64_t sp[2] = {0, 0};
    const MsvcStr n = Str(base.c_str());
    if (!ConvertSafe(sp, &n)) return false;
    Layer& L = g_layers[top];
    L.base = base; L.baseAnim = reinterpret_cast<void*>(sp[0]); L.baseCtrl = sp[1];
    return true;
}
bool SetAds(const std::string& up, float upWeight, float upTime, const AdsTracks* upTracks,
            const std::string& down, float downWeight, float downTime, const AdsTracks* downTracks)
{
    if (!g_ready) return false;
    const std::string* names[2] = {&up, &down};
    const float weights[2] = {upWeight, downWeight}, times[2] = {upTime, downTime};
    const AdsTracks* tracks[2] = {upTracks, downTracks};
    bool any = false;
    for (int k = 0; k < 2; ++k)
    {
        AdsNode& N = g_ads.node[k];
        if (N.name != *names[k] || !N.anim)
        {
            if (N.ctrl) Release(N.ctrl);
            N = AdsNode{};
            if (!names[k]->empty())
            {
                int64_t sp[2] = {0, 0};
                const MsvcStr n = Str(names[k]->c_str());
                if (ConvertSafe(sp, &n) && sp[0]) { N.name = *names[k]; N.anim = reinterpret_cast<void*>(sp[0]); N.ctrl = sp[1]; }
                else Release(sp[1]);
            }
        }
        if (tracks[k] != nullptr) { if (!N.hasTracks) { N.tracks = *tracks[k]; N.hasTracks = true; } }
        else if (N.hasTracks) { N.tracks.clear(); N.hasTracks = false; }
        N.weight = std::clamp(weights[k], 0.0f, 1.0f);
        N.time = std::clamp(times[k], 0.0f, 1.0f);
        any = any || N.anim;
    }
    return any;
}
void ClearAds()
{
    for (AdsNode& N : g_ads.node) { if (N.ctrl) Release(N.ctrl); N = AdsNode{}; }
    g_ads.built = Overlaid{};
    g_ads.note.clear();
}
void ClearLayers()
{
    for (auto& [k, L] : g_layers) Release(L.baseCtrl);
    g_layers.clear();
}

void BeforeFrame(void* widget) { g_widget = widget; g_inFrame = true; }

void AfterFrame()
{
    for (auto& h : g_hidden) *reinterpret_cast<void**>(h.first + kInstAnim) = h.second;
    g_hidden.clear();
    g_inFrame = false;
}

namespace
{
    // posedump reads APE's own anim structs, and an anim pointer not kept alive by a control block dies
    // when the preview reloads, so the reads run under SEH and an impossible part count counts as dead.
    bool DumpAnim(const char* label, const void* anim, bool last, std::string& reply)
    {
        const uint8_t* a = static_cast<const uint8_t*>(anim);
        uint8_t* const* pp = reinterpret_cast<uint8_t* const*>(a + kAnimParts);
        const size_t span = size_t(pp[1] - pp[0]);
        const size_t n = span % kPartSize ? SIZE_MAX : span / kPartSize;
        if (n > 1024) return false;
        char b[300];
        snprintf(b, sizeof b, "%s '%s' type %d frames %d parts %zu\n", label, AnimName(const_cast<void*>(anim)).c_str(), *reinterpret_cast<const int*>(a + kAnimType), *reinterpret_cast<const int*>(a + kAnimFrames), n);
        reply += b;
        for (size_t k = 0; k < n; ++k)
        {
            const uint8_t* part = pp[0] + k * kPartSize;
            size_t pn; const char* nm = NameAt(part, pn);
            const std::string name(nm, pn);
            if (!last && name.rfind("tag_", 0) != 0) continue;
            const Track t = TrackOf(part);
            for (int end = last ? 1 : 0; end < 2; ++end)
            {
                const float* q = t.nq ? t.q + (end ? t.nq - 1 : 0) * 4 : nullptr;
                const float* v = t.nv ? t.v + (end ? t.nv - 1 : 0) * 3 : nullptr;
                const float* w = t.nw ? t.w + (end ? t.nw - 1 : 0) * 4 : nullptr;
                snprintf(b, sizeof b, "  %s %s nq %zu nv %zu nw %zu q %.3f %.3f %.3f %.3f v %.2f %.2f %.2f w %.3f %.3f %.3f %.3f\n", name.c_str(), end ? "last" : "first", t.nq, t.nv, t.nw,
                         q ? q[0] : 0, q ? q[1] : 0, q ? q[2] : 0, q ? q[3] : 0, v ? v[0] : 0, v ? v[1] : 0, v ? v[2] : 0, w ? w[0] : 0, w ? w[1] : 0, w ? w[2] : 0, w ? w[3] : 0);
                reply += b;
            }
        }
        return true;
    }
    bool PartNames(const void* anim, std::string& reply)
    {
        uint8_t* const* pp = reinterpret_cast<uint8_t* const*>(static_cast<const uint8_t*>(anim) + kAnimParts);
        const size_t n = size_t(pp[1] - pp[0]) / kPartSize;
        if (n > 1024) return false;
        for (size_t k = 0; k < n; ++k)
        {
            const uint8_t* part = pp[0] + k * kPartSize;
            size_t pn; const char* nm = NameAt(part, pn);
            const Track t = TrackOf(part);
            bool rq = true, rv = true;
            for (size_t i = 1; i < t.nq && rq; ++i) for (int c = 0; c < 4; ++c) if (std::fabs(t.q[i * 4 + c] - t.q[c]) > 1e-5f) rq = false;
            for (size_t i = 1; i < t.nv && rv; ++i) for (int c = 0; c < 3; ++c) if (std::fabs(t.v[i * 3 + c] - t.v[c]) > 1e-4f) rv = false;
            char b[200];
            snprintf(b, sizeof b, "%s %s%s q %.3f %.3f %.3f %.3f v %.2f %.2f %.2f\n", std::string(nm, pn).c_str(), rq ? "Cq" : "-", rv ? "Cv" : "-",
                     t.nq ? t.q[0] : 0, t.nq ? t.q[1] : 0, t.nq ? t.q[2] : 0, t.nq ? t.q[3] : 0, t.nv ? t.v[0] : 0, t.nv ? t.v[1] : 0, t.nv ? t.v[2] : 0);
            reply += b;
        }
        return true;
    }
    bool PartNamesSafe(const void* anim, std::string& reply)
    {
        __try { return PartNames(anim, reply); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool DumpAnimSafe(const char* label, const void* anim, bool last, std::string& reply)
    {
        __try { return DumpAnim(label, anim, last, reply); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}

bool Command(const std::string& command, std::string& reply)
{
    if (command.compare(0, 12, "xanimpreview") != 0) return false;
    const std::string arg = command.size() > 13 ? command.substr(13) : std::string();
    if (!g_ready) { reply = "xanimpreview unavailable"; return true; }
    if (arg.compare(0, 10, "partnames ") == 0)   // test harness: every part of an anim, C when its rotation and translation never change
    {
        reply.clear();
        int64_t sp[2] = {0, 0};
        const MsvcStr n = Str(arg.c_str() + 10);
        if (!ConvertSafe(sp, &n) || !sp[0]) { Release(sp[1]); reply = "no anim " + arg.substr(10); return true; }
        if (!PartNamesSafe(reinterpret_cast<void*>(sp[0]), reply)) reply += "partnames: read failed\n";
        Release(sp[1]);
        return true;
    }
    if (arg.compare(0, 9, "posedump ") == 0)   // test harness: any anim's tag_ tracks at the first and last key
    {
        reply.clear();
        int64_t sp[2] = {0, 0};
        const MsvcStr n = Str(arg.c_str() + 9);
        if (!ConvertSafe(sp, &n) || !sp[0]) { Release(sp[1]); reply = "no anim " + arg.substr(9); return true; }
        if (!DumpAnimSafe("anim", reinterpret_cast<void*>(sp[0]), false, reply)) reply += "posedump: read failed\n";
        Release(sp[1]);
        return true;
    }
    if (arg == "posedump")   // test harness: the ADS anims' tracks at their last key
    {
        reply.clear();
        if (!g_ads.node[0].anim && !g_ads.node[1].anim) { reply = "no ADS anims set"; return true; }
        for (const AdsNode& N : g_ads.node)
            if (N.anim && !DumpAnimSafe("ads", N.anim, true, reply)) reply += "posedump: the ADS anim is gone (stale pointer)\n";
        return true;
    }
    if (arg == "on" || arg == "off")
    {
        g_on = arg == "on";
        PatchChildTagCheck(g_on);
        rp::ConfigSet("xanimpreview", "enabled", g_on ? "1" : "0");
        reply = std::string("xanimpreview ") + arg + " (select the xanim again to reload the preview)";
        return true;
    }
    if (arg.compare(0, 6, "frame ") == 0)   // test harness: a paused preview to frame n
    {
        uint8_t** opts = static_cast<uint8_t**>(Ape(kOpts));
        uint8_t* o = opts ? *opts : nullptr;
        uint8_t* inst = o ? *reinterpret_cast<uint8_t**>(o + kSlot0) : nullptr;
        uint8_t* anim = inst ? *reinterpret_cast<uint8_t**>(inst + kInstAnim) : nullptr;
        HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll");
        auto update = w ? reinterpret_cast<void (*)(void*)>(GetProcAddress(w, "?update@QWidget@@QEAAXXZ")) : nullptr;
        if (!anim || !g_widget || !update) { reply = "no preview anim"; return true; }
        if (!o[kOptsPaused]) { reply = "the preview is playing (pause it first)"; return true; }
        const float fps = *reinterpret_cast<float*>(anim + 72);
        *reinterpret_cast<double*>(o + kOptsTime) = atof(arg.c_str() + 6) / (fps > 0 ? fps : 30.0f);
        update(g_widget);
        reply = "preview at frame " + arg.substr(6);
        return true;
    }
    char line[256];
    uint8_t** opts = static_cast<uint8_t**>(Ape(kOpts));
    const double time = opts && *opts ? *reinterpret_cast<double*>(*opts + kOptsTime) : -1.0;
    snprintf(line, sizeof line, "xanimpreview %s, attach_anim %d, empty child tag attaches %d; %u anims with full offsets seen, %u pose calls rebuilt; time %.3f s%s\n",
             g_on ? "on" : "off", g_attachAnim ? 1 : 0, g_jz ? 1 : 0, g_fullCount, g_poseCalls, time, opts && *opts && (*opts)[kOptsPaused] ? " (paused)" : "");
    reply = line;
    int inUse = 0, enabled = 0, withObj = 0;
    if (opts && *opts)
        for (int i = 0; i < 32; ++i)
        {
            const uint8_t* e = *opts + 2440 + size_t(i) * kEntryStride;
            inUse += e[kEntryInUse] != 0; enabled += e[kEntryInUse - 1] != 0; withObj += *reinterpret_cast<void* const*>(e + kEntryHolder) != nullptr;
        }
    reply += "entries in use " + std::to_string(inUse) + ", enabled " + std::to_string(enabled) + ", with a model " + std::to_string(withObj) + "\n";
    for (const auto& [k, L] : g_layers)
        reply += "layer " + k + " over " + L.base + ": " + std::to_string(L.parts.size() / kPartSize) + " parts, " + std::to_string(std::count_if(L.map.begin(), L.map.end(), [](int v) { return v >= 0; })) + " bones moved\n";
    {
        char b[300]; snprintf(b, sizeof b, "ads up '%s' weight %.2f time %.2f, down '%s' weight %.2f time %.2f, %u pose calls, %u blended\n",
                              g_ads.node[0].name.c_str(), g_ads.node[0].weight, g_ads.node[0].time, g_ads.node[1].name.c_str(), g_ads.node[1].weight, g_ads.node[1].time, g_ads.calls, g_ads.hits);
        reply += b;
        if (!g_ads.note.empty()) reply += "ads bones: " + g_ads.note + "\n";
    }
    reply += "layered pose calls " + std::to_string(g_layerCalls) + ", anims seen:";
    for (const std::string& n : g_seen) reply += " '" + n + "'";
    reply += "\n";
    reply += "attach calls " + std::to_string(g_attachCalls) + ", recorded " + std::to_string(g_attachRecorded) + ", stale slots freed " + std::to_string(g_freed) + ", entries " + std::to_string(g_atts.size()) + "\n";
    for (const Att& a : g_atts)
    {
        if (!a.entry || !a.entry[kEntryInUse]) continue;
        reply += "attach " + a.model + ": parent tag '" + a.ptag + "' child tag '" + a.ctag + "' -> uses '" + a.usePtag + "'"
               + (a.haveChild ? ", child offset" : "") + (a.childIsRoot ? " (root)" : "") + (a.animDriven ? ", follows the anim's '" + a.useCtag + "'" : "")
               + (a.swapped ? ", swapped" : "") + (a.animated ? ", animated" : "")
               + (a.resolved ? "" : ", waiting for models") + (a.note.empty() ? "" : "\n  " + a.note) + "\n";
    }
    return true;
}
}
