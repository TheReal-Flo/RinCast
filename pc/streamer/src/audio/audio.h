// Audio: PC audio -> headset (WASAPI loopback + Opus) and headset mic -> PC
// (Opus -> a virtual microphone's render side, which its driver loops back
// into the matching capture endpoint).
//
// Two virtual device sets are supported:
//   RinCast: "RinCast Speaker", "RinCast Microphone" (+ internal "RinCast Microphone Feed")
//   Steam:   "Steam Streaming Speakers", "Steam Streaming Microphone" (render and capture
//            endpoints share the device name; apps record from the capture side)
#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "rc/audio_codec.h"
#include "rc/connection.h"

namespace rcs {

namespace audio {

enum class DeviceSet { RinCast, Steam };

struct DeviceNames {
    const wchar_t* speaker;   // render endpoint captured with loopback
    const wchar_t* mic;       // capture endpoint apps record from
    const wchar_t* micFeed;   // render endpoint we write the headset mic into
};
const DeviceNames& Names(DeviceSet set);

struct DriverStatus {
    bool speaker = false;
    bool microphone = false;
    bool microphoneFeed = false;
};
DriverStatus QueryDriver(DeviceSet set = DeviceSet::RinCast);

// Endpoints chosen for a session. Null members fall back to the Windows
// default speaker / no microphone.
struct Route {
    const wchar_t* speaker = nullptr;
    const wchar_t* mic = nullptr;
    const wchar_t* micFeed = nullptr;
    std::string speakerSet, micSet;  // "rincast" | "steam" | "" (for the UI)
};
// preference: "auto" (RinCast, then Steam), "rincast", "steam" or "system".
Route ResolveRoute(const std::string& preference);

// Makes the route's endpoints the Windows defaults (remembering the previous
// ones, also on disk) and restores them later. RestoreDefaults at start-up puts
// back what a killed streamer couldn't.
void SwitchDefaults(const Route& route, bool speaker, bool microphone);
void RestoreDefaults();

}  // namespace audio

class AudioStreamer {
   public:
    ~AudioStreamer() { Stop(); }
    // speakerName: render endpoint to capture; null follows the Windows default.
    bool Start(rc::Connection* connection, const wchar_t* speakerName, int bitrate);
    void Stop();

   private:
    void Run();
    rc::Connection* m_connection = nullptr;
    std::wstring m_speakerName;
    int m_bitrate = 128000;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
};

class MicPlayer {
   public:
    ~MicPlayer() { Stop(); }
    // feedName: render endpoint of the virtual microphone to write into; micName: its
    // capture endpoint (apps record from it), whose sample rate the feed must match.
    bool Start(const wchar_t* feedName, const wchar_t* micName);
    void Stop();
    void PushPacket(const uint8_t* payload, size_t size);  // AudioPacketHeader + opus
    bool Available() const { return m_available; }

   private:
    void Run();
    std::wstring m_feedName, m_micName;
    rc::AudioDecoder m_decoder;
    rc::JitterBuffer m_jitter;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_available{false};
    uint32_t m_lastSeq = 0;
};

}  // namespace rcs
