// What main_terminal.cpp asks of each platform before the service starts.
#pragma once

namespace rcs {

// Permissions, cleanup after a killed run and the like: macOS asks for Screen Recording and
// Accessibility here. Returns false if the streamer can't run at all.
bool PreparePlatform();

}  // namespace rcs
