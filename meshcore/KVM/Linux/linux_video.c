/*
KVM (Linux): the H.264 video path of the capture process. See linux_video.h.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#include "linux_video.h"

#ifdef KVML_VIDEO_ENABLED

#include "linux_h264.h"
#include "linux_tileinfo.h"
#include "meshcore/KVM/common/kvm_annexb.h"
#include "meshcore/KVM/common/kvm_motion.h"
#include "meshcore/KVM/common/kvm_pace.h"
#include "meshcore/KVM/common/kvm_sharpen.h"
#include "meshcore/KVM/common/kvm_video_core.h"
#include "meshcore/KVM/common/kvm_video_rc.h"
#include "meshcore/KVM/common/kvm_yuv.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VID_MAX_REQ 4
#define VID_MAX_AU (KVM_FRAME_MAX_N - (4u * 1024u))		// an access unit above this would make a 243 bigger than the 8 MiB of the protocol
#define VID_SETTLE_MS 150				// a tile that has not changed for this long is sharpened (WA-2)
#define VID_SHARPEN_WANT 32768			// the bytes of sharpening that one iteration asks for (kvm_sharp_begin limits them while the video moves)
#define VID_TILE_BYTES 900				// what a JPEG tile is expected to cost: the number of tiles of an iteration follows from the bytes
#define VID_SHARPEN_TILES_MAX 96
#define VID_LOG_MS 1000

// per tile flags
#define TF_REFRESH 1					// sent only because the viewer was reset (a REFRESH): a picture, never motion
#define TF_SHARPEN 2					// marked for the sharpening: its CRC was changed on purpose so that the scan sends it, and it is not motion

enum { START_NONE = 0, START_OP1 = 1, START_GEOMETRY = 2, START_RETRY = 3 };

static struct
{
	int sessionOn, capReason, log;
	kvm_vctl ctl;
	kvm_vtiles tiles;
	kvm_motion motion;
	kvm_sharp sharp;
	kvm_vrc rc;
	kvm_late late;
	// the geometry the model was built for
	int rows, cols, screenW, screenH, codedW, codedH;
	// per tile (rows * cols)
	int32_t *contentCrc;				// the CRC of the content when it was last looked at (0x7FFFFFFF = never)
	uint32_t *lastChange, *noted;
	uint8_t *streak, *flags;
	uint8_t *map, *cover;				// scratch of rows * cols
	uint32_t stamp;						// number of the frame (the scan) that is running
	uint32_t now, lastFrameTick;		// the clock of the frame, and of the one before
	unsigned realChanged;				// tiles of this frame whose content really changed
	// the encoder and its pictures
	kvml_h264 *enc;
	int encW, encH, encLevel, encProfile;
	unsigned encFlags;
	int profile, level, fpsCap, qp, appliedQp;
	uint8_t *y;							// = the Y plane of the NV12 picture (codedW x codedH), the chroma of it is converted into u and v
	uint8_t *nv12, *u, *v, *bgra;		// the picture that the encoder sees: it is changed in the rectangles of a frame only (what is outside them stays as it was: the video is clipped to the rectangles)
	uint8_t *au, *frame;				// the access unit of the encoder; the 243 (header, rectangles, Annex B)
	size_t auCap, frameCap;
	int havePicture;
	// requests and starts
	kvm_ctrl req[VID_MAX_REQ];
	uint32_t reqAt[VID_MAX_REQ];
	int nreq, startPending, startKind;
	uint32_t genAt;
	uint32_t idrWatchBytes;
	uint32_t sharpPos;					// the tile where the next search for tiles to sharpen starts
	uint32_t encAvgUs;
	uint32_t avgAu;
	uint32_t logAt;
	// statistics of the period
	unsigned long long sFrames, sIdr, sBytes, sEncUs, sEncUsMax, sRects, sHeld, sHeldRate, sBroken, sErrors, sClaimed, sJpegBack, sSharpTiles, sSharpBytes, sStarts;
	unsigned long long totalFrames;
} gV;

static int32_t dms(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

static void vlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void vlog(const char *fmt, ...)
{
	va_list ap;
	if (!gV.log) { return; }
	va_start(ap, fmt);
	fputs("KVM video: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static uint64_t mono_us(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) { return 0; }
	return ((uint64_t)ts.tv_sec * 1000000ULL) + ((uint64_t)ts.tv_nsec / 1000ULL);
}

static int have_model(void) { return gV.sessionOn && gV.tiles.st != NULL && gV.contentCrc != NULL; }
static int model_current(void) { return have_model() && gV.rows == TILE_HEIGHT_COUNT && gV.cols == TILE_WIDTH_COUNT && gV.screenW == SCREEN_WIDTH && gV.screenH == SCREEN_HEIGHT; }

// ---------------------------------------------------------------------------------------------------------------------------------------
// tiles
// ---------------------------------------------------------------------------------------------------------------------------------------

// The tiles of the map go back to JPEG: the viewer's copy of them is not trusted any more, so the CRC is the one of a tile that was never sent and the next scan sends them
static void tiles_to_jpeg(const uint8_t *map)
{
	int r, c;
	if (g_tileInfo == NULL || !model_current()) { return; }
	for (r = 0; r < gV.rows; r++)
	{
		for (c = 0; c < gV.cols; c++)
		{
			if (map[((size_t)r * (size_t)gV.cols) + (size_t)c]) { g_tileInfo[r][c].crc = 0xFF; }
		}
	}
}

// I7: the video is off; the tiles that waited for a frame are sent as JPEG in the next iteration. The VIDEO tiles stay what they are (the sharpening handles them).
static void pending_to_jpeg(void)
{
	unsigned n;
	if (!have_model() || gV.map == NULL || gV.tiles.nPending == 0) { return; }
	n = kvm_vtiles_take_pending(&gV.tiles, gV.map);
	if (n > 0) { tiles_to_jpeg(gV.map); gV.sJpegBack += n; }
}

static void free_model(void)
{
	kvm_vtiles_free(&gV.tiles);
	kvm_motion_free(&gV.motion);
	free(gV.contentCrc); free(gV.lastChange); free(gV.noted); free(gV.streak); free(gV.flags); free(gV.map); free(gV.cover);
	gV.contentCrc = NULL; gV.lastChange = NULL; gV.noted = NULL; gV.streak = NULL; gV.flags = NULL; gV.map = NULL; gV.cover = NULL;
	gV.rows = gV.cols = 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the encoder and its pictures
// ---------------------------------------------------------------------------------------------------------------------------------------

static void free_pictures(void)
{
	free(gV.nv12); free(gV.u); free(gV.bgra); free(gV.au); free(gV.frame);
	gV.nv12 = gV.u = gV.v = gV.bgra = gV.au = gV.frame = gV.y = NULL;
	gV.auCap = gV.frameCap = 0;
	gV.havePicture = 0;
}

static void close_encoder(void)
{
	if (gV.enc != NULL) { kvml_h264_close(gV.enc); gV.enc = NULL; }
	free_pictures();
	gV.encW = gV.encH = gV.encLevel = gV.encProfile = 0;
	gV.appliedQp = 0;
}

static int alloc_pictures(int cw, int ch)
{
	const size_t pix = (size_t)cw * (size_t)ch;
	size_t au = pix / 2u;
	if (au < (1u << 20)) { au = 1u << 20; }
	if (au > VID_MAX_AU) { au = VID_MAX_AU; }
	free_pictures();
	gV.nv12 = (uint8_t *)malloc(kvm_yuv_nv12_size(cw, ch));
	gV.u = (uint8_t *)malloc(pix / 2u);					// U, then V behind it
	gV.bgra = (uint8_t *)calloc(pix, 4u);
	gV.auCap = au;
	gV.au = (uint8_t *)malloc(au);
	gV.frameCap = kvm_stream_frame_header_len(KVM_FRAME_MAX_RECTS) + au;
	gV.frame = (uint8_t *)malloc(gV.frameCap);
	if (gV.nv12 == NULL || gV.u == NULL || gV.bgra == NULL || gV.au == NULL || gV.frame == NULL) { free_pictures(); return -1; }
	gV.y = gV.nv12;
	gV.v = gV.u + (pix / 4u);
	memset(gV.nv12, 128, kvm_yuv_nv12_size(cw, ch));
	memset(gV.u, 128, pix / 2u);
	return 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the state of the video towards the stream layer
// ---------------------------------------------------------------------------------------------------------------------------------------

static void send_current_state(int reason)
{
	kvm_state_msg m;
	kvm_stream_state_msg(kvml_stream(), reason, &m);
	if (m.video) { m.profile = (uint8_t)gV.profile; m.level = (uint8_t)gV.level; m.codedW = (uint16_t)gV.codedW; m.codedH = (uint16_t)gV.codedH; }
	kvml_state(&m);
}

// The request cannot be met (5.2: reasons 2, 3, 7, 8): the video is off, ACK stays (want = ACK), and the viewer is told. cw / ch: the size that was refused (reason 7).
static void fail_request(int reason, int cw, int ch)
{
	kvm_state_msg m;
	kvm_stream *s = kvml_stream();
	if (s->want == KVM_WANT_VIDEO) { s->want = KVM_WANT_ACK; }
	s->videoOn = 0;
	kvm_stream_state_msg(s, reason, &m);
	if (reason == KVM_REASON_RESOLUTION) { m.codedW = (uint16_t)cw; m.codedH = (uint16_t)ch; }
	kvml_state(&m);
	kvm_vctl_stop(&gV.ctl);
	gV.startPending = 0; gV.startKind = START_NONE;
	close_encoder();
	pending_to_jpeg();
	vlog("request failed reason=%d coded=%dx%d", reason, cw, ch);
}

// The generation could not start for a reason that is not the request's (the stream layer changed its mind): nothing is sent, the stream layer has said what is needed
static void cancel_start(void)
{
	kvm_vctl_stop(&gV.ctl);
	gV.startPending = 0; gV.startKind = START_NONE;
	pending_to_jpeg();
}

// The video stops running (op 2, op 4, op 5, no ACK, ...): the stream layer has answered with 242 already (videoOn = 0)
static void stop_video(const char *why)
{
	kvm_vctl_stop(&gV.ctl);
	gV.startPending = 0; gV.startKind = START_NONE;
	close_encoder();
	pending_to_jpeg();
	vlog("stop why=%s frames=%llu", why, gV.totalFrames);
}

// An error of the encoder or a frame that cannot be sent (5.2): the video is off for 60 s (after the third error, for good), 242 reason 4, the pending tiles go as JPEG
static void video_error(const char *what, int code)
{
	const uint32_t now = kvml_clock();
	int retry;
	kvm_state_msg m;
	kvm_stream *s = kvml_stream();
	gV.sErrors++;
	retry = kvm_vctl_error(&gV.ctl, now);
	gV.startPending = 0; gV.startKind = START_NONE;
	s->videoOn = 0;		// want stays VIDEO: the retry is ours
	if (!gV.ctl.capVideo) { s->capVideo = 0; s->videoOffReason = KVM_REASON_DISABLED; }
	kvm_stream_state_msg(s, gV.ctl.capVideo ? KVM_REASON_ERROR : KVM_REASON_DISABLED, &m);	// reason 4 promises a retry (7.1); the 3rd error ends the video for the process: reason 6
	kvml_state(&m);
	close_encoder();
	pending_to_jpeg();
	vlog("error what=%s code=%d errors=%d retry=%d cap_video=%d", what, code, (int)gV.ctl.errorCount, retry, (int)gV.ctl.capVideo);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the model of the tiles
// ---------------------------------------------------------------------------------------------------------------------------------------

static void geometry_check(uint32_t now)
{
	size_t n;
	int i;
	if (!gV.sessionOn) { return; }
	if (TILE_HEIGHT_COUNT <= 0 || TILE_WIDTH_COUNT <= 0 || SCREEN_WIDTH <= 0 || SCREEN_HEIGHT <= 0) { return; }
	if (model_current()) { return; }
	// the picture has another size (cmd 7): the frames in flight (there are none: the encoder is called in line) and the model of the tiles are of the old grid; the tiles are all JPEG
	// again and a running video begins a new generation (5.2), with a new encoder when the coded size changed
	free_model();
	gV.rows = TILE_HEIGHT_COUNT; gV.cols = TILE_WIDTH_COUNT; gV.screenW = SCREEN_WIDTH; gV.screenH = SCREEN_HEIGHT;
	gV.codedW = (int)kvm_stream_pad32((unsigned)SCREEN_WIDTH); gV.codedH = (int)kvm_stream_pad32((unsigned)SCREEN_HEIGHT);
	n = (size_t)gV.rows * (size_t)gV.cols;
	kvm_motion_init(&gV.motion);
	if (kvm_vtiles_init(&gV.tiles, gV.rows, gV.cols) != 0
		|| (gV.contentCrc = (int32_t *)malloc(n * sizeof(int32_t))) == NULL || (gV.lastChange = (uint32_t *)calloc(n, sizeof(uint32_t))) == NULL
		|| (gV.noted = (uint32_t *)calloc(n, sizeof(uint32_t))) == NULL || (gV.streak = (uint8_t *)calloc(n, 1)) == NULL || (gV.flags = (uint8_t *)calloc(n, 1)) == NULL
		|| (gV.map = (uint8_t *)malloc(n)) == NULL || (gV.cover = (uint8_t *)malloc(n)) == NULL)
	{
		free_model();	// no memory: no video (the tiles go as JPEG); the same as an error of the encoder
		if (gV.ctl.state != KVM_VS_OFF) { video_error("tile model", 0); }
		return;
	}
	(void)kvm_motion_resize(&gV.motion, gV.rows, gV.cols);
	for (i = 0; (size_t)i < n; i++) { gV.contentCrc[i] = 0x7FFFFFFF; }
	gV.stamp = 0; gV.lastFrameTick = now; gV.realChanged = 0;
	if (gV.ctl.state != KVM_VS_OFF)
	{
		close_encoder();	// (the coded size may be another one; a start that finds the same size makes a new encoder anyway: the model of the picture is gone)
		gV.ctl.state = KVM_VS_WAIT;
		gV.genAt = now;
		gV.startPending = 1; gV.startKind = START_GEOMETRY;
	}
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the start of a generation
// ---------------------------------------------------------------------------------------------------------------------------------------

static void attempt_start(uint32_t now)
{
	kvm_stream *s = kvml_stream();
	kvm_state_msg m;
	int cw, ch, level = 0, fpsCap = 0, profile, fresh = 0, err = 0;
	if (s->want != KVM_WANT_VIDEO || s->vetoMask != 0 || !s->ackActive || !gV.ctl.capVideo) { cancel_start(); return; }
	kvm_vctl_apply_limits(&gV.ctl);	// the limits of an op 1 that was joined with a start take effect with this generation
	if (!model_current()) { return; }
	cw = gV.codedW; ch = gV.codedH;
	if ((gV.ctl.maxW != 0 && cw > (int)gV.ctl.maxW) || (gV.ctl.maxH != 0 && ch > (int)gV.ctl.maxH) || kvm_vcore_pick_level((unsigned)cw, (unsigned)ch, gV.ctl.maxLevel, &level, &fpsCap) != 0)
	{
		fail_request(KVM_REASON_RESOLUTION, cw, ch);
		return;
	}
	profile = kvm_vcore_pick_profile(gV.ctl.profFlags, 101);
	if (profile == 0) { fail_request(KVM_REASON_PROFILE, 0, 0); return; }
	if (gV.enc == NULL || gV.encW != cw || gV.encH != ch || gV.encLevel != level || gV.encFlags != gV.ctl.profFlags)
	{
		int p, qp;
		if (!kvm_vctl_open_allowed(&gV.ctl, now)) { gV.ctl.state = KVM_VS_WAIT; return; }	// at most one encoder in 2 s: the tiles go as JPEG meanwhile
		close_encoder();
		kvm_vctl_open_note(&gV.ctl, now);
		qp = kvm_vcore_clamp_qp(kvm_vrc_qp(&gV.rc, 1));
		if (alloc_pictures(cw, ch) != 0) { fail_request(KVM_REASON_INIT, 0, 0); return; }
		for (p = profile; p != 0 && gV.enc == NULL; p = kvm_vcore_pick_profile(gV.ctl.profFlags, p))
		{
			gV.enc = kvml_h264_open(cw, ch, p, level, qp, KVM_VIDEO_MAX_FPS, &err);	// (the profile that the viewer's list allows and the encoder accepts, the highest first)
			if (gV.enc != NULL) { profile = p; }
		}
		if (gV.enc == NULL)
		{
			vlog("open failed size=%dx%d level=%d flags=%u err=%d", cw, ch, level, gV.ctl.profFlags, err);
			if (err == KVML_H264_E_NOLIB || err == KVML_H264_E_SYMBOL || err == KVML_H264_E_VERSION)
			{
				kvm_stream *st = kvml_stream();
				st->capVideo = 0; st->videoOffReason = KVM_REASON_NO_MF; gV.ctl.capVideo = 0;
				fail_request(KVM_REASON_NO_MF, 0, 0);
			}
			else { fail_request(err == KVML_H264_E_PARAM ? KVM_REASON_PROFILE : KVM_REASON_INIT, 0, 0); }
			return;
		}
		gV.encW = cw; gV.encH = ch; gV.encLevel = level; gV.encProfile = profile; gV.encFlags = gV.ctl.profFlags;
		gV.appliedQp = qp; gV.qp = qp;
		fresh = 1;
	}
	gV.profile = gV.encProfile; gV.level = level; gV.fpsCap = fpsCap;
	if (kvm_vctl_begin_gen(&gV.ctl, s, gV.profile, gV.level, (unsigned)cw, (unsigned)ch, fresh, &m) != 0) { cancel_start(); return; }
	kvml_state(&m);
	gV.havePicture = 0;				// the first frame of a generation is a key frame of a flat picture with the rectangles in it
	gV.genAt = now;
	if (fresh) { kvm_vrc_init_backend(&gV.rc, gV.rc.qp, now, 0); kvm_vrc_set_picture(&gV.rc, (uint32_t)cw * (uint32_t)ch); gV.encAvgUs = 0; gV.avgAu = 0; }
	gV.startPending = 0;
	gV.sStarts++;
	if (gV.startKind == START_OP1 || gV.startKind == START_RETRY)
	{
		// op 1 repairs the VIDEO tiles like an op 3 with both flags (5.2): they are sent as JPEG again
		const unsigned n = kvm_vtiles_take_video(&gV.tiles, gV.map, 0);
		if (n > 0) { tiles_to_jpeg(gV.map); }
		kvm_vctl_repair_now(&gV.ctl, now);
	}
	vlog("start kind=%d gen=%d profile=%d level=%d coded=%dx%d fps_cap=%d qp=%d fresh=%d", gV.startKind, (int)m.gen, gV.profile, gV.level, cw, ch, fpsCap, gV.qp, fresh);
	gV.startKind = START_NONE;
}

// Request of the viewer (op 1) that the stream layer accepted: the window is on and want is VIDEO there. A later command of the same poll may have changed that; then it is dropped (the
// stream layer has answered it).
static void handle_request(int i, uint32_t now)
{
	const kvm_stream *s = kvml_stream();
	const kvm_ctrl c = gV.req[i];
	if (s->want != KVM_WANT_VIDEO || s->vetoMask != 0 || !s->ackActive) { return; }
	if (!gV.ctl.capVideo) { fail_request(KVM_REASON_DISABLED, 0, 0); return; }
	if (gV.ctl.inError) { kvm_vctl_limits(&gV.ctl, &c); send_current_state(KVM_REASON_ERROR); return; }	// after an error the video stays off until the retry (60 s)
	if (kvm_vctl_op1(&gV.ctl, &c, gV.reqAt[i]) == KVM_VOP1_COALESCE) { send_current_state(KVM_REASON_OK); return; }	// joined with the previous op 1: the answer is the state now
	gV.startPending = 1; gV.startKind = START_OP1;
	if (gV.ctl.state == KVM_VS_OFF) { gV.ctl.state = KVM_VS_WAIT; }
	attempt_start(now);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the interface
// ---------------------------------------------------------------------------------------------------------------------------------------

int kvml_video_session_begin(int *reason)
{
	const char *e = getenv("MESH_KVM_VIDEO");
	char why[200];
	int cap = 1, r = KVM_REASON_OK;
	memset(&gV, 0, sizeof(gV));
	if (e != NULL && e[0] == '0') { cap = 0; r = KVM_REASON_DISABLED; }
	else if (kvml_h264_load(why, sizeof(why)) != KVML_H264_OK) { cap = 0; r = KVM_REASON_NO_MF; }
	e = getenv("MESH_KVM_STREAM_LOG");
	gV.log = (e != NULL && e[0] != 0 && e[0] != '0') ? 1 : 0;
	kvm_vctl_init(&gV.ctl, cap);
	kvm_vctl_set_threaded(&gV.ctl, 0);
	kvm_vrc_init(&gV.rc, KVM_VIDEO_QP, kvml_clock());
	kvm_sharp_init(&gV.sharp, kvml_clock());
	kvm_motion_init(&gV.motion);
	gV.capReason = r;
	gV.sessionOn = 1;
	if (reason != NULL) { *reason = r; }
	if (cap) { vlog("start cap_video=1 %s qp=%d qp_min=%d", why, KVM_VIDEO_QP, KVM_VIDEO_QP_MIN); }
	else { vlog("start cap_video=0 reason=%d %s", r, r == KVM_REASON_NO_MF ? why : "switched off"); }
	return cap;
}

void kvml_video_session_end(void)
{
	if (!gV.sessionOn) { return; }
	close_encoder();
	free_model();
	gV.sessionOn = 0;
}

void kvml_video_ctrl(const kvm_ctrl *c, unsigned actions, uint32_t atMs)
{
	if (!gV.sessionOn) { return; }
	if ((actions & KVM_ACT_VIDEO_REQUEST) && gV.nreq < VID_MAX_REQ) { gV.req[gV.nreq] = *c; gV.reqAt[gV.nreq] = atMs; gV.nreq++; }
	if (actions & (KVM_ACT_IDR | KVM_ACT_REPAIR)) { kvm_vctl_op3(&gV.ctl, actions); }
}

#ifdef KVML_TEST
// the tests look at the model of the tiles and the counters
void kvml_video_test_state(int *state, unsigned *pending, unsigned *video, unsigned *inflight, unsigned long long *frames, unsigned long long *idrs, unsigned long long *held)
{
	if (state != NULL) { *state = gV.ctl.state; }
	if (pending != NULL) { *pending = gV.tiles.nPending; }
	if (video != NULL) { *video = gV.tiles.nVideo; }
	if (inflight != NULL) { *inflight = gV.tiles.nInflight; }
	if (frames != NULL) { *frames = gV.totalFrames; }
	if (idrs != NULL) { *idrs = gV.sIdr; }
	if (held != NULL) { *held = gV.sHeld; }
}
unsigned long long kvml_video_test_sharp_bytes(void) { return gV.sSharpBytes; }
int kvml_video_test_tile(int row, int col) { return model_current() ? kvm_vtiles_get(&gV.tiles, row, col) : -1; }
#endif

int kvml_video_on(void) { return (gV.sessionOn && gV.ctl.state == KVM_VS_ON) ? 1 : 0; }

void kvml_video_refresh(void)
{
	size_t i, n;
	if (!model_current()) { return; }
	n = (size_t)gV.rows * (size_t)gV.cols;
	for (i = 0; i < n; i++) { gV.flags[i] = (uint8_t)((gV.flags[i] | TF_REFRESH) & ~TF_SHARPEN); }	// every tile is sent as a picture (the CRCs are reset by the command)
}

void kvml_video_poll(void)
{
	const uint32_t now = kvml_clock();
	kvm_stream *s = kvml_stream();
	int i, n;
	if (!gV.sessionOn) { return; }
	geometry_check(now);
	// the stream layer switched the video off (op 2, op 4, op 5, no ACK for 15 s): it has answered; release the encoder and send the pending tiles as JPEG (I7)
	if (gV.ctl.state == KVM_VS_ON && !s->videoOn) { stop_video("stream layer"); }
	else if (gV.ctl.state == KVM_VS_WAIT && (s->want != KVM_WANT_VIDEO || s->vetoMask != 0 || !s->ackActive)) { stop_video("request withdrawn"); }
	// rate control: the capacity estimate and R every iteration, the QP regulator every 500 ms, one log line a second
	if (gV.ctl.state == KVM_VS_ON)
	{
		kvm_vrc_update(&gV.rc, now, 0, kvm_stream_rate_or_mem(s, now), kvm_stream_link_limited(s, now));
		(void)kvm_vrc_tick(&gV.rc, now);
		if (gV.log && (uint32_t)(now - gV.logAt) >= VID_LOG_MS)
		{
			const uint32_t dt = gV.logAt != 0 ? (uint32_t)(now - gV.logAt) : VID_LOG_MS;
			vlog("rc r_kbps=%llu rate_kbps=%llu qp=%d fps=%.1f idr=%u video_kb=%llu sharpen_kb=%llu pending=%u video_tiles=%u held=%llu held_rate=%llu enc_ms_avg=%.1f enc_ms_max=%.1f inflight_kb=%llu win_kb=%u",
				(unsigned long long)(gV.rc.rate * 8 / 1000), (unsigned long long)(kvm_stream_rate(s, now) * 8 / 1000), gV.rc.qp, (double)gV.rc.sFrames * 1000.0 / (double)dt, gV.rc.sIdr,
				(unsigned long long)(gV.rc.sBytes / 1024), (unsigned long long)(gV.sSharpBytes / 1024), gV.tiles.nPending, gV.tiles.nVideo, gV.sHeld, gV.sHeldRate,
				gV.rc.sFrames > 0 ? (double)gV.rc.sEncUs / (double)gV.rc.sFrames / 1000.0 : 0.0, (double)gV.rc.sEncMaxUs / 1000.0,
				(unsigned long long)(kvm_stream_inflight(s) / 1024ull), (unsigned)(kvm_stream_window(s, now) / 1024u));
			gV.logAt = now; kvm_vrc_clear_stats(&gV.rc);
		}
	}
	// the viewer acknowledged the last key frame: what the link took of it is the evidence that sizes the next one when the link is not full
	if (gV.ctl.state == KVM_VS_ON && gV.idrWatchBytes != 0)
	{
		uint32_t ackAt = 0, ts = 0, queueMs = 0, minRtt = 0;
		if (kvm_stream_watch_result(s, &ackAt, &ts, &queueMs, &minRtt))
		{
			kvm_vrc_idr_acked(&gV.rc, now, gV.idrWatchBytes, ts, ackAt, queueMs, minRtt);
			vlog("idr acked kb=%u ms=%ld queue_ms=%u min_rtt_ms=%u", gV.idrWatchBytes / 1024u, (long)(int32_t)(ackAt - ts), queueMs, minRtt);
			gV.idrWatchBytes = 0;
		}
	}
	n = gV.nreq;
	for (i = 0; i < n; i++) { handle_request(i, now); }
	gV.nreq = 0;
	// the retry after an error of the encoder (a new generation, like op 1)
	if (gV.ctl.state == KVM_VS_OFF && kvm_vctl_retry_due(&gV.ctl, now) && s->want == KVM_WANT_VIDEO && s->vetoMask == 0 && s->ackActive)
	{
		gV.startPending = 1; gV.startKind = START_RETRY; gV.ctl.state = KVM_VS_WAIT;
	}
	if (gV.startPending) { attempt_start(now); }
	// repair of the VIDEO tiles (I8): sent as JPEG again, at most once a second
	if (gV.ctl.state == KVM_VS_ON && kvm_vctl_repair_due(&gV.ctl, now) && have_model() && gV.map != NULL)
	{
		const unsigned k = kvm_vtiles_take_video(&gV.tiles, gV.map, 0);
		if (k > 0) { tiles_to_jpeg(gV.map); }
		vlog("repair tiles=%u", k);
	}
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the tile loop
// ---------------------------------------------------------------------------------------------------------------------------------------

// The tiles that have been painted by the video and have been still for VID_SETTLE_MS are marked for the sharpening: the CRC is made different so that the scan of this frame sends them (the
// window gate of the sharpening and the share of the bucket of kvm_sharp limit how many). Also after the video has stopped: the VIDEO tiles must not stay blurred.
static void mark_sharpening(uint32_t now)
{
	kvm_stream *s = kvml_stream();
	int64_t allowed;
	uint64_t window;
	unsigned maxTiles, marked = 0, k;
	const size_t n = (size_t)gV.rows * (size_t)gV.cols;
	if (!model_current() || g_tileInfo == NULL || gV.tiles.nVideo == 0) { return; }
	window = kvm_stream_ack_active(s) ? kvm_stream_window(s, now) : 0;
	allowed = kvm_sharp_begin(&gV.sharp, now, kvm_stream_rate_recent(s, now), window, VID_SHARPEN_WANT);
	if (allowed <= 0) { return; }
	maxTiles = (unsigned)(allowed / VID_TILE_BYTES);
	if (maxTiles < 1) { maxTiles = 1; }
	if (maxTiles > VID_SHARPEN_TILES_MAX) { maxTiles = VID_SHARPEN_TILES_MAX; }
	for (k = 0; k < n && marked < maxTiles; k++)
	{
		const size_t i = (size_t)((gV.sharpPos + k) % (uint32_t)n);
		const int r = (int)(i / (size_t)gV.cols), c = (int)(i % (size_t)gV.cols);
		if (kvm_vtiles_get(&gV.tiles, r, c) != KVM_VT_VIDEO || kvm_vtiles_inflight(&gV.tiles, r, c)) { continue; }
		if (!kvm_sharp_settled(now, gV.lastChange[i], VID_SETTLE_MS) || (gV.flags[i] & TF_SHARPEN)) { continue; }
		gV.flags[i] |= TF_SHARPEN;
		g_tileInfo[r][c].crc ^= 0x2A5A5A5;	// not what the viewer was given: the scan sees the tile as changed
		if (g_tileInfo[r][c].crc == 0xFF) { g_tileInfo[r][c].crc = 0xFE; }
		marked++;
		gV.sharpPos = (uint32_t)((i + 1) % n);
	}
	gV.sSharpTiles += marked;
}

void kvml_video_frame_begin(void)
{
	uint32_t now;
	if (!gV.sessionOn) { return; }
	now = kvml_clock();
	geometry_check(now);
	if (!model_current()) { return; }
	gV.now = now;
	gV.stamp++;
	kvm_motion_frame(&gV.motion, gV.lastFrameTick, gV.realChanged);	// the frame of motion of the last 250 ms (the rule of the video looks at it instead of the frame before this one)
	if (gV.realChanged >= KVM_MOTION_TILES) { kvm_sharp_motion(&gV.sharp, gV.lastFrameTick); }
	gV.realChanged = 0;
	gV.lastFrameTick = now;
	kvm_sharp_set_interval(&gV.sharp, (uint32_t)(FRAME_RATE_TIMER > 0 ? FRAME_RATE_TIMER : 100));
	if (gV.tiles.nPending > 0) { kvm_sharp_motion(&gV.sharp, now); }
	mark_sharpening(now);
}

void kvml_video_note_crc(int row, int col, int crc)
{
	size_t i;
	if (!model_current() || row < 0 || col < 0 || row >= gV.rows || col >= gV.cols) { return; }
	i = ((size_t)row * (size_t)gV.cols) + (size_t)col;
	if (gV.noted[i] == gV.stamp) { return; }
	gV.noted[i] = gV.stamp;
	if (g_tileInfo[row][col].crc == 0xFF) { gV.flags[i] |= TF_REFRESH; }	// sent only because the viewer was reset, not because of a change
	if (crc != gV.contentCrc[i])
	{
		gV.streak[i] = (uint8_t)kvm_motion_note_change(&gV.motion, gV.streak[i], dms(gV.now, gV.lastChange[i]), 0);
		gV.lastChange[i] = gV.now;
		gV.flags[i] = (uint8_t)(gV.flags[i] & ~TF_SHARPEN);	// the content moved on: no longer a tile to sharpen
		if (gV.contentCrc[i] != 0x7FFFFFFF) { gV.realChanged++; }	// a tile that was never looked at (a new geometry, a new session) is not motion
		gV.contentCrc[i] = crc;
	}
}

int kvml_video_class(int row, int col)
{
	size_t i;
	if (!model_current() || gV.ctl.state != KVM_VS_ON || gV.enc == NULL || row < 0 || col < 0 || row >= gV.rows || col >= gV.cols) { return 0; }
	i = ((size_t)row * (size_t)gV.cols) + (size_t)col;
	if (gV.flags[i] & (TF_REFRESH | TF_SHARPEN)) { return 0; }
	// with the video running a tile is motion when it has changed twice in a row while a frame of motion was seen in the last 250 ms, or when it is within two tiles of one that is (the first
	// change of the edge of a dragged window, the next frame of an animation). A tile that is sent only because of a REFRESH is not.
	return kvm_motion_class(&gV.motion, gV.now, row, col, gV.streak[i], dms(gV.now, gV.lastChange[i]));
}

int kvml_video_tile_sharpening(int row, int col)
{
	if (!model_current() || row < 0 || col < 0 || row >= gV.rows || col >= gV.cols) { return 0; }
	return (gV.flags[((size_t)row * (size_t)gV.cols) + (size_t)col] & TF_SHARPEN) ? 1 : 0;
}

int kvml_video_claim_rect(int row0, int col0, int row1, int col1, int cls)
{
	int r, c;
	if (cls != 1 || !model_current() || gV.ctl.state != KVM_VS_ON || gV.enc == NULL || g_tileInfo == NULL) { return 0; }
	if (row0 < 0 || col0 < 0 || row1 >= gV.rows || col1 >= gV.cols) { return 0; }
	// only changes of the content go to the video: a tile that is sent because of a REFRESH or a repair has to be a JPEG (the viewer's copy of it is not known)
	for (r = row0; r <= row1; r++)
	{
		for (c = col0; c <= col1; c++) { if (gV.flags[((size_t)r * (size_t)gV.cols) + (size_t)c] & (TF_REFRESH | TF_SHARPEN)) { return 0; } }
	}
	kvm_vtiles_claim(&gV.tiles, row0, col0, row1, col1);
	gV.sClaimed += (unsigned long long)(row1 - row0 + 1) * (unsigned long long)(col1 - col0 + 1);
	return 1;
}

void kvml_video_jpeg_sent(int row0, int col0, int row1, int col1, long long bytes)
{
	int r, c, sharp = 0;
	if (!model_current() || row0 < 0 || col0 < 0 || row1 >= gV.rows || col1 >= gV.cols) { return; }
	for (r = row0; r <= row1; r++)
	{
		for (c = col0; c <= col1; c++)
		{
			const size_t i = ((size_t)r * (size_t)gV.cols) + (size_t)c;
			if (gV.flags[i] & TF_SHARPEN) { sharp = 1; }
			gV.flags[i] = 0;
			kvm_vtiles_jpeg(&gV.tiles, r, c);	// sent as JPEG: not pending for the video, not a VIDEO tile (I3)
		}
	}
	if (sharp && bytes > 0)
	{
		const uint32_t now = kvml_clock();
		kvm_sharp_sent(&gV.sharp, now, bytes);
		kvm_vrc_note_sharpen(&gV.rc, now, (uint32_t)bytes, kvm_sharp_active(&gV.sharp, now));
		gV.sSharpBytes += (unsigned long long)bytes;
	}
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// a frame
// ---------------------------------------------------------------------------------------------------------------------------------------

static uint32_t interval_ms(void)
{
	uint32_t t = (uint32_t)(FRAME_RATE_TIMER > 0 ? FRAME_RATE_TIMER : 100);
	if (t < 16) { t = 16; }
	if (t > 1000) { t = 1000; }
	if (gV.fpsCap > 0 && (1000u / (uint32_t)gV.fpsCap) > t) { t = 1000u / (uint32_t)gV.fpsCap; }
	return t;
}

// The rectangles of the frame (tile aligned): the RGB of the picture into BGRA (the converter wants four bytes), then NV12 of the region, then the chroma into its planes.
// `flat`: the picture is first one flat colour (a key frame of the part of the screen that moves: what is outside the rectangles is never shown, so it costs next to nothing).
static void convert_cover(const uint8_t *desktop, const uint8_t *cover, int flat)
{
	const size_t pitch3 = (size_t)gV.codedW * 3u, pitch4 = (size_t)gV.codedW * 4u;
	const size_t cw2 = (size_t)gV.codedW / 2u;
	int r, c, c0;
	if (flat)
	{
		uint8_t fy, fcb, fcr;
		int b = 128, g = 128, rr = 128;
		for (r = 0; r < gV.rows && b == 128; r++)
		{
			for (c = 0; c < gV.cols; c++)
			{
				if (cover[((size_t)r * (size_t)gV.cols) + (size_t)c])
				{
					const uint8_t *px = desktop + ((size_t)r * 32u * pitch3) + ((size_t)c * 32u * 3u);
					rr = px[0]; g = px[1]; b = px[2];
					if (b == 128) { b = 127; }
					break;
				}
			}
		}
		kvm_yuv_rgb_to_yuv(b, g, rr, &fy, &fcb, &fcr);
		(void)kvm_yuv_fill_flat(gV.nv12, gV.codedW, gV.codedH, fy, fcb, fcr);
		memset(gV.u, fcb, (size_t)gV.codedW * (size_t)gV.codedH / 4u);
		memset(gV.v, fcr, (size_t)gV.codedW * (size_t)gV.codedH / 4u);
	}
	for (r = 0; r < gV.rows; r++)
	{
		for (c = 0; c < gV.cols; c = c0)
		{
			int x0, rw, y0, rh, yy, xx, cy;
			c0 = c + 1;
			if (!cover[((size_t)r * (size_t)gV.cols) + (size_t)c]) { continue; }
			while (c0 < gV.cols && cover[((size_t)r * (size_t)gV.cols) + (size_t)c0]) { c0++; }
			x0 = c * 32; rw = (c0 - c) * 32; y0 = r * 32; rh = 32;
			if (x0 + rw > gV.codedW) { rw = gV.codedW - x0; }
			if (y0 + rh > gV.codedH) { rh = gV.codedH - y0; }
			for (yy = y0; yy < y0 + rh; yy++)
			{
				const uint8_t *src = desktop + ((size_t)yy * pitch3) + ((size_t)x0 * 3u);
				uint8_t *dst = gV.bgra + ((size_t)yy * pitch4) + ((size_t)x0 * 4u);
				for (xx = 0; xx < rw; xx++, src += 3, dst += 4) { dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = 255; }
			}
			(void)kvm_yuv_bgra_to_nv12_region(gV.bgra, gV.codedW, gV.codedH, (int)pitch4, gV.nv12, gV.codedW, gV.codedH, x0, y0, rw, rh, KVM_YUV_AUTO);
			for (cy = y0 / 2; cy < (y0 + rh) / 2; cy++)
			{
				const uint8_t *uv = gV.nv12 + ((size_t)gV.codedW * (size_t)gV.codedH) + ((size_t)cy * (size_t)gV.codedW);
				uint8_t *pu = gV.u + ((size_t)cy * cw2), *pv = gV.v + ((size_t)cy * cw2);
				for (xx = x0 / 2; xx < (x0 + rw) / 2; xx++) { pu[xx] = uv[2 * xx]; pv[xx] = uv[(2 * xx) + 1]; }
			}
		}
	}
}

void kvml_video_emit(const void *desktop, long long desktopSize)
{
	kvm_stream *s = kvml_stream();
	kvm_vplan_in in;
	kvm_vplan plan;
	kvm_frame_params fp;
	struct kvm_sps_info sps;
	struct kvm_au_info info;
	uint16_t rects[KVM_FRAME_MAX_RECTS * 4];
	uint32_t now, slotAt, seq, encUs = 0;
	uint64_t t0, t1;
	size_t hdr, len = 0, nlen = 0, total;
	int nRects, isKey, isIdr = 0, qp, rc, fd, idrWhy;
	if (!model_current() || gV.ctl.state != KVM_VS_ON || gV.enc == NULL || gV.tiles.nPending == 0 || desktop == NULL) { return; }
	if (desktopSize < (long long)gV.codedW * (long long)gV.codedH * 3) { return; }
	now = kvml_clock();
	in.now = now; in.pending = gV.tiles.nPending; in.gateOpen = kvm_stream_window_allows(s, KVM_GATE_VIDEO, now); in.rateOpen = kvm_vrc_peek(&gV.rc, now);
	in.intervalMs = interval_ms(); in.rate = kvm_stream_rate_or_mem(s, now);
	kvm_vctl_plan(&gV.ctl, s, &in, &plan);
	if (plan.held)
	{
		if (kvm_stream_note_link_held(s, now)) { kvm_vrc_window_refused(&gV.rc); } else { kvm_vrc_window_refused_soft(&gV.rc); }
		gV.sHeld++;
		return;	// pendingVideo keeps growing: that is "less fps"
	}
	if (plan.heldRate)
	{
		// the byte bucket of the rate control is empty: the frame comes later and carries what has accumulated (never dropped); the refusal is what tells the regulator that the content wants more than R
		(void)kvm_vrc_may_encode(&gV.rc, now, 0);
		gV.sHeldRate++;
		return;
	}
	if (!plan.encode) { return; }
	nRects = kvm_vtiles_make_rects(&gV.tiles, (unsigned)gV.screenW, (unsigned)gV.screenH, rects, gV.cover);
	if (nRects <= 0) { video_error("rectangles of the frame", nRects); return; }
	hdr = kvm_stream_frame_header_len((unsigned)nRects);
	if (gV.frameCap <= hdr) { video_error("frame buffer", 0); return; }
	idrWhy = kvm_vctl_idr_why(&gV.ctl, &plan);
	isKey = (gV.ctl.idrFirst || plan.forceIdr || gV.ctl.idrForce) ? 1 : 0;
	// the slot of this frame follows the slot of the previous one, not the moment it was written
	slotAt = kvm_pace_slot(gV.ctl.haveFrame, gV.ctl.lastFrame, now, in.intervalMs, &gV.late);
	qp = kvm_vcore_clamp_qp(kvm_vrc_qp(&gV.rc, isKey));
	if (qp != gV.appliedQp)
	{
		if (kvml_h264_set_qp(gV.enc, qp) != KVML_H264_OK) { video_error("set qp", 0); return; }
		gV.appliedQp = qp; gV.qp = qp;
	}
	kvm_vtiles_dispatch(&gV.tiles, gV.cover, 0);	// the tiles of the frame are painted by it (the encoder is called in line: it lands before this function returns)
	convert_cover((const uint8_t *)desktop, gV.cover, isKey || !gV.havePicture);
	gV.havePicture = 1;
	t0 = mono_us();
	rc = kvml_h264_encode(gV.enc, gV.y, gV.u, gV.v, plan.forceIdr ? 1 : 0, gV.au, gV.auCap, &len, &isIdr);
	t1 = mono_us();
	encUs = (uint32_t)(t1 - t0);
	if (rc != KVML_H264_OK || len < 5)
	{
		// the encoder lost the access unit (or it did not fit): the next frame is a key frame, the tiles stay pending; too many in a row, or one over 8 MiB, is an error of the encoder
		(void)kvm_vtiles_abort(&gV.tiles, 0);
		gV.havePicture = 0;
		gV.sBroken++;
		vlog("lost access unit code=%d bytes=%zu", rc, len);
		if (kvm_vctl_frame_broken(&gV.ctl, rc == KVML_H264_E_SIZE) == KVM_VB_ERROR) { video_error("lost access units", rc); }
		return;
	}
	if (kvm_annexb_normalize(gV.au, len, NULL, 0, gV.frame + hdr, gV.frameCap - hdr, &nlen, &info) != KVM_ANNEXB_OK)
	{
		(void)kvm_vtiles_abort(&gV.tiles, 0);
		gV.havePicture = 0;
		gV.sBroken++;
		vlog("access unit that does not conform bytes=%zu", len);
		if (kvm_vctl_frame_broken(&gV.ctl, 0) == KVM_VB_ERROR) { video_error("access units that do not conform", 0); }
		return;
	}
	len = nlen;
	isIdr = info.idr;
	fd = kvm_vctl_frame_done(&gV.ctl, isIdr, len, now);
	if (fd == 0) { gV.ctl.lastFrame = slotAt; }	// the slot, not the time of this frame
	if (fd == KVM_VF_BROKEN)
	{
		(void)kvm_vtiles_abort(&gV.tiles, 0);
		gV.havePicture = 0;
		gV.sBroken++;
		vlog("first frame of the generation is not an IDR: not sent");
		return;
	}
	if (fd == KVM_VF_ERROR) { (void)kvm_vtiles_abort(&gV.tiles, 0); video_error("first frame is never an IDR", 0); return; }
	if (isIdr)
	{
		int reason;
		if (kvm_annexb_sps_info(gV.frame + hdr, len, &sps) != 0) { (void)kvm_vtiles_abort(&gV.tiles, 0); video_error("no SPS in a key frame", 0); return; }
		reason = kvm_vcore_check_sps(&sps, gV.ctl.profFlags, gV.ctl.maxLevel, (unsigned)gV.codedW, (unsigned)gV.codedH);
		if (reason != 0)
		{
			vlog("sps refused (IDR) reason=%d profile=%d constraint=0x%02x level=%d refs=%d mbs=%dx%d cropping=%d", reason, sps.profile, sps.constraint, sps.level, sps.numRefFrames, sps.mbsW, sps.mbsH, sps.cropping);
			(void)kvm_vtiles_abort(&gV.tiles, 0);
			fail_request(reason, gV.codedW, gV.codedH);
			return;
		}
		vlog("idr reason=%s frame_n=%u qp=%d kb=%zu rects=%d", kvm_idr_why_name(idrWhy, isIdr), (unsigned)gV.ctl.frames, qp, len / 1024u, nRects);
	}
	total = hdr + len;
	memset(&fp, 0, sizeof(fp));
	fp.flags = (uint8_t)(isIdr ? KVM_FRAME_IDR : 0);
	fp.codedW = (uint16_t)gV.codedW; fp.codedH = (uint16_t)gV.codedH; fp.nRects = (unsigned)nRects; fp.rects = rects; fp.annexB = NULL; fp.annexLen = len;
	if (!s->videoOn || !s->ackActive) { (void)kvm_vtiles_abort(&gV.tiles, 0); stop_video("stream state changed during the frame"); return; }
	now = kvml_clock();
	seq = kvm_stream_add_position(s, now, total);	// the frame is a position (rule M1): the viewer acknowledges it
	fp.seq = seq; fp.ts = now; fp.gen = s->gen;
	if (isIdr) { kvm_stream_watch(s, seq); gV.idrWatchBytes = (uint32_t)total; }
	if (kvm_stream_build_frame_header(gV.frame, hdr, &fp) != hdr) { (void)kvm_vtiles_abort(&gV.tiles, 0); video_error("243 header", 0); return; }
	if (kvml_write(gV.frame, total, KVM_W_POSITION) != 0) { (void)kvm_vtiles_abort(&gV.tiles, 0); video_error("write of the frame", 0); return; }
	{
		// the tiles that the frame painted: the sharpening waits the settle time after it, and they are VIDEO tiles now
		int r, c;
		(void)kvm_vtiles_land(&gV.tiles, 0, gV.map);
		for (r = 0; r < gV.rows; r++)
		{
			for (c = 0; c < gV.cols; c++)
			{
				const size_t i = ((size_t)r * (size_t)gV.cols) + (size_t)c;
				if (gV.map[i]) { gV.lastChange[i] = now; }
			}
		}
	}
	gV.encAvgUs = gV.encAvgUs == 0 ? encUs : (uint32_t)(((uint64_t)gV.encAvgUs * 7u + encUs) / 8u);
	kvm_vctl_note_write(&gV.ctl, now, (gV.encAvgUs + 500u) / 1000u);	// when this frame was written and how long a frame takes (the next one is not encoded right behind it)
	kvm_vrc_frame(&gV.rc, now, (uint32_t)total, qp, isIdr, encUs);
	kvm_sharp_motion(&gV.sharp, now);
	gV.sFrames++; gV.sBytes += total; gV.sRects += (unsigned long long)nRects; gV.sEncUs += encUs; if (encUs > gV.sEncUsMax) { gV.sEncUsMax = encUs; }
	if (isIdr) { gV.sIdr++; }
	gV.totalFrames++;
}

#endif /* KVML_VIDEO_ENABLED */
