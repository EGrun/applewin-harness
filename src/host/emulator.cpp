#include "host/emulator.h"
#include "host/applewin_host.h"
#include "host/harness_frame.h"
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
#include "NTSC.h"
#include "RGBMonitor.h"
#include "Speaker.h"
#include "Utilities.h"
#include "version.h"
#pragma warning(pop)

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>

namespace harness {

namespace {

bool g_started = false;
uint64_t g_cyclesAtPowerOn = 0;   // g_nCumulativeCycles at the last power-on (D10)

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

// The one step primitive behind RunFrames, RunRealtime and TypeText: executes at most 1 ms of emulated time.
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
        return ShouldStop();
    }

private:
    // Task 9: true once the window has been closed. There is no window yet.
    static bool ShouldStop() { return false; }

    bool realtime_;
    uint32_t period_;
    std::optional<BurstBudget> budget_;
    std::optional<RealtimePacer> pacer_;
};

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
    if (config.display != DisplayMode::Headless)
        throw std::logic_error("not implemented");   // Task 2 stub: only the M1 headless start exists so far

    // 1. Directories, config file (INI-only mode in Registry.cpp) and deterministic power-on memory.
    const std::string exeDir = ExeDirWithSeparator();
    g_sStartDir = exeDir;
    g_sProgramDir = exeDir;
    g_sConfigFile = config.configIni.string();
    g_nMemoryClearType = MIP_ZERO;
    // No sound (as AppleWin's -no-dsound switch). Speaker and Mockingboard skip DirectSound and never show
    // the "unable to initialize a waveform output device" message box.
    g_bDisableDirectSound = true;

    // 2. Version (AppleWin reads it from its exe's version resource; the harness uses version.h directly).
    SetAppleWinVersion(APPLEWIN_VERSION);

    const size_t messagesBefore = HostFrame().messages().size();

    // 3. RepeatInitialization, headless subset.
    KeybReset();
    GetVideo().SetVidHD(false);
    ResetToLogoMode();
    LoadConfiguration(true);
    SetApple2Type(A2TYPE_APPLE2EENHANCED);
    // Host joysticks are never read (determinism): both ports disabled before JoyInitialize().
    JoySetJoyType(JN_JOYSTICK0, J0C_DISABLED);
    JoySetJoyType(JN_JOYSTICK1, J1C_DISABLED);
    JoyInitialize();
    VideoSwitchVideocardPalette(RGB_GetVideocard(), GetVideo().GetVideoType());
    GetFrame().Initialize(true);
    // AppleWin calls SpkrInitialize() from WM_CREATE (window creation, skipped here). Without it the speaker
    // sample buffer is null and the first SpkrUpdate() crashes.
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
    GetFrame().Destroy();
    SpkrDestroy();   // pairs with SpkrInitialize() in Start (AppleWin calls it from WM_DESTROY)
    LogDone();
    g_started = false;
}

void Emulator::PowerCycle()
{
    RequireStarted();
    // ResetMachineState() already boots the Disk II card (Utilities.cpp); a second Boot() would load a disk image twice.
    ResetMachineState();
    PinPowerOnRandomness();
    g_nAppMode = MODE_RUNNING;
    g_cyclesAtPowerOn = g_nCumulativeCycles;
}

bool Emulator::RunFrames(uint32_t frames, Pacing pacing)
{
    RequireStarted();
    if (frames == 0)
        return true;
    Stepper stepper(pacing);   // Task 9: the sound unmute/mute rule for Realtime goes with this scope
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
    using clock = std::chrono::steady_clock;
    // The pacer catches up short stalls and drops time after long ones, so stop on elapsed wall-clock time (not a tick
    // count) to keep the run length honest.
    Stepper stepper(Pacing::Realtime);   // Task 9: the sound unmute/mute rule goes with this scope
    const auto start = clock::now();
    while (clock::now() - start < wallTime) {
        if (stepper.Step(UINT64_MAX))
            return false;
    }
    return true;
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
    Stepper stepper(pacing);   // Task 9: the sound unmute/mute rule for Realtime goes with this scope
    const uint64_t timeout = uint64_t(kTypeTimeoutFrames) * NTSC_GetCyclesPerFrame();
    for (const char c : text) {
        const uint64_t giveUp = g_nCumulativeCycles + timeout;
        while (KeybReadData() & 0x80) {
            if (g_nCumulativeCycles >= giveUp)
                throw std::runtime_error("TypeText: keyboard not read");
            if (stepper.Step(UINT64_MAX))
                return false;
        }
        KeybQueueKeypress(WPARAM(static_cast<unsigned char>(c)), ASCII);
    }
    return true;
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
    throw std::logic_error("not implemented");
}

bool Emulator::SoundActive() const
{
    throw std::logic_error("not implemented");
}

}  // namespace harness
