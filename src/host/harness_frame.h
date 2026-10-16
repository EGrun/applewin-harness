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

class MonitoredSoundBuffer;

// AppleWin's FrameBase for both display modes (D8). Owns no window and never shows a message box.
class HarnessFrame final : public FrameBase {
public:
    HarnessFrame() = default;
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

    // Sound. Off (the default): CreateSoundBuffer returns nullptr. On: it returns a MonitoredSoundBuffer wrapping
    // DXSoundBuffer::create(...), or nullptr if that returns nullptr.
    void EnableSound(bool enabled);
    void SetSoundMuted(bool muted);        // every buffer created so far, and buffers created later
    uint64_t SoundUnderruns() const;       // sum over all buffers
    size_t SoundBuffersCreated() const;    // non-null buffers returned

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
    bool soundEnabled_ = false;
    bool soundMuted_ = false;
    std::vector<std::shared_ptr<MonitoredSoundBuffer>> soundBuffers_;
};

}  // namespace harness
