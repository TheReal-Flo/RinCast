// The VR bridge's frame images on Linux (pc/common/vr_bridge_images.h): Vulkan images the
// streamer allocates on the GPU the stream is encoded on, whose memory goes to RinCast's Monado
// service over a Unix socket. The service renders each frame into one; VrStream encodes it from
// there, as a DMA-BUF (VAAPI, no copy) or read back to memory (NVENC, software).
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "platform/linux/ffmpeg_encoder.h"
#include "vr_bridge.h"

namespace rcs {

class VrImages {
   public:
    static VrImages& Get();

    // (Re)creates the images at width x height and hands them to connected producers. Writes
    // the GPU's id and the new generation into `shared`.
    bool Create(rcvr::Shared* shared, uint32_t width, uint32_t height);
    // Frees the images and stops serving them.
    void Destroy();

    uint32_t Width() const;
    uint32_t Height() const;
    // The slot's image as a DMA-BUF, when the GPU could export it so (linear images). The fd
    // stays the VrImages'.
    bool DmaBuf(int slot, DmaBufFrame& out) const;
    // Copies the slot's image into memory (rows of R, G, B, A); null on failure. Valid until
    // the next ReadBack. Called from one thread (the VR stream's).
    const uint8_t* ReadBack(int slot, int& stride);

   private:
    VrImages();
    ~VrImages();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace rcs
