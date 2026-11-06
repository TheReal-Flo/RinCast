// bridge-grab: read-only look at a running VR session. Prints the bridge's
// display config and the newest frame's metadata, and saves that frame (both
// eyes side by side, exactly as the encoder sees it) as a PNG.
//
//   bridge-grab [out.png]
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdio>
#include <vector>

#include "vr_bridge.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

int main(int argc, char** argv) {
    const char* outPath = argc > 1 ? argv[1] : "bridge-frame.png";
    HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, rcvr::kSharedMemoryName);
    if (!map) {
        std::printf("No VR bridge (is RinCast Streamer running?)\n");
        return 1;
    }
    auto* s = static_cast<const rcvr::Shared*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(rcvr::Shared)));
    if (!s) return 1;
    const rcvr::DisplayConfig& c = s->config;
    std::printf("config: eye %ux%u @ %.1f Hz, ipd %.4f, model %s\n", c.eyeWidth, c.eyeHeight, c.refreshRate, c.ipd,
                c.headsetModel);
    for (int e = 0; e < 2; e++)
        std::printf("  fovTan[%d] L %.3f R %.3f U %.3f D %.3f\n", e, c.fovTan[e][0], c.fovTan[e][1], c.fovTan[e][2],
                    c.fovTan[e][3]);
    std::printf("producer kind %ld, app '%s'\n", s->producerKind, s->applicationName);

    const LONG64 latest = s->latestReadySlot;
    if (latest < 0) {
        std::printf("No frame published yet\n");
        return 2;
    }
    const int slot = int(latest & 0xFF);
    const rcvr::FrameSlot& fs = s->slots[slot];
    std::printf("slot %d frame %llu: pose q(%.3f %.3f %.3f %.3f) p(%.3f %.3f %.3f)\n", slot,
                (unsigned long long)fs.frameIndex, fs.renderPose.orientation[0], fs.renderPose.orientation[1],
                fs.renderPose.orientation[2], fs.renderPose.orientation[3], fs.renderPose.position[0],
                fs.renderPose.position[1], fs.renderPose.position[2]);
    for (int e = 0; e < 2; e++)
        std::printf("  frame fovTan[%d] L %.3f R %.3f U %.3f D %.3f\n", e, fs.fovTan[e][0], fs.fovTan[e][1],
                    fs.fovTan[e][2], fs.fovTan[e][3]);

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    // Same adapter as the bridge textures.
    ComPtr<IDXGIFactory1> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        adapter->GetDesc1(&d);
        if (d.AdapterLuid.LowPart == c.adapterLuid.LowPart && d.AdapterLuid.HighPart == c.adapterLuid.HighPart) break;
        adapter.Reset();
    }
    if (FAILED(D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                 nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
        return 3;
    ComPtr<ID3D11Device1> dev1;
    dev.As(&dev1);
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(dev1->OpenSharedResourceByName(s->textureNames[slot], DXGI_SHARED_RESOURCE_READ, IID_PPV_ARGS(&tex)))) {
        std::printf("Could not open slot texture\n");
        return 4;
    }
    D3D11_TEXTURE2D_DESC td;
    tex->GetDesc(&td);
    std::printf("slot texture %ux%u format %d\n", td.Width, td.Height, td.Format);
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &staging))) return 5;
    ComPtr<IDXGIKeyedMutex> mutex;
    tex.As(&mutex);
    if (mutex && !rcvr::AcquireKeyed(mutex.Get(), 100)) mutex.Reset();
    ctx->CopyResource(staging.Get(), tex.Get());
    ctx->Flush();
    if (mutex) mutex->ReleaseSync(0);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return 6;
    std::vector<uint8_t> rgb(size_t(td.Width) * td.Height * 3);
    for (UINT y = 0; y < td.Height; y++) {
        const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
        for (UINT x = 0; x < td.Width; x++)
            for (int k = 0; k < 3; k++) rgb[(size_t(y) * td.Width + x) * 3 + k] = row[x * 4 + k];
    }
    ctx->Unmap(staging.Get(), 0);
    stbi_write_png(outPath, int(td.Width), int(td.Height), 3, rgb.data(), int(td.Width) * 3);
    std::printf("saved %s\n", outPath);
    return 0;
}
