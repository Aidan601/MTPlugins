// The launcher's "Publish Mod" dialog extended in place: Map Description apart from the Workshop Description (Steam tags, preview),
// thumbnail and language checks.
// MAP: Qt binding .. helpers (strings, files, json) .. images (worker, WIC) .. BbToHtml .. CheckThumbnail .. CheckLanguages .. Steam hook .. dialog (Enhance,
//      Update, Forget) .. Setup/Tick/Shutdown/Command
#include "modtools_plugin.hpp"
#include "publish.h"
#define LOCALIZED_DECIDE_ONLY
#include "linker/localized.h"   // skip_localized's decision: is a kept localized fastfile still the one a relink would make
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wininet.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace publish
{
namespace
{
    // ---- Qt binding (exports only) ----
    struct QArrayDataRaw { int ref; int size; unsigned alloc; ptrdiff_t offset; };
    struct Qt
    {
        bool bound = false;
        void (*qsDtor)(void**) = nullptr;
        void* (*qsFromUtf8)(void*, const char*, int) = nullptr;
        void* (*parent)(const void*) = nullptr;
        void* (*activeModal)() = nullptr;
        uintptr_t (*winId)(const void*) = nullptr;
        void* (*layout)(const void*) = nullptr;
        void (*resize)(void*, int, int) = nullptr;
        void (*setStyleSheet)(void*, const void*) = nullptr;
        void (*boxInsertWidget)(void*, int, void*, int, int) = nullptr;
        void** (*plainText)(const void*, void**) = nullptr;
        void (*plainSetText)(void*, const void*) = nullptr;
        void* (*plainCursor)(const void*, void*) = nullptr;            // QTextCursor by hidden pointer
        void (*plainSetCursor)(void*, const void*) = nullptr;
        void** (*labelText)(const void*, void**) = nullptr;
        void (*labelSetFormat)(void*, int) = nullptr;
        void (*labelSetAlignment)(void*, int) = nullptr;
        void (*labelSetInteraction)(void*, int) = nullptr;
        void* (*boxButton)(const void*, int) = nullptr;
        void (*dialogReject)(void*) = nullptr;
        void* getOpenFileName = nullptr;
        void* dialogAccept = nullptr;
        void (*cursorDtor)(void*) = nullptr;
        void** (*cursorSelectedText)(const void*, void**) = nullptr;
        void (*cursorInsertText)(void*, const void*) = nullptr;
        bool (*cursorMove)(void*, int, int, int) = nullptr;
        int* (*frameGeometry)(const void*, int*) = nullptr;            // optional: QRect by hidden pointer
    } qt;
    template <class T> bool Get(T& fn, HMODULE m, const char* name) { fn = reinterpret_cast<T>(GetProcAddress(m, name)); return fn != nullptr; }
    bool BindQt()
    {
        HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll"), c = GetModuleHandleW(L"Qt5Core64r.dll"), g = GetModuleHandleW(L"Qt5Gui64r.dll");
        if (!w || !c || !g) return false;
        bool ok = true;
        ok &= Get(qt.qsDtor, c, "??1QString@@QEAA@XZ");
        ok &= Get(qt.qsFromUtf8, c, "?fromUtf8@QString@@SA?AV1@PEBDH@Z");
        ok &= Get(qt.parent, c, "?parent@QObject@@QEBAPEAV1@XZ");
        ok &= Get(qt.activeModal, w, "?activeModalWidget@QApplication@@SAPEAVQWidget@@XZ");
        ok &= Get(qt.winId, w, "?winId@QWidget@@QEBA_KXZ");
        ok &= Get(qt.layout, w, "?layout@QWidget@@QEBAPEAVQLayout@@XZ");
        ok &= Get(qt.resize, w, "?resize@QWidget@@QEAAXHH@Z");
        ok &= Get(qt.setStyleSheet, w, "?setStyleSheet@QWidget@@QEAAXAEBVQString@@@Z");
        ok &= Get(qt.boxInsertWidget, w, "?insertWidget@QBoxLayout@@QEAAXHPEAVQWidget@@HV?$QFlags@W4AlignmentFlag@Qt@@@@@Z");
        ok &= Get(qt.plainText, w, "?toPlainText@QPlainTextEdit@@QEBA?AVQString@@XZ");
        ok &= Get(qt.plainSetText, w, "?setPlainText@QPlainTextEdit@@QEAAXAEBVQString@@@Z");
        ok &= Get(qt.plainCursor, w, "?textCursor@QPlainTextEdit@@QEBA?AVQTextCursor@@XZ");
        ok &= Get(qt.plainSetCursor, w, "?setTextCursor@QPlainTextEdit@@QEAAXAEBVQTextCursor@@@Z");
        ok &= Get(qt.labelText, w, "?text@QLabel@@QEBA?AVQString@@XZ");
        ok &= Get(qt.labelSetFormat, w, "?setTextFormat@QLabel@@QEAAXW4TextFormat@Qt@@@Z");
        ok &= Get(qt.labelSetAlignment, w, "?setAlignment@QLabel@@QEAAXV?$QFlags@W4AlignmentFlag@Qt@@@@@Z");
        ok &= Get(qt.labelSetInteraction, w, "?setTextInteractionFlags@QLabel@@QEAAXV?$QFlags@W4TextInteractionFlag@Qt@@@@@Z");
        ok &= Get(qt.boxButton, w, "?button@QDialogButtonBox@@QEBAPEAVQPushButton@@W4StandardButton@1@@Z");
        ok &= Get(qt.dialogReject, w, "?reject@QDialog@@UEAAXXZ");
        ok &= Get(qt.dialogAccept, w, "?accept@QDialog@@UEAAXXZ");
        ok &= Get(qt.getOpenFileName, w, "?getOpenFileName@QFileDialog@@SA?AVQString@@PEAVQWidget@@AEBV2@11PEAV2@V?$QFlags@W4Option@QFileDialog@@@@@Z");
        ok &= Get(qt.cursorDtor, g, "??1QTextCursor@@QEAA@XZ");
        ok &= Get(qt.cursorSelectedText, g, "?selectedText@QTextCursor@@QEBA?AVQString@@XZ");
        ok &= Get(qt.cursorInsertText, g, "?insertText@QTextCursor@@QEAAXAEBVQString@@@Z");
        ok &= Get(qt.cursorMove, g, "?movePosition@QTextCursor@@QEAA_NW4MoveOperation@1@W4MoveMode@1@H@Z");
        Get(qt.frameGeometry, w, "?frameGeometry@QWidget@@QEBA?AVQRect@@XZ");
        return qt.bound = ok;
    }
    const ModToolsUiApi* Ui() { return rp::host->ui; }

    // ---- helpers ----
    std::wstring Widen(const std::string& s)
    {
        if (s.empty()) return L"";
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
        std::wstring w(size_t(n), L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), &w[0], n); return w;
    }
    std::string Narrow(const wchar_t* p, size_t len)
    {
        if (!len) return "";
        int n = WideCharToMultiByte(CP_UTF8, 0, p, int(len), nullptr, 0, nullptr, nullptr);
        std::string s(size_t(n), '\0'); WideCharToMultiByte(CP_UTF8, 0, p, int(len), &s[0], n, nullptr, nullptr); return s;
    }
    std::string Narrow(const std::wstring& w) { return Narrow(w.c_str(), w.size()); }
    // the UTF-8 of a QString (its d pointer); U+2028/U+2029 (Qt's line and paragraph separators) become '\n'
    std::string FromQ(const void* d)
    {
        auto* a = static_cast<const QArrayDataRaw*>(d);
        if (!a || a->size <= 0) return "";
        std::wstring w(reinterpret_cast<const wchar_t*>(reinterpret_cast<const char*>(a) + a->offset), size_t(a->size));
        for (wchar_t& ch : w) if (ch == 0x2028 || ch == 0x2029) ch = L'\n';
        return Narrow(w);
    }
    struct QStr
    {
        void* d = nullptr;
        explicit QStr(const std::string& s) { qt.qsFromUtf8(&d, s.c_str(), int(s.size())); }
        ~QStr() { qt.qsDtor(&d); }
    };
    std::string Unix(std::string s)   // "\r\n" and "\r" -> "\n"
    {
        std::string out; out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '\r') { out += '\n'; if (i + 1 < s.size() && s[i + 1] == '\n') ++i; }
            else out += s[i];
        }
        return out;
    }
    std::string Trimmed(std::string s) { while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\t')) s.pop_back(); return s; }
    std::string PlainText(void* edit) { void* q = nullptr; qt.plainText(edit, &q); std::string s = FromQ(q); qt.qsDtor(&q); return s; }
    void PlainSet(void* edit, const std::string& s) { QStr q(s); qt.plainSetText(edit, &q.d); }
    std::string LabelText(void* label) { void* q = nullptr; qt.labelText(label, &q); std::string s = FromQ(q); qt.qsDtor(&q); return s; }
    std::string LineText(void* edit)
    {
        size_t n = Ui()->EditText(edit, nullptr, 0);
        std::string s(n + 1, '\0'); Ui()->EditText(edit, &s[0], s.size()); s.resize(n);
        return s;
    }
    bool ReadWhole(const std::wstring& path, std::string& out, long long cap = 4 << 20)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size = {}; GetFileSizeEx(h, &size);
        const DWORD want = DWORD(std::min<long long>(size.QuadPart, cap));
        out.assign(want, '\0');
        DWORD got = 0; const bool ok = want == 0 || (::ReadFile(h, &out[0], want, &got, nullptr) && got == want);
        CloseHandle(h);
        return ok;
    }
    bool WriteWhole(const std::wstring& path, const std::string& data)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD put = 0; const bool ok = ::WriteFile(h, data.data(), DWORD(data.size()), &put, nullptr) && put == data.size();
        CloseHandle(h);
        return ok;
    }
    void AppendUtf8(std::string& out, unsigned cp)
    {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
        else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    }
    // a top-level string value of workshop.json (flat object of strings, written by QJsonDocument)
    bool JsonString(const std::string& json, const char* key, std::string& out)
    {
        const std::string needle = std::string("\"") + key + "\"";
        size_t p = json.find(needle);
        if (p == std::string::npos) return false;
        p = json.find(':', p + needle.size());
        if (p == std::string::npos) return false;
        p = json.find_first_not_of(" \t\r\n", p + 1);
        if (p == std::string::npos || json[p] != '"') return false;
        out.clear();
        for (++p; p < json.size() && json[p] != '"'; ++p)
        {
            if (json[p] != '\\' || p + 1 >= json.size()) { out += json[p]; continue; }
            const char e = json[++p];
            if (e == 'n') out += '\n'; else if (e == 't') out += '\t'; else if (e == 'r') out += '\r';
            else if (e == 'b') out += '\b'; else if (e == 'f') out += '\f';
            else if (e == 'u' && p + 4 < json.size())
            {
                unsigned cp = unsigned(strtoul(json.substr(p + 1, 4).c_str(), nullptr, 16)); p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && p + 6 < json.size() && json[p + 1] == '\\' && json[p + 2] == 'u')
                {
                    const unsigned lo = unsigned(strtoul(json.substr(p + 3, 4).c_str(), nullptr, 16)); p += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                AppendUtf8(out, cp);
            }
            else out += e;
        }
        return true;
    }

    // ---- Steam's formatting tags -> Qt rich text (the preview) ----
    // Tags: h1 h2 h3 b u i strike spoiler noparse hr url list olist [*] quote code table tr th td (+ img as a note).
    // A tag without its closing tag stays literal, as on Steam.
    std::string Esc(const std::string& s, bool breaks = true)
    {
        std::string o; o.reserve(s.size() + 16);
        for (char ch : s)
        {
            if (ch == '&') o += "&amp;"; else if (ch == '<') o += "&lt;"; else if (ch == '>') o += "&gt;";
            else if (ch == '"') o += "&quot;"; else if (ch == '\n' && breaks) o += "<br>";
            else o += ch;
        }
        return o;
    }

    // ---- [img] pictures in the preview ----
    // Fetched on one worker thread (WinINet) and converted to a PNG in %TEMP%\LauncherPlusPreview with WIC (the launcher's Qt
    // has no JPG or GIF reader), at most 1000 px wide. A URL is fetched once it stood in the text for 0.7 s.
    struct Img { int state = 0; ULONGLONG seen = 0; std::string file; unsigned w = 0, h = 0; };   // 0 new, 1 loading, 2 ready, 3 failed
    std::mutex g_imgLock;                    // g_imgs, g_imgQueue
    std::map<std::string, Img> g_imgs;
    std::vector<std::string> g_imgQueue;
    std::vector<std::string> g_imgShown;     // main thread: the URLs of the last render
    std::atomic<unsigned> g_imgVersion{ 0 }; // bumped when a picture arrives or fails
    std::atomic<bool> g_imgStop{ false };
    HANDLE g_imgWake = nullptr, g_imgThread = nullptr;
    HINTERNET g_net = nullptr;
    int g_imgMaxWidth = 400;
    template <class T> void Drop(T*& p) { if (p) { p->Release(); p = nullptr; } }
    bool Fetch(const std::string& url, std::string& data)
    {
        HINTERNET h = g_net ? InternetOpenUrlW(g_net, Widen(url).c_str(), L"Accept: image/png,image/jpeg,image/gif,image/*;q=0.8\r\n", DWORD(-1),
                                               INTERNET_FLAG_NO_UI | INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_AUTH, 0) : nullptr;
        if (!h) return false;
        char buf[16384]; DWORD got = 0; bool ok = true;
        while (!g_imgStop && (ok = InternetReadFile(h, buf, sizeof buf, &got) != 0) && got)
        {
            data.append(buf, got);
            if (data.size() > (24u << 20)) { ok = false; break; }
        }
        InternetCloseHandle(h);
        return ok && !g_imgStop && !data.empty();
    }
    bool ToPng(const std::string& data, const std::wstring& out, unsigned& w, unsigned& h)
    {
        IWICImagingFactory* f = nullptr; IWICStream* in = nullptr; IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* frame = nullptr;
        IWICBitmapScaler* scaler = nullptr; IWICFormatConverter* conv = nullptr; IWICStream* os = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fe = nullptr;
        bool ok = false;
        do
        {
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) break;
            if (FAILED(f->CreateStream(&in)) || FAILED(in->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(data.data())), DWORD(data.size())))) break;
            if (FAILED(f->CreateDecoderFromStream(in, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) || FAILED(dec->GetFrame(0, &frame))) break;
            if (FAILED(frame->GetSize(&w, &h)) || !w || !h) break;
            IWICBitmapSource* src = frame;
            const unsigned kMax = 1000;
            if (w > kMax)
            {
                const unsigned nh = std::max(1u, unsigned(static_cast<unsigned long long>(h) * kMax / w));
                if (FAILED(f->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(frame, kMax, nh, WICBitmapInterpolationModeFant))) break;
                src = scaler; w = kMax; h = nh;
            }
            if (FAILED(f->CreateFormatConverter(&conv)) || FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) break;
            if (FAILED(f->CreateStream(&os)) || FAILED(os->InitializeFromFilename(out.c_str(), GENERIC_WRITE))) break;
            if (FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(os, WICBitmapEncoderNoCache))) break;
            if (FAILED(enc->CreateNewFrame(&fe, nullptr)) || FAILED(fe->Initialize(nullptr)) || FAILED(fe->SetSize(w, h))) break;
            WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
            if (FAILED(fe->SetPixelFormat(&pf)) || FAILED(fe->WriteSource(conv, nullptr)) || FAILED(fe->Commit()) || FAILED(enc->Commit())) break;
            ok = true;
        } while (false);
        Drop(fe); Drop(enc); Drop(os); Drop(conv); Drop(scaler); Drop(frame); Drop(dec); Drop(in); Drop(f);
        return ok;
    }
    DWORD WINAPI ImgWorker(void*)
    {
        try
        {
            const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
            wchar_t tmp[MAX_PATH] = {}; GetTempPathW(MAX_PATH, tmp);
            const std::wstring dir = std::wstring(tmp) + L"LauncherPlusPreview";
            CreateDirectoryW(dir.c_str(), nullptr);
            while (!g_imgStop)
            {
                std::string url;
                { std::lock_guard<std::mutex> l(g_imgLock); if (!g_imgQueue.empty()) { url = g_imgQueue.back(); g_imgQueue.pop_back(); } }
                if (url.empty()) { WaitForSingleObject(g_imgWake, INFINITE); continue; }
                unsigned long long hash = 1469598103934665603ull;
                for (char ch : url) hash = (hash ^ static_cast<unsigned char>(ch)) * 1099511628211ull;
                wchar_t name[40]; swprintf_s(name, L"\\%016llx.png", hash);
                const std::wstring file = dir + name;
                std::string data; unsigned w = 0, h = 0;
                const bool ok = Fetch(url, data) && ToPng(data, file, w, h);
                {
                    std::lock_guard<std::mutex> l(g_imgLock);
                    Img& i = g_imgs[url];
                    i.state = ok ? 2 : 3; i.w = w; i.h = h; i.file = Narrow(file);
                    for (char& ch : i.file) if (ch == '\\') ch = '/';
                }
                ++g_imgVersion;
            }
            if (com) CoUninitialize();
        }
        catch (...) {}
        return 0;
    }
    void QueueImages()   // main thread, every tick while the dialog is open
    {
        const ULONGLONG now = GetTickCount64();
        bool wake = false;
        {
            std::lock_guard<std::mutex> l(g_imgLock);
            for (const std::string& url : g_imgShown)
            {
                Img& i = g_imgs[url];
                if (i.state == 0 && now - i.seen > 700) { i.state = 1; g_imgQueue.push_back(url); wake = true; }
            }
        }
        if (!wake) return;
        if (!g_imgThread)
        {
            g_imgStop = false;
            if (!g_imgWake) g_imgWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            g_net = InternetOpenW(L"LauncherPlus", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
            DWORD ms = 10000;
            if (g_net) { InternetSetOptionW(g_net, INTERNET_OPTION_CONNECT_TIMEOUT, &ms, sizeof ms); InternetSetOptionW(g_net, INTERNET_OPTION_RECEIVE_TIMEOUT, &ms, sizeof ms); }
            g_imgThread = CreateThread(nullptr, 0, ImgWorker, nullptr, 0, nullptr);
        }
        SetEvent(g_imgWake);
    }
    void StopImages()    // the worker ends before the DLL goes, closing the session aborts a transfer
    {
        g_imgStop = true;
        if (g_net) { InternetCloseHandle(g_net); g_net = nullptr; }
        if (g_imgWake) SetEvent(g_imgWake);
        if (g_imgThread) { WaitForSingleObject(g_imgThread, INFINITE); CloseHandle(g_imgThread); g_imgThread = nullptr; }
        if (g_imgWake) { CloseHandle(g_imgWake); g_imgWake = nullptr; }
        std::lock_guard<std::mutex> l(g_imgLock);
        g_imgQueue.clear();
        for (auto it = g_imgs.begin(); it != g_imgs.end(); ) if (it->second.state != 2) it = g_imgs.erase(it); else ++it;
    }
    std::string ImageHtml(const std::string& raw)
    {
        std::string url = raw;
        while (!url.empty() && isspace(static_cast<unsigned char>(url.back()))) url.pop_back();
        while (!url.empty() && isspace(static_cast<unsigned char>(url.front()))) url.erase(0, 1);
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return "<i>[image: " + Esc(url, false) + "]</i>";
        g_imgShown.push_back(url);
        std::lock_guard<std::mutex> l(g_imgLock);
        Img& i = g_imgs[url];
        if (i.state == 0 && !i.seen) i.seen = GetTickCount64();
        if (i.state == 3) return "<i>[image not available: " + Esc(url, false) + "]</i>";
        if (i.state != 2) return "<i>[loading image...]</i>";
        const unsigned w = std::min(i.w, unsigned(std::max(g_imgMaxWidth, 60))), h = std::max(1u, unsigned(static_cast<unsigned long long>(i.h) * w / i.w));
        return "<img src=\"" + Esc(i.file, false) + "\" width=\"" + std::to_string(w) + "\" height=\"" + std::to_string(h) + "\">";
    }
    std::string BbToHtml(const std::string& in, bool images)
    {
        if (images) g_imgShown.clear();
        std::string low = in;
        for (char& ch : low) ch = char(tolower(static_cast<unsigned char>(ch)));
        struct Open { std::string name; bool item = false; };   // item: a <li> is open (lists)
        std::vector<Open> stack;
        std::string out; out.reserve(in.size() * 2 + 64);
        bool swallow = false;                                    // drop the newline right after a block tag
        auto inStack = [&](const char* n) { for (const Open& o : stack) if (o.name == n) return true; return false; };
        auto structural = [&] { return !stack.empty() && (stack.back().name == "table" || stack.back().name == "tr" || ((stack.back().name == "list" || stack.back().name == "olist") && !stack.back().item)); };
        auto trimBreak = [&] { if (out.size() >= 4 && out.compare(out.size() - 4, 4, "<br>") == 0) out.resize(out.size() - 4); };
        auto text = [&](const std::string& t) {
            std::string s = t;
            if (swallow && !s.empty() && s[0] == '\n') s.erase(0, 1);
            swallow = false;
            if (structural() && s.find_first_not_of(" \t\n") == std::string::npos) return;   // layout whitespace between cells and items
            out += Esc(s);
        };
        size_t i = 0;
        while (i < in.size())
        {
            const size_t lb = in.find('[', i);
            if (lb == std::string::npos) { text(in.substr(i)); break; }
            if (lb > i) text(in.substr(i, lb - i));
            const size_t rb = in.find(']', lb);
            const std::string body = rb == std::string::npos || rb - lb > 300 ? "" : low.substr(lb + 1, rb - lb - 1);
            const bool closing = !body.empty() && body[0] == '/';
            size_t nameEnd = closing ? 1 : 0;
            while (nameEnd < body.size() && (isalnum(static_cast<unsigned char>(body[nameEnd])) || body[nameEnd] == '*')) ++nameEnd;
            const std::string name = body.substr(closing ? 1 : 0, nameEnd - (closing ? 1 : 0));
            const std::string arg = body.size() > nameEnd && body[nameEnd] == '=' ? in.substr(lb + 1 + nameEnd + 1, rb - lb - nameEnd - 2) : "";
            const std::string closer = "[/" + name + "]";
            const size_t closeAt = rb == std::string::npos ? std::string::npos : low.find(closer, rb + 1);
            auto literal = [&] { text("["); i = lb + 1; };
            if (body.empty() || name.empty()) { literal(); continue; }
            static const char* kKnown[] = { "h1", "h2", "h3", "b", "u", "i", "strike", "spoiler", "noparse", "hr", "url", "list", "olist", "*", "quote", "code", "table", "tr", "th", "td", "img" };
            bool known = false;
            for (const char* k : kKnown) if (name == k) known = true;
            if (!known) { literal(); continue; }
            i = rb + 1;
            if (closing)
            {
                if (name == "hr") continue;                      // "[hr][/hr]": the rule was drawn at [hr]
                if (stack.empty() || stack.back().name != name) { i = lb; literal(); continue; }
                const Open top = stack.back(); stack.pop_back();
                if (name == "h1" || name == "h2" || name == "h3") { out += "</div>"; swallow = true; }
                else if (name == "b" || name == "u" || name == "i") out += "</" + name + ">";
                else if (name == "strike") out += "</s>";
                else if (name == "spoiler") out += "</span>";
                else if (name == "url") out += "</a>";
                else if (name == "list" || name == "olist") { trimBreak(); if (top.item) out += "</li>"; out += name == "list" ? "</ul>" : "</ol>"; swallow = true; }
                else if (name == "quote") { trimBreak(); out += "</td></tr></table>"; swallow = true; }
                else if (name == "table") { out += "</table>"; swallow = true; }
                else if (name == "tr") out += "</tr>";
                else if (name == "th") { trimBreak(); out += "</th>"; }
                else if (name == "td") { trimBreak(); out += "</td>"; }
                continue;
            }
            if (name == "hr") { trimBreak(); out += "<hr>"; swallow = true; continue; }
            if (name == "*")
            {
                if (stack.empty() || (stack.back().name != "list" && stack.back().name != "olist")) { i = lb; literal(); continue; }
                trimBreak();
                if (stack.back().item) out += "</li>";
                out += "<li>"; stack.back().item = true;
                continue;
            }
            if (closeAt == std::string::npos) { i = lb; literal(); continue; }
            if (name == "noparse" || name == "code" || name == "img")
            {
                std::string inner = in.substr(rb + 1, closeAt - rb - 1);
                if (name == "noparse") { swallow = false; out += Esc(inner); }
                else if (name == "img") out += images ? ImageHtml(inner) : "<i>[image: " + Esc(inner, false) + "]</i>";
                else
                {
                    if (!inner.empty() && inner[0] == '\n') inner.erase(0, 1);
                    trimBreak();
                    out += "<table width=\"100%\" cellpadding=\"6\" bgcolor=\"#131a25\"><tr><td><pre style=\"font-family:Consolas,'Courier New',monospace\">" + Esc(inner, false) + "</pre></td></tr></table>";
                    swallow = true;
                }
                i = closeAt + closer.size();
                continue;
            }
            if ((name == "tr" && !inStack("table")) || ((name == "th" || name == "td") && !inStack("tr"))) { i = lb; literal(); continue; }
            stack.push_back({ name, false });
            if (name == "h1" || name == "h2" || name == "h3")
            {
                trimBreak();
                out += std::string("<div style=\"color:#5aa9d6; font-size:") + (name == "h1" ? "20px" : name == "h2" ? "17px" : "15px") + "; margin-top:4px; margin-bottom:4px\">";
            }
            else if (name == "b" || name == "u" || name == "i") out += "<" + name + ">";
            else if (name == "strike") out += "<s>";
            else if (name == "spoiler") out += "<span style=\"background-color:#000000; color:#3d3d3d\">";
            else if (name == "url")
            {
                std::string href = arg.empty() ? in.substr(rb + 1, closeAt - rb - 1) : arg;
                out += "<a href=\"" + Esc(href, false) + "\" style=\"color:#66c0f4\">";
            }
            else if (name == "list") { trimBreak(); out += "<ul>"; }
            else if (name == "olist") { trimBreak(); out += "<ol>"; }
            else if (name == "quote")
            {
                trimBreak();
                out += "<table width=\"100%\" cellpadding=\"6\" bgcolor=\"#131a25\"><tr><td>";
                if (!arg.empty()) out += "<i>Originally posted by <b>" + Esc(arg, false) + "</b>:</i><br>";
                swallow = true;
            }
            else if (name == "table")
            {
                trimBreak();
                const bool noborder = body.find("noborder=1") != std::string::npos, equal = body.find("equalcells=1") != std::string::npos;
                out += std::string("<table cellspacing=\"0\" cellpadding=\"4\" border=\"") + (noborder ? "0" : "1") + "\"" + (equal ? " width=\"100%\"" : "") + ">";
            }
            else if (name == "tr") out += "<tr>";
            else if (name == "th") out += "<th align=\"left\">";
            else if (name == "td") out += "<td>";
        }
        while (!stack.empty())                                   // a closer that was eaten as text: keep the markup well formed
        {
            const Open top = stack.back(); stack.pop_back();
            const std::string& n = top.name;
            if (n == "h1" || n == "h2" || n == "h3") out += "</div>";
            else if (n == "b" || n == "u" || n == "i") out += "</" + n + ">";
            else if (n == "strike") out += "</s>"; else if (n == "spoiler") out += "</span>"; else if (n == "url") out += "</a>";
            else if (n == "list" || n == "olist") { if (top.item) out += "</li>"; out += n == "list" ? "</ul>" : "</ol>"; }
            else if (n == "quote") out += "</td></tr></table>"; else if (n == "table") out += "</table>";
            else if (n == "tr") out += "</tr>"; else if (n == "th") out += "</th>"; else if (n == "td") out += "</td>";
        }
        return out;
    }

    // ---- the thumbnail against Steam's rules ----
    // ISteamUGC::SetItemPreview: "must be under 1MB", "suggested formats include JPG, PNG and GIF". Judged by content.
    const long long kMaxPreviewBytes = 1024 * 1024;
    struct Verdict { bool ok = true; std::string text; };
    Verdict CheckThumbnail(const std::string& pathUtf8)
    {
        Verdict v;
        std::string path = pathUtf8;
        while (!path.empty() && (path.back() == ' ' || path.back() == '\t')) path.pop_back();
        if (path.empty()) { v.text = "No thumbnail chosen (Steam accepts PNG, JPG or GIF under 1 MB)."; return v; }
        v.ok = false;
        const std::wstring w = Widen(path);
        for (wchar_t ch : w) if (ch > 0xFF) { v.text = "The path has characters the launcher cannot pass to Steam. Move the image to a plain folder."; return v; }
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        if (!GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &fad) || (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { v.text = "File not found."; return v; }
        const long long size = (static_cast<long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        std::string head;
        if (!ReadWhole(w, head, kMaxPreviewBytes)) { v.text = "The file cannot be read."; return v; }
        auto u = [&](size_t i) { return static_cast<unsigned char>(head[i]); };
        const char* kind = nullptr; unsigned width = 0, height = 0;
        if (head.size() >= 24 && memcmp(head.data(), "\x89PNG\r\n\x1a\n", 8) == 0)
        {
            kind = "PNG";
            width = (u(16) << 24) | (u(17) << 16) | (u(18) << 8) | u(19); height = (u(20) << 24) | (u(21) << 16) | (u(22) << 8) | u(23);
        }
        else if (head.size() >= 10 && (memcmp(head.data(), "GIF87a", 6) == 0 || memcmp(head.data(), "GIF89a", 6) == 0))
        {
            kind = "GIF"; width = u(6) | (u(7) << 8); height = u(8) | (u(9) << 8);
        }
        else if (head.size() >= 4 && u(0) == 0xFF && u(1) == 0xD8 && u(2) == 0xFF)
        {
            kind = "JPG";
            for (size_t p = 2; p + 9 < head.size() && u(p) == 0xFF; )      // segments up to the frame header (SOFn)
            {
                const unsigned m = u(p + 1);
                if (m == 0xFF) { ++p; continue; }
                if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) { height = (u(p + 5) << 8) | u(p + 6); width = (u(p + 7) << 8) | u(p + 8); break; }
                p += 2 + ((u(p + 2) << 8) | u(p + 3));
            }
        }
        char buf[200];
        if (!kind) { v.text = "Not a PNG, JPG or GIF image (judged by its content). Steam accepts only those."; return v; }
        if (size >= kMaxPreviewBytes)
        {
            sprintf_s(buf, "%s, %.2f MB: too large, Steam takes preview images under 1 MB.", kind, double(size) / (1024.0 * 1024.0));
            v.text = buf; return v;
        }
        v.ok = true;
        if (width && height) sprintf_s(buf, "%s, %u x %u, %lld KB: accepted by Steam.", kind, width, height, (size + 1023) / 1024);
        else sprintf_s(buf, "%s, %lld KB: accepted by Steam.", kind, (size + 1023) / 1024);
        v.text = buf;
        return v;
    }


    // ---- languages: was the item linked for every language the launcher offers? ----
    // The launcher's Build Language "All" passes these 12 to the linker (gLanguages in the ModLauncher source),
    // each writes zone\<prefix>_<zone>.ff after the main zone\<zone>.ff.
    struct Language { const char* name; const char* prefix; };
    const Language kLanguages[] = {
        { "English", "en" }, { "French", "fr" }, { "Italian", "it" }, { "Spanish", "es" }, { "German", "ge" }, { "Portuguese", "bp" },
        { "Russian", "ru" }, { "Polish", "po" }, { "Japanese", "ja" }, { "Traditional Chinese", "tc" }, { "Simplified Chinese", "sc" },
        { "English (Arabic)", "ea" },
    };
    const unsigned long long kLinkSlack = 2ull * 10000000;   // 2 s in FILETIME units
    bool WriteTime(const std::wstring& path, unsigned long long& t, unsigned long long* size = nullptr)
    {
        WIN32_FILE_ATTRIBUTE_DATA fa = {};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
        t = (static_cast<unsigned long long>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
        if (size) *size = (static_cast<unsigned long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        return true;
    }
    // skip_localized (localized.h) can keep a localized fastfile older than the main one. It belongs to the main
    // fastfile's link when the state's kept= time is that link's, else when the skip would still keep it now.
    bool KeptCurrent(const std::wstring& folder, const std::wstring& zone, const std::string& language, const std::wstring& ff, unsigned long long mainT)
    {
        if (!localized::g_stat)
        {
            HMODULE crt = GetModuleHandleW(L"msvcr110.dll");
            localized::g_stat = crt ? reinterpret_cast<localized::StatFn>(GetProcAddress(crt, "_stat64i32")) : nullptr;
            if (!localized::g_stat) return false;
        }
        localized::Job job;
        job.map = Narrow(zone); job.language = language;
        job.root = rp::host->RadiantRoot(rp::self);
        job.deps = folder + L"\\zone_source\\" + Widen(language) + L"\\assetinfo\\" + zone + L".deps";
        job.zoneDir = folder + L"\\zone";
        job.mapDirLower = folder + L"\\";
        CharLowerBuffW(&job.mapDirLower[0], DWORD(job.mapDirLower.size()));
        job.gdtDb = job.root + L"\\gdtdb\\gdt.db";
        job.statePath = std::wstring(rp::host->PluginDir(rp::self)) + L"\\linker_state\\" + zone + L"_" + Widen(language) + L".txt";
        localized::State st;
        if (!st.Load(job.statePath)) return false;
        if (st.kv.count("kept"))
            return _wcsicmp(localized::Widen(st.kv["ff"]).c_str(), ff.c_str()) == 0 && localized::Identity(ff) == st.kv["ffid"]
                && strtoull(st.kv["kept"].c_str(), nullptr, 10) + kLinkSlack >= mainT;
        size_t checked = 0;
        return localized::Decide(job, checked).empty();
    }
    struct LangVerdict { bool ok = true; std::string text, missing; };
    LangVerdict CheckLanguages(const std::wstring& folder)
    {
        LangVerdict v;
        if (folder.empty()) { v.text = "Languages: not checked (no checked map or mod found)."; return v; }
        const std::wstring zoneDir = folder + L"\\zone";
        std::vector<std::wstring> zones;                         // the main fastfiles: <map>.ff, or the mod's *_mod.ff
        const bool isMod = folder.find(L"\\mods\\") != std::wstring::npos;
        if (isMod) { for (const wchar_t* z : { L"core_mod", L"mp_mod", L"cp_mod", L"zm_mod" }) { unsigned long long t; if (WriteTime(zoneDir + L"\\" + z + L".ff", t)) zones.push_back(z); } }
        else
        {
            const std::wstring name = folder.substr(folder.find_last_of(L'\\') + 1);
            unsigned long long t;
            if (WriteTime(zoneDir + L"\\" + name + L".ff", t)) zones.push_back(name);
        }
        if (zones.empty()) { v.ok = false; v.missing = "the item has not been linked yet"; v.text = "Languages: not linked yet (no fastfile in zone)."; return v; }
        std::vector<std::string> bad, current;
        for (const Language& l : kLanguages)
        {
            bool ok = true;
            for (const std::wstring& z : zones)
            {
                unsigned long long mainT = 0, t = 0;
                WriteTime(zoneDir + L"\\" + z + L".ff", mainT);
                const std::wstring ff = zoneDir + L"\\" + Widen(l.prefix) + L"_" + z + L".ff";
                std::string lower = l.name; for (char& ch : lower) ch = char(tolower(static_cast<unsigned char>(ch)));
                std::string key = lower;                         // the linker's -language value
                key.erase(std::remove_if(key.begin(), key.end(), [](char ch) { return ch == ' ' || ch == '(' || ch == ')'; }), key.end());
                if (!WriteTime(ff, t) || (t + kLinkSlack < mainT && !KeptCurrent(folder, z, key, ff, mainT))) ok = false;
            }
            (ok ? current : bad).push_back(l.name);
        }
        if (bad.empty()) { v.text = "Languages: all 12 linked with this build."; return v; }
        v.ok = false;
        for (size_t i = 0; i < bad.size(); ++i) v.missing += (i ? ", " : "") + bad[i];
        v.text = "Languages: " + std::to_string(current.size()) + " of 12 linked with this build. Missing or from an older link: " + v.missing
               + ". Link with Build Language \"All\" (Edit > Options) so it loads in every language.";
        return v;
    }

    // ---- Steam: the Workshop text instead of the launcher's description ----
    // k_cchPublishedDocumentDescriptionMax = 8000 (with the terminator)
    const size_t kMaxDescription = 7999;
    std::string g_workshopText;          // what the dialog held when it closed
    bool g_armed = false;                // SetItemDescription gets g_workshopText
    bool g_steamTried = false, g_steamHooked = false;
    std::string g_steamState = "not looked up yet";
    using SetDescFn = bool (*)(void*, uint64_t, const char*);
    SetDescFn g_origSetDesc = nullptr;
    unsigned g_sent = 0;
    bool SetDescHook(void* self, uint64_t handle, const char* description)
    {
        if (!g_armed) return g_origSetDesc(self, handle, description);
        ++g_sent;
        rp::Log("publish: Workshop Description handed to Steam (%zu bytes) instead of the map description", g_workshopText.size());
        return g_origSetDesc(self, handle, g_workshopText.c_str());
    }
    // the vtable slot from steam_api64's flat thunk (mov rax,[rcx], jmp [rax+disp])
    bool ThunkSlot(HMODULE api, const char* name, size_t& offset)
    {
        auto* p = reinterpret_cast<const unsigned char*>(GetProcAddress(api, name));
        if (!p || p[0] != 0x48 || p[1] != 0x8B || p[2] != 0x01 || p[3] != 0x48 || p[4] != 0xFF) return false;
        if (p[5] == 0x60) { offset = p[6]; return true; }
        if (p[5] == 0xA0) { unsigned d; memcpy(&d, p + 6, 4); offset = d; return true; }
        if (p[5] == 0x20) { offset = 0; return true; }
        return false;
    }
    void HookSteam()   // once Steam is up (without it the launcher cannot publish anyway)
    {
        if (g_steamHooked) return;
        HMODULE api = GetModuleHandleW(L"steam_api64.dll");
        if (!api) { g_steamState = "steam_api64.dll is not loaded"; return; }
        auto client = reinterpret_cast<void* (*)()>(GetProcAddress(api, "SteamClient"));
        auto user = reinterpret_cast<int (*)()>(GetProcAddress(api, "SteamAPI_GetHSteamUser"));
        auto pipe = reinterpret_cast<int (*)()>(GetProcAddress(api, "SteamAPI_GetHSteamPipe"));
        auto getUgc = reinterpret_cast<void* (*)(void*, int, int, const char*)>(GetProcAddress(api, "SteamAPI_ISteamClient_GetISteamUGC"));
        size_t slot = 0;
        if (!client || !user || !pipe || !getUgc || !ThunkSlot(api, "SteamAPI_ISteamUGC_SetItemDescription", slot)) { g_steamState = "steam_api64.dll has no flat API"; return; }
        void* c = client();
        if (!c) { g_steamState = "Steam is not initialised (start the launcher from Steam)"; return; }
        // the interface version the launcher asks for (the string is in modlauncher.exe)
        void* ugc = getUgc(c, user(), pipe(), "STEAMUGC_INTERFACE_VERSION008");
        if (!ugc) { g_steamState = "Steam has no STEAMUGC_INTERFACE_VERSION008"; return; }
        void* target = (*reinterpret_cast<void***>(ugc))[slot / sizeof(void*)];
        if (g_steamTried) return;                                // one attempt at the detour itself
        g_steamTried = true;
        if (!target || !rp::host->Detour(rp::self, target, reinterpret_cast<void*>(SetDescHook), reinterpret_cast<void**>(&g_origSetDesc))) { g_steamState = "the detour on SetItemDescription failed"; return; }
        g_steamHooked = true;
        char buf[96]; sprintf_s(buf, "hooked (ISteamUGC008 slot %zu)", slot / sizeof(void*));
        g_steamState = buf;
        rp::Log("publish: ISteamUGC::SetItemDescription %s", g_steamState.c_str());
    }

    // ---- the thumbnail file dialog: only what Steam takes ----
    using OpenNameFn = void* (*)(void*, void*, const void**, const void**, const void**, void*, int);
    OpenNameFn g_origOpenName = nullptr;
    void* OpenNameHook(void* ret, void* parent, const void** caption, const void** dir, const void** filter, void* selected, int options)
    {
        if (caption && FromQ(*caption) == "Open Thumbnail")
        {
            QStr f("Steam preview images (*.png *.jpg *.jpeg *.gif)");
            const void* fd = f.d;
            return g_origOpenName(ret, parent, caption, dir, &fd, selected, options);
        }
        return g_origOpenName(ret, parent, caption, dir, filter, selected, options);
    }

    // ---- the dialog ----
    struct Tag { const char* label; const char* open; const char* close; const char* tip; };
    const Tag kTags[] = {
        { "H1", "[h1]", "[/h1]", "Header" }, { "H2", "[h2]", "[/h2]", "Smaller header" }, { "H3", "[h3]", "[/h3]", "Smallest header" },
        { "B", "[b]", "[/b]", "Bold" }, { "I", "[i]", "[/i]", "Italic" }, { "U", "[u]", "[/u]", "Underlined" }, { "S", "[strike]", "[/strike]", "Strikethrough" },
        { "Spoiler", "[spoiler]", "[/spoiler]", "Hidden until hovered" }, { "Link", "[url=https://]", "[/url]", "Website link: [url=address]text[/url]" },
        { "List", "[list]\n[*]", "\n[/list]", "Bulleted list, one [*] per item" }, { "1. List", "[olist]\n[*]", "\n[/olist]", "Numbered list, one [*] per item" },
        { "Quote", "[quote=author]", "[/quote]", "Quoted text" }, { "Code", "[code]", "[/code]", "Fixed-width text, spaces kept" },
        { "Rule", "[hr][/hr]", "", "Horizontal rule" },
        { "Table", "[table]\n[tr]\n[th]Name[/th]\n[th]Value[/th]\n[/tr]\n[tr]\n[td]", "[/td]\n[td][/td]\n[/tr]\n[/table]", "Table: [tr] rows of [th] header and [td] data cells" },
        { "No parse", "[noparse]", "[/noparse]", "Shows tags as text" },
    };
    struct Dialog
    {
        void* widget = nullptr;          // the launcher's QDialog (on its stack: valid until it hides)
        void* mapEdit = nullptr;         // the stock Description line
        void* thumbEdit = nullptr;
        void* okButton = nullptr;
        void* editor = nullptr;          // ours, children of the dialog from here on
        void* preview = nullptr;
        void* scroll = nullptr;
        unsigned imgVersion = 0; int imgWidth = 0;
        void* counter = nullptr;
        void* thumbStatus = nullptr;
        void* langStatus = nullptr;
        std::wstring folder;
        ULONGLONG langAt = 0; bool langOk = true; std::string langMissing;
        void* draftButton = nullptr;
        std::wstring draftFile;          // <usermaps|mods>\<name>\workshop_description.txt
        std::string draft;               // its text when the dialog opened
        std::string loaded;              // the editor's first text
        std::string shownText; bool shownOnce = false;
        std::string thumbPath; ULONGLONG thumbAt = 0; bool thumbOk = true;
        bool tooLong = false;
    } g;
    ModToolsWidget g_filter = nullptr;
    unsigned g_enhanced = 0;
    std::string g_last = "no Publish dialog seen yet";

    // the item the launcher publishes: its first checked row, maps before mods (mlMainWindow::OnEditPublish)
    std::wstring ItemFolder(void* dialog)
    {
        void* main = qt.parent(dialog);
        if (!main) return L"";
        for (int n = 0; n < 16; ++n)
        {
            void* tree = Ui()->FindChild(main, "QTreeWidget", n);
            if (!tree) break;
            if (Ui()->TreeChildCount(tree, nullptr) != 2) continue;
            auto textOf = [&](ModToolsTreeItem it) { char b[512] = {}; Ui()->TreeItemText(it, 0, b, sizeof b); return std::string(b); };
            ModToolsTreeItem maps = Ui()->TreeChild(tree, nullptr, 0), mods = Ui()->TreeChild(tree, nullptr, 1);
            if (textOf(maps) != "Maps" || textOf(mods) != "Mods") continue;
            const std::wstring root = rp::host->RadiantRoot(rp::self);
            for (int i = 0; i < Ui()->TreeChildCount(tree, maps); ++i)
            {
                ModToolsTreeItem it = Ui()->TreeChild(tree, maps, i);
                if (Ui()->TreeItemChecked(it)) return root + L"\\usermaps\\" + Widen(textOf(it));
            }
            for (int i = 0; i < Ui()->TreeChildCount(tree, mods); ++i)
            {
                ModToolsTreeItem mod = Ui()->TreeChild(tree, mods, i);
                for (int k = 0; k < Ui()->TreeChildCount(tree, mod); ++k)
                    if (Ui()->TreeItemChecked(Ui()->TreeChild(tree, mod, k))) return root + L"\\mods\\" + Widen(textOf(mod));
            }
            return L"";
        }
        return L"";
    }
    bool IsPublishDialog(void* w)
    {
        HWND h = reinterpret_cast<HWND>(qt.winId(w));
        wchar_t title[64] = {};
        if (!h || !GetWindowTextW(h, title, 64) || wcscmp(title, L"Publish Mod") != 0) return false;
        return Ui()->FindChild(w, "QDialogButtonBox", 0) != nullptr;
    }
    void Capture()   // the editor's text, for Steam and for the draft file
    {
        if (!g.editor) return;
        g_workshopText = PlainText(g.editor);
        if (g_workshopText.size() > kMaxDescription) g_workshopText.resize(kMaxDescription);   // OK is disabled then, never over Steam's buffer
        g_armed = true;
    }
    void Forget() { g = Dialog(); }
    int DialogFilter(void*, ModToolsWidget watched, int type)
    {
        if (type == 18 && watched == g.widget && g.editor)       // QEvent::Hide: accepted or cancelled, the widgets die next
        {
            Capture();
            if (!g.draftFile.empty() && (g_workshopText != g.loaded || !g.draft.empty()) && g_workshopText != g.draft)
                WriteWhole(g.draftFile, g_workshopText);
            g_last = "closed, Workshop Description " + std::to_string(g_workshopText.size()) + " bytes ready for Steam";
            Forget();
        }
        return 0;
    }
    void InsertTag(void* user)
    {
        const Tag& t = kTags[reinterpret_cast<size_t>(user)];
        if (!g.editor) return;
        void* cursor = nullptr; qt.plainCursor(g.editor, &cursor);
        void* sel = nullptr; qt.cursorSelectedText(&cursor, &sel);
        const std::string selected = FromQ(sel); qt.qsDtor(&sel);
        { QStr q(std::string(t.open) + selected + t.close); qt.cursorInsertText(&cursor, &q.d); }
        if (selected.empty() && *t.close)                        // caret between the tags
        {
            qt.cursorMove(&cursor, 9 /*Left*/, 0 /*MoveAnchor*/, int(Widen(t.close).size()));
            qt.plainSetCursor(g.editor, &cursor);
        }
        qt.cursorDtor(&cursor);
        Ui()->SetFocus(g.editor);
    }
    void RestoreDraft(void*)
    {
        if (!g.editor) return;
        PlainSet(g.editor, g.draft);
        Ui()->SetVisible(g.draftButton, 0);
    }
    void Enhance(void* dialog)
    {
        const ModToolsUiApi* ui = Ui();
        g_armed = false;                                         // a dialog we cannot extend publishes the stock way
        void* title = ui->FindChild(dialog, "QLineEdit", 0);
        void* mapEdit = ui->FindChild(dialog, "QLineEdit", 1);
        void* thumbEdit = ui->FindChild(dialog, "QLineEdit", 2);
        void* box = ui->FindChild(dialog, "QDialogButtonBox", 0);
        void* layout = qt.layout(dialog);
        void* descLabel = nullptr;
        for (int n = 0; n < 12 && !descLabel; ++n)
        {
            void* l = ui->FindChild(dialog, "QLabel", n);
            if (!l) break;
            if (LabelText(l) == "Description:") descLabel = l;
        }
        if (!title || !mapEdit || !thumbEdit || !box || !layout || !descLabel || ui->FindChild(dialog, "QLineEdit", 3))
        {
            g_last = "a Publish dialog with another layout: left alone";
            rp::Log("publish: %s", g_last.c_str());
            g.widget = dialog;                                   // not again for this one
            return;
        }
        HookSteam();
        if (!g_steamHooked)
        {
            g_last = "Publish dialog left alone, Steam: " + g_steamState;
            rp::Log("publish: %s", g_last.c_str());
            g.widget = dialog;
            return;
        }
        g.widget = dialog; g.mapEdit = mapEdit; g.thumbEdit = thumbEdit;
        g.okButton = qt.boxButton(box, 0x400 /*QDialogButtonBox::Ok*/);

        // what is on disk: the map description of workshop.json and the last Workshop text typed here
        const std::wstring folder = ItemFolder(dialog);
        std::string json, mapText, id; bool haveMap = false;
        if (!folder.empty() && ReadWhole(folder + L"\\zone\\workshop.json", json))
        {
            haveMap = JsonString(json, "Description", mapText);
            JsonString(json, "PublisherID", id);
        }
        const bool published = !id.empty() && id != "0";
        if (!folder.empty()) { g.draftFile = folder + L"\\workshop_description.txt"; std::string d; if (ReadWhole(g.draftFile, d)) g.draft = Unix(d); }
        // the line holds Steam's text for a published item (RequestUGCDetails), else workshop.json's
        const std::string lineText = Unix(LineText(mapEdit));
        g.loaded = published ? lineText : !g.draft.empty() ? g.draft : lineText;
        if (published && haveMap) ui->SetEditText(mapEdit, mapText.c_str());
        ui->SetLabelText(descLabel, "Map Description:");
        ui->SetToolTip(mapEdit, "Shown in the game's map and mod lists (zone\\workshop.json). Plain text, no formatting tags.");

        // the Workshop Description block, under the form
        void* panel = ui->Widget(dialog);
        void* col = ui->VBox(panel);
        ui->LayoutMargins(col, 0, 6, 0, 0);
        void* head = ui->HBox();
        void* headLabel = ui->Label("Workshop Description (the item's Steam page; Steam formatting tags):", panel);
        g.counter = ui->Label("", panel);
        ui->BoxAddWidget(head, headLabel, 1); ui->BoxAddWidget(head, g.counter, 0);
        ui->BoxAddLayout(col, head, 0);
        void* bar = ui->HBox();
        ui->LayoutSpacing(bar, 2);
        for (size_t n = 0; n < sizeof kTags / sizeof kTags[0]; ++n)
        {
            void* b = ui->PushButton(kTags[n].label, panel);
            ui->SetToolTip(b, kTags[n].tip);
            rp::host->OnClicked(rp::self, b, InsertTag, reinterpret_cast<void*>(n));
            ui->BoxAddWidget(bar, b, 0);
        }
        ui->BoxAddStretch(bar, 1);
        ui->BoxAddLayout(col, bar, 0);
        void* split = ui->Splitter(1, panel);
        g.editor = ui->PlainTextEdit(split);
        g.preview = ui->Label("", nullptr);
        qt.labelSetFormat(g.preview, 1 /*Qt::RichText*/);
        qt.labelSetAlignment(g.preview, 0x1 | 0x20 /*AlignLeft | AlignTop*/);
        qt.labelSetInteraction(g.preview, 1 /*TextSelectableByMouse*/);
        ui->SetWordWrap(g.preview, 1);
        void* scroll = ui->ScrollArea(g.preview, split);
        { QStr sheet("QScrollArea, QScrollArea > QWidget > QWidget, QLabel { background-color: #1b2838; } QLabel { color: #acb2b8; padding: 8px; font-size: 13px; }"); qt.setStyleSheet(scroll, &sheet.d); }
        g.scroll = scroll;
        ui->SetToolTip(scroll, "Preview of the Workshop page text. Spoilers show dark; pictures are fetched from their address.");
        ui->SplitterAdd(split, g.editor, 1); ui->SplitterAdd(split, scroll, 1);
        ui->SetMinimumSize(g.editor, 430, 240); ui->SetMinimumSize(scroll, 430, 240);   // equal halves: the splitter sizes by hint otherwise
        ui->BoxAddWidget(col, split, 1);
        g.draftButton = ui->PushButton("Bring back the text last typed here (differs from Steam's)", panel);
        rp::host->OnClicked(rp::self, g.draftButton, RestoreDraft, nullptr);
        ui->BoxAddWidget(col, g.draftButton, 0);
        ui->SetVisible(g.draftButton, !g.draft.empty() && Trimmed(g.draft) != Trimmed(g.loaded) ? 1 : 0);
        g.thumbStatus = ui->Label("", panel);
        ui->SetWordWrap(g.thumbStatus, 1);
        ui->BoxAddWidget(col, g.thumbStatus, 0);
        g.langStatus = ui->Label("Languages:", panel);
        ui->SetWordWrap(g.langStatus, 1);
        ui->BoxAddWidget(col, g.langStatus, 0);
        g.folder = folder;
        qt.boxInsertWidget(layout, 1, panel, 1, 0);
        PlainSet(g.editor, g.loaded);
        qt.resize(dialog, 980, 720);

        if (!g_filter) g_filter = rp::host->MakeEventFilter(rp::self, DialogFilter, nullptr);
        if (g_filter) ui->InstallFilter(dialog, g_filter);
        ++g_enhanced;
        g_last = std::string("open: ") + (published ? "published item " + id : "new item") + ", " + (folder.empty() ? "folder unknown" : Narrow(folder));
        rp::Log("publish: dialog extended, %s", g_last.c_str());
    }
    // A dialog extended by an earlier load of this plugin: its widgets are found again by structure (else OK would send the map description to Steam).
    void Adopt(void* dialog)
    {
        const ModToolsUiApi* ui = Ui();
        void* editor = ui->FindChild(dialog, "QPlainTextEdit", 0);
        void* scroll = ui->FindChild(dialog, "QScrollArea", 0);
        void* preview = scroll ? ui->FindChild(scroll, "QLabel", 0) : nullptr;
        void* split = editor ? qt.parent(editor) : nullptr;
        void* panel = split ? qt.parent(split) : nullptr;
        void* box = ui->FindChild(dialog, "QDialogButtonBox", 0);
        void* counter = nullptr; void* status = nullptr;
        for (int n = 0; panel && n < 8; ++n)
        {
            void* l = ui->FindChild(panel, "QLabel", n);
            if (!l) break;
            if (l == preview) continue;
            const std::string t = LabelText(l);
            if (t.find(" bytes") != std::string::npos) counter = l; else if (t.rfind("Thumbnail:", 0) == 0) status = l;
            else if (t.rfind("Languages:", 0) == 0) g.langStatus = l;
        }
        HookSteam();
        g.widget = dialog;
        if (!editor || !preview || !panel || !box || !counter || !status || !g_steamHooked)
        {
            g_last = "an extended Publish dialog could not be taken over: close and reopen it";
            rp::Log("publish: %s", g_last.c_str());
            return;
        }
        g.mapEdit = ui->FindChild(dialog, "QLineEdit", 1); g.thumbEdit = ui->FindChild(dialog, "QLineEdit", 2);
        g.okButton = qt.boxButton(box, 0x400);
        g.editor = editor; g.scroll = scroll; g.preview = preview; g.counter = counter; g.thumbStatus = status;
        const size_t tags = sizeof kTags / sizeof kTags[0];
        for (size_t n = 0; n <= tags; ++n)
        {
            void* b = ui->FindChild(panel, "QPushButton", int(n));
            if (!b) break;
            if (n < tags) rp::host->OnClicked(rp::self, b, InsertTag, reinterpret_cast<void*>(n));
            else { g.draftButton = b; ui->SetVisible(b, 0); }
        }
        const std::wstring folder = ItemFolder(dialog);
        if (!folder.empty()) { g.draftFile = folder + L"\\workshop_description.txt"; std::string d; if (ReadWhole(g.draftFile, d)) g.draft = Unix(d); }
        g.folder = folder;
        g.loaded = g.draft;                                      // unknown here: a changed text is saved
        if (!g_filter) g_filter = rp::host->MakeEventFilter(rp::self, DialogFilter, nullptr);
        if (g_filter) ui->InstallFilter(dialog, g_filter);
        g_last = "open (taken over after a plugin reload)";
        rp::Log("publish: dialog taken over after a reload");
    }
    void Update()
    {
        const ModToolsUiApi* ui = Ui();
        const std::string text = PlainText(g.editor);
        int width = 400;
        if (qt.frameGeometry) { int r[4] = {}; qt.frameGeometry(g.scroll, r); width = r[2] - r[0] + 1 - 40; }   // less padding and scroll bar
        const unsigned version = g_imgVersion;
        const bool changed = !g.shownOnce || text != g.shownText;
        if (changed || version != g.imgVersion || (width != g.imgWidth && !g_imgShown.empty()))
        {
            g.imgVersion = version; g.imgWidth = g_imgMaxWidth = width;
            ui->SetLabelText(g.preview, text.empty() ? "<i>Nothing to preview.</i>" : BbToHtml(text, true).c_str());
        }
        QueueImages();
        if (changed)
        {
            g.shownOnce = true; g.shownText = text;
            g.tooLong = text.size() > kMaxDescription;
            char buf[160];
            sprintf_s(buf, g.tooLong ? "<span style=\"color:#e05050\">%zu / %zu bytes: too long for Steam</span>" : "%zu / %zu bytes", text.size(), kMaxDescription);
            ui->SetLabelText(g.counter, buf);
        }
        const std::string path = LineText(g.thumbEdit);
        const ULONGLONG now = GetTickCount64();
        if (path != g.thumbPath || now - g.thumbAt > 1000)       // the file can change under the same name
        {
            g.thumbPath = path; g.thumbAt = now;
            const Verdict v = CheckThumbnail(path);
            g.thumbOk = v.ok;
            ui->SetLabelText(g.thumbStatus, (std::string("Thumbnail: ") + (v.ok ? "" : "<span style=\"color:#e05050\">") + Esc(v.text) + (v.ok ? "" : "</span>")).c_str());
        }
        if (g.langStatus && now - g.langAt > 2000)
        {
            g.langAt = now;
            const LangVerdict lv = CheckLanguages(g.folder);
            g.langOk = lv.ok; g.langMissing = lv.missing;
            ui->SetLabelText(g.langStatus, (lv.ok ? Esc(lv.text) : "<span style=\"color:#e0a030\">" + Esc(lv.text) + "</span>").c_str());
        }
        if (g.okButton)
        {
            const bool ok = g.thumbOk && !g.tooLong;
            ui->SetEnabled(g.okButton, ok ? 1 : 0);
            ui->SetToolTip(g.okButton, ok ? "" : !g.thumbOk ? "The thumbnail would not be accepted by Steam." : "The Workshop Description is too long for Steam.");
        }
    }

    // OK (or Enter) on the extended dialog: ask before publishing an item not linked in every language
    using AcceptFn = void (*)(void*);
    AcceptFn g_origAccept = nullptr;
    bool g_autoAnswer = false; int g_autoValue = IDNO;            // test aid: "publish answer yes|no"
    void AcceptHook(void* dialog)
    {
        if (dialog != g.widget || !g.editor) return g_origAccept(dialog);
        const LangVerdict lv = CheckLanguages(g.folder);
        if (!lv.ok)
        {
            const std::string text = "This upload is not linked for every language.\n\nMissing or from an older link: " + lv.missing
                + ".\n\nPlayers whose game runs in one of those languages cannot load it. Link with Build Language \"All\" "
                  "(Edit > Options) before publishing.\n\nPublish anyway?";
            const int answer = g_autoAnswer ? g_autoValue
                : MessageBoxW(reinterpret_cast<HWND>(qt.winId(dialog)), Widen(text).c_str(), L"Publish Mod", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            g_autoAnswer = false;
            rp::Log("publish: languages incomplete (%s), answer %s", lv.missing.c_str(), answer == IDYES ? "publish anyway" : "back to the dialog");
            if (answer != IDYES) { g_last = "OK held back: languages incomplete"; return; }
        }
        g_origAccept(dialog);
    }

    // test aid: the launcher's own Publish action, started from a timer so the host's tick keeps running under the dialog
    void CALLBACK OpenTimer(HWND, UINT, UINT_PTR id, DWORD)
    {
        KillTimer(nullptr, id);
        if (rp::host->TriggerMenuAction(rp::self, "Edit/Publish") != 1) g_last = "Edit > Publish not found";
    }
}

void Setup()
{
    if (!BindQt()) { rp::Log("publish: Qt exports missing, the Publish dialog stays stock"); return; }
    if (!rp::host->Detour(rp::self, qt.getOpenFileName, reinterpret_cast<void*>(OpenNameHook), reinterpret_cast<void**>(&g_origOpenName)))
        rp::Log("publish: the thumbnail file dialog keeps its All Files filter");
    if (!rp::host->Detour(rp::self, qt.dialogAccept, reinterpret_cast<void*>(AcceptHook), reinterpret_cast<void**>(&g_origAccept)))
        rp::Log("publish: no language question on OK (QDialog::accept not hooked)");
}

void Tick()
{
    if (!qt.bound) return;
    void* modal = qt.activeModal();
    if (g.widget && !g.editor && modal != g.widget) Forget();    // a dialog we left alone is gone
    if (!g.widget)
    {
        if (!modal || !IsPublishDialog(modal)) return;
        if (Ui()->FindChild(modal, "QPlainTextEdit", 0)) Adopt(modal); else Enhance(modal);
    }
    if (g.editor) Update();
}

void Shutdown()
{
    // our widgets belong to the dialog and go with it (without the detour the launcher's own description is sent)
    Forget();
    StopImages();
    g_filter = nullptr; g_armed = false; g_steamHooked = false; g_steamTried = false; g_origSetDesc = nullptr; g_origOpenName = nullptr; g_origAccept = nullptr;
}

bool Command(const std::string& cmd, std::string& reply)
{
    if (cmd.rfind("publish", 0) != 0) return false;
    const std::string arg = cmd.size() > 8 ? cmd.substr(8) : "";
    auto unescape = [](std::string s) { for (size_t p; (p = s.find("\\n")) != std::string::npos; ) s.replace(p, 2, "\n"); return s; };
    if (arg.empty())
    {
        reply = std::string("Publish dialog: ") + (qt.bound ? "ready" : "Qt exports missing") + ". Steam: " + g_steamState + ". Extended " + std::to_string(g_enhanced)
              + " time(s), Workshop Descriptions sent " + std::to_string(g_sent) + ". Last: " + g_last + "."
              + (g.editor ? std::string(" Open now: OK ") + (g.thumbOk && !g.tooLong ? "enabled" : "disabled") + ", " + std::to_string(g.shownText.size()) + " bytes." : "");
        return true;
    }
    if (arg.rfind("html ", 0) == 0) { reply = BbToHtml(unescape(arg.substr(5)), false); return true; }
    if (arg.rfind("thumb ", 0) == 0) { const Verdict v = CheckThumbnail(arg.substr(6)); reply = std::string(v.ok ? "ok: " : "rejected: ") + v.text; return true; }
    if (arg.rfind("langs", 0) == 0) { const std::string f = arg.size() > 6 ? arg.substr(6) : ""; const LangVerdict v = CheckLanguages(f.empty() ? g.folder : Widen(f)); reply = v.text; return true; }
    if (arg == "answer yes" || arg == "answer no") { g_autoAnswer = true; g_autoValue = arg == "answer yes" ? IDYES : IDNO; reply = "the next language question is answered " + arg.substr(7); return true; }
    if (arg == "ok")   // test aid, never a real upload: only while the question is primed with "no" and languages are incomplete
    {
        if (!g.okButton || !g.editor) { reply = "no extended dialog open"; return true; }
        if (!g_autoAnswer || g_autoValue != IDNO || CheckLanguages(g.folder).ok) { reply = "refused: send \"publish answer no\" first, and only with languages incomplete (this would upload)"; return true; }
        Ui()->ClickButton(g.okButton); reply = g.editor ? "OK clicked, dialog still open" : "OK clicked, dialog closed"; return true;
    }
    if (arg == "steam") { HookSteam(); reply = g_steamState; return true; }
    if (arg == "open") { SetTimer(nullptr, 0, 10, OpenTimer); reply = "Edit > Publish triggered"; return true; }
    if (arg == "cancel") { if (g.widget && g.editor) { qt.dialogReject(g.widget); reply = "cancelled"; } else reply = "no extended dialog open"; return true; }
    if (arg.rfind("text ", 0) == 0) { if (g.editor) { PlainSet(g.editor, unescape(arg.substr(5))); reply = "set"; } else reply = "no extended dialog open"; return true; }
    if (arg.rfind("thumbnail ", 0) == 0) { if (g.thumbEdit) { Ui()->SetEditText(g.thumbEdit, arg.substr(10).c_str()); reply = "set"; } else reply = "no extended dialog open"; return true; }
    if (arg == "read") { reply = g.editor ? "map: " + LineText(g.mapEdit) + "\nthumbnail: " + LineText(g.thumbEdit) + "\nworkshop:\n" + PlainText(g.editor) : "no extended dialog open"; return true; }
    reply = "publish | publish html <tags> | publish thumb <path> | publish langs [folder] | publish answer yes|no | publish ok | publish steam | publish open | publish cancel | publish text <tags> | publish thumbnail <path> | publish read";
    return true;
}
}
