// Non-modal Settings > Preferences: its one QDialog::exec runs as show() plus a local loop.
#include "modtools_plugin.hpp"
#include "prefswindow.h"
#include <windows.h>
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <string>

namespace br::prefswin
{
namespace
{
    // IDA addresses, resolved through host->RadiantAddress
    const uintptr_t kPrefsSlot    = 0x1406527C0;   // Settings > Preferences... slot (ctor, load, exec, OK tail)
    const uintptr_t kPrefsExecRet = 0x14065286C;   // return address of its QDialog::exec call

    using SlotFn = void (__fastcall*)(void* self);
    using ExecFn = int (__fastcall*)(void* dialog);

    struct Qt
    {
        void (*show)(void*);
        void (*setWindowModality)(void*, int);
        void (*raise)(void*);
        void (*activateWindow)(void*);
        void (*setResult)(void*, int);
        int  (*result)(const void*);
        void* (*loopCtor)(void*, void*);                    // QEventLoop(QObject* parent)
        void (*loopDtor)(void*);
        int  (*loopExec)(void*, int);                       // exec(QFlags), the flags travel as an int
        void* (*connect)(void*, const void*, const char*, const void*, const char*, int);   // static, Connection by hidden pointer
        void (*connDtor)(void*);
        void* (*modalWindow)();                             // QGuiApplication::modalWindow(), status only
        void (*reject)(void*);                              // tests only
        uintptr_t (*winId)(const void*);                    // status only
    } qt = {};

    bool g_enabled = true;          // [preferences] modeless
    bool g_ready = false;
    SlotFn g_origSlot = nullptr;
    ExecFn g_origExec = nullptr;
    void* g_execRet = nullptr;      // live kPrefsExecRet
    void* g_open = nullptr;         // the Preferences dialog while our loop runs it
    unsigned g_shown = 0, g_raised = 0;

    template <class T> bool Get(T& fn, HMODULE m, const char* name)
    {
        fn = m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
        return fn != nullptr;
    }

    // Settings > Preferences while ours is open brings it forward instead of nesting a second one
    void __fastcall hk_PrefsSlot(void* self)
    {
        if (g_open)
        {
            qt.raise(g_open);
            qt.activateWindow(g_open);
            ++g_raised;
            return;
        }
        g_origSlot(self);
    }

    // every QDialog::exec in the process, only the Preferences slot's call runs without modality
    int __fastcall hk_DialogExec(void* dialog)
    {
        if (!g_enabled || g_open || _ReturnAddress() != g_execRet) return g_origExec(dialog);
        g_open = dialog;
        ++g_shown;
        qt.setResult(dialog, 0);
        qt.setWindowModality(dialog, 0);                    // Qt::NonModal (the .ui sets modal=true)
        qt.show(dialog);                                    // no WA_ShowModal, the main window keeps its input
        alignas(16) char loop[64] = {};
        qt.loopCtor(loop, nullptr);
        void* conn = nullptr;
        qt.connect(&conn, dialog, "2finished(int)", loop, "1quit()", 0);
        qt.connDtor(&conn);
        qt.loopExec(loop, 0);                               // also ends on QCoreApplication::quit (Radiant closing)
        qt.loopDtor(loop);
        g_open = nullptr;
        return qt.result(dialog);
    }
}

void Setup()
{
    g_enabled = rp::host->ConfigGetInt(rp::self, "preferences", "modeless", 1) != 0;
    HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll"), c = GetModuleHandleW(L"Qt5Core64r.dll");
    ExecFn exec = nullptr;
    bool ok = Get(qt.show, w, "?show@QWidget@@QEAAXXZ")
           && Get(qt.setWindowModality, w, "?setWindowModality@QWidget@@QEAAXW4WindowModality@Qt@@@Z")
           && Get(qt.raise, w, "?raise@QWidget@@QEAAXXZ")
           && Get(qt.activateWindow, w, "?activateWindow@QWidget@@QEAAXXZ")
           && Get(qt.setResult, w, "?setResult@QDialog@@QEAAXH@Z")
           && Get(qt.result, w, "?result@QDialog@@QEBAHXZ")
           && Get(exec, w, "?exec@QDialog@@UEAAHXZ")
           && Get(qt.loopCtor, c, "??0QEventLoop@@QEAA@PEAVQObject@@@Z")
           && Get(qt.loopDtor, c, "??1QEventLoop@@UEAA@XZ")
           && Get(qt.loopExec, c, "?exec@QEventLoop@@QEAAHV?$QFlags@W4ProcessEventsFlag@QEventLoop@@@@@Z")
           && Get(qt.connect, c, "?connect@QObject@@SA?AVConnection@QMetaObject@@PEBV1@PEBD01W4ConnectionType@Qt@@@Z")
           && Get(qt.connDtor, c, "??1Connection@QMetaObject@@QEAA@XZ");
    Get(qt.reject, w, "?reject@QDialog@@UEAAXXZ");
    Get(qt.winId, w, "?winId@QWidget@@QEBA_KXZ");
    Get(qt.modalWindow, GetModuleHandleW(L"Qt5Gui64r.dll"), "?modalWindow@QGuiApplication@@SAPEAVQWindow@@XZ");
    void* slot = rp::host->RadiantAddress(rp::self, kPrefsSlot);
    g_execRet = rp::host->RadiantAddress(rp::self, kPrefsExecRet);
    if (!ok || !slot || !g_execRet) { rp::Log("prefswin: Preferences stays modal (Qt or Radiant mismatch)"); return; }
    if (!rp::host->Detour(rp::self, slot, reinterpret_cast<void*>(&hk_PrefsSlot), reinterpret_cast<void**>(&g_origSlot))
        || !rp::host->Detour(rp::self, reinterpret_cast<void*>(exec), reinterpret_cast<void*>(&hk_DialogExec), reinterpret_cast<void**>(&g_origExec)))
    { rp::Log("prefswin: Preferences stays modal (detour failed)"); return; }
    g_ready = true;
    rp::Log("prefswin: Preferences without blocking: %s", g_enabled ? "on" : "off ([preferences] modeless=0)");
}

bool Command(const std::string& c, std::string& reply)
{
    if (c != "prefswin" && c.rfind("prefswin ", 0) != 0) return false;
    std::string arg = c.size() > 9 ? c.substr(9) : "";
    if (arg == "on" || arg == "off")
    {
        g_enabled = arg == "on";
        rp::host->ConfigSet(rp::self, "preferences", "modeless", g_enabled ? "1" : "0");
        reply = std::string("prefswin: ") + (g_enabled ? "on" : "off") + " (from the next Settings > Preferences)";
        return true;
    }
    // test aids: open from the next idle tick (the command returns at once), close as Cancel does
    if (arg == "open")
    {
        if (!rp::Has(&rp::host->TriggerMenuAction)) { reply = "prefswin: no TriggerMenuAction"; return true; }
        rp::host->PostToMainThread(rp::self, [](void*) { rp::host->TriggerMenuAction(rp::self, "Settings/Preferences..."); }, nullptr);
        reply = "prefswin: Settings > Preferences... queued";
        return true;
    }
    if (arg == "close")
    {
        if (!g_open || !qt.reject) { reply = "prefswin: no Preferences opened by this module"; return true; }
        qt.reject(g_open);
        reply = "prefswin: rejected";
        return true;
    }
    HWND h = g_open && qt.winId ? reinterpret_cast<HWND>(qt.winId(g_open)) : nullptr;
    char buf[320];
    snprintf(buf, sizeof buf, "prefswin: %s, hooks %s, open now %s (hwnd %p visible %d owner %p), modal window %s, shown %u, raised %u (prefswin on|off|open|close)",
             g_enabled ? "on" : "off", g_ready ? "installed" : "missing", g_open ? "yes" : "no",
             static_cast<void*>(h), h ? (IsWindowVisible(h) ? 1 : 0) : -1, h ? static_cast<void*>(GetWindow(h, GW_OWNER)) : nullptr,
             !qt.modalWindow ? "?" : qt.modalWindow() ? "yes" : "none", g_shown, g_raised);
    reply = buf;
    return true;
}
}
