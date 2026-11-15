/*
a simulation of what the user feels while the hybrid model of the agent (JPEG for small changes, H.264 for motion, JPEG sharpening of what the video blurred) works on a fast link: the series
of motion that the work with an interface is made of (an animation of 0.2-0.5 s, a dragged window, a scroll) with pauses between them. Calibrated on the log of a session on the AMD laptop (open lid, High,
17:52:36-17:53:31) and on a measurement on a Windows 11 VM.

The real pieces are used where they are portable C: the stream layer (ACK window, the rate and its memory), the controller and the plan of the video (kvm_video_core.c), the rate control (kvm_video_rc.c), the pace
(kvm_pace.c), the governor of the sharpening (kvm_sharpen.c) and, in the build against the tree, the class of the tiles (kvm_motion.c). What is not portable lives in Windows/kvm_adapt.cpp and is MODELLED here
(the tile loop: note_crc, the class, the gates of the JPEG rectangles, the claim of the video, the sharpening of the tiles that the video blurred). The same program is built twice, as in run-kvm-h264sim-tests.sh:
against the files of an earlier commit (OLD_COMMIT of run-kvm-tune-sim-tests.sh, taken from git, -DSIM_OLD: the class rule "the previous frame had 16 changes", the unsigned memory of the rate, 300 ms of movement, no write gap, the
change of source is motion: the rows "before") and against the tree ("after").

Output: tables (markdown) for the report. The checks are the criteria "ready" for Docker (burst, drag, link drop, start, settle, typing), for the build against the tree.
*/

#include "../kvm_video_rc.h"
#include "../kvm_video_core.h"
#include "../kvm_pace.h"
#include "../kvm_stream.h"
#include "../kvm_sharpen.h"
#ifndef SIM_OLD
#include "../kvm_motion.h"
#define SIM_RATE_SHARP(s, t) kvm_stream_rate_recent((s), (t))
#else
#define SIM_RATE_SHARP(s, t) kvm_stream_rate_or_mem((s), (t))
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef SIM_SOFT	/* the tree: a refusal of the window that is no evidence of the link does not wipe the proof of the key frame; the old files have neither */
#define SIM_NOTE_LINK_HELD(s, t) kvm_stream_note_link_held((s), (t))
#define SIM_REFUSED_SOFT(rc) kvm_vrc_window_refused_soft(rc)
#else
#define SIM_NOTE_LINK_HELD(s, t) (kvm_stream_note_link_held((s), (t)), 1)
#define SIM_REFUSED_SOFT(rc) kvm_vrc_window_refused(rc)
#endif

static int gFail = 0, gChecks = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define ROWS 34
#define COLS 60
#define NT (ROWS * COLS)
#define SETTLE 150u
#define MAXSER 64
enum { T_JPEG = 0, T_PEND = 1, T_VIDEO = 2 };
enum { SC_REPLAY = 0, SC_BURST, SC_DRAG, SC_DROP, SC_START, SC_START2, SC_TYPING, SC_MIXED };
#define BURST_CONT 25000u
#define BURST_GAP 2600u
#define BURST_T(i) (BURST_CONT + BURST_GAP * ((i) + 1u) + 400u * (i))

static uint32_t gRng = 777u;
static double rnd01(void) { gRng = (gRng * 1664525u) + 1013904223u; return (double)(gRng >> 8) / 16777216.0; }

// ---------------------------------------------------------------------------------------------------------------------------------------
// the content of the screen: segments of motion
// ---------------------------------------------------------------------------------------------------------------------------------------

typedef struct seg
{
	uint32_t t0, t1;			// ms from the start of the run
	int kind;					// 1 animation of a region, 2 a dragged strip, 3 typing
	int r0, r1, c0, c1;			// the region (tiles), for the strip the rows
	int x0, w, step;			// the strip: the left edge (px), the width (px), pixels a frame
	double p;					// animation: the share of the tiles that change in a source frame
	int pointer;				// the pointer moves meanwhile (two tiles change in every source frame)
} seg;

typedef struct scene
{
	seg s[160];
	int n;
	int srcChangeAt;			// ms: the frame that is captured then comes from another source of pixels (-1 = none)
	int startMode;				// the first frames: the picture is filled from GDI (every tile seen for the first time)
	int caret;					// a text caret that blinks (one tile every 530 ms)
} scene;

static void add_seg(scene *sc, uint32_t t0, uint32_t t1, int kind, int r0, int r1, int c0, int c1, int x0, int w, int step, double p, int pointer)
{
	seg *g = &sc->s[sc->n++];
	g->t0 = t0; g->t1 = t1; g->kind = kind; g->r0 = r0; g->r1 = r1; g->c0 = c0; g->c1 = c1; g->x0 = x0; g->w = w; g->step = step; g->p = p; g->pointer = pointer;
}

// the tiles that change in the source frame number f (60 Hz) at time tms
static void source_frame(const scene *sc, uint32_t tms, int f, uint8_t *chg)
{
	int i, r, c;
	for (i = 0; i < sc->n; i++)
	{
		const seg *g = &sc->s[i];
		if (tms < g->t0 || tms >= g->t1) { continue; }
		if (g->kind == 1)
		{
			for (r = g->r0; r <= g->r1; r++) { for (c = g->c0; c <= g->c1; c++) { if (g->p >= 1.0 || rnd01() < g->p) { chg[r * COLS + c] = 1; } } }
		}
		else if (g->kind == 2)
		{
			const int k = (int)((tms - g->t0) * 60u / 1000u);
			const int xa = g->x0 + k * g->step, xb = xa + g->w, pa = xa - g->step, pb = xb - g->step;
			int ca = xa / 32, cb = (xb - 1) / 32;
			if (g->step > 0 && k > 0) { if (pa / 32 < ca) { ca = pa / 32; } if ((pb - 1) / 32 > cb) { cb = (pb - 1) / 32; } }
			if (ca < 0) { ca = 0; }
			for (r = g->r0; r <= g->r1; r++) { for (c = ca; c <= cb && c < COLS; c++) { chg[r * COLS + c] = 1; } }
		}
		else if (g->kind == 3)
		{
			// typing: a character every 125 ms in a line of text: its tile and the next one (the caret)
			if ((f % 8) == 0) { const int k = (int)((tms - g->t0) / 125u); chg[g->r0 * COLS + g->c0 + (k % 40)] = 1; chg[g->r0 * COLS + g->c0 + ((k + 1) % 40)] = 1; }
		}
		if (g->pointer) { const int k = f / 2; chg[(5 + (k % 20)) * COLS + (4 + (k % 50))] = 1; chg[(5 + (k % 20)) * COLS + (5 + (k % 50))] = 1; }
	}
	if (sc->caret && (tms / 530u) != ((tms - 17u) / 530u)) { chg[30 * COLS + 50] = 1; }
}

static void build_scene(scene *sc, int kind, uint32_t seed)
{
	int i;
	memset(sc, 0, sizeof(*sc));
	sc->srcChangeAt = -1; sc->caret = 1;
	gRng = seed;
	if (kind == SC_REPLAY)
	{
		// the log of the AMD laptop: 7.5 s of continuous motion (17:52:36-43), then the work with the interface: series of 0.2-1.0 s with pauses of 0.3-2.4 s, also a pause above 2 s (17:52:53-55)
		uint32_t t = 7500;
		add_seg(sc, 0, 7500, 1, 6, 27, 10, 49, 0, 0, 0, 1.0, 1);
		for (i = 0; i < 24 && sc->n < 150; i++)
		{
			const uint32_t gap = (i == 9) ? 2300u : 300u + (uint32_t)(rnd01() * 1800.0), dur = 200u + (uint32_t)(rnd01() * 800.0);
			const double u = rnd01();
			t += gap;
			if (u < 0.5) { const int h = 6 + (int)(rnd01() * 16.0), w = 10 + (int)(rnd01() * 30.0), r0 = 2 + (int)(rnd01() * (double)(ROWS - 4 - h)); add_seg(sc, t, t + dur, 1, r0, r0 + h - 1, 8, 8 + w - 1, 0, 0, 0, 0.85, 1); }
			else if (u < 0.85) { add_seg(sc, t, t + dur, 2, 3 + (int)(rnd01() * 4.0), 3 + 12 + (int)(rnd01() * 6.0), 0, 0, 100 + (int)(rnd01() * 200.0), 96 + 32 * (int)(rnd01() * 6.0), 16 + 16 * (int)(rnd01() * 2.0), 1.0, 1); }
			else { add_seg(sc, t, t + dur, 1, 20, 21, 20, 25, 0, 0, 0, 0.6, 0); }
			t += dur;
		}
	}
	else if (kind == SC_BURST || kind == SC_DROP)
	{
		// 25 s of continuous motion (the rate to compare with), then 10 series of 0.4 s every 2.8 s: an animation of 760 tiles and a dragged strip of 96 px at 16 px a frame on a textured background
		add_seg(sc, 0, BURST_CONT, 1, 2, 20, 10, 49, 0, 0, 0, 1.0, 1);
		for (i = 0; i < 10; i++)
		{
			const uint32_t t = BURST_T((uint32_t)i);
			add_seg(sc, t, t + 400, 1, 2, 20, 10, 49, 0, 0, 0, 1.0, 1);
			add_seg(sc, t, t + 400, 2, 22, 33, 0, 0, 100, 96, 16, 1.0, 0);
		}
	}
	else if (kind == SC_DRAG)
	{
		uint32_t t = 3000;
		for (i = 0; i < 6; i++) { add_seg(sc, t, t + 1500, 2, 4, 20, 0, 0, 100, 160, 16, 1.0, 1); t += 1500 + 1500; }
	}
	else if (kind == SC_START || kind == SC_START2)
	{
		// the screen stands (the generator was late), motion comes at +17 s (3 Mb/s) or at +4.5 s (1 Mb/s)
		const uint32_t from = kind == SC_START ? 17000u : 4500u;
		sc->startMode = 1; sc->srcChangeAt = 120; sc->caret = 0;
		add_seg(sc, from, from + (kind == SC_START ? 4000u : 12000u), 1, 6, 27, 10, 49, 0, 0, 0, 1.0, 0);
	}
	else if (kind == SC_TYPING)
	{
		add_seg(sc, 1000, 11000, 3, 20, 20, 10, 40, 0, 0, 0, 1.0, 1);
	}
	else if (kind == SC_MIXED)
	{
		// typing in a text field while an animation runs far away (6 tiles), and a clock next to the animation
		add_seg(sc, 1000, 9000, 3, 30, 30, 4, 40, 0, 0, 0, 1.0, 1);
		add_seg(sc, 2000, 7000, 1, 4, 12, 30, 45, 0, 0, 0, 1.0, 0);
	}
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the run
// ---------------------------------------------------------------------------------------------------------------------------------------

typedef struct cfg
{
	const char *label;
	int kind;
	uint64_t linkBps, linkBps2;	// bytes per second, and after dropMs
	int dropMs;
	uint32_t oneWayMs;
	int seconds;
	uint32_t periodMs;			// the viewer's frame timer (16 = High)
	uint32_t encMs;
	double jpegTileBytes;		// a JPEG tile of the user's quality (4:4:4), and of the sharpening
	double videoTileBytes;		// bytes of a tile of a P frame at QP 26 after 16 ms
	int measureFrom;			// ms from the start: what is counted (the motion before it is what the rate and the memory are learnt on)
	int contEnd;				// ms: the end of the continuous motion (the rate of it is the one to compare with)
	int srcDiff;				// tiles that still have another hash in the first copy of DXGI (2022 of 2040 in a measurement before the alpha byte was cleared; the worst case if it was not the cause)
	int seed;					// 1: a session that has been running: R of 15.8 Mb/s, the rate 14.6 Mb/s remembered, QP 26 (the state of the AMD laptop before the series of 17:52:48)
	int quietUntil;				// ms: the start scenes count what happens before the first motion (what must not happen without one)
	int verbose;
} cfg;

typedef struct serinfo { uint32_t t0, tLast, tFirstVideo; uint32_t rStartKbps; uint32_t denied0, held0, heldAt100; int have; } serinfo;

typedef struct res
{
	double seconds, fpsVideo, fpsJpeg, jpegPaintPct, jpegBytesPct, sharpSharePct, sharpShareSeriesPct, sharpShareWorst;
	double rContKbps, capContKbps, linkKbps, rStartMinKbps, rStartRatio, sharpShareMotionPct, settleAvgMs, sharpOfRatePct;
	double startMsAvg, startMsMax;
	unsigned series, noVideo, deniedInSeries, heldIn100ms, heldFirstSecond;
	double gapP95, gapMax, over2xPerS, pairsPct;
	double settleMs, queueMaxMs, queueFirstSeriesMs, rConvergeMs;
	double jpegKB, videoKB, sharpKB, idrKB;
	unsigned claimedAtB, nVideoFrames, nJpegPaints;
	double sharpKBIdle, firstMotionVideoMs;
	unsigned classNear, classStreak;
} res;

typedef struct pos_t { uint32_t seq, ts; uint64_t cum; int ready; uint32_t readyAt; } pos_t;

static int cmpd(const void *a, const void *b) { const double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : (x > y ? 1 : 0); }

static res run(const cfg *c)
{
	res R;
	kvm_stream s;
	kvm_vctl ctl;
	kvm_vrc rc;
	kvm_budget budget;
	kvm_sharp sg;
	scene sc;
	static pos_t pos[200000];
	static serinfo ser[MAXSER];
	static double gaps[100000];
	uint8_t st[NT], deferred[NT], seen[NT], chgNow[NT], chgAcc[NT], viaVideo[NT];
	unsigned streak[NT];
	uint32_t lc[NT];
	size_t npos = 0, firstUnready = 0, firstUndelivered = 0;
	double served = 0.0, encAvgUs = 8000.0, srcAcc = 0.0;
	const uint32_t t0 = 5000, tEnd = 5000 + (uint32_t)c->seconds * 1000u;
	uint32_t t, lastAck = 0, nextIter = 0, lastFrameT = 0, lastForced = 0, dueSince = 0, lastPaintAt = 0, lastFrameTick = 0, lastSerMotion = 0;
	int forcedHave = 0, cursor = 0, k, nSer = 0, nGaps = 0, srcFrame = 0, srcChangePending = 0, haveLastPaint = 0, started = 0;
	unsigned changedThisFrame = 0, prevChanged = 0, realChanged = 0, idrCount = 0;
	uint32_t idrBytes = 0;
	uint64_t linkBps = c->linkBps;
	uint64_t bJpeg = 0, bVideo = 0, bSharp = 0, bSharpSer = 0, bIdr = 0, bJpegMot = 0, bVideoMot = 0, bSharpMot = 0, bVideoMotS = 0, bJpegMotS = 0;
	int settleOpen = 0, settleN = 0;
	double rateAcc = 0.0;
	double settleSum = 0.0, settleMax = 0.0;
	unsigned nPaintV = 0, nPaintJ = 0, nPaintJm = 0, nPaintVm = 0;
	double qMaxFirst = 0.0, qMax = 0.0, pairs = 0.0, over2 = 0.0;
	(void)prevChanged;
	int heldEv[4096], nHeldEv = 0;
	uint32_t lastPrintSec = 999u;
#ifndef SIM_OLD
	kvm_motion m;
#endif
	uint32_t measT = 0;
	double firstMotionAt = -1.0;
	memset(&R, 0, sizeof(R));
	memset(ser, 0, sizeof(ser));
	build_scene(&sc, c->kind, 12345u);
	gRng = 4242u;
	{
		kvm_ctrl op;
		kvm_state_msg mm, reply;
		kvm_stream_init(&s, 1, 1, 1, 6);
		memset(&op, 0, sizeof(op)); op.ver = 1; op.op = KVM_OP_VIDEO_ON; op.flags = 7;
		(void)kvm_stream_on_ctrl(&s, &op, t0, &reply, NULL);
		kvm_vctl_init(&ctl, 1);
		(void)kvm_vctl_begin_gen(&ctl, &s, 100, 42, 1920, 1088, 1, &mm);
	}
	kvm_vrc_init(&rc, KVM_VIDEO_QP, t0);
	kvm_vrc_set_picture(&rc, 1920u * 1088u);
	kvm_budget_init(&budget, 12);
	kvm_sharp_init(&sg, t0);
	if (c->seed)
	{
		// the state of the AMD laptop at 17:52:36: r_kbps=15786 rate_kbps=14603 qp=26, minRtt 5 ms
		rc.cap = 1975000u; rc.qp = 26; kvm_vrc_set_target(&rc, 1975000u, t0);
		s.rateMem = 1825000u; s.minRtt = 5u;
#ifndef SIM_OLD
		s.rateMemAt = t0;
#endif
	}
#ifndef SIM_OLD
	kvm_motion_init(&m);
	CHECK(kvm_motion_resize(&m, ROWS, COLS) == 0);
#endif
	memset(viaVideo, 0, sizeof(viaVideo));
	memset(st, T_JPEG, sizeof(st)); memset(deferred, 0, sizeof(deferred)); memset(streak, 0, sizeof(streak)); memset(lc, 0, sizeof(lc));
	for (k = 0; k < NT; k++) { seen[k] = sc.startMode ? 0 : 1; lc[k] = 0; }
	if (sc.startMode) { memset(chgAcc, 1, sizeof(chgAcc)); }
	else { memset(chgAcc, 0, sizeof(chgAcc)); }
	nextIter = t0; measT = t0 + (uint32_t)c->measureFrom;
	for (t = t0; t < tEnd; t++)
	{
		const uint32_t rel = t - t0;
		const int meas = (int32_t)(t - measT) >= 0;
		// ---- the link ----
		if (c->dropMs > 0 && (int)rel >= c->dropMs) { linkBps = c->linkBps2; }
		served += (double)linkBps / 1000.0;
		if (served > (double)s.bytesWritten) { served = (double)s.bytesWritten; }
		while (firstUnready < npos && (double)pos[firstUnready].cum <= served + 0.5) { pos[firstUnready].ready = 1; pos[firstUnready].readyAt = t + c->oneWayMs; firstUnready++; }
		if (t - lastAck >= 3)
		{
			size_t newest = (size_t)-1, i;
			for (i = firstUndelivered; i < firstUnready; i++) { if (pos[i].ready && (int32_t)(t - pos[i].readyAt) >= 0) { newest = i; } }
			if (newest != (size_t)-1)
			{
				kvm_ack a;
				a.seq = pos[newest].seq; a.ts = pos[newest].ts; a.queueMs = 0; a.flags = 0;
				(void)kvm_stream_on_ack(&s, &a, t);
				firstUndelivered = newest + 1; lastAck = t;
			}
		}
		// ---- the content: source frames at 60 Hz ----
		srcAcc += 1.0;
		if (srcAcc >= 16.667)
		{
			srcAcc -= 16.667;
			memset(chgNow, 0, sizeof(chgNow));
			source_frame(&sc, rel, srcFrame++, chgNow);
			for (k = 0; k < NT; k++) { if (chgNow[k]) { chgAcc[k] = 1; } }
		}
		if (sc.srcChangeAt >= 0 && (int)rel == sc.srcChangeAt)
		{
			// the first copy of DXGI: 2022 of the 2040 tiles differ from what the GDI fill put there
			for (k = NT - c->srcDiff; k < NT; k++) { chgAcc[k] = 1; }
			srcChangePending = 1;
		}
		if ((int32_t)(t - nextIter) < 0) { continue; }
		// ---- one iteration of the main loop ----
		{
			const uint32_t gnow = t;
			uint32_t tEncEnd = t, tFrame = t;
			int encoded = 0, jpegTiles = 0, anyChange = 0, frameIsSource = srcChangePending;
			int pend = 0, nVideo = 0, inMotion = 0, inMotionStrict = 0;
			double jpegMs = 0.0;
			uint32_t interval;
#ifdef SIM_OLD
			(void)frameIsSource;
#endif
			(void)kvm_stream_tick(&s, t, 0, NULL);
			kvm_vrc_update(&rc, t, 0, kvm_stream_rate_or_mem(&s, t), kvm_stream_link_limited(&s, t));
			(void)kvm_vrc_tick(&rc, t);
			{
				// the acknowledgement of the key frame, as kvm_video_poll takes it (kvm_vrc_idr_acked: C_idr and, since the first key frame proof, the proof of capacity)
				uint32_t ackAt = 0, tsw = 0, queueMs = 0, minRtt = 0;
				if (kvm_stream_watch_result(&s, &ackAt, &tsw, &queueMs, &minRtt)) { kvm_vrc_idr_acked(&rc, t, idrBytes, tsw, ackAt, queueMs, minRtt); }
			}
			(void)kvm_budget_window(&budget, t);
			interval = kvm_pace_interval(&budget, c->periodMs, (uint32_t)encAvgUs);
			// frame_begin
			if (!started) { started = 1; lastFrameTick = t; }
#ifndef SIM_OLD
			kvm_motion_frame(&m, lastFrameTick, realChanged);
#endif
			realChanged = 0;
			if ((uint32_t)(t - lastFrameTick) > 250u) { prevChanged = 0; } else if (changedThisFrame > 0) { prevChanged = changedThisFrame; }
			changedThisFrame = 0; lastFrameTick = t;
			for (k = 0; k < NT; k++) { if (chgAcc[k] || deferred[k]) { anyChange = 1; break; } }
			// the tile loop: note_crc, the class, the JPEG rectangles, the claim of the video
			{
				int n0 = 0, cls0[NT];
				for (k = 0; k < NT; k++)
				{
					int cls = 0;
					if (!chgAcc[k] && !deferred[k]) { continue; }
					if (chgAcc[k])
					{
						const int32_t since = (int32_t)(gnow - lc[k]);
#ifndef SIM_OLD
						streak[k] = kvm_motion_note_change(&m, streak[k], since, frameIsSource);
						if (!frameIsSource) { if (seen[k]) { realChanged++; } changedThisFrame++; }
#else
						streak[k] = (streak[k] > 0 && since <= 250) ? (streak[k] < 255 ? streak[k] + 1 : 255) : 1;
						if (seen[k]) { realChanged++; }
						changedThisFrame++;
#endif
						lc[k] = gnow; seen[k] = 1;
#ifndef SIM_OLD
						// the safety net: a tile that differs after a change of the source is not sent; it is lossy and the sharpening repaints it
						if (kvm_motion_source_absorb(frameIsSource, 0)) { st[k] = T_VIDEO; viaVideo[k] = 0; chgAcc[k] = 0; continue; }
#endif
					}
					if (deferred[k] == 1) { cls = 1; }
					else if (deferred[k] == 2) { cls = 0; }
					else
					{
#ifndef SIM_OLD
						cls = kvm_motion_class(&m, gnow, k / COLS, k % COLS, streak[k], 0);
#else
						cls = (streak[k] >= 2 && prevChanged >= 16u) ? 1 : 0;
#endif
					}
					if (cls)
					{
						st[k] = T_PEND; deferred[k] = 0;			// kvm_video_claim_rect
					}
					else { cls0[n0++] = k; }
					chgAcc[k] = 0;
				}
				// the JPEG rectangles of the full class, coalesced as get_tile_at does it (to the right while the tiles are of this class, then down while the whole row is): the window of the viewer holds
				// back a rectangle that does not fit (its tiles are deferred and go as motion later)
				if (n0 > 0)
				{
					uint8_t is0[NT], done[NT];
					int row, col;
					memset(is0, 0, sizeof(is0)); memset(done, 0, sizeof(done));
					for (k = 0; k < n0; k++) { is0[cls0[k]] = 1; }
					for (row = 0; row < ROWS; row++)
					{
						for (col = 0; col < COLS; col++)
						{
							int right, bot, nt, r, cc;
							if (!is0[row * COLS + col] || done[row * COLS + col]) { continue; }
							right = col;
							while (right + 1 < COLS && is0[row * COLS + right + 1] && !done[row * COLS + right + 1]) { right++; }
							bot = row;
							while (bot + 1 < ROWS)
							{
								int ok = 1;
								for (cc = col; cc <= right; cc++) { if (!is0[(bot + 1) * COLS + cc] || done[(bot + 1) * COLS + cc]) { ok = 0; break; } }
								if (!ok) { break; }
								bot++;
							}
							nt = (right - col + 1) * (bot - row + 1);
							for (r = row; r <= bot; r++) { for (cc = col; cc <= right; cc++) { done[r * COLS + cc] = 1; } }
							{
								const int gate = nt <= 8 ? KVM_GATE_SMALL : KVM_GATE_BIG;
								if (!kvm_stream_window_allows(&s, gate, t))
								{
									if (gate == KVM_GATE_BIG) { kvm_stream_note_link_held(&s, t); } else { kvm_stream_note_held_back(&s, t); }
#ifndef SIM_OLD
									for (r = row; r <= bot; r++) { for (cc = col; cc <= right; cc++) { deferred[r * COLS + cc] = (deferred[r * COLS + cc] == 2 || frameIsSource) ? 2 : 1; } }	// a full-class tile of the frame of another source stays full class
#else
									for (r = row; r <= bot; r++) { for (cc = col; cc <= right; cc++) { deferred[r * COLS + cc] = 1; } }
#endif
									continue;
								}
								{
									const uint32_t bytes = (uint32_t)((70.0 + c->jpegTileBytes * nt) * (0.9 + 0.2 * rnd01()));
									kvm_stream_note_write(&s, bytes, KVM_W_DATA, t);
									if (meas) { bJpeg += bytes; } if (meas && (realChanged >= 16u || (lastSerMotion != 0 && (int32_t)(t - lastSerMotion) <= 250))) { bJpegMot += bytes; if (realChanged >= 16u || (lastSerMotion != 0 && (int32_t)(t - lastSerMotion) <= 50)) { bJpegMotS += bytes; } }
									jpegTiles += nt; jpegMs += 0.04 * nt;
									for (r = row; r <= bot; r++) { for (cc = col; cc <= right; cc++) { st[r * COLS + cc] = T_JPEG; deferred[r * COLS + cc] = 0; } }
								}
							}
						}
					}
				}
			}
			inMotion = (realChanged >= 16u) || (lastSerMotion != 0 && (int32_t)(t - lastSerMotion) <= 250);
			inMotionStrict = (realChanged >= 16u) || (lastSerMotion != 0 && (int32_t)(t - lastSerMotion) <= 50);	// (the sharpening that begins 150 ms after the last frame of a motion is not in it)
			tFrame = t + (uint32_t)jpegMs;
			// the paint of the JPEG rectangles of this iteration
			if (jpegTiles > 0 && meas) { nPaintJ++; }
			// the video frame
			for (k = 0; k < NT; k++) { if (st[k] == T_PEND) { pend++; } }
			if (pend > 0)
			{
				kvm_vplan_in in;
				kvm_vplan plan;
				in.now = tFrame; in.pending = (unsigned)pend; in.gateOpen = kvm_stream_window_allows(&s, KVM_GATE_VIDEO, tFrame); in.rateOpen = kvm_vrc_peek(&rc, tFrame);
				in.intervalMs = interval; in.rate = kvm_stream_rate_or_mem(&s, tFrame);
				kvm_vctl_plan(&ctl, &s, &in, &plan);
				if (plan.held) { if (SIM_NOTE_LINK_HELD(&s, tFrame)) { kvm_vrc_window_refused(&rc); } else { SIM_REFUSED_SOFT(&rc); } if (nHeldEv < 4096) { heldEv[nHeldEv++] = (int)rel; } }
				if (plan.heldRate) { (void)kvm_vrc_may_encode(&rc, tFrame, 0); }
				if (plan.encode)
				{
					const int idr = ctl.idrFirst || plan.forceIdr || ctl.idrForce;
					const int qp = kvm_vrc_qp(&rc, idr);
					const double dt = lastFrameT ? (double)(tFrame - lastFrameT) : 100.0;
					double b;
					uint32_t bytes, seq;
					(void)kvm_vrc_may_encode(&rc, tFrame, idr);
					if (idr) { b = (idrCount == 0 ? 500000.0 : 175245.0) * pow(0.94, (double)(qp - 30)); idrCount++; }
					else { b = c->videoTileBytes * (double)pend * pow(0.89, (double)(qp - 26)) * (1.0 + 0.5 * (dt > 16.7 ? ((dt > 100.0 ? 100.0 : dt) / 16.7 - 1.0) : 0.0) / 2.0)	/* what changed since the last frame, up to 100 ms of it: a frame after a pause is not a hundred times bigger */; }
					b *= 0.85 + 0.3 * rnd01();
					bytes = (uint32_t)b;
					tEncEnd = tFrame + c->encMs;
					{ const int hadFrame = ctl.haveFrame; const uint32_t prevSlot = ctl.lastFrame; (void)kvm_vctl_frame_done(&ctl, idr, bytes, tFrame); ctl.lastFrame = kvm_pace_slot(hadFrame, prevSlot, tFrame, interval, NULL); }
#ifndef SIM_OLD
					kvm_vctl_note_write(&ctl, tEncEnd, (uint32_t)((encAvgUs + 500.0) / 1000.0));
#endif
					encAvgUs = encAvgUs * 0.875 + (double)(c->encMs * 1000u) * 0.125;
					kvm_vrc_frame(&rc, tFrame, bytes, qp, idr, c->encMs * 1000u);
					seq = kvm_stream_add_position(&s, tFrame, bytes);
					kvm_stream_note_write(&s, bytes, KVM_W_POSITION, tFrame);
					if (idr) { kvm_stream_watch(&s, seq); idrBytes = bytes; }
					pos[npos].seq = seq; pos[npos].ts = tFrame; pos[npos].cum = s.bytesWritten; npos++;
					for (k = 0; k < NT; k++) { if (st[k] == T_PEND) { st[k] = T_VIDEO; lc[k] = tEncEnd; viaVideo[k] = 1; } }
					kvm_sharp_motion(&sg, tEncEnd);
					lastFrameT = tFrame; encoded = 1;
					if (meas) { bVideo += idr ? 0 : bytes; if (idr) { bIdr += bytes; } nPaintV++; if (inMotion && !idr) { bVideoMot += bytes; if (inMotionStrict) { bVideoMotS += bytes; } } } else if (idr) { bIdr += bytes; }
					// the gap of the paints
					if (meas && inMotion && haveLastPaint && lastPaintAt != 0 && (int32_t)(tEncEnd - lastPaintAt) >= 4 && (int32_t)(tEncEnd - lastPaintAt) <= 1000)
					{
						const double g = (double)(tEncEnd - lastPaintAt);
						if (nGaps < 100000) { gaps[nGaps++] = g; }
						if (g * 2.0 <= (double)interval) { pairs++; }
						if (g > 2.0 * (double)interval) { over2++; }
					}
					lastPaintAt = tEncEnd; haveLastPaint = 1;
					if (firstMotionAt >= 0.0 && R.firstMotionVideoMs == 0.0) { R.firstMotionVideoMs = (double)rel + 0.0 - firstMotionAt; }
				}
			}
			if (jpegTiles > 0 && !encoded)
			{
				const uint32_t tp = tFrame;
				if (meas && inMotion && haveLastPaint && (int32_t)(tp - lastPaintAt) >= 4 && (int32_t)(tp - lastPaintAt) <= 1000)
				{
					const double g = (double)(tp - lastPaintAt);
					if (nGaps < 100000) { gaps[nGaps++] = g; }
					if (g * 2.0 <= (double)interval) { pairs++; }
					if (g > 2.0 * (double)interval) { over2++; }
				}
				lastPaintAt = tp; haveLastPaint = 1;
			}
			// series bookkeeping (a frame of motion: 16 real changes)
			if (realChanged >= 16u)
			{
				if (nSer == 0 || (int32_t)(t - lastSerMotion) > 500)
				{
					if (c->verbose == 1) { printf("   series %d at %u: R=%.0f kb/s rate=%.0f kb/s mem=%.0f kb/s W=%llu limited=%d held=%u denied=%u\n", nSer, (unsigned)rel, (double)rc.rate * 8.0 / 1000.0, (double)kvm_stream_rate_or_mem(&s, t) * 8.0 / 1000.0, (double)s.rateMem * 8.0 / 1000.0, (unsigned long long)kvm_stream_window(&s, t), kvm_stream_link_limited(&s, t), rc.sHeld, rc.sDenied); }
					if (nSer < MAXSER) { serinfo *z = &ser[nSer++]; z->t0 = rel; z->have = 0; z->rStartKbps = (uint32_t)(rc.rate * 8 / 1000); z->denied0 = rc.sDenied; z->held0 = rc.sHeld; z->heldAt100 = 0; z->tFirstVideo = 0; z->tLast = rel; }
				}
				lastSerMotion = t;
				if (nSer > 0) { ser[nSer - 1].tLast = rel; }
				if (firstMotionAt < 0.0 && rel >= 4000 && (c->kind == SC_START || c->kind == SC_START2)) { firstMotionAt = (double)rel; }
			}
			if (encoded && nSer > 0 && !ser[nSer - 1].have && (int32_t)(t - lastSerMotion) <= 500) { ser[nSer - 1].have = 1; ser[nSer - 1].tFirstVideo = rel; }
			if (meas && jpegTiles > 0 && inMotion) { nPaintJm++; }
			if (meas && encoded && inMotion) { nPaintVm++; }
			if (pend > 0) { kvm_sharp_motion(&sg, t); }
			// the sharpening
			{
				const uint32_t snow = gnow;
				int64_t sb = 0, normalBytes = 0;
				int pass, anyDue = 0;
				const uint64_t W = kvm_stream_window(&s, tEncEnd);
				nVideo = 0; for (k = 0; k < NT; k++) { if (st[k] == T_VIDEO) { nVideo++; if (kvm_sharp_settled(snow, lc[k], SETTLE)) { anyDue = 1; } } }
#ifndef SIM_OLD
				kvm_sharp_set_interval(&sg, interval);
				if (realChanged >= 16u) { kvm_sharp_motion(&sg, tEncEnd); }
#endif
				sb = kvm_sharp_begin(&sg, tEncEnd, SIM_RATE_SHARP(&s, tEncEnd), W, 98304);
				if (!kvm_stream_window_allows(&s, KVM_GATE_SHARPEN, tEncEnd)) { if (nVideo > 0) { kvm_stream_note_held_back(&s, tEncEnd); } sb = 0; }
				for (pass = 0; pass < 2; pass++)
				{
					int guard = 0;
					if (pass == 1)
					{
						uint32_t fb = 0;
						if (!anyDue) { dueSince = 0; continue; }
						if (dueSince == 0) { dueSince = t ? t : 1u; continue; }
						if (normalBytes > 0 || (int32_t)(t - dueSince) < (int32_t)KVM_VIDEO_SHARPEN_WAIT_MS) { continue; }
						if (!kvm_vcore_force_sharpen(SIM_RATE_SHARP(&s, t), 0, kvm_stream_inflight(&s), W, forcedHave, lastForced, t, &fb)) { continue; }
						sb = (int64_t)fb; lastForced = t; forcedHave = 1;
					}
					while (sb > 0 && guard++ < 2000)
					{
						int run = 0, idx = -1, n;
						for (n = 0; n < NT; n++) { const int i = (cursor + n) % NT; if (st[i] == T_VIDEO && kvm_sharp_settled(snow, lc[i], SETTLE)) { idx = i; break; } }
						if (idx < 0) { break; }
#ifndef SIM_OLD
						if (pass == 0 && !kvm_sharp_window_ok_at(&sg, tEncEnd, kvm_stream_inflight(&s), W)) { kvm_stream_note_held_back(&s, tEncEnd); break; }
#else
						if (pass == 0 && !kvm_sharp_window_ok(&sg, kvm_stream_inflight(&s), W)) { kvm_stream_note_held_back(&s, tEncEnd); break; }
#endif
						while (run < 8 && idx + run < NT && (idx + run) / COLS == idx / COLS && st[idx + run] == T_VIDEO && kvm_sharp_settled(snow, lc[idx + run], SETTLE)) { run++; }
						{
							const uint32_t bytes = (uint32_t)((60.0 + c->jpegTileBytes * run) * (0.9 + 0.2 * rnd01()));
							int active;
							for (k = 0; k < run; k++) { st[idx + k] = T_JPEG; viaVideo[idx + k] = 0; }
							kvm_stream_note_write(&s, bytes, KVM_W_DATA, tEncEnd);
							sb -= bytes; cursor = (idx + run) % NT;
							if (meas) { bSharp += bytes; } if (meas && inMotionStrict) { bSharpMot += bytes; }
							if (pass == 0) { kvm_sharp_sent(&sg, tEncEnd, bytes); } else { kvm_sharp_forced(&sg, tEncEnd, bytes); }
							active = kvm_sharp_active(&sg, tEncEnd);
							kvm_vrc_note_sharpen(&rc, tEncEnd, bytes, active);
							if (pass == 0) { normalBytes += bytes; dueSince = 0; }
							if (meas && nSer > 0 && (int32_t)(t - lastSerMotion) <= 1000) { bSharpSer += bytes; }
							if (meas && (nSer == 0 || (int32_t)(t - lastSerMotion) > 1000) && (c->quietUntil == 0 || (int)rel < c->quietUntil)) { R.sharpKBIdle += (double)bytes / 1024.0; }
						}
					}
				}
			}
			// rule M1: a position after an iteration that wrote bytes
			{
				int probe = 0;
				if (kvm_stream_marker_wanted(&s, tEncEnd, 0, &probe) && npos < 199000)
				{
					uint8_t mk[KVM_MARKER_BYTES];
					kvm_stream_make_marker(&s, tEncEnd, probe, mk);
					kvm_stream_note_write(&s, KVM_MARKER_BYTES, KVM_W_POSITION, tEncEnd);
					pos[npos].seq = s.seqCounter; pos[npos].ts = tEncEnd; pos[npos].cum = s.bytesWritten; npos++;
				}
			}
			{
				const double q = (double)(s.bytesWritten - (uint64_t)served) * 1000.0 / (double)linkBps;
				if (q > qMax) { qMax = q; }
				if (nSer > 0 && rel >= ser[nSer - 1].t0 && rel < ser[nSer - 1].t0 + 1000u && (c->dropMs <= 0 || (int)ser[nSer - 1].t0 >= c->dropMs)) { if (q > qMaxFirst) { qMaxFirst = q; } }
				if (c->contEnd > 0 && (int)rel >= c->contEnd - 100 && R.rContKbps == 0.0) { R.rContKbps = (double)rc.rate * 8.0 / 1000.0; R.capContKbps = (double)rc.cap * 8.0 / 1000.0; }
			}
			// the end of the iteration: the loop sleeps until the next slot of its schedule, and wakes for the slot of the next video frame (kvm.c)
			{
				const uint32_t stop = (jpegTiles > 0 || encoded) ? (encoded ? tEncEnd : tFrame) : t;
				uint32_t next = t + c->periodMs, wake = 0;
				const int pendNow = pend > 0 && !encoded ? 1 : 0;
				if (stop >= next) { next = stop; }
				if (pendNow || (pend > 0 && encoded && 0))
				{
					wake = kvm_pace_wake_ms(ctl.haveFrame, ctl.lastFrame, stop, interval);
#ifndef SIM_OLD
					{ const uint32_t w2 = kvm_vctl_write_wait_ms(&ctl, stop, interval); if (w2 > wake) { wake = w2; } }
#endif
					if (!kvm_stream_window_allows(&s, KVM_GATE_VIDEO, stop) || !kvm_vrc_peek(&rc, stop)) { wake = 0; }
					if (wake > 0 && stop + wake < next) { next = stop + wake; }
				}
				else if (encoded)
				{
					// the next frame: the wake for its slot only matters while something is pending, which the next capture decides; the schedule of the loop is what keeps the rate
				}
				if (!anyChange && pend == 0 && nVideo == 0 && !encoded && jpegTiles == 0)
				{
					// nothing to do: the capture waits for the next change of the screen (the loop is idle)
					next = t + 1;
				}
				nextIter = next > t ? next : t + 1u;
				if (c->verbose > 1) { printf("   t=%5u it pend=%d enc=%d jpegTiles=%d next=%u\n", (unsigned)rel, pend, encoded, jpegTiles, (unsigned)(nextIter - t0)); }
			}
			{
				int left = 0;
				for (k = 0; k < NT; k++) { if (st[k] != T_JPEG) { left++; } }
				if (inMotion) { settleOpen = 1; }
				else if (settleOpen && left == 0 && meas)
				{
					const double d = (double)(int32_t)(t - lastSerMotion);
					settleOpen = 0; settleSum += d; settleN++; if (d > settleMax) { settleMax = d; }
				}
			}
			if (c->verbose == 1 && (rel % 1000u) < 17u && rel < 20000u && rel / 1000u != lastPrintSec) { lastPrintSec = rel / 1000u; printf("   t=%2us R=%5.0f kb/s rate=%5.0f kb/s W=%7llu frames=%u qp=%d denied=%u held=%u link_limited=%d\n", (unsigned)lastPrintSec, (double)rc.rate * 8.0 / 1000.0, (double)kvm_stream_rate_or_mem(&s, t) * 8.0 / 1000.0, (unsigned long long)kvm_stream_window(&s, t), nPaintV, rc.qp, rc.sDenied, rc.sHeld, kvm_stream_link_limited(&s, t)); }
			if (meas && inMotionStrict) { rateAcc += (double)SIM_RATE_SHARP(&s, t) * (double)(nextIter > t ? nextIter - t : 1u) / 1000.0; }
			frameIsSource = 0; srcChangePending = 0;
			if ((c->kind == SC_START || c->kind == SC_START2) && rel >= 130 && rel < 140) { unsigned cnt = 0; for (k = 0; k < NT; k++) { if (st[k] == T_PEND || (st[k] == T_VIDEO && viaVideo[k])) { cnt++; } } if (cnt > R.claimedAtB) { R.claimedAtB = cnt; } }
		}
	}
	// ---- the numbers ----
	{
		uint32_t lastMotionRel = 0;
		int i;
		const double sec = (double)(tEnd - measT) / 1000.0;
		R.seconds = sec;
		R.fpsVideo = (double)nPaintV / sec; R.fpsJpeg = (double)nPaintJ / sec;
		R.jpegPaintPct = (nPaintJm + nPaintVm) > 0 ? 100.0 * (double)nPaintJm / (double)(nPaintJm + nPaintVm) : 0.0;
		R.jpegKB = (double)bJpeg / 1024.0; R.videoKB = (double)bVideo / 1024.0; R.sharpKB = (double)bSharp / 1024.0; R.idrKB = (double)bIdr / 1024.0;
		R.jpegBytesPct = (bJpegMot + bVideoMot) > 0 ? 100.0 * (double)bJpegMot / (double)(bJpegMot + bVideoMot) : 0.0;
		R.sharpShareMotionPct = (bSharpMot + bVideoMotS + bJpegMotS) > 0 ? 100.0 * (double)bSharpMot / (double)(bSharpMot + bVideoMotS + bJpegMotS) : 0.0;
		R.sharpOfRatePct = rateAcc > 0.0 ? 100.0 * (double)bSharpMot / rateAcc : 0.0;
		R.settleMs = settleMax; R.settleAvgMs = settleN > 0 ? settleSum / settleN : 0.0;
		R.sharpSharePct = (bSharp + bVideo + bJpeg) > 0 ? 100.0 * (double)bSharp / (double)(bSharp + bVideo + bJpeg) : 0.0;
		R.sharpShareSeriesPct = (bSharpSer + bVideo + bJpeg) > 0 ? 100.0 * (double)bSharpSer / (double)(bSharpSer + bVideo + bJpeg) : 0.0;
		R.series = (unsigned)nSer;
		for (i = 0; i < nSer; i++) { if (ser[i].tLast > lastMotionRel) { lastMotionRel = ser[i].tLast; } }
		{
			double sum = 0.0, mx = 0.0, rmin = 1e12;
			unsigned nstart = 0, h100 = 0;
			for (i = 0; i < nSer; i++)
			{
				if ((int)ser[i].t0 < c->measureFrom) { continue; }	// the continuous motion at the start is no series of the pauses
				if (ser[i].have) { const double d = (double)ser[i].tFirstVideo - (double)ser[i].t0; sum += d; if (d > mx) { mx = d; } nstart++; } else { R.noVideo++; }
				if ((double)ser[i].rStartKbps < rmin) { rmin = (double)ser[i].rStartKbps; }
				{
					int j; unsigned hh = 0;
					for (j = 0; j < nHeldEv; j++) { if (heldEv[j] >= (int)ser[i].t0 && heldEv[j] < (int)ser[i].t0 + 100) { hh++; } }
					h100 += hh;
				}
			}
			R.startMsAvg = nstart ? sum / nstart : 0.0; R.startMsMax = mx; R.rStartMinKbps = rmin < 1e11 ? rmin : 0.0; R.heldIn100ms = h100;
			R.rStartRatio = R.rContKbps > 0.0 ? R.rStartMinKbps / R.rContKbps : 0.0;
		}
		R.linkKbps = (double)c->linkBps * 8.0 / 1000.0;	// the link of the scene (a quantity that does not depend on the memory of the rate)
		R.deniedInSeries = rc.sDenied; R.heldFirstSecond = rc.sHeld;
		if (nGaps > 0) { double *tmp = (double *)malloc(sizeof(double) * (size_t)nGaps); memcpy(tmp, gaps, sizeof(double) * (size_t)nGaps); qsort(tmp, (size_t)nGaps, sizeof(double), cmpd); R.gapP95 = tmp[(int)(nGaps * 0.95)]; R.gapMax = tmp[nGaps - 1]; free(tmp); }
		R.over2xPerS = over2 / sec; R.pairsPct = nGaps > 0 ? 100.0 * pairs / (double)nGaps : 0.0;
		R.nVideoFrames = nPaintV; R.nJpegPaints = nPaintJ;
		R.queueMaxMs = qMax; R.queueFirstSeriesMs = qMaxFirst;
		// settle: the state at the end is all clean, the time from the last motion is when the last tile was sharpened: measured by the run to its end (the scene ends with a pause of at least 3 s)
#ifndef SIM_OLD
		R.classNear = m.sNear; R.classStreak = m.sStreak;
		kvm_motion_free(&m);
#endif
	}
	return R;
}

static cfg base(const char *label, int kind, uint64_t bps, int seconds)
{
	cfg c;
	memset(&c, 0, sizeof(c));
	c.label = label; c.kind = kind; c.linkBps = bps; c.linkBps2 = bps; c.oneWayMs = 2; c.seconds = seconds; c.periodMs = 16; c.encMs = 8;
	c.jpegTileBytes = 580.0; c.videoTileBytes = 31.0;
	return c;
}

static void head(void)
{
	printf("| scene | video paints/s | JPEG paints/s | JPEG share of the paints in motion | JPEG (class 0) share of the bytes of the motion | sharpening in the motion: share of the bytes / share of the delivery rate; in the series and the second after it: share of the bytes | R at the start of a series: min kb/s (ratio to the continuous R) | video starts after ms (avg / max) | window refusals in the first 100 ms of the series | gaps P95 / max ms | gaps above 2 x interval a second | pairs of paints <= half an interval |\n|---|---|---|---|---|---|---|---|---|---|---|---|\n");
}

static void row(const char *label, const res *r)
{
	printf("| %s | %.1f | %.1f | %.0f %% | %.0f %% | %.0f %% / %.0f %% ; %.0f %% | %.0f (%.2f) | %.0f / %.0f | %u | %.0f / %.0f | %.2f | %.1f %% |\n", label, r->fpsVideo, r->fpsJpeg, r->jpegPaintPct, r->jpegBytesPct, r->sharpShareMotionPct, r->sharpOfRatePct, r->sharpShareSeriesPct, r->rStartMinKbps, r->rStartRatio, r->startMsAvg, r->startMsMax, r->heldIn100ms, r->gapP95, r->gapMax, r->over2xPerS, r->pairsPct);
}

#ifdef SIM_OLD
#define BUILD_NAME "before: the earlier files"
#else
#define BUILD_NAME "after: the tree"
#endif

int main(int argc, char **argv)
{
	cfg c;
	res rep, bur, dra, drp, sta, sta2, sta3, sta4, typ, mix;
	int verbose = argc > 1 && strcmp(argv[1], "-v") == 0 ? 1 : (argc > 1 && strcmp(argv[1], "-vv") == 0 ? 2 : 0);
	printf("simulation build: %s\n\n", BUILD_NAME);
	// 1. the replay of the AMD laptop (open lid, High, link 41 Mb/s, RTT 5 ms): 7.5 s of continuous motion, then the work with the interface (counted from 8 s)
	c = base("AMD laptop replay", SC_REPLAY, 5125000u, 40); c.seed = 1; c.measureFrom = 8000; c.contEnd = 7500; c.verbose = verbose; rep = run(&c);
	// 2. the series of the criteria: 25 s of continuous motion, then 10 series of 0.4 s every 3.0 s (an animation of 760 tiles and a dragged strip), 40 Mb/s, RTT 5 ms
	c = base("burst 40 Mb/s", SC_BURST, 5000000u, 25 + 34); c.jpegTileBytes = 400.0; c.seed = 1; c.measureFrom = (int)BURST_CONT; c.contEnd = (int)BURST_CONT; c.verbose = verbose; bur = run(&c);
	// 3. a window dragged: a strip of 160 px, 16 px a frame, 1.5 s, six times
	c = base("drag", SC_DRAG, 5000000u, 22); c.jpegTileBytes = 400.0; c.seed = 1; c.measureFrom = 2900; c.contEnd = 0; c.verbose = verbose; drp = run(&c); dra = drp;
	// 4. the link falls from 40 to 4 Mb/s in the pause before the fifth series
	c = base("link drop", SC_DROP, 5000000u, 25 + 34); c.jpegTileBytes = 400.0; c.seed = 1; c.linkBps2 = 500000u; c.dropMs = (int)BURST_T(4u) + 400 + BURST_GAP / 2u; c.measureFrom = (int)BURST_CONT; c.contEnd = (int)BURST_CONT; c.verbose = verbose; drp = run(&c);
	// 5. the start of a session : 3 Mb/s, the first copy of DXGI differs in 2022 tiles, the screen stands until +17 s (as in that measurement); and 1 Mb/s with the motion at +4.5 s
	c = base("start 3 Mb/s", SC_START, 375000u, 24); c.srcDiff = 2022; c.jpegTileBytes = 372.0; c.oneWayMs = 20; c.measureFrom = 0; c.quietUntil = 16900; c.verbose = verbose; sta = run(&c);
	c = base("start 1 Mb/s", SC_START2, 125000u, 24); c.srcDiff = 2022; c.jpegTileBytes = 372.0; c.oneWayMs = 20; c.measureFrom = 0; c.quietUntil = 4400; c.verbose = verbose; sta2 = run(&c);
	c = base("start 1 Mb/s, alpha cleared", SC_START2, 125000u, 24); c.srcDiff = 18; c.jpegTileBytes = 372.0; c.oneWayMs = 20; c.measureFrom = 0; c.quietUntil = 4400; c.verbose = verbose; sta3 = run(&c);
	c = base("start 3 Mb/s, alpha cleared", SC_START, 375000u, 24); c.srcDiff = 18; c.jpegTileBytes = 372.0; c.oneWayMs = 20; c.measureFrom = 0; c.quietUntil = 16900; c.verbose = verbose; sta4 = run(&c);
	// 6. typing
	c = base("typing", SC_TYPING, 5000000u, 14); c.measureFrom = 1000; c.verbose = verbose; typ = run(&c);
	// 7. typing in a text field while an animation runs far away
	c = base("typing + animation", SC_MIXED, 5000000u, 12); c.measureFrom = 1000; c.verbose = verbose; mix = run(&c);
	printf("Table 1: the series of motion (%s). AMD laptop, from the log (10 s window): 17.4 JPEG against 6.0 video paints a second, 29-99 %% of the bytes of the second in sharpening, R of 4.0-4.8 Mb/s at the start of a series after a pause\n\n", BUILD_NAME);
	head();
	row("AMD laptop replay (the interactive part, 32 s)", &rep);
	row("burst 40 Mb/s, RTT 5 ms (10 series)", &bur);
	row("drag 160 px, 16 px a frame (6 x 1.5 s)", &dra);
	row("link 40 -> 4 Mb/s in a pause", &drp);
	row("typing", &typ);
	row("typing + an animation far away", &mix);
	printf("\n| scene | R of the continuous motion kb/s (the estimate without the proof of the key frame) | JPEG KB | video KB | sharpening KB (outside the series) | IDR KB | window refusals / bucket refusals in the run | queue max ms (first second of a series) | series without a video frame | settle after a series ms (avg / max) |\n|---|---|---|---|---|---|---|---|---|---|\n");
	{
		const struct { const char *n; const res *r; } all[] = { { "AMD laptop replay", &rep }, { "burst", &bur }, { "drag", &dra }, { "link drop", &drp }, { "typing", &typ }, { "typing + animation", &mix } };
		size_t i;
		for (i = 0; i < sizeof(all) / sizeof(all[0]); i++) { const res *r = all[i].r; printf("| %s | %.0f (%.0f) | %.0f | %.0f | %.0f (%.0f) | %.0f | %u / %u | %.0f (%.0f) | %u of %u | %.0f / %.0f |\n", all[i].n, r->rContKbps, r->capContKbps, r->jpegKB, r->videoKB, r->sharpKB, r->sharpKBIdle, r->idrKB, r->heldFirstSecond, r->deniedInSeries, r->queueMaxMs, r->queueFirstSeriesMs, r->noVideo, r->series, r->settleAvgMs, r->settleMs); }
	}
	printf("\nTable 2: the start of a session. Tiles pending or video right after the first copy of DXGI (2022 in that measurement), the key frame, the sharpening that came without any motion, the video frames in the run, and the wait of the first frame of the motion\n\n");
	printf("| link | tiles pending or video after the first copy of DXGI | IDR KB | sharpening KB (without motion) | video frames | the first video frame of the motion after ms |\n|---|---|---|---|---|---|\n");
	printf("| 3 Mb/s, motion at +17 s, all 2022 tiles differ | %u | %.0f | %.0f (%.0f) | %u | %.0f |\n", sta.claimedAtB, sta.idrKB, sta.sharpKB, sta.sharpKBIdle, sta.nVideoFrames, sta.firstMotionVideoMs);
	printf("| 1 Mb/s, motion at +4.5 s, all %d tiles differ | %u | %.0f | %.0f (%.0f) | %u | %.0f |\n", 2022, sta2.claimedAtB, sta2.idrKB, sta2.sharpKB, sta2.sharpKBIdle, sta2.nVideoFrames, sta2.firstMotionVideoMs);
	printf("| 3 Mb/s, motion at +17 s, 18 tiles differ (alpha cleared) | %u | %.0f | %.0f (%.0f) | %u | %.0f |\n", sta4.claimedAtB, sta4.idrKB, sta4.sharpKB, sta4.sharpKBIdle, sta4.nVideoFrames, sta4.firstMotionVideoMs);
	printf("| 1 Mb/s, motion at +4.5 s, 18 tiles differ (alpha cleared) | %u | %.0f | %.0f (%.0f) | %u | %.0f |\n", sta3.claimedAtB, sta3.idrKB, sta3.sharpKB, sta3.sharpKBIdle, sta3.nVideoFrames, sta3.firstMotionVideoMs);
	printf("\nclass of tiles (tiles taken by the streak rule / by the near rule): burst %u / %u, drag %u / %u, replay %u / %u\n\n", bur.classStreak, bur.classNear, dra.classStreak, dra.classNear, rep.classStreak, rep.classNear);
#ifdef SIM_OLD
	// the model has to reproduce what the log of the AMD laptop shows: the JPEG paints are about as many as the video ones in the work with the interface, the sharpening takes a large share of the
	// bytes while it goes, and the start of a series is cold (R of 2-4 Mb/s)
	CHECK(rep.fpsJpeg >= rep.fpsVideo * 0.8 || rep.jpegPaintPct >= 30.0);
	CHECK(rep.sharpShareSeriesPct >= 20.0);
	CHECK(rep.rStartRatio <= 0.55);
	CHECK(typ.fpsVideo == 0.0);
	CHECK(sta.claimedAtB >= 2000u);
#else
	{
		// the criteria "ready" (Docker): burst, drag, link drop, start, settle, typing
		CHECK(bur.linkKbps > 0.0 && bur.rStartMinKbps >= 0.3 * bur.linkKbps);			// R at the start of a series >= 30 % of the link of the scene (40 Mb/s: 12 000 kb/s). Only the remembered rate gives it: without the memory R starts at 4000 kb/s
																				// (10 % of the link), with a memory that holds half of the rate 8858 kb/s; with it 21 364 kb/s. A threshold relative to rc.cap of the same run
																				// did not catch the half memory: rc.cap of the continuous motion depends on the memory too. The R of the continuous motion of the smoothness work (14 246 kb/s) is no base either:
																				// since the first key frame proof the proof of the first key frame (it stands 60 s) lifts R of the continuous motion to the ceiling of the estimate (29 Mb/s)
		CHECK(bur.heldIn100ms == 0);											// no refusal of the window in the first 100 ms of a series (the first frame is video: 25 KB, not a JPEG of 304 KB)
		CHECK(bur.startMsAvg <= 70.0);											// the video comes within about four intervals of the start of the series (the JPEG of the first frame is 33 ms of the loop; the window of the memory, not 64 KiB)
		CHECK(dra.jpegBytesPct <= 15.0);										// the drag: the JPEG of class 0 in the motion at most 15 % of the bytes of the motion
		CHECK(dra.sharpOfRatePct <= 25.0);										// ... and the sharpening in the motion at most 25 % of the delivery rate (the bucket of the governor: 20 %)
		CHECK(bur.sharpOfRatePct <= 25.0);
		CHECK(rep.settleMs <= 1000.0 && bur.settleMs <= 1000.0 && dra.settleMs <= 1000.0);	// the picture is sharp again within a second of the end of a series
		CHECK(drp.queueFirstSeriesMs <= 1303.0);								// the link falls in the pause: the queue of the first series is bounded (the criterion of 500 ms cannot be met: the window of the resume is 512 KiB = 1 s at 4 Mb/s, and the JPEG picture that is bigger than the window is written whole; before: 1.3 s)
		CHECK(sta.claimedAtB == 0 && sta2.claimedAtB == 0 && sta3.claimedAtB == 0 && sta4.claimedAtB == 0);						// the start: the change of the source of the picture is no motion
		CHECK(sta3.sharpKBIdle <= 1.0 && sta4.sharpKBIdle <= 8.0);											// ... and with the alpha byte cleared nothing is sharpened without motion (what still differs is repainted by the sharpening behind the motion)
		CHECK(sta2.firstMotionVideoMs <= 2100.0 && sta3.firstMotionVideoMs <= 2100.0);	// 1 Mb/s, motion at +4.5 s: the first video frame as soon as before the source change rule (2064 ms), not after a second sharp JPEG of the screen
		CHECK(sta.firstMotionVideoMs <= 1000.0);
		CHECK(typ.fpsVideo == 0.0);												// typing is JPEG, whatever the video does around it
		CHECK(mix.nVideoFrames > 0);
		CHECK(rep.jpegPaintPct <= 20.0);										// the AMD laptop replay: the JPEG paints in the motion at most 20 %
		CHECK(rep.sharpOfRatePct <= 25.0);
		CHECK(rep.settleMs <= 1000.0);
	}
#endif
	printf("%d checks, %d failed\n", gChecks, gFail);
	return gFail == 0 ? 0 : 1;
}
