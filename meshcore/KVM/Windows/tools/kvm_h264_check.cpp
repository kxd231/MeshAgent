// kvm-h264-check: stand-alone probe and benchmark of the H.264 path of the Windows KVM (mf_h264.cpp and kvm_yuv.c, no agent code).
//
// Needs no desktop session: the test pictures are drawn with GDI into memory bitmaps, so it also runs as a service account, from a
// remote command line. Every measurement is printed twice: as a table row and as one line of JSON (lines that start with '{').
// The exit code is 0 when the mode completed, 1 when a measurement failed, 2 for a missing Media Foundation / wrong usage.
//
//   probe [--size WxH ...]      OS version, whether Media Foundation and the encoder load, the codec API properties of the encoder with
//                               their range and default, the rate control modes that it accepts, the frame sizes that it accepts, a short
//                               encode (profile, level, SPS/PPS, IDR, 1 in 1 out) and "qpcheck": which way of setting the quantizer
//                               really changes the size of the frames.
//   qpsample the ways of steering the size of the frames, one after the other (about 10 s): the QP through the codec API, the QP also in the input sample
//                               (MFSampleExtension_VideoEncodeQP), the sample attribute alone, and the bit rate of CBR / LowDelayVBR / PeakConstrainedVBR / UnconstrainedVBR set during
//                               the stream; the QP that the encoder reports in its output sample. `probe --qpsample` runs it after the probe.
//   bench [--max-seconds N] [--frames N] [--only 1080|1440|2160] [--rc quality|vbr|cbr|default] [--qpm 1..4] [--qvs N] [--profile 66|77|100]
//                               300 frames of a synthetic desktop (scrolling ClearType text on white, a moving window, a noise / video
//                               patch) at 1080p, then 1440p and 4K while the time and the speed allow, QP 24 / 30 / 36; then a change
//                               of QP and of the bit rate in the middle of a stream. N seconds (default 100) is the budget of the bench.
//   --hw [probe,bench,dump] [options] the HARDWARE H.264 encoder next to the software one: MFTEnumEx, codec API, QP set in the middle of the stream, latency / throughput / CPU / sizes of
//                               synthetic NV12 pictures, a dump of the first access units checked against the rules of the viewer (kvm_h264_rules.c). No capture, no desktop; at most
//                               2 minutes; see the header of kvm_hwprobe_win.cpp for the options, the output and the exit codes.
//   gentest [--bmp]             no Media Foundation: cost of the test pictures and of the BGRA to NV12 conversion on this machine
//   dump [--w W --h H --frames N --qp Q --out out.h264 --prefix p]
//                               writes the Annex B stream, a line per access unit (out.txt), the source of 9 frames as 24 bit BMP (src_NNNN.bmp,
//                               listed in saved.txt) and out.json (per frame: size, IDR, forced, QP, NAL types; SPS profile / level; statistics)
//                               for an offline analysis of the stream (VUI, PSNR). QP is Q in the first and last third and Q + 8 in the
//                               middle; a forced IDR in the middle frame.
//
// Build: cl /O2 /MT /EHsc mf_h264.cpp kvm_yuv.c kvm_annexb.c kvm_h264_check.cpp, libs user32 gdi32 and, with --hw,
// the same plus /DKVM_HWPROBE, kvm_h264_rules.c, tools/kvm_hwprobe_core.cpp, tools/kvm_hwprobe_win.cpp). Without /DKVM_HWPROBE the tool is what it was.

#include "../mf_h264.h"
#include "../../common/kvm_yuv.h"
#ifdef KVM_HWPROBE
#include "kvm_hwprobe.h"
#endif

#define NOMINMAX
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------------------------------------------------- helpers

static double now_ms(void)
{
	static LARGE_INTEGER freq;
	LARGE_INTEGER c;
	if (freq.QuadPart == 0) { QueryPerformanceFrequency(&freq); }
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static double cpu_ms(void) // user + kernel time of the process
{
	FILETIME c, e, k, u;
	if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) { return 0; }
	return (double)(((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime)) / 10000.0;
}

static void out(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}

static double g_t0 = 0;
static int g_threads = 0; // --threads N: AVEncNumWorkerThreads (0 = encoder default)
static double elapsed_s(void) { return (now_ms() - g_t0) / 1000.0; }

struct Series
{
	std::vector<double> v;
	void add(double x) { v.push_back(x); }
	double mean() const { if (v.empty()) { return 0; } double s = 0; for (size_t i = 0; i < v.size(); i++) { s += v[i]; } return s / (double)v.size(); }
	double max() const { double m = 0; for (size_t i = 0; i < v.size(); i++) { if (v[i] > m) { m = v[i]; } } return m; }
	double pct(double p) const
	{
		if (v.empty()) { return 0; }
		std::vector<double> c(v);
		std::sort(c.begin(), c.end());
		size_t i = (size_t)ceil(p * (double)c.size());
		if (i > 0) { i--; }
		if (i >= c.size()) { i = c.size() - 1; }
		return c[i];
	}
};

static void print_err(const char *what, int code, const mfh264_err &e)
{
	out("ERROR %s: %s stage=\"%s\" hr=0x%08lX reason=%d\n", what, mfh264_result_name(code), e.stage != NULL ? e.stage : "", (unsigned long)e.hr, e.reason);
	out("{\"kind\":\"error\",\"what\":\"%s\",\"result\":\"%s\",\"stage\":\"%s\",\"hr\":\"0x%08lX\",\"reason\":%d}\n", what, mfh264_result_name(code), e.stage != NULL ? e.stage : "", (unsigned long)e.hr, e.reason);
}

// Finds the first NAL unit of the given type in an Annex B buffer; returns a pointer to the NAL header byte or NULL.
static const uint8_t *find_nal(const uint8_t *p, size_t n, int type, size_t *left)
{
	for (size_t i = 0; i + 4 < n; i++)
	{
		if (p[i] == 0 && p[i + 1] == 0 && (p[i + 2] == 1 || (p[i + 2] == 0 && p[i + 3] == 1)))
		{
			const size_t o = (p[i + 2] == 1) ? i + 3 : i + 4;
			if (o < n && (p[o] & 0x1F) == type) { if (left != NULL) { *left = n - o; } return p + o; }
		}
	}
	return NULL;
}

// NAL types of the first 12 NAL units, e.g. "7 8 6 5".
static void nal_list(const uint8_t *p, size_t n, char *dst, size_t cap)
{
	size_t w = 0;
	int count = 0;
	dst[0] = 0;
	for (size_t i = 0; i + 4 < n && count < 12; i++)
	{
		if (p[i] == 0 && p[i + 1] == 0 && (p[i + 2] == 1 || (p[i + 2] == 0 && p[i + 3] == 1)))
		{
			const size_t o = (p[i + 2] == 1) ? i + 3 : i + 4;
			if (o < n && w + 4 < cap) { w += (size_t)_snprintf(dst + w, cap - w, "%s%d", count ? " " : "", p[o] & 0x1F); count++; }
			i = o;
		}
	}
}

// ------------------------------------------------------------------------------------------------------- synthetic desktop (GDI)

static uint32_t g_rng = 12345;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

struct Dib
{
	HDC dc;
	HBITMAP bmp, old;
	uint8_t *bits;
	int w, h;
};

static int dib_create(Dib *d, int w, int h)
{
	BITMAPINFO bi;
	memset(&bi, 0, sizeof(bi));
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h; // top-down
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	d->w = w; d->h = h; d->bits = NULL;
	d->dc = CreateCompatibleDC(NULL);
	if (d->dc == NULL) { return 0; }
	d->bmp = CreateDIBSection(d->dc, &bi, DIB_RGB_COLORS, (void **)&d->bits, NULL, 0);
	if (d->bmp == NULL || d->bits == NULL) { DeleteDC(d->dc); d->dc = NULL; return 0; }
	d->old = (HBITMAP)SelectObject(d->dc, d->bmp);
	return 1;
}

static void dib_destroy(Dib *d)
{
	if (d->dc != NULL) { SelectObject(d->dc, d->old); DeleteObject(d->bmp); DeleteDC(d->dc); }
	memset(d, 0, sizeof(*d));
}

static const wchar_t *g_words[] = { L"the", L"remote", L"desktop", L"session", L"quality", L"frame", L"encoder", L"tile", L"update", L"server", L"agent", L"window", L"cursor", L"stream",
	L"bandwidth", L"latency", L"keyboard", L"service", L"connection", L"rectangle", L"buffer", L"profile", L"settings", L"network", L"display", L"monitor", L"value", L"index",
	L"return", L"static", L"const", L"int", L"if", L"while", L"0x7FFF", L"1920x1080", L"error", L"warning", L"OK" };
#define N_WORDS ((int)(sizeof(g_words) / sizeof(g_words[0])))

static void draw_text_page(Dib *d, int lineH, int fontPx, const wchar_t *face)
{
	PatBlt(d->dc, 0, 0, d->w, d->h, WHITENESS);
	HFONT f = CreateFontW(-fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
	HGDIOBJ oldf = SelectObject(d->dc, f);
	SetBkMode(d->dc, OPAQUE);
	SetBkColor(d->dc, RGB(255, 255, 255));
	for (int y = 4; y + lineH < d->h; y += lineH)
	{
		wchar_t line[200];
		int len = 0, nw = 5 + (int)(rnd() % 10);
		const unsigned pick = rnd() % 100;
		SetTextColor(d->dc, pick < 80 ? RGB(0, 0, 0) : (pick < 90 ? RGB(0, 0, 170) : (pick < 95 ? RGB(170, 0, 0) : RGB(0, 120, 0))));
		for (int k = 0; k < nw && len < 170; k++)
		{
			const wchar_t *wd = g_words[rnd() % N_WORDS];
			for (; *wd != 0 && len < 190; wd++) { line[len++] = *wd; }
			line[len++] = L' ';
		}
		TextOutW(d->dc, 8, y, line, len);
	}
	SelectObject(d->dc, oldf);
	DeleteObject(f);
}

struct Gen
{
	int w, h;                 // size of the picture
	int tw;                   // width of the text pane
	int ww, wh;               // moving window
	int vw, vh, vx, vy;       // noise / video patch
	int scroll;               // pixels per frame
	Dib frame, page, backdrop, window;
	uint8_t sine[256];
};

static int gen_init(Gen *g, int w, int h)
{
	memset(g, 0, sizeof(*g));
	g_rng = 12345;
	g->w = w; g->h = h;
	g->tw = (w * 3 / 5) & ~1;
	g->ww = (w / 3) & ~1; g->wh = (h / 3) & ~1;
	g->vw = (w / 4) & ~1; g->vh = (h / 4) & ~1;
	g->vx = w - g->vw - w / 50; g->vy = h - g->vh - h / 30;
	g->scroll = std::max(2, 3 * h / 1080);
	for (int i = 0; i < 256; i++) { g->sine[i] = (uint8_t)(127.5 + 127.5 * sin(i * 3.14159265358979 * 2 / 256)); }
	const int fontPx = std::max(13, std::min(28, h / 60));
	const int lineH = fontPx * 14 / 10 + 2;
	if (!dib_create(&g->frame, w, h) || !dib_create(&g->page, g->tw, h * 2) || !dib_create(&g->backdrop, w - g->tw, h) || !dib_create(&g->window, g->ww, g->wh)) { return 0; }
	draw_text_page(&g->page, lineH, fontPx, L"Segoe UI");
	// backdrop: a light gradient with a few icons and labels
	{
		Dib *b = &g->backdrop;
		for (int y = 0; y < b->h; y++)
		{
			const uint8_t c = (uint8_t)(205 + 30 * y / b->h);
			uint32_t px = (uint32_t)c | ((uint32_t)(c - 8) << 8) | ((uint32_t)(c - 20) << 16);
			uint32_t *row = (uint32_t *)(b->bits + (size_t)y * b->w * 4);
			for (int x = 0; x < b->w; x++) { row[x] = px; }
		}
		HFONT f = CreateFontW(-std::max(12, h / 70), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
		HGDIOBJ of = SelectObject(b->dc, f);
		SetBkMode(b->dc, TRANSPARENT);
		SetTextColor(b->dc, RGB(20, 20, 20));
		for (int i = 0; i < 12; i++)
		{
			const int ix = 20 + (i % 3) * (b->w / 3), iy = 20 + (i / 3) * (b->h / 5);
			HBRUSH br = CreateSolidBrush(RGB(40 + (rnd() % 180), 40 + (rnd() % 180), 40 + (rnd() % 180)));
			RECT rc = { ix, iy, ix + 48, iy + 48 };
			FillRect(b->dc, &rc, br);
			DeleteObject(br);
			wchar_t lbl[32];
			const int n = _snwprintf(lbl, 32, L"Item %d", i + 1);
			TextOutW(b->dc, ix, iy + 52, lbl, n);
		}
		SelectObject(b->dc, of);
		DeleteObject(f);
	}
	// the window: title bar, border, text
	{
		Dib *wnd = &g->window;
		const int fpx = std::max(12, h / 70);
		draw_text_page(wnd, fpx * 14 / 10 + 2, fpx, L"Consolas");
		RECT title = { 0, 0, wnd->w, std::max(24, h / 40) };
		HBRUSH tb = CreateSolidBrush(RGB(0, 90, 170));
		FillRect(wnd->dc, &title, tb);
		DeleteObject(tb);
		HBRUSH fb = CreateSolidBrush(RGB(90, 90, 90));
		RECT rc = { 0, 0, wnd->w, wnd->h };
		FrameRect(wnd->dc, &rc, fb);
		DeleteObject(fb);
		HFONT f = CreateFontW(-std::max(14, h / 55), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
		HGDIOBJ of = SelectObject(wnd->dc, f);
		SetBkMode(wnd->dc, TRANSPARENT);
		SetTextColor(wnd->dc, RGB(255, 255, 255));
		TextOutW(wnd->dc, 8, 3, L"Notes - moving window", 21);
		SelectObject(wnd->dc, of);
		DeleteObject(f);
	}
	GdiFlush();
	return 1;
}

static void gen_destroy(Gen *g)
{
	dib_destroy(&g->frame); dib_destroy(&g->page); dib_destroy(&g->backdrop); dib_destroy(&g->window);
}

// Picture number t into g->frame.bits (BGRA, top-down, stride w * 4).
static const uint8_t *gen_frame(Gen *g, int t)
{
	const int ph = g->page.h;
	const int off = (int)(((long long)t * g->scroll) % ph);
	int h1 = std::min(g->h, ph - off);
	BitBlt(g->frame.dc, 0, 0, g->tw, h1, g->page.dc, 0, off, SRCCOPY);
	if (h1 < g->h) { BitBlt(g->frame.dc, 0, h1, g->tw, g->h - h1, g->page.dc, 0, 0, SRCCOPY); }
	BitBlt(g->frame.dc, g->tw, 0, g->w - g->tw, g->h, g->backdrop.dc, 0, 0, SRCCOPY);
	{
		const double ax = 0.5 + 0.5 * sin(t * 0.05), ay = 0.5 + 0.5 * sin(t * 0.031 + 1.0);
		const int wx = g->tw + (int)((g->w - g->tw - g->ww) * ax), wy = (int)((g->h - g->wh) * ay);
		BitBlt(g->frame.dc, wx, wy, g->ww, g->wh, g->window.dc, 0, 0, SRCCOPY);
	}
	GdiFlush();
	// video / noise patch: moving plasma with noise, written straight into the bits
	{
		uint32_t seed = (uint32_t)t * 2654435761u + 1;
		for (int y = 0; y < g->vh; y++)
		{
			uint8_t *row = g->frame.bits + ((size_t)(g->vy + y) * g->w + g->vx) * 4;
			const int v2 = g->sine[(y * 3 - t * 2) & 255];
			for (int x = 0; x < g->vw; x++)
			{
				const int v1 = g->sine[(x * 2 + t * 3) & 255];
				seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
				const int n = (int)(seed & 15) - 8;
				int r = 64 + (v1 >> 1) + n, gg = 64 + (v2 >> 1) + n, b = 64 + ((v1 + v2) >> 2) + n;
				row[0] = (uint8_t)std::max(0, std::min(255, b));
				row[1] = (uint8_t)std::max(0, std::min(255, gg));
				row[2] = (uint8_t)std::max(0, std::min(255, r));
				row[3] = 255;
				row += 4;
			}
		}
	}
	return g->frame.bits;
}

// -------------------------------------------------------------------------------------------------------------------- encoding

// 24 bit bottom-up BMP of a BGRA picture (stride w * 4), for a comparison with the decoded stream in another program.
static int write_bmp(const char *name, const uint8_t *src, int w, int hh)
{
	FILE *bf = fopen(name, "wb");
	if (bf == NULL) { return 0; }
	const int rowBytes = (w * 3 + 3) & ~3;
	BITMAPFILEHEADER fh;
	BITMAPINFOHEADER ih;
	memset(&fh, 0, sizeof(fh)); memset(&ih, 0, sizeof(ih));
	fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + (DWORD)rowBytes * hh;
	ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = hh; ih.biPlanes = 1; ih.biBitCount = 24; ih.biCompression = BI_RGB;
	fwrite(&fh, sizeof(fh), 1, bf); fwrite(&ih, sizeof(ih), 1, bf);
	std::vector<uint8_t> row((size_t)rowBytes, 0);
	for (int y = hh - 1; y >= 0; y--)
	{
		const uint8_t *q = src + (size_t)y * w * 4;
		for (int x = 0; x < w; x++) { row[(size_t)x * 3] = q[x * 4]; row[(size_t)x * 3 + 1] = q[x * 4 + 1]; row[(size_t)x * 3 + 2] = q[x * 4 + 2]; }
		fwrite(row.data(), 1, (size_t)rowBytes, bf);
	}
	fclose(bf);
	return 1;
}

struct EncRun
{
	Series conv, enc, bytesP, gen;
	std::vector<int> sizes;        // every access unit (0 for a frame that gave none)
	std::vector<int> idr;          // 1 if IDR
	std::vector<int> forcedAt;     // 1 if mfh264_force_idr() was called before the frame
	std::vector<int> qps;          // QP set for the frame (-1: not set)
	std::vector<std::string> nals; // NAL types of the access unit
	size_t idrFirst;
	Series idrForced;
	int forcedAsked, forcedOk;
	int brokenCalls;               // calls that returned MFH264_E_STREAM_BROKEN (the wrapper flushed and forced an IDR)
	int idrTotal, idrUnforced;     // IDR access units; IDR that nobody asked for (the wrapper's count, GOP check)
	int streamChanges, noOutput, extraOutput, oversize, incomplete, audRemoved, headersAdded;
	int spsProfile, spsLevel, levelRequested, levelSetOk;
	int failed;                    // result of the failing call, 0 if none
	double wallMs, cpuMs;
	int frames;
	EncRun() : idrFirst(0), forcedAsked(0), forcedOk(0), brokenCalls(0), idrTotal(0), idrUnforced(0), streamChanges(0), noOutput(0), extraOutput(0), oversize(0), incomplete(0),
		audRemoved(0), headersAdded(0), spsProfile(0), spsLevel(0), levelRequested(0), levelSetOk(0), failed(0), wallMs(0), cpuMs(0), frames(0) {}
};

struct QpMode { int rc; int method; int valid; };

// Encodes 'frames' pictures of the generator. QP: qpPerFrame[f] if given, else the constant qp (< 0 = the configuration decides, no mfh264_set_qp);
// mfh264_set_qp() is called when the value changes. forceAt: frames that get mfh264_force_idr() before the encode (may be NULL). dump: optional
// file for the Annex B stream, dumpTxt a line per access unit; the sources of the frames in bmpAt are written as BMP.
static int run_encode(Gen *g, int cw, int ch, int frames, const mfh264_cfg &cfgIn, int qp, const int *qpPerFrame, const int *forceAt, int nForce, EncRun *r, FILE *dump, FILE *dumpTxt, const int *bmpAt, int nBmp, const char *prefix)
{
	mfh264_cfg cfg = cfgIn;
	cfg.width = cw; cfg.height = ch;
	if (cfg.worker_threads == 0) { cfg.worker_threads = g_threads; }
	mfh264_err e;
	memset(&e, 0, sizeof(e));
	mfh264 *h = mfh264_open(&cfg, &e);
	if (h == NULL) { print_err("open", e.code, e); r->failed = e.code; return e.code; }
	const size_t nvLen = kvm_yuv_nv12_size(cw, ch);
	uint8_t *nv = (uint8_t *)malloc(nvLen);
	const size_t cap = mfh264_max_au_size(h);
	uint8_t *ob = (uint8_t *)malloc(cap);
	if (nv == NULL || ob == NULL) { free(nv); free(ob); mfh264_close(h); r->failed = MFH264_E_ALLOC; return r->failed; }
	const double w0 = now_ms(), c0 = cpu_ms();
	int result = 0, lastQp = -1;
	for (int f = 0; f < frames; f++)
	{
		const int wantQp = qpPerFrame != NULL ? qpPerFrame[f] : qp;
		if (wantQp >= 0 && wantQp != lastQp)
		{
			const int rc = mfh264_set_qp(h, wantQp, &e);
			if (rc != MFH264_OK) { print_err("set_qp", rc, e); r->failed = rc; result = rc; break; }
			lastQp = wantQp;
		}
		double t0 = now_ms();
		const uint8_t *src = gen_frame(g, f);
		const double tg = now_ms() - t0;
		t0 = now_ms();
		kvm_yuv_bgra_to_nv12(src, g->w, g->h, g->w * 4, nv, cw, ch, KVM_YUV_AUTO);
		const double tc = now_ms() - t0;
		int forced = 0;
		for (int k = 0; k < nForce; k++) { if (forceAt[k] == f) { forced = 1; } }
		if (forced)
		{
			r->forcedAsked++;
			if (mfh264_force_idr(h, &e) != MFH264_OK) { print_err("force_idr", e.code, e); }
		}
		size_t ol = 0;
		int isIdr = 0;
		t0 = now_ms();
		const int rc = mfh264_encode(h, nv, nvLen, (int64_t)f * 10000000LL / MFH264_NOMINAL_FPS, ob, cap, &ol, &isIdr, &e);
		const double te = now_ms() - t0;
		r->gen.add(tg); r->conv.add(tc);
		r->forcedAt.push_back(forced);
		r->qps.push_back(wantQp);
		if (rc == MFH264_E_STREAM_BROKEN)
		{
			// the frame is lost, the wrapper flushed the encoder and the next frame is an IDR: the caller's answer is to go on
			r->brokenCalls++;
			out("  frame %d: STREAM_BROKEN reason %d (%s), the next frame is an IDR\n", f, e.reason, e.stage != NULL ? e.stage : "");
			r->sizes.push_back(0); r->idr.push_back(0); r->nals.push_back("lost");
			if (r->brokenCalls > 10) { r->failed = rc; result = rc; break; }
			continue;
		}
		if (rc < 0) { print_err("encode", rc, e); r->failed = rc; result = rc; break; }
		r->enc.add(te);
		r->frames++;
		r->sizes.push_back((int)ol);
		r->idr.push_back(isIdr);
		{
			char nl[64];
			nal_list(ob, ol, nl, sizeof(nl));
			r->nals.push_back(nl);
		}
		if (isIdr)
		{
			if (r->idrFirst == 0) { r->idrFirst = ol; }
			else if (forced) { r->idrForced.add((double)ol); }
		}
		else { r->bytesP.add((double)ol); }
		if (forced && isIdr) { r->forcedOk++; }
		if (dump != NULL) { fwrite(ob, 1, ol, dump); }
		if (dumpTxt != NULL) { fprintf(dumpTxt, "%d %u %d %d\n", f, (unsigned)ol, isIdr, wantQp); }
		for (int k = 0; k < nBmp; k++)
		{
			if (bmpAt[k] == f && prefix != NULL)
			{
				char name[260];
				_snprintf(name, sizeof(name), "%ssrc_%04d.bmp", prefix, f);
				write_bmp(name, src, g->w, g->h);
			}
		}
	}
	r->wallMs = now_ms() - w0;
	r->cpuMs = cpu_ms() - c0;
	mfh264_stats st;
	mfh264_info in;
	mfh264_get_stats(h, &st);
	mfh264_get_info(h, &in);
	r->extraOutput = (int)st.extra_output; r->noOutput = (int)st.no_output; r->oversize = (int)st.oversize; r->incomplete = (int)st.incomplete;
	r->streamChanges = (int)st.stream_changes; r->idrTotal = (int)st.idr; r->idrUnforced = (int)st.idr_unforced;
	r->audRemoved = (int)st.aud_removed; r->headersAdded = (int)st.headers_added;
	r->spsProfile = in.sps_profile_idc; r->spsLevel = in.sps_level_idc; r->levelRequested = in.level_requested; r->levelSetOk = in.level_set_ok;
	mfh264_close(h);
	free(nv);
	free(ob);
	return result;
}

// ---------------------------------------------------------------------------------------------------------------------- probe

static const char *vt_name(int vt)
{
	switch (vt) { case 11: return "bool"; case 3: return "i4"; case 19: return "ui4"; case 20: return "i8"; case 21: return "ui8"; case 18: return "ui2"; case 2: return "i2"; case 17: return "ui1"; case 0: return "?"; }
	return "other";
}

// Which way of setting the QP changes the size of the frames? Encodes 'frames' pictures of a 1280x720 stream at QP 20 and QP 40 for every
// combination and compares the mean size of the P frames. Returns the first effective combination in the order of preference.
struct QpCombo { int rc, method; int opened, setOk; double lowQp, highQp; int effective; long hr; };

static const char *rc_name(int rc) { return rc == MFH264_RC_DEFAULT ? "default" : rc == MFH264_RC_CBR ? "cbr" : rc == MFH264_RC_QUALITY ? "quality" : "vbr"; }

static int qp_discovery(int profile, int qvs, QpMode *chosen, int quiet)
{
	static const int order[][2] = { { MFH264_RC_QUALITY, 1 }, { MFH264_RC_QUALITY, 2 }, { MFH264_RC_QUALITY, 3 }, { MFH264_RC_VBR, 1 }, { MFH264_RC_VBR, 2 }, { MFH264_RC_VBR, 3 },
		{ MFH264_RC_DEFAULT, 1 }, { MFH264_RC_DEFAULT, 2 }, { MFH264_RC_DEFAULT, 3 }, { MFH264_RC_QUALITY, 4 }, { MFH264_RC_VBR, 4 } };
	const int n = (int)(sizeof(order) / sizeof(order[0]));
	Gen g;
	chosen->valid = 0;
	if (!gen_init(&g, 1280, 720)) { out("qpcheck: cannot create the test pictures\n"); return 0; }
	if (!quiet) { out("\nqpcheck: P frame bytes at QP 20 and QP 40, 1280x736, 14 frames each (a form works when QP 20 gives > 1.4 x the bytes of QP 40)\n  %-8s %-26s %-8s %12s %12s  %s\n", "rate ctl", "form", "set_qp", "bytes QP20", "bytes QP40", "effective"); }
	for (int i = 0; i < n; i++)
	{
		QpCombo c;
		memset(&c, 0, sizeof(c));
		c.rc = order[i][0]; c.method = order[i][1];
		for (int pass = 0; pass < 2; pass++)
		{
			mfh264_cfg cfg = MFH264_CFG_INIT(1280, 736);
			cfg.profile = profile; cfg.rc = c.rc; cfg.qp_method = c.method; cfg.quality_vs_speed = qvs;
			EncRun r;
			if (run_encode(&g, 1280, 736, 14, cfg, pass == 0 ? 20 : 40, NULL, NULL, 0, &r, NULL, NULL, NULL, 0, NULL) == 0)
			{
				c.opened = 1; c.setOk = 1;
				(pass == 0 ? c.lowQp : c.highQp) = r.bytesP.mean();
			}
			else { c.opened = 0; break; }
		}
		c.effective = c.opened && c.lowQp > 0 && c.highQp > 0 && c.lowQp > 1.4 * c.highQp;
		static const char *mn[] = { "", "EncodeQP VT_UI4", "EncodeQP VT_UI8 (I|P|B)", "FrameTypeQP VT_UI8 (I|P|B)", "AVEncCommonQuality (100-qp*100/51)" };
		if (!quiet)
		{
			out("  %-8s %-26s %-8s %12.0f %12.0f  %s\n", rc_name(c.rc), mn[c.method], c.opened ? "ok" : "refused", c.lowQp, c.highQp, c.effective ? "YES" : "no");
		}
		out("{\"kind\":\"qpcheck\",\"rc\":\"%s\",\"form\":%d,\"accepted\":%d,\"bytes_qp20\":%.0f,\"bytes_qp40\":%.0f,\"effective\":%d}\n", rc_name(c.rc), c.method, c.opened, c.lowQp, c.highQp, c.effective);
		if (c.effective && !chosen->valid) { chosen->rc = c.rc; chosen->method = c.method; chosen->valid = 1; }
	}
	gen_destroy(&g);
	if (chosen->valid) { out("qpcheck: chosen rate control \"%s\", QP form %d\n", rc_name(chosen->rc), chosen->method); }
	else { out("qpcheck: NO form of setting the QP changed the frame size; the bench falls back to constant bit rate\n"); }
	out("{\"kind\":\"qpcheck_result\",\"valid\":%d,\"rc\":\"%s\",\"form\":%d}\n", chosen->valid, chosen->valid ? rc_name(chosen->rc) : "", chosen->valid ? chosen->method : 0);
	return chosen->valid;
}

static int mode_probe(int argc, char **argv, int profile)
{
	struct mfh264_err e;
	memset(&e, 0, sizeof(e));
	int extraSizes[8][2], nExtra = 0;
	for (int i = 2; i + 1 < argc && nExtra < 8; i++)
	{
		if (strcmp(argv[i], "--size") == 0) { int a, b; if (sscanf(argv[i + 1], "%dx%d", &a, &b) == 2) { extraSizes[nExtra][0] = a; extraSizes[nExtra][1] = b; nExtra++; } }
	}
	int rc = mfh264_init(&e);
	mfh264_caps caps;
	memset(&caps, 0, sizeof(caps));
	if (rc != MFH264_OK)
	{
		mfh264_probe(&caps, NULL); // fills the OS fields
		out("OS %d.%d build %d, Windows 8 or later: %d\n", caps.os_major, caps.os_minor, caps.os_build, caps.os_ok);
		print_err("init", rc, e);
		out("{\"kind\":\"probe\",\"os\":\"%d.%d.%d\",\"os_ok\":%d,\"init\":\"%s\",\"hr\":\"0x%08lX\"}\n", caps.os_major, caps.os_minor, caps.os_build, caps.os_ok, mfh264_result_name(rc), (unsigned long)e.hr);
		return 2;
	}
	rc = mfh264_probe(&caps, &e);
	out("OS %d.%d build %d (Windows 8 or later: %d); Media Foundation loaded: %d; encoder MFT created: %d (hr 0x%08lX); asynchronous MFT: %d\n", caps.os_major, caps.os_minor, caps.os_build, caps.os_ok, caps.mf_loaded, caps.mft_created, (unsigned long)caps.create_hr, caps.is_async);
	out("{\"kind\":\"probe\",\"os\":\"%d.%d.%d\",\"os_ok\":%d,\"mf_loaded\":%d,\"mft_created\":%d,\"create_hr\":\"0x%08lX\",\"async\":%d,\"result\":\"%s\"}\n", caps.os_major, caps.os_minor, caps.os_build, caps.os_ok, caps.mf_loaded, caps.mft_created, (unsigned long)caps.create_hr, caps.is_async, mfh264_result_name(rc));
	if (rc != MFH264_OK) { print_err("probe", rc, e); mfh264_shutdown(); return 2; }

	out("\ncodec API properties of the encoder\n  %-34s %-9s %-10s %-4s %14s %14s %8s %14s\n", "property", "supported", "modifiable", "type", "min", "max", "step", "default");
	for (int i = 0; i < caps.n_props; i++)
	{
		const mfh264_prop &p = caps.props[i];
		char mn[40] = "-", mx[40] = "-", st[24] = "-", df[40] = "-";
		if (p.has_range) { _snprintf(mn, sizeof(mn), "%lld", (long long)p.min); _snprintf(mx, sizeof(mx), "%lld", (long long)p.max); _snprintf(st, sizeof(st), "%lld", (long long)p.step); }
		if (p.has_default) { _snprintf(df, sizeof(df), "%lld", (long long)p.def); }
		out("  %-34s %-9s %-10s %-4s %14s %14s %8s %14s\n", p.name, p.supported ? "yes" : "no", p.supported ? (p.modifiable ? "yes" : "no") : "-", vt_name(p.vt), mn, mx, st, df);
		out("{\"kind\":\"prop\",\"name\":\"%s\",\"supported\":%d,\"modifiable\":%d,\"vt\":%d,\"min\":%lld,\"max\":%lld,\"step\":%lld,\"default\":%lld,\"has_range\":%d,\"has_default\":%d,\"hr\":\"0x%08lX\"}\n",
			p.name, p.supported, p.modifiable, p.vt, (long long)p.min, (long long)p.max, (long long)p.step, (long long)p.def, p.has_range, p.has_default, (unsigned long)p.hr_supported);
	}
	{
		static const char *rcn[] = { "CBR", "PeakConstrainedVBR", "UnconstrainedVBR", "Quality", "LowDelayVBR", "GlobalVBR", "GlobalLowDelayVBR" };
		out("\nAVEncCommonRateControlMode values accepted by SetValue:");
		for (int m = 0; m < 7; m++) { out(" %s=%s", rcn[m], caps.rc_mode_ok[m] ? "yes" : "no"); }
		out("\n{\"kind\":\"rc_modes\"");
		for (int m = 0; m < 7; m++) { out(",\"%s\":%d", rcn[m], caps.rc_mode_ok[m]); }
		out("}\n");
	}

	// frame sizes (coded sizes; 3840x2160 is not a multiple of 32 and is asked as well as 3840x2176)
	{
		std::vector<int> ws, hs;
		static const int std_sizes[][2] = { { 1280, 720 }, { 1920, 1088 }, { 2560, 1440 }, { 3840, 2160 }, { 3840, 2176 }, { 4096, 2304 }, { 5120, 2880 } };
		for (size_t i = 0; i < sizeof(std_sizes) / sizeof(std_sizes[0]); i++) { ws.push_back(std_sizes[i][0]); hs.push_back(std_sizes[i][1]); }
		for (int i = 0; i < nExtra; i++) { ws.push_back(extraSizes[i][0]); hs.push_back(extraSizes[i][1]); }
		out("\nframe sizes accepted (output type H.264 %s, then NV12 input)\n", profile == 100 ? "High" : profile == 77 ? "Main" : "Baseline");
		for (size_t i = 0; i < ws.size(); i++)
		{
			mfh264_err pe;
			memset(&pe, 0, sizeof(pe));
			const int r2 = mfh264_probe_size(ws[i], hs[i], profile, &pe);
			out("  %4dx%-4d  level %2d  %-18s %s hr=0x%08lX\n", ws[i], hs[i], mfh264_level_for_size(ws[i], hs[i]), r2 == 0 ? "accepted" : mfh264_result_name(r2), r2 == 0 ? "" : (pe.stage != NULL ? pe.stage : ""), (unsigned long)pe.hr);
			out("{\"kind\":\"size\",\"w\":%d,\"h\":%d,\"level_idc\":%d,\"accepted\":%d,\"result\":\"%s\",\"hr\":\"0x%08lX\"}\n", ws[i], hs[i], mfh264_level_for_size(ws[i], hs[i]), r2 == 0, mfh264_result_name(r2), (unsigned long)pe.hr);
		}
	}

	// a short encode: settings applied, profile / level in the SPS, parameter sets, IDR, 1 in 1 out
	int failed = 0;
	{
		Gen g;
		if (!gen_init(&g, 1920, 1080)) { out("cannot create the test pictures\n"); failed = 1; }
		else
		{
			mfh264_cfg cfg = MFH264_CFG_INIT(1920, 1088);
			cfg.profile = profile; cfg.quality_vs_speed = 0; cfg.worker_threads = g_threads;
			mfh264 *h = mfh264_open(&cfg, &e);
			if (h == NULL) { print_err("open 1920x1088", e.code, e); failed = 1; }
			else
			{
				mfh264_info in;
				mfh264_get_info(h, &in);
				out("\nopen 1920x1088: async=%d provides_samples=%d out_cb=%d in_cb=%d in_align=%d seq_header=%d(%d B) colour_attrs=%d\n", in.async, in.out_provides_samples, in.out_cb_size, in.in_cb_size, in.in_alignment, in.has_seq_header, in.seq_header_len, in.color_attrs_ok);
				out("  LowLatencyMode ok=%d (0x%08lX), B-frames=0 ok=%d (0x%08lX), GOP ok=%d value=%lld (0x%08lX), rate ctl ok=%d, QualityVsSpeed ok=%d (0x%08lX)\n", in.low_latency_ok, (unsigned long)in.low_latency_hr, in.bframes_ok, (unsigned long)in.bframes_hr, in.gop_ok, (long long)in.gop_value, (unsigned long)in.gop_hr, in.rc_mode_ok, in.qvs_ok, (unsigned long)in.qvs_hr);
				out("  level requested %d (set in the type: %d), SPS of the encoder: profile_idc %d level_idc %d, worker threads ok=%d, max AU size %d\n", in.level_requested, in.level_set_ok, in.sps_profile_idc, in.sps_level_idc, in.workers_ok, in.max_au_size);
				out("{\"kind\":\"open\",\"level_requested\":%d,\"level_set_ok\":%d,\"sps_profile_idc\":%d,\"sps_level_idc\":%d,\"async\":%d,\"provides_samples\":%d,\"out_cb\":%d,\"in_cb\":%d,\"seq_header\":%d,\"seq_header_len\":%d,\"colour_attrs\":%d,\"low_latency\":%d,\"bframes0\":%d,\"gop\":%d,\"gop_value\":%lld,\"rc_mode\":%d,\"qvs\":%d}\n",
					in.level_requested, in.level_set_ok, in.sps_profile_idc, in.sps_level_idc, in.async, in.out_provides_samples, in.out_cb_size, in.in_cb_size, in.has_seq_header, in.seq_header_len, in.color_attrs_ok, in.low_latency_ok, in.bframes_ok, in.gop_ok, (long long)in.gop_value, in.rc_mode_ok, in.qvs_ok);
				const size_t nvLen = kvm_yuv_nv12_size(1920, 1088);
				const size_t obCap = mfh264_max_au_size(h);
				uint8_t *nv = (uint8_t *)malloc(nvLen), *ob = (uint8_t *)malloc(obCap);
				if (mfh264_set_qp(h, 28, &e) != MFH264_OK) { print_err("set_qp", e.code, e); failed = 1; }
				for (int f = 0; f < 6 && !failed; f++)
				{
					const uint8_t *src = gen_frame(&g, f);
					kvm_yuv_bgra_to_nv12(src, 1920, 1080, 1920 * 4, nv, 1920, 1088, KVM_YUV_AUTO);
					if (f == 4) { mfh264_force_idr(h, &e); }
					size_t ol = 0;
					int idr = 0;
					const int r2 = mfh264_encode(h, nv, nvLen, (int64_t)f * 333333, ob, obCap, &ol, &idr, &e);
					if (r2 < 0) { print_err("encode", r2, e); failed = 1; break; }
					char nals[64];
					nal_list(ob, ol, nals, sizeof(nals));
					size_t left = 0;
					const uint8_t *sps = find_nal(ob, ol, 7, &left);
					out("  frame %d: %s %7u bytes, idr=%d, NAL types [%s]", f, "access unit", (unsigned)ol, idr, nals);
					if (sps != NULL && left >= 4) { out(", SPS profile_idc=%d constraint=0x%02X level_idc=%d", sps[1], sps[2], sps[3]); }
					out(", first bytes %02X %02X %02X %02X %02X\n", ol > 0 ? ob[0] : 0, ol > 1 ? ob[1] : 0, ol > 2 ? ob[2] : 0, ol > 3 ? ob[3] : 0, ol > 4 ? ob[4] : 0);
					out("{\"kind\":\"sanity_frame\",\"frame\":%d,\"bytes\":%u,\"idr\":%d,\"nals\":\"%s\",\"profile_idc\":%d,\"level_idc\":%d}\n", f, (unsigned)ol, idr, nals, sps != NULL && left >= 4 ? sps[1] : -1, sps != NULL && left >= 4 ? sps[3] : -1);
				}
				mfh264_stats st;
				mfh264_get_stats(h, &st);
				out("  1 in 1 out: frames in %llu, access units %llu, no output %llu, extra output %llu, broken %llu, stream changes %llu, split outputs %llu, AUD removed %llu, headers added from the media type %llu, forced IDR calls %llu, IDR %llu (unforced %llu)\n",
					(unsigned long long)st.frames_in, (unsigned long long)st.frames_out, (unsigned long long)st.no_output, (unsigned long long)st.extra_output, (unsigned long long)st.broken, (unsigned long long)st.stream_changes, (unsigned long long)st.split_outputs, (unsigned long long)st.aud_removed, (unsigned long long)st.headers_added, (unsigned long long)st.force_calls, (unsigned long long)st.idr, (unsigned long long)st.idr_unforced);
				out("{\"kind\":\"sanity_stats\",\"frames_in\":%llu,\"frames_out\":%llu,\"no_output\":%llu,\"extra_output\":%llu,\"broken\":%llu,\"stream_changes\":%llu,\"aud_removed\":%llu,\"headers_added\":%llu,\"idr\":%llu,\"idr_unforced\":%llu}\n",
					(unsigned long long)st.frames_in, (unsigned long long)st.frames_out, (unsigned long long)st.no_output, (unsigned long long)st.extra_output, (unsigned long long)st.broken, (unsigned long long)st.stream_changes, (unsigned long long)st.aud_removed, (unsigned long long)st.headers_added, (unsigned long long)st.idr, (unsigned long long)st.idr_unforced);
				free(nv); free(ob);
				mfh264_close(h);
			}
			gen_destroy(&g);
		}
	}
	QpMode qm;
	if (!failed) { qp_discovery(profile, 0, &qm, 0); }
	else { out("\nqpcheck skipped: the encoder could not be opened above\n"); }
	mfh264_shutdown();
	return failed ? 1 : 0;
}


// ---------------------------------------------------------------------------------------------------------------------- qpsample

// on Windows 11 build 26200 no form of setting the QP through the codec API changed the size of the frames. The agent has two other ways, tried here one by one on
// 1280x736 pictures, 14 frames in each of two phases (the second starts after a change of the parameter in the middle of the stream, the first 3 frames after it are not measured):
//   codec     the QP through the codec API (what the agent does today) QP 20 -> 40
//   sample+   the QP through the codec API AND in the input sample (MFSampleExtension_VideoEncodeQP, mfh264_set_sample_qp mode 1), QP 20 -> 40
//   sample    the QP in the input sample ALONE (the codec API stays at QP 30; mode 2), QP 20 -> 40
//   cbr / ldvbr / peak / vbr   the rate control by bit rate (AVEncCommonMeanBitRate set during the stream, 600 kb/s -> 4000 kb/s)
// "effective" = the mean P frame of the first phase is more than 1.4 x that of the second (QP 20 vs 40) / the second more than 1.4 x the first (bit rate). Besides, the QP that the encoder writes into its
// output sample (MFSampleExtension_VideoEncodeQP, the raw UINT64) is printed for the last frame of each phase: it says whether the setting reached the encoder even when the sizes do not change.
struct QsWay { const char *name; int rc, method, sampleMode, byBitrate; };

static int qs_run(Gen *g, const QsWay &w, int profile, double *meanA, double *meanB, unsigned long long *rawA, unsigned long long *rawB, int *havA, int *havB, long *openHr)
{
	const int cw = 1280, ch = 736, perPhase = 14;
	mfh264_cfg cfg = MFH264_CFG_INIT(cw, ch);
	cfg.profile = profile; cfg.rc = w.rc; cfg.qp_method = w.method; cfg.worker_threads = g_threads;
	cfg.bitrate = 600000;
	mfh264_err e;
	memset(&e, 0, sizeof(e));
	*meanA = *meanB = 0; *rawA = *rawB = 0; *havA = *havB = 0;
	mfh264 *h = mfh264_open(&cfg, &e);
	if (h == NULL) { *openHr = e.hr; return e.code; }
	const size_t nvLen = kvm_yuv_nv12_size(cw, ch), obCap = mfh264_max_au_size(h);
	uint8_t *nv = (uint8_t *)malloc(nvLen), *ob = (uint8_t *)malloc(obCap);
	int result = 0;
	if (nv == NULL || ob == NULL) { free(nv); free(ob); mfh264_close(h); return MFH264_E_ALLOC; }
	for (int phase = 0; phase < 2 && result == 0; phase++)
	{
		int rc0 = MFH264_OK;
		if (w.byBitrate) { rc0 = mfh264_set_bitrate(h, phase == 0 ? 600000 : 4000000, &e); }
		else
		{
			const int qp = phase == 0 ? 20 : 40;
			if (w.sampleMode == 2) { if (phase == 0) { rc0 = mfh264_set_qp(h, 30, &e); } if (rc0 == MFH264_OK) { rc0 = mfh264_set_sample_qp(h, 2, qp); } }
			else
			{
				rc0 = mfh264_set_qp(h, qp, &e);
				if (rc0 == MFH264_OK && w.sampleMode == 1 && phase == 0) { rc0 = mfh264_set_sample_qp(h, 1, 0); }
			}
		}
		if (rc0 != MFH264_OK) { *openHr = e.hr; result = rc0; break; }
		Series s;
		for (int f = 0; f < perPhase; f++)
		{
			const int t = phase * perPhase + f;
			const uint8_t *src = gen_frame(g, t);
			kvm_yuv_bgra_to_nv12(src, g->w, g->h, g->w * 4, nv, cw, ch, KVM_YUV_AUTO);
			size_t ol = 0;
			int idr = 0;
			const int r2 = mfh264_encode(h, nv, nvLen, (int64_t)t * 10000000LL / MFH264_NOMINAL_FPS, ob, obCap, &ol, &idr, &e);
			if (r2 == MFH264_E_STREAM_BROKEN) { continue; }
			if (r2 < 0) { *openHr = e.hr; result = r2; break; }
			if (!idr && f >= 3) { s.add((double)ol); }
		}
		uint64_t raw = 0;
		const int have = mfh264_last_out_qp(h, &raw);
		if (phase == 0) { *meanA = s.mean(); *rawA = raw; *havA = have; } else { *meanB = s.mean(); *rawB = raw; *havB = have; }
	}
	free(nv); free(ob);
	mfh264_close(h);
	return result;
}

static int run_qpsample(int profile)
{
	static const QsWay ways[] = {
		{ "codec API (today)", MFH264_RC_QUALITY, MFH264_QP_ENCODEQP_UI8, 0, 0 },
		{ "codec API + sample attribute", MFH264_RC_QUALITY, MFH264_QP_ENCODEQP_UI8, 1, 0 },
		{ "sample attribute alone", MFH264_RC_QUALITY, MFH264_QP_ENCODEQP_UI8, 2, 0 },
		{ "CBR, bit rate set during the stream", MFH264_RC_CBR, MFH264_QP_NONE, 0, 1 },
		{ "LowDelayVBR, bit rate set during the stream", MFH264_RC_LDVBR, MFH264_QP_NONE, 0, 1 },
		{ "PeakConstrainedVBR, bit rate set during the stream", MFH264_RC_PEAKVBR, MFH264_QP_NONE, 0, 1 },
		{ "UnconstrainedVBR, bit rate set during the stream", MFH264_RC_VBR, MFH264_QP_NONE, 0, 1 } };
	static const char *const jn[] = { "codec", "sample_plus_codec", "sample_alone", "cbr", "ldvbr", "peak_vbr", "vbr" };
	struct mfh264_err e;
	memset(&e, 0, sizeof(e));
	Gen g;
	int firstEffective = -1;
	if (!gen_init(&g, 1280, 720)) { out("qpsample: cannot create the test pictures\n"); return 1; }
	out("\nqpsample: P frame bytes in the two phases (QP 20 -> 40, or 600 -> 4000 kb/s), 1280x736, 14 frames a phase, the first 3 of each not counted; the raw MFSampleExtension_VideoEncodeQP of the last output sample\n  %-52s %12s %12s  %-22s %-22s %s\n", "way", "phase 1 B", "phase 2 B", "reported QP phase 1", "reported QP phase 2", "effective");
	for (int i = 0; i < (int)(sizeof(ways) / sizeof(ways[0])); i++)
	{
		double a = 0, b = 0;
		unsigned long long ra = 0, rb = 0;
		int ha = 0, hb = 0, eff = 0;
		long hr = 0;
		const int r = qs_run(&g, ways[i], profile, &a, &b, &ra, &rb, &ha, &hb, &hr);
		if (r == 0 && a > 0 && b > 0) { eff = ways[i].byBitrate ? (b > 1.4 * a) : (a > 1.4 * b); }
		if (r != 0) { out("  %-52s refused: %s (hr 0x%08lX)\n", ways[i].name, mfh264_result_name(r), (unsigned long)hr); }
		else
		{
			char sa[40], sb[40];
			if (ha) { _snprintf(sa, sizeof(sa), "0x%llX", ra); } else { _snprintf(sa, sizeof(sa), "(absent)"); }
			if (hb) { _snprintf(sb, sizeof(sb), "0x%llX", rb); } else { _snprintf(sb, sizeof(sb), "(absent)"); }
			out("  %-52s %12.0f %12.0f  %-22s %-22s %s\n", ways[i].name, a, b, sa, sb, eff ? "YES" : "no");
		}
		out("{\"kind\":\"qpsample\",\"way\":\"%s\",\"result\":\"%s\",\"bytes_phase1\":%.0f,\"bytes_phase2\":%.0f,\"reported_phase1\":%s,\"reported_phase2\":%s,\"reported_raw1\":\"0x%llX\",\"reported_raw2\":\"0x%llX\",\"effective\":%d}\n", jn[i], mfh264_result_name(r), a, b, ha ? "true" : "false", hb ? "true" : "false", ra, rb, eff);
		if (eff && firstEffective < 0) { firstEffective = i; }
	}
	gen_destroy(&g);
	if (firstEffective >= 0) { out("qpsample: the first way that changes the size of the frames: \"%s\"\n", ways[firstEffective].name); }
	else { out("qpsample: NO way changed the size of the frames\n"); }
	out("{\"kind\":\"qpsample_result\",\"first_effective\":\"%s\"}\n", firstEffective >= 0 ? jn[firstEffective] : "");
	return 0;
}

static int mode_qpsample(int profile)
{
	struct mfh264_err e;
	memset(&e, 0, sizeof(e));
	const int rc = mfh264_init(&e);
	if (rc != MFH264_OK) { print_err("init", rc, e); return 2; }
	const int r = run_qpsample(profile);
	mfh264_shutdown();
	return r;
}

// ---------------------------------------------------------------------------------------------------------------------- bench

struct BenchOpts { int maxSeconds; int frames; int only; int rc; int qpm; int qvs; int profile; int threads; };

static void report_run(const char *label, int w, int h, const char *what, int qpOrKbps, const EncRun &r, const char *rcName, int qpForm)
{
	const double wall = r.wallMs / 1000.0;
	const int oneInOne = (r.noOutput == 0 && r.extraOutput == 0 && r.brokenCalls == 0 && r.failed == 0);
	const double fpsMax = (r.conv.mean() + r.enc.mean()) > 0 ? 1000.0 / (r.conv.mean() + r.enc.mean()) : 0;
	out("  %-5s %4dx%-4d %s=%-5d %4d fr | convert %5.2f/%5.2f/%5.2f | encode %6.2f/%6.2f/%6.2f | P bytes %7.0f/%7.0f/%7.0f | IDR %7zu / %7.0f | 1in1out %s | IDR %d (unforced %d), forced %d/%d | %5.1f fps max | cpu %.2f cores | %.1f s\n",
		label, w, h, what, qpOrKbps, r.frames, r.conv.mean(), r.conv.pct(0.95), r.conv.max(), r.enc.mean(), r.enc.pct(0.95), r.enc.max(), r.bytesP.mean(), r.bytesP.pct(0.95), r.bytesP.max(),
		r.idrFirst, r.idrForced.mean(), oneInOne ? "yes" : "NO", r.idrTotal, r.idrUnforced, r.forcedOk, r.forcedAsked, fpsMax, r.wallMs > 0 ? r.cpuMs / r.wallMs : 0.0, wall);
	out("{\"kind\":\"bench\",\"test\":\"%s\",\"w\":%d,\"h\":%d,\"%s\":%d,\"rc\":\"%s\",\"qp_form\":%d,\"frames\":%d,\"gen_ms_mean\":%.2f,\"conv_ms_mean\":%.2f,\"conv_ms_p95\":%.2f,\"conv_ms_max\":%.2f,\"enc_ms_mean\":%.2f,\"enc_ms_p95\":%.2f,\"enc_ms_max\":%.2f,"
		"\"p_bytes_mean\":%.0f,\"p_bytes_p95\":%.0f,\"p_bytes_max\":%.0f,\"idr_first_bytes\":%zu,\"idr_forced_bytes_mean\":%.0f,\"one_in_one_out\":%d,\"no_output\":%d,\"extra_output\":%d,\"stream_changes\":%d,\"forced_idr_asked\":%d,\"forced_idr_ok\":%d,\"wall_s\":%.2f,\"cpu_cores\":%.2f,\"broken\":%d,\"idr_total\":%d,\"idr_unforced\":%d,\"aud_removed\":%d,\"headers_added\":%d,\"sps_profile_idc\":%d,\"sps_level_idc\":%d,\"level_requested\":%d,\"failed\":%d}\n",
		label, w, h, what, qpOrKbps, rcName, qpForm, r.frames, r.gen.mean(), r.conv.mean(), r.conv.pct(0.95), r.conv.max(), r.enc.mean(), r.enc.pct(0.95), r.enc.max(), r.bytesP.mean(), r.bytesP.pct(0.95), r.bytesP.max(),
		r.idrFirst, r.idrForced.mean(), oneInOne, r.noOutput, r.extraOutput, r.streamChanges, r.forcedAsked, r.forcedOk, wall, r.wallMs > 0 ? r.cpuMs / r.wallMs : 0.0, r.brokenCalls, r.idrTotal, r.idrUnforced, r.audRemoved, r.headersAdded, r.spsProfile, r.spsLevel, r.levelRequested, r.failed);
}

// Phases of a stream with a change of the parameter in the middle. kind 0: QP, kind 1: mean bit rate (CBR).
static void run_dynamic(Gen *g, int cw, int ch, int kind, const int *vals, int nVals, int phaseFrames, const BenchOpts &o, const QpMode &qm)
{
	mfh264_cfg cfg = MFH264_CFG_INIT(cw, ch);
	cfg.profile = o.profile; cfg.quality_vs_speed = o.qvs; cfg.worker_threads = o.threads;
	if (kind == 0) { cfg.rc = qm.rc; cfg.qp_method = qm.method; } else { cfg.rc = MFH264_RC_CBR; cfg.qp_method = MFH264_QP_NONE; cfg.bitrate = vals[0] * 1000; }
	mfh264_err e;
	memset(&e, 0, sizeof(e));
	mfh264 *h = mfh264_open(&cfg, &e);
	if (h == NULL) { print_err("open (dynamic)", e.code, e); return; }
	const size_t nvLen = kvm_yuv_nv12_size(cw, ch);
	const size_t obCap = mfh264_max_au_size(h);
	uint8_t *nv = (uint8_t *)malloc(nvLen), *ob = (uint8_t *)malloc(obCap);
	std::vector<Series> phase((size_t)nVals);
	std::vector<std::vector<int> > firstFrames((size_t)nVals);
	int change_fail = 0;
	for (int p = 0; p < nVals; p++)
	{
		const int rc0 = kind == 0 ? mfh264_set_qp(h, vals[p], &e) : mfh264_set_bitrate(h, vals[p] * 1000, &e);
		if (rc0 != MFH264_OK) { print_err(kind == 0 ? "set_qp (dynamic)" : "set_bitrate (dynamic)", rc0, e); change_fail = 1; break; }
		for (int f = 0; f < phaseFrames; f++)
		{
			const int t = p * phaseFrames + f;
			const uint8_t *src = gen_frame(g, t);
			kvm_yuv_bgra_to_nv12(src, g->w, g->h, g->w * 4, nv, cw, ch, KVM_YUV_AUTO);
			size_t ol = 0;
			int idr = 0;
			const int r2 = mfh264_encode(h, nv, nvLen, (int64_t)t * 10000000LL / MFH264_NOMINAL_FPS, ob, obCap, &ol, &idr, &e);
			if (r2 < 0) { print_err("encode (dynamic)", r2, e); if (r2 != MFH264_E_STREAM_BROKEN) { change_fail = 1; break; } continue; }
			if (!idr && r2 == 0) { phase[(size_t)p].add((double)ol); if (f < 4) { firstFrames[(size_t)p].push_back((int)ol); } }
		}
		if (change_fail) { break; }
		out("  %s %4dx%-4d phase %d: %s=%d  P frames: mean %7.0f B  (first four after the change:", kind == 0 ? "dynQP" : "dynBR", cw, ch, p, kind == 0 ? "QP" : "kbps", vals[p], phase[(size_t)p].mean());
		for (size_t k = 0; k < firstFrames[(size_t)p].size(); k++) { out(" %d", firstFrames[(size_t)p][k]); }
		out(")%s\n", kind == 1 ? "" : "");
		out("{\"kind\":\"bench_dynamic\",\"param\":\"%s\",\"w\":%d,\"h\":%d,\"phase\":%d,\"value\":%d,\"p_bytes_mean\":%.0f,\"implied_kbps\":%.0f}\n", kind == 0 ? "qp" : "kbps", cw, ch, p, vals[p], phase[(size_t)p].mean(), phase[(size_t)p].mean() * 8 * 30 / 1000.0);
	}
	free(nv); free(ob);
	mfh264_close(h);
}

static int mode_bench(int argc, char **argv, const BenchOpts &oIn)
{
	BenchOpts o = oIn;
	mfh264_err e;
	memset(&e, 0, sizeof(e));
	int rc = mfh264_init(&e);
	if (rc != MFH264_OK) { print_err("init", rc, e); return 2; }
	(void)argc; (void)argv;
	{
		mfh264_err pe;
		memset(&pe, 0, sizeof(pe));
		const int r0 = mfh264_probe_size(1280, 720, o.profile, &pe);
		if (r0 == MFH264_E_OS || r0 == MFH264_E_NOMF || r0 == MFH264_E_CREATE) { print_err("no H.264 encoder (run probe for details)", r0, pe); mfh264_shutdown(); return 2; }
	}
	int failures = 0;
	QpMode qm;
	memset(&qm, 0, sizeof(qm));
	out("bench: budget %d s, profile %d, QualityVsSpeed %d\n", o.maxSeconds, o.profile, o.qvs);
	if (o.rc >= 0 && o.qpm > 0) { qm.rc = o.rc; qm.method = o.qpm; qm.valid = 1; out("bench: rate control \"%s\", QP form %d given on the command line\n", rc_name(qm.rc), qm.method); }
	else { qp_discovery(o.profile, o.qvs, &qm, 0); }

	struct Res { int w, h, cw, ch, frames; };
	double perFrame1080 = 0;
	static const int qps[3] = { 24, 30, 36 };
	static const int cbrKbps[3] = { 6000, 3000, 1500 };
	for (int ri = 0; ri < 3; ri++)
	{
		Res res;
		if (ri == 0) { res.w = 1920; res.h = 1080; } else if (ri == 1) { res.w = 2560; res.h = 1440; } else { res.w = 3840; res.h = 2160; }
		if (o.only != 0 && o.only != res.h) { continue; }
		res.cw = kvm_yuv_coded(res.w); res.ch = kvm_yuv_coded(res.h);
		res.frames = o.frames > 0 ? o.frames : 300;
		if (ri == 1 && perFrame1080 > 20) { res.frames = std::min(res.frames, 150); }
		if (ri == 2) { res.frames = std::min(res.frames, 90); }
		if (ri >= 1 && perFrame1080 > 40) { out("\nSKIPPED %dx%d: 1080p costs %.1f ms per frame (> 40 ms)\n", res.w, res.h, perFrame1080); out("{\"kind\":\"skipped\",\"w\":%d,\"h\":%d,\"reason\":\"1080p too slow\"}\n", res.w, res.h); continue; }
		mfh264_err pe;
		memset(&pe, 0, sizeof(pe));
		if (mfh264_probe_size(res.cw, res.ch, o.profile, &pe) != MFH264_OK)
		{
			out("\nSKIPPED %dx%d: the encoder does not accept %dx%d (%s hr=0x%08lX)\n", res.w, res.h, res.cw, res.ch, pe.stage != NULL ? pe.stage : "", (unsigned long)pe.hr);
			out("{\"kind\":\"skipped\",\"w\":%d,\"h\":%d,\"reason\":\"size not accepted\",\"hr\":\"0x%08lX\"}\n", res.w, res.h, (unsigned long)pe.hr);
			continue;
		}
		Gen g;
		if (!gen_init(&g, res.w, res.h)) { out("cannot create the %dx%d test pictures\n", res.w, res.h); failures++; continue; }
		out("\n%dx%d (coded %dx%d), %s: convert / encode in ms (mean/p95/max), P frame bytes (mean/p95/max)\n", res.w, res.h, res.cw, res.ch, qm.valid ? "constant QP" : "constant bit rate (no QP form worked)");
		for (int qi = 0; qi < 3; qi++)
		{
			const double est = res.frames * (perFrame1080 > 0 ? perFrame1080 * (double)res.cw * res.ch / (1920.0 * 1088.0) : 30.0) / 1000.0;
			if (elapsed_s() + est > o.maxSeconds)
			{
				out("  SKIPPED %dx%d %s=%d: %.0f s used of %d, the run needs about %.0f s\n", res.w, res.h, qm.valid ? "QP" : "kbps", qm.valid ? qps[qi] : cbrKbps[qi], elapsed_s(), o.maxSeconds, est);
				out("{\"kind\":\"skipped\",\"w\":%d,\"h\":%d,\"reason\":\"time budget\"}\n", res.w, res.h);
				continue;
			}
			mfh264_cfg cfg = MFH264_CFG_INIT(res.cw, res.ch);
			cfg.profile = o.profile; cfg.quality_vs_speed = o.qvs;
			int forceAt[2] = { res.frames / 3, res.frames * 2 / 3 };
			if (qm.valid) { cfg.rc = qm.rc; cfg.qp_method = qm.method; }
			else { cfg.rc = MFH264_RC_CBR; cfg.qp_method = MFH264_QP_NONE; cfg.bitrate = cbrKbps[qi] * 1000; }
			EncRun r;
			run_encode(&g, res.cw, res.ch, res.frames, cfg, qm.valid ? qps[qi] : -1, NULL, forceAt, 2, &r, NULL, NULL, NULL, 0, NULL);
			report_run("bench", res.w, res.h, qm.valid ? "qp" : "kbps", qm.valid ? qps[qi] : cbrKbps[qi], r, rc_name(cfg.rc), qm.valid ? qm.method : 0);
			if (r.failed) { failures++; }
			if (ri == 0 && r.frames > 0) { const double c = r.conv.mean() + r.enc.mean(); if (c > perFrame1080) { perFrame1080 = c; } }
		}
		if (ri == 0 && elapsed_s() + 20 < o.maxSeconds)
		{
			// the parameters change in the middle of a stream, 1080p
			const int qv[3] = { 24, 36, 28 };
			const int bv[3] = { 4000, 1000, 4000 };
			out("\nchange of the parameter in the middle of a stream (1080p, 40 frames per phase)\n");
			if (qm.valid) { run_dynamic(&g, res.cw, res.ch, 0, qv, 3, 40, o, qm); }
			run_dynamic(&g, res.cw, res.ch, 1, bv, 3, 40, o, qm);
		}
		gen_destroy(&g);
	}
	out("\nbench finished after %.1f s, %d failed measurements\n", elapsed_s(), failures);
	out("{\"kind\":\"bench_end\",\"seconds\":%.1f,\"failures\":%d}\n", elapsed_s(), failures);
	mfh264_shutdown();
	return failures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------------------------------- dump

static int mode_dump(int argc, char **argv, const BenchOpts &o)
{
	int w = 1920, h = 1080, frames = 150, qp = 28;
	const char *outName = "out.h264", *prefix = "";
	for (int i = 2; i + 1 < argc; i++)
	{
		if (strcmp(argv[i], "--w") == 0) { w = atoi(argv[i + 1]); }
		else if (strcmp(argv[i], "--h") == 0) { h = atoi(argv[i + 1]); }
		else if (strcmp(argv[i], "--frames") == 0) { frames = atoi(argv[i + 1]); }
		else if (strcmp(argv[i], "--qp") == 0) { qp = atoi(argv[i + 1]); }
		else if (strcmp(argv[i], "--out") == 0) { outName = argv[i + 1]; }
		else if (strcmp(argv[i], "--prefix") == 0) { prefix = argv[i + 1]; }
	}
	if (w < 64 || h < 64 || w > 7680 || h > 4320 || frames < 2 || frames > 3000) { out("dump: bad arguments\n"); return 2; }
	mfh264_err e;
	memset(&e, 0, sizeof(e));
	int rc = mfh264_init(&e);
	if (rc != MFH264_OK) { print_err("init", rc, e); return 2; }
	QpMode qm;
	memset(&qm, 0, sizeof(qm));
	{
		mfh264_err pe;
		memset(&pe, 0, sizeof(pe));
		const int r0 = mfh264_probe_size(1280, 720, o.profile, &pe);
		if (r0 == MFH264_E_OS || r0 == MFH264_E_NOMF || r0 == MFH264_E_CREATE) { print_err("no H.264 encoder (run probe for details)", r0, pe); mfh264_shutdown(); return 2; }
	}
	if (o.rc >= 0 && o.qpm > 0) { qm.rc = o.rc; qm.method = o.qpm; qm.valid = 1; }
	else { qp_discovery(o.profile, o.qvs, &qm, 1); }
	Gen g;
	if (!gen_init(&g, w, h)) { out("dump: cannot create the test pictures\n"); mfh264_shutdown(); return 1; }
	const int cw = kvm_yuv_coded(w), ch = kvm_yuv_coded(h);
	FILE *f = fopen(outName, "wb");
	char txtName[300];
	_snprintf(txtName, sizeof(txtName), "%sout.txt", prefix);
	FILE *tf = fopen(txtName, "w");
	if (f == NULL || tf == NULL) { out("dump: cannot create %s / %s\n", outName, txtName); gen_destroy(&g); mfh264_shutdown(); return 1; }
	fprintf(tf, "# frame bytes idr   source %dx%d coded %dx%d\n", w, h, cw, ch);
	mfh264_cfg cfg = MFH264_CFG_INIT(cw, ch);
	cfg.profile = o.profile; cfg.quality_vs_speed = o.qvs;
	if (qm.valid) { cfg.rc = qm.rc; cfg.qp_method = qm.method; } else { cfg.rc = MFH264_RC_CBR; cfg.qp_method = MFH264_QP_NONE; cfg.bitrate = 3000000; }
	// QP: qp for the first third, qp + 8 for the second (a change in the middle of the stream), qp again for the last; a forced IDR in the middle
	std::vector<int> qpf((size_t)frames, qm.valid ? qp : -1);
	if (qm.valid) { for (int i = frames / 3; i < frames * 2 / 3; i++) { qpf[(size_t)i] = std::min(51, qp + 8); } }
	const int forceAt[1] = { frames / 2 };
	const int bmpAt[9] = { 0, 1, frames / 4, frames / 3, frames / 2 - 1, frames / 2, frames / 2 + 1, frames * 2 / 3, frames - 1 };
	EncRun r;
	const int res = run_encode(&g, cw, ch, frames, cfg, -1, qpf.data(), forceAt, 1, &r, f, tf, bmpAt, 9, prefix);
	fclose(f);
	fclose(tf);
	// saved.txt: the frames whose source was written; out.json: everything the analysis in Docker needs
	{
		char name[300];
		_snprintf(name, sizeof(name), "%ssaved.txt", prefix);
		FILE *sf = fopen(name, "w");
		if (sf != NULL)
		{
			int last = -1;
			for (int k = 0; k < 9; k++) { if (bmpAt[k] != last && bmpAt[k] < frames) { fprintf(sf, "%d src_%04d.bmp\n", bmpAt[k], bmpAt[k]); last = bmpAt[k]; } }
			fclose(sf);
		}
		_snprintf(name, sizeof(name), "%sout.json", prefix);
		FILE *jf = fopen(name, "w");
		if (jf != NULL)
		{
			fprintf(jf, "{\n \"tool\":\"kvm_h264_check dump\",\n \"stream\":\"%s\",\n \"source\":{\"w\":%d,\"h\":%d},\n \"coded\":{\"w\":%d,\"h\":%d},\n \"rate_control\":\"%s\",\n \"qp_form\":%d,\n \"qp_base\":%d,\n \"profile_requested\":%d,\n", outName, w, h, cw, ch, rc_name(cfg.rc), cfg.qp_method, qm.valid ? qp : -1, o.profile);
			fprintf(jf, " \"sps_profile_idc\":%d,\n \"sps_level_idc\":%d,\n \"level_requested\":%d,\n \"level_set_in_type\":%d,\n", r.spsProfile, r.spsLevel, r.levelRequested, r.levelSetOk);
			fprintf(jf, " \"stats\":{\"idr_total\":%d,\"idr_unforced\":%d,\"forced_asked\":%d,\"forced_ok\":%d,\"broken\":%d,\"aud_removed\":%d,\"headers_added\":%d},\n \"saved\":[", r.idrTotal, r.idrUnforced, r.forcedAsked, r.forcedOk, r.brokenCalls, r.audRemoved, r.headersAdded);
			{
				int last = -1, first = 1;
				for (int k = 0; k < 9; k++) { if (bmpAt[k] != last && bmpAt[k] < frames) { fprintf(jf, "%s{\"frame\":%d,\"file\":\"src_%04d.bmp\"}", first ? "" : ",", bmpAt[k], bmpAt[k]); first = 0; last = bmpAt[k]; } }
			}
			fprintf(jf, "],\n \"frames\":[\n");
			int au = 0;
			for (size_t i = 0; i < r.sizes.size(); i++)
			{
				const int lost = (r.nals[i] == "lost");
				fprintf(jf, "  {\"i\":%d,\"au\":%d,\"bytes\":%d,\"idr\":%d,\"forced\":%d,\"qp\":%d,\"nals\":\"%s\"}%s\n", (int)i, lost ? -1 : au, r.sizes[i], r.idr[i], r.forcedAt[i], r.qps[i], r.nals[i].c_str(), i + 1 < r.sizes.size() ? "," : "");
				if (!lost) { au++; }
			}
			fprintf(jf, " ]\n}\n");
			fclose(jf);
		}
	}
	out("dump: %d access units written to %s (%s), IDR %d (unforced %d), forced IDR %d of %d, result %d\n", r.frames, outName, qm.valid ? "constant QP" : "CBR 3 Mb/s", r.idrTotal, r.idrUnforced, r.forcedOk, r.forcedAsked, res);
	out("{\"kind\":\"dump\",\"file\":\"%s\",\"w\":%d,\"h\":%d,\"coded_w\":%d,\"coded_h\":%d,\"frames\":%d,\"qp\":%d,\"access_units\":%d,\"idr\":%d,\"idr_unforced\":%d,\"forced_idr_ok\":%d,\"sps_profile_idc\":%d,\"sps_level_idc\":%d,\"result\":%d}\n",
		outName, w, h, cw, ch, frames, qm.valid ? qp : -1, r.frames, r.idrTotal, r.idrUnforced, r.forcedOk, r.spsProfile, r.spsLevel, res);
	gen_destroy(&g);
	mfh264_shutdown();
	return res == 0 ? 0 : 1;
}

// No Media Foundation: only the picture generator and the BGRA to NV12 conversion (their cost on this machine). --bmp writes the pictures.
static int mode_gentest(int argc, char **argv)
{
	int bmp = 0;
	for (int i = 2; i < argc; i++) { if (strcmp(argv[i], "--bmp") == 0) { bmp = 1; } }
	static const int sizes[3][2] = { { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
	for (int i = 0; i < 3; i++)
	{
		const int w = sizes[i][0], h = sizes[i][1], cw = kvm_yuv_coded(w), ch = kvm_yuv_coded(h);
		Gen g;
		if (!gen_init(&g, w, h)) { out("gentest: cannot create the %dx%d pictures\n", w, h); return 1; }
		uint8_t *nv = (uint8_t *)malloc(kvm_yuv_nv12_size(cw, ch));
		Series tg, tc, ts;
		uint32_t sum = 0;
		for (int f = 0; f < 30; f++)
		{
			double t0 = now_ms();
			const uint8_t *src = gen_frame(&g, f);
			tg.add(now_ms() - t0);
			t0 = now_ms();
			kvm_yuv_bgra_to_nv12(src, w, h, w * 4, nv, cw, ch, KVM_YUV_AUTO);
			tc.add(now_ms() - t0);
			t0 = now_ms();
			kvm_yuv_bgra_to_nv12(src, w, h, w * 4, nv, cw, ch, KVM_YUV_SCALAR);
			ts.add(now_ms() - t0);
			if (f == 10)
			{
				for (size_t k = 0; k < kvm_yuv_nv12_size(cw, ch); k += 997) { sum = sum * 31 + nv[k]; }
				if (bmp) { char name[64]; _snprintf(name, sizeof(name), "gentest_%dx%d.bmp", w, h); write_bmp(name, src, w, h); }
			}
		}
		out("gentest %4dx%-4d: picture %6.2f ms, BGRA->NV12 %s %5.2f ms (scalar %5.2f ms), checksum of frame 10: %08x\n", w, h, tg.mean(), kvm_yuv_have_sse2() ? "sse2" : "scalar", tc.mean(), ts.mean(), (unsigned)sum);
		out("{\"kind\":\"gentest\",\"w\":%d,\"h\":%d,\"gen_ms\":%.2f,\"conv_ms\":%.2f,\"conv_scalar_ms\":%.2f,\"sse2\":%d,\"checksum\":\"%08x\"}\n", w, h, tg.mean(), tc.mean(), ts.mean(), kvm_yuv_have_sse2(), (unsigned)sum);
		free(nv);
		gen_destroy(&g);
	}
	return 0;
}

int main(int argc, char **argv)
{
	g_t0 = now_ms();
	const char *mode = (argc > 1) ? argv[1] : "";
	if (strcmp(mode, "--hw") == 0)	// the hardware encoder (kvm_hwprobe_win.cpp), its own options
	{
#ifdef KVM_HWPROBE
		return hwprobe_main(argc, argv);
#else
		out("{\"error\":\"this build has no --hw: compile with /DKVM_HWPROBE and the hardware probe files (see the build line at the top of kvm_h264_check.cpp)\"}\n");
		return 2;
#endif
	}
	BenchOpts o;
	o.maxSeconds = 100; o.frames = 0; o.only = 0; o.rc = -1; o.qpm = 0; o.qvs = 0; o.profile = MFH264_PROFILE_HIGH; o.threads = 0;
	for (int i = 2; i + 1 < argc; i++)
	{
		if (strcmp(argv[i], "--max-seconds") == 0) { o.maxSeconds = atoi(argv[i + 1]); }
		else if (strcmp(argv[i], "--frames") == 0 && strcmp(mode, "bench") == 0) { o.frames = atoi(argv[i + 1]); }
		else if (strcmp(argv[i], "--only") == 0) { o.only = atoi(argv[i + 1]); }
		else if (strcmp(argv[i], "--qpm") == 0) { o.qpm = atoi(argv[i + 1]); }
		else if (strcmp(argv[i], "--qvs") == 0) { o.qvs = atoi(argv[i + 1]); }
		else if (strcmp(argv[i], "--profile") == 0) { o.profile = atoi(argv[i + 1]); }
		else if (strcmp(argv[i], "--threads") == 0) { o.threads = atoi(argv[i + 1]); g_threads = o.threads; }
		else if (strcmp(argv[i], "--rc") == 0)
		{
			const char *v = argv[i + 1];
			o.rc = strcmp(v, "quality") == 0 ? MFH264_RC_QUALITY : strcmp(v, "vbr") == 0 ? MFH264_RC_VBR : strcmp(v, "cbr") == 0 ? MFH264_RC_CBR : MFH264_RC_DEFAULT;
		}
	}
	if (o.profile != 66 && o.profile != 77 && o.profile != 100) { o.profile = MFH264_PROFILE_HIGH; }
	if (o.maxSeconds < 10) { o.maxSeconds = 10; }
	if (o.frames < 0) { o.frames = 0; }
	if (strcmp(mode, "qpsample") == 0) { return mode_qpsample(o.profile); }
	if (strcmp(mode, "probe") == 0)
	{
		const int pr = mode_probe(argc, argv, o.profile);
		for (int i = 2; i < argc; i++) { if (strcmp(argv[i], "--qpsample") == 0 && pr != 2) { return mode_qpsample(o.profile) != 0 ? 1 : pr; } }
		return pr;
	}
	if (strcmp(mode, "bench") == 0) { return mode_bench(argc, argv, o); }
	if (strcmp(mode, "dump") == 0) { return mode_dump(argc, argv, o); }
	if (strcmp(mode, "gentest") == 0) { return mode_gentest(argc, argv); }
	out("{\"error\":\"usage: kvm_h264_check qpsample | probe [--size WxH] [--qpsample] | bench [--max-seconds N] [--frames N] [--only 1080|1440|2160] [--rc quality|vbr|cbr|default --qpm 1..4] [--qvs N] [--profile 66|77|100] | dump [--w W --h H --frames N --qp Q --out file --prefix p]\"}\n");
	return 2;
}
