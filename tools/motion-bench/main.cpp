// Developer tool: how well does generating a frame from motion vectors predict the real next
// frame, compared with showing the last one again? Also times the estimator on this GPU.
//
//   motion-bench <image.jpg>
#define NOMINMAX
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#include "../../pc/streamer/src/stream/motion_estimator.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr int W = 2496, H = 1312;

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    uint8_t* At(int x, int y) { return &rgba[size_t(y) * w * 4 + size_t(x) * 4]; }
    const uint8_t* At(int x, int y) const { return &rgba[size_t(y) * w * 4 + size_t(x) * 4]; }
};

Image Blank() {
    Image i;
    i.w = W;
    i.h = H;
    i.rgba.assign(size_t(W) * H * 4, 255);
    return i;
}

// `source` tiled, shifted by (ox, oy), drawn into the rectangle.
void Draw(Image& to, const Image& source, int rx, int ry, int rw, int rh, int ox, int oy) {
    for (int y = std::max(0, ry); y < std::min(H, ry + rh); y++) {
        for (int x = std::max(0, rx); x < std::min(W, rx + rw); x++) {
            int sx = (x + ox) % source.w, sy = (y + oy) % source.h;
            if (sx < 0) sx += source.w;
            if (sy < 0) sy += source.h;
            std::copy_n(source.At(sx, sy), 3, to.At(x, y));
        }
    }
}

struct Scene {
    const char* name;
    int pan;                     // the whole background, pixels per frame
    int spriteA, spriteB;        // moving objects, pixels per frame; 0 = absent
    bool noise;
    float yawDegrees = 0;        // head turn per frame
};

const float kFov[2][4] = {{1.376f, 0.839f, 0.966f, 1.191f}, {0.839f, 1.376f, 0.966f, 1.191f}};

Image Frame(const Scene& s, const Image& source, int t, std::mt19937& rng) {
    Image f = Blank();
    Draw(f, source, 0, 0, W, H, s.pan * t, 0);
    if (s.yawDegrees != 0) {
        // The image as a world around the head, seen through each eye after t turns.
        const float yaw = s.yawDegrees * t * 3.14159265f / 180.f, cy = std::cos(yaw), sy = std::sin(yaw);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const int eye = x >= W / 2;
                const float* c = kFov[eye];
                const float u = (x + 0.5f - eye * W / 2) / (W / 2), v = (y + 0.5f) / H;
                const float rx = -c[0] + u * (c[0] + c[1]), ry = c[2] - v * (c[2] + c[3]);
                const float wx = cy * rx - sy, wz = -sy * rx - cy;  // turned about y, looking along -z
                const float az = std::atan2(wx, -wz), el = std::atan2(ry, std::sqrt(wx * wx + wz * wz));
                int sx = int((az / 6.2831853f + 0.5f) * source.w * 3) % source.w;
                int sa = std::clamp(int((0.5f - el / 3.14159265f) * source.h), 0, source.h - 1);
                if (sx < 0) sx += source.w;
                std::copy_n(source.At(sx, sa), 3, f.At(x, y));
            }
        }
    }
    if (s.spriteA) Draw(f, source, 300 + s.spriteA * t, 250, 420, 340, 1200, 900 - (300 + s.spriteA * t));
    if (s.spriteB) Draw(f, source, 1800 - s.spriteB * t, 700, 300, 300, 400 + (1800 - s.spriteB * t), 100);
    if (s.noise) {
        std::uniform_int_distribution<int> d(-2, 2);
        for (auto& v : f.rgba) v = uint8_t(std::clamp(int(v) + d(rng), 0, 255));
        for (size_t i = 3; i < f.rgba.size(); i += 4) f.rgba[i] = 255;
    }
    return f;
}

double Psnr(const Image& a, const Image& b) {
    double sum = 0;
    int64_t n = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            for (int c = 0; c < 3; c++) {
                const double d = double(a.At(x, y)[c]) - b.At(x, y)[c];
                sum += d * d;
                n++;
            }
        }
    }
    return sum == 0 ? 99 : 10 * std::log10(255.0 * 255.0 / (sum / n));
}

// What the headset would do: pull each pixel from where the motion field says it is coming from.
Image Warp(const Image& cur, const std::vector<float>& field, int gw, int gh) {
    Image out = Blank();
    const int block = rcs::MotionEstimator::kBlockSize;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const float fx = (x + 0.5f) / block - 0.5f, fy = (y + 0.5f) / block - 0.5f;
            const int ix = std::clamp(int(std::floor(fx)), 0, gw - 1), iy = std::clamp(int(std::floor(fy)), 0, gh - 1);
            const int jx = std::min(ix + 1, gw - 1), jy = std::min(iy + 1, gh - 1);
            const float ax = std::clamp(fx - ix, 0.f, 1.f), ay = std::clamp(fy - iy, 0.f, 1.f);
            float mv[2];
            for (int c = 0; c < 2; c++) {
                const float a = field[(size_t(iy) * gw + ix) * 2 + c], b = field[(size_t(iy) * gw + jx) * 2 + c];
                const float d = field[(size_t(jy) * gw + ix) * 2 + c], e = field[(size_t(jy) * gw + jx) * 2 + c];
                mv[c] = (a + (b - a) * ax) * (1 - ay) + (d + (e - d) * ax) * ay;
            }
            const int sx = std::clamp(int(std::lround(x - mv[0])), 0, W - 1), sy = std::clamp(int(std::lround(y - mv[1])), 0, H - 1);
            std::copy_n(cur.At(sx, sy), 3, out.At(x, y));
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return std::printf("usage: motion-bench <image>\n"), 1;
    Image source;
    int channels = 0;
    uint8_t* pixels = stbi_load(argv[1], &source.w, &source.h, &channels, 4);
    if (!pixels) return std::printf("can't read %s\n", argv[1]), 1;
    source.rgba.assign(pixels, pixels + size_t(source.w) * source.h * 4);
    stbi_image_free(pixels);

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx)))
        return std::printf("no device\n"), 1;
    ComPtr<IDXGIDevice> dxgi;
    device.As(&dxgi);
    ComPtr<IDXGIAdapter> adapter;
    dxgi->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC ad;
    adapter->GetDesc(&ad);
    std::printf("GPU: %ls\n", ad.Description);

    rcs::MotionEstimator me;
    if (!me.Init(device.Get(), W, H)) return std::printf("estimator failed\n"), 1;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W;
    td.Height = H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    device->CreateTexture2D(&td, nullptr, &tex);
    ComPtr<ID3D11ShaderResourceView> srv;
    device->CreateShaderResourceView(tex.Get(), nullptr, &srv);

    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    ComPtr<ID3D11Query> disjoint, t0, t1;
    device->CreateQuery(&qd, &disjoint);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    device->CreateQuery(&qd, &t0);
    device->CreateQuery(&qd, &t1);

    const Scene scenes[] = {
        {"object moving fast", 0, 24, 0, false},
        {"two objects, different speeds", 0, 8, 40, false},
        {"whole picture panning", 12, 0, 0, false},
        {"objects over a pan", 6, 24, 16, false},
        {"nothing moving, with noise", 0, 0, 0, true},
        {"slow motion, with noise", 0, 4, 2, true},
        {"head turning 1 degree a frame", 0, 0, 0, false, 1.f},
        {"head turning 3 degrees a frame", 0, 0, 0, false, 3.f},
    };
    std::printf("%-32s %10s %10s %8s  %s\n", "scene", "repeat dB", "warp dB", "GPU ms", "blocks moved");
    for (const Scene& s : scenes) {
        std::mt19937 rng(7);
        // A direction from this frame's head, as the last frame's head saw it: one step less turned.
        const float back = s.yawDegrees * 3.14159265f / 180.f;
        const float turn[9] = {std::cos(back), 0, std::sin(back), 0, 1, 0, -std::sin(back), 0, std::cos(back)};
        Image prev = Frame(s, source, 0, rng), cur = Frame(s, source, 1, rng), next = Frame(s, source, 2, rng);
        ctx->UpdateSubresource(tex.Get(), 0, nullptr, prev.rgba.data(), W * 4, 0);
        me.Process(ctx.Get(), srv.Get(), turn, kFov);
        ctx->UpdateSubresource(tex.Get(), 0, nullptr, cur.rgba.data(), W * 4, 0);
        ctx->Begin(disjoint.Get());
        ctx->End(t0.Get());
        me.Process(ctx.Get(), srv.Get(), turn, kFov);
        ctx->End(t1.Get());
        ctx->End(disjoint.Get());
        std::vector<float> field;
        me.Read(ctx.Get(), field);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        while (ctx->GetData(disjoint.Get(), &dj, sizeof(dj), 0) != S_OK) {}
        UINT64 a = 0, b = 0;
        while (ctx->GetData(t0.Get(), &a, sizeof(a), 0) != S_OK) {}
        while (ctx->GetData(t1.Get(), &b, sizeof(b), 0) != S_OK) {}
        int moved = 0;
        for (size_t i = 0; i + 1 < field.size(); i += 2)
            if (field[i] != 0 || field[i + 1] != 0) moved++;
        const Image warped = Warp(cur, field, int(me.GridWidth()), int(me.GridHeight()));
        std::printf("%-32s %10.2f %10.2f %8.2f  %d of %zu\n", s.name, Psnr(cur, next), Psnr(warped, next),
                    double(b - a) * 1000.0 / double(dj.Frequency), moved, field.size() / 2);
    }
    return 0;
}
