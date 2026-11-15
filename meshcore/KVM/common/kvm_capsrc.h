/*
KVM: choice of the capture source of the Windows agent: DXGI Desktop Duplication or GDI. Portable C99, no operating system API, time is an injected millisecond counter
(uint32_t, wraps), so the tests are deterministic. Windows/tile.cpp is the glue: it measures (images of the duplication, comparisons of GDI captures and of the persistent image)
and does what this module decides (re-create the duplication, capture with GDI, try DXGI again).

Two failures of DXGI look alike from the outside and are told apart here:

  silent     the duplication delivers no change although the screen changes (seen on a laptop whose internal panel is off, lid closed: AcquireNextFrame times out for the whole session,
             GDI still sees every change). Evidence: no image since the persistent image began, and a GDI capture differs from the REFERENCE, twice in a row, KVM_CAPSRC_CONFIRM_MS apart.
             The reference is a GDI capture (with CAPTUREBLT, the way the comparison is made) that the glue takes at the moment the persistent image begins (the duplication was opened and the image
             was filled from GDI, or a full copy arrived), see kvm_capsrc_ref_wanted(). GDI against GDI: whatever DXGI shows (HDR colours, layered windows, the cursor) does not matter, only
             "the screen changed and DXGI said nothing". Action: capture with GDI (the path of MESH_KVM_FORCE_GDI) and try DXGI again from time to time (probation, below).
  differs    the duplication delivers images and the persistent image still differs from a GDI capture (HDR, a layered window), or it went quiet after it had delivered images. Action as
             before the first time the duplication is created again; if it differs again after that, "systematic": only measured and logged. That latch is no longer for the life of the
             process: it ends when the two pictures agree again, after KVM_CAPSRC_SYSTEMATIC_MS, on a change of the desktop or of the geometry. Behind the latch the proof of silence is again
             GDI against GDI: the reference is the capture of the previous comparison, and a screen that changed with no image since is silence (a moved mouse does not count).

A static screen never switches anything: without a change of the screen there is no evidence, whatever DXGI says.

GDI mode: every KVM_CAPSRC_PROBE_FIRST_MS << failed probes (at most KVM_CAPSRC_PROBE_MAX_MS) the glue creates the duplication again (the outputs are listed again at that moment: an
inactive output, a changed active output) and the module is in probation. Probation is a SHADOW: the mode stays GDI, so the viewer keeps getting true pictures and nothing stops; the
duplication is only polled and its images go to a private copy. DXGI becomes the source only when the evidence says it works: at least KVM_CAPSRC_PROBE_UPDATES images arrived after the first
full copy, the private copy and a GDI capture agree, and no difference was noted (a difference noted in probation, even a single one that was not confirmed, ends the probe as failed
when the time is up). A probe ends as failed when GDI differs from the reference twice (KVM_CAPSRC_PROBE_NEED, KVM_CAPSRC_PROBE_CONFIRM_MS apart: DXGI is silent, or only late by a few frames) or when the
duplication could not be kept open (a longer pause follows). A busy screen is never quiet enough to compare: images and no difference noted are a success KVM_CAPSRC_PROBE_BUSY_MS after the
probe began. A probe ends as inconclusive when KVM_CAPSRC_PROBATION_MS passed on a static screen (nothing arrived, nothing differs: GDI stays, the
pause stays, nothing is held against DXGI). The cost of GDI comparisons in DXGI mode is the one before the capture source choice (every KVM_CAPSRC_PERIOD_MS, only after KVM_CAPSRC_QUIET_MS without an image), plus
a short window after every opening of the duplication: KVM_CAPSRC_FAST_COUNT comparisons KVM_CAPSRC_FAST_MS apart while nothing has arrived, and one confirming comparison
KVM_CAPSRC_CONFIRM_MS after a difference. On a machine that fell to GDI before in this process, a duplication that has delivered nothing since it was opened is compared every
KVM_CAPSRC_WATCH_MS for its first KVM_CAPSRC_WATCH_WINDOW_MS (silence would otherwise be found up to a whole period late).

Values marked (T) are tunable with /D<NAME>=<value>.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef KVM_CAPSRC_H
#define KVM_CAPSRC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef KVM_CAPSRC_QUIET_MS
#ifdef KVM_STALE_QUIET_MS
#define KVM_CAPSRC_QUIET_MS KVM_STALE_QUIET_MS	// (T) DXGI must have delivered nothing for this long before the pictures are compared
#else
#define KVM_CAPSRC_QUIET_MS 1000
#endif
#endif
#ifndef KVM_CAPSRC_PERIOD_MS
#ifdef KVM_STATS_PERIOD_MS
#define KVM_CAPSRC_PERIOD_MS KVM_STATS_PERIOD_MS	// (T) the pictures are compared at most this often in the steady state (the cost before the capture source choice)
#else
#define KVM_CAPSRC_PERIOD_MS 10000
#endif
#endif
#ifndef KVM_CAPSRC_FIRST_MS
#define KVM_CAPSRC_FIRST_MS 2000		// (T) first comparison after the duplication was opened, while nothing has arrived
#endif
#ifndef KVM_CAPSRC_FAST_MS
#define KVM_CAPSRC_FAST_MS 3000			// (T) the following ones while nothing has arrived ...
#endif
#ifndef KVM_CAPSRC_FAST_COUNT
#define KVM_CAPSRC_FAST_COUNT 3			// (T) ... KVM_CAPSRC_FAST_COUNT comparisons in all, then KVM_CAPSRC_PERIOD_MS
#endif
#ifndef KVM_CAPSRC_CONFIRM_MS
#define KVM_CAPSRC_CONFIRM_MS 1000		// (T) a comparison that found a difference is repeated after this long
#endif
#ifndef KVM_CAPSRC_CONFIRM_DIFFS
#define KVM_CAPSRC_CONFIRM_DIFFS 2		// (T) differences in a row that decide (outside probation; in probation KVM_CAPSRC_PROBE_NEED)
#endif
#ifndef KVM_CAPSRC_PROBE_FIRST_MS
#define KVM_CAPSRC_PROBE_FIRST_MS 5000	// (T) first pause in GDI mode before DXGI is tried again; doubles with every failed probe
#endif
#ifndef KVM_CAPSRC_PROBE_MAX_MS
#define KVM_CAPSRC_PROBE_MAX_MS 60000	// (T) longest pause
#endif
#ifndef KVM_CAPSRC_PROBATION_MS
#define KVM_CAPSRC_PROBATION_MS 20000	// (T) a probe lasts at most this long (it is a shadow: the viewer is not waiting for it, a static screen needs time to show a change)
#endif
#ifndef KVM_CAPSRC_PROBE_BUSY_MS
#define KVM_CAPSRC_PROBE_BUSY_MS 4000	// (T) a probe that has seen images and noted no difference ends in success this long after it began, if no comparison decided before (a busy screen is never quiet)
#endif
#ifndef KVM_CAPSRC_PROBE_QUIET_MS
#define KVM_CAPSRC_PROBE_QUIET_MS 600	// (T) in probation: silence needed before a comparison, and the spacing of the first KVM_CAPSRC_FAST_COUNT ones
#endif
#ifndef KVM_CAPSRC_PROBE_PERIOD_MS
#define KVM_CAPSRC_PROBE_PERIOD_MS 2000	// (T) in probation: spacing of the later comparisons
#endif
#ifndef KVM_CAPSRC_PROBE_CONFIRM_MS
#define KVM_CAPSRC_PROBE_CONFIRM_MS 200	// (T) in probation: a comparison that found a difference is repeated after this long (a change that GDI shows and DXGI delivers a frame later is no failure)
#endif
#ifndef KVM_CAPSRC_PROBE_NEED
#define KVM_CAPSRC_PROBE_NEED 2			// (T) differences in a row that fail a probe
#endif
#ifndef KVM_CAPSRC_WATCH_MS
#define KVM_CAPSRC_WATCH_MS 1500		// (T) spacing of the comparisons of a duplication that delivered nothing yet, on a machine that fell to GDI before
#endif
#ifndef KVM_CAPSRC_WATCH_WINDOW_MS
#define KVM_CAPSRC_WATCH_WINDOW_MS 60000	// (T) ... for this long after it was opened, then KVM_CAPSRC_PERIOD_MS again
#endif
#ifndef KVM_CAPSRC_PROBE_MIN_MS
#define KVM_CAPSRC_PROBE_MIN_MS 1000	// (T) a probe is not a success before this long
#endif
#ifndef KVM_CAPSRC_PROBE_UPDATES
#define KVM_CAPSRC_PROBE_UPDATES 1		// (T) images after the first full copy that make a probe a success
#endif
#ifndef KVM_CAPSRC_SYSTEMATIC_MS
#define KVM_CAPSRC_SYSTEMATIC_MS 300000	// (T) the "systematic" latch ends after this long and the difference is judged again
#endif

enum { KVM_CAPSRC_MODE_DXGI = 0, KVM_CAPSRC_MODE_GDI = 1 };
enum { KVM_CAPSRC_R_NONE = 0, KVM_CAPSRC_R_SILENT = 1, KVM_CAPSRC_R_UNAVAILABLE = 2 };	// why GDI is used

// What the glue has to do after a call.
enum {
	KVM_CAPSRC_ACT_NONE = 0,
	KVM_CAPSRC_ACT_SUSPECT = 1,			// a difference was found, it is confirmed by the next comparison (log only)
	KVM_CAPSRC_ACT_REOPEN = 2,			// "differs", first time: create the duplication again (close it, call kvm_capsrc_opened() when it is open)
	KVM_CAPSRC_ACT_SYSTEMATIC = 3,		// "differs" after the duplication was created again: measure and log only, latch set
	KVM_CAPSRC_ACT_CLEARED = 4,			// the latch ended because the pictures agree again
	KVM_CAPSRC_ACT_TO_GDI = 5,			// "silent" (or DXGI unavailable): close the duplication, capture with GDI from now on, this frame included
	KVM_CAPSRC_ACT_PROBE_FAILED = 6,	// a probe did not succeed: close the duplication (GDI is the source all along)
	KVM_CAPSRC_ACT_RECOVERED = 7,		// a probe succeeded: DXGI is the source from the next frame, its persistent image begins again (the duplication stays open)
	KVM_CAPSRC_ACT_INCONCLUSIVE = 8		// a probe on a static screen ended without evidence: close the duplication, GDI stays, the next probe comes after the same pause
};

typedef struct kvm_capsrc_stats {
	uint32_t toGdi, probes, probeFails, recovered, inconclusive, reopens, systematic, cleared;	// counters since init
	uint64_t gdiMs;						// time spent in GDI mode (closed periods)
} kvm_capsrc_stats;

typedef struct kvm_capsrc {
	int mode, reason, probation;		// probation: a shadow probe, mode is GDI meanwhile
	int cmpCount, diffRun, afterReinit, systematic, fails, gdiActive;
	int refValid, needRef;				// the glue holds a reference GDI capture (valid when refUpdates == updates), the glue is asked to take one
	int noted;							// probation: a difference was noted and no later comparison found the pictures equal
	uint32_t openAt, lastImage, lastCmp, probeAt, gdiSince, nextProbe, systematicAt;
	uint32_t updates, refUpdates;		// images since the duplication was opened, not counting full copies; the value when the reference was taken
	uint32_t lastGdiMs;					// length of the last GDI period (for the log, set when DXGI is the source again)
	uint32_t lastProbeUpdates;			// images the last successful probe saw (for the log)
	kvm_capsrc_stats st;
} kvm_capsrc;

void kvm_capsrc_init(kvm_capsrc *c, uint32_t now);
int kvm_capsrc_mode(const kvm_capsrc *c);
int kvm_capsrc_probation(const kvm_capsrc *c);					// a shadow probe is running (the source is GDI)
const char *kvm_capsrc_mode_name(const kvm_capsrc *c);		// "dxgi", "probe" (DXGI on trial in the shadow of GDI), "gdi"
const char *kvm_capsrc_reason_name(int reason);				// "dxgi_silent", "dxgi_unavailable", "none"

// Events from the glue (DXGI mode).
void kvm_capsrc_opened(kvm_capsrc *c, uint32_t now);			// a duplication was opened (also after a re-creation)
void kvm_capsrc_image(kvm_capsrc *c, uint32_t now, int fullCopy);	// an image with rectangles arrived; fullCopy: the whole texture was copied (the persistent image begins: a reference is wanted)
void kvm_capsrc_fed(kvm_capsrc *c);								// the persistent image was filled from a GDI capture (it begins: a reference is wanted)
void kvm_capsrc_reset(kvm_capsrc *c, uint32_t now);				// the desktop or the geometry changed: latches and a probation end, in GDI mode DXGI is tried at once

// The reference GDI capture (with CAPTUREBLT, same flags as the comparison). The glue takes it when asked, right after the event, and says whether it worked.
int kvm_capsrc_ref_wanted(const kvm_capsrc *c);
void kvm_capsrc_ref_done(kvm_capsrc *c, int ok);				// the reference was taken (ok) or could not be taken
int kvm_capsrc_ref_usable(const kvm_capsrc *c);					// the glue may compare against its reference (no image arrived since it was taken)

// Should the pictures be compared now? (only on an iteration that got no image; in DXGI mode and in probation)
int kvm_capsrc_compare_due(const kvm_capsrc *c, uint32_t now);
// Result of the comparison, differing pixels (0 = equal, -1 = could not be made or no such picture): persistentPx: persistent image against a GDI capture made now (cursor field left
// out); refPx: that GDI capture against the reference (the cursor fields of both left out). Returns KVM_CAPSRC_ACT_*.
int kvm_capsrc_compare_result(kvm_capsrc *c, uint32_t now, int64_t persistentPx, int64_t refPx);
// Once per frame in probation: ends a probe that ran out. dxOpen: the duplication is open. Returns KVM_CAPSRC_ACT_*.
int kvm_capsrc_tick(kvm_capsrc *c, uint32_t now, int dxOpen);
// DXGI cannot be used now (no output, rotated, errors): GDI, or, in probation, a failed probe. Returns TO_GDI or PROBE_FAILED.
int kvm_capsrc_unavailable(kvm_capsrc *c, uint32_t now);

// GDI mode.
int kvm_capsrc_probe_due(const kvm_capsrc *c, uint32_t now);
void kvm_capsrc_probe_begin(kvm_capsrc *c, uint32_t now);		// DXGI on trial in the shadow (mode stays GDI): the glue creates the duplication now and only polls it
uint32_t kvm_capsrc_probe_interval(const kvm_capsrc *c);		// the pause that follows the failed probes counted so far
uint32_t kvm_capsrc_gdi_ms(const kvm_capsrc *c, uint32_t now);	// time in GDI mode, the closed periods and the current one

// what the capture says to the choice of the video encoder. 0 = not known yet, 1 = GDI is the source (or will be), 2 = DXGI is the source AND has delivered an image since it was opened. Not "is open":
// with the lid closed the duplication is open and silent for seconds, and the hardware encoder (or its self test) must not start then. inited: the capture has run; dxDisabled: DXGI is off for the process;
// gdiSource: this module says GDI (or a probe is under way); dxOpen: the duplication is open; dxImages: the images received since it was opened (it starts from 0 at every open).
int kvm_capsrc_known_state(int inited, int dxDisabled, int gdiSource, int dxOpen, int dxImages);

#ifdef __cplusplus
}
#endif

#endif
