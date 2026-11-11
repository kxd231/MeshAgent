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

#include <stdio.h>
#include <math.h>
#include "tile.h"
#include <gdiplus.h>
#include "meshcore/meshdefines.h"
#ifdef _TURBOJPEG
#include <turbojpeg.h>
#endif
using namespace Gdiplus;

#if defined(WIN32) && !defined(_WIN32_WCE) && !defined(_MINCORE)
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif

// #define KVMDEBUGENABLED 1

#ifdef KVMDEBUGENABLED
extern "C"
{
extern void KvmCriticalLog(const char* msg, const char* file, int line, int user1, int user2);
#define KVMDEBUG(m,u) { KvmCriticalLog(m, __FILE__, __LINE__, u, GetLastError()); printf("TVMMSG: %s (%d,%d).\r\n", m, (int)u, (int)GetLastError()); }
#define KVMDEBUG2(x) x
}
#else
#define KVMDEBUG(m, u)
#define KVMDEBUG2(x)
#endif

extern "C"
{
#include "microstack/ILibCrypto.h"
extern int TILE_WIDTH;
extern int TILE_HEIGHT;
extern int SCREEN_WIDTH;
extern int SCREEN_HEIGHT;
extern int SCREEN_X;
extern int SCREEN_Y;
extern int SCALED_WIDTH;
extern int SCALED_HEIGHT;
extern int PIXEL_SIZE;
extern int TILE_WIDTH_COUNT;
extern int TILE_HEIGHT_COUNT;
extern int COMPRESSION_RATIO;
extern int SCALING_FACTOR;
extern int SCALING_FACTOR_NEW;
extern int FRAME_RATE_TIMER;
extern tileInfo_t **tileInfo;
}

// Used with setting up a GDI+ session.
GdiplusStartupInput gdiplusStartupInput;
ULONG_PTR gdiplusToken;
HDC hDesktopDC;
HDC hCaptureDC;
HBITMAP hCapturedBitmap;
int hCapturedBitmapWidth = 0;	// Size of hCapturedBitmap, it is only re-created when this changes.
int hCapturedBitmapHeight = 0;
HGDIOBJ hCaptureDCDefaultBitmap = NULL;	// Original bitmap of hCaptureDC, a bitmap can't be deleted while it is selected into a DC.
//HDC hdc;
CLSID encoderClsid;
ULONG encCompression = 50; // Image compression
int encType = 1; // 1 = JPEG, 2 = PNG, 3 = TIFF, 4 = WEBP (encoded as JPEG)
EncoderParameters encParam;
LPVOID tilebuffer = NULL;
unsigned int tilebuffersize = 0;

#ifdef _TURBOJPEG
// JPEG chroma subsampling used by TurboJPEG. TJSAMP_444 keeps colored text sharp, TJSAMP_420 gives smaller tiles.
#ifndef KVM_TJ_SUBSAMP
#define KVM_TJ_SUBSAMP TJSAMP_444
#endif
tjhandle tjCompressor = NULL;
#endif

// Used to obtain the GUID for the image encoder.
int GetEncoderClsid(const WCHAR* format, CLSID* pClsid)
{
	unsigned int num = 0, size = 0;
	ImageCodecInfo* pImageCodecInfo = NULL;

	GetImageEncodersSize(&num, &size);
	if (size == 0) return -1;

	if ((pImageCodecInfo = (ImageCodecInfo*)(malloc(size))) == NULL) return -1;
	GetImageEncoders(num, size, pImageCodecInfo);

	for (unsigned int j = 0; j < num; ++j)
	{
		if (wcsncmp(pImageCodecInfo[j].MimeType, format, size) == 0)
		{
			*pClsid = pImageCodecInfo[j].Clsid;
			free(pImageCodecInfo);
			return j;
		}
	}

	free(pImageCodecInfo);
	return -1;
}

// When scaling, the screen is captured at full resolution into a DIB section and downscaled on the CPU
// with an area-average filter (similar quality to HALFTONE, but much faster than StretchBlt).
// The visible screen area (SCREEN_WIDTH x SCREEN_HEIGHT) is mapped to the visible scaled area
// (SCALED_WIDTH x SCALED_HEIGHT), the padding up to the tile size is black.
#if defined(_M_X64) || defined(_M_IX86)
#include <emmintrin.h>
#define KVM_SSE2 1
#endif
typedef struct scale_span { int start; int count; } scale_span;
HDC hFullDC = NULL;
HBITMAP hFullBitmap = NULL;
HGDIOBJ hFullDCDefaultBitmap = NULL;
unsigned char *fullBits = NULL;
int fullWidth = 0, fullHeight = 0;
scale_span *scaleXSpan = NULL, *scaleYSpan = NULL;
unsigned short *scaleXW = NULL, *scaleYW = NULL;
unsigned int *scaleAcc = NULL;
unsigned char *scaleTmp = NULL;
int scaleKX = 0, scaleKY = 0;
int scaleSrcW = 0, scaleSrcH = 0, scaleDstW = 0, scaleDstH = 0;
volatile LONG captureResetPending = 0;	// Set by reset_capture_bitmap() (any thread), handled by the capture thread.

// StretchBlt() mode used when scaling. HALFTONE gives better text quality, COLORONCOLOR is much faster.
#ifndef KVM_STRETCH_MODE
#define KVM_STRETCH_MODE HALFTONE
#endif

// Adjusts the screen size(width or height) to be exactly divisible by TILE_WIDTH
int adjust_screen_size(int pixles) 
{
	int extra = pixles % TILE_WIDTH; // Assuming tile width and height will remain the same.
	if (extra != 0) return pixles + TILE_WIDTH - extra;
	return pixles;
}

// Builds the area-average weights (sum = 16384) mapping src pixels to dst pixels along one axis.
int build_scale_table(int src, int dst, scale_span **spans, unsigned short **weights, int *k)
{
	double s = (double)src / (double)dst;
	int kk = (int)ceil(s) + 1;
	scale_span *sp = (scale_span*)malloc(sizeof(scale_span) * dst);
	unsigned short *w = (unsigned short*)malloc(sizeof(unsigned short) * dst * kk);
	if (sp == NULL || w == NULL) { if (sp) free(sp); if (w) free(w); return 1; }

	for (int i = 0; i < dst; i++)
	{
		double a = i * s, b = (i + 1) * s;
		int first = (int)floor(a), last = (int)ceil(b) - 1, total = 0, maxj = 0;
		if (last > src - 1) last = src - 1;
		if (last - first + 1 > kk) last = first + kk - 1;
		sp[i].start = first;
		sp[i].count = last - first + 1;
		for (int j = 0; j < sp[i].count; j++)
		{
			double lo = (a > first + j) ? a : (double)(first + j);
			double hi = (b < first + j + 1) ? b : (double)(first + j + 1);
			int wt = (int)(((hi - lo) / s) * 16384.0 + 0.5);
			w[i * kk + j] = (unsigned short)wt;
			total += wt;
			if (wt > w[i * kk + maxj]) maxj = j;
		}
		w[i * kk + maxj] = (unsigned short)(w[i * kk + maxj] + (16384 - total)); // Rounding error goes to the largest weight
	}

	if (*spans) free(*spans);
	if (*weights) free(*weights);
	*spans = sp;
	*weights = w;
	*k = kk;
	return 0;
}

void free_scale_tables()
{
	if (scaleXSpan) free(scaleXSpan);
	if (scaleYSpan) free(scaleYSpan);
	if (scaleXW) free(scaleXW);
	if (scaleYW) free(scaleYW);
	if (scaleAcc) free(scaleAcc);
	if (scaleTmp) free(scaleTmp);
	scaleXSpan = scaleYSpan = NULL;
	scaleXW = scaleYW = NULL;
	scaleAcc = NULL;
	scaleTmp = NULL;
	scaleSrcW = scaleSrcH = scaleDstW = scaleDstH = 0;
}

// The scalers below work on top-down rows: srcTop/dstTop point to the top row and the strides are negative for
// bottom-up DIBs. Pixels are 32bpp BGRX.
#pragma optimize("t", on)

// Generic area-average downscale using the tables built by build_scale_table().
void scale_area(const unsigned char *srcTop, ptrdiff_t srcStride, int srcW, unsigned char *dstTop, ptrdiff_t dstStride, int dstW, int dstH)
{
	int rowBytes = srcW * 4;
	for (int y = 0; y < dstH; y++)
	{
		const scale_span *ys = &scaleYSpan[y];
		const unsigned short *yw = &scaleYW[y * scaleKY];
		unsigned char *out = dstTop + (y * dstStride);

		// Vertical pass: weighted sum of the source rows (fits in 22 bits)
		const unsigned char *row = srcTop + (ys->start * srcStride);
		unsigned int w = yw[0];
		for (int i = 0; i < rowBytes; i++) { scaleAcc[i] = row[i] * w; }
		for (int j = 1; j < ys->count; j++)
		{
			row = srcTop + ((ys->start + j) * srcStride);
			w = yw[j];
			for (int i = 0; i < rowBytes; i++) { scaleAcc[i] += row[i] * w; }
		}

		// Horizontal pass
		for (int x = 0; x < dstW; x++)
		{
			const scale_span *xs = &scaleXSpan[x];
			const unsigned short *xw = &scaleXW[x * scaleKX];
			const unsigned int *p = scaleAcc + (xs->start * 4);
			unsigned int b = 0, g = 0, r = 0;
			for (int j = 0; j < xs->count; j++, p += 4)
			{
				w = xw[j];
				b += (p[0] >> 6) * w;
				g += (p[1] >> 6) * w;
				r += (p[2] >> 6) * w;
			}
			out[0] = (unsigned char)((b + (1 << 21)) >> 22);
			out[1] = (unsigned char)((g + (1 << 21)) >> 22);
			out[2] = (unsigned char)((r + (1 << 21)) >> 22);
			out[3] = 0;
			out += 4;
		}
	}
}

// Exact 2:1 downscale (average of 2x2 pixels).
void scale_half(const unsigned char *srcTop, ptrdiff_t srcStride, unsigned char *dstTop, ptrdiff_t dstStride, int dstW, int dstH)
{
	for (int y = 0; y < dstH; y++)
	{
		const unsigned char *r0 = srcTop + ((2 * y) * srcStride);
		const unsigned char *r1 = r0 + srcStride;
		unsigned char *out = dstTop + (y * dstStride);
		int x = 0;
#ifdef KVM_SSE2
		const __m128i zero = _mm_setzero_si128(), two = _mm_set1_epi16(2);
		for (; x + 4 <= dstW; x += 4) // 8 source pixels per row -> 4 pixels
		{
			__m128i a0 = _mm_loadu_si128((const __m128i*)(r0 + (x * 8)));
			__m128i a1 = _mm_loadu_si128((const __m128i*)(r0 + (x * 8) + 16));
			__m128i b0 = _mm_loadu_si128((const __m128i*)(r1 + (x * 8)));
			__m128i b1 = _mm_loadu_si128((const __m128i*)(r1 + (x * 8) + 16));
			__m128i s0 = _mm_add_epi16(_mm_unpacklo_epi8(a0, zero), _mm_unpacklo_epi8(b0, zero)); // pixels 0,1
			__m128i s1 = _mm_add_epi16(_mm_unpackhi_epi8(a0, zero), _mm_unpackhi_epi8(b0, zero)); // pixels 2,3
			__m128i s2 = _mm_add_epi16(_mm_unpacklo_epi8(a1, zero), _mm_unpacklo_epi8(b1, zero)); // pixels 4,5
			__m128i s3 = _mm_add_epi16(_mm_unpackhi_epi8(a1, zero), _mm_unpackhi_epi8(b1, zero)); // pixels 6,7
			__m128i h0 = _mm_add_epi16(_mm_unpacklo_epi64(s0, s1), _mm_unpackhi_epi64(s0, s1));     // 0+1, 2+3
			__m128i h1 = _mm_add_epi16(_mm_unpacklo_epi64(s2, s3), _mm_unpackhi_epi64(s2, s3));     // 4+5, 6+7
			h0 = _mm_srli_epi16(_mm_add_epi16(h0, two), 2);
			h1 = _mm_srli_epi16(_mm_add_epi16(h1, two), 2);
			_mm_storeu_si128((__m128i*)(out + (x * 4)), _mm_packus_epi16(h0, h1));
		}
#endif
		for (; x < dstW; x++)
		{
			for (int c = 0; c < 4; c++)
			{
				out[(x * 4) + c] = (unsigned char)((r0[(x * 8) + c] + r0[(x * 8) + 4 + c] + r1[(x * 8) + c] + r1[(x * 8) + 4 + c] + 2) >> 2);
			}
		}
	}
}

#ifdef KVM_SSE2
// 4:3 downscale (75%), area-average weights 3/4+1/4, 1/2+1/2, 1/4+3/4 computed with byte averages (error <= 1).
// Vertical: 4 source rows -> 3 rows in scaleTmp, then horizontal: 4 pixels -> 3 pixels.
void scale_three_quarter(const unsigned char *srcTop, ptrdiff_t srcStride, int srcW, unsigned char *dstTop, ptrdiff_t dstStride, int dstW, int dstH)
{
	int rowBytes = srcW * 4;
	unsigned char *t0 = scaleTmp, *t1 = scaleTmp + rowBytes, *t2 = scaleTmp + (2 * rowBytes);
	const __m128i lane1 = _mm_set_epi32(0, 0, -1, 0);

	for (int g = 0; g < dstH / 3; g++)
	{
		const unsigned char *a = srcTop + ((4 * g) * srcStride);
		const unsigned char *b = a + srcStride, *c = b + srcStride, *d = c + srcStride;
		int i = 0;
		for (; i + 16 <= rowBytes; i += 16)
		{
			__m128i va = _mm_loadu_si128((const __m128i*)(a + i)), vb = _mm_loadu_si128((const __m128i*)(b + i));
			__m128i vc = _mm_loadu_si128((const __m128i*)(c + i)), vd = _mm_loadu_si128((const __m128i*)(d + i));
			_mm_storeu_si128((__m128i*)(t0 + i), _mm_avg_epu8(va, _mm_avg_epu8(va, vb)));
			_mm_storeu_si128((__m128i*)(t1 + i), _mm_avg_epu8(vb, vc));
			_mm_storeu_si128((__m128i*)(t2 + i), _mm_avg_epu8(vd, _mm_avg_epu8(vc, vd)));
		}
		for (; i < rowBytes; i++)
		{
			t0[i] = (unsigned char)((3 * a[i] + b[i] + 2) >> 2);
			t1[i] = (unsigned char)((b[i] + c[i] + 1) >> 1);
			t2[i] = (unsigned char)((c[i] + 3 * d[i] + 2) >> 2);
		}

		for (int r = 0; r < 3; r++)
		{
			const unsigned char *t = (r == 0) ? t0 : ((r == 1) ? t1 : t2);
			unsigned char *out = dstTop + (((3 * g) + r) * dstStride);
			for (int x = 0; x + 3 <= dstW; x += 3) // pixels 4n..4n+3 -> 3n..3n+2
			{
				__m128i v = _mm_loadu_si128((const __m128i*)(t + ((x / 3) * 16)));        // p0 p1 p2 p3
				__m128i p = _mm_shuffle_epi32(v, _MM_SHUFFLE(3, 3, 1, 0));                 // p0 p1 p3 p3
				__m128i q = _mm_shuffle_epi32(v, _MM_SHUFFLE(2, 2, 2, 1));                 // p1 p2 p2 p2
				__m128i m = _mm_avg_epu8(p, q);                                             // p0p1 p1p2 p2p3 p2p3
				__m128i e = _mm_shuffle_epi32(v, _MM_SHUFFLE(3, 3, 0, 0));                 // p0 p0 p3 p3
				e = _mm_or_si128(_mm_andnot_si128(lane1, e), _mm_and_si128(lane1, m));      // p0 p1p2 p3
				__m128i res = _mm_avg_epu8(e, m);
				_mm_storel_epi64((__m128i*)(out + (x * 4)), res);
				*(int*)(out + (x * 4) + 8) = _mm_cvtsi128_si32(_mm_srli_si128(res, 8));
			}
		}
	}
}
#endif
#pragma optimize("", on)

void free_full_capture()
{
	if (hFullBitmap)
	{
		if (hFullDCDefaultBitmap) SelectObject(hFullDC, hFullDCDefaultBitmap);
		DeleteObject(hFullBitmap);
	}
	hFullBitmap = NULL;
	hFullDCDefaultBitmap = NULL;
	fullBits = NULL;
	fullWidth = fullHeight = 0;
}

#ifdef KVM_DXGI_ENABLED
static int fullCaptureGen = 0;	// Incremented whenever the full resolution DIB section is created (its contents are gone)
#endif

// Makes sure the full resolution DIB section exists and has the padded screen size sw x sh. Returns 0 on success.
static int ensure_full_capture(int sw, int sh)
{
	if (hFullBitmap == NULL || fullWidth != sw || fullHeight != sh)
	{
		BITMAPINFO bi;
		void *bits = NULL;
		free_full_capture();
		if (hFullDC == NULL && (hFullDC = CreateCompatibleDC(hDesktopDC)) == NULL) { KVMDEBUG("CreateCompatibleDC() failed", 0); return 1; }
		ZeroMemory(&bi, sizeof(BITMAPINFO));
		bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bi.bmiHeader.biWidth = sw;
		bi.bmiHeader.biHeight = sh; // Bottom-up, same as the GetDIBits() output
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;
		if ((hFullBitmap = CreateDIBSection(hDesktopDC, &bi, DIB_RGB_COLORS, &bits, NULL, 0)) == NULL || bits == NULL) { KVMDEBUG("CreateDIBSection() failed", 0); free_full_capture(); return 1; }
		fullBits = (unsigned char*)bits;
		hFullDCDefaultBitmap = SelectObject(hFullDC, hFullBitmap);
		fullWidth = sw;
		fullHeight = sh;
#ifdef KVM_DXGI_ENABLED
		fullCaptureGen++;
#endif
	}
	return 0;
}

// Copies the screen rectangle into the full resolution DIB section with GDI. Returns 0 on success.
static int capture_full_gdi()
{
	if (BitBlt(hFullDC, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, hDesktopDC, SCREEN_X, SCREEN_Y, SRCCOPY) == FALSE) { KVMDEBUG("BitBlt() returned FALSE", 0); return 1; }
	GdiFlush();
	return 0;
}

// Downscales the full resolution DIB section into a new buffer (*buffer, owned by the caller). Returns 0 on success.
static int scale_full_to_buffer(void **buffer, long long *bufferSize, int *pixelSize)
{
	int sw = adjust_screen_size(SCREEN_WIDTH), sh = adjust_screen_size(SCREEN_HEIGHT);
	int dw = adjust_screen_size(SCALED_WIDTH), dh = adjust_screen_size(SCALED_HEIGHT);
	int mode;

	// Pick the scaler: exact 2:1, exact 4:3 (SSE2 only) or the generic area-average filter.
	if (SCREEN_WIDTH == 2 * SCALED_WIDTH && SCREEN_HEIGHT == 2 * SCALED_HEIGHT) { mode = 2; }
#ifdef KVM_SSE2
	else if (SCREEN_WIDTH * 3 == SCALED_WIDTH * 4 && SCREEN_HEIGHT * 3 == SCALED_HEIGHT * 4) { mode = 3; }
#endif
	else { mode = 1; }

	if (scaleSrcW != SCREEN_WIDTH || scaleSrcH != SCREEN_HEIGHT || scaleDstW != SCALED_WIDTH || scaleDstH != SCALED_HEIGHT)
	{
		free_scale_tables();
		if ((scaleAcc = (unsigned int*)malloc(sizeof(unsigned int) * SCREEN_WIDTH * 4)) == NULL ||
			(scaleTmp = (unsigned char*)malloc((size_t)SCREEN_WIDTH * 4 * 3 + 16)) == NULL ||
			build_scale_table(SCREEN_WIDTH, SCALED_WIDTH, &scaleXSpan, &scaleXW, &scaleKX) != 0 ||
			build_scale_table(SCREEN_HEIGHT, SCALED_HEIGHT, &scaleYSpan, &scaleYW, &scaleKY) != 0)
		{
			free_scale_tables();
			return 1;
		}
		scaleSrcW = SCREEN_WIDTH; scaleSrcH = SCREEN_HEIGHT; scaleDstW = SCALED_WIDTH; scaleDstH = SCALED_HEIGHT;
	}

	*bufferSize = (long long)dw * dh * 4;
	if ((*buffer = malloc((size_t)*bufferSize)) == NULL) { KVMDEBUG("malloc() failed", 0); *bufferSize = 0; return 1; }
	*pixelSize = 4;

	{
		// Both buffers are bottom-up: the top row is the last one in memory.
		const unsigned char *srcTop = fullBits + ((size_t)(sh - 1) * sw * 4);
		unsigned char *dstTop = (unsigned char*)*buffer + ((size_t)(dh - 1) * dw * 4);
		ptrdiff_t srcStride = -(ptrdiff_t)sw * 4, dstStride = -(ptrdiff_t)dw * 4;

		switch (mode)
		{
			case 2: scale_half(srcTop, srcStride, dstTop, dstStride, SCALED_WIDTH, SCALED_HEIGHT); break;
#ifdef KVM_SSE2
			case 3: scale_three_quarter(srcTop, srcStride, SCREEN_WIDTH, dstTop, dstStride, SCALED_WIDTH, SCALED_HEIGHT); break;
#endif
			default: scale_area(srcTop, srcStride, SCREEN_WIDTH, dstTop, dstStride, SCALED_WIDTH, SCALED_HEIGHT); break;
		}

		// Black padding: right of the visible area, and the rows below it (the first rows in memory).
		if (dw > SCALED_WIDTH)
		{
			for (int y = 0; y < SCALED_HEIGHT; y++) { memset(dstTop + (y * dstStride) + (SCALED_WIDTH * 4), 0, (size_t)(dw - SCALED_WIDTH) * 4); }
		}
		if (dh > SCALED_HEIGHT) { memset(*buffer, 0, (size_t)(dh - SCALED_HEIGHT) * dw * 4); }
	}
	return 0;
}

// Captures the screen at full resolution and downscales it to the scaled size. Returns 0 on success.
static int get_scaled_desktop_buffer(void **buffer, long long *bufferSize, int *pixelSize)
{
	if (ensure_full_capture(adjust_screen_size(SCREEN_WIDTH), adjust_screen_size(SCREEN_HEIGHT)) != 0) { return 1; }
	if (capture_full_gdi() != 0) { return 1; }
	return scale_full_to_buffer(buffer, bufferSize, pixelSize);
}

#ifdef KVM_DXGI_ENABLED
// ---------------------------------------------------------------------------------------------------------------------
// DXGI Desktop Duplication: policy and glue, runs on the capture thread (see dxgi_capture.h for the module).
//
// The DXGI image is kept in the full resolution DIB section (fullBits, the same buffer the scaled GDI path uses): only the
// rectangles that DXGI reports as changed are copied into it from the GPU. Every captured frame is a new buffer, as with GDI:
// a copy of fullBits at 100%, the scaled image otherwise. The cursor is drawn into that copy (100%), like the GDI path does.
// A frame is PARTIAL when the changed rectangles are known: kvm.c then only checks the tiles they touch.
// ---------------------------------------------------------------------------------------------------------------------
#include "dxgi_ptrmask.h"
enum { DXS_IDLE = 0, DXS_OPEN = 1, DXS_DISABLED = 2 };

static struct
{
	int state;
	int envChecked;
	int strikes;				// Consecutive failures on the default desktop
	int transients;				// Consecutive "try again later" results (back-off)
	int hasNextTry;
	DWORD nextTry;				// GetTickCount() value before which no new attempt is made
	int ineligible;				// The last init was refused for the geometry in tgt* (no output / rotated): no retry until it changes
	int tgtX, tgtY, tgtW, tgtH;	// Geometry the duplication is open for
	char failDesktop[64];		// Desktop where an attempt failed without penalty: no new attempt until the desktop changes
	int persistentValid;		// fullBits holds the current screen
	int framesSinceOpen;		// Images received since the duplication was opened
	int seenFullGen;
	int upscaleLogged;
	int noOutputTries;			// Consecutive "no output with these coordinates" results (the topology can still be changing)
	int lostCount;				// ACCESS_LOST results in the current minute (a storm of them is slowed down)
	DWORD lostWindowStart;
	DWORD lastImageTick;		// GetTickCount() of the last image that changed something (for the check against GDI)
	DWORD lastStaleTick;
	int staleStrikes;
	int staleAfterReinit;		// A re-created duplication was caused by a stale check: the next difference means a systematic one
	int staleSystematic;		// GDI and DXGI differ permanently (e.g. HDR): the check only measures and logs from now on
	int staleSystematicLogged;
	int invalidCalls;			// Consecutive DXGI_ERROR_INVALID_CALL results without an image in between
	struct { int want, rectValid; dxgi_rect rect; HCURSOR cursor; int x, y; } ov; // Cursor overlay of the previous output (100% only)
	// System cursor composed into the DXGI image (software cursor: VMs, basic display drivers). See "Software cursor in the DXGI image" below.
	struct
	{
		int hwSeen, embedded, probes, probeValid, probeX, probeY;
		HCURSOR probeCursor;
		pm_cache cache;						// shape masks (and bitmaps) of the cursor handles
		int sw, sh;							// size of the persistent image, for the pre-copy hook
		int hookState;						// 0 = nothing saved, 1 = the regions hold pixels from before the frame, 2 = the frame went into an image without them
		int hookTouchCur;					// a rectangle of the frame touches the field at the position GetCursorInfo gave
		dxgi_rect hookUr, hookCr;			// that field (unclipped, clipped)
		HCURSOR hookHc;
		POINT hookPos;
		pm_region reg[PM_REGIONS];			// pixels from before the frame: small rectangles of the frame, and windows around the places the cursor can be in
		int nReg;
		dxgi_rect hist[3];					// fields of the last places where the cursor was found (windows are saved around them)
		int nHist;
		unsigned char *N, *G;				// raw field of the frame (with the cursor), GDI read of the field
		size_t nCap, gCap;
		int nValid;							// N and cur* belong to the last frame that touched the field
		dxgi_rect curUr, curCr;
		HCURSOR curHc;
		POINT curPos;
		int stale;							// pixels under the cursor may be out of date (the content of the field changed)
		DWORD lastTouch, lastGdi;
		int stRestores, stGdiReads, stFound, stNotFound, maskPx, logged;
		unsigned int stGdiUs, stSearchUs;
	} ptr;
} gDx;

static void dx_ptr_reset(void)
{
	pm_cache_free(&gDx.ptr.cache);
	pm_regions_free(gDx.ptr.reg, PM_REGIONS);
	free(gDx.ptr.N); free(gDx.ptr.G);
	memset(&gDx.ptr, 0, sizeof(gDx.ptr));
}
#define DX_PTR_RESET() dx_ptr_reset()

static void dx_desktop_name(char *name, int cap)
{
	DWORD needed = 0;
	HDESK d = GetThreadDesktop(GetCurrentThreadId());
	name[0] = 0;
	if (d == NULL || !GetUserObjectInformationA(d, UOI_NAME, name, (DWORD)cap, &needed)) { lstrcpynA(name, "?", cap); }
}

static int dx_time_reached(DWORD t) { return (int)(GetTickCount() - t) >= 0; }

static void dx_utf8(const wchar_t *w, char *out, int cap)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap - 1, NULL, NULL);
	if (n <= 0) { out[0] = 0; } else { out[n] = 0; }
	for (char *c = out; *c != 0; c++) { if (*c == '"') { *c = '\''; } }
}

// Closes the duplication. The next frame goes through GDI until it is opened again.
static void dx_close(void)
{
	dxgi_capture_shutdown();
	if (gDx.state == DXS_OPEN) { gDx.state = DXS_IDLE; }
	gDx.persistentValid = 0;
	gDx.ov.want = gDx.ov.rectValid = 0;
	DX_PTR_RESET();
}

static void dx_delay(DWORD ms)
{
	gDx.nextTry = GetTickCount() + ms;
	gDx.hasNextTry = 1;
}

static void dx_disable(const char *reason, kvm_frame_info *info)
{
	dx_close();
	gDx.state = DXS_DISABLED;
	info->dxgiEvent |= KVM_DXGI_EVENT_DISABLED;
	kvm_stats_log("dxgi disabled reason=%s hr=0x%08x", reason, (unsigned int)dxgi_capture_last_hr());
}

// A failure that counts towards switching DXGI off, only on the default desktop. On the secure desktops (logon screen, UAC)
// a failure is not held against DXGI: no new attempt until the desktop changes.
static void dx_strike(const char *desk, kvm_frame_info *info)
{
	if (lstrcmpiA(desk, "Default") != 0)
	{
		lstrcpynA(gDx.failDesktop, desk, (int)sizeof(gDx.failDesktop));
		return;
	}
	if (++gDx.strikes >= KVM_DXGI_MAX_STRIKES) { dx_disable("strikes", info); }
}

// reset_capture_bitmap() (desktop switch): close the duplication without penalty, the next frame tries again at once.
static void dx_reset(void)
{
	if (gDx.state == DXS_OPEN) { dx_close(); }
	gDx.persistentValid = 0;
	gDx.ineligible = 0;
	gDx.noOutputTries = 0;
	gDx.hasNextTry = 0;
}

static void dx_mark_overlay(unsigned char *map, const dxgi_rect &r)
{
	kvm_mark_rect_tiles(map, TILE_WIDTH_COUNT, TILE_HEIGHT_COUNT, TILE_WIDTH, TILE_HEIGHT, r.left, r.top, r.right, r.bottom, SCREEN_WIDTH, SCREEN_HEIGHT, SCALED_WIDTH, SCALED_HEIGHT);
}

static double dx_qpc_ms(void)
{
	static LARGE_INTEGER freq;
	LARGE_INTEGER t;
	if (freq.QuadPart == 0 && !QueryPerformanceFrequency(&freq)) { freq.QuadPart = 1; }
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

#ifdef KVM_STATS_LOG
// Safety net of the measurement branch: when DXGI has reported no change for a while, the persistent image is compared with a
// fresh GDI capture (outside the cursor). Differences twice in a row mean that the duplication stopped reporting. Returns the
// number of differing pixels, -1 when the check could not be made.
static int g_staleBox[4]; // rectangle around the differences of the last dx_stale_compare()
static long long dx_stale_compare(int sw, int sh)
{
	BITMAPINFO bi;
	void *bits = NULL;
	HBITMAP bmp;
	HGDIOBJ old;
	HDC dc;
	CURSORINFO ci;
	dxgi_rect skip;
	long long diff = -1;

	dc = CreateCompatibleDC(hDesktopDC);
	if (dc == NULL) { return -1; }
	ZeroMemory(&bi, sizeof(bi));
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = sw;
	bi.bmiHeader.biHeight = sh;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	bmp = CreateDIBSection(hDesktopDC, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	if (bmp != NULL && bits != NULL)
	{
		old = SelectObject(dc, bmp);
		// CAPTUREBLT: layered windows are part of the DXGI image, the capture of the GDI path leaves them out
		if (BitBlt(dc, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, hDesktopDC, SCREEN_X, SCREEN_Y, SRCCOPY | CAPTUREBLT))
		{
			GdiFlush();
			ZeroMemory(&ci, sizeof(ci));
			ci.cbSize = sizeof(ci);
			GetCursorInfo(&ci);
			skip.left = ci.ptScreenPos.x - SCREEN_X - 64; skip.right = skip.left + 192;
			skip.top = ci.ptScreenPos.y - SCREEN_Y - 64; skip.bottom = skip.top + 192;
			diff = kvm_count_pixel_diff(fullBits + ((size_t)(sh - 1) * sw * 4), -(ptrdiff_t)sw * 4, (unsigned char*)bits + ((size_t)(sh - 1) * sw * 4), -(ptrdiff_t)sw * 4, SCREEN_WIDTH, SCREEN_HEIGHT, &skip);
			g_staleBox[0] = g_staleBox[1] = g_staleBox[2] = g_staleBox[3] = 0;
			if (diff > 0)
			{
				// Rectangle around the differences (same skip field as the count): x0,y0 inclusive, x1,y1 exclusive, in screen pixels, top-down
				int x0 = SCREEN_WIDTH, y0 = SCREEN_HEIGHT, x1 = 0, y1 = 0;
				for (int y = 0; y < SCREEN_HEIGHT; y++)
				{
					const unsigned char *pa = fullBits + ((size_t)(sh - 1 - y) * sw * 4), *pb = (const unsigned char*)bits + ((size_t)(sh - 1 - y) * sw * 4);
					for (int x = 0; x < SCREEN_WIDTH; x++)
					{
						if (y >= skip.top && y < skip.bottom && x >= skip.left && x < skip.right) { continue; }
						if (pa[x * 4] != pb[x * 4] || pa[x * 4 + 1] != pb[x * 4 + 1] || pa[x * 4 + 2] != pb[x * 4 + 2])
						{
							if (x < x0) { x0 = x; }
							if (x + 1 > x1) { x1 = x + 1; }
							if (y < y0) { y0 = y; }
							if (y + 1 > y1) { y1 = y + 1; }
						}
					}
				}
				g_staleBox[0] = x0; g_staleBox[1] = y0; g_staleBox[2] = x1; g_staleBox[3] = y1;
			}
		}
		SelectObject(dc, old);
	}
	if (bmp != NULL) { DeleteObject(bmp); }
	DeleteDC(dc);
	return diff;
}
#endif


// ---------------------------------------------------------------------------------------------------------------------
// Software cursor in the DXGI image.
//
// On machines without a hardware cursor plane (some virtual machines) the system composes the mouse cursor into the desktop image that
// Desktop Duplication delivers. The GDI path (BitBlt) never contained it: the viewer draws its own pointer, and the agent draws one
// only for the overlay (kvm_cursor_xor, mouseMove[0] != 0, 100%). A cursor in the DXGI image would show twice.
//
// Detection (measured, see README-kvm-perf.md): with a software cursor the duplication reports no pointer at all (LastMouseUpdateTime
// is 0 in every frame, also while the cursor moves); with a hardware cursor it delivers the pointer separately (PointerPosition.Visible
// == TRUE and a shape) shortly after it is opened. So: once a pointer update with Visible == TRUE has been seen, the cursor is not in
// the image ("hardware": nothing more is done or examined until the duplication is opened again). Until then, whenever an image frame
// arrives and the cursor is showing at a position or with a handle that was not examined yet, the cursor field of the persistent image
// is compared with a GDI read (SRCCOPY): at least KVM_DXGI_POINTER_MIN_DIFF differing pixels mean "embedded". The state is kept between
// frames and logged at every change.
//
// Taking the cursor out without reading the screen. A GDI read of the screen costs 16-30 ms on those machines whatever its size, so it
// cannot be done for every frame. Invariant: the persistent image (fullBits) never holds the cursor. So before a frame is copied into
// it (hook dx_precopy(), called by dxgi_capture_acquire after the rectangles are known) the pixels of the cursor field are saved (S);
// after the copy the pixels under the SHAPE of the cursor (a mask per cursor handle: the pixels that differ between a DXGI image and a
// GDI read, widened by one pixel) are put back from S, the rest of the field keeps the new DXGI pixels. Where the content changed under
// the cursor, those pixels are out of date; a GDI read of the field (stale) puts the truth under the mask: KVM_DXGI_POINTER_QUIET_MS
// after the last frame that touched the field, or every KVM_DXGI_POINTER_REFRESH_MS while frames keep touching it (video under the
// cursor). A read in a quiet moment also corrects the mask (differences between the raw DXGI pixels N and the GDI read are added; a
// mask that was provisional, i.e. "the whole field", is made new). A cursor handle without a mask costs one GDI read. A moving cursor
// over static content, and a still screen, cost none: the pixels under the mask are then unchanged and nothing outside it changed.
//
// Race: a frame can have been composed with the cursor in A, while the field is taken at B (the position read in the hook). The trace
// in A stays in the image until the next frame; that frame reports A as changed (the cursor was moved away from it, the rectangle holds
// clean pixels) and overwrites it. The frame in which the cursor stopped is composed with the cursor in its final place, which is where
// the hook reads it, so no trace outlives the last frame. The field the cursor left is not restored for the same reason.
//
// KVM_DXGI_POINTER_KEEP: nothing is taken out (the detection and the log stay), for comparison and as a switch.
// ---------------------------------------------------------------------------------------------------------------------

static inline const pm_rect &PMR(const dxgi_rect &r) { return *(const pm_rect *)&r; }

static int dx_ptr_buf(unsigned char **p, size_t *cap, size_t need)
{
	unsigned char *n;
	if (*cap >= need) { return 1; }
	if ((n = (unsigned char *)realloc(*p, need)) == NULL) { return 0; }
	*p = n;
	*cap = need;
	return 1;
}

// The field of the cursor now: ur unclipped, cr clipped to the captured rectangle (captured-rectangle pixels). 0 when the cursor is hidden or outside.
static int dx_ptr_field(dxgi_rect *ur, dxgi_rect *cr, HCURSOR *hc, POINT *cp)
{
	dxgi_rect f;
	if (!kvm_cursor_field(KVM_DXGI_POINTER_MARGIN, &f, hc, cp)) { return 0; }
	f.left -= SCREEN_X; f.right -= SCREEN_X; f.top -= SCREEN_Y; f.bottom -= SCREEN_Y;
	*ur = f;
	if (f.left < 0) { f.left = 0; }
	if (f.top < 0) { f.top = 0; }
	if (f.right > SCREEN_WIDTH) { f.right = SCREEN_WIDTH; }
	if (f.bottom > SCREEN_HEIGHT) { f.bottom = SCREEN_HEIGHT; }
	if (f.right <= f.left || f.bottom <= f.top) { return 0; }
	*cr = f;
	return 1;
}

// GDI read (SRCCOPY, no CAPTUREBLT: that makes the system draw the cursor again, one new DXGI image per read) of cr into gDx.ptr.G, top-down, 4 bytes per pixel.
static int dx_gdi_read(const dxgi_rect &cr)
{
	int w = cr.right - cr.left, h = cr.bottom - cr.top, ok = 0;
	BITMAPINFO bi;
	void *bits = NULL;
	HBITMAP bmp;
	HGDIOBJ old;
	HDC dc;
	double t0 = dx_qpc_ms();
	unsigned int us;

	if (w <= 0 || h <= 0 || !dx_ptr_buf(&gDx.ptr.G, &gDx.ptr.gCap, (size_t)w * h * 4)) { return 0; }
	if ((dc = CreateCompatibleDC(hDesktopDC)) == NULL) { return 0; }
	ZeroMemory(&bi, sizeof(bi));
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = h; // bottom-up
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	bmp = CreateDIBSection(hDesktopDC, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	if (bmp != NULL && bits != NULL)
	{
		old = SelectObject(dc, bmp);
		if (BitBlt(dc, 0, 0, w, h, hDesktopDC, SCREEN_X + cr.left, SCREEN_Y + cr.top, SRCCOPY))
		{
			GdiFlush();
			for (int y = 0; y < h; y++) { memcpy(gDx.ptr.G + (size_t)y * w * 4, (const unsigned char *)bits + (size_t)(h - 1 - y) * w * 4, (size_t)w * 4); }
			ok = 1;
		}
		SelectObject(dc, old);
	}
	if (bmp != NULL) { DeleteObject(bmp); }
	DeleteDC(dc);
	us = (unsigned int)((dx_qpc_ms() - t0) * 1000.0);
	gDx.ptr.stGdiReads++;
	gDx.ptr.stGdiUs += (us > 0 ? us : 1);
	return ok;
}

static void dx_pointer_log(const char *reason, int diff, const dxgi_frame &f, const dxgi_rect &fld, HCURSOR hc)
{
	kvm_stats_log("dxgi pointer embedded=%d reason=%s diff=%d hardware_seen=%d mouse_update=%d dx_visible=%d shape_size=%u cursor=%p field=%d,%d,%d,%d patch=%s",
		gDx.ptr.embedded, reason, diff, gDx.ptr.hwSeen, f.ptrUpdate, f.ptrVisible, f.ptrShapeSize, (void *)hc, fld.left, fld.top, fld.right, fld.bottom,
#ifdef KVM_DXGI_POINTER_KEEP
		"off"
#else
		"on"
#endif
	);
}

#ifndef KVM_DXGI_POINTER_KEEP
static void dx_mask_log(const char *reason, HCURSOR hc, const pm_mask *e)
{
	if (gDx.ptr.logged >= 40) { return; }
	gDx.ptr.logged++;
	kvm_stats_log("dxgi pointer mask reason=%s cursor=%p field=%dx%d pixels=%d provisional=%d", reason, (void *)hc, e->w, e->h, e->count, e->provisional);
}
#endif


#ifndef KVM_DXGI_POINTER_KEEP
// The entry of a cursor handle (created when new): its bitmap, and for a colour cursor the mask made from it (no GDI read needed).
static pm_mask *dx_mask_entry(HCURSOR hc, const dxgi_rect &ur)
{
	int w = ur.right - ur.left, h = ur.bottom - ur.top;
	pm_mask *e = pm_cache_find(&gDx.ptr.cache, (void *)hc, w, h);
	if (e == NULL)
	{
		e = pm_cache_take(&gDx.ptr.cache, (void *)hc, w, h);
		if (kvm_cursor_sprite(hc, &e->sp)) { e->spState = 1; if (pm_mask_from_sprite(e, KVM_DXGI_POINTER_MARGIN)) { dx_mask_log("sprite", hc, e); } }
		else { e->spState = 2; }
	}
	return e;
}
#endif

// A frame has been acquired: a pointer update with Visible == TRUE means a hardware cursor.
static void dx_pointer_latch(const dxgi_frame &f)
{
	if (f.ptrUpdate && f.ptrVisible && !gDx.ptr.hwSeen)
	{
		int was = gDx.ptr.embedded;
		dxgi_rect none;
		memset(&none, 0, sizeof(none));
		gDx.ptr.hwSeen = 1;
		gDx.ptr.embedded = 0;
		gDx.ptr.hookState = 0;
		gDx.ptr.stale = 0;
		if (was || gDx.ptr.probes > 0) { dx_pointer_log("hardware_pointer", 0, f, none, NULL); } // not logged for the common case: nothing was ever examined
	}
}

// Hook of dxgi_capture_acquire: the rectangles of a new image are known, nothing has been copied yet. Saves the pixels of the persistent image
// where the cursor can be drawn by this frame: every small rectangle of the frame (a moving software cursor makes the frame report the
// rectangle around its old and new place, measured: one rectangle of 16-48 pixels per frame), and windows around the places where the cursor
// can be in a large rectangle (where GetCursorInfo says it is, and the last places where it was found).
static void dx_precopy(int fullCopy, int rectCount, const dxgi_rect *rects)
{
#ifndef KVM_DXGI_POINTER_KEEP
	dxgi_rect ur, cr, win[4], q;
	HCURSOR hc;
	POINT cp;
	int nWin = 0, touch = fullCopy;
	long long budget = 1200000;

	gDx.ptr.hookState = 0;
	gDx.ptr.nReg = 0;
	gDx.ptr.hookTouchCur = 0;
	if (!gDx.ptr.embedded || gDx.ptr.hwSeen) { return; }
	if (!dx_ptr_field(&ur, &cr, &hc, &cp)) { return; }
	for (int i = 0; i < rectCount && !touch; i++)
	{
		if (rects[i].left < cr.right && rects[i].right > cr.left && rects[i].top < cr.bottom && rects[i].bottom > cr.top) { touch = 1; }
	}
	gDx.ptr.hookUr = ur; gDx.ptr.hookCr = cr; gDx.ptr.hookHc = hc; gDx.ptr.hookPos = cp; gDx.ptr.hookTouchCur = touch;
	if (!gDx.persistentValid) { gDx.ptr.hookState = 2; return; }

	// windows: around the field GetCursorInfo gave, and around the last places where the cursor was found
	win[nWin++] = cr;
	for (int i = 0; i < gDx.ptr.nHist && nWin < 4; i++) { win[nWin++] = gDx.ptr.hist[i]; }
	for (int i = 0; i < nWin; i++)
	{
		win[i].left -= 48; win[i].top -= 48; win[i].right += 48; win[i].bottom += 48;
		if (win[i].left < 0) { win[i].left = 0; }
		if (win[i].top < 0) { win[i].top = 0; }
		if (win[i].right > SCREEN_WIDTH) { win[i].right = SCREEN_WIDTH; }
		if (win[i].bottom > SCREEN_HEIGHT) { win[i].bottom = SCREEN_HEIGHT; }
	}
	// Only PM_REGIONS rectangles are saved: first the ones near the places where the cursor can be (and the large ones, where the windows are saved), then the others
	for (int pass = 0; pass < 2; pass++)
	for (int i = 0; i < rectCount && gDx.ptr.nReg < PM_REGIONS; i++)
	{
		q = rects[i];
		if (q.left < 0) { q.left = 0; } if (q.top < 0) { q.top = 0; }
		if (q.right > SCREEN_WIDTH) { q.right = SCREEN_WIDTH; } if (q.bottom > SCREEN_HEIGHT) { q.bottom = SCREEN_HEIGHT; }
		if (q.right <= q.left || q.bottom <= q.top) { continue; }
		long long area = (long long)(q.right - q.left) * (q.bottom - q.top);
		{
			int nearWin = (area > 65536) ? 1 : 0;
			for (int k = 0; k < nWin && !nearWin; k++) { if (q.left < win[k].right && q.right > win[k].left && q.top < win[k].bottom && q.bottom > win[k].top) { nearWin = 1; } }
			if ((pass == 0) != (nearWin != 0)) { continue; }
		}
		if (area <= 65536)
		{
			if (budget < area || !pm_region_save(fullBits, gDx.ptr.sw, gDx.ptr.sh, &gDx.ptr.reg[gDx.ptr.nReg], PMR(q))) { continue; }
			budget -= area; gDx.ptr.nReg++;
		}
		else
		{
			for (int k = 0; k < nWin && gDx.ptr.nReg < PM_REGIONS; k++)
			{
				dxgi_rect w = win[k];
				if (w.left < q.left) { w.left = q.left; } if (w.top < q.top) { w.top = q.top; }
				if (w.right > q.right) { w.right = q.right; } if (w.bottom > q.bottom) { w.bottom = q.bottom; }
				if (w.right <= w.left || w.bottom <= w.top) { continue; }
				long long a = (long long)(w.right - w.left) * (w.bottom - w.top);
				if (budget < a || !pm_region_save(fullBits, gDx.ptr.sw, gDx.ptr.sh, &gDx.ptr.reg[gDx.ptr.nReg], PMR(w))) { continue; }
				budget -= a; gDx.ptr.nReg++;
			}
		}
	}
	if (fullCopy)
	{
		// no usable metadata: the whole output is written; save the windows
		for (int k = 0; k < nWin && gDx.ptr.nReg < PM_REGIONS; k++)
		{
			long long a = (long long)(win[k].right - win[k].left) * (win[k].bottom - win[k].top);
			if (a > 0 && budget >= a && pm_region_save(fullBits, gDx.ptr.sw, gDx.ptr.sh, &gDx.ptr.reg[gDx.ptr.nReg], PMR(win[k]))) { budget -= a; gDx.ptr.nReg++; }
		}
	}
	gDx.ptr.hookState = (gDx.ptr.nReg > 0) ? 1 : 0;
#else
	(void)fullCopy; (void)rectCount; (void)rects;
#endif
}

// After the frame (and after the image of a frame was copied into the persistent image): detection, taking the cursor out, refreshing the
// pixels under it from GDI. marks gets the rectangles (captured-rectangle pixels) whose tiles have to be checked although DXGI did not
// report them; returns their number (at most 2).
static int dx_pointer_process(const dxgi_frame &f, int gotImage, int sw, int sh, dxgi_rect *marks)
{
	int nMark = 0;
	DWORD now = GetTickCount();
	dxgi_rect ur, cr;
	HCURSOR hc;
	POINT cp;

	if (gDx.ptr.hwSeen) { return 0; }

	if (!gDx.ptr.embedded)
	{
		int diff;
		if (!gotImage || !dx_ptr_field(&ur, &cr, &hc, &cp)) { return 0; } // hidden, or on another monitor: no cursor in the image
		if (gDx.ptr.probeValid && gDx.ptr.probeCursor == hc && gDx.ptr.probeX == cp.x && gDx.ptr.probeY == cp.y) { return 0; } // examined already
		gDx.ptr.probeValid = 1; gDx.ptr.probeCursor = hc; gDx.ptr.probeX = cp.x; gDx.ptr.probeY = cp.y;
		gDx.ptr.probes++;
		if (!dx_gdi_read(cr)) { return 0; }
		diff = pm_diff_count(fullBits, sw, sh, PMR(cr), gDx.ptr.G);
		if (diff < KVM_DXGI_POINTER_MIN_DIFF) { if (gDx.ptr.probes <= 3) { dx_pointer_log("probe_clean", diff, f, cr, hc); } return 0; }
		gDx.ptr.embedded = 1;
		dx_pointer_log("probe", diff, f, cr, hc);
#ifndef KVM_DXGI_POINTER_KEEP
		// The field holds the cursor: take the pixels under its shape from the GDI read
		if (dx_ptr_buf(&gDx.ptr.N, &gDx.ptr.nCap, (size_t)(cr.right - cr.left) * (cr.bottom - cr.top) * 4))
		{
			pm_mask *e = dx_mask_entry(hc, ur);
			pm_copy_out(fullBits, sw, sh, PMR(cr), gDx.ptr.N);
			if (e->m == NULL) { pm_build(e, PMR(ur), PMR(cr), gDx.ptr.N, gDx.ptr.G, 1); dx_mask_log("probe", hc, e); }
			pm_apply(e, PMR(ur), PMR(cr), fullBits, sw, sh, gDx.ptr.G);
			gDx.ptr.maskPx = e->count;
			gDx.ptr.curUr = ur; gDx.ptr.curCr = cr; gDx.ptr.curHc = hc; gDx.ptr.curPos = cp;
			gDx.ptr.nValid = 1; gDx.ptr.stale = 0; gDx.ptr.lastTouch = gDx.ptr.lastGdi = now;
			marks[nMark++] = cr;
		}
#endif
		return nMark;
	}

#ifndef KVM_DXGI_POINTER_KEEP
	if (gotImage && gDx.ptr.hookState != 0)
	{
		int st = gDx.ptr.hookState;
		int found = 0, bx = 0, by = 0, hits = 0, outside = 0, farPick = 0;
		pm_mask *e;
		ur = gDx.ptr.hookUr; cr = gDx.ptr.hookCr; hc = gDx.ptr.hookHc; cp = gDx.ptr.hookPos;
		gDx.ptr.hookState = 0;
		e = dx_mask_entry(hc, ur);
		if (st == 1 && e->spState == 1)
		{
			// Where did the system put the cursor into this frame? Not necessarily where GetCursorInfo says it is now.
			// Several places can look like the cursor (the same arrow inside a screenshot shown in the frame): collect the best match of every region and take the
			// one nearest to where the cursor is expected (GetCursorInfo, the places where it was found before). A full match within KVM_DXGI_POINTER_NEAR_PX of an
			// expected place ends the search (the usual case costs what it did).
			double t0 = dx_qpc_ms();
			long long tried = 0;
			pm_cand cand[PM_REGIONS];
			int nCand = 0, ex[4], ey[4], nEx = 0;
			ex[nEx] = (cp.x - SCREEN_X) - e->sp.hx; ey[nEx++] = (cp.y - SCREEN_Y) - e->sp.hy;
			for (int i = 0; i < gDx.ptr.nHist && nEx < 4; i++) { ex[nEx] = gDx.ptr.hist[i].left + KVM_DXGI_POINTER_MARGIN; ey[nEx++] = gDx.ptr.hist[i].top + KVM_DXGI_POINTER_MARGIN; }
			for (int i = 0; i < gDx.ptr.nReg; i++)
			{
				int x = 0, y = 0, h2 = 0;
				long long left = 600000 - tried;
				if (left <= 0) { break; }
				if (pm_find(fullBits, sw, sh, &e->sp, gDx.ptr.reg[i].r, &x, &y, &h2, left < 120000 ? left : 120000, &tried))
				{
					cand[nCand].x = x; cand[nCand].y = y; cand[nCand].hits = h2; nCand++;
					if (h2 >= e->sp.n)
					{
						int isNear = 0;
						for (int k = 0; k < nEx; k++) { int dx = x - ex[k], dy = y - ey[k]; if (dx < 0) { dx = -dx; } if (dy < 0) { dy = -dy; } if (dx <= KVM_DXGI_POINTER_NEAR_PX && dy <= KVM_DXGI_POINTER_NEAR_PX) { isNear = 1; } }
						if (isNear) { break; }
					}
				}
			}
			{
				int pick = pm_pick(cand, nCand, ex, ey, nEx);
				if (pick >= 0)
				{
					int nearest = 0x7FFFFFFF;
					bx = cand[pick].x; by = cand[pick].y; hits = cand[pick].hits; found = 1;
					for (int k = 0; k < nEx; k++) { int dx = bx - ex[k], dy = by - ey[k], d; if (dx < 0) { dx = -dx; } if (dy < 0) { dy = -dy; } d = dx > dy ? dx : dy; if (d < nearest) { nearest = d; } }
					if (nearest > KVM_DXGI_POINTER_NEAR_PX) { farPick = 1; } // far from every expected place: maybe not the cursor (a picture of one inside the frame): check the field against GDI
				}
			}
			gDx.ptr.stSearchUs += (unsigned int)((dx_qpc_ms() - t0) * 1000.0);
			if (found)
			{
				// the field around the place where it was found (same margin as the field of GetCursorInfo)
				int dxm = (bx + e->sp.hx) - (cp.x - SCREEN_X), dym = (by + e->sp.hy) - (cp.y - SCREEN_Y);
				ur.left += dxm; ur.right += dxm; ur.top += dym; ur.bottom += dym;
				cr = ur;
				if (cr.left < 0) { cr.left = 0; } if (cr.top < 0) { cr.top = 0; }
				if (cr.right > SCREEN_WIDTH) { cr.right = SCREEN_WIDTH; } if (cr.bottom > SCREEN_HEIGHT) { cr.bottom = SCREEN_HEIGHT; }
				cp.x = bx + e->sp.hx + SCREEN_X; cp.y = by + e->sp.hy + SCREEN_Y;
				gDx.ptr.stFound++;
				if (gDx.ptr.nHist < 3) { gDx.ptr.nHist++; }
				for (int i = gDx.ptr.nHist - 1; i > 0; i--) { gDx.ptr.hist[i] = gDx.ptr.hist[i - 1]; }
				gDx.ptr.hist[0] = cr;
			}
			else if (gDx.ptr.hookTouchCur) { gDx.ptr.stNotFound++; }
		}
		if (dx_ptr_buf(&gDx.ptr.N, &gDx.ptr.nCap, (size_t)(cr.right - cr.left) * (cr.bottom - cr.top) * 4))
		{
			if (st == 1 && e->m != NULL && (found || e->spState != 1 || gDx.ptr.hookTouchCur))
			{
				// The pixels under the cursor shape come from before the frame, the rest of the field is new.
				// N (the raw pixels of the field, with the cursor) is only written here and in the branch below: it is what the next quiet GDI read compares with.
				int sameSpot = (gDx.ptr.nValid && gDx.ptr.curHc == hc && gDx.ptr.curPos.x == cp.x && gDx.ptr.curPos.y == cp.y) ? 1 : 0;
				pm_copy_out(fullBits, sw, sh, PMR(cr), gDx.ptr.N);
				outside = pm_restore_regions(e, PMR(ur), PMR(cr), fullBits, sw, sh, gDx.ptr.reg, gDx.ptr.nReg);
				gDx.ptr.stRestores++;
				// A still cursor whose field the frame touched: the content under its shape may have changed too (nothing outside the mask changed, so the count above cannot tell): refresh it from GDI.
				if (outside > 0 || e->provisional || (e->spState == 1 && !found) || farPick || (sameSpot && gDx.ptr.hookTouchCur)) { gDx.ptr.stale = 1; }
				gDx.ptr.curUr = ur; gDx.ptr.curCr = cr; gDx.ptr.curHc = hc; gDx.ptr.curPos = cp;
				gDx.ptr.nValid = 1;
				gDx.ptr.lastTouch = now;
				gDx.ptr.maskPx = e->count;
			}
			else if (gDx.ptr.hookTouchCur && (st == 2 || e->m == NULL))
			{
				// A cursor without a mask (monochrome cursor of a new handle), or no pixels from before the frame: one GDI read
				pm_copy_out(fullBits, sw, sh, PMR(cr), gDx.ptr.N);
				if (dx_gdi_read(cr))
				{
					if (e->m == NULL) { pm_build(e, PMR(ur), PMR(cr), gDx.ptr.N, gDx.ptr.G, 1); dx_mask_log("new_cursor", hc, e); }
					pm_apply(e, PMR(ur), PMR(cr), fullBits, sw, sh, gDx.ptr.G);
					marks[nMark++] = cr;
					gDx.ptr.stale = 0;
					gDx.ptr.lastGdi = now;
				}
				gDx.ptr.curUr = ur; gDx.ptr.curCr = cr; gDx.ptr.curHc = hc; gDx.ptr.curPos = cp;
				gDx.ptr.nValid = 1;
				gDx.ptr.lastTouch = now;
				gDx.ptr.maskPx = e->count;
			}
		}
	}

	if (gDx.ptr.stale)
	{
		dxgi_rect ur2, cr2;
		HCURSOR hc2;
		POINT cp2;
		if (!dx_ptr_field(&ur2, &cr2, &hc2, &cp2)) { gDx.ptr.stale = 0; } // hidden
		else if (gDx.ptr.nValid &&
			((DWORD)(now - gDx.ptr.lastTouch) >= KVM_DXGI_POINTER_QUIET_MS || (DWORD)(now - gDx.ptr.lastGdi) >= KVM_DXGI_POINTER_REFRESH_MS))
		{
			// The field is the one saved at the last restore (curCr, where the cursor was found), not necessarily where GetCursorInfo says the cursor is now: the GDI read
			// has no cursor in it, so what it puts under the mask is true wherever the cursor is.
			pm_mask *e = pm_cache_find(&gDx.ptr.cache, (void *)gDx.ptr.curHc, gDx.ptr.curUr.right - gDx.ptr.curUr.left, gDx.ptr.curUr.bottom - gDx.ptr.curUr.top);
			if (e != NULL && dx_gdi_read(gDx.ptr.curCr))
			{
				int before = e->count, wasProv = e->provisional;
				if ((DWORD)(now - gDx.ptr.lastTouch) >= KVM_DXGI_POINTER_QUIET_MS)
				{
					pm_build(e, PMR(gDx.ptr.curUr), PMR(gDx.ptr.curCr), gDx.ptr.N, gDx.ptr.G, e->provisional);
					if (e->count != before || e->provisional != wasProv) { dx_mask_log("quiet", gDx.ptr.curHc, e); }
					gDx.ptr.maskPx = e->count;
				}
				pm_apply(e, PMR(gDx.ptr.curUr), PMR(gDx.ptr.curCr), fullBits, sw, sh, gDx.ptr.G);
				if (nMark < 2) { marks[nMark++] = gDx.ptr.curCr; }
				gDx.ptr.stale = 0;
				gDx.ptr.lastGdi = now;
			}
		}
	}
#else
	(void)gotImage;
#endif
	return nMark;
}

// Returns 0 when the frame was produced (*buffer, or KVM_FRAME_UNCHANGED), 1 when this frame has to be captured with GDI.
// On 1 nothing is allocated and the caller captures a complete GDI frame.
static int kvm_dxgi_frame(void **buffer, long long *bufferSize, long *mouseMove, int *pixelSize, int flags, kvm_frame_info *info)
{
	char desk[64];
	int sw, sh, want, changed = 0, gotImage = 0, fed = 0, kind, drawn = 0, nPatch = 0;
	dxgi_rect patchRect[2];
	int forceFull = (flags & KVM_CAPTURE_FORCE_FULL) != 0;
	int wantImage = (flags & KVM_CAPTURE_WANT_IMAGE) != 0; // WA-2: a picture is needed although nothing changed (sharpening); the capture still waits the normal timeout for a new frame
	unsigned int timeout;
	double acquireStart, acquireMs, outStart;
	dxgi_result r;
	dxgi_frame f;
	CURSORINFO ci;
	dxgi_rect cur;
	unsigned char *dstTop, *map = NULL;
	void *out = NULL;
	long long outSize = 0;
	int outPixel = 4;

	if (gDx.state == DXS_DISABLED) { return 1; }
	if (!gDx.envChecked)
	{
		char env[8] = "";
		gDx.envChecked = 1;
		if (GetEnvironmentVariableA("MESH_KVM_FORCE_GDI", env, sizeof(env)) > 0 && env[0] == '1') { dx_disable("env", info); return 1; }
	}

	// Scaling up is done with StretchBlt on the GDI path. Scaling down and 100% use the full resolution image.
	if (SCALING_FACTOR != 1024 && !(adjust_screen_size(SCALED_WIDTH) <= adjust_screen_size(SCREEN_WIDTH) && adjust_screen_size(SCALED_HEIGHT) <= adjust_screen_size(SCREEN_HEIGHT)))
	{
		if (gDx.state == DXS_OPEN) { dx_close(); }
		if (!gDx.upscaleLogged) { gDx.upscaleLogged = 1; kvm_stats_log("dxgi ineligible reason=upscale scale=%d", SCALING_FACTOR); }
		return 1;
	}
	gDx.upscaleLogged = 0;

	// The duplication is bound to one output: a new capture rectangle needs a new duplication.
	if (gDx.state == DXS_OPEN && (gDx.tgtX != SCREEN_X || gDx.tgtY != SCREEN_Y || gDx.tgtW != SCREEN_WIDTH || gDx.tgtH != SCREEN_HEIGHT)) { dx_close(); }

	if (gDx.state == DXS_IDLE)
	{
		dxgi_info di;
		dxgi_result ir;
		double initStart;
		int sameGeometry = (gDx.tgtX == SCREEN_X && gDx.tgtY == SCREEN_Y && gDx.tgtW == SCREEN_WIDTH && gDx.tgtH == SCREEN_HEIGHT);

		if (gDx.ineligible)
		{
			if (sameGeometry) { return 1; }
			gDx.ineligible = 0;
		}
		if (!sameGeometry) { gDx.noOutputTries = 0; }
		dx_desktop_name(desk, (int)sizeof(desk));
		if (gDx.failDesktop[0] != 0)
		{
			if (lstrcmpiA(desk, gDx.failDesktop) == 0) { return 1; }
			gDx.failDesktop[0] = 0;
		}
		if (gDx.hasNextTry && !dx_time_reached(gDx.nextTry)) { return 1; }
		gDx.hasNextTry = 0;

		initStart = dx_qpc_ms();
		ir = dxgi_capture_init(SCREEN_X, SCREEN_Y, SCREEN_WIDTH, SCREEN_HEIGHT, &di);
		if (dx_qpc_ms() - initStart > 1000.0) { kvm_stats_log("dxgi slow_init ms=%d result=%s", (int)(dx_qpc_ms() - initStart), dxgi_result_name(ir)); }
		gDx.tgtX = SCREEN_X; gDx.tgtY = SCREEN_Y; gDx.tgtW = SCREEN_WIDTH; gDx.tgtH = SCREEN_HEIGHT;
		if (ir == DXGI_CAP_OK)
		{
			char ad[256], out8[64];
			gDx.state = DXS_OPEN;
			gDx.framesSinceOpen = 0;
			gDx.persistentValid = 0;
			gDx.transients = 0;
			gDx.noOutputTries = 0;
			gDx.lastImageTick = gDx.lastStaleTick = GetTickCount();
			gDx.staleStrikes = 0;
			gDx.ov.want = gDx.ov.rectValid = 0;
			DX_PTR_RESET();
			info->dxgiEvent |= KVM_DXGI_EVENT_OPEN;
			dx_utf8(di.adapter, ad, (int)sizeof(ad));
			dx_utf8(di.output, out8, (int)sizeof(out8));
			kvm_stats_log("dxgi open adapter=\"%s\" vendor=0x%04x output=%s mode=%dx%d rot=%d sysmem=%d desktop=%s", ad, di.vendorId, out8, di.width, di.height, di.rotation, di.systemMemory, desk);
		}
		else if (ir == DXGI_CAP_NO_OUTPUT || ir == DXGI_CAP_ROTATED)
		{
			info->dxgiEvent |= KVM_DXGI_EVENT_FALLBACK;
			if (ir == DXGI_CAP_NO_OUTPUT && ++gDx.noOutputTries < 3)
			{
				// Right after a change of the display topology the outputs of DXGI can lag behind: ask again in a second, then give up
				dx_delay(1000);
				return 1;
			}
			gDx.ineligible = 1;
			kvm_stats_log("dxgi ineligible reason=%s rect=%d,%d,%d,%d desktop=%s", ir == DXGI_CAP_ROTATED ? "rotated" : "no_output", SCREEN_X, SCREEN_Y, SCREEN_WIDTH, SCREEN_HEIGHT, desk);
			return 1;
		}
		else
		{
			info->dxgiEvent |= KVM_DXGI_EVENT_FALLBACK;
			if (ir == DXGI_CAP_UNSUPPORTED) { dx_disable("unsupported", info); return 1; }
			if (ir == DXGI_CAP_TRANSIENT)
			{
				// Try again later; every failed attempt doubles the wait (up to 8 times), no penalty
				if (gDx.transients < 4) { gDx.transients++; }
				if (lstrcmpiA(desk, "Default") != 0) { lstrcpynA(gDx.failDesktop, desk, (int)sizeof(gDx.failDesktop)); }
				dx_delay((DWORD)KVM_DXGI_RETRY_MS << (gDx.transients - 1));
				if (gDx.transients <= 3) { kvm_stats_log("dxgi fail stage=init result=transient hr=0x%08x desktop=%s", (unsigned int)dxgi_capture_last_hr(), desk); }
				return 1;
			}
			kvm_stats_log("dxgi fail stage=init result=%s hr=0x%08x strikes=%d/%d desktop=%s", dxgi_result_name(ir), (unsigned int)dxgi_capture_last_hr(), gDx.strikes + 1, KVM_DXGI_MAX_STRIKES, desk);
			dx_delay(KVM_DXGI_RETRY_MS);
			dx_strike(desk, info);
			return 1;
		}
	}

	// From here on the duplication is open.
	sw = adjust_screen_size(SCREEN_WIDTH);
	sh = adjust_screen_size(SCREEN_HEIGHT);
	if (ensure_full_capture(sw, sh) != 0) { return 1; }
	if (gDx.seenFullGen != fullCaptureGen) { gDx.seenFullGen = fullCaptureGen; gDx.persistentValid = 0; }

	// The mouse cursor is not part of the DXGI image. At 100% it is drawn into the output (like the GDI path), when the
	// client expects it in the image (mouseMove[0] != 0). A change of the cursor needs a new output.
	want = (SCALING_FACTOR == 1024 && mouseMove[0] != 0) ? 1 : 0;
	memset(&ci, 0, sizeof(ci));
	ci.cbSize = sizeof(ci);
	if (want)
	{
		GetCursorInfo(&ci);
		changed = (!gDx.ov.want || mouseMove[1] != gDx.ov.x || mouseMove[2] != gDx.ov.y || ci.hCursor != gDx.ov.cursor) ? 1 : 0;
	}
	else
	{
		changed = gDx.ov.want ? 1 : 0;
	}

	timeout = (forceFull || changed) ? 0 : (gDx.persistentValid ? KVM_DXGI_TIMEOUT_MS : KVM_DXGI_FIRST_FRAME_MS);
	dstTop = fullBits + ((size_t)(sh - 1) * sw * 4);
	memset(&f, 0, sizeof(f));
	gDx.ptr.sw = sw; gDx.ptr.sh = sh; gDx.ptr.hookState = 0;
	dxgi_capture_set_precopy(dx_precopy);
	acquireStart = dx_qpc_ms();
	r = dxgi_capture_acquire(timeout, dstTop, -(ptrdiff_t)sw * 4, gDx.persistentValid ? 0 : 1, &f);
	acquireMs = dx_qpc_ms() - acquireStart;

	switch (r)
	{
		case DXGI_CAP_OK:
			gDx.framesSinceOpen++;
			gDx.strikes = 0;
			gDx.invalidCalls = 0;
			gDx.persistentValid = 1;
			gotImage = (f.rectCount > 0);
			if (gotImage) { gDx.lastImageTick = GetTickCount(); gDx.staleStrikes = 0; }
			dx_pointer_latch(f);
			break;
		case DXGI_CAP_NO_CHANGE:
			dx_pointer_latch(f);
			break;
		case DXGI_CAP_TIMEOUT:
			break;
		default:
		{
			// The module is closed. This frame goes through GDI.
			int framesBefore = gDx.framesSinceOpen;
			dx_desktop_name(desk, (int)sizeof(desk));
			gDx.state = DXS_IDLE;
			gDx.persistentValid = 0;
			gDx.ov.want = gDx.ov.rectValid = 0;
			DX_PTR_RESET();
			info->dxgiEvent |= (r == DXGI_CAP_LOST) ? KVM_DXGI_EVENT_LOST : KVM_DXGI_EVENT_FALLBACK;
			if (r == DXGI_CAP_LOST)
			{
				// Resolution change, UAC, lock, sign in: open it again after a short pause. Only a duplication that dies
				// before it gave a single image counts against DXGI.
				kvm_stats_log("dxgi lost hr=0x%08x images=%d desktop=%s", (unsigned int)dxgi_capture_last_hr(), framesBefore, desk);
				if ((DWORD)(GetTickCount() - gDx.lostWindowStart) > 60000) { gDx.lostWindowStart = GetTickCount(); gDx.lostCount = 0; }
				if (++gDx.lostCount > 5)
				{
					// More than 5 in a minute: something keeps taking the duplication away, do not rebuild the device four times a second
					if (gDx.lostCount == 6) { kvm_stats_log("dxgi lost_storm count=%d", gDx.lostCount); }
					dx_delay(5000);
				}
				else
				{
					dx_delay(KVM_DXGI_LOST_DELAY_MS);
				}
				if (framesBefore == 0) { dx_strike(desk, info); }
			}
			else if (r == DXGI_CAP_TRANSIENT)
			{
				if (lstrcmpiA(desk, "Default") != 0) { lstrcpynA(gDx.failDesktop, desk, (int)sizeof(gDx.failDesktop)); }
				dx_delay(KVM_DXGI_RETRY_MS);
				kvm_stats_log("dxgi fail stage=acquire result=transient hr=0x%08x release_hr=0x%08x images=%d desktop=%s", (unsigned int)dxgi_capture_last_hr(), (unsigned int)dxgi_capture_release_hr(), framesBefore, desk);
				if (dxgi_capture_last_hr() == DXGI_ERROR_INVALID_CALL && ++gDx.invalidCalls >= 3)
				{
					// The same error again and again without an image: not a change of the display, count it like an error
					kvm_stats_log("dxgi invalid_call_repeat count=%d desktop=%s", gDx.invalidCalls, desk);
					gDx.invalidCalls = 0;
					dx_strike(desk, info);
				}
			}
			else
			{
				kvm_stats_log("dxgi fail stage=acquire result=%s hr=0x%08x strikes=%d/%d desktop=%s", dxgi_result_name(r), (unsigned int)dxgi_capture_last_hr(), gDx.strikes + 1, KVM_DXGI_MAX_STRIKES, desk);
				dx_delay(KVM_DXGI_RETRY_MS);
				dx_strike(desk, info);
			}
			return 1;
		}
	}

	// The software cursor: detection, taking it out of the image, refreshing the pixels under it (may need tiles to be checked and a frame without a new DXGI image)
	nPatch = dx_pointer_process(f, gotImage, sw, sh, patchRect);
	info->ptrEmbedded = gDx.ptr.embedded;
	info->ptrFound = gDx.ptr.stFound; info->ptrNotFound = gDx.ptr.stNotFound; info->ptrSearchUs = gDx.ptr.stSearchUs;
	gDx.ptr.stFound = gDx.ptr.stNotFound = 0; gDx.ptr.stSearchUs = 0;
	info->ptrRestores = gDx.ptr.stRestores; info->ptrGdiReads = gDx.ptr.stGdiReads; info->ptrGdiUs = gDx.ptr.stGdiUs; info->ptrMaskPx = gDx.ptr.maskPx;
	gDx.ptr.stRestores = gDx.ptr.stGdiReads = 0; gDx.ptr.stGdiUs = 0;

	// No image yet (the duplication gave nothing since it was opened): take the starting point from GDI. Changes after the
	// duplication was created arrive as rectangles later and are applied on top of it.
	if (!gDx.persistentValid)
	{
		if (capture_full_gdi() != 0) { return 1; }
		gDx.persistentValid = 1;
		fed = 1;
	}

	if (!gotImage && !fed && !changed && !forceFull && !wantImage && nPatch == 0)
	{
#ifdef KVM_STATS_LOG
		// Safety net of the measurement branch, see dx_stale_compare()
		if ((DWORD)(GetTickCount() - gDx.lastImageTick) >= KVM_STALE_QUIET_MS && (DWORD)(GetTickCount() - gDx.lastStaleTick) >= KVM_STATS_PERIOD_MS)
		{
			double t0 = dx_qpc_ms();
			long long diff = dx_stale_compare(sw, sh);
			info->staleUs = (unsigned int)((dx_qpc_ms() - t0) * 1000.0);
			gDx.lastStaleTick = GetTickCount();
			if (diff > 0 && gDx.staleSystematic)
			{
				// Known to differ permanently: measure and log only (no re-creation, no GDI frames)
				if (gDx.staleSystematicLogged < 5) { gDx.staleSystematicLogged++; kvm_stats_log("dxgi stale_systematic px=%lld", diff); }
			}
			else if (diff > 0)
			{
				info->staleSuspect = ++gDx.staleStrikes;
				if (gDx.staleAfterReinit)
				{
					// The picture of a freshly created duplication already differs: GDI and DXGI see the screen differently (HDR, a layered window...)
					gDx.staleSystematic = 1;
					gDx.staleSystematicLogged = 1;
					gDx.staleStrikes = 0;
					kvm_stats_log("dxgi stale_systematic px=%lld", diff);
				}
				else
				{
					kvm_stats_log("dxgi %s px=%lld box=%d,%d,%d,%d", gDx.staleStrikes >= 2 ? "stale" : "stale_suspect", diff, g_staleBox[0], g_staleBox[1], g_staleBox[2], g_staleBox[3]);
					if (gDx.staleStrikes >= 2)
					{
						// The duplication does not report what is on the screen: build it again, this frame comes from GDI (complete)
						gDx.staleStrikes = 0;
						gDx.staleAfterReinit = 1;
						dx_close();
						gDx.hasNextTry = 0;
						return 1;
					}
				}
			}
			else
			{
				gDx.staleStrikes = 0;
				gDx.staleAfterReinit = 0;
			}
		}
#endif
		// Nothing to send. A timeout was the wait for changes, the main loop does not need its pause after it.
		info->kind = KVM_FRAME_UNCHANGED;
		info->source = KVM_SOURCE_DXGI;
		info->idleWait = (r == DXGI_CAP_TIMEOUT && timeout > 0 && acquireMs >= (double)timeout / 2.0) ? 1 : 0; // Only if it really waited
		*buffer = NULL;
		*bufferSize = 0;
		*pixelSize = 4;
		return 0;
	}

	kind = (forceFull || f.fullCopy || fed) ? KVM_FRAME_FULL : KVM_FRAME_PARTIAL;
	memset(&cur, 0, sizeof(cur));
	outStart = dx_qpc_ms();
	if (SCALING_FACTOR == 1024)
	{
		outSize = (long long)sw * sh * 4;
		if ((out = malloc((size_t)outSize)) == NULL) { return 1; }
		memcpy(out, fullBits, (size_t)outSize);
		if (want) { drawn = kvm_cursor_xor((unsigned char*)out + ((size_t)(sh - 1) * sw * 4), -(ptrdiff_t)sw * 4, sw, sh, mouseMove[1], mouseMove[2], &cur); }
	}
	else if (scale_full_to_buffer(&out, &outSize, &outPixel) != 0)
	{
		return 1;
	}

	info->outUs = (unsigned int)((dx_qpc_ms() - outStart) * 1000.0);

	if (kind == KVM_FRAME_PARTIAL)
	{
		if ((map = (unsigned char*)calloc((size_t)TILE_HEIGHT_COUNT * TILE_WIDTH_COUNT, 1)) == NULL) { free(out); return 1; }
		for (int i = 0; i < f.rectCount; i++) { dx_mark_overlay(map, f.rects[i]); }
		for (int i = 0; i < nPatch; i++) { dx_mark_overlay(map, patchRect[i]); }
		if (SCALING_FACTOR == 1024)
		{
			// The overlay moved or changed: the tiles under the old and the new one
			if (gDx.ov.rectValid) { dx_mark_overlay(map, gDx.ov.rect); }
			if (drawn) { dx_mark_overlay(map, cur); }
		}
	}

	gDx.ov.want = want;
	gDx.ov.rectValid = drawn;
	gDx.ov.rect = cur;
	gDx.ov.cursor = ci.hCursor;
	gDx.ov.x = mouseMove[1];
	gDx.ov.y = mouseMove[2];

	*buffer = out;
	*bufferSize = outSize;
	*pixelSize = outPixel;
	info->kind = kind;
	info->source = KVM_SOURCE_DXGI;
	info->tileDirty = map;
	info->dirtyRects = f.dirtyCount;
	info->moveRects = f.moveCount;
	info->dirtyArea = f.dirtyArea;
	info->moveArea = f.moveArea;
	info->copyUs = f.copyUs;
	return 0;
}

// The DXGI and D3D11 calls are made in the agent's own process: a fault in a driver must not end the KVM session in an
// endless restart loop (the KVM process is restarted when it ends). On an exception DXGI is switched off for this process.
static int dxgi_frame_guarded(void **buffer, long long *bufferSize, long *mouseMove, int *pixelSize, int flags, kvm_frame_info *info, DWORD *code)
{
	__try
	{
		return kvm_dxgi_frame(buffer, bufferSize, mouseMove, pixelSize, flags, info);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		*code = GetExceptionCode();
		return -1;
	}
}

static void dxgi_shutdown_guarded(void)
{
	__try
	{
		dxgi_capture_shutdown();
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
}
#endif

// Extracts the required tile buffer from the desktop buffer
int get_tile_buffer(int x, int y, void **buffer, void *desktop, int tilewidth, int tileheight) 
{
	void *target = *buffer;
	for (int height = adjust_screen_size(SCALED_HEIGHT) - y - tileheight; height < adjust_screen_size(SCALED_HEIGHT) - y; height++)
	{
		memcpy_s(target, tilebuffersize, (const void *)((unsigned char *)desktop + (((height * adjust_screen_size(SCALED_WIDTH)) + x) * PIXEL_SIZE) ), (size_t)(tilewidth * PIXEL_SIZE));
		target = (void *) ((unsigned char *)target + tilewidth * PIXEL_SIZE);
	}
	return 0;
}

int tile_crc(int x, int y, void *desktop, int tilewidth, int tileheight) 
{
	int crc = 0;
	for (int height = adjust_screen_size(SCALED_HEIGHT) - y - tileheight; height < adjust_screen_size(SCALED_HEIGHT) - y; height++)
	{
		crc = util_crc(((unsigned char *)desktop + (((height * adjust_screen_size(SCALED_WIDTH)) + x) * PIXEL_SIZE) ), (size_t)(tilewidth * PIXEL_SIZE), crc);
	}
	return crc;
}

// Allocates a tile packet for an encoded image of jpegSize bytes and writes its header. The image goes at the returned offset.
char* alloc_tile_packet(int x, int y, DWORD jpegSize, long long *bufferSize, int *headerSize)
{
	char* Tile;
	*headerSize = (jpegSize > 65500 ? 16 : 8);
	*bufferSize = jpegSize + *headerSize;
	if ((Tile = (char*)ILibMemory_Allocate((int)*bufferSize, 0, NULL, NULL)) == NULL) return NULL;

	// Place the header
	if (jpegSize > 65500)
	{
		((unsigned short*)Tile)[0] = (unsigned short)htons((unsigned short)MNG_JUMBO);			// Write the type
		((unsigned short*)Tile)[1] = (unsigned short)htons((unsigned short)8);					// Write the size
		((unsigned int*)Tile)[1]   = (unsigned int)htonl(jpegSize + 8);						// Size of the Next Packet
		((unsigned short*)Tile)[4] = (unsigned short)htons((unsigned short)MNG_KVM_PICTURE);	// Write the type
		((unsigned short*)Tile)[5] = 0;														// RESERVED
		((unsigned short*)Tile)[6] = (unsigned short)htons((unsigned short)x);				// X position
		((unsigned short*)Tile)[7] = (unsigned short)htons((unsigned short)y);				// Y position
	}
	else
	{
		((unsigned short*)Tile)[0] = (unsigned short)htons((unsigned short)MNG_KVM_PICTURE);	// Write the type
		((unsigned short*)Tile)[1] = (unsigned short)htons((unsigned short)*bufferSize);		// Write the size
		((unsigned short*)Tile)[2] = (unsigned short)htons((unsigned short)x);				// X position
		((unsigned short*)Tile)[3] = (unsigned short)htons((unsigned short)y);				// Y position
	}
	return Tile;
}

#ifdef _TURBOJPEG
#ifdef KVM_ADAPT_ENABLED
static double tile_qpc_ms(void)
{
	static LARGE_INTEGER freq;
	LARGE_INTEGER t;
	if (freq.QuadPart == 0 && !QueryPerformanceFrequency(&freq)) { freq.QuadPart = 1; }
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}
#endif
static int g_tjOptimize = 0; // WA-2b: optimized Huffman tables for the next encode (set by the caller, see kvm_adapt_jpeg_opt)
// Encodes the tile as JPEG directly from the bottom-up desktop buffer using TurboJPEG. Returns 0 on success.
int calc_opt_compr_send_turbo(int x, int y, int captureWidth, int captureHeight, void* desktop, void ** buffer, long long *bufferSize, int quality, int subsamp)
{
	unsigned char *jpegBuf = NULL;
	size_t jpegSize = 0;
	int pitch = adjust_screen_size(SCALED_WIDTH) * PIXEL_SIZE;
	int headerSize;
	char* Tile;

	// The desktop buffer is bottom-up, so the first row in memory is the bottom row of the tile (same rows as get_tile_buffer).
	unsigned char *src = (unsigned char*)desktop + ((size_t)(adjust_screen_size(SCALED_HEIGHT) - y - captureHeight) * pitch) + ((size_t)x * PIXEL_SIZE);

	if (tjCompressor == NULL && (tjCompressor = tj3Init(TJINIT_COMPRESS)) == NULL) { KVMDEBUG("tj3Init() failed", 0); return 1; }
	if (tj3Set(tjCompressor, TJPARAM_QUALITY, quality) != 0 ||
		tj3Set(tjCompressor, TJPARAM_SUBSAMP, subsamp) != 0 ||
		tj3Set(tjCompressor, TJPARAM_FASTDCT, 1) != 0 ||
		tj3Set(tjCompressor, TJPARAM_OPTIMIZE, g_tjOptimize) != 0 ||
		tj3Set(tjCompressor, TJPARAM_BOTTOMUP, 1) != 0)
	{
		KVMDEBUG("tj3Set() failed", 0);
		return 1;
	}

	if (tj3Compress8(tjCompressor, src, captureWidth, pitch, captureHeight, PIXEL_SIZE == 4 ? TJPF_BGRX : TJPF_BGR, &jpegBuf, &jpegSize) != 0)
	{
		KVMDEBUG("tj3Compress8() failed", 0);
		tj3Free(jpegBuf);
		return 1;
	}

	if ((Tile = alloc_tile_packet(x, y, (DWORD)jpegSize, bufferSize, &headerSize)) == NULL)
	{
		tj3Free(jpegBuf);
		*bufferSize = 0;
		return 1;
	}
	memcpy_s(Tile + headerSize, (size_t)jpegSize, jpegBuf, (size_t)jpegSize);
	tj3Free(jpegBuf);

	*buffer = (unsigned char*)Tile;
	return 0;
}
#endif

// This function returns 0 and *buffer != NULL if everything was good. retval = jpegsize if the captured image was too large.
int calc_opt_compr_send(int x, int y, int captureWidth, int captureHeight, void* desktop, void ** buffer, long long *bufferSize, int quality, int subsamp) 
{
	BITMAPINFO bmpInfo;
	LARGE_INTEGER Offset;
	BITMAPFILEHEADER bmpFileHeader;
	int headerSize;
	*buffer = NULL;
	*bufferSize = 0;

	KVMDEBUG("calc_opt_compr_send()", 0);

#ifdef _TURBOJPEG
	// JPEG is encoded with TurboJPEG, if that fails we fall back to GDI+ below.
	if ((encType == 1 || encType == 4) && (PIXEL_SIZE == 4 || PIXEL_SIZE == 3))
	{
		if (calc_opt_compr_send_turbo(x, y, captureWidth, captureHeight, desktop, buffer, bufferSize, quality, subsamp) == 0) return 0;
	}
#endif

	// Get the bmpInfo structure
	bmpInfo = get_bmp_info(captureWidth, captureHeight);

	// Make sure a tile buffer is available. Most of the time, this is skipped.
	if (tilebuffersize != bmpInfo.bmiHeader.biSizeImage)
	{
		if (tilebuffer != NULL) free(tilebuffer);
		tilebuffersize = bmpInfo.bmiHeader.biSizeImage;
		if ((tilebuffer = malloc(tilebuffersize)) == NULL) return 0;
	}

	// Get the final coalesced tile
	get_tile_buffer(x, y, &tilebuffer, desktop, captureWidth, captureHeight);

	bmpFileHeader.bfReserved1 = 0;
	bmpFileHeader.bfReserved2 = 0;
	bmpFileHeader.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + bmpInfo.bmiHeader.biSizeImage;
	bmpFileHeader.bfType = 'MB';
	bmpFileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

	// Construct stream object.
	IStream* bmpStream = NULL;
	if (CreateStreamOnHGlobal(NULL, TRUE, (LPSTREAM*)&bmpStream) != S_OK)
	{
		KVMDEBUG("CreateStreamOnHGlobal() failed", 0);
		ILibCriticalLog(NULL, __FILE__, __LINE__, 252, GetLastError());
		return 0;
	}

	// Write entire contents of the source BMP into this stream.
	bmpStream->Write(&bmpFileHeader, sizeof(BITMAPFILEHEADER), NULL);
	bmpStream->Write(&bmpInfo, sizeof(BITMAPINFOHEADER), NULL);
	bmpStream->Write(tilebuffer, bmpInfo.bmiHeader.biSizeImage, NULL);

	// Move the stream pointer to the beginning of the stream.
	Offset.QuadPart = 0;
	if (bmpStream->Seek(Offset, STREAM_SEEK_SET, NULL) != S_OK)
	{
		KVMDEBUG("bmpStream->Seek() failed", 0);
		bmpStream->Release();
		ILibCriticalLog(NULL, __FILE__, __LINE__, 252, GetLastError());
		return 0;
	}

	// Construct GDI+ Image object from the BMP stream.
	Gdiplus::Image* DIBImage = Gdiplus::Image::FromStream(bmpStream);

	// Create stream to receive the encoded JPEG.
	IStream* jpegStream = NULL;
	if (CreateStreamOnHGlobal(NULL, TRUE, (LPSTREAM*)&jpegStream) != S_OK)
	{
		KVMDEBUG("CreateStreamOnHGlobal() failed", 0);
		delete DIBImage;
		bmpStream->Release();
		ILibCriticalLog(NULL, __FILE__, __LINE__, 252, GetLastError());
		return 0;
	}

	// Save image stream into the stream object.
	Status SaveStatus = DIBImage->Save(jpegStream, &encoderClsid, &encParam);
	if (SaveStatus != S_OK)
	{
		KVMDEBUG("DIBImage->Save() failed", 0);
		delete DIBImage;
		bmpStream->Release();
		jpegStream->Release();
		ILibCriticalLog(NULL, __FILE__, __LINE__, 252, GetLastError());
		return 0;
	}

	// Get the size of the output stream
	ULARGE_INTEGER Size;
	Offset.QuadPart = 0;
	if (jpegStream->Seek(Offset, STREAM_SEEK_END, &Size) != S_OK)
	{
		KVMDEBUG("jpegStream->Save() failed", 0);
		delete DIBImage;
		bmpStream->Release();
		jpegStream->Release();
		ILibCriticalLog(NULL, __FILE__, __LINE__, 252, GetLastError());
		return 0;
	}       

	// Move the image stream's pointer to its beginning.
	Offset.QuadPart = 0;
	if (jpegStream->Seek(Offset, STREAM_SEEK_SET, NULL) != S_OK)
	{
		KVMDEBUG("jpegStream->Seek() failed", 0);
		delete DIBImage;
		bmpStream->Release();
		jpegStream->Release();
		ILibCriticalLog(NULL, __FILE__, __LINE__, 252, GetLastError());
		return 0;
	}        

	// Check if the tile is too large to send
	DWORD jpegSize = (DWORD)Size.QuadPart;

	//if (jpegSize > 65500)
	//{
	//	KVMDEBUG("jpegSize > 65500", jpegSize);
	//	delete DIBImage;
	//	*bufferSize = 0;
	//	// ILibCriticalLog(NULL, __FILE__, __LINE__, 252, GetLastError());
	//	return jpegSize;
	//}

	// Save the image stream in memory.
	char* Tile = alloc_tile_packet(x, y, jpegSize, bufferSize, &headerSize);
	if (Tile == NULL || jpegStream->Read(Tile + headerSize, jpegSize, NULL) != S_OK)
	{
		KVMDEBUG("jpegStream->Read() failed", 0);
		delete DIBImage;
		if (Tile != NULL) free(Tile);
		*bufferSize = 0;
		bmpStream->Release();
		jpegStream->Release();
		ILibCriticalLog(NULL, __FILE__, __LINE__, 252, GetLastError());
		return 0;
	}

	// Cleanup
	delete DIBImage;
	bmpStream->Release();
	jpegStream->Release();

	*buffer = (unsigned char*)Tile;
	return 0;
}


extern "C"
{

// Quality and chroma subsampling for a tile class (kvm_adapt.h). Without WA-2 every tile gets the quality of the user and the normal subsampling.
#ifdef _TURBOJPEG
#define KVM_FULL_SUBSAMP ((int)KVM_TJ_SUBSAMP)
#else
#define KVM_FULL_SUBSAMP 0
#endif
static void tile_encode_params(int cls, int *quality, int *subsamp)
{
#ifdef KVM_ADAPT_ENABLED
	kvm_adapt_params(cls, (int)TJSAMP_420, KVM_FULL_SUBSAMP, quality, subsamp);
#else
	(void)cls;
	*quality = (int)encCompression;
	*subsamp = KVM_FULL_SUBSAMP;
#endif
}

// The time of the oldest held-back change among the tiles of the rectangle (0 = none of them was held back).
static unsigned int tile_rect_oldest_deferred(int row0, int col0, int row1, int col1)
{
#ifdef KVM_ADAPT_ENABLED
	unsigned int oldest = 0;
	for (int r = row0; r <= row1; r++)
	{
		for (int c = col0; c <= col1; c++)
		{
			if (KVM_TILE_DEFERRED(&tileInfo[r][c]) && (oldest == 0 || (int)(tileInfo[r][c].deferredAt - oldest) < 0)) { oldest = tileInfo[r][c].deferredAt; }
		}
	}
	return oldest;
#else
	(void)row0; (void)col0; (void)row1; (void)col1;
	return 0;
#endif
}

//Fetches the encoded jpeg tile at the given location. The neighboring tiles are coalesed to form a larger jpeg before returning.
int get_tile_at(int x, int y, void** buffer, long long *bufferSize, void *desktop, int row, int col)
{	
	int CRC;
	int rightcol = col;		// Used in coalescing. Indicates the right-most column to be coalesced.
	int botrow = row;		// Used in coalescing. Indicates the bottom-most row to be coalesced.
	int r_x = x;
	int r_y = y;
	int captureWidth = TILE_WIDTH;
	int captureHeight = TILE_HEIGHT;
	int startClass, quality, subsamp;

	*buffer = NULL;			// If anything fails, this will be the indication.
	*bufferSize = 0;

	if (tileInfo[row][col].flags == (char)TILE_TODO) // First check whether the tile-crc needs to be calcualted or not.
	{ 
		// Compute CRC on the contents of the bitmap; Proceed with image encoding only if the CRC is different.
		CRC = tile_crc(x, y, desktop, TILE_WIDTH, TILE_HEIGHT);
		kvm_adapt_note_crc(&tileInfo[row][col], CRC); // WA-2: the content of the tile was looked at (tileInfo.crc still has the old value)
		if (CRC == tileInfo[row][col].crc && !KVM_TILE_DEFERRED(&tileInfo[row][col])) return 0;
		tileInfo[row][col].crc = CRC; // Update the tile CRC in the global data structure.
	}

	tileInfo[row][col].flags = (char)TILE_MARKED_NOT_SENT;
	startClass = kvm_adapt_class(&tileInfo[row][col]); // Only tiles of the same class are coalesced into one rectangle (one quality)


	// COALESCING SECTION

	// First got to the right most changed tile and record it
	const int maxRectTiles = kvm_adapt_max_rect_tiles(); // under a budget a rectangle stays small enough for the bucket to ration the picture in pieces
	while (rightcol + 1 < TILE_WIDTH_COUNT)
	{
		if (maxRectTiles > 0 && (rightcol + 2 - col) > maxRectTiles) { break; }
		rightcol++;
		r_x = rightcol * TILE_WIDTH;
		
		CRC = tileInfo[row][rightcol].crc;

		if (tileInfo[row][rightcol].flags == (char)TILE_TODO) {
			// Compute CRC on the contents of the bitmap.
			CRC = tile_crc(r_x, y, desktop, TILE_WIDTH, TILE_HEIGHT);
			kvm_adapt_note_crc(&tileInfo[row][rightcol], CRC);
		}

		if (CRC != tileInfo[row][rightcol].crc || tileInfo[row][rightcol].flags == (char)TILE_MARKED_NOT_SENT || KVM_TILE_DEFERRED(&tileInfo[row][rightcol])) // If the tile has changed, increment the capturewidth.
		{
			tileInfo[row][rightcol].crc = CRC; 
			if (kvm_adapt_class(&tileInfo[row][rightcol]) != startClass)
			{
				// Changed, but another quality: it is sent by itself later (marked, not "don't send": the change must not get lost)
				tileInfo[row][rightcol].flags = (char)TILE_MARKED_NOT_SENT;
				--rightcol;
				break;
			}
			// Here we check whether the size of the coalesced bitmap is greater than the threshold (65500)
			//if ((captureWidth + TILE_WIDTH) * TILE_HEIGHT * PIXEL_SIZE / COMPRESSION_RATIO > 65500) { 
			//	tileInfo[row][rightcol].flags = (char)TILE_MARKED_NOT_SENT;
			//	--rightcol;
			//	break;
			//}

			tileInfo[row][rightcol].flags = (char)TILE_MARKED_NOT_SENT;
			captureWidth += TILE_WIDTH;
		} 
		else
		{
			tileInfo[row][rightcol].flags = (char)TILE_DONT_SEND;
			--rightcol;
			break;
		}
	}

	// int TOLERANCE = (rightcol - col) / 4;

	// Now go to the bottom tiles, check if they have changed and record them
	//while ((botrow + 1 < TILE_HEIGHT_COUNT) && ((captureHeight + TILE_HEIGHT) * captureWidth * PIXEL_SIZE / COMPRESSION_RATIO <= 65500))
	while ((botrow + 1 < TILE_HEIGHT_COUNT))
	{
		if (maxRectTiles > 0 && (botrow + 2 - row) * (rightcol - col + 1) > maxRectTiles) { break; }
		botrow++;
		r_y = botrow * TILE_HEIGHT;
		int fail = 0;
		r_x = x;

		// int missCount = 0;

		for (int rcol = col; rcol <= rightcol; rcol++) {

			CRC = tileInfo[botrow][rcol].crc;
			if (tileInfo[botrow][rcol].flags == (char)TILE_TODO)
			{
				// Compute CRC on the contents of the bitmap; Proceed with image encoding only if the CRC is different.
				CRC = tile_crc(r_x, r_y, desktop, TILE_WIDTH, TILE_HEIGHT);
				kvm_adapt_note_crc(&tileInfo[botrow][rcol], CRC);
			}

			if (CRC != tileInfo[botrow][rcol].crc || tileInfo[botrow][rcol].flags == (char)TILE_MARKED_NOT_SENT || KVM_TILE_DEFERRED(&tileInfo[botrow][rcol]))
			{
				tileInfo[botrow][rcol].flags = (char)TILE_MARKED_NOT_SENT;
				tileInfo[botrow][rcol].crc = CRC;
				if (kvm_adapt_class(&tileInfo[botrow][rcol]) != startClass)
				{
					// Changed, but another quality: this row does not join the rectangle; the tile stays marked and is sent by itself later
					fail = 1;
					botrow--;
					break;
				}
				r_x += TILE_WIDTH;
			}
			else
			{
				/*// Keep this part commented. Adding tolerance adds to the complexity of this code.
				missCount++;

				if (missCount > TOLERANCE) {
					fail = 1;
					for (int i = col; i < rcol; i++) {
						if (tileInfo[botrow][i].flags == (char)TILE_SKIPPED) {
							tileInfo[botrow][i].flags = (char)TILE_DONT_SEND;
						} 
						else {
							tileInfo[botrow][i].flags = (char)TILE_MARKED_NOT_SENT;
						}
					}
					tileInfo[botrow][rcol].flags = (char)TILE_DONT_SEND;
					botrow--;
					break;
				}
				else {
					tileInfo[botrow][rcol].flags = (char)TILE_SKIPPED;
					tileInfo[botrow][rcol].crc = CRC;
					r_x += TILE_WIDTH;
				}*/
				fail = 1;
				for (int i = col; i < rcol; i++)
				{
					tileInfo[botrow][i].flags = (char)TILE_MARKED_NOT_SENT;
				}
				tileInfo[botrow][rcol].flags = (char)TILE_DONT_SEND;
				botrow--;
				break;
			}
		}

		if (!fail)
		{
			captureHeight += TILE_HEIGHT;
		}
		else
		{
			break;
		}
	}

	// WA-2b: the motion frame limit holds this rectangle back. The tiles keep their new CRC and are marked as deferred: the next frame looks at them
	// again whether or not the capture lists them as changed (the change must not get lost), and sends them in the class they were held back in.
	// The same holds back what the budget (byte bucket) does not allow now: it is sent later, in the quality of the ladder step, and sharpened after that.
	if (kvm_adapt_gate_closed(startClass) || kvm_adapt_bucket_closed((botrow - row + 1) * (rightcol - col + 1), tile_rect_oldest_deferred(row, col, botrow, rightcol)))
	{
		for (int r = row; r <= botrow; r++)
		{
			for (int c = col; c <= rightcol; c++) { tileInfo[r][c].flags = (char)TILE_SENT; }
		}
		kvm_adapt_defer(row, col, botrow, rightcol);
		return 0;
	}

	int retval = 0;
	int firstTime = 1;
	// Under a budget, at a low ladder step, a larger rectangle of the full class is sent in the quality of the step as well (and sharpened later)
	int encClass = kvm_adapt_force_motion((botrow - row + 1) * (rightcol - col + 1)) ? 1 : startClass;
#ifdef KVM_ADAPT_ENABLED
	double encStart = tile_qpc_ms();
#endif
#ifdef _TURBOJPEG
	g_tjOptimize = kvm_adapt_jpeg_opt(0, (botrow - row + 1) * (rightcol - col + 1));
#endif

	// This loop is used to adjust the COMPRESSION_RATIO. This loop runs only once most of the time.
	do {
		// retval here is 0 if everything was good. It is > 0 if it contains the size of the jpeg that was created and not sent.
		tile_encode_params(encClass, &quality, &subsamp);
		retval = calc_opt_compr_send(x, y, captureWidth, captureHeight, desktop, buffer, bufferSize, quality, subsamp);
		if (retval == 0 && *bufferSize == 0) break;
		if (retval != 0)
		{
			if (firstTime)
			{
				// Re-adjust the compression ratio.
				//COMPRESSION_RATIO = (int)(((double)COMPRESSION_RATIO/(double)retval) * 60000);//Magic number: 60000 ~= 65500
				//if (COMPRESSION_RATIO <= 1) COMPRESSION_RATIO = 2;
				firstTime = 0;
			}

			if (botrow > row) // First time, try reducing the height.
			{ 
				botrow = row + ((botrow - row + 1) / 2);
				captureHeight = (botrow - row + 1) * TILE_HEIGHT;
			}
			else if (rightcol > col) // If it is not possible, reduce the width
			{
				rightcol = col + ((rightcol - col + 1) / 2);
				captureWidth = (rightcol - col + 1) * TILE_WIDTH;
			} 
			else
			{   // This never happens, but just in case.
				retval = 0;
				break;
			}

		}
	} while (*buffer == NULL);

	// Set the flags to TILE_SENT
	if (*buffer != NULL) {
#ifdef KVM_ADAPT_ENABLED
		kvm_adapt_note_encode(0, (unsigned int)((tile_qpc_ms() - encStart) * 1000.0), (botrow - row + 1) * (rightcol - col + 1));
#endif
		for (int r = row; r <= botrow; r++) {
			for (int c = col; c <= rightcol; c++) {
				tileInfo[r][c].flags = (char)TILE_SENT;
			}
		}
		kvm_adapt_sent(row, col, botrow, rightcol, encClass, quality, subsamp != KVM_FULL_SUBSAMP, *bufferSize);
	}

	return retval;
}

#ifdef KVM_ADAPT_ENABLED
// WA-2 sharpening: encodes tiles of the current picture with the quality of the user and the normal subsampling. Returns 0 and a packet in *buffer on success.
int kvm_adapt_encode_sharp(int x, int y, int w, int h, void *desktop, void **buffer, long long *bufferSize, int *quality)
{
	double encStart = tile_qpc_ms();
	int res;
	*quality = (encCompression < 1) ? 1 : (encCompression > 90 ? 90 : (int)encCompression);
#ifdef _TURBOJPEG
	g_tjOptimize = kvm_adapt_jpeg_opt(1, 0);
#endif
	res = (calc_opt_compr_send(x, y, w, h, desktop, buffer, bufferSize, *quality, KVM_FULL_SUBSAMP) == 0 && *buffer != NULL) ? 0 : 1;
	if (res == 0) { kvm_adapt_note_encode(1, (unsigned int)((tile_qpc_ms() - encStart) * 1000.0), (w / TILE_WIDTH) * (h / TILE_HEIGHT)); }
	return res;
}
#endif

// This function captures the entire desktop buffer to scan. It runs on the capture thread while the main thread may
// still encode the previous frame, so it reports the bytes per pixel of this frame in *pixelSize instead of changing
// PIXEL_SIZE; the main loop sets PIXEL_SIZE when it starts encoding this frame.
int get_desktop_buffer(void **buffer, long long *bufferSize, long* mouseMove, int *pixelSize, int flags, kvm_frame_info *info)
{
	BITMAPINFO bmpInfo;

	UNREFERENCED_PARAMETER(flags);
	memset(info, 0, sizeof(*info)); // GDI frames are always complete: kind = KVM_FRAME_FULL, source = KVM_SOURCE_GDI
	info->kind = KVM_FRAME_FULL;
	info->source = KVM_SOURCE_GDI;
	*buffer = NULL; // If anything fails, this will be the indication.
	*bufferSize = 0;
	*pixelSize = 0;

	if (hDesktopDC) ReleaseDC(NULL, hDesktopDC);
	if ((hDesktopDC = GetDC(NULL)) == NULL) { KVMDEBUG("GetDC(NULL) returned NULL", 0); return 1; } // We need to do this incase the current desktop changes.

	// reset_capture_bitmap() was called (e.g. desktop switch): the full resolution bitmap is created again when it is needed.
	if (InterlockedExchange(&captureResetPending, 0) != 0)
	{
		free_full_capture();
#ifdef KVM_DXGI_ENABLED
		dx_reset();
#endif
	}

#ifdef KVM_DXGI_ENABLED
	{
		DWORD exceptionCode = 0;
		int dxr = dxgi_frame_guarded(buffer, bufferSize, mouseMove, pixelSize, flags, info, &exceptionCode);
		if (dxr == 0) { return 0; }
		if (dxr < 0)
		{
			gDx.state = DXS_DISABLED;
			dxgi_shutdown_guarded();
			gDx.persistentValid = 0;
			info->dxgiEvent |= KVM_DXGI_EVENT_DISABLED;
			kvm_stats_log("dxgi disabled reason=exception code=0x%08x", (unsigned int)exceptionCode);
		}
		// This frame comes from GDI (a complete frame); only the DXGI events are passed on
		{
			int events = info->dxgiEvent;
			if (info->tileDirty != NULL && dxr > 0) { free(info->tileDirty); }
			memset(info, 0, sizeof(*info));
			info->kind = KVM_FRAME_FULL;
			info->source = KVM_SOURCE_GDI;
			info->dxgiEvent = events;
			*buffer = NULL;
			*bufferSize = 0;
			*pixelSize = 0;
		}
	}
#endif

	// Downscaling: capture at full resolution and scale on the CPU. Falls back to StretchBlt() if that fails.
	if (SCALING_FACTOR != 1024 && adjust_screen_size(SCALED_WIDTH) <= adjust_screen_size(SCREEN_WIDTH) && adjust_screen_size(SCALED_HEIGHT) <= adjust_screen_size(SCREEN_HEIGHT))
	{
		if (get_scaled_desktop_buffer(buffer, bufferSize, pixelSize) == 0) return 0;
		*buffer = NULL;
		*bufferSize = 0;
	}
	if (hCapturedBitmap == NULL || hCapturedBitmapWidth != adjust_screen_size(SCALED_WIDTH) || hCapturedBitmapHeight != adjust_screen_size(SCALED_HEIGHT))
	{
		if (hCapturedBitmap)
		{
			if (hCaptureDCDefaultBitmap) SelectObject(hCaptureDC, hCaptureDCDefaultBitmap);
			DeleteObject(hCapturedBitmap);
			hCapturedBitmap = NULL;
		}
		hCapturedBitmapWidth = hCapturedBitmapHeight = 0;
		if ((hCapturedBitmap = CreateCompatibleBitmap(hDesktopDC, adjust_screen_size(SCALED_WIDTH), adjust_screen_size(SCALED_HEIGHT))) == NULL)
		{
			KVMDEBUG("CreateCompatibleBitmap() returned NULL", 0);
			return 1;
		}
		hCapturedBitmapWidth = adjust_screen_size(SCALED_WIDTH);
		hCapturedBitmapHeight = adjust_screen_size(SCALED_HEIGHT);
	}
	
	if (SelectObject(hCaptureDC, hCapturedBitmap) == NULL) { KVMDEBUG("SelectObject() failed", 0); return(1); }
	if (SCALING_FACTOR == 1024)
	{
		if (BitBlt(hCaptureDC, 0, 0, adjust_screen_size(SCREEN_WIDTH), adjust_screen_size(SCREEN_HEIGHT), hDesktopDC, SCREEN_X, SCREEN_Y, SRCCOPY) == FALSE)
		{
			KVMDEBUG("BitBlt() returned FALSE", 0);
			return 1; // If the copy fails, error out.
		}
		if (mouseMove[0] != 0)
		{
			CURSORINFO info = { 0 };
			BITMAP bm;
			ICONINFO ii;
			info.cbSize = sizeof(info);
			ZeroMemory(&ii, sizeof(ii));
			GetCursorInfo(&info);
			GetIconInfo(info.hCursor, &ii);
			if (GetObject(ii.hbmMask, sizeof(bm), &bm) == sizeof(bm))
			{
				HDC hdcScreen = GetDC(NULL);
				if (hdcScreen != NULL)
				{
					HDC hdcMem = CreateCompatibleDC(hdcScreen);
					HBITMAP hbmCanvas = CreateCompatibleBitmap(hdcScreen, bm.bmWidth, ii.hbmColor ? bm.bmHeight : (bm.bmHeight / 2));
					if(hdcMem!=NULL && hbmCanvas != NULL)
					{
						HGDIOBJ hbmold = SelectObject(hdcMem, hbmCanvas);

						DrawIconEx(hdcMem, 0, 0, info.hCursor, bm.bmWidth, ii.hbmColor ? bm.bmHeight : (bm.bmHeight / 2), 0, NULL, DI_NORMAL);
						BitBlt(hCaptureDC, mouseMove[1], mouseMove[2], bm.bmWidth, ii.hbmColor ? bm.bmHeight : (bm.bmHeight / 2), hdcMem, 0, 0, SRCINVERT);

						SelectObject(hdcMem, hbmold);
					}
					if (hbmCanvas != NULL) { DeleteObject(hbmCanvas); }
					if (hdcMem != NULL) { DeleteDC(hdcMem); }
					ReleaseDC(NULL, hdcScreen);
				}
			}
			// GetIconInfo() hands over copies of the cursor bitmaps, the caller has to delete them.
			if (ii.hbmMask != NULL) { DeleteObject(ii.hbmMask); }
			if (ii.hbmColor != NULL) { DeleteObject(ii.hbmColor); }
		}
	}
	else
	{
		if (SetStretchBltMode(hCaptureDC, KVM_STRETCH_MODE) == 0) { KVMDEBUG("SetStretchBltMode() failed", 0); return(1); }
		if (StretchBlt(hCaptureDC, 0, 0, adjust_screen_size(SCALED_WIDTH), adjust_screen_size(SCALED_HEIGHT), hDesktopDC, SCREEN_X, SCREEN_Y, adjust_screen_size(SCREEN_WIDTH), adjust_screen_size(SCREEN_HEIGHT), SRCCOPY) == FALSE)
		{
			KVMDEBUG("StretchBlt() returned FALSE", 0);
			return 1; // If the copy fails, error out.
		}
	}

	ZeroMemory(&bmpInfo, sizeof(BITMAPINFO));
	bmpInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);

	// Populates some fields in the bmpInfo struct based on the properties of the bitmap.
	if (GetDIBits(hDesktopDC, hCapturedBitmap, 0, 0, NULL, &bmpInfo, DIB_RGB_COLORS) == 0)
	{
		KVMDEBUG("GetDIBits() failed", 0);
		ILibCriticalLog(NULL, __FILE__, __LINE__, 252, GetLastError());
		return(1);
	}

	if (bmpInfo.bmiHeader.biSizeImage <= 0)
	{
		bmpInfo.bmiHeader.biSizeImage = bmpInfo.bmiHeader.biWidth * abs(bmpInfo.bmiHeader.biHeight) * (bmpInfo.bmiHeader.biBitCount + 7) / 8;
	}

	*bufferSize = bmpInfo.bmiHeader.biSizeImage;
	*pixelSize = bmpInfo.bmiHeader.biBitCount / 8;
	if ((*buffer = malloc((size_t)*bufferSize)) == NULL) { KVMDEBUG("malloc() failed", 0); return 1; }
	
	bmpInfo.bmiHeader.biCompression = BI_RGB;
	if (GetDIBits(hDesktopDC, hCapturedBitmap, 0, bmpInfo.bmiHeader.biHeight, *buffer, &bmpInfo, DIB_RGB_COLORS) == 0) { KVMDEBUG("GetDIBits() failed", 0); free(*buffer); return(1); }

	return 0;
}

// Checks that a captured frame holds at least the area the tile code reads with the given bytes per pixel.
int desktop_buffer_fits(long long bufferSize, int pixelSize)
{
	if (pixelSize < 1 || pixelSize > 4) return 0;
	return bufferSize >= (long long)adjust_screen_size(SCALED_WIDTH) * adjust_screen_size(SCALED_HEIGHT) * pixelSize;
}

// Creates a BITMAPINFO object with required width and height
BITMAPINFO get_bmp_info(int width, int height)
{
	BITMAPINFO bmpInfo;

	ZeroMemory(&bmpInfo, sizeof(BITMAPINFO));
	bmpInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmpInfo.bmiHeader.biBitCount = (WORD)(PIXEL_SIZE * 8);
	bmpInfo.bmiHeader.biSize = 40;
	bmpInfo.bmiHeader.biHeight = height;
	bmpInfo.bmiHeader.biWidth = width;
	bmpInfo.bmiHeader.biSizeImage = height * width * PIXEL_SIZE;
	bmpInfo.bmiHeader.biPlanes = 1;

	return bmpInfo;
}

short initialize_gdiplus()
{
	GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

	TILE_WIDTH = 32;
	TILE_HEIGHT = 32;
	COMPRESSION_RATIO = 100;
	FRAME_RATE_TIMER = 50;
	SCALING_FACTOR = 1024;
	SCALING_FACTOR_NEW = 1024;

	SCALED_WIDTH = SCREEN_WIDTH = GetSystemMetrics(SM_CXSCREEN);
	SCALED_HEIGHT = SCREEN_HEIGHT = GetSystemMetrics(SM_CYSCREEN);

	if ((hDesktopDC = GetDC(NULL)) == NULL) { KVMDEBUG("GetDC() failed", 0); return 0; }
	if ((hCaptureDC = CreateCompatibleDC(hDesktopDC)) == NULL) { KVMDEBUG("CreateCompatibleDC() failed", 0); return 0; }
	if ((hCapturedBitmap = CreateCompatibleBitmap(hDesktopDC, SCALED_WIDTH, SCALED_HEIGHT)) == NULL) { KVMDEBUG("CreateCompatibleBitmap() failed", 0); return 0; }
	hCapturedBitmapWidth = SCALED_WIDTH;
	hCapturedBitmapHeight = SCALED_HEIGHT;
	if ((hCaptureDCDefaultBitmap = SelectObject(hCaptureDC, hCapturedBitmap)) == NULL) { KVMDEBUG("SelectObject() failed", 0); }
	
	// Find encoder and setup encoder parameters
	encType = 1;
	GetEncoderClsid(L"image/jpeg", &encoderClsid);
	encParam.Count = 1;
	encParam.Parameter[0].Guid = EncoderQuality;
	encParam.Parameter[0].Type = EncoderParameterValueTypeLong;
	encParam.Parameter[0].NumberOfValues = 1;
	encParam.Parameter[0].Value = &encCompression;

	return 1;
}

void teardown_gdiplus()
{
	if (tilebuffer) free(tilebuffer);
	tilebuffersize = 0;
	tilebuffer = NULL;
#ifdef _TURBOJPEG
	if (tjCompressor != NULL) { tj3Destroy(tjCompressor); tjCompressor = NULL; }
#endif
	GdiplusShutdown(gdiplusToken);
	DeleteDC(hCaptureDC);
	hCaptureDCDefaultBitmap = NULL;
	DeleteObject(hCapturedBitmap);
	hCapturedBitmap = NULL;
	hCapturedBitmapWidth = hCapturedBitmapHeight = 0;
	free_full_capture();
	if (hFullDC) { DeleteDC(hFullDC); hFullDC = NULL; }
	free_scale_tables();
	if (hDesktopDC) ReleaseDC(NULL, hDesktopDC);
	hDesktopDC = NULL;
}

// Releases the screen DC, it must be released on the thread that got it.
void release_desktop_dc()
{
	if (hDesktopDC) ReleaseDC(NULL, hDesktopDC);
	hDesktopDC = NULL;
}

#ifdef KVM_DXGI_ENABLED
// The capture thread ends: close the duplication, it belongs to this thread.
void kvm_dxgi_thread_end(void)
{
	dxgi_shutdown_guarded();
	if (gDx.state == DXS_OPEN) { gDx.state = DXS_IDLE; }
	gDx.persistentValid = 0;
	gDx.ov.want = gDx.ov.rectValid = 0;
	DX_PTR_RESET();
}
#endif

#ifdef KVM_STATS_LOG
// JPEG chroma subsampling in use (TJSAMP_* value), for the statistics log.
int kvm_subsamp_value(void)
{
#ifdef _TURBOJPEG
	return (int)KVM_TJ_SUBSAMP;
#else
	return -1;
#endif
}
#endif

// Forces the capture bitmap to be re-created on the next frame (e.g. after a desktop switch).
void reset_capture_bitmap()
{
	hCapturedBitmapWidth = hCapturedBitmapHeight = 0;
	InterlockedExchange(&captureResetPending, 1); // May be called from the input thread, the capture thread re-creates the DIB section
}

void set_tile_compression(int type, int level)
{
	encCompression = level;
	if (encCompression < 1) { encCompression = 1; } // Guard against bad values.
	if (encCompression > 90) { encCompression = 90; }
	if (type >= 1 && type <= 4) { encType = type; }
	// Note: no early return when tilebuffer == NULL, with TurboJPEG the tile buffer is never allocated for JPEG.
	KVMDEBUG("set_tile_compression() type", type);
	KVMDEBUG("set_tile_compression() level", level);

	switch (type)
	{
		case 1: { GetEncoderClsid(L"image/jpeg", &encoderClsid); break; }
		case 2: { GetEncoderClsid(L"image/png", &encoderClsid); break; }
		case 3: { GetEncoderClsid(L"image/tiff", &encoderClsid); break; }
	}
}

}

#endif
