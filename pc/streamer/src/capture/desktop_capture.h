// DXGI Desktop Duplication of one monitor, composed with the mouse cursor
// and scaled into an encoder-ready BGRA texture.
#pragma once

#include <d3d11_4.h>
#include <dxgi1_6.h>

#include <vector>

#include "../gpu.h"
#include "monitors.h"

namespace rcs {

class DesktopCapture {
   public:
    enum class Result { NewFrame, NoChange, Lost, Error };

    // `device` must live on the monitor's adapter.
    bool Init(ID3D11Device* device, const MonitorInfo& monitor, UINT outWidth, UINT outHeight);
    void Shutdown();

    // Waits up to timeoutMs for a desktop or cursor update.
    Result Capture(UINT timeoutMs);
    // Re-renders the last frame (used to produce a keyframe without changes).
    void Recompose();

    ID3D11Texture2D* Output() const { return m_output.Get(); }
    UINT Width() const { return m_outWidth; }
    UINT Height() const { return m_outHeight; }
    // Cursor in output pixels, (-1, -1) when hidden.
    POINT CursorPosition() const;
    const MonitorInfo& Monitor() const { return m_monitor; }
    bool HasDesktop() const { return m_haveDesktop; }

   private:
    bool CreateDuplication();
    void UpdateCursorShape(const DXGI_OUTDUPL_FRAME_INFO& info);
    void Compose();

    MonitorInfo m_monitor;
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_ctx;
    ComPtr<IDXGIOutputDuplication> m_dup;
    DXGI_OUTDUPL_DESC m_dupDesc{};
    UINT m_frameWidth = 0, m_frameHeight = 0;  // the duplicated frames' size (unrotated)
    QuadRenderer m_quad;

    ComPtr<ID3D11Texture2D> m_desktop;  // last full desktop image (source orientation)
    ComPtr<ID3D11ShaderResourceView> m_desktopSrv;
    ComPtr<ID3D11Texture2D> m_output;
    ComPtr<ID3D11RenderTargetView> m_outputRtv;
    UINT m_outWidth = 0, m_outHeight = 0;
    bool m_haveDesktop = false;
    uint32_t m_dupFailures = 0;

    // Cursor
    ComPtr<ID3D11ShaderResourceView> m_cursorColor;   // BGRA with alpha
    ComPtr<ID3D11ShaderResourceView> m_cursorInvert;  // white where pixels invert
    bool m_cursorHasInvert = false;
    int m_cursorW = 0, m_cursorH = 0;
    POINT m_cursorHotspot{};
    POINT m_cursorPos{};   // monitor-local desktop pixels (top-left of shape)
    bool m_cursorVisible = false;
    std::vector<uint8_t> m_shapeBuffer;
};

}  // namespace rcs
