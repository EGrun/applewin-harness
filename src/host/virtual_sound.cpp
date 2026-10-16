// Virtual sound buffers (D14). Stub (M3 Part B Task 1): every VirtualSoundBuffer method fails with E_NOTIMPL until
// Task 2 implements them.
#include "host/virtual_sound.h"
#include "core/audio_capture.h"

#pragma warning(push, 3)
#include "Windows/DXSoundBuffer.h"
#include "SoundCore.h"
#pragma warning(pop)

#include <string>
#include <utility>

// 0 while AppleWin's Windows/DXSoundBuffer.cpp is still compiled into AppleWinCore (it defines the same symbols).
// M3B Task 2 removes that file from build/AppleWinCore.vcxproj and sets this to 1.
#ifndef HARNESS_VIRTUAL_SOUND
#define HARNESS_VIRTUAL_SOUND 0
#endif

namespace harness {

VirtualSoundBuffer::VirtualSoundBuffer(uint32_t bytes, uint32_t rate, int channels, int voiceId, AudioCapture* capture,
                                       CycleClock clock)
    : bytes_(bytes), rate_(rate), channels_(channels), voiceId_(voiceId), capture_(capture), clock_(std::move(clock))
{
}

VirtualSoundBuffer::~VirtualSoundBuffer() = default;

HRESULT VirtualSoundBuffer::SetCurrentPosition(DWORD) { return E_NOTIMPL; }
HRESULT VirtualSoundBuffer::GetCurrentPosition(LPDWORD, LPDWORD) { return E_NOTIMPL; }
HRESULT VirtualSoundBuffer::Lock(DWORD, DWORD, LPVOID*, DWORD*, LPVOID*, DWORD*, DWORD) { return E_NOTIMPL; }
HRESULT VirtualSoundBuffer::Unlock(LPVOID, DWORD, LPVOID, DWORD) { return E_NOTIMPL; }
HRESULT VirtualSoundBuffer::Stop() { return E_NOTIMPL; }
HRESULT VirtualSoundBuffer::Play(DWORD, DWORD, DWORD) { return E_NOTIMPL; }
HRESULT VirtualSoundBuffer::SetVolume(LONG) { return E_NOTIMPL; }
HRESULT VirtualSoundBuffer::GetVolume(LONG*) { return E_NOTIMPL; }
HRESULT VirtualSoundBuffer::GetStatus(LPDWORD) { return E_NOTIMPL; }
HRESULT VirtualSoundBuffer::Restore() { return E_NOTIMPL; }

}  // namespace harness

#if HARNESS_VIRTUAL_SOUND
// ---- The replacement for AppleWin's Windows/DXSoundBuffer.cpp (D14). Global namespace, as AppleWin declares them. ----
// Not compiled yet (see HARNESS_VIRTUAL_SOUND): Task 2 enables it and must check it against the current
// DXSoundBuffer.cpp, adding any other symbol the linker asks for.

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
#endif  // HARNESS_VIRTUAL_SOUND
