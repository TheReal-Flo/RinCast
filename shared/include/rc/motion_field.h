// Frame generation: how the picture moved, carried after VrFrameMetadata in a VR frame.
//
//   [MotionHeader][runs: uint8 still blocks, uint8 moving blocks, then x, y per moving block]
//
// x and y are int8, in kMotionUnitPixels per frame. Head turns are already taken out, because
// the Quest's compositor handles those itself. Most blocks don't move, so this stays small.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rc {

inline constexpr int kMotionUnitPixels = 4;

#pragma pack(push, 1)
struct MotionHeader {
    uint16_t gridWidth;
    uint16_t gridHeight;
    uint16_t blockSize;           // pixels of the side-by-side frame per grid cell
    uint16_t reserved;
    uint64_t previousTimestampNs;  // VrFrameMetadata::trackingTimestampNs of the frame compared with
};
#pragma pack(pop)

// grid: x, y per block, row by row.
inline void EncodeMotionRuns(const int8_t* grid, size_t blocks, std::vector<uint8_t>& out) {
    size_t i = 0;
    while (i < blocks) {
        size_t still = 0, moving = 0;
        while (i + still < blocks && still < 255 && grid[(i + still) * 2] == 0 && grid[(i + still) * 2 + 1] == 0) still++;
        const size_t start = i + still;
        while (start + moving < blocks && moving < 255 && (grid[(start + moving) * 2] != 0 || grid[(start + moving) * 2 + 1] != 0))
            moving++;
        out.push_back(uint8_t(still));
        out.push_back(uint8_t(moving));
        const auto* bytes = reinterpret_cast<const uint8_t*>(grid + start * 2);
        out.insert(out.end(), bytes, bytes + moving * 2);
        i = start + moving;
    }
}

// False if the data doesn't describe exactly `blocks` blocks.
inline bool DecodeMotionRuns(const uint8_t* data, size_t size, size_t blocks, std::vector<int8_t>& grid) {
    grid.assign(blocks * 2, 0);
    size_t i = 0, at = 0;
    while (at < size) {
        if (size - at < 2) return false;
        const size_t still = data[at], moving = data[at + 1];
        at += 2;
        i += still;
        if (i + moving > blocks || size - at < moving * 2) return false;
        if (moving) std::memcpy(&grid[i * 2], data + at, moving * 2);
        at += moving * 2;
        i += moving;
    }
    return i == blocks;
}

}  // namespace rc
