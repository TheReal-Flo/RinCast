#include "rc/discovery.h"

#include <cstring>

#include "nlohmann/json.hpp"

namespace rc {

using nlohmann::json;

static std::vector<uint8_t> Build(DiscoveryType type, const std::string& body) {
    DiscoveryHeader h{kDiscoveryMagic, kProtocolVersion, uint8_t(type), 0};
    std::vector<uint8_t> out(sizeof(h) + body.size());
    std::memcpy(out.data(), &h, sizeof(h));
    std::memcpy(out.data() + sizeof(h), body.data(), body.size());
    return out;
}

std::vector<uint8_t> BuildProbe(const DiscoveryProbe& probe) {
    json j{{"deviceId", probe.deviceId}, {"deviceName", probe.deviceName}, {"nonce", probe.nonce}};
    return Build(DiscoveryType::Probe, j.dump());
}

std::vector<uint8_t> BuildAnnounce(const DiscoveryAnnounce& a) {
    json j{{"serverId", a.serverId},   {"name", a.name},
           {"version", a.version},     {"os", a.os},
           {"gpu", a.gpu},             {"port", a.streamPort},
           {"accountLinked", a.accountLinked}, {"accountName", a.accountName},
           {"busy", a.busy},           {"nonce", a.nonce}};
    return Build(DiscoveryType::Announce, j.dump());
}

uint8_t ParseDiscoveryType(const uint8_t* data, size_t size) {
    if (size < sizeof(DiscoveryHeader)) return 0;
    DiscoveryHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kDiscoveryMagic || h.protocolVersion != kProtocolVersion) return 0;
    return h.type;
}

static bool Body(const uint8_t* data, size_t size, json& out) {
    try {
        out = json::parse(reinterpret_cast<const char*>(data) + sizeof(DiscoveryHeader),
                          reinterpret_cast<const char*>(data) + size);
        return out.is_object();
    } catch (...) {
        return false;
    }
}

bool ParseProbe(const uint8_t* data, size_t size, DiscoveryProbe& out) {
    if (ParseDiscoveryType(data, size) != uint8_t(DiscoveryType::Probe)) return false;
    json j;
    if (!Body(data, size, j)) return false;
    out.deviceId = j.value("deviceId", "");
    out.deviceName = j.value("deviceName", "");
    out.nonce = j.value("nonce", 0u);
    return true;
}

bool ParseAnnounce(const uint8_t* data, size_t size, DiscoveryAnnounce& out) {
    if (ParseDiscoveryType(data, size) != uint8_t(DiscoveryType::Announce)) return false;
    json j;
    if (!Body(data, size, j)) return false;
    out.serverId = j.value("serverId", "");
    out.name = j.value("name", "");
    out.version = j.value("version", "");
    out.os = j.value("os", "");
    out.gpu = j.value("gpu", "");
    out.streamPort = j.value("port", uint16_t(kStreamPort));
    out.accountLinked = j.value("accountLinked", false);
    out.accountName = j.value("accountName", "");
    out.busy = j.value("busy", false);
    out.nonce = j.value("nonce", 0u);
    return !out.serverId.empty();
}

}  // namespace rc
