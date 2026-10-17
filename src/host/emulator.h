#pragma once
#include "core/frame_view.h"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace harness {

class AudioCapture;

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
// duration. Cycles are EmulatedCycles() values. discarded (ruling D1, docs/specs/m4.md "Restore purity"): the input was
// still pending when LoadSnapshot ran and never took (full) effect; firstCycle and lastCycle are then 0 and meaningless
// (the session reports them as null).
struct InputApplied {
    uint64_t inputId = 0, firstCycle = 0, lastCycle = 0;
    bool discarded = false;
};

// Speed settings (D11; docs/specs/m3.md Part C). machineSpeed is the session speed, applied to AppleWin's g_dwSpeed for realtime steps only (0..40; 10 = authentic,
// 40 = SPEED_MAX, unthrottled).
struct SpeedSettings {
    uint32_t machineSpeed;
    bool enhanceDisk;
    bool fullSpeedOverride;
};

// A slot-6 drive with an image inserted (docs/specs/m4.md, list_disks). drive is 0 or 1 (the API's drive 1 or 2).
struct DiskInfo {
    int drive = 0;
    std::filesystem::path workingPath;   // the file AppleWin has open (the session's working copy)
    bool writeProtected = false;
    std::string format;                  // AppleWin's image type, e.g. "DO", "PO", "NIB", "WOZ"
};

// The harness metadata a checkpoint stores beside AppleWin's save state (docs/specs/m4.md, checkpoint).
struct SnapshotMeta {
    uint64_t emulatedCycles = 0;   // EmulatedCycles() when the snapshot was taken
    uint64_t powerOnOrigin = 0;    // the power-on origin (g_nCumulativeCycles at the last power-on)
    SpeedSettings speed{};         // the session's speed settings when the snapshot was taken
};

class Emulator {
public:
    static Emulator& Instance();

    // Windowed: creates the view-only Window and opens AudioOutput on it (D14; if there is no audio device, prints
    // "warning: no audio device; running without sound" to stderr and runs without playback). Headless: no window and
    // no AudioOutput. Both: virtual sound buffers feed Audio(). Both: host joysticks disabled; disk1 (if set) inserted writable in slot 6 drive 1 before boot. A
    // startup FrameMessageBox (including an insert error) becomes the exception text. Throws std::logic_error if
    // already started.
    void Start(const EmulatorConfig&);
    // Releases every sound voice before DSUninit(), then AudioOutput, then the window. Safe to call twice; no-op if not started.
    void Shutdown();
    void PowerCycle();                   // ResetMachineState() then boot; EmulatedCycles() counts from 0 again
    uint64_t EmulatedCycles() const;     // cycles since the last power-on (D10)

    // Runs exactly frames x NTSC_GetCyclesPerFrame() cycles (more only because of the last instruction), in steps of
    // 1 ms or less (stepped: at speed 10 with enhanced disk access, whatever the settings; ruling C7).
    // Realtime: BurstBudget at EffectiveClockHz() +
    // AudioOutput feedback per step, paced by RealtimePacer (D9, D14); while FullSpeedActive(), unpaced steps of the same
    // size with AppleWin's full-speed video redraws (D11). AudioOutput plays only during a Realtime run, at speed 10 and
    // not at full speed. Returns false if the window was closed (checked after every step).
    bool RunFrames(uint32_t frames, Pacing pacing = Pacing::Stepped);
    bool RunRealtime(std::chrono::milliseconds wallTime);   // paced bursts; same feedback, playback and close rules
    // One open-ended realtime run, a step at a time (the server's Running state): each call runs one paced step of at
    // most 1 ms. The pacing state (BurstBudget, RealtimePacer) and playback persist from one call to the next,
    // so the long-run rate is exact; StopRealtime ends the run (and stops playback), and the next call starts a fresh one. Any
    // other run, PowerCycle and Shutdown also end it. Returns false if the window was closed.
    bool RunRealtimeStep();
    void StopRealtime();   // no-op when no realtime run is open
    // Windowed: dispatches the window's pending messages; false once the window has been closed. Headless, or before
    // Start: true.
    bool PumpWindow();
    // Called on the emulating thread after every presented frame (inside a run), in both display modes; an empty
    // function removes it. Persists across Start and Shutdown. Must not throw.
    void SetPresentObserver(std::function<void()> observer);
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

    // The agent-audio timeline (D14): every sound voice, mixed, by emulated cycle since power-on; cleared by Start and
    // PowerCycle. Fed by the VirtualSoundBuffers AppleWin creates, in every mode.
    const AudioCapture& Audio() const;

    // ---- Speed controls (D11). Session settings: they survive PowerCycle. Stepped runs always use authentic speed and
    // enhanced disk access; speed settings affect realtime runs only (ruling C7). Each throws std::logic_error before
    // Start.
    // The session's machine speed (std::out_of_range above 40). AppleWin's g_dwSpeed is set and SetCurrentCLK6502()
    // called at the start of each realtime step (each stepped step applies 10), only when it differs.
    void SetMachineSpeed(uint32_t speed);
    // AppleWin's UseClockMultiplier conversion to a slider value (never 40), after clamping x to at most 4.0. x must be
    // finite and above 0 (std::invalid_argument otherwise).
    void UseClockMultiplier(double multiplier);
    // GetCardMgr().GetDisk2CardMgr().SetEnhanceDisk(b). Default true. A stepped run turns it on and restores it.
    void SetEnhanceDiskSpeed(bool enable);
    // Harness stand-in for Scroll Lock: forces full speed in realtime runs. Default false.
    void SetFullSpeedOverride(bool enable);
    SpeedSettings Speed() const;
    // SetCurrentCLK6502's clock for the session's speed: g_fCurrentCLK6502 during a realtime run (still the clock of
    // speed 40 as AppleWin sets it; callers report null at 40).
    double EffectiveClockHz() const;
    // Why a realtime step would run at full speed right now, in this order: "machine_speed_max", "override",
    // "disk_motor" (Disk II motor on, enhance on, speaker and Mockingboard silent).
    std::vector<std::string> FullSpeedReasons() const;
    bool FullSpeedActive() const;   // !FullSpeedReasons().empty(): the condition, whether or not a realtime run is stepping
    // True only while an open realtime run has entered full speed (its steps are unpaced right now).
    bool FullSpeedStepping() const;

    // Windowed playback (AudioOutput, D14): its underrun count, and whether it exists (windowed with an audio device).
    uint64_t SoundUnderruns() const;
    bool SoundActive() const;

    // ---- Checkpoints (docs/specs/m4.md; D5, D13). Each throws std::logic_error before Start.
    // Ends any realtime run first (as StopRealtime: full speed and the Mockingboard mute are cleared), then writes
    // AppleWin's save state to stateFile (Snapshot_SetFilename + Snapshot_SaveState). A FrameMessageBox during the save
    // throws std::runtime_error carrying its text. Disks are not copied here (CheckpointStore does that).
    SnapshotMeta SaveSnapshot(const std::filesystem::path& stateFile);
    // Snapshot_LoadState from stateFile, then restore purity in the spec's order: speed settings from meta re-applied
    // (enhance-disk included), srand(1), speaker internals reset (SpkrReset), video scanner phase from the restored
    // cycle, pending input discarded (each id reported once by TakeAppliedInputs with discarded = true) and held keys
    // and buttons released, AudioCapture cleared with every voice reset (audio before meta.emulatedCycles is gone), the
    // presented frame re-rendered from the restored video memory, and EmulatedCycles() == meta.emulatedCycles.
    // The disk images the save state names must already be in place (the caller ejects with EjectAll and restores the
    // copies first); a missing image never opens a dialog. Any FrameMessageBox during the load throws
    // std::runtime_error carrying its text; the machine state is then undefined until PowerCycle.
    void LoadSnapshot(const std::filesystem::path& stateFile, const SnapshotMeta& meta);
    // SHA-256 (64 lowercase hex) over the documented byte layout of docs/specs/m4.md get_state_checksum: CPU
    // registers, memory mode, main and aux RAM with the language-card banks (shadow pages flushed first, as
    // MemSaveSnapshot does), keyboard latch and strobe, video mode, Disk II controller state. Never wall-clock, paths or
    // harness bookkeeping.
    std::string StateChecksum() const;
    // A stepped run of exactly cycles cycles (more only because of the last instruction); the input queue is serviced
    // at each step boundary as in RunFrames. 0 does nothing.
    void RunCycles(uint64_t cycles);
    // The slot-6 drives that hold an image, drive 0 first; empty drives are left out.
    std::vector<DiskInfo> Disks() const;
    // Writes each drive's current track back to its image (FlushCurrentTrack), so the file holds every write.
    void FlushDisks();
    // EjectAll: flushes and ejects both slot-6 drives, so AppleWin releases the files, and remembers what was inserted.
    // ReinsertAll: inserts the remembered images again (same paths, same write protection), for a restore abandoned
    // before LoadSnapshot. Insert errors throw std::runtime_error as InsertDisk does.
    void EjectAll();
    void ReinsertAll();
private:
    Emulator() = default;
};

}  // namespace harness
