// KVM (Windows): DXGI Desktop Duplication capture with dirty rectangles. See dxgi_capture.h for the interface.
//
// Everything with state must be called from one thread (the capture thread). D3D11/DXGI are loaded at run time
// (LoadLibraryEx), so the agent still starts on systems without them; no CoInitialize is needed.
// This file must not include microstack or agent headers.

#include "dxgi_capture.h"
#include "dxgi_ptrmask.h"

#ifdef KVM_DXGI_ENABLED

#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdlib.h>
#include <string.h>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#define DXGI_SSE2 1
#endif

typedef HRESULT(WINAPI *dxgi_PFN_CreateDXGIFactory1)(REFIID riid, void **ppFactory);

static HMODULE g_dxgiDll = NULL, g_d3dDll = NULL;
static dxgi_PFN_CreateDXGIFactory1 g_createFactory1 = NULL;
static PFN_D3D11_CREATE_DEVICE g_createDevice = NULL;
static int g_loadState = 0; // 0 = not tried, 1 = loaded, 2 = failed

static ID3D11Device *g_dev = NULL;
static ID3D11DeviceContext *g_ctx = NULL;
static IDXGIOutputDuplication *g_dupl = NULL;
static ID3D11Texture2D *g_staging = NULL;
static int g_w = 0, g_h = 0;          // output size (the requested rectangle)
static int g_held = 0;                // a frame is held (ReleaseFrame is due)
static int g_sysmem = 0;              // DesktopImageInSystemMemory
static int g_frames = 0;              // images handled since init
static HRESULT g_hr = S_OK;

static unsigned char *g_metaMove = NULL, *g_metaDirty = NULL; // metadata buffers, freed by dxgi_capture_shutdown()
static UINT g_metaCap = 0;
static dxgi_rect *g_rects = NULL;
static int g_rectsCap = 0;
static int g_bboxRects = KVM_DXGI_BBOX_RECTS;
static dxgi_precopy_fn g_precopy = NULL;
static HRESULT g_releaseHr = S_OK;      // result of the last ReleaseFrame
static int g_releaseLost = 0;           // ReleaseFrame reported a lost duplication: the next acquire reports it at once
static long long g_cntSysmem = 0, g_cntStaging = 0, g_cntBbox = 0;

template <class T> static void safe_release(T *&p)
{
	if (p != NULL) { p->Release(); p = NULL; }
}

static void copy_wide(wchar_t *dst, size_t cap, const wchar_t *src)
{
	size_t i = 0;
	if (cap == 0) { return; }
	for (; i + 1 < cap && src[i] != 0; i++) { dst[i] = src[i]; }
	dst[i] = 0;
}

static HMODULE load_system_dll(const wchar_t *name)
{
	// LOAD_LIBRARY_SEARCH_SYSTEM32 does not exist on Windows 7 without a patch, then use the plain call (same as kvm.c).
	HMODULE m = LoadLibraryExW(name, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (m == NULL && GetLastError() == ERROR_INVALID_PARAMETER) { m = LoadLibraryW(name); }
	return m;
}

static int load_libs(void)
{
	if (g_loadState != 0) { return g_loadState == 1; }
	g_loadState = 2;
	g_dxgiDll = load_system_dll(L"dxgi.dll");
	g_d3dDll = load_system_dll(L"d3d11.dll");
	if (g_dxgiDll == NULL || g_d3dDll == NULL) { return 0; }
	g_createFactory1 = (dxgi_PFN_CreateDXGIFactory1)GetProcAddress(g_dxgiDll, "CreateDXGIFactory1");
	g_createDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(g_d3dDll, "D3D11CreateDevice");
	if (g_createFactory1 == NULL || g_createDevice == NULL) { return 0; }
	g_loadState = 1;
	return 1;
}

static int is_lost(HRESULT hr)
{
	return hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET;
}

static int is_transient(HRESULT hr)
{
	return hr == E_ACCESSDENIED || hr == DXGI_ERROR_SESSION_DISCONNECTED || hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
}

// Makes both metadata buffers at least 'need' bytes (contents are not kept).
static int ensure_meta(UINT need)
{
	if (need <= g_metaCap) { return 1; }
	if (need < 4096) { need = 4096; }
	unsigned char *a = (unsigned char *)realloc(g_metaMove, need);
	if (a != NULL) { g_metaMove = a; }
	unsigned char *b = (unsigned char *)realloc(g_metaDirty, need);
	if (b != NULL) { g_metaDirty = b; }
	if (a == NULL || b == NULL) { return 0; }
	g_metaCap = need;
	return 1;
}

static int ensure_rects(int need)
{
	if (need <= g_rectsCap) { return 1; }
	if (need < 64) { need = 64; }
	dxgi_rect *r = (dxgi_rect *)realloc(g_rects, sizeof(dxgi_rect) * (size_t)need);
	if (r == NULL) { return 0; }
	g_rects = r;
	g_rectsCap = need;
	return 1;
}

// Reads the move and dirty rectangles of the held frame into g_metaMove / g_metaDirty. Returns 0 if they are not usable.
static int fetch_metadata(UINT total, UINT *nMove, UINT *nDirty)
{
	for (int attempt = 0; attempt < 3; attempt++)
	{
		UINT szMove = 0, szDirty = 0;
		HRESULT h;
		if (!ensure_meta(total)) { return 0; }
		h = g_dupl->GetFrameMoveRects(g_metaCap, (DXGI_OUTDUPL_MOVE_RECT *)g_metaMove, &szMove);
		if (h == DXGI_ERROR_MORE_DATA) { total = (szMove > g_metaCap) ? szMove : (g_metaCap * 2); continue; }
		if (FAILED(h)) { return 0; }
		h = g_dupl->GetFrameDirtyRects(g_metaCap, (RECT *)g_metaDirty, &szDirty);
		if (h == DXGI_ERROR_MORE_DATA) { total = (szDirty > g_metaCap) ? szDirty : (g_metaCap * 2); continue; }
		if (FAILED(h)) { return 0; }
		*nMove = szMove / (UINT)sizeof(DXGI_OUTDUPL_MOVE_RECT);
		*nDirty = szDirty / (UINT)sizeof(RECT);
		return 1;
	}
	return 0;
}

static long long rect_area(const RECT &r)
{
	long long w = (long long)r.right - r.left, h = (long long)r.bottom - r.top;
	return (w > 0 && h > 0) ? w * h : 0;
}

// Adds the rectangle clipped to the output to g_rects, empty results are dropped.
static void add_rect(int &count, const RECT &r)
{
	int l = r.left < 0 ? 0 : r.left, t = r.top < 0 ? 0 : r.top;
	int rr = r.right > g_w ? g_w : r.right, b = r.bottom > g_h ? g_h : r.bottom;
	if (rr <= l || b <= t) { return; }
	g_rects[count].left = l; g_rects[count].top = t; g_rects[count].right = rr; g_rects[count].bottom = b;
	count++;
}

// Copies the pixels of one rectangle from the mapped source (top-down, srcPitch bytes per row) to the destination.
static void copy_region(const unsigned char *src, size_t srcPitch, unsigned char *dstTop, ptrdiff_t dstStride, const dxgi_rect &q)
{
	size_t bytes = (size_t)(q.right - q.left) * 4;
	for (int y = q.top; y < q.bottom; y++)
	{
		const unsigned char *s = src + ((size_t)y * srcPitch) + ((size_t)q.left * 4);
		unsigned char *d = dstTop + ((ptrdiff_t)y * dstStride) + ((ptrdiff_t)q.left * 4);
#if KVM_DXGI_CLEAR_ALPHA
		size_t i = 0;
#ifdef DXGI_SSE2
		const __m128i mask = _mm_set1_epi32(0x00FFFFFF);
		for (; i + 16 <= bytes; i += 16)
		{
			_mm_storeu_si128((__m128i *)(d + i), _mm_and_si128(_mm_loadu_si128((const __m128i *)(s + i)), mask));
		}
#endif
		for (; i + 4 <= bytes; i += 4)
		{
			unsigned int v;
			memcpy(&v, s + i, 4);
			v &= 0x00FFFFFFu;
			memcpy(d + i, &v, 4);
		}
#else
		memcpy(d, s, bytes);
#endif
	}
}

extern "C"
{

dxgi_result dxgi_capture_init(int x, int y, int w, int h, dxgi_info *info)
{
	IDXGIFactory1 *factory = NULL;
	IDXGIAdapter1 *adapter = NULL;
	IDXGIOutput *output = NULL;
	IDXGIOutput1 *output1 = NULL;
	DXGI_OUTPUT_DESC od;
	DXGI_ADAPTER_DESC1 ad;
	DXGI_OUTDUPL_DESC dd;
	D3D_FEATURE_LEVEL got;
	dxgi_result result = DXGI_CAP_ERROR;
	HRESULT hr;
	static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_1 };

	dxgi_capture_shutdown();
	g_hr = S_OK;
	g_releaseHr = S_OK;
	memset(&od, 0, sizeof(od));
	memset(&ad, 0, sizeof(ad));
	memset(&dd, 0, sizeof(dd));
	if (info != NULL) { memset(info, 0, sizeof(*info)); }
	if (w <= 0 || h <= 0) { return DXGI_CAP_NO_OUTPUT; }
	if (!load_libs()) { g_hr = HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND); return DXGI_CAP_UNSUPPORTED; }

	// A new factory every time: after a topology change an old factory lists stale adapters.
	if (FAILED(hr = g_createFactory1(__uuidof(IDXGIFactory1), (void **)&factory))) { g_hr = hr; return DXGI_CAP_ERROR; }

	for (UINT ai = 0; output == NULL; ai++)
	{
		IDXGIAdapter1 *a = NULL;
		if (FAILED(factory->EnumAdapters1(ai, &a))) { break; }
		for (UINT oi = 0; ; oi++)
		{
			IDXGIOutput *o = NULL;
			DXGI_OUTPUT_DESC d;
			if (FAILED(a->EnumOutputs(oi, &o))) { break; }
			if (SUCCEEDED(o->GetDesc(&d)) && d.AttachedToDesktop &&
				d.DesktopCoordinates.left == x && d.DesktopCoordinates.top == y &&
				d.DesktopCoordinates.right == x + w && d.DesktopCoordinates.bottom == y + h)
			{
				output = o;
				od = d;
				break;
			}
			o->Release();
		}
		if (output != NULL) { adapter = a; } else { a->Release(); }
	}
	if (output == NULL) { result = DXGI_CAP_NO_OUTPUT; goto done; }

	if (info != NULL)
	{
		if (SUCCEEDED(adapter->GetDesc1(&ad)))
		{
			copy_wide(info->adapter, sizeof(info->adapter) / sizeof(wchar_t), ad.Description);
			info->vendorId = ad.VendorId;
			info->deviceId = ad.DeviceId;
		}
		copy_wide(info->output, sizeof(info->output) / sizeof(wchar_t), od.DeviceName);
		info->width = w;
		info->height = h;
		info->rotation = (int)od.Rotation;
	}
	if (od.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED && od.Rotation != DXGI_MODE_ROTATION_IDENTITY) { result = DXGI_CAP_ROTATED; goto done; }

	// The device has to be created on the adapter that owns the output, otherwise DuplicateOutput fails (hybrid graphics).
	hr = g_createDevice((IDXGIAdapter *)adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, levels, (UINT)(sizeof(levels) / sizeof(levels[0])), D3D11_SDK_VERSION, &g_dev, &got, &g_ctx);
	if (FAILED(hr)) { g_hr = hr; result = DXGI_CAP_ERROR; goto done; } // Not "unsupported": a driver reset or update can fail it once

	hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void **)&output1);
	if (FAILED(hr)) { g_hr = hr; result = (hr == E_NOINTERFACE) ? DXGI_CAP_UNSUPPORTED : DXGI_CAP_ERROR; goto done; }

	hr = output1->DuplicateOutput(g_dev, &g_dupl);
	if (FAILED(hr)) { g_hr = hr; result = is_transient(hr) ? DXGI_CAP_TRANSIENT : DXGI_CAP_ERROR; goto done; }

	g_dupl->GetDesc(&dd);
	if ((int)dd.ModeDesc.Width != w || (int)dd.ModeDesc.Height != h)
	{
		g_hr = DXGI_ERROR_MODE_CHANGE_IN_PROGRESS; // The mode changes while we look at it, try again later
		result = DXGI_CAP_TRANSIENT;
		goto done;
	}

	g_w = w;
	g_h = h;
	g_sysmem = dd.DesktopImageInSystemMemory ? 1 : 0;
	g_frames = 0;
	g_held = 0;
	if (info != NULL) { info->systemMemory = g_sysmem; }
	result = DXGI_CAP_OK;

done:
	safe_release(output1);
	safe_release(output);
	safe_release(adapter);
	safe_release(factory);
	if (result != DXGI_CAP_OK) { dxgi_capture_shutdown(); }
	return result;
}

dxgi_result dxgi_capture_acquire(unsigned int timeoutMs, unsigned char *dstTop, ptrdiff_t dstStride, int wantFull, dxgi_frame *frame)
{
	DXGI_OUTDUPL_FRAME_INFO fi;
	IDXGIResource *res = NULL;
	ID3D11Texture2D *tex = NULL;
	HRESULT hr = S_OK;
	dxgi_frame local;
	LARGE_INTEGER t0, t1, freq;
	int full, metaOk = 0, count = 0, mapped = 0, sysmapped = 0;
	UINT nMove = 0, nDirty = 0, i;
	const unsigned char *srcBits = NULL;
	size_t srcPitch = 0;
	D3D11_MAPPED_SUBRESOURCE ms;
	dxgi_rect bbox = { 0, 0, 0, 0 };
	dxgi_result result = DXGI_CAP_ERROR;

	if (frame == NULL) { frame = &local; }
	memset(frame, 0, sizeof(*frame));
	memset(&fi, 0, sizeof(fi));
	memset(&ms, 0, sizeof(ms));

	dxgi_capture_release();
	if (g_dupl == NULL) { g_hr = E_UNEXPECTED; return DXGI_CAP_ERROR; }
	if (g_releaseLost)
	{
		// ReleaseFrame already told that the duplication is gone
		g_hr = g_releaseHr;
		dxgi_capture_shutdown();
		return DXGI_CAP_LOST;
	}

	hr = g_dupl->AcquireNextFrame(timeoutMs, &fi, &res);
	if (hr == DXGI_ERROR_WAIT_TIMEOUT) { return DXGI_CAP_TIMEOUT; } // No frame is held after a timeout
	if (FAILED(hr))
	{
		g_hr = hr;
		dxgi_capture_shutdown();
		// INVALID_CALL: seen on Hyper-V right after a mode or desktop change, the state of the output is in flux: try again later
		if (hr == DXGI_ERROR_INVALID_CALL) { return DXGI_CAP_TRANSIENT; }
		return is_lost(hr) ? DXGI_CAP_LOST : (is_transient(hr) ? DXGI_CAP_TRANSIENT : DXGI_CAP_ERROR);
	}
	g_held = 1;
	frame->accumulatedFrames = (int)fi.AccumulatedFrames;
	frame->ptrUpdate = (fi.LastMouseUpdateTime.QuadPart != 0) ? 1 : 0;
	frame->ptrVisible = fi.PointerPosition.Visible ? 1 : 0;
	frame->ptrX = (int)fi.PointerPosition.Position.x;
	frame->ptrY = (int)fi.PointerPosition.Position.y;
	frame->ptrShapeSize = fi.PointerShapeBufferSize;

	if (fi.LastPresentTime.QuadPart == 0)
	{
		// Only the cursor changed: no new image and no rectangles. The frame stays held until the next call.
		safe_release(res);
		return DXGI_CAP_NO_CHANGE;
	}

	// Rectangles. Move destinations are not part of the dirty list but hold new pixels, so they are copied as well.
	if (fi.TotalMetadataBufferSize > 0 && fetch_metadata(fi.TotalMetadataBufferSize, &nMove, &nDirty) && ensure_rects((int)(nMove + nDirty) + 1))
	{
		metaOk = 1;
	}
	full = (wantFull || g_frames == 0 || !metaOk) ? 1 : 0; // Without usable metadata everything is copied (safe)
	if (metaOk)
	{
		const DXGI_OUTDUPL_MOVE_RECT *mv = (const DXGI_OUTDUPL_MOVE_RECT *)g_metaMove;
		const RECT *dr = (const RECT *)g_metaDirty;
		frame->dirtyCount = (int)nDirty;
		frame->moveCount = (int)nMove;
		for (i = 0; i < nDirty; i++) { frame->dirtyArea += rect_area(dr[i]); }
		for (i = 0; i < nMove; i++) { frame->moveArea += rect_area(mv[i].DestinationRect); }
		if (!full)
		{
			for (i = 0; i < nDirty; i++) { add_rect(count, dr[i]); }
			for (i = 0; i < nMove; i++) { add_rect(count, mv[i].DestinationRect); }
		}
	}
	if (full)
	{
		// One rectangle for the whole output, so the caller marks every tile.
		if (!ensure_rects(1)) { safe_release(res); g_hr = E_OUTOFMEMORY; dxgi_capture_shutdown(); return DXGI_CAP_ERROR; }
		count = 1;
		g_rects[0].left = 0; g_rects[0].top = 0; g_rects[0].right = g_w; g_rects[0].bottom = g_h;
	}
	frame->rectCount = count;
	frame->rects = g_rects;
	frame->fullCopy = full;

	if (count == 0)
	{
		// The image changed, but only outside of the output rectangle: nothing to copy.
		safe_release(res);
		g_frames++;
		return DXGI_CAP_OK;
	}

	if (g_precopy != NULL)
	{
		dxgi_rect hb;
		if (!full && count > g_bboxRects)
		{
			hb = g_rects[0];
			for (i = 1; i < (UINT)count; i++)
			{
				if (g_rects[i].left < hb.left) { hb.left = g_rects[i].left; }
				if (g_rects[i].top < hb.top) { hb.top = g_rects[i].top; }
				if (g_rects[i].right > hb.right) { hb.right = g_rects[i].right; }
				if (g_rects[i].bottom > hb.bottom) { hb.bottom = g_rects[i].bottom; }
			}
			g_precopy(0, 1, &hb);
		}
		else { g_precopy(full, count, g_rects); }
	}

	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);

	// The texture of the frame: its size is checked in both modes (a mode change can deliver a frame of the new size before ACCESS_LOST).
	if (res != NULL && SUCCEEDED(hr = res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&tex)))
	{
		D3D11_TEXTURE2D_DESC td0;
		tex->GetDesc(&td0);
		if ((int)td0.Width != g_w || (int)td0.Height != g_h) { g_hr = DXGI_ERROR_MODE_CHANGE_IN_PROGRESS; result = DXGI_CAP_TRANSIENT; goto fail; }
	}

	// Source: the desktop surface in system memory (VMs, basic display drivers) or a staging texture.
	if (g_sysmem)
	{
		DXGI_MAPPED_RECT mr;
		memset(&mr, 0, sizeof(mr));
		if (SUCCEEDED(g_dupl->MapDesktopSurface(&mr)) && mr.pBits != NULL && mr.Pitch > 0)
		{
			srcBits = mr.pBits;
			srcPitch = (size_t)mr.Pitch;
			sysmapped = 1;
			if (mr.Pitch < g_w * 4) { g_hr = DXGI_ERROR_MODE_CHANGE_IN_PROGRESS; result = DXGI_CAP_TRANSIENT; goto fail; }
		}
		else
		{
			g_sysmem = 0; // MapDesktopSurface is not usable here, use the texture from now on
		}
	}
	if (!sysmapped)
	{
		D3D11_TEXTURE2D_DESC td;
		if (tex == NULL)
		{
			g_hr = (res == NULL) ? E_POINTER : hr;
			result = DXGI_CAP_ERROR;
			goto fail;
		}
		tex->GetDesc(&td);
		if (td.Format != DXGI_FORMAT_B8G8R8A8_UNORM) { g_hr = DXGI_ERROR_UNSUPPORTED; result = DXGI_CAP_ERROR; goto fail; }
		if (g_staging == NULL)
		{
			D3D11_TEXTURE2D_DESC sd = td;
			sd.Usage = D3D11_USAGE_STAGING;
			sd.BindFlags = 0;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			sd.MiscFlags = 0;
			sd.MipLevels = 1;
			sd.ArraySize = 1;
			sd.SampleDesc.Count = 1;
			sd.SampleDesc.Quality = 0;
			if (FAILED(hr = g_dev->CreateTexture2D(&sd, NULL, &g_staging))) { g_hr = hr; result = is_lost(hr) ? DXGI_CAP_LOST : DXGI_CAP_ERROR; goto fail; }
		}

		if (full)
		{
			g_ctx->CopyResource(g_staging, tex);
		}
		else if (count > g_bboxRects)
		{
			bbox = g_rects[0];
			for (i = 1; i < (UINT)count; i++)
			{
				if (g_rects[i].left < bbox.left) { bbox.left = g_rects[i].left; }
				if (g_rects[i].top < bbox.top) { bbox.top = g_rects[i].top; }
				if (g_rects[i].right > bbox.right) { bbox.right = g_rects[i].right; }
				if (g_rects[i].bottom > bbox.bottom) { bbox.bottom = g_rects[i].bottom; }
			}
			D3D11_BOX box = { (UINT)bbox.left, (UINT)bbox.top, 0, (UINT)bbox.right, (UINT)bbox.bottom, 1 };
			g_ctx->CopySubresourceRegion(g_staging, 0, (UINT)bbox.left, (UINT)bbox.top, 0, tex, 0, &box);
			g_cntBbox++;
		}
		else
		{
			for (i = 0; i < (UINT)count; i++)
			{
				D3D11_BOX box = { (UINT)g_rects[i].left, (UINT)g_rects[i].top, 0, (UINT)g_rects[i].right, (UINT)g_rects[i].bottom, 1 };
				g_ctx->CopySubresourceRegion(g_staging, 0, (UINT)g_rects[i].left, (UINT)g_rects[i].top, 0, tex, 0, &box);
			}
		}

		if (FAILED(hr = g_ctx->Map(g_staging, 0, D3D11_MAP_READ, 0, &ms))) { g_hr = hr; result = is_lost(hr) ? DXGI_CAP_LOST : DXGI_CAP_ERROR; goto fail; }
		mapped = 1;
		srcBits = (const unsigned char *)ms.pData;
		srcPitch = (size_t)ms.RowPitch;
	}

	if (full)
	{
		copy_region(srcBits, srcPitch, dstTop, dstStride, g_rects[0]);
	}
	else if (count > g_bboxRects && !sysmapped)
	{
		copy_region(srcBits, srcPitch, dstTop, dstStride, bbox);
	}
	else
	{
		for (i = 0; i < (UINT)count; i++) { copy_region(srcBits, srcPitch, dstTop, dstStride, g_rects[i]); }
	}

	if (mapped) { g_ctx->Unmap(g_staging, 0); g_cntStaging++; }
	if (sysmapped) { g_dupl->UnMapDesktopSurface(); g_cntSysmem++; }
	QueryPerformanceCounter(&t1);
	frame->copyUs = (unsigned int)(((t1.QuadPart - t0.QuadPart) * 1000000LL) / (freq.QuadPart > 0 ? freq.QuadPart : 1));
	safe_release(tex);
	safe_release(res);
	g_frames++;
	return DXGI_CAP_OK;

fail:
	if (mapped) { g_ctx->Unmap(g_staging, 0); }
	if (sysmapped) { g_dupl->UnMapDesktopSurface(); }
	safe_release(tex);
	safe_release(res);
	dxgi_capture_shutdown();
	return result;
}

void dxgi_capture_set_precopy(dxgi_precopy_fn fn) { g_precopy = fn; }
void dxgi_capture_set_bbox_rects(int n) { g_bboxRects = (n < 1) ? 1 : n; }
int dxgi_capture_bbox_rects(void) { return g_bboxRects; }

void dxgi_capture_release(void)
{
	if (g_held && g_dupl != NULL)
	{
		g_releaseHr = g_dupl->ReleaseFrame();
		if (is_lost(g_releaseHr)) { g_releaseLost = 1; }
	}
	g_held = 0;
}

int dxgi_capture_sysmem(void) { return g_sysmem; }
HRESULT dxgi_capture_release_hr(void) { return g_releaseHr; }
void dxgi_capture_counters(long long *sysmemImages, long long *stagingImages, long long *bboxImages)
{
	if (sysmemImages != NULL) { *sysmemImages = g_cntSysmem; }
	if (stagingImages != NULL) { *stagingImages = g_cntStaging; }
	if (bboxImages != NULL) { *bboxImages = g_cntBbox; }
}

void dxgi_capture_shutdown(void)
{
	dxgi_capture_release();
	g_releaseLost = 0;
	safe_release(g_staging);
	safe_release(g_dupl);
	safe_release(g_ctx);
	safe_release(g_dev);
	free(g_metaMove); g_metaMove = NULL;
	free(g_metaDirty); g_metaDirty = NULL;
	g_metaCap = 0;
	free(g_rects); g_rects = NULL;
	g_rectsCap = 0;
	g_w = g_h = 0;
	g_frames = 0;
	g_sysmem = 0;
}

HRESULT dxgi_capture_last_hr(void) { return g_hr; }

const char *dxgi_result_name(dxgi_result r)
{
	switch (r)
	{
		case DXGI_CAP_OK: return "ok";
		case DXGI_CAP_NO_CHANGE: return "no_change";
		case DXGI_CAP_TIMEOUT: return "timeout";
		case DXGI_CAP_LOST: return "lost";
		case DXGI_CAP_ERROR: return "error";
		case DXGI_CAP_NO_OUTPUT: return "no_output";
		case DXGI_CAP_ROTATED: return "rotated";
		case DXGI_CAP_UNSUPPORTED: return "unsupported";
		case DXGI_CAP_TRANSIENT: return "transient";
	}
	return "?";
}

void kvm_mark_rect_tiles(unsigned char *map, int tilesX, int tilesY, int tileW, int tileH,
                         int l, int t, int r, int b, int srcW, int srcH, int dstW, int dstH)
{
	if (map == NULL || tilesX <= 0 || tilesY <= 0 || tileW <= 0 || tileH <= 0 || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) { return; }
	if (r <= l || b <= t) { return; }
	if (srcW != dstW || srcH != dstH)
	{
		// Scaled output: map the rectangle to destination pixels and add two pixels on every side. The scaling filters
		// (2:1, 4:3 and the area average) only read source pixels of their own destination pixel, so this covers all of them;
		// the surplus is removed by the tile CRC.
		l = (int)((long long)l * dstW / srcW) - 2;
		r = (int)(((long long)r * dstW + srcW - 1) / srcW) + 2;
		t = (int)((long long)t * dstH / srcH) - 2;
		b = (int)(((long long)b * dstH + srcH - 1) / srcH) + 2;
	}
	if (l < 0) { l = 0; }
	if (t < 0) { t = 0; }
	if (r > dstW) { r = dstW; }
	if (b > dstH) { b = dstH; }
	if (r <= l || b <= t) { return; }
	int tx0 = l / tileW, tx1 = (r - 1) / tileW, ty0 = t / tileH, ty1 = (b - 1) / tileH;
	if (tx1 > tilesX - 1) { tx1 = tilesX - 1; }
	if (ty1 > tilesY - 1) { ty1 = tilesY - 1; }
	for (int ty = ty0; ty <= ty1; ty++)
	{
		for (int tx = tx0; tx <= tx1; tx++) { map[(size_t)ty * tilesX + tx] = 1; }
	}
}

long long kvm_count_pixel_diff(const unsigned char *aTop, ptrdiff_t aStride, const unsigned char *bTop, ptrdiff_t bStride, int w, int h, const dxgi_rect *skip)
{
	long long diff = 0;
	for (int y = 0; y < h; y++)
	{
		const unsigned char *a = aTop + ((ptrdiff_t)y * aStride), *b = bTop + ((ptrdiff_t)y * bStride);
		int inSkipRows = (skip != NULL && y >= skip->top && y < skip->bottom);
		int x = 0;
		if (!inSkipRows)
		{
#ifdef DXGI_SSE2
			const __m128i mask = _mm_set1_epi32(0x00FFFFFF), zero = _mm_setzero_si128();
			for (; x + 4 <= w; x += 4)
			{
				__m128i d = _mm_and_si128(_mm_xor_si128(_mm_loadu_si128((const __m128i *)(a + ((size_t)x * 4))), _mm_loadu_si128((const __m128i *)(b + ((size_t)x * 4)))), mask);
				if (_mm_movemask_epi8(_mm_cmpeq_epi8(d, zero)) != 0xFFFF)
				{
					for (int k = 0; k < 4; k++)
					{
						const unsigned char *pa = a + ((size_t)(x + k) * 4), *pb = b + ((size_t)(x + k) * 4);
						if (pa[0] != pb[0] || pa[1] != pb[1] || pa[2] != pb[2]) { diff++; }
					}
				}
			}
#endif
		}
		for (; x < w; x++)
		{
			const unsigned char *pa = a + ((size_t)x * 4), *pb = b + ((size_t)x * 4);
			if (inSkipRows && x >= skip->left && x < skip->right) { continue; }
			if (pa[0] != pb[0] || pa[1] != pb[1] || pa[2] != pb[2]) { diff++; }
		}
	}
	return diff;
}

int kvm_cursor_xor(unsigned char *dstTop, ptrdiff_t dstStride, int dstW, int dstH, int x, int y, dxgi_rect *covered)
{
	CURSORINFO ci;
	ICONINFO ii;
	BITMAP bm;
	int drawn = 0;

	if (covered != NULL) { memset(covered, 0, sizeof(*covered)); }
	memset(&ci, 0, sizeof(ci));
	ci.cbSize = sizeof(ci);
	memset(&ii, 0, sizeof(ii));
	GetCursorInfo(&ci); // CURSOR_SHOWING is not checked, like the GDI path
	if (ci.hCursor == NULL || !GetIconInfo(ci.hCursor, &ii)) { return 0; }

	if (GetObject(ii.hbmMask, sizeof(bm), &bm) == sizeof(bm))
	{
		int cw = bm.bmWidth;
		int ch = ii.hbmColor ? bm.bmHeight : (bm.bmHeight / 2); // Monochrome cursors hold the AND and XOR masks in one bitmap
		if (cw > 0 && ch > 0 && cw <= 1024 && ch <= 1024)
		{
			HDC dc = CreateCompatibleDC(NULL);
			if (dc != NULL)
			{
				BITMAPINFO bi;
				void *bits = NULL;
				HBITMAP bmp;
				memset(&bi, 0, sizeof(bi));
				bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
				bi.bmiHeader.biWidth = cw;
				bi.bmiHeader.biHeight = -ch; // Top-down
				bi.bmiHeader.biPlanes = 1;
				bi.bmiHeader.biBitCount = 32;
				bi.bmiHeader.biCompression = BI_RGB;
				bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0); // Starts black
				if (bmp != NULL && bits != NULL)
				{
					HGDIOBJ old = SelectObject(dc, bmp);
					DrawIconEx(dc, 0, 0, ci.hCursor, cw, ch, 0, NULL, DI_NORMAL);
					GdiFlush();
					int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
					int x1 = (x + cw > dstW) ? dstW : x + cw, y1 = (y + ch > dstH) ? dstH : y + ch;
					for (int yy = y0; yy < y1; yy++)
					{
						const unsigned char *s = (const unsigned char *)bits + ((size_t)(yy - y) * cw * 4) + ((size_t)(x0 - x) * 4);
						unsigned char *d = dstTop + ((ptrdiff_t)yy * dstStride) + ((ptrdiff_t)x0 * 4);
						for (int xx = x0; xx < x1; xx++, s += 4, d += 4) { d[0] ^= s[0]; d[1] ^= s[1]; d[2] ^= s[2]; }
					}
					if (x1 > x0 && y1 > y0)
					{
						drawn = 1;
						if (covered != NULL) { covered->left = x0; covered->top = y0; covered->right = x1; covered->bottom = y1; }
					}
					SelectObject(dc, old);
				}
				if (bmp != NULL) { DeleteObject(bmp); }
				DeleteDC(dc);
			}
		}
	}
	if (ii.hbmMask != NULL) { DeleteObject(ii.hbmMask); }
	if (ii.hbmColor != NULL) { DeleteObject(ii.hbmColor); }
	return drawn;
}

int kvm_cursor_field(int margin, dxgi_rect *r, HCURSOR *hcur, POINT *pos)
{
	CURSORINFO ci;
	ICONINFO ii;
	BITMAP bm;
	int ok = 0;

	memset(r, 0, sizeof(*r));
	if (hcur != NULL) { *hcur = NULL; }
	if (pos != NULL) { pos->x = pos->y = 0; }
	memset(&ci, 0, sizeof(ci));
	ci.cbSize = sizeof(ci);
	memset(&ii, 0, sizeof(ii));
	if (!GetCursorInfo(&ci)) { return 0; }
	if (hcur != NULL) { *hcur = ci.hCursor; }
	if (pos != NULL) { *pos = ci.ptScreenPos; }
	if ((ci.flags & CURSOR_SHOWING) == 0 || ci.hCursor == NULL || !GetIconInfo(ci.hCursor, &ii)) { return 0; }
	if (GetObject(ii.hbmMask, sizeof(bm), &bm) == sizeof(bm))
	{
		int cw = bm.bmWidth, ch = ii.hbmColor ? bm.bmHeight : (bm.bmHeight / 2);
		if (cw > 0 && ch > 0 && cw <= 1024 && ch <= 1024)
		{
			r->left = ci.ptScreenPos.x - (int)ii.xHotspot - margin;
			r->top = ci.ptScreenPos.y - (int)ii.yHotspot - margin;
			r->right = r->left + cw + 2 * margin;
			r->bottom = r->top + ch + 2 * margin;
			ok = 1;
		}
	}
	if (ii.hbmMask != NULL) { DeleteObject(ii.hbmMask); }
	if (ii.hbmColor != NULL) { DeleteObject(ii.hbmColor); }
	return ok;
}

int kvm_cursor_sprite(HCURSOR hc, pm_sprite *sp)
{
	ICONINFO ii;
	BITMAP bm;
	int ok = 0;

	memset(sp, 0, sizeof(*sp));
	memset(&ii, 0, sizeof(ii));
	if (hc == NULL || !GetIconInfo(hc, &ii)) { return 0; }
	if (ii.hbmColor == NULL) { sp->monochrome = 1; sp->hc = hc; }
	else if (GetObject(ii.hbmMask, sizeof(bm), &bm) == sizeof(bm))
	{
		int cw = bm.bmWidth, ch = bm.bmHeight;
		if (cw > 0 && ch > 0 && cw <= 256 && ch <= 256)
		{
			HDC dc = CreateCompatibleDC(NULL);
			if (dc != NULL)
			{
				BITMAPINFO bi;
				void *bb = NULL, *wb = NULL;
				HBITMAP bmpB, bmpW;
				memset(&bi, 0, sizeof(bi));
				bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
				bi.bmiHeader.biWidth = cw;
				bi.bmiHeader.biHeight = -ch; // top-down
				bi.bmiHeader.biPlanes = 1;
				bi.bmiHeader.biBitCount = 32;
				bi.bmiHeader.biCompression = BI_RGB;
				bmpB = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bb, NULL, 0);
				bmpW = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &wb, NULL, 0);
				if (bmpB != NULL && bmpW != NULL && bb != NULL && wb != NULL)
				{
					// The cursor drawn over black and over white: where both are equal the pixel is fully opaque
					HGDIOBJ old = SelectObject(dc, bmpB);
					memset(bb, 0x00, (size_t)cw * ch * 4);
					DrawIconEx(dc, 0, 0, hc, cw, ch, 0, NULL, DI_NORMAL);
					SelectObject(dc, bmpW);
					memset(wb, 0xFF, (size_t)cw * ch * 4);
					DrawIconEx(dc, 0, 0, hc, cw, ch, 0, NULL, DI_NORMAL);
					GdiFlush();
					SelectObject(dc, old);
					sp->aff = (unsigned char *)calloc((size_t)cw * ch, 1);
					sp->x = (int *)malloc(sizeof(int) * (size_t)cw * ch);
					sp->y = (int *)malloc(sizeof(int) * (size_t)cw * ch);
					sp->rgb = (unsigned char *)malloc((size_t)cw * ch * 3);
					if (sp->x != NULL && sp->y != NULL && sp->rgb != NULL && sp->aff != NULL)
					{
						const unsigned char *pb = (const unsigned char *)bb, *pw = (const unsigned char *)wb;
						int n = 0;
						for (int y = 0; y < ch; y++)
						{
							for (int x = 0; x < cw; x++)
							{
								size_t o = ((size_t)y * cw + x) * 4;
								// alpha > 0 where the cursor changes the pixel: over black and over white the pixel differs from the background in some channel
								if (pb[o] != 0 || pb[o + 1] != 0 || pb[o + 2] != 0 || pw[o] != 0xFF || pw[o + 1] != 0xFF || pw[o + 2] != 0xFF) { sp->aff[(size_t)y * cw + x] = 1; }
								if (pb[o] == pw[o] && pb[o + 1] == pw[o + 1] && pb[o + 2] == pw[o + 2])
								{
									sp->x[n] = x; sp->y[n] = y; memcpy(sp->rgb + (size_t)n * 3, pb + o, 3);
									n++;
								}
							}
						}
						sp->n = n; sp->w = cw; sp->h = ch; sp->hx = (int)ii.xHotspot; sp->hy = (int)ii.yHotspot; sp->hc = hc;
						if (n >= 8) { pm_sprite_spread(sp); ok = 1; } else { pm_sprite_free(sp); }
					}
					else { pm_sprite_free(sp); }
				}
				if (bmpB != NULL) { DeleteObject(bmpB); }
				if (bmpW != NULL) { DeleteObject(bmpW); }
				DeleteDC(dc);
			}
		}
	}
	if (ii.hbmMask != NULL) { DeleteObject(ii.hbmMask); }
	if (ii.hbmColor != NULL) { DeleteObject(ii.hbmColor); }
	return ok;
}

} // extern "C"

#endif // KVM_DXGI_ENABLED
