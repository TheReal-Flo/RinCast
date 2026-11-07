#include "foveator.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../gpu.h"
#include "rc/platform.h"

namespace rcs {

namespace {

// For each pixel of the squeezed frame, the place in the full one it shows: along each axis of
// an eye, [0, a) and [a + F, 1] are at density s, [a, a + F) at full density (rc::VrFoveationMetadata).
// Squeezed parts average the pixels they cover instead of picking one, or fine detail would shimmer.
const char* kShader = R"(
Texture2D src : register(t0);
SamplerState samp : register(s0);
cbuffer Params : register(b0) {
    float4 start;    // left eye x, y, right eye x, y
    float4 layout;   // F, density x, density y
    float4 texel;    // 1 / full frame width, 1 / height
};
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    float2 c = float2((id << 1) & 2, id & 2);
    V o; o.pos = float4(c * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = c; return o;
}
// Full-picture position (0..1) of a squeezed position e, along one axis of an eye.
float Unsqueeze(float e, float a, float s) {
    float F = layout.x;
    float p = e * (F + (1 - F) * s);
    if (p < a * s) return p / s;
    if (p < a * s + F) return a + (p - a * s);
    return a + F + (p - a * s - F) / s;
}
float4 ps(V i) : SV_Target {
    float eye = i.uv.x < 0.5 ? 0 : 1;
    float2 a = eye < 0.5 ? start.xy : start.zw;
    float ux = Unsqueeze(i.uv.x * 2 - eye, a.x, layout.y);
    float uy = Unsqueeze(i.uv.y, a.y, layout.z);
    // Stay inside this eye's half.
    float2 uv = float2(clamp((ux + eye) * 0.5, eye * 0.5 + texel.x, eye * 0.5 + 0.5 - texel.x), uy);
    // Taps a quarter of the covered stretch either side of the centre.
    float2 o = float2(ux < a.x || ux >= a.x + layout.x ? 0.25 * texel.x / layout.y : 0,
                      uy < a.y || uy >= a.y + layout.x ? 0.25 * texel.y / layout.z : 0);
    if (o.x == 0 && o.y == 0) return src.Sample(samp, uv);
    return 0.25 * (src.Sample(samp, uv + float2(-o.x, -o.y)) + src.Sample(samp, uv + float2(o.x, -o.y)) +
                   src.Sample(samp, uv + float2(-o.x, o.y)) + src.Sample(samp, uv + float2(o.x, o.y)));
}
)";

struct Params {
    float start[4];
    float layout[4];
    float texel[4];
};

}  // namespace

uint32_t Foveator::EncodedSize(uint32_t full) {
    const double size = full * (kFoveaSize + (1.0 - kFoveaSize) * 0.5);
    return std::max<uint32_t>(64, uint32_t(std::lround(size / 16)) * 16);
}

bool Foveator::Init(ID3D11Device* device, uint32_t eyeWidth, uint32_t eyeHeight, uint32_t encodedEyeWidth,
                    uint32_t encodedEyeHeight) {
    m_device = device;
    m_eyeWidth = eyeWidth;
    m_eyeHeight = eyeHeight;
    m_encodedWidth = encodedEyeWidth;
    m_encodedHeight = encodedEyeHeight;
    m_lastInput = nullptr;
    m_inputSrv.Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = encodedEyeWidth * 2;
    td.Height = encodedEyeHeight;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device->CreateTexture2D(&td, nullptr, &m_output);
    if (FAILED(hr) || FAILED(device->CreateRenderTargetView(m_output.Get(), nullptr, &m_rtv))) {
        RC_LOGE("Foveated encoding: texture creation failed: %s", HResultString(hr).c_str());
        return false;
    }
    auto vs = CompileShader(kShader, "vs", "vs_5_0");
    auto ps = CompileShader(kShader, "ps", "ps_5_0");
    if (!vs || !ps) return false;
    device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vs);
    device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_ps);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device->CreateSamplerState(&sd, &m_sampler);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Params);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(device->CreateBuffer(&bd, nullptr, &m_params)) && m_vs && m_ps && m_sampler;
}

bool Foveator::Squeeze(ID3D11DeviceContext* ctx, ID3D11Texture2D* frame, const float foveaStart[2][2]) {
    if (frame != m_lastInput) {
        m_inputSrv.Reset();
        D3D11_TEXTURE2D_DESC desc;
        frame->GetDesc(&desc);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        // Shared VR textures are typeless; read the raw (gamma-encoded) values, as the colour
        // converter does.
        sd.Format = desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS   ? DXGI_FORMAT_R8G8B8A8_UNORM
                    : desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM
                                                                   : desc.Format;
        if (FAILED(m_device->CreateShaderResourceView(frame, &sd, &m_inputSrv))) return false;
        m_lastInput = frame;
    }
    const float F = kFoveaSize;
    Params p{};
    std::memcpy(p.start, foveaStart, sizeof(float) * 4);
    p.layout[0] = F;
    p.layout[1] = (float(m_encodedWidth) / m_eyeWidth - F) / (1 - F);
    p.layout[2] = (float(m_encodedHeight) / m_eyeHeight - F) / (1 - F);
    p.texel[0] = 1.f / (m_eyeWidth * 2);
    p.texel[1] = 1.f / m_eyeHeight;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ctx->Map(m_params.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    std::memcpy(mapped.pData, &p, sizeof(p));
    ctx->Unmap(m_params.Get(), 0);

    ctx->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    ctx->PSSetShader(m_ps.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, m_inputSrv.GetAddressOf());
    ctx->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    ctx->PSSetConstantBuffers(0, 1, m_params.GetAddressOf());
    ctx->RSSetState(nullptr);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(nullptr, 0);
    const D3D11_VIEWPORT vp{0, 0, float(m_encodedWidth * 2), float(m_encodedHeight), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->Draw(3, 0);
    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);
    return true;
}

}  // namespace rcs
