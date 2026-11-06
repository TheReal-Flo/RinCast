// D3D11 helpers: device creation per adapter and a small textured-quad
// renderer used for scaling, cursor composition and stereo packing.
#pragma once

#include <d3d11_4.h>
#include <dxgi1_6.h>

#include <string>

#include "util.h"

namespace rcs {

struct GpuInfo {
    LUID luid{};
    UINT vendorId = 0;   // 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel
    std::string name;
    uint64_t dedicatedVram = 0;
};

GpuInfo DescribeAdapter(IDXGIAdapter* adapter);
// High-performance adapter (what games use by default).
ComPtr<IDXGIAdapter1> PreferredAdapter();
ComPtr<IDXGIAdapter1> AdapterByLuid(LUID luid);
ComPtr<ID3D11Device> CreateDevice(IDXGIAdapter* adapter, ComPtr<ID3D11DeviceContext>* context = nullptr);

ComPtr<ID3DBlob> CompileShader(const char* source, const char* entry, const char* target);

// Draws textures into render targets with optional sub-rect placement,
// UV rotation and blend modes.
class QuadRenderer {
   public:
    enum class Blend { Opaque, Alpha, Invert };

    bool Init(ID3D11Device* device);

    // Draws `srv` into `rtv` (size rtWidth x rtHeight) at the destination rect
    // given in pixels. uvRotation: 0, 90, 180, 270 degrees (clockwise).
    void Draw(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srv, ID3D11RenderTargetView* rtv, UINT rtWidth,
              UINT rtHeight, float dstX, float dstY, float dstW, float dstH, Blend blend = Blend::Opaque,
              int uvRotation = 0, float uvX = 0, float uvY = 0, float uvW = 1, float uvH = 1);

   private:
    struct Constants {
        float dstRect[4];  // NDC x, y, w, h
        float uvRect[4];
        float rotation[4];  // cos, sin, 0, 0
    };
    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_ps;
    ComPtr<ID3D11Buffer> m_cb;
    ComPtr<ID3D11SamplerState> m_linear;
    ComPtr<ID3D11BlendState> m_blendAlpha;
    ComPtr<ID3D11BlendState> m_blendInvert;
    ComPtr<ID3D11RasterizerState> m_raster;
};

}  // namespace rcs
