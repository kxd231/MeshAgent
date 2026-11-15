/* Tests of kvm_h264_rules.c: access units built with a bit writer, so every field of the SPS, PPS and slice header is known. One test for every rule (it passes on a good stream,
   it fails on a stream that breaks exactly that rule), the splitting of a stream into access units, truncated and random input, and a comparison with the agent's own decision
   (kvm_vcore_check_sps) on a table of SPS variants. */
#define _POSIX_C_SOURCE 200809L

#include "../kvm_h264_rules.h"
#include "../kvm_stream.h"
#include "../kvm_video_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* ---- bit writer ---- */
struct bw { uint8_t b[2048]; size_t n; int bit; };
static void wbit(struct bw *w, unsigned v) { if (w->bit == 0) { w->b[w->n++] = 0; } if (v) { w->b[w->n - 1] |= (uint8_t)(0x80u >> w->bit); } w->bit = (w->bit + 1) & 7; }
static void wbits(struct bw *w, unsigned v, int n) { while (n-- > 0) { wbit(w, (v >> n) & 1u); } }
static void wue(struct bw *w, unsigned v) { unsigned x = v + 1; int len = 0; while ((x >> len) > 1) { len++; } wbits(w, 0, len); wbits(w, x, len + 1); }
static void wse(struct bw *w, int v) { wue(w, v > 0 ? (unsigned)(2 * v - 1) : (unsigned)(-2 * v)); }

/* NAL unit with start code (4 bytes), header byte, body with emulation prevention */
static size_t put_nal(uint8_t *o, size_t n, int sc4, int refIdc, int type, const uint8_t *body, size_t bn)
{
	size_t i;
	if (sc4) { o[n++] = 0; }
	o[n++] = 0; o[n++] = 0; o[n++] = 1;
	o[n++] = (uint8_t)((refIdc << 5) | type);
	for (i = 0; i < bn; i++)
	{
		if (n >= 3 && o[n - 1] == 0 && o[n - 2] == 0 && body[i] <= 3) { o[n++] = 3; }
		o[n++] = body[i];
	}
	return n;
}

struct sps_p
{
	int profile, constraint, level, spsId, refs, mbsW, mbsH, crop, frameMbsOnly, chroma, depth, poc, vui;	/* vui: 0 none, 1 timing only, 2 restriction (0, 1), 3 restriction (2, 4), 4 hrd + restriction (0, 1) */
	int pad;			/* extra bytes appended to the NAL unit body */
	int sig;			/* video_signal_type: 0 none, 1 BT.709 limited, 2 BT.709 full range, 3 BT.601 limited, 4 present without colour description, 5 all unspecified (2), 6 BT.709 matrix only with other primaries (5), 7 the AMD encoder: 0 / 0 / 2, 8 matrix 0 (identity, GBR) with BT.709 primaries and transfer, 9 0 / 0 / 0 */
};

static struct sps_p sps_default(void)
{
	struct sps_p p;
	memset(&p, 0, sizeof(p));
	p.profile = 100; p.constraint = 0; p.level = 42; p.spsId = 0; p.refs = 1; p.mbsW = 120; p.mbsH = 68; p.frameMbsOnly = 1; p.chroma = 1; p.depth = 8; p.vui = 2;
	return p;
}

static size_t put_sps(uint8_t *o, size_t n, const struct sps_p *p)
{
	struct bw w;
	uint8_t body[1500];
	size_t bn, i;
	memset(&w, 0, sizeof(w));
	wbits(&w, (unsigned)p->profile, 8); wbits(&w, (unsigned)p->constraint, 8); wbits(&w, (unsigned)p->level, 8);
	wue(&w, (unsigned)p->spsId);
	if (p->profile == 100 || p->profile == 110 || p->profile == 122)
	{
		wue(&w, (unsigned)p->chroma);
		if (p->chroma == 3) { wbit(&w, 0); }
		wue(&w, (unsigned)(p->depth - 8)); wue(&w, (unsigned)(p->depth - 8)); wbit(&w, 0);
		wbit(&w, 0);	/* no scaling matrix */
	}
	wue(&w, 4);			/* log2_max_frame_num_minus4 */
	if (p->poc == 1) { wue(&w, 1); wbit(&w, 0); wse(&w, -3); wse(&w, 5); wue(&w, 2); wse(&w, 1); wse(&w, -1); }
	else if (p->poc == 2) { wue(&w, 2); }
	else { wue(&w, 0); wue(&w, 6); }
	wue(&w, (unsigned)p->refs); wbit(&w, 0);
	wue(&w, (unsigned)p->mbsW - 1); wue(&w, (unsigned)p->mbsH - 1);
	wbit(&w, (unsigned)p->frameMbsOnly);
	if (!p->frameMbsOnly) { wbit(&w, 0); }
	wbit(&w, 1);		/* direct_8x8_inference_flag */
	wbit(&w, (unsigned)p->crop);
	if (p->crop) { wue(&w, 0); wue(&w, 0); wue(&w, 0); wue(&w, 4); }
	if (p->vui == 0) { wbit(&w, 0); }
	else
	{
		wbit(&w, 1);
		wbit(&w, 1); wbits(&w, 1, 8);				/* aspect_ratio_info: idc 1 */
		wbit(&w, 0);								/* overscan */
		if (p->sig == 0) { wbit(&w, 0); }
		else
		{
			wbit(&w, 1); wbits(&w, 5, 3);			/* video_signal_type_present, video_format 5 (unspecified) */
			wbit(&w, p->sig == 2 ? 1u : 0u);		/* video_full_range_flag */
			if (p->sig == 4) { wbit(&w, 0); }
			else
			{
				unsigned pr = 1, tr = 1, mx = 1;
				if (p->sig == 3) { pr = 6; tr = 6; mx = 6; } else if (p->sig == 5) { pr = tr = mx = 2; } else if (p->sig == 6) { pr = 5; } else if (p->sig == 7) { pr = 0; tr = 0; mx = 2; } else if (p->sig == 8) { mx = 0; } else if (p->sig == 9) { pr = tr = mx = 0; }
				wbit(&w, 1); wbits(&w, pr, 8); wbits(&w, tr, 8); wbits(&w, mx, 8);
			}
		}
		wbit(&w, 0);								/* chroma_loc_info */
		wbit(&w, 1); wbits(&w, 1001, 32); wbits(&w, 60000, 32); wbit(&w, 1);	/* timing info */
		if (p->vui == 4)
		{
			wbit(&w, 1);							/* nal_hrd_parameters_present */
			wue(&w, 0); wbits(&w, 0x11, 8);			/* cpb_cnt_minus1 = 0, bit_rate_scale / cpb_size_scale */
			wue(&w, 100000); wue(&w, 200000); wbit(&w, 1);
			wbits(&w, 0x0F0F0F, 20);
			wbit(&w, 0);							/* vcl_hrd */
			wbit(&w, 0);							/* low_delay_hrd_flag */
		}
		else { wbit(&w, 0); wbit(&w, 0); }
		wbit(&w, 0);								/* pic_struct_present_flag */
		if (p->vui >= 2)
		{
			wbit(&w, 1);							/* bitstream_restriction_flag */
			wbit(&w, 1); wue(&w, 2); wue(&w, 1); wue(&w, 16); wue(&w, 16);
			if (p->vui == 3) { wue(&w, 2); wue(&w, 4); } else { wue(&w, 0); wue(&w, 1); }
		}
		else { wbit(&w, 0); }
	}
	wbit(&w, 1);		/* rbsp stop bit */
	memcpy(body, w.b, w.n); bn = w.n;
	for (i = 0; i < (size_t)p->pad; i++) { body[bn++] = (uint8_t)(0x55 + (i & 7)); }
	return put_nal(o, n, 1, 3, 7, body, bn);
}

static size_t put_pps(uint8_t *o, size_t n, int ppsId, int spsId)
{
	struct bw w;
	memset(&w, 0, sizeof(w));
	wue(&w, (unsigned)ppsId); wue(&w, (unsigned)spsId); wbit(&w, 1); wbit(&w, 0); wue(&w, 0); wue(&w, 0); wue(&w, 0); wbit(&w, 0); wbits(&w, 0, 2); wse(&w, 0); wse(&w, 0); wse(&w, 0); wbit(&w, 1); wbit(&w, 0); wbit(&w, 0);
	wbit(&w, 1);
	return put_nal(o, n, 1, 3, 8, w.b, w.n);
}

/* slice NAL unit: first_mb_in_slice, slice_type, pps id, then len filler bytes */
static size_t put_slice(uint8_t *o, size_t n, int idr, unsigned firstMb, unsigned sliceType, int ppsId, int len)
{
	struct bw w;
	uint8_t body[200];
	int i;
	memset(&w, 0, sizeof(w));
	wue(&w, firstMb); wue(&w, sliceType); wue(&w, (unsigned)ppsId); wbits(&w, 0, 4);
	memcpy(body, w.b, w.n);
	for (i = 0; i < len && w.n + (size_t)i < sizeof(body); i++) { body[w.n + (size_t)i] = (uint8_t)(0x80 + i); }
	return put_nal(o, n, 0, idr ? 3 : 2, idr ? 5 : 1, body, w.n + (size_t)len);
}

static size_t put_other(uint8_t *o, size_t n, int type, int len)
{
	uint8_t body[64];
	int i;
	for (i = 0; i < len; i++) { body[i] = (uint8_t)(0x90 + i); }
	return put_nal(o, n, 0, 0, type, body, (size_t)len);
}

static size_t idr_au(uint8_t *o, const struct sps_p *p, int ppsSpsId)
{
	size_t n = 0;
	n = put_sps(o, n, p); n = put_pps(o, n, 0, ppsSpsId); n = put_slice(o, n, 1, 0, 7, 0, 30);
	return n;
}
static size_t p_au(uint8_t *o) { return put_slice(o, 0, 0, 0, 5, 0, 10); }

static int status_of(struct kvmh_check *c, int rule) { kvmh_finish(c); return c->r[rule].status; }

/* a stream of an IDR and three P pictures */
static void good_stream(struct kvmh_check *c, const struct sps_p *p, unsigned w, unsigned h)
{
	uint8_t b[600];
	size_t n;
	int i;
	kvmh_init(c, w, h, 0);
	n = idr_au(b, p, p->spsId);
	kvmh_au(c, b, n, 1);
	for (i = 0; i < 3; i++) { n = p_au(b); kvmh_au(c, b, n, 0); }
}

static void test_good(void)
{
	struct kvmh_check c;
	struct sps_p p = sps_default();
	int r;
	good_stream(&c, &p, 1920, 1088);
	CHECK(kvmh_finish(&c) == 0);
	for (r = 0; r < KVMH_NRULES; r++) { CHECK(c.r[r].status == KVMH_PASS || c.r[r].status == KVMH_NOTRUN); }
	CHECK(c.r[KVMH_R_VUI].status == KVMH_PASS && c.r[KVMH_R_ANNEXB].checks == 4 && c.idrs == 1 && c.pAus == 3);
	CHECK(c.sps.mbsW == 120 && c.sps.mbsH == 68 && c.extra.reorder == 0 && c.extra.buffering == 1 && c.extra.vui == 1 && c.extra.restriction == 1);
	/* Main and Constrained Baseline, POC types 0, 1, 2, no VUI at all (WARN only) */
	p.profile = 77; p.poc = 1; p.vui = 1;
	good_stream(&c, &p, 1920, 1088);
	CHECK(kvmh_finish(&c) == 0 && c.r[KVMH_R_VUI].status == KVMH_WARN);
	p.profile = 66; p.constraint = 0xC0; p.poc = 2; p.vui = 0;
	good_stream(&c, &p, 1920, 1088);
	CHECK(kvmh_finish(&c) == 0 && c.r[KVMH_R_VUI].status == KVMH_WARN && c.extra.vui == 0);
	p.profile = 100; p.poc = 0; p.vui = 4;	/* HRD parameters in the VUI are skipped correctly */
	good_stream(&c, &p, 1920, 1088);
	CHECK(kvmh_finish(&c) == 0 && c.r[KVMH_R_VUI].status == KVMH_PASS);
	CHECK(c.extra.reorder == 0 && c.extra.buffering == 1);
	p.vui = 3;
	good_stream(&c, &p, 1920, 1088);
	CHECK(kvmh_finish(&c) == 0 && c.r[KVMH_R_VUI].status == KVMH_WARN && c.extra.reorder == 2 && c.extra.buffering == 4);
	/* expected size not given: no geometry comparison */
	good_stream(&c, &p, 0, 0);
	CHECK(kvmh_finish(&c) == 0);
}

static void test_colour(void)
{
	struct kvmh_check c;
	struct sps_p p = sps_default();
	static const int expect[10] = { KVMH_COLOUR_UNSPECIFIED, KVMH_COLOUR_BT709, KVMH_COLOUR_WRONG, KVMH_COLOUR_WRONG, KVMH_COLOUR_UNSPECIFIED, KVMH_COLOUR_UNSPECIFIED, KVMH_COLOUR_WRONG, KVMH_COLOUR_UNSPECIFIED, KVMH_COLOUR_WRONG, KVMH_COLOUR_WRONG };
	int s;
	for (s = 0; s < 10; s++)
	{
		p.sig = s;
		good_stream(&c, &p, 1920, 1088);
		CHECK(kvmh_finish(&c) == 0);					/* the colour is not one of the 15 rules: the stream still passes */
		CHECK(c.extra.vui == 1);
		CHECK(kvmh_colour_verdict(&c.extra) == expect[s]);
		CHECK(c.extra.signal == (s != 0));
		CHECK(c.extra.fullRange == (s == 2));
		CHECK(c.extra.colourDesc == (s != 0 && s != 4));
		if (s == 1) { CHECK(c.extra.primaries == 1 && c.extra.transfer == 1 && c.extra.matrix == 1); }
		if (s == 3) { CHECK(c.extra.primaries == 6 && c.extra.matrix == 6); }
		if (s == 7) { CHECK(c.extra.primaries == 0 && c.extra.transfer == 0 && c.extra.matrix == 2 && c.extra.fullRange == 0); }
		/* the fields behind video_signal_type are still read right: timing, restriction */
		CHECK(c.extra.restriction == 1 && c.extra.reorder == 0 && c.extra.buffering == 1);
	}
	p.sig = 0; p.vui = 0;
	good_stream(&c, &p, 1920, 1088);
	CHECK(kvmh_colour_verdict(&c.extra) == KVMH_COLOUR_UNSPECIFIED && c.extra.vui == 0);
	CHECK(kvmh_colour_verdict(NULL) == KVMH_COLOUR_UNSPECIFIED);
}

static int broken(struct kvmh_check *c, int rule) { return status_of(c, rule) == KVMH_FAIL; }

static void test_rules(void)
{
	struct kvmh_check c;
	struct sps_p p;
	uint8_t b[2400], t[200];
	size_t n, k;

	/* annexb: a length prefixed access unit */
	kvmh_init(&c, 0, 0, 0);
	b[0] = 0; b[1] = 0; b[2] = 0; b[3] = 9; memset(b + 4, 0x41, 9);
	CHECK(kvmh_au(&c, b, 13, -1) == 1 && broken(&c, KVMH_R_ANNEXB) && strstr(c.r[KVMH_R_ANNEXB].first, "AVCC") != NULL);
	kvmh_init(&c, 0, 0, 0);
	CHECK(kvmh_au(&c, NULL, 0, -1) == 1 && broken(&c, KVMH_R_ANNEXB));

	/* slices */
	p = sps_default();
	kvmh_init(&c, 0, 0, 0);
	n = put_other(b, 0, 6, 8);
	kvmh_au(&c, b, n, -1);
	CHECK(broken(&c, KVMH_R_SLICES));
	kvmh_init(&c, 0, 0, 0);
	n = idr_au(b, &p, 0); n = put_slice(b, n, 0, 5, 5, 0, 8);
	kvmh_au(&c, b, n, 1);
	CHECK(broken(&c, KVMH_R_SLICES));

	/* idrflag: first picture not an IDR; expected flag does not match */
	kvmh_init(&c, 0, 0, 0);
	n = p_au(b); kvmh_au(&c, b, n, -1);
	CHECK(broken(&c, KVMH_R_IDRFLAG));
	good_stream(&c, &p, 0, 0);
	n = p_au(b); kvmh_au(&c, b, n, 1);
	CHECK(broken(&c, KVMH_R_IDRFLAG) && c.r[KVMH_R_IDRFLAG].fails == 1);
	good_stream(&c, &p, 0, 0);
	n = idr_au(b, &p, 0); kvmh_au(&c, b, n, 0);
	CHECK(broken(&c, KVMH_R_IDRFLAG));

	/* idr_ps: no SPS, no PPS, parameter sets behind the slice */
	kvmh_init(&c, 0, 0, 0);
	n = put_pps(b, 0, 0, 0); n = put_slice(b, n, 1, 0, 7, 0, 20);
	kvmh_au(&c, b, n, 1);
	CHECK(broken(&c, KVMH_R_IDR_PS));
	kvmh_init(&c, 0, 0, 0);
	n = put_sps(b, 0, &p); n = put_slice(b, n, 1, 0, 7, 0, 20);
	kvmh_au(&c, b, n, 1);
	CHECK(broken(&c, KVMH_R_IDR_PS));
	kvmh_init(&c, 0, 0, 0);
	n = put_slice(b, 0, 1, 0, 7, 0, 20); n = put_sps(b, n, &p); n = put_pps(b, n, 0, 0);
	kvmh_au(&c, b, n, 1);
	CHECK(broken(&c, KVMH_R_IDR_PS));

	/* naltypes */
	for (k = 0; k < 6; k++)
	{
		static const int types[6] = { 2, 3, 4, 14, 15, 20 };
		good_stream(&c, &p, 0, 0);
		n = p_au(b); n = put_other(b, n, types[k], 6);
		kvmh_au(&c, b, n, 0);
		CHECK(broken(&c, KVMH_R_NALTYPES));
	}
	good_stream(&c, &p, 0, 0);	/* SEI, AUD, filler (12), end of sequence (10) are allowed */
	n = put_other(b, 0, 9, 1); n = put_other(b, n, 6, 6); n = put_slice(b, n, 0, 0, 5, 0, 10); n = put_other(b, n, 12, 5); n = put_other(b, n, 10, 1);
	kvmh_au(&c, b, n, 0);
	CHECK(kvmh_finish(&c) == 0);

	/* pssize */
	p = sps_default(); p.pad = 1030;
	kvmh_init(&c, 0, 0, 0);
	n = idr_au(b, &p, 0); kvmh_au(&c, b, n, 1);
	CHECK(broken(&c, KVMH_R_PSSIZE));
	p = sps_default();
	kvmh_init(&c, 0, 0, 0);
	n = put_sps(b, 0, &p); for (k = 0; k < 5; k++) { n = put_pps(b, n, (int)k, 0); } n = put_slice(b, n, 1, 0, 7, 0, 20);
	kvmh_au(&c, b, n, 1);
	CHECK(broken(&c, KVMH_R_PSSIZE));
	kvmh_init(&c, 0, 0, 0);
	n = put_sps(b, 0, &p); p.level = 40; n = put_sps(b, n, &p); n = put_pps(b, n, 0, 0); n = put_slice(b, n, 1, 0, 7, 0, 20);
	kvmh_au(&c, b, n, 1);
	CHECK(broken(&c, KVMH_R_PSSIZE));
	p = sps_default();
	kvmh_init(&c, 0, 0, 0);
	n = put_sps(b, 0, &p); n = put_sps(b, n, &p); n = put_pps(b, n, 0, 0); n = put_pps(b, n, 1, 0); n = put_slice(b, n, 1, 0, 7, 0, 20);	/* the same SPS twice, two PPS: fine */
	kvmh_au(&c, b, n, 1);
	CHECK(kvmh_finish(&c) == 0);

	/* profile */
	{
		static const struct { int profile, constraint, ok; } pt[] = { { 66, 0xC0, 1 }, { 66, 0x40, 1 }, { 66, 0x80, 0 }, { 66, 0, 0 }, { 77, 0, 1 }, { 88, 0, 0 }, { 100, 0, 1 }, { 110, 0, 0 }, { 122, 0, 0 }, { 244, 0, 0 } };
		for (k = 0; k < sizeof(pt) / sizeof(pt[0]); k++)
		{
			p = sps_default(); p.profile = pt[k].profile; p.constraint = pt[k].constraint;
			kvmh_init(&c, 0, 0, 0);
			n = idr_au(b, &p, 0); kvmh_au(&c, b, n, 1);
			CHECK(broken(&c, KVMH_R_PROFILE) == !pt[k].ok);
		}
	}
	/* level: standard levels, 1b of High, an invalid one, above the viewer's limit */
	{
		static const struct { int profile, level, maxL, ok; } lt[] = { { 100, 52, 0, 1 }, { 100, 53, 0, 0 }, { 100, 62, 0, 0 }, { 100, 9, 0, 1 }, { 77, 9, 0, 0 }, { 100, 51, 42, 0 }, { 100, 42, 42, 1 }, { 77, 31, 0, 1 }, { 100, 0, 0, 0 }, { 100, 45, 0, 0 } };
		for (k = 0; k < sizeof(lt) / sizeof(lt[0]); k++)
		{
			p = sps_default(); p.profile = lt[k].profile; p.level = lt[k].level;
			kvmh_init(&c, 0, 0, lt[k].maxL);
			n = idr_au(b, &p, 0); kvmh_au(&c, b, n, 1);
			CHECK(broken(&c, KVMH_R_LEVEL) == !lt[k].ok);
		}
	}
	/* format */
	p = sps_default(); p.chroma = 2;
	kvmh_init(&c, 0, 0, 0); n = idr_au(b, &p, 0); kvmh_au(&c, b, n, 1); CHECK(broken(&c, KVMH_R_FORMAT));
	p = sps_default(); p.chroma = 0;
	kvmh_init(&c, 0, 0, 0); n = idr_au(b, &p, 0); kvmh_au(&c, b, n, 1); CHECK(broken(&c, KVMH_R_FORMAT));
	p = sps_default(); p.depth = 10;
	kvmh_init(&c, 0, 0, 0); n = idr_au(b, &p, 0); kvmh_au(&c, b, n, 1); CHECK(broken(&c, KVMH_R_FORMAT));
	p = sps_default(); p.frameMbsOnly = 0; p.mbsH = 34;
	kvmh_init(&c, 0, 0, 0); n = idr_au(b, &p, 0); kvmh_au(&c, b, n, 1); CHECK(broken(&c, KVMH_R_FORMAT));
	/* refs */
	p = sps_default(); p.refs = 2;
	kvmh_init(&c, 0, 0, 0); n = idr_au(b, &p, 0); kvmh_au(&c, b, n, 1); CHECK(broken(&c, KVMH_R_REFS));
	p.refs = 0;
	kvmh_init(&c, 0, 0, 0); n = idr_au(b, &p, 0); kvmh_au(&c, b, n, 1); CHECK(!broken(&c, KVMH_R_REFS));
	/* geometry */
	p = sps_default(); p.crop = 1;
	kvmh_init(&c, 0, 0, 0); n = idr_au(b, &p, 0); kvmh_au(&c, b, n, 1); CHECK(broken(&c, KVMH_R_GEOMETRY));
	p = sps_default();
	kvmh_init(&c, 1920, 1080, 0); n = idr_au(b, &p, 0); kvmh_au(&c, b, n, 1); CHECK(broken(&c, KVMH_R_GEOMETRY) && strstr(c.r[KVMH_R_GEOMETRY].first, "1920x1088") != NULL);
	p.mbsW = 260; p.mbsH = 150;
	kvmh_init(&c, 0, 0, 0); n = idr_au(b, &p, 0); kvmh_au(&c, b, n, 1); CHECK(broken(&c, KVMH_R_GEOMETRY));
	p.mbsW = 513; p.mbsH = 8;
	kvmh_init(&c, 0, 0, 0); n = idr_au(b, &p, 0); kvmh_au(&c, b, n, 1); CHECK(broken(&c, KVMH_R_GEOMETRY));
	p.mbsW = 256; p.mbsH = 144;	/* 36864 macroblocks exactly: allowed */
	kvmh_init(&c, 4096, 2304, 0); n = idr_au(b, &p, 0); kvmh_au(&c, b, n, 1); CHECK(!broken(&c, KVMH_R_GEOMETRY));

	/* no_b: slice_type 1 and 6 */
	for (k = 0; k < 4; k++)
	{
		static const unsigned st[4] = { 1, 6, 0, 5 };
		p = sps_default();
		good_stream(&c, &p, 0, 0);
		n = put_slice(b, 0, 0, 0, st[k], 0, 10); kvmh_au(&c, b, n, 0);
		CHECK(broken(&c, KVMH_R_NOB) == (k < 2));
	}

	/* spsstable */
	p = sps_default();
	good_stream(&c, &p, 0, 0);
	n = put_sps(b, 0, &p); n = put_slice(b, n, 0, 0, 5, 0, 10); kvmh_au(&c, b, n, 0);	/* the same SPS in a P picture: fine */
	CHECK(kvmh_finish(&c) == 0);
	good_stream(&c, &p, 0, 0);
	p.level = 40; n = put_sps(b, 0, &p); n = put_slice(b, n, 0, 0, 5, 0, 10); kvmh_au(&c, b, n, 0);
	CHECK(broken(&c, KVMH_R_SPSSTABLE));
	p = sps_default();
	kvmh_init(&c, 0, 0, 0);
	n = idr_au(b, &p, 1); kvmh_au(&c, b, n, 1);	/* PPS refers to SPS id 1 while the SPS has id 0 */
	CHECK(broken(&c, KVMH_R_SPSSTABLE));
	good_stream(&c, &p, 0, 0);
	n = put_pps(b, 0, 0, 3); n = put_slice(b, n, 0, 0, 5, 0, 10); kvmh_au(&c, b, n, 0);
	CHECK(broken(&c, KVMH_R_SPSSTABLE));
	/* a new SPS at a later IDR picture is allowed (the viewer reconfigures) and counted */
	good_stream(&c, &p, 0, 0);
	p.level = 40; n = idr_au(b, &p, 0); kvmh_au(&c, b, n, 1);
	CHECK(kvmh_finish(&c) == 0 && c.spsChanges == 1);

	/* vui is a warning, never a failure */
	p = sps_default(); p.vui = 0;
	kvmh_init(&c, 0, 0, 0); n = idr_au(b, &p, 0); kvmh_au(&c, b, n, 1);
	CHECK(status_of(&c, KVMH_R_VUI) == KVMH_WARN && kvmh_finish(&c) == 0);

	/* parse: an SPS cut short, a slice header cut short */
	p = sps_default();
	n = put_sps(t, 0, &p);
	kvmh_init(&c, 0, 0, 0);
	{
		size_t m = n - 6;
		memcpy(b, t, m); m = put_pps(b, m, 0, 0); m = put_slice(b, m, 1, 0, 7, 0, 20);
		kvmh_au(&c, b, m, 1);
		CHECK(broken(&c, KVMH_R_PARSE));
	}
	kvmh_init(&c, 0, 0, 0);
	n = idr_au(b, &p, 0); b[n - 30 - 2] = 0; /* no change of structure; only checks that damage never crashes */
	kvmh_au(&c, b, n, 1);
}

static void test_split(void)
{
	uint8_t s[4000], sp[200];
	size_t n = 0, a0, a1, a2, a3, a4, pos, e;
	struct sps_p p = sps_default();
	struct kvmh_au_info in;
	(void)sp;
	/* no AUD (x264 / hardware): IDR with two slices, P, P with SEI, IDR again with parameter sets */
	n = put_sps(s, n, &p); n = put_pps(s, n, 0, 0); n = put_slice(s, n, 1, 0, 7, 0, 30); n = put_slice(s, n, 1, 40, 7, 0, 30); a1 = n;
	n = put_slice(s, n, 0, 0, 5, 0, 10); a2 = n;
	n = put_other(s, n, 6, 5); n = put_slice(s, n, 0, 0, 5, 0, 10); a3 = n;
	n = put_other(s, n, 9, 1); n = put_sps(s, n, &p); n = put_pps(s, n, 0, 0); n = put_slice(s, n, 1, 0, 7, 0, 30); a4 = n;
	n = put_slice(s, n, 0, 0, 5, 0, 10);
	a0 = 0;
	pos = a0; e = kvmh_next_au(s, n, pos); CHECK(e == a1);
	pos = e; e = kvmh_next_au(s, n, pos); CHECK(e == a2);
	pos = e; e = kvmh_next_au(s, n, pos); CHECK(e == a3);
	pos = e; e = kvmh_next_au(s, n, pos); CHECK(e == a4);
	pos = e; e = kvmh_next_au(s, n, pos); CHECK(e == n);
	kvmh_au_info(s, a1, &in);
	CHECK(in.annexb && in.idr && in.slices == 2 && in.nSps == 1 && in.nPps == 1 && (in.types & (1u << 5)) && (in.types & (1u << 7)) && in.bSlices == 0);
	{
		char t[64];
		kvmh_nal_list(s, a1, t, sizeof(t));
		CHECK(strcmp(t, "7 8 5 5") == 0);
	}
	kvmh_au_info(s + a1, a2 - a1, &in);
	CHECK(in.idr == 0 && in.slices == 1 && in.nSps == 0);
}

static void test_fuzz(void)
{
	uint8_t b[600], c2[700];
	struct sps_p p = sps_default();
	struct kvmh_check c;
	size_t n, k;
	uint32_t x = 2463534242u;
	int i;
	p.vui = 4;
	n = idr_au(b, &p, 0);
	for (k = 0; k <= n; k++)	/* every prefix */
	{
		kvmh_init(&c, 1920, 1088, 52);
		kvmh_au(&c, b, k, 1);
		kvmh_finish(&c);
	}
	for (i = 0; i < 20000; i++)	/* random damage */
	{
		memcpy(c2, b, n);
		x ^= x << 13; x ^= x >> 17; x ^= x << 5;
		c2[x % n] ^= (uint8_t)(1u << (x % 8));
		x ^= x << 13; x ^= x >> 17; x ^= x << 5;
		kvmh_init(&c, 1920, 1088, 52);
		kvmh_au(&c, c2, n, 1);
		kvmh_au(&c, c2, n - (x % 20), 0);
		kvmh_finish(&c);
		(void)kvmh_next_au(c2, n, 0);
	}
	for (i = 0; i < 20000; i++)	/* random bytes */
	{
		size_t m = 1 + (x % 300), q;
		for (q = 0; q < m; q++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; c2[q] = (uint8_t)(x & 0xFF); if ((x & 0x30000) == 0) { c2[q] = (uint8_t)(q & 1 ? 1 : 0); } }
		kvmh_init(&c, 0, 0, 0);
		kvmh_au(&c, c2, m, -1);
		(void)kvmh_next_au(c2, m, 0);
	}
	CHECK(1);
}

/* the agent's own decision (kvm_vcore_check_sps) and the rules of this module agree on the SPS fields they both judge */
static void test_agent_agrees(void)
{
	int profiles[] = { 66, 77, 100 };
	int constraints[] = { 0, 0x40, 0xC0 };
	int levels[] = { 30, 42, 51, 52, 53 };	/* level 9 (1b of High) is left out: the viewer accepts it, the agent refuses every level below 10 (the agent is the stricter one) */
	int refs[] = { 0, 1, 2 };
	int crops[] = { 0, 1 };
	int sizes[][2] = { { 120, 68 }, { 80, 45 }, { 256, 144 }, { 260, 150 }, { 100, 68 } };
	int maxLevels[] = { 0, 42 };
	unsigned i, j, k, l, m, o, q, cases = 0, disagreements = 0;
	for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) for (k = 0; k < 5; k++) for (l = 0; l < 3; l++) for (m = 0; m < 2; m++) for (o = 0; o < 5; o++) for (q = 0; q < 2; q++)
	{
		struct sps_p p = sps_default();
		struct kvmh_check c;
		struct kvm_sps_info si;
		uint8_t b[400];
		size_t n;
		unsigned codedW = (unsigned)sizes[o][0] * 16u, codedH = (unsigned)sizes[o][1] * 16u;
		int agent, mine;
		p.profile = profiles[i]; p.constraint = constraints[j]; p.level = levels[k]; p.refs = refs[l]; p.crop = crops[m]; p.mbsW = sizes[o][0]; p.mbsH = sizes[o][1];
		n = idr_au(b, &p, 0);
		kvmh_init(&c, codedW, codedH, maxLevels[q]);
		kvmh_au(&c, b, n, 1);
		kvmh_finish(&c);
		CHECK(kvm_annexb_sps_info(b, n, &si) == 0);
		agent = kvm_vcore_check_sps(&si, 7u, maxLevels[q], codedW, codedH) != 0;
		mine = c.r[KVMH_R_PROFILE].status == KVMH_FAIL || c.r[KVMH_R_LEVEL].status == KVMH_FAIL || c.r[KVMH_R_FORMAT].status == KVMH_FAIL || c.r[KVMH_R_REFS].status == KVMH_FAIL || c.r[KVMH_R_GEOMETRY].status == KVMH_FAIL;
		cases++;
		if (agent != mine)
		{
			disagreements++;
			if (disagreements < 6) { printf("DISAGREE profile %d constraint 0x%02X level %d refs %d crop %d size %dx%d maxLevel %d: agent %d rules %d\n", p.profile, p.constraint, p.level, p.refs, p.crop, p.mbsW, p.mbsH, maxLevels[q], agent, mine); }
		}
	}
	printf("agent check against the rules: %u SPS variants, %u disagreements\n", cases, disagreements);
	CHECK(disagreements == 0);
}


/* A look at kvm_annexb_sps_info (the agent's own SPS reader; it dropped the second 03 until the fix that follows this module, tests/kvm_annexb_test.c epb_tests): an SPS whose RBSP holds 00 00 03 is written 00 00 03 03, and the reader has to drop only the first 03.
   kvm_annexb.c checks "the two bytes before are 00 00" against what it has already copied, so the second 03 is dropped as well and every field behind it is read from the wrong bit.
   The test checks that none of the generated SPS is misread any more (before the fix 12 of 4800 were) and, should the reader regress, that the rule "parse" notices it. */
static void test_agent_epb_note(void)
{
	unsigned tried = 0, hit = 0, misread = 0, v, spsId;
	for (spsId = 0; spsId < 8; spsId++)
	{
		for (v = 0; v < 600; v++)
		{
			struct bw w;
			uint8_t body[256], o[300];
			size_t n = 0, i;
			struct kvm_sps_info si;
			memset(&w, 0, sizeof(w));
			wbits(&w, 66, 8); wbits(&w, 0xC0, 8); wbits(&w, 42, 8); wue(&w, spsId);
			wue(&w, 4);			/* log2_max_frame_num_minus4 */
			wue(&w, 1);			/* pic_order_cnt_type 1 */
			wbit(&w, 0); wse(&w, 3); wse(&w, -2); wue(&w, 1);
			wse(&w, (int)(4194304u + v * 3571u));	/* offset_for_ref_frame[0]: 22 leading zero bits, then a value that makes 00 00 03 in the RBSP for some v */
			wue(&w, 1); wbit(&w, 0); wue(&w, 119); wue(&w, 67); wbit(&w, 1); wbit(&w, 1); wbit(&w, 0); wbit(&w, 0); wbit(&w, 1);
			memcpy(body, w.b, w.n);
			n = put_nal(o, 0, 1, 3, 7, body, w.n);
			tried++;
			for (i = 0; i + 4 < n; i++) { if (o[i] == 0 && o[i + 1] == 0 && o[i + 2] == 3 && o[i + 3] == 3) { hit++; break; } }
			if (i + 4 < n && (kvm_annexb_sps_info(o, n, &si) != 0 || si.mbsW != 120 || si.mbsH != 68 || si.numRefFrames != 1))
			{
				struct kvmh_check c;
				misread++;
				kvmh_init(&c, 0, 0, 0);
				kvmh_au(&c, o, n, -1);
				CHECK(c.r[KVMH_R_PARSE].fails == 1);	/* the module notices that the agent's reader and its own disagree (rule "parse") */
			}
		}
	}
	printf("agent kvm_annexb_sps_info and 00 00 03 03: %u generated SPS, %u contain 00 00 03 03, %u misread by the agent's reader\n", tried, hit, misread);
	CHECK(hit >= 12 && misread == 0);
}

int main(void)
{
	test_good();
	test_colour();
	test_rules();
	test_split();
	test_fuzz();
	test_agent_agrees();
	test_agent_epb_note();
	printf("kvm_h264_rules_test: %d checks, %d failed\n", g_checks, g_fail);
	return g_fail != 0;
}
