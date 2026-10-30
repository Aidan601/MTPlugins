// Notetrack fx in APE's preview, see notefx.h.
// notesounds reads the notes and hands the fx ones here, the fx player draws them, previewfix gives the posed bone.
#include "modtools_plugin.hpp"
#include "notefx.h"
#include "fxplayer.h"
#include "notesounds.h"
#include "previewfix.h"
#include "weaponpreview.h"
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ba::notefx
{
namespace
{
    const uintptr_t kOpts = 0x14118D620;         // previewWindowOpts (pointer)
    const size_t kEntries = 2440, kEntryStride = 1336, kEntryEnabled = 776, kEntryInUse = 777, kEntryHolder = 792;
    const int kOwner = 1;                        // fx player owner id of this module (the weapon preview is 0)
    const int kFirstAnchor = 8;                  // fx player anchors below this belong to the weapon preview
    const double kLoopFor = 1e9;                 // a looping fx runs until a Stop Fx note or the anim changes, as in game

    bool g_on = true;                            // [notefx] enabled
    bool g_ready = false;                        // the fx player is installed
    uint8_t** g_opts = nullptr;
    bool g_paused = false;
    uint64_t g_played = 0, g_stopped = 0;
    std::vector<std::string> g_told;

    struct Anchor { std::string tag; int id = 0; uint8_t* entry = nullptr; bool missing = false; };
    std::vector<Anchor> g_anchors;

    std::string Lower(std::string s) { for (char& c : s) c = char(tolower(static_cast<unsigned char>(c))); return s; }

    void Say(const std::string& text)
    {
        if (std::find(g_told.begin(), g_told.end(), text) != g_told.end()) return;
        g_told.push_back(text);
        rp::Log("notefx: %s", text.c_str());
        if (rp::Ape()) rp::Ape()->Print(rp::self, ("Notetrack fx: " + text).c_str());
    }

    // "<dir>\name", "<dir>/name.efx" or "fx\<dir>\name" -> "fx\<dir>\name.efx", the form the fx player loads
    std::string FxPath(const std::string& param)
    {
        std::string p;
        for (char c : param) { if (c == '/') c = '\\'; if (c == '\\' && !p.empty() && p.back() == '\\') continue; p += c; }
        while (!p.empty() && (p.front() == ' ' || p.front() == '"' || p.front() == '\\')) p.erase(0, 1);
        while (!p.empty() && (p.back() == ' ' || p.back() == '"')) p.pop_back();
        if (p.empty()) return {};
        std::string l = Lower(p);
        if (l.rfind("share\\raw\\", 0) == 0) { p.erase(0, 10); l.erase(0, 10); }
        if (l.rfind("fx\\", 0) == 0) { p.erase(0, 3); l.erase(0, 3); }
        if (l.size() < 4 || l.compare(l.size() - 4, 4, ".efx") != 0) p += ".efx";
        return "fx\\" + p;
    }

    uint8_t* Entries() { return g_opts && *g_opts ? *g_opts + kEntries : nullptr; }

    // the preview entry whose model has the bone: the preview model first, then the attached ones
    uint8_t* EntryWithTag(const std::string& tag, ba::fxplayer::Xform& x)
    {
        uint8_t* e = Entries();
        if (!e) return nullptr;
        if (ba::previewfix::AnimTagWorld(e, tag.c_str(), x.o)) return e;
        for (int i = 1; i < 32; ++i)
        {
            uint8_t* entry = e + size_t(i) * kEntryStride;
            if (!entry[kEntryInUse] || !entry[kEntryEnabled] || !*reinterpret_cast<void**>(entry + kEntryHolder)) continue;
            if (ba::previewfix::AnimTagWorld(entry, tag.c_str(), x.o)) return entry;
        }
        return nullptr;
    }

    bool Place(Anchor& a)
    {
        ba::fxplayer::Xform x{};
        if (a.entry && ba::previewfix::AnimTagWorld(a.entry, a.tag.c_str(), x.o)) { ba::fxplayer::SetAnchor(a.id, x); return true; }
        a.entry = EntryWithTag(a.tag, x);
        if (a.entry) { ba::fxplayer::SetAnchor(a.id, x); return true; }
        uint8_t* e = Entries();
        if (e && ba::previewfix::AnimTagWorld(e, "tag_origin", x.o)) { ba::fxplayer::SetAnchor(a.id, x); return true; }
        return false;
    }

    // one anchor per bone, placed now; an unknown bone plays at the model's origin with a note
    int AnchorFor(const std::string& tagIn)
    {
        const std::string tag = tagIn.empty() ? std::string("tag_origin") : Lower(tagIn);
        for (Anchor& a : g_anchors) if (a.tag == tag) { Place(a); return a.id; }
        if (kFirstAnchor + int(g_anchors.size()) >= ba::fxplayer::kAnchors) { Say("more than " + std::to_string(ba::fxplayer::kAnchors - kFirstAnchor) + " bones with fx, the rest play at the origin"); return kFirstAnchor; }
        Anchor a; a.tag = tag; a.id = kFirstAnchor + int(g_anchors.size());
        ba::fxplayer::Xform x{};
        a.entry = EntryWithTag(tag, x);
        if (!a.entry && tag != "tag_origin") { a.missing = true; Say("bone " + tagIn + " is not on the preview model or an attached one, the fx plays at the origin"); }
        g_anchors.push_back(a);
        Place(g_anchors.back());
        return a.id;
    }

    // the fx player's sounds: an element's spawn sound, a follow sound or the effect's looped sound
    void FxSound(const std::string& alias, int mode)
    {
        if (mode == 0) ba::notesounds::PlayAlias(alias, false);
        else if (mode == 1) ba::notesounds::PlayAliasLoop(alias);
        else ba::notesounds::StopAliasVoices(alias);
    }
}

void Install()
{
    g_on = rp::ConfigGetBool("notefx", "enabled", true);
    if (rp::Ape()) g_opts = static_cast<uint8_t**>(rp::Ape()->ApeAddress(rp::self, kOpts));
    ba::fxplayer::SetSoundSink(&FxSound);
    if (g_on) g_ready = ba::weaponpreview::EnsureFx();
    rp::Log("notefx: %s%s%s", g_on ? "on" : "off", g_on && !g_ready ? ", fx player NOT installed" : "", g_opts ? "" : ", preview globals NOT found");
}

bool Enabled() { return g_on && g_ready; }

bool IsFxAction(const std::string& action)
{
    const std::string a = Lower(action);
    return a == "play fx" || a == "play unmanaged fx" || a == "play viewmodel fx" || a == "stop fx" || a == "kill fx"
        || a == "play tagfx set" || a == "stop tagfx set" || a == "kill tagfx set" || a == "play entity impact fx" || a == "play exploder";
}

void Fire(const std::string& action, const std::string& fx, const std::string& tag)
{
    if (!Enabled() || !g_opts) return;
    const std::string a = Lower(action);
    if (a == "play fx" || a == "play unmanaged fx" || a == "play viewmodel fx")
    {
        const std::string path = FxPath(fx);
        if (path.empty()) return;
        const int anchor = AnchorFor(tag);
        ba::fxplayer::Stop(path, anchor, kOwner);   // a looping fx fired again on the same bone restarts
        ba::fxplayer::Play(path, anchor, kLoopFor, kOwner);
        ++g_played;
        return;
    }
    if (a == "stop fx" || a == "kill fx")
    {
        const std::string path = FxPath(fx);
        if (!path.empty()) { ba::fxplayer::Stop(path, -1, kOwner); ++g_stopped; }
        return;
    }
    Say(action + " notes are not played in the preview (" + fx + ")");
}

void StopAll()
{
    if (!g_ready) return;
    ba::fxplayer::Stop(std::string(), -1, kOwner);
    g_anchors.clear();
}

void Pause(bool paused)
{
    if (!g_ready || paused == g_paused) return;
    g_paused = paused;
    ba::fxplayer::PauseSounds(paused);
}

void AfterFrame()
{
    if (!g_ready || g_anchors.empty() || !ba::fxplayer::Active()) return;
    for (Anchor& a : g_anchors) Place(a);
}

void Shutdown()
{
    StopAll();
    g_told.clear();
}

bool Command(const std::string& c, std::string& reply)
{
    if (c.rfind("notefx", 0) != 0) return false;
    std::string arg = c.size() > 6 ? c.substr(7) : std::string();
    if (arg == "on" || arg == "off")
    {
        g_on = arg == "on"; rp::ConfigSet("notefx", "enabled", g_on ? "1" : "0");
        if (g_on && !g_ready) g_ready = ba::weaponpreview::EnsureFx();
        if (!g_on) StopAll();
        reply = std::string("notefx ") + (g_on ? "on" : "off") + (g_on && !g_ready ? " (fx player not installed)" : "");
        return true;
    }
    if (arg == "stop") { StopAll(); reply = "stopped"; return true; }
    if (arg.rfind("cam ", 0) == 0)   // test harness: cam <x> <y> <z> <pitch> <yaw>, the preview camera at opts +24 / +36
    {
        float v[5] = {};
        if (sscanf_s(arg.c_str() + 4, "%f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4]) != 5 || !g_opts || !*g_opts) { reply = "cam <x> <y> <z> <pitch> <yaw>"; return true; }
        float* org = reinterpret_cast<float*>(*g_opts + 24);
        float* ang = reinterpret_cast<float*>(*g_opts + 36);
        org[0] = v[0]; org[1] = v[1]; org[2] = v[2]; ang[0] = v[3]; ang[1] = v[4]; ang[2] = 0;
        reply = "camera set";
        return true;
    }
    if (arg == "resume" || arg == "pause") { reply = ba::weaponpreview::SetPaused(arg == "pause") ? "preview " + arg + "d" : "no preview"; return true; }   // test harness
    if (arg.rfind("play ", 0) == 0)   // test harness: play an fx on a bone of the preview model, as a note would
    {
        const std::string rest = arg.substr(5);
        const size_t sp = rest.find(' ');
        const std::string fx = rest.substr(0, sp), tag = sp == std::string::npos ? std::string() : rest.substr(sp + 1);
        if (!Enabled()) { reply = "notefx off or no fx player"; return true; }
        Fire("Play Fx", fx, tag);
        reply = "played " + FxPath(fx) + " on " + (tag.empty() ? std::string("tag_origin") : tag);
        return true;
    }
    char buf[512];
    snprintf(buf, sizeof buf, "notefx %s, fx player %s, %zu anchors, played %llu, stopped %llu%s\n", g_on ? "on" : "off", g_ready ? "installed" : "NOT installed",
             g_anchors.size(), (unsigned long long)g_played, (unsigned long long)g_stopped, g_paused ? ", paused" : "");
    reply = buf;
    for (const Anchor& a : g_anchors)
    {
        snprintf(buf, sizeof buf, "  anchor %d %s entry %p%s\n", a.id, a.tag.c_str(), (void*)a.entry, a.missing ? " (bone missing, origin)" : "");
        reply += buf;
    }
    reply += ba::fxplayer::Status();
    return true;
}
}
