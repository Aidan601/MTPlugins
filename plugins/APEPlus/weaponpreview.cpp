// Weapon preview, see weaponpreview.h. APE previews nothing for a weapon: the scene is built through APE's own xanim
// preview (preview Model, Attach Model and tags answered for the weapon), then anims are swapped with SetAnim.
#include "modtools_plugin.hpp"
#include "weaponpreview.h"
#include "notesounds.h"
#include "fxplayer.h"
#include "previewfix.h"
#include "xanimbin.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace ba::weaponpreview
{
namespace
{
    // IDA addresses (base 0x140000000), rebased through ApeAddress
    const uintptr_t kDispatch = 0x140131EA0;   // (apePreviewWidget*, asset record*): picks the preview by the asset's type
    const uintptr_t kRawGet = 0x14012D3F0;     // apeGfxGdtEntry vtable[1]: (entry, std::string* out, const std::string* key)
    const uintptr_t kAssign = 0x140050060;     // std::string::assign(const char*, size_t) with APE's allocator
    const uintptr_t kIntern = 0x14005FBC0;     // (uint64_t* out, const char*): the interned name handle
    const uintptr_t kTypeIndex = 0x14007CBC0;  // (asset db, const uint64_t* type handle) -> type index, -1 unknown
    const uintptr_t kField = 0x140062190;      // (record, const char** out, const char* key): the value, "" when unset
    const uintptr_t kLookup = 0x14007B690;     // (asset db, const uint64_t* name handle, int type, char 0) -> record or null
    const uintptr_t kDb = 0x14118D298;         // the asset db (pointer)
    const uintptr_t kOpts = 0x14118D620;       // previewWindowOpts (pointer)
    const uintptr_t kLength = 0x1406328D0;     // (entries) -> float: the longest anim of the preview entries in seconds
    const uintptr_t kSetAnim = 0x14047EA90;    // (holder, const std::string& anim name, int mode)
    const uintptr_t kViewOf = 0x140338A60;     // (camera, view out, float fov): origin, axes, +48 tan of the half height, +52 of the half width
    const uintptr_t kClockCall = 0x14012A197;  // frame function: call [clock], the msvcr110 import the play time runs on
    const uintptr_t kAttach = 0x140632170;     // (entries, parent entry, model, parent tag, child tag): attach a model to a preview entry

    const size_t kRecName = 16, kRecType = 32;        // asset record: char* name, int type index
    const size_t kEntryRecord = 8;                    // apeGfxGdtEntry: the record
    const size_t kOptsCamOrigin = 24, kOptsCamAngles = 36;   // camera at opts +16: origin +8, angles (pitch down negative, yaw, roll) +20
    const size_t kOptsTime = 0xB528, kOptsPaused = 0xB530, kOptsClock = 0xB534;
    const size_t kEntries = 2440, kEntryStride = 1336, kEntryEnabled = 776, kEntryInUse = 777, kEntryHolder = 792;
    const size_t kInstAnim = 104;
    const size_t kEntryAnimNames = 1080, kEntryAnimIndex = 1304;   // std::string[], int: the entry's anim names, the play time's length comes from these

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
    std::string ReadStr(const void* s)
    {
        const uint8_t* p = static_cast<const uint8_t*>(s);
        const size_t size = *reinterpret_cast<const size_t*>(p + 16), cap = *reinterpret_cast<const size_t*>(p + 24);
        const char* c = cap >= 16 ? *reinterpret_cast<const char* const*>(p) : reinterpret_cast<const char*>(p);
        return size < 4096 ? std::string(c, size) : std::string();
    }

    using DispatchFn = void (*)(void* widget, uint8_t* record);
    using RawGetFn = void* (*)(uint8_t* entry, void* out, const void* key);
    using AssignFn = void* (*)(void* str, const char* s, size_t n);
    using InternFn = uint64_t* (*)(uint64_t* out, const char* s);
    using TypeIndexFn = int (*)(void* db, const uint64_t* type);
    using FieldFn = const char** (*)(uint8_t* record, const char** out, const char* key);
    using LookupFn = uint8_t* (*)(void* db, const uint64_t* name, int type, char flag);
    using LengthFn = float (*)(uint8_t* entries);
    using SetAnimFn = void* (*)(void* holder, const MsvcStr* name, unsigned mode);
    using ClockFn = long (*)();
    using ViewOfFn = void* (*)(int* cam, float* out, float fov);
    using AttachFn = char (*)(uint8_t* entries, uint8_t* parent, const void* model, const void* ptag, const void* ctag);

    DispatchFn g_origDispatch = nullptr;
    RawGetFn g_origRawGet = nullptr;
    AssignFn g_assign = nullptr;
    InternFn g_intern = nullptr;
    TypeIndexFn g_typeIndex = nullptr;
    FieldFn g_field = nullptr;
    LookupFn g_lookup = nullptr;
    LengthFn g_length = nullptr;
    SetAnimFn g_setAnim = nullptr;
    ClockFn g_clock = nullptr;
    ViewOfFn g_origViewOf = nullptr;
    AttachFn g_attach = nullptr;
    bool g_fxOn = true;                  // [weaponpreview] fx
    unsigned long long (*g_winId)(void* widget) = nullptr;   // QWidget::effectiveWinId
    float g_fov1p = 80.0f;               // [weaponpreview] fov_1p
    void** g_db = nullptr;
    uint8_t** g_opts = nullptr;
    bool g_ready = false;
    bool g_on = true;                    // [weaponpreview] enabled

    void* Ape(uintptr_t ida) { return rp::Ape() ? rp::Ape()->ApeAddress(rp::self, ida) : nullptr; }
    void Say(const std::string& text)
    {
        rp::Log("weaponpreview: %s", text.c_str());
        if (rp::Ape()) rp::Ape()->Print(rp::self, ("Weapon preview: " + text).c_str());
    }

    // ---- the asset db (SEH: APE's memory) ----
    bool InternSafe(const char* s, uint64_t& h)
    {
        __try { h = 0; g_intern(&h, s); return h != 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    int TypeIndex(const char* type)
    {
        uint64_t h = 0;
        if (!InternSafe(type, h)) return -1;
        __try { return g_typeIndex(*g_db, &h); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }
    uint8_t* LookupSafe(const uint64_t* h, int type)
    {
        __try { return g_lookup(*g_db, h, type, 0); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    uint8_t* Find(const std::string& name, int type)
    {
        uint64_t h = 0;
        if (name.empty() || type < 0 || !InternSafe(name.c_str(), h)) return nullptr;
        return LookupSafe(&h, type);
    }
    const char* FieldSafe(uint8_t* rec, const char* key)
    {
        __try { const char* v = nullptr; g_field(rec, &v, key); return v; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    std::string Field(uint8_t* rec, const char* key)
    {
        const char* v = rec ? FieldSafe(rec, key) : nullptr;
        return v ? std::string(v) : std::string();
    }
    int RecType(uint8_t* rec)
    {
        __try { return *reinterpret_cast<int*>(rec + kRecType); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }
    const char* RecNamePtr(uint8_t* rec)
    {
        __try { return *reinterpret_cast<const char**>(rec + kRecName); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    std::string RecName(uint8_t* rec) { const char* n = RecNamePtr(rec); return n ? std::string(n) : std::string(); }

    std::string Lower(std::string s) { for (char& c : s) c = char(tolower(static_cast<unsigned char>(c))); return s; }

    // ---- settings ----
    struct Settings
    {
        std::string arms = "c_t7_mp_warrior_viewarms1";
        std::string body = "c_t7_mp_warrior_fb1";
        std::string tpIdle = "pb_rifle_endgame_3rdplace_idle", tpFire, tpReload;
    } g_set;

    // ---- the selected weapon ----
    const char* const kWeaponTypes[] = {"bulletweapon", "projectileweapon", "dualwieldweapon", "dualwieldprojectileweapon"};
    int g_types[4] = {-2, -2, -2, -2};
    unsigned g_dispatches = 0;
    int g_lastType = -9;
    std::string g_dispLog;
    int g_xanimType = -2;
    uint8_t* g_weapon = nullptr;         // the weapon record, null when the selection is no weapon
    std::string g_weaponName, g_weaponType;
    void* g_widget = nullptr;            // apePreviewWidget, from the dispatch
    bool g_firstPerson = true;
    bool g_ads = false, g_holdFire = false;
    float g_adsFrac = 0;                 // the aim fraction, 0 hip to 1 aimed
    float g_adsUpWeight = 0;             // ADS Up's weight, ADS Down has the rest
    ULONGLONG g_adsTick = 0;
    bool g_adsSet = false;

    // ---- the scene: the xanim record whose fields are answered for the weapon ----
    uint8_t* g_overRecord = nullptr;
    std::vector<std::pair<std::string, std::string>> g_over;
    bool g_inBuild = false;
    unsigned g_overHits = 0;
    std::string g_sceneAnim;             // the anim on the preview Model now
    bool g_sceneFirstPerson = true;
    uint8_t* g_camWeapon = nullptr;      // the weapon and view the camera was last placed for
    bool g_camFirstPerson = true;
    std::string g_status;
    std::string g_trace;                 // "weaponpreview trace": the last switches and step ends
    float LengthSafe(uint8_t* e);

    uint8_t* EntryRecord(uint8_t* entry)
    {
        __try { return *reinterpret_cast<uint8_t**>(entry + kEntryRecord); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    void* hk_RawGet(uint8_t* entry, void* out, const void* key)
    {
        if (g_overRecord && entry && out && key && EntryRecord(entry) == g_overRecord)
        {
            const std::string k = ReadStr(key);
            for (const auto& o : g_over)
                if (!_stricmp(o.first.c_str(), k.c_str()))
                {
                    uint8_t* s = static_cast<uint8_t*>(out);
                    memset(s, 0, 32);
                    *reinterpret_cast<size_t*>(s + 24) = 15;
                    g_assign(out, o.second.c_str(), o.second.size());
                    ++g_overHits;
                    return out;
                }
        }
        return g_origRawGet(entry, out, key);
    }

    uint8_t* Entries() { return g_opts && *g_opts ? *g_opts + kEntries : nullptr; }

    void SetCamera(bool firstPerson)
    {
        uint8_t* o = g_opts ? *g_opts : nullptr;
        if (!o) return;
        float* org = reinterpret_cast<float*>(o + kOptsCamOrigin);
        float* ang = reinterpret_cast<float*>(o + kOptsCamAngles);
        if (firstPerson) { org[0] = 0; org[1] = 0; org[2] = 0; ang[0] = 0; ang[1] = 0; ang[2] = 0; }
        else { org[0] = 110; org[1] = 40; org[2] = 62; ang[0] = -10.5f; ang[1] = 200.0f; ang[2] = 0; }
    }

    void ResetTime()
    {
        uint8_t* o = g_opts ? *g_opts : nullptr;
        if (!o) return;
        *reinterpret_cast<double*>(o + kOptsTime) = 0.0;
        if (g_clock) *reinterpret_cast<int*>(o + kOptsClock) = int(g_clock());
    }

    void DispatchSafe(void* widget, uint8_t* rec)
    {
        __try { g_origDispatch(widget, rec); }
        __except (EXCEPTION_EXECUTE_HANDLER) { rp::Log("weaponpreview: exception in APE's preview dispatch"); }
    }

    // builds the scene through APE's xanim preview: the anim on the arms (body), the gun on tag_weapon_right
    void DropShells();
    void AttachParts(bool fp);
    std::string W(const char* key);
    bool Build(const std::string& anim)
    {
        if (!g_weapon || !g_widget) return false;
        uint8_t* xr = Find(anim, g_xanimType);
        if (!xr) { g_status = "anim '" + anim + "' is not in gdt.db"; Say(g_status); return false; }
        const bool fp = g_firstPerson;
        const std::string gun = W(fp ? "gunModel" : "worldModel");
        g_over = {{"previewModel", fp ? g_set.arms : g_set.body},
                  {"previewAttachModel", gun},
                  {"previewAlignParentTag", "tag_weapon_right"},
                  {"previewAlignChildTag", fp ? "tag_weapon" : "tag_origin"}};
        g_overRecord = xr;
        DropShells();
        ba::previewfix::ClearLayers();
        g_inBuild = true;
        DispatchSafe(g_widget, xr);
        g_inBuild = false;
        if (!gun.empty()) AttachParts(fp);
        if (g_camWeapon != g_weapon || g_camFirstPerson != fp) { SetCamera(fp); g_camWeapon = g_weapon; g_camFirstPerson = fp; }
        g_sceneAnim = anim;
        g_sceneFirstPerson = fp;
        ResetTime();
        if (gun.empty()) Say(std::string(fp ? "View Gun (gunModel)" : "World Gun (worldModel)") + " is empty, the preview shows no gun");
        return true;
    }

    bool SetAnimSafe(void* holder, const MsvcStr* s)
    {
        __try { g_setAnim(holder, s, 0); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // the entry's current anim name, which APE's anim length reads
    bool SetEntryName(uint8_t* entry, const char* name, size_t n)
    {
        __try
        {
            const int idx = *reinterpret_cast<int*>(entry + kEntryAnimIndex);
            if (idx < 0 || idx > 5) return false;
            uint8_t* str = entry + kEntryAnimNames + size_t(idx) * 32;
            g_assign(str, name, n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // the anim on the preview Model and on every attached model that plays one (the view gun)
    bool SwitchAnim(const std::string& anim)
    {
        if (anim == g_sceneAnim && g_sceneFirstPerson == g_firstPerson) { ResetTime(); return true; }
        uint8_t* e = Entries();
        if (!e || g_sceneAnim.empty() || g_sceneFirstPerson != g_firstPerson || !Find(anim, g_xanimType)) return Build(anim);
        const MsvcStr s = Str(anim.c_str());
        bool ok = true;
        for (int i = 0; i < 32 && ok; ++i)
        {
            uint8_t* entry = e + size_t(i) * kEntryStride;
            uint8_t* inst = entry[kEntryInUse] || i == 0 ? *reinterpret_cast<uint8_t**>(entry + kEntryHolder) : nullptr;
            if (!inst || !entry[kEntryEnabled]) continue;
            if (i > 0 && !*reinterpret_cast<void**>(inst + kInstAnim)) continue;
            ok = SetAnimSafe(entry + kEntryHolder, &s) && SetEntryName(entry, anim.c_str(), anim.size());
        }
        if (!ok) { g_trace += "switch " + anim + ": SetAnim failed, rebuilt; "; return Build(anim); }
        g_sceneAnim = anim;
        ResetTime();
        char b[160]; snprintf(b, sizeof b, "switch %s: length %.3f; ", anim.c_str(), double(LengthSafe(e)));
        g_trace += b;
        return true;
    }

    // ---- fx: materials for the player, the flash on the gun ----
    int g_materialType = -2, g_imageType = -2;
    bool ResolveMaterial(const std::string& material, std::string& image, std::string& type, int& cols, int& rows)
    {
        if (g_materialType == -2) g_materialType = TypeIndex("material");
        if (g_imageType == -2) g_imageType = TypeIndex("image");
        uint8_t* m = Find(material, g_materialType);
        if (!m) return false;
        type = Field(m, "materialType");
        cols = atoi(Field(m, "textureAtlasColumnCount").c_str());
        rows = atoi(Field(m, "textureAtlasRowCount").c_str());
        uint8_t* img = Find(Field(m, "colorMap"), g_imageType);
        image = img ? Field(img, "baseImage") : std::string();
        return !image.empty();
    }

    bool IsShell(uint8_t* entry);
    uint8_t* GunEntry()
    {
        uint8_t* e = Entries();
        if (!e) return nullptr;
        for (int i = 1; i < 32; ++i)
        {
            uint8_t* entry = e + size_t(i) * kEntryStride;
            if (entry[kEntryInUse] && entry[kEntryEnabled] && *reinterpret_cast<void**>(entry + kEntryHolder) && *reinterpret_cast<uint8_t**>(entry + 1312) == e && !IsShell(entry)) return entry;
        }
        return nullptr;
    }


    std::string g_flashTag, g_brassTag;      // what the last anchors came from (status)

    // tag_flash of the gun plus the weapon's flash offset (forward, right, up)
    bool FlashAnchor(ba::fxplayer::Xform& x)
    {
        uint8_t* gun = GunEntry();
        if (!gun) { g_flashTag = "no gun entry"; return false; }
        g_flashTag = "tag_flash";
        if (!ba::previewfix::TagWorld(gun, "tag_flash", x.o))
        {
            g_flashTag = "tag_origin (no tag_flash)";
            if (!ba::previewfix::TagWorld(gun, "tag_origin", x.o) && !ba::previewfix::TagWorld(gun, "tag_weapon", x.o)) { g_flashTag = "none"; return false; }
        }
        const char* p = g_sceneFirstPerson ? "view" : "world";
        const float f = float(atof(W((std::string(p) + "FlashOffsetF").c_str()).c_str()));
        const float r = float(atof(W((std::string(p) + "FlashOffsetR").c_str()).c_str()));
        const float u = float(atof(W((std::string(p) + "FlashOffsetU").c_str()).c_str()));
        for (int j = 0; j < 3; ++j) x.o[j] += f * x.a[j] - r * x.a[3 + j] + u * x.a[6 + j];
        return true;
    }

    void Flash()
    {
        if (!g_fxOn || !g_weapon) return;
        const std::string path = W(g_sceneFirstPerson ? "viewFlashEffect" : "worldFlashEffect");
        if (path.empty()) return;
        ba::fxplayer::Xform x{};
        if (FlashAnchor(x)) ba::fxplayer::SetAnchor(0, x);
        ba::fxplayer::Play(path, 0);
    }

    // tag_brass of the gun, moved by the weapon's shell eject offset (forward, right, up) and turned by its rotation
    bool BrassAnchor(ba::fxplayer::Xform& x)
    {
        uint8_t* gun = GunEntry();
        if (!gun) return false;
        g_brassTag = "tag_brass";
        if (!ba::previewfix::TagWorld(gun, "tag_brass", x.o)) { g_brassTag = "none (no tag_brass)"; return false; }
        const std::string p = g_sceneFirstPerson ? "viewShellEject" : "worldShellEject";
        auto num = [&](const char* k) { return float(atof(W((p + k).c_str()).c_str())); };
        const float f = num("OffsetF"), r = num("OffsetR"), u = num("OffsetU");
        for (int j = 0; j < 3; ++j) x.o[j] += f * x.a[j] - r * x.a[3 + j] + u * x.a[6 + j];
        const float pitch = num("RotationP"), yaw = num("RotationY"), roll = num("RotationR");
        if (pitch || yaw || roll)
        {
            const float d = 0.017453292f;
            const float sp = sinf(pitch * d), cp = cosf(pitch * d), sy = sinf(yaw * d), cy = cosf(yaw * d), sr = sinf(roll * d), cr = cosf(roll * d);
            const float m[9] = {cp * cy, cp * sy, -sp, sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, sr * cp, cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp};
            float a[9];
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) a[i * 3 + j] = m[i * 3] * x.a[j] + m[i * 3 + 1] * x.a[3 + j] + m[i * 3 + 2] * x.a[6 + j];
            memcpy(x.a, a, sizeof a);
        }
        return true;
    }

    // ---- shell models: preview entries APE draws, placed by previewfix where the fx player's model particles are ----
    struct Shell { uint8_t* entry = nullptr; std::string model; bool used = false; };
    std::vector<Shell> g_shells;
    const int kShellsPerModel = 8;

    bool IsShell(uint8_t* entry) { for (const Shell& sh : g_shells) if (sh.entry == entry) return true; return false; }

    // APE's reset frees only enabled entries, so a hidden shell is enabled again before any reset. APE itself never
    // leaves a model on a disabled entry, so every such entry is one of ours (also from an earlier load of the plugin).
    void DropShells()
    {
        uint8_t* e = Entries();
        for (int i = 1; e && i < 32; ++i)
        {
            uint8_t* entry = e + size_t(i) * kEntryStride;
            if (entry[kEntryInUse] && !entry[kEntryEnabled] && *reinterpret_cast<void**>(entry + kEntryHolder)) entry[kEntryEnabled] = 1;
        }
        g_shells.clear();
        ba::previewfix::ClearFree();
    }

    char AttachSafe(uint8_t* e, const MsvcStr* m, const MsvcStr* pt, const MsvcStr* ct)
    {
        __try { return g_attach(e, e, m, pt, ct); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    char AttachToSafe(uint8_t* e, uint8_t* parent, const MsvcStr* m, const MsvcStr* pt, const MsvcStr* ct)
    {
        __try { return g_attach(e, parent, m, pt, ct); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    // the weapon's attachment slots (Optic 1, Optic 2, Ironsights, Clip) on their gun tags, as the game draws them on the gun
    void AttachParts(bool fp)
    {
        uint8_t* e = Entries();
        uint8_t* gun = GunEntry();
        if (!e || !gun || !g_attach) return;
        const std::string base = fp ? "attachViewModel" : "attachWorldModel";
        std::string offsets;
        for (int n : {1, 3, 4, 5})
        {
            const std::string i = std::to_string(n);
            const std::string model = W((base + i).c_str());
            if (model.empty()) continue;
            const std::string tag = W((base + "Tag" + i).c_str());
            const MsvcStr m = Str(model.c_str()), pt = Str(tag.empty() ? "tag_origin" : tag.c_str()), ct = Str("");
            AttachToSafe(e, gun, &m, &pt, &ct);
            for (const char* k : {"OffsetX", "OffsetY", "OffsetZ", "OffsetPitch", "OffsetYaw", "OffsetRoll"})
                if (atof(W((base + k + i).c_str()).c_str()) != 0) { offsets += (offsets.empty() ? "" : ", ") + model; break; }
        }
        if (!offsets.empty()) Say("attachment offsets are not applied in the preview (" + offsets + ")");
    }

    void EnsureShells(const std::string& model)
    {
        uint8_t* e = Entries();
        if (!e || !g_attach) return;
        int have = 0;
        for (const Shell& sh : g_shells) have += sh.model == model;
        const MsvcStr m = Str(model.c_str()), pt = Str("clear"), ct = Str("");
        for (; have < kShellsPerModel; ++have)
        {
            // the new entry is the one whose model holder changed to a model of this name (APE+ may free stale slots first)
            void* before[32];
            for (int i = 0; i < 32; ++i) before[i] = *reinterpret_cast<void**>(e + size_t(i) * kEntryStride + kEntryHolder);
            AttachSafe(e, &m, &pt, &ct);
            uint8_t* got = nullptr;
            for (int i = 1; i < 32 && !got; ++i)
            {
                uint8_t* entry = e + size_t(i) * kEntryStride;
                void* h = *reinterpret_cast<void**>(entry + kEntryHolder);
                if (h && h != before[i] && entry[kEntryInUse] && !_stricmp(reinterpret_cast<const char*>(entry + 8), model.c_str())) got = entry;
            }
            if (!got) { Say("no free preview slot for the shell model " + model); return; }
            got[kEntryEnabled] = 0;
            g_shells.push_back(Shell{got, model, false});
        }
    }

    void Eject()
    {
        if (!g_fxOn || !g_weapon) return;
        const std::string path = W(g_sceneFirstPerson ? "viewShellEjectEffect" : "worldShellEjectEffect");
        if (path.empty()) return;
        for (const std::string& m : ba::fxplayer::ModelNames(path)) EnsureShells(m);
        ba::fxplayer::Xform x{};
        if (!BrassAnchor(x)) { Say(std::string(g_sceneFirstPerson ? "the View Gun" : "the World Gun") + " has no tag_brass, no shell eject"); return; }
        ba::fxplayer::SetAnchor(1, x);
        ba::fxplayer::Play(path, 1);
    }

    // after a frame: the newest live model particles get a shell entry of their model (older shells have fallen out of
    // view long before their life ends), the rest are hidden
    void PlaceShells()
    {
        if (g_shells.empty()) return;
        for (Shell& sh : g_shells) sh.used = false;
        const std::vector<ba::fxplayer::ModelPart>& parts = ba::fxplayer::Models();
        for (auto it = parts.rbegin(); it != parts.rend(); ++it)
            for (Shell& sh : g_shells)
                if (!sh.used && sh.model == it->model)
                {
                    sh.used = true;
                    sh.entry[kEntryEnabled] = 1;
                    ba::previewfix::SetFree(sh.entry, it->x.o);
                    break;
                }
        for (Shell& sh : g_shells) if (!sh.used) { sh.entry[kEntryEnabled] = 0; ba::previewfix::SetFree(sh.entry, nullptr); }
    }

    // ---- actions ----
    struct Step
    {
        std::string anim;        // empty: keep the current anim
        bool loop = false;       // loops until the next action
        bool stay = false;       // holds its last frame until the next action
        double hold = 0;         // seconds, ends the step before the anim does (fire rate) or times a step without anim
        std::string sound;
        bool flash = false;
    };
    std::vector<Step> g_steps;
    ULONGLONG g_silentCheck = 0;         // when to tell that an action plays no sound (its fields and its anim's notes are empty)
    std::string g_silentAnim, g_silentAction;
    std::vector<std::string> g_silentTold;
    size_t g_step = 0;
    bool g_stepOn = false, g_atEnd = false, g_clampEnd = false;
    ULONGLONG g_stepStart = 0;
    std::string g_action;

    // the open tab's unsaved values win over gdt.db's, refreshed at each action (RefreshLive)
    std::unordered_map<std::string, std::string> g_live;   // lower-case GDT key -> grid value
    std::string W(const char* key)
    {
        if (!g_live.empty())
        {
            const auto it = g_live.find(Lower(key));
            if (it != g_live.end()) return it->second;
        }
        return Field(g_weapon, key);
    }
    double Seconds(const char* key, double def) { const std::string v = W(key); double d = v.empty() ? def : atof(v.c_str()); return d > 0 ? d : def; }
    std::string Sound(const std::string& base)
    {
        const std::string player = W((base + "Player").c_str()), npc = W(base.c_str());
        return g_firstPerson ? (player.empty() ? npc : player) : (npc.empty() ? player : npc);
    }

    // the weapon's Shared Weapon Sounds asset (raise, putaway, melee and ADS sounds the weapon leaves empty)
    int g_sharedType = -2;
    std::string SharedSound(const std::string& base)
    {
        if (g_sharedType == -2) g_sharedType = TypeIndex("sharedweaponsounds");
        uint8_t* rec = Find(W("sharedWeaponSounds"), g_sharedType);
        if (!rec) return {};
        const std::string player = Field(rec, (base + "Player").c_str()), npc = Field(rec, base.c_str());
        return g_firstPerson ? (player.empty() ? npc : player) : (npc.empty() ? player : npc);
    }

    // ---- third person: the anims the weapon's Player Anim Type plays, by BO3's player anim names ----
    struct Tp
    {
        std::string type, prefix;
        std::string idle, idleAds, fire, fireAds, raise, firstRaise, drop, melee, reload, reloadEmpty, sprint;
        std::string found, missing;
    } g_tp;
    std::vector<std::string> g_tpTold;

    void ResolveTp()
    {
        Tp t;
        t.type = Lower(W("playerAnimType"));
        t.prefix = rp::ConfigGet("weaponpreview", ("prefix_" + t.type).c_str(), "");
        if (t.prefix.empty()) t.prefix = t.type.empty() || t.type == "default" || t.type == "other" || t.type == "none" ? "rifle" : t.type;
        auto pick = [&](std::string& out, const char* label, std::initializer_list<const char*> names)
        {
            for (const char* n : names)
            {
                const std::string name = std::string(n[1] == 'b' ? "pb_" : "pt_") + t.prefix + "_stand_" + (n + 3);
                if (Find(name, g_xanimType)) { out = name; t.found += std::string(t.found.empty() ? "" : ", ") + label; return; }
            }
            t.missing += std::string(t.missing.empty() ? "" : ", ") + std::string(names.begin()[0][1] == 'b' ? "pb_" : "pt_") + t.prefix + "_stand_" + (names.begin()[0] + 3);
        };
        pick(t.idle, "idle", {"pb_idle"});
        pick(t.idleAds, "ads idle", {"pb_idle_ads"});
        pick(t.fire, "fire", {"pt_fire"});
        pick(t.fireAds, "ads fire", {"pt_fire_ads"});
        pick(t.raise, "raise", {"pt_raise"});
        pick(t.firstRaise, "first raise", {"pt_firstraise"});
        pick(t.drop, "putaway", {"pt_putaway"});
        pick(t.melee, "melee", {"pt_melee"});
        pick(t.reload, "reload", {"pt_reload"});
        pick(t.reloadEmpty, "empty reload", {"pt_reload_empty"});
        pick(t.sprint, "sprint", {"pb_sprint_f", "pb_sprint"});
        g_tp = t;
        if (std::find(g_tpTold.begin(), g_tpTold.end(), t.type) == g_tpTold.end())
        {
            g_tpTold.push_back(t.type);
            Say("Player Anim Type '" + t.type + "': third person anims " + (t.found.empty() ? std::string("none found") : "found: " + t.found)
                + (t.missing.empty() ? std::string() : "; not in gdt.db: " + t.missing + " (the mod tools ship none for most types, add them to a gdt to see them)"));
        }
    }

    std::string TpReload(bool empty) { return empty && !g_tp.reloadEmpty.empty() ? g_tp.reloadEmpty : !g_tp.reload.empty() ? g_tp.reload : g_set.tpReload; }
    std::string TpIdle() { return g_ads && !g_tp.idleAds.empty() ? g_tp.idleAds : !g_tp.idle.empty() ? g_tp.idle : g_set.tpIdle; }

    Step IdleStep()
    {
        Step s;
        if (g_firstPerson)
        {
            s.anim = W("idleAnim"); s.loop = true;
            const std::string ads = W("adsUpAnim");
            if (s.anim.empty() && g_ads && !ads.empty()) { s.anim = ads; s.loop = false; s.stay = true; }
        }
        else { s.anim = TpIdle(); s.loop = true; }
        return s;
    }

    // one segment: the 1P anim field, its time field and sound, 3P keeps the idle pose for the time unless a 3P anim is set
    Step Segment(const char* animKey, const char* timeKey, double defTime, const char* soundBase, const std::string& tpAnim, bool first)
    {
        Step s;
        if (soundBase) s.sound = Sound(soundBase);
        if (g_firstPerson) { s.anim = W(animKey); if (s.anim.empty()) s.hold = Seconds(timeKey, defTime); }
        else if (!tpAnim.empty() && first) s.anim = tpAnim;
        else s.hold = Seconds(timeKey, defTime);
        return s;
    }

    bool MakeAction(const std::string& name, std::vector<Step>& steps, std::string& why)
    {
        steps.clear();
        const std::string n = Lower(name);
        if (n == "idle") { steps.push_back(IdleStep()); return true; }
        if (n == "fire")
        {
            const bool ads = g_firstPerson && g_ads && !W("adsFireAnim").empty();
            const std::string tpFire = g_ads && !g_tp.fireAds.empty() ? g_tp.fireAds : !g_tp.fire.empty() ? g_tp.fire : g_set.tpFire;
            Step s = Segment(ads ? "adsFireAnim" : "fireAnim", "fireTime", 0.1, "fireSound", tpFire, true);
            s.flash = true;
            if (g_holdFire) s.hold = Seconds("fireTime", 0.1);
            steps.push_back(s);
            return true;
        }
        if (n == "reload")
        {
            if (W("segmentedReload") == "1")
            {
                const double clip = atof(W("clipSize").c_str()), add = std::max(1.0, atof(W("reloadAmmoAdd").c_str()));
                const int count = std::clamp(int(std::ceil(clip / add)), 1, 8);
                if (!g_firstPerson && !TpReload(false).empty()) { steps.push_back(Segment("reloadAnim", "reloadTime", 1, "reloadSound", TpReload(false), true)); return true; }
                steps.push_back(Segment("reloadStartAnim", "reloadStartTime", 0.5, "reloadStartSound", {}, false));
                for (int i = 0; i < count; ++i) steps.push_back(Segment("reloadAnim", "reloadTime", 0.5, "reloadSound", {}, false));
                steps.push_back(Segment("reloadEndAnim", "reloadEndTime", 0.5, "reloadEndSound", {}, false));
                return true;
            }
            steps.push_back(Segment("reloadAnim", "reloadTime", 2, "reloadSound", TpReload(false), true));
            return true;
        }
        if (n == "emptyreload" || n == "reloadempty") { steps.push_back(Segment("reloadEmptyAnim", "reloadEmptyTime", 2, "reloadEmptySound", TpReload(true), true)); return true; }
        auto shared = [&](Step s, const char* key) { if (s.sound.empty()) s.sound = SharedSound(key); return s; };
        if (n == "raise") { steps.push_back(shared(Segment("raiseAnim", "raiseTime", 0.5, "raiseSound", g_tp.raise, true), "raiseWeapon")); return true; }
        if (n == "firstraise") { steps.push_back(shared(Segment("firstRaiseAnim", "firstRaiseTime", 1, "firstRaiseSound", g_tp.firstRaise.empty() ? g_tp.raise : g_tp.firstRaise, true), "firstRaiseWeapon")); return true; }
        if (n == "drop") { Step s = shared(Segment("dropAnim", "dropTime", 0.5, "putawaySound", g_tp.drop, true), "putAway"); s.stay = true; steps.push_back(s); return true; }
        if (n == "melee") { steps.push_back(shared(Segment("meleeAnim", "meleeTime", 0.8, nullptr, g_tp.melee, true), "meleeWhoosh")); return true; }
        if (n == "sprint")
        {
            if (!g_firstPerson)
            {
                if (g_tp.sprint.empty()) { why = "no 3P sprint anim (pb_" + g_tp.prefix + "_stand_sprint_f) in gdt.db"; return false; }
                Step s; s.anim = g_tp.sprint; s.loop = true; s.hold = 2 * Seconds("sprintLoopTime", 1);
                steps.push_back(s);
                return true;
            }
            steps.push_back(Segment("sprintInAnim", "sprintInTime", 0.2, nullptr, {}, false));
            Step loop = Segment("sprintLoopAnim", "sprintLoopTime", 1, nullptr, {}, false);
            loop.hold = 2 * Seconds("sprintLoopTime", 1);
            steps.push_back(loop);
            steps.push_back(Segment("sprintOutAnim", "sprintOutTime", 0.2, nullptr, {}, false));
            return true;
        }
        why = "unknown action '" + name + "'";
        return false;
    }

    void StartStep()
    {
        if (g_step >= g_steps.size()) return;
        const Step& s = g_steps[g_step];
        if (!s.anim.empty())
        {
            SwitchAnim(s.anim);
            // a torso anim plays over the body idle, as the game layers them
            if (!g_sceneFirstPerson && s.anim.rfind("pt_", 0) == 0) ba::previewfix::SetLayer(s.anim, TpIdle());
        }
        if (!s.sound.empty()) ba::notesounds::PlayAlias(s.sound);
        if (s.flash) { Flash(); Eject(); }
        g_stepStart = GetTickCount64();
        if (s.sound.empty() && !s.anim.empty() && g_action != "idle" && g_action.rfind("anim ", 0) != 0) { g_silentCheck = g_stepStart + 250; g_silentAnim = s.anim; g_silentAction = g_action; }
        g_atEnd = false;
        g_clampEnd = !s.anim.empty() && !s.loop;
        g_stepOn = true;
    }

    void Run(const std::vector<Step>& steps, const std::string& name)
    {
        g_steps = steps;
        g_step = 0;
        g_action = name;
        g_stepOn = false;
        if (!g_steps.empty()) StartStep();
    }

    void RefreshLive();
    bool Action(const std::string& name, std::string* why = nullptr)
    {
        RefreshLive();
        if (!g_weapon) { if (why) *why = "no weapon selected"; return false; }
        std::vector<Step> steps;
        std::string w;
        if (!MakeAction(name, steps, w)) { if (why) *why = w; Say(w); return false; }
        Run(steps, Lower(name));
        return true;
    }

    // a single anim from the list, looping, first person
    void PlayAnim(const std::string& anim)
    {
        if (!g_weapon || anim.empty()) return;
        Step s; s.anim = anim; s.loop = true;
        if (!g_firstPerson) g_firstPerson = true;
        Run({s}, "anim " + anim);
    }

    void StepTick()
    {
        if (!g_stepOn || g_step >= g_steps.size() || !g_weapon) return;
        uint8_t* o = g_opts ? *g_opts : nullptr;
        static ULONGLONG last = 0;
        const ULONGLONG now = GetTickCount64();
        if (o && o[kOptsPaused]) { g_stepStart += last ? now - last : 0; last = now; return; }   // a paused preview stops the step timer
        last = now;
        const Step& s = g_steps[g_step];
        const double elapsed = double(GetTickCount64() - g_stepStart) / 1000.0;
        bool done = false;
        if (s.hold > 0) done = elapsed >= s.hold;
        else if (!s.loop && !s.stay) done = g_atEnd || s.anim.empty();
        if (!done) return;
        { char b[160]; snprintf(b, sizeof b, "step %zu of %s done after %.3f s (hold %.3f, at end %d); ", g_step, g_action.c_str(), elapsed, s.hold, g_atEnd ? 1 : 0); g_trace += b; }
        if (g_trace.size() > 4000) g_trace.erase(0, g_trace.size() - 3000);
        if (g_action == "fire" && g_holdFire && g_step + 1 >= g_steps.size()) { StartStep(); return; }   // the next shot
        if (++g_step < g_steps.size()) { StartStep(); return; }
        g_stepOn = false;
        Run({IdleStep()}, "idle");
    }

    // ---- the awi's anim fields, in its order with its titles ----
    struct AnimField { std::string key, title; };
    std::unordered_map<std::string, std::vector<AnimField>> g_awi;

    std::string PluginDir()
    {
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&PluginDir), &m);
        char p[MAX_PATH] = {};
        GetModuleFileNameA(m, p, MAX_PATH);
        std::string s = p;
        return s.substr(0, s.find_last_of("\\/"));
    }

    const std::vector<AnimField>& AwiFields(const std::string& type)
    {
        auto it = g_awi.find(type);
        if (it != g_awi.end()) return it->second;
        std::vector<AnimField>& out = g_awi[type];
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string root = exe; root = root.substr(0, root.find_last_of("\\/")); root = root.substr(0, root.find_last_of("\\/"));
        std::ifstream f(PluginDir() + "\\deffiles\\" + type + ".awi");
        if (!f) f.open(root + "\\deffiles\\" + type + ".awi");
        if (!f) return out;
        std::stringstream ss; ss << f.rdbuf();
        const std::string text = ss.str();
        static const std::regex re("AddEntry_AssetCombo\\(\\s*\"([A-Za-z0-9_]+)\"\\s*,\\s*\"xanim\"\\s*\\)(\\s*\\.SetTitle\\(\\s*\"([^\"]*)\"\\s*\\))?");
        for (std::sregex_iterator m(text.begin(), text.end(), re), end; m != end; ++m)
            out.push_back({(*m)[1].str(), (*m)[3].matched ? (*m)[3].str() : (*m)[1].str()});
        return out;
    }

    // GDT keys of the type's .awi with their titles, in script order (the property grid lists its entries in the same order)
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> g_awiKeys;
    const std::vector<std::pair<std::string, std::string>>& AwiKeys(const std::string& type)
    {
        auto it = g_awiKeys.find(type);
        if (it != g_awiKeys.end()) return it->second;
        auto& out = g_awiKeys[type];
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string root = exe; root = root.substr(0, root.find_last_of("\\/")); root = root.substr(0, root.find_last_of("\\/"));
        std::ifstream f(PluginDir() + "\\deffiles\\" + type + ".awi");
        if (!f) f.open(root + "\\deffiles\\" + type + ".awi");
        static const std::regex entry("AddEntry_[A-Za-z]+\\(\\s*\"([A-Za-z0-9_]+)\"\\s*[,)]");
        static const std::regex title("\\.SetTitle\\(\\s*\"([^\"]*)\"");
        std::string line;
        while (std::getline(f, line))
        {
            std::smatch m, t;
            if (!std::regex_search(line, m, entry)) continue;
            out.emplace_back(Lower(m[1].str()), std::regex_search(line, t, title) ? t[1].str() : m[1].str());
        }
        return out;
    }

    std::string g_liveNote;              // test harness: how the last refresh went
    void RefreshLive()
    {
        g_live.clear();
        g_liveNote = "not the open tab";
        const ModToolsApeApi* ape = rp::Ape();
        if (!ape || !g_weapon || reinterpret_cast<const char*>(&ape->Properties) >= reinterpret_cast<const char*>(ape) + ape->size) return;
        ModToolsApeAsset cur = {};
        if (!ape->CurrentAsset(rp::self, &cur) || _stricmp(cur.name, g_weaponName.c_str()) || _stricmp(cur.type, g_weaponType.c_str())) return;
        std::string buf(size_t(1) << 20, '\0');
        size_t need = ape->Properties(rp::self, buf.data(), buf.size());
        if (need >= buf.size()) { buf.assign(need + 1, '\0'); need = ape->Properties(rp::self, buf.data(), buf.size()); }
        buf.resize(std::min(need, buf.size()));
        struct Row { std::string title, value, key; };
        std::vector<Row> grid;                                    // in grid order, key from the tooltip's "(key)" APE appends
        size_t at = 0;
        while (at < buf.size())
        {
            size_t nl = buf.find('\n', at); if (nl == std::string::npos) nl = buf.size();
            const std::string ln = buf.substr(at, nl - at);
            at = nl + 1;
            std::vector<std::string> c;
            for (size_t a = 0, b; a <= ln.size(); a = b + 1) { b = ln.find('\t', a); if (b == std::string::npos) b = ln.size(); c.push_back(ln.substr(a, b - a)); }
            if (c.size() < 5) continue;
            Row r{c[2], c[4], {}};
            if (c.size() >= 6 && !c[5].empty() && c[5].back() == ')')
            {
                const size_t o = c[5].rfind('(');
                const std::string k = o == std::string::npos ? std::string() : c[5].substr(o + 1, c[5].size() - o - 2);
                if (!k.empty() && k.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::string::npos) r.key = Lower(k);
            }
            grid.push_back(std::move(r));
        }
        // entries without a key in their tooltip: the next awi key with that title, from where the last match was
        const auto& awi = AwiKeys(g_weaponType);
        size_t cursor = 0;
        for (const Row& r : grid)
        {
            if (!r.key.empty()) { g_live.emplace(r.key, r.value); continue; }
            for (size_t k = cursor; k < awi.size(); ++k)
                if (awi[k].second == r.title) { g_live.emplace(awi[k].first, r.value); cursor = k + 1; break; }
        }
        g_liveNote = std::to_string(grid.size()) + " grid entries, " + std::to_string(AwiKeys(g_weaponType).size()) + " awi keys";
    }

    // ---- the bar under the preview ----
    struct QtFns
    {
        void* (*addToolBarTitle)(void* mw, const void* title) = nullptr;    // QMainWindow::addToolBar(const QString&)
        void (*addToolBarArea)(void* mw, int area, void* tb) = nullptr;     // QMainWindow::addToolBar(Qt::ToolBarArea, QToolBar*)
        void* (*toolBarAddWidget)(void* tb, void* w) = nullptr;             // QToolBar::addWidget
        void (*toolBarSetMovable)(void* tb, bool) = nullptr;
        void (*deleteLater)(void* obj) = nullptr;
        void* (*fromUtf8)(void* ret, const char* s, int n) = nullptr;       // QString::fromUtf8, hidden return
        void (*qsDtor)(void* qs) = nullptr;
        void* (*windowTitle)(void* w, void* ret) = nullptr;                 // QWidget::windowTitle, hidden return
        void* (*toUtf8)(const void* qs, void* ret) = nullptr;               // QString::toUtf8, hidden return (QByteArray)
        const char* (*baData)(const void* ba) = nullptr;
        void (*baDtor)(void* ba) = nullptr;
    } qf;

    struct Bar
    {
        ModToolsWidget toolbar = nullptr, root = nullptr;
        ModToolsWidget fp = nullptr, tp = nullptr, ads = nullptr, hold = nullptr, list = nullptr, status = nullptr;
        ModToolsWidget arms = nullptr, body = nullptr, tpIdle = nullptr, tpFire = nullptr, tpReload = nullptr;
        bool tried = false;
        std::vector<std::string> listAnims;
        bool filling = false;
    } g_bar;

    bool BindQtFns()
    {
        HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll"), c = GetModuleHandleW(L"Qt5Core64r.dll");
        if (!w || !c) return false;
        qf.addToolBarTitle = reinterpret_cast<decltype(qf.addToolBarTitle)>(GetProcAddress(w, "?addToolBar@QMainWindow@@QEAAPEAVQToolBar@@AEBVQString@@@Z"));
        qf.addToolBarArea = reinterpret_cast<decltype(qf.addToolBarArea)>(GetProcAddress(w, "?addToolBar@QMainWindow@@QEAAXW4ToolBarArea@Qt@@PEAVQToolBar@@@Z"));
        qf.toolBarAddWidget = reinterpret_cast<decltype(qf.toolBarAddWidget)>(GetProcAddress(w, "?addWidget@QToolBar@@QEAAPEAVQAction@@PEAVQWidget@@@Z"));
        qf.toolBarSetMovable = reinterpret_cast<decltype(qf.toolBarSetMovable)>(GetProcAddress(w, "?setMovable@QToolBar@@QEAAX_N@Z"));
        qf.deleteLater = reinterpret_cast<decltype(qf.deleteLater)>(GetProcAddress(c, "?deleteLater@QObject@@QEAAXXZ"));
        qf.fromUtf8 = reinterpret_cast<decltype(qf.fromUtf8)>(GetProcAddress(c, "?fromUtf8@QString@@SA?AV1@PEBDH@Z"));
        qf.qsDtor = reinterpret_cast<decltype(qf.qsDtor)>(GetProcAddress(c, "??1QString@@QEAA@XZ"));
        qf.windowTitle = reinterpret_cast<decltype(qf.windowTitle)>(GetProcAddress(w, "?windowTitle@QWidget@@QEBA?AVQString@@XZ"));
        qf.toUtf8 = reinterpret_cast<decltype(qf.toUtf8)>(GetProcAddress(c, "?toUtf8@QString@@QEBA?AVQByteArray@@XZ"));
        qf.baData = reinterpret_cast<decltype(qf.baData)>(GetProcAddress(c, "?constData@QByteArray@@QEBAPEBDXZ"));
        qf.baDtor = reinterpret_cast<decltype(qf.baDtor)>(GetProcAddress(c, "??1QByteArray@@QEAA@XZ"));
        return qf.addToolBarTitle && qf.addToolBarArea && qf.toolBarAddWidget && qf.deleteLater && qf.fromUtf8 && qf.qsDtor;
    }

    std::string TitleOf(void* w)
    {
        if (!qf.windowTitle || !qf.toUtf8 || !qf.baData || !qf.baDtor) return {};
        void* qs = nullptr; void* ba = nullptr;
        qf.windowTitle(w, &qs);
        qf.toUtf8(&qs, &ba);
        std::string r = qf.baData(&ba);
        qf.baDtor(&ba);
        qf.qsDtor(&qs);
        return r;
    }

    // a bar left by an earlier load of the plugin (hot reload)
    void RemoveOldBars(ModToolsWidget mw)
    {
        std::vector<ModToolsWidget> old;
        for (int i = 0; i < 32; ++i)
        {
            ModToolsWidget tb = rp::host->ui->FindChild(mw, "QToolBar", i);
            if (!tb) break;
            if (TitleOf(tb) == "Weapon preview") old.push_back(tb);
        }
        for (ModToolsWidget tb : old) { rp::host->ui->SetVisible(tb, 0); qf.deleteLater(tb); }
    }

    void SetStatus(const std::string& s)
    {
        g_status = s;
        if (g_bar.status) rp::host->ui->SetLabelText(g_bar.status, s.c_str());
    }

    void SyncBar()
    {
        if (!g_bar.root) return;
        const ModToolsUiApi* ui = rp::host->ui;
        ui->SetChecked(g_bar.fp, g_firstPerson ? 1 : 0);
        ui->SetChecked(g_bar.tp, g_firstPerson ? 0 : 1);
    }

    void FillList()
    {
        if (!g_bar.list || !g_weapon) return;
        const ModToolsUiApi* ui = rp::host->ui;
        g_bar.filling = true;
        ui->ComboClear(g_bar.list);
        g_bar.listAnims.clear();
        ui->ComboAdd(g_bar.list, "Play one anim...");
        g_bar.listAnims.push_back({});
        for (const AnimField& f : AwiFields(g_weaponType))
        {
            const std::string v = W(f.key.c_str());
            if (v.empty()) continue;
            ui->ComboAdd(g_bar.list, (f.title + "  (" + v + ")").c_str());
            g_bar.listAnims.push_back(v);
        }
        ui->ComboSetCurrent(g_bar.list, 0);
        g_bar.filling = false;
    }

    void Rebuild()
    {
        if (!g_weapon) return;
        g_sceneAnim.clear();
        std::string why;
        Action("idle", &why);
    }

    void SaveSettings()
    {
        rp::ConfigSet("weaponpreview", "arms", g_set.arms);
        rp::ConfigSet("weaponpreview", "body", g_set.body);
        rp::ConfigSet("weaponpreview", "anim_3p_idle", g_set.tpIdle);
        rp::ConfigSet("weaponpreview", "anim_3p_fire", g_set.tpFire);
        rp::ConfigSet("weaponpreview", "anim_3p_reload", g_set.tpReload);
    }

    void OnMode(void* user, int on)
    {
        if (!on) return;
        const bool fp = user != nullptr;
        if (fp == g_firstPerson) return;
        g_firstPerson = fp;
        rp::ConfigSet("weaponpreview", "view", fp ? "1p" : "3p");
        Rebuild();
    }
    void OnAction(void* user) { Action(static_cast<const char*>(user)); }
    void OnAds(void*, int on)
    {
        g_ads = on != 0;
        if (!g_weapon || !g_firstPerson) return;
        const std::string snd = SharedSound(g_ads ? "raiseADSWeapon" : "lowerADSWeapon");
        if (!snd.empty()) ba::notesounds::PlayAlias(snd);
        if (g_action.empty()) Action("idle");
    }
    void OnHold(void*, int on) { g_holdFire = on != 0; }
    void OnList(void*, int index)
    {
        if (g_bar.filling || index <= 0 || size_t(index) >= g_bar.listAnims.size()) return;
        const bool wasTp = !g_firstPerson;
        PlayAnim(g_bar.listAnims[size_t(index)]);
        if (wasTp) SyncBar();
    }
    void OnEdit(void* user, int)
    {
        std::string* field = static_cast<std::string*>(user);
        ModToolsWidget w = field == &g_set.arms ? g_bar.arms : field == &g_set.body ? g_bar.body : field == &g_set.tpIdle ? g_bar.tpIdle
                         : field == &g_set.tpFire ? g_bar.tpFire : g_bar.tpReload;
        std::string v = rp::EditText(w);
        while (!v.empty() && v.back() == ' ') v.pop_back();
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        if (v == *field) return;
        *field = v;
        SaveSettings();
        const bool affects = field == &g_set.arms ? g_firstPerson : !g_firstPerson;
        if (affects) Rebuild();
    }
    void OnView(void*) { SetCamera(g_firstPerson); }

    ModToolsWidget Button(ModToolsWidget row, const char* text, const char* action, const char* tip)
    {
        const ModToolsUiApi* ui = rp::host->ui;
        ModToolsWidget b = ui->PushButton(text, g_bar.root);
        if (tip) ui->SetToolTip(b, tip);
        rp::host->OnClicked(rp::self, b, &OnAction, const_cast<char*>(action));
        ui->BoxAddWidget(row, b, 0);
        return b;
    }
    ModToolsWidget Edit(ModToolsWidget row, const char* label, std::string* field, const char* tip)
    {
        const ModToolsUiApi* ui = rp::host->ui;
        ModToolsWidget l = ui->Label(label, g_bar.root);
        ui->BoxAddWidget(row, l, 0);
        ModToolsWidget e = ui->LineEdit(field->c_str(), g_bar.root);
        ui->SetToolTip(e, tip);
        rp::host->OnChanged(rp::self, e, &OnEdit, field);
        ui->BoxAddWidget(row, e, 1);
        return e;
    }

    bool MakeBar()
    {
        if (g_bar.root) return true;
        if (g_bar.tried) return false;
        g_bar.tried = true;
        const ModToolsUiApi* ui = rp::host->ui;
        if (!ui || !rp::HasUi(&ui->ComboBox) || !BindQtFns()) { rp::Log("weaponpreview: Qt widgets unavailable, no preview bar"); return false; }
        ModToolsWidget dock = ui->FindEditorWidget("PreviewToolBar");
        ModToolsWidget mw = dock ? ui->FindChild(dock, "QMainWindow", 0) : nullptr;
        if (!mw) { rp::Log("weaponpreview: the preview's QMainWindow not found, no preview bar"); return false; }
        RemoveOldBars(mw);
        void* title = nullptr;
        qf.fromUtf8(&title, "Weapon preview", -1);
        void* tb = qf.addToolBarTitle(mw, &title);
        qf.qsDtor(&title);
        if (!tb) return false;
        qf.addToolBarArea(mw, 8, tb);   // Qt::BottomToolBarArea
        if (qf.toolBarSetMovable) qf.toolBarSetMovable(tb, false);
        g_bar.toolbar = tb;
        g_bar.root = ui->Widget(nullptr);
        ModToolsWidget col = ui->VBox(g_bar.root);
        ui->LayoutMargins(col, 2, 2, 2, 2);
        ui->LayoutSpacing(col, 3);
        ModToolsWidget row1 = ui->HBox(), row2 = ui->HBox(), row3 = ui->HBox();
        ui->BoxAddLayout(col, row1, 0);
        ui->BoxAddLayout(col, row2, 0);
        ui->BoxAddLayout(col, row3, 0);

        g_bar.fp = ui->RadioButton("1P", g_bar.root);
        g_bar.tp = ui->RadioButton("3P", g_bar.root);
        ui->SetToolTip(g_bar.fp, "First person: the arms and the View Gun, the camera at the eye");
        ui->SetToolTip(g_bar.tp, "Third person: the body and the World Gun on tag_weapon_right");
        rp::host->OnChanged(rp::self, g_bar.fp, &OnMode, reinterpret_cast<void*>(1));
        rp::host->OnChanged(rp::self, g_bar.tp, &OnMode, nullptr);
        ui->BoxAddWidget(row1, g_bar.fp, 0);
        ui->BoxAddWidget(row1, g_bar.tp, 0);
        Button(row1, "Idle", "idle", nullptr);
        Button(row1, "Fire", "fire", "Fire anim, fire sound and flash, back to idle");
        g_bar.hold = ui->CheckBox("Hold", g_bar.root);
        ui->SetToolTip(g_bar.hold, "Fire repeats every Fire Time (fireTime) until unticked");
        rp::host->OnChanged(rp::self, g_bar.hold, &OnHold, nullptr);
        ui->BoxAddWidget(row1, g_bar.hold, 0);
        g_bar.ads = ui->CheckBox("ADS", g_bar.root);
        ui->SetToolTip(g_bar.ads, "First person aims: the ADS Up pose over idle and fire, Fire plays ADS Fire");
        rp::host->OnChanged(rp::self, g_bar.ads, &OnAds, nullptr);
        ui->BoxAddWidget(row1, g_bar.ads, 0);
        Button(row1, "Reload", "reload", "Reload (start, a loop per round, end when Segmented Reload is set)");
        Button(row1, "Empty", "emptyreload", "Empty reload");
        ui->BoxAddStretch(row1, 1);

        Button(row2, "Raise", "raise", nullptr);
        Button(row2, "First raise", "firstraise", nullptr);
        Button(row2, "Drop", "drop", nullptr);
        Button(row2, "Sprint", "sprint", "Sprint in, loop, out");
        Button(row2, "Melee", "melee", nullptr);
        g_bar.list = ui->ComboBox(g_bar.root);
        ui->SetToolTip(g_bar.list, "Every anim the weapon sets, played on its own in first person");
        rp::host->OnChanged(rp::self, g_bar.list, &OnList, nullptr);
        ui->BoxAddWidget(row2, g_bar.list, 1);
        ModToolsWidget view = ui->PushButton("View", g_bar.root);
        ui->SetToolTip(view, "Camera back to the eye (1P) or in front of the body (3P)");
        rp::host->OnClicked(rp::self, view, &OnView, nullptr);
        ui->BoxAddWidget(row2, view, 0);

        g_bar.arms = Edit(row3, "Arms", &g_set.arms, "1P arms model (xmodel)");
        g_bar.body = Edit(row3, "Body", &g_set.body, "3P body model (a full body xmodel)");
        g_bar.tpIdle = Edit(row3, "3P idle", &g_set.tpIdle, "3P idle xanim on the body");
        g_bar.tpFire = Edit(row3, "3P fire", &g_set.tpFire, "3P fire xanim, empty = the idle pose for Fire Time");
        g_bar.tpReload = Edit(row3, "3P reload", &g_set.tpReload, "3P reload xanim, empty = the idle pose for the reload times");

        g_bar.status = ui->Label("", g_bar.root);
        ui->SetWordWrap(g_bar.status, 0);
        ui->BoxAddWidget(col, g_bar.status, 0);
        qf.toolBarAddWidget(tb, g_bar.root);
        SyncBar();
        return true;
    }

    void ShowBar(bool on)
    {
        if (on && !MakeBar()) return;
        if (g_bar.toolbar) rp::host->ui->SetVisible(g_bar.toolbar, on ? 1 : 0);
    }

    // APE's 65 degree view is 16:9 high: a pane narrower than 16:9 loses the sides, so the weapon preview fits 16:9 into the pane
    void* hk_ViewOf(int* cam, float* out, float fov)
    {
        if (!g_weapon || g_sceneAnim.empty()) return g_origViewOf(cam, out, fov);
        const float use = g_sceneFirstPerson ? g_fov1p : fov;
        void* r = g_origViewOf(cam, out, use);
        const float w = float(cam[0]), h = float(cam[1]);
        if (w > 0 && h > 0)
        {
            const float aspect = w / h, tx = tanf(use * 0.0087266462f);
            const float ty = aspect < 16.0f / 9.0f ? tx / aspect : tx * 0.5625f;
            out[12] = ty;
            out[13] = aspect * ty;
        }
        // origin, forward, left, up, then the two tangents: the fx player projects with it
        const float right[3] = {-out[6], -out[7], -out[8]};
        void* hwnd = g_winId && g_widget ? reinterpret_cast<void*>(g_winId(g_widget)) : nullptr;
        ba::fxplayer::SetCamera(out, out + 3, right, out + 9, out[12], out[13], hwnd);
        return r;
    }

    // ---- dispatch: a weapon gets the scene, anything else turns the weapon preview off ----
    bool IsWeapon(int type, std::string& name)
    {
        for (int i = 0; i < 4; ++i)
        {
            if (g_types[i] == -2) g_types[i] = TypeIndex(kWeaponTypes[i]);
            if (type >= 0 && type == g_types[i]) { name = kWeaponTypes[i]; return true; }
        }
        return false;
    }

    void Deactivate()
    {
        if (!g_weapon && !g_overRecord) return;
        g_weapon = nullptr;
        g_overRecord = nullptr;
        g_weaponName.clear(); g_weaponType.clear(); g_action.clear();
        g_steps.clear();
        g_stepOn = false;
        g_sceneAnim.clear();
        ba::fxplayer::StopAll();
        DropShells();
        ba::previewfix::ClearLayers();
        ba::previewfix::ClearAds(); g_adsSet = false; g_adsFrac = 0; g_adsUpWeight = 0;
        ShowBar(false);
    }

    void DispatchInner(void* widget, uint8_t* rec)
    {
        g_widget = widget;
        ++g_dispatches;
        g_lastType = rec ? RecType(rec) : -9;
        g_dispLog += (rec ? RecName(rec) : std::string("null")) + ":" + std::to_string(g_lastType) + " ";
        if (g_dispLog.size() > 600) g_dispLog.erase(0, 300);
        std::string type;
        if (!g_on || !rec || !IsWeapon(RecType(rec), type))
        {
            Deactivate();
            g_origDispatch(widget, rec);
            return;
        }
        if (g_xanimType == -2) g_xanimType = TypeIndex("xanim");
        const bool same = rec == g_weapon;
        g_weapon = rec;
        g_weaponType = type;
        g_weaponName = RecName(rec);
        RefreshLive();
        ResolveTp();
        g_sceneAnim.clear();
        if (!same) g_ads = false;
        ShowBar(true);
        if (!same) FillList();
        SyncBar();
        std::string why;
        if (!Action("idle", &why) || g_sceneAnim.empty())
        {
            g_overRecord = nullptr;
            g_origDispatch(widget, rec);
            SetStatus(g_weaponName + ": no Idle anim (" + (why.empty() ? g_status : why) + ")");
            return;
        }
        SetStatus(g_weaponName + (g_firstPerson ? ": 1P, " : ": 3P, ") + g_sceneAnim);
    }

    // runs inside APE's selection handling, outside the host's isolation: an exception here turns the weapon preview off
    bool g_broken = false;
    bool DispatchGuarded(void* widget, uint8_t* rec)
    {
        __try { DispatchInner(widget, rec); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_broken = true; return false; }
    }
    void hk_Dispatch(void* widget, uint8_t* rec)
    {
        if (g_inBuild || g_broken) { g_origDispatch(widget, rec); return; }
        if (!DispatchGuarded(widget, rec))
        {
            rp::Log("weaponpreview: exception in the weapon preview, off until the plugin reloads");
            g_weapon = nullptr; g_overRecord = nullptr; g_inBuild = false;
            g_origDispatch(widget, rec);
        }
    }

    bool StartsFunction(void* target)
    {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fn = target ? RtlLookupFunctionEntry(DWORD64(target), &base, nullptr) : nullptr;
        return fn && DWORD64(fn->BeginAddress) + base == DWORD64(target);
    }

    ClockFn ResolveClock()
    {
        const uint8_t* p = static_cast<const uint8_t*>(Ape(kClockCall));
        if (!p || p[0] != 0xFF || p[1] != 0x15) return nullptr;
        const int32_t rel = *reinterpret_cast<const int32_t*>(p + 2);
        return *reinterpret_cast<ClockFn const*>(p + 6 + rel);
    }

    float LengthSafe(uint8_t* e)
    {
        __try { return g_length(e); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
}

static void InstallHooks();
static bool g_hooksTried = false;

// In development: a copy installed next to the editor (<bin>\plugins) never turns it on, only a dev build elsewhere does
static bool DevCopy()
{
    wchar_t exe[MAX_PATH], self[MAX_PATH];
    HMODULE m = nullptr;
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)
        || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&DevCopy), &m)
        || !GetModuleFileNameW(m, self, MAX_PATH)) return false;
    std::wstring dir(exe);
    dir.resize(dir.find_last_of(L'\\') + 1);
    dir += L"plugins\\";
    return _wcsnicmp(self, dir.c_str(), dir.size()) != 0;
}

void Install()
{
    g_on = DevCopy() && rp::ConfigGetBool("weaponpreview", "enabled", false);
    g_set.arms = rp::ConfigGet("weaponpreview", "arms", g_set.arms.c_str());
    g_set.body = rp::ConfigGet("weaponpreview", "body", g_set.body.c_str());
    g_set.tpIdle = rp::ConfigGet("weaponpreview", "anim_3p_idle", g_set.tpIdle.c_str());
    g_set.tpFire = rp::ConfigGet("weaponpreview", "anim_3p_fire", "");
    g_set.tpReload = rp::ConfigGet("weaponpreview", "anim_3p_reload", "");
    g_firstPerson = rp::ConfigGet("weaponpreview", "view", "1p") != "3p";
    g_fxOn = rp::ConfigGetBool("weaponpreview", "fx", true);
    g_fov1p = std::clamp(float(atof(rp::ConfigGet("weaponpreview", "fov_1p", "80").c_str())), 20.0f, 140.0f);
    if (g_on) InstallHooks();
    else rp::Log("weaponpreview: off (enabled=0), no hooks installed");
}

// the APE detours and the fx player, only while the feature is on (enabled=1, or "weaponpreview on")
static void InstallHooks()
{
    if (g_ready || g_hooksTried) return;
    g_hooksTried = true;
    void* dispatch = Ape(kDispatch);
    void* rawGet = Ape(kRawGet);
    void* viewOf = Ape(kViewOf);
    g_assign = reinterpret_cast<AssignFn>(Ape(kAssign));
    g_intern = reinterpret_cast<InternFn>(Ape(kIntern));
    g_typeIndex = reinterpret_cast<TypeIndexFn>(Ape(kTypeIndex));
    g_field = reinterpret_cast<FieldFn>(Ape(kField));
    g_lookup = reinterpret_cast<LookupFn>(Ape(kLookup));
    g_length = reinterpret_cast<LengthFn>(Ape(kLength));
    g_setAnim = reinterpret_cast<SetAnimFn>(Ape(kSetAnim));
    g_db = static_cast<void**>(Ape(kDb));
    g_opts = static_cast<uint8_t**>(Ape(kOpts));
    g_clock = ResolveClock();
    g_attach = reinterpret_cast<AttachFn>(Ape(kAttach));
    if (HMODULE qw = GetModuleHandleW(L"Qt5Widgets64r.dll")) g_winId = reinterpret_cast<decltype(g_winId)>(GetProcAddress(qw, "?effectiveWinId@QWidget@@QEBA_KXZ"));
    const bool found = StartsFunction(dispatch) && StartsFunction(rawGet) && StartsFunction(viewOf) && StartsFunction(reinterpret_cast<void*>(g_assign))
                    && StartsFunction(reinterpret_cast<void*>(g_intern)) && StartsFunction(reinterpret_cast<void*>(g_typeIndex))
                    && StartsFunction(reinterpret_cast<void*>(g_field)) && StartsFunction(reinterpret_cast<void*>(g_lookup))
                    && StartsFunction(reinterpret_cast<void*>(g_length)) && StartsFunction(reinterpret_cast<void*>(g_setAnim))
                    && g_db && g_opts && g_clock;
    if (found)
        g_ready = rp::host->Detour(rp::self, dispatch, reinterpret_cast<void*>(&hk_Dispatch), reinterpret_cast<void**>(&g_origDispatch))
               && rp::host->Detour(rp::self, rawGet, reinterpret_cast<void*>(&hk_RawGet), reinterpret_cast<void**>(&g_origRawGet))
               && rp::host->Detour(rp::self, viewOf, reinterpret_cast<void*>(&hk_ViewOf), reinterpret_cast<void**>(&g_origViewOf));
    if (g_ready && g_fxOn) { ba::fxplayer::SetResolver(&ResolveMaterial); ba::fxplayer::Install(); }
    rp::Log("weaponpreview: %s", !found ? "APE binary not recognised, off" : g_ready ? "detours ok" : "detour FAILED");
}

void Tick()
{
    if (!g_ready || !g_weapon) return;
    if (g_silentCheck && GetTickCount64() >= g_silentCheck)
    {
        g_silentCheck = 0;
        const std::string key = g_weaponName + "|" + g_silentAction + "|" + (g_firstPerson ? "1" : "3");
        if (ba::notesounds::SoundNoteCount() == 0 && std::find(g_silentTold.begin(), g_silentTold.end(), key) == g_silentTold.end())
        {
            g_silentTold.push_back(key);
            Say(g_weaponName + " " + g_silentAction + ": no sound to play, the weapon's sound field for it is empty and " + g_silentAnim
                + " has no sound notes (GDT Sound notes or exported sndnt# notetracks)");
        }
    }
    StepTick();
    if (g_weapon && g_stepOn && g_step < g_steps.size())
    {
        const std::string what = g_action.rfind("anim ", 0) == 0 ? g_action.substr(5) : g_action;
        const std::string s = g_weaponName + (g_firstPerson ? ": 1P, " : ": 3P, ") + what + (g_sceneAnim.empty() ? "" : " (" + g_sceneAnim + ")");
        if (s != g_status) SetStatus(s);
    }
}

void Shutdown()
{
    DropShells();
    g_overRecord = nullptr;
    ba::fxplayer::Shutdown();
    if (g_bar.toolbar && qf.deleteLater) { rp::host->ui->SetVisible(g_bar.toolbar, 0); qf.deleteLater(g_bar.toolbar); }
    g_bar = Bar{};
}

// ---- ADS anims from their source exports: bones local to their nearest exported ancestor, as the linker converts them ----
namespace
{
    struct ExportCache
    {
        std::string path, arms;
        FILETIME stamp{};
        ULONGLONG checked = 0;
        bool ok = false;
        ba::previewfix::AdsTracks tracks;
        std::string why;
    };
    std::unordered_map<std::string, ExportCache> g_exports;   // anim name -> its export tracks
    bool g_exportsChanged = false;

    std::string ToolsRoot()
    {
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string root = exe;
        root = root.substr(0, root.find_last_of("\\/"));
        return root.substr(0, root.find_last_of("\\/"));
    }

    void QuatFromMatrix(const float m[3][3], float* q)
    {
        const float t = m[0][0] + m[1][1] + m[2][2];
        if (t > 0) { const float s = std::sqrt(t + 1) * 2; q[3] = s / 4; q[0] = (m[2][1] - m[1][2]) / s; q[1] = (m[0][2] - m[2][0]) / s; q[2] = (m[1][0] - m[0][1]) / s; }
        else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) { const float s = std::sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2; q[3] = (m[2][1] - m[1][2]) / s; q[0] = s / 4; q[1] = (m[0][1] + m[1][0]) / s; q[2] = (m[0][2] + m[2][0]) / s; }
        else if (m[1][1] > m[2][2]) { const float s = std::sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2; q[3] = (m[0][2] - m[2][0]) / s; q[0] = (m[0][1] + m[1][0]) / s; q[1] = s / 4; q[2] = (m[1][2] + m[2][1]) / s; }
        else { const float s = std::sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2; q[3] = (m[1][0] - m[0][1]) / s; q[0] = (m[0][2] + m[2][0]) / s; q[1] = (m[1][2] + m[2][1]) / s; q[2] = s / 4; }
        float len = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
        len = len > 0 ? 1.0f / std::sqrt(len) : 1.0f;
        const float sgn = q[3] < 0 ? -len : len;
        for (int c = 0; c < 4; ++c) q[c] *= sgn;
    }

    bool BuildExportTracks(const std::string& path, ExportCache& c)
    {
        ba::xanimbin::Export ex;
        if (!ba::xanimbin::Load(path, ex, c.why)) return false;
        std::vector<std::pair<std::string, std::string>> h;
        if (!ba::previewfix::Hierarchy(Entries(), h)) { c.why = "no arms hierarchy"; return false; }
        const std::unordered_map<std::string, std::string> parent(h.begin(), h.end());
        std::unordered_map<std::string, size_t> index;
        for (size_t b = 0; b < ex.bones.size(); ++b) if (!ex.bones[b].empty()) index[ex.bones[b]] = b;
        for (size_t b = 0; b < ex.bones.size(); ++b)
        {
            auto pit = parent.find(ex.bones[b]);
            if (pit == parent.end()) continue;
            std::string a = pit->second;
            while (!a.empty() && !index.count(a)) { auto q = parent.find(a); a = q == parent.end() ? std::string() : q->second; }
            if (a.empty()) continue;   // a root of the export: the converter drops it
            const size_t ai = index[a];
            std::vector<ba::previewfix::AdsKey> keys;
            for (const auto& frame : ex.frames)
            {
                const ba::xanimbin::Pose& P = frame[ai];
                const ba::xanimbin::Pose& B = frame[b];
                if (!P.set || !B.set) continue;
                float m[3][3];
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) m[i][j] = P.axis[i][0] * B.axis[j][0] + P.axis[i][1] * B.axis[j][1] + P.axis[i][2] * B.axis[j][2];
                ba::previewfix::AdsKey k{};
                QuatFromMatrix(m, k.q);
                const float d[3] = {B.o[0] - P.o[0], B.o[1] - P.o[1], B.o[2] - P.o[2]};
                for (int i = 0; i < 3; ++i) k.v[i] = P.axis[i][0] * d[0] + P.axis[i][1] * d[1] + P.axis[i][2] * d[2];
                keys.push_back(k);
            }
            if (!keys.empty()) c.tracks[ex.bones[b]] = std::move(keys);
        }
        if (c.tracks.empty()) { c.why = "no bone of " + path + " is under an exported ancestor"; return false; }
        return true;
    }

    // the anim's export tracks, rechecked once a second for edits, null when there is no readable export
    const ba::previewfix::AdsTracks* ExportTracks(const std::string& anim)
    {
        if (anim.empty()) return nullptr;
        ExportCache& c = g_exports[anim];
        const ULONGLONG now = GetTickCount64();
        if (c.checked && now - c.checked < 1000 && c.arms == g_set.arms) return c.ok ? &c.tracks : nullptr;
        c.checked = now;
        std::string file = Field(Find(anim, g_xanimType), "filename"), rel;
        for (char ch : file)
        {
            if (ch == '/') ch = '\\';
            if (ch == '\\' && !rel.empty() && rel.back() == '\\') continue;
            rel += ch;
        }
        const std::string path = rel.empty() ? std::string() : rel.size() > 1 && rel[1] == ':' ? rel : ToolsRoot() + "\\xanim_export\\" + rel;
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        const bool have = !path.empty() && GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa);
        if (c.path == path && c.arms == g_set.arms && have && CompareFileTime(&fa.ftLastWriteTime, &c.stamp) == 0) return c.ok ? &c.tracks : nullptr;
        const bool had = c.ok;
        c.path = path; c.arms = g_set.arms; c.stamp = fa.ftLastWriteTime; c.tracks.clear(); c.why.clear();
        c.ok = have ? BuildExportTracks(path, c) : (c.why = "no export file '" + path + "'", false);
        g_exportsChanged = g_exportsChanged || had || c.ok;
        return c.ok ? &c.tracks : nullptr;
    }
}

// ADS as the game runs it: the aim fraction moves linearly, ADS Up plays at time frac while aiming and ADS Down at
// 1 - frac otherwise, the two crossfading over 0.1 s.
static void UpdateAds()
{
    const ULONGLONG now = GetTickCount64();
    const double dt = g_adsTick ? std::min(0.25, double(now - g_adsTick) / 1000.0) : 0.0;
    g_adsTick = now;
    const bool fp = g_weapon && g_sceneFirstPerson;
    const std::string up = fp && W("aimDownSight") != "0" ? W("adsUpAnim") : std::string();
    const std::string down = fp ? W("adsDownAnim") : std::string();
    if (up.empty() && down.empty())
    {
        if (g_adsSet) ba::previewfix::ClearAds();
        g_adsSet = false; g_adsFrac = 0; g_adsUpWeight = 0;
        return;
    }
    const uint8_t* o = g_opts ? *g_opts : nullptr;
    const bool paused = o && o[kOptsPaused];
    const bool aim = g_ads && !up.empty() && (g_action == "idle" || g_action == "fire");
    if (!paused)
    {
        const double rate = aim ? 1.0 / Seconds("adsTransInTime", 0.2) : -1.0 / Seconds("adsTransOutTime", 0.2);
        g_adsFrac = float(std::clamp(g_adsFrac + dt * rate, 0.0, 1.0));
        const float blend = float(dt / 0.1);
        g_adsUpWeight = aim ? std::min(1.0f, g_adsUpWeight + blend) : std::max(0.0f, g_adsUpWeight - blend);
    }
    const ba::previewfix::AdsTracks* upTracks = ExportTracks(up);
    const ba::previewfix::AdsTracks* downTracks = ExportTracks(down);
    if (g_exportsChanged) { ba::previewfix::ClearAds(); g_exportsChanged = false; }
    g_adsSet = ba::previewfix::SetAds(up, g_adsUpWeight, g_adsFrac, upTracks, down, 1 - g_adsUpWeight, 1 - g_adsFrac, downTracks);
}

// a one-shot anim stops 4 ms before its end and stays there (the frame function wraps the time with fmodf)
static void BeforeFrameInner()
{
    if (g_ready) UpdateAds();
    if (!g_ready || !g_weapon || !g_clampEnd || !g_clock) return;
    uint8_t* o = g_opts ? *g_opts : nullptr;
    if (!o || o[kOptsPaused]) return;
    const float len = LengthSafe(o + kEntries);
    if (len <= 0.01f) return;
    const int now = int(g_clock());
    const double next = *reinterpret_cast<double*>(o + kOptsTime) + double(now - *reinterpret_cast<int*>(o + kOptsClock)) / 1000.0;
    if (!g_atEnd && next < double(len) - 0.004) return;
    *reinterpret_cast<double*>(o + kOptsTime) = std::max(0.0, double(len) - 0.004);
    *reinterpret_cast<int*>(o + kOptsClock) = now;
    g_atEnd = true;
}

static void AfterFrameInner()
{
    if (!g_ready) return;
    if (g_weapon && ba::fxplayer::Active())
    {
        ba::fxplayer::Xform x{};
        if (FlashAnchor(x)) ba::fxplayer::SetAnchor(0, x);
        if (BrassAnchor(x)) ba::fxplayer::SetAnchor(1, x);
    }
    ba::fxplayer::Update();
    if (g_weapon) PlaceShells();
}

// both run inside APE's frame function, outside the host's isolation
void BeforeFrame()
{
    if (g_broken) return;
    __try { BeforeFrameInner(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_broken = true; }
}
void AfterFrame()
{
    if (g_broken) return;
    __try { AfterFrameInner(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_broken = true; }
}

bool Command(const std::string& command, std::string& reply)
{
    if (command.compare(0, 13, "weaponpreview") != 0) return false;
    const std::string arg = command.size() > 14 ? command.substr(14) : std::string();
    if (arg == "on" && !g_ready && DevCopy()) InstallHooks();
    if (!g_ready) { reply = g_hooksTried ? "weaponpreview unavailable" : "weaponpreview off (enabled=0, no hooks installed; \"weaponpreview on\" turns it on)"; return true; }
    if (arg == "on" || arg == "off")
    {
        g_on = arg == "on";
        rp::ConfigSet("weaponpreview", "enabled", g_on ? "1" : "0");
        if (!g_on) Deactivate();
        reply = "weaponpreview " + arg + " (select the weapon again)";
        return true;
    }
    if (arg == "mode 1p" || arg == "mode 3p")
    {
        g_firstPerson = arg == "mode 1p";
        rp::ConfigSet("weaponpreview", "view", g_firstPerson ? "1p" : "3p");
        SyncBar();
        Rebuild();
        reply = "mode " + arg.substr(5);
        return true;
    }
    if (arg.compare(0, 7, "action ") == 0)
    {
        std::string why;
        reply = Action(arg.substr(7), &why) ? "action " + arg.substr(7) : why;
        return true;
    }
    if (arg.compare(0, 5, "anim ") == 0)
    {
        const std::string v = W(arg.substr(5).c_str());
        if (v.empty()) { reply = "field " + arg.substr(5) + " is empty"; return true; }
        PlayAnim(v);
        SyncBar();
        reply = "anim " + v;
        return true;
    }
    if (arg == "ads on" || arg == "ads off")   // test harness: the ADS box
    {
        const int on = arg == "ads on" ? 1 : 0;
        if (g_bar.ads && rp::host->ui) rp::host->ui->SetChecked(g_bar.ads, on);
        if (g_ads != (on != 0)) OnAds(nullptr, on);
        reply = std::string("ads ") + (g_ads ? "on" : "off") + ", action '" + g_action + "', scene anim '" + g_sceneAnim + "'";
        return true;
    }
    if (arg == "view") { SetCamera(g_firstPerson); reply = "camera reset"; return true; }
    if (arg == "trace") { reply = g_trace; g_trace.clear(); return true; }
    if (arg.compare(0, 7, "fxslow ") == 0) { ba::fxplayer::SetTimeScale(atof(arg.c_str() + 7)); reply = "fx time scale " + arg.substr(7); return true; }
    if (arg == "play" || arg == "pause")   // test harness: the preview's pause flag (the tool bar icon is not updated)
    {
        uint8_t* o = g_opts ? *g_opts : nullptr;
        HMODULE qw = GetModuleHandleW(L"Qt5Widgets64r.dll");
        auto update = qw ? reinterpret_cast<void (*)(void*)>(GetProcAddress(qw, "?update@QWidget@@QEAAXXZ")) : nullptr;
        if (!o || !g_widget || !update) { reply = "no preview"; return true; }
        o[kOptsPaused] = arg == "pause" ? 1 : 0;
        if (g_clock) *reinterpret_cast<int*>(o + kOptsClock) = int(g_clock());
        update(g_widget);
        reply = "preview " + arg;
        return true;
    }
    if (arg == "adsexport")   // test harness: each ADS anim's export, its tracks and their last key
    {
        reply.clear();
        for (auto& [name, c] : g_exports)
        {
            reply += name + ": " + (c.ok ? std::to_string(c.tracks.size()) + " tracks from " + c.path : "none (" + c.why + ")") + "\n";
            for (auto& [bone, keys] : c.tracks)
            {
                const ba::previewfix::AdsKey& k = keys.back();
                char b[200]; snprintf(b, sizeof b, "  %s %zu keys, last q %.3f %.3f %.3f %.3f v %.2f %.2f %.2f\n", bone.c_str(), keys.size(), k.q[0], k.q[1], k.q[2], k.q[3], k.v[0], k.v[1], k.v[2]);
                reply += b;
            }
        }
        return true;
    }
    if (arg.compare(0, 6, "bones ") == 0)   // test harness: bones of entry <i>, * = moved by the current anim
    {
        uint8_t* e = Entries();
        const int i = atoi(arg.c_str() + 6);
        void* anim = e && *reinterpret_cast<uint8_t**>(e + kEntryHolder) ? *reinterpret_cast<void**>(*reinterpret_cast<uint8_t**>(e + kEntryHolder) + kInstAnim) : nullptr;
        reply = e && i >= 0 && i < 32 ? ba::previewfix::BoneList(e + size_t(i) * kEntryStride, anim) : std::string("bones <entry index>");
        return true;
    }
    if (arg == "parents") { reply = ba::previewfix::BoneParents(Entries()); return true; }   // test harness: the arms' first bones and their parent field
    if (arg == "bones")   // test harness: the arms' and the gun's bones, * = moved by the current anim
    {
        uint8_t* e = Entries();
        void* anim = e && *reinterpret_cast<uint8_t**>(e + kEntryHolder) ? *reinterpret_cast<void**>(*reinterpret_cast<uint8_t**>(e + kEntryHolder) + kInstAnim) : nullptr;
        reply = "model: " + ba::previewfix::BoneList(e, anim) + "\ngun: " + (GunEntry() ? ba::previewfix::BoneList(GunEntry(), anim) : std::string("none"));
        return true;
    }
    if (arg == "shells")   // test harness: each shell entry, its state and where APE has its model
    {
        uint8_t* e = Entries();
        char b[256];
        reply.clear();
        for (const ba::fxplayer::ModelPart& mp : ba::fxplayer::Models())
        {
            snprintf(b, sizeof b, "particle %s at %.1f %.1f %.1f\n", mp.model.c_str(), mp.x.o[0], mp.x.o[1], mp.x.o[2]);
            reply += b;
        }
        for (const Shell& sh : g_shells)
        {
            uint8_t* inst = *reinterpret_cast<uint8_t**>(sh.entry + kEntryHolder);
            const float* o = inst ? reinterpret_cast<const float*>(inst + 16) : nullptr;
            snprintf(b, sizeof b, "entry %d %s used %d enabled %d flags %u inst %p origin %.1f %.1f %.1f scale %.2f\n", e ? int((sh.entry - e) / kEntryStride) : -1, sh.model.c_str(), sh.used ? 1 : 0,
                     sh.entry[kEntryEnabled], *reinterpret_cast<uint32_t*>(sh.entry + 4), inst, o ? o[0] : 0, o ? o[1] : 0, o ? o[2] : 0, inst ? *reinterpret_cast<const float*>(inst + 64) : 0.0f);
            reply += b;
        }
        if (reply.empty()) reply = "no shells";
        return true;
    }
    if (arg == "fxprobe" || arg == "fxprobe brass")   // test harness: the scene depth where the flash (or brass) anchor projects, read at the next Present
    {
        ba::fxplayer::Xform x{};
        if (FlashAnchor(x)) ba::fxplayer::SetAnchor(0, x);
        if (BrassAnchor(x)) ba::fxplayer::SetAnchor(1, x);
        ba::fxplayer::ProbeDepth(arg == "fxprobe" ? 0 : 1);
        reply = "probe queued";
        return true;
    }
    if (arg.compare(0, 4, "cam ") == 0)   // test harness: cam <x> <y> <z> <pitch> <yaw>
    {
        float v[5] = {};
        if (sscanf(arg.c_str() + 4, "%f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4]) != 5 || !g_opts || !*g_opts) { reply = "cam <x> <y> <z> <pitch> <yaw>"; return true; }
        float* org = reinterpret_cast<float*>(*g_opts + kOptsCamOrigin);
        float* ang = reinterpret_cast<float*>(*g_opts + kOptsCamAngles);
        org[0] = v[0]; org[1] = v[1]; org[2] = v[2]; ang[0] = v[3]; ang[1] = v[4]; ang[2] = 0;
        reply = "camera set";
        return true;
    }
    if (arg == "entries")   // test harness: each entry with a model: its anim, the anim name APE times it by, its play position
    {
        uint8_t* e = Entries();
        char b[300];
        reply.clear();
        for (int i = 0; e && i < 32; ++i)
        {
            uint8_t* entry = e + size_t(i) * kEntryStride;
            uint8_t* inst = *reinterpret_cast<uint8_t**>(entry + kEntryHolder);
            if (!inst || !entry[kEntryEnabled]) continue;
            const int idx = *reinterpret_cast<int*>(entry + kEntryAnimIndex);
            const std::string nm = idx >= 0 && idx < 6 ? ReadStr(entry + kEntryAnimNames + size_t(idx) * 32) : std::string("?");
            const float* o = reinterpret_cast<const float*>(inst + 16);
            snprintf(b, sizeof b, "entry %d model '%s' anim %p name[%d] '%s' t %.3f origin %.2f %.2f %.2f\n", i, reinterpret_cast<const char*>(entry + 8), *reinterpret_cast<void**>(inst + kInstAnim), idx, nm.c_str(),
                     *reinterpret_cast<float*>(inst + 224), o[0], o[1], o[2]);
            reply += b;
        }
        return true;
    }
    if (arg == "livecheck")   // test harness: live grid values that differ from gdt.db (an unedited asset should list none)
    {
        RefreshLive();
        int same = 0, diff = 0;
        reply.clear();
        for (const auto& [k, v] : g_live)
        {
            const std::string g = Field(g_weapon, k.c_str());
            const bool eq = g == v || (!g.empty() && !v.empty() && strtod(g.c_str(), nullptr) == strtod(v.c_str(), nullptr) && isdigit(static_cast<unsigned char>(v.back())));
            if (eq) { ++same; continue; }
            if (++diff <= 25) reply += k + ": grid '" + v + "' gdt '" + g + "'\n";
        }
        reply += std::to_string(same) + " same, " + std::to_string(diff) + " differ";
        return true;
    }
    if (arg.compare(0, 6, "field ") == 0) { RefreshLive(); reply = g_liveNote + ", " + std::to_string(g_live.size()) + " live values, " + arg.substr(6) + " = '" + W(arg.substr(6).c_str()) + "'"; return true; }   // test harness: a weapon field as the preview reads it
    if (arg == "fxdepth") { reply = ba::fxplayer::DepthReport(); return true; }
    if (arg == "fxdump") { reply = ba::fxplayer::Dump(); return true; }
    if (arg == "raise")   // test harness: the Preview dock's tab to the front
    {
        HMODULE qw = GetModuleHandleW(L"Qt5Widgets64r.dll");
        auto raise = qw ? reinterpret_cast<void (*)(void*)>(GetProcAddress(qw, "?raise@QWidget@@QEAAXXZ")) : nullptr;
        ModToolsWidget dock = rp::host->ui ? rp::host->ui->FindEditorWidget("PreviewToolBar") : nullptr;
        if (dock) rp::host->ui->SetVisible(dock, 1);
        if (raise && dock) raise(dock);
        reply = dock && raise ? "preview raised" : "no preview dock";
        return true;
    }
    if (arg.compare(0, 4, "set ") == 0)   // set arms|body|idle3p|fire3p|reload3p <value>, as typed in the bar
    {
        const std::string rest = arg.substr(4);
        const size_t sp = rest.find(' ');
        const std::string key = rest.substr(0, sp), value = sp == std::string::npos ? std::string() : rest.substr(sp + 1);
        if (key == "prefix")   // test harness: the 3P anim prefix for this weapon's Player Anim Type
        {
            rp::ConfigSet("weaponpreview", ("prefix_" + g_tp.type).c_str(), value);
            ResolveTp();
            Rebuild();
            reply = "prefix_" + g_tp.type + " = " + value + "; found [" + g_tp.found + "]";
            return true;
        }
        std::string* f = key == "arms" ? &g_set.arms : key == "body" ? &g_set.body : key == "idle3p" ? &g_set.tpIdle : key == "fire3p" ? &g_set.tpFire : key == "reload3p" ? &g_set.tpReload : nullptr;
        if (!f) { reply = "set arms|body|idle3p|fire3p|reload3p <value>"; return true; }
        *f = value;
        SaveSettings();
        ModToolsWidget w = f == &g_set.arms ? g_bar.arms : f == &g_set.body ? g_bar.body : f == &g_set.tpIdle ? g_bar.tpIdle : f == &g_set.tpFire ? g_bar.tpFire : g_bar.tpReload;
        if (w) rp::host->ui->SetEditText(w, value.c_str());
        Rebuild();
        reply = key + " = " + value;
        return true;
    }
    char line[512];
    uint8_t* o = g_opts ? *g_opts : nullptr;
    const float* org = o ? reinterpret_cast<const float*>(o + kOptsCamOrigin) : nullptr;
    const float* ang = o ? reinterpret_cast<const float*>(o + kOptsCamAngles) : nullptr;
    snprintf(line, sizeof line, "weaponpreview %s%s; weapon '%s' (%s), %s, action '%s' step %zu/%zu%s, scene anim '%s', field overrides answered %u, bar %s\n"
             "camera origin %.1f %.1f %.1f angles %.1f %.1f %.1f; time %.3f s, length %.3f s\n",
             g_on ? "on" : "off", g_broken ? " (BROKEN: exception, reload the plugin)" : "", g_weaponName.c_str(), g_weaponType.c_str(), g_firstPerson ? "1P" : "3P", g_action.c_str(), g_step, g_steps.size(),
             g_atEnd ? " (at end)" : "", g_sceneAnim.c_str(), g_overHits, g_bar.root ? "shown" : (g_bar.tried ? "FAILED" : "not built"),
             org ? org[0] : 0, org ? org[1] : 0, org ? org[2] : 0, ang ? ang[0] : 0, ang ? ang[1] : 0, ang ? ang[2] : 0,
             o ? *reinterpret_cast<double*>(o + kOptsTime) : -1.0, o ? double(LengthSafe(o + kEntries)) : -1.0);
    reply = line;
    reply += "arms '" + g_set.arms + "', body '" + g_set.body + "', 3P idle '" + g_set.tpIdle + "', 3P fire '" + g_set.tpFire + "', 3P reload '" + g_set.tpReload + "'\n";
    if (!g_status.empty()) reply += "status: " + g_status + "\n";
    reply += "dispatches " + std::to_string(g_dispatches) + ", last type " + std::to_string(g_lastType) + ", weapon types " + std::to_string(g_types[0]) + " " + std::to_string(g_types[1]) + " " + std::to_string(g_types[2]) + " " + std::to_string(g_types[3]) + ", log " + g_dispLog + "\n";
    {
        char b[256];
        uint8_t* gun = GunEntry();
        snprintf(b, sizeof b, "gun entry %d, flash anchor from %s, brass anchor from %s\n", gun && Entries() ? int((gun - Entries()) / kEntryStride) : -1, g_flashTag.c_str(), g_brassTag.c_str());
        reply += b;
    }
    reply += "3P set: Player Anim Type '" + g_tp.type + "', prefix '" + g_tp.prefix + "', found [" + g_tp.found + "], missing [" + g_tp.missing + "]; shell entries " + std::to_string(g_shells.size()) + "\n";
    reply += ba::fxplayer::Status() + "\n";
    return true;
}
}
