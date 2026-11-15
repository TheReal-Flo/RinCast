// CPU scaling and cursor drawing for the Linux captures (bgra_scale.h).
#include "platform/linux/bgra_scale.h"

#include <algorithm>

namespace rcs {

void ScaleBgra(const uint8_t* src, int srcWidth, int srcHeight, int srcStride, std::vector<uint8_t>& dst, int dstWidth,
               int dstHeight) {
    dst.resize(size_t(dstWidth) * dstHeight * 4);
    if (srcWidth <= 0 || srcHeight <= 0 || dstWidth <= 0 || dstHeight <= 0) return;
    // 16.16 fixed point, sampling at pixel centres.
    const int64_t stepX = (int64_t(srcWidth) << 16) / dstWidth;
    const int64_t stepY = (int64_t(srcHeight) << 16) / dstHeight;
    std::vector<int> x0(dstWidth), fx(dstWidth);
    for (int x = 0; x < dstWidth; x++) {
        const int64_t sx = std::max<int64_t>(0, x * stepX + stepX / 2 - 0x8000);
        x0[x] = std::min(int(sx >> 16), srcWidth - 1);
        fx[x] = int((sx >> 8) & 0xFF);
    }
    for (int y = 0; y < dstHeight; y++) {
        const int64_t sy = std::max<int64_t>(0, y * stepY + stepY / 2 - 0x8000);
        const int y0 = std::min(int(sy >> 16), srcHeight - 1);
        const int y1 = std::min(y0 + 1, srcHeight - 1);
        const int fy = int((sy >> 8) & 0xFF);
        const uint8_t* row0 = src + size_t(y0) * srcStride;
        const uint8_t* row1 = src + size_t(y1) * srcStride;
        uint8_t* out = dst.data() + size_t(y) * dstWidth * 4;
        for (int x = 0; x < dstWidth; x++) {
            const int xa = x0[x], xb = std::min(xa + 1, srcWidth - 1), f = fx[x];
            for (int c = 0; c < 4; c++) {
                const int top = row0[xa * 4 + c] * (256 - f) + row0[xb * 4 + c] * f;
                const int bottom = row1[xa * 4 + c] * (256 - f) + row1[xb * 4 + c] * f;
                out[x * 4 + c] = uint8_t((top * (256 - fy) + bottom * fy) >> 16);
            }
        }
    }
}

void DrawCursor(uint8_t* picture, int width, int height, int stride, const uint32_t* cursor, int cursorWidth,
                int cursorHeight, int cursorStride, int x, int y) {
    const int startX = std::max(0, -x), endX = std::min(cursorWidth, width - x);
    const int startY = std::max(0, -y), endY = std::min(cursorHeight, height - y);
    for (int cy = startY; cy < endY; cy++) {
        uint8_t* out = picture + size_t(y + cy) * stride + size_t(x) * 4;
        const uint32_t* in = cursor + size_t(cy) * cursorStride;
        for (int cx = startX; cx < endX; cx++) {
            const uint32_t p = in[cx];
            const uint32_t a = p >> 24;
            if (!a) continue;
            uint8_t* d = out + cx * 4;
            const uint32_t keep = 255 - a;
            // Premultiplied: colour + what shows through.
            d[0] = uint8_t(std::min<uint32_t>(255, (p & 0xFF) + (d[0] * keep + 127) / 255));
            d[1] = uint8_t(std::min<uint32_t>(255, ((p >> 8) & 0xFF) + (d[1] * keep + 127) / 255));
            d[2] = uint8_t(std::min<uint32_t>(255, ((p >> 16) & 0xFF) + (d[2] * keep + 127) / 255));
        }
    }
}

}  // namespace rcs
