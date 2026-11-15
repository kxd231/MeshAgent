/*
Tests of kvm_pace.c (the schedule of the loop, the slots of the video frames, the CPU budget) and a simulation of the main loop with a synchronous encoder: the viewer's frame periods 16 / 33 /
50 / 100 ms against encode times of 8 / 15 / 25 / 40 ms on 2 and 4 cores, before (the rule of the interval at least twice the encode time, a pause of the period minus the iteration, a
frame only on the iteration that finds the interval over) and after. Time is injected; the output is the table of the report. Run with run-kvm-pace-tests.sh (gcc in a container:
-Werror -pedantic, then ASan + UBSan).
*/

#include "../kvm_pace.h"
#include "../kvm_video_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gFail = 0, gChecks = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); gChecks++; if (_a != _b) { gFail++; printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

// ---------------------------------------------------------------------------------------------------------------------------------------
// tick
// ---------------------------------------------------------------------------------------------------------------------------------------

static void test_tick_basic(void)
{
	kvm_tick t;
	kvm_tick_init(&t);
	// the first iteration is its own slot: started at 1000, ends at 1010, period 33: sleep until 1033
	CHECK_EQ(kvm_tick_next(&t, 1000, 1010, 33), 23);
	// the next one wakes on time at 1033, works 10 ms: the slot is 1066
	CHECK_EQ(kvm_tick_next(&t, 1033, 1043, 33), 23);
	// it wakes 1 ms late: the sleep is shorter, the slot does not move
	CHECK_EQ(kvm_tick_next(&t, 1067, 1077, 33), 22);
	CHECK_EQ(t.st.n, 2);
	CHECK_EQ(t.st.late, 0);									// 1 ms is below KVM_PACE_LATE_MS
	CHECK_EQ(kvm_tick_next(&t, 1100, 1105, 33), 27);		// 1 ms late again (slot 1099)
	CHECK_EQ(kvm_tick_next(&t, 1135, 1140, 33), 25);		// slot 1132: 3 ms late
	CHECK_EQ(t.st.late, 1); CHECK_EQ(t.st.maxMs, 3); CHECK_EQ(t.st.sumMs, 3);
	CHECK_EQ(kvm_tick_next(&t, 0, 0, 0), 0);				// no period: no schedule
	CHECK(!t.have);
}

static void test_tick_no_drift(void)
{
	// a wake that is 1 ms late every time: the old rule (the period minus the iteration) gives 34 ms per iteration, the schedule keeps 33
	kvm_tick t;
	uint32_t start = 5000, now, sleep, i;
	kvm_tick_init(&t);
	for (i = 0; i < 300; i++)
	{
		uint32_t work = 12;
		now = start + work;
		sleep = kvm_tick_next(&t, start, now, 33);
		start = now + sleep + 1;							// the system wakes us 1 ms late
	}
	// 300 iterations took 300 x 33 = 9900 ms (+ the 1 ms of the last wake), not 10200
	CHECK(start - 5000 >= 9900 && start - 5000 <= 9902);
}

static void test_tick_overrun(void)
{
	kvm_tick t;
	uint32_t s;
	kvm_tick_init(&t);
	CHECK_EQ(kvm_tick_next(&t, 0, 10, 33), 23);				// slot 33
	// the iteration that started at 33 took 50 ms: it ends at 83, the next slot (66) has passed by 17 ms: start at once, the debt is paid back inside the period
	CHECK_EQ(kvm_tick_next(&t, 33, 83, 33), 0);
	CHECK_EQ(t.st.over, 1); CHECK_EQ(t.st.resync, 0);
	// the next iteration starts at 83, ends at 93: its slot was 66 (+33 = 99): sleep 6 ms (the schedule is back on its grid)
	s = kvm_tick_next(&t, 83, 93, 33);
	CHECK_EQ(s, 6);
	// an iteration that takes two periods: a whole period behind, no catch-up burst: the schedule starts again at the end
	kvm_tick_init(&t);
	(void)kvm_tick_next(&t, 0, 10, 33);						// slot 33
	CHECK_EQ(kvm_tick_next(&t, 33, 130, 33), 0);			// next slot 66, ends at 130: 64 behind
	CHECK_EQ(t.st.resync, 1);
	CHECK_EQ(kvm_tick_next(&t, 130, 140, 33), 23);			// the new grid begins at 130
}

static void test_tick_idle_and_early(void)
{
	kvm_tick t;
	kvm_tick_init(&t);
	(void)kvm_tick_next(&t, 0, 10, 33);						// slot 33
	// woken early for another reason (a video slot): the slot stays, no iteration of the schedule is counted
	CHECK_EQ(kvm_tick_next(&t, 20, 25, 33), 8);
	CHECK_EQ(t.st.n, 0);
	CHECK_EQ(kvm_tick_next(&t, 32, 40, 33), 26);			// 1 ms before the slot is the slot (KVM_PACE_EARLY_MS): the next one is 66
	CHECK_EQ(t.st.n, 1);
	// idle: no credit for the time nothing happened
	kvm_tick_idle(&t);
	CHECK_EQ(kvm_tick_next(&t, 100000, 100005, 33), 28);
	CHECK_EQ(t.st.resync, 0);
}

static void test_tick_wrap(void)
{
	kvm_tick t;
	uint32_t start = 0xFFFFFF00u, now, i;
	kvm_tick_init(&t);
	for (i = 0; i < 20; i++)
	{
		uint32_t sleep;
		now = start + 5;
		sleep = kvm_tick_next(&t, start, now, 16);
		CHECK(sleep <= 16);
		start = now + sleep;
	}
	CHECK_EQ(t.st.resync, 0);
	CHECK_EQ(start - 0xFFFFFF00u, 320);						// 20 x 16 ms across the wrap of the counter
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// slots
// ---------------------------------------------------------------------------------------------------------------------------------------

static void test_slots(void)
{
	kvm_late l;
	kvm_late_clear(&l);
	CHECK_EQ(kvm_pace_slot(0, 0, 1000, 33, &l), 1000);		// no frame yet: the slot is now
	CHECK_EQ(kvm_pace_slot(1, 1000, 1033, 33, &l), 1033);	// on time
	CHECK_EQ(kvm_pace_slot(1, 1033, 1070, 33, &l), 1066);	// 4 ms late: the slot is where it was
	CHECK_EQ(l.late, 1); CHECK_EQ(l.maxMs, 4);
	CHECK_EQ(kvm_pace_slot(1, 1066, 1090, 33, &l), 1099);	// early (the system timer): the slot stays, the average rate holds
	CHECK_EQ(kvm_pace_slot(1, 1099, 1300, 33, &l), 1300);	// idle for 200 ms: no burst of frames to catch up
	CHECK_EQ(l.resync, 1);
	CHECK_EQ(kvm_pace_slot(1, 1099, 1131, 0, NULL), 1131);	// interval 0: now
	// the interval shrinks (the viewer asked for more): the slot is not in the past by more than the new interval
	CHECK_EQ(kvm_pace_slot(1, 2000, 2150, 16, NULL), 2150);
	// the wake
	CHECK_EQ(kvm_pace_wake_ms(0, 0, 5, 33), 0);
	CHECK_EQ(kvm_pace_wake_ms(1, 1000, 1010, 33), 23);
	CHECK_EQ(kvm_pace_wake_ms(1, 1000, 1033, 33), 0);
	CHECK_EQ(kvm_pace_wake_ms(1, 1000, 1500, 33), 0);
	CHECK_EQ(kvm_pace_wake_ms(1, 0xFFFFFFF0u, 5, 33), 12);	// across the wrap: slot 0xFFFFFFF0 + 33 = 17
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the budget
// ---------------------------------------------------------------------------------------------------------------------------------------

static void test_budget_init(void)
{
	kvm_budget b;
	kvm_budget_init(&b, 0); CHECK_EQ(b.cores, 1); CHECK_EQ(b.budgetMilli, 500);
	kvm_budget_init(&b, 1); CHECK_EQ(b.budgetMilli, 500);									// one processor: 50 % of the machine, not the whole of it
	kvm_budget_init(&b, 2); CHECK_EQ(b.budgetMilli, 1000);									// 50 % of 2 = 1 core, the minimum is one core
	kvm_budget_init(&b, 3); CHECK_EQ(b.budgetMilli, 1500);
	kvm_budget_init(&b, 4); CHECK_EQ(b.budgetMilli, 2000);
	kvm_budget_init(&b, 16); CHECK_EQ(b.budgetMilli, 8000);
	kvm_budget_init(&b, 64); CHECK_EQ(b.budgetMilli, 32000);
}

static void test_enc_floor(void)
{
	kvm_budget b;
	kvm_budget_init(&b, 2);
	CHECK_EQ(kvm_budget_enc_floor_ms(&b, 0), 0);
	CHECK_EQ(kvm_budget_enc_floor_ms(&b, 8000), 12);			// 8 ms x 1.5 cores / 1 core
	CHECK_EQ(kvm_budget_enc_floor_ms(&b, 25000), 38);			// 37.5, rounded up
	CHECK_EQ(kvm_budget_enc_floor_ms(&b, 40000), 60);
	kvm_budget_init(&b, 4);
	CHECK_EQ(kvm_budget_enc_floor_ms(&b, 25000), 19);			// 37.5 / 2 cores
	CHECK_EQ(kvm_budget_enc_floor_ms(&b, 40000), 30);
	kvm_budget_init(&b, 1);									// one processor: the encoder cannot keep 1.5 cores busy, and may take half of the machine: twice the encode time
	CHECK_EQ(kvm_budget_enc_floor_ms(&b, 25000), 50);
	kvm_budget_init(&b, 64);
	CHECK_EQ(kvm_budget_enc_floor_ms(&b, 25000), 2);
	kvm_budget_init(&b, 2);
	CHECK_EQ(kvm_budget_enc_floor_ms(&b, 0xFFFFFFFFu) > 1000, 1);	// no overflow
}

static void test_interval(void)
{
	kvm_budget b;
	kvm_budget_init(&b, 2);
	CHECK_EQ(kvm_pace_interval(NULL, 33, 25000), 33);
	CHECK_EQ(kvm_pace_interval(&b, 33, 0), 33);
	CHECK_EQ(kvm_pace_interval(&b, 33, 8000), 33);				// the viewer's period is longer than the floor (12)
	CHECK_EQ(kvm_pace_interval(&b, 16, 8000), 16);
	CHECK_EQ(kvm_pace_interval(&b, 16, 25000), 38);				// the floor
	CHECK_EQ(kvm_pace_interval(&b, 100, 25000), 100);			// Slow stays slow
	CHECK_EQ(kvm_pace_interval(&b, 16, 400000), KVM_PACE_INTERVAL_MAX_MS);	// never beyond the maximum ...
	CHECK_EQ(kvm_pace_interval(&b, 1000, 400000), 1000);			// ... but a period of the viewer above it is kept
}

// `n` encode calls of `wallUs` each, in which the process used `cpuUs` of CPU each, then the time passes to `t`
static void encode_calls(kvm_budget *b, int n, uint32_t wallUs, uint64_t cpuUs)
{
	int i;
	for (i = 0; i < n; i++) { kvm_budget_note_encode(b, wallUs, cpuUs); }
}

// the wall time of the frames and the CPU of the window are noted apart (the encoder runs beside the loop): the same estimate as when they come together
static void test_budget_split_notes(void)
{
	kvm_budget a, b;
	uint32_t t = 1000;
	int i;
	kvm_budget_init(&a, 4); kvm_budget_init(&b, 4);
	CHECK_EQ(kvm_budget_window(&a, t), 0); CHECK_EQ(kvm_budget_window(&b, t), 0);
	for (i = 0; i < 10; i++) { kvm_budget_note_encode(&a, 20000, 30000); kvm_budget_note_encode(&b, 20000, 0); }
	kvm_budget_note_cpu(&b, 300000);
	t += 500;
	CHECK_EQ(kvm_budget_window(&a, t), 1); CHECK_EQ(kvm_budget_window(&b, t), 1);
	CHECK_EQ(b.parX10, a.parX10); CHECK_EQ(b.encMilli, a.encMilli);
	// no CPU noted (the clock cannot be read): the estimate stays where it was
	{ const uint32_t before = b.parX10; for (i = 0; i < 10; i++) { kvm_budget_note_encode(&b, 20000, 0); } t += 500; CHECK_EQ(kvm_budget_window(&b, t), 1); CHECK_EQ(b.parX10, before); }
}

static void test_budget_window(void)
{
	kvm_budget b;
	uint32_t t = 1000;
	kvm_budget_init(&b, 2);
	CHECK_EQ(b.parX10, KVM_PACE_ENC_PAR_X10);
	CHECK_EQ(kvm_budget_window(&b, t), 0);						// the first call opens a window
	encode_calls(&b, 10, 25000, 37500);							// 10 frames, 25 ms each, 1.5 cores busy
	t += 499;
	CHECK_EQ(kvm_budget_window(&b, t), 0);						// the window is not over
	t += 1;
	CHECK_EQ(kvm_budget_window(&b, t), 1);
	CHECK_EQ(b.windows, 1); CHECK_EQ(b.measured, 1);
	CHECK_EQ(b.parX10, 15);
	CHECK_EQ(b.encMilli, 750);									// 375 ms of CPU in 500 ms of wall time
	// the encoder is wider than assumed: 2 cores busy (capped by the cores of the machine); the estimate follows by a quarter per window
	{
		int i;
		uint32_t prev = b.parX10;
		for (i = 0; i < 12; i++)
		{
			encode_calls(&b, 10, 25000, 100000);
			t += 500; CHECK_EQ(kvm_budget_window(&b, t), 1);
			CHECK(b.parX10 >= prev && b.parX10 <= 20);
			prev = b.parX10;
		}
		CHECK_EQ(b.parX10, 20);
		CHECK_EQ(b.encMaxMilli, 2000);
	}
	// less than the cores of the machine allow
	kvm_budget_init(&b, 4); t = 0;
	(void)kvm_budget_window(&b, t);
	{
		int i;
		for (i = 0; i < 20; i++) { encode_calls(&b, 10, 25000, 200000); t += 500; (void)kvm_budget_window(&b, t); }	// 8 cores busy are not believed on a 4 core machine, nor above KVM_PACE_PAR_MAX_X10
		CHECK_EQ(b.parX10, 40);
	}
	// a one-thread encoder: not below 1.0 (the calling thread is busy)
	kvm_budget_init(&b, 8); t = 0;
	(void)kvm_budget_window(&b, t);
	{
		int i;
		for (i = 0; i < 30; i++) { encode_calls(&b, 10, 25000, 5000); t += 500; (void)kvm_budget_window(&b, t); }
		CHECK_EQ(b.parX10, 10);
	}
	// a clock that cannot be read (0 CPU), or a window with almost no encoding: the estimate stays
	kvm_budget_init(&b, 4); t = 0;
	(void)kvm_budget_window(&b, t);
	encode_calls(&b, 10, 25000, 0); t += 500; (void)kvm_budget_window(&b, t);
	CHECK_EQ(b.parX10, 15); CHECK_EQ(b.measured, 0);
	encode_calls(&b, 1, 25000, 90000); t += 500; (void)kvm_budget_window(&b, t);		// 25 ms of encoding in the window
	CHECK_EQ(b.parX10, 15); CHECK_EQ(b.measured, 0);
	// the time counter wraps
	kvm_budget_init(&b, 2); t = 0xFFFFFF00u;
	(void)kvm_budget_window(&b, t);
	encode_calls(&b, 10, 25000, 25000); t += 500;
	CHECK_EQ(kvm_budget_window(&b, t), 1);
	CHECK_EQ(b.encMilli, 500);
	// a huge CPU time does not overflow
	kvm_budget_init(&b, 2); t = 0;
	(void)kvm_budget_window(&b, t);
	kvm_budget_note_encode(&b, 100000, 0xFFFFFFFFFFFFFFFFull / 2u); t += 600;
	(void)kvm_budget_window(&b, t);
	CHECK(b.parX10 <= 20); CHECK_EQ(b.encMilli, 0xFFFFFFFFu);
	kvm_budget_clear_stats(&b);
	CHECK_EQ(b.windows, 0); CHECK_EQ(b.encMaxMilli, 0);
}

// the floor follows the measured parallelism
static void test_floor_follows_measurement(void)
{
	kvm_budget b;
	uint32_t t = 0;
	int i;
	kvm_budget_init(&b, 2);
	CHECK_EQ(kvm_pace_interval(&b, 16, 25000), 38);				// assumed 1.5 cores
	(void)kvm_budget_window(&b, t);
	for (i = 0; i < 20; i++) { encode_calls(&b, 10, 25000, 25000); t += 500; (void)kvm_budget_window(&b, t); }	// the encoder turns out to use one core only
	CHECK_EQ(b.parX10, 10);
	CHECK_EQ(kvm_pace_interval(&b, 16, 25000), 25);				// the encoder may run all the time (one core of budget)
	for (i = 0; i < 40; i++) { encode_calls(&b, 10, 25000, 50000); t += 500; (void)kvm_budget_window(&b, t); }	// ... or two
	CHECK_EQ(b.parX10, 20);
	CHECK_EQ(kvm_pace_interval(&b, 16, 25000), 50);				// the old rule: twice the encode time
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the simulation of the loop
// ---------------------------------------------------------------------------------------------------------------------------------------

// What is modelled (the numbers are those of the first video path smoke, see the report): content that changes on every iteration (the worst case, "dense"), the capture of the next frame is in
// flight so it is there when the iteration begins; an iteration costs PROC_MS of wall time on the loop thread (CRC of the tiles, classes, rectangles) and PROC_CPU_MS of CPU;
// the capture thread uses CAP_CPU_MS of CPU per iteration; a frame costs `enc` ms of wall time on the loop thread (the loop waits for the encoder) and enc x min(1.5, cores) of CPU
// (the encoder works on slices in parallel); the system wakes a sleeper 0 or 1 ms late (a fixed pseudo-random sequence).
#define PROC_MS 4
#define PROC_CPU_MS 4
#define CAP_CPU_MS 5				// the default: DXGI, 1440x832 (smoke of the first video path); the table at the end runs 12 (GDI, scaling, 4K) and 20 as well

typedef struct sim_result { double fps, encCores, cores, pctMachine, itersPerSec; uint32_t interval; } sim_result;

static uint32_t gRng = 12345;
static uint32_t wake_late(void) { gRng = gRng * 1103515245u + 12345u; return (gRng >> 16) & 1u; }

// the rule that the pace schedule replaces (kvm_vcore_frame_interval of the first rate control): the interval is the period, but at least the encode time / 50 % = twice the encode time, at most 250 ms
static uint32_t old_interval(uint32_t period, uint32_t encAvgUs)
{
	uint32_t fps = 1000u / period, twice, v, base;
	if (fps > 60u) { fps = 60u; }
	if (fps < 1u) { fps = 1u; }
	base = 1000u / fps;
	twice = (uint32_t)(((uint64_t)encAvgUs * 100u + 50u * 1000u - 1u) / (50u * 1000u));
	v = twice > base ? twice : base;
	if (v > 250u && v > base) { v = base > 250u ? base : 250u; }
	return v;
}

// subtractCapture: the encoder's CPU is measured as the process minus the capture thread (what kvm_video.cpp does now); 0: the process as a whole (the version of the first commit)
static sim_result simulate_ex(int newRule, uint32_t period, uint32_t encMs, uint32_t cores, uint32_t seconds, uint32_t capCpuMs, int subtractCapture)
{
	kvm_vctl c;
	kvm_tick tick;
	kvm_budget bud;
	sim_result r;
	uint32_t t = 100000, t0 = t, end = t + seconds * 1000u, encAvgUs = 0, frames = 0, iters = 0, interval = 0;
	uint64_t cpuUs = 0, encCpuUs = 0;
	double par = cores < 2u ? 1.0 : 1.5;
	kvm_vctl_init(&c, 1);
	kvm_tick_init(&tick);
	kvm_budget_init(&bud, cores);
	(void)kvm_budget_window(&bud, t);
	while (t < end)
	{
		uint32_t started = t, emitAt = t + PROC_MS, stop, sleep;
		int emit;
		iters++;
		cpuUs += (uint64_t)(PROC_CPU_MS + capCpuMs) * 1000u;
		if (newRule) { (void)kvm_budget_window(&bud, started); interval = kvm_pace_interval(&bud, period < 16u ? 16u : period, encAvgUs); }
		else { interval = old_interval(period, encAvgUs); }
		emit = kvm_vctl_pace_ok(&c, emitAt, interval);
		stop = emitAt;
		if (emit)
		{
			uint32_t us = encMs * 1000u;
			// what the process used while the encoder ran: the encoder (par cores) and the part of the capture of the next frame that falls into that time
			uint64_t during = (uint64_t)((double)us * par) + (subtractCapture ? 0u : (uint64_t)((double)(capCpuMs * 1000u) * (double)encMs / (double)(PROC_MS + encMs)));
			encAvgUs = encAvgUs == 0 ? us : (uint32_t)(((uint64_t)encAvgUs * 7u + us) / 8u);
			encCpuUs += (uint64_t)((double)us * par);
			cpuUs += (uint64_t)((double)us * par);
			kvm_budget_note_encode(&bud, us, during);
			stop = emitAt + encMs;
			frames++;
			if (newRule) { c.lastFrame = kvm_pace_slot(c.haveFrame, c.lastFrame, emitAt, interval, NULL); c.haveFrame = 1; }
			else { c.lastFrame = emitAt; c.haveFrame = 1; }
		}
		if (newRule)
		{
			uint32_t vw = c.haveFrame ? kvm_pace_wake_ms(1, c.lastFrame, stop, interval) : 0;
			sleep = kvm_tick_next(&tick, started, stop, period < 16u ? 16u : period);
			if (vw > 0 && vw < sleep) { sleep = vw; }	// as kvm.c: the loop wakes for the slot when it comes before its own next iteration, never sleeps longer than its period
		}
		else
		{
			uint32_t elapsed = stop - started;
			sleep = elapsed >= period ? 0 : period - elapsed;
		}
		t = stop + sleep + (sleep > 0 ? wake_late() : 0);
	}
	{
		double wall = (double)(t - t0) / 1000.0;
		r.fps = (double)frames / wall;
		r.encCores = ((double)encCpuUs / 1e6) / wall;
		r.cores = ((double)cpuUs / 1e6) / wall;
		r.pctMachine = 100.0 * r.cores / (double)cores;
		r.itersPerSec = (double)iters / wall;
		r.interval = interval;
	}
	return r;
}

static sim_result simulate(int newRule, uint32_t period, uint32_t encMs, uint32_t cores, uint32_t seconds) { return simulate_ex(newRule, period, encMs, cores, seconds, CAP_CPU_MS, 1); }

// the capture thread beside the encoder (GDI, scaling and 4K cost 12-20 ms of CPU per frame) must not be counted as the encoder's: on 2 cores the version that measured the whole
// process lost 5 to 24 % of the frames against the rule it replaced; with the capture thread taken out the frame rate is that of the budget at every cost of the capture
static void test_capture_cpu(void)
{
	static const uint32_t caps[] = { 5, 12, 20 };
	static const uint32_t periods[] = { 16, 33 };
	static const uint32_t encs[] = { 15, 25, 40 };
	size_t ci, pi, ei;
	printf("\nthe CPU of the capture thread beside the encoder, 2 cores (fps: before = the rule of the first rate control; whole process = the first version of the pace schedule, the CPU of the process in the encode call; capture out = the CPU of the capture thread taken out)\n");
	printf("capture CPU  period  enc | before | whole process | capture out |\n");
	for (ci = 0; ci < sizeof(caps) / sizeof(caps[0]); ci++)
	{
		for (pi = 0; pi < sizeof(periods) / sizeof(periods[0]); pi++)
		{
			for (ei = 0; ei < sizeof(encs) / sizeof(encs[0]); ei++)
			{
				sim_result o = simulate_ex(0, periods[pi], encs[ei], 2, 60, caps[ci], 1), w = simulate_ex(1, periods[pi], encs[ei], 2, 60, caps[ci], 0), f = simulate_ex(1, periods[pi], encs[ei], 2, 60, caps[ci], 1);
				printf("%6u ms   %4u  %4u | %6.1f | %6.1f (%+4.0f %%) | %6.1f (%+4.0f %%) |\n", (unsigned)caps[ci], (unsigned)periods[pi], (unsigned)encs[ei], o.fps, w.fps, 100.0 * (w.fps - o.fps) / o.fps, f.fps, 100.0 * (f.fps - o.fps) / o.fps);
				CHECK(f.fps >= o.fps * 0.93);											// with the capture taken out: never more than 7 % below the rule it replaces, at any cost of the capture
				CHECK(f.encCores <= 1.05);												// and the encoder is in its budget (1 core of 2)
			}
		}
	}
	printf("\n");
}

static void test_simulation(void)
{
	static const uint32_t periods[] = { 16, 33, 50, 100 };
	static const uint32_t encs[] = { 8, 15, 25, 40 };
	static const uint32_t cores[] = { 2, 4 };
	size_t ci, pi, ei;
	printf("\nsimulation of the main loop, dense content (every iteration has a new picture), 60 s per row\n");
	printf("period = what the viewer asks for (ms), enc = wall time of one encode call (ms), fps = frames written per second, enc = cores the encoder used, proc = cores the whole process used,\n");
	printf("%% = proc as a share of the machine, iv = the frame interval at the end (ms), bound = what the viewer, a synchronous loop and the budget allow\n");
	for (ci = 0; ci < sizeof(cores) / sizeof(cores[0]); ci++)
	{
		printf("\n%u cores (encoder budget %.1f cores = 50 %% of the machine, at least one core)\n", (unsigned)cores[ci], cores[ci] == 2 ? 1.0 : 2.0);
		printf("period  enc |  before: fps   enc  proc    %%   iv  |  after: fps   enc  proc    %%   iv  | bound fps | gain\n");
		for (pi = 0; pi < sizeof(periods) / sizeof(periods[0]); pi++)
		{
			for (ei = 0; ei < sizeof(encs) / sizeof(encs[0]); ei++)
			{
				sim_result o = simulate(0, periods[pi], encs[ei], cores[ci], 60), n = simulate(1, periods[pi], encs[ei], cores[ci], 60);
				kvm_budget b;
				double bound, viewer = 1000.0 / (double)(periods[pi] < 16 ? 16 : periods[pi]), loop = 1000.0 / (double)(encs[ei] + PROC_MS);
				uint32_t floorMs;
				kvm_budget_init(&b, cores[ci]);
				floorMs = kvm_budget_enc_floor_ms(&b, encs[ei] * 1000u);
				bound = 1000.0 / (double)(floorMs > periods[pi] ? floorMs : periods[pi]);
				if (loop < bound) { bound = loop; }
				if (viewer < bound) { bound = viewer; }
				printf("%4u  %4u | %9.1f %5.2f %5.2f %4.0f %4u  | %9.1f %5.2f %5.2f %4.0f %4u  | %9.1f | %+5.0f %%\n", (unsigned)periods[pi], (unsigned)encs[ei], o.fps, o.encCores, o.cores, o.pctMachine, (unsigned)o.interval,
					   n.fps, n.encCores, n.cores, n.pctMachine, (unsigned)n.interval, bound, o.fps > 0.0 ? 100.0 * (n.fps - o.fps) / o.fps : 0.0);
				// the properties the table stands for
				CHECK(n.fps >= o.fps * 0.93);											// not fewer frames than the rule it replaces (5 % less at most, where the encoder budget of one core binds both rules: 2 cores, 40 ms)
				CHECK(n.fps <= viewer * 1.02);											// never more than the viewer asked for
				CHECK(n.fps <= loop * 1.02);											// ... nor more than a synchronous loop can encode
				CHECK(n.fps >= 0.85 * bound - 0.6);										// and it gets close to what the viewer, the loop and the budget allow
				CHECK(n.encCores <= (double)b.budgetMilli / 1000.0 * 1.05);			// the encoder stays in its budget 
				CHECK(o.encCores <= (double)b.budgetMilli / 1000.0 * 1.05 || o.fps > 0.0);
			}
		}
	}
	printf("\n");
}

// a loop of 16 ms that overruns its slot now and then (an encode that took 30 ms: the CPU was taken), as kvm.c runs it: the next iteration starts at once after an overrun, and wakes for the slot of the
// next frame when that comes before the next iteration of the schedule. Before: the slot only; after: the write of a frame also has to be 75 % of the interval behind the write of the last one.
static void test_write_gap_burst(void)
{
	int variant;
	for (variant = 0; variant < 2; variant++)
	{
		kvm_vctl c;
		uint32_t t = 100000, end = t + 60000, interval = 16, lastWriteAt = 0, frames = 0, rng = 12345u, gapSum = 0;
		uint32_t pairs = 0, gaps = 0, over = 0, encAvg = 8000u;	// microseconds, as kvm_video.cpp keeps it (a moving average of 1/8)
		static uint32_t pairsBefore, framesBefore;
		int haveLast = 0;
		kvm_vctl_init(&c, 1);
		while (t < end)
		{
			const uint32_t started = t;
			uint32_t next, wake, stop = t;
			rng = rng * 1664525u + 1013904223u;
			if (kvm_vctl_pace_ok(&c, t, interval) != 0)
			{
				const uint32_t encMs = ((rng >> 20) % 100u) < 8u ? 30u : 8u;	// 8 % of the frames take 30 ms
				const uint32_t wr = t + encMs;
				const uint32_t prevSlot = c.lastFrame;
				const int had = c.haveFrame;
				c.haveFrame = 1; c.lastFrame = kvm_pace_slot(had, prevSlot, t, interval, NULL);
				encAvg = (uint32_t)(((uint64_t)encAvg * 7u + (uint64_t)encMs * 1000u) / 8u);
				if (variant == 1) { kvm_vctl_note_write(&c, wr, (encAvg + 500u) / 1000u); }
				if (haveLast) { const uint32_t g = wr - lastWriteAt; gaps++; gapSum += g; if (g * 2u <= interval) { pairs++; } if (g > 2u * interval) { over++; } }
				haveLast = 1; lastWriteAt = wr; frames++;
				stop = wr;
			}
			next = started + 16u;
			if (stop >= next) { next = stop; }									// an overrun: the next iteration starts at once
			wake = c.haveFrame ? kvm_pace_wake_ms(1, c.lastFrame, stop, interval) : 0u;
			if (variant == 1) { const uint32_t w2 = kvm_vctl_write_wait_ms(&c, stop, interval); if (w2 > wake) { wake = w2; } }
			if (wake > 0 && stop + wake < next) { next = stop + wake; }
			t = next > t ? next : t + 1u;
		}
		printf("write gap %s: %u frames in 60 s (%.1f a second), %u of %u writes at most half an interval behind the previous one (%.1f %%), %u gaps above twice the interval, mean gap %.1f ms\n", variant == 0 ? "before (the slot only)" : "after (write gap)", frames, frames / 60.0, pairs, gaps, 100.0 * pairs / gaps, over, (double)gapSum / gaps);
		if (variant == 0) { CHECK(pairs >= 100); pairsBefore = pairs; framesBefore = frames; }
		else { CHECK(pairs * 10u <= pairsBefore); CHECK(frames * 100u >= framesBefore * 97u); }	// the pairs are gone (a few from the error of the estimate of the encode time), and the frames that are given up for it are 3 % at most
	}
}

int main(void)
{
	test_tick_basic();
	test_tick_no_drift();
	test_tick_overrun();
	test_tick_idle_and_early();
	test_tick_wrap();
	test_slots();
	test_budget_init();
	test_enc_floor();
	test_interval();
	test_budget_window();
	test_budget_split_notes();
	test_floor_follows_measurement();
	test_simulation();
	test_capture_cpu();
	test_write_gap_burst();
	printf("%d checks, %d failed\n", gChecks, gFail);
	return gFail != 0;
}
