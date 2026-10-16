#include "host/monitored_sound_buffer.h"

#include <utility>

namespace harness {

MonitoredSoundBuffer::MonitoredSoundBuffer(std::shared_ptr<SoundBuffer> inner, uint32_t bufferBytes)
    : inner_(std::move(inner)), bufferBytes_(bufferBytes)
{
    LONG v = 0;
    recordedVolume_ = (inner_ && SUCCEEDED(inner_->GetVolume(&v))) ? v : 0;   // 0 = DSBVOLUME_MAX
}

HRESULT MonitoredSoundBuffer::SetCurrentPosition(DWORD dwNewPosition)
{
    return inner_->SetCurrentPosition(dwNewPosition);
}

HRESULT MonitoredSoundBuffer::GetCurrentPosition(LPDWORD lpdwCurrentPlayCursor, LPDWORD lpdwCurrentWriteCursor)
{
    // Always ask the inner buffer for both cursors when tracking, even if the caller passed null for one.
    DWORD play = 0, write = 0;
    const HRESULT hr = inner_->GetCurrentPosition(&play, &write);
    if (lpdwCurrentPlayCursor) *lpdwCurrentPlayCursor = play;
    if (lpdwCurrentWriteCursor) *lpdwCurrentWriteCursor = write;
    if (SUCCEEDED(hr) && tracking_ && !muted_) {
        // Mirrors SoundCore_ValidateAndAlignWriteOffset's unsafe zone (strict comparisons).
        const DWORD off = committedEnd_;
        const bool unsafe = (write > play) ? (off > play && off < write) : (off > play || off < write);
        if (unsafe)
            ++underruns_;
        tracking_ = false;
    }
    return hr;
}

HRESULT MonitoredSoundBuffer::Lock(DWORD dwWriteCursor, DWORD dwWriteBytes, LPVOID* p1, DWORD* n1, LPVOID* p2,
                                   DWORD* n2, DWORD dwFlags)
{
    const HRESULT hr = inner_->Lock(dwWriteCursor, dwWriteBytes, p1, n1, p2, n2, dwFlags);
    if (SUCCEEDED(hr))
        lockOffset_ = (dwFlags & DSBLOCK_ENTIREBUFFER) ? 0 : dwWriteCursor;
    return hr;
}

HRESULT MonitoredSoundBuffer::Unlock(LPVOID p1, DWORD n1, LPVOID p2, DWORD n2)
{
    const HRESULT hr = inner_->Unlock(p1, n1, p2, n2);
    if (SUCCEEDED(hr)) {
        const uint64_t total = uint64_t(n1) + n2;
        if (muted_ || bufferBytes_ == 0 || total >= bufferBytes_) {
            tracking_ = false;
        } else {
            committedEnd_ = DWORD((uint64_t(lockOffset_) + total) % bufferBytes_);
            tracking_ = true;
        }
    }
    return hr;
}

HRESULT MonitoredSoundBuffer::Stop() { return inner_->Stop(); }

HRESULT MonitoredSoundBuffer::Play(DWORD r1, DWORD r2, DWORD flags) { return inner_->Play(r1, r2, flags); }

HRESULT MonitoredSoundBuffer::SetVolume(LONG lVolume)
{
    if (muted_) {
        recordedVolume_ = lVolume;
        return S_OK;
    }
    const HRESULT hr = inner_->SetVolume(lVolume);
    if (SUCCEEDED(hr))
        recordedVolume_ = lVolume;
    return hr;
}

HRESULT MonitoredSoundBuffer::GetVolume(LONG* lplVolume)
{
    if (muted_) {
        if (lplVolume) *lplVolume = recordedVolume_;
        return S_OK;
    }
    return inner_->GetVolume(lplVolume);
}

HRESULT MonitoredSoundBuffer::GetStatus(LPDWORD lpdwStatus) { return inner_->GetStatus(lpdwStatus); }

HRESULT MonitoredSoundBuffer::Restore() { return inner_->Restore(); }

void MonitoredSoundBuffer::SetMuted(bool muted)
{
    if (muted == muted_)
        return;
    muted_ = muted;
    tracking_ = false;
    inner_->SetVolume(muted ? LONG(DSBVOLUME_MIN) : recordedVolume_);
}

uint64_t MonitoredSoundBuffer::Underruns() const { return underruns_; }

}  // namespace harness
