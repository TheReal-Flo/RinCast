#include "motion_estimator.h"

#include <d3dcompiler.h>

#include <cmath>
#include <cstring>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace rcs {

namespace {

const char* kLumaCs = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<unorm float> dst : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    float sum = 0;
    for (uint y = 0; y < 4; y++)
        for (uint x = 0; x < 4; x++)
            sum += dot(src.Load(int3(id.xy * 4 + uint2(x, y), 0)).rgb, float3(0.2126, 0.7152, 0.0722));
    dst[id.xy] = sum / 16;
}
)";

const char* kDownCs = R"(
Texture2D<float> src : register(t0);
RWTexture2D<unorm float> dst : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    float sum = 0;
    for (uint y = 0; y < 4; y++)
        for (uint x = 0; x < 4; x++) sum += src.Load(int3(id.xy * 4 + uint2(x, y), 0));
    dst[id.xy] = sum / 16;
}
)";

// Where a block's contents would have been in the last frame if only the head had turned.
const char* kExpected = R"(
cbuffer Constants : register(b0) {
    float4 turn0, turn1, turn2;
    float4 fov[2];
    float2 size;
};
float2 ExpectedPixels(uint2 block) {
    const float2 c = (float2(block) + 0.5) * 32.0;
    const float eyeW = size.x * 0.5;
    const int eye = c.x >= eyeW ? 1 : 0;
    const float4 f = fov[eye];
    const float u = (c.x - eye * eyeW) / eyeW, v = c.y / size.y;
    const float3 ray = float3(-f.x + u * (f.x + f.y), f.z - v * (f.z + f.w), -1.0);
    const float3 r = float3(dot(turn0.xyz, ray), dot(turn1.xyz, ray), dot(turn2.xyz, ray));
    if (r.z > -0.05) return float2(0, 0);
    const float tx = r.x / -r.z, ty = r.y / -r.z;
    const float un = (tx + f.x) / (f.x + f.y), vn = (f.z - ty) / (f.z + f.w);
    return float2((un - u) * eyeW, (vn - v) * size.y);
}
// The other eye's picture is no match: x, in pixels of an image size.x / scale wide.
bool SameEye(int cornerX, int offsetX, int blockW, float scale) {
    const int eyeW = int(size.x * 0.5 / scale);
    const int left = cornerX < eyeW ? 0 : eyeW;
    return cornerX + offsetX >= left && cornerX + offsetX + blockW <= left + eyeW;
}
)";

// Full search on the 1/16 images, around where the head turn alone would put it. A block is
// 2 x 2 pixels here, matched over the 4 x 4 around it. Result: where the block's contents were
// in the last frame, relative to now, in 1/16 pixels.
const char* kCoarseCs = R"(
Texture2D<float> cur : register(t0);
Texture2D<float> prev : register(t1);
RWTexture2D<float2> dst : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    float best = 1e9;
    int2 bestOffset = 0;
    const int2 corner = int2(id.xy) * 2 - 1;
    const int2 base = int2(round(ExpectedPixels(id.xy) / 16.0));
    bestOffset = base;
    for (int dy = -4; dy <= 4; dy++) {
        for (int dx = -8; dx <= 8; dx++) {
            const int2 o = base + int2(dx, dy);
            if (!SameEye(corner.x, o.x, 4, 16.0)) continue;
            float sad = 0;
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++)
                    sad += abs(cur.Load(int3(corner + int2(x, y), 0)) - prev.Load(int3(corner + int2(x + o.x, y + o.y), 0)));
            // Short vectors win ties, and by enough to matter on flat areas: their SAD is
            // nearly the same everywhere, so without this a block of plain wall matched a
            // hundred pixels away and the frame generation bent the picture by it.
            sad += 0.5 * (abs(dx) + abs(dy));
            if (sad < best) {
                best = sad;
                bestOffset = o;
            }
        }
    }
    dst[id.xy] = float2(bestOffset);
}
)";

// On the 1/4 images: tries this block's coarse vector, its neighbours' and none, then searches
// around the best. A block is 8 x 8 pixels here. Result in full pixels of motion: prev -> now.
const char* kRefineCs = R"(
Texture2D<float> cur : register(t0);
Texture2D<float> prev : register(t1);
Texture2D<float2> coarse : register(t2);
RWTexture2D<float2> dst : register(u0);
float Sad(int2 corner, int2 o) {
    float sad = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            sad += abs(cur.Load(int3(corner + int2(x, y), 0)) - prev.Load(int3(corner + int2(x + o.x, y + o.y), 0)));
    return sad;
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const int2 corner = int2(id.xy) * 8;
    const int2 base = int2(round(ExpectedPixels(id.xy) / 4.0));
    const float zero = Sad(corner, base);
    float best = zero;
    int2 bestOffset = base;
    static const int2 near[5] = {int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
    for (int i = 0; i < 5; i++) {
        const int2 c = int2(coarse.Load(int3(int2(id.xy) + near[i], 0))) * 4;
        if (!SameEye(corner.x, c.x, 8, 4.0)) continue;
        const float sad = Sad(corner, c);
        if (sad < best) {
            best = sad;
            bestOffset = c;
        }
    }
    const int2 centre = bestOffset;
    for (int dy = -3; dy <= 3; dy++) {
        for (int dx = -3; dx <= 3; dx++) {
            if (!SameEye(corner.x, centre.x + dx, 8, 4.0)) continue;
            const float sad = Sad(corner, centre + int2(dx, dy));
            if (sad < best) {
                best = sad;
                bestOffset = centre + int2(dx, dy);
            }
        }
    }
    // Moving only counts if it clearly matches better than staying: this ignores noise,
    // compression and flat areas, where a wrong vector would bend the picture for nothing.
    if (best > zero * 0.8 - 0.02) bestOffset = base;
    // More than 32 px of content motion in one frame (2880 px/s) is either a wrong match or
    // more than a held frame can be bent into: keep what the head turn alone explains.
    if (abs(bestOffset.x - base.x) > 8 || abs(bestOffset.y - base.y) > 8) bestOffset = base;
    dst[id.xy] = float2(base - bestOffset) * 4.0;
}
)";

// Component-wise median of each 3 x 3: single wrong blocks go away, edges stay.
const char* kMedianCs = R"(
Texture2D<float2> src : register(t0);
RWTexture2D<float2> dst : register(u0);
float Median(float v[9]) {
    for (int i = 0; i < 5; i++) {
        for (int j = i + 1; j < 9; j++) {
            if (v[j] < v[i]) {
                const float t = v[i];
                v[i] = v[j];
                v[j] = t;
            }
        }
    }
    return v[4];
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    float x[9], y[9];
    for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 3; i++) {
            const float2 m = src.Load(int3(int2(id.xy) + int2(i - 1, j - 1), 0));
            x[j * 3 + i] = m.x;
            y[j * 3 + i] = m.y;
        }
    }
    dst[id.xy] = float2(Median(x), Median(y));
}
)";

ComPtr<ID3D11ComputeShader> Compile(ID3D11Device* device, const char* source) {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(source, std::strlen(source), "motion", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                          &code, &errors))) {
        if (errors) OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
        return nullptr;
    }
    ComPtr<ID3D11ComputeShader> cs;
    device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs);
    return cs;
}

UINT Groups(uint32_t size) { return (size + 7) / 8; }

}  // namespace

bool MotionEstimator::MakeLevel(DXGI_FORMAT format, uint32_t w, uint32_t h, Level& level) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    return SUCCEEDED(m_device->CreateTexture2D(&d, nullptr, &level.tex)) &&
           SUCCEEDED(m_device->CreateShaderResourceView(level.tex.Get(), nullptr, &level.srv)) &&
           SUCCEEDED(m_device->CreateUnorderedAccessView(level.tex.Get(), nullptr, &level.uav));
}

bool MotionEstimator::Init(ID3D11Device* device, uint32_t width, uint32_t height) {
    m_device = device;
    m_width = width;
    m_height = height;
    m_quarterW = width / 4;
    m_quarterH = height / 4;
    m_sixteenthW = m_quarterW / 4;
    m_sixteenthH = m_quarterH / 4;
    m_gridWidth = width / kBlockSize;
    m_gridHeight = height / kBlockSize;
    m_hasPrevious = false;
    m_lumaCs = Compile(device, kLumaCs);
    m_downCs = Compile(device, kDownCs);
    m_coarseCs = Compile(device, (std::string(kExpected) + kCoarseCs).c_str());
    m_refineCs = Compile(device, (std::string(kExpected) + kRefineCs).c_str());
    m_medianCs = Compile(device, kMedianCs);
    if (!m_lumaCs || !m_downCs || !m_coarseCs || !m_refineCs || !m_medianCs) return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 96;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&bd, nullptr, &m_constants))) return false;
    for (int i = 0; i < 2; i++) {
        if (!MakeLevel(DXGI_FORMAT_R8_UNORM, m_quarterW, m_quarterH, m_quarter[i]) ||
            !MakeLevel(DXGI_FORMAT_R8_UNORM, m_sixteenthW, m_sixteenthH, m_sixteenth[i]))
            return false;
    }
    if (!MakeLevel(DXGI_FORMAT_R16G16_FLOAT, m_gridWidth, m_gridHeight, m_coarse) ||
        !MakeLevel(DXGI_FORMAT_R16G16_FLOAT, m_gridWidth, m_gridHeight, m_refined) ||
        !MakeLevel(DXGI_FORMAT_R16G16_FLOAT, m_gridWidth, m_gridHeight, m_field))
        return false;
    D3D11_TEXTURE2D_DESC d{};
    m_field.tex->GetDesc(&d);
    d.BindFlags = 0;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(device->CreateTexture2D(&d, nullptr, &m_staging));
}

void MotionEstimator::Dispatch(ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs, ID3D11ShaderResourceView* const* srvs, UINT srvCount,
                               ID3D11UnorderedAccessView* uav, UINT x, UINT y) {
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetShaderResources(0, srvCount, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx->Dispatch(x, y, 1);
    ID3D11ShaderResourceView* noSrv[3] = {};
    ID3D11UnorderedAccessView* noUav = nullptr;
    ctx->CSSetShaderResources(0, 3, noSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
}

bool MotionEstimator::Process(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* source, const float headTurn[9], const float fovTan[2][4]) {
    D3D11_MAPPED_SUBRESOURCE constants;
    if (FAILED(ctx->Map(m_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &constants))) return false;
    auto* c = static_cast<float*>(constants.pData);
    for (int row = 0; row < 3; row++) {
        std::memcpy(c + row * 4, headTurn + row * 3, sizeof(float) * 3);
        c[row * 4 + 3] = 0;
    }
    std::memcpy(c + 12, fovTan, sizeof(float) * 8);
    c[20] = float(m_width);
    c[21] = float(m_height);
    ctx->Unmap(m_constants.Get(), 0);
    ID3D11Buffer* buffer = m_constants.Get();
    ctx->CSSetConstantBuffers(0, 1, &buffer);
    m_current ^= 1;
    Level& quarter = m_quarter[m_current];
    Level& sixteenth = m_sixteenth[m_current];
    Dispatch(ctx, m_lumaCs.Get(), &source, 1, quarter.uav.Get(), Groups(m_quarterW), Groups(m_quarterH));
    ID3D11ShaderResourceView* quarterSrv = quarter.srv.Get();
    Dispatch(ctx, m_downCs.Get(), &quarterSrv, 1, sixteenth.uav.Get(), Groups(m_sixteenthW), Groups(m_sixteenthH));
    const bool compared = m_hasPrevious;
    if (compared) {
        ID3D11ShaderResourceView* coarseIn[2] = {sixteenth.srv.Get(), m_sixteenth[m_current ^ 1].srv.Get()};
        Dispatch(ctx, m_coarseCs.Get(), coarseIn, 2, m_coarse.uav.Get(), Groups(m_gridWidth), Groups(m_gridHeight));
        ID3D11ShaderResourceView* refineIn[3] = {quarter.srv.Get(), m_quarter[m_current ^ 1].srv.Get(), m_coarse.srv.Get()};
        Dispatch(ctx, m_refineCs.Get(), refineIn, 3, m_refined.uav.Get(), Groups(m_gridWidth), Groups(m_gridHeight));
        ID3D11ShaderResourceView* medianIn = m_refined.srv.Get();
        Dispatch(ctx, m_medianCs.Get(), &medianIn, 1, m_field.uav.Get(), Groups(m_gridWidth), Groups(m_gridHeight));
    }
    m_hasPrevious = true;
    return compared;
}

bool MotionEstimator::Read(ID3D11DeviceContext* ctx, std::vector<float>& out) {
    ctx->CopyResource(m_staging.Get(), m_field.tex.Get());
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(ctx->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    out.resize(size_t(m_gridWidth) * m_gridHeight * 2);
    for (uint32_t y = 0; y < m_gridHeight; y++) {
        const auto* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(map.pData) + y * map.RowPitch);
        for (uint32_t x = 0; x < m_gridWidth * 2; x++) {
            // Half float to float.
            const uint16_t h = row[x];
            const int exponent = (h >> 10) & 31;
            const float magnitude = exponent == 0 ? (h & 1023) / 16777216.f : std::ldexp(float(1024 + (h & 1023)), exponent - 25);
            out[size_t(y) * m_gridWidth * 2 + x] = (h & 0x8000) ? -magnitude : magnitude;
        }
    }
    ctx->Unmap(m_staging.Get(), 0);
    return true;
}

}  // namespace rcs
