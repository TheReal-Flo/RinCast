// LAN discovery. The headset broadcasts a Probe on kDiscoveryPort; every
// streamer replies with a unicast Announce (and also broadcasts an Announce
// every few seconds so passive listeners stay current).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rc/protocol.h"

namespace rc {

struct DiscoveryProbe {
    std::string deviceId;
    std::string deviceName;
    uint32_t nonce = 0;
};

struct DiscoveryAnnounce {
    std::string serverId;     // stable id of the PC
    std::string name;         // PC display name
    std::string version;      // streamer version
    std::string os;
    std::string gpu;
    uint16_t streamPort = kStreamPort;
    bool accountLinked = false;
    std::string accountName;  // shown in the headset when linked
    bool busy = false;        // another headset is connected
    uint32_t nonce = 0;       // echoes the probe
};

std::vector<uint8_t> BuildProbe(const DiscoveryProbe& probe);
std::vector<uint8_t> BuildAnnounce(const DiscoveryAnnounce& announce);

// Returns the discovery type, or 0 when the datagram is not a discovery packet.
uint8_t ParseDiscoveryType(const uint8_t* data, size_t size);
bool ParseProbe(const uint8_t* data, size_t size, DiscoveryProbe& out);
bool ParseAnnounce(const uint8_t* data, size_t size, DiscoveryAnnounce& out);

}  // namespace rc
