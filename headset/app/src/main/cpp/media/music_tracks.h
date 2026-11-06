// The background music, also credited in Help: CC-BY needs title, author and licence.
#pragma once

namespace rv {

struct MusicTrack {
    const char* file;   // in assets/music
    const char* title;  // by Scott Buckley, CC-BY 4.0
};

inline constexpr MusicTrack kMusicTracks[] = {
    {"electric-dreams.opus", "Electric Dreams"},
    {"twilight-echo.opus", "Twilight Echo"},
    {"machina.opus", "Machina"},
    {"tears-in-rain.opus", "Tears In Rain"},
};

}  // namespace rv
