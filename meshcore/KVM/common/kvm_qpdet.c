/*
KVM: does the encoder listen to the quantizer? See kvm_qpdet.h. Portable C99, no operating system API, no libm.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#include "kvm_qpdet.h"
#include <string.h>

#define LN089 (-0.116533816)	// ln(0.89): the size of a P frame per QP step

double kvm_qpdet_ln(uint32_t x)
{
	double m, z, z2;
	int e = 0;
	if (x == 0) { x = 1; }
	m = (double)x;
	while (m >= 2.0) { m *= 0.5; e++; }
	if (m > 1.5) { m *= 0.5; e++; }
	z = (m - 1.0) / (m + 1.0); z2 = z * z;
	return (double)e * 0.69314718056 + 2.0 * z * (1.0 + z2 / 3.0 + z2 * z2 / 5.0 + z2 * z2 * z2 / 7.0 + z2 * z2 * z2 * z2 / 9.0);
}

void kvm_qpdet_init(kvm_qpdet *d) { memset(d, 0, sizeof(*d)); }

void kvm_qpdet_reset(kvm_qpdet *d) { const uint8_t np = d->noProbes; const double ls = d->lnStep; memset(d, 0, sizeof(*d)); d->noProbes = np; d->lnStep = ls; }

void kvm_qpdet_set_step_pct(kvm_qpdet *d, int pct)
{
	if (pct < 50 || pct > 99) { d->lnStep = 0.0; return; }
	d->lnStep = kvm_qpdet_ln((uint32_t)pct) - kvm_qpdet_ln(100u);
}

void kvm_qpdet_set_probes(kvm_qpdet *d, int on) { d->noProbes = on ? 0 : 1; if (!on) { d->probeLeft = 0; } }

static void end_event(kvm_qpdet *d)
{
	const double areaRatio = d->evPostArea / (d->evPreArea > 0.5 ? d->evPreArea : 0.5);
	const double sigma = (double)KVM_QPDET_SIGMA_X1000 / 1000.0, s2 = sigma * sigma;
	const int step = d->evTo - d->evFrom;
	const double e = (double)step * (d->lnStep != 0.0 ? d->lnStep : LN089);
	const double r = d->evPostLn - d->evPreLn;
	double l = (e * e - 2.0 * r * e) / (2.0 * s2);		// ln( p(r | no effect) / p(r | the expected effect) )
	d->inEvent = 0;
	// An event is only as good as the content it was measured on: not counted when the covered area changed by more than a quarter between the frames before and
	// after the step (a burst of motion in a probe), when the frames are tiny (a clock, a caret: the size is overhead, not picture), or when it follows the previous counted event too closely
	// (the same content). The size is per covered tile.
	if (areaRatio > 1.0 + (double)KVM_QPDET_AREA_TOL_PCT / 100.0 || areaRatio < 1.0 / (1.0 + (double)KVM_QPDET_AREA_TOL_PCT / 100.0) || d->evPreBytes < (double)KVM_QPDET_MIN_BYTES ||
		(d->events > 0 && d->pFrames - d->lastEventAt < (uint32_t)KVM_QPDET_EVENT_GAP)) { d->rejected++; return; }
	if (d->evRepKnown)
	{
		const double follow = (double)(d->evPostRep - d->evPreRep) / (double)step;
		// The QP that the encoder writes into its output is the better signal (qpsample on the Windows 11 VM and the Windows 10 VM: it follows the asked QP, 20 -> 20 / 40 -> 40, and the size follows it): an event in which
		// it follows is "the QP works" whatever the sizes did (the content decides those). One that sits above the asked QP before and after the step is the floor of the encoder (26 for a 20 on
		// Windows 11), not deafness: no evidence either way.
		const int floored = (d->evPostRep == d->evPreRep && d->evPreRep > d->evFrom && d->evPostRep > d->evTo) ? 1 : 0;
		if (follow >= 0.5) { l = -(double)KVM_QPDET_EVENT_MAX; d->repOk++; }
		else if (floored) { l = 0.0; }
		else if (follow < 0.25) { l += (double)KVM_QPDET_REPORT_WEIGHT; d->repBad++; }	// (one that does not follow is evidence)
	}
	if (l > (double)KVM_QPDET_EVENT_MAX) { l = (double)KVM_QPDET_EVENT_MAX; } else if (l < -(double)KVM_QPDET_EVENT_MAX) { l = -(double)KVM_QPDET_EVENT_MAX; }	// one event never decides alone
	d->llr += l;
	d->events++;
	d->lastEventAt = d->pFrames;
	if (d->verdict == KVM_QPDET_UNKNOWN && d->events >= KVM_QPDET_MIN_EVENTS)
	{
		if (d->llr >= (double)KVM_QPDET_LLR_LIMIT && !(d->repOk > 0 && d->repBad == 0)) { d->verdict = KVM_QPDET_INEFFECTIVE; }	// (never while the reported QP has followed and has never failed to)
		else if (d->llr <= -(double)KVM_QPDET_LLR_LIMIT) { d->verdict = KVM_QPDET_EFFECTIVE; }
	}
}

int kvm_qpdet_frame(kvm_qpdet *d, int qpAsked, int qpReported, uint32_t bytes, int isIdr, uint32_t area)
{
	const double ln = kvm_qpdet_ln(bytes) - kvm_qpdet_ln(area > 0 ? area : 1u);	// per covered tile
	const double fa = (double)(area > 0 ? area : 1u), fb = (double)bytes;
	int i, k;
	if (isIdr || qpAsked <= 0) { return d->verdict; }
	d->pFrames++; d->sinceProbe++;
	if (d->probeLeft > 0) { d->probeLeft--; }
	if (d->curQp != 0 && qpAsked != d->curQp)
	{
		const int step = qpAsked > d->curQp ? qpAsked - d->curQp : d->curQp - qpAsked;
		if (d->inEvent) { d->inEvent = 0; }			// moved again before the event was complete: no event
		if (step >= KVM_QPDET_MIN_STEP && d->nHist >= KVM_QPDET_PRE)
		{
			double sum = 0.0, sa = 0.0, sb = 0.0;
			int rs = 0, rk = 1;
			for (i = 0; i < KVM_QPDET_PRE; i++) { sum += d->histLn[i]; sa += d->histArea[i]; sb += d->histBytes[i]; rs += d->histRep[i]; if (d->histRep[i] < 0) { rk = 0; } }
			d->evPreArea = sa / (double)KVM_QPDET_PRE; d->evPreBytes = sb / (double)KVM_QPDET_PRE; d->evPostArea = 0.0;
			d->inEvent = 1; d->evFrom = d->curQp; d->evTo = qpAsked;
			d->evPreLn = sum / (double)KVM_QPDET_PRE; d->evPostLn = 0.0;
			d->evPreRep = rs / KVM_QPDET_PRE; d->evPostRep = 0; d->evRepKnown = (uint8_t)rk;
			d->nPost = 0;
			d->skipLeft = 2;							// this frame (the first at the new QP) and the next one are not measured: a change may reach the encoder one frame late
		}
		d->nHist = 0;
	}
	d->curQp = qpAsked;
	if (d->inEvent)
	{
		if (d->skipLeft > 0) { d->skipLeft--; }
		else
		{
			d->evPostLn += ln; d->evPostArea += fa; d->evPostRep += qpReported; if (qpReported < 0) { d->evRepKnown = 0; }
			if (++d->nPost >= KVM_QPDET_POST)
			{
				d->evPostLn /= (double)KVM_QPDET_POST; d->evPostArea /= (double)KVM_QPDET_POST; d->evPostRep /= KVM_QPDET_POST;
				end_event(d);
			}
		}
	}
	// the history of the frames at this QP
	if (d->nHist < KVM_QPDET_PRE) { d->histLn[d->nHist] = ln; d->histArea[d->nHist] = fa; d->histBytes[d->nHist] = fb; d->histRep[d->nHist] = qpReported; d->nHist++; }
	else
	{
		for (k = 0; k + 1 < KVM_QPDET_PRE; k++) { d->histLn[k] = d->histLn[k + 1]; d->histArea[k] = d->histArea[k + 1]; d->histBytes[k] = d->histBytes[k + 1]; d->histRep[k] = d->histRep[k + 1]; }
		d->histLn[KVM_QPDET_PRE - 1] = ln; d->histArea[KVM_QPDET_PRE - 1] = fa; d->histBytes[KVM_QPDET_PRE - 1] = fb; d->histRep[KVM_QPDET_PRE - 1] = qpReported;
	}
	// a probe: nothing decided, long enough, a quiet history at one QP
	if (!d->noProbes && d->verdict == KVM_QPDET_UNKNOWN && d->probeLeft == 0 && !d->inEvent && d->probes < KVM_QPDET_PROBE_MAX && d->nHist >= KVM_QPDET_PRE &&
		((d->probes == 0 && d->pFrames >= KVM_QPDET_PROBE_AFTER) || (d->probes > 0 && d->sinceProbe >= KVM_QPDET_PROBE_EVERY)))
	{
		d->probes++; d->probeLeft = KVM_QPDET_PROBE_FRAMES; d->sinceProbe = 0; d->probeDir = 0;
	}
	return d->verdict;
}

int kvm_qpdet_probe_offset(const kvm_qpdet *d, int qp, int minQp, int maxQp)
{
	if (d->noProbes || d->probeLeft == 0 || d->verdict != KVM_QPDET_UNKNOWN) { return 0; }
	if (qp + KVM_QPDET_PROBE_STEP <= maxQp) { return KVM_QPDET_PROBE_STEP; }
	if (qp - KVM_QPDET_PROBE_STEP >= minQp) { return -KVM_QPDET_PROBE_STEP; }
	return 0;
}
