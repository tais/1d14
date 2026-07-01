// SDL3 -> JA2 input translation.
//
// Windows-only minimal SDL3 port. This TU replaces the Win32 WH_MOUSE hook
// (sgp/input.cpp) and the WM_KEYDOWN/WM_KEYUP dispatch that used to live in
// sgp/sgp.cpp's WindowProcedure. It feeds SDL events into the unchanged JA2
// event queue and key/mouse globals defined in input.cpp.
//
// Keyboard uses "Design 1": each SDL scancode is mapped to the Win32 virtual
// key (+ extended flag) that the old WM_KEYDOWN/WM_KEYUP path would have
// carried, then fed straight into the original KeyDown()/KeyUp() translator.
// That reuses gsKeyTranslationTable, the numpad NUMLOCK disambiguation, the
// shifted-symbol translation and the string-input redirect verbatim, and it
// keeps gfShiftState/gfCtrlState/gfAltState correct automatically (SHIFT/CTRL/
// ALT flow through KeyDown/KeyUp just like they did under the Win32 message
// pump). No key-translation logic is duplicated here.

#include <SDL3/SDL.h>
#include <windows.h>			// MapVirtualKey / VK_* constants

#include "sdl_input.h"
#include "input.h"

// gfApplicationActive is defined in sgp.cpp and gates GameLoop in the main
// loop; gfX1ButtonState/gfX2ButtonState are defined in input.cpp (not exposed
// through input.h). Everything else used below (QueueEvent, KeyDown, KeyUp,
// the event-code #defines, gusMouseXPos/gusMouseYPos, gfLeftButtonState/
// gfRightButtonState/gfMiddleButtonState, gsMouseWheelDeltaValue and
// gfSGPInputReceived) comes from input.h.
extern BOOLEAN gfApplicationActive;

// Map an SDL scancode to the Win32 virtual-key code (and its extended-key
// flag) that the legacy WM_KEYDOWN/WM_KEYUP path carried. Returns FALSE for
// keys we do not translate (they are simply ignored). US layout; the eventual
// character is produced downstream by gsKeyTranslationTable, so we only need
// the correct VK here.
static BOOLEAN MapScancodeToVK(SDL_Scancode sc, UINT16 *pVk, BOOLEAN *pExtended)
{
	UINT16	vk = 0;
	BOOLEAN	ext = FALSE;

	// Letters A-Z -> VK 'A'..'Z' (0x41-0x5A). SDL scancodes A..Z are contiguous.
	if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
	{
		vk = (UINT16)('A' + (sc - SDL_SCANCODE_A));
	}
	// Digits 1-9 -> '1'..'9' (SDL 1..9 contiguous; 0 sits after 9, handled below).
	else if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
	{
		vk = (UINT16)('1' + (sc - SDL_SCANCODE_1));
	}
	// Function keys F1-F12 -> VK_F1..VK_F12 (0x70-0x7B), contiguous in SDL.
	else if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12)
	{
		vk = (UINT16)(VK_F1 + (sc - SDL_SCANCODE_F1));
	}
	else
	{
		switch (sc)
		{
			case SDL_SCANCODE_0:				vk = '0';			break;

			// Editing / control keys
			case SDL_SCANCODE_RETURN:
			case SDL_SCANCODE_KP_ENTER:			vk = VK_RETURN;		break;	// 13
			case SDL_SCANCODE_ESCAPE:			vk = VK_ESCAPE;		break;	// 27
			case SDL_SCANCODE_SPACE:			vk = VK_SPACE;		break;	// 32
			case SDL_SCANCODE_TAB:				vk = VK_TAB;		break;	// 9
			case SDL_SCANCODE_BACKSPACE:		vk = VK_BACK;		break;	// 8

			// Modifiers -> generic VK (matches english.h SHIFT/CTRL/ALT = 16/17/18,
			// which KeyDown/KeyUp special-case to maintain gf*State).
			case SDL_SCANCODE_LSHIFT:
			case SDL_SCANCODE_RSHIFT:			vk = VK_SHIFT;		break;	// 16
			case SDL_SCANCODE_LCTRL:
			case SDL_SCANCODE_RCTRL:			vk = VK_CONTROL;	break;	// 17
			case SDL_SCANCODE_LALT:
			case SDL_SCANCODE_RALT:				vk = VK_MENU;		break;	// 18

			// Navigation cluster -> VK_* with the extended bit set so KeyChange
			// resolves them to the english.h symbolic nav codes (245-254) rather
			// than the numpad values.
			case SDL_SCANCODE_INSERT:			vk = VK_INSERT;	ext = TRUE;	break;	// 45
			case SDL_SCANCODE_DELETE:			vk = VK_DELETE;	ext = TRUE;	break;	// 46
			case SDL_SCANCODE_HOME:				vk = VK_HOME;	ext = TRUE;	break;	// 36
			case SDL_SCANCODE_END:				vk = VK_END;	ext = TRUE;	break;	// 35
			case SDL_SCANCODE_PAGEUP:			vk = VK_PRIOR;	ext = TRUE;	break;	// 33
			case SDL_SCANCODE_PAGEDOWN:			vk = VK_NEXT;	ext = TRUE;	break;	// 34
			case SDL_SCANCODE_LEFT:				vk = VK_LEFT;	ext = TRUE;	break;	// 37
			case SDL_SCANCODE_RIGHT:			vk = VK_RIGHT;	ext = TRUE;	break;	// 39
			case SDL_SCANCODE_UP:				vk = VK_UP;		ext = TRUE;	break;	// 38
			case SDL_SCANCODE_DOWN:				vk = VK_DOWN;	ext = TRUE;	break;	// 40

			// Numeric keypad -> VK_NUMPAD*/operators (NUMLOCK-on semantics).
			// KP_DIVIDE is the one extended keypad key.
			case SDL_SCANCODE_KP_0:				vk = VK_NUMPAD0;	break;
			case SDL_SCANCODE_KP_1:				vk = VK_NUMPAD1;	break;
			case SDL_SCANCODE_KP_2:				vk = VK_NUMPAD2;	break;
			case SDL_SCANCODE_KP_3:				vk = VK_NUMPAD3;	break;
			case SDL_SCANCODE_KP_4:				vk = VK_NUMPAD4;	break;
			case SDL_SCANCODE_KP_5:				vk = VK_NUMPAD5;	break;
			case SDL_SCANCODE_KP_6:				vk = VK_NUMPAD6;	break;
			case SDL_SCANCODE_KP_7:				vk = VK_NUMPAD7;	break;
			case SDL_SCANCODE_KP_8:				vk = VK_NUMPAD8;	break;
			case SDL_SCANCODE_KP_9:				vk = VK_NUMPAD9;	break;
			case SDL_SCANCODE_KP_PERIOD:		vk = VK_DECIMAL;	break;
			case SDL_SCANCODE_KP_PLUS:			vk = VK_ADD;		break;
			case SDL_SCANCODE_KP_MINUS:			vk = VK_SUBTRACT;	break;
			case SDL_SCANCODE_KP_MULTIPLY:		vk = VK_MULTIPLY;	break;
			case SDL_SCANCODE_KP_DIVIDE:		vk = VK_DIVIDE;	ext = TRUE;	break;

			// Punctuation / OEM keys (US layout) -> VK_OEM_*
			case SDL_SCANCODE_SEMICOLON:		vk = VK_OEM_1;		break;	// ; :
			case SDL_SCANCODE_EQUALS:			vk = VK_OEM_PLUS;	break;	// = +
			case SDL_SCANCODE_COMMA:			vk = VK_OEM_COMMA;	break;	// , <
			case SDL_SCANCODE_MINUS:			vk = VK_OEM_MINUS;	break;	// - _
			case SDL_SCANCODE_PERIOD:			vk = VK_OEM_PERIOD;	break;	// . >
			case SDL_SCANCODE_SLASH:			vk = VK_OEM_2;		break;	// / ?
			case SDL_SCANCODE_GRAVE:			vk = VK_OEM_3;		break;	// ` ~
			case SDL_SCANCODE_LEFTBRACKET:		vk = VK_OEM_4;		break;	// [ {
			case SDL_SCANCODE_BACKSLASH:		vk = VK_OEM_5;		break;	// \ |
			case SDL_SCANCODE_RIGHTBRACKET:		vk = VK_OEM_6;		break;	// ] }
			case SDL_SCANCODE_APOSTROPHE:		vk = VK_OEM_7;		break;	// ' "
			case SDL_SCANCODE_NONUSBACKSLASH:	vk = VK_OEM_102;	break;

			default:							vk = 0;				break;
		}
	}

	if (vk == 0)
		return FALSE;

	*pVk = vk;
	*pExtended = ext;
	return TRUE;
}

// Rebuild the Win32 lParam the KeyDown/KeyUp translator expects: hardware
// scancode in bits 16-23, extended flag in bit 24. KeyChange() reads both to
// disambiguate the numpad/nav keys.
static UINT32 BuildKeyLParam(UINT16 vk, BOOLEAN extended)
{
	UINT uiScanCode = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
	return (((UINT32)(uiScanCode & 0xFF)) << 16) | (extended ? EXT_CODE_MASK : 0);
}

// Pack mouse coords the way the old Win32 mouse hook packed them into the
// event's uiParam: (y << 16) | x. Consumers unpack via _EvMouseX/_EvMouseY.
static UINT32 PackMouseXY(int x, int y)
{
	return (((UINT32)(y & 0xFFFF)) << 16) | ((UINT32)(x & 0xFFFF));
}

extern "C" BOOLEAN SgpHandleSDLEvent(const SDL_Event *event)
{
	if (event == NULL)
		return FALSE;

	switch (event->type)
	{
		// ---- Application exit ------------------------------------------------
		case SDL_EVENT_QUIT:
		case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
			return TRUE;

		// ---- Focus / activation (gates GameLoop in the main loop) ------------
		case SDL_EVENT_WINDOW_FOCUS_GAINED:
		case SDL_EVENT_WINDOW_RESTORED:
			gfApplicationActive = TRUE;
			break;

		case SDL_EVENT_WINDOW_FOCUS_LOST:
		case SDL_EVENT_WINDOW_MINIMIZED:
			gfApplicationActive = FALSE;
			// Clear held input: a key/button RELEASE that happens while we are
			// unfocused (Alt-Tab, click onto another window) is delivered to the
			// other window, not us, so without this those globals stay TRUE and we
			// resume with a stuck button (spurious band-select / fire) or a stuck
			// Shift/Ctrl/Alt. Clearing gfKeyState also drops any stuck movement key.
			SDL_memset( gfKeyState, FALSE, sizeof(gfKeyState) );
			gfShiftState = gfCtrlState = gfAltState = 0;
			gfLeftButtonState = gfRightButtonState = gfMiddleButtonState = FALSE;
			gfX1ButtonState = gfX2ButtonState = FALSE;
			break;

		// ---- Keyboard (Design 1: reuse the original translator) --------------
		case SDL_EVENT_KEY_DOWN:
		{
			UINT16	vk;
			BOOLEAN	extended;
			if (MapScancodeToVK(event->key.scancode, &vk, &extended))
			{
				KeyDown(vk, BuildKeyLParam(vk, extended));
				gfSGPInputReceived = TRUE;
			}
			break;
		}
		case SDL_EVENT_KEY_UP:
		{
			UINT16	vk;
			BOOLEAN	extended;
			if (MapScancodeToVK(event->key.scancode, &vk, &extended))
			{
				KeyUp(vk, BuildKeyLParam(vk, extended));
				gfSGPInputReceived = TRUE;
			}
			break;
		}

		// ---- Mouse motion ----------------------------------------------------
		case SDL_EVENT_MOUSE_MOTION:
			// Window is 1:1 640x480, so SDL client coords are the game coords.
			// Do NOT QueueEvent(MOUSE_POS): the game polls gusMouseXPos/YPos
			// directly, and a MOUSE_POS atom sitting at the head of the queue
			// would pin all button atoms behind it (DequeueSpecificEvent bails
			// on a head that doesn't match its mouse-button mask).
			gusMouseXPos = (INT16)event->motion.x;
			gusMouseYPos = (INT16)event->motion.y;
			gfSGPInputReceived = TRUE;
			break;

		// ---- Mouse buttons ---------------------------------------------------
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
		{
			const BOOLEAN	down = (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
			const UINT32	uiParam = PackMouseXY((int)event->button.x, (int)event->button.y);
			UINT16			usEvent = 0;

			switch (event->button.button)
			{
				case SDL_BUTTON_LEFT:
					gfLeftButtonState = down;
					usEvent = down ? LEFT_BUTTON_DOWN : LEFT_BUTTON_UP;
					break;
				case SDL_BUTTON_RIGHT:
					gfRightButtonState = down;
					usEvent = down ? RIGHT_BUTTON_DOWN : RIGHT_BUTTON_UP;
					break;
				case SDL_BUTTON_MIDDLE:
					gfMiddleButtonState = down;
					usEvent = down ? MIDDLE_BUTTON_DOWN : MIDDLE_BUTTON_UP;
					break;
				case SDL_BUTTON_X1:
					gfX1ButtonState = down;
					usEvent = down ? X1_BUTTON_DOWN : X1_BUTTON_UP;
					break;
				case SDL_BUTTON_X2:
					gfX2ButtonState = down;
					usEvent = down ? X2_BUTTON_DOWN : X2_BUTTON_UP;
					break;
				default:
					break;
			}

			if (usEvent != 0)
			{
				QueueEvent(usEvent, 0, uiParam);
				gfSGPInputReceived = TRUE;
			}
			break;
		}

		// ---- Mouse wheel -----------------------------------------------------
		case SDL_EVENT_MOUSE_WHEEL:
			if (event->wheel.y != 0)
			{
				const UINT32 uiParam = PackMouseXY(gusMouseXPos, gusMouseYPos);
				if (event->wheel.y > 0)
				{
					gsMouseWheelDeltaValue = 1;
					QueueEvent(MOUSE_WHEEL_UP, 0, uiParam);
				}
				else
				{
					gsMouseWheelDeltaValue = -1;
					QueueEvent(MOUSE_WHEEL_DOWN, 0, uiParam);
				}
				gfSGPInputReceived = TRUE;
			}
			break;

		default:
			break;
	}

	return FALSE;
}
