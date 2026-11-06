// encode-bench: measures the streamer's encoder on this GPU with a moving picture
// (the first monitor's current image, panned like a turning head) and checks loss
// recovery by reference invalidation.
//
//   encode-bench [--codec hevc|av1|h264] [--size 3664x1920] [--fps 90] [--mbps 150]
//                [--frames 300] [--desktop] [--no-split] [--foveated] [--ten-bit]
//                [--no-aq] [--two-pass] [--one-pass]
//                [--loss <frame>] [--out <bitstream>] [--yuv <nv12 file>] [--compare-foveated]
//
// --loss F drops frames F..F+3 from --out (as if lost on the network) and answers the
// loss the way the streamer does: InvalidateAfter(F-1), then the next frame is a
// recovery frame. Decode --out with ffmpeg to check that it decodes cleanly.
// --yuv writes the encoder input, for PSNR comparisons with ffmpeg.
// --compare-foveated also encodes every frame with foveation into <out>.fov, from the
// identical input, so centre and edge quality can be compared with and without it.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../../pc/streamer/src/capture/desktop_capture.h"
#include "../../pc/streamer/src/capture/monitors.h"
#include "../../pc/streamer/src/encode/color_convert.h"
#include "../../pc/streamer/src/encode/encoder.h"
#include "../../pc/streamer/src/gpu.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
#include "rc/platform.h"

using namespace rcs;

int main(int argc, char** argv) {
    EncoderConfig ec;
    ec.codec = rc::VideoCodec::HEVC;
    ec.width = 3664;
    ec.height = 1920;
    ec.fps = 90;
    ec.bitrateKbps = 150000;
    int frames = 300;
    int lossAt = -1;
    int lossCount = 4;
    std::string outPath, yuvPath;
    bool moving = false;           // every pixel changes, like a turning head in a game
    std::string imagePath;         // pan over this picture instead of the desktop
    bool compareFoveated = false;  // also encode every frame with foveation, into <out>.fov
    bool onePass = false;          // --desktop without the streamer's second pass
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--codec") {
            const std::string c = next();
            ec.codec = c == "av1" ? rc::VideoCodec::AV1 : c == "h264" ? rc::VideoCodec::H264 : rc::VideoCodec::HEVC;
        } else if (a == "--size") {
            std::sscanf(next().c_str(), "%ux%u", &ec.width, &ec.height);
        } else if (a == "--fps") {
            ec.fps = std::stof(next());
        } else if (a == "--mbps") {
            ec.bitrateKbps = uint32_t(std::stoi(next()) * 1000);
        } else if (a == "--frames") {
            frames = std::stoi(next());
        } else if (a == "--desktop") {
            ec.desktop = true;
        } else if (a == "--no-split") {
            ec.splitFrame = false;
        } else if (a == "--no-aq") {
            ec.adaptiveQuant = false;
        } else if (a == "--two-pass") {
            ec.twoPass = true;
        } else if (a == "--one-pass") {
            onePass = true;
        } else if (a == "--foveated") {
            ec.foveated = true;
        } else if (a == "--moving") {
            moving = true;
        } else if (a == "--image") {
            imagePath = next();
        } else if (a == "--compare-foveated") {
            compareFoveated = true;
        } else if (a == "--ten-bit") {
            ec.tenBit = true;
        } else if (a == "--loss") {
            lossAt = std::stoi(next());
        } else if (a == "--lost") {
            lossCount = std::stoi(next());
        } else if (a == "--out") {
            outPath = next();
        } else if (a == "--yuv") {
            yuvPath = next();
        } else {
            std::printf("unknown option %s\n", a.c_str());
            return 2;
        }
    }
    // As the streamer does: desktop streams always take two passes.
    if (ec.desktop && !onePass) ec.twoPass = true;
    if (onePass) ec.twoPass = false;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto monitors = EnumerateMonitors();
    if (monitors.empty()) return 1;
    const auto& mon = monitors[0];
    auto adapter = AdapterByLuid(mon.adapterLuid);
    DXGI_ADAPTER_DESC ad{};
    adapter->GetDesc(&ad);
    ComPtr<ID3D11DeviceContext> ctx;
    auto dev = CreateDevice(adapter.Get(), &ctx);

    // One still of the desktop; frames are crops of it that pan around.
    DesktopCapture cap;
    if (!cap.Init(dev.Get(), mon, mon.width, mon.height)) {
        std::printf("capture init failed\n");
        return 1;
    }
    for (int i = 0; i < 40; i++)
        if (cap.Capture(50) == DesktopCapture::Result::NewFrame) break;
    cap.Recompose();
    D3D11_TEXTURE2D_DESC sd;
    cap.Output()->GetDesc(&sd);
    ID3D11Texture2D* panSource = cap.Output();
    ComPtr<ID3D11Texture2D> image;
    if (!imagePath.empty()) {
        int iw = 0, ih = 0, comp = 0;
        stbi_uc* px = stbi_load(imagePath.c_str(), &iw, &ih, &comp, 4);
        if (!px) {
            std::printf("can't read %s\n", imagePath.c_str());
            return 1;
        }
        if (sd.Format == DXGI_FORMAT_B8G8R8A8_UNORM || sd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS)
            for (int p = 0; p < iw * ih; p++) std::swap(px[p * 4], px[p * 4 + 2]);
        sd.Width = UINT(iw);
        sd.Height = UINT(ih);
        D3D11_TEXTURE2D_DESC id = sd;
        id.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        id.MiscFlags = 0;
        id.MipLevels = 1;
        D3D11_SUBRESOURCE_DATA init{px, UINT(iw) * 4, 0};
        if (FAILED(dev->CreateTexture2D(&id, &init, &image))) {
            std::printf("can't create the image texture\n");
            return 1;
        }
        stbi_image_free(px);
        panSource = image.Get();
    }
    D3D11_TEXTURE2D_DESC cd = sd;
    cd.Width = sd.Width * 3 / 4;
    cd.Height = sd.Height * 3 / 4;
    cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    cd.MiscFlags = 0;
    if (moving) cd.BindFlags |= D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> crop;
    dev->CreateTexture2D(&cd, nullptr, &crop);
    // --moving: draws the picture into the crop, rotating and zooming it a little.
    ComPtr<ID3D11RenderTargetView> cropRtv;
    ComPtr<ID3D11ShaderResourceView> panSrv;
    ComPtr<ID3D11VertexShader> moveVs;
    ComPtr<ID3D11PixelShader> movePs;
    ComPtr<ID3D11Buffer> moveCb;
    ComPtr<ID3D11SamplerState> moveSampler;
    if (moving) {
        static const char* kMove = R"(
Texture2D src : register(t0);
SamplerState samp : register(s0);
cbuffer C : register(b0) { float2 shift; float angle; float zoom; };
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    float2 c = float2((id << 1) & 2, id & 2);
    V o; o.pos = float4(c * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = c; return o;
}
float4 ps(V i) : SV_Target {
    float2 p = (i.uv - 0.5) * zoom;
    float s = sin(angle), c = cos(angle);
    return src.Sample(samp, 0.5 + shift + float2(c * p.x - s * p.y, s * p.x + c * p.y));
}
)";
        auto vsb = CompileShader(kMove, "vs", "vs_5_0");
        auto psb = CompileShader(kMove, "ps", "ps_5_0");
        if (!vsb || !psb) return 1;
        dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &moveVs);
        dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &movePs);
        dev->CreateRenderTargetView(crop.Get(), nullptr, &cropRtv);
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        srv.Format = sd.Format;
        dev->CreateShaderResourceView(panSource, &srv, &panSrv);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dev->CreateBuffer(&bd, nullptr, &moveCb);
        D3D11_SAMPLER_DESC samp{};
        samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR;
        dev->CreateSamplerState(&samp, &moveSampler);
    }

    ColorConverter conv;
    if (!conv.Init(dev.Get(), ec.width, ec.height, ec.tenBit)) return 1;
    ec.inputFormat = conv.Format();
    auto encoder = CreateEncoder(dev.Get(), ad.VendorId, ec);
    if (!encoder) return 1;
    // --compare-foveated: a second, foveated encoder gets exactly the same frames, so the
    // two outputs can be compared region by region (the panned desktop isn't repeatable).
    std::unique_ptr<VideoEncoder> fovEncoder;
    std::ofstream fovOut;
    if (compareFoveated) {
        EncoderConfig fc = ec;
        fc.foveated = true;
        fovEncoder = CreateEncoder(dev.Get(), ad.VendorId, fc);
        if (!fovEncoder) return 1;
        if (!outPath.empty()) fovOut.open(outPath + ".fov", std::ios::binary);
    }
    // Quest 3 field of view (left, right, up, down tangents per eye).
    const float fov[2][4] = {{1.376f, 0.839f, 0.966f, 1.428f}, {0.839f, 1.376f, 0.966f, 1.428f}};
    encoder->SetFoveation(fov);
    if (fovEncoder) fovEncoder->SetFoveation(fov);

    std::ofstream out, yuv;
    if (!outPath.empty()) out.open(outPath, std::ios::binary);
    if (!yuvPath.empty()) yuv.open(yuvPath, std::ios::binary);
    ComPtr<ID3D11Texture2D> staging;
    if (yuv) {
        D3D11_TEXTURE2D_DESC yd;
        conv.Output()->GetDesc(&yd);
        yd.Usage = D3D11_USAGE_STAGING;
        yd.BindFlags = 0;
        yd.MiscFlags = 0;
        yd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        dev->CreateTexture2D(&yd, nullptr, &staging);
    }

    std::vector<double> times;
    std::vector<size_t> sizes;
    size_t keyBytes = 0, recoveryBytes = 0;
    bool recoveryWasKey = false;
    uint64_t lastGoodTs = 0;
    EncodedFrame ef;
    const UINT panW = sd.Width - cd.Width, panH = sd.Height - cd.Height;
    for (int i = 0; i < frames; i++) {
        // Smooth pan: a few pixels per frame, like a slowly turning head.
        const double t = i / double(ec.fps);
        const UINT x = UINT((0.5 + 0.5 * std::sin(t * 1.3)) * panW);
        const UINT y = UINT((0.5 + 0.5 * std::sin(t * 0.7)) * panH);
        D3D11_BOX box{x, y, 0, x + cd.Width, y + cd.Height, 1};
        if (moving) {
            const float c[4] = {float(std::sin(t * 1.7) * 0.15), float(std::sin(t * 1.1) * 0.1), float(std::sin(t * 0.9) * 0.05),
                                1.0f + float(std::sin(t * 1.3)) * 0.04f};
            ctx->UpdateSubresource(moveCb.Get(), 0, nullptr, c, 0, 0);
            ctx->OMSetRenderTargets(1, cropRtv.GetAddressOf(), nullptr);
            D3D11_VIEWPORT mv{0, 0, float(cd.Width), float(cd.Height), 0, 1};
            ctx->RSSetViewports(1, &mv);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->IASetInputLayout(nullptr);
            ctx->VSSetShader(moveVs.Get(), nullptr, 0);
            ctx->PSSetShader(movePs.Get(), nullptr, 0);
            ctx->PSSetShaderResources(0, 1, panSrv.GetAddressOf());
            ctx->PSSetSamplers(0, 1, moveSampler.GetAddressOf());
            ctx->PSSetConstantBuffers(0, 1, moveCb.GetAddressOf());
            ctx->Draw(3, 0);
            ID3D11ShaderResourceView* noSrv = nullptr;
            ctx->PSSetShaderResources(0, 1, &noSrv);
            ID3D11RenderTargetView* noRtv = nullptr;
            ctx->OMSetRenderTargets(1, &noRtv, nullptr);
        } else {
            ctx->CopySubresourceRegion(crop.Get(), 0, 0, 0, 0, panSource, 0, &box);
        }
        conv.Convert(ctx.Get(), crop.Get());
        if (yuv) {
            ctx->CopyResource(staging.Get(), conv.Output());
            D3D11_MAPPED_SUBRESOURCE m{};
            ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
            const UINT bpp = ec.tenBit ? 2 : 1;
            for (UINT r = 0; r < ec.height * 3 / 2; r++)
                yuv.write(static_cast<const char*>(m.pData) + size_t(r) * m.RowPitch, ec.width * bpp);
            ctx->Unmap(staging.Get(), 0);
        }

        const bool lost = lossAt >= 0 && i >= lossAt && i < lossAt + lossCount;
        const bool recover = lossAt >= 0 && i == lossAt + lossCount;
        bool key = i == 0;
        if (recover && !encoder->InvalidateAfter(lastGoodTs)) {
            std::printf("InvalidateAfter refused: forcing a keyframe\n");
            key = true;
        }
        const int64_t t0 = rc::NowNs();
        if (!encoder->Encode(conv.Output(), key, ef)) {
            std::printf("encode failed at frame %d\n", i);
            return 1;
        }
        times.push_back(rc::NsToMs(rc::NowNs() - t0));
        sizes.push_back(ef.data.size());
        if (ef.keyframe && i == 0) keyBytes = ef.data.size();
        if (recover) {
            recoveryBytes = ef.data.size();
            recoveryWasKey = ef.keyframe;
        }
        if (lossAt < 0 || i < lossAt) lastGoodTs = ef.timestamp;
        if (out && !lost) out.write(reinterpret_cast<const char*>(ef.data.data()), std::streamsize(ef.data.size()));
        if (fovEncoder) {
            EncodedFrame ff;
            if (!fovEncoder->Encode(conv.Output(), key, ff)) return 1;
            if (fovOut) fovOut.write(reinterpret_cast<const char*>(ff.data.data()), std::streamsize(ff.data.size()));
        }
    }

    // Skip the first frames (session warm-up, keyframe) for the averages.
    const size_t skip = std::min<size_t>(10, times.size() / 2);
    std::vector<double> t(times.begin() + skip, times.end());
    std::sort(t.begin(), t.end());
    double sum = 0;
    for (double v : t) sum += v;
    size_t bytes = 0, maxBytes = 0;
    for (size_t i = skip; i < sizes.size(); i++) {
        bytes += sizes[i];
        maxBytes = std::max(maxBytes, sizes[i]);
    }
    const double avgBytes = double(bytes) / double(sizes.size() - skip);
    std::printf("%s %ux%u @%.0f, %u Mbps%s%s%s\n", encoder->Name(), ec.width, ec.height, ec.fps,
                ec.bitrateKbps / 1000, ec.desktop ? ", desktop" : "", ec.foveated ? ", foveated" : "",
                ec.splitFrame ? "" : ", no split");
    std::printf("encode ms: avg %.2f  p50 %.2f  p95 %.2f  max %.2f\n", sum / double(t.size()), t[t.size() / 2],
                t[t.size() * 95 / 100], t.back());
    std::printf("frame KB: avg %.1f  max %.1f  keyframe %.1f\n", avgBytes / 1024, double(maxBytes) / 1024,
                double(keyBytes) / 1024);
    if (lossAt >= 0)
        std::printf("recovery frame: %.1f KB (%s, %.1fx average)\n", double(recoveryBytes) / 1024,
                    recoveryWasKey ? "intra" : "inter", double(recoveryBytes) / avgBytes);
    return 0;
}
