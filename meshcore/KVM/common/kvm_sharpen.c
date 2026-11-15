/*
KVM: the governor of the sharpening while the H.264 video runs. See kvm_sharpen.h. Portable C99, no operating system API.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#include "kvm_sharpen.h"
#include <string.h>

static int32_t dms(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

static int64_t share_of(uint64_t rate)
{
	if (rate == 0) { rate = KVM_SHARP_DEFAULT_RATE; }
	if (rate > (1ull << 40)) { rate = 1ull << 40; }
	return (int64_t)((rate * KVM_SHARP_SHARE_PCT) / 100u);
}

static int64_t burst_of(uint64_t rate)
{
	const int64_t b = (share_of(rate) * KVM_SHARP_BURST_MS) / 1000;
	return b < KVM_SHARP_BURST_MIN ? KVM_SHARP_BURST_MIN : b;
}

static void refill(kvm_sharp *g, uint32_t now, uint64_t rate)
{
	const int64_t cap = burst_of(rate);
	int32_t dt;
	g->cap = cap;
	if (!g->haveTok) { g->tokens = cap; g->tokTime = now; g->haveTok = 1; return; }
	dt = dms(now, g->tokTime);
	if (dt <= 0) { if (g->tokens > cap) { g->tokens = cap; } return; }
	if (dt > 60000) { dt = 60000; }
	g->tokens += (share_of(rate) * dt) / 1000;
	if (g->tokens > cap) { g->tokens = cap; }
	g->tokTime = now;
}

void kvm_sharp_init(kvm_sharp *g, uint32_t now)
{
	memset(g, 0, sizeof(*g));
	g->tokTime = now;
}

void kvm_sharp_clear_stats(kvm_sharp *g)
{
	g->sBucketRefused = g->sIterCapped = g->sWindowRefused = g->sActiveIters = g->sIdleIters = 0;
	g->sBytesActive = g->sBytesIdle = g->sBytesForced = 0;
}

void kvm_sharp_motion(kvm_sharp *g, uint32_t now) { g->lastMotion = now ? now : 1u; g->haveMotion = 1; }

int kvm_sharp_active(const kvm_sharp *g, uint32_t now)
{
	return g->haveMotion && dms(now, g->lastMotion) < (int32_t)(g->activeMs != 0 ? g->activeMs : (uint32_t)KVM_SHARP_ACTIVE_MS);
}

void kvm_sharp_set_interval(kvm_sharp *g, uint32_t intervalMs)
{
	uint32_t ms = intervalMs * (uint32_t)KVM_SHARP_ACTIVE_FRAMES;
	if (intervalMs > 1000u) { ms = 1000u * (uint32_t)KVM_SHARP_ACTIVE_FRAMES; }
	g->activeMs = ms < (uint32_t)KVM_SHARP_ACTIVE_MIN_MS ? (uint32_t)KVM_SHARP_ACTIVE_MIN_MS : ms;
}

int kvm_sharp_post_motion(const kvm_sharp *g, uint32_t now)
{
	return g->haveMotion && dms(now, g->lastMotion) < (int32_t)KVM_SHARP_POST_MS;
}

int64_t kvm_sharp_begin(kvm_sharp *g, uint32_t now, uint64_t rate, uint64_t window, int64_t want)
{
	int64_t allowed = want;
	refill(g, now, rate);
	g->iterSent = 0;
	g->iterActive = (uint8_t)(kvm_sharp_active(g, now) ? 1 : 0);
	if (g->iterActive)
	{
		g->sActiveIters++;
		if (g->tokens <= 0) { g->sBucketRefused++; allowed = 0; }
		else
		{
			if (allowed > g->tokens) { allowed = g->tokens; }
			if (window > 0)
			{
				const int64_t cap = (int64_t)((window * KVM_SHARP_ITER_WINDOW_PCT) / 100u);
				if (allowed > cap) { allowed = cap; g->sIterCapped++; }
			}
		}
	}
	else { g->sIdleIters++; }
	if (allowed < 0) { allowed = 0; }
	g->iterAllowed = allowed;
	return allowed;
}

int kvm_sharp_window_ok(kvm_sharp *g, uint64_t inflight, uint64_t window)
{
	if (window == 0) { return 1; }
	if (inflight * 100u < window * KVM_SHARP_PACKET_WINDOW_PCT) { return 1; }
	g->sWindowRefused++;
	return 0;
}

int kvm_sharp_window_ok_at(kvm_sharp *g, uint32_t now, uint64_t inflight, uint64_t window)
{
	const unsigned pct = kvm_sharp_post_motion(g, now) ? (unsigned)KVM_SHARP_POST_WINDOW_PCT : (unsigned)KVM_SHARP_PACKET_WINDOW_PCT;
	if (window == 0) { return 1; }
	if (inflight * 100u < window * pct) { return 1; }
	g->sWindowRefused++;
	return 0;
}

void kvm_sharp_sent(kvm_sharp *g, uint32_t now, int64_t bytes)
{
	(void)now;
	if (bytes <= 0) { return; }
	g->iterSent += bytes;
	if (g->iterActive)
	{
		g->tokens -= bytes;
		if (g->tokens < -2 * g->cap) { g->tokens = -2 * g->cap; }	// (the debt is limited: the bucket refills at the share, a byte is never owed for ever)
		g->sBytesActive += (uint64_t)bytes;
	}
	else { g->sBytesIdle += (uint64_t)bytes; }
}

void kvm_sharp_forced(kvm_sharp *g, uint32_t now, int64_t bytes)
{
	(void)now;
	if (bytes <= 0) { return; }
	g->tokens -= bytes;
	if (g->tokens < -2 * g->cap) { g->tokens = -2 * g->cap; }
	g->sBytesForced += (uint64_t)bytes;
}
