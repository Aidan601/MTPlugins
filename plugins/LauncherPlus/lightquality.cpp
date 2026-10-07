// "Draft" in front of Low in the launcher's Light quality box: see lightquality.h.
#include "lightquality.h"
#include "modtools_plugin.hpp"
#include <windows.h>
#include <cstring>

namespace lightquality
{
namespace
{
    struct QArrayDataRaw { int ref; int size; unsigned alloc; ptrdiff_t offset; };
    struct QListDataD { int ref, alloc, begin, end; void* array[1]; };
    struct QListRaw { QListDataD* d; };
    struct QMetaObjectRaw;
    struct Qt
    {
        bool bound = false;
        QListRaw* (*allWidgets)(QListRaw*) = nullptr;
        void (*listDispose)(QListDataD*) = nullptr;
        const char* (*className)(const QMetaObjectRaw*) = nullptr;
        int (*count)(const void*) = nullptr;
        void** (*itemText)(const void*, void**, int) = nullptr;
        int (*currentIndex)(const void*) = nullptr;
        void (*setCurrentIndex)(void*, int) = nullptr;
        void (*insertItem)(void*, int, const void*, const void*) = nullptr;
        void (*removeItem)(void*, int) = nullptr;
        void* (*qstringFromUtf8)(void*, const char*, int) = nullptr;
        void (*qsDtor)(void**) = nullptr;
        void (*variantCtor)(void*) = nullptr;
        void (*variantDtor)(void*) = nullptr;
    } qt;
    template <class T> bool Get(T& fn, HMODULE m, const char* name) { fn = reinterpret_cast<T>(GetProcAddress(m, name)); return fn != nullptr; }

    bool BindQt()
    {
        HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll"), c = GetModuleHandleW(L"Qt5Core64r.dll");
        if (!w || !c) return false;
        bool ok = true;
        ok &= Get(qt.allWidgets, w, "?allWidgets@QApplication@@SA?AV?$QList@PEAVQWidget@@@@XZ");
        ok &= Get(qt.listDispose, c, "?dispose@QListData@@SAXPEAUData@1@@Z");
        ok &= Get(qt.className, c, "?className@QMetaObject@@QEBAPEBDXZ");
        ok &= Get(qt.count, w, "?count@QComboBox@@QEBAHXZ");
        ok &= Get(qt.itemText, w, "?itemText@QComboBox@@QEBA?AVQString@@H@Z");
        ok &= Get(qt.currentIndex, w, "?currentIndex@QComboBox@@QEBAHXZ");
        ok &= Get(qt.setCurrentIndex, w, "?setCurrentIndex@QComboBox@@QEAAXH@Z");
        ok &= Get(qt.insertItem, w, "?insertItem@QComboBox@@QEAAXHAEBVQString@@AEBVQVariant@@@Z");
        ok &= Get(qt.removeItem, w, "?removeItem@QComboBox@@QEAAXH@Z");
        ok &= Get(qt.qstringFromUtf8, c, "?fromUtf8@QString@@SA?AV1@PEBDH@Z");
        ok &= Get(qt.qsDtor, c, "??1QString@@QEAA@XZ");
        ok &= Get(qt.variantCtor, c, "??0QVariant@@QEAA@XZ");
        ok &= Get(qt.variantDtor, c, "??1QVariant@@QEAA@XZ");
        return qt.bound = ok;
    }

    // ASCII only, enough for the box's entries
    std::string Ascii(void** qs)
    {
        std::string out;
        auto* d = qs ? static_cast<QArrayDataRaw*>(*qs) : nullptr;
        if (!d) return out;
        const wchar_t* p = reinterpret_cast<const wchar_t*>(reinterpret_cast<const char*>(d) + d->offset);
        for (int i = 0; i < d->size; ++i) out += p[i] < 128 ? char(p[i]) : '?';
        return out;
    }
    std::string ItemText(void* combo, int i)
    {
        void* qs = nullptr; qt.itemText(combo, &qs, i);
        std::string s = Ascii(&qs);
        qt.qsDtor(&qs);
        return s;
    }

    const char* const kNames[] = { "draft", "low", "medium", "high" };
    std::wstring g_ini;
    bool (*g_lightRouted)() = nullptr;
    void* g_combo = nullptr;
    bool g_added = false;
    int g_lastIndex = -2;
    ULONGLONG g_nextSearch = 0;
    std::string g_status = "the Light quality box was not found yet";

    // The launcher's mLightQualityWidget: a QComboBox holding Low, Medium, High (or Draft and those, after a reload).
    void* FindCombo()
    {
        void* hit = nullptr;
        QListRaw list = {}; qt.allWidgets(&list);
        if (list.d)
            for (int i = list.d->begin; i < list.d->end && !hit; ++i)
            {
                void* w = list.d->array[i];
                if (!w) continue;
                auto metaObject = (*reinterpret_cast<const QMetaObjectRaw* (__fastcall***)(void*)>(w))[0];
                const char* cls = qt.className(metaObject(w));
                if (!cls || strcmp(cls, "QComboBox") != 0) continue;
                const int n = qt.count(w), first = n == 4 && ItemText(w, 0) == "Draft" ? 1 : 0;
                if (n - first == 3 && ItemText(w, first) == "Low" && ItemText(w, first + 1) == "Medium" && ItemText(w, first + 2) == "High") hit = w;
            }
        if (list.d && list.d->ref != -1 && InterlockedDecrement(reinterpret_cast<volatile long*>(&list.d->ref)) == 0) qt.listDispose(list.d);
        return hit;
    }

    void Record(int index)
    {
        if (index < 0 || index > 3) return;
        WritePrivateProfileStringW(L"light", L"quality", index == 0 ? L"draft" : index == 1 ? L"low" : index == 2 ? L"medium" : L"high", g_ini.c_str());
        WritePrivateProfileStringW(L"light", L"low_rays", index == 0 ? L"64" : L"256", g_ini.c_str());
        g_status = std::string("Light quality ") + kNames[index] + (index == 0 ? " (Low at 64 rays per voxel)" : "");
    }
    void Forget()
    {
        WritePrivateProfileStringW(L"light", L"quality", nullptr, g_ini.c_str());
        WritePrivateProfileStringW(L"light", L"low_rays", L"256", g_ini.c_str());
    }

    void Add()
    {
        if (qt.count(g_combo) == 3)
        {
            const int cur = qt.currentIndex(g_combo);
            void* qs = nullptr; qt.qstringFromUtf8(&qs, "Draft", 5);
            alignas(16) unsigned char variant[32] = {};
            qt.variantCtor(variant);
            qt.insertItem(g_combo, 0, &qs, variant);
            qt.variantDtor(variant);
            qt.qsDtor(&qs);
            if (cur >= 0) qt.setCurrentIndex(g_combo, cur + 1);   // the same quality stays picked
        }
        g_added = true; g_lastIndex = -2;
        rp::Log("Light quality box: Draft added in front of Low");
    }
    void Remove()
    {
        if (g_combo && qt.count(g_combo) == 4 && ItemText(g_combo, 0) == "Draft")
        {
            const int cur = qt.currentIndex(g_combo);
            qt.removeItem(g_combo, 0);
            qt.setCurrentIndex(g_combo, cur > 0 ? cur - 1 : 0);   // Draft falls back to Low
        }
        g_added = false; g_lastIndex = -2;
        Forget();
        g_status = "Draft is shown only while Light with BetterBuild is on";
    }
}

void Setup(const std::wstring& ini, bool (*lightRouted)())
{
    g_ini = ini; g_lightRouted = lightRouted;
    Forget();   // a pick is on record only while Draft is in the box
    if (!BindQt()) { g_status = "Qt functions for the quality box not found"; rp::Log("Light quality box: %s", g_status.c_str()); }
}

void Tick()
{
    if (!qt.bound) return;
    const bool want = g_lightRouted && g_lightRouted();
    if (!g_combo)
    {
        const ULONGLONG now = GetTickCount64();
        if (!want || now < g_nextSearch) return;
        g_nextSearch = now + 1000;
        g_combo = FindCombo();
        if (!g_combo) return;
        g_added = qt.count(g_combo) == 4;
    }
    if (want && !g_added) Add();
    else if (!want && g_added) Remove();
    if (!g_added) return;
    const int index = qt.currentIndex(g_combo);
    if (index != g_lastIndex) { Record(index); g_lastIndex = index; }
}

void Shutdown()
{
    if (qt.bound && g_added) Remove();
    g_combo = nullptr; g_added = false;
}

std::string Status()
{
    if (!qt.bound || !g_combo) return g_status;
    std::string items;
    for (int i = 0, n = qt.count(g_combo); i < n; ++i) items += (i ? ", " : "") + ItemText(g_combo, i);
    return g_status + " (box: " + items + ")";
}
}
