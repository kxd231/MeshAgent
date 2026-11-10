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
} gDx;

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
		}
		SelectObject(dc, old);
	}
	if (bmp != NULL) { DeleteObject(bmp); }
	DeleteDC(dc);
	return diff;
}
#endif

// Returns 0 when the frame was produced (*buffer, or KVM_FRAME_UNCHANGED), 1 when this frame has to be captured with GDI.
// On 1 nothing is allocated and the caller captures a complete GDI frame.
static int kvm_dxgi_frame(void **buffer, long long *bufferSize, long *mouseMove, int *pixelSize, int flags, kvm_frame_info *info)
{
	char desk[64];
	int sw, sh, want, changed = 0, gotImage = 0, fed = 0, kind, drawn = 0;
	int forceFull = (flags & KVM_CAPTURE_FORCE_FULL) != 0;
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
			break;
		case DXGI_CAP_NO_CHANGE:
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

	// No image yet (the duplication gave nothing since it was opened): take the starting point from GDI. Changes after the
	// duplication was created arrive as rectangles later and are applied on top of it.
	if (!gDx.persistentValid)
	{
		if (capture_full_gdi() != 0) { return 1; }
		gDx.persistentValid = 1;
		fed = 1;
	}

	if (!gotImage && !fed && !changed && !forceFull)
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
					kvm_stats_log("dxgi %s px=%lld", gDx.staleStrikes >= 2 ? "stale" : "stale_suspect", diff);
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
// Encodes the tile as JPEG directly from the bottom-up desktop buffer using TurboJPEG. Returns 0 on success.
int calc_opt_compr_send_turbo(int x, int y, int captureWidth, int captureHeight, void* desktop, void ** buffer, long long *bufferSize)
{
	unsigned char *jpegBuf = NULL;
	size_t jpegSize = 0;
	int pitch = adjust_screen_size(SCALED_WIDTH) * PIXEL_SIZE;
	int headerSize;
	char* Tile;

	// The desktop buffer is bottom-up, so the first row in memory is the bottom row of the tile (same rows as get_tile_buffer).
	unsigned char *src = (unsigned char*)desktop + ((size_t)(adjust_screen_size(SCALED_HEIGHT) - y - captureHeight) * pitch) + ((size_t)x * PIXEL_SIZE);

	if (tjCompressor == NULL && (tjCompressor = tj3Init(TJINIT_COMPRESS)) == NULL) { KVMDEBUG("tj3Init() failed", 0); return 1; }
	if (tj3Set(tjCompressor, TJPARAM_QUALITY, (int)encCompression) != 0 ||
		tj3Set(tjCompressor, TJPARAM_SUBSAMP, KVM_TJ_SUBSAMP) != 0 ||
		tj3Set(tjCompressor, TJPARAM_FASTDCT, 1) != 0 ||
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
int calc_opt_compr_send(int x, int y, int captureWidth, int captureHeight, void* desktop, void ** buffer, long long *bufferSize) 
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
		if (calc_opt_compr_send_turbo(x, y, captureWidth, captureHeight, desktop, buffer, bufferSize) == 0) return 0;
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

	*buffer = NULL;			// If anything fails, this will be the indication.
	*bufferSize = 0;

	if (tileInfo[row][col].flags == (char)TILE_TODO) // First check whether the tile-crc needs to be calcualted or not.
	{ 
		// Compute CRC on the contents of the bitmap; Proceed with image encoding only if the CRC is different.
		if ((CRC = tile_crc(x, y, desktop, TILE_WIDTH, TILE_HEIGHT)) == tileInfo[row][col].crc) return 0;
		tileInfo[row][col].crc = CRC; // Update the tile CRC in the global data structure.
	}

	tileInfo[row][col].flags = (char)TILE_MARKED_NOT_SENT;


	// COALESCING SECTION

	// First got to the right most changed tile and record it
	while (rightcol + 1 < TILE_WIDTH_COUNT)
	{
		rightcol++;
		r_x = rightcol * TILE_WIDTH;
		
		CRC = tileInfo[row][rightcol].crc;

		if (tileInfo[row][rightcol].flags == (char)TILE_TODO) {
			// Compute CRC on the contents of the bitmap.
			CRC = tile_crc(r_x, y, desktop, TILE_WIDTH, TILE_HEIGHT);
		}

		if (CRC != tileInfo[row][rightcol].crc || tileInfo[row][rightcol].flags == (char)TILE_MARKED_NOT_SENT) // If the tile has changed, increment the capturewidth.
		{
			tileInfo[row][rightcol].crc = CRC; 
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
			}

			if (CRC != tileInfo[botrow][rcol].crc || tileInfo[botrow][rcol].flags == (char)TILE_MARKED_NOT_SENT)
			{
				tileInfo[botrow][rcol].flags = (char)TILE_MARKED_NOT_SENT;
				tileInfo[botrow][rcol].crc = CRC;
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

	int retval = 0;
	int firstTime = 1;

	// This loop is used to adjust the COMPRESSION_RATIO. This loop runs only once most of the time.
	do {
		// retval here is 0 if everything was good. It is > 0 if it contains the size of the jpeg that was created and not sent.
		retval = calc_opt_compr_send(x, y, captureWidth, captureHeight, desktop, buffer, bufferSize);
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
		for (int r = row; r <= botrow; r++) {
			for (int c = col; c <= rightcol; c++) {
				tileInfo[r][c].flags = (char)TILE_SENT;
			}
		}
	}

	return retval;
}

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
