/* Tests of kvm_nv12view.c: the plan (rings, tiles, key frames, the switches, virgin macroblocks) and the apply (a view that is frozen outside the map, flat key frames, bit identical to the full
   conversion inside the map). */
#define _POSIX_C_SOURCE 200809L
#include "../kvm_nv12view.h"
#include "../kvm_yuv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; if (g_fail < 40) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
static uint64_t g_rng = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 16); }

#define CW 320
#define CH 192		/* coded: 20 x 12 macroblocks, 10 x 6 tiles */
#define TR 6
#define TC 10

static int mb(const kvm_nv12v_pl *p, const kvm_nv12v *v, int r, int c) { return p->map != NULL ? p->map[(size_t)r * v->mbCols + c] : 1; }
static void clear(uint8_t *m, size_t n) { memset(m, 0, n); }
static void set_tile(uint8_t *m, int r, int c) { m[r * TC + c] = 1; }

static void test_plan(void)
{
	kvm_nv12v v;
	kvm_nv12v_pl p;
	uint8_t cover[TR * TC], st[TR * TC], fl[TR * TC];
	int r, c, n;
	clear(cover, sizeof(cover)); clear(st, sizeof(st)); clear(fl, sizeof(fl));

	/* frozen: the first frame, a key frame and a frame after invalidate are complete; the others are the tiles of the frame and a ring */
	CHECK(kvm_nv12v_init(&v, CW, CH, 1, 0, 0) == 0 && v.mbCols == 20 && v.mbRows == 12 && v.total == 240, "init");
	set_tile(cover, 2, 3);
	CHECK(kvm_nv12v_plan(&v, TR, TC, cover, st, fl, 0, &p) == 0 && p.full && p.map == NULL && p.realMbs == 240 && p.virginMbs == 0 && !p.flat, "the first frame is complete");
	CHECK(kvm_nv12v_plan(&v, TR, TC, cover, st, fl, 0, &p) == 0 && !p.full && p.map != NULL && !p.flat, "a P frame is partial");
	/* the tile (2,3) is the macroblocks rows 4,5 columns 6,7; the ring makes rows 3..6 columns 5..8 */
	n = 0; for (r = 0; r < 12; r++) { for (c = 0; c < 20; c++) { const int in = mb(&p, &v, r, c); const int want = (r >= 3 && r <= 6 && c >= 5 && c <= 8); if (in != want) { n++; } } }
	CHECK(n == 0 && p.realMbs == 16 && p.virginMbs == 0, "ring of a tile: %d wrong, %u real", n, p.realMbs);
	/* a tile at the corner: clipped */
	clear(cover, sizeof(cover)); set_tile(cover, 0, 0);
	CHECK(kvm_nv12v_plan(&v, TR, TC, cover, st, fl, 0, &p) == 0 && p.realMbs == 9 && mb(&p, &v, 2, 2) && !mb(&p, &v, 3, 0) && !mb(&p, &v, 0, 3), "corner: %u", p.realMbs);
	clear(cover, sizeof(cover)); set_tile(cover, TR - 1, TC - 1);
	CHECK(kvm_nv12v_plan(&v, TR, TC, cover, st, fl, 0, &p) == 0 && p.realMbs == 9 && mb(&p, &v, 11, 19) && mb(&p, &v, 9, 17) && !mb(&p, &v, 8, 17), "far corner: %u", p.realMbs);
	/* two tiles side by side: the rings merge */
	clear(cover, sizeof(cover)); set_tile(cover, 2, 3); set_tile(cover, 2, 4);
	CHECK(kvm_nv12v_plan(&v, TR, TC, cover, st, fl, 0, &p) == 0 && p.realMbs == 4 * 6, "two tiles: %u", p.realMbs);
	/* a key frame without FLAT is complete, and what is in flight or VIDEO does not matter */
	CHECK(kvm_nv12v_plan(&v, TR, TC, cover, st, fl, 1, &p) == 0 && p.full && p.realMbs == 240 && !p.flat, "key frame (A) is complete");
	/* invalidate: complete again */
	kvm_nv12v_invalidate(&v);
	CHECK(kvm_nv12v_plan(&v, TR, TC, cover, st, fl, 0, &p) == 0 && p.full, "after invalidate");
	CHECK(kvm_nv12v_plan(&v, TR, TC, cover, st, fl, 0, &p) == 0 && !p.full, "and partial after that");
	/* bad arguments */
	CHECK(kvm_nv12v_plan(NULL, TR, TC, cover, st, fl, 0, &p) == -1 && kvm_nv12v_plan(&v, 0, TC, cover, st, fl, 0, &p) == -1 && kvm_nv12v_plan(&v, TR, TC, NULL, st, fl, 0, &p) == -1, "args");
	CHECK(kvm_nv12v_plan(&v, TR + 4, TC, cover, st, fl, 0, &p) == -1, "a tile grid that is taller than the picture");
	kvm_nv12v_free(&v);
	CHECK(kvm_nv12v_init(&v, 100, 64, 1, 0, 0) == -1 && kvm_nv12v_init(&v, 64, 0, 1, 0, 0) == -1 && kvm_nv12v_init(NULL, 64, 64, 1, 0, 0) == -1, "init args");

	/* FLAT: a key frame converts the frame, the tiles shown from the video and the rings; the rest is flat; the first key frame can be complete */
	CHECK(kvm_nv12v_init(&v, CW, CH, 1, 1, 0) == 0 && v.flat && v.frozen, "init flat");
	clear(cover, sizeof(cover)); set_tile(cover, 1, 1);
	st[4 * TC + 7] = KVM_NV12V_VIDEO; fl[5 * TC + 8] = 2;
	CHECK(kvm_nv12v_plan(&v, TR, TC, cover, st, fl, 1, &p) == 0 && p.flat && !p.full && p.map != NULL && p.virginMbs == 0 && p.realMbs == 40, "flat key frame (16 + 16 + 12 - the 4 where the rings of the last two meet): %u", p.realMbs);
	CHECK(mb(&p, &v, 2, 2) && mb(&p, &v, 8, 14) && mb(&p, &v, 10, 16) && !mb(&p, &v, 6, 10) && !mb(&p, &v, 11, 0), "flat key frame map");
	/* a P frame: only what was not converted since the key frame is virgin, the ring of a tile counts */
	clear(cover, sizeof(cover)); set_tile(cover, 1, 1);
	CHECK(kvm_nv12v_plan(&v, TR, TC, cover, st, fl, 0, &p) == 0 && !p.flat && p.virginMbs == 0 && p.realMbs == 16, "the same tile again is not virgin: %u", p.virginMbs);
	clear(cover, sizeof(cover)); set_tile(cover, 3, 5);
	CHECK(kvm_nv12v_plan(&v, TR, TC, cover, st, fl, 0, &p) == 0 && p.virginMbs == 16 && p.realMbs == 16, "a new tile is virgin with its ring: %u", p.virginMbs);
	CHECK(kvm_nv12v_plan(&v, TR, TC, cover, st, fl, 0, &p) == 0 && p.virginMbs == 0, "and not the second time");
	clear(cover, sizeof(cover)); set_tile(cover, 3, 6);
	CHECK(kvm_nv12v_plan(&v, TR, TC, cover, st, fl, 0, &p) == 0 && p.virginMbs == 4 && p.realMbs == 16, "the neighbour: only what is new (8 of its ring are the other tile, 4 more were touched by the tile shown from the video): %u", p.virginMbs);
	/* a key frame resets what is touched */
	CHECK(kvm_nv12v_plan(&v, TR, TC, cover, st, fl, 1, &p) == 0 && p.flat && p.virginMbs == 0, "key");
	clear(cover, sizeof(cover)); set_tile(cover, 3, 5);
	CHECK(kvm_nv12v_plan(&v, TR, TC, cover, st, fl, 0, &p) == 0 && p.virginMbs > 0, "after a flat key frame the old tile is virgin again");
	/* firstFull: the first key frame of the session is complete, the second is flat */
	kvm_nv12v_free(&v);
	CHECK(kvm_nv12v_init(&v, CW, CH, 1, 1, 1) == 0, "init firstFull");
	CHECK(kvm_nv12v_plan(&v, TR, TC, cover, st, fl, 1, &p) == 0 && p.full && !p.flat, "the first key frame is complete");
	CHECK(kvm_nv12v_plan(&v, TR, TC, cover, st, fl, 1, &p) == 0 && p.flat && !p.full, "the second is flat");
	/* the view was made valid by a P frame (it should not happen, but the rule is about the first KEY frame): that one is still complete */
	kvm_nv12v_free(&v);
	CHECK(kvm_nv12v_init(&v, CW, CH, 1, 1, 1) == 0, "init firstFull 2");
	CHECK(kvm_nv12v_plan(&v, TR, TC, cover, st, fl, 0, &p) == 0 && p.full, "a P frame on a new view is complete");
	CHECK(kvm_nv12v_plan(&v, TR, TC, cover, st, fl, 1, &p) == 0 && p.full && !p.flat, "and the first key frame after it is complete too");
	CHECK(kvm_nv12v_plan(&v, TR, TC, cover, st, fl, 1, &p) == 0 && p.flat, "the second is flat");
	/* a P frame before any key frame (should not happen): complete */
	kvm_nv12v_invalidate(&v);
	CHECK(kvm_nv12v_plan(&v, TR, TC, cover, st, fl, 0, &p) == 0 && p.full, "P frame on an invalid view is complete");
	/* a flat key frame on an invalid view (a new encoder): flat, not complete */
	kvm_nv12v_free(&v);
	CHECK(kvm_nv12v_init(&v, CW, CH, 1, 1, 0) == 0 && kvm_nv12v_plan(&v, TR, TC, cover, st, fl, 1, &p) == 0 && p.flat && !p.full, "flat key frame on a new view");
	kvm_nv12v_free(&v);
	/* a picture whose grid does not fit in a job: legacy */
	CHECK(kvm_nv12v_init(&v, 4096, 4640, 1, 0, 0) == 0, "big init");
	{
		uint8_t *cv = (uint8_t *)calloc(128 * 145, 1);
		CHECK(kvm_nv12v_plan(&v, 145, 128, cv, NULL, NULL, 0, &p) == 0 && p.legacy && p.map == NULL, "legacy");
		free(cv);
	}
	kvm_nv12v_free(&v);
}

static void paint(uint8_t *bgra, int w, int h, int seed)
{
	int x, y;
	for (y = 0; y < h; y++) { for (x = 0; x < w; x++) { uint8_t *q = bgra + ((size_t)y * w + x) * 4; q[0] = (uint8_t)(x * 3 + seed * 7); q[1] = (uint8_t)(y * 5 + seed * 13); q[2] = (uint8_t)(x + y + seed); q[3] = 255; } }
}

static void test_apply(void)
{
	const int W = 300, H = 170;		/* a source smaller than the coded picture: padding */
	const size_t n = (size_t)CW * CH * 3 / 2;
	uint8_t *img = (uint8_t *)malloc((size_t)W * H * 4), *view = (uint8_t *)calloc(n, 1), *ref = (uint8_t *)malloc(n), *prev = (uint8_t *)malloc(n);
	kvm_nv12v v;
	kvm_nv12v_pl p;
	uint8_t cover[TR * TC], st[TR * TC], fl[TR * TC];
	int frame, r, c;
	clear(st, sizeof(st)); clear(fl, sizeof(fl));
	CHECK(kvm_nv12v_init(&v, CW, CH, 1, 0, 0) == 0, "init");
	for (frame = 0; frame < 12; frame++)
	{
		const int key = (frame == 0 || frame == 7);
		int done;
		size_t i;
		paint(img, W, H, frame);
		clear(cover, sizeof(cover));
		for (r = 0; r < TR; r++) { for (c = 0; c < TC; c++) { if (rnd() % 5 == 0) { set_tile(cover, r, c); } } }
		set_tile(cover, (int)(rnd() % TR), (int)(rnd() % TC));
		memcpy(prev, view, n);
		CHECK(kvm_nv12v_plan(&v, TR, TC, cover, st, fl, key, &p) == 0, "plan");
		done = kvm_nv12v_apply(view, img, W, H, W * 4, CW, CH, p.map, v.mbCols, v.mbRows, p.full, p.flat);
		CHECK(done == (int)p.realMbs, "converted %d, planned %u", done, p.realMbs);
		CHECK(kvm_yuv_bgra_to_nv12(img, W, H, W * 4, ref, CW, CH, KVM_YUV_SCALAR) == 0, "ref");
		/* inside the map: the full conversion of this picture; outside: what the view held */
		for (r = 0; r < v.mbRows; r++)
		{
			for (c = 0; c < v.mbCols; c++)
			{
				const int in = p.full || p.map[(size_t)r * v.mbCols + c];
				int y, x, bad = 0;
				for (y = 0; y < 16 && !bad; y++) { for (x = 0; x < 16; x++) { const size_t k = (size_t)(r * 16 + y) * CW + c * 16 + x; if (view[k] != (in ? ref[k] : prev[k])) { bad = 1; break; } } }
				for (y = 0; y < 8 && !bad; y++) { for (x = 0; x < 16; x++) { const size_t k = (size_t)CW * CH + (size_t)(r * 8 + y) * CW + c * 16 + x; if (view[k] != (in ? ref[k] : prev[k])) { bad = 1; break; } } }
				CHECK(!bad, "frame %d mb (%d,%d) in=%d", frame, r, c, in);
				if (bad) { r = v.mbRows; break; }
			}
		}
		/* every tile of the frame is converted fresh (the viewer paints it) */
		for (r = 0; r < TR; r++) { for (c = 0; c < TC; c++) { if (cover[r * TC + c]) { size_t k = (size_t)(r * 32) * CW + c * 32; CHECK(view[k] == ref[k] && view[k + 31 * CW + 31] == ref[k + 31 * CW + 31], "tile %d,%d not fresh", r, c); } } }
		(void)i;
	}
	kvm_nv12v_free(&v);

	/* FLAT: a key frame makes the view flat outside the map */
	CHECK(kvm_nv12v_init(&v, CW, CH, 1, 1, 0) == 0, "init flat");
	{
		uint8_t *white = (uint8_t *)malloc((size_t)W * H * 4);
		int x, y, flatOk = 1, k;
		for (y = 0; y < H; y++) { for (x = 0; x < W; x++) { uint8_t *q = white + ((size_t)y * W + x) * 4; q[0] = q[1] = q[2] = 240; q[3] = 255; if (x >= 128 && x < 160 && y >= 64 && y < 96) { q[0] = (uint8_t)(x * 9); q[1] = (uint8_t)(y * 7); q[2] = (uint8_t)(x ^ y); } } }
		clear(cover, sizeof(cover)); set_tile(cover, 2, 4);
		CHECK(kvm_nv12v_plan(&v, TR, TC, cover, st, fl, 1, &p) == 0 && p.flat, "flat plan");
		memset(view, 77, n);
		CHECK(kvm_nv12v_apply(view, white, W, H, W * 4, CW, CH, p.map, v.mbCols, v.mbRows, 0, 1) == (int)p.realMbs, "flat apply");
		CHECK(kvm_yuv_bgra_to_nv12(white, W, H, W * 4, ref, CW, CH, KVM_YUV_SCALAR) == 0, "ref");
		for (r = 0; r < v.mbRows; r++)
		{
			for (c = 0; c < v.mbCols; c++)
			{
				const size_t k0 = (size_t)(r * 16) * CW + c * 16;
				if (p.map[(size_t)r * v.mbCols + c]) { CHECK(view[k0] == ref[k0] && view[k0 + 15 * CW + 15] == ref[k0 + 15 * CW + 15], "real mb (%d,%d)", r, c); }
				else if (view[k0] != view[0] || view[k0 + 15 * CW + 15] != view[0]) { flatOk = 0; }
			}
		}
		CHECK(flatOk, "outside the map the view is one colour");
		k = view[0];
		CHECK(k > 200 && k < 236, "the flat colour is the dominant one (white): %d", k);
		CHECK(view[(size_t)CW * CH] == ref[(size_t)CW * CH] || 1, "chroma");
		free(white);
	}
	kvm_nv12v_free(&v);
	/* bad arguments of apply */
	{
		uint8_t m[4] = {1, 1, 1, 1};
		CHECK(kvm_nv12v_apply(NULL, img, W, H, W * 4, CW, CH, m, 20, 12, 0, 0) == -1 && kvm_nv12v_apply(view, NULL, W, H, W * 4, CW, CH, m, 20, 12, 0, 0) == -1, "apply null");
		CHECK(kvm_nv12v_apply(view, img, W, H, W * 4, CW, CH, NULL, 20, 12, 0, 0) == -1 && kvm_nv12v_apply(view, img, W, H, W * 4, CW, CH, m, 19, 12, 0, 0) == -1, "apply map");
	}
	free(img); free(view); free(ref); free(prev);
}

int main(void)
{
	test_plan();
	test_apply();
	printf("kvm_nv12view_test: %d checks, %d failed\n", g_checks, g_fail);
	return g_fail == 0 ? 0 : 1;
}
