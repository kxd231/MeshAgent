/*
Copyright 2006 - 2022 Intel Corporation

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

	http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#if defined(_LINKVM)
#pragma warning(disable : 4996)

#include <stdio.h>
#include "kvm.h"
#include "tile.h"
#include <signal.h>
#include "input.h"
#include <Winuser.h>

#include "meshcore/meshdefines.h"
#include "microstack/ILibParsers.h"
#include "microstack/ILibAsyncSocket.h"
#include "microstack/ILibProcessPipe.h"
#include "microstack/ILibRemoteLogging.h"
#include <sas.h>
#include <stdarg.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#include "meshcore/KVM/common/kvm_stdin.h" // the framing of the stdin (every build)
#ifdef KVM_STREAM_ENABLED
#include "meshcore/KVM/common/kvm_stream.h"
#endif
#include "kvm_video.h" // the H.264 video path (stubs where it is not built)
#include "meshcore/KVM/common/kvm_pace.h" // the schedule of the loop (every build)

#if defined(WIN32) && !defined(_WIN32_WCE) && !defined(_MINCORE)
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif

extern void KVM_WriteLog(ILibKVM_WriteHandler writeHandler, void *user, char *format, ...);

// #define KVMDEBUGENABLED 1
ILibProcessPipe_SpawnTypes gProcessSpawnType = ILibProcessPipe_SpawnTypes_USER;
int gProcessTSID = -1;
extern int gRemoteMouseRenderDefault;
int gRemoteMouseMoved = 0;
extern int gCurrentCursor;

#pragma pack(push, 1)
typedef struct KVMDebugLog
{
	unsigned short length;
	unsigned short logType;
	unsigned short logFlags;
	char logData[];
} KVMDebugLog;
#pragma pack(pop)

#ifdef KVMDEBUGENABLED
void KvmCriticalLog(const char *msg, const char *file, int line, int user1, int user2)
{
	int len;
	HANDLE h;
	int DontDestroy = 0;
	h = OpenMutex(MUTEX_ALL_ACCESS, FALSE, TEXT("MeshAgentKvmLogLock"));
	if (h == NULL)
	{
		if (GetLastError() != ERROR_FILE_NOT_FOUND)
			return;
		if ((h = CreateMutex(NULL, TRUE, TEXT("MeshAgentKvmLogLock"))) == NULL)
			return;
		DontDestroy = 1;
	}
	else
	{
		WaitForSingleObject(h, INFINITE);
	}
	len = sprintf_s(ILibScratchPad, sizeof(ILibScratchPad), "\r\n%s:%d (%d,%d) %s", file, line, user1, user2, msg);
	if (len > 0 && len < (int)sizeof(ILibScratchPad))
		ILibAppendStringToDiskEx("C:\\Temp\\MeshAgentKvm.log", ILibScratchPad, len);
	ReleaseMutex(h);
	if (DontDestroy == 0)
		CloseHandle(h);
}
#define KVMDEBUG(m, u)                                                     \
	{                                                                      \
		KvmCriticalLog(m, __FILE__, __LINE__, u, GetLastError());          \
		printf("KVMMSG: %s (%d,%d).\r\n", m, (int)u, (int)GetLastError()); \
	}
#define KVMDEBUG2 ILIBLOGMESSAGEX
#else
#define KVMDEBUG(m, u)
#define KVMDEBUG2(...)
#endif

int TILE_WIDTH = 0;
int TILE_HEIGHT = 0;
int SCREEN_COUNT = -1;		// Total number of displays
int SCREEN_SEL = 0;			// Currently selected display (0 = all)
int SCREEN_SEL_TARGET = 0;	// Desired selected display (0 = all)
int SCREEN_SEL_PROCESS = 0; // In process of changing displays (0 = all)
int SCREEN_X = 0;			// Left most of current screen
int SCREEN_Y = 0;			// Top most of current screen
int SCREEN_WIDTH = 0;		// Width of current screen
int SCREEN_HEIGHT = 0;		// Height of current screen
int VSCREEN_X = 0;			// Left most of virtual screen
int VSCREEN_Y = 0;			// Top most of virtual screen
int VSCREEN_WIDTH = 0;		// Width of virtual screen
int VSCREEN_HEIGHT = 0;		// Height of virtual screen
int SCALED_WIDTH = 0;
int SCALED_HEIGHT = 0;
int PIXEL_SIZE = 0;
int TILE_WIDTH_COUNT = 0;
int TILE_HEIGHT_COUNT = 0;
int COMPRESSION_RATIO = 0;
int SCALING_FACTOR = 1024;	   // Scaling factor, 1024 = 100%
int SCALING_FACTOR_NEW = 1024; // Desired scaling factor, 1024 = 100%
int FRAME_RATE_TIMER = 0;
static kvm_tick gLoopTick;	// the schedule of the main loop (kvm_pace.h)
HANDLE kvmthread = NULL;
int g_shutdown = 999;
int g_pause = 0;
int g_remotepause = 1;
int g_restartcount = 0;
struct tileInfo_t **tileInfo = NULL;
int g_slavekvm = 0;
static ILibProcessPipe_Process gChildProcess;
#ifdef KVM_STREAM_ENABLED
// the end-to-end ACK (commands 240-243). The glue between kvm_stream.c and the pipe is further down (search "end-to-end ACK (commands 240-243)").
static volatile LONG gStreamOn = 0;										// this process runs a KVM session with the stream layer: every write is counted
static ILibTransport_DoneState kvm_child_write(char *buffer, int bufferLen, void *unused);
static void kvm_stream_input(const char *block, int type);			// a 240 or 241 from the input thread, queued for the main loop
static void kvm_stream_input_bad(void);
static void kvm_stream_stats_flush(int final);
static kvm_veto_tracker gRelayVeto;									// parent process: the veto sources (kvm_stream.h KVM_VETO_*) set by op 5 / op 6 on their way to the child
#endif
int kvm_relay_restart(int paused, void *pipeMgr, char *exePath, ILibKVM_WriteHandler writeHandler, void *reserved);

HANDLE hStdOut = INVALID_HANDLE_VALUE;
HANDLE hStdIn = INVALID_HANDLE_VALUE;
int ThreadRunning = 0;
int gGeometryGeneration = 0; // Incremented by kvm_server_SetResolution(), frames captured with an older geometry are dropped.
// Set when the next captured frame has to be complete (KVM_FRAME_FULL: every tile goes through the CRC check): a refresh request,
// a new geometry, the start of the session, or a frame that was not fully processed. Read and cleared by kvm_capture_start().
volatile LONG gKvmFullFrame = 1;
int kvmConsoleMode = 0;

ILibQueue gPendingPackets = NULL;

ILibRemoteLogging gKVMRemoteLogging = NULL;
#ifdef _WINSERVICE
void kvm_slave_OnRawForwardLog(ILibRemoteLogging sender, ILibRemoteLogging_Modules module, ILibRemoteLogging_Flags flags, char *buffer, int bufferLen)
{
	if (flags <= ILibRemoteLogging_Flags_VerbosityLevel_1)
	{
		KVMDebugLog *log = (KVMDebugLog *)buffer;
		log->length = bufferLen + 1;
		log->logType = (unsigned short)module;
		log->logFlags = (unsigned short)flags;
		buffer[bufferLen] = 0;

		WriteFile(GetStdHandle(STD_ERROR_HANDLE), buffer, log->length, &bufferLen, NULL);
	}
}
#endif

void kvm_setupSasPermissions()
{
	DWORD dw = 3;
	HKEY key = NULL;

	KVMDEBUG("kvm_setupSasPermissions", 0);

	// SoftwareSASGeneration
	if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", 0, KEY_ALL_ACCESS, &key) == ERROR_SUCCESS)
	{
		RegSetValueEx(key, "SoftwareSASGeneration", 0, REG_DWORD, (BYTE *)&dw, 4);
		RegCloseKey(key);
	}
}

// Emulate the CTRL-ALT-DEL (Should work on WinXP, not on Vista & Win7)
DWORD WINAPI kvm_ctrlaltdel(LPVOID Param)
{
	UNREFERENCED_PARAMETER(Param);
	KVMDEBUG("kvm_ctrlaltdel", (int)(uintptr_t)Param);
	typedef VOID(WINAPI * SendSas)(BOOL asUser);
	SendSas sas;

	// Perform new method (Vista & Win7)
	HMODULE m = LoadLibraryExA("sas.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

	// We need to dynamically load this, becuase it doesn't exist on Windows Core.
	// However, LOAD_LIBRARY_SEARCH_SYSTEM32 does not exist on Windows 7 SP1 / Windows Server 2008 R2 without a MSFT Patch,
	// but this patch is no longer available from MSFT, so this fallback case will only affect insecure versions of Windows 7 SP1 / Server 2008 R2
	if (m == NULL && GetLastError() == ERROR_INVALID_PARAMETER)
	{
		m = LoadLibraryA("sas.dll");
	}
	if (m != NULL)
	{
		sas = (SendSas)GetProcAddress(m, "SendSAS");
		if (sas != NULL)
		{
			kvm_setupSasPermissions();
			sas(FALSE);
		}
		FreeLibrary(m);
	}
	return 0;
}

BOOL CALLBACK DisplayInfoEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData)
{
	int w, h, deviceid = 0;
	MONITORINFOEX mi;
	DWORD *selection = (DWORD *)dwData;
	UNREFERENCED_PARAMETER(hdcMonitor);
	UNREFERENCED_PARAMETER(lprcMonitor);

	ZeroMemory(&mi, sizeof(mi));
	mi.cbSize = sizeof(mi);

	// Get the display information
	if (!GetMonitorInfo(hMonitor, (LPMONITORINFO)&mi))
		return TRUE;
	if (sscanf_s(mi.szDevice, "\\\\.\\DISPLAY%d", &deviceid) != 1)
		return TRUE;
	if (--selection[0] > 0)
	{
		return TRUE;
	}

	// See if anything changed
	w = abs(mi.rcMonitor.left - mi.rcMonitor.right);
	h = abs(mi.rcMonitor.top - mi.rcMonitor.bottom);
	if (SCREEN_X != mi.rcMonitor.left || SCREEN_Y != mi.rcMonitor.top || SCREEN_WIDTH != w || SCREEN_HEIGHT != h || SCALING_FACTOR != SCALING_FACTOR_NEW)
	{
		SCREEN_X = mi.rcMonitor.left;
		SCREEN_Y = mi.rcMonitor.top;
		SCREEN_WIDTH = w;
		SCREEN_HEIGHT = h;
		SCREEN_SEL_PROCESS |= 1; // Force the new resolution to be sent to the client.
	}

	if (SCREEN_SEL != SCREEN_SEL_TARGET)
	{
		SCREEN_SEL = SCREEN_SEL_TARGET;
		SCREEN_SEL_PROCESS |= 2; // Force the display list to be sent to the client, includes the new display selection
	}

	return TRUE;
}

BOOL CALLBACK DisplayInfoEnumProc_Info(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData)
{
	unsigned short *buffer = (unsigned short *)dwData;
	MONITORINFOEX mi;
	UNREFERENCED_PARAMETER(hdcMonitor);
	UNREFERENCED_PARAMETER(lprcMonitor);

	ZeroMemory(&mi, sizeof(mi));
	mi.cbSize = sizeof(mi);

	// Get the display information
	if (!GetMonitorInfo(hMonitor, (LPMONITORINFO)&mi))
		return TRUE;
	int w = abs(mi.rcMonitor.left - mi.rcMonitor.right);
	int h = abs(mi.rcMonitor.top - mi.rcMonitor.bottom);

	int i = (int)(buffer[0]++);
	int offset = (5 * i) + 4;

	if ((((i + 1) * 10) + 4) > buffer[1])
	{
		return (FALSE);
	}

	buffer[offset] = (unsigned short)htons((unsigned short)(i + 1));				 // ID
	buffer[offset + 1] = (unsigned short)htons((unsigned short)(mi.rcMonitor.left)); // X
	buffer[offset + 2] = (unsigned short)htons((unsigned short)(mi.rcMonitor.top));	 // Y
	buffer[offset + 3] = (unsigned short)htons((unsigned short)(w));				 // WIDTH
	buffer[offset + 4] = (unsigned short)htons((unsigned short)(h));				 // HEIGHT
	return (TRUE);
}
void kvm_send_display_list(ILibKVM_WriteHandler writeHandler, void *reserved)
{
	int i;
	char dwData[4096];
	((unsigned short *)dwData)[0] = (unsigned short)0;
	((unsigned short *)dwData)[1] = (unsigned short)sizeof(dwData);
	((unsigned short *)dwData)[2] = (unsigned short)htons((unsigned short)MNG_KVM_DISPLAY_INFO); // Write the type
	if (EnumDisplayMonitors(NULL, NULL, DisplayInfoEnumProc_Info, (LPARAM)dwData))
	{
		((unsigned short *)dwData)[3] = (unsigned short)htons((((unsigned short *)dwData)[0]) * 10 + 4); // Length
		writeHandler(dwData + 4, (((unsigned short *)dwData)[0]) * 10 + 4, reserved);
	}

	// Not looked at the number of screens yet
	if (SCREEN_COUNT == -1)
		return;
	char *buffer = ILibMemory_AllocateA((5 + SCREEN_COUNT) * 2);
	memset(buffer, 0xFF, ILibMemory_AllocateA_Size(buffer));
	// Send the list of possible displays to remote
	if (SCREEN_COUNT == 0 || SCREEN_COUNT == 1)
	{
		// Only one display, send empty
		((unsigned short *)buffer)[0] = (unsigned short)htons((unsigned short)MNG_KVM_GET_DISPLAYS); // Write the type
		((unsigned short *)buffer)[1] = (unsigned short)htons((unsigned short)(8));					 // Write the size
		((unsigned short *)buffer)[2] = (unsigned short)htons((unsigned short)(0));					 // Screen Count
		((unsigned short *)buffer)[3] = (unsigned short)htons((unsigned short)(0));					 // Selected Screen

		writeHandler(buffer, 8, reserved);
	}
	else
	{
		// Many displays
		((unsigned short *)buffer)[0] = (unsigned short)htons((unsigned short)MNG_KVM_GET_DISPLAYS);	  // Write the type
		((unsigned short *)buffer)[1] = (unsigned short)htons((unsigned short)(10 + (2 * SCREEN_COUNT))); // Write the size
		((unsigned short *)buffer)[2] = (unsigned short)htons((unsigned short)(SCREEN_COUNT + 1));		  // Screen Count
		((unsigned short *)buffer)[3] = (unsigned short)htons((unsigned short)(-1));					  // Possible Screen (ALL)
		for (i = 0; i < SCREEN_COUNT; i++)
		{
			((unsigned short *)buffer)[4 + i] = (unsigned short)htons((unsigned short)(i + 1)); // Possible Screen
		}
		if (SCREEN_SEL == 0)
		{
			((unsigned short *)buffer)[4 + i] = (unsigned short)htons((unsigned short)(-1)); // Selected Screen (All)
		}
		else
		{
			((unsigned short *)buffer)[4 + i] = (unsigned short)htons((unsigned short)(SCREEN_SEL)); // Selected Screen
		}

		writeHandler(buffer, (10 + (2 * SCREEN_COUNT)), reserved);
	}
}

int kvm_server_currentDesktopname = 0;
void CheckDesktopSwitch(int checkres, ILibKVM_WriteHandler writeHandler, void *reserved)
{
	int x, y, w, h;
	HDESK desktop;
	HDESK desktop2;
	char name[64];

	// KVMDEBUG("CheckDesktopSwitch", checkres);

	// Check desktop switch
	if ((desktop2 = GetThreadDesktop(GetCurrentThreadId())) == NULL)
	{
		KVMDEBUG("GetThreadDesktop Error", 0);
	} // CloseDesktop() is not needed
	if ((desktop = OpenInputDesktop(0, TRUE,
									DESKTOP_CREATEMENU |
										DESKTOP_CREATEWINDOW |
										DESKTOP_ENUMERATE |
										DESKTOP_HOOKCONTROL |
										DESKTOP_WRITEOBJECTS |
										DESKTOP_READOBJECTS |
										DESKTOP_SWITCHDESKTOP |
										GENERIC_WRITE)) == NULL)
	{
		KVMDEBUG("OpenInputDesktop Error", 0);
	}

	if (SetThreadDesktop(desktop) == 0)
	{
		if (CloseDesktop(desktop) == 0)
		{
			KVMDEBUG("CloseDesktop1 Error", 0);
		}
		desktop = desktop2;
	}
	else
	{
		CloseDesktop(desktop2);
	}

	// Check desktop name switch
	if (GetUserObjectInformationA(desktop, UOI_NAME, name, 63, 0))
	{
		ILibRemoteLogging_printf(gKVMRemoteLogging, ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_1, "KVM [SLAVE]: Current desktop = %s", name);

		// KVMDEBUG(name, 0);
		if (kvm_server_currentDesktopname == 0)
		{
			// This is the first time we come here.
			kvm_server_currentDesktopname = ((int *)name)[0];
			ILibRemoteLogging_printf(gKVMRemoteLogging, ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_1, "KVM [SLAVE]: Initial desktop captured: %s", name);
		}
		else
		{
			// If the desktop name has changed, switch to it instead of shutting down
			if (kvm_server_currentDesktopname != ((int *)name)[0])
			{
				KVMDEBUG("DESKTOP NAME CHANGE DETECTED, switching session", 0);
				ILibRemoteLogging_printf(gKVMRemoteLogging, ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_1, "KVM [SLAVE]: Desktop switched to: %s (UAC/Lock screen detected)", name);
				kvm_server_currentDesktopname = ((int *)name)[0];
				reset_capture_bitmap();
				// Force screen refresh after desktop switch
				if (checkres != 0)
				{
					kvm_server_SetResolution(writeHandler, reserved);
				}
			}
		}
	}
	else
	{
		KVMDEBUG("GetUserObjectInformation Error", 0);
		ILibRemoteLogging_printf(gKVMRemoteLogging, ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_1, "KVM [SLAVE]: Failed to get desktop information");
	}

	// See if the number of displays has changed
	x = GetSystemMetrics(SM_CMONITORS);
	if (SCREEN_COUNT != x)
	{
		SCREEN_COUNT = x;
		kvm_send_display_list(writeHandler, reserved);
	}

	// Check resolution change
	if (checkres != 0 && g_shutdown == 0)
	{
		VSCREEN_X = GetSystemMetrics(SM_XVIRTUALSCREEN);
		VSCREEN_Y = GetSystemMetrics(SM_YVIRTUALSCREEN);
		VSCREEN_WIDTH = GetSystemMetrics(SM_CXVIRTUALSCREEN);
		VSCREEN_HEIGHT = GetSystemMetrics(SM_CYVIRTUALSCREEN);

		if (SCREEN_SEL_TARGET == 0)
		{
			if (VSCREEN_WIDTH == 0)
			{
				// Old style, one display only. Added this just in case VIRTUALSCREEN does not work.
				x = 0;
				y = 0;
				w = GetSystemMetrics(SM_CXSCREEN);
				h = GetSystemMetrics(SM_CYSCREEN);
			}
			else
			{
				// New style, entire virtual desktop
				x = VSCREEN_X;
				y = VSCREEN_Y;
				w = VSCREEN_WIDTH;
				h = VSCREEN_HEIGHT;
			}

			if (SCREEN_X != x || SCREEN_Y != y || SCREEN_WIDTH != w || SCREEN_HEIGHT != h || SCALING_FACTOR != SCALING_FACTOR_NEW)
			{
				// printf("RESOLUTION CHANGED! (supposedly)\n");
				SCREEN_X = x;
				SCREEN_Y = y;
				SCREEN_WIDTH = w;
				SCREEN_HEIGHT = h;
				kvm_server_SetResolution(writeHandler, reserved);
			}

			if (SCREEN_SEL_TARGET != SCREEN_SEL)
			{
				SCREEN_SEL = SCREEN_SEL_TARGET;
				kvm_send_display_list(writeHandler, reserved);
			}
		}
		else
		{
			// Get the list of monitors
			if (SCREEN_SEL_PROCESS == 0)
			{
				DWORD selection = SCREEN_SEL_TARGET;
				if (EnumDisplayMonitors(NULL, NULL, DisplayInfoEnumProc, (LPARAM)&selection))
				{
					// Set the resolution
					if (SCREEN_SEL_PROCESS & 1)
						kvm_server_SetResolution(writeHandler, reserved);
					if (SCREEN_SEL_PROCESS & 2)
						kvm_send_display_list(writeHandler, reserved);
				}
				SCREEN_SEL_PROCESS = 0;
			}
		}
	}
}

unsigned char g_blockinput = 0;

static kvm_stdin_parser gStdinParser; // the stdin of this process (the input thread, or the agent's own thread in console mode): the bytes of an oversized frame still to be thrown away

// Feed network data into the KVM. Return the number of bytes consumed.
// This method consumes a single command.
int kvm_server_inputdata(char *block, int blocklen, ILibKVM_WriteHandler writeHandler, void *reserved)
{
	unsigned short type, size;

	// the rest of the body of an oversized frame is thrown away first, whatever the size of this read is (also less than 4 bytes)
	if (blocklen > 0 && gStdinParser.discard > 0)
		return (int)kvm_stdin_next(&gStdinParser, (const uint8_t *)block, (size_t)blocklen, NULL);

	// Decode the block header
	if (blocklen < 4)
		return 0;

#ifdef KVM_STREAM_ENABLED
	// Everything this function writes (cmd 7, the display list...) goes through the counter of the ACK window, also when the caller is the agent itself (console mode)
	if (gStreamOn) { writeHandler = kvm_child_write; reserved = NULL; }
#endif
	ILibRemoteLogging_printf(gKVMRemoteLogging, ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_2, "KVM [SLAVE]: Handle Input [Len = %d]", blocklen);
	// KVMDEBUG("kvm_server_inputdata", blocklen);
	CheckDesktopSwitch(0, writeHandler, reserved);

	type = ntohs(((unsigned short *)(block))[0]);
	size = ntohs(((unsigned short *)(block))[1]);

	ILibRemoteLogging_printf(gKVMRemoteLogging, ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_2, "KVM [SLAVE]: Handle Input [Len = %d, type = %u, size = %u]", blocklen, type, size);

	// one rule for every command (kvm_stdin.h). A command that is not complete yet waits; a size field below 4 is not a command: its 4 header bytes are consumed and counted
	// (input_bad), nothing is dispatched, and the parser goes on with the bytes behind them (size 0 used to stop the parser for good, size 1-3 moved it into the middle of a header).
	// A size above the input buffer (30000) can never be completed: the header and then the whole declared length are thrown away, never read as commands.
	{
		int kind = 0;
		size_t used = kvm_stdin_next(&gStdinParser, (const uint8_t *)block, (size_t)blocklen, &kind);
		if (used == 0)
			return 0;
		if (kind != KVM_STDIN_KIND_CMD)
		{
#ifdef KVM_STREAM_ENABLED
			kvm_stream_input_bad();
#endif
			ILibRemoteLogging_printf(gKVMRemoteLogging, ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_1, "KVM [SLAVE]: Input command with a bad size field [type = %u, size = %u]: %s", type, size, size > KVM_STDIN_MAX_CMD ? "header and body skipped" : "header skipped");
			return (int)used;
		}
	}

	// printf("INPUT: %d, %d\r\n", type, size);

	if (type == MNG_KVM_KEY || type == MNG_KVM_KEY_UNICODE || type == MNG_KVM_MOUSE || type == MNG_KVM_TOUCH) { kvm_adapt_note_input(); }
	else if (type == MNG_KVM_REFRESH) { kvm_adapt_note_refresh(); kvm_video_note_refresh(); }

	switch (type)
	{
	case MNG_KVM_INPUT_LOCK:
		// 0 = unlock
		// 1 = lock
		// 2 = query
		if (size == 5)
		{
			switch (block[4])
			{
			case 0:
				g_blockinput = 0;
				BlockInput(0);
				break;
			case 1:
				g_blockinput = 1;
				BlockInput(1);
				break;
			case 2:
				break;
			default:
				return (size);
				break;
			}

			unsigned char buffer[5];
			((unsigned short *)buffer)[0] = (unsigned short)htons((unsigned short)MNG_KVM_INPUT_LOCK); // Write the type
			((unsigned short *)buffer)[1] = (unsigned short)htons((unsigned short)5);				   // Write the size
			buffer[4] = g_blockinput;																   // status

			writeHandler((char *)buffer, 5, reserved);
		}

		break;
	case MNG_KVM_KEY: // Key
	{
		if (size != 6)
			break;
		// KVM_WriteLog(writeHandler, reserved, "Key[%u] UP: %d", (unsigned char)(block[5]), block[4]);
		KeyAction(block[5], block[4]);
		break;
	}
	case MNG_KVM_KEY_UNICODE: // Unicode key
	{
		if (size != 7)
			break;
		// KVM_WriteLog(writeHandler, reserved, "UnicodeKey[%u] UP: %d", ((((unsigned char)block[5]) << 8) + ((unsigned char)block[6])), block[4]);
		KeyActionUnicode(((((unsigned char)block[5]) << 8) + ((unsigned char)block[6])), block[4]);
		break;
	}
	case MNG_KVM_MOUSE: // Mouse
	{
		double x, y;
		short w = 0;
		KVM_MouseCursors curcursor = KVM_MouseCursor_NOCHANGE;

		if (size == 10 || size == 12)
		{
			gRemoteMouseMoved = 1;

			// Get positions and scale correctly
			x = (double)ntohs(((short *)(block))[3]) * 1024 / SCALING_FACTOR;
			y = (double)ntohs(((short *)(block))[4]) * 1024 / SCALING_FACTOR;

			// Add relative display position
			x += fabs((double)(SCREEN_X - VSCREEN_X));
			y += fabs((double)(SCREEN_Y - VSCREEN_Y));

			// Scale back to the virtual screen
			x = (x * ((double)SCREEN_WIDTH / (double)VSCREEN_WIDTH)) * (double)65535;
			y = (y * ((double)SCREEN_HEIGHT / (double)VSCREEN_HEIGHT)) * (double)65535;

			// Perform the mouse movement
			if (size == 12)
				w = ((short)ntohs(((short *)(block))[5]));
			MouseAction((((double)x / (double)SCREEN_WIDTH)), (((double)y / (double)SCREEN_HEIGHT)), (int)(unsigned char)(block[5]), w);
		}
		break;
	}
	case MNG_KVM_COMPRESSION: // Compression
	{
		if (size >= 12)
		{
			// WA-2b: bandwidth budget chosen by the viewer, units of 100 kbit/s, 0 = none. A command of size 10 (older viewer, thumbnails) leaves the budget alone.
			kvm_adapt_set_budget((int)ntohs(((unsigned short *)(block + 10))[0]) * 100);
		}
		if (size >= 10)
		{
			int fr = ((int)ntohs(((unsigned short *)(block + 8))[0]));
			if (fr >= KVM_ADAPT_MIN_FRAME_TIMER && fr <= 5000) // 10 ms with WA-2b (x64 MeshService), 16 ms in the other builds (the viewer's High is 16)
				FRAME_RATE_TIMER = fr;
		}
		if (size >= 8)
		{
			int ns = ((int)ntohs(((unsigned short *)(block + 6))[0]));
			if (ns >= 64 && ns <= 4096)
				SCALING_FACTOR_NEW = kvm_adapt_user_scale(ns); // WA-2b: the scale of the viewer times the share the budget asks for
		}
		if (size >= 6)
		{
			set_tile_compression((int)block[4], (int)block[5]);
		}
		COMPRESSION_RATIO = 100;
		break;
	}
	case MNG_KVM_REFRESH: // Refresh
	{
		int row, col;
		char buffer[8];
		if (size != 4)
			break;

		((unsigned short *)buffer)[0] = (unsigned short)htons((unsigned short)MNG_KVM_SCREEN); // Write the type
		((unsigned short *)buffer)[1] = (unsigned short)htons((unsigned short)8);			   // Write the size
		((unsigned short *)buffer)[2] = (unsigned short)htons((unsigned short)SCALED_WIDTH);   // X position
		((unsigned short *)buffer)[3] = (unsigned short)htons((unsigned short)SCALED_HEIGHT);  // Y position

		writeHandler((char *)buffer, 8, reserved);

		// Send the list of available displays
		kvm_send_display_list(writeHandler, reserved);

		// Reset all tile information
		if (tileInfo == NULL)
		{
			if ((tileInfo = (struct tileInfo_t **)malloc(TILE_HEIGHT_COUNT * sizeof(struct tileInfo_t *))) == NULL)
				ILIBCRITICALEXIT(254);
			for (row = 0; row < TILE_HEIGHT_COUNT; row++)
			{
				if ((tileInfo[row] = (struct tileInfo_t *)malloc(TILE_WIDTH_COUNT * sizeof(struct tileInfo_t))) == NULL)
					ILIBCRITICALEXIT(254);
				for (col = 0; col < TILE_WIDTH_COUNT; col++) { kvm_tile_init(&tileInfo[row][col]); }
			}
		}
		for (row = 0; row < TILE_HEIGHT_COUNT; row++)
		{
			for (col = 0; col < TILE_WIDTH_COUNT; col++)
			{
				tileInfo[row][col].crc = 0xFF;
				tileInfo[row][col].flags = 0;
			}
		}
		InterlockedExchange(&gKvmFullFrame, 1); // The next frame has to check every tile

		break;
	}
	case MNG_KVM_PAUSE: // Pause
	{
		if (size != 5)
			break;
		g_remotepause = block[4];
		break;
	}
	case MNG_KVM_FRAME_RATE_TIMER:
	{
		if (size < 6)
			break; // reads bytes 4-5
		int fr = ((int)ntohs(((unsigned short *)(block))[2]));
		if (fr >= KVM_ADAPT_MIN_FRAME_TIMER && fr <= 5000) // 10 ms with WA-2b (x64 MeshService), 16 ms in the other builds (the viewer's High is 16)
			FRAME_RATE_TIMER = fr;
		break;
	}
	case MNG_KVM_INIT_TOUCH:
	{
		// Attempt to initialized touch support
		char buffer[6];
		unsigned short r = (unsigned short)TouchInit();
		((unsigned short *)buffer)[0] = (unsigned short)htons((unsigned short)MNG_KVM_INIT_TOUCH); // Write the type
		((unsigned short *)buffer)[1] = (unsigned short)htons((unsigned short)6);				   // Write the size
		((unsigned short *)buffer)[2] = (unsigned short)htons(r);								   // Write the return code

		writeHandler((char *)buffer, 6, reserved);
		break;
	}
	case MNG_KVM_TOUCH:
	{
		int r = 0;

		if (size < 5)
			break; // reads byte 4 (the version); version 1 reads up to byte 13, checked below

		if (block[4] == 1 && size >= 14) // Version 1 touch structure (Very simple)
		{
			unsigned int flags = (unsigned int)ntohl(((unsigned int *)(block + 6))[0]);

			// Get positions and scale correctly
			unsigned short x = (unsigned short)(ntohs(((unsigned short *)(block + 10))[0])) * 1024 / (unsigned short)SCALING_FACTOR;
			unsigned short y = (unsigned short)(ntohs(((unsigned short *)(block + 12))[0])) * 1024 / (unsigned short)SCALING_FACTOR;

			// Add relative display position
			x += (unsigned short)fabs((double)(SCREEN_X - VSCREEN_X));
			y += (unsigned short)fabs((double)(SCREEN_Y - VSCREEN_Y));

			// Scale back to the virtual screen
			x = (unsigned short)(((double)x * ((double)SCREEN_WIDTH / (double)VSCREEN_WIDTH)) * (double)65535);
			y = (unsigned short)(((double)y * ((double)SCREEN_HEIGHT / (double)VSCREEN_HEIGHT)) * (double)65535);

			r = TouchAction1(block[5], flags, x, y);
		}
		else if (block[4] == 2) // Version 2 touch structure array
		{
			r = TouchAction2(block + 5, size - 5, SCALING_FACTOR);
		}

		if (r == 1)
		{
			// Reset touch
			char buffer[4];
			((unsigned short *)buffer)[0] = (unsigned short)htons((unsigned short)MNG_KVM_TOUCH); // Write the type
			((unsigned short *)buffer)[1] = (unsigned short)htons((unsigned short)4);			  // Write the size

			writeHandler((char *)buffer, 4, reserved);
		}
		break;
	}
	case MNG_KVM_GET_DISPLAYS:
	{
		kvm_send_display_list(writeHandler, reserved);
		break;
	}
	case MNG_KVM_SET_DISPLAY:
	{
		// Set the display
		int x = 0;
		if (size < 6)
			break;
		x = (unsigned short)ntohs(((unsigned short *)(block + 4))[0]);
		if (x == 65535)
			SCREEN_SEL_TARGET = 0;
		else
			SCREEN_SEL_TARGET = x;
		break;
	}
#ifdef KVM_STREAM_ENABLED
	case MNG_KVM_VIDEO_CTRL: // 240, 241 are exactly 16 bytes; they are queued and handled by the main loop (kvm_stream_poll)
	case MNG_KVM_STREAM_ACK:
	{
		// stdin is a byte stream and its only framing is the size field of each command, as for every other command: a 240 / 241 that is not exactly 16 bytes is dropped,
		// and the bytes it says it has are consumed (never a guessed 16: the commands behind it would be read in the middle). kvm_stream_stdin_consume is tested with
		// streams cut at every position.
		int deliver = 0;
		size_t used = kvm_stream_stdin_consume((const uint8_t *)block, (size_t)blocklen, &deliver);
		if (used == 0) { return 0; }
		if (deliver) { kvm_stream_input(block, type); } else { kvm_stream_input_bad(); }
		return (int)used;
	}
#endif
	}
	return size;
}

typedef struct kvm_data_handler
{
	ILibKVM_WriteHandler handler;
	void *reserved;
	int len;
	char buffer[];
} kvm_data_handler;

#ifdef KVM_STREAM_ENABLED
// Parent process: the child can be restarted (UAC, lock screen), and a new child knows nothing of the veto that meshcore or the multiplexer set (op 5, op 6 of command 240).
// The parent keeps the mask of sources (kvm_veto_track follows the byte stream by the size fields, across writes) while it passes the commands on, and gives it to every
// new child (kvm_relay_restart) before any other input; with it the last commands 5 and 13 (quality, scale, frame timer, budget).
#endif

// Feed network data into the KVM. Return the number of bytes consumed.
// This method consumes as many input commands as it can.
int kvm_relay_feeddata(char *buf, int len, ILibKVM_WriteHandler writeHandler, void *reserved)
{
	if (gChildProcess != NULL)
	{
		if (len >= 2 && ntohs(((unsigned short *)buf)[0]) == MNG_CTRLALTDEL)
		{
			HANDLE ht = CreateThread(NULL, 0, kvm_ctrlaltdel, 0, 0, 0);
			if (ht != NULL)
				CloseHandle(ht);
		}
#ifdef KVM_STREAM_ENABLED
		kvm_veto_track(&gRelayVeto, (const uint8_t *)buf, (size_t)len);
#endif
		ILibProcessPipe_Process_WriteStdIn(gChildProcess, buf, len, ILibTransport_MemoryOwnership_USER);
		ILibRemoteLogging_printf(ILibChainGetLogger(gILibChain), ILibRemoteLogging_Modules_Microstack_Generic, ILibRemoteLogging_Flags_VerbosityLevel_2, "KVM [Master]: Write Input [Type = %u]", ntohs(((unsigned short *)buf)[0]));
		return len;
	}
	else
	{
		int len2 = 0;
		int ptr = 0;
		// while ((len2 = kvm_server_inputdata(buf + ptr, len - ptr, kvm_relay_feeddata_ex, (void*[]) {writeHandler, reserved})) != 0) { ptr += len2; }
		while ((len2 = kvm_server_inputdata(buf + ptr, len - ptr, writeHandler, reserved)) != 0)
		{
			ptr += len2;
		}
		return ptr;
	}
}

// Set the KVM pause state
void kvm_pause(int pause)
{
	// KVMDEBUG("kvm_pause", pause);
	if (gChildProcess == NULL)
	{
		g_pause = pause;
	}
	else
	{
		if (pause == 0)
		{
			KVMDEBUG2("RESUME: KVM");
			ILibProcessPipe_Pipe_Resume(ILibProcessPipe_Process_GetStdOut(gChildProcess));
		}
		else
		{
			KVMDEBUG2("PAUSE: KVM");
			ILibProcessPipe_Pipe_Pause(ILibProcessPipe_Process_GetStdOut(gChildProcess));
		}
	}
}

void kvm_server_SetResolution(ILibKVM_WriteHandler writeHandler, void *reserved)
{
	char buffer[8];
	int row, col;

	KVMDEBUG("kvm_server_SetResolution", 0);

	// Free the tileInfo before you manipulate the TILE_HEIGHT_COUNT
	if (tileInfo != NULL)
	{
		for (row = 0; row < TILE_HEIGHT_COUNT; row++)
		{
			free(tileInfo[row]);
		}
		free(tileInfo);
		tileInfo = NULL;
	}

	// Setup scaling
	gGeometryGeneration++;
	SCALING_FACTOR = SCALING_FACTOR_NEW;
	SCALED_WIDTH = (SCREEN_WIDTH * SCALING_FACTOR) / 1024;
	SCALED_HEIGHT = (SCREEN_HEIGHT * SCALING_FACTOR) / 1024;

	// Compute the tile count
	TILE_WIDTH_COUNT = SCALED_WIDTH / TILE_WIDTH;
	TILE_HEIGHT_COUNT = SCALED_HEIGHT / TILE_HEIGHT;
	if (SCALED_WIDTH % TILE_WIDTH)
		TILE_WIDTH_COUNT++;
	if (SCALED_HEIGHT % TILE_HEIGHT)
		TILE_HEIGHT_COUNT++;

	((unsigned short *)buffer)[0] = (unsigned short)htons((unsigned short)MNG_KVM_SCREEN); // Write the type
	((unsigned short *)buffer)[1] = (unsigned short)htons((unsigned short)8);			   // Write the size
	((unsigned short *)buffer)[2] = (unsigned short)htons((unsigned short)SCALED_WIDTH);   // X position
	((unsigned short *)buffer)[3] = (unsigned short)htons((unsigned short)SCALED_HEIGHT);  // Y position

	writeHandler((char *)buffer, 8, reserved);

	if ((tileInfo = (struct tileInfo_t **)malloc(TILE_HEIGHT_COUNT * sizeof(struct tileInfo_t *))) == NULL)
		ILIBCRITICALEXIT(254);
	for (row = 0; row < TILE_HEIGHT_COUNT; row++)
	{
		if ((tileInfo[row] = (struct tileInfo_t *)malloc(TILE_WIDTH_COUNT * sizeof(struct tileInfo_t))) == NULL)
			ILIBCRITICALEXIT(254);
	}
	for (row = 0; row < TILE_HEIGHT_COUNT; row++)
	{
		for (col = 0; col < TILE_WIDTH_COUNT; col++)
		{
			kvm_tile_init(&tileInfo[row][col]);
		}
	}
	InterlockedExchange(&gKvmFullFrame, 1); // The next frame has to check every tile
#ifdef KVM_VIDEO_ENABLED
	kvm_video_geometry(TILE_HEIGHT_COUNT, TILE_WIDTH_COUNT, SCALED_WIDTH, SCALED_HEIGHT); // the cmd 7 is out; a running video begins a new generation (242 before its first frame)
#endif
}

#define BUFSIZE 65535
#ifdef KVM_VIDEO_ENABLED
static HANDLE gInputThreadH = NULL;	// a handle of the input thread, to read its CPU time (the CPU of the encoder is what the process used less the loop, the capture and the input threads: 5)
#endif
#ifdef _WINSERVICE
DWORD WINAPI kvm_mainloopinput_ex(LPVOID Param)
{
#ifdef KVM_VIDEO_ENABLED
	{
		HANDLE h = NULL, old;
		if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, THREAD_QUERY_INFORMATION, FALSE, 0)) { old = (HANDLE)InterlockedExchangePointer((PVOID volatile *)&gInputThreadH, h); if (old != NULL) { CloseHandle(old); } }
	}
#endif
	int ptr = 0;
	int ptr2 = 0;
	int len = 0;
	char pchRequest2[KVM_STDIN_BUF];
	BOOL fSuccess = FALSE;
	DWORD cbBytesRead = 0;
	ILibKVM_WriteHandler writeHandler = (ILibKVM_WriteHandler)((void **)Param)[0];
	void *reserved = ((void **)Param)[1];

	memset(&gStdinParser, 0, sizeof(gStdinParser)); // a new input stream: nothing to throw away
	KVMDEBUG("kvm_mainloopinput / start", (int)GetCurrentThreadId());

	ILibRemoteLogging_printf(gKVMRemoteLogging, ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_1, "KVM [SLAVE]: (mainloopinput) Starting...");

	while (!g_shutdown)
	{
		fSuccess = ReadFile(hStdIn, pchRequest2 + len, KVM_STDIN_BUF - len, &cbBytesRead, NULL);
		if (!fSuccess || cbBytesRead == 0 || g_shutdown)
		{
			ILibRemoteLogging_printf(gKVMRemoteLogging, ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_1, "KVM [SLAVE]: fSuccess/%d  cbBytesRead/%d  g_shutdown/%d", fSuccess, cbBytesRead, g_shutdown);
			KVMDEBUG("ReadFile() failed", 0); /*ILIBMESSAGE("KVMBREAK-K1\r\n");*/
			g_shutdown = 1;
			break;
		}
		len += cbBytesRead;
		ptr2 = 0;
		while ((ptr2 = kvm_server_inputdata((char *)pchRequest2 + ptr, len - ptr, writeHandler, reserved)) != 0)
		{
			ptr += ptr2;
		}
		if (ptr == len)
		{
			len = 0;
			ptr = 0;
		}
		else
		{
			// the beginning of a command that is not complete yet moves to the start of the buffer (kvm_stdin_compact)
			len = (int)kvm_stdin_compact((uint8_t *)pchRequest2, (size_t)len, (size_t)ptr);
			ptr = 0;
		}
	}
	ILibRemoteLogging_printf(gKVMRemoteLogging, ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_1, "KVM [SLAVE]: (mainloopinput) Exiting...");
	ILibRemoteLogging_Destroy(gKVMRemoteLogging);
	gKVMRemoteLogging = NULL;

	KVMDEBUG("kvm_mainloopinput / end", (int)GetCurrentThreadId());

	return 0;
}

DWORD WINAPI kvm_mainloopinput(LPVOID Param)
{
	DWORD ret = 0;
	if (((int *)&(((void **)Param)[3]))[0] == 1)
	{
		ILib_DumpEnabledContext winException;
		__try
		{
			ret = kvm_mainloopinput_ex(Param);
		}
		__except (ILib_WindowsExceptionFilterEx(GetExceptionCode(), GetExceptionInformation(), &winException))
		{
			ILib_WindowsExceptionDebugEx(&winException);
		}
	}
	else
	{
		ret = kvm_mainloopinput_ex(Param);
	}
	return (ret);
}
#endif

#ifdef KVM_STATS_LOG
// Statistics log: <exe directory>\<exe name without .exe>-kvm.log, one line per event and every KVM_STATS_PERIOD_MS.
// Off by default: written only when MESH_KVM_STATS_LOG=1 is in the environment of the service (or in builds with
// /DKVM_STATS_LOG_ALWAYS). The statistics themselves are always kept, the adaptive quality depends on them.
// At KVM_STATS_LOG_MAX bytes the file is renamed to <name>.1 (replacing the older one) and a new one is started: at most about 2 MiB, and the log never stops.
static SRWLOCK gStatsLock = SRWLOCK_INIT;
static wchar_t gStatsPath[MAX_PATH + 16];
static int gStatsPathReady = 0;
static volatile LONG gStatsLogOn = -1;	// -1 = not read yet, 0 = off, 1 = on

static int kvm_stats_log_enabled(void)
{
	LONG on = gStatsLogOn;
	if (on < 0)
	{
#ifdef KVM_STATS_LOG_ALWAYS
		on = 1;
#else
		char env[8];
		on = (GetEnvironmentVariableA("MESH_KVM_STATS_LOG", env, sizeof(env)) > 0 && env[0] == '1') ? 1 : 0;
#endif
		InterlockedExchange(&gStatsLogOn, on);
	}
	return (int)on;
}

static long long kvm_qpc_frequency(void)
{
	static long long freq = 0;
	if (freq == 0)
	{
		LARGE_INTEGER f;
		freq = (QueryPerformanceFrequency(&f) && f.QuadPart > 0) ? f.QuadPart : 1;
	}
	return freq;
}

// CPU time of this process (kernel + user, microseconds) and the logical processors of the machine (the group of the process); 0 / 1 when the system does not say
static unsigned long long kvm_process_cpu_us(void)
{
	FILETIME c, e, k, u;
	if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) { return 0; }
	return ((((unsigned long long)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((unsigned long long)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 10ull;
}

static unsigned kvm_cpu_cores(void)
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	return si.dwNumberOfProcessors > 0 ? (unsigned)si.dwNumberOfProcessors : 1u;
}

void kvm_stats_log(const char *fmt, ...)
{
	char line[1024];
	SYSTEMTIME st;
	va_list ap;
	int n, m;
	HANDLE h;
	DWORD written;
	LARGE_INTEGER size;

	if (!kvm_stats_log_enabled()) { return; }
	GetLocalTime(&st);
	n = sprintf_s(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d.%03d pid=%lu ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, (unsigned long)GetCurrentProcessId());
	if (n < 0) { return; }
	va_start(ap, fmt);
	m = _vsnprintf_s(line + n, sizeof(line) - n - 3, _TRUNCATE, fmt, ap); // Leaves room for the line end
	va_end(ap);
	if (m < 0) { m = (int)strlen(line + n); } // Truncated
	n += m;
	line[n++] = '\r';
	line[n++] = '\n';

	AcquireSRWLockExclusive(&gStatsLock);
	if (!gStatsPathReady)
	{
		DWORD len = GetModuleFileNameW(NULL, gStatsPath, MAX_PATH);
		if (len > 4 && _wcsicmp(gStatsPath + len - 4, L".exe") == 0) { len -= 4; }
		if (len > 0 && len < MAX_PATH)
		{
			wcscpy_s(gStatsPath + len, 16, L"-kvm.log");
			gStatsPathReady = 1;
		}
	}
	if (gStatsPathReady)
	{
		h = CreateFileW(gStatsPath, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (h != INVALID_HANDLE_VALUE)
		{
			if (!GetFileSizeEx(h, &size)) { CloseHandle(h); }
			else if (size.QuadPart >= KVM_STATS_LOG_MAX)
			{
				// full: the file becomes <name>.1 (the one before it is replaced) and the log goes on in a new one, so a long test never silently stops logging
				wchar_t old[MAX_PATH + 20];
				CloseHandle(h);
				int moved = (wcscpy_s(old, MAX_PATH + 20, gStatsPath) == 0 && wcscat_s(old, MAX_PATH + 20, L".1") == 0 && MoveFileExW(gStatsPath, old, MOVEFILE_REPLACE_EXISTING)) ? 1 : 0;
				// when the rename fails (the old file is locked, a sharing violation) the full file is emptied instead, so it never grows without limit
				h = CreateFileW(gStatsPath, moved ? (FILE_APPEND_DATA | FILE_READ_ATTRIBUTES) : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, moved ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
				if (h != INVALID_HANDLE_VALUE) { WriteFile(h, line, (DWORD)n, &written, NULL); CloseHandle(h); }
			}
			else { WriteFile(h, line, (DWORD)n, &written, NULL); CloseHandle(h); }
		}
	}
	ReleaseSRWLockExclusive(&gStatsLock);
}

// Counters of the main loop, one line is written per KVM_STATS_PERIOD_MS.
typedef struct KVM_Stats
{
	DWORD sessionStart, periodStart;
	int frames, full, partial, unchanged, gdi, dxgi, tiles, moveFrames, captureFrames, copyFrames, outFrames, staleChecks, staleSuspects, ptrGdiReads, ptrRestores, ptrEmbedded, ptrMaskPx, ptrFound, ptrNotFound;
	int capMode, capToGdi, capProbes, capProbeFails, capRecovered, capInconclusive; // choice of the capture source (tile.h KVM_CAP_*): mode after the last frame, events in the period
	long long bytes, writeUs, dirtyRects, dirtyArea, moveRects, moveArea, moveMaxArea, captureUs, copyUs, outUs, staleUs, ptrGdiUs, ptrSearchUs;
	int jpegFrames, videoFrames;						// iterations that wrote JPEG tiles / a video frame
	int dwmImages, dwmMax;								// DXGI images in the period, and the largest AccumulatedFrames (the frames of the DWM that one image stands for)
	long long dwmFrames;								// the sum of AccumulatedFrames over those images
	unsigned long long cpuStartUs;						// CPU time of the process (kernel + user) at the start of the period
} KVM_Stats;
static KVM_Stats gStats;

static void kvm_stats_account(const kvm_frame_info *fi, int tiles, long long bytes, long long writeUs)
{
	gStats.frames++;
	gStats.tiles += tiles;
	gStats.bytes += bytes;
	gStats.writeUs += writeUs;
	if (fi == NULL) { return; }
	if (fi->kind == KVM_FRAME_PARTIAL) { gStats.partial++; }
	else if (fi->kind == KVM_FRAME_UNCHANGED) { gStats.unchanged++; }
	else { gStats.full++; }
	if (fi->source == KVM_SOURCE_DXGI) { gStats.dxgi++; } else { gStats.gdi++; }
	gStats.dirtyRects += fi->dirtyRects;
	gStats.dirtyArea += fi->dirtyArea;
	gStats.moveRects += fi->moveRects;
	gStats.moveArea += fi->moveArea;
	if (fi->moveRects > 0)
	{
		gStats.moveFrames++;
		if (fi->moveArea > gStats.moveMaxArea) { gStats.moveMaxArea = fi->moveArea; }
	}
	if (fi->kind != KVM_FRAME_UNCHANGED) { gStats.captureUs += fi->captureUs; gStats.captureFrames++; }
	if (fi->dwmFrames > 0) { gStats.dwmImages++; gStats.dwmFrames += fi->dwmFrames; if (fi->dwmFrames > gStats.dwmMax) { gStats.dwmMax = fi->dwmFrames; } }
	if (fi->copyUs > 0) { gStats.copyUs += fi->copyUs; gStats.copyFrames++; }
	if (fi->outUs > 0) { gStats.outUs += fi->outUs; gStats.outFrames++; }
	if (fi->staleUs > 0) { gStats.staleUs += fi->staleUs; gStats.staleChecks++; }
	if (fi->staleSuspect > 0) { gStats.staleSuspects++; }
	if (fi->source == KVM_SOURCE_DXGI) { gStats.ptrEmbedded = fi->ptrEmbedded; }
	gStats.ptrGdiReads += fi->ptrGdiReads; gStats.ptrGdiUs += fi->ptrGdiUs; gStats.ptrRestores += fi->ptrRestores; gStats.ptrFound += fi->ptrFound; gStats.ptrNotFound += fi->ptrNotFound; gStats.ptrSearchUs += fi->ptrSearchUs;
	if (fi->source == KVM_SOURCE_DXGI) { gStats.ptrMaskPx = fi->ptrMaskPx; }
	gStats.capMode = fi->capMode;
	if (fi->capEvents & KVM_CAP_EV_TO_GDI) { gStats.capToGdi++; }
	if (fi->capEvents & KVM_CAP_EV_PROBE) { gStats.capProbes++; }
	if (fi->capEvents & KVM_CAP_EV_PROBE_FAILED) { gStats.capProbeFails++; }
	if (fi->capEvents & KVM_CAP_EV_RECOVERED) { gStats.capRecovered++; }
	if (fi->capEvents & KVM_CAP_EV_INCONCLUSIVE) { gStats.capInconclusive++; }
}

static void kvm_stats_flush(int final)
{
	DWORD now = timeGetTime();
	if (!final && (DWORD)(now - gStats.periodStart) < KVM_STATS_PERIOD_MS) { return; }
	if (gStats.frames > 0)
	{
		// the pace schedule, a line of its own (the statistics line is at the limit of the line buffer): the frame rates that the viewer gets, the frames of the DWM behind the DXGI images (an image that stands for more
		// than one frame: the DWM made frames that the loop did not take), the CPU of the process in the period and the lateness of the schedule of the loop (iterations that started 2 ms or more after
		// their slot; over: the work took longer than the period; resync: a whole period behind, the schedule began again).
		{
			const unsigned long long cpuNow = kvm_process_cpu_us(), cpuUs = cpuNow >= gStats.cpuStartUs ? cpuNow - gStats.cpuStartUs : 0ull;
			const unsigned long pms = (unsigned long)(now - gStats.periodStart);
			const unsigned cores = kvm_cpu_cores();
			const double secs = pms > 0 ? (double)pms / 1000.0 : 1.0;
			kvm_stats_log("pace t=%lu period_ms=%lu viewer_timer_ms=%d cores=%u cpu_ms=%llu cpu_cores=%.2f cpu_pct_machine=%.0f fps_jpeg=%.1f fps_video=%.1f dwm_images=%d dwm_frames=%lld dwm_per_image_avg=%.2f dwm_per_image_max=%d dwm_fps=%.1f tick_n=%u tick_late=%u tick_late_ms_avg=%.1f tick_late_ms_max=%u tick_over=%u tick_resync=%u%s",
				(unsigned long)((now - gStats.sessionStart) / 1000), pms, FRAME_RATE_TIMER, cores, cpuUs / 1000ull, (double)cpuUs / 1e6 / secs, 100.0 * (double)cpuUs / 1e6 / secs / (double)cores, (double)gStats.jpegFrames / secs, (double)gStats.videoFrames / secs,
				gStats.dwmImages, gStats.dwmFrames, gStats.dwmImages > 0 ? (double)gStats.dwmFrames / (double)gStats.dwmImages : 0.0, gStats.dwmMax, (double)gStats.dwmFrames / secs,
				(unsigned)gLoopTick.st.n, (unsigned)gLoopTick.st.late, gLoopTick.st.late > 0 ? (double)gLoopTick.st.sumMs / (double)gLoopTick.st.late : 0.0, (unsigned)gLoopTick.st.maxMs, (unsigned)gLoopTick.st.over, (unsigned)gLoopTick.st.resync, final ? " final=1" : "");
		}
		// "frames" are all loop iterations (a frame without changes counts, too); the rate of images that reached the tile check is
		// fps = (full + partial) / period. copy_us: GPU -> persistent image; out_us: making the output buffer (copy of the whole image
		// and cursor, or scaling); stale_*: the check of the DXGI image against GDI (KVM_STALE_QUIET_MS without changes, every period).
		unsigned long periodMs = (unsigned long)(now - gStats.periodStart);
		unsigned long fps10 = periodMs > 0 ? (unsigned long)(((long long)(gStats.full + gStats.partial) * 10000) / (long long)periodMs) : 0;
		kvm_stats_log("stats t=%lu period_ms=%lu path=%s frames=%d full=%d partial=%d unchanged=%d fps=%lu.%lu gdi=%d dxgi=%d tiles=%d bytes=%lld write_ms=%lld dirty_rects=%lld dirty_area=%lld move_rects=%lld move_area=%lld move_frames=%d move_max_area=%lld copy_us_avg=%lld out_us_avg=%lld capture_us_avg=%lld stale_checks=%d stale_suspects=%d stale_us_avg=%lld ptr_embedded=%d ptr_restores=%d ptr_gdi_reads=%d ptr_gdi_us_avg=%lld ptr_mask_px=%d ptr_found=%d ptr_not_found=%d ptr_search_us_avg=%lld cap_mode=%s cap_to_gdi=%d cap_probes=%d cap_probe_fails=%d cap_recovered=%d cap_inconclusive=%d%s",
			(unsigned long)((now - gStats.sessionStart) / 1000), periodMs, (gStats.dxgi == 0 ? "gdi" : (gStats.gdi == 0 ? "dxgi" : "mixed")),
			gStats.frames, gStats.full, gStats.partial, gStats.unchanged, fps10 / 10, fps10 % 10, gStats.gdi, gStats.dxgi, gStats.tiles, gStats.bytes, gStats.writeUs / 1000,
			gStats.dirtyRects, gStats.dirtyArea, gStats.moveRects, gStats.moveArea, gStats.moveFrames, gStats.moveMaxArea,
			gStats.copyFrames > 0 ? gStats.copyUs / gStats.copyFrames : 0LL, gStats.outFrames > 0 ? gStats.outUs / gStats.outFrames : 0LL,
			gStats.captureFrames > 0 ? gStats.captureUs / gStats.captureFrames : 0LL,
			gStats.staleChecks, gStats.staleSuspects, gStats.staleChecks > 0 ? gStats.staleUs / gStats.staleChecks : 0LL,
			gStats.ptrEmbedded, gStats.ptrRestores, gStats.ptrGdiReads, gStats.ptrGdiReads > 0 ? gStats.ptrGdiUs / gStats.ptrGdiReads : 0LL, gStats.ptrMaskPx, gStats.ptrFound, gStats.ptrNotFound, gStats.ptrFound > 0 ? gStats.ptrSearchUs / gStats.ptrFound : 0LL,
			gStats.capMode == KVM_CAP_MODE_GDI ? "gdi" : (gStats.capMode == KVM_CAP_MODE_PROBE ? "probe" : "dxgi"), gStats.capToGdi, gStats.capProbes, gStats.capProbeFails, gStats.capRecovered, gStats.capInconclusive,
			final ? " final=1" : "");
	}
	gStats.frames = gStats.full = gStats.partial = gStats.unchanged = gStats.gdi = gStats.dxgi = gStats.tiles = gStats.moveFrames = gStats.captureFrames = gStats.copyFrames = gStats.outFrames = gStats.staleChecks = gStats.staleSuspects = gStats.ptrGdiReads = gStats.ptrRestores = gStats.ptrFound = gStats.ptrNotFound = 0;
	gStats.capToGdi = gStats.capProbes = gStats.capProbeFails = gStats.capRecovered = gStats.capInconclusive = 0;
	gStats.jpegFrames = gStats.videoFrames = gStats.dwmImages = gStats.dwmMax = 0; gStats.dwmFrames = 0;
	gStats.bytes = gStats.writeUs = gStats.dirtyRects = gStats.dirtyArea = gStats.moveRects = gStats.moveArea = gStats.moveMaxArea = gStats.captureUs = gStats.copyUs = gStats.outUs = gStats.staleUs = gStats.ptrGdiUs = gStats.ptrSearchUs = 0;
	gStats.periodStart = now;
	gStats.cpuStartUs = kvm_process_cpu_us();
	kvm_late_clear(&gLoopTick.st);
	kvm_adapt_stats_flush(final);
#ifdef KVM_STREAM_ENABLED
	kvm_stream_stats_flush(final);
#endif
#ifdef KVM_VIDEO_ENABLED
	kvm_video_stats_flush(final);
#endif
}

static void kvm_stats_start(void)
{
	char env[8] = "";
	int forceGdi, fileGdi;
	memset(&gStats, 0, sizeof(gStats));
	gStats.sessionStart = gStats.periodStart = timeGetTime();
	gStats.cpuStartUs = kvm_process_cpu_us();
	forceGdi = (GetEnvironmentVariableA("MESH_KVM_FORCE_GDI", env, sizeof(env)) > 0 && env[0] == '1') ? 1 : 0;
	fileGdi = kvm_capture_file_gdi();
	kvm_stats_log("start build=\"%s %s\" dxgi=%d env_force_gdi=%d file_force_gdi=%d subsamp=%d screen=%d,%d,%d,%d scale=%d",
		__DATE__, __TIME__,
#ifdef KVM_DXGI_ENABLED
		1,
#else
		0,
#endif
		forceGdi, fileGdi, kvm_subsamp_value(), SCREEN_X, SCREEN_Y, SCREEN_WIDTH, SCREEN_HEIGHT, SCALING_FACTOR);
}
#endif

#ifdef KVM_STREAM_ENABLED
// ---- end-to-end ACK (commands 240-243) -------------------------------------------------------------------------------------------------------------------------
// The protocol and the window are in meshcore/KVM/common/kvm_stream.c (portable, tested in Docker); this is the glue. Section numbers in comments refer to
// the H.264 protocol specification of the MeshCentral fork (not published; chapters 3, 5 and 8); behaviour and switches: README-kvm-perf.md. Only the JPEG picture is there (no video encoder yet): op 1 is answered with reason 1.
//
// Threads: the input thread (kvm_mainloopinput, in console mode the agent's own thread) only validates 240 / 241 and hands them over (gCtlLock): the 240 to the inbox (kvm_inbox in
// kvm_stream.c: the veto as a state that cannot be lost, the other commands joined and rate limited), the 241 to a queue; the main loop (kvm_stream_poll, at the
// start of every iteration) handles them. The state of kvm_stream is touched under gStreamLock, and every write to the pipe is made under the same lock, so the byte counter
// and the positions in the ring describe the same order. The lock is held across the write: a blocked write blocks the other writer too, which the synchronous pipe handle does
// anyway.
#define KVM_ACK_QUEUE 128
typedef struct kvm_ctl_item
{
	unsigned char b[KVM_CTRL_SIZE];
	DWORD at; // timeGetTime() when the input thread got it: the round trip is measured from this, not from the moment the loop gets to it
} kvm_ctl_item;
static kvm_stream gStream;
static CRITICAL_SECTION gStreamLock, gCtlLock;
static volatile LONG gGlueState = 0; // 0 = not initialized, 1 = initializing, 2 = ready
static ILibKVM_WriteHandler gRealWrite = NULL;
static void *gRealReserved = NULL;
static kvm_ctl_item gAckQ[KVM_ACK_QUEUE];
static kvm_inbox gInbox;	// the 240 commands (under gCtlLock)
static int gAckHead = 0, gAckCount = 0;
static volatile LONG gInputBad = 0, gAckDropped = 0;
static DWORD gBlockedWriteMs = 100; // a write that takes longer than this counts as blocked: the timers of the window did not run meanwhile

static void kvm_stream_glue_init(void)
{
	if (InterlockedCompareExchange(&gGlueState, 1, 0) == 0)
	{
		InitializeCriticalSection(&gStreamLock);
		InitializeCriticalSection(&gCtlLock);
		InterlockedExchange(&gGlueState, 2);
	}
	else
	{
		while (gGlueState != 2) { Sleep(0); }
	}
}

// Every write to the pipe: the caller holds gStreamLock. kind: KVM_W_*
static ILibTransport_DoneState kvm_stream_write_locked(char *buffer, int bufferLen, int kind)
{
	ILibTransport_DoneState r;
	DWORD t0 = timeGetTime(), t1;
	kvm_stream_note_write(&gStream, (size_t)bufferLen, kind, t0);
	r = gRealWrite(buffer, bufferLen, gRealReserved);
	t1 = timeGetTime();
	if ((DWORD)(t1 - t0) > gBlockedWriteMs) { kvm_stream_note_blocked(&gStream, t1); }
	return r;
}

// The write handler of the whole child process (tiles, cmd 7, cursor, display list, 242 answers of the input thread...), in place of the one the process was started with.
static ILibTransport_DoneState kvm_child_write(char *buffer, int bufferLen, void *unused)
{
	ILibTransport_DoneState r;
	UNREFERENCED_PARAMETER(unused);
	if (buffer == NULL || bufferLen <= 0 || gRealWrite == NULL) { return gRealWrite != NULL ? gRealWrite(buffer, bufferLen, gRealReserved) : ILibTransport_DoneState_ERROR; }
	EnterCriticalSection(&gStreamLock);
	r = kvm_stream_write_locked(buffer, bufferLen, KVM_W_DATA);
	LeaveCriticalSection(&gStreamLock);
	return r;
}

// Log lines of the main loop are collected while gStreamLock is held and written after it is released (every line opens the file).
// The queue is NOT thread safe: only the main loop thread fills and flushes it (kvm_stream_poll, kvm_stream_announce, and the video code through kvm_glue_state_locked); the input thread
// never logs through it. Kind 2 (an RTT probe) is reserved: nothing produces it, and kvm_log_flush would print it as a state line - the probe is counted in the statistics instead.
typedef struct kvm_log_item { int kind, a, b, c, d, e, f; } kvm_log_item; // kind 0 = ctrl (a = op, b = flags), 1 = state (a video, b ack, c reason, d caps, e gen, f veto), 2 = reserved
static kvm_log_item gLogQ[64];
static int gLogN = 0;
static void kvm_log_add(int kind, int a, int b, int c, int d, int e, int f)
{
	if (gLogN < 64) { kvm_log_item *l = &gLogQ[gLogN++]; l->kind = kind; l->a = a; l->b = b; l->c = c; l->d = d; l->e = e; l->f = f; }
}
static void kvm_log_flush(void)
{
	int i;
	for (i = 0; i < gLogN; i++)
	{
		const kvm_log_item *l = &gLogQ[i];
		if (l->kind == 0) { kvm_stats_log("stream ctrl op=%d flags=%d", l->a, l->b); }
		else { kvm_stats_log("stream state video=%d ack=%d reason=%d caps=%d gen=%d veto=%d", l->a, l->b, l->c, l->d, l->e, l->f); }
	}
	gLogN = 0;
}

static void kvm_stream_send_state_locked(const kvm_state_msg *m0)
{
	unsigned char b[KVM_CTRL_SIZE];
	kvm_state_msg mc = *m0;
	const kvm_state_msg *m = &mc;
#ifdef KVM_VIDEO_ENABLED
	kvm_video_state_caps(&mc); // bit 2 of the caps in every 242: this agent knows Auto (bit 3 of the flags of op 1)
#endif
	kvm_stream_build_state(b, m);
	kvm_stream_write_locked((char *)b, KVM_CTRL_SIZE, KVM_W_STATE);
	kvm_log_add(1, (int)m->video, (int)m->ack, (int)m->reason, (int)m->caps, (int)m->gen, (int)gStream.vetoMask);
}

#ifdef KVM_VIDEO_ENABLED
// The glue for kvm_video.cpp (main loop thread): the lock of the stream state and of the writes, the state itself, the write of a frame and of a 242
void kvm_glue_lock(void) { EnterCriticalSection(&gStreamLock); }
void kvm_glue_unlock(void) { LeaveCriticalSection(&gStreamLock); }
kvm_stream *kvm_glue_stream(void) { return &gStream; }
int kvm_glue_write_locked(char *buffer, int len, int kind) { return (int)kvm_stream_write_locked(buffer, len, kind); }
void kvm_glue_state_locked(const kvm_state_msg *m) { kvm_stream_send_state_locked(m); }
void kvm_glue_log_flush(void) { kvm_log_flush(); }
unsigned long long kvm_glue_cpu_us(void) { return kvm_process_cpu_us(); }	// for the CPU budget of the encoder (kvm_video.cpp)
unsigned kvm_glue_cores(void) { return kvm_cpu_cores(); }

static void kvm_fbuf_lock(void *ctx) { EnterCriticalSection((CRITICAL_SECTION *)ctx); }
static void kvm_fbuf_unlock(void *ctx) { LeaveCriticalSection((CRITICAL_SECTION *)ctx); }

// a video frame was written (by the loop at any of its waiting points, a barrier or the emit): the pause and the error of the pipe are the loop's business as they were when it wrote the frame itself
void kvm_glue_video_written(int res, long long bytes, unsigned writeUs)
{
#ifdef KVM_STATS_LOG
	gStats.videoFrames++;
	gStats.tiles++;
	gStats.bytes += bytes;
	gStats.writeUs += writeUs;
#else
	(void)bytes; (void)writeUs;
#endif
	if (res == ILibTransport_DoneState_INCOMPLETE)
	{
		g_pause = 1;
		ILibRemoteLogging_printf(ILibChainGetLogger(gILibChain), ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_2, "Agent KVM: KVM PAUSE");
	}
	else if (res == ILibTransport_DoneState_ERROR) { g_shutdown = 1; }
}
#endif


// Start of the KVM session in this process: state, queues, and the counting write handler in place of the process's own (*wh, *reserved, and the parameters the input thread gets)
static void kvm_stream_session_begin(ILibKVM_WriteHandler *wh, void **reserved, void **parm)
{
	char env[16] = "";
	int capAck = 1, capVideo = 0, videoReason = KVM_REASON_DISABLED; // without the video code (/DKVM_VIDEO_DISABLE) an op 1 is answered with reason 6 (docs 13)
	kvm_stream_glue_init();
	// ACK needs the adaptive path (WA-2) to gate: it is off with MESH_KVM_ADAPT=0 (the switch back to the behaviour before WA-2) and with MESH_KVM_ACK=0
	if (GetEnvironmentVariableA("MESH_KVM_ADAPT", env, sizeof(env)) > 0 && env[0] == '0') { capAck = 0; }
	env[0] = 0;
	if (GetEnvironmentVariableA("MESH_KVM_ACK", env, sizeof(env)) > 0 && env[0] == '0') { capAck = 0; }
#ifdef KVM_VIDEO_ENABLED
	// The video needs the window and the sharpening (docs 5.4): without them it is off, and Media Foundation is not even loaded. Here (this thread) the encoder will be used too.
	if (capAck) { capVideo = kvm_video_session_begin(&videoReason); }
#endif
	EnterCriticalSection(&gStreamLock);
	if (*wh != kvm_child_write) { gRealWrite = *wh; gRealReserved = *reserved; } // not again after a restart of the session in the same process: it would call itself
	kvm_stream_init(&gStream, capVideo, capAck, videoReason, KVM_REASON_DISABLED);
	EnterCriticalSection(&gCtlLock);
	kvm_inbox_init(&gInbox, timeGetTime());
	gAckHead = gAckCount = 0;
	LeaveCriticalSection(&gCtlLock);
	InterlockedExchange(&gStreamOn, 1);
	LeaveCriticalSection(&gStreamLock);
	*wh = kvm_child_write;
	*reserved = NULL;
	if (parm != NULL) { parm[0] = (void *)kvm_child_write; parm[1] = NULL; }
	kvm_stats_log("stream start cap_ack=%d cap_video=%d", capAck, capVideo);
}

static void kvm_stream_session_end(void)
{
	if (!gStreamOn) { return; }
	kvm_stream_stats_flush(1);
#ifdef KVM_VIDEO_ENABLED
	kvm_video_session_end(); // the encoder and Media Foundation go before the stream state does
#endif
	InterlockedExchange(&gStreamOn, 0);
}

// 242 with reason 11: this process has started (also after a restart of the child); the viewer has to repeat its request
static void kvm_stream_announce(void)
{
	kvm_state_msg m;
	if (!gStreamOn) { return; }
	EnterCriticalSection(&gStreamLock);
	kvm_stream_start_state(&gStream, &m);
	kvm_stream_send_state_locked(&m);
	LeaveCriticalSection(&gStreamLock);
	kvm_log_flush();
}

static void kvm_stream_input_bad(void) { InterlockedIncrement(&gInputBad); }

// Input thread: a 240 or 241 (16 bytes). The stateless checks of 4.1 are made here, the rest in the main loop.
static void kvm_stream_input(const char *block, int type)
{
	kvm_ctl_item it;
	if (!gStreamOn) { return; }
	memcpy(it.b, block, KVM_CTRL_SIZE);
	it.at = timeGetTime();
	if (type == MNG_KVM_VIDEO_CTRL)
	{
		kvm_ctrl c;
		if (kvm_stream_parse_ctrl(it.b, KVM_CTRL_SIZE, &c) != KVM_REJ_NONE) { kvm_stream_input_bad(); return; }
		EnterCriticalSection(&gCtlLock);
		(void)kvm_inbox_put(&gInbox, &c, (uint32_t)it.at); // never refused for lack of room; over the rate only the ordinary commands are dropped (counted in the inbox)
		LeaveCriticalSection(&gCtlLock);
	}
	else
	{
		if (kvm_stream_parse_ack(it.b, KVM_CTRL_SIZE, NULL) != KVM_REJ_NONE) { kvm_stream_input_bad(); return; }
		EnterCriticalSection(&gCtlLock);
		if (gAckCount == KVM_ACK_QUEUE) { gAckHead = (gAckHead + 1) % KVM_ACK_QUEUE; gAckCount--; InterlockedIncrement(&gAckDropped); } // an ACK covers the older ones: keep the newest
		gAckQ[(gAckHead + gAckCount++) % KVM_ACK_QUEUE] = it;
		LeaveCriticalSection(&gCtlLock);
	}
}

// Main loop, start of every iteration: the queued commands, then the timers (2 s / 15 s without ACK). The answers (242) go out before the next picture.
static void kvm_stream_poll(void)
{
	kvm_inbox_op ctl[KVM_INBOX_OPS];
	kvm_inbox batch;
	kvm_ctl_item ack[KVM_ACK_QUEUE];
	size_t nctl = 0, i;
	int nack = 0, k;
	kvm_state_msg reply;
	if (!gStreamOn) { return; }
	EnterCriticalSection(&gCtlLock);
	kvm_inbox_take(&gInbox, &batch);
	while (gAckCount > 0) { ack[nack++] = gAckQ[gAckHead]; gAckHead = (gAckHead + 1) % KVM_ACK_QUEUE; gAckCount--; }
	LeaveCriticalSection(&gCtlLock);
	EnterCriticalSection(&gStreamLock);
	// the veto first (the stream gets the veto of the inbox before any request for video is looked at), then the last request of the viewer, then the repair
	nctl = kvm_inbox_expand(&batch, gStream.vetoMask, ctl);
	for (i = 0; i < nctl; i++)
	{
		kvm_ctrl c = ctl[i].c;
		unsigned actions = 0;
		kvm_log_add(0, (int)c.op, (int)c.flags, 0, 0, 0, 0);
		if (kvm_stream_on_ctrl(&gStream, &c, ctl[i].at, &reply, &actions)) { kvm_stream_send_state_locked(&reply); }
#ifdef KVM_VIDEO_ENABLED
		kvm_video_ctrl(&c, actions, (unsigned)ctl[i].at); // remembered; the work is done by kvm_video_poll right after, without the lock
#endif
	}
	for (k = 0; k < nack; k++)
	{
		kvm_ack a;
		if (kvm_stream_parse_ack(ack[k].b, KVM_CTRL_SIZE, &a) == KVM_REJ_NONE) { (void)kvm_stream_on_ack(&gStream, &a, ack[k].at); }
	}
	// "paused" for the timers is a blocked write (g_pause), never g_remotepause, which is written by command 8 but read by nobody
	// the picture type decides whether the window can be used (5.1): not for a type WA-2 does not handle
	if (kvm_stream_set_ack_capable(&gStream, kvm_adapt_ack_capable(), timeGetTime(), &reply)) { kvm_stream_send_state_locked(&reply); }
	if (kvm_stream_tick(&gStream, timeGetTime(), g_pause, &reply)) { kvm_stream_send_state_locked(&reply); }
	LeaveCriticalSection(&gStreamLock);
	kvm_log_flush();
}

// Main loop, end of every iteration: rule M1 (the iteration wrote something: it ends with a position) and the probe marker (M2).
static void kvm_stream_iteration_end(void)
{
	unsigned char marker[KVM_MARKER_BYTES];
	int probe = 0;
	DWORD now;
	if (!gStreamOn) { return; }
	now = timeGetTime();
	EnterCriticalSection(&gStreamLock);
	if (kvm_stream_marker_wanted(&gStream, now, g_pause, &probe))
	{
		kvm_stream_make_marker(&gStream, now, probe, marker);
		kvm_stream_write_locked((char *)marker, KVM_MARKER_BYTES, KVM_W_POSITION);
	}
	LeaveCriticalSection(&gStreamLock);
}

// WA-2 asks before it writes (kvm_adapt.cpp): kinds KVM_ACK_BIG / KVM_ACK_SMALL / KVM_ACK_SHARPEN = KVM_GATE_*
int kvm_ack_gate(int kind)
{
	int ok;
	DWORD now;
	if (!gStreamOn) { return 1; }
	now = timeGetTime();
	EnterCriticalSection(&gStreamLock);
	ok = kvm_stream_window_allows(&gStream, kind, now);
	if (!ok) { if (kind == KVM_GATE_BIG) { kvm_stream_note_link_held(&gStream, now); } else { kvm_stream_note_held_back(&gStream, now); } }	// only a big rectangle says that the link is the limit
	LeaveCriticalSection(&gStreamLock);
	return ok;
}

int kvm_ack_peek(int kind)
{
	int ok;
	if (!gStreamOn) { return 1; }
	EnterCriticalSection(&gStreamLock);
	ok = kvm_stream_window_allows(&gStream, kind, timeGetTime());
	LeaveCriticalSection(&gStreamLock);
	return ok;
}

int kvm_ack_force_info(unsigned long long *rate, unsigned long long *inflight, unsigned *window)
{
	int on;
	DWORD now;
	if (!gStreamOn) { return 0; }
	now = timeGetTime();
	EnterCriticalSection(&gStreamLock);
	on = kvm_stream_ack_active(&gStream);
	*rate = kvm_stream_rate_recent(&gStream, now); *inflight = kvm_stream_inflight(&gStream) + gStream.reservedBytes; *window = (unsigned)kvm_stream_window(&gStream, now);	// (the access unit that the encoder is making counts: 4)
	LeaveCriticalSection(&gStreamLock);
	return on;
}

void kvm_ack_note_held_back(void)
{
	if (!gStreamOn) { return; }
	EnterCriticalSection(&gStreamLock);
	kvm_stream_note_held_back(&gStream, timeGetTime());
	LeaveCriticalSection(&gStreamLock);
}

int kvm_ack_note_link_held(void)
{
	int ev;
	if (!gStreamOn) { return 1; }
	EnterCriticalSection(&gStreamLock);
	ev = kvm_stream_note_link_held(&gStream, timeGetTime());
	LeaveCriticalSection(&gStreamLock);
	return ev;
}

// One line per statistics period (and at the end of the session)
static void kvm_stream_stats_flush(int final)
{
	kvm_stream_stats st;
	DWORD now = timeGetTime();
	int ack, veto, want;
	unsigned win;
	unsigned long long inflight, rate;
	unsigned minRtt, ctlLimited, ctlMerged;
	if (!gStreamOn) { return; }
	EnterCriticalSection(&gCtlLock);
	ctlLimited = (unsigned)gInbox.nLimited; ctlMerged = (unsigned)gInbox.nMerged; // ctl_dropped: ordinary 240 over the rate; ctl_merged: ones that replaced / joined a waiting one
	LeaveCriticalSection(&gCtlLock);
	EnterCriticalSection(&gStreamLock);
	st = gStream.st;
	ack = kvm_stream_ack_active(&gStream); veto = gStream.vetoMask; want = gStream.want;
	win = kvm_stream_window(&gStream, now); inflight = kvm_stream_inflight(&gStream); rate = kvm_stream_rate(&gStream, now); minRtt = gStream.minRtt;
	LeaveCriticalSection(&gStreamLock);
	kvm_stats_log("stream stats%s ack=%d want=%d veto=%d win_kb=%u inflight_kb=%llu rate_kbps=%llu min_rtt_ms=%u acks_ok=%u acks_off=%u acks_flood=%u acks_unknown=%u acks_stale=%u markers=%u probes=%u rtt_probes=%u/%u/%u stalls=%u timeouts=%u gate_denied=%u ctrl_ok=%u input_bad=%ld ctl_dropped=%u ctl_merged=%u ack_dropped=%ld",
		final ? " final=1" : "", ack, want, veto, win / 1024u, inflight / 1024ull, rate * 8ull / 1000ull, minRtt, st.acksOk, st.acksOff, st.acksFlood, st.acksUnknown, st.acksStale, st.markers, st.probes, st.rttProbeStarts, st.rttProbeDone, st.rttProbeAborts, st.stalls, st.timeouts, st.gateDenied, st.ctrlOk,
		(long)gInputBad, ctlLimited, ctlMerged, (long)gAckDropped);
}
#endif // KVM_STREAM_ENABLED

// Screen capture runs on its own thread: while the screen is changing, the next frame is captured (GDI capture
// is paced by DWM composition) while the current one is encoded and sent. The capture thread only runs between
// kvm_capture_start() and kvm_capture_wait(); the geometry (resolution, scaling, desktop) is only changed by the
// main loop while the capture thread is idle.
typedef struct KVM_CaptureState
{
	HANDLE thread;
	HANDLE startEvent;
	HANDLE doneEvent;
	HDESK desktop;
	volatile int quit;
	long mouseMove[3];
	void *buffer;
	long long bufferSize;
	int pixelSize;
	int result;
	int generation;
	int flags;				// KVM_CAPTURE_* passed to get_desktop_buffer()
	kvm_frame_info info;	// What get_desktop_buffer() reported about the frame
} KVM_CaptureState;
KVM_CaptureState gCapture;
#ifdef KVM_VIDEO_ENABLED
// CPU time of the capture thread (microseconds), which works beside the encoder and is taken out of the parallelism that is measured for the encoder; 0 when there is no thread or the system does not say
unsigned long long kvm_glue_capture_cpu_us(void)
{
	FILETIME c, e, k, u;
	if (gCapture.thread == NULL || !GetThreadTimes(gCapture.thread, &c, &e, &k, &u)) { return 0; }
	return ((((unsigned long long)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((unsigned long long)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 10ull;
}
// the CPU of the calling thread (the main loop: kvm_video_poll) and of the capture thread together: what the process used less this is the encoder's
unsigned long long kvm_glue_nonenc_cpu_us(void)
{
	FILETIME c, e, k, u;
	unsigned long long cap = kvm_glue_capture_cpu_us();
	if (cap == 0 || !GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) { return 0; }
	{
		unsigned long long in = 0;
		FILETIME ic, ie, ik, iu;
		HANDLE ih = gInputThreadH;
		if (ih != NULL && GetThreadTimes(ih, &ic, &ie, &ik, &iu)) { in = ((((unsigned long long)ik.dwHighDateTime << 32) | ik.dwLowDateTime) + (((unsigned long long)iu.dwHighDateTime << 32) | iu.dwLowDateTime)) / 10ull; }
		return cap + in + ((((unsigned long long)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((unsigned long long)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 10ull;
	}
}
#endif

// Moves the capture thread to the current input desktop (UAC prompt, lock screen...)
void kvm_capture_switch_desktop()
{
	HDESK desktop = OpenInputDesktop(0, TRUE, DESKTOP_CREATEMENU | DESKTOP_CREATEWINDOW | DESKTOP_ENUMERATE | DESKTOP_HOOKCONTROL | DESKTOP_WRITEOBJECTS | DESKTOP_READOBJECTS | DESKTOP_SWITCHDESKTOP | GENERIC_WRITE);
	if (desktop == NULL) return;
	if (SetThreadDesktop(desktop) == 0)
	{
#ifdef KVM_STATS_LOG
		static int logged = 0; // Not on every frame
		DWORD err = GetLastError();
		if (logged < 5) { logged++; kvm_stats_log("capture SetThreadDesktop failed err=%lu", (unsigned long)err); }
#endif
		CloseDesktop(desktop);
		return;
	}
	if (gCapture.desktop != NULL) CloseDesktop(gCapture.desktop);
	gCapture.desktop = desktop;
}

DWORD WINAPI kvm_capture_thread(LPVOID parm)
{
	UNREFERENCED_PARAMETER(parm);
	while (WaitForSingleObject(gCapture.startEvent, INFINITE) == WAIT_OBJECT_0 && !gCapture.quit)
	{
#ifdef KVM_STATS_LOG
		LARGE_INTEGER t0, t1;
#endif
		kvm_capture_switch_desktop();
		gCapture.buffer = NULL;
		gCapture.bufferSize = 0;
#ifdef KVM_STATS_LOG
		QueryPerformanceCounter(&t0);
#endif
		gCapture.result = get_desktop_buffer(&gCapture.buffer, &gCapture.bufferSize, gCapture.mouseMove, &gCapture.pixelSize, gCapture.flags, &gCapture.info);
#ifdef KVM_STATS_LOG
		QueryPerformanceCounter(&t1);
		gCapture.info.captureUs = (unsigned int)(((t1.QuadPart - t0.QuadPart) * 1000000LL) / kvm_qpc_frequency());
#endif
		SetEvent(gCapture.doneEvent);
	}
#ifdef KVM_DXGI_ENABLED
	kvm_dxgi_thread_end(); // The duplication belongs to this thread
#endif
	release_desktop_dc(); // A DC must be released on the thread that got it
	return 0;
}

void kvm_capture_start(long *mouseMove)
{
	memcpy(gCapture.mouseMove, mouseMove, sizeof(gCapture.mouseMove));
	gCapture.generation = gGeometryGeneration;
	gCapture.flags = InterlockedExchange(&gKvmFullFrame, 0) ? KVM_CAPTURE_FORCE_FULL : 0;
	if (kvm_adapt_want_image()) { gCapture.flags |= KVM_CAPTURE_WANT_IMAGE; } // lossy tiles are due for sharpening: a picture is needed even if nothing changed
	SetEvent(gCapture.startEvent);
}

// The frame of the capture thread is taken over (after the wait for gCapture.doneEvent)
static int kvm_capture_collect(void **desktop, long long *bufferSize, int *generation, int *pixelSize, kvm_frame_info *info)
{
	*bufferSize = gCapture.bufferSize;
	*generation = gCapture.generation;
	*pixelSize = gCapture.pixelSize;
	*info = gCapture.info;
	if (gCapture.result != 0)
	{
		*desktop = NULL;
		if (info->tileDirty != NULL) { free(info->tileDirty); info->tileDirty = NULL; }
		return gCapture.result;
	}
	*desktop = gCapture.buffer;
	return 0;
}

// Waits for the frame started by kvm_capture_start(). Returns 0 on success, then *desktop is the frame (NULL when
// info->kind is KVM_FRAME_UNCHANGED) and info->tileDirty belongs to the caller; any other value means the capture failed.
// While it waits, the loop serves the video: a frame that the encoder finished is written at once, and the picture that waits for its slot is started when the slot comes.
int kvm_capture_wait(void **desktop, long long *bufferSize, int *generation, int *pixelSize, kvm_frame_info *info)
{
#ifdef KVM_VIDEO_ENABLED
	HANDLE ev = (HANDLE)kvm_video_event();
	if (ev != NULL)
	{
		HANDLE h[2];
		h[0] = gCapture.doneEvent; h[1] = ev;
		for (;;)
		{
			const int ms = kvm_video_wake_ms();
			const DWORD r = WaitForMultipleObjects(2, h, FALSE, ms > 0 ? (DWORD)ms : INFINITE);
			if (r == WAIT_OBJECT_0) { break; }
			if (r != WAIT_OBJECT_0 + 1 && r != WAIT_TIMEOUT) { WaitForSingleObject(gCapture.doneEvent, INFINITE); break; }
			kvm_video_service();
		}
		return kvm_capture_collect(desktop, bufferSize, generation, pixelSize, info);
	}
#endif
	WaitForSingleObject(gCapture.doneEvent, INFINITE);
	return kvm_capture_collect(desktop, bufferSize, generation, pixelSize, info);
}

// Handles desktop and resolution changes and the pending mouse packets before a capture. Returns 0 on shutdown.
int kvm_prepare_capture(ILibKVM_WriteHandler writeHandler, void *reserved, long *mouseMove, int *sentHideCursor)
{
	char *tmoBuffer;

	CheckDesktopSwitch(1, writeHandler, reserved);
	if (g_shutdown)
		return 0;

	// Enter Alertable State, so we can dispatch any packets if necessary.
	// We are doing it here, in case we need to merge any data with the bitmaps
	SleepEx(0, TRUE);
	mouseMove[0] = 0;
	while ((tmoBuffer = ILibQueue_DeQueue(gPendingPackets)) != NULL)
	{
		if (ntohs(((unsigned short *)tmoBuffer)[0]) == MNG_KVM_MOUSE_MOVE)
		{
			if (SCREEN_SEL_TARGET == 0)
			{
				mouseMove[0] = 1;
				mouseMove[1] = ((long *)tmoBuffer)[1] - VSCREEN_X;
				mouseMove[2] = ((long *)tmoBuffer)[2] - VSCREEN_Y;
			}
			else
			{
				if (((long *)tmoBuffer)[1] >= SCREEN_X && ((long *)tmoBuffer)[1] <= (SCREEN_X + SCREEN_WIDTH) &&
					((long *)tmoBuffer)[2] >= SCREEN_Y && ((long *)tmoBuffer)[2] <= (SCREEN_Y + SCREEN_HEIGHT))
				{
					mouseMove[0] = 1;
					mouseMove[1] = ((long *)tmoBuffer)[1] - SCREEN_X;
					mouseMove[2] = ((long *)tmoBuffer)[2] - SCREEN_Y;
				}
			}
		}
		else
		{
			if (ntohs(((unsigned short *)tmoBuffer)[0]) != MNG_KVM_MOUSE_CURSOR || *sentHideCursor == 0)
			{
				writeHandler(tmoBuffer, (int)ILibMemory_Size(tmoBuffer), reserved);
			}
		}
		ILibMemory_Free(tmoBuffer);
	}
	if (mouseMove[0] == 0 && (gRemoteMouseRenderDefault != 0 || gRemoteMouseMoved == 0))
	{
		mouseMove[0] = 1;
		CURSORINFO info = {0};
		info.cbSize = sizeof(info);
		GetCursorInfo(&info);

		if (SCREEN_SEL_TARGET == 0)
		{
			mouseMove[1] = info.ptScreenPos.x - VSCREEN_X;
			mouseMove[2] = info.ptScreenPos.y - VSCREEN_Y;
		}
		else
		{
			mouseMove[1] = info.ptScreenPos.x - SCREEN_X;
			mouseMove[2] = info.ptScreenPos.y - SCREEN_Y;
		}
	}
	if (mouseMove[0] != 0)
	{
		if (*sentHideCursor == 0)
		{
			*sentHideCursor = 1;
			char tmpBuffer[5];
			((unsigned short *)tmpBuffer)[0] = (unsigned short)htons((unsigned short)MNG_KVM_MOUSE_CURSOR); // Write the type
			((unsigned short *)tmpBuffer)[1] = (unsigned short)htons((unsigned short)5);					// Write the size
			tmpBuffer[4] = (char)KVM_MouseCursor_NONE;														// Cursor Type
			writeHandler(tmpBuffer, 5, reserved);
		}
	}
	else
	{
		if (*sentHideCursor != 0)
		{
			*sentHideCursor = 0;
			char tmpBuffer[5];
			((unsigned short *)tmpBuffer)[0] = (unsigned short)htons((unsigned short)MNG_KVM_MOUSE_CURSOR); // Write the type
			((unsigned short *)tmpBuffer)[1] = (unsigned short)htons((unsigned short)5);					// Write the size
			tmpBuffer[4] = (char)gCurrentCursor;															// Cursor Type
			writeHandler(tmpBuffer, 5, reserved);
		}
	}
	return 1;
}

// This is the main KVM pooling loop. It will look at the display and see if any changes occur. [Runs as daemon if Windows Service]
DWORD WINAPI kvm_server_mainloop_ex(LPVOID parm)
{
	// long cur_timestamp = 0;
	// long prev_timestamp = 0;
	// long time_diff = 50;
	long long tilesize;
	int width, height = 0;
	void *buf, *desktop;
	long long desktopsize;
	BITMAPINFO bmpInfo;
	int row, col;
	ILibKVM_WriteHandler writeHandler = (ILibKVM_WriteHandler)((void **)parm)[0];
	void *reserved = ((void **)parm)[1];
	char *tmoBuffer;
	long mouseMove[3] = {0, 0, 0};
	int sentHideCursor = 0;
	int captureInFlight = 0, lastSentTiles = 0, frameGeneration = 0, framePixelSize = 0, dropFrame = 0;
	kvm_frame_info frameInfo;
	int captureResult = 0, idleWait = 0, aborted = 0, frameTimer = 0;

	gPendingPackets = ILibQueue_Create();
	KVM_InitMouseCursors(gPendingPackets);

#ifdef _WINSERVICE
	if (!kvmConsoleMode)
	{
		gKVMRemoteLogging = ILibRemoteLogging_Create(NULL);
		ILibRemoteLogging_SetRawForward(gKVMRemoteLogging, sizeof(KVMDebugLog), kvm_slave_OnRawForwardLog);
		ILibRemoteLogging_printf(gKVMRemoteLogging, ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_1, "KVM [SLAVE]: Child Processing Running...");
	}
#endif

	// This basic lock will prevent 2 thread from running at the same time. Gives time for the first one to fully exit.
	while (ThreadRunning != 0 && height < 200)
	{
		height++;
		Sleep(50);
	}
	if (height >= 200 && ThreadRunning != 0)
		return 0;
	ThreadRunning = 1;
	g_shutdown = 0;

	g_pause = 0;
	g_remotepause = ((int *)&(((void **)parm)[2]))[0];

	KVMDEBUG("kvm_server_mainloop / start1", (int)GetCurrentThreadId());

#ifdef _WINSERVICE
	if (!kvmConsoleMode)
	{
		hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
		hStdIn = GetStdHandle(STD_INPUT_HANDLE);
	}
#endif

	KVMDEBUG("kvm_server_mainloop / start2", (int)GetCurrentThreadId());

	if (!initialize_gdiplus())
	{
#ifdef _WINSERVICE
		if (!kvmConsoleMode)
		{
			ILibRemoteLogging_printf(gKVMRemoteLogging, ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_1, "KVM [SLAVE]: initialize_gdiplus() failed");
		}
#endif
		KVMDEBUG("kvm_server_mainloop / initialize_gdiplus failed", (int)GetCurrentThreadId());
		return 0;
	}
#ifdef _WINSERVICE
	if (!kvmConsoleMode)
	{
		ILibRemoteLogging_printf(gKVMRemoteLogging, ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_1, "KVM [SLAVE]: initialize_gdiplus() SUCCESS");
	}
#endif
#ifdef KVM_STREAM_ENABLED
	kvm_stream_session_begin(&writeHandler, &reserved, (void **)parm); // from here on every write of this process is counted (the input thread gets the same handler)
#endif
	kvm_server_SetResolution(writeHandler, reserved);

#ifdef _WINSERVICE
	if (!kvmConsoleMode)
	{
		g_shutdown = 0;
		kvmthread = CreateThread(NULL, 0, kvm_mainloopinput, parm, 0, 0);
		CloseHandle(kvmthread);
	}
#endif

	// Set all CRCs to 0xFF
	for (row = 0; row < TILE_HEIGHT_COUNT; row++)
	{
		for (col = 0; col < TILE_WIDTH_COUNT; col++)
		{
			tileInfo[row][col].crc = 0xFF;
		}
	}
	InterlockedExchange(&gKvmFullFrame, 1); // The first frame has to check every tile
#ifdef KVM_STATS_LOG
	kvm_stats_start();
#endif
	kvm_adapt_session_start();

	// Send the list of displays
	kvm_send_display_list(writeHandler, reserved);
#ifdef KVM_STREAM_ENABLED
	kvm_stream_announce(); // 242 reason 11: a new process (or a new child after UAC / the lock screen): the viewer has to repeat its request
#endif

	Sleep(100); // Pausing here seems to fix connection issues, especially with WebRTC. TODO: Investigate why.
	KVMDEBUG("kvm_server_mainloop / start3", (int)GetCurrentThreadId());

	// Start the capture thread. It gets its own screen DC.
#ifdef KVM_VIDEO_ENABLED
	{
		static CRITICAL_SECTION fbufLock;
		static int fbufLockInit = 0;
		static kvm_fbuf_sys fbufSys = { NULL, NULL, NULL };
		if (!fbufLockInit) { InitializeCriticalSection(&fbufLock); fbufLockInit = 1; fbufSys.lock = kvm_fbuf_lock; fbufSys.unlock = kvm_fbuf_unlock; fbufSys.ctx = &fbufLock; }
		kvm_fbuf_setup(&fbufSys);	// the buffers of the pictures (the capture thread gets them, the loop and the encoder give them back)
	}
#endif
	release_desktop_dc();
	ZeroMemory(&gCapture, sizeof(gCapture));
	if ((gCapture.startEvent = CreateEvent(NULL, FALSE, FALSE, NULL)) == NULL ||
		(gCapture.doneEvent = CreateEvent(NULL, FALSE, FALSE, NULL)) == NULL ||
		(gCapture.thread = CreateThread(NULL, 0, kvm_capture_thread, NULL, 0, NULL)) == NULL)
	{
		KVMDEBUG("Unable to start the capture thread", 0);
		g_shutdown = 1;
	}

	// Use 1ms timer resolution, otherwise Sleep(FRAME_RATE_TIMER) is rounded up to the 15.6ms system tick.
	timeBeginPeriod(1);

	// Loop and send only when a tile changes.
	kvm_tick_init(&gLoopTick);
	while (!g_shutdown)
	{
		DWORD frameStart = timeGetTime();
		int sentTiles = 0;
#ifdef KVM_STATS_LOG
		int statTiles = 0;
		long long statBytes = 0, statWriteUs = 0;
		LARGE_INTEGER lq0, lq1, lq2, lq3;
		long long loopWaitUs = 0, loopProcUs = 0;
#endif
		idleWait = 0;
		KVMDEBUG("kvm_server_mainloop / loop1", (int)GetCurrentThreadId());

		// Reset all the flags to TILE_TODO
		for (row = 0; row < TILE_HEIGHT_COUNT; row++)
		{
			for (col = 0; col < TILE_WIDTH_COUNT; col++)
			{
				tileInfo[row][col].flags = (char)TILE_TODO;
#ifdef KVM_ALL_TILES
				tileInfo[row][col].crc = 0xFF;
#endif
			}
		}
		kvm_adapt_iteration(); // WA-2: regulator window, tiles due for sharpening
#ifdef KVM_STREAM_ENABLED
		kvm_stream_poll(); // 240 / 241 queued by the input thread, timers of the ACK window
#endif
#ifdef KVM_VIDEO_ENABLED
		kvm_video_poll(); // requests of the viewer for video, the encoder, a new generation, repair
#endif
		if (!captureInFlight)
		{
			if (!kvm_prepare_capture(writeHandler, reserved, mouseMove, &sentHideCursor))
				break;
			kvm_capture_start(mouseMove);
		}

		// Get the captured desktop
		desktop = NULL;
#ifdef KVM_STATS_LOG
		QueryPerformanceCounter(&lq0);
#endif
		captureResult = kvm_capture_wait(&desktop, &desktopsize, &frameGeneration, &framePixelSize, &frameInfo);
#ifdef KVM_STATS_LOG
		QueryPerformanceCounter(&lq1);
		loopWaitUs = ((lq1.QuadPart - lq0.QuadPart) * 1000000LL) / kvm_qpc_frequency();
#endif
		captureInFlight = 0;
		if (captureResult != 0)
		{
#ifdef _WINSERVICE
			if (!kvmConsoleMode)
			{
				ILibRemoteLogging_printf(gKVMRemoteLogging, ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_1, "KVM [SLAVE]: get_desktop_buffer() failed");
			}
#endif
			KVMDEBUG("get_desktop_buffer() failed, shutting down", (int)GetCurrentThreadId());
			g_shutdown = 1;
		}
		else
		{
			// While the screen is changing, start capturing the next frame now so it overlaps with encoding this one.
			if (lastSentTiles && kvm_prepare_capture(writeHandler, reserved, mouseMove, &sentHideCursor))
			{
				kvm_capture_start(mouseMove);
				captureInFlight = 1;
			}
			idleWait = frameInfo.idleWait;

			if (frameInfo.kind != KVM_FRAME_UNCHANGED && desktop != NULL)
			{
				// Drop the frame if the resolution or scaling changed after it was captured, or if the buffer is
				// smaller than what the tile code reads at this frame's color depth.
				PIXEL_SIZE = framePixelSize;
				dropFrame = (frameGeneration != gGeometryGeneration) || !desktop_buffer_fits(desktopsize, PIXEL_SIZE);
				aborted = 0;
				if (!dropFrame) { kvm_adapt_note_source(frameInfo.srcClass, frameInfo.kind == KVM_FRAME_FULL); kvm_adapt_frame_begin(); } // the first complete frame from another source of pixels is not motion

				// A partial frame lists the tiles that can have changed, the others are not checked.
				if (!dropFrame && frameInfo.kind == KVM_FRAME_PARTIAL && frameInfo.tileDirty != NULL)
				{
					for (row = 0; row < TILE_HEIGHT_COUNT; row++)
					{
						for (col = 0; col < TILE_WIDTH_COUNT; col++)
						{
							if (frameInfo.tileDirty[(row * TILE_WIDTH_COUNT) + col] == 0 && !KVM_TILE_DEFERRED(&tileInfo[row][col])) { tileInfo[row][col].flags = (char)TILE_DONT_SEND; } // WA-2b: a tile held back by the motion frame limit is looked at again
						}
					}
				}

				bmpInfo = get_bmp_info(TILE_WIDTH, TILE_HEIGHT);
				for (row = 0; !dropFrame && row < TILE_HEIGHT_COUNT; row++)
				{
					for (col = 0; col < TILE_WIDTH_COUNT; col++)
					{
						height = TILE_HEIGHT * row;
						width = TILE_WIDTH * col;

						while (!g_shutdown && (g_pause))
						{
							Sleep(50); /*printf(".");*/
						} // If the socket is in pause state, wait here. //ToDo: YLIAN!!!!

						if (g_shutdown || SCALING_FACTOR != SCALING_FACTOR_NEW)
						{
							height = SCALED_HEIGHT;
							width = SCALED_WIDTH;
							aborted = 1;
							break;
						}

						// Skip the tile if it has already been sent or if the CRC is same as before
						if (tileInfo[row][col].flags == (char)TILE_SENT || tileInfo[row][col].flags == (char)TILE_DONT_SEND)
						{
							continue;
						}

						if (get_tile_at(width, height, &buf, &tilesize, desktop, row, col) == 1)
						{
							// GetTileAt failed, lets not send the tile
							continue;
						}
						if (buf && !g_shutdown)
						{
#ifdef KVM_STATS_LOG
							LARGE_INTEGER w0, w1;
							QueryPerformanceCounter(&w0);
#endif
							sentTiles = 1;
							KVMDEBUG2("Writing JPEG: %llu bytes", tilesize);
							switch (writeHandler((char *)buf, (int)tilesize, reserved))
							{
							case ILibTransport_DoneState_INCOMPLETE:
								g_pause = 1;
								ILibRemoteLogging_printf(ILibChainGetLogger(gILibChain), ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_2, "Agent KVM: KVM PAUSE");
								KVMDEBUG2("JPEG WRITE resulted in PAUSE");
								break;
							case ILibTransport_DoneState_ERROR:
								g_shutdown = 1;
								height = SCALED_HEIGHT;
								width = SCALED_WIDTH;
								aborted = 1;
								KVMDEBUG2("JPEG WRITE resulted in ERROR");
								break;
							}
#ifdef KVM_STATS_LOG
							QueryPerformanceCounter(&w1);
							statTiles++;
							statBytes += tilesize;
							statWriteUs += ((w1.QuadPart - w0.QuadPart) * 1000000LL) / kvm_qpc_frequency();
							kvm_adapt_note_write((unsigned int)(((w1.QuadPart - w0.QuadPart) * 1000000LL) / kvm_qpc_frequency()), tilesize, 0);
#endif
							free(buf);
						}
						else if (buf)
						{
							free(buf); // The session is closing
						}
					}
				}

#ifdef KVM_STATS_LOG
				if (statTiles > 0) { gStats.jpegFrames++; }
#endif
				if (kvm_adapt_frame_deferred()) { sentTiles = 1; } // WA-2b: a change was held back: keep the loop pace and the overlap of the capture

#ifdef KVM_VIDEO_ENABLED
				// the tiles that moved wait in pendingVideo: one H.264 frame paints the sum of them (the window, the pace and the encoder decide whether it goes now)
				if (!dropFrame && !aborted && !g_shutdown)
				{
					kvm_video_note_capture((unsigned)((frameInfo.captureUs + frameInfo.outUs + 500u) / 1000u), (unsigned)frameInfo.dwmFrames); // what the capture of this image took (Auto's cost of the machine)
					kvm_video_emit(desktop, desktopsize);	// the picture is held and the frame handed to the encoder thread; it is written when it is done (kvm_glue_video_written)
					if (kvm_video_pending() > 0) { sentTiles = 1; } // a frame is still due (the window is full or the frame interval has not passed) or is in the encoder: keep the loop pace and the overlap of the capture
				}
#endif

				// WA-2: tiles that were sent with a lower quality while they moved and have been still for a while are sent again in full quality
				// (an ordinary picture command). Only after a frame that was processed to the end.
				if (!dropFrame && !aborted && !g_shutdown)
				{
					long long sharpenBudget = kvm_adapt_sharpen_budget();
					void *sharpBuf = NULL;
					long long sharpSize = 0;
					while (sharpenBudget > 0 && !g_shutdown && kvm_adapt_sharpen_next(desktop, sharpenBudget, &sharpBuf, &sharpSize))
					{
						int sharpResult;
#ifdef KVM_STATS_LOG
						LARGE_INTEGER sw0, sw1;
						QueryPerformanceCounter(&sw0);
#endif
						sharpResult = writeHandler((char *)sharpBuf, (int)sharpSize, reserved);
#ifdef KVM_STATS_LOG
						QueryPerformanceCounter(&sw1);
						statTiles++;
						statBytes += sharpSize;
						statWriteUs += ((sw1.QuadPart - sw0.QuadPart) * 1000000LL) / kvm_qpc_frequency();
						kvm_adapt_note_write((unsigned int)(((sw1.QuadPart - sw0.QuadPart) * 1000000LL) / kvm_qpc_frequency()), sharpSize, 1);
#endif
						free(sharpBuf);
						sharpBuf = NULL;
						sharpenBudget -= sharpSize;
						if (sharpResult == ILibTransport_DoneState_INCOMPLETE) { g_pause = 1; break; }
						if (sharpResult == ILibTransport_DoneState_ERROR) { g_shutdown = 1; break; }
					}
				}

				// A complete or partial frame that was not processed to the end (dropped, paused session closed, write error)
				// leaves changes that the CRC of the tiles does not know about yet. The next frame has to check every tile.
				if (dropFrame || aborted) { InterlockedExchange(&gKvmFullFrame, 1); }
			}

			KVMDEBUG("kvm_server_mainloop / loop2", (int)GetCurrentThreadId());

			if (desktop)
				KVM_BUF_FREE(desktop);
			desktop = NULL;
			desktopsize = 0;
			if (frameInfo.tileDirty)
				free(frameInfo.tileDirty);
			frameInfo.tileDirty = NULL;
		}

		KVMDEBUG("kvm_server_mainloop / loop3", (int)GetCurrentThreadId());
		lastSentTiles = sentTiles;
#ifdef KVM_STATS_LOG
		QueryPerformanceCounter(&lq2);
		loopProcUs = ((lq2.QuadPart - lq1.QuadPart) * 1000000LL) / kvm_qpc_frequency();
		kvm_stats_account(captureResult == 0 ? &frameInfo : NULL, statTiles, statBytes, statWriteUs);
		kvm_stats_flush(0);
#endif

#ifdef KVM_STREAM_ENABLED
		kvm_stream_iteration_end(); // rule M1 (position after an iteration that wrote) and the probe marker
#endif

		// We can't go full speed here, we need to slow this down.
		// When tiles were sent, the time spent capturing and encoding counts towards the frame rate timer.
		frameTimer = kvm_adapt_frame_pause(kvm_video_loop_period(FRAME_RATE_TIMER)); // WA-2: stretched while the link is congested. with Auto the step of Auto is the period (command 5 only asks for Medium as the reserve)
		height = frameTimer;
		if (sentTiles)
		{
			// the iterations are on a schedule (kvm_pace.h): the next one starts one period after the slot of this one, not one period after this one began (a wake that is late by 1 ms
			// every time would cost 6 % of the frames at 16 ms). An iteration that overran its slot starts the next at once.
			height = (int)kvm_tick_next(&gLoopTick, (uint32_t)frameStart, (uint32_t)timeGetTime(), (uint32_t)frameTimer);
#ifdef KVM_VIDEO_ENABLED
			{
				// A video frame that waits only for its slot: wake for it (the loop never sleeps longer than its own period, so the 240 / 241, a REFRESH and the JPEG of a typing echo are looked at
				// as often as before). When the slot comes sooner than the next iteration of the schedule the loop wakes early; kvm_tick_next does not count that iteration as a slot of the schedule.
				const int wake = kvm_video_wake_ms();
				if (wake > 0 && wake < height) { height = wake; }
			}
#endif
		}
		else
		{
			kvm_tick_idle(&gLoopTick); // nothing was sent: no schedule, no credit for the time that passes
		}
		if (idleWait)
		{
			height = 0; // The capture already waited for changes, no second pause
			kvm_tick_idle(&gLoopTick);
		}
#ifdef KVM_STATS_LOG
		QueryPerformanceCounter(&lq2);
#endif
		while (!g_shutdown && height > 0)
		{
#ifdef KVM_VIDEO_ENABLED
			HANDLE ev = (HANDLE)kvm_video_event();
			if (ev != NULL)
			{
				// the pause ends early when the encoder is done: the frame is written at once, the rest of the pause goes on (WAIT_IO_COMPLETION too: the time that went is counted)
				// (the deadline of the video, its slot or the write gap, is looked at again after every wait: a frame that the encoder just finished may leave the next one waiting for a few ms, and the loop wakes for them)
				const DWORD t0 = timeGetTime();
				const int wake = kvm_video_wake_ms();
				DWORD step = height > 50 ? 50 : (DWORD)height, el;
				if (wake > 0 && (DWORD)wake < step) { step = (DWORD)wake; }
				(void)WaitForSingleObjectEx(ev, step, TRUE);
				kvm_video_service();
				el = timeGetTime() - t0;
				height -= (int)(el > step ? step : el);
				SleepEx(0, TRUE);
				continue;
			}
#endif
			if (height > 50)
			{
				height -= 50;
				Sleep(50);
			}
			else
			{
				Sleep(height);
				height = 0;
			}
			SleepEx(0, TRUE);
		}
#ifdef KVM_STATS_LOG
		QueryPerformanceCounter(&lq3);
		kvm_adapt_note_loop((unsigned int)loopWaitUs, (unsigned int)loopProcUs, (unsigned int)(((lq3.QuadPart - lq2.QuadPart) * 1000000LL) / kvm_qpc_frequency()));
#endif
	}

	timeEndPeriod(1);
#ifdef KVM_STATS_LOG
	kvm_stats_flush(1);
#endif
	kvm_adapt_session_end();
#ifdef KVM_STREAM_ENABLED
	kvm_stream_session_end();
#endif

	// Stop the capture thread
	if (captureInFlight)
	{
		void *pending = NULL;
		kvm_frame_info pendingInfo;
		kvm_capture_wait(&pending, &desktopsize, &frameGeneration, &framePixelSize, &pendingInfo);
		if (pending != NULL) KVM_BUF_FREE(pending);
		if (pendingInfo.tileDirty != NULL) free(pendingInfo.tileDirty);
	}
	if (gCapture.thread != NULL)
	{
		gCapture.quit = 1;
		SetEvent(gCapture.startEvent);
		WaitForSingleObject(gCapture.thread, INFINITE);
		CloseHandle(gCapture.thread);
	}
	if (gCapture.startEvent != NULL) CloseHandle(gCapture.startEvent);
	if (gCapture.doneEvent != NULL) CloseHandle(gCapture.doneEvent);
	if (gCapture.desktop != NULL) CloseDesktop(gCapture.desktop);
	ZeroMemory(&gCapture, sizeof(gCapture));
#ifdef KVM_VIDEO_ENABLED
	kvm_fbuf_trim();
#endif

	KVMDEBUG("kvm_server_mainloop / end3", (int)GetCurrentThreadId());
	KVMDEBUG("kvm_server_mainloop / end2", (int)GetCurrentThreadId());

	if (tileInfo != NULL)
	{
		for (row = 0; row < TILE_HEIGHT_COUNT; row++)
			free(tileInfo[row]);
		free(tileInfo);
		tileInfo = NULL;
	}
	KVMDEBUG("kvm_server_mainloop / end1", (int)GetCurrentThreadId());
	teardown_gdiplus();

	KVMDEBUG("kvm_server_mainloop / end", (int)GetCurrentThreadId());

	KVM_UnInitMouseCursors();

	while ((tmoBuffer = ILibQueue_DeQueue(gPendingPackets)) != NULL)
	{
		ILibMemory_Free(tmoBuffer);
	}
	ILibQueue_Destroy(gPendingPackets);

	ILibRemoteLogging_printf(gKVMRemoteLogging, ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_1, "KVM [SLAVE]: Process Exiting...");

	ThreadRunning = 0;
	free(parm);
	return 0;
}

DWORD WINAPI kvm_server_mainloop(LPVOID parm)
{
	DWORD ret = 0;
	if (((int *)&(((void **)parm)[3]))[0] == 1)
	{
		// Enable Core Dump in KVM Child
		ILib_DumpEnabledContext winException;
		WCHAR str[_MAX_PATH];
		DWORD strLen;
		if ((strLen = GetModuleFileNameW(NULL, str, _MAX_PATH)) > 5)
		{
			str[strLen - 4] = 0;														  // We're going to convert .exe to _kvm.dmp
			g_ILibCrashDump_path = ILibMemory_Allocate((strLen * 2) + 10, 0, NULL, NULL); // Add enough space to add '.dmp' to the end of the path
			swprintf_s((wchar_t *)g_ILibCrashDump_path, strLen + 5, L"%s_kvm.dmp", str);
			ILibCriticalLogFilename = "KVMSlave.log";
		}

		__try
		{
			ret = kvm_server_mainloop_ex(parm);
		}
		__except (ILib_WindowsExceptionFilterEx(GetExceptionCode(), GetExceptionInformation(), &winException))
		{
			ILib_WindowsExceptionDebugEx(&winException);
		}
	}
	else
	{
		// Core Dump not enabled in KVM Child
		ret = kvm_server_mainloop_ex(parm);
	}
	return (ret);
}

#ifdef _WINSERVICE
void kvm_relay_ExitHandler(ILibProcessPipe_Process sender, int exitCode, void *user)
{
	ILibKVM_WriteHandler writeHandler = (ILibKVM_WriteHandler)((void **)user)[0];
	void *reserved = ((void **)user)[1];
	void *pipeMgr = ((void **)user)[2];
	char *exePath = (char *)((void **)user)[3];

	ILibRemoteLogging_printf(ILibChainGetLogger(gILibChain), ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_1, "Agent KVM: KVM Child Process(%d) [EXITED]", g_slavekvm);
	{
		const uint32_t childPid = (uint32_t)ILibProcessPipe_Process_GetPID(sender);
		const int gone = kvm_hwtest_child_gone(childPid, (uint32_t)exitCode);	// how the child ended decides what its marker "a try with the hardware encoder is under way" means (a crash is an error, the end of a session is not)
		if ((gone & 4) != 0) { ILibRemoteLogging_printf(ILibChainGetLogger(gILibChain), ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_1, "Agent KVM: kvm child pid=%u exit code=0x%08x hw_marker=%d counted=%d", childPid, (unsigned int)exitCode, (gone & 1) != 0 ? 1 : 0, (gone & 2) != 0 ? 1 : 0); }
	}
	UNREFERENCED_PARAMETER(exitCode);
	UNREFERENCED_PARAMETER(sender);

	if (g_restartcount < 4 && g_shutdown == 0)
	{
		kvm_relay_restart(1, pipeMgr, exePath, writeHandler, reserved);
	}
	else
	{
		ILibRemoteLogging_printf(ILibChainGetLogger(gILibChain), ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_1, "Agent KVM: g_restartcount = %d, aborting", g_restartcount);
		writeHandler(NULL, 0, reserved);
	}
}

void kvm_relay_StdOutHandler(ILibProcessPipe_Process sender, char *buffer, size_t bufferLen, size_t *bytesConsumed, void *user)
{
	unsigned short size = 0;
	UNREFERENCED_PARAMETER(sender);
	ILibKVM_WriteHandler writeHandler = (ILibKVM_WriteHandler)((void **)user)[0];
	void *reserved = ((void **)user)[1];

	if (bufferLen > 4)
	{
		if (ntohs(((unsigned short *)(buffer))[0]) > 1000)
		{
			KVMDEBUG2("Invalid KVM Command received: %u", ntohs(((unsigned short *)(buffer))[0]));
		}
		if (ntohs(((unsigned short *)(buffer))[0]) == (unsigned short)MNG_JUMBO)
		{
			if (bufferLen > 8)
			{
				if (bufferLen >= (size_t)(8 + (int)ntohl(((unsigned int *)(buffer))[1])))
				{
					*bytesConsumed = 8 + (int)ntohl(((unsigned int *)(buffer))[1]);
					KVMDEBUG2("Jumbo Packet received of size: %llu (bufferLen=%llu)", *bytesConsumed, bufferLen);

					writeHandler(buffer, (int)*bytesConsumed, reserved);
					return;
				}
			}
			KVMDEBUG2("Accumulate => JUMBO: [%llu bytes] bufferLen=%llu ", (size_t)(8 + (int)ntohl(((unsigned int *)(buffer))[1])), bufferLen);
		}
		else
		{
			size = ntohs(((unsigned short *)(buffer))[1]);
			if (size <= bufferLen)
			{
				KVMDEBUG2("KVM Command: [%u: %llu bytes]", ntohs(((unsigned short *)(buffer))[0]), size);
				*bytesConsumed = size;
				writeHandler(buffer, size, reserved);
				return;
			}
			KVMDEBUG2("Accumulate => KVM Command: [%u: %d bytes] bufferLen=%llu ", ntohs(((unsigned short *)(buffer))[0]), bufferLen);
		}
	}
	*bytesConsumed = 0;
}
void kvm_relay_StdErrHandler(ILibProcessPipe_Process sender, char *buffer, size_t bufferLen, size_t *bytesConsumed, void *user)
{
	KVMDebugLog *log = (KVMDebugLog *)buffer;

	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(user);

	if (bufferLen < sizeof(KVMDebugLog) || bufferLen < log->length)
	{
		*bytesConsumed = 0;
		return;
	}
	*bytesConsumed = log->length;
	// ILibRemoteLogging_printf(ILibChainGetLogger(gILibChain), (ILibRemoteLogging_Modules)log->logType, (ILibRemoteLogging_Flags)log->logFlags, "%s", log->logData);
	ILibRemoteLogging_printf(ILibChainGetLogger(gILibChain), ILibRemoteLogging_Modules_Microstack_Generic, (ILibRemoteLogging_Flags)log->logFlags, "%s", log->logData);
}

int kvm_relay_restart(int paused, void *pipeMgr, char *exePath, ILibKVM_WriteHandler writeHandler, void *reserved)
{
	char *parms0[] = {" -kvm0", g_ILibCrashDump_path != NULL ? "-coredump" : NULL, NULL, NULL};
	char *parms1[] = {" -kvm1", g_ILibCrashDump_path != NULL ? "-coredump" : NULL, NULL, NULL};
	void **user = (void **)ILibMemory_Allocate(4 * sizeof(void *), 0, NULL, NULL);

	if (parms0[1] == NULL)
	{
		parms0[1] = (gRemoteMouseRenderDefault != 0 ? "-remotecursor" : NULL);
		parms1[1] = (gRemoteMouseRenderDefault != 0 ? "-remotecursor" : NULL);
	}
	else
	{
		parms0[2] = (gRemoteMouseRenderDefault != 0 ? "-remotecursor" : NULL);
		parms1[2] = (gRemoteMouseRenderDefault != 0 ? "-remotecursor" : NULL);
	}

	user[0] = writeHandler;
	user[1] = reserved;
	user[2] = pipeMgr;
	user[3] = exePath;

	KVMDEBUG("kvm_relay_restart / start", paused);

	// If we are re-launching the child process, wait a bit. The computer may be switching desktop, etc.
	if (paused == 0)
		Sleep(500);
	if (gProcessSpawnType == ILibProcessPipe_SpawnTypes_SPECIFIED_USER && gProcessTSID < 0)
	{
		gProcessSpawnType = ILibProcessPipe_SpawnTypes_USER;
	}

	ILibRemoteLogging_printf(ILibChainGetLogger(gILibChain), ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_1, "KVM [Master]: Spawning Slave as %s", gProcessSpawnType == ILibProcessPipe_SpawnTypes_USER ? "USER" : "WIN_LOGON");
	gChildProcess = ILibProcessPipe_Manager_SpawnProcessEx3(pipeMgr, exePath, paused == 0 ? parms0 : parms1, gProcessSpawnType, (void *)(ULONG_PTR)gProcessTSID, 0);
	gProcessSpawnType = (gProcessSpawnType == ILibProcessPipe_SpawnTypes_SPECIFIED_USER || gProcessSpawnType == ILibProcessPipe_SpawnTypes_USER) ? ILibProcessPipe_SpawnTypes_WINLOGON : (gProcessTSID < 0 ? ILibProcessPipe_SpawnTypes_USER : ILibProcessPipe_SpawnTypes_SPECIFIED_USER);

	g_slavekvm = ILibProcessPipe_Process_GetPID(gChildProcess);
	char tmp[255];
	sprintf_s(tmp, sizeof(tmp), "Child KVM (pid: %d)", g_slavekvm);
	ILibProcessPipe_Process_ResetMetadata(gChildProcess, tmp);

	ILibProcessPipe_Process_AddHandlers(gChildProcess, 65535, &kvm_relay_ExitHandler, &kvm_relay_StdOutHandler, &kvm_relay_StdErrHandler, NULL, user);
#ifdef KVM_STREAM_ENABLED
	if (gRelayVeto.mask != 0)
	{
		// The new child starts without the veto of meshcore / the multiplexer (two viewers must stay on JPEG): op 5 with the remembered sources goes in before any other input
		kvm_ctrl c;
		unsigned char m[KVM_CTRL_SIZE];
		memset(&c, 0, sizeof(c));
		c.ver = 1; c.op = KVM_OP_VETO; c.flags = (uint8_t)(gRelayVeto.mask & KVM_VETO_ALL);
		kvm_stream_build_ctrl(m, &c);
		ILibProcessPipe_Process_WriteStdIn(gChildProcess, (char *)m, KVM_CTRL_SIZE, ILibTransport_MemoryOwnership_USER);
		ILibRemoteLogging_printf(ILibChainGetLogger(gILibChain), ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_1, "KVM [Master]: veto (sources 0x%x) given to the new child", (int)gRelayVeto.mask);
	}
	{
		// the last commands 5 (quality, scale, frame timer, budget) and 13 (frame timer) of the viewer: the new child starts with the defaults of the agent and the viewer does not repeat them
		unsigned char rb[2 * KVM_CTRL_SIZE];
		const size_t rl = kvm_veto_replay(&gRelayVeto, rb, sizeof(rb));
		if (rl > 0)
		{
			ILibProcessPipe_Process_WriteStdIn(gChildProcess, (char *)rb, (int)rl, ILibTransport_MemoryOwnership_USER);
			ILibRemoteLogging_printf(ILibChainGetLogger(gILibChain), ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_1, "KVM [Master]: settings of the viewer (%d bytes) given to the new child", (int)rl);
		}
	}
#endif

	KVMDEBUG("kvm_relay_restart() launched child process", g_slavekvm);

	// Run the relay
	g_shutdown = 0;
	KVMDEBUG("kvm_relay_restart / end", (int)(uintptr_t)kvmthread);

	return 1;
}
#endif

// Setup the KVM session. Return 1 if ok, 0 if it could not be setup.
int kvm_relay_setup(char *exePath, void *processPipeMgr, ILibKVM_WriteHandler writeHandler, void *reserved, int tsid)
{
	if (processPipeMgr != NULL)
	{
#ifdef _WINSERVICE
		if (ThreadRunning == 1 && g_shutdown == 0)
		{
			KVMDEBUG("kvm_relay_setup() session already exists", 0);
			return 0;
		}
		g_restartcount = 0;
#ifdef KVM_STREAM_ENABLED
		memset(&gRelayVeto, 0, sizeof(gRelayVeto)); // a new session: no veto yet (meshcore sets it when a second viewer joins)
#endif
		gProcessSpawnType = ILibProcessPipe_SpawnTypes_SPECIFIED_USER;
		gProcessTSID = tsid;
		KVMDEBUG("kvm_relay_setup() session starting", 0);
		return kvm_relay_restart(1, processPipeMgr, exePath, writeHandler, reserved);
#else
		return (0);
#endif
	}
	else
	{
		// if (kvmthread != NULL && g_shutdown == 0) return 0;
		void **parms = (void **)ILibMemory_Allocate((2 * sizeof(void *)) + sizeof(int), 0, NULL, NULL);
		parms[0] = writeHandler;
		parms[1] = reserved;
		((int *)(&parms[2]))[0] = 1;
		kvmConsoleMode = 1;
		memset(&gStdinParser, 0, sizeof(gStdinParser)); // a new input stream (console mode: kvm_relay_feeddata calls kvm_server_inputdata)

		if (ThreadRunning == 1 && g_shutdown == 0)
		{
			KVMDEBUG("kvm_relay_setup() session already exists", 0);
			free(parms);
			return 0;
		}
		kvmthread = CreateThread(NULL, 0, kvm_server_mainloop, (void *)parms, 0, 0);
		if (kvmthread != 0)
		{
			CloseHandle(kvmthread);
		}
		return 1;
	}
}

// Force a KVM reset & refresh
void kvm_relay_reset(ILibKVM_WriteHandler writeHandler, void *reserved)
{
	char buffer[4];
	KVMDEBUG("kvm_relay_reset", 0);
	((unsigned short *)buffer)[0] = (unsigned short)htons((unsigned short)MNG_KVM_REFRESH); // Write the type
	((unsigned short *)buffer)[1] = (unsigned short)htons((unsigned short)4);				// Write the size
	kvm_relay_feeddata(buffer, 4, writeHandler, reserved);
}

// Clean up the KVM session.
void kvm_cleanup()
{
	// ILIBMESSAGE("KVMBREAK-CLEAN\r\n");
	KVMDEBUG("kvm_cleanup", 0);
	g_shutdown = 1;
#ifdef KVM_STREAM_ENABLED
	memset(&gRelayVeto, 0, sizeof(gRelayVeto));
#endif
	if (gChildProcess != NULL)
	{
		ILibRemoteLogging_printf(ILibChainGetLogger(gILibChain), ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_1, "KVM.c/kvm_cleanup: Attempting to kill child process");
		kvm_hwtest_child_gone((uint32_t)ILibProcessPipe_Process_GetPID(gChildProcess), 1067u);	// the end of the session (SoftKill is TerminateProcess with 1067): the child's marker of a try is not a crash
		ILibProcessPipe_Process_SoftKill(gChildProcess);
		gChildProcess = NULL;
	}
	else
	{
		ILibRemoteLogging_printf(ILibChainGetLogger(gILibChain), ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_1, "KVM.c/kvm_cleanup: gChildProcess = NULL");
	}
}

#endif