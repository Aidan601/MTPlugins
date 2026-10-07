// The Theme editor shared by Radiant Plus and APE Plus (see themeeditor.h). Clean-room: only Qt's exported API.
#include "modtools_plugin.hpp"
#include "themeeditor.h"
#include "themeicons.h"
#include <windows.h>
#include <shellapi.h>
#include <objidl.h>
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }   // gdiplus.h wants min/max, NOMINMAX removes the macros
#include <gdiplus.h>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")

namespace themeedit
{
namespace
{
    Host g_host;
    std::string g_error;

    // ---- Qt exports, no headers (MS x64 ABI: a QString is returned through a hidden pointer) ----
    struct QArrayDataRaw { int ref; int size; unsigned alloc; ptrdiff_t offset; };
    struct Qt
    {
        bool tried = false, ok = false;
        void* (*fromUtf8)(void*, const char*, int) = nullptr;
        void (*qsDtor)(void**) = nullptr;
        void (*setStyleSheet)(void*, const void*) = nullptr;
        void** (*getText)(void**, void*, const void*, const void*, int, const void*, bool*, int, int) = nullptr;
        void** (*windowTitle)(const void*, void**) = nullptr;
    } qt;
    template <class T> bool Get(T& fn, HMODULE m, const char* name) { fn = reinterpret_cast<T>(GetProcAddress(m, name)); return fn != nullptr; }
    bool BindQt()
    {
        if (qt.tried) return qt.ok;
        qt.tried = true;
        HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll"), c = GetModuleHandleW(L"Qt5Core64r.dll");
        if (!w || !c) return false;
        bool ok = Get(qt.fromUtf8, c, "?fromUtf8@QString@@SA?AV1@PEBDH@Z") && Get(qt.qsDtor, c, "??1QString@@QEAA@XZ")
               && Get(qt.setStyleSheet, w, "?setStyleSheet@QWidget@@QEAAXAEBVQString@@@Z")
               && Get(qt.windowTitle, w, "?windowTitle@QWidget@@QEBA?AVQString@@XZ");
        Get(qt.getText, w, "?getText@QInputDialog@@SA?AVQString@@PEAVQWidget@@AEBV2@1W4EchoMode@QLineEdit@@1PEA_NV?$QFlags@W4WindowType@Qt@@@@V?$QFlags@W4InputMethodHint@Qt@@@@@Z");
        qt.ok = ok;
        return ok;
    }
    struct QStr
    {
        void* d = nullptr;
        explicit QStr(const std::string& s) { qt.fromUtf8(&d, s.c_str(), int(s.size())); }
        QStr() {}
        ~QStr() { qt.qsDtor(&d); }
        QStr(const QStr&) = delete; QStr& operator=(const QStr&) = delete;
    };
    std::wstring ToWide(const void* qs)
    {
        const QArrayDataRaw* d = *static_cast<QArrayDataRaw* const*>(qs);
        if (!d || d->size <= 0) return std::wstring();
        return std::wstring(reinterpret_cast<const wchar_t*>(reinterpret_cast<const char*>(d) + d->offset), size_t(d->size));
    }
    std::string Narrow(const std::wstring& w)
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? n : 0, '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), &s[0], n, nullptr, nullptr);
        return s;
    }
    std::wstring Widen(const std::string& s)
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
        std::wstring w(n > 0 ? n : 0, L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), &w[0], n);
        return w;
    }
    std::string Lower(std::string s) { for (char& ch : s) ch = char(tolower(static_cast<unsigned char>(ch))); return s; }
    float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
    bool FileExists(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
    bool ReadWhole(const std::wstring& path, std::string& out)
    {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{}; GetFileSizeEx(f, &size);
        out.resize(size.QuadPart > 0 && size.QuadPart < (8 << 20) ? size_t(size.QuadPart) : 0);
        DWORD got = 0;
        bool ok = out.empty() || (ReadFile(f, &out[0], DWORD(out.size()), &got, nullptr) && got == out.size());
        CloseHandle(f);
        return ok;
    }
    std::string SafeFileName(std::string s)
    {
        for (char& ch : s) if (strchr("\\/:*?\"<>|", ch) || static_cast<unsigned char>(ch) < 32) ch = '_';
        while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
        return s;
    }

    enum { P_BG, P_PANEL, P_BASE, P_ALT, P_BORDER, P_TEXT, P_DIM, P_DISABLED, P_ACCENT, P_ACCENT2, P_SEL, P_SELTEXT, P_HOVER,
           P_BUTTON, P_BUTTONHOVER, P_BUTTONPRESS, P_TIP, P_TIPTEXT, P_HANDLE, P_HANDLEHOVER, P_MARK, P_COUNT };
    const char* const kPaletteKeys[P_COUNT] = { "bg", "panel", "base", "alt", "border", "text", "dim", "disabled", "accent",
        "accent2", "sel", "seltext", "hover", "button", "buttonhover", "buttonpress", "tip", "tiptext", "handle", "handlehover", "mark" };
    struct CoreColor { int key; const char* label; };
    const CoreColor kCore[8] = { { P_BG, "Background" }, { P_PANEL, "Panels and menus" }, { P_BASE, "Fields and lists" },
        { P_BORDER, "Borders" }, { P_TEXT, "Text" }, { P_ACCENT, "Accent" }, { P_SEL, "Selection" }, { P_BUTTON, "Buttons" } };
    // icons: where the icons come from (iconFolder under <themes>\icons, "" = the editor's own) and whether their grey
    // ones are redrawn in the icons colour (recolor)
    struct Palette { uint32_t c[P_COUNT] = {}; uint32_t icons = 0; bool recolor = false; std::string iconFolder; };   // 0xRRGGBB

    float Ch(uint32_t c, int shift) { return float((c >> shift) & 0xFF) / 255.f; }
    float Lum(uint32_t c) { return 0.2126f * Ch(c, 16) + 0.7152f * Ch(c, 8) + 0.0722f * Ch(c, 0); }
    uint32_t Mix(uint32_t a, uint32_t b, float t)
    {
        uint32_t out = 0;
        for (int shift : { 16, 8, 0 })
        {
            float v = Ch(a, shift) + (Ch(b, shift) - Ch(a, shift)) * t;
            out |= uint32_t(std::lround(Clamp01(v) * 255.f)) << shift;
        }
        return out;
    }
    std::string Hex(uint32_t c) { char b[8]; snprintf(b, sizeof b, "#%06x", c & 0xFFFFFF); return b; }
    // the shades mixed from the core colors (close to the hand-made palettes of the shipped themes)
    void Derive(Palette& p)
    {
        uint32_t* c = p.c;
        const bool dark = Lum(c[P_BG]) < 0.5f;
        c[P_ALT] = Mix(c[P_BASE], c[P_PANEL], 0.45f);
        c[P_DIM] = Mix(c[P_TEXT], c[P_BG], 0.35f);
        c[P_DISABLED] = Mix(c[P_TEXT], c[P_BG], 0.62f);
        c[P_ACCENT2] = Mix(c[P_ACCENT], 0xFFFFFF, 0.18f);
        c[P_SELTEXT] = Lum(c[P_SEL]) < 0.55f ? 0xFFFFFF : 0x000000;
        c[P_HOVER] = Mix(c[P_BG], c[P_TEXT], 0.1f);
        c[P_BUTTONHOVER] = Mix(c[P_BUTTON], c[P_TEXT], 0.08f);
        c[P_BUTTONPRESS] = dark ? Mix(c[P_BUTTON], c[P_BASE], 0.5f) : Mix(c[P_BUTTON], c[P_TEXT], 0.12f);
        c[P_TIP] = dark ? Mix(c[P_PANEL], c[P_TEXT], 0.05f) : c[P_BASE];
        c[P_TIPTEXT] = c[P_TEXT];
        c[P_HANDLE] = Mix(c[P_BORDER], c[P_TEXT], 0.12f);
        c[P_HANDLEHOVER] = Mix(c[P_BORDER], c[P_TEXT], 0.25f);
        c[P_MARK] = Lum(c[P_ACCENT]) < 0.6f ? 0xFFFFFF : 0x000000;
    }
    // "palette: bg=#202225 panel=#..." in a sheet's header (the shipped themes and the editor's own write it)
    bool ParsePalette(const std::string& sheet, Palette& p)
    {
        size_t at = sheet.find("palette:");
        if (at == std::string::npos || at > 4096) return false;
        size_t end = sheet.find_first_of("\r\n*", at);
        std::string line = sheet.substr(at + 8, end == std::string::npos ? std::string::npos : end - at - 8);
        int found = 0;
        for (int k = 0; k < P_COUNT; ++k)
        {
            std::string tag = std::string(" ") + kPaletteKeys[k] + "=#";
            size_t i = (" " + line).find(tag);
            if (i == std::string::npos) continue;
            p.c[k] = uint32_t(strtoul(line.c_str() + i - 1 + tag.size(), nullptr, 16)) & 0xFFFFFF;
            ++found;
        }
        if (found != P_COUNT) return false;
        size_t ic = (" " + line).find(" icons=#");
        p.icons = ic != std::string::npos ? uint32_t(strtoul(line.c_str() + ic - 1 + 8, nullptr, 16)) & 0xFFFFFF : p.c[P_TEXT];
        // the icon lines of the sheet's header: icon-recolor, icon-pack (a folder, or none)
        std::string head = sheet.substr(0, 4096);
        p.recolor = false; p.iconFolder.clear();
        size_t rc = head.find("icon-recolor:"), pk = head.find("icon-pack:");
        if (rc != std::string::npos)
        {
            p.recolor = true;
            size_t h = head.find('#', rc);
            if (h != std::string::npos && h < rc + 24) p.icons = uint32_t(strtoul(head.c_str() + h + 1, nullptr, 16)) & 0xFFFFFF;
        }
        if (pk != std::string::npos)
        {
            size_t b = head.find_first_not_of(" \t", pk + 10), e = head.find_first_of("\r\n", b), c = head.find("*/", b);
            if (c != std::string::npos && (e == std::string::npos || c < e)) e = c;
            std::string v = b == std::string::npos ? "" : head.substr(b, e == std::string::npos ? std::string::npos : e - b);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '"')) v.pop_back();   // folder names may hold spaces
            if (!v.empty() && v[0] == '"') v.erase(0, 1);
            if (!v.empty() && Lower(v) != "none")
            {
                if (Lower(v).rfind("icons/", 0) == 0 || Lower(v).rfind("icons\\", 0) == 0) v = v.substr(6);
                p.iconFolder = v;
            }
        }
        return true;
    }
    std::string Replace(std::string s, const std::string& from, const std::string& to)
    {
        for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
        return s;
    }
    std::wstring TemplatePath() { return g_host.themesDir + L"\\template\\theme.qss.in"; }
    // the sheet header's icon lines: icon-pack (where the icons come from) and icon-recolor (grey ones redrawn in the icon colour)
    std::string IconLines(const std::string& name, const Palette& p)
    {
        std::string extra;
        if (!p.iconFolder.empty()) extra = "\n   icon-pack: icons/" + p.iconFolder;
        if (p.recolor)
            extra += "\n   icon-recolor: " + Hex(p.icons) + " " + Hex(p.c[P_BG]) + "\n   (the line above has Radiant+ and APE+ redraw the "
                     "grey icons in that colour, into icons\\_generated\\" + name + ", the first time " + name + " is applied)";
        if (p.iconFolder.empty() && !p.recolor) extra = "\n   icon-pack: none";
        return extra;
    }
    bool BuildSheet(const std::string& name, const std::string& key, const Palette& p, const std::string& note, std::string& sheet)
    {
        if (!ReadWhole(TemplatePath(), sheet)) return false;
        std::string palette, extra = IconLines(name, p);
        for (int k = 0; k < P_COUNT; ++k) palette += std::string(k ? " " : "") + kPaletteKeys[k] + "=" + Hex(p.c[k]);
        palette += " icons=" + Hex(p.icons);
        for (int k = 0; k < P_COUNT; ++k) sheet = Replace(sheet, std::string("@") + kPaletteKeys[k] + "@", Hex(p.c[k]));
        sheet = Replace(sheet, "@name@", name);
        sheet = Replace(sheet, "@key@", key);
        sheet = Replace(sheet, "@note@", note);
        sheet = Replace(sheet, "@palette@", palette);
        sheet = Replace(sheet, "@extra@", extra);
        return true;
    }

    // the sheet's small images, drawn as tools/make_themes.py draws them: 8x size, anti-aliased, scaled down
    ULONG_PTR g_gdiplus = 0;
    bool SavePng(Gdiplus::Bitmap& bmp, const std::wstring& path)
    {
        static const CLSID png = { 0x557CF406, 0x1A04, 0x11D3, { 0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E } };
        return bmp.Save(path.c_str(), &png, nullptr) == Gdiplus::Ok;
    }
    enum class Glyph { Up, Down, Left, Right, Check, Partial, Dot, Cross, Float };
    bool DrawGlyph(const std::wstring& path, int size, Glyph g, uint32_t rgb)
    {
        using namespace Gdiplus;
        const float n = float(size * 8), s = 8.f;
        Bitmap big(size * 8, size * 8, PixelFormat32bppARGB);
        {
            Graphics gr(&big);
            gr.SetSmoothingMode(SmoothingModeAntiAlias);
            gr.Clear(Color(0, 0, 0, 0));
            Color c(255, BYTE(rgb >> 16), BYTE(rgb >> 8), BYTE(rgb));
            auto lines = [&](std::initializer_list<PointF> pts, float width) {
                Pen pen(c, width * s); pen.SetLineJoin(LineJoinRound);
                std::vector<PointF> v(pts); gr.DrawLines(&pen, v.data(), INT(v.size()));
            };
            const float m = n * 0.26f;
            switch (g)
            {
            case Glyph::Down:  lines({ { m, n * .36f }, { n / 2, n - n * .34f }, { n - m, n * .36f } }, 1.7f); break;
            case Glyph::Up:    lines({ { m, n - n * .36f }, { n / 2, n * .34f }, { n - m, n - n * .36f } }, 1.7f); break;
            case Glyph::Right: lines({ { n * .36f, m }, { n - n * .34f, n / 2 }, { n * .36f, n - m } }, 1.7f); break;
            case Glyph::Left:  lines({ { n - n * .36f, m }, { n * .34f, n / 2 }, { n - n * .36f, n - m } }, 1.7f); break;
            case Glyph::Check: lines({ { n * .2f, n * .52f }, { n * .42f, n * .74f }, { n * .8f, n * .28f } }, 1.9f); break;
            case Glyph::Partial: lines({ { n * .24f, n / 2 }, { n * .76f, n / 2 } }, 1.9f); break;
            case Glyph::Dot: { SolidBrush b(c); float r = n * .2f; gr.FillEllipse(&b, n / 2 - r, n / 2 - r, 2 * r, 2 * r); break; }
            case Glyph::Cross: { float q = n * .22f; lines({ { q, q }, { n - q, n - q } }, 1.5f); lines({ { q, n - q }, { n - q, q } }, 1.5f); break; }
            case Glyph::Float:
            {
                Pen pen(c, 1.2f * s); pen.SetAlignment(PenAlignmentInset);
                gr.DrawRectangle(&pen, n * .34f, n * .14f, n * .52f, n * .48f);
                gr.DrawRectangle(&pen, n * .14f, n * .38f, n * .48f, n * .48f);
                break;
            }
            }
        }
        Bitmap scaled(size, size, PixelFormat32bppARGB);
        {
            Graphics gr(&scaled);
            gr.Clear(Color(0, 0, 0, 0));
            gr.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            gr.SetPixelOffsetMode(PixelOffsetModeHighQuality);
            gr.DrawImage(&big, 0, 0, size, size);
        }
        return SavePng(scaled, path);
    }
    // Tooltips: Qt word-wraps a tooltip only when the text looks like rich text, so wrap it in <p> with the HTML characters escaped
    std::string Tip(const char* text)
    {
        std::string out = "<p>";
        for (const char* p = text; *p; ++p) { if (*p == '<') out += "&lt;"; else if (*p == '>') out += "&gt;"; else if (*p == '&') out += "&amp;"; else out += *p; }
        return out + "</p>";
    }
    bool DrawThemeImages(const std::wstring& dir, const Palette& p)
    {
        if (!g_gdiplus) { Gdiplus::GdiplusStartupInput in; if (Gdiplus::GdiplusStartup(&g_gdiplus, &in, nullptr) != Gdiplus::Ok) return false; }
        CreateDirectoryW((g_host.themesDir + L"\\images").c_str(), nullptr);
        CreateDirectoryW(dir.c_str(), nullptr);
        const uint32_t dim = p.c[P_DIM], text = p.c[P_TEXT], mark = p.c[P_MARK];
        bool ok = true;
        ok &= DrawGlyph(dir + L"\\arrow-up.png", 10, Glyph::Up, dim);
        ok &= DrawGlyph(dir + L"\\arrow-down.png", 10, Glyph::Down, dim);
        ok &= DrawGlyph(dir + L"\\arrow-left.png", 10, Glyph::Left, dim);
        ok &= DrawGlyph(dir + L"\\arrow-right.png", 10, Glyph::Right, dim);
        ok &= DrawGlyph(dir + L"\\check.png", 13, Glyph::Check, mark);
        ok &= DrawGlyph(dir + L"\\check-dim.png", 13, Glyph::Check, text);
        ok &= DrawGlyph(dir + L"\\partial.png", 13, Glyph::Partial, mark);
        ok &= DrawGlyph(dir + L"\\radio.png", 13, Glyph::Dot, mark);
        ok &= DrawGlyph(dir + L"\\close.png", 10, Glyph::Cross, dim);
        ok &= DrawGlyph(dir + L"\\close-hover.png", 10, Glyph::Cross, text);
        ok &= DrawGlyph(dir + L"\\float.png", 10, Glyph::Float, dim);
        return ok;
    }

    float Hue(uint32_t c);
    // c's hue at HLS lightness l and saturation s, then mixed tint toward accent: shade() in tools/make_themes.py
    uint32_t Shade(uint32_t c, float l, float s, uint32_t accent, float tint)
    {
        const float h = Hue(c) / 360.f;
        auto v = [](float m1, float m2, float hue) {
            hue -= std::floor(hue);
            if (hue < 1.f / 6) return m1 + (m2 - m1) * hue * 6.f;
            if (hue < 0.5f) return m2;
            if (hue < 2.f / 3) return m1 + (m2 - m1) * (2.f / 3 - hue) * 6.f;
            return m1; };
        float m2 = l <= 0.5f ? l * (1.f + s) : l + s - l * s, m1 = 2.f * l - m2;
        uint32_t out = s == 0 ? (uint32_t(std::lround(l * 255.f)) * 0x010101u)
                              : uint32_t(std::lround(Clamp01(v(m1, m2, h + 1.f / 3)) * 255.f)) << 16 |
                                uint32_t(std::lround(Clamp01(v(m1, m2, h)) * 255.f)) << 8 | uint32_t(std::lround(Clamp01(v(m1, m2, h - 1.f / 3)) * 255.f));
        return Mix(out, accent, tint);
    }
    float Hue(uint32_t c)   // 0..360
    {
        float r = Ch(c, 16), g = Ch(c, 8), b = Ch(c, 0), mx = (std::max)({ r, g, b }), mn = (std::min)({ r, g, b }), d = mx - mn;
        if (d <= 0) return 0;
        float h = mx == r ? (g - b) / d : mx == g ? 2.f + (b - r) / d : 4.f + (r - g) / d;
        h *= 60.f; return h < 0 ? h + 360.f : h;
    }
    // the view colors that go with a palette, every row of the Color tab's Grid View, Camera View and Shared tabs (the
    // same rules as view_colors() in tools/make_themes.py)
    std::string ViewColorsText(const std::string& name, const Palette& p)
    {
        const uint32_t* c = p.c;
        const bool dark = Lum(c[P_BG]) < 0.5f;
        std::vector<std::pair<const char*, uint32_t>> v;
        if (dark)
        {
            v = { { "COLOR_GRIDBACK", c[P_BASE] }, { "COLOR_GRIDMINOR", Mix(c[P_BASE], c[P_TEXT], 0.07f) },
                  { "COLOR_GRIDMAJOR", Mix(c[P_BASE], c[P_TEXT], 0.16f) }, { "COLOR_GRIDBLOCK", Mix(c[P_BASE], c[P_ACCENT], 0.55f) },
                  { "COLOR_CAMERABACK", Mix(c[P_BASE], c[P_BG], 0.5f) }, { "COLOR_BRUSHES", Mix(c[P_TEXT], c[P_ACCENT2], 0.45f) },
                  { "COLOR_LIGHT", 0xF2D159 }, { "COLOR_WIREFRAME", Mix(c[P_DIM], c[P_TEXT], 0.25f) }, { "COLOR_PREFAB_WIREFRAME", 0xFF5C5C } };
        }
        else
        {
            v = { { "COLOR_GRIDBACK", c[P_BASE] }, { "COLOR_GRIDMINOR", Mix(c[P_BASE], c[P_TEXT], 0.09f) },
                  { "COLOR_GRIDMAJOR", Mix(c[P_BASE], c[P_TEXT], 0.2f) }, { "COLOR_GRIDBLOCK", Mix(c[P_BASE], c[P_ACCENT], 0.5f) },
                  { "COLOR_CAMERABACK", Mix(c[P_BG], c[P_TEXT], 0.32f) }, { "COLOR_BRUSHES", Mix(c[P_TEXT], c[P_ACCENT], 0.4f) },
                  { "COLOR_LIGHT", 0x000000 }, { "COLOR_WIREFRAME", c[P_DIM] }, { "COLOR_PREFAB_WIREFRAME", 0xD90000 } };
        }
        const uint32_t view = dark ? c[P_ACCENT2] : c[P_ACCENT];
        v.insert(v.end(), { { "COLOR_GRIDTEXT", c[P_DIM] }, { "COLOR_VIEWNAME", view }, { "COLOR_SIZEINFO", view },
                            { "COLOR_XYWND_CAMERA_ICON", view }, { "COLOR_TEXTUREBACK", c[P_PANEL] },
                            { "COLOR_TEXTURE_IMAGEBACK", Mix(c[P_PANEL], c[P_TEXT], 0.12f) },
                            { "COLOR_TEXTURE_IMAGEBACK_INUSE", Mix(c[P_PANEL], c[P_ACCENT], 0.35f) },
                            { "COLOR_LYRMTL_SELECTEDBACK", c[P_SEL] }, { "COLOR_LYRMTL_SELECTEDTEXT", c[P_SELTEXT] } });
        // Grid View: selected brushes in a hue away from the accent, script brush models like script models
        const uint32_t a = c[P_ACCENT];
        const float objL = dark ? 0.68f : 0.36f, tint = 0.12f, hue = Hue(a);
        v.push_back({ "COLOR_SELBRUSHES", Shade(hue >= 70 && hue <= 170 ? 0xFF7A3D : 0x42FFA3, dark ? 0.65f : 0.36f, 0.9f, a, 0) });
        v.push_back({ "COLOR_SCRIPT_BRUSHMODEL", Shade(0x0080CC, objL, 0.8f, a, tint) });
        // Camera View: its grid from the camera background toward the text, the axis in the accent, handles red/green/blue
        const uint32_t cam = dark ? Mix(c[P_BASE], c[P_BG], 0.5f) : Mix(c[P_BG], c[P_TEXT], 0.32f);
        v.insert(v.end(), { { "COLOR_CAMWND_GRID", Mix(cam, c[P_TEXT], 0.3f) }, { "COLOR_CAMWND_GRID_SUBDIVISION", Mix(cam, c[P_TEXT], 0.14f) },
                            { "COLOR_CAMWND_GRID_TEXT", c[P_TEXT] }, { "COLOR_CAMWND_GRID_AXIS", view },
                            { "COLOR_XAXIS_MANIP_HANDLE", Shade(0xFF0000, dark ? 0.62f : 0.42f, 0.9f, a, 0) },
                            { "COLOR_YAXIS_MANIP_HANDLE", Shade(0x00FF00, dark ? 0.62f : 0.42f, 0.9f, a, 0) },
                            { "COLOR_ZAXIS_MANIP_HANDLE", Shade(0x0000FF, dark ? 0.62f : 0.42f, 0.9f, a, 0) } });
        // Shared: each kind of object keeps its usual hue at a lightness that reads on these views, a little toward the
        // accent, selection tints in the accent (their alpha stays Radiant's)
        struct ObjHue { const char* key; uint32_t rgb; float sat; };
        const ObjHue hues[] = { { "COLOR_DETAIL", 0xA8ABFF, 0.45f }, { "COLOR_PORTAL", 0x1A66FF, 0.85f }, { "COLOR_FUNCCULLGROUP", 0x00A0A0, 0.7f },
            { "COLOR_WEAPONCLIP", 0xC9CC1A, 0.75f }, { "COLOR_SCRIPTMODEL", 0x0080CC, 0.8f }, { "COLOR_DYNMODEL", 0xD608D6, 0.75f },
            { "COLOR_NONCOLLIDING", 0xCC9900, 0.85f }, { "COLOR_FROZEN", 0xBF0000, 0.8f }, { "COLOR_TERRAINPATCH", 0x33FF33, 0.7f },
            { "COLOR_CURVEPATCH", 0x33FF33, 0.7f } };
        for (const auto& h : hues) v.push_back({ h.key, Shade(h.rgb, objL, h.sat, a, tint) });
        v.insert(v.end(), { { "COLOR_MODEL", Shade(0xE0E3FF, dark ? 0.8f : 0.3f, 0.35f, a, tint) }, { "COLOR_FUNCGROUP", Mix(c[P_BASE], c[P_TEXT], 0.55f) }, { "COLOR_CLIPPER", view },
                            { "COLOR_SELBRUSHESCAMERA", a }, { "COLOR_FACESELECTION", a }, { "COLOR_SELTEXTURETINT", a } });
        std::string out = "; view colors \"" + name + "\": the views to go with the " + name + " theme (Radiant+ Theme editor)\r\n"
                          "; Radiant view colors (Settings > Preferences > Color > Preset). KEY=r, g, b[, a] in 0..1.\r\n";
        char line[128];
        for (auto& kv : v)
        {
            snprintf(line, sizeof line, "%s=%.3g, %.3g, %.3g\r\n", kv.first, Ch(kv.second, 16), Ch(kv.second, 8), Ch(kv.second, 0));
            out += line;
        }
        return out;
    }
    bool WriteText(const std::wstring& path, const std::string& text)
    {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        DWORD wrote = 0;
        bool ok = WriteFile(f, text.data(), DWORD(text.size()), &wrote, nullptr) && wrote == text.size();
        CloseHandle(f);
        return ok;
    }

    // ---- the editor widget ----
    struct Editor
    {
        void* dialog = nullptr; void* page = nullptr; void* alsoColors = nullptr;
        void* save = nullptr; void* iconSet = nullptr; void* recolor = nullptr;
        void* swatch[9] = {};                // the 8 core colours, then the icon colour
        std::vector<std::string> iconSets;   // "Icons from" combo index -> "" the editor's own, else a folder under icons
        Palette pal;
        bool busy = false;
    };
    Editor g_ed;
    bool g_edited = false;                   // an unsaved edit is the look in effect (until a theme is applied again)
    bool g_colorsEdited = false;             // ... and it changed colours (else only the icons: the theme's own sheet stays)
    Palette g_editedPal;

    bool ThemePalette(const ThemeRef& t, Palette& p)
    {
        std::string sheet;
        return !t.file.empty() && ReadWhole(t.file, sheet) && ParsePalette(sheet, p);
    }
    void ApplyLive();
    // a theme's colours, Treyarch, Classic and sheets without a palette start from Dark (Light for Classic)
    bool LoadTheme(const std::string& name, Palette& p)
    {
        std::vector<ThemeRef> all = g_host.themes();
        for (const ThemeRef& t : all) if (Lower(t.name) == Lower(name) && ThemePalette(t, p)) return true;
        // Treyarch and Classic have no palette: the swatches start from Dark or Light, with Radiant's own icons as they
        // are (Recolor off until the user ticks it, the shipped themes recolor theirs)
        std::string fallback = Lower(name) == "classic" ? "light" : "dark";
        for (const ThemeRef& t : all) if (Lower(t.name) == fallback && ThemePalette(t, p)) { p.recolor = false; p.iconFolder.clear(); return true; }
        return false;
    }
    void ShowSwatches()
    {
        for (int i = 0; i < 9; ++i)
        {
            uint32_t c = i < 8 ? g_ed.pal.c[kCore[i].key] : g_ed.pal.icons;
            QStr css("QPushButton { background-color: " + Hex(c) + "; border: 1px solid " + Hex(Lum(c) < 0.5f ? Mix(c, 0xFFFFFF, 0.35f) : Mix(c, 0, 0.35f)) +
                     "; border-radius: 3px; min-height: 20px; min-width: " + std::string(g_host.columns > 1 ? "120" : "90") + "px; }");
            qt.setStyleSheet(g_ed.swatch[i], &css.d);
        }
        rp::host->ui->SetEnabled(g_ed.swatch[8], g_ed.pal.recolor ? 1 : 0);
    }
    // "Icons from": the editor's own, or any folder under <themes>\icons (but "_" ones: the export, the preview), the
    // "Recolor" box redraws the grey ones of either in the Icons colour
    void FillIconSets()
    {
        const ModToolsUiApi* ui = rp::host->ui;
        g_ed.busy = true;
        ui->ComboClear(g_ed.iconSet);
        g_ed.iconSets = { "" };
        ui->ComboAdd(g_ed.iconSet, "The editor's own icons");
        int current = g_ed.pal.iconFolder.empty() ? 0 : -1;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((g_host.themesDir + L"\\icons\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                std::string n = Narrow(fd.cFileName);
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || n.empty() || n[0] == '.' || n[0] == '_') continue;
                if (Lower(n) == Lower(g_ed.pal.iconFolder)) current = int(g_ed.iconSets.size());
                ui->ComboAdd(g_ed.iconSet, ("Folder: " + n).c_str());
                g_ed.iconSets.push_back(n);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (current < 0) { g_ed.pal.iconFolder.clear(); current = 0; }   // a folder that is gone: the editor's own
        ui->ComboSetCurrent(g_ed.iconSet, current);
        ui->SetChecked(g_ed.recolor, g_ed.pal.recolor ? 1 : 0);
        g_ed.busy = false;
    }
    void OnIconSetPicked(void* dialog, int index)
    {
        if (g_ed.busy || dialog != g_ed.dialog || index < 0 || size_t(index) >= g_ed.iconSets.size()) return;
        g_ed.pal.iconFolder = g_ed.iconSets[size_t(index)];
        ApplyLive();
    }
    void OnRecolorToggled(void*, int on)
    {
        if (g_ed.busy) return;
        g_ed.pal.recolor = on != 0;
        ShowSwatches();
        ApplyLive();
    }
    // the icons folder, with the editors' own icons exported once into icons\_editor_defaults to start from
    void OnIconsFolder(void*)
    {
        std::wstring icons = g_host.themesDir + L"\\icons";
        CreateDirectoryW(icons.c_str(), nullptr);
        if (GetFileAttributesW((icons + L"\\_editor_defaults").c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            SetCursor(LoadCursorW(nullptr, IDC_WAIT));
            rp::Log("theme: %s", themeicons::Command("export", g_host.themesDir).c_str());
        }
        ShellExecuteW(nullptr, L"open", icons.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    // every change shows at once. Only icons changed: just the icon pack, the theme's own sheet stays (Treyarch and Classic have
    // no palette, their sheet would otherwise become Dark or Light). A colour changed: the edited sheet (images and icon pack under "_preview") goes in through the host.
    void ApplyLive()
    {
        if (!g_ed.page) return;
        if (!g_colorsEdited && g_host.previewIcons)
        {
            g_edited = true;
            g_editedPal = g_ed.pal;
            g_host.previewIcons("/* " + IconLines("_preview", g_ed.pal) + " */");
            return;
        }
        std::string sheet;
        if (!BuildSheet("Edited", "_preview", g_ed.pal, "Theme editor, not saved", sheet)) { g_error = "cannot read " + Narrow(TemplatePath()); return; }
        DrawThemeImages(g_host.themesDir + L"\\images\\_preview", g_ed.pal);
        g_edited = true;
        g_editedPal = g_ed.pal;
        g_host.preview(sheet);
    }
    void OnSwatchClicked(void* user)
    {
        int i = int(reinterpret_cast<intptr_t>(user));
        if (i < 0 || i >= 9 || !g_ed.page || !rp::HasUi(&rp::host->ui->PickColor)) return;
        uint32_t out = 0;
        std::string title = std::string("Theme: ") + (i < 8 ? kCore[i].label : "Icons");
        if (!rp::host->ui->PickColor(g_ed.page, 0xFF000000u | (i < 8 ? g_ed.pal.c[kCore[i].key] : g_ed.pal.icons), title.c_str(), &out)) return;
        if (i == 8) g_ed.pal.icons = out & 0xFFFFFF;
        else { g_colorsEdited = true; if (kCore[i].key == P_TEXT && g_ed.pal.icons == g_ed.pal.c[P_TEXT]) g_ed.pal.icons = out & 0xFFFFFF; g_ed.pal.c[kCore[i].key] = out & 0xFFFFFF; }
        Derive(g_ed.pal);
        ShowSwatches();
        ApplyLive();
    }
    std::string ImageKey(const std::string& name)
    {
        std::string k;
        for (char ch : Lower(name)) k += isalnum(static_cast<unsigned char>(ch)) ? ch : '-';
        return "user-" + k;
    }
    void OnSaveTheme(void* dialog)
    {
        if (dialog != g_ed.dialog || !qt.getText) return;
        QStr title("Save Theme"), label("Theme name (saved in bin\\themes; Radiant and APE both list it):"), text(""), result;
        bool ok = false;
        qt.getText(&result.d, dialog, &title.d, &label.d, 0, &text.d, &ok, 0, 0);
        std::string name = SafeFileName(Narrow(ToWide(&result.d)));
        if (!ok || name.empty()) return;
        std::string l = Lower(name);
        bool taken = themeicons::IsClassicTreyarch(l) || l == "treyarch" || l == "classic" || l == "default";
        for (const ThemeRef& t : g_host.themes())
            if (Lower(t.name) == l && !t.file.empty())
            {
                std::string sheet;
                taken |= ReadWhole(t.file, sheet) && sheet.find("Generated by tools/make_themes.py") != std::string::npos;
            }
        if (taken) name += " Custom";                           // a shipped theme would come back over it on the next update
        std::string key = ImageKey(name), sheet;
        if (!BuildSheet(name, key, g_ed.pal, "Made with " + g_host.product + "'s Theme editor (" + g_host.where + "); pick it there to "
                        "change it.", sheet)) { g_error = "cannot read " + Narrow(TemplatePath()); return; }
        DrawThemeImages(g_host.themesDir + L"\\images\\" + Widen(key), g_ed.pal);
        if (!WriteText(g_host.themesDir + L"\\" + Widen(name) + L".qss", sheet)) { g_error = "cannot write the theme"; return; }
        if (g_ed.alsoColors && rp::host->ui->IsChecked(g_ed.alsoColors))
            WriteText(g_host.themesDir + L"\\" + Widen(name) + L".colors", ViewColorsText(name, g_ed.pal));
        rp::Log("theme: '%s' saved by the Theme editor", name.c_str());
        g_edited = g_colorsEdited = false;                      // it is a theme now: the host selects it (OK applies it)
        g_host.saved(name);
    }
    void* BuildEditor(void* parent, void* dialog)
    {
        const ModToolsUiApi* ui = rp::host->ui;
        void* ed = ui->Widget(parent);
        void* v = ui->VBox(ed);
        ui->LayoutMargins(v, 0, 4, 0, 4);
        void* grid = ui->Grid();
        for (int i = 0; i < 9; ++i)
        {
            void* b = ui->PushButton("", ed);
            ui->SetToolTip(b, i < 8 ? "Click to pick the color" : "The color the editors' grey icons are redrawn in (Icon set: Redrawn in the Icons color)");
            const bool two = g_host.columns > 1;
            int row = two ? (i < 8 ? i % 4 : 4) : i, col = two ? (i < 8 ? (i / 4) * 2 : 0) : 0;
            void* label = ui->Label((std::string(i < 8 ? kCore[i].label : "Icons") + ":").c_str(), ed);
            ui->GridAddWidget(grid, label, row, col);
            ui->GridAddWidget(grid, b, row, col + 1);
            if (i == 8 && !g_host.iconRecolor) { ui->SetVisible(label, 0); ui->SetVisible(b, 0); }   // kept (the palette's value), not shown
            g_ed.swatch[i] = b;
            rp::host->OnClicked(rp::self, b, OnSwatchClicked, reinterpret_cast<void*>(intptr_t(i)));
        }
        ui->BoxAddLayout(v, grid, 0);
        void* iconRow = ui->HBox();
        ui->BoxAddWidget(iconRow, ui->Label("Icons from:", ed), 0);
        void* iconSet = ui->ComboBox(ed);
        ui->SetToolTip(iconSet, Tip("The editor's own icons, or your own from bin\\themes\\icons\\<folder> (in the same folders the editors "
                                "use; icons the folder does not have stay the editor's)").c_str());
        ui->BoxAddWidget(iconRow, iconSet, 1);
        void* iconsFolder = ui->PushButton("Icons Folder", ed);
        ui->SetToolTip(iconsFolder, Tip("Open bin\\themes\\icons. The first time, both editors' icons go into _editor_defaults there: copy that "
                                    "folder under a new name, change the icons you want, then pick it under Icons from").c_str());
        ui->BoxAddWidget(iconRow, iconsFolder, 0);
        ui->BoxAddLayout(v, iconRow, 0);
        void* recolor = ui->CheckBox("Recolor in the Icons color", ed);
        ui->SetToolTip(recolor, Tip("Redraw the grey and white icons (the editor's or your folder's) in the Icons color; colored icons keep "
                                "their colors. Your folder is not changed: redrawn sets go to icons\\_generated").c_str());
        ui->BoxAddWidget(v, recolor, 0);
        if (!g_host.iconRecolor) ui->SetVisible(recolor, 0);
        g_ed.recolor = recolor;
        rp::host->OnChanged(rp::self, recolor, OnRecolorToggled, nullptr);
        rp::host->OnClicked(rp::self, iconsFolder, OnIconsFolder, nullptr);
        void* buttons = ui->HBox();
        void* save = ui->PushButton("Save As...", ed);
        ui->SetToolTip(save, Tip("Keep these colors as a theme in bin\\themes (Radiant and APE both list it). Unsaved changes stay until "
                             "a theme is applied again (pick it under Theme and press OK)").c_str());
        ui->BoxAddWidget(buttons, save, 0);
        void* also = nullptr;
        if (g_host.viewColors)
        {
            also = ui->CheckBox("Also save view colors", ed);
            ui->SetChecked(also, 1);
            ui->SetToolTip(also, "Also write <name>.colors, the view colors for this theme (Follow theme picks them)");
            ui->BoxAddWidget(buttons, also, 0);
        }
        ui->BoxAddStretch(buttons, 1);
        ui->BoxAddLayout(v, buttons, 0);
        ui->BoxAddStretch(v, 1);
        if (!qt.getText) ui->SetEnabled(save, 0);

        g_ed.dialog = dialog; g_ed.page = ed; g_ed.alsoColors = also; g_ed.save = save; g_ed.iconSet = iconSet;
        if (g_edited) g_ed.pal = g_editedPal;                   // the unsaved edit in effect
        else LoadTheme(g_host.current(), g_ed.pal);
        FillIconSets();
        ShowSwatches();
        rp::host->OnChanged(rp::self, iconSet, [](void* d, int i) { OnIconSetPicked(d, i); }, dialog);
        rp::host->OnClicked(rp::self, save, OnSaveTheme, dialog);
        return ed;
    }
    void* BuildGuarded(void* parent, void* dialog)
    {
        __try { return BuildEditor(parent, dialog); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
}

void* Build(void* parent, void* dialog, const Host& host)
{
    g_ed = Editor{};
    g_host = host;
    g_error.clear();
    const ModToolsUiApi* ui = rp::host->ui;
    if (!BindQt()) { g_error = "Qt exports missing"; return nullptr; }
    if (!ui || !rp::HasUi(&ui->PickColor) || !rp::HasUi(&ui->ComboClear) || !rp::Has(&rp::host->OnChanged)) { g_error = "the host is too old for the editor"; return nullptr; }
    if (!FileExists(TemplatePath())) { g_error = "no " + Narrow(TemplatePath()); return nullptr; }
    void* ed = BuildGuarded(parent, dialog);
    if (!ed) { g_ed = Editor{}; g_error = "building the editor faulted"; }
    return ed;
}
void Reset() { g_ed = Editor{}; }
bool Edited() { return g_edited; }
void ClearEdited() { g_edited = g_colorsEdited = false; }
std::string Error() { return g_error; }
void ShowTheme(const std::string& name)
{
    if (!g_ed.page || name.empty() || name[0] == '*') return;   // "*edited*": the edit itself
    if (LoadTheme(name, g_ed.pal)) { FillIconSets(); ShowSwatches(); }
}
std::string Command(const std::string& e)
{
    const ModToolsUiApi* ui = rp::host->ui;
    if (!g_ed.page) return "theme editor: not open" + (g_error.empty() ? std::string() : " (" + g_error + ")");
    if (e == "saveas") { ui->ClickButton(g_ed.save); return "clicked " + e; }
    if (e.rfind("show ", 0) == 0) ShowTheme(e.substr(5));
    else if (e.rfind("set ", 0) == 0)                // as a swatch pick: applied at once
    {
        int i = -1; unsigned rgb = 0;
        if (sscanf(e.c_str() + 4, "%d #%x", &i, &rgb) == 2 && i >= 0 && i < 9)
        {
            if (i == 8) g_ed.pal.icons = rgb & 0xFFFFFF; else { g_ed.pal.c[kCore[i].key] = rgb & 0xFFFFFF; g_colorsEdited = true; }
            Derive(g_ed.pal); ShowSwatches(); ApplyLive();
        }
    }
    else if (e.rfind("from ", 0) == 0)              // from <index>: 0 the editor's own, 1+ the folders
    {
        int i = atoi(e.c_str() + 5);
        if (i >= 0 && size_t(i) < g_ed.iconSets.size()) ui->ComboSetCurrent(g_ed.iconSet, i);
    }
    else if (e.rfind("recolor ", 0) == 0) ui->SetChecked(g_ed.recolor, atoi(e.c_str() + 8) ? 1 : 0);   // emits OnChanged
    std::string out;
    for (const CoreColor& c : kCore) out += std::string(out.empty() ? "" : " ") + kPaletteKeys[c.key] + "=" + Hex(g_ed.pal.c[c.key]);
    out += " icons=" + Hex(g_ed.pal.icons) + " from=" + (g_ed.pal.iconFolder.empty() ? "own" : g_ed.pal.iconFolder) + " recolor=" + (g_ed.pal.recolor ? "1" : "0");
    return out + (g_edited ? " (edited, not saved)" : "");
}
}
