#pragma once
#include "StdAfx.h"
#include <windows.h>
#include "Common.h"          // eApple2Type
#include "core/frame_view.h"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace harness {

struct EmulatorConfig {
    std::filesystem::path configIni;               // AppleWin settings file (assigned to g_sConfigFile)
    eApple2Type model = A2TYPE_APPLE2EENHANCED;
};

// AppleWin's state is global, so there is exactly one emulator per process.
// Every method except Instance, Start and Shutdown throws std::logic_error before Start.
class Emulator {
public:
    static Emulator& Instance();
    void Start(const EmulatorConfig&);   // headless init + boot; throws std::logic_error if already started
    void Shutdown();                     // safe to call twice; no-op if not started
    void PowerCycle();                   // ResetMachineState() then boot
    void RunFrames(uint32_t frames);     // fast, video on
    void RunRealtime(std::chrono::milliseconds wallTime);  // paced bursts
    uint64_t EmulatedCycles() const;     // g_nCumulativeCycles
    uint64_t PresentedFrames() const;
    FrameView Frame(bool borders = false) const;
    std::string ReadTextRow(int row) const;  // 40-column text page 1 row 0..23, each byte & 0x7F, as ASCII
    const std::vector<std::string>& Messages() const;  // FrameMessageBox calls so far
private:
    Emulator() = default;
};

}  // namespace harness
