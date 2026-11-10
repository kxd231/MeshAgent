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
HANDLE kvmthread = NULL;
int g_shutdown = 999;
int g_pause = 0;
int g_remotepause = 1;
int g_restartcount = 0;
struct tileInfo_t **tileInfo = NULL;
int g_slavekvm = 0;
static ILibProcessPipe_Process gChildProcess;
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

// Feed network data into the KVM. Return the number of bytes consumed.
// This method consumes a single command.
int kvm_server_inputdata(char *block, int blocklen, ILibKVM_WriteHandler writeHandler, void *reserved)
{
	unsigned short type, size;

	// Decode the block header
	if (blocklen < 4)
		return 0;

	ILibRemoteLogging_printf(gKVMRemoteLogging, ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_2, "KVM [SLAVE]: Handle Input [Len = %d]", blocklen);
	// KVMDEBUG("kvm_server_inputdata", blocklen);
	CheckDesktopSwitch(0, writeHandler, reserved);

	type = ntohs(((unsigned short *)(block))[0]);
	size = ntohs(((unsigned short *)(block))[1]);

	ILibRemoteLogging_printf(gKVMRemoteLogging, ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_2, "KVM [SLAVE]: Handle Input [Len = %d, type = %u, size = %u]", blocklen, type, size);

	if (size > blocklen)
		return 0;

	// printf("INPUT: %d, %d\r\n", type, size);

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
		if (size >= 10)
		{
			int fr = ((int)ntohs(((unsigned short *)(block + 8))[0]));
			if (fr >= 20 && fr <= 5000)
				FRAME_RATE_TIMER = fr;
		}
		if (size >= 8)
		{
			int ns = ((int)ntohs(((unsigned short *)(block + 6))[0]));
			if (ns >= 64 && ns <= 4096)
				SCALING_FACTOR_NEW = ns;
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
		int fr = ((int)ntohs(((unsigned short *)(block))[2]));
		if (fr >= 20 && fr <= 5000)
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

		if (block[4] == 1) // Version 1 touch structure (Very simple)
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
}

#define BUFSIZE 65535
#ifdef _WINSERVICE
DWORD WINAPI kvm_mainloopinput_ex(LPVOID Param)
{
	int ptr = 0;
	int ptr2 = 0;
	int len = 0;
	char pchRequest2[30000];
	BOOL fSuccess = FALSE;
	DWORD cbBytesRead = 0;
	ILibKVM_WriteHandler writeHandler = (ILibKVM_WriteHandler)((void **)Param)[0];
	void *reserved = ((void **)Param)[1];

	KVMDEBUG("kvm_mainloopinput / start", (int)GetCurrentThreadId());

	ILibRemoteLogging_printf(gKVMRemoteLogging, ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_1, "KVM [SLAVE]: (mainloopinput) Starting...");

	while (!g_shutdown)
	{
		fSuccess = ReadFile(hStdIn, pchRequest2 + len, 30000 - len, &cbBytesRead, NULL);
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
		// TODO: else move the reminder.
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
// Only written by the branch with the DXGI measurements; the file stops growing at KVM_STATS_LOG_MAX bytes.
static SRWLOCK gStatsLock = SRWLOCK_INIT;
static wchar_t gStatsPath[MAX_PATH + 16];
static int gStatsPathReady = 0;

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

void kvm_stats_log(const char *fmt, ...)
{
	char line[1024];
	SYSTEMTIME st;
	va_list ap;
	int n, m;
	HANDLE h;
	DWORD written;
	LARGE_INTEGER size;

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
			if (GetFileSizeEx(h, &size) && size.QuadPart < KVM_STATS_LOG_MAX) { WriteFile(h, line, (DWORD)n, &written, NULL); }
			CloseHandle(h);
		}
	}
	ReleaseSRWLockExclusive(&gStatsLock);
}

// Counters of the main loop, one line is written per KVM_STATS_PERIOD_MS.
typedef struct KVM_Stats
{
	DWORD sessionStart, periodStart;
	int frames, full, partial, unchanged, gdi, dxgi, tiles, moveFrames, captureFrames, copyFrames, outFrames, staleChecks, staleSuspects;
	long long bytes, writeUs, dirtyRects, dirtyArea, moveRects, moveArea, moveMaxArea, captureUs, copyUs, outUs, staleUs;
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
	if (fi->copyUs > 0) { gStats.copyUs += fi->copyUs; gStats.copyFrames++; }
	if (fi->outUs > 0) { gStats.outUs += fi->outUs; gStats.outFrames++; }
	if (fi->staleUs > 0) { gStats.staleUs += fi->staleUs; gStats.staleChecks++; }
	if (fi->staleSuspect > 0) { gStats.staleSuspects++; }
}

static void kvm_stats_flush(int final)
{
	DWORD now = timeGetTime();
	if (!final && (DWORD)(now - gStats.periodStart) < KVM_STATS_PERIOD_MS) { return; }
	if (gStats.frames > 0)
	{
		// "frames" are all loop iterations (a frame without changes counts, too); the rate of images that reached the tile check is
		// fps = (full + partial) / period. copy_us: GPU -> persistent image; out_us: making the output buffer (copy of the whole image
		// and cursor, or scaling); stale_*: the check of the DXGI image against GDI (KVM_STALE_QUIET_MS without changes, every period).
		unsigned long periodMs = (unsigned long)(now - gStats.periodStart);
		unsigned long fps10 = periodMs > 0 ? (unsigned long)(((long long)(gStats.full + gStats.partial) * 10000) / (long long)periodMs) : 0;
		kvm_stats_log("stats t=%lu period_ms=%lu path=%s frames=%d full=%d partial=%d unchanged=%d fps=%lu.%lu gdi=%d dxgi=%d tiles=%d bytes=%lld write_ms=%lld dirty_rects=%lld dirty_area=%lld move_rects=%lld move_area=%lld move_frames=%d move_max_area=%lld copy_us_avg=%lld out_us_avg=%lld capture_us_avg=%lld stale_checks=%d stale_suspects=%d stale_us_avg=%lld%s",
			(unsigned long)((now - gStats.sessionStart) / 1000), periodMs, (gStats.dxgi == 0 ? "gdi" : (gStats.gdi == 0 ? "dxgi" : "mixed")),
			gStats.frames, gStats.full, gStats.partial, gStats.unchanged, fps10 / 10, fps10 % 10, gStats.gdi, gStats.dxgi, gStats.tiles, gStats.bytes, gStats.writeUs / 1000,
			gStats.dirtyRects, gStats.dirtyArea, gStats.moveRects, gStats.moveArea, gStats.moveFrames, gStats.moveMaxArea,
			gStats.copyFrames > 0 ? gStats.copyUs / gStats.copyFrames : 0LL, gStats.outFrames > 0 ? gStats.outUs / gStats.outFrames : 0LL,
			gStats.captureFrames > 0 ? gStats.captureUs / gStats.captureFrames : 0LL,
			gStats.staleChecks, gStats.staleSuspects, gStats.staleChecks > 0 ? gStats.staleUs / gStats.staleChecks : 0LL,
			final ? " final=1" : "");
	}
	gStats.frames = gStats.full = gStats.partial = gStats.unchanged = gStats.gdi = gStats.dxgi = gStats.tiles = gStats.moveFrames = gStats.captureFrames = gStats.copyFrames = gStats.outFrames = gStats.staleChecks = gStats.staleSuspects = 0;
	gStats.bytes = gStats.writeUs = gStats.dirtyRects = gStats.dirtyArea = gStats.moveRects = gStats.moveArea = gStats.moveMaxArea = gStats.captureUs = gStats.copyUs = gStats.outUs = gStats.staleUs = 0;
	gStats.periodStart = now;
	kvm_adapt_stats_flush(final);
}

static void kvm_stats_start(void)
{
	char env[8] = "";
	int forceGdi;
	memset(&gStats, 0, sizeof(gStats));
	gStats.sessionStart = gStats.periodStart = timeGetTime();
	forceGdi = (GetEnvironmentVariableA("MESH_KVM_FORCE_GDI", env, sizeof(env)) > 0 && env[0] == '1') ? 1 : 0;
	kvm_stats_log("start build=\"%s %s\" dxgi=%d env_force_gdi=%d subsamp=%d screen=%d,%d,%d,%d scale=%d",
		__DATE__, __TIME__,
#ifdef KVM_DXGI_ENABLED
		1,
#else
		0,
#endif
		forceGdi, kvm_subsamp_value(), SCREEN_X, SCREEN_Y, SCREEN_WIDTH, SCREEN_HEIGHT, SCALING_FACTOR);
}
#endif

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

// Waits for the frame started by kvm_capture_start(). Returns 0 on success, then *desktop is the frame (NULL when
// info->kind is KVM_FRAME_UNCHANGED) and info->tileDirty belongs to the caller; any other value means the capture failed.
int kvm_capture_wait(void **desktop, long long *bufferSize, int *generation, int *pixelSize, kvm_frame_info *info)
{
	WaitForSingleObject(gCapture.doneEvent, INFINITE);
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

	Sleep(100); // Pausing here seems to fix connection issues, especially with WebRTC. TODO: Investigate why.
	KVMDEBUG("kvm_server_mainloop / start3", (int)GetCurrentThreadId());

	// Start the capture thread. It gets its own screen DC.
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
	while (!g_shutdown)
	{
		DWORD frameStart = timeGetTime();
		int sentTiles = 0;
#ifdef KVM_STATS_LOG
		int statTiles = 0;
		long long statBytes = 0, statWriteUs = 0;
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
		if (!captureInFlight)
		{
			if (!kvm_prepare_capture(writeHandler, reserved, mouseMove, &sentHideCursor))
				break;
			kvm_capture_start(mouseMove);
		}

		// Get the captured desktop
		desktop = NULL;
		captureResult = kvm_capture_wait(&desktop, &desktopsize, &frameGeneration, &framePixelSize, &frameInfo);
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
				if (!dropFrame) { kvm_adapt_frame_begin(); }

				// A partial frame lists the tiles that can have changed, the others are not checked.
				if (!dropFrame && frameInfo.kind == KVM_FRAME_PARTIAL && frameInfo.tileDirty != NULL)
				{
					for (row = 0; row < TILE_HEIGHT_COUNT; row++)
					{
						for (col = 0; col < TILE_WIDTH_COUNT; col++)
						{
							if (frameInfo.tileDirty[(row * TILE_WIDTH_COUNT) + col] == 0) { tileInfo[row][col].flags = (char)TILE_DONT_SEND; }
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
							kvm_adapt_note_write((unsigned int)(((w1.QuadPart - w0.QuadPart) * 1000000LL) / kvm_qpc_frequency()), tilesize);
#endif
							free(buf);
						}
						else if (buf)
						{
							free(buf); // The session is closing
						}
					}
				}

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
						kvm_adapt_note_write((unsigned int)(((sw1.QuadPart - sw0.QuadPart) * 1000000LL) / kvm_qpc_frequency()), sharpSize);
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
				free(desktop);
			desktop = NULL;
			desktopsize = 0;
			if (frameInfo.tileDirty)
				free(frameInfo.tileDirty);
			frameInfo.tileDirty = NULL;
		}

		KVMDEBUG("kvm_server_mainloop / loop3", (int)GetCurrentThreadId());
		lastSentTiles = sentTiles;
#ifdef KVM_STATS_LOG
		kvm_stats_account(captureResult == 0 ? &frameInfo : NULL, statTiles, statBytes, statWriteUs);
		kvm_stats_flush(0);
#endif

		// We can't go full speed here, we need to slow this down.
		// When tiles were sent, the time spent capturing and encoding counts towards the frame rate timer.
		frameTimer = kvm_adapt_frame_pause(FRAME_RATE_TIMER); // WA-2: stretched while the link is congested
		height = frameTimer;
		if (sentTiles)
		{
			DWORD elapsed = timeGetTime() - frameStart;
			height = (elapsed >= (DWORD)frameTimer) ? 0 : (frameTimer - (int)elapsed);
		}
		if (idleWait)
		{
			height = 0; // The capture already waited for changes, no second pause
		}
		while (!g_shutdown && height > 0)
		{
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
	}

	timeEndPeriod(1);
#ifdef KVM_STATS_LOG
	kvm_stats_flush(1);
#endif
	kvm_adapt_session_end();

	// Stop the capture thread
	if (captureInFlight)
	{
		void *pending = NULL;
		kvm_frame_info pendingInfo;
		kvm_capture_wait(&pending, &desktopsize, &frameGeneration, &framePixelSize, &pendingInfo);
		if (pending != NULL) free(pending);
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
	if (gChildProcess != NULL)
	{
		ILibRemoteLogging_printf(ILibChainGetLogger(gILibChain), ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_1, "KVM.c/kvm_cleanup: Attempting to kill child process");
		ILibProcessPipe_Process_SoftKill(gChildProcess);
		gChildProcess = NULL;
	}
	else
	{
		ILibRemoteLogging_printf(ILibChainGetLogger(gILibChain), ILibRemoteLogging_Modules_Agent_KVM, ILibRemoteLogging_Flags_VerbosityLevel_1, "KVM.c/kvm_cleanup: gChildProcess = NULL");
	}
}

#endif