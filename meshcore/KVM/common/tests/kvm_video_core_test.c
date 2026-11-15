/*
Tests of kvm_video_core.c (the tile bookkeeping, the rectangles of a frame, levels, the controller) and of the frame builder of kvm_stream.c that the video path uses
(the 243 layout, its limits). Time is injected, so every scenario is deterministic.
Run with run-kvm-video-core-tests.sh (gcc in a container: plain build with -Wall -Wextra -Werror -pedantic, then AddressSanitizer + UndefinedBehaviorSanitizer).
*/

#include "../kvm_video_core.h"
#include "../kvm_stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gFail = 0, gChecks = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); gChecks++; if (_a != _b) { gFail++; printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

static uint32_t gRng = 12345u;
static uint32_t rnd(void) { gRng = (gRng * 1664525u) + 1013904223u; return gRng >> 8; }

// ---------------------------------------------------------------------------------------------------------------------------------------
// the 243 on the wire: layout and limits
// ---------------------------------------------------------------------------------------------------------------------------------------

static void test_frame_layout(void)
{
	// a P frame: screen 1366x768 (coded 1376x768), two rectangles, 9 bytes of Annex B
	static const uint8_t annex[] = { 0, 0, 0, 1, 0x41, 0x9a, 1, 2, 3 };
	uint16_t rects[8] = { 0, 0, 64, 32, 1344, 736, 22, 32 };
	uint8_t a[256], b[256];
	kvm_frame_params p;
	kvm_frame f;
	size_t n, hdr;
	memset(&p, 0, sizeof(p));
	p.seq = 0x01020304u; p.ts = 0x0a0b0c0du; p.flags = 0; p.gen = 7; p.codedW = 1376; p.codedH = 768; p.nRects = 2; p.rects = rects; p.annexB = annex; p.annexLen = sizeof(annex);
	n = kvm_stream_build_frame(a, sizeof(a), &p);
	CHECK_EQ(n, 8 + 20 + 16 + 9);
	// the bytes, big endian, no padding after 8 + N
	{
		static const uint8_t want[] = {
			0, 27, 0, 8, 0, 0, 0, 45,									// JUMBO: cmd 27, size 8, N = 20 + 16 + 9
			0, 243, 0, 0, 1, 2, 3, 4, 0x0a, 0x0b, 0x0c, 0x0d,			// cmd 243, reserved, seq, ts
			0, 7, 0x05, 0x60, 0x03, 0x00, 0, 2,							// flags, gen, codedW 1376, codedH 768, nRects
			0, 0, 0, 0, 0, 64, 0, 32, 0x05, 0x40, 0x02, 0xe0, 0, 22, 0, 32, // rectangles
			0, 0, 0, 1, 0x41, 0x9a, 1, 2, 3 };
		CHECK(n == sizeof(want) && memcmp(a, want, sizeof(want)) == 0);
	}
	CHECK_EQ(kvm_stream_parse_frame(a, n, 1366, 768, &f), KVM_REJ_NONE);
	CHECK_EQ(f.nRects, 2); CHECK_EQ(f.annexLen, sizeof(annex)); CHECK_EQ(f.n, 45);
	// in two parts: the header, then the data placed behind it (no copy of the data by the builder)
	hdr = kvm_stream_build_frame_header(b, sizeof(b), &p);
	CHECK_EQ(hdr, kvm_stream_frame_header_len(2));
	CHECK_EQ(hdr, 28 + 16);
	memcpy(b + hdr, annex, sizeof(annex));
	CHECK(memcmp(a, b, n) == 0);
	// a marker through the header builder is the 28 bytes of kvm_stream_build_marker
	{
		uint8_t m1[KVM_MARKER_BYTES], m2[KVM_MARKER_BYTES];
		kvm_frame_params q;
		memset(&q, 0, sizeof(q));
		q.seq = 9; q.ts = 100; q.flags = KVM_FRAME_MARKER; q.gen = 3;
		CHECK_EQ(kvm_stream_build_frame_header(m1, sizeof(m1), &q), KVM_MARKER_BYTES);
		kvm_stream_build_marker(m2, 9, 100, 3);
		CHECK(memcmp(m1, m2, sizeof(m1)) == 0);
	}
	// errors: a frame without rectangles, no room, too many rectangles, a frame that does not fit the 8 MiB of N
	{
		kvm_frame_params q = p;
		q.nRects = 0;
		CHECK_EQ(kvm_stream_build_frame_header(b, sizeof(b), &q), 0);
		CHECK_EQ(kvm_stream_build_frame_header(b, 43, &p), 0);
		CHECK_EQ(kvm_stream_build_frame_header(b, 44, &p), 44);
		q = p; q.nRects = 257;
		CHECK_EQ(kvm_stream_build_frame_header(b, sizeof(b), &q), 0);
		q = p; q.annexLen = KVM_FRAME_MAX_N - 20 - 16 + 1;	// N would be 8 MiB + 1
		CHECK_EQ(kvm_stream_build_frame_header(b, sizeof(b), &q), 0);
		q.annexLen = KVM_FRAME_MAX_N - 20 - 16;				// N is exactly 8 MiB
		CHECK_EQ(kvm_stream_build_frame_header(b, sizeof(b), &q), 44);
		CHECK_EQ(b[4], 0); CHECK_EQ(b[5], 0x80); CHECK_EQ(b[6], 0); CHECK_EQ(b[7], 0);	// N = 0x00800000
	}
	// the frame of the video path with the most rectangles: 256 pass the parser, the data behind the header is where the parser looks for it
	{
		uint16_t r256[KVM_FRAME_MAX_RECTS * 4];
		unsigned i;
		uint8_t *big = (uint8_t *)malloc(28 + (8 * 258) + 9);	// room for one rectangle too many: only the limit may refuse it
		kvm_frame_params q;
		uint16_t r257[257 * 4];
		memset(&q, 0, sizeof(q));
		for (i = 0; i < 256; i++) { r256[4 * i] = (uint16_t)((i % 60) * 32); r256[(4 * i) + 1] = (uint16_t)((i / 60) * 32); r256[(4 * i) + 2] = 32; r256[(4 * i) + 3] = 32; }
		q.seq = 1; q.gen = 1; q.codedW = 1920; q.codedH = 1088; q.nRects = 256; q.rects = r256; q.annexB = annex; q.annexLen = sizeof(annex);
		n = kvm_stream_build_frame(big, 28 + (8 * 256) + 9, &q);
		CHECK_EQ(n, 28 + (8 * 256) + 9);
		CHECK_EQ(kvm_stream_parse_frame(big, n, 1920, 1080, &f), KVM_REJ_NONE);
		CHECK_EQ(f.nRects, 256);
		CHECK(f.annexB == big + 28 + (8 * 256));
		memcpy(r257, r256, sizeof(r256));
		r257[1024] = 0; r257[1025] = 0; r257[1026] = 32; r257[1027] = 32;
		q.nRects = 257; q.rects = r257;
		CHECK_EQ(kvm_stream_build_frame(big, 28 + (8 * 258) + 9, &q), 0);
		CHECK_EQ(kvm_stream_build_frame_header(big, 28 + (8 * 258) + 9, &q), 0);
		free(big);
	}
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// tile bookkeeping and rectangles
// ---------------------------------------------------------------------------------------------------------------------------------------

static unsigned grid_cols(unsigned w) { return (w + 31) / 32; }

// Checks everything a frame promises about its rectangles (the receiver checks the same in kvm_stream_parse_frame): the cover is exactly the union of the rectangles,
// the rectangles are tile aligned and inside the screen, and every pending tile is covered. Returns the number of rectangles.
static int check_rects(const kvm_vtiles *t, unsigned sw, unsigned sh, int *outN)
{
	uint16_t rects[KVM_FRAME_MAX_RECTS * 4];
	uint8_t *cover = (uint8_t *)malloc((size_t)t->rows * (size_t)t->cols);
	uint8_t *uni = (uint8_t *)calloc((size_t)t->rows * (size_t)t->cols, 1);
	int n, i, r, c, ok = 1;
	n = kvm_vtiles_make_rects(t, sw, sh, rects, cover);
	if (outN != NULL) { *outN = n; }
	CHECK(n >= 0 && n <= KVM_FRAME_MAX_RECTS);
	if (n < 0) { free(cover); free(uni); return n; }
	for (i = 0; i < n; i++)
	{
		unsigned x = rects[4 * i], y = rects[(4 * i) + 1], w = rects[(4 * i) + 2], h = rects[(4 * i) + 3];
		CHECK(w >= 1 && h >= 1 && x + w <= sw && y + h <= sh);
		CHECK_EQ(x % 32, 0); CHECK_EQ(y % 32, 0);
		CHECK(w % 32 == 0 || x + w == sw);
		CHECK(h % 32 == 0 || y + h == sh);
		for (r = (int)(y / 32); r <= (int)((y + h - 1) / 32); r++)
		{
			for (c = (int)(x / 32); c <= (int)((x + w - 1) / 32); c++) { uni[(size_t)r * (size_t)t->cols + (size_t)c] = 1; }
		}
	}
	for (r = 0; r < t->rows; r++)
	{
		for (c = 0; c < t->cols; c++)
		{
			size_t k = (size_t)r * (size_t)t->cols + (size_t)c;
			if (cover[k] != uni[k]) { ok = 0; }
			if (kvm_vtiles_get(t, r, c) == KVM_VT_PENDING && !cover[k]) { ok = 0; }
		}
	}
	CHECK(ok);
	CHECK_EQ(n == 0, t->nPending == 0);
	free(cover); free(uni);
	return n;
}

static void test_tiles_basic(void)
{
	kvm_vtiles t;
	uint8_t map[64];
	CHECK_EQ(kvm_vtiles_init(&t, 0, 6), -1);
	CHECK_EQ(kvm_vtiles_init(&t, 4, 0), -1);
	CHECK_EQ(kvm_vtiles_init(&t, 4, 6), 0);
	CHECK_EQ(t.nPending, 0); CHECK_EQ(kvm_vtiles_get(&t, 1, 1), KVM_VT_NONE); CHECK_EQ(kvm_vtiles_get(&t, 9, 9), KVM_VT_NONE); CHECK_EQ(kvm_vtiles_get(&t, -1, 0), KVM_VT_NONE);
	kvm_vtiles_claim(&t, 1, 1, 2, 3);
	CHECK_EQ(t.nPending, 6); CHECK_EQ(kvm_vtiles_get(&t, 2, 3), KVM_VT_PENDING); CHECK_EQ(kvm_vtiles_get(&t, 0, 0), KVM_VT_NONE);
	kvm_vtiles_claim(&t, 1, 1, 1, 1);			// claiming again changes nothing
	CHECK_EQ(t.nPending, 6);
	kvm_vtiles_claim(&t, 3, 5, 9, 9);			// clipped to the grid
	CHECK_EQ(t.nPending, 7);
	// I3: sending a pending tile as JPEG takes it off the pending set
	kvm_vtiles_jpeg(&t, 1, 1);
	CHECK_EQ(kvm_vtiles_get(&t, 1, 1), KVM_VT_NONE); CHECK_EQ(t.nPending, 6);
	kvm_vtiles_jpeg(&t, 1, 1);					// twice, or a tile that was never pending: nothing
	CHECK_EQ(t.nPending, 6);
	// the cover decides: the tiles outside it keep their state, the pending ones included (a frame paints exactly its rectangles)
	{
		uint8_t cover[24];
		memset(cover, 0, sizeof(cover));
		cover[(0 * 6) + 0] = 1;				// a tile that was NONE
		cover[(2 * 6) + 2] = 1;				// a tile that is PENDING
		CHECK_EQ(kvm_vtiles_get(&t, 1, 2), KVM_VT_PENDING);
		CHECK_EQ(kvm_vtiles_commit(&t, cover), 2);
		CHECK_EQ(kvm_vtiles_get(&t, 0, 0), KVM_VT_VIDEO); CHECK_EQ(kvm_vtiles_get(&t, 2, 2), KVM_VT_VIDEO);
		CHECK_EQ(kvm_vtiles_get(&t, 1, 2), KVM_VT_PENDING);		// pending, not covered: stays
		CHECK_EQ(kvm_vtiles_get(&t, 3, 3), KVM_VT_NONE);		// not covered: stays
		kvm_vtiles_jpeg(&t, 0, 0); kvm_vtiles_jpeg(&t, 2, 2);
		kvm_vtiles_claim(&t, 2, 2, 2, 2);	// back to the state the rest of the test starts from
	}
	// a frame covers the pending tiles
	{
		uint16_t rects[KVM_FRAME_MAX_RECTS * 4];
		uint8_t cover[24];
		CHECK(kvm_vtiles_make_rects(&t, 6 * 32, 4 * 32, rects, cover) >= 1);
		CHECK_EQ(kvm_vtiles_commit(&t, cover) >= 6, 1);
		CHECK_EQ(t.nPending, 0); CHECK(t.nVideo >= 6);
	}
	// a VIDEO tile that moves again is pending (I2: one place); sent as JPEG it is NONE
	kvm_vtiles_claim(&t, 1, 2, 1, 2);
	CHECK_EQ(kvm_vtiles_get(&t, 1, 2), KVM_VT_PENDING);
	CHECK_EQ(t.nPending, 1);
	kvm_vtiles_jpeg(&t, 2, 3);
	CHECK_EQ(kvm_vtiles_get(&t, 2, 3), KVM_VT_NONE);
	// I7: the pending tiles go back to JPEG, VIDEO tiles stay
	{
		unsigned nv = t.nVideo;
		CHECK_EQ(kvm_vtiles_take_pending(&t, map), 1);
		CHECK_EQ(map[(1 * 6) + 2], 1);
		CHECK_EQ(t.nPending, 0); CHECK_EQ(t.nVideo, nv);
	}
	// I8: the VIDEO tiles go back to JPEG
	{
		unsigned nv = t.nVideo;
		CHECK_EQ(kvm_vtiles_take_video(&t, map, 0), nv);
		CHECK_EQ(t.nVideo, 0);
	}
	kvm_vtiles_claim(&t, 0, 0, 0, 1);
	CHECK_EQ(kvm_vtiles_take_video(&t, map, 0), 0); CHECK_EQ(t.nPending, 2);
	CHECK_EQ(kvm_vtiles_take_video(&t, map, 1), 2); CHECK_EQ(t.nPending, 0);	// a REFRESH takes the pending ones too
	kvm_vtiles_free(&t);
}

static void test_rects_shapes(void)
{
	kvm_vtiles t;
	uint16_t rects[KVM_FRAME_MAX_RECTS * 4];
	uint8_t cover[64 * 40];
	int n;
	// screen 1366x768: 43 x 24 tiles; the last column is 22 pixels wide
	CHECK_EQ(kvm_vtiles_init(&t, 24, 43), 0);
	CHECK_EQ(kvm_vtiles_make_rects(&t, 1366, 768, rects, cover), 0);	// nothing pending
	CHECK_EQ(kvm_vtiles_make_rects(&t, 1366, 800, rects, cover), -1);	// the grid does not match the screen
	CHECK_EQ(kvm_vtiles_make_rects(&t, 1920, 768, rects, cover), -1);
	kvm_vtiles_claim(&t, 5, 42, 5, 42);
	n = kvm_vtiles_make_rects(&t, 1366, 768, rects, cover);
	CHECK_EQ(n, 1); CHECK_EQ(rects[0], 1344); CHECK_EQ(rects[1], 160); CHECK_EQ(rects[2], 22); CHECK_EQ(rects[3], 32);
	kvm_vtiles_reset(&t);
	// the bottom row: 768 = 24 * 32, the last row is full; with 770 pixels the grid has 25 rows and the last one is 2 pixels high
	kvm_vtiles_free(&t);
	CHECK_EQ(kvm_vtiles_init(&t, 25, 43), 0);
	kvm_vtiles_claim(&t, 24, 0, 24, 1);
	n = kvm_vtiles_make_rects(&t, 1366, 770, rects, cover);
	CHECK_EQ(n, 1); CHECK_EQ(rects[1], 768); CHECK_EQ(rects[3], 2); CHECK_EQ(rects[2], 64);
	kvm_vtiles_reset(&t);
	// a block, a row, a second block that has the same columns as the first (still two rectangles), an L
	kvm_vtiles_claim(&t, 2, 3, 4, 6);
	kvm_vtiles_claim(&t, 6, 3, 6, 6);	// row 5 between them is empty: not joined
	kvm_vtiles_claim(&t, 8, 10, 12, 10);
	kvm_vtiles_claim(&t, 12, 11, 12, 15);
	n = check_rects(&t, 1366, 770, NULL);
	CHECK_EQ(n, 4);
	n = kvm_vtiles_make_rects(&t, 1366, 770, rects, cover);
	CHECK_EQ(rects[0], 96); CHECK_EQ(rects[1], 64); CHECK_EQ(rects[2], 128); CHECK_EQ(rects[3], 96);	// the block (columns 3..6, rows 2..4)
	CHECK_EQ(rects[4], 96); CHECK_EQ(rects[5], 192); CHECK_EQ(rects[6], 128); CHECK_EQ(rects[7], 32);	// row 6
	CHECK_EQ(rects[8], 320); CHECK_EQ(rects[9], 256); CHECK_EQ(rects[10], 32); CHECK_EQ(rects[11], 128);	// column 10, rows 8..11
	CHECK_EQ(rects[12], 320); CHECK_EQ(rects[13], 384); CHECK_EQ(rects[14], 192); CHECK_EQ(rects[15], 32);	// row 12, columns 10..15 (the L: its last row is wider)
	kvm_vtiles_free(&t);
}

// A pattern that cannot fit in 256 rectangles: a chess board of the 1920x1080 screen (60 x 34 tiles, 1020 single tile runs)
static void test_rects_over_limit(void)
{
	kvm_vtiles t;
	int r, c, n, ok;
	CHECK_EQ(kvm_vtiles_init(&t, 34, 60), 0);
	for (r = 0; r < 34; r++) { for (c = 0; c < 60; c++) { if (((r + c) & 1) == 0) { kvm_vtiles_claim(&t, r, c, r, c); } } }
	CHECK_EQ(t.nPending, 1020);
	n = check_rects(&t, 1920, 1080, NULL);
	CHECK(n >= 1 && n <= 256);
	{
		uint16_t rects[KVM_FRAME_MAX_RECTS * 4];
		uint8_t cover[34 * 60];
		n = kvm_vtiles_make_rects(&t, 1920, 1080, rects, cover);
		// the frame with these rectangles passes the receiver's checks
		{
			uint8_t buf[28 + (8 * 256) + 9];
			static const uint8_t annex[] = { 0, 0, 0, 1, 0x41, 0x9a, 1, 2, 3 };
			kvm_frame_params p;
			kvm_frame f;
			memset(&p, 0, sizeof(p));
			p.seq = 5; p.gen = 1; p.codedW = 1920; p.codedH = 1088; p.nRects = (unsigned)n; p.rects = rects; p.annexB = annex; p.annexLen = sizeof(annex);
			ok = kvm_stream_build_frame(buf, sizeof(buf), &p) > 0;
			CHECK(ok);
			CHECK_EQ(kvm_stream_parse_frame(buf, kvm_stream_frame_header_len((unsigned)n) + sizeof(annex), 1920, 1080, &f), KVM_REJ_NONE);
		}
	}
	// every tile of a screen: one rectangle
	for (r = 0; r < 34; r++) { kvm_vtiles_claim(&t, r, 0, r, 59); }
	CHECK_EQ(check_rects(&t, 1920, 1080, &n), 1);
	{
		uint16_t rects[KVM_FRAME_MAX_RECTS * 4];
		uint8_t cover[34 * 60];
		CHECK_EQ(kvm_vtiles_make_rects(&t, 1920, 1080, rects, cover), 1);
		CHECK_EQ(rects[0], 0); CHECK_EQ(rects[1], 0); CHECK_EQ(rects[2], 1920); CHECK_EQ(rects[3], 1080);
	}
	// exactly 256 and 257 separate single tile rectangles: 256 stay as they are (no merging), 257 are merged
	kvm_vtiles_reset(&t);
	{
		int k = 0;
		for (r = 0; r < 34 && k < 256; r += 2) { for (c = 0; c < 60 && k < 256; c += 2) { kvm_vtiles_claim(&t, r, c, r, c); k++; } }
		CHECK_EQ(k, 256);
		CHECK_EQ(check_rects(&t, 1920, 1080, &n), 256);
		kvm_vtiles_claim(&t, 33, 59, 33, 59);
		n = check_rects(&t, 1920, 1080, NULL);
		CHECK(n < 257);
		CHECK(n >= 1);
	}
	kvm_vtiles_free(&t);
}

static void test_rects_property(void)
{
	int iter;
	for (iter = 0; iter < 300; iter++)
	{
		static const unsigned sizes[][2] = { { 1920, 1080 }, { 1366, 768 }, { 800, 600 }, { 3840, 2160 }, { 33, 33 }, { 32, 32 }, { 2560, 1440 } };
		unsigned sw = sizes[iter % 7][0], sh = sizes[iter % 7][1];
		kvm_vtiles t;
		int rows = (int)((sh + 31) / 32), cols = (int)grid_cols(sw), n, k, density = (int)(rnd() % 100), nrect = (int)(rnd() % 40);
		uint8_t *cover = (uint8_t *)malloc((size_t)rows * (size_t)cols);
		uint16_t rects[KVM_FRAME_MAX_RECTS * 4];
		unsigned before;
		CHECK_EQ(kvm_vtiles_init(&t, rows, cols), 0);
		if (iter & 1)
		{
			for (k = 0; k < rows * cols; k++) { if ((int)(rnd() % 100) < density) { kvm_vtiles_claim(&t, k / cols, k % cols, k / cols, k % cols); } }
		}
		else
		{
			for (k = 0; k < nrect; k++)
			{
				int r0 = (int)(rnd() % (unsigned)rows), c0 = (int)(rnd() % (unsigned)cols);
				kvm_vtiles_claim(&t, r0, c0, r0 + (int)(rnd() % 8), c0 + (int)(rnd() % 12));
			}
		}
		before = t.nPending;
		n = check_rects(&t, sw, sh, NULL);
		CHECK(n >= 0);
		// the frame is written: the cover is VIDEO, nothing is pending (I3), and the tiles that were not pending but covered are VIDEO too
		(void)kvm_vtiles_make_rects(&t, sw, sh, rects, cover);
		{
			uint8_t *map = (uint8_t *)malloc((size_t)rows * (size_t)cols);
			memcpy(map, cover, (size_t)rows * (size_t)cols);
			CHECK(kvm_vtiles_commit(&t, cover) >= before);
			CHECK_EQ(t.nPending, 0);
			for (k = 0; k < rows * cols; k++) { CHECK_EQ(kvm_vtiles_get(&t, k / cols, k % cols) == KVM_VT_VIDEO, map[k] != 0 || 0); if (gFail > 20) { break; } }
			free(map);
		}
		kvm_vtiles_free(&t);
		free(cover);
		if (gFail > 20) { return; }
	}
}

// A model of what the agent does with the tiles, written without the module: a tile has one state; a random sequence of the events of the loop is run on both and compared.
static void test_tiles_model(void)
{
	int iter;
	for (iter = 0; iter < 60; iter++)
	{
		const int rows = 12, cols = 20;
		const unsigned sw = (unsigned)(cols * 32) - 5, sh = (unsigned)(rows * 32);
		kvm_vtiles t;
		uint8_t model[12 * 20], map[12 * 20], cover[12 * 20];
		uint16_t rects[KVM_FRAME_MAX_RECTS * 4];
		int step, k;
		memset(model, 0, sizeof(model));
		CHECK_EQ(kvm_vtiles_init(&t, rows, cols), 0);
		for (step = 0; step < 400; step++)
		{
			int ev = (int)(rnd() % 10);
			if (ev <= 3)
			{
				int r0 = (int)(rnd() % rows), c0 = (int)(rnd() % cols), r1 = r0 + (int)(rnd() % 4), c1 = c0 + (int)(rnd() % 6), r, c;
				kvm_vtiles_claim(&t, r0, c0, r1, c1);
				for (r = r0; r <= r1 && r < rows; r++) { for (c = c0; c <= c1 && c < cols; c++) { model[(r * cols) + c] = KVM_VT_PENDING; } }
			}
			else if (ev <= 5)
			{
				int r = (int)(rnd() % rows), c = (int)(rnd() % cols);
				kvm_vtiles_jpeg(&t, r, c);
				model[(r * cols) + c] = KVM_VT_NONE;
				CHECK(kvm_vtiles_get(&t, r, c) != KVM_VT_PENDING);	// I3
			}
			else if (ev <= 7)
			{
				int n = kvm_vtiles_make_rects(&t, sw, sh, rects, cover);
				CHECK(n >= 0);
				for (k = 0; k < rows * cols; k++) { if (model[k] == KVM_VT_PENDING) { CHECK(cover[k]); } }	// a frame paints every pending tile
				(void)kvm_vtiles_commit(&t, cover);
				for (k = 0; k < rows * cols; k++) { if (cover[k]) { model[k] = KVM_VT_VIDEO; } }
				CHECK_EQ(t.nPending, 0);
			}
			else if (ev == 8)
			{
				unsigned want = 0;
				for (k = 0; k < rows * cols; k++) { if (model[k] == KVM_VT_PENDING) { want++; model[k] = KVM_VT_NONE; } }
				CHECK_EQ(kvm_vtiles_take_pending(&t, map), want);	// I7: video off, the pending tiles are JPEG again
				CHECK_EQ(t.nPending, 0);
			}
			else
			{
				unsigned want = 0;
				int all = (int)(rnd() & 1);
				for (k = 0; k < rows * cols; k++) { if (model[k] == KVM_VT_VIDEO || (all && model[k] == KVM_VT_PENDING)) { want++; model[k] = KVM_VT_NONE; } }
				CHECK_EQ(kvm_vtiles_take_video(&t, map, all), want);	// I8: repair
				CHECK_EQ(t.nVideo, 0);
			}
			{
				unsigned np = 0, nv = 0;
				for (k = 0; k < rows * cols; k++)
				{
					CHECK_EQ(kvm_vtiles_get(&t, k / cols, k % cols), model[k]);
					if (model[k] == KVM_VT_PENDING) { np++; } else if (model[k] == KVM_VT_VIDEO) { nv++; }
				}
				CHECK_EQ(t.nPending, np); CHECK_EQ(t.nVideo, nv);
			}
			if (gFail > 20) { return; }
		}
		kvm_vtiles_free(&t);
	}
}


// the model of the tiles with frames in flight (INFLIGHT), written without the module: a tile has a base state and a mask; the events are the ones of the glue (claim, dispatch of the
// pending tiles into a slot, land, abort, cancel, JPEG after the barrier, take_pending / take_video after the barrier). Besides the states, a model of the viewer's canvas is kept: every
// tile has a version of its content (a claim or a JPEG-class change makes a new one), a frame paints the versions it was started with, a JPEG paints the current one. The version on the
// canvas never goes down; at the end, when everything has landed, the canvas has the latest version of every tile that changed (nothing is lost).
typedef struct inflight_model
{
	int rows, cols, depth;
	uint8_t base[12 * 20], mask[12 * 20];
	unsigned ver[12 * 20], canvas[12 * 20], paint[3][12 * 20];
	uint8_t used[3], cov[3][12 * 20];
} inflight_model;

static void test_tiles_inflight_model(void)
{
	int session;
	for (session = 0; session < 300; session++)
	{
		const int rows = 12, cols = 20, n = rows * cols, depth = 1 + (session % 3);
		const unsigned sw = (unsigned)(cols * 32), sh = (unsigned)(rows * 32);
		kvm_vtiles t;
		inflight_model m;
		uint8_t map[12 * 20], cover[12 * 20];
		uint16_t rects[KVM_FRAME_MAX_RECTS * 4];
		int step, k, s;
		unsigned fifo[3], nf = 0;	// the slots in the order of dispatch
		memset(&m, 0, sizeof(m));
		m.rows = rows; m.cols = cols; m.depth = depth;
		CHECK_EQ(kvm_vtiles_init(&t, rows, cols), 0);
		for (step = 0; step < 500; step++)
		{
			int ev = (int)(rnd() % 14);
			if (ev <= 3)	// claim: the content changes and goes to the video
			{
				int r0 = (int)(rnd() % rows), c0 = (int)(rnd() % cols), r1 = r0 + (int)(rnd() % 4), c1 = c0 + (int)(rnd() % 6), r, c;
				kvm_vtiles_claim(&t, r0, c0, r1, c1);
				for (r = r0; r <= r1 && r < rows; r++) { for (c = c0; c <= c1 && c < cols; c++) { m.base[(r * cols) + c] = KVM_VT_PENDING; m.ver[(r * cols) + c]++; } }
			}
			else if (ev == 4)	// the tile changes and is NOT claimed (a class 0 change): it is sent as JPEG, after the barrier when a frame covers it
			{
				int r = (int)(rnd() % rows), c = (int)(rnd() % cols), i = (r * cols) + c;
				m.ver[i]++;
				if (m.mask[i])
				{
					// the barrier: the frames in flight land first (in the order of the dispatch), then the JPEG
					while (nf > 0) { s = (int)fifo[0]; kvm_vtiles_land(&t, s, map); for (k = 0; k < n; k++) { if (m.cov[s][k]) { m.mask[k] = (uint8_t)(m.mask[k] & ~(1u << s)); m.canvas[k] = m.paint[s][k] > m.canvas[k] ? m.paint[s][k] : m.canvas[k]; } } m.used[s] = 0; memmove(fifo, fifo + 1, (nf - 1) * sizeof(fifo[0])); nf--; }
					CHECK(!kvm_vtiles_inflight(&t, r, c));
				}
				kvm_vtiles_jpeg(&t, r, c);
				m.base[i] = KVM_VT_NONE; m.canvas[i] = m.ver[i];
			}
			else if (ev <= 7)	// dispatch: a frame of the pending tiles into a free slot
			{
				int slot = -1;
				for (k = 0; k < depth; k++) { if (!m.used[k]) { slot = k; break; } }
				if (slot < 0 || t.nPending == 0) { continue; }
				k = kvm_vtiles_make_rects(&t, sw, sh, rects, cover);
				CHECK(k > 0);
				kvm_vtiles_dispatch(&t, cover, slot);
				m.used[slot] = 1; fifo[nf++] = (unsigned)slot;
				for (k = 0; k < n; k++)
				{
					m.cov[slot][k] = cover[k];
					if (cover[k]) { m.paint[slot][k] = m.ver[k]; m.base[k] = KVM_VT_VIDEO; m.mask[k] = (uint8_t)(m.mask[k] | (1u << slot)); }
				}
			}
			else if (ev <= 9)	// land: the oldest frame is written
			{
				if (nf == 0) { continue; }
				s = (int)fifo[0];
				{ unsigned want = 0; for (k = 0; k < n; k++) { if (m.cov[s][k]) { want++; } } CHECK_EQ(kvm_vtiles_land(&t, s, map), want); }
				for (k = 0; k < n; k++)
				{
					CHECK_EQ(map[k], m.cov[s][k]);
					if (m.cov[s][k]) { m.mask[k] = (uint8_t)(m.mask[k] & ~(1u << s)); if (m.paint[s][k] > m.canvas[k]) { m.canvas[k] = m.paint[s][k]; } }
				}
				m.used[s] = 0; memmove(fifo, fifo + 1, (nf - 1) * sizeof(fifo[0])); nf--;
			}
			else if (ev == 10)	// abort: the oldest access unit was lost, the generation goes on (an IDR follows): its tiles are painted again
			{
				if (nf == 0) { continue; }
				s = (int)fifo[0];
				(void)kvm_vtiles_abort(&t, s);
				for (k = 0; k < n; k++) { if (m.cov[s][k]) { m.mask[k] = (uint8_t)(m.mask[k] & ~(1u << s)); if (m.base[k] == KVM_VT_VIDEO) { m.base[k] = KVM_VT_PENDING; } } }
				m.used[s] = 0; memmove(fifo, fifo + 1, (nf - 1) * sizeof(fifo[0])); nf--;
			}
			else if (ev == 11)	// cancel: every frame is given up (stop, geometry, error); the tiles of the frames go back to JPEG, then the pending ones too (I7)
			{
				unsigned want = 0;
				if (nf == 0) { continue; }
				memset(map, 0, sizeof(map));
				for (k = 0; k < n; k++) { if (m.mask[k]) { want++; m.mask[k] = 0; m.base[k] = KVM_VT_NONE; m.canvas[k] = m.ver[k]; } }
				CHECK_EQ(kvm_vtiles_cancel(&t, -1, map), want);
				for (k = 0; k < n; k++) { if (map[k]) { CHECK(m.canvas[k] == m.ver[k]); } }
				memset(m.used, 0, sizeof(m.used)); nf = 0;
				{ unsigned p = 0; for (k = 0; k < n; k++) { if (m.base[k] == KVM_VT_PENDING) { p++; m.base[k] = KVM_VT_NONE; m.canvas[k] = m.ver[k]; } } CHECK_EQ(kvm_vtiles_take_pending(&t, map), p); }
			}
			else if (ev == 12 && nf == 0)	// repair / REFRESH after the barrier: the VIDEO tiles (and with all the pending ones) are JPEG
			{
				int all = (int)(rnd() & 1);
				unsigned want = 0;
				for (k = 0; k < n; k++) { if (m.base[k] == KVM_VT_VIDEO || (all && m.base[k] == KVM_VT_PENDING)) { want++; m.base[k] = KVM_VT_NONE; m.canvas[k] = m.ver[k]; } }
				CHECK_EQ(kvm_vtiles_take_video(&t, map, all), want);
			}
			else if (ev == 13 && nf == 0)	// take_pending alone
			{
				unsigned p = 0;
				for (k = 0; k < n; k++) { if (m.base[k] == KVM_VT_PENDING) { p++; m.base[k] = KVM_VT_NONE; m.canvas[k] = m.ver[k]; } }
				CHECK_EQ(kvm_vtiles_take_pending(&t, map), p);
			}
			{
				unsigned np = 0, nv = 0, ni = 0;
				for (k = 0; k < n; k++)
				{
					CHECK_EQ(kvm_vtiles_get(&t, k / cols, k % cols), m.base[k]);
					CHECK_EQ(kvm_vtiles_inflight(&t, k / cols, k % cols), m.mask[k] != 0);
					if (m.base[k] == KVM_VT_PENDING) { np++; } else if (m.base[k] == KVM_VT_VIDEO) { nv++; }
					if (m.mask[k]) { ni++; }
					CHECK(t.fl[k] == m.mask[k]);
				}
				CHECK_EQ(t.nPending, np); CHECK_EQ(t.nVideo, nv); CHECK_EQ(t.nInflight, ni);
			}
			if (gFail > 20) { return; }
		}
		// settle: everything pending is painted, every frame lands; the canvas has the latest version of every tile (no loss) and no version ever went back (checked at every paint above)
		for (;;)
		{
			int slot = -1;
			while (nf > 0)
			{
				s = (int)fifo[0];
				kvm_vtiles_land(&t, s, map);
				for (k = 0; k < n; k++) { if (m.cov[s][k]) { m.mask[k] = (uint8_t)(m.mask[k] & ~(1u << s)); if (m.paint[s][k] > m.canvas[k]) { m.canvas[k] = m.paint[s][k]; } } }
				m.used[s] = 0; memmove(fifo, fifo + 1, (nf - 1) * sizeof(fifo[0])); nf--;
			}
			if (t.nPending == 0) { break; }
			for (k = 0; k < depth; k++) { if (!m.used[k]) { slot = k; break; } }
			CHECK(slot >= 0);
			CHECK(kvm_vtiles_make_rects(&t, sw, sh, rects, cover) > 0);
			kvm_vtiles_dispatch(&t, cover, slot);
			m.used[slot] = 1; fifo[nf++] = (unsigned)slot;
			for (k = 0; k < n; k++) { m.cov[slot][k] = cover[k]; if (cover[k]) { m.paint[slot][k] = m.ver[k]; m.base[k] = KVM_VT_VIDEO; m.mask[k] = (uint8_t)(m.mask[k] | (1u << slot)); } }
		}
		for (k = 0; k < n; k++) { CHECK_EQ(m.canvas[k], m.ver[k]); }
		CHECK_EQ(t.nInflight, 0);
		CHECK_EQ(t.jpegOnInflight, 0);
		kvm_vtiles_free(&t);
	}
}

// as the module sees it: a JPEG of a tile that a frame covers is counted
static void test_tiles_inflight_rules(void)
{
	kvm_vtiles t;
	uint8_t cover[6 * 6], map[6 * 6];
	uint16_t rects[KVM_FRAME_MAX_RECTS * 4];
	CHECK_EQ(kvm_vtiles_init(&t, 6, 6), 0);
	kvm_vtiles_claim(&t, 1, 1, 2, 2);
	CHECK_EQ(kvm_vtiles_make_rects(&t, 192, 192, rects, cover), 1);
	kvm_vtiles_dispatch(&t, cover, 0);
	CHECK_EQ(t.nPending, 0); CHECK_EQ(t.nVideo, 4); CHECK_EQ(t.nInflight, 4);
	CHECK(kvm_vtiles_inflight(&t, 1, 1)); CHECK(!kvm_vtiles_inflight(&t, 0, 0));
	CHECK(kvm_vtiles_rect_inflight(&t, 0, 0, 1, 1)); CHECK(!kvm_vtiles_rect_inflight(&t, 3, 3, 5, 5));
	CHECK(kvm_vtiles_rect_inflight(&t, 1, 1, 1, 1)); CHECK(kvm_vtiles_rect_inflight(&t, 2, 0, 2, 5)); CHECK(!kvm_vtiles_rect_inflight(&t, 0, 0, 0, 5));
	kvm_vtiles_claim(&t, 2, 2, 2, 2);							// FP: changed again while its frame is in flight
	CHECK_EQ(t.nPending, 1); CHECK_EQ(t.nVideo, 3); CHECK_EQ(t.nInflight, 4);
	CHECK_EQ(kvm_vtiles_take_pending(&t, map), 0);			// the tile with a frame in flight is left alone
	CHECK_EQ(kvm_vtiles_take_video(&t, map, 1), 0);
	CHECK_EQ(kvm_vtiles_land(&t, 0, map), 4);
	CHECK_EQ(kvm_vtiles_get(&t, 2, 2), KVM_VT_PENDING);		// the new change waits for the next frame
	CHECK_EQ(kvm_vtiles_get(&t, 1, 1), KVM_VT_VIDEO);
	CHECK_EQ(t.nInflight, 0);
	kvm_vtiles_jpeg(&t, 1, 1); CHECK_EQ(t.jpegOnInflight, 0);
	kvm_vtiles_dispatch(&t, cover, 1);
	kvm_vtiles_jpeg(&t, 1, 1);
	CHECK_EQ(t.jpegOnInflight, 1);
	CHECK_EQ(kvm_vtiles_abort(&t, 1), 4);
	CHECK_EQ(kvm_vtiles_get(&t, 2, 1), KVM_VT_PENDING);		// a lost frame: its tiles are painted again
	CHECK_EQ(t.nInflight, 0);
	// two frames in flight: cancel one slot, then every slot
	kvm_vtiles_claim(&t, 0, 0, 0, 1);
	CHECK(kvm_vtiles_make_rects(&t, 192, 192, rects, cover) > 0);
	kvm_vtiles_dispatch(&t, cover, 0);
	kvm_vtiles_claim(&t, 4, 4, 5, 5);
	CHECK(kvm_vtiles_make_rects(&t, 192, 192, rects, cover) > 0);
	kvm_vtiles_dispatch(&t, cover, 1);
	memset(map, 0, sizeof(map));
	CHECK_EQ(kvm_vtiles_cancel(&t, 1, map), 4);
	CHECK(map[(4 * 6) + 4] && !map[0]);
	CHECK(kvm_vtiles_inflight(&t, 0, 0) && !kvm_vtiles_inflight(&t, 4, 4));
	CHECK_EQ(kvm_vtiles_get(&t, 4, 4), KVM_VT_NONE);
	{ const unsigned nIn = t.nInflight; CHECK_EQ(kvm_vtiles_cancel(&t, -1, map), nIn); }
	CHECK_EQ(t.nInflight, 0);
	kvm_vtiles_free(&t);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// levels, profiles, quantizer, IDR spacing
// ---------------------------------------------------------------------------------------------------------------------------------------

static void test_levels(void)
{
	int lv, fps;
	// 1920x1088: 8160 macroblocks, 489 600 per second at 60 frames per second: level 4.2 (5.2 of the specification)
	CHECK_EQ(kvm_vcore_pick_level(1920, 1088, 0, &lv, &fps), 0); CHECK_EQ(lv, 42); CHECK_EQ(fps, 60);
	CHECK_EQ(kvm_vcore_pick_level(1920, 1088, 51, &lv, &fps), 0); CHECK_EQ(lv, 42); CHECK_EQ(fps, 60);
	CHECK_EQ(kvm_vcore_pick_level(1920, 1088, 42, &lv, &fps), 0); CHECK_EQ(lv, 42); CHECK_EQ(fps, 60);
	// the viewer takes 4.1 only: level 4.0 and 30 frames per second (245 760 / 8160 = 30)
	CHECK_EQ(kvm_vcore_pick_level(1920, 1088, 41, &lv, &fps), 0); CHECK_EQ(lv, 40); CHECK_EQ(fps, 30);
	CHECK_EQ(kvm_vcore_pick_level(1920, 1088, 40, &lv, &fps), 0); CHECK_EQ(lv, 40); CHECK_EQ(fps, 30);
	CHECK_EQ(kvm_vcore_pick_level(1920, 1088, 32, &lv, &fps), -1);	// reason 7
	CHECK_EQ(kvm_vcore_pick_level(1280, 736, 0, &lv, &fps), 0); CHECK_EQ(lv, 40); CHECK_EQ(fps, 60);
	CHECK_EQ(kvm_vcore_pick_level(2560, 1440, 0, &lv, &fps), 0); CHECK_EQ(lv, 51); CHECK_EQ(fps, 60);
	CHECK_EQ(kvm_vcore_pick_level(2560, 1440, 50, &lv, &fps), 0); CHECK_EQ(lv, 50); CHECK_EQ(fps, 40);	// 589 824 / 14 400 = 40
	CHECK_EQ(kvm_vcore_pick_level(3840, 2176, 0, &lv, &fps), 0); CHECK_EQ(lv, 52); CHECK_EQ(fps, 60);
	// 4096x2304 = 36 864 macroblocks: not at 60 frames per second, 56 at level 5.2
	CHECK_EQ(kvm_vcore_pick_level(4096, 2304, 0, &lv, &fps), 0); CHECK_EQ(lv, 52); CHECK_EQ(fps, 56);
	CHECK_EQ(kvm_vcore_pick_level(4096, 2304, 51, &lv, &fps), -1);
	CHECK_EQ(kvm_vcore_pick_level(5120, 2880, 0, &lv, &fps), -1);	// 57 600 macroblocks: more than level 5.2
	CHECK_EQ(kvm_vcore_pick_level(8192, 8192, 0, &lv, &fps), -1);
	CHECK_EQ(kvm_vcore_pick_level(8224, 64, 0, &lv, &fps), -1);	// above the protocol's 8192
	CHECK_EQ(kvm_vcore_pick_level(0, 0, 0, &lv, &fps), -1);
	CHECK_EQ(kvm_vcore_pick_level(32, 32, 0, &lv, &fps), 0); CHECK_EQ(lv, 30); CHECK_EQ(fps, 60);
	// profiles
	CHECK_EQ(kvm_vcore_pick_profile(7, 101), 100);
	CHECK_EQ(kvm_vcore_pick_profile(3, 101), 77);
	CHECK_EQ(kvm_vcore_pick_profile(1, 101), 66);
	CHECK_EQ(kvm_vcore_pick_profile(0, 101), 0);
	CHECK_EQ(kvm_vcore_pick_profile(7, 100), 77);	// the encoder refused High: the next one down
	CHECK_EQ(kvm_vcore_pick_profile(7, 77), 66);
	CHECK_EQ(kvm_vcore_pick_profile(7, 66), 0);
	CHECK_EQ(kvm_vcore_pick_profile(4, 100), 0);	// only High allowed and it was refused
	CHECK_EQ(kvm_vcore_pick_profile(5, 100), 66);	// High and CBP allowed, Main not
	// quantizer: fixed 28, never below the floor 26
	CHECK(KVM_VIDEO_QP >= KVM_VIDEO_QP_MIN);
	CHECK_EQ(kvm_vcore_clamp_qp(KVM_VIDEO_QP), 28);
	CHECK_EQ(kvm_vcore_clamp_qp(20), 26); CHECK_EQ(kvm_vcore_clamp_qp(26), 26); CHECK_EQ(kvm_vcore_clamp_qp(27), 27); CHECK_EQ(kvm_vcore_clamp_qp(39), 38); CHECK_EQ(kvm_vcore_clamp_qp(45), 38);
}

static void test_idr_spacing(void)
{
	CHECK_EQ(kvm_vcore_idr_spacing(200000, 0), 1000);				// not measured: 1 s
	CHECK_EQ(kvm_vcore_idr_spacing(0, 100000), 1000);
	CHECK_EQ(kvm_vcore_idr_spacing(200000, 1000000), 1000);			// 2 * 200 KB / 1 MB/s = 0.4 s: the 1 s floor
	CHECK_EQ(kvm_vcore_idr_spacing(500000, 1000000), 1000);			// exactly 1 s
	CHECK_EQ(kvm_vcore_idr_spacing(500001, 1000000), 1000);			// 1000.002 ms, integer division
	CHECK_EQ(kvm_vcore_idr_spacing(600000, 1000000), 1200);
	CHECK_EQ(kvm_vcore_idr_spacing(250000, 125000), 4000);			// 1 Mb/s link, an IDR of 250 KB: 4 s
	CHECK_EQ(kvm_vcore_idr_spacing(250000, 12500), 10000);			// 100 kb/s: capped
	CHECK_EQ(kvm_vcore_idr_spacing(0xFFFFFFFFu, 1), 10000);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the controller
// ---------------------------------------------------------------------------------------------------------------------------------------

static kvm_ctrl op1(unsigned flags, unsigned maxLevel)
{
	kvm_ctrl c;
	memset(&c, 0, sizeof(c));
	c.ver = 1; c.op = KVM_OP_VIDEO_ON; c.flags = (uint8_t)flags; c.maxLevelIdc = (uint8_t)maxLevel; c.maxW = 1920; c.maxH = 1200;
	return c;
}

static void test_op1_open_error(uint32_t t0)
{
	kvm_vctl c;
	kvm_ctrl a = op1(7, 51), b = op1(1, 40);
	int i;
	kvm_vctl_init(&c, 1);
	CHECK_EQ(kvm_vctl_op1(&c, &a, t0), KVM_VOP1_START);
	CHECK_EQ(c.profFlags, 7); CHECK_EQ(c.maxLevel, 51); CHECK_EQ(c.maxW, 1920);
	// the start has failed (state OFF): the next op 1 is a new attempt, not joined
	CHECK_EQ(kvm_vctl_op1(&c, &b, t0 + 100), KVM_VOP1_START);
	CHECK_EQ(c.profFlags, 1);
	// the start is under way or has succeeded (not OFF): an op 1 within 2 s is joined, but its limits are remembered for the next start
	c.state = KVM_VS_ON;
	CHECK_EQ(kvm_vctl_op1(&c, &a, t0 + 1999 + 100), KVM_VOP1_COALESCE);
	CHECK_EQ(c.profFlags, 1); CHECK_EQ(c.maxLevel, 40);					// the limits in force stay: the generation that runs was checked against them ...
	CHECK_EQ(c.nxValid, 1); CHECK_EQ(c.nxFlags, 7); CHECK_EQ(c.nxLevel, 51);	// ... the new ones are kept for the next start
	kvm_vctl_apply_limits(&c);
	CHECK_EQ(c.profFlags, 7); CHECK_EQ(c.maxLevel, 51); CHECK_EQ(c.nxValid, 0);
	kvm_vctl_apply_limits(&c); CHECK_EQ(c.profFlags, 7);				// nothing waiting: nothing changes
	CHECK_EQ(kvm_vctl_op1(&c, &b, t0 + 2100), KVM_VOP1_START);		// 2 s after the accepted one
	CHECK_EQ(c.profFlags, 1); CHECK_EQ(c.maxLevel, 40);
	c.state = KVM_VS_WAIT;
	CHECK_EQ(kvm_vctl_op1(&c, &a, t0 + 2200), KVM_VOP1_COALESCE);
	kvm_vctl_limits(&c, &b);								// the op 1 of the hold-off after an error
	CHECK_EQ(c.profFlags, 1); CHECK_EQ(c.maxLevel, 40);
	CHECK_EQ(c.coalesced, 2);
	// the encoder is created at most once in 2 s
	CHECK(kvm_vctl_open_allowed(&c, t0));
	kvm_vctl_open_note(&c, t0);
	CHECK(!kvm_vctl_open_allowed(&c, t0 + 1999));
	CHECK(kvm_vctl_open_allowed(&c, t0 + 2000));
	// errors: the video is off for 60 s, the third error is for good
	kvm_vctl_init(&c, 1);
	CHECK_EQ(kvm_vctl_error(&c, t0), 1);
	CHECK(!kvm_vctl_retry_due(&c, t0 + 59999));
	CHECK(kvm_vctl_retry_due(&c, t0 + 60000));
	CHECK(!kvm_vctl_retry_due(&c, t0 + 60001));			// once
	CHECK_EQ(kvm_vctl_error(&c, t0 + 70000), 1);			// the second
	CHECK(!kvm_vctl_retry_due(&c, t0 + 129999));
	CHECK(kvm_vctl_retry_due(&c, t0 + 130000));
	CHECK_EQ(kvm_vctl_error(&c, t0 + 140000), 0);			// the third: for good
	CHECK_EQ(c.capVideo, 0);
	CHECK(!kvm_vctl_retry_due(&c, t0 + 10000000));
	// a structured exception: for good at once
	kvm_vctl_init(&c, 1);
	kvm_vctl_fault(&c);
	CHECK_EQ(c.capVideo, 0); CHECK_EQ(c.faulted, 1);
	CHECK(!kvm_vctl_retry_due(&c, t0 + 10000000));
	// time wrap
	kvm_vctl_init(&c, 1);
	CHECK_EQ(kvm_vctl_error(&c, 0xFFFFFFFFu - 100), 1);
	CHECK(!kvm_vctl_retry_due(&c, 0xFFFFFFFFu));
	CHECK(kvm_vctl_retry_due(&c, 0xFFFFFFFFu - 100 + 60000u));
	for (i = 0; i < 3; i++) { kvm_vctl_init(&c, 0); CHECK_EQ(c.capVideo, 0); }
}

// a stream in the state "the viewer asked for video": ACK on
static void make_stream(kvm_stream *s, uint32_t now)
{
	kvm_ctrl c = op1(7, 0);
	kvm_state_msg r;
	unsigned act = 0;
	kvm_stream_init(s, 1, 1, KVM_REASON_NO_VIDEO_BUILD, KVM_REASON_DISABLED);
	CHECK_EQ(kvm_stream_on_ctrl(s, &c, now, &r, &act), 0);	// no reply: the video code answers
	CHECK(act & KVM_ACT_VIDEO_REQUEST);
	CHECK_EQ(s->want, KVM_WANT_VIDEO); CHECK_EQ(s->ackActive, 1);
}

static void test_begin_gen(uint32_t t0)
{
	kvm_vctl c;
	kvm_stream s;
	kvm_state_msg m;
	kvm_vctl_init(&c, 1);
	make_stream(&s, t0);
	CHECK_EQ(s.gen, 0);
	CHECK_EQ(kvm_vctl_begin_gen(&c, &s, 100, 42, 1920, 1088, 1, &m), 0);
	CHECK_EQ(s.gen, 1); CHECK_EQ(s.videoOn, 1); CHECK_EQ(c.state, KVM_VS_ON); CHECK_EQ(c.idrFirst, 1);
	CHECK_EQ(m.ver, 1); CHECK_EQ(m.video, 1); CHECK_EQ(m.ack, 1); CHECK_EQ(m.reason, KVM_REASON_OK); CHECK_EQ(m.profile, 100); CHECK_EQ(m.level, 42);
	CHECK_EQ(m.codedW, 1920); CHECK_EQ(m.codedH, 1088); CHECK_EQ(m.caps, KVM_CAPS_H264 | KVM_CAPS_ACK); CHECK_EQ(m.gen, 1);
	{ uint8_t b[16]; kvm_state_msg back; kvm_stream_build_state(b, &m); CHECK_EQ(kvm_stream_parse_state(b, 16, &back), KVM_REJ_NONE); CHECK_EQ(back.gen, 1); }
	// every generation: gen + 1 (wraps at 256)
	{
		int i;
		for (i = 0; i < 300; i++) { CHECK_EQ(kvm_vctl_begin_gen(&c, &s, 77, 40, 1280, 736, 0, &m), 0); CHECK_EQ(m.gen, (uint8_t)(s.gen)); }
		CHECK_EQ(s.gen, (uint8_t)(1 + 300));
	}
	// without the ACK window (veto, timeout) there is no video: nothing changes
	{
		kvm_stream s2;
		uint8_t g;
		kvm_ctrl veto;
		kvm_state_msg r;
		make_stream(&s2, t0);
		memset(&veto, 0, sizeof(veto));
		veto.ver = 1; veto.op = KVM_OP_VETO; veto.flags = 1;
		(void)kvm_stream_on_ctrl(&s2, &veto, t0 + 1, &r, NULL);
		CHECK_EQ(s2.ackActive, 0);
		g = s2.gen;
		kvm_vctl_stop(&c);
		CHECK_EQ(kvm_vctl_begin_gen(&c, &s2, 100, 42, 1920, 1088, 1, &m), -1);
		CHECK_EQ(s2.gen, g); CHECK_EQ(s2.videoOn, 0); CHECK_EQ(c.state, KVM_VS_OFF);
	}
	// a build or a process without video
	kvm_vctl_init(&c, 0);
	make_stream(&s, t0);
	CHECK_EQ(kvm_vctl_begin_gen(&c, &s, 100, 42, 1920, 1088, 1, &m), -1);
}

static kvm_vplan plan(kvm_vctl *c, const kvm_stream *s, uint32_t now, unsigned pending, int gate, uint32_t interval, uint64_t rate)
{
	kvm_vplan_in in;
	kvm_vplan p;
	in.now = now; in.pending = pending; in.gateOpen = gate; in.rateOpen = 1; in.intervalMs = interval; in.rate = rate;
	kvm_vctl_plan(c, s, &in, &p);
	return p;
}

static void test_plan_and_idr(uint32_t t0)
{
	kvm_vctl c;
	kvm_stream s;
	kvm_state_msg m;
	kvm_vplan p;
	uint32_t t = t0;
	kvm_vctl_init(&c, 1);
	make_stream(&s, t);
	// nothing started: nothing planned
	p = plan(&c, &s, t, 5, 1, 16, 0); CHECK_EQ(p.encode, 0);
	(void)kvm_vctl_begin_gen(&c, &s, 100, 42, 1920, 1088, 1, &m);
	// lazy: without motion nothing is encoded, also with a key frame wanted
	c.idrReq = 1;
	p = plan(&c, &s, t, 0, 1, 16, 0); CHECK_EQ(p.encode, 0); CHECK_EQ(p.held, 0);
	c.idrReq = 0;
	// the window: a closed window holds the frame back (the glue notes it)
	p = plan(&c, &s, t, 5, 0, 16, 0); CHECK_EQ(p.encode, 0); CHECK_EQ(p.held, 1);
	// the byte bucket of the rate control: an empty one holds a frame back (it comes later with what accumulated), but not the first frame of a generation
	{
		kvm_vplan_in in;
		in.now = t; in.pending = 5; in.gateOpen = 1; in.rateOpen = 0; in.intervalMs = 16; in.rate = 0;
		kvm_vctl_plan(&c, &s, &in, &p); CHECK_EQ(p.encode, 1);				// idrFirst: never refused by the bucket
		c.idrFirst = 0;
		kvm_vctl_plan(&c, &s, &in, &p); CHECK_EQ(p.encode, 0); CHECK_EQ(p.heldRate, 1); CHECK_EQ(p.held, 0);
		c.idrForce = 1; kvm_vctl_plan(&c, &s, &in, &p); CHECK_EQ(p.encode, 1); c.idrForce = 0;	// a key frame after a lost access unit neither
		c.idrFirst = 1;
	}
	// the first frame of a fresh encoder is an IDR by itself; of a reused encoder it is forced
	p = plan(&c, &s, t, 5, 1, 16, 0); CHECK_EQ(p.encode, 1); CHECK_EQ(p.forceIdr, 0);
	c.encFresh = 0;
	p = plan(&c, &s, t, 5, 1, 16, 0); CHECK_EQ(p.encode, 1); CHECK_EQ(p.forceIdr, 1);
	// the first frame is not P: it is not sent, an IDR is forced at once (not delayed)
	CHECK_EQ(kvm_vctl_frame_done(&c, 0, 1000, t), KVM_VF_BROKEN);
	CHECK_EQ(c.idrFirst, 1); CHECK_EQ(c.frames, 0);
	p = plan(&c, &s, t, 5, 1, 16, 0); CHECK_EQ(p.forceIdr, 1);
	CHECK_EQ(kvm_vctl_frame_done(&c, 1, 150000, t), 0);
	CHECK_EQ(c.idrFirst, 0); CHECK_EQ(c.idrForce, 0); CHECK_EQ(c.haveIdr, 1); CHECK_EQ(c.lastIdrBytes, 150000); CHECK_EQ(c.frames, 1);
	// P frames follow; the pace
	p = plan(&c, &s, t0 + 1, 5, 1, 33, 0); CHECK_EQ(p.encode, 0);			// 1 ms after the last frame, at 30 frames per second
	p = plan(&c, &s, t0 + 24, 5, 1, 33, 0); CHECK_EQ(p.encode, 0);			// the frame may come 25 % early: 33 - 8 = 25 ms
	p = plan(&c, &s, t0 + 25, 5, 1, 33, 0); CHECK_EQ(p.encode, 1);
	t = t0 + 40;
	p = plan(&c, &s, t, 5, 1, 33, 0); CHECK_EQ(p.encode, 1); CHECK_EQ(p.forceIdr, 0);
	CHECK_EQ(kvm_vctl_frame_done(&c, 0, 3000, t), 0);
	// op 3 with the IDR flag: after the spacing (1 s, or twice the IDR time on the link)
	kvm_vctl_op3(&c, KVM_ACT_IDR);
	t = t0 + 900;
	p = plan(&c, &s, t, 5, 1, 16, 0); CHECK_EQ(p.encode, 1); CHECK_EQ(p.forceIdr, 0);	// the IDR was at t0: 900 ms ago
	CHECK_EQ(kvm_vctl_frame_done(&c, 0, 3000, t), 0);
	t = t0 + 1000;
	p = plan(&c, &s, t, 5, 1, 16, 0); CHECK_EQ(p.encode, 1); CHECK_EQ(p.forceIdr, 1);
	// ... and with a slow link the spacing is longer: 150 000 bytes at 50 000 B/s = 6 s
	p = plan(&c, &s, t, 5, 1, 16, 50000); CHECK_EQ(p.forceIdr, 0);
	p = plan(&c, &s, t0 + 5999, 5, 1, 16, 50000); CHECK_EQ(p.forceIdr, 0);
	p = plan(&c, &s, t0 + 6000, 5, 1, 16, 50000); CHECK_EQ(p.forceIdr, 1);
	// many requests are one; the IDR serves all of them
	kvm_vctl_op3(&c, KVM_ACT_IDR); kvm_vctl_op3(&c, KVM_ACT_IDR);
	CHECK_EQ(kvm_vctl_frame_done(&c, 1, 160000, t0 + 6000), 0);
	CHECK_EQ(c.idrReq, 0); CHECK_EQ(c.idrs, 2);
	p = plan(&c, &s, t0 + 6100, 5, 1, 16, 50000); CHECK_EQ(p.forceIdr, 0);
	// a lost access unit: the next frame is an IDR at once, even right after the last one; the third in a row (or an oversize one) is an error
	CHECK_EQ(kvm_vctl_frame_broken(&c, 0), KVM_VB_RETRY);
	p = plan(&c, &s, t0 + 6101, 5, 1, 16, 50000); CHECK_EQ(p.forceIdr, 1);
	CHECK_EQ(kvm_vctl_frame_broken(&c, 0), KVM_VB_RETRY);
	CHECK_EQ(kvm_vctl_frame_broken(&c, 0), KVM_VB_ERROR);
	CHECK_EQ(kvm_vctl_frame_done(&c, 1, 100000, t0 + 6200), 0);	// a good frame ends the run
	CHECK_EQ(c.brokenRun, 0);
	CHECK_EQ(kvm_vctl_frame_broken(&c, 0), KVM_VB_RETRY);
	CHECK_EQ(kvm_vctl_frame_broken(&c, 1), KVM_VB_ERROR);
	// the first frame of a generation that is not an IDR, three times in a row, is an error of the encoder
	(void)kvm_vctl_begin_gen(&c, &s, 100, 42, 1920, 1088, 0, &m);
	CHECK_EQ(kvm_vctl_frame_done(&c, 0, 10, t0 + 7000), KVM_VF_BROKEN);
	CHECK_EQ(kvm_vctl_frame_done(&c, 0, 10, t0 + 7001), KVM_VF_BROKEN);
	CHECK_EQ(kvm_vctl_frame_done(&c, 0, 10, t0 + 7002), KVM_VF_ERROR);
	// when the stream layer says that the video is off nothing is planned
	kvm_vctl_stop(&c);
	p = plan(&c, &s, t0 + 8000, 5, 1, 16, 0); CHECK_EQ(p.encode, 0);
	(void)kvm_vctl_begin_gen(&c, &s, 100, 42, 1920, 1088, 1, &m);
	s.videoOn = 0;
	p = plan(&c, &s, t0 + 8000, 5, 1, 16, 0); CHECK_EQ(p.encode, 0);
}

static void test_repair_rate(uint32_t t0)
{
	kvm_vctl c;
	kvm_vctl_init(&c, 1);
	CHECK(!kvm_vctl_repair_due(&c, t0));			// nothing asked for
	kvm_vctl_op3(&c, KVM_ACT_REPAIR);
	CHECK(kvm_vctl_repair_due(&c, t0));				// the first one at once
	kvm_vctl_op3(&c, KVM_ACT_REPAIR); kvm_vctl_op3(&c, KVM_ACT_REPAIR); kvm_vctl_op3(&c, KVM_ACT_REPAIR);
	CHECK(!kvm_vctl_repair_due(&c, t0 + 999));
	CHECK(kvm_vctl_repair_due(&c, t0 + 1000));		// the requests of the window are one, done at its end
	CHECK(!kvm_vctl_repair_due(&c, t0 + 5000));		// and that was all
	CHECK_EQ(c.repairs, 2);
	// op 1 repairs at once and starts the second
	kvm_vctl_repair_now(&c, t0 + 5000);
	kvm_vctl_op3(&c, KVM_ACT_REPAIR);
	CHECK(!kvm_vctl_repair_due(&c, t0 + 5500));
	CHECK(kvm_vctl_repair_due(&c, t0 + 6000));
	// IDR and repair are separate flags
	kvm_vctl_op3(&c, KVM_ACT_IDR);
	CHECK_EQ(c.idrReq, 1); CHECK_EQ(c.repairReq, 0);
}


static void test_sps_check(void)
{
	struct kvm_sps_info s;
	memset(&s, 0, sizeof(s));
	s.profile = 100; s.constraint = 0; s.level = 42; s.numRefFrames = 1; s.mbsW = 90; s.mbsH = 52; s.frameMbsOnly = 1; s.chromaFormat = 1; s.bitDepthLuma = 8;
	CHECK_EQ(kvm_vcore_check_sps(&s, 7, 0, 1440, 832), 0);
	CHECK_EQ(kvm_vcore_check_sps(&s, 4, 51, 1440, 832), 0);
	CHECK_EQ(kvm_vcore_check_sps(&s, 3, 0, 1440, 832), KVM_REASON_PROFILE);		// the viewer did not accept High
	CHECK_EQ(kvm_vcore_check_sps(&s, 7, 41, 1440, 832), KVM_REASON_RESOLUTION);	// level 4.2 above the viewer's 4.1
	CHECK_EQ(kvm_vcore_check_sps(&s, 7, 42, 1440, 832), 0);
	s.numRefFrames = 2; CHECK_EQ(kvm_vcore_check_sps(&s, 7, 0, 1440, 832), KVM_REASON_PROFILE); s.numRefFrames = 0; CHECK_EQ(kvm_vcore_check_sps(&s, 7, 0, 1440, 832), 0); s.numRefFrames = 1;
	s.cropping = 1; CHECK_EQ(kvm_vcore_check_sps(&s, 7, 0, 1440, 832), KVM_REASON_RESOLUTION); s.cropping = 0;
	CHECK_EQ(kvm_vcore_check_sps(&s, 7, 0, 1440, 800), KVM_REASON_RESOLUTION);	// not the coded size
	s.chromaFormat = 3; CHECK_EQ(kvm_vcore_check_sps(&s, 7, 0, 1440, 832), KVM_REASON_PROFILE); s.chromaFormat = 1;
	s.bitDepthLuma = 10; CHECK_EQ(kvm_vcore_check_sps(&s, 7, 0, 1440, 832), KVM_REASON_PROFILE); s.bitDepthLuma = 8;
	s.frameMbsOnly = 0; CHECK_EQ(kvm_vcore_check_sps(&s, 7, 0, 1440, 832), KVM_REASON_PROFILE); s.frameMbsOnly = 1;
	s.mbsW = 256; s.mbsH = 145; CHECK_EQ(kvm_vcore_check_sps(&s, 7, 0, 4096, 2320), KVM_REASON_RESOLUTION);	// 37 120 macroblocks
	s.mbsW = 90; s.mbsH = 52;
	s.profile = 66; s.constraint = 0xC0; s.level = 42; CHECK_EQ(kvm_vcore_check_sps(&s, 1, 0, 1440, 832), 0);
	s.constraint = 0x80; CHECK_EQ(kvm_vcore_check_sps(&s, 1, 0, 1440, 832), KVM_REASON_PROFILE);	// Baseline without constraint_set1
	s.constraint = 0xC0; CHECK_EQ(kvm_vcore_check_sps(&s, 6, 0, 1440, 832), KVM_REASON_PROFILE);	// viewer did not accept CBP
	s.profile = 77; s.constraint = 0; CHECK_EQ(kvm_vcore_check_sps(&s, 2, 0, 1440, 832), 0);
	s.profile = 88; CHECK_EQ(kvm_vcore_check_sps(&s, 7, 0, 1440, 832), KVM_REASON_PROFILE);		// Extended: not in the protocol
	s.profile = 100; s.level = 53; CHECK_EQ(kvm_vcore_check_sps(&s, 7, 0, 1440, 832), KVM_REASON_RESOLUTION);
	// the bridge: SPS bytes from the parser (the encoder's real SPS of the smoke) through the check
	{
		static const uint8_t cbp[] = { 0, 0, 0, 1, 0x67, 0x42, 0xc0, 0x2a, 0x95, 0xa0, 0x16, 0x81, 0xa6, 0xc0, 0x44, 0x00, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0xf0, 0x36, 0x82, 0x21, 0x1a, 0x80 };
		struct kvm_sps_info p;
		CHECK_EQ(kvm_annexb_sps_info(cbp, sizeof(cbp), &p), 0);
		CHECK_EQ(kvm_vcore_check_sps(&p, 1, 0, 1440, 832), 0);
		CHECK_EQ(kvm_vcore_check_sps(&p, 6, 0, 1440, 832), KVM_REASON_PROFILE);
		CHECK_EQ(kvm_vcore_check_sps(&p, 1, 40, 1440, 832), KVM_REASON_RESOLUTION);
	}
}

// Forced sharpening at 0.5 / 1 / 3 Mb/s over 60 s: the long run is the share (20 % of the rate) and not more; and a full window stops it
static void test_force_sharpen(void)
{
	static const uint64_t rates[] = { 62500, 125000, 375000, 12500000 };
	size_t k;
	for (k = 0; k < sizeof(rates) / sizeof(rates[0]); k++)
	{
		uint32_t t, last = 0, bytes = 0;
		uint64_t sum = 0;
		int have = 0;
		unsigned n = 0;
		for (t = 1000; t < 61000; t++)
		{
			if (kvm_vcore_force_sharpen(rates[k], 0, 0, 100000, have, last, t, &bytes)) { sum += bytes; last = t; have = 1; n++; CHECK(bytes >= KVM_VIDEO_SHARPEN_FORCE_MIN && bytes <= KVM_VIDEO_SHARPEN_FORCE_MAX); }
		}
		// the share of the rate: at most 20 % (+ one packet), and not far below (the starvation is really relieved)
		CHECK(sum * 100u <= rates[k] * 60u * 20u + (uint64_t)KVM_VIDEO_SHARPEN_FORCE_MAX * 100u);
		if (rates[k] <= 375000) { CHECK(sum * 100u >= rates[k] * 60u * 15u); }	// (a fast link: the packet limit is the limit)
		CHECK(n >= 10);
		printf("forced sharpening at %llu kb/s: %u packets, %llu B in 60 s = %.1f %% of the rate\n", (unsigned long long)(rates[k] * 8 / 1000), n, (unsigned long long)sum, 100.0 * (double)sum / ((double)rates[k] * 60.0));
	}
	// not while the window is more than twice full
	{
		uint32_t b = 0;
		CHECK_EQ(kvm_vcore_force_sharpen(125000, 0, 200000, 100000, 0, 0, 5000, &b), 0);
		CHECK_EQ(kvm_vcore_force_sharpen(125000, 0, 199999, 100000, 0, 0, 5000, &b), 1);
		CHECK_EQ(kvm_vcore_force_sharpen(125000, 0, 5000000, 0, 0, 0, 5000, &b), 1);	// window not known: not blocked
	}
	// the rate is not known: the smallest packet once a second
	{
		uint32_t b = 0;
		CHECK_EQ(kvm_vcore_force_sharpen(0, 0, 0, 100000, 0, 0, 5000, &b), 1); CHECK_EQ(b, KVM_VIDEO_SHARPEN_FORCE_MIN);
		CHECK_EQ(kvm_vcore_force_sharpen(0, 0, 0, 100000, 1, 5000, 5999, &b), 0);
		CHECK_EQ(kvm_vcore_force_sharpen(0, 0, 0, 100000, 1, 5000, 6000, &b), 1);
	}
	// the share is taken of min(rate, budget): a 50 Mb/s link with a 1 Mb/s budget forces like a 1 Mb/s link
	{
		uint32_t b1 = 0, b2 = 0, t, i1 = 0, i2 = 0;
		CHECK_EQ(kvm_vcore_force_sharpen(6250000, 0, 0, 100000, 0, 0, 5000, &b1), 1); CHECK_EQ(b1, KVM_VIDEO_SHARPEN_FORCE_MAX);	// 20 % of 6 250 000 B/s in 250 ms: far above the maximum
		CHECK_EQ(kvm_vcore_force_sharpen(6250000, 1000, 0, 100000, 0, 0, 5000, &b2), 1); CHECK_EQ(b2, 6250);					// 20 % of 125 000 B/s in 250 ms
		// a budget of 300 kb/s (37 500 B/s): 20 % is 7 500 B/s, the packet is the minimum and comes less often
		for (t = 1; t < 20000 && i1 == 0; t++) { if (kvm_vcore_force_sharpen(6250000, 0, 0, 100000, 1, 0, t, &b1)) { i1 = t; } }
		for (t = 1; t < 20000 && i2 == 0; t++) { if (kvm_vcore_force_sharpen(6250000, 300, 0, 100000, 1, 0, t, &b2)) { i2 = t; } }
		CHECK(i1 > 0 && i2 > 0);
		CHECK(i2 > 2 * i1);
		CHECK_EQ(b2, KVM_VIDEO_SHARPEN_FORCE_MIN);
		CHECK_EQ(kvm_vcore_force_sharpen(6250000, 300, 0, 100000, 1, 0, i2 - 1, &b2), 0);
		// a budget above the rate changes nothing
		for (t = 1; t < 20000; t++) { if (kvm_vcore_force_sharpen(125000, 1000000, 0, 100000, 1, 0, t, &b1)) { break; } }
		for (i1 = 1; i1 < 20000; i1++) { if (kvm_vcore_force_sharpen(125000, 0, 0, 100000, 1, 0, i1, &b2)) { break; } }
		CHECK_EQ(t, i1);
		// the rate is not measured yet: the budget does not invent one, the minimum packet
		CHECK_EQ(kvm_vcore_force_sharpen(0, 1000, 0, 100000, 0, 0, 5000, &b1), 1); CHECK_EQ(b1, KVM_VIDEO_SHARPEN_FORCE_MIN);
	}
	// the clock wraps
	{
		uint32_t b = 0;
		CHECK_EQ(kvm_vcore_force_sharpen(375000, 0, 0, 100000, 1, 0xFFFFFFF0u, 0xFFFFFFF0u + 249u, &b), 0);
		CHECK_EQ(kvm_vcore_force_sharpen(375000, 0, 0, 100000, 1, 0xFFFFFFF0u, 0xFFFFFFF0u + 250u, &b), 1);
	}
}

static void test_scale_rule(void)
{
	CHECK(kvm_vcore_scale_allowed(0, 1, 0)); CHECK(kvm_vcore_scale_allowed(0, 0, 0));		// no video: the ladder as before
	CHECK(!kvm_vcore_scale_allowed(1, 1, 100000));										// no step down while the video runs, however long it has run
	CHECK(!kvm_vcore_scale_allowed(1, 0, 0)); CHECK(!kvm_vcore_scale_allowed(1, 0, KVM_VIDEO_SCALE_RETURN_MS - 1));
	CHECK(kvm_vcore_scale_allowed(1, 0, KVM_VIDEO_SCALE_RETURN_MS));					// the return to the viewer's scale: not sooner than 10 s after the last generation
}

static void test_frame_interval(void)
{
	// the pace uses it
	{
		kvm_vctl c;
		kvm_vctl_init(&c, 1);
		CHECK(kvm_vctl_pace_ok(&c, 1000, 60));
		c.haveFrame = 1; c.lastFrame = 1000;
		CHECK(!kvm_vctl_pace_ok(&c, 1044, 60)); CHECK(kvm_vctl_pace_ok(&c, 1045, 60));
		CHECK(kvm_vctl_pace_ok(&c, 1001, 0));
	}
}


// a frame is not encoded right behind the write of a frame that was late
static void test_write_gap(uint32_t base)
{
	kvm_vctl c;
	uint32_t t;
	kvm_vctl_init(&c, 1);
	// nothing written yet: the slot is the only rule
	CHECK_EQ(kvm_vctl_write_wait_ms(&c, base, 33), 0);
	c.haveFrame = 1; c.lastFrame = base;						// the slot of the last frame
	CHECK(kvm_vctl_pace_ok(&c, base + 25, 33));
	// the frame was late: its slot is `base`, it was written at base + 40 (the encoder 10 ms); the next frame is due by the slot at once (45 >= 33 - 8) but would be written at 55, 15 ms after
	kvm_vctl_note_write(&c, base + 40, 10);
	CHECK(!kvm_vctl_pace_ok(&c, base + 45, 33));				// 45 + 10 - 40 = 15 < 24 (75 % of 33)
	CHECK_EQ(kvm_vctl_write_wait_ms(&c, base + 45, 33), 9);
	CHECK(!kvm_vctl_pace_ok(&c, base + 53, 33));
	CHECK(kvm_vctl_pace_ok(&c, base + 54, 33));					// 54 + 10 - 40 = 24
	CHECK_EQ(kvm_vctl_write_wait_ms(&c, base + 54, 33), 0);
	// High (16 ms): the steady state: written 8 ms after its slot, the next one starts at slot + 16 (or 4 ms early): never held
	kvm_vctl_init(&c, 1);
	c.haveFrame = 1; c.lastFrame = base;
	kvm_vctl_note_write(&c, base + 8, 8);
	CHECK(kvm_vctl_pace_ok(&c, base + 16, 16));
	CHECK(kvm_vctl_pace_ok(&c, base + 12, 16));					// 12 early allowed by the slot (25 %): 12 + 8 - 8 = 12 = 75 % of 16: just allowed
	CHECK(!kvm_vctl_pace_ok(&c, base + 11, 16));				// (the slot rule alone would let 12 come too; 11 is held by both)
	{
		// a frame that took longer than expected (written 11 ms after the slot): the next at slot + 12 would be 9 ms behind it
		kvm_vctl_note_write(&c, base + 11, 8);
		CHECK(!kvm_vctl_pace_ok(&c, base + 12, 16));
		CHECK_EQ(kvm_vctl_write_wait_ms(&c, base + 12, 16), 3);
		CHECK(kvm_vctl_pace_ok(&c, base + 15, 16));
	}
	// a slow encoder does not hold the frame back by itself: encMs counts as half the interval at most (an average that a few slow frames raised must not let the frame come right behind a late one)
	kvm_vctl_init(&c, 1);
	c.haveFrame = 1; c.lastFrame = base;
	kvm_vctl_note_write(&c, base + 100, 100);
	CHECK_EQ(kvm_vctl_write_wait_ms(&c, base + 100, 33), 8);	// 100 + 16 - 100 = 16 < 24
	CHECK_EQ(kvm_vctl_write_wait_ms(&c, base + 108, 33), 0);
	CHECK_EQ(kvm_vctl_write_wait_ms(&c, base + 107, 33), 1);
	// with the encoder in a thread the whole encode time counts (at most the interval): the frame that starts at the landing of the last one is written an encode time later
	kvm_vctl_set_threaded(&c, 1);
	CHECK_EQ(kvm_vctl_write_wait_ms(&c, base + 100, 33), 0);	// 100 ms of average, an interval of 33: 33 counts, the gap of 24 is over
	kvm_vctl_note_write(&c, base + 100, 14);
	CHECK_EQ(kvm_vctl_write_wait_ms(&c, base + 100, 16), 0);	// 14 ms >= the gap of 12 ms
	kvm_vctl_note_write(&c, base + 100, 8);
	CHECK_EQ(kvm_vctl_write_wait_ms(&c, base + 100, 16), 4);	// 8 ms: 4 ms to wait (the loop wakes for them)
	kvm_vctl_note_write(&c, base + 100, 14);
	kvm_vctl_set_threaded(&c, 0);
	CHECK_EQ(kvm_vctl_write_wait_ms(&c, base + 100, 16), 4);	// the synchronous encoder: capped at half the interval (8 ms)
	// no interval: no limit
	CHECK_EQ(kvm_vctl_write_wait_ms(&c, base, 0), 0);
	CHECK(kvm_vctl_pace_ok(&c, base, 0));
	// a new generation: its first frame waits for nothing
	{
		kvm_stream s;
		kvm_state_msg m;
		kvm_vctl_init(&c, 1);
		kvm_stream_init(&s, 1, 1, KVM_REASON_OK, KVM_REASON_DISABLED);
		s.ackActive = 1;
		c.haveFrame = 1; c.lastFrame = base;
		kvm_vctl_note_write(&c, base, 30);
		CHECK(c.haveWrite);
		CHECK_EQ(kvm_vctl_begin_gen(&c, &s, 100, 42, 1920, 1088, 1, &m), 0);
		CHECK(!c.haveWrite);
	}
	// the plan: no frame inside the gap, and nothing is counted as held by the window or by the byte bucket
	{
		kvm_stream s;
		kvm_vplan_in in;
		kvm_vplan plan;
		kvm_state_msg m;
		kvm_vctl_init(&c, 1);
		kvm_stream_init(&s, 1, 1, KVM_REASON_OK, KVM_REASON_DISABLED);
		s.ackActive = 1;
		CHECK_EQ(kvm_vctl_begin_gen(&c, &s, 100, 42, 1920, 1088, 1, &m), 0);
		(void)kvm_vctl_frame_done(&c, 1, 1000, base); c.lastFrame = base;
		kvm_vctl_note_write(&c, base + 40, 10);
		memset(&in, 0, sizeof(in));
		in.pending = 50; in.gateOpen = 1; in.rateOpen = 1; in.intervalMs = 33; in.now = base + 45;
		kvm_vctl_plan(&c, &s, &in, &plan);
		CHECK(!plan.encode && !plan.held && !plan.heldRate);
		in.now = base + 54;
		kvm_vctl_plan(&c, &s, &in, &plan);
		CHECK(plan.encode);
	}
	// the clock wraps
	kvm_vctl_init(&c, 1);
	c.haveFrame = 1; c.lastFrame = 0xFFFFFFE0u;
	kvm_vctl_note_write(&c, 0xFFFFFFF8u, 8);
	CHECK(!kvm_vctl_pace_ok(&c, 0x00000004u, 33));				// 12 + 8 - 8 = 12 < 24 (the slot 36 ms ago allows it)
	CHECK(kvm_vctl_pace_ok(&c, 0x00000010u, 33));				// 24 + 8 - 8 = 24
	(void)t;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the sequence of a whole session, as the glue runs it: 242 and generation, the first frame, IDR rules, positions
// ---------------------------------------------------------------------------------------------------------------------------------------

typedef struct ev { int kind; int gen; int idr; uint32_t t; uint32_t seq; } ev;	// kind 1 = 242 (video 1), 2 = 242 (video 0), 3 = frame
#define NEV 20000
static ev gEv[NEV];
static int gNev;
static void emit(int kind, int gen, int idr, uint32_t t, uint32_t seq) { if (gNev < NEV) { gEv[gNev].kind = kind; gEv[gNev].gen = gen; gEv[gNev].idr = idr; gEv[gNev].t = t; gEv[gNev].seq = seq; gNev++; } }

typedef struct fake_enc { int open, idrNext, forceCalls, brokenNext, pNext, sz; } fake_enc;

// One run of the session with random events. Checks, on the log: a 242 (1,1) for generation N comes before every frame of N; the first frame of a generation is an IDR;
// a frame never has the generation of a stopped stream; IDRs after the first of a generation are at least the spacing apart; seq increases by one.
static void test_session_property(uint32_t t0)
{
	int iter;
	for (iter = 0; iter < 80; iter++)
	{
		kvm_vctl c;
		kvm_stream s;
		kvm_state_msg m;
		fake_enc e;
		uint32_t t = t0 + (uint32_t)(iter * 77777);
		int step, genFirst = 1, lastSeq = 0;
		memset(&e, 0, sizeof(e));
		gNev = 0;
		kvm_vctl_init(&c, 1);
		kvm_stream_init(&s, 1, 1, KVM_REASON_NO_VIDEO_BUILD, KVM_REASON_DISABLED);
		for (step = 0; step < 600; step++)
		{
			int what = (int)(rnd() % 14);
			t += 1 + (rnd() % 40);
			if (what == 0 || what == 1)
			{
				// op 1
				kvm_ctrl a = op1(7, (rnd() & 1) ? 0 : 51);
				kvm_state_msg r;
				unsigned act = 0;
				(void)kvm_stream_on_ctrl(&s, &a, t, &r, &act);
				if ((act & KVM_ACT_VIDEO_REQUEST) && s.want == KVM_WANT_VIDEO && s.vetoMask == 0 && c.capVideo)
				{
					if (kvm_vctl_op1(&c, &a, t) == KVM_VOP1_START && kvm_vctl_open_allowed(&c, t))
					{
						e.open = 1; e.idrNext = 1; kvm_vctl_open_note(&c, t);
						if (kvm_vctl_begin_gen(&c, &s, 100, 42, 1920, 1088, 1, &m) == 0)
						{
							genFirst = 1;
							emit(1, m.gen, 0, t, 0);
							kvm_vctl_repair_now(&c, t);
						}
					}
				}
			}
			else if (what == 2)
			{
				kvm_ctrl a = op1(0, 0);
				kvm_state_msg r;
				unsigned act = 0;
				a.op = (rnd() & 1) ? KVM_OP_VIDEO_OFF : KVM_OP_ACK_ONLY; a.flags = 0; a.maxW = 0; a.maxH = 0;
				(void)kvm_stream_on_ctrl(&s, &a, t, &r, &act);
				if (act & KVM_ACT_VIDEO_STOP) { kvm_vctl_stop(&c); e.open = 0; emit(2, s.gen, 0, t, 0); }
			}
			else if (what == 3)
			{
				kvm_ctrl a = op1(0, 0);
				kvm_state_msg r;
				unsigned act = 0;
				a.op = KVM_OP_REPAIR; a.flags = (uint8_t)(1 + (rnd() % 3));
				(void)kvm_stream_on_ctrl(&s, &a, t, &r, &act);
				kvm_vctl_op3(&c, act);
			}
			else if (what == 4 && c.state == KVM_VS_ON)
			{
				// cmd 7 with a new size: a new generation on the same encoder (or a new one)
				int fresh = (int)(rnd() & 1);
				if (fresh) { e.open = 1; e.idrNext = 1; }
				if (kvm_vctl_begin_gen(&c, &s, 100, 42, 1920, 1088, fresh, &m) == 0)
				{
					genFirst = 1;
					emit(1, m.gen, 0, t, 0);
				}
			}
			else if (what == 5)
			{
				// veto or the end of it
				kvm_ctrl a = op1(0, 0);
				kvm_state_msg r;
				unsigned act = 0;
				a.op = (rnd() & 1) ? KVM_OP_VETO : KVM_OP_VETO_END; a.flags = 1;
				(void)kvm_stream_on_ctrl(&s, &a, t, &r, &act);
				if (act & KVM_ACT_VIDEO_STOP) { kvm_vctl_stop(&c); e.open = 0; emit(2, s.gen, 0, t, 0); }
			}
			else if (what == 6 && c.state == KVM_VS_ON)
			{
				// an error of the encoder
				(void)kvm_vctl_error(&c, t); s.videoOn = 0; e.open = 0; emit(2, s.gen, 0, t, 0);
			}
			else if (what == 7 && c.inError)
			{
				t = c.errorUntil + 1;
				if (kvm_vctl_retry_due(&c, t) && s.want == KVM_WANT_VIDEO && s.vetoMask == 0 && kvm_vctl_open_allowed(&c, t))
				{
					e.open = 1; e.idrNext = 1; kvm_vctl_open_note(&c, t);
					if (kvm_vctl_begin_gen(&c, &s, 100, 42, 1920, 1088, 1, &m) == 0) { genFirst = 1; emit(1, m.gen, 0, t, 0); }
				}
			}
			else
			{
				// a picture with motion: the loop plans a frame
				kvm_vplan p;
				kvm_vplan_in in;
				in.now = t; in.pending = 1 + (rnd() % 5); in.gateOpen = (rnd() % 5) != 0; in.rateOpen = (rnd() % 7) != 0; in.intervalMs = 16; in.rate = (rnd() & 1) ? 0 : (uint64_t)(50000 + (rnd() % 400000));
				kvm_vctl_plan(&c, &s, &in, &p);
				if (p.encode)
				{
					int isIdr, bytes, r;
					if (p.forceIdr) { e.forceCalls++; e.idrNext = 1; }
					isIdr = e.idrNext || (rnd() % 97) == 0;	// the encoder also makes an IDR nobody asked for now and then
					if (genFirst && !e.idrNext && (rnd() % 7) == 0) { isIdr = 0; }	// an encoder that does not do what it was told
					e.idrNext = 0;
					bytes = isIdr ? 100000 + (int)(rnd() % 200000) : 2000 + (int)(rnd() % 20000);
					if ((rnd() % 40) == 0)
					{
						r = kvm_vctl_frame_broken(&c, 0);
						e.idrNext = 1;
						if (r == KVM_VB_ERROR) { (void)kvm_vctl_error(&c, t); s.videoOn = 0; e.open = 0; emit(2, s.gen, 0, t, 0); }
					}
					else
					{
						r = kvm_vctl_frame_done(&c, isIdr, (size_t)bytes, t);
						if (r == KVM_VF_BROKEN) { e.idrNext = 1; }
						else if (r == KVM_VF_ERROR) { (void)kvm_vctl_error(&c, t); s.videoOn = 0; e.open = 0; emit(2, s.gen, 0, t, 0); }
						else
						{
							uint32_t seq = kvm_stream_seq_next((uint32_t)lastSeq);
							lastSeq = (int)seq;
							emit(3, s.gen, isIdr, t, seq);
							genFirst = 0;
						}
					}
				}
			}
		}
		// the checks on the log
		{
			int i, cur = -1, haveFirst = 0;
			uint32_t prevSeq = 0;
			for (i = 0; i < gNev; i++)
			{
				const ev *x = &gEv[i];
				if (x->kind == 1) { cur = x->gen; haveFirst = 0; CHECK(cur >= 0); }
				else if (x->kind == 2) { cur = -1; }
				else
				{
					CHECK(cur >= 0);					// a frame only while a 242 (1,1) is in force
					CHECK_EQ(x->gen, cur);			// ... and of its generation
					if (!haveFirst) { CHECK(x->idr); haveFirst = 1; }	// the first frame of a generation is an IDR
					if (prevSeq != 0) { CHECK_EQ(x->seq, kvm_stream_seq_next(prevSeq)); }
					prevSeq = x->seq;
				}
			}
		}
		if (gFail > 20) { return; }
	}
}

// the IDRs that a viewer asks for are spaced; the first of a generation is not (a separate check on a longer, simpler run)
static void test_idr_spacing_run(uint32_t t0)
{
	kvm_vctl c;
	kvm_stream s;
	kvm_state_msg m;
	uint32_t t = t0;
	int i, nIdr = 0;
	uint32_t last = 0;
	kvm_vctl_init(&c, 1);
	make_stream(&s, t);
	(void)kvm_vctl_begin_gen(&c, &s, 100, 42, 1920, 1088, 1, &m);
	for (i = 0; i < 5000; i++)
	{
		kvm_vplan_in in;
		kvm_vplan p;
		t += 16;
		if ((i % 7) == 0) { kvm_vctl_op3(&c, KVM_ACT_IDR); }		// the viewer asks for an IDR all the time
		in.now = t; in.pending = 3; in.gateOpen = 1; in.intervalMs = 16; in.rate = 200000;
		kvm_vctl_plan(&c, &s, &in, &p);
		if (!p.encode) { continue; }
		if (c.idrFirst || p.forceIdr)
		{
			uint32_t bytes = 150000;
			if (nIdr > 0) { CHECK(t - last >= kvm_vcore_idr_spacing(bytes, 200000)); }
			CHECK_EQ(kvm_vctl_frame_done(&c, 1, bytes, t), 0);
			last = t; nIdr++;
		}
		else { CHECK_EQ(kvm_vctl_frame_done(&c, 0, 3000, t), 0); }
	}
	// 80 s with 1.5 s between the IDRs (2 * 150 000 / 200 000 = 1.5 s)
	CHECK(nIdr >= 50 && nIdr <= 55);
}

// ---------------------------------------------------------------------------------------------------------------------------------------

// the highest bit rate of a level (Table A-1 MaxBR, 1250 for High)
static void test_max_bitrate(void)
{
	CHECK_EQ(kvm_vcore_max_bitrate(42, 100), 62500000u);
	CHECK_EQ(kvm_vcore_max_bitrate(42, 77), 50000000u);
	CHECK_EQ(kvm_vcore_max_bitrate(40, 100), 25000000u);
	CHECK_EQ(kvm_vcore_max_bitrate(41, 66), 50000000u);
	CHECK_EQ(kvm_vcore_max_bitrate(31, 77), 14000000u);
	CHECK_EQ(kvm_vcore_max_bitrate(32, 77), 20000000u);
	CHECK_EQ(kvm_vcore_max_bitrate(50, 100), 168750000u);
	CHECK_EQ(kvm_vcore_max_bitrate(51, 100), 300000000u);
	CHECK_EQ(kvm_vcore_max_bitrate(52, 77), 240000000u);
	CHECK_EQ(kvm_vcore_max_bitrate(30, 77), 10000000u);
	CHECK_EQ(kvm_vcore_max_bitrate(0, 100), 12500000u);				// unknown: the limit of level 3.0
}

// why a frame is a key frame (the log line "video idr")
static void test_idr_why(void)
{
	kvm_vctl c;
	kvm_vplan p;
	memset(&c, 0, sizeof(c));
	memset(&p, 0, sizeof(p));
	CHECK_EQ(kvm_vctl_idr_why(&c, &p), KVM_IDR_WHY_NONE);
	c.idrFirst = 1; CHECK_EQ(kvm_vctl_idr_why(&c, &p), KVM_IDR_WHY_FIRST);
	c.idrForce = 1; CHECK_EQ(kvm_vctl_idr_why(&c, &p), KVM_IDR_WHY_BROKEN);		// a lost access unit is the stronger reason (it is what makes the IDR immediate)
	c.idrFirst = c.idrForce = 0;
	c.idrReq = 1; CHECK_EQ(kvm_vctl_idr_why(&c, &p), KVM_IDR_WHY_NONE);			// asked for but the spacing has not passed: the frame is no IDR
	p.forceIdr = 1; CHECK_EQ(kvm_vctl_idr_why(&c, &p), KVM_IDR_WHY_OP3);
	c.idrReq = 0; CHECK_EQ(kvm_vctl_idr_why(&c, &p), KVM_IDR_WHY_NONE);			// a forced IDR nobody asked for with op 3 (cannot happen) is not called op3
	CHECK_EQ(kvm_vctl_idr_why(&c, NULL), KVM_IDR_WHY_NONE);
	CHECK(strcmp(kvm_idr_why_name(KVM_IDR_WHY_FIRST, 1), "first") == 0);
	CHECK(strcmp(kvm_idr_why_name(KVM_IDR_WHY_BROKEN, 1), "broken") == 0);
	CHECK(strcmp(kvm_idr_why_name(KVM_IDR_WHY_OP3, 1), "op3") == 0);
	CHECK(strcmp(kvm_idr_why_name(KVM_IDR_WHY_NONE, 1), "encoder") == 0);			// an IDR that came out of the encoder with no request: its own doing
	CHECK(strcmp(kvm_idr_why_name(KVM_IDR_WHY_OP3, 0), "none") == 0);
}

int main(void)
{
	test_frame_layout();
	test_tiles_basic();
	test_rects_shapes();
	test_rects_over_limit();
	test_rects_property();
	test_tiles_model();
	test_tiles_inflight_model();
	test_tiles_inflight_rules();
	test_levels();
	test_idr_spacing();
	test_sps_check();
	test_force_sharpen();
	test_frame_interval();
	test_scale_rule();
	test_write_gap(1000);
	test_write_gap(0xFFFFFF00u);
	test_op1_open_error(1000);
	test_op1_open_error(0xFFFFFF00u);
	test_begin_gen(1000);
	test_plan_and_idr(1000);
	test_plan_and_idr(0xFFFFF000u);
	test_repair_rate(1000);
	test_session_property(1000);
	test_idr_spacing_run(1000);
	test_idr_why();
	test_max_bitrate();
	printf("%d checks, %d failed\n", gChecks, gFail);
	return gFail == 0 ? 0 : 1;
}
