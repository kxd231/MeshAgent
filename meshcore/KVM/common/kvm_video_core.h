/*
KVM: the portable core of the H.264 video path of the agent. Portable C99, no operating system API, no encoder: time is an injected
millisecond counter (uint32_t, wraps), so the tests are deterministic. The Windows glue (Windows/kvm_video.cpp, kvm.c, tile.cpp, kvm_adapt.cpp) calls
these functions and does the things that need the system: the encoder, the pipe, the pixels. The specification is docs/kvm-dev/h264-protocol.md of the
MeshCentral fork (chapters 3, 5, 8 and 9); the numbers in comments refer to it.

What is here:
  - the tile bookkeeping (kvm_vtiles): every tile is in exactly one place, NONE (the viewer has what the agent sent as JPEG, or nothing is known),
    PENDING (changed in the motion class, waits for the next frame: pendingVideo) or VIDEO (painted by a frame, blurred until it is sharpened). The
    transitions are the invariants I2 and I3 (a tile is in one place; a frame paints exactly the tiles it covers; sending a tile as JPEG takes it off the
    pending set), I7 (video off: the pending tiles go back to JPEG) and I8 (repair: the VIDEO tiles go back to JPEG);
  - the rectangles of a frame (kvm_vtiles_make_rects): tile aligned, inside the screen, at most 256 (neighbours are merged into bounding boxes);
  - the level (60 frames per second, 30 as the fallback) and the profile for the viewer's limits (5.2);
  - the controller kvm_vctl: the IDR rules (5.3), the repair and encoder creation limits, the error policy (5.2), the generation and the 242 that goes with
    it, the plan for the next frame and the check of the first frame of a generation.

Threading: none of this is thread safe; the glue calls it from the main loop of the KVM process only.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef KVM_VIDEO_CORE_H
#define KVM_VIDEO_CORE_H

#include <stddef.h>
#include <stdint.h>
#include "kvm_stream.h"
#include "kvm_annexb.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- tunables (T): /D<NAME>=<value> ----
#ifndef KVM_VIDEO_QP
#define KVM_VIDEO_QP 28					// constant quantizer of the frames until the regulator exists
#endif
#ifndef KVM_VIDEO_QP_MIN
#define KVM_VIDEO_QP_MIN 26				// the quantizer never goes below this: the text is limited by the 4:2:0 chroma, not by the QP, and the frames get big
#endif
#ifndef KVM_VIDEO_QP_MAX
#define KVM_VIDEO_QP_MAX 38				// the ceiling of the P frames (the regulator, 8.5: below it the frame rate is decided by the byte bucket and the window)
#endif
#ifndef KVM_VIDEO_OP1_COALESCE_MS
#define KVM_VIDEO_OP1_COALESCE_MS 2000	// an op 1 within this time of the previous accepted one is joined with it (5.2)
#endif
#ifndef KVM_VIDEO_OPEN_MIN_MS
#define KVM_VIDEO_OPEN_MIN_MS 2000		// the encoder is created at most once in this time (5.3)
#endif
#ifndef KVM_VIDEO_ERROR_RETRY_MS
#define KVM_VIDEO_ERROR_RETRY_MS 60000	// after an error of the encoder the video stays off this long (5.2)
#endif
#ifndef KVM_VIDEO_MAX_ERRORS
#define KVM_VIDEO_MAX_ERRORS 3			// the third error switches the video off until the process exits
#endif
#ifndef KVM_VIDEO_BROKEN_MAX
#define KVM_VIDEO_BROKEN_MAX 3			// lost access units in a row (the encoder is flushed and restarted with an IDR each time) before it counts as an error
#endif
#ifndef KVM_VIDEO_REPAIR_MIN_MS
#define KVM_VIDEO_REPAIR_MIN_MS 1000	// the VIDEO tiles are sent again as JPEG at most once in this time (I8)
#endif
#ifndef KVM_VIDEO_IDR_MIN_MS
#define KVM_VIDEO_IDR_MIN_MS 1000		// 5.3: max(this, 2 * IDR bytes / rate)
#endif
#ifndef KVM_VIDEO_IDR_MAX_MS
#define KVM_VIDEO_IDR_MAX_MS 10000		// (not in the specification) the spacing never grows beyond this: a very slow link must not wait for a key frame for minutes
#endif
#ifndef KVM_VIDEO_EARLY_PCT
#define KVM_VIDEO_EARLY_PCT 25			// a frame may come this many percent of the frame interval early (the loop period does not divide the interval)
#endif
#ifndef KVM_VIDEO_WRITE_GAP_PCT
#define KVM_VIDEO_WRITE_GAP_PCT 75		// (T) a frame is also not encoded before its write would come this many percent of the frame interval after the write of the previous one (a frame that was late is not followed by one right behind it)
#endif
#ifndef KVM_VIDEO_MAX_FPS
#define KVM_VIDEO_MAX_FPS 60			// the agent never encodes more often (5.2)
#endif
#ifndef KVM_VIDEO_SHARPEN_WAIT_MS
#define KVM_VIDEO_SHARPEN_WAIT_MS 1000	// with video on, tiles that have been due for sharpening this long are sharpened even when the window, the byte bucket or the token limit say no (it must not starve)
#endif
#ifndef KVM_VIDEO_SHARPEN_FORCE_MAX
#define KVM_VIDEO_SHARPEN_FORCE_MAX 16384	// forced sharpening: a packet of at most this many bytes ...
#endif
#ifndef KVM_VIDEO_SHARPEN_FORCE_MIN
#define KVM_VIDEO_SHARPEN_FORCE_MIN 4096	// ... at least this many (the interval grows instead of the share)
#endif
#ifndef KVM_VIDEO_SHARPEN_FORCE_MS
#define KVM_VIDEO_SHARPEN_FORCE_MS 250	// ... at most once in this time
#endif
#ifndef KVM_VIDEO_SHARPEN_SHARE_PCT
#define KVM_VIDEO_SHARPEN_SHARE_PCT 20	// the forced sharpening never uses more than this share of the delivery rate (8.5: 20 % of the link stays for sharpening)
#endif
#ifndef KVM_VIDEO_SCALE_RETURN_MS
#define KVM_VIDEO_SCALE_RETURN_MS 10000	// while the video runs the scale of the picture may go back to the viewer's, at the earliest this long after the last generation began (it is a new generation)
#endif

// ---- tile bookkeeping ----
#define KVM_VT_NONE 0
#define KVM_VT_PENDING 1
#define KVM_VT_VIDEO 2
#define KVM_VT_TILE 32

typedef struct kvm_vtiles
{
	int rows, cols;
	uint8_t *st;			// rows * cols states: what the tile is (or will be, once nothing is in flight): NONE, PENDING or VIDEO
	uint8_t *fl;			// rows * cols masks of the frames in flight that cover the tile (bit k = the job in slot k; the encoder thread INFLIGHT)
	unsigned nPending, nVideo, nInflight;	// tiles in each state; nInflight = tiles with a non-zero mask
	unsigned jpegOnInflight;		// violations of the rule about frames in flight that the glue made (a JPEG of a tile that a frame in flight covers): the tests expect 0
} kvm_vtiles;

int kvm_vtiles_init(kvm_vtiles *t, int rows, int cols);	// 0, or -1 (arguments, memory). Everything NONE.
void kvm_vtiles_free(kvm_vtiles *t);
void kvm_vtiles_reset(kvm_vtiles *t);						// everything NONE
int kvm_vtiles_get(const kvm_vtiles *t, int row, int col);	// KVM_VT_*, NONE outside the grid
// The rectangle of tiles [row0..row1] x [col0..col1] was changed in the motion class: its tiles are PENDING (I2: a VIDEO tile that moves again becomes PENDING)
void kvm_vtiles_claim(kvm_vtiles *t, int row0, int col0, int row1, int col1);
// A tile was sent as JPEG (a changed tile of the full class, a repair, a sharpening): it is NONE, whatever it was (I3)
void kvm_vtiles_jpeg(kvm_vtiles *t, int row, int col);
// I7: the pending tiles go back to JPEG. map (rows * cols bytes) gets 1 for every tile that was PENDING (others 0); returns their number. VIDEO tiles stay.
unsigned kvm_vtiles_take_pending(kvm_vtiles *t, uint8_t *map);
// I8 / a REFRESH: the VIDEO tiles (and with all = 1 the pending ones too) go back to JPEG: map gets 1 for each, they are NONE; returns their number.
unsigned kvm_vtiles_take_video(kvm_vtiles *t, uint8_t *map, int all);
// The rectangles of the next frame: tile aligned, x + w <= screenW, y + h <= screenH, at most KVM_FRAME_MAX_RECTS (consecutive rectangles are merged into their bounding box,
// at growing slack, until they fit). rects gets x, y, w, h (uint16_t); cover (rows * cols bytes) gets 1 for every tile inside a rectangle (so a superset of the PENDING tiles).
// Returns the number of rectangles (0 = nothing is pending), -1 for bad arguments (the grid does not match the screen) or memory.
int kvm_vtiles_make_rects(const kvm_vtiles *t, unsigned screenW, unsigned screenH, uint16_t rects[KVM_FRAME_MAX_RECTS * 4], uint8_t *cover);
// The frame with the cover was written: every covered tile is VIDEO (and no longer pending), I3. Returns the number of tiles that were not VIDEO before. (The synchronous path: dispatch and land of slot 0 in one call.)
unsigned kvm_vtiles_commit(kvm_vtiles *t, const uint8_t *cover);

// ---- INFLIGHT: a frame is encoded by the thread of the encoder while the loop goes on ----
// A tile has a base state (NONE, PENDING, VIDEO: kvm_vtiles_get) and a mask of the frames in flight that cover it. F = VIDEO with a mask, FP = PENDING with a mask (changed again after the frame was
// started). The rules :
//   claim: any base state -> PENDING, the mask stays (FP when it is not 0);
//   dispatch: the frame of the cover is started in slot k: covered tiles -> VIDEO (they are painted by the frame, so no longer pending) with bit k in the mask;
//   land: the frame was written: bit k is cleared, the base state stays (VIDEO, or PENDING when the tile changed meanwhile: the next frame paints it);
//   abort: the access unit was lost and the generation goes on (the next frame is an IDR): bit k cleared, VIDEO -> PENDING (the next frame paints the tile again);
//   cancel: the frame is given up (stop, new generation, error, watchdog): bit cleared, the tile is NONE and in map (the caller sends it as JPEG);
//   a JPEG for a tile with a mask is a violation of that rule (the caller waited for the frame first): counted in jpegOnInflight, the tile is NONE as always.
// take_pending and take_video leave the tiles with a mask alone: the caller made the frames land (or cancelled them) before.
void kvm_vtiles_dispatch(kvm_vtiles *t, const uint8_t *cover, int slot);
unsigned kvm_vtiles_land(kvm_vtiles *t, int slot, uint8_t *map);	// map (or NULL) gets 1 for every tile of the frame; returns their number
unsigned kvm_vtiles_abort(kvm_vtiles *t, int slot);
unsigned kvm_vtiles_cancel(kvm_vtiles *t, int slot, uint8_t *map);	// slot -1: every frame in flight
int kvm_vtiles_inflight(const kvm_vtiles *t, int row, int col);	// 1: a frame in flight covers the tile
int kvm_vtiles_rect_inflight(const kvm_vtiles *t, int row0, int col0, int row1, int col1);	// 1: it covers one tile of the rectangle

// ---- levels and profiles (5.2) ----
// Level for a picture of the coded size: the smallest level_idc (30 31 32 40 42 50 51 52) whose MaxFS holds the picture and whose MaxMBPS holds it at 60 frames per
// second. When that level is above the viewer's maxLevelIdc (0 = no limit), or none exists, the level for 30 frames per second is taken and the rate is limited to
// MaxMBPS / macroblocks (*fpsCap, at most KVM_VIDEO_MAX_FPS); if that does not fit either, or the picture is bigger than 36864 macroblocks: -1 (242 reason 7).
int kvm_vcore_pick_level(unsigned codedW, unsigned codedH, int viewerMaxLevel, int *level, int *fpsCap);
// The highest profile_idc (100 High, 77 Main, 66 Constrained Baseline) that the flags of op 1 allow and that is below `below` (use 101 for the first try); 0 = none
int kvm_vcore_pick_profile(unsigned flags, int below);
// The quantizer: the wanted value, never below KVM_VIDEO_QP_MIN or above KVM_VIDEO_QP_MAX
int kvm_vcore_clamp_qp(int qp);
// the QP of the hardware encoder for a QP of the regulator. The regulator, the key frame rule and the detector think in the scale of the software encoder (26 .. 38); the AMD encoder is coarser: its
// "QP 30" looks like QP 34 of x264 (PSNR, SSIM), so the QP that it is given is `qp + KVM_HW_QP_OFFSET`, never below KVM_HW_QP_FLOOR (it is thought to clamp what is below 26: the QP 20
// and QP 30 of the probe differed by 5 % in the size). The regulator has its floor at KVM_VIDEO_QP_MIN - KVM_HW_QP_OFFSET for this encoder (kvm_vrc_set_backend), so that no step of it is wasted
// above the floor. To be confirmed on a real picture .
// THE SCALE: only the QP that is GIVEN to the encoder (set_qp) is the mapped one. Everything that remembers a QP (kvm_vrc_frame, lastIdrQp, the QP of the detector's events, the log) uses the QP of the REGULATOR:
// the prediction of the next key frame (kvm_vrc, 0.96 per QP) is a function of the scale it was measured in; remembering the encoder's QP would shift it by 4 steps, about 28 %.
#ifndef KVM_HW_QP_OFFSET
#define KVM_HW_QP_OFFSET (-4)
#endif
#ifndef KVM_HW_QP_FLOOR
#define KVM_HW_QP_FLOOR 26
#endif
int kvm_vcore_hw_qp(int regulatorQp);
// Does the SPS that the encoder wrote fit the viewer's request and the protocol (3.4)? 0 = yes, else the reason of the 242: KVM_REASON_PROFILE (profile not in the viewer's flags, Constrained
// Baseline without constraint_set1, more than one reference frame, not 4:2:0 8 bit, not frame_mbs_only) or KVM_REASON_RESOLUTION (level above the viewer's maxLevelIdc (0 = no limit) or 5.2,
// more than 36 864 macroblocks, frame cropping, a size other than codedW x codedH).
int kvm_vcore_check_sps(const struct kvm_sps_info *s, unsigned profFlags, int viewerMaxLevel, unsigned codedW, unsigned codedH);
// Forced sharpening: 1 when a packet may be forced now, *bytes = its size. Not above 2 x the window (inflight >= 2 W: 0); the packet is
// clamp(KVM_VIDEO_SHARPEN_SHARE_PCT/100 x rate x KVM_VIDEO_SHARPEN_FORCE_MS, MIN, MAX) and the next one waits until the share is kept, so the long run never exceeds that share of the rate
// (rate 0 = not measured: the minimum packet, once a second). the share is taken of min(rate, viewer's budget in kbit/s x 125), 0 = no budget.
int kvm_vcore_force_sharpen(uint64_t rate, unsigned budgetKbps, uint64_t inflight, uint64_t window, int haveLast, uint32_t lastAt, uint32_t now, uint32_t *bytes);
// The scale ladder (WA-2b) wants to change the scale of the picture: with the video running or waiting for its encoder only the return towards the viewer's scale is allowed (a ladder that
// went down while the video was off must not keep the picture small for the whole video session), and not sooner than KVM_VIDEO_SCALE_RETURN_MS after the last generation began (every
// change is a new generation). Steps down are not allowed (v1, plan B.3). 1 = the change may be made.
int kvm_vcore_scale_allowed(int videoActive, int down, uint32_t msSinceGen);
// The highest bit rate (bits per second) that the level allows for the profile (Table A-1, MaxBR: 1000 bit/s units, 1250 for High): the bit rate of the stage 2 of the QP detector is never above it
// (a rate above it would make the encoder raise the level, and the viewer's SPS check would refuse the key frame). Levels below 3.0 and unknown ones: the limit of 3.0.
uint32_t kvm_vcore_max_bitrate(int levelIdc, int profileIdc);
// IDR spacing (5.3): max(KVM_VIDEO_IDR_MIN_MS, 2 * IDR bytes * 1000 / rate), at most KVM_VIDEO_IDR_MAX_MS; rate 0 (not measured) = KVM_VIDEO_IDR_MIN_MS
uint32_t kvm_vcore_idr_spacing(uint32_t lastIdrBytes, uint64_t rate);

// ---- the controller ----
#define KVM_VS_OFF 0		// no video
#define KVM_VS_WAIT 1		// the viewer wants video; the encoder is not ready (creation limit, new size)
#define KVM_VS_ON 2			// the encoder runs

typedef struct kvm_vctl
{
	uint8_t capVideo;		// 0 for good after the third error or a fault
	uint8_t faulted;		// a structured exception: nothing of the encoder may be touched any more
	uint8_t state;			// KVM_VS_*
	// what the viewer asked for (the last accepted op 1)
	uint8_t profFlags, maxLevel;
	uint16_t maxW, maxH;
	uint8_t nxFlags, nxLevel, nxValid;	// the limits of an op 1 that was joined with a start under way or running: applied by the next start (a new generation), not before
	uint16_t nxW, nxH;
	// op 1 and the creation of the encoder
	uint8_t haveOp1, haveOpen;
	uint32_t lastOp1, lastOpen;
	// errors
	uint8_t inError, errorCount, brokenRun;
	uint32_t errorUntil;
	// IDR
	uint8_t idrFirst;		// the first frame of the generation must be an IDR: never delayed
	uint8_t idrForce;		// an IDR at once (a lost access unit): not delayed
	uint8_t idrReq;			// op 3 with the IDR flag: after the spacing
	uint8_t encFresh;		// the encoder was opened for this generation: its first frame is an IDR by itself
	uint8_t haveIdr;
	uint32_t lastIdr, lastIdrBytes;
	// repair of the VIDEO tiles (I8)
	uint8_t repairReq, haveRepair;
	uint32_t lastRepair;
	// pace
	uint8_t haveFrame;
	uint32_t lastFrame;		// the slot of the last frame (kvm_pace_slot), not the time it was written
	uint8_t haveWrite;		// the time the last frame was written and the encode time to expect for the next one
	uint32_t lastWrite, encMs;
	uint8_t encThreaded;	// the encoder runs in a thread of its own: the next frame is written no sooner than the whole encode time after its start (the write gap does not cap the encode time at half the interval)
	// counters
	uint32_t frames, idrs, forcedIdrs, brokenFrames, errors, coalesced, repairs, gates;
} kvm_vctl;

void kvm_vctl_init(kvm_vctl *c, int capVideo);

// An op 1 that the stream layer accepted (KVM_ACT_VIDEO_REQUEST). Remembers the viewer's limits.
#define KVM_VOP1_START 0		// begin a generation now
#define KVM_VOP1_COALESCE 1		// the previous accepted op 1 was less than KVM_VIDEO_OP1_COALESCE_MS ago and its start succeeded or is under way (state not OFF): answer with the current state, nothing else
int kvm_vctl_op1(kvm_vctl *c, const kvm_ctrl *op, uint32_t now);
// Remember the limits of an op 1 without starting anything (the op 1 of the hold-off after an error: the retry uses them).
void kvm_vctl_limits(kvm_vctl *c, const kvm_ctrl *op);
// A start begins: the limits of a joined op 1 become the ones in force (safe option: a joined op 1 with other flags changes nothing until the next generation).
void kvm_vctl_apply_limits(kvm_vctl *c);
// The pace allows a frame now (the frame interval since the last one, 25 % early allowed, and the write of this frame at least KVM_VIDEO_WRITE_GAP_PCT of the interval after the write of the last one):
// also what a picture is wanted for.
int kvm_vctl_pace_ok(const kvm_vctl *c, uint32_t now, uint32_t intervalMs);
// a frame was written at `writeAt`, and a frame takes `encMs` of encoding (the average). The slot of kvm_vctl_pace_ok says when the next frame is due; this says that it also may not
// come right behind a frame that was late (after a window that was closed, a long capture): the frames of such a burst were 0-25 ms apart and the next gap was long.
void kvm_vctl_note_write(kvm_vctl *c, uint32_t writeAt, uint32_t encMs);
// 1 when the encoder is in a thread of its own. The frame that is started right after the write of the last one is written only an encode time later, so the write gap takes the whole encode time of
// the average (at most one interval), not at most half of it: with the cap a frame that could go right behind the landing of the last one was held back for a few ms the loop did not wake for.
void kvm_vctl_set_threaded(kvm_vctl *c, int threaded);
// Milliseconds until the write gap allows a frame (0 = it does): the frame that is started at `now` is written about encMs later.
uint32_t kvm_vctl_write_wait_ms(const kvm_vctl *c, uint32_t now, uint32_t intervalMs);
// An op 3 while the video is on (the actions of kvm_stream_on_ctrl): the IDR is asked for (and done after the spacing), the repair is asked for (done at most once a second)
void kvm_vctl_op3(kvm_vctl *c, unsigned actions);

// The encoder may be created now (at most once in KVM_VIDEO_OPEN_MIN_MS). kvm_vctl_open_note() says that it was (successfully or not).
int kvm_vctl_open_allowed(const kvm_vctl *c, uint32_t now);
void kvm_vctl_open_note(kvm_vctl *c, uint32_t now);

// The encoder is open: a new generation starts. The stream's generation is incremented, the stream says that video is on, the first frame will be an IDR, and *m is the 242
// that has to go out before that frame (video 1, ack 1, reason 0, the profile and level of the SPS, the coded size, the new generation). Nothing is written here.
// encFresh = the encoder was just opened (not reused for a new generation). Returns 0, or -1 (and changes nothing) when the stream layer has no ACK window or the video
// is not possible: a 242 with video 1 and ack 0 does not exist (3.3).
int kvm_vctl_begin_gen(kvm_vctl *c, kvm_stream *s, int profile, int level, unsigned codedW, unsigned codedH, int encFresh, kvm_state_msg *m);
// The video is not running any more (the stream layer or the glue switched it off). The viewer's request stays in the stream layer.
void kvm_vctl_stop(kvm_vctl *c);

// An error of the encoder (or a frame bigger than 8 MiB): the video is off. Returns 1 when the glue retries after KVM_VIDEO_ERROR_RETRY_MS, 0 when this was the last
// error (capVideo = 0). kvm_vctl_retry_due() is 1 once when the time has come and the video may start again (with a new generation).
int kvm_vctl_error(kvm_vctl *c, uint32_t now);
int kvm_vctl_retry_due(kvm_vctl *c, uint32_t now);
// A structured exception in a call of the encoder: capVideo = 0 for the rest of the process, nothing may be closed or shut down (the glue leaks the encoder).
void kvm_vctl_fault(kvm_vctl *c);

// The plan for the next frame (called when a picture is at hand)
typedef struct kvm_vplan_in
{
	uint32_t now;
	unsigned pending;		// tiles in pendingVideo
	int gateOpen;			// the window allows a video frame (KVM_GATE_VIDEO)
	int rateOpen;			// the byte bucket of the rate control (kvm_vrc) holds something; a first frame of a generation and a key frame after a lost access unit do not wait for it
	uint32_t intervalMs;	// 1000 / frame rate (0 = no limit)
	uint64_t rate;			// delivery rate of the link, bytes per second (0 = not measured)
} kvm_vplan_in;
typedef struct kvm_vplan
{
	int encode;				// encode the picture now
	int forceIdr;			// ... and make the frame an IDR (call the encoder's force before)
	int held;				// nothing was done because the window is closed (the glue notes it as "held back")
	int heldRate;			// nothing was done because the byte bucket of the rate control is empty (the frame comes later and carries what accumulated)
} kvm_vplan;
void kvm_vctl_plan(const kvm_vctl *c, const kvm_stream *s, const kvm_vplan_in *in, kvm_vplan *out);

// The encoder gave an access unit of `bytes` bytes (isIdr: it contains an IDR slice). Returns 0 = send it, KVM_VF_BROKEN = do not send it: the first frame of a generation
// has to be an IDR and this one is not (the glue forces an IDR and the tiles stay pending). KVM_VF_ERROR = the same KVM_VIDEO_BROKEN_MAX times in a row: an error of the encoder.
#define KVM_VF_BROKEN 1
#define KVM_VF_ERROR 2
int kvm_vctl_frame_done(kvm_vctl *c, int isIdr, size_t bytes, uint32_t now);
// Why does this frame become (or why did the encoder make) an IDR?. Asked BEFORE kvm_vctl_frame_done clears the requests, with the plan of the frame.
#define KVM_IDR_WHY_NONE 0		// nothing asked for one: an IDR that comes out of the encoder now is the encoder's own doing
#define KVM_IDR_WHY_FIRST 1		// the first frame of a generation
#define KVM_IDR_WHY_BROKEN 2	// after a lost access unit (idrForce)
#define KVM_IDR_WHY_OP3 3		// op 3 with the IDR flag (the viewer, or the recording of the server: the protocol does not say which)
int kvm_vctl_idr_why(const kvm_vctl *c, const kvm_vplan *plan);
const char *kvm_idr_why_name(int why, int isIdr);		// "first", "broken", "op3", "encoder" (an IDR that nobody asked for), "none" (not an IDR)
// The encoder lost the access unit of a frame (the wrapper flushed it; its next frame is an IDR) or it was bigger than 8 MiB. Returns KVM_VB_RETRY (the next frame is an
// IDR, the tiles stay pending) or KVM_VB_ERROR (oversize, or too many in a row: treat as an error of the encoder).
#define KVM_VB_RETRY 0
#define KVM_VB_ERROR 1
int kvm_vctl_frame_broken(kvm_vctl *c, int oversize);
// Repair (I8): 1 when the VIDEO tiles must be sent again as JPEG now (consumes the request). kvm_vctl_repair_now() is the op 1 case (the repair is part of it).
int kvm_vctl_repair_due(kvm_vctl *c, uint32_t now);
void kvm_vctl_repair_now(kvm_vctl *c, uint32_t now);

#ifdef __cplusplus
}
#endif

#endif // KVM_VIDEO_CORE_H
