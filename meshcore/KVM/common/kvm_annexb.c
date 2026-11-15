/* KVM: Annex B access unit clean-up. See kvm_annexb.h. */

#include "kvm_annexb.h"

#include <stdlib.h>
#include <string.h>

struct nal { size_t start, hdr, end; int type; };

static int sc_len(const uint8_t *p, size_t left)
{
	if (left >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) { return 4; }
	if (left >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) { return 3; }
	return 0;
}

/* Next NAL unit at or after *pos: [start, end) with its start code; the end is the start of the next start code (or n). 0 at the end.
   A start code with nothing behind it is not a NAL unit. */
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
	o->end = j;
	*pos = j;
	return 1;
}

int kvm_annexb_valid_start(const uint8_t *p, size_t n)
{
	const int sc = (p != NULL) ? sc_len(p, n) : 0;
	return sc != 0 && n > (size_t)sc;
}

int kvm_annexb_sps_ids(const uint8_t *p, size_t n, int *profile_idc, int *level_idc)
{
	size_t pos = 0;
	struct nal nl;
	while (p != NULL && next_nal(p, n, &pos, &nl))
	{
		if (nl.type == 7 && nl.end - nl.hdr >= 4)
		{
			if (profile_idc != NULL) { *profile_idc = p[nl.hdr + 1]; }
			if (level_idc != NULL) { *level_idc = p[nl.hdr + 3]; }
			return 0;
		}
	}
	return -1;
}


/* ---- SPS fields (7.3.2.1.1 of the standard) ---- */
struct bits { const uint8_t *p; size_t n, pos; int err; };	/* RBSP, bit position */

static unsigned bit1(struct bits *b)
{
	unsigned v;
	if ((b->pos >> 3) >= b->n) { b->err = 1; return 0; }
	v = (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1u;
	b->pos++;
	return v;
}

static uint32_t ue(struct bits *b)
{
	int zeros = 0, zeros0;
	uint32_t v = 0;
	while (!b->err && bit1(b) == 0) { if (++zeros > 31) { b->err = 1; return 0; } }
	zeros0 = zeros;
	while (zeros-- > 0 && !b->err) { v = (v << 1) | bit1(b); }
	return b->err ? 0 : ((1u << zeros0) - 1u + v);
}

static int32_t se(struct bits *b)
{
	uint32_t k = ue(b);
	return (k & 1u) ? (int32_t)((k + 1u) >> 1) : -(int32_t)(k >> 1);
}

int kvm_annexb_sps_info(const uint8_t *p, size_t n, struct kvm_sps_info *o)
{
	size_t pos = 0, i, rn = 0;
	int zeros = 0;
	struct nal nl;
	uint8_t *rbsp;
	struct bits b;
	uint32_t v, pocType, k;
	int hi;
	if (o == NULL) { return -1; }
	memset(o, 0, sizeof(*o));
	while (p != NULL && next_nal(p, n, &pos, &nl)) { if (nl.type == 7) { break; } }
	if (p == NULL || nl.type != 7 || nl.end - nl.hdr < 5) { return -1; }
	rbsp = (uint8_t *)malloc(nl.end - nl.hdr);
	if (rbsp == NULL) { return -1; }
	for (i = nl.hdr + 1; i < nl.end; i++)	/* remove the emulation prevention bytes 00 00 03 */
	{
		if (zeros >= 2 && p[i] == 3) { zeros = 0; continue; }	/* the byte after it starts a new count: 00 00 03 03 is 00 00 (data) 03 */
		rbsp[rn++] = p[i];
		zeros = p[i] == 0 ? zeros + 1 : 0;
	}
	b.p = rbsp; b.n = rn; b.pos = 0; b.err = 0;
	o->profile = rbsp[0]; o->constraint = rbsp[1]; o->level = rbsp[2];
	b.pos = 24;
	(void)ue(&b);	/* seq_parameter_set_id */
	hi = o->profile == 100 || o->profile == 110 || o->profile == 122 || o->profile == 244 || o->profile == 44 || o->profile == 83 || o->profile == 86 || o->profile == 118 || o->profile == 128 || o->profile == 138 || o->profile == 139 || o->profile == 134 || o->profile == 135;
	o->chromaFormat = 1; o->bitDepthLuma = 8;
	if (hi)
	{
		o->chromaFormat = (int)ue(&b);
		if (o->chromaFormat == 3) { (void)bit1(&b); }
		o->bitDepthLuma = 8 + (int)ue(&b);
		(void)ue(&b);	/* bit_depth_chroma_minus8 */
		(void)bit1(&b);	/* qpprime_y_zero_transform_bypass_flag */
		if (bit1(&b))	/* seq_scaling_matrix_present_flag: lists that are present are skipped */
		{
			int lists = o->chromaFormat != 3 ? 8 : 12, l;
			for (l = 0; l < lists && !b.err; l++)
			{
				if (bit1(&b))
				{
					int size = l < 6 ? 16 : 64, j, last = 8, next = 8;
					for (j = 0; j < size && !b.err; j++)
					{
						if (next != 0) { next = (last + (int)se(&b) + 256) % 256; }
						last = next == 0 ? last : next;
					}
				}
			}
		}
	}
	(void)ue(&b);	/* log2_max_frame_num_minus4 */
	pocType = ue(&b);
	if (pocType == 0) { (void)ue(&b); }
	else if (pocType == 1)
	{
		(void)bit1(&b); (void)se(&b); (void)se(&b);
		v = ue(&b);
		for (k = 0; k < v && k < 256 && !b.err; k++) { (void)se(&b); }
	}
	o->numRefFrames = (int)ue(&b);
	(void)bit1(&b);	/* gaps_in_frame_num_value_allowed_flag */
	o->mbsW = (int)ue(&b) + 1;
	o->mbsH = (int)ue(&b) + 1;
	o->frameMbsOnly = (int)bit1(&b);
	if (!o->frameMbsOnly) { (void)bit1(&b); o->mbsH *= 2; }
	(void)bit1(&b);	/* direct_8x8_inference_flag */
	o->cropping = (int)bit1(&b);
	free(rbsp);
	return b.err ? -1 : 0;
}

/* Bytes of the NAL units of the wanted kinds in the header buffer (copied to dst when dst != NULL). */
static size_t header_nals(const uint8_t *hdr, size_t hn, int wantSps, int wantPps, uint8_t *dst)
{
	size_t pos = 0, w = 0;
	struct nal nl;
	while (hdr != NULL && next_nal(hdr, hn, &pos, &nl))
	{
		if ((nl.type == 7 && wantSps) || (nl.type == 8 && wantPps))
		{
			if (dst != NULL) { memcpy(dst + w, hdr + nl.start, nl.end - nl.start); }
			w += nl.end - nl.start;
		}
	}
	return w;
}

int kvm_annexb_normalize(const uint8_t *raw, size_t n, const uint8_t *seqhdr, size_t hn, uint8_t *out, size_t cap, size_t *out_len, struct kvm_au_info *info)
{
	struct kvm_au_info li;
	struct nal nl;
	size_t pos = 0, audBytes = 0, extra = 0, total, w = 0;
	int hasSps = 0, hasPps = 0, firstSlice = -1, needSps, needPps, inserted = 0;
	if (info == NULL) { info = &li; }
	memset(info, 0, sizeof(*info));
	info->sps_profile_idc = info->sps_level_idc = -1;
	if (out_len != NULL) { *out_len = 0; }
	if (raw == NULL || out == NULL || out_len == NULL || !kvm_annexb_valid_start(raw, n)) { return KVM_ANNEXB_E_FORMAT; }

	while (next_nal(raw, n, &pos, &nl))
	{
		if (nl.type == 9) { audBytes += nl.end - nl.start; info->aud_removed++; continue; }
		if (nl.type == 7) { hasSps = 1; }
		else if (nl.type == 8) { hasPps = 1; }
		else if (nl.type == 1 || nl.type == 5)
		{
			info->slices++;
			if (firstSlice < 0) { firstSlice = nl.type; }
		}
	}
	if (firstSlice < 0) { return KVM_ANNEXB_E_FORMAT; }
	info->idr = (firstSlice == 5);
	needSps = info->idr && !hasSps;
	needPps = info->idr && !hasPps;
	if (needSps || needPps)
	{
		extra = header_nals(seqhdr, hn, needSps, needPps, NULL);
		/* both kinds that are missing have to be there */
		if (extra == 0 || (needSps && header_nals(seqhdr, hn, 1, 0, NULL) == 0) || (needPps && header_nals(seqhdr, hn, 0, 1, NULL) == 0)) { return KVM_ANNEXB_E_NOHEADER; }
	}
	total = n - audBytes + extra;
	if (total > cap) { *out_len = total; return KVM_ANNEXB_E_SIZE; }

	pos = 0;
	while (next_nal(raw, n, &pos, &nl))
	{
		if (nl.type == 9) { continue; }
		if (extra != 0 && !inserted && (nl.type == 1 || nl.type == 5 || nl.type == 6))
		{
			w += header_nals(seqhdr, hn, needSps, needPps, out + w);
			inserted = 1;
			info->header_added = 1;
		}
		memcpy(out + w, raw + nl.start, nl.end - nl.start);
		w += nl.end - nl.start;
	}
	/* an SPS/PPS that the stream has to get but that has no slice/SEI to go in front of cannot happen (a slice exists), but keep w exact */
	*out_len = w;
	if (info->idr)
	{
		size_t p2 = 0;
		struct nal f;
		if (!next_nal(out, w, &p2, &f) || f.type != 7) { return KVM_ANNEXB_E_ORDER; }
	}
	kvm_annexb_sps_ids(out, w, &info->sps_profile_idc, &info->sps_level_idc);
	return KVM_ANNEXB_OK;
}
