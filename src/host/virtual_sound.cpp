// Virtual sound buffers (D14), and the replacement for AppleWin's Windows/DXSoundBuffer.cpp translation unit.
#include "host/virtual_sound.h"
#include "core/audio_capture.h"

#pragma warning(push, 3)
#include "Windows/DXSoundBuffer.h"
#include "CPU.h"
#include "SoundCore.h"
#pragma warning(pop)

#include <algorithm>
#include <cstring>
#include <span>
#include <string>
#include <utility>

namespace harness {

VirtualSoundBuffer::VirtualSoundBuffer(uint32_t bytes, uint32_t rate, int channels, int voiceId, AudioCapture* capture,
                                       CycleClock clock)
    : bytes_(bytes), rate_(rate), channels_(channels), voiceId_(voiceId), capture_(capture), clock_(std::move(clock)),
      ring_(bytes, 0)
{
}

VirtualSoundBuffer::~VirtualSoundBuffer()
{
    if (capture_)
        capture_->ResetVoice(voiceId_);
}

HRESULT VirtualSoundBuffer::SetCurrentPosition(DWORD) { return S_OK; }

HRESULT VirtualSoundBuffer::GetCurrentPosition(LPDWORD lpdwCurrentPlayCursor, LPDWORD lpdwCurrentWriteCursor)
{
    const uint32_t frameBytes = uint32_t(2 * channels_);
    uint32_t play = 0, write = 0;
    if (bytes_ != 0) {
        play = (commitEnd_ + bytes_ - (bytes_ / 8 * 3) % bytes_) % bytes_;
        play -= play % frameBytes;
        write = (play + frameBytes) % bytes_;
    }
    if (lpdwCurrentPlayCursor)
        *lpdwCurrentPlayCursor = play;
    if (lpdwCurrentWriteCursor)
        *lpdwCurrentWriteCursor = write;
    return S_OK;
}

HRESULT VirtualSoundBuffer::Lock(DWORD dwWriteCursor, DWORD dwWriteBytes, LPVOID* lplpvAudioPtr1, DWORD* lpdwAudioBytes1,
                                 LPVOID* lplpvAudioPtr2, DWORD* lpdwAudioBytes2, DWORD dwFlags)
{
    if (!lplpvAudioPtr1 || !lpdwAudioBytes1)
        return DSERR_INVALIDPARAM;
    if ((dwFlags & DSBLOCK_ENTIREBUFFER) || dwWriteBytes == bytes_) {
        *lplpvAudioPtr1 = ring_.data();
        *lpdwAudioBytes1 = bytes_;
        if (lplpvAudioPtr2) *lplpvAudioPtr2 = nullptr;
        if (lpdwAudioBytes2) *lpdwAudioBytes2 = 0;
        return S_OK;
    }
    if (dwFlags & DSBLOCK_FROMWRITECURSOR) {
        DWORD play = 0;
        GetCurrentPosition(&play, &dwWriteCursor);
    }
    if (dwWriteCursor >= bytes_ || dwWriteBytes > bytes_)
        return DSERR_INVALIDPARAM;
    const DWORD n1 = std::min<DWORD>(dwWriteBytes, bytes_ - dwWriteCursor);
    const DWORD n2 = dwWriteBytes - n1;
    if (n2 != 0 && (!lplpvAudioPtr2 || !lpdwAudioBytes2))
        return DSERR_INVALIDPARAM;
    *lplpvAudioPtr1 = ring_.data() + dwWriteCursor;
    *lpdwAudioBytes1 = n1;
    if (lplpvAudioPtr2) *lplpvAudioPtr2 = n2 ? ring_.data() : nullptr;
    if (lpdwAudioBytes2) *lpdwAudioBytes2 = n2;
    return S_OK;
}

HRESULT VirtualSoundBuffer::Unlock(LPVOID lpvAudioPtr1, DWORD dwAudioBytes1, LPVOID lpvAudioPtr2, DWORD dwAudioBytes2)
{
    const uint64_t total = uint64_t(dwAudioBytes1) + dwAudioBytes2;
    if (total == bytes_) {   // whole-buffer commit: AppleWin re-initialising the buffer (D14 "Capture")
        if (capture_)
            capture_->ResetVoice(voiceId_);
        return S_OK;
    }
    if (total == 0 || !lpvAudioPtr1)
        return S_OK;
    const uint8_t* p1 = static_cast<const uint8_t*>(lpvAudioPtr1);
    if (p1 < ring_.data() || p1 >= ring_.data() + bytes_ || total > bytes_)
        return DSERR_INVALIDPARAM;
    const uint32_t offset = uint32_t(p1 - ring_.data());
    commitEnd_ = uint32_t((offset + total) % bytes_);

    if (!capture_)
        return S_OK;
    scratch_.resize(size_t(total / sizeof(int16_t)));
    uint8_t* out = reinterpret_cast<uint8_t*>(scratch_.data());
    const size_t usable = scratch_.size() * sizeof(int16_t);
    const size_t c1 = std::min<size_t>(dwAudioBytes1, usable);
    std::memcpy(out, p1, c1);
    if (lpvAudioPtr2 && usable > c1)
        std::memcpy(out + c1, lpvAudioPtr2, usable - c1);
    scratch_.resize(scratch_.size() - scratch_.size() % size_t(channels_));   // whole frames only
    const uint64_t now = clock_ ? clock_() : uint64_t(g_nCumulativeCycles);
    capture_->Commit(voiceId_, std::span<const int16_t>(scratch_.data(), scratch_.size()), channels_, now);
    return S_OK;
}

HRESULT VirtualSoundBuffer::Stop()
{
    status_ = 0;
    return S_OK;
}

HRESULT VirtualSoundBuffer::Play(DWORD, DWORD, DWORD dwFlags)
{
    status_ = DSBSTATUS_PLAYING | ((dwFlags & DSBPLAY_LOOPING) ? DSBSTATUS_LOOPING : 0);
    return S_OK;
}

HRESULT VirtualSoundBuffer::SetVolume(LONG lVolume)
{
    volume_ = lVolume;
    return S_OK;
}

HRESULT VirtualSoundBuffer::GetVolume(LONG* lplVolume)
{
    if (!lplVolume)
        return DSERR_INVALIDPARAM;
    *lplVolume = volume_;
    return S_OK;
}

HRESULT VirtualSoundBuffer::GetStatus(LPDWORD lpdwStatus)
{
    if (!lpdwStatus)
        return DSERR_INVALIDPARAM;
    *lpdwStatus = status_;
    return S_OK;
}

HRESULT VirtualSoundBuffer::Restore() { return S_OK; }

}  // namespace harness

// ---- The replacement for AppleWin's Windows/DXSoundBuffer.cpp (D14). Global namespace, as AppleWin declares them. ----
// build/AppleWinCore.vcxproj does not compile DXSoundBuffer.cpp. Nothing here touches DirectSound, so dsound.dll
// (delay-loaded) is never loaded on account of AppleWin's sound.

static bool g_dsAvailable = false;
static UINT g_dsInitRefCount = 0;

// Never used: HarnessFrame::CreateSoundBuffer returns VirtualSoundBuffers.
std::shared_ptr<SoundBuffer> DXSoundBuffer::create(uint32_t, uint32_t, int)
{
    return nullptr;
}

bool DSAvailable()
{
    return g_dsAvailable;
}

std::string GetDefaultAudioDeviceName()
{
    return "harness";
}

// Succeeds in every mode and opens no device.
bool DSInit()
{
    if (g_dsAvailable) {
        ++g_dsInitRefCount;
        return true;
    }
    SetAudioDeviceName(GetDefaultAudioDeviceName());
    g_dsAvailable = true;
    g_dsInitRefCount = 1;
    return true;
}

// As AppleWin's: the last DSUninit makes sound unavailable and stops SoundCore's fade timer.
void DSUninit()
{
    if (!g_dsAvailable || g_dsInitRefCount == 0)
        return;
    if (--g_dsInitRefCount)
        return;
    _ASSERT(g_uNumVoices == 0);
    g_dsAvailable = false;
    SoundCore_StopTimer();
}
