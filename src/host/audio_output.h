#pragma once
// Windowed playback of the agent-audio timeline (spec: docs/specs/m3.md, D14 "Playback"). Windowed realtime only;
// headless never creates one, so dsound.dll (delay-loaded) is never loaded headless.
#include <windows.h>
#include "core/audio_capture.h"
#include <cstdint>
#include <memory>

namespace harness {

// Streams the AudioCapture timeline (44,100 Hz stereo int16) to a harness-owned looping DirectSound buffer the size of
// AppleWin's speaker buffer (MAX_SAMPLES frames, 65,536 bytes), writing from 3/8 of the way in.
//
// Playback trails the emulated clock by kLagCycles (110 ms, the B9 hold-back): a voice can commit up to ~64 ms late
// (a Mockingboard with TIMER1 active), and any voice silent for longer is gapped by the capture's B7 rule, so every
// frame written is final. Frames past the capture's newest cycle (no voice committed) are written as silence.
//
// The DirectSound buffer plays only while playing (SetPlaying(true)) and is stopped otherwise, so nothing is heard
// outside realtime runs. Queued audio is kept across a stop and resumes seamlessly when the next realtime run continues
// the same timeline (back-to-back CLI steps, a server pause and resume). If the timeline moved in between (a stepped
// run, a power cycle), the buffer is silenced and the write position resynced to 3/8 ahead of the play cursor.
class AudioOutput {
public:
    // Emulated cycles playback trails the clock by: 110 ms at the base clock (112,253), as the session's detectors.
    static constexpr uint64_t kLagCycles = uint64_t(0.110 * kAudioBaseClockHz);

    // DirectSound on the default device; null if there is no device or any DirectSound call fails.
    static std::unique_ptr<AudioOutput> Open(HWND window);
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    // Called after every realtime step, with the emulated cycle reached (EmulatedCycles()). Does nothing while not
    // playing. Otherwise: polls the play cursor (counting an underrun if it passed the written end), updates the feedback
    // with Spkr_SubmitWaveBuffer's rule on the fill before writing (fill < 1/4 adds SoundCore_GetErrorInc(), fill > 1/2
    // subtracts it, otherwise the error resets to 0; clamped to +/-SoundCore_GetErrorMax()), then copies the timeline
    // frames not yet written, up to uptoCycle - kLagCycles. Frames that would push the fill past 7/8 of the buffer wait
    // for the next call.
    void Pump(const AudioCapture& capture, uint64_t uptoCycle);
    // The error in cycles (error x CLK / 44100): added to the realtime burst. 0 while not playing.
    int FeedbackCycles() const;
    // true only during realtime runs. false stops the buffer (silence) and resets the feedback.
    void SetPlaying(bool playing);
    // Times the play cursor passed the end of the written data after playback started (the play cursor moved from its
    // first observation in a playing period). A resync on SetPlaying(true) is not an underrun.
    uint64_t Underruns() const;

private:
    struct State;
    explicit AudioOutput(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

}  // namespace harness
