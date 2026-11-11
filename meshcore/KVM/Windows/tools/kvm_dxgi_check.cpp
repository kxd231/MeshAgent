// kvm-dxgi-check: stand-alone check of the DXGI capture module (built together with dxgi_capture.cpp, not part of the agent).
//
// Run it as the logged-on user (not as a service): the duplication and the test windows need the user's desktop.
// One line of JSON goes to stdout, the exit code is 0 when the check passed. Modes:
//   info                    adapters, outputs, monitors, result of dxgi_capture_init() per monitor
//   compare [--monitor n]   DXGI image against a GDI BitBlt of the same monitor (own topmost test window)
//   rects <seconds> [--bbox n]  (n = threshold of the bounding-box copy, a test hook for machines whose DWM reports few rectangles)
//                           scrolling text, a moving window and bursts of 100 small squares: dirty/move rectangle statistics,
//                           the image kept up to date from the rectangles is compared with GDI at the end.
//                           "verified" needs pass and o2_tested (move rectangles seen) and bbox_tested (a frame above KVM_DXGI_BBOX_RECTS rectangles)
//   cursor [--ibeam]        kvm_cursor_xor() against the cursor drawing code of the GDI path; --ibeam puts the (monochrome) text cursor
//                           under the mouse pointer first: it MOVES THE MOUSE POINTER, only for a test machine
//   pointer [seconds] [--move]  what DXGI says about the pointer (LastMouseUpdateTime, PointerPosition.Visible, shape size) next to GetCursorInfo,
//                           and the differences DXGI against GDI inside the cursor field (a software cursor that the system composes into the
//                           desktop image shows there). --move puts the pointer on three places with SetCursorPos (only for a test machine);
//                           without it nothing is touched. Also compares BitBlt with and without CAPTUREBLT in the field.
//   idle <seconds>          duplication open, nothing changes: CPU time of this process
//   stalecost [n]           cost of the check of the DXGI image against GDI that the agent makes every 10 s when nothing changes
// The test windows are transparent to the mouse, never take the focus and are closed by the tool itself.

#include "../dxgi_capture.h"
#include "../dxgi_ptrmask.h"

#ifndef KVM_DXGI_ENABLED
#error kvm_dxgi_check needs /D_LINKVM /DKVM_DXGI on x64
#endif

#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

// ---------------------------------------------------------------------------------------------------------------- helpers

static void json_str(const char *s)
{
	putchar('"');
	for (; *s != 0; s++)
	{
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') { putchar('\\'); putchar(c); }
		else if (c < 0x20) { printf("\\u%04x", c); }
		else { putchar(c); }
	}
	putchar('"');
}

static void json_wstr(const wchar_t *w)
{
	char buf[512];
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)sizeof(buf) - 1, NULL, NULL);
	if (n <= 0) { buf[0] = 0; } else { buf[n] = 0; }
	json_str(buf);
}

static void set_dpi_aware(void)
{
	typedef HRESULT(WINAPI * SetAwareness)(int);
	HMODULE sh = LoadLibraryW(L"Shcore.dll");
	if (sh != NULL)
	{
		SetAwareness f = (SetAwareness)GetProcAddress(sh, "SetProcessDpiAwareness");
		if (f != NULL && SUCCEEDED(f(2))) { return; } // PROCESS_PER_MONITOR_DPI_AWARE, like the agent
	}
	SetProcessDPIAware();
}

struct mon_t { RECT rc; char name[32]; int primary; };
static mon_t g_mon[16];
static int g_nmon = 0;

static BOOL CALLBACK enum_monitors(HMONITOR hm, HDC, LPRECT, LPARAM)
{
	MONITORINFOEXW mi;
	memset(&mi, 0, sizeof(mi));
	mi.cbSize = sizeof(mi);
	if (g_nmon < 16 && GetMonitorInfoW(hm, (MONITORINFO *)&mi))
	{
		g_mon[g_nmon].rc = mi.rcMonitor;
		g_mon[g_nmon].primary = (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0;
		memset(g_mon[g_nmon].name, 0, sizeof(g_mon[g_nmon].name));
		WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, g_mon[g_nmon].name, (int)sizeof(g_mon[g_nmon].name) - 1, NULL, NULL);
		g_nmon++;
	}
	return TRUE;
}

static void list_monitors(void)
{
	g_nmon = 0;
	EnumDisplayMonitors(NULL, NULL, enum_monitors, 0);
}

// Picks monitor n (1 based); 0 = the primary one. Returns the index in g_mon or -1.
static int pick_monitor(int n)
{
	list_monitors();
	if (n > 0) { return (n <= g_nmon) ? n - 1 : -1; }
	for (int i = 0; i < g_nmon; i++) { if (g_mon[i].primary) { return i; } }
	return g_nmon > 0 ? 0 : -1;
}

static unsigned long long now_ms(void) { return GetTickCount64(); }

static void pump(void)
{
	MSG msg;
	while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

static void pump_for(unsigned int ms)
{
	unsigned long long end = now_ms() + ms;
	while (now_ms() < end) { pump(); Sleep(5); }
}

// 32 bpp bottom-up DIB section: memory row 0 is the bottom row, like the agent's buffers.
struct dib_t { HDC dc; HBITMAP bmp; HGDIOBJ old; unsigned char *bits; int w, h; };

static int dib_create(dib_t *d, int w, int h)
{
	BITMAPINFO bi;
	HDC scr = GetDC(NULL);
	void *p = NULL;
	memset(d, 0, sizeof(*d));
	memset(&bi, 0, sizeof(bi));
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = h;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	d->dc = CreateCompatibleDC(scr);
	d->bmp = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &p, NULL, 0);
	ReleaseDC(NULL, scr);
	if (d->dc == NULL || d->bmp == NULL || p == NULL) { return 0; }
	d->old = SelectObject(d->dc, d->bmp);
	d->bits = (unsigned char *)p;
	d->w = w;
	d->h = h;
	return 1;
}

static void dib_free(dib_t *d)
{
	if (d->dc != NULL && d->old != NULL) { SelectObject(d->dc, d->old); }
	if (d->bmp != NULL) { DeleteObject(d->bmp); }
	if (d->dc != NULL) { DeleteDC(d->dc); }
	memset(d, 0, sizeof(*d));
}

static unsigned char *dib_top(dib_t *d) { return d->bits + ((size_t)(d->h - 1) * d->w * 4); }

// The same BitBlt as capture_full_gdi() in tile.cpp.
static int gdi_grab_rop(const RECT &rc, dib_t *d, DWORD rop)
{
	HDC scr = GetDC(NULL);
	BOOL ok = BitBlt(d->dc, 0, 0, d->w, d->h, scr, rc.left, rc.top, rop);
	GdiFlush();
	ReleaseDC(NULL, scr);
	return ok ? 1 : 0;
}

static int gdi_grab(const RECT &rc, dib_t *d) { return gdi_grab_rop(rc, d, SRCCOPY); }

struct cmp_t
{
	long long mismatch, mismatchBox, maxDelta, a3gdiZero, a3gdiNonZero, a3dxZero, a3dxNonZero;
	long long hist[4];					// differing pixels by the largest channel delta: 1, 2-8, 9-64, over 64
	int bx0, by0, bx1, by1;				// rectangle that contains all differences (bx0 > bx1: none)
	int nfirst, fx[10], fy[10];
	unsigned int fgdi[10], fdx[10];
};

// Compares bytes 0..2 of every pixel. Differences within 'box' pixels of (cx, cy) are counted separately (software cursor).
static void compare_dibs(const dib_t *g, const dib_t *d, int cx, int cy, int box, cmp_t *r)
{
	memset(r, 0, sizeof(*r));
	r->bx0 = g->w; r->by0 = g->h; r->bx1 = -1; r->by1 = -1;
	for (int m = 0; m < g->h; m++)
	{
		int y = g->h - 1 - m;
		const unsigned char *pg = g->bits + (size_t)m * g->w * 4;
		const unsigned char *pd = d->bits + (size_t)m * g->w * 4;
		for (int x = 0; x < g->w; x++, pg += 4, pd += 4)
		{
			if (pg[3] == 0) { r->a3gdiZero++; } else { r->a3gdiNonZero++; }
			if (pd[3] == 0) { r->a3dxZero++; } else { r->a3dxNonZero++; }
			if (pg[0] != pd[0] || pg[1] != pd[1] || pg[2] != pd[2])
			{
				int inBox = (x >= cx - box && x <= cx + box && y >= cy - box && y <= cy + box);
				if (inBox) { r->mismatchBox++; continue; }
				if (r->nfirst < 10)
				{
					r->fx[r->nfirst] = x; r->fy[r->nfirst] = y;
					r->fgdi[r->nfirst] = (unsigned int)pg[0] | ((unsigned int)pg[1] << 8) | ((unsigned int)pg[2] << 16);
					r->fdx[r->nfirst] = (unsigned int)pd[0] | ((unsigned int)pd[1] << 8) | ((unsigned int)pd[2] << 16);
					r->nfirst++;
				}
				r->mismatch++;
				if (x < r->bx0) { r->bx0 = x; } if (x > r->bx1) { r->bx1 = x; }
				if (y < r->by0) { r->by0 = y; } if (y > r->by1) { r->by1 = y; }
				long long pm = 0;
				for (int c = 0; c < 3; c++)
				{
					long long dl = (long long)pg[c] - pd[c];
					if (dl < 0) { dl = -dl; }
					if (dl > r->maxDelta) { r->maxDelta = dl; }
					if (dl > pm) { pm = dl; }
				}
				r->hist[pm <= 1 ? 0 : (pm <= 8 ? 1 : (pm <= 64 ? 2 : 3))]++;
			}
		}
	}
}

static void print_cmp(const cmp_t *r)
{
	printf("\"mismatch\":%lld,\"mismatch_cursor_box\":%lld,\"max_delta\":%lld,", r->mismatch, r->mismatchBox, r->maxDelta);
	printf("\"alpha_gdi\":{\"zero\":%lld,\"nonzero\":%lld},\"alpha_dxgi\":{\"zero\":%lld,\"nonzero\":%lld},", r->a3gdiZero, r->a3gdiNonZero, r->a3dxZero, r->a3dxNonZero);
	printf("\"diff_rect\":[%d,%d,%d,%d],\"delta_hist\":{\"d1\":%lld,\"d2_8\":%lld,\"d9_64\":%lld,\"d65_255\":%lld},", r->bx1 < 0 ? 0 : r->bx0, r->bx1 < 0 ? 0 : r->by0, r->bx1 < 0 ? 0 : r->bx1, r->bx1 < 0 ? 0 : r->by1, r->hist[0], r->hist[1], r->hist[2], r->hist[3]);
	printf("\"first\":[");
	for (int i = 0; i < r->nfirst; i++)
	{
		printf("%s{\"x\":%d,\"y\":%d,\"gdi\":\"%06x\",\"dxgi\":\"%06x\"}", i ? "," : "", r->fx[i], r->fy[i], r->fgdi[i], r->fdx[i]);
	}
	printf("]");
}

// ---------------------------------------------------------------------------------------------------------------- test window

#define TW_LINE_H 20
#define TW_TEXT_X 50
#define TW_TEXT_Y 50
#define TW_TEXT_W 800
#define TW_TEXT_H 600

static dib_t g_pattern;       // background of the window
static int g_scrollPos = 0;   // line shown in the first text row
static int g_boxX = 0;        // x of the moving box (client coordinates)
static int g_animate = 0;     // paint the text area and the box (rects mode)
#define TW_SQUARES 100
#define TW_SQ_SIZE 8
#define TW_SQ_GRID 12
static int g_sqX[TW_SQUARES], g_sqY[TW_SQUARES], g_sqOn = 0;
static RECT g_sqArea = { 0, 0, 0, 0 }; // client coordinates of the area for the squares

static void fill_pattern(dib_t *d)
{
	for (int y = 0; y < d->h; y++)
	{
		unsigned char *row = d->bits + (size_t)(d->h - 1 - y) * d->w * 4;
		for (int x = 0; x < d->w; x++)
		{
			unsigned char r = (unsigned char)(x * 255 / d->w), g = (unsigned char)(y * 255 / d->h), b = (unsigned char)((x + y) & 255);
			if ((x % 64) == 0 || (y % 64) == 0) { r = g = b = 255; }                  // 1 px lines
			else if (x < 256 && y < 256 && ((x / 8 + y / 8) & 1)) { r = g = b = 0; }  // checkerboard
			else if (x < 256 && y < 256) { r = g = b = 255; }
			row[x * 4 + 0] = b; row[x * 4 + 1] = g; row[x * 4 + 2] = r; row[x * 4 + 3] = 0;
		}
	}
}

static HFONT make_font(void)
{
	return CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

static void draw_text_line(HDC dc, int row, int line)
{
	RECT rc = { TW_TEXT_X, TW_TEXT_Y + row * TW_LINE_H, TW_TEXT_X + TW_TEXT_W, TW_TEXT_Y + (row + 1) * TW_LINE_H };
	wchar_t text[96];
	FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, RGB((line * 37) & 0xC0, (line * 71) & 0xC0, (line * 113) & 0xC0));
	swprintf(text, 96, L"Line %d: the quick brown fox jumps over the lazy dog 0123456789", line);
	TextOutW(dc, rc.left + 4, rc.top + 1, text, (int)wcslen(text));
}

static void draw_squares(HDC dc, int on)
{
	for (int i = 0; i < TW_SQUARES; i++)
	{
		RECT r = { g_sqX[i], g_sqY[i], g_sqX[i] + TW_SQ_SIZE, g_sqY[i] + TW_SQ_SIZE };
		HBRUSH b = CreateSolidBrush(on ? RGB(0, 90 + (i * 7) % 150, 200) : RGB(255, 255, 255));
		FillRect(dc, &r, b);
		DeleteObject(b);
	}
}

// New random positions on a grid (so no two squares touch): the previous ones are erased and the new ones drawn back to back,
// which makes DWM report a long list of small rectangles for one frame.
static void move_squares(HDC dc, unsigned int *seed)
{
	int cols = (g_sqArea.right - g_sqArea.left) / TW_SQ_GRID, rows = (g_sqArea.bottom - g_sqArea.top) / TW_SQ_GRID;
	if (cols < 1 || rows < 1) { return; }
	if (g_sqOn) { draw_squares(dc, 0); }
	for (int i = 0; i < TW_SQUARES; i++)
	{
		*seed = *seed * 1664525u + 1013904223u;
		g_sqX[i] = g_sqArea.left + (int)((*seed >> 8) % (unsigned int)cols) * TW_SQ_GRID;
		*seed = *seed * 1664525u + 1013904223u;
		g_sqY[i] = g_sqArea.top + (int)((*seed >> 8) % (unsigned int)rows) * TW_SQ_GRID;
	}
	draw_squares(dc, 1);
	g_sqOn = 1;
}

static void draw_box(HDC dc, int x, int on)
{
	RECT rc = { x, TW_TEXT_Y + TW_TEXT_H + 20, x + 40, TW_TEXT_Y + TW_TEXT_H + 60 };
	HBRUSH b = CreateSolidBrush(on ? RGB(200, 0, 0) : RGB(255, 255, 255));
	FillRect(dc, &rc, b);
	DeleteObject(b);
}

static LRESULT CALLBACK tw_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
		case WM_ERASEBKGND: return 1;
		case WM_NCHITTEST: return HTTRANSPARENT;
		case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
		case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC dc = BeginPaint(hwnd, &ps);
			HFONT font = make_font();
			HGDIOBJ oldFont = SelectObject(dc, font);
			BITMAPINFO bi;
			memset(&bi, 0, sizeof(bi));
			bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bi.bmiHeader.biWidth = g_pattern.w;
			bi.bmiHeader.biHeight = g_pattern.h;
			bi.bmiHeader.biPlanes = 1;
			bi.bmiHeader.biBitCount = 32;
			bi.bmiHeader.biCompression = BI_RGB;
			SetDIBitsToDevice(dc, 0, 0, g_pattern.w, g_pattern.h, 0, 0, 0, g_pattern.h, g_pattern.bits, &bi, DIB_RGB_COLORS);
			if (g_animate)
			{
				for (int row = 0; row < TW_TEXT_H / TW_LINE_H; row++) { draw_text_line(dc, row, g_scrollPos + row); }
				draw_box(dc, g_boxX, 1);
				if (g_sqOn) { draw_squares(dc, 1); }
			}
			else
			{
				// Colored ClearType text on the gradient
				for (int i = 0; i < 6; i++)
				{
					wchar_t text[64];
					swprintf(text, 64, L"ClearType sample %d: Hamburgefonstiv 0123456789", i);
					SetBkMode(dc, TRANSPARENT);
					SetTextColor(dc, i == 0 ? RGB(0, 0, 0) : (i == 1 ? RGB(255, 255, 255) : RGB(i * 40, 255 - i * 40, 128)));
					TextOutW(dc, 300, 300 + i * 24, text, (int)wcslen(text));
				}
			}
			SelectObject(dc, oldFont);
			DeleteObject(font);
			EndPaint(hwnd, &ps);
			return 0;
		}
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND create_test_window(const RECT &rc)
{
	WNDCLASSW wc;
	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = tw_proc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = L"KvmDxgiCheckWindow";
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	RegisterClassW(&wc);
	if (!dib_create(&g_pattern, rc.right - rc.left, rc.bottom - rc.top)) { return NULL; }
	fill_pattern(&g_pattern);
	HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, wc.lpszClassName, L"kvm-dxgi-check", WS_POPUP,
		rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
	if (h == NULL) { return NULL; }
	ShowWindow(h, SW_SHOWNOACTIVATE);
	SetWindowPos(h, HWND_TOPMOST, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
	UpdateWindow(h);
	return h;
}

static void destroy_test_window(HWND h)
{
	if (h != NULL) { DestroyWindow(h); }
	pump();
	dib_free(&g_pattern);
}

// A small window that is moved over the screen (DWM reports a window move as a move rectangle).
static LRESULT CALLBACK mover_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
		case WM_ERASEBKGND: return 1;
		case WM_NCHITTEST: return HTTRANSPARENT;
		case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
		case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC dc = BeginPaint(hwnd, &ps);
			RECT c;
			GetClientRect(hwnd, &c);
			for (int i = 0; i < 8; i++)
			{
				RECT r = { c.left, c.top + i * ((c.bottom - c.top) / 8), c.right, c.top + (i + 1) * ((c.bottom - c.top) / 8) };
				HBRUSH b = CreateSolidBrush(RGB(30 * i, 255 - 30 * i, 90 + 10 * i));
				FillRect(dc, &r, b);
				DeleteObject(b);
			}
			SetBkMode(dc, TRANSPARENT);
			TextOutW(dc, 10, 10, L"moving window", 13);
			EndPaint(hwnd, &ps);
			return 0;
		}
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND create_mover(int x, int y, int w, int h)
{
	WNDCLASSW wc;
	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = mover_proc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = L"KvmDxgiCheckMover";
	RegisterClassW(&wc);
	HWND hw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, wc.lpszClassName, L"kvm-dxgi-check mover", WS_POPUP, x, y, w, h, NULL, NULL, wc.hInstance, NULL);
	if (hw != NULL) { ShowWindow(hw, SW_SHOWNOACTIVATE); UpdateWindow(hw); }
	return hw;
}

// ---------------------------------------------------------------------------------------------------------------- info

static void print_init(const char *key, int x, int y, int w, int h)
{
	dxgi_info di;
	dxgi_result r = dxgi_capture_init(x, y, w, h, &di);
	printf("\"%s\":{\"result\":\"%s\",\"hr\":\"0x%08x\"", key, dxgi_result_name(r), (unsigned int)dxgi_capture_last_hr());
	printf(",\"adapter\":"); json_wstr(di.adapter);
	printf(",\"output\":"); json_wstr(di.output);
	printf(",\"vendor\":\"0x%04x\",\"device\":\"0x%04x\",\"mode\":\"%dx%d\",\"rotation\":%d,\"sysmem\":%d}", di.vendorId, di.deviceId, di.width, di.height, di.rotation, di.systemMemory);
	dxgi_capture_shutdown();
}

static int mode_info(void)
{
	typedef HRESULT(WINAPI * CreateFactory1)(REFIID, void **);
	HMODULE dll = LoadLibraryW(L"dxgi.dll");
	CreateFactory1 cf = (dll != NULL) ? (CreateFactory1)GetProcAddress(dll, "CreateDXGIFactory1") : NULL;
	IDXGIFactory1 *factory = NULL;

	list_monitors();
	printf("{\"mode\":\"info\",\"virtual_screen\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},\"monitors_count\":%d,\"remote_session\":%d,",
		GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
		GetSystemMetrics(SM_CMONITORS), GetSystemMetrics(SM_REMOTESESSION));
	printf("\"dxgi_dll\":%d,\"adapters\":[", dll != NULL);
	if (cf != NULL && SUCCEEDED(cf(__uuidof(IDXGIFactory1), (void **)&factory)))
	{
		for (UINT ai = 0; ; ai++)
		{
			IDXGIAdapter1 *a = NULL;
			DXGI_ADAPTER_DESC1 ad;
			if (FAILED(factory->EnumAdapters1(ai, &a))) { break; }
			memset(&ad, 0, sizeof(ad));
			a->GetDesc1(&ad);
			printf("%s{\"name\":", ai ? "," : ""); json_wstr(ad.Description);
			printf(",\"vendor\":\"0x%04x\",\"device\":\"0x%04x\",\"software\":%d,\"outputs\":[", ad.VendorId, ad.DeviceId, (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? 1 : 0);
			for (UINT oi = 0; ; oi++)
			{
				IDXGIOutput *o = NULL;
				DXGI_OUTPUT_DESC od;
				IDXGIOutput6 *o6 = NULL;
				if (FAILED(a->EnumOutputs(oi, &o))) { break; }
				memset(&od, 0, sizeof(od));
				o->GetDesc(&od);
				printf("%s{\"name\":", oi ? "," : ""); json_wstr(od.DeviceName);
				printf(",\"coords\":[%ld,%ld,%ld,%ld],\"attached\":%d,\"rotation\":%d", od.DesktopCoordinates.left, od.DesktopCoordinates.top, od.DesktopCoordinates.right, od.DesktopCoordinates.bottom, od.AttachedToDesktop ? 1 : 0, (int)od.Rotation);
				if (SUCCEEDED(o->QueryInterface(__uuidof(IDXGIOutput6), (void **)&o6)))
				{
					DXGI_OUTPUT_DESC1 d1;
					memset(&d1, 0, sizeof(d1));
					if (SUCCEEDED(o6->GetDesc1(&d1))) { printf(",\"color_space\":%d,\"bits_per_color\":%u", (int)d1.ColorSpace, d1.BitsPerColor); }
					o6->Release();
				}
				else
				{
					printf(",\"color_space\":null");
				}
				printf("}");
				o->Release();
			}
			printf("]}");
			a->Release();
		}
		factory->Release();
	}
	printf("],\"monitors\":[");
	for (int i = 0; i < g_nmon; i++)
	{
		printf("%s{\"name\":", i ? "," : ""); json_str(g_mon[i].name);
		printf(",\"rect\":[%ld,%ld,%ld,%ld],\"primary\":%d,", g_mon[i].rc.left, g_mon[i].rc.top, g_mon[i].rc.right, g_mon[i].rc.bottom, g_mon[i].primary);
		print_init("init", g_mon[i].rc.left, g_mon[i].rc.top, g_mon[i].rc.right - g_mon[i].rc.left, g_mon[i].rc.bottom - g_mon[i].rc.top);
		printf("}");
	}
	printf("],");
	// The "all screens" rectangle of the agent
	print_init("init_virtual_screen", GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
	printf("}\n");
	return 0;
}

// ---------------------------------------------------------------------------------------------------------------- compare

// Waits for an image; repaints the test window now and then to make DWM present a frame.
static dxgi_result acquire_image(HWND hwnd, dib_t *d, unsigned int totalMs, dxgi_frame *f)
{
	unsigned long long end = now_ms() + totalMs;
	dxgi_result r = DXGI_CAP_TIMEOUT;
	while (now_ms() < end)
	{
		pump();
		r = dxgi_capture_acquire(100, dib_top(d), -(ptrdiff_t)d->w * 4, 1, f);
		if (r == DXGI_CAP_OK) { return r; }
		if (r != DXGI_CAP_TIMEOUT && r != DXGI_CAP_NO_CHANGE) { return r; }
		if (hwnd != NULL) { InvalidateRect(hwnd, NULL, FALSE); }
	}
	return r;
}

static int mode_compare(int monitor)
{
	int idx = pick_monitor(monitor);
	if (idx < 0) { printf("{\"mode\":\"compare\",\"error\":\"no such monitor\"}\n"); return 2; }
	RECT rc = g_mon[idx].rc;
	int w = rc.right - rc.left, h = rc.bottom - rc.top;
	dib_t gdi, dx, gdi2;
	cmp_t cmp, cmp2;
	dxgi_frame f;
	dxgi_info di;
	POINT pt = { 0, 0 };
	HWND hwnd;
	HRESULT initHr, acqHr = S_OK;
	int ok = 0, extraImages = 0, haveGdi2 = 0;

	memset(&cmp, 0, sizeof(cmp));
	memset(&cmp2, 0, sizeof(cmp2));
	memset(&gdi2, 0, sizeof(gdi2));
	memset(&f, 0, sizeof(f));
	g_animate = 0;
	hwnd = create_test_window(rc);
	if (hwnd == NULL || !dib_create(&gdi, w, h) || !dib_create(&dx, w, h)) { printf("{\"mode\":\"compare\",\"error\":\"window or bitmap\"}\n"); return 2; }
	pump_for(600);

	dxgi_result ri = dxgi_capture_init(rc.left, rc.top, w, h, &di);
	initHr = dxgi_capture_last_hr();
	dxgi_result ra = DXGI_CAP_ERROR;
	if (ri == DXGI_CAP_OK)
	{
		ra = acquire_image(hwnd, &dx, 4000, &f);
		acqHr = dxgi_capture_last_hr();
		// The first image can be taken before the window has been painted completely (background first, text later): keep taking the
		// changes for 300 ms (the module applies them to the same image), then compare. A second BitBlt 200 ms later shows content that still changes.
		if (ra == DXGI_CAP_OK)
		{
			unsigned long long until = now_ms() + 300;
			dxgi_frame f2;
			while (now_ms() < until)
			{
				pump();
				memset(&f2, 0, sizeof(f2));
				if (dxgi_capture_acquire(50, dib_top(&dx), -(ptrdiff_t)dx.w * 4, 0, &f2) == DXGI_CAP_OK) { extraImages++; }
			}
		}
		pump_for(100);
		gdi_grab(rc, &gdi);
		GetCursorPos(&pt);
		compare_dibs(&gdi, &dx, pt.x - rc.left, pt.y - rc.top, 32, &cmp);
		if (dib_create(&gdi2, w, h))
		{
			pump_for(200);
			gdi_grab(rc, &gdi2);
			compare_dibs(&gdi, &gdi2, pt.x - rc.left, pt.y - rc.top, 32, &cmp2);
			haveGdi2 = 1;
		}
		ok = (ra == DXGI_CAP_OK && cmp.mismatch == 0);
	}
	printf("{\"mode\":\"compare\",\"monitor\":");
	json_str(g_mon[idx].name);
	printf(",\"size\":\"%dx%d\",\"init\":\"%s\",\"init_hr\":\"0x%08x\",\"acquire\":\"%s\",\"acquire_hr\":\"0x%08x\",\"full_copy\":%d,\"copy_us\":%u,\"sysmem\":%d,\"cursor\":{\"x\":%ld,\"y\":%ld},",
		w, h, dxgi_result_name(ri), (unsigned int)initHr, dxgi_result_name(ra), (unsigned int)acqHr, f.fullCopy, f.copyUs, di.systemMemory, pt.x - rc.left, pt.y - rc.top);
	if (ri == DXGI_CAP_OK && ra == DXGI_CAP_OK) { print_cmp(&cmp); } else { printf("\"mismatch\":-1"); }
	printf(",\"extra_images_300ms\":%d,\"gdi_second_blit_diff\":%lld,\"gdi_second_blit_rect\":[%d,%d,%d,%d]", extraImages, haveGdi2 ? cmp2.mismatch : -1, cmp2.bx1 < 0 ? 0 : cmp2.bx0, cmp2.bx1 < 0 ? 0 : cmp2.by0, cmp2.bx1 < 0 ? 0 : cmp2.bx1, cmp2.bx1 < 0 ? 0 : cmp2.by1);
	printf(",\"pass\":%d}\n", ok);
	dxgi_capture_shutdown();
	dib_free(&gdi2);
	dib_free(&gdi);
	dib_free(&dx);
	destroy_test_window(hwnd);
	return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------------------------------------------- rects

static int mode_rects(int seconds, int bboxRects)
{
	int idx = pick_monitor(0);
	if (idx < 0) { printf("{\"mode\":\"rects\",\"error\":\"no monitor\"}\n"); return 2; }
	RECT rc = g_mon[idx].rc;
	int w = rc.right - rc.left, h = rc.bottom - rc.top;
	dib_t gdi, dx;
	cmp_t cmp;
	dxgi_frame f;
	POINT pt = { 0, 0 };
	long long okFrames = 0, timeouts = 0, noChange = 0, dirtyRects = 0, dirtyArea = 0, moveRects = 0, moveArea = 0, moveFrames = 0, moveMaxArea = 0, copyUsSum = 0, partial = 0;
	long long bboxImages = 0, maxRects = 0;
	unsigned int copyUsMax = 0, seed = 12345;
	int errors = 0, haveImage = 0, reinit = 0;
	char lastErr[24] = "";
	HWND hwnd, mover;
	int moverW = 240, moverH = 160, moverX0 = w / 2, moverStep = 0;

	if (seconds < 1) { seconds = 1; }
	if (bboxRects > 0) { dxgi_capture_set_bbox_rects(bboxRects); }
	g_animate = 1;
	g_scrollPos = 0;
	g_boxX = TW_TEXT_X;
	g_sqOn = 0;
	g_sqArea.left = TW_TEXT_X; g_sqArea.right = TW_TEXT_X + TW_TEXT_W;
	g_sqArea.top = TW_TEXT_Y + TW_TEXT_H + 80; g_sqArea.bottom = h - 20;
	memset(&f, 0, sizeof(f));
	hwnd = create_test_window(rc);
	if (hwnd == NULL || !dib_create(&gdi, w, h) || !dib_create(&dx, w, h)) { printf("{\"mode\":\"rects\",\"error\":\"window or bitmap\"}\n"); return 2; }
	mover = create_mover(rc.left + moverX0, rc.top + 100, moverW, moverH);
	pump_for(600);

	dxgi_result ri = dxgi_capture_init(rc.left, rc.top, w, h, NULL);
	if (ri != DXGI_CAP_OK)
	{
		printf("{\"mode\":\"rects\",\"init\":\"%s\",\"init_hr\":\"0x%08x\",\"pass\":0}\n", dxgi_result_name(ri), (unsigned int)dxgi_capture_last_hr());
		if (mover != NULL) { DestroyWindow(mover); }
		dib_free(&gdi); dib_free(&dx); destroy_test_window(hwnd);
		return 1;
	}

	unsigned long long start = now_ms(), end = start + (unsigned long long)seconds * 1000, nextStep = start, nextSquares = start + 500, lastKick = start, stopAt = 0;
	int animating = 1;
	while (1)
	{
		unsigned long long t = now_ms();
		pump();
		if (animating && t >= end) { animating = 0; stopAt = t; }
		if (!animating && t > stopAt + 500) { break; } // let the last frames arrive
		if (animating && t >= nextStep)
		{
			// Scroll the text area up by one line and draw the new last line; move the box and the small window
			HDC dc = GetDC(hwnd);
			RECT sr = { TW_TEXT_X, TW_TEXT_Y, TW_TEXT_X + TW_TEXT_W, TW_TEXT_Y + TW_TEXT_H };
			HFONT font = make_font();
			HGDIOBJ oldFont = SelectObject(dc, font);
			ScrollDC(dc, 0, -TW_LINE_H, &sr, &sr, NULL, NULL);
			g_scrollPos++;
			draw_text_line(dc, TW_TEXT_H / TW_LINE_H - 1, g_scrollPos + TW_TEXT_H / TW_LINE_H - 1);
			draw_box(dc, g_boxX, 0);
			g_boxX = TW_TEXT_X + ((g_boxX - TW_TEXT_X + 7) % 700);
			draw_box(dc, g_boxX, 1);
			if (t >= nextSquares) { move_squares(dc, &seed); nextSquares = t + 200; }
			SelectObject(dc, oldFont);
			DeleteObject(font);
			ReleaseDC(hwnd, dc);
			if (mover != NULL)
			{
				// 4 px per step, back and forth over 300 px
				moverStep++;
				int ph = (moverStep * 4) % 600, mx = ph < 300 ? ph : 600 - ph;
				SetWindowPos(mover, HWND_TOPMOST, rc.left + moverX0 + mx, rc.top + 100 + (mx / 3), 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
			}
			nextStep += 16;
			if (nextStep < t) { nextStep = t + 16; }
		}
		if (!haveImage && t - lastKick > 200) { InvalidateRect(hwnd, NULL, FALSE); lastKick = t; }

		dxgi_result r = dxgi_capture_acquire(4, dib_top(&dx), -(ptrdiff_t)w * 4, !haveImage, &f);
		if (r == DXGI_CAP_OK)
		{
			okFrames++;
			haveImage = 1;
			if (!f.fullCopy)
			{
				partial++;
				if (f.rectCount > maxRects) { maxRects = f.rectCount; }
			}
			dirtyRects += f.dirtyCount; dirtyArea += f.dirtyArea; moveRects += f.moveCount; moveArea += f.moveArea;
			if (f.moveCount > 0) { moveFrames++; if (f.moveArea > moveMaxArea) { moveMaxArea = f.moveArea; } }
			copyUsSum += f.copyUs;
			if (f.copyUs > copyUsMax) { copyUsMax = f.copyUs; }
		}
		else if (r == DXGI_CAP_TIMEOUT) { timeouts++; }
		else if (r == DXGI_CAP_NO_CHANGE) { noChange++; }
		else
		{
			// Lost / error: open again and start over with a full image
			errors++;
			lstrcpynA(lastErr, dxgi_result_name(r), (int)sizeof(lastErr));
			Sleep(250);
			if (dxgi_capture_init(rc.left, rc.top, w, h, NULL) != DXGI_CAP_OK) { if (++reinit > 3) { break; } }
			haveImage = 0;
		}
	}
	double secs = (double)(now_ms() - start) / 1000.0;
	double animSecs = (double)((stopAt ? stopAt : now_ms()) - start) / 1000.0;

	pump_for(150);
	gdi_grab(rc, &gdi);
	GetCursorPos(&pt);
	compare_dibs(&gdi, &dx, pt.x - rc.left, pt.y - rc.top, 32, &cmp);
	int ok = (haveImage && cmp.mismatch == 0 && errors == 0);
	long long cntSys = 0, cntStg = 0, cntBbox = 0;
	dxgi_capture_counters(&cntSys, &cntStg, &cntBbox);
	bboxImages = cntBbox; // images that really went through the bounding-box copy of the GPU path (never with the system memory surface)
	int o2 = moveRects > 0, bbox = bboxImages > 0;

	printf("{\"mode\":\"rects\",\"monitor\":"); json_str(g_mon[idx].name);
	printf(",\"seconds\":%.1f,\"images\":%lld,\"images_per_s\":%.1f,\"partial_images\":%lld,\"timeouts\":%lld,\"no_change\":%lld,\"errors\":%d,\"last_error\":", secs, okFrames, animSecs > 0 ? okFrames / animSecs : 0.0, partial, timeouts, noChange, errors);
	json_str(lastErr);
	printf(",\"dirty_rects\":%lld,\"dirty_area\":%lld,\"move_rects\":%lld,\"move_area\":%lld,\"move_frames\":%lld,\"move_max_area\":%lld,\"max_rects_per_image\":%lld,\"bbox_threshold\":%d,\"bbox_images\":%lld,\"copy_path\":\"%s\",\"images_sysmem\":%lld,\"images_staging\":%lld,", dirtyRects, dirtyArea, moveRects, moveArea, moveFrames, moveMaxArea, maxRects, dxgi_capture_bbox_rects(), bboxImages, cntStg > 0 && cntSys == 0 ? "staging" : (cntSys > 0 && cntStg == 0 ? "sysmem" : (cntSys + cntStg == 0 ? "none" : "mixed")), cntSys, cntStg);
	printf("\"copy_us_avg\":%lld,\"copy_us_max\":%u,", okFrames > 0 ? copyUsSum / okFrames : 0, copyUsMax);
	print_cmp(&cmp);
	printf(",\"pass\":%d,\"o2_tested\":%d,\"bbox_tested\":%d,\"verified\":%d}\n", ok, o2, bbox, (ok && o2 && bbox) ? 1 : 0);
	dxgi_capture_shutdown();
	if (mover != NULL) { DestroyWindow(mover); }
	dib_free(&gdi);
	dib_free(&dx);
	destroy_test_window(hwnd);
	return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------------------------------------------- cursor

// The cursor drawing of the GDI path (tile.cpp, get_desktop_buffer), against a screen compatible bitmap.
static void gdi_cursor_original(HDC hCaptureDC, int x, int y)
{
	CURSORINFO info;
	BITMAP bm;
	ICONINFO ii;
	memset(&info, 0, sizeof(info));
	info.cbSize = sizeof(info);
	memset(&ii, 0, sizeof(ii));
	GetCursorInfo(&info);
	GetIconInfo(info.hCursor, &ii);
	if (GetObject(ii.hbmMask, sizeof(bm), &bm) == sizeof(bm))
	{
		HDC hdcScreen = GetDC(NULL);
		HDC hdcMem = CreateCompatibleDC(hdcScreen);
		HBITMAP hbmCanvas = CreateCompatibleBitmap(hdcScreen, bm.bmWidth, ii.hbmColor ? bm.bmHeight : (bm.bmHeight / 2));
		if (hdcMem != NULL && hbmCanvas != NULL)
		{
			HGDIOBJ hbmold = SelectObject(hdcMem, hbmCanvas);
			DrawIconEx(hdcMem, 0, 0, info.hCursor, bm.bmWidth, ii.hbmColor ? bm.bmHeight : (bm.bmHeight / 2), 0, NULL, DI_NORMAL);
			BitBlt(hCaptureDC, x, y, bm.bmWidth, ii.hbmColor ? bm.bmHeight : (bm.bmHeight / 2), hdcMem, 0, 0, SRCINVERT);
			SelectObject(hdcMem, hbmold);
		}
		if (hbmCanvas != NULL) { DeleteObject(hbmCanvas); }
		if (hdcMem != NULL) { DeleteDC(hdcMem); }
		ReleaseDC(NULL, hdcScreen);
	}
	if (ii.hbmMask != NULL) { DeleteObject(ii.hbmMask); }
	if (ii.hbmColor != NULL) { DeleteObject(ii.hbmColor); }
}

// Window that asks for the text cursor (monochrome in the default scheme)
static LRESULT CALLBACK ibeam_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
		case WM_SETCURSOR: SetCursor(LoadCursor(NULL, IDC_IBEAM)); return TRUE;
		case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static int mode_cursor(int ibeam)
{
	const int W = 256, H = 256;
	CURSORINFO ci;
	ICONINFO cii;
	HWND ibw = NULL;
	POINT oldPos = { 0, 0 };
	int isIbeam = 0, mono = -1;
	if (ibeam)
	{
		int idx = pick_monitor(0);
		WNDCLASSW wc;
		memset(&wc, 0, sizeof(wc));
		wc.lpfnWndProc = ibeam_proc;
		wc.hInstance = GetModuleHandleW(NULL);
		wc.lpszClassName = L"KvmDxgiCheckIbeam";
		RegisterClassW(&wc);
		GetCursorPos(&oldPos);
		if (idx >= 0)
		{
			int cx = (g_mon[idx].rc.left + g_mon[idx].rc.right) / 2, cy = (g_mon[idx].rc.top + g_mon[idx].rc.bottom) / 2;
			ibw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"kvm-dxgi-check ibeam", WS_POPUP, cx - 150, cy - 100, 300, 200, NULL, NULL, wc.hInstance, NULL);
			if (ibw != NULL)
			{
				ShowWindow(ibw, SW_SHOWNOACTIVATE);
				SetCursorPos(cx, cy);
				pump_for(400);
				SetCursorPos(cx + 1, cy + 1); // a move makes the system ask the window for the cursor
				pump_for(400);
			}
		}
	}
	memset(&ci, 0, sizeof(ci));
	ci.cbSize = sizeof(ci);
	GetCursorInfo(&ci);
	memset(&cii, 0, sizeof(cii));
	if (ci.hCursor != NULL && GetIconInfo(ci.hCursor, &cii))
	{
		mono = (cii.hbmColor == NULL) ? 1 : 0;
		if (cii.hbmMask != NULL) { DeleteObject(cii.hbmMask); }
		if (cii.hbmColor != NULL) { DeleteObject(cii.hbmColor); }
	}
	isIbeam = (ci.hCursor != NULL && ci.hCursor == LoadCursor(NULL, IDC_IBEAM)) ? 1 : 0;
	if (ci.hCursor == NULL)
	{
		printf("{\"mode\":\"cursor\",\"skipped\":\"no cursor handle\",\"pass\":%d}\n", ibeam ? 0 : 1);
		if (ibw != NULL) { DestroyWindow(ibw); }
		return ibeam ? 1 : 0;
	}
	static const int pos[][2] = { { 10, 10 }, { 100, 60 }, { -5, -5 }, { 246, 246 }, { 0, 200 } };
	long long total = 0;
	printf("{\"mode\":\"cursor\",\"ibeam_requested\":%d,\"hcursor_is_ibeam\":%d,\"monochrome\":%d,\"positions\":[", ibeam, isIbeam, mono);
	for (int p = 0; p < (int)(sizeof(pos) / sizeof(pos[0])); p++)
	{
		HDC scr = GetDC(NULL);
		HDC dcOrig = CreateCompatibleDC(scr);
		HBITMAP ddb = CreateCompatibleBitmap(scr, W, H);
		dib_t ref, mine;
		BITMAPINFO bi;
		dxgi_rect cov;
		memset(&cov, 0, sizeof(cov));
		dib_create(&ref, W, H);
		dib_create(&mine, W, H);
		fill_pattern(&ref);
		memcpy(mine.bits, ref.bits, (size_t)W * H * 4);
		memset(&bi, 0, sizeof(bi));
		bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = H; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;

		// Original: pattern into the screen bitmap, draw the cursor, read it back
		HGDIOBJ old = SelectObject(dcOrig, ddb);
		SetDIBitsToDevice(dcOrig, 0, 0, W, H, 0, 0, 0, H, ref.bits, &bi, DIB_RGB_COLORS);
		gdi_cursor_original(dcOrig, pos[p][0], pos[p][1]);
		SelectObject(dcOrig, old);
		GetDIBits(scr, ddb, 0, H, ref.bits, &bi, DIB_RGB_COLORS);

		int drawn = kvm_cursor_xor(dib_top(&mine), -(ptrdiff_t)W * 4, W, H, pos[p][0], pos[p][1], &cov);
		long long diff = 0;
		for (size_t i = 0; i < (size_t)W * H; i++)
		{
			const unsigned char *a = ref.bits + i * 4, *b = mine.bits + i * 4;
			if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2]) { diff++; }
		}
		printf("%s{\"x\":%d,\"y\":%d,\"drawn\":%d,\"covered\":[%d,%d,%d,%d],\"diff_pixels\":%lld}", p ? "," : "", pos[p][0], pos[p][1], drawn, cov.left, cov.top, cov.right, cov.bottom, diff);
		total += diff;
		dib_free(&ref);
		dib_free(&mine);
		DeleteObject(ddb);
		DeleteDC(dcOrig);
		ReleaseDC(NULL, scr);
	}
	int pass = (total == 0 && (!ibeam || (isIbeam && mono == 1))) ? 1 : 0;
	printf("],\"diff_total\":%lld,\"pass\":%d}\n", total, pass);
	if (ibw != NULL) { DestroyWindow(ibw); pump(); SetCursorPos(oldPos.x, oldPos.y); }
	return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------------------------------------------- pointer

// Pixels (bytes 0..2) that differ between two bottom-up images inside r (image coordinates, clipped).
static long long rect_diff(const dib_t *a, const dib_t *b, int l, int t, int rr, int bb)
{
	long long n = 0;
	if (l < 0) { l = 0; } if (t < 0) { t = 0; } if (rr > a->w) { rr = a->w; } if (bb > a->h) { bb = a->h; }
	for (int y = t; y < bb; y++)
	{
		int m = a->h - 1 - y;
		const unsigned char *pa = a->bits + ((size_t)m * a->w + l) * 4, *pb = b->bits + ((size_t)m * a->w + l) * 4;
		for (int x = l; x < rr; x++, pa += 4, pb += 4) { if (pa[0] != pb[0] || pa[1] != pb[1] || pa[2] != pb[2]) { n++; } }
	}
	return n;
}

struct ptr_frame_t { unsigned int t; int res, rects, upd, vis, x, y; unsigned int shape; POINT cur; };

static int mode_pointer(int seconds, int move, int blt)
{
	int idx = pick_monitor(0);
	if (idx < 0) { printf("{\"mode\":\"pointer\",\"error\":\"no monitor\"}\n"); return 2; }
	RECT rc = g_mon[idx].rc;
	int w = rc.right - rc.left, h = rc.bottom - rc.top;
	dib_t dx, g1, g2;
	dxgi_frame f;
	dxgi_info di;
	static ptr_frame_t fr[400];
	int nfr = 0, nblt = 0, nimg = 0, nnc = 0, ntimeout = 0, updVisible = 0, updHidden = 0, updNone = 0;
	POINT oldPos = { 0, 0 };
	unsigned long long t0;

	memset(&f, 0, sizeof(f));
	if (!dib_create(&dx, w, h) || !dib_create(&g1, w, h) || !dib_create(&g2, w, h)) { printf("{\"mode\":\"pointer\",\"error\":\"bitmap\"}\n"); return 2; }
	dxgi_result ri = dxgi_capture_init(rc.left, rc.top, w, h, &di);
	if (ri != DXGI_CAP_OK) { printf("{\"mode\":\"pointer\",\"init\":\"%s\",\"init_hr\":\"0x%08x\",\"pass\":0}\n", dxgi_result_name(ri), (unsigned int)dxgi_capture_last_hr()); return 1; }
	GetCursorPos(&oldPos);
	t0 = now_ms();
	dxgi_result ra = acquire_image(NULL, &dx, 4000, &f);
	{
		POINT p; GetCursorPos(&p);
		fr[nfr].t = (unsigned int)(now_ms() - t0); fr[nfr].res = (int)ra; fr[nfr].rects = f.rectCount; fr[nfr].upd = f.ptrUpdate; fr[nfr].vis = f.ptrVisible;
		fr[nfr].x = f.ptrX; fr[nfr].y = f.ptrY; fr[nfr].shape = f.ptrShapeSize; fr[nfr].cur = p; nfr++;
		if (ra == DXGI_CAP_OK) { nimg++; }
		if (f.ptrUpdate) { if (f.ptrVisible) { updVisible++; } else { updHidden++; } } else { updNone++; }
	}
	int firstUpd = f.ptrUpdate, firstVis = f.ptrVisible;
	if (blt != 0)
	{
		dxgi_rect fl;
		if (kvm_cursor_field(KVM_DXGI_POINTER_MARGIN, &fl, NULL, NULL))
		{
			HDC scr = GetDC(NULL);
			BitBlt(g1.dc, 0, 0, fl.right - fl.left, fl.bottom - fl.top, scr, fl.left, fl.top, blt == 2 ? (SRCCOPY | CAPTUREBLT) : SRCCOPY);
			GdiFlush();
			ReleaseDC(NULL, scr);
			nblt++;
		}
	}

	// The pointer is put on three places (--move), then left alone; frames are collected for 'seconds'
	int cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2;
	static const int dxy[][2] = { { 0, 0 }, { 220, 90 }, { -180, -120 } };
	int step = -1;
	unsigned long long tstart = now_ms(), tend = tstart + (unsigned long long)seconds * 1000;
	while (now_ms() < tend && ra == DXGI_CAP_OK)
	{
		unsigned long long el = now_ms() - tstart;
		if (move && step < 2 && el >= (unsigned long long)(300 + (step + 1) * 700)) { step++; SetCursorPos(cx + dxy[step][0], cy + dxy[step][1]); }
		pump();
		memset(&f, 0, sizeof(f));
		dxgi_result r = dxgi_capture_acquire(50, dib_top(&dx), -(ptrdiff_t)dx.w * 4, 0, &f);
		if (r == DXGI_CAP_TIMEOUT) { ntimeout++; continue; }
		if (r != DXGI_CAP_OK && r != DXGI_CAP_NO_CHANGE) { ra = r; break; }
		if (r == DXGI_CAP_OK) { nimg++; } else { nnc++; }
		if (r == DXGI_CAP_OK && blt != 0)
		{
			// Like the agent's patch of the cursor field: one BitBlt of the field after every image (1 = SRCCOPY, 2 = SRCCOPY | CAPTUREBLT); does that make the system draw again?
			dxgi_rect fl;
			if (kvm_cursor_field(KVM_DXGI_POINTER_MARGIN, &fl, NULL, NULL))
			{
				HDC scr = GetDC(NULL);
				BitBlt(g1.dc, 0, 0, fl.right - fl.left, fl.bottom - fl.top, scr, fl.left, fl.top, blt == 2 ? (SRCCOPY | CAPTUREBLT) : SRCCOPY);
				GdiFlush();
				ReleaseDC(NULL, scr);
				nblt++;
			}
		}
		if (f.ptrUpdate) { if (f.ptrVisible) { updVisible++; } else { updHidden++; } } else { updNone++; }
		if (nfr < 400)
		{
			POINT p; GetCursorPos(&p);
			fr[nfr].t = (unsigned int)(now_ms() - t0); fr[nfr].res = (int)r; fr[nfr].rects = f.rectCount; fr[nfr].upd = f.ptrUpdate; fr[nfr].vis = f.ptrVisible;
			fr[nfr].x = f.ptrX; fr[nfr].y = f.ptrY; fr[nfr].shape = f.ptrShapeSize; fr[nfr].cur = p; nfr++;
		}
	}
	// Settle: take the last changes
	{
		unsigned long long until = now_ms() + 400;
		while (now_ms() < until) { pump(); memset(&f, 0, sizeof(f)); if (dxgi_capture_acquire(50, dib_top(&dx), -(ptrdiff_t)dx.w * 4, 0, &f) == DXGI_CAP_OK) { nimg++; } }
	}
	HCURSOR hc = NULL; POINT cp = { 0, 0 };
	dxgi_rect fld0, fldM;
	int showing = kvm_cursor_field(0, &fld0, &hc, &cp);
	kvm_cursor_field(KVM_DXGI_POINTER_MARGIN, &fldM, NULL, NULL);
	CURSORINFO ci; memset(&ci, 0, sizeof(ci)); ci.cbSize = sizeof(ci); GetCursorInfo(&ci);
	gdi_grab_rop(rc, &g1, SRCCOPY);
	gdi_grab_rop(rc, &g2, SRCCOPY | CAPTUREBLT);
	int l = fldM.left - rc.left, t = fldM.top - rc.top, r = fldM.right - rc.left, b = fldM.bottom - rc.top;
	long long dxVsGdiField = showing ? rect_diff(&dx, &g1, l, t, r, b) : -1;
	long long dxVsGdiAll = rect_diff(&dx, &g1, 0, 0, w, h);
	long long capVsPlainField = showing ? rect_diff(&g2, &g1, l, t, r, b) : -1;
	long long capVsPlainAll = rect_diff(&g2, &g1, 0, 0, w, h);
	long long dxVsCapField = showing ? rect_diff(&dx, &g2, l, t, r, b) : -1;

	printf("{\"mode\":\"pointer\",\"monitor\":"); json_str(g_mon[idx].name);
	printf(",\"blt_mode\":%d,\"blts\":%d,\"images_per_s\":%.1f", blt, nblt, nimg * 1000.0 / (double)(now_ms() - tstart > 0 ? now_ms() - tstart : 1));
	printf(",\"size\":\"%dx%d\",\"sysmem\":%d,\"move\":%d,\"first_frame\":{\"result\":\"%s\",\"mouse_update\":%d,\"visible\":%d},", w, h, di.systemMemory, move, dxgi_result_name(ra), firstUpd, firstVis);
	printf("\"counts\":{\"images\":%d,\"no_change\":%d,\"timeouts\":%d,\"frames_with_mouse_update_visible\":%d,\"frames_with_mouse_update_hidden\":%d,\"frames_without_mouse_update\":%d},", nimg, nnc, ntimeout, updVisible, updHidden, updNone);
	printf("\"cursor_info\":{\"showing\":%d,\"flags\":%lu,\"handle\":\"%p\",\"x\":%ld,\"y\":%ld,\"field\":[%d,%d,%d,%d],\"field_margin\":%d},", showing, (unsigned long)ci.flags, (void *)hc, cp.x - rc.left, cp.y - rc.top, l, t, r, b, KVM_DXGI_POINTER_MARGIN);
	printf("\"field_diff\":{\"dxgi_vs_gdi\":%lld,\"capture_blt_vs_plain\":%lld,\"dxgi_vs_capture_blt\":%lld},\"all_diff\":{\"dxgi_vs_gdi\":%lld,\"capture_blt_vs_plain\":%lld},", dxVsGdiField, capVsPlainField, dxVsCapField, dxVsGdiAll, capVsPlainAll);
	printf("\"frames\":[");
	for (int i = 0; i < nfr && i < 60; i++)
	{
		printf("%s{\"t\":%u,\"r\":\"%s\",\"rects\":%d,\"mouse_update\":%d,\"visible\":%d,\"x\":%d,\"y\":%d,\"shape\":%u,\"cursor_pos\":[%ld,%ld]}", i ? "," : "", fr[i].t, dxgi_result_name((dxgi_result)fr[i].res), fr[i].rects, fr[i].upd, fr[i].vis, fr[i].x, fr[i].y, fr[i].shape, fr[i].cur.x - rc.left, fr[i].cur.y - rc.top);
	}
	printf("],\"frames_total\":%d,\"pass\":1}\n", nfr);
	dxgi_capture_shutdown();
	if (move) { SetCursorPos(oldPos.x, oldPos.y); }
	dib_free(&g2); dib_free(&g1); dib_free(&dx);
	return 0;
}

// ---------------------------------------------------------------------------------------------------------------- bltcost

static double qpc_us(void)
{
	static LARGE_INTEGER fq;
	LARGE_INTEGER t;
	if (fq.QuadPart == 0) { QueryPerformanceFrequency(&fq); }
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart * 1e6 / (double)fq.QuadPart;
}

// Time of one BitBlt from the screen into a DIB: plain or CAPTUREBLT, the cursor field (80x80) or the whole screen, without a duplication,
// and with the duplication open and a frame held (like the agent's patch).
static int mode_bltcost(int reps)
{
	int idx = pick_monitor(0);
	if (idx < 0) { printf("{\"mode\":\"bltcost\",\"error\":\"no monitor\"}\n"); return 2; }
	RECT rc = g_mon[idx].rc;
	int w = rc.right - rc.left, h = rc.bottom - rc.top;
	dib_t d;
	dxgi_frame f;
	dxgi_info di;
	dxgi_rect fl;
	POINT cp;
	if (!dib_create(&d, w, h)) { printf("{\"mode\":\"bltcost\",\"error\":\"bitmap\"}\n"); return 2; }
	if (!kvm_cursor_field(KVM_DXGI_POINTER_MARGIN, &fl, NULL, &cp)) { fl.left = rc.left + 100; fl.top = rc.top + 100; fl.right = fl.left + 80; fl.bottom = fl.top + 80; }
	HDC scr = GetDC(NULL);
	printf("{\"mode\":\"bltcost\",\"size\":\"%dx%d\",\"reps\":%d,\"results\":[", w, h, reps);
	int first = 1;
	for (int held = 0; held < 2; held++)
	{
		if (held)
		{
			memset(&f, 0, sizeof(f));
			if (dxgi_capture_init(rc.left, rc.top, w, h, &di) != DXGI_CAP_OK) { break; }
			acquire_image(NULL, &d, 3000, &f); // the frame stays held
		}
		for (int mode = 0; mode < 2; mode++)
		{
			for (int full = 0; full < 2; full++)
			{
				double sum = 0, mn = 1e12, mx = 0;
				for (int i = 0; i < reps; i++)
				{
					double t0 = qpc_us();
					if (full) { BitBlt(d.dc, 0, 0, w, h, scr, rc.left, rc.top, mode ? (SRCCOPY | CAPTUREBLT) : SRCCOPY); }
					else { BitBlt(d.dc, 0, 0, fl.right - fl.left, fl.bottom - fl.top, scr, fl.left, fl.top, mode ? (SRCCOPY | CAPTUREBLT) : SRCCOPY); }
					GdiFlush();
					double dt = qpc_us() - t0;
					sum += dt; if (dt < mn) { mn = dt; } if (dt > mx) { mx = dt; }
					Sleep(25);
				}
				printf("%s{\"frame_held\":%d,\"capture_blt\":%d,\"whole_screen\":%d,\"avg_us\":%.0f,\"min_us\":%.0f,\"max_us\":%.0f}", first ? "" : ",", held, mode, full, sum / reps, mn, mx);
				first = 0;
			}
		}
		if (held) { dxgi_capture_shutdown(); }
	}
	printf("],\"pass\":1}\n");
	ReleaseDC(NULL, scr);
	dib_free(&d);
	return 0;
}

// ---------------------------------------------------------------------------------------------------------------- trail

// A software cursor that moves on a still screen (SetCursorPos every 10 ms along a path, only for a test machine): what rectangles does DXGI report,
// where does the cursor sit in the frame (found by its opaque pixels, in the rectangles of the frame only) and how does that compare with the position
// that GetCursorPos gives right after the frame was acquired.
struct trail_frame { double t; int nr; int rc[4][4]; int nf; int fx[4], fy[4], fh[4]; int fr[4]; POINT pre, post; int step; int lag; long long tried; double us; };

static int mode_trail(int seconds, int moveIt)
{
	int idx = pick_monitor(0);
	if (idx < 0) { printf("{\"mode\":\"trail\",\"error\":\"no monitor\"}\n"); return 2; }
	RECT rc = g_mon[idx].rc;
	int w = rc.right - rc.left, h = rc.bottom - rc.top;
	dib_t dx;
	dxgi_frame f;
	dxgi_info di;
	pm_sprite sp;
	static trail_frame fr[400];
	static POINT hist[4000];
	int nfr = 0, steps = 0, nimg = 0, noFind = 0, exactPost = 0, exactPre = 0, other = 0, multi = 0, bigRects = 0;
	long long totalTried = 0;
	double totalUs = 0;
	int lagHist[8] = { 0 };
	int rectW[6] = { 0 }; // histogram of the largest rectangle side: <=48, <=96, <=200, <=400, more
	POINT oldPos = { 0, 0 };

	memset(&f, 0, sizeof(f));
	memset(&sp, 0, sizeof(sp));
	if (!dib_create(&dx, w, h)) { printf("{\"mode\":\"trail\",\"error\":\"bitmap\"}\n"); return 2; }
	dxgi_result ri = dxgi_capture_init(rc.left, rc.top, w, h, &di);
	if (ri != DXGI_CAP_OK) { printf("{\"mode\":\"trail\",\"init\":\"%s\",\"pass\":0}\n", dxgi_result_name(ri)); return 1; }
	GetCursorPos(&oldPos);
	acquire_image(NULL, &dx, 4000, &f);
	{
		CURSORINFO ci; memset(&ci, 0, sizeof(ci)); ci.cbSize = sizeof(ci); GetCursorInfo(&ci);
		kvm_cursor_sprite(ci.hCursor, &sp);
	}
	printf("{\"mode\":\"trail\",\"size\":\"%dx%d\",\"move\":%d,\"sprite\":{\"w\":%d,\"h\":%d,\"hotspot\":[%d,%d],\"opaque\":%d,\"monochrome\":%d},", w, h, moveIt, sp.w, sp.h, sp.hx, sp.hy, sp.n, sp.monochrome);
	int cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2;
	double t0 = qpc_us(), nextMove = t0;
	while (qpc_us() - t0 < seconds * 1e6 && sp.n > 0)
	{
		double now = qpc_us();
		if (moveIt && now >= nextMove && steps < 4000)
		{
			double t = (now - t0) / 1000.0;
			POINT p = { cx + (int)(w * 0.38 * cos(t * 0.0021)), cy + (int)(h * 0.38 * sin(t * 0.0033)) };
			SetCursorPos(p.x, p.y);
			hist[steps].x = p.x - rc.left; hist[steps].y = p.y - rc.top; steps++;
			nextMove += 10000.0;
		}
		pump();
		POINT pre; GetCursorPos(&pre);
		memset(&f, 0, sizeof(f));
		dxgi_result r = dxgi_capture_acquire(6, dib_top(&dx), -(ptrdiff_t)dx.w * 4, 0, &f);
		if (r != DXGI_CAP_OK || f.rectCount == 0) { continue; }
		nimg++;
		trail_frame *tf = (nfr < 400) ? &fr[nfr] : NULL;
		trail_frame tmp; if (tf == NULL) { tf = &tmp; }
		memset(tf, 0, sizeof(*tf));
		GetCursorPos(&tf->post);
		tf->pre = pre; tf->t = (qpc_us() - t0) / 1000.0; tf->step = steps - 1; tf->nr = f.rectCount;
		double q0 = qpc_us();
		int maxSide = 0;
		for (int i = 0; i < f.rectCount; i++)
		{
			const dxgi_rect &q = f.rects[i];
			int side = (q.right - q.left > q.bottom - q.top) ? q.right - q.left : q.bottom - q.top;
			if (side > maxSide) { maxSide = side; }
			if (i < 4) { tf->rc[i][0] = q.left; tf->rc[i][1] = q.top; tf->rc[i][2] = q.right; tf->rc[i][3] = q.bottom; }
			if (side > 400) { bigRects++; continue; }
			int bx = 0, by = 0, hits = 0;
			if (pm_find(dx.bits, w, h, &sp, *(const pm_rect *)&q, &bx, &by, &hits, 400000, &tf->tried) && tf->nf < 4)
			{
				tf->fx[tf->nf] = bx + sp.hx; tf->fy[tf->nf] = by + sp.hy; tf->fh[tf->nf] = hits; tf->fr[tf->nf] = i; tf->nf++;
			}
		}
		tf->us = qpc_us() - q0;
		totalUs += tf->us; totalTried += tf->tried;
		rectW[maxSide <= 48 ? 0 : (maxSide <= 96 ? 1 : (maxSide <= 200 ? 2 : (maxSide <= 400 ? 3 : 4)))]++;
		if (tf->nf == 0) { noFind++; }
		else
		{
			if (tf->nf > 1) { multi++; }
			int px = tf->post.x - rc.left, py = tf->post.y - rc.top, qx = tf->pre.x - rc.left, qy = tf->pre.y - rc.top;
			int isPost = 0, isPre = 0;
			for (int k = 0; k < tf->nf; k++) { if (tf->fx[k] == px && tf->fy[k] == py) { isPost = 1; } if (tf->fx[k] == qx && tf->fy[k] == qy) { isPre = 1; } }
			if (isPost) { exactPost++; } else if (isPre) { exactPre++; } else { other++; }
			// lag in steps behind the last position set
			tf->lag = -1;
			for (int k = 0; k < tf->nf && tf->lag < 0; k++)
				for (int b = 0; b < 40 && steps - 1 - b >= 0; b++) { if (hist[steps - 1 - b].x == tf->fx[k] && hist[steps - 1 - b].y == tf->fy[k]) { tf->lag = b; break; } }
			if (tf->lag >= 0) { lagHist[tf->lag > 6 ? 7 : tf->lag]++; }
		}
		if (nfr < 400) { nfr++; }
	}
	printf("\"images\":%d,\"steps\":%d,\"not_found\":%d,\"found_equals_getcursorpos_after\":%d,\"found_equals_getcursorpos_before\":%d,\"found_elsewhere\":%d,\"frames_with_several_matches\":%d,\"rects_over_400px\":%d,", nimg, steps, noFind, exactPost, exactPre, other, multi, bigRects);
	printf("\"search_us_avg\":%.1f,\"positions_tried_avg\":%.0f,\"largest_rect_side_hist\":{\"le48\":%d,\"le96\":%d,\"le200\":%d,\"le400\":%d,\"over400\":%d},\"lag_steps_hist\":[%d,%d,%d,%d,%d,%d,%d,%d],", nimg ? totalUs / nimg : 0.0, nimg ? (double)totalTried / nimg : 0.0, rectW[0], rectW[1], rectW[2], rectW[3], rectW[4], lagHist[0], lagHist[1], lagHist[2], lagHist[3], lagHist[4], lagHist[5], lagHist[6], lagHist[7]);
	// the sprite as text: # dark, o light, + other opaque, . not opaque
	printf("\"sprite_rows\":[");
	if (sp.n > 0)
	{
		for (int y = 0; y < sp.h; y++)
		{
			char row[300]; int k = 0;
			for (int x = 0; x < sp.w && k < 290; x++)
			{
				char c = '.';
				for (int i = 0; i < sp.n; i++) { if (sp.x[i] == x && sp.y[i] == y) { int v = (sp.rgb[i * 3] + sp.rgb[i * 3 + 1] + sp.rgb[i * 3 + 2]) / 3; c = v < 60 ? '#' : (v > 200 ? 'o' : '+'); break; } }
				row[k++] = c;
			}
			row[k] = 0;
			printf("%s\"%s\"", y ? "," : "", row);
		}
	}
	printf("],\"frames\":[");
	for (int i = 0; i < nfr && i < 70; i++)
	{
		const trail_frame &tf = fr[i];
		printf("%s{\"t\":%.0f,\"rects\":[", i ? "," : "", tf.t);
		for (int k = 0; k < tf.nr && k < 4; k++) { printf("%s[%d,%d,%d,%d]", k ? "," : "", tf.rc[k][0], tf.rc[k][1], tf.rc[k][2], tf.rc[k][3]); }
		printf("],\"n_rects\":%d,\"found\":[", tf.nr);
		for (int k = 0; k < tf.nf; k++) { printf("%s[%d,%d,%d,r%d]", k ? "," : "", tf.fx[k], tf.fy[k], tf.fh[k], tf.fr[k]); }
		printf("],\"pre\":[%ld,%ld],\"post\":[%ld,%ld],\"lag_steps\":%d,\"us\":%.0f}", tf.pre.x - rc.left, tf.pre.y - rc.top, tf.post.x - rc.left, tf.post.y - rc.top, tf.lag, tf.us);
	}
	printf("],\"pass\":1}\n");
	pm_sprite_free(&sp);
	dxgi_capture_shutdown();
	if (moveIt) { SetCursorPos(oldPos.x, oldPos.y); }
	dib_free(&dx);
	return 0;
}

// ---------------------------------------------------------------------------------------------------------------- idle

static unsigned long long proc_cpu_100ns(void)
{
	FILETIME c, e, k, u;
	if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) { return 0; }
	return (((unsigned long long)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((unsigned long long)u.dwHighDateTime << 32) | u.dwLowDateTime);
}

static int mode_idle(int seconds)
{
	int idx = pick_monitor(0);
	if (idx < 0) { printf("{\"mode\":\"idle\",\"error\":\"no monitor\"}\n"); return 2; }
	RECT rc = g_mon[idx].rc;
	int w = rc.right - rc.left, h = rc.bottom - rc.top;
	dib_t dx;
	dxgi_frame f;
	long long images = 0, timeouts = 0, noChange = 0, other = 0;
	if (seconds < 1) { seconds = 1; }
	memset(&f, 0, sizeof(f));
	if (!dib_create(&dx, w, h)) { printf("{\"mode\":\"idle\",\"error\":\"bitmap\"}\n"); return 2; }
	dxgi_result ri = dxgi_capture_init(rc.left, rc.top, w, h, NULL);
	if (ri != DXGI_CAP_OK)
	{
		printf("{\"mode\":\"idle\",\"init\":\"%s\",\"init_hr\":\"0x%08x\",\"pass\":0}\n", dxgi_result_name(ri), (unsigned int)dxgi_capture_last_hr());
		dib_free(&dx);
		return 1;
	}
	// The first image arrives at once; the CPU measurement starts after it
	for (int i = 0; i < 20; i++)
	{
		if (dxgi_capture_acquire(100, dib_top(&dx), -(ptrdiff_t)w * 4, 1, &f) == DXGI_CAP_OK) { break; }
	}
	unsigned long long cpu0 = proc_cpu_100ns(), t0 = now_ms(), end = t0 + (unsigned long long)seconds * 1000;
	while (now_ms() < end)
	{
		dxgi_result r = dxgi_capture_acquire(100, dib_top(&dx), -(ptrdiff_t)w * 4, 0, &f);
		if (r == DXGI_CAP_OK) { images++; }
		else if (r == DXGI_CAP_TIMEOUT) { timeouts++; }
		else if (r == DXGI_CAP_NO_CHANGE) { noChange++; }
		else { other++; Sleep(100); dxgi_capture_init(rc.left, rc.top, w, h, NULL); }
	}
	unsigned long long cpu = proc_cpu_100ns() - cpu0, wall = now_ms() - t0;
	printf("{\"mode\":\"idle\",\"seconds\":%.1f,\"cpu_ms\":%.1f,\"cpu_percent\":%.3f,\"images\":%lld,\"timeouts\":%lld,\"no_change\":%lld,\"other\":%lld,\"pass\":1}\n",
		wall / 1000.0, cpu / 10000.0, wall > 0 ? (cpu / 10000.0) * 100.0 / (double)wall : 0.0, images, timeouts, noChange, other);
	dxgi_capture_shutdown();
	dib_free(&dx);
	return 0;
}

// ---------------------------------------------------------------------------------------------------------------- stalecost

// The check that the agent makes (tile.cpp, dx_stale_compare): BitBlt of the screen with CAPTUREBLT into a new DIB and a count of the
// differing pixels against the persistent image, outside the cursor.
static int mode_stalecost(int n)
{
	int idx = pick_monitor(0);
	if (idx < 0) { printf("{\"mode\":\"stalecost\",\"error\":\"no monitor\"}\n"); return 2; }
	RECT rc = g_mon[idx].rc;
	int w = rc.right - rc.left, h = rc.bottom - rc.top;
	dib_t ref;
	double sum = 0, mx = 0;
	long long diffTotal = 0;
	if (n < 1) { n = 20; }
	if (!dib_create(&ref, w, h)) { printf("{\"mode\":\"stalecost\",\"error\":\"bitmap\"}\n"); return 2; }
	gdi_grab(rc, &ref);
	unsigned long long cpu0 = 0;
	{
		FILETIME c, e, k, u;
		GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
		cpu0 = (((unsigned long long)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((unsigned long long)u.dwHighDateTime << 32) | u.dwLowDateTime);
	}
	for (int i = 0; i < n; i++)
	{
		LARGE_INTEGER a, b, fq;
		POINT pt;
		dxgi_rect skip;
		QueryPerformanceFrequency(&fq);
		QueryPerformanceCounter(&a);
		HDC scr = GetDC(NULL);
		dib_t cur;
		if (dib_create(&cur, w, h))
		{
			BitBlt(cur.dc, 0, 0, w, h, scr, rc.left, rc.top, SRCCOPY | CAPTUREBLT);
			GdiFlush();
			GetCursorPos(&pt);
			skip.left = pt.x - rc.left - 64; skip.right = skip.left + 192; skip.top = pt.y - rc.top - 64; skip.bottom = skip.top + 192;
			diffTotal += kvm_count_pixel_diff(dib_top(&ref), -(ptrdiff_t)w * 4, dib_top(&cur), -(ptrdiff_t)w * 4, w, h, &skip);
			dib_free(&cur);
		}
		ReleaseDC(NULL, scr);
		QueryPerformanceCounter(&b);
		double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)fq.QuadPart;
		sum += ms;
		if (ms > mx) { mx = ms; }
		Sleep(50);
	}
	FILETIME c, e, k, u;
	GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
	unsigned long long cpu1 = (((unsigned long long)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((unsigned long long)u.dwHighDateTime << 32) | u.dwLowDateTime);
	double avg = sum / n;
	printf("{\"mode\":\"stalecost\",\"size\":\"%dx%d\",\"checks\":%d,\"wall_ms_avg\":%.2f,\"wall_ms_max\":%.2f,\"cpu_ms_avg\":%.2f,\"percent_of_a_core_every_10s\":%.3f,\"diff_pixels_total\":%lld,\"pass\":1}\n",
		w, h, n, avg, mx, (cpu1 - cpu0) / 10000.0 / n, ((cpu1 - cpu0) / 10000.0 / n) / 100.0, diffTotal);
	dib_free(&ref);
	return 0;
}

int main(int argc, char **argv)
{
	const char *mode = (argc > 1) ? argv[1] : "info";
	set_dpi_aware();
	if (strcmp(mode, "info") == 0) { return mode_info(); }
	if (strcmp(mode, "compare") == 0)
	{
		int mon = 0;
		for (int i = 2; i + 1 < argc; i++) { if (strcmp(argv[i], "--monitor") == 0) { mon = atoi(argv[i + 1]); } }
		return mode_compare(mon);
	}
	if (strcmp(mode, "rects") == 0)
	{
		int bb = 0;
		for (int i = 3; i + 1 < argc; i++) { if (strcmp(argv[i], "--bbox") == 0) { bb = atoi(argv[i + 1]); } }
		return mode_rects(argc > 2 ? atoi(argv[2]) : 5, bb);
	}
	if (strcmp(mode, "cursor") == 0)
	{
		int ib = 0;
		for (int i = 2; i < argc; i++) { if (strcmp(argv[i], "--ibeam") == 0) { ib = 1; } }
		return mode_cursor(ib);
	}
	if (strcmp(mode, "stalecost") == 0) { return mode_stalecost(argc > 2 ? atoi(argv[2]) : 20); }
	if (strcmp(mode, "pointer") == 0)
	{
		int mv = 0, secs = 4, bl = 0;
		for (int i = 2; i < argc; i++) { if (strcmp(argv[i], "--blt=plain") == 0) { bl = 1; } else if (strcmp(argv[i], "--blt=capture") == 0) { bl = 2; } else if (strcmp(argv[i], "--move") == 0) { mv = 1; } else if (atoi(argv[i]) > 0) { secs = atoi(argv[i]); } }
		return mode_pointer(secs, mv, bl);
	}
	if (strcmp(mode, "trail") == 0)
	{
		int mv = 0, secs = 3;
		for (int i = 2; i < argc; i++) { if (strcmp(argv[i], "--move") == 0) { mv = 1; } else if (atoi(argv[i]) > 0) { secs = atoi(argv[i]); } }
		return mode_trail(secs, mv);
	}
	if (strcmp(mode, "bltcost") == 0) { return mode_bltcost(argc > 2 ? atoi(argv[2]) : 20); }
	if (strcmp(mode, "idle") == 0) { return mode_idle(argc > 2 ? atoi(argv[2]) : 10); }
	printf("{\"error\":\"usage: kvm-dxgi-check info | compare [--monitor n] | rects <seconds> [--bbox n] | cursor [--ibeam] | pointer [seconds] [--move] | trail [seconds] [--move] | idle <seconds> | stalecost [n]\"}\n");
	return 2;
}
