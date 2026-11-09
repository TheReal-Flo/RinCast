// The keyboard's key clicks: BetterFrame Keyboard's two sounds (its "Original" thock and "Light"
// tap, for a key and for Space / Enter), exported from its own code as 48 kHz PCM. Like there, an
// output stays open while the keyboard is, so a click plays the moment a key is pressed.
#pragma once

#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#endif

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rv {
class Assets;
}

namespace rv::kb {

class ClickPlayer {
   public:
    ~ClickPlayer();
    void Load(const Assets& assets);
    // While the keyboard is open (the output only runs then).
    void SetOpen(bool open);
    // sound: "deep" (Original) or "light"; big: Space and Enter.
    void Play(const std::string& sound, bool big);

   private:
    bool Open();
    void Close();
    void Render(int16_t* out, int frames);
#ifdef __ANDROID__
    static aaudio_data_callback_result_t Callback(AAudioStream*, void* user, void* audio, int32_t frames);
    AAudioStream* m_stream = nullptr;
#elif !defined(__linux__)
    void* m_stream = nullptr;  // (the UI preview on Windows: no sound)
#else
    void* m_stream = nullptr;
    std::thread m_output;
    std::atomic<bool> m_running{false};
#endif
    std::vector<int16_t> m_clicks[4];  // deep key, deep big, light key, light big
    std::mutex m_lock;
    struct Voice {
        const std::vector<int16_t>* pcm;
        size_t pos;
    };
    std::vector<Voice> m_voices;
};

}  // namespace rv::kb
