// SDL3 -> JA2 input translation.
//
// Windows-only minimal SDL3 port. This TU replaces the Win32 WH_MOUSE hook
// (sgp/input.cpp) and the WM_KEYDOWN/WM_KEYUP/WM_CHAR dispatch that used to
// live in sgp/sgp.cpp's WindowProcedure. It feeds SDL events into the
// unchanged JA2 event queue and key/mouse globals defined in input.cpp.
//
// The real keyboard/mouse translation is wired up together with the SDL
// windowing + video seams; this initial revision only establishes the SDL3
// build integration (headers compile, SDL3 import lib links) so the switch
// from DirectDraw can land on a proven toolchain foundation.

#include <SDL3/SDL.h>

#include "sdl_input.h"

extern "C" BOOLEAN SgpHandleSDLEvent(const SDL_Event *event)
{
	if (event == NULL) return FALSE;

	switch (event->type)
	{
		case SDL_EVENT_QUIT:
		case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
			return TRUE;

		default:
			break;
	}

	return FALSE;
}
