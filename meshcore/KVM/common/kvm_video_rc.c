/*
KVM: rate control of the H.264 video. See kvm_video_rc.h. Portable C99, no operating system API.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#include "kvm_video_rc.h"
#include "kvm_video_core.h"
#include <string.h>

static int32_t dms(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

uint64_t kvm_vrc_target(unsigned budgetKbps, uint64_t capacity)
{
	uint64_t r = 0;
	int have = 0;
	if (budgetKbps > 0) { r = ((uint64_t)budgetKbps * 125u * KVM_VRC_BUDGET_PCT) / 100u; have = 1; }	// kbit/s -> bytes/s: x 125
	if (capacity > 0)
	{
		if (!have || capacity < r) { r = capacity; }
		have = 1;
	}
	if (!have) { r = KVM_VRC_DEFAULT_RATE; }
	return r < KVM_VRC_MIN_RATE ? KVM_VRC_MIN_RATE : r;
}

void kvm_vrc_init(kvm_vrc *r, int startQp, uint32_t now) { kvm_vrc_init_backend(r, startQp, now, 0); }

void kvm_vrc_init_backend(kvm_vrc *r, int startQp, uint32_t now, int hw)
{
	memset(r, 0, sizeof(*r));
	r->qp = kvm_vcore_clamp_qp(startQp);
	r->rate = KVM_VRC_DEFAULT_RATE;
	r->tokens = (int64_t)(r->rate * KVM_VRC_BURST_MS / 1000u);
	r->tokTime = now; r->winStart = now; r->started = 1;
	r->idrPriorBytes = KVM_VRC_IDR_PRIOR_BYTES; r->idrSlopePct = KVM_VRC_IDR_SLOPE_PCT; r->qpMin = KVM_VIDEO_QP_MIN;
	r->lastIdrBytes = KVM_VRC_IDR_PRIOR_BYTES; r->lastIdrQp = KVM_VRC_IDR_PRIOR_QP; r->flatMilli = KVM_VRC_IDR_FLAT_MILLI;
	r->idrLimited = 1;	// nothing is known about the link yet: the cautious rule (0.3 s of R)
	if (hw) { kvm_vrc_set_backend(r, 1); }	// (the QP floor, the prior and the slope of the hardware encoder)
}

void kvm_vrc_set_backend(kvm_vrc *r, int hw)
{
	r->hw = hw ? 1 : 0;
	r->idrPriorBytes = hw ? KVM_VRC_HW_IDR_PRIOR_BYTES : KVM_VRC_IDR_PRIOR_BYTES;
	r->idrSlopePct = hw ? KVM_VRC_HW_IDR_SLOPE_PCT : KVM_VRC_IDR_SLOPE_PCT;
	if (!r->idrMeasured) { r->lastIdrBytes = r->idrPriorBytes; r->lastIdrQp = KVM_VRC_IDR_PRIOR_QP; }
	r->qpMin = hw ? KVM_VIDEO_QP_MIN - KVM_HW_QP_OFFSET : KVM_VIDEO_QP_MIN;
	if (r->qp < r->qpMin) { r->qp = r->qpMin; }
}

void kvm_vrc_set_picture(kvm_vrc *r, uint32_t pixels)
{
	r->totalMbs = pixels / 256u;
	if (r->sIdr == 0 && !r->idrMeasured && pixels > 0) { r->lastIdrBytes = (uint32_t)(((uint64_t)r->idrPriorBytes * pixels) / (1920u * 1088u)); r->lastIdrQp = KVM_VRC_IDR_PRIOR_QP; }	// only while no key frame has been measured
	if (r->lastIdrBytes < 1000u) { r->lastIdrBytes = 1000u; }
}

static void refill(kvm_vrc *r, uint32_t now)
{
	int32_t dt = dms(now, r->tokTime);
	int64_t cap = (int64_t)(r->rate * KVM_VRC_BURST_MS / 1000u);
	if (dt <= 0) { return; }
	if (dt > 60000) { dt = 60000; }
	r->tokens += (int64_t)((r->rate * (uint64_t)dt) / 1000u);
	if (r->tokens > cap) { r->tokens = cap; }
	r->tokTime = now;
}

void kvm_vrc_set_target(kvm_vrc *r, uint64_t bytesPerSec, uint32_t now)
{
	refill(r, now);	// what was earned at the old rate stays
	r->rate = bytesPerSec < KVM_VRC_MIN_RATE ? KVM_VRC_MIN_RATE : bytesPerSec;
	{ int64_t cap = (int64_t)(r->rate * KVM_VRC_BURST_MS / 1000u); if (r->tokens > cap) { r->tokens = cap; } }
}

// The size a key frame of lastIdrBytes (encoded at lastIdrQp) would have at `qp`: KVM_VRC_IDR_SLOPE_PCT per QP
static uint64_t idr_predict_last(const kvm_vrc *r, int qp)
{
	uint64_t b = r->lastIdrBytes;
	int q = r->lastIdrQp;
	while (q < qp) { b = (b * r->idrSlopePct) / 100u; q++; }
	while (q > qp && b < 0x40000000ull) { b = (b * 100u) / r->idrSlopePct; q--; }
	return b;
}

// the macroblocks that the picture has, and the cost per macroblock that the last key frame had above the floor (milli bytes); the last key frame converted lastIdrReal of them (0 = all)
static uint32_t total_mbs(const kvm_vrc *r) { return r->idrTotal != 0 ? r->idrTotal : (r->totalMbs != 0 ? r->totalMbs : 8160u); }
static uint64_t c_milli(const kvm_vrc *r, int qp)
{
	const uint64_t total = total_mbs(r), lastReal = r->lastIdrReal != 0 ? r->lastIdrReal : total;
	const uint64_t last = idr_predict_last(r, qp) * 1000u, floorB = (uint64_t)r->flatMilli * total;
	const uint64_t above = last > floorB ? last - floorB : last / 8u;	// (a key frame that was no dearer than the floor: an eighth, never zero)
	return above / (lastReal != 0 ? lastReal : 1u) + 1u;
}
static uint64_t idr_predict(const kvm_vrc *r, int qp)
{
	const uint64_t total = total_mbs(r);
	uint64_t real = r->idrReal != 0 ? r->idrReal : total;
	if (real > total) { real = total; }
	if (r->idrReal == 0 && r->lastIdrReal == 0) { return idr_predict_last(r, qp); }	// the whole picture after the whole picture: as before
	return ((uint64_t)r->flatMilli * total + c_milli(r, qp) * real + 500u) / 1000u;
}

void kvm_vrc_set_idr_cover(kvm_vrc *r, uint32_t realMbs, uint32_t totalMbs) { r->idrReal = realMbs; r->idrTotal = realMbs != 0 ? totalMbs : 0; }
uint32_t kvm_vrc_idr_bytes(const kvm_vrc *r, int qp) { const uint64_t b = idr_predict(r, qp); return b > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)b; }
uint32_t kvm_vrc_virgin_bytes(const kvm_vrc *r, uint32_t virginMbs, int qp) { const uint64_t b = (c_milli(r, qp) * (uint64_t)virginMbs) / 1000u; return b > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)b; }
void kvm_vrc_set_flat_floor(kvm_vrc *r, uint32_t milliBytesPerMb) { r->flatMilli = milliBytesPerMb > 4000u ? 4000u : milliBytesPerMb; }

static int idr_qp_floor(const kvm_vrc *r) { return r->qpMin > KVM_VRC_IDR_QP_MIN ? r->qpMin : KVM_VRC_IDR_QP_MIN; }

// QP_link: the smallest QP (from the floor of the IDR range) at which the predicted key frame fits in KVM_VRC_IDR_MS of R; the top of the range when none does
static int idr_qp_link(const kvm_vrc *r)
{
	uint64_t basis = r->rate;
	if (!r->idrLimited && r->cIdr != 0)
	{
		uint64_t c = (r->cIdr * KVM_VRC_CIDR_PCT) / 100u;
		if (c > r->idrCap) { c = r->idrCap; }
		if (c > basis) { basis = c; }
	}
	const uint64_t fit = (basis * KVM_VRC_IDR_MS) / 1000u;
	int q;
	for (q = idr_qp_floor(r); q < KVM_VRC_IDR_QP_MAX; q++) { if (idr_predict(r, q) <= fit) { return q; } }
	return KVM_VRC_IDR_QP_MAX;
}

int kvm_vrc_qp(const kvm_vrc *r, int isIdr)
{
	int q = r->qp;
	if (isIdr)
	{
		const int link = idr_qp_link(r);
		if (link > q) { q = link; }
		if (q < idr_qp_floor(r)) { q = idr_qp_floor(r); }
		if (q > KVM_VRC_IDR_QP_MAX) { q = KVM_VRC_IDR_QP_MAX; }
	}
	return q;
}

static int held_windows(const kvm_vrc *r)
{
	uint64_t h = r->heldHist & (KVM_VRC_IDR_HISTORY_WINDOWS >= 64 ? ~0ull : ((1ull << KVM_VRC_IDR_HISTORY_WINDOWS) - 1ull));
	int n = 0;
	while (h) { n += (int)(h & 1u); h >>= 1; }
	return n;
}

void kvm_vrc_update(kvm_vrc *r, uint32_t now, unsigned budgetKbps, uint64_t rate, int linkLimited)
{
	int32_t dt = dms(now, r->capTime);
	uint64_t m, ceilCap;
	if (rate > (1ull << 40)) { rate = 1ull << 40; }									// (the arithmetic below cannot overflow: cap <= 2^42, x 25 x 60 000 < 2^64)
	m = (rate * KVM_VRC_RATE_PCT) / 100u;
	ceilCap = ((m > KVM_VRC_DEFAULT_RATE ? m : KVM_VRC_DEFAULT_RATE) * KVM_VRC_CAP_MAX_PCT) / 100u;
	if (r->capTime == 0 && r->cap == 0) { r->cap = KVM_VRC_DEFAULT_RATE; }	// (a rate that is no evidence of the link, a lower bound, does not set the first estimate: it can only raise it; the evidence of the link sets it just below)
	if (dt < 0 || dt > 60000) { dt = 0; }
	if (rate > 0 && linkLimited) { r->cap = m; }									// the link is the limit: the rate is what it takes
	else
	{
		if (m > r->cap) { r->cap = m; }											// the link takes at least what was delivered
		if (r->cap == 0) { r->cap = KVM_VRC_DEFAULT_RATE; }
		// probing for more: only while the video is held by its own bucket and the estimate itself is R (with the budget below it, more capacity changes nothing)
		if (dt > 0 && r->lastDenied != 0 && dms(now, r->lastDenied) < (int32_t)(2 * KVM_VRC_WINDOW_MS) &&
			(budgetKbps == 0 || r->cap < ((uint64_t)budgetKbps * 125u * KVM_VRC_BUDGET_PCT) / 100u))
		{
			r->cap += (r->cap * (uint64_t)KVM_VRC_PROBE_PCT * (uint64_t)dt) / 100000u;
		}
	}
	// And the estimate does not fall below the smallest R: from 30 B/s the probing (a quarter a second, in integers) never began and R stayed at its floor for 12 s (the Windows 11 VM)
	if (r->cap < KVM_VRC_MIN_RATE) { r->cap = KVM_VRC_MIN_RATE; }
	if (r->cap > ceilCap) { r->cap = ceilCap; }
	(void)linkLimited;	// (the refusals of the window of the viewer are counted by windows in kvm_vrc_tick: one refusal in a second is flow control, not a full link)
	if (r->cIdr != 0 && dms(now, r->cIdrAt) > (int32_t)KVM_VRC_CIDR_MS) { r->cIdr = 0; }
	if (r->capFloor != 0 && dms(now, r->capFloorAt) > (int32_t)KVM_VRC_FLOOR_MS) { r->capFloor = 0; }
	r->idrCap = ceilCap * 2u;	// the most a key frame may be sized for on the evidence of its own throughput (twice the ceiling of the estimate, 8 Mb/s at least: the measurement is of one burst, the cap only guards against a misread one)
	r->idrLimited = (uint8_t)((budgetKbps > 0 || rate == 0 || held_windows(r) >= KVM_VRC_IDR_LIMITED_WINDOWS) ? 1 : 0);
	r->capTime = now ? now : 1u;
	{
		// a key frame that the viewer acknowledged proved what the link takes (kvm_vrc_idr_acked): R is not below that proof (and not above the ceiling of the estimate) while the link is not full (the
		// same rule as for the size of the key frame: refusals of the window in KVM_VRC_IDR_LIMITED_WINDOWS of the last windows say it is, and then the samples decide). What the window refused before the
		// ACK of the key frame was no refusal by a full link: the window was sized by a trickle (a rate of 33 B/s before the first key frame of a session) and the key frame was bigger than 64 KB. The proof is
		// not put into the estimate itself: it falls out of R when it expires or a refusal takes it away, and the samples of the video that goes meanwhile (kvm_vrc_update, m > cap) take over
		uint64_t eff = r->cap, target;
		if (r->capFloor > eff && held_windows(r) < KVM_VRC_IDR_LIMITED_WINDOWS) { eff = r->capFloor; if (eff > ceilCap) { eff = ceilCap; } }
		target = kvm_vrc_target(budgetKbps, eff);
		// the log field: R of this second is higher than it would be without the proof (not when the budget of the viewer is what R follows, nor when the proof is not used)
		r->rFromFloor = (uint8_t)(target > kvm_vrc_target(budgetKbps, r->cap) ? 1 : 0);
		kvm_vrc_set_target(r, target, now);
	}
}

int kvm_vrc_peek(const kvm_vrc *r, uint32_t now)
{
	kvm_vrc t = *r;
	refill(&t, now);
	return t.tokens > 0;
}

int kvm_vrc_may_encode(kvm_vrc *r, uint32_t now, int never_refused)
{
	refill(r, now);
	if (r->tokens > 0 || never_refused) { return 1; }
	r->winDenied++; r->sDenied++; r->lastDenied = now ? now : 1u;
	return 0;
}

// a refusal that is no evidence of the link (the window was sized by a lower bound, kvm_stream_note_link_held returned 0): it says the window was too small, not that the link is full. Only counted for the log
void kvm_vrc_window_refused_soft(kvm_vrc *r) { r->sHeld++; }
void kvm_vrc_window_refused(kvm_vrc *r) { r->winHeld++; r->sHeld++; r->cIdr = 0; r->capFloor = 0; }

void kvm_vrc_idr_acked(kvm_vrc *r, uint32_t now, uint32_t idrBytes, uint32_t ts, uint32_t ackAt, uint32_t queueMs, uint32_t minRtt)
{
	int64_t dur = (int64_t)(int32_t)(ackAt - ts) - (int64_t)queueMs - (int64_t)minRtt;
	if (idrBytes == 0) { return; }
	{ const uint32_t proofMin = (uint32_t)(KVM_VRC_IDR_PROOF_MIN_BYTES); if (proofMin != 0u && idrBytes < proofMin) { return; } }	// a key frame that small proves nothing about the link
	if (dur < 10) { dur = 10; }
	r->cIdr = ((uint64_t)idrBytes * 1000u) / (uint64_t)dur;
	r->cIdrAt = now ? now : 1u;
	{
		// the proof for the estimate: the same bytes over the whole time to the ACK, the round trip not taken off (minRtt may be the value of a queue, 46 ms against an ACK after 40 ms: then the
		// time is the 10 ms of the clamp and C_idr is no more than "very fast"). Bytes that were through in that time cannot be more than the link takes.
		int64_t whole = (int64_t)(int32_t)(ackAt - ts) - (int64_t)queueMs;
		if (whole < 10) { whole = 10; }
		r->capFloor = (((uint64_t)idrBytes * 1000u) / (uint64_t)whole) * KVM_VRC_RATE_PCT / 100u;
		r->capFloorAt = now ? now : 1u;
	}
}

void kvm_vrc_note_sharpen(kvm_vrc *r, uint32_t now, uint32_t bytes, int videoActive)
{
	r->sSharpBytes += bytes;
	if (!videoActive || bytes == 0) { return; }
	{
		const int64_t floor = -(int64_t)(r->rate * KVM_VRC_DEBT_MS / 1000u);
		refill(r, now);
		r->tokens -= (int64_t)bytes;
		if (r->tokens < floor) { r->tokens = floor; }
		r->winSharp = (r->winSharp > 0xFFFFFFFFu - bytes) ? 0xFFFFFFFFu : r->winSharp + bytes;
	}
}

void kvm_vrc_set_bitrate_mode(kvm_vrc *r, int on) { r->bitrateMode = (uint8_t)(on ? 1 : 0); r->belowRun = 0; }

uint32_t kvm_vrc_bitrate_bps(const kvm_vrc *r)
{
	uint64_t b = r->rate * 8u;
	if (b < 100000u) { b = 100000u; }
	if (b > 400000000u) { b = 400000000u; }
	return (uint32_t)b;
}

void kvm_vrc_frame(kvm_vrc *r, uint32_t now, uint32_t bytes, int qp, int isIdr, uint32_t encUs) { kvm_vrc_frame_virgin(r, now, bytes, qp, isIdr, encUs, 0); }

void kvm_vrc_frame_virgin(kvm_vrc *r, uint32_t now, uint32_t bytes, int qp, int isIdr, uint32_t encUs, uint32_t virginBytes)
{
	int64_t floor = -(int64_t)(r->rate * KVM_VRC_DEBT_MS / 1000u);
	refill(r, now);
	r->tokens -= (int64_t)bytes;
	if (r->tokens < floor) { r->tokens = floor; }	// what is owed beyond the limit is forgiven: a key frame must not stop the video for minutes
	r->sFrames++; r->sBytes += bytes; r->sEncUs += encUs; if (encUs > r->sEncMaxUs) { r->sEncMaxUs = encUs; }
	r->lastFrame = now ? now : 1u;
	if (isIdr) { r->sIdr++; if (bytes > 0) { r->lastIdrBytes = bytes; r->lastIdrQp = qp; r->idrMeasured = 1; r->lastIdrReal = r->idrReal; if (r->idrTotal != 0) { r->totalMbs = r->idrTotal; } } r->idrReal = 0; r->idrTotal = 0; return; }	// a key frame is not the content getting busier: not counted by the regulator
	r->winFrames++; r->winBytes += bytes > virginBytes ? bytes - virginBytes : 0u;	// (the bytes of macroblocks that were never converted since the flat key frame are not the content getting busier)
}

int kvm_vrc_tick(kvm_vrc *r, uint32_t now)
{
	int32_t dt = dms(now, r->winStart);
	uint64_t limit;
	int change = 0, over, under;
	if (dt < (int32_t)KVM_VRC_WINDOW_MS) { return 0; }
	r->heldHist = (r->heldHist << 1) | (r->winHeld > 0 ? 1ull : 0ull);	// (before the idle branch below: a silent window is a window without a refusal)
	{ const int nw = (int)(dt / (int32_t)KVM_VRC_WINDOW_MS); int k; for (k = 1; k < nw && k < 64; k++) { r->heldHist <<= 1; } }
	// a long silence (a still screen): the next motion starts from a moderate QP, not from the ceiling the last busy minute left (it takes a second per QP step down otherwise)
	if (r->lastFrame != 0 && dms(now, r->lastFrame) >= (int32_t)KVM_VRC_IDLE_MS && r->qp > KVM_VRC_IDLE_QP)
	{
		change = KVM_VRC_IDLE_QP - r->qp; r->qp = KVM_VRC_IDLE_QP; r->belowRun = 0; r->sQpDown++;
		r->winStart = now; r->winFrames = 0; r->winBytes = 0; r->winDenied = 0; r->winHeld = 0; r->winSharp = 0;
		return change;
	}
	limit = (r->rate * (uint64_t)dt) / 1000u;	// what R allows in this window
	if (r->winSharp > 0) { const uint64_t sh = r->winSharp; limit = (sh >= limit - limit / 4u) ? limit / 4u : limit - sh; }	// R less the sharpening of the window; the video keeps at least a quarter of R (the bucket was charged for the rest)
	if (r->bitrateMode) { r->winStart = now; r->winFrames = 0; r->winBytes = 0; r->winDenied = 0; r->winHeld = 0; r->winSharp = 0; return 0; }	// the encoder is steered by the bit rate
	over = ((uint64_t)r->winBytes * 100u > limit * KVM_VRC_UP_PCT) || (r->winDenied > 0 && (uint64_t)r->winBytes * 100u > limit * KVM_VRC_REFUSED_PCT);
	under = r->winFrames >= KVM_VRC_MIN_FRAMES && r->winDenied == 0 &&
		((uint64_t)r->winBytes * 100u < limit * KVM_VRC_DOWN_PCT || (uint64_t)r->winBytes * 100u < limit * KVM_VRC_CALM_PCT);	// (the second: QP stuck at the ceiling on a fast link, where the encode time limits the frames and 70 % never comes)
	if (under && r->winHeld > 0) { under = 0; r->sQpBlocked++; }	// the window of the viewer refused a frame in this window: the link, not the content, limits the stream
	if (over)
	{
		r->belowRun = 0;
		if (r->qp < KVM_VIDEO_QP_MAX) { r->qp += KVM_VRC_QP_UP; if (r->qp > KVM_VIDEO_QP_MAX) { r->qp = KVM_VIDEO_QP_MAX; } change = KVM_VRC_QP_UP; r->sQpUp++; }
	}
	else if (under)
	{
		if (++r->belowRun >= KVM_VRC_DOWN_WINDOWS)
		{
			r->belowRun = 0;
			if (r->qp > r->qpMin) { r->qp -= KVM_VRC_QP_DOWN; change = -KVM_VRC_QP_DOWN; r->sQpDown++; }
		}
	}
	else if (r->winFrames > 0) { r->belowRun = 0; }	// a busy window that is neither: the run of quiet windows is broken (an idle one does not break it)
	r->winStart = now; r->winFrames = 0; r->winBytes = 0; r->winDenied = 0; r->winHeld = 0; r->winSharp = 0;
	return change;
}

int kvm_vrc_update_starved(kvm_vrc *r, uint32_t now)
{
	if (r->qp >= KVM_VIDEO_QP_MAX && r->sFrames < (uint32_t)KVM_VRC_STARVED_FPS && (r->sDenied > 0 || r->sHeld > 0))
	{
		if (r->starveSince == 0) { r->starveSince = now ? now : 1u; }
		r->starved = dms(now, r->starveSince) >= (int32_t)KVM_VRC_STARVED_MS ? 1 : 0;
	}
	else { r->starveSince = 0; r->starved = 0; }
	return r->starved;
}

void kvm_vrc_clear_stats(kvm_vrc *r)
{
	r->sFrames = r->sIdr = r->sDenied = r->sHeld = r->sQpUp = r->sQpDown = r->sQpBlocked = 0; r->sBytes = r->sEncUs = r->sSharpBytes = 0; r->sEncMaxUs = 0;
}
