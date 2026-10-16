#include "host/harness_frame.h"

#pragma warning(push, 3)
#include "Interface.h"
#pragma warning(pop)

#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "Windows/DXSoundBuffer.h"
#include "host/monitored_sound_buffer.h"

namespace harness {

// Video's full framebuffer (GetFrameBufferWidth/Height are constants: 560+2*20 by 384+2*18, BGRA32). Allocated up front so
// PresentedFrame is valid, and zero, before the first present.
static constexpr size_t kFullWidth = 600, kFullHeight = 420;

HarnessFrame::HarnessFrame() : snapshot_(kFullWidth * kFullHeight * 4, 0) {}

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
    Video& video = GetVideo();
    const uint8_t* live = video.GetFrameBuffer();
    if (live) {
        const size_t bytes = size_t(video.GetFrameBufferWidth()) * video.GetFrameBufferHeight() * 4;
        snapshot_.resize(bytes);   // no-op unless Video's dimensions ever differ from kFullWidth x kFullHeight
        std::memcpy(snapshot_.data(), live, bytes);
    }
    ++presentedFrames_;
    if (presentHook_)
        presentHook_();
}

// Same layout as the live buffer: bottom-up, row 0 of snapshot_ is the bottom row.
FrameView HarnessFrame::PresentedFrame(bool borders) const
{
    Video& video = GetVideo();
    const uint32_t fullW = video.GetFrameBufferWidth();
    const uint32_t fullH = video.GetFrameBufferHeight();
    const int32_t stride = -int32_t(fullW * 4);
    const uint8_t* base = snapshot_.data();
    if (borders)
        return FrameView{base + size_t(fullH - 1) * fullW * 4, fullW, fullH, stride};

    const uint32_t bw = video.GetFrameBufferBorderWidth();
    const uint32_t bh = video.GetFrameBufferBorderHeight();
    const uint8_t* top = base + (size_t(fullH - 1 - bh) * fullW + bw) * 4;
    return FrameView{top, video.GetFrameBufferBorderlessWidth(), video.GetFrameBufferBorderlessHeight(), stride};
}

void HarnessFrame::SetPresentHook(std::function<void()> hook)
{
    presentHook_ = std::move(hook);
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

std::shared_ptr<SoundBuffer> HarnessFrame::CreateSoundBuffer(uint32_t dwBufferSize, uint32_t nSampleRate, int nChannels, const char*)
{
    if (!soundEnabled_)
        return nullptr;
    std::shared_ptr<SoundBuffer> inner =
        innerFactory_ ? innerFactory_(dwBufferSize, nSampleRate, nChannels)
                      : DXSoundBuffer::create(dwBufferSize, nSampleRate, nChannels);
    if (!inner)
        return nullptr;
    auto monitored = std::make_shared<MonitoredSoundBuffer>(std::move(inner), dwBufferSize, underrunTotal_);
    if (soundMuted_)
        monitored->SetMuted(true);
    std::erase_if(soundBuffers_, [](const auto& w) { return w.expired(); });
    soundBuffers_.push_back(monitored);
    ++buffersCreated_;
    return monitored;
}

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

void HarnessFrame::EnableSound(bool enabled) { soundEnabled_ = enabled; }

void HarnessFrame::SetSoundMuted(bool muted)
{
    soundMuted_ = muted;
    std::erase_if(soundBuffers_, [](const auto& w) { return w.expired(); });
    for (auto& w : soundBuffers_)
        if (auto b = w.lock())
            b->SetMuted(muted);
}

void HarnessFrame::SetInnerSoundBufferFactory(std::function<std::shared_ptr<SoundBuffer>(uint32_t, uint32_t, int)> f)
{
    innerFactory_ = std::move(f);
}

uint64_t HarnessFrame::SoundUnderruns() const { return *underrunTotal_; }

size_t HarnessFrame::SoundBuffersCreated() const { return buffersCreated_; }

}  // namespace harness
