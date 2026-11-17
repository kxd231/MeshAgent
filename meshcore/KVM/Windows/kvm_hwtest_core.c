/* KVM (Windows): the checks of the self test of the hardware H.264 encoder, see kvm_hwtest_core.h. */

#include "kvm_hwtest_core.h"
#include "../common/kvm_synth.h"
#include "../common/kvm_h264_rules.h"
#include "../common/kvm_yuv.h"
#include "../common/kvm_video_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { K_TEXT = 0, K_SCROLL = 1, K_VIDEO = 2 };

static void set_rule(kvm_hwp_result *res, int verdict, const char *rule)
{
	// a failure is never turned into a SLOW, and the first failure is the one that is told
	if (res->verdict == KVM_HWP_V_FAIL) { return; }
	if (verdict == KVM_HWP_V_SLOW && res->verdict == KVM_HWP_V_SLOW) { return; }
	res->verdict = verdict;
	snprintf(res->rule, sizeof(res->rule), "%s", rule);
}

static int cmp_uint(const void *a, const void *b) { const unsigned x = *(const unsigned *)a, y = *(const unsigned *)b; return x < y ? -1 : (x > y ? 1 : 0); }

// A frame to the encoder by the way that a session takes: the NV12 view that the thread of the encoder keeps (the NV12 view, the default: hw_encode_nv12 copies it into the input buffer) when the backend has it,
// else the BGRA picture. The conversion is the one of a complete view (kvm_yuv_bgra_to_nv12, as kvm_nv12v_apply does it) and is made before the clock of the frame starts: the time is the encoder's.
static int encode_frame(const kvm_encbe *be, struct mfh264 *h, int viaNv12, const uint8_t *bgra, uint8_t *nv12, int w, int hh, int64_t ts, uint8_t *out, size_t cap, size_t *len, int *idr, struct mfh264_err *err, double (*nowMs)(void), double *a, double *b)
{
	int rc;
	if (viaNv12)
	{
		if (kvm_yuv_bgra_to_nv12(bgra, w, hh, w * 4, nv12, w, hh, KVM_YUV_AUTO) != 0) { err->code = MFH264_E_ARG; err->stage = "the conversion of the test picture"; return MFH264_E_ARG; }
		*a = nowMs();
		rc = be->encode_nv12(h, nv12, (size_t)w * (size_t)hh * 3u / 2u, ts, out, cap, len, idr, err);
		*b = nowMs();
		return rc;
	}
	*a = nowMs();
	rc = be->encode_bgra(h, bgra, w, hh, w * 4, ts, out, cap, len, idr, err);
	*b = nowMs();
	return rc;
}

typedef struct frame_plan { int kind, t, qp, force, expectIdr, measure; } frame_plan;

// The plan of both passes (the plan of the probe that worked on NVIDIA, also for the QP; the "QP" of a picture is the QP of the pass "qp" and the bit rate that stands for it in the pass "rate"): a key frame and 14 pictures of scroll
// at the middle QP (the key frame is paid back: a CBR encoder starves the pictures behind a big key frame, and the small noisy P pictures right after a key frame say nothing of the QP), 12 at 26 and 12 at 40 (the change may take a few pictures
// to reach the encoder: the first three of a window are not measured), a key frame that is asked for, and video
#define KVM_HWT_MAXPLAN 64
static int make_plan(frame_plan *p)
{
	int n = 0, i;
	p[n++] = (frame_plan){ K_TEXT, 0, 30, 0, 1, 0 };
	for (i = 0; i < 14; i++) { p[n++] = (frame_plan){ K_SCROLL, i, 30, 0, 0, 0 }; }
	for (i = 0; i < 12; i++) { p[n++] = (frame_plan){ K_SCROLL, 14 + i, 26, 0, 0, i >= 3 ? 26 : 0 }; }
	for (i = 0; i < 12; i++) { p[n++] = (frame_plan){ K_SCROLL, 26 + i, 40, 0, 0, i >= 3 ? 40 : 0 }; }
	p[n++] = (frame_plan){ K_SCROLL, 38, 30, 1, 1, 0 };
	for (i = 0; i < 6; i++) { p[n++] = (frame_plan){ K_VIDEO, i, 30, 0, 0, 0 }; }
	return n;
}

// the hardware self test diagnostics (stderr of the test process, never the result on stdout): the size of every picture of a pass, i = a key frame, | = the setting was changed before this picture
static void diag_sizes(FILE *f, const char *tag, const unsigned *sz, const unsigned char *fl, int n)
{
	int i;
	if (f == NULL) { return; }
	fprintf(f, "%s=", tag);
	for (i = 0; i < n; i++) { fprintf(f, "%s%u%s", (fl[i] & 2) ? " |" : "", sz[i], (fl[i] & 1) ? "i " : " "); }
	fprintf(f, "\n");
	fflush(f);
}

// what failed, when a call of the encoder fails (stderr of the test process; the rule of the result only says the code): the step, the result, the HRESULT and the stage that the backend names
static void diag_fail(FILE *f, const char *tag, const char *step, int frame, int rc, const struct mfh264_err *err)
{
	if (f == NULL) { return; }
	fprintf(f, "%s fail: step=%s frame=%d rc=%d code=%d hr=0x%08lx stage=%s reason=%d\n", tag, step, frame, rc, err->code, (unsigned long)(unsigned)err->hr, err->stage != NULL ? err->stage : "-", err->reason);
	fflush(f);
}

// the counters of the encoder that explain a failure of a hardware encoder: the events, the stream changes (and whether a new HaveOutput came after each, after how many ms), the failed ProcessOutput calls
static void diag_info(FILE *f, const char *tag, const kvm_encbe *be, struct mfh264 *h)
{
	struct mfh264_info in;
	if (f == NULL || h == NULL || be->get_info == NULL) { return; }
	memset(&in, 0, sizeof(in));
	be->get_info(h, &in);
	fprintf(f, "%s info: async=%d provides=%d events need=%u have=%u other=%u flushes=%u open_have=%u stream_changes=%u sc_unasked=%u sc_event=%u sc_no_event=%u sc_need_wait=%u sc_wait_ms last=%u max=%u sc_set_hr=0x%08lx sc_type=%d/%u sc_subtype=0x%08x out_fail=%u out_fail_hr=0x%08lx\n", tag, in.async, in.out_provides_samples, in.events_need, in.events_have, in.events_other, in.flushes, in.open_have, in.stream_changes, in.sc_unasked, in.sc_event, in.sc_no_event, in.sc_need_wait, in.sc_wait_ms_last, in.sc_wait_ms_max, (unsigned long)(unsigned)in.sc_set_hr, in.sc_type_idx, in.sc_type_n, in.sc_subtype, in.out_fail, (unsigned long)(unsigned)in.out_fail_hr);
	fflush(f);
}

// the bit rate that stands for a "QP" of the plan in the pass "rate" (26: 8 Mb/s, 30: 4, 40: 1 at 1920x1088; scaled by the area, never above what the level allows)
static int plan_bps(const kvm_hwt_cfg *c, int qp, int profile)
{
	unsigned long long b = qp <= 26 ? 8000000ull : (qp <= 30 ? 4000000ull : 1000000ull);
	const uint32_t mx = kvm_vcore_max_bitrate(c->level, profile);
	b = b * (unsigned long long)c->codedW * (unsigned long long)c->codedH / (1920ull * 1088ull);
	return (int)(b > mx ? mx : b);
}

// One pass of the test. rate = 0: the QP steers the encoder (the forms of the QP, the rule "qp"); rate = 1: the bit rate does (CBR, or LowDelayVBR when the open refuses CBR; the rule "rate", the form 8 or 9)
static int run_pass(const kvm_encbe *be, const kvm_hwt_cfg *c, kvm_hwp_result *res, int rate)
{
	static const int forms[3] = { MFH264_QP_ENCODEQP_UI4, MFH264_QP_FRAMETYPEQP, MFH264_QP_QUALITY };
	kvm_synth syn;
	struct mfh264 *h = NULL;
	struct mfh264_err err;
	struct mfh264_info info;
	struct kvmh_check chk;
	frame_plan plan[KVM_HWT_MAXPLAN];
	unsigned usP[KVM_HWT_MAXPLAN];
	unsigned dsz[KVM_HWT_MAXPLAN];
	unsigned char dfl[KVM_HWT_MAXPLAN];
	int nD = 0;
	unsigned long sumA = 0, sumB = 0;
	unsigned nA = 0, nB = 0, nP = 0, idrMaxUs = 0;
	uint8_t *bgra = NULL, *out = NULL, *nv12 = NULL;
	const size_t outCap = 8u * 1024u * 1024u;
	int form = 0, nPlan, i, k, fi, usedForm = 0;
	double t0;

	memset(res, 0, sizeof(*res));
	res->rev = c->rev; res->verdict = KVM_HWP_V_PASS;
	if (c->key != NULL) { snprintf(res->key, sizeof(res->key), "%s", c->key); }
	if (kvm_synth_init(&syn, c->codedW, c->codedH) != 0) { set_rule(res, KVM_HWP_V_FAIL, "synth"); return res->verdict; }
	bgra = (uint8_t *)malloc((size_t)c->codedW * (size_t)c->codedH * 4u);
	out = (uint8_t *)malloc(outCap);
	if (be->encode_nv12 != NULL) { nv12 = (uint8_t *)malloc((size_t)c->codedW * (size_t)c->codedH * 3u / 2u); }
	if (bgra == NULL || out == NULL || (be->encode_nv12 != NULL && nv12 == NULL)) { set_rule(res, KVM_HWP_V_FAIL, "memory"); goto done; }
	nPlan = make_plan(plan);

	// ---- open, with the first QP form that the encoder takes
	for (fi = 0; fi < (rate ? 2 : 3) && h == NULL; fi++)
	{
		struct mfh264_cfg cfg = MFH264_CFG_INIT(c->codedW, c->codedH);
		cfg.profile = c->profile != 0 ? c->profile : MFH264_PROFILE_HIGH;
		cfg.max_level_idc = c->maxLevel; cfg.level_idc = c->level; cfg.qp_method = forms[rate ? 0 : fi]; cfg.max_au_size = (int)outCap; cfg.hw_luid = c->luid;
		if (rate) { cfg.rc = fi == 0 ? MFH264_RC_CBR : MFH264_RC_LDVBR; cfg.qp_method = MFH264_QP_NONE; cfg.bitrate = plan_bps(c, 30, cfg.profile); }
		memset(&err, 0, sizeof(err));
		t0 = c->nowMs();
		h = be->open(&cfg, &err);
		if (h == NULL)
		{
			if (err.stage != NULL && strcmp(err.stage, "GOP periodic") == 0) { set_rule(res, KVM_HWP_V_FAIL, "gop"); goto done; }	// the encoder makes a key frame of its own every second or so (the GOP does not hold in either order)
			// a form that the open itself refuses (E_ARG; in the pass "rate" also the refusal of the mode, E_CODECAPI) is the next one's turn; anything else is the verdict
			if (err.code != MFH264_E_ARG && !(rate && err.code == MFH264_E_CODECAPI)) { char r[40]; snprintf(r, sizeof(r), "open%d", -err.code); set_rule(res, KVM_HWP_V_FAIL, r); goto done; }
			continue;
		}
		res->openMs = (uint32_t)(c->nowMs() - t0);
		memset(&err, 0, sizeof(err));
		if (rate) { usedForm = fi == 0 ? KVM_HWP_FORM_BR_CBR : KVM_HWP_FORM_BR_LDVBR; continue; }	// (h is not NULL: the loop ends)
		if (be->set_qp(h, 30, &err) != 0)
		{
			be->close(h); h = NULL;						// this form is not taken by the encoder: the next one
			if (err.code == MFH264_E_HANG || err.code == MFH264_E_EXCEPTION) { char r[40]; snprintf(r, sizeof(r), "setqp%d", -err.code); set_rule(res, KVM_HWP_V_FAIL, r); goto done; }
			continue;
		}
		usedForm = forms[fi];
		(void)form;
	}
	if (h == NULL) { set_rule(res, KVM_HWP_V_FAIL, rate ? "rate_form" : "qp_form"); goto done; }
	res->qpForm = usedForm;
	be->get_info(h, &info);
	if (c->diag != NULL && info.gop_trace[0] != 0) { fprintf(c->diag, "%s\n", info.gop_trace); }
	if (info.max_inflight > 1) { set_rule(res, KVM_HWP_V_FAIL, "inflight"); goto done; }
	if (res->openMs > (uint32_t)KVM_HWT_OPEN_MS) { set_rule(res, KVM_HWP_V_SLOW, "open_slow"); }

	// ---- the stream
	kvmh_init(&chk, (unsigned)c->codedW, (unsigned)c->codedH, c->maxLevel);
	for (i = 0; i < nPlan; i++)
	{
		const frame_plan *p = &plan[i];
		size_t len = 0;
		int idr = 0, rc;
		double a, b;
		memset(&err, 0, sizeof(err));
		dfl[nD] = 0;
		if (i == 0 || plan[i - 1].qp != p->qp)
		{
			dfl[nD] = i > 0 ? 2 : 0;
			if ((rate ? be->set_bitrate(h, plan_bps(c, p->qp, c->profile != 0 ? c->profile : MFH264_PROFILE_HIGH), &err) : be->set_qp(h, p->qp, &err)) != 0) { char r[40]; diag_fail(c->diag, rate ? "dr" : "dq", rate ? "set_bitrate" : "set_qp", i, err.code, &err); snprintf(r, sizeof(r), rate ? "setbr%d" : "setqp%d", -err.code); set_rule(res, KVM_HWP_V_FAIL, r); goto done; }
		}
		if (p->force && be->force_idr(h, &err) != 0) { char r[40]; diag_fail(c->diag, rate ? "dr" : "dq", "force_idr", i, err.code, &err); snprintf(r, sizeof(r), "force%d", -err.code); set_rule(res, KVM_HWP_V_FAIL, r); goto done; }
		kvm_synth_frame_bgra(&syn, p->kind, p->t, bgra, c->codedW * 4);
		// the last two pictures (video) go as BGRA, the others as the view: both ways into the encoder are in the stream (the BGRA way is the one of MESH_KVM_NV12_FROZEN=0 and of a picture that is too big for the view)
		rc = encode_frame(be, h, nv12 != NULL && i < nPlan - 2, bgra, nv12, c->codedW, c->codedH, (int64_t)(i + 1) * 333333, out, outCap, &len, &idr, &err, c->nowMs, &a, &b);
		if (rc != 0)
		{
			char r[40];
			diag_fail(c->diag, rate ? "dr" : "dq", "encode", i, rc, &err);
			if (rc == MFH264_E_HANG) { snprintf(r, sizeof(r), "hang"); } else { snprintf(r, sizeof(r), "encode%d", -rc); }
			set_rule(res, KVM_HWP_V_FAIL, r);
			goto done;
		}
		dsz[nD] = (unsigned)len; dfl[nD] = (unsigned char)(dfl[nD] | (idr ? 1 : 0)); nD++;
		if (len < 5) { set_rule(res, KVM_HWP_V_FAIL, "empty"); goto done; }
		if (idr != p->expectIdr) { set_rule(res, KVM_HWP_V_FAIL, p->force ? "idr_late" : (idr ? "idr_unasked" : "idr_missing")); goto done; }
		if (idr && len > 1536u * 1024u) { set_rule(res, KVM_HWP_V_FAIL, "idr_size"); goto done; }
		kvmh_au(&chk, out, len, idr);
		{
			const double us = (b - a) * 1000.0;
			const unsigned u = us < 0.0 ? 0u : (unsigned)us;
			if (idr) { if (u > idrMaxUs) { idrMaxUs = u; } if (i == 0) { res->idrKb = (uint32_t)((len + 1023) / 1024); } }
			else { usP[nP++] = u; }
		}
		if (p->measure == 26) { sumA += len; nA++; } else if (p->measure == 40) { sumB += len; nB++; }
	}
	// the floor of a flat key frame of this encoder (a flat picture, a key frame on request, by the way of the view as a session with (B) takes it; thousandths of a byte per macroblock). Only the size is
	// information. A hang or an exception on a flat picture is a failure: a lock screen or a black screen is such a picture, and an encoder that falls over on it ends the process of the session at the first one.
	{
		size_t len = 0;
		int idr = 0, rc;
		double a, b;
		const unsigned mbs = (unsigned)(c->codedW / 16) * (unsigned)(c->codedH / 16);
		memset(bgra, 100, (size_t)c->codedW * (size_t)c->codedH * 4u);
		memset(&err, 0, sizeof(err));
		rc = mbs > 0 ? be->force_idr(h, &err) : -1;
		if (rc != 0 && (rc == MFH264_E_HANG || rc == MFH264_E_EXCEPTION || err.code == MFH264_E_HANG || err.code == MFH264_E_EXCEPTION)) { set_rule(res, KVM_HWP_V_FAIL, "flat"); goto done; }
		if (rc == 0)
		{
			memset(&err, 0, sizeof(err));
			rc = encode_frame(be, h, nv12 != NULL, bgra, nv12, c->codedW, c->codedH, (int64_t)(nPlan + 1) * 333333, out, outCap, &len, &idr, &err, c->nowMs, &a, &b);
			if (rc != 0 && (rc == MFH264_E_HANG || rc == MFH264_E_EXCEPTION || err.code == MFH264_E_HANG || err.code == MFH264_E_EXCEPTION)) { set_rule(res, KVM_HWP_V_FAIL, "flat"); goto done; }
			if (rc == 0 && idr && len >= 5) { res->flatMilli = (uint32_t)(((unsigned long long)len * 1000ull) / (unsigned long long)mbs); }
		}
	}
	res->frames = (uint32_t)nPlan;

	// ---- the verdicts on what was seen
	k = kvmh_finish(&chk);
	if (k > 0)
	{
		for (i = 0; i < KVMH_NRULES; i++) { if (chk.r[i].status == KVMH_FAIL) { set_rule(res, KVM_HWP_V_FAIL, kvmh_rule_id(i)); break; } }
	}
	if (chk.haveSps && chk.sps.level > c->level) { set_rule(res, KVM_HWP_V_FAIL, "level_over"); }
	if (chk.haveSps && kvmh_colour_verdict(&chk.extra) == KVMH_COLOUR_WRONG) { set_rule(res, KVM_HWP_V_FAIL, "colour"); }
	if (nA == 0 || nB == 0 || sumA * (unsigned long)nB * 100ul <= (unsigned long)KVM_HWT_QP_RATIO_X100 * sumB * (unsigned long)nA) { set_rule(res, KVM_HWP_V_FAIL, rate ? "rate" : "qp"); }
	if (nP > 0)
	{
		qsort(usP, nP, sizeof(usP[0]), cmp_uint);
		res->p50Us = usP[nP / 2];
		res->p95Us = usP[(nP * 95 + 99) / 100 - 1 < nP ? (nP * 95 + 99) / 100 - 1 : nP - 1];
		if (res->p50Us > KVM_HWT_P50_US || res->p95Us > KVM_HWT_P95_US || idrMaxUs > KVM_HWT_IDR_US) { set_rule(res, KVM_HWP_V_SLOW, "latency"); }
	}
done:
	diag_sizes(c->diag, rate ? "dr" : "dq", dsz, dfl, nD);
	diag_info(c->diag, rate ? "dr" : "dq", be, h);
	if (h != NULL) { be->close(h); }
	free(bgra); free(out); free(nv12);
	kvm_synth_free(&syn);
	res->valid = 1;
	return res->verdict;
}

static int run_all(const kvm_encbe *be, const kvm_hwt_cfg *c, kvm_hwp_result *res)
{
	kvm_hwp_result r2;
	run_pass(be, c, res, 0);
	// an encoder that takes the QP and ignores it (NVIDIA: the MFT stays in CBR) is tried again with the bit rate; what the second pass says is the verdict (a failure of it is told with its own rule)
	if (res->verdict == KVM_HWP_V_FAIL && (strcmp(res->rule, "qp") == 0 || strcmp(res->rule, "qp_form") == 0))
	{
		run_pass(be, c, &r2, 1);
		*res = r2;
	}
	return res->verdict;
}

int kvm_hwt_run(const kvm_encbe *be, const kvm_hwt_cfg *c, kvm_hwp_result *res)
{
	if (c->clock != NULL) { c->clock(1); }		// the session runs with timeBeginPeriod(1): the test measures the encoder in the same conditions (the default tick of 15.6 ms rounds every wait of the encoder up)
	return run_all(be, c, res);
}

void kvm_hwt_diag_clock(const kvm_encbe *be, const kvm_hwt_cfg *c, const kvm_hwp_result *fine)
{
	kvm_hwp_result dflt;
	if (c->diag == NULL || c->clock == NULL) { return; }
	c->clock(0);
	run_all(be, c, &dflt);
	c->clock(1);
	fprintf(c->diag, "lat default p50=%u p95=%u rule=%s | lat 1ms p50=%u p95=%u rule=%s\n", (unsigned)dflt.p50Us, (unsigned)dflt.p95Us, dflt.rule[0] ? dflt.rule : "-", (unsigned)fine->p50Us, (unsigned)fine->p95Us, fine->rule[0] ? fine->rule : "-");
	fflush(c->diag);
}

// ---- the hardware self test diagnostics (see kvm_hwtest_core.h)
// n pictures of scroll in one open encoder: sizes into sz / fl (1 = key frame; 2 = the QP was changed before it). Returns how many pictures came out. qpAt > 0: the QP goes to 40 at this picture (Quality mode).
static int diag_run(const kvm_encbe *be, struct mfh264 *h, const kvm_hwt_cfg *c, kvm_synth *syn, uint8_t *bgra, uint8_t *nv12, uint8_t *out, size_t outCap, int n, int qpAt, unsigned *sz, unsigned char *fl)
{
	int i, got = 0;
	for (i = 0; i < n; i++)
	{
		struct mfh264_err err;
		size_t len = 0;
		int idr = 0;
		double a, b;
		memset(&err, 0, sizeof(err));
		fl[got] = 0;
		if (qpAt > 0 && i == qpAt) { if (be->set_qp(h, 40, &err) != 0) { break; } fl[got] = 2; }
		kvm_synth_frame_bgra(syn, K_SCROLL, i, bgra, c->codedW * 4);
		{
			const int rc = encode_frame(be, h, nv12 != NULL, bgra, nv12, c->codedW, c->codedH, (int64_t)(i + 1) * 333333, out, outCap, &len, &idr, &err, c->nowMs, &a, &b);
			if (rc != 0) { diag_fail(c->diag, "diag", "encode", i, rc, &err); break; }
		}
		sz[got] = (unsigned)len; fl[got] = (unsigned char)(fl[got] | (idr ? 1 : 0)); got++;
	}
	return got;
}

void kvm_hwt_diag(const kvm_encbe *be, const kvm_hwt_cfg *c)
{
	// G1 the order A with only 0x7FFFFFFF; G2 the order B with only 0xFFFFFFFF; G3 B with 0x7FFFFFFF; G4 B in Quality mode, the QP 26 and from the 18th picture 40; G5 A with MF_MT_MAX_KEYFRAME_SPACING in the output type
	static const char *const tags[5] = { "G1", "G2", "G3", "G4", "G5" };
	kvm_synth syn;
	unsigned sz[KVM_HWT_MAXPLAN];
	unsigned char fl[KVM_HWT_MAXPLAN];
	uint8_t *bgra = NULL, *out = NULL, *nv12 = NULL;
	const size_t outCap = 8u * 1024u * 1024u;
	int v;
	if (c->diag == NULL || kvm_synth_init(&syn, c->codedW, c->codedH) != 0) { return; }
	bgra = (uint8_t *)malloc((size_t)c->codedW * (size_t)c->codedH * 4u);
	out = (uint8_t *)malloc(outCap);
	if (be->encode_nv12 != NULL) { nv12 = (uint8_t *)malloc((size_t)c->codedW * (size_t)c->codedH * 3u / 2u); }
	if (bgra == NULL || out == NULL || (be->encode_nv12 != NULL && nv12 == NULL)) { goto done; }
	for (v = 0; v < 5; v++)
	{
		struct mfh264_cfg cfg = MFH264_CFG_INIT(c->codedW, c->codedH);
		struct mfh264_err err;
		struct mfh264_info info;
		struct mfh264 *h;
		int n;
		cfg.profile = c->profile != 0 ? c->profile : MFH264_PROFILE_HIGH;
		cfg.max_level_idc = c->maxLevel; cfg.level_idc = c->level; cfg.max_au_size = (int)outCap; cfg.hw_luid = c->luid;
		cfg.rc = MFH264_RC_CBR; cfg.qp_method = MFH264_QP_NONE; cfg.bitrate = plan_bps(c, 30, cfg.profile);
		cfg.order = (v == 0 || v == 4) ? 1 : 2;
		cfg.gop_only = v == 1 ? 0xFFFFFFFFu : 0x7FFFFFFFu;
		if (v == 3) { cfg.rc = MFH264_RC_QUALITY; cfg.qp_method = MFH264_QP_ENCODEQP_UI4; }
		if (v == 4) { cfg.keyspace = 0x7FFFFFFF; cfg.gop_only = 0; }
		memset(&err, 0, sizeof(err));
		h = be->open(&cfg, &err);
		if (h == NULL) { fprintf(c->diag, "%s=open failed code=%d\n", tags[v], err.code); continue; }
		memset(&info, 0, sizeof(info));
		be->get_info(h, &info);
		fprintf(c->diag, "%s %s\n", tags[v], info.gop_trace);
		memset(fl, 0, sizeof(fl));
		if (v == 3 && be->set_qp(h, 26, &err) != 0) { fprintf(c->diag, "%s=set_qp failed\n", tags[v]); be->close(h); continue; }
		n = diag_run(be, h, c, &syn, bgra, nv12, out, outCap, 35, v == 3 ? 18 : 0, sz, fl);
		diag_sizes(c->diag, tags[v], sz, fl, n);
		diag_info(c->diag, tags[v], be, h);
		be->close(h);
	}
done:
	free(bgra); free(out); free(nv12);
	kvm_synth_free(&syn);
}
