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
#include "kvm_adapt.h"   // KVM_ADAPT_ENABLED and the WA-2 interface (stubs elsewhere)
#ifdef KVM_VIDEO_ENABLED
#include "../common/kvm_fbuf.h"
// the pictures of the capture come from a cache of buffers with a reference count (the encoder thread holds one too); the main loop gives them back with KVM_BUF_FREE
#define KVM_BUF_GET(n) kvm_fbuf_get((size_t)(n))
#define KVM_BUF_FREE(p) kvm_fbuf_release(p)
#else
#define KVM_BUF_GET(n) malloc((size_t)(n))
#define KVM_BUF_FREE(p) free(p)
#endif

#if defined(_cplus_plus) || defined(__cplusplus) || defined(_cplusplus)
extern "C"
{
#endif

struct tileInfo_t {
	int crc;
	char flags; // See TILE_FLAGS_ENUM
#ifdef KVM_ADAPT_ENABLED
	// WA-2 (kvm_adapt.cpp). crc means "what the viewer has" (0xFF after a refresh); contentCrc is the content last seen, a refresh does not touch it.
	unsigned char sentQ;		// JPEG quality of the last send, 0 = never sent, 100 = not lossy
	unsigned char sentSub;		// 1 = the last send was chroma subsampled below the normal subsampling
	unsigned char streak;		// changes of the content in a row, each at most KVM_ADAPT_MOTION_MS after the previous one
	unsigned char noted;		// low byte of the frame stamp: the content of this frame was already looked at
	unsigned char refresh;		// the tile is sent only because of a refresh / new geometry (the content did not change)
	unsigned char deferred;		// WA-2b: a change of this tile was held back by the motion frame limit; it has to be looked at again and sent
	unsigned int deferredAt;	// timeGetTime() of the first time it was held back (0 = not held back): held-back changes are served in this order
	int contentCrc;
	unsigned int lastChange;	// timeGetTime() of the last change of the content
#endif
};

#ifdef KVM_ADAPT_ENABLED
#define KVM_TILE_DEFERRED(t) ((t)->deferred != 0)
#else
#define KVM_TILE_DEFERRED(t) 0
#endif

// Initial state of a tile: the viewer has nothing yet (crc 0xFF), WA-2 state empty.
static __inline void kvm_tile_init(struct tileInfo_t *t)
{
	t->crc = 0xFF;
	t->flags = 0;
#ifdef KVM_ADAPT_ENABLED
	t->sentQ = 0; t->sentSub = 0; t->streak = 0; t->noted = 0; t->refresh = 0; t->deferred = 0; t->deferredAt = 0;
	t->contentCrc = 0x7FFFFFFF;
	t->lastChange = 0;
#endif
}

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
// the choice of the capture source (meshcore/KVM/common/kvm_capsrc.h). capMode: the state after this frame; capEvents: what happened while capturing it.
enum KVM_CAP_MODE_ENUM { KVM_CAP_MODE_DXGI = 0, KVM_CAP_MODE_GDI = 1, KVM_CAP_MODE_PROBE = 2 };
enum KVM_CAP_EVENT_ENUM { KVM_CAP_EV_TO_GDI = 1, KVM_CAP_EV_PROBE = 2, KVM_CAP_EV_RECOVERED = 4, KVM_CAP_EV_PROBE_FAILED = 8, KVM_CAP_EV_INCONCLUSIVE = 16 };
#define KVM_CAPTURE_FORCE_FULL 1	// get_desktop_buffer flag: the frame must be KVM_FRAME_FULL (refresh, new geometry, previous frame not fully processed)
#define KVM_CAPTURE_WANT_IMAGE 2	// get_desktop_buffer flag, reserved: an image even if nothing changed

typedef struct kvm_frame_info {
	int kind, source;				// KVM_FRAME_*_ENUM, KVM_SOURCE_*
	int srcClass;					// where the pixels of this frame come from: 0 = GDI (also the fill of the persistent image of DXGI from GDI), 1 = the duplication of DXGI
	int idleWait;					// The capture already waited for changes: the main loop skips its frame pause
	unsigned char *tileDirty;		// KVM_FRAME_PARTIAL only: TILE_HEIGHT_COUNT x TILE_WIDTH_COUNT bytes (row 0 = top), owned by the caller (free())
	int dirtyRects, moveRects;		// Statistics of the DXGI frame
	long long dirtyArea, moveArea;
	unsigned int captureUs, copyUs;	// Time spent in get_desktop_buffer() (set by the capture thread) and in copying the changed rectangles from the GPU
	unsigned int outUs;				// Time spent making the output buffer (copy of the whole image and cursor, or scaling)
	unsigned int staleUs;			// Time of the check of the DXGI image against GDI done in this call (0 = none)
	int staleSuspect;				// That check found differences (1 = first time, 2 = second time in a row)
	int dwmFrames;					// DXGI image: AccumulatedFrames (the frames of the DWM that this image stands for), 0 for other frames
	int dxgiEvent;					// KVM_DXGI_EVENT_ENUM bits that happened while capturing this frame
	int capMode, capEvents;			// KVM_CAP_MODE_ENUM after this frame, KVM_CAP_EVENT_ENUM bits that happened while capturing it
	int ptrEmbedded;				// DXGI frame: the system cursor is composed into the desktop image (software cursor) and was taken out again
	unsigned int ptrGdiUs;			// Software cursor: time of the GDI reads of the cursor field made for this frame, and how many (ptrGdiReads)
	int ptrGdiReads, ptrRestores, ptrMaskPx, ptrFound, ptrNotFound;	// found/not found: the cursor looked for in the rectangles of the frame
	unsigned int ptrSearchUs;		// time of that search	// GDI reads, restores of the pixels under the cursor from before the frame, pixels of the current cursor mask
} kvm_frame_info;

short initialize_gdiplus();
void teardown_gdiplus();
int get_tile_at(int x, int y, void** buffer, long long *bufferSize, void *desktop, int row, int col);
int get_desktop_buffer(void **buffer, long long *bufferSize, long*, int *pixelSize, int flags, kvm_frame_info *info);
int desktop_buffer_fits(long long bufferSize, int pixelSize);
BITMAPINFO get_bmp_info(int width, int height);
void set_tile_compression(int type, int level);
void reset_capture_bitmap();
long long kvm_dxgi_adapter_luid(void);	// the adapter (LUID, high << 32 | low) of the output that DXGI captures now, 0 = none known (GDI)
int kvm_dxgi_adapter_sig(unsigned *ven, unsigned *dev, unsigned *sub, unsigned *rev);	// the PCI ids of that adapter (1 = known)
int kvm_capture_is_dxgi(void);
int kvm_capture_state(void);			// 0 = not known yet, 1 = GDI is the source, 2 = DXGI is open and the source (the encoder is chosen when it is not 0)
int kvm_capture_file_gdi(void);			// the file <exe>-kvm-capture.txt says "gdi": the capture is GDI for this process like MESH_KVM_FORCE_GDI=1 (read at the start of the process; 0 where there is no DXGI capture)			// 1 while the picture comes from DXGI (an active output); 0 in GDI mode (closed lid) and in the shadow probe of DXGI
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
