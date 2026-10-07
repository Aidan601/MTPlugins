// Draft bake tier below Low: see bake_lowrays.h.
#include "bake_lowrays.h"
#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace br::bakell
{
namespace
{
// Where Radiant turns quality index 0 into 256 rays: the address of the 32-bit immediate and the opcode bytes before it.
struct Site { uintptr_t imm; uint8_t before[6]; uint8_t count; bool builder; const char* what; };
const Site kSites[] = {
    {0x141120F7B, {0xB8}, 1, false, "ray count helper (packet tracer)"},
    {0x141121EBF, {0xBF}, 1, false, "ray parameters (probe+10176)"},
    {0x14112488F, {0xBA}, 1, false, "hit fix-up driver"},
    {0x141124F8E, {0xC7, 0x85, 0x50, 0x01, 0x00, 0x00}, 6, false, "gather job stage 0x141124D60"},
    {0x141125F0F, {0xBA}, 1, false, "gather job"},
    {0x141127AFE, {0xBA}, 1, false, "AO stage"},
    {0x141129EA6, {0xB8}, 1, false, "ray count helper (camera)"},
    {0x141129C8F, {0x41, 0xBD}, 2, true, "basis table builder"},
    {0x14112A196, {0x41, 0xBC}, 2, true, "direction table builder"},
};
const uintptr_t kBuildDirs   = 0x14112A090;   // void(): fills gUniformDirs for the four ray counts (halves ordered for 32-ray packets)
const uintptr_t kBuildBasis  = 0x141129C20;   // void(): the two basis tables, from the directions
const uintptr_t kBakeStart   = 0x1402F69E0;   // void(CCamWnd*, char incremental, char sun, uchar probes, char): every build starts here
const uintptr_t kProbeReset  = 0x141183B30;   // void(owner*, char full): per probe at a build's start, full drops its trace
const uintptr_t kBakeQuality = 0x1453A23A4;   // int 0/1/2: the toolbar's L / M / H, copied to the volume when a build starts
const uintptr_t kTraceJob    = 0x1411341D0;   // int64(ctx*): one probe's trace task (pass 0x1411237B0 inside)
// int: probe jobs running. Radiant's own count of trace tasks (0x145581E58) cannot be used: it is raised before the
// start function finds the probe's task already pending, so it stays above zero after a bake.
const uintptr_t kInflight    = 0x145581E54;
// std::vector per [4 qualities][8 sets], all zero until Radiant's static init: directions (float[3]), basis rows A and B (float[6]).
struct Table { uintptr_t address; size_t row; };
const Table kTables[] = { {0x1455822A0, 12}, {0x1455828A0, 24}, {0x145581FA0, 24} };
const unsigned kSets = 8;
const size_t off_Owner_Probe = 488;           // shared_ptr<probe> of the object kProbeReset gets
const size_t off_Probe_Stride = 1636, off_Probe_Base = 1780, off_Probe_Quality = 1784;

struct Vec { uint8_t* begin; uint8_t* end; uint8_t* cap; };
using BuildFn = void(__fastcall*)();
using BakeStartFn = void(__fastcall*)(int64_t, char, char, unsigned char, char);
using ProbeResetFn = int64_t(__fastcall*)(int64_t, char);
using TraceJobFn = int64_t(__fastcall*)(int64_t);

br::bakeguide::Env g_env{};
std::atomic<std::uint32_t> g_lowRays{256};
bool g_shifted = false;      // Setup's way: the builders ran with 64, which moves every quality's direction sets
bool g_switchable = false;
bool (*g_wantDraft)() = nullptr;
bool (*g_busy)() = nullptr;
BakeStartFn o_BakeStart = nullptr;
ProbeResetFn o_ProbeReset = nullptr;
TraceJobFn o_TraceJob = nullptr;
std::atomic<int> g_tracing{0};   // trace tasks inside kTraceJob
bool g_draftBuild = false;       // the build that is starting bakes at the draft tier
std::atomic<std::uint32_t> g_retraced{0};
std::string g_note;          // why the last switch was refused

void Log(const char* fmt, ...)
{
    if (!g_env.log) return;
    char line[512];
    va_list args; va_start(args, fmt); vsnprintf(line, sizeof line, fmt, args); va_end(args);
    g_env.log(line);
}

bool Matches(const Site& s, std::uint32_t value)
{
    const auto* imm = static_cast<const uint8_t*>(g_env.address(s.imm));
    return imm && !std::memcmp(imm - s.count, s.before, s.count) && !std::memcmp(imm, &value, 4);
}

bool Write(const Site& s, std::uint32_t value)
{
    void* imm = g_env.address(s.imm);
    DWORD old = 0;
    if (!VirtualProtect(imm, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(imm, &value, 4);
    VirtualProtect(imm, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), imm, 4);
    return true;
}

// Every site of one kind (the builders' two, or the seven that read the count) from one value to another, or none.
bool WriteAll(bool builders, std::uint32_t from, std::uint32_t to)
{
    for (const Site& s : kSites) if (s.builder == builders && !Matches(s, from)) return false;
    std::vector<const Site*> done;
    for (const Site& s : kSites)
        if (s.builder == builders)
        {
            if (!Write(s, to)) { for (const Site* d : done) Write(*d, from); return false; }
            done.push_back(&s);
        }
    return true;
}

Vec* Slot(const Table& t, unsigned set) { return static_cast<Vec*>(g_env.address(t.address)) + set; }   // quality index 0
size_t Rows(const Table& t, unsigned set)
{
    const Vec* v = Slot(t, set);
    return v->begin && v->end >= v->begin ? size_t(v->end - v->begin) / t.row : 0;
}
bool TablesHave(size_t rows)
{
    for (const Table& t : kTables) for (unsigned s = 0; s < kSets; ++s) if (Rows(t, s) != rows) return false;
    return true;
}
void RunBuilders()
{
    reinterpret_cast<BuildFn>(g_env.address(kBuildDirs))();
    reinterpret_cast<BuildFn>(g_env.address(kBuildBasis))();
}

// Low's tables as Radiant builds them for 64 rays, without touching the other qualities: the builders run once with
// 64 (which also moves Medium's and High's sets), Low's result is kept, they run again as shipped, and the kept rows
// go back in with the vectors cut to 64 entries.
bool DraftTables()
{
    if (!TablesHave(256)) return false;
    if (!WriteAll(true, 256, 64)) return false;
    RunBuilders();
    std::vector<uint8_t> kept;
    const bool built = TablesHave(64);
    if (built)
        for (const Table& t : kTables) for (unsigned s = 0; s < kSets; ++s) kept.insert(kept.end(), Slot(t, s)->begin, Slot(t, s)->begin + 64 * t.row);
    const bool restored = WriteAll(true, 64, 256);
    if (restored) RunBuilders();
    if (!built || !restored || !TablesHave(256)) return false;
    const uint8_t* from = kept.data();
    for (const Table& t : kTables)
        for (unsigned s = 0; s < kSets; ++s)
        {
            Vec* v = Slot(t, s);
            std::memcpy(v->begin, from, 64 * t.row); from += 64 * t.row;
            v->end = v->begin + 64 * t.row;
        }
    return true;
}

// Null when Low's ray count can be switched now, else what is still running.
const char* Running()
{
    const auto* inflight = static_cast<const volatile int*>(g_env.address(kInflight));
    if (!inflight) return "the job counter was not found";
    if (*inflight) return "probe jobs are running";
    if (g_tracing) return "probe traces are running";
    if (g_busy && g_busy()) return "a bake is running or paused";
    return nullptr;
}

// Main thread, nothing baking. The builders take a moment (they order 8192-ray sets too).
bool SetDraft(bool on)
{
    if (on == Active()) return true;
    if (const char* running = Running()) { g_note = running; return false; }
    try
    {
        if (on)
        {
            for (const Site& s : kSites) if (!Matches(s, 256)) { g_note = std::string("unexpected code at ") + s.what; return false; }
            if (!DraftTables()) { g_note = "the ray tables could not be rebuilt"; return false; }
            if (!WriteAll(false, 256, 64)) { RunBuilders(); g_note = "code page could not be written"; return false; }
            g_lowRays = 64;
        }
        else
        {
            if (!WriteAll(false, 64, 256)) { g_note = "code page could not be written"; return false; }
            g_lowRays = 256;
            RunBuilders();
        }
    }
    catch (...) { g_note = "out of memory"; return false; }
    g_note.clear();
    Log("bake: draft tier %s (Low = %u rays per voxel)", on ? "on" : "off", LowRays());
    return true;
}

std::uint32_t Traced(const std::uint8_t* probe) { return *reinterpret_cast<const std::uint32_t*>(probe + off_Probe_Stride); }   // rays per voxel in its hit store
int Index(const std::uint8_t* probe) { return *reinterpret_cast<const int*>(probe + off_Probe_Base) + *reinterpret_cast<const int*>(probe + off_Probe_Quality); }

void __fastcall hk_BakeStart(int64_t cam, char incremental, char sun, unsigned char probes, char a5)
{
    const bool want = g_wantDraft && g_wantDraft();
    if (want != Active() && !SetDraft(want)) Log("bake: draft tier stays %s for this build: %s", Active() ? "on" : "off", g_note.c_str());
    // Radiant rebuilt its tables since (a second graphics init): Low's go back to 64 entries.
    if (Active() && !g_shifted && TablesHave(256) && !DraftTables()) Log("bake: draft tier: Low's ray tables are Radiant's 256-entry ones again and could not be rebuilt");
    g_draftBuild = want && Active();
    if (g_draftBuild)
        if (auto* quality = static_cast<int*>(g_env.address(kBakeQuality))) *quality = 0;   // LL is Low's slot
    o_BakeStart(cam, incremental, sun, probes, a5);
    g_draftBuild = false;
}

int64_t __fastcall hk_TraceJob(int64_t ctx)
{
    struct Count { Count() { ++g_tracing; } ~Count() { --g_tracing; } } count;
    return o_TraceJob(ctx);
}

int64_t __fastcall hk_ProbeReset(int64_t owner, char full)
{
    // Radiant keeps a traced probe's ray count until its trace is dropped (picking another quality does not). A trace of
    // the other Low count has to go, and LL was picked to bake fast, so it also replaces Medium and High traces.
    if (!full && owner)
        if (const uint8_t* probe = *reinterpret_cast<const uint8_t* const*>(owner + off_Owner_Probe))
            if (Stale(probe) || (g_draftBuild && Traced(probe) && Index(probe) > 0)) { full = 1; ++g_retraced; }
    return o_ProbeReset(owner, full);
}
}

std::uint32_t LowRays() { return g_lowRays; }
bool Switchable() { return g_switchable; }
std::uint32_t KeySalt(int quality) { return Active() && (g_shifted || quality <= 0) ? LowRays() : 0; }

bool Stale(const std::uint8_t* probe)
{
    return Traced(probe) && Index(probe) <= 0 && Traced(probe) != LowRays();
}

std::string Status()
{
    char buf[256];
    if (!Active()) snprintf(buf, sizeof buf, "draft tier off (Low = 256 rays)%s%s", g_note.empty() ? "" : ", last switch refused: ", g_note.c_str());
    else snprintf(buf, sizeof buf, "draft tier on (Low = %u rays per voxel instead of 256); %u probes traced again after a switch", LowRays(), g_retraced.load());
    return buf;
}

void Setup(const br::bakeguide::Env& env)
{
    g_env = env;
    const std::string want = env.get ? env.get("low_rays", "256") : std::string("256");
    if (want != "64") { if (want != "256" && !want.empty()) Log("bake: low_rays=%s is not supported (64 or 256), Low stays at 256", want.c_str()); return; }
    if (!env.address) return;
    for (const Site& s : kSites)
        if (!Matches(s, 256)) { g_note = std::string("unexpected code at ") + s.what; Log("bake: low_rays=64 refused, %s", g_note.c_str()); return; }
    if (static_cast<const Vec*>(env.address(kTables[0].address))->begin)
    { g_note = "Radiant's ray tables are already built"; Log("bake: low_rays=64 refused, %s", g_note.c_str()); return; }
    if (!WriteAll(true, 256, 64)) { g_note = "code page could not be written"; Log("bake: low_rays=64 refused, %s", g_note.c_str()); return; }
    if (!WriteAll(false, 256, 64)) { WriteAll(true, 64, 256); g_note = "code page could not be written"; Log("bake: low_rays=64 refused, %s", g_note.c_str()); return; }
    g_lowRays = 64; g_shifted = true;
    Log("bake: %s", Status().c_str());
}

void SetupEditor(const br::bakeguide::Env& env, bool (*wantDraft)(), bool (*busy)())
{
    g_env = env; g_wantDraft = wantDraft; g_busy = busy;
    if (!env.address || !env.detour) return;
    // The plugin was unloaded with the tier on (it stays on then: the traces in memory are 64-ray ones).
    bool on = true, off = true;
    for (const Site& s : kSites) if (!s.builder) { on = on && Matches(s, 64); off = off && Matches(s, 256); }
    if (on) g_lowRays = 64;
    else if (!off) { g_note = "unexpected code"; Log("bake: draft tier unavailable, unexpected code at the ray count sites"); return; }
    g_switchable = env.detour(env.address(kTraceJob), reinterpret_cast<void*>(&hk_TraceJob), reinterpret_cast<void**>(&o_TraceJob)) &&
                   env.detour(env.address(kProbeReset), reinterpret_cast<void*>(&hk_ProbeReset), reinterpret_cast<void**>(&o_ProbeReset)) &&
                   env.detour(env.address(kBakeStart), reinterpret_cast<void*>(&hk_BakeStart), reinterpret_cast<void**>(&o_BakeStart));
    if (!g_switchable) Log("bake: draft tier unavailable (hooks failed)");
    else Log("bake: %s", Status().c_str());
}
}
