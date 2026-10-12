// Notetrack sounds in APE's preview, see notesounds.h.
// APE's preview holds the play position and every note of the anim (GDT notes and exported notetracks), the plugin only listens.
#include "modtools_plugin.hpp"
#include "notesounds.h"
#include <windows.h>
#include <xaudio2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace ba::notesounds
{
namespace
{
    // ---- APE preview layout (IDA addresses, base 0x140000000) ----
    const uintptr_t kOpts = 0x14118D620;         // previewWindowOpts (pointer)
    const size_t kOptsHasAnim = 0xB41D;          // byte: the preview holds an animation
    const size_t kOptsTime = 0xB528;             // double: play position in seconds, wrapped at the anim length
    const size_t kOptsPaused = 0xB530;           // byte: 1 = paused
    const size_t kSlots = 0xCA0;                 // 32 slots of 1336 bytes, slot +0 = preview object
    const size_t kSlotStride = 1336;
    const int kSlotCount = 32;
    const size_t kObjAnim = 104;                 // object: the anim
    const size_t kObjT = 224;                    // object: float play position 0..1
    const size_t kAnimFrames = 76;               // anim: int frame count
    const size_t kAnimNotes[3] = {136, 160, 184};   // anim: std::vector of 112-byte notes (custom, startup, shutdown)
    const size_t kNoteSize = 112;                // note: +4 int frame, +8 action, +40 param1, +72 param2 (std::string)

    enum Kind { kCustom = 0, kStartup = 1, kShutdown = 2 };

    // ---- settings and state ----
    bool g_on = true;                // [notesounds] enabled
    bool g_exported = true;          // [notesounds] exported: sndnt#<alias> notetracks of the anim file
    bool g_gdt = true;               // [notesounds] gdt: the asset's SOUND / Notetracks actions
    float g_volume = 1.0f;           // [notesounds] volume
    uint8_t** g_opts = nullptr;
    std::string g_root;              // BO3 root, utf-8, no trailing slash

    struct Note { double frame = 0; int kind = kCustom; std::string action, alias; bool exported = false; };

    void* g_anim = nullptr;          // the anim the notes belong to
    std::vector<Note> g_notes;
    std::string g_asset;             // the asset selected when the anim loaded (status only)
    int g_frames = 0;
    double g_prev = 0;
    bool g_prevValid = false;
    bool g_wasPlaying = false;
    uint64_t g_fired = 0, g_played = 0, g_missing = 0;
    std::string g_lastMissing;

    std::string Lower(std::string s) { for (char& c : s) c = char(tolower(static_cast<unsigned char>(c))); return s; }
    std::string Trim(const std::string& s)
    {
        size_t a = 0, b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n' || s[a] == '"')) ++a;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n' || s[b - 1] == '"')) --b;
        return s.substr(a, b - a);
    }
    std::wstring Widen(const std::string& s)
    {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
        std::wstring w(size_t(n), L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
        return w;
    }
    std::string Narrow(const std::wstring& w)
    {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(size_t(n), '\0'); WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }
    bool ReadFile(const fs::path& p, std::vector<uint8_t>& out)
    {
        HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{}; bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (1ll << 31);
        if (ok) { out.resize(size_t(size.QuadPart)); DWORD got = 0; ok = out.empty() || (::ReadFile(h, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size()); }
        CloseHandle(h);
        return ok;
    }

    // the alias a note names, empty when it is not a sound note
    std::string SoundAlias(const std::string& note)
    {
        std::string l = Lower(note);
        if (l.rfind("sndnt#", 0) != 0) return {};
        std::string a = note.substr(6);
        size_t cut = a.find_first_of("#, ");
        return Trim(cut == std::string::npos ? a : a.substr(0, cut));
    }

    // ---- alias index: share\raw\sound\aliases\**.csv with share\raw\sound\templates ----
    struct Alias { std::string fileSpec, secondary; float volMin = 92, volMax = 92, pitchMin = 0, pitchMax = 0; int startDelay = 0; bool looping = false; };

    std::vector<std::string> CsvLine(const std::string& s)
    {
        std::vector<std::string> out; std::string cur; bool q = false;
        for (size_t i = 0; i < s.size(); ++i)
        {
            char c = s[i];
            if (q) { if (c == '"') { if (i + 1 < s.size() && s[i + 1] == '"') { cur += '"'; ++i; } else q = false; } else cur += c; }
            else if (c == '"') q = true;
            else if (c == ',') { out.push_back(cur); cur.clear(); }
            else if (c != '\r') cur += c;
        }
        out.push_back(cur);
        return out;
    }

    using Row = std::unordered_map<std::string, std::string>;   // lower-case column -> value

    template <class Fn> void EachCsvRow(const fs::path& p, Fn fn)
    {
        std::vector<uint8_t> raw; if (!ReadFile(p, raw)) return;
        std::string text(raw.begin(), raw.end());
        std::vector<std::string> header; size_t pos = 0;
        while (pos < text.size())
        {
            size_t end = text.find('\n', pos); if (end == std::string::npos) end = text.size();
            std::string line = text.substr(pos, end - pos); pos = end + 1;
            if (line.empty()) continue;
            std::vector<std::string> cells = CsvLine(line);
            if (header.empty()) { if (Lower(Trim(cells[0])) == "name") for (auto& c : cells) header.push_back(Lower(Trim(c))); continue; }
            std::string name = Trim(cells[0]);
            if (name.empty() || name[0] == '#') continue;
            fn(header, cells);
        }
    }

    float ToF(const std::string& s, float def) { std::string t = Trim(s); if (t.empty()) return def; char* e = nullptr; float v = strtof(t.c_str(), &e); return e == t.c_str() ? def : v; }

    struct Index
    {
        std::unordered_map<std::string, std::vector<Alias>> aliases;
        size_t files = 0;
        uint64_t stamp = 0;
    };

    uint64_t TreeStamp(const fs::path& dir)
    {
        uint64_t h = 1469598103934665603ull; std::error_code ec;
        for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec))
        {
            if (ec) { ec.clear(); continue; }
            if (!it->is_regular_file(ec) || Lower(it->path().extension().string()) != ".csv") continue;
            auto t = fs::last_write_time(it->path(), ec).time_since_epoch().count();
            h = (h ^ uint64_t(t) ^ it->file_size(ec)) * 1099511628211ull;
        }
        return h;
    }

    std::shared_ptr<Index> BuildIndex()
    {
        auto idx = std::make_shared<Index>();
        const fs::path sound = fs::path(Widen(g_root)) / L"share" / L"raw" / L"sound";
        idx->stamp = TreeStamp(sound / L"aliases") ^ (TreeStamp(sound / L"templates") * 31);
        std::unordered_map<std::string, Row> templates;
        std::error_code ec;
        for (fs::directory_iterator it(sound / L"templates", ec), end; it != end; it.increment(ec))
        {
            if (ec || !it->is_regular_file(ec) || Lower(it->path().extension().string()) != ".csv") continue;
            EachCsvRow(it->path(), [&](const std::vector<std::string>& h, const std::vector<std::string>& c)
            {
                Row r; for (size_t i = 0; i < h.size() && i < c.size(); ++i) if (!Trim(c[i]).empty()) r[h[i]] = c[i];
                templates[Lower(Trim(c[0]))] = std::move(r);
            });
        }
        for (fs::recursive_directory_iterator it(sound / L"aliases", fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec))
        {
            if (ec) { ec.clear(); continue; }
            if (!it->is_regular_file(ec) || Lower(it->path().extension().string()) != ".csv") continue;
            ++idx->files;
            EachCsvRow(it->path(), [&](const std::vector<std::string>& h, const std::vector<std::string>& c)
            {
                const Row* tpl = nullptr;
                auto cell = [&](const char* key) -> std::string
                {
                    for (size_t i = 0; i < h.size() && i < c.size(); ++i) if (h[i] == key) { if (!Trim(c[i]).empty()) return c[i]; break; }
                    if (tpl) { auto f = tpl->find(key); if (f != tpl->end()) return f->second; }
                    return {};
                };
                std::string t = Lower(Trim(cell("template")));
                if (!t.empty()) { auto f = templates.find(t); if (f != templates.end()) tpl = &f->second; }
                Alias a;
                a.fileSpec = Trim(cell("filespec")); if (a.fileSpec.empty()) a.fileSpec = Trim(cell("filespecsustain"));
                a.secondary = Trim(cell("secondary"));
                a.volMin = ToF(cell("volmin"), 92); a.volMax = ToF(cell("volmax"), a.volMin);
                a.pitchMin = ToF(cell("pitchmin"), 0); a.pitchMax = ToF(cell("pitchmax"), a.pitchMin);
                a.startDelay = int(ToF(cell("startdelay"), 0));
                a.looping = Lower(Trim(cell("looping"))) == "looping";
                idx->aliases[Lower(Trim(c[0]))].push_back(std::move(a));
            });
        }
        return idx;
    }

    // ---- decoded audio ----
    struct Pcm { std::vector<uint8_t> fmt; std::vector<uint8_t> data; int64_t stamp = 0; const WAVEFORMATEX* Format() const { return reinterpret_cast<const WAVEFORMATEX*>(fmt.data()); } };

    bool LoadWav(const std::vector<uint8_t>& f, Pcm& out)
    {
        if (f.size() < 12 || memcmp(f.data(), "RIFF", 4) != 0 || memcmp(f.data() + 8, "WAVE", 4) != 0) return false;
        size_t p = 12; bool fmt = false, data = false;
        while (p + 8 <= f.size())
        {
            uint32_t cb; memcpy(&cb, f.data() + p + 4, 4);
            const uint8_t* body = f.data() + p + 8;
            size_t avail = std::min<size_t>(cb, f.size() - p - 8);
            if (memcmp(f.data() + p, "fmt ", 4) == 0 && avail >= 16) { out.fmt.assign(body, body + avail); if (out.fmt.size() < sizeof(WAVEFORMATEX)) out.fmt.resize(sizeof(WAVEFORMATEX), 0); fmt = true; }
            else if (memcmp(f.data() + p, "data", 4) == 0) { out.data.assign(body, body + avail); data = true; }
            p += 8 + size_t(cb) + (cb & 1);
        }
        if (!fmt || !data || out.data.empty()) return false;
        WAVEFORMATEX* w = reinterpret_cast<WAVEFORMATEX*>(out.fmt.data());
        if (w->wFormatTag == WAVE_FORMAT_PCM || w->wFormatTag == 3 /* IEEE float */) { w->cbSize = 0; return true; }
        return w->wFormatTag == WAVE_FORMAT_EXTENSIBLE && out.fmt.size() >= sizeof(WAVEFORMATEXTENSIBLE);
    }

    // Media Foundation, loaded on demand (absent on N editions): anything LoadWav does not take, to 16-bit PCM
    bool LoadMf(const fs::path& p, Pcm& out)
    {
        static HMODULE plat = LoadLibraryW(L"mfplat.dll"), rw = LoadLibraryW(L"mfreadwrite.dll");
        static bool started = false;
        if (!plat || !rw) return false;
        using StartupFn = HRESULT (WINAPI*)(ULONG, DWORD); using ReaderFn = HRESULT (WINAPI*)(LPCWSTR, IMFAttributes*, IMFSourceReader**);
        using TypeFn = HRESULT (WINAPI*)(IMFMediaType**);
        auto startup = reinterpret_cast<StartupFn>(GetProcAddress(plat, "MFStartup")); auto makeType = reinterpret_cast<TypeFn>(GetProcAddress(plat, "MFCreateMediaType"));
        auto reader = reinterpret_cast<ReaderFn>(GetProcAddress(rw, "MFCreateSourceReaderFromURL"));
        if (!startup || !makeType || !reader) return false;
        if (!started) { if (FAILED(startup(MF_VERSION, MFSTARTUP_LITE))) return false; started = true; }
        IMFSourceReader* r = nullptr; if (FAILED(reader(p.c_str(), nullptr, &r)) || !r) return false;
        bool ok = false;
        IMFMediaType* type = nullptr;
        if (SUCCEEDED(makeType(&type)))
        {
            type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio); type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM); type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            if (SUCCEEDED(r->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, type)))
            {
                IMFMediaType* cur = nullptr;
                if (SUCCEEDED(r->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &cur)) && cur)
                {
                    UINT32 ch = 0, rate = 0; cur->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch); cur->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
                    cur->Release();
                    if (ch && rate)
                    {
                        out.fmt.assign(sizeof(WAVEFORMATEX), 0);
                        WAVEFORMATEX* w = reinterpret_cast<WAVEFORMATEX*>(out.fmt.data());
                        w->wFormatTag = WAVE_FORMAT_PCM; w->nChannels = WORD(ch); w->nSamplesPerSec = rate; w->wBitsPerSample = 16;
                        w->nBlockAlign = WORD(ch * 2); w->nAvgBytesPerSec = rate * ch * 2;
                        for (;;)
                        {
                            DWORD flags = 0; IMFSample* s = nullptr;
                            if (FAILED(r->ReadSample(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags, nullptr, &s))) break;
                            if (s)
                            {
                                IMFMediaBuffer* b = nullptr;
                                if (SUCCEEDED(s->ConvertToContiguousBuffer(&b)) && b)
                                {
                                    BYTE* d = nullptr; DWORD len = 0;
                                    if (SUCCEEDED(b->Lock(&d, nullptr, &len))) { out.data.insert(out.data.end(), d, d + len); b->Unlock(); }
                                    b->Release();
                                }
                                s->Release();
                            }
                            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
                        }
                        ok = !out.data.empty();
                    }
                }
            }
            type->Release();
        }
        r->Release();
        return ok;
    }

    // ---- the loader thread: alias index and decoding ----
    struct Job { int what = 0; std::string text; };   // 0 alias index, 1 decode file `text`

    std::thread g_thread;
    // APE's exit skips OnUnload: a joinable thread here would std::terminate in the static destructors, so detach it (declared after g_thread).
    struct ExitThreadGuard { ~ExitThreadGuard() { if (g_thread.joinable()) g_thread.detach(); } } g_exitThreadGuard;
    std::mutex g_mutex;
    std::condition_variable g_cv;
    bool g_stop = false;
    std::deque<Job> g_jobs;
    std::shared_ptr<Index> g_index;                                          // under g_mutex
    bool g_indexing = false;
    std::unordered_map<std::string, std::shared_ptr<Pcm>> g_pcm;              // lower-case path -> decoded, no data = failed
    std::unordered_set<std::string> g_decoding;
    size_t g_pcmBytes = 0;
    std::string g_indexNote;
    bool g_rebuilt = false;              // under g_mutex: the index changed, the missing-alias notes start over
    ULONGLONG g_lastStampCheck = 0;

    const size_t kPcmCap = size_t(256) << 20;

    void RunJob(const Job& j)
    {
        if (j.what == 0)
        {
            std::shared_ptr<Index> cur;
            { std::lock_guard<std::mutex> l(g_mutex); cur = g_index; }
            if (cur)
            {
                const fs::path sound = fs::path(Widen(g_root)) / L"share" / L"raw" / L"sound";
                if ((TreeStamp(sound / L"aliases") ^ (TreeStamp(sound / L"templates") * 31)) == cur->stamp)
                {
                    std::lock_guard<std::mutex> l(g_mutex); g_indexing = false; return;
                }
            }
            auto t0 = std::chrono::steady_clock::now();
            auto idx = BuildIndex();
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            char buf[160]; snprintf(buf, sizeof buf, "%zu aliases from %zu csv files in %.0f ms", idx->aliases.size(), idx->files, ms);
            rp::Log("notesounds: %s", buf);
            std::lock_guard<std::mutex> l(g_mutex);
            g_index = idx; g_indexing = false; g_indexNote = buf;
            g_rebuilt = true;
        }
        else if (j.what == 1)
        {
            auto pcm = std::make_shared<Pcm>();
            fs::path p(Widen(j.text));
            std::error_code ec;
            const int64_t stamp = int64_t(fs::last_write_time(p, ec).time_since_epoch().count());
            std::vector<uint8_t> raw;
            bool ok = ReadFile(p, raw) && LoadWav(raw, *pcm);
            if (!ok) { *pcm = Pcm(); ok = LoadMf(p, *pcm); }
            if (!ok) rp::Log("notesounds: cannot decode %s", j.text.c_str());
            pcm->stamp = stamp;
            std::lock_guard<std::mutex> l(g_mutex);
            g_decoding.erase(Lower(j.text));
            if (!ok) pcm->data.clear();
            g_pcm[Lower(j.text)] = pcm;
            if (ok) g_pcmBytes += pcm->data.size();
        }
    }

    void ThreadMain()
    {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;)
        {
            Job j;
            {
                std::unique_lock<std::mutex> l(g_mutex);
                g_cv.wait(l, [] { return g_stop || !g_jobs.empty(); });
                if (g_stop) break;
                j = std::move(g_jobs.front()); g_jobs.pop_front();
            }
            try { RunJob(j); }
            catch (...) { rp::Log("notesounds: loader job %d failed", j.what); std::lock_guard<std::mutex> l(g_mutex); if (j.what == 0) g_indexing = false; if (j.what == 1) g_decoding.erase(Lower(j.text)); }
        }
        if (SUCCEEDED(co)) CoUninitialize();
    }

    void Post(Job j)
    {
        try { if (!g_thread.joinable()) g_thread = std::thread(ThreadMain); }
        catch (...) { return; }
        std::lock_guard<std::mutex> l(g_mutex);
        if (j.what == 0) { if (g_indexing) return; g_indexing = true; }
        if (j.what == 1) { std::string k = Lower(j.text); if (g_decoding.count(k) || g_pcm.count(k)) return; g_decoding.insert(k); }
        g_jobs.push_back(std::move(j));
        g_cv.notify_one();
    }

    // drops decoded files nothing plays once the cache is over its cap. Main thread.
    void TrimPcm()
    {
        std::lock_guard<std::mutex> l(g_mutex);
        if (g_pcmBytes <= kPcmCap) return;
        for (auto it = g_pcm.begin(); it != g_pcm.end() && g_pcmBytes > kPcmCap / 2;)
        {
            if (it->second.use_count() == 1) { g_pcmBytes -= it->second->data.size(); it = g_pcm.erase(it); }
            else ++it;
        }
    }

    // ---- playback (XAudio2 2.9, main thread) ----
    IXAudio2* g_xa = nullptr;
    IXAudio2MasteringVoice* g_master = nullptr;
    bool g_xaFailed = false;
    struct Voice { IXAudio2SourceVoice* v = nullptr; std::string alias; bool loop = false, keep = false; std::shared_ptr<Pcm> pcm; };   // keep: survives an anim change
    std::vector<Voice> g_voices;
    struct Pending { std::string alias; std::string path; Alias def; bool loop = false, keep = false; ULONGLONG due = 0, until = 0; int depth = 0; };
    std::vector<Pending> g_pending;
    std::mt19937 g_rng{std::random_device{}()};

    bool StartAudio()
    {
        if (g_xa) return true;
        if (g_xaFailed) return false;
        g_xaFailed = true;
        HMODULE m = LoadLibraryW(L"xaudio2_9.dll");
        using CreateFn = HRESULT (WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);
        auto create = m ? reinterpret_cast<CreateFn>(GetProcAddress(m, "XAudio2Create")) : nullptr;
        if (!create || FAILED(create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) || !g_xa) { rp::Log("notesounds: XAudio2 not available"); g_xa = nullptr; return false; }
        if (FAILED(g_xa->CreateMasteringVoice(&g_master)) || !g_master) { rp::Log("notesounds: no audio output device"); g_xa->Release(); g_xa = nullptr; return false; }
        g_xaFailed = false;
        return true;
    }

    void StopWhere(bool (*match)(const Voice&, const std::string&), const std::string& arg)
    {
        for (auto it = g_voices.begin(); it != g_voices.end();)
        {
            if (match(*it, arg)) { it->v->DestroyVoice(); it = g_voices.erase(it); }
            else ++it;
        }
    }
    void StopAll() { StopWhere([](const Voice&, const std::string&) { return true; }, {}); g_pending.clear(); }
    void StopAnimVoices()
    {
        StopWhere([](const Voice& v, const std::string&) { return !v.keep; }, {});
        g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(), [](const Pending& p) { return !p.keep; }), g_pending.end());
    }
    void StopLoops() { StopWhere([](const Voice& v, const std::string&) { return v.loop; }, {}); }
    void StopAlias(const std::string& alias)
    {
        StopWhere([](const Voice& v, const std::string& a) { return v.alias == a; }, Lower(alias));
        g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(), [&](const Pending& p) { return p.alias == Lower(alias); }), g_pending.end());
    }

    float Gain(float percent)
    {
        float v = std::clamp(percent, 0.0f, 100.0f);
        if (v <= 0) return 0;
        float g = std::pow(10.0f, (v - 100.0f) * 0.05f);
        return g < 0.0000152879f ? 0 : g;
    }

    float Uniform(float a, float b) { if (b < a) std::swap(a, b); return a == b ? a : std::uniform_real_distribution<float>(a, b)(g_rng); }

    // <root>\sound_assets\<FileSpec>, a folder gives one of its files
    std::string FileOf(const Alias& a)
    {
        if (a.fileSpec.empty()) return {};
        fs::path p = fs::path(Widen(g_root)) / L"sound_assets" / Widen(a.fileSpec);
        std::error_code ec;
        if (fs::is_directory(p, ec))
        {
            std::vector<fs::path> files;
            for (fs::directory_iterator it(p, ec), end; it != end; it.increment(ec))
            {
                if (ec) break;
                std::string ext = Lower(it->path().extension().string());
                if (it->is_regular_file(ec) && (ext == ".wav" || ext == ".flac")) files.push_back(it->path());
            }
            if (files.empty()) return {};
            std::sort(files.begin(), files.end());
            return Narrow(files[std::uniform_int_distribution<size_t>(0, files.size() - 1)(g_rng)].wstring());
        }
        if (!fs::exists(p, ec) && p.extension().empty()) p += L".wav";
        return fs::exists(p, ec) ? Narrow(p.wstring()) : std::string();
    }

    void Start(const Pending& p, const std::shared_ptr<Pcm>& pcm)
    {
        if (!pcm || pcm->data.empty() || !StartAudio()) return;
        IXAudio2SourceVoice* v = nullptr;
        if (FAILED(g_xa->CreateSourceVoice(&v, pcm->Format(), 0, XAUDIO2_DEFAULT_FREQ_RATIO)) || !v) { rp::Log("notesounds: voice for %s refused", p.path.c_str()); return; }
        XAUDIO2_BUFFER b{};
        b.AudioBytes = UINT32(pcm->data.size()); b.pAudioData = pcm->data.data(); b.Flags = XAUDIO2_END_OF_STREAM;
        if (p.loop) b.LoopCount = XAUDIO2_LOOP_INFINITE;
        if (FAILED(v->SubmitSourceBuffer(&b))) { v->DestroyVoice(); return; }
        v->SetVolume(Gain(Uniform(p.def.volMin, p.def.volMax)) * g_volume);
        v->SetFrequencyRatio(std::clamp(std::pow(2.0f, Uniform(p.def.pitchMin, p.def.pitchMax) / 1200.0f), 0.01f, XAUDIO2_DEFAULT_FREQ_RATIO));
        v->Start();
        Voice voice; voice.v = v; voice.alias = p.alias; voice.loop = p.loop; voice.keep = p.keep; voice.pcm = pcm;
        g_voices.push_back(std::move(voice));
        ++g_played;
    }

    // once per alias and session in the log and APE's Output pane
    std::unordered_set<std::string> g_told;
    void Missing(const std::string& alias, const std::string& why)
    {
        ++g_missing;
        g_lastMissing = alias + ": " + why;
        if (!g_told.insert(Lower(alias)).second) return;
        rp::Log("notesounds: %s", g_lastMissing.c_str());
        if (rp::Ape()) rp::Ape()->Print(rp::self, ("Notetrack sound " + g_lastMissing).c_str());
    }

    // queues an alias: one of its rows at random, its Secondary too
    void Play(const std::string& alias, bool loop, int depth = 0, bool keep = false)
    {
        if (alias.empty() || depth > 3) return;
        std::string key = Lower(alias);
        if (loop) for (const Voice& v : g_voices) if (v.loop && v.alias == key) return;
        std::shared_ptr<Index> idx;
        { std::lock_guard<std::mutex> l(g_mutex); idx = g_index; }
        ULONGLONG now = GetTickCount64();
        if (!idx)
        {
            Post(Job{0});
            Pending p; p.alias = key; p.loop = loop; p.keep = keep; p.due = now; p.until = now + 3000; p.depth = depth;
            g_pending.push_back(std::move(p));
            return;
        }
        auto it = idx->aliases.find(key);
        if (it == idx->aliases.end() || it->second.empty()) { Missing(alias, "not in share\\raw\\sound\\aliases (a stock alias has no source files)"); return; }
        const Alias& a = it->second[std::uniform_int_distribution<size_t>(0, it->second.size() - 1)(g_rng)];
        std::string path = FileOf(a);
        if (path.empty()) Missing(alias, a.fileSpec.empty() ? std::string("the alias has no FileSpec") : "sound_assets\\" + a.fileSpec + " not found");
        else
        {
            std::error_code ec;
            const int64_t stamp = int64_t(fs::last_write_time(fs::path(Widen(path)), ec).time_since_epoch().count());
            {
                std::lock_guard<std::mutex> l(g_mutex);
                auto f = g_pcm.find(Lower(path));
                if (f != g_pcm.end() && f->second->stamp != stamp) { g_pcmBytes -= f->second->data.size(); g_pcm.erase(f); }
            }
            Pending p; p.alias = key; p.path = path; p.def = a; p.loop = loop || a.looping;
            p.due = now + ULONGLONG(std::max(0, a.startDelay)); p.until = p.due + 400; p.depth = depth; p.keep = keep;
            Post(Job{1, path});
            g_pending.push_back(std::move(p));
        }
        if (!a.secondary.empty()) Play(a.secondary, false, depth + 1, keep);
    }

    // pending sounds whose file is decoded and whose start delay is over, finished voices freed
    void Pump()
    {
        ULONGLONG now = GetTickCount64();
        for (size_t i = 0; i < g_pending.size();)
        {
            Pending p = g_pending[i];
            if (p.path.empty())
            {
                bool ready; { std::lock_guard<std::mutex> l(g_mutex); ready = g_index != nullptr; }
                if (ready) { g_pending.erase(g_pending.begin() + i); Play(p.alias, p.loop, p.depth, p.keep); continue; }
                if (now > p.until) { g_pending.erase(g_pending.begin() + i); continue; }
                ++i; continue;
            }
            std::shared_ptr<Pcm> pcm; bool known = false;
            { std::lock_guard<std::mutex> l(g_mutex); auto f = g_pcm.find(Lower(p.path)); if (f != g_pcm.end()) { known = true; pcm = f->second; } }
            if (known && now >= p.due) { g_pending.erase(g_pending.begin() + i); if (now <= p.until + 600) Start(p, pcm); continue; }
            if (!known && now > p.until + 600) { g_pending.erase(g_pending.begin() + i); continue; }
            ++i;
        }
        for (auto it = g_voices.begin(); it != g_voices.end();)
        {
            XAUDIO2_VOICE_STATE st{}; it->v->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (!it->loop && st.BuffersQueued == 0) { it->v->DestroyVoice(); it = g_voices.erase(it); }
            else ++it;
        }
    }

    // ---- reading the preview (SEH: APE's memory) ----
    struct Live { bool hasAnim = false, paused = true; void* anim = nullptr; float t = 0; int frames = 0; double time = 0; };

    bool ReadLive(Live& out)
    {
        __try
        {
            uint8_t* o = g_opts ? *g_opts : nullptr;
            if (!o) return false;
            out.hasAnim = o[kOptsHasAnim] != 0; out.paused = o[kOptsPaused] != 0;
            out.time = *reinterpret_cast<double*>(o + kOptsTime);
            out.anim = nullptr;
            for (int i = 0; i < kSlotCount; ++i)
            {
                uint8_t* obj = *reinterpret_cast<uint8_t**>(o + kSlots + size_t(i) * kSlotStride);
                if (!obj) continue;
                uint8_t* anim = *reinterpret_cast<uint8_t**>(obj + kObjAnim);
                if (!anim) continue;
                out.anim = anim; out.t = *reinterpret_cast<float*>(obj + kObjT); out.frames = *reinterpret_cast<int*>(anim + kAnimFrames);
                break;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    struct RawNote { int kind; int frame; char action[64]; char p1[192]; };
    RawNote g_raw[256];

    void CopyStr(const uint8_t* s, char* out, size_t cap)
    {
        size_t size = *reinterpret_cast<const size_t*>(s + 16), capa = *reinterpret_cast<const size_t*>(s + 24);
        const char* p = capa >= 16 ? *reinterpret_cast<const char* const*>(s) : reinterpret_cast<const char*>(s);
        size_t n = std::min(size, cap - 1);
        memcpy(out, p, n); out[n] = 0;
    }

    int ReadNotes(void* anim)
    {
        int n = 0;
        __try
        {
            const uint8_t* a = static_cast<const uint8_t*>(anim);
            for (int k = 0; k < 3; ++k)
            {
                const uint8_t* b = *reinterpret_cast<const uint8_t* const*>(a + kAnimNotes[k]);
                const uint8_t* e = *reinterpret_cast<const uint8_t* const*>(a + kAnimNotes[k] + 8);
                if (!b || e < b || size_t(e - b) % kNoteSize || size_t(e - b) / kNoteSize > 128) continue;
                for (const uint8_t* r = b; r < e && n < 256; r += kNoteSize, ++n)
                {
                    g_raw[n].kind = k; g_raw[n].frame = *reinterpret_cast<const int*>(r + 4);
                    CopyStr(r + 8, g_raw[n].action, sizeof g_raw[n].action);
                    CopyStr(r + 40, g_raw[n].p1, sizeof g_raw[n].p1);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
        return n;
    }

    // ---- notes of the current anim ----
    void Fire(const Note& n)
    {
        ++g_fired;
        std::string a = Lower(n.action);
        if (n.exported || a == "sound" || a == "2d sound" || a == "vox") Play(n.alias, false);
        else if (a == "start looping sound") Play(n.alias, true);
        else if (a == "stop looping sound" || a == "stop sound") StopAlias(n.alias);
    }

    bool IsSoundAction(const std::string& action)
    {
        std::string a = Lower(action);
        return a == "sound" || a == "2d sound" || a == "vox" || a == "start looping sound" || a == "stop looping sound" || a == "stop sound";
    }

    void Preload()
    {
        std::shared_ptr<Index> idx; { std::lock_guard<std::mutex> l(g_mutex); idx = g_index; }
        if (!idx) return;
        for (const Note& n : g_notes)
        {
            auto it = idx->aliases.find(Lower(n.alias));
            if (it == idx->aliases.end()) continue;
            for (const Alias& a : it->second)
            {
                if (a.fileSpec.empty()) continue;
                fs::path p = fs::path(Widen(g_root)) / L"sound_assets" / Widen(a.fileSpec);
                std::error_code ec;
                if (!fs::is_directory(p, ec)) { std::string f = FileOf(a); if (!f.empty()) Post(Job{1, f}); }
            }
        }
    }

    std::string AssetName()
    {
        if (!rp::Ape()) return {};
        ModToolsApeAsset sel[4] = {};
        if (rp::Ape()->SelectedAssets(rp::self, sel, 4)) return sel[0].name;
        ModToolsApeAsset cur = {};
        return rp::Ape()->CurrentAsset(rp::self, &cur) ? cur.name : std::string();
    }

    // exported notetracks sit in the custom list with their name as the action and the frame relative to 0
    void LoadNotes(void* anim)
    {
        g_notes.clear();
        g_asset.clear();
        if (!anim) return;
        g_asset = AssetName();
        int n = ReadNotes(anim);
        for (int i = 0; i < n; ++i)
        {
            Note note; note.kind = g_raw[i].kind; note.frame = g_raw[i].frame; note.action = g_raw[i].action;
            note.alias = SoundAlias(note.action);
            if (!note.alias.empty()) { if (!g_exported) continue; note.exported = true; }
            else if (g_gdt && IsSoundAction(note.action)) note.alias = Trim(g_raw[i].p1);
            if (!note.alias.empty()) g_notes.push_back(std::move(note));
        }
        rp::Log("notesounds: %s: %d notes, %zu with sound", g_asset.c_str(), n, g_notes.size());
        Post(Job{0});
        Preload();
    }

    void FireRange(double from, double to, bool inclusiveFrom)
    {
        for (const Note& n : g_notes)
            if (n.kind == kCustom && (inclusiveFrom ? n.frame >= from : n.frame > from) && n.frame <= to) Fire(n);
    }
    void FireKind(int kind) { for (const Note& n : g_notes) if (n.kind == kind) Fire(n); }
}

void Install()
{
    g_on = rp::ConfigGetBool("notesounds", "enabled", true);
    g_exported = rp::ConfigGetBool("notesounds", "exported", true);
    g_gdt = rp::ConfigGetBool("notesounds", "gdt", true);
    g_volume = std::clamp(float(atof(rp::ConfigGet("notesounds", "volume", "1").c_str())), 0.0f, 4.0f);
    if (rp::Ape()) g_opts = static_cast<uint8_t**>(rp::Ape()->ApeAddress(rp::self, kOpts));
    wchar_t exe[MAX_PATH] = {}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_root = Narrow(fs::path(exe).parent_path().parent_path().wstring());
    rp::Log("notesounds: %s, root %s%s", g_on ? "on" : "off", g_root.c_str(), g_opts ? "" : ", preview globals NOT found");
}

void Tick()
{
    if (!g_opts) return;
    // an edited alias csv is picked up within a second: the worker compares the csv stamps and rebuilds only on a change
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastStampCheck >= 1000)
    {
        g_lastStampCheck = now;
        bool have, rebuilt;
        { std::lock_guard<std::mutex> l(g_mutex); have = g_index != nullptr; rebuilt = g_rebuilt; g_rebuilt = false; }
        if (have) Post(Job{0});
        if (rebuilt) g_told.clear();
    }
    Pump();
    if (!g_on) return;
    Live live;
    if (!ReadLive(live)) return;
    void* anim = live.hasAnim ? live.anim : nullptr;
    if (anim != g_anim)
    {
        StopAnimVoices();
        g_anim = anim; g_prevValid = false; g_wasPlaying = false;
        LoadNotes(anim);
        TrimPcm();
    }
    if (!anim || live.frames <= 0) return;
    g_frames = live.frames;
    const double pos = double(std::clamp(live.t, 0.0f, 1.0f)) * live.frames;
    const bool playing = !live.paused;
    if (!playing)
    {
        if (g_wasPlaying) StopAll();
        if (g_prevValid && pos > g_prev && pos - g_prev <= 1.5) FireRange(g_prev, pos, false);   // the one-frame step button
        g_prev = pos; g_prevValid = true; g_wasPlaying = false;
        return;
    }
    if (!g_wasPlaying || !g_prevValid)
    {
        if (pos < 1.0) { FireKind(kStartup); FireRange(0, pos, true); }
        g_prev = pos; g_prevValid = true; g_wasPlaying = true;
        return;
    }
    if (pos >= g_prev) { if (pos - g_prev < live.frames * 0.5) FireRange(g_prev, pos, false); }
    else if (g_prev - pos > live.frames * 0.5)
    {
        FireRange(g_prev, double(live.frames), false);
        FireKind(kShutdown);
        FireKind(kStartup);
        FireRange(0, pos, true);
    }
    g_prev = pos;
}

void PlayAlias(const std::string& alias) { if (g_opts) Play(alias, false, 0, true); }

int SoundNoteCount() { return g_anim ? int(g_notes.size()) : -1; }

void Shutdown()
{
    StopAll();
    {
        std::lock_guard<std::mutex> l(g_mutex);
        g_stop = true; g_jobs.clear();
    }
    g_cv.notify_all();
    if (g_thread.joinable()) g_thread.join();
    if (g_master) { g_master->DestroyVoice(); g_master = nullptr; }
    if (g_xa) { g_xa->Release(); g_xa = nullptr; }
    std::lock_guard<std::mutex> l(g_mutex);
    g_pcm.clear(); g_pcmBytes = 0; g_index.reset();
}

bool Command(const std::string& c, std::string& reply)
{
    if (c.rfind("notesounds", 0) != 0) return false;
    std::string arg = Trim(c.substr(10));
    char buf[512];
    if (arg == "on" || arg == "off")
    {
        g_on = arg == "on"; rp::ConfigSet("notesounds", "enabled", g_on ? "1" : "0");
        if (!g_on) StopAll();
        g_anim = nullptr;
        reply = std::string("notesounds ") + (g_on ? "on" : "off");
        return true;
    }
    if (arg == "stop") { StopAll(); reply = "stopped"; return true; }
    if (arg == "reindex") { { std::lock_guard<std::mutex> l(g_mutex); g_index.reset(); } Post(Job{0}); reply = "rebuilding the alias index"; return true; }
    if (arg.rfind("play ", 0) == 0) { Play(Trim(arg.substr(5)), false); reply = "queued " + Trim(arg.substr(5)); return true; }
    if (arg == "dump")
    {
        Live live; bool ok = ReadLive(live);
        snprintf(buf, sizeof buf, "read %d hasAnim %d paused %d time %.4f anim %p t %.5f frames %d pos %.2f\n", ok, live.hasAnim, live.paused, live.time, live.anim,
                 live.t, live.frames, double(live.t) * live.frames);
        reply = buf;
        if (live.anim)
        {
            int n = ReadNotes(live.anim);
            for (int i = 0; i < n; ++i)
            {
                snprintf(buf, sizeof buf, "  raw %d frame %d action '%s' p1 '%s'\n", g_raw[i].kind, g_raw[i].frame, g_raw[i].action, g_raw[i].p1);
                reply += buf;
            }
        }
        for (const Note& n : g_notes)
        {
            snprintf(buf, sizeof buf, "  note %s kind %d frame %.0f %s -> %s\n", n.exported ? "file" : "gdt", n.kind, n.frame, n.action.c_str(), n.alias.c_str());
            reply += buf;
        }
        return true;
    }
    std::lock_guard<std::mutex> l(g_mutex);
    snprintf(buf, sizeof buf, "notesounds %s (gdt %d, exported %d, volume %.2f); asset '%s' %zu sound notes, %d frames\n"
             "index: %s; decoded %zu files (%zu KB); voices %zu, pending %zu; fired %llu, played %llu, missing %llu%s%s",
             g_on ? "on" : "off", g_gdt, g_exported, g_volume, g_asset.c_str(), g_notes.size(), g_frames, g_index ? g_indexNote.c_str() : (g_indexing ? "building" : "not built"),
             g_pcm.size(), g_pcmBytes >> 10, g_voices.size(), g_pending.size(), (unsigned long long)g_fired, (unsigned long long)g_played,
             (unsigned long long)g_missing, g_lastMissing.empty() ? "" : ", last: ", g_lastMissing.c_str());
    reply = buf;
    return true;
}
}
