#include "gpu.h"

#include <d3dcompiler.h>

#include <cmath>

#include "rc/platform.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace rcs {

GpuInfo DescribeAdapter(IDXGIAdapter* adapter) {
    GpuInfo info;
    DXGI_ADAPTER_DESC desc{};
    if (adapter && SUCCEEDED(adapter->GetDesc(&desc))) {
        info.luid = desc.AdapterLuid;
        info.vendorId = desc.VendorId;
        info.name = Narrow(desc.Description);
        info.dedicatedVram = desc.DedicatedVideoMemory;
    }
    return info;
}

ComPtr<IDXGIAdapter1> PreferredAdapter() {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    }
    return adapter;
}

ComPtr<IDXGIAdapter1> AdapterByLuid(LUID luid) {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
    }
    return adapter;
}

ComPtr<ID3D11Device> CreateDevice(IDXGIAdapter* adapter, ComPtr<ID3D11DeviceContext>* context) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   flags, levels, 2, D3D11_SDK_VERSION, &device, nullptr, &ctx);
    if (FAILED(hr)) {
        RC_LOGE("D3D11CreateDevice failed: %s", HResultString(hr).c_str());
        return nullptr;
    }
    // Encoders and capture run on other threads.
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(device.As(&mt))) mt->SetMultithreadProtected(TRUE);
    if (context) *context = ctx;
    return device;
}

ComPtr<ID3DBlob> CompileShader(const char* source, const char* entry, const char* target) {
    ComPtr<ID3DBlob> blob, errors;
    const HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
    if (FAILED(hr)) {
        RC_LOGE("Shader compile failed (%s): %s", entry,
                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return nullptr;
    }
    return blob;
}

static const char* kQuadShader = R"(
cbuffer C : register(b0) { float4 dstRect; float4 uvRect; float4 rot; };
Texture2D tex : register(t0);
SamplerState samp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    float2 c = float2(id & 1, id >> 1);           // 0..1 quad corners (triangle strip)
    V o;
    o.pos = float4(dstRect.x + c.x * dstRect.z, dstRect.y - c.y * dstRect.w, 0, 1);
    float2 uv = c - 0.5;                           // rotate around center
    uv = float2(uv.x * rot.x - uv.y * rot.y, uv.x * rot.y + uv.y * rot.x) + 0.5;
    o.uv = uvRect.xy + uv * uvRect.zw;
    return o;
}
float4 ps(V i) : SV_Target { return tex.Sample(samp, i.uv); }
)";

bool QuadRenderer::Init(ID3D11Device* device) {
    auto vs = CompileShader(kQuadShader, "vs", "vs_5_0");
    auto ps = CompileShader(kQuadShader, "ps", "ps_5_0");
    if (!vs || !ps) return false;
    device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vs);
    device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_ps);

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(Constants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&cb, nullptr, &m_cb);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&sd, &m_linear);

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&bd, &m_blendAlpha);
    // out = src * (1 - dst) + dst * (1 - src): src = 1 inverts, src = 0 keeps.
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_COLOR;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                                               D3D11_COLOR_WRITE_ENABLE_BLUE;
    device->CreateBlendState(&bd, &m_blendInvert);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = FALSE;
    rd.DepthClipEnable = TRUE;
    device->CreateRasterizerState(&rd, &m_raster);
    return m_vs && m_ps && m_cb;
}

void QuadRenderer::Draw(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srv, ID3D11RenderTargetView* rtv,
                        UINT rtWidth, UINT rtHeight, float dstX, float dstY, float dstW, float dstH, Blend blend,
                        int uvRotation, float uvX, float uvY, float uvW, float uvH) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    auto* c = static_cast<Constants*>(mapped.pData);
    c->dstRect[0] = dstX / rtWidth * 2.f - 1.f;
    c->dstRect[1] = 1.f - dstY / rtHeight * 2.f;
    c->dstRect[2] = dstW / rtWidth * 2.f;
    c->dstRect[3] = dstH / rtHeight * 2.f;
    c->uvRect[0] = uvX;
    c->uvRect[1] = uvY;
    c->uvRect[2] = uvW;
    c->uvRect[3] = uvH;
    const float angle = float(uvRotation) * 3.14159265f / 180.f;
    c->rotation[0] = std::cos(angle);
    c->rotation[1] = std::sin(angle);
    c->rotation[2] = c->rotation[3] = 0;
    ctx->Unmap(m_cb.Get(), 0);

    D3D11_VIEWPORT vp{0, 0, float(rtWidth), float(rtHeight), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(m_raster.Get());
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    const float factor[4] = {0, 0, 0, 0};
    ID3D11BlendState* bs = blend == Blend::Alpha ? m_blendAlpha.Get() : blend == Blend::Invert ? m_blendInvert.Get() : nullptr;
    ctx->OMSetBlendState(bs, factor, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, m_cb.GetAddressOf());
    ctx->PSSetShader(m_ps.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, m_linear.GetAddressOf());
    ctx->Draw(4, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);
    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
}

}  // namespace rcs
