// A small .efx player for APE's preview, see fxplayer.h.
// Simulation and the sprite list on APE's main thread after each preview frame, drawing in IDXGISwapChain::Present.
#include "modtools_plugin.hpp"
#include "fxplayer.h"
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#pragma comment(lib, "ole32.lib")

namespace ba::fxplayer
{
namespace
{
    std::string Lower(std::string s) { for (char& c : s) c = char(tolower(static_cast<unsigned char>(c))); return s; }
    std::mt19937 g_rng{std::random_device{}()};
    float Rand01() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(g_rng); }
    float Range(const float* v) { return v[0] + v[1] * Rand01(); }   // "<base> <rand>": base + rand(0..range)

    // ---- .efx text: tokens, then per emitter key -> items (a token or a brace group) ----
    struct Item { std::string tok; std::vector<Item> group; bool isGroup = false; };
    using Emitter = std::unordered_map<std::string, std::vector<Item>>;

    std::vector<std::string> Tokens(const std::string& t)
    {
        std::vector<std::string> out;
        for (size_t i = 0; i < t.size();)
        {
            const char c = t[i];
            if (isspace(static_cast<unsigned char>(c))) { ++i; continue; }
            if (c == '/' && i + 1 < t.size() && t[i + 1] == '/') { while (i < t.size() && t[i] != '\n') ++i; continue; }
            if (c == '{' || c == '}' || c == ';') { out.push_back(std::string(1, c)); ++i; continue; }
            if (c == '"') { size_t j = t.find('"', i + 1); if (j == std::string::npos) j = t.size(); out.push_back("\"" + t.substr(i + 1, j - i - 1)); i = j + 1; continue; }
            size_t j = i;
            while (j < t.size() && !isspace(static_cast<unsigned char>(t[j])) && t[j] != '{' && t[j] != '}' && t[j] != ';') ++j;
            out.push_back(t.substr(i, j - i));
            i = j;
        }
        return out;
    }

    // items until the statement end or the closing brace of the current group
    std::vector<Item> Items(const std::vector<std::string>& tk, size_t& i, bool inGroup)
    {
        std::vector<Item> out;
        while (i < tk.size())
        {
            const std::string& s = tk[i];
            if (s == ";") { if (!inGroup) { ++i; return out; } ++i; continue; }
            if (s == "}") { if (inGroup) ++i; return out; }
            if (s == "{") { ++i; Item g; g.isGroup = true; g.group = Items(tk, i, true); out.push_back(std::move(g)); continue; }
            Item it; it.tok = s; out.push_back(std::move(it)); ++i;
        }
        return out;
    }

    // ---- an element, interpreted ----
    struct Curve { std::vector<float> k; int width = 2; };   // keys of (t, v...) rows
    struct Graph { float scale = 0; Curve a, b; bool set = false; };
    enum VisType { kNone, kBillboard, kRotated, kOriented, kLine, kTail, kRunner, kCloud, kModel, kOther };

    struct Elem
    {
        std::string name;
        bool eventOnly = false;          // playOnTouch, playOnDeath, playOnRun: spawned by another element's event, not at start
        bool looping = false, relEffect = true, nonUniform = false, randVel0 = false, randVel1 = false, randSize0 = false, randSize1 = false;
        bool randColor = false, randAlpha = false, randRot = false, randScale = false, sphere = false, cylinder = false;
        float delay[2] = {0, 0}, life[2] = {1000, 0}, loopInterval[2] = {100, 0}, loopCount[2] = {1, 0}, oneShot[2] = {1, 0};
        float org[3][2] = {}, offRadius[2] = {}, offHeight[2] = {}, angle[3][2] = {}, angleVel[3][2] = {}, initialRot[2] = {}, gravity[2] = {};
        int cylAxis = 0;
        Graph vel0[3], vel1[3], rot, size0, size1, scale, color, alpha;
        int atlasCols = 1, atlasRows = 1, atlasCount = 1, atlasIndex = 0, atlasFps = 0, atlasLoops = 0;
        bool atlasRandom = false, atlasOverLife = false;
        VisType vis = kNone;
        std::vector<std::string> visuals;   // materials or runner effects
    };
    struct Effect { std::vector<Elem> elems; std::string error; };

    void ReadPair(const std::vector<Item>& v, float* out)
    {
        for (size_t i = 0; i < v.size() && i < 2; ++i) if (!v[i].isGroup) out[i] = float(atof(v[i].tok.c_str()));
    }
    Curve ReadCurve(const Item& g, int width)
    {
        Curve c; c.width = width;
        for (const Item& it : g.group) if (!it.isGroup) c.k.push_back(float(atof(it.tok.c_str())));
        if (c.k.size() % size_t(width)) c.k.resize(c.k.size() - c.k.size() % size_t(width));
        return c;
    }
    Graph ReadGraph(const std::vector<Item>& v, int width)
    {
        Graph g;
        if (v.empty() || v[0].isGroup) return g;
        g.scale = float(atof(v[0].tok.c_str()));
        const Item* outer = v.size() > 1 && v[1].isGroup ? &v[1] : nullptr;
        if (!outer) return g;
        std::vector<const Item*> curves;
        for (const Item& it : outer->group) if (it.isGroup) curves.push_back(&it);
        if (curves.empty()) return g;
        g.a = ReadCurve(*curves[0], width);
        g.b = curves.size() > 1 ? ReadCurve(*curves[1], width) : g.a;
        g.set = !g.a.k.empty();
        return g;
    }
    // value column `col` (1..width-1) of a curve at t (0..1), piecewise linear
    float Sample(const Curve& c, float t, int col)
    {
        const size_t n = c.k.size() / size_t(c.width);
        if (!n) return 0;
        const float* k = c.k.data();
        if (t <= k[0]) return k[col];
        for (size_t i = 1; i < n; ++i)
        {
            const float* p = k + (i - 1) * c.width; const float* q = k + i * c.width;
            if (t <= q[0]) { const float span = q[0] - p[0]; const float u = span > 1e-6f ? (t - p[0]) / span : 1.0f; return p[col] + (q[col] - p[col]) * u; }
        }
        return k[(n - 1) * c.width + col];
    }
    float Eval(const Graph& g, float t, float r, int col = 1)
    {
        if (!g.set) return 0;
        const float a = Sample(g.a, t, col);
        return g.scale * (r >= 0 ? a + (Sample(g.b, t, col) - a) * r : a);
    }

    bool Has(const std::vector<Item>& v, const char* word)
    {
        for (const Item& it : v) if (!it.isGroup && it.tok == word) return true;
        return false;
    }

    Elem ReadElem(const Emitter& e)
    {
        Elem el;
        auto get = [&](const char* k) -> const std::vector<Item>* { auto it = e.find(k); return it == e.end() ? nullptr : &it->second; };
        if (auto v = get("name"); v && !v->empty()) el.name = (*v)[0].tok.substr((*v)[0].tok[0] == '"' ? 1 : 0);
        if (auto v = get("editorFlags"))
        {
            el.looping = Has(*v, "looping"); el.randVel0 = Has(*v, "useRandVel0"); el.randVel1 = Has(*v, "useRandVel1");
            el.randSize0 = Has(*v, "useRandSize0"); el.randSize1 = Has(*v, "useRandSize1"); el.randColor = Has(*v, "useRandColor");
            el.randAlpha = Has(*v, "useRandAlpha"); el.randRot = Has(*v, "useRandRotDelta"); el.randScale = Has(*v, "useRandScale");
            el.eventOnly = Has(*v, "playOnTouch") || Has(*v, "playOnDeath") || Has(*v, "playOnRun");
        }
        if (auto v = get("flags"))
        {
            el.relEffect = !(Has(*v, "runRelToSpawn") || Has(*v, "runRelToWorld"));
            el.nonUniform = Has(*v, "nonUniformScale"); el.sphere = Has(*v, "spawnOffsetSphere"); el.cylinder = Has(*v, "spawnOffsetCylinder");
        }
        auto pair = [&](const char* k, float* out) { if (auto v = get(k)) ReadPair(*v, out); };
        pair("spawnDelayMsec", el.delay); pair("lifeSpanMsec", el.life); pair("spawnLooping", el.loopInterval);
        pair("spawnLoopingSpawnCount", el.loopCount); pair("spawnOneShot", el.oneShot);
        pair("spawnOrgX", el.org[0]); pair("spawnOrgY", el.org[1]); pair("spawnOrgZ", el.org[2]);
        pair("spawnOffsetRadius", el.offRadius); pair("spawnOffsetHeight", el.offHeight);
        pair("spawnAnglePitch", el.angle[0]); pair("spawnAngleYaw", el.angle[1]); pair("spawnAngleRoll", el.angle[2]);
        pair("angleVelPitch", el.angleVel[0]); pair("angleVelYaw", el.angleVel[1]); pair("angleVelRoll", el.angleVel[2]);
        pair("initialRot", el.initialRot); pair("gravity", el.gravity);
        if (auto v = get("spawnOffsetCylindricalAxis"); v && !v->empty()) el.cylAxis = std::clamp(atoi((*v)[0].tok.c_str()), 0, 2);
        const char* vk[3] = {"X", "Y", "Z"};
        for (int i = 0; i < 3; ++i)
        {
            if (auto v = get((std::string("velGraph0") + vk[i]).c_str())) el.vel0[i] = ReadGraph(*v, 2);
            if (auto v = get((std::string("velGraph1") + vk[i]).c_str())) el.vel1[i] = ReadGraph(*v, 2);
        }
        if (auto v = get("rotGraph")) el.rot = ReadGraph(*v, 2);
        if (auto v = get("sizeGraph0")) el.size0 = ReadGraph(*v, 2);
        if (auto v = get("sizeGraph1")) el.size1 = ReadGraph(*v, 2);
        if (auto v = get("scaleGraph")) el.scale = ReadGraph(*v, 2);
        if (auto v = get("colorGraph")) el.color = ReadGraph(*v, 4);
        if (auto v = get("alphaGraph")) el.alpha = ReadGraph(*v, 2);
        auto num = [&](const char* k, int def) { auto v = get(k); return v && !v->empty() && !(*v)[0].isGroup ? atoi((*v)[0].tok.c_str()) : def; };
        el.atlasCols = 1 << std::clamp(num("atlasColIndexBits", 0), 0, 8);
        el.atlasRows = 1 << std::clamp(num("atlasRowIndexBits", 0), 0, 8);
        el.atlasCount = std::max(1, num("atlasEntryCount", 1));
        el.atlasIndex = num("atlasIndex", 0); el.atlasFps = num("atlasFps", 0); el.atlasLoops = num("atlasLoopCount", 0);
        if (auto v = get("atlasBehavior")) { el.atlasRandom = Has(*v, "startRandom"); el.atlasOverLife = Has(*v, "playOverLife"); }
        static const std::pair<const char*, VisType> kVis[] = {{"billboardSprite", kBillboard}, {"rotatedSprite", kRotated}, {"orientedSprite", kOriented},
            {"line", kLine}, {"tail", kTail}, {"runner", kRunner}, {"cloud", kCloud}, {"model", kModel}, {"dynamicLight2", kOther}, {"dynamicLight", kOther},
            {"light", kOther}, {"lensFlare", kOther}, {"decal", kOther}, {"trail", kOther}, {"beamSource", kOther}, {"beamTarget", kOther}};
        for (const auto& [k, t] : kVis)
        {
            auto v = get(k);
            if (!v) continue;
            el.vis = t;
            for (const Item& it : *v)
                if (it.isGroup)
                    for (const Item& s : it.group) if (!s.isGroup && !s.tok.empty() && s.tok[0] == '"') el.visuals.push_back(s.tok.substr(1));
            break;
        }
        return el;
    }

    std::string g_root;
    std::unordered_map<std::string, std::shared_ptr<Effect>> g_effects;
    std::vector<std::string> g_told;

    void Note(const std::string& text)
    {
        if (std::find(g_told.begin(), g_told.end(), text) != g_told.end()) return;
        g_told.push_back(text);
        rp::Log("fxplayer: %s", text.c_str());
        if (rp::Ape()) rp::Ape()->Print(rp::self, ("Weapon preview fx: " + text).c_str());
    }

    std::string Normalize(std::string p)
    {
        std::string out;
        for (char c : p) { if (c == '/') c = '\\'; if (c == '\\' && !out.empty() && out.back() == '\\') continue; out += c; }
        return out;
    }

    std::shared_ptr<Effect> Load(const std::string& rel)
    {
        const std::string key = Lower(Normalize(rel));
        auto it = g_effects.find(key);
        if (it != g_effects.end()) return it->second;
        auto fx = std::make_shared<Effect>();
        g_effects[key] = fx;
        std::ifstream f(g_root + "\\share\\raw\\" + key, std::ios::binary);
        if (!f) { fx->error = "share\\raw\\" + key + " not found"; Note(fx->error); return fx; }
        std::stringstream ss; ss << f.rdbuf();
        const std::vector<std::string> tk = Tokens(ss.str());
        size_t i = 0;
        while (i < tk.size() && tk[i] != "{") ++i;   // header and the iwfx 3 global block
        while (i < tk.size())
        {
            if (tk[i] != "{") { ++i; continue; }
            ++i;
            Emitter e;
            while (i < tk.size() && tk[i] != "}")
            {
                const std::string k = tk[i++];
                e[k] = Items(tk, i, false);
            }
            ++i;
            fx->elems.push_back(ReadElem(e));
        }
        return fx;
    }

    // ---- materials -> textures (WIC decode on the main thread, GPU upload at Present) ----
    struct Tex { std::vector<uint8_t> rgba; UINT w = 0, h = 0; bool additive = false; float boost = 1; int cols = 0, rows = 0; ID3D11ShaderResourceView* srv = nullptr; bool failed = false; };
    std::unordered_map<std::string, std::shared_ptr<Tex>> g_tex;
    Resolver g_resolver = nullptr;

    bool Decode(const std::string& path, Tex& t)
    {
        IWICImagingFactory* f = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) || !f) return false;
        bool ok = false;
        std::wstring w(path.begin(), path.end());
        IWICBitmapDecoder* d = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
        if (SUCCEEDED(f->CreateDecoderFromFilename(w.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &d)) && SUCCEEDED(d->GetFrame(0, &fr))
            && SUCCEEDED(f->CreateFormatConverter(&cv)) && SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))
            && SUCCEEDED(cv->GetSize(&t.w, &t.h)) && t.w && t.h && t.w <= 8192 && t.h <= 8192)
        {
            t.rgba.resize(size_t(t.w) * t.h * 4);
            ok = SUCCEEDED(cv->CopyPixels(nullptr, t.w * 4, UINT(t.rgba.size()), t.rgba.data()));
        }
        if (cv) cv->Release(); if (fr) fr->Release(); if (d) d->Release();
        f->Release();
        return ok;
    }

    std::shared_ptr<Tex> Material(const std::string& name)
    {
        auto it = g_tex.find(name);
        if (it != g_tex.end()) return it->second;
        auto t = std::make_shared<Tex>();
        g_tex[name] = t;
        std::string image, type;
        if (!g_resolver || !g_resolver(name, image, type, t->cols, t->rows)) { Note("material " + name + " has no colorMap image in gdt.db, drawn white"); }
        else if (!Decode(g_root + "\\" + Normalize(image), *t)) { Note("material " + name + ": " + image + " could not be read, drawn white"); t->rgba.clear(); }
        const std::string lt = Lower(type);
        // emissive effect materials are HDR bright in game, additive comes closest in the preview's LDR frame
        t->additive = lt.find("add") != std::string::npos || lt.find("screen") != std::string::npos || lt.find("emissive") != std::string::npos;
        t->boost = lt.find("emissive") != std::string::npos ? 3.0f : 1.0f;   // the game's HDR emissive saturates toward white
        if (t->rgba.empty()) { t->rgba.assign(16, 255); t->w = t->h = 2; }
        return t;
    }

    // ---- instances and particles ----
    void AnglesAxis(float pitch, float yaw, float roll, float* m)   // rows forward, left, up (com_math AnglesToAxis)
    {
        const float d = 0.017453292f;
        const float sp = sinf(pitch * d), cp = cosf(pitch * d), sy = sinf(yaw * d), cy = cosf(yaw * d), sr = sinf(roll * d), cr = cosf(roll * d);
        m[0] = cp * cy; m[1] = cp * sy; m[2] = -sp;
        m[3] = sr * sp * cy - cr * sy; m[4] = sr * sp * sy + cr * cy; m[5] = sr * cp;
        m[6] = cr * sp * cy + sr * sy; m[7] = cr * sp * sy - sr * cy; m[8] = cr * cp;
    }
    void Apply(const Xform& x, const float* p, float* out) { for (int j = 0; j < 3; ++j) out[j] = x.o[j] + p[0] * x.a[j] + p[1] * x.a[3 + j] + p[2] * x.a[6 + j]; }
    void ApplyDir(const Xform& x, const float* p, float* out) { for (int j = 0; j < 3; ++j) out[j] = p[0] * x.a[j] + p[1] * x.a[3 + j] + p[2] * x.a[6 + j]; }
    Xform Compose(const Xform& local, const Xform& parent)   // local in parent space -> world
    {
        Xform r{};
        Apply(parent, local.o, r.o);
        for (int i = 0; i < 3; ++i) ApplyDir(parent, local.a + i * 3, r.a + i * 3);
        return r;
    }
    Xform Identity() { Xform x{}; x.a[0] = x.a[4] = x.a[8] = 1; return x; }

    struct Inst
    {
        std::shared_ptr<Effect> fx;
        int anchor = 0;
        Xform local = Identity();       // in anchor space (a runner's child)
        double age = 0;
        std::vector<double> next;       // per elem: next looping spawn, or <0 when done
        std::vector<double> delay;
        bool alive = true;
    };
    struct Part
    {
        const Elem* el = nullptr;
        std::shared_ptr<Effect> keep;
        int anchor = 0;
        Xform local = Identity();       // the instance's frame in anchor space
        Xform spawn{};                  // world frame at spawn (runRelToSpawn / World)
        double age = 0, life = 1;
        float pos[3] = {}, rnd[8] = {}, roll = 0, angles[3] = {}, angVel[3] = {}, grav = 0;
        int frame0 = 0;
        std::shared_ptr<Tex> tex;
        const std::string* model = nullptr;   // a model element's xmodel (in el.visuals, kept by keep)
    };
    std::vector<Inst> g_insts;
    std::vector<Inst> g_newInsts;       // runner children, joined after the step (g_insts must not grow while it is walked)
    std::vector<Part> g_parts;
    bool g_stepping = false, g_stopPending = false;   // Play and StopAll during Step: staged or deferred
    Xform g_anchors[4] = {Identity(), Identity(), Identity(), Identity()};
    LARGE_INTEGER g_last{}, g_freq{};
    double g_timeScale = 1.0;           // test harness: slow motion
    uint64_t g_played = 0, g_spawned = 0, g_drawn = 0, g_presents = 0;
    const double kLoopFor = 0.25;       // a looping element of a shot effect spawns this long

    void Spawn(Inst& in, const Elem& el, int count);

    void StartInst(const std::shared_ptr<Effect>& fx, int anchor, const Xform& local)
    {
        if (!fx || fx->elems.empty() || g_insts.size() + g_newInsts.size() > 64) return;
        Inst in; in.fx = fx; in.anchor = anchor; in.local = local;
        for (const Elem& el : fx->elems) { in.delay.push_back(Range(el.delay) / 1000.0); in.next.push_back(el.eventOnly ? -1 : 0); }
        g_newInsts.push_back(std::move(in));
    }

    void Spawn(Inst& in, const Elem& el, int count)
    {
        for (int n = 0; n < count && g_parts.size() < 4096; ++n)
        {
            Part p; p.el = &el; p.keep = in.fx; p.anchor = in.anchor; p.local = in.local;
            p.life = std::max(0.001, double(Range(el.life)) / 1000.0);
            for (float& r : p.rnd) r = Rand01();
            for (int i = 0; i < 3; ++i) p.pos[i] = Range(el.org[i]);
            if (el.sphere || el.cylinder)
            {
                const float radius = Range(el.offRadius);
                if (el.sphere)
                {
                    const float z = Rand01() * 2 - 1, a = Rand01() * 6.2831853f, s = sqrtf(std::max(0.0f, 1 - z * z));
                    p.pos[0] += radius * s * cosf(a); p.pos[1] += radius * s * sinf(a); p.pos[2] += radius * z;
                }
                else
                {
                    const float a = Rand01() * 6.2831853f;
                    const int h = el.cylAxis, u = (h + 1) % 3, v = (h + 2) % 3;
                    p.pos[u] += radius * cosf(a); p.pos[v] += radius * sinf(a); p.pos[h] += Range(el.offHeight);
                }
            }
            for (int i = 0; i < 3; ++i) { p.angles[i] = Range(el.angle[i]); p.angVel[i] = Range(el.angleVel[i]); }
            p.roll = Range(el.initialRot);
            p.frame0 = el.atlasRandom ? int(Rand01() * el.atlasCount) % el.atlasCount : el.atlasIndex % el.atlasCount;
            p.spawn = Compose(in.local, g_anchors[in.anchor & 3]);
            if (el.vis == kRunner)
            {
                if (!el.visuals.empty())
                {
                    const std::string& child = el.visuals[size_t(Rand01() * el.visuals.size()) % el.visuals.size()];
                    Xform l{}; AnglesAxis(p.angles[0], p.angles[1], p.angles[2], l.a);
                    memcpy(l.o, p.pos, sizeof l.o);
                    StartInst(Load("fx\\" + child + ".efx"), in.anchor, Compose(l, in.local));
                }
                continue;
            }
            p.grav = Range(el.gravity) * 8.0f;   // percent of the game's 800 units/s2
            if (el.vis == kModel && !el.visuals.empty())
            {
                p.model = &el.visuals[size_t(Rand01() * el.visuals.size()) % el.visuals.size()];
                g_parts.push_back(std::move(p));
                ++g_spawned;
                continue;
            }
            if (el.vis == kOther || el.vis == kNone || el.visuals.empty()) continue;
            p.tex = Material(el.visuals[size_t(Rand01() * el.visuals.size()) % el.visuals.size()]);
            g_parts.push_back(std::move(p));
            ++g_spawned;
        }
    }

    void Step(double dt)
    {
        g_stepping = true;
        for (size_t k = 0; k < g_insts.size(); ++k)
        {
            Inst& in = g_insts[k];
            in.age += dt;
            bool pending = false;
            for (size_t e = 0; e < in.fx->elems.size(); ++e)
            {
                const Elem& el = in.fx->elems[e];
                if (in.next[e] < 0) continue;
                if (in.age < in.delay[e]) { pending = true; continue; }
                if (!el.looping) { Spawn(in, el, std::max(0, int(std::lround(Range(el.oneShot))))); in.next[e] = -1; continue; }
                const double since = in.age - in.delay[e];
                if (since > kLoopFor) { in.next[e] = -1; continue; }
                pending = true;
                while (in.next[e] <= since)
                {
                    Spawn(in, el, std::max(1, int(std::lround(Range(el.loopCount)))));
                    in.next[e] += std::max(0.005, double(Range(el.loopInterval)) / 1000.0);
                }
            }
            if (!pending) in.alive = false;
        }
        g_insts.erase(std::remove_if(g_insts.begin(), g_insts.end(), [](const Inst& i) { return !i.alive; }), g_insts.end());
        for (Inst& n : g_newInsts) g_insts.push_back(std::move(n));
        g_newInsts.clear();
        for (Part& p : g_parts)
        {
            p.age += dt;
            const float t = float(std::min(1.0, p.age / p.life));
            const Elem& el = *p.el;
            for (int i = 0; i < 3; ++i)
                p.pos[i] += float(dt) * (Eval(el.vel0[i], t, el.randVel0 ? p.rnd[0] : -1) + Eval(el.vel1[i], t, el.randVel1 ? p.rnd[1] : -1));
            p.roll += float(dt) * Eval(el.rot, t, el.randRot ? p.rnd[2] : -1);
            for (int i = 0; i < 3; ++i) p.angles[i] += float(dt) * p.angVel[i];
        }
        g_parts.erase(std::remove_if(g_parts.begin(), g_parts.end(), [](const Part& p) { return p.age >= p.life; }), g_parts.end());
        g_stepping = false;
        if (g_stopPending) { g_stopPending = false; g_insts.clear(); g_newInsts.clear(); g_parts.clear(); }
    }

    // ---- the sprite list ----
    struct Vtx { float x, y, u, v, r, g, b, a, z; };   // z: view depth, tested against the scene depth
    struct Batch { std::shared_ptr<Tex> tex; bool additive; UINT first, count; };
    struct Cam { float o[3], f[3], r[3], u[3], ty = 0.4f, tx = 0.7f; HWND hwnd = nullptr; bool set = false; } g_cam;
    SRWLOCK g_drawLock = SRWLOCK_INIT;   // g_verts, g_batches, g_drawWnd, g_drawStamp (Present thread vs main thread)
    std::vector<Vtx> g_verts;           // under g_drawLock
    std::vector<Batch> g_batches;
    HWND g_drawWnd = nullptr;
    ULONGLONG g_drawStamp = 0;

    float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    bool Project(const float* w, float& x, float& y, float& z)
    {
        const float d[3] = {w[0] - g_cam.o[0], w[1] - g_cam.o[1], w[2] - g_cam.o[2]};
        z = Dot(d, g_cam.f);
        if (z < 0.25f) return false;
        x = Dot(d, g_cam.r) / (z * g_cam.tx);
        y = Dot(d, g_cam.u) / (z * g_cam.ty);
        return true;
    }

    struct Sprite { float z; std::shared_ptr<Tex> tex; Vtx v[6]; };

    void Quad(Sprite& s, const float* c0, const float* c1, const float* c2, const float* c3, const float* uv, const float* col)
    {
        const float* cs[4] = {c0, c1, c2, c3};
        const float us[4][2] = {{uv[0], uv[3]}, {uv[2], uv[3]}, {uv[2], uv[1]}, {uv[0], uv[1]}};
        const int idx[6] = {0, 1, 2, 0, 2, 3};
        for (int i = 0; i < 6; ++i)
        {
            const int k = idx[i];
            s.v[i] = Vtx{cs[k][0], cs[k][1], us[k][0], us[k][1], col[0], col[1], col[2], col[3], 0.0f};
        }
    }

    std::vector<ModelPart> g_models;    // main thread
    void Build()
    {
        std::vector<ModelPart> models;
        std::vector<Sprite> sprites;
        sprites.reserve(g_parts.size());
        for (const Part& p : g_parts)
        {
            const Elem& el = *p.el;
            const float t = float(std::min(1.0, p.age / p.life));
            const Xform frame = el.relEffect ? Compose(p.local, g_anchors[p.anchor & 3]) : p.spawn;
            float w[3]; Apply(frame, p.pos, w);
            w[2] -= 0.5f * p.grav * float(p.age * p.age);
            if (p.model)
            {
                Xform l{}; AnglesAxis(p.angles[0], p.angles[1], p.angles[2], l.a);
                Xform m = Compose(l, frame);
                memcpy(m.o, w, sizeof w);
                models.push_back(ModelPart{*p.model, m});
                continue;
            }
            float x, y, z;
            if (!Project(w, x, y, z)) continue;
            float scale = el.scale.set && el.scale.scale != 0 ? Eval(el.scale, t, el.randScale ? p.rnd[3] : -1) : 1.0f;
            float s0 = Eval(el.size0, t, el.randSize0 ? p.rnd[4] : -1) * scale;
            float s1 = (el.nonUniform && el.size1.set ? Eval(el.size1, t, el.randSize1 ? p.rnd[5] : -1) : s0 / std::max(1e-6f, scale)) * scale;
            if (!el.nonUniform) s1 = s0;
            float col[4] = {1, 1, 1, 1};
            if (el.color.set) for (int i = 0; i < 3; ++i) col[i] = Eval(el.color, t, el.randColor ? p.rnd[6] : -1, i + 1);
            if (el.alpha.set) col[3] = std::clamp(Eval(el.alpha, t, el.randAlpha ? p.rnd[7] : -1), 0.0f, 1.0f);
            if (p.tex) for (int i = 0; i < 3; ++i) col[i] *= p.tex->boost;
            if (col[3] <= 0.001f || (fabsf(s0) < 1e-4f && fabsf(s1) < 1e-4f)) continue;
            // the material's atlas grid wins over the efx's (efx atlas fields are often stale)
            const bool matGrid = p.tex && (p.tex->cols > 1 || p.tex->rows > 1);
            const int cols = matGrid ? std::max(1, p.tex->cols) : el.atlasCols, rows = matGrid ? std::max(1, p.tex->rows) : el.atlasRows;
            const int count = matGrid ? cols * rows : std::min(el.atlasCount, cols * rows);
            int frameIdx = matGrid && el.atlasRandom ? int(p.rnd[2] * count) : p.frame0;
            if (count > 1)
            {
                if (el.atlasOverLife) frameIdx += int(t * count * std::max(1, el.atlasLoops));
                else if (el.atlasFps > 0) frameIdx += int(p.age * el.atlasFps);
                frameIdx = ((frameIdx % count) + count) % count;
            }
            else frameIdx = 0;
            const float cu = 1.0f / cols, cv = 1.0f / rows;
            const float uv[4] = {(frameIdx % cols) * cu, (frameIdx / cols % rows) * cv, 0, 0};
            const float uvr[4] = {uv[0], uv[1], uv[0] + cu, uv[1] + cv};
            Sprite s; s.z = z; s.tex = p.tex;
            if (el.vis == kLine || el.vis == kTail)
            {
                float dir[3];
                if (el.vis == kTail)
                {
                    float v[3] = {};
                    for (int i = 0; i < 3; ++i) v[i] = Eval(el.vel0[i], t, el.randVel0 ? p.rnd[0] : -1) + Eval(el.vel1[i], t, el.randVel1 ? p.rnd[1] : -1);
                    if (Dot(v, v) < 1e-6f) v[0] = 1;
                    ApplyDir(frame, v, dir);
                }
                else
                {
                    float ax[9]; AnglesAxis(p.angles[0], p.angles[1], p.angles[2], ax);
                    ApplyDir(frame, ax, dir);
                }
                const float n = sqrtf(Dot(dir, dir));
                if (n < 1e-6f) continue;
                for (float& d : dir) d /= n;
                const float half = s1 * 0.5f;
                const float a[3] = {w[0] - dir[0] * half, w[1] - dir[1] * half, w[2] - dir[2] * half};
                const float b[3] = {w[0] + dir[0] * half, w[1] + dir[1] * half, w[2] + dir[2] * half};
                float ax_, ay, az, bx, by, bz;
                if (!Project(a, ax_, ay, az) || !Project(b, bx, by, bz)) continue;
                float px = -(by - ay) * g_cam.ty, py = (bx - ax_) * g_cam.tx;   // perpendicular in view units
                const float pl = sqrtf(px * px + py * py);
                if (pl < 1e-6f) { px = 1; py = 0; } else { px /= pl; py /= pl; }
                const float wa = s0 * 0.5f / az, wb = s0 * 0.5f / bz;
                const float c0[2] = {ax_ - px * wa / g_cam.tx, ay - py * wa / g_cam.ty}, c1[2] = {bx - px * wb / g_cam.tx, by - py * wb / g_cam.ty};
                const float c2[2] = {bx + px * wb / g_cam.tx, by + py * wb / g_cam.ty}, c3[2] = {ax_ + px * wa / g_cam.tx, ay + py * wa / g_cam.ty};
                Quad(s, c0, c1, c2, c3, uvr, col);
            }
            else
            {
                const float rr = p.roll * 0.017453292f, cr = cosf(rr), sr = sinf(rr);
                float c[4][2];
                const float corners[4][2] = {{-s0, -s1}, {s0, -s1}, {s0, s1}, {-s0, s1}};
                for (int i = 0; i < 4; ++i)
                {
                    const float vx = corners[i][0] * cr - corners[i][1] * sr, vy = corners[i][0] * sr + corners[i][1] * cr;
                    c[i][0] = x + vx / (z * g_cam.tx); c[i][1] = y + vy / (z * g_cam.ty);
                }
                Quad(s, c[0], c[1], c[2], c[3], uvr, col);
            }
            for (Vtx& v : s.v) v.z = s.z;
            sprites.push_back(std::move(s));
        }
        std::sort(sprites.begin(), sprites.end(), [](const Sprite& a, const Sprite& b) { return a.z > b.z; });
        std::vector<Vtx> verts; std::vector<Batch> batches;
        verts.reserve(sprites.size() * 6);
        for (const Sprite& s : sprites)
        {
            if (batches.empty() || batches.back().tex != s.tex) batches.push_back(Batch{s.tex, s.tex->additive, UINT(verts.size()), 0});
            verts.insert(verts.end(), s.v, s.v + 6);
            batches.back().count += 6;
        }
        g_models.swap(models);
        AcquireSRWLockExclusive(&g_drawLock);
        g_verts.swap(verts); g_batches.swap(batches);
        g_drawWnd = g_cam.hwnd;
        g_drawStamp = GetTickCount64();
        ReleaseSRWLockExclusive(&g_drawLock);
        g_drawn = sprites.size();
    }

    // ---- drawing at Present ----
    using PresentFn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using Present1Fn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    PresentFn g_origPresent = nullptr;
    Present1Fn g_origPresent1 = nullptr;
    using CompileFn = HRESULT (WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

    struct Gpu
    {
        ID3D11Device* dev = nullptr;
        ID3DDeviceContextState* state = nullptr;
        ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr; ID3D11InputLayout* layout = nullptr;
        ID3D11Buffer* vb = nullptr; UINT vbVerts = 0;
        ID3D11BlendState* blendAlpha = nullptr; ID3D11BlendState* blendAdd = nullptr;
        ID3D11RasterizerState* raster = nullptr; ID3D11DepthStencilState* depth = nullptr; ID3D11SamplerState* sampler = nullptr;
        ID3D11ShaderResourceView* noDepth = nullptr;
        ID3D11ShaderResourceView* sceneSrv = nullptr; ID3D11Texture2D* sceneTex = nullptr;   // view of g_snap (sceneTex = the g_snap it was made for)
        bool failed = false;
    } g_gpu;

    template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
    void ReleaseGpu()
    {
        for (auto& [k, t] : g_tex) Rel(t->srv);
        Rel(g_gpu.state); Rel(g_gpu.vs); Rel(g_gpu.ps); Rel(g_gpu.layout); Rel(g_gpu.vb); Rel(g_gpu.blendAlpha); Rel(g_gpu.blendAdd);
        Rel(g_gpu.raster); Rel(g_gpu.depth); Rel(g_gpu.sampler); Rel(g_gpu.noDepth); Rel(g_gpu.sceneSrv); g_gpu.sceneTex = nullptr;
        g_gpu.dev = nullptr; g_gpu.vbVerts = 0; g_gpu.failed = false;
    }

    // t1: the scene depth copy, reverse Z with the near plane at 1 unit (depth = 1 / view distance, 0 = nothing).
    // A sprite fades out over 1.5 units in front of the scene surface and is gone behind it.
    const char kShader[] =
        "Texture2D t0 : register(t0); Texture2D<float> t1 : register(t1); SamplerState s0 : register(s0);\n"
        "struct VI { float2 p : POSITION; float2 uv : TEXCOORD0; float4 c : COLOR0; float z : TEXCOORD1; };\n"
        "struct PI { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 c : COLOR0; float z : TEXCOORD1; };\n"
        "PI VS(VI i) { PI o; o.p = float4(i.p, 0.5, 1); o.uv = i.uv; o.c = i.c; o.z = i.z; return o; }\n"
        "float4 PS(PI i) : SV_Target { float d = t1.Load(int3(i.p.xy, 0)); float sz = d > 0.0 ? 1.0 / d : 1e9;\n"
        "  float f = saturate((sz - i.z) / 1.5); float4 t = t0.Sample(s0, i.uv) * i.c; t.a *= f; return t; }\n";

    bool SetupGpu(ID3D11Device* dev)
    {
        if (g_gpu.dev == dev) return !g_gpu.failed;
        ReleaseGpu();
        g_gpu.dev = dev;
        g_gpu.failed = true;
        ID3D11Device1* dev1 = nullptr;
        if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dev1))) || !dev1) { rp::Log("fxplayer: no ID3D11Device1"); return false; }
        const D3D_FEATURE_LEVEL level = dev->GetFeatureLevel();
        HRESULT hr = dev1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &g_gpu.state);
        dev1->Release();
        if (FAILED(hr)) { rp::Log("fxplayer: CreateDeviceContextState 0x%08X", unsigned(hr)); return false; }
        HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
        if (!dc) dc = LoadLibraryW(L"d3dcompiler_46.dll");
        CompileFn compile = dc ? reinterpret_cast<CompileFn>(GetProcAddress(dc, "D3DCompile")) : nullptr;
        if (!compile) { rp::Log("fxplayer: no d3dcompiler"); return false; }
        ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
        if (FAILED(compile(kShader, sizeof kShader - 1, "fx", nullptr, nullptr, "VS", "vs_4_0", 0, 0, &vsb, &err)) ||
            FAILED(compile(kShader, sizeof kShader - 1, "fx", nullptr, nullptr, "PS", "ps_4_0", 0, 0, &psb, &err)))
        {
            rp::Log("fxplayer: shader: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            Rel(err); Rel(vsb); Rel(psb);
            return false;
        }
        dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_gpu.vs);
        dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_gpu.ps);
        const D3D11_INPUT_ELEMENT_DESC il[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        dev->CreateInputLayout(il, 4, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_gpu.layout);
        // no scene depth: a 1x1 texture of 0 (nothing in front)
        const float zero = 0.0f;
        D3D11_TEXTURE2D_DESC zd{}; zd.Width = zd.Height = 1; zd.MipLevels = zd.ArraySize = 1; zd.Format = DXGI_FORMAT_R32_FLOAT; zd.SampleDesc.Count = 1;
        zd.Usage = D3D11_USAGE_IMMUTABLE; zd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA zs{&zero, 4, 4};
        ID3D11Texture2D* zt = nullptr;
        if (SUCCEEDED(dev->CreateTexture2D(&zd, &zs, &zt)) && zt) { dev->CreateShaderResourceView(zt, nullptr, &g_gpu.noDepth); zt->Release(); }
        Rel(vsb); Rel(psb);
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        dev->CreateBlendState(&bd, &g_gpu.blendAlpha);
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        dev->CreateBlendState(&bd, &g_gpu.blendAdd);
        D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = FALSE;
        dev->CreateRasterizerState(&rd, &g_gpu.raster);
        D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthEnable = FALSE; dd.StencilEnable = FALSE;
        dev->CreateDepthStencilState(&dd, &g_gpu.depth);
        D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&sd, &g_gpu.sampler);
        g_gpu.failed = !(g_gpu.vs && g_gpu.ps && g_gpu.layout && g_gpu.blendAlpha && g_gpu.blendAdd && g_gpu.raster && g_gpu.depth && g_gpu.sampler);
        if (g_gpu.failed) rp::Log("fxplayer: GPU objects failed");
        return !g_gpu.failed;
    }

    ID3D11ShaderResourceView* Srv(Tex& t)
    {
        if (t.srv || t.failed) return t.srv;
        D3D11_TEXTURE2D_DESC td{}; td.Width = t.w; td.Height = t.h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = t.rgba.data(); sd.SysMemPitch = t.w * 4;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(g_gpu.dev->CreateTexture2D(&td, &sd, &tex)) || !tex) { t.failed = true; return nullptr; }
        g_gpu.dev->CreateShaderResourceView(tex, nullptr, &t.srv);
        tex->Release();
        if (!t.srv) t.failed = true;
        return t.srv;
    }

    // ---- the scene depth: the depth views APE clears, the preview's one is used to hide fx behind models ----
    using ClearDsvFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
    ClearDsvFn g_origClearDsv = nullptr;
    struct DsvSeen { ID3D11DepthStencilView* dsv; UINT w, h, fmt, bind, samples, clears; float depth; ULONGLONG last; };
    DsvSeen g_dsvs[8] = {};
    // test harness: read the scene depth at a screen point (-1..1) at the next Present
    volatile LONG g_probeReq = 0;
    float g_probeX = 0, g_probeY = 0, g_probeZ = 0;
    char g_probeOut[512] = {};
    UINT g_bbW = 0, g_bbH = 0;
    SRWLOCK g_dsvLock = SRWLOCK_INIT;

    // the scene depth before APE clears the preview's depth buffer again (it is empty at Present): copied at each clear of a
    // depth buffer the size of the preview, the copy made at the frame's last clear holds the scene
    ID3D11Texture2D* g_snap = nullptr;
    UINT g_snapW = 0, g_snapH = 0, g_snapFmt = 0;
    ULONGLONG g_snapCount = 0;
    volatile UINT g_wantW = 0, g_wantH = 0;   // the preview's back buffer size, from Present

    void Snapshot(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv)
    {
        ID3D11Resource* r = nullptr; dsv->GetResource(&r);
        ID3D11Texture2D* t = nullptr;
        if (r && SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&t))) && t)
        {
            D3D11_TEXTURE2D_DESC td{}; t->GetDesc(&td);
            if (td.Width == g_wantW && td.Height == g_wantH && td.SampleDesc.Count == 1 && td.ArraySize == 1 && (td.BindFlags & D3D11_BIND_SHADER_RESOURCE))
            {
                if (!g_snap || g_snapW != td.Width || g_snapH != td.Height || g_snapFmt != UINT(td.Format))
                {
                    if (g_snap) { g_snap->Release(); g_snap = nullptr; }
                    D3D11_TEXTURE2D_DESC sd = td; sd.MipLevels = 1; sd.BindFlags = D3D11_BIND_SHADER_RESOURCE; sd.MiscFlags = 0; sd.CPUAccessFlags = 0; sd.Usage = D3D11_USAGE_DEFAULT;
                    ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
                    if (dev) { dev->CreateTexture2D(&sd, nullptr, &g_snap); dev->Release(); }
                    g_snapW = td.Width; g_snapH = td.Height; g_snapFmt = td.Format;
                }
                if (g_snap) { ctx->CopyResource(g_snap, t); ++g_snapCount; }
            }
            t->Release();
        }
        if (r) r->Release();
    }

    void STDMETHODCALLTYPE hk_ClearDsv(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, UINT flags, FLOAT depth, UINT8 stencil)
    {
        if (dsv && ctx && (flags & D3D11_CLEAR_DEPTH) && g_wantW)
        {
            AcquireSRWLockExclusive(&g_dsvLock);
            __try { Snapshot(ctx, dsv); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            ReleaseSRWLockExclusive(&g_dsvLock);
        }
        g_origClearDsv(ctx, dsv, flags, depth, stencil);
        if (!dsv || !(flags & D3D11_CLEAR_DEPTH)) return;
        AcquireSRWLockExclusive(&g_dsvLock);
        int slot = -1, oldest = 0;
        for (int i = 0; i < 8; ++i)
        {
            if (g_dsvs[i].dsv == dsv) { slot = i; break; }
            if (g_dsvs[i].last < g_dsvs[oldest].last) oldest = i;
        }
        if (slot < 0)
        {
            slot = oldest;
            DsvSeen d{}; d.dsv = dsv;
            ID3D11Resource* r = nullptr; dsv->GetResource(&r);
            ID3D11Texture2D* t = nullptr;
            if (r && SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&t))) && t)
            {
                D3D11_TEXTURE2D_DESC td{}; t->GetDesc(&td);
                d.w = td.Width; d.h = td.Height; d.fmt = td.Format; d.bind = td.BindFlags; d.samples = td.SampleDesc.Count;
                t->Release();
            }
            if (r) r->Release();
            g_dsvs[slot] = d;
        }
        g_dsvs[slot].clears++; g_dsvs[slot].depth = depth; g_dsvs[slot].last = GetTickCount64();
        ReleaseSRWLockExclusive(&g_dsvLock);
    }

    // the shader view of the scene depth copy when it matches the back buffer
    ID3D11ShaderResourceView* SceneDepthSrv(ID3D11Device* dev, UINT w, UINT h)
    {
        AcquireSRWLockShared(&g_dsvLock);
        ID3D11Texture2D* snap = g_snap && g_snapW == w && g_snapH == h ? g_snap : nullptr;
        const UINT fmt = g_snapFmt;
        ReleaseSRWLockShared(&g_dsvLock);
        if (!snap) return nullptr;
        if (g_gpu.sceneTex != snap)
        {
            Rel(g_gpu.sceneSrv);
            g_gpu.sceneTex = snap;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = fmt == DXGI_FORMAT_R32G8X24_TYPELESS ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : fmt == DXGI_FORMAT_R24G8_TYPELESS ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS
                      : fmt == DXGI_FORMAT_R32_TYPELESS ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_UNKNOWN;
            if (sd.Format == DXGI_FORMAT_UNKNOWN) return nullptr;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
            dev->CreateShaderResourceView(snap, &sd, &g_gpu.sceneSrv);
        }
        return g_gpu.sceneSrv;
    }

    // everything Draw holds, released by DrawGuarded even after an exception (a live view of the back buffer breaks APE's ResizeBuffers)
    struct DrawRes
    {
        ID3D11Device* dev; ID3D11DeviceContext* ctx; ID3D11DeviceContext1* ctx1;
        ID3D11Texture2D* bb; ID3D11RenderTargetView* rtv; ID3DDeviceContextState* prev; bool swapped;
    } g_res{};

    void Draw(IDXGISwapChain* sc)
    {
        if (g_verts.empty() || GetTickCount64() - g_drawStamp > 250) return;
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(sc->GetDesc(&desc)) || !g_drawWnd || (desc.OutputWindow != g_drawWnd && !IsChild(desc.OutputWindow, g_drawWnd) && !IsChild(g_drawWnd, desc.OutputWindow))) return;
        DrawRes& r = g_res;
        if (FAILED(sc->GetDevice(IID_PPV_ARGS(&r.dev))) || !r.dev || !SetupGpu(r.dev)) return;
        r.dev->GetImmediateContext(&r.ctx);
        if (!r.ctx || FAILED(r.ctx->QueryInterface(IID_PPV_ARGS(&r.ctx1))) || !r.ctx1) return;
        if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&r.bb))) || !r.bb || FAILED(r.dev->CreateRenderTargetView(r.bb, nullptr, &r.rtv)) || !r.rtv) return;
        const UINT need = UINT(g_verts.size());
        if (g_gpu.vbVerts < need)
        {
            Rel(g_gpu.vb);
            g_gpu.vbVerts = std::max(need, 6144u);
            D3D11_BUFFER_DESC b{}; b.ByteWidth = g_gpu.vbVerts * sizeof(Vtx); b.Usage = D3D11_USAGE_DYNAMIC; b.BindFlags = D3D11_BIND_VERTEX_BUFFER; b.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(r.dev->CreateBuffer(&b, nullptr, &g_gpu.vb))) g_gpu.vbVerts = 0;
        }
        if (!g_gpu.vb) return;
        ID3D11DeviceContext* ctx = r.ctx;
        r.ctx1->SwapDeviceContextState(g_gpu.state, &r.prev);
        r.swapped = true;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(g_gpu.vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
        memcpy(m.pData, g_verts.data(), g_verts.size() * sizeof(Vtx));
        ctx->Unmap(g_gpu.vb, 0);
        D3D11_TEXTURE2D_DESC td{}; r.bb->GetDesc(&td);
        D3D11_VIEWPORT vp{0, 0, float(td.Width), float(td.Height), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(g_gpu.raster);
        ctx->OMSetRenderTargets(1, &r.rtv, nullptr);
        ctx->OMSetDepthStencilState(g_gpu.depth, 0);
        const UINT stride = sizeof(Vtx), offset = 0;
        ctx->IASetVertexBuffers(0, 1, &g_gpu.vb, &stride, &offset);
        ctx->IASetInputLayout(g_gpu.layout);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(g_gpu.vs, nullptr, 0);
        ctx->PSSetShader(g_gpu.ps, nullptr, 0);
        ctx->PSSetSamplers(0, 1, &g_gpu.sampler);
        ID3D11ShaderResourceView* depthSrv = SceneDepthSrv(r.dev, td.Width, td.Height);
        if (!depthSrv) depthSrv = g_gpu.noDepth;
        ctx->PSSetShaderResources(1, 1, &depthSrv);
        const float blendFactor[4] = {0, 0, 0, 0};
        for (size_t i = 0; i < g_batches.size(); ++i)
        {
            const Batch& b = g_batches[i];
            ID3D11ShaderResourceView* srv = b.tex ? Srv(*b.tex) : nullptr;
            if (!srv) continue;
            ctx->PSSetShaderResources(0, 1, &srv);
            ctx->OMSetBlendState(b.additive ? g_gpu.blendAdd : g_gpu.blendAlpha, blendFactor, 0xFFFFFFFF);
            ctx->Draw(b.count, b.first);
        }
        ++g_presents;
    }

    void ReleaseRes()
    {
        DrawRes& r = g_res;
        if (r.swapped && r.ctx && r.ctx1)
        {
            ID3D11RenderTargetView* noRtv = nullptr; ID3D11ShaderResourceView* noSrv = nullptr; ID3D11Buffer* noVb = nullptr;
            r.ctx->PSSetShaderResources(1, 1, &noSrv);
            const UINT zero = 0;
            r.ctx->OMSetRenderTargets(1, &noRtv, nullptr);
            r.ctx->PSSetShaderResources(0, 1, &noSrv);
            r.ctx->IASetVertexBuffers(0, 1, &noVb, &zero, &zero);
            r.ctx1->SwapDeviceContextState(r.prev, nullptr);
        }
        Rel(r.prev); Rel(r.rtv); Rel(r.bb); Rel(r.ctx1); Rel(r.ctx); Rel(r.dev);
        r.swapped = false;
    }

    void ClearDraw()
    {
        std::vector<Vtx> v; std::vector<Batch> b;
        AcquireSRWLockExclusive(&g_drawLock);
        g_verts.swap(v); g_batches.swap(b);
        ReleaseSRWLockExclusive(&g_drawLock);
    }

    ID3D11DepthStencilView* SceneDsv(UINT w, UINT h)
    {
        ID3D11DepthStencilView* best = nullptr; ULONGLONG last = 0;
        const ULONGLONG now = GetTickCount64();
        AcquireSRWLockShared(&g_dsvLock);
        for (const DsvSeen& d : g_dsvs)
            if (d.dsv && d.w == w && d.h == h && d.samples == 1 && (d.bind & D3D11_BIND_SHADER_RESOURCE) && now - d.last < 500 && d.last > last) { best = d.dsv; last = d.last; }
        ReleaseSRWLockShared(&g_dsvLock);
        return best;
    }

    void Probe(IDXGISwapChain* sc)
    {
        ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; ID3D11Texture2D* bb = nullptr; ID3D11Texture2D* st = nullptr; ID3D11Resource* dr = nullptr;
        g_probeOut[0] = 0;
        if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&dev))) && dev && SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&bb))) && bb)
        {
            D3D11_TEXTURE2D_DESC bd{}; bb->GetDesc(&bd);
            g_wantW = bd.Width; g_wantH = bd.Height;
            AcquireSRWLockShared(&g_dsvLock);
            ID3D11Texture2D* dt = g_snap && g_snapW == bd.Width && g_snapH == bd.Height ? g_snap : nullptr;
            if (dt) dt->AddRef();
            ReleaseSRWLockShared(&g_dsvLock);
            if (!dt) { snprintf(g_probeOut, sizeof g_probeOut, "no depth copy of %ux%u (copies %llu)", bd.Width, bd.Height, (unsigned long long)g_snapCount); }
            else
            {
                {
                    D3D11_TEXTURE2D_DESC td{}; dt->GetDesc(&td);
                    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
                    dev->GetImmediateContext(&ctx);
                    if (ctx && SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &st)) && st)
                    {
                        ctx->CopyResource(st, dt);
                        D3D11_MAPPED_SUBRESOURCE m{};
                        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m)))
                        {
                            const int px = std::clamp(int((g_probeX * 0.5f + 0.5f) * td.Width), 0, int(td.Width) - 1);
                            const int py = std::clamp(int((0.5f - g_probeY * 0.5f) * td.Height), 0, int(td.Height) - 1);
                            const UINT bpp = td.Format == 19 ? 8 : 4;
                            float best = 0;
                            for (int dy = -6; dy <= 6; ++dy) for (int dx = -6; dx <= 6; ++dx)
                            {
                                const int x = std::clamp(px + dx, 0, int(td.Width) - 1), y = std::clamp(py + dy, 0, int(td.Height) - 1);
                                const float d = *reinterpret_cast<const float*>(static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch + size_t(x) * bpp);
                                best = std::max(best, d);
                            }
                            snprintf(g_probeOut, sizeof g_probeOut, "pixel %d,%d of %ux%u: depth %.6f, point view z %.2f, depth*z %.4f", px, py, td.Width, td.Height, best, g_probeZ, best * g_probeZ);
                            ctx->Unmap(st, 0);
                        }
                    }
                    dt->Release();
                }
            }
        }
        if (st) st->Release(); if (dr) dr->Release(); if (ctx) ctx->Release(); if (bb) bb->Release(); if (dev) dev->Release();
    }

    bool g_broken = false;              // an exception in the player: fx off until the plugin reloads
    // the preview's back buffer size: depth buffers of that size are copied before their clears
    void NoteSize(IDXGISwapChain* sc)
    {
        DXGI_SWAP_CHAIN_DESC d{};
        HWND w = g_cam.hwnd;
        if (!w || FAILED(sc->GetDesc(&d)) || (d.OutputWindow != w && !IsChild(d.OutputWindow, w) && !IsChild(w, d.OutputWindow))) return;
        g_wantW = d.BufferDesc.Width; g_wantH = d.BufferDesc.Height;
    }

    void DrawGuarded(IDXGISwapChain* sc)
    {
        __try { NoteSize(sc); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (InterlockedExchange(&g_probeReq, 0))
        {
            __try { Probe(sc); }
            __except (EXCEPTION_EXECUTE_HANDLER) { snprintf(g_probeOut, sizeof g_probeOut, "probe exception"); }
        }
        if (g_broken) return;
        AcquireSRWLockExclusive(&g_drawLock);
        __try { Draw(sc); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_broken = true; }
        __try { ReleaseRes(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_broken = true; g_res = DrawRes{}; }
        ReleaseSRWLockExclusive(&g_drawLock);
    }

    HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags)
    {
        if (!(flags & DXGI_PRESENT_TEST)) DrawGuarded(sc);
        return g_origPresent(sc, sync, flags);
    }
    HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p)
    {
        if (!(flags & DXGI_PRESENT_TEST)) DrawGuarded(sc);
        return g_origPresent1(sc, sync, flags, p);
    }

    // the Present of this process's DXGI, from a throwaway device and swap chain on a hidden window
    bool HookPresent()
    {
        HMODULE d3d = GetModuleHandleW(L"d3d11.dll");
        if (!d3d) d3d = LoadLibraryW(L"d3d11.dll");
        auto create = d3d ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(GetProcAddress(d3d, "D3D11CreateDeviceAndSwapChain")) : nullptr;
        if (!create) return false;
        WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"APEPlusFxProbe";
        RegisterClassW(&wc);
        HWND w = CreateWindowW(L"APEPlusFxProbe", L"", WS_POPUP, 0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);
        if (!w) return false;
        DXGI_SWAP_CHAIN_DESC d{};
        d.BufferCount = 1; d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.BufferDesc.Width = 8; d.BufferDesc.Height = 8;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.OutputWindow = w; d.SampleDesc.Count = 1; d.Windowed = TRUE;
        IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
        HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, &sc, &dev, nullptr, &ctx);
        bool ok = false;
        if (SUCCEEDED(hr) && sc)
        {
            void** vt = *reinterpret_cast<void***>(sc);
            ok = rp::host->Detour(rp::self, vt[8], reinterpret_cast<void*>(&hk_Present), reinterpret_cast<void**>(&g_origPresent));
            void** cvt = ctx ? *reinterpret_cast<void***>(ctx) : nullptr;   // ID3D11DeviceContext::ClearDepthStencilView is slot 53
            if (cvt) rp::host->Detour(rp::self, cvt[53], reinterpret_cast<void*>(&hk_ClearDsv), reinterpret_cast<void**>(&g_origClearDsv));
            IDXGISwapChain1* sc1 = nullptr;
            if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1)
            {
                void** vt1 = *reinterpret_cast<void***>(sc1);
                if (vt1[22] != vt[8]) rp::host->Detour(rp::self, vt1[22], reinterpret_cast<void*>(&hk_Present1), reinterpret_cast<void**>(&g_origPresent1));
                sc1->Release();
            }
        }
        Rel(ctx); Rel(dev); Rel(sc);
        DestroyWindow(w);
        UnregisterClassW(L"APEPlusFxProbe", wc.hInstance);
        rp::Log("fxplayer: Present hook %s (0x%08X)", ok ? "ok" : "FAILED", unsigned(hr));
        return ok;
    }
    bool g_hooked = false;
}

bool Install()
{
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    g_root = exe; g_root = g_root.substr(0, g_root.find_last_of("\\/")); g_root = g_root.substr(0, g_root.find_last_of("\\/"));
    QueryPerformanceFrequency(&g_freq);
    g_hooked = HookPresent();
    return g_hooked;
}

void Shutdown()
{
    ClearDraw();
    AcquireSRWLockExclusive(&g_drawLock);
    ReleaseGpu();
    ReleaseSRWLockExclusive(&g_drawLock);
    AcquireSRWLockExclusive(&g_dsvLock);
    g_wantW = g_wantH = 0;
    if (g_snap) { g_snap->Release(); g_snap = nullptr; }
    ReleaseSRWLockExclusive(&g_dsvLock);
    g_models.clear();
}

const std::vector<ModelPart>& Models() { return g_models; }

namespace
{
    void CollectModels(const std::shared_ptr<Effect>& fx, std::vector<std::string>& out, int depth)
    {
        if (!fx || depth > 3) return;
        for (const Elem& el : fx->elems)
        {
            if (el.vis == kModel && !el.eventOnly) for (const std::string& m : el.visuals) if (std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m);
            if (el.vis == kRunner) for (const std::string& c : el.visuals) CollectModels(Load("fx\\" + c + ".efx"), out, depth + 1);
        }
    }
}
std::vector<std::string> ModelNames(const std::string& path)
{
    std::vector<std::string> out;
    if (!path.empty()) CollectModels(Load(path), out, 0);
    return out;
}

void SetResolver(Resolver r) { g_resolver = r; }

void Play(const std::string& path, int anchor)
{
    if (!g_hooked || g_broken || path.empty()) return;
    auto fx = Load(path);
    if (!fx->error.empty()) return;
    ++g_played;
    StartInst(fx, anchor, Identity());
    if (g_stepping) return;
    for (Inst& n : g_newInsts) g_insts.push_back(std::move(n));
    g_newInsts.clear();
}

void StopAll()
{
    if (g_stepping) { g_stopPending = true; return; }
    g_insts.clear(); g_newInsts.clear(); g_parts.clear(); g_models.clear();
    ClearDraw();
}

void SetAnchor(int anchor, const Xform& x) { g_anchors[anchor & 3] = x; }

void SetCamera(const float origin[3], const float forward[3], const float right[3], const float up[3], float tanY, float tanX, void* hwnd)
{
    memcpy(g_cam.o, origin, 12); memcpy(g_cam.f, forward, 12); memcpy(g_cam.r, right, 12); memcpy(g_cam.u, up, 12);
    g_cam.ty = tanY > 1e-4f ? tanY : 0.4f; g_cam.tx = tanX > 1e-4f ? tanX : 0.7f;
    g_cam.hwnd = static_cast<HWND>(hwnd);
    g_cam.set = true;
}

void UpdateInner();
void Update()
{
    if (g_broken) return;
    __try { UpdateInner(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_broken = true; rp::Log("fxplayer: exception, fx off until the plugin reloads"); }
}

void UpdateInner()
{
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    double dt = g_last.QuadPart ? double(now.QuadPart - g_last.QuadPart) / double(g_freq.QuadPart) : 0.0;
    g_last = now;
    if (dt > 0.1) dt = 0.1;
    dt *= g_timeScale;
    if (g_insts.empty() && g_parts.empty())
    {
        g_models.clear();
        ClearDraw();
        return;
    }
    Step(dt);
    if (g_cam.set) Build();
}

std::string Dump()
{
    std::string r;
    char b[400];
    for (const Part& p : g_parts)
    {
        const Elem& el = *p.el;
        const float t = float(std::min(1.0, p.age / p.life));
        const Xform frame = el.relEffect ? Compose(p.local, g_anchors[p.anchor & 3]) : p.spawn;
        float w[3]; Apply(frame, p.pos, w);
        float x = 0, y = 0, z = 0;
        const bool vis = Project(w, x, y, z);
        const float scale = el.scale.set && el.scale.scale != 0 ? Eval(el.scale, t, el.randScale ? p.rnd[3] : -1) : 1.0f;
        const float s0 = Eval(el.size0, t, el.randSize0 ? p.rnd[4] : -1) * scale;
        const float a = el.alpha.set ? Eval(el.alpha, t, el.randAlpha ? p.rnd[7] : -1) : 1.0f;
        snprintf(b, sizeof b, "%s vis %d t %.2f size %.2f (graph %d scale %.2f keys %zu) alpha %.2f (graph %d) screen %d %.2f %.2f z %.1f tex %ux%u%s\n",
                 el.name.c_str(), int(el.vis), t, s0, el.size0.set ? 1 : 0, el.size0.scale, el.size0.a.k.size(), a, el.alpha.set ? 1 : 0, vis ? 1 : 0, x, y, z,
                 p.tex ? p.tex->w : 0, p.tex ? p.tex->h : 0, p.tex && p.tex->additive ? " add" : "");
        r += b;
    }
    return r.empty() ? "no particles" : r;
}

void ProbeDepth(int anchor)
{
    const Xform& x = g_anchors[anchor & 3];
    float px = 0, py = 0, pz = 0;
    Project(x.o, px, py, pz);
    g_probeX = px; g_probeY = py; g_probeZ = pz;
    InterlockedExchange(&g_probeReq, 1);
}

std::string DepthReport()
{
    std::string r = std::string("probe: ") + g_probeOut + "\n";
    char b[256];
    AcquireSRWLockShared(&g_dsvLock);
    const ULONGLONG now = GetTickCount64();
    for (const DsvSeen& d : g_dsvs)
    {
        if (!d.dsv) continue;
        snprintf(b, sizeof b, "dsv %p %ux%u format %u bind 0x%x samples %u, %u clears to %.3f, last %llu ms ago\n", (void*)d.dsv, d.w, d.h, d.fmt, d.bind, d.samples, d.clears, d.depth,
                 (unsigned long long)(now - d.last));
        r += b;
    }
    ReleaseSRWLockShared(&g_dsvLock);
    return r.empty() ? "no depth clears seen" : r;
}

void SetTimeScale(double s) { g_timeScale = s >= 0 ? s : 1.0; }

bool Active() { return !g_insts.empty() || !g_parts.empty() || !g_newInsts.empty(); }

std::string Status()
{
    char b[512];
    snprintf(b, sizeof b, "fx%s: present hook %s; %llu played, %llu particles spawned, %zu live in %zu effects, %llu last frame, %llu draws; %zu effects and %zu materials cached; camera f %.2f %.2f %.2f r %.2f %.2f %.2f u %.2f %.2f %.2f tan %.3f %.3f",
             g_broken ? " BROKEN (exception)" : "", g_hooked ? "on" : "OFF", (unsigned long long)g_played, (unsigned long long)g_spawned, g_parts.size(), g_insts.size(), (unsigned long long)g_drawn,
             (unsigned long long)g_presents, g_effects.size(), g_tex.size(), g_cam.f[0], g_cam.f[1], g_cam.f[2], g_cam.r[0], g_cam.r[1], g_cam.r[2], g_cam.u[0], g_cam.u[1], g_cam.u[2], g_cam.ty, g_cam.tx);
    std::string r = b;
    const Xform& x = g_anchors[0];
    float px = 0, py = 0, pz = 0;
    const bool vis = Project(x.o, px, py, pz);
    snprintf(b, sizeof b, "\nanchor 0 origin %.2f %.2f %.2f, x axis %.2f %.2f %.2f, z axis %.2f %.2f %.2f; on screen %s %.3f %.3f depth %.2f; camera origin %.2f %.2f %.2f",
             x.o[0], x.o[1], x.o[2], x.a[0], x.a[1], x.a[2], x.a[6], x.a[7], x.a[8], vis ? "yes" : "no", px, py, pz, g_cam.o[0], g_cam.o[1], g_cam.o[2]);
    r += b;
    if (!g_parts.empty())
    {
        const Part& p = g_parts.front();
        snprintf(b, sizeof b, "\nfirst particle %s age %.3f/%.3f local %.2f %.2f %.2f", p.el->name.c_str(), p.age, p.life, p.pos[0], p.pos[1], p.pos[2]);
        r += b;
    }
    return r;
}
}
