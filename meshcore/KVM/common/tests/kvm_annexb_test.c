/* Tests of kvm_annexb.c on synthetic access units: removal of access unit delimiters, SPS/PPS insertion, order rule, buffer size, damaged input. */
#define _POSIX_C_SOURCE 200809L

#include "../kvm_annexb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* appends a NAL unit (start code of 3 or 4 bytes, header byte of the type, len payload bytes without zeros) */
static size_t add(uint8_t *b, size_t n, int sc4, int type, int len)
{
	int i;
	if (sc4) { b[n++] = 0; }
	b[n++] = 0; b[n++] = 0; b[n++] = 1;
	b[n++] = (uint8_t)(0x60 | type);
	for (i = 0; i < len; i++) { b[n++] = (uint8_t)(0x80 + i); }
	return n;
}

/* NAL types of a buffer as a string of characters ('A' + type for 10..31, '0' + type below 10) */
static void types(const uint8_t *p, size_t n, char *o)
{
	size_t i;
	int c = 0;
	for (i = 0; i + 3 < n; i++)
	{
		if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1)
		{
			const int t = p[i + 3] & 0x1F;
			o[c++] = (char)(t < 10 ? '0' + t : 'A' + t - 10);
			i += 3;
		}
	}
	o[c] = 0;
}


/* ---- kvm_annexb_sps_info: SPS built with a bit writer, so every field is known ---- */
struct bw { uint8_t b[128]; size_t n; int bit; };
static void wbit(struct bw *w, unsigned v) { if (w->bit == 0) { w->b[w->n++] = 0; } if (v) { w->b[w->n - 1] |= (uint8_t)(0x80u >> w->bit); } w->bit = (w->bit + 1) & 7; }
static void wbits(struct bw *w, unsigned v, int n) { while (n-- > 0) { wbit(w, (v >> n) & 1u); } }
static void wue(struct bw *w, unsigned v) { unsigned x = v + 1; int len = 0; while ((x >> len) > 1) { len++; } wbits(w, 0, len); wbits(w, x, len + 1); }
static void wse(struct bw *w, int v) { wue(w, v > 0 ? (unsigned)(2 * v - 1) : (unsigned)(-2 * v)); }

/* Annex B SPS (with an emulation prevention byte after every 00 00 that is followed by <= 3) */
static size_t make_sps(uint8_t *out, int profile, int constraint, int level, int refs, int mbsW, int mbsH, int crop, int scaling, int poc1)
{
	struct bw w;
	size_t i, n = 0;
	memset(&w, 0, sizeof(w));
	wbits(&w, 0x67, 8); wbits(&w, (unsigned)profile, 8); wbits(&w, (unsigned)constraint, 8); wbits(&w, (unsigned)level, 8);
	wue(&w, 0);
	if (profile == 100)
	{
		wue(&w, 1); wue(&w, 0); wue(&w, 0); wbit(&w, 0);
		wbit(&w, scaling);
		if (scaling) { int l; for (l = 0; l < 8; l++) { if (l == 1) { int j; wbit(&w, 1); for (j = 0; j < 16; j++) { wse(&w, j == 0 ? 2 : 0); } } else { wbit(&w, 0); } } }
	}
	wue(&w, 4);
	if (poc1) { wue(&w, 1); wbit(&w, 0); wse(&w, -3); wse(&w, 5); wue(&w, 2); wse(&w, 1); wse(&w, -1); }
	else { wue(&w, 0); wue(&w, 6); }
	wue(&w, (unsigned)refs); wbit(&w, 0);
	wue(&w, (unsigned)mbsW - 1); wue(&w, (unsigned)mbsH - 1); wbit(&w, 1); wbit(&w, 1);
	wbit(&w, crop);
	if (crop) { wue(&w, 0); wue(&w, 0); wue(&w, 0); wue(&w, 4); }
	wbit(&w, 0);	/* vui_parameters_present_flag */
	wbit(&w, 1);	/* rbsp stop bit */
	out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 1;
	out[n++] = w.b[0];
	for (i = 1; i < w.n; i++)
	{
		if (n >= 7 && out[n - 1] == 0 && out[n - 2] == 0 && w.b[i] <= 3) { out[n++] = 3; }
		out[n++] = w.b[i];
	}
	return n;
}

/* An SPS whose RBSP holds 00 00 03 is written 00 00 03 03: the reader removes only the first 03 (the second one is data and starts a new count of zeros). offset_for_ref_frame[0] of picture
   order count type 1 is a value of 22 leading zero bits, so that for some values the RBSP has 00 00 03 before the fields that are read (size, reference frames, cropping). Before the
   fix of kvm_annexb_sps_info the 12 SPS of this kind in the range below were misread. */
static size_t make_sps_epb(uint8_t *out, unsigned spsId, unsigned v)
{
	struct bw w;
	size_t i, n = 0;
	memset(&w, 0, sizeof(w));
	wbits(&w, 0x67, 8); wbits(&w, 66, 8); wbits(&w, 0xC0, 8); wbits(&w, 42, 8);
	wue(&w, spsId);
	wue(&w, 4); wue(&w, 1); wbit(&w, 0); wse(&w, 3); wse(&w, -2); wue(&w, 1);
	wse(&w, (int)(4194304u + v * 3571u));
	wue(&w, 1); wbit(&w, 0); wue(&w, 119); wue(&w, 67); wbit(&w, 1); wbit(&w, 1); wbit(&w, 0); wbit(&w, 0); wbit(&w, 1);
	out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 1;
	out[n++] = w.b[0];
	for (i = 1; i < w.n; i++)
	{
		if (n >= 7 && out[n - 1] == 0 && out[n - 2] == 0 && w.b[i] <= 3) { out[n++] = 3; }
		out[n++] = w.b[i];
	}
	return n;
}

static void epb_tests(void)
{
	unsigned spsId, v, hit = 0, bad = 0;
	for (spsId = 0; spsId < 8; spsId++)
	{
		for (v = 0; v < 600; v++)
		{
			uint8_t b[160];
			size_t n = make_sps_epb(b, spsId, v), i;
			struct kvm_sps_info s;
			int has0303 = 0;
			for (i = 0; i + 4 < n; i++) { if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 3 && b[i + 3] == 3) { has0303 = 1; } }
			if (!has0303) { continue; }
			hit++;
			if (kvm_annexb_sps_info(b, n, &s) != 0 || s.mbsW != 120 || s.mbsH != 68 || s.numRefFrames != 1 || s.frameMbsOnly != 1 || s.cropping != 0) { bad++; }
		}
	}
	CHECK(hit >= 12);	/* the range holds SPS with 00 00 03 03 */
	CHECK(bad == 0);
}

static void sps_tests(void)
{
	uint8_t b[160];
	struct kvm_sps_info s;
	size_t n, k;
	n = make_sps(b, 66, 0xC0, 42, 1, 120, 68, 0, 0, 0);
	CHECK(kvm_annexb_sps_info(b, n, &s) == 0 && s.profile == 66 && s.constraint == 0xC0 && s.level == 42 && s.numRefFrames == 1 && s.mbsW == 120 && s.mbsH == 68 && s.frameMbsOnly == 1 && s.cropping == 0);
	n = make_sps(b, 77, 0x40, 40, 2, 90, 48, 0, 0, 0);
	CHECK(kvm_annexb_sps_info(b, n, &s) == 0 && s.profile == 77 && s.constraint == 0x40 && s.numRefFrames == 2 && s.mbsW == 90 && s.mbsH == 48);
	n = make_sps(b, 100, 0, 51, 1, 160, 90, 1, 0, 0);
	CHECK(kvm_annexb_sps_info(b, n, &s) == 0 && s.profile == 100 && s.level == 51 && s.numRefFrames == 1 && s.mbsW == 160 && s.mbsH == 90 && s.cropping == 1 && s.chromaFormat == 1 && s.bitDepthLuma == 8);
	n = make_sps(b, 100, 0, 51, 3, 160, 90, 0, 1, 1);	/* a scaling list and picture order count type 1: the fields behind them are still found */
	CHECK(kvm_annexb_sps_info(b, n, &s) == 0 && s.numRefFrames == 3 && s.mbsW == 160 && s.mbsH == 90);
	n = make_sps(b, 66, 0xC0, 31, 4, 32, 32, 0, 0, 1);
	CHECK(kvm_annexb_sps_info(b, n, &s) == 0 && s.numRefFrames == 4 && s.mbsW == 32 && s.mbsH == 32);
	/* emulation prevention: a size that makes 00 00 0x appear (mbsW 1 gives a long run of zero bits) */
	n = make_sps(b, 66, 0, 10, 0, 1, 1, 0, 0, 0);
	CHECK(kvm_annexb_sps_info(b, n, &s) == 0 && s.mbsW == 1 && s.mbsH == 1 && s.numRefFrames == 0);
	/* behind other NAL units, with an AUD first */
	{
		uint8_t c[200]; size_t m = 0, q;
		static const uint8_t aud[] = { 0, 0, 0, 1, 9, 0xF0 };
		memcpy(c, aud, sizeof(aud)); m = sizeof(aud);
		q = make_sps(b, 100, 0, 42, 1, 120, 68, 0, 0, 0);
		memcpy(c + m, b, q); m += q;
		CHECK(kvm_annexb_sps_info(c, m, &s) == 0 && s.profile == 100 && s.level == 42 && s.mbsW == 120);
	}
	/* the SPS that the Microsoft encoder wrote at 1440x832 on Windows 10 19044 (smoke of the first video path, High and Constrained Baseline): 90 x 52 macroblocks, one reference frame, no cropping */
	{
		static const uint8_t high[] = { 0, 0, 0, 1, 0x67, 0x64, 0x00, 0x2a, 0xac, 0x2b, 0x40, 0x2d, 0x03, 0x4d, 0x80, 0x88, 0x00, 0x00, 0x03, 0x00, 0x08, 0x00, 0x00, 0x03, 0x01, 0xe0, 0x6d, 0x04, 0x23, 0x50, 0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x30 };
		static const uint8_t cbp[] = { 0, 0, 0, 1, 0x67, 0x42, 0xc0, 0x2a, 0x95, 0xa0, 0x16, 0x81, 0xa6, 0xc0, 0x44, 0x00, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0xf0, 0x36, 0x82, 0x21, 0x1a, 0x80, 0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80 };
		CHECK(kvm_annexb_sps_info(high, sizeof(high), &s) == 0 && s.profile == 100 && s.constraint == 0 && s.level == 42 && s.mbsW == 90 && s.mbsH == 52 && s.numRefFrames == 1 && s.frameMbsOnly == 1 && s.cropping == 0);
		CHECK(kvm_annexb_sps_info(cbp, sizeof(cbp), &s) == 0 && s.profile == 66 && s.constraint == 0xC0 && s.level == 42 && s.mbsW == 90 && s.mbsH == 52 && s.numRefFrames == 1 && s.cropping == 0);
	}
	/* no SPS, cut short at every length: never a crash, never success with garbage fields */
	CHECK(kvm_annexb_sps_info((const uint8_t *)"\0\0\0\1\11\360", 6, &s) == -1);
	n = make_sps(b, 100, 0, 51, 3, 160, 90, 0, 1, 1);
	for (k = 0; k < n; k++) { int r = kvm_annexb_sps_info(b, k, &s); CHECK(r == 0 || r == -1); }
	CHECK(kvm_annexb_sps_info(NULL, 0, &s) == -1);
	CHECK(kvm_annexb_sps_info(b, n, NULL) == -1);
}

int main(void)
{
	sps_tests();
	epb_tests();
	uint8_t raw[1024], hdr[128], out[2048];
	size_t n, hn = 0, ol, i;
	struct kvm_au_info in;
	char t[64];
	int r;

	hn = add(hdr, hn, 1, 7, 5); hn = add(hdr, hn, 1, 8, 2);
	hdr[5] = 100; hdr[7] = 40; /* SPS: profile_idc 100, level_idc 40 (bytes 1 and 3 after the NAL header) */

	/* what the Microsoft encoder gives: IDR = AUD SPS PPS SEI SEI slice slice; P = AUD slice slice */
	n = 0; n = add(raw, n, 1, 9, 1); n = add(raw, n, 1, 7, 5); n = add(raw, n, 1, 8, 2); n = add(raw, n, 0, 6, 4); n = add(raw, n, 0, 6, 3); n = add(raw, n, 0, 5, 20); n = add(raw, n, 0, 5, 18);
	raw[11] = 100; raw[13] = 40; /* the SPS of this AU: profile_idc and level_idc */
	r = kvm_annexb_normalize(raw, n, hdr, hn, out, sizeof(out), &ol, &in);
	types(out, ol, t);
	CHECK(r == 0 && strcmp(t, "78665" "5") == 0 && in.idr == 1 && in.aud_removed == 1 && in.header_added == 0 && in.slices == 2 && ol < n);
	CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 1 && (out[4] & 0x1F) == 7);

	n = 0; n = add(raw, n, 1, 9, 1); n = add(raw, n, 0, 1, 30); n = add(raw, n, 0, 1, 10);
	r = kvm_annexb_normalize(raw, n, hdr, hn, out, sizeof(out), &ol, &in);
	types(out, ol, t);
	CHECK(r == 0 && strcmp(t, "11") == 0 && in.idr == 0 && in.aud_removed == 1 && ol == n - 6);
	CHECK(out[0] == 0 && out[1] == 0 && out[2] == 1 && (out[3] & 0x1F) == 1); /* starts with the slice (3 byte start code kept) */

	/* AUD in the middle and at the end, several */
	n = 0; n = add(raw, n, 0, 1, 7); n = add(raw, n, 1, 9, 1); n = add(raw, n, 0, 1, 7); n = add(raw, n, 0, 9, 1);
	r = kvm_annexb_normalize(raw, n, NULL, 0, out, sizeof(out), &ol, &in);
	types(out, ol, t);
	CHECK(r == 0 && strcmp(t, "11") == 0 && in.aud_removed == 2);

	/* IDR without parameter sets: they are put in front of the first SEI or slice, behind the AUD (which is removed) */
	n = 0; n = add(raw, n, 1, 9, 1); n = add(raw, n, 0, 6, 4); n = add(raw, n, 0, 5, 9);
	r = kvm_annexb_normalize(raw, n, hdr, hn, out, sizeof(out), &ol, &in);
	types(out, ol, t);
	CHECK(r == 0 && strcmp(t, "7865") == 0 && in.header_added == 1 && in.sps_profile_idc == 100 && in.sps_level_idc == 40 && ol == n - 6 + hn);
	n = 0; n = add(raw, n, 1, 5, 20);
	r = kvm_annexb_normalize(raw, n, hdr, hn, out, sizeof(out), &ol, &in);
	types(out, ol, t);
	CHECK(r == 0 && strcmp(t, "785") == 0 && memcmp(out, hdr, hn) == 0 && memcmp(out + hn, raw, n) == 0);

	/* SPS present, PPS missing: only the PPS is added; the result starts with the SPS */
	n = 0; n = add(raw, n, 1, 7, 5); n = add(raw, n, 1, 5, 20);
	r = kvm_annexb_normalize(raw, n, hdr, hn, out, sizeof(out), &ol, &in);
	types(out, ol, t);
	CHECK(r == 0 && strcmp(t, "785") == 0 && in.header_added == 1);

	/* no header to give */
	n = 0; n = add(raw, n, 1, 5, 20);
	CHECK(kvm_annexb_normalize(raw, n, NULL, 0, out, sizeof(out), &ol, &in) == KVM_ANNEXB_E_NOHEADER);
	{
		uint8_t onlySps[32]; size_t sn = 0;
		sn = add(onlySps, sn, 1, 7, 5);
		CHECK(kvm_annexb_normalize(raw, n, onlySps, sn, out, sizeof(out), &ol, &in) == KVM_ANNEXB_E_NOHEADER); /* PPS is needed too */
	}

	/* IDR that has an SEI in front of its SPS: the order rule */
	n = 0; n = add(raw, n, 1, 6, 4); n = add(raw, n, 1, 7, 5); n = add(raw, n, 1, 8, 2); n = add(raw, n, 1, 5, 20);
	CHECK(kvm_annexb_normalize(raw, n, hdr, hn, out, sizeof(out), &ol, &in) == KVM_ANNEXB_E_ORDER);

	/* too small output: the needed size is reported and nothing is written */
	n = 0; n = add(raw, n, 1, 9, 1); n = add(raw, n, 1, 5, 20);
	memset(out, 0xEE, sizeof(out));
	r = kvm_annexb_normalize(raw, n, hdr, hn, out, 10, &ol, &in);
	CHECK(r == KVM_ANNEXB_E_SIZE && ol == n - 6 + hn && out[0] == 0xEE);
	r = kvm_annexb_normalize(raw, n, hdr, hn, out, ol, &ol, &in); /* exactly the needed size works */
	CHECK(r == 0);

	/* damaged input: not Annex B (a length prefix), no slice, start code at the very end, empty */
	n = 0; raw[n++] = 0; raw[n++] = 0; raw[n++] = 0; raw[n++] = 40; raw[n++] = 0x65; raw[n++] = 1; raw[n++] = 2;
	CHECK(kvm_annexb_normalize(raw, n, hdr, hn, out, sizeof(out), &ol, &in) == KVM_ANNEXB_E_FORMAT);
	n = 0; n = add(raw, n, 1, 9, 1); n = add(raw, n, 1, 7, 5);
	CHECK(kvm_annexb_normalize(raw, n, hdr, hn, out, sizeof(out), &ol, &in) == KVM_ANNEXB_E_FORMAT);
	n = 0; n = add(raw, n, 1, 1, 5); raw[n++] = 0; raw[n++] = 0; raw[n++] = 0; raw[n++] = 1; /* start code at the end: not a NAL unit, ignored */
	r = kvm_annexb_normalize(raw, n, hdr, hn, out, sizeof(out), &ol, &in);
	CHECK(r == 0 || r == KVM_ANNEXB_E_FORMAT);
	CHECK(kvm_annexb_normalize(raw, 0, hdr, hn, out, sizeof(out), &ol, &in) == KVM_ANNEXB_E_FORMAT);
	CHECK(kvm_annexb_normalize(NULL, 5, hdr, hn, out, sizeof(out), &ol, &in) == KVM_ANNEXB_E_FORMAT);
	CHECK(kvm_annexb_valid_start(raw, 0) == 0);

	/* sps ids */
	{
		int p = 0, l = 0;
		CHECK(kvm_annexb_sps_ids(hdr, hn, &p, &l) == 0 && p == 100 && l == 40);
		CHECK(kvm_annexb_sps_ids(raw, 3, &p, &l) == -1);
	}

	/* fuzz: random bytes with random start codes must not crash, overrun (run it under ASan) or write more than cap */
	{
		uint32_t s = 12345;
		int k;
		for (k = 0; k < 20000; k++)
		{
			size_t len, cap, o2;
			uint8_t *b, *o;
			s ^= s << 13; s ^= s >> 17; s ^= s << 5;
			len = s % 90;
			b = (uint8_t *)malloc(len + 1);
			for (i = 0; i < len; i++)
			{
				s ^= s << 13; s ^= s >> 17; s ^= s << 5;
				b[i] = (s & 3) == 0 ? 0 : ((s & 7) == 1 ? 1 : (uint8_t)(s >> 8));
			}
			s ^= s << 13; s ^= s >> 17; s ^= s << 5;
			cap = s % 160;
			o = (uint8_t *)malloc(cap + 1);
			r = kvm_annexb_normalize(b, len, hdr, hn, o, cap, &o2, &in);
			if (r == 0) { CHECK(o2 <= cap && o2 > 0); }
			free(b); free(o);
		}
	}

	printf("checks: %d, failures: %d\n%s\n", g_checks, g_fail, g_fail ? "RESULT: FAILED" : "RESULT: OK");
	return g_fail ? 1 : 0;
}
