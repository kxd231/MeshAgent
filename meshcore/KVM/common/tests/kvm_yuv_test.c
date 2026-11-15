/* Tests of kvm_yuv.c: SSE2 against scalar (bit exact), scalar against a floating point reference (+-1), padding by edge replication,
   odd sizes, strides, unaligned input, no read or write outside the buffers (run it with the sanitizers: make asan), and a timing line. */
#define _POSIX_C_SOURCE 200809L

#include "../kvm_yuv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
	g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
	return (uint32_t)(g_rng >> 16);
}

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; if (g_fail < 30) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

/* An image kind: 0 noise, 1 smooth gradients with a little noise, 2 each channel 0 or 255, 3 grey ramp, 4 all 0, 5 all 255, 6 text-like (two colours) */
static void fill(uint8_t *p, int w, int h, int stride, int kind)
{
	int x, y;
	for (y = 0; y < h; y++)
	{
		uint8_t *row = p + (size_t)y * stride;
		for (x = 0; x < w; x++)
		{
			uint8_t *q = row + (size_t)x * 4;
			switch (kind)
			{
				case 0: q[0] = (uint8_t)rnd(); q[1] = (uint8_t)rnd(); q[2] = (uint8_t)rnd(); q[3] = (uint8_t)rnd(); break;
				case 1: q[0] = (uint8_t)(x * 3 + y + (rnd() & 7)); q[1] = (uint8_t)(y * 2 + (rnd() & 7)); q[2] = (uint8_t)(255 - x - (rnd() & 7)); q[3] = 255; break;
				case 2: q[0] = (rnd() & 1) ? 255 : 0; q[1] = (rnd() & 1) ? 255 : 0; q[2] = (rnd() & 1) ? 255 : 0; q[3] = (uint8_t)rnd(); break;
				case 3: q[0] = q[1] = q[2] = (uint8_t)((x * 255) / (w > 1 ? w - 1 : 1)); q[3] = 255; break;
				case 4: q[0] = q[1] = q[2] = 0; q[3] = 0; break;
				case 5: q[0] = q[1] = q[2] = 255; q[3] = 255; break;
				default: if (((x / 3) + (y / 5)) % 4 == 0) { q[0] = 20; q[1] = 20; q[2] = 200; } else { q[0] = q[1] = q[2] = 250; } q[3] = 255; break;
			}
		}
		/* the bytes between the rows are not pixels: garbage must not matter */
		for (x = w * 4; x < stride; x++) { row[x] = (uint8_t)rnd(); }
	}
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Floating point reference on the edge-extended image. out[3] = Y, Cb, Cr (not rounded). */
static void ref_yuv(const uint8_t *p, int w, int h, int stride, int cx, int cy, double *Yp, double *Cbp, double *Crp)
{
	const double kr = 0.2126, kb = 0.0722, kg = 1.0 - kr - kb;
	int dx, dy;
	double r = 0, g = 0, b = 0;
	(void)cy;
	for (dy = 0; dy < 2; dy++)
	{
		for (dx = 0; dx < 2; dx++)
		{
			const uint8_t *q = p + (size_t)clampi(cy + dy, 0, h - 1) * stride + (size_t)clampi(cx + dx, 0, w - 1) * 4;
			b += q[0]; g += q[1]; r += q[2];
		}
	}
	r /= 4; g /= 4; b /= 4;
	{
		const double yf = kr * r + kg * g + kb * b;
		*Cbp = 128.0 + 224.0 / 255.0 * 0.5 * (b - yf) / (1.0 - kb);
		*Crp = 128.0 + 224.0 / 255.0 * 0.5 * (r - yf) / (1.0 - kr);
	}
	(void)Yp;
}

static double ref_y(const uint8_t *p, int w, int h, int stride, int x, int y)
{
	const uint8_t *q = p + (size_t)clampi(y, 0, h - 1) * stride + (size_t)clampi(x, 0, w - 1) * 4;
	return 16.0 + 219.0 / 255.0 * (0.2126 * q[2] + 0.7152 * q[1] + 0.0722 * q[0]);
}

static double g_maxErr = 0; /* largest |out - float| seen */

static void check_against_reference(const uint8_t *src, int w, int h, int stride, const uint8_t *nv, int cw, int ch, const char *what)
{
	const uint8_t *Y = nv, *UV = nv + (size_t)cw * ch;
	int x, y, bad = 0;
	for (y = 0; y < ch && !bad; y++)
	{
		for (x = 0; x < cw; x++)
		{
			const double e = ref_y(src, w, h, stride, x, y);
			const double d = fabs((double)Y[(size_t)y * cw + x] - e);
			if (d > g_maxErr) { g_maxErr = d; }
			if (fabs((double)Y[(size_t)y * cw + x] - floor(e + 0.5)) > 1.0) { bad = 1; printf("FAIL %s Y(%d,%d) = %d, reference %.3f (size %dx%d)\n", what, x, y, Y[(size_t)y * cw + x], e, w, h); break; }
		}
	}
	for (y = 0; y < ch / 2 && !bad; y++)
	{
		for (x = 0; x < cw / 2; x++)
		{
			double cb, cr, dummy = 0;
			const uint8_t u = UV[(size_t)y * cw + 2 * x], v = UV[(size_t)y * cw + 2 * x + 1];
			const int nrC = (w + 1) / 2, nrR = (h + 1) / 2; /* the padding repeats the last real chroma sample */
			ref_yuv(src, w, h, stride, 2 * (x < nrC ? x : nrC - 1), 2 * (y < nrR ? y : nrR - 1), &dummy, &cb, &cr);
			if (fabs(u - cb) > g_maxErr) { g_maxErr = fabs(u - cb); }
			if (fabs(v - cr) > g_maxErr) { g_maxErr = fabs(v - cr); }
			if (fabs((double)u - floor(cb + 0.5)) > 1.0 || fabs((double)v - floor(cr + 0.5)) > 1.0)
			{
				bad = 1;
				printf("FAIL %s UV(%d,%d) = %d,%d reference %.3f,%.3f (size %dx%d)\n", what, x, y, u, v, cb, cr, w, h);
				break;
			}
		}
	}
	g_checks++;
	if (bad) { g_fail++; }
}

/* Padding is a pure copy of the last real column / row (no arithmetic): exact. */
static void check_padding(const uint8_t *nv, int w, int h, int cw, int ch, const char *what)
{
	const uint8_t *Y = nv, *UV = nv + (size_t)cw * ch;
	const int nrC = (w + 1) / 2, nrR = (h + 1) / 2;
	int x, y;
	for (y = 0; y < ch; y++)
	{
		for (x = w; x < cw; x++) { CHECK(Y[(size_t)y * cw + x] == Y[(size_t)y * cw + w - 1], "%s: Y pad col (%d,%d)", what, x, y); }
	}
	for (y = h; y < ch; y++) { CHECK(memcmp(Y + (size_t)y * cw, Y + (size_t)(h - 1) * cw, (size_t)cw) == 0, "%s: Y pad row %d", what, y); }
	for (y = 0; y < ch / 2; y++)
	{
		for (x = nrC; x < cw / 2; x++)
		{
			CHECK(UV[(size_t)y * cw + 2 * x] == UV[(size_t)y * cw + 2 * (nrC - 1)] && UV[(size_t)y * cw + 2 * x + 1] == UV[(size_t)y * cw + 2 * (nrC - 1) + 1], "%s: UV pad col (%d,%d)", what, x, y);
		}
	}
	for (y = nrR; y < ch / 2; y++) { CHECK(memcmp(UV + (size_t)y * cw, UV + (size_t)(nrR - 1) * cw, (size_t)cw) == 0, "%s: UV pad row %d", what, y); }
}

/* One image: convert with both implementations in exactly sized buffers (a sanitizer sees any access outside). */
static void run_case(int w, int h, int extraStride, int kind, int unaligned)
{
	const int stride = w * 4 + extraStride;
	const int cw = kvm_yuv_coded(w), ch = kvm_yuv_coded(h);
	const size_t srcLen = (size_t)(h - 1) * stride + (size_t)w * 4;
	const size_t outLen = kvm_yuv_nv12_size(cw, ch);
	uint8_t *srcBuf = (uint8_t *)malloc(srcLen + 1), *src;
	uint8_t *a = (uint8_t *)malloc(outLen), *b = (uint8_t *)malloc(outLen);
	char what[96];
	int r;
	snprintf(what, sizeof(what), "%dx%d stride+%d kind %d", w, h, extraStride, kind);
	src = srcBuf + (unaligned ? 1 : 0);
	/* fill() writes the bytes between the rows too, but the last row has none: the buffer ends at the last pixel */
	{
		const size_t fullLen = (size_t)h * stride;
		uint8_t *tmp = (uint8_t *)malloc(fullLen);
		fill(tmp, w, h, stride, kind);
		memcpy(src, tmp, srcLen);
		free(tmp);
	}
	memset(a, 0xA5, outLen);
	memset(b, 0x5A, outLen);
	r = kvm_yuv_bgra_to_nv12(src, w, h, stride, a, cw, ch, KVM_YUV_SCALAR);
	CHECK(r == 0, "%s: scalar returned %d", what, r);
	check_against_reference(src, w, h, stride, a, cw, ch, what);
	check_padding(a, w, h, cw, ch, what);
	if (kvm_yuv_have_sse2())
	{
		r = kvm_yuv_bgra_to_nv12(src, w, h, stride, b, cw, ch, KVM_YUV_SSE2);
		CHECK(r == 0, "%s: sse2 returned %d", what, r);
		CHECK(memcmp(a, b, outLen) == 0, "%s: SSE2 differs from scalar", what);
		memset(b, 0x5A, outLen);
		r = kvm_yuv_bgra_to_nv12(src, w, h, stride, b, cw, ch, KVM_YUV_AUTO);
		CHECK(r == 0 && memcmp(a, b, outLen) == 0, "%s: AUTO differs from scalar", what);
	}
	/* the same picture as a bottom-up bitmap (negative stride, pointer to the top row): identical result, exact-size buffer for the sanitizers */
	{
		const int pitch = stride;
		uint8_t *bu = (uint8_t *)malloc(srcLen), *top;
		int y;
		for (y = 0; y < h; y++) { memcpy(bu + (size_t)(h - 1 - y) * pitch, src + (size_t)y * pitch, (size_t)w * 4); }
		top = bu + (size_t)(h - 1) * pitch;
		memset(b, 0x5A, outLen);
		r = kvm_yuv_bgra_to_nv12(top, w, h, -pitch, b, cw, ch, KVM_YUV_SCALAR);
		CHECK(r == 0 && memcmp(a, b, outLen) == 0, "%s: bottom-up scalar differs from top-down", what);
		if (kvm_yuv_have_sse2())
		{
			memset(b, 0x5A, outLen);
			r = kvm_yuv_bgra_to_nv12(top, w, h, -pitch, b, cw, ch, KVM_YUV_SSE2);
			CHECK(r == 0 && memcmp(a, b, outLen) == 0, "%s: bottom-up SSE2 differs from top-down", what);
		}
		check_against_reference(src, w, h, stride, b, cw, ch, what); /* b is the bottom-up result (equal to a, checked above) */
		free(bu);
	}
	free(a); free(b); free(srcBuf);
}

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}


/* ---- regions, flat fill, the colour of a flat picture, the dominant colour ---- */
static void test_region_case(int w, int h, int kind, int impl)
{
	const int cw = ((w + 31) / 32) * 32, ch = ((h + 31) / 32) * 32;
	const size_t n = (size_t)cw * ch * 3 / 2;
	uint8_t *src = (uint8_t *)malloc((size_t)w * h * 4 + 64), *full = (uint8_t *)malloc(n), *got = (uint8_t *)malloc(n), *orig = (uint8_t *)malloc(n);
	int trial;
	size_t i;
	fill(src, w, h, w * 4, kind);
	CHECK(kvm_yuv_bgra_to_nv12(src, w, h, w * 4, full, cw, ch, KVM_YUV_SCALAR) == 0, "full\n");
	for (trial = 0; trial < 6; trial++)
	{
		int x0 = 2 * (int)(rnd() % (cw / 2)), y0 = 2 * (int)(rnd() % (ch / 2)), rw, rh, x, y;
		rw = 2 * (int)(rnd() % ((cw - x0) / 2 + 1)); rh = 2 * (int)(rnd() % ((ch - y0) / 2 + 1));
		if (trial == 0) { x0 = 0; y0 = 0; rw = cw; rh = ch; }
		for (i = 0; i < n; i++) { orig[i] = (uint8_t)rnd(); }
		memcpy(got, orig, n);
		CHECK(kvm_yuv_bgra_to_nv12_region(src, w, h, w * 4, got, cw, ch, x0, y0, rw, rh, impl) == 0, "region call\n");
		for (y = 0; y < ch; y++)
		{
			for (x = 0; x < cw; x++)
			{
				const int in = (x >= x0 && x < x0 + rw && y >= y0 && y < y0 + rh);
				const size_t k = (size_t)y * cw + x;
				if (got[k] != (in ? full[k] : orig[k])) { CHECK(0, "Y %dx%d kind %d impl %d at (%d,%d) region (%d,%d,%d,%d)\n", w, h, kind, impl, x, y, x0, y0, rw, rh); y = ch; break; }
			}
		}
		for (y = 0; y < ch / 2; y++)
		{
			for (x = 0; x < cw; x++)
			{
				const int in = (x >= x0 && x < x0 + rw && 2 * y >= y0 && 2 * y < y0 + rh);
				const size_t k = (size_t)cw * ch + (size_t)y * cw + x;
				if (got[k] != (in ? full[k] : orig[k])) { CHECK(0, "UV %dx%d kind %d impl %d at (%d,%d) region (%d,%d,%d,%d)\n", w, h, kind, impl, x, y, x0, y0, rw, rh); y = ch; break; }
			}
		}
	}
	free(src); free(full); free(got); free(orig);
}

static void test_h_idr1(void)
{
	static const int sizes[][2] = {{1, 1}, {2, 3}, {3, 2}, {17, 5}, {31, 31}, {33, 17}, {64, 64}, {100, 37}, {1920, 1200}};
	int i, kind, impl, k;
	uint8_t nv[64 * 64 * 3 / 2], px[4 * 64];
	uint8_t y, cb, cr, y2, cb2, cr2;
	int b, g, r;
	for (i = 0; i < (int)(sizeof(sizes) / sizeof(sizes[0])); i++)
	{
		for (kind = 0; kind < 7; kind += (sizes[i][0] > 1000 ? 3 : 1)) { for (impl = KVM_YUV_SCALAR; impl <= KVM_YUV_SSE2; impl++) { if (impl == KVM_YUV_SSE2 && !kvm_yuv_have_sse2()) { continue; } test_region_case(sizes[i][0], sizes[i][1], kind, impl); } }
	}
	/* bad arguments */
	{
		uint8_t src[16 * 16 * 4], out[32 * 32 * 3 / 2];
		memset(src, 0, sizeof(src));
		CHECK(kvm_yuv_bgra_to_nv12_region(src, 16, 16, 64, out, 32, 32, 1, 0, 2, 2, KVM_YUV_AUTO) == -1, "odd x0\n");
		CHECK(kvm_yuv_bgra_to_nv12_region(src, 16, 16, 64, out, 32, 32, 0, 0, 34, 2, KVM_YUV_AUTO) == -1, "too wide\n");
		CHECK(kvm_yuv_bgra_to_nv12_region(src, 16, 16, 64, out, 32, 32, 30, 30, 4, 2, KVM_YUV_AUTO) == -1, "past the end\n");
		CHECK(kvm_yuv_bgra_to_nv12_region(src, 16, 16, 64, out, 32, 32, 0, 30, 2, 4, KVM_YUV_AUTO) == -1, "below the end\n");
		CHECK(kvm_yuv_bgra_to_nv12_region(src, 16, 16, 64, out, 32, 32, 0, 0, 0, 0, KVM_YUV_AUTO) == 0, "empty\n");
		CHECK(kvm_yuv_bgra_to_nv12_region(NULL, 16, 16, 64, out, 32, 32, 0, 0, 2, 2, KVM_YUV_AUTO) == -1, "null\n");
	}
	/* flat fill */
	CHECK(kvm_yuv_fill_flat(nv, 64, 64, 10, 20, 30) == 0, "fill\n");
	for (k = 0; k < 64 * 64; k++) { if (nv[k] != 10) { CHECK(0, "flat Y\n"); break; } }
	for (k = 0; k < 64 * 32; k += 2) { if (nv[64 * 64 + k] != 20 || nv[64 * 64 + k + 1] != 30) { CHECK(0, "flat UV\n"); break; } }
	CHECK(kvm_yuv_fill_flat(nv, 63, 64, 1, 2, 3) == -1 && kvm_yuv_fill_flat(NULL, 64, 64, 1, 2, 3) == -1, "fill args\n");
	/* the colour of a flat picture is the one that the conversion gives */
	for (k = 0; k < 40; k++)
	{
		int j;
		b = (int)(rnd() & 255); g = (int)(rnd() & 255); r = (int)(rnd() & 255);
		if (k == 0) { b = g = r = 0; } else if (k == 1) { b = g = r = 255; }
		for (j = 0; j < 4 * 4 * 4; j += 4) { px[j] = (uint8_t)b; px[j + 1] = (uint8_t)g; px[j + 2] = (uint8_t)r; px[j + 3] = 7; }
		CHECK(kvm_yuv_bgra_to_nv12(px, 4, 4, 16, nv, 32, 32, KVM_YUV_SCALAR) == 0, "conv\n");
		kvm_yuv_rgb_to_yuv(b, g, r, &y, &cb, &cr);
		y2 = nv[0]; cb2 = nv[32 * 32]; cr2 = nv[32 * 32 + 1];
		CHECK(y == y2 && cb == cb2 && cr == cr2, "rgb_to_yuv %d %d %d: %d %d %d against %d %d %d\n", b, g, r, y, cb, cr, y2, cb2, cr2);
	}
	/* the dominant colour */
	{
		uint8_t img[100 * 100 * 4];
		int x, yy;
		for (yy = 0; yy < 100; yy++) { for (x = 0; x < 100; x++) { uint8_t *q = img + ((size_t)yy * 100 + x) * 4; q[0] = 250; q[1] = 250; q[2] = 250; q[3] = 255; if (x < 30) { q[0] = (uint8_t)(x * 8); q[1] = (uint8_t)(yy * 2); q[2] = 40; } } }
		CHECK(kvm_yuv_dominant(img, 100, 100, 400, &b, &g, &r) == 0 && b >= 248 && g >= 248 && r >= 248, "dominant white: %d %d %d\n", b, g, r);
		/* negative stride: the same picture bottom-up */
		CHECK(kvm_yuv_dominant(img + (size_t)99 * 400, 100, 100, -400, &b, &g, &r) == 0 && b >= 248, "dominant bottom-up\n");
		/* a tie: the first sample (top left) wins */
		for (yy = 0; yy < 100; yy++) { for (x = 0; x < 100; x++) { uint8_t *q = img + ((size_t)yy * 100 + x) * 4; q[0] = q[1] = q[2] = (x < 50) ? 10 : 200; q[3] = 255; } }
		CHECK(kvm_yuv_dominant(img, 100, 100, 400, &b, &g, &r) == 0 && b == 10, "dominant tie: %d\n", b);
		/* cells that differ only in the red channel are different cells */
		for (yy = 0; yy < 100; yy++) { for (x = 0; x < 100; x++) { uint8_t *q = img + ((size_t)yy * 100 + x) * 4; q[0] = 100; q[1] = 100; q[2] = (x < 60) ? 200 : 8; q[3] = 255; } }
		CHECK(kvm_yuv_dominant(img, 100, 100, 400, &b, &g, &r) == 0 && r == 200, "dominant red: %d\n", r);
		/* 1 x 1 and uniform */
		img[0] = 1; img[1] = 2; img[2] = 3; img[3] = 0;
		CHECK(kvm_yuv_dominant(img, 1, 1, 4, &b, &g, &r) == 0 && b == 1 && g == 2 && r == 3, "dominant 1x1\n");
		CHECK(kvm_yuv_dominant(NULL, 1, 1, 4, &b, &g, &r) == -1 && kvm_yuv_dominant(img, 0, 1, 4, &b, &g, &r) == -1 && kvm_yuv_dominant(img, 1, 1, 3, &b, &g, &r) == -1, "dominant args\n");
	}
}

int main(void)
{
	static const int sizes[][2] = {{1, 1}, {1, 2}, {2, 1}, {2, 2}, {3, 3}, {5, 7}, {15, 15}, {16, 16}, {17, 17}, {16, 33}, {31, 5}, {32, 32}, {33, 34}, {47, 2}, {63, 64}, {64, 63}, {100, 75}, {127, 129}, {255, 3}, {1919, 33}, {1920, 34}};
	int i, kind, n = (int)(sizeof(sizes) / sizeof(sizes[0]));
	printf("kvm_yuv_test: sse2 compiled in: %d\n", kvm_yuv_have_sse2());

	/* sizes, strides, kinds, aligned and unaligned input */
	for (i = 0; i < n; i++)
	{
		for (kind = 0; kind < 7; kind++)
		{
			run_case(sizes[i][0], sizes[i][1], 0, kind, 0);
			run_case(sizes[i][0], sizes[i][1], (int)(rnd() % 41), kind, (int)(rnd() & 1));
		}
	}
	/* random sizes */
	for (i = 0; i < 150; i++) { run_case(1 + (int)(rnd() % 200), 1 + (int)(rnd() % 120), (int)(rnd() % 3) * (int)(rnd() % 50), (int)(rnd() % 7), (int)(rnd() & 1)); }
	/* a full size frame */
	run_case(1920, 1080, 0, 1, 0);
	run_case(1920, 1080, 64, 0, 1);

	test_h_idr1();

	/* known values */
	{
		uint8_t px[4 * 4], out[32 * 32 * 3 / 2];
		int j;
		/* black, white, mid grey: exact */
		for (j = 0; j < 16; j++) { px[j] = 0; }
		CHECK(kvm_yuv_bgra_to_nv12(px, 2, 2, 8, out, 32, 32, KVM_YUV_SCALAR) == 0 && out[0] == 16 && out[32 * 32] == 128 && out[32 * 32 + 1] == 128, "black");
		for (j = 0; j < 16; j++) { px[j] = 255; }
		CHECK(kvm_yuv_bgra_to_nv12(px, 2, 2, 8, out, 32, 32, KVM_YUV_SCALAR) == 0 && out[0] == 235 && out[32 * 32] == 128 && out[32 * 32 + 1] == 128, "white: Y=%d U=%d V=%d", out[0], out[32 * 32], out[32 * 32 + 1]);
		for (j = 0; j < 16; j++) { px[j] = 128; }
		CHECK(kvm_yuv_bgra_to_nv12(px, 2, 2, 8, out, 32, 32, KVM_YUV_SCALAR) == 0 && out[32 * 32] == 128 && out[32 * 32 + 1] == 128, "grey chroma");
		/* all 256 greys: chroma is always 128 */
		for (j = 0; j < 256; j++)
		{
			px[0] = px[1] = px[2] = px[4] = px[5] = px[6] = px[8] = px[9] = px[10] = px[12] = px[13] = px[14] = (uint8_t)j;
			kvm_yuv_bgra_to_nv12(px, 2, 2, 8, out, 32, 32, KVM_YUV_SCALAR);
			CHECK(out[32 * 32] == 128 && out[32 * 32 + 1] == 128, "grey %d chroma %d,%d", j, out[32 * 32], out[32 * 32 + 1]);
		}
		/* full red (BGRA = 0,0,255): BT.709 limited Y=63 Cb=102 Cr=240 */
		for (j = 0; j < 4; j++) { px[4 * j] = 0; px[4 * j + 1] = 0; px[4 * j + 2] = 255; px[4 * j + 3] = 255; }
		kvm_yuv_bgra_to_nv12(px, 2, 2, 8, out, 32, 32, KVM_YUV_SCALAR);
		CHECK(abs(out[0] - 63) <= 1 && abs(out[32 * 32] - 102) <= 1 && abs(out[32 * 32 + 1] - 240) <= 1, "red: %d %d %d", out[0], out[32 * 32], out[32 * 32 + 1]);
		/* argument checks */
		CHECK(kvm_yuv_bgra_to_nv12(NULL, 2, 2, 8, out, 32, 32, 0) == -1, "null src");
		CHECK(kvm_yuv_bgra_to_nv12(px, 0, 2, 8, out, 32, 32, 0) == -1, "width 0");
		CHECK(kvm_yuv_bgra_to_nv12(px, 2, 2, 7, out, 32, 32, 0) == -1, "stride too small");
		CHECK(kvm_yuv_bgra_to_nv12(px, 2, 2, -7, out, 32, 32, 0) == -1, "negative stride too small");
		CHECK(kvm_yuv_bgra_to_nv12(px, 2, 2, (-2147483647 - 1), out, 32, 32, 0) == -1, "INT_MIN stride");
		CHECK(kvm_yuv_bgra_to_nv12(px, 2, 2, 8, out, 31, 32, 0) == -1, "odd coded width");
		CHECK(kvm_yuv_bgra_to_nv12(px, 33, 2, 132, out, 32, 32, 0) == -1, "coded smaller than source");
		CHECK(kvm_yuv_coded(1) == 32 && kvm_yuv_coded(32) == 32 && kvm_yuv_coded(33) == 64 && kvm_yuv_coded(1080) == 1088 && kvm_yuv_coded(2160) == 2176, "kvm_yuv_coded");
		CHECK(kvm_yuv_nv12_size(1920, 1088) == 1920u * 1088u * 3u / 2u, "nv12 size");
	}

	printf("checks: %d, failures: %d, largest |out - float reference| = %.3f\n", g_checks, g_fail, g_maxErr);

	/* timing, 1920x1080 -> 1920x1088, informative only */
	{
		const int w = 1920, h = 1080, cw = 1920, ch = 1088, stride = w * 4;
		uint8_t *src = (uint8_t *)malloc((size_t)stride * h), *dst = (uint8_t *)malloc(kvm_yuv_nv12_size(cw, ch));
		int it, impl;
		fill(src, w, h, stride, 1);
		for (impl = KVM_YUV_SCALAR; impl <= KVM_YUV_SSE2; impl++)
		{
			double t0, best = 1e9, sum = 0;
			const int iters = 30;
			if (impl == KVM_YUV_SSE2 && !kvm_yuv_have_sse2()) { break; }
			kvm_yuv_bgra_to_nv12(src, w, h, stride, dst, cw, ch, impl);
			for (it = 0; it < iters; it++)
			{
				t0 = now_ms();
				kvm_yuv_bgra_to_nv12(src, w, h, stride, dst, cw, ch, impl);
				t0 = now_ms() - t0;
				sum += t0;
				if (t0 < best) { best = t0; }
			}
			printf("timing 1920x1080: %-6s mean %.2f ms/frame, best %.2f ms/frame (%d frames)\n", impl == KVM_YUV_SCALAR ? "scalar" : "sse2", sum / iters, best, iters);
		}
		free(src); free(dst);
	}

	if (g_fail != 0) { printf("RESULT: FAILED\n"); return 1; }
	printf("RESULT: OK\n");
	return 0;
}
