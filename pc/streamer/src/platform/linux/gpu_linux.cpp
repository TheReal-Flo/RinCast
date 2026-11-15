// The GPU on Linux: from the DRM render nodes in sysfs. The one games use is what we encode on:
// a discrete GPU (NVIDIA, AMD) before an integrated one.
#include <dirent.h>

#include <cstdlib>
#include <fstream>
#include <string>

#include "gpu.h"
#include "rc/platform.h"

namespace rcs {

namespace {

std::string ReadLine(const std::string& path) {
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    return s;
}

const char* VendorName(UINT vendor) {
    switch (vendor) {
        case 0x10DE: return "NVIDIA";
        case 0x1002: return "AMD";
        case 0x8086: return "Intel";
    }
    return "GPU";
}

// The model where the driver says it: NVIDIA's proc file, amdgpu's product name.
std::string ModelName(const std::string& device, UINT vendor) {
    if (vendor == 0x10DE) {
        if (DIR* d = opendir("/proc/driver/nvidia/gpus")) {
            while (dirent* e = readdir(d)) {
                if (e->d_name[0] == '.') continue;
                std::ifstream f(std::string("/proc/driver/nvidia/gpus/") + e->d_name + "/information");
                std::string line;
                while (std::getline(f, line))
                    if (line.rfind("Model:", 0) == 0) {
                        closedir(d);
                        const auto start = line.find_first_not_of(" \t", 6);
                        return start == std::string::npos ? std::string() : line.substr(start);
                    }
            }
            closedir(d);
        }
    }
    return ReadLine(device + "/product_name");
}

}  // namespace

GpuInfo DescribeGpu() {
    GpuInfo best;
    int bestRank = -1;
    if (DIR* d = opendir("/sys/class/drm")) {
        while (dirent* e = readdir(d)) {
            const std::string node = e->d_name;
            if (node.rfind("renderD", 0) != 0) continue;
            const std::string device = "/sys/class/drm/" + node + "/device";
            const std::string vendorText = ReadLine(device + "/vendor");  // "0x10de"
            const UINT vendor = vendorText.empty() ? 0 : UINT(std::strtoul(vendorText.c_str(), nullptr, 16));
            const int rank = vendor == 0x10DE || vendor == 0x1002 ? 2 : vendor ? 1 : 0;
            if (rank <= bestRank) continue;
            bestRank = rank;
            best.vendorId = vendor;
            const std::string model = ModelName(device, vendor);
            best.name = model.empty() ? std::string(VendorName(vendor)) + " GPU (" + node + ")" : model;
        }
        closedir(d);
    }
    if (bestRank < 0) best.name = "no GPU found";
    return best;
}

}  // namespace rcs
