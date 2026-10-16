// Stub (M3 Part B Task 1). Task 4 implements the detector.
#include "core/level_detector.h"
#include "core/audio_capture.h"

namespace harness {

LevelDetector::LevelDetector(double, uint32_t, uint64_t) {}

std::vector<SoundEvent> LevelDetector::Feed(const AudioCapture&, uint64_t)
{
    return {};
}

}  // namespace harness
