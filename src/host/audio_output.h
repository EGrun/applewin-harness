#pragma once
// Windowed playback of the agent-audio timeline (spec: docs/specs/m3.md, D14 "Playback"). Windowed realtime only;
// headless never creates one, so dsound.dll (delay-loaded) is never loaded headless.
#include <windows.h>
#include <cstdint>
#include <memory>

namespace harness {

class AudioCapture;

// Streams the AudioCapture timeline (44,100 Hz stereo int16) to a harness-owned looping DirectSound buffer the size of
// AppleWin's speaker buffer, writing from 3/8 of the way in.
class AudioOutput {
public:
    // DirectSound on window's default device; null if there is no device or any DirectSound call fails.
    static std::unique_ptr<AudioOutput> Open(HWND window);
    virtual ~AudioOutput() = default;

    // Called after every realtime step: copies the timeline frames not yet written, up to uptoCycle (silence instead
    // while not playing), then updates the feedback with Spkr_SubmitWaveBuffer's rule: fill < 1/4 adds
    // SoundCore_GetErrorInc(), fill > 1/2 subtracts it, otherwise the error resets to 0; clamped to
    // +/-SoundCore_GetErrorMax().
    void Pump(const AudioCapture& capture, uint64_t uptoCycle);
    // The error in cycles (error x CLK / 44100): added to the realtime burst. 0 until Pump measures a fill.
    int FeedbackCycles() const;
    // false (the default): Pump writes silence. true only during realtime runs.
    void SetPlaying(bool playing);
    // Times the play cursor passed the end of the written data after playback started.
    uint64_t Underruns() const;

protected:
    AudioOutput() = default;
};

}  // namespace harness
