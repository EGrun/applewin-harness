// SPDX-License-Identifier: MIT
#include "core/frame_view.h"

namespace harness {

std::vector<uint8_t> ToRgbTopDown(const FrameView& v)
{
    std::vector<uint8_t> out;
    if (!v.topRow || v.width == 0 || v.height == 0)
        return out;
    out.resize(static_cast<size_t>(v.width) * v.height * 3);
    uint8_t* dst = out.data();
    for (uint32_t y = 0; y < v.height; ++y) {
        const uint8_t* src = v.topRow + static_cast<ptrdiff_t>(y) * v.strideBytes;
        for (uint32_t x = 0; x < v.width; ++x, src += 4) {
            *dst++ = src[2];  // R
            *dst++ = src[1];  // G
            *dst++ = src[0];  // B
        }
    }
    return out;
}

}  // namespace harness
