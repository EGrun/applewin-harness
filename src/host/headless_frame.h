#pragma once
#include "StdAfx.h"
#include <windows.h>
#include "FrameBase.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace harness {

// Windowless FrameBase: no UI, no sound. Never shows a message box.
class HeadlessFrame final : public FrameBase {
public:
    HeadlessFrame() = default;
    ~HeadlessFrame() override = default;

    // Observers
    uint64_t presentedFrames() const { return presentedFrames_; }
    const std::vector<std::string>& messages() const { return messages_; }
    bool restartRequested() const { return restartRequested_; }

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
};

}  // namespace harness
