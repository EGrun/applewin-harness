#pragma once
// The agent-audio timeline (spec: docs/specs/m3.md, D14 "Capture"; plan: M3 Part B, Global Constraints "Timeline" and
// "Placing commits"). Independent of AppleWin: it only knows emulated cycles.
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace harness {

inline constexpr uint32_t kAudioSampleRate = 44100;   // timeline frames per emulated second
inline constexpr int kAudioChannels = 2;              // the timeline is interleaved stereo int16
inline constexpr uint64_t kAudioRingFrames = 30ull * kAudioSampleRate;   // 30 s = 1,323,000 frames
// AppleWin's CLK_6502_NTSC (Common.h), the base clock; a test checks they are equal.
inline constexpr double kAudioBaseClockHz = (157500000.0 / 11.0) * 65.0 / 912.0;
// Cycles per timeline frame, exactly: CLK_6502_NTSC / 44100 = 213281250 / 209 / 44100 = 203125 / 8778 (~23.1402).
inline constexpr uint64_t kCyclesPerFrameNum = 203125;
inline constexpr uint64_t kCyclesPerFrameDen = 8778;

// Mixes every sound voice into one 44,100 Hz stereo int16 timeline addressed by emulated cycle (cycles since
// power-on). Timeline frame k is at cycle FrameToCycle(k). The newest kAudioRingFrames frames are kept.
//
// Placement: a voice's commit of N sample frames covers the cycles from the end of that voice's previous commit to
// endCycle. The first commit of a voice (and the first after ResetVoice or Clear) covers the N x (CLK_6502_NTSC / rate)
// cycles before endCycle (clamped at 0). The N frames are resampled linearly (input frame i sits at timeline position
// start + i x M / N, M = the number of timeline frames; past the last input frame the final step's slope continues) onto the timeline frames
// [CycleToFrame(start), CycleToFrame(endCycle)) and added to what is there with saturation to int16. A mono commit
// (channels == 1) goes to both channels. If the interval exceeds N x (CLK_6502_NTSC / rate) cycles by more than 100 ms,
// the voice was paused: the gap is silence and the commit is placed like a first commit. A commit whose interval is
// empty (endCycle not after the voice's previous end) adds nothing but still counts as that voice's latest commit. Frames older than the ring are dropped.
//
// Not thread-safe: the emulating thread commits and the session reads, on the same thread (Part A threading).
class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    // interleaved.size() must be a multiple of channels; channels is 1 or 2 (std::invalid_argument otherwise).
    void Commit(int voiceId, std::span<const int16_t> interleaved, int channels, uint64_t endCycle);
    // Sets the voice's sample rate (default 44,100); kept across ResetVoice and Clear. Used for cycles-per-sample.
    void RegisterVoice(int voiceId, uint32_t sampleRate);
    // The voice's next commit is placed like a first commit (a buffer re-initialised or released).
    void ResetVoice(int voiceId);

    // The latest endCycle committed by any voice; 0 when empty. The timeline holds frames up to CycleToFrame(this).
    uint64_t NewestCycle() const;
    // FrameToCycle of the oldest frame still held: 0 until the ring has wrapped.
    uint64_t OldestCycle() const;

    // Timeline frames [CycleToFrame(fromCycle), CycleToFrame(min(toCycle, NewestCycle()))) as interleaved stereo; empty
    // when that range is empty. Throws std::out_of_range when fromCycle < OldestCycle().
    std::vector<int16_t> Read(uint64_t fromCycle, uint64_t toCycle) const;

    // Empties the timeline and forgets every voice (PowerCycle: the timeline restarts at cycle 0).
    void Clear();

    // The first cycle at or after frame k's time: ceil(k x 203125 / 8778).
    static uint64_t FrameToCycle(uint64_t frame);
    // The frame whose time is at or before cycle c: floor(c x 8778 / 203125). CycleToFrame(FrameToCycle(k)) == k.
    static uint64_t CycleToFrame(uint64_t cycle);

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace harness
