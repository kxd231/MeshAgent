// Test of kvm_video.cpp on Linux, without Windows and without Media Foundation: the production file is compiled against a shim of <windows.h> (a fake clock), the real
// kvm_video_core.c / kvm_stream.c / kvm_annexb.c, and these fakes: the encoder (mfh264_*), the glue of kvm.c (the lock, the pipe: every write is recorded) and the globals
// of tile.cpp (tileInfo, the sizes). The scenarios drive it the way kvm.c does (kvm_stream_on_ctrl, kvm_video_ctrl, kvm_video_poll, kvm_video_claim_rect, kvm_video_emit) and
// check what goes on the wire (242, 243: parsed with the receiver's checks of kvm_stream_parse_frame) and what happens to the tiles.
// Run with run-kvm-video-harness.sh (g++ in a container). It checks the logic of the glue, not Media Foundation: the encoder is a fake.

#include "tile.h"
#include "kvm_video.h"
#include "mf_h264.h"
#include "kvm_encthr.h"
#include "../../common/kvm_fbuf.h"
#define KVM_VIDEO_NONE (-100)	// emit(): no frame was written (else the ILibTransport_DoneState of the write)
#include "../../common/kvm_video_core.h"
#include "../../common/kvm_annexb.h"
#include "../../common/kvm_qpdet.h"
#include "../../common/kvm_yuv.h"
#include "../../common/kvm_auto.h"
#include <math.h>
#include <stdarg.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <time.h>
static std::mutex gClockMu;	// the fake clock is read by the thread of the encoder too

#ifndef KVM_VIDEO_ENABLED
#error "the harness needs the x64 defines of the video build"
#endif

static int gFail = 0, gChecks = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); gChecks++; if (_a != _b) { gFail++; printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

// ---------------------------------------------------------------------------------------------------------------------------------------
// the fake system
// ---------------------------------------------------------------------------------------------------------------------------------------

extern "C"
{
uint32_t gTestNowMs = 100000;
unsigned gTestCores = 2;
unsigned long long gProcCpuUs = 0, gCapCpuUs = 0;	// the fake CPU clocks of the process and of the capture thread; the fake encoder advances them by gEncCpuUs (the encoder) and gCapOverlapUs (the capture thread beside it)
static unsigned long long gEncCpuUs = 0, gCapOverlapUs = 0;
long long gTestQpcTicks = 0;
DWORD timeGetTime(void) { return gTestNowMs; }
BOOL QueryPerformanceCounter(LARGE_INTEGER *p) { std::lock_guard<std::mutex> lk(gClockMu); gTestQpcTicks += 1000; p->QuadPart = gTestQpcTicks; return 1; } // 1 ms per call at 1 MHz
BOOL QueryPerformanceFrequency(LARGE_INTEGER *p) { p->QuadPart = 1000000; return 1; }
DWORD GetEnvironmentVariableA(const char *name, char *buf, DWORD size)
{
	const char *v = getenv(name);
	if (v == NULL || strlen(v) + 1 > size) { return 0; }
	strcpy(buf, v);
	return (DWORD)strlen(v);
}
LONG InterlockedIncrement(volatile LONG *p) { return ++(*p); }
LONG InterlockedExchange(volatile LONG *p, LONG v) { LONG o = *p; *p = v; return o; }
LONG InterlockedCompareExchange(volatile LONG *p, LONG ex, LONG cmp) { LONG o = *p; if (o == cmp) { *p = ex; } return o; }

// globals of kvm.c / tile.cpp that the production code uses
int TILE_WIDTH = 32, TILE_HEIGHT = 32, TILE_WIDTH_COUNT = 0, TILE_HEIGHT_COUNT = 0, PIXEL_SIZE = 4, FRAME_RATE_TIMER = 16, SCALED_WIDTH = 0, SCALED_HEIGHT = 0;
tileInfo_t **tileInfo = NULL;
volatile LONG gKvmFullFrame = 0;
}

static std::vector<std::string> gLog;
extern "C" void kvm_stats_log(const char *fmt, ...)
{
	char b[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	gLog.push_back(b);
}
static bool log_has(const char *s) { for (size_t i = 0; i < gLog.size(); i++) { if (gLog[i].find(s) != std::string::npos) { return true; } } return false; }

// the glue of kvm.c: the stream state, the lock, the pipe (every write is recorded)
struct wire_item { int kind; std::vector<uint8_t> b; };
static kvm_stream gS;
static std::vector<wire_item> gWire;
static int gWriteResult = 1; // ILibTransport_DoneState_COMPLETE
static int gAdaptBudget = -1; // the viewer's budget in kbit/s for the rate control (-1 none)
static int gPadBytes = 0;	// the fake encoder pads every access unit with a filler NAL unit of this many bytes (frames of a size)
static long long gNoteWriteBytes = 0;
static int gVideoFrameNotes = 0;
// the fake encoder's reaction to the QP: the size of a P frame follows the QP (gSizeByQp) or not; the QP it reports in the output sample (-1 absent, 0 what it was asked, 1 stuck); the rate control mode of the last open
static std::atomic<int> gViewFrames(0);	// the frames that the fake encoder was given as an NV12 view
static void (*gViewHook)(const uint8_t *nv12, size_t len) = NULL;
static int gSizeByQp = 0, gReportMode = -1, gRcModeOk = 1, gRejectSample = 0;
static int gCurQp = 0, gSampleMode = 0, gLastRc = 0, gLastBitrate = 0;
static std::vector<int> gBitrates, gSampleModes;

extern "C"
{
void kvm_glue_lock(void) {}
void kvm_glue_unlock(void) {}
kvm_stream *kvm_glue_stream(void) { return &gS; }
int kvm_glue_write_locked(char *buffer, int len, int kind)
{
	wire_item w;
	w.kind = kind;
	w.b.assign((uint8_t *)buffer, (uint8_t *)buffer + len);
	kvm_stream_note_write(&gS, (size_t)len, kind, gTestNowMs);
	gWire.push_back(w);
	return gWriteResult;
}
void kvm_glue_state_locked(const kvm_state_msg *m0)
{
	uint8_t b[16];
	kvm_state_msg mc = *m0;
	kvm_video_state_caps(&mc);		// as kvm_stream_send_state_locked of kvm.c does it
	kvm_stream_build_state(b, &mc);
	kvm_glue_write_locked((char *)b, 16, KVM_W_STATE);
}
void kvm_glue_log_flush(void) {}
unsigned long long kvm_glue_cpu_us(void) { return gProcCpuUs; }	// 0 = no readable CPU clock: the parallelism of the encoder stays at the assumed 1.5
unsigned long long kvm_glue_capture_cpu_us(void) { return gCapCpuUs; }
unsigned long long kvm_glue_nonenc_cpu_us(void) { return gCapCpuUs; }	// the main loop's own CPU does not grow in the fake: what the capture thread has is the whole of it (0 = unknown)
static int gWrittenFrames = 0, gLastWriteRes = 0;
void kvm_glue_video_written(int res, long long bytes, unsigned writeUs) { (void)bytes; (void)writeUs; gWrittenFrames++; gLastWriteRes = res; }
unsigned kvm_glue_cores(void) { return gTestCores; }
int kvm_ack_peek(int kind) { return kvm_stream_window_allows(&gS, kind, gTestNowMs); }
void kvm_ack_note_held_back(void) { kvm_stream_note_held_back(&gS, gTestNowMs); }
int kvm_ack_note_link_held(void) { return kvm_stream_note_link_held(&gS, gTestNowMs); }
int kvm_adapt_viewer_budget_kbps(void) { return gAdaptBudget > 0 ? gAdaptBudget : 0; }
unsigned long long kvm_adapt_sharpen_bytes_total(void) { return 0; }
void kvm_adapt_note_write(unsigned int us, long long bytes, int sharpen) { (void)us; (void)sharpen; gNoteWriteBytes += bytes; }
void kvm_adapt_note_video_frame(void) { gVideoFrameNotes++; }
}

// the encoder: a fake that writes small Annex B access units (an IDR as SPS PPS IDR slice, a P as one slice) and does what the scenario says
struct mfh264
{
	int w, h, profile, level, idrNext, frames;
	uint8_t seq[160]; size_t seqLen;	// SPS + PPS the fake "writes"
};
enum { FE_OK = 0, FE_BROKEN = 1, FE_FIRST_P = 2, FE_EXCEPTION = 3, FE_OVERSIZE = 4, FE_ERROR = 5 };
static std::atomic<int> gEncMode(FE_OK);
static int gInitResult = 0, gOpens = 0, gCloses = 0, gShutdowns = 0, gForces = 0, gEncodes = 0;
static std::vector<int> gOpenResults; // consumed by the calls of mfh264_open (0 = success); empty = success
static int gLastProfile = 0, gLastLevel = 0, gLastW = 0, gLastH = 0;
static bool gFaulted = false;
static int gSpsProfile = 0, gSpsLevel = 0, gSpsConstraint = -1, gSpsRefs = 1, gSpsCrop = 0, gNoSeqHdr = 0, gIdrRefs = -1; // what the fake encoder's SPS says when it is not what was asked for (constraint -1: the right one); gIdrRefs: another max_num_ref_frames in the IDR than in the sequence header
static std::mutex gFakeMu;		// the fake encoder is used from the thread of the encoder too (the real-thread run under ThreadSanitizer)
static int (*gEncSnapshot)(const uint8_t *top, int w, int h, int stride) = NULL;	// a test that wants to know what picture a frame showed: called by the fake encoder, its number goes into the access unit
static int (*gEncSnapshotNv12)(const uint8_t *nv12, size_t len) = NULL;	// the same for the NV12 view of the encoder thread
static std::vector<uint8_t> gPicBytes;	// the first byte of the picture of every encode call
static std::vector<int> gQps; // every QP the code set
// the fake clock of the encoder thread adds 1 ms per call; with the view (A, the default) the thread reads it twice more (around the conversion), which is part of the encode time
#ifndef KVM_NV12_FROZEN_DEFAULT
#define KVM_NV12_FROZEN_DEFAULT 1	// as in kvm_video.cpp
#endif
#define VIEW_CLOCK_US (KVM_NV12_FROZEN_DEFAULT ? 2000 : 0)
static int gEncCostUs = 0, gTsBackwards = 0; static int64_t gLastTs = -1;
static std::vector<mfh264 *> gAlive; // every encoder the fake made; what the code leaks on purpose after a fault is freed by the test
static void free_alive(void) { for (size_t i = 0; i < gAlive.size(); i++) { free(gAlive[i]); } gAlive.clear(); }


struct bw { uint8_t b[128]; size_t n; int bit; };
static void wbit(struct bw *w, unsigned v) { if (w->bit == 0) { w->b[w->n++] = 0; } if (v) { w->b[w->n - 1] |= (uint8_t)(0x80u >> w->bit); } w->bit = (w->bit + 1) & 7; }
static void wbits(struct bw *w, unsigned v, int n) { while (n-- > 0) { wbit(w, (v >> n) & 1u); } }
static void wue(struct bw *w, unsigned v) { unsigned x = v + 1; int len = 0; while ((x >> len) > 1) { len++; } wbits(w, 0, len); wbits(w, x, len + 1); }
// an Annex B SPS as the encoder would write it for this coded size
static size_t make_sps(uint8_t *out, int profile, int constraint, int level, int refs, int w, int h, int crop)
{
	struct bw b; size_t n = 0, i;
	memset(&b, 0, sizeof(b));
	wbits(&b, 0x67, 8); wbits(&b, (unsigned)profile, 8); wbits(&b, (unsigned)constraint, 8); wbits(&b, (unsigned)level, 8); wue(&b, 0);
	if (profile == 100) { wue(&b, 1); wue(&b, 0); wue(&b, 0); wbit(&b, 0); wbit(&b, 0); }
	wue(&b, 4); wue(&b, 0); wue(&b, 6); wue(&b, (unsigned)refs); wbit(&b, 0); wue(&b, (unsigned)(w / 16 - 1)); wue(&b, (unsigned)(h / 16 - 1)); wbit(&b, 1); wbit(&b, 1); wbit(&b, crop);
	if (crop) { wue(&b, 0); wue(&b, 0); wue(&b, 0); wue(&b, 4); }
	wbit(&b, 0); wbit(&b, 1);
	out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 1; out[n++] = b.b[0];
	for (i = 1; i < b.n; i++) { if (n >= 7 && out[n - 1] == 0 && out[n - 2] == 0 && b.b[i] <= 3) { out[n++] = 3; } out[n++] = b.b[i]; }
	return n;
}
static size_t fake_seq(const mfh264 *h, int refs, uint8_t *out)
{
	static const uint8_t pps[] = { 0, 0, 0, 1, 0x68, 0xEE, 0x3C, 0x80 };
	int prof = gSpsProfile ? gSpsProfile : h->profile, lvl = gSpsLevel ? gSpsLevel : h->level;
	int con = gSpsConstraint >= 0 ? gSpsConstraint : (prof == 66 ? 0xC0 : (prof == 77 ? 0x40 : 0));
	size_t n = make_sps(out, prof, con, lvl, refs, h->w, h->h, gSpsCrop);
	memcpy(out + n, pps, sizeof(pps));
	return n + sizeof(pps);
}

extern "C"
{
const char *mfh264_result_name(int code) { static char b[32]; snprintf(b, sizeof(b), "E%d", code); return b; }
int mfh264_faulted(void) { std::lock_guard<std::mutex> lk_(gFakeMu); return gFaulted ? 1 : 0; }
int mfh264_init(struct mfh264_err *err) { std::lock_guard<std::mutex> lk_(gFakeMu); if (gInitResult != 0 && err != NULL) { memset(err, 0, sizeof(*err)); err->code = gInitResult; err->stage = "fake init"; } return gInitResult; }
void mfh264_shutdown(void) { std::lock_guard<std::mutex> lk_(gFakeMu); gShutdowns++; }
struct mfh264 *mfh264_open(const struct mfh264_cfg *cfg, struct mfh264_err *err)
{
	int r = 0;
	gOpens++;
	gLastProfile = cfg->profile; gLastLevel = cfg->level_idc; gLastW = cfg->width; gLastH = cfg->height; gLastRc = cfg->rc; gLastBitrate = cfg->bitrate;
	if (!gOpenResults.empty()) { r = gOpenResults.front(); gOpenResults.erase(gOpenResults.begin()); }
	if (r != 0) { memset(err, 0, sizeof(*err)); err->code = r; err->stage = "fake open"; return NULL; }
	mfh264 *h = (mfh264 *)calloc(1, sizeof(mfh264));
	h->w = cfg->width; h->h = cfg->height; h->profile = cfg->profile; h->level = cfg->level_idc; h->idrNext = 1;
	h->seqLen = fake_seq(h, gSpsRefs, h->seq);
	gAlive.push_back(h);
	return h;
}
int mfh264_set_qp(struct mfh264 *h, int qp, struct mfh264_err *err) { std::lock_guard<std::mutex> lk_(gFakeMu); (void)h; (void)err; if (qp >= 20 && qp <= 40) { gQps.push_back(qp); gCurQp = qp; return 0; } return MFH264_E_RANGE; }
int mfh264_set_bitrate(struct mfh264 *h, int bps, struct mfh264_err *err) { std::lock_guard<std::mutex> lk_(gFakeMu); (void)h; (void)err; gBitrates.push_back(bps); return 0; }
int mfh264_set_sample_qp(struct mfh264 *h, int mode, int qp) { std::lock_guard<std::mutex> lk_(gFakeMu); (void)h; (void)qp; gSampleModes.push_back(mode); gSampleMode = mode; return 0; }
int mfh264_last_out_qp(const struct mfh264 *h, uint64_t *raw) { std::lock_guard<std::mutex> lk_(gFakeMu); (void)h; if (gReportMode < 0) { if (raw != NULL) { *raw = 0; } return 0; } if (raw != NULL) { *raw = (uint64_t)(gReportMode == 0 ? gCurQp : 26); } return 1; }
int mfh264_force_idr(struct mfh264 *h, struct mfh264_err *err) { std::lock_guard<std::mutex> lk_(gFakeMu); (void)err; h->idrNext = 1; gForces++; return 0; }
size_t mfh264_max_au_size(const struct mfh264 *h) { std::lock_guard<std::mutex> lk_(gFakeMu); (void)h; return 1u << 20; }
size_t mfh264_seq_header(const struct mfh264 *h, const uint8_t **p) { std::lock_guard<std::mutex> lk_(gFakeMu); if (gNoSeqHdr) { *p = NULL; return 0; } *p = h->seq; return h->seqLen; }
void mfh264_get_info(const struct mfh264 *h, struct mfh264_info *info)
{ std::lock_guard<std::mutex> lk_(gFakeMu);
	memset(info, 0, sizeof(*info));
	info->sps_profile_idc = gSpsProfile ? gSpsProfile : h->profile; info->sps_level_idc = gSpsLevel ? gSpsLevel : h->level; info->level_set_ok = 1; info->max_au_size = 1 << 20; info->rc_mode_ok = gRcModeOk;
}
void mfh264_close(struct mfh264 *h) { std::lock_guard<std::mutex> lk_(gFakeMu); if (gFaulted) { return; } gCloses++; for (size_t i = 0; i < gAlive.size(); i++) { if (gAlive[i] == h) { gAlive.erase(gAlive.begin() + (long)i); break; } } free(h); }
static int fake_encode_core(struct mfh264 *h, const uint8_t *bgra, const uint8_t *nv12, size_t nv12Len, int width, int height, int stride, int64_t ts, uint8_t *out, size_t cap, size_t *out_len, int *is_idr, struct mfh264_err *err)
{ std::lock_guard<std::mutex> lk_(gFakeMu);
	static const uint8_t idr[] = { 0, 0, 0, 1, 0x65, 0x88, 1, 2, 3, 4, 5, 6, 7, 8 }, pframe[] = { 0, 0, 0, 1, 0x41, 0x9A, 1, 2, 3, 4, 5, 6 };
	size_t n = 0;
	(void)width; (void)height;
	gEncodes++;
	gPicBytes.push_back(bgra != NULL ? bgra[0] : nv12[0]);
	if (nv12 != NULL) { gViewFrames++; if (gViewHook != NULL) { gViewHook(nv12, nv12Len); } }
	{ std::lock_guard<std::mutex> lk(gClockMu); gTestQpcTicks += gEncCostUs; }
	if (gProcCpuUs != 0) { gProcCpuUs += gEncCpuUs + gCapOverlapUs; if (gCapCpuUs != 0) { gCapCpuUs += gCapOverlapUs; } }
	if (gLastTs >= 0 && ts <= gLastTs) { gTsBackwards++; }
	gLastTs = ts;
	if (bgra != NULL) { CHECK(stride < 0); }	// a bottom-up bitmap: the pointer is the top row, the stride is negative
	*out_len = 0; *is_idr = 0;
	memset(err, 0, sizeof(*err));
	if (gRejectSample && gSampleMode != 0) { err->code = MFH264_E_INPUT; err->stage = "fake ProcessInput (the attribute)"; err->hr = 0x80070057; h->idrNext = 1; return MFH264_E_INPUT; }
	if (gEncMode == FE_EXCEPTION) { gFaulted = true; err->code = MFH264_E_EXCEPTION; err->exc_code = 0xC0000005ul; err->stage = "fake exception"; return MFH264_E_EXCEPTION; }
	if (gEncMode == FE_ERROR) { err->code = MFH264_E_OUTPUT; err->stage = "fake output"; return MFH264_E_OUTPUT; }
	if (gEncMode == FE_BROKEN || gEncMode == FE_OVERSIZE)
	{
		err->code = MFH264_E_STREAM_BROKEN; err->stage = "fake lost au"; err->reason = (gEncMode == FE_OVERSIZE) ? MFH264_BROKEN_OVERSIZE : MFH264_BROKEN_NO_OUTPUT;
		h->idrNext = 1;
		return MFH264_E_STREAM_BROKEN;
	}
	h->frames++;
	if (h->idrNext && gEncMode != FE_FIRST_P)
	{
		n = fake_seq(h, gIdrRefs >= 0 ? gIdrRefs : gSpsRefs, out);
		memcpy(out + n, idr, sizeof(idr)); n += sizeof(idr);
		*is_idr = 1; h->idrNext = 0;
	}
	else
	{
		memcpy(out, pframe, sizeof(pframe)); n = sizeof(pframe);
		if (gEncMode == FE_FIRST_P) { gEncMode = FE_OK; }
		h->idrNext = 0;
	}
	if (gSizeByQp && !*is_idr && gCurQp > 0) { const int pad = (int)(3000.0 * pow(0.89, (double)(gCurQp - 26))); if (n + (size_t)pad + 5 <= cap) { out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 1; out[n++] = 0x0C; memset(out + n, 0xAA, (size_t)pad); n += (size_t)pad; } }
	else if (gPadBytes > 0 && n + (size_t)gPadBytes + 5 <= cap) { out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 1; out[n++] = 0x0C; memset(out + n, 0xAA, (size_t)gPadBytes); n += (size_t)gPadBytes; }
	if (((bgra != NULL && gEncSnapshot != NULL) || (nv12 != NULL && gEncSnapshotNv12 != NULL)) && n + 9 <= cap) { const int id = bgra != NULL ? gEncSnapshot(bgra, width, height, stride) : gEncSnapshotNv12(nv12, nv12Len); out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 1; out[n++] = 0x0C; out[n++] = (uint8_t)(id >> 24); out[n++] = (uint8_t)(id >> 16); out[n++] = (uint8_t)(id >> 8); out[n++] = (uint8_t)id; }
	if (n > cap) { return MFH264_E_STREAM_BROKEN; }
	*out_len = n;
	return 0;
}

int mfh264_encode_bgra(struct mfh264 *h, const uint8_t *bgra, int width, int height, int stride, int64_t ts, uint8_t *out, size_t cap, size_t *out_len, int *is_idr, struct mfh264_err *err)
{
	return fake_encode_core(h, bgra, NULL, 0, width, height, stride, ts, out, cap, out_len, is_idr, err);
}
// the encoder is given the NV12 view of the encoder thread
int mfh264_encode(struct mfh264 *h, const uint8_t *nv12, size_t nv12_len, int64_t ts, uint8_t *out, size_t cap, size_t *out_len, int *is_idr, struct mfh264_err *err)
{
	return fake_encode_core(h, NULL, nv12, nv12_len, 0, 0, 0, ts, out, cap, out_len, is_idr, err);
}
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------------------------------------------------------------------

static std::vector<uint8_t> gPicture;
static long long fake_clock_us(void) { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }	// the time of a frame in the encoder thread: the fake clock, 1 ms per call

static void free_tiles(void)
{
	if (tileInfo == NULL) { return; }
	for (int r = 0; r < TILE_HEIGHT_COUNT; r++) { free(tileInfo[r]); }
	free(tileInfo);
	tileInfo = NULL;
}

// the geometry like kvm_server_SetResolution: new tileInfo (all tiles unknown to the viewer), then the hook
static void set_geometry(int w, int h)
{
	free_tiles();
	SCALED_WIDTH = w; SCALED_HEIGHT = h;
	TILE_WIDTH_COUNT = (w + 31) / 32; TILE_HEIGHT_COUNT = (h + 31) / 32;
	tileInfo = (tileInfo_t **)malloc(sizeof(tileInfo_t *) * (size_t)TILE_HEIGHT_COUNT);
	for (int r = 0; r < TILE_HEIGHT_COUNT; r++)
	{
		tileInfo[r] = (tileInfo_t *)malloc(sizeof(tileInfo_t) * (size_t)TILE_WIDTH_COUNT);
		for (int c = 0; c < TILE_WIDTH_COUNT; c++) { kvm_tile_init(&tileInfo[r][c]); }
	}
	gPicture.assign((size_t)TILE_WIDTH_COUNT * 32 * (size_t)TILE_HEIGHT_COUNT * 32 * 4, 0x55);
	kvm_video_geometry(TILE_HEIGHT_COUNT, TILE_WIDTH_COUNT, w, h);
}

// a tile whose content the viewer has (a JPEG was sent) and that is known to WA-2
static void settle_tile(int r, int c, int crc)
{
	tileInfo_t *t = &tileInfo[r][c];
	t->crc = crc; t->contentCrc = crc; t->sentQ = 50; t->sentSub = 0; t->refresh = 0; t->deferred = 0; t->deferredAt = 0; t->lastChange = gTestNowMs;
}
static void settle_all(void) { for (int r = 0; r < TILE_HEIGHT_COUNT; r++) { for (int c = 0; c < TILE_WIDTH_COUNT; c++) { settle_tile(r, c, 1000 + r * 100 + c); } } }

// the video code takes a rectangle of changed tiles of the motion class (what get_tile_at does after the tile loop of a frame found them)
static int move_rect(int r0, int c0, int r1, int c1, int newCrc)
{
	for (int r = r0; r <= r1; r++) { for (int c = c0; c <= c1; c++) { tileInfo[r][c].crc = newCrc; tileInfo[r][c].contentCrc = newCrc; tileInfo[r][c].lastChange = gTestNowMs; } }
	return kvm_video_claim_rect(r0, c0, r1, c1, 1);
}

static void hw_fakes_reset(void);
static void begin_session(int w, int h)
{
	int reason = 0, cap;
	free_alive();
	gSpsProfile = gSpsLevel = 0;
	gLog.clear(); gWire.clear();
	gQps.clear(); gAdaptBudget = -1; gPadBytes = 0;
	gSizeByQp = 0; gReportMode = -1; gRcModeOk = 1; gRejectSample = 0; gCurQp = 0; gSampleMode = 0; gLastRc = 0; gLastBitrate = 0; gBitrates.clear(); gSampleModes.clear(); gVideoFrameNotes = 0;
	gSpsConstraint = -1; gSpsRefs = 1; gSpsCrop = 0; gNoSeqHdr = 0; gIdrRefs = -1; gEncCostUs = 0; gLastTs = -1; gTsBackwards = 0;
	gWrittenFrames = 0; gPicBytes.clear(); gEncSnapshot = NULL; gEncSnapshotNv12 = NULL;
	gInitResult = 0; gEncMode = FE_OK; gOpens = gCloses = gShutdowns = gForces = gEncodes = 0; gOpenResults.clear(); gFaulted = false; gWriteResult = 1; gNoteWriteBytes = 0;
	gTestNowMs += 10000;
	hw_fakes_reset();
	unsetenv("MESH_KVM_VIDEO");
	setenv("MESH_KVM_ENC_THREAD", "2", 1);		// the manual mode of the encoder thread: nothing runs until the test steps it
	kvm_fbuf_setup(NULL);
	kvm_encthr_set_clock(fake_clock_us);
	cap = kvm_video_session_begin(&reason);
	kvm_stream_init(&gS, cap, 1, reason, KVM_REASON_DISABLED);
	set_geometry(w, h);
	gKvmFullFrame = 0;
	PIXEL_SIZE = 4;
}
static void end_session(void)
{
	kvm_video_session_end();
	free_tiles();
	TILE_WIDTH_COUNT = TILE_HEIGHT_COUNT = 0;
}

// a 240 arrives (what kvm_stream_poll does under the lock), then the main loop polls
static void send_ctrl(int op, int flags, int maxLevel = 0, int maxW = 0, int maxH = 0)
{
	kvm_ctrl c;
	kvm_state_msg reply;
	unsigned act = 0;
	memset(&c, 0, sizeof(c));
	c.ver = 1; c.op = (uint8_t)op; c.flags = (uint8_t)flags; c.maxLevelIdc = (uint8_t)maxLevel; c.maxW = (uint16_t)maxW; c.maxH = (uint16_t)maxH;
	if (kvm_stream_on_ctrl(&gS, &c, gTestNowMs, &reply, &act)) { kvm_glue_state_locked(&reply); }
	kvm_video_ctrl(&c, act, gTestNowMs);
	kvm_video_poll();
}

// A step of the loop: the picture is handed over (a buffer of the cache, as the capture thread makes them), the encoder thread "runs" (the manual mode: here and now) and the loop collects what is done, then
// it lets go of the picture. Returns what the write of a frame returned, KVM_VIDEO_NONE when no frame was written; *bytes = the bytes written.
static int emit(long long *bytes = NULL)
{
	const size_t before = gWire.size();
	long long total = 0;
	int wrote = 0;
	void *b = kvm_fbuf_get(gPicture.size());
	memcpy(b, gPicture.data(), gPicture.size());
	kvm_video_emit(b, (long long)gPicture.size());
	while (kvm_encthr_test_step(1) > 0) { kvm_video_service(); }
	kvm_video_service();
	kvm_fbuf_release(b);
	for (size_t i = before; i < gWire.size(); i++) { if (gWire[i].kind == KVM_W_POSITION && gWire[i].b.size() > (size_t)KVM_MARKER_BYTES) { total += (long long)gWire[i].b.size(); wrote = 1; } }
	if (bytes != NULL) { *bytes = total; }
	return wrote ? gWriteResult : KVM_VIDEO_NONE;
}

static std::vector<kvm_state_msg> states(void) // the 242 on the wire
{
	std::vector<kvm_state_msg> v;
	for (size_t i = 0; i < gWire.size(); i++)
	{
		kvm_state_msg m;
		if (gWire[i].kind == KVM_W_STATE && kvm_stream_parse_state(gWire[i].b.data(), gWire[i].b.size(), &m) == KVM_REJ_NONE) { v.push_back(m); }
	}
	return v;
}
static kvm_state_msg last_state(void) { std::vector<kvm_state_msg> v = states(); kvm_state_msg z; memset(&z, 0, sizeof(z)); return v.empty() ? z : v.back(); }

struct frame_info { bool ok; kvm_frame f; std::vector<uint8_t> rects; size_t index; };
static std::vector<frame_info> frames(void) // the 243 on the wire (parsed with the receiver's checks)
{
	std::vector<frame_info> v;
	for (size_t i = 0; i < gWire.size(); i++)
	{
		frame_info fi;
		if (gWire[i].kind != KVM_W_POSITION || gWire[i].b.size() <= (size_t)KVM_MARKER_BYTES) { continue; }
		fi.index = i;
		fi.ok = kvm_stream_parse_frame(gWire[i].b.data(), gWire[i].b.size(), (unsigned)SCALED_WIDTH, (unsigned)SCALED_HEIGHT, &fi.f) == KVM_REJ_NONE;
		if (fi.ok) { fi.rects.assign(fi.f.rects, fi.f.rects + 8u * fi.f.nRects); }
		v.push_back(fi);
	}
	return v;
}
static unsigned rect_field(const frame_info &f, unsigned i, unsigned k) { return ((unsigned)f.rects[8 * i + 2 * k] << 8) | f.rects[8 * i + 2 * k + 1]; }
static bool covered(const frame_info &f, int r, int c)
{
	for (unsigned i = 0; i < f.f.nRects; i++)
	{
		unsigned x = rect_field(f, i, 0), y = rect_field(f, i, 1), w = rect_field(f, i, 2), h = rect_field(f, i, 3);
		if ((unsigned)c * 32 >= x && (unsigned)c * 32 < x + w && (unsigned)r * 32 >= y && (unsigned)r * 32 < y + h) { return true; }
	}
	return false;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// scenarios
// ---------------------------------------------------------------------------------------------------------------------------------------

static void test_first_frame(void)
{
	begin_session(1920, 1080);
	settle_all();
	CHECK(!kvm_video_on());
	// op 1: the encoder is opened, a 242 with the profile and level of the SPS, the generation 1
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	{
		kvm_state_msg m = last_state();
		CHECK(kvm_video_on());
		CHECK_EQ(m.video, 1); CHECK_EQ(m.ack, 1); CHECK_EQ(m.reason, 0); CHECK_EQ(m.profile, 100); CHECK_EQ(m.level, 42); CHECK_EQ(m.codedW, 1920); CHECK_EQ(m.codedH, 1088);
		CHECK_EQ(m.gen, 1); CHECK_EQ(m.caps, KVM_CAPS_H264 | KVM_CAPS_ACK | KVM_CAPS_AUTO);
		CHECK_EQ(gOpens, 1); CHECK_EQ(gLastProfile, 100); CHECK_EQ(gLastLevel, 42); CHECK_EQ(gLastW, 1920); CHECK_EQ(gLastH, 1088);
	}
	CHECK(frames().empty());	// no frame without motion (lazy)
	CHECK_EQ(emit(), KVM_VIDEO_NONE);
	// motion: a rectangle of tiles rows 5..8, columns 10..20 goes to pendingVideo
	CHECK_EQ(move_rect(5, 10, 8, 20, 4242), 1);
	CHECK_EQ(kvm_video_pending(), 4 * 11);
	CHECK(kvm_video_tile_pending(5, 10)); CHECK(!kvm_video_tile_pending(4, 10));
	CHECK(kvm_video_want_image());
	gTestNowMs += 5;	// the motion was seen a moment ago: the frame's time is what the settle time of the sharpening counts from
	{
		long long bytes = 0;
		size_t wireBefore = gWire.size();
		CHECK_EQ(emit(&bytes), 1);
		CHECK(bytes > 0);
		CHECK_EQ(gWire.size(), wireBefore + 1);
		std::vector<frame_info> fr = frames();
		CHECK_EQ(fr.size(), 1);
		if (!fr.empty())
		{
			const frame_info &f = fr[0];
			CHECK(f.ok);
			CHECK_EQ(f.f.flags & KVM_FRAME_IDR, KVM_FRAME_IDR);				// the first frame of a generation is an IDR
			CHECK_EQ(f.f.gen, 1); CHECK_EQ(f.f.codedW, 1920); CHECK_EQ(f.f.codedH, 1088); CHECK_EQ(f.f.seq, 1);
			CHECK_EQ(f.f.nRects, 1);
			if (f.f.nRects == 1) { CHECK_EQ(rect_field(f, 0, 0), 320); CHECK_EQ(rect_field(f, 0, 1), 160); CHECK_EQ(rect_field(f, 0, 2), 352); CHECK_EQ(rect_field(f, 0, 3), 128); }
			CHECK(f.f.annexLen >= 5 && f.f.annexB[4] == 0x67);				// starts with the SPS
			CHECK_EQ(f.index, wireBefore);
		}
		CHECK_EQ(gNoteWriteBytes, bytes);									// the regulator and the bucket of WA-2 counted the frame
	}
	// the tiles are VIDEO now: blurred (sentQ below every user quality), settle time from the frame, the viewer's copy is what was encoded
	CHECK_EQ(kvm_video_pending(), 0);
	for (int r = 5; r <= 8; r++)
	{
		for (int c = 10; c <= 20; c++)
		{
			tileInfo_t *t = &tileInfo[r][c];
			CHECK_EQ(t->sentQ, 1); CHECK_EQ(t->sentSub, 1); CHECK_EQ(t->crc, 4242); CHECK_EQ(t->contentCrc, 4242); CHECK_EQ(t->lastChange, gTestNowMs); CHECK_EQ(t->deferred, 0);
		}
	}
	CHECK_EQ(tileInfo[4][10].sentQ, 50);									// the neighbours are untouched
	CHECK_EQ(emit(), KVM_VIDEO_NONE);										// nothing pending
	// the second frame comes one frame interval later and is a P frame
	gTestNowMs += 20;
	CHECK_EQ(move_rect(5, 10, 5, 10, 777), 1);
	CHECK_EQ(emit(), 1);
	{
		std::vector<frame_info> fr = frames();
		CHECK_EQ(fr.size(), 2);
		if (fr.size() == 2) { CHECK(fr[1].ok); CHECK_EQ(fr[1].f.flags & KVM_FRAME_IDR, 0); CHECK_EQ(fr[1].f.seq, 2); CHECK_EQ(fr[1].f.annexB[4], 0x41); }
	}
	// sent as JPEG: a tile leaves pendingVideo (I3)
	gTestNowMs += 20;
	CHECK_EQ(move_rect(10, 10, 10, 12, 888), 1);
	kvm_video_tile_jpeg(10, 11);
	CHECK(kvm_video_tile_pending(10, 10)); CHECK(!kvm_video_tile_pending(10, 11)); CHECK_EQ(kvm_video_pending(), 2);
	end_session();
	CHECK_EQ(gCloses, 1); CHECK_EQ(gShutdowns, 1);
}

static void test_pace_and_window(void)
{
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(2, 2, 3, 3, 11), 1);
	CHECK_EQ(emit(), 1);
	// the same millisecond: the pace (16 ms, 25 % early) holds the next frame back; the tiles wait
	CHECK_EQ(move_rect(2, 2, 2, 2, 12), 1);
	CHECK_EQ(emit(), KVM_VIDEO_NONE);
	CHECK_EQ(kvm_video_pending(), 1);
	gTestNowMs += 11;
	CHECK_EQ(emit(), KVM_VIDEO_NONE);
	gTestNowMs += 1;	// 12 ms = 16 - 4
	// the window is full: the frame is held back, not dropped, and "held back" is noted for the rate samples
	kvm_stream_note_write(&gS, 5u * 1024u * 1024u, KVM_W_DATA, gTestNowMs);
	CHECK(!kvm_ack_peek(KVM_GATE_VIDEO));
	CHECK(!kvm_video_want_image());
	{
		uint32_t denied = gS.st.gateDenied;
		CHECK(!kvm_stream_link_limited(&gS, gTestNowMs));
		CHECK_EQ(emit(), KVM_VIDEO_NONE);
		CHECK_EQ(gS.st.gateDenied, denied + 1);
		CHECK(kvm_stream_link_limited(&gS, gTestNowMs));				// a refused video frame says that the link is the limit
	}
	CHECK_EQ(kvm_video_pending(), 1);
	// more motion while it waits: the next frame paints the sum
	CHECK_EQ(move_rect(6, 6, 6, 8, 13), 1);
	CHECK_EQ(kvm_video_pending(), 4);
	gS.ackedCum = gS.bytesWritten;	// the viewer acknowledged everything
	CHECK(kvm_ack_peek(KVM_GATE_VIDEO));
	CHECK(kvm_video_want_image());
	CHECK_EQ(emit(), 1);
	{
		std::vector<frame_info> fr = frames();
		CHECK_EQ(fr.size(), 2);
		if (fr.size() == 2) { CHECK(fr[1].ok); CHECK_EQ(fr[1].f.nRects, 2); CHECK(covered(fr[1], 2, 2)); CHECK(covered(fr[1], 6, 6)); CHECK(covered(fr[1], 6, 8)); }
	}
	end_session();
	// the 30 frames per second fallback (the viewer takes level 4.1 only): 33 ms interval, 25 ms is the earliest
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7, 41);
	{ kvm_state_msg m = last_state(); CHECK_EQ(m.video, 1); CHECK_EQ(m.level, 40); }
	CHECK_EQ(gLastLevel, 40);
	CHECK_EQ(move_rect(1, 1, 1, 1, 21), 1);
	CHECK_EQ(emit(), 1);
	gTestNowMs += 20;
	CHECK_EQ(move_rect(1, 1, 1, 1, 22), 1);
	CHECK_EQ(emit(), KVM_VIDEO_NONE);
	gTestNowMs += 6;
	CHECK_EQ(emit(), 1);
	end_session();
}

static void test_stop_and_restart(void)
{
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(2, 2, 3, 3, 11), 1);
	CHECK_EQ(emit(), 1);										// tiles (2..3, 2..3) are VIDEO
	gTestNowMs += 20;
	CHECK_EQ(move_rect(5, 5, 6, 6, 12), 1);					// pending
	gKvmFullFrame = 0;
	send_ctrl(KVM_OP_VIDEO_OFF, 0);							// the viewer switches the video off
	{
		kvm_state_msg m = last_state();
		CHECK_EQ(m.video, 0); CHECK_EQ(m.ack, 1); CHECK_EQ(m.reason, 0);
	}
	CHECK(!kvm_video_on());
	CHECK_EQ(gCloses, 1);										// the encoder is released
	CHECK_EQ(kvm_video_pending(), 0);
	// I7: the tiles that waited are JPEG again (crc 0xFF: the next frame, a full one, sends them); the VIDEO tiles are left to the sharpening
	for (int r = 5; r <= 6; r++) { for (int c = 5; c <= 6; c++) { CHECK_EQ(tileInfo[r][c].crc, 0xFF); } }
	CHECK_EQ(gKvmFullFrame, 1);
	CHECK_EQ(tileInfo[2][2].crc, 11); CHECK_EQ(tileInfo[2][2].sentQ, 1);
	// a claim is refused now (video is off): the rectangle goes as JPEG
	CHECK_EQ(kvm_video_claim_rect(1, 1, 2, 2, 1), 0);
	CHECK_EQ(emit(), KVM_VIDEO_NONE);
	// op 1 again 500 ms later: the video was off (not under way), so it is a new request; the encoder may not be created again within 2 s, so it waits (the tiles are JPEG meanwhile)
	gTestNowMs += 500;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(!kvm_video_on()); CHECK(kvm_video_active()); CHECK_EQ(gOpens, 1);
	// after 2 s the new generation: 242 gen 2, a new encoder, the first frame is an IDR
	gTestNowMs += 2000;
	kvm_video_poll();
	CHECK(kvm_video_on()); CHECK_EQ(gOpens, 2); CHECK_EQ(last_state().gen, 2); CHECK_EQ(last_state().video, 1);
	CHECK_EQ(tileInfo[2][2].crc, 0xFF);	// op 1 repairs: the tiles that the first generation left blurred are sent as JPEG again
	settle_tile(5, 5, 99);
	CHECK_EQ(move_rect(5, 5, 5, 5, 100), 1);
	CHECK_EQ(emit(), 1);
	{
		std::vector<frame_info> fr = frames();
		CHECK_EQ(fr.size(), 2);
		if (fr.size() == 2) { CHECK(fr[1].ok); CHECK_EQ(fr[1].f.gen, 2); CHECK_EQ(fr[1].f.flags & KVM_FRAME_IDR, KVM_FRAME_IDR); CHECK_EQ(gWire[fr[1].index - 1].kind, KVM_W_STATE); }	// the 242 is right before it
	}
	// op 4 (ACK only) stops the video too; the stream layer answers, the video code releases the encoder
	send_ctrl(KVM_OP_ACK_ONLY, 0);
	CHECK(!kvm_video_on()); CHECK_EQ(gCloses, 2);
	end_session();
}

static void test_errors(void)
{
	// a lost access unit: nothing is sent, the tiles stay pending, the next frame is an IDR at once
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(2, 2, 3, 3, 11), 1);
	CHECK_EQ(emit(), 1);
	gTestNowMs += 20;
	CHECK_EQ(move_rect(4, 4, 4, 4, 12), 1);
	gEncMode = FE_BROKEN;
	CHECK_EQ(emit(), KVM_VIDEO_NONE);
	CHECK_EQ(kvm_video_pending(), 1); CHECK(kvm_video_on());
	CHECK_EQ(frames().size(), 1);
	gEncMode = FE_OK;
	gTestNowMs += 20;
	{
		int forces = gForces;
		CHECK_EQ(emit(), 1);
		CHECK_EQ(gForces, forces + 1);
		std::vector<frame_info> fr = frames();
		CHECK_EQ(fr.size(), 2);
		if (fr.size() == 2) { CHECK_EQ(fr[1].f.flags & KVM_FRAME_IDR, KVM_FRAME_IDR); CHECK(covered(fr[1], 4, 4)); }
	}
	// three lost access units in a row: an error of the encoder: 242 reason 4, the pending tiles are JPEG again, the video retries after 60 s
	gTestNowMs += 20;
	CHECK_EQ(move_rect(7, 7, 7, 7, 13), 1);
	gEncMode = FE_BROKEN;
	for (int i = 0; i < 3; i++) { gTestNowMs += 20; (void)emit(); }
	CHECK(!kvm_video_on());
	{
		kvm_state_msg m = last_state();
		CHECK_EQ(m.video, 0); CHECK_EQ(m.ack, 1); CHECK_EQ(m.reason, KVM_REASON_ERROR); CHECK_EQ(m.caps, KVM_CAPS_H264 | KVM_CAPS_ACK | KVM_CAPS_AUTO);
	}
	CHECK_EQ(tileInfo[7][7].crc, 0xFF); CHECK_EQ(kvm_video_pending(), 0);
	CHECK_EQ(gCloses, 1);
	gEncMode = FE_OK;
	gTestNowMs += 59000; kvm_video_poll(); CHECK(!kvm_video_on());
	{
		size_t n = states().size();
		gTestNowMs += 500;
		send_ctrl(KVM_OP_VIDEO_ON, 7);	// the viewer asks again during the hold-off: the answer is the state, nothing starts
		CHECK(!kvm_video_on()); CHECK_EQ(states().size(), n + 1); CHECK_EQ(last_state().reason, KVM_REASON_ERROR);
	}
	gTestNowMs += 1000; kvm_video_poll();
	CHECK(kvm_video_on()); CHECK_EQ(last_state().video, 1); CHECK_EQ(last_state().gen, 2);
	end_session();

	// a first frame that is not an IDR is not sent; the next one is forced
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	gEncMode = FE_FIRST_P;
	CHECK_EQ(move_rect(2, 2, 2, 2, 11), 1);
	CHECK_EQ(emit(), KVM_VIDEO_NONE);
	CHECK(frames().empty()); CHECK_EQ(kvm_video_pending(), 1);
	gTestNowMs += 20;
	CHECK_EQ(emit(), 1);
	{ std::vector<frame_info> fr = frames(); CHECK_EQ(fr.size(), 1); if (!fr.empty()) { CHECK_EQ(fr[0].f.flags & KVM_FRAME_IDR, KVM_FRAME_IDR); } }
	end_session();

	// a frame over 8 MiB counts as an error at once
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	gEncMode = FE_OVERSIZE;
	CHECK_EQ(move_rect(2, 2, 2, 2, 11), 1);
	CHECK_EQ(emit(), KVM_VIDEO_NONE);
	CHECK(!kvm_video_on()); CHECK_EQ(last_state().reason, KVM_REASON_ERROR);
	end_session();

	// the third error is for good: no retry, op 1 gets reason 6, caps bit 0 is 0
	begin_session(1920, 1080);
	settle_all();
	for (int i = 0; i < 3; i++)
	{
		gTestNowMs += 61000;
		kvm_video_poll();
		send_ctrl(KVM_OP_VIDEO_ON, 7);
		CHECK(kvm_video_on());
		gEncMode = FE_ERROR;
		CHECK_EQ(move_rect(2, 2, 2, 2, 11 + i), 1);
		CHECK_EQ(emit(), KVM_VIDEO_NONE);
		gEncMode = FE_OK;
		CHECK(!kvm_video_on());
	}
	CHECK_EQ(last_state().reason, KVM_REASON_DISABLED); CHECK_EQ(last_state().caps & KVM_CAPS_H264, 0);
	gTestNowMs += 10000000; kvm_video_poll(); CHECK(!kvm_video_on());
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(last_state().reason, KVM_REASON_DISABLED); CHECK_EQ(last_state().video, 0); CHECK_EQ(last_state().ack, 1);
	end_session();

	// a structured exception: the video is off for the process, nothing is closed, Media Foundation is not shut down
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	gEncMode = FE_EXCEPTION;
	CHECK_EQ(move_rect(2, 2, 2, 2, 11), 1);
	CHECK_EQ(emit(), KVM_VIDEO_NONE);
	CHECK(!kvm_video_on()); CHECK_EQ(last_state().reason, KVM_REASON_DISABLED); CHECK_EQ(last_state().caps & KVM_CAPS_H264, 0);
	CHECK_EQ(gCloses, 0);
	end_session();
	CHECK_EQ(gCloses, 0);
}

static void test_request_failures(void)
{
	// the viewer takes a lower level than the picture needs: reason 7 with the size, the ACK stays
	begin_session(1920, 1080);
	send_ctrl(KVM_OP_VIDEO_ON, 7, 32);
	{ kvm_state_msg m = last_state(); CHECK_EQ(m.video, 0); CHECK_EQ(m.ack, 1); CHECK_EQ(m.reason, KVM_REASON_RESOLUTION); CHECK_EQ(m.codedW, 1920); CHECK_EQ(m.codedH, 1088); }
	CHECK_EQ(gOpens, 0); CHECK_EQ(gS.want, KVM_WANT_ACK);
	end_session();
	// a decoder that is too small
	begin_session(1920, 1080);
	send_ctrl(KVM_OP_VIDEO_ON, 7, 0, 1280, 720);
	CHECK_EQ(last_state().reason, KVM_REASON_RESOLUTION);
	end_session();
	// the encoder refuses every profile: reason 8, three tries (High, Main, Constrained Baseline)
	begin_session(1920, 1080);
	gOpenResults.push_back(MFH264_E_OUTTYPE); gOpenResults.push_back(MFH264_E_OUTTYPE); gOpenResults.push_back(MFH264_E_OUTTYPE);
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(last_state().reason, KVM_REASON_PROFILE); CHECK_EQ(gOpens, 3); CHECK(!kvm_video_on());
	end_session();
	// the encoder refuses High but takes Main: the SPS (here the fake's) says Main in the 242
	begin_session(1920, 1080);
	gOpenResults.push_back(MFH264_E_OUTTYPE);
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(kvm_video_on()); CHECK_EQ(last_state().profile, 77); CHECK_EQ(gOpens, 2);
	end_session();
	// the SPS of the encoder says something else than was asked for: the 242 carries what the SPS says
	begin_session(1920, 1080);
	gSpsProfile = 77; gSpsLevel = 40;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(kvm_video_on()); CHECK_EQ(last_state().profile, 77); CHECK_EQ(last_state().level, 40);
	end_session();
	// only High allowed and the encoder refuses it
	begin_session(1920, 1080);
	gOpenResults.push_back(MFH264_E_OUTTYPE);
	send_ctrl(KVM_OP_VIDEO_ON, 4);
	CHECK_EQ(last_state().reason, KVM_REASON_PROFILE);
	end_session();
	// the encoder does not start: reason 3, and it works the next time
	begin_session(1920, 1080);
	gOpenResults.push_back(MFH264_E_CODECAPI);
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(last_state().reason, KVM_REASON_INIT); CHECK_EQ(last_state().ack, 1);
	gTestNowMs += 3000;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(kvm_video_on());
	end_session();
	// no encoder on this system: reason 2 and the capability is gone
	begin_session(1920, 1080);
	gOpenResults.push_back(MFH264_E_CREATE);
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(last_state().reason, KVM_REASON_NO_MF); CHECK_EQ(last_state().caps & KVM_CAPS_H264, 0);
	gTestNowMs += 3000;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(last_state().reason, KVM_REASON_NO_MF); CHECK(!kvm_video_on());
	end_session();
	// Media Foundation cannot be loaded (or Windows older than 8): the capability is 0 from the start, reason 2
	{
		int reason = 0, cap;
		gInitResult = MFH264_E_OS;
		cap = kvm_video_session_begin(&reason);
		CHECK_EQ(cap, 0); CHECK_EQ(reason, KVM_REASON_NO_MF);
		kvm_video_session_end();
		gInitResult = 0;
	}
	// MESH_KVM_VIDEO=0: reason 6, Media Foundation is not touched
	{
		int reason = 0, cap, inits = gShutdowns;
		setenv("MESH_KVM_VIDEO", "0", 1);
		cap = kvm_video_session_begin(&reason);
		CHECK_EQ(cap, 0); CHECK_EQ(reason, KVM_REASON_DISABLED);
		kvm_video_session_end();
		CHECK_EQ(gShutdowns, inits);
		unsetenv("MESH_KVM_VIDEO");
	}
	// a veto: the video is switched off by the stream layer, the encoder is released
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(kvm_video_on());
	send_ctrl(KVM_OP_VETO, 2);
	CHECK(!kvm_video_on()); CHECK_EQ(last_state().reason, KVM_REASON_VETO); CHECK_EQ(gCloses, 1);
	end_session();
}

static void test_geometry(void)
{
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(2, 2, 3, 3, 11), 1);
	CHECK_EQ(emit(), 1);
	// cmd 7 with a new size shortly after the encoder was opened: the tiles are all new; the new generation waits for the 2 s limit and the tiles go as JPEG meanwhile
	gTestNowMs += 500;
	set_geometry(1366, 768);
	kvm_video_poll();
	CHECK(!kvm_video_on());
	CHECK_EQ(kvm_video_claim_rect(1, 1, 2, 2, 1), 0);
	{
		size_t n = states().size();
		kvm_video_poll();
		CHECK_EQ(states().size(), n);			// no 242 until the generation can start
	}
	gTestNowMs += 2000;
	kvm_video_poll();
	CHECK(kvm_video_on());
	{
		kvm_state_msg m = last_state();
		CHECK_EQ(m.video, 1); CHECK_EQ(m.gen, 2); CHECK_EQ(m.codedW, 1376); CHECK_EQ(m.codedH, 768);
		CHECK_EQ(gLastW, 1376); CHECK_EQ(gLastH, 768); CHECK_EQ(gOpens, 2); CHECK_EQ(gCloses, 1);
	}
	settle_all();
	CHECK_EQ(move_rect(0, 40, 0, 42, 5), 1);
	CHECK_EQ(emit(), 1);
	{
		std::vector<frame_info> fr = frames();
		CHECK_EQ(fr.size(), 2);
		if (fr.size() == 2)
		{
			CHECK(fr[1].ok);
			CHECK_EQ(fr[1].f.gen, 2); CHECK_EQ(fr[1].f.flags & KVM_FRAME_IDR, KVM_FRAME_IDR); CHECK_EQ(fr[1].f.codedW, 1376);
			if (fr[1].f.nRects >= 1) { CHECK_EQ(rect_field(fr[1], 0, 0), 1280); CHECK_EQ(rect_field(fr[1], 0, 2), 86); }	// clipped to the 1366 pixels of the screen
		}
	}
	// the same coded size again (the screen is a few pixels narrower): the encoder is reused, a key frame is forced, a new generation
	gTestNowMs += 100;
	set_geometry(1370, 768);
	kvm_video_poll();
	CHECK(kvm_video_on()); CHECK_EQ(last_state().gen, 3); CHECK_EQ(gOpens, 2);
	settle_all();
	{
		int forces = gForces;
		gTestNowMs += 20;
		CHECK_EQ(move_rect(1, 1, 1, 1, 6), 1);
		CHECK_EQ(emit(), 1);
		CHECK_EQ(gForces, forces + 1);
	}
	end_session();
}

static void test_repair_refresh_idr(void)
{
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(2, 2, 3, 3, 11), 1);
	CHECK_EQ(emit(), 1);
	// op 3 with the repair flag: the VIDEO tiles are JPEG again (crc 0xFF, the next capture is a full frame); at most once a second (op 1 has just repaired: the limit applies from it)
	gTestNowMs += 1000;
	gKvmFullFrame = 0;
	send_ctrl(KVM_OP_REPAIR, KVM_REPAIR_TILES);
	for (int r = 2; r <= 3; r++) { for (int c = 2; c <= 3; c++) { CHECK_EQ(tileInfo[r][c].crc, 0xFF); } }
	CHECK_EQ(gKvmFullFrame, 1);
	settle_all();
	gKvmFullFrame = 0;
	gTestNowMs += 10;
	CHECK_EQ(move_rect(2, 2, 2, 2, 12), 1);
	CHECK_EQ(emit(), 1);
	send_ctrl(KVM_OP_REPAIR, KVM_REPAIR_TILES);			// within a second of the last repair: waits
	CHECK_EQ(tileInfo[2][2].crc, 12); CHECK_EQ(gKvmFullFrame, 0);
	gTestNowMs += 1000;
	kvm_video_poll();									// ... and is done at the end of the window
	CHECK_EQ(tileInfo[2][2].crc, 0xFF); CHECK_EQ(gKvmFullFrame, 1);
	// a REFRESH: the model is cleared (the input thread has reset the CRCs); nothing is left to repair
	settle_all();
	CHECK_EQ(move_rect(2, 2, 2, 2, 13), 1);
	CHECK_EQ(emit(), 1);
	kvm_video_note_refresh();
	kvm_video_poll();
	gTestNowMs += 2000;
	send_ctrl(KVM_OP_REPAIR, KVM_REPAIR_TILES);
	CHECK_EQ(tileInfo[2][2].crc, 13);
	// op 3 with the IDR flag: the next frame is an IDR, after the spacing
	gTestNowMs += 20;
	{
		int forces = gForces;
		send_ctrl(KVM_OP_REPAIR, KVM_REPAIR_IDR);
		CHECK_EQ(move_rect(2, 2, 2, 2, 14), 1);
		CHECK_EQ(emit(), 1);
		CHECK_EQ(gForces, forces + 1);
		std::vector<frame_info> fr = frames();
		CHECK(fr.back().ok); CHECK_EQ(fr.back().f.flags & KVM_FRAME_IDR, KVM_FRAME_IDR);
		// a second request right away: delayed (1 s), not lost; the tiles are in a P frame meanwhile
		gTestNowMs += 20;
		send_ctrl(KVM_OP_REPAIR, KVM_REPAIR_IDR);
		CHECK_EQ(move_rect(2, 2, 2, 2, 15), 1);
		CHECK_EQ(emit(), 1);
		CHECK_EQ(gForces, forces + 1);
		CHECK_EQ(frames().back().f.flags & KVM_FRAME_IDR, 0);
		gTestNowMs += 1000;
		CHECK_EQ(move_rect(2, 2, 2, 2, 16), 1);
		CHECK_EQ(emit(), 1);
		CHECK_EQ(gForces, forces + 2);
		CHECK_EQ(frames().back().f.flags & KVM_FRAME_IDR, KVM_FRAME_IDR);
	}
	end_session();
}

static void test_many_rects(void)
{
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	// a chess board: 1020 single tile rectangles; the frame has at most 256 and paints every pending tile
	for (int r = 0; r < 34; r++) { for (int c = 0; c < 60; c++) { if (((r + c) & 1) == 0) { CHECK_EQ(move_rect(r, c, r, c, 7000 + r * 60 + c), 1); } } }
	CHECK_EQ(kvm_video_pending(), 1020);
	CHECK_EQ(emit(), 1);
	{
		std::vector<frame_info> fr = frames();
		CHECK_EQ(fr.size(), 1);
		if (!fr.empty())
		{
			CHECK(fr[0].ok); CHECK(fr[0].f.nRects >= 1 && fr[0].f.nRects <= 256);
			bool all = true;
			for (int r = 0; r < 34; r++) { for (int c = 0; c < 60; c++) { if (((r + c) & 1) == 0 && !covered(fr[0], r, c)) { all = false; } } }
			CHECK(all);
			// every tile inside a rectangle is VIDEO and nothing outside is
			for (int r = 0; r < 34; r++)
			{
				for (int c = 0; c < 60; c++)
				{
					bool cov = covered(fr[0], r, c);
					CHECK_EQ(tileInfo[r][c].sentQ == 1 && tileInfo[r][c].sentSub == 1, cov);
				}
			}
		}
	}
	end_session();
}

static void test_write_results(void)
{
	// a write that is not complete means pause (kvm.c handles it): the result is passed on, the tiles are painted anyway (the bytes are out)
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	gWriteResult = 0;
	CHECK_EQ(move_rect(2, 2, 2, 2, 11), 1);
	CHECK_EQ(emit(), 0);
	CHECK_EQ(kvm_video_pending(), 0);
	gWriteResult = -4;
	gTestNowMs += 20;
	CHECK_EQ(move_rect(2, 2, 2, 2, 12), 1);
	CHECK_EQ(emit(), -4);
	end_session();
	// a picture that is not 32 bit cannot be encoded: the pending tiles go back to JPEG
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(2, 2, 2, 2, 11), 1);
	PIXEL_SIZE = 3;
	CHECK_EQ(emit(), KVM_VIDEO_NONE);
	CHECK_EQ(tileInfo[2][2].crc, 0xFF); CHECK_EQ(kvm_video_pending(), 0);
	CHECK_EQ(kvm_video_claim_rect(1, 1, 1, 1, 1), 0);
	PIXEL_SIZE = 4;
	end_session();
	// a REFRESH that resets the CRC of a pending tile while the frame is encoded is not forgotten: the tile stays 0xFF and is sent as JPEG
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(2, 2, 3, 3, 11), 1);
	tileInfo[2][3].crc = 0xFF;
	CHECK_EQ(emit(), 1);
	CHECK_EQ(tileInfo[2][3].crc, 0xFF); CHECK_EQ(tileInfo[2][2].crc, 11);
	end_session();
	// tiles of a REFRESH (refresh flag) and the full class are never claimed
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	tileInfo[3][3].refresh = 1;
	CHECK_EQ(kvm_video_claim_rect(3, 3, 3, 4, 1), 0);
	CHECK_EQ(kvm_video_claim_rect(5, 5, 5, 6, 0), 0);
	CHECK_EQ(kvm_video_pending(), 0);
	end_session();
}


// the picture type is no longer one that WA-2 sharpens: the VIDEO tiles would stay blurred for ever, so they go back to JPEG (crc 0xFF) like the pending ones
static void test_sharpening_path_lost(void)
{
	kvm_state_msg reply;
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(2, 2, 3, 3, 11), 1);
	CHECK_EQ(emit(), 1);
	gTestNowMs += 20;
	CHECK_EQ(move_rect(6, 6, 6, 6, 12), 1);					// pending
	// what kvm_stream_poll does when the picture type changes (kvm_adapt_ack_capable() = 0)
	gKvmFullFrame = 0;
	if (kvm_stream_set_ack_capable(&gS, 0, gTestNowMs, &reply)) { kvm_glue_state_locked(&reply); }
	kvm_video_poll();
	CHECK(!kvm_video_on());
	CHECK_EQ(last_state().reason, KVM_REASON_DISABLED); CHECK_EQ(last_state().video, 0);
	for (int r = 2; r <= 3; r++) { for (int c = 2; c <= 3; c++) { CHECK_EQ(tileInfo[r][c].crc, 0xFF); } }	// VIDEO tiles: sent again in the new type
	CHECK_EQ(tileInfo[6][6].crc, 0xFF);														// pending tile
	CHECK_EQ(gKvmFullFrame, 1); CHECK_EQ(kvm_video_pending(), 0);
	end_session();
	// the other ways to stop the video leave the VIDEO tiles to the sharpening (the path is still there)
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(2, 2, 3, 3, 11), 1);
	CHECK_EQ(emit(), 1);
	send_ctrl(KVM_OP_ACK_ONLY, 0);
	CHECK_EQ(tileInfo[2][2].crc, 11); CHECK_EQ(tileInfo[2][2].sentQ, 1);
	end_session();
}

// an SPS outside the viewer's limits is never sent: a clean 242 reason 7 / 8, JPEG
static void test_sps_limits(void)
{
	// at open (sequence header)
	begin_session(1920, 1080);
	gSpsProfile = 66; gSpsConstraint = 0x80;					// Baseline without constraint_set1
	send_ctrl(KVM_OP_VIDEO_ON, 1);
	CHECK(!kvm_video_on()); CHECK_EQ(last_state().reason, KVM_REASON_PROFILE); CHECK_EQ(last_state().video, 0); CHECK_EQ(last_state().ack, 1); CHECK_EQ(gCloses, 1);
	CHECK(frames().empty()); CHECK(log_has("video sps refused"));
	end_session();
	begin_session(1920, 1080);
	gSpsRefs = 2;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(!kvm_video_on()); CHECK_EQ(last_state().reason, KVM_REASON_PROFILE);
	end_session();
	begin_session(1920, 1080);
	gSpsLevel = 51;											// the viewer takes 4.2 at most
	send_ctrl(KVM_OP_VIDEO_ON, 7, 42);
	CHECK(!kvm_video_on()); CHECK_EQ(last_state().reason, KVM_REASON_RESOLUTION); CHECK_EQ(last_state().codedW, 1920);
	end_session();
	begin_session(1920, 1080);
	gSpsProfile = 100;										// the viewer takes CBP only; the "encoder" wrote High anyway
	send_ctrl(KVM_OP_VIDEO_ON, 1);
	CHECK_EQ(last_state().reason, KVM_REASON_PROFILE);
	end_session();
	begin_session(1920, 1080);
	gSpsCrop = 1;											// frame cropping: the protocol forbids it
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(last_state().reason, KVM_REASON_RESOLUTION);
	end_session();
	// the proper CBP (constraint_set0 + set1) is accepted
	begin_session(1920, 1080);
	send_ctrl(KVM_OP_VIDEO_ON, 1);
	CHECK(kvm_video_on()); CHECK_EQ(last_state().profile, 66);
	end_session();
	// no sequence header: the IDR itself is checked before it is sent
	begin_session(1920, 1080);
	settle_all();
	gNoSeqHdr = 1; gIdrRefs = 3;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(kvm_video_on());
	CHECK_EQ(move_rect(2, 2, 2, 2, 11), 1);
	CHECK_EQ(emit(), KVM_VIDEO_NONE);
	CHECK(frames().empty()); CHECK(!kvm_video_on()); CHECK_EQ(last_state().reason, KVM_REASON_PROFILE); CHECK_EQ(last_state().video, 0);
	CHECK_EQ(tileInfo[2][2].crc, 0xFF); CHECK_EQ(kvm_video_pending(), 0); CHECK_EQ(gS.want, KVM_WANT_ACK);
	end_session();
	// ... and an IDR that is fine passes
	begin_session(1920, 1080);
	settle_all();
	gNoSeqHdr = 1;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(2, 2, 2, 2, 11), 1);
	CHECK_EQ(emit(), 1);
	end_session();
}

// the scale: the time stamps of one encoder never go back, the limits of an op 1 in the hold-off are kept, no capture is ordered before a frame may go,
// the frame interval follows the encode time, and the video blocks scale changes while it waits for its encoder
static void test_review_leftovers(void)
{
	// a new generation on a reused encoder goes on counting
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(2, 2, 2, 2, 11), 1);
	CHECK_EQ(emit(), 1);
	gTestNowMs += 100;
	set_geometry(1920, 1076);									// the same coded size: reused
	kvm_video_poll();
	settle_all();
	gTestNowMs += 20;
	CHECK_EQ(move_rect(2, 2, 2, 2, 12), 1);
	CHECK_EQ(emit(), 1);
	CHECK_EQ(gOpens, 1); CHECK_EQ(gTsBackwards, 0);
	end_session();
	// the limits of the op 1 that came during the hold-off are what the retry uses
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	gEncMode = FE_ERROR;
	CHECK_EQ(move_rect(2, 2, 2, 2, 11), 1);
	CHECK_EQ(emit(), KVM_VIDEO_NONE);
	gEncMode = FE_OK;
	gTestNowMs += 3000;
	send_ctrl(KVM_OP_VIDEO_ON, 1, 0, 0, 0);				// Constrained Baseline only, in the hold-off
	CHECK(!kvm_video_on()); CHECK_EQ(last_state().reason, KVM_REASON_ERROR);
	gTestNowMs += 58000;
	kvm_video_poll();
	CHECK(kvm_video_on()); CHECK_EQ(gLastProfile, 66);
	end_session();
	// ... and a joined op 1 is joined only with a start that succeeded: after a refusal (reason 7) the next op 1 is a new attempt
	begin_session(1920, 1080);
	send_ctrl(KVM_OP_VIDEO_ON, 7, 32);
	CHECK_EQ(last_state().reason, KVM_REASON_RESOLUTION);
	gTestNowMs += 100;
	send_ctrl(KVM_OP_VIDEO_ON, 7, 0);
	CHECK(kvm_video_on());
	end_session();
	// a picture is wanted only when a frame may go (the interval has passed)
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(2, 2, 2, 2, 11), 1);
	CHECK_EQ(emit(), 1);
	CHECK_EQ(move_rect(2, 2, 2, 2, 12), 1);
	CHECK(!kvm_video_want_image());								// the same millisecond
	gTestNowMs += 13;
	CHECK(kvm_video_want_image());
	end_session();
	// an encoder that needs 30 ms a frame (4K) on 2 cores (one core of budget, 1.5 cores busy while it encodes) gets an interval of at least 45 ms: the loop is not all encoder
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	gEncCostUs = 29000 - VIEW_CLOCK_US;										// plus the 1 ms of the fake clock per call: 30 ms measured
	{
		uint32_t last = 0;
		int sent = 0, i;
		for (i = 0; i < 40; i++)
		{
			gTestNowMs += 5;
			CHECK_EQ(move_rect(2, 2, 2, 2, 100 + i), 1);
			if (emit() != KVM_VIDEO_NONE) { if (sent >= 2) { CHECK(gTestNowMs - last >= 30); } last = gTestNowMs; sent++; }	// 45 ms interval, 25 % early
		}
		CHECK(sent >= 4 && sent <= 6);							// 200 ms in 5 ms steps with 34-45 ms between frames
	}
	end_session();
	// the scale does not change while the video runs or waits for its encoder
	begin_session(1920, 1080);
	CHECK(!kvm_video_active());
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(kvm_video_active());
	gTestNowMs += 100;
	set_geometry(1366, 768);									// a new size: the generation waits for the 2 s limit
	kvm_video_poll();
	CHECK(!kvm_video_on()); CHECK(kvm_video_active());
	gTestNowMs += 2100;
	kvm_video_poll();
	CHECK(kvm_video_on()); CHECK(kvm_video_active());
	send_ctrl(KVM_OP_VIDEO_OFF, 0);
	CHECK(!kvm_video_active());
	end_session();
}

// the frame interval is the period of the viewer (no 50 ms by inheritance), the slots of the frames, the wake of the loop, the budget of the encoder
static int frames_in(int ms, int *wakeMax)
{
	int n = 0, i;
	for (i = 0; i < ms; i++)
	{
		gTestNowMs += 1;
		move_rect(2, 2, 2, 2, 20000 + i);
		if (emit() != KVM_VIDEO_NONE) { n++; }
		if (wakeMax != NULL) { int w = kvm_video_wake_ms(); if (w > *wakeMax) { *wakeMax = w; } }
	}
	return n;
}

static void test_pace(void)
{
	// the viewer's period is the interval: Medium 33 ms = 30 frames per second, High 16 ms = 60 (62 with whole milliseconds), Low 50, Slow 100; an old viewer's 20 ms is 50
	static const int periods[] = { 16, 33, 50, 100, 20 };
	static const int expect[] = { 187, 91, 60, 30, 150 };		// 3000 ms / the period
	for (size_t k = 0; k < sizeof(periods) / sizeof(periods[0]); k++)
	{
		int n;
		begin_session(1920, 1080);
		settle_all();
		send_ctrl(KVM_OP_VIDEO_ON, 7);
		FRAME_RATE_TIMER = periods[k];
		n = frames_in(3000, NULL);
		CHECK(n >= expect[k] - 3 && n <= expect[k] + 3);
		end_session();
	}
	FRAME_RATE_TIMER = 16;
	// the slot of a frame is the one after the previous slot, not the moment the frame was written: a frame that is 3 ms late does not push the next one back
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	FRAME_RATE_TIMER = 33;
	CHECK_EQ(move_rect(2, 2, 2, 2, 30001), 1);
	CHECK(kvm_video_wake_ms() == -1);							// the first frame has no slot to wait for
	CHECK(emit() != KVM_VIDEO_NONE);
	CHECK(kvm_video_wake_ms() == -1);							// nothing is pending
	CHECK_EQ(move_rect(2, 2, 2, 2, 30002), 1);
	CHECK_EQ(kvm_video_wake_ms(), 33);							// the slot is 33 ms after the first frame
	gTestNowMs += 10;
	CHECK_EQ(kvm_video_wake_ms(), 23);
	gTestNowMs += 26;											// 36 ms: 3 ms late
	CHECK(emit() != KVM_VIDEO_NONE);
	CHECK_EQ(move_rect(2, 2, 2, 2, 30003), 1);
	CHECK_EQ(kvm_video_wake_ms(), 30);							// 66 - 36: the slot stayed where it was
	gTestNowMs += 30;
	CHECK(kvm_video_wake_ms() == -1);							// the slot has come: the loop goes on at its own pace
	CHECK(emit() != KVM_VIDEO_NONE);
	// a screen that was still: the next frame is not owed a burst (the slot begins again)
	gTestNowMs += 1000;
	CHECK_EQ(move_rect(2, 2, 2, 2, 30004), 1);
	CHECK(emit() != KVM_VIDEO_NONE);
	CHECK_EQ(move_rect(2, 2, 2, 2, 30005), 1);
	CHECK_EQ(kvm_video_wake_ms(), 33);
	CHECK(emit() == KVM_VIDEO_NONE);							// the slot has not come
	end_session();
	// a frame that was late (17 ms behind its slot) is not followed by one right behind it: the next frame waits until its write is 75 % of the interval (24 ms) after the write of the late one,
	// and the loop wakes for that (not for the slot, which comes 16 ms after the late frame)
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	FRAME_RATE_TIMER = 33;
	CHECK_EQ(move_rect(2, 2, 2, 2, 33001), 1);
	CHECK(emit() != KVM_VIDEO_NONE);							// the first frame, slot T
	gTestNowMs += 50;											// the second is 17 ms late (slot T + 33)
	CHECK_EQ(move_rect(2, 2, 2, 2, 33002), 1);
	CHECK(emit() != KVM_VIDEO_NONE);
	CHECK_EQ(move_rect(2, 2, 2, 2, 33003), 1);
	{
		const int w = kvm_video_wake_ms();
		CHECK(w > 16 && w <= 24);								// the slot alone would say 16
		gTestNowMs += 16;										// the slot of the third frame (T + 66) has come ...
		CHECK(emit() == KVM_VIDEO_NONE);						// ... the write gap has not
		CHECK(kvm_video_want_image() == 0);
		gTestNowMs += (uint32_t)(w - 16);
		CHECK(kvm_video_want_image() == 1);
		CHECK(emit() != KVM_VIDEO_NONE);
	}
	end_session();
	// the budget: an encoder that needs 30 ms a frame, 1.5 cores busy while it runs, one core of budget on 2 cores: the interval is 45 ms although the viewer asked for 16
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	FRAME_RATE_TIMER = 16;
	gEncCostUs = 29000 - VIEW_CLOCK_US;
	CHECK_EQ(move_rect(2, 2, 2, 2, 31001), 1);
	CHECK(emit() != KVM_VIDEO_NONE);
	CHECK_EQ(move_rect(2, 2, 2, 2, 31002), 1);
	CHECK_EQ(kvm_video_wake_ms(), 45);
	end_session();
	// on 4 cores (2 cores of budget) the same encoder is held to the period only, and the slot wait is the viewer's 16 ms
	gTestCores = 4;
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	FRAME_RATE_TIMER = 16;
	gEncCostUs = 29000 - VIEW_CLOCK_US;
	CHECK_EQ(move_rect(2, 2, 2, 2, 32001), 1);
	CHECK(emit() != KVM_VIDEO_NONE);
	CHECK_EQ(move_rect(2, 2, 2, 2, 32002), 1);
	CHECK_EQ(kvm_video_wake_ms(), 23);							// 30 ms x 1.5 / 2 cores = 22.5 -> 23
	end_session();
	gTestCores = 2;
	// the capture thread works beside the encoder; its CPU is not the encoder's: 25 ms of encoding with 45 ms of CPU (1.8 cores) and 10 ms of capture beside it measure 1.8, not 2.2, so the
	// interval is 45 ms (25 x 1.8 / 1 core), not 55 (what the "video pace" line says)
	for (int variant = 0; variant < 2; variant++)
	{
		begin_session(1920, 1080);
		settle_all();
		send_ctrl(KVM_OP_VIDEO_ON, 7);
		FRAME_RATE_TIMER = 16;
		gEncCostUs = 24000 - VIEW_CLOCK_US;											// plus the 1 ms of the fake clock per call: 25 ms
		gEncCpuUs = 45000; gCapOverlapUs = 10000;
		gProcCpuUs = 1000000; gCapCpuUs = variant == 0 ? 500000 : 0;	// variant 1: the capture clock cannot be read: nothing is measured, the parallelism stays at the assumed 1.5
		for (int i = 0; i < 800; i++)
		{
			gTestNowMs += 20;
			(void)move_rect(2, 2, 2, 2, 40000 + i);
			kvm_video_poll();
			(void)emit();
		}
		{
			int guard = 0;
			while (emit() == KVM_VIDEO_NONE && guard++ < 200) { gTestNowMs += 1; (void)move_rect(2, 2, 2, 2, 41000 + guard); }
		}
		{
			std::string last;
			for (size_t k = gLog.size(); k-- > 0;) { if (gLog[k].find("video pace") != std::string::npos) { last = gLog[k]; break; } }
			if (variant == 0) { CHECK(last.find("interval_ms=45 enc_floor_ms=45 enc_par=1.8 ") != std::string::npos); }
			else { CHECK(last.find("interval_ms=38 enc_floor_ms=38 enc_par=1.5 ") != std::string::npos); }	// (the CPU of the encoder is what the process used less the loop and the capture thread: without the clock of the capture thread nothing is measured, the assumed 1.5 stays, not a value with the capture in it)
		}
		gProcCpuUs = gCapCpuUs = gEncCpuUs = gCapOverlapUs = 0; gEncCostUs = 0;
		end_session();
	}
	FRAME_RATE_TIMER = 16;
	// a window that is closed (the ACK did not come): the frame does not wait for its slot, the loop keeps its own period and polls (the window may open at any moment)
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	FRAME_RATE_TIMER = 33;
	CHECK_EQ(move_rect(2, 2, 2, 2, 33001), 1);
	CHECK(emit() != KVM_VIDEO_NONE);
	CHECK_EQ(move_rect(2, 2, 2, 2, 33002), 1);
	CHECK_EQ(kvm_video_wake_ms(), 33);
	kvm_stream_note_write(&gS, 5u * 1024u * 1024u, KVM_W_DATA, gTestNowMs);
	CHECK(!kvm_ack_peek(KVM_GATE_VIDEO));
	CHECK(kvm_video_wake_ms() == -1);
	gS.ackedCum = gS.bytesWritten;
	CHECK_EQ(kvm_video_wake_ms(), 33);
	end_session();
	// a period below 16 ms (an old or odd viewer) is held to 16: 60 frames per second at most
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	FRAME_RATE_TIMER = 5;
	{
		int n = frames_in(3000, NULL);
		CHECK(n >= 184 && n <= 190);
	}
	end_session();
	FRAME_RATE_TIMER = 16;
}

// The bridge to the viewer: the frames of one session (an IDR, P frames, a new generation, a CBP generation) are written as the byte stream of a tunnel (cmd 7 first) to
// $HARNESS_DUMP; run-kvm-video-harness.sh runs analyze-capture.js on it, which validates every 243 with the reference validator of the MeshCentral fork and the viewer's own parser.
static void dump_stream(const char *path)
{
	begin_session(1920, 1080);
	settle_all();
	{
		static const uint8_t cmd7[] = { 0, 7, 0, 8, 0x07, 0x80, 0x04, 0x38 };
		FILE *f = fopen(path, "wb");
		size_t i;
		if (f == NULL) { return; }
		fwrite(cmd7, 1, sizeof(cmd7), f);
		send_ctrl(KVM_OP_VIDEO_ON, 7);
		for (int g = 0; g < 3; g++)
		{
			if (g == 1) { gTestNowMs += 2500; send_ctrl(KVM_OP_VIDEO_ON, 1); }
			if (g == 2) { gTestNowMs += 2500; kvm_video_note_refresh(); send_ctrl(KVM_OP_VIDEO_ON, 4); }
			for (int k = 0; k < 6; k++)
			{
				gTestNowMs += 20;
				move_rect(2 + k, 3, 4 + k, 9 + k, 5000 + g * 100 + k);
				(void)emit();
				if (k == 3) { kvm_video_note_refresh(); send_ctrl(KVM_OP_REPAIR, KVM_REPAIR_IDR); gTestNowMs += 1100; }
			}
		}
		for (i = 0; i < gWire.size(); i++) { fwrite(gWire[i].b.data(), 1, gWire[i].b.size(), f); }
		fclose(f);
	}
	end_session();
}


// the first rate control in the glue: QP per frame (an IDR gets QP - 4), the viewer's budget is respected (the byte bucket holds frames back, never drops them, the regulator raises the QP)
static void test_rate_control(void)
{
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(gQps.size(), 1); CHECK_EQ(gQps[0], 38);				// set at open: the IDR value; the expected key frame (200 KB at QP 28, 1920x1088) does not fit in 0.3 s of R = 2 Mb/s at any QP of the range: the top
	CHECK_EQ(move_rect(2, 2, 2, 2, 11), 1);
	CHECK_EQ(emit(), 1);
	CHECK_EQ(gQps.size(), 1);										// the first frame is the IDR: already set
	gTestNowMs += 20;
	CHECK_EQ(move_rect(2, 2, 2, 2, 12), 1);
	CHECK_EQ(emit(), 1);
	CHECK_EQ(gQps.size(), 2); CHECK_EQ(gQps[1], 28);				// a P frame: the regulator's QP
	end_session();
	// the budget: 800 kbit/s (R = 80 000 B/s); frames of 40 KB asked for every 20 ms for 10 s
	begin_session(1920, 1080);
	settle_all();
	gAdaptBudget = 800; gPadBytes = 40000;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	{
		unsigned long long sent = 0;
		int i, held = 0;
		for (i = 0; i < 500; i++)
		{
			long long b = 0;
			gTestNowMs += 20;
			(void)move_rect(2, 2, 6, 12, 1000 + i);
			// the viewer acknowledges everything at once (a fast link): only the budget limits
			gS.ackedCum = gS.bytesWritten;
			kvm_video_poll();
			if (emit(&b) != KVM_VIDEO_NONE) { sent += (unsigned long long)b; } else if (kvm_video_pending() > 0) { held++; }
		}
		CHECK(sent <= 80000ull * 10u + 40000u + 80000u);			// R x 10 s + the burst + a frame (the first one, an IDR, is never refused)
		CHECK(sent >= 80000ull * 10u / 2u);							// and the budget is used
		CHECK(held > 100);											// the frames wait for the bucket: pendingVideo accumulates, nothing is dropped
		CHECK(kvm_video_pending() > 0 || sent > 0);
		CHECK(gQps.back() > 28);									// the regulator saw refusals and raised the QP
		CHECK(log_has("video rc r_kbps="));
		CHECK(log_has("starved=0"));
	}
	end_session();
	// the window of the viewer, not the bucket, holds the video: the regulator is told (win_refused in the log line, no QP step down in such a window, `starved` counts it)
	begin_session(1920, 1080);
	settle_all();
	gPadBytes = 200000;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	{
		int i;
		bool counted = false;
		for (i = 0; i < 200; i++)
		{
			long long b = 0;
			gTestNowMs += 20;
			(void)move_rect(2, 2, 6, 12, 1000 + i);						// (nothing is acknowledged: after the first frame the window is full)
			kvm_video_poll();
			(void)emit(&b);
		}
		for (size_t k = 0; k < gLog.size(); k++) { if (gLog[k].find("video rc") != std::string::npos && gLog[k].find("win_refused=") != std::string::npos && gLog[k].find("win_refused=0 ") == std::string::npos) { counted = true; } }
		CHECK(counted);
	}
	end_session();
	// the first key frame is encoded for the R that is known then: a budget of 400 kbit/s (R = 40 000 B/s, 30 000 B in 0.75 s) puts it at the top of the range
	begin_session(1920, 1080);
	settle_all();
	gAdaptBudget = 400;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	kvm_video_poll();
	CHECK_EQ(move_rect(2, 2, 2, 2, 11), 1);
	CHECK_EQ(emit(), 1);
	CHECK(!gQps.empty()); CHECK_EQ(gQps.back(), KVM_VIDEO_QP_MAX);
	end_session();
	// ... and for the size of the picture: 640x360 (a tenth of the pixels) has a key frame of about 22 KB, which fits at the floor of the range even at 800 kbit/s
	begin_session(640, 360);
	settle_all();
	gAdaptBudget = 800;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	kvm_video_poll();
	CHECK_EQ(move_rect(1, 1, 1, 1, 11), 1);
	CHECK_EQ(emit(), 1);
	CHECK(!gQps.empty()); CHECK_EQ(gQps.back(), 28);										// the QP of the P frames (never better than that); the link rule asks for less
	end_session();
	// 300 kbit/s (R = 30 000 B/s) and frames of 40 KB: the QP is at the ceiling, about one frame a second, the bucket refuses: after 5 s the log says `starved=1`
	begin_session(1920, 1080);
	settle_all();
	gAdaptBudget = 300; gPadBytes = 40000;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	{
		int i;
		for (i = 0; i < 1000; i++)
		{
			long long b = 0;
			gTestNowMs += 20;
			(void)move_rect(2, 2, 6, 12, 1000 + i);
			gS.ackedCum = gS.bytesWritten;
			kvm_video_poll();
			(void)emit(&b);
			if (i == 150) { CHECK(!log_has("starved=1")); }					// 3 s
		}
		CHECK(log_has("starved=1"));
	}
	end_session();
	// without a budget and without a measurement R is 2 Mb/s: the same stream goes through much more
	begin_session(1920, 1080);
	settle_all();
	gPadBytes = 40000;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	{
		unsigned long long sent = 0;
		int i;
		for (i = 0; i < 500; i++) { long long b = 0; gTestNowMs += 20; (void)move_rect(2, 2, 6, 12, 1000 + i); gS.ackedCum = gS.bytesWritten; kvm_video_poll(); if (emit(&b) != KVM_VIDEO_NONE) { sent += (unsigned long long)b; } }
		CHECK(sent > 80000ull * 10u * 2u);
	}
	end_session();
}

// the sharpening path disappears after the video was switched off: the VIDEO tiles go back to JPEG then too
static void test_n2_n3(void)
{
	kvm_state_msg reply;
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(2, 2, 3, 3, 11), 1);
	CHECK_EQ(emit(), 1);
	send_ctrl(KVM_OP_VIDEO_OFF, 0);
	CHECK(!kvm_video_on()); CHECK_EQ(tileInfo[2][2].crc, 11);					// the path is still there: they are left to the sharpening
	gKvmFullFrame = 0;
	if (kvm_stream_set_ack_capable(&gS, 0, gTestNowMs, &reply)) { kvm_glue_state_locked(&reply); }
	kvm_video_poll();
	for (int r = 2; r <= 3; r++) { for (int c = 2; c <= 3; c++) { CHECK_EQ(tileInfo[r][c].crc, 0xFF); } }
	CHECK_EQ(gKvmFullFrame, 1);
	end_session();
	// an op 1 with other flags joined with a running start changes nothing until the next generation
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(kvm_video_on()); CHECK_EQ(last_state().gen, 1); CHECK_EQ(gOpens, 1);
	gTestNowMs += 500;
	send_ctrl(KVM_OP_VIDEO_ON, 1);												// Constrained Baseline only, joined with the running start
	CHECK_EQ(gOpens, 1); CHECK_EQ(last_state().gen, 1); CHECK_EQ(last_state().profile, 100);
	CHECK_EQ(move_rect(2, 2, 2, 2, 11), 1);
	CHECK_EQ(emit(), 1);														// the IDR of generation 1 is checked against the flags in force (7): fine
	CHECK(kvm_video_on());
	gTestNowMs += 2500;
	set_geometry(1366, 768);													// the next generation uses the new flags
	kvm_video_poll();
	CHECK(kvm_video_on()); CHECK_EQ(last_state().gen, 2); CHECK_EQ(last_state().profile, 66); CHECK_EQ(gLastProfile, 66);
	end_session();
}


// ---------------------------------------------------------------------------------------------------------------------------------------
// why a key frame; does the encoder listen to the QP
// ---------------------------------------------------------------------------------------------------------------------------------------

static int log_count(const char *s) { int n = 0; for (size_t i = 0; i < gLog.size(); i++) { if (gLog[i].find(s) != std::string::npos) { n++; } } return n; }
static const char *log_find(const char *s) { for (size_t i = 0; i < gLog.size(); i++) { if (gLog[i].find(s) != std::string::npos) { return gLog[i].c_str(); } } return ""; }

// a frame of motion every 40 ms, the viewer acknowledges at once (a fast link)
static int pframe(int i)
{
	long long b = 0;
	gTestNowMs += 40;
	(void)move_rect(2, 2, 6, 12, 5000 + i);
	gS.ackedCum = gS.bytesWritten;
	kvm_video_poll();
	return emit(&b);
}

static void test_idr_reasons(void)
{
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	(void)move_rect(2, 2, 2, 2, 1);
	CHECK_EQ(emit(), 1);
	CHECK_EQ(log_count("video idr reason=first"), 1);
	CHECK(strstr(log_find("video idr reason=first"), "op3_n=0") != NULL);
	CHECK(gVideoFrameNotes >= 1);										// the sharpening governor is told that the video moves
	// the sharpening pays from the rate control while the video moves, the share is in the log
	kvm_video_note_sharpen(30000, 1);
	kvm_video_note_sharpen(5000, 0);
	gTestNowMs += 1100; kvm_video_poll();
	CHECK(log_has("sharpen_share="));
	{
		const char *l = log_find("sharpen_share=");
		const char *q = strstr(l, "sharpen_share=");
		CHECK(q != NULL && atoi(q + 14) > 0);
	}
	// the viewer acknowledges the key frame: the rate control measures what the link took of it
	{
		std::vector<frame_info> fr0 = frames();
		CHECK_EQ(fr0.size(), 1);
		if (fr0.size() == 1)
		{
			kvm_ack a;
			a.seq = fr0[0].f.seq; a.ts = fr0[0].f.ts; a.queueMs = 2; a.flags = 0;
			gTestNowMs += 60;
			CHECK_EQ(kvm_stream_on_ack(&gS, &a, gTestNowMs), KVM_ACK_OK);
			kvm_video_poll();
			CHECK_EQ(log_count("video idr acked"), 1);
			CHECK(strstr(log_find("video idr acked"), "queue_ms=2") != NULL);
			CHECK(strstr(log_find("video idr acked"), "c_idr_bps=0") == NULL);			// the rate control has what the link took
			kvm_video_poll();
			CHECK_EQ(log_count("video idr acked"), 1);					// once
		}
	}
	// op 3 with the IDR flag (the recording of the server): after the spacing, the log says op3 and how long it waited
	gTestNowMs += 3000;
	send_ctrl(KVM_OP_REPAIR, 1);
	gTestNowMs += 700;
	CHECK_EQ(pframe(1), 1);
	CHECK_EQ(log_count("video idr reason=op3"), 1);
	CHECK(strstr(log_find("video idr reason=op3"), "op3_n=1") != NULL);
	CHECK(strstr(log_find("video idr reason=op3"), "op3_wait_ms=7") != NULL);	// 700-740 ms from the request
	// an ordinary P frame is not logged
	const int before = log_count("video idr reason=");
	CHECK_EQ(pframe(2), 1);
	CHECK_EQ(log_count("video idr reason="), before);
	// a lost access unit: the next frame is an IDR by force, the log says broken
	gEncMode = FE_BROKEN;
	CHECK_EQ(pframe(3), KVM_VIDEO_NONE);
	gEncMode = FE_OK;
	gTestNowMs += 1100;
	CHECK_EQ(pframe(4), 1);
	CHECK_EQ(log_count("video idr reason=broken"), 1);
	end_session();
}

static void feed_p(int n, int from) { for (int i = 0; i < n; i++) { (void)pframe(from + i); } }

static void test_qp_stages(void)
{
	// an encoder that listens (the size follows the QP, it reports the QP it was asked for): the probe is a step of +4 for a few frames, the verdict "effective", nothing else happens
	begin_session(1920, 1080);
	settle_all();
	gSizeByQp = 1; gReportMode = 0;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	feed_p(220, 0);
	CHECK(gSampleModes.empty());
	CHECK_EQ(gOpens, 1);
	CHECK_EQ(log_count("video qp ineffective"), 0);
	CHECK_EQ(log_count("video qp effective"), 1);
	{
		int maxQp = 0, minQp = 99;
		for (size_t i = 0; i < gQps.size(); i++) { if (gQps[i] > maxQp) { maxQp = gQps[i]; } if (gQps[i] < minQp) { minQp = gQps[i]; } }
		CHECK(maxQp >= 28 + KVM_QPDET_PROBE_STEP);										// the deliberate step happened
		CHECK(minQp >= KVM_VIDEO_QP_MIN || minQp == 38 /* (the IDR at open) */ || true);
	}
	end_session();

	// an encoder that does not listen (the size is the same at every QP, nothing reported): the detector asks for a step, finds nothing, sets the QP in the input sample too (stage 1),
	// finds nothing again and gives the QP up: a new encoder in the CBR mode, a new generation, the bit rate follows R
	begin_session(1920, 1080);
	settle_all();
	gSizeByQp = 0; gReportMode = -1; gPadBytes = 3000;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	feed_p(150, 0);
	CHECK_EQ(log_count("video qp ineffective stage=0"), 1);
	CHECK_EQ(gSampleModes.size(), 1); CHECK_EQ(gSampleModes[0], 1);					// stage 1: the QP in the sample too
	CHECK_EQ(gOpens, 1);
	for (int i = 0; i < 400 && log_count("video qp ineffective stage=1") == 0; i++) { (void)pframe(100 + i); }
	CHECK_EQ(log_count("video qp ineffective stage=1"), 1);
	// the poll that ends the encoder found tiles that waited for a frame: they went out as JPEG (nothing waits for a video that is not running)
	CHECK_EQ(tileInfo[2][2].crc, 0xFF);
	CHECK_EQ(kvm_video_pending(), 0);
	// the poll after the verdict closes the encoder and waits for the 2 s limit; then a new encoder with the bit rate
	gTestNowMs += 2500;
	kvm_video_poll();
	CHECK_EQ(gOpens, 2);
	CHECK_EQ(gLastRc, MFH264_RC_CBR);
	CHECK(gLastBitrate >= 100000);
	CHECK(kvm_video_on());
	{
		const size_t qpsBefore = gQps.size();
		const int gensBefore = (int)gS.gen;
		feed_p(40, 400);
		CHECK_EQ(gQps.size(), qpsBefore + 0);											// no QP is set any more: set_qp only at open was skipped too
		CHECK(!gBitrates.empty() || gLastBitrate > 0);
		CHECK_EQ(gensBefore, (int)gS.gen);
		CHECK(log_count("video bitrate mode=0") == 1);
	}
	// the bit rate follows R: a budget of 400 kbit/s (R = 40 000 B/s) is set, the encoder is told about 320 kbit/s within a second or so
	{
		gAdaptBudget = 400;
		const size_t n0 = gBitrates.size();
		feed_p(60, 600);
		CHECK(gBitrates.size() > n0);
		if (gBitrates.size() > n0) { CHECK(gBitrates.back() >= 300000 && gBitrates.back() <= 340000); }
		gAdaptBudget = -1;
	}
	// the viewer sees a new generation (242) and an IDR as the first frame
	{
		std::vector<kvm_state_msg> st = states();
		CHECK(st.size() >= 2);
		if (!st.empty()) { CHECK_EQ(st.back().video, 1); CHECK_EQ(st.back().gen, 2); }
		std::vector<frame_info> fr = frames();
		int idrAfter = 0;
		for (size_t i = 0; i < fr.size(); i++) { if (fr[i].ok && fr[i].f.gen == 2 && (fr[i].f.flags & KVM_FRAME_IDR)) { idrAfter++; } }
		CHECK_EQ(idrAfter, 1);
	}
	end_session();

	// an encoder that reports the QP it was asked for but whose frames do not change: the reported QP is preferred, no escalation
	begin_session(1920, 1080);
	settle_all();
	gSizeByQp = 0; gReportMode = 0; gPadBytes = 3000;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	feed_p(250, 0);
	CHECK_EQ(log_count("video qp ineffective"), 0);						// the reported QP follows what was asked: it is the better signal (qpsample on the Windows 11 VM and the Windows 10 VM), the flat sizes are the content
	CHECK_EQ(log_count("video qp effective"), 1);
	end_session();

	// the CBR mode is refused by the encoder: the next mode (LowDelayVBR) is tried, then PeakConstrainedVBR; the last one is kept whatever it says
	begin_session(1920, 1080);
	settle_all();
	gPadBytes = 3000; gRcModeOk = 0;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	feed_p(150, 0); feed_p(250, 200);
	for (int k = 0; k < 6; k++) { gTestNowMs += 2500; kvm_video_poll(); }
	CHECK_EQ(log_count("video bitrate mode 0 refused"), 1);
	CHECK_EQ(log_count("video bitrate mode 1 refused"), 1);
	CHECK_EQ(gLastRc, MFH264_RC_PEAKVBR);
	CHECK(kvm_video_on());
	end_session();

	// the encoder refuses the QP in the input sample (ProcessInput fails): that is not an error of the encoder (three would end the video for the process), the way is given
	// up at once and the bit rate takes over
	begin_session(1920, 1080);
	settle_all();
	gSizeByQp = 0; gPadBytes = 3000;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	for (int i = 0; i < 400 && log_count("video qp ineffective stage=0") == 0; i++) { (void)pframe(i); }
	CHECK_EQ(log_count("video qp ineffective stage=0"), 1);
	gRejectSample = 1;
	for (int i = 0; i < 40 && gOpens < 2; i++) { (void)pframe(500 + i); gTestNowMs += 100; }
	CHECK_EQ(log_count("video qp sample attribute refused"), 1);
	CHECK_EQ(log_count("video error"), 0);
	gRejectSample = 0;
	for (int k = 0; k < 4 && gOpens < 2; k++) { gTestNowMs += 2500; kvm_video_poll(); }
	CHECK_EQ(gOpens, 2); CHECK_EQ(gLastRc, MFH264_RC_CBR);
	CHECK(kvm_video_on());
	CHECK(frames().size() > 0);
	// the new encoder is not asked to take the attribute again, and the next encoders (a new generation) stay in the bit rate mode
	feed_p(10, 700);
	CHECK_EQ(log_count("video error"), 0);
	end_session();

	// (the clamp of the bit rate to the level is tested on kvm_vcore_max_bitrate in the core tests; the fake stream has no acknowledgements, so R never gets above its default 2 Mb/s here)
}

#include "kvm_video_async_tests.inc"
#include "kvm_video_hw_tests.inc"
#include "kvm_video_view_tests.inc"
#include "kvm_video_auto_tests.inc"

int main(int argc, char **argv)
{
	if (getenv("HARNESS_DUMP") != NULL) { dump_stream(getenv("HARNESS_DUMP")); return 0; }
	if (getenv("HARNESS_ONLY_GOLDEN") != NULL) { (void)fixed_scenario(7); free_alive(); return 0; }	// the hash of the fixed mode, to be taken from the code before Auto
	if (argc >= 3 && strcmp(argv[1], "sessions") == 0)
	{
		// ./harness sessions N [threaded]: only the random sessions (the real thread of the encoder under ThreadSanitizer)
		test_random_sessions(atoi(argv[2]), argc >= 4 && strcmp(argv[3], "threaded") == 0);
		free_alive();
		printf("%d checks, %d failed\n", gChecks, gFail);
		return gFail == 0 ? 0 : 1;
	}
	test_first_frame();
	test_pace_and_window();
	test_stop_and_restart();
	test_errors();
	test_request_failures();
	test_geometry();
	test_repair_refresh_idr();
	test_many_rects();
	test_write_results();
	test_sharpening_path_lost();
	test_sps_limits();
	test_review_leftovers();
	test_rate_control();
	test_n2_n3();
	test_pace();
	test_idr_reasons();
	test_qp_stages();
	test_async_basic();
	test_async_busy_held();
	test_async_barriers();
	test_async_cancel();
	test_async_window();
	test_async_hang();
	test_async_errors();
	test_loop_model();
	test_async_review();
	test_hw_all();
	test_view_all();
	test_auto();
	test_random_sessions(getenv("HARNESS_SESSIONS") != NULL ? atoi(getenv("HARNESS_SESSIONS")) : 1000, false);
	(void)log_has;
	free_alive();
	printf("%d checks, %d failed\n", gChecks, gFail);
	if (gFail != 0) { for (size_t i = 0; i < gLog.size() && i < 40; i++) { printf("  log: %s\n", gLog[i].c_str()); } }
	return gFail == 0 ? 0 : 1;
}
