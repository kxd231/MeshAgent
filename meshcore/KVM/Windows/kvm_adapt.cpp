// KVM (Windows): quality that depends on motion, sharpening of still tiles, congestion regulator (WA-2). See kvm_adapt.h.
//
// Everything here runs on the main loop thread of the KVM process (the tile loop, the pipe writes and the pause are all there), with one exception:
// the input thread changes encCompression / encType (read here as plain ints) and answers a refresh by clearing tileInfo[][].crc / flags (not touched here).

#include "tile.h"
#include "kvm_video.h"
#include "../common/kvm_sharpen.h"
#include "../common/kvm_motion.h"
#ifdef KVM_VIDEO_ENABLED
#include "../common/kvm_video_core.h"
#endif

#ifdef KVM_ADAPT_ENABLED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <timeapi.h>

extern "C"
{
extern int TILE_WIDTH;
extern int TILE_HEIGHT;
extern int TILE_WIDTH_COUNT;
extern int TILE_HEIGHT_COUNT;
extern int PIXEL_SIZE;
extern int FRAME_RATE_TIMER;
extern int SCALING_FACTOR_NEW;
extern tileInfo_t **tileInfo;
int kvm_adapt_encode_sharp(int x, int y, int w, int h, void *desktop, void **buffer, long long *bufferSize, int *quality); // tile.cpp
}
extern ULONG encCompression; // tile.cpp
extern int encType;          // tile.cpp

static struct
{
	int sessionOn, envOff;
	int active;						// WA-2 applies to the frame being processed
	int qUser;
	unsigned char stamp;
	DWORD now, lastFrameTick, sessionStart, lastContentChange;
	int changedThisFrame, prevChanged;
	int level, levelFrame, calm, hot;
	DWORD winStart;
	long long winWriteUs, winBytes;
	double acceptRate;				// bytes per second the link took while it was saturated (0 = unknown)
	double tokens; DWORD tokenTick;	// rate limiter (level 4 and up)
	int lossyCount; int dueIn;		// lossy tiles; ms until the first of them is due for sharpening
	int cursor;						// next tile to look at for sharpening
	// statistics of the period
	int levelMax, wantImage, rateSkips, pauseMult;
	long long rectsMotion, rectsFull, bytesMotion, bytesFull, sharpenRects, sharpenTiles, sharpenBytes, settleSum, settleN, settleMax;
	int blockPctMax;
	// WA-2b: bandwidth budget, motion frame limit, JPEG tables
	int budgetKbps, hardFps, jpegOpt;	// resolved at the start of the session (macro, environment)
	int budgetSrc; LONG seenSeq;		// where the budget comes from: 0 macro, 1 environment, 2 viewer; the last viewer setting looked at
	int bStep, bStepFrame;				// step of the budget ladder (B0..B5) and its value in the frame being processed
	int bOver, bUnder, bCalmReq, bSinceDown;
	int lastCap, gateOpen, motionSentFrame, deferredFrame, deferredCount;
	double motionNext;					// ms since gateOrigin: the next moment at which a motion frame may be sent
	DWORD gateOrigin;					// timeGetTime() the gate clock counts from (moved forward long before the 32 bit clock wraps)
	int bStepMax, bSteps;
	DWORD scaleChangeTick, lastMotionChange, scaleBanUntil, warmUntil, oldestDeferred, seenRefresh; int scaleChanges, realChanged;
	double bytesPerTile, bkForgiven;	// recent size of a tile in the packets (for the size of a rectangle under a budget), bytes that the cap of the debt forgave	// last change of the scale multiplier and their number in the period
	long long winKbpsSum, winN; int winKbpsMax;
	int qMotionMin; long long qMotionSum, qMotionN;
	long long loopN, loopWaitUs, loopProcUs, loopPauseUs, loopIterUs; DWORD loopLast;	// per iteration of the main loop: time waiting for the capture, processing the frame, pausing
	double bkTok, bkSTok, bkSmall; int lastPassSmall; DWORD bkTick;	// byte bucket of the budget: motion / full rectangles and the part reserved for sharpening (bytes; negative = debt)
	double starvedMs; long long bucketDefers, bucketDefersTotal;	// time of the window with an empty bucket, rectangles held back by it
	long long deferredTiles, deferredRects, encUs, encTiles, encN, sharpEncUs, sharpEncTiles, sharpEncN, sentBytesTotal;
	// sharpening while the video runs must not be starved by the window / the budget (kvm_video_core.h KVM_VIDEO_SHARPEN_*)
	DWORD sharpDueSince, sharpForcedAt; int sharpForcing; long long sharpForcedCount;
	// the class of a tile while the video runs (kvm_motion.h: the frame of motion of the last 250 ms, the tiles around a moving one), the source of the pixels, the series of motion for the log
	kvm_motion motion;
	kvm_series series;
	int srcChange, lastSrc, haveSrc; unsigned srcDiff;	// this frame is the first complete one from another source of pixels (GDI fill -> the first copy of DXGI, a probe, DXGI -> GDI)
	unsigned iterJpegTiles; DWORD iterJpegAt;	// the JPEG rectangles that this iteration sent (a picture, written to the log as one)
#ifdef KVM_VIDEO_ENABLED
	kvm_sharp sharp;					// the governor of the sharpening while the video moves (bucket of the share, window per packet)
#endif
} gA;

static volatile LONG gClientBudget = -1;	// kbit/s chosen by the viewer (written by the input thread), -1 = none
static volatile LONG gClientBudgetSeq = 0;
static unsigned long long gSharpenBytesAll = 0;	// bytes of sharpening since the process began (the "video rc" log line)
static volatile DWORD gLastInput = 0;		// timeGetTime() of the last mouse / key command of a viewer (input thread)
static volatile DWORD gRefreshTick = 0;		// ... of the last REFRESH
static volatile DWORD gScaleSentTick = 0;	// timeGetTime() when the agent last asked for another size of the picture (the viewer echoes the scale it was given for a few seconds)
static volatile LONG gUserScale = 0;		// the scale the viewer asked for (command 5, 1/1024), 0 = not known yet
static volatile LONG gScaleMult = 1024;		// the share of it that the budget ladder asks for now (1/1024)
static const char *const kBudgetSrc[] = { "default", "env", "client" };

static int clamp_q(unsigned long q) { return (q < 1) ? 1 : (q > 90 ? 90 : (int)q); }

static int motion_quality_level(int level)
{
	int q = KVM_ADAPT_Q_MOTION - level * KVM_ADAPT_Q_STEP;
	return q < KVM_ADAPT_Q_MOTION_MIN ? KVM_ADAPT_Q_MOTION_MIN : q;
}

// Budget ladder (WA-2b): motion quality, scale of the picture (1/1024 of the scale of the viewer) and motion frame limit per step.
// B0-B5: quality and frame limit as before; B6: quality 20; B7: the scale goes down to 75 % with the frame limit back at 30. Below that only the byte bucket of the
// budget acts: it lets the motion frames out as often as the budget allows (1-2 per second for a screen full of detail at 1 Mb/s).
static int ladder_q(int step)
{
	switch (step)
	{
		case 0: return KVM_ADAPT_Q_MOTION; case 1: return KVM_ADAPT_BQ1; case 2: return KVM_ADAPT_BQ2; case 3: return KVM_ADAPT_BQ3; case 4: return KVM_ADAPT_BQ4; case 5: return KVM_ADAPT_BQ5;
		default: return KVM_ADAPT_Q_FLOOR;
	}
}
static int ladder_scale(int step)
{
	return step >= 7 ? KVM_ADAPT_SCALE_S1 : 1024;
}
static int ladder_fps(int step)
{
	static const int kFps[] = { 0, 0, KVM_ADAPT_BF2, KVM_ADAPT_BF3, KVM_ADAPT_BF4, KVM_ADAPT_BF5, 15, 30 };
	return (step >= 0 && step <= 7) ? kFps[step] : 30;
}
// The frame limit of the motion class: the stricter of the fixed limit and the one of the ladder step (0 = none).
static int fps_cap(int step)
{
	int a = gA.hardFps, b = ladder_fps(step);
	if (a <= 0) { return b; }
	if (b <= 0) { return a; }
	return a < b ? a : b;
}

// Quality of the motion class: the stricter of the congestion level (blocking writes) and the budget step.
static int motion_quality(int level, int step)
{
	int q = motion_quality_level(level), b = ladder_q(step);
	return b < q ? b : q;
}

// encType is written by the input thread (command 5): read it with an interlocked access (the main loop thread decides about the window and the sharpening with it)
static int enc_type(void) { return (int)InterlockedCompareExchange((volatile LONG *)&encType, 0, 0); }

static int type_ok(void) { int t = enc_type(); return (t == 1 || t == 4) && (PIXEL_SIZE == 3 || PIXEL_SIZE == 4); }

static int is_lossy(const tileInfo_t *t, int qUser)
{
	return t->sentQ != 0 && (t->sentQ < qUser || t->sentSub);
}

// a tile that waits for the next video frame is not sharpened (it is going to be painted by the frame): kvm_video_tile_pending
// the comparison is signed. A video frame stamps the tiles it paints with the clock AFTER the encoder, which is later than gA.now of the start of the iteration; in unsigned
// arithmetic that difference was about 4.29e9 and every painted tile counted as "still for 150 ms" (measured: 63-70 % of the bytes were sharpening). A time stamp that is
// later than gA.now means "changed just now".
static int is_candidate(const tileInfo_t *t, int row, int col)
{
	return is_lossy(t, gA.qUser) && !t->deferred && kvm_sharp_settled(gA.now, t->lastChange, KVM_ADAPT_SETTLE_MS) && t->flags != (char)TILE_MARKED_NOT_SENT && t->crc == t->contentCrc && !kvm_video_tile_pending(row, col);
}

static double rate_limit(void) { return gA.level >= 4 ? gA.acceptRate * KVM_ADAPT_RATE_FRACTION_PCT / 100.0 : 0.0; }

static void refill_tokens(DWORD now)
{
	double rate = rate_limit();
	if (rate <= 0.0) { gA.tokens = 0.0; gA.tokenTick = now; return; }
	gA.tokens += rate * (double)(DWORD)(now - gA.tokenTick) / 1000.0;
	if (gA.tokens > rate) { gA.tokens = rate; }					// at most one second of burst
	if (gA.tokens < -2.0 * rate) { gA.tokens = -2.0 * rate; }
	gA.tokenTick = now;
}

// ---- the byte bucket of the budget (hard limit) ----
static double bk_rate(void) { return gA.budgetKbps > 0 ? (double)gA.budgetKbps * 125.0 : 0.0; }	// bytes per second

static void bk_caps(double *capMotion, double *capSharp)
{
	double R = bk_rate(), burst = R * KVM_ADAPT_BUCKET_BURST_MS / 1000.0;
	*capMotion = burst * (100 - KVM_ADAPT_BUCKET_SHARPEN_PCT - KVM_ADAPT_BUCKET_SMALL_PCT) / 100.0;
	*capSharp = burst * KVM_ADAPT_BUCKET_SHARPEN_PCT / 100.0;
}

static void bk_reset(DWORD now)
{
	double cm, cs;
	bk_caps(&cm, &cs);
	gA.bkTok = cm; gA.bkSTok = cs; gA.bkSmall = bk_rate() * KVM_ADAPT_BUCKET_BURST_MS / 1000.0 * KVM_ADAPT_BUCKET_SMALL_PCT / 100.0; gA.bkTick = now; gA.starvedMs = 0.0;
}

static void bk_refill(DWORD now)
{
	double R = bk_rate(), dt, cm, cs;
	if (R <= 0.0) { gA.bkTick = now; return; }
	dt = (double)(DWORD)(now - gA.bkTick);
	if (dt <= 0.0) { return; }
	gA.bkTick = now;
	bk_caps(&cm, &cs);
	gA.bkTok += R * (100 - KVM_ADAPT_BUCKET_SHARPEN_PCT - KVM_ADAPT_BUCKET_SMALL_PCT) / 100.0 * dt / 1000.0;
	gA.bkSTok += R * KVM_ADAPT_BUCKET_SHARPEN_PCT / 100.0 * dt / 1000.0;
	gA.bkSmall += R * KVM_ADAPT_BUCKET_SMALL_PCT / 100.0 * dt / 1000.0;
	{
		double capSmall = R * KVM_ADAPT_BUCKET_BURST_MS / 1000.0 * KVM_ADAPT_BUCKET_SMALL_PCT / 100.0;
		if (gA.bkSmall > capSmall) { gA.bkTok += gA.bkSmall - capSmall; gA.bkSmall = capSmall; } // the share of small changes that nobody uses goes to the motion
	}
	if (gA.bkSTok > cs) { gA.bkTok += gA.bkSTok - cs; gA.bkSTok = cs; } // the share reserved for sharpening that nobody uses goes to the motion (measured: without it the motion got only 80 % of the budget)
	if (gA.bkTok > cm) { gA.bkTok = cm; }
	if (gA.bkTok <= 0.0) { gA.starvedMs += dt; }
}

static void bk_debit(DWORD now, double bytes, int sharpen)
{
	double R = bk_rate();
	if (R <= 0.0) { return; }
	bk_refill(now);
	if (!sharpen && gA.lastPassSmall)
	{
		// a small rectangle that passed on the reserved share: paid from it (a debt of at most half a second of it)
		gA.lastPassSmall = 0;
		gA.bkSmall -= bytes;
		{
			// the debt of the reserved share is capped; what is above the cap is paid from the main bucket (no byte goes unpaid: a flood of small rectangles is limited to the budget too)
			double floorSmall = -R * KVM_ADAPT_BUCKET_SMALL_PCT / 100.0 * 0.5;
			if (gA.bkSmall < floorSmall) { gA.bkTok -= floorSmall - gA.bkSmall; gA.bkSmall = floorSmall; }
		}
		if (gA.bkTok < -R * KVM_ADAPT_BUCKET_DEBT_MS / 1000.0) { gA.bkForgiven += -R * KVM_ADAPT_BUCKET_DEBT_MS / 1000.0 - gA.bkTok; gA.bkTok = -R * KVM_ADAPT_BUCKET_DEBT_MS / 1000.0; }
		return;
	}
	if (sharpen && gA.bkSTok > 0.0)
	{
		double t = gA.bkSTok < bytes ? gA.bkSTok : bytes;
		gA.bkSTok -= t;
		bytes -= t;
	}
	gA.bkTok -= bytes;
	if (gA.bkTok < -R * KVM_ADAPT_BUCKET_DEBT_MS / 1000.0) { gA.bkForgiven += -R * KVM_ADAPT_BUCKET_DEBT_MS / 1000.0 - gA.bkTok; gA.bkTok = -R * KVM_ADAPT_BUCKET_DEBT_MS / 1000.0; }
}

// Budget ladder, evaluated once per window with the kilobits per second written in it. Down a step (or several) when the window is above the budget or the bucket
// was empty for half of it (the content wants more than the budget at this step); up one step after KVM_ADAPT_BUDGET_DOWN_WINDOWS windows that were under the budget
// with an (almost) always filled bucket.
static void budget_window(DWORD now, int kbps, int windowMs)
{
	int from = gA.bStep, starved, calm;
	double R;
	LONG seq = gClientBudgetSeq;		// the sequence first, then the value: a value written in between is applied now or at the next window, never lost
	if (seq != gA.seenSeq)
	{
		LONG v = gClientBudget;
		gA.seenSeq = seq;
		if (v >= 0 && !(gA.budgetSrc == 2 && gA.budgetKbps == (int)v))
		{
			gA.budgetKbps = (int)v; gA.budgetSrc = 2; gA.bStep = 0; gA.bOver = gA.bUnder = 0; gA.bCalmReq = KVM_ADAPT_BUDGET_DOWN_WINDOWS; from = 0;
			bk_reset(now);
			kvm_stats_log("adapt budget_set budget=%d source=client", gA.budgetKbps);
		}
	}
	gA.winKbpsSum += kbps; gA.winN++;
	if (kbps > gA.winKbpsMax) { gA.winKbpsMax = kbps; }
	if (gA.bSinceDown < 100000) { gA.bSinceDown++; }
	if (gA.bSinceDown > 60) { gA.bCalmReq = KVM_ADAPT_BUDGET_DOWN_WINDOWS; } // the last recovery held for 30 s: back to the normal patience
	bk_refill(now);
	starved = (windowMs > 0 && gA.starvedMs * 2.0 >= (double)windowMs) ? 1 : 0;
	calm = (windowMs <= 0 || gA.starvedMs * 10.0 < (double)windowMs) ? 1 : 0;
	gA.starvedMs = 0.0;
	if (gA.budgetKbps <= 0) { gA.bStep = 0; return; }
	if ((DWORD)(now - gA.sessionStart) < KVM_ADAPT_BUDGET_WARM_MS || (int)(gA.warmUntil - now) > 0) { gA.bOver = gA.bUnder = 0; return; } // start of the session: the full image and its sharpening are not motion
	R = bk_rate();
	if ((long long)kbps * 100 > (long long)gA.budgetKbps * KVM_ADAPT_BUDGET_UP_PCT || starved)
	{
		int fast = (starved && gA.bkTok < -0.5 * R) ? 1 : 0;	// deep in debt: do not wait for a second window
		gA.bUnder = 0;
		if (fast || ++gA.bOver >= KVM_ADAPT_BUDGET_UP_WINDOWS)
		{
			int n = 1 + (gA.bkTok < -1.0 * R ? 1 : 0) + (gA.bkTok < -2.0 * R ? 1 : 0); // as many steps as the depth of the debt says (at most 3 per window)
			if (gA.bStep < KVM_ADAPT_BUDGET_MAX_STEP)
			{
				gA.bStep += n;
				if (gA.bStep > KVM_ADAPT_BUDGET_MAX_STEP) { gA.bStep = KVM_ADAPT_BUDGET_MAX_STEP; }
				if (gA.bSinceDown < 20) { gA.bCalmReq = (gA.bCalmReq * 2 > 40) ? 40 : gA.bCalmReq * 2; } // a step that was given back was needed again soon: wait longer next time
			}
			gA.bOver = 0;
		}
	}
	else if ((long long)kbps * 100 < (long long)gA.budgetKbps * KVM_ADAPT_BUDGET_DOWN_PCT && calm)
	{
		gA.bOver = 0;
		if (++gA.bUnder >= gA.bCalmReq && gA.bStep > 0) { gA.bStep--; gA.bUnder = 0; gA.bSinceDown = 0; }
	}
	else
	{
		gA.bOver = 0;
		gA.bUnder = 0;
	}
	if (gA.bStep > gA.bStepMax) { gA.bStepMax = gA.bStep; }
	if (gA.bStep != from)
	{
		gA.bSteps++;
		kvm_stats_log("adapt budget from=B%d to=B%d window_kbps=%d budget_kbps=%d starved=%d q_motion=%d fps_cap=%d calm_windows=%d since_start_ms=%lu", from, gA.bStep, kbps, gA.budgetKbps, starved, motion_quality(gA.level, gA.bStep), fps_cap(gA.bStep), gA.bCalmReq, (unsigned long)(now - gA.sessionStart));
	}
}

extern "C"
{

static void scale_update(DWORD now);

void kvm_adapt_set_budget(int kbps)
{
	InterlockedExchange(&gClientBudget, kbps < 0 ? 0 : kbps);
	InterlockedIncrement(&gClientBudgetSeq);
}

int kvm_adapt_user_scale(int ns)
{
	long long r;
	if (gUserScale == 0) { InterlockedExchange(&gUserScale, ns); }
	else if (ns != (int)gUserScale && !(gScaleMult != 1024 && (long long)ns == (long long)gUserScale * gScaleMult / 1024 && (DWORD)(timeGetTime() - gScaleSentTick) < 5000)) // (a viewer that sends back the scale it was given, within 5 s of the change, has not chosen a new one)
	{
		// the viewer chose another scale: that is the new base, the budget multiplier starts again. The same scale again (the viewer asks after every change of the size
		// of the picture) leaves the multiplier alone, otherwise it would undo every change the budget makes.
		InterlockedExchange(&gUserScale, ns);
		InterlockedExchange(&gScaleMult, 1024);
	}
	r = (long long)gUserScale * gScaleMult / 1024;
	if (r < 64) { r = 64; } else if (r > 4096) { r = 4096; }
	return (int)r;
}

void kvm_adapt_session_start(void)
{
	char env[16] = "";
	kvm_motion_free(&gA.motion);
	memset(&gA, 0, sizeof(gA));
	kvm_motion_init(&gA.motion);
	kvm_motion_set(&gA.motion, KVM_ADAPT_MOTION_MS, KVM_ADAPT_MOTION_STREAK, KVM_ADAPT_MOTION_TILES, KVM_ADAPT_VIDEO_NEAR, KVM_ADAPT_VIDEO_RECALL_MS);
	kvm_series_init(&gA.series);
	gA.envOff = (GetEnvironmentVariableA("MESH_KVM_ADAPT", env, sizeof(env)) > 0 && env[0] == '0') ? 1 : 0;
	gA.sessionOn = 1;
	gA.sessionStart = gA.winStart = gA.lastFrameTick = gA.tokenTick = timeGetTime();
	gA.now = gA.sessionStart;
	gA.gateOrigin = gA.sessionStart;
	gA.qUser = clamp_q(encCompression);
	gA.dueIn = 0x7FFFFFFF;
#ifdef KVM_VIDEO_ENABLED
	kvm_sharp_init(&gA.sharp, gA.sessionStart);
#endif
	gA.budgetKbps = KVM_ADAPT_BUDGET_KBPS; gA.hardFps = KVM_ADAPT_MOTION_FPS; gA.jpegOpt = KVM_ADAPT_JPEG_OPT;
	if (GetEnvironmentVariableA("MESH_KVM_BUDGET_KBPS", env, sizeof(env)) > 0 && env[0] >= '0' && env[0] <= '9') { gA.budgetKbps = atoi(env); gA.budgetSrc = 1; }
	if (GetEnvironmentVariableA("MESH_KVM_MOTION_FPS", env, sizeof(env)) > 0 && env[0] >= '0' && env[0] <= '9') { gA.hardFps = atoi(env); }
	if (GetEnvironmentVariableA("MESH_KVM_JPEG_OPT", env, sizeof(env)) > 0 && env[0] >= '0' && env[0] <= '3') { gA.jpegOpt = env[0] - '0'; }
	if (gA.budgetKbps < 0) { gA.budgetKbps = 0; }
	if (gClientBudget >= 0) { gA.budgetKbps = (int)gClientBudget; gA.budgetSrc = 2; }
	gA.seenSeq = gClientBudgetSeq;
	bk_reset(gA.sessionStart);
	gA.bytesPerTile = 1500.0; gA.seenRefresh = gRefreshTick;
	if (gUserScale == 0) { gUserScale = SCALING_FACTOR_NEW; }
	gA.scaleChangeTick = gA.sessionStart - KVM_ADAPT_SCALE_DOWN_MS;
	if (gA.hardFps < 0 || gA.hardFps > 1000) { gA.hardFps = 0; }
	gA.bCalmReq = KVM_ADAPT_BUDGET_DOWN_WINDOWS; gA.bSinceDown = 1000; gA.lastCap = -1; gA.qMotionMin = 100; gA.gateOpen = 1;
	kvm_stats_log("adapt start enabled=%d reason=%s q_user=%d type=%d q_motion=%d motion_ms=%d settle_ms=%d budget_kbps=%d budget_source=%s motion_fps=%d jpeg_opt=%d", gA.envOff ? 0 : 1, gA.envOff ? "env" : "on", gA.qUser, encType, KVM_ADAPT_Q_MOTION, KVM_ADAPT_MOTION_MS, KVM_ADAPT_SETTLE_MS, gA.budgetKbps, kBudgetSrc[gA.budgetSrc], gA.hardFps, gA.jpegOpt);
}

void kvm_adapt_session_end(void)
{
	gA.sessionOn = 0;
	kvm_motion_free(&gA.motion);
	if (gUserScale != 0) { SCALING_FACTOR_NEW = (int)gUserScale; } // the next session of this process must not take the reduced scale for the one of its viewer
	InterlockedExchange(&gUserScale, 0); InterlockedExchange(&gScaleMult, 1024);
	InterlockedExchange(&gClientBudget, -1); // the budget of this viewer ends with its session (a value that arrives before the next session starts is kept by the start)
}

// Regulator window and review of the tiles; called at the start of every iteration of the loop.
void kvm_adapt_iteration(void)
{
	DWORD now;
	if (!gA.sessionOn) { return; }
	now = timeGetTime();
	gA.now = now;

	if ((DWORD)(now - gA.winStart) >= KVM_ADAPT_WINDOW_MS)
	{
		double windowUs = (double)(DWORD)(now - gA.winStart) * 1000.0, frac = (double)gA.winWriteUs / windowUs;
		int pct = (int)(frac * 100.0), from = gA.level;
		if (pct > gA.blockPctMax) { gA.blockPctMax = pct; }
		if (frac >= 0.5 && gA.winBytes > 0)
		{
			double rate = (double)gA.winBytes * 1000.0 / (double)(DWORD)(now - gA.winStart); // the link took this much while it was the bottleneck
			gA.acceptRate = (gA.acceptRate > 0.0) ? (0.5 * gA.acceptRate + 0.5 * rate) : rate;
		}
		if (frac * 100.0 >= KVM_ADAPT_CONGEST_PCT)
		{
			gA.calm = 0;
			// From level 0 only after two overloaded windows in a row: the first full image of a session (or a REFRESH) blocks the pipe write
			// for one window although the link is fine; real congestion lasts longer.
			if (gA.level > 0 || ++gA.hot >= 2) { if (gA.level < KVM_ADAPT_MAX_LEVEL) { gA.level++; } }
		}
		else if (frac * 100.0 <= KVM_ADAPT_CALM_PCT)
		{
			gA.hot = 0;
			if (++gA.calm >= KVM_ADAPT_CALM_WINDOWS && gA.level > 0) { gA.level--; gA.calm = 0; }
		}
		else
		{
			gA.calm = 0;
			gA.hot = 0;
		}
		if (gA.level == 0) { gA.acceptRate = 0.0; }
		budget_window(now, (int)((double)gA.winBytes * 8.0 / (double)(DWORD)(now - gA.winStart)), (int)(DWORD)(now - gA.winStart));
		if (gA.level > gA.levelMax) { gA.levelMax = gA.level; }
		if (gA.level != from)
		{
			kvm_stats_log("adapt level from=%d to=%d block=%d.%d%% accept_kbps=%d q_motion=%d since_start_ms=%lu", from, gA.level, pct, (int)(frac * 1000.0) % 10, (int)(gA.acceptRate * 8.0 / 1000.0), motion_quality(gA.level, gA.bStep), (unsigned long)(now - gA.sessionStart));
			if (gA.level >= 4 && from < 4) { gA.tokens = 0.0; gA.tokenTick = now; kvm_stats_log("adapt rate_cap kbps=%d", (int)(rate_limit() * 8.0 / 1000.0)); }
		}
		gA.winStart = now;
		gA.winWriteUs = 0;
		gA.winBytes = 0;
	}

	// Review: how many tiles are lossy and when the first one is due for sharpening
	gA.lossyCount = 0;
	gA.dueIn = 0x7FFFFFFF;
	gA.deferredCount = 0;
	DWORD oldestDef = 0;
	if (tileInfo != NULL && TILE_WIDTH_COUNT > 0)
	{
		int qUser = clamp_q(encCompression);
		for (int row = 0; row < TILE_HEIGHT_COUNT; row++)
		{
			const tileInfo_t *r = tileInfo[row];
			for (int col = 0; col < TILE_WIDTH_COUNT; col++)
			{
				if (r[col].deferred) { gA.deferredCount++; if (oldestDef == 0 || (int)(r[col].deferredAt - oldestDef) < 0) { oldestDef = r[col].deferredAt; } }
				if (is_lossy(&r[col], qUser) && r[col].crc == r[col].contentCrc && !kvm_video_tile_pending(row, col))
				{
					int due = (int)(r[col].lastChange + KVM_ADAPT_SETTLE_MS - now);
					gA.lossyCount++;
					if (due < gA.dueIn) { gA.dueIn = due; }
				}
			}
		}
	}
	gA.oldestDeferred = oldestDef;
	if (gRefreshTick != gA.seenRefresh) { gA.seenRefresh = gRefreshTick; gA.warmUntil = now + KVM_ADAPT_BUDGET_WARM_MS; } // a REFRESH brings a full picture of a still screen: not movement
	scale_update(now);
}

// The scale of the picture: the ladder step says how far below the scale of the viewer the picture may go while the screen moves; a still screen goes back to the
// scale of the viewer. Changes are spaced (down KVM_ADAPT_SCALE_DOWN_MS, up KVM_ADAPT_SCALE_UP_MS while moving; back to the viewer's scale when still: at once).
static void scale_update(DWORD now)
{
	int wanted = 1024, desired, mult = (int)gScaleMult, act, calm, down, allowed = 1, recentInput;
	long long r;
	DWORD since;
	if (gUserScale == 0) { return; }
	act = (gA.sessionOn && !gA.envOff && type_ok()) ? 1 : 0;
	if (act && gA.budgetKbps > 0 && !kvm_video_on()) { wanted = ladder_scale(gA.bStep); } // (the step of 75 % is not used while the video runs, v1: every change of the size is a new generation)
	calm = ((DWORD)(now - gA.lastMotionChange) >= KVM_ADAPT_SCALE_RESTORE_MS) ? 1 : 0;
	desired = (wanted < 1024 && calm) ? 1024 : wanted;
	if (desired != mult)
	{
		down = desired < mult;
		since = (DWORD)(now - gA.scaleChangeTick);
		recentInput = kvm_sharp_recent(now, gLastInput, KVM_ADAPT_SCALE_INPUT_MS) ? 1 : 0;	// signed: the input thread may stamp it after `now` was read
		if (down && (since < KVM_ADAPT_SCALE_DOWN_MS || (int)(gA.scaleBanUntil - now) > 0)) { allowed = 0; }
		else if (!down && !(calm || !act || gA.budgetKbps <= 0) && since < KVM_ADAPT_SCALE_UP_MS) { allowed = 0; }
		else if (act && gA.budgetKbps > 0 && recentInput) { allowed = 0; } // the viewer is using the mouse or the keyboard: its clicks around a change of the size would land beside
		if (allowed && !kvm_video_scale_allowed(down)) { allowed = 0; } // v1: while the video runs or waits for its encoder the picture does not get smaller; the way back to the viewer's scale is allowed 10 s after the last generation
		if (allowed)
		{
			InterlockedExchange(&gScaleMult, desired);
			gA.scaleChangeTick = now; gScaleSentTick = now;
			gA.warmUntil = now + KVM_ADAPT_BUDGET_WARM_MS; // the full picture in the new size is not movement for the ladder
			if (!down && calm && wanted < 1024) { gA.scaleBanUntil = now + KVM_ADAPT_SCALE_BAN_MS; } // back after a pause: no new descent for a while
			gA.scaleChanges++;
			kvm_stats_log("adapt scale mult=%d from=%d viewer_scale=%d step=B%d calm=%d", desired, mult, (int)gUserScale, gA.bStep, calm);
		}
	}
	// the one place where the requested scale is set for the budget: viewer scale x share (the input thread sets it from the viewer's command, the same value)
	r = (long long)gUserScale * gScaleMult / 1024;
	if (r < 64) { r = 64; } else if (r > 4096) { r = 4096; }
	if (SCALING_FACTOR_NEW != (int)r) { SCALING_FACTOR_NEW = (int)r; }
}

int kvm_adapt_viewer_budget_kbps(void)
{
	// the budget the viewer asked for (command 5), also before the next window of the regulator has taken it over; else the one of the environment / the build
	return (gClientBudget >= 0) ? (int)gClientBudget : (gA.budgetKbps > 0 ? gA.budgetKbps : 0);
}

unsigned long long kvm_adapt_sharpen_bytes_total(void) { return gSharpenBytesAll; }

int kvm_adapt_ack_capable(void)
{
	// like type_ok(), but also before the first frame (PIXEL_SIZE is not known yet) and not depending on the frame being processed
	const int t = enc_type(); // read with an interlocked access: the input thread writes it. PIXEL_SIZE is written by the main loop thread, the one that calls this.
	return (gA.sessionOn && !gA.envOff && (t == 1 || t == 4) && (PIXEL_SIZE == 0 || PIXEL_SIZE == 3 || PIXEL_SIZE == 4)) ? 1 : 0;
}

int kvm_adapt_want_image(void)
{
	if (!gA.sessionOn) { return 0; }
	// A change held back by the motion frame limit must be looked at again even if nothing changes now, also when WA-2 is no longer active (another image
	// type, 15 or 16 bit pixels): the tile is then sent in the normal way in the next frame.
	// While the byte bucket is empty nothing could be sent anyway: the capture is not asked to hurry (the next frame or the next timeout looks again).
	if (gA.deferredCount > 0)
	{
		int hold = 0;
		if (gA.budgetKbps > 0 && gA.active) { bk_refill(timeGetTime()); hold = (gA.bkTok <= 0.0) ? 1 : 0; }
		if (!hold && gA.active && !kvm_ack_peek(KVM_ACK_BIG)) { hold = 1; } // the window of the viewer is full too: nothing could be sent now
		if (!hold) { gA.wantImage++; return 1; }
	}
	if (kvm_video_want_image()) { gA.wantImage++; return 1; } // tiles wait for the next video frame and the window allows it: a picture is needed even if nothing changed
	if (gA.envOff || !type_ok()) { return 0; }
	if (gA.lossyCount > 0 && gA.dueIn <= 0 && kvm_sharp_settled(gA.now, gA.lastContentChange, KVM_ADAPT_SETTLE_MS) && kvm_ack_peek(KVM_ACK_SHARPEN)) // while the screen changes, new frames come by themselves
	{
		gA.wantImage++;
		return 1;
	}
	return 0;
}

void kvm_adapt_note_source(int srcClass, int full)
{
	// only a complete frame can show the difference between two sources of pixels; the first frame of a session has nothing before it to differ from
	gA.srcChange = (full && gA.haveSrc && srcClass != gA.lastSrc) ? 1 : 0;
	gA.lastSrc = srcClass; gA.haveSrc = 1;
}

void kvm_adapt_frame_begin(void)
{
	DWORD now = timeGetTime();
	gA.now = now;
	gA.qUser = clamp_q(encCompression);
	gA.active = (gA.sessionOn && !gA.envOff && type_ok()) ? 1 : 0;
	if (++gA.stamp == 0) { gA.stamp = 1; } // 0 is the value of a tile that was never looked at
	// A frame in which no tile changed (the capture was asked for an image only to look at held-back or lossy tiles again) keeps the count of the last
	// frame that changed something; otherwise the next frame of a moving picture would be taken for "no motion" and sent in full quality.
	if (gA.motion.rows != TILE_HEIGHT_COUNT || gA.motion.cols != TILE_WIDTH_COUNT) { (void)kvm_motion_resize(&gA.motion, TILE_HEIGHT_COUNT, TILE_WIDTH_COUNT); } // the grid of the stamps follows the picture
	kvm_motion_frame(&gA.motion, gA.lastFrameTick, (unsigned)gA.realChanged); // the frame of motion of the last 250 ms (the video's rule looks at it instead of the frame before this one)
	if (gA.realChanged >= KVM_ADAPT_MOTION_TILES) { gA.lastMotionChange = gA.lastFrameTick; } // movement: a frame with many tiles that really changed (not a caret, a clock, or a picture seen for the first time)
	gA.realChanged = 0;
	if ((DWORD)(now - gA.lastFrameTick) > KVM_ADAPT_MOTION_MS) { gA.prevChanged = 0; }
	else if (gA.changedThisFrame > 0) { gA.prevChanged = gA.changedThisFrame; }
	gA.changedThisFrame = 0;
	gA.lastFrameTick = now;
	gA.levelFrame = gA.level;
	gA.bStepFrame = gA.bStep;
	gA.motionSentFrame = 0;
	gA.deferredFrame = 0;
	{
		int cap = fps_cap(gA.bStepFrame);
		double t;
		if ((DWORD)(now - gA.gateOrigin) > 0x40000000UL) { gA.motionNext -= (double)(DWORD)(now - gA.gateOrigin); gA.gateOrigin = now; } // about every 12 days
		t = (double)(DWORD)(now - gA.gateOrigin);
		if (cap != gA.lastCap) { gA.lastCap = cap; gA.motionNext = t; }
		// a frame may come up to half a loop period early, so that the average rate holds the limit when the loop period does not divide the interval
		gA.gateOpen = (cap <= 0 || t - gA.motionNext >= -(double)(FRAME_RATE_TIMER / 2)) ? 1 : 0;
	}
}

void kvm_adapt_note_crc(struct tileInfo_t *t, int crc)
{
	if (!gA.active || t->noted == gA.stamp) { return; }
	t->noted = gA.stamp;
	if (t->crc == 0xFF) { t->refresh = 1; } // sent only because the viewer was reset, not because of a change
	if (crc != t->contentCrc)
	{
		// the first complete frame from another source of pixels (the fill from GDI, then the first copy of DXGI; a probe of DXGI; DXGI -> GDI) differs from the picture before it in a
		// part of the tiles for no reason on the screen: that is not a change of the content, so no streak, not moved, not a frame of motion
		t->streak = (unsigned char)kvm_motion_note_change(&gA.motion, t->streak, kvm_sharp_elapsed_ms(gA.now, t->lastChange), gA.srcChange);
		t->lastChange = gA.now;
		if (!gA.srcChange)
		{
			if (t->contentCrc != 0x7FFFFFFF)	// a tile that was never looked at (new geometry, new session) is not motion
			{
				gA.realChanged++;
#ifdef KVM_VIDEO_ENABLED
				if (gA.realChanged == KVM_ADAPT_MOTION_TILES && kvm_video_on()) { kvm_series_motion(&gA.series, gA.now, (uint32_t)kvm_video_r_kbps()); }	// a frame of motion
#endif
			}
			gA.changedThisFrame++;
		}
		t->contentCrc = crc;
		gA.lastContentChange = gA.now;
		// the smoothness work, the safety net: a tile that still differs after a change of the source (with the alpha byte the same on both paths it is a real difference in colour, if any) is not sent at once. The
		// viewer is taken to have it (crc), the tile is marked lossy, and the sharpening repaints it 150 ms after, behind the motion and with the share of the bucket (never as an IDR or a sharp JPEG in front of it)
		if (kvm_motion_source_absorb(gA.srcChange, t->crc == 0xFF)) { t->crc = crc; t->sentQ = 1; t->sentSub = 1; gA.srcDiff++; }
	}
}

int kvm_adapt_class(const struct tileInfo_t *t, int row, int col)
{
	if (!gA.active) { return 0; }
	if (t->deferred == 1) { return 1; } // held back as motion, it is sent as motion (and sharpened later)
	if (t->deferred == 2) { return 0; } // held back by the window in the frame of another source of pixels: it is a picture, not motion, and goes as one when the window allows (not as video)
	if (kvm_video_on())
	{
		// with the video running a tile is motion when it has changed twice in a row while a frame of motion was seen in the last 250 ms (not "in the frame before this one"), or when it is
		// within two tiles of one that is (the first change of the edge of a dragged window, the next frame of an animation). A tile that is sent only because of a REFRESH is not (a JPEG of the user's quality)
		if (!t->refresh && kvm_motion_class(&gA.motion, gA.now, row, col, t->streak, kvm_sharp_elapsed_ms(gA.now, t->lastChange))) { return 1; }
	}
	else if (t->streak >= KVM_ADAPT_MOTION_STREAK && kvm_sharp_elapsed_ms(gA.now, t->lastChange) <= (int32_t)KVM_ADAPT_MOTION_MS && gA.prevChanged >= KVM_ADAPT_MOTION_TILES) { return 1; }
	if (gA.levelFrame >= 1 && t->refresh) { return 1; } // under congestion a refresh is sent in the motion class and sharpened later
	return 0;
}

void kvm_adapt_params(int cls, int motionSub, int fullSub, int *q, int *sub)
{
	if (cls == 1 && gA.active)
	{
		int qm = motion_quality(gA.levelFrame, gA.bStepFrame);
		*q = gA.qUser < qm ? gA.qUser : qm;
		*sub = motionSub;
	}
	else
	{
		*q = gA.active ? gA.qUser : clamp_q(encCompression);
		*sub = fullSub;
	}
}

void kvm_adapt_sent(int row0, int col0, int row1, int col1, int cls, int q, int lossySub, long long bytes)
{
	for (int r = row0; r <= row1; r++)
	{
		for (int c = col0; c <= col1; c++)
		{
			tileInfo_t *t = &tileInfo[r][c];
			if (gA.active) { t->sentQ = (unsigned char)q; t->sentSub = (unsigned char)(lossySub ? 1 : 0); } else { t->sentQ = 100; t->sentSub = 0; } // not JPEG: nothing to sharpen
			if (t->refresh && cls == 1) { t->lastChange = gA.now; } // a refresh sent lossy waits for the settle time, it is not sharpened in the same frame
			t->refresh = 0;
			t->deferred = 0;
			t->deferredAt = 0;
			kvm_video_tile_jpeg(r, c); // sent as JPEG: not pending for the video, not a VIDEO tile (I3)
		}
	}
	{
		int nt = (row1 - row0 + 1) * (col1 - col0 + 1);
		if (nt > 0 && bytes > 0) { double per = (double)bytes / (double)nt; gA.bytesPerTile = gA.bytesPerTile > 0.0 ? 0.9 * gA.bytesPerTile + 0.1 * per : per; }
#ifdef KVM_VIDEO_ENABLED
		if (nt > 0 && kvm_video_on()) { if (gA.iterJpegTiles == 0) { gA.iterJpegAt = gA.now; } gA.iterJpegTiles += (unsigned)nt; } // a JPEG paint of this iteration
#endif
	}
	if (cls == 1)
	{
		gA.rectsMotion++; gA.bytesMotion += bytes;
		if (q < gA.qMotionMin) { gA.qMotionMin = q; }
		gA.qMotionSum += q; gA.qMotionN++;
		if (gA.active && !gA.motionSentFrame)
		{
			int cap = gA.lastCap;
			gA.motionSentFrame = 1;
			if (cap > 0 && gA.gateOpen)
			{
				double t = (double)(DWORD)(gA.now - gA.gateOrigin), interval = 1000.0 / (double)cap;
				gA.motionNext += interval;
				if (t > gA.motionNext) { gA.motionNext = t + interval; } // far behind (idle for a while): no burst of frames to catch up
			}
		}
	}
	else { gA.rectsFull++; gA.bytesFull += bytes; }
}

int kvm_adapt_gate_closed(int cls)
{
	return (gA.active && cls == 1 && !gA.gateOpen) ? 1 : 0;
}

void kvm_adapt_defer(int row0, int col0, int row1, int col1, int cls)
{
	for (int r = row0; r <= row1; r++)
	{
		for (int c = col0; c <= col1; c++)
		{
			tileInfo_t *t = &tileInfo[r][c];
			if (!t->deferred) { t->deferredAt = timeGetTime(); if (t->deferredAt == 0) { t->deferredAt = 1; } if (gA.oldestDeferred == 0 || (int)(t->deferredAt - gA.oldestDeferred) < 0) { gA.oldestDeferred = t->deferredAt; } }
			t->deferred = (t->deferred == 2 || (cls == 0 && gA.srcChange && kvm_video_on())) ? 2 : 1; // 2: a full-class tile of the frame of another source stays full class when it is looked at again
			gA.deferredTiles++;
		}
	}
	gA.deferredRects++;
	gA.deferredFrame = 1;
}

int kvm_adapt_frame_deferred(void) { return gA.deferredFrame; }

int kvm_adapt_jpeg_opt(int sharpen, int tiles)
{
	if (sharpen) { return gA.jpegOpt >= 1; }
	if (gA.jpegOpt == 2) { return 1; }
	return (gA.jpegOpt == 3 && tiles <= KVM_ADAPT_JPEG_OPT_TILES) ? 1 : 0;
}

void kvm_adapt_note_loop(unsigned int waitUs, unsigned int procUs, unsigned int pauseUs)
{
	gA.loopN++; gA.loopWaitUs += waitUs; gA.loopProcUs += procUs; gA.loopPauseUs += pauseUs;
}

void kvm_adapt_note_encode(int sharpen, unsigned int us, int tiles)
{
	if (sharpen) { gA.sharpEncUs += us; gA.sharpEncTiles += tiles; gA.sharpEncN++; }
	else { gA.encUs += us; gA.encTiles += tiles; gA.encN++; }
}

#ifdef KVM_VIDEO_ENABLED
// the JPEG rectangles of the iteration (if any) are one picture, written to the series at the time of the first of them
static void series_flush_jpeg(void)
{
	if (gA.iterJpegTiles > 0) { kvm_series_write(&gA.series, gA.iterJpegAt, 0, gA.iterJpegTiles, (uint32_t)kvm_video_interval_ms()); gA.iterJpegTiles = 0; }
}
#endif

static long long sharpen_budget_normal(void)
{
	long long b;
	if (!gA.active) { return 0; }
	if (!kvm_ack_peek(KVM_ACK_SHARPEN)) { if (gA.lossyCount > 0) { kvm_ack_note_held_back(); } return 0; } // sharpening only below half of the window of the viewer; the refusal feeds the rate samples only, not "link limited" (the 0.5 W gate says nothing about the capacity)
	b = KVM_ADAPT_SHARPEN_BYTES;
	if (gA.level >= 4) { b /= 4; } else if (gA.level >= 1) { b /= 2; }
	if (gA.budgetKbps > 0)
	{
		// the budget: sharpening gets its reserved part of the rate and what the bucket of the motion has left (it always finishes: the reserved part keeps coming)
		double avail;
		bk_refill(timeGetTime());
		// with held-back changes waiting, sharpening has only its own reserved share (the positive tokens of the main bucket are for them)
		avail = (gA.bkSTok > 0.0 ? gA.bkSTok : 0.0) + ((gA.bkTok > 0.0 && gA.deferredCount == 0) ? gA.bkTok : 0.0);
		if (avail <= 0.0) { return 0; }
		if ((double)b > avail) { b = (long long)avail; if (b < 1) { b = 1; } }
	}
	if (gA.level >= 4 && rate_limit() > 0.0)
	{
		refill_tokens(timeGetTime());
		if (gA.tokens <= 0.0) { return 0; }
		if ((double)b > gA.tokens) { b = (long long)gA.tokens; }
	}
	return b;
}

// the first video path: while the video runs, the tiles it painted are blurred until they are sharpened, and the sharpening is what makes the picture converge. A closed
// window (the video fills it), an empty bucket or the token limit must not hold it back for ever: once a tile has been due for KVM_VIDEO_SHARPEN_WAIT_MS without a single packet of the
// normal budget, a packet goes anyway. Its size and rhythm come from kvm_vcore_force_sharpen: 20 % of the delivery rate at most (8.5), 4-16 KiB, and not while the window is more than
// twice full (the review of a fixed 64 KiB/s would have been all of a 0.5 Mb/s link).
static long long video_sharpen_force(void)
{
#ifdef KVM_VIDEO_ENABLED
	DWORD now;
	unsigned long long rate = 0, inflight = 0;
	unsigned w = 0;
	uint32_t bytes = 0;
	if (!kvm_video_on() || gA.lossyCount <= 0 || gA.dueIn > 0) { gA.sharpDueSince = 0; return 0; }
	now = timeGetTime();
	if (gA.sharpDueSince == 0) { gA.sharpDueSince = (now != 0) ? now : 1; return 0; }
	if ((DWORD)(now - gA.sharpDueSince) < KVM_VIDEO_SHARPEN_WAIT_MS) { return 0; }
	if (!kvm_ack_force_info(&rate, &inflight, &w)) { return 0; }
	if (!kvm_vcore_force_sharpen(rate, kvm_adapt_viewer_budget_kbps() > 0 ? (unsigned)kvm_adapt_viewer_budget_kbps() : 0u, inflight, w, gA.sharpForcedAt != 0, gA.sharpForcedAt, now, &bytes)) { return 0; }
	return (long long)bytes;
#else
	return 0;
#endif
}

// the sharpening governor: while the video runs, how much of the iteration's budget the sharpening may send (the bucket of its share, a quarter of the window), and the state for the packets (kvm_sharp_*).
// The rate is the smaller of the delivery rate and the viewer's budget. Returns the budget (0 = nothing now).
static long long governed_budget(long long b)
{
#ifdef KVM_VIDEO_ENABLED
	unsigned long long rate = 0, inflight = 0;
	unsigned w = 0;
	const DWORD now = timeGetTime();
	const int budget = kvm_adapt_viewer_budget_kbps();
	if (!kvm_video_on()) { return b; }
	if (kvm_video_pending() > 0) { kvm_sharp_motion(&gA.sharp, now); }
	if (!kvm_ack_force_info(&rate, &inflight, &w)) { rate = 0; w = 0; }
	if (budget > 0 && (rate == 0 || rate > (unsigned long long)budget * 125ull)) { rate = (unsigned long long)budget * 125ull; }
	return (long long)kvm_sharp_begin(&gA.sharp, now, rate, w, b);
#else
	return b;
#endif
}

long long kvm_adapt_sharpen_budget(void)
{
	long long b;
	if (!gA.active) { return 0; }
	gA.now = timeGetTime();	// one clock for the frame and for the sharpening that follows it (a tile painted by the frame has a time stamp after the start of the iteration)
#ifdef KVM_VIDEO_ENABLED
	if (kvm_video_on())
	{
		// a frame with many real changes is a movement however it went out (JPEG too: the first frame of an animation, the edge of a window), and a movement lasts max(3 intervals, 100 ms)
		series_flush_jpeg();
		kvm_sharp_set_interval(&gA.sharp, (uint32_t)kvm_video_interval_ms());
		if (gA.realChanged >= KVM_ADAPT_MOTION_TILES) { kvm_sharp_motion(&gA.sharp, gA.now); }
	}
#endif
	if (gA.srcChange) { kvm_stats_log("adapt source_change tiles_differ=%u tiles=%d alpha_cleared=1", gA.srcDiff, TILE_WIDTH_COUNT * TILE_HEIGHT_COUNT); gA.srcDiff = 0; }	// how many tiles the first frame of another source still has with another hash
	gA.sharpForcing = 0;
	b = sharpen_budget_normal();
	if (b > 0) { b = governed_budget(b); }
	if (b <= 0)
	{
		const long long f = video_sharpen_force();
		if (f > 0) { gA.sharpForcing = 1; gA.sharpForcedAt = timeGetTime(); gA.sharpForcedCount++; b = f; }
	}
	return b;
}

int kvm_adapt_sharpen_next(void *desktop, long long budgetLeft, void **packet, long long *size)
{
	int total, n;
	if (!gA.active || budgetLeft <= 0 || tileInfo == NULL || TILE_WIDTH_COUNT <= 0) { return 0; }
#ifdef KVM_VIDEO_ENABLED
	if (kvm_video_on() && !gA.sharpForcing)
	{
		// the window is looked at before EVERY packet (inflight < 0.5 W), not once per iteration: 96 KiB went out after a single look (the forced packet has its own limits)
		unsigned long long rate = 0, inflight = 0;
		unsigned w = 0;
		if (kvm_ack_force_info(&rate, &inflight, &w) && !kvm_sharp_window_ok_at(&gA.sharp, timeGetTime(), inflight, w)) { if (gA.lossyCount > 0) { kvm_ack_note_held_back(); } return 0; } // (a quarter of W for a second after a movement)
	}
#endif
	total = TILE_WIDTH_COUNT * TILE_HEIGHT_COUNT;
	if (gA.cursor >= total || gA.cursor < 0) { gA.cursor = 0; }
	for (n = 0; n < total; n++)
	{
		int idx = (gA.cursor + n) % total, row = idx / TILE_WIDTH_COUNT, col = idx % TILE_WIDTH_COUNT, run = 1, q = 0;
		void *buf = NULL;
		long long sz = 0;
		if (!is_candidate(&tileInfo[row][col], row, col)) { continue; }
		while (run < KVM_ADAPT_SHARPEN_RUN && col + run < TILE_WIDTH_COUNT && is_candidate(&tileInfo[row][col + run], row, col + run)) { run++; }
		if (kvm_adapt_encode_sharp(col * TILE_WIDTH, row * TILE_HEIGHT, run * TILE_WIDTH, TILE_HEIGHT, desktop, &buf, &sz, &q) != 0 || buf == NULL)
		{
			// The encoder failed: wait another settle time before the next try
			for (int c = 0; c < run; c++) { tileInfo[row][col + c].lastChange = gA.now; }
			if (buf != NULL) { free(buf); }
			n += run - 1;
			continue;
		}
		for (int c = 0; c < run; c++)
		{
			tileInfo_t *t = &tileInfo[row][col + c];
			long long settle = (long long)kvm_sharp_elapsed_ms(gA.now, t->lastChange);
			if (settle < 0) { settle = 0; }
			t->sentQ = (unsigned char)q; t->sentSub = 0; t->refresh = 0;
			kvm_video_tile_jpeg(row, col + c); // a VIDEO tile that is sharpened is a JPEG tile again (I3)
			gA.settleSum += settle; gA.settleN++;
			if (settle > gA.settleMax) { gA.settleMax = settle; }
		}
		gA.sharpenRects++; gA.sharpenTiles += run; gA.sharpenBytes += sz; gSharpenBytesAll += (unsigned long long)sz;
		if (!gA.sharpForcing) { gA.sharpDueSince = 0; } // progress by the normal budget: the time limit starts again (a forced packet does not count)
		gA.cursor = (idx + run) % total;
		*packet = buf;
		*size = sz;
		return 1;
	}
	return 0;
}

int kvm_adapt_bucket_closed(int tiles, unsigned int rectOldest)
{
	DWORD now;
	DWORD oldest;
	if (!gA.active) { return 0; }
	// the window of the viewer (end-to-end ACK): a rectangle that does not fit is held back like one the byte bucket refuses (the tiles are "deferred" and looked at again)
	if (!kvm_ack_gate(tiles <= KVM_ADAPT_BUCKET_SMALL_TILES ? KVM_ACK_SMALL : KVM_ACK_BIG)) { return 1; }
	if (gA.budgetKbps <= 0) { return 0; }
	now = timeGetTime();
	bk_refill(now);
	gA.lastPassSmall = 0;
	// small rectangles (typing, the cursor, small animations) have a reserved share of the rate: they do not wait in the queue of the large ones, and cannot starve them
	if (tiles <= KVM_ADAPT_BUCKET_SMALL_TILES && gA.bkSmall > 0.0 && gA.bkTok > -bk_rate() * KVM_ADAPT_BUCKET_SMALL_GATE_MS / 1000.0) { gA.lastPassSmall = 1; return 0; }
	// An old held-back change is served in the order of its age, not of its place on the screen (the scan goes from the top left: otherwise what moves at the top of the
	// screen would be sent again and again before anything below): once the oldest has waited KVM_ADAPT_BUCKET_AGE_MS only a rectangle with a tile that was held back
	// no later than the oldest one (+ a tolerance) passes; a new change waits behind it (it is held back, and takes its place in the queue by its time).
	oldest = gA.oldestDeferred;
	if (oldest != 0 && (DWORD)(now - oldest) > KVM_ADAPT_BUCKET_AGE_MS)
	{
		if (rectOldest == 0 || (int)(rectOldest - oldest) > KVM_ADAPT_BUCKET_AGE_TOL_MS) { gA.bucketDefers++; gA.bucketDefersTotal++; return 1; }
	}
	if (gA.bkTok <= 0.0) { gA.bucketDefers++; gA.bucketDefersTotal++; return 1; }
	return 0;
}

int kvm_adapt_max_rect_tiles(void)
{
	double n;
	if (!gA.active || gA.budgetKbps <= 0) { return 0; }
	n = bk_rate() * 0.5 / (gA.bytesPerTile > 100.0 ? gA.bytesPerTile : 100.0); // about half a second of the budget per rectangle
	if (n > KVM_ADAPT_BUCKET_MAX_TILES) { n = KVM_ADAPT_BUCKET_MAX_TILES; }
	if (n < 4.0) { n = 4.0; }
	return (int)n;
}

void kvm_adapt_note_input(void) { gLastInput = timeGetTime(); }
void kvm_adapt_note_refresh(void) { gRefreshTick = timeGetTime(); }

int kvm_adapt_force_motion(int tiles)
{
	return (gA.active && gA.budgetKbps > 0 && gA.bStepFrame >= KVM_ADAPT_ALLMOTION_STEP && tiles > KVM_ADAPT_BUCKET_SMALL_TILES) ? 1 : 0;
}

void kvm_adapt_note_video_frame(void)
{
#ifdef KVM_VIDEO_ENABLED
	if (gA.sessionOn)
	{
		const DWORD now = timeGetTime();
		kvm_sharp_motion(&gA.sharp, now);
		series_flush_jpeg();
		kvm_series_write(&gA.series, now, 1, 0, (uint32_t)kvm_video_interval_ms());
	}
#endif
}

void kvm_adapt_note_write(unsigned int us, long long bytes, int sharpen)
{
	gA.winWriteUs += us;
	gA.winBytes += bytes;
#ifdef KVM_VIDEO_ENABLED
	if (sharpen && gA.sessionOn && kvm_video_on())
	{
		// the packet is charged to the bucket of the share (a forced one pays but is never refused) and to R of the video while the video moves
		const DWORD now = timeGetTime();
		if (gA.sharpForcing) { kvm_sharp_forced(&gA.sharp, now, bytes); } else { kvm_sharp_sent(&gA.sharp, now, bytes); }
		kvm_video_note_sharpen((unsigned)(bytes > 0 ? bytes : 0), kvm_sharp_active(&gA.sharp, now));
	}
#endif
	if (gA.active && gA.budgetKbps > 0) { bk_debit(timeGetTime(), (double)bytes, sharpen); }
	if (gA.level >= 4) { gA.tokens -= (double)bytes; }
}

int kvm_adapt_frame_pause(int base)
{
	int p = base, mult = (gA.level >= 5) ? 4 : 2;
	if (!gA.sessionOn || gA.envOff || gA.level < 4) { gA.pauseMult = 1; return base; } // up to level 3 only the quality of the motion class changes
	p = base * mult;
	if (p > KVM_ADAPT_MAX_PAUSE_MS) { p = (base > KVM_ADAPT_MAX_PAUSE_MS) ? base : KVM_ADAPT_MAX_PAUSE_MS; }
	if (rate_limit() > 0.0)
	{
		refill_tokens(timeGetTime());
		if (gA.tokens < 0.0)
		{
			int wait = (int)(-gA.tokens * 1000.0 / rate_limit());
			if (wait > KVM_ADAPT_MAX_PAUSE_MS) { wait = KVM_ADAPT_MAX_PAUSE_MS; }
			if (wait > p) { p = wait; gA.rateSkips++; }
		}
	}
	gA.pauseMult = mult;
	return p;
}

void kvm_adapt_stats_flush(int final)
{
	if (!gA.sessionOn) { return; }
	kvm_stats_log("adapt stats%s enabled=%d level_end=%d level_max=%d q_motion=%d rects_motion=%lld rects_full=%lld bytes_motion=%lld bytes_full=%lld sharpen_rects=%lld sharpen_tiles=%lld sharpen_bytes=%lld want_image=%d lossy_end=%d settle_ms_avg=%lld settle_ms_max=%lld block_pct_max=%d accept_kbps=%d pause_mult=%d rate_skips=%d"
		" bucket_forgiven_kb=%d scale_mult=%d scale_changes=%d budget_kbps=%d budget_source=%s bucket_defers=%lld bucket_tok_kb=%d win_kbps_avg=%lld win_kbps_max=%d bstep_end=%d bstep_max=%d bsteps=%d q_motion_min=%d q_motion_avg=%lld motion_fps_cap=%d deferred_tiles=%lld deferred_rects=%lld enc_rects=%lld enc_us_rect=%lld enc_us_tile=%lld sharp_enc_rects=%lld sharp_enc_us_rect=%lld sharp_enc_us_tile=%lld loop_n=%lld loop_wait_us=%lld loop_proc_us=%lld loop_pause_us=%lld",
		final ? " final=1" : "", gA.envOff ? 0 : 1, gA.level, gA.levelMax, motion_quality(gA.level, gA.bStep), gA.rectsMotion, gA.rectsFull, gA.bytesMotion, gA.bytesFull, gA.sharpenRects, gA.sharpenTiles, gA.sharpenBytes,
		gA.wantImage, gA.lossyCount, gA.settleN > 0 ? gA.settleSum / gA.settleN : 0LL, gA.settleMax, gA.blockPctMax, (int)(gA.acceptRate * 8.0 / 1000.0), gA.pauseMult ? gA.pauseMult : 1, gA.rateSkips,
		(int)(gA.bkForgiven / 1024.0), (int)gScaleMult, gA.scaleChanges, gA.budgetKbps, kBudgetSrc[gA.budgetSrc], gA.bucketDefers, (int)(gA.bkTok / 1024.0), gA.winN > 0 ? gA.winKbpsSum / gA.winN : 0LL, gA.winKbpsMax, gA.bStep, gA.bStepMax, gA.bSteps, gA.qMotionN > 0 ? gA.qMotionMin : 0, gA.qMotionN > 0 ? gA.qMotionSum / gA.qMotionN : 0LL, gA.lastCap > 0 ? gA.lastCap : 0,
		gA.deferredTiles, gA.deferredRects, gA.encN, gA.encN > 0 ? gA.encUs / gA.encN : 0LL, gA.encTiles > 0 ? gA.encUs / gA.encTiles : 0LL, gA.sharpEncN, gA.sharpEncN > 0 ? gA.sharpEncUs / gA.sharpEncN : 0LL, gA.sharpEncTiles > 0 ? gA.sharpEncUs / gA.sharpEncTiles : 0LL,
		gA.loopN, gA.loopN > 0 ? gA.loopWaitUs / gA.loopN : 0LL, gA.loopN > 0 ? gA.loopProcUs / gA.loopN : 0LL, gA.loopN > 0 ? gA.loopPauseUs / gA.loopN : 0LL);
	if (gA.sharpForcedCount > 0) { kvm_stats_log("adapt sharpen_forced=%lld", gA.sharpForcedCount); gA.sharpForcedCount = 0; }
#ifdef KVM_VIDEO_ENABLED
	{
		// one short line a period when there was motion with the video on: the series, the gaps between the pictures, the share of JPEG, the classes of the tiles
		char line[400];
		if (kvm_video_on() && kvm_series_format(&gA.series, timeGetTime(), (uint32_t)kvm_video_interval_ms(), line, sizeof(line)) > 0)
		{
			kvm_stats_log("%s class_streak=%u class_near=%u class_plain=%u", line, (unsigned)gA.motion.sStreak, (unsigned)gA.motion.sNear, (unsigned)gA.motion.sPlain);
		}
		kvm_motion_clear_stats(&gA.motion);
	}
#endif
#ifdef KVM_VIDEO_ENABLED
	if (gA.sharp.sActiveIters > 0 || gA.sharp.sBytesActive > 0 || gA.sharp.sBytesIdle > 0 || gA.sharp.sBytesForced > 0)
	{
		// the governor of the sharpening while the video moves (kb: bytes sent in a movement / outside it / forced; iterations: with and without a movement; refusals: the bucket was empty / a quarter of the window was reached / the window gate of a packet)
		kvm_stats_log("adapt sharp_gov active_kb=%llu idle_kb=%llu forced_kb=%llu iters_active=%u iters_idle=%u bucket_refused=%u iter_capped=%u window_refused=%u bucket_kb=%d",
					  (unsigned long long)(gA.sharp.sBytesActive / 1024), (unsigned long long)(gA.sharp.sBytesIdle / 1024), (unsigned long long)(gA.sharp.sBytesForced / 1024), gA.sharp.sActiveIters, gA.sharp.sIdleIters,
					  gA.sharp.sBucketRefused, gA.sharp.sIterCapped, gA.sharp.sWindowRefused, (int)(gA.sharp.tokens / 1024));
		kvm_sharp_clear_stats(&gA.sharp);
	}
#endif
	gA.levelMax = gA.level; gA.wantImage = 0; gA.rateSkips = 0;
	gA.rectsMotion = gA.rectsFull = gA.bytesMotion = gA.bytesFull = gA.sharpenRects = gA.sharpenTiles = gA.sharpenBytes = gA.settleSum = gA.settleN = gA.settleMax = 0;
	gA.blockPctMax = 0;
	gA.bucketDefers = 0; gA.scaleChanges = 0; gA.bkForgiven = 0.0;
	gA.winKbpsSum = gA.winN = 0; gA.winKbpsMax = 0; gA.bStepMax = gA.bStep; gA.bSteps = 0; gA.qMotionMin = 100; gA.qMotionSum = gA.qMotionN = 0;
	gA.deferredTiles = gA.deferredRects = gA.encUs = gA.encTiles = gA.encN = gA.sharpEncUs = gA.sharpEncTiles = gA.sharpEncN = 0;
	gA.loopN = gA.loopWaitUs = gA.loopProcUs = gA.loopPauseUs = 0;
}

} // extern "C"

#endif // KVM_ADAPT_ENABLED
