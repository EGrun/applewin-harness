#include "host/emulator.h"
#include "host/log.h"
#include "host/applewin_host.h"
#include "host/harness_frame.h"
#include "host/window.h"
#include "host/audio_output.h"
#include "core/pacing.h"
#include "core/audio_capture.h"
#include "core/sha256.h"

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
#include "DiskImageHelper.h"
#include "Joystick.h"
#include "Keyboard.h"
#include "Log.h"
#include "Memory.h"
#include "MockingboardCardManager.h"
#include "NTSC.h"
#include "RGBMonitor.h"
#include "SaveState.h"
#include "Speaker.h"
#include "Utilities.h"
#include "version.h"
#include "Windows/DXSoundBuffer.h"
#pragma warning(pop)

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
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

void CreateLanguageCard();   // Memory.cpp (external linkage, not in Memory.h): the //e's slot-0 language card

namespace harness {

namespace {

std::string StatePathForAppleWin(const std::filesystem::path& file);   // D10: an ANSI-safe string for a path

bool g_started = false;
bool g_dsInitialized = false;   // DSInit() (D14: the harness's own, opens no device) succeeded; DSUninit() pending
std::unique_ptr<Window> g_window;   // windowed mode only
std::unique_ptr<AudioOutput> g_audioOutput;   // windowed mode with an audio device only (D14 playback)
uint64_t g_cyclesAtPowerOn = 0;   // g_nCumulativeCycles at the last power-on (D10)
std::function<void()> g_presentObserver;   // SetPresentObserver

HarnessFrame& HostFrame()
{
    return static_cast<HarnessFrame&>(GetFrame());
}

// The agent-audio timeline (D14), fed by every VirtualSoundBuffer. Never destroyed: AppleWin owns the buffers, and a
// buffer's destructor (ResetVoice) may run late.
AudioCapture& Capture()
{
    static AudioCapture* const capture = new AudioCapture();
    return *capture;
}

// The capture's clock: EmulatedCycles(), clamped at 0 (D10: cycles since the last power-on).
uint64_t CaptureClock()
{
    return g_nCumulativeCycles >= g_cyclesAtPowerOn ? g_nCumulativeCycles - g_cyclesAtPowerOn : 0;
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

// Drops every queued input and releases held keys, reporting each dropped id once as discarded (ruling D1): the inputs
// still queued and those whose release or restore is still pending. LoadSnapshot.
void DiscardInput()
{
    std::vector<uint64_t> ids;
    for (const QueuedOp& op : g_input.fifo)
        ids.push_back(op.id);
    for (const TimedOp& t : g_input.timed)
        ids.push_back(t.id);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for (const uint64_t id : ids) {
        InputApplied a;
        a.inputId = id;
        a.discarded = true;
        g_input.applied.push_back(a);
    }
    ClearInput();
}

// Joystick 0 once every pending restore has taken effect (in due order): the position a checkpoint keeps.
JoyState SettledJoystick()
{
    std::vector<const TimedOp*> restores;
    for (const TimedOp& t : g_input.timed)
        if (t.kind == TimedOp::Kind::JoyRestore)
            restores.push_back(&t);
    std::stable_sort(restores.begin(), restores.end(), [](const TimedOp* a, const TimedOp* b) { return a->due < b->due; });
    JoyState s = g_input.joy;
    for (const TimedOp* t : restores)
        s = t->restore;
    return s;
}

// The //e any-key-down flag ($C010 bit 7). AppleWin's g_AKDFlags are static and KeybReadFlag clears the strobe; the
// harness is the only caller of KeybAnyKeyDown (the window is view-only), so the flag is set exactly while a key it
// pressed waits for its release.
bool AnyKeyDown()
{
    return std::any_of(g_input.timed.begin(), g_input.timed.end(),
                       [](const TimedOp& t) { return t.kind == TimedOp::Kind::KeyUp; });
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


// ---- Speed (D11; docs/specs/m3.md Part C; Ruling C7).
// The session's machine speed is the harness's own (g_sessionSpeed). AppleWin's g_dwSpeed and clock are what the
// current or last step needed: every step first calls ApplyClock, with SPEED_NORMAL in a stepped run and with
// g_sessionSpeed in a realtime run. So AppleWin's clock only changes at the start of a step, and only when the step
// needs another one. Changing it eagerly (in set_speed, or back after every stepped run) would not do:
// SetCurrentCLK6502 re-initializes the speaker (SpkrReinitialize drops its partial sample) and the Mockingboard clock,
// so stepped audio would depend on the speed setting and on when it was set.
// Enhanced disk access is AppleWin's card state and is the session's setting outside stepped runs: a stepped run
// turns it on and restores it when the run ends (SteppedEnhance). It has no side effects.
bool g_fullSpeedOverride = false;   // SetFullSpeedOverride: the harness's stand-in for Scroll Lock
uint32_t g_sessionSpeed = SPEED_NORMAL;   // the session's machine speed (Start: the INI's, as LoadConfiguration set it)

// What AppleWin does on a speed change (the config dialog, PageConfig.cpp, and the speed hotkeys, WinFrame.cpp): set
// g_dwSpeed, then SetCurrentCLK6502(), which recomputes g_fCurrentCLK6502 and re-initializes the speaker's samples per
// cycle (SpkrReinitialize) and the Mockingboard clock (ReinitializeClock). Only when the speed differs.
void ApplyClock(uint32_t speed)
{
    if (g_dwSpeed == speed)
        return;
    g_dwSpeed = speed;
    SetCurrentCLK6502();
}

// SetCurrentCLK6502's clock for a speed (Core.cpp), without applying it.
double ClockForSpeed(uint32_t speed)
{
    const double mhz = speed < SPEED_NORMAL ? 0.5 + double(speed) * 0.05 : double(speed) / 10.0;
    return Get6502BaseClock() * mhz;
}

// A stepped run's enhanced disk access: on for the run (AppleWin's default, kEnhanceDiskAccessSpeed_Default), then
// the session's setting again, however the run ends (return, window closed or throw).
class SteppedEnhance {
public:
    SteppedEnhance() : session_(GetCardMgr().GetDisk2CardMgr().GetEnhanceDisk())
    {
        GetCardMgr().GetDisk2CardMgr().SetEnhanceDisk(true);
    }
    SteppedEnhance(const SteppedEnhance&) = delete;
    SteppedEnhance& operator=(const SteppedEnhance&) = delete;
    ~SteppedEnhance() { GetCardMgr().GetDisk2CardMgr().SetEnhanceDisk(session_); }

private:
    bool session_;
};

// ContinueExecution's full-speed condition (Windows/AppleWin.cpp), with the override in place of Scroll Lock.
// Bit 0: speed 40; bit 1: override; bit 2: disk motor (Disk II motor on with enhance on, speaker and Mockingboard silent).
unsigned FullSpeedReasonBits()
{
    unsigned bits = 0;
    if (g_sessionSpeed == SPEED_MAX)
        bits |= 1u;
    if (g_fullSpeedOverride)
        bits |= 2u;
    if (GetCardMgr().GetDisk2CardMgr().IsConditionForFullSpeed() && !Spkr_IsActive() &&
        !GetCardMgr().GetMockingboardCardMgr().IsActiveToPreventFullSpeed())
        bits |= 4u;
    return bits;
}

// ---- Video phase (docs/specs/m4.md "Restore purity", pass condition 6).
// Two pieces of video state decide what a frame looks like: the scanner's position in the frame (g_dwCyclesThisFrame,
// which AppleWin's save state holds, and NTSC.cpp's clocks, set from it) and the text flash phase (NTSC.cpp's static
// g_nTextFlashCounter and g_nTextFlashMask, for FLASH characters such as the Applesoft cursor), which no save state
// holds. The flash counter goes up by one every time the scanner wraps from the last line to line 0, and the mask flips
// every 16; a fresh process starts both at 0, so the flash state is the number of wraps since the process started,
// mod 32. Neither static can be read or written, so the harness counts the wraps itself (g_flashFrames) and moves the
// phase forward with whole-screen redraws (NTSC_VideoRedrawWholeScreen: exactly one wrap each).
// The scanner advances only in two ways: CpuExecute with video updates (a normal step) moves it by exactly the cycles
// executed, and every whole-screen redraw (VideoRedrawScreen, which presents) wraps it once. At full speed CpuExecute
// leaves it alone and NTSC_VideoClockResync only moves it within the frame.
// Load-bearing dependency: AppleWin's Snapshot_LoadState calls frame.FrameUpdateApple2Type(), which in Win32Frame
// redraws the screen (one untracked wrap). HarnessFrame overrides it as a no-op (harness_frame.h); if it ever redraws,
// LoadSnapshot must count that wrap. Every path that runs the CPU or redraws must count its wraps here.
constexpr uint32_t kFlashPeriod = 32;   // 16 frames per flash state, two states
uint32_t g_flashFrames = 0;             // scanner wraps since the process started (only the value mod 32 matters)

uint32_t ScannerCycle()
{
    uint16_t vert = 0, horz = 0;
    NTSC_GetVideoVertHorzForDebugger(vert, horz);   // between steps and outside full speed: no side effect
    return uint32_t(vert) * NTSC_GetCyclesPerLine() + horz;
}

// Whole-screen redraws per VideoRedrawScreen (Video::VideoRefreshBuffer draws twice when paused or debugging).
uint32_t RedrawsPerPresent()
{
    return (g_nAppMode == MODE_DEBUG || g_nAppMode == MODE_PAUSED) ? 2u : 1u;
}

// ExecuteStep, counting the scanner's wraps. Normal step: (start + executed) / cycles per frame. Full speed: every
// present inside the step is a whole-screen redraw (VideoRedrawScreenDuringFullSpeed).
// Counts the whole-screen redraws presented since construction, also when a present hook (or the observer) throws out
// of the step: HarnessFrame counts a present before calling its hook.
class CountPresentedRedraws {
public:
    CountPresentedRedraws() : presents_(HostFrame().presentedFrames()) {}
    CountPresentedRedraws(const CountPresentedRedraws&) = delete;
    CountPresentedRedraws& operator=(const CountPresentedRedraws&) = delete;
    ~CountPresentedRedraws() { g_flashFrames += uint32_t(HostFrame().presentedFrames() - presents_) * RedrawsPerPresent(); }

private:
    uint64_t presents_;
};

uint32_t TrackedExecuteStep(uint32_t cycles, bool fullSpeed = false)
{
    if (fullSpeed) {
        CountPresentedRedraws count;
        return ExecuteStep(cycles, true);
    }
    // A normal step's wraps all happen inside CpuExecute, before ExecuteStep's present (the only place it can throw:
    // the present hook), so they are counted from the scanner's position afterwards in that case.
    const uint32_t start = ScannerCycle();
    const uint32_t frame = NTSC_GetCyclesPerFrame();
    try {
        const uint32_t executed = ExecuteStep(cycles, false);
        g_flashFrames += (start + executed) / frame;
        return executed;
    } catch (...) {
        const uint32_t end = ScannerCycle();
        g_flashFrames += end < start ? 1 : 0;   // a step is at most 1 ms, far less than a frame: at most one wrap
        throw;
    }
}

// Moves the flash phase forward to `phase` (mod kFlashPeriod) with whole-screen redraws. Draws into the video buffer;
// the scanner's position is unchanged.
void AdvanceFlashTo(uint32_t phase)
{
    while (g_flashFrames % kFlashPeriod != phase % kFlashPeriod) {
        NTSC_VideoRedrawWholeScreen();
        ++g_flashFrames;
    }
}

// The scanner at cycle 0 of a frame and the flash phase of a fresh process, with a cleared video buffer: what Start's
// Video::Initialize gives a fresh process (g_dwCyclesThisFrame is 0 there). Start and PowerCycle (pass condition 6).
void ResetVideoPhase()
{
    g_dwCyclesThisFrame = 0;
    GetVideo().VideoReinitialize(true);   // NTSC clocks from g_dwCyclesThisFrame, as NTSC_VideoInit does
    AdvanceFlashTo(0);
    GetVideo().ClearFrameBuffer();
}

// Re-renders the whole screen from video memory at the scanner's current position (Video::VideoRefreshBuffer, as
// AppleWin's full-speed redraw does) and makes it the presented frame, without counting a present or calling the
// present observer. Windowed: the window shows it.
void RenderPresentedFrame()
{
    GetVideo().VideoRefreshBuffer(GetVideo().GetVideoMode(), true);
    g_flashFrames += RedrawsPerPresent();
    HostFrame().RefreshSnapshot();
    if (g_window) {
        g_window->Show(HostFrame().PresentedFrame(false));
        g_window->Pump();
    }
}

// ---- Speaker (docs/specs/m4.md "Restore purity", M4-5).
// AppleWin saves only the speaker's last update cycle. The rest is history: the output level (g_nSpeakerData), the
// 8-bit-DAC quietening flag, the sample and remainder buffers, the quiet-cycle counter, the active flag and Speaker.cpp's
// static DC filter (g_uDCFilterState: 32768 + 10000 after every toggle, down by one per sample queued, output scaled by
// it). Each is put back to a fresh process's value: SpkrReset for the buffers and flags, g_nSpeakerData and
// g_bQuieterSpeaker directly, and the unreachable DC filter by queueing more samples than its maximum through it (a
// fresh process's filter is 0, and 0 stays 0). The queued samples reach the speaker voice and so the capture, which the
// caller clears afterwards. Needs g_bFullSpeed clear and the clock already set (CanonicalClock).
void NeutralizeSpeaker()
{
    ResetCyclesExecutedForDebugger();   // CpuExecute's per-batch counter: nothing below may add stray cycles
    SpkrReset();
    // UpdateSpkr queues one sample per g_fClksPerSpkrSample cycles since its last update, at most SPKR_SAMPLE_RATE - 1
    // (44,099 > 42,768) into an empty buffer. Run its clock forward far enough, then back (an unsigned difference that
    // hits the buffer limit again), so its last-update cycle ends where it was.
    const unsigned __int64 now = g_nCumulativeCycles;
    {
        struct RestoreClock {   // the emulated clock is never left moved, whatever SpkrUpdate does
            unsigned __int64 now;
            ~RestoreClock() { g_nCumulativeCycles = now; }
        } restoreClock{now};
        g_nCumulativeCycles = now + uint64_t(double(SPKR_SAMPLE_RATE + 1) * (g_fClksPerSpkrSample + 1.0));
        SpkrUpdate(0);
    }
    SpkrUpdate(0);
    g_nSpeakerData = static_cast<short>(-32768);   // SPKR_DATA_INIT, (short)0x8000 (Speaker.cpp)
    g_bQuieterSpeaker = false;
    SpkrReset();
}

// The clock every stepped run uses (ruling C7), applied unconditionally: SetCurrentCLK6502 also re-initializes the
// speaker's samples per cycle and every Mockingboard's clock, so afterwards they no longer depend on history.
void CanonicalClock()
{
    g_dwSpeed = SPEED_NORMAL;
    SetCurrentCLK6502();
}

// The one step primitive behind RunFrames, RunRealtime and TypeText: executes at most 1 ms of emulated time. It also
// holds the window-closed rule for all three.
// Stepped: stepped runs always use authentic speed and enhanced disk access; speed settings affect realtime runs only
// (ruling C7). Each step applies speed 10 (ApplyClock) and the run holds enhanced disk access on (SteppedEnhance);
// steps of 1 ms at the base clock, video on, never full speed, no pacing.
// Realtime (D9, D11, D14): each step first applies the session's speed (ApplyClock), then decides full speed with
// ContinueExecution's rule (FullSpeedReasonBits).
// - Normal: a BurstBudget burst at g_fCurrentCLK6502 (rebuilt whenever that clock changes) corrected by AudioOutput's
//   feedback, then AudioOutput::Pump, then wait for the 1 ms tick.
// - Full speed: unpaced bursts of the same size (g_fCurrentCLK6502 x 1 ms, so input, capture and the window keep
//   working) with CpuExecute(cycles, false) and no feedback. AppleWin's entry (VideoRedrawScreenDuringFullSpeed(0,
//   true), Mockingboard MuteControl(true), g_nCpuCyclesFeedback = 0); a redraw from the end-of-frame check at most every
//   ~16 ms of wall time (ExecuteStep); on exit VideoRedrawScreenAfterFullSpeed(g_dwCyclesThisFrame) and
//   MuteControl(false). Leaving full speed restarts the pacing, so the unpaced time is not "made up". The end of the
//   run (return or throw) exits full speed too, so g_bFullSpeed is only ever set inside a realtime run.
// Playback (AudioOutput, windowed): on only while a realtime run steps at the base clock (speed 10) and not at full
// speed. The capture timeline is in emulated time, so at any other rate it cannot play in step with the wall clock,
// and the feedback (up to +/-200 samples' worth of cycles per 1 ms step) would pull the emulation back to the base
// clock. Otherwise the buffer is stopped (silence; it resyncs when playback resumes). Capture is never affected.
class Stepper {
public:
    explicit Stepper(Pacing pacing)
        : realtime_(pacing == Pacing::Realtime), period_(uint32_t(CLK_6502_NTSC * 1.0e-3))
    {
        if (!realtime_) {
            enhance_.emplace();
            ApplyClock(SPEED_NORMAL);
        } else {
            ApplyClock(g_sessionSpeed);
            clock_ = g_fCurrentCLK6502;
            budget_.emplace(clock_, std::chrono::milliseconds(1));
            pacer_.emplace();
            pacer_->Start();
        }
    }
    Stepper(const Stepper&) = delete;
    Stepper& operator=(const Stepper&) = delete;

    ~Stepper()
    {
        if (fullSpeed_) {
            try { LeaveFullSpeed(); } catch (...) {}
        }
    }

    // Runs one step of at most `remaining` cycles. Returns true if the run must stop (window closed).
    bool Step(uint64_t remaining)
    {
        ProcessDueInput();   // input takes effect at the start of the step (D10)
        if (!realtime_) {
            ApplyClock(SPEED_NORMAL);   // a no-op after the first step
            TrackedExecuteStep(uint32_t((std::min)(uint64_t(period_), remaining)));
            return WindowClosed();
        }

        ApplyClock(g_sessionSpeed);   // a set_speed during the run takes effect from this step on
        const bool full = FullSpeedReasonBits() != 0;
        if (full && !fullSpeed_)
            EnterFullSpeed();
        else if (!full && fullSpeed_)
            LeaveFullSpeed();
        if (g_fCurrentCLK6502 != clock_) {   // a speed change: the new rate from this step on
            clock_ = g_fCurrentCLK6502;
            budget_.emplace(clock_, std::chrono::milliseconds(1));
        }
        playback_.Set(!fullSpeed_ && g_dwSpeed == SPEED_NORMAL);

        uint64_t cycles;
        if (fullSpeed_) {
            // Every full-speed step, as ContinueExecution does.
            GetCardMgr().GetMockingboardCardMgr().MuteControl(true);
            g_nCpuCyclesFeedback = 0;
            cycles = budget_->Next();
        } else {
            GetCardMgr().GetMockingboardCardMgr().MuteControl(false);
            const int64_t feedback = g_audioOutput ? g_audioOutput->FeedbackCycles() : 0;
            const int64_t withFeedback = int64_t(budget_->Next()) + feedback;
            cycles = withFeedback > 0 ? uint64_t(withFeedback) : 0;
        }
        if (cycles > remaining)
            cycles = remaining;
        TrackedExecuteStep(uint32_t(cycles), fullSpeed_);
        if (!fullSpeed_) {
            if (g_audioOutput)
                g_audioOutput->Pump(Capture(), CaptureClock());
            pacer_->WaitForTick();
        }
        return WindowClosed();
    }

private:
    void EnterFullSpeed()
    {
        g_bFullSpeed = true;
        fullSpeed_ = true;
        GetFrame().VideoRedrawScreenDuringFullSpeed(0, true);   // init: starts the ~16 ms redraw interval
        GetCardMgr().GetMockingboardCardMgr().MuteControl(true);
        g_nCpuCyclesFeedback = 0;
    }

    void LeaveFullSpeed()
    {
        g_bFullSpeed = false;
        fullSpeed_ = false;
        {
            CountPresentedRedraws count;
            GetFrame().VideoRedrawScreenAfterFullSpeed(g_dwCyclesThisFrame);   // resync the video clock, redraw, present
        }
        GetCardMgr().GetMockingboardCardMgr().MuteControl(false);
        clock_ = g_fCurrentCLK6502;
        budget_.emplace(clock_, std::chrono::milliseconds(1));
        pacer_->Start();   // paced again from now: the unpaced time is not made up
    }

    // Window-closed rule (spec): when windowed, pump the window's messages after every step; the run stops once the
    // window has been closed.
    static bool WindowClosed()
    {
        if (!g_window)
            return false;
        g_window->Pump();
        return g_window->Closed();
    }

    // Sound rule (D14, D11): AudioOutput plays only during a realtime run, and within it as Step decides;
    // SetPlaying(false) when the run ends or throws.
    class Playback {
    public:
        Playback() = default;
        Playback(const Playback&) = delete;
        Playback& operator=(const Playback&) = delete;
        void Set(bool on)
        {
            if (on == active_ || !g_audioOutput)
                return;
            g_audioOutput->SetPlaying(on);
            active_ = on;
        }
        ~Playback()
        {
            if (active_ && g_audioOutput)
                g_audioOutput->SetPlaying(false);
        }

    private:
        bool active_ = false;
    };

    bool realtime_;
    uint32_t period_;
    double clock_ = 0.0;      // the clock budget_ was built for
    bool fullSpeed_ = false;  // this run has set g_bFullSpeed
    std::optional<BurstBudget> budget_;
    std::optional<RealtimePacer> pacer_;
    Playback playback_;   // stops playback when the Stepper is destroyed
    std::optional<SteppedEnhance> enhance_;   // stepped runs only
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

// Releases every sound voice, the (virtual) sound system and the window: part of Shutdown (also used when Start fails).
// DSUninit() asserts that no voice is left, so the speaker, every Mockingboard card's SSI263 voices and the
// Mockingboard manager's voice are released first. Harmless when called again.
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
    if (g_dsInitialized) {
        DSUninit();
        g_dsInitialized = false;
    }
    HarnessFrame& frame = HostFrame();
    frame.SetPresentHook({});
    frame.SetAudioCapture(nullptr);
    g_audioOutput.reset();   // before the window it was opened on
    g_window.reset();
    frame.g_hFrameWindow = nullptr;
}

// Windowed start, before the M1 startup (spec): the window and g_hFrameWindow, then playback (D14): AudioOutput on
// the window, or the M2 warning and no sound if there is no audio device.
void StartWindow()
{
    g_window = std::make_unique<Window>(L"AppleWin Harness (view only)");
    HostFrame().g_hFrameWindow = g_window->Handle();
    g_audioOutput = AudioOutput::Open(g_window->Handle());
    if (!g_audioOutput)
        log::Warning(log::Component::Audio, "no audio device; running without sound");
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
    g_sConfigFile = StatePathForAppleWin(config.configIni);   // D10: ANSI-safe even under a non-ASCII %TEMP%
    g_nMemoryClearType = MIP_ZERO;
    // Any exception out of Start leaves the emulator stopped (failures after g_started go through Shutdown()); this
    // also releases a window and the sound system set up before the failure.
    struct ReleaseIfNotStarted {
        ~ReleaseIfNotStarted() { if (!g_started) ReleaseSoundAndWindow(); }
    } releaseIfNotStarted;
    if (config.display == DisplayMode::Windowed)
        StartWindow();
    // Sound (D14), every mode: the harness's DSInit() opens no device, and every buffer AppleWin creates is a
    // VirtualSoundBuffer feeding the capture. So the speaker and Mockingboard always run their sound paths.
    g_bDisableDirectSound = false;
    g_dsInitialized = DSInit();
    Capture().Clear();
    HostFrame().SetAudioCapture(&Capture(), CaptureClock);
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
    g_fullSpeedOverride = false;   // a new session; machine speed and enhance-disk come from the INI (LoadConfiguration)
    g_sessionSpeed = uint32_t(g_dwSpeed);
    g_input.applied.clear();
    g_input.firstCycles.clear();
    ResetJoystick();
    VideoSwitchVideocardPalette(RGB_GetVideocard(), GetVideo().GetVideoType());
    GetFrame().Initialize(true);
    // AppleWin calls SpkrInitialize() from WM_CREATE, after DSInit() (here: after DSInit() above). Without it
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
    // A fresh process's video phase and speaker (already so in a new process; not after an earlier Start in it).
    ResetVideoPhase();
    NeutralizeSpeaker();
    Capture().Clear();   // the timeline starts at this power-on
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
    ReleaseSoundAndWindow();   // every sound voice, then DSUninit(), then the window
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
    // Pass condition 6: what a fresh Start has. ResetMachineState leaves the video scanner where the last run stopped
    // (mid-frame after a realtime run) and the flash phase, the speaker's history and the Mockingboard manager's
    // audio-frame counter as they were.
    CanonicalClock();
    GetCardMgr().GetMockingboardCardMgr().Reset(true);
    ResetVideoPhase();
    NeutralizeSpeaker();
    Capture().Clear();   // the timeline restarts at cycle 0 (D14)
    ResetJoystick();
}

bool Emulator::RunFrames(uint32_t frames, Pacing pacing)
{
    RequireStarted();
    g_realtime.reset();
    if (frames == 0)
        return true;
    Stepper stepper(pacing);
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
    Stepper stepper(Pacing::Realtime);
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
        g_realtime = std::make_unique<Stepper>(Pacing::Realtime);   // fresh pacing
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
    Stepper stepper(pacing);
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

uint64_t Emulator::PendingInputEvents() const
{
    RequireStarted();
    uint64_t n = 0;
    for (const QueuedOp& op : g_input.fifo) {
        switch (op.kind) {
        case QueuedOp::Kind::Text: n += std::max<size_t>(1, op.text.size() - op.next); break;   // an empty text still occupies a slot
        case QueuedOp::Kind::Key: n += 1; break;
        case QueuedOp::Kind::Joy: n += op.durationCycles > 0 ? 2 : 1; break;
        }
    }
    for (const TimedOp& t : g_input.timed)
        if (t.kind == TimedOp::Kind::JoyRestore) ++n;
    return n;
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

namespace {
// The save state stores each image name as the UTF-8 of AppleWin's ANSI string (YamlSaveHelper::SaveString), and the
// load hands those bytes back to the ANSI file API (see CheckImagesExist). The two agree only when the UTF-8 bytes equal
// the ANSI bytes (ASCII, or a UTF-8 ANSI code page); otherwise the checkpoint could never be restored.
bool SurvivesSaveState(const std::string& ansi)
{
    if (ansi.empty())
        return true;
    const int wideLen = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, ansi.data(), int(ansi.size()), nullptr, 0);
    if (wideLen <= 0)
        return false;
    std::wstring wide(size_t(wideLen), L'\0');
    MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, ansi.data(), int(ansi.size()), wide.data(), wideLen);
    const int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLen, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0)
        return false;
    std::string utf8(size_t(utf8Len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLen, utf8.data(), utf8Len, nullptr, nullptr);
    return utf8 == ansi;
}


// D10 (M6): the string AppleWin is given for a working copy. AppleWin's save state keeps image paths as UTF-8 but its
// load reads them with the ANSI file API, so a path that is not ANSI-safe could never be restored. Such a path is
// replaced by its ASCII 8.3 short path (GetShortPathNameW; the file must exist) when one exists. A path with no ASCII
// form is passed on as ANSI (SaveSnapshot then refuses it); one that has no ANSI form at all is an error here.
bool g_noShortPathsForTest = false;   // test seam (SetShortPathsDisabledForTest): behave as if 8.3 names were off

std::string ImagePathForAppleWin(const std::filesystem::path& image)
{
    const std::wstring& wide = image.native();
    BOOL usedDefault = FALSE;
    std::string ansi;
    const int len = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide.data(), int(wide.size()), nullptr, 0, nullptr,
                                        &usedDefault);
    if (len > 0 && !usedDefault) {
        ansi.assign(size_t(len), '\0');
        usedDefault = FALSE;
        WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide.data(), int(wide.size()), ansi.data(), len, nullptr,
                            &usedDefault);
        if (!usedDefault && SurvivesSaveState(ansi))
            return ansi;
    }
    std::wstring shortPath(MAX_PATH, L'\0');
    DWORD n = g_noShortPathsForTest ? 0 : GetShortPathNameW(image.c_str(), shortPath.data(), DWORD(shortPath.size()));
    if (n >= shortPath.size()) {
        shortPath.resize(n);
        n = GetShortPathNameW(image.c_str(), shortPath.data(), DWORD(shortPath.size()));
    }
    if (n > 0 && n < shortPath.size()) {
        shortPath.resize(n);
        if (std::all_of(shortPath.begin(), shortPath.end(), [](wchar_t c) { return c > 0 && c < 0x80; }))
        {
            std::string ascii;
            for (const wchar_t c : shortPath)
                ascii.push_back(char(c));
            return ascii;
        }
    }
    if (ansi.empty())
        throw std::runtime_error("Emulator: a path has characters the ANSI code page cannot represent and "
                                 "no ASCII short (8.3) path exists for it");
    return ansi;
}

// The long path of every image inserted through a different (short) string, keyed by that string upper-cased: list_disks
// keeps reporting the long path although AppleWin (and so the save state) holds the short one.
std::map<std::string, std::filesystem::path> g_longPaths;

std::string UpperAscii(std::string s)
{
    for (char& c : s)
        if (c >= 'a' && c <= 'z')
            c = char(c - 'a' + 'A');
    return s;
}

// The save state file as AppleWin gets it (Snapshot_SetFilename takes ANSI): its directory through
// ImagePathForAppleWin (the file itself need not exist yet), then the file name, which must be ANSI-safe.
std::string StatePathForAppleWin(const std::filesystem::path& stateFile)
{
    const std::filesystem::path dir = stateFile.parent_path();
    const std::string name = ImagePathForAppleWin(stateFile.filename());   // no short form for a bare name: ANSI or error
    if (dir.empty())
        return name;
    std::string out = ImagePathForAppleWin(dir);
    if (!out.empty() && out.back() != '\\' && out.back() != '/')
        out += '\\';
    return out + name;
}

// For messages: UTF-8, never a failed ANSI conversion.
std::string Shown(const std::filesystem::path& p)
{
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

}  // namespace

void Emulator::SetShortPathsDisabledForTest(bool disabled) { g_noShortPathsForTest = disabled; }

void Emulator::InsertDisk(int drive, const std::filesystem::path& image, bool writeProtected)
{
    RequireStarted();
    if (drive < 0 || drive > 1)
        throw std::out_of_range("Emulator: drive must be 0 or 1");
    Disk2InterfaceCard& card = Disk2Card();
    const size_t before = HostFrame().messages().size();
    const std::string path = ImagePathForAppleWin(image);
    const ImageError_e err = card.InsertDisk(drive, path, writeProtected, false);
    if (err == eIMAGE_ERROR_NONE)
        g_longPaths[UpperAscii(path)] = image;
    if (err != eIMAGE_ERROR_NONE) {
        // InsertDisk() itself reports nothing; the Win32 UI calls this separately. It routes AppleWin's human-readable
        // text through FrameMessageBox, which HarnessFrame records.
        card.NotifyInvalidImage(drive, path, err);
        std::string what = "Emulator: cannot insert " + Shown(image) + " (image error " + std::to_string(int(err)) + ")";
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
    Disk2InterfaceCard& card = Disk2Card();
    if (!card.IsDriveEmpty(drive))
        card.FlushCurrentTrack(drive);
    card.EjectDisk(drive);
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

namespace {
// Maps a text-page byte to printable ASCII 0x20-0x7E (docs/specs/m5.md): inverse, flashing and MouseText glyphs map to
// their base character, MouseText to a space. In the primary set the inverse/flashing range $00-$7F has no lowercase
// ($60-$7F is flashing $20-$3F); normal video ($80-$FF) maps to the ASCII the program wrote, lowercase included.
char TextCodeToAscii(BYTE code, bool altCharSet)
{
    const int v = code & 0x7F;
    int c;
    if (altCharSet) {
        if (code < 0x80 && v >= 0x40 && v < 0x60) return ' ';   // MouseText
        c = v < 0x20 ? v + 0x40 : v;
    } else {
        c = v < 0x20 ? v + 0x40 : (code < 0x80 && v >= 0x60 ? v - 0x40 : v);
    }
    return (c < 0x20 || c > 0x7E) ? ' ' : char(c);
}
}  // namespace

TextScreen Emulator::ReadTextScreen() const
{
    RequireStarted();
    const Video& video = GetVideo();
    const bool text = video.VideoGetSWTEXT();
    const bool mixed = video.VideoGetSWMIXED();
    const bool col80 = video.VideoGetSW80COL();
    const bool alt = video.VideoGetSWAltCharSet();
    // With 80STORE on, PAGE2 switches memory banks rather than the displayed page: the display stays on page 1.
    const WORD page = (video.VideoGetSWPAGE2() && !video.VideoGetSW80STORE()) ? 0x800 : 0x400;

    TextScreen ts;
    ts.emulatedCycles = EmulatedCycles();
    ts.mode = text ? (col80 ? "80col" : "40col") : (mixed ? (col80 ? "mixed80" : "mixed40") : "graphics");
    if (!text && !mixed)
        return ts;   // 24 empty lines
    for (int row = text ? 0 : 20; row < 24; ++row) {
        const WORD addr = WORD(page + 0x80 * (row % 8) + 0x28 * (row / 8));
        // These return the live main / aux contents even when 80STORE + PAGE2 has swapped the pages in the CPU's view.
        const BYTE* mainRow = MemGetMainPtr(addr);
        const BYTE* auxRow = col80 ? MemGetAuxPtr(addr) : nullptr;
        std::string& line = ts.lines[size_t(row)];
        if (!col80) {
            line.resize(40);
            for (int i = 0; i < 40; ++i)
                line[size_t(i)] = TextCodeToAscii(mainRow[i], alt);
        } else {
            line.assign(80, ' ');
            for (int i = 0; i < 40; ++i) {
                if (auxRow) line[size_t(2 * i)] = TextCodeToAscii(auxRow[i], alt);
                line[size_t(2 * i + 1)] = TextCodeToAscii(mainRow[i], alt);
            }
        }
    }
    return ts;
}

const std::vector<std::string>& Emulator::Messages() const
{
    RequireStarted();
    return HostFrame().messages();
}

const AudioCapture& Emulator::Audio() const
{
    RequireStarted();
    return Capture();
}

// Windowed playback is AudioOutput's (D14).
uint64_t Emulator::SoundUnderruns() const
{
    RequireStarted();
    return g_audioOutput ? g_audioOutput->Underruns() : 0;
}

bool Emulator::SoundActive() const
{
    return g_started && g_audioOutput != nullptr;
}

// ---- Speed controls (D11): AppleWin's own calls. Session settings: PowerCycle (ResetMachineState) keeps them.
void Emulator::SetMachineSpeed(uint32_t speed)
{
    RequireStarted();
    if (speed > SPEED_MAX)
        throw std::out_of_range("SetMachineSpeed: speed must be 0..40");
    g_sessionSpeed = speed;   // AppleWin's clock follows at the next realtime step (ApplyClock)
}

void Emulator::UseClockMultiplier(double multiplier)
{
    RequireStarted();
    if (!std::isfinite(multiplier) || multiplier <= 0.0)
        throw std::invalid_argument("UseClockMultiplier: the multiplier must be finite and above 0");
    // AppleWin's ::UseClockMultiplier conversion (Core.cpp), copied because the original also applies the clock at
    // once (see ApplyClock); a test pins the copy to AppleWin's function. Clamped first: (ULONG)(x * 10) is undefined
    // above about 4.29e8, and every x of 3.9 or more gives 39 anyway.
    double x = (std::min)(multiplier, 4.0);
    uint32_t speed;
    if (x < 1.0) {
        if (x < 0.5)
            x = 0.5;
        speed = uint32_t((x - 0.5) * 20);   // [0.5..0.9] -> [0..9]
    } else {
        speed = uint32_t(x * 10);
        if (speed >= SPEED_MAX)
            speed = SPEED_MAX - 1;
    }
    g_sessionSpeed = speed;
}

void Emulator::SetEnhanceDiskSpeed(bool enable)
{
    RequireStarted();
    GetCardMgr().GetDisk2CardMgr().SetEnhanceDisk(enable);
}

void Emulator::SetFullSpeedOverride(bool enable)
{
    RequireStarted();
    g_fullSpeedOverride = enable;
}

SpeedSettings Emulator::Speed() const
{
    RequireStarted();
    return SpeedSettings{g_sessionSpeed, GetCardMgr().GetDisk2CardMgr().GetEnhanceDisk(), g_fullSpeedOverride};
}

double Emulator::EffectiveClockHz() const
{
    RequireStarted();
    return ClockForSpeed(g_sessionSpeed);
}

std::vector<std::string> Emulator::FullSpeedReasons() const
{
    RequireStarted();
    const unsigned bits = FullSpeedReasonBits();
    std::vector<std::string> reasons;
    if (bits & 1u) reasons.emplace_back("machine_speed_max");
    if (bits & 2u) reasons.emplace_back("override");
    if (bits & 4u) reasons.emplace_back("disk_motor");
    return reasons;
}

bool Emulator::FullSpeedActive() const
{
    RequireStarted();
    return FullSpeedReasonBits() != 0;
}

bool Emulator::FullSpeedStepping() const
{
    RequireStarted();
    return g_realtime && g_bFullSpeed;
}

// ---- Checkpoints (docs/specs/m4.md; D5, D13, rulings D1, D2, D6, D7, D8).
namespace {

// Disk2InterfaceCard keeps its drives private, and the image type (list_disks' format) has no accessor. A pointer to
// the private member is taken through an explicit instantiation (where access checks do not apply); no AppleWin file
// is changed.
struct Disk2DrivesTag {
    using type = FloppyDrive (Disk2InterfaceCard::*)[NUM_DRIVES];
    friend type Disk2Drives(Disk2DrivesTag);
};
template <typename Tag, typename Tag::type Member>
struct PrivateMember {
    friend typename Tag::type Disk2Drives(Tag) { return Member; }
};
template struct PrivateMember<Disk2DrivesTag, &Disk2InterfaceCard::m_floppyDrive>;

std::string ImageFormat(Disk2InterfaceCard& card, int drive)
{
    const ImageInfo* info = (card.*Disk2Drives(Disk2DrivesTag{}))[drive].m_disk.m_imagehandle;
    if (!info || !info->pImageType)
        return "UNKNOWN";
    switch (info->pImageType->GetType()) {
    case eImageDO: return "DO";
    case eImagePO: return "PO";
    case eImageNIB1: case eImageNIB2: return "NIB";
    case eImageHDV: return "HDV";
    case eImageIIE: return "IIE";
    case eImageAPL: return "APL";
    case eImagePRG: return "PRG";
    case eImageWOZ1: case eImageWOZ2: return "WOZ";
    default: return "UNKNOWN";
    }
}

// The Disk II motor: Disk2InterfaceCard::IsConditionForFullSpeed() is motor && enhance, and SetEnhanceDisk has no side
// effects, so enhance is set for the question and put back.
bool Disk2MotorOn(Disk2InterfaceCard& card)
{
    const bool enhance = card.GetEnhanceDisk();
    card.SetEnhanceDisk(true);
    const bool motor = card.IsConditionForFullSpeed();
    card.SetEnhanceDisk(enhance);
    return motor;
}

// The images EjectAll took out, for ReinsertAll.
struct EjectedImage {
    int drive = 0;
    std::string path;
    bool writeProtected = false;
};
std::vector<EjectedImage> g_ejected;

// A FrameMessageBox call since `before` (messages count) becomes an exception carrying every new message.
void ThrowIfNewMessages(size_t before, const std::string& what)
{
    const auto& msgs = HostFrame().messages();
    if (msgs.size() == before)
        return;
    std::string text = what;
    for (size_t i = before; i < msgs.size(); ++i)
        text += "\n  " + msgs[i];
    throw std::runtime_error(text);
}

// A YAML scalar's value as libyaml hands it to YamlLoadHelper, then with YamlLoadHelper::LoadString_NoThrow's
// backslash un-doubling (YamlHelper.cpp): exactly the bytes LoadSnapshotFloppy passes to GetFileAttributes.
// YamlSaveHelper::SaveString writes a plain scalar, or a double-quoted one when the text contains '#'. A double-quoted
// value with an escape other than \\ or \" is refused (std::nullopt), which is conservative.
std::optional<std::string> YamlStringValue(std::string v)
{
    const size_t b = v.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return std::string();
    const size_t e = v.find_last_not_of(" \t\r\n");
    v = v.substr(b, e - b + 1);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        std::string out;
        for (size_t i = 1; i + 1 < v.size(); ++i) {
            if (v[i] != '\\') {
                out += v[i];
                continue;
            }
            if (i + 2 >= v.size() || (v[i + 1] != '\\' && v[i + 1] != '"'))
                return std::nullopt;
            out += v[++i];
        }
        v = out;
    } else if (v.size() >= 2 && v.front() == '\'' && v.back() == '\'') {
        std::string out;
        for (size_t i = 1; i + 1 < v.size(); ++i) {
            out += v[i];
            if (v[i] == '\'' && i + 2 < v.size() && v[i + 1] == '\'')
                ++i;
        }
        v = out;
    }
    for (size_t pos = 0; (pos = v.find("\\\\", pos)) != std::string::npos; ++pos)   // LoadString_NoThrow
        v.replace(pos, 2, "\\");
    return v;
}

// The disk images a save state names: each "Filename" (the bare name LoadSnapshotFloppy tries first, relative to the
// current directory) with the "Absolute Path" after it, as the bytes AppleWin's load will use.
struct NamedImage {
    std::string filename, absolutePath;
};

std::vector<NamedImage> ImagesNamedBy(const std::filesystem::path& stateFile)
{
    std::ifstream in(stateFile, std::ios::binary);
    if (!in)
        throw std::runtime_error("LoadSnapshot: cannot read " + Shown(stateFile));
    std::vector<NamedImage> images;
    std::string line;
    static const std::string kFilename = "Filename:", kAbsolute = "Absolute Path:";
    auto value = [&](size_t from) {
        const std::optional<std::string> v = YamlStringValue(line.substr(from));
        if (!v)
            throw std::runtime_error("LoadSnapshot: the save state names a disk image the harness cannot check: " + line);
        return *v;
    };
    while (std::getline(in, line)) {
        const size_t b = line.find_first_not_of(' ');
        if (b == std::string::npos)
            continue;
        if (line.compare(b, kFilename.size(), kFilename) == 0) {
            images.push_back(NamedImage{value(b + kFilename.size()), {}});
        } else if (line.compare(b, kAbsolute.size(), kAbsolute) == 0 && !images.empty() &&
                   images.back().absolutePath.empty()) {
            images.back().absolutePath = value(b + kAbsolute.size());
        }
    }
    return images;
}

// The identity of an existing file (volume and file index), for "is it the same file"; nullopt if it cannot be opened.
std::optional<std::pair<DWORD, uint64_t>> FileIdentityA(const std::string& path)
{
    HANDLE h = CreateFileA(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return std::nullopt;
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL ok = GetFileInformationByHandle(h, &info);
    CloseHandle(h);
    if (!ok)
        return std::nullopt;
    return std::make_pair(info.dwVolumeSerialNumber, (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow);
}

// Ruling D7 and the dialog rule. Disk2InterfaceCard::LoadSnapshotFloppy (Disk.cpp) passes the loaded bytes to the
// narrow GetFileAttributes (ANSI code page): first the bare file name (relative to the current directory), then the
// absolute path; if neither is found it opens a file dialog (UserSelectNewDiskImage). The same lookups are made here,
// on the same bytes, before loading, with the same current directory (the caller holds a ScopedCurrentDirectory).
// Refused: an image neither finds (the save state's UTF-8 bytes of a non-ASCII path do not name the file in the ANSI
// code page, for one), a directory, and a bare name that finds a different file than the absolute path. With
// expected, the save state must also name exactly those images (as absolute paths, compared case-insensitively).
void CheckImagesExist(const std::filesystem::path& stateFile, const std::vector<std::filesystem::path>* expected)
{
    size_t named = 0;
    for (const NamedImage& image : ImagesNamedBy(stateFile)) {
        if (image.filename.empty())
            continue;   // an empty drive: no lookup
        ++named;
        if (expected) {
            const std::wstring absolute = std::filesystem::path(image.absolutePath).lexically_normal().native();
            const bool listed = std::any_of(expected->begin(), expected->end(), [&](const std::filesystem::path& p) {
                // The save state holds the path AppleWin was given at insert (D10: possibly the short path).
                const std::wstring w = std::filesystem::path(ImagePathForAppleWin(p)).lexically_normal().native();
                return CompareStringOrdinal(w.c_str(), int(w.size()), absolute.c_str(), int(absolute.size()), TRUE) ==
                       CSTR_EQUAL;
            });
            if (!listed)
                throw std::runtime_error("LoadSnapshot: the save state names a disk image the checkpoint has no copy "
                                         "of: " + image.absolutePath);
        }
        std::string used = image.filename;
        DWORD attributes = GetFileAttributesA(used.c_str());
        const bool bareFound = attributes != INVALID_FILE_ATTRIBUTES;
        if (!bareFound && !image.absolutePath.empty()) {
            used = image.absolutePath;
            attributes = GetFileAttributesA(used.c_str());
        }
        if (attributes == INVALID_FILE_ATTRIBUTES)
            throw std::runtime_error("LoadSnapshot: disk image not found as AppleWin will look it up: " +
                                     (image.absolutePath.empty() ? image.filename : image.absolutePath));
        if (attributes & FILE_ATTRIBUTE_DIRECTORY)
            throw std::runtime_error("LoadSnapshot: the save state's disk image is a directory: " + used);
        if (bareFound) {
            const auto bare = FileIdentityA(image.filename);
            const auto absolute = image.absolutePath.empty() ? std::nullopt : FileIdentityA(image.absolutePath);
            if (!bare || !absolute || *bare != *absolute)
                throw std::runtime_error("LoadSnapshot: the save state's disk image " + image.filename +
                                         " would be found in the current directory, not at " + image.absolutePath);
        }
    }
    if (expected && named != expected->size())
        throw std::runtime_error("LoadSnapshot: the save state names " + std::to_string(named) +
                                 " disk image(s), the checkpoint has " + std::to_string(expected->size()));
}

// Review I-1: the bare file name LoadSnapshotFloppy tries first is looked up in the process's current directory, so a
// user's same-named image there would be found (and refused). Around the image checks and Snapshot_LoadState the
// current directory is the checkpoint's own directory, whose files are state.yaml, meta.json and disk<N>-<name>; the
// previous one is put back on every path. Only the main thread (which owns the emulator) runs harness code that uses
// paths while it is switched; the pipe threads use no relative paths.
class ScopedCurrentDirectory {
public:
    explicit ScopedCurrentDirectory(const std::filesystem::path& dir)
    {
        const DWORD n = GetCurrentDirectoryW(0, nullptr);
        if (n == 0)
            throw std::runtime_error("LoadSnapshot: cannot read the current directory (error " +
                                     std::to_string(GetLastError()) + ")");
        previous_.resize(n);
        const DWORD got = GetCurrentDirectoryW(n, previous_.data());
        if (got == 0 || got >= n)
            throw std::runtime_error("LoadSnapshot: cannot read the current directory");
        previous_.resize(got);
        if (!SetCurrentDirectoryW(dir.c_str()))
            throw std::runtime_error("LoadSnapshot: cannot make " + Shown(dir) + " the current directory (error " +
                                     std::to_string(GetLastError()) + ")");
    }
    ~ScopedCurrentDirectory() { SetCurrentDirectoryW(previous_.c_str()); }
    ScopedCurrentDirectory(const ScopedCurrentDirectory&) = delete;
    ScopedCurrentDirectory& operator=(const ScopedCurrentDirectory&) = delete;

private:
    std::wstring previous_;
};

// Everything that can be checked about a save state without touching the machine or the drives (ruling D7), with the
// current directory already switched; every failure becomes a SnapshotPreLoadError.
void PreLoadChecks(const std::filesystem::path& stateFile, const std::vector<std::filesystem::path>* expected)
{
    try {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(stateFile, ec))
            throw SnapshotPreLoadError("LoadSnapshot: no save state at " + Shown(stateFile));
        CheckImagesExist(stateFile, expected);
    } catch (const SnapshotPreLoadError&) {
        throw;
    } catch (const std::exception& e) {   // runtime_error from the image checks, filesystem_error, bad_alloc...
        const std::string what = e.what();
        throw SnapshotPreLoadError(what.rfind("LoadSnapshot:", 0) == 0 ? what : "LoadSnapshot: " + what);
    }
}

// The save state's absolute path (made before the current directory changes).
std::filesystem::path AbsoluteStateFile(const std::filesystem::path& stateFile)
{
    std::error_code ec;
    std::filesystem::path p = std::filesystem::absolute(stateFile, ec);
    if (ec)
        throw SnapshotPreLoadError("LoadSnapshot: cannot make " + Shown(stateFile) + " absolute: " + ec.message());
    return p.make_preferred();
}

// The save state's own directory made current; a failure is a SnapshotPreLoadError (nothing has been touched yet).
std::unique_ptr<ScopedCurrentDirectory> EnterStateDirectory(const std::filesystem::path& absoluteStateFile)
{
    try {
        return std::make_unique<ScopedCurrentDirectory>(absoluteStateFile.parent_path());
    } catch (const std::exception& e) {
        throw SnapshotPreLoadError(e.what());
    }
}

// After a failed Snapshot_LoadState (which has removed every card, writing that to the config file, and may have
// inserted only some again): the slot cards and the aux card the machine had, in the machine and in the config file,
// so that InsertDisk, PowerCycle and a later Start see the session's cards.
struct CardLayout {
    std::array<SS_CARDTYPE, NUM_SLOTS> slots{};
    SS_CARDTYPE aux = CT_Empty;
};

CardLayout CurrentCards()
{
    CardLayout c;
    for (UINT slot = SLOT0; slot < NUM_SLOTS; ++slot)
        c.slots[slot] = GetCardMgr().QuerySlot(slot);
    c.aux = GetCardMgr().QueryAux();
    return c;
}

void RestoreCards(const CardLayout& c)
{
    CardManager& mgr = GetCardMgr();
    for (UINT slot = SLOT0; slot < NUM_SLOTS; ++slot) {
        if (mgr.QuerySlot(slot) == c.slots[slot])
            continue;
        if (slot == SLOT0) {
            if (c.slots[slot] == CT_LanguageCardIIe)
                ::CreateLanguageCard();
            else
                SetExpansionMemType(c.slots[slot]);
        } else if (c.slots[slot] == CT_Empty) {
            mgr.Remove(slot);
        } else {
            mgr.Insert(slot, c.slots[slot]);
        }
    }
    if (mgr.QueryAux() != c.aux)
        mgr.InsertAux(c.aux);
}

}  // namespace

SnapshotMeta Emulator::SaveSnapshot(const std::filesystem::path& stateFile)
{
    RequireStarted();
    g_realtime.reset();   // M4-7: the realtime run ends (full speed and the Mockingboard mute are cleared)
    SnapshotMeta meta;
    meta.emulatedCycles = EmulatedNow();
    meta.powerOnOrigin = g_cyclesAtPowerOn;
    meta.speed = Speed();
    const JoyState joy = SettledJoystick();
    meta.joystickX = joy.x;
    meta.joystickY = joy.y;

    if (GetCardMgr().QuerySlot(SLOT6) == CT_Disk2) {
        Disk2InterfaceCard& card = Disk2Card();
        for (int drive = DRIVE_1; drive <= DRIVE_2; ++drive) {
            if (card.IsDriveEmpty(drive))
                continue;
            if (!SurvivesSaveState(card.DiskGetFullPathName(drive)) || !SurvivesSaveState(card.GetFullName(drive)))
                throw std::runtime_error("Emulator::SaveSnapshot: the disk image path " + card.DiskGetFullPathName(drive) +
                                         " has characters AppleWin's save state cannot bring back (its file names are "
                                         "UTF-8 in the save state but ANSI when loaded); no checkpoint was written");
        }
    }

    const size_t before = HostFrame().messages().size();
    Snapshot_SetFilename(StatePathForAppleWin(std::filesystem::path(stateFile).make_preferred()));
    Snapshot_SaveState();
    ThrowIfNewMessages(before, "Emulator::SaveSnapshot: AppleWin could not save " + Shown(stateFile) + ":");
    return meta;
}

void Emulator::CheckSnapshot(const std::filesystem::path& stateFile,
                             const std::vector<std::filesystem::path>& expectedImages) const
{
    RequireStarted();
    const std::filesystem::path state = AbsoluteStateFile(stateFile);
    const auto cwd = EnterStateDirectory(state);
    PreLoadChecks(state, &expectedImages);
}

void Emulator::LoadSnapshot(const std::filesystem::path& stateFile, const SnapshotMeta& meta)
{
    RequireStarted();
    g_realtime.reset();   // no open realtime run: g_bFullSpeed clear, the Mockingboard unmuted

    // Ruling D7: everything that can be checked before the load is checked first; a failure here changes nothing.
    // Review I-1: the checks and the load run with the save state's directory current (see ScopedCurrentDirectory).
    const std::filesystem::path state = AbsoluteStateFile(stateFile);
    const bool sessionEnhance = GetCardMgr().GetDisk2CardMgr().GetEnhanceDisk();   // D8: the session's setting
    const CardLayout cards = CurrentCards();
    const size_t before = HostFrame().messages().size();
    {
        const auto cwd = EnterStateDirectory(state);
        PreLoadChecks(state, nullptr);
        Snapshot_SetFilename(StatePathForAppleWin(state));
        Snapshot_LoadState();
    }
    g_nAppMode = MODE_RUNNING;   // as before the load (AppleWin's own load leaves it; a failed one may not)

    if (HostFrame().messages().size() != before) {
        // AppleWin reported the error and asked for a restart (HarnessFrame::Restart only records it). Put the cards
        // back so that InsertDisk and PowerCycle work; the machine state is undefined until PowerCycle.
        try { RestoreCards(cards); } catch (...) {}
        DiscardInput();
        ThrowIfNewMessages(before, "Emulator::LoadSnapshot: AppleWin could not load " + Shown(stateFile) + ":");
    }

    // Cycle origin: EmulatedCycles() continues from the checkpoint's (the save state restores g_nCumulativeCycles).
    if (g_nCumulativeCycles < meta.emulatedCycles || g_nCumulativeCycles - meta.emulatedCycles != meta.powerOnOrigin) {
        DiscardInput();
        throw std::runtime_error("Emulator::LoadSnapshot: the checkpoint's metadata does not match its save state");
    }
    g_cyclesAtPowerOn = meta.powerOnOrigin;

    // Restore purity (docs/specs/m4.md), in the spec's order.
    // Speed (D8): the session's settings. Machine speed and the full-speed override are harness state the load does not
    // touch; enhanced disk access is Disk II card state the save state carries, so the session's is put back. AppleWin's
    // clock is set to the stepped clock unconditionally (it re-initializes the speaker and Mockingboard clocks).
    GetCardMgr().GetDisk2CardMgr().SetEnhanceDisk(sessionEnhance);
    CanonicalClock();
    // Random numbers (M4-3, D2, D13): RNDL/RNDH come from the snapshot's RAM; only the C runtime's stream is reset.
    std::srand(1);
    // Speaker (M4-5): every internal AppleWin does not save, back to a fresh process's.
    NeutralizeSpeaker();
    // Mockingboard: the manager's audio-frame counter (when it next mixes, with no 6522 timer running) is not saved.
    // The cards themselves are new (the load replaces every card) and their 6522 timers come from the save state.
    GetCardMgr().GetMockingboardCardMgr().Reset(false);
    GetCardMgr().GetMockingboardCardMgr().MuteControl(false);
    // Video scanner: AppleWin's save state restores g_dwCyclesThisFrame and the load sets NTSC's clocks from it (kept).
    // The flash phase is set from the restored cycle as a stepped run from power-on has it (one wrap per frame), less
    // the one wrap of the re-render below.
    const uint32_t framePhase = uint32_t((meta.emulatedCycles / NTSC_GetCyclesPerFrame()) % kFlashPeriod);
    AdvanceFlashTo(framePhase + kFlashPeriod - RedrawsPerPresent());
    // Harness state (M4-4): pending input discarded (reported), held keys and buttons released, the joystick where the
    // checkpoint left it (AppleWin saves no joystick position).
    DiscardInput();
    JoySetJoyType(JN_JOYSTICK0, J0C_MOUSE);
    JoyState joy;
    joy.x = meta.joystickX;
    joy.y = meta.joystickY;
    ApplyJoy(joy);
    Capture().Clear();   // audio from before the restore is gone; every voice's next commit is placed afresh
    RenderPresentedFrame();
    g_ejected.clear();   // the restored images are in; nothing is left to reinsert
}

std::string Emulator::StateChecksum() const
{
    RequireStarted();
    std::vector<uint8_t> b;
    b.reserve(2 * 64 * 1024 + 64);
    auto put8 = [&](uint32_t v) { b.push_back(uint8_t(v)); };
    auto put16 = [&](uint32_t v) { put8(v); put8(v >> 8); };
    auto put32 = [&](uint32_t v) { put16(v); put16(v >> 16); };
    auto putBytes = [&](const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); };

    // 1. CPU registers.
    put8(regs.a); put8(regs.x); put8(regs.y); put8(regs.ps); put16(regs.sp); put16(regs.pc);
    // 2. Memory mode.
    put32(GetMemMode());
    // 3. Main RAM (64 KiB, the language-card banks included: AppleWin keeps LC bank 1 at $C000-$CFFF), then the aux
    // card's RAM as MemSaveSnapshotAux saves it. MemGetBankPtr flushes the dirty shadow pages first (BackMainImage).
    putBytes(MemGetBankPtr(0), 64 * 1024);
    const SS_CARDTYPE aux = GetCardMgr().QueryAux();
    put8(uint32_t(aux));
    if (aux == CT_80Col) {
        putBytes(MemGetBankPtr(1) + TEXT_PAGE1_BEGIN, TEXT_PAGE1_SIZE);
    } else if (aux == CT_Extended80Col || aux == CT_RamWorksIII) {
        for (UINT bank = 1;; ++bank) {
            const uint8_t* p = MemGetBankPtr(bank);
            if (!p || (aux == CT_Extended80Col && bank > 1))
                break;
            putBytes(p, 64 * 1024);
        }
    }
    // 4. Keyboard latch and strobe.
    put8(KeybReadData());
    // 5. Video soft switches.
    put32(GetVideo().GetVideoMode());
    put8(GetVideo().VideoGetSWAltCharSet() ? 1 : 0);
    // 6. Disk II controller.
    if (GetCardMgr().QuerySlot(SLOT6) == CT_Disk2) {
        Disk2InterfaceCard& card = Disk2Card();
        put8(Disk2MotorOn(card) ? 1 : 0);
        put8(uint32_t(card.GetCurrentDrive()));
        for (int drive = DRIVE_1; drive <= DRIVE_2; ++drive) {
            const float phase = card.GetPhase(drive);
            uint32_t bits = 0;
            std::memcpy(&bits, &phase, sizeof(bits));
            put32(bits);
            put32(uint32_t(card.GetTrack(drive)));
        }
    } else {
        put8(0xFF);
    }
    // 7. Input state (ruling D6).
    put8(AnyKeyDown() ? 1 : 0);
    put8(g_input.joy.b0 ? 1 : 0);
    put8(g_input.joy.b1 ? 1 : 0);
    put8(g_input.joy.x);
    put8(g_input.joy.y);
    return Sha256Hex(b);
}

bool Emulator::RunCycles(uint64_t cycles)
{
    RequireStarted();
    g_realtime.reset();
    if (cycles == 0)
        return true;
    Stepper stepper(Pacing::Stepped);
    const uint64_t target = g_nCumulativeCycles + cycles;
    while (g_nCumulativeCycles < target) {
        if (stepper.Step(target - g_nCumulativeCycles))
            return false;   // the window was closed (as RunFrames)
    }
    return true;
}

std::vector<DiskInfo> Emulator::Disks() const
{
    RequireStarted();
    std::vector<DiskInfo> disks;
    if (GetCardMgr().QuerySlot(SLOT6) != CT_Disk2)
        return disks;
    Disk2InterfaceCard& card = Disk2Card();
    for (int drive = DRIVE_1; drive <= DRIVE_2; ++drive) {
        if (card.IsDriveEmpty(drive))
            continue;
        DiskInfo d;
        d.drive = drive;
        d.workingPath = std::filesystem::path(card.DiskGetFullPathName(drive));
        const auto longPath = g_longPaths.find(UpperAscii(card.DiskGetFullPathName(drive)));
        if (longPath != g_longPaths.end())
            d.workingPath = longPath->second;   // D10: AppleWin holds the short path
        d.writeProtected = card.GetProtect(drive);
        d.format = ImageFormat(card, drive);
        disks.push_back(std::move(d));
    }
    return disks;
}

void Emulator::FlushDisks()
{
    RequireStarted();
    if (GetCardMgr().QuerySlot(SLOT6) != CT_Disk2)
        return;
    Disk2InterfaceCard& card = Disk2Card();
    for (int drive = DRIVE_1; drive <= DRIVE_2; ++drive)
        if (!card.IsDriveEmpty(drive))
            card.FlushCurrentTrack(drive);
}

uint32_t Emulator::DiskWriteLight(int drive) const
{
    RequireStarted();
    if (drive < DRIVE_1 || drive > DRIVE_2 || GetCardMgr().QuerySlot(SLOT6) != CT_Disk2)
        return 0;
    Disk2InterfaceCard& card = Disk2Card();
    if (card.IsDriveEmpty(drive))
        return 0;
    return (card.*Disk2Drives(Disk2DrivesTag{}))[drive].m_writelight;
}

void Emulator::EjectAll()
{
    RequireStarted();
    g_ejected.clear();
    if (GetCardMgr().QuerySlot(SLOT6) != CT_Disk2)
        return;
    Disk2InterfaceCard& card = Disk2Card();
    for (int drive = DRIVE_1; drive <= DRIVE_2; ++drive) {
        if (card.IsDriveEmpty(drive))
            continue;
        g_ejected.push_back(EjectedImage{drive, card.DiskGetFullPathName(drive), card.GetProtect(drive)});
        card.FlushCurrentTrack(drive);
        card.EjectDisk(drive);   // the drive's head and phase stay (Disk2InterfaceCard keeps drive attributes)
    }
}

void Emulator::ReinsertAll()
{
    RequireStarted();
    const std::vector<EjectedImage> images = std::exchange(g_ejected, {});   // used once
    if (images.empty())
        return;
    Disk2InterfaceCard& card = Disk2Card();
    for (const EjectedImage& image : images) {
        const size_t before = HostFrame().messages().size();
        const ImageError_e err = card.InsertDisk(image.drive, image.path, image.writeProtected, false);
        if (err != eIMAGE_ERROR_NONE) {
            card.NotifyInvalidImage(image.drive, image.path, err);
            std::string what = "Emulator: cannot insert " + image.path + " (image error " + std::to_string(int(err)) + ")";
            const auto& msgs = HostFrame().messages();
            for (size_t i = before; i < msgs.size(); ++i)
                what += "\n  " + msgs[i];
            throw std::runtime_error(what);
        }
    }
}

}  // namespace harness
