// KVM (Windows): the H.264 video path of the agent. See kvm_video.h.
//
// The rules are those of docs/kvm-dev/h264-protocol.md of the MeshCentral fork (chapter 5.2 for the states, 5.3 for the key frames, 5.4 for what goes as video and what as JPEG,
// 8.2 for the window, 9 for the invariants); the numbers in the comments refer to it. The decisions that need no operating system are in ../common/kvm_video_core.c and are tested in
// Docker; this file is the glue: the encoder (mf_h264.cpp), the tiles of tile.cpp / kvm_adapt.cpp, the pipe (kvm.c), the log.
//
// The main loop thread does everything except the encoder: the encoder (mf_h264.cpp, or another backend, kvm_encthr.h) runs on a thread of its own. A frame is a JOB: the main loop plans it, takes the
// tiles of it out of the model (dispatch: INFLIGHT, kvm_video_core.h), hands the picture over, and goes on with the next picture; the result is collected (kvm_video_service, the barriers) and only then written to the pipe
// and committed. Only the main loop touches the tiles, the stream state (kvm_stream, only while kvm_glue_lock() is held), the rate control and the pipe; the encoder thread touches the picture (read only) and its own
// buffers. MESH_KVM_ENC_THREAD=0: the job is made in the call that hands it over (the behaviour before the encoder thread).

#include "tile.h"
#include "kvm_video.h"

#ifdef KVM_VIDEO_ENABLED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <timeapi.h>
#include <time.h>
#include "mf_h264.h"
#include "kvm_encthr.h"
#include "kvm_encbe_mfhw.h"
#include "kvm_hwtest.h"
#include "../common/kvm_hwpolicy.h"
#include "../common/kvm_fbuf.h"
#include "../common/kvm_video_core.h"
#include "../common/kvm_video_rc.h"
#include "../common/kvm_pace.h"
#include "../common/kvm_auto.h"
#include "../common/kvm_qpdet.h"
#include "../common/kvm_nv12view.h"
#include "../common/kvm_annexb.h"

extern "C"
{
extern int SCALED_WIDTH;
extern int SCALED_HEIGHT;
extern int TILE_WIDTH_COUNT;
extern int TILE_HEIGHT_COUNT;
extern int PIXEL_SIZE;
extern int FRAME_RATE_TIMER;
extern tileInfo_t **tileInfo;
extern volatile LONG gKvmFullFrame; // kvm.c: the next captured frame has to check every tile
}

#define VID_MAX_REQ 4
#define VID_DEPTH 1					// frames in flight (kvm_encjob: up to KVM_EJ_MAX; the tiles carry a mask of the slots)
#ifndef KVM_ENC_WATCHDOG_MS
#define KVM_ENC_WATCHDOG_MS 2000	// a frame that has been in the encoder this long: the encoder hangs; typically 10-25 ms, a 4K frame ~150 ms; 2 s so that a paused VM or a woken laptop is not taken for a hang
#endif
#ifndef KVM_ENC_CALL_MS
#define KVM_ENC_CALL_MS 3000	// open and the other calls on the encoder thread
#endif
#define VID_MAX_AU (KVM_FRAME_MAX_N - (4u * 1024u))	// an access unit above this would make a 243 bigger than the 8 MiB the protocol allows (N = 20 + 8 * rects + data)

enum { START_NONE = 0, START_OP1 = 1, START_GEOMETRY = 2, START_RETRY = 3, START_BACKEND = 4 };	// START_BACKEND: a new generation on another encoder backend (the self test has passed, or the hardware encoder was given up)
#define KVM_HW_EXIT_CODE 0x4857	// the exit code of a KVM child that ends itself after an exception in the driver of the hardware encoder (the counter is written first; not a status of a crash)

// One frame in the encoder (a slot of kvm_encjob): everything the main loop needs when the result comes back, and what the encoder thread must not touch
typedef struct vslot
{
	uint8_t *cover;						// rows * cols: the tiles of the frame (kvm_vtiles_make_rects)
	int32_t *paintCrc;					// rows * cols: contentCrc of the tiles when the frame was started (what the frame shows)
	uint8_t *frame;						// 243 buffer: header + rectangles + Annex B (the encoder thread writes the access unit behind the header)
	size_t frameCap;
	void *buf;							// the picture of the job (kvm_fbuf, one reference of the job)
	uint16_t rects[KVM_FRAME_MAX_RECTS * 4];
	int nRects;
	size_t hdr;
	DWORD at, slotAt;					// the moment the frame was planned (the clock of its slot), and its slot (kvm_pace_slot)
	int hadFrame; uint32_t prevSlot;
	int idrWhy, qpUsed;
	uint32_t coverTiles, intervalMs;
	unsigned long long rate;
	unsigned estBytes;					// the access unit that was reserved in the window for it
	unsigned virginMbs, realMbs, totalMbs;	// the macroblocks of the view that this frame converts: all of them, and those that had never been converted since the flat key frame
	int viewFull, viewFlat;
} vslot;

static struct
{
	int sessionOn, mfInit, capReason;
	kvm_vctl ctl;
	kvm_vtiles tiles;
	uint8_t *map;						// rows * cols scratch
	vslot sl[VID_DEPTH];
	void *held;							// the newest picture of the loop (a reference) that no frame has been started with yet: it waits for the encoder, the window, the bucket or its slot
	long long heldSize;
	int heldCodedW, heldCodedH;
	int screenW, screenH, codedW, codedH;
	int encW, encH, encLevel;			// what the open encoder was created for
	unsigned encFlags;					// ... and the profile flags of the request
	struct mfh264_info info;			// get_info of the open encoder (read on its thread at open)
	uint8_t seq[1024]; size_t seqLen;	// its sequence header (SPS + PPS), copied at open
	int heldBusy;						// gV.held waits for the encoder (it was refused for that and no other reason): only such a picture is started by the service of the loop
	unsigned long long inlineCpuUs;		// the inline mode: the CPU of the encoder in the call, as before the encoder thread
	const kvm_encbe *be;				// the encoder backend behind the thread (kvm_encbe_mf; the hardware encoder brings another)
	int haveEnc;						// an encoder is open (the handle itself is the thread's: kvm_encthr_handle())
	int profile, level, fpsCap, qp;	// the generation that runs (profile and level as the SPS of the encoder says)
	int64_t ts;							// presentation time of the frames given to the encoder, 100 ns (counts on while the encoder lives)
	kvm_vrc rc;							// rate control: target R, QP regulator, byte bucket
	int appliedQp;						// the QP the encoder runs with (0 = not set yet): the QP of the last job that carried one
	DWORD genAt, rcLog;					// when the last generation began (or was asked for); the last "video rc" line
	unsigned long long sharpLast;		// sharpening bytes at the last "video rc" line
	unsigned encAvgUs;					// average encode time of a frame (moving average): the frame interval is at least that x the cores the encoder keeps busy / its CPU budget (kvm_pace.h)
	unsigned avgAu;						// average size of a 243 (moving average): what a frame in flight is reserved in the window
	kvm_budget budget;					// the CPU budget of the encoder (50 % of the machine, at least one core) and the parallelism of the encoder as measured
	kvm_auto au;						// the Auto mode of the frame rate (bit 3 of the flags of op 1): the interval of the frames, see kvm_auto.h. Nothing is done while the bit is down
	int autoOk;							// MESH_KVM_AUTO=0 switches Auto off for the process: no caps bit in the 242, the bit of op 1 is ignored
	kvm_late slotLate;					// ... the lateness of the video frames against their slots (the period of the "video pace" line)
	unsigned long long cpuLast, nonEncCpuLast, cpuWinLast;	// ... CPU time of the process at the last "video pace" line; and what the other threads (main, capture) had used at the last window of the budget
	// where the key frames come from
	uint32_t idrWatchBytes;				// the last key frame that the viewer has not acknowledged yet (its position is watched in the stream layer): the rate control measures what the link took of it
	DWORD op3At, op3Prev, lastFrameAt, lastIdrAt;	// the last op 3 with the IDR flag and the one before it; the last encoded frame and IDR (0 = none)
	unsigned op3Count; unsigned op3Flags;
	// does the encoder listen to the QP? stage 0: the QP through the codec API, 1: also in the input sample, 2: the bit rate (kvm_vrc_set_bitrate_mode); 3: nothing helps (the QP stays)
	kvm_qpdet qpdet;
	int qpStage, qpRestart, qpEffLogged;
	int appliedBps, bitrateTry; DWORD bpsAt;
	int autoWant;						// the bit 3 of the last op 1 (Auto wanted): what the agent's Auto returns to when the hardware encoder in stage 2 gives way to the software one
	kvm_ctrl req[VID_MAX_REQ];			// op 1 that the stream layer accepted, to be answered
	unsigned reqAt[VID_MAX_REQ];
	int nreq;
	int startPending, startKind;
	int encMode;						// KVM_ENCTHR_THREADED, or KVM_ENCTHR_INLINE (MESH_KVM_ENC_THREAD=0)
	// the hardware encoder. The backend of the encoder thread is chosen for every new encoder (hw_choose): the hardware one only after a self test of this machine has passed.
	struct
	{
		int env, mode;					// KVM_HWP_ENV_* (MESH_KVM_VIDEO_HW), KVM_HWP_MODE_* (the file of the mode, read for every generation)
		int active;						// the thread runs the hardware backend (and the encoder that is open, or is made, is the hardware one)
		int disabled;					// an error of the hardware encoder in this process: the software one for the rest of it
		DWORD capWaitAt, switchSince, lastRecheck;	// since when the choice of the encoder waits for the capture to be known (0 = not waiting); since when a switch to the hardware encoder is due; the last look at a capture that became DXGI
		int capGaveUp, lastCap;			// the wait has run out (once per process); the state of the capture at the last look (kvm_capture_state)
		int testDone, switchPending;	// a self test of this process has finished; it passed while the software encoder runs: the hardware one comes at the next quiet moment
		int tryOpen, tryUsed;			// the marker "the hardware is in use by this process" is written and not cleared yet (until the hardware is closed in an orderly way, given up, or the session ends); tryUsed: its first access unit was out
		uint32_t flatMilli;				// the floor of a flat key frame of the hardware encoder (the entry of the cache, from the self test); 0 = unknown
		int selDone, selFound; int64_t selLuid;	// the enumeration of the encoders has been done in this process for this adapter (once: on a thread of its own, with a time limit); and what it found (hw.sel)
		char selKey[KVM_HWP_KEY_MAX]; int selKeyOk;	// the key of the entry "the enumeration hangs on this machine"
		int rebuild, bypassOpen;		// the next generation makes a new encoder although the size is the same; ... and is not held back by the limit of one open in 2 s (the return to the software encoder)
		int qpForm;						// the form of the QP that the self test found (MFH264_QP_*)
		int brTried;						// the hardware encoder with the QP form was opened again in the bit rate form once in this process
		int unaskedIdr;					// key frames of the hardware encoder that nobody asked for (a periodic GOP); the second one gives it up
		int br, brTry, brRun;			// the encoder is steered by the bit rate (form 8 / 9 of the entry): the first mode to try (0 CBR, 1 LDVBR); the hardware encoder of this generation ran so (the return to the software one starts from stage 0)
		int64_t luid;
		kvm_hwtest_run run;
		char key[KVM_HWP_KEY_MAX];		// the key of the cache for this machine, this size (valid while keyOk)
		int keyOk;
		kvm_mfhw_sel sel;
		char why[24];					// why this backend (kvm_hwp_out.why)
	} hw;
	// the NV12 view of the encoder (kvm_nv12view.h): A = frozen outside the rectangles with a complete key frame, B = flat key frame (v.flat)
	struct { kvm_nv12v v; int on, flat, firstFull, ok; } view;
	volatile LONG refreshSeq;
	LONG seenRefresh;
	// statistics of the period (kvm_video_stats_flush)
	struct
	{
		long long frames, idr, bytes, encUs, encUsMax, rects, covered, held, heldRate, broken, errors, starts, fallbackTiles, claimedTiles, spsMismatch, pendingMax, idrBytes, writeUs, skippedPace;
		long long convUs, convMbs, convFrames, convComplete, convFlat, virginMbs;	// the conversion of the view
		long long busyHeld, lateStarts, barJpeg, barRefresh, barRepair, barOther, barUs, barUsMax, cancelled, staleGen, hung;	// the picture waited for the encoder; the frame started from the held picture (not in emit); the barriers and the time the loop waited in them; results given up
	} st;
	long long totalFrames;
} gV;

static LARGE_INTEGER gQpcFreq;
static double qpc_us(const LARGE_INTEGER &a, const LARGE_INTEGER &b)
{
	if (gQpcFreq.QuadPart == 0 && !QueryPerformanceFrequency(&gQpcFreq)) { gQpcFreq.QuadPart = 1; }
	return (double)(b.QuadPart - a.QuadPart) * 1000000.0 / (double)gQpcFreq.QuadPart;
}

typedef struct vid_view { int want, veto, ackActive, videoOn, adaptOk, linkLimited; uint64_t rate; uint32_t gen; } vid_view;

static void get_view(vid_view *v, DWORD now)
{
	const kvm_stream *s;
	kvm_glue_lock();
	s = kvm_glue_stream();
	v->want = s->want; v->veto = s->vetoMask; v->ackActive = s->ackActive; v->videoOn = s->videoOn; v->adaptOk = s->adaptOk; v->linkLimited = kvm_stream_link_limited(s, now); v->rate = kvm_stream_rate_or_mem(s, now); v->gen = s->gen;
	kvm_glue_unlock();
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// tiles
// ---------------------------------------------------------------------------------------------------------------------------------------

// The tiles of the map go back to JPEG: the viewer's copy is no longer trusted, so crc 0xFF (as after a REFRESH) and the next captured frame checks all tiles
static void tiles_to_jpeg(const uint8_t *map)
{
	int r, c;
	if (tileInfo == NULL || gV.tiles.st == NULL || gV.tiles.rows != TILE_HEIGHT_COUNT || gV.tiles.cols != TILE_WIDTH_COUNT) { return; }
	for (r = 0; r < gV.tiles.rows; r++)
	{
		for (c = 0; c < gV.tiles.cols; c++)
		{
			if (map[((size_t)r * (size_t)gV.tiles.cols) + (size_t)c]) { tileInfo[r][c].crc = 0xFF; }
		}
	}
	InterlockedExchange(&gKvmFullFrame, 1);
}

// I7: the video is off; the tiles that waited for a frame are sent as JPEG in the next iteration. The VIDEO tiles stay what they are (sharpening handles them).
static void pending_to_jpeg(void)
{
	unsigned n;
	if (gV.tiles.st == NULL || gV.map == NULL || gV.tiles.nPending == 0) { return; }
	n = kvm_vtiles_take_pending(&gV.tiles, gV.map);
	if (n > 0) { tiles_to_jpeg(gV.map); gV.st.fallbackTiles += n; }
}

// The tiles painted by a frame that has been written: `map` has the tiles of the frame (kvm_vtiles_land). A tile that changed again meanwhile keeps the state of that change: it is pending (the
// next frame paints it), or its content is not the one that the frame showed (a change that was held back, WA-2b: its crc is already the new one and its `deferred` flag is the only thing that
// remembers it): nothing of that is touched. What is touched is the tile that the frame showed: it has been "changed" now (so the sharpening waits for the settle time and does not interleave with the
// next frames), the viewer's copy is the content that was encoded (crc = contentCrc, unless a REFRESH reset it meanwhile), and its quality is below every quality the user can ask for.
static void tiles_painted(const uint8_t *map, const vslot *sl, DWORD now)
{
	int r, c;
	if (tileInfo == NULL || gV.tiles.rows != TILE_HEIGHT_COUNT || gV.tiles.cols != TILE_WIDTH_COUNT) { return; }
	for (r = 0; r < gV.tiles.rows; r++)
	{
		for (c = 0; c < gV.tiles.cols; c++)
		{
			tileInfo_t *t;
			const size_t i = ((size_t)r * (size_t)gV.tiles.cols) + (size_t)c;
			if (!map[i]) { continue; }
			t = &tileInfo[r][c];
			if (kvm_vtiles_get(&gV.tiles, r, c) == KVM_VT_PENDING || t->contentCrc != sl->paintCrc[i]) { continue; }	// it changed after the frame was started: the change is what counts now
			t->lastChange = now;
			if (t->crc != 0xFF) { t->crc = t->contentCrc; }
			t->sentQ = 1; t->sentSub = 1;
			t->refresh = 0; t->deferred = 0; t->deferredAt = 0;
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the encoder (on its thread: kvm_encthr.h)
// ---------------------------------------------------------------------------------------------------------------------------------------

// R as the bit rate of the encoder, but never above what the level of the stream allows (kvm_vcore_max_bitrate): a higher rate would make the encoder raise its level
static uint32_t bitrate_for_level(void)
{
	const uint32_t want = kvm_vrc_bitrate_bps(&gV.rc), mx = kvm_vcore_max_bitrate(gV.level, gV.profile);
	return want > mx ? mx : want;
}

static int have_encoder(void) { return gV.haveEnc && kvm_encthr_handle() != NULL; }

static void free_frames(void)
{
	int k;
	for (k = 0; k < VID_DEPTH; k++) { free(gV.sl[k].frame); gV.sl[k].frame = NULL; gV.sl[k].frameCap = 0; }
}

static void release_held(void)
{
	if (gV.held != NULL) { kvm_fbuf_release(gV.held); gV.held = NULL; }
	gV.heldBusy = 0;
}

static void release_slot(vslot *sl)
{
	if (sl->buf != NULL) { kvm_fbuf_release(sl->buf); sl->buf = NULL; }
}

static int collect_one(void);
static void hw_fallback(const char *why, int weight);
static void hw_die(const char *what);
static void encoder_hung(const char *where);
static void barrier(long long *counter);

// The CPU time of the process during an encode call, less what the capture thread used meanwhile: what the encoder and its worker threads used (the inline mode, as before the encoder thread). 0 when the clock is not readable.
static unsigned long long enc_cpu_us(unsigned long long cpu0, unsigned long long cpu1, unsigned long long cap0, unsigned long long cap1)
{
	unsigned long long d, c = 0;
	if (cpu0 == 0 || cpu1 < cpu0) { return 0; }
	d = cpu1 - cpu0;
	if (cap0 != 0 && cap1 >= cap0) { c = cap1 - cap0; }
	return d > c ? d - c : 0ull;
}

// The jobs in flight are given up (stop, a new generation, an error, the end of the session): their tiles go back to JPEG (what follows is a new generation or the close of the encoder, so no
// access unit of them is ever missing in the viewer's reference), their results are discarded when they come, and the picture that waits for a frame is dropped. Does not wait.
// the view of the encoder thread is no longer what the plan of the loop assumes (a frame was given up, lost or failed before it was converted or without being written): the next frame is complete (A) or flat (B)
static void view_lost(void) { if (gV.view.ok) { kvm_nv12v_invalidate(&gV.view.v); } }
static int env_flag(const char *name, int def)
{
	char v[8] = "";
	if (GetEnvironmentVariableA(name, v, sizeof(v)) > 0) { return v[0] == '1' ? 1 : (v[0] == '0' ? 0 : def); }
	return def;
}
// a new encoder (a new coded size, or after a close): its view is made again, empty (the first frame is a key frame, complete or flat)
static void view_reset(int cw, int ch)
{
	kvm_nv12v_free(&gV.view.v);
	gV.view.ok = 0;
	if (gV.view.on && kvm_nv12v_init(&gV.view.v, cw, ch, 1, gV.view.flat, gV.view.firstFull) == 0) { gV.view.ok = 1; }
}

static void cancel_inflight(void)
{
	view_lost();
	if (gV.tiles.st != NULL && gV.map != NULL && gV.tiles.nInflight > 0)
	{
		unsigned n;
		memset(gV.map, 0, (size_t)gV.tiles.rows * (size_t)gV.tiles.cols);
		n = kvm_vtiles_cancel(&gV.tiles, -1, gV.map);
		if (n > 0) { tiles_to_jpeg(gV.map); gV.st.fallbackTiles += n; gV.st.cancelled++; }
	}
	if (kvm_encthr_cancel_all() > 0)
	{
		gV.st.cancelled++;
		gV.appliedQp = 0; gV.appliedBps = 0;	// a job that was given up before the thread took it did not set its QP / bit rate: the next frame sets them again
	}
	kvm_glue_lock(); kvm_stream_set_reserved(kvm_glue_stream(), 0); kvm_glue_unlock();
	release_held();
}

// What the thread of the encoder runs at open (and what the main loop reads back afterwards). The data is static: a call that is given up (the encoder hangs) may still write it.
struct OpenCall
{
	// in
	int cw, ch, level, qpStage, bitrateTry, qp;
	int hw, qpForm;						// the hardware encoder: the form of the QP that the self test found; the adapter of the captured output
	int64_t luid;
	unsigned profFlags;
	uint32_t bitrate;
	// out
	int rc, qpRc, profile;
	struct mfh264_err err, qpErr;
	struct mfh264_info info;
	size_t maxAu, seqLen;
	uint8_t seq[1024];
};
// (one record per open, on the heap: an open that hangs and is given up leaks its record, and the open on another backend that follows has its own: 11)

// ON THE ENCODER THREAD: a new encoder for the coded size. Profiles are tried from the highest the viewer allows down; the level was computed by the caller.
static void open_on_thread(kvm_encctx *c, void *arg)
{
	OpenCall *o = (OpenCall *)arg;
	int below = 101, last = MFH264_E_OUTTYPE;
	o->rc = last; o->qpRc = 0; o->profile = 0; o->maxAu = 0; o->seqLen = 0;
	memset(&o->err, 0, sizeof(o->err)); memset(&o->qpErr, 0, sizeof(o->qpErr)); memset(&o->info, 0, sizeof(o->info));
	for (;;)
	{
		const int p = kvm_vcore_pick_profile(o->profFlags, below);
		mfh264_cfg cfg = MFH264_CFG_INIT(o->cw, o->ch);
		struct mfh264 *h;
		if (p == 0) { o->rc = last; return; }
		cfg.profile = p;
		if (o->hw) { cfg.rc = MFH264_RC_QUALITY; cfg.qp_method = o->qpForm; cfg.hw_luid = o->luid; }	// the QP only as the self test found it (VT_UI4 on the AMD encoder), never the bit rate
		if (o->qpStage == 2)
		{
			// the QP is not listened to: the bit rate steers. The mode is the first that this encoder accepts: CBR, LowDelayVBR, PeakConstrainedVBR
			cfg.rc = o->bitrateTry == 0 ? MFH264_RC_CBR : (o->bitrateTry == 1 ? MFH264_RC_LDVBR : MFH264_RC_PEAKVBR);
			cfg.qp_method = MFH264_QP_NONE;
			cfg.bitrate = (int)o->bitrate;
			{ const uint32_t mx = kvm_vcore_max_bitrate(o->level, p); if ((uint32_t)cfg.bitrate > mx) { cfg.bitrate = (int)mx; } }	// never above what the level allows
		}
		cfg.max_level_idc = o->level;
		cfg.level_idc = o->level;
		cfg.max_au_size = (int)VID_MAX_AU;
		memset(&o->err, 0, sizeof(o->err));
		h = c->be->open(&cfg, &o->err);
		if (h != NULL)
		{
			const uint8_t *seq = NULL;
			c->enc = h;
			o->profile = p;
			o->maxAu = c->be->max_au_size(h);
			c->be->get_info(h, &o->info);
			o->seqLen = c->be->seq_header(h, &seq);
			if (seq == NULL || o->seqLen > sizeof(o->seq)) { o->seqLen = 0; } else { memcpy(o->seq, seq, o->seqLen); }
			o->rc = 0;
			if (o->qpStage != 2)
			{
				o->qpRc = c->be->set_qp(h, o->qp, &o->qpErr);
				if (o->qpRc == 0 && o->qpStage == 1) { (void)c->be->set_sample_qp(h, 1, 0); }
			}
			return;
		}
		last = o->err.code;
		if (o->err.code != MFH264_E_OUTTYPE) { o->rc = o->err.code; return; }
		below = p; // the encoder refused this profile: the next one down
	}
}

static void close_on_thread(kvm_encctx *c, void *arg)
{
	(void)arg;
	if (c->enc != NULL) { c->be->close(c->enc); c->enc = NULL; }	// after a fault this does nothing (the encoder is leaked on purpose)
}

static void sample_qp_on_thread(kvm_encctx *c, void *arg)
{
	int *rc = (int *)arg;
	*rc = (c->enc != NULL) ? c->be->set_sample_qp(c->enc, 1, 0) : -1;
}

// The encoder is closed. The jobs go first (the call runs behind them on the thread); frame buffers are freed unless the encoder hangs (then they are leaked: the thread may still write them).
static void close_encoder(void)
{
	cancel_inflight();
	if (kvm_encthr_started() && gV.haveEnc)
	{
		if (kvm_encthr_call(close_on_thread, NULL, KVM_ENC_CALL_MS) != 0) { gV.haveEnc = 0; encoder_hung("close"); return; }
		while (collect_one()) { }	// the cancelled results are in by now: their buffers go back
	}
	gV.haveEnc = 0;
	free_frames();
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// stream layer: answers
// ---------------------------------------------------------------------------------------------------------------------------------------

static void send_current_state(int reason)
{
	kvm_state_msg m;
	const kvm_stream *s;
	kvm_glue_lock();
	s = kvm_glue_stream();
	kvm_stream_state_msg(s, reason, &m);
	if (m.video) { m.profile = (uint8_t)gV.profile; m.level = (uint8_t)gV.level; m.codedW = (uint16_t)gV.codedW; m.codedH = (uint16_t)gV.codedH; }
	kvm_glue_state_locked(&m);
	kvm_glue_unlock();
	kvm_glue_log_flush();
}

// The video cannot be had (any more) for this process: op 1 is answered with `reason` from now on
static void stream_cap_off(int reason)
{
	kvm_stream *s;
	kvm_glue_lock();
	s = kvm_glue_stream();
	s->capVideo = 0;
	s->videoOffReason = (uint8_t)reason;
	kvm_glue_unlock();
}

// The request cannot be met (5.2: reasons 2, 3, 7, 8): the video is off, ACK stays (want = ACK), and the viewer is told. cw / ch: the size that was refused (reason 7).
static void fail_request(int reason, int cw, int ch)
{
	kvm_state_msg m;
	kvm_stream *s;
	kvm_glue_lock();
	s = kvm_glue_stream();
	if (s->want == KVM_WANT_VIDEO) { s->want = KVM_WANT_ACK; }
	s->videoOn = 0;
	kvm_stream_state_msg(s, reason, &m);
	if (reason == KVM_REASON_RESOLUTION) { m.codedW = (uint16_t)cw; m.codedH = (uint16_t)ch; }
	kvm_glue_state_locked(&m);
	kvm_glue_unlock();
	kvm_glue_log_flush();
	kvm_vctl_stop(&gV.ctl);
	gV.startPending = 0; gV.startKind = START_NONE;
	close_encoder();						// (the frames in flight are given up first)
	pending_to_jpeg();
	kvm_stats_log("video request failed reason=%d coded=%dx%d", reason, cw, ch);
}

// The generation could not start for a reason that is not the request's (the stream layer changed its mind): nothing is sent, the stream layer has said what is needed
static void cancel_start(void)
{
	cancel_inflight();						// (the encoder stays: the next generation on it starts with a key frame)
	kvm_vctl_stop(&gV.ctl);
	gV.startPending = 0; gV.startKind = START_NONE;
	pending_to_jpeg();
}

// The video stops running (op 2, op 4, op 5, no ACK, the picture type, ...): the stream layer has answered with 242 already (videoOn = 0)
static void stop_video(const char *why)
{
	kvm_vctl_stop(&gV.ctl);
	gV.startPending = 0; gV.startKind = START_NONE;
	close_encoder();
	pending_to_jpeg();
	// (the VIDEO tiles of a lost sharpening path (5.2) are sent as JPEG again by kvm_video_poll, whether the video was running or already off)
	kvm_stats_log("video stop why=%s frames=%lld", why, gV.totalFrames);
}

// An error of the encoder, an exception or a frame that cannot be sent (5.2): the video is off for 60 s (after the third error, for good), 242 reason 4, the pending tiles go as JPEG.
// fault = a structured exception: the process never touches the encoder or Media Foundation again (nothing is closed, nothing is shut down).
static void video_error(const char *what, const struct mfh264_err *err, int fault)
{
	DWORD now = timeGetTime();
	int retry;
	kvm_state_msg m;
	kvm_stream *s;
	gV.st.errors++;
	kvm_stats_log("video error backend=%s structural=%d what=%s code=%s hr=0x%08lx stage=%s exc=0x%08lx errors=%d", gV.be != NULL ? gV.be->name : "-", err != NULL ? kvm_encthr_error_structural(err->code) : 0, what, err != NULL ? mfh264_result_name(err->code) : "-", err != NULL ? (unsigned long)err->hr : 0ul,
				  (err != NULL && err->stage != NULL) ? err->stage : "-", err != NULL ? err->exc_code : 0ul, (int)gV.ctl.errorCount + 1);
	if (gV.hw.active)
	{
		// an error of the HARDWARE encoder is not an error of the video: the software encoder takes over in a new generation (one return per process: the hardware one is off for the rest of it, and the counter in the
		// cache says so for the next processes). An exception in the driver ends this process after the counter is written. The error rules below (60 s of silence, the third error is the last) are the software encoder's.
		const int exc = fault || (err != NULL && err->code == MFH264_E_EXCEPTION);
		const int weight = exc ? 2 : 1;				// (a deadline of the backend that passed once is a weight of 1: the entry is tested again; the exception and the watchdog, a thread that did not return, are 2)
		if (exc) { hw_die(what); }
		hw_fallback(what, weight);
		return;
	}
	if (fault) { kvm_vctl_fault(&gV.ctl); retry = 0; } else { retry = kvm_vctl_error(&gV.ctl, now); }
	gV.startPending = 0; gV.startKind = START_NONE;
	kvm_glue_lock();
	s = kvm_glue_stream();
	s->videoOn = 0; // want stays VIDEO: the retry is the video code's
	if (!gV.ctl.capVideo) { s->capVideo = 0; s->videoOffReason = KVM_REASON_DISABLED; }
	kvm_stream_state_msg(s, gV.ctl.capVideo ? KVM_REASON_ERROR : KVM_REASON_DISABLED, &m); // reason 4 promises a retry (7.1); the 3rd error (or an exception) ends the video for the process: reason 6
	kvm_glue_state_locked(&m);
	kvm_glue_unlock();
	kvm_glue_log_flush();
	if (!fault) { close_encoder(); } else { cancel_inflight(); gV.haveEnc = 0; }	// after a fault nothing of the encoder is touched, its buffers are left where they are
	pending_to_jpeg();
	kvm_stats_log("video off retry=%d cap_video=%d", retry, (int)gV.ctl.capVideo);
}


// ---------------------------------------------------------------------------------------------------------------------------------------
// the hardware encoder: the choice of the backend, the self test in the background, the way back to the software encoder
// ---------------------------------------------------------------------------------------------------------------------------------------

static int desktop_is_default(void)
{
	char name[64] = "";
	DWORD n = 0;
	HDESK d = GetThreadDesktop(GetCurrentThreadId());
	if (d == NULL || !GetUserObjectInformationA(d, UOI_NAME, name, sizeof(name), &n)) { return 1; }	// (unknown: as if it were)
	{
		static const char *dflt = "default";
		size_t i;
		for (i = 0; dflt[i] != 0; i++) { const char ch = (name[i] >= 'A' && name[i] <= 'Z') ? (char)(name[i] + 32) : name[i]; if (ch != dflt[i]) { return 0; } }
		return name[i] == 0 ? 1 : 0;
	}
}

// the changes of the cache file (under its mutex): what the callbacks of kvm_hwcache_update do
struct CacheOp { int op; const char *key; int verdict, qpForm, weight; uint32_t openMs, p50, p95, idrKb, pid, flatMilli; };
enum { CO_TEST = 1, CO_ERROR = 2, CO_BEGIN = 3, CO_END = 4, CO_RECOVER = 5, CO_USED = 6, CO_FORM = 7 };
static uint32_t now_sec(void) { return (uint32_t)(time(NULL) & 0xFFFFFFFFu); }
static int cache_op(kvm_hwp_cache *c, void *arg)
{
	const CacheOp *o = (const CacheOp *)arg;
	switch (o->op)
	{
		case CO_TEST: kvm_hwp_note_test(c, o->key, o->verdict, o->qpForm, o->openMs, o->p50, o->p95, o->idrKb, now_sec()); if (o->flatMilli != 0) { kvm_hwp_note_flat(c, o->key, o->flatMilli); } return 1;
		case CO_ERROR: kvm_hwp_note_error(c, o->key, o->weight, now_sec()); if (c->trying && c->tryPid == o->pid) { kvm_hwp_end_try(c); } return 1;
		case CO_BEGIN: kvm_hwp_begin_try(c, o->key, o->pid, now_sec()); return 1;
		case CO_END: if (c->trying && c->tryPid == o->pid) { kvm_hwp_end_try(c); return 1; } return 0;
		case CO_RECOVER: return kvm_hwp_recover_try(c, kvm_hw_pid_alive(c->tryPid), now_sec());
		case CO_FORM: { kvm_hwp_entry *e = kvm_hwp_find(c, o->key); if (e == NULL) { return 0; } e->qpForm = (uint8_t)o->qpForm; return 1; }	// the form that the session found (8: the bit rate)
		case CO_USED: if (c->trying && c->tryPid == o->pid) { kvm_hwp_mark_used(c, o->pid); return 1; } return 0;
	}
	return 0;
}
static int cache_do(CacheOp *o) { o->pid = GetCurrentProcessId(); if (kvm_hwcache_update(cache_op, o) != 0) { kvm_stats_log("video hw cache not written op=%d", o->op); return -1; } return 0; }	// 0 = written

static void hw_end_try(void)
{
	CacheOp o;
	if (!gV.hw.tryOpen) { return; }
	gV.hw.tryOpen = 0; gV.hw.tryUsed = 0;
	memset(&o, 0, sizeof(o)); o.op = CO_END; o.key = gV.hw.key;
	cache_do(&o);
}

static void hw_log_backend(const char *why)
{
	const kvm_mfhw_sel &s = gV.hw.sel;
	kvm_stats_log("video backend be=%s why=%s hw_name=%s adapter=0x%04x:0x%04x drv=%s size_class=%d qp_form=%d mode=%d env=%d disabled=%d", gV.hw.active ? "hw" : "sw", why, gV.hw.keyOk ? s.name : "-", gV.hw.keyOk ? s.vendor : 0u, gV.hw.keyOk ? s.device : 0u,
				  gV.hw.keyOk ? s.driver : "-", kvm_hwp_size_class((unsigned)gV.codedW, (unsigned)gV.codedH), gV.hw.qpForm, gV.hw.mode, gV.hw.env, gV.hw.disabled);
	if (gV.hw.keyOk) { kvm_stats_log("video hw key=%s", gV.hw.key); }
}

// Which backend makes the next encoder, and why: the decision of kvm_hwpolicy on what is known (the switches, the system, the capture, the cache), and the self test in the background when it is due. 1 = the hardware encoder.
// The video never waits for the test: the software encoder runs meanwhile, the hardware one comes with a later generation (the test passed: hw_poll_test).
static int hw_choose(int cw, int ch, int level, int profile, int wasHw, int dry)
{
	kvm_hwp_in in;
	kvm_hwp_out out;
	kvm_hwp_cache cache;
	int haveCache;
	memset(&in, 0, sizeof(in));
	gV.hw.keyOk = 0;
	gV.hw.env = kvm_hwenv_read();
	gV.hw.mode = kvm_hwmode_read();										// every generation: "sw" in the mode file takes effect with the next one, not only with the next session
	in.env = gV.hw.env; in.mode = gV.hw.mode;
	in.osOk = 1;
	in.sizeOk = kvm_hwp_size_class((unsigned)cw, (unsigned)ch) != 0;
	in.displayActive = (kvm_capture_state() == 2) ? 1 : 0;	// (DXGI is open and the source: "not known yet" is not "the lid is closed", the caller waited for it)
	if (!dry) { gV.hw.lastCap = kvm_capture_state(); }						// (what this decision saw: a change of it asks for another decision, hw_recheck)
	in.desktopDefault = desktop_is_default();
	in.hwRunning = wasHw;								// (the hardware encoder that is closed for this new generation: it stays when the output is gone)
	in.processDisabled = gV.hw.disabled;
	in.testRunning = gV.hw.run.running;
	in.testDone = gV.hw.testDone;
	in.nowSec = now_sec();
	if (gV.encMode == KVM_ENCTHR_INLINE) { snprintf(gV.hw.why, sizeof(gV.hw.why), "inline_mode"); return 0; }	// the hardware encoder lives on the thread that never calls SetThreadDesktop: not in the inline mode (MESH_KVM_ENC_THREAD=0; the manual mode is the tests')
	// the cheap refusals first: nothing is enumerated for a switch that says no
	if (!kvm_encthr_started() || kvm_encthr_abandoned()) { snprintf(gV.hw.why, sizeof(gV.hw.why), "no_thread"); return 0; }
	if (in.env == KVM_HWP_ENV_OFF || in.mode == KVM_HWP_MODE_SW || in.processDisabled || !in.sizeOk) { in.hwPresent = 1; kvm_hwp_decide(&in, &out); snprintf(gV.hw.why, sizeof(gV.hw.why), "%s", out.why); return 0; }
	// nothing is enumerated when the hardware would not be started anyway: no active output (the lid is closed: GDI) and no hardware encoder that runs now
	if (!in.displayActive && !in.hwRunning) { in.hwPresent = 1; kvm_hwp_decide(&in, &out); snprintf(gV.hw.why, sizeof(gV.hw.why), "%s", out.why); return 0; }
	haveCache = (kvm_hwcache_read(&cache) == 0);
	if (!haveCache) { memset(&cache, 0, sizeof(cache)); }
	// an earlier process found that the enumeration of the encoders hangs on this machine (the entry of its own key, an error of weight 2 that expires like the others): the software encoder, no enumeration
	{
		unsigned ven = 0, dev = 0, sub = 0, arev = 0;
		gV.hw.selKeyOk = (kvm_dxgi_adapter_sig(&ven, &dev, &sub, &arev) && kvm_hwtest_select_key(ven, dev, sub, arev, gV.hw.selKey, sizeof(gV.hw.selKey))) ? 1 : 0;
		if (gV.hw.selKeyOk)
		{
			const kvm_hwp_entry *se = kvm_hwp_find(&cache, gV.hw.selKey);
			if (se != NULL && kvm_hwp_is_bad(se, in.nowSec) && in.env != KVM_HWP_ENV_FORCE && in.mode != KVM_HWP_MODE_HW) { snprintf(gV.hw.why, sizeof(gV.hw.why), "select_hung_before"); return 0; }
		}
	}
	// the encoders that there are for the adapter of the captured output: ONCE per process and adapter, on a thread of its own with a time limit (the thread of the video encoder, the software one too, never waits
	// for the driver's enumeration). A hang is an error of the machine: written under the key of that, the software encoder goes on.
	{
		const int64_t luid = kvm_dxgi_adapter_luid();
		if (!gV.hw.selDone || gV.hw.selLuid != luid)
		{
			const int r = kvm_hwtest_select(luid, &gV.hw.sel);
			if (r < 0)
			{
				CacheOp so;
				kvm_stats_log("video hw select hung: the enumeration of the encoders did not return in time; the software encoder, the hardware one is off in this process and written off for this machine");
				gV.hw.disabled = 1;
				if (gV.hw.selKeyOk) { memset(&so, 0, sizeof(so)); so.op = CO_ERROR; so.key = gV.hw.selKey; so.weight = 2; cache_do(&so); }
				snprintf(gV.hw.why, sizeof(gV.hw.why), "select_hung");
				return 0;
			}
			gV.hw.selDone = 1; gV.hw.selLuid = luid; gV.hw.selFound = (r == 1 && gV.hw.sel.found) ? 1 : 0;
		}
		in.hwPresent = gV.hw.selFound ? 1 : 0;
		if (gV.hw.selFound && kvm_hwtest_key_from_sel(&gV.hw.sel, (unsigned)cw, (unsigned)ch, gV.hw.key, sizeof(gV.hw.key)))
		{
			gV.hw.keyOk = 1; gV.hw.luid = gV.hw.sel.luid != 0 ? gV.hw.sel.luid : luid;
		}
	}
	in.entry = gV.hw.keyOk ? kvm_hwp_find(&cache, gV.hw.key) : NULL;
	kvm_hwp_decide(&in, &out);
	snprintf(gV.hw.why, sizeof(gV.hw.why), "%s", out.why);
	if (out.startTest && gV.hw.keyOk)
	{
		if (kvm_hwtest_start(&gV.hw.run, (unsigned)cw, (unsigned)ch, level, (int)gV.ctl.maxLevel, profile, gV.hw.luid, gV.hw.key) == 0) { kvm_stats_log("video hw self test started size=%dx%d level=%d luid=%lld", cw, ch, level, (long long)gV.hw.luid); }
		else { gV.hw.testDone = 1; kvm_stats_log("video hw self test could not be started"); snprintf(gV.hw.why, sizeof(gV.hw.why), "test_not_started"); }
	}
	if (in.entry != NULL) { gV.hw.flatMilli = in.entry->flatMilli; }
	if (out.useHardware && in.entry != NULL)
	{
		// the forms 8 and 9 of the entry say that this encoder ignores the QP and the bit rate steers it (stage 2 from the first frame); 8 = CBR, 9 = LowDelayVBR
		const int f = (int)in.entry->qpForm;
		gV.hw.br = (f == KVM_HWP_FORM_BR_CBR || f == KVM_HWP_FORM_BR_LDVBR) ? 1 : 0;
		gV.hw.brTry = f == KVM_HWP_FORM_BR_LDVBR ? 1 : 0;
		gV.hw.qpForm = (f != 0 && !gV.hw.br) ? f : (int)MFH264_QP_ENCODEQP_UI4;
	}
	if (out.useHardware && !gV.hw.tryOpen && !dry)
	{
		// the marker "the hardware is in use by this process": once, before the first open; it stays until the hardware is closed in an orderly way, given up, or the session ends (the parent reads it from the exit code).
		// NO MARKER, NO HARDWARE: when it cannot be written (the cache is read-only, locked, the disk is full) a crash of the driver would not be counted by anybody and the restarts of this process would never end.
		CacheOp mo;
		memset(&mo, 0, sizeof(mo)); mo.op = CO_BEGIN; mo.key = gV.hw.key;
		if (cache_do(&mo) != 0)
		{
			kvm_stats_log("video hw marker could not be written: the software encoder in this process");
			gV.hw.disabled = 1;
			snprintf(gV.hw.why, sizeof(gV.hw.why), "marker_write_failed");
			return 0;
		}
		gV.hw.tryOpen = 1; gV.hw.tryUsed = 0;
	}
	return out.useHardware ? 1 : 0;
}

// Called by kvm_video_poll while a self test runs: its result is judged (the key must be the one that the child worked out: the encoder or the adapter may have changed meanwhile), written to the cache, and when the
// hardware encoder has passed while the software one runs, the switch is armed (hw_poll_switch).
static int hw_verdict_cb(kvm_hwp_cache *c, void *arg) { return cache_op(c, arg); }
static void hw_poll_test(void)
{
	int verdict = 0;
	kvm_hwp_result res;
	CacheOp o;
	if (!gV.hw.run.running) { return; }
	memset(&res, 0, sizeof(res));
	if (!kvm_hwtest_poll(&gV.hw.run, &verdict, &res)) { return; }
	gV.hw.testDone = 1;
	if (verdict == KVM_HWP_V_PASS || verdict == KVM_HWP_V_FAIL || verdict == KVM_HWP_V_SLOW)
	{
		if (res.rev != KVM_HW_SELFTEST_REV || strcmp(res.key, gV.hw.key) != 0)
		{
			kvm_stats_log("video hw self test result dropped (rev %d, the key differs: %d)", res.rev, strcmp(res.key, gV.hw.key) != 0 ? 1 : 0);
			return;
		}
	}
	memset(&o, 0, sizeof(o));
	o.op = CO_TEST; o.key = gV.hw.key; o.verdict = verdict; o.qpForm = res.qpForm; o.openMs = res.openMs; o.p50 = res.p50Us; o.p95 = res.p95Us; o.idrKb = res.idrKb; o.flatMilli = res.flatMilli;
	(void)hw_verdict_cb;
	if (gV.hw.keyOk) { cache_do(&o); }
	kvm_stats_log("video hw self test verdict=%d rule=%s qp_form=%d open_ms=%u p50_us=%u p95_us=%u idr_kb=%u frames=%u flat_idr_milli_b_per_mb=%u", verdict, res.rule[0] ? res.rule : "-", res.qpForm, (unsigned)res.openMs, (unsigned)res.p50Us, (unsigned)res.p95Us, (unsigned)res.idrKb, (unsigned)res.frames, (unsigned)res.flatMilli);
	if (verdict == KVM_HWP_V_PASS && !gV.hw.active && !gV.hw.disabled) { gV.hw.switchPending = 1; gV.hw.switchSince = timeGetTime(); }
}

// The backend of the encoder thread is changed (the encoder is closed): a new thread with the other backend. The hardware one that does not start is no reason to be without video: the software one is started again.
static int switch_thread_backend(const kvm_encbe *be)
{
	struct mfh264_err err;
	int rc;
	memset(&err, 0, sizeof(err));
	rc = kvm_encthr_start(be, gV.encMode, VID_DEPTH, KVM_ENC_CALL_MS, &err);	// (stops the one that runs: its encoder is closed, its backend shut down, its thread joined or left to the process)
	if (rc != 0 && be != &kvm_encbe_mf)
	{
		kvm_stats_log("video hw backend did not start code=%s hr=0x%08lx stage=%s: software", mfh264_result_name(err.code), (unsigned long)err.hr, err.stage != NULL ? err.stage : "-");
		gV.hw.disabled = 1;
		snprintf(gV.hw.why, sizeof(gV.hw.why), "init_failed");
		be = &kvm_encbe_mf;
		memset(&err, 0, sizeof(err));
		rc = kvm_encthr_start(be, gV.encMode, VID_DEPTH, KVM_ENC_CALL_MS, &err);
	}
	gV.be = be;
	gV.hw.active = (rc == 0 && be == &kvm_encbe_mfhw) ? 1 : 0;
	gV.mfInit = (rc == 0) ? 1 : 0;
	return rc;
}

// The hardware encoder is given up (an error in a session: a deadline, a failed frame, three lost access units in a row, a QP that has no effect): the error is written (weight 2 for a hang or an exception: the entry is
// bad at once; 1 for the rest: the entry is tested again before the hardware is used again), the video goes on with the software encoder in a new generation (a key frame, the tiles as JPEG meanwhile), once,
// without waiting for the limit of one open in 2 s. The software encoder keeps the rules it had (one error is an error of the video, the third ends it).
static void hw_fallback(const char *why, int weight)
{
	CacheOp o;
	kvm_stats_log("video hw fallback why=%s weight=%d frames=%lld", why, weight, gV.totalFrames);
	gV.hw.disabled = 1;
	if (weight <= 0) { hw_end_try(); }		// no error of the hardware (a mismatch with the viewer, not a fault): only the marker goes
	else if (gV.hw.keyOk)
	{
		memset(&o, 0, sizeof(o)); o.op = CO_ERROR; o.key = gV.hw.key; o.weight = weight;
		gV.hw.tryOpen = 0; gV.hw.tryUsed = 0;
		cache_do(&o);
	}
	snprintf(gV.hw.why, sizeof(gV.hw.why), "fallback_%s", why);
	cancel_inflight();
	pending_to_jpeg();
	close_encoder();
	if (gV.ctl.faulted) { return; }
	gV.hw.bypassOpen = 1; gV.hw.rebuild = 1;
	gV.startPending = 1; gV.startKind = START_BACKEND; gV.ctl.state = KVM_VS_WAIT;
}

// The driver crashed in a call (a structured exception): the counter is written, then this KVM process ends. The service starts a new one (as after UAC), and the new one finds the entry bad and uses the software
// encoder. After a fault in a driver nothing of this process (its heap, its locks, Media Foundation, which the software encoder uses too) can be trusted.
static void hw_die(const char *what)
{
	CacheOp o;
	kvm_stats_log("video hw exception what=%s: the counter is written and this KVM process ends (exit code 0x%x)", what, KVM_HW_EXIT_CODE);
	if (gV.hw.keyOk) { memset(&o, 0, sizeof(o)); o.op = CO_ERROR; o.key = gV.hw.key; o.weight = 2; gV.hw.tryOpen = 0; gV.hw.tryUsed = 0; cache_do(&o); }
	kvm_glue_log_flush();
	TerminateProcess(GetCurrentProcess(), KVM_HW_EXIT_CODE);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// start of a generation (5.2: op 1, a new size, the retry after an error)
// ---------------------------------------------------------------------------------------------------------------------------------------

// The SPS of the encoder against the viewer's request and the protocol (3.4): nothing that the viewer would reject is ever sent. reason 0 = fine.
static int sps_reason(const uint8_t *p, size_t n, int cw, int ch, struct kvm_sps_info *info)
{
	if (kvm_annexb_sps_info(p, n, info) != 0) { return KVM_REASON_INIT; }
	return kvm_vcore_check_sps(info, gV.ctl.profFlags, gV.ctl.maxLevel, (unsigned)cw, (unsigned)ch);
}

// After open: the sequence header of the encoder. 1 = go on (also when the encoder gave none: the first IDR is checked again); 0 = the request was failed (reason 7 / 8) and the encoder closed.
static int check_open_sps(int cw, int ch)
{
	const uint8_t *hdr = gV.seq;
	size_t n = gV.seqLen;
	struct kvm_sps_info info;
	int reason;
	if (n == 0 || hdr == NULL) { return 1; }
	reason = sps_reason(hdr, n, cw, ch, &info);
	if (reason == 0) { return 1; }
	kvm_stats_log("video sps refused reason=%d profile=%d constraint=0x%02x level=%d refs=%d mbs=%dx%d cropping=%d flags=%u max_level=%d", reason, info.profile, info.constraint, info.level, info.numRefFrames, info.mbsW, info.mbsH, info.cropping, gV.ctl.profFlags, gV.ctl.maxLevel);
	fail_request(reason, cw, ch);
	return 0;
}

// the constants that two headers keep apart (kvm_nv12view.h does not know the tile bookkeeping of kvm_video_core.h or the slots of kvm_encjob.h) are the same ones; a change of one of them
// without the other would copy a plan into a smaller map, or read the states of the tiles with the wrong numbers
static_assert(KVM_NV12V_MB_MAX <= KVM_EJ_MB_MAX, "a plan has to fit into the map of a job (kvm_nv12view.h, kvm_encjob.h)");
static_assert(KVM_NV12V_VIDEO == KVM_VT_VIDEO, "the state VIDEO of the tiles (kvm_nv12view.h, kvm_video_core.h)");
static_assert(KVM_NV12V_TILE == KVM_VT_TILE, "the tile of the bookkeeping (kvm_nv12view.h, kvm_video_core.h)");

#ifndef KVM_NV12_FROZEN_DEFAULT
#define KVM_NV12_FROZEN_DEFAULT 1		// the NV12 view of the encoder is frozen outside the rectangles of a frame, the key frame is complete (MESH_KVM_NV12_FROZEN=0 switches it off)
#endif
#ifndef KVM_IDR_FLAT_DEFAULT
#define KVM_IDR_FLAT_DEFAULT 0			// a flat background at a key frame, the forecast by coverage, the first touch (MESH_KVM_IDR_FLAT=1 switches it on; needs A)
#endif
#ifndef KVM_IDR_FLAT_FIRST_FULL_DEFAULT
#define KVM_IDR_FLAT_FIRST_FULL_DEFAULT 1	// the first key frame of a session is complete (the proof of throughput of the first key frame)
#endif
#ifndef KVM_HW_CAP_WAIT_MS
#define KVM_HW_CAP_WAIT_MS 1000		// the choice of the encoder waits at most this long for the capture to be known ; the tiles go as JPEG meanwhile
#endif
#ifndef KVM_HW_SWITCH_DUE_MS
#define KVM_HW_SWITCH_DUE_MS 5000	// a switch to the hardware encoder that has not found a quiet moment this long is made anyway, as one more generation 
#endif

// the first request for video may come before the capture has opened DXGI (the log of the AMD laptop: op 1 at .346, "dxgi open" at .401): that is not a closed lid. The start of the video waits (at most
// KVM_HW_CAP_WAIT_MS, once per process) for the state of the capture to be known when the hardware encoder is a possibility at all (the cheap refusals first: they decide at once). A wait costs the start of the
// video at most that long (the JPEG covers it; the open DXGI came 55 ms later in the log); the alternative, to start on the software encoder and to switch when the capture is known, costs a generation
// (the key frame of the software encoder, then that of the hardware one, 380 KB on the AMD encoder) in every session of this machine.
static int hw_wait_capture(DWORD now)
{
	if (gV.hw.disabled || gV.hw.active || gV.hw.capGaveUp || gV.encMode == KVM_ENCTHR_INLINE || !kvm_encthr_started() || kvm_encthr_abandoned()) { return 0; }
	if (kvm_capture_state() != 0) { gV.hw.capWaitAt = 0; return 0; }
	if (kvm_hwenv_read() == KVM_HWP_ENV_OFF || kvm_hwmode_read() == KVM_HWP_MODE_SW || kvm_hwp_size_class((unsigned)gV.codedW, (unsigned)gV.codedH) == 0) { return 0; }
	if (gV.hw.capWaitAt == 0) { gV.hw.capWaitAt = now != 0 ? now : 1; kvm_stats_log("video hw choice waits for the capture to be known"); }
	if ((DWORD)(now - gV.hw.capWaitAt) >= KVM_HW_CAP_WAIT_MS) { gV.hw.capGaveUp = 1; gV.hw.capWaitAt = 0; kvm_stats_log("video hw choice: the capture is not known after %d ms, decided as it is", KVM_HW_CAP_WAIT_MS); return 0; }
	return 1;
}

// the capture became DXGI while the software encoder runs (the lid was opened: "capture mode=dxgi reason=recovered", or the first open came after the choice): the choice is made again. It is a decision, not a
// generation: the cheap refusals first (no enumeration when the answer is no), a self test may start, and only a "yes" arms the switch (which waits for a quiet moment, or for KVM_HW_SWITCH_DUE_MS).
static void hw_recheck(DWORD now)
{
	const int cap = kvm_capture_state();
	const int was = gV.hw.lastCap;
	gV.hw.lastCap = cap;
	if (cap != 2 || was == 2 || gV.hw.active || gV.hw.disabled || gV.hw.switchPending || gV.ctl.state != KVM_VS_ON || gV.screenW <= 0) { return; }
	if (gV.hw.lastRecheck != 0 && (DWORD)(now - gV.hw.lastRecheck) < 1000u) { return; }
	gV.hw.lastRecheck = now != 0 ? now : 1;
	{
		int level = 0, fpsCap = 0;
		const int profile = kvm_vcore_pick_profile(gV.ctl.profFlags, 101);
		if (profile == 0 || kvm_vcore_pick_level((unsigned)gV.codedW, (unsigned)gV.codedH, gV.ctl.maxLevel, &level, &fpsCap) != 0) { return; }
		if (hw_choose(gV.codedW, gV.codedH, level, profile, 0, 1))
		{
			gV.hw.switchPending = 1; gV.hw.switchSince = now != 0 ? now : 1;
			kvm_stats_log("video hw recheck: the capture is DXGI now and the hardware encoder is wanted: a switch is due");
		}
	}
}

static void attempt_start(DWORD now)
{
	vid_view v;
	int cw, ch, level = 0, fpsCap = 0, profile, reuse, r, fresh = 0;
	kvm_state_msg m;
	int ok;
	double openMs = 0.0;
	OpenCall *oc = NULL, o;
	LARGE_INTEGER q0, q1;

	get_view(&v, now);
	if (v.want != KVM_WANT_VIDEO || v.veto || !v.ackActive || !gV.ctl.capVideo) { cancel_start(); return; }
	kvm_vctl_apply_limits(&gV.ctl); // the limits of an op 1 that was joined with a start take effect with this generation
	if (gV.screenW <= 0 || gV.screenH <= 0) { return; }
	cw = gV.codedW; ch = gV.codedH;
	if ((gV.ctl.maxW != 0 && cw > (int)gV.ctl.maxW) || (gV.ctl.maxH != 0 && ch > (int)gV.ctl.maxH) || kvm_vcore_pick_level((unsigned)cw, (unsigned)ch, gV.ctl.maxLevel, &level, &fpsCap) != 0)
	{
		fail_request(KVM_REASON_RESOLUTION, cw, ch);
		return;
	}
	profile = kvm_vcore_pick_profile(gV.ctl.profFlags, 101);
	if (profile == 0) { fail_request(KVM_REASON_PROFILE, 0, 0); return; }

	// a frame of the old generation that is still in the encoder is given up (the new generation begins with a key frame); the tiles of it go back to JPEG
	cancel_inflight();
	reuse = (have_encoder() && !gV.hw.rebuild && gV.encW == cw && gV.encH == ch && gV.encLevel == level && gV.encFlags == gV.ctl.profFlags) ? 1 : 0;
	if (!reuse)
	{
		int wantHw;
		const int wasHw = (gV.hw.active && have_encoder()) ? 1 : 0;		// the hardware encoder that is running now and is closed for this generation (decides what a closed lid does)
		if (!gV.hw.bypassOpen && !kvm_vctl_open_allowed(&gV.ctl, now)) { gV.ctl.state = KVM_VS_WAIT; return; } // at most one encoder in 2 s: the tiles go as JPEG meanwhile (the one return to the software encoder after the hardware one was given up is not held back)
		if (!gV.hw.bypassOpen && hw_wait_capture(now)) { gV.ctl.state = KVM_VS_WAIT; return; }	// the capture is not known yet: not the closed lid
		gV.hw.bypassOpen = 0; gV.hw.rebuild = 0;
		close_encoder();
		if (gV.ctl.faulted) { return; }	// the encoder hung while it was closed: video_error has said so
		kvm_vctl_open_note(&gV.ctl, now);
		// which backend makes this encoder (the hardware one only when its self test of this machine has passed), and its thread
		wantHw = hw_choose(cw, ch, level, profile, wasHw, 0);
		if (gV.ctl.faulted) { return; }
		if ((wantHw ? 1 : 0) != gV.hw.active || !kvm_encthr_started() || kvm_encthr_abandoned())
		{
			if (switch_thread_backend(wantHw ? &kvm_encbe_mfhw : &kvm_encbe_mf) != 0) { stream_cap_off(KVM_REASON_NO_MF); gV.ctl.capVideo = 0; fail_request(KVM_REASON_NO_MF, 0, 0); return; }
		}
		if (gV.hw.active)
		{
			gV.qpStage = gV.hw.br ? 2 : 0;		// the hardware encoder has the QP (the self test proved it), or, the hardware self test, the bit rate (stage 2, the existing path: no QP, no Auto of the agent) and no other way
			if (gV.hw.br) { gV.bitrateTry = gV.hw.brTry; kvm_auto_set(&gV.au, 0, 0); }
			gV.hw.brRun = gV.hw.br; gV.hw.unaskedIdr = 0;
		}
		else
		{
			if (gV.hw.brRun)
			{
				// back from a hardware encoder that ran in stage 2: the software one starts from the beginning (the detector decides again) and the agent's Auto returns when the viewer asked for it
				gV.hw.brRun = 0; gV.qpStage = 0; gV.bitrateTry = 0;
				kvm_auto_set(&gV.au, (gV.autoOk && gV.autoWant) ? 1 : 0, now);
			}
			if (gV.hw.tryOpen) { hw_end_try(); }		// the software encoder takes over in an orderly way (the mode file, the lid, another size): no hardware in use
		}
		hw_log_backend(gV.hw.why);
		oc = (OpenCall *)calloc(1, sizeof(OpenCall));
		if (oc == NULL) { fail_request(KVM_REASON_INIT, 0, 0); return; }
		oc->cw = cw; oc->ch = ch; oc->level = level; oc->qpStage = gV.qpStage; oc->bitrateTry = gV.bitrateTry; oc->profFlags = gV.ctl.profFlags;
		oc->hw = gV.hw.active; oc->qpForm = gV.hw.qpForm; oc->luid = gV.hw.luid;
		oc->bitrate = kvm_vrc_bitrate_bps(&gV.rc);
		gV.qp = kvm_vrc_qp(&gV.rc, 1);
		if (gV.hw.active && gV.qp < KVM_VIDEO_QP_MIN - KVM_HW_QP_OFFSET) { gV.qp = KVM_VIDEO_QP_MIN - KVM_HW_QP_OFFSET; }	// the floor of the regulator for the hardware encoder
		oc->qp = gV.hw.active ? kvm_vcore_hw_qp(gV.qp) : gV.qp;	// what the encoder is given is the mapped QP, what is remembered (gV.qp, appliedQp, kvm_vrc) is the QP of the regulator
		QueryPerformanceCounter(&q0);
		r = kvm_encthr_call(open_on_thread, oc, KVM_ENC_CALL_MS);
		QueryPerformanceCounter(&q1);
		openMs = qpc_us(q0, q1) / 1000.0;
		if (r != 0) { gV.haveEnc = 0; encoder_hung("open"); return; }	// (the record is leaked: the thread may still write it)
		o = *oc; free(oc); oc = NULL;		// the thread is through with it
		memcpy(gV.seq, o.seq, o.seqLen); gV.seqLen = o.seqLen;
		r = o.rc;
		if (r != 0)
		{
			const struct mfh264_err &err = o.err;
			kvm_stats_log("video open failed code=%s hr=0x%08lx stage=%s size=%dx%d level=%d flags=%u ms=%.1f backend=%s", mfh264_result_name(r), (unsigned long)err.hr, err.stage != NULL ? err.stage : "-", cw, ch, level, gV.ctl.profFlags, openMs, gV.be != NULL ? gV.be->name : "-");
			if (gV.hw.active)
			{
				// the hardware encoder does not open (the driver, the token, the size): not a failed request of the viewer: back to the software encoder (an exception: the process ends)
				if (r == MFH264_E_EXCEPTION) { hw_die("open"); }
				hw_fallback(r == MFH264_E_HANG ? "open_hang" : "open", 1);
				return;
			}
			switch (r)
			{
				case MFH264_E_EXCEPTION: video_error("open", &err, 1); break;
				case MFH264_E_OS: case MFH264_E_NOMF: case MFH264_E_CREATE: stream_cap_off(KVM_REASON_NO_MF); gV.ctl.capVideo = 0; fail_request(KVM_REASON_NO_MF, 0, 0); break;
				case MFH264_E_LEVEL: case MFH264_E_INTYPE: fail_request(KVM_REASON_RESOLUTION, cw, ch); break;
				case MFH264_E_OUTTYPE: fail_request(KVM_REASON_PROFILE, 0, 0); break; // no profile of the viewer's list is accepted
				default: fail_request(KVM_REASON_INIT, 0, 0); break;
			}
			return;
		}
		gV.haveEnc = 1;
		gV.profile = o.profile;
		gV.info = o.info;
		{
			// the frame buffers: the header of 256 rectangles and the biggest access unit
			size_t au = o.maxAu;
			int k;
			if (au > VID_MAX_AU) { au = VID_MAX_AU; }
			for (k = 0; k < VID_DEPTH; k++)
			{
				gV.sl[k].frameCap = kvm_stream_frame_header_len(KVM_FRAME_MAX_RECTS) + au;
				gV.sl[k].frame = (uint8_t *)malloc(gV.sl[k].frameCap);
				if (gV.sl[k].frame == NULL) { gV.sl[k].frameCap = 0; free_frames(); close_encoder(); fail_request(KVM_REASON_INIT, 0, 0); return; }
			}
		}
		gV.appliedQp = 0;
		kvm_qpdet_reset(&gV.qpdet); gV.qpEffLogged = 0;
		kvm_qpdet_set_probes(&gV.qpdet, gV.hw.active ? 0 : 1);	// the hardware encoder: no deliberate QP steps now and then (the self test has proved the QP); only the steps of the regulator are watched, with the slope of that encoder
		kvm_qpdet_set_step_pct(&gV.qpdet, gV.hw.active ? 92 : 0);
		if (gV.qpStage == 2)
		{
			const struct mfh264_info &oi = gV.info;
			{ const uint32_t b0 = kvm_vrc_bitrate_bps(&gV.rc), mx = kvm_vcore_max_bitrate(level, gV.profile); gV.appliedBps = (int)(b0 > mx ? mx : b0); gV.bpsAt = now; }
			if (!oi.rc_mode_ok && gV.bitrateTry < 2)
			{
				// this encoder refuses that mode: the next one (the encoder is made again, a new generation follows by itself: the attempt is the same call)
				kvm_stats_log("video bitrate mode %d refused hr=0x%08lx: next", gV.bitrateTry, (unsigned long)oi.rc_mode_hr);
				gV.bitrateTry++; close_encoder(); gV.startPending = 1; gV.ctl.state = KVM_VS_WAIT;
				return;
			}
			kvm_stats_log("video bitrate mode=%d rc_mode_ok=%d bps=%d", gV.bitrateTry, oi.rc_mode_ok, gV.appliedBps);
		}
		else
		{
			if (o.qpRc != 0)
			{
				const struct mfh264_err &err = o.qpErr;
				kvm_stats_log("video set_qp failed code=%s hr=0x%08lx stage=%s qp=%d", mfh264_result_name(err.code), (unsigned long)err.hr, err.stage != NULL ? err.stage : "-", gV.qp);
				if (gV.hw.active) { if (err.code == MFH264_E_EXCEPTION) { hw_die("set_qp"); } hw_fallback("set_qp_open", 1); return; }
				if (err.code == MFH264_E_EXCEPTION) { video_error("set_qp", &err, 1); } else { fail_request(KVM_REASON_INIT, 0, 0); }
				return;
			}
			gV.appliedQp = gV.qp;
		}
		gV.encW = cw; gV.encH = ch; gV.encLevel = level; gV.encFlags = gV.ctl.profFlags;
		fresh = 1;
		view_reset(cw, ch);
	}
	{
		// the profile and the level that the encoder wrote into its sequence header (the SPS that is sent), else what was asked for
		const struct mfh264_info &info = gV.info;
		gV.profile = info.sps_profile_idc != 0 ? info.sps_profile_idc : gV.profile;
		gV.level = (info.sps_level_idc != 0 && !gV.hw.active) ? info.sps_level_idc : level;	// (the hardware encoder: the sequence header says 42 and its IDR 40; what is announced is the level that was computed, never below what the SPS says)
		if (fresh) { kvm_stats_log("video encoder open size=%dx%d profile_asked=%d level_asked=%d sps_profile=%d sps_level=%d level_set=%d max_au_kb=%d ms=%.1f backend=%s", cw, ch, gV.profile, level, info.sps_profile_idc, info.sps_level_idc, info.level_set_ok, info.max_au_size / 1024, openMs, gV.be->name); }
	}
	if (!gV.hw.active && !check_open_sps(cw, ch)) { return; }	// (the hardware encoder: its sequence header is not what it writes; the SPS of its first IDR is checked when it comes)
	gV.fpsCap = fpsCap;
	if (fresh) { gV.ts = 0; gV.encAvgUs = 0; gV.avgAu = 0; }	// the time stamps of one encoder never go back
	kvm_glue_lock();
	ok = kvm_vctl_begin_gen(&gV.ctl, kvm_glue_stream(), gV.profile, gV.level, (unsigned)cw, (unsigned)ch, fresh, &m) == 0;
	if (ok) { kvm_glue_state_locked(&m); }
	kvm_glue_unlock();
	kvm_glue_log_flush();
	if (!ok) { cancel_start(); return; }
	gV.genAt = now;
	gV.cpuWinLast = 0; gV.nonEncCpuLast = 0;		// the window of the CPU of the encoder starts again with the generation (not with the whole pause before it)
	if (fresh) { kvm_vrc_init_backend(&gV.rc, gV.rc.qp, now, gV.hw.active); kvm_vrc_set_picture(&gV.rc, (uint32_t)cw * (uint32_t)ch); }	// a new encoder starts with a full bucket; the QP the regulator found is kept
	if (fresh && gV.hw.active && gV.hw.flatMilli != 0) { kvm_vrc_set_flat_floor(&gV.rc, gV.hw.flatMilli); }	// the floor of a flat key frame that the self test measured on this encoder
	gV.startPending = 0;
	gV.st.starts++;
	if (gV.startKind == START_OP1 || gV.startKind == START_RETRY || gV.startKind == START_BACKEND)
	{
		// op 1 repairs the VIDEO tiles like an op 3 with both flags (5.2): they are sent as JPEG again
		unsigned n = kvm_vtiles_take_video(&gV.tiles, gV.map, 0);
		if (n > 0) { tiles_to_jpeg(gV.map); }
		kvm_vctl_repair_now(&gV.ctl, now);
	}
	kvm_stats_log("video start kind=%d gen=%d profile=%d level=%d coded=%dx%d fps_cap=%d qp=%d fresh=%d", gV.startKind, (int)m.gen, gV.profile, gV.level, cw, ch, fpsCap, gV.qp, fresh);
	gV.startKind = START_NONE;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the interface
// ---------------------------------------------------------------------------------------------------------------------------------------

int kvm_video_session_begin(int *reason)
{
	char env[16] = "";
	struct mfh264_err err;
	int cap = 1, r = KVM_REASON_OK;
	LARGE_INTEGER q0, q1;
	memset(&gV, 0, sizeof(gV));
	if (GetEnvironmentVariableA("MESH_KVM_VIDEO", env, sizeof(env)) > 0 && env[0] == '0') { cap = 0; r = KVM_REASON_DISABLED; }
	gV.encMode = KVM_ENCTHR_THREADED;
	if (GetEnvironmentVariableA("MESH_KVM_ENC_THREAD", env, sizeof(env)) > 0)
	{
		if (env[0] == '0') { gV.encMode = KVM_ENCTHR_INLINE; }
#ifdef KVM_VIDEO_TEST
		else if (env[0] == '2') { gV.encMode = KVM_ENCTHR_MANUAL; }	// (the tests only: the harness says when the thread runs)
#endif
	}	// the encoder in the call that hands the frame over: the path of before the encoder thread
	gV.be = &kvm_encbe_mf;
	gV.hw.env = kvm_hwenv_read(); gV.hw.mode = kvm_hwmode_read();
	gV.view.on = env_flag("MESH_KVM_NV12_FROZEN", KVM_NV12_FROZEN_DEFAULT);
	gV.view.flat = gV.view.on && env_flag("MESH_KVM_IDR_FLAT", KVM_IDR_FLAT_DEFAULT);
	gV.view.firstFull = env_flag("MESH_KVM_IDR_FLAT_FIRST_FULL", KVM_IDR_FLAT_FIRST_FULL_DEFAULT);
	if (cap)
	{
		// a marker "a try with the hardware is under way" that nobody resolved (the parent died too, or this is a child that was started by hand): one error against the entry, before anything is tried
		CacheOp ro;
		memset(&ro, 0, sizeof(ro)); ro.op = CO_RECOVER;
		cache_do(&ro);
		// Windows 8 or later and Media Foundation (loaded here, at run time: nothing in the import table), on the thread of the encoder. The encoder itself is created when the first viewer asks for video.
		QueryPerformanceCounter(&q0);
		if (kvm_encthr_start(gV.be, gV.encMode, VID_DEPTH, KVM_ENC_CALL_MS, &err) != 0)
		{
			cap = 0; r = KVM_REASON_NO_MF;
			kvm_stats_log("video init failed code=%s hr=0x%08lx stage=%s", mfh264_result_name(err.code), (unsigned long)err.hr, err.stage != NULL ? err.stage : "-");
		}
		else
		{
			gV.mfInit = 1;
			QueryPerformanceCounter(&q1);
			kvm_stats_log("video init ok ms=%.1f enc_thread=%d backend=%s", qpc_us(q0, q1) / 1000.0, gV.encMode == KVM_ENCTHR_THREADED ? 1 : 0, gV.be->name);
		}
	}
	kvm_vctl_init(&gV.ctl, cap);
	kvm_vctl_set_threaded(&gV.ctl, gV.encMode != KVM_ENCTHR_INLINE);	// counts the whole encode time of a frame that the thread makes
	kvm_vrc_init(&gV.rc, KVM_VIDEO_QP, timeGetTime());
	kvm_budget_init(&gV.budget, kvm_glue_cores());
	kvm_auto_init(&gV.au);
	gV.autoOk = !(GetEnvironmentVariableA("MESH_KVM_AUTO", env, sizeof(env)) > 0 && env[0] == '0');
	gV.cpuLast = kvm_glue_cpu_us();
	gV.nonEncCpuLast = kvm_glue_nonenc_cpu_us();
	gV.capReason = r;
	gV.sessionOn = 1;
	if (reason != NULL) { *reason = r; }
	kvm_stats_log("video start cap_video=%d reason=%d qp=%d qp_min=%d", cap, r, KVM_VIDEO_QP, KVM_VIDEO_QP_MIN);
	return cap;
}

void kvm_video_session_end(void)
{
	int k;
	if (!gV.sessionOn) { return; }
	kvm_video_stats_flush(1);
	kvm_hwtest_abort(&gV.hw.run);					// a self test that is still running ends with the session (no verdict: it was not given the time)
	close_encoder();								// the frames in flight are given up, the encoder closed on its thread
	hw_end_try();									// an orderly end: the marker of a try is no crash
	release_held();
	for (k = 0; k < VID_DEPTH; k++) { free(gV.sl[k].cover); free(gV.sl[k].paintCrc); gV.sl[k].cover = NULL; gV.sl[k].paintCrc = NULL; }
	if (gV.mfInit) { kvm_encthr_stop(2000); gV.mfInit = 0; }	// Media Foundation is shut down on the thread; a thread that does not end is left to the process (after a fault or a hang it is never waited for)
	kvm_nv12v_free(&gV.view.v); gV.view.ok = 0;
	kvm_vtiles_free(&gV.tiles);
	free(gV.map);
	gV.map = NULL;
	gV.sessionOn = 0;
}

static void free_slot_maps(void)
{
	int k;
	for (k = 0; k < VID_DEPTH; k++) { free(gV.sl[k].cover); free(gV.sl[k].paintCrc); gV.sl[k].cover = NULL; gV.sl[k].paintCrc = NULL; }
}

void kvm_video_geometry(int tileRows, int tileCols, int screenW, int screenH)
{
	int k, ok = 1;
	if (!gV.sessionOn) { return; }
	// the frames in flight were made for the old grid: they are given up (the new generation begins with a key frame) and their tiles go with the old model
	cancel_inflight();
	kvm_vtiles_free(&gV.tiles);
	free_slot_maps();
	free(gV.map);
	gV.map = NULL;
	gV.screenW = screenW; gV.screenH = screenH;
	gV.codedW = (int)kvm_stream_pad32((unsigned)screenW); gV.codedH = (int)kvm_stream_pad32((unsigned)screenH);
	if (kvm_vtiles_init(&gV.tiles, tileRows, tileCols) != 0 || (gV.map = (uint8_t *)malloc((size_t)tileRows * (size_t)tileCols)) == NULL) { ok = 0; }
	for (k = 0; ok && k < VID_DEPTH; k++)
	{
		gV.sl[k].cover = (uint8_t *)malloc((size_t)tileRows * (size_t)tileCols);
		gV.sl[k].paintCrc = (int32_t *)malloc((size_t)tileRows * (size_t)tileCols * sizeof(int32_t));
		if (gV.sl[k].cover == NULL || gV.sl[k].paintCrc == NULL) { ok = 0; }
	}
	if (!ok)
	{
		// no memory: no video (the tiles go as JPEG); the same as an error of the encoder
		kvm_vtiles_free(&gV.tiles);
		free_slot_maps();
		free(gV.map);
		gV.map = NULL;
		if (gV.ctl.state != KVM_VS_OFF) { video_error("tile model", NULL, 0); }
		return;
	}
	if (gV.ctl.state != KVM_VS_OFF)
	{
		// cmd 7 with a new size (or the same after a switch of the desktop): the tiles are all JPEG again; a new generation begins (5.2), with a new encoder if the coded size changed
		gV.ctl.state = KVM_VS_WAIT;
		gV.genAt = timeGetTime();
		gV.startPending = 1; gV.startKind = START_GEOMETRY;
	}
}

void kvm_video_ctrl(const kvm_ctrl *c, unsigned actions, unsigned atMs)
{
	if (!gV.sessionOn) { return; }
	if ((actions & KVM_ACT_VIDEO_REQUEST) && gV.nreq < VID_MAX_REQ) { gV.req[gV.nreq] = *c; gV.reqAt[gV.nreq] = atMs; gV.nreq++; }
	if (actions & (KVM_ACT_IDR | KVM_ACT_REPAIR)) { kvm_vctl_op3(&gV.ctl, actions); }
	if (actions & KVM_ACT_IDR) { gV.op3Prev = gV.op3At; gV.op3At = (DWORD)(atMs ? atMs : 1u); gV.op3Count++; gV.op3Flags = c->flags; }	// (the log line "video idr": the viewer or the recording of the server)
}

void kvm_video_note_refresh(void) { InterlockedIncrement(&gV.refreshSeq); }

#ifdef KVM_VIDEO_TEST
const kvm_auto *kvm_video_test_auto(void) { return &gV.au; }		// tests only: the state of Auto
#endif
int kvm_video_on(void) { return (gV.sessionOn && gV.ctl.state == KVM_VS_ON) ? 1 : 0; }
// tiles that wait for a frame, and 1 more while a frame is in the encoder: the loop keeps its pace and the overlap of the capture, and the sharpening governor sees the motion
int kvm_video_pending(void) { return (gV.sessionOn && gV.tiles.st != NULL) ? (int)gV.tiles.nPending + (kvm_encthr_inflight() > 0 ? 1 : 0) : 0; }
// 1 while a frame is in the encoder
int kvm_video_busy(void) { return (gV.sessionOn && kvm_encthr_inflight() > 0) ? 1 : 0; }
void *kvm_video_event(void) { return kvm_encthr_event(); }
// tests only: counters of the tile model
unsigned kvm_video_test_qpdet_pframes(void) { return gV.qpdet.pFrames; }
unsigned kvm_video_test_rc_win_bytes(void) { return gV.rc.winBytes; }
unsigned kvm_video_test_rc_flat_milli(void) { return gV.rc.flatMilli; }
void kvm_video_test_qp_ineffective(void) { gV.qpRestart = gV.qpStage + 1; }
void kvm_video_test_counts(unsigned *nInflight, unsigned *nVideo, unsigned *nPending, unsigned *jpegOnInflight)
{
	*nInflight = gV.tiles.nInflight; *nVideo = gV.tiles.nVideo; *nPending = gV.tiles.nPending; *jpegOnInflight = gV.tiles.jpegOnInflight;
}

// the interval of the step of Auto while the bit of op 1 is up and the video runs (or is about to); 0 = no override, the period of command 5 holds
static uint32_t auto_interval_ms(void)
{
	return (gV.sessionOn && gV.qpStage < 2 && (gV.ctl.state == KVM_VS_ON || gV.ctl.state == KVM_VS_WAIT)) ? kvm_auto_interval_ms(&gV.au) : 0u;
}

// The period of the loop of kvm.c (the pause of an iteration): the step of Auto while it is on, else the one of command 5 that the caller passes
int kvm_video_loop_period(int viewerPeriodMs)
{
	const uint32_t ai = auto_interval_ms();
	return ai > 0 ? (int)ai : viewerPeriodMs;
}

// The 242 that kvm.c writes: this agent knows Auto (bit 2 of the caps) when it has video at all, and the QP steers the encoder (not in stage 2, where the bit rate does and Auto's QP rules have nothing to
// judge by: then the viewer's own Auto, which sees the link, steers). Called for every 242, from the one place that writes them.
void kvm_video_state_caps(kvm_state_msg *m)
{
	if (gV.autoOk && gV.qpStage < 2 && (m->caps & KVM_CAPS_H264)) { m->caps = (uint8_t)(m->caps | KVM_CAPS_AUTO); }
}

// The capture of the image that kvm_video_emit looks at next took `ms` (capture and output buffer) and stands for `dwmFrames` frames of the desktop manager (0 = not known)
void kvm_video_note_capture(unsigned ms, unsigned dwmFrames)
{
	if (gV.sessionOn) { kvm_auto_note_capture(&gV.au, ms, dwmFrames); }
}

// The period of the frame rate that the viewer asked for (cmd 5 / FRAME_RATE_TIMER: 16 ms High, 33 Medium, 50 Low, 100 Slow; older viewers send 20 / 50 / 100): the video takes it as it is, no longer 50 ms by
// inheritance , but never shorter than 1000 / KVM_VIDEO_MAX_FPS and never below what the level of the stream allows (fpsCap)
static uint32_t frame_period_ms(void)
{
	const uint32_t shortest = 1000u / KVM_VIDEO_MAX_FPS;
	uint32_t p = FRAME_RATE_TIMER >= 1 ? (uint32_t)FRAME_RATE_TIMER : shortest;
	if (p < shortest) { p = shortest; }
	{ const uint32_t ai = auto_interval_ms(); if (ai > 0) { p = ai < shortest ? shortest : ai; } }	// the step of Auto is the period (command 5 asks for Medium as the reserve)
	if (gV.fpsCap > 0 && (uint32_t)gV.fpsCap < KVM_VIDEO_MAX_FPS && 1000u / (uint32_t)gV.fpsCap > p) { p = 1000u / (uint32_t)gV.fpsCap; }
	return p;
}

// The interval between frames: that period, and never below what the encoder needs to stay in its CPU budget (kvm_pace_interval)
static uint32_t frame_interval_ms(void)
{
	return kvm_pace_interval(&gV.budget, frame_period_ms(), gV.encAvgUs);
}

// The loop may sleep until the next frame is due (the slot of the frame, kvm_pace.h): >= 1 = milliseconds until then; -1 = the video is not waiting for its slot (nothing is pending, the window of the
// ACK or the byte bucket of the rate control holds the frame, no frame was written yet (the first frame of the session has no slot), or the slot has come): the loop keeps its own period
int kvm_video_wake_ms(void)
{
	DWORD now;
	uint32_t ms;
	if (!(gV.sessionOn && gV.ctl.state == KVM_VS_ON && have_encoder() && gV.tiles.st != NULL && gV.tiles.nPending > 0 && gV.ctl.haveFrame && kvm_encthr_free_slot() >= 0)) { return -1; }
	now = timeGetTime();
	if (!kvm_ack_peek(KVM_GATE_VIDEO) || !kvm_vrc_peek(&gV.rc, now)) { return -1; }
	ms = kvm_pace_wake_ms(1, gV.ctl.lastFrame, now, frame_interval_ms());
	{
		// the frame also waits until its write would be 75 % of the interval behind the write of the last one: the loop wakes for that, not for the next iteration of its own schedule
		const uint32_t w = kvm_vctl_write_wait_ms(&gV.ctl, now, frame_interval_ms());
		if (w > ms) { ms = w; }
	}
	return ms > 0 ? (int)ms : -1;
}

int kvm_video_interval_ms(void) { return gV.sessionOn ? (int)frame_interval_ms() : 0; }

unsigned kvm_video_r_kbps(void) { return (gV.sessionOn && gV.ctl.state == KVM_VS_ON) ? (unsigned)(gV.rc.rate * 8 / 1000) : 0u; }

int kvm_video_want_image(void)
{
	// a picture is only worth asking for when a frame may go now: the window is open and the frame interval has passed (otherwise every iteration would order a capture, a full BitBlt with GDI)
	return (gV.sessionOn && gV.ctl.state == KVM_VS_ON && have_encoder() && gV.tiles.nPending > 0 && kvm_vctl_pace_ok(&gV.ctl, timeGetTime(), frame_interval_ms()) && kvm_vrc_peek(&gV.rc, timeGetTime()) && kvm_ack_peek(KVM_GATE_VIDEO)) ? 1 : 0;
}

// The video is running or about to (a new generation waits for the encoder): the scale of the picture does not change meanwhile (WA-2b ladder, plan B.3)
// The scale ladder wants a change (kvm_adapt.cpp): see kvm_vcore_scale_allowed
int kvm_video_scale_allowed(int down) { return kvm_vcore_scale_allowed(kvm_video_active(), down, (uint32_t)(timeGetTime() - gV.genAt)); }

int kvm_video_active(void) { return (gV.sessionOn && gV.ctl.state != KVM_VS_OFF) ? 1 : 0; }

void kvm_video_tile_jpeg(int row, int col)
{
	if (gV.sessionOn && gV.tiles.st != NULL) { kvm_vtiles_jpeg(&gV.tiles, row, col); }
}

// 1: the tile waits for a frame or is in a frame that is being encoded: the sharpening leaves it (it is going to be painted by the frame)
int kvm_video_tile_pending(int row, int col)
{
	return (gV.sessionOn && gV.tiles.st != NULL && (kvm_vtiles_get(&gV.tiles, row, col) == KVM_VT_PENDING || kvm_vtiles_inflight(&gV.tiles, row, col))) ? 1 : 0;
}

int kvm_video_claim_rect(int row0, int col0, int row1, int col1, int cls)
{
	int r, c;
	if (!gV.sessionOn || cls != 1 || gV.ctl.state != KVM_VS_ON || !have_encoder() || PIXEL_SIZE != 4 || gV.tiles.st == NULL || tileInfo == NULL) { return 0; }
	if (row0 < 0 || col0 < 0 || row1 >= gV.tiles.rows || col1 >= gV.tiles.cols || row1 >= TILE_HEIGHT_COUNT || col1 >= TILE_WIDTH_COUNT) { return 0; }
	// only changes of the content go to the video: a tile that is sent because of a REFRESH or a repair has to be a JPEG (the viewer's copy of it is not known)
	for (r = row0; r <= row1; r++)
	{
		for (c = col0; c <= col1; c++) { if (tileInfo[r][c].refresh) { return 0; } }
	}
	kvm_vtiles_claim(&gV.tiles, row0, col0, row1, col1);
	for (r = row0; r <= row1; r++)
	{
		for (c = col0; c <= col1; c++) { tileInfo[r][c].deferred = 0; tileInfo[r][c].deferredAt = 0; } // video took the change over: nothing is held back any more
	}
	gV.st.claimedTiles += (long long)(row1 - row0 + 1) * (col1 - col0 + 1);
	if ((long long)gV.tiles.nPending > gV.st.pendingMax) { gV.st.pendingMax = (long long)gV.tiles.nPending; }
	return 1;
}

// Request of the viewer (op 1) that the stream layer accepted: the window is on and want is VIDEO there. A later command of the same poll may have changed that; then it is dropped (the
// stream layer has answered it).
static void handle_request(int i, DWORD now)
{
	vid_view v;
	const kvm_ctrl c = gV.req[i];
	get_view(&v, now);
	if (v.want != KVM_WANT_VIDEO || v.veto || !v.ackActive) { return; }
	gV.autoWant = (c.flags & KVM_AUTO_OP1_FLAG) ? 1 : 0;
	kvm_auto_set(&gV.au, (gV.autoOk && gV.qpStage < 2 && gV.autoWant) ? 1 : 0, now);
	if (gV.ctl.state == KVM_VS_OFF) { kvm_auto_resume(&gV.au, now); }		// the video was off (a veto, op 2, JPEG): the window and the series of before are not this stream's	// bit 3 of the flags = Auto (the next generation starts on its step; the fixed modes are as they were)
	if (!gV.ctl.capVideo) { fail_request(KVM_REASON_DISABLED, 0, 0); return; } // lost after errors (cannot happen with the stream layer's capVideo, kept for the race)
	if (gV.ctl.inError) { kvm_vctl_limits(&gV.ctl, &c); send_current_state(KVM_REASON_ERROR); return; } // after an error of the encoder the video stays off until the retry (60 s): the answer is the state, nothing is started
	if (kvm_vctl_op1(&gV.ctl, &c, gV.reqAt[i]) == KVM_VOP1_COALESCE)
	{
		send_current_state(KVM_REASON_OK); // joined with the previous op 1: the answer is the state now
		return;
	}
	gV.startPending = 1; gV.startKind = START_OP1;
	if (gV.ctl.state == KVM_VS_OFF) { gV.ctl.state = KVM_VS_WAIT; }
	attempt_start(now);
}

// once an iteration while the video runs. Closes the window of Auto when it is a second old and may change the step (the interval of the frames: no new generation, no key frame). Nothing happens while the bit is down.
static void auto_tick(DWORD now, const vid_view *v)
{
	kvm_auto_env e;
	kvm_auto_dec d;
	kvm_auto_win w;
	uint64_t inflight;
	uint32_t minRtt;
	char line[640];
	if (!kvm_auto_on(&gV.au)) { return; }
	kvm_glue_lock();
	minRtt = kvm_glue_stream()->minRtt;
	inflight = kvm_stream_inflight(kvm_glue_stream());
	kvm_glue_unlock();
	memset(&e, 0, sizeof(e));
	e.now = now; e.valid = (gV.ctl.state == KVM_VS_ON && !gV.startPending && have_encoder()) ? 1 : 0; e.cls = KVM_AUTO_CLASS_UI;	// (the class "video" comes with a later stage: kvm_motion)
	e.minRttMs = minRtt;
	e.encAvgUs = gV.encAvgUs; e.parX10 = gV.budget.parX10; e.budgetMilli = gV.budget.budgetMilli;
	e.floorMs = gV.encAvgUs > 0 ? kvm_budget_enc_floor_ms(&gV.budget, gV.encAvgUs) : 0u;
	e.qpNow = (uint32_t)gV.rc.qp; e.qpFloor = (uint32_t)gV.rc.qpMin; e.rateBps = (uint32_t)(gV.rc.rate > 0xFFFFFFFFull ? 0xFFFFFFFFull : gV.rc.rate);
	{
		// the data in flight is the pipe (rate x round trip) and a queue: only the queue says that the link is full (20 Mb/s at 150 ms has 150 ms in flight with an empty queue)
		const uint32_t fl = v->rate > 0 ? (uint32_t)(inflight * 1000ull / v->rate) : 0u;
		e.queueMs = fl > minRtt ? fl - minRtt : 0u;
	}
	if (gV.fpsCap > 0 && (uint32_t)gV.fpsCap < KVM_VIDEO_MAX_FPS) { e.maxFps = (uint32_t)gV.fpsCap; e.maxReason = KVM_AUTO_R_SCREEN; }		// (the level of the stream allows no more: the video would go at that rate whatever Auto chose)
	memset(&w, 0, sizeof(w));
	(void)kvm_auto_tick(&gV.au, &e, &d, &w);
	if (d.changed || (w.windowMs > 0 && w.motionMs > 0))
	{
		(void)kvm_auto_format(&gV.au, &w, &d, line, (int)sizeof(line));
		kvm_stats_log("%s", line);
	}
}

void kvm_video_poll(void)
{
	DWORD now;
	vid_view v;
	int i, n;
	if (!gV.sessionOn) { return; }
	now = timeGetTime();
	kvm_video_service();				// the frames that are done are written, the watchdog looks at the ones that are not
	get_view(&v, now);
	// the self test of the hardware encoder runs in a process of its own; when it has passed while the software encoder is running, the hardware one is started with a new generation at the next quiet moment
	// (no frame in flight, no pending tile, no frame for a second): the video does not wait for the test and is not interrupted by it
	hw_poll_test();
	if (gV.hw.switchPending && gV.hw.keyOk)
	{
		if (gV.hw.active || gV.hw.disabled || gV.hw.env == KVM_HWP_ENV_OFF || kvm_hwmode_read() == KVM_HWP_MODE_SW) { gV.hw.switchPending = 0; }	// (the mode file says "sw": no generation for nothing)
		else if (gV.ctl.state == KVM_VS_OFF) { gV.hw.switchPending = 0; }	// nobody is looking: the next op 1 chooses the backend
		else if (gV.ctl.state == KVM_VS_ON && !gV.startPending && kvm_vctl_open_allowed(&gV.ctl, now))
		{
			const int quiet = (kvm_encthr_inflight() == 0 && gV.tiles.st != NULL && gV.tiles.nPending == 0 && gV.held == NULL && (gV.lastFrameAt == 0 || (DWORD)(now - gV.lastFrameAt) > 1000u)) ? 1 : 0;
			// a screen that never rests (a video, a game) never has a quiet moment: after KVM_HW_SWITCH_DUE_MS the switch is made anyway, as one more generation (the frame in the encoder and the tiles that wait go
			// as JPEG, as at every other generation; the first frame is an IDR: 380 KB once on the AMD encoder, the price of the switch, paid once per process)
			const int due = (gV.hw.switchSince != 0 && (DWORD)(now - gV.hw.switchSince) >= KVM_HW_SWITCH_DUE_MS) ? 1 : 0;
			if (quiet || due)
			{
				if (!quiet) { cancel_inflight(); pending_to_jpeg(); }
				gV.hw.switchPending = 0; gV.hw.switchSince = 0; gV.hw.rebuild = 1;
				gV.startPending = 1; gV.startKind = START_BACKEND; gV.ctl.state = KVM_VS_WAIT;
				kvm_stats_log(quiet ? "video hw switch: the hardware encoder is wanted, a new generation on it" : "video hw switch: the hardware encoder is wanted and the screen never rested, a new generation on it anyway");
			}
		}
	}
	hw_recheck(now);
	// the stream layer switched the video off (op 2, op 4, op 5, no ACK for 15 s, the picture type): it has answered; release the encoder and send the pending tiles as JPEG (I7)
	if (gV.ctl.state == KVM_VS_ON && !v.videoOn) { stop_video("stream layer"); }
	else if (gV.ctl.state == KVM_VS_WAIT && (v.want != KVM_WANT_VIDEO || v.veto || !v.ackActive)) { stop_video("request withdrawn"); }
	// the sharpening path disappeared (picture type, colour depth) after the video was off already: the VIDEO tiles would stay blurred, they go back to JPEG
	if (!v.adaptOk && gV.tiles.st != NULL && gV.map != NULL && gV.tiles.nVideo > 0)
	{
		unsigned k;
		barrier(&gV.st.barOther);			// (a frame in flight lands first)
		k = kvm_vtiles_take_video(&gV.tiles, gV.map, 1);
		if (k > 0) { tiles_to_jpeg(gV.map); gV.st.fallbackTiles += k; }
	}
	// rate control: the capacity estimate and R every iteration, the QP regulator every 500 ms, one log line a second
	if (gV.ctl.state == KVM_VS_ON)
	{
		const unsigned budget = (unsigned)kvm_adapt_viewer_budget_kbps();
		kvm_vrc_update(&gV.rc, now, budget, v.rate, v.linkLimited);
		(void)kvm_vrc_tick(&gV.rc, now);
		{
			// the CPU of the encoder = what the process used less the main loop and the capture thread (the encoder runs beside them now, so the process clock over one call says nothing): counted here, once an iteration
			const unsigned long long cpu = kvm_glue_cpu_us(), non = kvm_glue_nonenc_cpu_us();
			if (gV.encMode != KVM_ENCTHR_INLINE && cpu != 0 && non != 0 && gV.nonEncCpuLast != 0 && gV.cpuWinLast != 0 && cpu >= gV.cpuWinLast && non >= gV.nonEncCpuLast)
			{
				const unsigned long long d = cpu - gV.cpuWinLast, o = non - gV.nonEncCpuLast;
				kvm_budget_note_cpu(&gV.budget, d > o ? d - o : 0ull);
			}
			gV.cpuWinLast = cpu; gV.nonEncCpuLast = non;
		}
		(void)kvm_budget_window(&gV.budget, now);	// the parallelism of the encoder, measured in windows of 500 ms
		auto_tick(now, &v);							// Auto mode
		if ((DWORD)(now - gV.rcLog) >= 1000)
		{
			const unsigned long long sh = kvm_adapt_sharpen_bytes_total();
			const DWORD dt = gV.rcLog != 0 ? (DWORD)(now - gV.rcLog) : 1000;
			kvm_stats_log("video rc r_kbps=%llu rate_kbps=%llu budget_kbps=%u link_limited=%d starved=%d qp=%d qp_idr=%d fps=%.1f idr=%u video_kb=%llu sharpen_kb=%llu enc_ms_avg=%.1f enc_ms_max=%.1f interval_ms=%u denied=%u win_refused=%u qp_up=%u qp_down=%u bucket_kb=%d qp_blocked=%u sharpen_share=%d qp_stage=%d qp_verdict=%d qp_events=%u r_from_floor=%d floor_kbps=%llu be=%s",
						  (unsigned long long)(gV.rc.rate * 8 / 1000), (unsigned long long)(v.rate * 8 / 1000), budget, v.linkLimited, kvm_vrc_update_starved(&gV.rc, now), gV.rc.qp, kvm_vrc_qp(&gV.rc, 1), (double)gV.rc.sFrames * 1000.0 / (double)dt, gV.rc.sIdr,
						  (unsigned long long)(gV.rc.sBytes / 1024), (sh - gV.sharpLast) / 1024, gV.rc.sFrames > 0 ? (double)gV.rc.sEncUs / (double)gV.rc.sFrames / 1000.0 : 0.0, (double)gV.rc.sEncMaxUs / 1000.0, (unsigned)frame_interval_ms(),
						  gV.rc.sDenied, gV.rc.sHeld, gV.rc.sQpUp, gV.rc.sQpDown, (int)(gV.rc.tokens / 1024), gV.rc.sQpBlocked,
						  (gV.rc.sBytes + gV.rc.sSharpBytes) > 0 ? (int)(gV.rc.sSharpBytes * 100ull / (gV.rc.sBytes + gV.rc.sSharpBytes)) : 0, gV.qpStage, (int)gV.qpdet.verdict, (unsigned)gV.qpdet.events, (int)gV.rc.rFromFloor, (unsigned long long)(gV.rc.capFloor * 8 / 1000), gV.hw.active ? "hw" : "sw");
			{
				const unsigned long long cpu = kvm_glue_cpu_us();
				const uint32_t period = frame_period_ms(), interval = frame_interval_ms();
				kvm_stats_log("video pace period_ms=%u interval_ms=%u enc_floor_ms=%u enc_par=%.1f enc_cpu_cores=%.2f budget_cores=%.2f cores=%u process_cpu_cores=%.2f slots=%u slot_late=%u slot_late_ms_max=%u slot_resync=%u",
							  (unsigned)period, (unsigned)interval, (unsigned)kvm_budget_enc_floor_ms(&gV.budget, gV.encAvgUs), (double)gV.budget.parX10 / 10.0, (double)gV.budget.encMilli / 1000.0, (double)gV.budget.budgetMilli / 1000.0, (unsigned)gV.budget.cores,
							  cpu >= gV.cpuLast ? (double)(cpu - gV.cpuLast) / 1000.0 / (double)dt : 0.0, (unsigned)gV.slotLate.n, (unsigned)gV.slotLate.late, (unsigned)gV.slotLate.maxMs, (unsigned)gV.slotLate.resync);
				gV.cpuLast = cpu; kvm_late_clear(&gV.slotLate); kvm_budget_clear_stats(&gV.budget);
			}
			gV.sharpLast = sh; gV.rcLog = now; kvm_vrc_clear_stats(&gV.rc);
		}
	}
	// the viewer acknowledged the last key frame: what the link took of it (kvm_vrc_idr_acked) is the evidence that sizes the next one when the link is not full
	if (gV.ctl.state == KVM_VS_ON && gV.idrWatchBytes != 0)
	{
		uint32_t ackAt = 0, ts = 0, queueMs = 0, minRtt = 0;
		int got;
		kvm_glue_lock();
		got = kvm_stream_watch_result(kvm_glue_stream(), &ackAt, &ts, &queueMs, &minRtt);
		kvm_glue_unlock();
		if (got)
		{
			kvm_vrc_idr_acked(&gV.rc, now, gV.idrWatchBytes, ts, ackAt, queueMs, minRtt);
			kvm_stats_log("video idr acked kb=%lu ms=%ld queue_ms=%lu min_rtt_ms=%lu c_idr_bps=%llu floor_bps=%llu", (unsigned long)(gV.idrWatchBytes / 1024), (long)(int32_t)(ackAt - ts), (unsigned long)queueMs, (unsigned long)minRtt, (unsigned long long)gV.rc.cIdr, (unsigned long long)gV.rc.capFloor);
			gV.idrWatchBytes = 0;
		}
	}
	// the encoder does not listen to the QP (kvm_qpdet): first the QP also in the input sample, then the bit rate (a new encoder, a new generation like a change of the size)
	if (gV.qpRestart != 0)
	{
		const int to = gV.qpRestart;
		gV.qpRestart = 0;
		if (gV.hw.active && gV.ctl.state == KVM_VS_ON && have_encoder())
		{
			// the hardware encoder: the QP is its only way (the self test has proved it on a picture of its own); a verdict of the natural steps that it has no effect means that the encoder is not the one that was
			// tested: back to the software encoder (the stages of the software encoder, the QP in the sample and the bit rate, are not for it)
			kvm_stats_log("video qp ineffective (hardware encoder) events=%u llr=%.1f pframes=%u", (unsigned)gV.qpdet.events, gV.qpdet.llr, (unsigned)gV.qpdet.pFrames);
			// the hardware self test (NVIDIA takes the QP at the open and ignores a change in the running stream): once, the same hardware encoder is opened again in the form 8 (stage 2, CBR) and the cache says so for the next session; only an error
			// of that try (or a QP form that is not ours to change) gives the hardware up
			int reopened = 0;
			if (!gV.hw.br && !gV.hw.brTried && gV.hw.keyOk)
			{
				CacheOp fo;
				memset(&fo, 0, sizeof(fo)); fo.op = CO_FORM; fo.key = gV.hw.key; fo.qpForm = KVM_HWP_FORM_BR_CBR;
				gV.hw.brTried = 1;
				if (cache_do(&fo) == 0)
				{
					kvm_stats_log("video hw: the encoder takes the QP at the open only: opened again with the bit rate (form %d)", KVM_HWP_FORM_BR_CBR);
					cancel_inflight();
					pending_to_jpeg();
					close_encoder();
					reopened = 1;
					if (!gV.ctl.faulted) { gV.hw.bypassOpen = 1; gV.hw.rebuild = 1; gV.startPending = 1; gV.startKind = START_BACKEND; gV.ctl.state = KVM_VS_WAIT; }
				}
			}
			if (!reopened) { hw_fallback("qp_ineffective", 1); }
		}
		else if (gV.ctl.state == KVM_VS_ON && have_encoder() && to == gV.qpStage + 1)
		{
			kvm_stats_log("video qp ineffective stage=%d events=%u llr=%.1f probes=%u pframes=%u -> stage %d", gV.qpStage, (unsigned)gV.qpdet.events, gV.qpdet.llr, (unsigned)gV.qpdet.probes, (unsigned)gV.qpdet.pFrames, to);
			if (to == 1)
			{
				static int sqRc;					// (static: the argument of a call that may be given up)
				barrier(&gV.st.barOther);			// the frames in flight land first: they were made with the old setting, and are still good
				sqRc = -1;
				if (kvm_encthr_call(sample_qp_on_thread, &sqRc, KVM_ENC_CALL_MS) != 0) { encoder_hung("set_sample_qp"); return; }
				if (sqRc == 0) { gV.qpStage = 1; kvm_qpdet_reset(&gV.qpdet); gV.qpEffLogged = 0; }
				else { gV.qpStage = 1; gV.qpRestart = 2; }
			}
			else
			{
				gV.qpStage = 2; gV.bitrateTry = 0; gV.appliedBps = 0;
				kvm_auto_set(&gV.au, 0, 0);			// the bit rate steers the encoder, the QP says nothing: the agent's Auto ends (the 242 of the next generation has no caps bit 2)
				kvm_vrc_set_bitrate_mode(&gV.rc, 1);
				pending_to_jpeg();
				close_encoder();
				gV.ctl.state = KVM_VS_WAIT;
				gV.startPending = 1; gV.startKind = START_RETRY;
			}
		}
	}
	// REFRESH: every tile is JPEG again (the input thread has reset the CRCs)
	if (gV.refreshSeq != gV.seenRefresh)
	{
		gV.seenRefresh = gV.refreshSeq;
		// the JPEG of the whole screen fills the window of the viewer: what the video is refused meanwhile is no sign of the link; about a quarter of a byte a pixel
		kvm_auto_note_key(&gV.au, now, gV.rc.rate > 0 ? (uint32_t)((unsigned long long)gV.codedW * (unsigned long long)gV.codedH / 4ull * 1000ull / gV.rc.rate) : 0u);
		if (gV.tiles.st != NULL)
		{
			barrier(&gV.st.barRefresh);		// a frame in flight is written before the JPEGs of the REFRESH go: the P frames go on, no key frame is needed
			(void)kvm_vtiles_take_video(&gV.tiles, gV.map, 1);
		}
	}
	n = gV.nreq;
	for (i = 0; i < n; i++) { handle_request(i, now); }
	gV.nreq = 0;
	// the retry after an error of the encoder (a new generation, like op 1)
	if (gV.ctl.state == KVM_VS_OFF && kvm_vctl_retry_due(&gV.ctl, now))
	{
		get_view(&v, now);
		if (v.want == KVM_WANT_VIDEO && !v.veto && v.ackActive)
		{
			gV.startPending = 1; gV.startKind = START_RETRY; gV.ctl.state = KVM_VS_WAIT;
		}
	}
	if (gV.startPending) { attempt_start(now); }
	// repair of the VIDEO tiles (I8): sent as JPEG again, at most once a second
	if (gV.ctl.state == KVM_VS_ON && kvm_vctl_repair_due(&gV.ctl, now) && gV.tiles.st != NULL)
	{
		unsigned k;
		barrier(&gV.st.barRepair);
		k = kvm_vtiles_take_video(&gV.tiles, gV.map, 0);
		if (k > 0) { tiles_to_jpeg(gV.map); }
		kvm_stats_log("video repair tiles=%u", k);
	}
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// a frame: started (try_start), collected and written (collect_one), the barriers
// ---------------------------------------------------------------------------------------------------------------------------------------

// The frame in flight (or the oldest) is waited for, collected and written: everything that has to be ordered after it (a JPEG of a tile that it covers, the JPEGs of a REFRESH, a repair, a change of
// the encoder's setting) calls this first. Bounded by the watchdog. `counter` says why it was needed.
static void barrier(long long *counter)
{
	LARGE_INTEGER b0, b1;
	long long us;
	if (!gV.sessionOn || kvm_encthr_inflight() == 0) { return; }
	QueryPerformanceCounter(&b0);
	(*counter)++;
	while (kvm_encthr_inflight() > 0)
	{
		if (collect_one()) { continue; }
		if (kvm_encthr_abandoned() || kvm_encthr_oldest_age(timeGetTime()) > KVM_ENC_WATCHDOG_MS) { encoder_hung("barrier"); break; }
		(void)kvm_encthr_wait(20);
	}
	QueryPerformanceCounter(&b1);
	us = (long long)qpc_us(b0, b1);
	gV.st.barUs += us;
	if (us > gV.st.barUsMax) { gV.st.barUsMax = us; }
}

// The encoder does not answer (a frame in it for longer than the watchdog allows, or a call that did not return): it is given up for good. Its thread, its buffers and what the frame in flight
// points to are left where they are, the tiles go back to JPEG, the video is off for the rest of the process (the same as after a structured exception, kvm_vctl_fault), the stream goes on as JPEG.
static void *gLeaked[VID_DEPTH];	// the frame buffers that a hung encoder may still write (nothing frees them; the tests do)
void kvm_video_leaked_free(void) { int k; for (k = 0; k < VID_DEPTH; k++) { free(gLeaked[k]); gLeaked[k] = NULL; } }

static void encoder_hung(const char *where)
{
	int k;
	gV.st.hung++;
	kvm_stats_log("video encoder hung backend=%s where=%s age_ms=%lu inflight=%d", gV.be != NULL ? gV.be->name : "-", where, (unsigned long)kvm_encthr_oldest_age(timeGetTime()), kvm_encthr_inflight());
	(void)kvm_encthr_abandon();
	for (k = 0; k < VID_DEPTH; k++) { gLeaked[k] = gV.sl[k].frame; gV.sl[k].buf = NULL; gV.sl[k].frame = NULL; gV.sl[k].frameCap = 0; }	// leaked: the thread may still read the picture and write the access unit
	cancel_inflight();
	if (gV.hw.active)
	{
		// the hardware encoder hangs in the driver: its thread is given up with everything it holds; the software encoder takes over on a new thread (the first and only return), the hang is an error of weight 2
		gV.haveEnc = 0;
		hw_fallback("hung", 2);
		return;
	}
	video_error("encoder hung", NULL, 1);
}

static void check_watchdog(void)
{
	if (!kvm_encthr_started() || kvm_encthr_inflight() == 0) { return; }
	if (kvm_encthr_oldest_age(timeGetTime()) > KVM_ENC_WATCHDOG_MS) { encoder_hung("watchdog"); }
}

// A frame of the picture that waits (gV.held) is started if the encoder is free and the window, the bucket and the pace allow it (the plan of the frame as before the encoder thread). fromEmit: the call comes from
// kvm_video_emit (once an iteration): only those count the refusals for the rate control; the calls from the service of the loop (the encoder has just finished, a slot has come) repeat the plan quietly.
static int try_start(int fromEmit)
{
	DWORD now;
	vid_view v;
	kvm_vplan_in in;
	kvm_vplan plan;
	kvm_ejob job;
	vslot *sl;
	int slotIdx, nRects, isKey, idrWhy, r, c;
	uint32_t coverTiles = 0;
	const size_t pitch = (size_t)gV.codedW * 4u;
	size_t hdr, i, nc;
	unsigned long long cpu0 = 0, cap0 = 0;
	if (!gV.sessionOn || gV.ctl.state != KVM_VS_ON || !have_encoder() || gV.tiles.st == NULL || gV.held == NULL) { return 0; }
	if (gV.tiles.nPending == 0) { release_held(); return 0; }	// the tiles went to JPEG meanwhile: no frame
	if (gV.tiles.rows != TILE_HEIGHT_COUNT || gV.tiles.cols != TILE_WIDTH_COUNT || gV.codedW != (int)kvm_stream_pad32((unsigned)SCALED_WIDTH) || gV.codedH != (int)kvm_stream_pad32((unsigned)SCALED_HEIGHT) || gV.heldCodedW != gV.codedW || gV.heldCodedH != gV.codedH)
	{
		release_held();	// the geometry changed under the picture: the next iteration starts over
		return 0;
	}
	slotIdx = kvm_encthr_free_slot();
	if (slotIdx < 0) { gV.heldBusy = 1; if (fromEmit) { gV.st.busyHeld++; } return 0; }	// the encoder still makes the last frame: the picture waits for it (and is replaced by a newer one meanwhile)
	sl = &gV.sl[slotIdx];
	if (sl->frame == NULL || sl->cover == NULL || sl->paintCrc == NULL) { return 0; }

	now = timeGetTime();
	get_view(&v, now);
	in.now = now; in.pending = gV.tiles.nPending; in.gateOpen = kvm_ack_peek(KVM_GATE_VIDEO); in.rateOpen = kvm_vrc_peek(&gV.rc, now); in.intervalMs = frame_interval_ms(); in.rate = v.rate;
	{
		kvm_glue_lock();
		kvm_vctl_plan(&gV.ctl, kvm_glue_stream(), &in, &plan);
		kvm_glue_unlock();
	}
	if (fromEmit && plan.held) { kvm_auto_note_held(&gV.au, 0); }
	if (fromEmit && plan.heldRate) { kvm_auto_note_held(&gV.au, 1); }
	if (plan.held) { if (fromEmit) { if (kvm_ack_note_link_held()) { kvm_vrc_window_refused(&gV.rc); } else { kvm_vrc_window_refused_soft(&gV.rc); } gV.st.held++; } return 0; } // pendingVideo keeps growing: that is "less fps"
	if (plan.heldRate)
	{
		// the byte bucket of the rate control is empty: the frame comes later and carries what has accumulated (never dropped); the refusal is what tells the regulator that the content wants more than R
		if (fromEmit) { (void)kvm_vrc_may_encode(&gV.rc, now, 0); gV.st.heldRate++; }
		return 0;
	}
	if (!plan.encode) { if (fromEmit) { gV.st.skippedPace++; } return 0; }
	if (!fromEmit) { gV.st.lateStarts++; }

	nRects = kvm_vtiles_make_rects(&gV.tiles, (unsigned)gV.screenW, (unsigned)gV.screenH, sl->rects, sl->cover);
	if (nRects <= 0) { video_error("rectangles of the frame", NULL, 0); return 0; }
	nc = (size_t)gV.tiles.rows * (size_t)gV.tiles.cols;
	for (i = 0; i < nc; i++) { if (sl->cover[i]) { coverTiles++; } }	// the area of the frame (kvm_qpdet judges the size per tile)
	hdr = kvm_stream_frame_header_len((unsigned)nRects);
	if (sl->frameCap <= hdr) { video_error("frame buffer", NULL, 0); return 0; }

	memset(&job, 0, sizeof(job));
	// the key frame: the first of a generation (on a reused encoder), asked for by op 3 (after the spacing), or after a lost access unit
	job.forceIdr = plan.forceIdr ? 1 : 0;
	// the quantizer of this frame: the regulator's; an IDR gets max(QP, the one at which it fits in 0.3 s of R), 26..38. Stage 2 (the encoder does not listen to the QP): the bit rate is R.
	// The encoder thread applies them before the frame (what is asked for is remembered here: appliedQp, appliedBps).
	idrWhy = kvm_vctl_idr_why(&gV.ctl, &plan);
	isKey = (gV.ctl.idrFirst || plan.forceIdr || gV.ctl.idrForce) ? 1 : 0;
	// the view of the encoder thread (A: frozen outside the rectangles, a complete key frame; B: a flat key frame): what the thread converts is planned here, from the tiles of this frame; the job gets a COPY
	{
		kvm_nv12v_pl vp;
		if (gV.view.ok && kvm_nv12v_plan(&gV.view.v, gV.tiles.rows, gV.tiles.cols, sl->cover, gV.tiles.st, gV.tiles.fl, isKey, &vp) == 0 && !vp.legacy)
		{
			job.viewMode = 1; job.viewFull = vp.full; job.viewFlat = vp.flat; job.mbCols = gV.view.v.mbCols; job.mbRows = gV.view.v.mbRows;
			job.viewMap = vp.map;										// (valid until the next plan; the submit copies it into the slot of the job)
			if (vp.full) { gV.st.convComplete++; }
			if (vp.flat) { gV.st.convFlat++; }
			gV.st.virginMbs += (long long)vp.virginMbs;
			sl->virginMbs = vp.virginMbs; sl->realMbs = vp.realMbs; sl->totalMbs = vp.totalMbs; sl->viewFull = vp.full; sl->viewFlat = vp.flat;
			// B: the key frame is proportional to what it converts: the rate control is told before it chooses the QP of the key frame (kvm_vrc_qp(isKey) below)
			if (gV.view.flat) { kvm_vrc_set_idr_cover(&gV.rc, (isKey && !vp.full) ? vp.realMbs : 0u, vp.totalMbs); }
		}
		else { sl->virginMbs = sl->realMbs = sl->totalMbs = 0; sl->viewFull = sl->viewFlat = 0; if (gV.view.flat) { kvm_vrc_set_idr_cover(&gV.rc, 0, 0); } }
	}
	if (gV.qpStage == 2)
	{
		const int want = (int)bitrate_for_level();
		if (gV.appliedBps == 0 || ((DWORD)(now - gV.bpsAt) >= 500 && (want > gV.appliedBps + gV.appliedBps / 8 || want < gV.appliedBps - gV.appliedBps / 8)))
		{
			job.setBitrate = want;
			gV.appliedBps = want; gV.bpsAt = now;
		}
	}
	else
	{
		int wantQp = kvm_vrc_qp(&gV.rc, isKey);
		if (!isKey && gV.qpStage <= 1)
		{
			wantQp += kvm_qpdet_probe_offset(&gV.qpdet, wantQp, KVM_VIDEO_QP_MIN, KVM_VIDEO_QP_MAX);	// a deliberate step now and then until it is known whether the encoder listens
		}
		if (wantQp != gV.appliedQp)
		{
			job.setQp = gV.hw.active ? kvm_vcore_hw_qp(wantQp) : wantQp;	// the encoder is given the mapped QP (kvm_vcore_hw_qp) ...
			gV.appliedQp = wantQp; gV.qp = wantQp;						// ... what is remembered (the rate control, the key frame prediction, the detector, the log) is the QP of the regulator
		}
	}
	// the picture is a bottom-up bitmap of the coded size: the top row is the last one in memory
	job.top = (const uint8_t *)gV.held + (pitch * (size_t)(gV.codedH - 1));
	job.width = gV.codedW; job.height = gV.codedH; job.stride = -(int)pitch;
	gV.ts += 333333; // 30 frames per second, nominal (mf_h264.h); with a constant quantizer the time stamps do not matter
	job.ts = gV.ts;
	job.out = sl->frame + hdr; job.outCap = sl->frameCap - hdr;
	job.gen = v.gen;
	sl->nRects = nRects; sl->hdr = hdr; sl->at = now; sl->hadFrame = gV.ctl.haveFrame; sl->prevSlot = gV.ctl.lastFrame;	// the slot of this frame follows the slot of the previous one, not the moment it was written
	sl->slotAt = kvm_pace_slot(sl->hadFrame, sl->prevSlot, now, in.intervalMs, &gV.slotLate);
	sl->idrWhy = idrWhy; sl->qpUsed = gV.qpStage == 2 ? 0 : gV.appliedQp; sl->coverTiles = coverTiles; sl->intervalMs = in.intervalMs; sl->rate = in.rate;
	sl->estBytes = gV.avgAu;
	if (isKey && gV.rc.lastIdrBytes > sl->estBytes) { sl->estBytes = gV.rc.lastIdrBytes; }
	if (gV.view.flat && gV.qpStage != 2)
	{
		// B: a key frame that converts a part of the picture is forecast by its area at the QP it gets; a P frame with virgin macroblocks costs their intra size on top of an ordinary one
		if (isKey) { sl->estBytes = kvm_vrc_idr_bytes(&gV.rc, sl->qpUsed != 0 ? sl->qpUsed : gV.rc.qp); }
		else { sl->estBytes = gV.avgAu + kvm_vrc_virgin_bytes(&gV.rc, sl->virginMbs, sl->qpUsed != 0 ? sl->qpUsed : gV.rc.qp); }
	}	// a key frame is many times a P frame: the last one, or the one that is expected before the first
	for (r = 0; r < gV.tiles.rows; r++) { for (c = 0; c < gV.tiles.cols; c++) { const size_t k = ((size_t)r * (size_t)gV.tiles.cols) + (size_t)c; if (sl->cover[k]) { sl->paintCrc[k] = tileInfo[r][c].contentCrc; } } }
	sl->buf = gV.held; gV.held = NULL; gV.heldBusy = 0;				// the picture goes to the job (its reference)

	kvm_vtiles_dispatch(&gV.tiles, sl->cover, slotIdx);	// INFLIGHT: the tiles of the frame are painted by it, and wait for it
	kvm_adapt_note_video_frame();						// the sharpening governor: the video moves, also while the frame is being made
	kvm_glue_lock(); kvm_stream_set_reserved(kvm_glue_stream(), sl->estBytes); kvm_glue_unlock();	// the JPEG and the sharpening count the frame as in flight
	if (gV.encMode == KVM_ENCTHR_INLINE) { cpu0 = kvm_glue_cpu_us(); cap0 = kvm_glue_capture_cpu_us(); }
	r = kvm_encthr_submit(&job, now);
	if (gV.encMode == KVM_ENCTHR_INLINE) { gV.inlineCpuUs = enc_cpu_us(cpu0, kvm_glue_cpu_us(), cap0, kvm_glue_capture_cpu_us()); }
	if (r < 0)
	{
		// cannot happen (the slot was free): the frame is given up and its tiles are painted by the next one
		(void)kvm_vtiles_abort(&gV.tiles, slotIdx);
		kvm_glue_lock(); kvm_stream_set_reserved(kvm_glue_stream(), 0); kvm_glue_unlock();
		release_slot(sl);
		return 0;
	}
	return 1;
}

// A job that the encoder has finished: the result is checked and written (the rules of the frame as before the encoder thread), the tiles of the frame land. A job that the main loop gave up (cancel) is only released.
static void process_result(int slot, const kvm_ejob *job, const kvm_ejres *r)
{
	vslot *sl = &gV.sl[slot];
	struct mfh264_err err;
	kvm_frame_params fp;
	LARGE_INTEGER w0, w1;
	DWORD now = sl->at;
	size_t len = r->len, total;
	const int isIdr = r->isIdr;
	int fd, res;
	uint32_t seq, curGen;
	unsigned us;
	memset(&err, 0, sizeof(err));
	err.code = r->code; err.hr = r->hr; err.stage = r->stage; err.reason = r->reason; err.exc_code = r->exc;

	kvm_glue_lock();
	kvm_stream_set_reserved(kvm_glue_stream(), 0);
	curGen = kvm_glue_stream()->gen;
	kvm_glue_unlock();

	if (job->gen != curGen)
	{
		// a result of a generation that is over: never written. The next frame is a key frame, and its tiles go back to JPEG
		unsigned n;
		memset(gV.map, 0, (size_t)gV.tiles.rows * (size_t)gV.tiles.cols);
		n = kvm_vtiles_cancel(&gV.tiles, slot, gV.map);
		if (n > 0) { tiles_to_jpeg(gV.map); }
		gV.ctl.idrForce = 1; gV.ctl.encFresh = 0;
		gV.st.staleGen++;
		kvm_stats_log("video result of an old generation dropped gen=%u now=%u", (unsigned)job->gen, (unsigned)curGen);
		return;
	}
	if (r->encUs > 0)
	{
		const long long encUs = (long long)r->encUs;
		kvm_budget_note_encode(&gV.budget, r->encUs, gV.encMode == KVM_ENCTHR_INLINE ? gV.inlineCpuUs : 0ull);
		gV.st.encUs += encUs;
		gV.encAvgUs = gV.encAvgUs == 0 ? (unsigned)encUs : (unsigned)((gV.encAvgUs * 7ull + (unsigned long long)encUs) / 8ull);	// moving average, 1/8
		if (encUs > gV.st.encUsMax) { gV.st.encUsMax = encUs; }
	}
	if (r->convMbs > 0) { gV.st.convUs += (long long)r->convUs; gV.st.convMbs += (long long)r->convMbs; gV.st.convFrames++; }
	if (r->phase == 1) { video_error("force key frame", &err, err.code == MFH264_E_EXCEPTION); return; }
	if (r->phase == 2) { video_error("set qp", &err, err.code == MFH264_E_EXCEPTION); return; }
	if (r->phase == 3) { video_error("set bitrate", &err, err.code == MFH264_E_EXCEPTION); return; }
	if (r->code == MFH264_E_HANG) { view_lost(); (void)kvm_vtiles_abort(&gV.tiles, slot); video_error("encode", &err, 0); return; }	// (the hardware encoder is dead: a deadline of its own passed)
	if (r->code == MFH264_E_EXCEPTION) { video_error("encode", &err, 1); return; }
	if (r->code == MFH264_E_STREAM_BROKEN || (r->code == 0 && len < 5))
	{
		// a lost access unit (nothing may be sent for this frame; the wrapper flushed the encoder and its next frame is an IDR): the tiles of the frame are painted again by the next frame, which is a
		// key frame. A frame bigger than 8 MiB, or too many in a row, is an error of the encoder.
		const int oversize = (r->code == MFH264_E_STREAM_BROKEN && r->reason == MFH264_BROKEN_OVERSIZE) ? 1 : 0;
		view_lost();
		(void)kvm_vtiles_abort(&gV.tiles, slot);
		gV.st.broken++;
		kvm_stats_log("video lost access unit reason=%d oversize_bytes=%lu stage=%s", r->reason, oversize ? (unsigned long)len : 0ul, r->stage != NULL ? r->stage : "-");
		if (kvm_vctl_frame_broken(&gV.ctl, oversize) == KVM_VB_ERROR) { video_error(oversize ? "frame over 8 MiB" : "lost access units", &err, 0); }
		return;
	}
	if (r->code == MFH264_E_INPUT && gV.qpStage == 1)
	{
		// the encoder does not take the QP in the input sample (ProcessInput fails): not an error of the encoder (three would end the video for the process), the way is given up and the
		// bit rate takes over; the tiles of the frame are painted by the first frame of the new generation
		view_lost();
		(void)kvm_vtiles_abort(&gV.tiles, slot);
		kvm_stats_log("video qp sample attribute refused hr=0x%08lx stage=%s: -> stage 2", (unsigned long)err.hr, err.stage != NULL ? err.stage : "-");
		gV.qpRestart = 2;
		return;
	}
	if (r->code != 0) { view_lost(); video_error("encode", &err, 0); return; }
	fd = kvm_vctl_frame_done(&gV.ctl, isIdr, len, now);
	if (fd == 0) { gV.ctl.lastFrame = sl->slotAt; }	// the slot, not the time of this frame
	if (fd == KVM_VF_BROKEN)
	{
		// the first frame of a generation has to be an IDR; this one is not: it is not sent, the next one is forced to be
		(void)kvm_vtiles_abort(&gV.tiles, slot);
		gV.st.broken++;
		kvm_stats_log("video first frame of the generation is not an IDR: not sent");
		return;
	}
	if (fd == KVM_VF_ERROR) { video_error("first frame is never an IDR", NULL, 0); return; }
	// does the encoder listen to the QP? (the frames it really made: the QP asked for, the QP it says it used, the size)
	if (gV.qpStage <= 1 && !isIdr)
	{
		int rep = -1;
		if (r->haveQp)
		{
			const uint64_t raw = r->qpRaw;
			const uint64_t pq = raw <= 63u ? raw : ((raw >> 16) & 0xFFFFu);	// a plain number, or the packed I | P << 16 | B << 32 of AVEncVideoEncodeQP (the P field)
			rep = pq <= 51u ? (int)pq : -1;
		}
		if (sl->virginMbs == 0 && kvm_qpdet_frame(&gV.qpdet, sl->qpUsed, rep, (uint32_t)len, 0, sl->coverTiles) == KVM_QPDET_INEFFECTIVE) { gV.qpRestart = gV.qpStage + 1; }
		else if (gV.qpdet.verdict == KVM_QPDET_EFFECTIVE && gV.qpdet.events > 0 && !gV.qpEffLogged) { gV.qpEffLogged = 1; kvm_stats_log("video qp effective stage=%d events=%u llr=%.1f", gV.qpStage, (unsigned)gV.qpdet.events, gV.qpdet.llr); }
	}
	if (isIdr)
	{
		int sp = 0, sl2 = 0, reason;
		struct kvm_sps_info info;
		// the viewer rejects an IDR with an SPS outside its limits (error of the stream, op 2, JPEG for good): the frame is not sent, the viewer gets a clean 242 and JPEG
		reason = sps_reason(sl->frame + sl->hdr, len, gV.codedW, gV.codedH, &info);
		if (reason != 0)
		{
			if (gV.hw.active)
			{
				// the SPS of the hardware encoder is outside what this viewer takes: a mismatch with the viewer, not a fault of the hardware (no error is written); the software encoder, whose profile and level follow the viewer
				kvm_stats_log("video sps refused (IDR) of the hardware encoder reason=%d profile=%d level=%d: software", reason, info.profile, info.level);
				(void)kvm_vtiles_abort(&gV.tiles, slot);
				hw_fallback("sps_refused", 0);
				return;
			}
			kvm_stats_log("video sps refused (IDR) reason=%d profile=%d constraint=0x%02x level=%d refs=%d mbs=%dx%d cropping=%d", reason, info.profile, info.constraint, info.level, info.numRefFrames, info.mbsW, info.mbsH, info.cropping);
			fail_request(reason, gV.codedW, gV.codedH);
			return;
		}
		if (kvm_annexb_sps_ids(sl->frame + sl->hdr, len, &sp, &sl2) == 0 && (sp != gV.profile || (gV.hw.active ? sl2 > gV.level : sl2 != gV.level)))
		{
			gV.st.spsMismatch++;
			kvm_stats_log("video sps differs from the 242: sps profile=%d level=%d announced profile=%d level=%d", sp, sl2, gV.profile, gV.level);
		}
		if (gV.hw.active && sl->idrWhy == KVM_IDR_WHY_NONE && ++gV.hw.unaskedIdr >= 2)
		{
			// the encoder makes key frames of its own (its GOP is finite): 300 KB every second is no video; back to the software encoder
			(void)kvm_vtiles_abort(&gV.tiles, slot);
			hw_fallback("periodic_idr", 1);
			return;
		}
		gV.st.idr++;
		gV.st.idrBytes += (long long)len;
		if (gV.view.on) { kvm_stats_log("video idr view real_mbs=%u total_mbs=%u complete=%d flat=%d bytes=%lu bytes_per_real_mb=%.1f forecast=%u", sl->realMbs, sl->totalMbs, sl->viewFull, sl->viewFlat, (unsigned long)len, sl->realMbs > 0 ? (double)len / (double)sl->realMbs : 0.0, sl->estBytes); }
	}
	if (isIdr || sl->idrWhy != KVM_IDR_WHY_NONE)
	{
		// why is there a key frame? (measured: one every 10 s, the recording of the server asked for it with op 3; the encoder makes none of its own)
		const unsigned long long rate = sl->rate;
		kvm_stats_log("video idr reason=%s frame_n=%u qp=%d qp_p=%d kb=%lu link_ms=%lu gap_ms=%lu since_idr_ms=%lu op3_n=%u op3_flags=%u op3_wait_ms=%lu op3_period_ms=%lu spacing_ms=%lu forced_total=%u",
					  kvm_idr_why_name(sl->idrWhy, isIdr), (unsigned)gV.ctl.frames, gV.qpStage == 2 ? gV.rc.qp : sl->qpUsed, gV.rc.qp, (unsigned long)(len / 1024), rate > 0 ? (unsigned long)((unsigned long long)len * 1000ull / rate) : 0ul,
					  gV.lastFrameAt != 0 ? (unsigned long)(DWORD)(now - gV.lastFrameAt) : 0ul, gV.lastIdrAt != 0 ? (unsigned long)(DWORD)(now - gV.lastIdrAt) : 0ul, gV.op3Count, gV.op3Flags,
					  (sl->idrWhy == KVM_IDR_WHY_OP3 && gV.op3At != 0) ? (unsigned long)(DWORD)(now - gV.op3At) : 0ul, (gV.op3Prev != 0 && gV.op3At != 0) ? (unsigned long)(DWORD)(gV.op3At - gV.op3Prev) : 0ul,
					  (unsigned long)kvm_vcore_idr_spacing(gV.ctl.lastIdrBytes, rate), (unsigned)gV.ctl.forcedIdrs);
		if (isIdr) { gV.lastIdrAt = now ? now : 1u; }
	}
	gV.lastFrameAt = now ? now : 1u;

	total = sl->hdr + len;
	memset(&fp, 0, sizeof(fp));
	fp.flags = (uint8_t)(isIdr ? KVM_FRAME_IDR : 0);
	fp.codedW = (uint16_t)gV.codedW; fp.codedH = (uint16_t)gV.codedH; fp.nRects = (unsigned)sl->nRects; fp.rects = sl->rects; fp.annexB = NULL; fp.annexLen = len;
	QueryPerformanceCounter(&w0);
	kvm_glue_lock();
	{
		kvm_stream *s = kvm_glue_stream();
		if (!s->videoOn || !s->ackActive)
		{
			kvm_glue_unlock();
			stop_video("stream state changed during the frame");
			return;
		}
		now = timeGetTime();
		seq = kvm_stream_add_position(s, now, total); // the frame is a position (rule M1): the viewer acknowledges it
		fp.seq = seq; fp.ts = now; fp.gen = s->gen;
		if (isIdr) { kvm_stream_watch(s, seq); gV.idrWatchBytes = (uint32_t)total; }
		if (kvm_stream_build_frame_header(sl->frame, sl->hdr, &fp) != sl->hdr)
		{
			kvm_glue_unlock();
			video_error("243 header", NULL, 0);
			return;
		}
		res = kvm_glue_write_locked((char *)sl->frame, (int)total, KVM_W_POSITION);
	}
	kvm_glue_unlock();
	QueryPerformanceCounter(&w1);

	// the frame is out: its tiles land (I3), and the ones that did not change meanwhile are VIDEO
	us = (unsigned)qpc_us(w0, w1);
	(void)kvm_vtiles_land(&gV.tiles, slot, gV.map);
	tiles_painted(gV.map, sl, now);
	kvm_vctl_note_write(&gV.ctl, now, (gV.encAvgUs + 500u) / 1000u);	// when this frame was written and how long a frame takes (the next one is not encoded right behind it)
	const unsigned virginBytes = (gV.view.flat && !isIdr && sl->virginMbs > 0) ? kvm_vrc_virgin_bytes(&gV.rc, sl->virginMbs, sl->qpUsed != 0 ? sl->qpUsed : gV.appliedQp) : 0u;	// the first touch of virgin macroblocks is not the size of a frame of this content
	kvm_vrc_frame_virgin(&gV.rc, now, (uint32_t)total, sl->qpUsed != 0 ? sl->qpUsed : gV.appliedQp, isIdr, gV.encAvgUs, virginBytes);
	if (isIdr) { kvm_auto_note_key(&gV.au, now, gV.rc.rate > 0 ? (uint32_t)((unsigned long long)total * 1000ull / gV.rc.rate) : 0u); }	// a key frame fills the window and the bucket: the windows around it are not judged
	else { kvm_auto_note_frame(&gV.au, now, sl->qpUsed != 0 ? sl->qpUsed : gV.appliedQp, (uint32_t)total); }
	if (!isIdr && virginBytes > 0) { kvm_auto_note_key(&gV.au, now, gV.rc.rate > 0 ? (uint32_t)((unsigned long long)virginBytes * 1000ull / gV.rc.rate) : 0u); }	// the first touch of virgin macroblocks is a key frame in all but name: the windows are not judged
	kvm_adapt_note_video_frame(); // the sharpening governor: the video moves
	kvm_adapt_note_write(us, (long long)total, 0); // the congestion signal and the byte bucket of WA-2 count the video bytes (the bucket holds back only JPEG)
	gV.st.frames++; gV.st.bytes += (long long)total; gV.st.rects += sl->nRects; gV.st.writeUs += us;
	gV.totalFrames++;
	if (!isIdr) { const unsigned long long content = total > virginBytes ? total - virginBytes : total; gV.avgAu = gV.avgAu == 0 ? (unsigned)content : (unsigned)(((unsigned long long)gV.avgAu * 7ull + content) / 8ull); }
	kvm_glue_video_written(res, (long long)total, us);
	if (gV.hw.active && gV.hw.tryOpen && !gV.hw.tryUsed)
	{
		// the start did not crash: the marker is now "in use" (it stays: a crash of the driver later, in a thread of its own, is counted by the parent from the exit code of this process)
		CacheOp uo;
		gV.hw.tryUsed = 1;
		memset(&uo, 0, sizeof(uo)); uo.op = CO_USED; uo.key = gV.hw.key;
		cache_do(&uo);
	}
}

// One job that is done is taken out of the encoder and handled. 1 = there was one.
static int collect_one(void)
{
	kvm_ejob job;
	kvm_ejres res;
	int cancelled = 0, slot;
	if (!kvm_encthr_started()) { return 0; }
	slot = kvm_encthr_collect(&job, &res, &cancelled);
	if (slot < 0) { return 0; }
	if (!cancelled && gV.sessionOn && gV.tiles.st != NULL && slot < VID_DEPTH) { process_result(slot, &job, &res); }
	if (slot < VID_DEPTH) { release_slot(&gV.sl[slot]); }
	return 1;
}

// The loop calls this whenever it may: the results of the encoder are written, the watchdog looks at the frame that is not done, and the picture that waits for a frame is started when the encoder is free and
// its slot has come. Never waits.
void kvm_video_service(void)
{
	if (!gV.sessionOn) { return; }
	check_watchdog();
	if (!gV.sessionOn) { return; }
	while (collect_one()) { }
	// only a picture that was held because the encoder was busy is started here: one that the pace, the write gap, the window or the bucket refused waits for the next emit with a newer picture, as before the encoder thread
	if (gV.held != NULL && gV.heldBusy && gV.sessionOn && try_start(0) && gV.encMode == KVM_ENCTHR_INLINE) { while (collect_one()) { } }
}

// After the tile loop of a frame: the picture of it (a bottom-up bitmap, PIXEL_SIZE 4, a kvm_fbuf buffer) is held for a frame of the pending tiles. The frame starts now if the encoder is free and the window,
// the pace and the bucket allow; else the picture waits (a newer one replaces it) and the loop starts the frame when it can (kvm_video_service).
void kvm_video_emit(const void *desktop, long long desktopSize)
{
	const size_t pitch = (size_t)gV.codedW * 4u;
	if (!gV.sessionOn) { return; }
	while (collect_one()) { }				// a frame that the encoder finished during the tile loop is written now, before this picture asks for the slot
	if (gV.ctl.state != KVM_VS_ON || !have_encoder() || gV.tiles.st == NULL || desktop == NULL) { return; }
	kvm_auto_note_image(&gV.au, timeGetTime(), gV.tiles.nPending);	// an image with (or without) tiles that wait for the video
	if (gV.tiles.nPending == 0) { release_held(); return; }
	if (kvm_fbuf_refs(desktop) < 0) { pending_to_jpeg(); return; }	// not a buffer of kvm_fbuf: the job could not hold it
	if (gV.tiles.rows != TILE_HEIGHT_COUNT || gV.tiles.cols != TILE_WIDTH_COUNT || gV.codedW != (int)kvm_stream_pad32((unsigned)SCALED_WIDTH) || gV.codedH != (int)kvm_stream_pad32((unsigned)SCALED_HEIGHT))
	{
		return; // the geometry changed under the frame: the next iteration starts over
	}
	if (PIXEL_SIZE != 4 || desktopSize < (long long)(pitch * (size_t)gV.codedH)) { pending_to_jpeg(); return; }
	if (gV.held != desktop)
	{
		kvm_fbuf_ref((void *)desktop);
		release_held();
		gV.held = (void *)desktop; gV.heldSize = desktopSize; gV.heldCodedW = gV.codedW; gV.heldCodedH = gV.codedH;
	}
	gV.heldBusy = 0;
	(void)try_start(1);
	if (gV.encMode == KVM_ENCTHR_INLINE) { while (collect_one()) { } }
}

// tile.cpp, get_tile_at: a JPEG of the rectangle is about to be made. A frame in flight that covers one of its tiles is written first: the JPEG shows newer content than the frame, and must come after it.
void kvm_video_before_jpeg(int row0, int col0, int row1, int col1)
{
	if (!gV.sessionOn || gV.tiles.st == NULL || gV.tiles.nInflight == 0) { return; }
	if (kvm_vtiles_rect_inflight(&gV.tiles, row0, col0, row1, col1)) { barrier(&gV.st.barJpeg); }
}

// The sharpening wrote `bytes`: the rate control pays for them while the video moves (kvm_vrc_note_sharpen)
void kvm_video_note_sharpen(unsigned bytes, int videoActive)
{
	if (!gV.sessionOn || gV.ctl.state != KVM_VS_ON) { return; }
	kvm_vrc_note_sharpen(&gV.rc, timeGetTime(), bytes, videoActive);
}

void kvm_video_stats_flush(int final)
{
	long long f;
	if (!gV.sessionOn) { return; }
	f = gV.st.frames;
	if (!final && f == 0 && gV.st.starts == 0 && gV.st.errors == 0 && gV.st.fallbackTiles == 0 && gV.st.claimedTiles == 0 && gV.ctl.state == KVM_VS_OFF) { return; } // quiet while nobody uses the video
	kvm_stats_log("video stats%s state=%d cap_video=%d frames=%lld idr=%lld idr_kb=%lld forced_idr=%u kb=%lld enc_ms_avg=%.1f enc_ms_max=%.1f write_ms_avg=%.1f rects_avg=%.1f claimed_tiles=%lld held=%lld held_rate=%lld pace_skips=%lld"
				  " lost_au=%lld errors=%lld fallback_tiles=%lld sps_mismatch=%lld pending=%u pending_max=%lld video_tiles=%u qp=%d fps_cap=%d repairs=%u be=%s hw_disabled=%d hw_why=%s",
				  final ? " final=1" : "", (int)gV.ctl.state, (int)gV.ctl.capVideo, f, gV.st.idr, gV.st.idrBytes / 1024, (unsigned)gV.ctl.forcedIdrs, gV.st.bytes / 1024,
				  f > 0 ? (double)gV.st.encUs / (double)f / 1000.0 : 0.0, (double)gV.st.encUsMax / 1000.0, f > 0 ? (double)gV.st.writeUs / (double)f / 1000.0 : 0.0, f > 0 ? (double)gV.st.rects / (double)f : 0.0,
				  gV.st.claimedTiles, gV.st.held, gV.st.heldRate, gV.st.skippedPace, gV.st.broken, gV.st.errors, gV.st.fallbackTiles, gV.st.spsMismatch,
				  gV.tiles.st != NULL ? gV.tiles.nPending : 0u, gV.st.pendingMax, gV.tiles.st != NULL ? gV.tiles.nVideo : 0u, gV.qp, gV.fpsCap, (unsigned)gV.ctl.repairs, gV.hw.active ? "hw" : "sw", gV.hw.disabled, gV.hw.why);
	if (gV.view.on) { kvm_stats_log("video view mode=%s frames=%lld conv_us_avg=%.0f conv_mb_avg=%.0f complete=%lld flat=%lld virgin_mb=%lld", gV.view.flat ? "flat" : "frozen", gV.st.convFrames, gV.st.convFrames > 0 ? (double)gV.st.convUs / (double)gV.st.convFrames : 0.0, gV.st.convFrames > 0 ? (double)gV.st.convMbs / (double)gV.st.convFrames : 0.0, gV.st.convComplete, gV.st.convFlat, gV.st.virginMbs); }
	if (final || gV.st.busyHeld > 0 || gV.st.barJpeg > 0 || gV.st.barRefresh > 0 || gV.st.barRepair > 0 || gV.st.barOther > 0 || gV.st.cancelled > 0 || gV.st.lateStarts > 0 || gV.st.hung > 0 || gV.st.staleGen > 0)
	{
		const long long nb = gV.st.barJpeg + gV.st.barRefresh + gV.st.barRepair + gV.st.barOther;
		kvm_stats_log("video enc thread=%d inflight=%d busy_held=%lld late_starts=%lld bar_jpeg=%lld bar_refresh=%lld bar_repair=%lld bar_other=%lld bar_ms_avg=%.2f bar_ms_max=%.2f cancelled=%lld stale_gen=%lld hung=%d",
					  gV.encMode == KVM_ENCTHR_THREADED ? 1 : 0, kvm_encthr_inflight(), gV.st.busyHeld, gV.st.lateStarts, gV.st.barJpeg, gV.st.barRefresh, gV.st.barRepair, gV.st.barOther, nb > 0 ? (double)gV.st.barUs / (double)nb / 1000.0 : 0.0, (double)gV.st.barUsMax / 1000.0, gV.st.cancelled, gV.st.staleGen, kvm_encthr_abandoned());
	}
	memset(&gV.st, 0, sizeof(gV.st));
}

#endif // KVM_VIDEO_ENABLED
