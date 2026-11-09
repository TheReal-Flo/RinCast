#include "xr_loader.h"

#include <dlfcn.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#else
#include <cstdlib>
#include <vector>
#endif

#include <cstring>
#include <string>

#include "rc/platform.h"

#define RV_XR_DEFINE(name) PFN_##name name = nullptr;
RV_XR_DEFINE(xrGetInstanceProcAddr)
RV_XR_GLOBAL_FUNCTIONS(RV_XR_DEFINE)
RV_XR_INSTANCE_FUNCTIONS(RV_XR_DEFINE)
#undef RV_XR_DEFINE

namespace rv::xrloader {

namespace {

// Packaged in the APK (see CMakeLists.txt).
constexpr const char* kKhronos = "libopenxr_loader.so";
constexpr const char* kPico = "libopenxr_loader_pico.so";
constexpr const char* kQuest1 = "libopenxr_loader_quest1.so";  // only in the Quest 1 build
const char* g_name = kKhronos;

#ifdef __ANDROID__
std::string Property(const char* name) {
    char value[PROP_VALUE_MAX] = {};
    __system_property_get(name, value);
    return value;
}
#else
std::string Property(const char*) { return {}; }  // not a PICO or a Quest 1
#endif

template <typename T>
bool Get(XrInstance instance, const char* name, T& out) {
    PFN_xrVoidFunction f = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(instance, name, &f)) || !f) {
        RC_LOGE("OpenXR: %s not found in %s", name, g_name);
        return false;
    }
    out = reinterpret_cast<T>(f);
    return true;
}

// PICO's loader on PICO headsets older than the PICO 4 Ultra.
[[maybe_unused]] bool UsePicoLoader() {
    // The headsets that shipped before PICO OS 5.9 brought the standard loader (as in ALVR).
    // PICO's own loader still works on them after updates, so these always get it; the PICO 4
    // Ultra and anything newer use the standard one. Chinese domestic units report codenames
    // instead of marketing names: the PICO 4 is "Phoenix" (model A8110).
    const std::string manufacturer = Property("ro.product.manufacturer");
    if (strcasecmp(manufacturer.c_str(), "pico") != 0) return false;
    const std::string model = Property("ro.product.model");
    const std::string product = Property("ro.product.name");
    RC_LOGI("PICO headset: model '%s', product '%s'", model.c_str(), product.c_str());
    if (model == "Pico Neo 3" || model == "Pico Neo3 Link") return true;
    if (model == "A8110" || model == "A8110 Pro") return true;
    return product == "PICO 4" || product == "PICO 4 Pro" || product == "PICO 4 Enterprise" || product == "PICO G3" ||
           product == "Phoenix" || product == "Phoenix Pro";
}

}  // namespace

// Meta's loader on the Quest 1, whose Horizon OS stopped before the standard loader worked there.
bool IsQuest1() {
    return strcasecmp(Property("ro.product.manufacturer").c_str(), "oculus") == 0 &&
           Property("ro.product.device") == "monterey";
}

bool Load() {
    if (xrGetInstanceProcAddr) return true;
    void* lib = nullptr;
#ifndef __ANDROID__
    // Linux (Steam Frame): SteamVR's loader, launched from Steam. RINCAST_OPENXR_LOADER names
    // another one; a system-wide loader comes next.
    static std::string chosen;
    std::vector<std::string> candidates;
    if (const char* env = std::getenv("RINCAST_OPENXR_LOADER"); env && *env) candidates.push_back(env);
    candidates.push_back("libopenxr_loader.so.1");
    candidates.push_back("libopenxr_loader.so");
    const std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    for (const char* arch : {"linuxarm64", "linux64"}) {
        candidates.push_back(std::string("/opt/steamvr/bin/") + arch + "/libopenxr_loader.so");
        for (const char* steam : {"/.local/share/Steam", "/.steam/steam"})
            candidates.push_back(home + steam + "/steamapps/common/SteamVR/bin/" + arch + "/libopenxr_loader.so");
    }
    for (const auto& c : candidates) {
        lib = dlopen(c.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (lib) {
            chosen = c;
            g_name = chosen.c_str();
            break;
        }
    }
    if (!lib) {
        RC_LOGE("OpenXR: no loader found (start RinCast from Steam, or set RINCAST_OPENXR_LOADER)");
        return false;
    }
#else
    if (IsQuest1()) {
        g_name = kQuest1;
        lib = dlopen(g_name, RTLD_NOW | RTLD_LOCAL);
        if (!lib) RC_LOGE("OpenXR: this is a Quest 1, which needs RinCast's Quest 1 build (%s)", dlerror());
    } else {
        g_name = UsePicoLoader() ? kPico : kKhronos;
        lib = dlopen(g_name, RTLD_NOW | RTLD_LOCAL);
    }
#endif
    if (!lib) {
        RC_LOGE("OpenXR: couldn't load %s: %s", g_name, dlerror());
        return false;
    }
    xrGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(dlsym(lib, "xrGetInstanceProcAddr"));
    if (!xrGetInstanceProcAddr) {
        RC_LOGE("OpenXR: %s has no xrGetInstanceProcAddr", g_name);
        return false;
    }
    RC_LOGI("OpenXR loader: %s", g_name);
#define RV_XR_GET(name) ok = Get(XR_NULL_HANDLE, #name, name) && ok;
    bool ok = true;
    RV_XR_GLOBAL_FUNCTIONS(RV_XR_GET)
    return ok;
}

bool LoadInstance(XrInstance instance) {
    bool ok = true;
#undef RV_XR_GET
#define RV_XR_GET(name) ok = Get(instance, #name, name) && ok;
    RV_XR_INSTANCE_FUNCTIONS(RV_XR_GET)
#undef RV_XR_GET
    return ok;
}

const char* Name() { return g_name; }

}  // namespace rv::xrloader
