// Models and Textures Icon Size: ThumbnailBrowser::setIconSize resizes the view, drops the model's thumbnails and reruns
// the whole search (SQL query, every row fetched), about 330 ms per slider step. Inside setIconSize the model keeps its
// thumbnails, drawn at the slider's size, and the search waits: both run once 250 ms after the last step.
#include "modtools_plugin.hpp"
#include "iconsize.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace br::iconsize
{
namespace
{
    // IDA addresses (no ASLR), resolved through host->RadiantAddress
    const uintptr_t kSetIconSize  = 0x140B4C8A0;   // ThumbnailBrowser::setIconSize(int): view icon and grid size, QSettings, vtable +472
    const uintptr_t kUpdateSearch = 0x140B4C430;   // ThumbnailBrowser::UpdateSearch (vtable +472)
    const uintptr_t kModelSetSize = 0x140B4A6D0;   // the browser's SQL model setIconSize(QSize): size at +32, drops the thumbnails (vtable +384), grey placeholder
    const size_t    off_Browser_Model = 48;        // the SQL model
    const size_t    off_Browser_Size  = 88;        // QSize: the icon size setIconSize stored
    const size_t    off_Browser_Delegate = 80;     // the item delegate (TextureBrowserItemDelegate for Textures)
    const size_t    off_Opt_DecorationSize = 80;   // QStyleOptionViewItem::decorationSize (Qt 5.3 x64)
    const size_t    off_Opt_Icon = 160;            // QStyleOptionViewItem::icon
    const uint64_t  kRestMs       = 250;

    using SetIconSizeFn  = void (*)(void* browser, int size);
    using UpdateSearchFn = int64_t (*)(void* browser);
    using ModelSetSizeFn = void (*)(void* model, int64_t size);
    ModelSetSizeFn g_origModelSetSize = nullptr;
    using InitStyleOptionFn = void (*)(const void* delegate, void* option, const void* index);
    InitStyleOptionFn g_origInitStyleOption = nullptr;
    struct QSizeRaw { int w, h; };
    struct QtFns
    {
        void* (*iconPixmap)(const void* icon, void* ret, const QSizeRaw* size, int mode, int state);
        void* (*pixmapScaled)(const void* pix, void* ret, int w, int h, int aspect, int mode);
        int   (*pixmapWidth)(const void* pix);
        void  (*pixmapDtor)(void* pix);
        void* (*iconFromPixmap)(void* icon, const void* pix);
        void* (*iconAssign)(void* icon, const void* other);
        void  (*iconDtor)(void* icon);
    } qt = {};
    SetIconSizeFn  g_origSetIconSize = nullptr;
    UpdateSearchFn g_origUpdateSearch = nullptr;

    bool g_enabled = true;          // [browsers] live_icon_size
    bool g_inSetIconSize = false;
    struct Pending { void* browser; void* delegate; int size; uint64_t dueMs; };
    std::vector<Pending> g_pending;
    unsigned g_held = 0, g_ran = 0;

    template <class T> T At(uintptr_t ida) { return reinterpret_cast<T>(rp::host->RadiantAddress(rp::self, ida)); }

    void Hold(void* browser, int size)
    {
        const uint64_t due = GetTickCount64() + kRestMs;
        ++g_held;
        for (Pending& p : g_pending) if (p.browser == browser) { p.dueMs = due; p.size = size; return; }
        g_pending.push_back({ browser, *reinterpret_cast<void**>(static_cast<char*>(browser) + off_Browser_Delegate), size, due });
    }
    void ApplyModelSize(void* browser)
    {
        void* model = *reinterpret_cast<void**>(static_cast<char*>(browser) + off_Browser_Model);
        if (model) g_origModelSetSize(model, *reinterpret_cast<int64_t*>(static_cast<char*>(browser) + off_Browser_Size));
    }
    void hk_SetIconSize(void* browser, int size)
    {
        g_inSetIconSize = g_enabled;
        g_origSetIconSize(browser, size);
        if (g_inSetIconSize) Hold(browser, size);
        g_inSetIconSize = false;
    }
    void hk_ModelSetSize(void* model, int64_t size)
    {
        if (!g_inSetIconSize) g_origModelSetSize(model, size);
    }
    // while held, the old thumbnails are drawn at the new size (Qt never draws an icon above its pixmap's size)
    void hk_InitStyleOption(const void* delegate, void* option, const void* index)
    {
        g_origInitStyleOption(delegate, option, index);
        for (const Pending& p : g_pending)
            if (p.delegate == delegate)
            {
                int* size = reinterpret_cast<int*>(static_cast<char*>(option) + off_Opt_DecorationSize);
                size[0] = size[1] = p.size;
                if (!qt.iconDtor) break;
                void* icon = static_cast<char*>(option) + off_Opt_Icon;
                alignas(16) char pix[64], big[64], scaledIcon[16];
                const QSizeRaw want = { p.size, p.size };
                qt.iconPixmap(icon, pix, &want, 0 /* Normal */, 1 /* Off */);
                if (qt.pixmapWidth(pix) > 0 && qt.pixmapWidth(pix) < p.size)
                {
                    qt.pixmapScaled(pix, big, p.size, p.size, 1 /* KeepAspectRatio */, 0 /* Fast */);
                    qt.iconFromPixmap(scaledIcon, big);
                    qt.iconAssign(icon, scaledIcon);
                    qt.iconDtor(scaledIcon);
                    qt.pixmapDtor(big);
                }
                qt.pixmapDtor(pix);
                break;
            }
    }
    int64_t hk_UpdateSearch(void* browser)
    {
        if (g_inSetIconSize) return 0;
        for (size_t i = 0; i < g_pending.size(); ++i)       // a search from elsewhere covers the held one
            if (g_pending[i].browser == browser) { g_pending.erase(g_pending.begin() + ptrdiff_t(i)); ApplyModelSize(browser); break; }
        return g_origUpdateSearch(browser);
    }
    void RunDue(bool all)
    {
        const uint64_t now = GetTickCount64();
        for (size_t i = 0; i < g_pending.size();)
        {
            if (!all && now < g_pending[i].dueMs) { ++i; continue; }
            void* browser = g_pending[i].browser;
            g_pending.erase(g_pending.begin() + ptrdiff_t(i));
            ++g_ran;
            ApplyModelSize(browser);
            g_origUpdateSearch(browser);
        }
    }
}

void Setup()
{
    g_enabled = rp::host->ConfigGetInt(rp::self, "browsers", "live_icon_size", 1) != 0;
    if (!rp::host->Detour) return;
    void* set = At<void*>(kSetIconSize);
    void* search = At<void*>(kUpdateSearch);
    void* modelSize = At<void*>(kModelSetSize);
    if (!set || !search || !modelSize) return;
    if (!rp::host->Detour(rp::self, search, reinterpret_cast<void*>(&hk_UpdateSearch), reinterpret_cast<void**>(&g_origUpdateSearch)))
    {
        g_origUpdateSearch = nullptr;
        return;
    }
    if (!rp::host->Detour(rp::self, modelSize, reinterpret_cast<void*>(&hk_ModelSetSize), reinterpret_cast<void**>(&g_origModelSetSize)))
    {
        g_origModelSetSize = nullptr;
        return;
    }
    const HMODULE gui = GetModuleHandleW(L"Qt5Gui64r.dll");
    QtFns f = {};
    f.iconPixmap     = reinterpret_cast<decltype(f.iconPixmap)>(GetProcAddress(gui, "?pixmap@QIcon@@QEBA?AVQPixmap@@AEBVQSize@@W4Mode@1@W4State@1@@Z"));
    f.pixmapScaled   = reinterpret_cast<decltype(f.pixmapScaled)>(GetProcAddress(gui, "?scaled@QPixmap@@QEBA?AV1@HHW4AspectRatioMode@Qt@@W4TransformationMode@3@@Z"));
    f.pixmapWidth    = reinterpret_cast<decltype(f.pixmapWidth)>(GetProcAddress(gui, "?width@QPixmap@@QEBAHXZ"));
    f.pixmapDtor     = reinterpret_cast<decltype(f.pixmapDtor)>(GetProcAddress(gui, "??1QPixmap@@UEAA@XZ"));
    f.iconFromPixmap = reinterpret_cast<decltype(f.iconFromPixmap)>(GetProcAddress(gui, "??0QIcon@@QEAA@AEBVQPixmap@@@Z"));
    f.iconAssign     = reinterpret_cast<decltype(f.iconAssign)>(GetProcAddress(gui, "??4QIcon@@QEAAAEAV0@AEBV0@@Z"));
    f.iconDtor       = reinterpret_cast<decltype(f.iconDtor)>(GetProcAddress(gui, "??1QIcon@@QEAA@XZ"));
    if (f.iconPixmap && f.pixmapScaled && f.pixmapWidth && f.pixmapDtor && f.iconFromPixmap && f.iconAssign && f.iconDtor) qt = f;
    void* init = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"Qt5Widgets64r.dll"),
        "?initStyleOption@QStyledItemDelegate@@MEBAXPEAVQStyleOptionViewItem@@AEBVQModelIndex@@@Z"));
    if (init && !rp::host->Detour(rp::self, init, reinterpret_cast<void*>(&hk_InitStyleOption), reinterpret_cast<void**>(&g_origInitStyleOption)))
        g_origInitStyleOption = nullptr;
    if (!rp::host->Detour(rp::self, set, reinterpret_cast<void*>(&hk_SetIconSize), reinterpret_cast<void**>(&g_origSetIconSize)))
        g_origSetIconSize = nullptr;
}

void Tick()
{
    if (!g_pending.empty()) RunDue(false);
}

void Shutdown()
{
    if (g_origUpdateSearch) RunDue(true);
}

bool Command(const std::string& command, std::string& reply)
{
    if (command.compare(0, 8, "iconsize") != 0) return false;
    if (command == "iconsize on" || command == "iconsize off")
    {
        g_enabled = command == "iconsize on";
        rp::host->ConfigSet(rp::self, "browsers", "live_icon_size", g_enabled ? "1" : "0");
    }
    char buf[200];
    snprintf(buf, sizeof buf, "iconsize: live %s, hooks %s, stretch %s, searches held %u, run after rest %u, waiting %zu\n", g_enabled ? "on" : "off",
             g_origSetIconSize && g_origUpdateSearch && g_origModelSetSize ? "on" : "missing", g_origInitStyleOption && qt.iconDtor ? "on" : "missing", g_held, g_ran, g_pending.size());
    reply = buf;
    return true;
}
}
