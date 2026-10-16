#pragma once
#include <cstdint>
#include <vector>

namespace harness {

struct FrameView {
    const uint8_t* topRow;   // first byte of the top visible row
    uint32_t width, height;  // pixels
    int32_t strideBytes;     // distance from one row to the next row down; negative for bottom-up buffers
};                           // pixels are BGRA32 (AppleWin bgra_t); alpha is ignored

std::vector<uint8_t> ToRgbTopDown(const FrameView&);   // width*height*3 bytes

}  // namespace harness
