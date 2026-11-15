/* KVM: offline check of an H.264 Annex B stream against the rules of the viewer. See kvm_h264_rules.h. */

#include "kvm_h264_rules.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PS_BYTES 1024
#define MAX_PPS 4
#define MAX_MBS 36864u
#define MAX_SIDE 8192u

static const char *g_ids[KVMH_NRULES] = { "annexb", "slices", "idrflag", "idr_ps", "naltypes", "pssize", "profile", "level", "format", "refs", "geometry", "no_b", "spsstable", "vui", "parse" };
static const char *g_texts[KVMH_NRULES] = {
	"Annex B byte stream with start codes (not length prefixed AVCC)",
	"at least one slice; IDR and non-IDR slices not mixed in one picture",
	"the first picture is an IDR; the IDR flag that was expected matches the slices",
	"an IDR picture has SPS and PPS in front of its first slice",
	"no data partitioning (NAL 2-4) and no SVC / MVC NAL units (14, 15, 20)",
	"SPS and PPS at most 1024 bytes, at most 4 PPS, SPS of one picture identical",
	"profile_idc 66 (only with constraint_set1), 77 or 100",
	"level_idc is a level up to 5.2 (9 = 1b of High) and within the viewer's limit",
	"4:2:0, 8 bit, frame_mbs_only (progressive)",
	"max_num_ref_frames <= 1",
	"no cropping, expected coded size, at most 36864 macroblocks and 8192 per side",
	"no B slices",
	"SPS in a P picture identical to the running one; PPS refers to the SPS in use",
	"VUI bitstream_restriction with num_reorder_frames 0 and max_dec_frame_buffering <= 1 (optional)",
	"SPS, PPS and slice headers parse to the end; both SPS parsers agree" };

const char *kvmh_rule_id(int rule) { return (rule >= 0 && rule < KVMH_NRULES) ? g_ids[rule] : "?"; }
const char *kvmh_rule_text(int rule) { return (rule >= 0 && rule < KVMH_NRULES) ? g_texts[rule] : "?"; }
const char *kvmh_status_name(int status) { return status == KVMH_PASS ? "PASS" : status == KVMH_FAIL ? "FAIL" : status == KVMH_WARN ? "WARN" : "n/a"; }

/* ------------------------------------------------------------------------------------------------------------------ NAL units */

struct nal { size_t start, hdr, end; int type; };

static int sc_len(const uint8_t *p, size_t left)
{
	if (left >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) { return 4; }
	if (left >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) { return 3; }
	return 0;
}

static int next_nal(const uint8_t *p, size_t n, size_t *pos, struct nal *o)
{
	size_t i = *pos, j;
	int sc = 0;
	while (i < n && (sc = sc_len(p + i, n - i)) == 0) { i++; }
	if (i >= n || i + (size_t)sc >= n) { return 0; }
	o->start = i;
	o->hdr = i + (size_t)sc;
	o->type = p[o->hdr] & 0x1F;
	for (j = o->hdr + 1; j < n && sc_len(p + j, n - j) == 0; j++) { }
	/* trailing_zero_8bits of the byte stream belong to no NAL unit */
	o->end = j;
	while (o->end > o->hdr + 1 && p[o->end - 1] == 0) { o->end--; }
	*pos = j;
	return 1;
}

/* ------------------------------------------------------------------------------------------------------------------ bit reader */

struct bits { const uint8_t *p; size_t n, pos; int err; };

static unsigned bit1(struct bits *b)
{
	unsigned v;
	if ((b->pos >> 3) >= b->n) { b->err = 1; return 0; }
	v = (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1u;
	b->pos++;
	return v;
}

static uint32_t bitsn(struct bits *b, int n)
{
	uint32_t v = 0;
	while (n-- > 0) { v = (v << 1) | bit1(b); }
	return v;
}

static uint32_t ue(struct bits *b)
{
	int zeros = 0, z0;
	uint32_t v = 0;
	while (!b->err && bit1(b) == 0) { if (++zeros > 31) { b->err = 1; return 0; } }
	z0 = zeros;
	while (zeros-- > 0 && !b->err) { v = (v << 1) | bit1(b); }
	return b->err ? 0 : ((z0 >= 32 ? 0xFFFFFFFFu : (1u << z0)) - 1u + v);
}

static int32_t se(struct bits *b)
{
	uint32_t k = ue(b);
	return (k & 1u) ? (int32_t)((k + 1u) >> 1) : -(int32_t)(k >> 1);
}

/* RBSP of the NAL unit body behind the header byte (emulation prevention bytes removed); the caller frees. */
static uint8_t *unescape(const uint8_t *p, size_t from, size_t to, size_t *outLen)
{
	size_t i, rn = 0;
	int zeros = 0;
	uint8_t *r = (uint8_t *)malloc(to > from ? to - from : 1);
	if (r == NULL) { return NULL; }
	for (i = from; i < to; i++)
	{
		if (zeros >= 2 && p[i] == 3) { zeros = 0; continue; }	/* the byte after an emulation prevention byte starts a new count (00 00 03 03 keeps its second 03) */
		r[rn++] = p[i];
		zeros = p[i] == 0 ? zeros + 1 : 0;
	}
	*outLen = rn;
	return r;
}

static void skip_hrd(struct bits *b)
{
	uint32_t cnt = ue(b), i;
	(void)bitsn(b, 8);
	if (cnt >= 32) { b->err = 1; return; }
	for (i = 0; i <= cnt && !b->err; i++) { (void)ue(b); (void)ue(b); (void)bit1(b); }
	(void)bitsn(b, 20);
}

/* The fields of the SPS that kvm_annexb_sps_info() does not return (the same walk as parseSpsExtra of the viewer). 0 or -1 (truncated). */
static int sps_extra(const uint8_t *p, const struct nal *nl, struct kvmh_sps_extra *x)
{
	size_t rn = 0;
	uint8_t *r = unescape(p, nl->hdr + 1, nl->end, &rn);
	struct bits b;
	int profile, hi, nal, vcl;
	uint32_t v, k, chroma = 1;
	memset(x, 0, sizeof(*x));
	x->reorder = x->buffering = -1;
	x->primaries = x->transfer = x->matrix = 2;	/* unspecified */
	if (r == NULL || rn < 4) { free(r); return -1; }
	b.p = r; b.n = rn; b.pos = 0; b.err = 0;
	profile = (int)bitsn(&b, 8); (void)bitsn(&b, 16);
	x->spsId = (int)ue(&b);
	hi = profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 || profile == 135;
	if (hi)
	{
		chroma = ue(&b);
		if (chroma == 3) { (void)bit1(&b); }
		(void)ue(&b); (void)ue(&b); (void)bit1(&b);
		if (bit1(&b))
		{
			int lists = chroma != 3 ? 8 : 12, l, j;
			for (l = 0; l < lists && !b.err; l++)
			{
				int size, last = 8, next = 8;
				if (!bit1(&b)) { continue; }
				size = l < 6 ? 16 : 64;
				for (j = 0; j < size && !b.err; j++)
				{
					if (next != 0) { next = (last + (int)se(&b) + 256) % 256; }
					if (next != 0) { last = next; }
				}
			}
		}
	}
	(void)ue(&b);
	x->pocType = (int)ue(&b);
	if (x->pocType == 0) { (void)ue(&b); }
	else if (x->pocType == 1)
	{
		(void)bit1(&b); (void)se(&b); (void)se(&b);
		v = ue(&b);
		if (v > 255) { b.err = 1; }
		for (k = 0; k < v && !b.err; k++) { (void)se(&b); }
	}
	x->refs = (int)ue(&b);
	(void)bit1(&b);
	x->mbsW = (int)ue(&b) + 1;
	x->mbsH = (int)ue(&b) + 1;
	x->frameMbsOnly = (int)bit1(&b);
	if (!x->frameMbsOnly) { (void)bit1(&b); x->mbsH *= 2; }
	(void)bit1(&b);
	x->cropping = (int)bit1(&b);
	if (x->cropping) { (void)ue(&b); (void)ue(&b); (void)ue(&b); (void)ue(&b); }
	if (!b.err && bit1(&b))
	{
		x->vui = 1;
		if (bit1(&b)) { if (bitsn(&b, 8) == 255) { (void)bitsn(&b, 32); } }
		if (bit1(&b)) { (void)bit1(&b); }
		if (bit1(&b))
		{
			x->signal = 1;
			(void)bitsn(&b, 3);
			x->fullRange = (int)bit1(&b);
			if (bit1(&b)) { x->colourDesc = 1; x->primaries = (int)bitsn(&b, 8); x->transfer = (int)bitsn(&b, 8); x->matrix = (int)bitsn(&b, 8); }
		}
		if (bit1(&b)) { (void)ue(&b); (void)ue(&b); }
		if (bit1(&b)) { (void)bitsn(&b, 32); (void)bitsn(&b, 32); (void)bit1(&b); }
		nal = (int)bit1(&b); if (nal) { skip_hrd(&b); }
		vcl = (int)bit1(&b); if (vcl) { skip_hrd(&b); }
		if (nal || vcl) { (void)bit1(&b); }
		(void)bit1(&b);
		if (bit1(&b))
		{
			x->restriction = 1;
			(void)bit1(&b); (void)ue(&b); (void)ue(&b); (void)ue(&b); (void)ue(&b);
			x->reorder = (int)ue(&b);
			x->buffering = (int)ue(&b);
		}
	}
	free(r);
	return b.err ? -1 : 0;
}

/* PPS id and the SPS id it refers to; slice header: first_mb_in_slice, slice_type, pps id. 0 or -1. */
static int pps_ids(const uint8_t *p, const struct nal *nl, int *ppsId, int *spsId)
{
	size_t rn = 0;
	uint8_t *r = unescape(p, nl->hdr + 1, nl->end, &rn);
	struct bits b;
	if (r == NULL || rn < 1) { free(r); return -1; }
	b.p = r; b.n = rn; b.pos = 0; b.err = 0;
	*ppsId = (int)ue(&b);
	*spsId = (int)ue(&b);
	free(r);
	return b.err ? -1 : 0;
}

static int slice_head(const uint8_t *p, const struct nal *nl, uint32_t *firstMb, int *sliceType)
{
	size_t rn = 0;
	uint8_t *r = unescape(p, nl->hdr + 1, nl->end > nl->hdr + 1 + 24 ? nl->hdr + 1 + 24 : nl->end, &rn);	/* the first bytes are enough */
	struct bits b;
	if (r == NULL || rn < 1) { free(r); return -1; }
	b.p = r; b.n = rn; b.pos = 0; b.err = 0;
	*firstMb = ue(&b);
	*sliceType = (int)ue(&b);
	free(r);
	return b.err ? -1 : 0;
}

/* ------------------------------------------------------------------------------------------------------------------ access units */

size_t kvmh_next_au(const uint8_t *p, size_t n, size_t start)
{
	size_t pos = start;
	struct nal nl;
	int slices = 0;
	while (p != NULL && next_nal(p, n, &pos, &nl))
	{
		uint32_t fm = 0;
		int st = 0;
		if (nl.type == 1 || nl.type == 5)
		{
			if (slices > 0 && slice_head(p, &nl, &fm, &st) == 0 && fm == 0) { return nl.start; }
			slices++;
		}
		else if (nl.type == 9 || nl.type == 7 || nl.type == 8 || nl.type == 6) { if (slices > 0) { return nl.start; } }
	}
	return n;
}

void kvmh_au_info(const uint8_t *au, size_t n, struct kvmh_au_info *o)
{
	size_t pos = 0;
	struct nal nl;
	int first = 1;
	memset(o, 0, sizeof(*o));
	o->annexb = au != NULL && kvm_annexb_valid_start(au, n);
	while (au != NULL && next_nal(au, n, &pos, &nl))
	{
		if (nl.type < 32) { o->types |= 1u << nl.type; }
		if (nl.type == 7) { o->nSps++; }
		else if (nl.type == 8) { o->nPps++; }
		else if (nl.type == 1 || nl.type == 5)
		{
			uint32_t fm; int st;
			o->slices++;
			if (first) { o->idr = nl.type == 5; first = 0; }
			if (slice_head(au, &nl, &fm, &st) == 0 && (st % 5 == 1)) { o->bSlices++; }
		}
	}
}

void kvmh_nal_list(const uint8_t *au, size_t n, char *dst, size_t cap)
{
	size_t pos = 0, w = 0;
	struct nal nl;
	int count = 0;
	if (cap > 0) { dst[0] = 0; }
	while (au != NULL && count < 12 && next_nal(au, n, &pos, &nl))
	{
		int r = snprintf(dst + w, cap - w, "%s%d", count ? " " : "", nl.type);
		if (r < 0 || (size_t)r >= cap - w) { break; }
		w += (size_t)r;
		count++;
	}
}

void kvmh_init(struct kvmh_check *c, unsigned codedW, unsigned codedH, int viewerMaxLevel)
{
	memset(c, 0, sizeof(*c));
	c->codedW = codedW; c->codedH = codedH; c->viewerMaxLevel = viewerMaxLevel;
	c->extra.reorder = c->extra.buffering = -1;
}

static int fail_rule(struct kvmh_check *c, int rule, const char *fmt, ...)
{
	struct kvmh_rule *r = &c->r[rule];
	va_list ap;
	r->fails++;
	if (r->first[0] == 0)
	{
		va_start(ap, fmt);
		vsnprintf(r->first, sizeof(r->first), fmt, ap);
		va_end(ap);
	}
	return 1;
}

static int valid_level(int level, int profile)
{
	static const int levels[] = { 10, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41, 42, 50, 51, 52 };
	size_t i;
	for (i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) { if (levels[i] == level) { return 1; } }
	return level == 9 && profile == 100;
}

int kvmh_au(struct kvmh_check *c, const uint8_t *au, size_t n, int wantIdr)
{
	size_t pos = 0, firstSliceAt = (size_t)-1, spsAt = (size_t)-1, ppsAt = (size_t)-1, firstSpsBytes = 0, firstSpsStart = 0;
	struct nal nl;
	int broke = 0, i;
	int nSps = 0, nPps = 0, idr = 0, nonIdr = 0, slices = 0, bad = 0, parseBad = 0, b = 0, sameSps = 1, spsTooBig = 0, ppsTooBig = 0;
	int ppsSid[16], nPpsSid = 0, ppsRef = -1, ppsRefBad = 0;
	int haveSpsHere = 0, agentBad = 0;
	struct kvm_sps_info si;
	struct kvmh_sps_extra ex;
	int r1 = 0, r2 = 0;
	int firstAu = (c->aus == 0);
	int before[KVMH_NRULES];

	for (i = 0; i < KVMH_NRULES; i++) { before[i] = (int)c->r[i].fails; }
	c->aus++;
	if (n > c->auBytesMax) { c->auBytesMax = (unsigned)n; }

	/* annexb */
	c->r[KVMH_R_ANNEXB].checks++;
	if (au == NULL || !kvm_annexb_valid_start(au, n))
	{
		const unsigned long len = (au != NULL && n >= 4) ? (((unsigned long)au[0] << 24) | ((unsigned long)au[1] << 16) | ((unsigned long)au[2] << 8) | au[3]) : 0ul;
		fail_rule(c, KVMH_R_ANNEXB, "no start code at the start (first bytes %02X %02X %02X %02X%s)", n > 0 ? au[0] : 0, n > 1 ? au[1] : 0, n > 2 ? au[2] : 0, n > 3 ? au[3] : 0,
			(len + 4 == n || (len > 0 && len + 4 < n)) ? ", looks like a length prefix (AVCC)" : "");
		return 1;
	}

	while (next_nal(au, n, &pos, &nl))
	{
		switch (nl.type)
		{
			case 7:
				nSps++;
				if (nl.end - nl.hdr > MAX_PS_BYTES) { spsTooBig = 1; }
				if (nSps == 1)
				{
					firstSpsStart = nl.hdr; firstSpsBytes = nl.end - nl.hdr; spsAt = nl.start;
					r1 = kvm_annexb_sps_info(au + nl.start, nl.end - nl.start, &si);
					r2 = sps_extra(au, &nl, &ex);
					if (r1 == 0 && r2 == 0) { haveSpsHere = 1; }
					else { parseBad = 1; if (r1 != 0 && r2 == 0) { agentBad = 1; } }
				}
				else if (nl.end - nl.hdr != firstSpsBytes || memcmp(au + nl.hdr, au + firstSpsStart, firstSpsBytes) != 0) { sameSps = 0; }
				break;
			case 8:
			{
				int pid = 0, sid = 0;
				nPps++;
				if (nl.end - nl.hdr > MAX_PS_BYTES) { ppsTooBig = 1; }
				if (ppsAt == (size_t)-1) { ppsAt = nl.start; }
				if (pps_ids(au, &nl, &pid, &sid) != 0) { parseBad = 1; }
				else if (nPpsSid < 16) { ppsSid[nPpsSid++] = sid; }
				break;
			}
			case 5: idr = 1; slices++; if (firstSliceAt == (size_t)-1) { firstSliceAt = nl.start; }
			/* fall through */
			case 1:
			{
				uint32_t fm = 0; int st = 0;
				if (nl.type == 1) { nonIdr = 1; slices++; if (firstSliceAt == (size_t)-1) { firstSliceAt = nl.start; } }
				if (slice_head(au, &nl, &fm, &st) != 0) { parseBad = 1; }
				else if (st % 5 == 1) { b++; }
				break;
			}
			case 2: case 3: case 4: case 14: case 15: case 20: bad++; break;
			default: break;
		}
	}

	/* slices, idrflag */
	c->r[KVMH_R_SLICES].checks++;
	if (slices == 0) { fail_rule(c, KVMH_R_SLICES, "no slice NAL unit in the access unit"); }
	else if (idr && nonIdr) { fail_rule(c, KVMH_R_SLICES, "IDR and non-IDR slices in one picture"); }
	c->r[KVMH_R_IDRFLAG].checks++;
	if (slices > 0)
	{
		if (firstAu && !idr) { fail_rule(c, KVMH_R_IDRFLAG, "the first picture is not an IDR"); }
		else if (wantIdr >= 0 && (wantIdr != 0) != (idr != 0)) { fail_rule(c, KVMH_R_IDRFLAG, "expected %s picture, the slices are %s", wantIdr ? "an IDR" : "a non-IDR", idr ? "IDR" : "non-IDR"); }
	}
	/* idr_ps */
	if (idr)
	{
		c->r[KVMH_R_IDR_PS].checks++;
		if (nSps == 0 || nPps == 0) { fail_rule(c, KVMH_R_IDR_PS, "IDR picture without %s", nSps == 0 && nPps == 0 ? "SPS and PPS" : nSps == 0 ? "SPS" : "PPS"); }
		else if (spsAt > firstSliceAt || ppsAt > firstSliceAt) { fail_rule(c, KVMH_R_IDR_PS, "SPS or PPS behind the first slice"); }
		c->idrs++;
	}
	else { c->pAus++; }
	/* naltypes */
	c->r[KVMH_R_NALTYPES].checks++;
	if (bad) { fail_rule(c, KVMH_R_NALTYPES, "%d NAL unit(s) of a type that the viewer refuses (2-4, 14, 15, 20)", bad); }
	/* pssize */
	c->r[KVMH_R_PSSIZE].checks++;
	if (spsTooBig || ppsTooBig) { fail_rule(c, KVMH_R_PSSIZE, "%s larger than %d bytes", spsTooBig ? "SPS" : "PPS", MAX_PS_BYTES); }
	else if (nPps > MAX_PPS) { fail_rule(c, KVMH_R_PSSIZE, "%d PPS in one picture (limit %d)", nPps, MAX_PPS); }
	else if (!sameSps) { fail_rule(c, KVMH_R_PSSIZE, "several different SPS in one picture"); }
	/* no_b */
	c->r[KVMH_R_NOB].checks++;
	if (b > 0) { fail_rule(c, KVMH_R_NOB, "%d B slice(s) (slice_type 1 or 6)", b); c->bSlices += (unsigned)b; }
	/* parse */
	c->r[KVMH_R_PARSE].checks++;
	if (agentBad) { fail_rule(c, KVMH_R_PARSE, "kvm_annexb_sps_info (the agent's SPS reader) fails on an SPS that parses to the end (00 00 03 03 in the RBSP?)"); }
	else if (parseBad) { fail_rule(c, KVMH_R_PARSE, "an SPS, PPS or slice header is truncated or malformed"); }

	if (nSps > 0) { c->spsSeen += (unsigned)nSps; }
	if (nPps > 0) { c->ppsSeen += (unsigned)nPps; }

	if (haveSpsHere)
	{
		const int level = si.level;
		/* the SPS of this access unit, judged on its fields */
		if (si.numRefFrames != ex.refs || si.frameMbsOnly != ex.frameMbsOnly || si.mbsW != ex.mbsW || si.mbsH != ex.mbsH || si.cropping != ex.cropping)
		{
			fail_rule(c, KVMH_R_PARSE, "the two SPS parsers disagree (refs %d / %d, size %dx%d / %dx%d macroblocks, cropping %d / %d)", si.numRefFrames, ex.refs, si.mbsW, si.mbsH, ex.mbsW, ex.mbsH, si.cropping, ex.cropping);
		}
		c->r[KVMH_R_PROFILE].checks++;
		if (si.profile != 66 && si.profile != 77 && si.profile != 100) { fail_rule(c, KVMH_R_PROFILE, "profile_idc %d", si.profile); }
		else if (si.profile == 66 && (si.constraint & 0x40) == 0) { fail_rule(c, KVMH_R_PROFILE, "Baseline without constraint_set1 (flags 0x%02X): FMO and ASO are not excluded", si.constraint); }
		c->r[KVMH_R_LEVEL].checks++;
		if (!valid_level(level, si.profile)) { fail_rule(c, KVMH_R_LEVEL, "level_idc %d is no level of the standard up to 5.2", level); }
		else if (c->viewerMaxLevel > 0 && level > c->viewerMaxLevel) { fail_rule(c, KVMH_R_LEVEL, "level_idc %d is above the viewer's limit %d", level, c->viewerMaxLevel); }
		c->r[KVMH_R_FORMAT].checks++;
		if (si.chromaFormat != 1) { fail_rule(c, KVMH_R_FORMAT, "chroma_format_idc %d (4:2:0 needed)", si.chromaFormat); }
		else if (si.bitDepthLuma != 8) { fail_rule(c, KVMH_R_FORMAT, "luma bit depth %d (8 needed)", si.bitDepthLuma); }
		else if (!si.frameMbsOnly) { fail_rule(c, KVMH_R_FORMAT, "frame_mbs_only_flag 0 (interlaced coding allowed)"); }
		c->r[KVMH_R_REFS].checks++;
		if (si.numRefFrames > 1) { fail_rule(c, KVMH_R_REFS, "max_num_ref_frames %d", si.numRefFrames); }
		c->r[KVMH_R_GEOMETRY].checks++;
		if (si.cropping) { fail_rule(c, KVMH_R_GEOMETRY, "frame_cropping_flag 1 (the viewer rejects cropping)"); }
		else if (si.mbsW < 1 || si.mbsH < 1 || (unsigned)si.mbsW * 16u > MAX_SIDE || (unsigned)si.mbsH * 16u > MAX_SIDE) { fail_rule(c, KVMH_R_GEOMETRY, "size %dx%d macroblocks out of range", si.mbsW, si.mbsH); }
		else if ((unsigned long)si.mbsW * (unsigned long)si.mbsH > MAX_MBS) { fail_rule(c, KVMH_R_GEOMETRY, "%lu macroblocks (limit %u of level 5.2)", (unsigned long)si.mbsW * (unsigned long)si.mbsH, MAX_MBS); }
		else if (c->codedW != 0 && c->codedH != 0 && ((unsigned)si.mbsW * 16u != c->codedW || (unsigned)si.mbsH * 16u != c->codedH))
		{
			fail_rule(c, KVMH_R_GEOMETRY, "coded size %dx%d, expected %ux%u", si.mbsW * 16, si.mbsH * 16, c->codedW, c->codedH);
		}
		c->r[KVMH_R_VUI].checks++;
		if (!(ex.restriction && ex.reorder == 0 && ex.buffering >= 0 && ex.buffering <= 1))
		{
			fail_rule(c, KVMH_R_VUI, ex.restriction ? "reorder %d, max_dec_frame_buffering %d" : "no bitstream_restriction in the VUI (viewer's decoder may buffer frames)", ex.reorder, ex.buffering);
		}
		/* running SPS: the SPS of an IDR picture replaces it; in a P picture it has to be the same */
		c->r[KVMH_R_SPSSTABLE].checks++;
		if (idr)
		{
			if (c->haveRun && (firstSpsBytes != c->runLen || memcmp(au + firstSpsStart, c->run, c->runLen) != 0)) { c->spsChanges++; }
			if (firstSpsBytes <= sizeof(c->run)) { memcpy(c->run, au + firstSpsStart, firstSpsBytes); c->runLen = firstSpsBytes; c->haveRun = 1; c->runSpsId = ex.spsId; }
		}
		else if (!c->haveRun || firstSpsBytes != c->runLen || memcmp(au + firstSpsStart, c->run, c->runLen) != 0)
		{
			fail_rule(c, KVMH_R_SPSSTABLE, "an SPS in a non-IDR picture that differs from the running one");
		}
		c->sps = si; c->extra = ex; c->haveSps = 1;
	}
	if (nPpsSid > 0 && (haveSpsHere || c->haveRun))
	{
		const int want = haveSpsHere ? ex.spsId : c->runSpsId;
		for (i = 0; i < nPpsSid; i++) { if (ppsSid[i] != want) { ppsRefBad = 1; ppsRef = ppsSid[i]; } }
		if (!haveSpsHere) { c->r[KVMH_R_SPSSTABLE].checks++; }
		if (ppsRefBad) { fail_rule(c, KVMH_R_SPSSTABLE, "a PPS refers to SPS id %d, the SPS in use has id %d", ppsRef, want); }
	}
	for (i = 0; i < KVMH_NRULES; i++) { if ((int)c->r[i].fails > before[i]) { broke++; } }
	return broke;
}

int kvmh_finish(struct kvmh_check *c)
{
	int i, fails = 0;
	for (i = 0; i < KVMH_NRULES; i++)
	{
		struct kvmh_rule *r = &c->r[i];
		if (r->checks == 0) { r->status = KVMH_NOTRUN; }
		else if (r->fails == 0) { r->status = KVMH_PASS; }
		else if (i == KVMH_R_VUI) { r->status = KVMH_WARN; }
		else { r->status = KVMH_FAIL; fails++; }
	}
	return fails;
}

int kvmh_colour_verdict(const struct kvmh_sps_extra *x)
{
	if (x == NULL || !x->vui || !x->signal) { return KVMH_COLOUR_UNSPECIFIED; }
	if (x->fullRange) { return KVMH_COLOUR_WRONG; }
	if (!x->colourDesc) { return KVMH_COLOUR_UNSPECIFIED; }
	/* The pictures are BT.709 limited range (kvm_yuv.h). Table E-3 / E-4 / E-5: colour_primaries and transfer_characteristics 0 are RESERVED and 2 is unspecified; the AMD encoder of the AMD laptop writes 0 / 0 / 2
	   (measured) and the viewer sets the colour space of its decoder itself (bt709, limited), so those say nothing and the SPS is allowed (only noted). matrix_coefficients 0 is NOT reserved: it is the
	   identity matrix (GBR, RGB coded as it is), and a decoder that listens to it shows the YUV picture as GBR: refused. Any other value is a colour space that the viewer would not use: refused. */
	if (x->primaries > 2 || x->transfer > 2 || x->matrix > 2 || x->matrix == 0) { return KVMH_COLOUR_WRONG; }
	return (x->primaries == 1 && x->transfer == 1 && x->matrix == 1) ? KVMH_COLOUR_BT709 : KVMH_COLOUR_UNSPECIFIED;
}
