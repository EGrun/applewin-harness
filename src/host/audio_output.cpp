// Windowed playback (spec: docs/specs/m3.md, D14 "Playback"). dsound.dll is delay-loaded (Harness.props), so it is
// loaded only when Open runs: windowed starts only.
#include "host/audio_output.h"
#include "core/audio_capture.h"

#pragma warning(push, 3)
#include "StdAfx.h"
#include <mmsystem.h>
#include <dsound.h>
#include "Core.h"
#include "SoundCore.h"
#pragma warning(pop)

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace harness {

namespace {

constexpr uint32_t kFrameBytes = sizeof(int16_t) * kAudioChannels;   // 4
// AppleWin's g_dwDSSpkrBufferSize (Speaker.cpp): MAX_SAMPLES x sizeof(short) x 2 channels = 65,536 bytes (~372 ms).
constexpr uint32_t kBufferBytes = MAX_SAMPLES * sizeof(short) * kAudioChannels;
constexpr uint32_t kBufferFrames = kBufferBytes / kFrameBytes;
constexpr uint32_t kStartBytes = (kBufferBytes / 8) * 3;   // write position after a resync: 3/8 ahead of play
constexpr uint32_t kMaxFillBytes = kBufferBytes - kBufferBytes / 8;   // never queue past 7/8 (DirectSound's write
                                                                      // cursor runs ~10-30 ms ahead of play)
constexpr double kBytesPerSecond = double(kAudioSampleRate) * kFrameBytes;

template <typename T>
void Release(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

double QpcSeconds()
{
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart) / freq;
}

}  // namespace

struct AudioOutput::State {
    IDirectSound8* device = nullptr;
    IDirectSoundBuffer* buffer = nullptr;

    bool playing = false;   // SetPlaying
    bool started = false;   // the buffer is playing in this playing period (the first Pump after SetPlaying(true))

    // Byte positions since Open, unwrapped (the buffer offset is the position mod kBufferBytes). fill = written - played.
    uint64_t played = 0;
    uint64_t written = 0;
    DWORD lastPlayCursor = 0;
    double lastPollSeconds = 0.0;

    // Underrun arming (as M2's R11): armed once the play cursor moved from its first observation in a playing period.
    bool armed = false;
    DWORD firstPlayCursor = 0;
    uint64_t underruns = 0;

    // The next timeline frame to write; valid once set by a resync.
    bool haveNextFrame = false;
    uint64_t nextFrame = 0;

    int error = 0;   // Spkr_SubmitWaveBuffer's nNumSamplesError

    ~State()
    {
        if (buffer)
            buffer->Stop();
        Release(buffer);
        Release(device);
    }

    // Copies bytes (interleaved int16) at the written position; null data writes silence.
    bool Write(const int16_t* data, uint32_t bytes)
    {
        if (bytes == 0)
            return true;
        void* p1 = nullptr;
        void* p2 = nullptr;
        DWORD n1 = 0, n2 = 0;
        HRESULT hr = buffer->Lock(DWORD(written % kBufferBytes), bytes, &p1, &n1, &p2, &n2, 0);
        if (hr == DSERR_BUFFERLOST && SUCCEEDED(buffer->Restore()))
            hr = buffer->Lock(DWORD(written % kBufferBytes), bytes, &p1, &n1, &p2, &n2, 0);
        if (FAILED(hr))
            return false;
        if (data) {
            std::memcpy(p1, data, n1);
            if (p2)
                std::memcpy(p2, reinterpret_cast<const uint8_t*>(data) + n1, n2);
        } else {
            std::memset(p1, 0, n1);
            if (p2)
                std::memset(p2, 0, n2);
        }
        buffer->Unlock(p1, n1, p2, n2);
        written += uint64_t(n1) + n2;
        return true;
    }

    // Zeroes the whole buffer (written position unchanged).
    void ZeroAll()
    {
        void* p1 = nullptr;
        void* p2 = nullptr;
        DWORD n1 = 0, n2 = 0;
        HRESULT hr = buffer->Lock(0, 0, &p1, &n1, &p2, &n2, DSBLOCK_ENTIREBUFFER);
        if (hr == DSERR_BUFFERLOST && SUCCEEDED(buffer->Restore()))
            hr = buffer->Lock(0, 0, &p1, &n1, &p2, &n2, DSBLOCK_ENTIREBUFFER);
        if (FAILED(hr))
            return;
        std::memset(p1, 0, n1);
        if (p2)
            std::memset(p2, 0, n2);
        buffer->Unlock(p1, n1, p2, n2);
    }

    // Advances `played` from the play cursor. Polls normally come every ~1 ms; after a stall longer than half the buffer
    // the cursor may have wrapped, so whole laps are recovered from the wall-clock time since the last poll.
    bool Poll()
    {
        DWORD play = 0, writeCursor = 0;
        if (FAILED(buffer->GetCurrentPosition(&play, &writeCursor)))
            return false;
        const double now = QpcSeconds();
        uint64_t delta = (uint64_t(play) + kBufferBytes - lastPlayCursor) % kBufferBytes;
        const double expected = (now - lastPollSeconds) * kBytesPerSecond;
        if (expected > kBufferBytes / 2.0) {
            const double laps = std::floor((expected - double(delta)) / kBufferBytes + 0.5);
            if (laps > 0)
                delta += uint64_t(laps) * kBufferBytes;
        }
        played += delta;
        lastPlayCursor = play;
        lastPollSeconds = now;
        if (!armed && play != firstPlayCursor)
            armed = true;
        return true;
    }

    // Starts the buffer for a playing period: the cursor baseline, and underrun arming starts over.
    void Play()
    {
        DWORD play = 0, writeCursor = 0;
        buffer->GetCurrentPosition(&play, &writeCursor);
        // While stopped the cursor stays put, so the unwrapped positions remain consistent with it.
        lastPlayCursor = play;
        firstPlayCursor = play;
        armed = false;
        lastPollSeconds = QpcSeconds();
        HRESULT hr = buffer->Play(0, 0, DSBPLAY_LOOPING);
        if (hr == DSERR_BUFFERLOST && SUCCEEDED(buffer->Restore()))
            buffer->Play(0, 0, DSBPLAY_LOOPING);
    }

    // Silence, and the write position 3/8 ahead of the play cursor (AppleWin's first-call position).
    void Resync(uint64_t targetFrame)
    {
        ZeroAll();
        written = played + kStartBytes;
        nextFrame = targetFrame;
        haveNextFrame = true;
    }

    // True if frames [nextFrame, targetFrame) can be written as a continuation: the timeline did not move back (power
    // cycle), is not more than a buffer ahead (a stall or a stepped run in between), and is still in the capture's ring.
    bool Continues(const AudioCapture& capture, uint64_t targetFrame) const
    {
        return haveNextFrame && targetFrame >= nextFrame && targetFrame - nextFrame <= kBufferFrames &&
               AudioCapture::FrameToCycle(nextFrame) >= capture.OldestCycle();
    }
};

AudioOutput::AudioOutput(std::unique_ptr<State> state) : state_(std::move(state)) {}

AudioOutput::~AudioOutput() = default;

std::unique_ptr<AudioOutput> AudioOutput::Open(HWND window)
{
    auto s = std::make_unique<State>();
    if (FAILED(DirectSoundCreate8(nullptr, &s->device, nullptr)) || !s->device)
        return nullptr;
    if (FAILED(s->device->SetCooperativeLevel(window, DSSCL_NORMAL)))   // as AppleWin's DSInit
        return nullptr;

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = WORD(kAudioChannels);
    format.nSamplesPerSec = kAudioSampleRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = WORD(kFrameBytes);
    format.nAvgBytesPerSec = kAudioSampleRate * kFrameBytes;

    DSBUFFERDESC desc{};
    desc.dwSize = sizeof(desc);
    // GLOBALFOCUS (AppleWin uses STICKYFOCUS): the view-only window is usually not the foreground window.
    desc.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS;
    desc.dwBufferBytes = kBufferBytes;
    desc.lpwfxFormat = &format;
    if (FAILED(s->device->CreateSoundBuffer(&desc, &s->buffer, nullptr)) || !s->buffer)
        return nullptr;
    s->ZeroAll();
    return std::unique_ptr<AudioOutput>(new AudioOutput(std::move(s)));
}

void AudioOutput::Pump(const AudioCapture& capture, uint64_t uptoCycle)
{
    State& s = *state_;
    if (!s.playing)
        return;
    const uint64_t targetFrame = AudioCapture::CycleToFrame(uptoCycle > kLagCycles ? uptoCycle - kLagCycles : 0);

    if (!s.started) {
        // Continue the queued audio if this run carries on the same timeline; otherwise resync (not an underrun).
        if (!s.Continues(capture, targetFrame))
            s.Resync(targetFrame);
        s.Play();
        s.started = true;
    }
    if (!s.Poll())
        return;

    // Underrun: the play cursor passed the end of the written data.
    if (s.played > s.written) {
        if (s.armed)
            ++s.underruns;
        s.Resync(s.haveNextFrame ? s.nextFrame : targetFrame);
    }

    // Feedback (Spkr_SubmitWaveBuffer's rule) on the fill before this write.
    const uint64_t fill = s.written - s.played;
    const int inc = SoundCore_GetErrorInc();
    if (fill < kBufferBytes / 4)
        s.error += inc;
    else if (fill > kBufferBytes / 2)
        s.error -= inc;
    else
        s.error = 0;
    const int maxError = SoundCore_GetErrorMax();
    s.error = std::clamp(s.error, -maxError, maxError);

    // New timeline frames.
    if (!s.Continues(capture, targetFrame)) {
        s.nextFrame = targetFrame;   // the timeline jumped (cannot happen within a run; defensive): skip ahead
        s.haveNextFrame = true;
        return;
    }
    const uint64_t room = fill < kMaxFillBytes ? (kMaxFillBytes - fill) / kFrameBytes : 0;
    const uint64_t frames = (std::min)(targetFrame - s.nextFrame, room);
    if (frames == 0)
        return;
    std::vector<int16_t> samples = capture.Read(AudioCapture::FrameToCycle(s.nextFrame),
                                                AudioCapture::FrameToCycle(s.nextFrame + frames));
    samples.resize(size_t(frames) * kAudioChannels, 0);   // past the capture's newest cycle: silence
    if (s.Write(samples.data(), uint32_t(frames * kFrameBytes)))
        s.nextFrame += frames;
}

int AudioOutput::FeedbackCycles() const
{
    const State& s = *state_;
    if (!s.playing)
        return 0;
    return int(double(s.error) * g_fCurrentCLK6502 / double(kAudioSampleRate));
}

void AudioOutput::SetPlaying(bool playing)
{
    State& s = *state_;
    if (playing == s.playing)
        return;
    s.playing = playing;
    if (!playing) {
        if (s.started) {
            s.buffer->Stop();   // silence; the queued audio waits for the next playing period
            s.Poll();           // account for what played up to the stop
        }
        s.started = false;
        s.error = 0;
    }
}

uint64_t AudioOutput::Underruns() const
{
    return state_->underruns;
}

}  // namespace harness
