/*
KVM (Windows): DXGI Desktop Duplication capture with dirty rectangles.

Thin C interface over IDXGIOutputDuplication, used by the capture thread in tile.cpp and by the
stand-alone check tool (tools/kvm_dxgi_check.cpp). This header and dxgi_capture.cpp must not include
microstack or agent headers (winsock conflicts, and the tool is built without the agent).

Rotated screens are not handled in this version (the capture falls back to GDI). For a later version,
rectangles come in texture coordinates (not rotated) for a desktop of size W x H:
  ROTATE90  dest = { W - r.bottom, r.left,      W - r.top,   r.right  }, pixel (tx, ty) -> (W - 1 - ty, tx)
  ROTATE180 dest = { W - r.right,  H - r.bottom, W - r.left,  H - r.top }
  ROTATE270 dest = { r.top,        H - r.right,  r.bottom,    H - r.left }
These formulas are not verified.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef KVM_DXGI_CAPTURE_H
#define KVM_DXGI_CAPTURE_H

// Statistics log: also in builds with DXGI switched off at compile time (/DKVM_DXGI_DISABLE), for A/B comparison.
#if defined(_LINKVM) && defined(KVM_DXGI) && defined(_M_X64)
#define KVM_STATS_LOG 1
#if !defined(KVM_DXGI_DISABLE)
#define KVM_DXGI_ENABLED 1
#endif
#endif

#ifdef KVM_STATS_LOG
#ifndef KVM_STALE_QUIET_MS
#define KVM_STALE_QUIET_MS 1000			// DXGI must have reported no change for this long before the screen is compared with GDI
#endif
#ifndef KVM_STATS_PERIOD_MS
#define KVM_STATS_PERIOD_MS 10000		// statistics line period
#endif
#ifndef KVM_STATS_LOG_MAX
#define KVM_STATS_LOG_MAX (1024 * 1024)	// no more lines are appended once the log file is this large
#endif
#endif

#ifdef KVM_DXGI_ENABLED

#include <windows.h>
#include <stddef.h>

struct pm_sprite; // dxgi_ptrmask.h (C++ only, included by the files that use it)

// Tunables (milliseconds unless noted). Can be overridden with /D on the compiler command line.
#ifndef KVM_DXGI_TIMEOUT_MS
#define KVM_DXGI_TIMEOUT_MS 50			// AcquireNextFrame wait when nothing is pending
#endif
#ifndef KVM_DXGI_FIRST_FRAME_MS
#define KVM_DXGI_FIRST_FRAME_MS 100		// wait for the first image after the duplication was created
#endif
#ifndef KVM_DXGI_LOST_DELAY_MS
#define KVM_DXGI_LOST_DELAY_MS 250		// delay before re-creating the duplication after ACCESS_LOST
#endif
#ifndef KVM_DXGI_RETRY_MS
#define KVM_DXGI_RETRY_MS 500			// delay between failed attempts to create the duplication
#endif
#ifndef KVM_DXGI_MAX_STRIKES
#define KVM_DXGI_MAX_STRIKES 3			// consecutive failures (default desktop) before DXGI is switched off for the process
#endif
#ifndef KVM_DXGI_BBOX_RECTS
#define KVM_DXGI_BBOX_RECTS 64			// more rectangles than this are copied from the GPU as one bounding box
#endif
#ifndef KVM_DXGI_POINTER_MARGIN
#define KVM_DXGI_POINTER_MARGIN 24		// pixels added on every side of the cursor bitmap when its field is taken from GDI (shadow, enlarged cursor)
#endif
#ifndef KVM_DXGI_POINTER_MIN_DIFF
#define KVM_DXGI_POINTER_MIN_DIFF 8		// differing pixels in the cursor field (DXGI image against GDI) that mean 'the cursor is part of the image'
#endif
#ifndef KVM_DXGI_POINTER_NEAR_PX
#define KVM_DXGI_POINTER_NEAR_PX 128		// a full match of the cursor within this distance of an expected place ends the search for the cursor in a frame (others are only compared when none is)
#endif
#ifndef KVM_DXGI_POINTER_QUIET_MS
#define KVM_DXGI_POINTER_QUIET_MS 150	// no frame touched the cursor field for this long: the pixels under the cursor are read from GDI and the mask is corrected
#endif
#ifndef KVM_DXGI_POINTER_REFRESH_MS
#define KVM_DXGI_POINTER_REFRESH_MS 500	// while frames keep touching the field, the pixels under the cursor are read from GDI at most this often
#endif
#ifndef KVM_DXGI_CLEAR_ALPHA
#define KVM_DXGI_CLEAR_ALPHA 1			// zero the 4th byte of every pixel, like GDI does (equal tile CRCs on both paths)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum dxgi_result {
	DXGI_CAP_OK = 0,          // init: duplication ready; acquire: new image, rectangles copied to dst
	DXGI_CAP_NO_CHANGE = 1,   // acquire: frame with only a cursor update (LastPresentTime == 0); dst untouched
	DXGI_CAP_TIMEOUT = 2,     // acquire: DXGI_ERROR_WAIT_TIMEOUT; dst untouched
	DXGI_CAP_LOST = 3,        // acquire: ACCESS_LOST / DEVICE_REMOVED / DEVICE_RESET; module already closed, call init again
	DXGI_CAP_ERROR = 4,       // init/acquire: any other error (dxgi_capture_last_hr()); module closed
	DXGI_CAP_NO_OUTPUT = 5,   // init: no attached output has DesktopCoordinates equal to the requested rectangle
	DXGI_CAP_ROTATED = 6,     // init: output is rotated
	DXGI_CAP_UNSUPPORTED = 7, // init: no d3d11/dxgi, no IDXGIOutput1 (Windows 7), device creation failed: permanent
	DXGI_CAP_TRANSIENT = 8    // init/acquire: E_ACCESSDENIED, SESSION_DISCONNECTED, NOT_CURRENTLY_AVAILABLE, mode size mismatch; module closed
} dxgi_result;

typedef struct dxgi_rect { int left, top, right, bottom; } dxgi_rect; // output pixels, right/bottom exclusive

typedef struct dxgi_frame {
	int fullCopy;             // 1: the whole texture was copied (wantFull, first frame after init, or no usable metadata)
	int rectCount;            // number of rectangles in rects (dirty + move destinations, clipped, empty ones dropped)
	const dxgi_rect *rects;   // owned by the module, valid until the next acquire/release/shutdown
	int dirtyCount; long long dirtyArea;   // GetFrameDirtyRects (statistics, before clipping)
	int moveCount;  long long moveArea;    // GetFrameMoveRects (statistics, destination areas, before clipping)
	int accumulatedFrames;
	unsigned int copyUs;      // time of CopySubresourceRegion + Map + copy into dst
	// Pointer fields of DXGI_OUTDUPL_FRAME_INFO, filled for every acquired frame (also NO_CHANGE), zero after a timeout.
	int ptrUpdate;            // LastMouseUpdateTime != 0: this frame carries a pointer update (position or shape)
	int ptrVisible;           // PointerPosition.Visible (meaningful when ptrUpdate)
	int ptrX, ptrY;           // PointerPosition.Position (meaningful when ptrUpdate)
	unsigned int ptrShapeSize;// PointerShapeBufferSize: a new pointer shape is delivered with this frame
} dxgi_frame;

typedef struct dxgi_info {
	wchar_t adapter[128]; wchar_t output[32];   // DXGI_ADAPTER_DESC1.Description, DXGI_OUTPUT_DESC.DeviceName
	int width, height, rotation, systemMemory;  // ModeDesc size, output rotation, DesktopImageInSystemMemory
	unsigned int vendorId, deviceId;
} dxgi_info;

// Creates a new factory, looks for the attached output whose DesktopCoordinates equal (x, y, x+w, y+h), creates a device on
// that output's adapter and the duplication. Closes any previous duplication first. info may be NULL.
dxgi_result dxgi_capture_init(int x, int y, int w, int h, dxgi_info *info);

// Releases the frame held from the previous call and waits up to timeoutMs for the next one. For a new image the changed
// rectangles (and move destinations), or everything when wantFull, are copied to dst. dstTop is the address of the top row,
// dstStride is negative for bottom-up buffers; dst holds at least w x h 32bpp pixels. The frame is held until the next call.
// wantFull without a new image (TIMEOUT / NO_CHANGE) copies nothing: the caller decides how to fill the buffer.
dxgi_result dxgi_capture_acquire(unsigned int timeoutMs, unsigned char *dstTop, ptrdiff_t dstStride, int wantFull, dxgi_frame *frame);

// Hook called by dxgi_capture_acquire after the rectangles of a new image are known and before anything is copied into dst: the caller can still
// read the old pixels there (the agent keeps the pixels under a software cursor). rects are the areas that will be written (one bounding box when
// more rectangles than the bounding-box threshold are copied; the whole output for fullCopy). NULL = no hook.
typedef void (*dxgi_precopy_fn)(int fullCopy, int rectCount, const dxgi_rect *rects);
void dxgi_capture_set_precopy(dxgi_precopy_fn fn);
void dxgi_capture_set_bbox_rects(int n); // Test hook: more rectangles than n are copied as one bounding box (default KVM_DXGI_BBOX_RECTS)
int dxgi_capture_bbox_rects(void);
int dxgi_capture_sysmem(void);             // 1 while frames are read from the mapped system memory surface (no GPU copy path)
HRESULT dxgi_capture_release_hr(void);    // result of the last ReleaseFrame (for the log)
// Counters since the process started: images copied from the system memory surface, through the staging texture (GPU path), and
// among those the images copied as one bounding box. Tells a test which copy paths really ran.
void dxgi_capture_counters(long long *sysmemImages, long long *stagingImages, long long *bboxImages);
void dxgi_capture_release(void);   // ReleaseFrame if a frame is held (idempotent)
void dxgi_capture_shutdown(void);  // release + free all COM objects; the DLLs stay loaded
HRESULT dxgi_capture_last_hr(void);
const char *dxgi_result_name(dxgi_result r);

// Helpers without DXGI state, any thread (also used by the check tool).
// Marks the tiles touched by the rectangle (l, t, r, b), given in source pixels, in map (tilesX x tilesY, row 0 = top).
// srcW x srcH is the visible source size, dstW x dstH the visible destination size (equal when not scaling).
void kvm_mark_rect_tiles(unsigned char *map, int tilesX, int tilesY, int tileW, int tileH,
                         int l, int t, int r, int b, int srcW, int srcH, int dstW, int dstH);
// Counts the pixels whose bytes 0..2 differ between two top-down addressed images of w x h pixels (strides in bytes, negative for
// bottom-up buffers). Pixels inside 'skip' (may be NULL) are not compared.
long long kvm_count_pixel_diff(const unsigned char *aTop, ptrdiff_t aStride, const unsigned char *bTop, ptrdiff_t bStride, int w, int h, const dxgi_rect *skip);
// XORs the current mouse cursor into dst at (x, y), the way the GDI path draws it. covered gets the touched rectangle (may be empty).
// Returns 1 if something was drawn.
int kvm_cursor_xor(unsigned char *dstTop, ptrdiff_t dstStride, int dstW, int dstH, int x, int y, dxgi_rect *covered);
// The field of the system cursor: bitmap of the current cursor placed at its hotspot, plus 'margin' pixels on every side, in screen
// coordinates (not clipped). Returns 1 when the cursor is showing (CURSOR_SHOWING) and has a handle, 0 otherwise (r is then empty).
// hcur / pos get the handle and the hotspot position (ptScreenPos) when not NULL. Used by the agent and by the check tool.
int kvm_cursor_field(int margin, dxgi_rect *r, HCURSOR *hcur, POINT *pos);
// The fully opaque pixels of the cursor with this handle (colour cursors), see pm_sprite. Returns 1 when sp is filled (free it with pm_sprite_free),
// 0 for monochrome cursors (sp->monochrome = 1, nothing allocated) and on errors.
int kvm_cursor_sprite(HCURSOR hc, struct pm_sprite *sp);

#ifdef __cplusplus
}
#endif

#endif // KVM_DXGI_ENABLED
#endif // KVM_DXGI_CAPTURE_H
