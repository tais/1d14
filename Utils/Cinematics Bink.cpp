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
#include "ddraw.h"
#include "Mss.h"
#include "DirectX Common.h"
#include "DirectDraw Calls.h"
#include "soundman.h"
#include "video.h"

#include "Cinematics Bink.h"

#include "vsurface_private.h"

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

//LPDIRECTDRAWSURFACE lpBinkVideoPlayback=NULL;
LPDIRECTDRAWSURFACE2 lpBinkVideoPlayback2=NULL;
HWND				hBinkDisplayWindow=0;
UINT32			guiWidth;
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
static BINKFLIC *BinkOpenFlic(const CHAR8 *cFilename);
static BINKFLIC *BinkGetFreeFlic(void);
static void		BinkSetBlitPosition(BINKFLIC *pBink, UINT32 uiLeft, UINT32 uiTop);



//*******************************************************************
//
// Functions
//
//*******************************************************************

void				BinkInitialize(HWND hWindow, UINT32 uiWidth, UINT32 uiHeight)
{
	memset(BinkList, 0, sizeof(BinkList));

	hBinkDisplayWindow = hWindow;
	guiWidth = uiWidth;
	guiHeight = uiHeight;
	guiBinkPixelFormat = BINKSURFACE565;

	// The old sound manager supplied a DirectSound object to Bink. SDL3_mixer
	// deliberately does not expose one, but Bink's own WaveOut backend remains
	// available in binkw32.dll and keeps embedded movie audio functional.
	BinkSoundUseWaveOut();
}

BINKFLIC			*BinkPlayFlic(const CHAR8 *cFilename, UINT32 uiLeft, UINT32 uiTop, UINT32 uiFlags )
{
	BINKFLIC *pBink = BinkOpenFlic(cFilename);
	if (pBink == NULL)
	{
		return(NULL);
	}

	if (uiFlags & BINK_FLIC_CENTER_VERTICAL)
	{
		uiTop = guiHeight > pBink->BinkHandle->Height
			? (guiHeight - pBink->BinkHandle->Height) / 2
			: 0;
	}

	BinkSetBlitPosition(pBink, uiLeft, uiTop);
	pBink->uiFlags |= BINK_FLIC_PLAYING;
	pBink->uiFlags |= (uiFlags & BINK_FLIC_AUTOCLOSE)
		? BINK_FLIC_AUTOCLOSE
		: BINK_FLIC_LOOP;

	return(pBink);
}


BOOLEAN			BinkPollFlics(void)
{
	BOOLEAN fFlicStatus = FALSE;

	for (UINT32 uiCount = 0; uiCount < BINK_NUM_FLICS; ++uiCount)
	{
		BINKFLIC *pBink = &BinkList[uiCount];
		if (!(pBink->uiFlags & BINK_FLIC_PLAYING))
		{
			continue;
		}

		fFlicStatus = TRUE;
		if (BinkWait(pBink->BinkHandle))
		{
			continue;
		}

		BinkDoFrame(pBink->BinkHandle);

		UINT32 uiPitch = 0;
		BYTE *pFrameBuffer = LockVideoSurface(FRAME_BUFFER, &uiPitch);
		if (pFrameBuffer != NULL)
		{
			// The SDL video backend exposes the original CPU-side RGB565 frame
			// buffer. Decode into it directly, clipping oversized or offset movies
			// to the logical framebuffer before SDL uploads the next dirty frame.
			UINT32 uiCopyWidth = pBink->BinkHandle->Width;
			UINT32 uiCopyHeight = pBink->BinkHandle->Height;
			if (pBink->uiLeft >= guiWidth || pBink->uiTop >= guiHeight)
			{
				uiCopyWidth = 0;
				uiCopyHeight = 0;
			}
			else
			{
				if (uiCopyWidth > guiWidth - pBink->uiLeft)
					uiCopyWidth = guiWidth - pBink->uiLeft;
				if (uiCopyHeight > guiHeight - pBink->uiTop)
					uiCopyHeight = guiHeight - pBink->uiTop;
			}

			if (uiCopyWidth != 0 && uiCopyHeight != 0)
			{
				BinkCopyToBufferRect(pBink->BinkHandle,
					pFrameBuffer,
					(S32)uiPitch,
					guiHeight,
					pBink->uiLeft,
					pBink->uiTop,
					0,
					0,
					uiCopyWidth,
					uiCopyHeight,
					guiBinkPixelFormat);
			}
			UnLockVideoSurface(FRAME_BUFFER);
		}

		// Bink frame numbers are one-based. Display the final frame before
		// either closing the movie or returning a loop to frame one.
		if (pBink->BinkHandle->FrameNum >= pBink->BinkHandle->Frames)
		{
			if (pBink->uiFlags & BINK_FLIC_LOOP)
			{
				BinkGoto(pBink->BinkHandle, 1, 0);
			}
			else if (pBink->uiFlags & BINK_FLIC_AUTOCLOSE)
			{
				BinkCloseFlic(pBink);
			}
		}
		else
		{
			BinkNextFrame(pBink->BinkHandle);
		}
	}

	return(fFlicStatus);
}


void BinkCloseFlic( BINKFLIC *pBink )
{
	if (pBink == NULL)
	{
		return;
	}

	if (pBink->BinkHandle != NULL)
	{
		BinkClose(pBink->BinkHandle);
	}
	if (pBink->hFileHandle != 0)
	{
		FileClose(pBink->hFileHandle);
	}

	memset(pBink, 0, sizeof(BINKFLIC));
}


void				BinkShutdownVideo(void)
{
	for (UINT32 uiCount = 0; uiCount < BINK_NUM_FLICS; ++uiCount)
	{
		if (BinkList[uiCount].uiFlags & BINK_FLIC_OPEN)
		{
			BinkCloseFlic(&BinkList[uiCount]);
		}
	}
}


static BINKFLIC *BinkOpenFlic(const CHAR8 *cFilename)
{
	BINKFLIC *pBink = BinkGetFreeFlic();
	if (pBink == NULL)
	{
		ErrorMsg("BINK ERROR: Out of flic slots, cannot open another");
		return(NULL);
	}

	// Bink opens a real filesystem path. Extract files supplied by the VFS
	// (including files inside SLF archives) into Temp, matching the Smacker path.
	vfs::Path introName(cFilename);
	vfs::Path directory;
	vfs::Path filename;
	introName.splitLast(directory, filename);
	vfs::Path tempFile = vfs::Path(L"Temp") + filename;

	if (!getVFS()->fileExists(tempFile))
	{
		try
		{
			if (!getVFS()->fileExists(introName))
			{
				return(NULL);
			}

			vfs::COpenReadFile sourceFile(introName);
			vfs::size_t size = sourceFile->getSize();
			std::vector<vfs::Byte> data(size);
			if (size != 0)
			{
				sourceFile->read(&data[0], size);
			}

			vfs::COpenWriteFile destinationFile(tempFile, true);
			if (size != 0)
			{
				destinationFile->write(&data[0], size);
			}
		}
		catch (std::exception& ex)
		{
			SGP_RETHROW(_BS(L"Intro file \"") << filename << L"\" could not be extracted" << _BS::wget, ex);
		}
	}

	vfs::Path realPath;
	try
	{
		vfs::COpenWriteFile tempFileHandle(tempFile);
		if (!tempFileHandle->_getRealPath(realPath))
		{
			return(NULL);
		}
	}
	catch (std::exception& ex)
	{
		SGP_RETHROW(L"Temporary intro file could not be read", ex);
	}

	pBink->BinkHandle = BinkOpen(realPath.to_string().c_str(), BINKNOTHREADEDIO);
	if (pBink->BinkHandle == NULL)
	{
		ErrorMsg("BINK ERROR: Bink won't open the BINK file");
		return(NULL);
	}

	pBink->cFilename = cFilename;
	pBink->hWindow = hBinkDisplayWindow;
	pBink->uiFlags |= BINK_FLIC_OPEN;
	return(pBink);
}


static BINKFLIC *BinkGetFreeFlic(void)
{
	for (UINT32 uiCount = 0; uiCount < BINK_NUM_FLICS; ++uiCount)
	{
		if (!(BinkList[uiCount].uiFlags & BINK_FLIC_OPEN))
		{
			return(&BinkList[uiCount]);
		}
	}
	return(NULL);
}


static void BinkSetBlitPosition(BINKFLIC *pBink, UINT32 uiLeft, UINT32 uiTop)
{
	pBink->uiLeft = uiLeft;
	pBink->uiTop = uiTop;
}
