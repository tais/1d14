#include "types.h"
#include <stdio.h>
#include <io.h>
#include <string.h>
#include <fcntl.h>
#include <share.h>
#include <sys/stat.h>
#include <malloc.h>
#include <stdlib.h>

#include "DEBUG.H"
#include "FileMan.h"
#include "SMACK.H"
#include "soundman.h"
#include "video.h"

#include "Cinematics Bink.h"

//#include "Intro.h"
#include <vfs/Core/vfs.h>
#include <vfs/Core/vfs_file_raii.h>




#include <crtdbg.h>



//*******************************************************************
//
// Local Defines
//
//*******************************************************************


#define BINK_NUM_FLICS							4										// Maximum number of flics open



//*******************************************************************
//
// Global Variables
//
//*******************************************************************

BINKFLIC BinkList[BINK_NUM_FLICS];
UINT32	 guiBinkPixelFormat=0;

HWND				hBinkDisplayWindow=0;
UINT32			guiHeight;



//*******************************************************************
//
// Function Prototypes
//
//*******************************************************************

void			BinkInitialize(HWND hWindow, UINT32 uiWidth, UINT32 uiHeight);
BINKFLIC		*BinkPlayFlic(const CHAR8 *cFilename, UINT32 uiLeft, UINT32 uiTop, UINT32 uiFlags );
BOOLEAN			BinkPollFlics(void);
void			BinkCloseFlic(BINKFLIC *pBink);
void			BinkShutdownVideo(void);



//*******************************************************************
//
// Functions
//
//*******************************************************************

// SDL3 port: no .bik assets ship with the game, so the Bink path is dead in practice. To avoid any
// DirectDraw-surface runtime path with untestable data, the whole Bink player is reduced to safe
// no-op stubs. binkw32.lib / bink.h stay linked/included (harmless on Windows) so real .bik playback
// can be reinstated later by mirroring the Smacker redirect (LockVideoSurface + BinkCopyToBuffer with
// BINKSURFACE565). The exported signatures (including the HWND parameter) are preserved.


void				BinkInitialize(HWND hWindow, UINT32 uiWidth, UINT32 uiHeight)
{
	// no-op
}


BINKFLIC			*BinkPlayFlic(const CHAR8 *cFilename, UINT32 uiLeft, UINT32 uiTop, UINT32 uiFlags )
{
	return( NULL );
}


BOOLEAN			BinkPollFlics(void)
{
	return( FALSE );
}


void BinkCloseFlic( BINKFLIC *pBink )
{
	// no-op
}


void				BinkShutdownVideo(void)
{
	// no-op
}
