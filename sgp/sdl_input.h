#ifndef __SDL_INPUT_H
#define __SDL_INPUT_H

// SDL3 event translation seam for the minimal Windows-only SDL3 port.
//
// The main loop in sgp.cpp pumps SDL_PollEvent and hands each event here;
// SgpHandleSDLEvent() translates it into the existing JA2 input queue
// (QueueEvent / KeyDown / KeyUp) and live input globals, exactly as the old
// Win32 WH_MOUSE hook + WindowProcedure WM_KEY* path did. Returns TRUE when
// the event requests application exit (window close / quit).

#include "types.h"

union SDL_Event;

#ifdef __cplusplus
extern "C" {
#endif

// Translate a single SDL event. Returns TRUE to request program exit.
BOOLEAN SgpHandleSDLEvent(const SDL_Event *event);

#ifdef __cplusplus
}
#endif

#endif
