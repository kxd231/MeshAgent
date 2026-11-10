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

#include <Windows.h>
#include <tchar.h>
#include "dxgi_capture.h" // KVM_STATS_LOG / KVM_DXGI_ENABLED, and the capture module interface on x64

#if defined(_cplus_plus) || defined(__cplusplus) || defined(_cplusplus)
extern "C"
{
#endif

struct tileInfo_t {
	int crc;
	char flags; // See TILE_FLAGS_ENUM
};

enum TILE_FLAGS_ENUM {
	TILE_TODO,				// The tile CRC needs to be calculated.
	TILE_SENT,				// CRC has been calculated and the tile has been sent.
	TILE_MARKED_NOT_SENT,	// CRC has been calculated, but the tile was not sent.
	TILE_DONT_SEND			// CRC has been calculated, tile need not be sent.
	//TILE_SKIPPED			// CRC has been calculated, tile need not be sent, but was skipped to include a greater region
};

// What get_desktop_buffer() returns for a frame.
enum KVM_FRAME_KIND_ENUM {
	KVM_FRAME_FULL = 0,		// The whole screen is in the buffer: every tile goes through the CRC check.
	KVM_FRAME_PARTIAL = 1,	// The whole screen is in the buffer, tileDirty lists the tiles that can have changed (the others are not checked).
	KVM_FRAME_UNCHANGED = 2	// Nothing changed since the previous frame: no buffer.
};
enum KVM_FRAME_SOURCE_ENUM { KVM_SOURCE_GDI = 0, KVM_SOURCE_DXGI = 1 };
enum KVM_DXGI_EVENT_ENUM { KVM_DXGI_EVENT_OPEN = 1, KVM_DXGI_EVENT_LOST = 2, KVM_DXGI_EVENT_FALLBACK = 4, KVM_DXGI_EVENT_DISABLED = 8 };
#define KVM_CAPTURE_FORCE_FULL 1	// get_desktop_buffer flag: the frame must be KVM_FRAME_FULL (refresh, new geometry, previous frame not fully processed)
#define KVM_CAPTURE_WANT_IMAGE 2	// get_desktop_buffer flag, reserved: an image even if nothing changed

typedef struct kvm_frame_info {
	int kind, source;				// KVM_FRAME_*_ENUM, KVM_SOURCE_*
	int idleWait;					// The capture already waited for changes: the main loop skips its frame pause
	unsigned char *tileDirty;		// KVM_FRAME_PARTIAL only: TILE_HEIGHT_COUNT x TILE_WIDTH_COUNT bytes (row 0 = top), owned by the caller (free())
	int dirtyRects, moveRects;		// Statistics of the DXGI frame
	long long dirtyArea, moveArea;
	unsigned int captureUs, copyUs;	// Time spent in get_desktop_buffer() (set by the capture thread) and in copying the changed rectangles from the GPU
	unsigned int outUs;				// Time spent making the output buffer (copy of the whole image and cursor, or scaling)
	unsigned int staleUs;			// Time of the check of the DXGI image against GDI done in this call (0 = none)
	int staleSuspect;				// That check found differences (1 = first time, 2 = second time in a row)
	int dxgiEvent;					// KVM_DXGI_EVENT_ENUM bits that happened while capturing this frame
} kvm_frame_info;

short initialize_gdiplus();
void teardown_gdiplus();
int get_tile_at(int x, int y, void** buffer, long long *bufferSize, void *desktop, int row, int col);
int get_desktop_buffer(void **buffer, long long *bufferSize, long*, int *pixelSize, int flags, kvm_frame_info *info);
int desktop_buffer_fits(long long bufferSize, int pixelSize);
BITMAPINFO get_bmp_info(int width, int height);
void set_tile_compression(int type, int level);
void reset_capture_bitmap();
void release_desktop_dc();
void switch_to_desktop_context();
#ifdef KVM_STATS_LOG
void kvm_stats_log(const char *fmt, ...);	// kvm.c: appends a line to <exe>-kvm.log
int kvm_subsamp_value(void);				// tile.cpp: JPEG chroma subsampling in use (TJSAMP_*), -1 without TurboJPEG
#endif
#ifdef KVM_DXGI_ENABLED
void kvm_dxgi_thread_end(void);				// tile.cpp: the capture thread ends, close the duplication
#endif

#if defined(_cplus_plus) || defined(__cplusplus) || defined(_cplusplus)
}
#endif

#endif
