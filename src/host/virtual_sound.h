#pragma once
// Virtual sound buffers (spec: docs/specs/m3.md, D14). AppleWin never gets a real DirectSound buffer: every
// SoundBuffer it creates is a VirtualSoundBuffer, and every commit feeds the AudioCapture timeline.
#include "StdAfx.h"
#include <windows.h>
#include "SoundBuffer.h"
#include <cstdint>
#include <functional>
#include <vector>

namespace harness {

class AudioCapture;

// A memory ring of `bytes` bytes holding int16 frames of `channels` channels at `rate` Hz (frame = channels x 2 bytes).
// - Lock / Unlock behave like DirectSound's: Lock(offset, n) returns the region [offset, offset + n) of the ring, split
//   in two at the end of the ring; DSBLOCK_ENTIREBUFFER (or n == bytes) locks the whole ring from 0. Offsets and sizes
//   outside the ring return DSERR_INVALIDPARAM.
// - Commit: an Unlock of the region the last Lock returned. Its end, (offset + n1 + n2) mod bytes, becomes the commit
//   end. It starts at 0, so the play cursor starts at 5/8 of the ring and AppleWin's first offset (play + 3/8) is 0.
// - Cursors: GetCurrentPosition reports play = (commit end - bytes x 3/8) mod bytes, rounded down to a whole frame,
//   and write = the commit end (= play + 3/8; ruling B6). AppleWin therefore always sees 3/8 of the buffer queued: its
//   SoundCore_ValidateAndAlignWriteOffset check never fires (its offset equals write, and the check is strict) and its
//   sample errors stay 0 (g_nCpuCyclesFeedback == 0, and the Mockingboard's and SSI263's corrections), even for a voice
//   whose first write starts at the write cursor.
// - Unlock accepts only (a prefix of) the region the last Lock returned; anything else returns DSERR_INVALIDPARAM and
//   changes nothing.
// - Capture: a commit of n1 + n2 bytes (other than a whole-buffer commit) calls
//   capture->Commit(voiceId, samples, channels, clock()) with the committed samples in order. A whole-buffer commit is
//   AppleWin re-initialising the buffer: it is not captured, it doesn't move the commit end, and it calls
//   capture->ResetVoice(voiceId). capture may be null (nothing is captured).
// - Play, Stop, SetCurrentPosition, Restore: S_OK, no effect on the cursors. SetVolume/GetVolume store and return a
//   volume (initially DSBVOLUME_MAX); it does not affect capture. GetStatus: DSBSTATUS_PLAYING | DSBSTATUS_LOOPING
//   after Play(..., DSBPLAY_LOOPING), DSBSTATUS_PLAYING after Play otherwise, 0 before Play and after Stop.
// - The destructor calls capture->ResetVoice(voiceId).
// clock returns the current emulated cycle on the capture's timeline (cycles since power-on); empty means AppleWin's
// g_nCumulativeCycles. HarnessFrame passes the Emulator's EmulatedCycles.
class VirtualSoundBuffer final : public SoundBuffer {
public:
    using CycleClock = std::function<uint64_t()>;

    VirtualSoundBuffer(uint32_t bytes, uint32_t rate, int channels, int voiceId, AudioCapture* capture,
                       CycleClock clock = {});
    ~VirtualSoundBuffer() override;

    HRESULT SetCurrentPosition(DWORD dwNewPosition) override;
    HRESULT GetCurrentPosition(LPDWORD lpdwCurrentPlayCursor, LPDWORD lpdwCurrentWriteCursor) override;

    HRESULT Lock(DWORD dwWriteCursor, DWORD dwWriteBytes, LPVOID* lplpvAudioPtr1, DWORD* lpdwAudioBytes1,
                 LPVOID* lplpvAudioPtr2, DWORD* lpdwAudioBytes2, DWORD dwFlags) override;
    HRESULT Unlock(LPVOID lpvAudioPtr1, DWORD dwAudioBytes1, LPVOID lpvAudioPtr2, DWORD dwAudioBytes2) override;

    HRESULT Stop() override;
    HRESULT Play(DWORD dwReserved1, DWORD dwReserved2, DWORD dwFlags) override;

    HRESULT SetVolume(LONG lVolume) override;
    HRESULT GetVolume(LONG* lplVolume) override;

    HRESULT GetStatus(LPDWORD lpdwStatus) override;
    HRESULT Restore() override;

    uint32_t Bytes() const { return bytes_; }
    uint32_t Rate() const { return rate_; }
    int Channels() const { return channels_; }
    int VoiceId() const { return voiceId_; }

private:
    uint32_t bytes_;
    uint32_t rate_;
    int channels_;
    int voiceId_;
    AudioCapture* capture_;
    CycleClock clock_;
    std::vector<uint8_t> ring_;
    uint32_t commitEnd_ = 0;   // byte offset in the ring of the end of the last (partial) commit
    uint32_t lockOffset_ = 0;  // the region the last Lock returned (lockN1_ + lockN2_ == 0: none outstanding)
    DWORD lockN1_ = 0;
    DWORD lockN2_ = 0;
    LONG volume_ = DSBVOLUME_MAX;
    DWORD status_ = 0;
    std::vector<int16_t> scratch_;   // the samples of one commit, in order
};

}  // namespace harness

// virtual_sound.cpp is also the replacement for AppleWin's Windows/DXSoundBuffer.cpp translation unit (D14), which
// build/AppleWinCore.vcxproj no longer compiles: DSInit() (succeeds, opens no device, never touches DirectSound),
// DSUninit(), DSAvailable(), GetDefaultAudioDeviceName() ("harness") and DXSoundBuffer::create (returns null). They are
// declared by AppleWin's Windows/DXSoundBuffer.h and SoundBuffer.h.
