// Tests of kvm_encjob.c: the slots of the jobs between the main loop and the thread of the encoder. The "thread" calls (take, finish) are made by the test, so every
// interleaving is deterministic. A model written without the module (a queue of jobs with their flags) is run against it with random calls.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kvm_encjob.h"

static int gFail = 0, gChecks = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); gChecks++; if (_a != _b) { gFail++; printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

static kvm_ejob mkjob(uint32_t gen) { kvm_ejob j; memset(&j, 0, sizeof(j)); j.gen = gen; return j; }
static kvm_ejres mkres(int code, size_t len) { kvm_ejres r; memset(&r, 0, sizeof(r)); r.code = code; r.len = len; return r; }

static void test_basic(void)
{
	kvm_ej e;
	kvm_ej_init(&e, 9); CHECK_EQ(e.depth, KVM_EJ_MAX);
	kvm_ej_init(&e, 0); CHECK_EQ(e.depth, 1);
	kvm_ejob j = mkjob(7), jt, jc;
	kvm_ejres r = mkres(0, 100), rc;
	int s, c;
	kvm_ej_init(&e, 1);
	CHECK_EQ(kvm_ej_free_slot(&e), 0);
	CHECK_EQ(kvm_ej_inflight(&e), 0);
	CHECK_EQ(kvm_ej_oldest(&e), -1);
	CHECK_EQ(kvm_ej_take(&e, &jt, &c), -1);					// nothing to do
	s = kvm_ej_submit(&e, &j, 1000);
	CHECK_EQ(s, 0);
	CHECK_EQ(kvm_ej_submit(&e, &j, 1000), -1);				// depth 1: no second job
	CHECK_EQ(kvm_ej_inflight(&e), 1);
	CHECK(!kvm_ej_ready(&e));
	CHECK_EQ(kvm_ej_collect(&e, &jc, &rc, &c), -1);			// not done
	CHECK_EQ(kvm_ej_oldest_age(&e, 1030), 30);
	CHECK_EQ(kvm_ej_take(&e, &jt, &c), 0);
	CHECK_EQ(jt.gen, 7);
	CHECK_EQ(c, 0);
	CHECK_EQ(kvm_ej_take(&e, &jt, &c), -1);					// RUNNING: not taken twice
	CHECK_EQ(kvm_ej_oldest_age(&e, 1250), 250);
	CHECK(!kvm_ej_ready(&e));
	kvm_ej_finish(&e, 0, &r);
	CHECK(kvm_ej_ready(&e));
	CHECK_EQ(kvm_ej_oldest_age(&e, 2000), 0);				// a result that waits is not a hung encoder
	CHECK_EQ(kvm_ej_collect(&e, &jc, &rc, &c), 0);
	CHECK_EQ(rc.len, 100);
	CHECK_EQ(jc.gen, 7);
	CHECK_EQ(c, 0);
	CHECK_EQ(kvm_ej_inflight(&e), 0);
	CHECK_EQ(kvm_ej_free_slot(&e), 0);
	// cancel: the thread still takes it and finishes it, the result is marked
	kvm_ej_submit(&e, &j, 5);
	CHECK_EQ(kvm_ej_cancel_all(&e), 1);
	CHECK_EQ(kvm_ej_cancel_all(&e), 0);						// once
	CHECK_EQ(kvm_ej_take(&e, &jt, &c), 0);
	CHECK_EQ(c, 1);
	CHECK_EQ(kvm_ej_free_slot(&e), -1);						// the slot is not free until the thread is through with it and the main loop collected
	kvm_ej_finish(&e, 0, &r);
	CHECK_EQ(kvm_ej_collect(&e, &jc, &rc, &c), 0);
	CHECK_EQ(c, 1);
	// cancel while RUNNING
	kvm_ej_submit(&e, &j, 5);
	kvm_ej_take(&e, &jt, &c);
	CHECK_EQ(c, 0);
	kvm_ej_cancel_all(&e);
	kvm_ej_finish(&e, 0, &r);
	kvm_ej_collect(&e, &jc, &rc, &c);
	CHECK_EQ(c, 1);
	// abandon: the slot never comes back, a late finish is dropped
	kvm_ej_submit(&e, &j, 5);
	kvm_ej_take(&e, &jt, &c);
	CHECK_EQ(kvm_ej_abandon_all(&e), 1);
	CHECK(kvm_ej_is_abandoned(&e, 0));
	CHECK_EQ(kvm_ej_free_slot(&e), -1);
	CHECK_EQ(kvm_ej_inflight(&e), 0);
	kvm_ej_finish(&e, 0, &r);
	CHECK(!kvm_ej_ready(&e));
	CHECK_EQ(kvm_ej_collect(&e, &jc, &rc, &c), -1);
	CHECK_EQ(kvm_ej_submit(&e, &j, 9), -1);
	CHECK_EQ(kvm_ej_oldest_age(&e, 99999), 0);
	// bad calls
	kvm_ej_finish(&e, 5, &r);
	kvm_ej_finish(&e, -1, &r);
}

static void test_depth3_order(void)
{
	kvm_ej e;
	kvm_ejob j, jc;
	kvm_ejres r, rc;
	int c, a, b, d, k;
	kvm_ej_init(&e, 3);
	j = mkjob(1); a = kvm_ej_submit(&e, &j, 10);
	j = mkjob(2); b = kvm_ej_submit(&e, &j, 11);
	j = mkjob(3); d = kvm_ej_submit(&e, &j, 12);
	CHECK(a == 0 && b == 1 && d == 2);
	CHECK_EQ(kvm_ej_free_slot(&e), -1);
	CHECK_EQ(kvm_ej_inflight(&e), 3);
	// the thread takes in the order of the submit
	CHECK_EQ(kvm_ej_take(&e, &jc, &c), a);
	CHECK_EQ(kvm_ej_take(&e, &jc, &c), b);
	// the second one finishes first: collect() still waits for the first (FIFO)
	r = mkres(0, 22); kvm_ej_finish(&e, b, &r);
	CHECK(!kvm_ej_ready(&e));
	CHECK_EQ(kvm_ej_collect(&e, &jc, &rc, &c), -1);
	r = mkres(0, 11); kvm_ej_finish(&e, a, &r);
	CHECK(kvm_ej_ready(&e));
	CHECK_EQ(kvm_ej_collect(&e, &jc, &rc, &c), a);
	CHECK_EQ(rc.len, 11);
	CHECK_EQ(jc.gen, 1);
	CHECK_EQ(kvm_ej_collect(&e, &jc, &rc, &c), b);
	CHECK_EQ(rc.len, 22);
	CHECK_EQ(kvm_ej_free_slot(&e), 0);							// a slot that was freed is the lowest free one, and takes the next order
	j = mkjob(4); k = kvm_ej_submit(&e, &j, 20);
	CHECK_EQ(k, 0);
	CHECK_EQ(kvm_ej_oldest(&e), d);							// the third job (submitted before it) is older than the fourth
	CHECK_EQ(kvm_ej_take(&e, &jc, &c), d);
	CHECK_EQ(kvm_ej_take(&e, &jc, &c), k);
	// order counter wraps: submit and collect still agree
	e.nextOrder = 0xFFFFFFFEu;
	kvm_ej_finish(&e, d, &r); kvm_ej_finish(&e, k, &r);
	kvm_ej_collect(&e, &jc, &rc, &c); kvm_ej_collect(&e, &jc, &rc, &c);
	j = mkjob(5); a = kvm_ej_submit(&e, &j, 30);
	j = mkjob(6); b = kvm_ej_submit(&e, &j, 31);
	j = mkjob(7); d = kvm_ej_submit(&e, &j, 32);
	CHECK_EQ(kvm_ej_oldest(&e), a);
	CHECK_EQ(kvm_ej_take(&e, &jc, &c), a);
	CHECK_EQ(jc.gen, 5);
	CHECK_EQ(kvm_ej_take(&e, &jc, &c), b);
	CHECK_EQ(kvm_ej_take(&e, &jc, &c), d);
	CHECK_EQ(jc.gen, 7);
}

// the model: a list of jobs in the order of the submit, each with a state; the module has to agree after every call
typedef struct mjob { int state, cancelled, abandoned; uint32_t gen; size_t len; int slot; } mjob;

static void test_model(void)
{
	unsigned seed;
	int session;
	for (session = 0; session < 400; session++)
	{
		kvm_ej e;
		mjob m[64];
		int nm = 0, depth = 1 + (session % 3), step, head = 0;	// head: the first model job that has not been collected
		uint32_t gen = 1, now = 1000;
		seed = (unsigned)session * 2654435761u + 12345u;
		kvm_ej_init(&e, depth);
		for (step = 0; step < 300; step++)
		{
			int op, i;
			seed = seed * 1103515245u + 12345u;
			op = (int)((seed >> 16) % 8);
			now += 3;
			if (op <= 1 && nm < 64)			// submit
			{
				kvm_ejob j = mkjob(gen);
				int s, expectFree = 0, inuse = 0, abandoned = 0;
				for (i = head; i < nm; i++) { if (m[i].abandoned) { abandoned++; } else { inuse++; } }
				// the model: a free slot exists when fewer than depth slots are in use or abandoned (abandoned slots are gone for good)
				for (i = 0; i < nm; i++) { if (m[i].abandoned) { expectFree--; } }
				expectFree += depth;
				(void)abandoned;
				{ int active = 0; for (i = head; i < nm; i++) { if (!m[i].abandoned && m[i].state != 4) { active++; } } expectFree = expectFree - active; }
				s = kvm_ej_submit(&e, &j, now);
				if (expectFree > 0) { CHECK(s >= 0); } else { CHECK_EQ(s, -1); }
				if (s >= 0) { m[nm].state = 1; m[nm].cancelled = 0; m[nm].abandoned = 0; m[nm].gen = gen; m[nm].len = 0; m[nm].slot = s; nm++; gen++; }
			}
			else if (op == 2)				// the thread takes
			{
				kvm_ejob jt; int c, s;
				s = kvm_ej_take(&e, &jt, &c);
				for (i = head; i < nm; i++) { if (!m[i].abandoned && m[i].state == 1) { break; } }
				if (i < nm) { CHECK(s >= 0); CHECK_EQ(jt.gen, m[i].gen); CHECK_EQ(c, m[i].cancelled); CHECK_EQ(s, m[i].slot); m[i].state = 2; } else { CHECK_EQ(s, -1); }
			}
			else if (op == 3 || op == 4)		// the thread finishes a running job (any of them)
			{
				int pick = -1, cnt = 0;
				for (i = head; i < nm; i++) { if (m[i].state == 2 && !m[i].abandoned) { cnt++; if ((int)((seed >> 8) % (unsigned)cnt) == 0) { pick = i; } } }
				if (pick >= 0) { kvm_ejres r = mkres(0, 1000 + (size_t)pick); kvm_ej_finish(&e, m[pick].slot, &r); m[pick].state = 3; m[pick].len = 1000 + (size_t)pick; }
			}
			else if (op == 5)				// collect
			{
				kvm_ejob jc; kvm_ejres rc; int c, s;
				int first = -1;
				for (i = head; i < nm; i++) { if (!m[i].abandoned && m[i].state != 4) { first = i; break; } }
				s = kvm_ej_collect(&e, &jc, &rc, &c);
				if (first >= 0 && m[first].state == 3)
				{
					CHECK_EQ(s, m[first].slot);
					CHECK_EQ(rc.len, m[first].len);
					CHECK_EQ(jc.gen, m[first].gen);
					CHECK_EQ(c, m[first].cancelled);
					m[first].state = 4;
				}
				else { CHECK_EQ(s, -1); }
			}
			else if (op == 6 && (seed >> 20) % 4 == 0)	// cancel all
			{
				int n = 0;
				for (i = head; i < nm; i++) { if (!m[i].abandoned && m[i].state != 4 && !m[i].cancelled) { m[i].cancelled = 1; n++; } }
				CHECK_EQ(kvm_ej_cancel_all(&e), n);
			}
			else if (op == 7 && (seed >> 20) % 16 == 0)	// the watchdog
			{
				int n = 0;
				for (i = head; i < nm; i++) { if (!m[i].abandoned && m[i].state != 4) { m[i].abandoned = 1; n++; } }
				CHECK_EQ(kvm_ej_abandon_all(&e), n);
			}
			{
				int fl = 0, ab = 0;
				for (i = 0; i < nm; i++) { if (!m[i].abandoned && m[i].state != 4) { fl++; } }
				for (i = 0; i < nm; i++) { if (m[i].abandoned) { ab++; } }
				CHECK_EQ(kvm_ej_inflight(&e), fl);
				CHECK((int)e.abandonedN >= 0);
				CHECK_EQ(kvm_ej_ready(&e), 0 + (kvm_ej_oldest(&e) >= 0 && e.slot[kvm_ej_oldest(&e)].state == KVM_EJS_DONE));
				(void)ab;
			}
			while (head < nm && (m[head].state == 4 || m[head].abandoned)) { head++; }
		}
	}
}

// the map of the view is copied into the slot by the submit, the job of the thread points at that copy, the collect does not give it back
static void test_view_map(void)
{
	kvm_ej e;
	kvm_ejob j, jt, jc;
	kvm_ejres r = mkres(0, 10), rc;
	static uint8_t mapA[12], mapB[12], big[4];
	int a, b, d, c, i;
	CHECK(sizeof(kvm_ejob) < 512);							// a job is copied on the stack of the loop and of the thread several times a frame: no map in it
	kvm_ej_init(&e, 3);
	for (i = 0; i < 12; i++) { mapA[i] = (uint8_t)(i & 1); mapB[i] = (uint8_t)(1 - (i & 1)); }
	// a job of the view: the map is a copy, whatever the caller does with its own map afterwards
	j = mkjob(1); j.viewMode = 1; j.mbCols = 4; j.mbRows = 3; j.viewMap = mapA;
	a = kvm_ej_submit(&e, &j, 10);
	j = mkjob(2); j.viewMode = 1; j.mbCols = 3; j.mbRows = 4; j.viewMap = mapB;
	b = kvm_ej_submit(&e, &j, 11);
	CHECK(a >= 0 && b >= 0 && a != b);
	for (i = 0; i < 12; i++) { mapA[i] = 9; mapB[i] = 9; }
	CHECK_EQ(kvm_ej_take(&e, &jt, &c), a);
	CHECK(jt.viewMode == 1 && jt.viewMap != NULL && jt.viewMap != mapA && jt.mbCols == 4 && jt.mbRows == 3);
	for (i = 0; i < 12; i++) { CHECK_EQ(jt.viewMap[i], i & 1); }
	CHECK_EQ(kvm_ej_take(&e, &jt, &c), b);
	CHECK(jt.viewMap != NULL && jt.mbCols == 3);
	for (i = 0; i < 12; i++) { CHECK_EQ(jt.viewMap[i], 1 - (i & 1)); }
	// the map of the first job is still its own (two slots do not share it)
	CHECK(e.slot[a].job.viewMap != e.slot[b].job.viewMap);
	for (i = 0; i < 12; i++) { CHECK_EQ(e.slot[a].job.viewMap[i], i & 1); }
	kvm_ej_finish(&e, a, &r); kvm_ej_finish(&e, b, &r);
	CHECK_EQ(kvm_ej_collect(&e, &jc, &rc, &c), a);
	CHECK(jc.viewMap == NULL && jc.viewMode == 1);			// the copy stays in the slot
	CHECK_EQ(kvm_ej_collect(&e, &jc, &rc, &c), b);
	CHECK(jc.viewMap == NULL);
	// a job that is not of the view carries no map, also when the caller left a pointer in it
	j = mkjob(3); j.viewMode = 0; j.viewMap = mapA; j.mbCols = 4; j.mbRows = 3;
	d = kvm_ej_submit(&e, &j, 12);
	CHECK_EQ(kvm_ej_take(&e, &jt, &c), d);
	CHECK(jt.viewMode == 0 && jt.viewMap == NULL);
	kvm_ej_finish(&e, d, &r); (void)kvm_ej_collect(&e, &jc, &rc, &c);
	// a complete conversion needs no map
	j = mkjob(4); j.viewMode = 1; j.viewFull = 1; j.mbCols = 4; j.mbRows = 3;
	d = kvm_ej_submit(&e, &j, 13);
	CHECK_EQ(kvm_ej_take(&e, &jt, &c), d);
	CHECK(jt.viewMode == 1 && jt.viewFull == 1 && jt.viewMap == NULL);
	kvm_ej_finish(&e, d, &r); (void)kvm_ej_collect(&e, &jc, &rc, &c);
	// a partial conversion without a map, with an empty grid or with a grid that does not fit the slot is a job of the picture as it is (always right)
	j = mkjob(5); j.viewMode = 1; j.mbCols = 4; j.mbRows = 3; j.viewMap = NULL;
	d = kvm_ej_submit(&e, &j, 14); CHECK_EQ(kvm_ej_take(&e, &jt, &c), d); CHECK(jt.viewMode == 0 && jt.viewMap == NULL); kvm_ej_finish(&e, d, &r); (void)kvm_ej_collect(&e, &jc, &rc, &c);
	j = mkjob(6); j.viewMode = 1; j.mbCols = 0; j.mbRows = 3; j.viewMap = big;
	d = kvm_ej_submit(&e, &j, 15); CHECK_EQ(kvm_ej_take(&e, &jt, &c), d); CHECK(jt.viewMode == 0 && jt.viewMap == NULL); kvm_ej_finish(&e, d, &r); (void)kvm_ej_collect(&e, &jc, &rc, &c);
	j = mkjob(7); j.viewMode = 1; j.mbCols = -2; j.mbRows = -3; j.viewMap = big;
	d = kvm_ej_submit(&e, &j, 16); CHECK_EQ(kvm_ej_take(&e, &jt, &c), d); CHECK(jt.viewMode == 0 && jt.viewMap == NULL); kvm_ej_finish(&e, d, &r); (void)kvm_ej_collect(&e, &jc, &rc, &c);
	j = mkjob(8); j.viewMode = 1; j.mbCols = KVM_EJ_MB_MAX; j.mbRows = 2; j.viewMap = big;
	d = kvm_ej_submit(&e, &j, 17); CHECK_EQ(kvm_ej_take(&e, &jt, &c), d); CHECK(jt.viewMode == 0 && jt.viewMap == NULL); kvm_ej_finish(&e, d, &r); (void)kvm_ej_collect(&e, &jc, &rc, &c);
	// the biggest grid that fits is copied whole
	{
		static uint8_t full[KVM_EJ_MB_MAX];
		for (i = 0; i < KVM_EJ_MB_MAX; i++) { full[i] = (uint8_t)((i * 7) & 1); }
		j = mkjob(9); j.viewMode = 1; j.mbCols = 192; j.mbRows = 192; j.viewMap = full;
		d = kvm_ej_submit(&e, &j, 18); CHECK_EQ(kvm_ej_take(&e, &jt, &c), d);
		CHECK(jt.viewMode == 1 && jt.viewMap != NULL && memcmp(jt.viewMap, full, KVM_EJ_MB_MAX) == 0);
		kvm_ej_finish(&e, d, &r); (void)kvm_ej_collect(&e, &jc, &rc, &c);
	}
}

int main(void)
{
	test_basic();
	test_view_map();
	test_depth3_order();
	test_model();
	printf("%d checks, %d failed\n", gChecks, gFail);
	return gFail ? 1 : 0;
}
