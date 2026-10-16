#include "core/frame_view.h"
#include <stdexcept>

namespace harness {

std::vector<uint8_t> ToRgbTopDown(const FrameView&)
{
    throw std::runtime_error("not implemented");
}

}  // namespace harness
