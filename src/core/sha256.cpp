#include "core/sha256.h"
#include <stdexcept>

namespace harness {

std::string Sha256Hex(std::span<const uint8_t>)
{
    throw std::runtime_error("not implemented");
}

std::string FrameHash(const FrameView&)
{
    throw std::runtime_error("not implemented");
}

}  // namespace harness
