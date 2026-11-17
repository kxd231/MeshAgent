/* Tests of kvm_hwtest_core.c: the checks of the self test of the hardware H.264 encoder, with a FAKE encoder (a kvm_encbe table) that breaks one thing at a time: the SPS, the colour, the QP, the key
   frame, the 1:1 rule, the time, the open. Access units are built with a bit writer, so every field is known. Run with run-kvm-hwtest-core-tests.sh (gcc in a container). */
#define _POSIX_C_SOURCE 200809L
#include "../kvm_hwtest_core.h"
#include "../../common/kvm_h264_rules.h"
#include "../../common/kvm_synth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gFail = 0, gChecks = 0;
#define CHECK(c) do { gChecks++; if (!(c)) { gFail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* ---- bit writer and access units ---- */
struct bw { uint8_t b[512]; size_t n; int bit; };
static void wbit(struct bw *w, unsigned v) { if (w->bit == 0) { w->b[w->n++] = 0; } if (v) { w->b[w->n - 1] |= (uint8_t)(0x80u >> w->bit); } w->bit = (w->bit + 1) & 7; }
static void wbits(struct bw *w, unsigned v, int n) { while (n-- > 0) { wbit(w, (v >> n) & 1u); } }
static void wue(struct bw *w, unsigned v) { unsigned x = v + 1; int len = 0; while ((x >> len) > 1) { len++; } wbits(w, 0, len); wbits(w, x, len + 1); }
static size_t put_nal(uint8_t *o, size_t n, int refIdc, int type, const uint8_t *body, size_t bn)
{
	size_t i;
	o[n++] = 0; o[n++] = 0; o[n++] = 0; o[n++] = 1; o[n++] = (uint8_t)((refIdc << 5) | type);
	for (i = 0; i < bn; i++) { if (n >= 3 && o[n - 1] == 0 && o[n - 2] == 0 && body[i] <= 3) { o[n++] = 3; } o[n++] = body[i]; }
	return n;
}

/* what the fake does wrong */
struct knobs
{
	int level, refs, crop, bSlice, vui, colour;		/* SPS: level_idc; max_num_ref_frames; cropping; a B slice in every P; vui 0 none / 2 restriction; colour 0 none, 1 BT.709 limited, 2 0/0/2 (AMD), 3 full range, 4 BT.601 */
	int mbsW, mbsH;									/* 0 = the size of the picture */
	int formMask;									/* QP forms that set_qp accepts: bit 1 UI4, bit 3 FrameTypeQP, bit 4 Quality */
	int openRefuseForm;								/* the open itself refuses this form with E_ARG (0 = none) */
	int openRc;										/* the open fails with this code */
	int openUs;										/* time that the open takes, microseconds */
	int inflight;									/* max_inflight in the info */
	int qpEffective;								/* 0: the size does not depend on the QP; 1: 1.12 per QP step (26 / 40 = 4.8); 2: such that the ratio is 1.5; 3: 1.3 */
	int idrLateForced, idrUnasked, firstNotIdr;		/* the key frame that was asked for comes a frame late; an IDR in the 8th picture that nobody asked for; the first picture is no IDR */
	int encodeRcAt, encodeRc;						/* the picture with this index (0 based) fails with this code (-1 = never) */
	int emptyAt;									/* the picture gives 3 bytes */
	int idrBytes;									/* size of the key frame */
	int setQpRc;									/* set_qp fails with this code (0 = never) */
	int qpLag;										/* the QP that is set reaches the encoder one picture late */
	int errOutput;									/* a picture that fails still leaves a key frame in the output (a defect of the encoder: the size of it is not to be trusted) */
	int forceFailAt, forceRc;						/* the Nth call of force_idr (1 based, 0 = never) fails with this code */
	int rateEffective;								/* 1 = the size of a P picture follows the bit rate that set_bitrate gave (an encoder that ignores the QP: NVIDIA) */
	int qpStatic;									/* the QP is taken at the open (the first set_qp) and a change in the running stream is accepted and ignored (NVIDIA in the order B) */
	int gopStuck;									/* the GOP reads 30 in the order A: 1 = the order B (a second encoder inside the open) holds it, 2 = it does not either (the open fails: GOP periodic) */
	int rateLag;									/* a new bit rate reaches the sizes of the pictures this many pictures late */
	int openRefuseRc;								/* the open refuses this rate control mode (MFH264_RC_*) with E_CODECAPI (0 = none) */
	int coarseUs;									/* added to the time of a P picture while the clock has the default resolution (the 15.6 ms tick rounds the waits of the encoder up) */
	long encodeHr;									/* the HRESULT that the failed picture reports (0 = none) */
	int scChanges;									/* the stream changes that the info reports (the counters of a hardware encoder) */
	int latencyUs, slowEvery, slowUs, idrUs;		/* time of a P picture; every Nth picture takes slowUs; the key frame takes idrUs */
};
static struct knobs K;
static double gNow;
static int gClockFine, gClockCalls, gClockDefaultCalls, gClockFirst = -1;
static void fake_clock(int fine) { if (gClockFirst < 0) { gClockFirst = fine; } gClockCalls++; if (!fine) { gClockDefaultCalls++; } gClockFine = fine; }
static double now_ms(void) { return gNow; }

static int gOpenOrders[16], gNOrders, gGopB, gQpCalls;
static int gSetBitrateCalls, gLastBitrate, gLastRc, gMaxBitrate, gBrHist[512], gBrN, gVbvOpens, gVbvSet, gOpenLog[16];
static int gOpened, gClosed, gForce, gSetQp[64], gNSetQp, gPic, gLastQpForm, gLastQp, gForcePending, gPrevQp, gLagLeft;

static void knobs_default(void)
{
	memset(&K, 0, sizeof(K));
	K.level = 40; K.refs = 1; K.vui = 2; K.colour = 2; K.formMask = (1 << 1) | (1 << 3) | (1 << 4); K.inflight = 1; K.qpEffective = 1; K.encodeRcAt = -1; K.emptyAt = -1; K.idrBytes = 300000; K.latencyUs = 8000; K.idrUs = 25000; K.openUs = 160000;
	gNOrders = gGopB = gQpCalls = 0; gSetBitrateCalls = gLastBitrate = gLastRc = gMaxBitrate = gBrN = gVbvOpens = gVbvSet = 0; gOpened = gClosed = gForce = gNSetQp = gPic = gLastQpForm = gLastQp = gForcePending = gPrevQp = gLagLeft = 0;
	gNow = 1000.0;
}

static int g_w = 1920, g_h = 1088;
static FILE *gDiag;

static size_t make_sps(uint8_t *o, size_t n)
{
	struct bw w;
	uint8_t body[300];
	memset(&w, 0, sizeof(w));
	wbits(&w, 100, 8); wbits(&w, 0, 8); wbits(&w, (unsigned)K.level, 8); wue(&w, 0);
	wue(&w, 1); wue(&w, 0); wue(&w, 0); wbit(&w, 0); wbit(&w, 0);							/* chroma 4:2:0, depth 8, no scaling matrix */
	wue(&w, 4); wue(&w, 2); wue(&w, (unsigned)K.refs); wbit(&w, 0);
	wue(&w, (unsigned)((K.mbsW ? K.mbsW : g_w / 16) - 1)); wue(&w, (unsigned)((K.mbsH ? K.mbsH : g_h / 16) - 1));
	wbit(&w, 1); wbit(&w, 1);
	wbit(&w, (unsigned)K.crop); if (K.crop) { wue(&w, 0); wue(&w, 0); wue(&w, 0); wue(&w, 4); }
	if (K.vui == 0) { wbit(&w, 0); }
	else
	{
		wbit(&w, 1); wbit(&w, 0); wbit(&w, 0);												/* aspect_ratio, overscan */
		if (K.colour == 0) { wbit(&w, 0); }
		else
		{
			unsigned pr = 1, tr = 1, mx = 1, full = 0;
			if (K.colour == 2) { pr = 0; tr = 0; mx = 2; } else if (K.colour == 3) { full = 1; } else if (K.colour == 4) { pr = tr = mx = 6; }
			wbit(&w, 1); wbits(&w, 5, 3); wbit(&w, full); wbit(&w, 1); wbits(&w, pr, 8); wbits(&w, tr, 8); wbits(&w, mx, 8);
		}
		wbit(&w, 0);																			/* chroma_loc */
		wbit(&w, 1); wbits(&w, 1001, 32); wbits(&w, 60000, 32); wbit(&w, 1);
		wbit(&w, 0); wbit(&w, 0); wbit(&w, 0);
		wbit(&w, 1); wbit(&w, 1); wue(&w, 2); wue(&w, 1); wue(&w, 16); wue(&w, 16); wue(&w, 0); wue(&w, 1);
	}
	wbit(&w, 1);
	memcpy(body, w.b, w.n);
	return put_nal(o, n, 3, 7, body, w.n);
}
static size_t make_pps(uint8_t *o, size_t n)
{
	struct bw w;
	memset(&w, 0, sizeof(w));
	wue(&w, 0); wue(&w, 0); wbit(&w, 1); wbit(&w, 0); wue(&w, 0); wue(&w, 0); wue(&w, 0); wbit(&w, 0); wbits(&w, 0, 2); wue(&w, 0); wue(&w, 0); wue(&w, 0); wbit(&w, 1); wbit(&w, 0); wbit(&w, 0); wbit(&w, 1);
	return put_nal(o, n, 3, 8, w.b, w.n);
}
static size_t make_slice(uint8_t *o, size_t n, int idr, unsigned sliceType, int filler)
{
	struct bw w;
	uint8_t *body = (uint8_t *)malloc((size_t)filler + 64);
	int i;
	memset(&w, 0, sizeof(w));
	wue(&w, 0); wue(&w, sliceType); wue(&w, 0); wbits(&w, 0, 4);
	memcpy(body, w.b, w.n);
	for (i = 0; i < filler; i++) { body[w.n + (size_t)i] = (uint8_t)(0x80 + (i & 0x3F)); }
	n = put_nal(o, n, idr ? 3 : 2, idr ? 5 : 1, body, w.n + (size_t)filler);
	free(body);
	return n;
}

static int pbytes(int qp)
{
	double b = 20000.0;
	int k;
	if (K.qpEffective == 0) { return 6000; }
	if (K.qpEffective == 4) { return qp <= 26 ? 14000 - 7 : 10000 - 7; }			/* exactly 1.4 (the 7 bytes: start code, NAL header, slice header) */
	if (K.qpEffective == 6) { return qp <= 26 ? 14200 - 7 : 10000 - 7; }			/* 1.42 only at QP 26: the picture before it (QP 30 of the lag) is 1.0 */
	if (K.qpEffective == 5) { return qp <= 26 ? 14010 - 7 : 10000 - 7; }			/* a little above */
	{
		const double step = K.qpEffective == 1 ? 1.12 : (K.qpEffective == 2 ? 1.0295 : 1.0189);	/* (1.0295)^14 = 1.5, (1.0189)^14 = 1.3 */
		for (k = 0; k < 40 - qp; k++) { b *= step; }
		for (k = 0; k < qp - 40; k++) { b /= step; }
	}
	b /= (K.qpEffective == 1 ? 8.0 : 1.0);
	return (int)b < 200 ? 200 : (int)b;
}

/* ---- the fake encoder ---- */
static struct mfh264 *f_open(const struct mfh264_cfg *cfg, struct mfh264_err *err)
{
	memset(err, 0, sizeof(*err));
	if (K.openRc != 0) { err->code = K.openRc; err->stage = "fake open"; return NULL; }
	if (K.openRefuseForm != 0 && cfg->qp_method == K.openRefuseForm) { err->code = MFH264_E_ARG; err->stage = "fake: form"; return NULL; }
	if (K.openRefuseRc != 0 && cfg->rc == K.openRefuseRc) { err->code = MFH264_E_CODECAPI; err->stage = "fake: rate control mode"; return NULL; }
	if (gNOrders < 16) { gOpenOrders[gNOrders] = cfg->order; } gNOrders++;
	if (K.gopStuck != 0 && cfg->order == 0)
	{
		gGopB++;
		if (K.gopStuck == 2) { err->code = MFH264_E_CODECAPI; err->stage = "GOP periodic"; return NULL; }
	}
	gQpCalls = 0;
	gLastQpForm = cfg->qp_method; gLastRc = cfg->rc; gLastBitrate = cfg->bitrate; gBrN = 0;
	if (gVbvOpens < 16) { gOpenLog[gVbvOpens] = cfg->vbv; } gVbvOpens++; if (cfg->vbv != 0) { gVbvSet++; }
	gNow += (double)K.openUs / 1000.0;
	gOpened++; gPic = 0;
	return (struct mfh264 *)&gLastQpForm;
}
static void f_close(struct mfh264 *h) { (void)h; gClosed++; }
static int f_set_qp(struct mfh264 *h, int qp, struct mfh264_err *err)
{
	(void)h; memset(err, 0, sizeof(*err));
	if (K.setQpRc != 0) { err->code = K.setQpRc; err->stage = "fake set qp"; return K.setQpRc; }
	if (!(K.formMask & (1 << gLastQpForm))) { err->code = MFH264_E_CODECAPI; err->stage = "fake set qp"; return MFH264_E_CODECAPI; }
	if (gNSetQp < 64) { gSetQp[gNSetQp++] = qp; }
	if (K.qpStatic && gQpCalls++ >= 1) { return 0; }									/* accepted, ignored */
	if (K.qpLag && qp != gLastQp) { gPrevQp = gLastQp; gLagLeft = 1; }
	gLastQp = qp;
	return 0;
}
static int f_force(struct mfh264 *h, struct mfh264_err *err)
{
	(void)h; memset(err, 0, sizeof(*err)); gForce++;
	if (K.forceFailAt == gForce) { err->code = K.forceRc; err->stage = "fake force"; return K.forceRc; }
	gForcePending = 1;
	return 0;
}
static int gBgraCalls, gNv12Calls, gNv12Bad, gNv12FlatCalls;
static int f_encode(struct mfh264 *h, const uint8_t *bgra, int w, int hh, int stride, int64_t ts, uint8_t *out, size_t cap, size_t *len, int *idr, struct mfh264_err *err)
{
	size_t n = 0;
	int isIdr, pic = gPic++;
	(void)h; (void)w; (void)hh; (void)stride; (void)ts; (void)cap;
	if (bgra != NULL) { gBgraCalls++; }
	if (gBrN < 512) { gBrHist[gBrN++] = gLastBitrate; }
	memset(err, 0, sizeof(*err));
	*len = 0; *idr = 0;
	if (K.encodeRcAt == pic) { if (K.errOutput) { n = make_sps(out, 0); n = make_pps(out, n); n = make_slice(out, n, 1, 7, K.idrBytes); *len = n; *idr = 1; } err->code = K.encodeRc; err->hr = K.encodeHr; err->stage = "fake encode"; return K.encodeRc; }
	isIdr = (pic == 0 && !K.firstNotIdr) || (gForcePending && !K.idrLateForced) || (K.idrLateForced && pic > 0 && gForcePending == 2) || (K.idrUnasked && pic == 7);
	if (gForcePending == 1 && K.idrLateForced) { gForcePending = 2; }
	else if (isIdr && gForcePending) { gForcePending = 0; }
	if (isIdr)
	{
		n = make_sps(out, 0); n = make_pps(out, n); n = make_slice(out, n, 1, 7, K.idrBytes);
		gNow += (double)(K.idrUs) / 1000.0;
	}
	else
	{
		{ int q = gLastQp; if (gLagLeft > 0 && K.qpLag) { q = gPrevQp; gLagLeft--; } n = make_slice(out, 0, 0, K.bSlice ? 6u : 5u, (K.rateEffective && gLastRc != MFH264_RC_QUALITY) ? gBrHist[gBrN - 1 - ((K.rateLag < gBrN - 1) ? K.rateLag : gBrN - 1)] / 8 / 30 : pbytes(q)); }
		gNow += (double)(((K.slowEvery > 0 && pic % K.slowEvery == 0) ? K.slowUs : K.latencyUs) + (gClockFine ? 0 : K.coarseUs)) / 1000.0;
	}
	if (K.emptyAt == pic) { n = 3; }
	*len = n; *idr = isIdr;
	return 0;
}
/* the view: the same encoder, the NV12 picture of the coded size (the test checks its size and that it is a converted picture, not zeros) */
static int f_encode_nv12(struct mfh264 *h, const uint8_t *nv12, size_t len, int64_t ts, uint8_t *out, size_t cap, size_t *olen, int *idr, struct mfh264_err *err)
{
	const size_t need = (size_t)g_w * (size_t)g_h * 3u / 2u;
	size_t i;
	unsigned lo = 255, hi = 0;
	gNv12Calls++;
	if (len != need) { gNv12Bad++; }
	for (i = 0; i < (size_t)g_w * (size_t)g_h; i += 97) { if (nv12[i] < lo) { lo = nv12[i]; } if (nv12[i] > hi) { hi = nv12[i]; } }
	if (gPic >= KVM_HWT_FRAMES) { gNv12FlatCalls++; if (lo != hi) { gNv12Bad++; } }		/* the flat picture is one grey in the plane of luma */
	else if (gPic == 0 && hi - lo < 40) { gNv12Bad++; }									/* the first picture is text: it has black and white in it */
	return f_encode(h, NULL, g_w, g_h, 0, ts, out, cap, olen, idr, err);
}
static void f_info(const struct mfh264 *h, struct mfh264_info *info) { (void)h; memset(info, 0, sizeof(*info)); info->max_inflight = K.inflight; info->backend = 1; info->stream_changes = (unsigned)K.scChanges; info->sc_event = (unsigned)K.scChanges; info->sc_wait_ms_last = info->sc_wait_ms_max = K.scChanges ? 12u : 0u; info->open_have = 1; info->sc_unasked = 0; info->sc_need_wait = (unsigned)K.scChanges; info->sc_set_hr = 0; info->sc_type_idx = K.scChanges ? 1 : 0; info->sc_type_n = K.scChanges ? 2u : 0u; info->sc_subtype = K.scChanges ? 0x34363248u : 0u; info->async = 1; snprintf(info->gop_trace, sizeof(info->gop_trace), "gop order=%c try=7fffffff:00000000:2147483647 rc set=0 read=-1", gGopB ? 'B' : 'A'); }
static int f_init(struct mfh264_err *e) { (void)e; return 0; }
static void f_shutdown(void) {}
static int f_faulted(void) { return 0; }
static int f_set_bitrate(struct mfh264 *h, int b, struct mfh264_err *e) { (void)h; (void)e; gSetBitrateCalls++; gLastBitrate = b; if (b > gMaxBitrate) { gMaxBitrate = b; } return 0; }
static int f_set_sample_qp(struct mfh264 *h, int m, int q) { (void)h; (void)m; (void)q; return 0; }
static int f_last_qp(const struct mfh264 *h, uint64_t *r) { (void)h; if (r) { *r = 0; } return 0; }
static size_t f_max_au(const struct mfh264 *h) { (void)h; return 1u << 22; }
static size_t f_seq(const struct mfh264 *h, const uint8_t **p) { (void)h; if (p) { *p = NULL; } return 0; }

static const kvm_encbe FAKE = { "fake", 0, f_init, f_shutdown, f_faulted, f_open, f_close, f_set_qp, f_set_bitrate, f_set_sample_qp, f_last_qp, f_force, f_encode, f_encode_nv12, f_info, f_max_au, f_seq };
static const kvm_encbe FAKE_BGRA = { "fake-bgra", 0, f_init, f_shutdown, f_faulted, f_open, f_close, f_set_qp, f_set_bitrate, f_set_sample_qp, f_last_qp, f_force, f_encode, NULL, f_info, f_max_au, f_seq };
static const kvm_encbe *gBe = &FAKE;

static int run(kvm_hwp_result *res)
{
	kvm_hwt_cfg c;
	memset(&c, 0, sizeof(c));
	c.codedW = g_w; c.codedH = g_h; c.level = 42; c.maxLevel = 0; c.profile = MFH264_PROFILE_HIGH; c.rev = 7; c.key = "rev:7,test"; c.nowMs = now_ms; c.clock = fake_clock; c.diag = gDiag;
	gClockFine = gClockCalls = gClockDefaultCalls = 0; gClockFirst = -1;
	gBgraCalls = gNv12Calls = gNv12Bad = gNv12FlatCalls = 0;
	return kvm_hwt_run(gBe, &c, res);
}

#define EXPECT(wv, wr) do { kvm_hwp_result rr; const int vv = run(&rr); CHECK(vv == (wv)); CHECK(strcmp(rr.rule, (wr)) == 0); CHECK(rr.valid == 1 && rr.verdict == vv); CHECK(gOpened == gClosed); if (vv != (wv) || strcmp(rr.rule, (wr)) != 0) { printf("   got verdict %d rule \"%s\" (wanted %d \"%s\")\n", vv, rr.rule, (int)(wv), (wr)); } } while (0)
#define P KVM_HWP_V_PASS
#define F KVM_HWP_V_FAIL
#define S KVM_HWP_V_SLOW

int main(void)
{
	kvm_hwp_result r;
	int i;
	/* the good encoder, with what the AMD encoder writes (colour 0 / 0 / 2) */
	knobs_default();
	CHECK(run(&r) == P && r.rule[0] == 0 && r.valid && r.rev == 7 && strcmp(r.key, "rev:7,test") == 0 && r.qpForm == MFH264_QP_ENCODEQP_UI4 && r.frames == KVM_HWT_FRAMES);
	CHECK(r.openMs == 160 && r.p50Us == 8000 && r.p95Us == 8000 && r.idrKb == 294 && gOpened == 1 && gClosed == 1);
	/* what the test did with the encoder: the QP of the frames, one key frame asked for */
	CHECK(gForce == 2);						/* the key frame of the plan, and the flat one that measures the floor of a key frame */
	CHECK(r.flatMilli > 0 && r.flatMilli < 2000000u);
	CHECK(gNSetQp >= 5 && gSetQp[0] == 30);
	{ int has26 = 0, has40 = 0; for (i = 0; i < gNSetQp; i++) { has26 |= gSetQp[i] == 26; has40 |= gSetQp[i] == 40; } CHECK(has26 && has40); }
	/* the ways into the encoder: the view for all but the last two pictures of the plan and for the flat one, the BGRA picture for the two */
	CHECK(gNv12Calls == KVM_HWT_FRAMES - 2 + 1 && gBgraCalls == 2 && gNv12Bad == 0 && gNv12FlatCalls == 1);
	gBe = &FAKE_BGRA; knobs_default(); CHECK(run(&r) == P && gNv12Calls == 0 && gBgraCalls == KVM_HWT_FRAMES + 1 && r.flatMilli > 0); gBe = &FAKE;	/* a backend without the view (software, the old table) */
	knobs_default(); K.colour = 0; EXPECT(P, "");			/* no colour description */
	knobs_default(); K.colour = 1; EXPECT(P, "");
	knobs_default(); K.vui = 0; K.colour = 0; EXPECT(P, "");	/* no VUI: a warning of the rules, not a failure */
	/* the QP forms */
	knobs_default(); K.formMask = (1 << 3) | (1 << 4); CHECK(run(&r) == P && r.qpForm == MFH264_QP_FRAMETYPEQP && gOpened == 2 && gClosed == 2);
	knobs_default(); K.formMask = (1 << 4); CHECK(run(&r) == P && r.qpForm == MFH264_QP_QUALITY && gOpened == 3 && gClosed == 3);
	knobs_default(); K.openRefuseForm = MFH264_QP_ENCODEQP_UI4; CHECK(run(&r) == P && r.qpForm == MFH264_QP_FRAMETYPEQP && gOpened == gClosed);
	knobs_default(); K.formMask = 0; EXPECT(F, "rate");
	knobs_default(); K.openRefuseForm = -1; K.formMask = 0; EXPECT(F, "rate");
	/* the open */
	knobs_default(); K.openRc = MFH264_E_CREATE; EXPECT(F, "open5");
	knobs_default(); K.openRc = MFH264_E_HANG; EXPECT(F, "open20");
	knobs_default(); K.setQpRc = MFH264_E_HANG; EXPECT(F, "setqp20");
	knobs_default(); K.setQpRc = MFH264_E_EXCEPTION; EXPECT(F, "setqp17");
	knobs_default(); K.openRc = MFH264_E_EXCEPTION; EXPECT(F, "open17");
	knobs_default(); K.inflight = 2; EXPECT(F, "inflight");
	knobs_default(); K.inflight = 0; EXPECT(P, "");			/* an encoder that gives the unit at once says 0 */
	knobs_default(); K.openUs = 2500000; EXPECT(S, "open_slow");
	knobs_default(); K.openUs = 1900000; EXPECT(P, "");
	/* the QP */
	knobs_default(); K.qpEffective = 0; EXPECT(F, "rate");
	knobs_default(); K.qpEffective = 3; EXPECT(F, "rate");		/* 1.3: below 1.4 */
	knobs_default(); K.qpEffective = 2; EXPECT(P, "");		/* 1.5 */
	knobs_default(); K.qpEffective = 4; EXPECT(F, "rate");		/* exactly 1.4: not above it */
	knobs_default(); K.qpEffective = 5; EXPECT(P, "");
	/* an encoder that takes the QP and ignores it (NVIDIA) is tried again with the bit rate; the AMD encoder never gets to the second pass */
	knobs_default(); CHECK(run(&r) == P && r.qpForm == MFH264_QP_ENCODEQP_UI4 && gSetBitrateCalls == 0 && gOpened == 1);	/* AMD: no set_bitrate at all, the same result as before */
	knobs_default(); K.qpEffective = 0; K.rateEffective = 1; CHECK(run(&r) == P && r.rule[0] == 0 && r.qpForm == KVM_HWP_FORM_BR_CBR && gLastRc == MFH264_RC_CBR && gSetBitrateCalls >= 3 && gOpened == 2 && gClosed == 2);
	CHECK(gLastBitrate == 4000000 && gMaxBitrate == 8000000);							/* 26: 8 Mb/s, 30: 4, 40: 1 at 1920x1088 */
	knobs_default(); K.qpEffective = 0; K.rateEffective = 1; g_w = 960; g_h = 544; CHECK(run(&r) == P && gMaxBitrate == 2000000); g_w = 1920; g_h = 1088;	/* scaled by the area */
	{
		kvm_hwt_cfg c; knobs_default(); K.qpEffective = 0; K.rateEffective = 1; g_w = 3840; g_h = 2160; memset(&c, 0, sizeof(c));
		c.codedW = g_w; c.codedH = g_h; c.level = 40; c.profile = MFH264_PROFILE_HIGH; c.nowMs = now_ms;
		kvm_hwt_run(&FAKE, &c, &r); CHECK(gMaxBitrate == 25000000);						/* never above what the level allows (8 Mb/s x 3.97 = 31.7 Mb/s, level 4.0: 25) */
		g_w = 1920; g_h = 1088;
	}
	/* the hardware self test (the NVIDIA run): the bit rate reaches the sizes of the pictures a few pictures late: the first three pictures of a window are not measured; no BufferSize in the passes */
	knobs_default(); K.qpEffective = 0; K.rateEffective = 1; K.rateLag = 3; CHECK(run(&r) == P && r.qpForm == KVM_HWP_FORM_BR_CBR && gVbvSet == 0);
	knobs_default(); K.qpEffective = 0; K.rateEffective = 1; K.rateLag = 12; EXPECT(F, "rate");
	knobs_default(); K.qpEffective = 0; K.rateEffective = 1; CHECK(run(&r) == P && r.frames == 46 && gVbvSet == 0);
	{
		/* the diagnostics: the sizes of every picture of both passes (| a change, i a key frame) and the line of the open */
		char buf[8192]; size_t n;
		gDiag = tmpfile(); knobs_default(); K.qpEffective = 0; K.rateEffective = 1; CHECK(run(&r) == P);
		rewind(gDiag); n = fread(buf, 1, sizeof(buf) - 1, gDiag); buf[n] = 0;
		CHECK(strstr(buf, "dq=") != NULL && strstr(buf, "dr=") != NULL && strstr(buf, " |") != NULL && strstr(buf, "i ") != NULL && strstr(buf, "gop order=A") != NULL && strstr(buf, "G1") == NULL);
		fclose(gDiag); gDiag = NULL;
	}
	/* the QP that is taken at the open and ignored in the running stream: the pass "qp" fails (windows measured from the 4th picture after a change), the pass "rate" decides; the AMD encoder (the QP works at once or a picture late) stays on the form 1 */
	knobs_default(); K.qpStatic = 1; K.rateEffective = 1; CHECK(run(&r) == P && r.qpForm == KVM_HWP_FORM_BR_CBR && gSetBitrateCalls >= 3);
	knobs_default(); K.qpStatic = 1; EXPECT(F, "rate");
	knobs_default(); K.qpLag = 1; K.qpEffective = 2; CHECK(run(&r) == P && r.qpForm == MFH264_QP_ENCODEQP_UI4 && gSetBitrateCalls == 0 && gOpened == 1);
	knobs_default(); CHECK(run(&r) == P && r.qpForm == MFH264_QP_ENCODEQP_UI4 && gSetBitrateCalls == 0 && gOpened == 1 && r.frames == 46);
	/* the GOP that reads 30: the order B holds it (PASS), or neither order does (FAIL gop, no stream, no second pass); the AMD encoder opens once in the order A */
	knobs_default(); K.gopStuck = 1; CHECK(run(&r) == P && r.qpForm == MFH264_QP_ENCODEQP_UI4 && gGopB == 1);
	knobs_default(); K.gopStuck = 2; EXPECT(F, "gop"); CHECK(gSetBitrateCalls == 0 && gNSetQp == 0 && gOpened == 0);
	knobs_default(); CHECK(run(&r) == P && gGopB == 0 && gOpenOrders[0] == 0);
	{
		/* the diagnostics G1..G5: the orders and the only candidate are the ones of the variants; every one has the line of the open and its sizes */
		char buf[8192]; size_t n; kvm_hwt_cfg c;
		gDiag = tmpfile(); knobs_default(); memset(&c, 0, sizeof(c)); c.codedW = g_w; c.codedH = g_h; c.level = 42; c.profile = MFH264_PROFILE_HIGH; c.nowMs = now_ms; c.diag = gDiag;
		kvm_hwt_diag(&FAKE, &c);
		rewind(gDiag); n = fread(buf, 1, sizeof(buf) - 1, gDiag); buf[n] = 0;
		CHECK(strstr(buf, "G1 gop order=") != NULL && strstr(buf, "G2=") != NULL && strstr(buf, "G3=") != NULL && strstr(buf, "G4=") != NULL && strstr(buf, "G5=") != NULL && strstr(buf, " |") != NULL && n < 6000);
		CHECK(gNOrders == 5 && gOpenOrders[0] == 1 && gOpenOrders[1] == 2 && gOpenOrders[2] == 2 && gOpenOrders[3] == 2 && gOpenOrders[4] == 1 && gOpened == 5 && gClosed == 5);
		fclose(gDiag); gDiag = NULL;
	}
	knobs_default(); K.qpEffective = 0; EXPECT(F, "rate");									/* both ignored: the rule of the second pass */
	knobs_default(); K.qpEffective = 0; K.rateEffective = 1; K.openRefuseRc = MFH264_RC_CBR; CHECK(run(&r) == P && r.qpForm == KVM_HWP_FORM_BR_LDVBR && gLastRc == MFH264_RC_LDVBR && gOpened == 2 && gClosed == 2);
	knobs_default(); K.qpEffective = 0; K.rateEffective = 1; K.latencyUs = 30000; CHECK(run(&r) == S && strcmp(r.rule, "latency") == 0 && r.qpForm == KVM_HWP_FORM_BR_CBR);	/* the verdict is the one of the second pass */
	knobs_default(); K.qpEffective = 2; K.qpLag = 1; EXPECT(P, "");	/* the QP reaches the encoder a picture late: that picture is not measured */
	knobs_default(); K.qpEffective = 6; K.qpLag = 1; EXPECT(P, "");	/* ... and the one after the change to 26 is not measured either (it would pull the mean down) */
	/* the key frame */
	knobs_default(); K.idrLateForced = 1; EXPECT(F, "idr_late");
	knobs_default(); K.idrUnasked = 1; EXPECT(F, "idr_unasked");
	knobs_default(); K.firstNotIdr = 1; EXPECT(F, "idr_missing");
	knobs_default(); K.idrBytes = 2 * 1024 * 1024; EXPECT(F, "idr_size");
	knobs_default(); K.idrBytes = 1500 * 1024; EXPECT(P, "");
	/* 1:1 */
	knobs_default(); K.encodeRcAt = 6; K.encodeRc = MFH264_E_STREAM_BROKEN; EXPECT(F, "encode14");
	{
		/* the clock: the test asks for 1 ms before the first picture and never gives it back by itself; a coarse clock would slow the encoder down (the verdict is the one of 1 ms) */
		knobs_default(); K.coarseUs = 20000; CHECK(run(&r) == P && r.p50Us == 8000 && gClockFirst == 1 && gClockCalls == 1 && gClockDefaultCalls == 0 && gClockFine == 1);
	}
	{
		/* the diagnostics of the clock: once more at the default resolution, both results on one line; the verdict stays the one of 1 ms, the clock is back at 1 ms */
		char buf[8192]; size_t n; kvm_hwt_cfg c;
		kvm_hwp_result fine;
		gDiag = tmpfile(); knobs_default(); K.coarseUs = 20000;
		CHECK(run(&fine) == P);
		memset(&c, 0, sizeof(c)); c.codedW = g_w; c.codedH = g_h; c.level = 42; c.profile = MFH264_PROFILE_HIGH; c.nowMs = now_ms; c.clock = fake_clock; c.diag = gDiag;
		gClockCalls = gClockDefaultCalls = 0;
		kvm_hwt_diag_clock(&FAKE, &c, &fine);
		rewind(gDiag); n = fread(buf, 1, sizeof(buf) - 1, gDiag); buf[n] = 0;
		CHECK(strstr(buf, "lat default p50=28000 p95=28000 rule=latency | lat 1ms p50=8000 p95=8000 rule=-") != NULL);
		CHECK(gClockDefaultCalls == 1 && gClockCalls == 2 && gClockFine == 1);
		fclose(gDiag); gDiag = NULL;
		memset(&c, 0, sizeof(c)); c.diag = stderr;	/* without a clock callback nothing is measured twice */
		gClockCalls = 0; kvm_hwt_diag_clock(&FAKE, &c, &fine); CHECK(gClockCalls == 0);
	}
	{
		/* the diagnostics of a failed picture: the step, the picture, the HRESULT and the stage of the backend, then the counters of the stream changes */
		char buf[8192]; size_t n;
		gDiag = tmpfile(); knobs_default(); K.encodeRcAt = 6; K.encodeRc = MFH264_E_STREAM_BROKEN; K.encodeHr = (long)0x8000FFFFL; K.scChanges = 1; EXPECT(F, "encode14");
		rewind(gDiag); n = fread(buf, 1, sizeof(buf) - 1, gDiag); buf[n] = 0;
		CHECK(strstr(buf, "dq fail: step=encode frame=6 rc=-14 code=-14 hr=0x8000ffff stage=fake encode") != NULL);
		CHECK(strstr(buf, "dq info: async=1") != NULL && strstr(buf, "open_have=1 stream_changes=1 sc_unasked=0 sc_event=1 sc_no_event=0 sc_need_wait=1 sc_wait_ms last=12 max=12 sc_set_hr=0x00000000 sc_type=1/2 sc_subtype=0x34363248 out_fail=0") != NULL);
		fclose(gDiag); gDiag = NULL;
	}
	knobs_default(); K.encodeRcAt = 0; K.encodeRc = MFH264_E_INPUT; EXPECT(F, "encode12");
	knobs_default(); K.encodeRcAt = 9; K.encodeRc = MFH264_E_HANG; EXPECT(F, "hang");
	knobs_default(); K.emptyAt = 4; EXPECT(F, "empty");
	/* a flat key frame (the picture after the plan): a hang or an exception is a failure, any other error or no key frame only means that the floor is not known */
	knobs_default(); K.encodeRcAt = KVM_HWT_FRAMES; K.encodeRc = MFH264_E_HANG; EXPECT(F, "flat");
	knobs_default(); K.encodeRcAt = KVM_HWT_FRAMES; K.encodeRc = MFH264_E_EXCEPTION; EXPECT(F, "flat");
	knobs_default(); K.encodeRcAt = KVM_HWT_FRAMES; K.encodeRc = MFH264_E_STREAM_BROKEN; CHECK(run(&r) == P && r.flatMilli == 0 && r.rule[0] == 0);
	knobs_default(); K.encodeRcAt = KVM_HWT_FRAMES; K.encodeRc = MFH264_E_INPUT; CHECK(run(&r) == P && r.flatMilli == 0);
	knobs_default(); K.encodeRcAt = KVM_HWT_FRAMES; K.encodeRc = MFH264_E_INPUT; K.errOutput = 1; CHECK(run(&r) == P && r.flatMilli == 0);	/* the size of a picture that failed is not the floor */
	knobs_default(); K.encodeRcAt = KVM_HWT_FRAMES; K.encodeRc = MFH264_E_HANG; gBe = &FAKE_BGRA; EXPECT(F, "flat"); gBe = &FAKE;		/* by the BGRA way too */
	knobs_default(); K.forceFailAt = 2; K.forceRc = MFH264_E_HANG; EXPECT(F, "flat");
	knobs_default(); K.forceFailAt = 2; K.forceRc = MFH264_E_EXCEPTION; EXPECT(F, "flat");
	knobs_default(); K.forceFailAt = 2; K.forceRc = MFH264_E_CODECAPI; CHECK(run(&r) == P && r.flatMilli == 0);
	knobs_default(); K.forceFailAt = 1; K.forceRc = MFH264_E_CODECAPI; EXPECT(F, "force9");			/* the key frame of the plan: a failure of its own */
	/* a failure of the plan is told before the flat key frame is made, and a failure of the stream stays the first one */
	knobs_default(); K.encodeRcAt = 9; K.encodeRc = MFH264_E_HANG; EXPECT(F, "hang");
	/* the rules of the viewer */
	knobs_default(); K.level = 50; EXPECT(F, "level_over");
	knobs_default(); K.refs = 2; EXPECT(F, "refs");
	knobs_default(); K.crop = 1; EXPECT(F, "geometry");
	knobs_default(); K.mbsW = 100; EXPECT(F, "geometry");
	knobs_default(); K.bSlice = 1; EXPECT(F, "no_b");
	knobs_default(); K.colour = 3; EXPECT(F, "colour");		/* full range */
	knobs_default(); K.colour = 4; EXPECT(F, "colour");		/* BT.601 */
	{
		/* the viewer's limit of the level: the rule of the viewer says it (before the announced level is compared) */
		kvm_hwt_cfg c; knobs_default(); memset(&c, 0, sizeof(c)); c.codedW = g_w; c.codedH = g_h; c.level = 42; c.maxLevel = 31; c.profile = MFH264_PROFILE_HIGH; c.nowMs = now_ms;
		CHECK(kvm_hwt_run(&FAKE, &c, &r) == F && strcmp(r.rule, "level") == 0);
	}
	/* the time: too slow is not a failure, a failure beats it */
	knobs_default(); K.latencyUs = 30000; EXPECT(S, "latency");
	knobs_default(); K.latencyUs = 24000; EXPECT(P, "");
	knobs_default(); K.slowEvery = 5; K.slowUs = 70000; EXPECT(S, "latency");		/* 95th percentile */
	knobs_default(); K.slowEvery = 5; K.slowUs = 59000; EXPECT(P, "");
	knobs_default(); K.idrUs = 300000; EXPECT(S, "latency");
	knobs_default(); K.idrUs = 250000; EXPECT(P, "");
	knobs_default(); K.latencyUs = 30000; K.qpEffective = 0; EXPECT(F, "rate");
	knobs_default(); K.latencyUs = 30000; K.openUs = 2500000; EXPECT(S, "open_slow");	/* the first SLOW is the one that is told */
	knobs_default(); K.openUs = 2500000; K.refs = 2; EXPECT(F, "refs");
	/* another size, and a picture that is not a multiple of 16 is the caller's mistake: the synth refuses */
	knobs_default(); g_w = 1920; g_h = 1216; CHECK(run(&r) == P && r.frames == KVM_HWT_FRAMES);
	knobs_default(); g_w = 1000; g_h = 600; CHECK(run(&r) == F && strcmp(r.rule, "synth") == 0); g_w = 1920; g_h = 1088;
	printf("kvm_hwtest_core_test: %d checks, %d failed\n", gChecks, gFail);
	return gFail ? 1 : 0;
}
