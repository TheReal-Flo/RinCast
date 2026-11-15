// What the front ends (main_terminal.cpp; on Linux also main_gtk.cpp) ask of each platform.
#pragma once

namespace rcs {

// Permissions, cleanup after a killed run and the like: macOS asks for Screen Recording and
// Accessibility here. Returns false if the streamer can't run at all.
bool PreparePlatform();

// The streamer without a window: the service, with approvals asked in the terminal. Returns the
// process's exit code. main() on macOS; on Linux when there's no display or with --headless.
int RunInTerminal(int argc, char** argv);

}  // namespace rcs
