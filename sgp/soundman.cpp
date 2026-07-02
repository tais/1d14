/*********************************************************************************
* SGP Digital Sound Module -- SDL3_mixer backend
*
*		This module handles the playing of digital samples, preloaded or streamed.
*
*		Originally an FMOD 3.75 (FSOUND_*) / DirectSound backend by Derek Beland
*	(May 28, 1997). The FMOD backend has been replaced by SDL3_mixer (the MIX_*
*	API) for the Windows-only MSVC x86 SDL3 port. The public soundman.h surface
*	(SoundPlay/SoundPlayStreamedFile/SoundPlayRandom/SoundServiceStreams and the
*	SOUNDPARMS / RANDOMPARMS structs) is preserved byte-identically so the rest of
*	the game (Sound Control.cpp / Music Control.cpp / GAP.cpp / Ambient Control.cpp
*	...) is unchanged.
*
*	Backend model:
*	 - one MIX_Mixer* (MIX_CreateMixerDevice) created at startup.
*	 - a fixed cache of SOUND_MAX_CACHED predecoded MIX_Audio* keyed by filename.
*	 - a fixed array of SOUND_MAX_CHANNELS pre-created MIX_Track*, one per channel.
*	 - files are read whole through FileMan (so SLF archives work), the RIFF/WAVE
*	   header size is sanitized (many JA2 SPEECH\*.wav lie about it and SDL_mixer's
*	   stricter WAV decoder would reject them), then decoded via MIX_LoadAudio_IO.
*	 - finished tracks are reaped LAZILY on the main thread (LazyReapChannel) --
*	   we deliberately do NOT register the audio-thread stopped callback, because
*	   mutating channel/sample state from the audio thread races the main thread.
*	   EOSCallback (music-advance, streamed-SFX end) fires from that main-thread
*	   reap, driven from SoundServiceStreams()/GetFreeChannel()/FindChannelByID().
*********************************************************************************/
	#include "builddefines.h"
	#include <stdio.h>
	#include <string.h>
	#include <stdlib.h>
	#include "soundman.h"
	#include "types.h"
	#include "FileMan.h"
	#include "DEBUG.H"
	#include "random.h"
	#include "sgp_logger.h"
	// sevenfm
	#include "message.h"
	#include "Sound Control.h"
	#include <map>
	#include <string>
	#include <cstdint>
	#include <chrono>

	#include <SDL3/SDL.h>
	#include <SDL3_mixer/SDL_mixer.h>

// Uncomment this to disable the startup of sound hardware
//#define SOUND_DISABLE

// global settings
#define		SOUND_MAX_CACHED		128						// number of cache slots

// sevenfm: increased number of channels
#define		SOUND_MAX_CHANNELS		128

// playing/random value to indicate default
#define		SOUND_PARMS_DEFAULT		0xffffffff

// Max volume
#define	 MAX_VOLUME	(127)

// Pan: JA2 scale 0..255 (FARLEFT=0, MIDDLE=128, FARRIGHT=255)
#define		SOUND_PAN_CENTER		128

// Sound debug
CHAR8 SndDebugFileName[]="sound.log";

// Debug logging
void SoundLog(CHAR8 *strMessage);

//*******************************************************************************
// Local (file-scope) types and state
//*******************************************************************************

// Struct definition for sample slots in the cache. Holds the decoded sample
// (MIX_Audio*) as well as the scheduling data for random/ambient samples.
typedef struct {
				CHAR8		pName[256];						// Sample path (cache key)
				MIX_Audio*	pAudio;							// Decoded sample
				UINT32		uiFlags;						// Status flags (SAMPLE_*)
				UINT32		uiInstances;					// Channels currently referencing pAudio
				UINT32		uiCacheHits;					// Cache hits for this sample

				UINT32		uiTimeNext;						// Random sound data
				UINT32		uiTimeMin, uiTimeMax;
				UINT32		uiVolMin, uiVolMax;
				UINT32		uiPanMin, uiPanMax;				// 0..255
				UINT32		uiPriority;						// Priority
				UINT32		uiMaxInstances;					// Max allowable instances of sample
				} SAMPLETAG;

// Structure definition for slots in the sound output. Each channel owns a
// pre-created MIX_Track that is reused across every play.
typedef struct {
				MIX_Track*	pTrack;							// Pre-created mixer track
				UINT32		uiSample;						// Sample slot in cache (NO_SAMPLE == none)
				UINT32		uiSoundID;						// Sound unique ID (0 == free)
				UINT32		uiPriority;						// Priority
				void		(*EOSCallback)(void *);
				void		*pCallbackData;
				UINT32		uiTimeStamp;					// GetTickCount() at play start
				UINT32		uiVolume;						// Current volume 0..127
				UINT32		uiPan;							// Current pan 0..255
				BOOLEAN		fLooping;						// Infinite-loop flag
				} SOUNDTAG;

// Sample cache list for files loaded
static SAMPLETAG	pSampleList[SOUND_MAX_CACHED];
// Sound channel list for output channels
static SOUNDTAG		pSoundList[SOUND_MAX_CHANNELS];

// Global variables
UINT32		guiSoundDefaultVolume = 127;

// Local module variables
static MIX_Mixer*	gMixer				= NULL;		// SDL3_mixer device
BOOLEAN		fSoundSystemInit	= FALSE;		// Device up? (play calls no-op when FALSE)
static BOOLEAN		gfEnableStartup		= TRUE;		// Allow hardware to start up

// Forward declarations for file-local helpers
static BOOLEAN	SoundInitHardware(void);
static void		SoundShutdownHardware(void);
static UINT32	FindCachedSample(STR pFilename);
static UINT32	GetFreeSampleSlot(void);
static UINT32	LoadSampleFromFile(STR pFilename);
static void		LazyReapChannel(SOUNDTAG *pChannel);
static UINT32	SoundGetFreeChannel(void);
static UINT32	SoundGetIndexByID(UINT32 uiSoundID);
static UINT32	SoundGetUniqueID(void);
static UINT32	SoundStartSampleInternal(UINT32 uiSample, UINT32 uiChannel, SOUNDPARMS *pParms, BOOLEAN fHonorEOS);
static UINT32	SoundStartRandom(UINT32 uiSample);
static BOOLEAN	SoundRandomShouldPlay(UINT32 uiSample);
static void		ApplyPan(MIX_Track *pTrack, UINT32 uiPan);

//*******************************************************************************
// High Level Interface
//*******************************************************************************

//*******************************************************************************
// SoundEnableSound
//	Allows or disallows the startup of the sound hardware.
//*******************************************************************************
void SoundEnableSound(BOOLEAN fEnable)
{
	gfEnableStartup=fEnable;
}

//*******************************************************************************
// SoundGetDriverHandle
//
//	Cross-seam: this used to hand FMOD's DirectSound output object to the
//	Smacker/Bink cinematic players. There is no DirectSound object under
//	SDL3_mixer; return NULL. Both callers guard with if(pSoundDriver), so the
//	cinematics simply play without their own audio track in this phase.
//*******************************************************************************
void *SoundGetDriverHandle( void )
{
	return(NULL);
}

//*******************************************************************************
// InitializeSoundManager
//	Zeros out the channel/cache arrays and starts the audio device.
//	Returns TRUE always (matches the original's tolerance: on device failure
//	fSoundSystemInit stays FALSE and every play call becomes a no-op).
//*******************************************************************************
BOOLEAN InitializeSoundManager(void)
{
	UINT32 uiCount;

	if(fSoundSystemInit)
	{
		SoundLog("Reopening JA2 sound manager");
		ShutdownSoundManager();
	}
	else
		SoundLog("Initialising JA2 sound manager");

	SoundLog((CHAR8 *)String("	Using %d channels", SOUND_MAX_CHANNELS));
	SoundLog((CHAR8 *)String("	Using %d cache slots", SOUND_MAX_CACHED));

	for(uiCount=0; uiCount < SOUND_MAX_CHANNELS; uiCount++)
	{
		memset(&pSoundList[uiCount], 0, sizeof(SOUNDTAG));
		pSoundList[uiCount].uiSample  = NO_SAMPLE;
		pSoundList[uiCount].uiSoundID = 0;			// 0 == free
	}

	for(uiCount=0; uiCount < SOUND_MAX_CACHED; uiCount++)
	{
		memset(&pSampleList[uiCount], 0, sizeof(SAMPLETAG));
	}

#ifndef SOUND_DISABLE
	if(gfEnableStartup && SoundInitHardware())
		fSoundSystemInit=TRUE;
#endif

	return(TRUE);
}

//*******************************************************************************
// ShutdownSoundManager
//	Silences all currently playing sound, deallocates the cache and releases
//	the audio device.
//*******************************************************************************
void ShutdownSoundManager(void)
{
	SoundLog("Closing sound system...");

	SoundStopAll();
	SoundEmptyCache();
	SoundShutdownHardware();
	fSoundSystemInit=FALSE;
	SoundLog("JA2 sound manager shutdown");
}

//*******************************************************************************
// SoundPlay
//		Starts a sample playing. If the sample is not loaded in the cache, it will
//	be found and loaded, then played on a free channel. Returns a unique sound ID
//	or SOUND_ERROR.
//*******************************************************************************

std::map<std::string, uint64_t, std::less<>> gSoundMap;

uint64_t TimeMS()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

UINT32 SoundPlay(STR pFilename, SOUNDPARMS *pParms)
{
	UINT32 uiSample, uiChannel;

	if (!fSoundSystemInit)
	{
		SoundLog((CHAR8 *)String("SoundSystemInit FALSE"));
		return(SOUND_ERROR);
	}

	// sevenfm: limit simultaneous sound playing
	if (gGameExternalOptions.fLimitSimultaneousSound)
	{
		uint64_t curtime = TimeMS();
		std::string filename(pFilename);

		if (gSoundMap[filename] > curtime)
		{
			return 0;
		}

		// set delay for this sound type
		gSoundMap[filename] = curtime + 50;
	}

	if ((uiSample = SoundLoadSample(pFilename)) != NO_SAMPLE)
	{
		if ((uiChannel = SoundGetFreeChannel()) != SOUND_ERROR)
		{
			return(SoundStartSampleInternal(uiSample, uiChannel, pParms, FALSE));
		}
		else
		{
			SoundLog((CHAR8 *)String("Could not get free channel, uiChannel = %d", uiChannel));
		}
	}
	else
	{
		SoundLog((CHAR8 *)String("Could not load sample, uiSample = %d", uiSample));
	}

	return(SOUND_ERROR);
}

void ResetSoundMap(void)
{
	gSoundMap.clear();
}

//*******************************************************************************
// SoundPlayStreamedFile
//		Plays a (typically larger) file. Under SDL3_mixer this uses exactly the
//	same predecoded load+play path as SoundPlay, but -- unlike SoundPlay -- it
//	honors the SOUNDPARMS EOSCallback. This is the one path where a caller
//	(Music Control / PlayJA2StreamingSampleFromFile) properly initializes the
//	callback field; the SFX callers leave it as 0xff stack garbage.
//*******************************************************************************
UINT32	SoundPlayStreamedFile( STR pFilename, SOUNDPARMS *pParms )
{
	UINT32	uiSample, uiChannel;

	if( !fSoundSystemInit )
		return(SOUND_ERROR);

	if((uiSample = SoundLoadSample(pFilename)) == NO_SAMPLE)
	{
		SoundLog((CHAR8 *)String("	ERROR in SoundPlayStreamedFile():	Couldnt load '%s'", pFilename ) );
		return( SOUND_ERROR );
	}

	if((uiChannel=SoundGetFreeChannel())==SOUND_ERROR)
		return(SOUND_ERROR);

	return(SoundStartSampleInternal( uiSample, uiChannel, pParms, TRUE));
}

//*******************************************************************************
// SoundPlayRandom
//		Registers a sample to be played randomly within the specified parameters.
//	Random samples are always loaded into the cache and locked in place.
//	Returns the sample index, or SOUND_ERROR.
//*******************************************************************************
UINT32 SoundPlayRandom(STR pFilename, RANDOMPARMS *pParms)
{
	UINT32 uiSample;

	if(fSoundSystemInit)
	{
		if((uiSample=SoundLoadSample(pFilename))!=NO_SAMPLE)
		{
			// Sample loaded - marking slot
			pSampleList[uiSample].uiFlags|=(SAMPLE_RANDOM|SAMPLE_LOCKED);

			// Setup time intervals
			if(pParms->uiTimeMin==SOUND_PARMS_DEFAULT)
				return(SOUND_ERROR);
			else
				pSampleList[uiSample].uiTimeMin=pParms->uiTimeMin;

			if(pParms->uiTimeMax==SOUND_PARMS_DEFAULT)
				pSampleList[uiSample].uiTimeMax=pParms->uiTimeMin;
			else
				pSampleList[uiSample].uiTimeMax=pParms->uiTimeMax;

			// Volume
			if(pParms->uiVolMin==SOUND_PARMS_DEFAULT)
				pSampleList[uiSample].uiVolMin=guiSoundDefaultVolume;
			else
				pSampleList[uiSample].uiVolMin=pParms->uiVolMin;

			if(pParms->uiVolMax==SOUND_PARMS_DEFAULT)
				pSampleList[uiSample].uiVolMax=guiSoundDefaultVolume;
			else
				pSampleList[uiSample].uiVolMax=pParms->uiVolMax;

			// Panning (JA2 0..255 scale, 128 == center)
			if(pParms->uiPanMin==SOUND_PARMS_DEFAULT)
			{
				pSampleList[uiSample].uiPanMin=SOUND_PAN_CENTER;
				pSampleList[uiSample].uiPanMax=SOUND_PAN_CENTER;
			}
			else
			{
				pSampleList[uiSample].uiPanMin=pParms->uiPanMin;
				pSampleList[uiSample].uiPanMax=pParms->uiPanMax;
			}

			// Guard max>=min: SoundServiceRandom computes Random(uiXxxMax-uiXxxMin),
			// so a caller passing an explicit Max below Min would underflow the
			// UINT32 subtraction (ambient scheduled ~never, garbage vol/pan).
			if( pSampleList[uiSample].uiTimeMax < pSampleList[uiSample].uiTimeMin ) pSampleList[uiSample].uiTimeMax = pSampleList[uiSample].uiTimeMin;
			if( pSampleList[uiSample].uiVolMax  < pSampleList[uiSample].uiVolMin  ) pSampleList[uiSample].uiVolMax  = pSampleList[uiSample].uiVolMin;
			if( pSampleList[uiSample].uiPanMax  < pSampleList[uiSample].uiPanMin  ) pSampleList[uiSample].uiPanMax  = pSampleList[uiSample].uiPanMin;

			// Max instances
			if(pParms->uiMaxInstances==SOUND_PARMS_DEFAULT)
				pSampleList[uiSample].uiMaxInstances=1;
			else
				pSampleList[uiSample].uiMaxInstances=pParms->uiMaxInstances;

			// Priority
			if(pParms->uiPriority==SOUND_PARMS_DEFAULT)
				pSampleList[uiSample].uiPriority=PRIORITY_RANDOM;
			else
				pSampleList[uiSample].uiPriority=pParms->uiPriority;

			pSampleList[uiSample].uiInstances=0;

			// Time stamp
			pSampleList[uiSample].uiTimeNext=GetTickCount()+pSampleList[uiSample].uiTimeMin+Random(pSampleList[uiSample].uiTimeMax-pSampleList[uiSample].uiTimeMin);

			return(uiSample);
		}
		else
		{
			SoundLog((CHAR8 *)String("	ERROR in SoundPlayRandom():	Couldnt open '%s'", pFilename ) );
		}
	}

	return(SOUND_ERROR);
}

//*******************************************************************************
// SoundIsPlaying
//		Returns TRUE/FALSE that an instance of a sound is still playing.
//*******************************************************************************
BOOLEAN SoundIsPlaying(UINT32 uiSoundID)
{
	UINT32 uiSound;

	if(fSoundSystemInit)
	{
		uiSound=SoundGetIndexByID(uiSoundID);
		if(uiSound!=NO_SAMPLE)
		{
			return((pSoundList[uiSound].pTrack && MIX_TrackPlaying(pSoundList[uiSound].pTrack)) ? TRUE : FALSE);
		}
	}

	return(FALSE);
}

//*******************************************************************************
// SoundStop
//		Stops the playing of a sound instance, if still playing.
//*******************************************************************************
BOOLEAN SoundStop(UINT32 uiSoundID)
{
	UINT32 uiSound;

	if(fSoundSystemInit)
	{
		uiSound=SoundGetIndexByID(uiSoundID);
		if(uiSound!=NO_SAMPLE)
		{
			if(pSoundList[uiSound].pTrack)
				MIX_StopTrack(pSoundList[uiSound].pTrack, 0);
			LazyReapChannel(&pSoundList[uiSound]);	// clear slot + fire EOSCallback (main thread)
			return(TRUE);
		}
	}

	return(FALSE);
}

//*******************************************************************************
// SoundSetMemoryLimit / SoundSetCacheThreshhold
//	SDL3_mixer predecodes samples, so the original raw-byte memory cache and its
//	LRU/threshold accounting no longer apply. These become no-ops returning TRUE.
//*******************************************************************************
BOOLEAN SoundSetMemoryLimit(UINT32 uiLimit)
{
	return(TRUE);
}

BOOLEAN SoundSetCacheThreshhold(UINT32 uiThreshold)
{
	return(TRUE);
}

//*******************************************************************************
// SoundGetSystemInfo
//	Returns FALSE, always (kept for parity with the original).
//*******************************************************************************
BOOLEAN SoundGetSystemInfo(void)
{
	return(FALSE);
}

//*******************************************************************************
// SoundSetDefaultVolume / SoundGetDefaultVolume
//	Master volume 0..127 (applied as the mixer's overall gain).
//*******************************************************************************
void SoundSetDefaultVolume(UINT32 uiVolume)
{
	guiSoundDefaultVolume=__min(uiVolume, MAX_VOLUME);
	if(gMixer)
		MIX_SetMixerGain(gMixer, guiSoundDefaultVolume / (float)MAX_VOLUME);
}

UINT32 SoundGetDefaultVolume(void)
{
	return(guiSoundDefaultVolume);
}

//*******************************************************************************
// SoundStopAll
//		Stops all currently playing sounds.
//*******************************************************************************
BOOLEAN SoundStopAll(void)
{
	UINT32 uiCount;

	SoundLog("	Stopping all sounds");

	if(fSoundSystemInit)
	{
		for(uiCount=0; uiCount < SOUND_MAX_CHANNELS; uiCount++)
		{
			if(pSoundList[uiCount].pTrack)
			{
				MIX_StopTrack(pSoundList[uiCount].pTrack, 0);
				LazyReapChannel(&pSoundList[uiCount]);
			}
		}
	}

	return(TRUE);
}

//*******************************************************************************
// SoundSetVolume
//		Sets the volume (0..127) on a currently playing sound.
//*******************************************************************************
BOOLEAN SoundSetVolume(UINT32 uiSoundID, UINT32 uiVolume)
{
	UINT32 uiSound, uiVolCap;

	if(fSoundSystemInit)
	{
		uiVolCap=__min(uiVolume, MAX_VOLUME);

		if((uiSound=SoundGetIndexByID(uiSoundID))!=NO_SAMPLE)
		{
			pSoundList[uiSound].uiVolume = uiVolCap;
			if(pSoundList[uiSound].pTrack)
				MIX_SetTrackGain(pSoundList[uiSound].pTrack, uiVolCap / (float)MAX_VOLUME);
			return(TRUE);
		}
	}

	return(FALSE);
}

//*******************************************************************************
// SoundSetPan
//		Sets the pan (JA2 0..255 scale, 128 == center) on a currently playing sound.
//*******************************************************************************
BOOLEAN SoundSetPan(UINT32 uiSoundID, UINT32 uiPan)
{
	UINT32 uiSound, uiPanCap;

	if(fSoundSystemInit)
	{
		uiPanCap=__min(uiPan, 255);

		if((uiSound=SoundGetIndexByID(uiSoundID))!=NO_SAMPLE)
		{
			pSoundList[uiSound].uiPan = uiPanCap;
			ApplyPan(pSoundList[uiSound].pTrack, uiPanCap);
			return(TRUE);
		}
	}

	return(FALSE);
}

//*******************************************************************************
// SoundGetVolume
//		Returns the current volume (0..127) of a playing sound, or SOUND_ERROR.
//*******************************************************************************
UINT32 SoundGetVolume(UINT32 uiSoundID)
{
	UINT32 uiSound;

	if(fSoundSystemInit)
	{
		if((uiSound=SoundGetIndexByID(uiSoundID))!=NO_SAMPLE)
			return(pSoundList[uiSound].uiVolume);
	}

	return(SOUND_ERROR);
}

//*******************************************************************************
// SoundServiceRandom
//		Polled every frame. Fires each due random/ambient sample that is below its
//	concurrent-instance cap, and reschedules it.
//*******************************************************************************
BOOLEAN SoundServiceRandom(void)
{
	UINT32 uiCount;
	BOOLEAN fRandomSoundWasCreated=FALSE;

	if(!fSoundSystemInit)
		return(FALSE);

	for(uiCount=0; uiCount < SOUND_MAX_CACHED; uiCount++)
	{
		if(!(pSampleList[uiCount].uiFlags&SAMPLE_RANDOM_MANUAL) && SoundRandomShouldPlay(uiCount))
			fRandomSoundWasCreated |= SoundStartRandom(uiCount);
	}

	return(fRandomSoundWasCreated);
}

//*******************************************************************************
// SoundRandomShouldPlay
//	Determines whether a random sound is ready for playing or not.
//*******************************************************************************
static BOOLEAN SoundRandomShouldPlay(UINT32 uiSample)
{
	if(pSampleList[uiSample].uiFlags&SAMPLE_RANDOM)
		if(pSampleList[uiSample].uiTimeNext <= GetTickCount())
			if(pSampleList[uiSample].uiInstances < pSampleList[uiSample].uiMaxInstances)
			{
				return(TRUE);
			}

	return(FALSE);
}

//*******************************************************************************
// SoundStartRandom
//	Starts an instance of a random sample and reschedules its next play.
//*******************************************************************************
static UINT32 SoundStartRandom(UINT32 uiSample)
{
	UINT32 uiChannel, uiSoundID;
	SOUNDPARMS spParms;

	if((uiChannel=SoundGetFreeChannel())!=SOUND_ERROR)
	{
		memset(&spParms, 0xff, sizeof(SOUNDPARMS));

		spParms.uiVolume=pSampleList[uiSample].uiVolMin+Random(pSampleList[uiSample].uiVolMax-pSampleList[uiSample].uiVolMin);
		spParms.uiPan=pSampleList[uiSample].uiPanMin+Random(pSampleList[uiSample].uiPanMax-pSampleList[uiSample].uiPanMin);
		spParms.uiLoop=1;
		spParms.uiPriority=pSampleList[uiSample].uiPriority;

		// SoundStartSampleInternal increments uiInstances on success.
		if((uiSoundID=SoundStartSampleInternal(uiSample, uiChannel, &spParms, FALSE))!=SOUND_ERROR)
		{
			pSampleList[uiSample].uiTimeNext=GetTickCount()+pSampleList[uiSample].uiTimeMin+Random(pSampleList[uiSample].uiTimeMax-pSampleList[uiSample].uiTimeMin);
			return(TRUE);
		}
		else
			SoundLog((CHAR8 *)String("	ERROR in SoundStartRandom(): Sample #%d start error", uiSample));
	}
	else
		SoundLog("	ERROR in SoundStartRandom(): Failed to get free channel");
	return(FALSE);
}

//*******************************************************************************
// SoundStopAllRandom
//		Stops ONLY the channels playing a SAMPLE_RANDOM (ambient) sample -- never
//	the music or ordinary SFX -- then unregisters the random samples so they are
//	no longer serviced and can leave the cache.
//*******************************************************************************
BOOLEAN SoundStopAllRandom(void)
{
	UINT32 uiChannel, uiSample;

	// Stop all currently playing random sounds
	for(uiChannel=0; uiChannel < SOUND_MAX_CHANNELS; uiChannel++)
	{
		if( pSoundList[uiChannel].pTrack!=NULL && pSoundList[uiChannel].uiSoundID!=0 )
		{
			uiSample=pSoundList[uiChannel].uiSample;

			// if this was a random sample, stop it and reap the channel
			if (uiSample != NO_SAMPLE && uiSample < SOUND_MAX_CACHED && (pSampleList[uiSample].uiFlags & SAMPLE_RANDOM))
			{
				MIX_StopTrack(pSoundList[uiChannel].pTrack, 0);
				LazyReapChannel(&pSoundList[uiChannel]);	// frees the slot + decrements instances
			}
		}
	}

	// Unlock all random sounds so they can be dumped from the cache, and
	// take the random flag off so they won't be serviced/played
	for(uiSample=0; uiSample < SOUND_MAX_CACHED; uiSample++)
	{
		if(pSampleList[uiSample].uiFlags & SAMPLE_RANDOM)
		{
			pSampleList[uiSample].uiFlags &= (~(SAMPLE_RANDOM | SAMPLE_LOCKED));
			pSampleList[uiSample].uiInstances = 0;
		}
	}

	return(TRUE);
}

//*******************************************************************************
// SoundServiceStreams
//		Polled every frame (from MusicPoll and the main loop). SDL3_mixer streams
//	internally, so there are no decode buffers to refill; instead we reap finished
//	channels on the main thread. That reap fires each finished sound's EOSCallback
//	(music-advance / streamed-SFX end) promptly, on the main thread.
//*******************************************************************************
BOOLEAN SoundServiceStreams(void)
{
	UINT32 uiCount;

	if(fSoundSystemInit)
	{
		for(uiCount=0; uiCount < SOUND_MAX_CHANNELS; uiCount++)
		{
			if(pSoundList[uiCount].pTrack)
				LazyReapChannel(&pSoundList[uiCount]);
		}
	}

	return(TRUE);
}

//*******************************************************************************
// SoundGetPosition
//	Reports the elapsed wall-clock time of the sample in milliseconds. This
//	deliberately returns GetTickCount()-timestamp (NOT the true mixer frame
//	position): Tactical/GAP.cpp speech lip-sync was tuned to the original's
//	wall-clock behavior.
//*******************************************************************************
UINT32 SoundGetPosition(UINT32 uiSoundID)
{
	UINT32 uiSound, uiTime, uiPosition;

	if(fSoundSystemInit)
	{
		if((uiSound=SoundGetIndexByID(uiSoundID))!=NO_SAMPLE)
		{
			uiTime=GetTickCount();
			// check for rollover
			if(uiTime < pSoundList[uiSound].uiTimeStamp)
				uiPosition=(0-pSoundList[uiSound].uiTimeStamp)+uiTime;
			else
				uiPosition=(uiTime-pSoundList[uiSound].uiTimeStamp);

			return(uiPosition);
		}
	}
	return(0);
}

//*******************************************************************************
// Cacheing Subsystem
//*******************************************************************************

//*******************************************************************************
// SoundEmptyCache
//		Frees up all (non-locked, idle) samples in the cache.
//*******************************************************************************
BOOLEAN SoundEmptyCache(void)
{
	UINT32 uiCount;

	SoundLog("Cleaning cache");
	SoundStopAll();

	for(uiCount=0; uiCount < SOUND_MAX_CACHED; uiCount++)
	{
		if(pSampleList[uiCount].pAudio && pSampleList[uiCount].uiInstances==0 && !(pSampleList[uiCount].uiFlags&SAMPLE_LOCKED))
		{
			MIX_DestroyAudio(pSampleList[uiCount].pAudio);
			memset(&pSampleList[uiCount], 0, sizeof(SAMPLETAG));
		}
	}

	return(TRUE);
}

//*******************************************************************************
// SoundLoadSample
//		Loads a sample into cache. Returns the sample index, or NO_SAMPLE on error.
//*******************************************************************************
UINT32 SoundLoadSample(STR pFilename)
{
	UINT32 uiSample=NO_SAMPLE;

	if(!gMixer || !pFilename)
		return(NO_SAMPLE);

	if((uiSample=FindCachedSample(pFilename))!=NO_SAMPLE)
		return(uiSample);

	return(LoadSampleFromFile(pFilename));
}

//*******************************************************************************
// SoundLockSample
//		Locks a sample into the cache so it won't be evicted. Loads it if needed.
//*******************************************************************************
UINT32 SoundLockSample(STR pFilename)
{
	UINT32 uiSample;

	uiSample=FindCachedSample(pFilename);
	if(uiSample==NO_SAMPLE)
		uiSample=LoadSampleFromFile(pFilename);

	if(uiSample!=NO_SAMPLE)
	{
		pSampleList[uiSample].uiFlags|=SAMPLE_LOCKED;
		return(uiSample);
	}

	return(NO_SAMPLE);
}

//*******************************************************************************
// SoundUnlockSample
//		Removes the lock on a sample so the cache is free to dump it.
//*******************************************************************************
UINT32 SoundUnlockSample(STR pFilename)
{
	UINT32 uiSample;

	if((uiSample=FindCachedSample(pFilename))!=NO_SAMPLE)
	{
		pSampleList[uiSample].uiFlags&=(~SAMPLE_LOCKED);
		return(uiSample);
	}

	return(NO_SAMPLE);
}

//*******************************************************************************
// SoundFreeSample
//		Releases the resources associated with a sample from the cache (if idle).
//*******************************************************************************
UINT32 SoundFreeSample(STR pFilename)
{
	UINT32 uiSample;

	if((uiSample=FindCachedSample(pFilename))!=NO_SAMPLE)
	{
		if(pSampleList[uiSample].uiInstances==0)
		{
			if(pSampleList[uiSample].pAudio)
				MIX_DestroyAudio(pSampleList[uiSample].pAudio);
			memset(&pSampleList[uiSample], 0, sizeof(SAMPLETAG));
			return(uiSample);
		}
	}

	return(NO_SAMPLE);
}

//*******************************************************************************
// FindCachedSample
//		Tries to locate a sound by filename among the currently cached samples.
//*******************************************************************************
static UINT32 FindCachedSample(STR pFilename)
{
	UINT32 uiCount;

	if(!pFilename)
		return(NO_SAMPLE);

	for(uiCount=0; uiCount < SOUND_MAX_CACHED; uiCount++)
	{
		if(pSampleList[uiCount].pAudio && _stricmp(pSampleList[uiCount].pName, pFilename)==0)
			return(uiCount);
	}

	return(NO_SAMPLE);
}

//*******************************************************************************
// GetFreeSampleSlot
//		Returns a free cache slot. First an unused one, then (if full) an idle,
//	non-locked slot is evicted to make room. Returns NO_SAMPLE if none.
//*******************************************************************************
static UINT32 GetFreeSampleSlot(void)
{
	UINT32 uiCount;

	// First pass: an unused slot.
	for(uiCount=0; uiCount < SOUND_MAX_CACHED; uiCount++)
	{
		if(pSampleList[uiCount].pAudio==NULL)
			return(uiCount);
	}

	// Second pass: evict a non-locked, idle sample.
	for(uiCount=0; uiCount < SOUND_MAX_CACHED; uiCount++)
	{
		if(pSampleList[uiCount].uiInstances==0 && !(pSampleList[uiCount].uiFlags&SAMPLE_LOCKED))
		{
			MIX_DestroyAudio(pSampleList[uiCount].pAudio);
			memset(&pSampleList[uiCount], 0, sizeof(SAMPLETAG));
			return(uiCount);
		}
	}

	return(NO_SAMPLE);
}

//*******************************************************************************
// LoadSampleFromFile
//		Reads a sound file from disk (through FileMan, so SLF archives work) into
//	memory, sanitizes a malformed RIFF/WAVE header, decodes it via SDL3_mixer,
//	and stores the decoded MIX_Audio in a cache slot.
//*******************************************************************************
static UINT32 LoadSampleFromFile(STR pFilename)
{
	// Sound hardware never came up (MIX device failed to init): don't open,
	// decode and cache an unusable sample. SoundLockSample reaches here bypassing
	// SoundLoadSample's !gMixer guard.
	if( !gMixer ) return( NO_SAMPLE );

	HWFILE	hFile;
	UINT32	uiSize, uiSample, uiBytesRead;
	void	*pBuffer;
	SDL_IOStream	*pIO;
	MIX_Audio		*pAudio;

	if((hFile=FileOpen(pFilename, FILE_ACCESS_READ | FILE_OPEN_EXISTING, FALSE))==0)
	{
		SoundLog((CHAR8 *)String("	ERROR in LoadSampleFromFile(): Failed to open '%s'", pFilename));
		return(NO_SAMPLE);
	}

	uiSize=FileGetSize(hFile);
	if(uiSize == 0)
	{
		FileClose(hFile);
		return(NO_SAMPLE);
	}

	if((pBuffer=malloc(uiSize))==NULL)
	{
		FileClose(hFile);
		return(NO_SAMPLE);
	}

	uiBytesRead=0;
	FileRead(hFile, pBuffer, uiSize, &uiBytesRead);
	FileClose(hFile);
	if(uiBytesRead != uiSize)
	{
		free(pBuffer);
		return(NO_SAMPLE);
	}

	// Sanitize a malformed RIFF/WAVE header. Many original JA2 speech assets
	// (SPEECH\*.wav) store a RIFF chunk size a few bytes smaller than the file.
	// FMOD tolerated it; SDL3_mixer's stricter WAV decoder clamps to the stored
	// size and then rejects the data chunk ("unknown/unsupported/corrupt format"),
	// so speech would go silent. Only the size field lies -- patch bytes[4..7] to
	// (fileSize - 8), the canonical value, when the stored value is too small.
	if(uiSize >= 12)
	{
		const unsigned char *b = (const unsigned char *)pBuffer;
		if( b[0]=='R' && b[1]=='I' && b[2]=='F' && b[3]=='F' &&
			b[8]=='W' && b[9]=='A' && b[10]=='V' && b[11]=='E' )
		{
			UINT32 uiRiffSize = (UINT32)b[4] | ((UINT32)b[5]<<8) | ((UINT32)b[6]<<16) | ((UINT32)b[7]<<24);
			UINT32 uiExpected = uiSize - 8;
			if(uiRiffSize < uiExpected)
			{
				unsigned char *w = (unsigned char *)pBuffer;
				w[4] = (unsigned char)( uiExpected        & 0xFF);
				w[5] = (unsigned char)((uiExpected >> 8)  & 0xFF);
				w[6] = (unsigned char)((uiExpected >> 16) & 0xFF);
				w[7] = (unsigned char)((uiExpected >> 24) & 0xFF);
			}
		}
	}

	// Own the buffer lifecycle explicitly: predecode=true (so the decode happens
	// during the load and the IO is no longer needed afterward), closeio=false
	// (we close the IO ourselves), then free our buffer.
	pIO = SDL_IOFromMem(pBuffer, uiSize);
	if(!pIO)
	{
		free(pBuffer);
		return(NO_SAMPLE);
	}
	pAudio = MIX_LoadAudio_IO(gMixer, pIO, true, false);
	SDL_CloseIO(pIO);
	free(pBuffer);
	if(!pAudio)
	{
		SoundLog((CHAR8 *)String("	ERROR in LoadSampleFromFile(): MIX_LoadAudio_IO('%s') failed: %s", pFilename, SDL_GetError()));
		return(NO_SAMPLE);
	}

	if((uiSample=GetFreeSampleSlot())==NO_SAMPLE)
	{
		SoundLog((CHAR8 *)String("	ERROR in LoadSampleFromFile(): '%s', cache slots are full", pFilename));
		MIX_DestroyAudio(pAudio);
		return(NO_SAMPLE);
	}

	memset(&pSampleList[uiSample], 0, sizeof(SAMPLETAG));
	strncpy(pSampleList[uiSample].pName, pFilename, sizeof(pSampleList[uiSample].pName)-1);
	pSampleList[uiSample].pName[sizeof(pSampleList[uiSample].pName)-1]='\0';
	pSampleList[uiSample].pAudio=pAudio;
	pSampleList[uiSample].uiFlags|=SAMPLE_ALLOCATED;
	pSampleList[uiSample].uiInstances=0;
	return(uiSample);
}

//*******************************************************************************
// Low Level Interface (Local use only)
//*******************************************************************************

//*******************************************************************************
// LazyReapChannel
//		If the channel's track has finished playing, clear the channel state on the
//	main thread and fire its EOSCallback. We deliberately do NOT use SDL3_mixer's
//	audio-thread stopped callback (mutating channel/sample state there races the
//	main thread). This is the single point where a finished channel is cleaned up.
//*******************************************************************************
static void LazyReapChannel(SOUNDTAG *pChannel)
{
	void	(*pCallback)(void *);
	void	*pCallbackData;
	UINT32	uiSample;

	if(pChannel->uiSoundID==0 || pChannel->pTrack==NULL)
		return;
	if(MIX_TrackPlaying(pChannel->pTrack))
		return;		// still playing

	// Snapshot + clear the callback before firing it, so anything the callback
	// does (e.g. MusicStopCallback -> queues the next song) sees a clean slot.
	pCallback     = pChannel->EOSCallback;
	pCallbackData = pChannel->pCallbackData;

	uiSample = pChannel->uiSample;
	if(uiSample != NO_SAMPLE && uiSample < SOUND_MAX_CACHED)
	{
		if(pSampleList[uiSample].uiInstances > 0)
			pSampleList[uiSample].uiInstances--;
	}

	pChannel->uiSample      = NO_SAMPLE;
	pChannel->uiSoundID     = 0;
	pChannel->EOSCallback   = NULL;
	pChannel->pCallbackData = NULL;
	pChannel->fLooping      = FALSE;

	if(pCallback!=NULL)
		pCallback(pCallbackData);
}

//*******************************************************************************
// SoundGetFreeChannel
//		Reaps any finished channels, then returns an unused sound channel, or
//	SOUND_ERROR if none are free.
//*******************************************************************************
static UINT32 SoundGetFreeChannel(void)
{
	UINT32 uiCount;

	// First reap any channels whose tracks have finished (main-thread only).
	for(uiCount=0; uiCount < SOUND_MAX_CHANNELS; uiCount++)
	{
		if(pSoundList[uiCount].pTrack)
			LazyReapChannel(&pSoundList[uiCount]);
	}

	// Now find a channel that has a track but is not in use.
	for(uiCount=0; uiCount < SOUND_MAX_CHANNELS; uiCount++)
	{
		if(pSoundList[uiCount].pTrack && pSoundList[uiCount].uiSoundID==0)
			return(uiCount);
	}

	return(SOUND_ERROR);
}

//*******************************************************************************
// SoundGetIndexByID
//		Searches out a sound instance by its ID, reaping it first if it has since
//	finished. Returns the channel slot, or NO_SAMPLE.
//*******************************************************************************
static UINT32 SoundGetIndexByID(UINT32 uiSoundID)
{
	UINT32 uiCount;

	if(uiSoundID==0 || uiSoundID==NO_SAMPLE)
		return(NO_SAMPLE);

	for(uiCount=0; uiCount < SOUND_MAX_CHANNELS; uiCount++)
	{
		if(pSoundList[uiCount].uiSoundID==uiSoundID)
		{
			// Lazy cleanup: if it has since finished, reap it here.
			LazyReapChannel(&pSoundList[uiCount]);
			return((pSoundList[uiCount].uiSoundID==uiSoundID) ? uiCount : NO_SAMPLE);
		}
	}

	return(NO_SAMPLE);
}

//*******************************************************************************
// ApplyPan
//		Applies a JA2 0..255 pan (0=left, 128=center, 255=right) to a track via
//	SDL3_mixer's forced-stereo per-channel gains. Center clears spatialization.
//*******************************************************************************
static void ApplyPan(MIX_Track *pTrack, UINT32 uiPan)
{
	float			panF;
	MIX_StereoGains	gains;

	if(!pTrack)
		return;

	if(uiPan==SOUND_PAN_CENTER)
	{
		MIX_SetTrackStereo(pTrack, NULL);	// centered -- disable forced stereo
		return;
	}

	// -1.0 (full left) .. +~0.99 (full right)
	panF = ((INT32)uiPan - SOUND_PAN_CENTER) / (float)SOUND_PAN_CENTER;
	gains.left  = (panF <= 0.0f) ? 1.0f : (1.0f - panF);
	gains.right = (panF >= 0.0f) ? 1.0f : (1.0f + panF);
	MIX_SetTrackStereo(pTrack, &gains);
}

//*******************************************************************************
// SoundInitHardware
//		Starts the SDL audio subsystem and the SDL3_mixer device, then pre-creates
//	one MIX_Track per channel. Returns TRUE on success, FALSE otherwise (the
//	caller keeps running with sound disabled).
//*******************************************************************************
static BOOLEAN SoundInitHardware(void)
{
	UINT32 uiCount;

	SoundLog("Init hardware...");

	if(!SDL_InitSubSystem(SDL_INIT_AUDIO))
	{
		SoundLog((CHAR8 *)String("	ERROR in SoundInitHardware(): SDL_InitSubSystem(AUDIO): %s", SDL_GetError()));
		return(FALSE);
	}

	if(!MIX_Init())
	{
		SoundLog((CHAR8 *)String("	ERROR in SoundInitHardware(): MIX_Init: %s", SDL_GetError()));
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return(FALSE);
	}

	gMixer = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
	if(!gMixer)
	{
		SoundLog((CHAR8 *)String("	ERROR in SoundInitHardware(): MIX_CreateMixerDevice: %s", SDL_GetError()));
		MIX_Quit();
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return(FALSE);
	}

	MIX_SetMixerGain(gMixer, guiSoundDefaultVolume / (float)MAX_VOLUME);

	// Pre-allocate one MIX_Track per channel slot. They are reused across every
	// play (new audio is attached with MIX_SetTrackAudio). We intentionally do
	// NOT register a stopped-callback -- reaping happens lazily on the main
	// thread (LazyReapChannel).
	for(uiCount=0; uiCount < SOUND_MAX_CHANNELS; uiCount++)
	{
		pSoundList[uiCount].pTrack = MIX_CreateTrack(gMixer);
	}

	SoundLog("	SDL3_mixer started");
	return(TRUE);
}

//*******************************************************************************
// SoundShutdownHardware
//		Destroys all tracks and decoded samples, closes the mixer device and the
//	SDL audio subsystem.
//*******************************************************************************
static void SoundShutdownHardware(void)
{
	UINT32 uiCount;

	if(!gMixer)
		return;

	// Destroy tracks first (they may reference the decoded audio), then audio.
	for(uiCount=0; uiCount < SOUND_MAX_CHANNELS; uiCount++)
	{
		if(pSoundList[uiCount].pTrack)
		{
			MIX_StopTrack(pSoundList[uiCount].pTrack, 0);
			MIX_DestroyTrack(pSoundList[uiCount].pTrack);
			pSoundList[uiCount].pTrack = NULL;
		}
	}

	for(uiCount=0; uiCount < SOUND_MAX_CACHED; uiCount++)
	{
		if(pSampleList[uiCount].pAudio)
		{
			MIX_DestroyAudio(pSampleList[uiCount].pAudio);
			memset(&pSampleList[uiCount], 0, sizeof(SAMPLETAG));
		}
	}

	MIX_DestroyMixer(gMixer);
	gMixer = NULL;
	MIX_Quit();
	SDL_QuitSubSystem(SDL_INIT_AUDIO);

	SoundLog("	SDL3_mixer closed");
}

//*******************************************************************************
// SoundStartSampleInternal
//		Starts a cached sample on a specific channel. Override parameters come from
//	pParms (any field == SOUND_PARMS_DEFAULT is filled in by the system). When
//	fHonorEOS is TRUE the SOUNDPARMS EOSCallback is honored (streamed path only).
//	Returns a unique sound ID, or SOUND_ERROR.
//*******************************************************************************
static UINT32 SoundStartSampleInternal(UINT32 uiSample, UINT32 uiChannel, SOUNDPARMS *pParms, BOOLEAN fHonorEOS)
{
	SOUNDTAG		*pChannel;
	UINT32			uiSoundID, uiVolume, uiPan, uiLoop;
	BOOLEAN			fLoopForever;
	int				iMixLoops;
	SDL_PropertiesID	uiOpts;

	if(!fSoundSystemInit)
		return(SOUND_ERROR);

	pChannel = &pSoundList[uiChannel];
	if(!pChannel->pTrack)
		return(SOUND_ERROR);

	// Attach the decoded sample to this channel's track.
	if(!MIX_SetTrackAudio(pChannel->pTrack, pSampleList[uiSample].pAudio))
	{
		SoundLog((CHAR8 *)String("	ERROR in SoundStartSample(): MIX_SetTrackAudio: %s", SDL_GetError()));
		return(SOUND_ERROR);
	}

	// Volume (0..127). Speed and pitchbend are not used (as in the original).
	if((pParms!=NULL) && (pParms->uiVolume!=SOUND_PARMS_DEFAULT))
		uiVolume=__min(pParms->uiVolume, MAX_VOLUME);
	else
		uiVolume=guiSoundDefaultVolume;

	// Pan (JA2 0..255, 128 == center).
	if((pParms!=NULL) && (pParms->uiPan!=SOUND_PARMS_DEFAULT))
		uiPan=__min(pParms->uiPan, 255);
	else
		uiPan=SOUND_PAN_CENTER;

	// Loop: 0 == loop forever (and lock the sample), 1 == play once, N == N plays.
	if((pParms!=NULL) && (pParms->uiLoop!=SOUND_PARMS_DEFAULT))
		uiLoop=pParms->uiLoop;
	else
		uiLoop=1;
	fLoopForever = (uiLoop==0);
	if(fLoopForever)
		pSampleList[uiSample].uiFlags|=SAMPLE_LOCKED;	// don't evict an infinitely-looping sample

	MIX_SetTrackGain(pChannel->pTrack, uiVolume / (float)MAX_VOLUME);
	ApplyPan(pChannel->pTrack, uiPan);

	// SDL3_mixer loop convention: number of extra plays after the first; -1 is
	// infinite, 0 is play-once. The loop count MUST be supplied to MIX_PlayTrack
	// via MIX_PROP_PLAY_LOOPS_NUMBER (MIX_SetTrackLoops has no effect on a stopped
	// track, and starting a stopped track resets the count).
	iMixLoops = fLoopForever ? -1 : ((int)uiLoop - 1);
	uiOpts = SDL_CreateProperties();
	SDL_SetNumberProperty(uiOpts, MIX_PROP_PLAY_LOOPS_NUMBER, iMixLoops);
	if(!MIX_PlayTrack(pChannel->pTrack, uiOpts))
	{
		SDL_DestroyProperties(uiOpts);
		SoundLog((CHAR8 *)String("	ERROR in SoundStartSample(): MIX_PlayTrack: %s", SDL_GetError()));
		return(SOUND_ERROR);
	}
	SDL_DestroyProperties(uiOpts);

	// Bind the instance to the channel.
	uiSoundID = SoundGetUniqueID();
	pChannel->uiSoundID  = uiSoundID;
	pChannel->uiSample   = uiSample;
	pChannel->uiVolume   = uiVolume;
	pChannel->uiPan      = uiPan;
	pChannel->fLooping   = fLoopForever;
	pChannel->uiTimeStamp= GetTickCount();

	// Priority
	if((pParms!=NULL) && (pParms->uiPriority!=SOUND_PARMS_DEFAULT))
		pChannel->uiPriority=pParms->uiPriority;
	else
		pChannel->uiPriority=PRIORITY_MAX;

	// End-of-sample callback -- honored on the streamed path only. Callers memset
	// SOUNDPARMS to 0xff, so an unset callback arrives as SOUND_PARMS_DEFAULT.
	pChannel->EOSCallback   = NULL;
	pChannel->pCallbackData = NULL;
	if(fHonorEOS && (pParms!=NULL) && ((UINT32)pParms->EOSCallback!=SOUND_PARMS_DEFAULT) && (pParms->EOSCallback!=NULL))
	{
		pChannel->EOSCallback   = pParms->EOSCallback;
		pChannel->pCallbackData = pParms->pCallbackData;
	}

	pSampleList[uiSample].uiInstances++;
	pSampleList[uiSample].uiCacheHits++;

	return(uiSoundID);
}

//*******************************************************************************
// SoundGetUniqueID
//		Returns a unique, non-zero, non-sentinel ID with every call (0 marks a free
//	channel and 0xffffffff is NO_SAMPLE/SOUND_ERROR, so both are skipped).
//*******************************************************************************
static UINT32 SoundGetUniqueID(void)
{
	static UINT32 uiNextID=1;

	if(uiNextID==0 || uiNextID==NO_SAMPLE)
		uiNextID=1;

	return(uiNextID++);
}

// FUNCTIONS TO SET / RESET SAMPLE FLAGS
void SoundSetSampleFlags( UINT32 uiSample, UINT32 uiFlags )
{
	// CHECK FOR VALID SAMPLE
	if(uiSample < SOUND_MAX_CACHED && (pSampleList[ uiSample ].uiFlags&SAMPLE_ALLOCATED) )
	{
		// SET
		pSampleList[uiSample].uiFlags |= uiFlags;
	}
}

void SoundRemoveSampleFlags( UINT32 uiSample, UINT32 uiFlags )
{
	// CHECK FOR VALID SAMPLE
	if(uiSample < SOUND_MAX_CACHED && (pSampleList[ uiSample ].uiFlags&SAMPLE_ALLOCATED) )
	{
		//REMOVE
		pSampleList[uiSample].uiFlags &= (~uiFlags);
	}
}

//*****************************************************************************************
// SoundLog
//	Writes string into log file
//
// Returns nothing
//
// Created:	10.12.2005 Lesh
//*****************************************************************************************
void SoundLog(CHAR8 *strMessage)
{
	static struct SoundLog {
		sgp::Logger_ID id;
		SoundLog() {
			id = sgp::Logger::instance().createLogger();
			sgp::Logger::instance().connectFile(id, SndDebugFileName, true, sgp::Logger::FLUSH_ON_DELETE);
		}
	} s_SoundLog;
	SGP_LOG(s_SoundLog.id, vfs::String::widen(strMessage,strlen(strMessage)));
}
