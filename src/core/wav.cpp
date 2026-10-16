// Stub (M3 Part B Task 1). Task 3 implements the encoder.
#include "core/wav.h"
#include <stdexcept>

namespace harness {

std::vector<uint8_t> EncodeWav(std::span<const int16_t>, uint32_t, uint16_t)
{
    throw std::logic_error("EncodeWav: not implemented");
}

}  // namespace harness
