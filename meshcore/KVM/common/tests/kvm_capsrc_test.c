/*
Tests of kvm_capsrc.c (choice of the capture source, the capture source choice): unit tests of every rule, and a simulation of the capture loop of Windows/tile.cpp on a model of the screen and of
the duplication (silent, healthy, differing by colour, a layered window, a moving mouse), driven the way the glue drives the module. Time is injected and every scenario runs on three
clocks, one of them about to wrap. Run with run-kvm-capsrc-tests.sh (gcc in a container: -Werror -pedantic, then ASan + UBSan). The lines starting with "sim" are the numbers of the report.

-DKVM_CAPSRC_OLD_API compiles the tests against the module of the capture source choice (the first version: compare_result with one number, no reference, probation in DXGI mode). The tests of the capture source choice are written so that
they run there too, and fail there: that is how the report shows that they catch the problems.
*/

#include "../kvm_capsrc.h"
#include <stdio.h>
#include <string.h>

static int gFail = 0, gChecks = 0, gPrint = 1;
#define SIMP(...) do { if (gPrint) { printf(__VA_ARGS__); } } while (0)
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); gChecks++; if (_a != _b) { gFail++; printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

// ---------------------------------------------------------------------------------------------------------------------------------------
// what the glue does around the calls: it takes the reference when the module asks for it
// ---------------------------------------------------------------------------------------------------------------------------------------

#ifdef KVM_CAPSRC_OLD_API
#define KVM_CAPSRC_PROBE_BUSY_MS KVM_CAPSRC_PROBATION_MS		// (the constants of the capture source choice that the old module does not have)
#define KVM_CAPSRC_WATCH_MS 1500
#define KVM_CAPSRC_WATCH_WINDOW_MS 60000
static int rwanted(const kvm_capsrc *c) { (void)c; return 0; }
static int rusable(const kvm_capsrc *c) { (void)c; return 1; }
static int raw_cmp(kvm_capsrc *c, uint32_t t, int64_t persistentPx, int64_t refPx) { (void)refPx; return kvm_capsrc_compare_result(c, t, persistentPx); }
static void svc(kvm_capsrc *c) { (void)c; }
#else
static int rwanted(const kvm_capsrc *c) { return kvm_capsrc_ref_wanted(c); }
static int rusable(const kvm_capsrc *c) { return kvm_capsrc_ref_usable(c); }
static int raw_cmp(kvm_capsrc *c, uint32_t t, int64_t persistentPx, int64_t refPx) { return kvm_capsrc_compare_result(c, t, persistentPx, refPx); }
static void svc(kvm_capsrc *c) { if (kvm_capsrc_ref_wanted(c)) { kvm_capsrc_ref_done(c, 1); } }
#endif
static int cmp(kvm_capsrc *c, uint32_t t, int64_t persistentPx, int64_t refPx) { int a = raw_cmp(c, t, persistentPx, refPx); svc(c); return a; }
static int cmp1(kvm_capsrc *c, uint32_t t, int64_t px) { return cmp(c, t, px, px); }		// the persistent image began as a GDI capture: both are the same
static void fed(kvm_capsrc *c) { kvm_capsrc_fed(c); svc(c); }								// the persistent image was filled from GDI
static void full(kvm_capsrc *c, uint32_t t) { kvm_capsrc_image(c, t, 1); svc(c); }		// a first (full) copy of DXGI

// ---------------------------------------------------------------------------------------------------------------------------------------
// units
// ---------------------------------------------------------------------------------------------------------------------------------------

static void test_init(uint32_t t0)
{
	kvm_capsrc c;
	kvm_capsrc_init(&c, t0);
	CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_DXGI);
	CHECK(!kvm_capsrc_probation(&c));
	CHECK(!kvm_capsrc_probe_due(&c, t0 + 100000u));			// DXGI mode: no probe
	CHECK(strcmp(kvm_capsrc_mode_name(&c), "dxgi") == 0);
	CHECK(strcmp(kvm_capsrc_reason_name(KVM_CAPSRC_R_SILENT), "dxgi_silent") == 0);
	CHECK(strcmp(kvm_capsrc_reason_name(KVM_CAPSRC_R_UNAVAILABLE), "dxgi_unavailable") == 0);
	CHECK(strcmp(kvm_capsrc_reason_name(KVM_CAPSRC_R_NONE), "none") == 0);
}

// the comparison is due: not before 2 s after the opening, then 3 s apart (3 times), then every 10 s; never while images came less than 1 s ago
static void test_due(uint32_t t0)
{
	kvm_capsrc c;
	uint32_t t = t0;
	kvm_capsrc_init(&c, t);
	kvm_capsrc_opened(&c, t);
	fed(&c);
	CHECK(!kvm_capsrc_compare_due(&c, t + 1999u));
	CHECK(kvm_capsrc_compare_due(&c, t + 2000u));
	t += 2000u;
	CHECK_EQ(cmp1(&c, t, 0), KVM_CAPSRC_ACT_NONE);
	CHECK(!kvm_capsrc_compare_due(&c, t + 2999u));
	CHECK(kvm_capsrc_compare_due(&c, t + 3000u));
	t += 3000u;
	CHECK_EQ(cmp1(&c, t, 0), KVM_CAPSRC_ACT_NONE);
	CHECK(kvm_capsrc_compare_due(&c, t + 3000u));
	t += 3000u;
	CHECK_EQ(cmp1(&c, t, 0), KVM_CAPSRC_ACT_NONE);		// third: 8 s
	CHECK(!kvm_capsrc_compare_due(&c, t + 9999u));							// then the old pace
	CHECK(kvm_capsrc_compare_due(&c, t + 10000u));
	// an image: nothing for 1 s
	t += 10000u;
	kvm_capsrc_image(&c, t, 0);
	CHECK(!kvm_capsrc_compare_due(&c, t + 500u));
	CHECK(!kvm_capsrc_compare_due(&c, t + 999u));
	CHECK(kvm_capsrc_compare_due(&c, t + 1000u));							// quiet for 1 s and 11 s since the last comparison
}

// the sequence of "silent": the screen changes after a GDI start; two comparisons 1 s apart decide
static void test_silent_decision(uint32_t t0)
{
	kvm_capsrc c;
	uint32_t t = t0;
	kvm_capsrc_init(&c, t);
	kvm_capsrc_opened(&c, t);
	fed(&c);
	t += 2000u;
	CHECK(kvm_capsrc_compare_due(&c, t));
	CHECK_EQ(cmp1(&c, t, 1500000), KVM_CAPSRC_ACT_SUSPECT);
	CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_DXGI);
	CHECK(!kvm_capsrc_compare_due(&c, t + 999u));						// the confirmation is 1 s later
	CHECK(kvm_capsrc_compare_due(&c, t + 1000u));
	t += 1000u;
	CHECK_EQ(cmp1(&c, t, 1500000), KVM_CAPSRC_ACT_TO_GDI);
	CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_GDI);
	CHECK_EQ(c.reason, KVM_CAPSRC_R_SILENT);
	CHECK(strcmp(kvm_capsrc_mode_name(&c), "gdi") == 0);
	CHECK_EQ(c.st.toGdi, 1);
	CHECK(!kvm_capsrc_compare_due(&c, t + 100000u));					// GDI mode: no comparisons
	CHECK(!kvm_capsrc_probe_due(&c, t + 4999u));
	CHECK(kvm_capsrc_probe_due(&c, t + 5000u));
}

// a static screen: DXGI silent, the pictures agree: nothing happens, for ever
static void test_static_never_switches(uint32_t t0)
{
	kvm_capsrc c;
	uint32_t t = t0, ms;
	int compares = 0;
	kvm_capsrc_init(&c, t);
	kvm_capsrc_opened(&c, t);
	fed(&c);
	for (ms = 0; ms < 3600u * 1000u; ms += 50u)
	{
		t = t0 + ms;
		if (kvm_capsrc_compare_due(&c, t)) { compares++; CHECK_EQ(cmp1(&c, t, 0), KVM_CAPSRC_ACT_NONE); }
		CHECK_EQ(kvm_capsrc_tick(&c, t, 1), KVM_CAPSRC_ACT_NONE);
	}
	CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_DXGI);
	CHECK_EQ(c.st.toGdi, 0);
	SIMP("unit static screen, 1 hour, no image: %d comparisons (3 in the first 8 s, then one per 10 s)\n", compares);
	CHECK(compares <= 3 + 361);
}

// one difference is not enough, and it must persist: a difference seen once and then gone (a change DXGI reports a moment later) resets the run
static void test_one_diff_not_enough(uint32_t t0)
{
	kvm_capsrc c;
	uint32_t t = t0;
	kvm_capsrc_init(&c, t);
	kvm_capsrc_opened(&c, t);
	fed(&c);
	t += 2000u;
	CHECK_EQ(cmp1(&c, t, 900), KVM_CAPSRC_ACT_SUSPECT);
	t += 1000u;
	CHECK_EQ(cmp1(&c, t, 0), KVM_CAPSRC_ACT_NONE);		// equal again
	CHECK_EQ(c.diffRun, 0);
	t += 3000u;
	CHECK_EQ(cmp1(&c, t, 900), KVM_CAPSRC_ACT_SUSPECT);	// the run starts again from 1
	CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_DXGI);
	// an image between the two differences resets the run, too
	kvm_capsrc_image(&c, t + 100u, 0);
	t += 1100u;
	CHECK_EQ(cmp1(&c, t, 900), KVM_CAPSRC_ACT_SUSPECT);	// updates > 0 now: "differs" branch, first strike
	CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_DXGI);
	// a comparison that failed (-1) learns nothing and changes nothing
	{
		int run = c.diffRun, cnt = c.cmpCount;
		CHECK_EQ(cmp(&c, t + 1000u, -1, -1), KVM_CAPSRC_ACT_NONE);
		CHECK_EQ(c.diffRun, run);
		CHECK_EQ(c.cmpCount, cnt);
	}
}

// DXGI delivered images and went quiet: first the duplication is created again (as before the capture source choice), then silence is judged on the new one
static void test_quiet_after_images(uint32_t t0)
{
	kvm_capsrc c;
	uint32_t t = t0;
	kvm_capsrc_init(&c, t);
	kvm_capsrc_opened(&c, t);
	full(&c, t);												// the first full copy
	kvm_capsrc_image(&c, t + 100u, 0);
	kvm_capsrc_image(&c, t + 200u, 0);
	t += 11000u;
	CHECK(kvm_capsrc_compare_due(&c, t));
	CHECK_EQ(cmp1(&c, t, 40000), KVM_CAPSRC_ACT_SUSPECT);
	t += 1000u;
	CHECK_EQ(cmp1(&c, t, 40000), KVM_CAPSRC_ACT_REOPEN);
	CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_DXGI);
	CHECK_EQ(c.st.reopens, 1);
	kvm_capsrc_opened(&c, t);									// the glue created it again
	fed(&c);													// no first frame came: the persistent image is a GDI capture
	t += 2000u;
	CHECK(kvm_capsrc_compare_due(&c, t));
	CHECK_EQ(cmp1(&c, t, 40000), KVM_CAPSRC_ACT_SUSPECT);		// silent on the new duplication: not "systematic"
	t += 1000u;
	CHECK_EQ(cmp1(&c, t, 40000), KVM_CAPSRC_ACT_TO_GDI);
	CHECK_EQ(c.reason, KVM_CAPSRC_R_SILENT);
	CHECK(!c.systematic);
}

// the persistent image is filled from GDI after it was a DXGI full copy (a new generation of the full capture): the reference is taken again
static void test_fed_after_full(uint32_t t0)
{
	kvm_capsrc c;
	uint32_t t = t0;
	kvm_capsrc_init(&c, t);
	kvm_capsrc_opened(&c, t);
	full(&c, t);
	fed(&c);
	t += 2000u;
	CHECK_EQ(cmp1(&c, t, 500), KVM_CAPSRC_ACT_SUSPECT);
	t += 1000u;
	CHECK_EQ(cmp1(&c, t, 500), KVM_CAPSRC_ACT_TO_GDI);
}

// DXGI delivers images and still differs after the duplication was created again: systematic, measured only; it ends when the pictures agree
static void test_systematic_latch(uint32_t t0)
{
	kvm_capsrc c;
	uint32_t t = t0;
	kvm_capsrc_init(&c, t);
	kvm_capsrc_opened(&c, t);
	full(&c, t);
	kvm_capsrc_image(&c, t + 100u, 0);
	t += 11000u;
	CHECK_EQ(cmp(&c, t, 70, 0), KVM_CAPSRC_ACT_SUSPECT);
	t += 1000u;
	CHECK_EQ(cmp(&c, t, 70, 0), KVM_CAPSRC_ACT_REOPEN);
	kvm_capsrc_opened(&c, t);
	full(&c, t + 10u);											// the first frame of the new duplication
	kvm_capsrc_image(&c, t + 500u, 0);
	t += 11000u;
	CHECK_EQ(cmp(&c, t, 70, 0), KVM_CAPSRC_ACT_SYSTEMATIC);
	CHECK(c.systematic);
	CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_DXGI);
	// while latched: the same difference again and again (the screen does not change), with and without images: nothing, never GDI
	{
		int i;
		for (i = 0; i < 20; i++)
		{
			if (i % 2) { kvm_capsrc_image(&c, t + 100u, 0); }
			t += 10000u;
			CHECK_EQ(cmp(&c, t, 70, 0), KVM_CAPSRC_ACT_NONE);
		}
	}
	CHECK(c.systematic);
	CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_DXGI);
	// the pictures agree: the latch ends
	t += 10000u;
	CHECK_EQ(cmp(&c, t, 0, 0), KVM_CAPSRC_ACT_CLEARED);
	CHECK(!c.systematic);
	CHECK_EQ(c.st.cleared, 1);
	// and after it the old rules apply again: a difference twice, the duplication is created again
	t += 10000u;
	CHECK_EQ(cmp(&c, t, 70, 0), KVM_CAPSRC_ACT_SUSPECT);
}

static void test_systematic_expires_and_resets(uint32_t t0)
{
	kvm_capsrc c;
	uint32_t t = t0;
	kvm_capsrc_init(&c, t);
	kvm_capsrc_opened(&c, t);
	full(&c, t);
	kvm_capsrc_image(&c, t + 100u, 0);
	c.afterReinit = 1;
	t += 11000u;
	CHECK_EQ(cmp(&c, t, 70, 0), KVM_CAPSRC_ACT_SYSTEMATIC);
	// after KVM_CAPSRC_SYSTEMATIC_MS the difference is judged again (a first strike, the latch is off)
	t += KVM_CAPSRC_SYSTEMATIC_MS - 10000u;
	CHECK_EQ(cmp(&c, t, 70, 0), KVM_CAPSRC_ACT_NONE);		// just before: still latched
	t += 10000u;
	CHECK_EQ(cmp(&c, t, 70, 0), KVM_CAPSRC_ACT_SUSPECT);
	CHECK(!c.systematic);
	// a reset (desktop switch, geometry) ends it at once
	c.systematic = 1; c.systematicAt = t; c.afterReinit = 1;
	kvm_capsrc_reset(&c, t);
	CHECK(!c.systematic);
	CHECK(!c.afterReinit);
	CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_DXGI);
}

// silence behind the latch: nothing arrives, the screen changes (GDI against GDI)
static void test_silence_behind_latch(uint32_t t0)
{
	kvm_capsrc c;
	uint32_t t = t0;
	kvm_capsrc_init(&c, t);
	kvm_capsrc_opened(&c, t);
	full(&c, t);
	kvm_capsrc_image(&c, t + 100u, 0);
	c.afterReinit = 1;
	t += 11000u;
	CHECK_EQ(cmp(&c, t, 70, 0), KVM_CAPSRC_ACT_SYSTEMATIC);
	t += 10000u;
	CHECK_EQ(cmp(&c, t, 70, 0), KVM_CAPSRC_ACT_NONE);		// same difference, the screen did not change: a colour difference
	t += 10000u;
	CHECK_EQ(cmp(&c, t, 90000, 90000), KVM_CAPSRC_ACT_SUSPECT);	// the screen changed with no image since the reference
	CHECK(kvm_capsrc_compare_due(&c, t + 1000u));						// confirmed after 1 s
	t += 1000u;
	CHECK_EQ(cmp(&c, t, 120000, 120000), KVM_CAPSRC_ACT_TO_GDI);
	CHECK_EQ(c.reason, KVM_CAPSRC_R_SILENT);
	// but with images in between it is a working DXGI that differs
	kvm_capsrc_init(&c, t);
	kvm_capsrc_opened(&c, t);
	full(&c, t);
	kvm_capsrc_image(&c, t + 100u, 0);
	c.afterReinit = 1;
	t += 11000u;
	CHECK_EQ(cmp(&c, t, 70, 0), KVM_CAPSRC_ACT_SYSTEMATIC);
	kvm_capsrc_image(&c, t + 9000u, 0);
	t += 10000u;
	CHECK_EQ(cmp(&c, t, 90000, 90000), KVM_CAPSRC_ACT_NONE);
	CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_DXGI);
}

// the persistent image began as a DXGI full copy that differs from GDI by colour (HDR) and DXGI sends nothing more (static screen): never GDI
static void test_hdr_static(uint32_t t0)
{
	kvm_capsrc c;
	uint32_t t = t0;
	int i;
	kvm_capsrc_init(&c, t);
	kvm_capsrc_opened(&c, t);
	full(&c, t);
	t += 11000u;
	CHECK_EQ(cmp(&c, t, 123456, 0), KVM_CAPSRC_ACT_NONE);	// the persistent image differs for good, the screen did not change since the full copy
	for (i = 0; i < 100; i++)
	{
		CHECK(c.updates == 0);
		t += 10000u;
		CHECK_EQ(cmp(&c, t, 123456, 0), KVM_CAPSRC_ACT_NONE);
	}
	CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_DXGI);
	// the same started from GDI: a difference is a change too
	{
		kvm_capsrc d;
		kvm_capsrc_init(&d, t);
		kvm_capsrc_opened(&d, t);
		fed(&d);
		CHECK_EQ(cmp1(&d, t + 2000u, 123456), KVM_CAPSRC_ACT_SUSPECT);
	}
	// full copy, then the screen changes and nothing arrives: silent, after the second comparison
	t += 10000u;
	CHECK_EQ(cmp(&c, t, 200000, 200000), KVM_CAPSRC_ACT_SUSPECT);
	t += 1000u;
	CHECK_EQ(cmp(&c, t, 260000, 260000), KVM_CAPSRC_ACT_TO_GDI);
}

// GDI mode: the pauses 5, 10, 20, 40, 60, 60 s; recovery resets them. A probe is a shadow: the mode is GDI all the time.
static void test_probe_backoff(uint32_t t0)
{
	kvm_capsrc c;
	uint32_t t = t0, iv;
	static const uint32_t want[] = { 5000, 10000, 20000, 40000, 60000, 60000, 60000 };
	int i;
	kvm_capsrc_init(&c, t);
	kvm_capsrc_opened(&c, t);
	fed(&c);
	t += 2000u;
	CHECK_EQ(cmp1(&c, t, 5000), KVM_CAPSRC_ACT_SUSPECT);
	t += 1000u;
	CHECK_EQ(cmp1(&c, t, 5000), KVM_CAPSRC_ACT_TO_GDI);
	for (i = 0; i < 7; i++)
	{
		iv = want[i];
		CHECK_EQ(kvm_capsrc_probe_interval(&c), iv);
		CHECK(!kvm_capsrc_probe_due(&c, t + iv - 1u));
		CHECK(kvm_capsrc_probe_due(&c, t + iv));
		t += iv;
		kvm_capsrc_probe_begin(&c, t);
		CHECK(kvm_capsrc_probation(&c));
		CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_GDI);			// the source is GDI while DXGI is tried
		CHECK(strcmp(kvm_capsrc_mode_name(&c), "probe") == 0);
		CHECK(!kvm_capsrc_probe_due(&c, t + 999999u));				// not again while it runs
		kvm_capsrc_opened(&c, t);
		fed(&c);
		CHECK(!kvm_capsrc_compare_due(&c, t + 599u));
		CHECK(kvm_capsrc_compare_due(&c, t + 600u));
		t += 600u;
		CHECK_EQ(cmp1(&c, t, 5000), KVM_CAPSRC_ACT_SUSPECT);		// the screen changed: twice, 0.2 s apart
		CHECK(!kvm_capsrc_compare_due(&c, t + 199u));
		CHECK(kvm_capsrc_compare_due(&c, t + 200u));
		t += 200u;
		CHECK_EQ(cmp1(&c, t, 5000), KVM_CAPSRC_ACT_PROBE_FAILED);
		CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_GDI);
		CHECK(!kvm_capsrc_probation(&c));
		CHECK_EQ(c.fails, i + 1);
	}
	CHECK_EQ(c.st.probes, 7);
	CHECK_EQ(c.st.probeFails, 7);
	CHECK_EQ(c.st.toGdi, 1);
	// the period in GDI mode is counted once
	CHECK(kvm_capsrc_gdi_ms(&c, t) >= 5000u + 10000u + 20000u + 40000u + 60000u);
	// a long run: the pause stays at the cap
	CHECK_EQ(kvm_capsrc_probe_interval(&c), 60000);
}

// recovery: images after the first full copy and the pictures agree
static void test_probe_recovery(uint32_t t0)
{
	kvm_capsrc c;
	uint32_t t = t0;
	kvm_capsrc_init(&c, t);
	kvm_capsrc_opened(&c, t);
	fed(&c);
	t += 2000u;
	(void)cmp1(&c, t, 5000);
	t += 1000u;
	CHECK_EQ(cmp1(&c, t, 5000), KVM_CAPSRC_ACT_TO_GDI);
	t += 5000u;
	CHECK(kvm_capsrc_probe_due(&c, t));
	// one failed probe first (the latches of the old duplication are not carried into a probe)
	c.systematic = 1; c.afterReinit = 1;
	kvm_capsrc_probe_begin(&c, t);
	CHECK(!c.systematic && !c.afterReinit);
	kvm_capsrc_opened(&c, t);
	fed(&c);
	t += 600u;
	CHECK_EQ(cmp1(&c, t, 5000), KVM_CAPSRC_ACT_SUSPECT);
	t += 200u;
	CHECK_EQ(cmp1(&c, t, 5000), KVM_CAPSRC_ACT_PROBE_FAILED);
	CHECK_EQ(c.fails, 1);
	t += 10000u;
	CHECK(kvm_capsrc_probe_due(&c, t));
	kvm_capsrc_probe_begin(&c, t);
	kvm_capsrc_opened(&c, t);
	full(&c, t + 5u);												// the first full copy is not enough
	CHECK_EQ(kvm_capsrc_tick(&c, t + 3000u, 1), KVM_CAPSRC_ACT_NONE);
	kvm_capsrc_image(&c, t + 300u, 0);
	kvm_capsrc_image(&c, t + 900u, 0);
	CHECK_EQ(cmp(&c, t + 700u, 0, 0), KVM_CAPSRC_ACT_NONE);		// images and equal, but a probe is no success before 1 s
	CHECK(kvm_capsrc_probation(&c));
	CHECK(kvm_capsrc_compare_due(&c, t + 1500u));
	CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_GDI);				// GDI up to the moment of the verdict
	CHECK_EQ(cmp(&c, t + 1500u, 0, 0), KVM_CAPSRC_ACT_RECOVERED);
	CHECK(!kvm_capsrc_probation(&c));
	CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_DXGI);
	CHECK_EQ(c.fails, 0);
	CHECK_EQ(c.st.recovered, 1);
	CHECK_EQ(c.st.probeFails, 1);
	CHECK_EQ(c.lastGdiMs, 17300);										// from the switch (3 s) to the recovery (20.3 s)
	CHECK_EQ(c.updates, 0);												// the persistent image begins again (the glue fills it from GDI or from a full copy)
	CHECK_EQ(c.cmpCount, 0);
	CHECK(c.lastImage == t + 1500u && c.openAt == t + 1500u);
	CHECK(kvm_capsrc_gdi_ms(&c, t + 99999u) == c.lastGdiMs);			// the period is closed
	// the next fall starts at the shortest pause again
	t += 2000u;
	kvm_capsrc_opened(&c, t);
	fed(&c);
	t += 2000u;
	(void)cmp1(&c, t, 100);
	t += 1000u;
	CHECK_EQ(cmp1(&c, t, 100), KVM_CAPSRC_ACT_TO_GDI);
	CHECK_EQ(kvm_capsrc_probe_interval(&c), 5000);
	CHECK_EQ(c.st.toGdi, 2);
}

// a probe on a static screen: nothing arrives, nothing differs. Inconclusive: GDI stays (DXGI is not made the source without evidence), the pause is not forgotten
static void test_probe_inconclusive(uint32_t t0)
{
	kvm_capsrc c;
	uint32_t t = t0;
	kvm_capsrc_init(&c, t);
	kvm_capsrc_unavailable(&c, t);									// GDI
	t += 5000u;
	kvm_capsrc_probe_begin(&c, t);
	kvm_capsrc_opened(&c, t);
	fed(&c);
	CHECK_EQ(kvm_capsrc_tick(&c, t + 600u, 1), KVM_CAPSRC_ACT_NONE);
	CHECK_EQ(cmp1(&c, t + 600u, 0), KVM_CAPSRC_ACT_NONE);			// agree, but nothing arrived: no success
	CHECK_EQ(cmp1(&c, t + 1500u, 0), KVM_CAPSRC_ACT_NONE);			// not later either
	CHECK(kvm_capsrc_probation(&c));
	CHECK_EQ(kvm_capsrc_tick(&c, t + KVM_CAPSRC_PROBATION_MS - 1u, 1), KVM_CAPSRC_ACT_NONE);
	CHECK_EQ(kvm_capsrc_tick(&c, t + KVM_CAPSRC_PROBATION_MS, 1), KVM_CAPSRC_ACT_INCONCLUSIVE);
	CHECK(!kvm_capsrc_probation(&c));
	CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_GDI);				// the source never was DXGI, and it is not now
	CHECK_EQ(c.st.inconclusive, 1);
	CHECK_EQ(c.fails, 0);											// no failed probe so far
	CHECK_EQ(kvm_capsrc_tick(&c, t + 99999u, 1), KVM_CAPSRC_ACT_NONE);	// not in probation any more
	CHECK(!kvm_capsrc_probe_due(&c, t + KVM_CAPSRC_PROBATION_MS + 4999u));	// the next probe after the same pause
	CHECK(kvm_capsrc_probe_due(&c, t + KVM_CAPSRC_PROBATION_MS + 5000u));
	t += KVM_CAPSRC_PROBATION_MS + 5000u;
	// the screen changes during the next probe while DXGI is still silent: it fails in the shadow within a second, the viewer never saw a stale picture
	kvm_capsrc_probe_begin(&c, t);
	kvm_capsrc_opened(&c, t);
	fed(&c);
	CHECK_EQ(cmp1(&c, t + 600u, 777), KVM_CAPSRC_ACT_SUSPECT);
	CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_GDI);
	CHECK_EQ(cmp1(&c, t + 800u, 777), KVM_CAPSRC_ACT_PROBE_FAILED);
	CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_GDI);
	CHECK_EQ(c.fails, 1);
}

// a probe that ran out with images and no difference noted: recovered; one that ran out without the duplication ever being open: failed
static void test_probe_expiry(uint32_t t0)
{
	kvm_capsrc c;
	uint32_t t = t0, k;
	kvm_capsrc_init(&c, t);
	(void)kvm_capsrc_unavailable(&c, t);
	t += 5000u;
	kvm_capsrc_probe_begin(&c, t);
	kvm_capsrc_opened(&c, t);
	full(&c, t + 1u);
	for (k = 40u; k < KVM_CAPSRC_PROBE_BUSY_MS + 400u; k += 400u) { kvm_capsrc_image(&c, t + k, 0); }	// video: a stream of images, never quiet, so never compared
	CHECK(!kvm_capsrc_compare_due(&c, t + KVM_CAPSRC_PROBE_BUSY_MS));
	CHECK_EQ(kvm_capsrc_tick(&c, t + KVM_CAPSRC_PROBE_BUSY_MS - 1u, 1), KVM_CAPSRC_ACT_NONE);
	CHECK_EQ(kvm_capsrc_tick(&c, t + KVM_CAPSRC_PROBE_BUSY_MS, 1), KVM_CAPSRC_ACT_RECOVERED);
	CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_DXGI);
	// closed during the whole probe
	kvm_capsrc_init(&c, t);
	(void)kvm_capsrc_unavailable(&c, t);
	t += 5000u;
	kvm_capsrc_probe_begin(&c, t);
	CHECK_EQ(kvm_capsrc_tick(&c, t + KVM_CAPSRC_PROBATION_MS, 0), KVM_CAPSRC_ACT_PROBE_FAILED);
	CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_GDI);
	CHECK_EQ(c.fails, 1);
	CHECK_EQ(kvm_capsrc_probe_interval(&c), 10000);
}

// the duplication cannot be created (no output, rotated, errors): GDI at once, and the output is looked for again later
static void test_unavailable_and_reset(uint32_t t0)
{
	kvm_capsrc c;
	uint32_t t = t0;
	kvm_capsrc_init(&c, t);
	CHECK_EQ(kvm_capsrc_unavailable(&c, t), KVM_CAPSRC_ACT_TO_GDI);
	CHECK_EQ(c.reason, KVM_CAPSRC_R_UNAVAILABLE);
	CHECK(!kvm_capsrc_probe_due(&c, t + 4999u));
	// a failed attempt to create it during probation: failed probe, longer pause
	t += 5000u;
	kvm_capsrc_probe_begin(&c, t);
	CHECK_EQ(kvm_capsrc_unavailable(&c, t), KVM_CAPSRC_ACT_PROBE_FAILED);
	CHECK_EQ(c.fails, 1);
	CHECK(!kvm_capsrc_probe_due(&c, t + 9999u));
	CHECK(kvm_capsrc_probe_due(&c, t + 10000u));
	// a change of the geometry or the desktop: try at once, the pause is kept
	kvm_capsrc_reset(&c, t + 100u);
	CHECK(kvm_capsrc_probe_due(&c, t + 100u));
	CHECK_EQ(c.fails, 1);
#ifndef KVM_CAPSRC_OLD_API
	// ... also while a probe runs: it ends without a verdict (nothing is held against DXGI), a new one is due at once
	kvm_capsrc_probe_begin(&c, t + 200u);
	CHECK(kvm_capsrc_probation(&c));
	kvm_capsrc_reset(&c, t + 300u);
	CHECK(!kvm_capsrc_probation(&c));
	CHECK(kvm_capsrc_probe_due(&c, t + 300u));
	CHECK_EQ(c.fails, 1);
#endif
	// reset in DXGI mode touches nothing but the latches
	kvm_capsrc_init(&c, t);
	kvm_capsrc_reset(&c, t);
	CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_DXGI);
	CHECK(!kvm_capsrc_probe_due(&c, t + 100000u));
}

// the wrap of the counter: GDI mode for a time that crosses 2^32
static void test_gdi_ms_wrap(void)
{
	kvm_capsrc c;
	uint32_t t = 0xFFFFFC00u;
	kvm_capsrc_init(&c, t);
	(void)kvm_capsrc_unavailable(&c, t);
	CHECK_EQ(kvm_capsrc_gdi_ms(&c, t + 2000u), 2000);
	CHECK(kvm_capsrc_probe_due(&c, t + 5000u));
	CHECK(!kvm_capsrc_probe_due(&c, t + 4000u));
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the scenarios of the first version of the capture source choice. Every one of them fails against the module of the capture source choice (-DKVM_CAPSRC_OLD_API).
// ---------------------------------------------------------------------------------------------------------------------------------------

// a duplication that delivered nothing since it was opened, on a machine that fell to GDI before, is compared every 1.5 s for a minute, not every 10 s
static void test_m1_watch(uint32_t t0)
{
	kvm_capsrc c, d;
	uint32_t t = t0, last, k;
	int n = 0;
	kvm_capsrc_init(&c, t);
	kvm_capsrc_opened(&c, t);
	fed(&c);
	t += 2000u; (void)cmp1(&c, t, 100);
	t += 1000u; CHECK_EQ(cmp1(&c, t, 100), KVM_CAPSRC_ACT_TO_GDI);	// the machine fell to GDI once
	t += 5000u;
	kvm_capsrc_probe_begin(&c, t);
	kvm_capsrc_opened(&c, t);
	full(&c, t + 5u);
	kvm_capsrc_image(&c, t + 100u, 0);
	CHECK_EQ(cmp(&c, t + 1500u, 0, 0), KVM_CAPSRC_ACT_RECOVERED);
	t += 1500u;
	// DXGI is the source again, the duplication delivered images in the probe; the persistent image begins again from GDI and nothing arrives (the panel went off, a static screen)
	kvm_capsrc_opened(&c, t);
	fed(&c);
	last = t;
	for (k = 0; k < 80000u; k += 50u)
	{
		uint32_t now = t + k;
		if (kvm_capsrc_compare_due(&c, now))
		{
			if (n >= KVM_CAPSRC_FAST_COUNT && now - t < KVM_CAPSRC_WATCH_WINDOW_MS) { CHECK(now - last >= KVM_CAPSRC_WATCH_MS && now - last < KVM_CAPSRC_WATCH_MS + 50u); }
			if (n >= KVM_CAPSRC_FAST_COUNT && now - t >= KVM_CAPSRC_WATCH_WINDOW_MS + 50u) { CHECK(now - last >= KVM_CAPSRC_PERIOD_MS); }
			n++;
			last = now;
			CHECK_EQ(cmp1(&c, now, 0), KVM_CAPSRC_ACT_NONE);
		}
	}
	CHECK(n >= 3 + 30 && n <= 3 + 40 + 3);							// 3 fast ones, about 35 in the window, 2 more after it
	// the screen changes after 30 s of a static screen: found within the watch spacing and the confirmation, not up to a period later
	kvm_capsrc_init(&d, t);
	kvm_capsrc_opened(&d, t);
	fed(&d);
	t += 2000u; (void)cmp1(&d, t, 100); t += 1000u; (void)cmp1(&d, t, 100);		// two differences: GDI
	CHECK_EQ(kvm_capsrc_mode(&d), KVM_CAPSRC_MODE_GDI);
	t += 5000u;
	kvm_capsrc_probe_begin(&d, t); kvm_capsrc_opened(&d, t); full(&d, t); kvm_capsrc_image(&d, t + 50u, 0);
	CHECK_EQ(cmp(&d, t + 1200u, 0, 0), KVM_CAPSRC_ACT_RECOVERED);
	t += 1200u;
	kvm_capsrc_opened(&d, t);
	fed(&d);
	{
		uint32_t change = t + 30000u, found = 0;
		for (k = 0; k < 60000u && !found; k += 50u)
		{
			uint32_t now = t + k;
			int64_t px = (now >= change) ? 90000 : 0;
			if (kvm_capsrc_compare_due(&d, now) && cmp1(&d, now, px) == KVM_CAPSRC_ACT_TO_GDI) { found = now - change; }
		}
		SIMP("unit machine that fell to GDI before, static 30 s, silent DXGI, then a change: GDI %u ms after the change (an earlier version: up to 11 s)\n", found);
		CHECK(found > 0);
		CHECK(found <= KVM_CAPSRC_WATCH_MS + KVM_CAPSRC_CONFIRM_MS + 100u);
	}
}

// after the pause the duplication is tried in the shadow. A probe on a static screen ends without a verdict and DXGI is NOT the source afterwards:
// a screen that changes later cannot be shown stale. It is found by the probe that is running when the screen changes.
static void test_m1_scroll_during_probe(uint32_t t0)
{
	kvm_capsrc c;
	uint32_t t = t0, begin, k;
	int act, failedAt = 0, nCmp = 0;
	kvm_capsrc_init(&c, t);
	kvm_capsrc_opened(&c, t);
	fed(&c);
	t += 2000u; (void)cmp1(&c, t, 5000);
	t += 1000u; CHECK_EQ(cmp1(&c, t, 5000), KVM_CAPSRC_ACT_TO_GDI);
	t += 5000u;
	begin = t;
	kvm_capsrc_probe_begin(&c, t);
	kvm_capsrc_opened(&c, t);
	fed(&c);
	for (k = 0; k < 9000u; k += 50u)								// the screen is static: nothing arrives, nothing differs
	{
		if (kvm_capsrc_compare_due(&c, t + k)) { nCmp++; CHECK_EQ(cmp1(&c, t + k, 0), KVM_CAPSRC_ACT_NONE); }
		CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_GDI);
	}
	CHECK(nCmp >= 5 && nCmp <= 7);									// 0.6 s apart three times, then every 2 s
	// the user scrolls 9 s into the probe, DXGI is silent
	for (k = 9000u; k < 15000u; k += 50u)
	{
		CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_GDI);
		if (kvm_capsrc_compare_due(&c, t + k)) { act = cmp1(&c, t + k, 400000); if (act == KVM_CAPSRC_ACT_PROBE_FAILED) { failedAt = (int)(t + k - begin); break; } }
	}
	CHECK(failedAt > 0 && failedAt <= 9000 + 2000 + 200 + 100);	// within one comparison spacing and the confirmation of the change, in the shadow
	CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_GDI);
	SIMP("unit probe on a static screen, scroll at 9 s, DXGI silent: failed in the shadow at %d ms, the source was GDI all the time\n", failedAt);
}

// a difference noted in a probe is no success. Not at the end of the time, and no "reopen" or latch in a probe either.
static void test_m2_probe_success_needs_no_difference(uint32_t t0)
{
	kvm_capsrc c;
	uint32_t t = t0;
	kvm_capsrc_init(&c, t);
	kvm_capsrc_unavailable(&c, t);
	t += 5000u;
	kvm_capsrc_probe_begin(&c, t);
	kvm_capsrc_opened(&c, t);
	full(&c, t + 5u);
	kvm_capsrc_image(&c, t + 100u, 0);
	CHECK_EQ(cmp1(&c, t + 800u, 40000), KVM_CAPSRC_ACT_SUSPECT);	// images arrived and the pictures differ: partly silent
	CHECK_EQ(kvm_capsrc_tick(&c, t + KVM_CAPSRC_PROBE_BUSY_MS - 1u, 1), KVM_CAPSRC_ACT_NONE);
	CHECK_EQ(kvm_capsrc_tick(&c, t + KVM_CAPSRC_PROBE_BUSY_MS, 1), KVM_CAPSRC_ACT_PROBE_FAILED);	// at the end of the time that is no success
	CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_GDI);
	CHECK_EQ(c.fails, 1);
	// a partly silent DXGI: the difference stays. Never REOPEN or SYSTEMATIC in a probe: a failed probe
	t += KVM_CAPSRC_PROBE_BUSY_MS + 10000u;
	kvm_capsrc_probe_begin(&c, t);
	kvm_capsrc_opened(&c, t);
	full(&c, t + 5u);
	kvm_capsrc_image(&c, t + 100u, 0);
	CHECK_EQ(cmp1(&c, t + 800u, 40000), KVM_CAPSRC_ACT_SUSPECT);
	CHECK_EQ(cmp1(&c, t + 1000u, 40000), KVM_CAPSRC_ACT_PROBE_FAILED);
	CHECK_EQ(c.st.systematic, 0);
	CHECK_EQ(c.st.reopens, 0);
	CHECK(!c.systematic);
	CHECK_EQ(c.fails, 2);
	// nothing arrived, a difference was noted once and not confirmed before the time was up: no inconclusive probe, a failed one
	t += 10000u;
	kvm_capsrc_probe_begin(&c, t);
	kvm_capsrc_opened(&c, t);
	fed(&c);
	CHECK_EQ(cmp1(&c, t + 600u, 777), KVM_CAPSRC_ACT_SUSPECT);
	CHECK_EQ(kvm_capsrc_tick(&c, t + KVM_CAPSRC_PROBATION_MS, 1), KVM_CAPSRC_ACT_PROBE_FAILED);
	CHECK_EQ(c.fails, 3);
	// a difference that was noted and then found gone does not count at the end of the time (a busy screen, 1 s minimum not yet reached when the pictures agreed)
	t += 40000u;
	kvm_capsrc_probe_begin(&c, t);
	kvm_capsrc_opened(&c, t);
	full(&c, t + 5u);
	kvm_capsrc_image(&c, t + 10u, 0);
	CHECK_EQ(cmp1(&c, t + 700u, 25000), KVM_CAPSRC_ACT_SUSPECT);
	CHECK_EQ(cmp1(&c, t + 900u, 0), KVM_CAPSRC_ACT_NONE);			// equal, but a probe is no success before 1 s
	kvm_capsrc_image(&c, t + 3000u, 0);
	CHECK_EQ(kvm_capsrc_tick(&c, t + KVM_CAPSRC_PROBE_BUSY_MS, 1), KVM_CAPSRC_ACT_RECOVERED);
}

// a change that GDI shows a few frames before DXGI delivers it is no failure: the comparison is repeated after 0.2 s
static void test_l2_late_frame(uint32_t t0)
{
	kvm_capsrc c;
	uint32_t t = t0;
	kvm_capsrc_init(&c, t);
	kvm_capsrc_unavailable(&c, t);
	t += 5000u;
	kvm_capsrc_probe_begin(&c, t);
	kvm_capsrc_opened(&c, t);
	full(&c, t + 5u);
	kvm_capsrc_image(&c, t + 100u, 0);
	CHECK_EQ(cmp1(&c, t + 1200u, 25000), KVM_CAPSRC_ACT_SUSPECT);	// the screen changed 30 ms ago, the frame is not there yet
	CHECK(!kvm_capsrc_compare_due(&c, t + 1399u));
	CHECK(kvm_capsrc_compare_due(&c, t + 1400u));
	kvm_capsrc_image(&c, t + 1300u, 0);								// the frame came
	CHECK(!kvm_capsrc_compare_due(&c, t + 1400u));					// ... the pictures are compared after the quiet time
	CHECK(kvm_capsrc_compare_due(&c, t + 1900u));
	CHECK_EQ(cmp1(&c, t + 1900u, 0), KVM_CAPSRC_ACT_RECOVERED);
	CHECK_EQ(c.fails, 0);
	// DXGI caught up between the two comparisons without a quiet time
	kvm_capsrc_init(&c, t);
	kvm_capsrc_unavailable(&c, t);
	t += 5000u;
	kvm_capsrc_probe_begin(&c, t);
	kvm_capsrc_opened(&c, t);
	full(&c, t + 5u);
	kvm_capsrc_image(&c, t + 100u, 0);
	CHECK_EQ(cmp1(&c, t + 1200u, 25000), KVM_CAPSRC_ACT_SUSPECT);
	CHECK_EQ(cmp1(&c, t + 1400u, 0), KVM_CAPSRC_ACT_RECOVERED);
}

// the persistent image began as a DXGI full copy, the screen changes once (a click) and then stays static: the reference shows it, however constant the difference stays
static void test_m3a_one_change_after_full_copy(uint32_t t0)
{
	kvm_capsrc c;
	uint32_t t = t0, ms, at = 0;
	kvm_capsrc_init(&c, t);
	kvm_capsrc_opened(&c, t);
	full(&c, t);														// the first frame of a silent duplication (nothing more ever comes)
#ifndef KVM_CAPSRC_OLD_API
	CHECK(kvm_capsrc_ref_usable(&c));
#endif
	for (ms = 0; ms < 300000u && at == 0; ms += 50u)
	{
		int64_t px = (ms >= 3500u) ? 250000 : 0;						// the click at 3.5 s, static afterwards
		if (kvm_capsrc_compare_due(&c, t + ms) && cmp(&c, t + ms, px, px) == KVM_CAPSRC_ACT_TO_GDI) { at = ms; }
	}
	SIMP("unit full copy, one change at 3.5 s, then static: GDI at %u ms (0 = never; an earlier version: the picture stood for good)\n", at);
	CHECK(at > 0 && at <= 3500u + 3000u + 1000u + 50u);
}

// a layered window on a static screen. The persistent image began as a GDI capture (SRCCOPY, without layered windows) and the comparison sees them: a constant difference.
// The proof is GDI against GDI, so a static screen never switches, and a probe is not failed by it.
static void test_m3b_layered_window(uint32_t t0)
{
	kvm_capsrc c;
	uint32_t t = t0, ms;
	int switched = 0;
	kvm_capsrc_init(&c, t);
	kvm_capsrc_opened(&c, t);
	fed(&c);
	for (ms = 0; ms < 3600000u; ms += 50u)
	{
		// persistent image against GDI: 7000 pixels of the window; GDI against the reference: nothing
		if (kvm_capsrc_compare_due(&c, t + ms) && cmp(&c, t + ms, 7000, 0) == KVM_CAPSRC_ACT_TO_GDI) { switched = 1; }
	}
	CHECK(!switched);
	CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_DXGI);
	// the probe after an earlier fall: the window does not fail it
	kvm_capsrc_init(&c, t);
	kvm_capsrc_unavailable(&c, t);
	t += 5000u;
	kvm_capsrc_probe_begin(&c, t);
	kvm_capsrc_opened(&c, t);
	fed(&c);
	CHECK_EQ(cmp(&c, t + 600u, 7000, 0), KVM_CAPSRC_ACT_NONE);		// (before a probe failed at once, then every probe)
	CHECK_EQ(cmp(&c, t + 1200u, 7000, 0), KVM_CAPSRC_ACT_NONE);
	full(&c, t + 1300u);											// the screen changes, DXGI delivers its first frame, then more
	kvm_capsrc_image(&c, t + 1400u, 0);
	CHECK_EQ(cmp(&c, t + 2100u, 0, 0), KVM_CAPSRC_ACT_RECOVERED);	// its own picture agrees with GDI (windows included)
}

// HDR, the latch is set (images arrive, GDI differs by colour for good), the mouse moves: the cursor fields change the number of the differing pixels, the picture does not change
static void test_m3c_hdr_and_mouse(uint32_t t0)
{
	kvm_capsrc c;
	uint32_t t = t0;
	int i, act = KVM_CAPSRC_ACT_NONE;
	kvm_capsrc_init(&c, t);
	kvm_capsrc_opened(&c, t);
	full(&c, t);
	kvm_capsrc_image(&c, t + 100u, 0);
	t += 11000u;
	CHECK_EQ(cmp(&c, t, 1900000, 0), KVM_CAPSRC_ACT_SUSPECT);
	t += 1000u;
	CHECK_EQ(cmp(&c, t, 1900000, 0), KVM_CAPSRC_ACT_REOPEN);
	kvm_capsrc_opened(&c, t);
	full(&c, t + 10u);
	kvm_capsrc_image(&c, t + 200u, 0);
	t += 3000u;
	CHECK_EQ(cmp(&c, t, 1900000, 0), KVM_CAPSRC_ACT_SYSTEMATIC);
	// the mouse moves, nothing else: 28 comparisons (less than the life of the latch)
	for (i = 0; i < 28; i++)
	{
		t += 10000u;
		act = cmp(&c, t, 1900000 - (int64_t)(i % 7) * 11000, 0);
		CHECK(act == KVM_CAPSRC_ACT_NONE);
		CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_DXGI);
	}
	CHECK_EQ(c.st.toGdi, 0);
	// ... and the screen really changes while nothing arrives: silence, found as before
	t += 10000u;
	CHECK_EQ(cmp(&c, t, 1850000, 52000), KVM_CAPSRC_ACT_SUSPECT);
	t += 1000u;
	CHECK_EQ(cmp(&c, t, 1850000, 52000), KVM_CAPSRC_ACT_TO_GDI);
}

#ifndef KVM_CAPSRC_OLD_API
// the reference protocol: asked for at the beginning of a persistent image, outdated by an image, kept while suspect, moved on behind the latch
static void test_reference_protocol(uint32_t t0)
{
	kvm_capsrc c;
	uint32_t t = t0;
	kvm_capsrc_init(&c, t);
	kvm_capsrc_opened(&c, t);
	CHECK(!kvm_capsrc_ref_wanted(&c));								// opened alone is not a persistent image
	CHECK(!kvm_capsrc_ref_usable(&c));
	kvm_capsrc_fed(&c);
	CHECK(kvm_capsrc_ref_wanted(&c));
	CHECK(!kvm_capsrc_ref_usable(&c));								// until the glue has taken it
	kvm_capsrc_ref_done(&c, 1);
	CHECK(!kvm_capsrc_ref_wanted(&c));
	CHECK(kvm_capsrc_ref_usable(&c));
	kvm_capsrc_image(&c, t + 10u, 0);								// an image outdates it
	CHECK(!kvm_capsrc_ref_usable(&c));
	kvm_capsrc_image(&c, t + 20u, 1);								// a full copy after images asks for nothing (nothing would use it: a driver without metadata sends one with every image)
	CHECK(!kvm_capsrc_ref_wanted(&c));
	CHECK(!kvm_capsrc_ref_usable(&c));
	kvm_capsrc_opened(&c, t + 25u);
	kvm_capsrc_image(&c, t + 26u, 1);								// ... but a full copy before any image does
	CHECK(kvm_capsrc_ref_wanted(&c));
	kvm_capsrc_ref_done(&c, 0);										// ... which could not be taken
	CHECK(!kvm_capsrc_ref_usable(&c));
	CHECK(!kvm_capsrc_ref_wanted(&c));
	kvm_capsrc_image(&c, t + 27u, 0);
	CHECK_EQ(kvm_capsrc_compare_result(&c, t + 12000u, 500, 500), KVM_CAPSRC_ACT_SUSPECT);	// images arrived: "differs" branch; no reference needed
	CHECK(!kvm_capsrc_ref_wanted(&c));
	kvm_capsrc_opened(&c, t + 30u);									// a new duplication: the old reference belongs to another picture
	kvm_capsrc_fed(&c);
	kvm_capsrc_ref_done(&c, 1);
	CHECK(kvm_capsrc_ref_usable(&c));
	kvm_capsrc_opened(&c, t + 40u);
	CHECK(!kvm_capsrc_ref_usable(&c));
	CHECK(!kvm_capsrc_ref_wanted(&c));
	// without a usable reference silence is not proven by anything
	kvm_capsrc_opened(&c, t);
	kvm_capsrc_fed(&c);
	kvm_capsrc_ref_done(&c, 0);
	CHECK_EQ(kvm_capsrc_compare_result(&c, t + 2000u, 500, 500), KVM_CAPSRC_ACT_NONE);
	CHECK_EQ(kvm_capsrc_compare_result(&c, t + 3000u, 500, 500), KVM_CAPSRC_ACT_NONE);
	CHECK_EQ(kvm_capsrc_compare_result(&c, t + 4000u, 500, -1), KVM_CAPSRC_ACT_NONE);
	CHECK_EQ(kvm_capsrc_mode(&c), KVM_CAPSRC_MODE_DXGI);
	// a suspect run keeps its reference (the next comparison is against the same picture)
	kvm_capsrc_opened(&c, t);
	fed(&c);
	CHECK_EQ(cmp1(&c, t + 2000u, 9000), KVM_CAPSRC_ACT_SUSPECT);
	CHECK(!kvm_capsrc_ref_wanted(&c));
	CHECK(kvm_capsrc_ref_usable(&c));
	// the latch asks for the reference at the comparison that sets it, and for a new one when images have outdated it
	kvm_capsrc_opened(&c, t);
	full(&c, t);
	kvm_capsrc_image(&c, t + 100u, 0);
	c.afterReinit = 1;
	CHECK_EQ(cmp(&c, t + 11000u, 70, 0), KVM_CAPSRC_ACT_SYSTEMATIC);
	CHECK(kvm_capsrc_ref_usable(&c));								// svc() took the reference at this very comparison
	kvm_capsrc_image(&c, t + 12000u, 0);
	CHECK(!kvm_capsrc_ref_usable(&c));
	CHECK_EQ(kvm_capsrc_compare_result(&c, t + 21000u, 70, 0), KVM_CAPSRC_ACT_NONE);
	CHECK(kvm_capsrc_ref_wanted(&c));								// outdated: the glue keeps this capture as the new reference
}

// A: one failed capture of the reference must not end the proof of silence (nothing else asks for a new one on a silent DXGI)
static void test_a_failed_reference(uint32_t t0)
{
	kvm_capsrc c;
	uint32_t t = t0, ms, at = 0, asked = 0;
	// DXGI mode, closed lid: the first full copy came, the capture of the reference failed
	kvm_capsrc_init(&c, t);
	kvm_capsrc_opened(&c, t);
	kvm_capsrc_image(&c, t, 1);
	kvm_capsrc_ref_done(&c, 0);
	CHECK(!kvm_capsrc_ref_usable(&c));
	for (ms = 0; ms < 300000u && at == 0; ms += 50u)
	{
		const int havRef = kvm_capsrc_ref_usable(&c);
		int64_t px = (ms >= 3500u) ? 250000 : 0;						// one change at 3.5 s, static afterwards
		if (kvm_capsrc_compare_due(&c, t + ms))
		{
			int a = kvm_capsrc_compare_result(&c, t + ms, px, havRef ? px : -1);
			if (kvm_capsrc_ref_wanted(&c)) { if (asked == 0) { asked = ms; } kvm_capsrc_ref_done(&c, 1); }	// the glue adopts the capture of the comparison
			if (a == KVM_CAPSRC_ACT_TO_GDI) { at = ms; }
		}
	}
	SIMP("unit failed reference capture, one change at 3.5 s, static afterwards: a new reference asked for at %u ms, GDI at %u ms (0 = never; before the fix the picture stood for good)\n", asked, at);
	CHECK(asked > 0 && asked <= 2000u);
	CHECK(at > 0 && at <= 6050u);
	// the same when the failure repeats a few times
	kvm_capsrc_init(&c, t);
	kvm_capsrc_opened(&c, t);
	fed(&c);
	kvm_capsrc_fed(&c);
	kvm_capsrc_ref_done(&c, 0);
	CHECK_EQ(kvm_capsrc_compare_result(&c, t + 2000u, 500, -1), KVM_CAPSRC_ACT_NONE);
	CHECK(kvm_capsrc_ref_wanted(&c));
	kvm_capsrc_ref_done(&c, 0);										// failed again
	CHECK_EQ(kvm_capsrc_compare_result(&c, t + 5000u, 500, -1), KVM_CAPSRC_ACT_NONE);
	CHECK(kvm_capsrc_ref_wanted(&c));
	kvm_capsrc_ref_done(&c, 1);
	CHECK_EQ(kvm_capsrc_compare_result(&c, t + 8000u, 500, 500), KVM_CAPSRC_ACT_SUSPECT);
	CHECK_EQ(kvm_capsrc_compare_result(&c, t + 9000u, 500, 500), KVM_CAPSRC_ACT_TO_GDI);
	// a probe in the shadow: no persistent image yet (nothing arrived) and the reference could not be taken; the comparison fails to learn anything and asks for a reference
	kvm_capsrc_init(&c, t);
	kvm_capsrc_unavailable(&c, t);
	t += 5000u;
	kvm_capsrc_probe_begin(&c, t);
	kvm_capsrc_opened(&c, t);
	kvm_capsrc_fed(&c);
	kvm_capsrc_ref_done(&c, 0);
	CHECK_EQ(kvm_capsrc_compare_result(&c, t + 600u, -1, -1), KVM_CAPSRC_ACT_NONE);
	CHECK(kvm_capsrc_ref_wanted(&c));
	kvm_capsrc_ref_done(&c, 1);
	CHECK_EQ(kvm_capsrc_compare_result(&c, t + 1200u, -1, 777), KVM_CAPSRC_ACT_SUSPECT);	// the screen changed: silent, found in the shadow
	CHECK_EQ(kvm_capsrc_compare_result(&c, t + 1400u, -1, 777), KVM_CAPSRC_ACT_PROBE_FAILED);
	// ... and with a persistent image of the shadow whose comparison went through but without a usable reference
	kvm_capsrc_init(&c, t);
	kvm_capsrc_unavailable(&c, t);
	t += 20000u;
	kvm_capsrc_probe_begin(&c, t);
	kvm_capsrc_opened(&c, t);
	kvm_capsrc_fed(&c);
	kvm_capsrc_ref_done(&c, 0);
	CHECK_EQ(kvm_capsrc_compare_result(&c, t + 600u, 5, 5), KVM_CAPSRC_ACT_NONE);			// (a probe with no usable reference and nothing arrived)
	CHECK(kvm_capsrc_ref_wanted(&c));
}
#endif

// ---------------------------------------------------------------------------------------------------------------------------------------
// simulation: the capture loop of the glue on a model
// ---------------------------------------------------------------------------------------------------------------------------------------

typedef struct sim_world
{
	int dxgiMode;				// 0 silent (the closed lid), 1 healthy, 3 silent until switchAt then healthy, 4 delivers a first full copy and is silent afterwards
	uint32_t switchAt;			// ms after the start
	int changing;				// the screen changes (events every 'period' ms)
	uint32_t changeFrom, changeTo, changePeriod, gapFrom, gapTo;	// no change in the gap
	int firstFrame;				// the duplication delivers a first full copy when it is opened (otherwise the glue fills the persistent image from GDI)
	int hdr;					// the image of DXGI differs from GDI by a constant colour difference (5000 pixels)
	int layered;				// a layered window: an image filled from GDI (SRCCOPY) lacks it, the comparison (CAPTUREBLT) shows it: 7000 pixels
	int mouse;					// the mouse moves all the time: the skipped cursor field changes the count of the differing pixels by a few thousand
} sim_world;

typedef struct sim_result
{
	uint32_t firstGdiMs;		// time of the first switch to GDI (0 = never)
	uint32_t recoveredMs;		// time of the first recovery (0 = never)
	uint32_t compares;
	uint32_t probes, probeFails, inconclusive;
	uint32_t staleMs;			// time the viewer saw a stale picture: the screen changed, the source was DXGI and silent
	uint32_t gdiMs;				// time with GDI as the source (a probe in the shadow included)
	uint32_t maxStaleRun;
	uint32_t toGdi, recovered, reopens, switches;
	uint32_t shadowMs;			// time a probe ran in the shadow of GDI
} sim_result;

typedef struct sim_state
{
	long screen;				// serial number of the screen
	int persistentValid, persistentSrc;		// the persistent image (fullBits): holds a picture; src 1: began as a DXGI full copy, 0: as a GDI capture
	long persistentPic, refPic, shadowPic, screenAtOpen;
	int shadowValid, fresh, dxOpen;
} sim_state;

static int64_t sim_jitter(const sim_world *w, int64_t px, uint32_t ms)
{
	if (w->mouse && px > 0) { px -= (int64_t)((ms / 700u) % 5u) * 900; }
	return px;
}

static void sim_recovered(sim_result *r, sim_state *s, uint32_t ms)
{
	r->recovered++; r->switches++;
	if (r->recoveredMs == 0) { r->recoveredMs = ms; }
	s->persistentValid = 0; s->shadowValid = 0;
}

// Model of the glue: every 50 ms one iteration (a timeout of AcquireNextFrame). Pictures are serial numbers.
static sim_result simulate(const sim_world *w, uint32_t t0, uint32_t durationMs)
{
	kvm_capsrc c;
	sim_result r;
	sim_state s;
	uint32_t ms, nextChange = w->changeFrom;
	uint32_t staleRun = 0;
#ifdef KVM_CAPSRC_OLD_API
	const int shadowProbes = 0;
#else
	const int shadowProbes = 1;
#endif

	memset(&r, 0, sizeof(r));
	memset(&s, 0, sizeof(s));
	s.dxOpen = 1;
	kvm_capsrc_init(&c, t0);
	kvm_capsrc_opened(&c, t0);
	// the persistent image begins when the duplication is opened (a first frame, or the glue fills it from GDI after 100 ms)
#define SIM_BEGIN(firstFrameHere) do { \
		if (w->firstFrame && (firstFrameHere)) { kvm_capsrc_image(&c, now, 1); svc(&c); s.persistentSrc = 1; } else { kvm_capsrc_fed(&c); svc(&c); s.persistentSrc = 0; } \
		s.persistentValid = 1; s.persistentPic = s.screen; s.refPic = s.screen; } while (0)
	{
		uint32_t now = t0;
		SIM_BEGIN(w->dxgiMode == 1 || w->dxgiMode == 4);
	}
	for (ms = 0; ms < durationMs; ms += 50u)
	{
		uint32_t now = t0 + ms;
		int healthy = (w->dxgiMode == 1 || (w->dxgiMode == 3 && ms >= w->switchAt));
		int first = (healthy || w->dxgiMode == 4);
		int act;
		// the screen
		if (w->changing && ms >= w->changeFrom && ms < w->changeTo && !(ms >= w->gapFrom && ms < w->gapTo) && ms >= nextChange) { s.screen++; nextChange = ms + w->changePeriod; }

		if (kvm_capsrc_mode(&c) == KVM_CAPSRC_MODE_GDI && !kvm_capsrc_probation(&c))
		{
			if (kvm_capsrc_probe_due(&c, now))
			{
				kvm_capsrc_probe_begin(&c, now);
				s.dxOpen = 1;
				kvm_capsrc_opened(&c, now);
				s.shadowValid = 0; s.fresh = 1; s.screenAtOpen = s.screen;
				if (!shadowProbes) { SIM_BEGIN(first); }			// the persistent image is the source of the probe
			}
			else
			{
				s.persistentPic = s.screen;							// a GDI frame: the viewer has the truth
				r.gdiMs += 50u;
				continue;
			}
		}
		if (shadowProbes && kvm_capsrc_probation(&c))
		{
			// a probe in the shadow: GDI is the source; the duplication is polled, its images go into a private copy
			int gotFull = 0;
			r.gdiMs += 50u; r.shadowMs += 50u;
			s.persistentPic = s.screen;
			if (s.dxOpen)
			{
				if (first && !s.shadowValid && (w->firstFrame || s.screen != s.screenAtOpen)) { kvm_capsrc_image(&c, now, 1); s.shadowValid = 1; s.shadowPic = s.screen; gotFull = 1; }
				else if (healthy && s.shadowValid && s.shadowPic != s.screen) { kvm_capsrc_image(&c, now, 0); s.shadowPic = s.screen; }
				if (s.fresh && !gotFull) { kvm_capsrc_fed(&c); }
				s.fresh = 0;
				if (rwanted(&c)) { s.refPic = s.screen; }
				svc(&c);
			}
			act = kvm_capsrc_tick(&c, now, s.dxOpen);
			if (act == KVM_CAPSRC_ACT_RECOVERED) { sim_recovered(&r, &s, ms); }
			else if (act == KVM_CAPSRC_ACT_INCONCLUSIVE) { r.inconclusive++; s.dxOpen = 0; s.shadowValid = 0; }
			else if (act == KVM_CAPSRC_ACT_PROBE_FAILED) { r.probeFails++; s.dxOpen = 0; s.shadowValid = 0; }
			if (s.dxOpen && kvm_capsrc_probation(&c) && kvm_capsrc_compare_due(&c, now))
			{
				int64_t p = s.shadowValid ? sim_jitter(w, ((s.shadowPic != s.screen) ? 100000 + s.screen : 0) + (w->hdr ? 5000 : 0), ms) : -1;
				int64_t rf = rusable(&c) ? ((s.refPic != s.screen) ? 100000 + s.screen : 0) : -1;
				r.compares++;
				act = raw_cmp(&c, now, p, rf);
				if (rwanted(&c)) { s.refPic = s.screen; }
				svc(&c);
				if (act == KVM_CAPSRC_ACT_PROBE_FAILED) { r.probeFails++; s.dxOpen = 0; s.shadowValid = 0; }
				else if (act == KVM_CAPSRC_ACT_RECOVERED) { sim_recovered(&r, &s, ms); }
			}
			continue;
		}

		// DXGI mode (also a probe)
		if (!s.persistentValid) { SIM_BEGIN(first); }
		// images arrive only when healthy and the screen changed
		if (healthy && s.persistentPic != s.screen) { kvm_capsrc_image(&c, now, 0); s.persistentPic = s.screen; }
		if (s.persistentPic != s.screen) { staleRun += 50u; r.staleMs += 50u; if (staleRun > r.maxStaleRun) { r.maxStaleRun = staleRun; } } else { staleRun = 0; }
		act = kvm_capsrc_tick(&c, now, s.dxOpen);
		if (act == KVM_CAPSRC_ACT_RECOVERED) { sim_recovered(&r, &s, ms); }
		else if (act == KVM_CAPSRC_ACT_PROBE_FAILED) { r.probeFails++; s.persistentPic = s.screen; continue; }
		if (kvm_capsrc_compare_due(&c, now))
		{
			// the comparison: the persistent image against GDI, and GDI against the reference
			int64_t p = s.persistentValid ? ((s.persistentPic != s.screen) ? 100000 + s.screen : 0) : -1;
			int64_t rf = rusable(&c) ? ((s.refPic != s.screen) ? 100000 + s.screen : 0) : -1;
			if (p >= 0)
			{
				if (s.persistentSrc == 1 && w->hdr) { p += 5000; }
				if (s.persistentSrc == 0 && w->layered) { p += 7000; }
				p = sim_jitter(w, p, ms);
			}
			r.compares++;
			act = raw_cmp(&c, now, p, rf);
			if (rwanted(&c)) { s.refPic = s.screen; }
			svc(&c);
			if (act == KVM_CAPSRC_ACT_TO_GDI || act == KVM_CAPSRC_ACT_PROBE_FAILED)
			{
				if (act == KVM_CAPSRC_ACT_TO_GDI) { r.toGdi++; r.switches++; if (r.firstGdiMs == 0) { r.firstGdiMs = ms; } } else { r.probeFails++; }
				s.persistentPic = s.screen;
				s.persistentValid = 0;
			}
			else if (act == KVM_CAPSRC_ACT_REOPEN)
			{
				r.reopens++;
				kvm_capsrc_opened(&c, now);
				SIM_BEGIN(first);
			}
			else if (act == KVM_CAPSRC_ACT_RECOVERED) { sim_recovered(&r, &s, ms); }
		}
	}
	r.probes = c.st.probes;
	r.probeFails = c.st.probeFails;
	return r;
}

static void sim_world_init(sim_world *w) { memset(w, 0, sizeof(*w)); }

static void test_simulation(uint32_t t0)
{
	sim_world w;
	sim_result r;

	// 1. the closed lid: silent from the first frame, the screen changes from 1 s on every 500 ms; the user works for 5 minutes
	sim_world_init(&w);
	w.dxgiMode = 0; w.changing = 1; w.changeFrom = 1000; w.changeTo = 300000; w.changePeriod = 500;
	r = simulate(&w, t0, 300000);
	SIMP("sim lid closed, silent from the start, screen changing 1..300 s: GDI after %u ms, stale picture %u ms in all, longest %u ms, probes %u (failed %u), %u comparisons in 300 s, %u ms in GDI (%u ms of it a probe in the shadow)\n",
		r.firstGdiMs, r.staleMs, r.maxStaleRun, r.probes, r.probeFails, r.compares, r.gdiMs, r.shadowMs);
	CHECK(r.firstGdiMs > 0 && r.firstGdiMs <= 3300u);
	CHECK(r.maxStaleRun <= 3300u);
	CHECK(r.staleMs <= 3300u);													// no more stale pictures at the probes
	CHECK(r.recoveredMs == 0);
	CHECK(r.probeFails == r.probes || r.probeFails + 1 == r.probes);
	CHECK(r.probes <= 9);														// 5, 10, 20, 40, 60, 60 ...: at most 8 in 300 s
	CHECK(r.compares <= 2u + 2u + 2u * 9u + 4u);

	// 2. the same, the screen static for the first 30 s: the first change is found within one period and the confirmation (no fall to GDI before in this process: no faster watch)
	sim_world_init(&w);
	w.dxgiMode = 0; w.changing = 1; w.changeFrom = 30000; w.changeTo = 120000; w.changePeriod = 500;
	r = simulate(&w, t0, 120000);
	SIMP("sim lid closed, screen static for 30 s then changing: GDI at %u ms (change at 30000), stale %u ms, longest %u ms\n", r.firstGdiMs, r.staleMs, r.maxStaleRun);
	CHECK(r.firstGdiMs >= 30000u && r.firstGdiMs <= 30000u + 10000u + 1100u);
	CHECK(r.maxStaleRun <= 11100u);

	// 3. a screen that never changes: DXGI silent, nothing happens in 10 minutes
	sim_world_init(&w);
	w.dxgiMode = 0; w.changing = 0;
	r = simulate(&w, t0, 600000);
	SIMP("sim static screen, DXGI silent, 10 min: GDI %s, %u comparisons (3 fast ones, then 1 per 10 s)\n", r.firstGdiMs ? "YES" : "never", r.compares);
	CHECK(r.firstGdiMs == 0);
	CHECK(r.toGdi == 0);
	CHECK(r.compares <= 3u + 60u);

	// 4. a healthy DXGI on an active screen: no comparison at all (images never stop for 1 s), no switch
	sim_world_init(&w);
	w.dxgiMode = 1; w.firstFrame = 1; w.changing = 1; w.changeFrom = 0; w.changeTo = 600000; w.changePeriod = 100;
	r = simulate(&w, t0, 600000);
	SIMP("sim healthy DXGI, screen changing every 100 ms, 10 min: %u comparisons, GDI %s, stale %u ms\n", r.compares, r.firstGdiMs ? "YES" : "never", r.staleMs);
	CHECK(r.firstGdiMs == 0);
	CHECK(r.compares == 0);

	// 5. a healthy DXGI that reports a change now and then (every 20 s): the cost of the comparisons stays the old one (at most one per 10 s)
	sim_world_init(&w);
	w.dxgiMode = 1; w.firstFrame = 1; w.changing = 1; w.changeFrom = 0; w.changeTo = 600000; w.changePeriod = 20000;
	r = simulate(&w, t0, 600000);
	SIMP("sim healthy DXGI, a change every 20 s, 10 min: %u comparisons (limit 61), GDI %s\n", r.compares, r.firstGdiMs ? "YES" : "never");
	CHECK(r.firstGdiMs == 0);
	CHECK(r.compares <= 61u);

	// 6. the lid is opened after 60 s: the first probe after that finds DXGI working again
	sim_world_init(&w);
	w.dxgiMode = 3; w.switchAt = 60000; w.firstFrame = 1; w.changing = 1; w.changeFrom = 500; w.changeTo = 200000; w.changePeriod = 300;
	r = simulate(&w, t0, 200000);
	SIMP("sim panel on again at 60 s: GDI at %u ms, DXGI again at %u ms (%u ms after the panel came on), probes %u (failed %u), longest stale run %u ms, stale in all %u ms\n",
		r.firstGdiMs, r.recoveredMs, r.recoveredMs - 60000u, r.probes, r.probeFails, r.maxStaleRun, r.staleMs);
	CHECK(r.firstGdiMs > 0);
	CHECK(r.recoveredMs > 60000u && r.recoveredMs <= 60000u + 60000u + 4100u);
	CHECK(r.firstGdiMs <= 3300u);
	CHECK(r.maxStaleRun <= 3300u);

	// 7. HDR: healthy DXGI that differs by a constant colour difference: no GDI mode, the duplication is created again once and the latch is set
	sim_world_init(&w);
	w.dxgiMode = 1; w.firstFrame = 1; w.hdr = 1; w.changing = 1; w.changeFrom = 0; w.changeTo = 10000; w.changePeriod = 100;
	r = simulate(&w, t0, 600000);
	SIMP("sim HDR model (constant difference, screen changing for 10 s, then static), 10 min: GDI %s, comparisons %u\n", r.firstGdiMs ? "YES" : "never", r.compares);
	CHECK(r.firstGdiMs == 0);

	// 8. the screen changes in bursts, DXGI silent for 5 min: tested for stale time
	sim_world_init(&w);
	w.dxgiMode = 0; w.changing = 1; w.changeFrom = 1000; w.changeTo = 8000; w.changePeriod = 200;
	r = simulate(&w, t0, 120000);
	CHECK(r.firstGdiMs > 0 && r.firstGdiMs <= 3300u);

	// 9. lid closed, DXGI silent: the screen changes 1..3 s, then stands still, a probe lands on the static screen, and the user scrolls at 14 s
	sim_world_init(&w);
	w.dxgiMode = 0; w.changing = 1; w.changeFrom = 1000; w.changeTo = 60000; w.changePeriod = 300; w.gapFrom = 3000; w.gapTo = 14000;
	r = simulate(&w, t0, 60000);
	SIMP("sim lid closed, screen 1..3 s, static 3..14 s (a probe lands on it), scroll from 14 s: GDI at %u ms, stale %u ms in all, longest %u ms, probes %u (failed %u, inconclusive %u)\n",
		r.firstGdiMs, r.staleMs, r.maxStaleRun, r.probes, r.probeFails, r.inconclusive);
	CHECK(r.firstGdiMs > 0 && r.firstGdiMs <= 3300u);
	CHECK(r.staleMs <= 3300u);													// before the capture source choice the probe ended undecided with DXGI as the source and the scroll stood still for up to 11 s
	CHECK(r.maxStaleRun <= 3300u);

	// 10. a layered window, healthy DXGI that delivers no first frame within 100 ms (the image is filled from GDI), static screen: not a reason for GDI
	sim_world_init(&w);
	w.dxgiMode = 1; w.firstFrame = 0; w.layered = 1; w.changing = 0;
	r = simulate(&w, t0, 600000);
	SIMP("sim layered window, healthy DXGI, persistent image filled from GDI, static screen, 10 min: GDI %s (at %u ms), comparisons %u\n", r.firstGdiMs ? "YES" : "never", r.firstGdiMs, r.compares);
	CHECK(r.firstGdiMs == 0);
	CHECK(r.toGdi == 0);
	// ... and with a screen that changes for 10 s: images arrive, the pictures differ for good (the window): the old reopen and latch, no GDI
	sim_world_init(&w);
	w.dxgiMode = 1; w.firstFrame = 0; w.layered = 1; w.changing = 1; w.changeFrom = 500; w.changeTo = 10000; w.changePeriod = 200;
	r = simulate(&w, t0, 600000);
	SIMP("sim layered window, healthy DXGI, screen changing for 10 s, 10 min: GDI %s, reopens %u, comparisons %u\n", r.firstGdiMs ? "YES" : "never", r.reopens, r.compares);
	CHECK(r.firstGdiMs == 0);

	// 11. a first full copy, one change (a click) at 3.5 s, then static: DXGI is silent
	sim_world_init(&w);
	w.dxgiMode = 4; w.firstFrame = 1; w.changing = 1; w.changeFrom = 3500; w.changeTo = 3550; w.changePeriod = 100000;
	r = simulate(&w, t0, 300000);
	SIMP("sim full copy then silent, one change at 3.5 s, static for 5 min: GDI at %u ms (0 = never), stale %u ms in all\n", r.firstGdiMs, r.staleMs);
	CHECK(r.firstGdiMs > 0 && r.firstGdiMs <= 3500u + 3000u + 1000u + 100u);
	CHECK(r.staleMs <= 3500u);

	// 12. HDR and a moving mouse: the screen changes for 10 s, then static, the mouse moves for 10 minutes: no switch of the source at all
	sim_world_init(&w);
	w.dxgiMode = 1; w.firstFrame = 1; w.hdr = 1; w.mouse = 1; w.changing = 1; w.changeFrom = 0; w.changeTo = 10000; w.changePeriod = 100;
	r = simulate(&w, t0, 600000);
	SIMP("sim HDR + moving mouse (screen changing for 10 s, then static), 10 min: GDI %s, switches of the source %u, reopens %u, comparisons %u\n", r.firstGdiMs ? "YES" : "never", r.switches, r.reopens, r.compares);
	CHECK(r.switches == 0);
	CHECK(r.firstGdiMs == 0);
}

int main(void)
{
	static const uint32_t starts[] = { 1000u, 0xFFFFF000u, 0xFFFFFF00u };
	size_t i;
	for (i = 0; i < sizeof(starts) / sizeof(starts[0]); i++)
	{
		uint32_t t0 = starts[i];
		test_init(t0);
		test_due(t0);
		test_silent_decision(t0);
		test_one_diff_not_enough(t0);
		test_quiet_after_images(t0);
		test_fed_after_full(t0);
		test_systematic_latch(t0);
		test_systematic_expires_and_resets(t0);
		test_silence_behind_latch(t0);
		test_hdr_static(t0);
		test_probe_backoff(t0);
		test_probe_recovery(t0);
		test_probe_inconclusive(t0);
		test_probe_expiry(t0);
		test_unavailable_and_reset(t0);
		test_m1_watch(t0);
		test_m1_scroll_during_probe(t0);
		test_m2_probe_success_needs_no_difference(t0);
		test_l2_late_frame(t0);
		test_m3a_one_change_after_full_copy(t0);
		test_m3b_layered_window(t0);
		test_m3c_hdr_and_mouse(t0);
#ifndef KVM_CAPSRC_OLD_API
		test_reference_protocol(t0);
		test_a_failed_reference(t0);
#endif
		if (i == 0) { test_static_never_switches(t0); }
		test_simulation(t0);
		gPrint = 0;
	}
	test_gdi_ms_wrap();
	/* the state that the choice of the video encoder looks at: DXGI is "the source" only when it has delivered an image since it was opened */
	CHECK_EQ(kvm_capsrc_known_state(0, 0, 0, 1, 5), 0);			/* the capture has not run yet */
	CHECK_EQ(kvm_capsrc_known_state(1, 0, 0, 0, 0), 0);			/* DXGI is the choice, not open (the first open, the duplication is made again) */
	CHECK_EQ(kvm_capsrc_known_state(1, 0, 0, 1, 0), 0);			/* open and silent (the lid is closed: seconds without an image) */
	CHECK_EQ(kvm_capsrc_known_state(1, 0, 0, 1, 1), 2);			/* the first image */
	CHECK_EQ(kvm_capsrc_known_state(1, 0, 0, 1, 900), 2);
	CHECK_EQ(kvm_capsrc_known_state(1, 0, 0, 0, 7), 0);			/* closed again: the count of the old duplication is of no use */
	CHECK_EQ(kvm_capsrc_known_state(1, 1, 0, 0, 0), 1);			/* DXGI is off for the process */
	CHECK_EQ(kvm_capsrc_known_state(1, 1, 0, 1, 9), 1);
	CHECK_EQ(kvm_capsrc_known_state(1, 0, 1, 1, 9), 1);			/* GDI is the source, DXGI is only probed in its shadow */
	CHECK_EQ(kvm_capsrc_known_state(1, 0, 1, 0, 0), 1);
	printf("%d checks, %d failed\n", gChecks, gFail);
	return gFail == 0 ? 0 : 1;
}
