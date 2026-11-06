// audio-check: shows which virtual audio devices RinCast finds and which ones
// each "Audio devices" setting resolves to. Changes nothing on the system.
//
//   audio-check                 list devices and routes
//   audio-check --mic-test S    play a tone through the streamer's microphone path into
//                               virtual microphone set S (rincast | steam) and measure its
//                               pitch and delay on the capture side
//   audio-check --formats S     formats both sides of virtual microphone set S accept
//   audio-check --set-format S RATE CH   set both sides of S to 16-bit RATE/CH (saves the old formats)
//   audio-check --restore-format S       put the saved formats back
#include <windows.h>
#include <audioclient.h>
#include <initguid.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <algorithm>

#include "audio/audio.h"
#include "rc/audio_codec.h"
#include "rc/platform.h"
#include "rc/protocol.h"
#include "util.h"

using namespace rcs;
using Microsoft::WRL::ComPtr;

static ComPtr<IMMDevice> Find(EDataFlow flow, const std::wstring& name) {
    ComPtr<IMMDeviceEnumerator> e;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e)))) return nullptr;
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) return nullptr;
    UINT n = 0;
    col->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        ComPtr<IMMDevice> d;
        ComPtr<IPropertyStore> props;
        col->Item(i, &d);
        if (FAILED(d->OpenPropertyStore(STGM_READ, &props))) continue;
        PROPVARIANT v;
        PropVariantInit(&v);
        std::wstring fn;
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) fn = v.pwszVal;
        PropVariantClear(&v);
        if (fn == name || fn.rfind(name + L" (", 0) == 0 || fn.find(L"(" + name + L")") != std::wstring::npos) return d;
    }
    return nullptr;
}

static bool IsFloat(const WAVEFORMATEX* f) {
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        return reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT &&
               f->wBitsPerSample == 32;
    return false;
}

// Plays a 440 Hz tone through the real microphone path (Opus packets every 10 ms into
// MicPlayer, as from the headset) and records the virtual microphone like an app would.
// Two tone bursts: the pitch shows whether the sample rates match, the delay from each
// burst's start to where it is heard whether latency grows.
static int MicTest(audio::DeviceSet set) {
    const auto& names = audio::Names(set);
    auto mic = Find(eCapture, names.mic);
    if (!mic || !Find(eRender, names.micFeed)) {
        std::printf("virtual microphone not found\n");
        return 2;
    }
    ComPtr<IAudioClient> in;
    mic->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &in);
    WAVEFORMATEX* inf = nullptr;
    in->GetMixFormat(&inf);
    if (!IsFloat(inf) || FAILED(in->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, inf, nullptr))) {
        std::printf("could not open %s (float32 expected)\n", Narrow(names.mic).c_str());
        return 3;
    }
    ComPtr<IAudioCaptureClient> capture;
    in->GetService(IID_PPV_ARGS(&capture));
    const UINT rate = inf->nSamplesPerSec, ch = inf->nChannels;

    MicPlayer player;
    if (!player.Start(names.micFeed, names.mic)) return 4;
    rc::AudioEncoder enc;
    enc.Init(48000, 1, 48000, true);
    std::atomic<bool> done{false};
    // Tone 1.0-3.0 s and 4.0-6.0 s, silence otherwise.
    const int64_t t0 = rc::NowNs() + 500'000'000LL;
    auto toneAt = [&](int64_t ns) {
        const double s = double(ns - t0) / 1e9;
        return (s >= 1.0 && s < 3.0) || (s >= 4.0 && s < 6.0);
    };
    std::thread feeder([&] {
        uint32_t seq = 0;
        double phase = 0;
        int16_t pcm[480];
        uint8_t packet[sizeof(rc::AudioPacketHeader) + 1000];
        for (int64_t next = t0; !done; next += 10'000'000LL) {
            while (rc::NowNs() < next) Sleep(1);
            const bool on = toneAt(next);
            for (int i = 0; i < 480; i++, phase += 2 * 3.14159265358979 * 440.0 / 48000)
                pcm[i] = on ? int16_t(8000 * std::sin(phase)) : 0;
            rc::AudioPacketHeader h{};
            h.stream = uint8_t(rc::AudioStream::MicToPc);
            h.channels = 1;
            h.frameSamples = 480;
            h.sequence = ++seq;
            const int n = enc.Encode(pcm, 480, packet + sizeof(h), 1000);
            std::memcpy(packet, &h, sizeof(h));
            if (n > 0) player.PushPacket(packet, sizeof(h) + size_t(n));
        }
    });

    in->Start();
    std::vector<float> mono;
    std::vector<int64_t> when;  // capture time of each sample (QPC ns)
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    while (rc::NowNs() < t0 + 7'500'000'000LL) {
        UINT32 packetFrames = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packetFrames)) && packetFrames) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 qpc = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, &qpc))) break;
            const float* f = reinterpret_cast<const float*>(data);
            for (UINT32 i = 0; i < frames; i++) {
                mono.push_back((flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.f : f[i * ch]);
                // qpc is in 100 ns units for the first frame of the packet.
                when.push_back(int64_t(qpc) * 100 + int64_t(i) * 1'000'000'000LL / rate);
            }
            capture->ReleaseBuffer(frames);
        }
        Sleep(5);
    }
    done = true;
    feeder.join();
    in->Stop();
    player.Stop();
    CoTaskMemFree(inf);

    // Onset of each burst (first sample above a threshold after it starts) and pitch from
    // zero crossings in the middle of each burst.
    bool ok = true;
    for (int b = 0; b < 2; b++) {
        const int64_t start = t0 + int64_t(b * 3 + 1) * 1'000'000'000LL;
        size_t onset = 0;
        for (size_t i = 0; i < mono.size(); i++)
            if (when[i] > start && std::fabs(mono[i]) > 0.05f) {
                onset = i;
                break;
            }
        if (!onset) {
            std::printf("burst %d: not heard\n", b + 1);
            ok = false;
            continue;
        }
        // Pitch from the typical spacing of upward zero crossings (gaps don't skew it), and
        // how much of the burst actually arrived (gaps are dropouts).
        const size_t from = onset + rate / 4, to = std::min(mono.size(), onset + rate * 3 / 2);
        std::vector<double> periods;
        size_t last = 0, loud = 0;
        for (size_t i = from + 1; i < to; i++) {
            if (std::fabs(mono[i]) > 0.02f) loud++;
            if (mono[i - 1] < 0 && mono[i] >= 0) {
                if (last) periods.push_back(double(i - last));
                last = i;
            }
        }
        std::sort(periods.begin(), periods.end());
        const double hz = periods.empty() ? 0 : rate / periods[periods.size() / 2];
        const double present = std::min(1.0, double(loud) / double(to - from) / 0.9);  // a sine is above 0.02 ~90 % of the time
        const double delayMs = (when[onset] - start) / 1e6;
        std::printf("burst %d: heard after %.0f ms at %.1f Hz (sent 440 Hz), audio present %.0f %% of the time\n", b + 1,
                    delayMs, hz, present * 100);
        ok &= std::fabs(hz - 440) < 5 && delayMs < 400 && present > 0.95;
    }
    std::printf("%s -> %s (capture at %u Hz): %s\n", Narrow(names.micFeed).c_str(), Narrow(names.mic).c_str(), rate,
                ok ? "OK" : "PROBLEM");
    return ok ? 0 : 1;
}

// Undocumented interface the Sound control panel uses to change an endpoint's default
// format (same declaration as in the streamer).
MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8")
IPolicyConfig : public IUnknown {
   public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
};
static const CLSID kPolicyConfigClient = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

static std::wstring Id(IMMDevice* d) {
    LPWSTR id = nullptr;
    d->GetId(&id);
    std::wstring s = id ? id : L"";
    CoTaskMemFree(id);
    return s;
}

static std::string SavePath(EDataFlow flow) {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    return std::string(tmp) + (flow == eRender ? "rincast-micfeed-format.bin" : "rincast-mic-format.bin");
}

static WAVEFORMATEXTENSIBLE MakeFormat(UINT rate, WORD ch, WORD bits, bool isFloat) {
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = ch;
    f.Format.nSamplesPerSec = rate;
    f.Format.wBitsPerSample = bits;
    f.Format.nBlockAlign = WORD(ch * bits / 8);
    f.Format.nAvgBytesPerSec = rate * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = bits;
    f.dwChannelMask = ch == 1 ? SPEAKER_FRONT_CENTER : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    f.SubFormat = isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    return f;
}

// Sets both sides of a virtual microphone to the same default format (16-bit PCM at the
// given rate and channels), saving the previous formats once for --restore-format.
static int SetFormat(audio::DeviceSet set, UINT rate, WORD ch, bool restore) {
    const auto& names = audio::Names(set);
    ComPtr<IPolicyConfig> policy;
    if (FAILED(CoCreateInstance(kPolicyConfigClient, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig), &policy))) {
        std::printf("IPolicyConfig unavailable\n");
        return 2;
    }
    int failures = 0;
    for (auto [flow, name] : {std::pair{eRender, names.micFeed}, std::pair{eCapture, names.mic}}) {
        auto dev = Find(flow, name);
        if (!dev) continue;
        const std::wstring id = Id(dev.Get());
        const std::string path = SavePath(flow);
        HRESULT hr;
        if (restore) {
            FILE* f = nullptr;
            fopen_s(&f, path.c_str(), "rb");
            if (!f) {
                std::printf("%s: nothing saved\n", Narrow(name).c_str());
                continue;
            }
            std::vector<uint8_t> blob(4096);
            blob.resize(std::fread(blob.data(), 1, blob.size(), f));
            std::fclose(f);
            // Saved as [device format size][device format][mix format].
            const uint32_t devSize = *reinterpret_cast<const uint32_t*>(blob.data());
            hr = policy->SetDeviceFormat(id.c_str(), reinterpret_cast<WAVEFORMATEX*>(blob.data() + 4),
                                         reinterpret_cast<WAVEFORMATEX*>(blob.data() + 4 + devSize));
            if (SUCCEEDED(hr)) std::remove(path.c_str());
        } else {
            FILE* existing = nullptr;
            fopen_s(&existing, path.c_str(), "rb");
            if (existing) {
                std::fclose(existing);  // keep the original from the first change
            } else {
                WAVEFORMATEX *dev = nullptr, *mix = nullptr;
                policy->GetDeviceFormat(id.c_str(), 0, &dev);
                policy->GetMixFormat(id.c_str(), &mix);
                if (dev && mix) {
                    FILE* f = nullptr;
                    fopen_s(&f, path.c_str(), "wb");
                    const uint32_t devSize = sizeof(WAVEFORMATEX) + dev->cbSize;
                    std::fwrite(&devSize, 4, 1, f);
                    std::fwrite(dev, 1, devSize, f);
                    std::fwrite(mix, 1, sizeof(WAVEFORMATEX) + mix->cbSize, f);
                    std::fclose(f);
                }
                CoTaskMemFree(dev);
                CoTaskMemFree(mix);
            }
            WAVEFORMATEXTENSIBLE dev = MakeFormat(rate, ch, 16, false), mix = MakeFormat(rate, ch, 32, true);
            hr = policy->SetDeviceFormat(id.c_str(), &dev.Format, &mix.Format);
        }
        std::printf("%s (%s): %s (0x%08lx)\n", Narrow(name).c_str(), flow == eRender ? "render" : "capture",
                    SUCCEEDED(hr) ? (restore ? "restored" : "set") : "failed", unsigned long(hr));
        failures += FAILED(hr);
    }
    return failures ? 1 : 0;
}

// Lists the formats both sides of a virtual microphone accept in exclusive mode (what
// their drivers really support), next to the shared-mode format Windows runs them at.
static int Formats(audio::DeviceSet set) {
    const auto& names = audio::Names(set);
    for (auto [flow, name] : {std::pair{eRender, names.micFeed}, std::pair{eCapture, names.mic}}) {
        auto dev = Find(flow, name);
        if (!dev) {
            std::printf("%s: not found\n", Narrow(name).c_str());
            continue;
        }
        ComPtr<IAudioClient> c;
        dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &c);
        WAVEFORMATEX* mix = nullptr;
        c->GetMixFormat(&mix);
        std::printf("%s (%s): shared %u Hz, %u ch, %u bit; exclusive:", Narrow(name).c_str(),
                    flow == eRender ? "render" : "capture", unsigned(mix->nSamplesPerSec), unsigned(mix->nChannels),
                    unsigned(mix->wBitsPerSample));
        CoTaskMemFree(mix);
        for (UINT rate : {16000u, 22050u, 32000u, 44100u, 48000u, 96000u})
            for (WORD ch : {WORD(1), WORD(2)})
                for (auto [bits, isFloat] : {std::pair{WORD(16), false}, std::pair{WORD(32), false}, std::pair{WORD(32), true}}) {
                    WAVEFORMATEX w{};
                    w.wFormatTag = isFloat ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
                    w.nChannels = ch;
                    w.nSamplesPerSec = rate;
                    w.wBitsPerSample = bits;
                    w.nBlockAlign = WORD(ch * bits / 8);
                    w.nAvgBytesPerSec = rate * w.nBlockAlign;
                    if (c->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &w, nullptr) == S_OK)
                        std::printf(" %u/%u/%u%s", rate, unsigned(ch), unsigned(bits), isFloat ? "f" : "");
                }
        std::printf("\n");
    }
    return 0;
}

int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (argc == 5 && std::strcmp(argv[1], "--set-format") == 0)
        return SetFormat(std::strcmp(argv[2], "steam") == 0 ? audio::DeviceSet::Steam : audio::DeviceSet::RinCast,
                         UINT(std::atoi(argv[3])), WORD(std::atoi(argv[4])), false);
    if (argc == 3 && std::strcmp(argv[1], "--restore-format") == 0)
        return SetFormat(std::strcmp(argv[2], "steam") == 0 ? audio::DeviceSet::Steam : audio::DeviceSet::RinCast, 0, 0, true);
    if (argc == 3 && std::strcmp(argv[1], "--formats") == 0)
        return Formats(std::strcmp(argv[2], "steam") == 0 ? audio::DeviceSet::Steam : audio::DeviceSet::RinCast);
    if (argc == 3 && std::strcmp(argv[1], "--mic-test") == 0)
        return MicTest(std::strcmp(argv[2], "steam") == 0 ? audio::DeviceSet::Steam : audio::DeviceSet::RinCast);

    for (auto [set, label] : {std::pair{audio::DeviceSet::RinCast, "RinCast"}, std::pair{audio::DeviceSet::Steam, "Steam"}}) {
        const auto st = audio::QueryDriver(set);
        std::printf("%-8s speaker=%s microphone=%s microphoneFeed=%s\n", label, st.speaker ? "yes" : "no",
                    st.microphone ? "yes" : "no", st.microphoneFeed ? "yes" : "no");
    }
    for (const char* pref : {"auto", "rincast", "steam", "system"}) {
        const auto r = audio::ResolveRoute(pref);
        std::printf("%-8s -> speaker: %s, microphone: %s\n", pref,
                    r.speaker ? Narrow(r.speaker).c_str() : "default device",
                    r.mic ? Narrow(r.mic).c_str() : "none");
    }
    return 0;
}
