// Stub (M3 Part B Task 1): no device is ever opened. Task 5 implements playback.
#include "host/audio_output.h"
#include "core/audio_capture.h"

namespace harness {

std::unique_ptr<AudioOutput> AudioOutput::Open(HWND)
{
    return nullptr;
}

void AudioOutput::Pump(const AudioCapture&, uint64_t) {}
int AudioOutput::FeedbackCycles() const { return 0; }
void AudioOutput::SetPlaying(bool) {}
uint64_t AudioOutput::Underruns() const { return 0; }

}  // namespace harness
