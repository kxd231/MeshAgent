/*
KVM (Windows): the checks of the self test of the hardware H.264 encoder. Portable C (the encoder is a kvm_encbe table, the clock is passed in), so that the checks themselves are tested in a container with a
fake encoder that breaks one thing at a time; kvm_hwtest.cpp is the process around them (arguments, the real encoder, the result on stdout).

What it proves, on pictures of the size of the screen (kvm_synth: text, scrolling, video), with the QP form that the encoder takes (VT_UI4 of AVEncVideoEncodeQP, FrameTypeQP, AVEncCommonQuality, in this order):
  open        the encoder opens, in at most 2 s, asks for ONE input at the start (depth 1), takes the QP.
  stream      every access unit passes the rules of the viewer (kvm_h264_rules: 15 rules, VUI only a warning), after kvm_annexb_normalize in the backend; the SPS is not above the level that is announced; its colour,
              when it says one, is not full range or another colour space (0 = reserved and 2 = unspecified say nothing); a key frame is under 1.5 MB.
  QP          the mean size of the P frames of scrolling picture at QP 26 is more than 1.4 times that at QP 40, the QP changed in the running stream (the size is what it says: the encoder does not report the QP).
  rate        an encoder that fails the QP rule (or takes no QP form) is tried once more with the bit rate: open with CBR (LowDelayVBR when the open refuses CBR), set_bitrate 8 / 4 / 1 Mb/s (at 1920x1088, scaled by the
              area) in place of the QP 26 / 30 / 40, the same rule (more than 1.4 times). It passes as form 8 (CBR) or 9 (LDVBR); a failure is told with the rule of this pass ("rate", or a rule of the stream).
  IDR         a key frame that is asked for (AVEncVideoForceKeyFrame) is the frame that it was asked for, not the next one.
  1:1         every input gives exactly one access unit in the same call (no error, no lost unit), in the order given.
  time        median of the P frames <= 25 ms, 95th percentile <= 60 ms, key frame <= 250 ms (the AMD laptop: 8 / 10 / 25 ms; a slow state 2.6 times that fits). Too slow is a verdict of its own (SLOW), not a failure:
              the machine may have been busy, so it is not held against the encoder for good.
The frames use QP of the encoder directly (not the QP of the regulator: kvm_vcore_hw_qp is the business of the video code).

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef KVM_HWTEST_CORE_H
#define KVM_HWTEST_CORE_H

#include <stdio.h>
#include "kvm_encthr.h"
#include "../common/kvm_hwpolicy.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef KVM_HWT_P50_US
#define KVM_HWT_P50_US 25000u
#endif
#ifndef KVM_HWT_P95_US
#define KVM_HWT_P95_US 60000u
#endif
#ifndef KVM_HWT_IDR_US
#define KVM_HWT_IDR_US 250000u
#endif
#ifndef KVM_HWT_OPEN_MS
#define KVM_HWT_OPEN_MS 2000
#endif
#ifndef KVM_HWT_QP_RATIO_X100
#define KVM_HWT_QP_RATIO_X100 140	// P(QP 26) / P(QP 40) has to be above this (the AMD laptop: 1.9, with the QP 20 / 40 of the first probe)
#endif
#define KVM_HWT_FRAMES 46

typedef struct kvm_hwt_cfg
{
	int codedW, codedH;				// the picture, a multiple of 16 (the agent: 32)
	int level;						// the level that the 242 will announce (the SPS may be lower, never higher)
	int maxLevel;					// the viewer's maxLevelIdc, 0 = 52
	int profile;					// MFH264_PROFILE_*
	int64_t luid;					// the adapter of the captured output, 0 = any
	int rev;						// KVM_HW_SELFTEST_REV, goes into the result
	const char *key;				// the key of the cache, goes into the result
	double (*nowMs)(void);			// a clock in milliseconds
	FILE *diag;						// where the sizes of all pictures of both passes go (stderr of the test process; NULL = nowhere). Never the result: that is stdout, and the parent's buffer is 2048 bytes
} kvm_hwt_cfg;

// Runs the checks on `be` (be->init() has been called on this thread). Fills *res (verdict PASS / FAIL / SLOW, the rule that failed, the QP form, the times) and returns the verdict.
int kvm_hwt_run(const kvm_encbe *be, const kvm_hwt_cfg *cfg, kvm_hwp_result *res);

// the hardware self test diagnostics, only when the key of the test starts with "diag" (a diagnostics run): the sizes of 35 pictures of scroll to cfg->diag in five variants of the setting of the GOP (G1 the order A with only 0x7FFFFFFF,
// G2 the order B (the codec API before the types) with only 0xFFFFFFFF, G3 B with 0x7FFFFFFF, G4 B in Quality mode with the QP 26 and 40, G5 A with MF_MT_MAX_KEYFRAME_SPACING), each with the line "gop ..." of the open. No verdict, no rules.
void kvm_hwt_diag(const kvm_encbe *be, const kvm_hwt_cfg *cfg);

#ifdef __cplusplus
}
#endif

#endif
