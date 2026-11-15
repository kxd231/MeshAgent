/*
KVM: the governor of the sharpening while the H.264 video runs. Portable C99, no operating system API, time is an injected millisecond counter (uint32_t, wraps; every
comparison of two times is a signed difference). The decisions of WA-2 that were in kvm_adapt.cpp and could not be tested in Docker live here; the glue (Windows/kvm_adapt.cpp)
only reads the clock and the window of the ACK and calls these functions.

Why. A measurement found 63-70 % of the bytes of the H.264 sessions in sharpening, 12-25 % was planned. Two causes:

  (1) the settle rule "a tile that has not changed for 150 ms" was evaluated as (DWORD)(gA.now - t->lastChange) >= 150, but the tile that a video frame paints gets lastChange from a clock read
      AFTER the encoder (a few ms later than gA.now of the start of the iteration): the difference is negative, in unsigned arithmetic about 4.29e9, so every painted tile was a candidate at once.
      kvm_sharp_settled() is the signed comparison.
  (2) nothing limited the share of sharpening while the video moved: a bucket refilled with KVM_SHARP_SHARE_PCT of the rate (the smaller of the delivery rate and the viewer's budget) pays for it,
      the burst is KVM_SHARP_BURST_MS of that share. While the video is idle (no frame and nothing pending for KVM_SHARP_ACTIVE_MS) there is no limit but the window, so the picture
      converges as fast as before.
  (3) the window is looked at before EVERY packet (inflight < 0.5 W, kvm_sharp_window_ok), and while the video is active one iteration sends at most KVM_SHARP_ITER_WINDOW_PCT of W.

Forced sharpening (kvm_vcore_force_sharpen) pays from the same bucket (kvm_sharp_forced) but is never refused by it: it is what keeps the sharpening from starving.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef KVM_SHARPEN_H
#define KVM_SHARPEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef KVM_SHARP_ACTIVE_MS
#define KVM_SHARP_ACTIVE_MS 300			// (T) the video is "moving" for this long after a frame went out or tiles were pending
#endif
#ifndef KVM_SHARP_ACTIVE_FRAMES
#define KVM_SHARP_ACTIVE_FRAMES 3		// (T) once the frame interval is known (kvm_sharp_set_interval) the movement lasts this many intervals ...
#endif
#ifndef KVM_SHARP_ACTIVE_MIN_MS
#define KVM_SHARP_ACTIVE_MIN_MS 100		// (T) ... and at least this long: max(3 intervals, 100 ms) instead of 300 ms (a frame of the video every 16 ms is gone for 48 ms; the sharpening of a tile waits 150 ms anyway)
#endif
#ifndef KVM_SHARP_POST_MS
#define KVM_SHARP_POST_MS 1000			// (T) for this long after the last movement (any: a frame of the video, tiles waiting, a frame with many changes that went out as JPEG) the packets of the sharpening go ...
#endif
#ifndef KVM_SHARP_POST_WINDOW_PCT
#define KVM_SHARP_POST_WINDOW_PCT 25	// (T) ... only while inflight < this share of W (instead of 50 %): the next series of motion does not meet a window that the sharpening of the last one has filled
#endif
#ifndef KVM_SHARP_SHARE_PCT
#define KVM_SHARP_SHARE_PCT 20			// (T) share of min(delivery rate, viewer's budget) that the sharpening may use while the video moves (8.5: 20 % of the link)
#endif
#ifndef KVM_SHARP_BURST_MS
#define KVM_SHARP_BURST_MS 250			// (T) the bucket holds at most this much of the share ...
#endif
#ifndef KVM_SHARP_BURST_MIN
#define KVM_SHARP_BURST_MIN 8192		// ... and never less than this (one packet of the sharpening is a few KB)
#endif
#ifndef KVM_SHARP_ITER_WINDOW_PCT
#define KVM_SHARP_ITER_WINDOW_PCT 25	// (T) one iteration sends at most this share of W while the video moves
#endif
#ifndef KVM_SHARP_PACKET_WINDOW_PCT
#define KVM_SHARP_PACKET_WINDOW_PCT 50	// the window gate of a packet: inflight < this share of W (= KVM_STREAM_SHARPEN_PCT, kvm_stream.h)
#endif
#ifndef KVM_SHARP_DEFAULT_RATE
#define KVM_SHARP_DEFAULT_RATE 125000	// bytes per second when the delivery rate is not known yet (1 Mb/s)
#endif

// milliseconds from `then` to `now`, signed: negative when `then` is later than `now` (a time stamp taken after the clock that `now` came from).
static __inline int32_t kvm_sharp_elapsed_ms(uint32_t now, uint32_t then) { return (int32_t)(now - then); }
// has the content been still for `settleMs`? A change that is later than `now` (elapsed < 0) is not.
static __inline int kvm_sharp_settled(uint32_t now, uint32_t lastChange, uint32_t settleMs) { return (int32_t)(now - lastChange) >= (int32_t)settleMs; }

// again: has something happened within `ms` before `now`? `stamp` 0 = never. A stamp that is later than `now` (written by another thread after `now` was read: the input
// thread) happened "just now": it counts as recent. The unsigned difference of such a pair is about 4.29e9.
static __inline int kvm_sharp_recent(uint32_t now, uint32_t stamp, uint32_t ms) { return stamp != 0 && (int32_t)(now - stamp) < (int32_t)ms; }

typedef struct kvm_sharp
{
	int64_t tokens;				// the bucket of the share, bytes (negative = debt)
	int64_t cap;				// its burst at the last rate
	uint32_t tokTime;
	uint32_t lastMotion;		// the last frame or pending tile
	uint8_t haveMotion, haveTok;
	uint32_t activeMs;			// how long a movement lasts (0 = KVM_SHARP_ACTIVE_MS until kvm_sharp_set_interval)
	uint8_t iterActive;			// the iteration that began last was inside a movement
	int64_t iterAllowed, iterSent;
	// statistics (the log reads and clears them)
	uint32_t sBucketRefused, sIterCapped, sWindowRefused, sActiveIters, sIdleIters;
	uint64_t sBytesActive, sBytesIdle, sBytesForced;
} kvm_sharp;

void kvm_sharp_init(kvm_sharp *g, uint32_t now);
void kvm_sharp_clear_stats(kvm_sharp *g);
// A video frame went out, or tiles wait for the next one, or a frame with at least 16 tiles that really changed went out in any way (JPEG too): the movement goes on.
void kvm_sharp_motion(kvm_sharp *g, uint32_t now);
int kvm_sharp_active(const kvm_sharp *g, uint32_t now);
// The start of an iteration's sharpening: `rate` = min(delivery rate, viewer's budget in bytes per second), 0 = not known; `window` = W (0 = no window); `want` = what WA-2 would send now
// (KVM_ADAPT_SHARPEN_BYTES at the level of the regulator). Returns the bytes this iteration may send: `want` when the video is idle; else min(want, bucket, 0.25 W), 0 when the bucket is empty.
int64_t kvm_sharp_begin(kvm_sharp *g, uint32_t now, uint64_t rate, uint64_t window, int64_t want);
// Before each packet: the window gate (inflight < 0.5 W; window 0 = there is no window). A refusal is counted.
int kvm_sharp_window_ok(kvm_sharp *g, uint64_t inflight, uint64_t window);
// The same with the time: for KVM_SHARP_POST_MS after the last movement the gate is KVM_SHARP_POST_WINDOW_PCT of W (a quarter), later the half of the line above.
int kvm_sharp_window_ok_at(kvm_sharp *g, uint32_t now, uint64_t inflight, uint64_t window);
// The frame interval of the video (ms): a movement lasts max(KVM_SHARP_ACTIVE_FRAMES intervals, KVM_SHARP_ACTIVE_MIN_MS) after the last frame / pending tile / frame with many changes.
void kvm_sharp_set_interval(kvm_sharp *g, uint32_t intervalMs);
// 1 for KVM_SHARP_POST_MS after the last movement (the movement itself included)
int kvm_sharp_post_motion(const kvm_sharp *g, uint32_t now);
// A packet of `bytes` went out: charged to the bucket while the video moves.
void kvm_sharp_sent(kvm_sharp *g, uint32_t now, int64_t bytes);
// A forced packet went out (it is never refused by the bucket, it only pays).
void kvm_sharp_forced(kvm_sharp *g, uint32_t now, int64_t bytes);

#ifdef __cplusplus
}
#endif

#endif // KVM_SHARPEN_H
