/*
KVM: the pace of the main loop and the CPU budget of the video. See kvm_pace.h. Portable C99, no operating system API.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#include "kvm_pace.h"
#include <string.h>

static int32_t dms(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

void kvm_late_clear(kvm_late *l) { memset(l, 0, sizeof(*l)); }

static void late_note(kvm_late *l, int32_t lateMs)
{
	l->n++;
	if (lateMs >= (int32_t)KVM_PACE_LATE_MS)
	{
		l->late++;
		l->sumMs += (uint32_t)lateMs;
		if ((uint32_t)lateMs > l->maxMs) { l->maxMs = (uint32_t)lateMs; }
	}
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// tick
// ---------------------------------------------------------------------------------------------------------------------------------------

void kvm_tick_init(kvm_tick *t) { memset(t, 0, sizeof(*t)); }

void kvm_tick_idle(kvm_tick *t) { t->have = 0; }

static uint32_t sleep_until(uint32_t slot, uint32_t now)
{
	int32_t d = dms(slot, now);
	return d > 0 ? (uint32_t)d : 0;
}

uint32_t kvm_tick_next(kvm_tick *t, uint32_t started, uint32_t now, uint32_t periodMs)
{
	uint32_t cur, next;
	int32_t over;
	if (periodMs == 0) { t->have = 0; return 0; }
	if (t->have && dms(started, t->due) < -(int32_t)KVM_PACE_EARLY_MS)
	{
		// woken before the slot for another reason (a video frame was due): this iteration is not the one of the schedule, the slot stays
		return sleep_until(t->due, now);
	}
	if (!t->have) { cur = started; }	// the first iteration after an idle loop is its own slot
	else
	{
		int32_t late = dms(started, t->due);
		cur = t->due;
		late_note(&t->st, late < 0 ? 0 : late);
	}
	next = cur + periodMs;
	over = dms(now, next);
	if (over > 0) { t->st.over++; }
	if (over >= (int32_t)periodMs) { t->st.resync++; next = now; }	// a whole period behind: no catch-up, the schedule begins again here
	t->due = next;
	t->have = 1;
	return sleep_until(next, now);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the slots of the video frames
// ---------------------------------------------------------------------------------------------------------------------------------------

uint32_t kvm_pace_slot(int haveLast, uint32_t lastSlot, uint32_t now, uint32_t intervalMs, kvm_late *l)
{
	uint32_t s;
	int32_t late;
	if (!haveLast || intervalMs == 0) { return now; }
	s = lastSlot + intervalMs;
	late = dms(now, s);
	if (late >= (int32_t)intervalMs)
	{
		// a whole interval behind (the screen was still, or the frame took very long): not "late", the schedule begins again; no burst of frames to catch up
		if (l != NULL) { l->n++; l->resync++; }
		return now;
	}
	if (l != NULL) { late_note(l, late < 0 ? 0 : late); }
	return s;
}

uint32_t kvm_pace_wake_ms(int haveLast, uint32_t lastSlot, uint32_t now, uint32_t intervalMs)
{
	if (!haveLast || intervalMs == 0) { return 0; }
	return sleep_until(lastSlot + intervalMs, now);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the CPU budget
// ---------------------------------------------------------------------------------------------------------------------------------------

void kvm_budget_clear_stats(kvm_budget *b)
{
	b->encMaxMilli = 0;
	b->windows = b->measured = 0;
}

void kvm_budget_init(kvm_budget *b, uint32_t cores)
{
	uint64_t m;
	memset(b, 0, sizeof(*b));
	b->cores = cores > 0 ? cores : 1;
	m = ((uint64_t)b->cores * 1000u * KVM_PACE_CPU_SHARE_PCT) / 100u;
	// from two processors up 50 % of the machine is at least one core ("at least one core" holds by itself); on one processor it is half a core: the encoder never takes the whole machine
	b->budgetMilli = (uint32_t)m;
	b->parX10 = KVM_PACE_ENC_PAR_X10;
}

void kvm_budget_note_encode(kvm_budget *b, uint32_t wallUs, uint64_t cpuUs)
{
	b->winWallUs += wallUs;
	b->winCpuUs += cpuUs;
}

void kvm_budget_note_cpu(kvm_budget *b, uint64_t cpuUs) { b->winCpuUs += cpuUs; }

int kvm_budget_window(kvm_budget *b, uint32_t now)
{
	uint32_t dt;
	uint64_t wall, cpu, u;
	if (!b->haveWin)
	{
		b->haveWin = 1; b->winStart = now; b->winWallUs = b->winCpuUs = 0;
		return 0;
	}
	if (dms(now, b->winStart) < (int32_t)KVM_PACE_WINDOW_MS) { return 0; }
	dt = (uint32_t)dms(now, b->winStart);
	wall = b->winWallUs; cpu = b->winCpuUs;
	b->winStart = now; b->winWallUs = b->winCpuUs = 0;
	b->windows++;
	u = cpu / dt;											// microseconds per millisecond = 1/1000 of a core, over the whole window
	b->encMilli = u > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)u;
	if (b->encMilli > b->encMaxMilli) { b->encMaxMilli = b->encMilli; }
	if (wall >= (uint64_t)KVM_PACE_MIN_ENC_MS * 1000u && cpu > 0)
	{
		// cores busy while the encoder ran; a clock that cannot be read gives 0 CPU and leaves the estimate where it was
		uint64_t p = (cpu / wall) * 10u + (((cpu % wall) * 10u + wall / 2u) / wall);	// cores x 10, without overflow
		uint64_t lim = (uint64_t)b->cores * 10u;
		if (lim > KVM_PACE_PAR_MAX_X10) { lim = KVM_PACE_PAR_MAX_X10; }
		if (p > lim) { p = lim; }
		if (p < 10u) { p = 10u; }							// the thread that calls the encoder is busy while it runs
		{
			uint32_t avg = (uint32_t)((3u * (uint64_t)b->parX10 + p + 2u) / 4u);	// moving average, a quarter per window ...
			if (avg == b->parX10 && p != b->parX10) { avg = p > b->parX10 ? avg + 1u : avg - 1u; }	// ... that does not stop short of the value by the rounding
			b->parX10 = avg;
		}
		b->measured++;
	}
	return 1;
}

uint32_t kvm_budget_enc_floor_ms(const kvm_budget *b, uint32_t encAvgUs)
{
	uint64_t par10 = b->parX10, need, den;
	if (par10 > (uint64_t)b->cores * 10u) { par10 = (uint64_t)b->cores * 10u; }
	if (par10 < 10u) { par10 = 10u; }
	need = (uint64_t)encAvgUs * par10;				// CPU microseconds of one frame, x 10
	den = (uint64_t)10u * b->budgetMilli;			// microseconds of CPU per millisecond of wall time that the budget allows, x 10
	return (uint32_t)((need + den - 1u) / den);
}

uint32_t kvm_pace_interval(const kvm_budget *b, uint32_t periodMs, uint32_t encAvgUs)
{
	uint32_t v = periodMs;
	if (b != NULL)
	{
		uint32_t f = kvm_budget_enc_floor_ms(b, encAvgUs);
		if (f > v) { v = f; }
	}
	if (v > KVM_PACE_INTERVAL_MAX_MS && v > periodMs) { v = periodMs > KVM_PACE_INTERVAL_MAX_MS ? periodMs : KVM_PACE_INTERVAL_MAX_MS; }
	return v;
}
