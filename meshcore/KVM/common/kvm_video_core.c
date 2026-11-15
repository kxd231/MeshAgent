/*
KVM: the portable core of the H.264 video path of the agent. See kvm_video_core.h.
Portable C99, no operating system API.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#include "kvm_video_core.h"
#include "kvm_video_rc.h"
#include <stdlib.h>
#include <string.h>

static int32_t dms(uint32_t a, uint32_t b) { return (int32_t)(a - b); }	// a - b on the wrapping millisecond counter

// ---------------------------------------------------------------------------------------------------------------------------------------
// tile bookkeeping
// ---------------------------------------------------------------------------------------------------------------------------------------

int kvm_vtiles_init(kvm_vtiles *t, int rows, int cols)
{
	memset(t, 0, sizeof(*t));
	if (rows < 1 || cols < 1 || rows > 4096 || cols > 4096) { return -1; }
	t->st = (uint8_t *)calloc((size_t)rows * (size_t)cols, 1);
	t->fl = (uint8_t *)calloc((size_t)rows * (size_t)cols, 1);
	if (t->st == NULL || t->fl == NULL) { free(t->st); free(t->fl); memset(t, 0, sizeof(*t)); return -1; }
	t->rows = rows; t->cols = cols;
	return 0;
}

void kvm_vtiles_free(kvm_vtiles *t)
{
	free(t->st);
	free(t->fl);
	memset(t, 0, sizeof(*t));
}

void kvm_vtiles_reset(kvm_vtiles *t)
{
	if (t->st != NULL) { memset(t->st, 0, (size_t)t->rows * (size_t)t->cols); memset(t->fl, 0, (size_t)t->rows * (size_t)t->cols); }
	t->nPending = t->nVideo = t->nInflight = 0;
}

int kvm_vtiles_get(const kvm_vtiles *t, int row, int col)
{
	if (t->st == NULL || row < 0 || col < 0 || row >= t->rows || col >= t->cols) { return KVM_VT_NONE; }
	return t->st[(size_t)row * (size_t)t->cols + (size_t)col];
}

static void set_state(kvm_vtiles *t, size_t i, int to)
{
	int from = t->st[i];
	if (from == to) { return; }
	if (from == KVM_VT_PENDING) { t->nPending--; } else if (from == KVM_VT_VIDEO) { t->nVideo--; }
	if (to == KVM_VT_PENDING) { t->nPending++; } else if (to == KVM_VT_VIDEO) { t->nVideo++; }
	t->st[i] = (uint8_t)to;
}

void kvm_vtiles_claim(kvm_vtiles *t, int row0, int col0, int row1, int col1)
{
	int r, c;
	if (t->st == NULL) { return; }
	if (row0 < 0) { row0 = 0; }
	if (col0 < 0) { col0 = 0; }
	if (row1 >= t->rows) { row1 = t->rows - 1; }
	if (col1 >= t->cols) { col1 = t->cols - 1; }
	for (r = row0; r <= row1; r++)
	{
		for (c = col0; c <= col1; c++) { set_state(t, (size_t)r * (size_t)t->cols + (size_t)c, KVM_VT_PENDING); }
	}
}

void kvm_vtiles_jpeg(kvm_vtiles *t, int row, int col)
{
	if (t->st == NULL || row < 0 || col < 0 || row >= t->rows || col >= t->cols) { return; }
	if (t->fl[(size_t)row * (size_t)t->cols + (size_t)col] != 0) { t->jpegOnInflight++; }
	set_state(t, (size_t)row * (size_t)t->cols + (size_t)col, KVM_VT_NONE);
}

unsigned kvm_vtiles_take_pending(kvm_vtiles *t, uint8_t *map)
{
	size_t i, n = (size_t)t->rows * (size_t)t->cols;
	unsigned count = 0;
	if (t->st == NULL) { return 0; }
	memset(map, 0, n);
	for (i = 0; i < n; i++)
	{
		if (t->st[i] == KVM_VT_PENDING && t->fl[i] == 0) { map[i] = 1; count++; set_state(t, i, KVM_VT_NONE); }
	}
	return count;
}

unsigned kvm_vtiles_take_video(kvm_vtiles *t, uint8_t *map, int all)
{
	size_t i, n = (size_t)t->rows * (size_t)t->cols;
	unsigned count = 0;
	if (t->st == NULL) { return 0; }
	memset(map, 0, n);
	for (i = 0; i < n; i++)
	{
		if (t->fl[i] == 0 && (t->st[i] == KVM_VT_VIDEO || (all && t->st[i] == KVM_VT_PENDING))) { map[i] = 1; count++; set_state(t, i, KVM_VT_NONE); }
	}
	return count;
}

typedef struct trect { int c0, r0, c1, r1; } trect;

static long area(const trect *a) { return (long)(a->c1 - a->c0 + 1) * (long)(a->r1 - a->r0 + 1); }

static trect bbox(const trect *a, const trect *b)
{
	trect r;
	r.c0 = a->c0 < b->c0 ? a->c0 : b->c0; r.r0 = a->r0 < b->r0 ? a->r0 : b->r0;
	r.c1 = a->c1 > b->c1 ? a->c1 : b->c1; r.r1 = a->r1 > b->r1 ? a->r1 : b->r1;
	return r;
}

// One pass over the list in order: a rectangle is merged with the next one when the bounding box adds at most `slack` tiles. Returns the new count (in place).
static int merge_pass(trect *R, int n, long slack)
{
	int i, out = 0;
	trect cur;
	if (n < 2) { return n; }
	cur = R[0];
	for (i = 1; i < n; i++)
	{
		trect b = bbox(&cur, &R[i]);
		if (area(&b) - area(&cur) - area(&R[i]) <= slack) { cur = b; }
		else { R[out++] = cur; cur = R[i]; }
	}
	R[out++] = cur;
	return out;
}

int kvm_vtiles_make_rects(const kvm_vtiles *t, unsigned screenW, unsigned screenH, uint16_t rects[KVM_FRAME_MAX_RECTS * 4], uint8_t *cover)
{
	size_t total;
	trect *R = NULL;
	int *prev = NULL, *cur = NULL;
	int n = 0, r, c, k, i, nPrev = 0, nCur;
	long slack;
	if (t->st == NULL || cover == NULL || rects == NULL || screenW == 0 || screenH == 0) { return -1; }
	// the grid must be the tile grid of the screen (a tile that lies wholly outside the screen could not be given a rectangle)
	if ((unsigned)t->cols != (screenW + (KVM_VT_TILE - 1)) / KVM_VT_TILE || (unsigned)t->rows != (screenH + (KVM_VT_TILE - 1)) / KVM_VT_TILE) { return -1; }
	total = (size_t)t->rows * (size_t)t->cols;
	memset(cover, 0, total);
	if (t->nPending == 0) { return 0; }
	R = (trect *)malloc(sizeof(trect) * ((size_t)t->rows * (size_t)((t->cols + 1) / 2) + 1));
	prev = (int *)malloc(sizeof(int) * (size_t)((t->cols + 1) / 2 + 1));
	cur = (int *)malloc(sizeof(int) * (size_t)((t->cols + 1) / 2 + 1));
	if (R == NULL || prev == NULL || cur == NULL) { free(R); free(prev); free(cur); return -1; }

	// runs of pending tiles in a row; a run that has the same columns as a rectangle that ended in the row above extends it
	for (r = 0; r < t->rows; r++)
	{
		nCur = 0;
		c = 0;
		while (c < t->cols)
		{
			int c0, found = -1;
			if (t->st[(size_t)r * (size_t)t->cols + (size_t)c] != KVM_VT_PENDING) { c++; continue; }
			c0 = c;
			while (c < t->cols && t->st[(size_t)r * (size_t)t->cols + (size_t)c] == KVM_VT_PENDING) { c++; }
			for (k = 0; k < nPrev; k++)
			{
				if (R[prev[k]].c0 == c0 && R[prev[k]].c1 == c - 1) { found = prev[k]; break; }
			}
			if (found >= 0) { R[found].r1 = r; cur[nCur++] = found; }
			else { R[n].c0 = c0; R[n].c1 = c - 1; R[n].r0 = r; R[n].r1 = r; cur[nCur++] = n; n++; }
		}
		{ int *tmp = prev; prev = cur; cur = tmp; nPrev = nCur; }
	}

	// more than KVM_FRAME_MAX_RECTS: merge neighbours (in the order of the list) into bounding boxes, with a slack that doubles until it fits; the last resort is one box
	slack = 0;
	while (n > KVM_FRAME_MAX_RECTS)
	{
		n = merge_pass(R, n, slack);
		slack = slack == 0 ? 1 : slack * 2;
		if (slack > (long)total * 2) { slack = (long)total * 2; }
	}

	for (i = 0; i < n; i++)
	{
		unsigned x = (unsigned)R[i].c0 * KVM_VT_TILE, y = (unsigned)R[i].r0 * KVM_VT_TILE;
		unsigned x1 = ((unsigned)R[i].c1 + 1u) * KVM_VT_TILE, y1 = ((unsigned)R[i].r1 + 1u) * KVM_VT_TILE;
		if (x1 > screenW) { x1 = screenW; }
		if (y1 > screenH) { y1 = screenH; }
		rects[4 * i] = (uint16_t)x; rects[(4 * i) + 1] = (uint16_t)y; rects[(4 * i) + 2] = (uint16_t)(x1 - x); rects[(4 * i) + 3] = (uint16_t)(y1 - y);
		for (r = R[i].r0; r <= R[i].r1; r++)
		{
			for (c = R[i].c0; c <= R[i].c1; c++) { cover[(size_t)r * (size_t)t->cols + (size_t)c] = 1; }
		}
	}
	free(R); free(prev); free(cur);
	return n;
}

unsigned kvm_vtiles_commit(kvm_vtiles *t, const uint8_t *cover)
{
	size_t i, n = (size_t)t->rows * (size_t)t->cols;
	unsigned fresh = 0;
	if (t->st == NULL) { return 0; }
	for (i = 0; i < n; i++)
	{
		if (!cover[i]) { continue; }
		if (t->st[i] != KVM_VT_VIDEO) { fresh++; }
		set_state(t, i, KVM_VT_VIDEO);
	}
	return fresh;
}

// ---- INFLIGHT ----

void kvm_vtiles_dispatch(kvm_vtiles *t, const uint8_t *cover, int slot)
{
	size_t i, n = (size_t)t->rows * (size_t)t->cols;
	const uint8_t bit = (uint8_t)(1u << (unsigned)(slot & 7));
	if (t->st == NULL || cover == NULL || slot < 0 || slot > 7) { return; }
	for (i = 0; i < n; i++)
	{
		if (!cover[i]) { continue; }
		set_state(t, i, KVM_VT_VIDEO);
		if (t->fl[i] == 0) { t->nInflight++; }
		t->fl[i] |= bit;
	}
}

static void fl_clear(kvm_vtiles *t, size_t i, uint8_t bits)
{
	if (t->fl[i] == 0) { return; }
	t->fl[i] &= (uint8_t)~bits;
	if (t->fl[i] == 0) { t->nInflight--; }
}

unsigned kvm_vtiles_land(kvm_vtiles *t, int slot, uint8_t *map)
{
	size_t i, n = (size_t)t->rows * (size_t)t->cols;
	const uint8_t bit = (uint8_t)(1u << (unsigned)(slot & 7));
	unsigned count = 0;
	if (t->st == NULL || slot < 0 || slot > 7) { return 0; }
	if (map != NULL) { memset(map, 0, n); }
	for (i = 0; i < n; i++)
	{
		if (!(t->fl[i] & bit)) { continue; }
		if (map != NULL) { map[i] = 1; }
		count++;
		fl_clear(t, i, bit);
	}
	return count;
}

unsigned kvm_vtiles_abort(kvm_vtiles *t, int slot)
{
	size_t i, n = (size_t)t->rows * (size_t)t->cols;
	const uint8_t bit = (uint8_t)(1u << (unsigned)(slot & 7));
	unsigned count = 0;
	if (t->st == NULL || slot < 0 || slot > 7) { return 0; }
	for (i = 0; i < n; i++)
	{
		if (!(t->fl[i] & bit)) { continue; }
		count++;
		fl_clear(t, i, bit);
		if (t->st[i] == KVM_VT_VIDEO) { set_state(t, i, KVM_VT_PENDING); }
	}
	return count;
}

unsigned kvm_vtiles_cancel(kvm_vtiles *t, int slot, uint8_t *map)
{
	size_t i, n = (size_t)t->rows * (size_t)t->cols;
	const uint8_t bits = slot < 0 ? (uint8_t)0xFF : (uint8_t)(1u << (unsigned)(slot & 7));
	unsigned count = 0;
	if (t->st == NULL || slot > 7) { return 0; }
	for (i = 0; i < n; i++)
	{
		if (!(t->fl[i] & bits)) { continue; }
		fl_clear(t, i, bits);
		if (t->fl[i] != 0) { continue; }	// another frame in flight still covers the tile: it goes back to JPEG with that one
		if (map != NULL) { map[i] = 1; }
		count++;
		set_state(t, i, KVM_VT_NONE);
	}
	return count;
}

int kvm_vtiles_inflight(const kvm_vtiles *t, int row, int col)
{
	if (t->st == NULL || row < 0 || col < 0 || row >= t->rows || col >= t->cols) { return 0; }
	return t->fl[(size_t)row * (size_t)t->cols + (size_t)col] != 0 ? 1 : 0;
}

int kvm_vtiles_rect_inflight(const kvm_vtiles *t, int row0, int col0, int row1, int col1)
{
	int r, c;
	if (t->st == NULL || t->nInflight == 0) { return 0; }
	for (r = row0; r <= row1; r++) { for (c = col0; c <= col1; c++) { if (kvm_vtiles_inflight(t, r, c)) { return 1; } } }
	return 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// levels, profiles, quantizer, IDR spacing
// ---------------------------------------------------------------------------------------------------------------------------------------

// Table A-1 of the standard: level_idc, MaxMBPS (macroblocks per second), MaxFS (macroblocks per picture). Levels 4.0 and 4.1 have the same limits (as in mf_h264.cpp).
static const struct { int level; unsigned mbps, fs; } kLevels[] = {
	{ 30, 40500, 1620 }, { 31, 108000, 3600 }, { 32, 216000, 5120 }, { 40, 245760, 8192 }, { 42, 522240, 8704 }, { 50, 589824, 22080 }, { 51, 983040, 36864 }, { 52, 2073600, 36864 } };
#define NLEVELS ((int)(sizeof(kLevels) / sizeof(kLevels[0])))

static int level_index(unsigned mbs, unsigned fps)
{
	int i;
	for (i = 0; i < NLEVELS; i++)
	{
		if (mbs <= kLevels[i].fs && (uint64_t)mbs * fps <= kLevels[i].mbps) { return i; }
	}
	return -1;
}

int kvm_vcore_pick_level(unsigned codedW, unsigned codedH, int viewerMaxLevel, int *level, int *fpsCap)
{
	unsigned mbs;
	int maxL = viewerMaxLevel > 0 ? viewerMaxLevel : 52, i;
	if (codedW < 16 || codedH < 16 || codedW > KVM_FRAME_MAX_DIM || codedH > KVM_FRAME_MAX_DIM) { return -1; }
	mbs = ((codedW + 15u) / 16u) * ((codedH + 15u) / 16u);
	if (mbs > kLevels[NLEVELS - 1].fs) { return -1; }
	i = level_index(mbs, KVM_VIDEO_MAX_FPS);
	if (i >= 0 && kLevels[i].level <= maxL)
	{
		if (level != NULL) { *level = kLevels[i].level; }
		if (fpsCap != NULL) { *fpsCap = KVM_VIDEO_MAX_FPS; }
		return 0;
	}
	i = level_index(mbs, 30);
	if (i >= 0 && kLevels[i].level <= maxL)
	{
		unsigned fps = kLevels[i].mbps / mbs;
		if (fps > (unsigned)KVM_VIDEO_MAX_FPS) { fps = KVM_VIDEO_MAX_FPS; }
		if (level != NULL) { *level = kLevels[i].level; }
		if (fpsCap != NULL) { *fpsCap = (int)fps; }
		return 0;
	}
	return -1;
}

int kvm_vcore_pick_profile(unsigned flags, int below)
{
	if ((flags & KVM_PROFILE_HIGH) && 100 < below) { return 100; }
	if ((flags & KVM_PROFILE_MAIN) && 77 < below) { return 77; }
	if ((flags & KVM_PROFILE_CBP) && 66 < below) { return 66; }
	return 0;
}

int kvm_vcore_clamp_qp(int qp)
{
	if (qp < KVM_VIDEO_QP_MIN) { return KVM_VIDEO_QP_MIN; }
	if (qp > KVM_VIDEO_QP_MAX) { return KVM_VIDEO_QP_MAX; }
	return qp;
}

int kvm_vcore_hw_qp(int regulatorQp)
{
	int q = regulatorQp + KVM_HW_QP_OFFSET;
	if (q < KVM_HW_QP_FLOOR) { q = KVM_HW_QP_FLOOR; }
	if (q > 51) { q = 51; }
	return q;
}

int kvm_vcore_scale_allowed(int videoActive, int down, uint32_t msSinceGen)
{
	if (!videoActive) { return 1; }
	if (down) { return 0; }
	return msSinceGen >= (uint32_t)KVM_VIDEO_SCALE_RETURN_MS;
}

uint32_t kvm_vcore_idr_spacing(uint32_t lastIdrBytes, uint64_t rate)
{
	uint64_t sp = KVM_VIDEO_IDR_MIN_MS;
	if (rate > 0)
	{
		uint64_t t = (2ull * (uint64_t)lastIdrBytes * 1000ull) / rate;
		if (t > sp) { sp = t; }
	}
	if (sp > KVM_VIDEO_IDR_MAX_MS) { sp = KVM_VIDEO_IDR_MAX_MS; }
	return (uint32_t)sp;
}

int kvm_vcore_check_sps(const struct kvm_sps_info *s, unsigned profFlags, int viewerMaxLevel, unsigned codedW, unsigned codedH)
{
	unsigned flag = s->profile == 66 ? KVM_PROFILE_CBP : (s->profile == 77 ? KVM_PROFILE_MAIN : (s->profile == 100 ? KVM_PROFILE_HIGH : 0u));
	int maxL = viewerMaxLevel > 0 ? viewerMaxLevel : 52;
	if (flag == 0 || (profFlags & flag) == 0) { return KVM_REASON_PROFILE; }
	if (s->profile == 66 && (s->constraint & 0x40) == 0) { return KVM_REASON_PROFILE; }	// Baseline that is not Constrained Baseline: the viewer rejects it
	if (s->numRefFrames > 1 || s->chromaFormat != 1 || s->bitDepthLuma != 8 || !s->frameMbsOnly) { return KVM_REASON_PROFILE; }
	if (s->level > maxL || s->level > 52 || s->level < 10) { return KVM_REASON_RESOLUTION; }
	if (s->cropping || s->mbsW < 1 || s->mbsH < 1 || s->mbsW > 8192 / 16 || s->mbsH > 8192 / 16 || (long)s->mbsW * (long)s->mbsH > 36864) { return KVM_REASON_RESOLUTION; }
	if ((unsigned)s->mbsW * 16u != codedW || (unsigned)s->mbsH * 16u != codedH) { return KVM_REASON_RESOLUTION; }
	return 0;
}

int kvm_vcore_force_sharpen(uint64_t rate, unsigned budgetKbps, uint64_t inflight, uint64_t window, int haveLast, uint32_t lastAt, uint32_t now, uint32_t *bytes)
{
	uint64_t share, b, interval;
	if (window > 0 && inflight >= 2u * window) { return 0; }
	if (budgetKbps > 0 && (uint64_t)budgetKbps * 125u < rate) { rate = (uint64_t)budgetKbps * 125u; }	// the budget of the viewer is the limit of the whole stream, not only of the video
	share = (rate * (uint64_t)KVM_VIDEO_SHARPEN_SHARE_PCT) / 100u;	// bytes per second that sharpening may use
	b = (share * (uint64_t)KVM_VIDEO_SHARPEN_FORCE_MS) / 1000u;
	if (b < KVM_VIDEO_SHARPEN_FORCE_MIN) { b = KVM_VIDEO_SHARPEN_FORCE_MIN; }
	if (b > KVM_VIDEO_SHARPEN_FORCE_MAX) { b = KVM_VIDEO_SHARPEN_FORCE_MAX; }
	interval = share > 0 ? (b * 1000u) / share : 1000u;
	if (interval < KVM_VIDEO_SHARPEN_FORCE_MS) { interval = KVM_VIDEO_SHARPEN_FORCE_MS; }
	if (interval > 10000u) { interval = 10000u; }
	if (haveLast && dms(now, lastAt) < (int32_t)interval) { return 0; }
	if (bytes != NULL) { *bytes = (uint32_t)b; }
	return 1;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the controller
// ---------------------------------------------------------------------------------------------------------------------------------------

void kvm_vctl_init(kvm_vctl *c, int capVideo)
{
	memset(c, 0, sizeof(*c));
	c->capVideo = (uint8_t)(capVideo ? 1 : 0);
}

void kvm_vctl_limits(kvm_vctl *c, const kvm_ctrl *op)
{
	c->profFlags = (uint8_t)(op->flags & 7);
	c->maxLevel = op->maxLevelIdc;
	c->maxW = op->maxW; c->maxH = op->maxH;
	c->nxValid = 0;
}

void kvm_vctl_apply_limits(kvm_vctl *c)
{
	if (!c->nxValid) { return; }
	c->profFlags = c->nxFlags; c->maxLevel = c->nxLevel; c->maxW = c->nxW; c->maxH = c->nxH;
	c->nxValid = 0;
}

int kvm_vctl_op1(kvm_vctl *c, const kvm_ctrl *op, uint32_t now)
{
	// joined only with a start that succeeded or is under way; after a failed one (state OFF) the viewer's new request is a new attempt
	if (c->haveOp1 && c->state != KVM_VS_OFF && dms(now, c->lastOp1) < (int32_t)KVM_VIDEO_OP1_COALESCE_MS)
	{
		// the limits in force belong to the generation that runs (its SPS was checked against them): the new ones wait for the next generation, where they are used
		c->nxFlags = (uint8_t)(op->flags & 7); c->nxLevel = op->maxLevelIdc; c->nxW = op->maxW; c->nxH = op->maxH; c->nxValid = 1;
		c->coalesced++;
		return KVM_VOP1_COALESCE;
	}
	kvm_vctl_limits(c, op);
	c->lastOp1 = now; c->haveOp1 = 1;
	return KVM_VOP1_START;
}

int kvm_vctl_pace_ok(const kvm_vctl *c, uint32_t now, uint32_t intervalMs)
{
	uint32_t early;
	if (!c->haveFrame || intervalMs == 0) { return 1; }
	early = (intervalMs * (uint32_t)KVM_VIDEO_EARLY_PCT) / 100u;
	if (dms(now, c->lastFrame) < (int32_t)(intervalMs - early)) { return 0; }
	return kvm_vctl_write_wait_ms(c, now, intervalMs) == 0;
}

void kvm_vctl_note_write(kvm_vctl *c, uint32_t writeAt, uint32_t encMs)
{
	c->haveWrite = 1; c->lastWrite = writeAt; c->encMs = encMs;
}

void kvm_vctl_set_threaded(kvm_vctl *c, int threaded) { c->encThreaded = (uint8_t)(threaded ? 1 : 0); }

uint32_t kvm_vctl_write_wait_ms(const kvm_vctl *c, uint32_t now, uint32_t intervalMs)
{
	uint32_t gap, enc;
	int32_t wait;
	if (!c->haveWrite || intervalMs == 0) { return 0; }
	gap = (uint32_t)(((uint64_t)intervalMs * KVM_VIDEO_WRITE_GAP_PCT) / 100u);
	// The encode time to expect is the average, but not more than half the interval: an average that the occasional slow frame has raised (30 ms among 8 ms ones) would predict a write far behind the start
	// and the rule would allow the frame right behind a late one; and a slow encoder does not hold the frames back by itself (the interval of the CPU budget is longer than its encode time anyway)
	enc = c->encMs > intervalMs / 2u && !c->encThreaded ? intervalMs / 2u : c->encMs;
	if (enc > intervalMs) { enc = intervalMs; }
	wait = dms(c->lastWrite + gap, now + enc);
	return wait > 0 ? (uint32_t)wait : 0u;
}

void kvm_vctl_op3(kvm_vctl *c, unsigned actions)
{
	if (actions & KVM_ACT_IDR) { c->idrReq = 1; }
	if (actions & KVM_ACT_REPAIR) { c->repairReq = 1; }
}

int kvm_vctl_open_allowed(const kvm_vctl *c, uint32_t now)
{
	return !c->haveOpen || dms(now, c->lastOpen) >= (int32_t)KVM_VIDEO_OPEN_MIN_MS;
}

void kvm_vctl_open_note(kvm_vctl *c, uint32_t now) { c->haveOpen = 1; c->lastOpen = now; }

int kvm_vctl_begin_gen(kvm_vctl *c, kvm_stream *s, int profile, int level, unsigned codedW, unsigned codedH, int encFresh, kvm_state_msg *m)
{
	if (!s->ackActive || !c->capVideo) { return -1; }	// a 242 with video 1 and ack 0 does not exist (3.3)
	kvm_stream_bump_gen(s);
	s->videoOn = 1;
	c->state = KVM_VS_ON;
	c->haveWrite = 0;	// the first frame of a generation waits for nothing
	c->idrFirst = 1; c->idrForce = 0; c->idrReq = 0;
	c->encFresh = (uint8_t)(encFresh ? 1 : 0);
	c->brokenRun = 0; c->inError = 0;
	if (m != NULL)
	{
		kvm_stream_state_msg(s, KVM_REASON_OK, m);
		m->profile = (uint8_t)profile; m->level = (uint8_t)level; m->codedW = (uint16_t)codedW; m->codedH = (uint16_t)codedH;
	}
	return 0;
}

void kvm_vctl_stop(kvm_vctl *c)
{
	c->state = KVM_VS_OFF;
	c->idrFirst = c->idrForce = c->idrReq = c->encFresh = 0;
	c->repairReq = 0;
}

int kvm_vctl_error(kvm_vctl *c, uint32_t now)
{
	kvm_vctl_stop(c);
	c->errors++;
	if (c->errorCount < 255) { c->errorCount++; }
	if (c->errorCount >= KVM_VIDEO_MAX_ERRORS) { c->capVideo = 0; c->inError = 0; return 0; }
	c->inError = 1;
	c->errorUntil = now + (uint32_t)KVM_VIDEO_ERROR_RETRY_MS;
	return 1;
}

int kvm_vctl_retry_due(kvm_vctl *c, uint32_t now)
{
	if (!c->inError || !c->capVideo || dms(now, c->errorUntil) < 0) { return 0; }
	c->inError = 0;
	return 1;
}

void kvm_vctl_fault(kvm_vctl *c)
{
	kvm_vctl_stop(c);
	c->faulted = 1; c->capVideo = 0; c->inError = 0;
}

void kvm_vctl_plan(const kvm_vctl *c, const kvm_stream *s, const kvm_vplan_in *in, kvm_vplan *out)
{
	memset(out, 0, sizeof(*out));
	if (!c->capVideo || c->state != KVM_VS_ON || !s->videoOn) { return; }
	if (in->pending == 0) { return; }	// lazy: with nothing in motion nothing is encoded, not even a key frame that was asked for
	if (!kvm_vctl_pace_ok(c, in->now, in->intervalMs)) { return; }
	if (!in->gateOpen) { out->held = 1; return; }
	if (!in->rateOpen && !c->idrFirst && !c->idrForce) { out->heldRate = 1; return; }
	out->encode = 1;
	if (c->idrFirst) { out->forceIdr = c->encFresh ? 0 : 1; }
	if (c->idrForce) { out->forceIdr = 1; }
	else if (c->idrReq && (!c->haveIdr || dms(in->now, c->lastIdr) >= (int32_t)kvm_vcore_idr_spacing(c->lastIdrBytes, in->rate))) { out->forceIdr = 1; }
}

int kvm_vctl_frame_done(kvm_vctl *c, int isIdr, size_t bytes, uint32_t now)
{
	if (c->idrFirst && !isIdr)
	{
		// the first frame of a generation has to be an IDR; the viewer would drop everything until one comes
		c->brokenFrames++;
		c->idrForce = 1; c->encFresh = 0;
		if (c->brokenRun < 255) { c->brokenRun++; }
		return c->brokenRun >= KVM_VIDEO_BROKEN_MAX ? KVM_VF_ERROR : KVM_VF_BROKEN;
	}
	c->brokenRun = 0;
	c->frames++;
	c->haveFrame = 1; c->lastFrame = now;
	if (isIdr)
	{
		c->idrs++;
		if (c->idrForce) { c->forcedIdrs++; }
		c->haveIdr = 1; c->lastIdr = now; c->lastIdrBytes = bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)bytes;
		c->idrFirst = c->idrForce = c->idrReq = c->encFresh = 0;
	}
	return 0;
}

uint32_t kvm_vcore_max_bitrate(int levelIdc, int profileIdc)
{
	uint32_t kbit;
	switch (levelIdc)
	{
		case 31: kbit = 14000; break;
		case 32: case 40: kbit = 20000; break;
		case 41: case 42: kbit = 50000; break;
		case 50: kbit = 135000; break;
		case 51: case 52: kbit = 240000; break;
		default: kbit = 10000; break;
	}
	return profileIdc == 100 ? kbit * 1250u : kbit * 1000u;
}

int kvm_vctl_idr_why(const kvm_vctl *c, const kvm_vplan *plan)
{
	if (c->idrForce) { return KVM_IDR_WHY_BROKEN; }
	if (c->idrFirst) { return KVM_IDR_WHY_FIRST; }
	if (plan != NULL && plan->forceIdr && c->idrReq) { return KVM_IDR_WHY_OP3; }
	return KVM_IDR_WHY_NONE;
}

const char *kvm_idr_why_name(int why, int isIdr)
{
	if (!isIdr) { return "none"; }
	switch (why) { case KVM_IDR_WHY_FIRST: return "first"; case KVM_IDR_WHY_BROKEN: return "broken"; case KVM_IDR_WHY_OP3: return "op3"; }
	return "encoder";
}

int kvm_vctl_frame_broken(kvm_vctl *c, int oversize)
{
	c->brokenFrames++;
	c->idrForce = 1; c->encFresh = 0;
	if (c->brokenRun < 255) { c->brokenRun++; }
	return (oversize || c->brokenRun >= KVM_VIDEO_BROKEN_MAX) ? KVM_VB_ERROR : KVM_VB_RETRY;
}

int kvm_vctl_repair_due(kvm_vctl *c, uint32_t now)
{
	if (!c->repairReq) { return 0; }
	if (c->haveRepair && dms(now, c->lastRepair) < (int32_t)KVM_VIDEO_REPAIR_MIN_MS) { return 0; }
	c->repairReq = 0; c->haveRepair = 1; c->lastRepair = now; c->repairs++;
	return 1;
}

void kvm_vctl_repair_now(kvm_vctl *c, uint32_t now)
{
	c->repairReq = 0; c->haveRepair = 1; c->lastRepair = now; c->repairs++;
}
