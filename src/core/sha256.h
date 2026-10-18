// SPDX-License-Identifier: MIT
#pragma once
#include "core/frame_view.h"
#include <cstdint>
#include <span>
#include <string>

namespace harness {

// 64 lowercase hex characters (Windows BCrypt).
std::string Sha256Hex(std::span<const uint8_t>);

// Defined as Sha256Hex(ToRgbTopDown(view)).
std::string FrameHash(const FrameView&);

}  // namespace harness
