/*
Tests of kvm_qpdet.c (does the encoder listen to the quantizer?): the logarithm without libm, the events, the test, the probe, the error rates over many seeds of pseudo-random noise.
Run with run-kvm-qpdet-tests.sh (gcc in a container: -Werror -pedantic, then ASan + UBSan).
*/

#include "../kvm_qpdet.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gFail = 0, gChecks = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); gChecks++; if (_a != _b) { gFail++; printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

static uint32_t gRng = 1u;
static double rnd01(void) { gRng = (gRng * 1664525u) + 1013904223u; return (double)(gRng >> 8) / 16777216.0; }

static void test_ln(void)
{
	uint32_t x;
	double worst = 0.0;
	for (x = 1; x < 4000000000u; x = x < 100000u ? x + 1u : x + x / 7u + 13u)
	{
		const double d = fabs(kvm_qpdet_ln(x) - log((double)x));
		if (d > worst) { worst = d; }
	}
	CHECK(worst < 2e-4);
	CHECK(fabs(kvm_qpdet_ln(0) - 0.0) < 1e-9);
}

// an encoder: effective = the size follows 0.89 per QP step; the content jitters by +-jit; reported = the QP (or stuck / unknown)
typedef struct enc_model { int effective; double base, jit; int repMode; /* 0 = -1 (unknown), 1 = follows, 2 = stuck at 26 */ } enc_model;
static double gRatio = 0.0;	/* the size per QP step of the model of the encoder (0 = 0.89) */

static uint32_t enc_bytes(const enc_model *m, int qp)
{
	const double b = m->base * (m->effective ? pow(gRatio > 0.0 ? gRatio : 0.89, (double)(qp - 26)) : 1.0) * (1.0 + m->jit * (2.0 * rnd01() - 1.0));
	return (uint32_t)b;
}
static int enc_rep(const enc_model *m, int qp) { return m->repMode == 0 ? -1 : (m->repMode == 1 ? qp : 26); }

// The regulator moves the QP by `step` every `every` frames (up and down alternately), plus the probe of the detector. Returns the frame number of the verdict (0 = none in n frames).
static int run(kvm_qpdet *d, const enc_model *m, int n, int every, int step)
{
	int f, qp = 30, dir = 1;
	for (f = 1; f <= n; f++)
	{
		int asked;
		if (every > 0 && f % every == 0) { qp += dir * step; dir = -dir; }
		asked = qp + kvm_qpdet_probe_offset(d, qp, 26, 38);
		if (kvm_qpdet_frame(d, asked, enc_rep(m, asked), enc_bytes(m, asked), 0, 100) != KVM_QPDET_UNKNOWN) { return f; }
	}
	return 0;
}

static void test_basic(void)
{
	kvm_qpdet d;
	enc_model eff = { 1, 8000.0, 0.15, 0 }, dead = { 0, 6000.0, 0.15, 0 };
	int f;
	// the regulator moves the QP now and then by 2: an encoder that listens is recognised, one that does not as well
	gRng = 5; kvm_qpdet_init(&d); f = run(&d, &eff, 400, 12, 2); CHECK(f > 0); CHECK_EQ(d.verdict, KVM_QPDET_EFFECTIVE);
	gRng = 5; kvm_qpdet_init(&d); f = run(&d, &dead, 400, 12, 2); CHECK(f > 0); CHECK_EQ(d.verdict, KVM_QPDET_INEFFECTIVE);
	// the regulator does not move the QP at all: the probe decides, after PROBE_AFTER frames, within a few frames
	gRng = 6; kvm_qpdet_init(&d); f = run(&d, &dead, 400, 0, 0); CHECK(f >= KVM_QPDET_PROBE_AFTER + KVM_QPDET_PROBE_EVERY && f <= KVM_QPDET_PROBE_AFTER + KVM_QPDET_PROBE_EVERY + 12); CHECK_EQ(d.verdict, KVM_QPDET_INEFFECTIVE); CHECK_EQ(d.probes, 2);
	gRng = 6; kvm_qpdet_init(&d); f = run(&d, &eff, 400, 0, 0); CHECK(f >= KVM_QPDET_PROBE_AFTER + KVM_QPDET_PROBE_EVERY && f <= KVM_QPDET_PROBE_AFTER + KVM_QPDET_PROBE_EVERY + 12); CHECK_EQ(d.verdict, KVM_QPDET_EFFECTIVE);
	// before PROBE_AFTER frames nothing happens, and the QP is not touched
	kvm_qpdet_init(&d);
	{
		int i;
		for (i = 0; i < KVM_QPDET_PROBE_AFTER - 2; i++) { CHECK_EQ(kvm_qpdet_probe_offset(&d, 30, 26, 38), 0); (void)kvm_qpdet_frame(&d, 30, -1, 7000, 0, 100); }
		CHECK_EQ(d.verdict, KVM_QPDET_UNKNOWN); CHECK_EQ(d.probes, 0);
	}
	// the probe goes up, at the ceiling down, never out of the range, and ends by itself
	kvm_qpdet_init(&d); d.probeLeft = 3;
	CHECK_EQ(kvm_qpdet_probe_offset(&d, 30, 26, 38), KVM_QPDET_PROBE_STEP);
	CHECK_EQ(kvm_qpdet_probe_offset(&d, 37, 26, 38), -KVM_QPDET_PROBE_STEP);
	CHECK_EQ(kvm_qpdet_probe_offset(&d, 30, 30, 32), 0);
	d.probeLeft = 0; CHECK_EQ(kvm_qpdet_probe_offset(&d, 30, 26, 38), 0);
	d.probeLeft = 3; d.verdict = KVM_QPDET_EFFECTIVE; CHECK_EQ(kvm_qpdet_probe_offset(&d, 30, 26, 38), 0);	// decided: no more steps
	// IDR frames are ignored (neither counted nor in the history)
	kvm_qpdet_init(&d); (void)kvm_qpdet_frame(&d, 30, 30, 200000, 1, 100); CHECK_EQ(d.pFrames, 0); CHECK_EQ(d.nHist, 0);
	(void)kvm_qpdet_frame(&d, 0, 0, 5000, 0, 100); CHECK_EQ(d.pFrames, 0);
	// reset starts again
	gRng = 5; kvm_qpdet_init(&d); (void)run(&d, &dead, 400, 12, 2); CHECK_EQ(d.verdict, KVM_QPDET_INEFFECTIVE);
	kvm_qpdet_reset(&d); CHECK_EQ(d.verdict, KVM_QPDET_UNKNOWN); CHECK_EQ(d.events, 0); CHECK_EQ(d.probes, 0);
	// a change that comes again before the event is complete is no event; a change of 1 is no event
	kvm_qpdet_init(&d);
	{
		int i;
		for (i = 0; i < 4; i++) { (void)kvm_qpdet_frame(&d, 30, -1, 7000, 0, 100); }
		(void)kvm_qpdet_frame(&d, 32, -1, 6000, 0, 100); (void)kvm_qpdet_frame(&d, 34, -1, 6000, 0, 100);
		CHECK_EQ(d.events, 0);
		kvm_qpdet_init(&d);
		for (i = 0; i < 6; i++) { (void)kvm_qpdet_frame(&d, 30, -1, 7000, 0, 100); }
		for (i = 0; i < 8; i++) { (void)kvm_qpdet_frame(&d, 31, -1, 7000, 0, 100); }
		CHECK_EQ(d.events, 0);
	}
}

// the timing of an event and of the probes: scripted sizes, no noise
static void test_scripted(void)
{
	kvm_qpdet d;
	int i;
	// an event: 3 frames at 30, then 32: the first two frames at the new QP are not measured, the next three are; the verdict (the size did not change) needs more than one event only if the step is small
	kvm_qpdet_init(&d);
	for (i = 0; i < 3; i++) { (void)kvm_qpdet_frame(&d, 30, -1, 7000, 0, 100); }
	for (i = 0; i < 4; i++) { (void)kvm_qpdet_frame(&d, 32, -1, 7000, 0, 100); CHECK_EQ(d.events, 0); }	// frames 1-4 at the new QP: skipped, skipped, measured, measured
	(void)kvm_qpdet_frame(&d, 32, -1, 7000, 0, 100); CHECK_EQ(d.events, 1);									// the third measured frame completes it
	// one event never decides, whatever the step (a verdict needs two agreeing events)
	CHECK_EQ(d.verdict, KVM_QPDET_UNKNOWN);
	kvm_qpdet_init(&d);
	for (i = 0; i < 3; i++) { (void)kvm_qpdet_frame(&d, 30, -1, 7000, 0, 100); }
	for (i = 0; i < 5; i++) { (void)kvm_qpdet_frame(&d, 34, -1, 7000, 0, 100); }
	CHECK_EQ(d.events, 1); CHECK_EQ(d.verdict, KVM_QPDET_UNKNOWN);
	// ... the second, on another stretch of the content and the other way (34 -> 30), with the size still not following: ineffective
	for (i = 0; i < KVM_QPDET_EVENT_GAP; i++) { (void)kvm_qpdet_frame(&d, 34, -1, 7000, 0, 100); }
	for (i = 0; i < 5; i++) { (void)kvm_qpdet_frame(&d, 30, -1, 7000, 0, 100); }
	CHECK_EQ(d.events, 2); CHECK_EQ(d.verdict, KVM_QPDET_INEFFECTIVE);
	// the size follows 0.89 per step: 30 -> 34 gives 0.627 x the bytes, and 34 -> 30 gives them back: effective
	kvm_qpdet_init(&d);
	for (i = 0; i < 3; i++) { (void)kvm_qpdet_frame(&d, 30, -1, 7000, 0, 100); }
	for (i = 0; i < 5; i++) { (void)kvm_qpdet_frame(&d, 34, -1, 4390, 0, 100); }
	CHECK_EQ(d.verdict, KVM_QPDET_UNKNOWN);
	for (i = 0; i < KVM_QPDET_EVENT_GAP; i++) { (void)kvm_qpdet_frame(&d, 34, -1, 4390, 0, 100); }
	for (i = 0; i < 5; i++) { (void)kvm_qpdet_frame(&d, 30, -1, 7000, 0, 100); }
	CHECK_EQ(d.verdict, KVM_QPDET_EFFECTIVE);
	// the second event too soon after the first (the same content) is not counted
	kvm_qpdet_init(&d);
	for (i = 0; i < 3; i++) { (void)kvm_qpdet_frame(&d, 30, -1, 7000, 0, 100); }
	for (i = 0; i < 5; i++) { (void)kvm_qpdet_frame(&d, 34, -1, 7000, 0, 100); }
	for (i = 0; i < 3; i++) { (void)kvm_qpdet_frame(&d, 34, -1, 7000, 0, 100); }
	for (i = 0; i < 5; i++) { (void)kvm_qpdet_frame(&d, 30, -1, 7000, 0, 100); }
	CHECK_EQ(d.events, 1); CHECK_EQ(d.rejected, 1); CHECK_EQ(d.verdict, KVM_QPDET_UNKNOWN);
	// the covered area changes by more than a quarter across the step (a burst of motion): not counted, whatever the sizes say
	kvm_qpdet_init(&d);
	for (i = 0; i < 3; i++) { (void)kvm_qpdet_frame(&d, 30, -1, 7000, 0, 100); }
	for (i = 0; i < 5; i++) { (void)kvm_qpdet_frame(&d, 34, -1, 7000, 0, 300); }
	CHECK_EQ(d.events, 0); CHECK_EQ(d.rejected, 1);
	kvm_qpdet_init(&d);
	for (i = 0; i < 3; i++) { (void)kvm_qpdet_frame(&d, 30, -1, 7000, 0, 300); }
	for (i = 0; i < 5; i++) { (void)kvm_qpdet_frame(&d, 34, -1, 7000, 0, 100); }
	CHECK_EQ(d.events, 0); CHECK_EQ(d.rejected, 1);
	// ... a change of 20 % is within the tolerance
	kvm_qpdet_init(&d);
	for (i = 0; i < 3; i++) { (void)kvm_qpdet_frame(&d, 30, -1, 7000, 0, 100); }
	for (i = 0; i < 5; i++) { (void)kvm_qpdet_frame(&d, 34, -1, 7000, 0, 120); }
	CHECK_EQ(d.events, 1); CHECK_EQ(d.rejected, 0);
	// tiny frames (a caret, a clock) say nothing about the QP
	kvm_qpdet_init(&d);
	for (i = 0; i < 3; i++) { (void)kvm_qpdet_frame(&d, 30, -1, 1000, 0, 100); }
	for (i = 0; i < 5; i++) { (void)kvm_qpdet_frame(&d, 34, -1, 1000, 0, 100); }
	CHECK_EQ(d.events, 0); CHECK_EQ(d.rejected, 1);
	// the size is judged per covered tile: the same bytes on twice the area are half the size per tile, that is "the QP works" if it was asked for 4 steps up... (area within the tolerance: 1.2 x)
	kvm_qpdet_init(&d);
	for (i = 0; i < 3; i++) { (void)kvm_qpdet_frame(&d, 30, -1, 7000, 0, 100); }
	for (i = 0; i < 5; i++) { (void)kvm_qpdet_frame(&d, 34, -1, 7000, 0, 125); }	// 1.25 x the area, the same bytes: per tile 0.8 x
	CHECK_EQ(d.events, 1);
	CHECK(d.llr < 1.0 && d.llr > -1.0);											// per tile the size fell by 20 %: about half of the expected 37 %, no evidence (unnormalised it would be +3)
	// the QP moves again before the event is complete: nothing is concluded from it
	kvm_qpdet_init(&d);
	for (i = 0; i < 3; i++) { (void)kvm_qpdet_frame(&d, 30, -1, 7000, 0, 100); }
	(void)kvm_qpdet_frame(&d, 34, -1, 7000, 0, 100); (void)kvm_qpdet_frame(&d, 34, -1, 7000, 0, 100); (void)kvm_qpdet_frame(&d, 34, -1, 7000, 0, 100);
	(void)kvm_qpdet_frame(&d, 38, -1, 7000, 0, 100); (void)kvm_qpdet_frame(&d, 38, -1, 7000, 0, 100);
	CHECK_EQ(d.events, 0); CHECK_EQ(d.verdict, KVM_QPDET_UNKNOWN);
	// a change of the QP before KVM_QPDET_PRE frames at the old one is no event (nothing to compare with)
	kvm_qpdet_init(&d);
	for (i = 0; i < KVM_QPDET_PRE - 1; i++) { (void)kvm_qpdet_frame(&d, 30, -1, 7000, 0, 100); }
	for (i = 0; i < 8; i++) { (void)kvm_qpdet_frame(&d, 34, -1, 7000, 0, 100); }
	CHECK_EQ(d.events, 0); CHECK_EQ(d.rejected, 0); CHECK_EQ(d.verdict, KVM_QPDET_UNKNOWN);
	// the probes: an encoder whose sizes change by half of what they should (the evidence is a draw): no verdict; the probes are KVM_QPDET_PROBE_AFTER frames from the start and
	// KVM_QPDET_PROBE_EVERY apart, at most KVM_QPDET_PROBE_MAX of them
	kvm_qpdet_init(&d);
	{
		int f, qp = 30, probesAt[8], np = 0;
		for (f = 1; f <= 2000; f++)
		{
			const int asked = qp + kvm_qpdet_probe_offset(&d, qp, 26, 38);
			const uint32_t before = d.probes;
			(void)kvm_qpdet_frame(&d, asked, -1, (uint32_t)(7000.0 * pow(0.943, (double)(asked - 30))), 0, 100);	// half the slope of 0.89
			if (d.probes != before && np < 8) { probesAt[np++] = f; }
		}
		CHECK_EQ(d.verdict, KVM_QPDET_UNKNOWN);
		CHECK_EQ(d.probes, KVM_QPDET_PROBE_MAX);
		CHECK_EQ(probesAt[0], KVM_QPDET_PROBE_AFTER);
		CHECK_EQ(probesAt[1] - probesAt[0], KVM_QPDET_PROBE_EVERY);									// (counted from the start of the previous probe)
		CHECK(np == KVM_QPDET_PROBE_MAX);
	}
}

// the case of a measurement: a Windows 10 like encoder (the size follows the QP), one probe happens during a burst of the content (the post frames are 2.2 x bigger with the area unchanged,
// so the step looks like "no effect" or even the opposite): the verdict must not be "ineffective" on that one probe; the clean probes that follow decide "effective"
static void test_burst_during_probe(void)
{
	int seed;
	int wrong = 0, decided = 0;
	for (seed = 1; seed <= 100; seed++)
	{
		kvm_qpdet d;
		enc_model eff = { 1, 8000.0, 0.15, 0 };
		int f, qp = 30;
		gRng = (uint32_t)seed * 2654435761u;
		kvm_qpdet_init(&d);
		for (f = 1; f <= 800 && d.verdict == KVM_QPDET_UNKNOWN; f++)
		{
			const int asked = qp + kvm_qpdet_probe_offset(&d, qp, 26, 38);
			double b = (double)enc_bytes(&eff, asked);
			if (f >= 43 && f <= 50) { b *= 2.2; }								// the burst: the first probe, the frames that are measured
			(void)kvm_qpdet_frame(&d, asked, -1, (uint32_t)b, 0, 100);
		}
		if (d.verdict == KVM_QPDET_INEFFECTIVE) { wrong++; }
		if (d.verdict == KVM_QPDET_EFFECTIVE) { decided++; }
	}
	CHECK_EQ(wrong, 0);
	CHECK(decided >= 95);
	// the opposite: an encoder that does not listen, a burst in the first probe that happens to look like an effect: nothing is decided on it, the later probes still find the truth
	{
		int wrongEff = 0;
		for (seed = 1; seed <= 100; seed++)
		{
			kvm_qpdet d;
			enc_model dead = { 0, 6000.0, 0.15, 0 };
			int f, qp = 30;
			gRng = (uint32_t)seed * 40503u + 11u;
			kvm_qpdet_init(&d);
			for (f = 1; f <= 1500 && d.verdict == KVM_QPDET_UNKNOWN; f++)
			{
				const int asked = qp + kvm_qpdet_probe_offset(&d, qp, 26, 38);
				double b = (double)enc_bytes(&dead, asked);
				if (f >= 43 && f <= 50) { b *= 0.55; }
				(void)kvm_qpdet_frame(&d, asked, -1, (uint32_t)b, 0, 100);
			}
			if (d.verdict == KVM_QPDET_EFFECTIVE) { wrongEff++; }
		}
		CHECK_EQ(wrongEff, 0);
	}
}

// the reported QP is preferred over the sizes (qpsample of the Windows 11 VM: the QP follows the asked one)
static void test_report_preferred(void)
{
	kvm_qpdet d;
	int i, j;
	// the sizes do not change (content), the reported QP follows: effective, never ineffective, whatever the sizes
	kvm_qpdet_init(&d);
	for (j = 0; j < 4; j++)
	{
		const int from = j % 2 ? 34 : 30, to = j % 2 ? 30 : 34;
		for (i = 0; i < 25; i++) { (void)kvm_qpdet_frame(&d, from, from, 7000, 0, 100); }
		for (i = 0; i < 5; i++) { (void)kvm_qpdet_frame(&d, to, to, 7000, 0, 100); }
	}
	CHECK_EQ(d.verdict, KVM_QPDET_EFFECTIVE); CHECK(d.repOk >= 2); CHECK_EQ(d.repBad, 0);
	// ... even when one event has sizes that look like deafness and the verdict is not reached yet: the guard
	kvm_qpdet_init(&d); d.llr = 5.0; d.events = 1; d.repOk = 1;
	for (i = 0; i < 25; i++) { (void)kvm_qpdet_frame(&d, 30, -1, 7000, 0, 100); }
	for (i = 0; i < 5; i++) { (void)kvm_qpdet_frame(&d, 34, -1, 7000, 0, 100); }
	CHECK(d.llr >= 4.0); CHECK_EQ(d.verdict, KVM_QPDET_UNKNOWN);
	// the encoder floor: asked 20 -> 24, reported 26 -> 26 (above the asked QP before and after the step): no evidence against it
	kvm_qpdet_init(&d);
	for (i = 0; i < 4; i++) { (void)kvm_qpdet_frame(&d, 20, 26, 7000, 0, 100); }
	for (i = 0; i < 5; i++) { (void)kvm_qpdet_frame(&d, 24, 26, 7000, 0, 100); }
	CHECK_EQ(d.events, 1); CHECK_EQ(d.repBad, 0); CHECK_EQ(d.repOk, 0);
	// a reported QP that sits BELOW the asked one and does not move is not a floor: the evidence of deafness
	kvm_qpdet_init(&d);
	for (i = 0; i < 4; i++) { (void)kvm_qpdet_frame(&d, 30, 26, 7000, 0, 100); }
	for (i = 0; i < 5; i++) { (void)kvm_qpdet_frame(&d, 34, 26, 7000, 0, 100); }
	CHECK_EQ(d.events, 1); CHECK_EQ(d.repBad, 1);
}

// the reported QP: an encoder that says it used the QP it was asked for but whose frames do not change is decided by the sizes; one whose reported QP is stuck is decided at once
static void test_reported(void)
{
	kvm_qpdet d;
	enc_model stuck = { 1, 8000.0, 0.15, 2 }, liar = { 0, 6000.0, 0.15, 1 }, honest = { 1, 8000.0, 0.15, 1 };
	int f;
	gRng = 9; kvm_qpdet_init(&d); f = run(&d, &stuck, 200, 10, 2); CHECK(f > 0); CHECK_EQ(d.verdict, KVM_QPDET_INEFFECTIVE);		// the sizes follow, the reported QP does not: the weight of 3 beats the size
	CHECK(d.events <= 20);
	gRng = 9; kvm_qpdet_init(&d); f = run(&d, &honest, 200, 10, 2); CHECK(f > 0); CHECK_EQ(d.verdict, KVM_QPDET_EFFECTIVE); CHECK(d.events <= 4);
	gRng = 9; kvm_qpdet_init(&d); f = run(&d, &liar, 400, 10, 2); CHECK(f > 0); CHECK_EQ(d.verdict, KVM_QPDET_EFFECTIVE);				// reports the asked QP, sizes do not change: the reported QP is the better signal (qpsample on the Windows 11 VM and the Windows 10 VM: it follows, and the size follows it)
	CHECK_EQ(d.repBad, 0);
}

// error rates over many seeds: an encoder that listens is never taken for deaf and vice versa (noise +-25 %, more than the +-15 % of the measurement)
static void test_rates(void)
{
	int seed, wrongEff = 0, wrongDead = 0, undecided = 0, nEff = 0, nDead = 0;
	long sumF = 0;
	for (seed = 1; seed <= 400; seed++)
	{
		kvm_qpdet d;
		enc_model eff = { 1, 8000.0, 0.25, 0 }, dead = { 0, 6000.0, 0.25, 0 };
		int f;
		gRng = (uint32_t)seed * 2654435761u; kvm_qpdet_init(&d); f = run(&d, &eff, 300, seed % 2 ? 0 : 15, 2); nEff++;
		if (d.verdict == KVM_QPDET_INEFFECTIVE) { wrongEff++; } else if (d.verdict == KVM_QPDET_UNKNOWN) { undecided++; } else { sumF += f; }
		gRng = (uint32_t)seed * 2654435761u + 7u; kvm_qpdet_init(&d); f = run(&d, &dead, 300, seed % 2 ? 0 : 15, 2); nDead++;
		if (d.verdict == KVM_QPDET_EFFECTIVE) { wrongDead++; } else if (d.verdict == KVM_QPDET_UNKNOWN) { undecided++; }
	}
	printf("qpdet: %d encoders that listen, %d that do not (noise +-25 %%): taken for the other kind %d / %d, undecided %d, mean frames to a verdict (listening) %.1f\n", nEff, nDead, wrongEff, wrongDead, undecided, (double)sumF / (double)(nEff - wrongEff > 0 ? nEff - wrongEff : 1));
	CHECK(wrongEff <= nEff / 50);
	CHECK(wrongDead <= nDead / 50);
	CHECK(undecided <= (nEff + nDead) / 50);
}

// the probes can be switched off (the hardware encoder); the natural events still count, reset keeps the setting
static void test_no_probes(void)
{
	kvm_qpdet d;
	enc_model eff = { 1, 8000.0, 0.15, 0 }, dead = { 0, 6000.0, 0.15, 0 };
	int f, i;
	gRng = 6; kvm_qpdet_init(&d); kvm_qpdet_set_probes(&d, 0);
	f = run(&d, &dead, 400, 0, 0);
	CHECK_EQ(f, 0); CHECK_EQ(d.verdict, KVM_QPDET_UNKNOWN); CHECK_EQ(d.probes, 0); CHECK_EQ(d.probeLeft, 0);
	gRng = 6; kvm_qpdet_init(&d); kvm_qpdet_set_probes(&d, 0);
	f = run(&d, &eff, 400, 0, 0);
	CHECK_EQ(f, 0); CHECK_EQ(d.verdict, KVM_QPDET_UNKNOWN); CHECK_EQ(d.probes, 0);
	// the offset is zero even while a probe is "armed"
	kvm_qpdet_init(&d); kvm_qpdet_set_probes(&d, 0); d.probeLeft = 3;
	CHECK_EQ(kvm_qpdet_probe_offset(&d, 30, 26, 38), 0);
	// ... and switching the probes off stops one that is under way
	kvm_qpdet_init(&d); d.probeLeft = 3; CHECK_EQ(kvm_qpdet_probe_offset(&d, 30, 26, 38), KVM_QPDET_PROBE_STEP); kvm_qpdet_set_probes(&d, 0); CHECK_EQ(d.probeLeft, 0);
	// the steps of the regulator are watched as before: a dead encoder is found, an encoder that listens is not condemned (the verdict EFFECTIVE is not set by the switch)
	gRng = 5; kvm_qpdet_init(&d); kvm_qpdet_set_probes(&d, 0); f = run(&d, &dead, 400, 12, 2); CHECK(f > 0); CHECK_EQ(d.verdict, KVM_QPDET_INEFFECTIVE);
	gRng = 5; kvm_qpdet_init(&d); kvm_qpdet_set_probes(&d, 0); f = run(&d, &eff, 400, 12, 2); CHECK(f > 0); CHECK_EQ(d.verdict, KVM_QPDET_EFFECTIVE);
	// reset keeps it, init does not, and it can be switched on again
	kvm_qpdet_reset(&d); CHECK_EQ(d.noProbes, 1); CHECK_EQ(d.verdict, KVM_QPDET_UNKNOWN);
	for (i = 0; i < KVM_QPDET_PROBE_AFTER + 5; i++) { (void)kvm_qpdet_frame(&d, 30, -1, 7000, 0, 100); }
	CHECK_EQ(d.probes, 0);
	kvm_qpdet_set_probes(&d, 1); CHECK_EQ(d.noProbes, 0);
	for (i = 0; i < KVM_QPDET_PROBE_AFTER + 5; i++) { (void)kvm_qpdet_frame(&d, 30, -1, 7000, 0, 100); }
	CHECK(d.probes >= 1);
	kvm_qpdet_init(&d); CHECK_EQ(d.noProbes, 0);
}

// the AMD encoder gives 7-9 % per QP step, not 11 %: the expected step is a setting of the detector; an encoder that listens (0.92) is not condemned with it, a deaf one still is, and the setting survives a reset
static void test_hw_slope(void)
{
	kvm_qpdet d;
	enc_model eff = { 1, 8000.0, 0.15, 0 }, dead = { 0, 6000.0, 0.15, 0 };
	int seed, nEff = 0, nBad = 0, nDeadOk = 0, nDefaultBad = 0, f;
	const int N = 300;
	gRatio = 0.92;
	for (seed = 1; seed <= N; seed++)
	{
		gRng = (uint32_t)seed * 2654435761u + 11u; kvm_qpdet_init(&d); kvm_qpdet_set_probes(&d, 0); kvm_qpdet_set_step_pct(&d, 92);
		f = run(&d, &eff, 400, 12, 2); (void)f;
		if (d.verdict == KVM_QPDET_EFFECTIVE) { nEff++; } else if (d.verdict == KVM_QPDET_INEFFECTIVE) { nBad++; }
		gRng = (uint32_t)seed * 2654435761u + 11u; kvm_qpdet_init(&d); kvm_qpdet_set_probes(&d, 0); kvm_qpdet_set_step_pct(&d, 92);
		f = run(&d, &dead, 400, 12, 2);
		if (d.verdict == KVM_QPDET_INEFFECTIVE) { nDeadOk++; }
		gRng = (uint32_t)seed * 2654435761u + 11u; kvm_qpdet_init(&d); kvm_qpdet_set_probes(&d, 0);
		f = run(&d, &eff, 400, 12, 2);
		if (d.verdict == KVM_QPDET_INEFFECTIVE) { nDefaultBad++; }
	}
	printf("qpdet hw slope 0.92 (%d seeds): effective found %d, taken for deaf %d, deaf found %d; with the software slope the encoder that listens is taken for deaf %d times\n", N, nEff, nBad, nDeadOk, nDefaultBad);
	CHECK(nBad == 0); CHECK(nEff * 100 >= N * 85); CHECK(nDeadOk * 100 >= N * 85);
	gRatio = 0.0;
	kvm_qpdet_init(&d); kvm_qpdet_set_step_pct(&d, 92); kvm_qpdet_set_probes(&d, 0);
	kvm_qpdet_reset(&d); CHECK(d.lnStep < -0.08 && d.lnStep > -0.0835 && d.noProbes == 1);
	kvm_qpdet_set_step_pct(&d, 0); CHECK(d.lnStep == 0.0); kvm_qpdet_set_step_pct(&d, 100); CHECK(d.lnStep == 0.0); kvm_qpdet_set_step_pct(&d, 49); CHECK(d.lnStep == 0.0);
	kvm_qpdet_init(&d); CHECK(d.lnStep == 0.0);
}

int main(void)
{
	test_ln();
	test_basic();
	test_scripted();
	test_burst_during_probe();
	test_reported();
	test_report_preferred();
	test_rates();
	test_no_probes();
	test_hw_slope();
	printf("%d checks, %d failed\n", gChecks, gFail);
	return gFail == 0 ? 0 : 1;
}
