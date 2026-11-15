/*
the cost per 1080p frame of what the smoothness work / the first key frame proof added on the capture and main loop threads (kvm_clear_alpha, the class of a tile, the series of motion, the write gap wait), next to the
conversion BGRA -> NV12 that runs inside the window of enc_ms (kvm_yuv_bgra_to_nv12, with the alpha byte 0x00, 0xFF and random: the conversion must not depend on it). Run by
run-kvm-frame-cost-bench.sh under the benchmark lock. The numbers are of the machine of the container, not of a Hyper-V guest: compare the ratios.
*/
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "kvm_motion.h"
#include "kvm_yuv.h"
#include "kvm_video_core.h"

#define W 1920
#define H 1080
#define CW 1920
#define CH 1088
#define RING 8   /* 8 x 8.3 MB = 66 MB: larger than any L3 of a desktop CPU, so every call starts from memory like a frame that was just written by the capture */

static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double med(double *v, int n) { qsort(v, (size_t)n, sizeof(double), cmpd); return v[n / 2]; }

static uint8_t *ring[RING];
static void fill(uint8_t *p, int alphaMode)
{
	size_t i, n = (size_t)W * H;
	uint32_t x = 12345u;
	for (i = 0; i < n; i++)
	{
		uint32_t px;
		x = x * 1664525u + 1013904223u;
		px = (x >> 8) & 0x00FFFFFFu;
		if (((i / 64) & 3u) == 0) { px = 0x00F0F0F0u; }	/* some flat areas like a desktop */
		if (alphaMode == 1) { px |= 0xFF000000u; } else if (alphaMode == 2) { px |= (x & 0xFFu) << 24; }
		memcpy(p + i * 4, &px, 4);
	}
}

#define REPS 7
static double bench_clear(int hot, int iters)
{
	double v[REPS]; int r, i;
	for (r = 0; r < REPS; r++)
	{
		double t0, t1;
		for (i = 0; i < RING; i++) { fill(ring[i], 1); }
		t0 = now_ms();
		for (i = 0; i < iters; i++) { kvm_clear_alpha(ring[hot ? 0 : (i % RING)], (size_t)W * H * 4); }
		t1 = now_ms();
		v[r] = (t1 - t0) / iters;
	}
	return med(v, REPS);
}

static double bench_yuv(int alphaMode, int impl, int iters)
{
	double v[REPS]; int r, i;
	static uint8_t nv[CW * CH * 3 / 2];
	for (r = 0; r < REPS; r++)
	{
		double t0, t1;
		for (i = 0; i < RING; i++) { fill(ring[i], alphaMode); }
		t0 = now_ms();
		for (i = 0; i < iters; i++)
		{
			const uint8_t *top = ring[i % RING] + (size_t)(H - 1) * W * 4;	/* the bottom-up bitmap as the agent passes it: top row last in memory, negative stride */
			if (kvm_yuv_bgra_to_nv12(top, W, H, -W * 4, nv, CW, CH, impl) != 0) { fprintf(stderr, "yuv failed\n"); exit(1); }
		}
		t1 = now_ms();
		v[r] = (t1 - t0) / iters;
	}
	return med(v, REPS);
}

static volatile unsigned sink;
/* one frame of the class decisions with 872 changed tiles (the video of the Windows 11 VM) in a 60 x 34 grid: per changed tile `calls` class calls (the start tile, the right neighbour, the rows below: at most 3) */
static double bench_motion(int calls, int changed, int iters)
{
	double v[REPS]; int r, i;
	for (r = 0; r < REPS; r++)
	{
		kvm_motion m; double t0, t1; uint32_t now = 1000u; unsigned acc = 0; unsigned streak[34 * 60]; int k, c;
		kvm_motion_init(&m); (void)kvm_motion_resize(&m, 34, 60); memset(streak, 0, sizeof(streak));
		t0 = now_ms();
		for (i = 0; i < iters; i++)
		{
			now += 33u;
			kvm_motion_frame(&m, now, (unsigned)changed);
			for (k = 0; k < changed; k++)
			{
				streak[k] = kvm_motion_note_change(&m, streak[k], 33, 0);
				for (c = 0; c < calls; c++) { acc += (unsigned)kvm_motion_class(&m, now, k / 60, k % 60, streak[k], 33); }
			}
			if ((i & 63) == 0) { kvm_motion_clear_stats(&m); }
		}
		t1 = now_ms(); sink += acc; kvm_motion_free(&m);
		v[r] = (t1 - t0) / iters;
	}
	return med(v, REPS);
}
/* a worst case for the near rule: the neighbourhood is empty (no tile classified as motion lately), every call scans 25 stamps and the memory of the place */
static double bench_motion_cold(int calls, int changed, int iters)
{
	double v[REPS]; int r, i;
	for (r = 0; r < REPS; r++)
	{
		kvm_motion m; double t0, t1; uint32_t now = 1000u; unsigned acc = 0; int k, c;
		kvm_motion_init(&m); (void)kvm_motion_resize(&m, 34, 60);
		t0 = now_ms();
		for (i = 0; i < iters; i++)
		{
			now += 33u;
			for (k = 0; k < changed; k++) { for (c = 0; c < calls; c++) { acc += (unsigned)kvm_motion_class(&m, now, k / 60, k % 60, 1, 33); } }
		}
		t1 = now_ms(); sink += acc; kvm_motion_free(&m);
		v[r] = (t1 - t0) / iters;
	}
	return med(v, REPS);
}

static double bench_series_pace(int iters)
{
	double t0, t1; int i; kvm_series s; kvm_vctl c; unsigned acc = 0; uint32_t now = 1000u;
	memset(&c, 0, sizeof(c)); kvm_series_init(&s);
	t0 = now_ms();
	for (i = 0; i < iters; i++)
	{
		now += 16u;
		kvm_series_motion(&s, now, 4000u); kvm_series_write(&s, now, 1, 0, 16u);
		kvm_vctl_note_write(&c, now, 10u); acc += kvm_vctl_write_wait_ms(&c, now + 5u, 16u);
	}
	t1 = now_ms(); sink += acc;
	return (t1 - t0) / iters * 1000.0;	/* microseconds */
}

int main(int argc, char **argv)
{
	int i;
	const char *label = argc > 1 ? argv[1] : "";
	for (i = 0; i < RING; i++) { ring[i] = (uint8_t *)aligned_alloc(64, (size_t)W * H * 4); if (ring[i] == NULL) { return 1; } }
	printf("== %s\n", label);
	printf("kvm_clear_alpha, 1920x1080 BGRA (8.3 MB), buffer from memory:  %.3f ms\n", bench_clear(0, 40));
	printf("kvm_clear_alpha, 1920x1080 BGRA, buffer hot in cache (1 buffer): %.3f ms\n", bench_clear(1, 40));
	printf("kvm_yuv_bgra_to_nv12 SSE2, alpha 0x00:   %.3f ms\n", bench_yuv(0, KVM_YUV_SSE2, 40));
	printf("kvm_yuv_bgra_to_nv12 SSE2, alpha 0xFF:   %.3f ms\n", bench_yuv(1, KVM_YUV_SSE2, 40));
	printf("kvm_yuv_bgra_to_nv12 SSE2, alpha random: %.3f ms\n", bench_yuv(2, KVM_YUV_SSE2, 40));
	printf("kvm_yuv_bgra_to_nv12 scalar, alpha 0x00: %.3f ms\n", bench_yuv(0, KVM_YUV_SCALAR, 8));
	printf("kvm_yuv_bgra_to_nv12 scalar, alpha 0xFF: %.3f ms\n", bench_yuv(1, KVM_YUV_SCALAR, 8));
	printf("motion class, 872 changed tiles, 1 call each, neighbourhood busy: %.4f ms\n", bench_motion(1, 872, 2000));
	printf("motion class, 872 changed tiles, 3 calls each, neighbourhood busy: %.4f ms\n", bench_motion(3, 872, 2000));
	printf("motion class, 2040 changed tiles, 3 calls each, cold (every call scans 25 stamps): %.4f ms\n", bench_motion_cold(3, 2040, 2000));
	printf("series_motion + series_write + note_write + write_wait_ms, per frame: %.4f us\n", bench_series_pace(2000000));
	return 0;
}
