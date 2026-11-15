/*
KVM: stream protocol of the commands 240-243 (VIDEO_CTRL, STREAM_ACK, VIDEO_STATE, VIDEO_FRAME) and the end-to-end ACK window.

Portable C99, no operating system API: every function works on buffers and on a state object, and time is an injected millisecond
counter (uint32_t, wraps), so the tests are deterministic. The specification is docs/kvm-dev/h264-protocol.md of the MeshCentral fork
(chapters 3, 4, 5.1-5.2 and 8); the numbers in comments refer to it. Values marked (T) are tunable with /D<NAME>=<value>.

What is here:
  - building and strict parsing of 240, 241, 242 and 243 (the frame inside MNG_JUMBO), in the validation order of chapter 4;
  - the agent side state of chapter 5 that does not need a video encoder: what the viewer asked for (want), the veto mask, capabilities,
    the 242 answers, generation counter;
  - the ACK window of chapter 8: ring of positions, inflight, delivery rate, window W, gates, timeouts, the marker rule and the probe marker.

Threading: none of this is thread safe. The caller (Windows/kvm.c) takes one lock around every call and around every write to the pipe,
so that "bytes written" and the positions in the ring describe the same order.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef KVM_STREAM_H
#define KVM_STREAM_H

#include <stddef.h>
#include <stdint.h>
#include "kvm_stdin.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- command numbers and sizes (chapter 2) ----
#define KVM_CMD_JUMBO 27
#define KVM_CMD_VIDEO_CTRL 240
#define KVM_CMD_STREAM_ACK 241
#define KVM_CMD_VIDEO_STATE 242
#define KVM_CMD_VIDEO_FRAME 243
#define KVM_CTRL_SIZE 16						// 240, 241, 242: size field and length
#define KVM_MARKER_INNER 20						// inner length N of a marker
#define KVM_MARKER_BYTES (8 + KVM_MARKER_INNER)	// a marker with the JUMBO header (28)
#define KVM_FRAME_MAX_N (8u * 1024u * 1024u)	// largest N of a 243
#define KVM_FRAME_MAX_RECTS 256
#define KVM_FRAME_MAX_DIM 8192
#define KVM_TILE_PAD 32							// coded = screen padded up to a multiple of this

// 240 operations (3.1)
#define KVM_OP_VIDEO_ON 1
#define KVM_OP_VIDEO_OFF 2
#define KVM_OP_REPAIR 3
#define KVM_OP_ACK_ONLY 4
#define KVM_OP_VETO 5
#define KVM_OP_VETO_END 6
// flags of op 1 (profiles), op 3, op 5/6 (source of the veto)
#define KVM_PROFILE_CBP 1
#define KVM_PROFILE_MAIN 2
#define KVM_PROFILE_HIGH 4
#define KVM_REPAIR_IDR 1
#define KVM_REPAIR_TILES 2
#define KVM_VETO_MESHCORE 1
#define KVM_VETO_MUX 2
#define KVM_VETO_ALL (KVM_VETO_MESHCORE | KVM_VETO_MUX)	// the defined sources: every other bit of the flags of op 5 / op 6 is ignored and never stored
// 243 flags
#define KVM_FRAME_IDR 1
#define KVM_FRAME_MARKER 2
// 242 reasons (3.3) and capability bits
#define KVM_REASON_OK 0
#define KVM_REASON_NO_VIDEO_BUILD 1
#define KVM_REASON_NO_MF 2
#define KVM_REASON_INIT 3
#define KVM_REASON_ERROR 4
#define KVM_REASON_VETO 5
#define KVM_REASON_DISABLED 6
#define KVM_REASON_RESOLUTION 7
#define KVM_REASON_PROFILE 8
#define KVM_REASON_NO_ACK 9
#define KVM_REASON_AVAILABLE 10
#define KVM_REASON_STARTED 11
#define KVM_CAPS_H264 1
#define KVM_CAPS_ACK 2

// ---- tunables (8) ----
#ifndef KVM_STREAM_RING
#define KVM_STREAM_RING 1024				// positions the agent remembers; a full ring overwrites the oldest
#endif
#ifndef KVM_STREAM_W_START
#define KVM_STREAM_W_START (256u * 1024u)	// (T) window before the first rate sample
#endif
#ifndef KVM_STREAM_W_MIN
#define KVM_STREAM_W_MIN (64u * 1024u)		// (T)
#endif
#ifndef KVM_STREAM_W_MAX
#define KVM_STREAM_W_MAX (4u * 1024u * 1024u)	// (T)
#endif
#ifndef KVM_STREAM_W_RESUME
#define KVM_STREAM_W_RESUME (256u * 1024u)	// (T) the window while there is no sample of the last 2 s and only the memory of the rate sizes it: a series after a pause starts with the remembered size, but never above this (the link may have changed in the pause: a queue of this much is 0.5 s at 4 Mb/s; and the check of the resume below corrects it as soon as 64 KB have been through)
#endif
#ifndef KVM_STREAM_MEM_HOLD_MS
#define KVM_STREAM_MEM_HOLD_MS 30000		// (T) the remembered rate is kept this long without a confirmation (a sample as high, or one that the window limited) ...
#endif
#ifndef KVM_STREAM_MEM_HALF_MS
#define KVM_STREAM_MEM_HALF_MS 30000		// (T) ... then it is halved, and halved again every this long
#endif
#ifndef KVM_STREAM_BUSY_GAP_MS
#define KVM_STREAM_BUSY_GAP_MS 500			// (T) a write into a pipe that has been empty this long begins a new period of work: a rate sample never spans the silence between two series of motion (a burst of 0.3 s measured over 0.7 s of which 0.4 s was nothing says less than half of what the link took). A sender that the window holds back is not silent: the pipe is not empty then. Not shorter than the span of a sample (KVM_STREAM_RATE_MIN_MS): a stream of 2 frames a second (R at its floor) has gaps of that size and must still make samples
#endif
#ifndef KVM_STREAM_RESUME_BYTES
#define KVM_STREAM_RESUME_BYTES (64u * 1024u)	// (T) the check of the resume: the first acknowledged position of a period of work that the memory alone sizes, with at least this much in flight when it was written (a smaller flight is dominated by the jitter of the round trip)
#endif
#ifndef KVM_STREAM_RESUME_PCT
#define KVM_STREAM_RESUME_PCT 50				// (T) ... says the link is slower than remembered when what it delivered is below this share of the remembered rate: the memory becomes that
#endif
#ifndef KVM_STREAM_QUEUE_DELAY_MS
#define KVM_STREAM_QUEUE_DELAY_MS 200		// (T) W = rate * (minRtt + this)
#endif
#ifndef KVM_STREAM_RATE_SPAN_MS
#define KVM_STREAM_RATE_SPAN_MS 2000		// (T) the rate is the maximum of the samples of this long
#endif
#ifndef KVM_STREAM_RATE_MIN_MS
#define KVM_STREAM_RATE_MIN_MS 500			// (T) a sample covers at least this long
#endif
#ifndef KVM_STREAM_SMALL_PCT
#define KVM_STREAM_SMALL_PCT 125			// (T) small rectangles go while inflight < this percent of W
#endif
#ifndef KVM_STREAM_SHARPEN_PCT
#define KVM_STREAM_SHARPEN_PCT 50			// (T) sharpening goes while inflight < this percent of W
#endif
#ifndef KVM_STREAM_STALL_MS
#define KVM_STREAM_STALL_MS 2000			// (T) no ACK this long with data in flight: W = KVM_STREAM_W_MIN until the next ACK
#endif
#ifndef KVM_STREAM_TIMEOUT_MS
#define KVM_STREAM_TIMEOUT_MS 15000			// (T) no ACK this long with data in flight: ACK and video off, 242 reason 9
#endif
#ifndef KVM_STREAM_PROBE_MS
#define KVM_STREAM_PROBE_MS 1000			// (T) probe marker: data in flight, no ACK this long; at most one per this time
#endif
#ifndef KVM_STREAM_RTT_WINDOW_MS
#define KVM_STREAM_RTT_WINDOW_MS 10000		// (T) minRtt is the minimum of the last this long; an older value is refreshed by the RTT probe below
#endif
#ifndef KVM_STREAM_PROBE_INTERVAL_MS
#define KVM_STREAM_PROBE_INTERVAL_MS 10000	// (T) RTT probe: at most one per this time
#endif
#ifndef KVM_STREAM_PROBE_MIN_MS
#define KVM_STREAM_PROBE_MIN_MS 200			// (T) the pipe is given max(this, minRtt) after the time the data in flight needs to drain, to reach a low inflight; then the probe is given up
#endif
#ifndef KVM_STREAM_PROBE_WAIT_MS
#define KVM_STREAM_PROBE_WAIT_MS 3000		// (T) the probe marker has been written; its ACK is waited for this long
#endif
#ifndef KVM_STREAM_PROBE_FLIGHT
#define KVM_STREAM_PROBE_FLIGHT 4096		// (T) a position written with at most this many bytes in flight (or this much as 20 ms of the rate) is a clean round trip sample
#endif
#ifndef KVM_STREAM_PROBE_QDELAY_MS
#define KVM_STREAM_PROBE_QDELAY_MS 20		// (T) ... the second form
#endif
#ifndef KVM_STREAM_HELD_MS
#define KVM_STREAM_HELD_MS 1000				// the window counts as the limiting factor when it refused a write in the last this long
#endif
#ifndef KVM_STREAM_ACK_RATE
#define KVM_STREAM_ACK_RATE 200				// ACK per second the agent accepts (token bucket)
#endif
#ifndef KVM_STREAM_ACK_BURST
#define KVM_STREAM_ACK_BURST 50				// ... with this burst
#endif
#ifndef KVM_INBOX_RATE
#define KVM_INBOX_RATE 40					// ordinary 240 (op 1-4) per second the agent accepts from the input thread (token bucket); the veto (op 5, op 6) is not counted
#endif
#ifndef KVM_INBOX_BURST
#define KVM_INBOX_BURST 32					// ... with this burst
#endif
#ifndef KVM_STREAM_HIST
#define KVM_STREAM_HIST 256					// ACK records kept for the rate (rate limit 200/s * 0.5 s + burst fits)
#endif
#ifndef KVM_STREAM_SAMPLES
#define KVM_STREAM_SAMPLES 16
#endif

// ---- rejection codes (chapter 11; the names are the strings of vectors.json) ----
typedef enum kvm_rej
{
	KVM_REJ_NONE = 0,
	KVM_REJ_BAD_SIZE,
	KVM_REJ_TRUNCATED,
	KVM_REJ_BAD_VERSION,
	KVM_REJ_BAD_OP,
	KVM_REJ_BAD_FLAGS,
	KVM_REJ_BAD_LIMIT,
	KVM_REJ_BAD_FIELD,
	KVM_REJ_BAD_SEQ,
	KVM_REJ_FRAME_TOO_BIG,
	KVM_REJ_BAD_MARKER,
	KVM_REJ_BAD_RECT_COUNT,
	KVM_REJ_CODED_SIZE,
	KVM_REJ_NO_SCREEN,
	KVM_REJ_BAD_RECT,
	KVM_REJ_NO_PAYLOAD,
	KVM_REJ_NOT_243,			// a JUMBO whose inner command is not 243 (the reference validator calls it NOT_243)
	KVM_REJ_COUNT
} kvm_rej;
const char *kvm_stream_rej_name(kvm_rej r);	// "NONE" for KVM_REJ_NONE

// ---- messages ----
typedef struct kvm_ctrl		// 240
{
	uint8_t ver, op, flags, maxLevelIdc;
	uint16_t maxW, maxH;
	uint32_t reserved;
} kvm_ctrl;

typedef struct kvm_ack		// 241
{
	uint32_t seq, ts;
	uint16_t queueMs, flags;
} kvm_ack;

typedef struct kvm_state_msg	// 242
{
	uint8_t ver, video, ack, reason, profile, level;
	uint16_t codedW, codedH;
	uint8_t caps, gen;
} kvm_state_msg;

typedef struct kvm_frame		// 243 (parsed; the pointers point into the buffer that was parsed)
{
	uint32_t n;				// N: length of the inner command
	uint32_t seq, ts;
	uint8_t flags, gen;
	uint16_t codedW, codedH, nRects;
	const uint8_t *rects;	// nRects * 8 bytes (big endian x, y, w, h), NULL for a marker
	const uint8_t *annexB;	// NULL for a marker
	size_t annexLen;
} kvm_frame;

typedef struct kvm_frame_params	// input of kvm_stream_build_frame
{
	uint32_t seq, ts;
	uint8_t flags, gen;
	uint16_t codedW, codedH;
	unsigned nRects;
	const uint16_t *rects;	// nRects * 4 values (x, y, w, h)
	const uint8_t *annexB;
	size_t annexLen;
} kvm_frame_params;

// Parsers: 4.1 (240, 241, 242: one message = one command) and 4.2 steps 1-10 (243). Return KVM_REJ_NONE and fill *out, or the first violation.
// Reserved fields are not checked. screenW / screenH are the size of the last cmd 7 (0 = none yet).
kvm_rej kvm_stream_parse_ctrl(const uint8_t *b, size_t len, kvm_ctrl *out);
kvm_rej kvm_stream_parse_ack(const uint8_t *b, size_t len, kvm_ack *out);
kvm_rej kvm_stream_parse_state(const uint8_t *b, size_t len, kvm_state_msg *out);
kvm_rej kvm_stream_parse_frame(const uint8_t *b, size_t len, unsigned screenW, unsigned screenH, kvm_frame *out);
kvm_rej kvm_stream_validate(int cmd, const uint8_t *b, size_t len, unsigned screenW, unsigned screenH);	// any of the four, no output

// Builders. The 16 byte ones always write 16 bytes. kvm_stream_build_frame returns the number of bytes of the whole message (8 + N), 0 when
// it does not fit in cap, nRects is 0 or above 256 for a frame, or N would exceed 8 MiB. A marker is built with KVM_FRAME_MARKER in flags and
// coded = nRects = 0; kvm_stream_build_marker is the shortcut (28 bytes).
void kvm_stream_build_ctrl(uint8_t out[16], const kvm_ctrl *c);
void kvm_stream_build_ack(uint8_t out[16], const kvm_ack *a);
void kvm_stream_build_state(uint8_t out[16], const kvm_state_msg *m);
size_t kvm_stream_build_frame(uint8_t *out, size_t cap, const kvm_frame_params *p);
// The same in two parts for a sender that encodes straight behind the header (no copy of the Annex B data): kvm_stream_frame_header_len(nRects) = 28 + 8 * nRects
// is the offset of the Annex B data; kvm_stream_build_frame_header writes the JUMBO header, the inner header and the rectangles (the length N is computed with
// p->annexLen, p->annexB is not touched) and returns the header length, 0 on the errors of kvm_stream_build_frame.
size_t kvm_stream_frame_header_len(unsigned nRects);
size_t kvm_stream_build_frame_header(uint8_t *out, size_t cap, const kvm_frame_params *p);
void kvm_stream_build_marker(uint8_t out[KVM_MARKER_BYTES], uint32_t seq, uint32_t ts, uint8_t gen);

// The seq rule (3.4, 7.2): the position after 0xFFFFFFFF is 1 (0 is skipped); an IDR may jump; the first position after a 242 (no last) is never a gap.
uint32_t kvm_stream_seq_next(uint32_t last);
int kvm_stream_seq_is_gap(int haveLast, uint32_t last, uint32_t seq, int idr);
// coded size for a screen size: padded up to a multiple of 32
unsigned kvm_stream_pad32(unsigned v);

// ---- agent state (5.1, 5.2) and the ACK window (8) ----
#define KVM_WANT_NONE 0
#define KVM_WANT_ACK 1
#define KVM_WANT_VIDEO 2

// What a write to the pipe was (kvm_stream_note_write)
#define KVM_W_DATA 0		// tiles, cmd 7, cursor, display list... : must be covered by a position (rule M1)
#define KVM_W_POSITION 1	// a marker or a video frame that carries a position
#define KVM_W_STATE 2		// a 242: counted, but does not need a position of its own

// Results of kvm_stream_on_ack
#define KVM_ACK_OK 0		// accepted: the window moved
#define KVM_ACK_OFF 1		// ACK is not on: ignored
#define KVM_ACK_FLOOD 2		// over KVM_STREAM_ACK_RATE: ignored
#define KVM_ACK_UNKNOWN 3	// (seq, ts) is not in the ring (forged, overwritten, from before a reset): ignored
#define KVM_ACK_STALE 4		// in the ring, but not newer than the last accepted one (repeat, late): ignored

// Actions that on_ctrl asks the caller to do (video comes with the first video path; in a build without video they are never set)
#define KVM_ACT_VIDEO_REQUEST 1	// op 1 reached a build with video: the caller starts the encoder and answers with 242
#define KVM_ACT_REPAIR 2		// op 3 with the repair flag while video is on
#define KVM_ACT_IDR 4			// op 3 with the IDR flag while video is on
#define KVM_ACT_VIDEO_STOP 8	// video was on and is off now (op 2, op 4, op 5): the video code releases the encoder and puts the pending tiles back as JPEG (I7)

typedef struct kvm_stream_pos
{
	uint32_t seq, ts;
	uint64_t cum;	// bytes written up to and including this position
	uint32_t flight;	// bytes in flight (written, not acknowledged) right after this position, saturated
	uint8_t rttProbe;	// this position is the marker of an RTT probe
} kvm_stream_pos;

typedef struct kvm_stream_hist
{
	uint32_t t;
	uint64_t d;		// acked bytes at t
} kvm_stream_hist;

typedef struct kvm_stream_sample
{
	uint32_t t;
	uint64_t rate;	// bytes per second
	uint8_t appLtd;	// the pipe was empty at some moment of the span (the sender had nothing to send): the rate is a lower bound of what the link takes, not what it takes
} kvm_stream_sample;

typedef struct kvm_stream_stats
{
	uint32_t acksOk, acksOff, acksFlood, acksUnknown, acksStale;
	uint32_t markers, probes, timeouts, stalls, ctrlOk, ctrlBad, gateDenied;
	uint32_t rttProbeStarts, rttProbeDone, rttProbeAborts;
} kvm_stream_stats;

typedef struct kvm_stream
{
	// capabilities (5.1); the reasons say why a capability is missing (1 no video in this build, 2 no encoder platform, 6 switched off)
	uint8_t capVideo, capAck, videoOffReason, ackOffReason;
	uint8_t capAckCfg, adaptOk;	// capAck as configured (build, environment) and whether the adaptive path can use the window now (picture type); capAck = capAckCfg && adaptOk
	// requests and state
	uint8_t want;			// KVM_WANT_*
	uint8_t vetoMask;		// KVM_VETO_* sources
	uint8_t gen;
	uint8_t videoOn;		// set by the video code (never in a build without video); implies ackActive
	uint32_t seqCounter;	// last seq handed out (0 = none yet)
	uint32_t lastOp1;		// time of the last accepted op 1, for the 2 s coalescing of the video code
	uint8_t haveOp1;
	// bytes
	uint64_t bytesWritten;	// everything written to the pipe, since the start of the process
	// the window; valid while ackActive
	uint8_t ackActive, dirty, everHeld, stallNoted;
	uint32_t lastProgress;	// last accepted ACK, or the moment data started to be in flight
	uint32_t lastProbe;
	uint32_t lastHeldBack;	// last time a gate refused a write (any gate: this feeds the rate samples)
	uint8_t linkHeldEver;	// a refused video frame or a refused big JPEG rectangle, outside the RTT probe: this alone says "the link is the limit" for the rate control
	uint32_t lastLinkHeld;
	uint32_t minRtt;		// ms, 0 = not measured yet; the minimum of the last KVM_STREAM_RTT_WINDOW_MS, refreshed by the RTT probe
	uint32_t rttStamp;		// when minRtt was last confirmed or refreshed
	uint8_t armed;			// the pipe has been empty once since the activation: only positions written after that give a round trip sample (not the queue of the first picture)
	uint64_t armedOrd;
	uint8_t rateMemFull;	// the remembered rate was last raised by a sample that was no lower bound (the window held the sender back, or the pipe never emptied)
	uint64_t rateMem;		// the rate that is remembered (the highest that was seen, a maximum filter): used while there is no sample (a pause in the traffic does not send the window back to its start value).
							// Only a higher sample changes it: a lower one says how much was sent, or how much a window took of a burst, not what the link can do. Kept KVM_STREAM_MEM_HOLD_MS, then halved every
							// KVM_STREAM_MEM_HALF_MS; cleared by a real stall (KVM_STREAM_STALL_MS without an acknowledgement) and by a round trip that has more than doubled. While samples exist they size the window and R
	uint32_t rateMemAt;		// when it was last set or confirmed
	uint32_t busyFrom, emptySince;	// the start of the period of work the last write belongs to (the pipe had been empty for KVM_STREAM_BUSY_GAP_MS before it), and since when the pipe is empty (haveEmpty)
	uint8_t haveEmpty;
	uint8_t resumeArmed;	// the period of work began on the remembered rate alone: the first position with KVM_STREAM_RESUME_BYTES in flight checks it against the link
	uint32_t reservedBytes;	// the access unit of a video frame that the encoder thread is making (an estimate): the gates of the other writes count it as in flight, because it will be written before they are acknowledged
	uint8_t probeState;		// RTT probe: 0 none, 1 draining (the gates are closed), 2 the marker is out, waiting for its ACK
	uint32_t probeDeadline, probeSentAt, lastProbeEnd;
	uint64_t ackedCum;		// bytes covered by the last accepted ACK
	uint64_t nextOrd, ackedOrd;	// ordinal of the next position (from 1) and of the last acknowledged one
	uint8_t watchOn, watchDone;	// the ACK of one position (the last key frame) is remembered with its times, for the throughput of that key frame
	uint32_t watchSeq, watchTs, watchAckAt, watchQueueMs, watchMinRtt;
	uint32_t tbTime, tokensMilli;	// ACK token bucket (thousandths of a token)
	uint32_t nHist, histStart;
	uint32_t nSamples, sampleHead;
	kvm_stream_pos ring[KVM_STREAM_RING];
	kvm_stream_hist hist[KVM_STREAM_HIST];
	kvm_stream_sample samples[KVM_STREAM_SAMPLES];
	kvm_stream_stats st;
} kvm_stream;

void kvm_stream_init(kvm_stream *s, int capVideo, int capAck, int videoOffReason, int ackOffReason);

// 242 for the start of the process (reason 11, no video, no ACK)
void kvm_stream_start_state(const kvm_stream *s, kvm_state_msg *reply);

// A 240 that passed kvm_stream_parse_ctrl. Updates the state (5.2). Returns 1 when *reply must be sent as 242 (always after the state change, before the next frame);
// *actions (may be NULL) gets KVM_ACT_* for the video code.
int kvm_stream_on_ctrl(kvm_stream *s, const kvm_ctrl *c, uint32_t now, kvm_state_msg *reply, unsigned *actions);

// The adaptive path (picture type JPEG / WebP, WA-2 active) can or cannot use the window now (5.1: capAck depends on it). Returns 1 when *reply must be sent as 242:
// reason 6 and ACK off when it cannot (the viewer has to ask again), reason 10 when it can again.
int kvm_stream_set_ack_capable(kvm_stream *s, int capable, uint32_t now, kvm_state_msg *reply);

// A 241 that passed kvm_stream_parse_ack, with the time it arrived. Returns KVM_ACK_*.
int kvm_stream_on_ack(kvm_stream *s, const kvm_ack *a, uint32_t now);

// Periodic (every loop iteration): the stall and the 15 s timeout. blocked = the agent's write is blocked (pause); the timers do not run then and
// a time without data in flight never counts. Returns 1 when *reply must be sent as 242 (reason 9).
int kvm_stream_tick(kvm_stream *s, uint32_t now, int blocked, kvm_state_msg *reply);

// A write to the pipe was blocked until `to` (the pipe was full: the viewer or the link did not take the data). The timers of the window did not run for that time.
void kvm_stream_note_blocked(kvm_stream *s, uint32_t to);

// Every byte written to the pipe by anybody (also the 242 and the markers) goes through here, in the order of the writes.
void kvm_stream_note_write(kvm_stream *s, size_t len, int kind, uint32_t now);

// A position (a marker or a video frame): takes the next seq, remembers (seq, ts = now, bytes written + msgBytes) in the ring. The caller writes the message and
// then calls kvm_stream_note_write(msgBytes, KVM_W_POSITION). Clears the "dirty" flag (the bytes written so far are covered).
uint32_t kvm_stream_add_position(kvm_stream *s, uint32_t now, size_t msgBytes);

// Rule M1 / M2: 1 when a marker must be written now. *probe (may be NULL): 0 = marker of rule M1, 1 = the probe marker (data in flight, no ACK for KVM_STREAM_PROBE_MS),
// 2 = the marker of an RTT probe (the pipe has drained after the gates were closed; its ACK refreshes minRtt).
// kvm_stream_make_marker adds the position and builds the 28 bytes; it counts the marker in the statistics.
int kvm_stream_marker_wanted(const kvm_stream *s, uint32_t now, int blocked, int *probe);
void kvm_stream_make_marker(kvm_stream *s, uint32_t now, int probe, uint8_t out[KVM_MARKER_BYTES]);

// Gates (8.2). kind: KVM_GATE_*. 1 = the write may go now. Always 1 while ACK is not on.
#define KVM_GATE_BIG 0		// big JPEG rectangles
#define KVM_GATE_SMALL 1	// rectangles up to KVM_STREAM_SMALL_TILES tiles
#define KVM_GATE_SHARPEN 2	// sharpening
#define KVM_GATE_VIDEO 3	// a video frame (also an IDR bigger than W: it goes when inflight < W, so always when nothing is in flight)
int kvm_stream_window_allows(const kvm_stream *s, int kind, uint32_t now);
// bytes that are on their way to the pipe (a frame that is being encoded). The gates of the JPEG and the sharpening count them (not the gate of the video itself: the frame that is being
// encoded is the one it would let in). 0 clears.
void kvm_stream_set_reserved(kvm_stream *s, uint32_t bytes);
// A gate refused a write that the caller wanted to make: the sender was limited by the window, not by its own lack of data (the rate samples use this).
void kvm_stream_note_held_back(kvm_stream *s, uint32_t now);
// The same, and the refusal was one of a video frame or of a big JPEG rectangle outside the RTT probe: the rate control reads this one (kvm_stream_link_limited). The sharpening gate
// (0.5 W) and the closed gates of the probe say nothing about the capacity of the link, they only feed the rate samples (note_held_back).
int kvm_stream_note_link_held(kvm_stream *s, uint32_t now);

// The state of the window, for the regulator and the log
int kvm_stream_ack_active(const kvm_stream *s);
uint64_t kvm_stream_inflight(const kvm_stream *s);
uint32_t kvm_stream_window(const kvm_stream *s, uint32_t now);
uint64_t kvm_stream_rate(const kvm_stream *s, uint32_t now);	// bytes per second, 0 = no sample in the last KVM_STREAM_RATE_SPAN_MS
// 1 when the window refused a video frame or a big JPEG rectangle (kvm_stream_note_link_held) in the last KVM_STREAM_HELD_MS: the sender is limited by the link, so the measured rate says what
// the link takes (else it only says what was sent)
// Remember the ACK of the position `seq` (a key frame just written): when the viewer acknowledges it (not as part of a later ACK that came first), kvm_stream_watch_result gives the times once.
void kvm_stream_watch(kvm_stream *s, uint32_t seq);
// 1 once after the ACK of the watched position: *ackAt = the time the agent got it, *ts = the time stamp of the position (when it was written), *queueMs = how long the viewer held it, *minRtt = the network round trip
int kvm_stream_watch_result(kvm_stream *s, uint32_t *ackAt, uint32_t *ts, uint32_t *queueMs, uint32_t *minRtt);
int kvm_stream_link_limited(const kvm_stream *s, uint32_t now);
uint64_t kvm_stream_rate_or_mem(const kvm_stream *s, uint32_t now);	// the same, but the last measured rate while there is no sample (a pause in the traffic is not a slow link); what the window is sized with
// What the sharpening takes its share of (the bucket of kvm_sharp, the forced packets): the samples of the last 2 s, else the remembered rate: the rate that was delivered, not the highest one seen (// the memory is for the window and R; a peak that it keeps must not enlarge the share that the sharpening may take while the video moves)
uint64_t kvm_stream_rate_recent(const kvm_stream *s, uint32_t now);

// The generation changes (chapter 3.4); the caller sends the 242 that the change requires.
void kvm_stream_bump_gen(kvm_stream *s);
// The 242 that describes the state now (reason as given)
void kvm_stream_state_msg(const kvm_stream *s, int reason, kvm_state_msg *out);

// ---- framing of the stdin of the KVM process (the viewer's side of the byte stream) ----
// The only framing in that stream is the size field of every command (a read of the pipe can end anywhere). For a 240 or 241 the number of bytes to consume is the size
// field, as for every other command (kvm_stdin.h: a size below 4 or above the input buffer consumes the 4 bytes of the header); a message that is not exactly 16 bytes
// is not delivered.
// Returns 0 when more data is needed, else the bytes to consume; *deliver = 1 when the message is a candidate for kvm_stream_parse_ctrl / _ack.
size_t kvm_stream_stdin_consume(const uint8_t *b, size_t blocklen, int *deliver);

// ---- inbox of the 240 commands: input thread -> main loop ----
// The input thread puts every valid 240 in the inbox, the main loop takes the lot once per iteration. Nothing in the inbox can be lost for lack of room (it used to be a queue of 32
// that refused what came after it, so a flood of op 3 could push out the op 5 that meshcore sends when a second viewer joins):
//   - the veto (op 5, op 6) is a state: vetoTarget is the set of sources that hold the veto after all commands that came so far, in their order. vetoPulse remembers the sources that
//     were set since the last take, even when they were cleared again, so that a set followed by a clear still stops the video and the ACK as it did when every command was handled
//     in turn. The main loop makes the veto of the stream equal vetoTarget before it handles any other command (kvm_inbox_expand);
//   - op 1, 2, 4 (what the viewer wants) keep only the last one; op 3 (repair) is a set of flags that is ORed until the main loop takes it;
//   - ordinary commands (op 1-4) pass a token bucket (KVM_INBOX_RATE / KVM_INBOX_BURST): over the rate they are dropped and counted. The veto does not pass it: it comes from meshcore
//     or the multiplexer, not from the viewer, and one command costs nothing.
// Not thread safe, the caller takes one lock for put and take. Time is the injected millisecond counter.
typedef struct kvm_inbox
{
	uint8_t vetoTarget, vetoPulse;	// KVM_VETO_* sources
	uint32_t vetoAt;				// arrival of the last op 5 / op 6
	uint8_t haveState;				// the last op 1, 2 or 4 below
	kvm_ctrl state;
	uint32_t stateAt;
	uint8_t repairFlags;			// KVM_REPAIR_* ORed since the last take
	uint32_t repairAt;				// arrival of the first op 3 of those
	uint32_t tbTime, tokensMilli;	// token bucket for op 1-4 (thousandths of a token)
	uint32_t nMerged, nLimited;		// ordinary commands that replaced / joined a waiting one, and ones dropped by the bucket (counted for the whole session)
} kvm_inbox;
#define KVM_INBOX_STORED 0			// kept (possibly joined with a waiting command)
#define KVM_INBOX_LIMITED 1			// ordinary command over the rate: dropped
void kvm_inbox_init(kvm_inbox *q, uint32_t now);
int kvm_inbox_put(kvm_inbox *q, const kvm_ctrl *c, uint32_t at);	// c passed kvm_stream_parse_ctrl
// Moves everything that waits into *batch (a copy of the whole inbox) and empties the waiting part; vetoTarget, the bucket and the counters stay.
void kvm_inbox_take(kvm_inbox *q, kvm_inbox *batch);
typedef struct kvm_inbox_op
{
	kvm_ctrl c;
	uint32_t at;
} kvm_inbox_op;
#define KVM_INBOX_OPS 4
// The commands to hand to kvm_stream_on_ctrl, in this order: op 5 (sources to set), op 6 (sources to clear), the last op 1 / 2 / 4, op 3 with the joined flags. curVeto is the veto
// of the stream now; after op 5 and op 6 it equals the target of the batch, also when the stream was reset behind the inbox. Returns the number of commands (0..KVM_INBOX_OPS).
size_t kvm_inbox_expand(const kvm_inbox *batch, uint8_t curVeto, kvm_inbox_op out[KVM_INBOX_OPS]);

// The parent process passes the commands to the child and keeps the veto sources (op 5 sets, op 6 clears the bits of flags) to give them to a new child. The tracker
// follows the byte stream by the size fields across writes, so a command cut between two writes is seen whole. it also keeps the last command 5 (compression: quality,
// scale, frame timer, budget) and the last command 13 (frame timer) that passed, because a new child (UAC, lock screen, a change of desktop) starts with the defaults and the viewer
// does not send them again: without them the session would fall back to the default frame rate, quality and budget of the agent while the viewer shows what it chose.
#define KVM_CMD_COMPRESSION 5			// MNG_KVM_COMPRESSION (meshcore/meshdefines.h)
#define KVM_CMD_FRAME_RATE 13			// MNG_KVM_FRAME_RATE_TIMER
typedef struct kvm_veto_tracker
{
	uint8_t mask;
	uint8_t buf[KVM_CTRL_SIZE];
	uint8_t have;		// bytes of the current header / command in buf
	uint8_t collect;	// 0 = the header is being collected, else the length of the command that is (a 240 of 16 bytes, a 5 or a 13 of 6 to 16 bytes)
	uint32_t skip;		// bytes of the current command still to pass
	uint8_t cmd5[KVM_CTRL_SIZE], len5;		// the last command 5 (0 = none yet)
	uint8_t cmd13[KVM_CTRL_SIZE], len13;	// the last command 13
	uint8_t rateLast;	// 1 = the command 13 came after the command 5 (the timer of the later one is in force)
} kvm_veto_tracker;
void kvm_veto_track(kvm_veto_tracker *t, const uint8_t *b, size_t len);
// The remembered commands 5 and 13 in the order in which they came, for a new child (at most 2 x KVM_CTRL_SIZE bytes into `out`); returns their length. The veto goes first, as op 5 of command 240.
size_t kvm_veto_replay(const kvm_veto_tracker *t, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif // KVM_STREAM_H
