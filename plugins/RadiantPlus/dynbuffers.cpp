// Dynamic buffer twins: Map on one of Radiant's big dynamic vertex or index buffers returns a write-watched copy,
// Unmap copies the written pages to a default-usage twin through a small scratch buffer, and the bind calls swap the twin in.
#include "modtools_plugin.hpp"
#include "dynbuffers.h"
#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace br::dynbuffers
{
namespace
{
    const uintptr_t kDxContext = 0x145574210;   // ID3D11DeviceContext* (dx.context)
    enum { kSlots = 16, kWatchBatch = 4096 };
    const UINT   kMinBytes = 4u << 20;
    const size_t kPage = 4096;
    const size_t kMergeGap = 16 * kPage;        // two written runs this close go up as one
    const UINT   kScratchBytes = 1u << 20;
    const size_t kCursorSlack = 64 * 1024;
    const size_t kCursorWindow = 4u << 20;
    const uint64_t kFullLookMs = 2000;
    const uint64_t kCommitMargin = 4ull << 30;  // free commit that must stay after a copy is made

    // {6B1F3D52-9C0E-4E4B-A1B7-52D4F0C3A9E1}
    const GUID kTagGuid = { 0x6b1f3d52, 0x9c0e, 0x4e4b, { 0xa1, 0xb7, 0x52, 0xd4, 0xf0, 0xc3, 0xa9, 0xe1 } };

    using CreateBufferFn = HRESULT (STDMETHODCALLTYPE *)(ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
    using MapFn    = HRESULT (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
    using UnmapFn  = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
    using SetVBFn  = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);
    using SetIBFn  = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, ID3D11Buffer*, DXGI_FORMAT, UINT);

    struct Slot
    {
        ID3D11Buffer* dyn;      // Radiant's buffer, nullptr = free slot
        ID3D11Buffer* twin;     // what draws read, nullptr until the first map
        uint8_t*      mem;      // what Radiant writes into
        UINT size, bind;
        UINT high;              // end of the written part
        bool fresh;             // holds nothing yet, so a new copy needs no content
        bool gaveUp;            // no twin could be made, the buffer stays Radiant's own
        size_t cursor;          // end of the last written run
        uint64_t lastFull;      // tick of the last whole look
        bool mapped;
    };

    bool g_enabled = true;      // [performance] dynamic_buffers
    Slot g_slots[kSlots] = {};
    int  g_used = 0;            // slots [0, g_used) may be taken
    int  g_twins = 0;
    ID3D11DeviceContext** g_ctxGlobal = nullptr;
    ID3D11DeviceContext*  g_ctx = nullptr;
    ID3D11Device*         g_dev = nullptr;
    ID3D11Buffer*         g_scratch = nullptr;  // dynamic, carries one chunk to the GPU
    CreateBufferFn g_oCreateBuffer = nullptr;
    MapFn   g_oMap = nullptr;
    UnmapFn g_oUnmap = nullptr;
    SetVBFn g_oSetVB = nullptr;
    SetIBFn g_oSetIB = nullptr;
    bool g_tried = false, g_hooked = false;

    uint64_t g_maps = 0, g_uploads = 0, g_uploadBytes = 0, g_failed = 0, g_strays = 0;
    double g_workMs = 0;        // time spent finding and sending written pages
    double NowMs()
    {
        static LARGE_INTEGER freq = {};
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        return double(t.QuadPart) * 1000.0 / double(freq.QuadPart);
    }

    Slot* Find(const void* res)
    {
        for (int i = 0; i < g_used; ++i)
            if (g_slots[i].dyn == res) return &g_slots[i];
        return nullptr;
    }

    void FreeSlot(Slot* s)
    {
        if (s->twin) { s->twin->Release(); --g_twins; }
        if (s->mem) VirtualFree(s->mem, 0, MEM_RELEASE);
        *s = Slot{};
        while (g_used > 0 && !g_slots[g_used - 1].dyn) --g_used;
    }

    // Rides on Radiant's buffer as private data: its last Release is the buffer's destruction
    struct Tag final : IUnknown
    {
        LONG refs = 1;
        ID3D11Buffer* dyn = nullptr;
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
        {
            if (!out) return E_POINTER;
            if (iid == __uuidof(IUnknown)) { *out = this; AddRef(); return S_OK; }
            *out = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(InterlockedIncrement(&refs)); }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const LONG n = InterlockedDecrement(&refs);
            if (n == 0)
            {
                if (Slot* s = Find(dyn)) FreeSlot(s);
                delete this;
            }
            return ULONG(n);
        }
    };

    Slot* Note(ID3D11Buffer* buf, const D3D11_BUFFER_DESC& bd, bool fresh)
    {
        Slot* s = nullptr;
        for (int i = 0; i < kSlots && !s; ++i)
            if (!g_slots[i].dyn) { s = &g_slots[i]; if (i >= g_used) g_used = i + 1; }
        if (!s) return nullptr;
        *s = Slot{};
        s->dyn = buf; s->size = bd.ByteWidth; s->bind = bd.BindFlags; s->fresh = fresh;
        Tag* tag = new Tag;
        tag->dyn = buf;
        buf->SetPrivateDataInterface(kTagGuid, tag);
        tag->Release();
        return s;
    }

    bool Wanted(const D3D11_BUFFER_DESC& bd)
    {
        return bd.Usage == D3D11_USAGE_DYNAMIC && bd.ByteWidth >= kMinBytes && (bd.BindFlags == D3D11_BIND_VERTEX_BUFFER || bd.BindFlags == D3D11_BIND_INDEX_BUFFER);
    }

    // Puts `to` wherever `from` is bound to the input assembler
    void Rebind(ID3D11Buffer* from, ID3D11Buffer* to)
    {
        ID3D11Buffer* vb[8] = {}; UINT strides[8] = {}, offsets[8] = {};
        g_ctx->IAGetVertexBuffers(0, 8, vb, strides, offsets);
        for (UINT i = 0; i < 8; ++i)
        {
            if (vb[i] == from) g_oSetVB(g_ctx, i, 1, &to, &strides[i], &offsets[i]);
            if (vb[i]) vb[i]->Release();
        }
        ID3D11Buffer* ib = nullptr; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN; UINT off = 0;
        g_ctx->IAGetIndexBuffer(&ib, &fmt, &off);
        if (ib == from) g_oSetIB(g_ctx, to, fmt, off);
        if (ib) ib->Release();
    }

    // Gives the copy and the twin what a buffer from before the hooks holds, read back through a staging buffer
    bool Seed(Slot* s)
    {
        D3D11_BUFFER_DESC sd = {};
        sd.ByteWidth = s->size; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Buffer* staging = nullptr;
        if (FAILED(g_oCreateBuffer(g_dev, &sd, nullptr, &staging)) || !staging) return false;
        g_ctx->CopyResource(staging, s->dyn);
        D3D11_MAPPED_SUBRESOURCE map = {};
        const bool ok = SUCCEEDED(g_oMap(g_ctx, staging, 0, D3D11_MAP_READ, 0, &map)) && map.pData;
        if (ok)
        {
            memcpy(s->mem, map.pData, s->size);
            g_oUnmap(g_ctx, staging, 0);
            ResetWriteWatch(s->mem, s->size);
            g_ctx->CopyResource(s->twin, s->dyn);
            s->high = s->size;
        }
        staging->Release();
        return ok;
    }

    bool StartTwin(Slot* s)
    {
        if (!g_scratch)
        {
            D3D11_BUFFER_DESC sd = {};
            sd.ByteWidth = kScratchBytes; sd.Usage = D3D11_USAGE_DYNAMIC; sd.BindFlags = D3D11_BIND_VERTEX_BUFFER; sd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(g_oCreateBuffer(g_dev, &sd, nullptr, &g_scratch))) { g_scratch = nullptr; ++g_failed; return false; }
        }
        MEMORYSTATUSEX ms = {}; ms.dwLength = sizeof ms;
        if (GlobalMemoryStatusEx(&ms) && ms.ullAvailPageFile >= s->size + kCommitMargin)
            s->mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, s->size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE));
        if (s->mem)
        {
            D3D11_BUFFER_DESC bd = {};
            bd.ByteWidth = s->size; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = s->bind;
            if (FAILED(g_oCreateBuffer(g_dev, &bd, nullptr, &s->twin))) s->twin = nullptr;
        }
        if (!s->twin)
        {
            if (s->mem) { VirtualFree(s->mem, 0, MEM_RELEASE); s->mem = nullptr; }
            ++g_failed;
            rp::Log("dynbuf: no twin for the %u MB buffer %p (memory is short), it stays Radiant's own", s->size >> 20, static_cast<void*>(s->dyn));
            return false;
        }
        if (!s->fresh && !Seed(s))
        {
            s->twin->Release(); s->twin = nullptr;
            VirtualFree(s->mem, 0, MEM_RELEASE); s->mem = nullptr;
            ++g_failed;
            return false;
        }
        ++g_twins;
        Rebind(s->dyn, s->twin);
        rp::Log("dynbuf: %u MB %s buffer %p drawn from a twin", s->size >> 20, s->bind == D3D11_BIND_VERTEX_BUFFER ? "vertex" : "index", static_cast<void*>(s->dyn));
        return true;
    }

    // Copies [begin, end) of the copy into the twin on the GPU, in command order
    void Send(Slot* s, size_t begin, size_t end)
    {
        while (begin < end)
        {
            const UINT len = UINT(end - begin < kScratchBytes ? end - begin : kScratchBytes);
            D3D11_MAPPED_SUBRESOURCE map = {};
            if (FAILED(g_oMap(g_ctx, g_scratch, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)) || !map.pData) return;
            memcpy(map.pData, s->mem + begin, len);
            g_oUnmap(g_ctx, g_scratch, 0);
            const D3D11_BOX box = { 0, 0, 0, len, 1, 1 };
            g_ctx->CopySubresourceRegion(s->twin, 0, UINT(begin), 0, 0, g_scratch, 0, &box);
            ++g_uploads; g_uploadBytes += len;
            begin += len;
        }
    }

    // Sends the written pages of [from, to) to the twin, returns how many there were
    size_t SendWritten(Slot* s, size_t from, size_t to)
    {
        void* pages[kWatchBatch];
        size_t total = 0;
        for (;;)
        {
            ULONG_PTR count = kWatchBatch; DWORD gran = 0;
            if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, s->mem + from, to - from, pages, &count, &gran) != 0 || !count) return total;
            total += count;
            size_t begin = 0, end = 0;
            for (ULONG_PTR i = 0; i <= count; ++i)
            {
                const size_t at = i < count ? size_t(static_cast<uint8_t*>(pages[i]) - s->mem) : SIZE_MAX;
                if (end && at <= end + kMergeGap) { end = at + kPage; continue; }
                if (end)
                {
                    if (end > s->size) end = s->size;
                    Send(s, begin, end);
                    if (end > s->high) s->high = UINT(end);
                    s->cursor = end;
                }
                begin = at; end = at + kPage;
            }
            if (count < kWatchBatch) return total;
        }
    }

    // Radiant appends, so the written pages sit at the cursor. The whole buffer is looked at when they do not, and now and then.
    void Upload(Slot* s)
    {
        const double t0 = NowMs();
        const uint64_t now = GetTickCount64();
        size_t found = 0;
        size_t from = s->cursor > kCursorSlack ? s->cursor - kCursorSlack : 0;
        for (;;)
        {
            const size_t to = from + kCursorWindow < s->size ? from + kCursorWindow : s->size;
            const size_t n = SendWritten(s, from, to);
            found += n;
            if (!n || to == s->size || s->cursor + kPage < to) break;
            from = to;
        }
        if (!found || now - s->lastFull > kFullLookMs)
        {
            if (SendWritten(s, 0, s->size) && found) ++g_strays;
            s->lastFull = now;
        }
        g_workMs += NowMs() - t0;
    }

    HRESULT STDMETHODCALLTYPE hk_CreateBuffer(ID3D11Device* dev, const D3D11_BUFFER_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, ID3D11Buffer** out)
    {
        const HRESULT hr = g_oCreateBuffer(dev, desc, init, out);
        if (g_enabled && SUCCEEDED(hr) && desc && out && *out && !init && dev == g_dev && Wanted(*desc)) Note(*out, *desc, true);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hk_Map(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* out)
    {
        if (ctx != g_ctx || !out || (!g_used && !g_enabled)) return g_oMap(ctx, res, sub, type, flags, out);
        Slot* s = Find(res);
        if (!s && g_enabled && res && (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE_NO_OVERWRITE))
        {
            // a buffer from before the hooks
            D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
            res->GetType(&dim);
            if (dim == D3D11_RESOURCE_DIMENSION_BUFFER)
            {
                D3D11_BUFFER_DESC bd = {};
                static_cast<ID3D11Buffer*>(res)->GetDesc(&bd);
                if (Wanted(bd)) s = Note(static_cast<ID3D11Buffer*>(res), bd, false);
            }
        }
        if (!s) return g_oMap(ctx, res, sub, type, flags, out);
        if (!s->twin)
        {
            if (type == D3D11_MAP_WRITE_DISCARD) s->fresh = true;
            if (!g_enabled || s->gaveUp || !StartTwin(s))
            {
                if (g_enabled) s->gaveUp = true;
                s->fresh = false;
                return g_oMap(ctx, res, sub, type, flags, out);
            }
        }
        s->mapped = true;
        if (type == D3D11_MAP_WRITE_DISCARD) s->cursor = 0;
        ++g_maps;
        out->pData = s->mem; out->RowPitch = s->size; out->DepthPitch = s->size;
        return S_OK;
    }

    void STDMETHODCALLTYPE hk_Unmap(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub)
    {
        if (g_twins && ctx == g_ctx)
        {
            Slot* s = Find(res);
            if (s && s->mapped) { s->mapped = false; Upload(s); return; }
        }
        g_oUnmap(ctx, res, sub);
    }

    void STDMETHODCALLTYPE hk_SetVB(ID3D11DeviceContext* ctx, UINT start, UINT n, ID3D11Buffer* const* bufs, const UINT* strides, const UINT* offsets)
    {
        if (g_twins && ctx == g_ctx && bufs && n <= D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT)
        {
            for (UINT i = 0; i < n; ++i)
            {
                const Slot* s = bufs[i] ? Find(bufs[i]) : nullptr;
                if (!s || !s->twin) continue;
                ID3D11Buffer* swapped[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
                for (UINT k = 0; k < n; ++k)
                {
                    const Slot* t = bufs[k] ? Find(bufs[k]) : nullptr;
                    swapped[k] = t && t->twin ? t->twin : bufs[k];
                }
                g_oSetVB(ctx, start, n, swapped, strides, offsets);
                return;
            }
        }
        g_oSetVB(ctx, start, n, bufs, strides, offsets);
    }

    void STDMETHODCALLTYPE hk_SetIB(ID3D11DeviceContext* ctx, ID3D11Buffer* buf, DXGI_FORMAT fmt, UINT off)
    {
        if (g_twins && ctx == g_ctx && buf)
        {
            const Slot* s = Find(buf);
            if (s && s->twin) buf = s->twin;
        }
        g_oSetIB(ctx, buf, fmt, off);
    }

    // DXVK maps dynamic buffers straight into GPU memory
    bool NativeD3D11()
    {
        const HMODULE mod = GetModuleHandleW(L"d3d11.dll");
        wchar_t path[MAX_PATH] = {}, sys[MAX_PATH] = {};
        if (!mod || !GetModuleFileNameW(mod, path, MAX_PATH) || !GetSystemDirectoryW(sys, MAX_PATH)) return false;
        return _wcsnicmp(path, sys, wcslen(sys)) == 0;
    }

    // Radiant's own buffer gets the twin's content back
    void Restore(Slot* s)
    {
        if (!s->twin) return;
        D3D11_MAPPED_SUBRESOURCE map = {};
        if (SUCCEEDED(g_oMap(g_ctx, s->dyn, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)) && map.pData)
        {
            memcpy(map.pData, s->mem, s->high);
            g_oUnmap(g_ctx, s->dyn, 0);
        }
        Rebind(s->twin, s->dyn);
    }

    std::string Status()
    {
        char buf[600];
        int n = snprintf(buf, sizeof buf, "dynamic_buffers=%d hooked=%d twins=%d maps=%llu uploads=%llu (%.1f MB) failed=%llu strays=%llu work=%.0f ms",
                         g_enabled ? 1 : 0, g_hooked ? 1 : 0, g_twins, (unsigned long long)g_maps, (unsigned long long)g_uploads, double(g_uploadBytes) / 1048576.0, (unsigned long long)g_failed, (unsigned long long)g_strays, g_workMs);
        for (int i = 0; i < g_used && n > 0 && n < int(sizeof buf) - 64; ++i)
            if (g_slots[i].dyn)
                n += snprintf(buf + n, sizeof buf - n, "; %u MB %s %s, written to %u MB", g_slots[i].size >> 20, g_slots[i].bind == D3D11_BIND_VERTEX_BUFFER ? "vertex" : "index",
                              g_slots[i].twin ? "twin" : "own", g_slots[i].high >> 20);
        return buf;
    }
}

void Setup()
{
    // the headless light build draws nothing
    g_enabled = rp::ConfigGetBool("performance", "dynamic_buffers", true) && !wcsstr(GetCommandLineW(), L" -led");
    g_ctxGlobal = static_cast<ID3D11DeviceContext**>(rp::host->RadiantAddress(rp::self, kDxContext));
}

void Tick()
{
    if (g_tried || !g_enabled || !g_ctxGlobal || !*g_ctxGlobal) return;
    g_tried = true;
    if (!NativeD3D11()) { rp::Log("dynbuf: not Direct3D 11's own runtime, nothing to do"); return; }
    g_ctx = *g_ctxGlobal;
    g_ctx->GetDevice(&g_dev);
    if (!g_dev) return;
    void** cvt = *reinterpret_cast<void***>(g_ctx);
    void** dvt = *reinterpret_cast<void***>(g_dev);
    g_hooked = rp::host->Detour(rp::self, cvt[14], reinterpret_cast<void*>(&hk_Map), reinterpret_cast<void**>(&g_oMap))
            && rp::host->Detour(rp::self, cvt[15], reinterpret_cast<void*>(&hk_Unmap), reinterpret_cast<void**>(&g_oUnmap))
            && rp::host->Detour(rp::self, cvt[18], reinterpret_cast<void*>(&hk_SetVB), reinterpret_cast<void**>(&g_oSetVB))
            && rp::host->Detour(rp::self, cvt[19], reinterpret_cast<void*>(&hk_SetIB), reinterpret_cast<void**>(&g_oSetIB))
            && rp::host->Detour(rp::self, dvt[3], reinterpret_cast<void*>(&hk_CreateBuffer), reinterpret_cast<void**>(&g_oCreateBuffer));
    if (!g_hooked) { g_enabled = false; rp::Log("dynbuf: a detour is unavailable, off"); return; }
    rp::Log("dynbuf: on for dynamic vertex and index buffers of %u MB and up", kMinBytes >> 20);
}

void Shutdown()
{
    g_enabled = false;
    if (!g_hooked) return;
    for (int i = 0; i < kSlots; ++i)
    {
        Slot* s = &g_slots[i];
        if (!s->dyn) continue;
        Restore(s);
        ID3D11Buffer* dyn = s->dyn;
        FreeSlot(s);
        dyn->SetPrivateDataInterface(kTagGuid, nullptr);
    }
    g_used = 0;
    if (g_scratch) { g_scratch->Release(); g_scratch = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

bool Command(const std::string& c, std::string& reply)
{
    if (c.rfind("dynbuf", 0) != 0) return false;
    if (c == "dynbuf 0" || c == "dynbuf 1")
    {
        g_enabled = c.back() == '1';
        rp::ConfigSet("performance", "dynamic_buffers", g_enabled ? "1" : "0");
        if (g_enabled) g_tried = g_hooked;
        else if (g_hooked)
            for (int i = 0; i < kSlots; ++i)
                if (g_slots[i].dyn && g_slots[i].twin)
                {
                    Slot* s = &g_slots[i];
                    Restore(s);
                    s->twin->Release(); s->twin = nullptr; --g_twins;
                    VirtualFree(s->mem, 0, MEM_RELEASE); s->mem = nullptr;
                    s->fresh = false; s->mapped = false;
                }
    }
    else if (c != "dynbuf" && c != "dynbuf status") { reply = "dynbuf [0|1|status]"; return true; }
    reply = Status();
    return true;
}
}
