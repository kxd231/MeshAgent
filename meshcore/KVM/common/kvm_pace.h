/*
KVM: the pace of the main loop and the CPU budget of the video. Portable C99, no operating system API: every time is a 32 bit millisecond counter (wraps), compared with
a signed difference; the CPU time is a counter of microseconds that the caller reads from the system. Tested in Docker (tests/kvm_pace_test.c).

Three small pieces:

  tick     the schedule of the loop. The loop does not sleep "the period minus what the iteration took" (that drifts by the lateness of every wake: 1 ms late on a 16 ms period is 6 % fewer
           frames) but until the next slot: slot(n+1) = slot(n) + period. An iteration that overran its slot makes the next one start at once and the debt is paid back within the next period;
           a debt of a whole period or more is forgotten (no burst of catch-up frames) and an idle loop has no schedule at all (no credit for the time nothing happened).
  slot     the same for the video frames: a frame belongs to a slot of its own (the previous slot + the frame interval), not to the moment it was written, so the interval between two
           frames is the interval and not "interval + the time it took to get here". kvm_pace_wake_ms() says how long until the next slot, so that the loop can wake for it (never sleeping longer than its own period), which removes the aliasing of
           a loop that wakes up on a grid of its own (a frame every second iteration when the interval is a little more than one period).
  budget   the CPU the encoder may use: at most KVM_PACE_CPU_SHARE_PCT of the machine ("50 %, at least one core"; 50 % is one core from two processors up, and on one processor the
           whole machine would be that core, so there the encoder stays at half of it). The Media Foundation encoder works on slices in parallel,
           so for `e` ms of wall time per frame it uses `e x p` ms of CPU, where p (cores busy while it runs) is measured: the CPU time of the process during the encode calls over their
           wall time, a moving average over windows of KVM_PACE_WINDOW_MS, KVM_PACE_ENC_PAR_X10 / 10 until there is a measurement. The frame interval is never shorter than e x p / budget:
           one core of budget and p = 1 let the encoder run all the time, p = 1.5 on two cores makes 67 % of the loop. The bandwidth regulator (kvm_video_rc.c) is separate and unchanged: the
           link limits the frame rate on its own account. The CPU of the whole process is only reported (the log), it is not regulated: the capture and the JPEG have their own limits.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef KVM_PACE_H
#define KVM_PACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// (T) = tunable with -D on the compiler command line.
#ifndef KVM_PACE_EARLY_MS
#define KVM_PACE_EARLY_MS 2			// (T) a wake this much before the slot still counts as that slot (the timer of the system is not exact)
#endif
#ifndef KVM_PACE_LATE_MS
#define KVM_PACE_LATE_MS 2			// (T) an iteration or a frame that starts this much after its slot is "late" in the statistics
#endif
#ifndef KVM_PACE_INTERVAL_MAX_MS
#define KVM_PACE_INTERVAL_MAX_MS 250	// (T) the interval that follows the budget never grows beyond this (a viewer period above it is kept as it is)
#endif
#ifndef KVM_PACE_CPU_SHARE_PCT
#define KVM_PACE_CPU_SHARE_PCT 50		// (T) the encoder may use this share of the machine : from two processors up that is at least one core, on one processor half of it
#endif
#ifndef KVM_PACE_ENC_PAR_X10
#define KVM_PACE_ENC_PAR_X10 15			// (T) cores that the encoder keeps busy while it works, x 10, until it is measured (1.5 on 2 vCPU)
#endif
#ifndef KVM_PACE_WINDOW_MS
#define KVM_PACE_WINDOW_MS 500			// (T) the window in which the encoder's parallelism is measured
#endif
#ifndef KVM_PACE_PAR_MAX_X10
#define KVM_PACE_PAR_MAX_X10 80			// (T) the measured parallelism is not believed above this (x 10): the CPU of the other threads of the process is in the number too
#endif
#ifndef KVM_PACE_MIN_ENC_MS
#define KVM_PACE_MIN_ENC_MS 40			// (T) a window with less encoding than this (wall time) says nothing about the parallelism
#endif

// ---------------------------------------------------------------------------------------------------------------------------------------
// lateness statistics (of the ticks and of the slots)
// ---------------------------------------------------------------------------------------------------------------------------------------

typedef struct kvm_late
{
	uint32_t n;				// iterations (frames) that had a slot
	uint32_t late;			// ... of which started KVM_PACE_LATE_MS or more after it
	uint32_t sumMs;			// the sum of the lateness of the late ones
	uint32_t maxMs;			// the largest lateness
	uint32_t over;			// iterations that ended after the slot of the next one (the work was longer than the period)
	uint32_t resync;		// ... and of those, the ones that were a whole period late: the schedule began again
} kvm_late;

void kvm_late_clear(kvm_late *l);

// ---------------------------------------------------------------------------------------------------------------------------------------
// tick: the schedule of the loop
// ---------------------------------------------------------------------------------------------------------------------------------------

typedef struct kvm_tick
{
	uint8_t have;			// 1: `due` is the slot at which the next iteration is to start
	uint32_t due;
	kvm_late st;
} kvm_tick;

void kvm_tick_init(kvm_tick *t);
// Nothing was sent, or the capture waited for changes: there is no schedule, the next iteration that works starts one.
void kvm_tick_idle(kvm_tick *t);
// The end of an iteration that started at `started` and ends at `now`: the number of milliseconds to sleep until the next slot (0 = start at once). An iteration that started before its slot
// (woken early for another reason) is not an iteration of the schedule and does not use up a slot. periodMs 0: no schedule, 0.
uint32_t kvm_tick_next(kvm_tick *t, uint32_t started, uint32_t now, uint32_t periodMs);

// ---------------------------------------------------------------------------------------------------------------------------------------
// the slots of the video frames
// ---------------------------------------------------------------------------------------------------------------------------------------

// The slot of the frame that goes at `now`: the previous slot + the interval, or `now` when there was no frame yet or the frame is a whole interval late (no catch-up burst). A frame that goes
// early (the system timer) keeps the slot, so the average rate holds the interval. `l` (may be NULL) gets the lateness of the frame.
uint32_t kvm_pace_slot(int haveLast, uint32_t lastSlot, uint32_t now, uint32_t intervalMs, kvm_late *l);
// Milliseconds until the next slot (0 = it has come, or there is no previous frame).
uint32_t kvm_pace_wake_ms(int haveLast, uint32_t lastSlot, uint32_t now, uint32_t intervalMs);

// ---------------------------------------------------------------------------------------------------------------------------------------
// the CPU budget
// ---------------------------------------------------------------------------------------------------------------------------------------

typedef struct kvm_budget
{
	uint32_t cores;			// logical processors of the machine (at least 1)
	uint32_t budgetMilli;	// the CPU the encoder may use, in 1/1000 of a core: cores x 1000 x KVM_PACE_CPU_SHARE_PCT / 100 (two processors: one core, one processor: half a core)
	uint32_t parX10;		// cores that the encoder keeps busy while it works, x 10 (the estimate)
	uint8_t haveWin;
	uint32_t winStart;		// the window that is open: its start and what the encode calls have used since
	uint64_t winWallUs, winCpuUs;
	// the last closed window and the extremes since kvm_budget_clear_stats()
	uint32_t encMilli, encMaxMilli;	// CPU of the encoder in the window as 1/1000 of a core (over the wall time of the window, not of the encoding)
	uint32_t windows, measured;
} kvm_budget;

void kvm_budget_init(kvm_budget *b, uint32_t cores);
void kvm_budget_clear_stats(kvm_budget *b);
// One encode call took `wallUs` of wall time and the process used `cpuUs` of CPU time meanwhile (the difference of the process CPU counter before and after; 0 when it cannot be read).
void kvm_budget_note_encode(kvm_budget *b, uint32_t wallUs, uint64_t cpuUs);
// the encoder runs beside the main loop, so the CPU of one call cannot be told from the process clock; the loop adds, once an iteration, what the process used less its own threads (the main loop and
// the capture thread) since the last call: that is what the encoder and its worker threads used. The wall time of the frames comes from kvm_budget_note_encode(b, wallUs, 0).
void kvm_budget_note_cpu(kvm_budget *b, uint64_t cpuUs);
// Called every iteration with the time. Closes the window every KVM_PACE_WINDOW_MS and updates the estimate of the parallelism. Returns 1 when a window was closed.
int kvm_budget_window(kvm_budget *b, uint32_t now);
// The shortest frame interval at which the encoder, taking `encAvgUs` of wall time per frame, stays in the budget: encAvgUs x parallelism / budget (the parallelism is at most the cores).
uint32_t kvm_budget_enc_floor_ms(const kvm_budget *b, uint32_t encAvgUs);
// The frame interval: the period the viewer asked for, but not below the floor of the encoder; above the viewer's period at most KVM_PACE_INTERVAL_MAX_MS (a period that is longer is kept).
// `b` may be NULL (only the period).
uint32_t kvm_pace_interval(const kvm_budget *b, uint32_t periodMs, uint32_t encAvgUs);

#ifdef __cplusplus
}
#endif

#endif
