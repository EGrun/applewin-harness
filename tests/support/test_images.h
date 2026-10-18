// SPDX-License-Identifier: MIT
#pragma once
#include "core/frame_view.h"
#include <array>
#include <cstdint>

namespace harness::testimg {

// 3x2 image. Visual top row = red, green, blue; visual bottom row = white, black, gray(0x80). BGRA32.
// Bottom-up storage: buffer row 0 is the visual BOTTOM row; strideBytes = -12.
inline const std::array<uint8_t, 24> kBottomUp = {
    255, 255, 255, 0,   0, 0, 0, 0,     128, 128, 128, 0,   // bottom row
    0, 0, 255, 255,     0, 255, 0, 255, 255, 0, 0, 255,     // top row (B,G,R,A order per pixel)
};

// Same image stored top-down, strideBytes = +12.
inline const std::array<uint8_t, 24> kTopDown = {
    0, 0, 255, 255,     0, 255, 0, 255, 255, 0, 0, 255,
    255, 255, 255, 0,   0, 0, 0, 0,     128, 128, 128, 0,
};

inline FrameView View3x2() { return FrameView{kBottomUp.data() + 12, 3, 2, -12}; }
inline FrameView TopDownView3x2() { return FrameView{kTopDown.data(), 3, 2, 12}; }

}  // namespace harness::testimg
