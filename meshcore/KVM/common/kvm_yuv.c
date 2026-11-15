/*
KVM: BGRA to NV12 conversion (BT.709, limited range). See kvm_yuv.h for the contract.

Fixed point: Y uses coefficients with 15 fractional bits on one pixel; Cb and Cr use the same scale on the SUM of the four pixels of a
2x2 block (so the shift is 17 = 15 + 2 and the average comes for free). All sums are kept positive by adding the offsets (16 or 128)
before the shift, so no right shift of a negative number is involved and the SSE2 and scalar results are identical by construction:
both run the same integer formulas, the SSE2 loops only cover the full groups of 16 columns, the scalar code does the rest (right edge,
replicated padding).
*/

#include "kvm_yuv.h"

#include <limits.h>
#include <string.h>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define KVM_YUV_SSE2_COMPILED 1
#else
#define KVM_YUV_SSE2_COMPILED 0
#endif

/* BT.709 limited range, 15 fractional bits. The chroma rows sum to 0 so that grey gives exactly 128. */
#define YR 5983
#define YG 20127
#define YB 2032
#define Y_OFF ((16 << 15) + (1 << 14))
#define UR (-3298)
#define UG (-11094)
#define UB 14392
#define VR 14392
#define VG (-13072)
#define VB (-1320)
#define C_OFF ((128 << 17) + (1 << 16))
#define MAX_DIM 16384
/* Two signed 16 bit coefficients in one dword (pmaddwd operand): low word first. */
#define PAIR(lo, hi) ((int)(((unsigned)(hi) << 16) | ((unsigned)(lo) & 0xFFFFu)))

int kvm_yuv_coded(int v)
{
	return (v + (KVM_YUV_ALIGN - 1)) & ~(KVM_YUV_ALIGN - 1);
}

size_t kvm_yuv_nv12_size(int codedW, int codedH)
{
	return (size_t)codedW * (size_t)codedH * 3u / 2u;
}

int kvm_yuv_have_sse2(void)
{
	return KVM_YUV_SSE2_COMPILED;
}

/* ------------------------------------------------------------------------------------------------------------- scalar */

/* Y of the pixels x0 .. codedW-1 of one row; columns >= width repeat the last pixel. */
static void y_row_scalar(const uint8_t *src, int width, uint8_t *dst, int x0, int codedW)
{
	int x;
	for (x = x0; x < width; x++)
	{
		const uint8_t *p = src + (size_t)x * 4;
		dst[x] = (uint8_t)((YB * p[0] + YG * p[1] + YR * p[2] + Y_OFF) >> 15);
	}
	for (x = (x0 > width ? x0 : width); x < codedW; x++) { dst[x] = dst[width - 1]; }
}

/* Cb/Cr of the chroma samples i0 .. codedW/2-1 of one row pair; samples beyond the source repeat the last one. */
static void uv_row_scalar(const uint8_t *r0, const uint8_t *r1, int width, uint8_t *dst, int i0, int codedW)
{
	const int nreal = (width + 1) / 2;
	int i;
	for (i = i0; i < nreal; i++)
	{
		const size_t xa = (size_t)2 * i * 4;
		const size_t xb = (size_t)(2 * i + 1 < width ? 2 * i + 1 : width - 1) * 4;
		const int b = r0[xa] + r0[xb] + r1[xa] + r1[xb];
		const int g = r0[xa + 1] + r0[xb + 1] + r1[xa + 1] + r1[xb + 1];
		const int r = r0[xa + 2] + r0[xb + 2] + r1[xa + 2] + r1[xb + 2];
		dst[2 * i] = (uint8_t)((UB * b + UG * g + UR * r + C_OFF) >> 17);
		dst[2 * i + 1] = (uint8_t)((VB * b + VG * g + VR * r + C_OFF) >> 17);
	}
	for (i = (i0 > nreal ? i0 : nreal); i < codedW / 2; i++)
	{
		dst[2 * i] = dst[2 * (nreal - 1)];
		dst[2 * i + 1] = dst[2 * (nreal - 1) + 1];
	}
}

/* ---------------------------------------------------------------------------------------------------------------- SSE2 */

#if KVM_YUV_SSE2_COMPILED

/* 16 pixels per step. A pixel is the dword A<<24 | R<<16 | G<<8 | B: masking with 0x00FF00FF leaves the 16 bit words (B, R), shifting
   the words right by 8 leaves (G, A); pmaddwd then multiplies by the pairs (cB, cR) and (cG, 0) and adds the pair. */
static void y_row_sse2(const uint8_t *src, uint8_t *dst, int n16)
{
	const __m128i mask = _mm_set1_epi32(0x00FF00FF);
	const __m128i cbr = _mm_set1_epi32(PAIR(YB, YR));
	const __m128i cga = _mm_set1_epi32(PAIR(YG, 0));
	const __m128i off = _mm_set1_epi32(Y_OFF);
	int x;
	for (x = 0; x < n16; x += 16)
	{
		__m128i s[4];
		int k;
		for (k = 0; k < 4; k++)
		{
			const __m128i p = _mm_loadu_si128((const __m128i *)(src + ((size_t)x + 4 * (size_t)k) * 4));
			const __m128i a = _mm_madd_epi16(_mm_and_si128(p, mask), cbr);
			const __m128i b = _mm_madd_epi16(_mm_srli_epi16(p, 8), cga);
			s[k] = _mm_srli_epi32(_mm_add_epi32(_mm_add_epi32(a, b), off), 15);
		}
		_mm_storeu_si128((__m128i *)(dst + x), _mm_packus_epi16(_mm_packs_epi32(s[0], s[1]), _mm_packs_epi32(s[2], s[3])));
	}
}

/* Four chroma samples (8 pixels of two rows) for one of Cb / Cr: cbr = (cB, cR) pair, cga = cG. */
static __m128i chroma4(const uint8_t *r0, const uint8_t *r1, __m128i cbr, __m128i cga)
{
	const __m128i mask = _mm_set1_epi32(0x00FF00FF);
	const __m128i a0 = _mm_loadu_si128((const __m128i *)r0), a1 = _mm_loadu_si128((const __m128i *)(r0 + 16));
	const __m128i b0 = _mm_loadu_si128((const __m128i *)r1), b1 = _mm_loadu_si128((const __m128i *)(r1 + 16));
	/* sums over the two rows, per channel word (at most 510) */
	const __m128i br0 = _mm_add_epi16(_mm_and_si128(a0, mask), _mm_and_si128(b0, mask));
	const __m128i br1 = _mm_add_epi16(_mm_and_si128(a1, mask), _mm_and_si128(b1, mask));
	const __m128i ga0 = _mm_add_epi16(_mm_srli_epi16(a0, 8), _mm_srli_epi16(b0, 8));
	const __m128i ga1 = _mm_add_epi16(_mm_srli_epi16(a1, 8), _mm_srli_epi16(b1, 8));
	/* one dword per pixel (the vertical sum of its column): pixels 0..3 and 4..7 */
	const __m128i pa = _mm_add_epi32(_mm_madd_epi16(br0, cbr), _mm_madd_epi16(ga0, cga));
	const __m128i pb = _mm_add_epi32(_mm_madd_epi16(br1, cbr), _mm_madd_epi16(ga1, cga));
	/* neighbouring pixels (2i, 2i+1) are added: even and odd dwords of the two vectors */
	const __m128i ev = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(pa), _mm_castsi128_ps(pb), _MM_SHUFFLE(2, 0, 2, 0)));
	const __m128i od = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(pa), _mm_castsi128_ps(pb), _MM_SHUFFLE(3, 1, 3, 1)));
	return _mm_srli_epi32(_mm_add_epi32(_mm_add_epi32(ev, od), _mm_set1_epi32(C_OFF)), 17);
}

/* 16 pixels (8 chroma samples, 16 output bytes) per step. */
static void uv_row_sse2(const uint8_t *r0, const uint8_t *r1, uint8_t *dst, int n16)
{
	const __m128i ubr = _mm_set1_epi32(PAIR(UB, UR)), uga = _mm_set1_epi32(PAIR(UG, 0));
	const __m128i vbr = _mm_set1_epi32(PAIR(VB, VR)), vga = _mm_set1_epi32(PAIR(VG, 0));
	int x;
	for (x = 0; x < n16; x += 16)
	{
		const uint8_t *a = r0 + (size_t)x * 4, *b = r1 + (size_t)x * 4;
		const __m128i u0 = chroma4(a, b, ubr, uga), u1 = chroma4(a + 32, b + 32, ubr, uga);
		const __m128i v0 = chroma4(a, b, vbr, vga), v1 = chroma4(a + 32, b + 32, vbr, vga);
		const __m128i uw = _mm_packs_epi32(u0, u1), vw = _mm_packs_epi32(v0, v1); /* 8 words 16..240 */
		_mm_storeu_si128((__m128i *)(dst + x), _mm_or_si128(uw, _mm_slli_epi16(vw, 8)));
	}
}

#endif

/* ---------------------------------------------------------------------------------------------------------------- driver */

int kvm_yuv_bgra_to_nv12(const uint8_t *bgra, int width, int height, int stride, uint8_t *nv12, int codedW, int codedH, int impl)
{
	int useSse, n16, j;
	uint8_t *Y, *UV;
	if (bgra == NULL || nv12 == NULL || width < 1 || height < 1 || width > MAX_DIM || height > MAX_DIM) { return -1; }
	if (stride == INT_MIN || (stride < 0 ? -stride : stride) < width * 4 || codedW < width || codedH < height || (codedW & 1) || (codedH & 1) || codedW > MAX_DIM || codedH > MAX_DIM) { return -1; }
	if (impl == KVM_YUV_SSE2 && !KVM_YUV_SSE2_COMPILED) { return -2; }
	useSse = KVM_YUV_SSE2_COMPILED && impl != KVM_YUV_SCALAR;
	n16 = useSse ? (width & ~15) : 0;
	Y = nv12;
	UV = nv12 + (size_t)codedW * (size_t)codedH;

	for (j = 0; j < codedH / 2; j++)
	{
		const int ya = 2 * j, yb = 2 * j + 1;
		uint8_t *yd0 = Y + (size_t)ya * codedW, *yd1 = Y + (size_t)yb * codedW, *uvd = UV + (size_t)j * codedW;
		if (ya >= height)
		{
			/* below the source: repeat the last Y row and the last chroma row */
			memcpy(yd0, Y + (size_t)(height - 1) * codedW, (size_t)codedW);
			memcpy(yd1, yd0, (size_t)codedW);
			memcpy(uvd, uvd - codedW, (size_t)codedW);
			continue;
		}
		else
		{
			const uint8_t *r0 = bgra + (ptrdiff_t)ya * (ptrdiff_t)stride;
			const uint8_t *r1 = bgra + (ptrdiff_t)(yb < height ? yb : height - 1) * (ptrdiff_t)stride;
#if KVM_YUV_SSE2_COMPILED
			if (n16 > 0)
			{
				y_row_sse2(r0, yd0, n16);
				uv_row_sse2(r0, r1, uvd, n16);
			}
#endif
			y_row_scalar(r0, width, yd0, n16, codedW);
			uv_row_scalar(r0, r1, width, uvd, n16 / 2, codedW);
			if (yb < height)
			{
#if KVM_YUV_SSE2_COMPILED
				if (n16 > 0) { y_row_sse2(r1, yd1, n16); }
#endif
				y_row_scalar(r1, width, yd1, n16, codedW);
			}
			else
			{
				memcpy(yd1, yd0, (size_t)codedW); /* odd height: the row below the last one repeats it */
			}
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------------------------------------------- regions */

/* One pixel's Y, with the columns beyond the source repeating the last one. */
static uint8_t y_pix(const uint8_t *src, int width, int x)
{
	const uint8_t *p = src + (size_t)(x < width ? x : width - 1) * 4;
	return (uint8_t)((YB * p[0] + YG * p[1] + YR * p[2] + Y_OFF) >> 15);
}

/* The chroma sample i (pixels 2i and 2i + 1 of two rows; beyond the source the last sample). */
static void uv_pix(const uint8_t *r0, const uint8_t *r1, int width, int i, uint8_t *cb, uint8_t *cr)
{
	const int nreal = (width + 1) / 2;
	size_t xa, xb;
	int b, g, r;
	if (i >= nreal) { i = nreal - 1; }
	xa = (size_t)2 * (size_t)i * 4;
	xb = (size_t)(2 * i + 1 < width ? 2 * i + 1 : width - 1) * 4;
	b = r0[xa] + r0[xb] + r1[xa] + r1[xb];
	g = r0[xa + 1] + r0[xb + 1] + r1[xa + 1] + r1[xb + 1];
	r = r0[xa + 2] + r0[xb + 2] + r1[xa + 2] + r1[xb + 2];
	*cb = (uint8_t)((UB * b + UG * g + UR * r + C_OFF) >> 17);
	*cr = (uint8_t)((VB * b + VG * g + VR * r + C_OFF) >> 17);
}

int kvm_yuv_bgra_to_nv12_region(const uint8_t *bgra, int width, int height, int stride, uint8_t *nv12, int codedW, int codedH,
								int x0, int y0, int rw, int rh, int impl)
{
	int useSse, y, j, x, xs, xe;
	uint8_t *Y, *UV;
	if (bgra == NULL || nv12 == NULL || width < 1 || height < 1 || width > MAX_DIM || height > MAX_DIM) { return -1; }
	if (stride == INT_MIN || (stride < 0 ? -stride : stride) < width * 4 || codedW < width || codedH < height || (codedW & 1) || (codedH & 1) || codedW > MAX_DIM || codedH > MAX_DIM) { return -1; }
	if (x0 < 0 || y0 < 0 || rw < 0 || rh < 0 || (x0 & 1) || (y0 & 1) || (rw & 1) || (rh & 1) || x0 > codedW || y0 > codedH || rw > codedW - x0 || rh > codedH - y0) { return -1; }
	if (impl == KVM_YUV_SSE2 && !KVM_YUV_SSE2_COMPILED) { return -2; }
	useSse = KVM_YUV_SSE2_COMPILED && impl != KVM_YUV_SCALAR;
	if (rw == 0 || rh == 0) { return 0; }
	Y = nv12;
	UV = nv12 + (size_t)codedW * (size_t)codedH;
	xs = x0; xe = x0 + rw;

	/* luma rows: a row below the source is the last source row */
	for (y = y0; y < y0 + rh; y++)
	{
		const uint8_t *r0 = bgra + (ptrdiff_t)(y < height ? y : height - 1) * (ptrdiff_t)stride;
		uint8_t *d = Y + (size_t)y * codedW;
		const int lim = xe < width ? xe : width;
		x = xs;
#if KVM_YUV_SSE2_COMPILED
		if (useSse && lim - x >= 16)
		{
			const int n16 = (lim - x) & ~15;
			y_row_sse2(r0 + (size_t)x * 4, d + x, n16);
			x += n16;
		}
#endif
		for (; x < xe; x++) { d[x] = y_pix(r0, width, x); }
	}
	/* chroma rows: a row pair below the source is the pair of the last source row */
	for (j = y0 / 2; j < (y0 + rh) / 2; j++)
	{
		const int ra = (2 * j < height) ? 2 * j : 2 * ((height - 1) / 2);
		const int rb = (ra + 1 < height) ? ra + 1 : height - 1;
		const uint8_t *r0 = bgra + (ptrdiff_t)ra * (ptrdiff_t)stride, *r1 = bgra + (ptrdiff_t)rb * (ptrdiff_t)stride;
		uint8_t *d = UV + (size_t)j * codedW;
		const int lim = xe < width ? xe : width;
		int i;
		x = xs;
#if KVM_YUV_SSE2_COMPILED
		if (useSse && lim - x >= 16)
		{
			const int n16 = (lim - x) & ~15;
			uv_row_sse2(r0 + (size_t)x * 4, r1 + (size_t)x * 4, d + x, n16);
			x += n16;
		}
#endif
		for (i = x / 2; i < xe / 2; i++) { uint8_t cb, cr; uv_pix(r0, r1, width, i, &cb, &cr); d[2 * i] = cb; d[2 * i + 1] = cr; }
	}
	return 0;
}

int kvm_yuv_fill_flat(uint8_t *nv12, int codedW, int codedH, uint8_t y, uint8_t cb, uint8_t cr)
{
	size_t i, n;
	if (nv12 == NULL || codedW < 2 || codedH < 2 || (codedW & 1) || (codedH & 1) || codedW > MAX_DIM || codedH > MAX_DIM) { return -1; }
	n = (size_t)codedW * (size_t)codedH;
	memset(nv12, y, n);
	for (i = 0; i < n / 2; i += 2) { nv12[n + i] = cb; nv12[n + i + 1] = cr; }
	return 0;
}

void kvm_yuv_rgb_to_yuv(int b, int g, int r, uint8_t *y, uint8_t *cb, uint8_t *cr)
{
	*y = (uint8_t)((YB * b + YG * g + YR * r + Y_OFF) >> 15);
	*cb = (uint8_t)((UB * 4 * b + UG * 4 * g + UR * 4 * r + C_OFF) >> 17);
	*cr = (uint8_t)((VB * 4 * b + VG * 4 * g + VR * 4 * r + C_OFF) >> 17);
}

#define DOM_CELLS 1024
int kvm_yuv_dominant(const uint8_t *bgra, int width, int height, int stride, int *b, int *g, int *r)
{
	struct { int key; unsigned n, first; unsigned sb, sg, sr; } cell[DOM_CELLS];
	unsigned order = 0;
	int gx, gy, nx, ny, best = -1;
	unsigned bestN = 0;
	if (bgra == NULL || b == NULL || g == NULL || r == NULL || width < 1 || height < 1 || width > MAX_DIM || height > MAX_DIM) { return -1; }
	if (stride == INT_MIN || (stride < 0 ? -stride : stride) < width * 4) { return -1; }
	memset(cell, 0, sizeof(cell));
	for (gx = 0; gx < DOM_CELLS; gx++) { cell[gx].key = -1; }
	nx = width < 32 ? width : 32; ny = height < 32 ? height : 32;
	for (gy = 0; gy < ny; gy++)
	{
		const int sy = (int)(((long long)(2 * gy + 1) * height) / (2 * ny));
		const uint8_t *row = bgra + (ptrdiff_t)sy * (ptrdiff_t)stride;
		for (gx = 0; gx < nx; gx++)
		{
			const int sx = (int)(((long long)(2 * gx + 1) * width) / (2 * nx));
			const uint8_t *p = row + (size_t)sx * 4;
			const int key = ((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3);
			int h = (key * 2654435761u) >> 22, probes;
			h &= DOM_CELLS - 1;
			for (probes = 0; probes < DOM_CELLS && cell[h].key != -1 && cell[h].key != key; probes++) { h = (h + 1) & (DOM_CELLS - 1); }
			if (cell[h].key == -1) { cell[h].key = key; cell[h].first = order++; }
			cell[h].n++; cell[h].sb += p[0]; cell[h].sg += p[1]; cell[h].sr += p[2];
		}
	}
	for (gx = 0; gx < DOM_CELLS; gx++) { if (cell[gx].key != -1 && (cell[gx].n > bestN || (cell[gx].n == bestN && cell[gx].first < cell[best].first))) { bestN = cell[gx].n; best = gx; } }
	if (best < 0) { return -1; }
	*b = (int)((cell[best].sb + cell[best].n / 2) / cell[best].n);
	*g = (int)((cell[best].sg + cell[best].n / 2) / cell[best].n);
	*r = (int)((cell[best].sr + cell[best].n / 2) / cell[best].n);
	return 0;
}
