// Color dialog extras shared by RadiantPlus and APEPlus.
#include "modtools_plugin.hpp"
#include "colorextras.h"
#include <windows.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace colorx
{
Qt qt = {};
Pieces pieces = {};

namespace
{
    SetFn g_set = nullptr;
    GetFn g_get = nullptr;
    void* g_kLabel = nullptr;

    template <class T> bool Get(T& fn, HMODULE m, const char* name)
    {
        fn = m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
        return fn != nullptr;
    }
    double Clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
    bool HexDigits(const std::string& s)
    {
        if (s.empty()) return false;
        for (char c : s) if (!isxdigit(static_cast<unsigned char>(c))) return false;
        return true;
    }

    std::string EditText(void* edit)
    {
        char buf[256];
        const size_t n = rp::host->ui->EditText(edit, buf, sizeof buf);
        return std::string(buf, n < sizeof buf ? n : sizeof buf - 1);
    }

    bool ApplyText(const std::string& text)
    {
        double c[4];
        bool hasAlpha = false;
        if (!g_set || !Parse(text, c, hasAlpha)) { UpdateFields(); return false; }   // bad text: show the color again
        g_set(c, hasAlpha ? &c[3] : nullptr);
        return true;
    }
    void ApplyKelvin(int kelvin)
    {
        double rgb[3];
        KelvinRgb(kelvin, rgb);
        if (g_set) g_set(rgb, nullptr);
    }
    void SetKelvinLabel(int kelvin)
    {
        char buf[32];
        snprintf(buf, sizeof buf, "%d K", kelvin);
        rp::host->ui->SetLabelText(g_kLabel, buf);
    }

    void OnHex(void*, int) { ApplyText(EditText(pieces.hex)); }
    void OnFloats(void*, int) { ApplyText(EditText(pieces.floats)); }
    void OnSlider(void*, int value)
    {
        SetKelvinLabel(value * 100);
        ApplyKelvin(value * 100);
    }

    void CopyText(const std::string& s)
    {
        if (!OpenClipboard(static_cast<HWND>(rp::host->MainWindow(rp::self)))) return;
        EmptyClipboard();
        if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t)))
        {
            wchar_t* w = static_cast<wchar_t*>(GlobalLock(mem));
            for (size_t i = 0; i <= s.size(); ++i) w[i] = i < s.size() ? wchar_t(static_cast<unsigned char>(s[i])) : 0;
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        }
        CloseClipboard();
    }
    // Qt 5.3 keeps 16-bit channels and its 8-bit getters shift them right by 8: the same here, so hex matches R/G/B
    unsigned Argb(const double c[4])
    {
        auto b = [](double v) { return unsigned(Clamp01(v) * 65535.0 + 0.5) >> 8; };
        return b(c[3]) << 24 | b(c[0]) << 16 | b(c[1]) << 8 | b(c[2]);
    }
    void OnCopyHex(void*) { double c[4]; if (g_get && g_get(c)) CopyText(HexText(Argb(c))); }
    void OnCopyFloats(void*) { double c[4]; if (g_get && g_get(c)) CopyText(FloatText(c[0], c[1], c[2])); }
}

bool Bind()
{
    const ModToolsUiApi* ui = rp::host->ui;
    HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll"), c = GetModuleHandleW(L"Qt5Core64r.dll"), g = GetModuleHandleW(L"Qt5Gui64r.dll");
    const bool ok = Get(qt.parent, c, "?parent@QObject@@QEBAPEAV1@XZ")
        && Get(qt.deleteLater, c, "?deleteLater@QObject@@QEAAXXZ")
        && Get(qt.layout, w, "?layout@QWidget@@QEBAPEAVQLayout@@XZ")
        && Get(qt.indexOf, w, "?indexOf@QLayout@@UEBAHPEAVQWidget@@@Z")
        && Get(qt.insertWidget, w, "?insertWidget@QBoxLayout@@QEAAXHPEAVQWidget@@HV?$QFlags@W4AlignmentFlag@Qt@@@@@Z")
        && Get(qt.removeWidget, w, "?removeWidget@QLayout@@QEAAXPEAVQWidget@@@Z")
        && Get(qt.setParent, w, "?setParent@QWidget@@QEAAXPEAV1@@Z")
        && Get(qt.setAttribute, w, "?setAttribute@QWidget@@QEAAXW4WidgetAttribute@Qt@@_N@Z")
        && Get(qt.setCursor, w, "?setCursor@QWidget@@QEAAXAEBVQCursor@@@Z")
        && Get(qt.setAlignment, w, "?setAlignment@QLabel@@QEAAXV?$QFlags@W4AlignmentFlag@Qt@@@@@Z")
        && Get(qt.setPixmap, w, "?setPixmap@QLabel@@QEAAXAEBVQPixmap@@@Z")
        && Get(qt.setMaxLength, w, "?setMaxLength@QLineEdit@@QEAAXH@Z")
        && Get(qt.addRow, w, "?addRow@QFormLayout@@QEAAXPEAVQWidget@@@Z")
        && Get(qt.getWidgetPosition, w, "?getWidgetPosition@QFormLayout@@QEBAXPEAVQWidget@@PEAHPEAW4ItemRole@1@@Z")
        && Get(qt.setWidget, w, "?setWidget@QFormLayout@@QEAAXHW4ItemRole@1@PEAVQWidget@@@Z")
        && Get(qt.gridAddWidget, w, "?addWidget@QGridLayout@@QEAAXPEAVQWidget@@HHHHV?$QFlags@W4AlignmentFlag@Qt@@@@@Z")
        && Get(qt.gridItemPosition, w, "?getItemPosition@QGridLayout@@QEBAXHPEAH000@Z")
        && Get(qt.addButton, w, "?addButton@QDialogButtonBox@@QEAAXPEAVQAbstractButton@@W4ButtonRole@1@@Z")
        && Get(qt.removeButton, w, "?removeButton@QDialogButtonBox@@QEAAXPEAVQAbstractButton@@@Z")
        && Get(qt.exec, w, "?exec@QDialog@@UEAAHXZ")
        && Get(qt.reject, w, "?reject@QDialog@@UEAAXXZ")
        && Get(qt.cursorCtor, g, "??0QCursor@@QEAA@W4CursorShape@Qt@@@Z")
        && Get(qt.cursorDtor, g, "??1QCursor@@QEAA@XZ")
        && Get(qt.imageCtor, g, "??0QImage@@QEAA@PEBEHHHW4Format@0@P6AXPEAX@Z2@Z")
        && Get(qt.imageDtor, g, "??1QImage@@UEAA@XZ")
        && Get(qt.pixmapFromImage, g, "?fromImage@QPixmap@@SA?AV1@AEBVQImage@@V?$QFlags@W4ImageConversionFlag@Qt@@@@@Z")
        && Get(qt.pixmapDtor, g, "??1QPixmap@@UEAA@XZ")
        && Get(qt.rgba, g, "?rgba@QColor@@QEBAIXZ")
        && Get(qt.setRgba, g, "?setRgba@QColor@@QEAAXI@Z")
        && Get(qt.getRgbF, g, "?getRgbF@QColor@@QEBAXPEAN000@Z")
        && Get(qt.getHsvF, g, "?getHsvF@QColor@@QEBAXPEAN000@Z")
        && Get(qt.setRgbF, g, "?setRgbF@QColor@@QEAAXNNNN@Z");
    return ok && ui && rp::HasUi(&ui->Slider) && rp::HasUi(&ui->SetToolTip) && rp::HasUi(&ui->FindChild)
        && rp::Has(&rp::host->OnChanged) && rp::Has(&rp::host->OnClicked);
}

bool Build(SetFn set, GetFn get)
{
    g_set = set;
    g_get = get;
    if (pieces.hex) return true;
    const ModToolsUiApi* ui = rp::host->ui;
    pieces.hex = ui->LineEdit("", nullptr);
    if (!pieces.hex) return false;
    qt.setMaxLength(pieces.hex, 64);
    ui->SetMaximumWidth(pieces.hex, 80);
    ui->SetToolTip(pieces.hex, "Hex color: #RRGGBB, or #RRGGBBAA with alpha. Also takes rgb(252, 67, 67), 252 67 67 or 0-1 numbers.\n"
                               "Tab applies it, Enter applies it and presses OK.");
    pieces.floats = ui->LineEdit("", nullptr);
    qt.setMaxLength(pieces.floats, 128);
    ui->SetMaximumWidth(pieces.floats, 124);
    ui->SetToolTip(pieces.floats, "Red, green and blue from 0 to 1, the way _color holds them in a .map. A fourth number sets alpha.\n"
                                  "A pasted \"_color\" \"1 0.5 0.2\" line works too. Tab applies it, Enter applies it and presses OK.");

    pieces.kelvinRow = ui->Widget(nullptr);
    void* row = ui->HBox();
    ui->LayoutMargins(row, 0, 0, 0, 0);
    ui->LayoutSpacing(row, 6);
    ui->SetLayout(pieces.kelvinRow, row);
    ui->BoxAddWidget(row, ui->Label("Kelvin:", pieces.kelvinRow), 0);
    pieces.slider = ui->Slider(1, 10, 120, pieces.kelvinRow);
    ui->SliderSetValue(pieces.slider, 65);
    ui->SetToolTip(pieces.slider, "Color temperature: a light color from 1000 K (candle orange) to 12000 K (clear sky blue). Keeps alpha.");
    ui->BoxAddWidget(row, pieces.slider, 1);
    g_kLabel = ui->Label("6500 K", pieces.kelvinRow);
    ui->SetMinimumSize(g_kLabel, 48, 0);
    ui->BoxAddWidget(row, g_kLabel, 0);

    pieces.copyHex = ui->PushButton("Copy Hex", nullptr);
    ui->SetToolTip(pieces.copyHex, "Copy the color as #RRGGBB");
    pieces.copyFloats = ui->PushButton("Copy 0-1", nullptr);
    ui->SetToolTip(pieces.copyFloats, "Copy the color as 0-1 numbers (r g b)");

    rp::host->OnChanged(rp::self, pieces.hex, OnHex, nullptr);
    rp::host->OnChanged(rp::self, pieces.floats, OnFloats, nullptr);
    rp::host->OnChanged(rp::self, pieces.slider, OnSlider, nullptr);
    rp::host->OnClicked(rp::self, pieces.copyHex, OnCopyHex, nullptr);
    rp::host->OnClicked(rp::self, pieces.copyFloats, OnCopyFloats, nullptr);
    return true;
}

void UpdateFields()
{
    double c[4];
    if (!pieces.hex || !g_get || !g_get(c)) return;
    rp::host->ui->SetEditText(pieces.hex, HexText(Argb(c)).c_str());
    rp::host->ui->SetEditText(pieces.floats, FloatText(c[0], c[1], c[2]).c_str());
}

void Destroy()
{
    if (qt.deleteLater)
        for (void* w : { pieces.hex, pieces.floats, pieces.kelvinRow, pieces.copyHex, pieces.copyFloats }) if (w) qt.deleteLater(w);
    pieces = {};
    g_kLabel = nullptr;
    g_set = nullptr;
    g_get = nullptr;
}

void Take(void* widget, void*& layout)
{
    if (widget && layout) { qt.removeWidget(layout, widget); qt.setParent(widget, nullptr); }   // hidden top level until the next dialog
    layout = nullptr;
}

void AddCopyButtons(void* box)
{
    qt.addButton(box, pieces.copyHex, 7 /* ResetRole: left of the stretch, no accept or reject */);
    qt.addButton(box, pieces.copyFloats, 7);
}

void RemoveCopyButtons(void* box)
{
    if (!box) return;
    qt.removeButton(box, pieces.copyHex);     // also clears the parent
    qt.removeButton(box, pieces.copyFloats);
}

void* Metacast(void* obj, const char* cls)
{
    using MetacastFn = void* (*)(void* obj, const char* cls);   // QObject vtable slot 1
    return obj ? (*reinterpret_cast<MetacastFn**>(obj))[1](obj, cls) : nullptr;
}

void* LayoutOf(void* widget, const char* cls)
{
    void* l = widget ? qt.layout(widget) : nullptr;
    return l && Metacast(l, cls) ? l : nullptr;
}

// #RGB, #RRGGBB, #RRGGBBAA (also without '#' when 6 or 8 digits), rgb()/rgba(), a pasted "_color" "r g b" line,
// 3 or 4 numbers: 0-1 when all are at most 1, else 0-255
bool Parse(const std::string& text, double out[4], bool& hasAlpha)
{
    std::string s;
    for (char c : text) s += char(tolower(static_cast<unsigned char>(c)));
    for (const char* word : { "_color", "rgba", "rgb" })
        for (size_t p; (p = s.find(word)) != std::string::npos;) s.replace(p, strlen(word), " ");
    for (char& c : s) if (c == ',' || c == ';' || c == '(' || c == ')' || c == '"' || c == '\'' || c == '\t') c = ' ';
    std::vector<std::string> tok;
    for (size_t i = 0; i < s.size();)
    {
        while (i < s.size() && s[i] == ' ') ++i;
        const size_t j = s.find(' ', i);
        if (i < s.size()) tok.push_back(s.substr(i, j == std::string::npos ? std::string::npos : j - i));
        i = j == std::string::npos ? s.size() : j;
    }
    hasAlpha = false;
    if (tok.size() == 1)
    {
        std::string h = tok[0];
        bool marked = false;
        if (!h.empty() && h[0] == '#') { h.erase(0, 1); marked = true; }
        else if (h.compare(0, 2, "0x") == 0) { h.erase(0, 2); marked = true; }
        const bool letters = h.find_first_of("abcdef") != std::string::npos;
        if (!HexDigits(h) || !(h.size() == 3 || h.size() == 6 || h.size() == 8)) return false;
        if (h.size() == 3 && !marked) return false;
        if (!marked && !letters && h.size() != 6 && h.size() != 8) return false;
        if (h.size() == 3) h = std::string() + h[0] + h[0] + h[1] + h[1] + h[2] + h[2];
        const unsigned long v = strtoul(h.c_str(), nullptr, 16);
        const int shift = h.size() == 8 ? 8 : 0;
        out[0] = ((v >> (16 + shift)) & 255) / 255.0;
        out[1] = ((v >> (8 + shift)) & 255) / 255.0;
        out[2] = ((v >> shift) & 255) / 255.0;
        out[3] = h.size() == 8 ? (v & 255) / 255.0 : 1.0;
        hasAlpha = h.size() == 8;
        return true;
    }
    if (tok.size() != 3 && tok.size() != 4) return false;
    double v[4] = { 0, 0, 0, 1 };
    bool unit = true;
    for (size_t i = 0; i < tok.size(); ++i)
    {
        char* end = nullptr;
        v[i] = strtod(tok[i].c_str(), &end);
        if (!end || *end || !std::isfinite(v[i]) || v[i] < 0) return false;
        unit &= v[i] <= 1.0;
    }
    const double scale = unit ? 1.0 : 255.0;
    for (int i = 0; i < 4; ++i) out[i] = Clamp01(v[i] / (i < int(tok.size()) ? scale : 1.0));
    hasAlpha = tok.size() == 4;
    return true;
}

// a common curve fit of blackbody color, 1000 K to 40000 K
void KelvinRgb(int kelvin, double out[3])
{
    const double t = kelvin / 100.0;
    double r = t <= 66 ? 255 : 329.698727446 * pow(t - 60, -0.1332047592);
    double g = t <= 66 ? 99.4708025861 * log(t) - 161.1195681661 : 288.1221695283 * pow(t - 60, -0.0755148492);
    double b = t >= 66 ? 255 : t <= 19 ? 0 : 138.5177312231 * log(t - 10) - 305.0447927307;
    out[0] = Clamp01(r / 255.0);
    out[1] = Clamp01(g / 255.0);
    out[2] = Clamp01(b / 255.0);
}

void RgbToHsv(const double rgb[3], double& h, double& s, double& v)
{
    QColorRaw c = {};
    double a = 0;
    qt.setRgbF(&c, Clamp01(rgb[0]), Clamp01(rgb[1]), Clamp01(rgb[2]), 1.0);
    qt.getHsvF(&c, &h, &s, &v, &a);
}

std::string HexText(unsigned argb)
{
    char buf[16];
    snprintf(buf, sizeof buf, "#%02X%02X%02X", (argb >> 16) & 255, (argb >> 8) & 255, argb & 255);
    return buf;
}

std::string Num(double v)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%.3f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s.empty() || s == "-0" ? "0" : s;
}

std::string FloatText(double r, double g, double b) { return Num(r) + " " + Num(g) + " " + Num(b); }

bool TypeHex(const std::string& text)
{
    if (!pieces.hex) return false;
    rp::host->ui->SetEditText(pieces.hex, text.c_str());
    return ApplyText(text);
}

bool TypeFloats(const std::string& text)
{
    if (!pieces.floats) return false;
    rp::host->ui->SetEditText(pieces.floats, text.c_str());
    return ApplyText(text);
}

void SetKelvin(int kelvin) { if (pieces.slider) rp::host->ui->SliderSetValue(pieces.slider, kelvin / 100); }
std::string FieldText(bool floats) { void* e = floats ? pieces.floats : pieces.hex; return e ? EditText(e) : std::string(); }
}
