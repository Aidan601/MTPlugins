// Selected model outline. Radiant's selection pass (J) draws each selected model as a wireframe through Gfx. With the
// outline on (Alt+J, View > Toggle Model Outline) those draws fill a stencil mask instead, and one edge pass then draws
// the mask's border in the selection color, on top of the scene.
#include "modtools_plugin.hpp"
#include "seloutline.h"
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>

namespace br::seloutline
{
namespace
{
    // IDA addresses (no ASLR), resolved through host->RadiantAddress
    const uintptr_t kOutlineLambda = 0x1402AE780;   // Cam_Draw selection pass, per selected BrushInst: (lambda*, BrushInst**) -> bool
    const uintptr_t kModelWire     = 0x1411BB860;   // camera model wireframe: (model, int, RGBA bytes*, drawState*, lod, -1, flags, flags)
    const uintptr_t kDrawIndexed   = 0x14010A420;   // Gfx DrawIndexed wrapper: (indexCount, startIndex, baseVertex)
    const uintptr_t kDxContext     = 0x145574210;   // ID3D11DeviceContext*
    const uintptr_t kNoSelOutline  = 0x1453A1830;   // int, nonzero = J turned the selection pass off

    using LambdaFn = uint8_t (*)(void* lambda, void* inst);
    using ModelWireFn = uintptr_t (*)(uintptr_t, uintptr_t, const uint8_t*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
    using DrawFn = int64_t (*)(uint32_t indexCount, uint32_t startIndex, int32_t baseVertex);
    LambdaFn    g_oLambda = nullptr;
    ModelWireFn g_oModelWire = nullptr;
    DrawFn      g_oDraw = nullptr;

    bool g_enabled = true;          // [selection] model_outline, Alt+J flips and saves it
    int  g_px = 2;                  // [selection] model_outline_px
    ModToolsWidget g_menuAction = nullptr;   // View > Toggle Model Outline
    ModToolsWidget g_viewMenu = nullptr;
    const char* const kMenuText = "Toggle Model Outline";

    // Qt 5.3 exports, MS x64 ABI: QString returned through a hidden pointer
    struct QArrayDataRaw { int ref; int size; unsigned alloc; ptrdiff_t offset; };
    struct
    {
        void (*deleteLater)(void*) = nullptr;
        void (*removeAction)(void* widget, void* action) = nullptr;
        void** (*text)(const void* action, void** out) = nullptr;
        void (*qsDtor)(void**) = nullptr;
    } qt;
    bool g_hooked = false;
    ID3D11DeviceContext** g_ctxGlobal = nullptr;
    const int* g_noOutline = nullptr;

    int  g_inPass = 0;              // inside the selection pass lambda
    bool g_capture = false;         // inside a model wireframe call of that pass
    float g_color[4] = { 1, 0, 0, 1 };

    // GPU objects, created on first use
    ID3D11Device*             g_dev = nullptr;
    ID3D11Texture2D*          g_mask = nullptr;
    ID3D11DepthStencilView*   g_maskDsv = nullptr;
    ID3D11ShaderResourceView* g_maskSrv = nullptr;
    UINT                      g_maskW = 0, g_maskH = 0;
    ID3D11DepthStencilState*  g_dsWrite = nullptr;  // stencil := 1, no depth
    ID3D11DepthStencilState*  g_dsOff = nullptr;
    ID3D11RasterizerState*    g_rsSolid = nullptr;
    ID3D11BlendState*         g_blend = nullptr;
    ID3D11VertexShader*       g_vs = nullptr;
    ID3D11PixelShader*        g_ps = nullptr;
    ID3D11Buffer*             g_cb = nullptr;
    bool                      g_gpuFailed = false;

    // the mask waiting for its edge pass
    bool                      g_pending = false;
    ID3D11RenderTargetView*   g_target = nullptr;   // AddRef'd
    float                     g_pendingColor[4] = {};
    unsigned g_masks = 0, g_edges = 0, g_fallbacks = 0;

    const char* kVs =
        "float4 main(uint id : SV_VertexID) : SV_Position {\n"
        "  float2 uv = float2((id << 1) & 2, id & 2);\n"
        "  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
        "}\n";
    const char* kPs =
        "Texture2D<uint2> m : register(t0);\n"
        "cbuffer C : register(b0) { float4 color; int r; int3 pad; };\n"
        "float4 main(float4 p : SV_Position) : SV_Target {\n"
        "  int2 c = int2(p.xy);\n"
        "  if (m.Load(int3(c, 0)).y != 0) discard;\n"
        "  bool edge = false;\n"
        "  [loop] for (int y = -r; y <= r; ++y)\n"
        "    [loop] for (int x = -r; x <= r; ++x)\n"
        "      if (x * x + y * y <= r * r + 1 && m.Load(int3(c + int2(x, y), 0)).y != 0) edge = true;\n"
        "  if (!edge) discard;\n"
        "  return color;\n"
        "}\n";

    template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

    using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    D3DCompileFn Compiler()
    {
        static D3DCompileFn fn = nullptr;
        if (fn) return fn;
        for (const wchar_t* name : { L"d3dcompiler_47.dll", L"D3DCOMPILER_47.dll", L"d3dcompiler_46.dll" })
        {
            HMODULE m = GetModuleHandleW(name);
            if (!m) m = LoadLibraryW(name);
            if (m && (fn = reinterpret_cast<D3DCompileFn>(GetProcAddress(m, "D3DCompile")))) return fn;
        }
        return nullptr;
    }

    ID3DBlob* Compile(const char* src, const char* profile)
    {
        D3DCompileFn compile = Compiler();
        if (!compile) return nullptr;
        ID3DBlob* code = nullptr; ID3DBlob* errors = nullptr;
        const HRESULT hr = compile(src, strlen(src), "seloutline", nullptr, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(hr)) rp::Log("seloutline: %s compile failed 0x%08X %s", profile, unsigned(hr), errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        SafeRelease(errors);
        return SUCCEEDED(hr) ? code : nullptr;
    }

    bool EnsureGpu(ID3D11DeviceContext* ctx)
    {
        if (g_ps) return true;
        if (g_gpuFailed) return false;
        g_gpuFailed = true;
        ctx->GetDevice(&g_dev);
        if (!g_dev) return false;
        ID3DBlob* vs = Compile(kVs, "vs_5_0");
        ID3DBlob* ps = Compile(kPs, "ps_5_0");
        if (vs) g_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs);
        if (ps) g_dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps);
        SafeRelease(vs); SafeRelease(ps);

        D3D11_DEPTH_STENCIL_DESC ds = {};
        ds.DepthEnable = FALSE;
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
        g_dev->CreateDepthStencilState(&ds, &g_dsOff);
        ds.StencilEnable = TRUE;
        ds.StencilReadMask = ds.StencilWriteMask = 0xFF;
        ds.FrontFace = { D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS };
        ds.BackFace = ds.FrontFace;
        g_dev->CreateDepthStencilState(&ds, &g_dsWrite);

        D3D11_RASTERIZER_DESC rs = {};
        rs.FillMode = D3D11_FILL_SOLID;
        rs.CullMode = D3D11_CULL_NONE;
        rs.DepthClipEnable = TRUE;
        g_dev->CreateRasterizerState(&rs, &g_rsSolid);

        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        g_dev->CreateBlendState(&bd, &g_blend);

        D3D11_BUFFER_DESC cb = {};
        cb.ByteWidth = 32;
        cb.Usage = D3D11_USAGE_DEFAULT;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        g_dev->CreateBuffer(&cb, nullptr, &g_cb);

        if (!g_vs || !g_ps || !g_dsOff || !g_dsWrite || !g_rsSolid || !g_blend || !g_cb)
        {
            rp::Log("seloutline: GPU setup failed, the stock wireframe stays");
            SafeRelease(g_ps);
            return false;
        }
        g_gpuFailed = false;
        return true;
    }

    bool EnsureMask(UINT w, UINT h)
    {
        if (g_mask && g_maskW == w && g_maskH == h) return true;
        SafeRelease(g_maskSrv); SafeRelease(g_maskDsv); SafeRelease(g_mask);
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R24G8_TYPELESS;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_mask))) return false;
        D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
        dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        if (FAILED(g_dev->CreateDepthStencilView(g_mask, &dv, &g_maskDsv)) || FAILED(g_dev->CreateShaderResourceView(g_mask, &sv, &g_maskSrv)))
        {
            SafeRelease(g_maskSrv); SafeRelease(g_maskDsv); SafeRelease(g_mask);
            return false;
        }
        g_maskW = w; g_maskH = h;
        return true;
    }

    // ---- edge pass ----
    void Composite(ID3D11DeviceContext* ctx)
    {
        if (!g_pending) return;
        g_pending = false;
        ID3D11RenderTargetView* target = g_target;
        g_target = nullptr;
        if (!target) return;

        ID3D11RenderTargetView* oRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* oDsv = nullptr;
        ID3D11BlendState* oBlend = nullptr; FLOAT oFactor[4]; UINT oSampleMask = 0;
        ID3D11DepthStencilState* oDs = nullptr; UINT oRef = 0;
        ID3D11RasterizerState* oRs = nullptr;
        UINT oVpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; D3D11_VIEWPORT oVp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        ID3D11VertexShader* oVs = nullptr; ID3D11PixelShader* oPs = nullptr; ID3D11GeometryShader* oGs = nullptr;
        ID3D11HullShader* oHs = nullptr; ID3D11DomainShader* oDsh = nullptr;
        ID3D11InputLayout* oLayout = nullptr; D3D11_PRIMITIVE_TOPOLOGY oTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        ID3D11ShaderResourceView* oSrv = nullptr; ID3D11Buffer* oCb = nullptr;

        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oRtv, &oDsv);
        ctx->OMGetBlendState(&oBlend, oFactor, &oSampleMask);
        ctx->OMGetDepthStencilState(&oDs, &oRef);
        ctx->RSGetState(&oRs);
        ctx->RSGetViewports(&oVpCount, oVp);
        ctx->VSGetShader(&oVs, nullptr, nullptr);
        ctx->PSGetShader(&oPs, nullptr, nullptr);
        ctx->GSGetShader(&oGs, nullptr, nullptr);
        ctx->HSGetShader(&oHs, nullptr, nullptr);
        ctx->DSGetShader(&oDsh, nullptr, nullptr);
        ctx->IAGetInputLayout(&oLayout);
        ctx->IAGetPrimitiveTopology(&oTopo);
        ctx->PSGetShaderResources(0, 1, &oSrv);
        ctx->PSGetConstantBuffers(0, 1, &oCb);

        struct { float color[4]; int r; int pad[3]; } cb = { { g_pendingColor[0], g_pendingColor[1], g_pendingColor[2], g_pendingColor[3] }, g_px, {} };
        ctx->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);
        ctx->OMSetRenderTargets(1, &target, nullptr);
        const FLOAT zero[4] = {};
        ctx->OMSetBlendState(g_blend, zero, 0xFFFFFFFF);
        ctx->OMSetDepthStencilState(g_dsOff, 0);
        ctx->RSSetState(g_rsSolid);
        const D3D11_VIEWPORT vp = { 0, 0, float(g_maskW), float(g_maskH), 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ctx->VSSetShader(g_vs, nullptr, 0);
        ctx->PSSetShader(g_ps, nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->PSSetShaderResources(0, 1, &g_maskSrv);
        ctx->PSSetConstantBuffers(0, 1, &g_cb);
        ctx->Draw(3, 0);
        ++g_edges;

        ctx->PSSetConstantBuffers(0, 1, &oCb);
        ctx->PSSetShaderResources(0, 1, &oSrv);
        ctx->IASetPrimitiveTopology(oTopo);
        ctx->IASetInputLayout(oLayout);
        ctx->DSSetShader(oDsh, nullptr, 0);
        ctx->HSSetShader(oHs, nullptr, 0);
        ctx->GSSetShader(oGs, nullptr, 0);
        ctx->PSSetShader(oPs, nullptr, 0);
        ctx->VSSetShader(oVs, nullptr, 0);
        ctx->RSSetViewports(oVpCount, oVp);
        ctx->RSSetState(oRs);
        ctx->OMSetDepthStencilState(oDs, oRef);
        ctx->OMSetBlendState(oBlend, oFactor, oSampleMask);
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oRtv, oDsv);
        for (ID3D11RenderTargetView* v : oRtv) SafeRelease(v);
        SafeRelease(oDsv); SafeRelease(oBlend); SafeRelease(oDs); SafeRelease(oRs);
        SafeRelease(oVs); SafeRelease(oPs); SafeRelease(oGs); SafeRelease(oHs); SafeRelease(oDsh);
        SafeRelease(oLayout); SafeRelease(oSrv); SafeRelease(oCb);
        target->Release();
    }

    // ---- mask draw: the model's own vertex stage, no pixel shader, stencil only ----
    bool MaskDraw(ID3D11DeviceContext* ctx, uint32_t indexCount, uint32_t startIndex, int32_t baseVertex)
    {
        ID3D11RenderTargetView* rtv = nullptr;
        ctx->OMGetRenderTargets(1, &rtv, nullptr);
        if (!rtv) return false;
        UINT w = 0, h = 0;
        {
            ID3D11Resource* res = nullptr;
            rtv->GetResource(&res);
            ID3D11Texture2D* tex = nullptr;
            if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))))
            {
                D3D11_TEXTURE2D_DESC td; tex->GetDesc(&td);
                w = td.Width; h = td.Height;
                tex->Release();
            }
            SafeRelease(res);
        }
        if (g_pending && (g_target != rtv || w != g_maskW || h != g_maskH)) Composite(ctx);
        if (!w || !h || !EnsureMask(w, h)) { rtv->Release(); return false; }
        if (!g_pending)
        {
            ctx->ClearDepthStencilView(g_maskDsv, D3D11_CLEAR_STENCIL, 1.0f, 0);
            g_pending = true;
            g_target = rtv;     // keeps the reference
            memcpy(g_pendingColor, g_color, sizeof g_color);
        }
        else rtv->Release();

        ID3D11RenderTargetView* oRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* oDsv = nullptr;
        ID3D11DepthStencilState* oDs = nullptr; UINT oRef = 0;
        ID3D11RasterizerState* oRs = nullptr;
        ID3D11PixelShader* oPs = nullptr;
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oRtv, &oDsv);
        ctx->OMGetDepthStencilState(&oDs, &oRef);
        ctx->RSGetState(&oRs);
        ctx->PSGetShader(&oPs, nullptr, nullptr);

        ctx->OMSetRenderTargets(0, nullptr, g_maskDsv);
        ctx->OMSetDepthStencilState(g_dsWrite, 1);
        ctx->RSSetState(g_rsSolid);
        ctx->PSSetShader(nullptr, nullptr, 0);
        g_oDraw(indexCount, startIndex, baseVertex);
        ++g_masks;

        ctx->PSSetShader(oPs, nullptr, 0);
        ctx->RSSetState(oRs);
        ctx->OMSetDepthStencilState(oDs, oRef);
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oRtv, oDsv);
        for (ID3D11RenderTargetView* v : oRtv) SafeRelease(v);
        SafeRelease(oDsv); SafeRelease(oDs); SafeRelease(oRs); SafeRelease(oPs);
        return true;
    }

    ID3D11DeviceContext* Context() { return g_ctxGlobal ? *g_ctxGlobal : nullptr; }

    bool Active() { return g_enabled && *g_noOutline == 0; }

    void SyncMenu()
    {
        if (g_menuAction && rp::HasUi(&rp::host->ui->SetActionChecked)) rp::host->ui->SetActionChecked(g_menuAction, 1, g_enabled ? 1 : 0);
    }

    void SetEnabled(bool on)
    {
        g_enabled = on;
        rp::host->ConfigSet(rp::self, "selection", "model_outline", on ? "1" : "0");
        SyncMenu();
        rp::host->RequestRedraw(rp::self, MODTOOLS_VIEW_3D);
    }

    void OnToggle(void*) { SetEnabled(!g_enabled); }

    bool BindQt()
    {
        HMODULE w = GetModuleHandleW(L"Qt5Widgets64r.dll"), c = GetModuleHandleW(L"Qt5Core64r.dll");
        if (!w || !c) return false;
        qt.deleteLater = reinterpret_cast<decltype(qt.deleteLater)>(GetProcAddress(c, "?deleteLater@QObject@@QEAAXXZ"));
        qt.qsDtor = reinterpret_cast<decltype(qt.qsDtor)>(GetProcAddress(c, "??1QString@@QEAA@XZ"));
        qt.removeAction = reinterpret_cast<decltype(qt.removeAction)>(GetProcAddress(w, "?removeAction@QWidget@@QEAAXPEAVQAction@@@Z"));
        qt.text = reinterpret_cast<decltype(qt.text)>(GetProcAddress(w, "?text@QAction@@QEBA?AVQString@@XZ"));
        return qt.deleteLater && qt.qsDtor && qt.removeAction && qt.text;
    }

    bool IsOurEntry(void* action)
    {
        void* str = nullptr;
        qt.text(action, &str);
        bool ours = false;
        if (const QArrayDataRaw* d = static_cast<const QArrayDataRaw*>(str))
        {
            const wchar_t* chars = reinterpret_cast<const wchar_t*>(reinterpret_cast<const char*>(d) + d->offset);
            const size_t n = strlen(kMenuText);
            ours = size_t(d->size) >= n;
            for (size_t i = 0; ours && i < n; ++i) ours = chars[i] == wchar_t(kMenuText[i]);
        }
        qt.qsDtor(&str);
        return ours;
    }

    // Radiant's View menu outlives the plugin: every entry of ours goes, an earlier load's included
    void RemoveMenuEntries()
    {
        if (!g_viewMenu || !qt.text || !rp::HasUi(&rp::host->ui->FindChild)) return;
        void* found[64];
        int count = 0;
        for (int i = 0; count < 64; ++i)
        {
            void* a = rp::host->ui->FindChild(g_viewMenu, "QAction", i);
            if (!a) break;
            if (IsOurEntry(a)) found[count++] = a;
        }
        for (int i = 0; i < count; ++i) { qt.removeAction(g_viewMenu, found[i]); qt.deleteLater(found[i]); }
        g_menuAction = nullptr;
    }

    // "Alt+J", "Ctrl+Shift+F5": false when empty or not understood
    bool ParseKey(const std::string& text, int& vk, int& mods, std::string& label)
    {
        vk = 0; mods = 0; label.clear();
        size_t at = 0;
        while (at < text.size())
        {
            size_t end = text.find('+', at);
            if (end == std::string::npos) end = text.size();
            std::string part = text.substr(at, end - at);
            for (char& ch : part) ch = char(toupper(static_cast<unsigned char>(ch)));
            if (part == "CTRL") mods |= MODTOOLS_MOD_CTRL;
            else if (part == "SHIFT") mods |= MODTOOLS_MOD_SHIFT;
            else if (part == "ALT") mods |= MODTOOLS_MOD_ALT;
            else if (part.size() == 1 && isalnum(static_cast<unsigned char>(part[0]))) vk = part[0];
            else if (part.size() >= 2 && part[0] == 'F' && atoi(part.c_str() + 1) >= 1 && atoi(part.c_str() + 1) <= 12) vk = VK_F1 + atoi(part.c_str() + 1) - 1;
            else return false;
            at = end + 1;
        }
        if (!vk) return false;
        label = text;
        return true;
    }

    // ---- detours ----
    uint8_t hk_Lambda(void* lambda, void* inst)
    {
        ++g_inPass;
        const uint8_t r = g_oLambda(lambda, inst);
        --g_inPass;
        return r;
    }

    uintptr_t hk_ModelWire(uintptr_t a1, uintptr_t a2, const uint8_t* rgba, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8)
    {
        if (!g_inPass || g_capture || !Active()) return g_oModelWire(a1, a2, rgba, a4, a5, a6, a7, a8);
        if (rgba) for (int i = 0; i < 4; ++i) g_color[i] = rgba[i] / 255.0f;
        g_capture = true;
        const uintptr_t r = g_oModelWire(a1, a2, rgba, a4, a5, a6, a7, a8);
        g_capture = false;
        return r;
    }

    int64_t hk_Draw(uint32_t indexCount, uint32_t startIndex, int32_t baseVertex)
    {
        ID3D11DeviceContext* ctx = Context();
        if (g_capture && ctx)
        {
            if (EnsureGpu(ctx) && MaskDraw(ctx, indexCount, startIndex, baseVertex)) return 0;
            ++g_fallbacks;
        }
        else if (g_pending && !g_inPass && ctx) Composite(ctx);
        return g_oDraw(indexCount, startIndex, baseVertex);
    }
}

void Setup()
{
    g_enabled = rp::ConfigGetBool("selection", "model_outline", true);
    g_px = rp::ConfigGetInt("selection", "model_outline_px", 2);
    if (g_px < 1) g_px = 1;
    if (g_px > 8) g_px = 8;
    g_ctxGlobal = static_cast<ID3D11DeviceContext**>(rp::host->RadiantAddress(rp::self, kDxContext));
    g_noOutline = static_cast<const int*>(rp::host->RadiantAddress(rp::self, kNoSelOutline));
    void* const lambda = rp::host->RadiantAddress(rp::self, kOutlineLambda);
    void* const wire = rp::host->RadiantAddress(rp::self, kModelWire);
    void* const draw = rp::host->RadiantAddress(rp::self, kDrawIndexed);
    g_hooked = g_ctxGlobal && g_noOutline && lambda && wire && draw &&
        rp::host->Detour(rp::self, lambda, reinterpret_cast<void*>(&hk_Lambda), reinterpret_cast<void**>(&g_oLambda)) &&
        rp::host->Detour(rp::self, wire, reinterpret_cast<void*>(&hk_ModelWire), reinterpret_cast<void**>(&g_oModelWire)) &&
        rp::host->Detour(rp::self, draw, reinterpret_cast<void*>(&hk_Draw), reinterpret_cast<void**>(&g_oDraw));
    if (!g_hooked)
    {
        rp::Log("seloutline: selected model outline not available (hooks)");
        return;
    }
    int vk = 0, mods = 0;
    std::string label;
    const std::string key = rp::ConfigGet("selection", "model_outline_key", "Alt+J");
    const bool hasKey = ParseKey(key, vk, mods, label);
    if (hasKey) rp::host->RegisterHotkey(rp::self, vk, mods, &OnToggle, nullptr);
    if (rp::host->ui && rp::HasUi(&rp::host->ui->FindEditorWidget) && BindQt())
        if ((g_viewMenu = rp::host->ui->FindEditorWidget("menuView")))
        {
            RemoveMenuEntries();
            const std::string text = hasKey ? std::string(kMenuText) + "\t" + label : std::string(kMenuText);
            g_menuAction = rp::host->AddMenuItem(rp::self, g_viewMenu, text.c_str(), &OnToggle, nullptr);
            SyncMenu();
        }
    rp::Log("seloutline: selected model outline on=%d px=%d key=%s menu=%d", g_enabled ? 1 : 0, g_px, hasKey ? label.c_str() : "none", g_menuAction ? 1 : 0);
}

void Draw(int view)
{
    if (view == 1 && g_pending)
        if (ID3D11DeviceContext* ctx = Context()) Composite(ctx);
}

void Shutdown()
{
    RemoveMenuEntries();
    g_viewMenu = nullptr;
    g_pending = false;
    SafeRelease(g_target);
    SafeRelease(g_maskSrv); SafeRelease(g_maskDsv); SafeRelease(g_mask);
    SafeRelease(g_dsWrite); SafeRelease(g_dsOff); SafeRelease(g_rsSolid); SafeRelease(g_blend);
    SafeRelease(g_vs); SafeRelease(g_ps); SafeRelease(g_cb); SafeRelease(g_dev);
    g_maskW = g_maskH = 0;
}

bool Command(const std::string& command, std::string& reply)
{
    if (command.rfind("seloutline", 0) != 0) return false;
    if (command == "seloutline 0" || command == "seloutline 1") SetEnabled(command.back() == '1');
    if (command == "seloutline toggle") OnToggle(nullptr);
    char b[256];
    snprintf(b, sizeof b, "seloutline: enabled=%d ready=%d active=%d menu=%d px=%d masks=%u edges=%u fallbacks=%u mask=%ux%u gpu_failed=%d",
             g_enabled ? 1 : 0, g_hooked ? 1 : 0, g_hooked && Active() ? 1 : 0, g_menuAction ? 1 : 0, g_px,
             g_masks, g_edges, g_fallbacks, g_maskW, g_maskH, g_gpuFailed ? 1 : 0);
    reply = b;
    return true;
}
}
