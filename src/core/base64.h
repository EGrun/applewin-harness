#pragma once
#include <cstdint>
#include <span>
#include <string>

namespace harness {

// RFC 4648 base64 with '=' padding.
std::string Base64Encode(std::span<const uint8_t>);

}  // namespace harness
