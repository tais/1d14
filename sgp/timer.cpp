	#include "types.h"
	#include <windows.h>
	#include "timer.h"

#ifndef WIN32_LEAN_AND_MEAN
	#define WIN32_LEAN_AND_MEAN
#endif

UINT32 guiStartupTime;
UINT32 guiCurrentTime;

// SGP 'local' millisecond clock. This used to advance from a Win32 WM_TIMER
// (SetTimer(ghWindow, MAIN_TIMER_ID, 10, Clock)) dispatched by the game's
// message pump. With SDL3 owning the window there is no GetMessage/Dispatch
// loop, so instead we sample GetTickCount() on demand: guiCurrentTime is the
// elapsed milliseconds since startup (loopback-corrected), computed each time
// the clock is read. Semantics of guiStartupTime/guiCurrentTime are unchanged.
static UINT32 SampleCurrentTime(void)
{
	UINT32 uiNow = GetTickCount();
	if (uiNow < guiStartupTime)
	{	// Adjust because of loopback (wrap-around) on the tick value
		return uiNow + (0xffffffff - guiStartupTime);
	}
	// Normal case
	return uiNow - guiStartupTime;
}

BOOLEAN InitializeClockManager(void)
{
	// Register the start time (use WIN95 API call)
	guiCurrentTime = guiStartupTime = GetTickCount();

	return TRUE;
}

void	ShutdownClockManager(void)
{
	// Nothing to do: the on-demand clock owns no Win32 timer to kill.
}

TIMER	GetClock(void)
{
	guiCurrentTime = SampleCurrentTime();
	return guiCurrentTime;
}

TIMER	SetCountdownClock(UINT32 uiTimeToElapse)
{
	guiCurrentTime = SampleCurrentTime();
	return (guiCurrentTime + uiTimeToElapse);
}

UINT32 ClockIsTicking(TIMER uiTimer)
{
	guiCurrentTime = SampleCurrentTime();
	if (uiTimer > guiCurrentTime)
	{ // Well timer still hasn't elapsed
	return (uiTimer - guiCurrentTime);
	}
	// Time's up
	return 0;
}
