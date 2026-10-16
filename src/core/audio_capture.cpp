// The agent-audio timeline; see audio_capture.h for the placement rules.
#include "core/audio_capture.h"
#include <algorithm>
#include <stdexcept>
#include <unordered_map>

namespace harness {

namespace {
struct Voice {
    uint64_t prevEnd = 0;   // endCycle of the voice's latest commit
    bool hasPrev = false;
};

int16_t Saturate(int v) { return int16_t(std::clamp(v, -32768, 32767)); }
}  // namespace

struct AudioCapture::State {
    std::vector<int16_t> ring;                // interleaved stereo; frame k lives at slot k % kAudioRingFrames
    uint64_t highFrame = 0;                   // timeline frames [0, highFrame) exist (ring holds the newest kAudioRingFrames)
    uint64_t newestCycle = 0;
    std::unordered_map<int, Voice> voices;
    std::unordered_map<int, uint32_t> rates;   // per-voice sample rate (default kAudioSampleRate); survives Clear
};

AudioCapture::AudioCapture() : state_(std::make_unique<State>())
{
    state_->ring.assign(size_t(kAudioRingFrames) * 2, 0);
}
AudioCapture::~AudioCapture() = default;

uint64_t AudioCapture::FrameToCycle(uint64_t frame)
{
    // ceil(frame x 203125 / 8778) in 128-bit-safe form: frame < 2^47 keeps the product inside 64 bits only up to ~2^46;
    // split to stay exact beyond that.
    const uint64_t q = frame / kCyclesPerFrameDen, r = frame % kCyclesPerFrameDen;
    return q * kCyclesPerFrameNum + (r * kCyclesPerFrameNum + kCyclesPerFrameDen - 1) / kCyclesPerFrameDen;
}

uint64_t AudioCapture::CycleToFrame(uint64_t cycle)
{
    const uint64_t q = cycle / kCyclesPerFrameNum, r = cycle % kCyclesPerFrameNum;
    return q * kCyclesPerFrameDen + (r * kCyclesPerFrameDen) / kCyclesPerFrameNum;
}

void AudioCapture::Commit(int voiceId, std::span<const int16_t> in, int channels, uint64_t endCycle)
{
    if (channels != 1 && channels != 2) throw std::invalid_argument("AudioCapture::Commit: channels must be 1 or 2");
    if (in.size() % size_t(channels) != 0) throw std::invalid_argument("AudioCapture::Commit: partial frame");
    State& s = *state_;
    Voice& v = s.voices[voiceId];
    const uint64_t n = in.size() / size_t(channels);
    const uint64_t f1 = CycleToFrame(endCycle);
    const auto rit = s.rates.find(voiceId);
    const uint64_t rate = rit == s.rates.end() ? kAudioSampleRate : rit->second;
    // The N samples span N x CLK/rate cycles at the voice's own rate: that many cycles, and that many timeline frames.
    const uint64_t spanFrames = (n * kAudioSampleRate + rate / 2) / rate;
    const uint64_t spanCycles = (n * kCyclesPerFrameNum * kAudioSampleRate + kCyclesPerFrameDen * rate - 1) / (kCyclesPerFrameDen * rate);

    uint64_t f0 = 0;
    bool placed = true;
    if (v.hasPrev) {
        if (endCycle <= v.prevEnd) placed = false;
        else {
            // A paused voice: a gap well beyond what N samples need is silence, and the commit is placed like a first one.
            const uint64_t slack = uint64_t(0.1 * kAudioBaseClockHz);
            if (endCycle - v.prevEnd > spanCycles + slack) f0 = f1 > spanFrames ? f1 - spanFrames : 0;
            else f0 = CycleToFrame(v.prevEnd);
        }
    } else {
        f0 = f1 > spanFrames ? f1 - spanFrames : 0;
    }
    v.hasPrev = true;
    v.prevEnd = (std::max)(v.prevEnd, endCycle);
    s.newestCycle = (std::max)(s.newestCycle, endCycle);
    if (!placed) return;

    // Extend the timeline to f1; slots of frames newly in range hold an old lap's data, so zero them.
    if (f1 > s.highFrame) {
        const uint64_t zeroFrom = (std::max)(s.highFrame, f1 > kAudioRingFrames ? f1 - kAudioRingFrames : 0);
        for (uint64_t k = zeroFrom; k < f1; ++k) {
            const size_t slot = size_t(k % kAudioRingFrames) * 2;
            s.ring[slot] = 0;
            s.ring[slot + 1] = 0;
        }
        s.highFrame = f1;
    }
    if (n == 0 || f1 <= f0) return;
    const uint64_t oldest = s.highFrame > kAudioRingFrames ? s.highFrame - kAudioRingFrames : 0;
    const uint64_t m = f1 - f0;
    const uint64_t jStart = f0 < oldest ? oldest - f0 : 0;   // frames older than the ring are dropped

    const size_t ch = size_t(channels);
    for (uint64_t j = jStart; j < m; ++j) {
        // Input position j x N / M, linearly interpolated.
        const uint64_t num = j * n;
        const uint64_t i0 = num / m;
        const int64_t frac = int64_t(num % m);
        // Between input N-1 and the end of the interval there is no next sample: continue the last step's slope.
        const bool tail = i0 + 1 >= n;
        const uint64_t ia = tail ? (n > 1 ? n - 2 : n - 1) : i0;
        const uint64_t ib = tail ? n - 1 : i0 + 1;
        const int64_t num2 = tail ? int64_t(num) - int64_t(ib) * int64_t(m) : frac;   // fraction (of m) from input ib
        int l, r;
        auto interp = [&](size_t c) {
            const int a = in[size_t(ia) * ch + c], b = in[size_t(ib) * ch + c];
            if (ia == ib) return a;
            const int64_t base = tail ? b : a;
            return int(base + (int64_t(b - a) * num2) / int64_t(m));
        };
        l = Saturate(interp(0));
        r = ch == 2 ? Saturate(interp(1)) : l;
        const size_t slot = size_t((f0 + j) % kAudioRingFrames) * 2;
        s.ring[slot] = Saturate(s.ring[slot] + l);
        s.ring[slot + 1] = Saturate(s.ring[slot + 1] + r);
    }
}

void AudioCapture::RegisterVoice(int voiceId, uint32_t sampleRate)
{
    if (sampleRate == 0) throw std::invalid_argument("AudioCapture::RegisterVoice: rate must not be 0");
    state_->rates[voiceId] = sampleRate;
}

void AudioCapture::ResetVoice(int voiceId)
{
    auto it = state_->voices.find(voiceId);
    if (it != state_->voices.end()) it->second = Voice{};
}

uint64_t AudioCapture::NewestCycle() const { return state_->newestCycle; }

uint64_t AudioCapture::OldestCycle() const
{
    const uint64_t h = state_->highFrame;
    return h > kAudioRingFrames ? FrameToCycle(h - kAudioRingFrames) : 0;
}

std::vector<int16_t> AudioCapture::Read(uint64_t fromCycle, uint64_t toCycle) const
{
    if (fromCycle < OldestCycle()) throw std::out_of_range("AudioCapture::Read: before the oldest buffered sample");
    const State& s = *state_;
    const uint64_t f0 = CycleToFrame(fromCycle);
    const uint64_t f1 = (std::min)(CycleToFrame((std::min)(toCycle, s.newestCycle)), s.highFrame);
    std::vector<int16_t> out;
    if (f0 >= f1) return out;
    out.resize(size_t(f1 - f0) * 2);
    for (uint64_t k = f0; k < f1; ++k) {
        const size_t slot = size_t(k % kAudioRingFrames) * 2, o = size_t(k - f0) * 2;
        out[o] = s.ring[slot];
        out[o + 1] = s.ring[slot + 1];
    }
    return out;
}

void AudioCapture::Clear()
{
    state_->highFrame = 0;
    state_->newestCycle = 0;
    state_->voices.clear();
}

}  // namespace harness
