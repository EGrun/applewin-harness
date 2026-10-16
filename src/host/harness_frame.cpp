#include "host/harness_frame.h"

#pragma warning(push, 3)
#include "Interface.h"
#pragma warning(pop)

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace harness {

void HarnessFrame::Initialize(bool resetVideoState)
{
    Video& video = GetVideo();
    const size_t bytes = size_t(video.GetFrameBufferWidth()) * video.GetFrameBufferHeight() * 4;
    buffer_.assign(bytes, 0);
    video.Initialize(buffer_.data(), resetVideoState);
}

void HarnessFrame::Destroy()
{
    GetVideo().Destroy();
    buffer_.clear();
    buffer_.shrink_to_fit();
}

void HarnessFrame::VideoPresentScreen()
{
    ++presentedFrames_;
}

int HarnessFrame::FrameMessageBox(LPCSTR lpText, LPCSTR lpCaption, UINT)
{
    std::string msg = std::string(lpCaption ? lpCaption : "") + ": " + (lpText ? lpText : "");
    std::fprintf(stderr, "%s\n", msg.c_str());
    messages_.push_back(std::move(msg));
    return IDOK;
}

void HarnessFrame::GetBitmap(WORD, LONG cb, LPVOID lpvBits)
{
    if (lpvBits && cb > 0)
        std::memset(lpvBits, 0, size_t(cb));
}

std::shared_ptr<NetworkBackend> HarnessFrame::CreateNetworkBackend(const std::string&) { return nullptr; }

std::shared_ptr<SoundBuffer> HarnessFrame::CreateSoundBuffer(uint32_t, uint32_t, int, const char*) { return nullptr; }

// Same logic as Win32Frame::GetResource (external/AppleWin/source/Windows/Win32Frame.cpp).
BYTE* HarnessFrame::GetResource(WORD id, LPCSTR lpType, uint32_t expectedSize)
{
    HRSRC hResInfo = FindResourceA(nullptr, MAKEINTRESOURCEA(id), lpType);
    if (hResInfo == nullptr)
        return nullptr;

    const uint32_t resSize = SizeofResource(nullptr, hResInfo);
    if (resSize != expectedSize)
        return nullptr;

    HGLOBAL hResData = LoadResource(nullptr, hResInfo);
    if (hResData == nullptr)
        return nullptr;

    return static_cast<BYTE*>(LockResource(hResData));   // no unlock needed
}

void HarnessFrame::Restart()
{
    restartRequested_ = true;
}

// Task 2 stubs: the interface is final, the behaviour arrives in a later task.
FrameView HarnessFrame::PresentedFrame(bool) const { throw std::logic_error("not implemented"); }

void HarnessFrame::SetPresentHook(std::function<void()>) { throw std::logic_error("not implemented"); }

void HarnessFrame::EnableSound(bool) { throw std::logic_error("not implemented"); }

void HarnessFrame::SetSoundMuted(bool) { throw std::logic_error("not implemented"); }

uint64_t HarnessFrame::SoundUnderruns() const { throw std::logic_error("not implemented"); }

size_t HarnessFrame::SoundBuffersCreated() const { throw std::logic_error("not implemented"); }

}  // namespace harness
