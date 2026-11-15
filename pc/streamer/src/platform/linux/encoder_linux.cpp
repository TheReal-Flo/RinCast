// Encoding on Linux, with FFmpeg (docs/LINUX.md). Not there yet: no codec is offered, so the
// headset is told this PC can't stream video.
#include "encode/encoder.h"

#include "rc/platform.h"

namespace rcs {

EncoderCaps ProbeEncoderCaps(UINT) {
    EncoderCaps caps;
    caps.backend = "none";
    RC_LOGW("Video encoding isn't implemented on Linux yet");
    return caps;
}

}  // namespace rcs
