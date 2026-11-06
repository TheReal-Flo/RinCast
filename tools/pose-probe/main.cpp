// Pose probe: reads the right controller's pose the way a SteamVR game sees it, while
// headset-sim --move moves that controller on a known path, and prints how far ahead
// of (or behind) real time SteamVR's pose is. Used to check how SteamVR applies the
// driver's poseTimeOffset and velocities.
//
//   pose-probe [seconds]
//   pose-probe --scene [seconds] [log] [busy ms] run as a VR app and log the poses of each frame
//
// The path (shared with headset-sim): every 8 s of QPC time, 4 s moving along x from
// -1 m to +1 m at 0.5 m/s, then 4 s standing still at x = 0 (to measure the constant
// grip offset the driver adds).
#include <windows.h>
#include <d3d11.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

#include "openvr/openvr.h"

typedef void* (*InitFn)(vr::EVRInitError*, vr::EVRApplicationType, const char*);
typedef void* (*GetIfaceFn)(const char*, vr::EVRInitError*);
typedef void (*ShutdownFn)();

static double NowS() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// --scene: run as a VR app. Logs, per frame, when it was submitted, which tracking sample
// the head pose came from (headset-sim --move encodes it in the head height) and the time
// the right controller's pose corresponds to on the --move path.
static int RunScene(GetIfaceFn get, double seconds, const char* logPath, double busyMs) {
    vr::EVRInitError err;
    auto* sys = (vr::IVRSystem*)get(vr::IVRSystem_Version, &err);
    auto* comp = (vr::IVRCompositor*)get(vr::IVRCompositor_Version, &err);
    auto* overlay = (vr::IVROverlay*)get(vr::IVROverlay_Version, &err);
    if (!sys || !comp) return std::printf("no IVRSystem / IVRCompositor\n"), 3;
    comp->SetTrackingSpace(vr::TrackingUniverseRawAndUncalibrated);
    ID3D11Device* dev = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                                 nullptr, nullptr)))
        return std::printf("no D3D11 device\n"), 4;
    uint32_t w = 0, h = 0;
    sys->GetRecommendedRenderTargetSize(&w, &h);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w ? w : 1024;
    d.Height = h ? h : 1024;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* tex = nullptr;
    dev->CreateTexture2D(&d, nullptr, &tex);
    // A known colour, to check the picture that reaches the stream (tools/bridge-grab).
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (tex && SUCCEEDED(dev->CreateRenderTargetView(tex, nullptr, &rtv))) {
        const float color[4] = {128 / 255.f, 64 / 255.f, 32 / 255.f, 1};
        ctx->ClearRenderTargetView(rtv, color);
        ctx->Flush();
    }
    FILE* log = std::fopen(logPath, "w");
    const double start = NowS();
    int frames = 0;
    while (NowS() - start < seconds) {
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
        comp->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
        const double gotPoses = NowS();
        const auto& hmd = poses[vr::k_unTrackedDeviceIndex_Hmd];
        const long sample = hmd.bPoseIsValid ? std::lround((hmd.mDeviceToAbsoluteTracking.m[1][3] - 1.0) * 1000) : -1;
        double ctrlX = 1e9;
        const auto idx = sys->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
        if (idx != vr::k_unTrackedDeviceIndexInvalid && poses[idx].bPoseIsValid) ctrlX = poses[idx].mDeviceToAbsoluteTracking.m[0][3];
        // Stand in for a game that needs most of the frame to render.
        while ((NowS() - gotPoses) * 1000 < busyMs) {}
        vr::Texture_t t{tex, vr::TextureType_DirectX, vr::ColorSpace_Auto};
        comp->Submit(vr::Eye_Left, &t);
        comp->Submit(vr::Eye_Right, &t);
        const double submitted = NowS();
        // Last column: the dashboard is open (SteamVR then shows its own scene, not ours).
        const int dashboard = overlay && overlay->IsDashboardVisible() ? 1 : 0;
        if (log)
            std::fprintf(log, "%lld %ld %.6f %.6f %d\n", (long long)(submitted * 1e9), sample, ctrlX, gotPoses, dashboard);
        frames++;
    }
    if (log) std::fclose(log);
    std::printf("%d frames\n", frames);
    return 0;
}

int main(int argc, char** argv) {
    const bool scene = argc > 1 && std::string(argv[1]) == "--scene";
    const double seconds = argc > 1 + scene ? atof(argv[1 + scene]) : 24;
    HMODULE dll = LoadLibraryExA("C:/Program Files (x86)/Steam/steamapps/common/SteamVR/bin/win64/openvr_api.dll", nullptr,
                                 LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) return std::printf("openvr_api.dll not found\n"), 1;
    auto init = (InitFn)GetProcAddress(dll, "VR_InitInternal2");
    auto get = (GetIfaceFn)GetProcAddress(dll, "VR_GetGenericInterface");
    auto shutdown = (ShutdownFn)GetProcAddress(dll, "VR_ShutdownInternal");
    vr::EVRInitError err;
    init(&err, scene ? vr::VRApplication_Scene : vr::VRApplication_Background, nullptr);
    if (err != vr::VRInitError_None) return std::printf("VR_Init failed: %d\n", err), 2;
    if (scene) {
        const int r = RunScene(get, seconds, argc > 2 + scene ? argv[2 + scene] : "scene.txt", argc > 3 + scene ? atof(argv[3 + scene]) : 0);
        shutdown();
        return r;
    }
    auto* sys = (vr::IVRSystem*)get(vr::IVRSystem_Version, &err);
    if (!sys) return std::printf("no IVRSystem\n"), 3;

    double stillSum = 0, shiftSum = 0, velSum = 0;
    int stillN = 0, shiftN = 0;
    const double start = NowS();
    while (NowS() - start < seconds) {
        const vr::TrackedDeviceIndex_t idx = sys->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
        sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseRawAndUncalibrated, 0, poses, vr::k_unMaxTrackedDeviceCount);
        const double now = NowS();
        if (idx != vr::k_unTrackedDeviceIndexInvalid && poses[idx].bPoseIsValid) {
            const double x = poses[idx].mDeviceToAbsoluteTracking.m[0][3];
            const double phase = std::fmod(now, 8.0);
            if (phase > 4.3 && phase < 7.7) {
                stillSum += x;
                stillN++;
            } else if (stillN && phase > 0.5 && phase < 3.5) {
                const double implied = (x - stillSum / stillN + 1.0) / 0.5;  // seconds into the cycle
                shiftSum += implied - phase;
                velSum += poses[idx].vVelocity.v[0];
                shiftN++;
            }
        }
        Sleep(5);
    }
    if (shiftN)
        std::printf("SteamVR's controller pose is %+.1f ms ahead of real time (%d samples, velocity %.2f m/s)\n",
                    shiftSum / shiftN * 1000, shiftN, velSum / shiftN);
    else
        std::printf("no samples (still %d)\n", stillN);
    shutdown();
    return 0;
}
