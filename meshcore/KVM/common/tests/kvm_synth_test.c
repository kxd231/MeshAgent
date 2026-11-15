/* Tests of kvm_synth.c: determinism, ranges, what changes between pictures. */
#include "../kvm_synth.h"
#include "../kvm_yuv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gFail = 0, gChecks = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static size_t count_diff(const uint8_t *a, const uint8_t *b, size_t n) { size_t i, d = 0; for (i = 0; i < n; i++) { d += a[i] != b[i]; } return d; }

int main(void)
{
	kvm_synth s, s2;
	uint8_t *a, *b, *c;
	size_t n, i, ysz;
	int kind, t, w = 1920, h = 1088, x, y;
	CHECK(kvm_synth_init(&s, 1920, 1088) == 0);
	CHECK(kvm_synth_init(&s2, 1920, 1088) == 0);
	n = kvm_synth_size(&s);
	CHECK(n == (size_t)1920 * 1088 * 3 / 2);
	ysz = (size_t)w * (size_t)h;
	a = (uint8_t *)malloc(n); b = (uint8_t *)malloc(n); c = (uint8_t *)malloc(n);
	/* determinism: two generators, the same bytes; a picture twice, the same bytes */
	for (kind = 0; kind < 3; kind++)
	{
		for (t = 0; t < 4; t++)
		{
			kvm_synth_frame(&s, kind, t, a); kvm_synth_frame(&s2, kind, t, b); kvm_synth_frame(&s, kind, t, c);
			CHECK(memcmp(a, b, n) == 0 && memcmp(a, c, n) == 0);
			for (i = 0; i < ysz; i++) { if (a[i] < 16 || a[i] > 235) { CHECK(0); break; } }
			for (i = ysz; i < n; i++) { if (a[i] < 16 || a[i] > 240) { CHECK(0); break; } }
		}
	}
	/* the page is dense (text) and not flat */
	{
		size_t dark = 0;
		kvm_synth_frame(&s, KVM_SYNTH_TEXT, 1, a);
		for (i = 0; i < ysz; i++) { dark += a[i] < 100; }
		CHECK(dark > ysz / 40 && dark < ysz / 2);
	}
	/* text: a still desktop, the pictures differ by a small area */
	kvm_synth_frame(&s, KVM_SYNTH_TEXT, 2, a); kvm_synth_frame(&s, KVM_SYNTH_TEXT, 3, b);
	{ const size_t d = count_diff(a, b, n); CHECK(d > 500 && d < 4000); }
	kvm_synth_frame(&s, KVM_SYNTH_TEXT, 2, b);
	CHECK(count_diff(a, b, n) == 0);
	/* scroll: picture t+1 is picture t moved up by 4 rows (and by 2 chroma rows) */
	kvm_synth_frame(&s, KVM_SYNTH_SCROLL, 5, a); kvm_synth_frame(&s, KVM_SYNTH_SCROLL, 6, b);
	for (y = 0; y < h - 4; y++) { if (memcmp(b + (size_t)y * (size_t)w, a + (size_t)(y + 4) * (size_t)w, (size_t)w) != 0) { CHECK(0); break; } }
	for (y = 0; y < h / 2 - 2; y++) { if (memcmp(b + ysz + (size_t)y * (size_t)w, a + ysz + (size_t)(y + 2) * (size_t)w, (size_t)w) != 0) { CHECK(0); break; } }
	CHECK(count_diff(a, b, ysz) > ysz / 20);
	/* scroll wraps after height / 4 pictures */
	kvm_synth_frame(&s, KVM_SYNTH_SCROLL, 0, a); kvm_synth_frame(&s, KVM_SYNTH_SCROLL, h / 4, b);
	CHECK(count_diff(a, b, n) == 0);
	/* video: the middle quarter changes, the corners do not */
	kvm_synth_frame(&s, KVM_SYNTH_VIDEO, 4, a); kvm_synth_frame(&s, KVM_SYNTH_VIDEO, 5, b);
	{
		size_t mid = 0, edge = 0;
		for (y = 0; y < h; y++) { for (x = 0; x < w; x++) { const int d = a[(size_t)y * (size_t)w + (size_t)x] != b[(size_t)y * (size_t)w + (size_t)x]; if (x >= w / 4 && x < w * 3 / 4 && y >= h / 4 && y < h * 3 / 4) { mid += (size_t)d; } else if (y > h * 7 / 8) { edge += (size_t)d; } } }
		CHECK(mid > (size_t)(w / 2) * (size_t)(h / 2) * 7 / 10 && edge == 0);
	}
	/* BGRA: the same picture; converted back with the agent's own conversion it is the NV12 picture (within the rounding of two conversions) */
	{
		uint8_t *bg = (uint8_t *)malloc((size_t)w * (size_t)h * 4u);
		size_t big = 0, tot = 0;
		kvm_synth_frame(&s, KVM_SYNTH_TEXT, 3, a);
		kvm_synth_frame_bgra(&s, KVM_SYNTH_TEXT, 3, bg, w * 4);
		CHECK(kvm_yuv_bgra_to_nv12(bg, w, h, w * 4, b, w, h, KVM_YUV_AUTO) == 0);
		for (i = 0; i < ysz; i++) { const int d = (int)a[i] - (int)b[i]; tot++; if (d > 3 || d < -3) { big++; } }
		printf("bgra round trip: %zu of %zu luma samples off by more than 3\n", big, tot); CHECK(big * 16 < tot);	/* luma: less than 6 % off by more than 3 (the coloured words: the chroma of 4:2:0 is not the chroma of the BGRA picture) */
		CHECK(bg[3] == 255 && bg[4 * (size_t)w * 100 + 3] == 255);
		free(bg);
	}
	/* arguments */
	{
		kvm_synth z;
		CHECK(kvm_synth_init(&z, 1000, 1088) == -1 && kvm_synth_init(&z, 32, 1088) == -1 && kvm_synth_init(&z, 1920, 9000) == -1 && kvm_synth_init(NULL, 64, 64) == -1);
		CHECK(kvm_synth_init(&z, 64, 64) == 0);
		kvm_synth_frame(&z, KVM_SYNTH_VIDEO, 3, a);	/* a small picture: no overrun (ASan) */
		kvm_synth_frame(&z, KVM_SYNTH_SCROLL, 100000, a);
		kvm_synth_free(&z); kvm_synth_free(&z); kvm_synth_free(NULL);
	}
	free(a); free(b); free(c);
	kvm_synth_free(&s); kvm_synth_free(&s2);
	printf("kvm_synth_test: %d checks, %d failed\n", gChecks, gFail);
	return gFail ? 1 : 0;
}
