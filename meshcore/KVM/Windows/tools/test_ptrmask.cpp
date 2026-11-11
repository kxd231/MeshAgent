// Host test of dxgi_ptrmask.h (no Windows): g++ -std=c++11 -O1 -I.. tools/test_ptrmask.cpp -o /tmp/test_ptrmask && /tmp/test_ptrmask
// Simulates the screen, a software cursor composed into the DXGI frames, and the way tile.cpp uses the header.
#include "../dxgi_ptrmask.h"
#include <stdio.h>
#include <vector>

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const int SW = 200, SH = 120, M = 24, CW = 12, CH = 18, HX = 1, HY = 1;
static inline unsigned content(int x, int y, int t) { return (unsigned)((x * 7 + y * 13 + t * 31) & 0xFF); }
static bool sprite(int dx, int dy) { return dx >= 0 && dy >= 0 && dx < CW && dy < CH && dx <= dy / 2 + 2; } // arrow like shape
struct Screen
{
	std::vector<unsigned char> content_; // bottom-up, as fullBits
	int t = 0;
	Screen() : content_((size_t)SW * SH * 4) { redraw(-1000); }
	void redraw(int region) { (void)region; for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) { unsigned char *p = pm_fb(content_.data(), SW, SH, x, y); p[0] = p[1] = p[2] = (unsigned char)content(x, y, t); p[3] = 0xFF; } }
	// pixel as the GDI sees it (no cursor)
	void gdi(unsigned char *G, const pm_rect &cr) { int w = cr.right - cr.left; for (int y = cr.top; y < cr.bottom; y++) for (int x = cr.left; x < cr.right; x++) { unsigned char *p = pm_fb(content_.data(), SW, SH, x, y), *g = G + ((size_t)(y - cr.top) * w + (x - cr.left)) * 4; memcpy(g, p, 4); } }
	// pixel as DXGI delivers it: cursor composed in
	void dxgi_px(int x, int y, int cx, int cy, unsigned char *out) { unsigned char *p = pm_fb(content_.data(), SW, SH, x, y); memcpy(out, p, 4); if (sprite(x - (cx - HX), y - (cy - HY))) { out[0] = 255 - out[0]; out[1] = out[0]; out[2] = out[0]; } out[3] = 0xFF; }
};
static void fields(int cx, int cy, pm_rect *ur, pm_rect *cr)
{
	ur->left = cx - HX - M; ur->top = cy - HY - M; ur->right = ur->left + CW + 2 * M; ur->bottom = ur->top + CH + 2 * M;
	cr->left = ur->left < 0 ? 0 : ur->left; cr->top = ur->top < 0 ? 0 : ur->top; cr->right = ur->right > SW ? SW : ur->right; cr->bottom = ur->bottom > SH ? SH : ur->bottom;
}
// The persistent image after a DXGI copy of rect q (the cursor at (cx, cy) is part of what is copied)
static void dxgi_copy(Screen &s, std::vector<unsigned char> &fb, const pm_rect &q, int cx, int cy)
{
	for (int y = q.top; y < q.bottom; y++) for (int x = q.left; x < q.right; x++) s.dxgi_px(x, y, cx, cy, pm_fb(fb.data(), SW, SH, x, y));
}
static int count_cursor_px(Screen &s, std::vector<unsigned char> &fb) // pixels of fb that differ from the cursor free screen
{
	int n = 0; for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) { unsigned char *a = pm_fb(fb.data(), SW, SH, x, y), *b = pm_fb(s.content_.data(), SW, SH, x, y); if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2]) n++; } return n;
}

int main()
{
	Screen s;
	std::vector<unsigned char> fb((size_t)SW * SH * 4);
	pm_cache cache; memset(&cache, 0, sizeof(cache));
	void *hc = (void *)0x10003;
	std::vector<unsigned char> S((size_t)(CW + 2 * M) * (CH + 2 * M) * 4), N(S.size()), G(S.size());
	pm_rect all = { 0, 0, SW, SH };

	// first frame: full copy with the cursor at (100, 60), probe, mask from the difference, GDI pixels under the mask
	int cx = 100, cy = 60;
	dxgi_copy(s, fb, all, cx, cy);
	pm_rect ur, cr; fields(cx, cy, &ur, &cr);
	s.gdi(G.data(), cr);
	int diff = pm_diff_count(fb.data(), SW, SH, cr, G.data());
	CHECK(diff >= 8, "probe diff %d", diff);
	pm_copy_out(fb.data(), SW, SH, cr, N.data());
	pm_mask *e = pm_cache_take(&cache, hc, ur.right - ur.left, ur.bottom - ur.top);
	pm_build(e, ur, cr, N.data(), G.data(), 1);
	CHECK(e->m != NULL && !e->provisional, "mask made, count %d", e->count);
	printf("mask pixels %d of %d (shape %d)\n", e->count, e->w * e->h, diff);
	pm_apply(e, ur, cr, fb.data(), SW, SH, G.data());
	CHECK(count_cursor_px(s, fb) == 0, "after the first fix: %d cursor pixels left", count_cursor_px(s, fb));

	// the cursor moves, content static: DXGI delivers erase of the old place and the new cursor
	for (int step = 0; step < 40; step++)
	{
		int ox = cx, oy = cy; cx = 100 + (step * 5) % 60; cy = 60 + (step * 3) % 30;
		pm_rect oldUr, oldCr; fields(ox, oy, &oldUr, &oldCr);
		fields(cx, cy, &ur, &cr);
		// pre-copy: S from the persistent image
		pm_copy_out(fb.data(), SW, SH, cr, S.data());
		// frame: rect of the old place (erase, clean pixels as DXGI composes without the cursor there) and the new place with the cursor
		pm_rect oldRect = oldCr, newRect = cr;
		for (int y = oldRect.top; y < oldRect.bottom; y++) for (int x = oldRect.left; x < oldRect.right; x++) s.dxgi_px(x, y, -1000, -1000, pm_fb(fb.data(), SW, SH, x, y));
		dxgi_copy(s, fb, newRect, cx, cy);
		pm_mask *ee = pm_cache_find(&cache, hc, ur.right - ur.left, ur.bottom - ur.top);
		CHECK(ee != NULL, "mask found");
		int outside = pm_restore(ee, ur, cr, fb.data(), SW, SH, S.data(), N.data());
		CHECK(outside == 0, "static content, cursor only: outside=%d", outside);
		CHECK(count_cursor_px(s, fb) == 0, "step %d: %d cursor pixels in the image", step, count_cursor_px(s, fb));
	}

	// the content changes under a stationary cursor (video): the frame brings new content and the cursor; masked pixels come from S (old), the rest is new
	s.t = 5;
	s.redraw(0);
	fields(cx, cy, &ur, &cr);
	pm_copy_out(fb.data(), SW, SH, cr, S.data());
	dxgi_copy(s, fb, all, cx, cy);
	pm_mask *ee = pm_cache_find(&cache, hc, ur.right - ur.left, ur.bottom - ur.top);
	int outside = pm_restore(ee, ur, cr, fb.data(), SW, SH, S.data(), N.data());
	CHECK(outside > 0, "content changed: outside=%d", outside);
	int bad = 0; for (int y = cr.top; y < cr.bottom; y++) for (int x = cr.left; x < cr.right; x++) { unsigned char *p = pm_fb(fb.data(), SW, SH, x, y); bool inCursor = ee->m[(size_t)(y - ur.top) * (ur.right - ur.left) + (x - ur.left)] != 0; if (!inCursor) { unsigned char *c = pm_fb(s.content_.data(), SW, SH, x, y); if (p[0] != c[0]) bad++; } }
	CHECK(bad == 0, "pixels outside the mask are the new content: bad=%d", bad);
	// quiet refresh: GDI read, mask corrected from N against G (nothing to add), masked pixels from G
	s.gdi(G.data(), cr);
	pm_build(ee, ur, cr, N.data(), G.data(), ee->provisional);
	pm_apply(ee, ur, cr, fb.data(), SW, SH, G.data());
	CHECK(count_cursor_px(s, fb) == 0, "after the refresh: %d pixels differ from the screen", count_cursor_px(s, fb));

	// incomplete mask (shadow): a cursor with a pixel outside the mask, then a quiet refresh grows the mask
	pm_mask *m2 = pm_cache_take(&cache, (void *)0x10007, ur.right - ur.left, ur.bottom - ur.top);
	m2->m = (unsigned char *)calloc((size_t)m2->w * m2->h, 1); m2->provisional = 0; // an empty mask
	pm_copy_out(fb.data(), SW, SH, cr, S.data());
	dxgi_copy(s, fb, cr, cx, cy);
	outside = pm_restore(m2, ur, cr, fb.data(), SW, SH, S.data(), N.data());
	CHECK(outside > 0, "empty mask: cursor pixels count as changed outside (%d), which makes the field stale", outside);
	s.gdi(G.data(), cr);
	pm_build(m2, ur, cr, N.data(), G.data(), 0);
	pm_apply(m2, ur, cr, fb.data(), SW, SH, G.data());
	CHECK(count_cursor_px(s, fb) == 0, "grown mask clears the cursor: %d", count_cursor_px(s, fb));
	CHECK(m2->count > 0 && !m2->provisional, "mask count %d", m2->count);

	// provisional whole field mask, replaced in a quiet moment
	pm_mask *m3 = pm_cache_take(&cache, (void *)0x10009, ur.right - ur.left, ur.bottom - ur.top);
	CHECK(m3->m == NULL && m3->provisional, "new entry is the whole field");
	pm_copy_out(fb.data(), SW, SH, cr, S.data());
	dxgi_copy(s, fb, cr, cx, cy);
	pm_restore(m3, ur, cr, fb.data(), SW, SH, S.data(), N.data());
	s.gdi(G.data(), cr);
	pm_build(m3, ur, cr, N.data(), G.data(), m3->provisional);
	CHECK(m3->m != NULL && !m3->provisional && m3->count < m3->w * m3->h / 4, "provisional replaced, count %d of %d", m3->count, m3->w * m3->h);
	pm_apply(m3, ur, cr, fb.data(), SW, SH, G.data());
	CHECK(count_cursor_px(s, fb) == 0, "after replacing the provisional mask: %d", count_cursor_px(s, fb));

	// clipped field at the corner: unseen part counts as set (conservative), pixels still come out right
	cx = 2; cy = 3;
	fields(cx, cy, &ur, &cr);
	pm_mask *m4 = pm_cache_take(&cache, (void *)0x1000b, ur.right - ur.left, ur.bottom - ur.top);
	dxgi_copy(s, fb, cr, cx, cy);
	pm_copy_out(fb.data(), SW, SH, cr, N.data());
	s.gdi(G.data(), cr);
	pm_build(m4, ur, cr, N.data(), G.data(), 1);
	pm_apply(m4, ur, cr, fb.data(), SW, SH, G.data());
	CHECK(count_cursor_px(s, fb) == 0, "corner: %d", count_cursor_px(s, fb));
	printf("corner mask: count %d of %d provisional %d\n", m4->count, m4->w * m4->h, m4->provisional);

	// cache: replacement of the least recently used entry, handle with another size dropped
	for (int i = 0; i < 12; i++) pm_cache_take(&cache, (void *)(size_t)(0x2000 + i), 10, 10);
	CHECK(pm_cache_find(&cache, (void *)0x2000, 10, 10) == NULL, "oldest replaced");
	CHECK(pm_cache_find(&cache, (void *)0x200b, 10, 10) != NULL, "newest present");
	CHECK(pm_cache_find(&cache, (void *)0x200b, 11, 10) == NULL, "other size dropped");
	pm_cache_free(&cache);

	// finding the cursor: sprite of the opaque pixels of the arrow, composed into the image at several places (also cut by the screen edge)
	{
		pm_sprite sp; memset(&sp, 0, sizeof(sp));
		sp.w = CW; sp.h = CH; sp.hx = HX; sp.hy = HY;
		sp.x = (int *)malloc(sizeof(int) * CW * CH); sp.y = (int *)malloc(sizeof(int) * CW * CH); sp.rgb = (unsigned char *)malloc(CW * CH * 3);
		for (int y = 0; y < CH; y++) for (int x = 0; x < CW; x++) if (sprite(x, y)) { int n = sp.n++; sp.x[n] = x; sp.y[n] = y; sp.rgb[n * 3] = sp.rgb[n * 3 + 1] = sp.rgb[n * 3 + 2] = (unsigned char)(x * 17 + y * 5 + 1); }
		pm_sprite_spread(&sp);
		std::vector<unsigned char> img((size_t)SW * SH * 4);
		static const int at[][2] = { { 30, 20 }, { 150, 90 }, { -1, 50 }, { 190, 100 } }; // bitmap top-left, the last two cut by the screen edge
		for (int k = 0; k < 4; k++)
		{
			for (size_t i = 0; i < img.size(); i += 4) { img[i] = (unsigned char)(i * 31 / 4); img[i + 1] = (unsigned char)(i / 4 + 7); img[i + 2] = (unsigned char)(i / 3); }
			for (int i = 0; i < sp.n; i++) { int x = at[k][0] + sp.x[i], y = at[k][1] + sp.y[i]; if (x >= 0 && y >= 0 && x < SW && y < SH) { unsigned char *d = pm_fb(img.data(), SW, SH, x, y); memcpy(d, sp.rgb + i * 3, 3); } }
			pm_rect q = { at[k][0] - 3, at[k][1] - 3, at[k][0] + CW + 3, at[k][1] + CH + 3 };
			if (q.left < 0) q.left = 0; if (q.top < 0) q.top = 0; if (q.right > SW) q.right = SW; if (q.bottom > SH) q.bottom = SH;
			int bx = 0, by = 0, hits = 0; long long tried = 0;
			int found = pm_find(img.data(), SW, SH, &sp, q, &bx, &by, &hits, 1000000, &tried);
			CHECK(found && bx == at[k][0] && by == at[k][1], "find %d: found %d at %d,%d expected %d,%d (hits %d, tried %lld)", k, found, bx, by, at[k][0], at[k][1], hits, tried);
			// an image without the sprite: nothing found
			for (int i = 0; i < sp.n; i++) { int x = at[k][0] + sp.x[i], y = at[k][1] + sp.y[i]; if (x >= 0 && y >= 0 && x < SW && y < SH) { unsigned char *d = pm_fb(img.data(), SW, SH, x, y); d[0] ^= 0x55; } }
			CHECK(!pm_find(img.data(), SW, SH, &sp, q, &bx, &by, &hits, 1000000, &tried), "find %d: found in an image without it", k);
		}
		// cost of searching a 200x200 rectangle
		pm_rect big = { 0, 0, 200, 120 }; int bx, by, hits; long long tried = 0;
		pm_find(img.data(), SW, SH, &sp, big, &bx, &by, &hits, 100000000, &tried);
		printf("search of %dx%d: %lld positions\n", big.right, big.bottom, tried);
		pm_sprite_free(&sp);
	}
	{
		pm_cache cache2; memset(&cache2, 0, sizeof(cache2));

	// moving cursor: the frame is composed with the cursor at P, GetCursorInfo says Q; regions = the rectangle of the frame; find P, restore under the mask
	{
		pm_mask *me = pm_cache_take(&cache2, (void *)0x7777, CW + 2 * M, CH + 2 * M);
		me->sp.w = CW; me->sp.h = CH; me->sp.hx = HX; me->sp.hy = HY;
		me->sp.x = (int *)malloc(sizeof(int) * CW * CH); me->sp.y = (int *)malloc(sizeof(int) * CW * CH); me->sp.rgb = (unsigned char *)malloc(CW * CH * 3); me->sp.aff = (unsigned char *)calloc(CW * CH, 1);
		for (int y = 0; y < CH; y++) for (int x = 0; x < CW; x++) if (sprite(x, y)) { int n = me->sp.n++; me->sp.x[n] = x; me->sp.y[n] = y; me->sp.aff[y * CW + x] = 1; me->sp.rgb[n * 3] = me->sp.rgb[n * 3 + 1] = me->sp.rgb[n * 3 + 2] = (unsigned char)(255 - 0); }
		pm_sprite_spread(&me->sp);
		CHECK(pm_mask_from_sprite(me, M) && me->m != NULL && me->count > me->sp.n, "mask from the sprite: %d (opaque %d)", me->count, me->sp.n);
		Screen s2; std::vector<unsigned char> fb2((size_t)SW * SH * 4); dxgi_copy(s2, fb2, all, -1000, -1000);
		int P[2] = { 80, 50 }, Q[2] = { 96, 61 };
		pm_rect fr = { P[0] - 6, P[1] - 4, P[0] + CW + 6, P[1] + CH + 4 };
		pm_region reg[PM_REGIONS]; memset(reg, 0, sizeof(reg));
		CHECK(pm_region_save(fb2.data(), SW, SH, &reg[0], fr), "save");
		for (int y = fr.top; y < fr.bottom; y++) for (int x = fr.left; x < fr.right; x++) { unsigned char *d = pm_fb(fb2.data(), SW, SH, x, y); memcpy(d, pm_fb(s2.content_.data(), SW, SH, x, y), 4); if (sprite(x - (P[0]), y - (P[1]))) { d[0] = d[1] = d[2] = 255; } } // composed with the opaque colour
		pm_rect ur = { Q[0] - HX - M, Q[1] - HY - M, Q[0] - HX - M + CW + 2 * M, Q[1] - HY - M + CH + 2 * M };
		// the position GetCursorInfo gave (Q) is wrong: look for the cursor in the region
		int bx = 0, by = 0, hits = 0; long long tried = 0;
		CHECK(pm_find(fb2.data(), SW, SH, &me->sp, reg[0].r, &bx, &by, &hits, 200000, &tried) && bx == P[0] && by == P[1], "found %d,%d expected %d,%d", bx, by, P[0], P[1]);
		pm_rect ur2 = { bx - M, by - M, bx + CW + M, by + CH + M }, cr2 = ur2;
		int o = pm_restore_regions(me, ur2, cr2, fb2.data(), SW, SH, reg, 1);
		CHECK(count_cursor_px(s2, fb2) == 0, "restored at the place where it was found: %d pixels left (outside %d)", count_cursor_px(s2, fb2), o);
		(void)ur;
		pm_regions_free(reg, PM_REGIONS);
	}
		pm_cache_free(&cache2);
	}
	// pm_sprite_spread is a permutation: every opaque pixel exactly once (n = 4..4000 all, then every 97th up to 65536)
	{
		int bad = 0, tested = 0;
		for (int n = 4; n <= 65536; n += (n < 4000 ? 1 : 97))
		{
			pm_sprite sp; memset(&sp, 0, sizeof(sp));
			sp.n = n; sp.x = (int *)malloc(sizeof(int) * n); sp.y = (int *)malloc(sizeof(int) * n); sp.rgb = (unsigned char *)malloc((size_t)n * 3);
			for (int i = 0; i < n; i++) { sp.x[i] = i; sp.y[i] = n - i; sp.rgb[i * 3] = (unsigned char)i; sp.rgb[i * 3 + 1] = sp.rgb[i * 3 + 2] = 0; }
			pm_sprite_spread(&sp);
			std::vector<unsigned char> seen((size_t)n, 0);
			bool ok = true;
			for (int i = 0; i < n && ok; i++) { if (sp.x[i] < 0 || sp.x[i] >= n || seen[sp.x[i]] || sp.y[i] != n - sp.x[i] || sp.rgb[i * 3] != (unsigned char)sp.x[i]) { ok = false; } else { seen[sp.x[i]] = 1; } }
			if (!ok) { bad++; if (bad <= 3) { printf("not a permutation: n = %d\n", n); } }
			tested++;
			pm_sprite_free(&sp);
		}
		CHECK(bad == 0, "pm_sprite_spread: %d of %d sizes are not permutations", bad, tested);
		printf("spread: %d sizes tested\n", tested);
	}
	// pm_pick: the same cursor twice in the frame (a screenshot of a desktop inside the frame): the one near the expected place wins
	{
		pm_cand c[3] = { { 300, 200, 70 }, { 40, 30, 70 }, { 41, 31, 50 } };
		int ex[2] = { 38, 500 }, ey[2] = { 29, 500 };
		CHECK(pm_pick(c, 3, ex, ey, 2) == 1, "pm_pick near candidate: %d", pm_pick(c, 3, ex, ey, 2));
		pm_cand one[1] = { { 900, 900, 40 } };
		CHECK(pm_pick(one, 1, ex, ey, 2) == 0, "a single candidate far away is taken");
		pm_cand weak[2] = { { 40, 30, 20 }, { 300, 200, 70 } };
		CHECK(pm_pick(weak, 2, ex, ey, 2) == 1, "a much weaker candidate near does not beat a full one: %d", pm_pick(weak, 2, ex, ey, 2));
		CHECK(pm_pick(c, 0, ex, ey, 2) == -1, "no candidates");
	}
	// more than PM_REGIONS small rectangles: the ones near the cursor must be saved first (the rule of dx_precopy, here as the ordering it relies on)
	{
		pm_rect win = { 90, 50, 130, 90 };
		int order[40], no = 0;
		for (int pass = 0; pass < 2; pass++) for (int i = 0; i < 40; i++)
		{
			pm_rect q = { i * 4, i * 2, i * 4 + 6, i * 2 + 6 };
			bool near = q.left < win.right && q.right > win.left && q.top < win.bottom && q.bottom > win.top;
			if ((pass == 0) == near) { order[no++] = i; }
		}
		int posOfNear = -1; for (int k = 0; k < no; k++) { pm_rect q = { order[k] * 4, order[k] * 2, order[k] * 4 + 6, order[k] * 2 + 6 }; if (q.left < win.right && q.right > win.left && q.top < win.bottom && q.bottom > win.top) { posOfNear = k; break; } }
		CHECK(no == 40 && posOfNear == 0, "near rectangles come first (%d)", posOfNear);
	}
	printf(g_fail ? "FAILED %d\n" : "ALL OK\n", g_fail);
	return g_fail ? 1 : 0;
}
