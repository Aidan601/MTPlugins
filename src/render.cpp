#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <cmath>
#include <atomic>
#include <mutex>
#include <map>
#include <vector>
#include <string>
#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <memory>
#include "MinHook.h"
#include "render.h"
#include "plugins.h"
#include "config.h"
#include "log.h"
#include "radiant_addrs.h"

// Radiant draws through Direct3D 11 (one swap chain per view). We hook IDXGISwapChain::Present via a throwaway swap chain's vtable
// and draw screen-space line lists on the back buffer after a Cam_Draw / XY_Draw, saving and restoring the pipeline state.

namespace
{
    using Present_t       = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using ResizeBuffers_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    using CreateInputLayout_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, ID3D11InputLayout**);
    using DrawIndexed_t   = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
    using DrawIndexedInstanced_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
    using DrawIndexedInstancedIndirect_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
    using PSSetShaderResources_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
    using D3DCompile_t    = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

    Present_t       o_Present = nullptr;
    ResizeBuffers_t o_ResizeBuffers = nullptr;
    CreateInputLayout_t o_CreateInputLayout = nullptr;
    DrawIndexed_t   o_DrawIndexed = nullptr;
    DrawIndexedInstanced_t o_DrawIndexedInstanced = nullptr;
    DrawIndexedInstancedIndirect_t o_DrawIndexedInstancedIndirect = nullptr;
    PSSetShaderResources_t o_PSSetShaderResources = nullptr;

    std::mutex        g_mutex;
    render::View      g_view = render::View::None;
    render::View      g_pendingView = render::View::None;
    render::CamParams g_cam{};
    void*             g_camHwnd = nullptr;   // HWND behind the camera pane's swap chain (tests: pane rect)
    render::XYParams  g_xy{};
    void*             g_xyHwnd[3] = {};      // HWND per 2D view type (0 = YZ side, 1 = XZ front, 2 = XY top)
    int               g_logBudget = 30;

    struct Vtx { float x, y; uint32_t rgba; };

    // the frame being built (only valid inside DrawOverlay or CamHudPass, main thread)
    std::vector<Vtx>  g_frame;
    std::vector<Vtx>  g_triFrame;
    float             g_frameW = 0, g_frameH = 0;
    bool              g_inFrame = false;
    bool              g_textPass = false;        // inside CamHudPass: labels go to Radiant's DrawText2D right away
    // the cam frame built in the HUD pass, drawn by the Present that follows (lines are normalized to its size)
    std::vector<Vtx>  g_camPrebuilt;
    std::vector<Vtx>  g_camTriPrebuilt;
    float             g_camPrebuiltW = 0, g_camPrebuiltH = 0;
    bool              g_camPrebuiltReady = false;
    bool              g_engineTextHooked = false;

    // Native preview adapter: for a triangle-list draw with a readable float3 POSITION stream, read a small indexed range back,
    // prove every position belongs to the selected XMODEL source mesh, then substitute a temporary copy for that draw only.
    struct LayoutInfo
    {
        bool hasPosition = false;
        UINT positionOffset = 0;
        UINT positionSlot = 0;
        // Tessellation compacts the index buffer only when every per-vertex input arrives through the source-position stream.
        // Instance data stays bound as-is, skinned or split-stream layouts keep the vertex-only path.
        bool tessellationEligible = false;
        // Layouts created before CreateInputLayout is hooked have no descriptors: used only once slot zero is proven the sole vertex stream.
        bool tessellationFallbackEligible = false;
        struct Element
        {
            UINT offset = 0;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            std::string semantic;
        };
        std::vector<Element> elements;
    };
    std::mutex g_nativeMutex;
    bool g_nativeHooksReady = false;
    std::unordered_map<ID3D11InputLayout*, LayoutInfo> g_layouts;
    uint64_t g_nativeFrame = 0;
    unsigned g_nativeReadbacks = 0;
    unsigned g_nativeDrawsThisFrame = 0;
    // A retained preview whose rest submission is culled cannot become a native draw while the camera is still, so draws are not
    // inspected then. A request change or camera movement opens a short acquisition window, a prior live draw keeps its cached submission.
    constexpr unsigned kNativeProbeBurstFrames = 3;
    render::CamParams g_nativeProbeCamera{};
    bool g_nativeProbeCameraValid = false;
    unsigned g_nativeProbeBurstFrames = 0;
    std::atomic<bool> g_nativeHadLiveDrawLastFrame{ false };
    std::atomic<bool> g_nativeProbeAllowed{ true };
    bool g_nativeLoggedNoLayout = false;
    bool g_nativeLoggedFallbackLayout = false;
    bool g_nativeLoggedSourceSetInvalid = false;
    bool g_nativeLoggedIndexed = false;
    bool g_nativeLoggedInstanced = false;
    bool g_nativeLoggedIndirect = false;
    bool g_nativeLoggedTessellation = false;
    bool g_nativeLoggedTessellationLimit = false;
    bool g_nativeLoggedTessellationBinding = false;
    void* g_nativeDrawIndexedTarget = nullptr;
    void* g_nativeDrawIndexedInstancedTarget = nullptr;
    void* g_nativeDrawIndexedIndirectTarget = nullptr;
    bool g_nativeLoggedLiveDevice = false;
    // d3d11 has two entry points per draw: the plain DrawIndexed and an ..._Amortized wrapper that calls it through the context
    // function table (the vtable is swapped between them at runtime). Hooking both with one shared o_* trampoline recurses until
    // the stack is gone, so only one target per draw stays hooked.
    std::atomic<bool> g_nativeDrawIndexedSeen{ false };
    std::atomic<bool> g_nativeDrawIndexedInstancedSeen{ false };
    std::atomic<bool> g_nativeDrawIndexedIndirectSeen{ false };
    constexpr unsigned kNativeRetargetLimit = 6;
    unsigned g_nativeRetargets = 0;
    bool g_nativeLoggedRetargetKept = false;
    bool g_nativeLoggedRetargetLimit = false;
    // Last resort: drop a nested submission instead of running the stack out.
    constexpr unsigned kNativeDrawDepthLimit = 8;
    thread_local unsigned t_nativeDrawDepth = 0;
    bool g_nativeLoggedDrawDepth = false;

    // ---- Radiant's HUD font (DrawText2D + g_debugFont, see radiant_addrs.h) ---------------------
    using DrawText2D_t = void(__fastcall*)(const char* text, void** font, const float* pos, const float* xAdvance, const float* yDir, const float* rgba, float* outEnd, unsigned char backgroundAlpha);
    using GetGlyph_t   = const unsigned char*(__fastcall*)(void** font, int ch);

    void** EngineFontSlot() { return reinterpret_cast<void**>(rt::Rva(rt::g_debugFont)); }
    bool EngineFontReady()
    {
        if (!g_engineTextHooked || !Cfg().engineText) return false;
        void* font = *EngineFontSlot();
        return font && *reinterpret_cast<void**>(static_cast<char*>(font) + 0x48) != nullptr;
    }
    float EngineLineHeight() { return float(*reinterpret_cast<int*>(static_cast<char*>(*EngineFontSlot()) + 0x40)); }
    float EngineTextWidth(const std::string& text, float scale)
    {
        auto glyph = reinterpret_cast<GetGlyph_t>(rt::Rva(rt::Font_GetGlyph));
        int adv = 0;
        for (char ch : text) if (const unsigned char* g = glyph(EngineFontSlot(), ch)) adv += g[4];
        return float(adv) * scale;
    }

    // Per-device GPU resources.
    struct DeviceRes
    {
        ID3D11VertexShader* vs = nullptr;
        ID3D11PixelShader*  ps = nullptr;
        ID3D11InputLayout*  layout = nullptr;
        ID3D11Buffer*       vb = nullptr;
        UINT                vbCapacity = 0;
        ID3D11RasterizerState*   rs = nullptr;
        ID3D11BlendState*        bs = nullptr;
        ID3D11DepthStencilState* dss = nullptr;
        bool                failed = false;
    };
    std::map<ID3D11Device*, DeviceRes> g_res;
    std::map<IDXGISwapChain*, ID3D11RenderTargetView*> g_rtvs;

    struct CameraLutPreviewData
    {
        UINT width = 0, height = 0;
        std::shared_ptr<const std::vector<uint16_t>> referenceRgb;
        std::shared_ptr<const std::vector<uint16_t>> previewRgb;
        uint64_t generation = 0;
        bool enabled = false;
    };
    std::shared_ptr<const CameraLutPreviewData> g_cameraLutData;
    std::atomic<int> g_cameraLutStatus{ MODTOOLS_CAMERA_LUT_PREVIEW_OFF };
    std::atomic<bool> g_cameraLutWasBound{ false };
    std::atomic<bool> g_cameraDrawActive{ false };
    std::atomic<bool> g_cameraLutHookReady{ false };
    std::atomic<unsigned> g_cameraLutBindingLogCount{ 0 };
    std::atomic<unsigned> g_cameraLutProbeLogCount{ 0 };
    std::atomic<unsigned> g_cameraLutCamDrawLogCount{ 0 };
    std::atomic<unsigned> g_cameraLutRejectedCount{ 0 };
    std::atomic<unsigned> g_cameraLutFramesWithoutCandidate{ 0 };
    std::atomic<bool> g_cameraLutNoMatchLogged{ false };

    std::mutex g_cameraLutCandidateMutex;
    std::vector<ID3D11ShaderResourceView*> g_cameraLutCandidates;
    std::vector<ID3D11ShaderResourceView*> g_cameraLutRejectedCandidates;
    std::unordered_set<ID3D11ShaderResourceView*> g_cameraLutCandidateSet;
    std::mutex g_cameraLutResourceMutex;
    ID3D11ShaderResourceView* g_cameraLutSourceView = nullptr;
    ID3D11Resource* g_cameraLutSourceResource = nullptr;
    ID3D11Texture2D* g_cameraLutReplacementTexture = nullptr;
    ID3D11ShaderResourceView* g_cameraLutReplacementView = nullptr;
    DXGI_FORMAT g_cameraLutTextureFormat = DXGI_FORMAT_UNKNOWN;
    bool g_cameraLutFlipY = false;
    bool g_cameraLutSwapRB = false;
    ID3D11Device* g_cameraLutDevice = nullptr;
    uint64_t g_cameraLutTextureGeneration = 0;
    void* g_psSetShaderResourcesTarget = nullptr;
    std::atomic<bool> g_psSetShaderResourcesSeen{ false };

    constexpr float kDeg = 3.14159265f / 180.0f;

    const char* kShader = R"(
struct VSIn  { float2 pos : POSITION; float4 col : COLOR; };
struct PSIn  { float4 pos : SV_POSITION; float4 col : COLOR; };
PSIn VS(VSIn i) { PSIn o; o.pos = float4(i.pos, 0.5, 1.0); o.col = i.col; return o; }
float4 PS(PSIn i) : SV_Target { return i.col; }
)";

    D3DCompile_t LoadCompiler()
    {
        static D3DCompile_t fn = nullptr;
        if (fn) return fn;
        for (const wchar_t* name : { L"d3dcompiler_47.dll", L"D3DCOMPIL64_47.dll", L"d3dcompiler_46.dll" })
        {
            HMODULE m = GetModuleHandleW(name); if (!m) m = LoadLibraryW(name);
            if (m) { fn = reinterpret_cast<D3DCompile_t>(GetProcAddress(m, "D3DCompile")); if (fn) { LOGW(L"render: using %s", name); return fn; } }
        }
        LOG("render: no D3DCompile available");
        return nullptr;
    }

    bool EnsureResources(ID3D11Device* dev, DeviceRes& r)
    {
        if (r.vs) return true;
        if (r.failed) return false;
        D3DCompile_t compile = LoadCompiler();
        if (!compile) { r.failed = true; return false; }
        ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
        if (FAILED(compile(kShader, strlen(kShader), "rtc", nullptr, nullptr, "VS", "vs_4_0", 0, 0, &vsb, &err)))
        { LOG("render: VS compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?"); if (err) err->Release(); r.failed = true; return false; }
        if (FAILED(compile(kShader, strlen(kShader), "rtc", nullptr, nullptr, "PS", "ps_4_0", 0, 0, &psb, &err)))
        { LOG("render: PS compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?"); if (err) err->Release(); vsb->Release(); r.failed = true; return false; }
        dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &r.vs);
        dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &r.ps);
        D3D11_INPUT_ELEMENT_DESC il[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
        dev->CreateInputLayout(il, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &r.layout);
        vsb->Release(); psb->Release();

        D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = FALSE; rd.ScissorEnable = FALSE; rd.AntialiasedLineEnable = TRUE;
        dev->CreateRasterizerState(&rd, &r.rs);
        D3D11_BLEND_DESC bd{}; bd.RenderTarget[0].BlendEnable = TRUE; bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO; bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD; bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        dev->CreateBlendState(&bd, &r.bs);
        D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthEnable = FALSE; dd.StencilEnable = FALSE;
        dev->CreateDepthStencilState(&dd, &r.dss);
        bool ok = r.vs && r.ps && r.layout && r.rs && r.bs && r.dss;
        LOG("render: d3d11 resources %s", ok ? "created" : "FAILED");
        if (!ok) r.failed = true;
        return ok;
    }

    bool EnsureVB(ID3D11Device* dev, DeviceRes& r, UINT count)
    {
        if (r.vb && r.vbCapacity >= count) return true;
        if (r.vb) { r.vb->Release(); r.vb = nullptr; }
        D3D11_BUFFER_DESC bd{}; bd.Usage = D3D11_USAGE_DYNAMIC; bd.ByteWidth = sizeof(Vtx) * (count + 512); bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &r.vb))) return false;
        r.vbCapacity = count + 512;
        return true;
    }

    bool CameraLutEnabled()
    {
        const std::shared_ptr<const CameraLutPreviewData> data = std::atomic_load(&g_cameraLutData);
        return data && data->enabled && data->referenceRgb && data->previewRgb &&
            !data->referenceRgb->empty() && !data->previewRgb->empty();
    }

    void QueueCameraLutCandidate(ID3D11ShaderResourceView* view)
    {
        if (!view) return;
        {
            std::lock_guard<std::mutex> lock(g_cameraLutCandidateMutex);
            if (g_cameraLutCandidateSet.find(view) != g_cameraLutCandidateSet.end() || g_cameraLutCandidateSet.size() >= 256) return;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
        view->GetDesc(&viewDesc);
        if (viewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) return;
        ID3D11Resource* resource = nullptr;
        view->GetResource(&resource);
        if (!resource) return;
        ID3D11Texture2D* texture = nullptr;
        HRESULT hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
        resource->Release();
        if (FAILED(hr) || !texture) return;
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        texture->Release();
        const std::shared_ptr<const CameraLutPreviewData> data = std::atomic_load(&g_cameraLutData);
        if (!data || !data->enabled || desc.Width != data->width || desc.Height != data->height || desc.SampleDesc.Count != 1) return;

        std::lock_guard<std::mutex> lock(g_cameraLutCandidateMutex);
        if (g_cameraLutCandidateSet.size() >= 256 || !g_cameraLutCandidateSet.insert(view).second) return;
        view->AddRef();
        g_cameraLutCandidates.push_back(view);
        g_cameraLutFramesWithoutCandidate.store(0, std::memory_order_release);
        g_cameraLutNoMatchLogged.store(false, std::memory_order_release);
        if (g_cameraLutStatus.load(std::memory_order_acquire) == MODTOOLS_CAMERA_LUT_PREVIEW_NO_MATCH)
            g_cameraLutStatus.store(MODTOOLS_CAMERA_LUT_PREVIEW_WAITING, std::memory_order_release);
    }

    void STDMETHODCALLTYPE hk_PSSetShaderResources(ID3D11DeviceContext* ctx, UINT startSlot, UINT count,
                                                      ID3D11ShaderResourceView* const* views)
    {
        g_psSetShaderResourcesSeen.store(true, std::memory_order_release);
        g_cameraLutHookReady.store(true, std::memory_order_release);
        if (!o_PSSetShaderResources) return;
        if (ctx && views && count && g_cameraDrawActive.load(std::memory_order_acquire) && CameraLutEnabled())
        {
            const unsigned logIndex = g_cameraLutBindingLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logIndex < 8)
            {
                LOG("render: camera LUT observed PSSetShaderResources call %u (slot=%u count=%u)", logIndex + 1, startSlot, count);
                for (UINT i = 0; i < count && i < 12; ++i)
                {
                    ID3D11ShaderResourceView* view = views[i];
                    if (!view) continue;
                    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
                    view->GetDesc(&viewDesc);
                    ID3D11Resource* resource = nullptr;
                    view->GetResource(&resource);
                    ID3D11Texture2D* texture = nullptr;
                    D3D11_TEXTURE2D_DESC textureDesc{};
                    if (resource)
                    {
                        if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) && texture)
                            texture->GetDesc(&textureDesc);
                        resource->Release();
                    }
                    LOG("render: camera LUT texture slot %u: dimension=%u format=%u size=%ux%u mips=%u", startSlot + i,
                        static_cast<unsigned>(viewDesc.ViewDimension), static_cast<unsigned>(viewDesc.Format),
                        textureDesc.Width, textureDesc.Height, textureDesc.MipLevels);
                    if (texture) texture->Release();
                }
            }
        }
        if (!ctx || !views || count == 0 || !g_cameraDrawActive.load(std::memory_order_acquire) || !CameraLutEnabled())
        {
            o_PSSetShaderResources(ctx, startSlot, count, views);
            return;
        }

        ID3D11ShaderResourceView* source = nullptr;
        ID3D11ShaderResourceView* replacement = nullptr;
        ID3D11Resource* sourceResource = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_cameraLutResourceMutex);
            source = g_cameraLutSourceView;
            replacement = g_cameraLutReplacementView;
            sourceResource = g_cameraLutSourceResource;
            if (source) source->AddRef();
            if (replacement) replacement->AddRef();
            if (sourceResource) sourceResource->AddRef();
        }

        bool substitute = false;
        std::vector<ID3D11ShaderResourceView*> bound;
        if (replacement && source)
        {
            for (UINT i = 0; i < count; ++i)
            {
                if (views[i] == source) { substitute = true; break; }
                if (!views[i] || !sourceResource) continue;
                ID3D11Resource* boundResource = nullptr;
                views[i]->GetResource(&boundResource);
                const bool sameResource = boundResource == sourceResource;
                if (boundResource) boundResource->Release();
                if (sameResource) { substitute = true; break; }
            }
        }
        if (substitute)
        {
            bound.assign(views, views + count);
            for (ID3D11ShaderResourceView*& view : bound)
            {
                if (view == source) { view = replacement; continue; }
                if (!view || !sourceResource) continue;
                ID3D11Resource* boundResource = nullptr;
                view->GetResource(&boundResource);
                const bool sameResource = boundResource == sourceResource;
                if (boundResource) boundResource->Release();
                if (sameResource) view = replacement;
            }
            o_PSSetShaderResources(ctx, startSlot, count, bound.data());
            g_cameraLutWasBound.store(true, std::memory_order_release);
            g_cameraLutStatus.store(MODTOOLS_CAMERA_LUT_PREVIEW_ACTIVE, std::memory_order_release);
            if (sourceResource) sourceResource->Release();
            source->Release();
            replacement->Release();
            return;
        }
        if (sourceResource) sourceResource->Release();
        if (source) source->Release();
        if (replacement) replacement->Release();

        for (UINT i = 0; i < count; ++i) QueueCameraLutCandidate(views[i]);
        o_PSSetShaderResources(ctx, startSlot, count, views);
    }

    float HalfToFloat(uint16_t half)
    {
        const uint32_t sign = uint32_t(half & 0x8000u) << 16;
        uint32_t exponent = (half >> 10) & 0x1Fu;
        uint32_t mantissa = half & 0x03FFu;
        uint32_t bits = 0;
        if (exponent == 0)
        {
            if (mantissa == 0) bits = sign;
            else
            {
                int shift = 0;
                while ((mantissa & 0x0400u) == 0) { mantissa <<= 1; ++shift; }
                mantissa &= 0x03FFu;
                bits = sign | (uint32_t(113 - shift) << 23) | (mantissa << 13);
            }
        }
        else if (exponent == 31) bits = sign | 0x7F800000u | (mantissa << 13);
        else bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
        float value = 0.0f;
        memcpy(&value, &bits, sizeof value);
        return value;
    }

    uint16_t FloatToHalf(float value)
    {
        uint32_t bits = 0;
        memcpy(&bits, &value, sizeof bits);
        const uint16_t sign = static_cast<uint16_t>((bits >> 16) & 0x8000u);
        const uint32_t exponent = (bits >> 23) & 0xFFu;
        uint32_t mantissa = bits & 0x7FFFFFu;
        if (exponent < 103) return sign;
        if (exponent > 142) return static_cast<uint16_t>(sign | 0x7BFFu);
        if (exponent < 113)
        {
            mantissa |= 0x800000u;
            const unsigned shift = 126u - exponent;
            const uint32_t rounded = (mantissa + (1u << (shift - 1))) >> shift;
            return static_cast<uint16_t>(sign | rounded);
        }
        uint32_t halfExponent = exponent - 112u;
        mantissa += 0x1000u;
        if (mantissa & 0x800000u) { mantissa = 0; ++halfExponent; }
        if (halfExponent >= 31u) return static_cast<uint16_t>(sign | 0x7BFFu);
        return static_cast<uint16_t>(sign | (halfExponent << 10) | (mantissa >> 13));
    }

    bool ReadLutSample(const D3D11_MAPPED_SUBRESOURCE& mapped, DXGI_FORMAT format, UINT x, UINT y, float rgb[3])
    {
        const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
        if (format == DXGI_FORMAT_R16G16B16A16_UNORM)
        {
            const uint16_t* px = reinterpret_cast<const uint16_t*>(row + size_t(x) * 8);
            rgb[0] = px[0] / 65535.0f; rgb[1] = px[1] / 65535.0f; rgb[2] = px[2] / 65535.0f;
            return true;
        }
        if (format == DXGI_FORMAT_R16G16B16A16_FLOAT)
        {
            const uint16_t* px = reinterpret_cast<const uint16_t*>(row + size_t(x) * 8);
            rgb[0] = HalfToFloat(px[0]); rgb[1] = HalfToFloat(px[1]); rgb[2] = HalfToFloat(px[2]);
            return true;
        }
        if (format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        {
            const uint8_t* px = row + size_t(x) * 4;
            rgb[0] = px[0] / 255.0f; rgb[1] = px[1] / 255.0f; rgb[2] = px[2] / 255.0f;
            return true;
        }
        if (format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        {
            const uint8_t* px = row + size_t(x) * 4;
            rgb[0] = px[2] / 255.0f; rgb[1] = px[1] / 255.0f; rgb[2] = px[0] / 255.0f;
            return true;
        }
        if (format == DXGI_FORMAT_R10G10B10A2_UNORM)
        {
            uint32_t packed = 0;
            memcpy(&packed, row + size_t(x) * 4, sizeof packed);
            rgb[0] = float(packed & 0x3FFu) / 1023.0f;
            rgb[1] = float((packed >> 10) & 0x3FFu) / 1023.0f;
            rgb[2] = float((packed >> 20) & 0x3FFu) / 1023.0f;
            return true;
        }
        return false;
    }

    bool MatchCameraLut(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11ShaderResourceView* view,
                        const CameraLutPreviewData& data, DXGI_FORMAT& matchedFormat,
                        bool& matchedFlipY, bool& matchedSwapRB)
    {
        if (!device || !context || !view || data.width == 0 || data.height == 0) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
        view->GetDesc(&viewDesc);
        if (viewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || viewDesc.Texture2D.MostDetailedMip != 0) return false;
        ID3D11Resource* resource = nullptr;
        view->GetResource(&resource);
        if (!resource) return false;
        ID3D11Texture2D* texture = nullptr;
        HRESULT hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
        resource->Release();
        if (FAILED(hr) || !texture) return false;

        D3D11_TEXTURE2D_DESC sourceDesc{};
        texture->GetDesc(&sourceDesc);
        const UINT mip = viewDesc.Texture2D.MostDetailedMip;
        const UINT width = (std::max)(1u, sourceDesc.Width >> mip);
        const UINT height = (std::max)(1u, sourceDesc.Height >> mip);
        if (width != data.width || height != data.height || sourceDesc.SampleDesc.Count != 1)
        { texture->Release(); return false; }

        D3D11_TEXTURE2D_DESC stagingDesc{};
        stagingDesc.Width = width; stagingDesc.Height = height; stagingDesc.MipLevels = 1; stagingDesc.ArraySize = 1;
        stagingDesc.Format = viewDesc.Format; stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_STAGING; stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* staging = nullptr;
        hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
        if (SUCCEEDED(hr) && staging)
            context->CopySubresourceRegion(staging, 0, 0, 0, 0, texture, D3D11CalcSubresource(mip, 0, sourceDesc.MipLevels), nullptr);
        texture->Release();
        if (FAILED(hr) || !staging) return false;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) { staging->Release(); return false; }
        double bestError = 1.0;
        bool bestFlipY = false, bestSwapRB = false;
        const UINT sampleStep = (std::max)(1u, data.width / 64);
        for (int flipY = 0; flipY < 2; ++flipY)
        for (int swapRB = 0; swapRB < 2; ++swapRB)
        {
            double sum = 0.0;
            size_t samples = 0;
            bool readable = true;
            for (UINT y = 0; y < data.height; y += sampleStep)
            for (UINT x = 0; x < data.width; x += sampleStep)
            {
                float actual[3]{};
                if (!ReadLutSample(mapped, viewDesc.Format, x, flipY ? data.height - 1 - y : y, actual))
                { readable = false; break; }
                const size_t at = (size_t(y) * data.width + x) * 3;
                const float expected[3] = { (*data.referenceRgb)[at] / 65535.0f, (*data.referenceRgb)[at + 1] / 65535.0f,
                                             (*data.referenceRgb)[at + 2] / 65535.0f };
                for (int c = 0; c < 3; ++c)
                {
                    const float a = actual[swapRB && c != 1 ? 2 - c : c];
                    const double delta = double(a - expected[c]);
                    sum += delta * delta;
                }
                ++samples;
            }
            if (readable && samples)
            {
                const double error = std::sqrt(sum / (samples * 3.0));
                if (error < bestError)
                {
                    bestError = error;
                    bestFlipY = flipY != 0;
                    bestSwapRB = swapRB != 0;
                }
            }
        }
        context->Unmap(staging, 0);
        staging->Release();
        const double tolerance = (viewDesc.Format == DXGI_FORMAT_R16G16B16A16_UNORM ||
                                  viewDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 0.012 : 0.022;
        if (bestError > tolerance) return false;
        matchedFormat = viewDesc.Format;
        matchedFlipY = bestFlipY;
        matchedSwapRB = bestSwapRB;
        return true;
    }

    bool BuildCameraLutBytes(const std::vector<uint16_t>& rgb, UINT width, UINT height, DXGI_FORMAT format,
                             std::vector<uint8_t>& bytes, UINT& rowPitch)
    {
        const bool rgba16 = format == DXGI_FORMAT_R16G16B16A16_UNORM;
        const bool rgba16Float = format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        const bool rgba8 = format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        const bool bgra8 = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        const bool rgb10a2 = format == DXGI_FORMAT_R10G10B10A2_UNORM;
        if (!rgba16 && !rgba16Float && !rgba8 && !bgra8 && !rgb10a2) return false;

        const size_t pixelCount = size_t(width) * height;
        const size_t bytesPerPixel = (rgba16 || rgba16Float) ? 8 : 4;
        bytes.resize(pixelCount * bytesPerPixel);
        rowPitch = width * static_cast<UINT>(bytesPerPixel);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            const uint16_t* src = rgb.data() + i * 3;
            if (rgba16 || rgba16Float)
            {
                uint16_t* dst = reinterpret_cast<uint16_t*>(bytes.data()) + i * 4;
                dst[0] = rgba16 ? src[0] : FloatToHalf(src[0] / 65535.0f);
                dst[1] = rgba16 ? src[1] : FloatToHalf(src[1] / 65535.0f);
                dst[2] = rgba16 ? src[2] : FloatToHalf(src[2] / 65535.0f);
                dst[3] = rgba16 ? 65535 : 0x3C00;
            }
            else
            {
                if (rgb10a2)
                {
                    const uint32_t r = (uint32_t(src[0]) * 1023u + 32767u) / 65535u;
                    const uint32_t g = (uint32_t(src[1]) * 1023u + 32767u) / 65535u;
                    const uint32_t b = (uint32_t(src[2]) * 1023u + 32767u) / 65535u;
                    const uint32_t packed = r | (g << 10) | (b << 20) | (3u << 30);
                    memcpy(bytes.data() + i * 4, &packed, sizeof packed);
                }
                else
                {
                    uint8_t* dst = bytes.data() + i * 4;
                    dst[0] = static_cast<uint8_t>((src[bgra8 ? 2 : 0] + 128) / 257);
                    dst[1] = static_cast<uint8_t>((src[1] + 128) / 257);
                    dst[2] = static_cast<uint8_t>((src[bgra8 ? 0 : 2] + 128) / 257);
                    dst[3] = 255;
                }
            }
        }
        return true;
    }

    bool BuildCameraLutMipChain(const std::vector<uint16_t>& baseRgb, UINT baseWidth, UINT baseHeight,
                                UINT mipCount, DXGI_FORMAT format, bool flipY, bool swapRB,
                                std::vector<std::vector<uint8_t>>& mipBytes,
                                std::vector<UINT>& rowPitches)
    {
        if (baseRgb.size() != size_t(baseWidth) * baseHeight * 3 || mipCount == 0) return false;
        std::vector<uint16_t> rgb(baseRgb.size());
        for (UINT y = 0; y < baseHeight; ++y)
        for (UINT x = 0; x < baseWidth; ++x)
        {
            const UINT dstY = flipY ? baseHeight - 1 - y : y;
            const size_t src = (size_t(y) * baseWidth + x) * 3;
            const size_t dst = (size_t(dstY) * baseWidth + x) * 3;
            rgb[dst] = baseRgb[src + (swapRB ? 2 : 0)];
            rgb[dst + 1] = baseRgb[src + 1];
            rgb[dst + 2] = baseRgb[src + (swapRB ? 0 : 2)];
        }
        UINT width = baseWidth, height = baseHeight;
        mipBytes.clear(); rowPitches.clear();
        mipBytes.reserve(mipCount); rowPitches.reserve(mipCount);
        for (UINT mip = 0; mip < mipCount; ++mip)
        {
            mipBytes.emplace_back();
            UINT pitch = 0;
            if (!BuildCameraLutBytes(rgb, width, height, format, mipBytes.back(), pitch)) return false;
            rowPitches.push_back(pitch);
            if (mip + 1 == mipCount) break;
            const UINT nextWidth = (std::max)(1u, width / 2);
            const UINT nextHeight = (std::max)(1u, height / 2);
            std::vector<uint16_t> next(size_t(nextWidth) * nextHeight * 3);
            for (UINT y = 0; y < nextHeight; ++y)
            for (UINT x = 0; x < nextWidth; ++x)
            for (UINT channel = 0; channel < 3; ++channel)
            {
                uint32_t sum = 0, samples = 0;
                for (UINT oy = 0; oy < 2; ++oy)
                for (UINT ox = 0; ox < 2; ++ox)
                {
                    const UINT sx = (std::min)(width - 1, x * 2 + ox);
                    const UINT sy = (std::min)(height - 1, y * 2 + oy);
                    sum += rgb[(size_t(sy) * width + sx) * 3 + channel];
                    ++samples;
                }
                next[(size_t(y) * nextWidth + x) * 3 + channel] = static_cast<uint16_t>((sum + samples / 2) / samples);
            }
            rgb = std::move(next);
            width = nextWidth; height = nextHeight;
        }
        return true;
    }

    bool UpdateCameraLutReplacement(ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                                    DXGI_FORMAT format, bool flipY, bool swapRB,
                                    const CameraLutPreviewData& data)
    {
        if (!context || !texture || !data.previewRgb) return false;
        std::vector<std::vector<uint8_t>> mipBytes;
        std::vector<UINT> rowPitches;
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.Width != data.width || desc.Height != data.height || desc.MipLevels == 0) return false;
        if (!BuildCameraLutMipChain(*data.previewRgb, data.width, data.height, desc.MipLevels,
                                    format, flipY, swapRB, mipBytes, rowPitches)) return false;
        for (UINT mip = 0; mip < desc.MipLevels; ++mip)
            context->UpdateSubresource(texture, mip, nullptr, mipBytes[mip].data(), rowPitches[mip], 0);
        return true;
    }

    bool CreateCameraLutReplacement(ID3D11Device* device, ID3D11ShaderResourceView* sourceView,
                                    const CameraLutPreviewData& data, ID3D11ShaderResourceView** output,
                                    ID3D11Resource** sourceResource, ID3D11Texture2D** outputTexture,
                                    bool flipY, bool swapRB)
    {
        if (output) *output = nullptr;
        if (sourceResource) *sourceResource = nullptr;
        if (outputTexture) *outputTexture = nullptr;
        if (!device || !sourceView || !output || !sourceResource || !outputTexture) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sourceViewDesc{};
        sourceView->GetDesc(&sourceViewDesc);
        if (sourceViewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) return false;
        ID3D11Resource* resource = nullptr;
        sourceView->GetResource(&resource);
        if (!resource) return false;
        ID3D11Texture2D* sourceTexture = nullptr;
        HRESULT hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&sourceTexture));
        if (FAILED(hr) || !sourceTexture) { resource->Release(); return false; }
        D3D11_TEXTURE2D_DESC sourceDesc{};
        sourceTexture->GetDesc(&sourceDesc);
        if (sourceViewDesc.Texture2D.MostDetailedMip != 0 || sourceDesc.Width != data.width ||
            sourceDesc.Height != data.height || sourceDesc.SampleDesc.Count != 1 || sourceDesc.MipLevels == 0)
        { sourceTexture->Release(); resource->Release(); return false; }

        const DXGI_FORMAT format = sourceViewDesc.Format;
        std::vector<std::vector<uint8_t>> mipBytes;
        std::vector<UINT> rowPitches;
        const UINT mipCount = sourceDesc.MipLevels;
        if (!BuildCameraLutMipChain(*data.previewRgb, data.width, data.height, mipCount,
                                    format, flipY, swapRB, mipBytes, rowPitches))
        { sourceTexture->Release(); resource->Release(); return false; }

        D3D11_TEXTURE2D_DESC targetDesc{};
        targetDesc.Width = data.width; targetDesc.Height = data.height; targetDesc.MipLevels = mipCount; targetDesc.ArraySize = 1;
        targetDesc.Format = format; targetDesc.SampleDesc.Count = 1; targetDesc.Usage = D3D11_USAGE_DEFAULT;
        targetDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<D3D11_SUBRESOURCE_DATA> initial(mipCount);
        for (UINT mip = 0; mip < mipCount; ++mip)
        {
            initial[mip].pSysMem = mipBytes[mip].data();
            initial[mip].SysMemPitch = rowPitches[mip];
        }
        ID3D11Texture2D* targetTexture = nullptr;
        hr = device->CreateTexture2D(&targetDesc, initial.data(), &targetTexture);
        if (SUCCEEDED(hr) && targetTexture)
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC targetViewDesc{};
            targetViewDesc.Format = format; targetViewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            targetViewDesc.Texture2D.MipLevels = sourceViewDesc.Texture2D.MipLevels == UINT(-1) ||
                sourceViewDesc.Texture2D.MipLevels == 0
                ? mipCount : (std::min)(mipCount, sourceViewDesc.Texture2D.MipLevels);
            hr = device->CreateShaderResourceView(targetTexture, &targetViewDesc, output);
        }
        sourceTexture->Release();
        if (FAILED(hr) || !*output || !targetTexture) { if (targetTexture) targetTexture->Release(); resource->Release(); return false; }
        *sourceResource = resource;
        *outputTexture = targetTexture;
        return true;
    }

    void ClearCameraLutResources()
    {
        {
            std::lock_guard<std::mutex> lock(g_cameraLutResourceMutex);
            if (g_cameraLutSourceView) g_cameraLutSourceView->Release();
            if (g_cameraLutSourceResource) g_cameraLutSourceResource->Release();
            if (g_cameraLutReplacementTexture) g_cameraLutReplacementTexture->Release();
            if (g_cameraLutReplacementView) g_cameraLutReplacementView->Release();
            if (g_cameraLutDevice) g_cameraLutDevice->Release();
            g_cameraLutSourceView = nullptr; g_cameraLutSourceResource = nullptr;
            g_cameraLutReplacementTexture = nullptr; g_cameraLutReplacementView = nullptr;
            g_cameraLutTextureFormat = DXGI_FORMAT_UNKNOWN; g_cameraLutDevice = nullptr;
            g_cameraLutFlipY = false; g_cameraLutSwapRB = false;
            g_cameraLutTextureGeneration = 0;
        }
        g_cameraLutWasBound.store(false, std::memory_order_release);
        g_cameraLutFramesWithoutCandidate.store(0, std::memory_order_release);
        g_cameraLutNoMatchLogged.store(false, std::memory_order_release);
        std::vector<ID3D11ShaderResourceView*> candidates;
        std::vector<ID3D11ShaderResourceView*> rejected;
        {
            std::lock_guard<std::mutex> lock(g_cameraLutCandidateMutex);
            candidates.swap(g_cameraLutCandidates);
            rejected.swap(g_cameraLutRejectedCandidates);
            g_cameraLutCandidateSet.clear();
        }
        for (ID3D11ShaderResourceView* view : candidates) if (view) view->Release();
        for (ID3D11ShaderResourceView* view : rejected) if (view) view->Release();
        g_cameraLutRejectedCount.store(0, std::memory_order_release);
    }

    void ProcessCameraLutPreview(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        const std::shared_ptr<const CameraLutPreviewData> data = std::atomic_load(&g_cameraLutData);
        if (!data || !data->enabled)
        {
            ClearCameraLutResources();
            g_cameraLutStatus.store(MODTOOLS_CAMERA_LUT_PREVIEW_OFF, std::memory_order_release);
            return;
        }
        if (!device || !context || !data->referenceRgb || !data->previewRgb ||
            data->referenceRgb->empty() || data->previewRgb->empty())
        {
            g_cameraLutStatus.store(MODTOOLS_CAMERA_LUT_PREVIEW_UNSUPPORTED, std::memory_order_release);
            return;
        }

        ID3D11ShaderResourceView* sourceView = nullptr;
        bool newSource = false;
        bool sourceFlipY = false, sourceSwapRB = false;
        {
            std::lock_guard<std::mutex> lock(g_cameraLutResourceMutex);
            if (g_cameraLutSourceView)
            {
                if (g_cameraLutTextureGeneration == data->generation)
                {
                    g_cameraLutStatus.store(g_cameraLutWasBound.load(std::memory_order_acquire)
                        ? MODTOOLS_CAMERA_LUT_PREVIEW_ACTIVE : MODTOOLS_CAMERA_LUT_PREVIEW_MATCHED, std::memory_order_release);
                    return;
                }
                sourceView = g_cameraLutSourceView;
                sourceFlipY = g_cameraLutFlipY;
                sourceSwapRB = g_cameraLutSwapRB;
                sourceView->AddRef();
            }
        }

        // A camera frame may bind many 1024x1024 images before Present. Probe a small bounded batch
        // per frame so the world LUT is found quickly without stalling the camera render loop.
        for (unsigned attempt = 0; !sourceView && attempt < 8; ++attempt)
        {
            ID3D11ShaderResourceView* candidate = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_cameraLutCandidateMutex);
                if (!g_cameraLutCandidates.empty())
                {
                    candidate = g_cameraLutCandidates.front();
                    g_cameraLutCandidates.erase(g_cameraLutCandidates.begin());
                }
            }
            if (!candidate) break;
            DXGI_FORMAT matchedFormat = DXGI_FORMAT_UNKNOWN;
            const bool matched = MatchCameraLut(device, context, candidate, *data, matchedFormat,
                                                sourceFlipY, sourceSwapRB);
            if (matched)
            {
                sourceView = candidate; // transfer the candidate's reference to the preview state below
                newSource = true;
                LOG("render: matched the world LUT image (format=%u)", static_cast<unsigned>(matchedFormat));
                break;
            }
            const unsigned logIndex = g_cameraLutProbeLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logIndex < 16)
            {
                D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
                candidate->GetDesc(&viewDesc);
                ID3D11Resource* resource = nullptr;
                candidate->GetResource(&resource);
                ID3D11Texture2D* texture = nullptr;
                D3D11_TEXTURE2D_DESC textureDesc{};
                if (resource && SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) && texture)
                    texture->GetDesc(&textureDesc);
                LOG("render: camera LUT probe %u did not match (view=%u format=%u size=%ux%u)", logIndex + 1,
                    static_cast<unsigned>(viewDesc.ViewDimension), static_cast<unsigned>(viewDesc.Format),
                    textureDesc.Width, textureDesc.Height);
                if (texture) texture->Release();
                if (resource) resource->Release();
            }
            std::lock_guard<std::mutex> lock(g_cameraLutCandidateMutex);
            g_cameraLutRejectedCandidates.push_back(candidate); // keep it alive and don't read it back again
            g_cameraLutRejectedCount.fetch_add(1, std::memory_order_relaxed);
        }

        if (sourceView && !newSource)
        {
            ID3D11Texture2D* replacementTexture = nullptr;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            {
                std::lock_guard<std::mutex> lock(g_cameraLutResourceMutex);
                replacementTexture = g_cameraLutReplacementTexture;
                format = g_cameraLutTextureFormat;
                sourceFlipY = g_cameraLutFlipY;
                sourceSwapRB = g_cameraLutSwapRB;
                if (replacementTexture) replacementTexture->AddRef();
            }
            const bool updated = replacementTexture && UpdateCameraLutReplacement(context, replacementTexture, format,
                                                                                   sourceFlipY, sourceSwapRB, *data);
            if (replacementTexture) replacementTexture->Release();
            if (updated)
            {
                std::lock_guard<std::mutex> lock(g_cameraLutResourceMutex);
                if (g_cameraLutSourceView == sourceView)
                {
                    g_cameraLutTextureGeneration = data->generation;
                    g_cameraLutWasBound.store(false, std::memory_order_release);
                    g_cameraLutStatus.store(MODTOOLS_CAMERA_LUT_PREVIEW_MATCHED, std::memory_order_release);
                    LOG("render: updated the camera LUT texture in memory (generation=%llu)",
                        static_cast<unsigned long long>(data->generation));
                }
            }
            else
            {
                g_cameraLutStatus.store(MODTOOLS_CAMERA_LUT_PREVIEW_UNSUPPORTED, std::memory_order_release);
                LOG("render: couldn't update the in-memory camera LUT texture");
            }
            sourceView->Release();
            return;
        }

        if (sourceView)
        {
            ID3D11ShaderResourceView* replacementView = nullptr;
            ID3D11Resource* sourceResource = nullptr;
            ID3D11Texture2D* replacementTexture = nullptr;
            if (CreateCameraLutReplacement(device, sourceView, *data, &replacementView, &sourceResource,
                                           &replacementTexture, sourceFlipY, sourceSwapRB))
            {
                std::lock_guard<std::mutex> lock(g_cameraLutResourceMutex);
                if (g_cameraLutReplacementTexture) g_cameraLutReplacementTexture->Release();
                if (g_cameraLutReplacementView) g_cameraLutReplacementView->Release();
                if (g_cameraLutSourceResource) g_cameraLutSourceResource->Release();
                if (newSource)
                {
                    if (g_cameraLutSourceView) g_cameraLutSourceView->Release();
                    {
                        std::lock_guard<std::mutex> candidateLock(g_cameraLutCandidateMutex);
                        g_cameraLutCandidateSet.erase(sourceView);
                    }
                    g_cameraLutSourceView = sourceView; // transfer candidate's owned reference
                    sourceView = nullptr;
                    if (g_cameraLutDevice) g_cameraLutDevice->Release();
                    g_cameraLutDevice = device; device->AddRef();
                }
                g_cameraLutSourceResource = sourceResource;
                g_cameraLutReplacementTexture = replacementTexture;
                g_cameraLutReplacementView = replacementView;
                D3D11_SHADER_RESOURCE_VIEW_DESC sourceViewDesc{};
                g_cameraLutSourceView->GetDesc(&sourceViewDesc);
                g_cameraLutTextureFormat = sourceViewDesc.Format;
                g_cameraLutFlipY = sourceFlipY;
                g_cameraLutSwapRB = sourceSwapRB;
                g_cameraLutTextureGeneration = data->generation;
                g_cameraLutWasBound.store(false, std::memory_order_release);
                g_cameraLutStatus.store(MODTOOLS_CAMERA_LUT_PREVIEW_MATCHED, std::memory_order_release);
            }
            else if (newSource)
            {
                g_cameraLutStatus.store(MODTOOLS_CAMERA_LUT_PREVIEW_UNSUPPORTED, std::memory_order_release);
                LOG("render: matched the world LUT but could not create its replacement texture");
                std::lock_guard<std::mutex> candidateLock(g_cameraLutCandidateMutex);
                g_cameraLutRejectedCandidates.push_back(sourceView);
                sourceView = nullptr; // keep the reference so this unsupported match isn't retried
            }
            if (sourceView) sourceView->Release();
        }
        else if (g_cameraLutStatus.load(std::memory_order_acquire) != MODTOOLS_CAMERA_LUT_PREVIEW_UNSUPPORTED)
        {
            std::lock_guard<std::mutex> candidateLock(g_cameraLutCandidateMutex);
            const bool exhausted = g_cameraLutCandidates.empty() &&
                g_cameraLutRejectedCount.load(std::memory_order_relaxed) != 0;
            if (exhausted)
            {
                g_cameraLutStatus.store(MODTOOLS_CAMERA_LUT_PREVIEW_NO_MATCH, std::memory_order_release);
            }
            else if (g_cameraLutCandidates.empty())
            {
                const unsigned frames = g_cameraLutFramesWithoutCandidate.fetch_add(1, std::memory_order_acq_rel) + 1;
                if (frames >= 60)
                {
                    g_cameraLutStatus.store(MODTOOLS_CAMERA_LUT_PREVIEW_NO_MATCH, std::memory_order_release);
                    if (!g_cameraLutNoMatchLogged.exchange(true, std::memory_order_acq_rel))
                        LOG("render: no camera LUT texture matching %ux%u was bound during 60 camera frames; worldspawn LUT preview cannot be applied", data->width, data->height);
                }
                else g_cameraLutStatus.store(MODTOOLS_CAMERA_LUT_PREVIEW_WAITING, std::memory_order_release);
            }
            else g_cameraLutStatus.store(MODTOOLS_CAMERA_LUT_PREVIEW_WAITING, std::memory_order_release);
        }
    }

    void AngleVectors(const float angles[3], float fwd[3], float right[3], float up[3])
    {
        // Quake convention: pitch, yaw, roll in degrees, x forward, y left, z up. Radiant's camera looks UP for positive pitch (pitch_sign=1).
        float p = angles[0] * kDeg * Cfg().pitchSign, y = angles[1] * kDeg;
        float cp = cosf(p), sp = sinf(p), cy = cosf(y), sy = sinf(y);
        fwd[0] = cp * cy; fwd[1] = cp * sy; fwd[2] = sp;
        right[0] = fwd[1]; right[1] = -fwd[0]; right[2] = 0;                 // forward x worldUp
        float rl = sqrtf(right[0] * right[0] + right[1] * right[1]);
        if (rl < 1e-5f) { right[0] = 0; right[1] = -1; } else { right[0] /= rl; right[1] /= rl; }
        up[0] = right[1] * fwd[2] - right[2] * fwd[1];                        // right x forward
        up[1] = right[2] * fwd[0] - right[0] * fwd[2];
        up[2] = right[0] * fwd[1] - right[1] * fwd[0];
    }

    bool ProjectXY(const render::XYParams& xy, float w, float h, const float p[3], float& sx, float& sy)
    {
        int ha = xy.type == 0 ? 1 : 0;              // horizontal axis index
        int va = xy.type == 2 ? 1 : 2;              // vertical axis index
        sx = w * 0.5f + (p[ha] - xy.origin[ha]) * xy.scale;
        sy = h * 0.5f - (p[va] - xy.origin[va]) * xy.scale;
        return true;
    }

    uint32_t ToRGBA(uint32_t argb)
    {
        uint8_t a = argb >> 24, r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
        return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24);   // R8G8B8A8 memory order
    }

    struct NativeVec { float x, y, z; };
    NativeVec NVAdd(NativeVec a, NativeVec b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    NativeVec NVSub(NativeVec a, NativeVec b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    NativeVec NVMul(NativeVec a, float s) { return { a.x * s, a.y * s, a.z * s }; }
    float NVDot(NativeVec a, NativeVec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    NativeVec NVCross(NativeVec a, NativeVec b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
    float NVLength(NativeVec a) { return std::sqrt(NVDot(a, a)); }
    NativeVec NVNormalize(NativeVec a)
    {
        const float length = NVLength(a);
        return length > 0.0001f ? NVMul(a, 1.0f / length) : NativeVec{};
    }

    NativeVec NV(const float p[3]) { return { p[0], p[1], p[2] }; }
    bool IsFinite(NativeVec p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }

    struct NativePositionKey
    {
        int32_t x = 0, y = 0, z = 0;
        bool operator==(const NativePositionKey& other) const { return x == other.x && y == other.y && z == other.z; }
    };

    struct NativePositionKeyHash
    {
        size_t operator()(const NativePositionKey& value) const
        {
            const uint64_t x = static_cast<uint32_t>(value.x);
            const uint64_t y = static_cast<uint32_t>(value.y);
            const uint64_t z = static_cast<uint32_t>(value.z);
            return static_cast<size_t>((x * 0x9E3779B185EBCA87ull) ^ (y * 0xC2B2AE3D27D4EB4Full) ^ (z * 0x165667B19E3779F9ull));
        }
    };

    struct NativePositionSet
    {
        std::unordered_set<NativePositionKey, NativePositionKeyHash> keys;
    };

    // A request is keyed by the persistent map-entity GUID from the plugin. The host owns the position proof, so plugin reloads
    // and its mesh cache cannot invalidate a live D3D replacement.
    struct NativePreviewRequest
    {
        ModToolsNativeModelPreview preview{};
        std::shared_ptr<const NativePositionSet> sourcePositions;
        const float* sourceInput = nullptr;
        int status = MODTOOLS_NATIVE_PREVIEW_UNAVAILABLE;
        unsigned drawsThisFrame = 0;
    };

    std::vector<NativePreviewRequest> g_nativePreviews;

    // Draw hooks run for every indexed submission in the camera pass: request data lives in a lock-free snapshot,
    // status stays in g_nativePreviews under g_nativeMutex (a non-matching draw takes no mutex).
    struct NativePreviewDispatchRequest
    {
        ModToolsNativeModelPreview preview{};
        std::shared_ptr<const NativePositionSet> sourcePositions;
    };

    struct NativePreviewDispatchSet
    {
        std::vector<NativePreviewDispatchRequest> requests;
    };

    std::shared_ptr<const NativePreviewDispatchSet> g_nativeDispatchSet;
    std::atomic<bool> g_nativeDispatchShowsOriginal{ false };

    // A source proof needs a staging readback that stalls the immediate context: remember streams read successfully that did not
    // belong to the selected XMODEL so dense maps skip the repeat.
    struct NativeDrawKey
    {
        ID3D11Buffer* vertexBuffer = nullptr;
        ID3D11Buffer* indexBuffer = nullptr;
        ID3D11InputLayout* inputLayout = nullptr;
        D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
        UINT stride = 0;
        UINT vertexOffset = 0;
        UINT indexOffset = 0;
        UINT positionSlot = 0;
        UINT positionOffset = 0;
        UINT indexCount = 0;
        UINT startIndex = 0;
        INT baseVertex = 0;
        UINT startInstance = 0;
        bool instanced = false;

        bool operator==(const NativeDrawKey& other) const
        {
            return vertexBuffer == other.vertexBuffer && indexBuffer == other.indexBuffer && inputLayout == other.inputLayout &&
                topology == other.topology && indexFormat == other.indexFormat && stride == other.stride &&
                vertexOffset == other.vertexOffset && indexOffset == other.indexOffset && positionSlot == other.positionSlot &&
                positionOffset == other.positionOffset && indexCount == other.indexCount && startIndex == other.startIndex &&
                baseVertex == other.baseVertex && startInstance == other.startInstance && instanced == other.instanced;
        }
    };

    struct NativeDrawKeyHash
    {
        size_t operator()(const NativeDrawKey& value) const
        {
            size_t hash = reinterpret_cast<size_t>(value.vertexBuffer) ^ (reinterpret_cast<size_t>(value.indexBuffer) << 1) ^
                (reinterpret_cast<size_t>(value.inputLayout) << 2);
            const uint32_t words[] = { value.stride, value.vertexOffset, value.indexOffset, value.positionSlot, value.positionOffset,
                value.indexCount, value.startIndex, static_cast<uint32_t>(value.baseVertex), value.startInstance,
                static_cast<uint32_t>(value.topology), static_cast<uint32_t>(value.indexFormat), value.instanced ? 1u : 0u };
            for (uint32_t word : words) hash = (hash * 16777619u) ^ word;
            return hash;
        }
    };

    std::unordered_set<NativeDrawKey, NativeDrawKeyHash> g_nativeRejectedDraws;

    struct NativeProbeState
    {
        size_t nextRequest = 0;
        size_t failures = 0;
    };
    // An unrecognized draw can belong to a brush, terrain or another model: probe one requested source per repeated draw,
    // not every saved entity per frame.
    std::unordered_map<NativeDrawKey, NativeProbeState, NativeDrawKeyHash> g_nativeProbeStates;

    // Old deferred replay state, kept only to release diagnostic data. Present-time replay is never called
    // (a pipeline is valid only during its original scene submission).
    struct NativePipelineSnapshot
    {
        ID3D11InputLayout* inputLayout = nullptr;
        D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        std::array<ID3D11Buffer*, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertexBuffers{};
        std::array<UINT, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertexStrides{};
        std::array<UINT, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertexOffsets{};
        ID3D11VertexShader* vertexShader = nullptr;
        ID3D11PixelShader* pixelShader = nullptr;
        ID3D11GeometryShader* geometryShader = nullptr;
        ID3D11HullShader* hullShader = nullptr;
        ID3D11DomainShader* domainShader = nullptr;
        std::array<ID3D11Buffer*, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> vertexConstantBuffers{};
        std::array<ID3D11Buffer*, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> pixelConstantBuffers{};
        std::array<ID3D11Buffer*, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> geometryConstantBuffers{};
        std::array<ID3D11Buffer*, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> hullConstantBuffers{};
        std::array<ID3D11Buffer*, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> domainConstantBuffers{};
        std::array<ID3D11ShaderResourceView*, 16> pixelResources{};
        std::array<ID3D11SamplerState*, 16> pixelSamplers{};
        ID3D11RasterizerState* rasterizer = nullptr;
        ID3D11BlendState* blend = nullptr;
        float blendFactor[4] = {};
        UINT sampleMask = 0xFFFFFFFF;
        ID3D11DepthStencilState* depthStencil = nullptr;
        UINT stencilRef = 0;
        std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
        UINT viewportCount = 0;
        std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissors{};
        UINT scissorCount = 0;
        bool materialReady = false;
        bool valid = false;
    };

    template <typename T, size_t N>
    void ReleaseNativePipelineRefs(std::array<T*, N>& values)
    {
        for (T*& value : values) { if (value) value->Release(); value = nullptr; }
    }

    void ReleaseNativePipelineSnapshot(NativePipelineSnapshot& state)
    {
        if (state.inputLayout) state.inputLayout->Release();
        if (state.vertexShader) state.vertexShader->Release();
        if (state.pixelShader) state.pixelShader->Release();
        if (state.geometryShader) state.geometryShader->Release();
        if (state.hullShader) state.hullShader->Release();
        if (state.domainShader) state.domainShader->Release();
        if (state.rasterizer) state.rasterizer->Release();
        if (state.blend) state.blend->Release();
        if (state.depthStencil) state.depthStencil->Release();
        ReleaseNativePipelineRefs(state.vertexBuffers);
        ReleaseNativePipelineRefs(state.vertexConstantBuffers);
        ReleaseNativePipelineRefs(state.pixelConstantBuffers);
        ReleaseNativePipelineRefs(state.geometryConstantBuffers);
        ReleaseNativePipelineRefs(state.hullConstantBuffers);
        ReleaseNativePipelineRefs(state.domainConstantBuffers);
        ReleaseNativePipelineRefs(state.pixelResources);
        ReleaseNativePipelineRefs(state.pixelSamplers);
        state = {};
    }

    bool FreezeNativeReplayBuffer(ID3D11DeviceContext* ctx, ID3D11Buffer*& buffer, size_t& budget)
    {
        if (!ctx || !buffer) return true;
        D3D11_BUFFER_DESC desc{};
        buffer->GetDesc(&desc);
        if (!desc.ByteWidth || desc.ByteWidth > budget) return false;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return false;
        D3D11_BUFFER_DESC cloneDesc = desc;
        cloneDesc.Usage = D3D11_USAGE_DEFAULT;
        cloneDesc.CPUAccessFlags = 0;
        ID3D11Buffer* clone = nullptr;
        const HRESULT hr = dev->CreateBuffer(&cloneDesc, nullptr, &clone);
        dev->Release();
        if (FAILED(hr) || !clone) return false;
        ctx->CopyResource(clone, buffer);
        buffer->Release();
        buffer = clone;
        budget -= desc.ByteWidth;
        return true;
    }

    bool FreezeNativeReplayBuffers(ID3D11DeviceContext* ctx, NativePipelineSnapshot& out, UINT replacedPositionSlot)
    {
        // Transform constants and instance streams are often ring-buffered: copy the draw-time values instead of retaining
        // interface pointers (a later draw would overwrite a cached model with sprite or HUD data).
        constexpr size_t kNativeReplayBufferBudget = 1024 * 1024;
        size_t budget = kNativeReplayBufferBudget;
        auto freezeConstants = [&](auto& values)
        {
            for (ID3D11Buffer*& value : values)
                if (!FreezeNativeReplayBuffer(ctx, value, budget)) return false;
            return true;
        };
        if (!freezeConstants(out.vertexConstantBuffers) || !freezeConstants(out.pixelConstantBuffers) ||
            !freezeConstants(out.geometryConstantBuffers) || !freezeConstants(out.hullConstantBuffers) ||
            !freezeConstants(out.domainConstantBuffers)) return false;
        for (UINT slot = 0; slot < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; ++slot)
        {
            if (slot == replacedPositionSlot || !out.vertexBuffers[slot]) continue;
            D3D11_BUFFER_DESC desc{};
            out.vertexBuffers[slot]->GetDesc(&desc);
            if (desc.Usage == D3D11_USAGE_IMMUTABLE) continue;
            if (!FreezeNativeReplayBuffer(ctx, out.vertexBuffers[slot], budget)) return false;
        }
        return true;
    }

    bool CaptureNativePipelineSnapshot(ID3D11DeviceContext* ctx, NativePipelineSnapshot& out, bool freezeForReplay = false, UINT replacedPositionSlot = UINT_MAX)
    {
        if (!ctx) return false;
        ReleaseNativePipelineSnapshot(out);
        ctx->IAGetInputLayout(&out.inputLayout);
        ctx->IAGetPrimitiveTopology(&out.topology);
        ctx->IAGetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, out.vertexBuffers.data(), out.vertexStrides.data(), out.vertexOffsets.data());
        ctx->VSGetShader(&out.vertexShader, nullptr, nullptr);
        ctx->PSGetShader(&out.pixelShader, nullptr, nullptr);
        ctx->GSGetShader(&out.geometryShader, nullptr, nullptr);
        ctx->HSGetShader(&out.hullShader, nullptr, nullptr);
        ctx->DSGetShader(&out.domainShader, nullptr, nullptr);
        ctx->VSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, out.vertexConstantBuffers.data());
        ctx->PSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, out.pixelConstantBuffers.data());
        ctx->GSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, out.geometryConstantBuffers.data());
        ctx->HSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, out.hullConstantBuffers.data());
        ctx->DSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, out.domainConstantBuffers.data());
        ctx->PSGetShaderResources(0, static_cast<UINT>(out.pixelResources.size()), out.pixelResources.data());
        ctx->PSGetSamplers(0, static_cast<UINT>(out.pixelSamplers.size()), out.pixelSamplers.data());
        ctx->RSGetState(&out.rasterizer);
        ctx->OMGetBlendState(&out.blend, out.blendFactor, &out.sampleMask);
        ctx->OMGetDepthStencilState(&out.depthStencil, &out.stencilRef);
        out.viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetViewports(&out.viewportCount, out.viewports.data());
        out.scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetScissorRects(&out.scissorCount, out.scissors.data());
        for (ID3D11ShaderResourceView* resource : out.pixelResources)
            if (resource) { out.materialReady = true; break; }
        // Some material layouts predate the hook and bind no input-layout object: null is a valid state to restore on replay.
        out.valid = out.vertexShader && out.pixelShader && out.topology != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        if (out.valid && freezeForReplay) out.valid = FreezeNativeReplayBuffers(ctx, out, replacedPositionSlot);
        if (!out.valid) ReleaseNativePipelineSnapshot(out);
        return out.valid;
    }

    void BindNativePipelineSnapshot(ID3D11DeviceContext* ctx, const NativePipelineSnapshot& state)
    {
        ctx->IASetInputLayout(state.inputLayout);
        ctx->IASetPrimitiveTopology(state.topology);
        ctx->IASetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, state.vertexBuffers.data(), state.vertexStrides.data(), state.vertexOffsets.data());
        ctx->VSSetShader(state.vertexShader, nullptr, 0);
        ctx->PSSetShader(state.pixelShader, nullptr, 0);
        ctx->GSSetShader(state.geometryShader, nullptr, 0);
        ctx->HSSetShader(state.hullShader, nullptr, 0);
        ctx->DSSetShader(state.domainShader, nullptr, 0);
        ctx->VSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, state.vertexConstantBuffers.data());
        ctx->PSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, state.pixelConstantBuffers.data());
        ctx->GSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, state.geometryConstantBuffers.data());
        ctx->HSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, state.hullConstantBuffers.data());
        ctx->DSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, state.domainConstantBuffers.data());
        ctx->PSSetShaderResources(0, static_cast<UINT>(state.pixelResources.size()), state.pixelResources.data());
        ctx->PSSetSamplers(0, static_cast<UINT>(state.pixelSamplers.size()), state.pixelSamplers.data());
        ctx->RSSetState(state.rasterizer);
        if (state.viewportCount) ctx->RSSetViewports(state.viewportCount, state.viewports.data());
        if (state.scissorCount) ctx->RSSetScissorRects(state.scissorCount, state.scissors.data());
        ctx->OMSetBlendState(state.blend, state.blendFactor, state.sampleMask);
        ctx->OMSetDepthStencilState(state.depthStencil, state.stencilRef);
    }

    struct NativeCachedDraw
    {
        ModToolsNativeModelPreview preview{};
        std::string requestId;
        ID3D11Buffer* sourceVertexBuffer = nullptr;
        ID3D11Buffer* sourceIndexBuffer = nullptr;
        ID3D11Buffer* deformedVertexBuffer = nullptr;
        ID3D11Buffer* deformedIndexBuffer = nullptr;
        UINT positionSlot = 0;
        UINT stride = 0;
        UINT indexCount = 0;
        DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
        UINT startInstance = 0;
        bool instanced = false;
        NativePipelineSnapshot pipeline{};
        uint64_t lastUsedFrame = 0;
    };

    std::unordered_map<NativeDrawKey, NativeCachedDraw, NativeDrawKeyHash> g_nativeCachedDraws;

    void ReleaseNativeCachedDraw(NativeCachedDraw& cached)
    {
        if (cached.sourceVertexBuffer) cached.sourceVertexBuffer->Release();
        if (cached.sourceIndexBuffer) cached.sourceIndexBuffer->Release();
        if (cached.deformedVertexBuffer) cached.deformedVertexBuffer->Release();
        if (cached.deformedIndexBuffer) cached.deformedIndexBuffer->Release();
        ReleaseNativePipelineSnapshot(cached.pipeline);
        cached = {};
    }

    void ClearNativeDrawCaches()
    {
        for (auto& item : g_nativeCachedDraws) ReleaseNativeCachedDraw(item.second);
        g_nativeCachedDraws.clear();
        g_nativeRejectedDraws.clear();
        g_nativeProbeStates.clear();
    }

    void ClearNativeDrawCachesForRequests(const std::unordered_set<std::string>& requestIds)
    {
        if (requestIds.empty()) return;
        for (auto it = g_nativeCachedDraws.begin(); it != g_nativeCachedDraws.end(); )
        {
            if (requestIds.find(it->second.requestId) == requestIds.end()) { ++it; continue; }
            ReleaseNativeCachedDraw(it->second);
            it = g_nativeCachedDraws.erase(it);
        }
    }

    void TrimNativeDrawCaches(size_t limit)
    {
        while (g_nativeCachedDraws.size() > limit)
        {
            auto oldest = g_nativeCachedDraws.begin();
            for (auto it = std::next(oldest); it != g_nativeCachedDraws.end(); ++it)
                if (it->second.lastUsedFrame < oldest->second.lastUsedFrame) oldest = it;
            ReleaseNativeCachedDraw(oldest->second);
            g_nativeCachedDraws.erase(oldest);
        }
    }

    void ExpireNativeDrawCaches(uint64_t frame, uint64_t maxAge)
    {
        for (auto it = g_nativeCachedDraws.begin(); it != g_nativeCachedDraws.end(); )
        {
            if (frame <= it->second.lastUsedFrame || frame - it->second.lastUsedFrame <= maxAge) { ++it; continue; }
            ReleaseNativeCachedDraw(it->second);
            it = g_nativeCachedDraws.erase(it);
        }
    }

    bool NativePreviewEquivalent(const ModToolsNativeModelPreview& a, const ModToolsNativeModelPreview& b);

    bool HasReplayableNativeCachedDraw(const NativePreviewRequest& request)
    {
        for (const auto& item : g_nativeCachedDraws)
        {
            const NativeCachedDraw& cached = item.second;
            if (cached.requestId == request.preview.requestId && NativePreviewEquivalent(cached.preview, request.preview) &&
                cached.pipeline.valid && cached.pipeline.materialReady)
                return true;
        }
        return false;
    }

    bool NativePreviewEquivalent(const ModToolsNativeModelPreview& a, const ModToolsNativeModelPreview& b)
    {
        return a.enabled == b.enabled &&
            memcmp(a.anchor, b.anchor, sizeof a.anchor) == 0 &&
            memcmp(a.axis, b.axis, sizeof a.axis) == 0 &&
            memcmp(a.sideAxis, b.sideAxis, sizeof a.sideAxis) == 0 &&
            memcmp(a.upAxis, b.upAxis, sizeof a.upAxis) == 0 &&
            a.restLength == b.restLength && a.minSide == b.minSide && a.maxSide == b.maxSide &&
            a.minUp == b.minUp && a.maxUp == b.maxUp &&
            a.scaleX == b.scaleX && a.scaleY == b.scaleY && a.scaleZ == b.scaleZ &&
            a.anchorXAtMax == b.anchorXAtMax && a.anchorYAtMax == b.anchorYAtMax && a.anchorZAtMax == b.anchorZAtMax &&
            a.offsetX == b.offsetX && a.offsetY == b.offsetY && a.offsetZ == b.offsetZ &&
            a.mode == b.mode && a.curveSide == b.curveSide && a.curveUp == b.curveUp &&
            a.skirtDepth == b.skirtDepth && a.skirtFalloff == b.skirtFalloff &&
            a.sourcePositionCount == b.sourcePositionCount && a.sourcePositionStrideFloats == b.sourcePositionStrideFloats &&
            a.showOriginal == b.showOriginal && a.curveSegments == b.curveSegments &&
            strcmp(a.requestId, b.requestId) == 0;
    }

    bool MakeNativeDrawKey(ID3D11DeviceContext* ctx, const LayoutInfo& layout, UINT indexCount, UINT startIndex, INT baseVertex, UINT startInstance, bool instanced, NativeDrawKey& out)
    {
        if (!ctx) return false;
        ID3D11Buffer* vertexBuffer = nullptr;
        ID3D11Buffer* indexBuffer = nullptr;
        ID3D11InputLayout* inputLayout = nullptr;
        UINT stride = 0, vertexOffset = 0, indexOffset = 0;
        DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
        D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        ctx->IAGetVertexBuffers(layout.positionSlot, 1, &vertexBuffer, &stride, &vertexOffset);
        ctx->IAGetIndexBuffer(&indexBuffer, &indexFormat, &indexOffset);
        ctx->IAGetInputLayout(&inputLayout);
        ctx->IAGetPrimitiveTopology(&topology);
        out = { vertexBuffer, indexBuffer, inputLayout, topology, indexFormat, stride, vertexOffset, indexOffset, layout.positionSlot,
            layout.positionOffset, indexCount, startIndex, baseVertex, startInstance, instanced };
        if (vertexBuffer) vertexBuffer->Release();
        if (indexBuffer) indexBuffer->Release();
        if (inputLayout) inputLayout->Release();
        return out.vertexBuffer && out.indexBuffer && stride >= layout.positionOffset + sizeof(NativeVec) &&
            (indexFormat == DXGI_FORMAT_R16_UINT || indexFormat == DXGI_FORMAT_R32_UINT);
    }

    bool NativePositionKeyFor(NativeVec point, NativePositionKey& out)
    {
        constexpr float kQuantize = 256.0f;
        constexpr float kLimit = 8000000.0f;
        if (!IsFinite(point) || std::fabs(point.x) > kLimit || std::fabs(point.y) > kLimit || std::fabs(point.z) > kLimit) return false;
        out.x = static_cast<int32_t>(std::lround(point.x * kQuantize));
        out.y = static_cast<int32_t>(std::lround(point.y * kQuantize));
        out.z = static_cast<int32_t>(std::lround(point.z * kQuantize));
        return true;
    }

    bool SourceContains(const NativePositionSet& positions, NativeVec point)
    {
        NativePositionKey key{};
        if (!NativePositionKeyFor(point, key)) return false;
        // Imported XMODEL positions may travel through a packed GPU format: a one-quantum neighbourhood tolerates that
        // and is still far more specific than a model-sized bounding box.
        for (int z = -1; z <= 1; ++z)
        {
            for (int y = -1; y <= 1; ++y)
            {
                for (int x = -1; x <= 1; ++x)
                {
                    const NativePositionKey nearby{ key.x + x, key.y + y, key.z + z };
                    if (positions.keys.find(nearby) != positions.keys.end()) return true;
                }
            }
        }
        return false;
    }

    std::shared_ptr<const NativePositionSet> BuildNativePositionSet(const ModToolsNativeModelPreview& preview)
    {
        constexpr uint32_t kMaxSourcePositions = 300000;
        if (!preview.sourcePositions || preview.sourcePositionCount < 3 || preview.sourcePositionCount > kMaxSourcePositions ||
            preview.sourcePositionStrideFloats < 3 || preview.sourcePositionStrideFloats > 64)
        {
            return {};
        }
        auto result = std::make_shared<NativePositionSet>();
        result->keys.reserve(size_t(preview.sourcePositionCount) * 2);
        for (uint32_t index = 0; index < preview.sourcePositionCount; ++index)
        {
            const float* source = preview.sourcePositions + size_t(index) * preview.sourcePositionStrideFloats;
            NativePositionKey key{};
            if (!NativePositionKeyFor({ source[0], source[1], source[2] }, key)) return {};
            result->keys.insert(key);
        }
        return result->keys.size() >= 3 ? result : std::shared_ptr<const NativePositionSet>{};
    }

    bool CopyNativePreviews(std::shared_ptr<const NativePreviewDispatchSet>& out)
    {
        out = std::atomic_load_explicit(&g_nativeDispatchSet, std::memory_order_acquire);
        return out && !out->requests.empty();
    }

    bool NativePreviewShowsOriginal()
    {
        return g_nativeDispatchShowsOriginal.load(std::memory_order_acquire);
    }

    bool NativeCameraChanged(const render::CamParams& current)
    {
        if (!g_nativeProbeCameraValid)
        {
            g_nativeProbeCamera = current;
            g_nativeProbeCameraValid = true;
            return true;
        }
        bool changed = current.gameView != g_nativeProbeCamera.gameView || current.fovDeg != g_nativeProbeCamera.fovDeg || current.width != g_nativeProbeCamera.width || current.height != g_nativeProbeCamera.height;
        for (int i = 0; i != 3; ++i)
        {
            if (std::fabs(current.origin[i] - g_nativeProbeCamera.origin[i]) > 4.0f ||
                std::fabs(current.angles[i] - g_nativeProbeCamera.angles[i]) > 0.5f)
            {
                changed = true;
                break;
            }
        }
        if (changed) g_nativeProbeCamera = current;
        return changed;
    }

    void RequestNativeProbeBurst()
    {
        g_nativeProbeBurstFrames = kNativeProbeBurstFrames;
        g_nativeProbeAllowed.store(true, std::memory_order_release);
    }

    void MarkNativePreviewActive(const char* requestId)
    {
        if (!requestId || !*requestId) return;
        std::lock_guard<std::mutex> lock(g_nativeMutex);
        for (NativePreviewRequest& request : g_nativePreviews)
        {
            if (strcmp(request.preview.requestId, requestId) != 0) continue;
            request.status = MODTOOLS_NATIVE_PREVIEW_ACTIVE;
            ++request.drawsThisFrame;
            ++g_nativeDrawsThisFrame;
            return;
        }
    }

    bool ReadBufferRange(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Buffer* source, UINT begin, UINT bytes, std::vector<uint8_t>& out)
    {
        if (!dev || !ctx || !source || !bytes) return false;
        D3D11_BUFFER_DESC sourceDesc{};
        source->GetDesc(&sourceDesc);
        if (begin > sourceDesc.ByteWidth || bytes > sourceDesc.ByteWidth - begin) return false;
        D3D11_BUFFER_DESC staging{};
        staging.ByteWidth = bytes;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Buffer* copy = nullptr;
        if (FAILED(dev->CreateBuffer(&staging, nullptr, &copy)) || !copy) return false;
        D3D11_BOX box{};
        box.left = begin; box.right = begin + bytes; box.top = 0; box.bottom = 1; box.front = 0; box.back = 1;
        ctx->CopySubresourceRegion(copy, 0, 0, 0, 0, source, 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = ctx->Map(copy, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr) || !mapped.pData) { copy->Release(); return false; }
        out.resize(bytes);
        memcpy(out.data(), mapped.pData, bytes);
        ctx->Unmap(copy, 0);
        copy->Release();
        return true;
    }

    bool PreviewContains(const ModToolsNativeModelPreview& preview, NativeVec point)
    {
        const NativeVec anchor = NV(preview.anchor);
        const NativeVec rel = NVSub(point, anchor);
        const float along = NVDot(rel, NV(preview.axis));
        const float side = NVDot(rel, NV(preview.sideAxis));
        const float up = NVDot(rel, NV(preview.upAxis));
        const float sideHalf = (preview.maxSide - preview.minSide) * 0.5f;
        const float upHalf = (preview.maxUp - preview.minUp) * 0.5f;
        const float lengthPad = std::max(2.0f, preview.restLength * 0.035f);
        const float sidePad = std::max(2.0f, std::fabs(sideHalf) * 0.08f);
        const float upPad = std::max(2.0f, std::fabs(upHalf) * 0.08f);
        return IsFinite(point) && along >= -lengthPad && along <= preview.restLength + lengthPad &&
            side >= -sideHalf - sidePad && side <= sideHalf + sidePad &&
            up >= -upHalf - upPad && up <= upHalf + upPad;
    }

    float ResizeMin(float restMin, float restMax, float scale, int anchorAtMax, float offset)
    {
        const float span = restMax - restMin;
        const float baseMin = restMin + span * offset;
        const float baseMax = baseMin + span;
        return anchorAtMax ? baseMax - span * scale : baseMin;
    }

    float ResizeCoordinate(float restMin, float restMax, float scale, int anchorAtMax, float offset, float t)
    {
        const float minValue = ResizeMin(restMin, restMax, scale, anchorAtMax, offset);
        return minValue + (restMax - restMin) * scale * t;
    }

    uint32_t NativeCurveSegments(const ModToolsNativeModelPreview& preview)
    {
        if (preview.mode != 1) return 1;
        const uint32_t requested = preview.curveSegments ? preview.curveSegments : 8u;
        return std::max(2u, std::min(16u, requested));
    }

    NativeVec NativeExactCurveCenter(const ModToolsNativeModelPreview& preview, NativeVec minEnd, NativeVec axis, NativeVec sideAxis, NativeVec upAxis, float length, float u)
    {
        const float clamped = std::max(0.0f, std::min(1.0f, u));
        NativeVec result = NVAdd(minEnd, NVMul(axis, length * preview.scaleX * clamped));
        if (preview.mode == 1)
        {
            const float arch = 4.0f * clamped * (1.0f - clamped);
            result = NVAdd(result, NVMul(sideAxis, preview.curveSide * arch));
            result = NVAdd(result, NVMul(upAxis, preview.curveUp * arch));
        }
        return result;
    }

    struct NativeCurveFrame
    {
        NativeVec center;
        NativeVec axis;
        NativeVec side;
        NativeVec up;
    };

    NativeCurveFrame NativeFrameAt(const ModToolsNativeModelPreview& preview, NativeVec minEnd, NativeVec axis, NativeVec sideAxis, NativeVec upAxis, float length, float u)
    {
        const uint32_t segments = NativeCurveSegments(preview);
        const float clamped = std::max(0.0f, std::min(1.0f, u));
        const float scaled = clamped * static_cast<float>(segments);
        const uint32_t section = std::min<uint32_t>(segments - 1, static_cast<uint32_t>(std::floor(scaled)));
        const float startU = static_cast<float>(section) / static_cast<float>(segments);
        const float endU = static_cast<float>(section + 1) / static_cast<float>(segments);
        const float local = std::max(0.0f, std::min(1.0f, scaled - static_cast<float>(section)));
        const NativeVec start = NativeExactCurveCenter(preview, minEnd, axis, sideAxis, upAxis, length, startU);
        const NativeVec end = NativeExactCurveCenter(preview, minEnd, axis, sideAxis, upAxis, length, endU);
        NativeVec frameAxis = NVNormalize(NVSub(end, start));
        if (NVLength(frameAxis) < 0.0001f) frameAxis = axis;
        NativeVec frameSide = NVSub(sideAxis, NVMul(frameAxis, NVDot(sideAxis, frameAxis)));
        if (NVLength(frameSide) < 0.0001f) frameSide = NVSub(upAxis, NVMul(frameAxis, NVDot(upAxis, frameAxis)));
        frameSide = NVNormalize(frameSide);
        NativeVec frameUp = NVNormalize(NVCross(frameAxis, frameSide));
        if (NVDot(frameUp, upAxis) < 0.0f) frameUp = NVMul(frameUp, -1.0f);
        frameSide = NVNormalize(NVCross(frameUp, frameAxis));
        return { NVAdd(start, NVMul(NVSub(end, start), local)), frameAxis, frameSide, frameUp };
    }

    NativeVec DeformNativeVertex(const ModToolsNativeModelPreview& preview, NativeVec source)
    {
        const NativeVec axis = NV(preview.axis), sideAxis = NV(preview.sideAxis), upAxis = NV(preview.upAxis);
        const NativeVec anchor = NV(preview.anchor);
        const NativeVec rel = NVSub(source, anchor);
        const float length = std::max(0.0001f, preview.restLength);
        const float sideSpan = std::max(0.0001f, preview.maxSide - preview.minSide);
        const float upSpan = std::max(0.0001f, preview.maxUp - preview.minUp);
        const float along = std::max(0.0f, std::min(1.0f, NVDot(rel, axis) / length));
        const float across = std::max(0.0f, std::min(1.0f, (NVDot(rel, sideAxis) + sideSpan * 0.5f) / sideSpan));
        const float height = std::max(0.0f, std::min(1.0f, (NVDot(rel, upAxis) + upSpan * 0.5f) / upSpan));

        const NativeVec baseMin = NVAdd(anchor, NVMul(axis, length * preview.offsetX));
        const NativeVec baseMax = NVAdd(baseMin, NVMul(axis, length));
        const NativeVec minEnd = preview.anchorXAtMax ? NVSub(baseMax, NVMul(axis, length * preview.scaleX)) : baseMin;
        const NativeCurveFrame frame = NativeFrameAt(preview, minEnd, axis, sideAxis, upAxis, length, along);
        const float side = ResizeCoordinate(preview.minSide, preview.maxSide, preview.scaleY, preview.anchorYAtMax, preview.offsetY, across);
        const float up = ResizeCoordinate(preview.minUp, preview.maxUp, preview.scaleZ, preview.anchorZAtMax, preview.offsetZ, height);
        NativeVec result = NVAdd(frame.center, NVMul(frame.side, side - (preview.minSide + preview.maxSide) * 0.5f));
        result = NVAdd(result, NVMul(frame.up, up - (preview.minUp + preview.maxUp) * 0.5f));
        if (preview.mode == 2) result = NVSub(result, NVMul(frame.up, preview.skirtDepth * powf(1.0f - height, preview.skirtFalloff)));
        return result;
    }

    enum class NativeFormatKind
    {
        Unsupported,
        Float32,
        Float16,
        UNorm8,
        SNorm8,
        UNorm16,
        SNorm16,
        UNorm1010102
    };

    struct NativeFormatInfo
    {
        NativeFormatKind kind = NativeFormatKind::Unsupported;
        UINT components = 0;
        UINT bytes = 0;
    };

    NativeFormatInfo GetNativeFormatInfo(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R32_FLOAT: return { NativeFormatKind::Float32, 1, 4 };
        case DXGI_FORMAT_R32G32_FLOAT: return { NativeFormatKind::Float32, 2, 8 };
        case DXGI_FORMAT_R32G32B32_FLOAT: return { NativeFormatKind::Float32, 3, 12 };
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return { NativeFormatKind::Float32, 4, 16 };
        case DXGI_FORMAT_R16_FLOAT: return { NativeFormatKind::Float16, 1, 2 };
        case DXGI_FORMAT_R16G16_FLOAT: return { NativeFormatKind::Float16, 2, 4 };
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return { NativeFormatKind::Float16, 4, 8 };
        case DXGI_FORMAT_R8_UNORM: return { NativeFormatKind::UNorm8, 1, 1 };
        case DXGI_FORMAT_R8G8_UNORM: return { NativeFormatKind::UNorm8, 2, 2 };
        case DXGI_FORMAT_R8G8B8A8_UNORM: return { NativeFormatKind::UNorm8, 4, 4 };
        case DXGI_FORMAT_R8_SNORM: return { NativeFormatKind::SNorm8, 1, 1 };
        case DXGI_FORMAT_R8G8_SNORM: return { NativeFormatKind::SNorm8, 2, 2 };
        case DXGI_FORMAT_R8G8B8A8_SNORM: return { NativeFormatKind::SNorm8, 4, 4 };
        case DXGI_FORMAT_R16_UNORM: return { NativeFormatKind::UNorm16, 1, 2 };
        case DXGI_FORMAT_R16G16_UNORM: return { NativeFormatKind::UNorm16, 2, 4 };
        case DXGI_FORMAT_R16G16B16A16_UNORM: return { NativeFormatKind::UNorm16, 4, 8 };
        case DXGI_FORMAT_R16_SNORM: return { NativeFormatKind::SNorm16, 1, 2 };
        case DXGI_FORMAT_R16G16_SNORM: return { NativeFormatKind::SNorm16, 2, 4 };
        case DXGI_FORMAT_R16G16B16A16_SNORM: return { NativeFormatKind::SNorm16, 4, 8 };
        case DXGI_FORMAT_R10G10B10A2_UNORM: return { NativeFormatKind::UNorm1010102, 4, 4 };
        default: return {};
        }
    }

    float NativeHalfToFloat(uint16_t value)
    {
        const uint32_t sign = (uint32_t(value & 0x8000u)) << 16;
        const uint32_t exponent = (value >> 10) & 0x1Fu;
        uint32_t mantissa = value & 0x03FFu;
        uint32_t bits = 0;
        if (exponent == 0)
        {
            if (mantissa != 0)
            {
                uint32_t adjustedExponent = 127 - 14;
                while ((mantissa & 0x0400u) == 0) { mantissa <<= 1; --adjustedExponent; }
                mantissa &= 0x03FFu;
                bits = sign | (adjustedExponent << 23) | (mantissa << 13);
            }
            else bits = sign;
        }
        else if (exponent == 0x1Fu) bits = sign | 0x7F800000u | (mantissa << 13);
        else bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
        float result = 0.0f;
        memcpy(&result, &bits, sizeof result);
        return result;
    }

    uint16_t NativeFloatToHalf(float value)
    {
        uint32_t bits = 0;
        memcpy(&bits, &value, sizeof bits);
        const uint32_t sign = (bits >> 16) & 0x8000u;
        const int exponent = int((bits >> 23) & 0xFFu) - 127 + 15;
        const uint32_t mantissa = bits & 0x7FFFFFu;
        if (exponent <= 0)
        {
            if (exponent < -10) return static_cast<uint16_t>(sign);
            uint32_t rounded = (mantissa | 0x800000u) >> (1 - exponent);
            rounded = (rounded + 0x1000u) >> 13;
            return static_cast<uint16_t>(sign | rounded);
        }
        if (exponent >= 31)
        {
            if (((bits >> 23) & 0xFFu) == 0xFFu && mantissa) return static_cast<uint16_t>(sign | 0x7C00u | (mantissa >> 13));
            return static_cast<uint16_t>(sign | 0x7C00u);
        }
        return static_cast<uint16_t>(sign | (uint32_t(exponent) << 10) | ((mantissa + 0x1000u) >> 13));
    }

    bool DecodeNativeElement(DXGI_FORMAT format, const uint8_t* source, float out[4])
    {
        const NativeFormatInfo info = GetNativeFormatInfo(format);
        if (!source || info.kind == NativeFormatKind::Unsupported) return false;
        std::fill(out, out + 4, 0.0f);
        switch (info.kind)
        {
        case NativeFormatKind::Float32:
            memcpy(out, source, info.bytes);
            return true;
        case NativeFormatKind::Float16:
            for (UINT i = 0; i < info.components; ++i) { uint16_t value = 0; memcpy(&value, source + i * 2, sizeof value); out[i] = NativeHalfToFloat(value); }
            return true;
        case NativeFormatKind::UNorm8:
            for (UINT i = 0; i < info.components; ++i) out[i] = float(source[i]) / 255.0f;
            return true;
        case NativeFormatKind::SNorm8:
            for (UINT i = 0; i < info.components; ++i) out[i] = std::max(-1.0f, float(static_cast<int8_t>(source[i])) / 127.0f);
            return true;
        case NativeFormatKind::UNorm16:
            for (UINT i = 0; i < info.components; ++i) { uint16_t value = 0; memcpy(&value, source + i * 2, sizeof value); out[i] = float(value) / 65535.0f; }
            return true;
        case NativeFormatKind::SNorm16:
            for (UINT i = 0; i < info.components; ++i) { int16_t value = 0; memcpy(&value, source + i * 2, sizeof value); out[i] = std::max(-1.0f, float(value) / 32767.0f); }
            return true;
        case NativeFormatKind::UNorm1010102:
        {
            uint32_t value = 0; memcpy(&value, source, sizeof value);
            out[0] = float(value & 0x3FFu) / 1023.0f;
            out[1] = float((value >> 10) & 0x3FFu) / 1023.0f;
            out[2] = float((value >> 20) & 0x3FFu) / 1023.0f;
            out[3] = float((value >> 30) & 0x3u) / 3.0f;
            return true;
        }
        default: return false;
        }
    }

    bool EncodeNativeElement(DXGI_FORMAT format, const float in[4], uint8_t* destination)
    {
        const NativeFormatInfo info = GetNativeFormatInfo(format);
        if (!destination || info.kind == NativeFormatKind::Unsupported) return false;
        switch (info.kind)
        {
        case NativeFormatKind::Float32:
            memcpy(destination, in, info.bytes);
            return true;
        case NativeFormatKind::Float16:
            for (UINT i = 0; i < info.components; ++i) { const uint16_t value = NativeFloatToHalf(in[i]); memcpy(destination + i * 2, &value, sizeof value); }
            return true;
        case NativeFormatKind::UNorm8:
            for (UINT i = 0; i < info.components; ++i) destination[i] = static_cast<uint8_t>(std::lround(std::max(0.0f, std::min(1.0f, in[i])) * 255.0f));
            return true;
        case NativeFormatKind::SNorm8:
            for (UINT i = 0; i < info.components; ++i) destination[i] = static_cast<uint8_t>(static_cast<int8_t>(std::lround(std::max(-1.0f, std::min(1.0f, in[i])) * 127.0f)));
            return true;
        case NativeFormatKind::UNorm16:
            for (UINT i = 0; i < info.components; ++i) { const uint16_t value = static_cast<uint16_t>(std::lround(std::max(0.0f, std::min(1.0f, in[i])) * 65535.0f)); memcpy(destination + i * 2, &value, sizeof value); }
            return true;
        case NativeFormatKind::SNorm16:
            for (UINT i = 0; i < info.components; ++i) { const int16_t value = static_cast<int16_t>(std::lround(std::max(-1.0f, std::min(1.0f, in[i])) * 32767.0f)); memcpy(destination + i * 2, &value, sizeof value); }
            return true;
        case NativeFormatKind::UNorm1010102:
        {
            const uint32_t x = static_cast<uint32_t>(std::lround(std::max(0.0f, std::min(1.0f, in[0])) * 1023.0f));
            const uint32_t y = static_cast<uint32_t>(std::lround(std::max(0.0f, std::min(1.0f, in[1])) * 1023.0f));
            const uint32_t z = static_cast<uint32_t>(std::lround(std::max(0.0f, std::min(1.0f, in[2])) * 1023.0f));
            const uint32_t w = static_cast<uint32_t>(std::lround(std::max(0.0f, std::min(1.0f, in[3])) * 3.0f));
            const uint32_t value = x | (y << 10) | (z << 20) | (w << 30);
            memcpy(destination, &value, sizeof value);
            return true;
        }
        default: return false;
        }
    }

    bool NativeSemanticIsDirection(const std::string& semantic)
    {
        return _stricmp(semantic.c_str(), "NORMAL") == 0 || _stricmp(semantic.c_str(), "TANGENT") == 0 || _stricmp(semantic.c_str(), "BINORMAL") == 0;
    }

    bool NativeSemanticIsSkinning(const char* semantic)
    {
        return semantic && (_stricmp(semantic, "BLENDINDICES") == 0 || _stricmp(semantic, "BLENDWEIGHT") == 0);
    }

    NativeCurveFrame NativeFrameForSource(const ModToolsNativeModelPreview& preview, NativeVec source)
    {
        const NativeVec axis = NV(preview.axis), sideAxis = NV(preview.sideAxis), upAxis = NV(preview.upAxis);
        const NativeVec anchor = NV(preview.anchor);
        const float length = std::max(0.0001f, preview.restLength);
        const float along = std::max(0.0f, std::min(1.0f, NVDot(NVSub(source, anchor), axis) / length));
        const NativeVec baseMin = NVAdd(anchor, NVMul(axis, length * preview.offsetX));
        const NativeVec baseMax = NVAdd(baseMin, NVMul(axis, length));
        const NativeVec minEnd = preview.anchorXAtMax ? NVSub(baseMax, NVMul(axis, length * preview.scaleX)) : baseMin;
        return NativeFrameAt(preview, minEnd, axis, sideAxis, upAxis, length, along);
    }

    NativeVec TransformNativeDirection(const ModToolsNativeModelPreview& preview, NativeVec sourcePoint, NativeVec direction)
    {
        const NativeCurveFrame frame = NativeFrameForSource(preview, sourcePoint);
        const NativeVec sourceAxis = NV(preview.axis), sourceSide = NV(preview.sideAxis), sourceUp = NV(preview.upAxis);
        NativeVec result = NVAdd(NVMul(frame.axis, NVDot(direction, sourceAxis)), NVMul(frame.side, NVDot(direction, sourceSide)));
        result = NVAdd(result, NVMul(frame.up, NVDot(direction, sourceUp)));
        return NVNormalize(result);
    }

    bool WantsNativeTessellation(const ModToolsNativeModelPreview& preview, bool layoutEligible, UINT indexCount)
    {
        return preview.mode == 1 && layoutEligible && indexCount >= 3 && indexCount % 3 == 0 &&
            (std::fabs(preview.curveSide) > 0.001f || std::fabs(preview.curveUp) > 0.001f) && NativeCurveSegments(preview) > 1;
    }

    bool NativeTessellationBindingSafe(ID3D11DeviceContext* ctx, const LayoutInfo& layout)
    {
        if (layout.tessellationEligible) return true;
        if (!ctx || !layout.tessellationFallbackEligible) return false;
        bool safe = true;
        for (UINT slot = 0; slot < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; ++slot)
        {
            ID3D11Buffer* buffer = nullptr;
            UINT stride = 0, offset = 0;
            ctx->IAGetVertexBuffers(slot, 1, &buffer, &stride, &offset);
            if (buffer && slot != layout.positionSlot)
            {
                if (!g_nativeLoggedTessellationBinding)
                {
                    D3D11_BUFFER_DESC description{};
                    buffer->GetDesc(&description);
                    LOG("render: native curve tessellation deferred for generic layout, auxiliary slot=%u stride=%u offset=%u bytes=%u", slot, stride, offset, description.ByteWidth);
                    g_nativeLoggedTessellationBinding = true;
                }
                safe = false;
            }
            if (buffer) buffer->Release();
            if (!safe) break;
        }
        return safe;
    }

    uint32_t NativeTriangleTessellationLevel(const ModToolsNativeModelPreview& preview, NativeVec a, NativeVec b, NativeVec c)
    {
        const NativeVec anchor = NV(preview.anchor), axis = NV(preview.axis);
        const float length = std::max(0.0001f, preview.restLength);
        const float ua = std::max(0.0f, std::min(1.0f, NVDot(NVSub(a, anchor), axis) / length));
        const float ub = std::max(0.0f, std::min(1.0f, NVDot(NVSub(b, anchor), axis) / length));
        const float uc = std::max(0.0f, std::min(1.0f, NVDot(NVSub(c, anchor), axis) / length));
        const float span = std::max(ua, std::max(ub, uc)) - std::min(ua, std::min(ub, uc));
        return std::max(1u, std::min(NativeCurveSegments(preview), static_cast<uint32_t>(std::ceil(span * NativeCurveSegments(preview)))));
    }

    struct NativeTessellationPlan
    {
        std::vector<uint8_t> levels;
        uint64_t indexCount = 0;
        uint64_t vertexBytes = 0;
        bool active = false;
        bool limited = false;
    };

    NativeTessellationPlan BuildNativeTessellationPlan(const ModToolsNativeModelPreview& preview, const std::vector<UINT>& sourceIndices, const std::vector<uint8_t>& sourceVertices, UINT minVertex, UINT stride, UINT positionOffset)
    {
        NativeTessellationPlan plan{};
        if (sourceIndices.size() < 3 || sourceIndices.size() % 3 != 0) return plan;
        const size_t triangleCount = sourceIndices.size() / 3;
        plan.levels.resize(triangleCount, 1);
        for (size_t triangle = 0; triangle < triangleCount; ++triangle)
        {
            NativeVec points[3]{};
            bool valid = true;
            for (size_t corner = 0; corner < 3; ++corner)
            {
                const UINT sourceIndex = sourceIndices[triangle * 3 + corner];
                if (sourceIndex < minVertex) { valid = false; break; }
                const size_t offset = size_t(sourceIndex - minVertex) * stride + positionOffset;
                if (offset + sizeof(NativeVec) > sourceVertices.size()) { valid = false; break; }
                memcpy(&points[corner], sourceVertices.data() + offset, sizeof points[corner]);
                if (!IsFinite(points[corner])) { valid = false; break; }
            }
            if (valid) plan.levels[triangle] = static_cast<uint8_t>(NativeTriangleTessellationLevel(preview, points[0], points[1], points[2]));
        }
        constexpr uint64_t kIndexLimit = 1500000;
        constexpr uint64_t kVertexBytesLimit = 32ull * 1024ull * 1024ull;
        for (;;)
        {
            plan.indexCount = 0;
            plan.vertexBytes = 0;
            for (uint8_t level : plan.levels)
            {
                const uint64_t n = level;
                plan.indexCount += n * n * 3;
                plan.vertexBytes += (n + 1) * (n + 2) / 2 * stride;
            }
            if (plan.indexCount <= kIndexLimit && plan.vertexBytes <= kVertexBytesLimit) break;
            uint8_t highest = 1;
            for (uint8_t level : plan.levels) highest = std::max(highest, level);
            if (highest <= 1) { plan.limited = true; return plan; }
            for (uint8_t& level : plan.levels) if (level == highest) --level;
            plan.limited = true;
        }
        for (uint8_t level : plan.levels) if (level > 1) { plan.active = true; break; }
        return plan;
    }

    uint32_t NativeGridIndex(uint32_t level, uint32_t row, uint32_t column)
    {
        return row * (level + 1) - row * (row - 1) / 2 + column;
    }

    uint32_t AppendNativeTessellatedVertex(const ModToolsNativeModelPreview& preview, const LayoutInfo& layout, UINT stride,
        const std::array<const uint8_t*, 3>& sourceData, const std::array<NativeVec, 3>& sourcePoints, const std::array<float, 3>& weights,
        std::vector<uint8_t>& vertices)
    {
        size_t nearest = 0;
        if (weights[1] > weights[nearest]) nearest = 1;
        if (weights[2] > weights[nearest]) nearest = 2;
        const uint32_t result = static_cast<uint32_t>(vertices.size() / stride);
        const size_t begin = vertices.size();
        vertices.resize(begin + stride);
        uint8_t* output = vertices.data() + begin;
        memcpy(output, sourceData[nearest], stride);
        NativeVec source{};
        for (size_t corner = 0; corner < 3; ++corner) source = NVAdd(source, NVMul(sourcePoints[corner], weights[corner]));
        for (const LayoutInfo::Element& element : layout.elements)
        {
            const NativeFormatInfo info = GetNativeFormatInfo(element.format);
            if (info.kind == NativeFormatKind::Unsupported || element.offset + info.bytes > stride) continue;
            float a[4]{}, b[4]{}, c[4]{}, blended[4]{};
            if (!DecodeNativeElement(element.format, sourceData[0] + element.offset, a) ||
                !DecodeNativeElement(element.format, sourceData[1] + element.offset, b) ||
                !DecodeNativeElement(element.format, sourceData[2] + element.offset, c)) continue;
            for (UINT component = 0; component < info.components; ++component)
                blended[component] = a[component] * weights[0] + b[component] * weights[1] + c[component] * weights[2];
            // Vertex direction data follows the new curve frame, only for signed or floating formats (an UNORM normal convention is ambiguous).
            if (NativeSemanticIsDirection(element.semantic) && info.components >= 3 && info.kind != NativeFormatKind::UNorm8 &&
                info.kind != NativeFormatKind::UNorm16 && info.kind != NativeFormatKind::UNorm1010102)
            {
                const NativeVec direction = TransformNativeDirection(preview, source, { blended[0], blended[1], blended[2] });
                blended[0] = direction.x; blended[1] = direction.y; blended[2] = direction.z;
            }
            EncodeNativeElement(element.format, blended, output + element.offset);
        }
        NativeVec deformed = DeformNativeVertex(preview, source);
        memcpy(output + layout.positionOffset, &deformed, sizeof deformed);
        return result;
    }

    bool DrawNativePreview(ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndex, INT baseVertex, UINT startInstance, const ModToolsNativeModelPreview& preview, const NativePositionSet& sourcePositions, const LayoutInfo& layout, bool instanced, bool& sourceMismatch, NativeCachedDraw* cacheOut)
    {
        sourceMismatch = false;
        if (!ctx || (!instanced && !o_DrawIndexed) || (instanced && !o_DrawIndexedInstanced) || indexCount < 3 || indexCount > 400000) return false;
        D3D11_PRIMITIVE_TOPOLOGY topology{};
        ctx->IAGetPrimitiveTopology(&topology);
        if (topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) return false;

        ID3D11Buffer* vertexBuffer = nullptr;
        UINT stride = 0, vertexOffset = 0;
        ctx->IAGetVertexBuffers(layout.positionSlot, 1, &vertexBuffer, &stride, &vertexOffset);
        ID3D11Buffer* indexBuffer = nullptr;
        DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
        UINT indexOffset = 0;
        ctx->IAGetIndexBuffer(&indexBuffer, &indexFormat, &indexOffset);
        const UINT indexBytes = indexFormat == DXGI_FORMAT_R16_UINT ? 2u : (indexFormat == DXGI_FORMAT_R32_UINT ? 4u : 0u);
        if (!vertexBuffer || !indexBuffer || stride < layout.positionOffset + sizeof(NativeVec) || !indexBytes || indexCount * indexBytes > 4u * 1024u * 1024u)
        {
            if (vertexBuffer) vertexBuffer->Release();
            if (indexBuffer) indexBuffer->Release();
            return false;
        }

        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        std::vector<uint8_t> indexBytesData;
        const bool readIndices = dev && ReadBufferRange(dev, ctx, indexBuffer, indexOffset + startIndex * indexBytes, indexCount * indexBytes, indexBytesData);
        if (!readIndices)
        {
            if (dev) dev->Release();
            vertexBuffer->Release(); indexBuffer->Release();
            return false;
        }

        UINT minVertex = UINT_MAX, maxVertex = 0;
        std::vector<UINT> sourceIndices(indexCount);
        for (UINT i = 0; i < indexCount; ++i)
        {
            const UINT raw = indexBytes == 2 ? *reinterpret_cast<const uint16_t*>(indexBytesData.data() + size_t(i) * 2) : *reinterpret_cast<const uint32_t*>(indexBytesData.data() + size_t(i) * 4);
            const int64_t resolved = int64_t(raw) + int64_t(baseVertex);
            if (resolved < 0 || resolved > INT_MAX) { dev->Release(); vertexBuffer->Release(); indexBuffer->Release(); return false; }
            sourceIndices[i] = static_cast<UINT>(resolved);
            minVertex = std::min(minVertex, sourceIndices[i]); maxVertex = std::max(maxVertex, sourceIndices[i]);
        }
        const uint64_t vertexCount = uint64_t(maxVertex) - minVertex + 1;
        const uint64_t vertexBytes = vertexCount * stride;
        if (!vertexCount || vertexBytes > 12ull * 1024ull * 1024ull || vertexOffset + uint64_t(minVertex) * stride + vertexBytes > UINT_MAX)
        {
            dev->Release(); vertexBuffer->Release(); indexBuffer->Release(); return false;
        }
        std::vector<uint8_t> sourceVertices;
        if (!ReadBufferRange(dev, ctx, vertexBuffer, vertexOffset + minVertex * stride, static_cast<UINT>(vertexBytes), sourceVertices))
        {
            dev->Release(); vertexBuffer->Release(); indexBuffer->Release(); return false;
        }

        unsigned matched = 0;
        for (UINT sourceIndex : sourceIndices)
        {
            NativeVec point{};
            memcpy(&point, sourceVertices.data() + size_t(sourceIndex - minVertex) * stride + layout.positionOffset, sizeof point);
            if (PreviewContains(preview, point) && SourceContains(sourcePositions, point)) ++matched;
        }
        // Draws may include a few duplicated bookkeeping vertices: requiring 96% of the indexed positions to match
        // declines nearby models whose local bounds overlap this one.
        if (matched * 100u < indexCount * 96u)
        {
            sourceMismatch = true;
            dev->Release(); vertexBuffer->Release(); indexBuffer->Release(); return false;
        }

        std::vector<uint8_t> previewVertices;
        std::vector<uint32_t> previewIndices;
        NativeTessellationPlan tessellation{};
        if (WantsNativeTessellation(preview, NativeTessellationBindingSafe(ctx, layout), indexCount))
            tessellation = BuildNativeTessellationPlan(preview, sourceIndices, sourceVertices, minVertex, stride, layout.positionOffset);
        if (tessellation.limited && !g_nativeLoggedTessellationLimit)
        {
            g_nativeLoggedTessellationLimit = true;
            LOG("render: native curve tessellation reached its temporary-buffer budget and reduced detail for this draw");
        }
        if (tessellation.active)
        {
            previewVertices.reserve(static_cast<size_t>(tessellation.vertexBytes));
            previewIndices.reserve(static_cast<size_t>(tessellation.indexCount));
            for (size_t triangle = 0; triangle < tessellation.levels.size(); ++triangle)
            {
                const uint32_t level = tessellation.levels[triangle];
                const std::array<UINT, 3> indices = {
                    sourceIndices[triangle * 3], sourceIndices[triangle * 3 + 1], sourceIndices[triangle * 3 + 2]
                };
                std::array<const uint8_t*, 3> sourceData{};
                std::array<NativeVec, 3> sourcePoints{};
                for (size_t corner = 0; corner < 3; ++corner)
                {
                    sourceData[corner] = sourceVertices.data() + size_t(indices[corner] - minVertex) * stride;
                    memcpy(&sourcePoints[corner], sourceData[corner] + layout.positionOffset, sizeof sourcePoints[corner]);
                }
                std::vector<uint32_t> grid((level + 1) * (level + 2) / 2);
                for (uint32_t row = 0; row <= level; ++row)
                {
                    for (uint32_t column = 0; column <= level - row; ++column)
                    {
                        const std::array<float, 3> weights = {
                            1.0f - float(row + column) / float(level), float(row) / float(level), float(column) / float(level)
                        };
                        grid[NativeGridIndex(level, row, column)] = AppendNativeTessellatedVertex(preview, layout, stride, sourceData, sourcePoints, weights, previewVertices);
                    }
                }
                for (uint32_t row = 0; row < level; ++row)
                {
                    for (uint32_t column = 0; column < level - row; ++column)
                    {
                        previewIndices.push_back(grid[NativeGridIndex(level, row, column)]);
                        previewIndices.push_back(grid[NativeGridIndex(level, row + 1, column)]);
                        previewIndices.push_back(grid[NativeGridIndex(level, row, column + 1)]);
                        if (column + 1 < level - row)
                        {
                            previewIndices.push_back(grid[NativeGridIndex(level, row + 1, column)]);
                            previewIndices.push_back(grid[NativeGridIndex(level, row + 1, column + 1)]);
                            previewIndices.push_back(grid[NativeGridIndex(level, row, column + 1)]);
                        }
                    }
                }
            }
            if (!g_nativeLoggedTessellation)
            {
                g_nativeLoggedTessellation = true;
                LOG("render: native curve tessellation active, %u source indices expanded to %zu preview indices", indexCount, previewIndices.size());
            }
        }
        else
        {
            std::unordered_map<UINT, UINT> remap;
            remap.reserve(sourceIndices.size());
            previewVertices.reserve(size_t(sourceIndices.size()) * stride);
            previewIndices.reserve(sourceIndices.size());
            for (UINT sourceIndex : sourceIndices)
            {
                auto it = remap.find(sourceIndex);
                if (it == remap.end())
                {
                    const UINT compact = static_cast<UINT>(remap.size());
                    std::vector<uint8_t> vertex(stride);
                    memcpy(vertex.data(), sourceVertices.data() + size_t(sourceIndex - minVertex) * stride, stride);
                    NativeVec point{};
                    memcpy(&point, vertex.data() + layout.positionOffset, sizeof point);
                    point = DeformNativeVertex(preview, point);
                    memcpy(vertex.data() + layout.positionOffset, &point, sizeof point);
                    previewVertices.insert(previewVertices.end(), vertex.begin(), vertex.end());
                    it = remap.emplace(sourceIndex, compact).first;
                }
                previewIndices.push_back(it->second);
            }
        }
        if (previewVertices.empty() || previewIndices.empty() || previewVertices.size() > UINT_MAX) { dev->Release(); vertexBuffer->Release(); indexBuffer->Release(); return false; }
        const uint32_t outputVertexCount = static_cast<uint32_t>(previewVertices.size() / stride);
        const DXGI_FORMAT outputIndexFormat = outputVertexCount <= 65535 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        std::vector<uint8_t> outputIndices(previewIndices.size() * (outputIndexFormat == DXGI_FORMAT_R16_UINT ? 2 : 4));
        for (size_t i = 0; i < previewIndices.size(); ++i)
        {
            if (outputIndexFormat == DXGI_FORMAT_R16_UINT)
            {
                const uint16_t value = static_cast<uint16_t>(previewIndices[i]);
                memcpy(outputIndices.data() + i * 2, &value, sizeof value);
            }
            else memcpy(outputIndices.data() + i * 4, &previewIndices[i], sizeof(uint32_t));
        }
        D3D11_BUFFER_DESC vertexDesc{};
        vertexDesc.ByteWidth = static_cast<UINT>(previewVertices.size()); vertexDesc.Usage = D3D11_USAGE_DEFAULT; vertexDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vertexData{}; vertexData.pSysMem = previewVertices.data();
        D3D11_BUFFER_DESC indexDesc{};
        indexDesc.ByteWidth = static_cast<UINT>(outputIndices.size()); indexDesc.Usage = D3D11_USAGE_DEFAULT; indexDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA indexData{}; indexData.pSysMem = outputIndices.data();
        ID3D11Buffer* previewVertexBuffer = nullptr;
        ID3D11Buffer* previewIndexBuffer = nullptr;
        const bool created = SUCCEEDED(dev->CreateBuffer(&vertexDesc, &vertexData, &previewVertexBuffer)) && previewVertexBuffer &&
            SUCCEEDED(dev->CreateBuffer(&indexDesc, &indexData, &previewIndexBuffer)) && previewIndexBuffer;
        if (!created)
        {
            if (previewVertexBuffer) previewVertexBuffer->Release();
            if (previewIndexBuffer) previewIndexBuffer->Release();
            dev->Release(); vertexBuffer->Release(); indexBuffer->Release(); return false;
        }
        if (cacheOut)
        {
            // Reused only from a later call of this exact live draw key. Keeps deformed geometry, never renderer state
            // (the current submission's material pipeline stays bound).
            cacheOut->startInstance = startInstance;
            cacheOut->instanced = instanced;
        }
        const UINT zero = 0;
        ctx->IASetVertexBuffers(layout.positionSlot, 1, &previewVertexBuffer, &stride, &zero);
        ctx->IASetIndexBuffer(previewIndexBuffer, outputIndexFormat, 0);
        const UINT outputIndexCount = static_cast<UINT>(previewIndices.size());
        if (instanced) o_DrawIndexedInstanced(ctx, outputIndexCount, 1, 0, 0, startInstance);
        else o_DrawIndexed(ctx, outputIndexCount, 0, 0);
        ctx->IASetVertexBuffers(layout.positionSlot, 1, &vertexBuffer, &stride, &vertexOffset);
        ctx->IASetIndexBuffer(indexBuffer, indexFormat, indexOffset);
        if (cacheOut)
        {
            cacheOut->preview = preview;
            cacheOut->positionSlot = layout.positionSlot;
            cacheOut->stride = stride;
            cacheOut->indexCount = outputIndexCount;
            cacheOut->indexFormat = outputIndexFormat;
            cacheOut->deformedVertexBuffer = previewVertexBuffer; previewVertexBuffer->AddRef();
            cacheOut->deformedIndexBuffer = previewIndexBuffer; previewIndexBuffer->AddRef();
        }
        previewVertexBuffer->Release(); previewIndexBuffer->Release();
        dev->Release(); vertexBuffer->Release(); indexBuffer->Release();
        return true;
    }

    bool DrawCachedNativePreview(ID3D11DeviceContext* ctx, UINT startInstance, bool instanced, const NativeCachedDraw& cached)
    {
        if (!ctx || !cached.deformedVertexBuffer || !cached.deformedIndexBuffer || !cached.stride || !cached.indexCount) return false;
        ID3D11Buffer* vertexBuffer = nullptr;
        ID3D11Buffer* indexBuffer = nullptr;
        UINT stride = 0, vertexOffset = 0, indexOffset = 0;
        DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
        ctx->IAGetVertexBuffers(cached.positionSlot, 1, &vertexBuffer, &stride, &vertexOffset);
        ctx->IAGetIndexBuffer(&indexBuffer, &indexFormat, &indexOffset);
        if (!vertexBuffer || !indexBuffer)
        {
            if (vertexBuffer) vertexBuffer->Release();
            if (indexBuffer) indexBuffer->Release();
            return false;
        }
        const UINT zero = 0;
        ctx->IASetVertexBuffers(cached.positionSlot, 1, &cached.deformedVertexBuffer, &cached.stride, &zero);
        ctx->IASetIndexBuffer(cached.deformedIndexBuffer, cached.indexFormat, 0);
        if (instanced) o_DrawIndexedInstanced(ctx, cached.indexCount, 1, 0, 0, startInstance);
        else o_DrawIndexed(ctx, cached.indexCount, 0, 0);
        ctx->IASetVertexBuffers(cached.positionSlot, 1, &vertexBuffer, &stride, &vertexOffset);
        ctx->IASetIndexBuffer(indexBuffer, indexFormat, indexOffset);
        vertexBuffer->Release(); indexBuffer->Release();
        return true;
    }

    bool TryCachedNativePreview(ID3D11DeviceContext* ctx, const NativeDrawKey& key, UINT startInstance, bool instanced,
        const NativePreviewDispatchSet& previews)
    {
        auto found = g_nativeCachedDraws.find(key);
        if (found == g_nativeCachedDraws.end()) return false;
        NativeCachedDraw& cached = found->second;
        const auto request = std::find_if(previews.requests.begin(), previews.requests.end(), [&](const NativePreviewDispatchRequest& item)
        {
            return cached.requestId == item.preview.requestId && NativePreviewEquivalent(cached.preview, item.preview);
        });
        if (request == previews.requests.end() || !DrawCachedNativePreview(ctx, startInstance, instanced, cached))
        {
            ReleaseNativeCachedDraw(cached);
            g_nativeCachedDraws.erase(found);
            return false;
        }
        cached.lastUsedFrame = g_nativeFrame;
        MarkNativePreviewActive(cached.requestId.c_str());
        return true;
    }

    void RefreshNativeReplayPipeline(ID3D11DeviceContext* ctx, NativeCachedDraw& cached)
    {
        // A depth prepass often arrives before the textured pass with the same geometry key: keep the latter,
        // only it has the material shader/resources needed by the offscreen replay.
        NativePipelineSnapshot candidate{};
        if (!CaptureNativePipelineSnapshot(ctx, candidate, true, cached.positionSlot) || !candidate.materialReady) return;
        ReleaseNativePipelineSnapshot(cached.pipeline);
        std::swap(cached.pipeline, candidate);
        ReleaseNativePipelineSnapshot(candidate);
    }

    bool ReplayCachedNativePreview(ID3D11DeviceContext* ctx, const NativeCachedDraw& cached)
    {
        if (!ctx || !cached.pipeline.valid || !cached.pipeline.materialReady || !cached.deformedVertexBuffer || !cached.deformedIndexBuffer ||
            !cached.stride || !cached.indexCount) return false;
        NativePipelineSnapshot restore{};
        if (!CaptureNativePipelineSnapshot(ctx, restore)) return false;
        BindNativePipelineSnapshot(ctx, cached.pipeline);
        const UINT zero = 0;
        ctx->IASetVertexBuffers(cached.positionSlot, 1, &cached.deformedVertexBuffer, &cached.stride, &zero);
        ctx->IASetIndexBuffer(cached.deformedIndexBuffer, cached.indexFormat, 0);
        if (cached.instanced) o_DrawIndexedInstanced(ctx, cached.indexCount, 1, 0, 0, cached.startInstance);
        else o_DrawIndexed(ctx, cached.indexCount, 0, 0);
        BindNativePipelineSnapshot(ctx, restore);
        ReleaseNativePipelineSnapshot(restore);
        return true;
    }

    void ReplayMissingNativePreviews(IDXGISwapChain* sc)
    {
        if (!sc || !o_DrawIndexed || !o_DrawIndexedInstanced) return;
        ID3D11Device* dev = nullptr;
        if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev) return;
        ID3D11DeviceContext* ctx = nullptr;
        dev->GetImmediateContext(&ctx);
        dev->Release();
        if (!ctx) return;
        std::lock_guard<std::mutex> lock(g_nativeMutex);
        for (NativePreviewRequest& request : g_nativePreviews)
        {
            // A verified source draw already replaced itself in this scene pass. ACTIVE may also mean a replayable material state
            // was known when the HUD built (wire fallback suppressed), the draw count tells the two apart.
            if (request.drawsThisFrame != 0) continue;
            bool replayed = false;
            for (auto& item : g_nativeCachedDraws)
            {
                NativeCachedDraw& cached = item.second;
                if (cached.requestId != request.preview.requestId || !NativePreviewEquivalent(cached.preview, request.preview)) continue;
                if (!ReplayCachedNativePreview(ctx, cached)) continue;
                cached.lastUsedFrame = g_nativeFrame;
                replayed = true;
            }
            if (replayed)
            {
                request.status = MODTOOLS_NATIVE_PREVIEW_ACTIVE;
                ++request.drawsThisFrame;
                ++g_nativeDrawsThisFrame;
            }
            else
            {
                if (request.status == MODTOOLS_NATIVE_PREVIEW_ACTIVE)
                    request.status = MODTOOLS_NATIVE_PREVIEW_WAITING;
            }
        }
        ctx->Release();
    }

    HRESULT STDMETHODCALLTYPE hk_CreateInputLayout(ID3D11Device* dev, const D3D11_INPUT_ELEMENT_DESC* elements, UINT count, const void* signature, SIZE_T signatureBytes, ID3D11InputLayout** layout)
    {
        const HRESULT hr = o_CreateInputLayout(dev, elements, count, signature, signatureBytes, layout);
        if (FAILED(hr) || !layout || !*layout || !elements) return hr;
        LayoutInfo info{};
        std::array<UINT, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> nextOffsets{};
        std::array<bool, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> nextOffsetsKnown{};
        nextOffsetsKnown.fill(true);
        std::vector<UINT> perVertexSlots;
        bool hasSkinning = false;
        for (UINT i = 0; i < count; ++i)
        {
            const D3D11_INPUT_ELEMENT_DESC& element = elements[i];
            if (element.InputSlotClass != D3D11_INPUT_PER_VERTEX_DATA || element.InputSlot >= D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) continue;
            if (std::find(perVertexSlots.begin(), perVertexSlots.end(), element.InputSlot) == perVertexSlots.end()) perVertexSlots.push_back(element.InputSlot);
            hasSkinning = hasSkinning || NativeSemanticIsSkinning(element.SemanticName);
            const NativeFormatInfo format = GetNativeFormatInfo(element.Format);
            UINT offset = 0;
            bool offsetKnown = false;
            if (element.AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT)
            {
                offsetKnown = nextOffsetsKnown[element.InputSlot];
                offset = nextOffsets[element.InputSlot];
            }
            else { offsetKnown = true; offset = element.AlignedByteOffset; }
            if (format.kind != NativeFormatKind::Unsupported && offsetKnown)
            {
                nextOffsets[element.InputSlot] = offset + format.bytes;
                nextOffsetsKnown[element.InputSlot] = true;
                info.elements.push_back({ offset, element.Format, element.SemanticName ? element.SemanticName : "" });
            }
            else nextOffsetsKnown[element.InputSlot] = false;
            if (element.SemanticName && _stricmp(element.SemanticName, "POSITION") == 0 && element.SemanticIndex == 0 &&
                element.Format == DXGI_FORMAT_R32G32B32_FLOAT && offsetKnown)
            {
                info.hasPosition = true; info.positionOffset = offset; info.positionSlot = element.InputSlot;
            }
        }
        info.tessellationEligible = info.hasPosition && !hasSkinning && perVertexSlots.size() == 1 && perVertexSlots[0] == info.positionSlot;
        if (info.hasPosition)
        {
            std::lock_guard<std::mutex> lock(g_nativeMutex);
            g_layouts[*layout] = info;
        }
        return hr;
    }

    bool TryNativePreview(ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndex, INT baseVertex, UINT startInstance, bool instanced)
    {
        if (!g_nativeHadLiveDrawLastFrame.load(std::memory_order_acquire) && !g_nativeProbeAllowed.load(std::memory_order_acquire))
            return false;
        std::shared_ptr<const NativePreviewDispatchSet> previews;
        if (!CopyNativePreviews(previews)) return false;
        render::View view = render::View::None;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            view = g_view;
        }
        // The source-position proof also covers small backing and collision-looking sections: probe a few unseen streams per frame
        // and remember negative results.
        if (view != render::View::Cam || indexCount < 3)
        {
            return false;
        }
        ID3D11InputLayout* inputLayout = nullptr;
        ctx->IAGetInputLayout(&inputLayout);
        LayoutInfo info{};
        bool usable = false;
        if (inputLayout)
        {
            std::lock_guard<std::mutex> lock(g_nativeMutex);
            auto found = g_layouts.find(inputLayout);
            if (found != g_layouts.end()) { info = found->second; usable = info.hasPosition; }
        }
        if (inputLayout) inputLayout->Release();
        // Layouts created before the host started were not recorded by CreateInputLayout. Known GeoCache streams keep a source-local
        // float3 at the start of slot zero (record sizes vary), so every reasonable stream is a candidate. DrawNativePreview still
        // needs the exact XMODEL position-set proof, so brushes and other models are unaffected.
        if (!usable)
        {
            ID3D11Buffer* vertexBuffer = nullptr;
            UINT stride = 0, vertexOffset = 0;
            ctx->IAGetVertexBuffers(0, 1, &vertexBuffer, &stride, &vertexOffset);
            if (vertexBuffer) vertexBuffer->Release();
            if (stride >= sizeof(NativeVec) && stride <= 256)
            {
                info.hasPosition = true;
                info.positionSlot = 0;
                info.positionOffset = 0;
                info.tessellationFallbackEligible = true;
                info.elements.push_back({ 0, DXGI_FORMAT_R32G32B32_FLOAT, "POSITION" });
                usable = true;
                if (!g_nativeLoggedFallbackLayout)
                {
                    g_nativeLoggedFallbackLayout = true;
                    LOG("render: native preview is using the exact-source generic slot-zero fallback");
                }
            }
        }
        if (!usable)
        {
            if (!g_nativeLoggedNoLayout) { g_nativeLoggedNoLayout = true; LOG("render: native preview is waiting for a recorded float3 POSITION layout"); }
            return false;
        }
        NativeDrawKey key{};
        if (!MakeNativeDrawKey(ctx, info, indexCount, startIndex, baseVertex, startInstance, instanced, key)) return false;
        // A positive entry came from this same D3D geometry binding and replays only in the current live submission,
        // bypassing the probe budget (a stationary model otherwise alternated between native and fallback).
        if (TryCachedNativePreview(ctx, key, startInstance, instanced, *previews)) return true;
        // A D3D buffer pointer and draw range are not a durable model identity (Radiant reuses dynamic ranges),
        // so every live replacement keeps the exact position-set proof below.
        if (g_nativeRejectedDraws.find(key) != g_nativeRejectedDraws.end()) return false;
        // A staging readback syncs the immediate context with the GPU: one new proof per frame, proven submissions keep using their cache.
        if (g_nativeReadbacks >= 1) return false;
        ++g_nativeReadbacks;
        constexpr size_t kNativeProbeStateLimit = 512;
        if (g_nativeProbeStates.size() >= kNativeProbeStateLimit && g_nativeProbeStates.find(key) == g_nativeProbeStates.end())
            g_nativeProbeStates.erase(g_nativeProbeStates.begin());
        NativeProbeState& probe = g_nativeProbeStates[key];
        const size_t requestIndex = previews->requests.empty() ? 0 : probe.nextRequest % previews->requests.size();
        if (previews->requests.empty()) return false;
        const NativePreviewDispatchRequest& request = previews->requests[requestIndex];
        probe.nextRequest = (requestIndex + 1) % previews->requests.size();
        if (request.sourcePositions)
        {
            bool oneSourceMismatch = false;
            NativeCachedDraw cached{};
            if (DrawNativePreview(ctx, indexCount, startIndex, baseVertex, startInstance, request.preview, *request.sourcePositions,
                info, instanced, oneSourceMismatch, &cached))
            {
                cached.requestId = request.preview.requestId;
                cached.lastUsedFrame = g_nativeFrame;
                auto old = g_nativeCachedDraws.find(key);
                if (old != g_nativeCachedDraws.end())
                {
                    ReleaseNativeCachedDraw(old->second);
                    g_nativeCachedDraws.erase(old);
                }
                g_nativeCachedDraws.emplace(key, std::move(cached));
                TrimNativeDrawCaches(96);
                g_nativeProbeStates.erase(key);
                MarkNativePreviewActive(request.preview.requestId);
                return true;
            }
            if (oneSourceMismatch && ++probe.failures >= previews->requests.size())
            {
                constexpr size_t kNativeRejectedDrawLimit = 512;
                if (g_nativeRejectedDraws.size() >= kNativeRejectedDrawLimit) g_nativeRejectedDraws.erase(g_nativeRejectedDraws.begin());
                g_nativeRejectedDraws.insert(key);
                g_nativeProbeStates.erase(key);
            }
        }
        return false;
    }

    // Guards one draw detour: a nested entry only passes the submission through, deeper than kNativeDrawDepthLimit is dropped.
    struct NativeDrawScope
    {
        unsigned depth;
        NativeDrawScope() : depth(++t_nativeDrawDepth) {}
        ~NativeDrawScope() { --t_nativeDrawDepth; }
        bool nested() const { return depth > 1; }
        bool tooDeep() const
        {
            if (depth <= kNativeDrawDepthLimit) return false;
            if (!g_nativeLoggedDrawDepth)
            {
                g_nativeLoggedDrawDepth = true;
                LOG("render: native draw hook re-entered %u deep, dropping the nested submission", depth);
            }
            return true;
        }
    };

    void STDMETHODCALLTYPE hk_DrawIndexed(ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndex, INT baseVertex)
    {
        if (!o_DrawIndexed) return;
        g_nativeDrawIndexedSeen.store(true, std::memory_order_release);
        if (!g_nativeLoggedIndexed) { g_nativeLoggedIndexed = true; LOG("render: native preview observed DrawIndexed"); }
        NativeDrawScope scope;
        if (scope.tooDeep()) return;
        if (scope.nested()) { o_DrawIndexed(ctx, indexCount, startIndex, baseVertex); return; }
        const bool showOriginal = NativePreviewShowsOriginal();
        if (showOriginal) o_DrawIndexed(ctx, indexCount, startIndex, baseVertex);
        if (TryNativePreview(ctx, indexCount, startIndex, baseVertex, 0, false)) return;
        if (!showOriginal) o_DrawIndexed(ctx, indexCount, startIndex, baseVertex);
    }

    void STDMETHODCALLTYPE hk_DrawIndexedInstanced(ID3D11DeviceContext* ctx, UINT indexCountPerInstance, UINT instanceCount, UINT startIndex, INT baseVertex, UINT startInstance)
    {
        if (!o_DrawIndexedInstanced) return;
        g_nativeDrawIndexedInstancedSeen.store(true, std::memory_order_release);
        if (!g_nativeLoggedInstanced) { g_nativeLoggedInstanced = true; LOG("render: native preview observed DrawIndexedInstanced"); }
        NativeDrawScope scope;
        if (scope.tooDeep()) return;
        if (scope.nested()) { o_DrawIndexedInstanced(ctx, indexCountPerInstance, instanceCount, startIndex, baseVertex, startInstance); return; }
        const bool showOriginal = NativePreviewShowsOriginal();
        if (showOriginal) o_DrawIndexedInstanced(ctx, indexCountPerInstance, instanceCount, startIndex, baseVertex, startInstance);
        // A multi-instance batch needs a per-instance transform: only single-instance submissions are substituted, so a sibling model is never deformed.
        if (instanceCount == 1 && TryNativePreview(ctx, indexCountPerInstance, startIndex, baseVertex, startInstance, true)) return;
        if (!showOriginal) o_DrawIndexedInstanced(ctx, indexCountPerInstance, instanceCount, startIndex, baseVertex, startInstance);
    }

    void STDMETHODCALLTYPE hk_DrawIndexedInstancedIndirect(ID3D11DeviceContext* ctx, ID3D11Buffer* argsBuffer, UINT alignedByteOffsetForArgs)
    {
        if (!o_DrawIndexedInstancedIndirect) return;
        g_nativeDrawIndexedIndirectSeen.store(true, std::memory_order_release);
        if (!g_nativeLoggedIndirect) { g_nativeLoggedIndirect = true; LOG("render: native preview observed DrawIndexedInstancedIndirect"); }
        NativeDrawScope scope;
        if (scope.tooDeep()) return;
        if (scope.nested()) { o_DrawIndexedInstancedIndirect(ctx, argsBuffer, alignedByteOffsetForArgs); return; }
        const bool showOriginal = NativePreviewShowsOriginal();
        if (showOriginal) o_DrawIndexedInstancedIndirect(ctx, argsBuffer, alignedByteOffsetForArgs);
        // D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS is five UINTs: a temporary readback identifies an isolated single-instance
        // submission, whose vertices get the same source-bounds proof as the direct paths below.
        uint32_t args[5]{};
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        std::vector<uint8_t> data;
        const bool haveArgs = dev && argsBuffer && ReadBufferRange(dev, ctx, argsBuffer, alignedByteOffsetForArgs, sizeof args, data) && data.size() == sizeof args;
        if (dev) dev->Release();
        if (haveArgs)
        {
            memcpy(args, data.data(), sizeof args);
            if (args[1] == 1 && TryNativePreview(ctx, args[0], args[2], static_cast<INT>(args[3]), args[4], true)) return;
        }
        if (!showOriginal) o_DrawIndexedInstancedIndirect(ctx, argsBuffer, alignedByteOffsetForArgs);
    }

    // Bind one draw hook to the entry point the live context dispatches through. Never enable two hooks for the same draw
    // (the Amortized wrapper calls the plain entry, so the shared o_* trampoline recurses). If draws already arrive through the hook we hold, keep it.
    void RetargetNativeDrawHook(const void* target, void** current, void* detour, void** original,
                                std::atomic<bool>& seen, const char* what)
    {
        void* next = const_cast<void*>(target);
        if (!next || next == *current) return;
        if (*current && seen.load(std::memory_order_acquire))
        {
            if (!g_nativeLoggedRetargetKept)
            {
                g_nativeLoggedRetargetKept = true;
                LOG("render: live context moved %s to 0x%p, keeping the hook that already draws", what, next);
            }
            return;
        }
        if (g_nativeRetargets >= kNativeRetargetLimit)
        {
            if (!g_nativeLoggedRetargetLimit)
            {
                g_nativeLoggedRetargetLimit = true;
                LOG("render: live context draw targets keep moving, stopped retargeting after %u", kNativeRetargetLimit);
            }
            return;
        }
        ++g_nativeRetargets;
        if (*current)
        {
            // MH_RemoveHook releases the trampoline, so o_* must not be called until the new hook is in place.
            // Runs at the top of Present on the drawing thread, so no draw is in flight.
            MH_RemoveHook(*current);
            *current = nullptr;
            *original = nullptr;
        }
        void* trampoline = nullptr;
        if (MH_CreateHook(next, detour, &trampoline) == MH_OK && MH_EnableHook(next) == MH_OK)
        {
            *original = trampoline;
            *current = next;
            LOG("render: hooked live %s at 0x%p", what, next);
        }
        else
        {
            // Nothing is hooked now: call the entry point directly so the editor keeps drawing.
            *original = next;
            LOG("render: unable to hook live %s at 0x%p", what, next);
        }
    }

    // The live context's vtable may differ from the throwaway probe device's: bind the guarded draw hooks to it on first Present.
    void ProbeNativeDeviceFromSwapChain(IDXGISwapChain* sc)
    {
        if (!sc) return;
        ID3D11Device* device = nullptr;
        if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) || !device) return;
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        device->Release();
        if (!context) return;
        void** vtable = *reinterpret_cast<void***>(context);
        const void* drawIndexed = vtable[12];
        const void* drawIndexedInstanced = vtable[20];
        const void* drawIndexedIndirect = vtable[39];
        const void* psSetShaderResources = vtable[8];
        context->Release();

        if (!g_nativeLoggedLiveDevice)
        {
            g_nativeLoggedLiveDevice = true;
            LOG("render: live context draw targets DrawIndexed=0x%p DrawIndexedInstanced=0x%p DrawIndexedInstancedIndirect=0x%p", drawIndexed, drawIndexedInstanced, drawIndexedIndirect);
        }
        RetargetNativeDrawHook(drawIndexed, &g_nativeDrawIndexedTarget, reinterpret_cast<void*>(&hk_DrawIndexed),
            reinterpret_cast<void**>(&o_DrawIndexed), g_nativeDrawIndexedSeen, "DrawIndexed");
        RetargetNativeDrawHook(drawIndexedInstanced, &g_nativeDrawIndexedInstancedTarget, reinterpret_cast<void*>(&hk_DrawIndexedInstanced),
            reinterpret_cast<void**>(&o_DrawIndexedInstanced), g_nativeDrawIndexedInstancedSeen, "DrawIndexedInstanced");
        RetargetNativeDrawHook(drawIndexedIndirect, &g_nativeDrawIndexedIndirectTarget, reinterpret_cast<void*>(&hk_DrawIndexedInstancedIndirect),
            reinterpret_cast<void**>(&o_DrawIndexedInstancedIndirect), g_nativeDrawIndexedIndirectSeen, "DrawIndexedInstancedIndirect");
        RetargetNativeDrawHook(psSetShaderResources, &g_psSetShaderResourcesTarget, reinterpret_cast<void*>(&hk_PSSetShaderResources),
            reinterpret_cast<void**>(&o_PSSetShaderResources), g_psSetShaderResourcesSeen, "PSSetShaderResources");
    }

    void PushScreenLine(float x0, float y0, float x1, float y1, uint32_t rgba)
    {
        float w = g_frameW, h = g_frameH;
        g_frame.push_back({ x0 / w * 2 - 1, 1 - y0 / h * 2, rgba });
        g_frame.push_back({ x1 / w * 2 - 1, 1 - y1 / h * 2, rgba });
    }

    void PushScreenTriangle(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t rgba)
    {
        float w = g_frameW, h = g_frameH;
        g_triFrame.push_back({ x0 / w * 2 - 1, 1 - y0 / h * 2, rgba });
        g_triFrame.push_back({ x1 / w * 2 - 1, 1 - y1 / h * 2, rgba });
        g_triFrame.push_back({ x2 / w * 2 - 1, 1 - y2 / h * 2, rgba });
    }

    // ---- tiny stroke font (4x6 grid, y down) for labels, polylines per character --------
    const std::map<char, std::vector<std::vector<int>>>& Font()
    {
        static const std::map<char, std::vector<std::vector<int>>> f = {
            {'A', {{0,6,2,0,4,6}, {1,3,3,3}}},
            {'B', {{0,0,0,6}, {0,0,3,0,4,1,4,2,3,3,0,3}, {3,3,4,4,4,5,3,6,0,6}}},
            {'C', {{4,1,3,0,1,0,0,1,0,5,1,6,3,6,4,5}}},
            {'D', {{0,0,0,6}, {0,0,3,0,4,1,4,5,3,6,0,6}}},
            {'E', {{4,0,0,0,0,6,4,6}, {0,3,3,3}}},
            {'F', {{4,0,0,0,0,6}, {0,3,3,3}}},
            {'G', {{4,1,3,0,1,0,0,1,0,5,1,6,3,6,4,5,4,3,2,3}}},
            {'H', {{0,0,0,6}, {4,0,4,6}, {0,3,4,3}}},
            {'I', {{1,0,3,0}, {2,0,2,6}, {1,6,3,6}}},
            {'J', {{4,0,4,5,3,6,1,6,0,5}}},
            {'K', {{0,0,0,6}, {4,0,0,3,4,6}}},
            {'L', {{0,0,0,6,4,6}}},
            {'M', {{0,6,0,0,2,3,4,0,4,6}}},
            {'N', {{0,6,0,0,4,6,4,0}}},
            {'O', {{1,0,3,0,4,1,4,5,3,6,1,6,0,5,0,1,1,0}}},
            {'P', {{0,6,0,0,3,0,4,1,4,2,3,3,0,3}}},
            {'Q', {{1,0,3,0,4,1,4,5,3,6,1,6,0,5,0,1,1,0}, {2,4,4,6}}},
            {'R', {{0,6,0,0,3,0,4,1,4,2,3,3,0,3}, {2,3,4,6}}},
            {'S', {{4,1,3,0,1,0,0,1,0,2,1,3,3,3,4,4,4,5,3,6,1,6,0,5}}},
            {'T', {{0,0,4,0}, {2,0,2,6}}},
            {'U', {{0,0,0,5,1,6,3,6,4,5,4,0}}},
            {'V', {{0,0,2,6,4,0}}},
            {'W', {{0,0,1,6,2,3,3,6,4,0}}},
            {'X', {{0,0,4,6}, {4,0,0,6}}},
            {'Y', {{0,0,2,3,4,0}, {2,3,2,6}}},
            {'Z', {{0,0,4,0,0,6,4,6}}},
            {'0', {{1,0,3,0,4,1,4,5,3,6,1,6,0,5,0,1,1,0}, {0,5,4,1}}},
            {'1', {{1,1,2,0,2,6}, {1,6,3,6}}},
            {'2', {{0,1,1,0,3,0,4,1,4,2,0,6,4,6}}},
            {'3', {{0,0,4,0,2,2,4,4,4,5,3,6,1,6,0,5}}},
            {'4', {{3,6,3,0,0,4,4,4}}},
            {'5', {{4,0,0,0,0,3,3,3,4,4,4,5,3,6,0,6}}},
            {'6', {{4,0,1,0,0,1,0,5,1,6,3,6,4,5,4,4,3,3,0,3}}},
            {'7', {{0,0,4,0,1,6}}},
            {'8', {{1,0,3,0,4,1,4,2,3,3,1,3,0,2,0,1,1,0}, {1,3,0,4,0,5,1,6,3,6,4,5,4,4,3,3}}},
            {'9', {{4,3,1,3,0,2,0,1,1,0,3,0,4,1,4,5,3,6,0,6}}},
            {'-', {{1,3,3,3}}},
            {'_', {{0,6,4,6}}},
            {'.', {{2,5,2,6}}},
            {':', {{2,1,2,2}, {2,4,2,5}}},
            {'(', {{3,0,2,1,2,5,3,6}}},
            {')', {{1,0,2,1,2,5,1,6}}},
            {'/', {{4,0,0,6}}},
            {'+', {{2,1,2,5}, {0,3,4,3}}},
            {'=', {{0,2,4,2}, {0,4,4,4}}},
            {'!', {{2,0,2,4}, {2,5,2,6}}},
            {'?', {{0,1,1,0,3,0,4,1,4,2,2,3,2,4}, {2,5,2,6}}},
            {'\'', {{2,0,2,2}}},
            {',', {{2,5,1,6}}},
        };
        return f;
    }

    // Radiant's font, top-left at pixel (sx, sy) of the frame being built. Only inside the HUD pass, where the
    // HUD's ortho (origin = pane centre, +y up, 1 unit = 1 pixel) is still current.
    void EngineText(float sx, float sy, const std::string& text, uint32_t argb, float px)
    {
        float s = px / 2.0f;
        float lineH = EngineLineHeight() * s;
        float pos[3] = { floorf(sx) - g_frameW * 0.5f, g_frameH * 0.5f - floorf(sy + lineH), 0.0f };
        float xAdv[3] = { s, 0, 0 }, yDir[3] = { 0, -s, 0 };
        float rgba[4] = { ((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f, ((argb >> 24) & 0xFF) / 255.0f };
        reinterpret_cast<DrawText2D_t>(rt::Rva(rt::DrawText2D))(text.c_str(), EngineFontSlot(), pos, xAdv, yDir, rgba, nullptr, (unsigned char)Cfg().textBackground);
    }

    // Adds text starting at pixel (sx, sy) (top-left).
    void AddText(float sx, float sy, const std::string& text, uint32_t argb, float px)
    {
        uint32_t c = ToRGBA(argb);
        const auto& font = Font();
        float x = sx;
        for (char ch : text)
        {
            char up = char(toupper(static_cast<unsigned char>(ch)));
            auto it = font.find(up);
            if (it != font.end())
                for (const auto& poly : it->second)
                    for (size_t i = 0; i + 3 < poly.size(); i += 2)
                        PushScreenLine(x + poly[i] * px, sy + poly[i + 1] * px, x + poly[i + 2] * px, sy + poly[i + 3] * px, c);
            x += 6 * px;
        }
    }

    // Asks every plugin to draw into g_frame (which the caller has set up) plus the calibration marker.
    void BuildFrame(int view, float w, float h)
    {
        ModToolsDrawContext ctx{};
        ctx.view = view; ctx.width = int(w); ctx.height = int(h);
        memcpy(ctx.cam.origin, g_cam.origin, sizeof ctx.cam.origin); memcpy(ctx.cam.angles, g_cam.angles, sizeof ctx.cam.angles);
        ctx.cam.width = g_cam.width; ctx.cam.height = g_cam.height; ctx.cam.gameView = g_cam.gameView ? 1 : 0;
        ctx.xy.type = g_xy.type; memcpy(ctx.xy.origin, g_xy.origin, sizeof ctx.xy.origin); ctx.xy.scale = g_xy.scale;
        plugins::DispatchDraw(ctx);
        if (Cfg().markerEnabled)
        {
            const float S = 32.0f; const float* m = Cfg().marker;
            for (int axis = 0; axis < 3; ++axis) { float a[3] = { m[0], m[1], m[2] }, b[3] = { m[0], m[1], m[2] }; a[axis] -= S; b[axis] += S; render::LineWorld(a, b, 0xFFFF00FF); }
        }
    }

    ID3D11RenderTargetView* RtvFor(IDXGISwapChain* sc, ID3D11Device* dev)
    {
        auto it = g_rtvs.find(sc);
        if (it != g_rtvs.end()) return it->second;
        ID3D11Texture2D* bb = nullptr;
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) return nullptr;
        ID3D11RenderTargetView* rtv = nullptr;
        dev->CreateRenderTargetView(bb, nullptr, &rtv);
        bb->Release();
        g_rtvs[sc] = rtv;
        return rtv;
    }

    void DrawOverlay(IDXGISwapChain* sc)
    {
        ID3D11Device* dev = nullptr;
        if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev) return;
        DeviceRes& res = g_res[dev];
        if (!EnsureResources(dev, res)) { dev->Release(); return; }
        DXGI_SWAP_CHAIN_DESC desc{}; sc->GetDesc(&desc);
        float w = float(desc.BufferDesc.Width), h = float(desc.BufferDesc.Height);
        if (g_view == render::View::Cam) { g_cam.width = int(w); g_cam.height = int(h); g_camHwnd = desc.OutputWindow; }
        if (g_view == render::View::Cam)
        {
            ID3D11DeviceContext* lutContext = nullptr;
            dev->GetImmediateContext(&lutContext);
            if (lutContext) { ProcessCameraLutPreview(dev, lutContext); lutContext->Release(); }
        }

        // let every plugin add its lines and labels (the cam frame was already built in the HUD pass when it ran)
        bool prebuilt = g_view == render::View::Cam && g_camPrebuiltReady;
        g_camPrebuiltReady = false;
        if (prebuilt)
        {
            static int mismatchLogs = 3;
            if ((g_camPrebuiltW != w || g_camPrebuiltH != h) && mismatchLogs > 0) { --mismatchLogs; LOG("render: HUD pane %gx%g differs from the cam back buffer %gx%g", g_camPrebuiltW, g_camPrebuiltH, w, h); }
        }
        else
        {
            g_frame.clear(); g_triFrame.clear(); g_frameW = w; g_frameH = h; g_inFrame = true;
            BuildFrame(g_view == render::View::Cam ? 1 : 2, w, h);
            g_inFrame = false;
        }
        std::vector<Vtx>& lineVerts = prebuilt ? g_camPrebuilt : g_frame;
        std::vector<Vtx>& triVerts = prebuilt ? g_camTriPrebuilt : g_triFrame;
        const UINT vertexCount = UINT(lineVerts.size() + triVerts.size());
        if (vertexCount == 0 || !EnsureVB(dev, res, vertexCount)) { dev->Release(); return; }

        ID3D11DeviceContext* ctxd = nullptr; dev->GetImmediateContext(&ctxd);
        ID3D11RenderTargetView* rtv = RtvFor(sc, dev);
        if (!ctxd || !rtv) { if (ctxd) ctxd->Release(); dev->Release(); return; }

        // ---- save state -------------------------------------------------------------------
        ID3D11RenderTargetView* oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {}; ID3D11DepthStencilView* oldDsv = nullptr;
        ctxd->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);
        UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; D3D11_VIEWPORT oldVp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]; ctxd->RSGetViewports(&nvp, oldVp);
        ID3D11RasterizerState* oldRs = nullptr; ctxd->RSGetState(&oldRs);
        ID3D11BlendState* oldBs = nullptr; float oldBf[4]; UINT oldMask = 0; ctxd->OMGetBlendState(&oldBs, oldBf, &oldMask);
        ID3D11DepthStencilState* oldDss = nullptr; UINT oldRef = 0; ctxd->OMGetDepthStencilState(&oldDss, &oldRef);
        ID3D11InputLayout* oldIl = nullptr; ctxd->IAGetInputLayout(&oldIl);
        D3D11_PRIMITIVE_TOPOLOGY oldTopo; ctxd->IAGetPrimitiveTopology(&oldTopo);
        ID3D11Buffer* oldVb = nullptr; UINT oldStride = 0, oldOff = 0; ctxd->IAGetVertexBuffers(0, 1, &oldVb, &oldStride, &oldOff);
        ID3D11VertexShader* oldVs = nullptr; ctxd->VSGetShader(&oldVs, nullptr, nullptr);
        ID3D11PixelShader* oldPs = nullptr; ctxd->PSGetShader(&oldPs, nullptr, nullptr);
        ID3D11GeometryShader* oldGs = nullptr; ctxd->GSGetShader(&oldGs, nullptr, nullptr);
        ID3D11HullShader* oldHs = nullptr; ctxd->HSGetShader(&oldHs, nullptr, nullptr);
        ID3D11DomainShader* oldDs = nullptr; ctxd->DSGetShader(&oldDs, nullptr, nullptr);

        // ---- draw -------------------------------------------------------------------------
        D3D11_MAPPED_SUBRESOURCE map{};
        if (SUCCEEDED(ctxd->Map(res.vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
        {
            Vtx* dst = static_cast<Vtx*>(map.pData);
            if (!triVerts.empty()) { memcpy(dst, triVerts.data(), triVerts.size() * sizeof(Vtx)); dst += triVerts.size(); }
            if (!lineVerts.empty()) memcpy(dst, lineVerts.data(), lineVerts.size() * sizeof(Vtx));
            ctxd->Unmap(res.vb, 0);
            ctxd->OMSetRenderTargets(1, &rtv, nullptr);
            D3D11_VIEWPORT vp{ 0, 0, w, h, 0, 1 }; ctxd->RSSetViewports(1, &vp);
            ctxd->RSSetState(res.rs);
            float bf[4] = { 0, 0, 0, 0 }; ctxd->OMSetBlendState(res.bs, bf, 0xFFFFFFFF);
            ctxd->OMSetDepthStencilState(res.dss, 0);
            ctxd->IASetInputLayout(res.layout);
            UINT stride = sizeof(Vtx), off = 0; ctxd->IASetVertexBuffers(0, 1, &res.vb, &stride, &off);
            ctxd->VSSetShader(res.vs, nullptr, 0); ctxd->PSSetShader(res.ps, nullptr, 0);
            ctxd->GSSetShader(nullptr, nullptr, 0); ctxd->HSSetShader(nullptr, nullptr, 0); ctxd->DSSetShader(nullptr, nullptr, 0);
            if (!triVerts.empty())
            {
                ctxd->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ctxd->Draw(UINT(triVerts.size()), 0);
            }
            if (!lineVerts.empty())
            {
                ctxd->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
                ctxd->Draw(UINT(lineVerts.size()), UINT(triVerts.size()));
            }
            static int logged = 0;
            if (logged < 3) { ++logged; LOG("render: drew %zu overlay triangles and %zu lines in %s view (%gx%g)", triVerts.size() / 3, lineVerts.size() / 2, g_view == render::View::Cam ? "cam" : "xy", w, h); }
        }

        // ---- restore state ----------------------------------------------------------------
        ctxd->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
        for (auto* v : oldRtv) if (v) v->Release();
        if (oldDsv) oldDsv->Release();
        ctxd->RSSetViewports(nvp, oldVp);
        ctxd->RSSetState(oldRs); if (oldRs) oldRs->Release();
        ctxd->OMSetBlendState(oldBs, oldBf, oldMask); if (oldBs) oldBs->Release();
        ctxd->OMSetDepthStencilState(oldDss, oldRef); if (oldDss) oldDss->Release();
        ctxd->IASetInputLayout(oldIl); if (oldIl) oldIl->Release();
        ctxd->IASetPrimitiveTopology(oldTopo);
        ctxd->IASetVertexBuffers(0, 1, &oldVb, &oldStride, &oldOff); if (oldVb) oldVb->Release();
        ctxd->VSSetShader(oldVs, nullptr, 0); if (oldVs) oldVs->Release();
        ctxd->PSSetShader(oldPs, nullptr, 0); if (oldPs) oldPs->Release();
        ctxd->GSSetShader(oldGs, nullptr, 0); if (oldGs) oldGs->Release();
        ctxd->HSSetShader(oldHs, nullptr, 0); if (oldHs) oldHs->Release();
        ctxd->DSSetShader(oldDs, nullptr, 0); if (oldDs) oldDs->Release();
        ctxd->Release();
        dev->Release();
    }

    HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags)
    {
        ProbeNativeDeviceFromSwapChain(sc);
        render::View view; bool draw; int captureXY = -1;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            view = g_view; draw = g_pendingView != render::View::None && g_pendingView == g_view;
            if (draw) g_pendingView = render::View::None;
            // the swap chain presenting right after a 2D pass is that view's: remember its window (screenshots)
            if (view == render::View::XY && g_xy.type >= 0 && g_xy.type < 3 &&
                (!g_xyHwnd[g_xy.type] || !IsWindow(static_cast<HWND>(g_xyHwnd[g_xy.type])))) captureXY = g_xy.type;
        }
        if (captureXY >= 0)
        {
            DXGI_SWAP_CHAIN_DESC d{}; sc->GetDesc(&d);
            std::lock_guard<std::mutex> lock(g_mutex);
            g_xyHwnd[captureXY] = d.OutputWindow;
        }
        if (g_logBudget > 0)
        {
            DXGI_SWAP_CHAIN_DESC d{}; sc->GetDesc(&d);
            LOG("render: Present sc=0x%p hwnd=0x%p %ux%u view=%d draw=%d", sc, d.OutputWindow, d.BufferDesc.Width, d.BufferDesc.Height, int(view), draw);
            --g_logBudget;
        }
        if (draw) DrawOverlay(sc);
        return o_Present(sc, sync, flags);
    }

    HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags)
    {
        auto it = g_rtvs.find(sc);
        if (it != g_rtvs.end()) { if (it->second) it->second->Release(); g_rtvs.erase(it); }
        return o_ResizeBuffers(sc, n, w, h, f, flags);
    }
}

namespace render
{
    bool Init()
    {
        HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
        if (!d3d11) { LOG("render: d3d11.dll not loaded in Radiant; overlay disabled"); return false; }
        using CreateFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
        auto create = reinterpret_cast<CreateFn>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
        if (!create) { LOG("render: D3D11CreateDeviceAndSwapChain missing"); return false; }

        WNDCLASSEXW wc{ sizeof wc }; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"RadiantPlugins_DXProbe";
        RegisterClassExW(&wc);
        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
        DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 1; sd.BufferDesc.Width = 64; sd.BufferDesc.Height = 64; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0, got;
        IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
        HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &sd, &sc, &dev, &got, &ctx);
        if (FAILED(hr) || !sc) { LOG("render: probe swap chain failed 0x%08lX", hr); DestroyWindow(hwnd); return false; }
        void** vtbl = *reinterpret_cast<void***>(sc);
        void* pPresent = vtbl[8]; void* pResize = vtbl[13];
        void** deviceVtbl = *reinterpret_cast<void***>(dev);
        void** contextVtbl = *reinterpret_cast<void***>(ctx);
        void* pCreateInputLayout = deviceVtbl[11];
        void* pDrawIndexed = contextVtbl[12];
        void* pDrawIndexedInstanced = contextVtbl[20];
        void* pDrawIndexedInstancedIndirect = contextVtbl[39];
        void* pPSSetShaderResources = contextVtbl[8];
        sc->Release(); ctx->Release(); dev->Release(); DestroyWindow(hwnd);

        bool ok = MH_CreateHook(pPresent, &hk_Present, reinterpret_cast<void**>(&o_Present)) == MH_OK && MH_EnableHook(pPresent) == MH_OK;
        ok &= MH_CreateHook(pResize, &hk_ResizeBuffers, reinterpret_cast<void**>(&o_ResizeBuffers)) == MH_OK && MH_EnableHook(pResize) == MH_OK;
        const bool layoutHooked = MH_CreateHook(pCreateInputLayout, &hk_CreateInputLayout, reinterpret_cast<void**>(&o_CreateInputLayout)) == MH_OK &&
            MH_EnableHook(pCreateInputLayout) == MH_OK;
        const bool drawHooked = MH_CreateHook(pDrawIndexed, &hk_DrawIndexed, reinterpret_cast<void**>(&o_DrawIndexed)) == MH_OK &&
            MH_EnableHook(pDrawIndexed) == MH_OK;
        const bool instancedHooked = MH_CreateHook(pDrawIndexedInstanced, &hk_DrawIndexedInstanced, reinterpret_cast<void**>(&o_DrawIndexedInstanced)) == MH_OK &&
            MH_EnableHook(pDrawIndexedInstanced) == MH_OK;
        const bool indirectHooked = MH_CreateHook(pDrawIndexedInstancedIndirect, &hk_DrawIndexedInstancedIndirect, reinterpret_cast<void**>(&o_DrawIndexedInstancedIndirect)) == MH_OK &&
            MH_EnableHook(pDrawIndexedInstancedIndirect) == MH_OK;
        const bool lutHooked = MH_CreateHook(pPSSetShaderResources, &hk_PSSetShaderResources, reinterpret_cast<void**>(&o_PSSetShaderResources)) == MH_OK &&
            MH_EnableHook(pPSSetShaderResources) == MH_OK;
        g_cameraLutHookReady.store(lutHooked, std::memory_order_release);
        g_nativeDrawIndexedTarget = pDrawIndexed;
        g_nativeDrawIndexedInstancedTarget = pDrawIndexedInstanced;
        g_nativeDrawIndexedIndirectTarget = pDrawIndexedInstancedIndirect;
        g_psSetShaderResourcesTarget = pPSSetShaderResources;
        {
            std::lock_guard<std::mutex> lock(g_nativeMutex);
            g_nativeHooksReady = layoutHooked && drawHooked && instancedHooked;
            for (NativePreviewRequest& request : g_nativePreviews)
                request.status = g_nativeHooksReady ? MODTOOLS_NATIVE_PREVIEW_WAITING : MODTOOLS_NATIVE_PREVIEW_UNAVAILABLE;
        }
        LOG("render: dxgi hooks %s (Present=0x%p ResizeBuffers=0x%p)", ok ? "installed" : "FAILED", pPresent, pResize);
        LOG("render: in-memory camera LUT resource hook %s (PSSetShaderResources=0x%p)", lutHooked ? "ready" : "unavailable", pPSSetShaderResources);
        LOG("render: native material preview %s (CreateInputLayout=0x%p DrawIndexed=0x%p DrawIndexedInstanced=0x%p DrawIndexedInstancedIndirect=0x%p indirect=%s)", g_nativeHooksReady ? "ready" : "unavailable", pCreateInputLayout, pDrawIndexed, pDrawIndexedInstanced, pDrawIndexedInstancedIndirect, indirectHooked ? "yes" : "no");
        return ok;
    }

    void Shutdown() {}

    void SetCameraLutPreview(const ModToolsCameraLutPreview* preview)
    {
        std::shared_ptr<const CameraLutPreviewData> old = std::atomic_load(&g_cameraLutData);
        std::shared_ptr<CameraLutPreviewData> next = std::make_shared<CameraLutPreviewData>();
        if (old) *next = *old;
        bool unsupported = false;
        if (preview && preview->size >= offsetof(ModToolsCameraLutPreview, enabled) + sizeof(preview->enabled))
        {
            if (preview->width) next->width = preview->width;
            if (preview->height) next->height = preview->height;
            const bool dimensionsOk = next->width > 0 && next->height > 0 && next->width <= 1024 && next->height <= 1024;
            const size_t values = dimensionsOk ? size_t(next->width) * next->height * 3 : 0;
            if (!dimensionsOk) unsupported = preview->enabled != 0;
            if (preview->referenceRgb && values)
                next->referenceRgb = std::make_shared<const std::vector<uint16_t>>(preview->referenceRgb, preview->referenceRgb + values);
            if (preview->previewRgb && values)
                next->previewRgb = std::make_shared<const std::vector<uint16_t>>(preview->previewRgb, preview->previewRgb + values);
            next->enabled = preview->enabled != 0;
        }
        else next->enabled = false;

        const size_t expected = next->width <= 1024 && next->height <= 1024 ? size_t(next->width) * next->height * 3 : 0;
        if (next->enabled && (!expected ||
            (next->referenceRgb && next->referenceRgb->size() != expected) ||
            (next->previewRgb && next->previewRgb->size() != expected)))
        {
            next->enabled = false;
            unsupported = true;
        }
        next->generation = old ? old->generation + 1 : 1;
        std::atomic_store(&g_cameraLutData, std::shared_ptr<const CameraLutPreviewData>(next));
        LOG("render: camera LUT preview request enabled=%d size=%ux%u generation=%llu reference=%s preview=%s",
            next->enabled ? 1 : 0, next->width, next->height,
            static_cast<unsigned long long>(next->generation),
            next->referenceRgb && !next->referenceRgb->empty() ? "yes" : "no",
            next->previewRgb && !next->previewRgb->empty() ? "yes" : "no");
        g_cameraLutWasBound.store(false, std::memory_order_release);
        if (!next->enabled) ClearCameraLutResources();
        else if (preview && preview->referenceRgb) ClearCameraLutResources();
        if (unsupported)
            g_cameraLutStatus.store(MODTOOLS_CAMERA_LUT_PREVIEW_UNSUPPORTED, std::memory_order_release);
        else if (next->enabled && next->referenceRgb && next->previewRgb)
            g_cameraLutStatus.store(MODTOOLS_CAMERA_LUT_PREVIEW_WAITING, std::memory_order_release);
        else if (next->enabled)
            g_cameraLutStatus.store(MODTOOLS_CAMERA_LUT_PREVIEW_UNSUPPORTED, std::memory_order_release);
        else
            g_cameraLutStatus.store(MODTOOLS_CAMERA_LUT_PREVIEW_OFF, std::memory_order_release);
    }

    int CameraLutPreviewStatus()
    {
        const std::shared_ptr<const CameraLutPreviewData> data = std::atomic_load(&g_cameraLutData);
        if (data && data->enabled && !g_cameraLutHookReady.load(std::memory_order_acquire))
            return MODTOOLS_CAMERA_LUT_PREVIEW_UNSUPPORTED;
        return g_cameraLutStatus.load(std::memory_order_acquire);
    }

    void BeginCamView(const CamParams& p)
    {
        static int logged = 0;
        if (logged < 3) { ++logged; LOG("render: cam origin=(%.1f %.1f %.1f) angles=(%.1f %.1f %.1f) pane=%dx%d gameView=%d", p.origin[0], p.origin[1], p.origin[2], p.angles[0], p.angles[1], p.angles[2], p.width, p.height, p.gameView); }
        {
            std::lock_guard<std::mutex> lock(g_mutex); g_cam = p; g_view = View::Cam; g_pendingView = View::Cam;
        }
        g_cameraDrawActive.store(true, std::memory_order_release);
        const bool cameraChanged = NativeCameraChanged(p);
        ++g_nativeFrame;
        g_nativeReadbacks = 0;
        bool hadLiveDraw = false;
        {
            std::lock_guard<std::mutex> lock(g_nativeMutex);
            // Keep a proven live-draw cache only while its D3D resources are plausibly part of the current scene
            // (never a culling or Present-time replay cache).
            ExpireNativeDrawCaches(g_nativeFrame, 180);
            hadLiveDraw = g_nativeDrawsThisFrame != 0;
            g_nativeDrawsThisFrame = 0;
            for (NativePreviewRequest& request : g_nativePreviews)
            {
                request.drawsThisFrame = 0;
                // Only an exact live source submission may mark a preview active: replaying a cached engine material state at Present is unsafe.
                request.status = !g_nativeHooksReady ? MODTOOLS_NATIVE_PREVIEW_UNAVAILABLE : MODTOOLS_NATIVE_PREVIEW_WAITING;
            }
        }
        g_nativeHadLiveDrawLastFrame.store(hadLiveDraw, std::memory_order_release);
        if (cameraChanged) g_nativeProbeBurstFrames = kNativeProbeBurstFrames;
        const bool probeAllowed = hadLiveDraw || g_nativeProbeBurstFrames != 0;
        g_nativeProbeAllowed.store(probeAllowed, std::memory_order_release);
        if (!hadLiveDraw && g_nativeProbeBurstFrames != 0) --g_nativeProbeBurstFrames;
    }
    void BeginXYView(const XYParams& p)   { std::lock_guard<std::mutex> lock(g_mutex); g_xy = p; g_view = View::XY; g_pendingView = View::XY; }
    void EndView()                        { g_cameraDrawActive.store(false, std::memory_order_release); }

    void SetEngineTextAvailable(bool hooked) { g_engineTextHooked = hooked; }

    void SetNativeModelPreview(const ModToolsNativeModelPreview* preview)
    {
        if (!preview || !preview->enabled)
        {
            SetNativeModelPreviewSet(nullptr, 0);
            return;
        }
        ModToolsNativeModelPreview legacy{};
        memcpy(&legacy, preview, std::min<size_t>(preview->size, sizeof legacy));
        legacy.size = sizeof legacy;
        if (!legacy.requestId[0]) strcpy_s(legacy.requestId, "legacy");
        SetNativeModelPreviewSet(&legacy, 1);
    }

    void SetNativeModelPreviewSet(const ModToolsNativeModelPreview* previews, size_t count)
    {
        constexpr size_t kNativePreviewV1Size = offsetof(ModToolsNativeModelPreview, showOriginal);
        constexpr size_t kNativePreviewSetLimit = 32;
        std::lock_guard<std::mutex> lock(g_nativeMutex);
        if (!previews || !count)
        {
            g_nativePreviews.clear();
            std::atomic_store_explicit(&g_nativeDispatchSet, std::shared_ptr<const NativePreviewDispatchSet>{}, std::memory_order_release);
            g_nativeDispatchShowsOriginal.store(false, std::memory_order_release);
            g_nativeHadLiveDrawLastFrame.store(false, std::memory_order_release);
            g_nativeProbeBurstFrames = 0;
            g_nativeProbeAllowed.store(false, std::memory_order_release);
            ClearNativeDrawCaches();
            return;
        }
        if (count > kNativePreviewSetLimit)
        {
            LOG("render: native material preview declined an oversized request set (%zu)", count);
            return;
        }
        std::vector<NativePreviewRequest> next;
        next.reserve(count);
        std::unordered_set<std::string> requestIds;
        std::unordered_set<std::string> changedRequestIds;
        bool changed = g_nativePreviews.size() != count;
        for (size_t index = 0; index < count; ++index)
        {
            const ModToolsNativeModelPreview* incoming = previews + index;
            if (incoming->size < kNativePreviewV1Size || !incoming->enabled)
            {
                LOG("render: native material preview declined an invalid request at index %zu", index);
                return;
            }
            ModToolsNativeModelPreview normalized{};
            memcpy(&normalized, incoming, std::min<size_t>(incoming->size, sizeof normalized));
            normalized.size = sizeof normalized;
            if (!normalized.requestId[0])
            {
                LOG("render: native material preview set member %zu has no request id", index);
                return;
            }
            if (!requestIds.insert(normalized.requestId).second)
            {
                LOG("render: native material preview set has duplicate request id %s", normalized.requestId);
                return;
            }
            if (!normalized.curveSegments) normalized.curveSegments = 8;
            normalized.curveSegments = std::max(2u, std::min(16u, normalized.curveSegments));
            if (!std::isfinite(normalized.restLength) || normalized.restLength <= 0.0001f ||
                !std::isfinite(normalized.scaleX) || !std::isfinite(normalized.scaleY) || !std::isfinite(normalized.scaleZ))
            {
                LOG("render: native material preview set member %s has invalid deformation data", normalized.requestId);
                return;
            }

            const NativePreviewRequest* oldRequest = nullptr;
            std::shared_ptr<const NativePositionSet> positions;
            for (const NativePreviewRequest& old : g_nativePreviews)
            {
                if (strcmp(old.preview.requestId, normalized.requestId) != 0) continue;
                oldRequest = &old;
                // The old preview has a nulled source pointer: compare the raw incoming proof with the plugin's stable source-cache pointer before reusing it.
                if (old.preview.sourcePositionCount == normalized.sourcePositionCount &&
                    old.preview.sourcePositionStrideFloats == normalized.sourcePositionStrideFloats &&
                    old.sourceInput == normalized.sourcePositions)
                    positions = old.sourcePositions;
                break;
            }
            if (!positions)
            {
                positions = BuildNativePositionSet(normalized);
                if (!positions)
                {
                    if (!g_nativeLoggedSourceSetInvalid)
                    {
                        g_nativeLoggedSourceSetInvalid = true;
                        LOG("render: native material preview declined an invalid or oversized source position set");
                    }
                    return;
                }
            }
            ModToolsNativeModelPreview stored = normalized;
            // The position proof is copied: null this non-semantic input so cached-request comparisons stay stable.
            stored.sourcePositions = nullptr;
            NativePreviewRequest request{};
            request.preview = stored;
            request.sourcePositions = std::move(positions);
            request.sourceInput = normalized.sourcePositions;
            request.status = g_nativeHooksReady ? MODTOOLS_NATIVE_PREVIEW_WAITING : MODTOOLS_NATIVE_PREVIEW_UNAVAILABLE;
            if (!oldRequest || !NativePreviewEquivalent(oldRequest->preview, stored))
            {
                changed = true;
                changedRequestIds.insert(stored.requestId);
            }
            else
            {
                request.status = oldRequest->status;
                request.drawsThisFrame = oldRequest->drawsThisFrame;
            }
            next.push_back(std::move(request));
        }
        for (const NativePreviewRequest& old : g_nativePreviews)
        {
            if (requestIds.find(old.preview.requestId) != requestIds.end()) continue;
            changed = true;
            changedRequestIds.insert(old.preview.requestId);
        }
        g_nativeLoggedSourceSetInvalid = false;
        if (!changed) return;
        g_nativePreviews = std::move(next);
        auto dispatch = std::make_shared<NativePreviewDispatchSet>();
        dispatch->requests.reserve(g_nativePreviews.size());
        bool showOriginal = false;
        for (const NativePreviewRequest& request : g_nativePreviews)
        {
            dispatch->requests.push_back({ request.preview, request.sourcePositions });
            showOriginal = showOriginal || (request.preview.enabled && request.preview.showOriginal != 0);
        }
        std::atomic_store_explicit(&g_nativeDispatchSet,
            std::shared_ptr<const NativePreviewDispatchSet>(std::move(dispatch)), std::memory_order_release);
        g_nativeDispatchShowsOriginal.store(showOriginal, std::memory_order_release);
        RequestNativeProbeBurst();
        // Publishing a second model must not evict other models' proven GPU buffers: only changed or removed requests lose their cache
        // (rejection and probe state are set-wide).
        ClearNativeDrawCachesForRequests(changedRequestIds);
        g_nativeRejectedDraws.clear();
        g_nativeProbeStates.clear();
        g_nativeLoggedNoLayout = false;
    }

    int NativeModelPreviewStatus()
    {
        std::lock_guard<std::mutex> lock(g_nativeMutex);
        if (g_nativePreviews.empty()) return MODTOOLS_NATIVE_PREVIEW_UNAVAILABLE;
        int status = MODTOOLS_NATIVE_PREVIEW_UNAVAILABLE;
        for (const NativePreviewRequest& request : g_nativePreviews)
        {
            if (request.status == MODTOOLS_NATIVE_PREVIEW_ACTIVE) return MODTOOLS_NATIVE_PREVIEW_ACTIVE;
            if (request.status == MODTOOLS_NATIVE_PREVIEW_WAITING) status = MODTOOLS_NATIVE_PREVIEW_WAITING;
        }
        return status;
    }

    unsigned NativeModelPreviewDrawCount()
    {
        std::lock_guard<std::mutex> lock(g_nativeMutex);
        return g_nativeDrawsThisFrame;
    }

    int NativeModelPreviewStatusForId(const char* requestId)
    {
        if (!requestId || !*requestId) return MODTOOLS_NATIVE_PREVIEW_UNAVAILABLE;
        std::lock_guard<std::mutex> lock(g_nativeMutex);
        for (const NativePreviewRequest& request : g_nativePreviews)
            if (strcmp(request.preview.requestId, requestId) == 0) return request.status;
        return MODTOOLS_NATIVE_PREVIEW_UNAVAILABLE;
    }

    unsigned NativeModelPreviewDrawCountForId(const char* requestId)
    {
        if (!requestId || !*requestId) return 0;
        std::lock_guard<std::mutex> lock(g_nativeMutex);
        for (const NativePreviewRequest& request : g_nativePreviews)
            if (strcmp(request.preview.requestId, requestId) == 0) return request.drawsThisFrame;
        return 0;
    }

    void CamHudPass(int hudW, int hudH, int argW, int argH)
    {
        if (hudW <= 0 || hudH <= 0 || !EngineFontReady()) return;     // Radiant's HUD returns early on an empty pane too
        static int logged = 0;
        if (logged < 3) { ++logged; LOG("render: HUD pass pane %dx%d (OnRender passed %dx%d)", hudW, hudH, argW, argH); }
        { std::lock_guard<std::mutex> lock(g_mutex); g_cam.width = hudW; g_cam.height = hudH; }
        float w = float(hudW), h = float(hudH);
        g_frame.clear(); g_triFrame.clear(); g_frameW = w; g_frameH = h; g_inFrame = true; g_textPass = true;
        BuildFrame(1, w, h);
        g_textPass = false; g_inFrame = false;
        g_camPrebuilt.swap(g_frame); g_frame.clear();
        g_camTriPrebuilt.swap(g_triFrame); g_triFrame.clear();
        g_camPrebuiltW = w; g_camPrebuiltH = h; g_camPrebuiltReady = true;
    }
    CamParams LastCam()                   { std::lock_guard<std::mutex> lock(g_mutex); return g_cam; }

    // Radiant keeps the VERTICAL field of view fixed and lets the horizontal one follow the pane's aspect:
    //   editor view: "Fov" pref (default 100) = horizontal FOV of a 16:9 view, GAME VIEW: cg_fov (65) = horizontal FOV of a 4:3 view
    // focal = (paneH / 2) / tan(vfov / 2), tan(vfov / 2) = tan(hfov / 2) * (9/16 or 3/4). Cam_GetFov's live value (c.fovDeg) wins, the ini numbers are the fallback.
    float TanHalfVertical(const CamParams& c)
    {
        if (c.fovDeg > 1.0f && c.fovDeg < 179.0f) return tanf(c.fovDeg * 0.5f * kDeg) * (9.0f / 16.0f);
        return c.gameView ? tanf(Cfg().gameFov * 0.5f * kDeg) * 0.75f : tanf(Cfg().fovDegrees * 0.5f * kDeg) * (9.0f / 16.0f);
    }

    bool ProjectCam(const CamParams& c, const float p[3], float& sx, float& sy)
    {
        float f[3], r[3], u[3];
        AngleVectors(c.angles, f, r, u);
        float d[3] = { p[0] - c.origin[0], p[1] - c.origin[1], p[2] - c.origin[2] };
        float z = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
        if (z < 1.0f) return false;
        float x = d[0] * r[0] + d[1] * r[1] + d[2] * r[2];
        float y = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
        float paneW = float(c.width), paneH = float(c.height);
        float cx = paneW * 0.5f, cy = paneH * 0.5f;
        float focal = (paneH * 0.5f) / TanHalfVertical(c);
        sx = cx + x / z * focal;
        sy = cy - y / z * focal;
        return true;
    }

    void* CamWindow()                     { std::lock_guard<std::mutex> lock(g_mutex); return g_camHwnd; }
    void* XYWindow(int type)
    {
        if (type < 0 || type > 2) return nullptr;
        std::lock_guard<std::mutex> lock(g_mutex);
        void* h = g_xyHwnd[type];
        return h && IsWindow(static_cast<HWND>(h)) ? h : nullptr;
    }

    bool InFrame() { return g_inFrame; }

    bool ProjectCurrent(const float p[3], float& sx, float& sy)
    {
        if (!g_inFrame) return false;
        return g_view == View::Cam ? ProjectCam(g_cam, p, sx, sy) : ProjectXY(g_xy, g_frameW, g_frameH, p, sx, sy);
    }

    void LineWorld(const float a[3], const float b[3], uint32_t argb)
    {
        if (!g_inFrame) return;
        float x0, y0, x1, y1;
        if (!ProjectCurrent(a, x0, y0) || !ProjectCurrent(b, x1, y1)) return;
        PushScreenLine(x0, y0, x1, y1, ToRGBA(argb));
    }

    void LineScreen(float x0, float y0, float x1, float y1, uint32_t argb)
    {
        if (!g_inFrame) return;
        PushScreenLine(x0, y0, x1, y1, ToRGBA(argb));
    }

    void TriangleWorld(const float a[3], const float b[3], const float c[3], uint32_t argb)
    {
        if (!g_inFrame) return;
        float x0, y0, x1, y1, x2, y2;
        if (!ProjectCurrent(a, x0, y0) || !ProjectCurrent(b, x1, y1) || !ProjectCurrent(c, x2, y2)) return;
        PushScreenTriangle(x0, y0, x1, y1, x2, y2, ToRGBA(argb));
    }

    void Text(float x, float y, const std::string& text, uint32_t argb, float px)
    {
        if (!g_inFrame || text.empty()) return;
        if (px <= 0) px = 2.0f;
        if (g_textPass) EngineText(x, y, text, argb, px);
        else AddText(x, y, text, argb, px);
    }

    bool MeasureText(const std::string& text, float px, int view, float& width, float& height)
    {
        if (px <= 0) px = 2.0f;
        bool engine = g_inFrame ? g_textPass : (view == 1 && EngineFontReady());
        if (engine) { width = EngineTextWidth(text, px / 2.0f); height = EngineLineHeight() * px / 2.0f; }
        else { width = 6.0f * px * float(text.size()); height = 6.0f * px; }
        return engine;
    }
}
