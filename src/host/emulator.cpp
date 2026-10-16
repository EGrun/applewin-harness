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
#include <stdexcept>
#include <string>

namespace harness {

namespace {

bool g_started = false;

HarnessFrame& Headless()
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
    // No sound (as AppleWin's -no-dsound switch). Speaker and Mockingboard skip DirectSound and never show
    // the "unable to initialize a waveform output device" message box.
    g_bDisableDirectSound = true;

    // 2. Version (AppleWin reads it from its exe's version resource; the harness uses version.h directly).
    SetAppleWinVersion(APPLEWIN_VERSION);

    const size_t messagesBefore = Headless().messages().size();

    // 3. RepeatInitialization, headless subset.
    KeybReset();
    GetVideo().SetVidHD(false);
    ResetToLogoMode();
    LoadConfiguration(true);
    SetApple2Type(A2TYPE_APPLE2EENHANCED);
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

    g_started = true;

    const auto& msgs = Headless().messages();
    if (msgs.size() != messagesBefore) {
        std::string what = "Emulator: FrameMessageBox during startup:";
        for (size_t i = messagesBefore; i < msgs.size(); ++i)
            what += "\n  " + msgs[i];
        Shutdown();
        throw std::runtime_error(what);
    }

    // 4. Boot.
    Boot();
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
    g_nAppMode = MODE_RUNNING;
}

void Emulator::RunFrames(uint32_t frames)
{
    RequireStarted();
    if (frames == 0)
        return;
    const uint64_t target = g_nCumulativeCycles + uint64_t(frames) * NTSC_GetCyclesPerFrame();
    // Steps are capped at ContinueExecution's 1 ms execution period (~1020 cycles). One huge CpuExecute call would
    // present at most one frame and breaks per-period card updates (Mockingboard asserts on >= 0x10000 cycles).
    const uint32_t period = uint32_t(g_fCurrentCLK6502 * 1.0e-3);
    while (g_nCumulativeCycles < target) {
        const uint64_t remaining = target - g_nCumulativeCycles;
        ExecuteStep(remaining < period ? uint32_t(remaining) : period);
    }
}

void Emulator::RunRealtime(std::chrono::milliseconds wallTime)
{
    RequireStarted();
    using clock = std::chrono::steady_clock;
    // 1 ms bursts. The pacer catches up short stalls and drops time after long ones, so stop on elapsed wall-clock time
    // (not a tick count) to keep the run length honest.
    BurstBudget budget(g_fCurrentCLK6502, std::chrono::milliseconds(1));
    RealtimePacer pacer;
    pacer.Start();
    const auto start = clock::now();
    while (clock::now() - start < wallTime) {
        ExecuteStep(budget.Next());
        pacer.WaitForTick();
    }
}

uint64_t Emulator::EmulatedCycles() const
{
    RequireStarted();
    return g_nCumulativeCycles;
}

uint64_t Emulator::PresentedFrames() const
{
    RequireStarted();
    return Headless().presentedFrames();
}

FrameView Emulator::Frame(bool borders) const
{
    RequireStarted();
    Video& video = GetVideo();
    const uint32_t fullW = video.GetFrameBufferWidth();
    const uint32_t fullH = video.GetFrameBufferHeight();
    const int32_t stride = -int32_t(fullW * 4);
    const uint8_t* base = video.GetFrameBuffer();   // bottom-up: row 0 is the bottom row
    if (borders)
        return FrameView{base + size_t(fullH - 1) * fullW * 4, fullW, fullH, stride};

    const uint32_t bw = video.GetFrameBufferBorderWidth();
    const uint32_t bh = video.GetFrameBufferBorderHeight();
    const uint8_t* top = base + (size_t(fullH - 1 - bh) * fullW + bw) * 4;
    return FrameView{top, video.GetFrameBufferBorderlessWidth(), video.GetFrameBufferBorderlessHeight(), stride};
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
    return Headless().messages();
}

}  // namespace harness
