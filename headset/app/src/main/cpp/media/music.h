// Background music while no PC is connected: Scott Buckley's tracks (CC-BY 4.0), in a
// random order, crossfaded, faded out once a connection is up.
#pragma once

#include <aaudio/AAudio.h>

#include <atomic>
#include <thread>

#include "media/music_tracks.h"
#include "platform/assets.h"
#include "rc/music.h"

namespace rv {

class MusicPlayer {
   public:
    ~MusicPlayer();
    // Reads the tracks in the background (about 15 MB).
    void Load(const Assets& assets);
    // Every frame: whether music should play now. The output only runs while it does or
    // while it fades out.
    void Update(bool wanted);

   private:
    bool Open();
    void Close();
    static aaudio_data_callback_result_t Callback(AAudioStream*, void* user, void* audio, int32_t frames);
    static void ErrorCallback(AAudioStream*, void* user, aaudio_result_t error);

    rc::MusicMixer m_mixer;
    std::thread m_loader;
    std::atomic<bool> m_loaded{false};
    AAudioStream* m_stream = nullptr;
    std::atomic<bool> m_restart{false};
    int m_openDelay = 0;  // frames
};

}  // namespace rv
