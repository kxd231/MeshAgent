/* KVM: deterministic test pictures in NV12, see kvm_synth.h. */

#include "kvm_synth.h"
#include <stdlib.h>
#include <string.h>

static uint32_t hash32(uint32_t x)
{
	x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
	return x;
}

static uint8_t clampy(int v) { return (uint8_t)(v < 16 ? 16 : (v > 235 ? 235 : v)); }
static uint8_t clampc(int v) { return (uint8_t)(v < 16 ? 16 : (v > 240 ? 240 : v)); }

static void fill_rect_y(kvm_synth *s, int x0, int y0, int x1, int y1, int v)
{
	int x, y;
	if (x0 < 0) { x0 = 0; } if (y0 < 0) { y0 = 0; } if (x1 > s->w) { x1 = s->w; } if (y1 > s->h) { y1 = s->h; }
	for (y = y0; y < y1; y++) { for (x = x0; x < x1; x++) { s->y[(size_t)y * (size_t)s->w + (size_t)x] = clampy(v); } }
}

// the chroma of a rectangle (luma coordinates, 2 x 2 per sample)
static void fill_rect_uv(kvm_synth *s, int x0, int y0, int x1, int y1, int u, int v)
{
	int x, y;
	x0 &= ~1; y0 &= ~1; x1 = (x1 + 1) & ~1; y1 = (y1 + 1) & ~1;
	if (x0 < 0) { x0 = 0; } if (y0 < 0) { y0 = 0; } if (x1 > s->w) { x1 = s->w; } if (y1 > s->h) { y1 = s->h; }
	for (y = y0; y < y1; y += 2) { for (x = x0; x < x1; x += 2) { uint8_t *p = s->uv + (size_t)(y / 2) * (size_t)s->w + (size_t)x; p[0] = clampc(u); p[1] = clampc(v); } }
}

// a glyph of 5 x 7 cells of 2 x 2 pixels (the bits come from the hash of the code), at (x, y), dark on the page
static void glyph(kvm_synth *s, int x, int y, uint32_t code, int lum)
{
	const uint32_t bits = hash32(code * 2654435761u + 12345u);
	int cx, cy;
	for (cy = 0; cy < 7; cy++)
	{
		for (cx = 0; cx < 5; cx++)
		{
			const int on = (int)((bits >> ((cy * 5 + cx) % 32)) & 1u) ^ (cy == 0 || cx == 0 ? 0 : (int)((bits >> 31) & 0u));
			if (on || (cy == 3 && (bits & 8u))) { fill_rect_y(s, x + cx * 2, y + cy * 2, x + cx * 2 + 2, y + cy * 2 + 2, lum); }
		}
	}
}

int kvm_synth_init(kvm_synth *s, int w, int h)
{
	int row, col, line;
	if (s == NULL) { return -1; }
	memset(s, 0, sizeof(*s));
	if (w < 64 || h < 64 || w > 8192 || h > 8192 || (w & 15) || (h & 15)) { return -1; }
	s->w = w; s->h = h;
	s->y = (uint8_t *)malloc((size_t)w * (size_t)h);
	s->uv = (uint8_t *)malloc((size_t)w * (size_t)h / 2u);
	if (s->y == NULL || s->uv == NULL) { kvm_synth_free(s); return -1; }
	memset(s->y, 220, (size_t)w * (size_t)h);									/* the page */
	for (row = 0; row < h / 2; row++) { for (col = 0; col < w; col += 2) { s->uv[(size_t)row * (size_t)w + (size_t)col] = 128; s->uv[(size_t)row * (size_t)w + (size_t)col + 1] = 128; } }
	fill_rect_y(s, 0, 0, w, 40, 70); fill_rect_uv(s, 0, 0, w, 40, 160, 110);			/* title bar */
	fill_rect_y(s, 0, 40, 220, h, 200); fill_rect_uv(s, 0, 40, 220, h, 135, 120);		/* side panel */
	for (line = 0; line < (h - 60) / 22; line++)
	{
		int x = 240, word = 0, c;
		const int y = 56 + line * 22;
		for (c = 0; x < w - 24; c++)
		{
			const uint32_t code = (uint32_t)(line * 131 + c * 17 + 7);
			if ((hash32(code) & 7u) == 0) { x += 12; word++; continue; }			/* a space */
			glyph(s, x, y, hash32(code) % 94u + 33u, 40);
			if ((word % 5) == 3) { fill_rect_uv(s, x, y, x + 10, y + 14, 90, 170); }	/* coloured words */
			x += 12;
		}
		/* the side panel has short lines of text too */
		for (c = 0; c < 14; c++) { glyph(s, 12 + c * 12, y, hash32((uint32_t)(line * 7 + c)) % 94u + 33u, 60); }
	}
	return 0;
}

void kvm_synth_free(kvm_synth *s)
{
	if (s == NULL) { return; }
	free(s->y); free(s->uv);
	s->y = s->uv = NULL; s->w = s->h = 0;
}

size_t kvm_synth_size(const kvm_synth *s) { return (size_t)s->w * (size_t)s->h * 3u / 2u; }

void kvm_synth_frame(const kvm_synth *s, int kind, int t, uint8_t *nv12)
{
	const int w = s->w, h = s->h;
	uint8_t *oy = nv12, *ouv = nv12 + (size_t)w * (size_t)h;
	int r;
	if (t < 0) { t = 0; }
	if (kind == KVM_SYNTH_SCROLL)
	{
		const int sy = (4 * t) % h, suv = (2 * t) % (h / 2);
		for (r = 0; r < h; r++) { memcpy(oy + (size_t)r * (size_t)w, s->y + (size_t)((r + sy) % h) * (size_t)w, (size_t)w); }
		for (r = 0; r < h / 2; r++) { memcpy(ouv + (size_t)r * (size_t)w, s->uv + (size_t)((r + suv) % (h / 2)) * (size_t)w, (size_t)w); }
		return;
	}
	memcpy(oy, s->y, (size_t)w * (size_t)h);
	memcpy(ouv, s->uv, (size_t)w * (size_t)h / 2u);
	{
		/* small updates: a caret that blinks every other picture, a clock field of 64 x 16 pixels that changes every picture */
		kvm_synth tmp = *s;
		int x, y;
		tmp.y = oy; tmp.uv = ouv;
		if ((t & 1) == 0) { fill_rect_y(&tmp, 400, 100, 404, 116, 20); }
		for (y = 8; y < 24; y++) { for (x = w - 80; x < w - 16; x++) { oy[(size_t)y * (size_t)w + (size_t)x] = clampy(150 + (int)(hash32((uint32_t)(x * 31 + y * 17 + t * 1009)) & 63u)); } }
	}
	if (kind == KVM_SYNTH_VIDEO)
	{
		const int x0 = w / 4, x1 = w * 3 / 4, y0 = h / 4, y1 = h * 3 / 4;
		int x, y;
		for (y = y0; y < y1; y++)
		{
			for (x = x0; x < x1; x++)
			{
				const int base = 60 + (((x * 3 + y * 5 + t * 7) >> 1) & 127);
				oy[(size_t)y * (size_t)w + (size_t)x] = clampy(base + (int)(hash32((uint32_t)(x + y * 8191 + t * 100003)) & 15u));
			}
		}
		for (y = y0 / 2; y < y1 / 2; y++)
		{
			for (x = x0; x < x1; x += 2)
			{
				uint8_t *p = ouv + (size_t)y * (size_t)w + (size_t)x;
				p[0] = clampc(100 + ((x + t * 3) & 63)); p[1] = clampc(100 + ((y * 2 + t * 5) & 63));
			}
		}
	}
}

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

void kvm_synth_frame_bgra(const kvm_synth *s, int kind, int t, uint8_t *bgra, int stride)
{
	const int w = s->w, h = s->h;
	uint8_t *nv = (uint8_t *)malloc(kvm_synth_size(s));
	int x, y;
	if (nv == NULL) { for (y = 0; y < h; y++) { memset(bgra + (size_t)y * (size_t)stride, 0x80, (size_t)w * 4u); } return; }
	kvm_synth_frame(s, kind, t, nv);
	for (y = 0; y < h; y++)
	{
		const uint8_t *py = nv + (size_t)y * (size_t)w, *puv = nv + (size_t)w * (size_t)h + (size_t)(y / 2) * (size_t)w;
		uint8_t *o = bgra + (size_t)y * (size_t)stride;
		for (x = 0; x < w; x++)
		{
			const int Y = (int)py[x] - 16, U = (int)puv[x & ~1] - 128, V = (int)puv[(x & ~1) + 1] - 128;
			const int c = 298 * Y;															/* 1.164 * 256 */
			o[4 * x + 2] = clamp8((c + 459 * V + 128) >> 8);								/* R = 1.164 Y + 1.793 V */
			o[4 * x + 1] = clamp8((c - 55 * U - 136 * V + 128) >> 8);						/* G = 1.164 Y - 0.213 U - 0.533 V */
			o[4 * x + 0] = clamp8((c + 541 * U + 128) >> 8);								/* B = 1.164 Y + 2.112 U */
			o[4 * x + 3] = 255;
		}
	}
	free(nv);
}
