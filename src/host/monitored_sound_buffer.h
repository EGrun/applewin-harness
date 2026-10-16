#pragma once
#include "StdAfx.h"
#include <windows.h>
#include "SoundBuffer.h"
#include <cstdint>
#include <memory>

namespace harness {

// Wraps another SoundBuffer of a known size and forwards every call to it, except:
// - Muting: SetMuted(true) sets the inner volume to DSBVOLUME_MIN. While muted, SetVolume(v) only records v and
//   GetVolume returns the recorded value. SetMuted(false) restores the recorded volume. The recorded volume starts as
//   the inner buffer's volume at construction. Buffers start unmuted.
// - Underruns: tracks the end of the last committed write (Lock offset + matching Unlock bytes, modulo bufferBytes).
//   A successful GetCurrentPosition while tracking counts one underrun if that end lies in the unsafe zone between the
//   play and write cursors (the predicate of SoundCore_ValidateAndAlignWriteOffset); tracking then stops until the next
//   commit. Tracking is off at construction, while muted, after unmuting, and after a whole-buffer commit; each waits
//   for the next partial commit.
// - Arming (ruling R11): no underrun is counted until playback has visibly begun, i.e. until a successful
//   GetCurrentPosition returns a play cursor different from the one returned by the first successful call. DirectSound
//   holds the cursors still after Play() until the device starts the buffer. Once armed, the buffer stays armed for its
//   life (muting doesn't disarm). Polls before arming leave tracking unchanged.
class MonitoredSoundBuffer final : public SoundBuffer {
public:
    // sharedUnderruns (optional) is incremented alongside the per-buffer count, so a total can outlive the buffer.
    MonitoredSoundBuffer(std::shared_ptr<SoundBuffer> inner, uint32_t bufferBytes,
                         std::shared_ptr<uint64_t> sharedUnderruns = nullptr);
    ~MonitoredSoundBuffer() override = default;

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

    void SetMuted(bool muted);
    uint64_t Underruns() const;

private:
    std::shared_ptr<SoundBuffer> inner_;
    uint32_t bufferBytes_ = 0;
    bool muted_ = false;
    LONG recordedVolume_ = 0;
    bool tracking_ = false;
    bool firstPlaySeen_ = false;
    DWORD firstPlay_ = 0;
    bool armed_ = false;
    DWORD lockOffset_ = 0;
    DWORD committedEnd_ = 0;
    uint64_t underruns_ = 0;
    std::shared_ptr<uint64_t> sharedUnderruns_;
};

}  // namespace harness
