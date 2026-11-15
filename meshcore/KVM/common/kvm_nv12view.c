/* KVM: the NV12 view of the encoder, see kvm_nv12view.h. Portable C99. */

#include "kvm_nv12view.h"
#include "kvm_yuv.h"

#include <stdlib.h>
#include <string.h>

int kvm_nv12v_init(kvm_nv12v *v, int codedW, int codedH, int frozen, int flat, int firstFull)
{
	if (v == NULL || codedW < 32 || codedH < 32 || (codedW & 31) || (codedH & 31) || codedW > 16384 || codedH > 16384) { return -1; }
	memset(v, 0, sizeof(*v));
	v->codedW = codedW; v->codedH = codedH; v->mbCols = codedW / KVM_NV12V_MB; v->mbRows = codedH / KVM_NV12V_MB;
	v->frozen = (frozen || flat) ? 1 : 0; v->flat = flat ? 1 : 0; v->firstFull = firstFull ? 1 : 0;
	v->total = (unsigned)v->mbCols * (unsigned)v->mbRows;
	v->touched = (uint8_t *)calloc(v->total, 1);
	v->map = (uint8_t *)calloc(v->total, 1);
	v->tmp = (uint8_t *)calloc(v->total, 1);
	v->base = (uint8_t *)calloc(v->total, 1);
	if (v->touched == NULL || v->map == NULL || v->tmp == NULL || v->base == NULL) { kvm_nv12v_free(v); return -1; }
	return 0;
}

void kvm_nv12v_free(kvm_nv12v *v)
{
	if (v == NULL) { return; }
	free(v->touched); free(v->map); free(v->tmp); free(v->base);
	memset(v, 0, sizeof(*v));
}

void kvm_nv12v_invalidate(kvm_nv12v *v)
{
	if (v == NULL) { return; }
	v->valid = 0;
	if (v->touched != NULL) { memset(v->touched, 0, v->total); }
}

/* the macroblocks of the tile (r, c) are set in `m` */
static void mark_tile(const kvm_nv12v *v, uint8_t *m, int r, int c)
{
	int dr, dc;
	for (dr = 0; dr < KVM_NV12V_TILE / KVM_NV12V_MB; dr++)
	{
		const int mr = r * (KVM_NV12V_TILE / KVM_NV12V_MB) + dr;
		if (mr >= v->mbRows) { break; }
		for (dc = 0; dc < KVM_NV12V_TILE / KVM_NV12V_MB; dc++)
		{
			const int mc = c * (KVM_NV12V_TILE / KVM_NV12V_MB) + dc;
			if (mc >= v->mbCols) { break; }
			m[(size_t)mr * v->mbCols + mc] = 1;
		}
	}
}

/* v->map = the ring of one macroblock around the marked ones (the marked ones included), from `base` */
static void dilate(const kvm_nv12v *v, const uint8_t *base, uint8_t *out)
{
	const int R = v->mbRows, C = v->mbCols;
	int r, c;
	uint8_t *tmp = v->tmp;
	/* horizontal */
	for (r = 0; r < R; r++)
	{
		const uint8_t *b = base + (size_t)r * C;
		uint8_t *t = tmp + (size_t)r * C;
		for (c = 0; c < C; c++) { t[c] = (uint8_t)(b[c] || (c > 0 && b[c - 1]) || (c + 1 < C && b[c + 1])); }
	}
	/* vertical */
	for (r = 0; r < R; r++)
	{
		uint8_t *o = out + (size_t)r * C;
		for (c = 0; c < C; c++) { o[c] = (uint8_t)(tmp[(size_t)r * C + c] || (r > 0 && tmp[(size_t)(r - 1) * C + c]) || (r + 1 < R && tmp[(size_t)(r + 1) * C + c])); }
	}
}

int kvm_nv12v_plan(kvm_nv12v *v, int tileRows, int tileCols, const uint8_t *cover, const uint8_t *st, const uint8_t *fl, int isKey, kvm_nv12v_pl *out)
{
	int r, c;
	unsigned i, real = 0, virgin = 0;
	if (v == NULL || out == NULL || cover == NULL || tileRows < 1 || tileCols < 1 || v->map == NULL) { return -1; }
	if (tileRows * KVM_NV12V_TILE > v->codedH + KVM_NV12V_TILE - 1 || tileCols * KVM_NV12V_TILE > v->codedW + KVM_NV12V_TILE - 1) { return -1; }
	memset(out, 0, sizeof(*out));
	out->totalMbs = v->total;
	if (v->total > KVM_NV12V_MB_MAX) { out->legacy = 1; out->realMbs = v->total; return 0; }
	/* a complete conversion: the view holds no picture yet (or the caller said so), or a key frame without FLAT, or the first key frame of the session when it is to be complete */
	if (!v->valid && !(isKey && v->flat && !v->firstFull)) { goto complete; }
	if (isKey && (!v->flat || (v->firstFull && !v->sessionIdr))) { goto complete; }
	/* the macroblocks of the frame (and, at a flat key frame, of the tiles that are shown from the video) */
	memset(v->map, 0, v->total);
	{
		uint8_t *base = v->base;
		memset(base, 0, v->total);
		for (r = 0; r < tileRows; r++)
		{
			for (c = 0; c < tileCols; c++)
			{
				const size_t k = (size_t)r * tileCols + c;
				if (cover[k] || (isKey && ((st != NULL && st[k] == KVM_NV12V_VIDEO) || (fl != NULL && fl[k] != 0)))) { mark_tile(v, base, r, c); }
			}
		}
		dilate(v, base, v->map);
	}
	for (i = 0; i < v->total; i++) { if (v->map[i]) { real++; } }
	if (isKey)
	{
		/* a flat key frame: V is flat except for the map; what is converted is what V holds real content of */
		memcpy(v->touched, v->map, v->total);
		v->valid = 1; v->sessionIdr = 1;
		out->flat = 1; out->map = v->map; out->realMbs = real; out->virginMbs = 0;
		return 0;
	}
	for (i = 0; i < v->total; i++) { if (v->map[i] && !v->touched[i]) { virgin++; v->touched[i] = 1; } }
	out->map = v->map; out->realMbs = real; out->virginMbs = virgin;
	return 0;
complete:
	memset(v->map, 1, v->total);
	memset(v->touched, 1, v->total);
	v->valid = 1;
	if (isKey) { v->sessionIdr = 1; }
	out->full = 1; out->map = NULL; out->realMbs = v->total; out->virginMbs = 0;
	return 0;
}

int kvm_nv12v_apply(uint8_t *view, const uint8_t *bgra, int width, int height, int stride, int codedW, int codedH, const uint8_t *map, int mbCols, int mbRows, int full, int flat)
{
	int r, c, done = 0;
	if (view == NULL || bgra == NULL) { return -1; }
	if (full) { return kvm_yuv_bgra_to_nv12(bgra, width, height, stride, view, codedW, codedH, KVM_YUV_AUTO) == 0 ? mbCols * mbRows : -1; }
	if (map == NULL || mbCols != codedW / KVM_NV12V_MB || mbRows != codedH / KVM_NV12V_MB) { return -1; }
	if (flat)
	{
		int b = 0, g = 0, rr = 0;
		uint8_t y, cb, cr;
		if (kvm_yuv_dominant(bgra, width, height, stride, &b, &g, &rr) != 0) { return -1; }
		kvm_yuv_rgb_to_yuv(b, g, rr, &y, &cb, &cr);
		if (kvm_yuv_fill_flat(view, codedW, codedH, y, cb, cr) != 0) { return -1; }
	}
	for (r = 0; r < mbRows; r++)
	{
		const uint8_t *row = map + (size_t)r * mbCols;
		c = 0;
		while (c < mbCols)
		{
			int c1;
			if (!row[c]) { c++; continue; }
			for (c1 = c; c1 < mbCols && row[c1]; c1++) {}
			if (kvm_yuv_bgra_to_nv12_region(bgra, width, height, stride, view, codedW, codedH, c * KVM_NV12V_MB, r * KVM_NV12V_MB, (c1 - c) * KVM_NV12V_MB, KVM_NV12V_MB, KVM_YUV_AUTO) != 0) { return -1; }
			done += c1 - c;
			c = c1;
		}
	}
	return done;
}
