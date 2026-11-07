#include "audio.h"

#include <audioclient.h>
#include <avrt.h>
#include <endpointvolume.h>
#include <initguid.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "../util.h"
#include "rc/platform.h"
#include "rc/protocol.h"

#pragma comment(lib, "ole32.lib")

namespace rcs {

namespace {

// Undocumented but stable (Windows 7+) interface used by the Sound control
// panel to change default endpoints.
MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8")
IPolicyConfig : public IUnknown {
   public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR deviceId, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
const CLSID CLSID_PolicyConfigClient = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

ComPtr<IMMDeviceEnumerator> Enumerator() {
    ComPtr<IMMDeviceEnumerator> e;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e));
    return e;
}

std::wstring FriendlyName(IMMDevice* device) {
    ComPtr<IPropertyStore> props;
    std::wstring name;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) name = v.pwszVal;
        PropVariantClear(&v);
    }
    return name;
}

// Endpoint friendly names look like "Speakers (RinCast Speaker)"; match the
// device description part.
ComPtr<IMMDevice> FindEndpoint(EDataFlow flow, const wchar_t* name) {
    auto e = Enumerator();
    if (!e) return nullptr;
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) return nullptr;
    UINT count = 0;
    col->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
        ComPtr<IMMDevice> d;
        col->Item(i, &d);
        // Friendly names look like "RinCast Speaker (RinCast Virtual Audio)". Match the
        // endpoint name exactly so "RinCast Microphone" never matches "... Feed".
        const std::wstring fn = FriendlyName(d.Get());
        const std::wstring needle(name);
        if (fn == needle || fn.rfind(needle + L" (", 0) == 0 || fn.find(L"(" + needle + L")") != std::wstring::npos)
            return d;
    }
    return nullptr;
}

std::wstring DeviceId(IMMDevice* d) {
    LPWSTR id = nullptr;
    std::wstring out;
    if (d && SUCCEEDED(d->GetId(&id))) {
        out = id;
        CoTaskMemFree(id);
    }
    return out;
}

// The Windows volume and mute of an endpoint, as a gain for the samples. RinCast's driver
// reports its volume controls as hardware that it never applies, and loopback records a
// speaker before its volume anyway: without this the sliders and volume keys do nothing.
class EndpointVolume {
   public:
    void Open(IMMDevice* device) {
        m_volume.Reset();
        m_nextRead = 0;
        m_target = m_gain = 1.f;
        if (device) device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &m_volume);
    }
    // Per sample; follows changes over ~20 ms so they don't click.
    float Next() {
        m_gain += (m_target - m_gain) * 0.002f;
        return m_gain;
    }
    // Reads the setting at most 10 times a second.
    void Update() {
        const int64_t now = rc::NowNs();
        if (!m_volume || now < m_nextRead) return;
        m_nextRead = now + 100'000'000;
        BOOL mute = FALSE;
        float scalar = 1, db = 0;
        if (FAILED(m_volume->GetMute(&mute)) || FAILED(m_volume->GetMasterVolumeLevelScalar(&scalar)) ||
            FAILED(m_volume->GetMasterVolumeLevel(&db)))
            return;
        m_target = mute || scalar <= 0.f ? 0.f : std::min(1.f, std::pow(10.f, db / 20.f));
    }

   private:
    ComPtr<IAudioEndpointVolume> m_volume;
    int64_t m_nextRead = 0;
    float m_target = 1.f, m_gain = 1.f;
};

std::mutex g_defaultsMutex;
std::wstring g_prevRender, g_prevCapture;

void SetDefault(const std::wstring& id) {
    ComPtr<IPolicyConfig> policy;
    if (FAILED(CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig),
                                reinterpret_cast<void**>(policy.GetAddressOf()))))
        return;
    policy->SetDefaultEndpoint(id.c_str(), eConsole);
    policy->SetDefaultEndpoint(id.c_str(), eMultimedia);
    policy->SetDefaultEndpoint(id.c_str(), eCommunications);
}

// Converts any WASAPI mix format frame to stereo float.
struct FormatReader {
    WORD channels = 2;
    bool isFloat = true;
    WORD bits = 32;
    void Init(const WAVEFORMATEX* f) {
        channels = f->nChannels;
        bits = f->wBitsPerSample;
        isFloat = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
            isFloat = ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        }
    }
    float Sample(const uint8_t* frame, int ch) const {
        if (ch >= channels) return 0;
        if (isFloat) return reinterpret_cast<const float*>(frame)[ch];
        if (bits == 16) return reinterpret_cast<const int16_t*>(frame)[ch] / 32768.f;
        if (bits == 32) return float(reinterpret_cast<const int32_t*>(frame)[ch] / 2147483648.0);
        if (bits == 24) {
            const uint8_t* p = frame + ch * 3;
            const int32_t v = (int32_t(p[0]) << 8 | int32_t(p[1]) << 16 | int32_t(p[2]) << 24) >> 8;
            return v / 8388608.f;
        }
        return 0;
    }
    void Stereo(const uint8_t* frame, float& l, float& r) const {
        if (channels == 1) {
            l = r = Sample(frame, 0);
            return;
        }
        l = Sample(frame, 0);
        r = Sample(frame, 1);
        if (channels >= 6) {  // 5.1/7.1: fold centre and surrounds
            const float c = Sample(frame, 2) * 0.707f;
            l += c + Sample(frame, 4) * 0.5f;
            r += c + Sample(frame, 5) * 0.5f;
        }
    }
};

inline int16_t ToS16(float v) {
    v = std::fmax(-1.f, std::fmin(1.f, v));
    return int16_t(std::lround(v * 32767.f));
}

// Virtual microphones loop their render side into their capture side as raw bytes,
// without converting. Steam's ships playing stereo 48 kHz and recording mono 44.1 kHz,
// so the voice came out at less than half its pitch with 400+ ms of growing delay
// (measured with audio-check --mic-test steam). Give both sides the same default format,
// as the Sound control panel would: then the audio passes through unchanged.
void MatchMicFormats(const std::wstring& feedName, const std::wstring& micName) {
    auto feed = FindEndpoint(eRender, feedName.c_str());
    auto mic = micName.empty() ? nullptr : FindEndpoint(eCapture, micName.c_str());
    if (!feed || !mic) return;
    auto mixOf = [](IMMDevice* d, UINT& rate, UINT& ch) {
        ComPtr<IAudioClient> c;
        WAVEFORMATEX* f = nullptr;
        if (FAILED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &c)) || FAILED(c->GetMixFormat(&f))) return false;
        rate = f->nSamplesPerSec;
        ch = f->nChannels;
        CoTaskMemFree(f);
        return true;
    };
    UINT fr = 0, fc = 0, mr = 0, mc = 0;
    if (!mixOf(feed.Get(), fr, fc) || !mixOf(mic.Get(), mr, mc) || (fr == mr && fc == mc)) return;
    ComPtr<IPolicyConfig> policy;
    if (FAILED(CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig),
                                reinterpret_cast<void**>(policy.GetAddressOf()))))
        return;
    auto make = [](WORD bits, bool isFloat) {
        WAVEFORMATEXTENSIBLE f{};
        f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        f.Format.nChannels = 2;
        f.Format.nSamplesPerSec = 48000;
        f.Format.wBitsPerSample = bits;
        f.Format.nBlockAlign = WORD(2 * bits / 8);
        f.Format.nAvgBytesPerSec = 48000 * f.Format.nBlockAlign;
        f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        f.Samples.wValidBitsPerSample = bits;
        f.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        f.SubFormat = isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
        return f;
    };
    WAVEFORMATEXTENSIBLE device = make(16, false), mix = make(32, true);
    const HRESULT a = policy->SetDeviceFormat(DeviceId(feed.Get()).c_str(), &device.Format, &mix.Format);
    const HRESULT b = policy->SetDeviceFormat(DeviceId(mic.Get()).c_str(), &device.Format, &mix.Format);
    if (SUCCEEDED(a) && SUCCEEDED(b))
        RC_LOGI("Microphone: %ls played %u Hz / %u ch but recorded %u Hz / %u ch; set both to 48 kHz stereo",
                micName.c_str(), fr, fc, mr, mc);
    else
        RC_LOGW("Microphone: %ls plays %u Hz / %u ch but records %u Hz / %u ch and its format couldn't be changed "
                "(0x%08lx, 0x%08lx); voices will sound wrong. Set both to the same format in the Sound control panel.",
                micName.c_str(), fr, fc, mr, mc, unsigned long(a), unsigned long(b));
}

}  // namespace

namespace audio {

const DeviceNames& Names(DeviceSet set) {
    static const DeviceNames rincast{L"RinCast Speaker", L"RinCast Microphone", L"RinCast Microphone Feed"};
    // Steam's microphone driver exposes a render and a capture endpoint with the same name.
    static const DeviceNames steam{L"Steam Streaming Speakers", L"Steam Streaming Microphone",
                                   L"Steam Streaming Microphone"};
    return set == DeviceSet::Steam ? steam : rincast;
}

DriverStatus QueryDriver(DeviceSet set) {
    ScopedCoInit com;
    const DeviceNames& n = Names(set);
    DriverStatus s;
    s.speaker = FindEndpoint(eRender, n.speaker) != nullptr;
    s.microphone = FindEndpoint(eCapture, n.mic) != nullptr;
    s.microphoneFeed = FindEndpoint(eRender, n.micFeed) != nullptr;
    return s;
}

Route ResolveRoute(const std::string& preference) {
    std::vector<DeviceSet> order;
    if (preference == "rincast") order = {DeviceSet::RinCast};
    else if (preference == "steam") order = {DeviceSet::Steam};
    else if (preference != "system") order = {DeviceSet::RinCast, DeviceSet::Steam};  // auto
    Route r;
    for (DeviceSet set : order) {
        const DriverStatus st = QueryDriver(set);
        const DeviceNames& n = Names(set);
        const char* key = set == DeviceSet::Steam ? "steam" : "rincast";
        if (!r.speaker && st.speaker) {
            r.speaker = n.speaker;
            r.speakerSet = key;
        }
        if (!r.micFeed && st.microphone && st.microphoneFeed) {
            r.mic = n.mic;
            r.micFeed = n.micFeed;
            r.micSet = key;
        }
    }
    return r;
}

void SwitchDefaults(const Route& route, bool speaker, bool microphone) {
    ScopedCoInit com;
    std::lock_guard lock(g_defaultsMutex);
    auto e = Enumerator();
    if (!e) return;
    if (speaker && route.speaker) {
        if (auto rc = FindEndpoint(eRender, route.speaker)) {
            ComPtr<IMMDevice> current;
            if (SUCCEEDED(e->GetDefaultAudioEndpoint(eRender, eConsole, &current))) {
                const auto id = DeviceId(current.Get());
                if (id != DeviceId(rc.Get()) && g_prevRender.empty()) g_prevRender = id;
            }
            SetDefault(DeviceId(rc.Get()));
        }
    }
    if (microphone && route.mic) {
        if (auto rc = FindEndpoint(eCapture, route.mic)) {
            ComPtr<IMMDevice> current;
            if (SUCCEEDED(e->GetDefaultAudioEndpoint(eCapture, eConsole, &current))) {
                const auto id = DeviceId(current.Get());
                if (id != DeviceId(rc.Get()) && g_prevCapture.empty()) g_prevCapture = id;
            }
            SetDefault(DeviceId(rc.Get()));
        }
    }
}

void RestoreDefaults() {
    ScopedCoInit com;
    std::lock_guard lock(g_defaultsMutex);
    if (!g_prevRender.empty()) SetDefault(g_prevRender);
    if (!g_prevCapture.empty()) SetDefault(g_prevCapture);
    g_prevRender.clear();
    g_prevCapture.clear();
}

}  // namespace audio

// ---------------------------------------------------------------------------
// PC audio -> headset
// ---------------------------------------------------------------------------

bool AudioStreamer::Start(rc::Connection* connection, const wchar_t* speakerName, int bitrate) {
    Stop();
    m_connection = connection;
    m_speakerName = speakerName ? speakerName : L"";
    m_bitrate = bitrate;
    m_running = true;
    m_thread = std::thread([this] { Run(); });
    return true;
}

void AudioStreamer::Stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
}

void AudioStreamer::Run() {
    ScopedCoInit com;
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    rc::AudioEncoder encoder;
    if (!encoder.Init(rc::kAudioSampleRate, 2, m_bitrate, true)) return;
    constexpr int kFrame = 480;  // 10 ms
    std::vector<int16_t> pcm;
    pcm.reserve(kFrame * 2 * 4);
    uint8_t packet[1500];
    uint32_t seq = 0;

    while (m_running) {
        ComPtr<IMMDevice> device;
        if (!m_speakerName.empty()) device = FindEndpoint(eRender, m_speakerName.c_str());
        if (!device) {
            if (auto e = Enumerator()) e->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        }
        if (!device) {
            rc::SleepMs(1000);
            continue;
        }
        const std::wstring deviceId = DeviceId(device.Get());
        ComPtr<IAudioClient> client;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
            rc::SleepMs(1000);
            continue;
        }
        WAVEFORMATEX* mix = nullptr;
        client->GetMixFormat(&mix);
        FormatReader reader;
        reader.Init(mix);
        const double ratio = double(mix->nSamplesPerSec) / rc::kAudioSampleRate;
        const UINT blockAlign = mix->nBlockAlign;
        HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 200000, 0, mix, nullptr);
        CoTaskMemFree(mix);
        ComPtr<IAudioCaptureClient> capture;
        if (FAILED(hr) || FAILED(client->GetService(IID_PPV_ARGS(&capture)))) {
            RC_LOGW("Audio loopback init failed: %s", HResultString(hr).c_str());
            rc::SleepMs(1000);
            continue;
        }
        client->Start();
        RC_LOGI("Streaming audio from %s", Narrow(FriendlyName(device.Get())).c_str());
        EndpointVolume volume;
        volume.Open(device.Get());
        double resamplePos = 0;
        float lastL = 0, lastR = 0;
        int64_t lastDeviceCheck = rc::NowNs();

        while (m_running) {
            rc::SleepMs(3);
            // Follow default-device changes when not pinned to RinCast Speaker.
            if (m_speakerName.empty() && rc::NowNs() - lastDeviceCheck > 2'000'000'000LL) {
                lastDeviceCheck = rc::NowNs();
                ComPtr<IMMDevice> def;
                if (auto e = Enumerator(); e && SUCCEEDED(e->GetDefaultAudioEndpoint(eRender, eConsole, &def)) &&
                                             DeviceId(def.Get()) != deviceId)
                    break;
            }
            UINT32 packetFrames = 0;
            if (FAILED(capture->GetNextPacketSize(&packetFrames))) break;  // device lost
            while (packetFrames > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                volume.Update();
                // Linear resampling to 48 kHz.
                for (UINT32 i = 0; i < frames; i++) {
                    float l = 0, r = 0;
                    if (!silent) reader.Stereo(data + size_t(i) * blockAlign, l, r);
                    const float g = volume.Next();
                    l *= g;
                    r *= g;
                    while (resamplePos < 1.0) {
                        pcm.push_back(ToS16(lastL + (l - lastL) * float(resamplePos)));
                        pcm.push_back(ToS16(lastR + (r - lastR) * float(resamplePos)));
                        resamplePos += ratio;
                    }
                    resamplePos -= 1.0;
                    lastL = l;
                    lastR = r;
                }
                capture->ReleaseBuffer(frames);
                capture->GetNextPacketSize(&packetFrames);
            }
            while (pcm.size() >= size_t(kFrame) * 2) {
                rc::AudioPacketHeader h{};
                h.stream = uint8_t(rc::AudioStream::PcToHeadset);
                h.channels = 2;
                h.frameSamples = kFrame;
                h.sequence = ++seq;
                h.timestampNs = uint64_t(rc::NowNs());
                const int n = encoder.Encode(pcm.data(), kFrame, packet, sizeof(packet) - 64);
                if (n > 0) m_connection->Send2(rc::PacketType::Audio, &h, sizeof(h), packet, size_t(n));
                pcm.erase(pcm.begin(), pcm.begin() + kFrame * 2);
            }
        }
        client->Stop();
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

// ---------------------------------------------------------------------------
// Headset mic -> RinCast Microphone
// ---------------------------------------------------------------------------

bool MicPlayer::Start(const wchar_t* feedName, const wchar_t* micName) {
    Stop();
    if (!feedName) return false;
    m_feedName = feedName;
    m_micName = micName ? micName : L"";
    if (!m_decoder.Init(rc::kAudioSampleRate, 1)) return false;
    m_jitter.Configure(1, rc::kAudioSampleRate, 40, 400);  // grows up to 200 ms on a jittery link
    m_lastSeq = 0;
    m_running = true;
    m_thread = std::thread([this] { Run(); });
    return true;
}

void MicPlayer::Stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
}

void MicPlayer::PushPacket(const uint8_t* payload, size_t size) {
    if (size < sizeof(rc::AudioPacketHeader) || !m_running) return;
    rc::AudioPacketHeader h;
    std::memcpy(&h, payload, sizeof(h));
    // A packet that arrives after a newer one is too late: its slot was already
    // concealed or skipped, and playing it now would scramble the speech.
    // (A big jump back means the headset restarted its microphone: start over.)
    const int32_t ahead = int32_t(h.sequence - m_lastSeq);
    if (m_lastSeq && ahead <= 0 && ahead > -500) return;
    if (ahead <= 0) m_lastSeq = 0;
    int16_t pcm[5760];
    // Conceal up to 5 lost packets (50 ms) with Opus PLC; after longer gaps just carry on.
    const uint32_t missing = m_lastSeq ? h.sequence - m_lastSeq - 1 : 0;
    for (uint32_t i = 0; i < missing && i < 5; i++) {
        const int n = m_decoder.Decode(nullptr, 0, pcm, h.frameSamples);
        if (n > 0) m_jitter.Push(pcm, n);
    }
    m_lastSeq = h.sequence;
    const int n = m_decoder.Decode(payload + sizeof(h), int(size - sizeof(h)), pcm, 5760);
    if (n > 0) m_jitter.Push(pcm, n);
}

void MicPlayer::Run() {
    ScopedCoInit com;
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    while (m_running) {
        auto device = FindEndpoint(eRender, m_feedName.c_str());
        if (!device) {
            m_available = false;
            rc::SleepMs(2000);
            continue;
        }
        MatchMicFormats(m_feedName, m_micName);
        ComPtr<IAudioClient> client;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
            rc::SleepMs(1000);
            continue;
        }
        WAVEFORMATEX* mix = nullptr;
        client->GetMixFormat(&mix);
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        const auto* m = reinterpret_cast<const uint8_t*>(mix);
        const std::vector<uint8_t> format(m, m + sizeof(WAVEFORMATEX) + mix->cbSize);
        HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 100000, 0, mix, nullptr);
        if (SUCCEEDED(hr)) hr = client->SetEventHandle(event);
        CoTaskMemFree(mix);
        const auto* wf = reinterpret_cast<const WAVEFORMATEX*>(format.data());
        FormatReader fmt;
        fmt.Init(wf);
        const UINT channels = wf->nChannels;
        const UINT rate = wf->nSamplesPerSec;
        const UINT blockAlign = wf->nBlockAlign;
        ComPtr<IAudioRenderClient> render;
        UINT32 bufferFrames = 0;
        if (FAILED(hr) || !client || FAILED(client->GetService(IID_PPV_ARGS(&render))) ||
            FAILED(client->GetBufferSize(&bufferFrames))) {
            CloseHandle(event);
            rc::SleepMs(1000);
            continue;
        }
        m_available = true;
        client->Start();
        // The microphone's recording level in Windows: turning it down did nothing on
        // "RinCast Microphone", and testers found the same on Steam Streaming Microphone.
        EndpointVolume level;
        if (!m_micName.empty()) level.Open(FindEndpoint(eCapture, m_micName.c_str()).Get());
        // Source samples are pulled from the jitter buffer in 10 ms blocks and
        // linearly resampled to the device rate; leftovers carry over.
        std::vector<int16_t> block(480);
        size_t blockPos = block.size();
        double pos = 0;
        int16_t prev = 0, cur = 0;
        const double step = double(rc::kAudioSampleRate) / rate;
        while (m_running) {
            if (WaitForSingleObject(event, 200) != WAIT_OBJECT_0) continue;
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) break;
            const UINT32 frames = bufferFrames - padding;
            if (!frames) continue;
            BYTE* out = nullptr;
            if (FAILED(render->GetBuffer(frames, &out))) break;
            level.Update();
            for (UINT32 i = 0; i < frames; i++) {
                while (pos >= 1.0) {
                    if (blockPos >= block.size()) {
                        m_jitter.Pop(block.data(), int(block.size()));
                        blockPos = 0;
                    }
                    prev = cur;
                    cur = block[blockPos++];
                    pos -= 1.0;
                }
                const float v = (prev + (cur - prev) * float(pos)) / 32768.f * level.Next();
                pos += step;
                uint8_t* frame = out + size_t(i) * blockAlign;
                for (UINT c = 0; c < channels; c++) {
                    if (fmt.isFloat)
                        reinterpret_cast<float*>(frame)[c] = v;
                    else if (fmt.bits == 16)
                        reinterpret_cast<int16_t*>(frame)[c] = ToS16(v);
                    else if (fmt.bits == 32)
                        reinterpret_cast<int32_t*>(frame)[c] = int32_t(v * 2147483647.0);
                }
            }
            render->ReleaseBuffer(frames, 0);
        }
        client->Stop();
        CloseHandle(event);
    }
    m_available = false;
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

}  // namespace rcs
