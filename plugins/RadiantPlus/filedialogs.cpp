// Windows file dialogs. Radiant opens its file dialogs as QRadFileDialog (a QFileDialog subclass) with DontUseNativeDialog set,
// and Qt also refuses the native dialog for any subclass. While on, the flag is dropped and the subclass reports
// QFileDialog's meta object, so Qt shows the Windows dialog.
#include "modtools_plugin.hpp"
#include "filedialogs.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace br::filedialogs
{
namespace
{
    // IDA addresses (no ASLR), resolved through host->RadiantAddress
    const uintptr_t kRadDialogVtbl = 0x1424FB138;   // QRadFileDialog's vtable, slot 0 = metaObject thunk 0x140011F4A
    const uintptr_t kRadMetaObject = 0x140620F90;   // QRadFileDialog::metaObject
    const uintptr_t kRadStaticMeta = 0x14331F720;   // QRadFileDialog::staticMetaObject (no methods, no properties)
    const int kDontUseNativeDialog = 0x10;          // QFileDialog::Option

    using SetOptionFn = void (*)(void* dialog, int option, bool on);
    using MetaObjectFn = const void* (*)(const void* obj);

    SetOptionFn  g_origSetOption = nullptr;
    MetaObjectFn g_origMeta = nullptr;
    const void*  g_radMeta = nullptr;
    const void*  g_fileDialogMeta = nullptr;   // QFileDialog::staticMetaObject
    bool g_enabled = false;                    // [files] windows_dialogs
    unsigned g_dropped = 0;

    void hk_SetOption(void* dialog, int option, bool on)
    {
        if (g_enabled && option == kDontUseNativeDialog && on) { on = false; ++g_dropped; }
        g_origSetOption(dialog, option, on);
    }

    // QFileDialogPrivate::canBeNativeDialog compares metaObject()->className() with "QFileDialog"
    const void* hk_MetaObject(const void* obj)
    {
        const void* m = g_origMeta(obj);
        return g_enabled && m == g_radMeta && g_fileDialogMeta ? g_fileDialogMeta : m;
    }

    uintptr_t FollowJump(uintptr_t p)
    {
        for (int i = 0; i < 4 && p && *reinterpret_cast<const uint8_t*>(p) == 0xE9; ++i)
            p = p + 5 + *reinterpret_cast<const int32_t*>(p + 1);
        return p;
    }
}

void Setup()
{
    g_enabled = rp::host->ConfigGetInt(rp::self, "files", "windows_dialogs", 0) != 0;
    HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll");
    void* setOption = w ? reinterpret_cast<void*>(GetProcAddress(w, "?setOption@QFileDialog@@QEAAXW4Option@1@_N@Z")) : nullptr;
    g_fileDialogMeta = w ? reinterpret_cast<const void*>(GetProcAddress(w, "?staticMetaObject@QFileDialog@@2UQMetaObject@@B")) : nullptr;
    void** vtbl = static_cast<void**>(rp::host->RadiantAddress(rp::self, kRadDialogVtbl));
    void* meta = rp::host->RadiantAddress(rp::self, kRadMetaObject);
    g_radMeta = rp::host->RadiantAddress(rp::self, kRadStaticMeta);
    const bool vtblOk = vtbl && meta && FollowJump(reinterpret_cast<uintptr_t>(vtbl[0])) == reinterpret_cast<uintptr_t>(meta);
    if (setOption && g_fileDialogMeta && vtblOk && g_radMeta)
    {
        if (!rp::host->Detour(rp::self, setOption, reinterpret_cast<void*>(&hk_SetOption), reinterpret_cast<void**>(&g_origSetOption))) g_origSetOption = nullptr;
        if (g_origSetOption && !rp::host->Detour(rp::self, meta, reinterpret_cast<void*>(&hk_MetaObject), reinterpret_cast<void**>(&g_origMeta))) g_origMeta = nullptr;
    }
    rp::Log("filedialogs: Windows file dialogs %s; setOption detour %s, metaObject detour %s%s", g_enabled ? "on" : "off ([files] windows_dialogs=0)",
            g_origSetOption ? "ok" : "FAILED", g_origMeta ? "ok" : "FAILED", vtblOk ? "" : " (QRadFileDialog vtable not as expected)");
}

bool Enabled() { return g_enabled; }

void Set(bool on)
{
    if (on == g_enabled) return;
    g_enabled = on;
    rp::host->ConfigSet(rp::self, "files", "windows_dialogs", on ? "1" : "0");
    rp::Log("filedialogs: Windows file dialogs %s", on ? "on" : "off");
}

bool Command(const std::string& command, std::string& reply)
{
    if (command.compare(0, 11, "filedialogs") != 0) return false;
    const std::string arg = command.size() > 12 ? command.substr(12) : std::string();
    if (arg == "on" || arg == "1") Set(true);
    else if (arg == "off" || arg == "0") Set(false);
    char buf[256];
    snprintf(buf, sizeof buf, "filedialogs: %s; setOption detour %s, metaObject detour %s; %u dialogs switched\n  filedialogs on|off\n",
             g_enabled ? "on (Windows dialogs)" : "off (Radiant's Qt dialogs)", g_origSetOption ? "ok" : "missing", g_origMeta ? "ok" : "missing", g_dropped);
    reply = buf;
    return true;
}
}
