/*
KVM: the Auto mode of the video frame rate. See kvm_auto.h. Portable C99, no operating system API.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#include "kvm_auto.h"
#include <stdio.h>
#include <string.h>

static const uint8_t kFps[KVM_AUTO_STEPS] = { 60, 30, 20, 15, 10 };
static const uint8_t kMs[KVM_AUTO_STEPS] = { 16, 33, 50, 66, 100 };
// the interval that the rate really is, in microseconds: 1000 / 60 is 16.67 ms, not the 16 that the loop is given (the cost thresholds count from this one)
static const uint32_t kUs[KVM_AUTO_STEPS] = { 16667, 33333, 50000, 66667, 100000 };

// does a cost of `costMs` milliseconds not fit an interval of `us` microseconds (the cost is more than interval / KEEP: the uneven 45 of 60 fits)
static int over(uint32_t costMs, uint32_t us) { return (uint64_t)costMs * 1000u * (uint64_t)KVM_AUTO_KEEP_PCT > (uint64_t)us * 100u; }
// the ceiling of the mean QP of the motion for a step: UI (scrolling, dragging, animations) and video
static const uint8_t kCeil[2][KVM_AUTO_STEPS] = { { 32, 32, 34, 36, 38 }, { 34, 34, 36, 38, 38 } };

static int32_t dms(uint32_t a, uint32_t b) { return (int32_t)(a - b); }
// the milliseconds since `then`: a `then` that is a little in the future (a note that is a moment late) is 0; one that is far in the future is a clock that has gone round the 32 bits (24.9 days of a
// process): long ago. dms() alone would read it as "in 24.9 days".
static uint32_t since(uint32_t now, uint32_t then)
{
	const int32_t d = dms(now, then);
	if (d >= 0) { return (uint32_t)d; }
	return d > -(int32_t)KVM_AUTO_FUTURE_MS ? 0u : 0x7FFFFFFFu;
}
static int clamp_step(int s) { return s < 0 ? 0 : (s >= KVM_AUTO_STEPS ? KVM_AUTO_STEPS - 1 : s); }

int kvm_auto_step_fps(int step) { return kFps[clamp_step(step)]; }
uint32_t kvm_auto_step_ms(int step) { return kMs[clamp_step(step)]; }
int kvm_auto_qp_ceiling(int cls, int step) { return kCeil[cls == KVM_AUTO_CLASS_VIDEO ? 1 : 0][clamp_step(step)]; }

const char *kvm_auto_reason_name(int reason)
{
	switch (reason)
	{
	case KVM_AUTO_R_START: return "start";
	case KVM_AUTO_R_LINK: return "link";
	case KVM_AUTO_R_CPU: return "cpu";
	case KVM_AUTO_R_CAPTURE: return "capture";
	case KVM_AUTO_R_SCREEN: return "screen";
	case KVM_AUTO_R_POWER: return "power";
	case KVM_AUTO_R_BUDGET: return "budget";
	case KVM_AUTO_R_BACK: return "back";
	default: return "none";
	}
}

static void reset(kvm_auto *a)
{
	memset(a, 0, sizeof(*a));
	a->step = 1;	// until the start: the interval of Medium, which is what command 5 asks for with the bit
	a->upNeed = (uint32_t)KVM_AUTO_UP_WINDOWS;
}

void kvm_auto_init(kvm_auto *a) { reset(a); }

void kvm_auto_set(kvm_auto *a, int on, uint32_t now)
{
	if (on)
	{
		if (a->on) { return; }
		reset(a);
		a->on = 1;
		a->beginAt = now;
	}
	else { reset(a); }
}

int kvm_auto_on(const kvm_auto *a) { return a->on ? 1 : 0; }

void kvm_auto_resume(kvm_auto *a, uint32_t now)
{
	uint32_t gap = 0;
	if (!a->on) { return; }
	if (a->haveEval) { gap = since(now, a->lastEvalAt); }
	else if (a->haveChange) { gap = since(now, a->lastChangeAt); }
	// what was seen before the pause: the window that is open, the series, the capture that waited, the freeze of a key frame
	a->haveWin = 0; a->motionAcc = 0; a->haveMotion = 0; a->keyInWin = 0; a->haveKey = 0;
	a->made = a->heldWin = a->heldRate = a->pendingIters = a->qpN = a->qpSum = a->qpEnd = a->bytes = 0;
	a->dwmImages = a->dwmFrames = 0;
	a->haveImage = 0; a->haveCap = 0;
	memset(a->capHist, 0, sizeof(a->capHist)); memset(a->gapHist, 0, sizeof(a->gapHist)); a->capN = a->gapN = 0;
	a->cpuStreak = a->qpStreak = a->goodStreak = 0;
	if (gap > (uint32_t)KVM_AUTO_STALE_MS)
	{
		a->started = 0; a->step = 1; a->beginAt = now; a->haveChange = 0; a->haveUp = 0; a->haveEval = 0;
	}
}
// 0 before the start: the first op 1 has no step yet and the period of command 5 that the viewer sent with the bit (its last step of its own, or Medium) holds. After the start the step is
// the interval, also across a restart of the generation (the hardware encoder, another QP stage): the step is known then
uint32_t kvm_auto_interval_ms(const kvm_auto *a) { return (a->on && a->started) ? (uint32_t)kMs[clamp_step(a->step)] : 0u; }
int kvm_auto_fps(const kvm_auto *a) { return a->on ? (int)kFps[clamp_step(a->step)] : 0; }

// ---------------------------------------------------------------------------------------------------------------------------------------
// the notes
// ---------------------------------------------------------------------------------------------------------------------------------------

static void open_window(kvm_auto *a, uint32_t now)
{
	if (!a->haveWin) { a->haveWin = 1; a->winStart = now; }
}

static void mark_motion(kvm_auto *a, uint32_t now)
{
	open_window(a, now);
	if (a->haveMotion)
	{
		const uint32_t gap = since(now, a->lastMotionAt);
		if (gap == 0) { return; }		// the image and the frame of one moment: one step of motion
		a->motionAcc += gap <= (uint32_t)KVM_AUTO_MOTION_GAP_MS ? gap : (uint32_t)kMs[clamp_step(a->step)];
	}
	else { a->motionAcc += (uint32_t)kMs[clamp_step(a->step)]; }
	a->lastMotionAt = now; a->haveMotion = 1;
}

static void hist_add(uint16_t *h, uint32_t *n, uint32_t v)
{
	if (v > 255u) { v = 255u; }
	if (h[v] < 65535u) { h[v]++; (*n)++; }
}

// the value below which `num` / `den` of the samples are (rounded up: the P75 of 10 samples is the 8th)
static uint32_t hist_pct(const uint16_t *h, uint32_t n, uint32_t num, uint32_t den)
{
	uint32_t need, cum = 0, i;
	if (n == 0) { return 0; }
	need = (n * num + den - 1u) / den;
	for (i = 0; i < 256u; i++) { cum += h[i]; if (cum >= need) { return i; } }
	return 255u;
}
static uint32_t hist_p75(const uint16_t *h, uint32_t n) { return hist_pct(h, n, 3u, 4u); }
static uint32_t hist_med(const uint16_t *h, uint32_t n) { return hist_pct(h, n, 1u, 2u); }

void kvm_auto_note_capture(kvm_auto *a, uint32_t ms, uint32_t dwmFrames)
{
	if (!a->on) { return; }
	a->capMs = ms; a->capDwm = dwmFrames; a->haveCap = 1;
}

void kvm_auto_note_image(kvm_auto *a, uint32_t now, uint32_t pending)
{
	if (!a->on) { return; }
	open_window(a, now);
	if (pending == 0) { a->haveCap = 0; a->haveImage = 0; return; }	// no motion for the video: a break of the series
	mark_motion(a, now);
	a->pendingIters++;
	if (a->haveCap)
	{
		hist_add(a->capHist, &a->capN, a->capMs);
		if (a->capDwm > 0) { a->dwmImages++; a->dwmFrames += a->capDwm; }
		a->haveCap = 0;
	}
	if (a->haveImage)
	{
		const int32_t gap = dms(now, a->lastImageAt);
		if (gap > 0 && gap < (int32_t)KVM_AUTO_GAP_MAX_MS) { hist_add(a->gapHist, &a->gapN, (uint32_t)gap); }
	}
	a->lastImageAt = now; a->haveImage = 1;
}

void kvm_auto_note_held(kvm_auto *a, int kind)
{
	if (!a->on) { return; }
	if (kind == 0) { a->heldWin++; } else { a->heldRate++; }
}

// is the freeze of the last key frame still on? (a `keyUntil` that is far away either way is old: the clock went round)
static int key_live(const kvm_auto *a, uint32_t now)
{
	const int32_t rem = dms(a->keyUntil, now);
	return (a->haveKey && rem > 0 && rem <= (int32_t)(KVM_AUTO_KEY_FREEZE_MS + 60000u + 10000u)) ? 1 : 0;
}

void kvm_auto_note_key(kvm_auto *a, uint32_t now, uint32_t needMs)
{
	uint32_t until;
	if (!a->on) { return; }
	open_window(a, now);
	if (needMs > 60000u) { needMs = 60000u; }
	until = now + (uint32_t)KVM_AUTO_KEY_FREEZE_MS + needMs;
	if (!a->haveKey || !key_live(a, now) || dms(until, a->keyUntil) > 0) { a->keyUntil = until; }
	a->haveKey = 1; a->keyInWin = 1;
}

void kvm_auto_note_frame(kvm_auto *a, uint32_t now, int qp, uint32_t bytes)
{
	if (!a->on) { return; }
	mark_motion(a, now);
	a->made++;
	a->bytes += bytes;
	if (qp > 0) { a->qpN++; a->qpSum += (uint32_t)qp; a->qpEnd = (uint32_t)qp; }
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the decision
// ---------------------------------------------------------------------------------------------------------------------------------------

// the rise of the QP, in tenths, that a share of refused frames of 0, 10, 20 ... 100 % needs (the nearest decile): ln(1 / (1 - h)) / -ln(0.89) (a P frame is 11 % smaller per QP step; measured 11..19 %)
static const uint16_t kRise10[11] = { 0, 9, 19, 31, 44, 60, 79, 103, 138, 198, 300 };

uint32_t kvm_auto_pred_qp10(uint32_t qpNow10, uint32_t heldPct)
{
	if (heldPct > 100u) { heldPct = 100u; }
	return qpNow10 + kRise10[(heldPct + 5u) / 10u];		// the decile that is nearest: 56 % held is 60 %
}

// the change of the QP, in tenths, that a stream of `ratio100` per cent of R needs: ln(ratio) / -ln(0.89), negative when the stream is under R (the regulator goes down by itself, to its floor)
static int qp_delta10(uint64_t ratio100)
{
	static const struct { uint16_t r; int16_t d; } t[] = { { 25, -119 }, { 35, -90 }, { 50, -59 }, { 70, -31 }, { 80, -19 }, { 90, -9 }, { 100, 0 }, { 110, 8 }, { 125, 19 }, { 150, 35 }, { 175, 48 }, { 200, 60 },
		{ 250, 79 }, { 300, 94 }, { 400, 119 }, { 600, 154 }, { 1000, 198 } };
	size_t i;
	for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) { if (ratio100 <= t[i].r) { return t[i].d; } }
	return 300;
}

uint32_t kvm_auto_up_qp10(int step, uint32_t qpNow10, uint32_t bytes, uint32_t motionMs, uint32_t rateBps, uint32_t qpFloor)
{
	const int n = clamp_step(step) > 0 ? clamp_step(step) - 1 : 0;
	const uint64_t scale100 = (uint64_t)kFps[n] * 100u / (uint64_t)kFps[clamp_step(step)];
	uint64_t ratio100 = scale100;
	int p;
	const int floor10 = (int)(qpFloor > 0 ? qpFloor : (uint32_t)KVM_AUTO_QP_FLOOR) * 10;
	if (rateBps > 0 && bytes > 0 && motionMs > 0)
	{
		const uint64_t bps = (uint64_t)bytes * 1000u / motionMs;
		ratio100 = bps * scale100 / rateBps;		// (the faster rate x the bytes a second now) / R x 100
	}
	p = (int)qpNow10 + qp_delta10(ratio100);
	return (uint32_t)(p < floor10 ? floor10 : p);
}


// a step is made: the bookkeeping of the patience, the streaks and the time
static void set_step(kvm_auto *a, int to, int reason, uint32_t now, kvm_auto_dec *dec)
{
	const int from = a->step;
	if (to == from) { return; }
	if (to > from)
	{
		// a step down soon after a step up: the attempt failed, the patience grows
		if (a->haveUp && since(now, a->lastUpAt) <= (uint32_t)KVM_AUTO_FAIL_MS)
		{
			uint32_t n = a->upNeed * 2u;
			a->upNeed = n > (uint32_t)KVM_AUTO_UP_MAX ? (uint32_t)KVM_AUTO_UP_MAX : n;
		}
		// the link queued at this step and queues again at once: the next attempt waits (the CPU and the capture keep the patience they had)
		if (reason == KVM_AUTO_R_LINK && a->upNeed < (uint32_t)KVM_AUTO_LINK_WAIT) { a->upNeed = (uint32_t)KVM_AUTO_LINK_WAIT; }
		a->haveUp = 0;
		a->downs++;
	}
	else
	{
		a->lastUpAt = now; a->haveUp = 1; a->upStep = (uint8_t)to;
		a->ups++;
	}
	a->step = (uint8_t)to;
	a->lastChangeAt = now; a->haveChange = 1;
	a->cpuStreak = a->qpStreak = a->goodStreak = 0;
	a->changes++;
	a->lastReason = (uint8_t)reason;
	dec->changed = 1; dec->from = (uint8_t)from; dec->to = (uint8_t)to; dec->reason = (uint8_t)reason;
	dec->intervalMs = kMs[to];
}

static uint32_t sat8(uint32_t v) { return v > 255u ? 255u : v; }

int kvm_auto_decide(kvm_auto *a, const kvm_auto_win *w, const kvm_auto_env *env, kvm_auto_dec *dec)
{
	const uint32_t now = env->now;
	uint64_t ach10;
	uint32_t held, due, heldPct = 0, need = 0, qpNow10;
	int cls, step, limited, qpBad = 0, fA, fB, fC, fD, srcFaster, ceilQp;
	memset(dec, 0, sizeof(*dec));
	dec->from = dec->to = a->step;
	if (!a->on) { return 0; }
	dec->intervalMs = kMs[clamp_step(a->step)];

	// the start: the first minimum round trip decides (a low one is a LAN: 60); without one, after a while, 30. It is no step up: it does not count as an attempt that can fail
	if (!a->started)
	{
		int to = -1;
		if (env->minRttMs > 0) { to = env->minRttMs < (uint32_t)KVM_AUTO_START_RTT_MS ? 0 : 1; }
		else if (since(now, a->beginAt) >= (uint32_t)KVM_AUTO_START_WAIT_MS) { to = 1; }
		if (to < 0) { return 0; }
		a->started = 1;
		a->lastChangeAt = now; a->haveChange = 1;
		a->lastReason = KVM_AUTO_R_START;
		dec->reason = KVM_AUTO_R_START;
		if (to != (int)a->step)
		{
			dec->changed = 1; dec->from = a->step; dec->to = (uint8_t)to;
			a->step = (uint8_t)to; a->changes++;
			dec->intervalMs = kMs[to];
		}
		return dec->changed;
	}

	// a ceiling from outside (the screen of the viewer, the battery saver, the budget of the viewer: a later stage): at once, with or without motion
	if (env->maxFps > 0 && (uint32_t)kFps[clamp_step(a->step)] > env->maxFps)
	{
		int to = a->step;
		while (to < KVM_AUTO_STEPS - 1 && (uint32_t)kFps[to] > env->maxFps) { to++; }
		set_step(a, to, env->maxReason != 0 ? env->maxReason : KVM_AUTO_R_SCREEN, now, dec);
		return dec->changed;
	}
	if (w == NULL) { return 0; }

	// the window: only with enough motion, and only while the video runs; otherwise it counts neither way (a silent screen is not a slow link)
	if (!env->valid || w->key || w->motionMs < (uint32_t)KVM_AUTO_MIN_MOTION_MS) { return 0; }
	if (a->haveEval && since(now, a->lastEvalAt) > (uint32_t)KVM_AUTO_STALE_MS) { a->cpuStreak = a->qpStreak = a->goodStreak = 0; }
	a->lastEvalAt = now; a->haveEval = 1;
	dec->evaluated = 1;
	// a step up that has held for 60 s on the faster step was a success: the patience is the first one again (60 s of nothing happening is not: that is the wait for the next attempt)
	if (a->haveUp && a->step == a->upStep && since(now, a->lastUpAt) >= (uint32_t)KVM_AUTO_STABLE_MS) { a->upNeed = (uint32_t)KVM_AUTO_UP_WINDOWS; a->haveUp = 0; }

	step = clamp_step(a->step);
	cls = env->cls == KVM_AUTO_CLASS_VIDEO ? KVM_AUTO_CLASS_VIDEO : KVM_AUTO_CLASS_UI;
	ceilQp = kCeil[cls][step];
	ach10 = ((uint64_t)w->made * 10000u) / (uint64_t)w->motionMs;
	held = w->heldWin + w->heldRate;
	due = w->made + held;
	if (due > 0) { heldPct = (uint32_t)(((uint64_t)held * 100u) / due); }
	srcFaster = (w->dwmX100 == 0 || w->dwmX100 >= (uint32_t)KVM_AUTO_DWM_SLOW_X100) ? 1 : 0;

	// does the machine keep up? (the criteria that follow; every one of them has to hold at 75 % of the interval: a rate that is 75 % of the target is the uneven 45 of 60 that is accepted)
	fA = (ach10 * 100u < (uint64_t)kFps[step] * 10u * (uint64_t)KVM_AUTO_KEEP_PCT && held == 0 && w->pendingIters > 0 && srcFaster) ? 1 : 0;
	fB = (env->floorMs > 0 && (uint64_t)env->floorMs * 1000u > (uint64_t)kUs[step]) ? 1 : 0;
	fC = (w->capP75Ms > 0 && over(w->capP75Ms, kUs[step]) && srcFaster) ? 1 : 0;
	fD = (w->gapMedMs > 0 && over(w->gapMedMs, kUs[step]) && srcFaster) ? 1 : 0;
	limited = (fA || fB || fC || fD) ? 1 : 0;
	if (fB && env->floorMs > need) { need = env->floorMs; }
	if (fC && w->capP75Ms > need) { need = w->capP75Ms; }
	if (fD && w->gapMedMs > need) { need = w->gapMedMs; }
	if (fA) { const uint32_t n = ach10 > 0 ? (uint32_t)(10000u / ach10) : 1000u; if (n > need) { need = n; } }

	qpNow10 = env->qpNow > 0 ? env->qpNow * 10u : (w->qpN > 0 ? w->qpEnd * 10u : 0u);
	qpBad = (qpNow10 > 0 && kvm_auto_pred_qp10(qpNow10, heldPct) > (uint32_t)ceilQp * 10u) ? 1 : 0;

	dec->achieved10 = (uint32_t)ach10; dec->heldPct = heldPct; dec->needMs = need;
	if (limited) { dec->flags |= KVM_AUTO_F_LIMITED; }
	if (qpBad) { dec->flags |= KVM_AUTO_F_QPBAD; }
	if (fB) { dec->flags |= KVM_AUTO_F_FLOOR; }
	if (fC || fD) { dec->flags |= KVM_AUTO_F_CAPTURE; }
	if (fA) { dec->flags |= KVM_AUTO_F_ACHIEVED; }

	if (limited) { a->cpuStreak = (uint8_t)sat8(a->cpuStreak + 1u); a->qpStreak = 0; a->goodStreak = 0; }
	else if (qpBad) { a->qpStreak = (uint8_t)sat8(a->qpStreak + 1u); a->cpuStreak = 0; a->goodStreak = 0; }
	else { a->cpuStreak = 0; a->qpStreak = 0; }

	{
		const int canMove = (!a->haveChange || since(now, a->lastChangeAt) >= (uint32_t)KVM_AUTO_MIN_CHANGE_MS) ? 1 : 0;
		if (a->cpuStreak >= KVM_AUTO_DOWN_WINDOWS && step < KVM_AUTO_STEPS - 1 && canMove)
		{
			int to = step + 1;
			while (to < KVM_AUTO_STEPS - 1 && over(need, kUs[to])) { to++; }
			// the capture, the loop, or a rate that is low with nothing to say why where the desktop manager says nothing (GDI): the capture; the encoder's own budget: the CPU
			set_step(a, to, (fC || fD || (fA && !fB && w->dwmX100 == 0)) ? KVM_AUTO_R_CAPTURE : KVM_AUTO_R_CPU, now, dec);
			return dec->changed;
		}
		if (a->qpStreak >= KVM_AUTO_DOWN_WINDOWS && step < KVM_AUTO_STEPS - 1 && canMove)
		{
			set_step(a, step + 1, KVM_AUTO_R_LINK, now, dec);
			return dec->changed;
		}
	}

	// up: the conditions for the faster step, for as many windows in a row as the patience asks
	if (!limited && !qpBad && step > 0)
	{
		const int n = step - 1;
		const uint32_t nus = kUs[n], nfps = kFps[n];
		const int ceilN = kCeil[cls][n];
		int ok = 1;
		if (env->maxFps > 0 && nfps > env->maxFps) { ok = 0; }
		if (w->qpN == 0 || qpNow10 == 0 || kvm_auto_up_qp10(step, qpNow10, w->bytes, w->motionMs, env->rateBps, env->qpFloor) > (uint32_t)(ceilN - KVM_AUTO_UP_QP_HEADROOM) * 10u) { ok = 0; }
		if (heldPct >= (uint32_t)KVM_AUTO_HELD_UP_PCT) { ok = 0; }
		if (ach10 * 100u < (uint64_t)kFps[step] * 10u * (uint64_t)KVM_AUTO_SAT_PCT) { ok = 0; }
		if (w->capP75Ms > 0 && over(w->capP75Ms, nus)) { ok = 0; }
		if (env->encAvgUs > 0 && (uint64_t)env->encAvgUs * (uint64_t)KVM_AUTO_KEEP_PCT > (uint64_t)nus * 100u) { ok = 0; }
		if (env->encAvgUs > 0 && env->budgetMilli > 0 && ((uint64_t)env->encAvgUs * (uint64_t)env->parX10 * (uint64_t)nfps) / 10000u > (uint64_t)env->budgetMilli) { ok = 0; }
		if ((uint64_t)env->floorMs * 1000u > (uint64_t)nus) { ok = 0; }
		if (env->queueMs >= (uint32_t)KVM_AUTO_UP_QUEUE_MS) { ok = 0; }
		if (ok) { dec->flags |= KVM_AUTO_F_GOOD; a->goodStreak = (uint8_t)sat8(a->goodStreak + 1u); } else { a->goodStreak = 0; }
		if (a->goodStreak >= a->upNeed && (!a->haveChange || since(now, a->lastChangeAt) >= (uint32_t)KVM_AUTO_UP_AFTER_MS))
		{
			set_step(a, n, KVM_AUTO_R_BACK, now, dec);
			return dec->changed;
		}
	}
	else { a->goodStreak = 0; }
	return dec->changed;
}

int kvm_auto_tick(kvm_auto *a, const kvm_auto_env *env, kvm_auto_dec *dec, kvm_auto_win *win)
{
	kvm_auto_win w;
	int r;
	if (!a->on) { memset(dec, 0, sizeof(*dec)); dec->from = dec->to = a->step; return 0; }
	open_window(a, env->now);
	if (since(env->now, a->winStart) < (uint32_t)KVM_AUTO_WINDOW_MS) { return kvm_auto_decide(a, NULL, env, dec); }
	memset(&w, 0, sizeof(w));
	w.windowMs = since(env->now, a->winStart);
	if (a->haveKey && !key_live(a, env->now)) { a->haveKey = 0; }	// the freeze is over (and a stamp that went round the clock is not a freeze for another 24.9 days)
	w.motionMs = a->motionAcc;
	if (w.motionMs > w.windowMs) { w.motionMs = w.windowMs; }
	w.made = a->made; w.heldWin = a->heldWin; w.heldRate = a->heldRate; w.pendingIters = a->pendingIters;
	w.qpN = a->qpN; w.qpSum = a->qpSum; w.qpEnd = a->qpEnd; w.bytes = a->bytes;
	w.capP75Ms = hist_p75(a->capHist, a->capN);
	w.gapMedMs = hist_med(a->gapHist, a->gapN);
	w.dwmX100 = a->dwmImages > 0 ? (uint32_t)(((uint64_t)a->dwmFrames * 100u) / a->dwmImages) : 0u;
	w.key = (uint8_t)((a->keyInWin || key_live(a, env->now)) ? 1 : 0);
	// the next window
	a->winStart = env->now; a->motionAcc = 0; a->keyInWin = 0;
	a->made = a->heldWin = a->heldRate = a->pendingIters = a->qpN = a->qpSum = a->qpEnd = a->bytes = 0;
	a->dwmImages = a->dwmFrames = 0;
	memset(a->capHist, 0, sizeof(a->capHist)); memset(a->gapHist, 0, sizeof(a->gapHist)); a->capN = a->gapN = 0;
	if (win != NULL) { *win = w; }
	r = kvm_auto_decide(a, &w, env, dec);
	return r;
}

int kvm_auto_format(const kvm_auto *a, const kvm_auto_win *w, const kvm_auto_dec *d, char *out, int cap)
{
	kvm_auto_win z;
	if (w == NULL) { memset(&z, 0, sizeof(z)); w = &z; }
	return snprintf(out, (size_t)cap, "auto on=%d step=%d fps=%d interval_ms=%u changed=%d reason=%s eval=%d key=%d flags=%u motion_ms=%u made=%u achieved_fps=%u.%u held_win=%u held_rate=%u held_pct=%u qp_mean=%u.%u qp_end=%u cap_p75_ms=%u gap_med_ms=%u dwm_x100=%u need_ms=%u cpu_streak=%u qp_streak=%u good_streak=%u up_need=%u changes=%u downs=%u ups=%u",
					a->on ? 1 : 0, (int)a->step, kvm_auto_fps(a), (unsigned)kvm_auto_interval_ms(a), d->changed ? 1 : 0, kvm_auto_reason_name(d->changed ? d->reason : a->lastReason), d->evaluated ? 1 : 0, w->key ? 1 : 0, (unsigned)d->flags,
					(unsigned)w->motionMs, (unsigned)w->made, (unsigned)(d->achieved10 / 10u), (unsigned)(d->achieved10 % 10u), (unsigned)w->heldWin, (unsigned)w->heldRate, (unsigned)d->heldPct,
					w->qpN > 0 ? (unsigned)((w->qpSum * 10u / w->qpN) / 10u) : 0u, w->qpN > 0 ? (unsigned)((w->qpSum * 10u / w->qpN) % 10u) : 0u, (unsigned)w->qpEnd, (unsigned)w->capP75Ms, (unsigned)w->gapMedMs, (unsigned)w->dwmX100,
					(unsigned)d->needMs, (unsigned)a->cpuStreak, (unsigned)a->qpStreak, (unsigned)a->goodStreak, (unsigned)a->upNeed, (unsigned)a->changes, (unsigned)a->downs, (unsigned)a->ups);
}
