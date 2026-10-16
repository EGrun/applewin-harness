#pragma once
// WAV encoding for get_audio (spec: docs/specs/m3.md, Part B "get_audio").
#include <cstdint>
#include <span>
#include <vector>

namespace harness {

// A canonical 44-byte-header PCM WAV: "RIFF", size 36 + data bytes, "WAVE", a 16-byte "fmt " chunk (format 1,
// channels, rate, byte rate = rate x channels x 2, block align = channels x 2, 16 bits), then "data", its size, and the
// samples as little-endian int16. channels must be 1 or 2 and samples.size() a multiple of it, and rate must not be 0
// (std::invalid_argument otherwise).
std::vector<uint8_t> EncodeWav(std::span<const int16_t> samples, uint32_t rate, uint16_t channels);

}  // namespace harness
