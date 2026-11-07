#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#import <CoreVideo/CoreVideo.h>

#include "mac_monitors.h"

namespace rcm {

using FrameCallback = std::function<void(CVPixelBufferRef pixelBuffer, uint64_t timestampNs)>;

class MacDesktopCapture {
   public:
    MacDesktopCapture();
    ~MacDesktopCapture();

    bool Init(const MacMonitorInfo& monitor, uint32_t outWidth, uint32_t outHeight, float fps, FrameCallback onFrame);
    bool Start();
    void Stop();

    uint32_t Width() const { return m_outWidth; }
    uint32_t Height() const { return m_outHeight; }
    const MacMonitorInfo& Monitor() const { return m_monitor; }

   private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    MacMonitorInfo m_monitor;
    uint32_t m_outWidth = 0;
    uint32_t m_outHeight = 0;
    float m_fps = 60.0f;
};

}  // namespace rcm
