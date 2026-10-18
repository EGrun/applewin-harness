// SPDX-License-Identifier: MIT
// LevelDetector (spec: docs/specs/m3.md, Part B "subscribe_audio").
#include "core/level_detector.h"
#include "core/audio_capture.h"
#include "core/fft.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace harness {

struct LevelDetector::State {
    double thresholdDb = -50.0;
    uint32_t silenceBlocks = 10;
    uint64_t fromCycle = 0;
    bool initialised = false;
    uint64_t nextBlock = 0;

    bool inSound = false;
    uint64_t startFrame = 0;
    uint64_t lastLoudEndFrame = 0;
    uint32_t quietRun = 0;
    double peakDb = -std::numeric_limits<double>::infinity();
    std::vector<float> mono;   // mono mix from the sound's first frame, at most kDominantFftPoints frames
};

LevelDetector::LevelDetector(double thresholdDb, uint32_t minSilenceMs, uint64_t fromCycle)
    : state_(std::make_unique<State>())
{
    state_->thresholdDb = thresholdDb;
    state_->silenceBlocks = (std::max)(1u, (minSilenceMs + 9) / 10);
    state_->fromCycle = fromCycle;
}

LevelDetector::~LevelDetector() = default;
LevelDetector::LevelDetector(LevelDetector&&) noexcept = default;
LevelDetector& LevelDetector::operator=(LevelDetector&&) noexcept = default;

namespace {
// The first block whose first frame is at or after cycle.
uint64_t FirstBlockAtOrAfter(uint64_t cycle)
{
    uint64_t f = AudioCapture::CycleToFrame(cycle);
    if (AudioCapture::FrameToCycle(f) < cycle) ++f;
    return (f + kLevelBlockFrames - 1) / kLevelBlockFrames;
}
}  // namespace

std::vector<SoundEvent> LevelDetector::Feed(const AudioCapture& capture, uint64_t uptoCycle)
{
    State& s = *state_;
    std::vector<SoundEvent> events;

    if (!s.initialised) {
        s.nextBlock = FirstBlockAtOrAfter(s.fromCycle);
        s.initialised = true;
    }
    s.nextBlock = (std::max)(s.nextBlock, FirstBlockAtOrAfter(capture.OldestCycle()));   // older frames are gone

    const uint64_t limitFrame = AudioCapture::CycleToFrame((std::min)(uptoCycle, capture.NewestCycle()));
    for (; (s.nextBlock + 1) * kLevelBlockFrames <= limitFrame; ++s.nextBlock) {
        const uint64_t f0 = s.nextBlock * kLevelBlockFrames, f1 = f0 + kLevelBlockFrames;
        const std::vector<int16_t> pcm = capture.Read(AudioCapture::FrameToCycle(f0), AudioCapture::FrameToCycle(f1));
        if (pcm.size() != size_t(kLevelBlockFrames) * 2) continue;

        float mono[kLevelBlockFrames];
        double mean = 0.0;
        for (uint32_t i = 0; i < kLevelBlockFrames; ++i) {
            mono[i] = (float(pcm[2 * i]) + float(pcm[2 * i + 1])) * 0.5f;
            mean += mono[i];
        }
        mean /= kLevelBlockFrames;
        // AC level: the block's mean and its least-squares linear trend are removed. AppleWin's speaker ramps its DC
        // level back to zero at one unit per sample after a sound (a ~740 ms slow ramp at -48 dBFS once the bell has
        // been, which is not sound); a line has no AC content.
        constexpr double kCentre = (kLevelBlockFrames - 1) / 2.0;
        double sxy = 0.0, sxx = 0.0;
        for (uint32_t i = 0; i < kLevelBlockFrames; ++i) {
            sxy += (i - kCentre) * (mono[i] - mean);
            sxx += (i - kCentre) * (i - kCentre);
        }
        const double slope = sxy / sxx;
        double sq = 0.0;
        for (uint32_t i = 0; i < kLevelBlockFrames; ++i) {
            const double r = (mono[i] - mean) - slope * (i - kCentre);
            sq += r * r;
        }
        const double rms = std::sqrt(sq / kLevelBlockFrames);
        const double db = rms > 0.0 ? 20.0 * std::log10(rms / 32768.0) : -std::numeric_limits<double>::infinity();
        const bool loud = db > s.thresholdDb;

        if (!s.inSound) {
            if (!loud) continue;
            s.inSound = true;
            s.startFrame = f0;
            s.quietRun = 0;
            s.peakDb = db;
            s.mono.clear();
            SoundEvent e;
            e.kind = SoundEvent::Started;
            e.startCycle = AudioCapture::FrameToCycle(f0);
            events.push_back(e);
        }
        if (s.mono.size() < kDominantFftPoints) {
            const size_t take = (std::min)(size_t(kLevelBlockFrames), kDominantFftPoints - s.mono.size());
            s.mono.insert(s.mono.end(), mono, mono + take);
        }
        if (loud) {
            s.quietRun = 0;
            s.lastLoudEndFrame = f1;
            s.peakDb = (std::max)(s.peakDb, db);
        } else if (++s.quietRun >= s.silenceBlocks) {
            SoundEvent e;
            e.kind = SoundEvent::Stopped;
            e.startCycle = AudioCapture::FrameToCycle(s.startFrame);
            e.endCycle = AudioCapture::FrameToCycle(s.lastLoudEndFrame);
            e.peakDb = s.peakDb;
            const size_t len = (std::min)(s.mono.size(), size_t(s.lastLoudEndFrame - s.startFrame));
            e.dominantHz = DominantHz(std::span<const float>(s.mono.data(), len), double(kAudioSampleRate));
            events.push_back(e);
            s.inSound = false;
            s.mono.clear();
        }
    }
    return events;
}

}  // namespace harness
