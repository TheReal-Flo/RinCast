#include "desktop_capture.h"

#include <algorithm>

#include "rc/platform.h"

namespace rcs {

bool DesktopCapture::Init(ID3D11Device* device, const MonitorInfo& monitor, UINT outWidth, UINT outHeight) {
    Shutdown();
    m_device = device;
    m_device->GetImmediateContext(&m_ctx);
    m_monitor = monitor;
    m_outWidth = outWidth & ~1u;
    m_outHeight = outHeight & ~1u;
    if (!m_quad.Init(device)) return false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = m_outWidth;
    td.Height = m_outHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_output))) return false;
    m_device->CreateRenderTargetView(m_output.Get(), nullptr, &m_outputRtv);
    const float black[4] = {0, 0, 0, 1};
    m_ctx->ClearRenderTargetView(m_outputRtv.Get(), black);
    return CreateDuplication();
}

void DesktopCapture::Shutdown() {
    if (m_dup) m_dup->ReleaseFrame();
    m_dup.Reset();
    m_desktop.Reset();
    m_desktopSrv.Reset();
    m_output.Reset();
    m_outputRtv.Reset();
    m_haveDesktop = false;
}

bool DesktopCapture::CreateDuplication() {
    m_dup.Reset();
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    m_device.As(&dxgiDevice);
    dxgiDevice->GetAdapter(&adapter);
    ComPtr<IDXGIOutput> output;
    for (UINT i = 0; adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_OUTPUT_DESC od;
        output->GetDesc(&od);
        if (Narrow(od.DeviceName) == m_monitor.id) break;
        output.Reset();
    }
    if (!output) {
        RC_LOGE("Monitor %s not found on capture adapter", m_monitor.id.c_str());
        return false;
    }
    ComPtr<IDXGIOutput5> output5;
    HRESULT hr = E_FAIL;
    if (SUCCEEDED(output.As(&output5))) {
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
        hr = output5->DuplicateOutput1(m_device.Get(), 0, 1, formats, &m_dup);
    }
    if (FAILED(hr)) {
        ComPtr<IDXGIOutput1> output1;
        if (SUCCEEDED(output.As(&output1))) hr = output1->DuplicateOutput(m_device.Get(), &m_dup);
    }
    if (FAILED(hr)) {
        // E_ACCESSDENIED happens on the secure desktop (UAC, lock screen); caller retries.
        if (m_dupFailures++ % 50 == 0)
            RC_LOGW("DuplicateOutput failed for %s: %s (retrying)", m_monitor.id.c_str(), HResultString(hr).c_str());
        return false;
    }
    m_dup->GetDesc(&m_dupDesc);
    m_dupFailures = 0;
    // ModeDesc is the desktop's size as it appears, rotated, but the frames come unrotated: a
    // portrait monitor (rotated 90 or 270) sends them landscape. Comparing against ModeDesc
    // turned every frame of one away, and the monitor never showed.
    const bool sideways =
        m_dupDesc.Rotation == DXGI_MODE_ROTATION_ROTATE90 || m_dupDesc.Rotation == DXGI_MODE_ROTATION_ROTATE270;
    m_frameWidth = sideways ? m_dupDesc.ModeDesc.Height : m_dupDesc.ModeDesc.Width;
    m_frameHeight = sideways ? m_dupDesc.ModeDesc.Width : m_dupDesc.ModeDesc.Height;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = m_frameWidth;
    td.Height = m_frameHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    m_desktop.Reset();
    m_desktopSrv.Reset();
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_desktop))) return false;
    m_device->CreateShaderResourceView(m_desktop.Get(), nullptr, &m_desktopSrv);
    m_haveDesktop = false;
    return true;
}

DesktopCapture::Result DesktopCapture::Capture(UINT timeoutMs) {
    if (!m_dup) {
        if (!CreateDuplication()) {
            Sleep(std::max<UINT>(timeoutMs, 100));
            return Result::Lost;
        }
    }
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = m_dup->AcquireNextFrame(timeoutMs, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return Result::NoChange;
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
        m_dup.Reset();
        return Result::Lost;
    }
    if (FAILED(hr)) {
        m_dup.Reset();
        return Result::Error;
    }

    bool changed = false;
    if (info.LastPresentTime.QuadPart != 0 && resource) {
        ComPtr<ID3D11Texture2D> frame;
        if (SUCCEEDED(resource.As(&frame))) {
            D3D11_TEXTURE2D_DESC fd;
            frame->GetDesc(&fd);
            if (fd.Format == DXGI_FORMAT_B8G8R8A8_UNORM && fd.Width == m_frameWidth && fd.Height == m_frameHeight) {
                m_ctx->CopyResource(m_desktop.Get(), frame.Get());
                if (!m_haveDesktop) RC_LOGI("Capturing %s (%ux%u)", m_monitor.id.c_str(), fd.Width, fd.Height);
                m_haveDesktop = true;
                changed = true;
            } else if (!m_haveDesktop) {
                RC_LOGW("Unexpected desktop frame %ux%u format %d (expected %ux%u)", fd.Width, fd.Height, int(fd.Format),
                        m_frameWidth, m_frameHeight);
            }
        }
    }
    if (info.LastMouseUpdateTime.QuadPart != 0) {
        const bool visible = info.PointerPosition.Visible != FALSE;
        if (visible != m_cursorVisible || info.PointerPosition.Position.x != m_cursorPos.x ||
            info.PointerPosition.Position.y != m_cursorPos.y) {
            changed = true;
        }
        m_cursorVisible = visible;
        if (visible) m_cursorPos = info.PointerPosition.Position;
        if (info.PointerShapeBufferSize) {
            UpdateCursorShape(info);
            changed = true;
        }
    }
    m_dup->ReleaseFrame();
    if (!changed || !m_haveDesktop) return Result::NoChange;
    Compose();
    return Result::NewFrame;
}

void DesktopCapture::UpdateCursorShape(const DXGI_OUTDUPL_FRAME_INFO& info) {
    m_shapeBuffer.resize(info.PointerShapeBufferSize);
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape{};
    UINT required = 0;
    if (FAILED(m_dup->GetFramePointerShape(UINT(m_shapeBuffer.size()), m_shapeBuffer.data(), &required, &shape))) return;

    int w = int(shape.Width);
    int h = int(shape.Height);
    if (shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) h /= 2;
    if (w <= 0 || h <= 0) return;
    std::vector<uint32_t> color(size_t(w) * h, 0);
    std::vector<uint32_t> invert(size_t(w) * h, 0);
    bool hasInvert = false;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t& c = color[size_t(y) * w + x];
            uint32_t& inv = invert[size_t(y) * w + x];
            switch (shape.Type) {
                case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR: {
                    c = *reinterpret_cast<const uint32_t*>(&m_shapeBuffer[size_t(y) * shape.Pitch + x * 4]);
                    break;
                }
                case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR: {
                    const uint32_t px = *reinterpret_cast<const uint32_t*>(&m_shapeBuffer[size_t(y) * shape.Pitch + x * 4]);
                    if ((px >> 24) == 0) {
                        c = px | 0xFF000000u;  // opaque replace
                    } else if (px & 0x00FFFFFFu) {
                        inv = 0xFFFFFFFFu;  // XOR with non-black: approximate as invert
                        hasInvert = true;
                    }
                    break;
                }
                case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME: {
                    const uint8_t mask = uint8_t(0x80 >> (x % 8));
                    const bool andBit = (m_shapeBuffer[size_t(y) * shape.Pitch + x / 8] & mask) != 0;
                    const bool xorBit = (m_shapeBuffer[size_t(y + h) * shape.Pitch + x / 8] & mask) != 0;
                    if (!andBit) {
                        c = xorBit ? 0xFFFFFFFFu : 0xFF000000u;
                    } else if (xorBit) {
                        inv = 0xFFFFFFFFu;
                        hasInvert = true;
                    }
                    break;
                }
            }
        }
    }

    auto makeSrv = [&](const std::vector<uint32_t>& pixels, ComPtr<ID3D11ShaderResourceView>& out) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(w);
        td.Height = UINT(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{pixels.data(), UINT(w * 4), 0};
        ComPtr<ID3D11Texture2D> tex;
        out.Reset();
        if (SUCCEEDED(m_device->CreateTexture2D(&td, &init, &tex)))
            m_device->CreateShaderResourceView(tex.Get(), nullptr, &out);
    };
    makeSrv(color, m_cursorColor);
    m_cursorHasInvert = hasInvert;
    if (hasInvert) makeSrv(invert, m_cursorInvert);
    m_cursorW = w;
    m_cursorH = h;
    m_cursorHotspot = {LONG(shape.HotSpot.x), LONG(shape.HotSpot.y)};
}

void DesktopCapture::Compose() {
    if (!m_haveDesktop) return;
    // Turning the frame back: a monitor rotated 90 degrees has its top-left in the frame's
    // bottom-left corner, which the quad's UVs reach turned the other way (270), and so on.
    m_quad.Draw(m_ctx.Get(), m_desktopSrv.Get(), m_outputRtv.Get(), m_outWidth, m_outHeight, 0, 0, float(m_outWidth),
                float(m_outHeight), QuadRenderer::Blend::Opaque, (360 - m_monitor.rotation) % 360);
    if (m_cursorVisible && m_cursorColor) {
        // Cursor coordinates are in logical (rotated) monitor space.
        const float sx = float(m_outWidth) / float(m_monitor.width);
        const float sy = float(m_outHeight) / float(m_monitor.height);
        const float x = float(m_cursorPos.x) * sx;
        const float y = float(m_cursorPos.y) * sy;
        const float w = float(m_cursorW) * sx;
        const float h = float(m_cursorH) * sy;
        m_quad.Draw(m_ctx.Get(), m_cursorColor.Get(), m_outputRtv.Get(), m_outWidth, m_outHeight, x, y, w, h,
                    QuadRenderer::Blend::Alpha);
        if (m_cursorHasInvert && m_cursorInvert)
            m_quad.Draw(m_ctx.Get(), m_cursorInvert.Get(), m_outputRtv.Get(), m_outWidth, m_outHeight, x, y, w, h,
                        QuadRenderer::Blend::Invert);
    }
}

void DesktopCapture::Recompose() { Compose(); }

POINT DesktopCapture::CursorPosition() const {
    if (!m_cursorVisible) return {-1, -1};
    const float sx = float(m_outWidth) / float(m_monitor.width);
    const float sy = float(m_outHeight) / float(m_monitor.height);
    return {LONG((m_cursorPos.x + m_cursorHotspot.x) * sx), LONG((m_cursorPos.y + m_cursorHotspot.y) * sy)};
}

}  // namespace rcs
