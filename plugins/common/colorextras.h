#pragma once
// Color dialog extras shared by RadiantPlus and APEPlus: hex and 0-1 fields, a Kelvin row, Copy buttons, the parser.
// The widgets are built once with no parent and moved into each dialog by the plugin. Main thread only.
#include <string>

namespace colorx
{
// Qt 5.3 exports the extras and their placement use (no headers)
struct Qt
{
    void* (*parent)(const void* obj);
    void* (*layout)(const void* widget);
    int   (*indexOf)(const void* layout, const void* widget);
    void  (*insertWidget)(void* box, int index, void* widget, int stretch, int align);
    void  (*removeWidget)(void* layout, void* widget);
    void  (*setParent)(void* widget, void* parent);
    void  (*setAttribute)(void* widget, int attr, bool on);
    void  (*setCursor)(void* widget, const void* cursor);
    void  (*setAlignment)(void* label, int align);
    void  (*setPixmap)(void* label, const void* pixmap);
    void  (*setMaxLength)(void* edit, int n);
    void  (*addRow)(void* form, void* widget);
    void  (*getWidgetPosition)(const void* form, void* widget, int* row, int* role);
    void  (*setWidget)(void* form, int row, int role, void* widget);
    void  (*gridAddWidget)(void* grid, void* widget, int row, int col, int rowSpan, int colSpan, int align);
    void  (*gridItemPosition)(const void* grid, int index, int* row, int* col, int* rowSpan, int* colSpan);
    void  (*addButton)(void* box, void* button, int role);
    void  (*removeButton)(void* box, void* button);
    void  (*deleteLater)(void* obj);
    int   (*exec)(void* dialog);
    void  (*reject)(void* dialog);
    void* (*cursorCtor)(void* cursor, int shape);
    void  (*cursorDtor)(void* cursor);
    void* (*imageCtor)(void* img, const unsigned char* data, int w, int h, int bpl, int format, void* cleanup, void* info);
    void  (*imageDtor)(void* img);
    void* (*pixmapFromImage)(void* ret, const void* img, int flags);
    void  (*pixmapDtor)(void* pixmap);
    unsigned (*rgba)(const void* color);
    void  (*setRgba)(void* color, unsigned argb);
    void  (*getRgbF)(const void* color, double* r, double* g, double* b, double* a);
    void  (*getHsvF)(const void* color, double* h, double* s, double* v, double* a);
    void  (*setRgbF)(void* color, double r, double g, double b, double a);
};
extern Qt qt;

struct QColorRaw { int spec; unsigned short v[5]; unsigned short pad; };   // spec 0 = invalid

// Resolves the exports and checks the host UI calls. False when anything is missing.
bool Bind();

// The plugin's side: apply a color to the open dialog like its own controls do, read its current color.
using SetFn = void (*)(const double rgb[3], const double* alpha);
using GetFn = bool (*)(double rgba[4]);

struct Pieces { void* hex; void* floats; void* kelvinRow; void* slider; void* copyHex; void* copyFloats; };
extern Pieces pieces;

bool Build(SetFn set, GetFn get);   // once, later calls return true
void UpdateFields();                // shows the current color in the hex and 0-1 fields
void Destroy();                     // deleteLater on every piece

// Placement helpers. Take puts a piece back to no parent (hidden) and clears the layout it sat in.
void Take(void* widget, void*& layout);
void AddCopyButtons(void* buttonBox);       // left of OK/Cancel
void RemoveCopyButtons(void* buttonBox);
void* Metacast(void* obj, const char* cls);
void* LayoutOf(void* widget, const char* cls);   // the widget's layout when it is a cls

// Text and color math
bool Parse(const std::string& text, double out[4], bool& hasAlpha);
void KelvinRgb(int kelvin, double out[3]);
void RgbToHsv(const double rgb[3], double& h, double& s, double& v);   // h < 0 for grey
std::string HexText(unsigned argb);
std::string FloatText(double r, double g, double b);
std::string Num(double v);

// Test entry points for the plugins' commands: type into a field and apply, move the slider
bool TypeHex(const std::string& text);
bool TypeFloats(const std::string& text);
void SetKelvin(int kelvin);
std::string FieldText(bool floats);
}
