/*
KVM (Windows): pixel logic for taking a software cursor out of the DXGI image without reading the screen with GDI.

Header only, no Windows headers (tested on Linux, see tools/test_ptrmask.cpp). The persistent image (fullBits) is a 32 bpp bottom-up
bitmap of sw x sh pixels (memory row 0 = bottom row), coordinates here are top-down pixels of the captured rectangle. The "field" of
the cursor is the cursor bitmap placed at its hotspot plus a margin; ur is the field unclipped, cr the part of it that lies inside the
captured rectangle. A mask holds one byte per pixel of ur (1 = the pixel can hold the cursor: its shape dilated by one pixel).
Scratch buffers (S, N, G) are top-down, packed, cr-sized, 4 bytes per pixel.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef KVM_DXGI_PTRMASK_H
#define KVM_DXGI_PTRMASK_H

#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#ifndef KVM_DXGI_POINTER_MASK_PERCENT
#define KVM_DXGI_POINTER_MASK_PERCENT 60	// a mask that covers more than this share of the field becomes "the whole field" (provisional)
#endif
#define PM_CACHE 8

struct pm_rect { int left, top, right, bottom; };

// ------------------------------------------------------------------------------------------------------------------------------
// The cursor bitmap as pixels: the part that is composed unchanged (fully opaque pixels) is used to find the cursor in a frame.

struct pm_sprite
{
	void *hc;				// cursor handle
	int w, h, hx, hy;		// bitmap size and hotspot
	int n;					// number of fully opaque pixels
	int *x, *y;				// their positions in the bitmap (in an order that spreads the first probes over the shape)
	unsigned char *rgb;		// their colours, B G R (3 bytes each)
	int monochrome;			// no colour bitmap: composed with AND/XOR masks, cannot be found by colour
	unsigned char *aff;		// w x h bytes: 1 = the pixel is changed by the cursor (alpha > 0), colour cursors only
};

static inline void pm_sprite_free(pm_sprite *sp) { free(sp->x); free(sp->y); free(sp->rgb); free(sp->aff); memset(sp, 0, sizeof(*sp)); }

static inline int pm_gcd(int a, int b) { while (b != 0) { int t = a % b; a = b; b = t; } return a; }

// Orders the n opaque pixels so that the first ones are far apart (stride permutation); a, b are the arrays to permute together.
static inline void pm_sprite_spread(pm_sprite *sp)
{
	int n = sp->n;
	if (n < 4) { return; }
	int *nx = (int *)malloc(sizeof(int) * (size_t)n), *ny = (int *)malloc(sizeof(int) * (size_t)n);
	unsigned char *nc = (unsigned char *)malloc((size_t)n * 3);
	if (nx == NULL || ny == NULL || nc == NULL) { free(nx); free(ny); free(nc); return; }
	int step = 7;
	while (pm_gcd(step, n) != 1) { step += 2; } // coprime with n: the stride visits every index exactly once
	for (int i = 0, j = 0; i < n; i++, j = (j + step) % n) { nx[i] = sp->x[j]; ny[i] = sp->y[j]; memcpy(nc + (size_t)i * 3, sp->rgb + (size_t)j * 3, 3); }
	free(sp->x); free(sp->y); free(sp->rgb);
	sp->x = nx; sp->y = ny; sp->rgb = nc;
}

struct pm_mask
{
	void *hc;				// cursor handle (NULL = slot free)
	int w, h;				// size of the field (ur)
	unsigned char *m;		// w x h bytes, NULL = the whole field
	int provisional;		// the whole field, until a mask is made in a quiet moment
	int count;				// pixels set
	unsigned int use;		// last use (counter), for replacement
	pm_sprite sp;			// the cursor bitmap (colour cursors)
	int spState;			// 0 = not examined, 1 = sp is usable, 2 = monochrome or failed
};

struct pm_cache { pm_mask e[PM_CACHE]; unsigned int tick; };

static inline void pm_free_mask(pm_mask *e) { free(e->m); pm_sprite_free(&e->sp); memset(e, 0, sizeof(*e)); }
static inline void pm_cache_free(pm_cache *c) { for (int i = 0; i < PM_CACHE; i++) { pm_free_mask(&c->e[i]); } c->tick = 0; }

// The entry of a cursor handle with a field of this size, or NULL. A handle with another size is dropped (the handle was reused).
static inline pm_mask *pm_cache_find(pm_cache *c, void *hc, int w, int h)
{
	for (int i = 0; i < PM_CACHE; i++)
	{
		if (c->e[i].hc == hc)
		{
			if (c->e[i].w != w || c->e[i].h != h) { pm_free_mask(&c->e[i]); return NULL; }
			c->e[i].use = ++c->tick;
			return &c->e[i];
		}
	}
	return NULL;
}

// A free or the least recently used entry, emptied and given to hc.
static inline pm_mask *pm_cache_take(pm_cache *c, void *hc, int w, int h)
{
	int best = 0;
	for (int i = 0; i < PM_CACHE; i++)
	{
		if (c->e[i].hc == NULL) { best = i; break; }
		if (c->e[i].use < c->e[best].use) { best = i; }
	}
	pm_free_mask(&c->e[best]);
	c->e[best].hc = hc;
	c->e[best].w = w;
	c->e[best].h = h;
	c->e[best].provisional = 1;
	c->e[best].use = ++c->tick;
	return &c->e[best];
}

static inline unsigned char *pm_fb(unsigned char *fb, int sw, int sh, int x, int y) { return fb + ((size_t)(sh - 1 - y) * sw + (size_t)x) * 4; }

// Rectangle cr (clipped field) of the persistent image into buf, top-down.
static inline void pm_copy_out(unsigned char *fb, int sw, int sh, const pm_rect &cr, unsigned char *buf)
{
	int w = cr.right - cr.left;
	for (int y = cr.top; y < cr.bottom; y++) { memcpy(buf + (size_t)(y - cr.top) * w * 4, pm_fb(fb, sw, sh, cr.left, y), (size_t)w * 4); }
}

// Pixels (bytes 0..2) of cr that differ between the persistent image and G.
static inline int pm_diff_count(unsigned char *fb, int sw, int sh, const pm_rect &cr, const unsigned char *G)
{
	int w = cr.right - cr.left, n = 0;
	for (int y = cr.top; y < cr.bottom; y++)
	{
		const unsigned char *d = pm_fb(fb, sw, sh, cr.left, y), *g = G + (size_t)(y - cr.top) * w * 4;
		for (int x = 0; x < w; x++, d += 4, g += 4) { if (d[0] != g[0] || d[1] != g[1] || d[2] != g[2]) { n++; } }
	}
	return n;
}

static inline void pm_set_count(pm_mask *e)
{
	int n = 0, total = e->w * e->h;
	if (e->m == NULL) { e->count = total; return; }
	for (int i = 0; i < total; i++) { if (e->m[i]) { n++; } }
	e->count = n;
	if ((long long)n * 100 > (long long)total * KVM_DXGI_POINTER_MASK_PERCENT)
	{
		free(e->m);
		e->m = NULL;			// the whole field
		e->provisional = 1;
		e->count = total;
	}
}

// Makes or corrects the mask of e from the pixels that differ between N (the DXGI pixels with the cursor) and G (GDI, without it), both cr-sized.
// replace: a new mask (creation, or the correction of a provisional one) in which the part of the field outside the screen counts as set;
// otherwise (the mask is finished) the differences are added to the mask. Call it with replace = e->provisional. Every difference is widened by one pixel in every direction.
static inline void pm_build(pm_mask *e, const pm_rect &ur, const pm_rect &cr, const unsigned char *N, const unsigned char *G, int replace)
{
	int uw = ur.right - ur.left, uh = ur.bottom - ur.top, cw = cr.right - cr.left;
	unsigned char *t = (unsigned char *)calloc((size_t)uw * uh, 1);
	if (t == NULL) { return; }
	if (replace)
	{
		for (int y = 0; y < uh; y++)
		{
			for (int x = 0; x < uw; x++)
			{
				int ax = ur.left + x, ay = ur.top + y;
				if (ax < cr.left || ax >= cr.right || ay < cr.top || ay >= cr.bottom) { t[(size_t)y * uw + x] = 1; }
			}
		}
	}
	for (int y = cr.top; y < cr.bottom; y++)
	{
		const unsigned char *n = N + (size_t)(y - cr.top) * cw * 4, *g = G + (size_t)(y - cr.top) * cw * 4;
		for (int x = cr.left; x < cr.right; x++, n += 4, g += 4)
		{
			if (n[0] != g[0] || n[1] != g[1] || n[2] != g[2])
			{
				for (int dy = -1; dy <= 1; dy++)
				{
					for (int dx = -1; dx <= 1; dx++)
					{
						int mx = x + dx - ur.left, my = y + dy - ur.top;
						if (mx >= 0 && mx < uw && my >= 0 && my < uh) { t[(size_t)my * uw + mx] = 1; }
					}
				}
			}
		}
	}
	if (replace || e->m == NULL)
	{
		free(e->m);
		e->m = t;
		e->provisional = 0;
	}
	else
	{
		for (int i = 0; i < uw * uh; i++) { if (t[i]) { e->m[i] = 1; } }
		free(t);
	}
	pm_set_count(e);
}

// The part of cr under the mask (the whole of it without a mask) is taken from G (alpha zeroed) into the persistent image.
static inline int pm_apply(const pm_mask *e, const pm_rect &ur, const pm_rect &cr, unsigned char *fb, int sw, int sh, const unsigned char *G)
{
	int uw = ur.right - ur.left, cw = cr.right - cr.left, n = 0;
	for (int y = cr.top; y < cr.bottom; y++)
	{
		const unsigned char *g = G + (size_t)(y - cr.top) * cw * 4;
		unsigned char *d = pm_fb(fb, sw, sh, cr.left, y);
		for (int x = cr.left; x < cr.right; x++, g += 4, d += 4)
		{
			if (e == NULL || e->m == NULL || e->m[(size_t)(y - ur.top) * uw + (x - ur.left)]) { d[0] = g[0]; d[1] = g[1]; d[2] = g[2]; d[3] = 0; n++; }
		}
	}
	return n;
}

// After a frame was copied into the persistent image: N gets the raw pixels of cr (with the cursor), and the pixels under the mask (the whole
// of cr without a mask) are put back from S, the pixels of the field before the frame. Returns the number of pixels outside the mask that
// differ from S: the content of the field changed, so the pixels under the cursor are out of date.
static inline int pm_restore(const pm_mask *e, const pm_rect &ur, const pm_rect &cr, unsigned char *fb, int sw, int sh, const unsigned char *S, unsigned char *N)
{
	int uw = ur.right - ur.left, cw = cr.right - cr.left, outside = 0;
	for (int y = cr.top; y < cr.bottom; y++)
	{
		const unsigned char *s = S + (size_t)(y - cr.top) * cw * 4;
		unsigned char *n = N + (size_t)(y - cr.top) * cw * 4, *d = pm_fb(fb, sw, sh, cr.left, y);
		for (int x = cr.left; x < cr.right; x++, s += 4, n += 4, d += 4)
		{
			memcpy(n, d, 4);
			if (e == NULL || e->m == NULL || e->m[(size_t)(y - ur.top) * uw + (x - ur.left)]) { memcpy(d, s, 4); }
			else if (d[0] != s[0] || d[1] != s[1] || d[2] != s[2]) { outside++; }
		}
	}
	return outside;
}


// ------------------------------------------------------------------------------------------------------------------------------
// Finding the cursor in a frame. The system composes a software cursor into the frame at the place where the pointer was when the
// frame was made, which is not necessarily where GetCursorInfo says it is when the agent asks (a moving pointer). The fully opaque
// pixels of a colour cursor are composed unchanged, so the cursor is found by comparing them with the image.

// Does the sprite with its bitmap's top-left corner at (px, py) match the persistent image? Opaque pixels outside the image are not compared (at
// least half of them must be inside). Strict on the first probes, then at most 2 % mismatches. Returns the number of matching pixels, 0 = no.
static inline int pm_match_at(unsigned char *fb, int sw, int sh, const pm_sprite *sp, int px, int py)
{
	int inside = 0, hit = 0, miss = 0, allow = sp->n / 50;
	for (int i = 0; i < sp->n; i++)
	{
		int x = px + sp->x[i], y = py + sp->y[i];
		if (x < 0 || y < 0 || x >= sw || y >= sh) { continue; }
		const unsigned char *d = pm_fb(fb, sw, sh, x, y), *c = sp->rgb + (size_t)i * 3;
		inside++;
		if (d[0] == c[0] && d[1] == c[1] && d[2] == c[2]) { hit++; }
		else if (i < 12 || ++miss > allow) { return 0; }
	}
	return (inside * 2 >= sp->n && hit > 0) ? hit : 0;
}

// Searches the bitmap positions whose bitmap overlaps the rectangle q (image pixels) and lies where the persistent image was written, for the best
// match; returns 1 and the bitmap's top-left in (*bx, *by), the number of matching pixels in *hits. At most 'budget' positions are tried
// (the rest of the rectangle is not searched); *tried gets the number of positions tried.
static inline int pm_find(unsigned char *fb, int sw, int sh, const pm_sprite *sp, const pm_rect &q, int *bx, int *by, int *hits, long long budget, long long *tried)
{
	int best = 0;
	long long t = 0;
	for (int py = q.top - sp->h + 1; py < q.bottom; py++)
	{
		for (int px = q.left - sp->w + 1; px < q.right; px++)
		{
			if (t++ >= budget) { if (tried) { *tried += t - 1; } return best > 0; }
			int h = pm_match_at(fb, sw, sh, sp, px, py);
			if (h > best) { best = h; *bx = px; *by = py; *hits = h; }
		}
	}
	if (tried) { *tried += t; }
	return best > 0;
}

// Several places in the frame can look like the cursor (the same arrow inside a screenshot shown in the frame): pm_find gives the best match of each region,
// pm_pick chooses among them. The one nearest to a place where the cursor is expected (GetCursorInfo, the places where it was found before) wins among the
// candidates that match about as well as the best one (at least 90 % of its matching pixels); a single candidate is taken as it is. Distance = the larger of
// the two coordinate differences, to the nearest expected top-left corner of the sprite. Returns the index of the chosen candidate, -1 for none.
struct pm_cand { int x, y, hits; };
static inline int pm_pick(const pm_cand *c, int n, const int *ex, const int *ey, int ne)
{
	int best = -1, maxHits = 0, bestDist = 0;
	for (int i = 0; i < n; i++) { if (c[i].hits > maxHits) { maxHits = c[i].hits; } }
	if (maxHits <= 0) { return -1; }
	for (int i = 0; i < n; i++)
	{
		int dist = 0x7FFFFFFF;
		if ((long long)c[i].hits * 10 < (long long)maxHits * 9) { continue; }
		for (int k = 0; k < ne; k++)
		{
			int dx = c[i].x - ex[k], dy = c[i].y - ey[k];
			if (dx < 0) { dx = -dx; } if (dy < 0) { dy = -dy; }
			if ((dx > dy ? dx : dy) < dist) { dist = dx > dy ? dx : dy; }
		}
		if (best < 0 || dist < bestDist || (dist == bestDist && c[i].hits > c[best].hits)) { best = i; bestDist = dist; }
	}
	return best;
}

// The mask of a colour cursor from its bitmap: every pixel that the cursor changes (alpha > 0), widened by one pixel. The field has
// 'margin' pixels around the bitmap (the same margin as the field of the cursor).
static inline int pm_mask_from_sprite(pm_mask *e, int margin)
{
	const pm_sprite *sp = &e->sp;
	int uw = e->w, uh = e->h;
	if (sp->aff == NULL || sp->w + 2 * margin != uw || sp->h + 2 * margin != uh) { return 0; }
	unsigned char *t = (unsigned char *)calloc((size_t)uw * uh, 1);
	if (t == NULL) { return 0; }
	for (int y = 0; y < sp->h; y++)
	{
		for (int x = 0; x < sp->w; x++)
		{
			if (!sp->aff[(size_t)y * sp->w + x]) { continue; }
			for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
			{
				int mx = margin + x + dx, my = margin + y + dy;
				if (mx >= 0 && mx < uw && my >= 0 && my < uh) { t[(size_t)my * uw + mx] = 1; }
			}
		}
	}
	free(e->m);
	e->m = t;
	e->provisional = 0;
	pm_set_count(e);
	return 1;
}

// Pixels of the persistent image saved before a frame was copied into it: a rectangle and its pixels, top-down, packed.
#define PM_REGIONS 16
struct pm_region { pm_rect r; unsigned char *S; size_t cap; };

static inline void pm_regions_free(pm_region *rg, int n) { for (int i = 0; i < n; i++) { free(rg[i].S); rg[i].S = NULL; rg[i].cap = 0; } }

// Saves the rectangle r (already clipped to the image) as region *rg. Returns 0 when memory is short.
static inline int pm_region_save(unsigned char *fb, int sw, int sh, pm_region *rg, const pm_rect &r)
{
	size_t need = (size_t)(r.right - r.left) * (r.bottom - r.top) * 4;
	if (rg->cap < need)
	{
		unsigned char *n = (unsigned char *)realloc(rg->S, need);
		if (n == NULL) { return 0; }
		rg->S = n; rg->cap = need;
	}
	rg->r = r;
	pm_copy_out(fb, sw, sh, r, rg->S);
	return 1;
}

// Like pm_restore, for a field cr (clipped) whose pixels before the frame were saved in several regions: the pixels of cr that lie in a region and
// under the mask (the whole of it without a mask) are put back; returns the number of pixels outside the mask that differ from the saved ones.
static inline int pm_restore_regions(const pm_mask *e, const pm_rect &ur, const pm_rect &cr, unsigned char *fb, int sw, int sh, const pm_region *rg, int n)
{
	int uw = ur.right - ur.left, outside = 0;
	for (int i = 0; i < n; i++)
	{
		pm_rect q = rg[i].r;
		if (q.left < cr.left) { q.left = cr.left; } if (q.top < cr.top) { q.top = cr.top; }
		if (q.right > cr.right) { q.right = cr.right; } if (q.bottom > cr.bottom) { q.bottom = cr.bottom; }
		if (q.right <= q.left || q.bottom <= q.top) { continue; }
		int rw = rg[i].r.right - rg[i].r.left;
		for (int y = q.top; y < q.bottom; y++)
		{
			const unsigned char *s = rg[i].S + ((size_t)(y - rg[i].r.top) * rw + (size_t)(q.left - rg[i].r.left)) * 4;
			unsigned char *d = pm_fb(fb, sw, sh, q.left, y);
			for (int x = q.left; x < q.right; x++, s += 4, d += 4)
			{
				if (e == NULL || e->m == NULL || e->m[(size_t)(y - ur.top) * uw + (x - ur.left)]) { memcpy(d, s, 4); }
				else if (d[0] != s[0] || d[1] != s[1] || d[2] != s[2]) { outside++; }
			}
		}
	}
	return outside;
}

#endif // KVM_DXGI_PTRMASK_H
