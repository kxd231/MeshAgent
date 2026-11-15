/*
KVM: does the encoder listen to the quantizer? Portable C99, no operating system API, no libm.

On Windows 10 (19044) setting AVEncVideoEncodeQP changes the size of the P frames about 11 % per step; on Windows 11 build 26200 it did not (5331 B at QP 20 and at QP 40,
and the P frames of that measurement were 6-7.6 KB for QP 26-32). The regulator of kvm_video_rc.c has nothing to steer with then. This detector watches the frames the agent really produces and says so:

  frame  for every P frame: the QP that was asked for, the QP the encoder says it used (the output sample attribute MFSampleExtension_VideoEncodeQP, -1 = not known) and the size.
  event  a change of the asked QP by at least KVM_QPDET_MIN_STEP between two P frames, with KVM_QPDET_PRE frames before it at the old QP and (after skipping one) KVM_QPDET_POST frames at the new one.
         Expected: the size changes by ln(0.89) per step (measured), the reported QP follows the asked one.
  guard  an event is not counted when the covered area changed by more than 25 % across the step (a burst of motion), when its frames are tiny, or when it follows the previous one within 20 frames
         (the same content); the size is judged per covered tile; one event adds at most KVM_QPDET_EVENT_MAX to the sum, so a verdict needs two agreeing events.
  test   a sequential probability ratio test on the events: log-likelihood ratio of "the QP does not matter" against "it does", with the noise of the size of a frame (sigma of ln(bytes) of the
         mean of 3 frames and the difference of two of them, KVM_QPDET_SIGMA_X1000), plus a fixed weight for an event in which the reported QP did not follow. A reported QP that follows the asked one is the better signal and decides the event ("the QP works") whatever the sizes did, and the verdict is never "ineffective" while it has followed and never failed to; a reported QP that sits above the asked one before and after the step is the floor of the encoder (26 for a 20 on Windows 11) and counts for nothing. At +KVM_QPDET_LLR_LIMIT: INEFFECTIVE, at
         -KVM_QPDET_LLR_LIMIT: EFFECTIVE (both final until kvm_qpdet_reset).
  probe  the regulator moves the QP rarely (measured: once in 22 s). When no verdict has come after KVM_QPDET_PROBE_AFTER P frames the detector asks for a deliberate step of KVM_QPDET_PROBE_STEP for
         KVM_QPDET_PROBE_FRAMES frames (kvm_qpdet_probe_offset): the frames are a little smaller or blurrier for a moment and the next event is decisive (4 steps = 47 % in size).

The glue (Windows/kvm_video.cpp) reacts to INEFFECTIVE in two stages: first it sets the QP in the input sample too (MFSampleExtension_VideoEncodeQP, mf_h264.cpp), then it gives up the QP and steers the
encoder by its bit rate (kvm_vrc_set_bitrate_mode).

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef KVM_QPDET_H
#define KVM_QPDET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef KVM_QPDET_MIN_STEP
#define KVM_QPDET_MIN_STEP 2			// (T) smaller changes of the QP are not an event (11 % per step is within the noise)
#endif
#ifndef KVM_QPDET_PRE
#define KVM_QPDET_PRE 3
#endif
#ifndef KVM_QPDET_POST
#define KVM_QPDET_POST 3
#endif
#ifndef KVM_QPDET_SIGMA_X1000
#define KVM_QPDET_SIGMA_X1000 120		// (T) noise of ln(mean of 3 frames after / mean of 3 before)
#endif
#ifndef KVM_QPDET_LLR_LIMIT
#define KVM_QPDET_LLR_LIMIT 4			// (T) the sum of the log-likelihood ratios that ends the test (e^4 = 55 : 1)
#endif
#ifndef KVM_QPDET_EVENT_MAX
#define KVM_QPDET_EVENT_MAX 3			// (T) the most one event adds to the sum (either way): with the limit below, a verdict needs two agreeing events
#endif
#ifndef KVM_QPDET_MIN_EVENTS
#define KVM_QPDET_MIN_EVENTS 2			// (T) no verdict before this many counted events
#endif
#ifndef KVM_QPDET_AREA_TOL_PCT
#define KVM_QPDET_AREA_TOL_PCT 25		// (T) an event in which the covered area changed by more than this (up, or its inverse down) is not counted
#endif
#ifndef KVM_QPDET_MIN_BYTES
#define KVM_QPDET_MIN_BYTES 1500		// (T) ... nor one whose frames before the step are smaller than this (mean)
#endif
#ifndef KVM_QPDET_EVENT_GAP
#define KVM_QPDET_EVENT_GAP 20			// (T) ... nor one that comes sooner than this many P frames after the previous counted event (the same content)
#endif
#ifndef KVM_QPDET_REPORT_WEIGHT
#define KVM_QPDET_REPORT_WEIGHT 3		// (T) the weight of an event in which the reported QP did not follow
#endif
#ifndef KVM_QPDET_PROBE_AFTER
#define KVM_QPDET_PROBE_AFTER 40		// (T) P frames without a verdict before the first deliberate step
#endif
#ifndef KVM_QPDET_PROBE_EVERY
#define KVM_QPDET_PROBE_EVERY 60		// (T) ... and between two of them
#endif
#ifndef KVM_QPDET_PROBE_MAX
#define KVM_QPDET_PROBE_MAX 6			// (T) at most this many in the life of the encoder (events that are not counted use one up)
#endif
#ifndef KVM_QPDET_PROBE_STEP
#define KVM_QPDET_PROBE_STEP 4			// (T) QP steps of a probe (up; down at the ceiling)
#endif
#ifndef KVM_QPDET_PROBE_FRAMES
#define KVM_QPDET_PROBE_FRAMES 5		// (T) frames at the stepped QP: one skipped, KVM_QPDET_POST measured, one spare
#endif

#define KVM_QPDET_UNKNOWN 0
#define KVM_QPDET_EFFECTIVE 1
#define KVM_QPDET_INEFFECTIVE 2

typedef struct kvm_qpdet
{
	uint8_t verdict;
	uint8_t nHist;					// P frames in hist (at the same asked QP as the last one, at most PRE)
	int curQp;						// asked QP of the last P frame, 0 = none yet
	double histLn[KVM_QPDET_PRE], histArea[KVM_QPDET_PRE], histBytes[KVM_QPDET_PRE];
	int histRep[KVM_QPDET_PRE];
	// an event under way
	uint8_t inEvent, nPost, skipLeft;
	int evFrom, evTo;
	double evPreLn, evPostLn, evPreArea, evPostArea, evPreBytes;
	int evPreRep, evPostRep;
	uint8_t evRepKnown;
	// the test
	double llr;
	uint32_t events, rejected, pFrames, sinceProbe, lastEventAt, repOk, repBad;
	uint8_t probes, probeLeft;
	int8_t probeDir;
	uint8_t noProbes;				// no deliberate steps (kvm_qpdet_set_probes); kept by kvm_qpdet_reset
	double lnStep;					// ln of the size of a P frame per QP step that is expected (0 = the 0.89 of the software encoder); kept by kvm_qpdet_reset (kvm_qpdet_set_step_pct)
} kvm_qpdet;

void kvm_qpdet_init(kvm_qpdet *d);
void kvm_qpdet_reset(kvm_qpdet *d);			// a new encoder or another way of setting the QP: start again (the setting of kvm_qpdet_set_probes stays)
// probes on (the default) or off. With the probes off the detector only watches the steps that the regulator makes by itself and never asks for a deliberate one (the hardware encoder: a step of +4 QP for
// 5 frames now and then is a visible price, and the self test has proved the QP on a synthetic picture; a verdict INEFFECTIVE from the natural events still means: back to the software encoder).
void kvm_qpdet_set_probes(kvm_qpdet *d, int on);
// the size that a P frame has one QP step higher, as a percent of the size before (the software encoder: 89, the default; the AMD encoder 92: 7-9 % per step). The expected effect of a step is what the
// test compares the observed change with: with the wrong one an encoder that listens, but less, would be taken for a deaf one. 50 .. 99; anything else leaves the default. Kept by kvm_qpdet_reset.
void kvm_qpdet_set_step_pct(kvm_qpdet *d, int pct);
// One frame. area: the tiles the frame covers (>= 1; the size is judged per tile). qpAsked: the QP given to the encoder for this frame; qpReported: the QP it says it used (-1 = not known); isIdr frames are ignored. Returns the verdict (KVM_QPDET_*).
int kvm_qpdet_frame(kvm_qpdet *d, int qpAsked, int qpReported, uint32_t bytes, int isIdr, uint32_t area);
// The deliberate step of a probe, to be added to the QP for the next P frame (0 = none). minQp/maxQp: the range the QP may be in (the probe goes down at the ceiling).
int kvm_qpdet_probe_offset(const kvm_qpdet *d, int qp, int minQp, int maxQp);
// ln of a positive integer without libm (accurate to 1e-4), for the tests and the detector
double kvm_qpdet_ln(uint32_t x);

#ifdef __cplusplus
}
#endif

#endif // KVM_QPDET_H
