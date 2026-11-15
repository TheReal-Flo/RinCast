// rincast-monado: RinCast's Monado service. The streamer starts it when a headset launches VR with
// Monado as the OpenXR runtime (and, on Windows, Monado's OpenXR runtime starts it when a game
// opens it). It runs Monado's IPC server with RinCast's devices and compositor target
// (rincast_monado.h) on the GPU the streamer encodes on.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "rincast_monado.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/prctl.h>
#endif

#include "server/ipc_server_interface.h"
#include "util/u_logging.h"
#include "util/u_metrics.h"
#include "util/u_trace_marker.h"

namespace {

void SetEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// The index, among Vulkan's GPUs, of the one the streamer encodes on: frames never cross GPUs,
// and games are told to render on it too. -1 if it can't be found.
int StreamerGpuIndex() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) return -1;
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    const rcvr::Shared* s = rcm::Hub::Get().Shared();
    int found = -1;
    for (uint32_t i = 0; i < count && found < 0; i++) {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
        vkGetPhysicalDeviceProperties2(devices[i], &props);
#ifdef _WIN32
        rcvr::DisplayConfig config{};
        rcvr::ReadDisplayConfig(s, config);
        LUID luid;
        std::memcpy(&luid, id.deviceLUID, sizeof(luid));
        if (id.deviceLUIDValid && luid.LowPart == config.adapterLuid.LowPart && luid.HighPart == config.adapterLuid.HighPart)
            found = int(i);
#else
        if (std::memcmp(id.deviceUUID, s->deviceUuid, sizeof(id.deviceUUID)) == 0) found = int(i);
#endif
    }
    vkDestroyInstance(instance, nullptr);
    return found;
}

}  // namespace

int main(int argc, char* argv[]) {
    u_trace_marker_init();
    u_metrics_init();
#ifndef _WIN32
    // The streamer starts this service: it ends with the streamer, whose images it renders into.
    prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
    if (!rcm::Hub::Get().Open(10'000)) {
        std::fprintf(stderr, "rincast-monado: RinCast Streamer isn't running (no VR bridge). Start it first.\n");
        return 1;
    }
    // The compositor's graphics path renders through sRGB views: the frame images then hold the
    // gamma-encoded colour the encoder takes, as SteamVR's do.
    SetEnv("XRT_COMPOSITOR_COMPUTE", "0");
    if (const int gpu = StreamerGpuIndex(); gpu >= 0) {
        SetEnv("XRT_COMPOSITOR_FORCE_GPU_INDEX", std::to_string(gpu));
        SetEnv("XRT_COMPOSITOR_FORCE_CLIENT_GPU_INDEX", std::to_string(gpu));
    } else {
        U_LOG_W("RinCast: couldn't find the streamer's GPU among Vulkan's; using Monado's choice");
    }

    ipc_server_main_info info{};
    std::snprintf(info.udgci.window_title, sizeof(info.udgci.window_title), "RinCast Monado");
    info.udgci.open = U_DEBUG_GUI_OPEN_NEVER;
    info.exit_on_disconnect = false;
    info.no_stdin = true;
    const int ret = ipc_server_main(argc, argv, &info);
    rcm::Hub::Get().StopHeartbeat();
    u_metrics_close();
    return ret;
}
