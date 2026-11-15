/*
Tests of kvm_video_rc.c (target R, the QP regulator, the byte bucket) and a simulation of the video path on a link: the real kvm_stream (ACK window, rate measurement, markers), the real
controller and plan of kvm_video_core.c, the real rate control, and a model of the content (frame sizes from the encoder probe numbers, see frame_bytes()). Time is injected; the output is
the table of the report. Run with run-kvm-video-rc-tests.sh (gcc in a container: -Werror -pedantic, then ASan + UBSan).
*/

#include "../kvm_video_rc.h"
#include "../kvm_video_core.h"
#include "../kvm_pace.h"
#include "../kvm_stream.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gFail = 0, gChecks = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); gChecks++; if (_a != _b) { gFail++; printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

// ---------------------------------------------------------------------------------------------------------------------------------------
// units
// ---------------------------------------------------------------------------------------------------------------------------------------

static void test_target(void)
{
	// 2 Mb/s without budget and rate; a budget alone; a rate alone; both: the smaller
	CHECK_EQ(kvm_vrc_target(0, 0), 250000);
	CHECK_EQ(kvm_vrc_target(2000, 0), 200000);						// 2 Mb/s budget: 0.8 x 250 000 B/s
	CHECK_EQ(kvm_vrc_target(0, 850000), 850000);					// a capacity estimate alone
	CHECK_EQ(kvm_vrc_target(2000, 5312500), 200000);					// budget 2 Mb/s on a 50 Mb/s link
	CHECK_EQ(kvm_vrc_target(10000, 106250), 106250);					// 10 Mb/s budget, 1 Mb/s link: the link
	CHECK_EQ(kvm_vrc_target(0, 1000), KVM_VRC_MIN_RATE);			// never below 100 kb/s
	CHECK_EQ(kvm_vrc_target(1, 0), KVM_VRC_MIN_RATE);
}

static void feed(kvm_vrc *r, uint32_t *t, uint32_t bytesPerWindow, int frames, int windows)
{
	int w, k;
	for (w = 0; w < windows; w++)
	{
		for (k = 0; k < frames; k++)
		{
			*t += KVM_VRC_WINDOW_MS / (uint32_t)frames;
			kvm_vrc_frame(r, *t, bytesPerWindow / (uint32_t)frames, 28, 0, 5000);
		}
		(void)kvm_vrc_tick(r, *t + 1);
		*t += 1;
	}
}

static void test_regulator(uint32_t t0)
{
	kvm_vrc r;
	uint32_t t = t0;
	int q;
	kvm_vrc_init(&r, 28, t);
	kvm_vrc_set_target(&r, 100000, t);								// R = 800 kb/s: 50 000 B per window
	CHECK_EQ(r.qp, 28);
	// above 110 %: +2 per window up to the ceiling 38
	feed(&r, &t, 60000, 10, 1); CHECK_EQ(r.qp, 30);
	feed(&r, &t, 60000, 10, 1); CHECK_EQ(r.qp, 32);
	feed(&r, &t, 60000, 10, 3); CHECK_EQ(r.qp, 38);
	feed(&r, &t, 600000, 10, 3); CHECK_EQ(r.qp, KVM_VIDEO_QP_MAX);	// the ceiling holds
	// the ceiling holds from an odd QP too (+2 must not step over it)
	kvm_vrc_init(&r, 29, t); kvm_vrc_set_target(&r, 100000, t);
	feed(&r, &t, 60000, 10, 4); CHECK_EQ(r.qp, 37); feed(&r, &t, 60000, 10, 1); CHECK_EQ(r.qp, KVM_VIDEO_QP_MAX); feed(&r, &t, 60000, 10, 3); CHECK_EQ(r.qp, KVM_VIDEO_QP_MAX);
	// exactly 110 % is not over
	kvm_vrc_init(&r, 30, t); kvm_vrc_set_target(&r, 100000, t);
	feed(&r, &t, 54000, 10, 1); CHECK_EQ(r.qp, 30);
	// below 70 %: one step down after two windows in a row, never below the floor
	kvm_vrc_init(&r, 30, t); kvm_vrc_set_target(&r, 100000, t);
	feed(&r, &t, 30000, 10, 1); CHECK_EQ(r.qp, 30);					// the first quiet window: nothing yet (hysteresis)
	feed(&r, &t, 30000, 10, 1); CHECK_EQ(r.qp, 29);
	feed(&r, &t, 30000, 10, 2); CHECK_EQ(r.qp, 28);
	feed(&r, &t, 30000, 10, 20); CHECK_EQ(r.qp, KVM_VIDEO_QP_MIN);
	feed(&r, &t, 30000, 10, 4); CHECK_EQ(r.qp, KVM_VIDEO_QP_MIN);
	// no refusal at all (the encode time limits the stream, not R): below 80 % is quiet too, 80 % is not; a refusal of the window in the window of the measurement is not
	kvm_vrc_init(&r, 38, t); kvm_vrc_set_target(&r, 100000, t);
	feed(&r, &t, 39000, 10, 1); CHECK_EQ(r.qp, 38);					// 78 %: the first quiet window
	feed(&r, &t, 39000, 10, 1); CHECK_EQ(r.qp, 37);
	feed(&r, &t, 39000, 10, 2); CHECK_EQ(r.qp, 36);
	kvm_vrc_init(&r, 38, t); kvm_vrc_set_target(&r, 100000, t);
	feed(&r, &t, 40600, 10, 6); CHECK_EQ(r.qp, 38);					// 81 %: not quiet
	kvm_vrc_init(&r, 38, t); kvm_vrc_set_target(&r, 100000, t);
	{
		int w;
		for (w = 0; w < 6; w++) { kvm_vrc_window_refused(&r); feed(&r, &t, 39000, 10, 1); }	// the window of the viewer refused frames: R is not the only limit
		CHECK_EQ(r.qp, 38);
		CHECK_EQ(r.sHeld, 6);
		for (w = 0; w < 6; w++) { feed(&r, &t, 30000, 10, 1); }		// 60 % is quiet whatever the window did before (the 70 % rule is unchanged)
		CHECK(r.qp < 38);
	}
	kvm_vrc_init(&r, 38, t); kvm_vrc_set_target(&r, 100000, t);
	r.winDenied = 1; feed(&r, &t, 39000, 10, 1); CHECK_EQ(r.qp, 38);	// (the feed closes the window; a bucket refusal is cleared with it, so the run starts again)
	// a window in between that is neither under nor over breaks the run
	kvm_vrc_init(&r, 30, t); kvm_vrc_set_target(&r, 100000, t);
	feed(&r, &t, 30000, 10, 1); feed(&r, &t, 45000, 10, 1); feed(&r, &t, 30000, 10, 1); CHECK_EQ(r.qp, 30);
	// fewer than 3 frames say nothing; an idle window neither lowers the QP nor breaks the run
	kvm_vrc_init(&r, 30, t); kvm_vrc_set_target(&r, 100000, t);
	feed(&r, &t, 30000, 2, 6); CHECK_EQ(r.qp, 30);
	feed(&r, &t, 30000, 10, 1); (void)kvm_vrc_tick(&r, t + 600); t += 600; feed(&r, &t, 30000, 10, 1); CHECK_EQ(r.qp, 29);
	// the key frames are not counted by the regulator
	kvm_vrc_init(&r, 28, t); kvm_vrc_set_target(&r, 100000, t);
	{
		int k;
		for (k = 0; k < 5; k++) { t += 100; kvm_vrc_frame(&r, t, 8000, 28, 0, 5000); }
		kvm_vrc_frame(&r, t, 250000, 28, 1, 20000);
		(void)kvm_vrc_tick(&r, t + 1); t += 1;
		CHECK_EQ(r.qp, 28);
	}
	// frames refused by the bucket while the stream is above 90 % of R: the content wants more than R: up
	kvm_vrc_init(&r, 28, t); kvm_vrc_set_target(&r, 100000, t);
	for (q = 0; q < 10; q++) { t += 50; kvm_vrc_frame(&r, t, 5000, 28, 0, 5000); }	// 50 000 B in 500 ms = 100 % of R, none over 110 %
	(void)kvm_vrc_may_encode(&r, t, 0);
	r.tokens = 0; r.winDenied = 1;
	(void)kvm_vrc_tick(&r, t + 1); t += 1;
	CHECK_EQ(r.qp, 30);
	// a refusal with the stream far below R (a refusal after a key frame): no change
	kvm_vrc_init(&r, 28, t); kvm_vrc_set_target(&r, 100000, t);
	t += 100; kvm_vrc_frame(&r, t, 2000, 28, 0, 5000); r.winDenied = 1;
	(void)kvm_vrc_tick(&r, t + 500); t += 501;
	CHECK_EQ(r.qp, 28);
	// the time wraps
	kvm_vrc_init(&r, 28, 0xFFFFFF00u); kvm_vrc_set_target(&r, 100000, 0xFFFFFF00u);
	{
		uint32_t tw = 0xFFFFFF00u;
		feed(&r, &tw, 60000, 10, 2); CHECK_EQ(r.qp, 32);
	}
}

static void test_capacity(uint32_t t0)
{
	kvm_vrc r;
	uint32_t t = t0;
	int i;
	kvm_vrc_init(&r, 28, t);
	// nothing known: 2 Mb/s
	kvm_vrc_update(&r, t, 0, 0, 0); CHECK_EQ(r.rate, 250000);
	// the link is the limit: 0.85 x the rate, at once (also downwards)
	t += 100; kvm_vrc_update(&r, t, 0, 100000, 1); CHECK_EQ(r.rate, 85000);
	t += 100; kvm_vrc_update(&r, t, 0, 40000, 1); CHECK_EQ(r.rate, 34000);
	// not limited by the link and the video is not held back by its bucket: the estimate stays (it is not 0.85 of what was sent)
	t += 100; kvm_vrc_update(&r, t, 0, 30000, 0); CHECK_EQ(r.rate, 34000);
	// held by its own bucket and the link is not the limit: probing, 25 % per second
	for (i = 0; i < 10; i++) { t += 100; (void)kvm_vrc_may_encode(&r, t, 0); r.tokens = 0; (void)kvm_vrc_may_encode(&r, t, 0); kvm_vrc_update(&r, t, 0, 30000, 0); }
	CHECK(r.rate > 34000 * 1.2 && r.rate < 34000 * 1.4);			// 1 s: x 1.25
	// the budget caps it
	for (i = 0; i < 100; i++) { t += 100; (void)kvm_vrc_may_encode(&r, t, 0); r.tokens = 0; (void)kvm_vrc_may_encode(&r, t, 0); kvm_vrc_update(&r, t, 1000, 30000, 0); }
	CHECK_EQ(r.rate, 100000);											// 0.8 x 1 Mb/s
	// the link becomes the limit again
	t += 100; kvm_vrc_update(&r, t, 1000, 50000, 1); CHECK_EQ(r.rate, 42500);
	// not limited by the link, more delivered than the estimate: the estimate follows (the link takes at least what it delivered)
	t += 100; kvm_vrc_update(&r, t, 0, 200000, 0); CHECK(r.rate >= 170000 && r.rate <= 180000);
	// wrap
	kvm_vrc_init(&r, 28, 0xFFFFFFF0u); kvm_vrc_update(&r, 0xFFFFFFF0u, 0, 100000, 1); kvm_vrc_update(&r, 0xFFFFFFF0u + 100, 0, 100000, 1); CHECK_EQ(r.rate, 85000);
}

static void test_idr_qp(void)
{
	kvm_vrc r;
	int rate, qp;
	kvm_vrc_init(&r, 28, 0);
	// a fast link: the key frame has the QP of the P frames (never better than the frames after it, floor 26)
	kvm_vrc_set_target(&r, 2000000, 0);
	CHECK_EQ(kvm_vrc_qp(&r, 0), 28);
	CHECK_EQ(kvm_vrc_qp(&r, 1), 28);
	r.qp = 26; CHECK_EQ(kvm_vrc_qp(&r, 1), 26);
	r.qp = 38; CHECK_EQ(kvm_vrc_qp(&r, 1), 38);
	r.qp = 34; CHECK_EQ(kvm_vrc_qp(&r, 1), 34);
	CHECK_EQ(kvm_vrc_qp(&r, 0), 34);
	r.qp = 50; CHECK_EQ(kvm_vrc_qp(&r, 1), KVM_VRC_IDR_QP_MAX);					// (defensive: the clamps hold whatever the P QP is)
	r.qp = 10; CHECK_EQ(kvm_vrc_qp(&r, 1), 26);											// the floor of the IDR QP is the floor of the P frames
	r.qp = 34;
	// 4 Mb/s (R = 500 000 B/s, 150 000 B in 0.3 s) and the expected key frame of 200 000 B at QP 28: x 0.94^5 = 146 700 B fits, x 0.94^4 = 156 100 B does not: QP 33
	kvm_vrc_init(&r, 28, 0); kvm_vrc_set_target(&r, 500000, 0);
	CHECK_EQ(kvm_vrc_qp(&r, 1), 33);
	// 2 Mb/s (R = 250 000: 75 000 B), 1 Mb/s and 0.5 Mb/s: not reachable even at the top of the range
	kvm_vrc_set_target(&r, 250000, 0); CHECK_EQ(kvm_vrc_qp(&r, 1), 38);
	kvm_vrc_set_target(&r, 106250, 0); CHECK_EQ(kvm_vrc_qp(&r, 1), 38);
	kvm_vrc_set_target(&r, 53125, 0); CHECK_EQ(kvm_vrc_qp(&r, 1), 38);
	// a measured key frame replaces the expectation: 120 000 B at QP 30 on 300 000 B/s (90 000 B): x 0.94^5 = 88 000 B fits, x 0.94^4 = 93 700 B does not: QP 35
	kvm_vrc_init(&r, 28, 0); kvm_vrc_set_target(&r, 300000, 0);
	kvm_vrc_frame(&r, 1, 120000, 30, 1, 20000);
	CHECK_EQ(r.lastIdrBytes, 120000); CHECK_EQ(r.lastIdrQp, 30);
	CHECK_EQ(kvm_vrc_qp(&r, 1), 35);
	// ... a bigger P QP does not lower it (max of the two rules) and a P QP above it wins
	r.qp = 36; CHECK_EQ(kvm_vrc_qp(&r, 1), 36); r.qp = 26; CHECK_EQ(kvm_vrc_qp(&r, 1), 35);
	// a small key frame (a screen that is mostly one colour) on a slow link: it fits at once, the QP of the P frames decides (and it is never lower than that)
	kvm_vrc_init(&r, 28, 0); kvm_vrc_set_target(&r, 53125, 0); kvm_vrc_frame(&r, 1, 10000, 28, 1, 20000);
	CHECK_EQ(kvm_vrc_qp(&r, 1), 28);
	// an unknown picture size scales the expectation: a quarter of the pixels, a quarter of the bytes
	kvm_vrc_init(&r, 28, 0); kvm_vrc_set_picture(&r, 960u * 544u);
	CHECK(r.lastIdrBytes > 49000 && r.lastIdrBytes < 51000);
	kvm_vrc_frame(&r, 1, 100000, 28, 1, 20000); kvm_vrc_set_picture(&r, 3840u * 2160u); CHECK_EQ(r.lastIdrBytes, 100000);	// a measured one stays
	// for every rate and P QP: in the range, never below the P QP, never above the top, not lower on a slower link
	for (qp = KVM_VIDEO_QP_MIN; qp <= KVM_VIDEO_QP_MAX; qp++)
	{
		int last = 0;
		for (rate = 3000000; rate >= 12500; rate = rate * 3 / 4)
		{
			int q;
			kvm_vrc_init(&r, 28, 0); r.qp = qp; kvm_vrc_set_target(&r, (uint64_t)rate, 0);
			q = kvm_vrc_qp(&r, 1);
			CHECK(q >= KVM_VRC_IDR_QP_MIN && q <= KVM_VRC_IDR_QP_MAX);
			CHECK(q >= qp);
			CHECK(q >= last);
			last = q;
		}
	}
	// the key frame fits in 0.3 s of R whenever some QP of the range makes it fit
	for (rate = 3000000; rate >= 12500; rate = rate * 3 / 4)
	{
		kvm_vrc_init(&r, 28, 0); kvm_vrc_set_target(&r, (uint64_t)rate, 0);
		{
			const int q = kvm_vrc_qp(&r, 1);
			double b = (double)r.lastIdrBytes;
			int k;
			for (k = r.lastIdrQp; k < q; k++) { b *= 0.94; }
			if (q < KVM_VRC_IDR_QP_MAX) { CHECK(b <= (double)rate * 0.3 * 1.01); }
		}
	}
}

// the key frame is sized from R only while the link is the limit (the window of the viewer refused a video frame in at least half of the last 3 s) or the viewer has a
// budget; on a link that is not full it is sized from the ceiling of the capacity estimate
static uint32_t held_windows(kvm_vrc *r, uint32_t t, int windows, int every)	// `windows` regulator windows, a refusal in every `every`-th
{
	int w;
	for (w = 0; w < windows; w++)
	{
		if (every > 0 && w % every == 0) { kvm_vrc_window_refused(r); }
		t += KVM_VRC_WINDOW_MS; (void)kvm_vrc_tick(r, t);
	}
	return t;
}

static void test_idr_unsaturated(uint32_t t0)
{
	kvm_vrc r;
	uint32_t t = t0;
	kvm_vrc_init(&r, 28, t);
	CHECK_EQ(r.idrLimited, 1);												// nothing known: the cautious rule
	// 1.6 Mb/s delivered, no refusal: without evidence the key frame is sized from R = 0.85 x rate = 170 000 B/s (51 000 B in 0.3 s: the top)
	t += 100; kvm_vrc_update(&r, t, 0, 200000, 0);
	CHECK_EQ(r.idrLimited, 0);
	CHECK_EQ(r.rate, 250000);												// (a rate that is no evidence of the link, a lower bound, does not set the first estimate: it is the default 2 Mb/s and the rate only raises it)
	t += 1000; kvm_vrc_update(&r, t, 0, 400000, 0);
	CHECK_EQ(r.rate, 340000);												// 0.85 x 400 000 B/s: raised by what was delivered
	kvm_vrc_init(&r, 28, t);
	t += 100; kvm_vrc_update(&r, t, 0, 200000, 1);							// the window held the sender back: that is what the link takes, the estimate is 0.85 x rate
	CHECK_EQ(r.rate, 170000);
	CHECK_EQ(kvm_vrc_qp(&r, 1), 38);
	// the viewer acknowledged the last key frame: 150 000 B written at t - 300, acknowledged at t, held 0 ms, round trip 10 ms: 150 000 / 290 ms = 517 241 B/s; 0.85 x it = 439 655 B/s: 131 896 B
	// in 0.3 s: 200 000 B at QP 28 shrink to 125 600 at QP 35 (QP 34: 133 600)
	t += 300; kvm_vrc_idr_acked(&r, t, 150000, t - 300, t, 0, 10);
	CHECK_EQ(r.cIdr, 517241);
	kvm_vrc_update(&r, t, 0, 200000, 0);
	CHECK_EQ(kvm_vrc_qp(&r, 1), 35);
	// a refusal of the window takes the evidence away at once
	kvm_vrc_window_refused(&r);
	CHECK_EQ(r.cIdr, 0);
	kvm_vrc_update(&r, t, 0, 200000, 1);	// (the next iteration: the first key frame proof, R was lifted to the proof of the key frame, which the refusal took away as well)
	CHECK_EQ(kvm_vrc_qp(&r, 1), 38);
	// ... and it is valid for KVM_VRC_CIDR_MS
	kvm_vrc_idr_acked(&r, t, 150000, t - 300, t, 0, 10);
	t += KVM_VRC_CIDR_MS - 1; kvm_vrc_update(&r, t, 0, 200000, 0);
	CHECK_EQ(r.cIdr, 517241); CHECK_EQ(kvm_vrc_qp(&r, 1), 35);
	t += 2; kvm_vrc_update(&r, t, 0, 200000, 0);
	CHECK_EQ(r.cIdr, 0); CHECK_EQ(kvm_vrc_qp(&r, 1), 38);
	// a slow link (1 Mb/s, 100 000 B/s delivered): the last key frame of 113 000 B took 1 s: 114 141 B/s, 0.85 x it = 97 020 B/s, a bit above R = 85 000 B/s: 29 106 B in 0.3 s, the top, as before
	kvm_vrc_init(&r, 28, t);
	t += 100; kvm_vrc_update(&r, t, 0, 100000, 0);
	t += 1000; kvm_vrc_idr_acked(&r, t, 113000, t - 1000, t, 0, 10);
	kvm_vrc_update(&r, t, 0, 100000, 0);
	CHECK_EQ(kvm_vrc_qp(&r, 1), 38);
	CHECK(r.cIdr > 100000 && r.cIdr < 130000);
	// a fast link (3 MB/s delivered): 175 000 B acknowledged 20 ms after they were written (5 ms of round trip): the evidence is capped by twice the ceiling of the estimate, the key frame keeps the QP of the P frames
	kvm_vrc_init(&r, 28, t); r.qp = 28;
	t += 100; kvm_vrc_update(&r, t, 0, 3000000, 0);
	kvm_vrc_idr_acked(&r, t, 175000, t - 20, t, 0, 5);
	kvm_vrc_update(&r, t, 0, 3000000, 0);
	CHECK_EQ(kvm_vrc_qp(&r, 1), 28);
	// the cap: with 200 000 B/s delivered the ceiling is 500 000 B/s, twice that is the cap of the evidence; a key frame that went through in 20 ms is sized for 1 MB/s (300 000 B in 0.3 s)
	kvm_vrc_init(&r, 28, t); r.qp = 28;
	t += 100; kvm_vrc_update(&r, t, 0, 200000, 0);
	kvm_vrc_idr_acked(&r, t, 175000, t - 20, t, 0, 5);
	kvm_vrc_update(&r, t, 0, 200000, 0);
	CHECK_EQ(r.idrCap, 1000000);
	CHECK_EQ(kvm_vrc_qp(&r, 1), 28);
	// ... and the cap bites with a big key frame: 600 000 B at QP 28 do not fit 300 000 B (the cap) before the top of the range, they would fit 9.9 MB/s x 0.3 s at QP 28
	r.lastIdrBytes = 600000; r.lastIdrQp = 28;
	CHECK_EQ(kvm_vrc_qp(&r, 1), 38);
	// the evidence never makes the key frame smaller than R allows: a link that took 100 000 B/s of the last one (0.85 x it = 85 000 B/s, below R = 170 000 B/s): R decides, 51 000 B in 0.3 s, a key frame of 40 000 B at QP 28 fits
	kvm_vrc_init(&r, 28, t); r.qp = 28;
	t += 100; kvm_vrc_update(&r, t, 0, 200000, 0);
	r.lastIdrBytes = 40000; r.lastIdrQp = 28;
	kvm_vrc_idr_acked(&r, t, 100000, t - 1010, t, 0, 10);
	CHECK_EQ(r.cIdr, 100000);
	CHECK_EQ(kvm_vrc_qp(&r, 1), 28);
	// the time can come out zero or negative (clock jitter): at least 10 ms; no bytes: nothing
	kvm_vrc_idr_acked(&r, t, 175000, t, t, 0, 5);
	CHECK_EQ(r.cIdr, 17500000);
	kvm_vrc_idr_acked(&r, t, 175000, t + 5, t, 30, 20);
	CHECK_EQ(r.cIdr, 17500000);
	r.cIdr = 0; kvm_vrc_idr_acked(&r, t, 0, t - 20, t, 0, 5);
	CHECK_EQ(r.cIdr, 0);
	// a link that refuses frames in at least 6 of the last 40 windows is full: R decides, whatever the key frame did
	kvm_vrc_init(&r, 28, t);
	t += 100; kvm_vrc_update(&r, t, 0, 200000, 0);
	kvm_vrc_idr_acked(&r, t, 150000, t - 300, t, 0, 10);
	t = held_windows(&r, t, 6, 1); kvm_vrc_idr_acked(&r, t, 150000, t - 300, t, 0, 10);	// (the refusals cleared the evidence, a new acknowledgement brings it back)
	kvm_vrc_update(&r, t, 0, 200000, 1);
	CHECK_EQ(r.idrLimited, 1);
	CHECK_EQ(kvm_vrc_qp(&r, 1), 38);
	// five refusals are not enough; quiet windows push them out of the 20 s that are remembered
	kvm_vrc_init(&r, 28, t);
	t += 100; kvm_vrc_update(&r, t, 0, 200000, 0);
	t = held_windows(&r, t, 5, 1); kvm_vrc_update(&r, t, 0, 200000, 1);
	CHECK_EQ(r.idrLimited, 0);
	t = held_windows(&r, t, 1, 1); kvm_vrc_update(&r, t, 0, 200000, 1);
	CHECK_EQ(r.idrLimited, 1);
	t = held_windows(&r, t, KVM_VRC_IDR_HISTORY_WINDOWS - 6, 0); kvm_vrc_update(&r, t, 0, 200000, 0);
	CHECK_EQ(r.idrLimited, 1);							// (the oldest of the 6 refusals is still inside the 40 windows)
	t = held_windows(&r, t, 1, 0); kvm_vrc_update(&r, t, 0, 200000, 0);
	CHECK_EQ(r.idrLimited, 0);
	// a refusal now and then (one window in eight, a fast link with flow control) is not a full link; one in three is
	kvm_vrc_init(&r, 28, t);
	t += 100; kvm_vrc_update(&r, t, 0, 200000, 0);
	t = held_windows(&r, t, 80, 8); kvm_vrc_update(&r, t, 0, 200000, 1);
	CHECK_EQ(r.idrLimited, 0);
	kvm_vrc_init(&r, 28, t);
	t += 100; kvm_vrc_update(&r, t, 0, 200000, 0);
	t = held_windows(&r, t, 80, 3); kvm_vrc_update(&r, t, 0, 200000, 1);
	CHECK_EQ(r.idrLimited, 1);
	// the viewer's budget is the limit whatever the link says: R decides
	kvm_vrc_init(&r, 28, t);
	t += 100; kvm_vrc_idr_acked(&r, t, 150000, t - 300, t, 0, 10); kvm_vrc_update(&r, t, 2000, 3000000, 0);
	CHECK_EQ(r.idrLimited, 1);
	CHECK_EQ(r.rate, 200000);
	CHECK_EQ(kvm_vrc_qp(&r, 1), 38);
	// no rate sample yet: limited (cautious)
	kvm_vrc_init(&r, 28, t);
	t += 100; kvm_vrc_update(&r, t, 0, 0, 0);
	CHECK_EQ(r.idrLimited, 1);
	// the clock wraps
	kvm_vrc_init(&r, 28, 0xFFFFFF00u);
	t = held_windows(&r, 0xFFFFFF00u, 6, 1); kvm_vrc_update(&r, t, 0, 200000, 1);
	CHECK_EQ(r.idrLimited, 1);
	t = held_windows(&r, t, 40, 0); kvm_vrc_update(&r, t, 0, 200000, 0);
	CHECK_EQ(r.idrLimited, 0);
	kvm_vrc_idr_acked(&r, 0xFFFFFFF0u, 150000, 0xFFFFFF10u + 0u, 0xFFFFFFF0u + 0u, 0, 10);	// (written 224 ms before, acknowledged before the wrap)
	CHECK(r.cIdr > 600000 && r.cIdr < 800000);
	kvm_vrc_idr_acked(&r, 0x00000020u, 150000, 0xFFFFFF10u, 0x00000010u, 0, 10);			// acknowledged after the wrap: 0x110 = 272 ms
	CHECK(r.cIdr > 600000 && r.cIdr < 620000);
	// a long silence (windows that were never closed) counts as windows without a refusal
	kvm_vrc_init(&r, 28, t);
	t = held_windows(&r, t, 6, 1); kvm_vrc_update(&r, t, 0, 200000, 1);
	CHECK_EQ(r.idrLimited, 1);
	t += 25000; (void)kvm_vrc_tick(&r, t); kvm_vrc_update(&r, t, 0, 200000, 0);
	CHECK_EQ(r.idrLimited, 0);
}

// the QP does not go down in a window in which the window of the viewer refused a frame; the sharpening of the window is taken off R
// the first key frame of a session on a link that took it in 40 ms, with a trickle of 33 B/s as the only rate sample and the window of the viewer that the trickle sized (64 KB) refusing the
// frames behind it. The trace of the Windows 11 VM: R fell to its floor of 100 kb/s, the bucket held -89 KB and nothing was sent for 7 s, though the ACK of the key frame came after 40 ms
static void test_idr_proof(uint32_t t0)
{
	kvm_vrc r;
	uint32_t t = t0, tAck;
	int i;
	kvm_vrc_init(&r, 28, t);
	// before the key frame: the trickle (a rate that says nothing), then the key frame of 233 472 B (never refused by the bucket), then frames refused by the window of the viewer (the link is "limited")
	t += 100; kvm_vrc_update(&r, t, 0, 33, 0);
	CHECK(r.cap >= KVM_VRC_MIN_RATE);									// (a rate of 33 B/s is not the estimate of a link: the estimate is not below the smallest R)
	(void)kvm_vrc_may_encode(&r, t, 1); kvm_vrc_frame(&r, t, 233472, 38, 1, 50000);
	t += 5; kvm_vrc_window_refused(&r); kvm_vrc_update(&r, t, 0, 33, 1);
	CHECK_EQ(r.rate, KVM_VRC_MIN_RATE);									// the key frame is not acknowledged yet: all there is is the trickle
	// the ACK: 40 ms after the write, held 14 ms by the viewer, minRtt 46 ms (a queue of the first picture): the time to carry it was 26 ms, so the link took 233 472 B in 26 ms at least
	t += 40; kvm_vrc_idr_acked(&r, t, 233472, t - 40, t, 14, 46); tAck = t;
	CHECK_EQ(r.cIdr, 23347200);											// (C_idr: 10 ms, the clamp: the round trip of 46 ms is more than the ACK took)
	CHECK_EQ(r.capFloor, 7632738);										// 0.85 x 233 472 / 26 ms: no round trip taken off
	kvm_vrc_update(&r, t, 0, 33, 1);									// the window refused a moment ago: still "limited" with the same trickle
	CHECK_EQ(r.rate, 500000);											// R is the ceiling of the estimate (2 x 2 Mb/s), not 100 kb/s
	CHECK_EQ(r.rFromFloor, 1);											// (the log says that R is the proof, not the estimate)
	// the bucket: -105 KB of debt at the first R of 2 Mb/s paid back at 500 KB/s, not at 12.5 KB/s
	for (i = 0; i < 20; i++) { t += 16; kvm_vrc_update(&r, t, 0, 33, 0); }
	CHECK(kvm_vrc_peek(&r, t));
	CHECK(r.tokens > 0);
	// the proof is not the estimate: it stands for KVM_VRC_FLOOR_MS, then R follows the estimate again (the samples of the video that went meanwhile raise it: m > cap)
	t = tAck + KVM_VRC_FLOOR_MS; kvm_vrc_update(&r, t, 0, 0, 0);
	CHECK_EQ(r.rate, 500000);
	t += 1; kvm_vrc_update(&r, t, 0, 0, 0);
	CHECK_EQ(r.capFloor, 0);
	CHECK_EQ(r.rFromFloor, 0);
	CHECK(r.rate < 500000);
	// a refusal of the window takes the proof away at once (the link may have fallen since): R follows the samples as it always did
	kvm_vrc_init(&r, 28, t);
	t += 100; kvm_vrc_update(&r, t, 0, 33, 0);
	kvm_vrc_idr_acked(&r, t, 233472, t - 40, t, 14, 46); kvm_vrc_update(&r, t, 0, 33, 0);
	CHECK_EQ(r.rate, 500000);
	kvm_vrc_window_refused(&r);
	CHECK_EQ(r.capFloor, 0);
	t += 100; kvm_vrc_update(&r, t, 0, 100000, 1);						// the link is the limit now: 0.85 x the delivery rate
	CHECK_EQ(r.rate, 85000);
	CHECK_EQ(r.rFromFloor, 0);
	// a link that is full (refusals of the window in six of the last 40 windows): the proof of a key frame is not used, the samples decide (as for the size of the key frame)
	kvm_vrc_init(&r, 28, t);
	t += 100; kvm_vrc_update(&r, t, 0, 100000, 0);
	t = held_windows(&r, t, 6, 1); kvm_vrc_idr_acked(&r, t, 233472, t - 40, t, 14, 46);
	kvm_vrc_update(&r, t, 0, 100000, 1);
	CHECK_EQ(r.idrLimited, 1);
	CHECK_EQ(r.rate, 85000);
	// the key frame that took long: 1 Mb/s, 113 000 B in 1 s: the proof is what it took (0.85 x 113 B/ms), not more, and never above the ceiling of the estimate
	kvm_vrc_init(&r, 28, t);
	t += 100; kvm_vrc_update(&r, t, 0, 100000, 1);
	t += 1000; kvm_vrc_idr_acked(&r, t, 113000, t - 1000, t, 0, 10);
	CHECK_EQ(r.capFloor, 96050);
	kvm_vrc_update(&r, t, 0, 100000, 0);
	CHECK_EQ(r.rate, 96050);											// (above 0.85 x the rate that was delivered, 85 000: the key frame took 113 000 B in 1 s, the link takes at least that)
	// no bytes, a time that comes out zero or negative: at least 10 ms; no proof out of nothing
	kvm_vrc_init(&r, 28, t);
	kvm_vrc_idr_acked(&r, t, 0, t - 20, t, 0, 5);
	CHECK_EQ(r.capFloor, 0);
	kvm_vrc_idr_acked(&r, t, 1000, t + 5, t, 30, 20);
	CHECK_EQ(r.capFloor, 85000);										// 1000 B / 10 ms x 0.85
	// across the wrap of the clock
	kvm_vrc_init(&r, 28, 0xFFFFFFE0u);
	kvm_vrc_idr_acked(&r, 0x00000010u, 100000, 0xFFFFFFF0u, 0x00000010u, 0, 5);	// 32 ms
	CHECK_EQ(r.capFloor, 2656250);
	// the estimate does not stay at 30 B/s: the probing (a quarter a second) begins from the smallest R, in the loop of 16 ms that the glue runs
	kvm_vrc_init(&r, 28, t);
	t += 100; kvm_vrc_update(&r, t, 0, 33, 1);							// (the window held the sender back: 0.85 x 33 B/s is the estimate, clamped to the smallest R)
	CHECK_EQ(r.cap, KVM_VRC_MIN_RATE);
	for (i = 0; i < 313; i++) { t += 16; kvm_vrc_update(&r, t, 0, 0, 0); (void)kvm_vrc_may_encode(&r, t, 0); r.tokens = -1; (void)kvm_vrc_may_encode(&r, t, 0); }	// 5 s of a bucket that refuses
	CHECK(r.rate > 3 * KVM_VRC_MIN_RATE);
	// the rate filter: r_from_floor says that R of this second was raised by the proof, not that a proof stands. The budget of the viewer is what R follows when it is at or below the estimate
	// (40 kbit/s: R is the smallest R with or without the proof): 0. A budget above the estimate and below the proof: R goes up to the budget by the proof: 1
	kvm_vrc_init(&r, 28, t);
	t += 100; kvm_vrc_update(&r, t, 40, 33, 1);
	kvm_vrc_idr_acked(&r, t, 233472, t - 40, t, 14, 46); kvm_vrc_update(&r, t, 40, 33, 1);
	CHECK(r.capFloor > r.cap);											// (the proof stands and is above the estimate)
	CHECK_EQ(r.rate, KVM_VRC_MIN_RATE);
	CHECK_EQ(r.rFromFloor, 0);
	kvm_vrc_update(&r, t, 1000, 33, 0);									// 1000 kbit/s: 100 000 B/s, the estimate (12 500 B/s) is below it, the proof (ceiling 500 000 B/s) above
	CHECK_EQ(r.rate, 100000);
	CHECK_EQ(r.rFromFloor, 1);
	kvm_vrc_update(&r, t, 0, 33, 0);
	CHECK_EQ(r.rFromFloor, 1);
}

// a rate that is no evidence of the link (a lower bound: the pipe was empty) does not set the first estimate, and a refusal of the window that is no evidence of the link (soft) keeps the proof
// of the key frame and is no held window: the scene E (a trickle of 33 B/s before the key frame, the window of 64 KB that it sized refuses the frames behind the key frame)
static void test_lower_bound(uint32_t t0)
{
	kvm_vrc r;
	uint32_t t = t0;
	kvm_vrc_init(&r, 28, t);
	t += 100; kvm_vrc_update(&r, t, 0, 33, 0);
	CHECK_EQ(r.cap, KVM_VRC_DEFAULT_RATE);								// (not 28 B/s, not the smallest R: the estimate of the start, the rate would only raise it)
	CHECK_EQ(r.rate, KVM_VRC_DEFAULT_RATE);
	t += 40; kvm_vrc_idr_acked(&r, t, 233472, t - 40, t, 14, 46); kvm_vrc_update(&r, t, 0, 33, 0);
	CHECK_EQ(r.rate, 500000);											// the proof, up to the ceiling of the estimate
	t += 5; kvm_vrc_window_refused_soft(&r); kvm_vrc_update(&r, t, 0, 33, 0);
	CHECK(r.capFloor != 0);												// (the window was too small for what the sender had to send: the link took 233 KB in 26 ms)
	CHECK_EQ(r.winHeld, 0);
	CHECK_EQ(r.sHeld, 1);
	CHECK_EQ(r.rate, 500000);
	t += 5; kvm_vrc_window_refused(&r); kvm_vrc_update(&r, t, 0, 33, 0);	// a refusal that is evidence of the link: the proof goes, as before
	CHECK_EQ(r.capFloor, 0);
	CHECK_EQ(r.winHeld, 1);
	// the link is the limit (evidence): the estimate is 0.85 x the rate from the first call on, down to the smallest R
	kvm_vrc_init(&r, 28, t);
	t += 100; kvm_vrc_update(&r, t, 0, 100000, 1);
	CHECK_EQ(r.cap, 85000);
}

static void test_z4(uint32_t t0)
{
	kvm_vrc r;
	uint32_t t = t0;
	int w;
	// the old rule: 60 % of R and the window refused: QP down. Now: not (the link limits the stream)
	kvm_vrc_init(&r, 34, t); kvm_vrc_set_target(&r, 100000, t);
	for (w = 0; w < 8; w++) { kvm_vrc_window_refused(&r); feed(&r, &t, 30000, 10, 1); }
	CHECK_EQ(r.qp, 34); CHECK(r.sQpBlocked >= 7);
	for (w = 0; w < 4; w++) { feed(&r, &t, 30000, 10, 1); }			// the refusals stop: the QP comes down again
	CHECK(r.qp < 34);
	// every other window refused: no two quiet windows in a row, so no step
	kvm_vrc_init(&r, 34, t); kvm_vrc_set_target(&r, 100000, t);
	for (w = 0; w < 12; w++) { if (w % 2) { kvm_vrc_window_refused(&r); } feed(&r, &t, 30000, 10, 1); }
	CHECK_EQ(r.qp, 34);
	// the sharpening counts against R: 60 % of R in video and 50 % in sharpening is 120 % of what is left (50 % of R), the QP goes UP, not down
	kvm_vrc_init(&r, 34, t); kvm_vrc_set_target(&r, 100000, t);
	for (w = 0; w < 3; w++)
	{
		kvm_vrc_note_sharpen(&r, t, 25000, 1);
		feed(&r, &t, 30000, 10, 1);
	}
	CHECK(r.qp > 34);
	CHECK(r.sSharpBytes >= 75000);
	// the same video with only a little sharpening (10 % of R): 60 % of 90 % = 67 %, below 70 %: down after two windows
	kvm_vrc_init(&r, 34, t); kvm_vrc_set_target(&r, 100000, t);
	for (w = 0; w < 4; w++)
	{
		kvm_vrc_note_sharpen(&r, t, 5000, 1);
		feed(&r, &t, 30000, 10, 1);
	}
	CHECK(r.qp < 34);
	// the sharpening takes nearly all of R (more than three quarters): the video keeps a quarter of it as its measure, it is not "over" with a few frames
	kvm_vrc_init(&r, 34, t); kvm_vrc_set_target(&r, 100000, t);
	for (w = 0; w < 3; w++)
	{
		kvm_vrc_note_sharpen(&r, t, 45000, 1);
		feed(&r, &t, 6000, 10, 1);
	}
	CHECK(r.qp <= 34);
	// sharpening outside a movement is only counted, it takes nothing from the bucket and the window
	kvm_vrc_init(&r, 34, t); kvm_vrc_set_target(&r, 100000, t);
	{
		const int64_t tok = r.tokens;
		kvm_vrc_note_sharpen(&r, t, 40000, 0);
		CHECK_EQ(r.tokens, tok); CHECK_EQ(r.winSharp, 0); CHECK_EQ(r.sSharpBytes, 40000);
		kvm_vrc_note_sharpen(&r, t, 40000, 1);
		CHECK(r.tokens < tok); CHECK_EQ(r.winSharp, 40000);
	}
	// the bit rate mode: the QP stands still, R is the bit rate
	kvm_vrc_init(&r, 34, t); kvm_vrc_set_target(&r, 100000, t);
	CHECK_EQ(kvm_vrc_bitrate_bps(&r), 800000);
	kvm_vrc_set_bitrate_mode(&r, 1);
	feed(&r, &t, 300000, 10, 6); CHECK_EQ(r.qp, 34);
	feed(&r, &t, 3000, 10, 6); CHECK_EQ(r.qp, 34);
	kvm_vrc_set_target(&r, 1000, t); CHECK_EQ(kvm_vrc_bitrate_bps(&r), 100000);		// (R is at least 12 500 B/s)
	kvm_vrc_set_target(&r, 90000000, t); CHECK_EQ(kvm_vrc_bitrate_bps(&r), 400000000);
}

// the capacity estimate does not run away while the viewer's budget (not the link) is R
static void test_capacity_bound(uint32_t t0)
{
	kvm_vrc r;
	uint32_t t = t0;
	int i;
	kvm_vrc_init(&r, 28, t);
	// 400 s: budget 2 Mb/s, 170 000 B/s delivered (the video holds its own bucket all the time), the link is not the limit
	for (i = 0; i < 4000; i++)
	{
		t += 100;
		(void)kvm_vrc_may_encode(&r, t, 0); r.tokens = 0; (void)kvm_vrc_may_encode(&r, t, 0);
		kvm_vrc_update(&r, t, 2000, 170000, 0);
		if (i % 500 == 499) { CHECK_EQ(r.rate, 200000); CHECK(r.cap <= 2 * 250000); CHECK(r.cap <= 250000); }	// stays at the estimate of the start (the first estimate is the default 2 Mb/s, a rate that is no evidence of the link only raises it) and does not run away
	}
	// the budget is removed: R is the estimate, which is at most 2 x max(0.85 x rate, 2 Mb/s) and not the 1e17 B/s that the unbounded growth reached
	kvm_vrc_update(&r, t, 0, 170000, 0);
	CHECK(r.rate <= 500000 && r.rate >= 195000);
	for (i = 0; i < 400; i++) { t += 100; (void)kvm_vrc_may_encode(&r, t, 0); r.tokens = 0; (void)kvm_vrc_may_encode(&r, t, 0); kvm_vrc_update(&r, t, 0, 170000, 0); }
	CHECK_EQ(r.cap, 500000);											// probing without a budget stops at the ceiling
	CHECK_EQ(r.rate, 500000);
	// the ceiling follows the delivery rate (0.85 x 5 MB/s x 2)
	for (i = 0; i < 400; i++) { t += 100; (void)kvm_vrc_may_encode(&r, t, 0); r.tokens = 0; (void)kvm_vrc_may_encode(&r, t, 0); kvm_vrc_update(&r, t, 0, 2000000, 0); }
	CHECK_EQ(r.cap, 3400000);
	// absurd input cannot overflow anything
	kvm_vrc_init(&r, 28, t);
	for (i = 0; i < 100; i++) { t += 600; (void)kvm_vrc_may_encode(&r, t, 0); r.tokens = 0; (void)kvm_vrc_may_encode(&r, t, 0); kvm_vrc_update(&r, t, 0, ~(uint64_t)0 / 4, 0); }
	CHECK(r.cap <= (2ull * 85ull * (1ull << 40)) / 100ull + 1ull);
	CHECK(r.rate == r.cap);
	// the link is the limit again: the estimate comes down at once
	kvm_vrc_init(&r, 28, t); r.cap = 3000000;
	kvm_vrc_update(&r, t + 100, 0, 100000, 1); CHECK_EQ(r.cap, 85000);
}

// after a silence the QP goes back to 30
static void test_idle_qp(uint32_t t0)
{
	kvm_vrc r;
	uint32_t t = t0;
	kvm_vrc_init(&r, 38, t);
	kvm_vrc_set_target(&r, 100000, t);
	t += 100; kvm_vrc_frame(&r, t, 3000, 38, 0, 5000);
	(void)kvm_vrc_tick(&r, t + 500); CHECK_EQ(r.qp, 38);
	CHECK_EQ(kvm_vrc_tick(&r, t + 9000), 0); CHECK_EQ(r.qp, 38);					// 9 s of silence: nothing yet
	CHECK_EQ(kvm_vrc_tick(&r, t + KVM_VRC_IDLE_MS - 1), 0); CHECK_EQ(r.qp, 38);
	CHECK_EQ(kvm_vrc_tick(&r, t + KVM_VRC_IDLE_MS + 500), KVM_VRC_IDLE_QP - 38); CHECK_EQ(r.qp, KVM_VRC_IDLE_QP);
	CHECK_EQ(kvm_vrc_tick(&r, t + KVM_VRC_IDLE_MS + 1100), 0); CHECK_EQ(r.qp, KVM_VRC_IDLE_QP);	// once
	// a QP below 30 is left alone, a frame restarts the clock
	kvm_vrc_init(&r, 27, t); kvm_vrc_frame(&r, t, 3000, 27, 0, 5000);
	(void)kvm_vrc_tick(&r, t + 20000); CHECK_EQ(r.qp, 27);
	kvm_vrc_init(&r, 36, t); kvm_vrc_frame(&r, t, 3000, 36, 0, 5000);
	t += 8000; kvm_vrc_frame(&r, t, 3000, 36, 0, 5000); (void)kvm_vrc_tick(&r, t + 3000); CHECK_EQ(r.qp, 36);
	// before the first frame there is nothing to compare with
	kvm_vrc_init(&r, 36, t); (void)kvm_vrc_tick(&r, t + 60000); CHECK_EQ(r.qp, 36);
	// the time wraps
	kvm_vrc_init(&r, 38, 0xFFFFF000u); kvm_vrc_frame(&r, 0xFFFFF000u, 3000, 38, 0, 5000);
	(void)kvm_vrc_tick(&r, 0xFFFFF000u + 9000); CHECK_EQ(r.qp, 38);
	(void)kvm_vrc_tick(&r, 0xFFFFF000u + 10600); CHECK_EQ(r.qp, 30);
}

// the flag `starved`
static void test_starved(void)
{
	kvm_vrc r;
	uint32_t t = 1000;
	int i;
	kvm_vrc_init(&r, 38, t);
	for (i = 0; i < 8; i++)
	{
		r.sFrames = 1; r.sDenied = 5; t += 1000;
		(void)kvm_vrc_update_starved(&r, t);
		CHECK_EQ(r.starved, i >= 5 ? 1 : 0);							// 5 s from the first second
	}
	r.sFrames = 3; t += 1000; CHECK_EQ(kvm_vrc_update_starved(&r, t), 0);	// 3 frames in the second: not starved
	r.sFrames = 1; t += 1000; CHECK_EQ(kvm_vrc_update_starved(&r, t), 0);	// and the clock starts again
	r.sDenied = 0; r.sHeld = 0; for (i = 0; i < 8; i++) { t += 1000; CHECK_EQ(kvm_vrc_update_starved(&r, t), 0); }	// no refusals (a still screen): not starved
	// the window of the viewer limits, the bucket does not (a slow link, R above it in the saw): starved all the same
	r.sDenied = 0; r.sHeld = 7; r.sFrames = 1; r.qp = 38; for (i = 0; i < 8; i++) { t += 1000; (void)kvm_vrc_update_starved(&r, t); CHECK_EQ(r.starved, i >= 5 ? 1 : 0); }
	r.sDenied = 2; r.qp = 36; for (i = 0; i < 8; i++) { t += 1000; CHECK_EQ(kvm_vrc_update_starved(&r, t), 0); }	// QP below the ceiling: not starved
}

static void test_bucket(uint32_t t0)
{
	kvm_vrc r;
	uint32_t t = t0;
	kvm_vrc_init(&r, 28, t);
	kvm_vrc_set_target(&r, 100000, t);
	// starts with a burst of 0.5 s; frames are allowed while it holds anything; the debt is limited to 2 s of R
	CHECK(kvm_vrc_may_encode(&r, t, 0));
	kvm_vrc_frame(&r, t, 40000, 28, 0, 1000); CHECK(kvm_vrc_may_encode(&r, t, 0));	// 50 000 - 40 000
	kvm_vrc_frame(&r, t, 40000, 28, 0, 1000); CHECK(!kvm_vrc_may_encode(&r, t, 0));	// -30 000
	CHECK_EQ(r.winDenied, 1);
	CHECK(kvm_vrc_may_encode(&r, t, 1));								// a first frame / a key frame after a lost AU is not refused ...
	CHECK_EQ(r.winDenied, 1);											// ... and not counted as a refusal
	CHECK(!kvm_vrc_peek(&r, t + 299)); CHECK(kvm_vrc_peek(&r, t + 301));	// 100 000 B/s: the debt of 30 000 is paid after 300 ms
	CHECK_EQ(r.winDenied, 1);											// peek counts nothing
	// a key frame of 250 000 B at R = 100 000: the debt is limited to 2 s (200 000 B): the video is not stopped for 2.5 s
	kvm_vrc_init(&r, 28, t); kvm_vrc_set_target(&r, 100000, t);
	kvm_vrc_frame(&r, t, 300000, 28, 1, 20000);
	CHECK_EQ(r.tokens, -200000);
	CHECK(!kvm_vrc_peek(&r, t + 1999)); CHECK(kvm_vrc_peek(&r, t + 2001));
	// the long run never exceeds R: 100 s of frames of 12 000 B asked for every 16 ms are let through at R
	kvm_vrc_init(&r, 28, t); kvm_vrc_set_target(&r, 100000, t);
	{
		uint64_t sent = 0;
		uint32_t k, st = t;
		for (k = 0; k < 100000 / 16; k++)
		{
			st += 16;
			if (kvm_vrc_may_encode(&r, st, 0)) { kvm_vrc_frame(&r, st, 12000, 28, 0, 1000); sent += 12000; }
		}
		CHECK(sent <= 100000ull * 100u + 50000u + 12000u);				// R x 100 s + the burst + one frame
		CHECK(sent >= 100000ull * 100u * 9u / 10u);					// and R is used (within 10 %)
	}
	// a change of the target keeps what was earned
	kvm_vrc_init(&r, 28, t); kvm_vrc_set_target(&r, 100000, t);
	kvm_vrc_frame(&r, t, 50000, 28, 0, 1000);
	kvm_vrc_set_target(&r, 200000, t + 250);
	CHECK_EQ(r.tokens, 25000);
	// the time wraps
	kvm_vrc_init(&r, 28, 0xFFFFFFF0u); kvm_vrc_set_target(&r, 100000, 0xFFFFFFF0u);
	kvm_vrc_frame(&r, 0xFFFFFFF0u, 51000, 28, 0, 1000);
	CHECK(!kvm_vrc_peek(&r, 0xFFFFFFF0u + 5)); CHECK(kvm_vrc_peek(&r, 0xFFFFFFF0u + 20));
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the simulation
// ---------------------------------------------------------------------------------------------------------------------------------------

typedef enum { C_STATIC = 0, C_SCROLL = 1, C_NOISE = 2 } content_t;
static const char *const kContent[] = { "text+small motion", "scrolling text", "noise (video)" };

static uint32_t gRng = 777u;
static double rnd01(void) { gRng = (gRng * 1664525u) + 1013904223u; return (double)(gRng >> 8) / 16777216.0; }

// Frame sizes (1080p). P and IDR of the encoder probe measurement (scrolling text and a moving window, constant QP): P 31 679 / 8 913 / 3 628 B at QP 24 / 30 / 36, IDR 250 976 / 175 245 / 121 410 B;
// log-linear between and beyond those points (-19 % per QP below 24 and between 24 and 30, -14 % per QP above 30). "text + small motion" is a tenth of that (a cursor, a clock, a small
// window: most of the picture is skipped). "noise" (full-screen video-like content, the smoke of about 300 KB per P frame at QP 28 at 1440x832) is modelled as 400 KB at QP 28,
// -11 % per QP (6 QP halve the size), IDR 1.5 x that of the text. +-15 % of jitter.
static double pts(double qp, const double *q, const double *v)
{
	int i = 0;
	double lo, hi;
	if (qp <= q[0]) { return v[0] * pow(v[0] / v[1], (q[0] - qp) / (q[1] - q[0])); }
	if (qp >= q[2]) { return v[2] * pow(v[2] / v[1], (qp - q[2]) / (q[2] - q[1])); }
	i = qp < q[1] ? 0 : 1;
	lo = v[i]; hi = v[i + 1];
	return lo * pow(hi / lo, (qp - q[i]) / (q[i + 1] - q[i]));
}

static double frame_bytes(content_t c, int idr, int qp)
{
	static const double q[3] = { 24, 30, 36 }, p[3] = { 31679, 8913, 3628 }, k[3] = { 250976, 175245, 121410 };
	double b;
	if (c == C_NOISE) { b = idr ? 1.5 * pts(qp, q, k) : 400000.0 * pow(0.89, qp - 28); }
	else { b = idr ? pts(qp, q, k) : pts(qp, q, p) * (c == C_STATIC ? 0.1 : 1.0); }
	return b * (0.85 + 0.3 * rnd01());
}

typedef struct sim_result
{
	double rAvg, videoBps, ratio, fps, latAvg, latMax, sharpenShare, qpAvg;
	double idrLatMax, idrLatFirst, idrLatRepair, rTail, linkLimShare, latTail, fpsTail;	// key frame latency (all key frames, from the start), R (kb/s), latency and fps in the last 10 s, share of iterations with "link limited"
	uint64_t capEnd;
	int qpMin, qpMax, qpStart, qpEnd, frames, idr, denied, idrQpMin, idrQpMax;
	unsigned qpUp, qpDown;		// QP steps in the measured part
	uint64_t videoBytes, sharpBytes, written;
	float rSec[600];		// R in kb/s at the start of every simulated second
} sim_result;

// the link and the viewer's budget change at the start of a phase (seconds from the start)
typedef struct sim_phase { int fromSec; uint64_t linkBps; unsigned budget; } sim_phase;

typedef struct pos_t { uint32_t seq, ts; uint64_t cum; int ready; uint32_t readyAt; } pos_t;

// linkBps: bytes per second of the link, budget: the viewer's budget in kbit/s (0 = none), seconds of simulated time, the first `warm` seconds are not measured
static sim_result simulate_ph(const sim_phase *ph, int nph, content_t content, int seconds, int warm, int verbose)
{
	sim_result res;
	uint64_t linkBps = ph[0].linkBps;
	unsigned budget = ph[0].budget;
	double rTailSum = 0, latTailSum = 0, linkLimIter = 0;
	unsigned tailN = 0, tailFrames = 0, up0 = 0, down0 = 0;
	int ip, measuredStarted = 0;
	kvm_stream s;
	kvm_vctl ctl;
	kvm_vrc rc;
	kvm_state_msg m, reply;
	kvm_ctrl op;
	kvm_budget paceBudget;
	static pos_t pos[200000];
	size_t npos = 0, firstUnready = 0, firstUndelivered = 0;
	double served = 0.0, lastIter = 0, tilesVideo = 0, sharpDebt = 0, encAvgUs = 0, rSum = 0, latSum = 0;
	uint32_t t, t0 = 5000, lastAck = 0, lastFrameT = 0, lastForced = 0, lastMotionMs = 0, nIter = 0, lastQuiet = 0;
	int forcedHave = 0, motion, pending = 0, tick, repairDone = 0, k;
	kvm_budget_init(&paceBudget, 2);	// the machine of the smoke test: 2 vCPU, one core of budget for the encoder
	uint64_t videoBytesM = 0, sharpM = 0;
	unsigned framesM = 0;
	double qpSum = 0;
	unsigned qpN = 0;
	int firstQp = -1;
	memset(&res, 0, sizeof(res));
	memset(pos, 0, sizeof(pos[0]) * 4);
	kvm_stream_init(&s, 1, 1, 1, 6);
	memset(&op, 0, sizeof(op)); op.ver = 1; op.op = KVM_OP_VIDEO_ON; op.flags = 7;
	(void)kvm_stream_on_ctrl(&s, &op, t0, &reply, NULL);
	kvm_vctl_init(&ctl, 1);
	(void)kvm_vctl_begin_gen(&ctl, &s, 100, 42, 1920, 1088, 1, &m);
	kvm_vrc_init(&rc, KVM_VIDEO_QP, t0);
	gRng = 777u;
	for (t = t0; t < t0 + (uint32_t)seconds * 1000u; t++)
	{
		const int measured = t >= t0 + (uint32_t)warm * 1000u;
		const int tail = t >= t0 + (uint32_t)(seconds - 10) * 1000u;
		for (ip = 0; ip < nph; ip++) { if (t >= t0 + (uint32_t)ph[ip].fromSec * 1000u) { linkBps = ph[ip].linkBps; budget = ph[ip].budget; } }
		// the link serves the bytes written, in order
		served += (double)linkBps / 1000.0;
		if (served > (double)s.bytesWritten) { served = (double)s.bytesWritten; }
		while (firstUnready < npos && (double)pos[firstUnready].cum <= served + 0.5) { pos[firstUnready].ready = 1; pos[firstUnready].readyAt = t + 40; firstUnready++; }	// the data takes 20 ms and the ACK 20 ms
		if (t - lastAck >= 20)
		{
			size_t newest = (size_t)-1, i;
			for (i = firstUndelivered; i < firstUnready; i++) { if (pos[i].ready && (int32_t)(t - pos[i].readyAt) >= 0) { newest = i; } }
			if (newest != (size_t)-1)
			{
				kvm_ack a;
				a.seq = pos[newest].seq; a.ts = pos[newest].ts; a.queueMs = 0; a.flags = 0;
				(void)kvm_stream_on_ack(&s, &a, t);
				firstUndelivered = newest + 1; lastAck = t;
			}
		}
		if (t - (uint32_t)lastIter < 16) { continue; }
		lastIter = (double)t;
		nIter++;
		// ---- one iteration of the main loop ----
		(void)kvm_stream_tick(&s, t, 0, &reply);
		kvm_vrc_update(&rc, t, budget, kvm_stream_rate_or_mem(&s, t), kvm_stream_link_limited(&s, t));
		if (measured && !measuredStarted) { measuredStarted = 1; up0 = rc.sQpUp; down0 = rc.sQpDown; }
		if (measured) { rSum += (double)rc.rate; }
		if (kvm_stream_link_limited(&s, t)) { linkLimIter += 1.0; }
		if (tail) { rTailSum += (double)rc.rate; }
		if (kvm_vrc_tick(&rc, t) != 0) { }
		// the content
		switch (content)
		{
			case C_STATIC: motion = ((t / 200u) % 4u) == 0; break;		// a burst of small motion of 200 ms every 800 ms
			case C_SCROLL: motion = 1; break;
			default: motion = 1; break;
		}
		if (motion) { pending = 1; lastMotionMs = t; }
		{
			kvm_vplan_in in;
			kvm_vplan plan;
			in.now = t; in.pending = pending ? 10u : 0u; in.gateOpen = kvm_stream_window_allows(&s, KVM_GATE_VIDEO, t); in.rateOpen = kvm_vrc_peek(&rc, t);
			in.intervalMs = kvm_pace_interval(&paceBudget, 16, (uint32_t)encAvgUs); in.rate = kvm_stream_rate_or_mem(&s, t);
			kvm_vctl_plan(&ctl, &s, &in, &plan);
			if (plan.held) { if (kvm_stream_note_link_held(&s, t)) { kvm_vrc_window_refused(&rc); } else { kvm_vrc_window_refused_soft(&rc); } }	// kvm_video_emit: a video frame refused by the window
			if (plan.heldRate) { (void)kvm_vrc_may_encode(&rc, t, 0); }	// a refusal by the bucket is counted for the regulator
			if (plan.encode)
			{
				int idr = ctl.idrFirst || plan.forceIdr || ctl.idrForce, qp = kvm_vrc_qp(&rc, idr);
				uint32_t bytes, encUs = (uint32_t)(content == C_STATIC ? 14000 : (content == C_SCROLL ? 25000 : 28000));
				uint32_t seq;
				(void)kvm_vrc_may_encode(&rc, t, idr);
				bytes = (uint32_t)frame_bytes(content, idr, qp);
				encAvgUs = encAvgUs == 0 ? (double)encUs : (encAvgUs * 7.0 + (double)encUs) / 8.0;
				{ const int hadFrame = ctl.haveFrame; const uint32_t prevSlot = ctl.lastFrame; (void)kvm_vctl_frame_done(&ctl, idr, bytes, t); ctl.lastFrame = kvm_pace_slot(hadFrame, prevSlot, t, in.intervalMs, NULL); }	// the slot of the frame, as kvm_video_emit does
				kvm_vrc_frame(&rc, t, bytes, 28, idr, encUs);
				seq = kvm_stream_add_position(&s, t, bytes);
				kvm_stream_note_write(&s, bytes, KVM_W_POSITION, t);
				pos[npos].seq = seq; pos[npos].ts = t; pos[npos].cum = s.bytesWritten; npos++;
				pending = 0;
				tilesVideo = content == C_STATIC ? 40 : (content == C_SCROLL ? 400 : 2000);	// the area that moves is painted again and again: the same tiles
				sharpDebt = tilesVideo * (content == C_NOISE ? 3000.0 : 1500.0);
				if (firstQp < 0) { firstQp = rc.qp; }
				lastFrameT = t;
				if (measured) { videoBytesM += bytes; framesM++; if (idr) { res.idr++; } }
				if (tail) { tailFrames++; }
				if (idr)
				{
					const double lat = (double)(s.bytesWritten - (uint64_t)served) * 1000.0 / (double)linkBps + 40.0;	// from the write to the last byte at the viewer
					if (lat > res.idrLatMax) { res.idrLatMax = lat; }
					if (res.idrLatFirst == 0) { res.idrLatFirst = lat; }
					if (t > t0 + 25000u && lat > res.idrLatRepair) { res.idrLatRepair = lat; }	// the key frame asked for at 30 s: the pipe is full and R is known
					if (res.idrQpMin == 0 || qp < res.idrQpMin) { res.idrQpMin = qp; }
					if (qp > res.idrQpMax) { res.idrQpMax = qp; }
				}
			}
		}
		// sharpening: when the screen has been still for 150 ms the normal way (below half of the window, up to 98 304 B per iteration), else forced after 1 s and by the rules of M2
		if (sharpDebt > 0)
		{
			double give = 0;
			// what kvm_adapt_sharpen_budget does every iteration while there are VIDEO tiles: the 0.5 W gate refuses and the refusal is noted (kvm_ack_note_held_back: the rate samples, not "link limited")
			if (!kvm_stream_window_allows(&s, KVM_GATE_SHARPEN, t)) { kvm_stream_note_held_back(&s, t); }
			if (t - lastMotionMs >= 150 && kvm_stream_window_allows(&s, KVM_GATE_SHARPEN, t)) { give = sharpDebt < 98304.0 ? sharpDebt : 98304.0; lastQuiet = t; }
			else if (t - (lastQuiet ? lastQuiet : t0) >= KVM_VIDEO_SHARPEN_WAIT_MS)
			{
				uint32_t fb = 0;
				if (kvm_vcore_force_sharpen(kvm_stream_rate_recent(&s, t), budget, kvm_stream_inflight(&s), kvm_stream_window(&s, t), forcedHave, lastForced, t, &fb)) { give = sharpDebt < (double)fb ? sharpDebt : (double)fb; lastForced = t; forcedHave = 1; }
			}
			if (give > 0)
			{
				kvm_stream_note_write(&s, (size_t)give, KVM_W_DATA, t);
				sharpDebt -= give; tilesVideo = sharpDebt / (content == C_NOISE ? 3000.0 : 1500.0);
				if (measured) { sharpM += (uint64_t)give; }
			}
		}
		// rule M1: a position after an iteration that wrote bytes
		{
			int probe = 0;
			if (kvm_stream_marker_wanted(&s, t, 0, &probe))
			{
				uint8_t mk[KVM_MARKER_BYTES];
				kvm_stream_make_marker(&s, t, probe, mk);
				kvm_stream_note_write(&s, KVM_MARKER_BYTES, KVM_W_POSITION, t);
				pos[npos].seq = s.seqCounter; pos[npos].ts = t; pos[npos].cum = s.bytesWritten; npos++;
			}
		}
		if (npos > 199000) { break; }
		// a key frame asked for once (a recording or a repair) at 30 s
		if (!repairDone && t > t0 + 30000u) { repairDone = 1; kvm_vctl_op3(&ctl, KVM_ACT_IDR); }
		if (measured)
		{
			double lat = (double)(s.bytesWritten - (uint64_t)served) * 1000.0 / (double)linkBps + 40.0;
			if (tail) { latTailSum += lat; tailN++; }
			latSum += lat; if (lat > res.latMax) { res.latMax = lat; }
			qpSum += rc.qp; qpN++;
			if (res.qpMin == 0 || rc.qp < res.qpMin) { res.qpMin = rc.qp; }
			if (rc.qp > res.qpMax) { res.qpMax = rc.qp; }
		}
		tick = (int)((t - t0) / 1000u);
		if (tick >= 0 && tick < 600 && ((t - t0) % 1000u) < 16) { res.rSec[tick] = (float)((double)rc.rate * 8.0 / 1000.0); }
		if (verbose && ((t - t0) % 5000u) < 16 && tick > 0 && nIter % 2 == 0) { printf("     t=%2ds R=%6.0f kb/s qp=%d inflight=%llu B\n", tick, (double)rc.rate * 8 / 1000, rc.qp, (unsigned long long)kvm_stream_inflight(&s)); }
		(void)lastFrameT; (void)k;
	}
	{
		double sec = (double)(seconds - warm);
		double ni = sec * 1000.0 / 16.0;
		res.rAvg = rSum / ni;
		res.videoBps = (double)videoBytesM / sec;
		res.ratio = res.videoBps / res.rAvg;
		res.fps = (double)framesM / sec;
		res.latAvg = latSum / ni;
		res.sharpBytes = sharpM; res.videoBytes = videoBytesM; res.frames = (int)framesM;
		res.sharpenShare = (double)sharpM / ((double)sharpM + (double)videoBytesM + 1.0);
		res.qpAvg = qpN ? qpSum / qpN : 0; res.qpStart = firstQp; res.qpEnd = rc.qp;
		res.written = s.bytesWritten;
		res.capEnd = rc.cap;
		res.qpUp = rc.sQpUp - up0; res.qpDown = rc.sQpDown - down0;
		res.rTail = tailN ? rTailSum / (double)tailN * 8.0 / 1000.0 : 0.0;
		res.latTail = tailN ? latTailSum / (double)tailN : 0.0;
		res.fpsTail = (double)tailFrames / 10.0;
		res.linkLimShare = linkLimIter / (double)(nIter ? nIter : 1);
	}
	return res;
}

static sim_result simulate(uint64_t linkBps, unsigned budget, content_t content, int seconds, int warm, int verbose)
{
	sim_phase ph;
	ph.fromSec = 0; ph.linkBps = linkBps; ph.budget = budget;
	return simulate_ph(&ph, 1, content, seconds, warm, verbose);
}

static void row(const char *link, content_t c, const sim_result *r)
{
	printf("| %-8s | %-17s | %6.0f | %6.0f | %4.2f | %2d / %4.1f / %2d / %2d | %5.1f | %5.0f / %5.0f | %4.1f %% |\n", link, kContent[c], r->rAvg * 8 / 1000, r->videoBps * 8 / 1000, r->ratio, r->qpStart, r->qpAvg, r->qpEnd, r->qpMax, r->fps, r->latAvg, r->latMax, r->sharpenShare * 100.0);
}

// what makes a stream "link limited": a refused video frame or big rectangle, not the sharpening gate and not the closed gates of the RTT probe
static void test_link_held(void)
{
	kvm_stream s;
	kvm_ctrl op;
	kvm_state_msg reply;
	const uint32_t t0 = 5000;
	kvm_stream_init(&s, 1, 1, 1, 6);
	memset(&op, 0, sizeof(op)); op.ver = 1; op.op = KVM_OP_VIDEO_ON; op.flags = 7;
	(void)kvm_stream_on_ctrl(&s, &op, t0, &reply, NULL);
	CHECK(kvm_stream_ack_active(&s));
	CHECK(!kvm_stream_link_limited(&s, t0 + 10));
	kvm_stream_note_held_back(&s, t0 + 100);											// the sharpening gate (0.5 W) refused
	CHECK(!kvm_stream_link_limited(&s, t0 + 110));
	CHECK(s.everHeld);																	// ... it still feeds the rate samples
	kvm_stream_note_link_held(&s, t0 + 200);											// a video frame was refused
	CHECK(kvm_stream_link_limited(&s, t0 + 210));
	CHECK(kvm_stream_link_limited(&s, t0 + 200 + KVM_STREAM_HELD_MS - 1));
	CHECK(!kvm_stream_link_limited(&s, t0 + 200 + KVM_STREAM_HELD_MS));				// for a second
	kvm_stream_note_held_back(&s, t0 + 1500);											// a later sharpening refusal does not extend it
	CHECK(!kvm_stream_link_limited(&s, t0 + 1600));
	s.probeState = 1;																	// the RTT probe drains the pipe with every gate closed
	kvm_stream_note_link_held(&s, t0 + 2000);
	CHECK(!kvm_stream_link_limited(&s, t0 + 2010));
	CHECK(s.lastHeldBack == t0 + 2000);												// the rate samples are told
	s.probeState = 0;
	kvm_stream_note_link_held(&s, t0 + 2100);
	CHECK(kvm_stream_link_limited(&s, t0 + 2110));
	kvm_stream_note_link_held(&s, 0xFFFFFFF0u);										// the clock wraps
	CHECK(kvm_stream_link_limited(&s, 0x10u)); CHECK(!kvm_stream_link_limited(&s, 0x10u + KVM_STREAM_HELD_MS));
}

static void test_simulation_review(void)
{
	sim_result r;
	int ci;
	double rate;
	// M1 + M2: 320 s with a budget of 2 Mb/s on 50 Mb/s: the estimate does not run away, R stays at 0.8 x the budget, "link limited" is not raised by the sharpening gate
	printf("\n| long run, budget 2 Mb/s on 50 Mb/s, 320 s | content | R end kb/s | cap end kb/s | video kb/s | video/R | link limited | sharpening share |\n|---|---|---|---|---|---|---|---|\n");
	for (ci = 1; ci < 3; ci++)
	{
		r = simulate(6250000, 2000, (content_t)ci, 320, 20, 0);
		printf("| 50 Mb/s | %-17s | %6.0f | %6.0f | %6.0f | %4.2f | %4.1f %% | %4.1f %% |\n", kContent[ci], r.rTail, (double)r.capEnd * 8 / 1000, r.videoBps * 8 / 1000, r.ratio, r.linkLimShare * 100.0, r.sharpenShare * 100.0);
		CHECK(r.capEnd <= 2u * 250000u);										// 2 x max(0.85 x rate, 2 Mb/s)
		CHECK(r.rTail >= 1500.0 && r.rTail <= 1620.0);							// 0.8 x 2 Mb/s
		CHECK(r.ratio <= 1.12);
		if (ci == C_SCROLL) { CHECK(r.linkLimShare <= 0.05); }					// the link is not the limit: the refusals of the sharpening gate are not "link limited" (noise: its frames of 100+ KB do close the window now and then, that is a refusal of a video frame)
		CHECK(r.videoBps * 8 >= 0.5 * 1600000.0);
	}
	// the budget is removed after 100 s, the link takes 5 Mb/s: R rises to what the link takes (from 1.6 Mb/s, +25 % a second at most, not 1e17)
	{
		static const sim_phase ph[] = { { 0, 625000, 2000 }, { 100, 625000, 0 } };
		r = simulate_ph(ph, 2, C_NOISE, 200, 20, 0);
		printf("\nbudget 2 Mb/s, removed at 100 s, link 5 Mb/s, noise: R kb/s at 99 s / 103 s / 110 s / 130 s / 199 s: %.0f / %.0f / %.0f / %.0f / %.0f; cap end %.0f kb/s; latency last 10 s %.0f ms\n", r.rSec[99], r.rSec[103], r.rSec[110], r.rSec[130], r.rSec[199], (double)r.capEnd * 8 / 1000, r.latTail);
		CHECK(r.rSec[99] >= 1500.0 && r.rSec[99] <= 1620.0);
		CHECK(r.rSec[103] <= 1.25 * 1.25 * 1.25 * 1.25 * 1620.0);				// at most 25 % a second
		CHECK(r.rSec[130] >= 0.5 * 0.85 * 5000.0);
		CHECK(r.rTail >= 0.6 * 0.85 * 5000.0 && r.rTail <= 1.1 * 0.85 * 5000.0);
		CHECK(r.capEnd <= 2u * 625000u);
		CHECK(r.latTail <= 1300.0);
	}
	// the link drops from 5 to 1 Mb/s after 100 s (no budget): R comes down to what the link takes, the queue does not stay
	for (ci = 1; ci < 3; ci++)
	{
		static const sim_phase ph[] = { { 0, 625000, 0 }, { 100, 125000, 0 } };
		r = simulate_ph(ph, 2, (content_t)ci, 190, 20, 0);
		printf("link 5 Mb/s, 1 Mb/s from 100 s, %-15s: R kb/s at 99 s / 105 s / 115 s / 189 s: %.0f / %.0f / %.0f / %.0f; latency last 10 s %.0f ms, max %.0f ms; fps last 10 s %.1f\n", kContent[ci], r.rSec[99], r.rSec[105], r.rSec[115], r.rSec[189], r.latTail, r.latMax, r.fpsTail);
		rate = 0.85 * 1000.0;
		CHECK(r.rTail <= 1.1 * rate);
		CHECK(r.rTail >= 0.5 * rate);
		CHECK(r.latTail <= 1300.0);
		CHECK(r.ratio <= 1.5);
	}
	// the QP stuck at the ceiling on a fast link: the encode time limits the frames, R is not reached, nothing is refused; the quiet rule at 80 % brings the QP down
	// and it must not oscillate (QP steps in the 50 s measured)
	printf("\n| QP regulator, no refusals | content | R kb/s | video kb/s | video/R | QP avg / end / max | QP steps up / down | fps |\n|---|---|---|---|---|---|---|---|\n");
	{
		static const struct { const char *name; uint64_t bps; unsigned budget; content_t c; } qc[] = {
			{ "50 Mb/s, no budget", 6250000, 0, C_NOISE }, { "50 Mb/s, no budget", 6250000, 0, C_SCROLL }, { "5 Mb/s", 625000, 0, C_SCROLL },
			{ "50 Mb/s, budget 2 Mb/s", 6250000, 2000, C_NOISE }, { "50 Mb/s, budget 2 Mb/s", 6250000, 2000, C_SCROLL } };
		size_t qi;
		for (qi = 0; qi < sizeof(qc) / sizeof(qc[0]); qi++)
		{
			r = simulate(qc[qi].bps, qc[qi].budget, qc[qi].c, 70, 20, 0);
			printf("| %-24s | %-17s | %6.0f | %6.0f | %4.2f | %4.1f / %d / %d | %u / %u | %4.1f |\n", qc[qi].name, kContent[qc[qi].c], r.rAvg * 8 / 1000, r.videoBps * 8 / 1000, r.ratio, r.qpAvg, r.qpEnd, r.qpMax, r.qpUp, r.qpDown, r.fps);
			CHECK(r.qpUp + r.qpDown <= 4);									// no oscillation in 50 s
			CHECK(r.ratio <= 1.12);
			if (qi == 0) { CHECK(r.qpEnd <= 37 && r.qpAvg <= 37.5); CHECK(r.ratio >= 0.84); }	// the QP came down from 38 (the video is 0.79 R before, 0.88 R after; with the slots of the pace schedule the 28 ms noise gets 24 frames a second, 0.86 R) and stays
		}
		// the same for 320 s: it settles and stays
		r = simulate(6250000, 0, C_NOISE, 320, 20, 0);
		printf("(50 Mb/s, no budget, noise, 320 s: QP end %d, steps up %u / down %u, video/R %.2f)\n", r.qpEnd, r.qpUp, r.qpDown, r.ratio);
		CHECK(r.qpEnd <= 37); CHECK(r.qpUp + r.qpDown <= 4);
	}
	// the key frame at a low rate: the first one (the pipe is empty) and the one asked for at 30 s (a full pipe behind it)
	printf("\n| key frame | link | content | QP of the key frame | first (R not known yet) ms | at 30 s ms |\n|---|---|---|---|---|---|\n");
	{
		// limits for the key frame at 30 s (text / noise); the plan wants < 1 s; at 1 Mb/s and below a 1080p key frame cannot get there (QP 38 is about 90 KB), so these are the measured values + 20 %
		static const struct { const char *name; uint64_t bps; double repair, repairNoise; } lk[] = { { "0.5 Mb/s", 62500, 3300.0, 3500.0 }, { "1 Mb/s", 125000, 1400.0, 1800.0 }, { "3 Mb/s", 375000, 950.0, 900.0 } };
		size_t li;
		for (li = 0; li < sizeof(lk) / sizeof(lk[0]); li++)
		{
			for (ci = 0; ci < 3; ci++)
			{
				r = simulate(lk[li].bps, 0, (content_t)ci, 60, 20, 0);
				printf("| IDR | %-8s | %-17s | %d..%d | %5.0f | %5.0f |\n", lk[li].name, kContent[ci], r.idrQpMin, r.idrQpMax, r.idrLatFirst, r.idrLatRepair);
				CHECK(r.idrLatRepair > 0.0 && r.idrLatRepair <= (ci == C_NOISE ? lk[li].repairNoise : lk[li].repair));
				CHECK(r.idrLatFirst <= 5000.0);										// the first one is encoded before the rate is known (R = 2 Mb/s)
			}
		}
	}
}

static void test_simulation(void)
{
	static const struct { const char *name; uint64_t bps; } links[] = { { "0.5 Mb/s", 62500 }, { "1 Mb/s", 125000 }, { "3 Mb/s", 375000 }, { "5 Mb/s", 625000 } };
	size_t li;
	int ci;
	printf("\n| link | content | R kb/s | video kb/s | video/R | QP start / avg / end / max | fps | latency avg / max ms | sharpening share |\n|---|---|---|---|---|---|---|---|---|\n");
	for (li = 0; li < sizeof(links) / sizeof(links[0]); li++)
	{
		for (ci = 0; ci < 3; ci++)
		{
			sim_result r = simulate(links[li].bps, 0, (content_t)ci, 70, 20, 0);
			row(links[li].name, (content_t)ci, &r);
			// the video stays within R (the bucket), the QP within its range, the picture keeps moving, the queue stays short, and a high rate never starves the sharpening
			CHECK(r.ratio <= 1.12);
			CHECK(r.qpMin >= KVM_VIDEO_QP_MIN && r.qpMax <= KVM_VIDEO_QP_MAX);
			CHECK(r.fps >= 0.3);
			CHECK(r.latAvg <= 1300.0);
			CHECK(r.latMax <= 3200.0);	// the key frame at 30 s (QP at the top of the range at a low rate): 1.5-2.5 s at 0.5 Mb/s, the queue does not grow beyond it
			if (ci != C_STATIC) { CHECK(r.sharpenShare <= 0.30); }	// (small motion: the sharpening of its tiles is as big as its few video frames, that is the point of the hybrid)
			if (ci == C_NOISE && links[li].bps <= 125000) { CHECK(r.qpMax == KVM_VIDEO_QP_MAX); }	// noise at a low rate: the QP goes to the ceiling and the bucket decides the frame rate
			if (ci == C_STATIC) { CHECK(r.qpAvg <= 30.0); CHECK(r.fps >= 3.0); }					// small motion is cheap: the quality stays high and the frames keep coming
			if (ci == C_SCROLL && links[li].bps >= 375000) { CHECK(r.fps >= 12.0); }
			if (ci == C_SCROLL && links[li].bps >= 375000) { CHECK(r.rAvg * 8 / 1000 >= (links[li].bps >= 625000 ? 3300.0 : 2400.0)); }	// R follows the link (not 0.85 of 0.85 of ... : the sharpening gate must not make the sender "link limited")			// the 25 ms encode of 1080p on 2 vCPU caps the frame rate at 20 (share 50 %)
			CHECK(r.fps <= 27.0 || ci == C_STATIC);	// the encode cap (1.5 x the 25-28 ms of encoding = 38-42 ms, the slots keep the rate at 1 / interval: 26 and 24 a second); it was 21 with twice the encode time
		}
	}
	// the viewer's budget is respected on a fast link: budget 2 Mb/s on 50 Mb/s: the video is at most 0.8 x 2 Mb/s (+ 10 %)
	{
		sim_result r;
		printf("\n| budget case | content | R kb/s | video kb/s | video/R | QP start / avg / end / max | fps | latency avg / max ms | sharpening share |\n|---|---|---|---|---|---|---|---|---|\n");
		for (ci = 0; ci < 3; ci++)
		{
			r = simulate(6250000, 2000, (content_t)ci, 70, 20, 0);
			row("50 Mb/s", (content_t)ci, &r);
			CHECK(r.videoBps * 8 <= 0.8 * 2000000.0 * 1.12);
			CHECK(r.ratio <= 1.12);
			CHECK(r.fps >= 0.3);
			if (ci != C_STATIC) { CHECK(r.videoBps * 8 >= 0.5 * 0.8 * 2000000.0); }	// and the budget is really used (R does not decay towards what was sent)
		}
		r = simulate(6250000, 0, C_NOISE, 70, 20, 0);
		CHECK(r.videoBps * 8 > 0.8 * 2000000.0 * 4.0);		// without the budget the same stream takes far more (so the test above tests the budget)
		printf("(no budget, noise, 50 Mb/s: %.0f kb/s)\n", r.videoBps * 8 / 1000);
	}
	// longer runs, a budget that comes and goes, a link that drops, the key frame at a low rate
	test_simulation_review();
	// a QP trajectory (noise at 1 Mb/s)
	{
		printf("\nnoise, 1 Mb/s link, trajectory:\n");
		(void)simulate(125000, 0, C_NOISE, 30, 20, 1);
	}
}

// the encode-rate cap: the interval is at least encode time x cores busy / budget, one core of budget on 2 cores
static void test_enc_cap(void)
{
	uint32_t i;
	kvm_budget b;
	kvm_budget_init(&b, 2);
	for (i = 5000; i <= 60000; i += 5000)
	{
		uint32_t iv = kvm_pace_interval(&b, 16, i);
		CHECK((uint64_t)i * 15u <= (uint64_t)iv * 10u * 1000u || iv == KVM_PACE_INTERVAL_MAX_MS);	// the CPU of the encoder (1.5 cores while it runs) <= the budget (1 core) x interval
		CHECK(iv >= 16);
	}
	CHECK_EQ(kvm_pace_interval(&b, 16, 30000), 45);
	CHECK_EQ(kvm_pace_interval(&b, 16, 4000), 16);
}

// the hardware encoder. Its key frame is not bigger than the software one (8 % smaller on the same picture), so the prior and the slope are the software ones; what changes is the scale of the QP: the
// regulator keeps its scale (26 .. 38) but the hardware encoder is given 4 less and is floored at 26, so the regulator has its own floor at 30 for it (no step is wasted).
static void test_hw_backend(void)
{
	kvm_vrc r;
	int i, qp;
	// the mapping of the QP (kvm_vcore_hw_qp)
	CHECK_EQ(kvm_vcore_hw_qp(26), 26); CHECK_EQ(kvm_vcore_hw_qp(28), 26); CHECK_EQ(kvm_vcore_hw_qp(30), 26); CHECK_EQ(kvm_vcore_hw_qp(31), 27); CHECK_EQ(kvm_vcore_hw_qp(34), 30); CHECK_EQ(kvm_vcore_hw_qp(38), 34);
	CHECK_EQ(kvm_vcore_hw_qp(0), 26); CHECK_EQ(kvm_vcore_hw_qp(51), 47); CHECK_EQ(kvm_vcore_hw_qp(60), 51);
	for (qp = 0; qp <= 60; qp++) { CHECK(kvm_vcore_hw_qp(qp) >= 26 && kvm_vcore_hw_qp(qp) <= 51 && kvm_vcore_hw_qp(qp) <= kvm_vcore_hw_qp(qp + 1)); }
	// the whole range of the regulator maps into what the encoder obeys, and the floor of the regulator is the first value that is not at the floor of the encoder
	CHECK_EQ(KVM_VIDEO_QP_MIN - KVM_HW_QP_OFFSET, 30); CHECK_EQ(kvm_vcore_hw_qp(KVM_VIDEO_QP_MIN - KVM_HW_QP_OFFSET), KVM_HW_QP_FLOOR);
	CHECK(kvm_vcore_hw_qp(KVM_VIDEO_QP_MIN - KVM_HW_QP_OFFSET + 1) > KVM_HW_QP_FLOOR);
	// the backend
	kvm_vrc_init(&r, 28, 0);
	CHECK_EQ(r.idrPriorBytes, KVM_VRC_IDR_PRIOR_BYTES); CHECK_EQ(r.idrSlopePct, KVM_VRC_IDR_SLOPE_PCT); CHECK_EQ(r.hw, 0); CHECK_EQ(r.qpMin, KVM_VIDEO_QP_MIN); CHECK_EQ(r.qp, 28);
	kvm_vrc_set_backend(&r, 1);
	CHECK_EQ(r.hw, 1); CHECK_EQ(r.qpMin, 30); CHECK_EQ(r.qp, 30);							// the start QP 28 is raised to the floor
	CHECK_EQ(r.idrPriorBytes, KVM_VRC_HW_IDR_PRIOR_BYTES); CHECK_EQ(r.idrSlopePct, KVM_VRC_HW_IDR_SLOPE_PCT); CHECK_EQ(r.lastIdrBytes, KVM_VRC_HW_IDR_PRIOR_BYTES);
	CHECK_EQ(KVM_VRC_HW_IDR_PRIOR_BYTES, KVM_VRC_IDR_PRIOR_BYTES * 11 / 10); CHECK_EQ(KVM_VRC_HW_IDR_SLOPE_PCT, KVM_VRC_IDR_SLOPE_PCT);	// (12.5: at the QP of the regulator about 10 % more than the software encoder, the slope is the same)
	// a new regulator for a new encoder (kvm_vrc_init_backend) cannot forget which encoder it is for; the software init is the software encoder
	kvm_vrc_set_backend(&r, 1);
	kvm_vrc_init_backend(&r, 28, 0, 1);
	CHECK_EQ(r.hw, 1); CHECK_EQ(r.qpMin, 30); CHECK_EQ(r.qp, 30); CHECK_EQ(r.idrPriorBytes, KVM_VRC_HW_IDR_PRIOR_BYTES); CHECK_EQ(r.lastIdrBytes, KVM_VRC_HW_IDR_PRIOR_BYTES); CHECK_EQ(r.lastIdrQp, KVM_VRC_IDR_PRIOR_QP);
	kvm_vrc_init(&r, 28, 0);
	CHECK_EQ(r.hw, 0); CHECK_EQ(r.qpMin, KVM_VIDEO_QP_MIN); CHECK_EQ(r.qp, 28); CHECK_EQ(r.idrPriorBytes, KVM_VRC_IDR_PRIOR_BYTES);
	kvm_vrc_init_backend(&r, 38, 0, 0);
	CHECK_EQ(r.hw, 0); CHECK_EQ(r.qpMin, KVM_VIDEO_QP_MIN); CHECK_EQ(r.qp, 38);
	kvm_vrc_init_backend(&r, 38, 0, 1);
	CHECK_EQ(r.hw, 1); CHECK_EQ(r.qp, 38);
	// the regulator does not go below its floor: quiet windows for a long time
	{
		uint32_t t = 1000;
		kvm_vrc_init(&r, 36, t); kvm_vrc_set_backend(&r, 1); kvm_vrc_set_target(&r, 2000000, t);
		for (i = 0; i < 1500; i++)
		{
			t += 20;
			kvm_vrc_update(&r, t, 0, 2000000, 0);
			if (kvm_vrc_may_encode(&r, t, 0)) { kvm_vrc_frame(&r, t, 2000, r.qp, 0, 5000); }
			(void)kvm_vrc_tick(&r, t);
			CHECK(r.qp >= 30 && r.qp <= KVM_VIDEO_QP_MAX);
		}
		CHECK_EQ(r.qp, 30);
		// and the software one goes down to 26 in the same situation
		t = 1000; kvm_vrc_init(&r, 36, t); kvm_vrc_set_target(&r, 2000000, t);
		for (i = 0; i < 1500; i++)
		{
			t += 20;
			kvm_vrc_update(&r, t, 0, 2000000, 0);
			if (kvm_vrc_may_encode(&r, t, 0)) { kvm_vrc_frame(&r, t, 2000, r.qp, 0, 5000); }
			(void)kvm_vrc_tick(&r, t);
		}
		CHECK_EQ(r.qp, KVM_VIDEO_QP_MIN);
	}
	// the key frame: not below the floor either (R = 100 MB/s: everything fits, the QP of the P frames decides)
	kvm_vrc_init(&r, 26, 0); kvm_vrc_set_target(&r, 100000000, 0); CHECK_EQ(kvm_vrc_qp(&r, 1), 26);
	kvm_vrc_init(&r, 26, 0); kvm_vrc_set_backend(&r, 1); kvm_vrc_set_target(&r, 100000000, 0); CHECK_EQ(kvm_vrc_qp(&r, 1), 30);
	r.qp = 26; CHECK_EQ(kvm_vrc_qp(&r, 1), 30);				// (even when the QP of the P frames was below the floor)
	// the slope is a field of the regulator: a prediction with 96 % per QP (the hardware encoder's own, if a measurement says so) differs from one with 94 %
	kvm_vrc_init(&r, 26, 0); kvm_vrc_set_target(&r, 1200000, 0); kvm_vrc_frame(&r, 1, 400000, 30, 1, 20000);
	CHECK_EQ(kvm_vrc_qp(&r, 1), 32);
	r.idrSlopePct = 96; CHECK_EQ(kvm_vrc_qp(&r, 1), 33);
	kvm_vrc_init(&r, 26, 0); kvm_vrc_set_target(&r, 1600000, 0); kvm_vrc_frame(&r, 1, 400000, 30, 1, 20000);
	CHECK_EQ(kvm_vrc_qp(&r, 1), 28); r.idrSlopePct = 96; CHECK_EQ(kvm_vrc_qp(&r, 1), 26);	// (downwards from the measured QP too)
	// a prior of its own (a field too)
	kvm_vrc_init(&r, 26, 0); kvm_vrc_set_target(&r, 1500000, 0); r.idrPriorBytes = 450000; kvm_vrc_set_backend(&r, 0); r.idrPriorBytes = 450000; r.lastIdrBytes = 450000; CHECK_EQ(kvm_vrc_qp(&r, 1), 28);
	// a measured key frame stays when the backend is set again, and when the picture size is told again
	kvm_vrc_init(&r, 26, 0); kvm_vrc_set_target(&r, 1200000, 0); kvm_vrc_frame(&r, 1, 400000, 30, 1, 20000);
	kvm_vrc_set_backend(&r, 1); CHECK_EQ(r.lastIdrBytes, 400000); CHECK_EQ(r.lastIdrQp, 30);
	kvm_vrc_set_backend(&r, 0); CHECK_EQ(r.lastIdrBytes, 400000); CHECK_EQ(r.qpMin, KVM_VIDEO_QP_MIN);
	kvm_vrc_clear_stats(&r); kvm_vrc_set_picture(&r, 3840u * 2160u); CHECK_EQ(r.lastIdrBytes, 400000);	// (the statistics are cleared every second: sIdr is 0 again)
	// back to the software encoder before any key frame: the software prior
	kvm_vrc_init(&r, 26, 0); kvm_vrc_set_backend(&r, 1); kvm_vrc_set_backend(&r, 0); CHECK_EQ(r.lastIdrBytes, KVM_VRC_IDR_PRIOR_BYTES);
	// the ceiling of the key frame QP is 38 for both backends
	kvm_vrc_init(&r, 26, 0); kvm_vrc_set_backend(&r, 1); kvm_vrc_set_target(&r, 53125, 0); CHECK_EQ(kvm_vrc_qp(&r, 1), 38);
}



/* the key frame is proportional to the area that it converts; the virgin macroblocks of a P frame are not the content getting busier */
static void test_idr_cover(void)
{
	kvm_vrc r;
	uint32_t full, half, tenth, tiny, b;
	const uint32_t total = 8160u;
	kvm_vrc_init(&r, 28, 1000);
	kvm_vrc_set_picture(&r, 1920u * 1088u);
	/* nothing planned: the prediction of before; the whole picture planned is the same */
	full = kvm_vrc_idr_bytes(&r, 28);
	CHECK_EQ(full, r.lastIdrBytes);
	kvm_vrc_set_idr_cover(&r, total, total);
	CHECK(kvm_vrc_idr_bytes(&r, 28) >= full - 2u && kvm_vrc_idr_bytes(&r, 28) <= full + 2u);
	/* a tenth of the picture: about a tenth of the content plus the floor; always more than the floor, less than the whole */
	kvm_vrc_set_idr_cover(&r, total / 10u, total);
	tenth = kvm_vrc_idr_bytes(&r, 28);
	CHECK(tenth < full / 8u && tenth > (total * KVM_VRC_IDR_FLAT_MILLI) / 1000u);
	kvm_vrc_set_idr_cover(&r, total / 2u, total);
	half = kvm_vrc_idr_bytes(&r, 28);
	CHECK(half > tenth && half < full && half > full / 3u);
	kvm_vrc_set_idr_cover(&r, 4u, total);
	tiny = kvm_vrc_idr_bytes(&r, 28);
	CHECK(tiny >= (total * KVM_VRC_IDR_FLAT_MILLI) / 1000u && tiny < tenth);
	/* the QP: the same slope as before for every coverage */
	kvm_vrc_set_idr_cover(&r, total / 10u, total);
	CHECK(kvm_vrc_idr_bytes(&r, 26) > kvm_vrc_idr_bytes(&r, 32) && kvm_vrc_idr_bytes(&r, 32) > kvm_vrc_idr_bytes(&r, 38));
	/* a measured key frame of a tenth: the next one of a tenth is predicted as the measurement, the next one of the whole picture is the measurement scaled up (above the floor) */
	kvm_vrc_set_idr_cover(&r, total / 10u, total);
	kvm_vrc_frame(&r, 1100, 30000u, 28, 1, 5000u);
	CHECK_EQ(r.lastIdrReal, total / 10u); CHECK_EQ(r.idrReal, 0u);
	kvm_vrc_set_idr_cover(&r, total / 10u, total);
	b = kvm_vrc_idr_bytes(&r, 28);
	CHECK(b >= 29900u && b <= 30100u);
	kvm_vrc_set_idr_cover(&r, total, total);
	b = kvm_vrc_idr_bytes(&r, 28);
	CHECK(b > 200000u && b < 330000u);
	kvm_vrc_set_idr_cover(&r, total / 2u, total);
	b = kvm_vrc_idr_bytes(&r, 28);
	CHECK(b > 100000u && b < 170000u);
	/* a whole picture after a measured part: the picture has the macroblocks that set_picture said, the forecast is the one of the whole picture */
	kvm_vrc_set_idr_cover(&r, 0, 0);
	b = kvm_vrc_idr_bytes(&r, 28);
	CHECK(b > 200000u && b < 330000u);
	kvm_vrc_set_idr_cover(&r, total / 2u, total);
	/* the cover is reset by the key frame (the next one that is not planned is a whole picture) */
	CHECK_EQ(r.idrReal, total / 2u);
	kvm_vrc_frame(&r, 1200, 90000u, 28, 1, 5000u);
	CHECK_EQ(r.idrReal, 0u); CHECK_EQ(r.lastIdrReal, total / 2u);
	/* the virgin macroblocks: about what an area of that size costs in a key frame, none when there are none */
	kvm_vrc_set_idr_cover(&r, total / 2u, total);
	CHECK_EQ(kvm_vrc_virgin_bytes(&r, 0, 28), 0u);
	b = kvm_vrc_virgin_bytes(&r, total / 4u, 28);
	CHECK(b > 30000u && b < 60000u);
	CHECK(kvm_vrc_virgin_bytes(&r, 2u * (total / 4u), 28) >= 2u * b - 4u);
	/* the floor can be set (the self test): a bigger floor, a bigger tiny key frame */
	kvm_vrc_set_idr_cover(&r, 4u, total);
	{ const uint32_t t0 = kvm_vrc_idr_bytes(&r, 28); kvm_vrc_set_flat_floor(&r, 1000u); CHECK(kvm_vrc_idr_bytes(&r, 28) > t0 + 4000u); kvm_vrc_set_flat_floor(&r, 9000000u); CHECK_EQ(r.flatMilli, 4000u); }
	/* the regulator counts the content, not the virgin macroblocks: the bucket pays all of them */
	{
		kvm_vrc a, c;
		kvm_vrc_init(&a, 28, 1000); kvm_vrc_init(&c, 28, 1000);
		kvm_vrc_frame_virgin(&a, 1100, 40000u, 28, 0, 5000u, 30000u);
		kvm_vrc_frame_virgin(&c, 1100, 40000u, 28, 0, 5000u, 0u);
		CHECK_EQ(a.winBytes, 10000u); CHECK_EQ(c.winBytes, 40000u);
		CHECK_EQ(a.tokens, c.tokens);
		kvm_vrc_frame_virgin(&a, 1200, 1000u, 28, 0, 5000u, 30000u);	/* more virgin bytes than the frame: nothing negative */
		CHECK_EQ(a.winBytes, 10000u);
	}
}

int main(void)
{
	test_idr_cover();
	test_target();
	test_hw_backend();
	test_regulator(1000);
	test_regulator(0xFFFFFC00u);
	test_capacity(1000);
	test_capacity(0xFFFFFF00u);
	test_capacity_bound(1000);
	test_capacity_bound(0xFFFF0000u);
	test_idle_qp(1000);
	test_idle_qp(0xFFFFE000u);
	test_starved();
	test_link_held();
	test_idr_qp();
	test_idr_unsaturated(1000);
	test_idr_proof(1000);
	test_lower_bound(1000);
	test_idr_proof(0xFFFFFC00u);
	test_z4(1000);
	test_z4(0xFFFFFC00u);
	test_bucket(1000);
	test_bucket(0xFFFFF000u);
	test_enc_cap();
	test_simulation();
	printf("%d checks, %d failed\n", gChecks, gFail);
	return gFail == 0 ? 0 : 1;
}
