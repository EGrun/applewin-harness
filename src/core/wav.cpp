#include "core/wav.h"
#include <cstring>
#include <stdexcept>

namespace harness {

namespace {
void PutU32(std::vector<uint8_t>& b, size_t at, uint32_t v)
{
    for (int i = 0; i < 4; ++i) b[at + size_t(i)] = uint8_t(v >> (8 * i));
}
void PutU16(std::vector<uint8_t>& b, size_t at, uint16_t v)
{
    b[at] = uint8_t(v);
    b[at + 1] = uint8_t(v >> 8);
}
}  // namespace

std::vector<uint8_t> EncodeWav(std::span<const int16_t> samples, uint32_t rate, uint16_t channels)
{
    if (channels != 1 && channels != 2) throw std::invalid_argument("EncodeWav: channels must be 1 or 2");
    if (samples.size() % channels != 0) throw std::invalid_argument("EncodeWav: samples is not a whole number of frames");
    if (rate == 0) throw std::invalid_argument("EncodeWav: rate must not be 0");

    const uint32_t dataBytes = uint32_t(samples.size() * 2);
    std::vector<uint8_t> b(44 + size_t(dataBytes));
    std::memcpy(&b[0], "RIFF", 4);
    PutU32(b, 4, 36 + dataBytes);
    std::memcpy(&b[8], "WAVE", 4);
    std::memcpy(&b[12], "fmt ", 4);
    PutU32(b, 16, 16);
    PutU16(b, 20, 1);
    PutU16(b, 22, channels);
    PutU32(b, 24, rate);
    PutU32(b, 28, rate * channels * 2);
    PutU16(b, 32, uint16_t(channels * 2));
    PutU16(b, 34, 16);
    std::memcpy(&b[36], "data", 4);
    PutU32(b, 40, dataBytes);
    for (size_t i = 0; i < samples.size(); ++i) PutU16(b, 44 + 2 * i, uint16_t(samples[i]));
    return b;
}

}  // namespace harness
