// Color picker extras in apeColorDialog (Select Color, opened by every color property): the shared colorx widgets moved
// into each dialog and taken back out in its destructor. APE keeps its own recent colors and eyedropper.
#include "modtools_plugin.hpp"
#include "colorpicker.h"
#include "colorextras.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace ba::colorpicker
{
namespace
{
    using colorx::qt;
    using colorx::pieces;
    using colorx::QColorRaw;

    // IDA addresses (base 0x140000000), rebased through ApeAddress
    const uintptr_t kCtor        = 0x1400C9EA0;   // apeColorDialog(this, QWidget* parent, const QColor* initial, bool alpha)
    const uintptr_t kRefresh     = 0x1400CC980;   // (this, bool notify): pixmaps, +280, spin boxes, notify = live update of the property
    const uintptr_t kDtor        = 0x1400CBC90;   // complete dtor, the one the property's stack dialog calls
    const uintptr_t kDeletingDtor = 0x1400CBF10;  // vtable slot 3, inlines the complete dtor
    const uintptr_t kAccept      = 0x1400CD0B0;   // accept slot: recent colors to QSettings, QDialog::accept
    const uintptr_t kVtbl        = 0x140810E88;

    // apeColorDialog fields
    const size_t off_H = 232, off_S = 240, off_V = 248, off_A = 256;   // doubles 0..1
    const size_t off_Current = 280;            // QColor, rebuilt by refresh
    const size_t off_HasAlpha = 297;           // bool, the property has alpha (A spin box and strip shown)
    const size_t off_Red = 88;                 // QSpinBox R, in the Current Values grid
    const size_t off_Picker = 224;             // QToolButton, the eyedropper

    using CtorFn = void* (*)(void* dlg, void* parent, const void* color, bool alpha);
    using RefreshFn = void (*)(void* dlg, bool notify);
    using DtorFn = void (*)(void* dlg);
    using DeletingDtorFn = void* (*)(void* dlg, unsigned flags);

    CtorFn g_origCtor = nullptr;
    RefreshFn g_origRefresh = nullptr;
    DtorFn g_origDtor = nullptr;
    DeletingDtorFn g_origDeleting = nullptr;
    void* g_vtbl = nullptr;
    void* g_ctorAddr = nullptr;
    void* g_dtorAddr = nullptr;
    void* g_acceptAddr = nullptr;
    bool g_ready = false;
    bool g_extras = true;               // [colorpicker] extras
    unsigned g_opened = 0;

    void* g_dlg = nullptr;
    void* g_hexLayout = nullptr;        // the Current Values grid
    void* g_eyeLayout = nullptr;        // the eyedropper row's box
    void* g_kelvinLayout = nullptr;     // the dialog's grid
    void* g_buttonBox = nullptr;        // OK/Cancel

    void* Ape(uintptr_t ida) { return rp::Ape() ? rp::Ape()->ApeAddress(rp::self, ida) : nullptr; }
    bool StartsFunction(void* target)
    {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fn = target ? RtlLookupFunctionEntry(DWORD64(target), &base, nullptr) : nullptr;
        return fn && DWORD64(fn->BeginAddress) + base == DWORD64(target);
    }
    // the complete dtor is a frameless leaf with no .pdata entry: it must open with lea rax, [the dialog's vtable]
    bool LoadsVtable(void* target, void* vtbl)
    {
        const unsigned char* p = static_cast<const unsigned char*>(target);
        if (!p || !vtbl || p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x05) return false;
        const int32_t disp = *reinterpret_cast<const int32_t*>(p + 3);
        return p + 7 + disp == static_cast<const unsigned char*>(vtbl);
    }
    double& D(void* dlg, size_t off) { return *reinterpret_cast<double*>(static_cast<char*>(dlg) + off); }
    void* Field(void* dlg, size_t off) { return *reinterpret_cast<void**>(static_cast<char*>(dlg) + off); }
    bool HasAlpha(void* dlg) { return *(static_cast<unsigned char*>(dlg) + off_HasAlpha) != 0; }

    void hk_Refresh(void* dlg, bool notify)
    {
        g_origRefresh(dlg, notify);
        if (dlg == g_dlg) colorx::UpdateFields();
    }

    // like APE's spin boxes: h, s, v (and a), then refresh with the live update when the dialog belongs to a property
    void SetColor(const double rgb[3], const double* alpha)
    {
        if (!g_dlg) return;
        double h, s, v;
        colorx::RgbToHsv(rgb, h, s, v);
        if (h >= 0) D(g_dlg, off_H) = h;                // grey: keep the hue strip where it is
        D(g_dlg, off_S) = s;
        D(g_dlg, off_V) = v;
        if (alpha && HasAlpha(g_dlg)) D(g_dlg, off_A) = *alpha < 0 ? 0 : *alpha > 1 ? 1 : *alpha;
        hk_Refresh(g_dlg, qt.parent(g_dlg) != nullptr);
    }

    bool GetColor(double rgba[4])
    {
        if (!g_dlg) return false;
        qt.getRgbF(static_cast<char*>(g_dlg) + off_Current, &rgba[0], &rgba[1], &rgba[2], &rgba[3]);
        rgba[3] = HasAlpha(g_dlg) ? D(g_dlg, off_A) : 1.0;
        return true;
    }

    void Detach()
    {
        colorx::RemoveCopyButtons(g_buttonBox);
        colorx::Take(pieces.hex, g_hexLayout);
        colorx::Take(pieces.floats, g_eyeLayout);
        colorx::Take(pieces.kelvinRow, g_kelvinLayout);
        g_dlg = g_buttonBox = nullptr;
    }

    // Hex beside A: under R/G/B, 0-1 beside the eyedropper, Kelvin in the empty grid row above Recent Colors, Copy at the
    // left of OK/Cancel
    void Attach(void* dlg)
    {
        if (!colorx::Build(SetColor, GetColor)) return;
        if (g_dlg) Detach();
        g_dlg = dlg;
        const ModToolsUiApi* ui = rp::host->ui;
        std::string missing;

        if (void* values = colorx::LayoutOf(qt.parent(Field(dlg, off_Red)), "QGridLayout"))
        {
            qt.gridAddWidget(values, pieces.hex, 3, 2, 1, 2, 0);
            g_hexLayout = values;
        }
        else missing += " hex";

        void* grid = colorx::LayoutOf(dlg, "QGridLayout");
        void* picker = Field(dlg, off_Picker);
        const int index = grid && picker ? qt.indexOf(grid, picker) : -1;
        if (index >= 0)
        {
            int row = 0, col = 0, rs = 0, cs = 0;
            qt.gridItemPosition(grid, index, &row, &col, &rs, &cs);
            void* holder = ui->Widget(nullptr);
            void* h = ui->HBox();
            ui->LayoutMargins(h, 0, 0, 0, 0);
            ui->LayoutSpacing(h, 6);
            ui->SetLayout(holder, h);
            qt.removeWidget(grid, picker);
            ui->BoxAddWidget(h, picker, 0);
            ui->BoxAddWidget(h, pieces.floats, 1);
            qt.gridAddWidget(grid, holder, row, col, 1, 2, 0);   // the Current and Initial Color columns
            g_eyeLayout = h;
        }
        else missing += " 0-1";

        if (grid)
        {
            qt.gridAddWidget(grid, pieces.kelvinRow, 5, 0, 1, -1, 0);
            g_kelvinLayout = grid;
        }
        else missing += " kelvin";

        if (void* bb = ui->FindChild(dlg, "QDialogButtonBox", 0))
        {
            colorx::AddCopyButtons(bb);
            g_buttonBox = bb;
        }
        else missing += " copy";

        for (void* w : { pieces.hex, pieces.floats, pieces.kelvinRow, pieces.copyHex, pieces.copyFloats }) ui->SetVisible(w, 1);
        if (!missing.empty()) rp::Log("colorpicker: dialog 0x%p, left out (layout not found):%s", dlg, missing.c_str());
    }

    // ---- detours ----
    void* hk_Ctor(void* dlg, void* parent, const void* color, bool alpha)
    {
        void* ret = g_origCtor(dlg, parent, color, alpha);
        if (*static_cast<void**>(dlg) != g_vtbl || !g_extras) return ret;
        ++g_opened;
        Attach(dlg);
        colorx::UpdateFields();
        return ret;
    }

    void hk_Dtor(void* dlg)
    {
        if (dlg == g_dlg) Detach();
        g_origDtor(dlg);
    }

    void* hk_DeletingDtor(void* dlg, unsigned flags)
    {
        if (dlg == g_dlg) Detach();
        return g_origDeleting(dlg, flags);
    }

    // "colorpicker open [alpha]": a dialog with no property behind it (no live update), from a posted call so the command returns
    alignas(16) char g_testDlg[0x400];
    bool g_testAlpha = false;
    void OpenTest(void*)
    {
        QColorRaw initial = {};
        qt.setRgba(&initial, 0xFFFC4343u);
        reinterpret_cast<CtorFn>(g_ctorAddr)(g_testDlg, nullptr, &initial, g_testAlpha);
        const int r = qt.exec(g_testDlg);
        rp::Log("colorpicker: test dialog closed (%s), color %s", r == 1 ? "OK" : "Cancel",
                colorx::HexText(qt.rgba(g_testDlg + off_Current)).c_str());
        reinterpret_cast<DtorFn>(g_dtorAddr)(g_testDlg);
    }
}

void Install()
{
    g_extras = rp::host->ConfigGetInt(rp::self, "colorpicker", "extras", 1) != 0;
    const bool ok = rp::Ape() && colorx::Bind();
    g_ctorAddr = Ape(kCtor);
    g_dtorAddr = Ape(kDtor);
    g_acceptAddr = Ape(kAccept);
    void* refresh = Ape(kRefresh);
    void* deleting = Ape(kDeletingDtor);
    g_vtbl = Ape(kVtbl);
    const bool found = g_ctorAddr && g_dtorAddr && g_acceptAddr && refresh && deleting && g_vtbl
                    && StartsFunction(g_ctorAddr) && LoadsVtable(g_dtorAddr, g_vtbl) && StartsFunction(refresh) && StartsFunction(deleting)
                    && StartsFunction(g_acceptAddr);
    if (ok && found && g_extras)
    {
        g_ready = rp::host->Detour(rp::self, g_ctorAddr, reinterpret_cast<void*>(&hk_Ctor), reinterpret_cast<void**>(&g_origCtor))
               && rp::host->Detour(rp::self, g_dtorAddr, reinterpret_cast<void*>(&hk_Dtor), reinterpret_cast<void**>(&g_origDtor))
               && rp::host->Detour(rp::self, deleting, reinterpret_cast<void*>(&hk_DeletingDtor), reinterpret_cast<void**>(&g_origDeleting))
               && rp::host->Detour(rp::self, refresh, reinterpret_cast<void*>(&hk_Refresh), reinterpret_cast<void**>(&g_origRefresh));
    }
    rp::Log("colorpicker: %s", !ok ? "unavailable (Qt or host mismatch)" : !found ? "APE binary not recognised, off"
                               : !g_extras ? "off ([colorpicker] extras=0)" : g_ready ? "detours ok" : "detour FAILED");
}

void Shutdown()
{
    if (g_dlg) Detach();
    colorx::Destroy();
}

bool Command(const std::string& command, std::string& reply)
{
    if (command.compare(0, 11, "colorpicker") != 0) return false;
    const std::string arg = command.size() > 12 ? command.substr(12) : std::string();
    if (!g_ready) { reply = "colorpicker unavailable"; return true; }
    auto rest = [&](size_t n) { return arg.size() > n ? arg.substr(n) : std::string(); };
    if (arg == "open" || arg == "open alpha")
    {
        if (g_dlg) { reply = "a color dialog is already open"; return true; }
        g_testAlpha = arg == "open alpha";
        rp::host->PostToMainThread(rp::self, OpenTest, nullptr);
        reply = "opening a Select Color dialog";
        return true;
    }
    const bool needsDialog = arg.compare(0, 4, "hex ") == 0 || arg.compare(0, 7, "floats ") == 0 || arg.compare(0, 7, "kelvin ") == 0
                          || arg == "accept" || arg == "cancel";
    if (needsDialog && !g_dlg) { reply = "no color dialog open"; return true; }
    if (arg.compare(0, 4, "hex ") == 0) colorx::TypeHex(rest(4));
    else if (arg.compare(0, 7, "floats ") == 0) colorx::TypeFloats(rest(7));
    else if (arg.compare(0, 7, "kelvin ") == 0) colorx::SetKelvin(atoi(rest(7).c_str()));
    else if (arg == "accept" || arg == "cancel")
    {
        if (arg == "accept") reinterpret_cast<DtorFn>(g_acceptAddr)(g_dlg); else qt.reject(g_dlg);
        reply = arg + "ed";
        return true;
    }
    std::string state = "no dialog open";
    double c[4];
    if (GetColor(c))
    {
        char ptr[32];
        snprintf(ptr, sizeof ptr, "dialog 0x%p", g_dlg);
        state = std::string(ptr) + (HasAlpha(g_dlg) ? " (alpha)" : "") + ": color " + colorx::HexText(qt.rgba(static_cast<char*>(g_dlg) + off_Current))
              + " (" + colorx::FloatText(c[0], c[1], c[2]) + ", alpha " + colorx::Num(c[3]) + "); fields hex '" + colorx::FieldText(false)
              + "' floats '" + colorx::FieldText(true) + "'";
    }
    reply = "colorpicker: extras " + std::string(g_extras ? "on" : "off") + ", " + std::to_string(g_opened) + " opened; " + state
          + "\n  colorpicker open [alpha]|accept|cancel, colorpicker hex <text>|floats <text>|kelvin <K>";
    return true;
}
}
