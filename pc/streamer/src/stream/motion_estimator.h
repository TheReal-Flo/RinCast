// Finds how a frame's contents moved since the frame before, on the GPU: one motion vector
// per 32 x 32 pixel block, by block matching. The headset uses them to generate a frame when
// the next real one is late.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace rcs {

class MotionEstimator {
   public:
    static constexpr uint32_t kBlockSize = 32;

    bool Init(ID3D11Device* device, uint32_t width, uint32_t height);
    // The next frame has nothing to be compared with.
    void Reset() { m_hasPrevious = false; }

    uint32_t GridWidth() const { return m_gridWidth; }
    uint32_t GridHeight() const { return m_gridHeight; }

    // Matches `source` (width x height, gamma-encoded colour) against the previous call's.
    // headTurn rotates a direction seen from this frame's head into the previous frame's
    // (row by row); fovTan is per eye: left, right, up, down. The result leaves out what that
    // turn explains. False if there's no previous frame to compare with yet.
    bool Process(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* source, const float headTurn[9], const float fovTan[2][4]);

    // The newest field: x, y in pixels per frame, row by row. Waits for the GPU.
    bool Read(ID3D11DeviceContext* ctx, std::vector<float>& out);

   private:
    struct Level {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
    };
    bool MakeLevel(DXGI_FORMAT format, uint32_t w, uint32_t h, Level& level);
    void Dispatch(ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs, ID3D11ShaderResourceView* const* srvs, UINT srvCount,
                  ID3D11UnorderedAccessView* uav, UINT x, UINT y);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_lumaCs, m_downCs, m_coarseCs, m_refineCs, m_medianCs;
    Level m_quarter[2], m_sixteenth[2];  // this frame's and the last, swapping
    Level m_coarse, m_refined, m_field;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constants;
    uint32_t m_width = 0, m_height = 0;
    uint32_t m_gridWidth = 0, m_gridHeight = 0, m_quarterW = 0, m_quarterH = 0, m_sixteenthW = 0, m_sixteenthH = 0;
    int m_current = 0;
    bool m_hasPrevious = false;
};

}  // namespace rcs
