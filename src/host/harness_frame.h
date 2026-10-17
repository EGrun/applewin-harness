#pragma once
#include "StdAfx.h"
#include <windows.h>
#include "FrameBase.h"
#include "core/frame_view.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace harness {

class AudioCapture;

// AppleWin's FrameBase for both display modes (D8). Owns no window and never shows a message box.
class HarnessFrame final : public FrameBase {
public:
    HarnessFrame();
    ~HarnessFrame() override = default;

    // Observers
    uint64_t presentedFrames() const { return presentedFrames_; }
    const std::vector<std::string>& messages() const { return messages_; }
    bool restartRequested() const { return restartRequested_; }

    // Presented frame: VideoPresentScreen() copies the whole video buffer (with borders) into a snapshot, then calls the
    // present hook. The view is laid out like the live buffer (bottom-up BGRA32, negative stride). All zero before the
    // first present.
    FrameView PresentedFrame(bool borders) const;
    void SetPresentHook(std::function<void()> hook);   // called after each snapshot; an empty hook does nothing
    // Copies the live video buffer into the presented-frame snapshot as VideoPresentScreen does, without counting a
    // present or calling the hook: a re-render (restore), not a presented frame.
    void RefreshSnapshot();

    // Sound (D14): CreateSoundBuffer returns a VirtualSoundBuffer in every mode, with voice id 0, 1, 2, ... (a counter per
    // frame). Its commits go to the capture set here (null: nothing is captured) at the clock's cycle (empty: AppleWin's
    // g_nCumulativeCycles). Applies to buffers created after the call.
    void SetAudioCapture(AudioCapture* capture, std::function<uint64_t()> clock = {});
    size_t SoundBuffersCreated() const { return buffersCreated_; }

    void Initialize(bool resetVideoState) override;
    void Destroy() override;

    void FrameDrawDiskLEDS() override {}
    void FrameDrawDiskStatus() override {}
    void FrameRefreshStatus(int) override {}
    void FrameUpdateApple2Type() override {}
    void FrameSetCursorPosByMousePos() override {}

    bool GetFullScreenShowSubunitStatus() override { return false; }
    void SetFullScreenShowSubunitStatus(bool) override {}
    void SetWindowedModeShowDiskiiStatus(bool) override {}
    bool GetBestDisplayResolutionForFullScreen(UINT&, UINT&, UINT = 0, UINT = 0) override { return false; }
    int SetViewportScale(int nNewScale, bool = false) override { return nNewScale; }
    void SetAltEnterToggleFullScreen(bool) override {}

    void SetLoadedSaveStateFlag(const bool) override {}

    void VideoPresentScreen() override;
    void ResizeWindow() override {}

    int FrameMessageBox(LPCSTR lpText, LPCSTR lpCaption, UINT uType) override;
    void GetBitmap(WORD id, LONG cb, LPVOID lpvBits) override;
    std::shared_ptr<NetworkBackend> CreateNetworkBackend(const std::string& interfaceName) override;
    std::shared_ptr<SoundBuffer> CreateSoundBuffer(uint32_t dwBufferSize, uint32_t nSampleRate, int nChannels, const char* pszVoiceName) override;
    BYTE* GetResource(WORD id, LPCSTR lpType, uint32_t expectedSize) override;
    void Restart() override;
    std::string Video_GetScreenShotFolder() const override { return std::string(); }

private:
    std::vector<uint8_t> buffer_;
    uint64_t presentedFrames_ = 0;
    std::vector<std::string> messages_;
    bool restartRequested_ = false;
    std::vector<uint8_t> snapshot_;
    std::function<void()> presentHook_;
    AudioCapture* capture_ = nullptr;
    std::function<uint64_t()> captureClock_;
    size_t buffersCreated_ = 0;   // also the next voice id
};

}  // namespace harness
