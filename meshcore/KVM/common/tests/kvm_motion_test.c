/*
Tests of kvm_motion.h / kvm_motion.c: the class of a tile while the video runs (the streak rule on the neighbourhood of the last 250 ms, the near rule), the source of the
picture is not motion, the series of motion and their line in the log. The old rule of kvm_adapt.cpp is modelled here, line by line,
so that the tests show what changed. Run with run-kvm-motion-tests.sh (gcc in a container: -Werror -pedantic, then ASan + UBSan).
*/

#include "../kvm_motion.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gFail = 0, gChecks = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); gChecks++; if (_a != _b) { gFail++; printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

#define ROWS 34
#define COLS 60

// ---------------------------------------------------------------------------------------------------------------------------------------
// a model of the tile loop of kvm_adapt.cpp: the old rule and the new one on the same sequence of frames
// ---------------------------------------------------------------------------------------------------------------------------------------

typedef struct mtile { unsigned streak; uint32_t lastChange; int seen; } mtile;

typedef struct model
{
	mtile t[ROWS][COLS];
	kvm_motion m;
	// the old rule: changedThisFrame / prevChanged as kvm_adapt_frame_begin kept them
	unsigned changedThisFrame, prevChanged, realChanged;
	uint32_t lastFrameTick;
	int sourceChange;
	// the results of the last frame
	unsigned nChanged, nMotionOld, nMotionNew;
} model;

static void model_init_ex(model *x, int seen)
{
	int r, c;
	memset(x, 0, sizeof(*x));
	kvm_motion_init(&x->m);
	CHECK_EQ(kvm_motion_resize(&x->m, ROWS, COLS), 0);
	for (r = 0; r < ROWS; r++) { for (c = 0; c < COLS; c++) { x->t[r][c].seen = seen; } }	// a session that has been running: every tile has been looked at
}
static void model_init(model *x) { model_init_ex(x, 1); }
static void model_init_fresh(model *x) { model_init_ex(x, 0); }

// kvm_adapt_frame_begin
static void model_frame_begin(model *x, uint32_t now)
{
	kvm_motion_frame(&x->m, x->lastFrameTick, x->realChanged);		// (the new rule: the frame before this one, as lastMotionChange = lastFrameTick)
	x->realChanged = 0;
	if ((uint32_t)(now - x->lastFrameTick) > 250u) { x->prevChanged = 0; }
	else if (x->changedThisFrame > 0) { x->prevChanged = x->changedThisFrame; }
	x->changedThisFrame = 0;
	x->lastFrameTick = now;
	x->nChanged = x->nMotionOld = x->nMotionNew = 0;
}

// kvm_adapt_note_crc + kvm_adapt_class for a tile whose content changed in this frame
static void model_change(model *x, uint32_t now, int row, int col)
{
	mtile *t = &x->t[row][col];
	int32_t since = (int32_t)(now - t->lastChange);
	// ---- the old rule ----
	unsigned oldStreak = (t->streak > 0 && since <= 250) ? (t->streak < 255 ? t->streak + 1 : 255) : 1;
	int oldClass = (oldStreak >= 2 && since <= 250 && x->prevChanged >= 16) ? 1 : 0;	// kvm_adapt_class: streak >= 2, the change is at most 250 ms old (always: lastChange = now), prevChanged >= 16
	// ---- the new rule ----
	unsigned newStreak = kvm_motion_note_change(&x->m, t->streak, since, x->sourceChange);
	int newClass;
	(void)oldClass;
	t->streak = newStreak;
	newClass = kvm_motion_class(&x->m, now, row, col, newStreak, 0);
	if (!x->sourceChange && t->seen) { x->realChanged++; }
	if (!x->sourceChange) { x->changedThisFrame++; }
	t->seen = 1; t->lastChange = now;
	x->nChanged++; x->nMotionOld += (unsigned)oldClass; x->nMotionNew += (unsigned)newClass;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the rules one by one
// ---------------------------------------------------------------------------------------------------------------------------------------

static void test_streak_and_recent(void)
{
	kvm_motion m;
	kvm_motion_init(&m);
	CHECK_EQ(kvm_motion_resize(&m, ROWS, COLS), 0);
	// nothing happened yet: no tile is motion, whatever its streak
	CHECK(!kvm_motion_recent(&m, 1000));
	CHECK(!kvm_motion_recent(&m, 100));						// (also when the clock of the process is young)
	CHECK(!kvm_motion_recent(&m, 0));
	CHECK_EQ(kvm_motion_class(&m, 1000, 5, 5, 2, 0), 0);
	// a frame with 15 changes is not a frame of motion, 16 is
	kvm_motion_frame(&m, 1000, 15); CHECK(!kvm_motion_recent(&m, 1000));
	kvm_motion_frame(&m, 1000, 16); CHECK(kvm_motion_recent(&m, 1000));
	CHECK(kvm_motion_recent(&m, 1250));
	CHECK(!kvm_motion_recent(&m, 1251));
	CHECK(kvm_motion_recent(&m, 990));						// a stamp later than `now` (the frame of this iteration) is "just now"
	// the streak rule: two changes in a row, the neighbourhood is the frame of motion within 250 ms
	CHECK_EQ(kvm_motion_class(&m, 1100, 5, 5, 2, 0), 1);
	CHECK_EQ(kvm_motion_class(&m, 1100, 5, 6, 3, 0), 1);
	CHECK_EQ(kvm_motion_class(&m, 1100, 12, 12, 1, 0), 0);	// the first change, with no motion around it
	CHECK_EQ(kvm_motion_class(&m, 1100, 8, 8, 2, 300), 0);	// the previous change is too old
	CHECK_EQ(kvm_motion_class(&m, 1260, 9, 9, 2, 0), 0);	// the last frame of motion is too old
	// a frame with a few changes in the middle of a motion does not end it (the old rule looked at the previous frame only)
	kvm_motion_frame(&m, 1300, 3);
	kvm_motion_frame(&m, 1316, 2);
	CHECK_EQ(kvm_motion_class(&m, 1330, 9, 9, 2, 0), 0);	// (1000 is more than 250 ms ago)
	kvm_motion_frame(&m, 1320, 40);
	kvm_motion_frame(&m, 1336, 2);
	CHECK_EQ(kvm_motion_class(&m, 1352, 9, 9, 2, 0), 1);
	// the tile positions outside the grid do not crash and are not stamped
	CHECK_EQ(kvm_motion_class(&m, 1352, -1, 5, 2, 0), 1);
	CHECK_EQ(kvm_motion_class(&m, 1352, ROWS, COLS, 2, 0), 1);
	kvm_motion_free(&m);
}

static void test_near(void)
{
	kvm_motion m;
	kvm_motion_init(&m);
	CHECK_EQ(kvm_motion_resize(&m, ROWS, COLS), 0);
	kvm_motion_frame(&m, 1000, 100);
	CHECK_EQ(kvm_motion_class(&m, 1010, 10, 10, 2, 0), 1);			// a tile that moves: the stamp
	// the first change of a tile within two tiles (64 px) of it goes to the video
	CHECK_EQ(kvm_motion_class(&m, 1016, 10, 12, 1, 0), 1);
	CHECK_EQ(kvm_motion_class(&m, 1016, 12, 12, 1, 0), 1);
	CHECK_EQ(kvm_motion_class(&m, 1016, 8, 8, 1, 0), 1);
	CHECK_EQ(kvm_motion_class(&m, 1016, 10, 7, 1, 0), 0);			// three tiles away, on the left
	CHECK_EQ(kvm_motion_class(&m, 1016, 7, 10, 1, 0), 0);			// ... above
	CHECK_EQ(kvm_motion_class(&m, 1016, 10, 13, 1, 0), 0);
	CHECK_EQ(kvm_motion_class(&m, 1016, 13, 10, 1, 0), 0);
	CHECK_EQ(kvm_motion_class(&m, 1016, 7, 7, 1, 0), 0);
	// a tile that was taken by the near rule does not stamp: the area does not spread (typing beside a video)
	CHECK_EQ(kvm_motion_class(&m, 1020, 10, 12, 1, 0), 1);
	CHECK_EQ(kvm_motion_class(&m, 1020, 10, 14, 1, 0), 0);			// two tiles from the near tile, four from the tile that moves
	// the stamp lasts 250 ms
	CHECK_EQ(kvm_motion_class(&m, 1260, 10, 12, 1, 0), 1);			// 250 ms after 1010
	CHECK_EQ(kvm_motion_class(&m, 1261, 10, 12, 1, 0), 0);
	// the edge of the grid
	kvm_motion_frame(&m, 2000, 100);
	CHECK_EQ(kvm_motion_class(&m, 2000, 0, 0, 2, 0), 1);
	CHECK_EQ(kvm_motion_class(&m, 2000, 2, 2, 1, 0), 1);
	CHECK_EQ(kvm_motion_class(&m, 2000, 3, 3, 1, 0), 0);
	CHECK_EQ(kvm_motion_class(&m, 2000, ROWS - 1, COLS - 1, 2, 0), 1);
	CHECK_EQ(kvm_motion_class(&m, 2000, ROWS - 3, COLS - 3, 1, 0), 1);
	CHECK_EQ(kvm_motion_class(&m, 2000, ROWS - 4, COLS - 4, 1, 0), 0);
	// reset forgets
	kvm_motion_reset(&m);
	CHECK(!kvm_motion_recent(&m, 2000));
	CHECK_EQ(kvm_motion_class(&m, 2000, 1, 1, 1, 0), 0);
	// a new grid of the same size forgets the stamps
	kvm_motion_frame(&m, 2100, 100);
	CHECK_EQ(kvm_motion_class(&m, 2100, 20, 20, 2, 0), 1);
	CHECK_EQ(kvm_motion_class(&m, 2100, 20, 21, 1, 0), 1);
	CHECK_EQ(kvm_motion_resize(&m, ROWS, COLS), 0);
	CHECK_EQ(kvm_motion_class(&m, 2100, 20, 21, 1, 0), 0);
	// no memory: the near rule is off, the streak rule works
	kvm_motion_free(&m);
	kvm_motion_init(&m);
	kvm_motion_frame(&m, 3000, 100);
	CHECK_EQ(kvm_motion_class(&m, 3000, 5, 5, 2, 0), 1);
	CHECK_EQ(kvm_motion_class(&m, 3000, 5, 6, 1, 0), 0);
	// the radius is a parameter
	kvm_motion_set(&m, 250, 2, 16, 0, 0);
	CHECK_EQ(kvm_motion_resize(&m, ROWS, COLS), 0);
	kvm_motion_frame(&m, 4000, 100);
	CHECK_EQ(kvm_motion_class(&m, 4000, 5, 5, 2, 0), 1);
	CHECK_EQ(kvm_motion_class(&m, 4000, 5, 6, 1, 0), 0);
	CHECK_EQ(kvm_motion_class(&m, 4000, 5, 5, 1, 0), 0);			// not even the tile itself, that was stamped a moment ago
	kvm_motion_free(&m);
	// a bad grid
	CHECK_EQ(kvm_motion_resize(&m, 0, 5), -1);
	CHECK_EQ(kvm_motion_resize(&m, 5, 0), -1);
	CHECK_EQ(kvm_motion_resize(&m, 5000, 5), -1);
	CHECK(m.at == NULL);
	// the time wraps
	kvm_motion_init(&m);
	CHECK_EQ(kvm_motion_resize(&m, ROWS, COLS), 0);
	kvm_motion_frame(&m, 0xFFFFFFF0u, 100);
	CHECK_EQ(kvm_motion_class(&m, 0xFFFFFFF8u, 4, 4, 2, 0), 1);
	CHECK_EQ(kvm_motion_class(&m, 0x00000010u, 4, 6, 1, 0), 1);		// 32 ms later, over the wrap
	CHECK_EQ(kvm_motion_class(&m, 0x00000100u, 4, 6, 1, 0), 0);		// 264 ms
	kvm_motion_free(&m);
}

// the memory of the place: the first change of a tile whose own last motion is younger than 3 s is motion (the animation comes back where the last one was), without a stamp of its own
static void test_recall(void)
{
	kvm_motion m;
	int r, c;
	kvm_motion_init(&m);
	CHECK_EQ(kvm_motion_resize(&m, ROWS, COLS), 0);
	kvm_motion_frame(&m, 1000, 100);
	for (r = 4; r < 8; r++) { for (c = 4; c < 8; c++) { CHECK_EQ(kvm_motion_class(&m, 1000, r, c, 2, 0), 1); } }	// the animation, 16 tiles: stamps at 1000
	// a pause of 2.4 s, no frame of motion: the first frame of the next series in the same place goes to the video
	CHECK_EQ(kvm_motion_class(&m, 3400, 5, 5, 1, 0), 1);
	CHECK_EQ(kvm_motion_class(&m, 3999, 7, 7, 1, 0), 1);
	CHECK_EQ(kvm_motion_class(&m, 4000, 6, 6, 1, 0), 0);			// 3000 ms: the memory is gone
	CHECK_EQ(kvm_motion_class(&m, 4000, 5, 5, 1, 0), 0);
	// a place where nothing moved: JPEG (a window that appears, typing)
	CHECK_EQ(kvm_motion_class(&m, 3400, 20, 20, 1, 0), 0);
	CHECK_EQ(kvm_motion_class(&m, 3400, 4, 12, 1, 0), 0);			// (not even next to it: the near rule looks 250 ms back)
	// the tile taken by the memory does not stamp: the same tile asked again after the stamp of the others has aged is not motion
	CHECK_EQ(kvm_motion_class(&m, 3999, 5, 6, 1, 0), 1);
	CHECK_EQ(kvm_motion_class(&m, 4001, 5, 6, 1, 0), 0);
	CHECK_EQ(m.sRecall, 3);
	// switched off
	kvm_motion_set(&m, 250, 2, 16, 2, 0);
	kvm_motion_reset(&m);
	kvm_motion_frame(&m, 1000, 100);
	CHECK_EQ(kvm_motion_class(&m, 1000, 5, 5, 2, 0), 1);
	CHECK_EQ(kvm_motion_class(&m, 2000, 5, 5, 1, 0), 0);
	// the wrap of the clock
	kvm_motion_set(&m, 250, 2, 16, 2, KVM_MOTION_RECALL_MS);
	kvm_motion_reset(&m);
	kvm_motion_frame(&m, 0xFFFFFFF0u, 100);
	CHECK_EQ(kvm_motion_class(&m, 0xFFFFFFF0u, 9, 9, 2, 0), 1);
	CHECK_EQ(kvm_motion_class(&m, 0x00000800u, 9, 9, 1, 0), 1);		// 2064 ms later
	CHECK_EQ(kvm_motion_class(&m, 0x00000C00u, 9, 9, 1, 0), 0);		// 3088 ms
	kvm_motion_free(&m);
}

// the source of the picture: the safety net and the alpha byte
static void test_source_absorb_and_alpha(void)
{
	uint8_t a[16] = { 1, 2, 3, 255, 4, 5, 6, 0, 7, 8, 9, 128, 10, 11, 12, 1 };
	CHECK_EQ(kvm_motion_source_absorb(1, 0), 1);			// a tile that differs after a change of the source: not sent, repainted by the sharpening
	CHECK_EQ(kvm_motion_source_absorb(1, 1), 0);			// the viewer has nothing (0xFF): it has to be sent
	CHECK_EQ(kvm_motion_source_absorb(0, 0), 0);			// an ordinary change
	CHECK_EQ(kvm_motion_source_absorb(0, 1), 0);
	kvm_clear_alpha(a, 16);
	CHECK(a[3] == 0 && a[7] == 0 && a[11] == 0 && a[15] == 0);
	CHECK(a[0] == 1 && a[1] == 2 && a[2] == 3 && a[4] == 4 && a[5] == 5 && a[6] == 6 && a[8] == 7 && a[9] == 8 && a[10] == 9 && a[12] == 10 && a[13] == 11 && a[14] == 12);
	{
		// the same screen from GDI (alpha 0xFF) and from DXGI (alpha 0) has the same bytes after it; a length that is not a multiple of 4 leaves the tail
		uint8_t g[11] = { 9, 9, 9, 255, 8, 8, 8, 255, 7, 7, 7 }, d[11] = { 9, 9, 9, 0, 8, 8, 8, 0, 7, 7, 7 };
		kvm_clear_alpha(g, 11);
		CHECK(memcmp(g, d, 11) == 0);
		kvm_clear_alpha(g, 0);
	}
}

static void test_note_change(void)
{
	kvm_motion m;
	kvm_motion_init(&m);
	CHECK_EQ(kvm_motion_note_change(&m, 0, 0, 0), 1);
	CHECK_EQ(kvm_motion_note_change(&m, 1, 100, 0), 2);
	CHECK_EQ(kvm_motion_note_change(&m, 1, 250, 0), 2);
	CHECK_EQ(kvm_motion_note_change(&m, 1, 251, 0), 1);
	CHECK_EQ(kvm_motion_note_change(&m, 254, 10, 0), 255);
	CHECK_EQ(kvm_motion_note_change(&m, 255, 10, 0), 255);
	CHECK_EQ(kvm_motion_note_change(&m, 5, -11, 0), 6);					// a stamp that is later than the clock of the frame: just changed
	// the picture of another source is not a change of the content, the streak starts again
	CHECK_EQ(kvm_motion_note_change(&m, 1, 100, 1), 0);
	CHECK_EQ(kvm_motion_note_change(&m, 7, 10, 1), 0);
	CHECK_EQ(kvm_motion_note_change(&m, 0, 0, 1), 0);
	// and the next real change is the first one
	CHECK_EQ(kvm_motion_note_change(&m, 0, 100, 0), 1);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// scenes: the old rule against the new on the same frames
// ---------------------------------------------------------------------------------------------------------------------------------------

typedef struct scene_res
{
	unsigned frames, tiles, oldVideo, newVideo;			// tiles that changed (all frames), of them class 1 by the old rule / by the new
	unsigned lateTiles, lateOld, lateNew;				// the same without the first frame (nothing can be known then)
} scene_res;

static void res_add(scene_res *r, const model *x, int first)
{
	r->frames++; r->tiles += x->nChanged; r->oldVideo += x->nMotionOld; r->newVideo += x->nMotionNew;
	if (!first) { r->lateTiles += x->nChanged; r->lateOld += x->nMotionOld; r->lateNew += x->nMotionNew; }
}

// A window of `wTiles` x `hTiles` tiles with a texture dragged to the right by `step` pixels a frame (60 frames a second): the tiles under the window change in every frame (the texture moves),
// and the edge uncovers new tiles
static scene_res scene_drag(int step, int frames, int extraSmall)
{
	model *x = (model *)calloc(1, sizeof(model));
	scene_res res;
	int f, r, c, x0 = 3 * 32, hTiles = 12, wTiles = 6;
	uint32_t now = 10000;
	memset(&res, 0, sizeof(res));
	model_init(x);
	for (f = 0; f < frames; f++)
	{
		int xa, xb, ca, cb, prevXa = x0 + (f - 1) * step, prevXb = prevXa + wTiles * 32;
		now += 16;
		model_frame_begin(x, now);
		xa = x0 + f * step; xb = xa + wTiles * 32;
		ca = xa / 32; cb = (xb - 1) / 32;
		if (f > 0) { int pa = prevXa / 32; if (pa < ca) { ca = pa; } if ((prevXb - 1) / 32 > cb) { cb = (prevXb - 1) / 32; } }
		for (r = 4; r < 4 + hTiles; r++) { for (c = ca; c <= cb && c < COLS; c++) { model_change(x, now, r, c); } }
		if (extraSmall && (f % 2) == 1) { model_change(x, now, 30, 50); model_change(x, now, 30, 51); }	// the pointer, a clock: a frame with a few changes
		res_add(&res, x, f == 0);
	}
	kvm_motion_free(&x->m);
	free(x);
	return res;
}

// an animation: `area` tiles change in every frame for `frames` frames, the other frames of the loop (the content is 30 Hz, the loop 60) change only a few tiles (the pointer)
static scene_res scene_animation(int tilesWide, int tilesHigh, int frames, int alternate)
{
	model *x = (model *)calloc(1, sizeof(model));
	scene_res res;
	int f, r, c;
	uint32_t now = 20000;
	memset(&res, 0, sizeof(res));
	model_init(x);
	for (f = 0; f < frames; f++)
	{
		now += 16;
		model_frame_begin(x, now);
		if (!alternate || (f % 2) == 0) { for (r = 6; r < 6 + tilesHigh; r++) { for (c = 10; c < 10 + tilesWide; c++) { model_change(x, now, r, c); } } }
		else { model_change(x, now, 30, 50); model_change(x, now, 30, 51); }	// only the pointer
		res_add(&res, x, f == 0);
	}
	kvm_motion_free(&x->m);
	free(x);
	return res;
}

static void test_scenes(void)
{
	static const int steps[3] = { 16, 32, 48 };
	int i;
	for (i = 0; i < 3; i++)
	{
		scene_res a = scene_drag(steps[i], 30, 0), b = scene_drag(steps[i], 30, 1);
		printf("class drag %2d px a frame: tiles %u, video after the first frame: old rule %u (%.0f %%), new %u (%.0f %%); with the pointer in half the frames: old %.0f %%, new %.0f %%\n", steps[i], a.lateTiles,
			   a.lateOld, 100.0 * a.lateOld / a.lateTiles, a.lateNew, 100.0 * a.lateNew / a.lateTiles, 100.0 * b.lateOld / b.lateTiles, 100.0 * b.lateNew / b.lateTiles);
		CHECK(a.lateNew * 100 >= a.lateTiles * 99);						// everything that moves goes to the video once the motion is known
		CHECK(b.lateNew * 100 >= b.lateTiles * 95);
		CHECK(a.lateOld < a.lateTiles);								// the old rule sends the uncovered edge as JPEG
		if (steps[i] >= 32) { CHECK(a.lateOld * 100 <= a.lateTiles * 90); }
		CHECK(a.newVideo > a.oldVideo);
	}
	{
		// an animation 30 Hz in a loop of 60: the frame in between has only the pointer, and the next frame of the animation was JPEG by the old rule
		scene_res a = scene_animation(30, 18, 40, 1), c = scene_animation(30, 18, 40, 0);
		printf("class animation 30 x 18 tiles: continuous: old %.0f %% new %.0f %% of the tiles after the first frame as video; every other frame only the pointer: old %.0f %%, new %.0f %%\n",
			   100.0 * c.lateOld / c.lateTiles, 100.0 * c.lateNew / c.lateTiles, 100.0 * a.lateOld / a.lateTiles, 100.0 * a.lateNew / a.lateTiles);
		CHECK(c.lateNew * 100 >= c.lateTiles * 99);
		CHECK(a.lateNew * 100 >= a.lateTiles * 90);
		CHECK(a.lateOld * 100 <= a.lateTiles * 60);					// the old rule: the frame after a frame with a few changes was JPEG
	}
}

// typing stays JPEG: a line of text typed at 8 characters a second, a caret that blinks, with and without a window that is dragged elsewhere; the first frame of a window that appears
static void test_typing_stays_jpeg(void)
{
	model *x = (model *)calloc(1, sizeof(model));
	unsigned video = 0, tiles = 0;
	int f, k;
	uint32_t now = 30000;
	model_init(x);
	for (f = 0; f < 200; f++)
	{
		now += 16;
		model_frame_begin(x, now);
		if (f % 8 == 0)
		{
			// a character: the tile of the character and the one of the caret (a text field of 4 tiles of height)
			k = f / 8;
			model_change(x, now, 20, 10 + (k % 40)); model_change(x, now, 20, 10 + ((k + 1) % 40));
			video += x->nMotionNew; tiles += x->nChanged;
		}
		else if (f % 33 == 0) { model_change(x, now, 20, 10 + (f / 8 + 1) % 40); video += x->nMotionNew; tiles += x->nChanged; }	// the caret
	}
	printf("class typing: %u tiles changed, %u of them video (must be 0)\n", tiles, video);
	CHECK_EQ(video, 0);
	CHECK(tiles > 40);
	kvm_motion_free(&x->m);
	// the same tile typed in twice within 250 ms (two characters of one tile: streak 2) with no motion on the screen: the frames have 2 changes, so JPEG
	video = 0;
	for (f = 0; f < 40; f++)
	{
		now += 100;
		model_frame_begin(x, now);
		model_change(x, now, 25, 20); video += x->nMotionNew;
	}
	CHECK_EQ(video, 0);
	free(x);
	// a window that appears: all its tiles are a first change, and nothing moves around it: the whole first frame is JPEG, the next frame of it (nothing changes in it) is not even sent
	x = (model *)calloc(1, sizeof(model));
	model_init(x);
	now += 1000;
	model_frame_begin(x, now);
	{ int r, c; for (r = 5; r < 25; r++) { for (c = 5; c < 45; c++) { model_change(x, now, r, c); } } }
	CHECK_EQ(x->nChanged, 800);
	CHECK_EQ(x->nMotionNew, 0);
	kvm_motion_free(&x->m);
	free(x);
}

// (the whole screen is filled from GDI, 120 ms later the first copy of DXGI differs in 2022 tiles. The old rule: 2022 tiles are motion (the video takes the whole
// screen: a key frame of 500 KB, then 820 KB of sharpening). The new: none.
static void test_source_change(void)
{
	model *x = (model *)calloc(1, sizeof(model));
	int r, c, changed = 0;
	uint32_t now = 40000;
	model_init_fresh(x);
	// frame A: GDI fill, every tile seen for the first time
	model_frame_begin(x, now);
	for (r = 0; r < ROWS; r++) { for (c = 0; c < COLS; c++) { model_change(x, now, r, c); } }
	CHECK_EQ(x->nChanged, (unsigned)(ROWS * COLS));
	CHECK_EQ(x->nMotionOld, 0);
	CHECK_EQ(x->nMotionNew, 0);
	CHECK_EQ(x->realChanged, 0);
	// frame B: the first copy of DXGI, 120 ms later, 2022 of 2040 tiles differ
	now += 120;
	model_frame_begin(x, now);
	for (r = 0; r < ROWS; r++) { for (c = 0; c < COLS; c++) { if (r * COLS + c >= 18) { model_change(x, now, r, c); changed++; } } }
	printf("source change start: the first DXGI copy changes %d tiles: the old rule makes %u of them video, the new %u\n", changed, x->nMotionOld, x->nMotionNew);
	CHECK_EQ(changed, 2022);
	CHECK_EQ(x->nMotionOld, 2022);			// what did (a failed measurement: 2022 tiles pending, a key frame of the whole screen)
	{
		// the same with the flag of the source
		model *y = (model *)calloc(1, sizeof(model));
		uint32_t t2 = 40000;
		model_init_fresh(y);
		model_frame_begin(y, t2);
		for (r = 0; r < ROWS; r++) { for (c = 0; c < COLS; c++) { model_change(y, t2, r, c); } }
		t2 += 120;
		model_frame_begin(y, t2);
		y->sourceChange = 1;
		for (r = 0; r < ROWS; r++) { for (c = 0; c < COLS; c++) { if (r * COLS + c >= 18) { model_change(y, t2, r, c); } } }
		CHECK_EQ(y->nChanged, 2022);
		CHECK_EQ(y->nMotionNew, 0);
		CHECK_EQ(y->realChanged, 0);		// it is not motion for the frame of motion either
		CHECK_EQ(y->changedThisFrame, 0);	// nor for "the tiles of the previous frame"
		CHECK(!kvm_motion_recent(&y->m, t2 + 16));
		// the picture then stands: a real change of a tile a little later is its first change (no motion), two in a row are motion only with a frame of motion
		y->sourceChange = 0;
		t2 += 100; model_frame_begin(y, t2); model_change(y, t2, 10, 10);
		CHECK_EQ(y->t[10][10].streak, 1);
		CHECK_EQ(y->nMotionNew, 0);
		t2 += 50; model_frame_begin(y, t2); model_change(y, t2, 10, 10);
		CHECK_EQ(y->t[10][10].streak, 2);
		CHECK_EQ(y->nMotionNew, 0);				// (no frame with 16 changes: a text field, not an animation)
		kvm_motion_free(&y->m);
		free(y);
	}
	// real motion that starts in the frame after the change of the source is motion again from its second frame
	{
		model *y = (model *)calloc(1, sizeof(model));
		uint32_t t2 = 50000;
		int f;
		model_init_fresh(y);
		model_frame_begin(y, t2);
		for (r = 0; r < ROWS; r++) { for (c = 0; c < COLS; c++) { model_change(y, t2, r, c); } }
		t2 += 100; model_frame_begin(y, t2); y->sourceChange = 1;
		for (r = 0; r < ROWS; r++) { for (c = 0; c < COLS; c++) { model_change(y, t2, r, c); } }
		y->sourceChange = 0;
		for (f = 0; f < 4; f++)
		{
			t2 += 16; model_frame_begin(y, t2);
			for (r = 5; r < 15; r++) { for (c = 5; c < 25; c++) { model_change(y, t2, r, c); } }
			if (f == 0) { CHECK_EQ(y->nMotionNew, 0); }		// the first frame of the motion: JPEG
			if (f >= 2) { CHECK_EQ(y->nMotionNew, y->nChanged); }
		}
		kvm_motion_free(&y->m);
		free(y);
	}
	kvm_motion_free(&x->m);
	free(x);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the series
// ---------------------------------------------------------------------------------------------------------------------------------------

static void test_series(void)
{
	kvm_series s;
	char line[512];
	uint32_t t = 100000;
	int i;
	kvm_series_init(&s);
	CHECK_EQ(kvm_series_format(&s, t, 16, line, sizeof(line)), 0);			// nothing happened: no line
	// a write outside a series is not counted
	kvm_series_write(&s, t, 0, 3, 16);
	CHECK_EQ(s.paintsJpeg, 0);
	// series 1: the first frame is JPEG, the video comes 33 ms later, then every 16 ms, one gap of 80 ms
	kvm_series_motion(&s, t, 15000);
	kvm_series_write(&s, t + 1, 0, 600, 16);
	kvm_series_motion(&s, t + 16, 15000);
	kvm_series_motion(&s, t + 33, 15000);
	kvm_series_write(&s, t + 33, 1, 600, 16);
	for (i = 2; i < 10; i++) { kvm_series_motion(&s, t + 33 + (uint32_t)i * 16, 15000); kvm_series_write(&s, t + 33 + (uint32_t)i * 16, 1, 600, 16); }
	kvm_series_motion(&s, t + 33 + 9 * 16 + 80, 15000);
	kvm_series_write(&s, t + 33 + 9 * 16 + 80, 1, 600, 16);
	CHECK_EQ(s.nSeries, 1); CHECK_EQ(s.paintsJpeg, 1); CHECK_EQ(s.paintsVideo, 10); CHECK_EQ(s.startMax, 33);
	CHECK_EQ(s.nGaps, 10);								// the JPEG write -> the first video (32 ms), 32 ms, 7 x 16 ms, 1 x 80 ms
	CHECK_EQ(s.over2x, 1);								// only the 80 ms gap is above twice 16
	CHECK_EQ(s.gapMax, 80);
	CHECK_EQ(kvm_series_gap_p95(&s), 80);						// the bucket edge (84), but never above the largest gap
	// the pause, series 2 that never gets a video frame (JPEG only)
	t += 5000;
	kvm_series_motion(&s, t, 4000);
	kvm_series_write(&s, t + 2, 0, 20, 16);
	kvm_series_motion(&s, t + 20, 4000);
	kvm_series_write(&s, t + 22, 0, 20, 16);
	// writes of one iteration (2 ms apart) are one picture
	kvm_series_write(&s, t + 23, 1, 100, 16);
	CHECK_EQ(s.nSeries, 2);
	CHECK_EQ(s.paintsJpeg, 3);
	CHECK_EQ(s.nGaps, 11);								// series 2: the gap of 20 ms; the write 1 ms later is the same picture
	CHECK_EQ(kvm_series_format(&s, t + 5000, 16, line, sizeof(line)) > 0, 1);	// the line closes series 2: it did get a video frame (the merged write above) -> no_video 0
	printf("series %s\n", line);
	CHECK(strstr(line, "video burst series=2 ") == line);
	CHECK(strstr(line, "no_video=0 ") != NULL);
	CHECK(strstr(line, "r_kbps_start_min=4000 ") != NULL);
	CHECK(strstr(line, "interval_ms=16") != NULL);
	CHECK(strlen(line) < 260);
	// the period is cleared, a series that is under way is not lost
	CHECK_EQ(s.nSeries, 0);
	// a series without any video frame
	t += 20000;
	kvm_series_motion(&s, t, 4000);
	kvm_series_write(&s, t + 5, 0, 50, 16);
	CHECK_EQ(kvm_series_format(&s, t + 3000, 16, line, sizeof(line)) > 0, 1);
	CHECK(strstr(line, "no_video=1 ") != NULL);
	CHECK(strstr(line, "jpeg_pct=100 ") != NULL);
	CHECK_EQ(kvm_series_format(&s, t + 3000, 16, line, sizeof(line)), 0);
	// a gap above a second is not a gap of the series (the motion stood still): the pictures of a series with a pause inside it
	t += 10000;
	kvm_series_init(&s);
	kvm_series_motion(&s, t, 0); kvm_series_write(&s, t, 1, 10, 16);
	kvm_series_motion(&s, t + 400, 0); kvm_series_write(&s, t + 400, 1, 10, 16);	// 400 ms: a gap
	kvm_series_motion(&s, t + 800, 0); kvm_series_write(&s, t + 800, 1, 10, 16);
	CHECK_EQ(s.nGaps, 2); CHECK_EQ(s.gapMax, 400);
	CHECK_EQ(kvm_series_gap_p95(&s), 400);
	CHECK_EQ(s.nSeries, 1);
	// motion goes on but nothing is written for 1.6 s: that is not a gap of the series
	kvm_series_motion(&s, t + 1200, 0); kvm_series_motion(&s, t + 1600, 0); kvm_series_motion(&s, t + 2000, 0); kvm_series_motion(&s, t + 2400, 0);
	kvm_series_write(&s, t + 2400, 1, 10, 16);
	CHECK_EQ(s.nGaps, 2);
	// R 0 (no video yet) is not the lowest R
	t += 10000;
	kvm_series_motion(&s, t, 0);
	kvm_series_motion(&s, t + 5000, 5000);
	kvm_series_motion(&s, t + 9000, 0);
	CHECK_EQ(s.rStartMin, 5000);
	// the buffer is too small: nothing is written past it
	t += 10000;
	kvm_series_motion(&s, t, 0); kvm_series_write(&s, t, 1, 10, 16);
	CHECK_EQ(kvm_series_format(&s, t + 5000, 16, line, 20), 0);
	// the clock wraps in a series
	kvm_series_init(&s);
	kvm_series_motion(&s, 0xFFFFFFF0u, 1000);
	kvm_series_write(&s, 0xFFFFFFF8u, 1, 10, 16);
	kvm_series_write(&s, 0x00000010u, 1, 10, 16);
	CHECK_EQ(s.nGaps, 1); CHECK_EQ(s.gapMax, 24);
	CHECK_EQ(s.startMax, 8);
}

int main(void)
{
	test_streak_and_recent();
	test_near();
	test_recall();
	test_source_absorb_and_alpha();
	test_note_change();
	test_scenes();
	test_typing_stays_jpeg();
	test_source_change();
	test_series();
	printf("%d checks, %d failed\n", gChecks, gFail);
	return gFail == 0 ? 0 : 1;
}
