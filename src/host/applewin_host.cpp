// Provides what external/AppleWin/source/Windows/AppleWin.cpp provides to the rest of AppleWin. The harness does not
// compile that file (it is AppleWin's WinMain, message loop and window startup).
//
// Factories (same shape as AppleWin.cpp: function-local statics):
//   GetFrame()          -> static harness::HarnessFrame (instead of Win32Frame)
//   GetVideo()          -> static Video
//   GetPropertySheet()  -> static CPropertySheet
//
// Symbols copied from AppleWin.cpp because the linker reported them unresolved. Definitions and initial values are as
// in AppleWin.cpp unless noted:
//   static bool g_bLoadedSaveState = false;
//   bool g_bRestartFullScreen = false;
//   static bool g_fullScreenResolutionChangedByUser = false;
//   bool GetLoadedSaveStateFlag();
//   void Win32Frame::SetLoadedSaveStateFlag(const bool);   (Win32Frame.obj is linked in, but never instantiated)
//   bool GetHookAltGrControl();
//   bool GetFullScreenResolutionChangedByUser();
//   void SingleStep(bool bReinit);   DIFFERS: AppleWin's runs ContinueExecution (pacing, scroll-lock via
//                                    Win32Frame::GetWin32Frame, full-speed). Here it runs one unpaced ExecuteStep(0),
//                                    i.e. one opcode, which is what ContinueExecution executes in MODE_STEPPING. It is
//                                    only reached from the debugger's DebugContinueStepping, which M1 never enters.
//
// No AppleWin file is modified.

#include "host/applewin_host.h"
#include "host/harness_frame.h"

#pragma warning(push, 3)
#include "Interface.h"
#include "Core.h"
#include "CmdLine.h"
#include "CPU.h"
#include "CardManager.h"
#include "NTSC.h"
#include "Speaker.h"
#include "Configuration/PropertySheet.h"
#include "Windows/AppleWin.h"
#include "Windows/Win32Frame.h"
#pragma warning(pop)

//===========================================================================
// Factories

IPropertySheet& GetPropertySheet()
{
    static CPropertySheet sg_PropertySheet;
    return sg_PropertySheet;
}

FrameBase& GetFrame()
{
    static harness::HarnessFrame sg_HarnessFrame;
    return sg_HarnessFrame;
}

Video& GetVideo()
{
    static Video video;
    return video;
}

//===========================================================================
// Copied from Windows/AppleWin.cpp

static bool g_bLoadedSaveState = false;

bool g_bRestartFullScreen = false;

static bool g_fullScreenResolutionChangedByUser = false;

bool GetLoadedSaveStateFlag()
{
    return g_bLoadedSaveState;
}

void Win32Frame::SetLoadedSaveStateFlag(const bool bFlag)
{
    g_bLoadedSaveState = bFlag;
}

bool GetHookAltGrControl()
{
    return g_bHookAltGrControl;
}

bool GetFullScreenResolutionChangedByUser()
{
    return g_fullScreenResolutionChangedByUser;
}

void SingleStep(bool /*bReinit*/)
{
    harness::ExecuteStep(0);
}

//===========================================================================

namespace harness {

uint32_t ExecuteStep(uint32_t cycles)
{
    const uint32_t executed = CpuExecute(cycles, true);
    g_dwCyclesThisFrame += executed;
    GetCardMgr().Update(executed);
    SpkrUpdate(executed);

    const UINT clksPerFrame = NTSC_GetCyclesPerFrame();
    if (g_dwCyclesThisFrame >= clksPerFrame && !GetVideo().VideoGetVblBarEx(g_dwCyclesThisFrame)) {
        g_dwCyclesThisFrame -= clksPerFrame;
        GetFrame().VideoPresentScreen();
    }
    return executed;
}

}  // namespace harness
