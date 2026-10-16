// Stub (M3 Part B Task 1). Task 3 implements the timeline.
#include "core/audio_capture.h"
#include <stdexcept>
#include <string>

namespace harness {

namespace {
[[noreturn]] void NotImplemented(const char* what)
{
    throw std::logic_error(std::string("AudioCapture::") + what + ": not implemented");
}
}  // namespace

AudioCapture::AudioCapture() = default;

void AudioCapture::Commit(int, std::span<const int16_t>, int, uint64_t) { NotImplemented("Commit"); }
void AudioCapture::ResetVoice(int) { NotImplemented("ResetVoice"); }
uint64_t AudioCapture::NewestCycle() const { NotImplemented("NewestCycle"); }
uint64_t AudioCapture::OldestCycle() const { NotImplemented("OldestCycle"); }
std::vector<int16_t> AudioCapture::Read(uint64_t, uint64_t) const { NotImplemented("Read"); }
void AudioCapture::Clear() { NotImplemented("Clear"); }
uint64_t AudioCapture::FrameToCycle(uint64_t) { NotImplemented("FrameToCycle"); }
uint64_t AudioCapture::CycleToFrame(uint64_t) { NotImplemented("CycleToFrame"); }

}  // namespace harness
