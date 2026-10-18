// SPDX-License-Identifier: MIT
#pragma once
// Sound events for subscribe_audio (spec: docs/specs/m3.md, Part B "subscribe_audio").
#include <cstdint>
#include <memory>
#include <vector>

namespace harness {

class AudioCapture;

inline constexpr uint32_t kLevelBlockFrames = 441;   // 10 ms blocks on the 44,100 Hz timeline
inline constexpr uint32_t kDominantFftPoints = 16384;

struct SoundEvent {
    enum Kind { Started, Stopped } kind = Started;
    uint64_t startCycle = 0;   // FrameToCycle of the first loud block's first frame
    uint64_t endCycle = 0;     // Stopped: FrameToCycle of the end of the last loud block; Started: 0
    double peakDb = 0.0;       // Stopped: the loudest block's level in dBFS; Started: 0
    double dominantHz = 0.0;   // Stopped: DominantHz of the sound's first 16,384 frames (mono); Started: 0
};

// Turns the timeline into sound_started / sound_stopped events.
// - Blocks are timeline frames [441 b, 441 b + 441). A block's level is the AC RMS (its mean subtracted) of the mono
//   mix (L + R) / 2, in dBFS with full scale 32,768 (silence: -infinity).
// - Sound starts at the first block above thresholdDb. It stops once ceil(minSilenceMs / 10) consecutive blocks are
//   at or below it; endCycle is the end of the last loud block, so a shorter quiet gap is part of the sound.
// - dominantHz: DominantHz over the sound's first 16,384 frames (from its first loud block; up to its last loud block
//   if shorter, zero-padded).
// Feed processes, in order, every block not processed before whose first frame's cycle is at or after fromCycle (and
// at or after the capture's OldestCycle()) and whose end frame is at or before
// CycleToFrame(min(uptoCycle, capture.NewestCycle())).
// Feeding in pieces gives the same events as one Feed. The session constructs one per subscription with fromCycle =
// EmulatedCycles() at subscribe time, so only sound from then on is reported.
class LevelDetector {
public:
    LevelDetector(double thresholdDb = -50.0, uint32_t minSilenceMs = 100, uint64_t fromCycle = 0);

    ~LevelDetector();
    LevelDetector(LevelDetector&&) noexcept;
    LevelDetector& operator=(LevelDetector&&) noexcept;

    std::vector<SoundEvent> Feed(const AudioCapture& capture, uint64_t uptoCycle);

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace harness
