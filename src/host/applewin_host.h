#pragma once
#include <cstdint>

namespace harness {

// One emulation step, as ContinueExecution in Windows/AppleWin.cpp does it, without pacing: CpuExecute(cycles,
// !fullSpeed), then g_dwCyclesThisFrame, GetCardMgr().Update and SpkrUpdate, then the end-of-frame check, which calls
// GetFrame().VideoPresentScreen(), or at full speed GetFrame().VideoRedrawScreenDuringFullSpeed(g_dwCyclesThisFrame)
// (a redraw at most every ~16 ms of wall time). fullSpeed requires g_bFullSpeed to be set (the caller does AppleWin's
// full-speed entry and exit). Returns the cycles executed.
uint32_t ExecuteStep(uint32_t cycles, bool fullSpeed = false);

}  // namespace harness
