// One connected headset: control protocol, desktop & VR video streams,
// input injection and audio in both directions.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../capture/monitors.h"
#include "../net/stream_server.h"
#include "../util.h"
#include "rc/connection.h"
#include "rc/protocol.h"

namespace rcs {

// Defined in settings.cpp: PICO's model codes ("A8110") read as product names ("PICO 4").
std::string HeadsetName(const std::string& model);

class DesktopStream;
class VrStream;
class AudioStreamer;
class MicPlayer;

struct ClientCaps {
    std::string controllerProfiles[2];
    bool h264 = true, hevc = true, hevc10 = false, av1 = false, av110 = false;
    uint32_t maxDecodeWidth = 4096, maxDecodeHeight = 4096;
    std::vector<float> refreshRates{72, 90};
    uint32_t recommendedEyeWidth = 1832, recommendedEyeHeight = 1920;  // 100% of the resolution: the panel (the runtime's recommended size before 1.2.0)
    float fovTan[2][4] = {{1.0f, 0.9f, 0.95f, 1.1f}, {0.9f, 1.0f, 0.95f, 1.1f}};
    float ipd = 0.063f;
};

struct ClientStreamSettings {
    std::string codec = "auto";
    bool tenBit = false;
    bool frameGeneration = false;
    std::string vrQuality = "high";   // low | medium | high | ultra | godlike (headsets before 1.1.0)
    float vrScale = 0;                // pixels per eye against the headset's own; 0 = from vrQuality
    bool matchSteamVr = false;        // stream at the size SteamVR renders at
    bool gazeFoveation = false;       // foveated encoding around where the eyes look
    bool eyesTracked = true;          // false: the headset can't track them; fixed at the lens centre
    float refreshRate = 90;
    int vrBitrateMbps = 0;            // 0 = PC default
    int desktopBitrateMbps = 0;
    int desktopFps = 0;
    bool adaptiveBitrate = true;
    bool streamAudio = true;
    bool micPassthrough = true;
};

class ClientSession : public std::enable_shared_from_this<ClientSession> {
   public:
    ClientSession(std::unique_ptr<rc::Connection> connection, const HelloInfo& hello, std::string clientPkHex,
                  std::vector<uint8_t> welcome, bool remote, bool vpn);
    ~ClientSession();

    void Start();
    void Close(const std::string& reason);

    void HandleDatagram(const uint8_t* data, size_t size, const rc::NetAddress& from);
    void Tick(int64_t nowNs);

    const std::string& DeviceId() const { return m_hello.deviceId; }
    const std::string& DeviceKey() const { return m_hello.deviceKey; }
    const std::string& DeviceName() const { return m_hello.deviceName; }
    const std::string& Model() const { return m_hello.model; }
    // "PICO 4" instead of "A8110"; the device's own name when nothing better is known.
    std::string DisplayName() const {
        const std::string name = HeadsetName(m_hello.model);
        return name.empty() ? m_hello.deviceName : name;
    }
    const std::string& ClientPublicKeyHex() const { return m_clientPk; }
    const std::vector<uint8_t>& WelcomePacket() const { return m_welcome; }
    int64_t LastReceiveNs() const { return m_connection->LastReceiveNs(); }
    bool PeerDisconnected() const { return m_connection->PeerDisconnected(); }
    bool IsRemote() const { return m_remote; }
    rc::Connection* Connection() { return m_connection.get(); }

    void SendControl(const json& message);
    json StatusJson();

    // Called by the VR stream when the producer state changes.
    void NotifyVrState(const std::string& state, const std::string& producer, const std::string& app,
                       const std::string& error = {});
    void OnSettingsChanged();  // PC-side settings changed in the UI
    void OnMonitorsChanged();

    rc::VideoCodec ChooseCodec(bool forVr) const;
    ClientCaps Caps();
    ClientStreamSettings StreamSettings();

   private:
    void OnControl(const std::string& text);
    void OnPacket(rc::PacketType type, const uint8_t* payload, size_t size);
    void WorkerLoop();
    void HandleMessage(const json& m);

    void SendServerInfo();
    void SendMonitors();
    void StartDesktop(const json& m);
    void StopDesktop();
    void StartVr();
    void StopVr();
    void RestartVr();
    void LaunchSteamVr();
    void UpdateAudio();
    void ApplyClientInfo(const json& m);
    void MeasurePoseWait(int64_t receivedNs);

    std::unique_ptr<rc::Connection> m_connection;
    // Network thread only: how long each tracking packet waited for SteamVR to take a pose,
    // reported to the headset once a second (rc::msg::kVrPoseWait).
    std::vector<int64_t> m_poseWaits;
    int64_t m_poseWaitPeriodNs = 0, m_poseWaitSentNs = 0;
    HelloInfo m_hello;
    std::string m_clientPk;
    std::vector<uint8_t> m_welcome;
    bool m_remote;    // through the account, with its proof
    bool m_vpn;       // no account, but through a tunnel
    bool m_slowPath;  // either way: smaller packets, a careful bitrate
    std::atomic<bool> m_closed{false};
    // "Let headsets use the mouse and keyboard": checked on every input event, so not a
    // settings snapshot each time.
    std::atomic<bool> m_inputAllowed{true};
    void ApplyInputSetting(bool announce);

    // Control work happens off the network thread.
    std::thread m_worker;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::deque<json> m_queue;

    std::mutex m_mutex;  // protects everything below
    ClientCaps m_caps;
    ClientStreamSettings m_settings;
    std::vector<MonitorInfo> m_monitors;
    std::map<uint8_t, std::unique_ptr<DesktopStream>> m_desktop;  // by stream id
    std::unique_ptr<VrStream> m_vr;
    std::unique_ptr<AudioStreamer> m_audio;
    std::unique_ptr<MicPlayer> m_mic;
    std::string m_vrState = "idle";
    std::string m_vrProducer;
    std::string m_vrApp;
    std::atomic<int64_t> m_steamVrLaunchNs = 0;  // written from VR threads, read on the worker
    // Matching SteamVR's resolution: the size the VR stream was started for, a new one
    // SteamVR has rendered at since, for how many ticks (s) it held, and when the stream
    // last restarted to follow one.
    uint32_t m_vrMatchedWidth = 0, m_vrMatchedHeight = 0;
    uint32_t m_vrSeenWidth = 0, m_vrSeenHeight = 0;
    int m_vrSeenTicks = 0;
    int64_t m_vrFollowedNs = 0;
    void FollowSteamVrResolution();
    int64_t m_connectedAtNs = 0;
    json m_clientStats;
    bool m_shotActive = false;
    std::string m_shotName;
    // Written as the chunks arrive, not kept in memory: a screenshot can be a few megabytes,
    // and a dropped session must not leave half a picture behind under the real name.
    std::ofstream m_shotFile;
    std::string m_shotTemp;
    uint64_t m_shotCap = 0, m_shotWritten = 0;
    void ResetShot();
    uint32_t m_lastMouseMonitor = 0;
};

}  // namespace rcs
