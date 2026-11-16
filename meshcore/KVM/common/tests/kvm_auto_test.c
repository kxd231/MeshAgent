/*
tests of kvm_auto.c, the decision of the Auto mode of the frame rate (kvm_auto_decide) and the accumulators that close into its windows (kvm_auto_note_*, kvm_auto_tick). Pure functions, time
is injected, so nothing is measured and no benchmark lock is needed. Built by run-kvm-auto-tests.sh (gcc -std=c99 -Wall -Wextra -Werror -pedantic, then ASan + UBSan); the mutations of
run-kvm-auto-mutations.sh change one rule of kvm_auto.c at a time and these tests have to turn red.
*/

#include "../kvm_auto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gFail = 0, gChecks = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); gChecks++; if (_a != _b) { gFail++; printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

// a window of a machine that has nothing to complain about: 60 frames in a second of motion at QP 26
static kvm_auto_win good_win(void)
{
	kvm_auto_win w;
	memset(&w, 0, sizeof(w));
	w.windowMs = 1000; w.motionMs = 1000; w.made = 60; w.pendingIters = 60;
	w.qpN = 60; w.qpSum = 60u * 26u; w.qpEnd = 26;
	w.capP75Ms = 8; w.gapMedMs = 16;
	return w;
}

// the same at the rate of step `s` (frames of one second of motion at the QP `qp`)
static kvm_auto_win win_at(int s, int qp)
{
	kvm_auto_win w = good_win();
	w.made = (uint32_t)kvm_auto_step_fps(s); w.pendingIters = w.made;
	w.qpN = w.made; w.qpSum = w.made * (uint32_t)qp; w.qpEnd = (uint32_t)qp;
	w.capP75Ms = 5; w.gapMedMs = kvm_auto_step_ms(s);
	return w;
}

static kvm_auto_env env_at(uint32_t now)
{
	kvm_auto_env e;
	memset(&e, 0, sizeof(e));
	e.now = now; e.valid = 1; e.cls = KVM_AUTO_CLASS_UI; e.minRttMs = 5;
	e.encAvgUs = 8000; e.parX10 = 15; e.budgetMilli = 2000; e.floorMs = 0;
	return e;
}

// Auto on, started at `step` at time t0 (a start from the round trip, then a step down or none)
static uint32_t gT;
static void begin(kvm_auto *a, int step)
{
	kvm_auto_dec d;
	kvm_auto_env e;
	kvm_auto_init(a);
	gT = 100000;
	kvm_auto_set(a, 1, gT);
	e = env_at(gT);
	e.minRttMs = step == 0 ? 5 : 40;
	(void)kvm_auto_decide(a, NULL, &e, &d);
	CHECK(a->started);
	if (step >= 2) { a->step = (uint8_t)step; }
	// the start is long ago: nothing holds a step back
	gT += 120000;
	a->lastChangeAt = gT - 100000;
}

// one evaluated window one second after the last one; returns dec->changed
static kvm_auto_dec gD;
static int win(kvm_auto *a, const kvm_auto_win *w, kvm_auto_env e)
{
	gT += 1000;
	e.now = gT;
	return kvm_auto_decide(a, w, &e, &gD);
}
static int winU(kvm_auto *a, const kvm_auto_win *w) { return win(a, w, env_at(0)); }

static void test_tables(void)
{
	static const int fps[5] = { 60, 30, 20, 15, 10 };
	static const int ms[5] = { 16, 33, 50, 66, 100 };
	static const int ui[5] = { 32, 32, 34, 36, 38 };
	static const int vid[5] = { 34, 34, 36, 38, 38 };
	int i;
	for (i = 0; i < 5; i++)
	{
		CHECK_EQ(kvm_auto_step_fps(i), fps[i]);
		CHECK_EQ((int)kvm_auto_step_ms(i), ms[i]);
		CHECK_EQ(kvm_auto_qp_ceiling(KVM_AUTO_CLASS_UI, i), ui[i]);
		CHECK_EQ(kvm_auto_qp_ceiling(KVM_AUTO_CLASS_VIDEO, i), vid[i]);
	}
	CHECK_EQ(kvm_auto_step_fps(-3), 60);
	CHECK_EQ(kvm_auto_step_fps(9), 10);
	CHECK_EQ(KVM_AUTO_OP1_FLAG, 8);
	CHECK_EQ(KVM_CAPS_AUTO, 4);
	CHECK(strcmp(kvm_auto_reason_name(KVM_AUTO_R_LINK), "link") == 0);
	CHECK(strcmp(kvm_auto_reason_name(KVM_AUTO_R_CAPTURE), "capture") == 0);
	CHECK(strcmp(kvm_auto_reason_name(KVM_AUTO_R_BACK), "back") == 0);
	CHECK(strcmp(kvm_auto_reason_name(0), "none") == 0);
}

// the bit down: nothing happens, whatever the notes and the windows say
static void test_off(void)
{
	kvm_auto a, b;
	kvm_auto_win w = good_win();
	kvm_auto_env e = env_at(5000);
	kvm_auto_dec d;
	kvm_auto_init(&a);
	CHECK(!kvm_auto_on(&a));
	CHECK_EQ(kvm_auto_interval_ms(&a), 0);
	CHECK_EQ(kvm_auto_fps(&a), 0);
	memcpy(&b, &a, sizeof(a));
	kvm_auto_note_capture(&a, 30, 2);
	kvm_auto_note_image(&a, 1000, 5);
	kvm_auto_note_held(&a, 0);
	kvm_auto_note_held(&a, 1);
	kvm_auto_note_frame(&a, 1100, 30, 1000);
	CHECK(memcmp(&a, &b, sizeof(a)) == 0);	// not one byte of the state moved
	w.made = 5; w.qpEnd = 40; w.heldWin = 50;
	CHECK_EQ(kvm_auto_decide(&a, &w, &e, &d), 0);
	CHECK_EQ(d.changed, 0);
	CHECK_EQ(d.evaluated, 0);
	CHECK_EQ(d.intervalMs, 0);
	e.now = 9000;
	CHECK_EQ(kvm_auto_tick(&a, &e, &d, NULL), 0);
	e.now = 20000;
	CHECK_EQ(kvm_auto_tick(&a, &e, &d, NULL), 0);
	CHECK(memcmp(&a, &b, sizeof(a)) == 0);
	CHECK_EQ(kvm_auto_interval_ms(&a), 0);
	e.maxFps = 10;
	CHECK_EQ(kvm_auto_decide(&a, &w, &e, &d), 0);
	// going up, then down: nothing is left behind
	kvm_auto_set(&a, 1, 1000);
	CHECK(kvm_auto_on(&a));
	CHECK_EQ(kvm_auto_interval_ms(&a), 0);			// before the start: no override, the period of command 5 (Medium or the viewer's own last step) holds
	kvm_auto_set(&a, 1, 2000);						// again: no reset
	CHECK_EQ((int)a.beginAt, 1000);
	a.step = 3; a.changes = 7;
	kvm_auto_set(&a, 0, 3000);
	CHECK(!kvm_auto_on(&a));
	CHECK_EQ(kvm_auto_interval_ms(&a), 0);
	CHECK_EQ((int)a.changes, 0);
	kvm_auto_set(&a, 1, 4000);
	CHECK_EQ((int)a.step, 1);
	CHECK_EQ((int)a.changes, 0);
}

static void test_start(void)
{
	kvm_auto a;
	kvm_auto_dec d;
	kvm_auto_env e;
	// a LAN: 60
	kvm_auto_init(&a); kvm_auto_set(&a, 1, 1000);
	e = env_at(1100); e.minRttMs = 19;
	CHECK_EQ(kvm_auto_decide(&a, NULL, &e, &d), 1);
	CHECK_EQ((int)a.step, 0); CHECK_EQ(d.reason, KVM_AUTO_R_START); CHECK_EQ(d.from, 1); CHECK_EQ(d.to, 0);
	CHECK_EQ(kvm_auto_interval_ms(&a), 16); CHECK_EQ(kvm_auto_fps(&a), 60);
	CHECK_EQ((int)a.ups, 0);							// the start is no step up that can fail
	CHECK_EQ((int)a.upNeed, KVM_AUTO_UP_WINDOWS);
	// 20 ms is not a LAN
	kvm_auto_init(&a); kvm_auto_set(&a, 1, 1000);
	e = env_at(1100); e.minRttMs = 20;
	CHECK_EQ(kvm_auto_decide(&a, NULL, &e, &d), 0);
	CHECK(a.started); CHECK_EQ((int)a.step, 1); CHECK_EQ(d.reason, KVM_AUTO_R_START);
	// no round trip yet: 30 meanwhile (the interval of command 5), not started, the decision waits
	kvm_auto_init(&a); kvm_auto_set(&a, 1, 1000);
	e = env_at(1100); e.minRttMs = 0;
	CHECK_EQ(kvm_auto_decide(&a, NULL, &e, &d), 0);
	CHECK(!a.started); CHECK_EQ(kvm_auto_interval_ms(&a), 0); CHECK_EQ(kvm_auto_fps(&a), 30);
	e.now = 1000 + KVM_AUTO_START_WAIT_MS - 1;
	CHECK_EQ(kvm_auto_decide(&a, NULL, &e, &d), 0);
	CHECK(!a.started);
	e.now = 1000 + KVM_AUTO_START_WAIT_MS;
	CHECK_EQ(kvm_auto_decide(&a, NULL, &e, &d), 0);
	CHECK(a.started); CHECK_EQ((int)a.step, 1);
	// the round trip arrives late: the start is not repeated
	kvm_auto_init(&a); kvm_auto_set(&a, 1, 1000);
	e = env_at(2000); e.minRttMs = 0;
	(void)kvm_auto_decide(&a, NULL, &e, &d);
	e = env_at(3000); e.minRttMs = 8;
	CHECK_EQ(kvm_auto_decide(&a, NULL, &e, &d), 1);
	CHECK_EQ((int)a.step, 0);
	e = env_at(4000); e.minRttMs = 80;
	CHECK_EQ(kvm_auto_decide(&a, NULL, &e, &d), 0);
	CHECK_EQ((int)a.step, 0);
	// a window before the start decides nothing
	kvm_auto_init(&a); kvm_auto_set(&a, 1, 1000);
	{ kvm_auto_win w = win_at(1, 40); w.heldWin = 30; e = env_at(2000); e.minRttMs = 0; CHECK_EQ(kvm_auto_decide(&a, &w, &e, &d), 0); CHECK_EQ(d.evaluated, 0); }
	// the start counts as a change in time: no step in the 2 s after it, an improvement not before 4 s
	kvm_auto_init(&a); kvm_auto_set(&a, 1, 1000);
	e = env_at(1100); e.minRttMs = 40;
	(void)kvm_auto_decide(&a, NULL, &e, &d);
	CHECK_EQ((int)a.lastChangeAt, 1100);
}

static void test_frozen(void)
{
	kvm_auto a;
	kvm_auto_win w = win_at(0, 26), quiet;
	kvm_auto_env e;
	begin(&a, 0);
	w.made = 20; w.pendingIters = 20; w.qpN = 20; w.qpSum = 520;			// 20 frames in a second: 20 fps (below 45)
	CHECK_EQ(winU(&a, &w), 0);
	CHECK_EQ(gD.evaluated, 1); CHECK_EQ((int)a.cpuStreak, 1);
	// a window with 200 ms of motion is not evaluated and does not break the streak
	quiet = w; quiet.motionMs = 200; quiet.made = 5;
	CHECK_EQ(winU(&a, &quiet), 0);
	CHECK_EQ(gD.evaluated, 0); CHECK_EQ((int)a.cpuStreak, 1);
	quiet.motionMs = 0; quiet.made = 0;
	CHECK_EQ(winU(&a, &quiet), 0);
	CHECK_EQ((int)a.cpuStreak, 1);
	// not valid (a new generation, the video off): not evaluated
	e = env_at(0); e.valid = 0;
	CHECK_EQ(win(&a, &w, e), 0);
	CHECK_EQ(gD.evaluated, 0); CHECK_EQ((int)a.cpuStreak, 1);
	// 300 ms is enough
	quiet = w; quiet.motionMs = KVM_AUTO_MIN_MOTION_MS; quiet.made = 6; quiet.pendingIters = 6; quiet.qpN = 6; quiet.qpSum = 156;
	CHECK_EQ(winU(&a, &quiet), 1);									// the second evaluated window of a machine that does not keep up: down
	CHECK_EQ(gD.evaluated, 1); CHECK_EQ(gD.reason, KVM_AUTO_R_CAPTURE);		// (the achieved rate is low and the desktop manager says nothing: GDI, the capture)
	// a silence of more than 30 s between two evaluated windows: no streak across it
	begin(&a, 0);
	CHECK_EQ(winU(&a, &w), 0);
	CHECK_EQ((int)a.cpuStreak, 1);
	gT += KVM_AUTO_STALE_MS;
	CHECK_EQ(winU(&a, &w), 0);										// 31 s after the first one
	CHECK_EQ((int)a.cpuStreak, 1);
	begin(&a, 0);
	CHECK_EQ(winU(&a, &w), 0);
	gT += KVM_AUTO_STALE_MS - 1000;									// exactly 30 s since the last one: still a streak
	CHECK_EQ(winU(&a, &w), 1);
}

// the machine does not keep up
static void test_cpu(void)
{
	kvm_auto a;
	kvm_auto_win w;
	// 44 frames a second of 60: not kept; 45: kept (the uneven 45..55 of the slow laptop stays at 60)
	begin(&a, 0);
	w = win_at(0, 26); w.made = 45; w.pendingIters = 45; w.qpN = 45; w.qpSum = 45u * 26u;
	CHECK_EQ(winU(&a, &w), 0); CHECK_EQ(winU(&a, &w), 0); CHECK_EQ(winU(&a, &w), 0);
	CHECK_EQ((int)a.step, 0); CHECK(!(gD.flags & KVM_AUTO_F_LIMITED)); CHECK_EQ((int)a.cpuStreak, 0);
	w.made = 44; w.pendingIters = 44; w.qpN = 44; w.qpSum = 44u * 26u;
	CHECK_EQ(winU(&a, &w), 0);
	CHECK(gD.flags & KVM_AUTO_F_LIMITED); CHECK(gD.flags & KVM_AUTO_F_ACHIEVED); CHECK_EQ((int)a.cpuStreak, 1); CHECK_EQ((int)a.step, 0);
	CHECK_EQ(winU(&a, &w), 1);
	// 44 fps: the interval that fits 44 fps = 22.7 ms: the step of 33 ms (30 fps)
	CHECK_EQ((int)a.step, 1); CHECK_EQ(gD.reason, KVM_AUTO_R_CAPTURE); CHECK_EQ(gD.from, 0); CHECK_EQ(gD.to, 1); CHECK_EQ(gD.intervalMs, 33);
	CHECK_EQ(kvm_auto_interval_ms(&a), 33);
	CHECK_EQ((int)a.cpuStreak, 0);
	// the rate is counted over the time with motion, not over the window: 30 frames in 500 ms of motion are 60 a second
	begin(&a, 0);
	w = win_at(0, 26); w.motionMs = 500; w.made = 30; w.pendingIters = 30; w.qpN = 30; w.qpSum = 30u * 26u;
	CHECK_EQ(winU(&a, &w), 0); CHECK_EQ(winU(&a, &w), 0); CHECK_EQ(gD.achieved10, 600); CHECK_EQ((int)a.cpuStreak, 0);
	w.motionMs = 1000;															// the same frames in 1000 ms: 30 a second
	CHECK_EQ(winU(&a, &w), 0); CHECK_EQ(gD.achieved10, 300); CHECK_EQ((int)a.cpuStreak, 1);
	// frames held back by the window or the bucket: that is the link, not a slow machine
	begin(&a, 0);
	w = win_at(0, 26); w.made = 20; w.pendingIters = 20; w.heldWin = 1;
	CHECK_EQ(winU(&a, &w), 0); CHECK(!(gD.flags & KVM_AUTO_F_ACHIEVED)); CHECK_EQ((int)a.cpuStreak, 0);
	w.heldWin = 0; w.heldRate = 1;
	CHECK_EQ(winU(&a, &w), 0); CHECK(!(gD.flags & KVM_AUTO_F_ACHIEVED)); CHECK_EQ((int)a.cpuStreak, 0);
	// no image had tiles waiting: the source stands still more than the machine is slow
	w.heldRate = 0; w.pendingIters = 0;
	CHECK_EQ(winU(&a, &w), 0); CHECK(!(gD.flags & KVM_AUTO_F_ACHIEVED)); CHECK_EQ((int)a.cpuStreak, 0);
	// a slow source: the loop takes every image of the desktop manager (dwm 1.0 per image): not a limit of the machine
	w.pendingIters = 20; w.dwmX100 = 100;
	CHECK_EQ(winU(&a, &w), 0); CHECK(!(gD.flags & KVM_AUTO_F_ACHIEVED)); CHECK_EQ((int)a.cpuStreak, 0);
	w.dwmX100 = KVM_AUTO_DWM_SLOW_X100 - 1;
	CHECK_EQ(winU(&a, &w), 0); CHECK_EQ((int)a.cpuStreak, 0);
	w.dwmX100 = KVM_AUTO_DWM_SLOW_X100;							// the loop leaves images of the source out: it is the machine
	CHECK_EQ(winU(&a, &w), 0); CHECK_EQ((int)a.cpuStreak, 1);
	w.dwmX100 = 0;												// not known (GDI): the achieved rate is all there is
	CHECK_EQ(winU(&a, &w), 1);
	// the jump: 15 fps achieved = 66 ms needed: the step is the first whose interval is at least 0.75 x the need (50 ms: a machine of 15 frames a second makes 75 % of 20), several steps in one go
	begin(&a, 0);
	w = win_at(0, 26); w.made = 15; w.pendingIters = 15; w.qpN = 15; w.qpSum = 15u * 26u;
	(void)winU(&a, &w); CHECK_EQ(winU(&a, &w), 1);
	CHECK_EQ((int)a.step, 2); CHECK_EQ((int)gD.needMs, 66);
	begin(&a, 0);
	w.made = 14; w.pendingIters = 14; w.qpN = 14; w.qpSum = 14u * 26u;						// 71 ms: 50 ms is 70 % of it, the next one
	(void)winU(&a, &w); CHECK_EQ(winU(&a, &w), 1);
	CHECK_EQ((int)a.step, 3);
	// 8 fps: needs 125 ms: the last step
	begin(&a, 0);
	w.made = 8; w.pendingIters = 8; w.qpN = 8; w.qpSum = 8u * 26u;
	(void)winU(&a, &w); CHECK_EQ(winU(&a, &w), 1);
	CHECK_EQ((int)a.step, 4);
	// nothing below the last step
	(void)winU(&a, &w); (void)winU(&a, &w); CHECK_EQ(winU(&a, &w), 0);
	CHECK_EQ((int)a.step, 4);
	// the encoder: the interval of its CPU budget above the target (B): the step that holds it
	begin(&a, 0);
	w = win_at(0, 26);
	{
		kvm_auto_env e = env_at(0);
		e.floorMs = 17;
		CHECK_EQ(win(&a, &w, e), 0); CHECK(gD.flags & KVM_AUTO_F_FLOOR);
		CHECK_EQ(win(&a, &w, e), 1);
		CHECK_EQ((int)a.step, 1); CHECK_EQ(gD.reason, KVM_AUTO_R_CPU);
		begin(&a, 0);
		e.floorMs = 16;												// equal to the target: fine
		CHECK_EQ(win(&a, &w, e), 0); CHECK_EQ(win(&a, &w, e), 0); CHECK(!(gD.flags & KVM_AUTO_F_FLOOR));
		begin(&a, 0);
		e.floorMs = 67;												// 0.75 x 67 = 50.25 > 50: 50 ms is too short, 66 holds it
		(void)win(&a, &w, e); CHECK_EQ(win(&a, &w, e), 1);
		CHECK_EQ((int)a.step, 3);
		begin(&a, 0);
		e.floorMs = 66;												// 0.75 x 66 = 49.5: 50 ms holds it
		(void)win(&a, &w, e); CHECK_EQ(win(&a, &w, e), 1);
		CHECK_EQ((int)a.step, 2);
		begin(&a, 0);
		e.floorMs = 0;												// unknown: no rule
		(void)win(&a, &w, e); CHECK_EQ(win(&a, &w, e), 0);
	}
}

// the capture and the loop
static void test_capture(void)
{
	kvm_auto a;
	kvm_auto_win w;
	// P75 of the capture time above 4/3 of the interval, counted from 1000 / 60 = 16.67 ms: 22 ms fits (22.2), 23 does not
	begin(&a, 0);
	w = win_at(0, 26); w.capP75Ms = 22;
	CHECK_EQ(winU(&a, &w), 0); CHECK(!(gD.flags & KVM_AUTO_F_CAPTURE)); CHECK_EQ((int)a.cpuStreak, 0);
	w.capP75Ms = 23;
	CHECK_EQ(winU(&a, &w), 0); CHECK(gD.flags & KVM_AUTO_F_CAPTURE); CHECK_EQ((int)a.cpuStreak, 1);
	CHECK_EQ(winU(&a, &w), 1);
	CHECK_EQ((int)a.step, 1); CHECK_EQ(gD.reason, KVM_AUTO_R_CAPTURE);			// 23 ms / 0.75 = 31: 33 holds it
	// the closed lid: GDI takes 35..230 ms, the P75 about 180 ms: the last steps
	begin(&a, 0);
	w.capP75Ms = 180;
	(void)winU(&a, &w); CHECK_EQ(winU(&a, &w), 1);
	CHECK_EQ((int)a.step, 4);
	// 89 ms: 66 ms is 74 % of it: too short, the last step; 88 ms: 66 ms is 75 %
	begin(&a, 0);
	w.capP75Ms = 89;
	(void)winU(&a, &w); CHECK_EQ(winU(&a, &w), 1);
	CHECK_EQ((int)a.step, 4);
	begin(&a, 0);
	w.capP75Ms = 88;
	(void)winU(&a, &w); CHECK_EQ(winU(&a, &w), 1);
	CHECK_EQ((int)a.step, 3);
	begin(&a, 0);
	w.capP75Ms = 66;												// 50 ms is 75.8 % of it
	(void)winU(&a, &w); CHECK_EQ(winU(&a, &w), 1);
	CHECK_EQ((int)a.step, 2);
	// a capture that waits for a slow source (DXGI): dwm 1.0 per image says the machine takes every image
	begin(&a, 0);
	w = win_at(0, 26); w.capP75Ms = 66; w.gapMedMs = 66; w.dwmX100 = 100;
	CHECK_EQ(winU(&a, &w), 0); CHECK_EQ(winU(&a, &w), 0); CHECK_EQ(winU(&a, &w), 0);
	CHECK_EQ((int)a.step, 0);
	// the gap between two images: the loop does not keep its schedule
	begin(&a, 0);
	w = win_at(0, 26); w.gapMedMs = 22;
	CHECK_EQ(winU(&a, &w), 0); CHECK_EQ((int)a.cpuStreak, 0);
	w.gapMedMs = 23;
	CHECK_EQ(winU(&a, &w), 0); CHECK_EQ((int)a.cpuStreak, 1); CHECK(gD.flags & KVM_AUTO_F_CAPTURE);
	CHECK_EQ(winU(&a, &w), 1); CHECK_EQ((int)a.step, 1);
	// at 30 the same 22 ms is fine; 44 ms is exactly 33 / 0.75
	begin(&a, 1);
	w = win_at(1, 26); w.capP75Ms = 40; w.gapMedMs = 40;
	CHECK_EQ(winU(&a, &w), 0); CHECK_EQ((int)a.cpuStreak, 0);
	w.capP75Ms = 44; w.gapMedMs = 44;
	CHECK_EQ(winU(&a, &w), 0); CHECK_EQ((int)a.cpuStreak, 0); CHECK(!(gD.flags & KVM_AUTO_F_CAPTURE));
	w.capP75Ms = 45;												// 45 * 0.75 = 33.75 > 33
	CHECK_EQ(winU(&a, &w), 0); CHECK_EQ((int)a.cpuStreak, 1);
}

// the link: the QP of the regulator is above the ceiling of the step (the refusals count as the rise of the QP that they are about to cause)
static void test_link(void)
{
	kvm_auto a;
	kvm_auto_win w;
	kvm_auto_env e;
	// the table of the prediction: ln(1 / (1 - h)) / -ln(0.89) in tenths
	CHECK_EQ(kvm_auto_pred_qp10(300, 0), 300);
	CHECK_EQ(kvm_auto_pred_qp10(300, 4), 300);
	CHECK_EQ(kvm_auto_pred_qp10(300, 5), 309);
	CHECK_EQ(kvm_auto_pred_qp10(300, 10), 309);
	CHECK_EQ(kvm_auto_pred_qp10(300, 20), 319);
	CHECK_EQ(kvm_auto_pred_qp10(300, 54), 360);	// the deciles are rounded: 45..54 % is the rise of 50 %
	CHECK_EQ(kvm_auto_pred_qp10(300, 55), 379);
	CHECK_EQ(kvm_auto_pred_qp10(300, 60), 379);
	CHECK_EQ(kvm_auto_pred_qp10(300, 96), 600);
	CHECK_EQ(kvm_auto_pred_qp10(300, 100), 600);
	CHECK_EQ(kvm_auto_pred_qp10(300, 400), 600);
	// the QP above the ceiling without a single refusal (the regulator found a QP at which the frames fit: 3 Mb/s, QP 34 at 60): two windows, one step
	begin(&a, 0);
	w = win_at(0, 33);
	CHECK_EQ(winU(&a, &w), 0); CHECK(gD.flags & KVM_AUTO_F_QPBAD); CHECK_EQ((int)a.qpStreak, 1); CHECK_EQ(gD.heldPct, 0);
	CHECK_EQ(winU(&a, &w), 1);
	CHECK_EQ((int)a.step, 1); CHECK_EQ(gD.reason, KVM_AUTO_R_LINK); CHECK_EQ(gD.intervalMs, 33);
	// the ceiling itself is fine
	begin(&a, 0);
	w = win_at(0, 32);
	CHECK_EQ(winU(&a, &w), 0); CHECK_EQ(winU(&a, &w), 0); CHECK_EQ((int)a.qpStreak, 0);
	// the refusals are a rise of the QP to come: QP 28 with 46 % held (4.4) is 32.4: over; with 40 % (4.4) the same; with 30 % (3.1) 31.1: not
	begin(&a, 0);
	w = win_at(0, 28); w.made = 54; w.pendingIters = 54; w.qpN = 54; w.qpSum = 54u * 28u; w.heldWin = 46;						// 46 of 100
	CHECK_EQ(winU(&a, &w), 0); CHECK(gD.flags & KVM_AUTO_F_QPBAD); CHECK_EQ(gD.heldPct, 46);
	CHECK_EQ(winU(&a, &w), 1); CHECK_EQ(gD.reason, KVM_AUTO_R_LINK);
	begin(&a, 0);
	w.made = 70; w.pendingIters = 70; w.qpN = 70; w.qpSum = 70u * 28u; w.heldWin = 15; w.heldRate = 15;						// 30 of 100: 28 + 3.1
	CHECK_EQ(winU(&a, &w), 0); CHECK(!(gD.flags & KVM_AUTO_F_QPBAD)); CHECK_EQ(gD.heldPct, 30);
	w.qpSum = 70u * 29u; w.qpEnd = 29;																							// 29 + 3.1 = 32.1
	CHECK_EQ(winU(&a, &w), 0); CHECK(gD.flags & KVM_AUTO_F_QPBAD);
	// the window of the viewer counts as much as the bucket
	begin(&a, 0);
	w = win_at(0, 26); w.made = 40; w.pendingIters = 40; w.qpN = 40; w.qpSum = 40u * 26u; w.heldWin = 30; w.heldRate = 30;		// 60 of 100: 26 + 7.9 = 33.9
	CHECK_EQ(winU(&a, &w), 0); CHECK(gD.flags & KVM_AUTO_F_QPBAD);
	CHECK_EQ(winU(&a, &w), 1);
	// one step at a time (the link)
	CHECK_EQ((int)a.step, 1);
	w = win_at(1, 35);
	(void)winU(&a, &w); CHECK_EQ(winU(&a, &w), 1);
	CHECK_EQ((int)a.step, 2);										// the ceiling of 30 is 32, of 20 is 34
	// the ceiling is per step: QP 34 is fine at 20 (34), over it at 30
	begin(&a, 2);
	w = win_at(2, 34);
	(void)winU(&a, &w); (void)winU(&a, &w); CHECK_EQ((int)a.step, 2);
	w = win_at(2, 35);
	(void)winU(&a, &w); CHECK_EQ(winU(&a, &w), 1); CHECK_EQ((int)a.step, 3);
	// the class: video has 34 at 60 and 30
	begin(&a, 0);
	w = win_at(0, 34);
	e = env_at(0); e.cls = KVM_AUTO_CLASS_VIDEO;
	(void)win(&a, &w, e); CHECK_EQ(win(&a, &w, e), 0); CHECK_EQ((int)a.step, 0);
	e.cls = KVM_AUTO_CLASS_UI;
	(void)win(&a, &w, e); CHECK_EQ(win(&a, &w, e), 1);
	// the QP of the regulator is the one that counts when it is known: the frames of the window were made at a low QP, the regulator is at 36 (a link that fell)
	begin(&a, 0);
	w = win_at(0, 26);
	e = env_at(0); e.qpNow = 36;
	(void)win(&a, &w, e); CHECK(gD.flags & KVM_AUTO_F_QPBAD); CHECK_EQ(win(&a, &w, e), 1);
	// ... and the other way: the frames at 40, the regulator already at 26
	begin(&a, 0);
	w = win_at(0, 40);
	e = env_at(0); e.qpNow = 26;
	(void)win(&a, &w, e); (void)win(&a, &w, e); CHECK_EQ((int)a.step, 0); CHECK_EQ((int)a.qpStreak, 0);
	// no frame in the window and no QP of the regulator: nothing to judge; with the QP of the regulator a starved link (nothing made, everything refused) is judged
	begin(&a, 0);
	w = win_at(0, 40); w.qpN = 0; w.qpSum = 0; w.qpEnd = 0; w.heldRate = 50; w.made = 0; w.pendingIters = 5;
	(void)winU(&a, &w); CHECK_EQ(winU(&a, &w), 0); CHECK_EQ((int)a.qpStreak, 0);
	e = env_at(0); e.qpNow = 38;
	(void)win(&a, &w, e); CHECK_EQ(win(&a, &w, e), 1); CHECK_EQ(gD.reason, KVM_AUTO_R_LINK);
	// a window that is not bad in between breaks the streak
	begin(&a, 0);
	w = win_at(0, 35);
	(void)winU(&a, &w); CHECK_EQ((int)a.qpStreak, 1);
	w = win_at(0, 26); (void)winU(&a, &w); CHECK_EQ((int)a.qpStreak, 0);
	// the last step stays
	begin(&a, 4);
	w = win_at(4, 40);
	(void)winU(&a, &w); CHECK_EQ(winU(&a, &w), 0); CHECK_EQ((int)a.step, 4);
	// a window of the other kind breaks the streak: the link, then the machine, then the link again is no two in a row
	begin(&a, 0);
	w = win_at(0, 35);
	CHECK_EQ(winU(&a, &w), 0); CHECK_EQ((int)a.qpStreak, 1);
	{ kvm_auto_win m = win_at(0, 26); m.made = 50; m.pendingIters = 50; m.qpN = 50; m.qpSum = 50u * 26u; m.capP75Ms = 30; CHECK_EQ(winU(&a, &m), 0); CHECK_EQ((int)a.qpStreak, 0); CHECK_EQ((int)a.cpuStreak, 1);
	  CHECK_EQ(winU(&a, &w), 0); CHECK_EQ((int)a.qpStreak, 1); CHECK_EQ((int)a.cpuStreak, 0); }
	// ... and the machine, a good window, the machine again
	begin(&a, 0);
	{ kvm_auto_win m = win_at(0, 26); m.made = 50; m.pendingIters = 50; m.qpN = 50; m.qpSum = 50u * 26u; m.capP75Ms = 30; CHECK_EQ(winU(&a, &m), 0); CHECK_EQ((int)a.cpuStreak, 1);
	  { kvm_auto_win g = win_at(0, 26); CHECK_EQ(winU(&a, &g), 0); CHECK_EQ((int)a.cpuStreak, 0); }
	  CHECK_EQ(winU(&a, &m), 0); CHECK_EQ((int)a.cpuStreak, 1); CHECK_EQ((int)a.step, 0); }
	// a machine problem and a link problem at once: the machine one is the one counted (the step that holds the cost, not just one)
	begin(&a, 0);
	w = win_at(0, 35); w.capP75Ms = 60;
	(void)winU(&a, &w); CHECK_EQ(winU(&a, &w), 1); CHECK_EQ(gD.reason, KVM_AUTO_R_CAPTURE); CHECK_EQ((int)a.step, 2);
}

// a key frame: the windows around it are not evaluated
static void test_key(void)
{
	kvm_auto a;
	kvm_auto_dec d;
	kvm_auto_env e;
	kvm_auto_win w;
	uint32_t t = 500000;
	int i;
	kvm_auto_init(&a);
	kvm_auto_set(&a, 1, t);
	e = env_at(t); e.minRttMs = 5;
	(void)kvm_auto_tick(&a, &e, &d, NULL);						// the start: 60
	// a window with a key frame in it is frozen, whatever it says; the one that follows within the time the key frame needs too
	for (i = 0; i < 60; i++) { kvm_auto_note_image(&a, t + 16u * (uint32_t)i, 100); kvm_auto_note_frame(&a, t + 16u * (uint32_t)i, 36, 1000); }
	kvm_auto_note_key(&a, t + 500, 2000);						// frozen until t + 500 + 1000 + 2000
	e = env_at(t + 1000); e.qpNow = 36;
	(void)kvm_auto_tick(&a, &e, &d, &w);
	CHECK_EQ(w.key, 1); CHECK_EQ(d.evaluated, 0); CHECK_EQ((int)a.qpStreak, 0);
	t += 1000;
	for (i = 0; i < 60; i++) { kvm_auto_note_image(&a, t + 16u * (uint32_t)i, 100); kvm_auto_note_frame(&a, t + 16u * (uint32_t)i, 36, 1000); }
	e = env_at(t + 1000); e.qpNow = 36;							// the window ends at t0 + 2000 < 3500: still frozen
	(void)kvm_auto_tick(&a, &e, &d, &w);
	CHECK_EQ(w.key, 1); CHECK_EQ(d.evaluated, 0);
	t += 1000;
	for (i = 0; i < 60; i++) { kvm_auto_note_image(&a, t + 16u * (uint32_t)i, 100); kvm_auto_note_frame(&a, t + 16u * (uint32_t)i, 36, 1000); }
	e = env_at(t + 1000); e.qpNow = 36;							// ends at t0 + 3000 < 3500
	(void)kvm_auto_tick(&a, &e, &d, &w);
	CHECK_EQ(w.key, 1); CHECK_EQ(d.evaluated, 0);
	t += 1000;
	for (i = 0; i < 60; i++) { kvm_auto_note_image(&a, t + 16u * (uint32_t)i, 100); kvm_auto_note_frame(&a, t + 16u * (uint32_t)i, 36, 1000); }
	e = env_at(t + 1000); e.qpNow = 36;							// ends at t0 + 4000 > 3500: evaluated
	(void)kvm_auto_tick(&a, &e, &d, &w);
	CHECK_EQ(w.key, 0); CHECK_EQ(d.evaluated, 1); CHECK_EQ((int)a.qpStreak, 1);
	// a key frame at the start of a window freezes that window even when it needs nothing on the link (its freeze ends with the window)
	kvm_auto_set(&a, 0, 0); kvm_auto_set(&a, 1, t);
	e = env_at(t); e.minRttMs = 5; (void)kvm_auto_tick(&a, &e, &d, NULL);
	e = env_at(t + 10); (void)kvm_auto_tick(&a, &e, &d, NULL);
	kvm_auto_note_image(&a, t + 10, 5);
	kvm_auto_note_key(&a, t + 10, 0);
	e = env_at(t + 1010);
	(void)kvm_auto_tick(&a, &e, &d, &w);
	CHECK_EQ(w.key, 1);
	// ... and the next window, which starts after it, is free
	kvm_auto_note_image(&a, t + 1020, 5);
	e = env_at(t + 2010);
	(void)kvm_auto_tick(&a, &e, &d, &w);
	CHECK_EQ(w.key, 0);
	// a later key frame does not shorten the freeze of an earlier one, a shorter one does not replace it
	kvm_auto_set(&a, 0, 0); kvm_auto_set(&a, 1, t);
	kvm_auto_note_key(&a, t, 5000);
	kvm_auto_note_key(&a, t + 100, 0);
	CHECK_EQ((int)a.keyUntil, (int)(t + 1000 + 5000));
	// the freeze is bounded (a rate of nothing must not freeze the mode for good)
	kvm_auto_set(&a, 0, 0); kvm_auto_set(&a, 1, t);
	kvm_auto_note_key(&a, t, 0xFFFFFFFFu);
	CHECK_EQ((int)a.keyUntil, (int)(t + 1000 + 60000));
	// with the bit down nothing is noted
	kvm_auto_set(&a, 0, 0);
	kvm_auto_note_key(&a, t, 5000);
	CHECK_EQ((int)a.haveKey, 0);
	// a key frame does not break the streaks: a frozen window is neither
	begin(&a, 0);
	w = win_at(0, 36);
	(void)winU(&a, &w); CHECK_EQ((int)a.qpStreak, 1);
	{ kvm_auto_win k = w; k.key = 1; CHECK_EQ(winU(&a, &k), 0); CHECK_EQ(gD.evaluated, 0); CHECK_EQ((int)a.qpStreak, 1); }
	CHECK_EQ(winU(&a, &w), 1);
}

// no step up into a link with a queue
static void test_queue(void)
{
	kvm_auto a;
	kvm_auto_win w;
	kvm_auto_env e = env_at(0);
	int i;
	begin(&a, 1);
	w = win_at(1, 24);
	e.queueMs = KVM_AUTO_UP_QUEUE_MS - 1;
	for (i = 0; i < 3; i++) { CHECK_EQ(win(&a, &w, e), 0); }
	CHECK_EQ(win(&a, &w, e), 1);
	begin(&a, 1);
	e.queueMs = KVM_AUTO_UP_QUEUE_MS;
	for (i = 0; i < 8; i++) { CHECK_EQ(win(&a, &w, e), 0); CHECK(!(gD.flags & KVM_AUTO_F_GOOD)); }
	// a queue never takes a step down by itself (the link has its own rule)
	begin(&a, 1);
	e.queueMs = 5000;
	for (i = 0; i < 8; i++) { CHECK_EQ(win(&a, &w, e), 0); }
	CHECK_EQ((int)a.step, 1);
}

// the way up
static void test_up(void)
{
	kvm_auto a;
	kvm_auto_win w;
	int i;
	begin(&a, 1);
	w = win_at(1, 24);												// 30 frames a second at QP 24 (a faster step needs at most 32 - 6 = 26)
	for (i = 0; i < KVM_AUTO_UP_WINDOWS - 1; i++) { CHECK_EQ(winU(&a, &w), 0); CHECK(gD.flags & KVM_AUTO_F_GOOD); }
	CHECK_EQ((int)a.goodStreak, KVM_AUTO_UP_WINDOWS - 1);
	CHECK_EQ(winU(&a, &w), 1);
	CHECK_EQ((int)a.step, 0); CHECK_EQ(gD.reason, KVM_AUTO_R_BACK); CHECK_EQ(gD.intervalMs, 16);
	CHECK_EQ((int)a.goodStreak, 0); CHECK_EQ((int)a.ups, 1);
	// not above the first step
	w = win_at(0, 26);
	for (i = 0; i < 10; i++) { CHECK_EQ(winU(&a, &w), 0); }
	CHECK_EQ((int)a.step, 0);
	// the QP of the faster step: with the stream taken to fill R (utilization not known) the faster rate costs 6 QP: 25 + 6 = 31 is the most for the 60 step (32 - 1); 26 is not good
	begin(&a, 1);
	w = win_at(1, 25);
	for (i = 0; i < 3; i++) { CHECK_EQ(winU(&a, &w), 0); }
	CHECK_EQ(winU(&a, &w), 1);
	begin(&a, 1);
	w = win_at(1, 26);
	for (i = 0; i < 8; i++) { CHECK_EQ(winU(&a, &w), 0); CHECK(!(gD.flags & KVM_AUTO_F_GOOD)); }
	// the QP that the regulator asks for now is the one that counts when it is known: the frames of the window at 24, the regulator at 26 (it just went up)
	{
		kvm_auto_env e = env_at(0);
		begin(&a, 1);
		w = win_at(1, 24); e.qpNow = 26;
		for (i = 0; i < 8; i++) { CHECK_EQ(win(&a, &w, e), 0); }
		begin(&a, 1);
		w = win_at(1, 28); e.qpNow = 24;										// ... and the other way round
		for (i = 0; i < 3; i++) { CHECK_EQ(win(&a, &w, e), 0); }
		CHECK_EQ(win(&a, &w, e), 1);
	}
	// the utilization of R: the stream that is a fifth of R (the faster rate is two fifths) costs no QP, the regulator is at its floor: a step up at the floor of the hardware encoder (30) too
	{
		kvm_auto_env e = env_at(0);
		begin(&a, 1);
		w = win_at(1, 30); w.bytes = 100000u;
		e.qpNow = 30; e.qpFloor = 30; e.rateBps = 1000000u;						// 100 KB/s of the stream, 2 x that is 20 % of R
		for (i = 0; i < 3; i++) { CHECK_EQ(win(&a, &w, e), 0); }
		CHECK_EQ(win(&a, &w, e), 1);
		CHECK_EQ(kvm_auto_up_qp10(1, 300, 100000u, 1000u, 1000000u, 30), 300);
		// the same QP with the floor of the software encoder (26): the regulator is above its floor, 30 is a QP that the load asked for... and 20 % of R takes it down to the floor
		CHECK_EQ(kvm_auto_up_qp10(1, 300, 100000u, 1000u, 1000000u, 26), 260);
		// the same at the software floor with the stream unknown: 6 QP up
		CHECK_EQ(kvm_auto_up_qp10(1, 300, 0, 1000u, 1000000u, 26), 360);
		CHECK_EQ(kvm_auto_up_qp10(1, 300, 100000u, 1000u, 0, 26), 360);
		CHECK_EQ(kvm_auto_up_qp10(1, 300, 100000u, 0, 1000000u, 26), 360);
		// a stream that is 80 % of R at 30 is 160 % at 60 (the bucket of 175 %: +4.8 QP); one that is 40 % of R is 80 % at 60: the regulator goes down by 1.9
		CHECK_EQ(kvm_auto_up_qp10(1, 300, 800000u, 1000u, 1000000u, 26), 348);
		CHECK_EQ(kvm_auto_up_qp10(1, 300, 400000u, 1000u, 1000000u, 26), 281);
		// no QP, no step: the stream is too big for the faster step
		begin(&a, 1);
		w = win_at(1, 30); w.bytes = 800000u;
		e.qpNow = 31;
		for (i = 0; i < 8; i++) { CHECK_EQ(win(&a, &w, e), 0); }
		// the software encoder at its floor in a LAN (R large): back to 60 from the start of 30
		begin(&a, 1);
		w = win_at(1, 26); w.bytes = 60000u;
		e.qpNow = 26; e.qpFloor = 26; e.rateBps = 500000u;
		for (i = 0; i < 3; i++) { CHECK_EQ(win(&a, &w, e), 0); }
		CHECK_EQ(win(&a, &w, e), 1);
		// the steps: 30 -> 20 is 150 %, 20 -> 15 is 133 %, 15 -> 10 is 150 % ... and 60 -> 30 does not exist as a step up
		CHECK_EQ(kvm_auto_up_qp10(2, 300, 0, 1000u, 0, 26), 335);
		CHECK_EQ(kvm_auto_up_qp10(3, 300, 0, 1000u, 0, 26), 335);
		CHECK_EQ(kvm_auto_up_qp10(4, 300, 0, 1000u, 0, 26), 335);
		CHECK_EQ(kvm_auto_up_qp10(0, 300, 0, 1000u, 0, 26), 300);			// (the fastest step: nothing to go to; defined, not used)
		CHECK_EQ(kvm_auto_up_qp10(1, 100, 0, 1000u, 0, 0), 260);			// a floor that is not given: 26
		CHECK_EQ(kvm_auto_up_qp10(1, 300, 525000u, 1000u, 1000000u, 26), 308);	// 105 % of R: the bucket of 110 %, +0.8
		// the floor of the regulator holds the prediction up: a regulator that cannot go below 32 cannot be promised 31
		begin(&a, 1);
		w = win_at(1, 30);
		e.qpNow = 30; e.qpFloor = 32; e.rateBps = 1000000u; w.bytes = 10000u;
		for (i = 0; i < 8; i++) { CHECK_EQ(win(&a, &w, e), 0); CHECK(!(gD.flags & KVM_AUTO_F_GOOD)); }
		e.qpFloor = 30;
		for (i = 0; i < 3; i++) { CHECK_EQ(win(&a, &w, e), 0); }
		CHECK_EQ(win(&a, &w, e), 1);
	}
	// no QP in the window
	begin(&a, 1);
	w = win_at(1, 24); w.qpN = 0; w.qpSum = 0; w.qpEnd = 0;
	for (i = 0; i < 8; i++) { CHECK_EQ(winU(&a, &w), 0); }
	// 5 % held: no (4 % is fine)
	begin(&a, 1);
	w = win_at(1, 24); w.made = 30; w.pendingIters = 30; w.qpN = 30; w.qpSum = 30u * 24u; w.heldRate = 2;			// 2 / 32 = 6 %
	for (i = 0; i < 8; i++) { CHECK_EQ(winU(&a, &w), 0); }
	begin(&a, 1);
	w.heldRate = 1; w.heldWin = 0;															// 1 / 31 = 3 %
	for (i = 0; i < 3; i++) { CHECK_EQ(winU(&a, &w), 0); }
	CHECK_EQ(winU(&a, &w), 1);
	// exactly 5 % held: no
	begin(&a, 3);
	w = win_at(3, 24); w.made = 19; w.pendingIters = 19; w.qpN = 19; w.qpSum = 19u * 24u; w.heldRate = 1;		// 1 / 20
	for (i = 0; i < 8; i++) { CHECK_EQ(winU(&a, &w), 0); }
	CHECK_EQ(gD.heldPct, 5);
	// the frames of the current step are not made (a source that does not make more): no use in a faster step
	begin(&a, 1);
	w = win_at(1, 24); w.made = 25; w.pendingIters = 25; w.qpN = 25; w.qpSum = 25u * 24u;	// 25 of 30 = 83 % < 85 %
	for (i = 0; i < 8; i++) { CHECK_EQ(winU(&a, &w), 0); }
	begin(&a, 1);
	w.made = 26; w.pendingIters = 26; w.qpN = 26; w.qpSum = 26u * 24u;						// 26 of 30 = 86 %
	for (i = 0; i < 3; i++) { CHECK_EQ(winU(&a, &w), 0); }
	CHECK_EQ(winU(&a, &w), 1);
	// the capture: the step up has the threshold of staying (4/3 of 16.67 ms = 22.2): 22 ms goes, 23 does not
	begin(&a, 1);
	w = win_at(1, 24); w.capP75Ms = 22;
	for (i = 0; i < 3; i++) { CHECK_EQ(winU(&a, &w), 0); }
	CHECK_EQ(winU(&a, &w), 1);
	begin(&a, 1);
	w.capP75Ms = 23;
	for (i = 0; i < 8; i++) { CHECK_EQ(winU(&a, &w), 0); CHECK(!(gD.flags & KVM_AUTO_F_GOOD)); }
	// the encoder: the wall time of a frame has to fit the faster interval as staying does (22 ms: yes, 23: no)
	{
		kvm_auto_env e = env_at(0);
		begin(&a, 1);
		w = win_at(1, 24);
		e.encAvgUs = 22000; e.budgetMilli = 100000;
		for (i = 0; i < 3; i++) { CHECK_EQ(win(&a, &w, e), 0); }
		CHECK_EQ(win(&a, &w, e), 1);
		begin(&a, 1);
		e.encAvgUs = 23000;
		for (i = 0; i < 8; i++) { CHECK_EQ(win(&a, &w, e), 0); }
		// the CPU budget: 8 ms x 1.5 cores x 60 fps = 720 milli-cores
		begin(&a, 1);
		e.encAvgUs = 8000; e.parX10 = 15; e.budgetMilli = 720;
		for (i = 0; i < 3; i++) { CHECK_EQ(win(&a, &w, e), 0); }
		CHECK_EQ(win(&a, &w, e), 1);
		begin(&a, 1);
		e.budgetMilli = 719;
		for (i = 0; i < 8; i++) { CHECK_EQ(win(&a, &w, e), 0); }
		begin(&a, 1);
		e.budgetMilli = 0;											// unknown budget: no rule
		for (i = 0; i < 3; i++) { CHECK_EQ(win(&a, &w, e), 0); }
		CHECK_EQ(win(&a, &w, e), 1);
		// the floor of the budget above the faster interval
		begin(&a, 1);
		e = env_at(0); e.floorMs = 17;
		for (i = 0; i < 8; i++) { CHECK_EQ(win(&a, &w, e), 0); }
		begin(&a, 1);
		e.floorMs = 16;
		for (i = 0; i < 3; i++) { CHECK_EQ(win(&a, &w, e), 0); }
		CHECK_EQ(win(&a, &w, e), 1);
		// a ceiling from outside: 30 is the most
		begin(&a, 1);
		e = env_at(0); e.maxFps = 30;
		for (i = 0; i < 8; i++) { CHECK_EQ(win(&a, &w, e), 0); }
		CHECK_EQ((int)a.step, 1);
		begin(&a, 2);
		w = win_at(2, 24);
		e.maxFps = 20;
		for (i = 0; i < 8; i++) { CHECK_EQ(win(&a, &w, e), 0); }
		CHECK_EQ((int)a.step, 2);
		e.maxFps = 30;
		for (i = 0; i < 3; i++) { CHECK_EQ(win(&a, &w, e), 0); }
		CHECK_EQ(win(&a, &w, e), 1); CHECK_EQ((int)a.step, 1);
	}
	// the faster step is judged by ITS ceiling: 15 -> 20 costs 3.5 QP (133 % of R, the bucket of 150 %): 29 + 3.5 = 32.5 is under 33 (34 - 1), 30 is not
	begin(&a, 3);
	w = win_at(3, 29);
	for (i = 0; i < 3; i++) { CHECK_EQ(winU(&a, &w), 0); }
	CHECK_EQ(winU(&a, &w), 1); CHECK_EQ((int)a.step, 2);
	begin(&a, 3);
	w = win_at(3, 30);
	for (i = 0; i < 8; i++) { CHECK_EQ(winU(&a, &w), 0); }
	// ... of the class: video 34 at 30: 27 + 6 = 33 is the most
	{
		kvm_auto_env e = env_at(0);
		e.cls = KVM_AUTO_CLASS_VIDEO;
		begin(&a, 1);
		w = win_at(1, 27);
		for (i = 0; i < 3; i++) { CHECK_EQ(win(&a, &w, e), 0); }
		CHECK_EQ(win(&a, &w, e), 1);
		begin(&a, 1);
		w = win_at(1, 28);
		for (i = 0; i < 8; i++) { CHECK_EQ(win(&a, &w, e), 0); }
	}
	// a window that is not good breaks the streak
	begin(&a, 1);
	w = win_at(1, 24);
	for (i = 0; i < 3; i++) { CHECK_EQ(winU(&a, &w), 0); }
	{ kvm_auto_win bad = win_at(1, 29); CHECK_EQ(winU(&a, &bad), 0); CHECK_EQ((int)a.goodStreak, 0); }
	for (i = 0; i < 3; i++) { CHECK_EQ(winU(&a, &w), 0); }
	CHECK_EQ(winU(&a, &w), 1);
}

// the clock of a step: 2 s between steps, the first improvement 4 s after the previous step
static void test_timing(void)
{
	kvm_auto a;
	kvm_auto_win w, bad;
	kvm_auto_env e;
	int i;
	// the two bad windows 1 s apart come right after a step: down only 2 s after it
	begin(&a, 1);
	bad = win_at(1, 36); bad.heldRate = 20;
	a.lastChangeAt = gT + 1000;											// the last step was at the first window below
	a.haveChange = 1;
	CHECK_EQ(winU(&a, &bad), 0);										// 0 s after
	CHECK_EQ(winU(&a, &bad), 0);										// 1 s after: the streak is 2, but the step is too young
	CHECK_EQ((int)a.qpStreak, 2);
	CHECK_EQ(winU(&a, &bad), 1);										// 2 s after
	CHECK_EQ((int)a.step, 2);
	// an improvement 4 s after the step, not before
	begin(&a, 1);
	w = win_at(1, 24);
	a.upNeed = 1;
	a.lastChangeAt = gT + 1000; a.haveChange = 1;
	for (i = 0; i < 4; i++) { CHECK_EQ(winU(&a, &w), 0); }				// 0, 1, 2, 3 s after the step
	CHECK_EQ(winU(&a, &w), 1);											// 4 s
	CHECK_EQ((int)a.step, 0);
	// the same with the step 3 s ago
	begin(&a, 1);
	a.upNeed = 1;
	a.lastChangeAt = gT - 2000; a.haveChange = 1;						// the next window is 3 s after the step
	CHECK_EQ(winU(&a, &w), 0);
	CHECK_EQ(winU(&a, &w), 1);											// 4 s
	// a wrap of the 32 bit clock
	begin(&a, 1);
	gT = 0xFFFFFFFFu - 2500u;
	a.lastChangeAt = gT - 20000u;
	a.upNeed = 1;
	CHECK_EQ(winU(&a, &w), 1);
	begin(&a, 0);
	gT = 0xFFFFFFFFu - 1500u;
	a.lastChangeAt = gT - 20000u;
	w = win_at(0, 26); w.made = 40; w.pendingIters = 40; w.qpN = 40; w.qpSum = 1040;
	CHECK_EQ(winU(&a, &w), 0);
	CHECK_EQ(winU(&a, &w), 1);											// now = 500 after the wrap
	CHECK_EQ((int)a.step, 1);
	// the clock of the start survives the wrap too
	kvm_auto_init(&a); kvm_auto_set(&a, 1, 0xFFFFFFFFu - 1000u);
	e = env_at(0xFFFFFFFFu - 1000u + KVM_AUTO_START_WAIT_MS); e.minRttMs = 0;
	{ kvm_auto_dec d; (void)kvm_auto_decide(&a, NULL, &e, &d); }
	CHECK(a.started);
}

// the patience
static void test_patience(void)
{
	kvm_auto a;
	kvm_auto_win good, bad;
	int i, k;
	begin(&a, 1);
	good = win_at(1, 24);
	bad = win_at(0, 26); bad.made = 40; bad.pendingIters = 40; bad.qpN = 40; bad.qpSum = 1040;			// 40 fps at the step 60: down one step
	CHECK_EQ((int)a.upNeed, 4);
	// a failed attempt: up, then down within 10 s: 4 -> 8 -> 16 -> 32 -> 64 -> 120, and 120 stays (nothing gives the first patience back but a step up that held)
	{
		static const int need[6] = { 8, 16, 32, 64, 120, 120 };
		for (k = 0; k < 6; k++)
		{
			a.step = 1;
			a.lastChangeAt = gT; a.haveChange = 1; a.goodStreak = 0;
			for (i = 0; i < (int)a.upNeed - 1; i++) { CHECK_EQ(winU(&a, &good), 0); }
			CHECK_EQ(winU(&a, &good), 1);								// up
			CHECK_EQ((int)a.step, 0);
			CHECK_EQ(winU(&a, &bad), 0);
			CHECK_EQ(winU(&a, &bad), 1);									// the second bad window is 2 s after the step: down
			CHECK_EQ((int)a.step, 1);
			CHECK_EQ((int)a.upNeed, need[k]);
		}
	}
	// a down later than 10 s after the up is no failed attempt
	begin(&a, 1);
	a.upNeed = 4;
	for (i = 0; i < 3; i++) { CHECK_EQ(winU(&a, &good), 0); }
	CHECK_EQ(winU(&a, &good), 1);
	gT += KVM_AUTO_FAIL_MS;												// the next window is 11 s after the step up
	CHECK_EQ(winU(&a, &bad), 0);
	CHECK_EQ(winU(&a, &bad), 1);										// 12 s
	CHECK_EQ((int)a.upNeed, 4);
	// exactly 10 s: a failed attempt
	begin(&a, 1);
	for (i = 0; i < 3; i++) { CHECK_EQ(winU(&a, &good), 0); }
	CHECK_EQ(winU(&a, &good), 1);
	gT += KVM_AUTO_FAIL_MS - 2000;										// the first bad window is 9 s after the up, the second 10 s
	CHECK_EQ(winU(&a, &bad), 0);
	CHECK_EQ(winU(&a, &bad), 1);
	CHECK_EQ((int)a.upNeed, 8);
	// 10 s and 1 ms: no failure
	begin(&a, 1);
	for (i = 0; i < 3; i++) { CHECK_EQ(winU(&a, &good), 0); }
	CHECK_EQ(winU(&a, &good), 1);
	gT += KVM_AUTO_FAIL_MS - 1999;
	CHECK_EQ(winU(&a, &bad), 0);
	CHECK_EQ(winU(&a, &bad), 1);
	CHECK_EQ((int)a.upNeed, 4);
	// a second down does not double again
	a.step = 0; a.haveUp = 0;
	a.lastChangeAt = gT - 5000;
	CHECK_EQ(winU(&a, &bad), 0); CHECK_EQ(winU(&a, &bad), 1);
	CHECK_EQ((int)a.upNeed, 4);
	// two steps down within 10 s of one step up: the attempt failed once
	begin(&a, 1);
	for (i = 0; i < 3; i++) { CHECK_EQ(winU(&a, &good), 0); }
	CHECK_EQ(winU(&a, &good), 1);										// up, 4 -> upNeed 4
	CHECK_EQ(winU(&a, &bad), 0); CHECK_EQ(winU(&a, &bad), 1);			// down at 2 s: 8
	CHECK_EQ((int)a.upNeed, 8); CHECK_EQ((int)a.step, 1);
	{ kvm_auto_win slow = win_at(1, 26); slow.made = 15; slow.pendingIters = 15; slow.qpN = 15; slow.qpSum = 15u * 26u;
	  CHECK_EQ(winU(&a, &slow), 0); CHECK_EQ(winU(&a, &slow), 1); CHECK_EQ((int)a.step, 2); CHECK_EQ((int)a.upNeed, 8); }
	// waiting is no success: 70 s of windows that are not good leave the patience where it was (the old rule gave the first one back after 60 s without a change)
	begin(&a, 1);
	a.upNeed = 32;
	{ kvm_auto_win w = win_at(1, 29); for (i = 0; i < 70; i++) { (void)winU(&a, &w); } CHECK_EQ((int)a.upNeed, 32); CHECK_EQ((int)a.step, 1); }
	// a step up that held for 60 s on the faster step is the success that gives the first patience back: not before the 60th second
	begin(&a, 1);
	a.upNeed = 32; a.step = 0; a.haveUp = 1; a.lastUpAt = gT; a.upStep = 0; a.lastChangeAt = gT; a.haveChange = 1;
	{ kvm_auto_win w = win_at(0, 26); for (i = 0; i < 59; i++) { CHECK_EQ(winU(&a, &w), 0); } CHECK_EQ((int)a.upNeed, 32); CHECK_EQ(winU(&a, &w), 0); CHECK_EQ((int)a.upNeed, 4); CHECK_EQ((int)a.haveUp, 0); }
	// ... on the faster step: a step up that was followed by another change of the step before the 60 s is over does not count for the old one
	begin(&a, 2);
	a.upNeed = 32; a.step = 1; a.haveUp = 1; a.lastUpAt = gT; a.upStep = 0; a.lastChangeAt = gT; a.haveChange = 1;
	{ kvm_auto_win w = win_at(1, 29); for (i = 0; i < 61; i++) { (void)winU(&a, &w); } CHECK_EQ((int)a.upNeed, 32); }
	// a step up is a step like the others: the patience that was won is gone with the next failure only
	begin(&a, 1);
	a.upNeed = 8;
	a.lastChangeAt = gT - 20000;
	for (i = 0; i < 7; i++) { CHECK_EQ(winU(&a, &good), 0); }
	CHECK_EQ(winU(&a, &good), 1);
}

// a step down because of the link is not tried again sooner than KVM_AUTO_LINK_WAIT windows; a failed attempt doubles from there to the ceiling; the CPU, the capture and a ceiling from outside keep the patience they had
static void test_link_wait(void)
{
	kvm_auto a;
	kvm_auto_win good, linkBad, slow, w;
	kvm_auto_env e;
	int i;
	CHECK_EQ(KVM_AUTO_LINK_WAIT, 64);
	good = win_at(1, 24);
	linkBad = win_at(1, 35);															// the QP over the ceiling of 30 frames a second
	slow = win_at(0, 26); slow.made = 40; slow.pendingIters = 40; slow.qpN = 40; slow.qpSum = 40u * 26u;	// 40 frames a second at 60: the machine
	// the first step down is the link (the patience was 4): the next attempt is the 64th good window, not the 4th
	begin(&a, 1);
	CHECK_EQ((int)a.upNeed, 4);
	CHECK_EQ(winU(&a, &linkBad), 0); CHECK_EQ(winU(&a, &linkBad), 1);
	CHECK_EQ((int)a.step, 2); CHECK_EQ(gD.reason, KVM_AUTO_R_LINK); CHECK_EQ((int)a.upNeed, KVM_AUTO_LINK_WAIT);
	good = win_at(2, 26);
	for (i = 0; i < KVM_AUTO_LINK_WAIT - 1; i++) { CHECK_EQ(winU(&a, &good), 0); }
	CHECK_EQ((int)a.goodStreak, KVM_AUTO_LINK_WAIT - 1); CHECK_EQ((int)a.step, 2);
	CHECK_EQ(winU(&a, &good), 1); CHECK_EQ((int)a.step, 1); CHECK_EQ(gD.reason, KVM_AUTO_R_BACK);
	// that attempt fails (the link queues again): down within 10 s, the wait is 64 * 2 capped at 120, and it is 120 from then on
	linkBad = win_at(1, 35);
	CHECK_EQ(winU(&a, &linkBad), 0); CHECK_EQ(winU(&a, &linkBad), 1);
	CHECK_EQ((int)a.step, 2); CHECK_EQ(gD.reason, KVM_AUTO_R_LINK); CHECK_EQ((int)a.upNeed, KVM_AUTO_UP_MAX);
	for (i = 0; i < KVM_AUTO_UP_MAX - 1; i++) { CHECK_EQ(winU(&a, &good), 0); }
	CHECK_EQ(winU(&a, &good), 1); CHECK_EQ((int)a.step, 1);
	CHECK_EQ(winU(&a, &linkBad), 0); CHECK_EQ(winU(&a, &linkBad), 1);
	CHECK_EQ((int)a.upNeed, KVM_AUTO_UP_MAX);
	// the wait is a floor, not a reset: a patience that is above it stays (nothing here shortens it)
	begin(&a, 1);
	a.upNeed = 100;
	CHECK_EQ(winU(&a, &linkBad), 0); CHECK_EQ(winU(&a, &linkBad), 1); CHECK_EQ((int)a.upNeed, 100);
	// a step up that held for 60 s gives the first patience back, also after a wait for the link
	begin(&a, 1);
	CHECK_EQ(winU(&a, &linkBad), 0); CHECK_EQ(winU(&a, &linkBad), 1); CHECK_EQ((int)a.upNeed, KVM_AUTO_LINK_WAIT);
	for (i = 0; i < KVM_AUTO_LINK_WAIT - 1; i++) { CHECK_EQ(winU(&a, &good), 0); }
	CHECK_EQ(winU(&a, &good), 1); CHECK_EQ((int)a.step, 1);
	{ kvm_auto_win held = win_at(1, 29); for (i = 0; i < 59; i++) { (void)winU(&a, &held); } CHECK_EQ((int)a.upNeed, KVM_AUTO_LINK_WAIT); (void)winU(&a, &held); CHECK_EQ((int)a.upNeed, KVM_AUTO_UP_WINDOWS); }
	// the machine: a step down for the CPU or the capture keeps the first patience (4 windows)
	begin(&a, 0);
	CHECK_EQ(winU(&a, &slow), 0); CHECK_EQ(winU(&a, &slow), 1);
	CHECK(gD.reason == KVM_AUTO_R_CPU || gD.reason == KVM_AUTO_R_CAPTURE); CHECK_EQ((int)a.upNeed, KVM_AUTO_UP_WINDOWS);
	good = win_at(a.step, 24);
	for (i = 0; i < KVM_AUTO_UP_WINDOWS - 1; i++) { CHECK_EQ(winU(&a, &good), 0); }
	CHECK_EQ(winU(&a, &good), 1); CHECK_EQ(gD.reason, KVM_AUTO_R_BACK);
	begin(&a, 0);
	{ kvm_auto_win cap = win_at(0, 26); cap.capP75Ms = 30; cap.made = 50; cap.pendingIters = 50; cap.qpN = 50; cap.qpSum = 50u * 26u;
	  CHECK_EQ(winU(&a, &cap), 0); CHECK_EQ(winU(&a, &cap), 1); CHECK_EQ(gD.reason, KVM_AUTO_R_CAPTURE); CHECK_EQ((int)a.upNeed, KVM_AUTO_UP_WINDOWS); }
	// the order is the doubling first and the wait after it: an attempt with the first patience (4) that ends in a step down for the link ends at 64, not at the doubled 8
	begin(&a, 1);
	good = win_at(1, 24);
	for (i = 0; i < KVM_AUTO_UP_WINDOWS - 1; i++) { CHECK_EQ(winU(&a, &good), 0); }
	CHECK_EQ(winU(&a, &good), 1); CHECK_EQ((int)a.step, 0); CHECK_EQ((int)a.upNeed, KVM_AUTO_UP_WINDOWS);
	linkBad = win_at(0, 35);															// the QP over the ceiling of 60 frames a second
	CHECK_EQ(winU(&a, &linkBad), 0); CHECK_EQ(winU(&a, &linkBad), 1);
	CHECK_EQ((int)a.step, 1); CHECK_EQ(gD.reason, KVM_AUTO_R_LINK); CHECK_EQ((int)a.upNeed, KVM_AUTO_LINK_WAIT);
	// a step down for the CPU (the cost the budget of the encoder needs does not fit the interval) is a step down for the machine: the first patience stays, and the next attempt is the 4th good window
	begin(&a, 0);
	e = env_at(0); e.floorMs = 20;
	w = win_at(0, 26);
	CHECK_EQ(win(&a, &w, e), 0); CHECK_EQ(win(&a, &w, e), 1);
	CHECK_EQ(gD.reason, KVM_AUTO_R_CPU); CHECK((int)a.step >= 1); CHECK_EQ((int)a.upNeed, KVM_AUTO_UP_WINDOWS);
	good = win_at(a.step, 24);
	for (i = 0; i < KVM_AUTO_UP_WINDOWS - 1; i++) { CHECK_EQ(winU(&a, &good), 0); }
	CHECK_EQ(winU(&a, &good), 1); CHECK_EQ(gD.reason, KVM_AUTO_R_BACK);
	// a ceiling from outside (the screen, the battery saver, the budget) is no link either
	begin(&a, 0);
	e = env_at(gT + 10); e.maxFps = 30; e.maxReason = KVM_AUTO_R_SCREEN;
	CHECK_EQ(kvm_auto_decide(&a, NULL, &e, &gD), 1); CHECK_EQ((int)a.upNeed, KVM_AUTO_UP_WINDOWS);
	// a failed attempt after a step down for the machine doubles as before: 4 -> 8 (the link wait does not apply)
	begin(&a, 1);
	good = win_at(1, 24); slow = win_at(0, 26); slow.made = 40; slow.pendingIters = 40; slow.qpN = 40; slow.qpSum = 40u * 26u;
	for (i = 0; i < 3; i++) { CHECK_EQ(winU(&a, &good), 0); }
	CHECK_EQ(winU(&a, &good), 1); CHECK_EQ(winU(&a, &slow), 0); CHECK_EQ(winU(&a, &slow), 1);
	CHECK_EQ((int)a.upNeed, 8);
}

// the case that was measured in the browser (a link of 3 Mb/s that is full at 30 frames a second, a queue of 160-180 ms every few seconds, quiet at 20): the patience of 4 and then 8 windows took the step
// up after 5 s and 9 s and down again each time, 5 changes in 23 s. A scene of a minute at a time: in no 60 s more than 2 changes, the first step down is the link, the waits between the attempts do
// not shrink, and in 10 minutes few changes
static void test_link_trace(void)
{
	static const double lagPer2s[15] = { 98.1, 67.4, 66.6, 163.2, 164.7, 82, 67, 66.8, 67.4, 67.1, 51, 182.6, 58.3, 89.2, 58.5 };
	kvm_auto a;
	uint32_t tc[64], tu[64];
	int nc = 0, nu = 0, firstLink = 0, i, j, c, mx = 0;
	begin(&a, 1);
	for (i = 0; i < 600; i++)
	{
		const int spike = lagPer2s[(i / 2) % 15] > 150.0;
		const int step = a.step;
		kvm_auto_win w;
		if (step >= 2) { w = win_at(step, 26); }										// 20 frames a second and slower: the queue is gone
		else if (spike) { w = win_at(step, 35); }										// the QP of the regulator over the ceiling: the link queues
		else { w = win_at(step, 29); }													// between the spikes: nothing against staying, not enough room for the faster step
		if (winU(&a, &w) && nc < 64)
		{
			tc[nc] = (uint32_t)i;
			if (nc == 0) { firstLink = gD.reason == KVM_AUTO_R_LINK; }
			if (gD.reason == KVM_AUTO_R_BACK && nu < 64) { tu[nu++] = (uint32_t)i; }
			nc++;
		}
	}
	CHECK(nc < 64);
	CHECK(firstLink);
	for (i = 0; i < nc; i++)
	{
		c = 0;
		for (j = 0; j < nc; j++) { if (tc[j] >= tc[i] && tc[j] < tc[i] + 60u) { c++; } }
		if (c > mx) { mx = c; }
	}
	CHECK(mx <= 2);
	for (i = 2; i < nu; i++) { CHECK((int)(tu[i] - tu[i - 1]) >= (int)(tu[i - 1] - tu[i - 2]) - 15); }		// the wait does not shrink (64, then 120 windows)
	CHECK(nc <= 14);
	CHECK(nu >= 1 && tu[0] >= 64u);																				// the first attempt is not before the 64th window
}

// a ceiling from outside
static void test_ceiling(void)
{
	kvm_auto a;
	kvm_auto_dec d;
	kvm_auto_env e;
	kvm_auto_win w = win_at(0, 26);
	begin(&a, 0);
	e = env_at(gT + 10); e.maxFps = 30; e.maxReason = KVM_AUTO_R_SCREEN;
	CHECK_EQ(kvm_auto_decide(&a, NULL, &e, &d), 1);						// without a window, at once
	CHECK_EQ((int)a.step, 1); CHECK_EQ(d.reason, KVM_AUTO_R_SCREEN); CHECK_EQ(d.from, 0); CHECK_EQ(d.to, 1);
	// two steps at once
	begin(&a, 0);
	e = env_at(gT + 10); e.maxFps = 15; e.maxReason = KVM_AUTO_R_POWER;
	CHECK_EQ(kvm_auto_decide(&a, NULL, &e, &d), 1);
	CHECK_EQ((int)a.step, 3); CHECK_EQ(d.reason, KVM_AUTO_R_POWER);
	// 25: the 20 step
	begin(&a, 0);
	e.maxFps = 25;
	CHECK_EQ(kvm_auto_decide(&a, NULL, &e, &d), 1);
	CHECK_EQ((int)a.step, 2);
	// below the slowest step: the slowest
	begin(&a, 0);
	e.maxFps = 5;
	CHECK_EQ(kvm_auto_decide(&a, NULL, &e, &d), 1);
	CHECK_EQ((int)a.step, 4);
	// not within the 2 s that hold the other steps back
	begin(&a, 0);
	a.lastChangeAt = gT; a.haveChange = 1;
	e = env_at(gT + 5); e.maxFps = 30; e.maxReason = KVM_AUTO_R_BUDGET;
	CHECK_EQ(kvm_auto_decide(&a, NULL, &e, &d), 1);
	CHECK_EQ(d.reason, KVM_AUTO_R_BUDGET);
	// a window without motion changes nothing about it
	begin(&a, 0);
	e = env_at(gT + 10); e.maxFps = 30; e.valid = 0;
	w.motionMs = 0;
	CHECK_EQ(kvm_auto_decide(&a, &w, &e, &d), 1);
	// the same ceiling does nothing at the step that fits
	begin(&a, 1);
	e = env_at(gT + 10); e.maxFps = 30;
	CHECK_EQ(kvm_auto_decide(&a, NULL, &e, &d), 0);
	// no reason given: the screen
	begin(&a, 0);
	e = env_at(gT + 10); e.maxFps = 30; e.maxReason = 0;
	CHECK_EQ(kvm_auto_decide(&a, NULL, &e, &d), 1);
	CHECK_EQ(d.reason, KVM_AUTO_R_SCREEN);
	// no ceiling: nothing
	begin(&a, 0);
	e = env_at(gT + 10); e.maxFps = 0;
	CHECK_EQ(kvm_auto_decide(&a, NULL, &e, &d), 0);
	// the ceiling is not applied before the start
	kvm_auto_init(&a); kvm_auto_set(&a, 1, 1000);
	e = env_at(1100); e.minRttMs = 0; e.maxFps = 15;
	CHECK_EQ(kvm_auto_decide(&a, NULL, &e, &d), 0);
	CHECK_EQ((int)a.step, 1);
}

// the accumulators and the window
static void test_tick(void)
{
	kvm_auto a;
	kvm_auto_dec d;
	kvm_auto_win w;
	kvm_auto_env e;
	int i;
	uint32_t t = 200000;
	kvm_auto_init(&a);
	kvm_auto_set(&a, 1, t);
	e = env_at(t); e.minRttMs = 5;
	CHECK_EQ(kvm_auto_tick(&a, &e, &d, &w), 1);							// the start: 60
	CHECK_EQ((int)a.step, 0);
	// a second of motion at 60 per second: 16 ms apart, QP 26, capture 8 ms
	for (i = 0; i < 60; i++)
	{
		const uint32_t now = t + 16u * (uint32_t)i;
		kvm_auto_note_capture(&a, 8, 1);
		kvm_auto_note_image(&a, now, 100);
		kvm_auto_note_frame(&a, now, 26, 1000);
		e = env_at(now);
		CHECK_EQ(kvm_auto_tick(&a, &e, &d, &w), 0);
	}
	memset(&w, 0, sizeof(w));
	e = env_at(t + 1000);
	CHECK_EQ(kvm_auto_tick(&a, &e, &d, &w), 0);
	CHECK_EQ(d.evaluated, 1);
	CHECK_EQ((int)w.windowMs, 1000);
	CHECK_EQ((int)w.made, 60); CHECK_EQ((int)w.pendingIters, 60);
	CHECK_EQ((int)w.qpN, 60); CHECK_EQ((int)w.qpSum, 60 * 26); CHECK_EQ((int)w.qpEnd, 26);
	CHECK_EQ((int)w.capP75Ms, 8); CHECK_EQ((int)w.gapMedMs, 16);
	CHECK_EQ((int)w.dwmX100, 100);
	CHECK(w.motionMs >= 900 && w.motionMs <= 1000);
	CHECK_EQ((int)w.heldWin, 0); CHECK_EQ((int)w.heldRate, 0);
	// the accumulators are clear for the next window
	CHECK_EQ((int)a.made, 0); CHECK_EQ((int)a.pendingIters, 0); CHECK_EQ((int)a.qpN, 0); CHECK_EQ((int)a.capN, 0); CHECK_EQ((int)a.gapN, 0); CHECK_EQ((int)a.motionAcc, 0);
	CHECK_EQ((int)a.winStart, (int)(t + 1000));
	// the window does not close before a second: 999 ms
	t += 1000;
	kvm_auto_note_image(&a, t + 10, 5);
	e = env_at(t + 999);
	CHECK_EQ(kvm_auto_tick(&a, &e, &d, &w), 0);
	CHECK_EQ(d.evaluated, 0);
	CHECK_EQ((int)a.pendingIters, 1);									// still open
	e = env_at(t + 1000);
	(void)kvm_auto_tick(&a, &e, &d, &w);
	CHECK_EQ((int)a.pendingIters, 0);
	CHECK_EQ(d.evaluated, 0);											// 100 ms of motion: frozen
	CHECK(w.motionMs > 0 && w.motionMs < (uint32_t)KVM_AUTO_MIN_MOTION_MS);
	// held counters, the span of the motion, the means
	t += 1000;
	for (i = 0; i < 10; i++) { kvm_auto_note_image(&a, t + 100u * (uint32_t)i + 5u, 10); }
	kvm_auto_note_held(&a, 0); kvm_auto_note_held(&a, 0); kvm_auto_note_held(&a, 1);
	kvm_auto_note_frame(&a, t + 50, 30, 1000); kvm_auto_note_frame(&a, t + 150, 32, 1000); kvm_auto_note_frame(&a, t + 250, 34, 1000);
	e = env_at(t + 1000);
	(void)kvm_auto_tick(&a, &e, &d, &w);
	CHECK_EQ((int)w.heldWin, 2); CHECK_EQ((int)w.heldRate, 1);
	CHECK_EQ((int)w.made, 3); CHECK_EQ((int)w.qpSum, 96); CHECK_EQ((int)w.qpEnd, 34); CHECK_EQ((int)w.bytes, 3000);
	CHECK(w.motionMs >= 900 && w.motionMs <= 1000);
	CHECK_EQ((int)w.pendingIters, 10);
	CHECK_EQ((int)w.gapMedMs, 100);
	CHECK_EQ((int)w.dwmX100, 0);										// no DXGI information
	// a note without pending tiles is a break of the series: no gap across it, the capture is dropped
	t += 1000;
	kvm_auto_note_capture(&a, 90, 3);
	kvm_auto_note_image(&a, t + 10, 0);
	kvm_auto_note_image(&a, t + 20, 4);
	kvm_auto_note_image(&a, t + 40, 4);
	e = env_at(t + 1000);
	(void)kvm_auto_tick(&a, &e, &d, &w);
	CHECK_EQ((int)w.capP75Ms, 0);
	CHECK_EQ((int)w.gapMedMs, 20);
	CHECK_EQ((int)w.pendingIters, 2);
	// a gap of 500 ms or more is a pause between series
	t += 1000;
	kvm_auto_note_image(&a, t + 10, 4);
	kvm_auto_note_image(&a, t + 600, 4);
	e = env_at(t + 1000);
	(void)kvm_auto_tick(&a, &e, &d, &w);
	CHECK_EQ((int)w.gapMedMs, 0);
	// P75: 10 samples of 1..10 ms: the 8th is 8
	t += 1000;
	for (i = 1; i <= 10; i++) { kvm_auto_note_capture(&a, (uint32_t)i, 0); kvm_auto_note_image(&a, t + 10u * (uint32_t)i, 3); }
	e = env_at(t + 1000);
	(void)kvm_auto_tick(&a, &e, &d, &w);
	CHECK_EQ((int)w.capP75Ms, 8);
	// 4 samples: the 3rd
	t += 1000;
	for (i = 1; i <= 4; i++) { kvm_auto_note_capture(&a, (uint32_t)(i * 10), 0); kvm_auto_note_image(&a, t + 10u * (uint32_t)i, 3); }
	e = env_at(t + 1000);
	(void)kvm_auto_tick(&a, &e, &d, &w);
	CHECK_EQ((int)w.capP75Ms, 30);
	// a capture of more than 255 ms is 255; dwm 2 frames per image: 200
	t += 1000;
	kvm_auto_note_capture(&a, 900, 2); kvm_auto_note_image(&a, t + 10, 3);
	e = env_at(t + 1000);
	(void)kvm_auto_tick(&a, &e, &d, &w);
	CHECK_EQ((int)w.capP75Ms, 255); CHECK_EQ((int)w.dwmX100, 200);
	// a long idle: the window that closes is a window without motion and the next one starts at the tick
	t += 60000;
	e = env_at(t);
	(void)kvm_auto_tick(&a, &e, &d, &w);
	CHECK_EQ(d.evaluated, 0); CHECK_EQ((int)w.windowMs, 1000 + 59000 - 1000 + 0);
}

// the tick drives the whole thing: a link that falls
static void test_tick_decides(void)
{
	kvm_auto a;
	kvm_auto_dec d;
	kvm_auto_env e;
	uint32_t t = 300000;
	int s, i, changed = 0;
	kvm_auto_init(&a);
	kvm_auto_set(&a, 1, t);
	e = env_at(t);
	(void)kvm_auto_tick(&a, &e, &d, NULL);
	for (s = 0; s < 4 && !changed; s++)
	{
		for (i = 0; i < 60; i++)
		{
			const uint32_t now = t + 1000u * (uint32_t)s + 16u * (uint32_t)i;
			kvm_auto_note_image(&a, now, 100);
			kvm_auto_note_frame(&a, now, 36, 1000);
			if (i % 4 == 0) { kvm_auto_note_held(&a, 1); }
			e = env_at(now);
			(void)kvm_auto_tick(&a, &e, &d, NULL);
		}
		e = env_at(t + 1000u * (uint32_t)(s + 1));
		changed = kvm_auto_tick(&a, &e, &d, NULL);
	}
	CHECK_EQ(changed, 1); CHECK_EQ(s, 2);
	CHECK_EQ((int)a.step, 1); CHECK_EQ(d.reason, KVM_AUTO_R_LINK);
}


// the slow laptop stays at 60 and goes back to it; an attempt that cannot succeed does not go on for ever
static void test_review_b2_b3(void)
{
	kvm_auto a;
	int i, c, downs;
	kvm_auto_win w;
	// 50 frames a second made, the median gap 22 ms (a jitter), the source faster than the loop (dwm 1.3): that is the uneven 45..55 that stays at 60
	begin(&a, 0);
	for (i = 0; i < 10; i++)
	{
		w = win_at(0, 26); w.made = 50; w.pendingIters = 50; w.qpN = 50; w.qpSum = 50u * 26u; w.gapMedMs = 22; w.capP75Ms = 9; w.dwmX100 = 130;
		CHECK_EQ(winU(&a, &w), 0);
	}
	CHECK_EQ((int)a.step, 0);
	// the median 23 ms is 43 frames a second whatever is made: the step down is the loop's
	w.gapMedMs = 23;
	(void)winU(&a, &w); CHECK_EQ(winU(&a, &w), 1); CHECK_EQ(gD.reason, KVM_AUTO_R_CAPTURE);
	// ... and the same machine comes back to 60 when the capture of ~19 ms (P75 20) is what limits it: the threshold of going up is the one of staying
	begin(&a, 1);
	w = win_at(1, 24); w.capP75Ms = 20; w.made = 30;
	for (i = 0; i < 3; i++) { CHECK_EQ(winU(&a, &w), 0); }
	CHECK_EQ(winU(&a, &w), 1); CHECK_EQ((int)a.step, 0);
	// 30 is good, 60 is not (the QP 34 over the ceiling at 60): 600 s of it make no more than 2 changes a minute, and the patience rises to its ceiling
	begin(&a, 1);
	c = 0; downs = 0;
	for (i = 0; i < 600; i++)
	{
		kvm_auto_env e = env_at(0);
		const int qp = a.step == 0 ? 34 : 24;
		w = win_at(a.step, qp);
		e.qpNow = (uint32_t)qp;
		if (win(&a, &w, e)) { c++; if (gD.to > gD.from) { downs++; } }
	}
	CHECK(c <= 20);
	CHECK(downs >= 6);
	CHECK_EQ((int)a.upNeed, KVM_AUTO_UP_MAX);
	// an attempt that the machine cannot keep up (the GDI capture of a source of 30 frames a second: the step 60 makes 30 and looks slow): the same
	begin(&a, 1);
	c = 0;
	for (i = 0; i < 600; i++)
	{
		w = win_at(a.step, 24);
		if (a.step == 0) { w.made = 30; w.pendingIters = 30; w.qpN = 30; w.qpSum = 30u * 24u; }
		if (winU(&a, &w)) { c++; }
	}
	CHECK(c <= 20);
	// a success gives the first patience back and the next failure starts again from 8
	a.step = 0; a.haveUp = 1; a.lastUpAt = gT; a.upStep = 0; a.upNeed = 120;
	w = win_at(0, 26);
	for (i = 0; i < 61; i++) { (void)winU(&a, &w); }
	CHECK_EQ((int)a.upNeed, 4);
}


// the last few boundaries: the budget floor against the interval of a step, the gap across a break of a series, the median
static void test_review_edges(void)
{
	kvm_auto a;
	kvm_auto_dec d;
	kvm_auto_env e;
	kvm_auto_win w;
	uint32_t t = 700000;
	int i;
	// the floor of the budget equal to the interval of the step (50 ms at 20) is no limit, 51 is
	begin(&a, 2);
	w = win_at(2, 26); e = env_at(0); e.floorMs = 50;
	(void)win(&a, &w, e); CHECK(!(gD.flags & KVM_AUTO_F_FLOOR));
	e.floorMs = 51;
	(void)win(&a, &w, e); CHECK(gD.flags & KVM_AUTO_F_FLOOR);
	// ... and the faster step of 50 ms (from 15) is not blocked by a floor of 50
	begin(&a, 3);
	w = win_at(3, 26); e = env_at(0); e.floorMs = 50;
	for (i = 0; i < 3; i++) { CHECK_EQ(win(&a, &w, e), 0); }
	CHECK_EQ(win(&a, &w, e), 1);
	begin(&a, 3);
	e.floorMs = 51;
	for (i = 0; i < 8; i++) { CHECK_EQ(win(&a, &w, e), 0); }
	// the gap is not measured across an image without tiles waiting (a break of the series)
	kvm_auto_init(&a); kvm_auto_set(&a, 1, t);
	e = env_at(t); e.minRttMs = 5; (void)kvm_auto_tick(&a, &e, &d, &w);
	kvm_auto_note_image(&a, t + 10, 4);
	kvm_auto_note_image(&a, t + 20, 0);
	kvm_auto_note_image(&a, t + 30, 4);
	e = env_at(t + 1000);
	(void)kvm_auto_tick(&a, &e, &d, &w);
	CHECK_EQ((int)w.gapMedMs, 0);
	// the median of the gaps, not their P75: seven of 16 ms and three of 40 ms
	t += 1000;
	{
		uint32_t at = t + 5;
		const int gaps[10] = { 16, 16, 40, 16, 16, 40, 16, 16, 40, 16 };
		kvm_auto_note_image(&a, at, 4);
		for (i = 0; i < 10; i++) { at += (uint32_t)gaps[i]; kvm_auto_note_image(&a, at, 4); }
	}
	e = env_at(t + 1000);
	(void)kvm_auto_tick(&a, &e, &d, &w);
	CHECK_EQ((int)w.gapMedMs, 16);
}


// a clock that has gone round its 32 bits (24.9 days of one process) is not a reason to freeze the mode or to hold its steps back
static void test_review_wrap(void)
{
	kvm_auto a;
	kvm_auto_dec d;
	kvm_auto_env e;
	kvm_auto_win w;
	uint32_t t0 = 1000;
	int i, frozen = 0;
	kvm_auto_init(&a);
	kvm_auto_set(&a, 1, t0);
	kvm_auto_note_key(&a, t0, 0);
	e = env_at(t0 + 1000); e.minRttMs = 5;
	(void)kvm_auto_tick(&a, &e, &d, &w);
	t0 += 1000u;
	t0 += 2147483648u + 5000u;											// 2^31 ms + 5 s without a key frame
	for (i = 0; i < 3; i++)
	{
		uint32_t k;
		for (k = 0; k < 1000; k += 16) { kvm_auto_note_image(&a, t0 + k, 1); kvm_auto_note_frame(&a, t0 + k, 26, 1000); }
		t0 += 1000; e = env_at(t0); e.qpNow = 26;
		memset(&w, 0, sizeof(w));
		(void)kvm_auto_tick(&a, &e, &d, &w);
		if (w.key) { frozen++; }
	}
	CHECK_EQ(frozen, 0);
	CHECK_EQ((int)a.haveKey, 0);
	// a freeze that nobody looked at for 2^31 ms (the loop was idle for 24.9 days): the stamp reads as "far in the future", and is not a freeze
	kvm_auto_init(&a); kvm_auto_set(&a, 1, 5000);
	kvm_auto_note_key(&a, 5000, 5000);									// frozen until 11000
	e = env_at(5000u + 6000u + 2147483648u + 10u); e.minRttMs = 5;
	for (i = 0; i < 1000; i += 16) { kvm_auto_note_image(&a, e.now - 1000u + (uint32_t)i, 1); }
	memset(&w, 0, sizeof(w));
	(void)kvm_auto_tick(&a, &e, &d, &w);
	CHECK_EQ(w.key, 1);													// (the key frame was noted in this window: that is the window that it freezes)
	e = env_at(e.now + 1000u);
	(void)kvm_auto_tick(&a, &e, &d, &w);
	CHECK_EQ(w.key, 0);
	CHECK_EQ((int)a.haveKey, 0);
	// a stamp of a key frame that is read after a window closed at once: 5 s of freeze, and 2^31 later nothing
	kvm_auto_init(&a); kvm_auto_set(&a, 1, 5000);
	e = env_at(5000u + 1000u); e.minRttMs = 5; (void)kvm_auto_tick(&a, &e, &d, &w);
	kvm_auto_note_key(&a, 6000u, 5000u);								// until 12000
	e = env_at(6000u + 6000u + 2147483648u + 10u);
	(void)kvm_auto_tick(&a, &e, &d, &w);
	CHECK_EQ(w.key, 1);													// the window that holds the note
	e = env_at(e.now + 1000u);
	for (i = 0; i < 1000; i += 16) { kvm_auto_note_image(&a, e.now - 1000u + (uint32_t)i, 1); }
	(void)kvm_auto_tick(&a, &e, &d, &w);
	CHECK_EQ(w.key, 0);
	// a new key frame 2^31 ms after an old one that nobody looked at: its freeze is its own (the old stamp reads as "far ahead" and must not win over it)
	kvm_auto_init(&a); kvm_auto_set(&a, 1, 5000);
	kvm_auto_note_key(&a, 5000u, 5000u);
	{
		const uint32_t tn = 5000u + 6000u + 2147483648u + 10u;
		kvm_auto_note_key(&a, tn, 5000u);								// until tn + 6000
		for (i = 0; i < 1000; i += 16) { kvm_auto_note_image(&a, tn + (uint32_t)i, 1); }
		e = env_at(tn + 1000u); e.minRttMs = 5; (void)kvm_auto_tick(&a, &e, &d, &w);
		CHECK_EQ(w.key, 1);
		for (i = 0; i < 1000; i += 16) { kvm_auto_note_image(&a, tn + 1000u + (uint32_t)i, 1); }
		e = env_at(tn + 2000u); (void)kvm_auto_tick(&a, &e, &d, &w);
		CHECK_EQ(w.key, 1);												// the key frame needs 5 s on the link: the next window is frozen too
	}
	// ... and a step that is held back by the stamp of the last change that is 2^31 ms old: the step is made
	begin(&a, 0);
	a.lastChangeAt = gT - 2147483648u + 3000u;						// "in 24.8 days" for dms(), long ago for the mode
	w = win_at(0, 26); w.made = 40; w.pendingIters = 40; w.qpN = 40; w.qpSum = 1040;
	CHECK_EQ(winU(&a, &w), 0);
	CHECK_EQ(winU(&a, &w), 1);
	// a stamp that is a few seconds ahead (a late note) is 0 ms ago, not long ago
	begin(&a, 0);
	a.lastChangeAt = gT + 3000;
	CHECK_EQ(winU(&a, &w), 0);
	CHECK_EQ(winU(&a, &w), 0);										// 3 s ahead, 1 s and 2 s later: the step is too young
	// a window that began 2^31 ms ago closes at once
	kvm_auto_init(&a); kvm_auto_set(&a, 1, 5000);
	e = env_at(5000); e.minRttMs = 5; (void)kvm_auto_tick(&a, &e, &d, &w);
	e = env_at(5000u + 2147483648u + 10u); (void)kvm_auto_tick(&a, &e, &d, &w);
	CHECK_EQ((int)a.winStart, (int)(5000u + 2147483648u + 10u));
}


// the time of motion is the span of the series (gaps up to 250 ms, one step for the first image of a series), not slots of 100 ms that make 5 frames of 33 ms 200 ms and the rate 25
static void test_motion_span(void)
{
	kvm_auto a;
	kvm_auto_dec d;
	kvm_auto_env e;
	kvm_auto_win w;
	uint32_t t = 900000;
	int i;
	kvm_auto_init(&a); kvm_auto_set(&a, 1, t);
	e = env_at(t); e.minRttMs = 40; (void)kvm_auto_tick(&a, &e, &d, &w);			// the start: 30 (the step is 33 ms)
	CHECK_EQ((int)a.step, 1);
	// 5 frames of 33 ms: the first is a step, the other four are their gaps: 165 ms
	t += 100;
	for (i = 0; i < 5; i++) { kvm_auto_note_image(&a, t + 33u * (uint32_t)i, 4); kvm_auto_note_frame(&a, t + 33u * (uint32_t)i, 26, 1000); }
	e = env_at(t + 1000u); (void)kvm_auto_tick(&a, &e, &d, &w);
	CHECK_EQ((int)w.motionMs, 165);
	CHECK_EQ((int)w.made, 5);
	// the image and the frame of one moment are one step; a gap of 500 ms still belongs to the series (the loop may run less often than every 250 ms), 501 starts another (one more step)
	kvm_auto_init(&a); kvm_auto_set(&a, 1, t);
	e = env_at(t); e.minRttMs = 40; (void)kvm_auto_tick(&a, &e, &d, &w);			// the start: 30
	kvm_auto_note_image(&a, t + 10, 4); kvm_auto_note_frame(&a, t + 10, 26, 1000);	// a step: 33
	kvm_auto_note_image(&a, t + 510, 4);											// 500 after: a gap
	CHECK_EQ((int)a.motionAcc, 33 + 500);
	kvm_auto_note_image(&a, t + 1011, 4);											// 501 after: a step
	CHECK_EQ((int)a.motionAcc, 33 + 500 + 33);
	// the images of a loop that runs every 260 ms (the pause of WA-2 x 4 at 15 frames a second) with every frame refused: 4 images a second are motion of ~1000 ms, not 4 steps of 66 ms that
	// no window may judge. The link is the limit (everything refused, the QP asked for is high): two windows, one step down. Before the correction nothing happened: 0 of 8 windows were evaluated.
	begin(&a, 3);
	{
		int n, changed = 0;
		kvm_auto_env e2 = env_at(0);
		for (n = 0; n < 8 && !changed; n++)
		{
			uint32_t at;
			gT += 1000;
			for (at = 0; at < 1000; at += 260) { kvm_auto_note_image(&a, gT - 1000u + at, 100); kvm_auto_note_held(&a, 1); }
			e2.now = gT; e2.qpNow = 38;
			memset(&w, 0, sizeof(w));
			changed = kvm_auto_tick(&a, &e2, &d, &w);
			if (n == 0) { CHECK(w.motionMs >= (uint32_t)KVM_AUTO_MIN_MOTION_MS); }
		}
		CHECK(changed);
		CHECK_EQ((int)a.step, 4);
		CHECK(d.reason == KVM_AUTO_R_LINK || d.reason == KVM_AUTO_R_CAPTURE);	// (the gap of the images is 260 ms against an interval of 66: the loop does not keep its schedule, too)
		CHECK(n <= 4);
	}
	// the office: bursts of 10 frames of 33 ms (330 ms of motion) once a second: the rate is the 30 of the source, whatever the phase against the window; the slots of 100 ms said 25
	{
		int k, minAch = 100000, maxAch = 0;
		kvm_auto_init(&a); kvm_auto_set(&a, 1, t);
		e = env_at(t); e.minRttMs = 40; (void)kvm_auto_tick(&a, &e, &d, &w);
		for (k = 0; k < 8; k++)
		{
			const uint32_t base = t + 3000u + 1000u * (uint32_t)k + 37u * (uint32_t)k;		// the phase moves against the windows
			for (i = 0; i < 10; i++) { kvm_auto_note_image(&a, base + 33u * (uint32_t)i, 4); kvm_auto_note_frame(&a, base + 33u * (uint32_t)i, 26, 1000); }
		}
		for (k = 0; k < 9; k++)
		{
			e = env_at(t + 1000u * (uint32_t)(k + 2) + 3000u); e.qpNow = 26;
			memset(&w, 0, sizeof(w));
			(void)kvm_auto_tick(&a, &e, &d, &w);
			if (d.evaluated) { if ((int)d.achieved10 < minAch) { minAch = (int)d.achieved10; } if ((int)d.achieved10 > maxAch) { maxAch = (int)d.achieved10; } }
		}
		CHECK(maxAch > 0);
		CHECK(minAch >= 280 && maxAch <= 330);							// 30.3 a second (10 frames in 330 ms), not 25
	}
	// a pause of the video forgets the series: the first image after it is a step again
	kvm_auto_resume(&a, t + 20000u);
	CHECK_EQ((int)a.haveMotion, 0); CHECK_EQ((int)a.motionAcc, 0);
}


// a pause of the video
static void test_resume(void)
{
	kvm_auto a;
	kvm_auto_env e;
	kvm_auto_win w;
	kvm_auto_dec d;
	int i;
	kvm_auto_resume(&a, 1000);											// (a state that was never used: nothing to do, and with the bit down nothing happens)
	kvm_auto_init(&a);
	kvm_auto_resume(&a, 1000);
	CHECK_EQ((int)a.on, 0);
	begin(&a, 0);
	w = win_at(0, 26);
	(void)winU(&a, &w);
	kvm_auto_note_image(&a, gT + 10, 4); kvm_auto_note_frame(&a, gT + 10, 26, 100); kvm_auto_note_held(&a, 1); kvm_auto_note_capture(&a, 7, 1); kvm_auto_note_key(&a, gT + 10, 100);
	a.cpuStreak = 1; a.qpStreak = 1; a.goodStreak = 1;
	// 3 s later: the step stays, the window, the series and the streaks are gone
	kvm_auto_resume(&a, gT + 3000);
	CHECK(a.started); CHECK_EQ((int)a.step, 0);
	CHECK_EQ((int)a.made, 0); CHECK_EQ((int)a.pendingIters, 0); CHECK_EQ((int)a.heldRate, 0); CHECK_EQ((int)a.bytes, 0); CHECK_EQ((int)a.haveWin, 0); CHECK_EQ((int)a.haveKey, 0); CHECK_EQ((int)a.keyInWin, 0);
	CHECK_EQ((int)a.haveMotion, 0); CHECK_EQ((int)a.motionAcc, 0); CHECK_EQ((int)a.haveImage, 0); CHECK_EQ((int)a.haveCap, 0); CHECK_EQ((int)a.capN, 0);
	CHECK_EQ((int)a.cpuStreak, 0); CHECK_EQ((int)a.qpStreak, 0); CHECK_EQ((int)a.goodStreak, 0);
	// more than 30 s (30 s and 1 ms) since the last evaluated window: the start again
	begin(&a, 0);
	(void)winU(&a, &w);
	kvm_auto_resume(&a, gT + KVM_AUTO_STALE_MS);
	CHECK(a.started);													// exactly 30 s: still the same stream
	kvm_auto_resume(&a, gT + KVM_AUTO_STALE_MS + 1);
	CHECK(!a.started); CHECK_EQ((int)a.step, 1); CHECK_EQ((int)a.beginAt, (int)(gT + KVM_AUTO_STALE_MS + 1)); CHECK_EQ((int)a.haveChange, 0); CHECK_EQ((int)a.haveUp, 0);
	CHECK_EQ(kvm_auto_interval_ms(&a), 0);								// before the start the period of command 5 holds
	e = env_at(gT + KVM_AUTO_STALE_MS + 100u); e.minRttMs = 5;
	CHECK_EQ(kvm_auto_decide(&a, NULL, &e, &d), 1);						// ... and the start comes from the round trip
	CHECK_EQ((int)a.step, 0);
	// never evaluated: the gap is taken from the last change (the start)
	kvm_auto_init(&a); kvm_auto_set(&a, 1, 1000);
	e = env_at(1100); e.minRttMs = 5; (void)kvm_auto_decide(&a, NULL, &e, &d);
	kvm_auto_resume(&a, 1100u + KVM_AUTO_STALE_MS + 1u);
	CHECK(!a.started);
	// a long pause in the middle of the patience: the patience is the one that was earned
	begin(&a, 1);
	a.upNeed = 32;
	(void)winU(&a, &w);
	kvm_auto_resume(&a, gT + 5u * KVM_AUTO_STALE_MS);
	CHECK_EQ((int)a.upNeed, 32);
	for (i = 0; i < 1; i++) { (void)i; }
}

static void test_format(void)
{
	kvm_auto a;
	kvm_auto_dec d;
	kvm_auto_win w = win_at(0, 26);
	char line[1024];
	int n;
	begin(&a, 0);
	w.made = 20; w.pendingIters = 20; w.qpN = 20; w.qpSum = 520;
	(void)winU(&a, &w);
	CHECK_EQ(winU(&a, &w), 1);
	n = kvm_auto_format(&a, &w, &gD, line, (int)sizeof(line));
	CHECK(n > 0 && n < (int)sizeof(line));
	CHECK(strstr(line, "auto on=1 step=2 fps=20 interval_ms=50 changed=1 reason=capture") != NULL);
	CHECK(strstr(line, "achieved_fps=20.0") != NULL);
	CHECK(strstr(line, "qp_mean=26.0") != NULL);
	CHECK(strstr(line, "downs=1") != NULL);
	memset(&d, 0, sizeof(d));
	n = kvm_auto_format(&a, NULL, &d, line, (int)sizeof(line));
	CHECK(n > 0);
	CHECK(strstr(line, "changed=0") != NULL);
	CHECK(strstr(line, "reason=capture") != NULL);							// the last reason stays in the line
	kvm_auto_set(&a, 0, 0);
	n = kvm_auto_format(&a, NULL, &d, line, 16);						// cut, not overrun
	CHECK(n > 0);
}

int main(void)
{
	test_tables();
	test_off();
	test_start();
	test_frozen();
	test_cpu();
	test_capture();
	test_link();
	test_key();
	test_queue();
	test_up();
	test_timing();
	test_patience();
	test_link_wait();
	test_link_trace();
	test_ceiling();
	test_tick();
	test_tick_decides();
	test_review_b2_b3();
	test_review_edges();
	test_review_wrap();
	test_motion_span();
	test_resume();
	test_format();
	printf("%d checks, %d failed\n", gChecks, gFail);
	return gFail == 0 ? 0 : 1;
}
