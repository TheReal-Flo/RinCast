#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../capture/mac_monitors.h"
#include "../stream/mac_desktop_stream.h"
#include "nlohmann/json.hpp"
#include "rc/connection.h"
#include "rc/protocol.h"

namespace rcm {

using json = nlohmann::json;

struct MacHelloInfo {
    std::string deviceId;
    std::string deviceName;
    std::string model;
    std::string appVersion;
    rc::NetAddress address;
    json raw;
};

class MacClientSession : public std::enable_shared_from_this<MacClientSession> {
   public:
    MacClientSession(std::unique_ptr<rc::Connection> connection, const MacHelloInfo& hello);
    ~MacClientSession();

    void Start();
    void Close(const std::string& reason);

    void HandleDatagram(const uint8_t* data, size_t size, const rc::NetAddress& from);
    void Tick(int64_t nowNs);

    const std::string& DeviceName() const { return m_hello.deviceName; }
    const std::string& DeviceId() const { return m_hello.deviceId; }

   private:
    void OnControl(const std::string& text);
    void OnPacket(rc::PacketType type, const uint8_t* payload, size_t size);
    void SendControl(const json& message);

    void SendState();
    void SendMonitors();
    void StartDesktop(const json& m);
    void StopDesktop();

    std::unique_ptr<rc::Connection> m_connection;
    MacHelloInfo m_hello;
    std::atomic<bool> m_closed{false};
    std::atomic<bool> m_inputAllowed{true};

    std::mutex m_mutex;
    std::vector<MacMonitorInfo> m_monitors;
    std::map<uint8_t, std::unique_ptr<MacDesktopStream>> m_desktopStreams;
    rc::VideoCodec m_codec = rc::VideoCodec::HEVC;
};

}  // namespace rcm
