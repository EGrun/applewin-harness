#pragma once
#include <cstdint>

namespace harness {

// One emulation step, as ContinueExecution in Windows/AppleWin.cpp does it with video on and without pacing or
// full-speed handling: CpuExecute(cycles, true), then g_dwCyclesThisFrame, GetCardMgr().Update and SpkrUpdate, then the
// end-of-frame check that calls GetFrame().VideoPresentScreen(). Returns the cycles executed.
uint32_t ExecuteStep(uint32_t cycles);

}  // namespace harness
