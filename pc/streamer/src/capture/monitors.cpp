#include "monitors.h"

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <map>

#include "../util.h"

namespace rcs {

// Maps GDI device names (\\.\DISPLAY1) to the monitor's friendly name and
// refresh rate using the DisplayConfig API.
static void QueryDisplayConfig(std::map<std::wstring, std::pair<std::wstring, double>>& out) {
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) !=
        ERROR_SUCCESS)
        return;
    for (UINT32 i = 0; i < pathCount; i++) {
        const auto& path = paths[i];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        std::wstring friendly;
        if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) friendly = target.monitorFriendlyDeviceName;
        double hz = 60;
        const auto& rr = path.targetInfo.refreshRate;
        if (rr.Denominator) hz = double(rr.Numerator) / rr.Denominator;
        out[source.viewGdiDeviceName] = {friendly, hz};
    }
}

std::vector<MonitorInfo> EnumerateMonitors() {
    std::vector<MonitorInfo> monitors;
    std::map<std::wstring, std::pair<std::wstring, double>> names;
    QueryDisplayConfig(names);

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return monitors;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; o++) {
            DXGI_OUTPUT_DESC od{};
            output->GetDesc(&od);
            if (!od.AttachedToDesktop) continue;
            MonitorInfo m;
            m.id = Narrow(od.DeviceName);
            m.desktopRect = od.DesktopCoordinates;
            m.adapterLuid = ad.AdapterLuid;
            switch (od.Rotation) {
                case DXGI_MODE_ROTATION_ROTATE90: m.rotation = 90; break;
                case DXGI_MODE_ROTATION_ROTATE180: m.rotation = 180; break;
                case DXGI_MODE_ROTATION_ROTATE270: m.rotation = 270; break;
                default: m.rotation = 0; break;
            }
            m.width = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
            m.height = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(od.Monitor, &mi)) m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
            if (auto it = names.find(od.DeviceName); it != names.end()) {
                m.name = Narrow(it->second.first);
                m.refreshHz = it->second.second;
            }
            if (m.name.empty()) m.name = m.primary ? "Primary display" : "Display";
            monitors.push_back(m);
        }
    }
    std::stable_sort(monitors.begin(), monitors.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
        if (a.primary != b.primary) return a.primary;
        return a.desktopRect.left < b.desktopRect.left;
    });
    for (size_t i = 0; i < monitors.size(); i++) monitors[i].index = int(i);
    return monitors;
}

}  // namespace rcs
