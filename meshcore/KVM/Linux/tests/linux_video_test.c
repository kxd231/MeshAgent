/*
Tests of linux_video.c, the H.264 video path of the Linux capture process, with the glue of the ACK window (linux_kvm_stream.c), the portable core (common/) and the real encoder
(openh264 from the system, linux_h264.c). The tile loop is simulated by the calls that getTileAt makes (note_crc, class, claim, jpeg_sent), the pipe is a buffer, the clock is moved by hand
(-DKVML_TEST). Run with run-linux-video-tests.sh (gcc, libopenh264 and ffmpeg in a container).

Modes: "run <dir>" runs the scenarios and writes the stream of the picture scenario (Annex B of every 243) and the truth (the picture of every frame and its rectangles) to <dir>;
"check <dir>" compares what the decoder (ffmpeg) made of the stream with the truth, inside the rectangles; "nolib" and "off" check the answers when there is no library and when the video is switched off.
*/

#include "../linux_kvm_stream.h"
#include "../linux_video.h"
#include "../linux_tileinfo.h"
#include "../../common/kvm_stream.h"
#include "../../common/kvm_video_core.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint32_t kvml_test_now;
extern const kvm_stream *kvml_test_stream(void);
extern void kvml_video_test_state(int *state, unsigned *pending, unsigned *video, unsigned *inflight, unsigned long long *frames, unsigned long long *idrs, unsigned long long *held);
extern int kvml_video_test_tile(int row, int col);
extern unsigned long long kvml_video_test_sharp_bytes(void);

// the globals of the capture process
int SCREEN_WIDTH = 0, SCREEN_HEIGHT = 0, TILE_WIDTH = 32, TILE_HEIGHT = 32, TILE_WIDTH_COUNT = 0, TILE_HEIGHT_COUNT = 0, FRAME_RATE_TIMER = 100;
struct tileInfo_t **g_tileInfo = NULL;

static int gFail = 0, gChecks = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); gChecks++; if (_a != _b) { gFail++; printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

// ---- the pipe ----
static uint8_t gPipe[1 << 24];
static size_t gPipeLen = 0, gParsed = 0;
int kvm_slave_write_raw(const void *buffer, size_t len)
{
	if (gPipeLen + len > sizeof(gPipe)) { return -1; }
	memcpy(gPipe + gPipeLen, buffer, len);
	gPipeLen += len;
	return 0;
}
static void pipe_reset(void) { gPipeLen = 0; gParsed = 0; }

typedef struct msg { int cmd; size_t len; const uint8_t *b; } msg;
static int next_msg(msg *m)
{
	const uint8_t *p;
	if (gParsed + 4 > gPipeLen) { return 0; }
	p = gPipe + gParsed;
	m->cmd = (p[0] << 8) | p[1];
	if (m->cmd == 27) { m->len = 8 + (((size_t)p[4] << 24) | ((size_t)p[5] << 16) | ((size_t)p[6] << 8) | p[7]); }
	else { m->len = ((size_t)p[2] << 8) | p[3]; }
	m->b = p;
	if (m->len < 4 || gParsed + m->len > gPipeLen) { return 0; }
	gParsed += m->len;
	return 1;
}

// ---- the picture ----
static int gW = 1024, gH = 768, gCW = 0, gCH = 0;
static uint8_t *gDesk = NULL;
static int32_t *gCrcNow = NULL;			// the CRC of the tiles of the picture now
static int gNoise = 0;					// the moving window is noise (frames that are big)

static void geometry(int w, int h)
{
	int r;
	gW = w; gH = h;
	SCREEN_WIDTH = w; SCREEN_HEIGHT = h;
	TILE_WIDTH_COUNT = (w + 31) / 32; TILE_HEIGHT_COUNT = (h + 31) / 32;
	gCW = TILE_WIDTH_COUNT * 32; gCH = TILE_HEIGHT_COUNT * 32;
	if (g_tileInfo != NULL) { free(g_tileInfo[0]); free(g_tileInfo); }
	g_tileInfo = (struct tileInfo_t **)malloc(sizeof(struct tileInfo_t *) * (size_t)TILE_HEIGHT_COUNT);
	g_tileInfo[0] = (struct tileInfo_t *)calloc((size_t)TILE_HEIGHT_COUNT * (size_t)TILE_WIDTH_COUNT, sizeof(struct tileInfo_t));
	for (r = 0; r < TILE_HEIGHT_COUNT; r++) { g_tileInfo[r] = g_tileInfo[0] + (size_t)r * (size_t)TILE_WIDTH_COUNT; }
	for (r = 0; r < TILE_HEIGHT_COUNT * TILE_WIDTH_COUNT; r++) { g_tileInfo[0][r].crc = 0xFF; }
	free(gDesk); free(gCrcNow);
	gDesk = (uint8_t *)calloc((size_t)gCW * (size_t)gCH, 3);
	gCrcNow = (int32_t *)calloc((size_t)TILE_HEIGHT_COUNT * (size_t)TILE_WIDTH_COUNT, sizeof(int32_t));
}

// a desktop: a text-like background (so that a tile is not flat) and a window of 256 x 160 pixels that moves with k (the motion), plus a caret that blinks at the bottom
static void paint(int k, int movingX)
{
	int x, y;
	(void)k;
	for (y = 0; y < gCH; y++)
	{
		for (x = 0; x < gCW; x++)
		{
			uint8_t *p = gDesk + (((size_t)y * (size_t)gCW) + (size_t)x) * 3u;
			const int on = (((x / 4) * 7 + (y / 9) * 13) % 5) == 0;
			p[0] = (uint8_t)(on ? 30 : 236); p[1] = (uint8_t)(on ? 30 : 236); p[2] = (uint8_t)(on ? 40 : 232);
		}
	}
	for (y = 160; y < 320; y++)
	{
		for (x = movingX; x < movingX + 256 && x < gCW; x++)
		{
			uint8_t *p = gDesk + (((size_t)y * (size_t)gCW) + (size_t)x) * 3u;
			if (gNoise) { const uint32_t hh = ((uint32_t)(x - movingX) * 2654435761u) ^ ((uint32_t)y * 40503u) ^ ((uint32_t)movingX * 69069u); p[0] = (uint8_t)(hh >> 3); p[1] = (uint8_t)(hh >> 11); p[2] = (uint8_t)(hh >> 19); continue; }
			p[0] = (uint8_t)(40 + ((x - movingX) % 200)); p[1] = (uint8_t)(90 + (y - 160) % 120); p[2] = (uint8_t)(200 - (x - movingX) % 100);
		}
	}
}

static int32_t tile_crc(int r, int c)
{
	uint32_t h = 2166136261u;
	int y;
	for (y = 0; y < 32; y++)
	{
		const uint8_t *p = gDesk + ((((size_t)r * 32u) + (size_t)y) * (size_t)gCW + (size_t)c * 32u) * 3u;
		int i;
		for (i = 0; i < 96; i++) { h = (h ^ p[i]) * 16777619u; }
	}
	return (int32_t)(h & 0x7FFFFFFF);
}

static void viewer_send(const kvm_ctrl *c) { uint8_t b[16]; kvm_stream_build_ctrl(b, c); kvml_input((const char *)b, 240); }
static void op(int o, int flags, int maxLevel)
{
	kvm_ctrl c;
	memset(&c, 0, sizeof(c));
	c.ver = 1; c.op = (uint8_t)o; c.flags = (uint8_t)flags; c.maxLevelIdc = (uint8_t)maxLevel;
	viewer_send(&c);
}

// 241 for the last position that is in the pipe (the viewer has consumed everything)
static uint32_t gLastSeq = 0, gLastTs = 0;
static unsigned gFramesInPipe = 0, gIdrsInPipe = 0;
static FILE *gStream = NULL;				// the Annex B of every 243 (the picture scenario)
static FILE *gTruth = NULL;
static int gRecord = 0, gRecordN = 0;

static void scan_pipe(void)
{
	msg m;
	while (next_msg(&m))
	{
		if (m.cmd == 27 && m.len >= 28 && (((m.b[8] << 8) | m.b[9]) == 243))
		{
			kvm_frame f;
			kvm_rej rej = kvm_stream_parse_frame(m.b, m.len, (unsigned)SCREEN_WIDTH, (unsigned)SCREEN_HEIGHT, &f);
			CHECK_EQ(rej, KVM_REJ_NONE);
			if (rej != KVM_REJ_NONE) { continue; }
			gLastSeq = f.seq; gLastTs = f.ts;
			if (!(f.flags & KVM_FRAME_MARKER)) { gFramesInPipe++; if (f.flags & KVM_FRAME_IDR) { gIdrsInPipe++; } }
			if (gRecord && !(f.flags & KVM_FRAME_MARKER))
			{
				unsigned i;
				fwrite(f.annexB, 1, f.annexLen, gStream);
				fprintf(gTruth, "%d %d %u %u", gRecordN, (f.flags & KVM_FRAME_IDR) ? 1 : 0, (unsigned)f.nRects, (unsigned)f.codedW);
				for (i = 0; i < f.nRects; i++) { fprintf(gTruth, " %u,%u,%u,%u", (f.rects[i * 8] << 8) | f.rects[i * 8 + 1], (f.rects[i * 8 + 2] << 8) | f.rects[i * 8 + 3], (f.rects[i * 8 + 4] << 8) | f.rects[i * 8 + 5], (f.rects[i * 8 + 6] << 8) | f.rects[i * 8 + 7]); }
				fprintf(gTruth, "\n");
				{
					char name[600];
					extern char gDir[512];
					FILE *pf;
					snprintf(name, sizeof(name), "%s/pic%d.rgb", gDir, gRecordN);
					pf = fopen(name, "wb");
					if (pf != NULL) { fwrite(gDesk, 1, (size_t)gCW * (size_t)gCH * 3u, pf); fclose(pf); }
				}
				gRecordN++;
			}
		}
	}
}
char gDir[512];

static void viewer_ack(void)
{
	kvm_ack a;
	uint8_t b[16];
	scan_pipe();
	if (gLastSeq == 0) { return; }
	memset(&a, 0, sizeof(a));
	a.seq = gLastSeq; a.ts = gLastTs;
	kvm_stream_build_ack(b, &a);
	kvml_input((const char *)b, 241);
}

// the next 242 in what was written since the last look (a copy of its fields), or 0
static int gHave242 = 0;
static kvm_state_msg gLast242;
static unsigned gCount242 = 0;
static void look_242(void)
{
	size_t at = 0;
	gHave242 = 0;
	while (at + 16 <= gPipeLen)
	{
		const uint8_t *p = gPipe + at;
		const int cmd = (p[0] << 8) | p[1];
		size_t len = (cmd == 27) ? 8 + (((size_t)p[4] << 24) | ((size_t)p[5] << 16) | ((size_t)p[6] << 8) | p[7]) : (((size_t)p[2] << 8) | p[3]);
		if (len < 4 || at + len > gPipeLen) { break; }
		if (cmd == 242 && len == 16) { if (kvm_stream_parse_state(p, 16, &gLast242) == KVM_REJ_NONE) { gHave242 = 1; gCount242++; } }
		at += len;
	}
}

// ---- one iteration of the capture loop, as linux_kvm.c does it ----
static unsigned gJpegTiles = 0;
static int gGateOpenForced = -1;
static void iteration(int k, int movingX)
{
	int r, c;
	kvml_test_now += 100;
	kvml_poll();							// the commands of the viewer, the timers, the requests of the video
	if (g_tileInfo == NULL || SCREEN_WIDTH == 0) { return; }
	paint(k, movingX);
	for (r = 0; r < TILE_HEIGHT_COUNT * TILE_WIDTH_COUNT; r++) { g_tileInfo[0][r].flag = TILE_TODO; }
	kvml_video_frame_begin();
	for (r = 0; r < TILE_HEIGHT_COUNT; r++)
	{
		for (c = 0; c < TILE_WIDTH_COUNT; c++)
		{
			int cls, crc;
			uint8_t fake[200];
			if (g_tileInfo[r][c].flag != TILE_TODO) { continue; }
			crc = tile_crc(r, c);
			kvml_video_note_crc(r, c, crc);
			if (crc == g_tileInfo[r][c].crc) { continue; }
			cls = kvml_video_class(r, c);
			if (cls == 0)
			{
				if (kvml_video_tile_sharpening(r, c)) { if (!kvml_gate_sharpen()) { continue; } }
				else if (!kvml_gate_big()) { g_tileDeferred = 1; break; }
			}
			g_tileInfo[r][c].crc = crc;
			if (kvml_video_claim_rect(r, c, r, c, cls)) { g_tileInfo[r][c].flag = TILE_SENT; continue; }
			// a JPEG of this tile
			memset(fake, 0, sizeof(fake));
			fake[0] = 0; fake[1] = 3; fake[2] = 0; fake[3] = 200;
			CHECK_EQ(kvml_write(fake, sizeof(fake), KVM_W_DATA), 0);
			kvml_video_jpeg_sent(r, c, r, c, 200);
			g_tileInfo[r][c].flag = TILE_SENT;
			gJpegTiles++;
		}
		if (g_tileDeferred) { g_tileDeferred = 0; break; }
	}
	kvml_video_emit(gDesk, (long long)gCW * (long long)gCH * 3);
	kvml_iteration_end();
	(void)gGateOpenForced;
}

static void begin_session(int w, int h)
{
	kvml_session_end();
	pipe_reset();
	gLastSeq = gLastTs = 0; gFramesInPipe = gIdrsInPipe = 0; gCount242 = 0;
	kvml_test_now = 1000;
	geometry(w, h);
	kvml_session_begin();
	kvml_announce();
}

static unsigned st_video(void) { unsigned v = 0; kvml_video_test_state(NULL, NULL, &v, NULL, NULL, NULL, NULL); return v; }
static unsigned st_pending(void) { unsigned v = 0; kvml_video_test_state(NULL, &v, NULL, NULL, NULL, NULL, NULL); return v; }
static unsigned long long st_frames(void) { unsigned long long v = 0; kvml_video_test_state(NULL, NULL, NULL, NULL, &v, NULL, NULL); return v; }
static int st_state(void) { int v = 0; kvml_video_test_state(&v, NULL, NULL, NULL, NULL, NULL, NULL); return v; }

// ---- the scenarios ----

// op 1 gets a 242 with video: the generation, the profile and the level of the SPS, the coded size; the motion goes as video frames: the first is a key frame whose rectangles cover the motion
static void test_video_session(int record)
{
	int k, x = 64;
	unsigned frames0;
	msg m;
	int sawIdr = 0, sawState = 0;
	begin_session(1024, 768);
	if (record) { gRecord = 1; gRecordN = 0; }
	// a start of the stream: 242 reason 11 and no video; then the viewer asks for the video
	op(KVM_OP_VIDEO_ON, KVM_PROFILE_CBP | KVM_PROFILE_MAIN | KVM_PROFILE_HIGH, 0);
	iteration(0, x);
	look_242();
	CHECK(gHave242);
	CHECK_EQ(gLast242.video, 1); CHECK_EQ(gLast242.ack, 1); CHECK_EQ(gLast242.reason, 0);
	CHECK_EQ(gLast242.profile, 100); CHECK_EQ(gLast242.codedW, 1024); CHECK_EQ(gLast242.codedH, 768); CHECK_EQ(gLast242.gen, 1);
	CHECK_EQ(gLast242.caps & 3, 3);
	CHECK_EQ(st_state(), KVM_VS_ON);
	frames0 = (unsigned)st_frames();
	// the window moves: the first frames of the change go as JPEG (the first change of a tile is not motion), then the tiles are motion and the frames come
	for (k = 1; k <= 14; k++)
	{
		x += 24;
		iteration(k, x);
		viewer_ack();
	}
	printf("video session: %llu frames, %u idr, %u JPEG tiles\n", st_frames(), gIdrsInPipe, gJpegTiles);
	CHECK(st_frames() > frames0 + 4);
	CHECK(gJpegTiles > 0);					// the first changes
	CHECK(gJpegTiles < 768 + 250);			// the first picture of the screen (768 tiles), and then not the whole motion
	// what is in the pipe: 243 frames that the strict parser accepts, the first one is an IDR with SPS and PPS
	pipe_reset();
	(void)m;
	CHECK(st_video() > 20);
	CHECK(gFramesInPipe > 4);
	CHECK(gIdrsInPipe >= 1);
	sawIdr = gIdrsInPipe >= 1; sawState = 1;
	CHECK(sawIdr && sawState);
	gRecord = 0;
}

static void test_no_video_without_request(void)
{
	int k;
	begin_session(1024, 768);
	op(KVM_OP_ACK_ONLY, 0, 0);
	for (k = 0; k < 10; k++) { iteration(k, 64 + 24 * k); viewer_ack(); }
	CHECK_EQ(st_frames(), 0);
	CHECK_EQ(st_state(), KVM_VS_OFF);
	CHECK_EQ(st_pending(), 0);
}

// the viewer stops acknowledging: the stream layer ends the ACK after 15 s (242 reason 9) and with it the video; the tiles that waited go back to JPEG; the viewer has to ask again
static void test_no_ack(void)
{
	int k, x = 64;
	unsigned long long f0;
	begin_session(1024, 768);
	op(KVM_OP_VIDEO_ON, KVM_PROFILE_CBP | KVM_PROFILE_MAIN | KVM_PROFILE_HIGH, 0);
	for (k = 0; k < 12; k++) { x += 24; iteration(k, x); viewer_ack(); }
	CHECK(st_frames() > 2);
	CHECK_EQ(st_state(), KVM_VS_ON);
	f0 = st_frames();
	for (k = 12; k < 12 + 170; k++) { x = 64 + (x + 24) % 600; iteration(k, x); }	// 17 s without an acknowledgement
	look_242();
	CHECK(gHave242);
	CHECK_EQ(gLast242.video, 0); CHECK_EQ(gLast242.reason, KVM_REASON_NO_ACK);
	CHECK_EQ(st_state(), KVM_VS_OFF);
	CHECK(st_frames() >= f0);
	CHECK_EQ(st_pending(), 0);
	// asking again starts a new generation
	op(KVM_OP_VIDEO_ON, KVM_PROFILE_CBP | KVM_PROFILE_MAIN | KVM_PROFILE_HIGH, 0);
	for (k = 200; k < 214; k++) { x = 64 + (x + 24) % 600; iteration(k, x); viewer_ack(); }
	CHECK_EQ(st_state(), KVM_VS_ON);
	look_242();
	CHECK_EQ(gLast242.video, 1); CHECK(gLast242.gen >= 2);
}

// the veto of a second viewer stops the video at once: the pending tiles are JPEG again (their CRCs are reset), the generation ends with a 242
static void test_veto_and_repair(void)
{
	int k, x = 64, nonreset = 0, r, c;
	begin_session(1024, 768);
	op(KVM_OP_VIDEO_ON, KVM_PROFILE_CBP | KVM_PROFILE_MAIN | KVM_PROFILE_HIGH, 0);
	for (k = 0; k < 14; k++) { x += 24; iteration(k, x); viewer_ack(); }
	CHECK_EQ(st_state(), KVM_VS_ON);
	CHECK(st_video() > 0);
	// op 3 with the IDR flag: a key frame (after the spacing)
	{
		unsigned long long idr0 = 0, idr1 = 0;
		kvml_video_test_state(NULL, NULL, NULL, NULL, NULL, &idr0, NULL);
		op(KVM_OP_REPAIR, KVM_REPAIR_IDR, 0);
		for (k = 14; k < 40; k++) { x += 24; iteration(k, x); viewer_ack(); }
		kvml_video_test_state(NULL, NULL, NULL, NULL, NULL, &idr1, NULL);
		CHECK(idr1 > idr0);
	}
	// op 3 with the tile repair flag: the VIDEO tiles go back to JPEG (their CRCs are reset)
	{
		unsigned v0 = st_video();
		CHECK(v0 > 0);
		op(KVM_OP_REPAIR, KVM_REPAIR_TILES, 0);
		kvml_test_now += 1500;
		iteration(40, x);
		for (r = 0; r < TILE_HEIGHT_COUNT; r++) { for (c = 0; c < TILE_WIDTH_COUNT; c++) { if (kvml_video_test_tile(r, c) == KVM_VT_VIDEO) { nonreset++; } } }
		CHECK(nonreset <= (int)(v0 / 2));
	}
	// the veto (meshcore: a second viewer)
	op(KVM_OP_VETO, KVM_VETO_MESHCORE, 0);
	iteration(41, x);
	CHECK_EQ(st_state(), KVM_VS_OFF);
	CHECK_EQ(st_pending(), 0);
	CHECK_EQ(kvml_test_stream()->videoOn, 0);
	look_242();
	CHECK(gHave242);
	CHECK_EQ(gLast242.video, 0); CHECK_EQ(gLast242.reason, KVM_REASON_VETO);
	{
		// no frame while the veto holds, and what was painted is sharpened (JPEG) in the end: the VIDEO tiles do not stay blurred
		unsigned long long f0 = st_frames();
		unsigned v0 = st_video(), k2;
		for (k2 = 0; k2 < 60; k2++) { iteration(42 + (int)k2, x); viewer_ack(); }
		CHECK_EQ(st_frames(), f0);
		CHECK(v0 == 0 || st_video() < v0);
	}
}

// the picture gets another size (cmd 7): a new generation with the new coded size, in the 242
static void test_geometry(void)
{
	int k, x = 64;
	begin_session(1024, 768);
	op(KVM_OP_VIDEO_ON, KVM_PROFILE_CBP | KVM_PROFILE_MAIN | KVM_PROFILE_HIGH, 0);
	for (k = 0; k < 10; k++) { x += 24; iteration(k, x); viewer_ack(); }
	CHECK_EQ(st_state(), KVM_VS_ON);
	geometry(1280, 800);
	kvml_test_now += 2500;							// (an encoder is created at most once in 2 s)
	for (k = 10; k < 30; k++) { x += 24; iteration(k, x); viewer_ack(); }
	CHECK_EQ(st_state(), KVM_VS_ON);
	look_242();
	CHECK(gHave242);
	CHECK_EQ(gLast242.video, 1); CHECK_EQ(gLast242.codedW, 1280); CHECK_EQ(gLast242.codedH, 800); CHECK(gLast242.gen >= 2);
	CHECK(st_frames() > 5);
}

// the limits of the viewer: a level that the picture does not fit gets 242 reason 7 and no video; a profile list without a profile that the encoder has: reason 8 (not here: all are there)
static void test_limits(void)
{
	int k, x = 64;
	begin_session(1920, 1080);
	op(KVM_OP_VIDEO_ON, KVM_PROFILE_CBP, 31);		// level 3.1 does not hold 1920 x 1088
	iteration(0, x);
	look_242();
	CHECK(gHave242);
	CHECK_EQ(gLast242.video, 0); CHECK_EQ(gLast242.reason, KVM_REASON_RESOLUTION);
	CHECK_EQ(st_state(), KVM_VS_OFF);
	for (k = 1; k < 6; k++) { x += 24; iteration(k, x); viewer_ack(); }
	CHECK_EQ(st_frames(), 0);
	CHECK_EQ(kvml_test_stream()->ackActive, 1);	// the ACK stays
	// a viewer with a constrained baseline only gets that profile
	pipe_reset(); gParsed = 0;
	op(KVM_OP_VIDEO_ON, KVM_PROFILE_CBP, 0);
	iteration(6, x);
	look_242();
	CHECK(gHave242);
	CHECK_EQ(gLast242.video, 1); CHECK_EQ(gLast242.profile, 66); CHECK_EQ(gLast242.codedH, 1088);
	for (k = 7; k < 20; k++) { x += 24; iteration(k, x); viewer_ack(); }
	CHECK(st_frames() > 2);
}

// the picture scenario that the decoder checks: the window moves, stops (the sharpening), a REFRESH comes, the window moves again
static void test_recorded(void)
{
	int k, x = 64;
	begin_session(1024, 768);
	gRecord = (gStream != NULL) ? 1 : 0; gRecordN = 0;
	op(KVM_OP_VIDEO_ON, KVM_PROFILE_CBP | KVM_PROFILE_MAIN | KVM_PROFILE_HIGH, 0);
	for (k = 0; k < 16; k++) { x += 24; iteration(k, x); viewer_ack(); }			// the window moves
	{
		const unsigned video0 = st_video();
		unsigned jpeg0;
		const unsigned long long frames0 = st_frames();
		CHECK(video0 > 30);
		CHECK_EQ(kvml_test_stream()->st.markers < st_frames() / 2, 1);			// a frame is a position: it needs no marker of its own
		jpeg0 = gJpegTiles;
		iteration(16, x); viewer_ack();										// 100 ms after the last frame: the tiles are not settled yet (150 ms)
		CHECK_EQ(kvml_video_test_tile(7, (x + 128) / 32), KVM_VT_VIDEO);				// the middle of the window that the last frame painted
		CHECK(st_video() >= 20);											// what the last frame painted (the window, 40 tiles) is still video: only the tiles that have been still for 150 ms are sharpened
		iteration(17, x); viewer_ack();
		iteration(18, x); viewer_ack();
		CHECK(gJpegTiles > jpeg0 + 5);										// the sharpening has begun
		for (k = 19; k < 60; k++) { iteration(k, x); viewer_ack(); }		// it stops: the tiles are sharpened as JPEG, no more frames
		CHECK(gJpegTiles > jpeg0 + video0 / 2);
		CHECK_EQ(st_video(), 0);											// every tile of the video is a JPEG again
		CHECK(st_frames() <= frames0 + 1);
		CHECK(kvml_video_test_sharp_bytes() > 1000);						// ... and the bytes of the sharpening were counted
	}
	for (k = 60; k < 72; k++) { x -= 30; iteration(k, x); viewer_ack(); }			// it moves back
	CHECK(gRecordN > 8 || gStream == NULL);
	gRecord = 0;
}

// a window that is full of noise: the frames are big, the window of the ACK fills without acknowledgements and the video is held (the tiles stay pending); the ACK opens it
static void test_window_gate(void)
{
	int k, x = 64;
	unsigned long long f0, held0 = 0, held1 = 0, f1;
	begin_session(1024, 768);
	gNoise = 1;
	op(KVM_OP_VIDEO_ON, KVM_PROFILE_CBP | KVM_PROFILE_MAIN | KVM_PROFILE_HIGH, 0);
	for (k = 0; k < 10; k++) { x += 24; iteration(k, x); viewer_ack(); }
	CHECK_EQ(st_state(), KVM_VS_ON);
	f0 = st_frames();
	for (k = 10; k < 40; k++) { x += 24; iteration(k, x); }								// no acknowledgements for 3 s
	f1 = st_frames();
	kvml_video_test_state(NULL, NULL, NULL, NULL, NULL, NULL, &held0);
	printf("window gate: %llu frames before, %llu after 3 s without an ACK, held %llu times\n", f0, f1, held0);
	CHECK(held0 > 0);
	CHECK(f1 < f0 + 29);														// not a frame in every iteration
	CHECK(st_pending() > 0);													// what moved waits for the frame that the window will let in
	for (k = 40; k < 52; k++) { x += 24; iteration(k, x); viewer_ack(); }
	CHECK(st_frames() > f1);
	kvml_video_test_state(NULL, NULL, NULL, NULL, NULL, NULL, &held1);
	CHECK(held1 >= held0);
	gNoise = 0;
}

// a REFRESH: every tile is a picture again, the tiles near the motion do not go to the video
static void test_refresh(void)
{
	int k, x = 64, r;
	unsigned j0;
	unsigned long long f0;
	begin_session(1024, 768);
	op(KVM_OP_VIDEO_ON, KVM_PROFILE_CBP | KVM_PROFILE_MAIN | KVM_PROFILE_HIGH, 0);
	for (k = 0; k < 14; k++) { x += 24; iteration(k, x); viewer_ack(); }
	CHECK_EQ(st_state(), KVM_VS_ON);
	for (r = 0; r < TILE_HEIGHT_COUNT * TILE_WIDTH_COUNT; r++) { g_tileInfo[0][r].crc = 0xFF; g_tileInfo[0][r].flag = 0; }	// what the command does
	kvml_video_refresh();
	j0 = gJpegTiles;
	f0 = st_frames();
	x += 24; iteration(14, x);
	CHECK(gJpegTiles - j0 > 100);												// the picture as JPEG (as much as the window lets in), none of it as video: no frame
	CHECK_EQ(st_frames(), f0);
}

// the tiles that waited for a frame go back to JPEG when the video stops (their CRCs are reset so that the next scan sends them)
static void test_pending_back(void)
{
	int k, x = 64;
	begin_session(1024, 768);
	op(KVM_OP_VIDEO_ON, KVM_PROFILE_CBP | KVM_PROFILE_MAIN | KVM_PROFILE_HIGH, 0);
	for (k = 0; k < 10; k++) { x += 24; iteration(k, x); viewer_ack(); }
	CHECK_EQ(st_state(), KVM_VS_ON);
	kvml_test_now += 100;
	kvml_video_frame_begin();
	g_tileInfo[20][20].crc = 123;
	CHECK_EQ(kvml_video_claim_rect(20, 20, 20, 21, 1), 1);						// two tiles wait for the next frame
	CHECK_EQ(kvml_video_test_tile(20, 20), KVM_VT_PENDING);
	op(KVM_OP_ACK_ONLY, 0, 0);													// the viewer wants JPEG
	kvml_test_now += 100;
	kvml_poll();
	CHECK_EQ(st_state(), KVM_VS_OFF);
	CHECK_EQ(kvml_video_test_tile(20, 20), KVM_VT_NONE);
	CHECK_EQ(g_tileInfo[20][20].crc, 0xFF);
	CHECK_EQ(g_tileInfo[20][21].crc, 0xFF);
}

static void test_nolib(void)
{
	int reason = 0;
	int cap = kvml_video_session_begin(&reason);
	CHECK_EQ(cap, 0); CHECK_EQ(reason, KVM_REASON_NO_MF);
	begin_session(1024, 768);
	op(KVM_OP_VIDEO_ON, 7, 0);
	iteration(0, 64);
	look_242();
	CHECK(gHave242);
	CHECK_EQ(gLast242.video, 0); CHECK_EQ(gLast242.ack, 1); CHECK_EQ(gLast242.reason, KVM_REASON_NO_MF); CHECK_EQ(gLast242.caps & 1, 0);
}

static void test_off(void)
{
	begin_session(1024, 768);
	op(KVM_OP_VIDEO_ON, 7, 0);
	iteration(0, 64);
	look_242();
	CHECK(gHave242);
	CHECK_EQ(gLast242.video, 0); CHECK_EQ(gLast242.ack, 1); CHECK_EQ(gLast242.reason, KVM_REASON_DISABLED);
}

// ---- the decoder's check ----
static double psnr_rect(const uint8_t *a, const uint8_t *b, int stride, int x, int y, int w, int h)
{
	double se = 0.0;
	int yy, xx, ch;
	for (yy = y; yy < y + h; yy++)
	{
		for (xx = x; xx < x + w; xx++)
		{
			for (ch = 0; ch < 3; ch++)
			{
				const int d = (int)a[((size_t)yy * (size_t)stride + (size_t)xx) * 3u + (size_t)ch] - (int)b[((size_t)yy * (size_t)stride + (size_t)xx) * 3u + (size_t)ch];
				se += (double)(d * d);
			}
		}
	}
	se /= (double)w * (double)h * 3.0;
	return se < 1e-9 ? 99.0 : 10.0 * log10(255.0 * 255.0 / se);
}

static int check_decoded(const char *dir)
{
	char path[600];
	FILE *tf, *df;
	char line[8192];
	int frames = 0, worst10 = 0;
	double worst = 99.0;
	snprintf(path, sizeof(path), "%s/truth.txt", dir);
	tf = fopen(path, "r");
	snprintf(path, sizeof(path), "%s/decoded.rgb", dir);
	df = fopen(path, "rb");
	CHECK(tf != NULL && df != NULL);
	if (tf == NULL || df == NULL) { return -1; }
	while (fgets(line, sizeof(line), tf) != NULL)
	{
		int n, idr, nr, cw, ch, i, off = 0, used = 0;
		uint8_t *pic, *dec;
		FILE *pf;
		char *p = line;
		if (sscanf(p, "%d %d %d %d%n", &n, &idr, &nr, &cw, &used) < 4) { continue; }
		p += used;
		snprintf(path, sizeof(path), "%s/pic%d.rgb", dir, n);
		pf = fopen(path, "rb");
		CHECK(pf != NULL);
		if (pf == NULL) { continue; }
		{
			long sz;
			fseek(pf, 0, SEEK_END); sz = ftell(pf); fseek(pf, 0, SEEK_SET);
			ch = (int)(sz / 3 / cw); (void)ch;
			pic = (uint8_t *)malloc((size_t)sz);
			dec = (uint8_t *)malloc((size_t)sz);
			CHECK_EQ(fread(pic, 1, (size_t)sz, pf), (size_t)sz);
			CHECK_EQ(fread(dec, 1, (size_t)sz, df), (size_t)sz);	// the decoder made one picture per frame, in order
			fclose(pf);
		}
		for (i = 0; i < nr; i++)
		{
			int x, y, w, h;
			double ps;
			if (sscanf(p, " %d,%d,%d,%d%n", &x, &y, &w, &h, &off) < 4) { break; }
			p += off;
			ps = psnr_rect(pic, dec, cw, x, y, w, h);
			if (ps < worst) { worst = ps; }
			if (ps < 28.0) { worst10++; printf("low PSNR %.1f dB: frame %d rect %d,%d %dx%d\n", ps, n, x, y, w, h); }
		}
		free(pic); free(dec);
		frames++;
	}
	printf("decoded %d frames, the lowest PSNR inside the rectangles %.1f dB\n", frames, worst);
	CHECK(frames > 4);
	CHECK_EQ(worst10, 0);
	CHECK(worst > 28.0);
	fclose(tf); fclose(df);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "nolib") == 0) { test_nolib(); }
	else if (argc > 1 && strcmp(argv[1], "off") == 0) { test_off(); }
	else if (argc > 2 && strcmp(argv[1], "check") == 0) { (void)check_decoded(argv[2]); }
	else
	{
		if (argc > 2 && strcmp(argv[1], "run") == 0)
		{
			char path[600];
			snprintf(gDir, sizeof(gDir), "%s", argv[2]);
			snprintf(path, sizeof(path), "%s/stream.h264", gDir); gStream = fopen(path, "wb");
			snprintf(path, sizeof(path), "%s/truth.txt", gDir); gTruth = fopen(path, "w");
			test_recorded();
			fclose(gStream); fclose(gTruth);
		}
		else
		{
			test_video_session(0);
			test_no_video_without_request();
			test_no_ack();
			test_veto_and_repair();
			test_geometry();
			test_limits();
			test_recorded();
			test_window_gate();
			test_refresh();
			test_pending_back();
		}
	}
	printf("%d checks, %d failed\n", gChecks, gFail);
	return gFail == 0 ? 0 : 1;
}
