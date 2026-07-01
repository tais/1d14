/* $Id: sgp.c,v 1.4 2004/03/19 06:16:04 digicrab Exp $ */
//its test what doeas it do?
#include "builddefines.h"
#include "types.h"
#include <windows.h>
// SDL3 owns the window + event pump now. <SDL3/SDL_main.h> remaps our
// main() to SDL_main and supplies the real Win32 WinMain, so the exe stays
// /subsystem:windows. Include both in exactly this one TU (the one defining main).
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <string.h>
#include "sgp.h"
#include "vobject.h"
#include "Font.h"
#include "local.h"
#include "FileMan.h"
#include "input.h"
#include "sdl_input.h"
#include "random.h"
#include "gameloop.h"
#include "soundman.h"
#include "JA2 Splash.h"
#include "Timer Control.h"
#include "Utilities.h"
#include "GameSettings.h"
#include "zmouse.h"
#include <vfs/Aspects/vfs_settings.h>
#include <vfs/Core/vfs.h>
#include <vfs/Core/vfs_init.h>
#include <vfs/Tools/vfs_log.h>
#include <vfs/Tools/vfs_file_logger.h>
#include "sgp_logger.h"
#include "Text.h"
#include "ExportStrings.h"
#include "ImportStrings.h"
#include <excpt.h>
#include "INIReader.h"
#include "connect.h"
#include "Intro.h"
#include <Music Control.h>
#include <language.hpp>


#define USE_CONSOLE 0

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif


static void MAGIC(std::string const& aarrrrgggh = "")
{}

static bool			s_VfsIsInitialized = false;
static std::list<vfs::Path> vfs_config_ini;

static bool			s_DebugKeyboardInput = false;
static vfs::Path	s_CodePage;

static vfs::FileLogger *vfslog = NULL;

int		iWindowedMode;

static void SHOWEXCEPTION(sgp::Exception& ex)
{
	try {
		_ExceptionMessage(ex);
	}
	catch(sgp::Exception &ex2) {
		SGP_ERROR(ex2.what());
		exit(0);
	}
}

static void SHOWEXCEPTION(vfs::Exception& ex)
{
	try {
		_ExceptionMessage(ex);
	}
	catch(vfs::Exception &ex2) {
		SGP_ERROR(ex2.what());
		exit(0);
	}
}

#define HANDLE_FATAL_ERROR \
	catch(sgp::Exception &ex){ \
		SGP_ERROR(ex.what()); \
		FatalError((const STR8)ex.what()); \
		exit(0); } \
	catch(vfs::Exception &ex){ \
		SGP_ERROR(ex.what()); \
		FatalError((const STR8)ex.getExceptionString().utf8().c_str()); \
		exit(0); } \
	catch(std::exception &ex){ \
		SGP_ERROR(ex.what()); \
		FatalError((const STR8)ex.what()); \
		exit(0); } \
	catch(const char* msg){ \
		SGP_ERROR(msg); \
		FatalError((const STR8)msg); \
		exit(0); } \
	catch(...){ \
		SGP_ERROR("Caught undefined exception"); \
		FatalError("Caught undefined exception"); \
		exit(0); }


extern UINT32		MemDebugCounter;
	extern BOOLEAN	gfPauseDueToPlayerGamePause;
	extern int		iScreenMode;
	extern BOOL		bScreenModeCmdLine;

extern	BOOLEAN		CheckIfGameCdromIsInCDromDrive();
extern	void		QueueEvent(UINT16 ubInputEvent, UINT32 usParam, UINT32 uiParam);

// Prototype Declarations
BOOLEAN				InitializeStandardGamingPlatform(HINSTANCE hInstance, int sCommandShow);
void				ShutdownStandardGamingPlatform(void);
void				GetRuntimeSettings( );

void				SafeSGPExit(void);
static bool			CallGameLoop(bool wait);
static CRITICAL_SECTION gcsGameLoop;



// The real body of the entry point. main() wraps this in Win32 SEH.
static int			HandledMain(int argc, char** argv);



static void PopulateSectionFromCommandLine(vfs::PropertyContainer &oProps, vfs::String const& sSection);

HINSTANCE			ghInstance;


	void ProcessJa2CommandLineBeforeInitialization(CHAR8 *pCommandLine);

// Global Variable Declarations
RECT				rcWindow;
POINT				ptWindowSize;

// moved from header file: 24mar98:HJH
//UINT8				gbPixelDepth;		// redefintion... look down a few lines (jonathanl)
// GLOBAL RUN-TIME SETTINGS

UINT32				guiMouseWheelMsg;			// For mouse wheel messages

BOOLEAN				gfApplicationActive;
BOOLEAN				gfProgramIsRunning;
BOOLEAN				gfGameInitialized = FALSE;
BOOLEAN				gfDontUseDDBlits	= FALSE;

// There were TWO of them??!?! -- DB
//CHAR8				gzCommandLine[ 100 ];
CHAR8				gzCommandLine[100];		// Command line given

CHAR8				gzErrorMsg[2048]="";
BOOLEAN				gfIgnoreMessages=FALSE;


// WindowProcedure + SyncWindowProcedure deleted: the Win32 WndProc message
// cases (WM_CLOSE/quit, WM_KEY*/WM_CHAR/mouse, WM_ACTIVATEAPP focus/minimize,
// WM_MOVE/WM_GETMINMAXINFO/WM_SETCURSOR geometry, WM_TIMER) are now handled by
// SDL events inside SgpHandleSDLEvent (the input seam), driven by SDL_PollEvent
// in the main loop below.

bool				s_bExportStrings		= false;
extern bool			g_bUseXML_Strings;//	= false;
bool				g_bUseXML_Structures	= false;
//bool				g_bUseXML_Tilesets		= false;

static vfs::Path	sp_force_load_jsd_xml_file;

BOOLEAN InitializeStandardGamingPlatform(HINSTANCE hInstance, int sCommandShow)
{
	FontTranslationTable *pFontTable;

	// now required by all (even JA2) in order to call ShutdownSGP
	atexit(SafeSGPExit);

	// Second, read in settings
	GetRuntimeSettings( );

	// Initialize the Debug Manager - success doesn't matter
	InitializeDebugManager();

	// Now start up everything else.
	RegisterDebugTopic(TOPIC_SGP, "Standard Gaming Platform");

	// this one needs to go ahead of all others (except Debug), for MemDebugCounter to work right...
	FastDebugMsg("Initializing Memory Manager");
	// Initialize the Memory Manager
	if (InitializeMemoryManager() == FALSE)
	{
		// We were unable to initialize the memory manager
		FastDebugMsg("FAILED : Initializing Memory Manager");
		return FALSE;
	}

	FastDebugMsg("Initializing File Manager");
	// Initialize the File Manager
	if (InitializeFileManager(NULL) == FALSE)
	{
		// We were unable to initialize the file manager
		FastDebugMsg("FAILED : Initializing File Manager");
		return FALSE;
	}

	FastDebugMsg("Initializing Input Manager");
	// Initialize the Input Manager
	if (InitializeInputManager() == FALSE)
	{
		// We were unable to initialize the input manager
		FastDebugMsg("FAILED : Initializing Input Manager");
		return FALSE;
	}

	InitializeCriticalSection(&gcsGameLoop);


	FastDebugMsg("Initializing Video Manager");
	// The video seam now creates the SDL window + renderer + texture; the
	// third (WindowProc) argument is ignored but the 3-arg signature stays.
	if (InitializeVideoManager(ghInstance, (UINT16) sCommandShow, NULL) == FALSE)
	{
		// We were unable to initialize the video manager
		FastDebugMsg("FAILED : Initializing Video Manager");
		return FALSE;
	}

	// Initialize Video Object Manager
	FastDebugMsg("Initializing Video Object Manager");
	if ( !InitializeVideoObjectManager( ) )
	{
		FastDebugMsg("FAILED : Initializing Video Object Manager");
		return FALSE;
	}

	// Initialize Video Surface Manager
	FastDebugMsg("Initializing Video Surface Manager");
	if ( !InitializeVideoSurfaceManager( ) )
	{ 
		FastDebugMsg("FAILED : Initializing Video Surface Manager");
		return FALSE;
	}

	//vfs::Path exe_dir, exe_file;
	//os::getExecutablePath(exe_dir, exe_file);

	//// set current directory to exe's directory 
	//os::setCurrectDirectory(exe_dir);

	SGP_THROW_IFFALSE( vfs_init::initVirtualFileSystem( vfs_config_ini ), L"Initializing Virtual File System failed");


	s_VfsIsInitialized = true;

	getVFS()->getVirtualLocation(vfs::Path("Temp"),true)->setIsExclusive(true);
	getVFS()->getVirtualLocation(vfs::Path("ShadeTables"),true)->setIsExclusive(true);
	getVFS()->getVirtualLocation(vfs::Path(pMessageStrings[MSG_SAVEDIRECTORY]+3),true)->setIsExclusive(true);
	getVFS()->getVirtualLocation(vfs::Path(pMessageStrings[MSG_MPSAVEDIRECTORY]+3),true)->setIsExclusive(true);

	if(!sp_force_load_jsd_xml_file.empty())
	{
		try
		{
			std::string filename = vfs::String::as_utf8(sp_force_load_jsd_xml_file());
			STRUCTURE_FILE_REF *pStructureFileRef = LoadStructureFile((STR8)filename.c_str());
		}
		catch(std::exception &ex)
		{
			SGP_RETHROW(_BS(L"failed to load and/or process file : ") << sp_force_load_jsd_xml_file << _BS::wget, ex);
		}
	}


	if(g_bUseXML_Strings)
	{
		if(s_bExportStrings)
		{
			Loc::ExportStrings();
		}
		Loc::ImportStrings();
	}

	InitJA2SplashScreen();

	// Make sure we start up our local clock (in milliseconds)
	// We don't need to check for a return value here since so far its always TRUE
	InitializeClockManager();	// must initialize after VideoManager, 'cause it uses ghWindow

	// Create font translation table (store in temp structure)
	pFontTable = CreateEnglishTransTable( );
	if ( pFontTable == NULL )
	{
		return( FALSE );
	}

	// Initialize Font Manager
	FastDebugMsg("Initializing the Font Manager");
	// Init the manager and copy the TransTable stuff into it.
	if ( !InitializeFontManager( 8, pFontTable ) )
	{
		FastDebugMsg("FAILED : Initializing Font Manager");
		return FALSE;
	}
	// Don't need this thing anymore, so get rid of it (but don't de-alloc the contents)
	MemFree( pFontTable );

	FastDebugMsg("Initializing Sound Manager");
	// Initialize the Sound Manager (DirectSound)
	if (InitializeSoundManager() == FALSE)
	{
		// We were unable to initialize the sound manager
		FastDebugMsg("FAILED : Initializing Sound Manager");
		return FALSE;
	}

	FastDebugMsg("Initializing Music");
	InitializeMusicLists();

	FastDebugMsg("Initializing Game Manager");
	// Initialize the Game
	if (InitializeGame() == FALSE)
	{
		// We were unable to initialize the game
		FastDebugMsg("FAILED : Initializing Game Manager");
		return FALSE;
	}

	// SDL delivers mouse-wheel input natively as SDL_EVENT_MOUSE_WHEEL, so
	// the legacy RegisterWindowMessage(MSH_MOUSEWHEEL) path is gone. The
	// guiMouseWheelMsg global is kept (defined above) but no longer assigned.
	gfGameInitialized = TRUE;

	return TRUE;
}

// TimerActivatedCallback + CreateStandardGamingPlatform (the WM_CREATE
// handler) deleted. The notify-thread used to drive GameLoop via
// AddTimerNotifyCallback(TimerActivatedCallback); that could run
// RefreshScreen -> SDL present off the main thread, which is not allowed.
// The main loop is now the ONLY GameLoop driver, so we just call
// InitializeJA2Clock() directly from HandledMain() (no SetTimer / notify
// callback). The Timer Control.cpp clock/notify threads still run; they
// simply have no GameLoop callback registered.


void ShutdownStandardGamingPlatform(void)
{

	//
	// Shut down the different components of the SGP
	//

	ClearTimerNotifyCallbacks();

	// TEST
	SoundServiceStreams();

	if (gfGameInitialized)
	{
		ShutdownGame();
	}

	ShutdownButtonSystem();
	MSYS_Shutdown();

	ShutdownSoundManager();

	DestroyEnglishTransTable( );	// has to go before ShutdownFontManager()
	ShutdownFontManager();

	ShutdownClockManager();	// must shutdown before VideoManager, 'cause it uses ghWindow

#ifdef SGP_VIDEO_DEBUGGING
	PerformVideoInfoDumpIntoFile( "SGPVideoShutdownDump.txt", FALSE );
#endif

	ShutdownVideoSurfaceManager();
	ShutdownVideoObjectManager();
	ShutdownVideoManager();

	ShutdownInputManager();
	ShutdownFileManager();

#ifdef EXTREME_MEMORY_DEBUGGING
	DumpMemoryInfoIntoFile( "ExtremeMemoryDump.txt", FALSE );
#endif

	ShutdownMemoryManager();	// must go last (except for Debug), for MemDebugCounter to work right...

	//
	// Make sure we unregister the last remaining debug topic before shutting
	// down the debugging layer
	UnRegisterDebugTopic(TOPIC_SGP, "Standard Gaming Platform");

	DeleteCriticalSection(&gcsGameLoop);

	ShutdownDebugManager();

	sgp::Logger::instance().shutdown();
	vfs::Log::flushDeleteAll();
	if(vfslog) delete vfslog;
	vfs::CVirtualFileSystem::shutdownVFS();
	vfs::ObjectAllocator::clear();
}

#include "MPJoinScreen.h"

static vfs::String getGameID()
{
	static vfs::String _id;
	static bool has_id = false;
	if(!has_id)
	{
		CUniqueServerId::uniqueRandomString(_id);
		has_id = true;
	}
	return _id;
}

#include "debug_util.h"
#include <vfs/Aspects/vfs_logging.h>

class VfsLogAdapter : public vfs::Aspects::ILogger
{
public:
	VfsLogAdapter(sgp::Logger_ID ID, bool stacktrace = false) : _id(ID), _trace(stacktrace) {};

	virtual void Msg(const wchar_t* msg)
	{
		SGP_LOG(_id, msg);
		if(_trace)
		{
			sgp::dumpStackTrace(msg);
		}
	}
	virtual void Msg(const char* msg)
	{
		SGP_LOG(_id, msg);
		if(_trace)
		{
			sgp::dumpStackTrace(msg);
		}
	}
private:
	sgp::Logger_ID	_id;
	bool			_trace;
};

//#include <vfs/Aspects/vfs_synchronization.h>
//#include "sgp_mutex.h"
//class VfsMutex : public vfs::Aspects::IMutex
//{
//public:
//	virtual void lock(){
//		_mutex.lock();
//	}
//	virtual void unlock(){
//		_mutex.unlock();
//	}
//private:
//	sgp::Mutex _mutex;
//};
//class VfsMutexFactory : public vfs::Aspects::IMutexFactory
//{
//public:
//	virtual vfs::Aspects::IMutex* createMutex()
//	{
//		return new VfsMutex();
//	}
//};

// Make the process DPI-aware at startup. SDL3 does not set DPI awareness for
// us and we cannot embed a manifest here, so do it programmatically before any
// window exists. Without this, Windows virtualizes window/mouse coordinates on
// a scaled (HiDPI) display: GetCursorPos()+ScreenToClient() (which drives the
// game's hit-testing in gameloop.cpp) and the SDL event coordinates (which
// drive the software cursor) then disagree, so the cursor is drawn in one place
// while clicks register in another. Resolve the API dynamically so we don't
// depend on a particular Windows SDK header level.
static void MakeProcessDpiAware(void)
{
	if (HMODULE hUser32 = GetModuleHandleW(L"user32.dll"))
	{
		typedef BOOL (WINAPI *PFN_SetCtx)(HANDLE);
		if (PFN_SetCtx p = (PFN_SetCtx)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext"))
		{
			// DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4 (Win10 1703+),
			// PER_MONITOR_AWARE == (HANDLE)-3 as a fallback.
			if (p((HANDLE)-4)) return;
			if (p((HANDLE)-3)) return;
		}
	}
	if (HMODULE hShcore = LoadLibraryW(L"Shcore.dll"))
	{
		typedef HRESULT (WINAPI *PFN_SetAwareness)(int);
		if (PFN_SetAwareness p = (PFN_SetAwareness)GetProcAddress(hShcore, "SetProcessDpiAwareness"))
		{
			p(2); // PROCESS_PER_MONITOR_DPI_AWARE
			FreeLibrary(hShcore);
			return;
		}
		FreeLibrary(hShcore);
	}
	SetProcessDPIAware(); // Vista+ system-DPI fallback
}

int main(int argc, char** argv)
{
	// Must run before SDL_Init / any window creation (see MakeProcessDpiAware).
	MakeProcessDpiAware();

#ifdef _DEBUG
	// Use this one ONLY if you're having memory corruption issues that can be repeated in a short time
	// Otherwise it will just run out of memory.

	/****************************************************************************************************/
	/*                                                                                                  */
	/*               DEBUG MEMORY ALLOCATION ON THE HEAP :  uncomment when required                     */
	/*          ------------------------------------------------------------------------                */
	/*                                                                                                  */
	/*  _CrtSetDbgFlag( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF | _CRTDBG_CHECK_EVERY_1024_DF);    */
	/*  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG );                                            */
	/*                                                                                                  */
	/****************************************************************************************************/
#endif

	// SDL_main.h remapped this main() to SDL_main and provides the real Win32
	// WinMain, so there is no HINSTANCE parameter any more -- recover it from
	// the module handle for the ~1 file that extern-references ghInstance.
	ghInstance = GetModuleHandle(NULL);

	// SDL must be up before InitializeStandardGamingPlatform, because the
	// video seam's InitializeVideoManager creates the SDL window/renderer.
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
	{
		return 1;
	}

	// Keep the Win32 SEH wrapper (Windows-only, MSVC): run the real body
	// under __try/__except so RecordExceptionInfo() logs any structured
	// exception. main() itself holds no C++ objects needing unwinding, so
	// this satisfies the MSVC SEH/C++ mixing rule (C2712).
	int Result = -1;
#ifdef ENABLE_EXCEPTION_HANDLING
	__try
	{
		Result = HandledMain(argc, argv);
	}
	__except( RecordExceptionInfo( GetExceptionInformation() ))
	{
		// Do nothing here - RecordExceptionInfo() has already done
		// everything that is needed. Actually this code won't even
		// get called unless you return EXCEPTION_EXECUTE_HANDLER from
		// the __except clause.
	}
#else
	Result = HandledMain(argc, argv);
#endif

	SDL_Quit();
	return Result;
}

//Do not place code in between main and HandledMain



static int HandledMain(int argc, char** argv)
{
	// The command line is reconstructed from GetCommandLineA() below, so
	// argc/argv are unused here (main() only needs them for SDL_main).
	(void)argc;
	(void)argv;

	HWND			hPrevInstanceWindow;

	vfs::Log::setSharedString( getGameID() );
	//if(!vfs::Aspects::getMutexFactory())
	//{
	//	vfs::Aspects::setMutexFactory( new VfsMutexFactory() );
	//}
	sgp::Logger_ID VFS_LOG = sgp::Logger::instance().createLogger();

	sgp::Logger::instance().connectFile(VFS_LOG, L"vfs.log", false, sgp::Logger::FLUSH_ON_DELETE);

	VfsLogAdapter* vfslog = new VfsLogAdapter(VFS_LOG, false);
	VfsLogAdapter* vfslog_error = new VfsLogAdapter(VFS_LOG, true);

	vfs::Aspects::setLogger(vfslog, vfslog, vfslog_error, NULL /* vfslog */);

	// Make sure that only one instance of this application is running at once
	// // Look for prev instance by searching for the window
	hPrevInstanceWindow = FindWindowEx( NULL, NULL, APPLICATION_NAME, APPLICATION_NAME );

	// One is found, bring it up!
	if ( hPrevInstanceWindow != NULL )
	{
		SetForegroundWindow( hPrevInstanceWindow );
		ShowWindow( hPrevInstanceWindow, SW_RESTORE );
		return( 0 );
	}

	FastDebugMsg("Initializing Random");
	// Initialize random number generator
	InitializeRandom(); // no Shutdown

	//rain
	//NSLoadSettings();
	//NSSaveSettings();
	//InitResolution();

	//EmergencyExitButtonInit();
	//end rain

#ifdef _DEBUG
	// Use this one ONLY if you're having memory corruption issues that can be repeated in a short time
	// Otherwise it will just run out of memory.
	//_CrtSetDbgFlag( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_DELAY_FREE_MEM_DF | _CRTDBG_LEAK_CHECK_DF | _CRTDBG_CHECK_ALWAYS_DF);

	/****************************************************************************************************/
	/*                                                                                                  */
	/*               DEBUG MEMORY ALLOCATION ON THE HEAP :  uncomment when required                     */
	/*          ------------------------------------------------------------------------                */
	/*                                                                                                  */
	/*.._CrtSetDbgFlag( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF | _CRTDBG_CHECK_EVERY_1024_DF);    */
	/*                                                                                                  */
	/****************************************************************************************************/

#endif

	// ghInstance was already set in main(); keep it here for link-compat too.
	ghInstance = GetModuleHandle(NULL);

	// Reconstruct the args-only command line for the legacy helpers.
	// GetCommandLineA() returns the whole line WITH the module path as the
	// first token; strip it so gzCommandLine and ProcessJa2CommandLine... see
	// exactly what the old WinMain's pCommandLine (arguments only) contained.
	// PopulateSectionFromCommandLine still parses GetCommandLineW() as before.
	LPSTR	pFullCommandLine = GetCommandLineA();
	CHAR8	*pCommandLine = pFullCommandLine;
	if (*pCommandLine == '"')
	{
		// Quoted module path: skip past the closing quote.
		pCommandLine++;
		while (*pCommandLine && *pCommandLine != '"')
			pCommandLine++;
		if (*pCommandLine == '"')
			pCommandLine++;
	}
	else
	{
		// Unquoted module path: skip to the first whitespace.
		while (*pCommandLine && *pCommandLine != ' ' && *pCommandLine != '\t')
			pCommandLine++;
	}
	// Skip the whitespace separating the module path from the first argument.
	while (*pCommandLine == ' ' || *pCommandLine == '\t')
		pCommandLine++;

	// Copy commandline!
	strncpy( gzCommandLine, pCommandLine, 100);
	gzCommandLine[99]='\0';

	//Process the command line BEFORE initialization
	ProcessJa2CommandLineBeforeInitialization( pCommandLine );

	// Handle Check for CD
	if ( !HandleJA2CDCheck( ) )
	{
		return( 0 );
	}

//	ShowCursor(FALSE);

	try
	{
		// Inititialize the SGP
		if (InitializeStandardGamingPlatform(ghInstance, SW_SHOW) == FALSE)
		{
			// We failed to initialize the SGP
			return 0;
		}
	}
	HANDLE_FATAL_ERROR;

	vfs::Log::flushReleaseAll();

#ifdef LUACONSOLE
	if (1==iScreenMode)
	{
		CreateConsole();
	}
#endif

	if( g_lang == i18n::Lang::en ) {
	try
	{
		SetIntroType( INTRO_SPLASH );
	}
	HANDLE_FATAL_ERROR;
	}

	gfApplicationActive = TRUE;
	gfProgramIsRunning = TRUE;

	FastDebugMsg("Running Game");

	// Start the SGP local clock. This used to run from the WM_CREATE handler
	// (CreateStandardGamingPlatform); with SDL owning the window we call it
	// directly. Deliberately NO AddTimerNotifyCallback / SetTimer: the main
	// loop below is the ONLY GameLoop driver, so GameLoop (and the
	// RefreshScreen -> SDL present it performs) always runs on this thread.
	InitializeJA2Clock();

	// At this point the SGP is set up, which means all I/O, Memory, tools, etc... are available. All we need to do is
	// attend to the gaming mechanics themselves
	try
	{
		MAGIC();
		while (gfProgramIsRunning)
		{
			// GameLoop's own frame limiter is commented out, so measure the
			// iteration and sleep off any slack afterwards to cap CPU usage.
			DWORD dwFrameStart = GetTickCount();

			// Pump every pending SDL event through the input seam. It owns ALL
			// event handling: key/mouse -> JA2 input queue, window focus and
			// minimize -> gfApplicationActive, and window-close / quit ->
			// returns TRUE (we then request program exit). We do NOT duplicate
			// any of that here.
			SDL_Event e;
			while (SDL_PollEvent(&e))
			{
				if (SGP_GetSDLRenderer())
					SDL_ConvertEventToRenderCoordinates(SGP_GetSDLRenderer(), &e);
				if (SgpHandleSDLEvent(&e))
					gfProgramIsRunning = FALSE;
			}

			if (gfApplicationActive && gfProgramIsRunning)
			{
				CallGameLoop(true);
			}
			else
			{
				// Minimized / unfocused: don't burn a core spinning.
				SDL_Delay(5);
			}

			// Frame pacing: target ~60 FPS. If the whole iteration took less
			// than 15 ms, sleep off the remainder so the bare loop does not
			// busy-spin at 100% CPU (GetTickCount wraps cleanly under DWORD
			// arithmetic).
			DWORD dwFrameTime = GetTickCount() - dwFrameStart;
			if (dwFrameTime < 15)
			{
				SDL_Delay(15 - dwFrameTime);
			}
		}
	}
	catch(sgp::Exception &ex)
	{
		SGP_ERROR(ex.what());
		SHOWEXCEPTION(ex);
	}
	catch(vfs::Exception &ex)
	{
		SGP_ERROR(ex.what());
		SHOWEXCEPTION(ex);
	}
	catch(std::exception &ex)
	{
		sgp::Exception nex(ex.what());
		SGP_ERROR(nex.what());
		SHOWEXCEPTION(nex);
	}
	catch(const char* msg)
	{
		sgp::Exception ex(msg);
		SGP_ERROR(ex.what());
		SHOWEXCEPTION(ex);
	}
	catch(...)
	{
		sgp::Exception ex("Caught undefined exception");
		SGP_ERROR( ex.what() );
		SHOWEXCEPTION(ex);
	}



	// This is the normal exit point
	FastDebugMsg("Exiting Game");

	// SGPExit() will be called next through the atexit() mechanism...	This way we correctly process both normal exits and
	// emergency aborts (such as those caused by a failed assertion).

	return 0;
}


void SGPExit(void)
{
	static BOOLEAN fAlreadyExiting = FALSE;
	// helps prevent heap crashes when multiple assertions occur and call us
	if ( fAlreadyExiting )
	{
		return;
	}

	fAlreadyExiting = TRUE;
	gfProgramIsRunning = FALSE;

// Wizardry only

	ShutdownStandardGamingPlatform();
//	ShowCursor(TRUE);
	if(strlen(gzErrorMsg))
	{
		MessageBox(NULL, gzErrorMsg, "Error", MB_OK | MB_ICONERROR	);
	}


}

void GetRuntimeSettings( )
{
	int		iMaximize;

	// cnc-ddraw detection retired: SDL3 owns presentation, there is no
	// DirectDraw shim to coax into fullscreen.

	vfs::PropertyContainer oProps;
	oProps.initFromIniFile(GAME_INI_FILE);
	PopulateSectionFromCommandLine(oProps, "Ja2 Settings");
	
	vfs::String loc = oProps.getStringProperty("Ja2 Settings", L"LOCALE");
	if(!loc.empty())
	{
		SGP_THROW_IFFALSE( setlocale(LC_ALL, loc.utf8().c_str()), _BS(L"invalid locale : ") << loc << _BS::wget );
	}

	iResolution = (int)oProps.getIntProperty(L"Ja2 Settings", L"SCREEN_RESOLUTION", -1);
	
	// WANNE: Always enable
	//iMaximize = (int)oProps.getIntProperty(L"Ja2 Settings", L"SCREEN_MODE_WINDOWED_MAXIMIZE", -1);
	iMaximize = 1;
	
	iWindowedMode = (int)oProps.getIntProperty(L"Ja2 Settings", L"SCREEN_MODE_WINDOWED", -1);

	vfs::Settings::setUseUnicode( !oProps.getBoolProperty(L"Ja2 Settings", L"VFS_NO_UNICODE", false) );

	std::list<vfs::String> ini_list;

	vfs::String vfs_config_file;
	if(oProps.getStringProperty(L"Ja2 Settings", L"VFS_CONFIG", vfs_config_file))
	{
		vfs::PropertyContainer temp_cont;
		temp_cont.initFromIniFile(vfs_config_file);
		vfs::String temp_str;
		if(temp_cont.getStringProperty(L"vfs_config", L"VFS_CONFIG_INI", temp_str))
		{
			oProps.setStringProperty(L"Ja2 Settings", L"VFS_CONFIG_INI", temp_str);
		}
	}
	if(oProps.getStringListProperty(L"Ja2 Settings", L"VFS_CONFIG_INI", ini_list, L""))
	{
		vfs_config_ini.clear();
		for(std::list<vfs::String>::iterator it = ini_list.begin(); it != ini_list.end(); ++it)
		{
			vfs_config_ini.push_back(*it);
		}
	}
	else
	{
		vfs_config_ini.push_back(L"vfs_config.ini");
	}
	std::list<vfs::String> merge_list;
	if(oProps.getStringListProperty(L"Ja2 Settings", L"MERGE_INI_FILES", merge_list, L""))
	{
		for(std::list<vfs::String>::iterator it = merge_list.begin(); it != merge_list.end(); ++it)
		{
			CIniReader::RegisterFileForMerging(*it);
		}
	}
	
	std::list<vfs::String> merge_list_ub;
	if(oProps.getStringListProperty(L"Ja2 Settings", L"MERGE_INI_FILES_UB", merge_list_ub, L""))
	{
		for(std::list<vfs::String>::iterator it = merge_list_ub.begin(); it != merge_list_ub.end(); ++it)
		{
			CIniReader::RegisterFileForMerging(*it);
		}
	}

	extern bool g_bUsePngItemImages;
	g_bUsePngItemImages		= oProps.getBoolProperty(L"Ja2 Settings", L"USE_PNG_ITEM_IMAGES", false);
	g_bUseXML_Structures	= oProps.getBoolProperty(L"Ja2 Settings", L"USE_XML_STRUCTURES", false);
	
	// WANNE: Always use XML tilesets (ja2Set.dat.xml), because now we have P4-P9 items integrated. The old method (ja2set.dat) will not work anymore!
	// To generate ja2Set.dat.xml, set "USE_XML_TILESETS = 1" in ja2.ini then start the game with the official (4870) ja2 1.13 executable. 
	// Yes, you have to start a game with an older executable where p4-p9 is not integrated (see: TileDat.h -> enum TileTypeDefines)
	// Once the game reaches the main menu, the ja2Set.dat.xml file will be 
	// available in the "Profiles" folder of the MOD
	//g_bUseXML_Tilesets = true;

	// WANNE: Yes, make it optional again
	//Madd: moved to ja2_options.ini instead
	//g_bUseXML_Tilesets		= oProps.getBoolProperty(L"Ja2 Settings", L"USE_XML_TILESETS", false);

	g_bUseXML_Strings		= oProps.getBoolProperty(L"Ja2 Settings", L"USE_XML_STRINGS", false);
	s_bExportStrings		= oProps.getBoolProperty(L"Ja2 Settings", L"EXPORT_STRINGS", false);

	sp_force_load_jsd_xml_file = oProps.getStringProperty(L"Ja2 Settings", L"FORCE_LOAD_JSD_XML_FILE", L"");

#ifdef JA2EDITOR
	iResolution = (int)oProps.getIntProperty("Ja2 Settings","EDITOR_SCREEN_RESOLUTION", -1); 
#endif

	int	iResX;
	int iResY;

	switch (iResolution)
	{
		case _640x480:
			iResX = 640;
			iResY = 480;
			break;
		case _960x540:
			iResX = 960;
			iResY = 540;
			break;
		case _800x600:
			iResX = 800;
			iResY = 600;
			break;
		case _1024x600:
			iResX = 1024;
			iResY = 600;
			break;
		case _1280x720:
			iResX = 1280;
			iResY = 720;
			break;
		case _1024x768:
			iResX = 1024;
			iResY = 768;
			break;
		case _1280x768:
			iResX = 1280;
			iResY = 768;
			break;
		case _1360x768:
			iResX = 1360;
			iResY = 768;
			break;
		case _1366x768:
			iResX = 1366;
			iResY = 768;
			break;
		case _1280x800:
			iResX = 1280;
			iResY = 800;
			break;
		case _1440x900:
			iResX = 1440;
			iResY = 900;
			break;
		case _1600x900:
			iResX = 1600;
			iResY = 900;
			break;
		case _1280x960:
			iResX = 1280;
			iResY = 960;
			break;
		case _1440x960:
			iResX = 1440;
			iResY = 960;
			break;
		case _1770x1000:
			iResX = 1770;
			iResY = 1000;
			break;
		case _1280x1024:
			iResX = 1280;
			iResY = 1024;
			break;
		case _1360x1024:
			iResX = 1360;
			iResY = 1024;
			break;
		case _1600x1024:
			iResX = 1600;
			iResY = 1024;
			break;
		case _1440x1050:
			iResX = 1440;
			iResY = 1050;
			break;
		case _1680x1050:
			iResX = 1680;
			iResY = 1050;
			break;
		case _1920x1080:
			iResX = 1920;
			iResY = 1080;
			break;
		case _1600x1200:
			iResX = 1600;
			iResY = 1200;
			break;
		case _1920x1200:
			iResX = 1920;
			iResY = 1200;
			break;
		case _2560x1440:
			iResX = 2560;
			iResY = 1440;
			break;
		case _2560x1600:
			iResX = 2560;
			iResY = 1600;
			break;
		case _CustomRes:
			iResX = max( (int)oProps.getIntProperty(L"Ja2 Settings", L"CUSTOM_SCREEN_RESOLUTION_X", -1), 640 );
			iResY = max( (int)oProps.getIntProperty(L"Ja2 Settings", L"CUSTOM_SCREEN_RESOLUTION_Y", -1), 480 );

			if (iResX < 800 || iResY < 600)
				iResolution = _640x480;
			else if (iResX < 1024 || iResY < 768)
				iResolution = _800x600;
			else
				iResolution = _1024x768;

			break;
		default:	// 800x600
			iResolution = _800x600;
			iResX = 800;
			iResY = 600;
			break;
	}

	if (iWindowedMode == 1 && iMaximize == 1)
	{
		if ((iResX - 16) >= 1024)
			iResX = iResX - 16;

		if ((iResY - 70) >= 768)
			iResY = iResY - 70;
	}


	SCREEN_WIDTH = iResX;
	SCREEN_HEIGHT = iResY;

	iScreenWidthOffset = (SCREEN_WIDTH - 640) / 2;
	iScreenHeightOffset = (SCREEN_HEIGHT - 480) / 2;

	if (iResolution >= _640x480 && iResolution < _800x600)
	{
		xResOffset = ((SCREEN_WIDTH - 640) / 2);
		yResOffset = ((SCREEN_HEIGHT - 480) / 2);	
	}
	else if (iResolution < _1024x768)
	{
		xResOffset = ((SCREEN_WIDTH - 800) / 2);
		yResOffset = ((SCREEN_HEIGHT - 600) / 2);
	}
	else
	{
		xResOffset = ((SCREEN_WIDTH - 1024) / 2);
		yResOffset = ((SCREEN_HEIGHT - 768) / 2);
	}

	xResSize = (SCREEN_WIDTH - 2 * xResOffset);		// one of the following: 1024 or 800 or 640
	yResSize = (SCREEN_HEIGHT - 2 * yResOffset);	// one of the follownig: 768 or 600 or 480

	/* Sergeant_Kolja. 2007-02-20: runtime Windowed mode instead of compile-time */
	/* 1 for Windowed, 0 for Fullscreen */
	if( !bScreenModeCmdLine )
	{
		iScreenMode = (int)oProps.getIntProperty("Ja2 Settings","SCREEN_MODE_WINDOWED", iScreenMode);
	}

	// WANNE: Should we play the intro?
	iPlayIntro = (int)oProps.getIntProperty("Ja2 Settings","PLAY_INTRO", iPlayIntro);

    iUseWinFonts= (int)oProps.getIntProperty("Ja2 Settings","USE_WINFONTS", iUseWinFonts);
	fTooltipScaleFactor = ((float)oProps.getFloatProperty("Ja2 Settings", "TOOLTIP_SCALE_FACTOR", 100)) / 100;
	if (fTooltipScaleFactor < 1) fTooltipScaleFactor = 1;

	// haydent: mouse scrolling
	iDisableMouseScrolling = (int)oProps.getIntProperty("Ja2 Settings","DISABLE_MOUSE_SCROLLING", iDisableMouseScrolling);


	// WANNE: Highspeed Timer always ON (no more optional in the ja2.ini)
	// get timer/clock initialization state
	//SetHiSpeedClockMode( oProps.getBoolProperty("Ja2 Settings", "HIGHSPEED_TIMER", false) ? TRUE : FALSE );	
	SetHiSpeedClockMode( TRUE );
}


void SafeSGPExit(void)
{
	// SGPExit tends to use resources that are already uninitialized so handle 
	__try
	{
		SGPExit();
	}
	__except( EXCEPTION_EXECUTE_HANDLER )
	{
		// The application is in exit and best effort to clean up 
		//  has failed so just ignore and continue silently
	}
}


void ShutdownWithErrorBox(CHAR8 *pcMessage)
{
	strncpy(gzErrorMsg, pcMessage, 255);
	gzErrorMsg[255]='\0';
	gfIgnoreMessages=TRUE;

	exit(0);
}




void ProcessJa2CommandLineBeforeInitialization(CHAR8 *pCommandLine)
{
	CHAR8 cSeparators[]="\t =";
	CHAR8	*pCopy=NULL, *pToken;

	pCopy=(CHAR8 *)MemAlloc(strlen(pCommandLine) + 1);

	Assert(pCopy);
	if(!pCopy)
		return;

	memcpy(pCopy, pCommandLine, strlen(pCommandLine)+1);

	pToken=strtok(pCopy, cSeparators);
	while(pToken)
	{
		//if its the NO SOUND option
		if(!_strnicmp(pToken, "/NOSOUND", 8))
		{
			//disable the sound
			SoundEnableSound(FALSE);
		}
		else if(!_strnicmp(pToken, "/FULLSCREEN", 11))
		{
			//overwrite Graphic setting from JA2_settings.ini
			iScreenMode=0; /* 1 for Windowed, 0 for Fullscreen */
			bScreenModeCmdLine = TRUE; /* if set TRUE, INI is no longer evaluated */
			/* no resolution read from Args. Still from INI, but could be added here, too...*/
		}
		else if(!_strnicmp(pToken, "/WINDOW", 7))
		{
			//overwrite Graphic setting from JA2_settings.ini
			iScreenMode=1; /* 1 for Windowed, 0 for Fullscreen */
			bScreenModeCmdLine = TRUE; /* if set TRUE, INI is no longer evaluated */
			/* no resolution read from Args. Still from INI, but could be added here, too...*/
		}

		//get the next token
		pToken=strtok(NULL, cSeparators);
	}

	MemFree(pCopy);
}

static void PopulateSectionFromCommandLine(vfs::PropertyContainer &oProps, vfs::String const& sSection)
{
	const wchar_t* lpCommandLine = GetCommandLineW();
	int argc = 0, nchars = 0;
	ParseCommandLine( lpCommandLine, NULL, NULL, &argc, &nchars);
	wchar_t **argv = (wchar_t **)_alloca(argc * sizeof(wchar_t *) + nchars * sizeof(wchar_t));
	ParseCommandLine( lpCommandLine, argv, (wchar_t *)(((char*)argv) + argc * sizeof(wchar_t*)), &argc, &nchars);

	for (int i = 1; i < argc; i++)
	{
		wchar_t *arg = argv[i];
		if (arg == NULL)
			continue;
		if (arg[0] == L'-' || arg[0] == L'/')
		{
			wchar_t *pkey = arg+1;
			wchar_t *psep = wcspbrk(arg, L":=");
			wchar_t *param = (psep ? psep+1 : NULL);
			if (psep) *psep = 0;
			if ( (param == NULL || param[0] == 0) && ( i+1<argc && argv[i+1] && ( argv[i+1][0] != L'-' && argv[i+1][0] != L'/' ) ) )//dnl ch79 291113
			{
				param = argv[++i];
				argv[i] = NULL;
			}
			if (param != NULL)
			{
				oProps.setStringProperty(sSection, pkey, param);
			}
		}
	}
}

static LONG __stdcall SGPExceptionFilter(int exceptionCount, EXCEPTION_POINTERS* pExceptInfo)
{
#ifdef ENABLE_EXCEPTION_HANDLING
	extern BOOL ERGetFirstModuleException(EXCEPTION_POINTERS*, HMODULE, LPSTR, INT, LPSTR, INT, INT *);
	extern STR GetExceptionString( DWORD uiExceptionCode );
	CHAR funcName[64], sourceName[MAX_PATH];
	INT lineNum = 0;
	if (exceptionCount >= 1)
	{
		bool showAssert = true;
		__try{
			// the exception handler writer can fail with exceptions too
			RecordExceptionInfo(pExceptInfo);

			LPCSTR exceptMsg = GetExceptionString(pExceptInfo->ExceptionRecord->ExceptionCode);
			if ( ERGetFirstModuleException(pExceptInfo, NULL, funcName, _countof(funcName), sourceName, _countof(sourceName), &lineNum ) )
			{
				_FailMessage(exceptMsg, lineNum, funcName, sourceName);
				showAssert = false;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
		if (showAssert) AssertMsg(FALSE, "Unhanded exception processing GameLoop unable to recover.");
	}

#endif

	return EXCEPTION_EXECUTE_HANDLER;
}

static void SGPGameLoop()
{
	try
	{
		GameLoop();
	}
	catch(sgp::Exception &ex)
	{
		SGP_ERROR(ex.what());
		SHOWEXCEPTION(ex);
	}
	catch(vfs::Exception &ex)
	{
		SGP_ERROR(ex.what());
		SHOWEXCEPTION(ex);
	}
	catch(std::exception &ex)
	{
		sgp::Exception nex(ex.what());
		SGP_ERROR(nex.what());
		SHOWEXCEPTION(nex);
	}
	catch(const char* msg)
	{
		sgp::Exception ex(msg);
		SGP_ERROR(ex.what());
		SHOWEXCEPTION(ex);
	}
}

static bool CallGameLoop(bool wait)
{
	static int numUnsuccessfulTries = 0;
	if (wait)
	{
		EnterCriticalSection(&gcsGameLoop);
	}
	else
	{
		if ( !TryEnterCriticalSection(&gcsGameLoop) )
			return false;
	}

	__try
	{
		__try
		{
			SGPGameLoop();
			numUnsuccessfulTries = 0;
		}
		__except( SGPExceptionFilter(++numUnsuccessfulTries, GetExceptionInformation()) )
		{
		}
	}
	__finally
	{
		LeaveCriticalSection(&gcsGameLoop);
	}

	// Give it several attempts to recover from random exceptions and to display error screen
	if (numUnsuccessfulTries > 5)
		ShutdownWithErrorBox("Unhandled exception. Unable to recover.");

	return true;
}

