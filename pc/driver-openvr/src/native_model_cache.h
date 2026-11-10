// Private, local copies of SteamVR models with the headset's measured pose components.
// Valve's source assets are neither modified nor bundled in RinCast releases.
#pragma once
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>
#include "controller_models.h"

namespace rincast::controllers {

inline std::string ReadModelFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read controller model");
    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad()) throw std::runtime_error("Cannot read controller model");
    return contents.str();
}

inline std::string CacheNativeModel(const std::filesystem::path& source, const std::filesystem::path& cacheRoot, int hand,
                                    const rcvr::BridgeController& controller) {
    namespace fs = std::filesystem;
    if (hand < 0 || hand > 1 || !controller.aimValid || (controller.flags & 7) != 3)
        throw std::runtime_error("Controller calibration needs live controller poses");
    const auto deviceInGrip = GripToDevice(hand, true);
    const auto aimInDevice = AimInDevice(controller.pose, controller.aim, deviceInGrip);
    double distanceSquared = 0;
    for (double value : aimInDevice.t) distanceSquared += value * value;
    if (!std::isfinite(distanceSquared) || distanceSquared > 1) throw std::runtime_error("Controller grip and aim are inconsistent");
    auto model = nlohmann::json::parse(ReadModelFile(source));
    if (!model.contains("components") || !model["components"].is_object()) throw std::runtime_error("Controller model has no components");
    auto& components = model["components"];
    for (const char* name : {"grip", "handgrip", "openxr_grip"}) components[name] = ModelComponent(Invert(deviceInGrip));
    for (const char* name : {"tip", "openxr_aim"}) components[name] = ModelComponent(aimInDevice);
    const auto contents = model.dump(2);
    std::vector<fs::path> assets;
    for (const auto& entry : fs::recursive_directory_iterator(source.parent_path())) {
        if (entry.is_symlink()) throw std::runtime_error("Symlink in controller model assets");
        if (entry.is_regular_file()) assets.push_back(entry.path().lexically_relative(source.parent_path()));
    }
    std::sort(assets.begin(), assets.end());
    std::string key = "rincast-native-v1:" + source.stem().string() + contents;
    for (const auto& asset : assets) {
        const auto path = source.parent_path() / asset;
        key += asset.generic_string() + ':' + std::to_string(fs::file_size(path)) + ':' +
               std::to_string(fs::last_write_time(path).time_since_epoch().count());
    }
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char byte : key) hash = (hash ^ byte) * 1099511628211ULL;
    const auto name = "native_" + source.stem().string() + '_' + std::to_string(hash);
    const auto destination = cacheRoot / name;
    const auto validCache = [&] {
        try {
            if (ReadModelFile(destination / (name + ".json")) != contents) return false;
            for (const auto& asset : assets)
                if (!fs::is_regular_file(destination / asset) ||
                    fs::file_size(destination / asset) != fs::file_size(source.parent_path() / asset))
                    return false;
            return true;
        } catch (...) {
            return false;
        }
    };
    // Serialize callers in this process; publication by directory rename also protects
    // readers from a partially copied cache if another process builds the same model.
    static std::mutex mutex;
    static std::atomic<unsigned> sequence{0};
    std::lock_guard lock(mutex);
    if (validCache()) return name;
    fs::create_directories(cacheRoot);
    const auto suffix = std::to_string(GetCurrentProcessId()) + '-' + std::to_string(++sequence);
    const auto temporary = cacheRoot / (".building-" + name + '-' + suffix);
    try {
        fs::create_directory(temporary);
        fs::copy(source.parent_path(), temporary, fs::copy_options::recursive);
        std::ofstream output(temporary / (name + ".json"), std::ios::binary);
        output << contents;
        output.close();
        if (!output) throw std::runtime_error("Cannot write controller model");
        std::error_code error;
        fs::rename(temporary, destination, error);
        if (error) {
            if (!validCache()) {
                if (fs::exists(destination)) fs::rename(destination, cacheRoot / (".invalid-" + name + '-' + suffix));
                fs::rename(temporary, destination);
            } else
                fs::remove_all(temporary);
        }
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(temporary, ignored);  // temporary is a generated child of cacheRoot
        throw;
    }
    return name;
}

}  // namespace rincast::controllers
