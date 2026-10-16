#include "core/png_writer.h"
#include <stdexcept>

namespace harness {

std::vector<uint8_t> EncodePng(const FrameView&)
{
    throw std::runtime_error("not implemented");
}

bool WritePngFile(const std::filesystem::path&, const FrameView&)
{
    throw std::runtime_error("not implemented");
}

}  // namespace harness
