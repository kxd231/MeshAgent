/*
KVM: rate control of the H.264 video. Portable C99, no operating system API, time is an injected millisecond counter. Specification: docs/kvm-dev/h264-protocol.md of the
MeshCentral fork, 8.5 (target R, QP regulator) and plan B.3; the numbers in comments refer to it. Values marked (T) are tunable with /D<NAME>=<value>.

  target R    = min(0.8 x the viewer's budget, C); 2 Mb/s without a budget before the first rate sample. 20 % of the link stays for sharpening. C is the estimate of what the link takes:
              0.85 x the measured delivery rate while the window refused a video frame or a big JPEG rectangle in the last second (the link is the limit); otherwise it does not fall, and it
              grows by 25 % per second only while the video is held by its own bucket and C itself (not the budget) is R; never above 2 x max(0.85 x rate, 2 Mb/s).
  QP          constant quantizer per frame, regulated every KVM_VRC_WINDOW_MS (500 ms) on the bytes of the P frames of that window (IDR frames are not counted: one key frame is not a
              change of the content): above 110 % of R, or frames were refused by the byte bucket and the stream is above 90 % of R: QP + 2; below 70 % of R with at least 3 frames and
              no refusal for two windows in a row, or below 80 % when neither the bucket nor the window refused anything (the encode time limits the stream, R does not): QP - 1 (the hysteresis: one step down makes the next frame about 20 % bigger and a step down at once after every quiet window would
              oscillate). Floor KVM_VIDEO_QP_MIN (26), ceiling KVM_VIDEO_QP_MAX (38) for P frames. An IDR gets max(QP, QP_link), clamped to 26..38, where QP_link is the smallest QP at which the predicted key
              frame (the last one, 6 % per QP) fits in 0.3 s of R while the link is the limit (the window of the viewer refused a video frame in at least 6 of the last 40 windows of 500 ms) or the viewer has a budget; on a link that is not full R is only what was delivered, so the key frame is sized from the ceiling of the capacity estimate (2 x max(0.85 x rate, 2 Mb/s); the first key frame that fills the link makes the window refuse a frame, and from then on the 0.3 s rule holds) and is not forced to the top QP (LAN, 3-5 Mb/s with a frame rate that the loop limits): at a low rate the key frame is what the user waits for. A key frame is the base of everything after
              it, so it is never better than the P frames (IDR QP >= P QP, floor 26). After 10 s without a frame the QP goes back to 30. QP 38 with fewer than 2 frames a second and refusals for 5 s: `starved`.
  byte bucket the long run never exceeds R: the bucket refills at R, a frame may be encoded while it holds anything, its bytes are debited (the debt is limited), so at the QP ceiling the
              frame rate is whatever R allows (the encoder is asked less often, pendingVideo accumulates, an encoded frame is never dropped). The first frame of a generation and a key
              frame after a lost access unit are never refused by it. The WA-2b byte bucket of the JPEG path counts the video bytes too (kvm_adapt_note_write) but holds back only JPEG.
  encode cap  the frame interval is at least encode time x cores busy while it encodes / the CPU budget of the encoder (kvm_pace_interval, kvm_pace.h; 50 % of the machine,
              at least one core): fewer frames per second rather than a stalled capture, JPEG and input.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef KVM_VIDEO_RC_H
#define KVM_VIDEO_RC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef KVM_VRC_WINDOW_MS
#define KVM_VRC_WINDOW_MS 500			// (T) the regulator looks at this long
#endif
#ifndef KVM_VRC_UP_PCT
#define KVM_VRC_UP_PCT 110				// (T) above this share of R: QP up
#endif
#ifndef KVM_VRC_DOWN_PCT
#define KVM_VRC_DOWN_PCT 70				// (T) below this share of R: QP down (after KVM_VRC_DOWN_WINDOWS windows)
#endif
#ifndef KVM_VRC_REFUSED_PCT
#define KVM_VRC_REFUSED_PCT 90			// (T) frames refused by the bucket and more than this share of R was sent: the content wants more than R
#endif
#ifndef KVM_VRC_CALM_PCT
#define KVM_VRC_CALM_PCT 80				// (T) a window with no refusal of the bucket and none of the window (the encode time, not R, limits the stream) counts as quiet below this share of R.
										// One QP step down makes the frames 11-19 % larger (measured): from 80 % that is 89-95 % of R, i.e. up to the 90 % from which a refusal raises QP again
										// (KVM_VRC_REFUSED_PCT). Not seen, only possible: with a step above 12.5 % and the bucket refusing at 90-95 % QP can go q, q-1, q+1, q (about 5 windows, 2.5 s).
										// On a fast link, where the encode time limits the frames, the bucket does not refuse there. If it ever shows: after a QP raise use only the 70 % rule for 5 s.
#endif
#ifndef KVM_VRC_DOWN_WINDOWS
#define KVM_VRC_DOWN_WINDOWS 2			// (T)
#endif
#ifndef KVM_VRC_QP_UP
#define KVM_VRC_QP_UP 2
#endif
#ifndef KVM_VRC_QP_DOWN
#define KVM_VRC_QP_DOWN 1
#endif
#ifndef KVM_VRC_MIN_FRAMES
#define KVM_VRC_MIN_FRAMES 3			// a window with fewer frames says nothing about the content (no step down)
#endif
#ifndef KVM_VRC_IDR_QP_MIN
#define KVM_VRC_IDR_QP_MIN 26			// the range of the IDR QP (the floor is that of the P frames, not 22; the key frame is never better than the frames after it)
#endif
#ifndef KVM_VRC_IDR_QP_MAX
#define KVM_VRC_IDR_QP_MAX 38			// (22..32 left a key frame of 2 s at 1 Mb/s and 4.8 s at 0.5 Mb/s)
#endif
#ifndef KVM_VRC_IDR_MS
#define KVM_VRC_IDR_MS 300				// (T) a key frame should take at most this long at R (750 ms left 1.2-1.5 s key frames at 1 Mb/s)
#endif
#ifndef KVM_VRC_IDR_LIMITED_WINDOWS
#define KVM_VRC_IDR_LIMITED_WINDOWS 6	// (T) the link is the limit for the key frame when the window of the viewer refused a video frame in at least this many of the last KVM_VRC_IDR_HISTORY_WINDOWS windows (a key frame comes every 10 or 30 s: the memory has to reach that far)
#endif
#ifndef KVM_VRC_IDR_HISTORY_WINDOWS
#define KVM_VRC_IDR_HISTORY_WINDOWS 40	// (T) windows of KVM_VRC_WINDOW_MS (500 ms): 20 s (at most 64)
#endif
#ifndef KVM_VRC_CIDR_MS
#define KVM_VRC_CIDR_MS 60000			// (T) the throughput measured on the last key frame counts for this long (cleared at once by a refusal of the window)
#endif
#ifndef KVM_VRC_CIDR_PCT
#define KVM_VRC_CIDR_PCT 85				// (T) the next key frame is sized for this share of it
#endif
#ifndef KVM_VRC_IDR_SLOPE_PCT
#define KVM_VRC_IDR_SLOPE_PCT 94		// (T) a key frame is this share of the size one QP lower (measured: about 6 % per QP)
#endif
#ifndef KVM_VRC_IDR_PRIOR_BYTES
#define KVM_VRC_IDR_PRIOR_BYTES 200000	// the key frame to expect before one was measured: 1080p desktop at QP 28 (175 245 B at QP 30, 250 976 B at QP 24)
#endif
#ifndef KVM_VRC_HW_IDR_PRIOR_BYTES
#define KVM_VRC_HW_IDR_PRIOR_BYTES ((KVM_VRC_IDR_PRIOR_BYTES * 11) / 10)	// the hardware encoder, at the QP of the regulator: its key frame is 8 % SMALLER than the software one at the same nominal QP (420 KB against 450 KB at QP 30), but it is given QP - 4 (kvm_vcore_hw_qp): at the regulator's QP 30 it codes QP 26, 496 KB against the software 450 KB at QP 30 on the same picture: about 10 % MORE. Only a prior until the first key frame is measured. (The "2-3 times bigger" of an earlier measurement compared different pictures.)
#endif
#ifndef KVM_VRC_HW_IDR_SLOPE_PCT
#define KVM_VRC_HW_IDR_SLOPE_PCT KVM_VRC_IDR_SLOPE_PCT	// the same (0.957 per QP of the hardware encoder, 0.960 of the software one on the same picture)
#endif
#ifndef KVM_VRC_IDR_FLAT_MILLI
#define KVM_VRC_IDR_FLAT_MILLI 200			// the floor of a flat key frame, thousandths of a byte per macroblock (x264, CABAC: 0.05 - 0.1 B/MB; 0.8 B/MB with CAVLC)
#endif
#ifndef KVM_VRC_IDR_PROOF_MIN_BYTES
#define KVM_VRC_IDR_PROOF_MIN_BYTES 0		// a key frame smaller than this proves nothing about the link (it fits in the round trip): no capFloor, no cIdr; 0 = every key frame does (as before)
#endif
#ifndef KVM_VRC_IDR_PRIOR_QP
#define KVM_VRC_IDR_PRIOR_QP 28
#endif
#ifndef KVM_VRC_IDLE_MS
#define KVM_VRC_IDLE_MS 10000			// (T) no frame for this long: the QP goes back to KVM_VRC_IDLE_QP if it was above (a still picture says nothing about what the next motion costs)
#endif
#ifndef KVM_VRC_IDLE_QP
#define KVM_VRC_IDLE_QP 30
#endif
#ifndef KVM_VRC_STARVED_MS
#define KVM_VRC_STARVED_MS 5000			// QP at the ceiling, fewer than KVM_VRC_STARVED_FPS frames a second and refusals by the bucket for this long: the video is starved
#endif
#ifndef KVM_VRC_STARVED_FPS
#define KVM_VRC_STARVED_FPS 2
#endif
#ifndef KVM_VRC_CAP_MAX_PCT
#define KVM_VRC_CAP_MAX_PCT 200			// the capacity estimate is at most this share of max(0.85 x rate, KVM_VRC_DEFAULT_RATE)
#endif
#ifndef KVM_VRC_DEFAULT_RATE
#define KVM_VRC_DEFAULT_RATE 250000		// R in bytes per second (2 Mb/s) without a budget before the first rate sample
#endif
#ifndef KVM_VRC_MIN_RATE
#define KVM_VRC_MIN_RATE 12500			// R never below 100 kb/s
#endif
#ifndef KVM_VRC_PROBE_PCT
#define KVM_VRC_PROBE_PCT 25			// (T) per second: growth of the capacity estimate while the video is held by its own bucket and the link is not the limit
#endif
#ifndef KVM_VRC_BUDGET_PCT
#define KVM_VRC_BUDGET_PCT 80			// (T)
#endif
#ifndef KVM_VRC_RATE_PCT
#define KVM_VRC_RATE_PCT 85				// (T)
#endif
#ifndef KVM_VRC_FLOOR_MS
#define KVM_VRC_FLOOR_MS 60000			// (T) the proof of a key frame holds the estimate this long (as C_idr): cleared at once by a refusal of the window (the link may have fallen since)
#endif
#ifndef KVM_VRC_BURST_MS
#define KVM_VRC_BURST_MS 500			// (T) the bucket holds at most this much of R
#endif
#ifndef KVM_VRC_DEBT_MS
#define KVM_VRC_DEBT_MS 2000			// (T) ... and owes at most this much (a key frame is bigger than a window of R at a low rate)
#endif

typedef struct kvm_vrc
{
	int qp;							// the quantizer of P frames
	uint64_t rate;					// target R, bytes per second
	int64_t tokens;					// the bucket, bytes (negative = debt)
	uint32_t tokTime;
	uint32_t winStart;
	uint32_t winFrames, winBytes, winDenied;	// P frames of the window: count, bytes, refusals by the bucket (also IDR refusals)
	uint32_t winHeld;				// refusals of the window of the viewer (a video frame that could not go)
	uint32_t winSharp;				// bytes of sharpening in the window while the video moved
	uint8_t bitrateMode;			// the encoder cannot be steered by the QP (kvm_qpdet): R becomes its bit rate, the QP regulator stands still
	uint8_t belowRun, started;
	uint8_t idrLimited;				// the key frame is sized from R (the link or the budget is the limit, or nothing is known yet), else from the capacity ceiling
	uint64_t heldHist;				// one bit per regulator window (newest in bit 0): the window of the viewer refused a video frame in it
	uint64_t idrCap;				// the most a key frame may be sized for when the link is not full: twice the ceiling of the capacity estimate
	uint64_t cIdr;					// bytes per second that the link took on the last key frame (0 = not measured, expired or a refusal of the window since): the evidence that the link is faster than R says
	uint32_t cIdrAt;
	uint64_t cap;					// estimate of what the link takes, bytes per second (the part of it that the video may use)
	uint64_t capFloor;				// what the last acknowledged key frame proved: the link took at least this (0.85 x bytes / (time to the ACK less the hold of the viewer), no round trip taken off); the estimate is never below it while it stands
	uint32_t capFloorAt;
	uint8_t rFromFloor;				// the R of the last kvm_vrc_update was raised by the proof of a key frame (the proof was above the estimate, the link was not full, and the budget of the viewer did not hold R at or below the estimate), for the log
	uint32_t capTime, lastDenied;	// last update, last refusal by the bucket
	uint32_t idrPriorBytes;			// the key frame to expect before one was measured (at KVM_VRC_IDR_PRIOR_QP, 1920 x 1088) and the share per QP: those of the software encoder or of the hardware one (kvm_vrc_set_backend)
	uint8_t idrSlopePct, idrMeasured, hw;
	int qpMin;						// the floor of the QP of the regulator (and of the key frame): KVM_VIDEO_QP_MIN, for the hardware encoder KVM_VIDEO_QP_MIN - KVM_HW_QP_OFFSET (kvm_vcore_hw_qp maps it to the floor of that encoder)	// idrMeasured: a key frame was seen (lastIdrBytes is a measurement, not the prior)
	uint32_t lastIdrBytes;			// the last key frame (bytes) and the QP it was encoded with: the prediction of the next one
	int lastIdrQp;
	// the key frame is proportional to the area that it converts (the rest of the picture is flat): the macroblocks of the last key frame (0 = the whole picture), of the one that is planned, and of the
	// picture; the floor of a flat key frame in thousandths of a byte per macroblock
	uint32_t lastIdrReal, idrReal, idrTotal, totalMbs, flatMilli;
	uint32_t lastFrame;				// time of the last frame
	uint32_t starveSince;			// since when the video is starved, 0 = not; starved = the flag read by the log
	uint8_t starved;
	// statistics (the log reads and clears them every second)
	uint32_t sFrames, sIdr, sDenied, sHeld, sQpUp, sQpDown, sQpBlocked;	// sQpBlocked: windows in which the QP would have gone down but a refusal of the window forbade it
	uint64_t sBytes, sEncUs, sSharpBytes;
	uint32_t sEncMaxUs;
} kvm_vrc;

// R from the viewer's budget (kbit/s: cmd 5 bytes 10-11 are in units of 100 kbit/s, the agent keeps kbit/s; 0 = none) and the link capacity estimate (bytes per second, 0 = none yet):
// min(0.8 x budget, capacity), 2 Mb/s when neither is known. Bytes per second. (The literal 0.85 x measured rate of 8.5 is applied to the estimate by kvm_vrc_update.)
uint64_t kvm_vrc_target(unsigned budgetKbps, uint64_t capacity);

void kvm_vrc_init(kvm_vrc *r, int startQp, uint32_t now);	// the software encoder (the regulator of a new encoder: kvm_vrc_init_backend with the backend that this encoder is)
// the same for the encoder that is a hardware one (hw = 1): kvm_vrc_set_backend is applied again, so a new regulator for a new encoder cannot forget which encoder it is for (the QP floor and the prior of the key frame).
void kvm_vrc_init_backend(kvm_vrc *r, int startQp, uint32_t now, int hw);
// The size of the picture in pixels: the key frame expected before one has been measured (KVM_VRC_IDR_PRIOR_BYTES for 1920 x 1088).
void kvm_vrc_set_picture(kvm_vrc *r, uint32_t pixels);
// the key frame that is planned converts realMbs of totalMbs macroblocks of the picture (0, 0 = the whole picture, as before); call before kvm_vrc_qp(r, 1). The forecast is
// bytes = floor * totalMbs + c(qp) * realMbs, c learned from the last key frame (what it cost above the floor per macroblock it converted).
void kvm_vrc_set_idr_cover(kvm_vrc *r, uint32_t realMbs, uint32_t totalMbs);
// The forecast of the planned key frame at `qp` (the reservation of the window and the choice of the QP use it).
uint32_t kvm_vrc_idr_bytes(const kvm_vrc *r, int qp);
// What the virgin macroblocks of a P frame cost above the others at `qp` (the same c(qp): a macroblock that was never converted since the flat key frame is coded about as an intra macroblock).
uint32_t kvm_vrc_virgin_bytes(const kvm_vrc *r, uint32_t virginMbs, int qp);
// The floor of a flat key frame (thousandths of a byte per macroblock): KVM_VRC_IDR_FLAT_MILLI at the start, the measurement of the self test when there is one.
void kvm_vrc_set_flat_floor(kvm_vrc *r, uint32_t milliBytesPerMb);
// kvm_vrc_frame with the bytes of the virgin macroblocks of the frame taken out of what the regulator counts as the content (the bucket is charged with all of them): the content did not become dearer.
void kvm_vrc_frame_virgin(kvm_vrc *r, uint32_t now, uint32_t bytes, int qp, int isIdr, uint32_t encUs, uint32_t virginBytes);
// which encoder makes the key frames: 0 = the software MFT (prior KVM_VRC_IDR_PRIOR_BYTES, KVM_VRC_IDR_SLOPE_PCT), 1 = the hardware MFT (KVM_VRC_HW_*: about 10 % more at the QP of the regulator, the QP is mapped by kvm_vcore_hw_qp). Only the prior (until the
// first key frame is measured) and the prediction of the next key frame change; call before kvm_vrc_set_picture.
void kvm_vrc_set_backend(kvm_vrc *r, int hw);
void kvm_vrc_set_target(kvm_vrc *r, uint64_t bytesPerSec, uint32_t now);
// Every iteration: the capacity estimate and R. rate = delivery rate of the ACK window (bytes per second, last measured one while there is no sample), linkLimited = the window refused a write
// recently (kvm_stream_link_limited). While the link is the limit the rate is what it takes: capacity = 0.85 x rate. Otherwise the rate only says what was sent, and the bucket of the video
// is what limits it: the capacity is never below 0.85 x rate and grows by KVM_VRC_PROBE_PCT per second as long as the bucket refuses frames and the capacity (not the budget) is R
// (probing for more), up to 2 x max(0.85 x rate, 2 Mb/s). Then R = kvm_vrc_target.
void kvm_vrc_update(kvm_vrc *r, uint32_t now, unsigned budgetKbps, uint64_t rate, int linkLimited);
// The quantizer for the next frame. An IDR gets max(QP, QP_link) in 26..38 (QP_link: the smallest QP at which the predicted key frame fits in KVM_VRC_IDR_MS of R).
int kvm_vrc_qp(const kvm_vrc *r, int isIdr);
// May a frame be encoded now? (the bucket holds something, or the frame is one that is never refused). A refusal is counted for the regulator.
int kvm_vrc_may_encode(kvm_vrc *r, uint32_t now, int never_refused);
int kvm_vrc_peek(const kvm_vrc *r, uint32_t now);	// the same without counting anything
// Bytes of sharpening went out. While the video moves (`videoActive`, kvm_sharp_active) they are debited from the same bucket as the frames and counted against R by the regulator: R is the
// whole stream, so what the sharpening takes is not available to the video. Outside a movement they only go into the statistics.
void kvm_vrc_note_sharpen(kvm_vrc *r, uint32_t now, uint32_t bytes, int videoActive);
// The encoder does not listen to the QP: from now on the stream is steered by the bit rate (kvm_vrc_bitrate_bps), the QP stays where it is.
void kvm_vrc_set_bitrate_mode(kvm_vrc *r, int on);
uint32_t kvm_vrc_bitrate_bps(const kvm_vrc *r);		// R as the bit rate of the encoder (R is bytes per second), 100 kb/s .. 400 Mb/s
// The viewer acknowledged the position of the last key frame (kvm_stream_watch_result): idrBytes written at `ts`, the ACK arrived at `ackAt`, the viewer held it `queueMs`, the network round trip is
// `minRtt`. The throughput is idrBytes / (ackAt - ts - queueMs - minRtt), at least 10 ms of it: bytes that the link really carried in one go, so it cannot exceed its capacity. While the link is
// not full the next key frame is sized from min(0.85 x it, the ceiling of the capacity estimate) instead of R, which only follows what was delivered.
// the same bytes over the whole time to the ACK less the hold of the viewer (the round trip not taken off: minRtt may be the value of a queue) say that the link took at least that: 0.85 x it is
// the proof (capFloor) under R, for KVM_VRC_FLOOR_MS, while the link is not full; a refusal of the window takes it away. It lifts R out of what a trickle of rate samples made of it before the first key frame.
// Every acknowledged key frame arms it again: with periodic key frames R goes to the ceiling of the estimate (2 x 0.85 x the remembered rate, which holds the bytes of the key frame) at each of them,
// higher than before the first key frame proof; only the first key frame of a session without samples is no higher than in. The window of the viewer still bounds the queue.
void kvm_vrc_idr_acked(kvm_vrc *r, uint32_t now, uint32_t idrBytes, uint32_t ts, uint32_t ackAt, uint32_t queueMs, uint32_t minRtt);
// A video frame was refused by the window of the viewer (not by the bucket): the regulator does not lower the QP in such a window, and `starved` counts it.
void kvm_vrc_window_refused(kvm_vrc *r);
void kvm_vrc_window_refused_soft(kvm_vrc *r);	// a refusal of the window that is no evidence of the link: not a window of the held windows, the proof of the key frame stays
// A frame was written (encoded with `qp`): debit the bucket, count it for the window and the statistics, remember a key frame for the prediction of the next one.
void kvm_vrc_frame(kvm_vrc *r, uint32_t now, uint32_t bytes, int qp, int isIdr, uint32_t encUs);
// Once a second, before the statistics are cleared: the flag `starved` (QP at the ceiling, fewer than KVM_VRC_STARVED_FPS frames a second and refusals by the bucket or the window, for
// KVM_VRC_STARVED_MS). Only a flag in the log: no protocol field, no way back to JPEG. Returns the flag.
int kvm_vrc_update_starved(kvm_vrc *r, uint32_t now);
// Every iteration: closes the regulator window when it is over; after KVM_VRC_IDLE_MS without a frame the QP goes back to KVM_VRC_IDLE_QP. Returns the change of QP (0 none).
int kvm_vrc_tick(kvm_vrc *r, uint32_t now);
// Bytes per second that the P frames of the last window were (for the log), 0 when there was none.
void kvm_vrc_clear_stats(kvm_vrc *r);

#ifdef __cplusplus
}
#endif

#endif // KVM_VIDEO_RC_H
