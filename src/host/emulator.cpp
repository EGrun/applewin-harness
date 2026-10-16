#include "host/emulator.h"
#include "host/applewin_host.h"
#include "host/harness_frame.h"
#include "host/window.h"
#include "core/pacing.h"

#pragma warning(push, 3)
#include "StdAfx.h"
#include <windows.h>
#include "Common.h"
#include "Interface.h"
#include "Core.h"
#include "CmdLine.h"
#include "CPU.h"
#include "CardManager.h"
#include "Disk.h"
#include "Joystick.h"
#include "Keyboard.h"
#include "Log.h"
#include "Memory.h"
#include "MockingboardCardManager.h"
#include "NTSC.h"
#include "RGBMonitor.h"
#include "Speaker.h"
#include "Utilities.h"
#include "version.h"
#include "Windows/DXSoundBuffer.h"
#pragma warning(pop)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <string>
#include <variant>
#include <vector>

namespace harness {

namespace {

bool g_started = false;
bool g_soundActive = false;   // windowed start, DSInit() succeeded: DirectSound is up and must be DSUninit()ed
std::unique_ptr<Window> g_window;   // windowed mode only
uint64_t g_cyclesAtPowerOn = 0;   // g_nCumulativeCycles at the last power-on (D10)
std::function<void()> g_presentObserver;   // SetPresentObserver

HarnessFrame& HostFrame()
{
    return static_cast<HarnessFrame&>(GetFrame());
}

void RequireStarted()
{
    if (!g_started)
        throw std::logic_error("Emulator: not started");
}

std::string ExeDirWithSeparator()
{
    char path[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameA(nullptr, path, DWORD(sizeof(path)));
    std::string dir(path, n);
    const size_t sep = dir.find_last_of("\\/:");
    dir.resize(sep == std::string::npos ? 0 : sep + 1);
    return dir;
}

Disk2InterfaceCard& Disk2Card()
{
    if (GetCardMgr().QuerySlot(SLOT6) != CT_Disk2)
        throw std::runtime_error("Emulator: no Disk II controller in slot 6");
    return dynamic_cast<Disk2InterfaceCard&>(GetCardMgr().GetRef(SLOT6));
}

// Copy of the static ResetToLogoMode() in Windows/AppleWin.cpp.
void ResetToLogoMode()
{
    g_nAppMode = MODE_LOGO;
    GetFrame().SetLoadedSaveStateFlag(false);
}

// AppleWin's BTN_RUN handler from logo mode (Windows/WinFrame.cpp).
void Boot()
{
    if (GetCardMgr().QuerySlot(SLOT6) == CT_Disk2)
        dynamic_cast<Disk2InterfaceCard&>(GetCardMgr().GetRef(SLOT6)).Boot();
    g_nAppMode = MODE_RUNNING;
}

constexpr uint32_t kTypeTimeoutFrames = 600;   // spec: TypeText gives up after 600 frames of emulated time

// ---- Input queue (spec: docs/specs/m3.md "Input queue"; D10). Cycles here are EmulatedCycles() values.

struct JoyState {
    uint8_t x = 127, y = 127;
    bool b0 = false, b1 = false;
};

struct QueuedOp {
    enum class Kind { Text, Key, Joy } kind = Kind::Key;
    uint64_t id = 0;
    // Text
    std::string text;
    size_t next = 0;
    uint64_t delayCycles = 0;
    uint64_t earliest = 0;      // the next text key is not due before this cycle
    // Key
    KeySpec key{uint8_t(0)};
    uint64_t holdCycles = 0;
    // Joy
    JoystickChange change;
    uint64_t durationCycles = 0;
    // Text and Key: set once the first key has been applied.
    bool started = false;
    uint64_t first = 0;
};

struct TimedOp {
    enum class Kind { KeyUp, JoyRestore } kind = Kind::KeyUp;
    uint64_t id = 0, due = 0, first = 0;
    WPARAM vk = 0;
    JoyState restore;
};

struct InputState {
    std::deque<QueuedOp> fifo;
    std::vector<TimedOp> timed;
    std::vector<InputApplied> applied;           // completed, not yet taken
    std::map<uint64_t, uint64_t> firstCycles;    // id -> cycle of the first effect (bounded)
    JoyState joy;
    uint64_t nextId = 1;
    uint64_t keysApplied = 0;                    // text and key events applied (TypeText's progress counter)
};
InputState g_input;

uint64_t EmulatedNow()
{
    return g_nCumulativeCycles - g_cyclesAtPowerOn;
}

uint64_t MsToBaseCycles(uint32_t ms)
{
    return uint64_t(double(ms) * CLK_6502_NTSC / 1000.0);   // base clock, not the speed-adjusted one
}

bool StrobeClear()
{
    return (KeybReadData() & 0x80) == 0;
}

// The key's own VK where AppleWin's AKD table accepts one, VK_SPACE otherwise.
WPARAM VkForAscii(uint8_t c)
{
    if (c >= 'a' && c <= 'z') return WPARAM(c - 'a' + 'A');
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return WPARAM(c);
    switch (c) {
    case 0x0D: return VK_RETURN;
    case 0x1B: return VK_ESCAPE;
    case 0x09: return VK_TAB;
    case 0x08: return VK_BACK;
    case 0x7F: return VK_DELETE;
    default: return VK_SPACE;
    }
}

WPARAM VkForArrow(ArrowKey a)
{
    switch (a) {
    case ArrowKey::Left: return VK_LEFT;
    case ArrowKey::Right: return VK_RIGHT;
    case ArrowKey::Up: return VK_UP;
    default: return VK_DOWN;
    }
}

void NoteFirst(uint64_t id, uint64_t cycle)
{
    g_input.firstCycles.emplace(id, cycle);
    while (g_input.firstCycles.size() > 4096)
        g_input.firstCycles.erase(g_input.firstCycles.begin());
}

void Complete(uint64_t id, uint64_t first, uint64_t last)
{
    g_input.applied.push_back(InputApplied{id, first, last});
}

void ApplyJoy(const JoyState& s)
{
    g_input.joy = s;
    JoySetPosition(s.x, 255, s.y, 255);
    JoySetButton(BUTTON0, s.b0 ? BUTTON_DOWN : BUTTON_UP);
    JoySetButton(BUTTON1, s.b1 ? BUTTON_DOWN : BUTTON_UP);
}

// Joystick 0 as the mouse-as-joystick seam: no host device is ever read. Centre, buttons up.
void ResetJoystick()
{
    JoySetJoyType(JN_JOYSTICK0, J0C_MOUSE);
    ApplyJoy(JoyState{});
}

// Drops every queued input and releases held keys (PowerCycle, Start, Shutdown).
void ClearInput()
{
    for (const TimedOp& t : g_input.timed)
        if (t.kind == TimedOp::Kind::KeyUp)
            KeybAnyKeyDown(WM_KEYUP, t.vk, false);
    g_input.timed.clear();
    g_input.fifo.clear();
}

// Applies every event that is due at the current step boundary.
void ProcessDueInput()
{
    const uint64_t now = EmulatedNow();

    for (;;) {   // timed releases and restores, earliest first
        auto best = g_input.timed.end();
        for (auto it = g_input.timed.begin(); it != g_input.timed.end(); ++it)
            if (it->due <= now && (best == g_input.timed.end() || it->due < best->due))
                best = it;
        if (best == g_input.timed.end())
            break;
        const TimedOp t = *best;
        g_input.timed.erase(best);
        if (t.kind == TimedOp::Kind::KeyUp)
            KeybAnyKeyDown(WM_KEYUP, t.vk, false);
        else
            ApplyJoy(t.restore);
        Complete(t.id, t.first, now);
    }

    while (!g_input.fifo.empty()) {   // first in, first out: the first event that is not due blocks the rest
        QueuedOp& op = g_input.fifo.front();
        if (op.kind == QueuedOp::Kind::Text) {
            if (op.next >= op.text.size()) {   // empty text
                NoteFirst(op.id, now);
                Complete(op.id, now, now);
                g_input.fifo.pop_front();
                continue;
            }
            if (now < op.earliest || !StrobeClear())
                break;
            KeybQueueKeypress(WPARAM(static_cast<unsigned char>(op.text[op.next])), ASCII);
            ++g_input.keysApplied;
            if (!op.started) {
                op.started = true;
                op.first = now;
                NoteFirst(op.id, now);
            }
            ++op.next;
            op.earliest = now + op.delayCycles;
            if (op.next >= op.text.size()) {
                Complete(op.id, op.first, now);
                g_input.fifo.pop_front();
            }
        } else if (op.kind == QueuedOp::Kind::Key) {
            if (std::holds_alternative<ResetKey>(op.key)) {
                CtrlReset();   // does not wait for the strobe
                // CtrlReset sets g_bFreshReset, which makes KeybQueueKeypress swallow the next CTRL-C (a Win32
                // CTRL-BREAK artefact). Harness input never produces that spurious key, so clear it.
                GetFrame().g_bFreshReset = false;
                NoteFirst(op.id, now);
                Complete(op.id, now, now);
                g_input.fifo.pop_front();
                continue;
            }
            if (!StrobeClear())
                break;
            WPARAM vk;
            if (const ArrowKey* a = std::get_if<ArrowKey>(&op.key)) {
                vk = VkForArrow(*a);
                KeybQueueKeypress(vk, NOT_ASCII);
            } else {
                const uint8_t c = std::get<uint8_t>(op.key);
                vk = VkForAscii(c);
                KeybQueueKeypress(WPARAM(c), ASCII);
            }
            ++g_input.keysApplied;
            NoteFirst(op.id, now);
            if (op.holdCycles > 0) {
                KeybAnyKeyDown(WM_KEYDOWN, vk, false);
                TimedOp t;
                t.kind = TimedOp::Kind::KeyUp;
                t.id = op.id;
                t.due = now + op.holdCycles;
                t.first = now;
                t.vk = vk;
                g_input.timed.push_back(t);
            } else {
                Complete(op.id, now, now);
            }
            g_input.fifo.pop_front();
        } else {   // Joy
            const JoyState previous = g_input.joy;
            JoyState s = previous;
            if (op.change.x) s.x = *op.change.x;
            if (op.change.y) s.y = *op.change.y;
            if (op.change.button0) s.b0 = *op.change.button0;
            if (op.change.button1) s.b1 = *op.change.button1;
            ApplyJoy(s);
            NoteFirst(op.id, now);
            if (op.durationCycles > 0) {
                TimedOp t;
                t.kind = TimedOp::Kind::JoyRestore;
                t.id = op.id;
                t.due = now + op.durationCycles;
                t.first = now;
                t.restore = previous;
                g_input.timed.push_back(t);
            } else {
                Complete(op.id, now, now);
            }
            g_input.fifo.pop_front();
        }
    }
}


// The one step primitive behind RunFrames, RunRealtime and TypeText: executes at most 1 ms of emulated time. It also
// holds the sound rule and the window-closed rule for all three.
// Stepped: period-sized steps, no pacing. Realtime (D9): a BurstBudget burst corrected by the speaker's cycle
// feedback, then wait for the 1 ms tick.
class Stepper {
public:
    explicit Stepper(Pacing pacing)
        : realtime_(pacing == Pacing::Realtime), period_(uint32_t(g_fCurrentCLK6502 * 1.0e-3))
    {
        if (realtime_) {
            budget_.emplace(g_fCurrentCLK6502, std::chrono::milliseconds(1));
            pacer_.emplace();
            pacer_->Start();
        }
    }

    // Runs one step of at most `remaining` cycles. Returns true if the run must stop (window closed).
    bool Step(uint64_t remaining)
    {
        ProcessDueInput();   // input takes effect at the start of the step (D10)
        uint64_t cycles;
        if (realtime_) {
            const int64_t withFeedback = int64_t(budget_->Next()) + g_nCpuCyclesFeedback;
            cycles = withFeedback > 0 ? uint64_t(withFeedback) : 0;
        } else {
            cycles = period_;
        }
        if (cycles > remaining)
            cycles = remaining;
        ExecuteStep(uint32_t(cycles));
        if (realtime_)
            pacer_->WaitForTick();
        return WindowClosed();
    }

private:
    // Window-closed rule (spec): when windowed, pump the window's messages after every step; the run stops once the
    // window has been closed.
    static bool WindowClosed()
    {
        if (!g_window)
            return false;
        g_window->Pump();
        return g_window->Closed();
    }

    // Sound rule (D8): playback is unmuted only for the lifetime of a Realtime run, and muted again when the run
    // returns or throws.
    class RealtimeUnmute {
    public:
        explicit RealtimeUnmute(bool realtime) : active_(realtime && g_soundActive)
        {
            if (active_)
                HostFrame().SetSoundMuted(false);
        }
        ~RealtimeUnmute()
        {
            if (active_)
                HostFrame().SetSoundMuted(true);
        }
        RealtimeUnmute(const RealtimeUnmute&) = delete;
        RealtimeUnmute& operator=(const RealtimeUnmute&) = delete;

    private:
        bool active_;
    };

    bool realtime_;
    uint32_t period_;
    std::optional<BurstBudget> budget_;
    std::optional<RealtimePacer> pacer_;
    RealtimeUnmute unmute_{realtime_};   // after realtime_; if the constructor body throws, its destructor re-mutes
};

// The open-ended realtime run of RunRealtimeStep (null when none is open).
std::unique_ptr<Stepper> g_realtime;

// AppleWin seeds zero-page $4E/$4F (RNDL/RNDH) from rand() ^ timeGetTime() on every power-on (Memory.cpp
// getRandomTime, a Pooyan work-around), whatever the power-on memory pattern. The monitor's key-wait loop increments
// them, so the cycle count of every wait for a key (and so the frame hash) would differ between runs. Pin them, and
// restart the C runtime's rand() sequence (the Disk II uses it for unreadable bits), at every power-on.
void PinPowerOnRandomness()
{
    std::srand(1);
    BYTE* rnd = MemGetMainPtr(0x4E);
    rnd[0] = 0x20;   // AppleWin ORs 0x20 into both (Memory.cpp, "Pooyan" needs them non-zero); pin to that floor
    rnd[1] = 0x20;
}

// Releases every sound voice, DirectSound and the window: the windowed part of Shutdown (also used when a windowed Start
// fails). DSUninit() asserts that no voice is left, so the speaker, every Mockingboard card's SSI263 voices and the
// Mockingboard manager's voice are released first. Harmless in headless mode, and when called again.
void ReleaseSoundAndWindow()
{
    SpkrDestroy();   // speaker voice and buffers (AppleWin calls it from WM_DESTROY); pairs with SpkrInitialize()
    MockingboardCardManager& mbMgr = GetCardMgr().GetMockingboardCardMgr();
    for (UINT slot = SLOT0; slot < NUM_SLOTS; ++slot) {
        // MockingboardCard::Destroy() releases its SSI263 voices. Its cards are replaced by the next Start's
        // LoadConfiguration() (CardManager::Insert always creates a new card), and its destructor tolerates a second call.
        if (mbMgr.IsMockingboard(slot))
            GetCardMgr().GetRef(slot).Destroy();
    }
    mbMgr.Destroy();
    if (g_soundActive) {
        DSUninit();
        g_soundActive = false;
    }
    HarnessFrame& frame = HostFrame();
    frame.SetPresentHook({});
    frame.EnableSound(false);
    frame.SetSoundMuted(false);
    g_window.reset();
    frame.g_hFrameWindow = nullptr;
}

// Windowed start, before the M1 startup (spec): window, g_hFrameWindow, DSInit(), then the sound decision.
void StartWindowAndSound()
{
    HarnessFrame& frame = HostFrame();
    g_window = std::make_unique<Window>(L"AppleWin Harness (view only)");
    frame.g_hFrameWindow = g_window->Handle();
    if (DSInit()) {
        g_bDisableDirectSound = false;
        frame.EnableSound(true);
        frame.SetSoundMuted(true);   // unmuted only during Realtime runs (D8)
        g_soundActive = true;
    } else {
        std::cerr << "warning: no audio device; running without sound" << std::endl;
        g_bDisableDirectSound = true;
    }
}

}  // namespace

Emulator& Emulator::Instance()
{
    static Emulator instance;
    return instance;
}

void Emulator::Start(const EmulatorConfig& config)
{
    if (g_started)
        throw std::logic_error("Emulator: already started");
    if (config.configIni.empty() || !config.configIni.is_absolute())
        throw std::invalid_argument("Emulator: configIni must be an absolute path");

    // 1. Directories, config file (INI-only mode in Registry.cpp) and deterministic power-on memory.
    const std::string exeDir = ExeDirWithSeparator();
    g_sStartDir = exeDir;
    g_sProgramDir = exeDir;
    g_sConfigFile = config.configIni.string();
    g_nMemoryClearType = MIP_ZERO;
    // Sound. Headless: none (as AppleWin's -no-dsound switch); speaker and Mockingboard skip DirectSound and never show
    // the "unable to initialize a waveform output device" message box. Windowed: DirectSound if DSInit() succeeds.
    g_bDisableDirectSound = true;
    // Any exception out of Start leaves the emulator stopped (failures after g_started go through Shutdown()); this
    // also releases a window and DirectSound set up before the failure.
    struct ReleaseIfNotStarted {
        ~ReleaseIfNotStarted() { if (!g_started) ReleaseSoundAndWindow(); }
    } releaseIfNotStarted;
    if (config.display == DisplayMode::Windowed)
        StartWindowAndSound();
    // After each present: show the frame in the window (windowed), then the observer (both modes).
    HostFrame().SetPresentHook([] {
        if (g_window) {
            g_window->Show(HostFrame().PresentedFrame(false));
            g_window->Pump();
        }
        if (g_presentObserver)
            g_presentObserver();
    });

    // 2. Version (AppleWin reads it from its exe's version resource; the harness uses version.h directly).
    SetAppleWinVersion(APPLEWIN_VERSION);

    const size_t messagesBefore = HostFrame().messages().size();

    // 3. RepeatInitialization, headless subset.
    KeybReset();
    GetVideo().SetVidHD(false);
    ResetToLogoMode();
    LoadConfiguration(true);
    SetApple2Type(A2TYPE_APPLE2EENHANCED);
    // Host joysticks are never read (determinism): port 1 disabled, port 0 is the mouse-as-joystick seam (ResetJoystick).
    JoySetJoyType(JN_JOYSTICK0, J0C_MOUSE);   // mouse-as-joystick seam: driven by the input queue, no host device
    JoySetJoyType(JN_JOYSTICK1, J1C_DISABLED);
    JoyInitialize();
    ClearInput();
    g_input.nextId = 1;
    g_input.applied.clear();
    g_input.firstCycles.clear();
    ResetJoystick();
    VideoSwitchVideocardPalette(RGB_GetVideocard(), GetVideo().GetVideoType());
    GetFrame().Initialize(true);
    // AppleWin calls SpkrInitialize() from WM_CREATE, after DSInit() (here: after the sound decision above). Without it
    // the speaker sample buffer is null and the first SpkrUpdate() crashes.
    SpkrInitialize();
    // DebugInitialize() is deliberately NOT called (the spec lists it as skipped): its first statement,
    // GetConsoleFontDC(), calls Win32Frame::GetWin32Frame(), which throws std::bad_cast for a HarnessFrame. Its
    // remaining work (debugger console, windows, colours, fonts, breakpoint tables already zero-initialised) is
    // debugger-UI state that the M1 run path does not use.
    MemInitialize();
    GetCardMgr().Reset(true);
    PinPowerOnRandomness();

    g_started = true;

    const auto& msgs = HostFrame().messages();
    if (msgs.size() != messagesBefore) {
        std::string what = "Emulator: FrameMessageBox during startup:";
        for (size_t i = messagesBefore; i < msgs.size(); ++i)
            what += "\n  " + msgs[i];
        Shutdown();
        throw std::runtime_error(what);
    }

    // 4. Disk, then boot.
    if (!config.disk1.empty()) {
        try { InsertDisk(0, config.disk1); }
        catch (...) { Shutdown(); throw; }
    }
    Boot();
    g_cyclesAtPowerOn = g_nCumulativeCycles;
}

void Emulator::Shutdown()
{
    if (!g_started)
        return;
    g_realtime.reset();
    ClearInput();
    // Ruling R10: Shutdown owns releasing the disk images; otherwise AppleWin keeps the image files open.
    if (GetCardMgr().QuerySlot(SLOT6) == CT_Disk2) {
        Disk2InterfaceCard& card = Disk2Card();
        card.EjectDisk(DRIVE_1);
        card.EjectDisk(DRIVE_2);
    }
    GetFrame().Destroy();
    ReleaseSoundAndWindow();   // every sound voice, then DSUninit() (windowed), then the window
    LogDone();
    g_started = false;
}

void Emulator::PowerCycle()
{
    RequireStarted();
    // ResetMachineState() already boots the Disk II card (Utilities.cpp); a second Boot() would load a disk image twice.
    g_realtime.reset();
    ClearInput();   // discards queued input and releases held keys
    ResetMachineState();
    PinPowerOnRandomness();
    g_nAppMode = MODE_RUNNING;
    g_cyclesAtPowerOn = g_nCumulativeCycles;
    ResetJoystick();
}

bool Emulator::RunFrames(uint32_t frames, Pacing pacing)
{
    RequireStarted();
    g_realtime.reset();
    if (frames == 0)
        return true;
    Stepper stepper(pacing);   // Realtime: sound unmuted for this scope
    const uint64_t target = g_nCumulativeCycles + uint64_t(frames) * NTSC_GetCyclesPerFrame();
    while (g_nCumulativeCycles < target) {
        if (stepper.Step(target - g_nCumulativeCycles))
            return false;
    }
    return true;
}

bool Emulator::RunRealtime(std::chrono::milliseconds wallTime)
{
    RequireStarted();
    g_realtime.reset();
    using clock = std::chrono::steady_clock;
    // The pacer catches up short stalls and drops time after long ones, so stop on elapsed wall-clock time (not a tick
    // count) to keep the run length honest.
    Stepper stepper(Pacing::Realtime);   // sound unmuted for this scope
    const auto start = clock::now();
    while (clock::now() - start < wallTime) {
        if (stepper.Step(UINT64_MAX))
            return false;
    }
    return true;
}

bool Emulator::RunRealtimeStep()
{
    RequireStarted();
    if (!g_realtime)
        g_realtime = std::make_unique<Stepper>(Pacing::Realtime);   // fresh pacing; sound unmuted until it ends
    if (g_realtime->Step(UINT64_MAX)) {
        g_realtime.reset();
        return false;
    }
    return true;
}

void Emulator::StopRealtime()
{
    g_realtime.reset();
}

bool Emulator::PumpWindow()
{
    if (!g_started || !g_window)
        return true;
    return g_window->Pump();
}

void Emulator::SetPresentObserver(std::function<void()> observer)
{
    g_presentObserver = std::move(observer);
}

uint64_t Emulator::EmulatedCycles() const
{
    RequireStarted();
    return g_nCumulativeCycles - g_cyclesAtPowerOn;
}

bool Emulator::TypeText(std::string_view text, Pacing pacing)
{
    RequireStarted();
    for (const char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u != 0x0D && (u < 0x20 || u > 0x7E))
            throw std::invalid_argument("TypeText: only 0x0D and 0x20-0x7E can be typed");
    }
    g_realtime.reset();
    if (text.empty())
        return true;
    // M2's behaviour on the input queue: queue the text with no delay and run until its last key has been applied. A key
    // that is not applied within 600 frames of emulated time of the previous one (the strobe never clears) is a timeout.
    const uint64_t id = QueueText(text, 0);
    Stepper stepper(pacing);   // Realtime: sound unmuted for this scope
    const uint64_t timeout = uint64_t(kTypeTimeoutFrames) * NTSC_GetCyclesPerFrame();
    uint64_t seen = g_input.keysApplied;
    uint64_t progressAt = g_nCumulativeCycles;
    for (;;) {
        ProcessDueInput();
        if (g_input.keysApplied != seen) {
            seen = g_input.keysApplied;
            progressAt = g_nCumulativeCycles;
        }
        auto done = std::find_if(g_input.applied.begin(), g_input.applied.end(),
                                 [id](const InputApplied& a) { return a.inputId == id; });
        if (done != g_input.applied.end()) {
            g_input.applied.erase(done);   // TypeText consumes its own completion
            return true;
        }
        if (g_nCumulativeCycles - progressAt >= timeout) {
            std::erase_if(g_input.fifo, [id](const QueuedOp& o) { return o.id == id; });
            throw std::runtime_error("TypeText: keyboard not read");
        }
        if (stepper.Step(UINT64_MAX)) {
            std::erase_if(g_input.fifo, [id](const QueuedOp& o) { return o.id == id; });   // window closed
            return false;
        }
    }
}

uint64_t Emulator::QueueText(std::string_view text, uint32_t interKeyDelayMs)
{
    RequireStarted();
    for (const char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u != 0x0D && (u < 0x20 || u > 0x7E))
            throw std::invalid_argument("QueueText: only 0x0D and 0x20-0x7E can be typed");
    }
    QueuedOp op;
    op.kind = QueuedOp::Kind::Text;
    op.id = g_input.nextId++;
    op.text = std::string(text);
    op.delayCycles = MsToBaseCycles(interKeyDelayMs);
    g_input.fifo.push_back(std::move(op));
    return g_input.fifo.back().id;
}

uint64_t Emulator::QueueKey(KeySpec key, uint32_t holdMs)
{
    RequireStarted();
    if (const uint8_t* c = std::get_if<uint8_t>(&key); c && *c > 127)
        throw std::invalid_argument("QueueKey: ASCII code above 127");
    QueuedOp op;
    op.kind = QueuedOp::Kind::Key;
    op.id = g_input.nextId++;
    op.key = key;
    op.holdCycles = MsToBaseCycles(holdMs);
    g_input.fifo.push_back(std::move(op));
    return g_input.fifo.back().id;
}

uint64_t Emulator::QueueJoystick(JoystickChange change, uint32_t durationMs)
{
    RequireStarted();
    QueuedOp op;
    op.kind = QueuedOp::Kind::Joy;
    op.id = g_input.nextId++;
    op.change = change;
    op.durationCycles = MsToBaseCycles(durationMs);
    g_input.fifo.push_back(std::move(op));
    return g_input.fifo.back().id;
}

std::optional<uint64_t> Emulator::ScheduledCycle(uint64_t inputId) const
{
    RequireStarted();
    if (const auto it = g_input.firstCycles.find(inputId); it != g_input.firstCycles.end())
        return it->second;
    // Not applied yet: known only when nothing is queued ahead and, for a key, the strobe is clear.
    if (g_input.fifo.empty() || g_input.fifo.front().id != inputId)
        return std::nullopt;
    const QueuedOp& op = g_input.fifo.front();
    bool waitsForStrobe = false;
    if (op.kind == QueuedOp::Kind::Text)
        waitsForStrobe = op.next < op.text.size();
    else if (op.kind == QueuedOp::Kind::Key)
        waitsForStrobe = !std::holds_alternative<ResetKey>(op.key);
    if (waitsForStrobe && !StrobeClear())
        return std::nullopt;
    return EmulatedNow();
}

std::vector<InputApplied> Emulator::TakeAppliedInputs()
{
    RequireStarted();
    return std::exchange(g_input.applied, {});
}

void Emulator::InsertDisk(int drive, const std::filesystem::path& image)
{
    RequireStarted();
    if (drive < 0 || drive > 1)
        throw std::out_of_range("Emulator: drive must be 0 or 1");
    Disk2InterfaceCard& card = Disk2Card();
    const size_t before = HostFrame().messages().size();
    const ImageError_e err = card.InsertDisk(drive, image.string(), false, false);
    if (err != eIMAGE_ERROR_NONE) {
        // InsertDisk() itself reports nothing; the Win32 UI calls this separately. It routes AppleWin's human-readable
        // text through FrameMessageBox, which HarnessFrame records.
        card.NotifyInvalidImage(drive, image.string(), err);
        std::string what = "Emulator: cannot insert " + image.string() + " (image error " + std::to_string(int(err)) + ")";
        const auto& msgs = HostFrame().messages();
        for (size_t i = before; i < msgs.size(); ++i)
            what += "\n  " + msgs[i];
        throw std::runtime_error(what);
    }
}

void Emulator::EjectDisk(int drive)
{
    RequireStarted();
    if (drive < 0 || drive > 1)
        throw std::out_of_range("Emulator: drive must be 0 or 1");
    Disk2Card().EjectDisk(drive);
}

uint64_t Emulator::PresentedFrames() const
{
    RequireStarted();
    return HostFrame().presentedFrames();
}

FrameView Emulator::Frame(bool borders) const
{
    RequireStarted();
    return HostFrame().PresentedFrame(borders);
}

std::string Emulator::ReadTextRow(int row) const
{
    RequireStarted();
    if (row < 0 || row > 23)
        throw std::out_of_range("Emulator::ReadTextRow: row must be 0..23");
    const WORD addr = WORD(0x400 + 0x80 * (row % 8) + 0x28 * (row / 8));
    const BYTE* p = MemGetMainPtr(addr);
    std::string s(40, ' ');
    for (int i = 0; i < 40; ++i)
        s[size_t(i)] = char(p[i] & 0x7F);
    return s;
}

const std::vector<std::string>& Emulator::Messages() const
{
    RequireStarted();
    return HostFrame().messages();
}

uint64_t Emulator::SoundUnderruns() const
{
    RequireStarted();
    return HostFrame().SoundUnderruns();
}

bool Emulator::SoundActive() const
{
    return g_started && g_soundActive;
}

}  // namespace harness
