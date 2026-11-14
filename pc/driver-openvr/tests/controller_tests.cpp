#include <cstdio>
#include <limits>
#include <thread>
#include "controller_models.h"
#include "native_model_cache.h"

using namespace rincast::controllers;
namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(test)                                                     \
    do {                                                                \
        if (!(test)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #test); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

static bool Near(const RigidPose& a, const RigidPose& b, double tolerance = 2e-5) {
    const double sign = a.q.w * b.q.w + a.q.x * b.q.x + a.q.y * b.q.y + a.q.z * b.q.z < 0 ? -1 : 1;
    if (std::abs(a.q.w - sign * b.q.w) > tolerance || std::abs(a.q.x - sign * b.q.x) > tolerance ||
        std::abs(a.q.y - sign * b.q.y) > tolerance || std::abs(a.q.z - sign * b.q.z) > tolerance)
        return false;
    for (int i = 0; i < 3; ++i)
        if (std::abs(a.t[i] - b.t[i]) > tolerance) return false;
    return true;
}

static rcvr::BridgePose Bridge(const RigidPose& pose) {
    rcvr::BridgePose result{};
    const auto& q = pose.q;
    result.orientation[0] = float(q.x);
    result.orientation[1] = float(q.y);
    result.orientation[2] = float(q.z);
    result.orientation[3] = float(q.w);
    for (int i = 0; i < 3; ++i) result.position[i] = float(pose.t[i]);
    return result;
}

static void TestProfilesAndRegistration() {
    CHECK(ControllerNamesFor("Quest 3", "/interaction_profiles/bytedance/pico4_controller", false)->pico);
    CHECK(ControllerNamesFor("PICO 4", "/interaction_profiles/bytedance/pico4s_controller", false)->picoUltra);
    CHECK(!ControllerNamesFor("PICO 4", "/interaction_profiles/oculus/touch_controller", false)->pico);
    CHECK(!ControllerNamesFor("PICO 4", "", false));
    CHECK(!ControllerNamesFor("Quest 3", "/interaction_profiles/unknown/controller", false));
    CHECK(!ControllerNamesFor("PICO 4", "/interaction_profiles/bytedance/pico4_controller", true)->pico);
    CHECK(ControllerNamesFor("Quest 2", "/interaction_profiles/meta/touch_pro_controller", false)->controllerLeft ==
          "oculus_quest_plus_controller_left");
    rcvr::TrackingSnapshot tracking{};
    constexpr int64_t now = 2'000'000'000;
    CHECK(!HasLiveInputSample(tracking, 0, false, now));
    tracking.receivedPcNs = now - 1;
    tracking.controllers[0].flags = 1;
    CHECK(!HasLiveInputSample(tracking, 0, false, now));
    tracking.controllers[0].flags = 3;
    CHECK(HasLiveInputSample(tracking, 0, false, now));
    CHECK(!HasLiveInputSample(tracking, 1, false, now));
    CHECK(!HasLiveInputSample(tracking, 0, true, now));
    tracking.controllers[0].flags = 7;
    CHECK(HasLiveInputSample(tracking, 0, true, now));
    CHECK(!HasLiveInputSample(tracking, 0, false, now));
    tracking.receivedPcNs = now - 500'000'000;
    CHECK(!HasLiveInputSample(tracking, 0, true, now));
    tracking.receivedPcNs = now + 1;
    CHECK(!HasLiveInputSample(tracking, 0, true, now));
}

static void TestFramesAndBones() {
    rcvr::BridgePose moving{};
    moving.linearVelocity[0] = 1;
    moving.angularVelocity[1] = 2;
    const double lever[] = {0, 0, 0.1};
    const auto velocity = OffsetLinearVelocity(moving, lever);
    CHECK(std::abs(velocity[0] - 1.2) < 1e-6);
    CHECK(velocity[1] == 0 && velocity[2] == 0);
    const RigidPose worldGrip{FromEuler(25, 77, -15), {2, 1.4, -3}};
    const RigidPose worldAim{FromEuler(-12, 93, 8), {2.1, 1.5, -3.2}};
    for (int hand = 0; hand < 2; ++hand) {
        for (bool native : {false, true}) {
            const auto frame = GripToDevice(hand, native);
            const auto raw = Compose(worldGrip, frame);
            CHECK(Near(Compose(raw, Invert(frame)), worldGrip));
            const auto offset = AimInDevice(Bridge(worldGrip), Bridge(worldAim), frame);
            CHECK(Near(Compose(raw, offset), worldAim));
        }
        vr::VRBoneTransform_t before[rcvr::kHandBones]{}, after[rcvr::kHandBones];
        for (int i = 0; i < rcvr::kHandBones; ++i) {
            before[i].orientation = {1, 0, 0, 0};
            before[i].position = {{float(i) * 0.001f, 0.02f, -0.03f, 1}};
        }
        RebaseControllerBones(before, after, hand);
        const auto pose = [](const vr::VRBoneTransform_t& b) {
            return RigidPose{{b.orientation.w, b.orientation.x, b.orientation.y, b.orientation.z},
                             {b.position.v[0], b.position.v[1], b.position.v[2]}};
        };
        for (int root : {1, 26, 27, 28, 29, 30})
            CHECK(Near(Compose(GripToDevice(hand, false), pose(before[root])), Compose(GripToDevice(hand, true), pose(after[root]))));
        for (int i = 2; i < 26; ++i) CHECK(std::memcmp(&before[i], &after[i], sizeof(before[i])) == 0);
    }
    const RigidPose source{FromEuler(31, 47, -12), {0.125, -0.25, 0.5}};
    const auto local = ModelComponent(source)["component_local"];
    const auto angles = local["rotate_xyz"];
    const auto q = Mul(Mul(FromEuler(0, 0, angles[2].get<double>()), FromEuler(0, angles[1].get<double>(), 0)),
                       FromEuler(angles[0].get<double>(), 0, 0));
    CHECK(Near(source, {q, {0.125, -0.25, 0.5}}));
    CHECK(ModelComponent({{1, 0, 0, 0}, {-0.0, 0, 0}}).dump() == ModelComponent({{1, 0, 0, 0}, {0, 0, 0}}).dump());
}

static void TestConfigSnapshots() {
    rcvr::Shared shared{};
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (uint32_t n = 1; n <= 10000; ++n) {
            rcvr::DisplayConfig config{};
            config.eyeWidth = config.eyeHeight = n;
            const auto value = std::to_string(n);
            for (auto& profile : config.controllerProfiles) strcpy_s(profile, value.c_str());
            rcvr::WriteDisplayConfig(&shared, config);
        }
        done = true;
    });
    while (!done) {
        rcvr::DisplayConfig config{};
        if (!rcvr::ReadDisplayConfig(&shared, config)) continue;
        CHECK(config.eyeWidth == config.eyeHeight);
        CHECK(std::strcmp(config.controllerProfiles[0], config.controllerProfiles[1]) == 0);
        if (config.eyeWidth) CHECK(std::to_string(config.eyeWidth) == config.controllerProfiles[0]);
    }
    writer.join();
}

static void TestModelCache() {
    const auto root = fs::absolute("controller-test-data-" + std::to_string(GetCurrentProcessId()));
    if (!fs::create_directory(root)) throw std::runtime_error("Test directory already exists");
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(path, e);
        }
    } cleanup{root};
    const auto source = root / "source";
    fs::create_directory(source);
    const nlohmann::json original = {{"components", {{"body", {{"filename", "body.obj"}}}}}};
    {
        std::ofstream(source / "pico.json") << original.dump();
        std::ofstream(source / "body.obj") << "fixture-mesh";
    }
    rcvr::BridgeController controller{};
    controller.flags = 3;
    controller.aimValid = 1;
    controller.pose = Bridge({FromEuler(11, 27, -4), {2, 1, -3}});
    controller.aim = Bridge(Compose(BridgeRigidPose(controller.pose), {FromEuler(-34, 0, 0), {0, -0.03, -0.04}}));
    const auto cache = root / "cache";
    const auto name = CacheNativeModel(source / "pico.json", cache, 0, controller);
    const auto model = cache / name / (name + ".json");
    CHECK(nlohmann::json::parse(ReadModelFile(source / "pico.json")) == original);
    CHECK(nlohmann::json::parse(ReadModelFile(model))["components"]["body"] == original["components"]["body"]);
    CHECK(CacheNativeModel(source / "pico.json", cache, 0, controller) == name);
    {
        std::ofstream(cache / name / "body.obj") << "corrupt";
    }
    CHECK(CacheNativeModel(source / "pico.json", cache, 0, controller) == name);
    CHECK(ReadModelFile(cache / name / "body.obj") == "fixture-mesh");
    std::string results[4];
    std::thread workers[4];
    for (int i = 0; i < 4; ++i)
        workers[i] = std::thread([&, i] { results[i] = CacheNativeModel(source / "pico.json", cache, 0, controller); });
    for (auto& thread : workers) thread.join();
    for (const auto& result : results) CHECK(result == name);
    for (const auto& entry : fs::directory_iterator(cache)) CHECK(entry.path().filename().string().find(".building-") != 0);
    controller.aim.orientation[0] = std::numeric_limits<float>::quiet_NaN();
    bool rejected = false;
    try {
        CacheNativeModel(source / "pico.json", cache, 0, controller);
    } catch (const std::exception&) {
        rejected = true;
    }
    CHECK(rejected);
}

int main() {
    try {
        TestProfilesAndRegistration();
        TestFramesAndBones();
        TestConfigSnapshots();
        TestModelCache();
    } catch (const std::exception& error) {
        std::printf("FAIL: %s\n", error.what());
        ++failures;
    }
    std::puts(failures ? "CONTROLLER TESTS FAILED" : "ALL CONTROLLER TESTS PASSED");
    return failures ? 1 : 0;
}
