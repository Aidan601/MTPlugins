// Color picker extras in QRadColorSelectDialog (Entity Color and the other color prompts): the shared colorx widgets
// moved into each dialog and taken back out in its destructor, an eyedropper icon, recent colors kept in the ini.
#include "modtools_plugin.hpp"
#include "colorpicker.h"
#include "colorextras.h"
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace br::colorpicker
{
namespace
{
    using colorx::qt;
    using colorx::pieces;
    using colorx::QColorRaw;

    // IDA addresses (no ASLR), resolved through host->RadiantAddress
    const uintptr_t kCtor    = 0x140815740;   // QRadColorSelectDialog(this, const QColor* initial, ...)
    const uintptr_t kDtor    = 0x1408165F0;   // complete dtor, the deleting dtor 0x1408181F0 calls it too
    const uintptr_t kRefresh = 0x140816DD0;   // rebuilds the pixmaps, +304 and the spin boxes from h, s, v, a
    const uintptr_t kAccept  = 0x140817400;   // accept slot: pushes +304 onto the recent row
    const uintptr_t kGetColor = 0x140816700;  // QColor* (QColor* ret, const QColor* initial, ...): construct, exec, destruct
    const uintptr_t kVtbl    = 0x1424FA348;
    const uintptr_t kRecent  = 0x145214C70;   // static QColor[10], the recent row

    // QRadColorSelectDialog fields
    const size_t off_H = 256, off_S = 264, off_V = 272, off_A = 280;   // doubles 0..1
    const size_t off_Current = 304;            // QColor, rebuilt by refresh
    const size_t off_Picker = 96;              // QLabel: press and drag samples the screen
    const size_t off_Red = 152;                // QSpinBox R, in the R/G/B form
    const size_t off_Recent = 176;             // QLabel*[10]
    const int kRecentCount = 10;

    using CtorFn = void* (*)(void* dlg, const void* color, void* a3, void* a4);
    using DlgFn = void (*)(void* dlg);
    using GetColorFn = void* (*)(void* ret, const void* initial, void* a3, void* a4);

    CtorFn g_origCtor = nullptr;
    DlgFn  g_origDtor = nullptr, g_origRefresh = nullptr, g_origAccept = nullptr;
    void*  g_vtbl = nullptr;
    void*  g_acceptAddr = nullptr;
    QColorRaw* g_recent = nullptr;
    bool g_ready = false;
    bool g_extras = true;               // [colorpicker] extras
    bool g_keepRecent = true;           // [colorpicker] keep_recent
    bool g_recentLoaded = false;
    unsigned g_opened = 0;

    // the open dialog and where the pieces sit in it
    void* g_dlg = nullptr;
    void* g_hexLayout = nullptr;        // the R/G/B form
    void* g_eyeLayout = nullptr;        // the eyedropper row's box
    void* g_kelvinLayout = nullptr;     // the right column
    void* g_buttonBox = nullptr;        // OK/Cancel

    template <class T> T At(uintptr_t ida) { return reinterpret_cast<T>(rp::host->RadiantAddress(rp::self, ida)); }
    double& D(void* dlg, size_t off) { return *reinterpret_cast<double*>(static_cast<char*>(dlg) + off); }
    void* Field(void* dlg, size_t off) { return *reinterpret_cast<void**>(static_cast<char*>(dlg) + off); }

    void hk_Refresh(void* dlg)
    {
        g_origRefresh(dlg);
        if (dlg == g_dlg) colorx::UpdateFields();
    }

    // like a click in the picker: h, s, v (and a), then refresh
    void SetColor(const double rgb[3], const double* alpha)
    {
        if (!g_dlg) return;
        double h, s, v;
        colorx::RgbToHsv(rgb, h, s, v);
        if (h >= 0) D(g_dlg, off_H) = h;                // grey: keep the hue strip where it is
        D(g_dlg, off_S) = s;
        D(g_dlg, off_V) = v;
        if (alpha) D(g_dlg, off_A) = *alpha < 0 ? 0 : *alpha > 1 ? 1 : *alpha;
        hk_Refresh(g_dlg);
    }

    bool GetColor(double rgba[4])
    {
        if (!g_dlg) return false;
        qt.getRgbF(static_cast<char*>(g_dlg) + off_Current, &rgba[0], &rgba[1], &rgba[2], &rgba[3]);
        rgba[3] = D(g_dlg, off_A);
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

    // Hex beside A: under R/G/B, 0-1 beside the eyedropper, Kelvin above the recent row, Copy at the left of OK/Cancel.
    // The right column keeps its height, so the picker does not move.
    void Attach(void* dlg)
    {
        if (!colorx::Build(SetColor, GetColor)) return;
        if (g_dlg) Detach();
        g_dlg = dlg;
        const ModToolsUiApi* ui = rp::host->ui;
        std::string missing;

        if (void* rgb = colorx::LayoutOf(qt.parent(Field(dlg, off_Red)), "QFormLayout"))
        {
            qt.addRow(rgb, pieces.hex);
            g_hexLayout = rgb;
        }
        else missing += " hex";

        void* box = Field(dlg, off_Picker);
        void* form = box ? colorx::LayoutOf(qt.parent(box), "QFormLayout") : nullptr;
        int row = -1, role = 0;
        if (form) qt.getWidgetPosition(form, box, &row, &role);
        if (row >= 0)
        {
            void* holder = ui->Widget(nullptr);
            void* h = ui->HBox();
            ui->LayoutMargins(h, 0, 0, 0, 0);
            ui->LayoutSpacing(h, 6);
            ui->SetLayout(holder, h);
            qt.removeWidget(form, box);
            ui->BoxAddWidget(h, box, 0);
            ui->BoxAddWidget(h, pieces.floats, 1);
            qt.setWidget(form, row, 2 /* SpanningRole */, holder);
            g_eyeLayout = h;
        }
        else missing += " 0-1";

        void* recentRow = qt.parent(Field(dlg, off_Recent));
        void* column = recentRow ? qt.parent(recentRow) : nullptr;
        void* layout = column ? qt.layout(column) : nullptr;
        const int index = layout ? qt.indexOf(layout, recentRow) : -1;
        if (index >= 0)
        {
            qt.insertWidget(layout, index, pieces.kelvinRow, 0, 0);
            g_kelvinLayout = layout;
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

    // 20x20 eyedropper, dark grey on transparent
    void EyedropperPixels(unsigned char* bgra, int size)
    {
        struct Seg { float ax, ay, bx, by, r; };
        const Seg segs[] = {
            { 2.6f, 17.4f, 5.0f, 15.0f, 0.9f },          // tip
            { 4.6f, 15.4f, 11.6f, 8.4f, 1.7f },          // tube
            { 8.6f, 6.4f, 13.6f, 11.4f, 1.3f },          // collar
            { 11.6f, 8.4f, 15.0f, 5.0f, 2.8f },          // bulb
        };
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                const float px = x + 0.5f, py = y + 0.5f;
                float cover = 0;
                for (const Seg& s : segs)
                {
                    const float dx = s.bx - s.ax, dy = s.by - s.ay;
                    float t = ((px - s.ax) * dx + (py - s.ay) * dy) / (dx * dx + dy * dy);
                    t = t < 0 ? 0 : t > 1 ? 1 : t;
                    const float ex = px - (s.ax + t * dx), ey = py - (s.ay + t * dy);
                    float c = s.r + 0.5f - sqrtf(ex * ex + ey * ey);
                    c = c < 0 ? 0 : c > 1 ? 1 : c;
                    if (c > cover) cover = c;
                }
                unsigned char* p = bgra + (size_t(y) * size + x) * 4;
                p[0] = p[1] = p[2] = 0x40;
                p[3] = static_cast<unsigned char>(cover * 255.0f + 0.5f);
            }
    }

    // the picker box gets an icon (a child label the mouse goes through), a cross cursor and a tooltip
    void DecoratePicker(void* dlg)
    {
        void* box = Field(dlg, off_Picker);
        if (!box) return;
        const ModToolsUiApi* ui = rp::host->ui;
        ui->SetToolTip(box, "Eyedropper: press here and drag over anything on screen, the color under the cursor is picked");
        alignas(16) char cursor[16];
        qt.cursorCtor(cursor, 2);                        // Qt::CrossCursor
        qt.setCursor(box, cursor);
        qt.cursorDtor(cursor);

        const int size = 20;
        std::vector<unsigned char> px(size_t(size) * size * 4);
        EyedropperPixels(px.data(), size);
        void* icon = ui->Label("", box);
        void* lay = icon ? ui->VBox(box) : nullptr;
        if (!lay) return;
        ui->LayoutMargins(lay, 0, 0, 0, 0);
        ui->BoxAddWidget(lay, icon, 1);
        qt.setAttribute(icon, 51, true);                 // Qt::WA_TransparentForMouseEvents: presses reach the box
        qt.setAlignment(icon, 0x84);                     // AlignCenter
        alignas(16) char img[64], pix[64];
        qt.imageCtor(img, px.data(), size, size, size * 4, 5 /* ARGB32 */, nullptr, nullptr);
        qt.pixmapFromImage(pix, img, 0);
        qt.setPixmap(icon, pix);
        qt.pixmapDtor(pix);
        qt.imageDtor(img);
    }

    // ---- recent colors ----
    std::string RecentText(const QColorRaw& c)
    {
        const unsigned v = qt.rgba(&c);
        char buf[16];
        snprintf(buf, sizeof buf, "%02X%02X%02X%02X", (v >> 16) & 255, (v >> 8) & 255, v & 255, v >> 24);
        return buf;
    }

    void LoadRecent()
    {
        if (g_recentLoaded || !g_recent) return;
        g_recentLoaded = true;
        if (!g_keepRecent) return;
        char buf[512] = {};
        rp::host->ConfigGet(rp::self, "colorpicker", "recent", "", buf, sizeof buf);
        int n = 0;
        for (const char* p = buf; *p && n < kRecentCount;)
        {
            while (*p == ',' || *p == ' ') ++p;
            char tok[16] = {};
            int len = 0;
            while (p[len] && p[len] != ',' && len < 15) { tok[len] = p[len]; ++len; }
            p += len;
            if (len != 8 || strspn(tok, "0123456789abcdefABCDEF") != 8) continue;
            const unsigned long v = strtoul(tok, nullptr, 16);   // RRGGBBAA
            qt.setRgba(&g_recent[n++], unsigned((v & 255) << 24 | v >> 8));
        }
        if (n) rp::Log("colorpicker: %d recent colors restored", n);
    }

    void SaveRecent()
    {
        if (!g_keepRecent || !g_recent) return;
        std::string out;
        for (int i = 0; i < kRecentCount; ++i)
            if (g_recent[i].spec) out += (out.empty() ? "" : ",") + RecentText(g_recent[i]);
        rp::host->ConfigSet(rp::self, "colorpicker", "recent", out.c_str());
    }

    // the dialog compares colors with their spec too: a restored RGB color and the same HSV color would both stay
    void Dedupe()
    {
        if (!g_recent || !g_recent[0].spec) return;
        const unsigned first = qt.rgba(&g_recent[0]);
        int w = 1;
        for (int r = 1; r < kRecentCount; ++r)
        {
            if (g_recent[r].spec && qt.rgba(&g_recent[r]) == first) continue;
            if (w != r) g_recent[w] = g_recent[r];
            ++w;
        }
        for (; w < kRecentCount; ++w) g_recent[w] = QColorRaw{ 0, { 0xFFFF, 0, 0, 0, 0 }, 0 };
    }

    void RecentTooltips(void* dlg)
    {
        for (int i = 0; i < kRecentCount; ++i)
        {
            void* label = Field(dlg, off_Recent + size_t(i) * 8);
            if (!label) continue;
            const std::string tip = g_recent[i].spec ? colorx::HexText(qt.rgba(&g_recent[i])) + "  (click to use)"
                                                     : std::string("Recent colors appear here after OK");
            rp::host->ui->SetToolTip(label, tip.c_str());
        }
    }

    // ---- detours ----
    void* hk_Ctor(void* dlg, const void* color, void* a3, void* a4)
    {
        LoadRecent();
        void* ret = g_origCtor(dlg, color, a3, a4);
        if (*static_cast<void**>(dlg) != g_vtbl) return ret;
        ++g_opened;
        if (g_extras)
        {
            Attach(dlg);
            DecoratePicker(dlg);
            colorx::UpdateFields();
        }
        RecentTooltips(dlg);
        return ret;
    }

    void hk_Dtor(void* dlg)
    {
        if (dlg == g_dlg) Detach();
        g_origDtor(dlg);
    }

    void hk_Accept(void* dlg)
    {
        g_origAccept(dlg);
        Dedupe();
        SaveRecent();
    }

    // "colorpicker open": runs Radiant's own prompt from a posted call so the command returns
    void OpenTest(void*)
    {
        QColorRaw initial = {}, result = {};
        qt.setRgba(&initial, 0xFFFC4343u);
        At<GetColorFn>(kGetColor)(&result, &initial, nullptr, nullptr);
        rp::Log("colorpicker: test prompt closed, result %s", result.spec ? colorx::HexText(qt.rgba(&result)).c_str() : "invalid");
    }
}

void Setup()
{
    g_extras = rp::host->ConfigGetInt(rp::self, "colorpicker", "extras", 1) != 0;
    g_keepRecent = rp::host->ConfigGetInt(rp::self, "colorpicker", "keep_recent", 1) != 0;
    const bool ok = colorx::Bind();
    g_vtbl = At<void*>(kVtbl);
    g_recent = At<QColorRaw*>(kRecent);
    void* ctor = At<void*>(kCtor);
    void* dtor = At<void*>(kDtor);
    void* refresh = At<void*>(kRefresh);
    g_acceptAddr = At<void*>(kAccept);
    if (ok && g_vtbl && g_recent && ctor && dtor && refresh && g_acceptAddr && (g_extras || g_keepRecent))
    {
        g_ready = rp::host->Detour(rp::self, ctor, reinterpret_cast<void*>(&hk_Ctor), reinterpret_cast<void**>(&g_origCtor))
               && rp::host->Detour(rp::self, dtor, reinterpret_cast<void*>(&hk_Dtor), reinterpret_cast<void**>(&g_origDtor))
               && rp::host->Detour(rp::self, refresh, reinterpret_cast<void*>(&hk_Refresh), reinterpret_cast<void**>(&g_origRefresh))
               && rp::host->Detour(rp::self, g_acceptAddr, reinterpret_cast<void*>(&hk_Accept), reinterpret_cast<void**>(&g_origAccept));
    }
    rp::Log("colorpicker: %s; extras %s, recent colors %s", !ok ? "unavailable (Qt or host mismatch)" : g_ready ? "detours ok" : "off or detour FAILED",
            g_extras ? "on" : "off", g_keepRecent ? "kept" : "not kept");
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
    if (!g_ready) { reply = "colorpicker unavailable\n"; return true; }
    auto rest = [&](size_t n) { return arg.size() > n ? arg.substr(n) : std::string(); };
    if (arg == "open")
    {
        if (g_dlg) { reply = "a color dialog is already open\n"; return true; }
        rp::host->PostToMainThread(rp::self, OpenTest, nullptr);
        reply = "opening Radiant's color prompt\n";
        return true;
    }
    const bool needsDialog = arg.compare(0, 4, "hex ") == 0 || arg.compare(0, 7, "floats ") == 0 || arg.compare(0, 7, "kelvin ") == 0
                          || arg == "accept" || arg == "cancel";
    if (needsDialog && !g_dlg) { reply = "no color dialog open\n"; return true; }
    if (arg.compare(0, 4, "hex ") == 0) colorx::TypeHex(rest(4));      // types into the field like a user, then applies
    else if (arg.compare(0, 7, "floats ") == 0) colorx::TypeFloats(rest(7));
    else if (arg.compare(0, 7, "kelvin ") == 0) colorx::SetKelvin(atoi(rest(7).c_str()));
    else if (arg == "accept" || arg == "cancel")
    {
        if (arg == "accept") reinterpret_cast<DlgFn>(g_acceptAddr)(g_dlg); else qt.reject(g_dlg);
        reply = arg + "ed\n";
        return true;
    }
    else if (arg.compare(0, 6, "parse ") == 0)
    {
        double c[4] = { 0, 0, 0, 1 };
        bool alpha = false;
        const bool ok = colorx::Parse(rest(6), c, alpha);
        char buf[160];
        snprintf(buf, sizeof buf, ok ? "%.4f %.4f %.4f%s\n" : "not a color\n", c[0], c[1], c[2], alpha ? (" a " + colorx::Num(c[3])).c_str() : "");
        reply = buf;
        return true;
    }
    std::string recent;
    for (int i = 0; i < kRecentCount; ++i) recent += g_recent[i].spec ? " " + colorx::HexText(qt.rgba(&g_recent[i])) : std::string(" -");
    std::string state = "no dialog open";
    double c[4];
    if (GetColor(c))
    {
        char ptr[32];
        snprintf(ptr, sizeof ptr, "dialog 0x%p", g_dlg);
        state = std::string(ptr) + ": color " + colorx::HexText(qt.rgba(static_cast<char*>(g_dlg) + off_Current)) + " ("
              + colorx::FloatText(c[0], c[1], c[2]) + ", alpha " + colorx::Num(c[3]) + "); fields hex '" + colorx::FieldText(false)
              + "' floats '" + colorx::FieldText(true) + "'";
    }
    reply = "colorpicker: extras " + std::string(g_extras ? "on" : "off") + ", recent " + (g_keepRecent ? "kept" : "not kept") + ", "
          + std::to_string(g_opened) + " opened; " + state + "\n  recent:" + recent
          + "\n  colorpicker open|accept|cancel, colorpicker hex <text>|floats <text>|kelvin <K>|parse <text>\n";
    return true;
}
}
