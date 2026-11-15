/*
KVM: the hand-off of frames to the thread of the encoder. See kvm_encjob.h. Portable C99.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#include "kvm_encjob.h"
#include <string.h>

static int32_t dms(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

void kvm_ej_init(kvm_ej *e, int depth)
{
	memset(e, 0, sizeof(*e));
	e->depth = depth < 1 ? 1 : (depth > KVM_EJ_MAX ? KVM_EJ_MAX : depth);
}

int kvm_ej_free_slot(const kvm_ej *e)
{
	int i;
	for (i = 0; i < e->depth; i++) { if (e->slot[i].state == KVM_EJS_FREE) { return i; } }
	return -1;
}

int kvm_ej_submit(kvm_ej *e, const kvm_ejob *job, uint32_t now)
{
	const int i = kvm_ej_free_slot(e);
	if (i < 0) { return -1; }
	e->slot[i].job = *job;
	e->slot[i].job.viewMap = NULL;
	if (job->viewMode != 0)
	{
		const long n = (job->mbCols > 0 && job->mbRows > 0) ? (long)job->mbCols * (long)job->mbRows : 0l;
		if (job->viewMap != NULL && n > 0 && n <= (long)KVM_EJ_MB_MAX)
		{
			memcpy(e->slot[i].map, job->viewMap, (size_t)n);
			e->slot[i].job.viewMap = e->slot[i].map;
		}
		else if (!job->viewFull) { e->slot[i].job.viewMode = 0; }	// no map to go by: the picture as it is
	}
	memset(&e->slot[i].res, 0, sizeof(e->slot[i].res));
	e->slot[i].cancelled = 0;
	e->slot[i].order = e->nextOrder++;
	e->slot[i].startedAt = now;
	e->slot[i].state = KVM_EJS_SUBMITTED;
	e->submitted++;
	return i;
}

static int in_flight_state(int s) { return s == KVM_EJS_SUBMITTED || s == KVM_EJS_RUNNING || s == KVM_EJS_DONE; }

int kvm_ej_oldest(const kvm_ej *e)
{
	int i, best = -1;
	for (i = 0; i < e->depth; i++)
	{
		if (!in_flight_state(e->slot[i].state)) { continue; }
		if (best < 0 || (int32_t)(e->slot[i].order - e->slot[best].order) < 0) { best = i; }
	}
	return best;
}

int kvm_ej_inflight(const kvm_ej *e)
{
	int i, n = 0;
	for (i = 0; i < e->depth; i++) { if (in_flight_state(e->slot[i].state)) { n++; } }
	return n;
}

int kvm_ej_ready(const kvm_ej *e)
{
	const int i = kvm_ej_oldest(e);
	return i >= 0 && e->slot[i].state == KVM_EJS_DONE;
}

int kvm_ej_collect(kvm_ej *e, kvm_ejob *job, kvm_ejres *res, int *cancelled)
{
	const int i = kvm_ej_oldest(e);
	if (i < 0 || e->slot[i].state != KVM_EJS_DONE) { return -1; }
	if (job != NULL) { *job = e->slot[i].job; job->viewMap = NULL; }
	if (res != NULL) { *res = e->slot[i].res; }
	if (cancelled != NULL) { *cancelled = e->slot[i].cancelled; }
	e->slot[i].state = KVM_EJS_FREE;
	e->collected++;
	return i;
}

int kvm_ej_cancel_all(kvm_ej *e)
{
	int i, n = 0;
	for (i = 0; i < e->depth; i++)
	{
		if (in_flight_state(e->slot[i].state) && !e->slot[i].cancelled) { e->slot[i].cancelled = 1; n++; e->cancelledN++; }
	}
	return n;
}

int kvm_ej_abandon_all(kvm_ej *e)
{
	int i, n = 0;
	for (i = 0; i < e->depth; i++)
	{
		if (in_flight_state(e->slot[i].state)) { e->slot[i].state = KVM_EJS_ABANDONED; e->slot[i].cancelled = 1; n++; e->abandonedN++; }
	}
	return n;
}

uint32_t kvm_ej_oldest_age(const kvm_ej *e, uint32_t now)
{
	int i;
	int32_t best = 0;
	for (i = 0; i < e->depth; i++)
	{
		if (e->slot[i].state == KVM_EJS_SUBMITTED || e->slot[i].state == KVM_EJS_RUNNING)
		{
			const int32_t a = dms(now, e->slot[i].startedAt);
			if (a > best) { best = a; }
		}
	}
	return (uint32_t)best;
}

int kvm_ej_is_abandoned(const kvm_ej *e, int slot)
{
	return slot >= 0 && slot < e->depth && e->slot[slot].state == KVM_EJS_ABANDONED;
}

int kvm_ej_take(kvm_ej *e, kvm_ejob *job, int *cancelled)
{
	int i, best = -1;
	for (i = 0; i < e->depth; i++)
	{
		if (e->slot[i].state != KVM_EJS_SUBMITTED) { continue; }
		if (best < 0 || (int32_t)(e->slot[i].order - e->slot[best].order) < 0) { best = i; }
	}
	if (best < 0) { return -1; }
	e->slot[best].state = KVM_EJS_RUNNING;
	if (job != NULL) { *job = e->slot[best].job; }
	if (cancelled != NULL) { *cancelled = e->slot[best].cancelled; }
	return best;
}

void kvm_ej_finish(kvm_ej *e, int slot, const kvm_ejres *res)
{
	if (slot < 0 || slot >= e->depth) { return; }
	if (e->slot[slot].state != KVM_EJS_RUNNING) { return; }	// abandoned meanwhile (or a bad call): the result is dropped
	e->slot[slot].res = *res;
	e->slot[slot].state = KVM_EJS_DONE;
}
