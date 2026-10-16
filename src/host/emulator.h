#pragma once
#include "core/frame_view.h"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace harness {

enum class DisplayMode { Windowed, Headless };
enum class Pacing { Stepped, Realtime };

struct EmulatorConfig {
    std::filesystem::path configIni;              // absolute; AppleWin settings file (g_sConfigFile)
    DisplayMode display = DisplayMode::Windowed;  // D8 default
    std::filesystem::path disk1;                  // optional; image to insert in slot 6 drive 1 before boot
};

// AppleWin's state is global, so there is exactly one emulator per process.
// Every method except Instance, Start and Shutdown throws std::logic_error before Start.
class Emulator {
public:
    static Emulator& Instance();

    // Windowed: creates the view-only Window and initialises DirectSound (muted). Headless: the M1 setup, no sound and
    // no window. Both: host joysticks disabled; disk1 (if set) inserted writable in slot 6 drive 1 before boot. A
    // startup FrameMessageBox (including an insert error) becomes the exception text. Throws std::logic_error if
    // already started.
    void Start(const EmulatorConfig&);
    // Releases every sound voice before DSUninit(), then destroys the window. Safe to call twice; no-op if not started.
    void Shutdown();
    void PowerCycle();                   // ResetMachineState() then boot; EmulatedCycles() counts from 0 again
    uint64_t EmulatedCycles() const;     // cycles since the last power-on (D10)

    // Runs exactly frames x NTSC_GetCyclesPerFrame() cycles (more only because of the last instruction), in steps of
    // 1 ms or less. Realtime: BurstBudget + speaker feedback per step, paced by RealtimePacer (D9). Sound is unmuted
    // only during a Realtime run. Returns false if the window was closed (checked after every step).
    bool RunFrames(uint32_t frames, Pacing pacing = Pacing::Stepped);
    bool RunRealtime(std::chrono::milliseconds wallTime);   // paced bursts; same feedback, unmute and close rules
    // Types bytes 0x0D or 0x20-0x7E with the emulated keyboard (std::invalid_argument before typing anything
    // otherwise). Waits for the strobe to clear before each byte; throws std::runtime_error("TypeText: keyboard not
    // read") after 600 frames of emulated time. Returns right after queueing the last byte; false if the window closed.
    bool TypeText(std::string_view text, Pacing pacing = Pacing::Stepped);
    // Slot 6, drive 0 or 1 (std::out_of_range otherwise). Insert errors throw std::runtime_error with AppleWin's message.
    void InsertDisk(int drive, const std::filesystem::path& image);
    void EjectDisk(int drive);

    uint64_t PresentedFrames() const;
    FrameView Frame(bool borders = false) const;   // the last presented frame (HarnessFrame::PresentedFrame)
    std::string ReadTextRow(int row) const;  // 40-column text page 1 row 0..23, each byte & 0x7F, as ASCII
    const std::vector<std::string>& Messages() const;  // FrameMessageBox calls so far

    uint64_t SoundUnderruns() const;
    bool SoundActive() const;            // true when windowed and DSInit() succeeded
private:
    Emulator() = default;
};

}  // namespace harness
