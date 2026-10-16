#pragma once
#include "core/frame_view.h"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
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
// Input queue (spec: docs/specs/m3.md, Part A "Input queue"; D10).
enum class ArrowKey { Left, Right, Up, Down };   // KeybQueueKeypress(VK_LEFT..VK_DOWN, NOT_ASCII)
struct ResetKey {};                               // CTRL-RESET: AppleWin's CtrlReset(); does not wait for the strobe
// An ASCII code 0..127 (sent with KeybQueueKeypress(code, ASCII)), an arrow key, or RESET.
using KeySpec = std::variant<uint8_t, ArrowKey, ResetKey>;

// Joystick 0 through AppleWin's mouse-as-joystick seam (J0C_MOUSE). Fields left out keep their values; positions
// start at 127 (centre).
struct JoystickChange {
    std::optional<uint8_t> x, y;
    std::optional<bool> button0, button1;
};

// An input that has fully taken effect. type_text: the cycles at which the first and the last key were applied.
// press_key and set_joystick: the press (or change) and the release (or restore); equal when there is no hold or
// duration. Cycles are EmulatedCycles() values.
struct InputApplied {
    uint64_t inputId = 0, firstCycle = 0, lastCycle = 0;
};

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

    // ---- Input queue. Every run (RunFrames, RunRealtime, TypeText) applies the events that are due at each step
    // boundary, so input queued between runs takes effect at the start of the next step (D10). Each Queue* call
    // returns a new input id (unique, increasing, never 0). PowerCycle discards queued input (and releases held keys).
    // Keys are first in, first out, and each key also waits until the keyboard strobe is clear. Text: bytes 0x0D and
    // 0x20-0x7E (std::invalid_argument before queueing anything otherwise). Key i of the text is due at the later of
    // the previous key's applied cycle plus interKeyDelayMs (converted at the base clock, CLK_6502_NTSC) and the
    // moment the strobe clears; with 0 this is TypeText's behaviour.
    uint64_t QueueText(std::string_view text, uint32_t interKeyDelayMs);
    // One key. While it is held the //e any-key-down flag ($C010 bit 7) is set: KeybAnyKeyDown(WM_KEYDOWN, vk) when
    // applied and WM_KEYUP holdMs later (vk: the key's own VK where one exists, VK_SPACE otherwise). An ASCII code
    // above 127 throws std::invalid_argument.
    uint64_t QueueKey(KeySpec key, uint32_t holdMs);
    // JoySetPosition / JoySetButton. durationMs > 0 queues a restore of the previous state durationMs later.
    uint64_t QueueJoystick(JoystickChange change, uint32_t durationMs);
    // The cycle at which the input takes effect when that is already known: once applied, its first cycle; before
    // that, EmulatedCycles() when nothing is queued ahead of it and, for a key, the strobe is clear (it then applies
    // at the start of the next step). Otherwise, or for an unknown id, nullopt.
    std::optional<uint64_t> ScheduledCycle(uint64_t inputId) const;
    // The inputs that have fully taken effect since the last call, in the order they completed.
    std::vector<InputApplied> TakeAppliedInputs();
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
