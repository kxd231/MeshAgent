/*
KVM: choice of the capture source (DXGI or GDI). See kvm_capsrc.h. Portable C99, no operating system API.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#include "kvm_capsrc.h"
#include <string.h>

static int32_t dms(uint32_t a, uint32_t b) { return (int32_t)(a - b); }	// a - b on the wrapping millisecond counter

void kvm_capsrc_init(kvm_capsrc *c, uint32_t now)
{
	memset(c, 0, sizeof(*c));
	c->mode = KVM_CAPSRC_MODE_DXGI;
	c->reason = KVM_CAPSRC_R_NONE;
	c->openAt = c->lastImage = c->lastCmp = now;
}

int kvm_capsrc_mode(const kvm_capsrc *c) { return c->mode; }
int kvm_capsrc_probation(const kvm_capsrc *c) { return c->probation; }

int kvm_capsrc_known_state(int inited, int dxDisabled, int gdiSource, int dxOpen, int dxImages)
{
	if (!inited) { return 0; }
	if (dxDisabled || gdiSource) { return 1; }
	return (dxOpen && dxImages > 0) ? 2 : 0;
}

const char *kvm_capsrc_mode_name(const kvm_capsrc *c)
{
	if (c->probation) { return "probe"; }
	return c->mode == KVM_CAPSRC_MODE_GDI ? "gdi" : "dxgi";
}

const char *kvm_capsrc_reason_name(int reason)
{
	switch (reason)
	{
		case KVM_CAPSRC_R_SILENT: return "dxgi_silent";
		case KVM_CAPSRC_R_UNAVAILABLE: return "dxgi_unavailable";
		default: return "none";
	}
}

// A new picture is started from the next duplication: nothing counted for the old one applies.
static void restart_counts(kvm_capsrc *c, uint32_t now)
{
	c->openAt = c->lastImage = c->lastCmp = now;
	c->updates = c->refUpdates = 0;
	c->cmpCount = 0;
	c->diffRun = 0;
	c->refValid = c->needRef = 0;
	c->noted = 0;
}

void kvm_capsrc_opened(kvm_capsrc *c, uint32_t now) { restart_counts(c, now); }

void kvm_capsrc_image(kvm_capsrc *c, uint32_t now, int fullCopy)
{
	c->lastImage = now;
	c->diffRun = 0;
	if (fullCopy)
	{
		// The persistent image begins here: what DXGI says. The reference is the GDI capture of this moment, but only where it will be used (nothing arrived yet, the latch, a probe):
		// a driver without metadata makes every image a full copy, and every one would cost a GDI capture.
		if (c->updates == 0 || c->systematic || c->probation) { c->needRef = 1; }
		c->refValid = 0;
	}
	else
	{
		c->updates++;
	}
}

void kvm_capsrc_fed(kvm_capsrc *c)
{
	c->needRef = 1;
	c->refValid = 0;
}

int kvm_capsrc_ref_wanted(const kvm_capsrc *c) { return c->needRef; }

void kvm_capsrc_ref_done(kvm_capsrc *c, int ok)
{
	c->needRef = 0;
	c->refValid = ok ? 1 : 0;
	c->refUpdates = c->updates;
}

int kvm_capsrc_ref_usable(const kvm_capsrc *c) { return c->refValid && c->refUpdates == c->updates; }

void kvm_capsrc_reset(kvm_capsrc *c, uint32_t now)
{
	c->systematic = 0;
	c->afterReinit = 0;
	c->diffRun = 0;
	if (c->mode == KVM_CAPSRC_MODE_GDI)
	{
		// A probe in progress ends without a verdict (the duplication is closed by the glue): the new desktop or geometry is tried at once
		c->probation = 0;
		c->nextProbe = now;
	}
}

uint32_t kvm_capsrc_probe_interval(const kvm_capsrc *c)
{
	uint32_t iv = KVM_CAPSRC_PROBE_FIRST_MS;
	int i;
	for (i = 0; i < c->fails && iv < KVM_CAPSRC_PROBE_MAX_MS; i++) { iv *= 2u; }
	return iv > KVM_CAPSRC_PROBE_MAX_MS ? KVM_CAPSRC_PROBE_MAX_MS : iv;
}

uint32_t kvm_capsrc_gdi_ms(const kvm_capsrc *c, uint32_t now)
{
	uint64_t t = c->st.gdiMs;
	if (c->gdiActive) { t += (uint32_t)dms(now, c->gdiSince); }
	return t > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)t;
}

static void close_gdi_period(kvm_capsrc *c, uint32_t now)
{
	if (c->gdiActive)
	{
		c->lastGdiMs = (uint32_t)dms(now, c->gdiSince);
		c->st.gdiMs += c->lastGdiMs;
		c->gdiActive = 0;
	}
}

// DXGI is not the source: GDI. A probation that fails makes the next pause longer.
static int enter_gdi(kvm_capsrc *c, uint32_t now, int reason)
{
	int act = KVM_CAPSRC_ACT_TO_GDI;
	if (c->probation)
	{
		c->fails++;
		c->st.probeFails++;
		act = KVM_CAPSRC_ACT_PROBE_FAILED;
	}
	else
	{
		c->st.toGdi++;
	}
	if (!c->gdiActive) { c->gdiActive = 1; c->gdiSince = now; }
	c->probation = 0;
	c->mode = KVM_CAPSRC_MODE_GDI;
	c->reason = reason;
	c->diffRun = 0;
	c->nextProbe = now + kvm_capsrc_probe_interval(c);
	return act;
}

int kvm_capsrc_unavailable(kvm_capsrc *c, uint32_t now)
{
	return enter_gdi(c, now, KVM_CAPSRC_R_UNAVAILABLE);
}

// A probe succeeded: DXGI is the source from the next frame on, and its persistent image begins again (the glue fills it from GDI or from a full copy).
static int recover(kvm_capsrc *c, uint32_t now)
{
	close_gdi_period(c, now);
	c->lastProbeUpdates = c->updates;
	c->probation = 0;
	c->mode = KVM_CAPSRC_MODE_DXGI;
	c->fails = 0;
	c->reason = KVM_CAPSRC_R_NONE;
	c->st.recovered++;
	restart_counts(c, now);
	return KVM_CAPSRC_ACT_RECOVERED;
}

int kvm_capsrc_compare_due(const kvm_capsrc *c, uint32_t now)
{
	if (c->probation)
	{
		// A shadow probe: the viewer is not waiting, but a verdict is wanted soon. Spacing 0.6 s while it is new, then 2 s; a difference is looked at again after 0.2 s.
		if (dms(now, c->lastImage) < (int32_t)KVM_CAPSRC_PROBE_QUIET_MS) { return 0; }
		if (c->diffRun > 0) { return dms(now, c->lastCmp) >= (int32_t)KVM_CAPSRC_PROBE_CONFIRM_MS; }
		return dms(now, c->lastCmp) >= (int32_t)(c->cmpCount < KVM_CAPSRC_FAST_COUNT ? KVM_CAPSRC_PROBE_QUIET_MS : KVM_CAPSRC_PROBE_PERIOD_MS);
	}
	if (c->mode != KVM_CAPSRC_MODE_DXGI) { return 0; }
	if (dms(now, c->lastImage) < (int32_t)KVM_CAPSRC_QUIET_MS) { return 0; }
	if (c->diffRun > 0) { return dms(now, c->lastCmp) >= (int32_t)KVM_CAPSRC_CONFIRM_MS; }
	if (c->updates == 0 && !c->systematic)
	{
		// Nothing has arrived since the duplication was opened: look sooner, a few times
		if (c->cmpCount == 0) { return dms(now, c->openAt) >= (int32_t)KVM_CAPSRC_FIRST_MS; }
		if (c->cmpCount < KVM_CAPSRC_FAST_COUNT) { return dms(now, c->lastCmp) >= (int32_t)KVM_CAPSRC_FAST_MS; }
		// A machine that has fallen to GDI before: a silent duplication on a static screen is found when the screen changes, not up to a period later
		if (c->st.toGdi > 0 && dms(now, c->openAt) < (int32_t)KVM_CAPSRC_WATCH_WINDOW_MS) { return dms(now, c->lastCmp) >= (int32_t)KVM_CAPSRC_WATCH_MS; }
	}
	return dms(now, c->lastCmp) >= (int32_t)KVM_CAPSRC_PERIOD_MS;
}

// A shadow probe: the private copy of the images (persistentPx) against GDI, and GDI against the reference (refPx).
static int probation_result(kvm_capsrc *c, uint32_t now, int64_t persistentPx, int refOk, int64_t refPx)
{
	int64_t px;
	if (c->updates == 0)
	{
		// Nothing arrived: the screen changing since the reference is silence
		if (!refOk) { c->needRef = 1; return KVM_CAPSRC_ACT_NONE; }	// no usable reference (it could not be taken): ask again, the comparison's own capture becomes one
		px = refPx;
	}
	else
	{
		if (persistentPx < 0) { return KVM_CAPSRC_ACT_NONE; }
		px = persistentPx;
	}
	if (px == 0)
	{
		c->diffRun = 0;
		c->noted = 0;
		// Success: images after the first full copy, the pictures agree, the probe ran for its minimum (a static screen with no image is not a success)
		if (c->updates >= KVM_CAPSRC_PROBE_UPDATES && dms(now, c->probeAt) >= (int32_t)KVM_CAPSRC_PROBE_MIN_MS) { return recover(c, now); }
		return KVM_CAPSRC_ACT_NONE;
	}
	c->noted = 1;
	if (++c->diffRun >= KVM_CAPSRC_PROBE_NEED) { return enter_gdi(c, now, KVM_CAPSRC_R_SILENT); }
	return KVM_CAPSRC_ACT_SUSPECT;
}

// Where a reference is used (nothing arrived yet, the latch, a probe) and there is none: it is asked for again at every comparison, however that comparison turned out. The capture of the
// comparison becomes the reference, so what a failed capture cost is one comparison.
static int ref_lacking(const kvm_capsrc *c)
{
	return (c->updates == 0 || c->systematic || c->probation) && !kvm_capsrc_ref_usable(c);
}

int kvm_capsrc_compare_result(kvm_capsrc *c, uint32_t now, int64_t persistentPx, int64_t refPx)
{
	const int need = KVM_CAPSRC_CONFIRM_DIFFS;
	const int refOk = (refPx >= 0 && kvm_capsrc_ref_usable(c));
	int act = KVM_CAPSRC_ACT_NONE;

	c->lastCmp = now;
	if (persistentPx < 0 && refPx < 0)
	{
		// Nothing is learned (the comparison could not be made, or there is no persistent image and no usable reference). Where a reference is used, ask for one again.
		if (ref_lacking(c)) { c->needRef = 1; }
		return KVM_CAPSRC_ACT_NONE;
	}
	c->cmpCount++;
	if (c->probation) { return probation_result(c, now, persistentPx, refOk, refPx); }

	if (persistentPx == 0)
	{
		// The picture DXGI keeps is what GDI shows
		c->diffRun = 0;
		c->afterReinit = 0;
		if (c->systematic) { c->systematic = 0; c->st.cleared++; act = KVM_CAPSRC_ACT_CLEARED; }
		if (ref_lacking(c)) { c->needRef = 1; }
		return act;
	}

	if (c->systematic && dms(now, c->systematicAt) >= (int32_t)KVM_CAPSRC_SYSTEMATIC_MS) { c->systematic = 0; c->afterReinit = 0; }
	if (c->systematic)
	{
		// DXGI and GDI differ for good. The proof of silence is GDI against GDI: the screen changed since the reference (the previous comparison) and DXGI delivered nothing meanwhile.
		// What DXGI shows is of no interest, so a constant colour difference and a moving mouse prove nothing. A reference that images have outdated is replaced.
		if (refOk && refPx > 0)
		{
			if (++c->diffRun >= need) { return enter_gdi(c, now, KVM_CAPSRC_R_SILENT); }
			return KVM_CAPSRC_ACT_SUSPECT;	// the reference stays: the next comparison is against the same picture
		}
		c->diffRun = 0;
		if (!refOk || refPx > 0) { c->needRef = 1; }
		return KVM_CAPSRC_ACT_NONE;
	}

	if (c->updates == 0)
	{
		// DXGI has reported nothing since the persistent image began, and the screen changed since (GDI against the reference taken when it began): silence, whatever the persistent image looks like
		if (!refOk) { c->needRef = 1; return KVM_CAPSRC_ACT_NONE; }	// a failed capture of the reference must not end the proof of silence for good: nothing else would ask again
		if (refPx == 0) { c->diffRun = 0; return KVM_CAPSRC_ACT_NONE; }
		if (++c->diffRun >= need) { return enter_gdi(c, now, KVM_CAPSRC_R_SILENT); }
		return KVM_CAPSRC_ACT_SUSPECT;
	}

	// DXGI delivered images and the pictures differ: it went quiet (create it again) or it differs (latch after the re-creation)
	if (persistentPx < 0) { return KVM_CAPSRC_ACT_NONE; }
	c->diffRun++;
	if (c->afterReinit)
	{
		c->systematic = 1;
		c->systematicAt = now;
		c->diffRun = 0;
		c->needRef = 1;		// behind the latch GDI is compared with GDI: the reference starts at this comparison
		c->st.systematic++;
		return KVM_CAPSRC_ACT_SYSTEMATIC;
	}
	if (c->diffRun >= KVM_CAPSRC_CONFIRM_DIFFS)
	{
		c->afterReinit = 1;
		c->diffRun = 0;
		c->st.reopens++;
		return KVM_CAPSRC_ACT_REOPEN;
	}
	return KVM_CAPSRC_ACT_SUSPECT;
}

int kvm_capsrc_tick(kvm_capsrc *c, uint32_t now, int dxOpen)
{
	if (!c->probation) { return KVM_CAPSRC_ACT_NONE; }
	// Images arrived: the verdict is due early (a busy screen never gets quiet enough for a comparison); nothing arrived: a static screen needs the whole time.
	if (dms(now, c->probeAt) < (int32_t)(c->updates >= KVM_CAPSRC_PROBE_UPDATES ? KVM_CAPSRC_PROBE_BUSY_MS : KVM_CAPSRC_PROBATION_MS)) { return KVM_CAPSRC_ACT_NONE; }
	// Images and no difference noted: DXGI works. A difference noted, even once, is no success.
	if (c->updates >= KVM_CAPSRC_PROBE_UPDATES) { return c->noted ? enter_gdi(c, now, KVM_CAPSRC_R_SILENT) : recover(c, now); }
	if (!dxOpen) { return enter_gdi(c, now, KVM_CAPSRC_R_UNAVAILABLE); }
	if (c->noted) { return enter_gdi(c, now, KVM_CAPSRC_R_SILENT); }
	// Open, quiet, nothing differs: a static screen. Nothing is known; GDI stays, the pause is not forgotten (fails is kept) and not made longer.
	c->probation = 0;
	c->diffRun = 0;
	c->nextProbe = now + kvm_capsrc_probe_interval(c);
	c->st.inconclusive++;
	return KVM_CAPSRC_ACT_INCONCLUSIVE;
}

int kvm_capsrc_probe_due(const kvm_capsrc *c, uint32_t now)
{
	return c->mode == KVM_CAPSRC_MODE_GDI && !c->probation && dms(now, c->nextProbe) >= 0;
}

void kvm_capsrc_probe_begin(kvm_capsrc *c, uint32_t now)
{
	c->probation = 1;		// the mode stays GDI: the duplication is polled in the shadow
	c->probeAt = now;
	c->systematic = 0;
	c->afterReinit = 0;
	c->st.probes++;
	restart_counts(c, now);
}
