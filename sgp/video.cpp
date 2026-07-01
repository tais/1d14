#include "types.h"
#include "video.h"
#include "vobject_blitters.h"
#include "sgp.h"
#include <stdio.h>
#include <io.h>
#include "renderworld.h"
#include "Render Dirty.h"
#include "Fade Screen.h"
#include "impTGA.h"
#include "Timer Control.h"
#include "FileMan.h"
#include "input.h"
#include "GameSettings.h"
#include "sgp_logger.h"
#include <SDL3/SDL.h>
#include <stdlib.h>

#include "resource.h"
#include <vfs/Core/vfs.h>
#include <vfs/Core/vfs_file_raii.h>

#include "local.h"
#include "Text.h"


#ifndef _MT
#define _MT
#endif

extern int iScreenMode;

///////////////////////////////////////////////////////////////////////////////////////////////////
//
// Local Defines
//
///////////////////////////////////////////////////////////////////////////////////////////////////

#define MAX_DIRTY_REGIONS	 128

#define VIDEO_OFF			 0x00
#define VIDEO_ON				0x01
#define VIDEO_SHUTTING_DOWN	0x02
#define VIDEO_SUSPENDED		0x04

#define THREAD_OFF			0x00
#define THREAD_ON			 0x01
#define THREAD_SUSPENDED		0x02

#define CURRENT_MOUSE_DATA		0
#define PREVIOUS_MOUSE_DATA		1


///////////////////////////////////////////////////////////////////////////////////////////////////
//
// Local Typedefs
//
///////////////////////////////////////////////////////////////////////////////////////////////////

// (MouseCursorBackground DirectDraw helper struct removed - the SDL port
//  composites the cursor in software directly onto the heap FRAME_BUFFER.)

///////////////////////////////////////////////////////////////////////////////////////////////////
//
// LOCAL globals
//
///////////////////////////////////////////////////////////////////////////////////////////////////

//
// Video state variables
//

static UINT16				 gusScreenWidth;
static UINT16				 gusScreenHeight;
static UINT8					gubScreenPixelDepth;

static RECT	gScrollRegion;

#define			MAX_NUM_FRAMES			25

BOOLEAN												gfVideoCapture=FALSE;
UINT32												guiFramePeriod = (1000 / 15 );
UINT32												guiLastFrame;
UINT16													*gpFrameData[ MAX_NUM_FRAMES ];
INT32													giNumFrames = 0;

//
// SDL3 presentation objects (replace the DirectDraw primary/back/frame surfaces).
// The whole scene is composited into gpHeapFrame and presented directly via gFrameTex.
//

static SDL_Window*		gWindow		= NULL;
static SDL_Renderer*	gRenderer	= NULL;
static SDL_Texture*		gFrameTex	= NULL;

//
// Heap RGB565 buffers, one per logical surface. Row pitch is always
// SCREEN_WIDTH*2 (frame/back/primary) or MAX_CURSOR_WIDTH*2 (mouse), no padding.
// gpHeapFrame is the buffer everything renders into and the one presented;
// gpHeapBack / gpHeapPrimary are vestigial (vsurface + the rain overlay still Lock them).
//

static UINT16*		gpHeapFrame		= NULL;
static UINT16*		gpHeapBack		= NULL;
static UINT16*		gpHeapPrimary	= NULL;
static UINT16*		gpHeapMouse		= NULL;

extern RECT									rcWindow;
extern POINT									ptWindowSize;

UINT32 CurrentSurface = BACKBUFFER;

//
// Globals for mouse cursor
//

static UINT16				 gusMouseCursorWidth;
static UINT16				 gusMouseCursorHeight;
static INT16					gsMouseCursorXOffset;
static INT16					gsMouseCursorYOffset;

static HVOBJECT				gpCursorStore;

BOOLEAN			gfFatalError = FALSE;
char				gFatalErrorString[ 512 ];

// 8-bit palette stuff

SGPPaletteEntry								gSgpPalette[256];

//
// Make sure we record the value of the hWindow (main window frame for the application)
//

HWND							ghWindow;

//
// Refresh thread based variables
//

UINT32						guiFrameBufferState;	// BUFFER_READY, BUFFER_DIRTY
UINT32						guiMouseBufferState;	// BUFFER_READY, BUFFER_DIRTY, BUFFER_DISABLED
UINT32									 guiVideoManagerState;	// VIDEO_ON, VIDEO_OFF, VIDEO_SUSPENDED, VIDEO_SHUTTING_DOWN
UINT32						guiRefreshThreadState;	// THREAD_ON, THREAD_OFF, THREAD_SUSPENDED

//
// Dirty rectangle management variables
//

void							(*gpFrameBufferRefreshOverride)(void);
SGPRect						gListOfDirtyRegions[MAX_DIRTY_REGIONS];
UINT32						guiDirtyRegionCount;
BOOLEAN						gfForceFullScreenRefresh;


SGPRect						gDirtyRegionsEx[MAX_DIRTY_REGIONS];
UINT32						gDirtyRegionsFlagsEx[MAX_DIRTY_REGIONS];
UINT32						guiDirtyRegionExCount;

SGPRect						gBACKUPListOfDirtyRegions[MAX_DIRTY_REGIONS];
UINT32						gBACKUPuiDirtyRegionCount;
BOOLEAN						gBACKUPfForceFullScreenRefresh;

//
// Screen output stuff
//

BOOLEAN						gfPrintFrameBuffer;
UINT32						guiPrintFrameBufferIndex;

// DX Loop Error Count Limiter
const INT32					iMaxDXLoopCount = 10; 


///////////////////////////////////////////////////////////////////////////////////////////////////
//
// External Variables
//
///////////////////////////////////////////////////////////////////////////////////////////////////

extern UINT16 gusRedMask;
extern UINT16 gusGreenMask;
extern UINT16 gusBlueMask;
extern INT16	gusRedShift;
extern INT16	gusBlueShift;
extern INT16	gusGreenShift;

///////////////////////////////////////////////////////////////////////////////////////////////////
//
// Local Function Prototypes
//
///////////////////////////////////////////////////////////////////////////////////////////////////

void AddRegionEx(INT32 iLeft, INT32 iTop, INT32 iRight, INT32 iBottom, UINT32 uiFlags );
void SnapshotSmall( void );
void VideoMovieCapture( BOOLEAN fEnable );
void RefreshMovieCache( );



///////////////////////////////////////////////////////////////////////////////////////////////////

BOOLEAN InitializeVideoManager(HINSTANCE hInstance, UINT16 usCommandShow, void *WindowProc)
{
	PTR		pTmpPointer;
	UINT32	uiPitch;

	// hInstance / usCommandShow / WindowProc are ignored - the window is now
	// created by SDL. The 3-arg signature is preserved so callers stay unchanged.
	(void)hInstance; (void)usCommandShow; (void)WindowProc;

	RegisterDebugTopic(TOPIC_VIDEO, "Video");
	DebugMsg(TOPIC_VIDEO, DBG_LEVEL_0, "Initializing the video manager");

	//
	// SDL_Init(SDL_INIT_VIDEO) is normally done by main() in sgp.cpp before we
	// get here; initialise defensively in case it was not.
	//
	if ( !SDL_WasInit( SDL_INIT_VIDEO ) )
	{
		if ( !SDL_InitSubSystem( SDL_INIT_VIDEO ) )
		{
			DebugMsg(TOPIC_VIDEO, DBG_LEVEL_0, String("SDL_InitSubSystem(VIDEO) failed: %s", SDL_GetError()));
			return FALSE;
		}
	}

	//
	// Create the window at native 640x480. No SDL_SetRenderLogicalPresentation:
	// the window stays 1:1 (the retained Win32 mouse path depends on it). The
	// title matches APPLICATION_NAME so the single-instance FindWindowEx() check
	// can still locate a running instance. iScreenMode: 0 == fullscreen
	// (borderless desktop), 1 == windowed.
	//
	{
		SDL_WindowFlags winFlags = ( iScreenMode == 0 ) ? SDL_WINDOW_FULLSCREEN : 0;
		gWindow = SDL_CreateWindow( APPLICATION_NAME, SCREEN_WIDTH, SCREEN_HEIGHT, winFlags );
	}
	if ( gWindow == NULL )
	{
		DebugMsg(TOPIC_VIDEO, DBG_LEVEL_0, String("SDL_CreateWindow failed: %s", SDL_GetError()));
		return FALSE;
	}

	gRenderer = SDL_CreateRenderer( gWindow, NULL );
	if ( gRenderer == NULL )
	{
		DebugMsg(TOPIC_VIDEO, DBG_LEVEL_0, String("SDL_CreateRenderer failed: %s", SDL_GetError()));
		return FALSE;
	}
	SDL_SetRenderVSync( gRenderer, gGameExternalOptions.gfVSync ? 1 : 0 );

	gFrameTex = SDL_CreateTexture( gRenderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, SCREEN_WIDTH, SCREEN_HEIGHT );
	if ( gFrameTex == NULL )
	{
		DebugMsg(TOPIC_VIDEO, DBG_LEVEL_0, String("SDL_CreateTexture failed: %s", SDL_GetError()));
		return FALSE;
	}
	SDL_SetTextureScaleMode( gFrameTex, SDL_SCALEMODE_NEAREST );

	//
	// Allocate the heap surfaces (16bpp, no row padding).
	//
	gpHeapFrame   = (UINT16 *)calloc( (size_t)SCREEN_WIDTH * SCREEN_HEIGHT, sizeof(UINT16) );
	gpHeapBack    = (UINT16 *)calloc( (size_t)SCREEN_WIDTH * SCREEN_HEIGHT, sizeof(UINT16) );
	gpHeapPrimary = (UINT16 *)calloc( (size_t)SCREEN_WIDTH * SCREEN_HEIGHT, sizeof(UINT16) );
	gpHeapMouse   = (UINT16 *)calloc( (size_t)MAX_CURSOR_WIDTH * MAX_CURSOR_HEIGHT, sizeof(UINT16) );
	if ( !gpHeapFrame || !gpHeapBack || !gpHeapPrimary || !gpHeapMouse )
	{
		DebugMsg(TOPIC_VIDEO, DBG_LEVEL_0, "Failed to allocate video heap buffers");
		return FALSE;
	}

	memset( gpFrameData, 0, sizeof( gpFrameData ) );

	//
	// Record the Win32 HWND of the SDL window so the retained Win32 hooks
	// (input ScreenToClient, clipboard, FatalError message box) keep working.
	//
	ghWindow = (HWND)SDL_GetPointerProperty( SDL_GetWindowProperties( gWindow ), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL );

	// JA2 draws its own cursor into MOUSE_BUFFER and we composite it; hide the OS arrow.
	SDL_HideCursor();

	gusScreenWidth = SCREEN_WIDTH;
	gusScreenHeight = SCREEN_HEIGHT;
	gubScreenPixelDepth = PIXEL_DEPTH;

	//
	// Blank out the frame buffer
	//
	pTmpPointer = LockFrameBuffer(&uiPitch);
	memset(pTmpPointer, 0, SCREEN_HEIGHT * uiPitch);
	UnlockFrameBuffer();

	//
	// Initialize state variables
	//
	guiFrameBufferState			= BUFFER_DIRTY;
	guiMouseBufferState			= BUFFER_DISABLED;
	guiVideoManagerState		 = VIDEO_ON;
	guiRefreshThreadState		= THREAD_OFF;
	guiDirtyRegionCount			= 0;
	guiDirtyRegionExCount		= 0;
	gfForceFullScreenRefresh	 = TRUE;
	gpFrameBufferRefreshOverride = NULL;
	gpCursorStore				= NULL;
	gfPrintFrameBuffer			= FALSE;
	guiPrintFrameBufferIndex	 = 0;

	//
	// This sets up the fixed RGB565 masks/shifts + translucent mask.
	//
	if (GetRGBDistribution() == FALSE)
		return FALSE;

	return TRUE;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void ShutdownVideoManager(void)
{
	DebugMsg(TOPIC_VIDEO, DBG_LEVEL_0, "Shutting down the video manager");

	if ( gFrameTex )  { SDL_DestroyTexture( gFrameTex );  gFrameTex = NULL; }
	if ( gRenderer )  { SDL_DestroyRenderer( gRenderer ); gRenderer = NULL; }
	if ( gWindow )    { SDL_DestroyWindow( gWindow );     gWindow = NULL; }

	if ( gpHeapFrame )   { free( gpHeapFrame );   gpHeapFrame = NULL; }
	if ( gpHeapBack )    { free( gpHeapBack );    gpHeapBack = NULL; }
	if ( gpHeapPrimary ) { free( gpHeapPrimary ); gpHeapPrimary = NULL; }
	if ( gpHeapMouse )   { free( gpHeapMouse );   gpHeapMouse = NULL; }

	guiVideoManagerState = VIDEO_OFF;

	if (gpCursorStore != NULL)
	{
		DeleteVideoObject(gpCursorStore);
		gpCursorStore = NULL;
	}

	// ATE: Release mouse cursor!
	FreeMouseCursor( FALSE );

	UnRegisterDebugTopic(TOPIC_VIDEO, "Video");
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void SuspendVideoManager(void)
{
	guiVideoManagerState = VIDEO_SUSPENDED;

}

void DoTester( )
{
	// (Was a DirectDraw display-mode reset helper; no-op in the SDL port.)
}

///////////////////////////////////////////////////////////////////////////////////////////////////

BOOLEAN RestoreVideoManager(void)
{
	//
	// SDL textures / renderer don't get "lost" the way DirectDraw surfaces did,
	// so restoring is simply flipping the manager back on and forcing a repaint.
	//
	if (guiVideoManagerState == VIDEO_SUSPENDED)
	{
		guiFrameBufferState = BUFFER_DIRTY;
		guiMouseBufferState = BUFFER_DIRTY;
		gfForceFullScreenRefresh = TRUE;
		guiVideoManagerState = VIDEO_ON;
		return TRUE;
	}
	else
	{
		return FALSE;
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void GetCurrentVideoSettings( UINT16 *usWidth, UINT16 *usHeight, UINT8 *ubBitDepth )
{
	*usWidth = (UINT16) gusScreenWidth;
	*usHeight = (UINT16) gusScreenHeight;
	*ubBitDepth = (UINT8) gubScreenPixelDepth;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

BOOLEAN CanBlitToFrameBuffer(void)
{
	BOOLEAN fCanBlit;

	//
	// W A R N I N G ---- W A R N I N G ---- W A R N I N G ---- W A R N I N G ---- W A R N I N G ----
	//
	// This function is intended to be called by a thread which has already locked the
	// FRAME_BUFFER_MUTEX mutual exclusion section. Anything else will cause the application to
	// yack
	//

	fCanBlit = (guiFrameBufferState == BUFFER_READY);

	return fCanBlit;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

BOOLEAN CanBlitToMouseBuffer(void)
{
	BOOLEAN fCanBlit;

	//
	// W A R N I N G ---- W A R N I N G ---- W A R N I N G ---- W A R N I N G ---- W A R N I N G ----
	//
	// This function is intended to be called by a thread which has already locked the
	// MOUSE_BUFFER_MUTEX mutual exclusion section. Anything else will cause the application to
	// yack
	//

	fCanBlit = (guiMouseBufferState == BUFFER_READY);

	return fCanBlit;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void InvalidateRegion(INT32 iLeft, INT32 iTop, INT32 iRight, INT32 iBottom)
{
	if (gfForceFullScreenRefresh == TRUE)
	{
		//
		// There's no point in going on since we are forcing a full screen refresh
		//

		return;
	}

	if (guiDirtyRegionCount < MAX_DIRTY_REGIONS)
	{
		//
		// Well we haven't broken the MAX_DIRTY_REGIONS limit yet, so we register the new region
		//

		// DO SOME PREMIMARY CHECKS FOR VALID RECTS
		if ( iLeft < 0 )
			iLeft = 0;

		if ( iTop < 0 )
			iTop = 0;

		if ( iRight > SCREEN_WIDTH )
			iRight = SCREEN_WIDTH;

		if ( iBottom > SCREEN_HEIGHT )
			iBottom = SCREEN_HEIGHT;

		if (	( iRight - iLeft ) <= 0 )
			return;

		if (	( iBottom - iTop ) <= 0 )
			return;

		gListOfDirtyRegions[guiDirtyRegionCount].iLeft	= iLeft;
		gListOfDirtyRegions[guiDirtyRegionCount].iTop	= iTop;
		gListOfDirtyRegions[guiDirtyRegionCount].iRight	= iRight;
		gListOfDirtyRegions[guiDirtyRegionCount].iBottom = iBottom;

		//		gDirtyRegionFlags[ guiDirtyRegionCount ] = TRUE;

		guiDirtyRegionCount++;

	}
	else
	{
		//
		// The MAX_DIRTY_REGIONS limit has been exceeded. Therefore we arbitrarely invalidate the entire
		// screen and force a full screen refresh
		//
		guiDirtyRegionExCount = 0;
		guiDirtyRegionCount = 0;
		gfForceFullScreenRefresh = TRUE;
	}
}


void InvalidateRegionEx(INT32 iLeft, INT32 iTop, INT32 iRight, INT32 iBottom, UINT32 uiFlags )
{
	INT32 iOldBottom;

	iOldBottom = iBottom;

	// Check if we are spanning the rectangle - if so slit it up!
	if ( iTop <= gsVIEWPORT_WINDOW_END_Y && iBottom > gsVIEWPORT_WINDOW_END_Y )
	{
		// Add new top region
		iBottom				= gsVIEWPORT_WINDOW_END_Y;
		AddRegionEx( iLeft, iTop, iRight, iBottom, uiFlags );

		// Add new bottom region
		iTop	= gsVIEWPORT_WINDOW_END_Y;
		iBottom	= iOldBottom;
		AddRegionEx( iLeft, iTop, iRight, iBottom, uiFlags );

	}
	else
	{
		AddRegionEx( iLeft, iTop, iRight, iBottom, uiFlags );
	}
}


void AddRegionEx(INT32 iLeft, INT32 iTop, INT32 iRight, INT32 iBottom, UINT32 uiFlags )
{

	if (guiDirtyRegionExCount < MAX_DIRTY_REGIONS)
	{

		// DO SOME PREMIMARY CHECKS FOR VALID RECTS
		if ( iLeft < 0 )
			iLeft = 0;

		if ( iTop < 0 )
			iTop = 0;

		if ( iRight > SCREEN_WIDTH )
			iRight = SCREEN_WIDTH;

		if ( iBottom > SCREEN_HEIGHT )
			iBottom = SCREEN_HEIGHT;

		if (	( iRight - iLeft ) <= 0 )
			return;

		if (	( iBottom - iTop ) <= 0 )
			return;



		gDirtyRegionsEx[ guiDirtyRegionExCount ].iLeft	= iLeft;
		gDirtyRegionsEx[ guiDirtyRegionExCount ].iTop	= iTop;
		gDirtyRegionsEx[ guiDirtyRegionExCount ].iRight	= iRight;
		gDirtyRegionsEx[ guiDirtyRegionExCount ].iBottom = iBottom;

		gDirtyRegionsFlagsEx[ guiDirtyRegionExCount ] = uiFlags;

		guiDirtyRegionExCount++;

	}
	else
	{
		guiDirtyRegionExCount = 0;
		guiDirtyRegionCount = 0;
		gfForceFullScreenRefresh = TRUE;

	}
}


///////////////////////////////////////////////////////////////////////////////////////////////////

void InvalidateRegions(SGPRect *pArrayOfRegions, UINT32 uiRegionCount)
{
	if (gfForceFullScreenRefresh == TRUE)
	{
		//
		// There's no point in going on since we are forcing a full screen refresh
		//

		return;
	}

	if ((guiDirtyRegionCount + uiRegionCount) < MAX_DIRTY_REGIONS)
	{
		UINT32 uiIndex;

		for (uiIndex = 0; uiIndex < uiRegionCount; uiIndex++)
		{
			//
			// Well we haven't broken the MAX_DIRTY_REGIONS limit yet, so we register the new region
			//

			gListOfDirtyRegions[guiDirtyRegionCount].iLeft	= pArrayOfRegions[uiIndex].iLeft;
			gListOfDirtyRegions[guiDirtyRegionCount].iTop	= pArrayOfRegions[uiIndex].iTop;
			gListOfDirtyRegions[guiDirtyRegionCount].iRight	= pArrayOfRegions[uiIndex].iRight;
			gListOfDirtyRegions[guiDirtyRegionCount].iBottom = pArrayOfRegions[uiIndex].iBottom;

			guiDirtyRegionCount++;
		}
	}
	else
	{
		guiDirtyRegionCount = 0;
		gfForceFullScreenRefresh = TRUE;
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void InvalidateScreen(void)
{
	//
	// W A R N I N G ---- W A R N I N G ---- W A R N I N G ---- W A R N I N G ---- W A R N I N G ----
	//
	// This function is intended to be called by a thread which has already locked the
	// FRAME_BUFFER_MUTEX mutual exclusion section. Anything else will cause the application to
	// yack
	//

	guiDirtyRegionCount = 0;
	guiDirtyRegionExCount = 0;
	gfForceFullScreenRefresh = TRUE;
	guiFrameBufferState = BUFFER_DIRTY;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void InvalidateFrameBuffer(void)
{
	//
	// W A R N I N G ---- W A R N I N G ---- W A R N I N G ---- W A R N I N G ---- W A R N I N G ----
	//
	// This function is intended to be called by a thread which has already locked the
	// FRAME_BUFFER_MUTEX mutual exclusion section. Anything else will cause the application to
	// yack
	//

	guiFrameBufferState = BUFFER_DIRTY;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void SetFrameBufferRefreshOverride(PTR pFrameBufferRefreshOverride)
{

	gpFrameBufferRefreshOverride = (void (__cdecl *)(void))pFrameBufferRefreshOverride;
}

//#define SCROLL_TEST

///////////////////////////////////////////////////////////////////////////////////////////////////
void ScrollJA2Background(UINT32 uiDirection, INT16 sScrollXIncrement, INT16 sScrollYIncrement, PTR pSource, PTR pDest, BOOLEAN fRenderStrip, UINT32 uiCurrentMouseBackbuffer )
{
	//
	// SDL port - ScrollJA2Background option (B): the DirectDraw incremental
	// viewport-scroll blit has been removed. The whole scene is composited into
	// (and presented directly from) the heap FRAME_BUFFER, so rather than shifting
	// a separate back buffer we simply force a full re-upload of the frame this
	// present. Signature preserved; the surface args are ignored.
	//
	(void)uiDirection; (void)sScrollXIncrement; (void)sScrollYIncrement;
	(void)pSource; (void)pDest; (void)fRenderStrip; (void)uiCurrentMouseBackbuffer;

	gfForceFullScreenRefresh = TRUE;
}


//rain
//extern BOOLEAN gfVSync;

BOOLEAN IsItAllowedToRenderRain();
extern UINT32 guiRainRenderSurface;

BOOLEAN gfNextRefreshFullScreen = FALSE;
//end rain

static void UnionDirty( BOOLEAN *pfHave, INT32 *pL, INT32 *pT, INT32 *pR, INT32 *pB, INT32 l, INT32 t, INT32 r, INT32 b )
{
	if ( l < 0 ) l = 0;
	if ( t < 0 ) t = 0;
	if ( r > (INT32)SCREEN_WIDTH )  r = SCREEN_WIDTH;
	if ( b > (INT32)SCREEN_HEIGHT ) b = SCREEN_HEIGHT;
	if ( l >= r || t >= b ) return;

	if ( !*pfHave )
	{
		*pL = l; *pT = t; *pR = r; *pB = b;
		*pfHave = TRUE;
	}
	else
	{
		if ( l < *pL ) *pL = l;
		if ( t < *pT ) *pT = t;
		if ( r > *pR ) *pR = r;
		if ( b > *pB ) *pB = b;
	}
}

void RefreshScreen(void *DummyVariable)
{
	UINT16	usScreenWidth, usScreenHeight;
	UINT32	uiIndex;
	UINT32	uiTime;
	BOOLEAN	fFullUpload;

	// Software mouse-cursor composite bookkeeping.
	static UINT16	sCursorSave[ MAX_CURSOR_WIDTH * MAX_CURSOR_HEIGHT ];
	static INT32	sCursorSaveL = 0, sCursorSaveT = 0, sCursorSaveR = 0, sCursorSaveB = 0;
	static INT32	sPrevCursorL = 0, sPrevCursorT = 0, sPrevCursorR = 0, sPrevCursorB = 0;
	INT32			curCursorL = 0, curCursorT = 0, curCursorR = 0, curCursorB = 0;
	BOOLEAN			fCursorStamped = FALSE;

	INT32			upL = 0, upT = 0, upR = 0, upB = 0;
	BOOLEAN			fHaveRect = FALSE;

	usScreenWidth = usScreenHeight = 0;

	if( gfNextRefreshFullScreen )
	{
		if( guiCurrentScreen == GAME_SCREEN )
		{
			InvalidateScreen();
			gfRenderScroll = FALSE;
		}
		gfNextRefreshFullScreen = FALSE;
	}

	switch (guiVideoManagerState)
	{
		case VIDEO_ON:
			guiRefreshThreadState = THREAD_ON;
			usScreenWidth = gusScreenWidth;
			usScreenHeight = gusScreenHeight;
			break;
		case VIDEO_OFF:
			guiRefreshThreadState = THREAD_OFF;
			return;
		case VIDEO_SUSPENDED:
			guiRefreshThreadState = THREAD_SUSPENDED;
			return;
		case VIDEO_SHUTTING_DOWN:
			guiRefreshThreadState = THREAD_OFF;
			return;
	}

	if ( gRenderer == NULL || gFrameTex == NULL || gpHeapFrame == NULL )
		return;

	fFullUpload = gfForceFullScreenRefresh;

	//
	// Update the frame buffer (override / software fade / scroll) if dirty.
	//
	if (guiFrameBufferState == BUFFER_DIRTY)
	{
		if (gpFrameBufferRefreshOverride != NULL)
		{
			(*gpFrameBufferRefreshOverride)();
			gpFrameBufferRefreshOverride = NULL;
		}

		if ( gfFadeInitialized && gfFadeInVideo )
		{
			// The original 16bpp software fade writes straight into FRAME_BUFFER.
			gFadeFunction( );
			fFullUpload = TRUE;
		}

		if ( gfRenderScroll )
		{
			ScrollJA2Background( guiScrollDirection, gsScrollXIncrement, gsScrollYIncrement, NULL, NULL, TRUE, PREVIOUS_MOUSE_DATA );
			fFullUpload = TRUE;
		}

		gfIgnoreScrollDueToCenterAdjust = FALSE;

		guiFrameBufferState = BUFFER_READY;
	}

	//
	// Movie capture (JA2TESTVERSION)
	//
	if( gfVideoCapture )
	{
		uiTime = GetTickCount();
		if((uiTime < guiLastFrame) || (uiTime > (guiLastFrame+guiFramePeriod)))
		{
			SnapshotSmall( );
			guiLastFrame = uiTime;
		}
	}

	//
	// PrintScreen: dump FRAME_BUFFER to a 16bpp TGA (565 -> 555). (Beta hotkey.)
	//
	if (gfPrintFrameBuffer == TRUE)
	{
		CHAR8	FileName[64];
		INT32	iIndex;
		UINT16	*p16BPPData = NULL;

		do
		{
			sprintf( FileName, "SCREEN%03d.TGA", guiPrintFrameBufferIndex++);
		}
		while(FileExists(FileName));

		try
		{
			vfs::COpenWriteFile wfile(FileName,true,true);
			char head[] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, LOBYTE(SCREEN_WIDTH), HIBYTE(SCREEN_WIDTH), LOBYTE(SCREEN_HEIGHT), HIBYTE(SCREEN_HEIGHT), 0x10, 0};
			SGP_TRYCATCH_RETHROW(wfile->write(head,18), L"");

			if (gusRedMask == 0xF800 && gusGreenMask == 0x07E0 && gusBlueMask == 0x001F)
				p16BPPData = (UINT16 *)MemAlloc( SCREEN_WIDTH * 2 );

			for (iIndex = SCREEN_HEIGHT - 1; iIndex >= 0; iIndex--)
			{
				if (p16BPPData)
				{
					memcpy( p16BPPData, gpHeapFrame + ( iIndex * SCREEN_WIDTH ), SCREEN_WIDTH * 2 );
					ConvertRGBDistribution565To555( p16BPPData, SCREEN_WIDTH );
					SGP_TRYCATCH_RETHROW(wfile->write((vfs::Byte*)p16BPPData, SCREEN_WIDTH * 2), L"");
				}
				else
				{
					SGP_TRYCATCH_RETHROW(wfile->write((vfs::Byte*)( gpHeapFrame + ( iIndex * SCREEN_WIDTH ) ), SCREEN_WIDTH * 2), L"");
				}
			}

			if (p16BPPData)
				MemFree( p16BPPData );
		}
		catch(std::exception& ex)
		{
			SGP_RETHROW(L"", ex);
		}

		gfPrintFrameBuffer = FALSE;
	}

	//
	// Mouse buffer: the cursor art already lives in MOUSE_BUFFER (gpHeapMouse);
	// under DirectDraw this uploaded gpMouseCursorOriginal -> gpMouseCursor.
	//
	if (guiMouseBufferState == BUFFER_DIRTY)
	{
		guiMouseBufferState = BUFFER_READY;
	}

	//
	// Rain overlay: draw straight into the presented FRAME_BUFFER (was BACKBUFFER
	// under DirectDraw; BACKBUFFER is no longer presented in the SDL port).
	//
	if( IsItAllowedToRenderRain() && gfProgramIsRunning )
	{
		BltVideoSurface( FRAME_BUFFER, guiRainRenderSurface, 0, 0, 0, VS_BLT_FAST | VS_BLT_USECOLORKEY, NULL );
		gfNextRefreshFullScreen = TRUE;
		fFullUpload = TRUE;
	}

	//
	// Software-composite the mouse cursor onto FRAME_BUFFER (colour key 0),
	// saving the covered pixels so FRAME_BUFFER stays clean once presented.
	// Positions come from the input seam (gusMouseXPos / gusMouseYPos).
	//
	if ( guiMouseBufferState == BUFFER_READY )
	{
		INT32 dstX = (INT32)gusMouseXPos - (INT32)gsMouseCursorXOffset;
		INT32 dstY = (INT32)gusMouseYPos - (INT32)gsMouseCursorYOffset;
		INT32 srcX0 = ( dstX < 0 ) ? -dstX : 0;
		INT32 srcY0 = ( dstY < 0 ) ? -dstY : 0;
		INT32 dX = dstX + srcX0;
		INT32 dY = dstY + srcY0;
		INT32 w  = (INT32)gusMouseCursorWidth  - srcX0;
		INT32 h  = (INT32)gusMouseCursorHeight - srcY0;

		if ( dX + w > (INT32)usScreenWidth )  w = (INT32)usScreenWidth  - dX;
		if ( dY + h > (INT32)usScreenHeight ) h = (INT32)usScreenHeight - dY;

		if ( w > 0 && h > 0 )
		{
			INT32 sy;
			for ( sy = 0; sy < h; sy++ )
			{
				memcpy( sCursorSave + sy * MAX_CURSOR_WIDTH,
						gpHeapFrame + (size_t)( dY + sy ) * SCREEN_WIDTH + dX,
						w * sizeof(UINT16) );
			}
			sCursorSaveL = dX; sCursorSaveT = dY; sCursorSaveR = dX + w; sCursorSaveB = dY + h;

			Blt16BPPTo16BPPTrans( gpHeapFrame, SCREEN_WIDTH * 2, gpHeapMouse, MAX_CURSOR_WIDTH * 2,
								  dX, dY, srcX0, srcY0, (UINT32)w, (UINT32)h, 0 );

			curCursorL = dX; curCursorT = dY; curCursorR = dX + w; curCursorB = dY + h;
			fCursorStamped = TRUE;
		}
	}

	//
	// Build the sub-rectangle to upload: union of the engine's dirty regions plus
	// this frame's and last frame's cursor boxes. Full upload when forced.
	//
	if ( !fFullUpload )
	{
		for (uiIndex = 0; uiIndex < guiDirtyRegionCount; uiIndex++)
			UnionDirty( &fHaveRect, &upL, &upT, &upR, &upB,
						gListOfDirtyRegions[uiIndex].iLeft, gListOfDirtyRegions[uiIndex].iTop,
						gListOfDirtyRegions[uiIndex].iRight, gListOfDirtyRegions[uiIndex].iBottom );

		for (uiIndex = 0; uiIndex < guiDirtyRegionExCount; uiIndex++)
			UnionDirty( &fHaveRect, &upL, &upT, &upR, &upB,
						gDirtyRegionsEx[uiIndex].iLeft, gDirtyRegionsEx[uiIndex].iTop,
						gDirtyRegionsEx[uiIndex].iRight, gDirtyRegionsEx[uiIndex].iBottom );

		if ( fCursorStamped )
			UnionDirty( &fHaveRect, &upL, &upT, &upR, &upB, curCursorL, curCursorT, curCursorR, curCursorB );

		if ( sPrevCursorR > sPrevCursorL && sPrevCursorB > sPrevCursorT )
			UnionDirty( &fHaveRect, &upL, &upT, &upR, &upB, sPrevCursorL, sPrevCursorT, sPrevCursorR, sPrevCursorB );
	}

	//
	// Upload the changed pixels to the streaming texture.
	//
	if ( fFullUpload )
	{
		SDL_UpdateTexture( gFrameTex, NULL, gpHeapFrame, SCREEN_WIDTH * 2 );
	}
	else if ( fHaveRect )
	{
		if ( upL <= 0 && upT <= 0 && upR >= (INT32)SCREEN_WIDTH && upB >= (INT32)SCREEN_HEIGHT )
		{
			SDL_UpdateTexture( gFrameTex, NULL, gpHeapFrame, SCREEN_WIDTH * 2 );
		}
		else
		{
			SDL_Rect r;
			r.x = upL; r.y = upT; r.w = upR - upL; r.h = upB - upT;
			SDL_UpdateTexture( gFrameTex, &r, gpHeapFrame + (size_t)upT * SCREEN_WIDTH + upL, SCREEN_WIDTH * 2 );
		}
	}
	// else: nothing changed this frame; the texture already holds the right pixels.

	//
	// Restore the pixels the cursor overwrote so FRAME_BUFFER is clean next frame.
	//
	if ( fCursorStamped )
	{
		INT32 sy;
		for ( sy = 0; sy < (sCursorSaveB - sCursorSaveT); sy++ )
		{
			memcpy( gpHeapFrame + (size_t)( sCursorSaveT + sy ) * SCREEN_WIDTH + sCursorSaveL,
					sCursorSave + sy * MAX_CURSOR_WIDTH,
					(sCursorSaveR - sCursorSaveL) * sizeof(UINT16) );
		}
	}

	// Remember this frame's cursor box for next frame's partial upload.
	sPrevCursorL = curCursorL; sPrevCursorT = curCursorT;
	sPrevCursorR = curCursorR; sPrevCursorB = curCursorB;

	//
	// Present.
	//
	SDL_SetRenderDrawColor( gRenderer, 0, 0, 0, 255 );
	SDL_RenderClear( gRenderer );
	SDL_RenderTexture( gRenderer, gFrameTex, NULL, NULL );
	SDL_RenderPresent( gRenderer );

	//
	// Clear the per-frame dirty state.
	//
	guiDirtyRegionCount = 0;
	guiDirtyRegionExCount = 0;
	gfForceFullScreenRefresh = FALSE;
	gfRenderScroll = FALSE;
	gfScrollStart = FALSE;

	(void)DummyVariable;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

// Exposed so sgp.cpp's main loop can reach the SDL renderer.
SDL_Renderer* SGP_GetSDLRenderer(void)
{
	return gRenderer;
}


///////////////////////////////////////////////////////////////////////////////////////////////////
//
// Buffer access functions
//
///////////////////////////////////////////////////////////////////////////////////////////////////

PTR LockPrimarySurface(UINT32 *uiPitch)
{
	*uiPitch = SCREEN_WIDTH * 2;
	return gpHeapPrimary;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void UnlockPrimarySurface(void)
{
}

///////////////////////////////////////////////////////////////////////////////////////////////////

PTR LockBackBuffer(UINT32 *uiPitch)
{
	*uiPitch = SCREEN_WIDTH * 2;
	return gpHeapBack;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void UnlockBackBuffer(void)
{
}

///////////////////////////////////////////////////////////////////////////////////////////////////

PTR LockFrameBuffer(UINT32 *uiPitch)
{
	*uiPitch = SCREEN_WIDTH * 2;
	return gpHeapFrame;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void UnlockFrameBuffer(void)
{
}

///////////////////////////////////////////////////////////////////////////////////////////////////

PTR LockMouseBuffer(UINT32 *uiPitch)
{
	// Fixed stride - ETRLE cursor decode relies on a MAX_CURSOR_WIDTH row pitch.
	*uiPitch = MAX_CURSOR_WIDTH * 2;
	return gpHeapMouse;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void UnlockMouseBuffer(void)
{
}

///////////////////////////////////////////////////////////////////////////////////////////////////
//
// RGB color management functions
//
///////////////////////////////////////////////////////////////////////////////////////////////////

BOOLEAN GetRGBDistribution(void)
{
	//
	// Fixed RGB565 distribution - the SDL frame texture is SDL_PIXELFORMAT_RGB565.
	// himage.cpp's Get16BPPColor reads these globals to pack 8-bit RGB into 565.
	//
	gusRedMask		= 0xF800;
	gusGreenMask	= 0x07E0;
	gusBlueMask		= 0x001F;
	gusRedShift		= 8;
	gusGreenShift	= 3;
	gusBlueShift	= -3;
	guiTranslucentMask = 0x7bef;

	return TRUE;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

BOOLEAN GetPrimaryRGBDistributionMasks(UINT32 *RedBitMask, UINT32 *GreenBitMask, UINT32 *BlueBitMask)
{
	*RedBitMask	= gusRedMask;
	*GreenBitMask = gusGreenMask;
	*BlueBitMask	= gusBlueMask;

	return TRUE;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

BOOLEAN SetMouseCursorFromObject(UINT32 uiVideoObjectHandle, UINT16 usVideoObjectSubIndex, UINT16 usOffsetX, UINT16 usOffsetY )
{
	BOOLEAN		ReturnValue;
	PTR			pTmpPointer;
	UINT32		uiPitch;
	ETRLEObject	pETRLEPointer;

	//
	// Erase cursor background
	//

	pTmpPointer = LockMouseBuffer(&uiPitch);
	memset(pTmpPointer, 0, MAX_CURSOR_HEIGHT * uiPitch);
	UnlockMouseBuffer();

	//
	// Get new cursor data
	//

	ReturnValue = BltVideoObjectFromIndex(MOUSE_BUFFER, uiVideoObjectHandle, usVideoObjectSubIndex, 0, 0, VO_BLT_SRCTRANSPARENCY, NULL);
	guiMouseBufferState = BUFFER_DIRTY;

	if (GetVideoObjectETRLEPropertiesFromIndex(uiVideoObjectHandle, &pETRLEPointer, usVideoObjectSubIndex))
	{
		gsMouseCursorXOffset = usOffsetX;
		gsMouseCursorYOffset = usOffsetY;
		gusMouseCursorWidth = pETRLEPointer.usWidth + pETRLEPointer.sOffsetX;
		gusMouseCursorHeight = pETRLEPointer.usHeight + pETRLEPointer.sOffsetY;

		DebugMsg(TOPIC_VIDEO, DBG_LEVEL_0, "=================================================");
		DebugMsg(TOPIC_VIDEO, DBG_LEVEL_0, String("Mouse Create with [ %d. %d ] [ %d, %d]", pETRLEPointer.sOffsetX, pETRLEPointer.sOffsetY, pETRLEPointer.usWidth, pETRLEPointer.usHeight));
		DebugMsg(TOPIC_VIDEO, DBG_LEVEL_0, "=================================================");

	}
	else
	{
		DebugMsg(TOPIC_VIDEO, DBG_LEVEL_0, "Failed to get mouse info");
	}

	return ReturnValue;
}

BOOLEAN EraseMouseCursor( )
{
	PTR			pTmpPointer;
	UINT32		uiPitch;

	//
	// Erase cursor background
	//

	pTmpPointer = LockMouseBuffer(&uiPitch);
	// when there is no mouse buffer the game can run into an infinite loop (some DirectX stuff)
	if(pTmpPointer)
	{
		memset(pTmpPointer, 0, MAX_CURSOR_HEIGHT * uiPitch);
	}
	UnlockMouseBuffer();

	// Don't set dirty
	return( TRUE );
}

BOOLEAN SetMouseCursorProperties( INT16 sOffsetX, INT16 sOffsetY, UINT16 usCursorHeight, UINT16 usCursorWidth )
{
	gsMouseCursorXOffset = sOffsetX;
	gsMouseCursorYOffset = sOffsetY;
	gusMouseCursorWidth	= usCursorWidth;
	gusMouseCursorHeight = usCursorHeight;
	return( TRUE );
}

BOOLEAN BltToMouseCursor(UINT32 uiVideoObjectHandle, UINT16 usVideoObjectSubIndex, UINT16 usXPos, UINT16 usYPos )
{
	BOOLEAN		ReturnValue;

	ReturnValue = BltVideoObjectFromIndex(MOUSE_BUFFER, uiVideoObjectHandle, usVideoObjectSubIndex, usXPos, usYPos, VO_BLT_SRCTRANSPARENCY, NULL);

	return ReturnValue;
}

void DirtyCursor( )
{
	guiMouseBufferState = BUFFER_DIRTY;
}

void EnableCursor( BOOLEAN fEnable )
{
	if ( fEnable )
	{
		guiMouseBufferState = BUFFER_DISABLED;
	}
	else
	{
		guiMouseBufferState = BUFFER_READY;
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////

BOOLEAN HideMouseCursor(void)
{
	guiMouseBufferState = BUFFER_DISABLED;

	return TRUE;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

BOOLEAN LoadCursorFile(STR8 pFilename)
{
	VOBJECT_DESC VideoObjectDescription;

	//
	// Make sure the old cursor store is destroyed
	//

	if (gpCursorStore != NULL)
	{
		DeleteVideoObject(gpCursorStore);
		gpCursorStore = NULL;
	}

	//
	// Get the source file with all the cursors inside
	//

	VideoObjectDescription.fCreateFlags = VOBJECT_CREATE_FROMFILE;
	strcpy(VideoObjectDescription.ImageFile, pFilename);
	gpCursorStore = CreateVideoObject(&VideoObjectDescription);

	//
	// Were we successful in creating the cursor store ?
	//

	if (gpCursorStore == NULL)
	{
		return FALSE;
	}

	return TRUE;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

BOOLEAN SetCurrentCursor(UINT16 usVideoObjectSubIndex,	UINT16 usOffsetX, UINT16 usOffsetY )
{
	BOOLEAN		ReturnValue;
	PTR			pTmpPointer;
	UINT32		uiPitch;
	ETRLEObject	pETRLEPointer;

	//
	// Make sure we have a cursor store
	//

	if (gpCursorStore == NULL)
	{
		DebugMsg(TOPIC_VIDEO, DBG_LEVEL_0, "ERROR : Cursor store is not loaded");
		return FALSE;
	}

	//
	// Ok, then blit the mouse cursor to the MOUSE_BUFFER (which is really gpMouseBufferOriginal)
	//
	//
	// Erase cursor background
	//

	pTmpPointer = LockMouseBuffer(&uiPitch);
	memset(pTmpPointer, 0, MAX_CURSOR_HEIGHT * uiPitch);
	UnlockMouseBuffer();

	//
	// Get new cursor data
	//

	ReturnValue = BltVideoObject(MOUSE_BUFFER, gpCursorStore, usVideoObjectSubIndex, 0, 0, VO_BLT_SRCTRANSPARENCY, NULL);
	guiMouseBufferState = BUFFER_DIRTY;

	if (GetVideoObjectETRLEProperties(gpCursorStore, &pETRLEPointer, usVideoObjectSubIndex))
	{
		gsMouseCursorXOffset = usOffsetX;
		gsMouseCursorYOffset = usOffsetY;
		gusMouseCursorWidth = pETRLEPointer.usWidth + pETRLEPointer.sOffsetX;
		gusMouseCursorHeight = pETRLEPointer.usHeight + pETRLEPointer.sOffsetY;

		DebugMsg(TOPIC_VIDEO, DBG_LEVEL_0, "=================================================");
		DebugMsg(TOPIC_VIDEO, DBG_LEVEL_0, String("Mouse Create with [ %d. %d ] [ %d, %d]", pETRLEPointer.sOffsetX, pETRLEPointer.sOffsetY, pETRLEPointer.usWidth, pETRLEPointer.usHeight));
		DebugMsg(TOPIC_VIDEO, DBG_LEVEL_0, "=================================================");
	}
	else
	{
		DebugMsg(TOPIC_VIDEO, DBG_LEVEL_0, "Failed to get mouse info");
	}

	return ReturnValue;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void StartFrameBufferRender(void)
{
	return;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void EndFrameBufferRender(void)
{

	guiFrameBufferState = BUFFER_DIRTY;

	return;

}

///////////////////////////////////////////////////////////////////////////////////////////////////

void PrintScreen(void)
{
	gfPrintFrameBuffer = TRUE;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
BOOLEAN Set8BPPPalette(SGPPaletteEntry *pPalette)
{
	// 16bpp renderer - just keep the palette copy; no DirectDraw palette to build.
	memcpy(gSgpPalette, pPalette, sizeof(SGPPaletteEntry)*256);
	return(TRUE);
}


void FatalError( const STR8 pError, ...)
{
	va_list argptr;

	va_start(argptr, pError);			// Set up variable argument pointer
	vsprintf(gFatalErrorString, pError, argptr);
	va_end(argptr);

	gfFatalError = TRUE;

	if ( ghWindow )
		ShowWindow( ghWindow, SW_HIDE );

	gfProgramIsRunning = FALSE;

	MessageBoxW( ghWindow, vfs::String::as_utf16(gFatalErrorString).c_str(), L"JA2 Fatal Error", MB_OK | MB_TASKMODAL );
}


/*********************************************************************************
* SnapshotSmall
*
*		Grabs a screen from the [rimary surface, and stuffs it into a 16-bit (RGB 5,5,5),
* uncompressed Targa file. Each time the routine is called, it increments the
* file number by one. The files are create in the current directory, usually the
* EXE directory. This routine produces 1/4 sized images.
*
*********************************************************************************/

#pragma pack (push, 1)

typedef struct {

	UINT8		ubIDLength;
	UINT8		ubColorMapType;
	UINT8		ubTargaType;
	UINT16	usColorMapOrigin;
	UINT16	usColorMapLength;
	UINT8		ubColorMapEntrySize;
	UINT16	usOriginX;
	UINT16	usOriginY;
	UINT16	usImageWidth;
	UINT16	usImageHeight;
	UINT8		ubBitsPerPixel;
	UINT8		ubImageDescriptor;

} TARGA_HEADER;

#pragma pack (pop)


void SnapshotSmall(void)
{
	INT32 iCountX, iCountY;
	UINT16 *pVideo, *pDest;

	// Snapshot from the heap FRAME_BUFFER instead of locking the DDraw primary.
	pVideo = gpHeapFrame;
	if ( pVideo == NULL )
		return;

	pDest = gpFrameData[ giNumFrames ];

	for(iCountY=SCREEN_HEIGHT-1; iCountY >=0 ; iCountY-=1)
	{
		for(iCountX=0; iCountX < SCREEN_WIDTH; iCountX+= 1)
		{
			*( pDest + ( iCountY * SCREEN_WIDTH ) + ( iCountX ) ) = *( pVideo + ( iCountY * SCREEN_WIDTH ) + ( iCountX ) );
		}
	}

	giNumFrames++;

	if ( giNumFrames == MAX_NUM_FRAMES )
	{
		RefreshMovieCache( );
	}
}


void VideoCaptureToggle(void)
{
#ifdef JA2TESTVERSION
	VideoMovieCapture( (BOOLEAN)!gfVideoCapture);
#endif
}

void VideoMovieCapture( BOOLEAN fEnable )
{
	INT32 cnt;

	gfVideoCapture=fEnable;
	if(fEnable)
	{
		for ( cnt = 0; cnt < MAX_NUM_FRAMES; cnt++ )
		{
			gpFrameData[ cnt ] = (UINT16 *)MemAlloc( SCREEN_WIDTH * SCREEN_HEIGHT * 2 );
		}

		giNumFrames = 0;

		guiLastFrame=GetTickCount();
	}
	else
	{
		RefreshMovieCache( );

		for ( cnt = 0; cnt < MAX_NUM_FRAMES; cnt++ )
		{
			if ( gpFrameData[ cnt ] != NULL )
			{
				MemFree( gpFrameData[ cnt ] );
			}
		}
		giNumFrames = 0;
	}
}

void RefreshMovieCache( )
{
	TARGA_HEADER Header;
	INT32 iCountX, iCountY;
	CHAR8 cFilename[_MAX_PATH];
	static UINT32 uiPicNum=0;
	UINT16 *pDest;
	INT32	cnt;
	PauseTime( TRUE );
	try
	{
	for ( cnt = 0; cnt < giNumFrames; cnt++ )
	{
		sprintf( cFilename, "JA%5.5d.TGA", uiPicNum++ );
		vfs::COpenWriteFile wfile(cFilename, true, true);
		memset(&Header, 0, sizeof(TARGA_HEADER));

		Header.ubTargaType=2;			// Uncompressed 16/24/32 bit
		Header.usImageWidth=SCREEN_WIDTH;
		Header.usImageHeight=SCREEN_HEIGHT;
		Header.ubBitsPerPixel=16;
		SGP_TRYCATCH_RETHROW(wfile->write((vfs::Byte*)&Header, sizeof(TARGA_HEADER)), L"");
		pDest = gpFrameData[ cnt ];

		for(iCountY=SCREEN_HEIGHT-1; iCountY >=0 ; iCountY-=1)
		{
			for(iCountX=0; iCountX < SCREEN_WIDTH; iCountX ++ )
			{
				SGP_TRYCATCH_RETHROW(wfile->write( (vfs::Byte*)( pDest + ( iCountY * SCREEN_WIDTH ) + iCountX ), sizeof(UINT16)), L"");
			}

		}
	}

	PauseTime( FALSE );

	giNumFrames = 0;
	}
	catch(std::exception& ex)
	{
		SGP_ERROR(ex.what());
	}
}
