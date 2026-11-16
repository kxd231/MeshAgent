/*
KVM: the Auto mode of the video frame rate. Portable C99, no operating system API: time is a 32 bit millisecond counter (wraps, compared with a signed difference), nothing is
measured here, the glue (Windows/kvm_video.cpp) notes what happened and asks once an iteration. Tested in Docker (tests/kvm_auto_test.c, the closed loop in tests/kvm_auto_sim_test.c).

What it is. The viewer's list of frame rates has a fifth entry, Auto (bit 3 of the flags of op 1). Auto has ONE actuator, the target frame interval of the video: 60 / 30 / 20 / 15 / 10 frames
a second (16 / 33 / 50 / 66 / 100 ms), the same interval that command 5 sets in the fixed modes. It picks the highest rate that the machine and the viewer keep up evenly while the mean QP of the
motion stays under the ceiling of that step. It changes nothing else: the QP regulator, the window of the viewer, the byte bucket, the sharpening of the JPEG tiles (the text at rest), the
refresh and the JPEG quality are the loops that were there before and they stay as they are; Auto only reads what they produce. Fewer frames at the same R are more bytes per frame, so the QP
regulator lowers the QP by itself: Auto steers the quality indirectly and does not fight the regulator for one parameter. A change of the step is not a new generation of the video (the media type
has a fixed 30 frames a second, the level is computed for 60), so it costs no key frame.

Rules, for a window of one second that holds at least 300 ms of motion (a window without motion counts neither way: a silent screen is not a slow link):
  down (CPU)   two evaluated windows in a row in which the machine does not keep up (below): to the first step whose interval holds the cost (at least the P75 of the capture, the interval that the
               CPU budget of the encoder needs, what was really achieved), possibly several steps at once.
  down (link)  two evaluated windows in a row with the QP of the regulator (at the end of the window) above the ceiling of the step: one step. The design asks for "AND at least 20 % of the due
               frames held back"; the simulation shows why that cannot stand: the QP regulator raises the QP until the frames are no longer refused, so a QP above the ceiling and refusals do not
               coexist in a steady state (at 3 Mb/s the QP sits at 34 with nothing refused for a minute), and a fall of the link is seen after 5 s. Instead the refusals count as a prediction: the QP
               that the regulator is about to ask for (the frames that are due and refused would need that many more bytes, 11 % per QP step) is added to the QP of the moment (kvm_auto_pred).
  up           `upNeed` (4, then 8, 16, 32, 64, 120 after a failed attempt, back to 4 when a step up has held for 60 s; after a step down because of the link at least 64, so the next attempt
               is no sooner than 64 evaluated windows and a failed one doubles from there to 120: the patience of 4 and 8 windows made 5 changes in 23 s on a link that queues at the faster step;
               the steps down for the CPU and the capture keep the patience they had) evaluated windows in a row in which the QP of the motion is at most the ceiling of the
               faster step less 1 when the regulator is predicted to ask for that QP at the faster rate (its floor and the use of R count), under 5 % of the frames held back, under 100 ms in flight
               at the link, the machine does not limit, the frames of the current step are made (85 %), and the cost (capture, encode, the CPU budget) fits the faster step by the same rule as staying
               (4/3 of its interval, counted from 1000 / frames a second): one step. The attempt is judged by what it achieves: the achieved rate at the faster step. No step within 2 s of the previous
               one, the first improvement not before 4 s.
  start        60 when the first minimum round trip is under 20 ms, else 30 (3 s without a round trip: 30).
  The machine "does not keep up" (extended): the achieved rate under 75 % of the target with nothing held back by the window or the bucket and tiles waiting;
  or the frame interval that the CPU budget of the encoder needs is above the target; or the P75 of the capture time, or the median of the gap between two images, is above 4/3 of the interval. The
  last three do not count when the DXGI frames of the desktop manager say that the loop takes every image the source makes (dwm_per_image < 1.1): a slow source is not a slow machine.
  A "nonuniform 45..55 frames a second" on a slow laptop is therefore not a reason to leave 60 (a design decision of 2026-11-14): below 45 it is.
  Ceilings from outside (the screen of the viewer, the battery saver: a later stage, not implemented here) are an input (`maxFps`), they apply at once and also in a window without motion.

Auto never touches what the text at rest needs: the sharpening to JPEG 4:4:4, REFRESH and the JPEG quality are not parameters of this module. With the bit down kvm_auto does nothing at all:
kvm_auto_interval_ms() is 0, the notes are ignored, the tick does not decide.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef KVM_AUTO_H
#define KVM_AUTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KVM_AUTO_OP1_FLAG 8				// bit 3 of the flags of op 1 (240): the viewer chose Auto (the parser keeps it in kvm_ctrl.flags since the first video path, nothing else looks at it)
#define KVM_CAPS_AUTO 4					// bit 2 of the caps of 242: this agent knows Auto (bit 0 H.264, bit 1 ACK)

#define KVM_AUTO_STEPS 5
#define KVM_AUTO_CLASS_UI 0				// scrolling, dragging, animations (always)
#define KVM_AUTO_CLASS_VIDEO 1			// motion over a quarter of the screen for 3 s (kvm_motion)

// the reason of a step (the same numbers as the 244 of a later stage): 0 none
#define KVM_AUTO_R_NONE 0
#define KVM_AUTO_R_START 1				// the first step, from the round trip
#define KVM_AUTO_R_LINK 2				// the QP is above the ceiling and the window / the bucket hold frames back
#define KVM_AUTO_R_CPU 3				// the encoder does not keep up (the CPU budget, the encode time, the achieved rate)
#define KVM_AUTO_R_CAPTURE 4			// the capture, or the loop, does not keep up
#define KVM_AUTO_R_SCREEN 5				// the screen of the viewer 
#define KVM_AUTO_R_POWER 6				// the battery saver 
#define KVM_AUTO_R_BUDGET 7				// the budget of the viewer 
#define KVM_AUTO_R_BACK 8				// a step up: the conditions are good again

// bits of kvm_auto_dec.flags
#define KVM_AUTO_F_LIMITED 1			// the machine does not keep up in this window
#define KVM_AUTO_F_QPBAD 2				// the QP is above the ceiling and frames are held back
#define KVM_AUTO_F_GOOD 4				// the conditions for the faster step are met
#define KVM_AUTO_F_FLOOR 8				// the interval of the CPU budget is above the target
#define KVM_AUTO_F_CAPTURE 16			// the capture or the loop is slower than the target allows
#define KVM_AUTO_F_ACHIEVED 32			// the achieved rate is under 75 % of the target

// (T) = tunable with -D on the compiler command line.
#ifndef KVM_AUTO_WINDOW_MS
#define KVM_AUTO_WINDOW_MS 1000			// (T) the length of an evaluated window
#endif
#ifndef KVM_AUTO_MIN_MOTION_MS
#define KVM_AUTO_MIN_MOTION_MS 300		// (T) a window with less motion than this is not evaluated (frozen)
#endif
#ifndef KVM_AUTO_MOTION_GAP_MS
#define KVM_AUTO_MOTION_GAP_MS 500		// the time of motion is the span of the series: the gaps between two images (or frames) of motion up to this long, and one step (the interval of the target) for the first one and after a longer gap. 500 ms and not the viewer's 250: the loop can run less often than every 250 ms (the pause of WA-2 x 4 at 15 frames a second, a blocked write), and the images with tiles waiting are motion that goes on, not a pause between series
#endif
#ifndef KVM_AUTO_DOWN_WINDOWS
#define KVM_AUTO_DOWN_WINDOWS 2			// (T) evaluated windows in a row for a step down
#endif
#ifndef KVM_AUTO_UP_WINDOWS
#define KVM_AUTO_UP_WINDOWS 4			// (T) the first patience: evaluated windows in a row for a step up
#endif
#ifndef KVM_AUTO_UP_MAX
#define KVM_AUTO_UP_MAX 120				// (T) the patience grows 4, 8, 16, 32, 64, 120 after a failed attempt
#endif
#ifndef KVM_AUTO_LINK_WAIT
#define KVM_AUTO_LINK_WAIT 64			// (T) a step down because of the link is not tried again sooner than this many evaluated windows: a link that queued at that step queues again at once; a failed attempt doubles from here, to KVM_AUTO_UP_MAX
#endif
#ifndef KVM_AUTO_FAIL_MS
#define KVM_AUTO_FAIL_MS 10000			// (T) a step down this soon after a step up is a failed attempt
#endif
#ifndef KVM_AUTO_STABLE_MS
#define KVM_AUTO_STABLE_MS 60000		// (T) a step up that holds this long (the faster step is still the step) is a success: the patience is the first one again
#endif
#ifndef KVM_AUTO_MIN_CHANGE_MS
#define KVM_AUTO_MIN_CHANGE_MS 2000		// (T) no step within this long of the previous one (a ceiling from outside is exempt)
#endif
#ifndef KVM_AUTO_UP_AFTER_MS
#define KVM_AUTO_UP_AFTER_MS 4000		// (T) the first improvement not before this long after the previous step
#endif
#ifndef KVM_AUTO_STALE_MS
#define KVM_AUTO_STALE_MS 30000			// (T) evaluated windows this far apart do not make a streak (the link may have changed in the pause)
#endif
#ifndef KVM_AUTO_START_RTT_MS
#define KVM_AUTO_START_RTT_MS 20		// (T) a minimum round trip below this starts at 60, else 30
#endif
#ifndef KVM_AUTO_START_WAIT_MS
#define KVM_AUTO_START_WAIT_MS 3000		// (T) without a round trip for this long the start is 30
#endif
#ifndef KVM_AUTO_HELD_UP_PCT
#define KVM_AUTO_HELD_UP_PCT 5			// (T) under this a step up is allowed
#endif
#ifndef KVM_AUTO_UP_QP_HEADROOM
#define KVM_AUTO_UP_QP_HEADROOM 1		// (T) the QP that the regulator is predicted to ask for at the faster step is at most its ceiling less this (kvm_auto_up_qp10)
#endif
#ifndef KVM_AUTO_QP_FLOOR
#define KVM_AUTO_QP_FLOOR 26			// the floor of the regulator when the glue does not say (kvm_vrc: KVM_VIDEO_QP_MIN; the hardware encoder's is 30)
#endif
#ifndef KVM_AUTO_KEEP_PCT
#define KVM_AUTO_KEEP_PCT 75			// (T) achieved / target below this, or a cost above interval / this: the machine does not keep up. 75 % of 60 is the 45 a second that is accepted as uneven
#endif
#ifndef KVM_AUTO_SAT_PCT
#define KVM_AUTO_SAT_PCT 85				// (T) a step up only when the frames of the current step are made at this share: nobody has anything to do with more frames of a source that does not make them
#endif
#ifndef KVM_AUTO_DWM_SLOW_X100
#define KVM_AUTO_DWM_SLOW_X100 110		// (T) dwm_per_image below this x 100: the loop takes every image of the source (DXGI), the source is the limit
#endif
#ifndef KVM_AUTO_GAP_MAX_MS
#define KVM_AUTO_GAP_MAX_MS 500			// a gap between two images longer than this is a pause between series, not an iteration
#endif

// the QP that the regulator is about to ask for, in tenths: its QP now plus the rise that the share of the due frames that were refused (heldPct, 0..100) needs (the frames would be 1 / (1 - share) times bigger
// than R lets through; 11 % per QP step). Exported for the log and the tests.
uint32_t kvm_auto_pred_qp10(uint32_t qpNow10, uint32_t heldPct);
// The QP, in tenths, that the regulator is predicted to ask for after a step up from `step` to the next faster one: the stream of the window (`bytes` over `motionMs`) at the faster rate against R
// (`rateBps`; unknown: the stream is taken to fill R), 11 % of bytes per QP step, not below the floor `qpFloor` (the regulator does not go lower: the hardware encoder's floor is 30, so a fixed
// "QP at most 26" could never hold there). A step up is allowed while this is at most the ceiling of the faster step less KVM_AUTO_UP_QP_HEADROOM.
uint32_t kvm_auto_up_qp10(int step, uint32_t qpNow10, uint32_t bytes, uint32_t motionMs, uint32_t rateBps, uint32_t qpFloor);

#ifndef KVM_AUTO_FUTURE_MS
#define KVM_AUTO_FUTURE_MS 60000		// a time stamp that is this far ahead of the clock is not a late note but a clock that has gone round its 32 bits
#endif
#ifndef KVM_AUTO_UP_QUEUE_MS
#define KVM_AUTO_UP_QUEUE_MS 100		// (T) no step up while this much is in flight at the link (the rate control probes up to twice the rate it has seen: the queue shows it before the window refuses)
#endif
#ifndef KVM_AUTO_KEY_FREEZE_MS
#define KVM_AUTO_KEY_FREEZE_MS 1000		// (T) a window that holds a key frame, or ends within this long (plus the time the key frame needs on the link) after one, is not evaluated: a key frame is 10 - 50 times a P frame, it fills the window of the viewer and the bucket for a while, and what it refuses is no sign of the link
#endif

// the steps: 0 = 60 frames a second ... 4 = 10
int kvm_auto_step_fps(int step);			// 60, 30, 20, 15, 10
uint32_t kvm_auto_step_ms(int step);		// 16, 33, 50, 66, 100
int kvm_auto_qp_ceiling(int cls, int step);	// the ceiling of the mean QP of the motion for the step
const char *kvm_auto_reason_name(int reason);

// What the accumulators close into once a second (and what a test hands to kvm_auto_decide).
typedef struct kvm_auto_win
{
	uint32_t windowMs;			// the length of the window
	uint32_t motionMs;			// of it with motion: the span of the series of images and frames (the gaps of at most KVM_AUTO_MOTION_GAP_MS, one step for the first of a series), not slots of 100 ms (slots made 5 frames of 33 ms 200 ms and the rate 25)
	uint32_t made;				// P frames written (key frames are not counted)
	uint32_t heldWin, heldRate;	// refusals of the window of the viewer / of the byte bucket, counted when a frame was due (the pace allowed it)
	uint32_t pendingIters;		// images with tiles waiting for the video
	uint32_t qpN;				// P frames that carry a QP
	uint32_t qpSum;				// ... and the sum of their QP
	uint32_t qpEnd;				// the QP of the last one (used when the regulator's QP is not known: kvm_auto_env.qpNow)
	uint32_t bytes;				// the bytes of those P frames (with the rate of the link: how much of R the stream uses)
	uint32_t capP75Ms;			// the P75 of the capture time of the images (0 = none)
	uint32_t gapMedMs;			// the median of the gap between two images with motion (0 = none)
	uint32_t dwmX100;			// the frames of the desktop manager per image, x 100 (DXGI; 0 = not known)
	uint8_t key;				// a key frame was written in the window, or shortly before it ended: the window is not evaluated
} kvm_auto_win;

// What is not in the window.
typedef struct kvm_auto_env
{
	uint32_t now;
	int valid;					// the video runs and no new generation is under way: otherwise the window is not evaluated
	int cls;					// KVM_AUTO_CLASS_*
	uint32_t minRttMs;			// the minimum round trip of the link, 0 = not measured yet
	uint32_t queueMs;			// the data in flight at the link, as milliseconds of its rate, less the round trip that a link that is not full holds in flight anyway (inflight / rate - minRtt, not below 0), 0 = not known: no step up into a link that is full (a proper estimator is future work)
	uint32_t qpFloor;			// the lowest QP the regulator goes to (kvm_vrc.qpMin: 26 for the software encoder, 30 for the hardware one), 0 = KVM_AUTO_QP_FLOOR
	uint32_t rateBps;			// R of the rate control, bytes a second (kvm_vrc.rate), 0 = not known
	uint32_t qpNow;				// the QP that the regulator asks for now (kvm_vrc.qp), 0 = not known (then the QP of the last frame of the window)
	uint32_t encAvgUs;			// the average encode time of a frame (wall), 0 = unknown
	uint32_t parX10;			// the cores the encoder keeps busy while it works, x 10 (kvm_budget)
	uint32_t budgetMilli;		// the CPU budget of the encoder, 1/1000 of a core (kvm_budget), 0 = unknown
	uint32_t floorMs;			// the interval that the encoder needs to stay in that budget (kvm_budget_enc_floor_ms), 0 = unknown
	uint32_t maxFps;			// a ceiling from outside (the screen of the viewer, the battery saver, the budget of the viewer), 0 = none
	int maxReason;				// KVM_AUTO_R_* of that ceiling
} kvm_auto_env;

// What a call decided.
typedef struct kvm_auto_dec
{
	uint8_t changed;			// the step is another one now
	uint8_t from, to;			// the steps (to = the step in force)
	uint8_t reason;				// KVM_AUTO_R_*
	uint8_t evaluated;			// the window counted
	uint8_t flags;				// KVM_AUTO_F_*
	uint32_t intervalMs;		// the interval in force
	uint32_t needMs;			// the interval that the machine needs (for the log; 0 = none)
	uint32_t achieved10;		// frames a second x 10 achieved during the motion
	uint32_t heldPct;
} kvm_auto_dec;

typedef struct kvm_auto
{
	uint8_t on;					// Auto is chosen (bit 3 of op 1)
	uint8_t started;			// the first step is made
	uint8_t step;				// 0..4 (before the start: 1, the interval of Medium, which is what command 5 asks for)
	uint8_t lastReason;
	uint8_t cpuStreak, qpStreak, goodStreak;
	uint32_t upNeed;			// the patience
	uint32_t beginAt, lastChangeAt, lastUpAt, lastEvalAt;
	uint8_t haveChange, haveUp, haveEval, upStep;	// upStep: the step that the last step up went to
	uint32_t changes, downs, ups;	// statistics since the bit went up
	// the accumulators of the window that is open
	uint32_t winStart; uint8_t haveWin;
	uint32_t motionAcc, lastMotionAt; uint8_t haveMotion;	// the time of motion of the window, and the time of the last image or frame of motion (it stays across the windows: a series goes on)
	uint32_t made, heldWin, heldRate, pendingIters, qpN, qpSum, qpEnd, bytes;
	uint32_t dwmImages, dwmFrames;
	uint32_t keyUntil; uint8_t haveKey, keyInWin;	// a key frame went out: the windows until keyUntil are frozen
	uint32_t lastImageAt; uint8_t haveImage;
	uint32_t capMs, capDwm; uint8_t haveCap;	// the capture of the image that is about to be noted
	uint16_t capHist[256], gapHist[256];
	uint32_t capN, gapN;
} kvm_auto;

void kvm_auto_init(kvm_auto *a);
// The bit of op 1: on = Auto chosen. Going up starts from nothing (the step of Medium until the start); going down leaves nothing behind.
void kvm_auto_set(kvm_auto *a, int on, uint32_t now);
int kvm_auto_on(const kvm_auto *a);
// The video starts again after it was off (op 2, a veto, JPEG for a while): what the old window and the series saw is gone, and after more than KVM_AUTO_STALE_MS since the last evaluated window the step
// is not remembered either: the start is made again from the round trip (the link may be another one, and the step may have been judged by another viewer's Auto meanwhile).
void kvm_auto_resume(kvm_auto *a, uint32_t now);
// The interval in force, 0 when the bit is down or the start is not made yet (no override: the period of command 5 holds).
uint32_t kvm_auto_interval_ms(const kvm_auto *a);
int kvm_auto_fps(const kvm_auto *a);			// the rate of the step (60 ... 10) while the bit is up (before the start: that of the provisional step 30), else 0

// The notes (main loop thread). All of them do nothing while the bit is down.
// An image of the capture, with the time it took (ms, the capture and the copy to the output buffer) and the frames of the desktop manager that it stands for (0 = not known); it is held until kvm_auto_note_image.
void kvm_auto_note_capture(kvm_auto *a, uint32_t ms, uint32_t dwmFrames);
// The video code looked at an image: `pending` = tiles wait for the video (0: no motion for the video in this image).
void kvm_auto_note_image(kvm_auto *a, uint32_t now, uint32_t pending);
// A frame was due and refused: kind 0 = the window of the viewer, 1 = the byte bucket.
void kvm_auto_note_held(kvm_auto *a, int kind);
// A key frame was written at `now`: `needMs` is how long it needs on the link (bytes / R; 0 = not known). The windows up to KVM_AUTO_KEY_FREEZE_MS + needMs after it are not evaluated.
void kvm_auto_note_key(kvm_auto *a, uint32_t now, uint32_t needMs);
// A P frame of `bytes` bytes was written with this QP (0 = unknown) at `now`.
void kvm_auto_note_frame(kvm_auto *a, uint32_t now, int qp, uint32_t bytes);

// The decision on one window (a pure function of the state and the window: no clock, no I/O). Returns dec->changed.
int kvm_auto_decide(kvm_auto *a, const kvm_auto_win *w, const kvm_auto_env *env, kvm_auto_dec *dec);
// Once an iteration: closes the window that is open when it is KVM_AUTO_WINDOW_MS old (then evaluates it, kvm_auto_decide), applies the start and the ceiling from outside. Returns 1 when the step changed.
// `win` (may be NULL) gets the window that was closed, `evaluated` is dec->evaluated.
int kvm_auto_tick(kvm_auto *a, const kvm_auto_env *env, kvm_auto_dec *dec, kvm_auto_win *win);
// One line for the log (no new line): the state, the window, the decision.
int kvm_auto_format(const kvm_auto *a, const kvm_auto_win *w, const kvm_auto_dec *d, char *out, int cap);

#ifdef __cplusplus
}
#endif

#endif // KVM_AUTO_H
