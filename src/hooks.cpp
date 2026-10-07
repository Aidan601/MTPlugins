#include <windows.h>
#include <objbase.h>
#include <shlwapi.h>
#include <mutex>
#include <set>
#include <string>
#include <atomic>
#include <array>
#include <cstring>
#include "MinHook.h"
#include "hooks.h"
#include "radiant_addrs.h"
#include "mainthread.h"
#include "render.h"
#include "plugins.h"
#include "dock.h"
#include "pluginmgr.h"
#include "qt.h"
#include "ui.h"
#include "config.h"
#include "log.h"

// MAP (grep the names, line numbers drift):
//   Radiant function shapes ... Map_LoadFile_t, Map_SaveFile_t, ... (typedefs), the o_* originals, kImportFlags
//   State ..................... g_mutex guards g_currentMap, g_reloading, g_maximizedOnce, g_ownWriteDepth,
//                               g_ownWriteEndTick
//   Address helpers ........... Global<T>, Func<Fn> (rt::Rva over src/radiant_addrs.h), Widen, Narrow,
//                               ResolveMapPath, CamWnd
//   Detours ................... hk_Map_LoadFile, hk_Map_SaveFile, hk_Map_EnterPrefab, hk_Map_LeavePrefab
//                               (OnMapLoaded, NotifyScope), hk_RoutineProcessing (the main-thread tick),
//                               hk_Cam_Draw, hk_CamDrawHud, hk_XY_Draw (overlay), hk_QuestionBox (reload
//                               prompts), SaveCamera, RestoreCamera
//   Install ................... Hook, Install (MinHook, after rt::VerifyBinary), Remove
//   namespace hooks (public, main thread unless noted)
//     map / scope ............. CurrentMapPath (under g_mutex), CurrentScopePath, Modified, PrefabDepth,
//                               IsReloading, ReloadMapNow, MaximizeMainWindow, BeginOwnWrite, EndOwnWrite,
//                               OwnWriteRecent
//     camera .................. GetCamera, SetCamera, GameView, SetGameView
//     selection ............... GetSelection, SelectedBrushPtrs, SelectBrushes, SelectEntityByGuid, DeselectAll,
//                               GuidText
//     brush access ............ MoveBrush, FindBrushesByGuid, FindEntitiesByGuid, ParseGuidText, FaceWalk,
//                               BrushPlanes, SetBrushPlanes, BrushBounds, EntityFixedSize, FindBrushInst
//     patch vertices .......... PatchBrushByGuid, PatchGrid, PatchSelectVerts, PatchSelectedVertCount, PatchWeldSelected,
//                               DragMode (file end, PatchViewSeh, ConnectSeh)
//     entity walk / text ...... ForEachEntity, ForEachEntityBrush, EntityGuid, BrushGuid, BrushOwner, WorldEntity,
//                               WriteEntityText, WriteEntityTextWith
//     prefabs ................. EnterSelectedPrefab, LeavePrefab
//     import / delete ......... ImportMapText, DeleteBrushes, RestampImportedGuids, RestampImportedEntities (RestampEntity,
//                               GuidFromText, RegisteredInstsOf)
//     undo .................... BeginUndo, EndUndo, InternLabel (label pointers must outlive the undo step),
//                               GetUndoTags, SetUndoTags, BeginUntracked, EndUntracked, g_untracked

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")

namespace init { void BeforeMapLoad(); }   // dllmain.cpp

namespace
{
    // ---- Radiant function shapes (x64 MSVC) --------
    using Map_LoadFile_t      = __int64(__fastcall*)(char* filename);
    using Map_SaveFile_t      = char(__fastcall*)(char* filename, char silentAutosave, char saveSelected);
    using Bookmarks_Save_t    = char(__fastcall*)(void* bookmarks, const char* mapName);
    using Bookmarks_Count_t   = int(__fastcall*)(void* bookmarks);
    using RoutineProcessing_t = __int64(__fastcall*)(void* mainWin);
    using Entity_Write_t      = int(__fastcall*)(BYTE* entity, void* crtFile, char selectedOnly);
    using Map_ImportBuffer_t  = __int64(__fastcall*)(char** text, unsigned int flags, unsigned char regenerateGuids);
    using ParseSession_t      = void(__fastcall*)(const char* name);
    using Void_t              = void(__fastcall*)();
    using UndoBegin_t         = __int64(__fastcall*)(const char* label);
    using Collection1_t       = __int64(__fastcall*)(void* collection);
    using Collection2_t       = __int64(__fastcall*)(void* collection, void* brush);
    using EndBatch_t          = __int64(__fastcall*)(void* collection, int, __int64, char);
    using Deselect_t          = __int64(__fastcall*)(char clearDrag);
    constexpr unsigned kImportFlags = 4;   // Radiant's paste value: selects the mesh/curve syntax in Brush_Parse (0 = old patchDef5, fatal '{ not found')
    // draw arity unknown: 4 register args passed through
    using Cam_Draw_t          = __int64(__fastcall*)(BYTE* camWnd, __int64 a2, __int64 a3, __int64 a4);
    using XY_Draw_t           = __int64(__fastcall*)(BYTE* xyWnd, __int64* a2, __int64 a3, __int64 a4);
    // category callback routing a fixed-size misc_model through the generic culler, then the XMODEL renderer
    using ModelDispatcher_t   = void(__fastcall*)(unsigned int a1, BYTE* brushInst, __int64 a3, __int64 a4, __int64 a5, char a6);
    // called first for fixed-size models, true = outside the frustum. Bounds are brush_t +88/+100, not the BrushInst cache at +480
    using GenericBrushCull_t  = bool(__fastcall*)(float* cameraPlanes, BYTE* brushInst);
    // second fixed-size model culler (normal camera context), same brush_t bounds
    using AlternateBrushCull_t = char(__fastcall*)(BYTE* culler, BYTE* brushInst);
    // Map_EnterPrefab(EntityInst* or null, char showAll, char flag) / Map_LeavePrefab(char updateTitle), 4 regs passed through
    using EnterPrefab_t       = __int64(__fastcall*)(__int64 a1, __int64 a2, __int64 a3, __int64 a4);
    using LeavePrefab_t       = __int64(__fastcall*)(__int64 a1, __int64 a2, __int64 a3, __int64 a4);

    Map_LoadFile_t      o_Map_LoadFile = nullptr;
    using QuestionBox_t = __int64(__fastcall*)(void* parent, const char* text, const char* title, int buttons, int defaultButton);
    QuestionBox_t       o_QuestionBox = nullptr;
    Map_SaveFile_t      o_Map_SaveFile = nullptr;
    RoutineProcessing_t o_RoutineProcessing = nullptr;
    Cam_Draw_t          o_Cam_Draw = nullptr;
    XY_Draw_t           o_XY_Draw = nullptr;
    Cam_Draw_t          o_CamDrawHud = nullptr;
    EnterPrefab_t       o_Map_EnterPrefab = nullptr;
    LeavePrefab_t       o_Map_LeavePrefab = nullptr;
    ModelDispatcher_t   o_ModelDispatcher = nullptr;
    GenericBrushCull_t  o_GenericBrushCull = nullptr;
    AlternateBrushCull_t o_AlternateBrushCull = nullptr;

    std::mutex   g_mutex;
    std::wstring g_currentMap;
    bool         g_reloading = false;    // our own reload in flight
    bool         g_maximizedOnce = false;
    std::atomic<int> g_ownWriteDepth{ 0 };
    ULONGLONG    g_ownWriteEndTick = 0;

    constexpr size_t kNativeModelCullBoundsLimit = 32;
    struct NativeModelCullBounds
    {
        BYTE* brush = nullptr;
        BYTE* entity = nullptr;
        BYTE* brushInst = nullptr;             // learned only from a live dispatcher call
        float mins[3] = {};
        float maxs[3] = {};
    };
    // main thread only: a flat table avoids a lock or allocation per model dispatch
    std::array<NativeModelCullBounds, kNativeModelCullBoundsLimit> g_nativeModelCullBounds{};
    size_t g_nativeModelCullBoundsCount = 0;

    NativeModelCullBounds* FindNativeModelCullBounds(BYTE* brushInst)
    {
        if (!brushInst) return nullptr;
        BYTE* brush = *reinterpret_cast<BYTE**>(brushInst + rt::off_BInst_Brush);
        BYTE* entityInst = *reinterpret_cast<BYTE**>(brushInst + rt::off_BInst_EntityInst);
        BYTE* entity = entityInst ? *reinterpret_cast<BYTE**>(entityInst + rt::off_EInst_Entity) : nullptr;
        for (size_t i = 0; i < g_nativeModelCullBoundsCount; ++i)
            if (g_nativeModelCullBounds[i].brush == brush ||
                (entity && g_nativeModelCullBounds[i].entity == entity)) return &g_nativeModelCullBounds[i];
        return nullptr;
    }

    // unused root-cache reinsertion experiment
#if 0
    struct BrushCacheCategoryEntry
    {
        BYTE* brushInst = nullptr;
        uint32_t flags = 0;
        uint32_t reserved = 0;
    };
    static_assert(sizeof(BrushCacheCategoryEntry) == 16, "Radiant's category record is 16 bytes");

    struct BrushCacheCategoryVector
    {
        BrushCacheCategoryEntry* first = nullptr;
        BrushCacheCategoryEntry* last = nullptr;
        BrushCacheCategoryEntry* capacity = nullptr;
    };
    static_assert(sizeof(BrushCacheCategoryVector) == 24, "Radiant's category vector is three pointers");

    struct BrushCacheRootEntry
    {
        uint32_t arenaIndex = 0;
        uint32_t passMask = 0;
    };
    static_assert(sizeof(BrushCacheRootEntry) == 8, "Radiant's root cache record is index plus pass mask");

    struct BrushCacheRootVector
    {
        BrushCacheRootEntry* first = nullptr;
        BrushCacheRootEntry* last = nullptr;
        BrushCacheRootEntry* capacity = nullptr;
    };
    static_assert(sizeof(BrushCacheRootVector) == 24, "Radiant's root cache vector is three pointers");

    bool ValidBrushCacheCategoryVector(const BrushCacheCategoryVector& list)
    {
        if (!list.first || !list.last || !list.capacity) return list.first == list.last && list.last == list.capacity;
        const uintptr_t first = reinterpret_cast<uintptr_t>(list.first);
        const uintptr_t last = reinterpret_cast<uintptr_t>(list.last);
        const uintptr_t capacityEnd = reinterpret_cast<uintptr_t>(list.capacity);
        if (first > last || last > capacityEnd || (last - first) % sizeof(BrushCacheCategoryEntry) ||
            (capacityEnd - first) % sizeof(BrushCacheCategoryEntry)) return false;
        const size_t size = static_cast<size_t>((last - first) / sizeof(BrushCacheCategoryEntry));
        const size_t capacity = static_cast<size_t>((capacityEnd - first) / sizeof(BrushCacheCategoryEntry));
        return size <= capacity && capacity <= 32768;
    }

    bool ReadableBrushCachePointer(const void* address, size_t bytes)
    {
        if (!address || !bytes) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(address, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        const uintptr_t begin = reinterpret_cast<uintptr_t>(address);
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        return begin <= regionEnd && bytes <= regionEnd - begin;
    }

    bool ValidBrushCacheRootVector(const BrushCacheRootVector& list)
    {
        if (!list.first || !list.last || !list.capacity) return list.first == list.last && list.last == list.capacity;
        const uintptr_t first = reinterpret_cast<uintptr_t>(list.first);
        const uintptr_t last = reinterpret_cast<uintptr_t>(list.last);
        const uintptr_t capacityEnd = reinterpret_cast<uintptr_t>(list.capacity);
        if (first > last || last > capacityEnd || (last - first) % sizeof(BrushCacheRootEntry) ||
            (capacityEnd - first) % sizeof(BrushCacheRootEntry)) return false;
        const size_t size = static_cast<size_t>((last - first) / sizeof(BrushCacheRootEntry));
        const size_t capacity = static_cast<size_t>((capacityEnd - first) / sizeof(BrushCacheRootEntry));
        return size <= capacity && capacity <= 1048576;
    }

    BYTE* BrushCacheRootRecord(uint32_t arenaIndex)
    {
        const uintptr_t arenaAddress = rt::Rva(rt::g_BrushCacheArena);
        if (!ReadableBrushCachePointer(reinterpret_cast<const void*>(arenaAddress), sizeof(uintptr_t))) return nullptr;
        const uintptr_t arena = *reinterpret_cast<const uintptr_t*>(arenaAddress);
        constexpr uintptr_t maxIndex = (UINTPTR_MAX - rt::kBrushCacheArenaRecordOffset) / rt::kBrushCacheArenaRecordStride;
        if (!arena || arenaIndex > maxIndex) return nullptr;
        const uintptr_t address = arena + rt::kBrushCacheArenaRecordOffset +
            static_cast<uintptr_t>(arenaIndex) * rt::kBrushCacheArenaRecordStride;
        if (!ReadableBrushCachePointer(reinterpret_cast<const void*>(address), rt::kBrushCacheArenaRecordStride)) return nullptr;
        return reinterpret_cast<BYTE*>(address);
    }

    BYTE* BrushCacheRecordForInst(const BYTE* brushInst)
    {
        if (!brushInst || !ReadableBrushCachePointer(brushInst + rt::off_BInst_RootCacheRecord, sizeof(BYTE*))) return nullptr;
        BYTE* record = *reinterpret_cast<BYTE* const*>(brushInst + rt::off_BInst_RootCacheRecord);
        return ReadableBrushCachePointer(record, 1) ? record : nullptr;
    }

    void RefreshNativeModelRootBindings()
    {
        BrushCacheRootVector* list = reinterpret_cast<BrushCacheRootVector*>(rt::Rva(rt::g_BrushCacheRootEntries));
        if (!list || !ValidBrushCacheRootVector(*list)) return;
        for (size_t bound = 0; bound < g_nativeModelCullBoundsCount; ++bound)
        {
            NativeModelCullBounds& target = g_nativeModelCullBounds[bound];
            if (!target.brushInst || target.rootEntryIndex != 0xFFFFFFFFu) continue;
            const BYTE* targetRecord = BrushCacheRecordForInst(target.brushInst);
            if (!targetRecord) continue;
            for (const BrushCacheRootEntry* entry = list->first; entry != list->last; ++entry)
            {
                BYTE* entryRecord = BrushCacheRootRecord(entry->arenaIndex);
                if (entryRecord != targetRecord) continue;
                target.rootEntryIndex = entry->arenaIndex;
                target.rootEntryFlags = entry->passMask;
                LOG("cull: root cache bound brush %p index=%u flags=0x%X record=%p dispatch=%p",
                    target.brush, target.rootEntryIndex, target.rootEntryFlags, entryRecord, target.brushInst);
                break;
            }
            if (target.rootEntryIndex == 0xFFFFFFFFu && !target.rootEntryProbeLogged)
            {
                LOG("cull: root cache did not find brush %p in default list %zu/%zu", target.brush,
                    static_cast<size_t>(list->last - list->first), static_cast<size_t>(list->capacity - list->first));
                target.rootEntryProbeLogged = true;
            }
        }
    }

    void ReinsertNativeModelRootEntries()
    {
        BrushCacheRootVector* list = reinterpret_cast<BrushCacheRootVector*>(rt::Rva(rt::g_BrushCacheRootEntries));
        if (!list || !ValidBrushCacheRootVector(*list)) return;
        RefreshNativeModelRootBindings();
        for (size_t bound = 0; bound < g_nativeModelCullBoundsCount; ++bound)
        {
            NativeModelCullBounds& target = g_nativeModelCullBounds[bound];
            if (target.rootEntryIndex == 0xFFFFFFFFu) continue;
            BYTE* targetRecord = BrushCacheRecordForInst(target.brushInst);
            if (!targetRecord || BrushCacheRootRecord(target.rootEntryIndex) != targetRecord)
            {
                target.rootEntryIndex = 0xFFFFFFFFu;
                target.rootEntryFlags = 0;
                target.rootEntryProbeLogged = false;
                continue;
            }
            bool present = false;
            for (const BrushCacheRootEntry* entry = list->first; entry != list->last; ++entry)
            {
                if (entry->arenaIndex == target.rootEntryIndex) { present = true; break; }
            }
            if (present || list->last == list->capacity) continue;
            *list->last++ = { target.rootEntryIndex, target.rootEntryFlags };
            LOG("cull: root cache reinserted brush %p index=%u flags=0x%X", target.brush,
                target.rootEntryIndex, target.rootEntryFlags);
        }
    }

    void RefreshNativeModelCategoryBindings()
    {
        BrushCacheCategoryVector* lists = reinterpret_cast<BrushCacheCategoryVector*>(rt::Rva(rt::g_BrushCacheCategories));
        if (!lists) return;
        for (size_t category = 0; category < rt::kBrushCacheCategoryCount; ++category)
        {
            const BrushCacheCategoryVector& list = lists[category];
            if (!ValidBrushCacheCategoryVector(list)) continue;
            for (size_t bound = 0; bound < g_nativeModelCullBoundsCount; ++bound)
            {
                NativeModelCullBounds& target = g_nativeModelCullBounds[bound];
                if (!target.brushInst || target.categoryIndex != 0xFF) continue;
                for (const BrushCacheCategoryEntry* entry = list.first; entry != list.last; ++entry)
                {
                    // The typed vectors store the BrushCache arena record, not the BrushInst:
                    // BrushCache_Add links them through BrushInst+176, the identity across camera categories.
                    BYTE* targetRecord = BrushCacheRecordForInst(target.brushInst);
                    if (!targetRecord || entry->brushInst != targetRecord) continue;
                    target.categoryIndex = static_cast<uint8_t>(category);
                    target.categoryFlags = entry->flags;
                    target.categoryRecord = entry->brushInst;
                    LOG("cull: root cache bound brush %p to category %zu flags=0x%X record=%p dispatch=%p",
                        target.brush, category, entry->flags, target.categoryRecord, target.brushInst);
                    break;
                }
            }
        }
        for (size_t bound = 0; bound < g_nativeModelCullBoundsCount; ++bound)
        {
            NativeModelCullBounds& target = g_nativeModelCullBounds[bound];
            if (!target.brushInst || target.categoryIndex != 0xFF || target.rootProbeLogged) continue;
            std::string categories;
            for (size_t category = 0; category < rt::kBrushCacheCategoryCount; ++category)
            {
                const BrushCacheCategoryVector& list = lists[category];
                if (!ValidBrushCacheCategoryVector(list)) { categories += " invalid"; continue; }
                categories += " " + std::to_string(list.last - list.first) + "/" + std::to_string(list.capacity - list.first);
            }
            LOG("cull: root cache did not find live BrushInst %p for brush %p in categories:%s", target.brushInst, target.brush, categories.c_str());
            target.rootProbeLogged = true;
        }
    }

    void ReinsertNativeModelCategoryEntries()
    {
        BrushCacheCategoryVector* lists = reinterpret_cast<BrushCacheCategoryVector*>(rt::Rva(rt::g_BrushCacheCategories));
        if (!lists) return;
        ReinsertNativeModelRootEntries();
        // A source can be absent from the current camera category (rest AABB offscreen, arena record live):
        // the process hook scopes the pass across Prepare so it is admitted only to the category being traversed.
        size_t activeCategory = rt::kBrushCacheCategoryCount;
        switch (g_brushCacheProcessingPass)
        {
        case 0x10: activeCategory = 0; break;
        case 0x40: activeCategory = 1; break;
        case 0x100: activeCategory = 2; break;
        case 0x1100: activeCategory = 3; break;
        case 0x40000: activeCategory = 4; break;
        case 0x600: activeCategory = 5; break;
        }
        if (activeCategory < rt::kBrushCacheCategoryCount && ValidBrushCacheCategoryVector(lists[activeCategory]))
        {
            BrushCacheCategoryVector& activeList = lists[activeCategory];
            for (size_t bound = 0; bound < g_nativeModelCullBoundsCount; ++bound)
            {
                NativeModelCullBounds& target = g_nativeModelCullBounds[bound];
                BYTE* record = BrushCacheRecordForInst(target.brushInst);
                if (!record) continue;
                bool present = false;
                for (const BrushCacheCategoryEntry* entry = activeList.first; entry != activeList.last; ++entry)
                {
                    if (entry->brushInst == record) { present = true; break; }
                }
                if (present || activeList.last == activeList.capacity) continue;
                *activeList.last++ = { record, target.categoryFlags, 0 };
                LOG("cull: root cache admitted brush %p to active camera category %zu", target.brush, activeCategory);
            }
        }
        RefreshNativeModelCategoryBindings();
        for (size_t bound = 0; bound < g_nativeModelCullBoundsCount; ++bound)
        {
            NativeModelCullBounds& target = g_nativeModelCullBounds[bound];
            if (!target.categoryRecord || target.categoryIndex >= rt::kBrushCacheCategoryCount) continue;
            BrushCacheCategoryVector& list = lists[target.categoryIndex];
            if (!ValidBrushCacheCategoryVector(list)) continue;
            bool present = false;
            for (const BrushCacheCategoryEntry* entry = list.first; entry != list.last; ++entry)
            {
                if (entry->brushInst == target.categoryRecord) { present = true; break; }
            }
            if (present || list.last == list.capacity) continue;
            *list.last++ = { target.categoryRecord, target.categoryFlags, 0 };
            LOG("cull: root cache reinserted brush %p in category %u", target.brush, target.categoryIndex);
        }
    }

    __int64 __fastcall hk_BrushCachePrepare(__int64 a1, __int64 a2)
    {
        const __int64 result = o_BrushCachePrepare ? o_BrushCachePrepare(a1, a2) : 0;
        // Never allocate or grow Radiant's private vectors. The bridge is
        // capacity-only, so an unfamiliar cache layout merely retains the safe
        // public fallback rather than perturbing renderer ownership.
        ReinsertNativeModelCategoryEntries();
        return result;
    }

    __int64 __fastcall hk_BrushCacheProcessRadBrushes(int passMask, __int64 callback)
    {
        const int previousPass = g_brushCacheProcessingPass;
        g_brushCacheProcessingPass = passMask;
        const __int64 result = o_BrushCacheProcessRadBrushes ? o_BrushCacheProcessRadBrushes(passMask, callback) : 0;
        g_brushCacheProcessingPass = previousPass;
        return result;
    }

#endif
    // Both early cullers read brush_t's padded bounds: swap in the envelope for the test only, then restore the brush
    // (forcing "visible" was not enough for the normal camera branch)
    bool __fastcall hk_GenericBrushCull(float* cameraPlanes, BYTE* brushInst)
    {
        const NativeModelCullBounds* bounds = FindNativeModelCullBounds(brushInst);
        if (!bounds || !o_GenericBrushCull) return o_GenericBrushCull ? o_GenericBrushCull(cameraPlanes, brushInst) : true;
        BYTE* brush = *reinterpret_cast<BYTE**>(brushInst + rt::off_BInst_Brush);
        if (!brush) return o_GenericBrushCull(cameraPlanes, brushInst);
        float oldMins[3], oldMaxs[3];
        memcpy(oldMins, brush + rt::off_Brush_PaddedMins, sizeof oldMins);
        memcpy(oldMaxs, brush + rt::off_Brush_PaddedMaxs, sizeof oldMaxs);
        memcpy(brush + rt::off_Brush_PaddedMins, bounds->mins, sizeof bounds->mins);
        memcpy(brush + rt::off_Brush_PaddedMaxs, bounds->maxs, sizeof bounds->maxs);
        const bool result = o_GenericBrushCull(cameraPlanes, brushInst);
        memcpy(brush + rt::off_Brush_PaddedMins, oldMins, sizeof oldMins);
        memcpy(brush + rt::off_Brush_PaddedMaxs, oldMaxs, sizeof oldMaxs);
        return result;
    }

    char __fastcall hk_AlternateBrushCull(BYTE* culler, BYTE* brushInst)
    {
        const NativeModelCullBounds* bounds = FindNativeModelCullBounds(brushInst);
        if (!bounds || !o_AlternateBrushCull) return o_AlternateBrushCull ? o_AlternateBrushCull(culler, brushInst) : 1;
        BYTE* brush = *reinterpret_cast<BYTE**>(brushInst + rt::off_BInst_Brush);
        if (!brush) return o_AlternateBrushCull(culler, brushInst);
        float oldMins[3], oldMaxs[3];
        memcpy(oldMins, brush + rt::off_Brush_PaddedMins, sizeof oldMins);
        memcpy(oldMaxs, brush + rt::off_Brush_PaddedMaxs, sizeof oldMaxs);
        memcpy(brush + rt::off_Brush_PaddedMins, bounds->mins, sizeof bounds->mins);
        memcpy(brush + rt::off_Brush_PaddedMaxs, bounds->maxs, sizeof bounds->maxs);
        const char result = o_AlternateBrushCull(culler, brushInst);
        memcpy(brush + rt::off_Brush_PaddedMins, oldMins, sizeof oldMins);
        memcpy(brush + rt::off_Brush_PaddedMaxs, oldMaxs, sizeof oldMaxs);
        return result;
    }

    struct HostPerfState
    {
        bool active = false;
        uint64_t frequency = 0;
        uint64_t startTick = 0;
        uint64_t endTick = 0;
        ModToolsPerfCounter metrics[MODTOOLS_HOST_PERF_METRIC_COUNT] = {};
        uint64_t xyFrames[3] = {};
    } g_perf;

    uint64_t PerfNow()
    {
        LARGE_INTEGER value;
        QueryPerformanceCounter(&value);
        return static_cast<uint64_t>(value.QuadPart);
    }

    uint64_t PerfBegin() { return g_perf.active ? PerfNow() : 0; }

    void PerfEnd(int metric, uint64_t begin)
    {
        if (!begin || !g_perf.active) return;
        uint64_t elapsed = PerfNow() - begin;
        ModToolsPerfCounter& counter = g_perf.metrics[metric];
        if (!counter.calls || elapsed < counter.minTicks) counter.minTicks = elapsed;
        if (elapsed > counter.maxTicks) counter.maxTicks = elapsed;
        ++counter.calls;
        counter.totalTicks += elapsed;
    }

    void PerfClear(bool start)
    {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        g_perf = {};
        g_perf.frequency = static_cast<uint64_t>(frequency.QuadPart);
        if (start)
        {
            g_perf.startTick = PerfNow();
            g_perf.active = true;
        }
    }

    template <typename T> T* Global(uintptr_t idaAddr) { return reinterpret_cast<T*>(rt::Rva(idaAddr)); }
    template <typename Fn> Fn Func(uintptr_t idaAddr) { return reinterpret_cast<Fn>(rt::Rva(idaAddr)); }

    std::wstring Widen(const char* s)
    {
        if (!s) return L"";
        int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
        std::wstring w(n ? n - 1 : 0, L'\0');
        if (n) MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
        return w;
    }
    std::string Narrow(const std::wstring& w)
    {
        int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n ? n - 1 : 0, '\0');
        if (n) WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
        return s;
    }

    // Map names come as "x.map", "map_source\zm\x.map" or absolute: try cwd, TA_TOOLS_PATH, TA_TOOLS_PATH\map_source
    std::wstring ResolveMapPath(const std::wstring& name)
    {
        auto full = [](const std::wstring& p) { wchar_t buf[MAX_PATH * 2]; DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH * 2, buf, nullptr); return n ? std::wstring(buf) : p; };
        if (!PathIsRelativeW(name.c_str())) return full(name);
        if (PathFileExistsW(name.c_str())) return full(name);
        wchar_t root[MAX_PATH * 2] = {};
        if (GetEnvironmentVariableW(L"TA_TOOLS_PATH", root, MAX_PATH * 2))
        {
            std::wstring a = std::wstring(root) + L"\\" + name;                    if (PathFileExistsW(a.c_str())) return full(a);
            std::wstring b = std::wstring(root) + L"\\map_source\\" + name;        if (PathFileExistsW(b.c_str())) return full(b);
        }
        return full(name);
    }

    BYTE* CamWnd()
    {
        auto mainWin = *Global<BYTE*>(rt::g_pMainWin);
        if (!mainWin) return nullptr;
        auto qe = *reinterpret_cast<BYTE**>(mainWin + rt::off_MainWin_QEChildren);
        if (!qe) return nullptr;
        return *reinterpret_cast<BYTE**>(qe + rt::off_QEChildren_CamWnd);
    }

    int PrefabDepthNow() { return *Global<int>(rt::g_prefabDepth); }

    // origin/angles of each entered misc_prefab (index = level - 1), read at Map_EnterPrefab time:
    // the stack entry's +1752 is the prefab's own object, not that entity
    struct Placement { float origin[3]; float angles[3]; };
    std::vector<Placement> g_placements;
    bool ReadPlacement(BYTE* ent, Placement& out)
    {
        for (int k = 0; k < 3; ++k) { out.origin[k] = 0; out.angles[k] = 0; }
        if (!ent) return false;
        std::string v;
        if (hooks::EntityKeyValue(ent, "origin", v)) sscanf_s(v.c_str(), "%f %f %f", &out.origin[0], &out.origin[1], &out.origin[2]);
        else memcpy(out.origin, ent + rt::off_Entity_Origin, sizeof out.origin);
        if (hooks::EntityKeyValue(ent, "angles", v)) sscanf_s(v.c_str(), "%f %f %f", &out.angles[0], &out.angles[1], &out.angles[2]);
        else memcpy(out.angles, ent + rt::off_Entity_Angles, sizeof out.angles);
        return true;
    }

    void OnMapLoading(const char* filename)
    {
        if (filename && *filename) plugins::OnMapLoading(ResolveMapPath(Widen(filename)));
    }

    void OnMapLoaded(const char* filename)
    {
        std::wstring path = ResolveMapPath(Widen(filename));
        { std::lock_guard<std::mutex> lock(g_mutex); g_currentMap = path; }
        g_placements.clear();
        LOGW(L"hook: map now '%s' (Radiant name '%S', modified=%d, prefabDepth=%d)", path.c_str(),
            Global<char>(rt::g_currentMapName), *Global<int>(rt::g_modified), PrefabDepthNow());
        plugins::OnMapLoaded(path);
    }

    // Inside a prefab the current-map global, entity list and world object are the prefab's
    void NotifyScope()
    {
        std::wstring path = hooks::CurrentScopePath();
        int depth = PrefabDepthNow();
        LOGW(L"hook: editing scope now '%s' (prefabDepth=%d)", path.c_str(), depth);
        plugins::OnScopeChanged(path, depth);
    }

    __int64 __fastcall hk_Map_EnterPrefab(__int64 a1, __int64 a2, __int64 a3, __int64 a4)
    {
        int before = PrefabDepthNow();
        LOG("hook: Map_EnterPrefab(0x%llx, showAll=%d) depth %d", (unsigned long long)a1, int(a2 & 0xff), before);
        // the entered entity: the argument, else the selected prefab's instance
        BYTE* inst = reinterpret_cast<BYTE*>(a1);
        if (!inst) { std::vector<hooks::RadPtr> sel = hooks::SelectedBrushPtrs(); if (!sel.empty()) inst = *reinterpret_cast<BYTE**>(sel[0] + rt::off_BInst_EntityInst); }
        Placement place{}; bool havePlace = inst && ReadPlacement(*reinterpret_cast<BYTE**>(inst + rt::off_EInst_Entity), place);
        plugins::OnBeforeScopeChange();
        __int64 r = o_Map_EnterPrefab(a1, a2, a3, a4);
        int after = PrefabDepthNow();
        if (after != before)
        {
            g_placements.resize(size_t(after > 0 ? after : 0));
            if (after > 0) { g_placements[size_t(after) - 1] = place; LOG("hook: prefab level %d placed at origin (%g %g %g) angles (%g %g %g)%s", after, place.origin[0], place.origin[1], place.origin[2], place.angles[0], place.angles[1], place.angles[2], havePlace ? "" : " (entity unknown: zeros)"); }
            NotifyScope();
        }
        else LOG("hook: Map_EnterPrefab did not enter (depth still %d)", before);
        return r;
    }

    __int64 __fastcall hk_Map_LeavePrefab(__int64 a1, __int64 a2, __int64 a3, __int64 a4)
    {
        int before = PrefabDepthNow();
        LOG("hook: Map_LeavePrefab(%d) depth %d", int(a1 & 0xff), before);
        if (before > 0) plugins::OnBeforeScopeChange();
        __int64 r = o_Map_LeavePrefab(a1, a2, a3, a4);
        if (PrefabDepthNow() != before) { int d = PrefabDepthNow(); g_placements.resize(size_t(d > 0 ? d : 0)); NotifyScope(); }
        return r;
    }

    __int64 __fastcall hk_Map_LoadFile(char* filename)
    {
        LOG("hook: Map_LoadFile('%s')", filename ? filename : "(null)");
        init::BeforeMapLoad();     // a command-line map loads before the main window shows
        OnMapLoading(filename);
        __int64 r = o_Map_LoadFile(filename);
        OnMapLoaded(filename);
        // saved geometry is applied by now
        if (Cfg().maximize && !g_maximizedOnce) { g_maximizedOnce = true; hooks::MaximizeMainWindow(); }
        if (Cfg().debugCamEnabled)
        {
            if (BYTE* cam = CamWnd())
            {
                float* o = reinterpret_cast<float*>(cam + rt::off_CamWnd_Origin); float* a = reinterpret_cast<float*>(cam + rt::off_CamWnd_Angles);
                o[0] = Cfg().debugCam[0]; o[1] = Cfg().debugCam[1]; o[2] = Cfg().debugCam[2]; a[0] = Cfg().debugCam[3]; a[1] = Cfg().debugCam[4]; a[2] = 0;
                LOG("hook: debug camera placed at (%.0f %.0f %.0f) pitch %.0f yaw %.0f", o[0], o[1], o[2], a[0], a[1]);
            }
        }
        return r;
    }

    char __fastcall hk_Map_SaveFile(char* filename, char silentAutosave, char saveSelected)
    {
        LOG("hook: Map_SaveFile('%s', autosave=%d, selected=%d)", filename ? filename : "(null)", silentAutosave, saveSelected);
        hooks::BeginOwnWrite();
        char r = o_Map_SaveFile(filename, silentAutosave, saveSelected);
        hooks::EndOwnWrite();
        // Save As changes the current-map global (not for autosaves): trust it
        std::wstring saved = hooks::CurrentScopePath();
        if (!silentAutosave && !saveSelected)
        {
            if (PrefabDepthNow() > 0) LOG("hook: save inside a prefab (depth %d); root map unchanged", PrefabDepthNow());
            else
            {
                std::wstring path = ResolveMapPath(Widen(Global<char>(rt::g_currentMapName)));
                std::lock_guard<std::mutex> lock(g_mutex); g_currentMap = path;
            }
        }
        plugins::OnMapSaved(saved, silentAutosave != 0, saveSelected != 0);
        return r;
    }

    // Main-window idle tick (main thread): drives mt jobs, the plugin loader, hotkeys and OnTick
    __int64 __fastcall hk_RoutineProcessing(void* mainWin)
    {
        uint64_t routineBegin = PerfBegin();
        mt::DrainNow();
        ui::Tick();
        pluginmgr::Tick();
        uint64_t pluginsBegin = PerfBegin();
        plugins::OnTick();
        dock::RememberVisibility();
        PerfEnd(MODTOOLS_HOST_PERF_PLUGINS_ON_TICK, pluginsBegin);
        uint64_t radiantBegin = PerfBegin();
        __int64 result = o_RoutineProcessing(mainWin);
        PerfEnd(MODTOOLS_HOST_PERF_RADIANT_ROUTINE_PROCESSING, radiantBegin);
        PerfEnd(MODTOOLS_HOST_PERF_ROUTINE_HOOK, routineBegin);
        return result;
    }

    __int64 __fastcall hk_Cam_Draw(BYTE* camWnd, __int64 a2, __int64 a3, __int64 a4)
    {
        render::CamParams p{};
        memcpy(p.origin, camWnd + rt::off_CamWnd_Origin, sizeof p.origin);
        memcpy(p.angles, camWnd + rt::off_CamWnd_Angles, sizeof p.angles);
        p.width = *reinterpret_cast<int*>(camWnd + rt::off_CamWnd_Width);
        p.height = *reinterpret_cast<int*>(camWnd + rt::off_CamWnd_Height);
        p.gameView = *Global<char>(rt::g_gameView) != 0 || *Global<char>(rt::g_probeView) != 0;
        p.fovDeg = Func<float(__fastcall*)()>(rt::Cam_GetFov)();   // what this draw projects with
        render::BeginCamView(p);
        uint64_t begin = PerfBegin();
        __int64 r = o_Cam_Draw(camWnd, a2, a3, a4);
        PerfEnd(MODTOOLS_HOST_PERF_CAM_DRAW, begin);
        render::EndView();
        return r;
    }

    void __fastcall hk_ModelDispatcher(unsigned int a1, BYTE* brushInst, __int64 a3, __int64 a4, __int64 a5, char a6)
    {
        if (!o_ModelDispatcher) return;
        NativeModelCullBounds* bounds = FindNativeModelCullBounds(brushInst);
        if (!bounds)
        {
            o_ModelDispatcher(a1, brushInst, a3, a4, a5, a6);
            return;
        }
        bounds->brushInst = brushInst;
        float oldMins[3], oldMaxs[3];
        memcpy(oldMins, brushInst + rt::off_BInst_CullMins, sizeof oldMins);
        memcpy(oldMaxs, brushInst + rt::off_BInst_CullMaxs, sizeof oldMaxs);
        memcpy(brushInst + rt::off_BInst_CullMins, bounds->mins, sizeof bounds->mins);
        memcpy(brushInst + rt::off_BInst_CullMaxs, bounds->maxs, sizeof bounds->maxs);
        o_ModelDispatcher(a1, brushInst, a3, a4, a5, a6);
        memcpy(brushInst + rt::off_BInst_CullMins, oldMins, sizeof oldMins);
        memcpy(brushInst + rt::off_BInst_CullMaxs, oldMaxs, sizeof oldMaxs);
    }

    // Radiant's cam HUD text: its pixel ortho is still current and Present has not happened, so labels draw here (render::CamHudPass)
    __int64 __fastcall hk_CamDrawHud(BYTE* camWnd, __int64 a2, __int64 a3, __int64 a4)
    {
        uint64_t begin = PerfBegin();
        __int64 r = o_CamDrawHud(camWnd, a2, a3, a4);
        PerfEnd(MODTOOLS_HOST_PERF_DRAW_HUD, begin);
        render::CamHudPass(*reinterpret_cast<int*>(camWnd + rt::off_CamWnd_HudW), *reinterpret_cast<int*>(camWnd + rt::off_CamWnd_HudH), int(a3), int(a4));
        return r;
    }

    __int64 __fastcall hk_XY_Draw(BYTE* xyWnd, __int64* a2, __int64 a3, __int64 a4)
    {
        // camera movement queues hidden 2D panes too: skip their map traversal (Qt repaints when shown)
        void* widget = *reinterpret_cast<void**>(xyWnd + rt::off_XYWnd_Widget);
        if (widget && qt::Bind() && !qt::IsVisible(widget))
            return 0;

        render::XYParams p{};
        p.type = *reinterpret_cast<int*>(xyWnd + rt::off_XYWnd_ViewType);
        memcpy(p.origin, xyWnd + rt::off_XYWnd_Origin, sizeof p.origin);
        p.scale = *reinterpret_cast<float*>(xyWnd + rt::off_XYWnd_Scale);
        render::BeginXYView(p);
        uint64_t begin = PerfBegin();
        __int64 r = o_XY_Draw(xyWnd, a2, a3, a4);
        PerfEnd(MODTOOLS_HOST_PERF_XY_DRAW, begin);
        if (g_perf.active)
        {
            int index = p.type == 2 ? 0 : p.type == 1 ? 1 : p.type == 0 ? 2 : -1;
            if (index >= 0) ++g_perf.xyFrames[index];
        }
        render::EndView();
        return r;
    }

    struct CameraState { float origin[3]; float angles[3]; bool valid = false; };

    CameraState SaveCamera()
    {
        CameraState s;
        s.valid = hooks::GetCamera(s.origin, s.angles);
        return s;
    }

    void RestoreCamera(const CameraState& s)
    {
        if (!s.valid) return;
        if (BYTE* cam = CamWnd())
        {
            memcpy(cam + rt::off_CamWnd_Origin, s.origin, sizeof s.origin);
            memcpy(cam + rt::off_CamWnd_Angles, s.angles, sizeof s.angles);
        }
    }

    // Map_LoadFile/Map_New ask "Lighting changed. Do you want to export?" every load: answer No during our own reloads
    __int64 __fastcall hk_QuestionBox(void* parent, const char* text, const char* title, int buttons, int defaultButton)
    {
        if (g_reloading && text && (text == reinterpret_cast<const char*>(rt::Rva(rt::str_LightingChanged)) || strcmp(text, "Lighting changed. Do you want to export?") == 0))
        {
            LOG("reload: Radiant asked '%s' during our reload; answered No", text);
            return 0x10000;
        }
        return o_QuestionBox(parent, text, title, buttons, defaultButton);
    }

    template <typename Fn>
    bool Hook(const char* name, uintptr_t idaAddr, Fn detour, Fn* original)
    {
        if (!idaAddr) { LOG("hook: %s skipped (address not located yet)", name); return false; }
        void* target = reinterpret_cast<void*>(rt::Rva(idaAddr));
        MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
        if (st != MH_OK) { LOG("hook: %s MH_CreateHook failed: %s", name, MH_StatusToString(st)); return false; }
        st = MH_EnableHook(target);
        if (st != MH_OK) { LOG("hook: %s MH_EnableHook failed: %s", name, MH_StatusToString(st)); return false; }
        LOG("hook: %s installed at 0x%p", name, target);
        return true;
    }
}

namespace hooks
{
    int PerformanceProfile(int action, ModToolsHostPerfSnapshot* out)
    {
        switch (action)
        {
        case MODTOOLS_HOST_PERF_RESET:
            PerfClear(false);
            break;
        case MODTOOLS_HOST_PERF_START:
            PerfClear(true);
            break;
        case MODTOOLS_HOST_PERF_STOP:
            if (g_perf.active)
            {
                g_perf.endTick = PerfNow();
                g_perf.active = false;
            }
            break;
        case MODTOOLS_HOST_PERF_SNAPSHOT:
            break;
        default:
            return 0;
        }

        if (out)
        {
            memset(out, 0, sizeof(*out));
            out->size = sizeof(*out);
            out->active = g_perf.active ? 1u : 0u;
            out->qpcFrequency = g_perf.frequency;
            out->startTick = g_perf.startTick;
            out->endTick = g_perf.active ? PerfNow() : g_perf.endTick;
            memcpy(out->metrics, g_perf.metrics, sizeof(out->metrics));
            memcpy(out->xyFrames, g_perf.xyFrames, sizeof(out->xyFrames));
        }
        return 1;
    }

    bool Install()
    {
        MH_STATUS st = MH_Initialize();
        if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) { LOG("hook: MH_Initialize failed: %s", MH_StatusToString(st)); return false; }
        if (!rt::VerifyBinary()) { LOG("hook: not the known Radiant build (or not Radiant at all); Radiant hooks disabled"); return false; }

        Hook("Map_LoadFile", rt::Map_LoadFile, &hk_Map_LoadFile, &o_Map_LoadFile);
        Hook("Map_SaveFile", rt::Map_SaveFile, &hk_Map_SaveFile, &o_Map_SaveFile);
        Hook("RadiantMainWin::RoutineProcessing", rt::RadiantMainWin_RoutineProcessing, &hk_RoutineProcessing, &o_RoutineProcessing);
        Hook("CCamWnd::Cam_Draw", rt::CCamWnd_Cam_Draw, &hk_Cam_Draw, &o_Cam_Draw);
        Hook("CXYWnd::XY_Draw", rt::CXYWnd_XY_Draw, &hk_XY_Draw, &o_XY_Draw);
        render::SetEngineTextAvailable(Hook("CCamWnd::DrawHud", rt::CCamWnd_DrawHud, &hk_CamDrawHud, &o_CamDrawHud));
        Hook("Map_EnterPrefab", rt::Map_EnterPrefab, &hk_Map_EnterPrefab, &o_Map_EnterPrefab);
        Hook("Map_LeavePrefab", rt::Map_LeavePrefab, &hk_Map_LeavePrefab, &o_Map_LeavePrefab);
        Hook("QuestionBox", rt::QuestionBox, &hk_QuestionBox, &o_QuestionBox);
        Hook("BrushCache::GenericCull", rt::BrushCache_GenericCull, &hk_GenericBrushCull, &o_GenericBrushCull);
        Hook("BrushCache::AlternateCull", rt::BrushCache_AlternateCull, &hk_AlternateBrushCull, &o_AlternateBrushCull);
        Hook("BrushCache::ModelDispatcher", rt::BrushCache_ModelDispatcher, &hk_ModelDispatcher, &o_ModelDispatcher);

        // injected after a map was opened: pick it up from the global
        const char* cur = Global<char>(rt::g_currentMapName);
        if (cur && *cur && strcmp(cur, "unnamed.map") != 0) OnMapLoaded(cur);
        return true;
    }

    void Remove()
    {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
    }

    void SetNativeModelCullBounds(const ModToolsNativeModelCullBounds* bounds, size_t count)
    {
        if (!bounds || !count)
        {
            g_nativeModelCullBoundsCount = 0;
            return;
        }
        const size_t capped = std::min(count, kNativeModelCullBoundsLimit);
        const std::array<NativeModelCullBounds, kNativeModelCullBoundsLimit> previous = g_nativeModelCullBounds;
        const size_t previousCount = g_nativeModelCullBoundsCount;
        g_nativeModelCullBoundsCount = 0;
        for (size_t i = 0; i < capped; ++i)
        {
            const ModToolsNativeModelCullBounds& source = bounds[i];
            if (!source.brush || !std::isfinite(source.mins[0]) || !std::isfinite(source.mins[1]) || !std::isfinite(source.mins[2]) ||
                !std::isfinite(source.maxs[0]) || !std::isfinite(source.maxs[1]) || !std::isfinite(source.maxs[2]) ||
                source.maxs[0] <= source.mins[0] || source.maxs[1] <= source.mins[1] || source.maxs[2] <= source.mins[2]) continue;
            NativeModelCullBounds& target = g_nativeModelCullBounds[g_nativeModelCullBoundsCount++];
            target = {};
            target.brush = static_cast<BYTE*>(source.brush);
            target.entity = target.brush ? *reinterpret_cast<BYTE**>(target.brush + rt::off_Brush_Owner) : nullptr;
            for (size_t prior = 0; prior < previousCount; ++prior)
            {
                if (previous[prior].brush != target.brush) continue;
                target.brushInst = previous[prior].brushInst;
                break;
            }
            memcpy(target.mins, source.mins, sizeof target.mins);
            memcpy(target.maxs, source.maxs, sizeof target.maxs);
        }
    }

    std::wstring CurrentMapPath() { std::lock_guard<std::mutex> lock(g_mutex); return g_currentMap; }

    std::wstring CurrentScopePath()
    {
        if (!rt::VerifyBinary()) return L"";
        std::wstring path = ResolveMapPath(Widen(Global<char>(rt::g_currentMapName)));
        for (auto& c : path) if (c == L'/') c = L'\\';
        return path;
    }

    int Modified() { return rt::VerifyBinary() ? *Global<int>(rt::g_modified) : 0; }

    void BeginOwnWrite() { ++g_ownWriteDepth; }
    void EndOwnWrite() { g_ownWriteEndTick = GetTickCount64(); --g_ownWriteDepth; }
    bool OwnWriteRecent() { return g_ownWriteDepth.load() > 0 || (GetTickCount64() - g_ownWriteEndTick) < 1500; }

    bool GetCamera(float origin[3], float angles[3])
    {
        BYTE* cam = rt::VerifyBinary() ? CamWnd() : nullptr;
        if (!cam) return false;
        memcpy(origin, cam + rt::off_CamWnd_Origin, 3 * sizeof(float));
        memcpy(angles, cam + rt::off_CamWnd_Angles, 3 * sizeof(float));
        return true;
    }

    bool SetCamera(const float origin[3], const float angles[3])
    {
        BYTE* cam = rt::VerifyBinary() ? CamWnd() : nullptr;
        if (!cam) return false;
        memcpy(cam + rt::off_CamWnd_Origin, origin, 3 * sizeof(float));
        memcpy(cam + rt::off_CamWnd_Angles, angles, 3 * sizeof(float));
        return true;
    }

    bool GameView() { return rt::VerifyBinary() && *Global<char>(rt::g_gameView) != 0; }
    void SetGameView(bool on) { if (rt::VerifyBinary()) *Global<char>(rt::g_gameView) = on ? 1 : 0; }

    // Set bits in Radiant's pending-redraw global before the idle tick clears it, so a plugin's OnTick bit counts this tick
    void RequestRedraw(unsigned bits) { if (bits && rt::VerifyBinary()) *Global<unsigned>(rt::g_redrawFlags) |= bits; }
    bool CamXYUpdateOnDrag() { return rt::VerifyBinary() && *Global<int>(rt::g_camXYUpdate) != 0; }

    static std::string GuidText(const BYTE* guid16)
    {
        wchar_t w[64] = {};
        if (StringFromGUID2(*reinterpret_cast<const GUID*>(guid16), w, 64) <= 0) return "";
        char a[64] = {};
        WideCharToMultiByte(CP_ACP, 0, w, -1, a, 64, nullptr, nullptr);
        return a;
    }

    bool GetSelection(std::vector<SelectedBrush>& out, size_t maxItems)
    {
        out.clear();
        if (!rt::VerifyBinary()) return false;
        BYTE* collection = Global<BYTE>(rt::g_selectedBrushes);
        BYTE* sentinel = *reinterpret_cast<BYTE**>(collection + rt::off_Collection_List);
        if (!sentinel) return false;
        BYTE* node = *reinterpret_cast<BYTE**>(sentinel + rt::off_BInst_Next);
        size_t n = 0;
        while (node && node != sentinel && n++ < maxItems)
        {
            BYTE* brush = *reinterpret_cast<BYTE**>(node + rt::off_BInst_Brush);
            if (brush)
            {
                SelectedBrush sb;
                sb.brushGuid = GuidText(brush + rt::off_Brush_Guid);
                BYTE* owner = *reinterpret_cast<BYTE**>(brush + rt::off_Brush_Owner);
                sb.entityGuid = owner ? GuidText(owner + rt::off_Entity_Guid) : "";
                memcpy(sb.mins, brush + rt::off_Brush_Mins, sizeof sb.mins);
                memcpy(sb.maxs, brush + rt::off_Brush_Maxs, sizeof sb.maxs);
                out.push_back(std::move(sb));
            }
            node = *reinterpret_cast<BYTE**>(node + rt::off_BInst_Next);
        }
        return true;
    }

    void MarkMapModified()
    {
        if (!rt::VerifyBinary()) return;
        int* flag = Global<int>(rt::g_modified);
        if (flag && *flag == 0) *flag = 1;
    }

    bool MoveBrush(RadPtr brush, const float delta[3])
    {
        if (!brush || !delta || !rt::VerifyBinary()) return false;
        if (delta[0] == 0.0f && delta[1] == 0.0f && delta[2] == 0.0f) return true;
        using BrushMove_t = __int64(__fastcall*)(void* brush, const float* delta, unsigned char snap, unsigned char markModified);
        Func<BrushMove_t>(rt::Brush_Move)(brush, delta, 0, 0);
        return true;
    }

    static bool ParseGuidText(const std::string& text, GUID& out)
    {
        wchar_t w[64] = {};
        if (text.size() < 36 || text.size() > 40) return false;
        for (size_t i = 0; i < text.size(); ++i) w[i] = wchar_t(text[i]);
        if (text[0] != '{') { std::wstring b = L"{" + std::wstring(w) + L"}"; return CLSIDFromString(b.c_str(), &out) == S_OK; }
        return CLSIDFromString(w, &out) == S_OK;
    }

    size_t FindBrushesByGuid(const std::vector<std::string>& guids, std::vector<RadPtr>& out)
    {
        out.assign(guids.size(), nullptr);
        if (guids.empty() || !rt::VerifyBinary()) return 0;
        struct Key { GUID g; size_t index; };
        std::vector<Key> keys;
        for (size_t i = 0; i < guids.size(); ++i) { GUID g; if (ParseGuidText(guids[i], g)) keys.push_back({ g, i }); }
        if (keys.empty()) return 0;
        size_t found = 0, remaining = keys.size();
        ForEachEntity([&](RadPtr ent)
        {
            if (!remaining) return;
            ForEachEntityBrush(ent, [&](RadPtr b)
            {
                if (!remaining) return;
                const GUID* bg = reinterpret_cast<const GUID*>(b + rt::off_Brush_Guid);
                for (Key& k : keys)
                {
                    if (k.index == size_t(-1) || memcmp(&k.g, bg, sizeof(GUID)) != 0) continue;
                    out[k.index] = b; k.index = size_t(-1); ++found; --remaining;
                    break;
                }
            });
        });
        return found;
    }

    size_t FindEntitiesByGuid(const std::vector<std::string>& guids, std::vector<RadPtr>& out)
    {
        out.assign(guids.size(), nullptr);
        if (guids.empty() || !rt::VerifyBinary()) return 0;
        struct Key { GUID g; size_t index; };
        std::vector<Key> keys;
        for (size_t i = 0; i < guids.size(); ++i) { GUID g; if (ParseGuidText(guids[i], g)) keys.push_back({ g, i }); }
        if (keys.empty()) return 0;
        size_t found = 0, remaining = keys.size();
        ForEachEntity([&](RadPtr ent)
        {
            if (!remaining) return;
            const GUID* eg = reinterpret_cast<const GUID*>(ent + rt::off_Entity_Guid);
            for (Key& k : keys)
            {
                if (k.index == size_t(-1) || memcmp(&k.g, eg, sizeof(GUID)) != 0) continue;
                out[k.index] = ent; k.index = size_t(-1); ++found; --remaining;
                break;
            }
        });
        return found;
    }

    static size_t FaceWalk(RadPtr b, const std::function<void(BYTE* face, size_t index)>& fn)
    {
        int type = *reinterpret_cast<int*>(b + rt::off_Brush_Type);
        if (type == 1 || type == 2) return 0;   // physics box / cylinder: saved as parameters, rebuilt on load (physprim.h)
        BYTE* begin = *reinterpret_cast<BYTE**>(b + rt::off_Brush_FaceBegin);
        BYTE* end = *reinterpret_cast<BYTE**>(b + rt::off_Brush_FaceEnd);
        if (!begin || !end || end < begin) return 0;
        size_t n = size_t(end - begin) / rt::sz_Face;
        if (n > 512) return 0;
        size_t count = 0;
        for (size_t i = 0; i < n; ++i)
        {
            BYTE* face = begin + i * rt::sz_Face;
            if (!*reinterpret_cast<void**>(face + rt::off_Face_Winding)) continue;
            fn(face, count++);
        }
        return count;
    }

    size_t BrushPlanes(RadPtr brush, float* out, size_t capFaces)
    {
        if (!brush || !rt::VerifyBinary()) return 0;
        return FaceWalk(brush, [&](BYTE* face, size_t i) { if (out && i < capFaces) memcpy(out + i * 9, face + rt::off_Face_PlanePts, sizeof(float) * 9); });
    }

    bool SetBrushPlanes(RadPtr brush, const float* pts, size_t nFaces)
    {
        if (!brush || !pts || !nFaces || !rt::VerifyBinary()) return false;
        if (FaceWalk(brush, [](BYTE*, size_t) {}) != nFaces) return false;
        FaceWalk(brush, [&](BYTE* face, size_t i) { memcpy(face + rt::off_Face_PlanePts, pts + i * 9, sizeof(float) * 9); });
        using BrushBuild_t = void(__fastcall*)(void* brush, unsigned char snap, unsigned char markModified, unsigned flag);
        Func<BrushBuild_t>(rt::Brush_Build)(brush, 0, 0, 0);
        return true;
    }

    bool EntityFixedSize(RadPtr ent)
    {
        if (!ent || !rt::VerifyBinary()) return false;
        BYTE* eclass = *reinterpret_cast<BYTE**>(ent + rt::off_Entity_EClass);
        return eclass && eclass[72] != 0;
    }

    RadPtr BrushOfInst(RadPtr inst) { return inst ? *reinterpret_cast<BYTE**>(inst + rt::off_BInst_Brush) : nullptr; }

    bool WriteEntityTextWith(RadPtr ent, const std::vector<RadPtr>& extraBrushes, std::string& out)
    {
        if (!ent || !rt::VerifyBinary()) return false;
        // brushes already linked need nothing
        std::set<BYTE*> linked;
        ForEachEntityBrush(ent, [&](RadPtr b) { linked.insert(b); });
        struct Saved { BYTE* brush; BYTE* prev; BYTE* next; };
        std::vector<Saved> saved;
        BYTE* entPrev = *reinterpret_cast<BYTE**>(ent + rt::off_Entity_BrushPrev);
        BYTE* entNext = *reinterpret_cast<BYTE**>(ent + rt::off_Entity_BrushNext);
        std::vector<std::pair<BYTE**, BYTE*>> restore;   // every word written with its old value, restored in reverse
        for (RadPtr b : extraBrushes)
        {
            if (!b || linked.count(b)) continue;
            if (*reinterpret_cast<BYTE**>(b + rt::off_Brush_Owner) != ent) continue;
            // splice at the tail: tail <-> b <-> ent
            BYTE** tailNext = reinterpret_cast<BYTE**>(*reinterpret_cast<BYTE**>(ent + rt::off_Entity_BrushPrev) + rt::off_Brush_Next);
            BYTE** entPrevW = reinterpret_cast<BYTE**>(ent + rt::off_Entity_BrushPrev);
            BYTE** bPrev = reinterpret_cast<BYTE**>(b + rt::off_Brush_Prev);
            BYTE** bNext = reinterpret_cast<BYTE**>(b + rt::off_Brush_Next);
            restore.push_back({ bPrev, *bPrev }); restore.push_back({ bNext, *bNext });
            restore.push_back({ tailNext, *tailNext }); restore.push_back({ entPrevW, *entPrevW });
            *bPrev = *entPrevW; *bNext = ent; *tailNext = b; *entPrevW = b;
            linked.insert(b);
        }
        bool ok = WriteEntityText(ent, out);
        for (size_t i = restore.size(); i-- > 0;) *restore[i].first = restore[i].second;
        (void)entPrev; (void)entNext; (void)saved;
        return ok;
    }

    bool BrushBounds(RadPtr brush, float mins[3], float maxs[3])
    {
        if (!brush || !rt::VerifyBinary()) return false;
        if (mins) memcpy(mins, brush + rt::off_Brush_Mins, sizeof(float) * 3);
        if (maxs) memcpy(maxs, brush + rt::off_Brush_Maxs, sizeof(float) * 3);
        return true;
    }

    void MaximizeMainWindow()
    {
        mt::Post([]
        {
            HWND h = mt::MainWindow();
            if (!h) return;
            // primary monitor first so the maximize lands there
            SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            ShowWindow(h, SW_MAXIMIZE);
            SetForegroundWindow(h);
            RECT r{}; GetWindowRect(h, &r);
            LOG("window: maximized 0x%p -> %ld,%ld %ldx%ld", h, r.left, r.top, r.right - r.left, r.bottom - r.top);
        });
    }

    bool ReloadMapNow(const std::wstring& fullPath, bool keepCamera)
    {
        if (g_reloading || !o_Map_LoadFile) return false;
        if (PrefabDepthNow() > 0) { LOG("reload: refused inside a prefab (depth %d)", PrefabDepthNow()); return false; }
        // a reload under a held button swallows the release and the freelook cursor stays hidden: callers wait and retry
        if (MouseButtonHeld()) { LOG("reload: deferred, a mouse button is held"); return false; }
        g_reloading = true;
        std::string narrow = Narrow(fullPath);

        // persist camera bookmarks like Radiant does before a reload
        char* radiantName = Global<char>(rt::g_currentMapName);
        auto bookmarks = Global<void>(rt::g_bookmarks);
        if (Func<Bookmarks_Count_t>(rt::Bookmarks_Count)(bookmarks) && strcmp(radiantName, "unnamed.map") != 0)
            Func<Bookmarks_Save_t>(rt::Bookmarks_Save)(bookmarks, radiantName);

        CameraState cam = SaveCamera();
        LOG("reload: Map_LoadFile('%s') keepCamera=%d cam=(%.1f %.1f %.1f)", narrow.c_str(), keepCamera, cam.origin[0], cam.origin[1], cam.origin[2]);
        OnMapLoading(narrow.c_str());
        o_Map_LoadFile(&narrow[0]);
        if (keepCamera) RestoreCamera(cam);
        g_reloading = false;
        CURSORINFO ci{ sizeof ci };
        if (GetCursorInfo(&ci) && !(ci.flags & CURSOR_SHOWING))
        {
            int n = 0; while (ShowCursor(TRUE) < 0 && ++n < 16) {}
            LOG("reload: the cursor was hidden, shown again (%d step%s)", n, n == 1 ? "" : "s");
        }
        OnMapLoaded(narrow.c_str());
        return true;
    }

    bool IsReloading() { return g_reloading; }
    bool MouseButtonHeld() { return ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000) != 0; }

    void ForEachEntity(const std::function<void(RadPtr)>& fn)
    {
        if (!rt::VerifyBinary()) return;
        BYTE* list = Global<BYTE>(rt::g_entityList);
        BYTE* e = *reinterpret_cast<BYTE**>(list + rt::off_Entity_ListNext);
        size_t guard = 0;
        while (e && e != list && guard++ < 200000)
        {
            fn(e);
            e = *reinterpret_cast<BYTE**>(e + rt::off_Entity_ListNext);
        }
    }

    void ForEachEntityBrush(RadPtr ent, const std::function<void(RadPtr)>& fn)
    {
        if (!ent) return;
        BYTE* b = *reinterpret_cast<BYTE**>(ent + rt::off_Entity_BrushNext);
        size_t guard = 0;
        while (b && b != ent && guard++ < 1000000)
        {
            fn(b);
            b = *reinterpret_cast<BYTE**>(b + rt::off_Brush_Next);
        }
    }

    std::vector<RadPtr> SelectedBrushPtrs()
    {
        std::vector<RadPtr> out;
        if (!rt::VerifyBinary()) return out;
        BYTE* collection = Global<BYTE>(rt::g_selectedBrushes);
        BYTE* sentinel = *reinterpret_cast<BYTE**>(collection + rt::off_Collection_List);
        if (!sentinel) return out;
        BYTE* node = *reinterpret_cast<BYTE**>(sentinel + rt::off_BInst_Next);
        size_t guard = 0;
        while (node && node != sentinel && guard++ < 100000) { out.push_back(node); node = *reinterpret_cast<BYTE**>(node + rt::off_BInst_Next); }
        return out;
    }

    RadPtr FindBrushInst(RadPtr brush)
    {
        if (!brush) return nullptr;
        BYTE* owner = *reinterpret_cast<BYTE**>(brush + rt::off_Brush_Owner);
        using ListFn = BYTE** (__fastcall*)(void* world, BYTE** out);
        void* world = Global<void>(rt::g_world);
        BYTE* first = nullptr; BYTE* endTmp = nullptr;
        Func<ListFn>(rt::EntityInst_Begin)(world, &first);
        BYTE** endPtr = Func<ListFn>(rt::EntityInst_End)(world, &endTmp);
        BYTE* end = endPtr ? *endPtr : nullptr;
        size_t guard = 0;
        for (BYTE* inst = first; inst && inst != end && guard++ < 200000; inst = *reinterpret_cast<BYTE**>(inst + rt::off_EInst_Next))
        {
            if (*reinterpret_cast<BYTE**>(inst + rt::off_EInst_Entity) != owner) continue;
            BYTE* sentinel = inst + rt::off_EInst_ListSentinel;
            BYTE* n = *reinterpret_cast<BYTE**>(sentinel + rt::off_BInst_ListNext);
            size_t g2 = 0;
            while (n && n != sentinel && g2++ < 100000)
            {
                if (*reinterpret_cast<BYTE**>(n + rt::off_BInst_Brush) == brush) return n;
                n = *reinterpret_cast<BYTE**>(n + rt::off_BInst_ListNext);
            }
            return nullptr;
        }
        return nullptr;
    }

    static bool GuidFromText(const std::string& text, BYTE* out16)
    {
        wchar_t w[64] = {};
        MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, w, 64);
        GUID g;
        if (FAILED(CLSIDFromString(w, &g))) return false;
        memcpy(out16, &g, 16);
        return true;
    }

    using InstFn_t = void(__fastcall*)(RadPtr entityInst);

    // Live EntityInsts of `ent` in Radiant's GUID registry (values only compared, never dereferenced)
    static std::vector<RadPtr> RegisteredInstsOf(RadPtr ent)
    {
        std::vector<RadPtr> out;
        BYTE* head = *Global<BYTE*>(rt::g_entityRegistry);
        if (!head || !ent) return out;
        std::vector<RadPtr> live;
        using ListFn = BYTE** (__fastcall*)(void* world, BYTE** out);
        void* world = Global<void>(rt::g_world);
        BYTE* first = nullptr; BYTE* endTmp = nullptr;
        Func<ListFn>(rt::EntityInst_Begin)(world, &first);
        BYTE** endPtr = Func<ListFn>(rt::EntityInst_End)(world, &endTmp);
        BYTE* end = endPtr ? *endPtr : nullptr;
        size_t guard = 0;
        for (BYTE* inst = first; inst && inst != end && guard++ < 200000; inst = *reinterpret_cast<BYTE**>(inst + rt::off_EInst_Next))
            if (*reinterpret_cast<BYTE**>(inst + rt::off_EInst_Entity) == ent) live.push_back(inst);
        if (live.empty()) return out;
        std::vector<BYTE*> stack;
        stack.push_back(*reinterpret_cast<BYTE**>(head + 8));          // root = head->parent (the head itself when empty)
        guard = 0;
        while (!stack.empty() && guard++ < 1000000)
        {
            BYTE* node = stack.back(); stack.pop_back();
            if (!node || node[25]) continue;                            // isnil: the head
            RadPtr inst = *reinterpret_cast<BYTE**>(node + 48);
            for (RadPtr l : live) if (l == inst) { out.push_back(inst); break; }
            stack.push_back(*reinterpret_cast<BYTE**>(node + 0));
            stack.push_back(*reinterpret_cast<BYTE**>(node + 16));
        }
        return out;
    }

    namespace
    {
        // Writing a GUID over a registered entity strands its registry entry (freed EntityInst later): like Radiant's
        // duplicate-GUID fixer, take the instances out, change the GUID, put them back
        // Brushes: the selected ones this entity owns (the import selected them), else all of its brushes.
        void RestampEntity(RadPtr ent, const std::vector<RadPtr>& sel, const std::string& entityGuid, const std::string* brushGuids, size_t nGuids, bool verbose)
        {
            std::string before = verbose ? EntityGuid(ent) : std::string();
            std::vector<RadPtr> registered = RegisteredInstsOf(ent);
            for (RadPtr inst : registered) Func<InstFn_t>(rt::EntityRegistry_Remove)(inst);
            GuidFromText(entityGuid, ent + rt::off_Entity_Guid);
            for (RadPtr inst : registered) Func<InstFn_t>(rt::EntityRegistry_Add)(inst);
            std::vector<RadPtr> brushes;
            for (RadPtr b : sel) if (BrushOwner(b) == ent) brushes.push_back(b);
            if (brushes.empty()) ForEachEntityBrush(ent, [&](RadPtr b) { brushes.push_back(b); });
            size_t n = brushes.size() < nGuids ? brushes.size() : nGuids;
            for (size_t i = 0; i < n; ++i) GuidFromText(brushGuids[i], brushes[i] + rt::off_Brush_Guid);
            if (verbose)
                LOG("hook: restamp entity %s -> %s, %zu/%zu brush guids, %zu registry entr%s moved", before.c_str(), entityGuid.c_str(), n, brushes.size(),
                    registered.size(), registered.size() == 1 ? "y" : "ies");
        }

        std::vector<RadPtr> SelectedBrushes()
        {
            std::vector<RadPtr> sel;
            for (RadPtr bi : SelectedBrushPtrs()) { RadPtr b = *reinterpret_cast<BYTE**>(bi + rt::off_BInst_Brush); if (b) sel.push_back(b); }
            return sel;
        }
    }

    bool RestampImportedGuids(const std::string& entityGuid, const std::vector<std::string>& brushGuids)
    {
        std::vector<RadPtr> sel = SelectedBrushes();
        if (entityGuid.empty())
        {
            if (sel.size() != brushGuids.size()) { LOG("hook: restamp: %zu selected brushes but %zu guids", sel.size(), brushGuids.size()); return false; }
            for (size_t i = 0; i < sel.size(); ++i) GuidFromText(brushGuids[i], sel[i] + rt::off_Brush_Guid);
            return true;
        }
        // the imported entity is the newest one in the global list
        BYTE* list = Global<BYTE>(rt::g_entityList);
        BYTE* ent = *reinterpret_cast<BYTE**>(list + rt::off_Entity_ListPrev);
        if (!ent || ent == list) return false;
        RestampEntity(ent, sel, entityGuid, brushGuids.data(), brushGuids.size(), true);
        return true;
    }

    size_t RestampImportedEntities(const std::vector<std::string>& entityGuids, const std::vector<std::vector<std::string>>& brushGuids)
    {
        size_t n = entityGuids.size();
        if (n == 0 || brushGuids.size() != n) return 0;
        // one import appends its entities to the global list in text order: the last n entries, oldest first
        BYTE* list = Global<BYTE>(rt::g_entityList);
        std::vector<RadPtr> ents(n, nullptr);
        BYTE* e = list;
        for (size_t i = n; i-- > 0;)
        {
            e = *reinterpret_cast<BYTE**>(e + rt::off_Entity_ListPrev);
            if (!e || e == list) { LOG("hook: restamp: %zu entities asked, fewer in the list", n); return 0; }
            ents[i] = e;
        }
        std::vector<RadPtr> sel = SelectedBrushes();
        for (size_t i = 0; i < n; ++i) RestampEntity(ents[i], sel, entityGuids[i], brushGuids[i].data(), brushGuids[i].size(), false);
        LOG("hook: restamp of %zu imported entities (%zu brushes selected)", n, sel.size());
        return n;
    }

    void SelectBrushes(const std::vector<RadPtr>& brushes)
    {
        void* sel = Global<void>(rt::g_selectedBrushes);
        Func<Collection1_t>(rt::Selection_BeginBatch)(sel);
        for (RadPtr b : brushes) Func<Collection2_t>(rt::Select_Brush)(sel, b);
        Func<EndBatch_t>(rt::Selection_EndBatch)(sel, 3, 0, 1);
    }

    void DeselectAll() { Func<Deselect_t>(rt::Select_Deselect)(1); }

    int PrefabDepth() { return rt::VerifyBinary() ? PrefabDepthNow() : 0; }

    // entity_t+304 std::map<std::string, QVariant> layout: (entity_t). In-order walk from the
    // leftmost node, bounded by the size, SEH catches bad pointers.
    namespace
    {
        bool ReadVariantText(const BYTE* var, std::string& out)
        {
            uint32_t flags = *reinterpret_cast<const uint32_t*>(var + 8);
            uint32_t type = flags & 0x3fffffff; bool shared = (flags & 0x40000000) != 0;
            const BYTE* payload = var;                                   // inline data
            if (shared) { const BYTE* ps = *reinterpret_cast<const BYTE* const*>(var); if (!ps) return false; payload = *reinterpret_cast<const BYTE* const*>(ps); if (!payload) return false; }
            char buf[96];
            switch (type)
            {
            case 10:   // QString
            {
                const BYTE* d = shared ? *reinterpret_cast<const BYTE* const*>(payload) : *reinterpret_cast<const BYTE* const*>(var);
                if (!d) return false;
                int size = *reinterpret_cast<const int*>(d + 4); ptrdiff_t off = *reinterpret_cast<const ptrdiff_t*>(d + 16);
                if (size < 0 || size > 65536 || off < 16 || off > 4096) return false;
                const wchar_t* w = reinterpret_cast<const wchar_t*>(d + off);
                int n = WideCharToMultiByte(CP_UTF8, 0, w, size, nullptr, 0, nullptr, nullptr);
                out.assign(size_t(n > 0 ? n : 0), '\0');
                if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, size, &out[0], n, nullptr, nullptr);
                return true;
            }
            case 83:   // QVector3D
            {
                const float* v = reinterpret_cast<const float*>(payload);
                _snprintf_s(buf, _TRUNCATE, "%g %g %g", v[0], v[1], v[2]); out = buf; return true;
            }
            case 82: { const float* v = reinterpret_cast<const float*>(payload); _snprintf_s(buf, _TRUNCATE, "%g %g", v[0], v[1]); out = buf; return true; }   // QVector2D
            case 1:  out = *reinterpret_cast<const bool*>(payload) ? "1" : "0"; return true;
            case 2:  _snprintf_s(buf, _TRUNCATE, "%d", *reinterpret_cast<const int*>(payload)); out = buf; return true;
            case 3:  _snprintf_s(buf, _TRUNCATE, "%u", *reinterpret_cast<const unsigned*>(payload)); out = buf; return true;
            case 6:  _snprintf_s(buf, _TRUNCATE, "%g", *reinterpret_cast<const double*>(payload)); out = buf; return true;
            case 38: _snprintf_s(buf, _TRUNCATE, "%g", *reinterpret_cast<const float*>(payload)); out = buf; return true;
            default: return false;
            }
        }
        bool ReadStdString(const BYTE* s, std::string& out)
        {
            size_t size = *reinterpret_cast<const size_t*>(s + 16), cap = *reinterpret_cast<const size_t*>(s + 24);
            if (size > 8192 || cap < size) return false;
            const char* p = cap < 16 ? reinterpret_cast<const char*>(s) : *reinterpret_cast<const char* const*>(s);
            if (!p) return false;
            out.assign(p, size);
            return true;
        }
        bool KeyValueWalk(const BYTE* ent, const char* key, std::string& out)
        {
            const BYTE* head = *reinterpret_cast<const BYTE* const*>(ent + rt::off_Entity_KeyValues);
            size_t size = *reinterpret_cast<const size_t*>(ent + rt::off_Entity_KeyValues + 8);
            if (!head || size == 0 || size > 4096 || head[25] != 1) return false;
            const BYTE* node = *reinterpret_cast<const BYTE* const*>(head + 0);        // leftmost
            for (size_t n = 0; node && node != head && node[25] == 0 && n <= size; ++n)
            {
                std::string k;
                if (ReadStdString(node + 32, k) && k == key) return ReadVariantText(node + 64, out);
                // successor
                const BYTE* right = *reinterpret_cast<const BYTE* const*>(node + 16);
                if (right && right[25] == 0)
                {
                    node = right;
                    for (const BYTE* l = *reinterpret_cast<const BYTE* const*>(node + 0); l && l[25] == 0; l = *reinterpret_cast<const BYTE* const*>(node + 0)) node = l;
                }
                else
                {
                    const BYTE* parent = *reinterpret_cast<const BYTE* const*>(node + 8);
                    while (parent && parent[25] == 0 && node == *reinterpret_cast<const BYTE* const*>(parent + 16)) { node = parent; parent = *reinterpret_cast<const BYTE* const*>(node + 8); }
                    node = parent;
                }
            }
            return false;
        }
        bool KeyValueSeh(const BYTE* ent, const char* key, std::string* out)
        {
            __try { return KeyValueWalk(ent, key, *out); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
    }

    bool EntityKeyValue(RadPtr ent, const char* key, std::string& out)
    {
        if (!ent || !key || !rt::VerifyBinary()) return false;
        return KeyValueSeh(ent, key, &out);
    }

    bool PrefabStackPlacement(int level, float origin[3], float angles[3])
    {
        if (level < 1 || size_t(level) > g_placements.size()) return false;
        memcpy(origin, g_placements[size_t(level) - 1].origin, 3 * sizeof(float));
        memcpy(angles, g_placements[size_t(level) - 1].angles, 3 * sizeof(float));
        return true;
    }

    bool SelectEntityByGuid(const std::string& entityGuid)
    {
        std::vector<RadPtr> insts;
        ForEachEntity([&](RadPtr ent)
        {
            if (EntityGuid(ent) != entityGuid) return;
            ForEachEntityBrush(ent, [&](RadPtr b) { if (RadPtr bi = FindBrushInst(b)) insts.push_back(bi); });
        });
        if (insts.empty()) { LOG("hook: SelectEntityByGuid: no brush instances for %s", entityGuid.c_str()); return false; }
        DeselectAll();
        SelectBrushes(insts);
        return true;
    }

    bool EnterSelectedPrefab()
    {
        if (!o_Map_EnterPrefab) return false;
        int before = PrefabDepthNow();
        hk_Map_EnterPrefab(0, 0, 1, 0);                      // same shape as Radiant's own "Enter Prefab" action
        return PrefabDepthNow() == before + 1;
    }

    bool LeavePrefab()
    {
        if (!o_Map_LeavePrefab || PrefabDepthNow() <= 0) return false;
        int before = PrefabDepthNow();
        hk_Map_LeavePrefab(1, 0, 0, 0);
        return PrefabDepthNow() == before - 1;
    }

    // Radiant keeps the label POINTER of every undo operation: labels are copied into host storage that is never
    // freed (a plugin's string would dangle after a reload)
    static const char* InternLabel(const char* label)
    {
        static std::mutex mutex;
        static std::set<std::string> labels;
        std::lock_guard<std::mutex> lock(mutex);
        return labels.insert(label && *label ? label : "plugin").first->c_str();
    }

    static int g_untracked = 0;
    void BeginUntracked() { ++g_untracked; }
    void EndUntracked() { if (g_untracked > 0) --g_untracked; }
    void ResetUntracked() { if (g_untracked) LOG("hook: untracked edit depth %d reset", g_untracked); g_untracked = 0; }
    bool GetUndoTags(RadPtr o, bool entity, int tags[3])
    {
        if (!o || !tags) return false;
        tags[0] = *reinterpret_cast<int*>(o + (entity ? rt::off_Entity_UndoId : rt::off_Brush_UndoId));
        tags[1] = *reinterpret_cast<int*>(o + (entity ? rt::off_Entity_RedoId : rt::off_Brush_RedoId));
        tags[2] = entity ? *reinterpret_cast<int*>(o + rt::off_Entity_Id) : 0;
        return true;
    }
    bool SetUndoTags(RadPtr o, bool entity, const int tags[3])
    {
        if (!o || !tags) return false;
        *reinterpret_cast<int*>(o + (entity ? rt::off_Entity_UndoId : rt::off_Brush_UndoId)) = tags[0];
        *reinterpret_cast<int*>(o + (entity ? rt::off_Entity_RedoId : rt::off_Brush_RedoId)) = tags[1];
        if (entity && tags[2]) *reinterpret_cast<int*>(o + rt::off_Entity_Id) = tags[2];
        return true;
    }

    bool ImportMapText(const std::string& text, const char* undoLabel)
    {
        using IsOpen_t = bool(__fastcall*)();
        // like Radiant's paste: join an open operation, untracked = record nothing
        bool undoOpen = g_untracked > 0 || Func<IsOpen_t>(rt::Undo_IsOpen)();
        std::string buf = text;                 // the importer advances the pointer
        char* p = &buf[0];
        void* sel = Global<void>(rt::g_selectedBrushes);
        Func<ParseSession_t>(rt::BeginParseSession)("RadiantPlugins");
        if (!undoOpen) { Func<UndoBegin_t>(rt::Undo_Begin)(InternLabel(undoLabel)); Func<Collection1_t>(rt::Undo_AddBrushList)(sel); }
        Func<Collection1_t>(rt::Selection_BeginBatch)(sel);
        // keep target/linkto: Radiant's paste remap deletes them unless this flag is up (restored after)
        char* keepLinks = Global<char>(rt::g_importKeepLinks);
        char keepLinksWas = *keepLinks;
        *keepLinks = 1;
        __int64 r = Func<Map_ImportBuffer_t>(rt::Map_ImportBuffer)(&p, kImportFlags, 0);
        *keepLinks = keepLinksWas;
        Func<EndBatch_t>(rt::Selection_EndBatch)(sel, 3, 0, 1);
        if (!undoOpen) { Func<Collection1_t>(rt::Selection_CloseUndo)(sel); Func<Void_t>(rt::Undo_End)(); }
        Func<Void_t>(rt::EndParseSession)();
        LOG("hook: ImportMapText(%zu bytes%s) -> %lld", text.size(), g_untracked > 0 ? ", untracked" : "", (long long)r);
        return true;
    }

    void BeginUndo(const char* label)
    {
        using IsOpen_t = bool(__fastcall*)();
        if (Func<IsOpen_t>(rt::Undo_IsOpen)()) { LOG("hook: BeginUndo('%s') while an operation is open; joining it", label ? label : ""); return; }
        Func<UndoBegin_t>(rt::Undo_Begin)(InternLabel(label));
        Func<Collection1_t>(rt::Undo_AddBrushList)(Global<void>(rt::g_selectedBrushes));
    }
    void EndUndo()
    {
        using IsOpen_t = bool(__fastcall*)();
        if (!Func<IsOpen_t>(rt::Undo_IsOpen)()) return;
        void* sel = Global<void>(rt::g_selectedBrushes);
        Func<Collection1_t>(rt::Selection_CloseUndo)(sel);
        Func<Void_t>(rt::Undo_End)();
    }

    bool DeleteBrushes(const std::vector<RadPtr>& brushes, const char* undoLabel)
    {
        if (brushes.empty()) return true;
        std::vector<RadPtr> insts;
        for (RadPtr b : brushes) { RadPtr bi = FindBrushInst(b); if (bi) insts.push_back(bi); else LOG("hook: no BrushInst for brush 0x%p", b); }
        if (insts.empty()) return false;
        using IsOpen_t = bool(__fastcall*)();
        bool undoOpen = Func<IsOpen_t>(rt::Undo_IsOpen)();
        void* sel = Global<void>(rt::g_selectedBrushes);
        DeselectAll();
        SelectBrushes(insts);
        bool untracked = g_untracked > 0;
        if (!untracked)
        {
            if (!undoOpen) { Func<UndoBegin_t>(rt::Undo_Begin)(InternLabel(undoLabel)); }
            Func<Collection1_t>(rt::Undo_AddBrushList)(sel);      // the brushes about to go must be in the record, also inside a plugin's bracket
        }
        Func<Void_t>(rt::Select_Delete)();
        if (!untracked && !undoOpen) { Func<Collection1_t>(rt::Selection_CloseUndo)(sel); Func<Void_t>(rt::Undo_End)(); }
        LOG("hook: DeleteBrushes(%zu%s)", brushes.size(), untracked ? ", untracked" : "");
        return true;
    }

    std::string EntityGuid(RadPtr ent) { return ent ? GuidText(ent + rt::off_Entity_Guid) : ""; }
    RadPtr      WorldEntity()
    {
        if (!rt::VerifyBinary()) return nullptr;
        // Map_LoadFile stores the worldspawn's EntityInst here (the 776-byte instance), not the entity_t.
        BYTE* inst = *Global<BYTE*>(rt::g_worldEntity);
        return inst ? *reinterpret_cast<BYTE**>(inst + rt::off_EInst_Entity) : nullptr;
    }
    std::string BrushGuid(RadPtr brush) { return brush ? GuidText(brush + rt::off_Brush_Guid) : ""; }
    RadPtr      BrushOwner(RadPtr brush) { return brush ? *reinterpret_cast<BYTE**>(brush + rt::off_Brush_Owner) : nullptr; }

    bool WriteEntityText(RadPtr ent, std::string& out)
    {
        // the writer takes a FILE* from Radiant's CRT (MSVCR110). One scratch file per thread stays open and is rewound
        // per entity (open/close/delete cost about 8 ms each, the virus scanner). "T" keeps it cached, "D" deletes it at exit.
        static HMODULE crt = GetModuleHandleW(L"MSVCR110.dll");
        using wfopen_t = void* (__cdecl*)(const wchar_t*, const wchar_t*);
        using fclose_t = int (__cdecl*)(void*);
        using fseek_t = int (__cdecl*)(void*, long, int);
        using ftell_t = long (__cdecl*)(void*);
        using fread_t = size_t (__cdecl*)(void*, size_t, size_t, void*);
        static auto crt_wfopen = crt ? reinterpret_cast<wfopen_t>(GetProcAddress(crt, "_wfopen")) : nullptr;
        static auto crt_fclose = crt ? reinterpret_cast<fclose_t>(GetProcAddress(crt, "fclose")) : nullptr;
        static auto crt_fseek = crt ? reinterpret_cast<fseek_t>(GetProcAddress(crt, "fseek")) : nullptr;
        static auto crt_ftell = crt ? reinterpret_cast<ftell_t>(GetProcAddress(crt, "ftell")) : nullptr;
        static auto crt_fread = crt ? reinterpret_cast<fread_t>(GetProcAddress(crt, "fread")) : nullptr;
        if (!crt_wfopen || !crt_fclose || !crt_fseek || !crt_ftell || !crt_fread) { LOG("hook: MSVCR110 file functions not found"); return false; }

        thread_local void* t_file = nullptr;
        if (!t_file)
        {
            wchar_t dir[MAX_PATH]; GetTempPathW(MAX_PATH, dir);
            wchar_t path[MAX_PATH]; _snwprintf_s(path, _TRUNCATE, L"%sRadiantPlugins_ent_%lu_%lu.map", dir, GetCurrentProcessId(), GetCurrentThreadId());
            t_file = crt_wfopen(path, L"w+bTD");
            if (!t_file) { LOGW(L"hook: cannot open %s", path); return false; }
        }
        out.clear();
        if (crt_fseek(t_file, 0, SEEK_SET) != 0) { crt_fclose(t_file); t_file = nullptr; return false; }
        Func<Entity_Write_t>(rt::Entity_Write)(ent, t_file, 0);
        long size = crt_ftell(t_file);                      // end of this entity, older longer text past it is ignored
        if (size < 0 || crt_fseek(t_file, 0, SEEK_SET) != 0) { crt_fclose(t_file); t_file = nullptr; return false; }
        out.resize(size_t(size));
        if (size > 0 && crt_fread(&out[0], 1, size_t(size), t_file) != size_t(size)) { out.clear(); crt_fclose(t_file); t_file = nullptr; return false; }
        return true;
    }
}

// ---- patch vertices: Radiant's Drag Vertices selection driven from code ----------------------------------------
namespace hooks
{
    namespace
    {
        constexpr int kMaxPatchSide = 4096;
        constexpr int kMaxPatchVertSel = 1024;   // Radiant's patch connect stops at 1025

        struct PatchView { BYTE* ctrl; int rows; int cols; };
        bool PatchViewSeh(const BYTE* brush, PatchView* out)
        {
            __try
            {
                const BYTE* patch = *reinterpret_cast<BYTE* const*>(brush + rt::off_Brush_Patch);
                if (!patch) return false;
                int rows = *reinterpret_cast<const int*>(patch + rt::off_Patch_Rows);
                int cols = *reinterpret_cast<const int*>(patch + rt::off_Patch_Cols);
                BYTE* ctrl = *reinterpret_cast<BYTE* const*>(patch + rt::off_Patch_Ctrl);
                if (!ctrl || rows < 1 || cols < 1 || rows > kMaxPatchSide || cols > kMaxPatchSide) return false;
                volatile BYTE touch = ctrl[0]; (void)touch;
                touch = ctrl[size_t(rows) * cols * rt::sz_PatchCtrl - 1];
                *out = { ctrl, rows, cols };
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        bool PatchView_(RadPtr brush, PatchView& out) { return brush && rt::VerifyBinary() && PatchViewSeh(brush, &out); }

        bool ConnectSeh()
        {
            using Connect_t = void(__fastcall*)(char);
            __try { Func<Connect_t>(rt::Patch_ConnectVerts)(0); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
    }

    RadPtr PatchBrushByGuid(const std::string& guid, bool selectedOnly)
    {
        GUID want;
        if (!rt::VerifyBinary() || !ParseGuidText(guid, want)) return nullptr;
        PatchView v;
        for (RadPtr inst : SelectedBrushPtrs())
        {
            RadPtr b = BrushOfInst(inst);
            if (b && memcmp(b + rt::off_Brush_Guid, &want, sizeof(GUID)) == 0) return PatchView_(b, v) ? b : nullptr;
        }
        if (selectedOnly) return nullptr;
        std::vector<RadPtr> found;
        FindBrushesByGuid({ guid }, found);
        return !found.empty() && PatchView_(found[0], v) ? found[0] : nullptr;
    }

    int PatchGrid(RadPtr brush, int* rows, int* cols, float* xyz, int cap)
    {
        PatchView v;
        if (!PatchView_(brush, v)) return 0;
        if (rows) *rows = v.rows;
        if (cols) *cols = v.cols;
        int n = v.rows * v.cols;
        for (int i = 0; xyz && i < n && i < cap; ++i) memcpy(xyz + 3 * i, v.ctrl + size_t(i) * rt::sz_PatchCtrl, 3 * sizeof(float));
        return n;
    }

    int PatchSelectVerts(const std::vector<std::pair<RadPtr, int>>& refs)
    {
        if (!rt::VerifyBinary()) return -1;
        if (refs.size() > size_t(kMaxPatchVertSel)) return -1;
        std::vector<BYTE*> ptrs;
        for (auto& r : refs)
        {
            PatchView v;
            if (!PatchView_(r.first, v)) return -2;
            if (r.second < 0 || r.second >= v.rows * v.cols) return -3;
            ptrs.push_back(v.ctrl + size_t(r.second) * rt::sz_PatchCtrl);
        }
        int* count = Global<int>(rt::g_patchVertSelCount);
        BYTE** sel = Global<BYTE*>(rt::g_patchVertSel);
        *count = 0;   // never leave a half-written list visible
        for (size_t i = 0; i < ptrs.size(); ++i) sel[i] = ptrs[i];
        *Global<int>(rt::g_modelVertCandCount) = 0;
        *count = int(ptrs.size());
        RequestRedraw(rt::kRedraw3D | rt::kRedraw2D);
        return int(ptrs.size());
    }

    int PatchSelectedVertCount() { return rt::VerifyBinary() ? *Global<int>(rt::g_patchVertSelCount) : 0; }
    int DragMode() { return rt::VerifyBinary() ? *Global<int>(rt::g_dragMode) : -1; }

    int PatchWeldSelected(const float xyz[3])
    {
        if (!xyz || !rt::VerifyBinary()) return -1;
        int* count = Global<int>(rt::g_patchVertSelCount);
        int n = *count;
        if (n <= 0) return 0;
        if (n > kMaxPatchVertSel) return -1;
        // every selected vertex must still sit in a selected patch (undo, redo and inserts re-create patches)
        std::vector<PatchView> patches;
        for (RadPtr inst : SelectedBrushPtrs()) { PatchView v; if (PatchView_(BrushOfInst(inst), v)) patches.push_back(v); }
        if (patches.empty()) return -1;
        BYTE** sel = Global<BYTE*>(rt::g_patchVertSel);
        for (int i = 0; i < n; ++i)
        {
            bool inside = false;
            for (auto& v : patches)
            {
                size_t off = size_t(sel[i] - v.ctrl);
                if (sel[i] >= v.ctrl && off < size_t(v.rows) * v.cols * rt::sz_PatchCtrl && off % rt::sz_PatchCtrl == 0) { inside = true; break; }
            }
            if (!inside) return -1;
        }
        memcpy(Global<float>(rt::g_modelVertCand), xyz, 3 * sizeof(float));
        *Global<int>(rt::g_modelVertCandCount) = 1;
        int* tolerant = Global<int>(rt::g_tolerantWeldFlag);
        int savedTolerant = *tolerant;
        *tolerant = 0;   // the tolerant weld ignores height and our target
        bool ok = ConnectSeh();
        *tolerant = savedTolerant;
        *count = 0;
        *Global<int>(rt::g_modelVertCandCount) = 0;
        if (!ok) { LOG("hook: Patch_ConnectVerts faulted"); return -1; }
        RequestRedraw(rt::kRedraw3D | rt::kRedraw2D);
        return n;
    }
}
