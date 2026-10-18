// SPDX-License-Identifier: MIT
#pragma once
// Pitch estimate for sound_stopped.dominant_hz (spec: docs/specs/m3.md, Part B "dominant_hz").
#include <span>

namespace harness {

// The frequency of the largest magnitude bin (excluding bin 0) of a Hann-windowed 16,384-point FFT of mono, refined by
// parabolic interpolation over the peak bin and its neighbours. Uses the first 16,384 samples, with their mean
// subtracted before windowing (so a DC level, like the speaker's idle level, can't leak into bin 1 and win); a shorter
// input is windowed over its own length and zero-padded. Returns 0 for an empty or all-zero input. rate must be > 0
// (std::invalid_argument otherwise).
double DominantHz(std::span<const float> mono, double rate);

}  // namespace harness
