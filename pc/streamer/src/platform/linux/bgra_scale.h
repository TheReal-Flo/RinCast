// Scaling a BGRA picture on the CPU, for the rare stream whose size differs from what the capture
// delivers (the headset's decoder limit, the PC's maximum height setting, a monitor's mode
// changed mid-stream). Captures deliver the monitor's own size otherwise, which needs no copy.
#pragma once

#include <cstdint>
#include <vector>

namespace rcs {

// Bilinear: good enough for a downscale of up to 2x, which is what the stream sizes ask for.
// `dst` is resized to dstWidth * dstHeight * 4 (tightly packed).
void ScaleBgra(const uint8_t* src, int srcWidth, int srcHeight, int srcStride, std::vector<uint8_t>& dst, int dstWidth,
               int dstHeight);

// Draws a premultiplied ARGB cursor image (as XFixes and PipeWire give it) onto a BGRA picture,
// its top left at x, y (may be partly outside).
void DrawCursor(uint8_t* picture, int width, int height, int stride, const uint32_t* cursor, int cursorWidth,
                int cursorHeight, int cursorStride, int x, int y);

}  // namespace rcs
