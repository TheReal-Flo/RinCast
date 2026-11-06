// RGB -> NV12 / P010 conversion on the GPU (BT.709, limited range), so every
// encoder backend receives identical YUV input and the bitstream can signal
// its colour space precisely.
#pragma once

#include <d3d11_4.h>

#include "../util.h"

namespace rcs {

class ColorConverter {
   public:
    // Creates the YUV target (NV12 or P010) of the given size.
    bool Init(ID3D11Device* device, UINT width, UINT height, bool tenBit);
    // Converts `rgb` (any size; scaled to fit) into the YUV target.
    bool Convert(ID3D11DeviceContext* ctx, ID3D11Texture2D* rgb);
    ID3D11Texture2D* Output() const { return m_yuv.Get(); }
    DXGI_FORMAT Format() const { return m_tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12; }

   private:
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11Texture2D> m_yuv;
    ComPtr<ID3D11RenderTargetView> m_rtvY;
    ComPtr<ID3D11RenderTargetView> m_rtvUV;
    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_psY;
    ComPtr<ID3D11PixelShader> m_psUV;
    ComPtr<ID3D11SamplerState> m_sampler;
    ComPtr<ID3D11RasterizerState> m_raster;
    // SRV cache for the most recent input texture.
    ID3D11Texture2D* m_lastInput = nullptr;
    ComPtr<ID3D11ShaderResourceView> m_inputSrv;
    UINT m_width = 0, m_height = 0;
    bool m_tenBit = false;
};

}  // namespace rcs
