// Crash guard for Radiant: each guard answers one cluster of crash dumps from unmodified Radiant, has an ini switch under [crashguard], is counted ("RadiantPlus crashguard").
// Guards: 1 critical sections, 2 asserts, 3 Entity Info, 4 Brush_Deselect, 5 OpenEXR, 6 asset tree filter, 7 face texture, 8 Brush_Build, 9 material lookup, 10 winding builder,
// 11 load flags left set by a failed map load.
// The "more than 64 faces" assert during map load stays fatal on purpose (continuing overruns a stack table).
#include "modtools_plugin.hpp"
#include "crashguard.h"
#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace br::crashguard
{
namespace
{
    // IDA addresses (no ASLR), resolved through host->RadiantAddress: a different build gets nothing
    const uintptr_t kImpEnterCriticalSection    = 0x146D31B88;   // Radiant's import slot for EnterCriticalSection
    const uintptr_t kImpTryEnterCriticalSection = 0x146D31BF0;   // ... TryEnterCriticalSection
    const uintptr_t kAssertHandler    = 0x140F35CF0;   // (file, line, kind, expr, fmt, ...) -> nonzero = continue, thunk 0x14000C1C6
    const uintptr_t kIdleTimerPtr     = 0x145216990;   // QTimer* driving RadiantMainWin::RoutineProcessing
    const uintptr_t kEntityIsPropType = 0x1408097A0;   // Entity Info: "is_prop_type" of the selected entity's definition
    const uintptr_t kBrushDeselect    = 0x1401EBF00;   // Brush_Deselect(brush_t*): asserts (b->brushFlags & (1 << 7))
    const uintptr_t kTreeSetSelected  = 0x14042C300;   // asset tree proxy: store the selected set, then invalidateFilter()
    const uintptr_t kTreeModelClear   = 0x140420920;   // asset tree source model: removeRows(0, rowCount())
    const uintptr_t kTreePaneRefresh  = 0x1404305C0;   // asset tree pane: selection broadcast -> rebuild the model, filter the proxy
    const uintptr_t kTreeSelectionSlot = 0x140432AE0;  // asset tree pane: tree selection changed -> select in the map, broadcast
    const uintptr_t kFaceSetTexture   = 0x1401E9840;   // copies a texdef onto a face (+0x78 = material, +0x20 = name)
    const uintptr_t kCurrentTexdef    = 0x145341248;   // g_qeglobals.d_texturewin.texdef, the current texture
    const uintptr_t kBrushBuild       = 0x1401DEB00;   // Brush_Build(brush, ...): faces = vector of 864-byte face_t at +144/+152/+160
    const uintptr_t kFaceGather       = 0x1401ED4D0;   // point gather over face-plane triples, only caller Brush_BuildWindings 0x1401DED20
    const uintptr_t kMaterialProp     = 0x1401035F0;   // property of a material handle (map lookup, null -> read at +336)
    const uintptr_t kBrushFileInfo    = 0x1422A5BE8;   // assert "file" record of the brush module, line 0x7E = more than 64 faces
    const uintptr_t kFxMapLoading     = 0x145413B30;   // byte: FX s_mapIsLoading, set and cleared by Map_LoadFile
    const uintptr_t kMapLoading       = 0x1453A1F90;   // byte: map loading, set and cleared by Map_LoadFile
    const uintptr_t kIdaImageBase     = 0x140000000;

    uintptr_t g_imageBase = 0, g_imageEnd = 0;

    uintptr_t IdaAddress(const void* p)
    {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return (v >= g_imageBase && v < g_imageEnd) ? kIdaImageBase + (v - g_imageBase) : v;
    }

    template <typename Fn>
    bool Hook(const char* name, uintptr_t idaAddress, Fn detour, Fn* original)
    {
        void* target = rp::host->RadiantAddress(rp::self, idaAddress);
        if (!target || !rp::host->Detour(rp::self, target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original)))
        {
            rp::Log("crashguard: %s: detour unavailable at 0x%llx", name, static_cast<unsigned long long>(idaAddress));
            return false;
        }
        return true;
    }

    // ---- 1. critical sections ----------------------------------------------------------------------------
    using EnterFn    = void (WINAPI*)(LPCRITICAL_SECTION);
    using TryEnterFn = BOOL (WINAPI*)(LPCRITICAL_SECTION);
    EnterFn    g_origEnter = nullptr;
    TryEnterFn g_origTryEnter = nullptr;
    void**     g_slotEnter = nullptr;
    void**     g_slotTryEnter = nullptr;
    bool       g_csRepair = false;
    std::atomic<long> g_csRepairs{0};
    volatile LONG g_csLock = 0;

    bool ThreadAlive(HANDLE owner)
    {
        const DWORD tid = static_cast<DWORD>(reinterpret_cast<uintptr_t>(owner));
        if (!tid) return false;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
        if (!h) return false;
        DWORD code = 0;
        const bool alive = GetExitCodeThread(h, &code) && code == STILL_ACTIVE;
        CloseHandle(h);
        return alive;
    }

    volatile DWORD g_csLockOwner = 0;

    void RepairCriticalSection(LPCRITICAL_SECTION cs, void* caller)
    {
        const DWORD tid = GetCurrentThreadId();
        const bool nested = g_csLockOwner == tid;   // a log call inside the repair coming back through this hook
        if (!nested)
        {
            while (InterlockedCompareExchange(&g_csLock, 1, 0) != 0) YieldProcessor();
            g_csLockOwner = tid;
        }
        if (cs->DebugInfo == nullptr)   // re-checked under the lock: only one thread repairs a given section
        {
            const char* kind;
            if (cs->LockCount == -1)
            {
                // free but stripped of its DebugInfo: -1 is what InitializeCriticalSection leaves there
                cs->DebugInfo = reinterpret_cast<PRTL_CRITICAL_SECTION_DEBUG>(static_cast<intptr_t>(-1));
                kind = "free, DebugInfo restored";
            }
            else if (!ThreadAlive(cs->OwningThread))
            {
                // zeroed (LockCount 0 reads as locked with no owner) or owned by a dead thread: a fresh section
                InitializeCriticalSection(cs);
                kind = "no live owner, re-initialised";
            }
            else
            {
                // genuinely held by a live thread: -1 lets ntdll wait for it the normal way
                cs->DebugInfo = reinterpret_cast<PRTL_CRITICAL_SECTION_DEBUG>(static_cast<intptr_t>(-1));
                kind = "held by a live thread, DebugInfo restored";
            }
            const long n = ++g_csRepairs;
            if (n <= 20)
                rp::Log("crashguard: critical section 0x%llx had a NULL DebugInfo (%s); caller 0x%llx, thread %lu",
                        static_cast<unsigned long long>(IdaAddress(cs)), kind,
                        static_cast<unsigned long long>(IdaAddress(caller)), tid);
        }
        if (!nested)
        {
            g_csLockOwner = 0;
            InterlockedExchange(&g_csLock, 0);
        }
    }

    void WINAPI Hk_EnterCriticalSection(LPCRITICAL_SECTION cs)
    {
        if (cs && cs->DebugInfo == nullptr) RepairCriticalSection(cs, _ReturnAddress());
        g_origEnter(cs);
    }

    BOOL WINAPI Hk_TryEnterCriticalSection(LPCRITICAL_SECTION cs)
    {
        if (cs && cs->DebugInfo == nullptr) RepairCriticalSection(cs, _ReturnAddress());
        return g_origTryEnter(cs);
    }

    bool PatchImportSlot(void** slot, void* replacement, void** saved)
    {
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
        *saved = *slot;
        *slot = replacement;
        VirtualProtect(slot, sizeof(void*), old, &old);
        return true;
    }

    void RestoreImportSlot(void** slot, void* ours, void* saved)
    {
        if (!slot || !saved || *slot != ours) return;
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return;
        *slot = saved;
        VirtualProtect(slot, sizeof(void*), old, &old);
    }

    void SetupCriticalSections()
    {
        g_slotEnter = static_cast<void**>(rp::host->RadiantAddress(rp::self, kImpEnterCriticalSection));
        g_slotTryEnter = static_cast<void**>(rp::host->RadiantAddress(rp::self, kImpTryEnterCriticalSection));
        if (!g_slotEnter || !g_slotTryEnter)
        {
            rp::Log("crashguard: critical section guard unavailable (import slots not resolved)");
            return;
        }
        // both slots must point into kernel32 or ntdll (a forwarder ends in ntdll)
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll"), nt = GetModuleHandleW(L"ntdll.dll");
        auto inModule = [](void* p, HMODULE m) {
            if (!m || !p) return false;
            auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(m);
            auto* ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<char*>(m) + dos->e_lfanew);
            const uintptr_t base = reinterpret_cast<uintptr_t>(m), v = reinterpret_cast<uintptr_t>(p);
            return v >= base && v < base + ntHeaders->OptionalHeader.SizeOfImage;
        };
        if (!(inModule(*g_slotEnter, k32) || inModule(*g_slotEnter, nt)) || !(inModule(*g_slotTryEnter, k32) || inModule(*g_slotTryEnter, nt)))
        {
            rp::Log("crashguard: critical section guard skipped: import slots already point elsewhere (%p, %p)", *g_slotEnter, *g_slotTryEnter);
            g_slotEnter = g_slotTryEnter = nullptr;
            return;
        }
        void* savedEnter = nullptr; void* savedTry = nullptr;
        if (!PatchImportSlot(g_slotEnter, reinterpret_cast<void*>(&Hk_EnterCriticalSection), &savedEnter))
        {
            rp::Log("crashguard: critical section guard unavailable (VirtualProtect failed)");
            g_slotEnter = g_slotTryEnter = nullptr;
            return;
        }
        g_origEnter = reinterpret_cast<EnterFn>(savedEnter);
        if (PatchImportSlot(g_slotTryEnter, reinterpret_cast<void*>(&Hk_TryEnterCriticalSection), &savedTry))
            g_origTryEnter = reinterpret_cast<TryEnterFn>(savedTry);
        else
            g_slotTryEnter = nullptr;
        g_csRepair = true;
        rp::Log("crashguard: critical section guard on (Enter %s, TryEnter %s)", "patched", g_slotTryEnter ? "patched" : "not patched");
    }

    void ShutdownCriticalSections()
    {
        RestoreImportSlot(g_slotEnter, reinterpret_cast<void*>(&Hk_EnterCriticalSection), reinterpret_cast<void*>(g_origEnter));
        RestoreImportSlot(g_slotTryEnter, reinterpret_cast<void*>(&Hk_TryEnterCriticalSection), reinterpret_cast<void*>(g_origTryEnter));
        g_slotEnter = g_slotTryEnter = nullptr;
        g_csRepair = false;
    }

    // ---- 2. asserts --------------------------------------------------------------------------------------
    enum AssertMode { ASSERT_RADIANT = 0, ASSERT_ASK = 1, ASSERT_CONTINUE = 2 };
    using AssertFn = __int64 (*)(const char* file, unsigned line, unsigned kind, const char* expr, const char* fmt, ...);
    using TimerVoidFn = void (*)(void*);
    using TimerBoolFn = bool (*)(const void*);
    AssertFn    g_origAssert = nullptr;
    bool        g_assertHooked = false;
    int         g_assertMode = ASSERT_ASK;
    std::atomic<long> g_asserts{0};
    volatile LONG g_inAssertBox = 0;
    void**      g_timerPtr = nullptr;
    TimerVoidFn g_timerStop = nullptr;
    TimerVoidFn g_timerStart = nullptr;
    TimerBoolFn g_timerActive = nullptr;

    const char* AssertModeName(int mode)
    {
        return mode == ASSERT_RADIANT ? "radiant" : mode == ASSERT_CONTINUE ? "continue" : "ask";
    }

    int ParseAssertMode(const std::string& s)
    {
        if (s == "radiant" || s == "0") return ASSERT_RADIANT;
        if (s == "continue" || s == "2") return ASSERT_CONTINUE;
        return ASSERT_ASK;
    }

    // the idle timer is paused while the box is up (a ticking timer under Radiant's own dialog caused second crashes)
    bool AskContinue(const char* text)
    {
        void* timer = g_timerPtr ? *g_timerPtr : nullptr;
        const bool pausedTick = timer && g_timerStop && g_timerStart && g_timerActive && g_timerActive(timer);
        if (pausedTick) g_timerStop(timer);
        int shown = 0;
        for (;;) { const int c = ShowCursor(TRUE); ++shown; if (c >= 0 || shown >= 8) break; }
        const int r = MessageBoxA(GetActiveWindow(), text, "Radiant internal check failed (Radiant+ crash guard)",
                                  MB_YESNO | MB_ICONERROR | MB_TASKMODAL | MB_SETFOREGROUND | MB_DEFBUTTON1);
        for (int i = 0; i < shown; ++i) ShowCursor(FALSE);
        if (pausedTick) g_timerStart(timer);
        return r != IDNO;
    }

    __int64 Hk_Assert(const char* file, unsigned line, unsigned kind, const char* expr, const char* fmt, ...)
    {
        char msg[1024] = { 0 };
        if (fmt && *fmt)
        {
            va_list va; va_start(va, fmt);
            _vsnprintf(msg, sizeof msg - 1, fmt, va);
            va_end(va);
        }
        const long n = ++g_asserts;
        rp::Log("crashguard: Radiant assert #%ld: %s (%s:%u, kind %u)%s%s", n, expr ? expr : "?", file ? file : "?", line, kind,
                msg[0] ? ": " : "", msg);
        if (IsDebuggerPresent()) return 0;                                        // break into the debugger, as Radiant does
        if (reinterpret_cast<uintptr_t>(file) == reinterpret_cast<uintptr_t>(rp::host->RadiantAddress(rp::self, kBrushFileInfo)) && line == 0x7E)
        {
            // the stack face table holds 64 entries, continuing overruns it
            rp::Log("crashguard: a brush has more than 64 faces; continuing would overrun Radiant's face table, so this one stays fatal. "
                    "Find the brush with over 64 planes in the .map and split it.");
            return g_origAssert(file, line, kind, expr, "%s", msg);
        }
        if (g_assertMode == ASSERT_RADIANT) return g_origAssert(file, line, kind, expr, "%s", msg);
        if (g_assertMode == ASSERT_CONTINUE) return 1;
        if (InterlockedCompareExchange(&g_inAssertBox, 1, 0) != 0) return 1;      // an assert while the box is up: continue
        char text[2048];
        _snprintf(text, sizeof text - 1,
                  "Radiant hit an internal check and would normally crash here.\n\n%s\n%s:%u\n%s\n\n"
                  "Yes: keep working. Save your map under a new name soon, then restart Radiant.\n"
                  "No: exit Radiant now (unsaved work is lost, as before).\n\n"
                  "Logged by the Radiant+ plugin (RadiantPlugins.log).",
                  expr ? expr : "?", file ? file : "?", line, msg);
        text[sizeof text - 1] = 0;
        const bool go = AskContinue(text);
        InterlockedExchange(&g_inAssertBox, 0);
        if (!go)
        {
            rp::Log("crashguard: exit chosen after the assert");
            ExitProcess(static_cast<UINT>(-1));
        }
        return 1;
    }

    void SetupAsserts()
    {
        g_assertMode = ParseAssertMode(rp::ConfigGet("crashguard", "asserts", "ask"));
        if (g_assertMode == ASSERT_RADIANT) { rp::Log("crashguard: asserts left to Radiant"); return; }
        if (!Hook("assert handler", kAssertHandler, &Hk_Assert, &g_origAssert)) return;
        g_assertHooked = true;
        g_timerPtr = static_cast<void**>(rp::host->RadiantAddress(rp::self, kIdleTimerPtr));
        if (HMODULE core = GetModuleHandleW(L"Qt5Core64r.dll"))
        {
            g_timerStop = reinterpret_cast<TimerVoidFn>(GetProcAddress(core, "?stop@QTimer@@QEAAXXZ"));
            g_timerStart = reinterpret_cast<TimerVoidFn>(GetProcAddress(core, "?start@QTimer@@QEAAXXZ"));
            g_timerActive = reinterpret_cast<TimerBoolFn>(GetProcAddress(core, "?isActive@QTimer@@QEBA_NXZ"));
        }
        rp::Log("crashguard: assert guard on (mode %s)", AssertModeName(g_assertMode));
    }

    // ---- 3. entity info ----------------------------------------------------------------------------------
    using IsPropTypeFn = char (*)(__int64 entity);
    IsPropTypeFn g_origIsPropType = nullptr;
    bool g_entityInfoHooked = false;
    std::atomic<long> g_entityInfoCatches{0};

    int NullReadFilter(unsigned code, EXCEPTION_POINTERS* ep)
    {
        if (code != EXCEPTION_ACCESS_VIOLATION || !ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
        const EXCEPTION_RECORD* r = ep->ExceptionRecord;
        return (r->NumberParameters >= 2 && r->ExceptionInformation[1] < 0x10000) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
    }

    // No C++ objects in this frame: __try needs a frame without unwinding.
    char Hk_IsPropType(__int64 entity)
    {
        __try
        {
            return g_origIsPropType(entity);
        }
        __except (NullReadFilter(GetExceptionCode(), GetExceptionInformation()))
        {
            const long n = ++g_entityInfoCatches;
            if (n <= 5) rp::Log("crashguard: Entity Info read a null entity definition (is_prop_type), answered no (#%ld)", n);
            return 0;
        }
    }

    // ---- 4. Brush_Deselect -------------------------------------------------------------------------------
    using BrushFn = __int64 (*)(void* brush);
    BrushFn g_origBrushDeselect = nullptr;
    bool g_deselectHooked = false;
    std::atomic<long> g_deselectSkips{0};

    __int64 Hk_BrushDeselect(void* brush)
    {
        if (brush && (*reinterpret_cast<uint32_t*>(static_cast<char*>(brush) + 96) & 0x80u) == 0)
        {
            const long n = ++g_deselectSkips;
            if (n <= 5) rp::Log("crashguard: Brush_Deselect on a brush that is not selected, skipped (#%ld; Radiant would assert)", n);
            return 0;
        }
        return g_origBrushDeselect(brush);
    }

    // ---- 7. face texture without a material -------------------------------------------------------------
    using FaceTexFn = unsigned __int64 (*)(__int64 brush, __int64 face, __int64* texdef, char a4, char a5);
    FaceTexFn g_origFaceTex = nullptr;
    bool g_faceTexHooked = false;
    std::atomic<long> g_faceTexSkips{0};

    unsigned __int64 Hk_FaceTex(__int64 brush, __int64 face, __int64* texdef, char a4, char a5)
    {
        if (texdef && texdef[15] == 0)
        {
            const long n = ++g_faceTexSkips;
            if (n <= 5)
            {
                const char* name = reinterpret_cast<const char*>(texdef + 4);                     // std::string at +0x20
                if (static_cast<unsigned __int64>(texdef[7]) >= 0x10) name = reinterpret_cast<const char*>(texdef[4]);
                const bool current = reinterpret_cast<uintptr_t>(texdef) == reinterpret_cast<uintptr_t>(rp::host->RadiantAddress(rp::self, kCurrentTexdef));
                rp::Log("crashguard: texture '%s' has no material%s, face left untextured instead of crashing; "
                        "pick a texture in the texture window (#%ld)", name ? name : "?", current ? " (it is the current texture)" : "", n);
            }
            return 0;
        }
        return g_origFaceTex(brush, face, texdef, a4, a5);
    }

    // ---- 8. Brush_Build on a corrupt brush ----------------------------------------------------------------
    using BrushBuildFn = void (*)(__int64 brush, char a2, char a3, unsigned a4);
    BrushBuildFn g_origBrushBuild = nullptr;
    bool g_brushBuildHooked = false;
    std::atomic<long> g_brushBuildSkips{0};

    bool ReadableRange(const char* begin, const char* end)
    {
        __try
        {
            volatile char c = begin[0];
            c = end[-1];
            (void)c;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadFaceTable(__int64 brush, __int64* b, __int64* e, __int64* cap)
    {
        __try
        {
            *b = *reinterpret_cast<__int64*>(brush + 144);
            *e = *reinterpret_cast<__int64*>(brush + 152);
            *cap = *reinterpret_cast<__int64*>(brush + 160);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // a brush_t keeps its faces in a std::vector<face_t> (864 bytes each), whose begin pointer can be garbage
    bool FaceTableSane(__int64 brush, __int64* b, __int64* e)
    {
        __int64 cap = 0;
        *b = *e = 0;
        if (!brush || !ReadFaceTable(brush, b, e, &cap)) return false;
        if (*b == 0 && *e == 0) return true;
        if (*b == 0 || *e < *b || cap < *e) return false;
        const __int64 bytes = *e - *b;
        if (bytes % 864 != 0 || bytes / 864 > 4096) return false;
        if (bytes == 0) return true;
        return ReadableRange(reinterpret_cast<const char*>(*b), reinterpret_cast<const char*>(*e));
    }

    void Hk_BrushBuild(__int64 brush, char a2, char a3, unsigned a4)
    {
        __int64 b = 0, e = 0;
        if (!FaceTableSane(brush, &b, &e))
        {
            const long n = ++g_brushBuildSkips;
            if (n <= 5) rp::Log("crashguard: Brush_Build on a brush with a corrupt face table (brush 0x%llx, faces 0x%llx..0x%llx), skipped; "
                                "the map may hold a damaged brush, save under a new name and reload (#%ld)",
                                static_cast<unsigned long long>(brush), static_cast<unsigned long long>(b), static_cast<unsigned long long>(e), n);
            return;
        }
        g_origBrushBuild(brush, a2, a3, a4);
    }

    // ---- 10. fewer than three faces in the winding builder ----
    // The gather 0x1401ED4D0 compares its loop bounds unsigned and exits early only on "count - 2 == 0", so one face wraps the outer bound and reads off the
    // end of the face array. Below three faces no triple exists, so it answers 0 points (Radiant then drops the degenerate brush itself).
    using FaceGatherFn = __int64 (*)(__int64 brush, void* addPoint, int maxPoints, void* outPoints);
    FaceGatherFn g_origFaceGather = nullptr;
    bool g_faceGatherHooked = false;
    std::atomic<long> g_faceGatherSkips{0};

    __int64 Hk_FaceGather(__int64 brush, void* addPoint, int maxPoints, void* outPoints)
    {
        __int64 b = 0, e = 0, cap = 0;
        const bool read = brush != 0 && ReadFaceTable(brush, &b, &e, &cap);
        const __int64 faces = (read && e > b) ? (e - b) / 864 : 0;
        if (read && faces >= 3) return g_origFaceGather(brush, addPoint, maxPoints, outPoints);
        const long n = ++g_faceGatherSkips;
        if (n <= 5) rp::Log("crashguard: the winding builder was given a brush with %lld faces (brush 0x%llx), too few for a "
                            "face triple; answered 0 points instead of walking off the face array (#%ld)",
                            static_cast<long long>(faces), static_cast<unsigned long long>(brush), n);
        return 0;
    }

    // ---- 9. material lookup while drawing -----------------------------------------------------------------
    using MaterialPropFn = __int64 (*)(unsigned handle);
    MaterialPropFn g_origMaterialProp = nullptr;
    bool g_materialPropHooked = false;
    std::atomic<long> g_materialPropCatches{0};

    __int64 Hk_MaterialProp(unsigned handle)
    {
        __try
        {
            return g_origMaterialProp(handle);
        }
        __except (NullReadFilter(GetExceptionCode(), GetExceptionInformation()))
        {
            const long n = ++g_materialPropCatches;
            if (n <= 5) rp::Log("crashguard: the 3D view asked for material handle %u which no longer resolves, default answered (#%ld)", handle, n);
            return 6;                                                             // Radiant's answer for an empty entry
        }
    }

    // ---- 11. load flags after a failed map load ----
    // Map_LoadFile clears both flags only when the map has a worldspawn. A missing or empty map leaves them set
    // and the next load stops on the (!s_mapIsLoading) assert in fx_glue_material.cpp.
    std::atomic<long> g_loadFlagResets{0};

    void ResetLoadFlags()
    {
        auto* fx = static_cast<volatile unsigned char*>(rp::host->RadiantAddress(rp::self, kFxMapLoading));
        auto* map = static_cast<volatile unsigned char*>(rp::host->RadiantAddress(rp::self, kMapLoading));
        if (!fx || !map || (*fx == 0 && *map == 0)) return;
        const unsigned fxWas = *fx, mapWas = *map;
        *fx = 0;
        *map = 0;
        const long n = ++g_loadFlagResets;
        if (n <= 5) rp::Log("crashguard: map load ended without a worldspawn (missing or empty map), load flags cleared (fx %u, map %u) (#%ld)",
                            fxWas, mapWas, n);
    }

    // ---- 5. OpenEXR --------------------------------------------------------------------------------------
    bool g_exrWarm = false;

    void WarmOpenExr()
    {
        HMODULE exr = LoadLibraryW(L"openexr64r.dll");
        if (!exr) { rp::Log("crashguard: OpenEXR warm-up skipped, openexr64r.dll not loadable (error %lu)", GetLastError()); return; }
        auto threadCount = reinterpret_cast<int (*)()>(GetProcAddress(exr, "?globalThreadCount@Imf_2_1@@YAHXZ"));
        if (!threadCount) { rp::Log("crashguard: OpenEXR warm-up skipped, globalThreadCount export missing"); return; }
        const int n = threadCount();
        g_exrWarm = true;
        rp::Log("crashguard: OpenEXR global thread pool created on the main thread (%d worker threads)", n);
    }

    // ---- 6. asset tree filter ----------------------------------------------------------------------------
    using SetSelectedFn = void (*)(void* proxy, unsigned char flag, void* set);
    using ClearFn       = int (*)(void* model);
    using InvalidateFn  = void (*)(void* proxy);
    using SourceModelFn = void* (*)(const void* proxy);
    SetSelectedFn g_origSetSelected = nullptr;
    ClearFn       g_origClear = nullptr;
    InvalidateFn  g_invalidate = nullptr;
    using PaneFn        = void (*)(void* pane);
    SourceModelFn g_sourceModel = nullptr;
    PaneFn        g_origPaneRefresh = nullptr;
    PaneFn        g_origSelectionSlot = nullptr;
    InvalidateFn  g_origInvalidateFilter = nullptr;
    bool g_treeHooked = false;
    std::atomic<long> g_treeInvalidations{0};
    std::atomic<long> g_treeReentries{0};
    thread_local int t_treeBusy = 0;                    // > 0 while a pane refresh, proxy filter or model clear runs here
    struct TreeBusy { TreeBusy() { ++t_treeBusy; } ~TreeBusy() { --t_treeBusy; } };
    std::mutex g_treeLock;
    std::unordered_map<void*, void*> g_sourceToProxy;   // source model -> its QSortFilterProxyModel, learned from the filter call

    void Hk_TreeSetSelected(void* proxy, unsigned char flag, void* set)
    {
        if (proxy && g_sourceModel)
        {
            if (void* source = g_sourceModel(proxy))
            {
                std::lock_guard<std::mutex> lock(g_treeLock);
                if (g_sourceToProxy.find(source) == g_sourceToProxy.end())
                    rp::Log("crashguard: asset tree proxy %p over source model %p (filter call)", proxy, source);
                g_sourceToProxy[source] = proxy;
            }
        }
        TreeBusy busy;
        g_origSetSelected(proxy, flag, set);
    }

    void Hk_TreePaneRefresh(void* pane)
    {
        TreeBusy busy;
        g_origPaneRefresh(pane);
    }

    // every other filter change of the pane ends in Qt's invalidateFilter too
    void Hk_InvalidateFilter(void* proxy)
    {
        TreeBusy busy;
        g_origInvalidateFilter(proxy);
    }

    // While the pane filters or rebuilds, a selection change is Qt dropping rows, not the user: acting on it re-enters
    // the filter under Qt's row removal.
    void Hk_TreeSelectionSlot(void* pane)
    {
        if (t_treeBusy > 0)
        {
            const long n = ++g_treeReentries;
            if (n <= 5) rp::Log("crashguard: asset tree pane %p: selection change during its own filter update ignored (#%ld)", pane, n);
            return;
        }
        g_origSelectionSlot(pane);
    }

    int Hk_TreeModelClear(void* model)
    {
        void* proxy = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_treeLock);
            auto it = g_sourceToProxy.find(model);
            if (it != g_sourceToProxy.end()) proxy = it->second;
        }
        static std::atomic<long> clears{0};
        const long c = ++clears;
        TreeBusy busy;
        if (proxy && g_invalidate)
        {
            g_invalidate(proxy);
            const long n = ++g_treeInvalidations;
            if (n <= 5) rp::Log("crashguard: asset tree model %p cleared, its filter proxy mapping rebuilt first (#%ld)", model, n);
        }
        else if (c <= 5)
            rp::Log("crashguard: asset tree model %p cleared, no filter proxy known for it yet (#%ld)", model, c);
        return g_origClear(model);
    }

    void SetupTreeFilter()
    {
        HMODULE core = GetModuleHandleW(L"Qt5Core64r.dll");
        g_invalidate = core ? reinterpret_cast<InvalidateFn>(GetProcAddress(core, "?invalidate@QSortFilterProxyModel@@QEAAXXZ")) : nullptr;
        g_sourceModel = core ? reinterpret_cast<SourceModelFn>(GetProcAddress(core, "?sourceModel@QAbstractProxyModel@@QEBAPEAVQAbstractItemModel@@XZ")) : nullptr;
        if (!g_invalidate || !g_sourceModel) { rp::Log("crashguard: asset tree guard unavailable (Qt exports missing)"); return; }
        const bool a = Hook("asset tree filter", kTreeSetSelected, &Hk_TreeSetSelected, &g_origSetSelected);
        const bool b = Hook("asset tree clear", kTreeModelClear, &Hk_TreeModelClear, &g_origClear);
        const bool c = Hook("asset tree refresh", kTreePaneRefresh, &Hk_TreePaneRefresh, &g_origPaneRefresh);
        const bool d = Hook("asset tree selection", kTreeSelectionSlot, &Hk_TreeSelectionSlot, &g_origSelectionSlot);
        if (d)
        {
            void* filter = reinterpret_cast<void*>(GetProcAddress(core, "?invalidateFilter@QSortFilterProxyModel@@IEAAXXZ"));
            if (!filter || !rp::host->Detour(rp::self, filter, reinterpret_cast<void*>(&Hk_InvalidateFilter), reinterpret_cast<void**>(&g_origInvalidateFilter)))
                rp::Log("crashguard: asset tree: QSortFilterProxyModel::invalidateFilter not hooked, only the refresh path is covered");
        }
        g_treeHooked = a || b || d;
        if (g_treeHooked)
            rp::Log("crashguard: asset tree guard on (filter %s, clear %s, refresh %s, selection %s)", a ? "hooked" : "not hooked",
                    b ? "hooked" : "not hooked", c ? "hooked" : "not hooked", d ? "hooked" : "not hooked");
    }

    // ---- settings --------------------------------------------------------------------------------------
    bool g_enabled = false;
    bool g_wantCs = true, g_wantAsserts = true, g_wantEntityInfo = true, g_wantDeselect = true, g_wantExr = true, g_wantTree = true;
    bool g_wantFaceTex = true, g_wantBrushBuild = true, g_wantMaterialProp = true, g_wantFaceGather = true, g_wantLoadFlags = true;

    std::string Status()
    {
        char buf[1024];
        _snprintf(buf, sizeof buf - 1,
                  "crashguard: %s; critical sections %s (repairs %ld); asserts %s (seen %ld); entity info %s (caught %ld); "
                  "brush deselect %s (skipped %ld); OpenEXR warm-up %s; asset tree %s (mapping rebuilds %ld, re-entries ignored %ld); "
                  "face texture %s (skipped %ld); brush build %s (skipped %ld); material lookup %s (caught %ld); "
                  "winding faces %s (skipped %ld); load flags %s (cleared %ld)",
                  g_enabled ? "on" : "off",
                  g_csRepair ? "guarded" : "off", g_csRepairs.load(),
                  g_assertHooked ? AssertModeName(g_assertMode) : "radiant", g_asserts.load(),
                  g_entityInfoHooked ? "guarded" : "off", g_entityInfoCatches.load(),
                  g_deselectHooked ? "guarded" : "off", g_deselectSkips.load(),
                  g_exrWarm ? "done" : "off",
                  g_treeHooked ? "guarded" : "off", g_treeInvalidations.load(), g_treeReentries.load(),
                  g_faceTexHooked ? "guarded" : "off", g_faceTexSkips.load(),
                  g_brushBuildHooked ? "guarded" : "off", g_brushBuildSkips.load(),
                  g_materialPropHooked ? "guarded" : "off", g_materialPropCatches.load(),
                  g_faceGatherHooked ? "guarded" : "off", g_faceGatherSkips.load(),
                  (g_enabled && g_wantLoadFlags) ? "guarded" : "off", g_loadFlagResets.load());
        buf[sizeof buf - 1] = 0;
        return buf;
    }
}

// ---- beam reload ---------------------------------------------------------------------------------------
// CCamWnd::ReloadAssets " Beams" reloads every beam flagged changed and dereferences the result without a null check:
// a beam an effect names that is not in gdt.db kills Radiant.
namespace
{
    const uintptr_t kBeamReloadAll = 0x1409B5ED0;   // (): under the SRW lock below
    const uintptr_t kBeamLock      = 0x1452158C8;   // SRWLOCK the reload holds while it walks the beam map
    using VoidFn = void (*)();
    VoidFn g_origBeamReload = nullptr;
    bool g_beamReloadHooked = false, g_wantBeamReload = true, g_beamReloadBlocked = false;

    void Hk_BeamReload()
    {
        if (g_beamReloadBlocked) return;
        __try
        {
            g_origBeamReload();
        }
        __except (NullReadFilter(GetExceptionCode(), GetExceptionInformation()))
        {
            if (void* lock = rp::host->RadiantAddress(rp::self, kBeamLock)) ReleaseSRWLockExclusive(static_cast<PSRWLOCK>(lock));
            g_beamReloadBlocked = true;
            rp::Log("crashguard: the beam reload read a beam that is not in gdt.db (an effect names it); beam reloads are off until Radiant restarts");
        }
    }
}

void Setup()
{
    g_enabled = rp::ConfigGetBool("crashguard", "enabled", true);
    if (!g_enabled) { rp::Log("crashguard: off ([crashguard] enabled=0)"); return; }
    if (HMODULE exe = GetModuleHandleW(nullptr))
    {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<char*>(exe) + dos->e_lfanew);
        g_imageBase = reinterpret_cast<uintptr_t>(exe);
        g_imageEnd = g_imageBase + nt->OptionalHeader.SizeOfImage;
    }
    g_wantCs = rp::ConfigGetBool("crashguard", "critical_sections", true);
    g_wantAsserts = true;
    g_wantEntityInfo = rp::ConfigGetBool("crashguard", "entity_info", true);
    g_wantDeselect = rp::ConfigGetBool("crashguard", "brush_deselect", true);
    g_wantExr = rp::ConfigGetBool("crashguard", "openexr_warmup", true);
    g_wantTree = rp::ConfigGetBool("crashguard", "asset_tree", true);
    g_wantFaceTex = rp::ConfigGetBool("crashguard", "face_texture", true);
    g_wantBrushBuild = rp::ConfigGetBool("crashguard", "brush_build", true);
    g_wantMaterialProp = rp::ConfigGetBool("crashguard", "material_lookup", true);
    g_wantFaceGather = rp::ConfigGetBool("crashguard", "winding_faces", true);
    g_wantLoadFlags = rp::ConfigGetBool("crashguard", "load_flags", true);
    g_wantBeamReload = rp::ConfigGetBool("crashguard", "beam_reload", true);

    if (g_wantCs) SetupCriticalSections();
    if (g_wantExr) WarmOpenExr();
    SetupAsserts();
    if (g_wantEntityInfo) g_entityInfoHooked = Hook("entity info is_prop_type", kEntityIsPropType, &Hk_IsPropType, &g_origIsPropType);
    if (g_wantDeselect) g_deselectHooked = Hook("Brush_Deselect", kBrushDeselect, &Hk_BrushDeselect, &g_origBrushDeselect);
    if (g_wantTree) SetupTreeFilter();
    if (g_wantFaceTex) g_faceTexHooked = Hook("face texture", kFaceSetTexture, &Hk_FaceTex, &g_origFaceTex);
    if (g_wantBrushBuild) g_brushBuildHooked = Hook("Brush_Build", kBrushBuild, &Hk_BrushBuild, &g_origBrushBuild);
    if (g_wantMaterialProp) g_materialPropHooked = Hook("material lookup", kMaterialProp, &Hk_MaterialProp, &g_origMaterialProp);
    if (g_wantFaceGather) g_faceGatherHooked = Hook("winding face gather", kFaceGather, &Hk_FaceGather, &g_origFaceGather);
    if (g_wantLoadFlags) ResetLoadFlags();                                 // a hot load after a failed startup map
    if (g_wantBeamReload) g_beamReloadHooked = Hook("beam reload", kBeamReloadAll, &Hk_BeamReload, &g_origBeamReload);
    rp::Log("%s", Status().c_str());
}

void Shutdown()
{
    ShutdownCriticalSections();
    {
        std::lock_guard<std::mutex> lock(g_treeLock);
        g_sourceToProxy.clear();
    }
    if (g_enabled) rp::Log("%s", Status().c_str());
    g_assertHooked = g_entityInfoHooked = g_deselectHooked = g_treeHooked = false;
    g_faceTexHooked = g_brushBuildHooked = g_materialPropHooked = g_faceGatherHooked = g_beamReloadHooked = false;
    g_beamReloadBlocked = false;
    g_enabled = false;
}

void AfterMapLoad()
{
    if (g_enabled && g_wantLoadFlags) ResetLoadFlags();
}

bool Command(const std::string& command, std::string& reply)
{
    if (command.rfind("crashguard", 0) != 0) return false;
    std::string rest = command.size() > 10 ? command.substr(10) : std::string();
    while (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
    if (rest.empty() || rest == "status") { reply = Status(); return true; }
    if (rest.rfind("asserts ", 0) == 0)
    {
        const std::string mode = rest.substr(8);
        if (mode != "ask" && mode != "continue" && mode != "radiant") { reply = "crashguard asserts ask|continue|radiant"; return true; }
        rp::ConfigSet("crashguard", "asserts", mode);
        g_assertMode = ParseAssertMode(mode);
        reply = g_assertHooked ? "crashguard: asserts now " + mode
                               : "crashguard: asserts " + mode + " saved; the assert hook installs at the next load";
        return true;
    }
    if (rest == "test-assert")
    {
        // exercises the detour path exactly as Radiant's thunk would: log, then ask or continue
        const __int64 r = Hk_Assert("crashguard.cpp", 0, 0, "(test assert from the command channel)", "manual test %d", 1);
        reply = "crashguard: test assert handled, result " + std::to_string(static_cast<long long>(r));
        return true;
    }
    if (rest == "test-cs")
    {
        if (!g_csRepair || !g_slotEnter) { reply = "crashguard: critical section guard is off"; return true; }
        CRITICAL_SECTION cs; memset(&cs, 0, sizeof cs);                       // like the crashing sections
        reinterpret_cast<EnterFn>(*g_slotEnter)(&cs);                         // through Radiant's patched import slot
        LeaveCriticalSection(&cs);
        DeleteCriticalSection(&cs);
        reply = "crashguard: zeroed critical section entered and repaired, repairs now " + std::to_string(g_csRepairs.load());
        return true;
    }
    reply = "crashguard [status] | asserts ask|continue|radiant | test-assert | test-cs";
    return true;
}
}
