/*
Tests of kvm_sharpen.h / kvm_sharpen.c: the governor of the sharpening while the H.264 video runs: the signed settle rule , the bucket of the share, the window per packet and a
quarter of the window per iteration. Run with run-kvm-sharpen-tests.sh (gcc in a container: -Werror -pedantic, then ASan + UBSan).
*/

#include "../kvm_sharpen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gFail = 0, gChecks = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); gChecks++; if (_a != _b) { gFail++; printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

// ---------------------------------------------------------------------------------------------------------------------------------------
static void test_settled(uint32_t base)
{
	// the case of the measurement: gA.now = base, the tile was stamped 11 ms LATER (after the encoder). The unsigned comparison said "still for 150 ms".
	CHECK((uint32_t)(base - (base + 11u)) >= 150u);							// (the old rule: wrongly true)
	CHECK(!kvm_sharp_settled(base, base + 11u, 150));						// the fixed one
	CHECK_EQ(kvm_sharp_elapsed_ms(base, base + 11u), -11);
	CHECK_EQ(kvm_sharp_elapsed_ms(base + 11u, base), 11);
	// the ordinary cases
	CHECK(!kvm_sharp_settled(base + 149u, base, 150));
	CHECK(kvm_sharp_settled(base + 150u, base, 150));
	CHECK(kvm_sharp_settled(base + 5000u, base, 150));
	CHECK(!kvm_sharp_settled(base, base, 150));
	CHECK(kvm_sharp_settled(base, base, 0));
	CHECK(!kvm_sharp_settled(base, base + 1u, 0));
	// a change a long time ago, the clock has wrapped in between
	CHECK(kvm_sharp_settled(base + 3000000000u, base, 150) || kvm_sharp_elapsed_ms(base + 3000000000u, base) < 0);	// (more than 24 days: signed arithmetic gives up, that is not a case)
	CHECK(kvm_sharp_settled(0x00000100u, 0xFFFFFF00u, 150));				// the clock wrapped 512 ms ago
	CHECK(!kvm_sharp_settled(0x00000010u, 0xFFFFFFF0u, 150));				// ... 32 ms ago
	CHECK(!kvm_sharp_settled(0xFFFFFFF0u, 0x00000010u, 150));				// the stamp is after the wrap, `now` before it: a stamp in the future
}

// the input thread stamps after `now` was read
static void test_recent(uint32_t base)
{
	CHECK(!kvm_sharp_recent(base, 0, 1500));							// never
	CHECK(kvm_sharp_recent(base + 100u, base, 1500));
	CHECK(kvm_sharp_recent(base + 1499u, base, 1500));
	CHECK(!kvm_sharp_recent(base + 1500u, base, 1500));
	CHECK(kvm_sharp_recent(base, base + 3u, 1500));						// the stamp is 3 ms AFTER `now`: recent (unsigned: about 4.29e9, "not recent")
	CHECK((uint32_t)(base - (base + 3u)) >= 1500u);						// (the old comparison)
	CHECK(kvm_sharp_recent(0xFFFFFFF0u, 0x00000008u, 1500));				// across the wrap, the stamp after `now`
	CHECK(kvm_sharp_recent(0x00000200u, 0xFFFFFF00u, 1500));				// across the wrap, the stamp before `now`
	CHECK(!kvm_sharp_recent(0x00000F00u, 0xFFFFFF00u, 1500));
}

// motion -----------------------------------------------------------------------------------------------------------------------------------
static void test_motion(uint32_t t0)
{
	kvm_sharp g;
	kvm_sharp_init(&g, t0);
	CHECK(!kvm_sharp_active(&g, t0));										// no frame yet
	kvm_sharp_motion(&g, t0 + 10u);
	CHECK(kvm_sharp_active(&g, t0 + 10u));
	CHECK(kvm_sharp_active(&g, t0 + 10u + KVM_SHARP_ACTIVE_MS - 1u));
	CHECK(!kvm_sharp_active(&g, t0 + 10u + KVM_SHARP_ACTIVE_MS));
	kvm_sharp_motion(&g, t0 + 400u);
	CHECK(kvm_sharp_active(&g, t0 + 400u + KVM_SHARP_ACTIVE_MS - 1u));
	kvm_sharp_motion(&g, 0);													// (time 0 is a valid time)
	CHECK(kvm_sharp_active(&g, 0));
	CHECK(kvm_sharp_active(&g, 299));
	CHECK(!kvm_sharp_active(&g, 301));			// (time 0 is kept as 1)
}

// the bucket of the share ----------------------------------------------------------------------------------------------------------------
static void test_begin(uint32_t t0)
{
	kvm_sharp g;
	const uint64_t rate = 375000;											// 3 Mb/s
	const int64_t share = (int64_t)(rate * KVM_SHARP_SHARE_PCT / 100);			// 75 000 B/s
	const int64_t burst = share * KVM_SHARP_BURST_MS / 1000;					// 18 750 B
	int64_t a;
	uint32_t t = t0;
	// idle: no limit at all (the full budget), whatever the window
	kvm_sharp_init(&g, t);
	CHECK_EQ(kvm_sharp_begin(&g, t, rate, 90000, 98304), 98304);
	CHECK_EQ(g.sIdleIters, 1); CHECK_EQ(g.sActiveIters, 0);
	// a movement: the first iteration gets the burst (18 750 B), below a quarter of the window (22 500 B)
	kvm_sharp_motion(&g, t);
	a = kvm_sharp_begin(&g, t, rate, 90000, 98304);
	CHECK_EQ(a, burst);
	CHECK_EQ(g.sActiveIters, 1); CHECK_EQ(g.sIterCapped, 0);
	// ... and what it sent is paid from the bucket
	kvm_sharp_sent(&g, t, 10000);
	CHECK_EQ(g.tokens, burst - 10000); CHECK_EQ(g.sBytesActive, 10000);
	CHECK_EQ(g.iterSent, 10000);
	// the next iteration 34 ms later: the bucket has 8 750 + 34 x 75 = 11 300 B
	t += 34;
	kvm_sharp_motion(&g, t);
	a = kvm_sharp_begin(&g, t, rate, 90000, 98304);
	CHECK_EQ(a, burst - 10000 + 34 * 75);
	CHECK_EQ(g.iterSent, 0);
	// a smaller want wins; a smaller window wins (a quarter of 40 000 B is 10 000 B)
	CHECK_EQ(kvm_sharp_begin(&g, t, rate, 90000, 3000), 3000);
	CHECK_EQ(kvm_sharp_begin(&g, t, rate, 40000, 98304), 10000);
	CHECK_EQ(g.sIterCapped, 1);
	// window 0 (no ACK window): only the bucket
	CHECK_EQ(kvm_sharp_begin(&g, t, rate, 0, 98304), g.tokens);
	// the bucket empty: nothing, and it is counted; debt is not forgiven early
	kvm_sharp_sent(&g, t, 40000);
	CHECK(g.tokens < 0);
	CHECK_EQ(kvm_sharp_begin(&g, t, rate, 90000, 98304), 0);
	CHECK(g.sBucketRefused >= 1);
	// exactly empty is empty: refused and counted
	{
		kvm_sharp h;
		kvm_sharp_init(&h, t); kvm_sharp_motion(&h, t);
		(void)kvm_sharp_begin(&h, t, rate, 0, 1000000);
		kvm_sharp_sent(&h, t, burst);
		CHECK_EQ(h.tokens, 0);
		CHECK_EQ(kvm_sharp_begin(&h, t, rate, 0, 1000000), 0);
		CHECK_EQ(h.sBucketRefused, 1);
	}
	// it refills at the share: 1 s later it is full again (the debt is at most two bursts)
	t += 1000;
	kvm_sharp_motion(&g, t);
	CHECK_EQ(kvm_sharp_begin(&g, t, rate, 90000, 98304), burst);
	// the burst is never above KVM_SHARP_BURST_MS of the share, and never below KVM_SHARP_BURST_MIN
	t += 60000;
	kvm_sharp_motion(&g, t);
	CHECK_EQ(kvm_sharp_begin(&g, t, rate, 0, 1000000), burst);
	kvm_sharp_init(&g, t); kvm_sharp_motion(&g, t);
	CHECK_EQ(kvm_sharp_begin(&g, t, 12500, 0, 1000000), KVM_SHARP_BURST_MIN);		// 100 kb/s: the share is 2 500 B/s, the burst 625 B: the minimum
	// the rate is not known yet: 1 Mb/s is assumed
	kvm_sharp_init(&g, t); kvm_sharp_motion(&g, t);
	CHECK_EQ(kvm_sharp_begin(&g, t, 0, 0, 1000000), KVM_SHARP_BURST_MIN > (int64_t)(KVM_SHARP_DEFAULT_RATE * KVM_SHARP_SHARE_PCT / 100 * KVM_SHARP_BURST_MS / 1000) ? KVM_SHARP_BURST_MIN : (int64_t)(KVM_SHARP_DEFAULT_RATE * KVM_SHARP_SHARE_PCT / 100 * KVM_SHARP_BURST_MS / 1000));
	// ... and its share refills (25 000 B/s): 200 ms after the burst was spent there are 5 000 B again
	{
		kvm_sharp h;
		kvm_sharp_init(&h, t); kvm_sharp_motion(&h, t);
		(void)kvm_sharp_begin(&h, t, 0, 0, 1000000);
		kvm_sharp_sent(&h, t, 8192);
		CHECK_EQ(h.tokens, 0);
		t += 200; kvm_sharp_motion(&h, t);
		CHECK_EQ(kvm_sharp_begin(&h, t, 0, 0, 1000000), 5000);
	}
	// outside a movement the bytes are only counted
	kvm_sharp_init(&g, t);
	(void)kvm_sharp_begin(&g, t, rate, 90000, 98304);
	kvm_sharp_sent(&g, t, 50000);
	CHECK_EQ(g.sBytesIdle, 50000); CHECK_EQ(g.sBytesActive, 0);
	CHECK_EQ(g.tokens, burst);												// (nothing was taken from the bucket: the picture converges at full speed)
	// forced packets pay (below zero too) but nobody refuses them
	kvm_sharp_forced(&g, t, 30000);
	CHECK_EQ(g.sBytesForced, 30000); CHECK_EQ(g.tokens, burst - 30000);
	kvm_sharp_forced(&g, t, 1000000);
	CHECK(g.tokens >= -2 * burst);											// the debt is limited
	// the movement ends: the full budget comes back at once, whatever the debt of the bucket
	CHECK_EQ(kvm_sharp_begin(&g, t, rate, 90000, 98304), 98304);
	// a negative or zero charge is nothing
	{ const int64_t tk = g.tokens; kvm_sharp_sent(&g, t, 0); kvm_sharp_sent(&g, t, -5); kvm_sharp_forced(&g, t, 0); CHECK_EQ(g.tokens, tk); }
}

// the long run: with demand all the time inside a movement the sharpening takes the share, not more
static void test_long_run(uint32_t t0)
{
	kvm_sharp g;
	const uint64_t rates[] = { 125000, 375000, 625000, 6250000 };
	size_t i;
	for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
	{
		uint32_t t = t0;
		uint64_t total = 0;
		const int seconds = 20;
		kvm_sharp_init(&g, t);
		for (; t < t0 + (uint32_t)seconds * 1000u; t += 34)
		{
			int64_t a;
			kvm_sharp_motion(&g, t);
			a = kvm_sharp_begin(&g, t, rates[i], 65536, 98304);
			while (a > 0) { const int64_t pk = 700; kvm_sharp_sent(&g, t, pk); total += (uint64_t)pk; a -= pk; }
		}
		{
			const double share = (double)total / (double)seconds / (double)rates[i];
			CHECK(share <= 0.20 * 1.15 + 0.05);							// the share, plus the first burst and the last packet of each iteration
			CHECK(share >= 0.20 * 0.5 || rates[i] == 6250000 || rates[i] == 625000);		// ... and it is used (at 5 Mb/s and above a quarter of W per iteration may be the smaller limit)
		}
	}
}

// the window ---------------------------------------------------------------------------------------------------------------------------
static void test_window(void)
{
	kvm_sharp g;
	kvm_sharp_init(&g, 0);
	CHECK(kvm_sharp_window_ok(&g, 0, 100000));
	CHECK(kvm_sharp_window_ok(&g, 49999, 100000));
	CHECK(!kvm_sharp_window_ok(&g, 50000, 100000));					// inflight < 0.5 W
	CHECK(!kvm_sharp_window_ok(&g, 400000, 100000));
	CHECK_EQ(g.sWindowRefused, 2);
	CHECK(kvm_sharp_window_ok(&g, 12345678, 0));						// no window: no gate
	CHECK(kvm_sharp_window_ok(&g, 0xFFFFFFFFFFull, 0xFFFFFFFFFFull * 4));	// (no overflow in the product at large values)
}

// how long a movement lasts follows the frame interval; after it the window gate is a quarter of W for a second
static void test_active_ms(uint32_t t0)
{
	kvm_sharp g;
	kvm_sharp_init(&g, t0);
	kvm_sharp_motion(&g, t0);
	// before the interval is known: 300 ms as before
	CHECK(kvm_sharp_active(&g, t0 + 299));
	CHECK(!kvm_sharp_active(&g, t0 + 300));
	// High (16 ms): max(48, 100) = 100 ms
	kvm_sharp_set_interval(&g, 16);
	CHECK_EQ(g.activeMs, 100);
	CHECK(kvm_sharp_active(&g, t0 + 99));
	CHECK(!kvm_sharp_active(&g, t0 + 100));
	kvm_sharp_set_interval(&g, 33); CHECK_EQ(g.activeMs, 100);						// 99 -> 100
	kvm_sharp_set_interval(&g, 34); CHECK_EQ(g.activeMs, 102);
	kvm_sharp_set_interval(&g, 50); CHECK_EQ(g.activeMs, 150);
	kvm_sharp_set_interval(&g, 100); CHECK_EQ(g.activeMs, 300);
	kvm_sharp_set_interval(&g, 250); CHECK_EQ(g.activeMs, 750);
	kvm_sharp_set_interval(&g, 0); CHECK_EQ(g.activeMs, 100);						// not known: the floor
	kvm_sharp_set_interval(&g, 1000000u); CHECK_EQ(g.activeMs, 3000);				// no overflow
	// the bucket of the share and the iteration cap follow it: a movement that ended 120 ms ago is no movement at 16 ms
	kvm_sharp_set_interval(&g, 16);
	CHECK(kvm_sharp_begin(&g, t0 + 120, 625000, 65536, 98304) == 98304);			// idle: the whole budget
	CHECK_EQ(g.sIdleIters, 1);
	kvm_sharp_set_interval(&g, 100);
	CHECK(kvm_sharp_begin(&g, t0 + 120, 625000, 65536, 98304) < 98304);			// 300 ms: still a movement
	CHECK_EQ(g.sActiveIters, 1);
}

static void test_post_window(uint32_t t0)
{
	kvm_sharp g;
	kvm_sharp_init(&g, t0);
	kvm_sharp_set_interval(&g, 16);
	// never a movement: the gate is the half
	CHECK(!kvm_sharp_post_motion(&g, t0));
	CHECK(kvm_sharp_window_ok_at(&g, t0, 49999, 100000));
	CHECK(!kvm_sharp_window_ok_at(&g, t0, 50000, 100000));
	// the movement and the second after it: a quarter
	kvm_sharp_motion(&g, t0 + 1000);
	CHECK(kvm_sharp_post_motion(&g, t0 + 1000));
	CHECK(kvm_sharp_window_ok_at(&g, t0 + 1000, 24999, 100000));
	CHECK(!kvm_sharp_window_ok_at(&g, t0 + 1000, 25000, 100000));
	CHECK(!kvm_sharp_window_ok_at(&g, t0 + 1500, 25000, 100000));
	CHECK(kvm_sharp_window_ok_at(&g, t0 + 1999, 24999, 100000));
	CHECK(!kvm_sharp_window_ok_at(&g, t0 + 1999, 25000, 100000));
	CHECK(kvm_sharp_post_motion(&g, t0 + 1999));
	// a second after: the half again
	CHECK(!kvm_sharp_post_motion(&g, t0 + 2000));
	CHECK(kvm_sharp_window_ok_at(&g, t0 + 2000, 49999, 100000));
	CHECK(!kvm_sharp_window_ok_at(&g, t0 + 2000, 50000, 100000));
	// the refusals are counted, no window = no gate, no overflow
	CHECK_EQ(g.sWindowRefused, 5);
	CHECK(kvm_sharp_window_ok_at(&g, t0 + 1000, 12345678, 0));
	CHECK(kvm_sharp_window_ok_at(&g, t0 + 3000, 0xFFFFFFFFFFull, 0xFFFFFFFFFFull * 4));
	// a movement later moves the second on
	kvm_sharp_motion(&g, t0 + 5000);
	CHECK(kvm_sharp_post_motion(&g, t0 + 5500));
	CHECK(!kvm_sharp_window_ok_at(&g, t0 + 5500, 25000, 100000));
}

int main(void)
{
	test_settled(1000);
	test_settled(0xFFFFFF00u);
	test_settled(0);
	test_recent(1000);
	test_recent(0xFFFFFF00u);
	test_motion(1000);
	test_motion(0xFFFFFE00u);
	test_begin(1000);
	test_begin(0xFFFFF000u);
	test_long_run(1000);
	test_long_run(0xFFFF0000u);
	test_window();
	test_active_ms(1000);
	test_active_ms(0xFFFFFF00u);
	test_post_window(1000);
	test_post_window(0xFFFFFC00u);
	printf("%d checks, %d failed\n", gChecks, gFail);
	return gFail == 0 ? 0 : 1;
}
