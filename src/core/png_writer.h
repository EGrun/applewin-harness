#pragma once
#include "core/frame_view.h"
#include <cstdint>
#include <filesystem>
#include <vector>

namespace harness {

// 8-bit RGB PNG (colour type 2, no interlace), one IDAT, filter 0 on every row, CRCs via zlib crc32.
// Throws std::invalid_argument if width or height is 0, or topRow is null.
std::vector<uint8_t> EncodePng(const FrameView&);

// Returns false if the file cannot be written.
bool WritePngFile(const std::filesystem::path&, const FrameView&);

}  // namespace harness
