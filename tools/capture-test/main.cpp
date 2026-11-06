// Diagnoses the desktop pipeline stage by stage: duplication -> composed BGRA
// -> NV12. Prints the average of each stage and writes BMPs for inspection.
#include <cstdio>
#include <fstream>
#include <vector>

#include "../../pc/streamer/src/capture/desktop_capture.h"
#include "../../pc/streamer/src/capture/monitors.h"
#include "../../pc/streamer/src/encode/color_convert.h"
#include "../../pc/streamer/src/gpu.h"
#include "rc/platform.h"

using namespace rcs;

static std::vector<uint8_t> ReadBack(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, UINT& pitch,
                                     UINT& w, UINT& h) {
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.MiscFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    dev->CreateTexture2D(&d, nullptr, &staging);
    ctx->CopyResource(staging.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
    w = d.Width;
    h = d.Height;
    pitch = m.RowPitch;
    const UINT rows = d.Format == DXGI_FORMAT_NV12 ? h * 3 / 2 : h;
    std::vector<uint8_t> out(static_cast<const uint8_t*>(m.pData), static_cast<const uint8_t*>(m.pData) + size_t(pitch) * rows);
    ctx->Unmap(staging.Get(), 0);
    return out;
}

static void WriteBmp(const char* path, const std::vector<uint8_t>& bgra, UINT pitch, UINT w, UINT h) {
    std::ofstream f(path, std::ios::binary);
    const uint32_t size = 54 + w * h * 4;
    uint8_t hdr[54] = {'B', 'M'};
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    int32_t nh = -int32_t(h);
    memcpy(hdr + 22, &nh, 4);
    hdr[26] = 1;
    hdr[28] = 32;
    f.write(reinterpret_cast<char*>(hdr), 54);
    for (UINT y = 0; y < h; y++) f.write(reinterpret_cast<const char*>(&bgra[size_t(y) * pitch]), w * 4);
}

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto monitors = EnumerateMonitors();
    if (monitors.empty()) return 1;
    const auto& mon = monitors[0];
    std::printf("Monitor %s %s %dx%d\n", mon.id.c_str(), mon.name.c_str(), mon.width, mon.height);
    auto adapter = AdapterByLuid(mon.adapterLuid);
    ComPtr<ID3D11DeviceContext> ctx;
    auto dev = CreateDevice(adapter.Get(), &ctx);
    DesktopCapture cap;
    if (!cap.Init(dev.Get(), mon, mon.width, mon.height)) {
        std::printf("capture init failed\n");
        return 2;
    }
    int frames = 0;
    for (int i = 0; i < 200 && frames < 3; i++) {
        if (cap.Capture(50) == DesktopCapture::Result::NewFrame) frames++;
    }
    std::printf("captured %d new frames, hasDesktop=%d\n", frames, cap.HasDesktop());
    UINT pitch, w, h;
    auto bgra = ReadBack(dev.Get(), ctx.Get(), cap.Output(), pitch, w, h);
    uint64_t sum = 0;
    for (UINT y = 0; y < h; y += 4)
        for (UINT x = 0; x < w; x += 4) sum += bgra[size_t(y) * pitch + x * 4 + 1];
    std::printf("composed avg green: %.1f\n", double(sum) / ((h / 4) * (w / 4)));
    WriteBmp("composed.bmp", bgra, pitch, w, h);

    ColorConverter conv;
    conv.Init(dev.Get(), w, h, false);
    conv.Convert(ctx.Get(), cap.Output());
    auto nv12 = ReadBack(dev.Get(), ctx.Get(), conv.Output(), pitch, w, h);
    sum = 0;
    for (UINT y = 0; y < h; y += 4)
        for (UINT x = 0; x < w; x += 4) sum += nv12[size_t(y) * pitch + x];
    std::printf("NV12 avg luma: %.1f\n", double(sum) / ((h / 4) * (w / 4)));
    return 0;
}
