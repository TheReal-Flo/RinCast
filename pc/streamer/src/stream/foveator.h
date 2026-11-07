// Foveated encoding: squeezes a VR frame (both eyes side by side) so that a square around where
// the eyes look keeps every pixel and the rest keeps half of them in each direction. The headset
// stretches it back (rc::VrFoveationMetadata describes the layout). About half the pixels to
// encode, send and decode, taken where the eye can't see detail anyway.
#pragma once

#include <d3d11_4.h>

#include "../util.h"

namespace rcs {

class Foveator {
   public:
    // The full share of each eye (along both axes) that keeps every pixel.
    static constexpr float kFoveaSize = 0.45f;
    // The squeezed eye size for a full one: the fovea, and the rest at half density.
    static uint32_t EncodedSize(uint32_t full);

    // eyeWidth x eyeHeight per eye in, encodedEyeWidth x encodedEyeHeight per eye out.
    bool Init(ID3D11Device* device, uint32_t eyeWidth, uint32_t eyeHeight, uint32_t encodedEyeWidth,
              uint32_t encodedEyeHeight);
    // foveaStart: per eye, where the full-density square starts (x, y from the top, 0..1).
    bool Squeeze(ID3D11DeviceContext* ctx, ID3D11Texture2D* frame, const float foveaStart[2][2]);
    ID3D11Texture2D* Output() const { return m_output.Get(); }

   private:
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11Texture2D> m_output;
    ComPtr<ID3D11RenderTargetView> m_rtv;
    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_ps;
    ComPtr<ID3D11SamplerState> m_sampler;
    ComPtr<ID3D11Buffer> m_params;
    ID3D11Texture2D* m_lastInput = nullptr;
    ComPtr<ID3D11ShaderResourceView> m_inputSrv;
    uint32_t m_eyeWidth = 0, m_eyeHeight = 0, m_encodedWidth = 0, m_encodedHeight = 0;
};

}  // namespace rcs
