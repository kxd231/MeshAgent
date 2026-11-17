// KVM (Windows): the hardware H.264 encoder (a hardware Media Foundation transform) as an encoder backend, see kvm_encbe_mfhw.h for what it does and why.
// The shared parts (loading of Media Foundation, media types, level, codec API setters) are in mf_h264.cpp (mf_h264_priv.h); the GUIDs of the Media Foundation headers are defined there.
// This file must not include microstack or agent headers.

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <strmif.h>
#include <codecapi.h>
#include <dxgi.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "mf_h264.h"
#include "mf_h264_priv.h"
#include "kvm_encbe_mfhw.h"
#include "../common/kvm_yuv.h"
#include "../common/kvm_annexb.h"

#ifdef KVM_HW_TEST_LOCAL
#define MFHW_ENUM_LOCAL MFT_ENUM_FLAG_LOCALMFT	// the Wine test (a mock encoder registered with MFTRegisterLocal): local MFTs count as hardware
#else
#define MFHW_ENUM_LOCAL 0
#endif

#define MFHW_RING 4				// input samples in rotation (the software encoder has 3): a hardware encoder may hold an input longer; the self test checks that it does not
#define MFHW_MAX_PARTS 8
#define MFHW_EVQ 128

// Interface ids and attributes as values (the MinGW headers lack some, the MSVC ones have them for __uuidof only): no clash with the names of the headers
static const GUID kHw_IID_ICodecAPI = { 0x901db4c7, 0x31ce, 0x41a2, { 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda } };
static const GUID kHw_IID_IMFTransform = { 0xbf94c121, 0x5b05, 0x4e6f, { 0x80, 0x00, 0xba, 0x59, 0x89, 0x61, 0x41, 0x4d } };
static const GUID kHw_IID_IMFMediaEventGenerator = { 0x2cd0bd52, 0xbcd5, 0x4b89, { 0xb6, 0x2c, 0xea, 0xdc, 0x0c, 0x03, 0x1e, 0x7d } };
static const GUID kHw_IID_IDXGIFactory1 = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
static const GUID kHw_IID_IDXGIDevice = { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };
static const GUID kHw_MFT_ENUM_ADAPTER_LUID = { 0x1d39518c, 0xe220, 0x4da8, { 0xa0, 0x7f, 0xba, 0x17, 0x25, 0x52, 0xd6, 0xb1 } };

static const GUID kHw_MFT_ENUM_HARDWARE_VENDOR_ID = { 0x3aecb0cc, 0x035b, 0x4bcc, { 0x81, 0x85, 0x2b, 0x8d, 0x55, 0x1e, 0xf3, 0xaf } };

typedef HRESULT(WINAPI *PFN_MFCreateAttributes)(IMFAttributes **, UINT32);
typedef HRESULT(WINAPI *PFN_MFTEnumEx)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO *, const MFT_REGISTER_TYPE_INFO *, IMFActivate ***, UINT32 *);
typedef HRESULT(WINAPI *PFN_MFTEnum2)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO *, const MFT_REGISTER_TYPE_INFO *, IMFAttributes *, IMFActivate ***, UINT32 *);
typedef void(WINAPI *PFN_CoTaskMemFree)(LPVOID);
typedef HRESULT(WINAPI *PFN_CreateDXGIFactory1)(REFIID, void **);

static PFN_MFCreateAttributes p_MFCreateAttributes = NULL;
static PFN_MFTEnumEx p_MFTEnumEx = NULL;
static PFN_MFTEnum2 p_MFTEnum2 = NULL;
static PFN_CoTaskMemFree p_CoTaskMemFree = NULL;
static PFN_CreateDXGIFactory1 p_CreateDXGIFactory1 = NULL;
static volatile LONG g_hwFaulted = 0;			// a structured exception in the hardware backend: it is not called again in this process
static unsigned g_inMs = KVM_MFHW_IN_MS, g_outMs = KVM_MFHW_OUT_MS, g_openMs = KVM_MFHW_OPEN_MS;

void kvm_mfhw_test_set_deadlines(unsigned inMs, unsigned outMs, unsigned openMs)
{
	if (inMs != 0) { g_inMs = inMs; }
	if (outMs != 0) { g_outMs = outMs; }
	if (openMs != 0) { g_openMs = openMs; }
}

int kvm_mfhw_faulted(void) { return g_hwFaulted != 0; }

template <class T> static void rel(T *&p)
{
	if (p != NULL) { p->Release(); p = NULL; }
}

static double qpc_ms(void)
{
	LARGE_INTEGER f, t;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t);
	return f.QuadPart > 0 ? (double)t.QuadPart * 1000.0 / (double)f.QuadPart : 0.0;
}

// ------------------------------------------------------------------------------------------------------------------ the events of the MFT

// A thread that waits in IMFMediaEventGenerator::GetEvent (it blocks until an event comes or the MFT is shut down) and puts what it gets into a small queue with a Win32 event, so that the encoder thread can wait
// with a deadline. Two owners (the encoder and the thread): the thread of an MFT that does not shut down is left behind with the object and nobody waits for it.
class EventPump
{
public:
	struct Ev { DWORD type; HRESULT status; };
	EventPump() : m_thread(NULL), m_gen(NULL), m_stop(0), m_ended(0), m_refs(2), m_head(0), m_n(0), m_dropped(0)
	{
		m_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
		InitializeCriticalSection(&m_lock);
	}
	~EventPump() { if (m_ev != NULL) { CloseHandle(m_ev); } DeleteCriticalSection(&m_lock); }
	bool start(IMFMediaEventGenerator *g)
	{
		m_gen = g;
		g->AddRef();
		m_thread = CreateThread(NULL, 0, proc, this, 0, NULL);
		if (m_thread == NULL) { rel(m_gen); }
		return m_thread != NULL;
	}
	bool wait(DWORD ms) { return WaitForSingleObject(m_ev, ms) == WAIT_OBJECT_0; }
	bool pop(Ev *e)
	{
		EnterCriticalSection(&m_lock);
		const bool have = m_n > 0;
		if (have) { *e = m_q[m_head]; m_head = (m_head + 1) % MFHW_EVQ; m_n--; }
		LeaveCriticalSection(&m_lock);
		return have;
	}
	// The events of before a flush are of no use (a NeedInput for an input that the flush threw away): dropped. An error and the end of the queue stay: they say that the encoder is dead, flush or no flush
	// (an MEError that the flush itself brought must not be lost). Returns how many were dropped.
	int clear()
	{
		Ev keep[MFHW_EVQ];
		int nk = 0, dropped = 0;
		EnterCriticalSection(&m_lock);
		for (int i = 0; i < m_n; i++)
		{
			const Ev &e = m_q[(m_head + i) % MFHW_EVQ];
			if (e.type == MEError || e.type == 0xFFFF) { keep[nk++] = e; } else { dropped++; }
		}
		for (int i = 0; i < nk; i++) { m_q[i] = keep[i]; }
		m_head = 0; m_n = nk;
		LeaveCriticalSection(&m_lock);
		return dropped;
	}
	int dropped() const { return m_dropped; }
	bool ended() const { return m_ended != 0; }
	// The encoder lets go (after IMFActivate::ShutdownObject: GetEvent then returns an error and the thread ends): waits a moment, a thread that is still blocked ends itself with the object later.
	void release_by_encoder(DWORD ms)
	{
		m_stop = 1;
		if (m_thread != NULL) { WaitForSingleObject(m_thread, ms); CloseHandle(m_thread); m_thread = NULL; }
		if (InterlockedDecrement(&m_refs) == 0) { delete this; }
	}
private:
	static DWORD WINAPI proc(LPVOID p)
	{
		EventPump *self = (EventPump *)p;
		self->loop();
		if (InterlockedDecrement(&self->m_refs) == 0) { delete self; }
		return 0;
	}
	void push(const Ev &ev)
	{
		EnterCriticalSection(&m_lock);
		if (m_n >= MFHW_EVQ) { m_head = (m_head + 1) % MFHW_EVQ; m_n--; m_dropped++; }	// the oldest goes (an encoder that is not listened to for 128 events is not a working one anyway)
		m_q[(m_head + m_n) % MFHW_EVQ] = ev; m_n++;
		LeaveCriticalSection(&m_lock);
		SetEvent(m_ev);
	}
	void loop()
	{
		if (p_CoInitializeEx != NULL) { p_CoInitializeEx(NULL, COINIT_MULTITHREADED); }
		while (!m_stop)
		{
			IMFMediaEvent *e = NULL;
			HRESULT hr = m_gen->GetEvent(0, &e);
			Ev ev;
			ev.type = 0xFFFF; ev.status = hr;
			if (SUCCEEDED(hr) && e != NULL)
			{
				MediaEventType t = 0;
				HRESULT st = S_OK;
				e->GetType(&t);
				e->GetStatus(&st);
				ev.type = (DWORD)t; ev.status = st;
				e->Release();
			}
			push(ev);
			if (FAILED(hr)) { break; }
		}
		m_ended = 1;
		SetEvent(m_ev);
		rel(m_gen);
	}
	HANDLE m_thread, m_ev;
	IMFMediaEventGenerator *m_gen;
	volatile LONG m_stop, m_ended, m_refs;
	CRITICAL_SECTION m_lock;
	Ev m_q[MFHW_EVQ];
	int m_head, m_n, m_dropped;
};

// ------------------------------------------------------------------------------------------------------------------ the handle

struct mfhw
{
	int gopPass;				// see mfh264_info.gop_pass
	IMFActivate *act;
	IMFTransform *mft;
	ICodecAPI *api;
	IMFMediaEventGenerator *gen;
	EventPump *pump;
	IMFSample *inSample[MFHW_RING];
	IMFMediaBuffer *inBuf[MFHW_RING];
	int inIdx;
	IMFSample *outSample;		// only for an MFT that does not provide its own output samples
	IMFMediaBuffer *outBuf;
	DWORD outCap;
	int outProvides;
	LONGLONG frameDur;
	mfh264_cfg cfg;
	uint8_t *seqHdr;
	size_t seqHdrLen;
	uint8_t *raw;
	size_t rawCap, rawLen;
	int needInput;				// the MFT asked for an input (a credit: depth 1 needs no count)
	int pendHave;				// HaveOutput events that were not answered with a ProcessOutput yet
	int expectIdr;
	int dead;					// a deadline passed: never call the MFT again
	int curQp;
	int outQpHave;
	ULONGLONG outQpRaw;
	unsigned initCredits;
	mfh264_stats st;
	mfh264_info info;
};

static mfhw *H(struct mfh264 *h) { return reinterpret_cast<mfhw *>(h); }
static const mfhw *H(const struct mfh264 *h) { return reinterpret_cast<const mfhw *>(h); }

static int start_code_len(const uint8_t *p, size_t left)
{
	if (left >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) { return 4; }
	if (left >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) { return 3; }
	return 0;
}

static void copy_str(char *dst, size_t cap, const char *src) { size_t n = strlen(src); if (n >= cap) { n = cap - 1; } memcpy(dst, src, n); dst[n] = 0; }
static void wide_to_ascii(char *dst, size_t cap, const wchar_t *w)
{
	size_t i = 0;
	for (; w != NULL && w[i] != 0 && i + 1 < cap; i++) { dst[i] = (w[i] > 31 && w[i] < 127) ? (char)w[i] : '_'; }
	dst[i] = 0;
}

// ------------------------------------------------------------------------------------------------------------------ selection of the encoder

struct AdapterInfo { unsigned vendor, device, subsys, rev; int64_t luid; char driver[40]; bool software; };
#define MFHW_MAX_ADAPTERS 8

static int64_t luid_pack(const LUID &l) { return ((int64_t)l.HighPart << 32) | (int64_t)(uint32_t)l.LowPart; }

static int enum_adapters(AdapterInfo *out, int cap)
{
	int n = 0;
	HMODULE dx = mfp_load_system_dll(L"dxgi.dll");
	if (dx == NULL) { return 0; }
	if (p_CreateDXGIFactory1 == NULL) { p_CreateDXGIFactory1 = (PFN_CreateDXGIFactory1)GetProcAddress(dx, "CreateDXGIFactory1"); }
	if (p_CreateDXGIFactory1 == NULL) { return 0; }
	IDXGIFactory1 *fac = NULL;
	if (FAILED(p_CreateDXGIFactory1(kHw_IID_IDXGIFactory1, (void **)&fac)) || fac == NULL) { return 0; }
	for (UINT i = 0; n < cap; i++)
	{
		IDXGIAdapter1 *a = NULL;
		if (fac->EnumAdapters1(i, &a) != S_OK || a == NULL) { break; }
		DXGI_ADAPTER_DESC1 d;
		memset(&d, 0, sizeof(d));
		if (SUCCEEDED(a->GetDesc1(&d)))
		{
			AdapterInfo &x = out[n];
			memset(&x, 0, sizeof(x));
			x.vendor = d.VendorId; x.device = d.DeviceId; x.subsys = d.SubSysId; x.rev = d.Revision; x.luid = luid_pack(d.AdapterLuid); x.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
			LARGE_INTEGER ver;
			memset(&ver, 0, sizeof(ver));
			if (SUCCEEDED(a->CheckInterfaceSupport(kHw_IID_IDXGIDevice, &ver)))
			{
				_snprintf(x.driver, sizeof(x.driver), "%u.%u.%u.%u", (unsigned)(ver.HighPart >> 16), (unsigned)(ver.HighPart & 0xFFFF), (unsigned)(ver.LowPart >> 16), (unsigned)(ver.LowPart & 0xFFFF));
				x.driver[sizeof(x.driver) - 1] = 0;
			}
			n++;
		}
		a->Release();
	}
	fac->Release();
	return n;
}

static void free_acts(IMFActivate **acts, UINT32 n)
{
	if (acts == NULL) { return; }
	for (UINT32 i = 0; i < n; i++) { if (acts[i] != NULL) { acts[i]->Release(); } }
	if (p_CoTaskMemFree != NULL) { p_CoTaskMemFree(acts); }
}

// The first hardware encoder (NV12 in, H.264 out): that of the adapter `luid` when MFTEnum2 knows it, else the first of MFTEnumEx. *act is a new reference. *onLuid: it is the one of the adapter that was asked for.
static HRESULT enum_pick(int64_t luid, IMFActivate **act, int *onLuid, int *nHw)
{
	MFT_REGISTER_TYPE_INFO in, out;
	IMFActivate **acts = NULL;
	UINT32 n = 0;
	const UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER | MFHW_ENUM_LOCAL;
	HRESULT hr = E_FAIL;
	in.guidMajorType = MFMediaType_Video; in.guidSubtype = MFVideoFormat_NV12;
	out.guidMajorType = MFMediaType_Video; out.guidSubtype = MFVideoFormat_H264;
	*act = NULL; *onLuid = 0; if (nHw != NULL) { *nHw = 0; }
	if (p_MFTEnumEx == NULL) { return E_NOTIMPL; }
	if (luid != 0 && p_MFTEnum2 != NULL && p_MFCreateAttributes != NULL)
	{
		IMFAttributes *at = NULL;
		LUID l;
		l.LowPart = (DWORD)(uint32_t)(luid & 0xFFFFFFFF); l.HighPart = (LONG)(luid >> 32);
		if (SUCCEEDED(p_MFCreateAttributes(&at, 1)) && at != NULL && SUCCEEDED(at->SetBlob(kHw_MFT_ENUM_ADAPTER_LUID, (const UINT8 *)&l, sizeof(l))))
		{
			hr = p_MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, flags, &in, &out, at, &acts, &n);
			if (SUCCEEDED(hr) && n > 0 && acts != NULL) { *act = acts[0]; acts[0] = NULL; *onLuid = 1; free_acts(acts, n); rel(at); return S_OK; }
		}
		rel(at);
		free_acts(acts, n);
		acts = NULL; n = 0;
	}
	hr = p_MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &in, &out, &acts, &n);
	if (FAILED(hr)) { free_acts(acts, n); return hr; }
	if (nHw != NULL) { *nHw = (int)n; }
	if (n == 0 || acts == NULL) { free_acts(acts, n); return MF_E_TOPO_CODEC_NOT_FOUND; }
	*act = acts[0]; acts[0] = NULL;
	free_acts(acts, n);
	return S_OK;
}

static void guid_str(const GUID &g, char *out, size_t cap)
{
	_snprintf(out, cap, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", (unsigned long)g.Data1, (unsigned)g.Data2, (unsigned)g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
	if (cap > 0) { out[cap - 1] = 0; }
}

static int hw_load_funcs(void);

static int select_impl(int64_t luid, kvm_mfhw_sel *out)
{
	IMFActivate *act = NULL;
	int onLuid = 0, nHw = 0;
	AdapterInfo ad[MFHW_MAX_ADAPTERS];
	int nAd;
	memset(out, 0, sizeof(*out));
	if (!hw_load_funcs()) { return 0; }
	if (enum_pick(luid, &act, &onLuid, &nHw) != S_OK || act == NULL) { return 0; }
	out->found = 1; out->onLuid = onLuid; out->nHw = nHw;
	{
		WCHAR name[128];
		UINT32 got = 0;
		name[0] = 0;
		if (SUCCEEDED(act->GetString(MFT_FRIENDLY_NAME_Attribute, name, 127, &got))) { name[127] = 0; wide_to_ascii(out->name, sizeof(out->name), name); }
		GUID g;
		if (SUCCEEDED(act->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &g))) { guid_str(g, out->clsid, sizeof(out->clsid)); }
		// the adapter: the one asked for (MFTEnum2 gave its encoder), else the one that the activation says (LUID attribute), else the first one of the vendor in the registration ("VEN_1002")
		int64_t al = onLuid ? luid : 0;
		LUID l;
		UINT32 bl = 0;
		if (al == 0 && SUCCEEDED(act->GetBlob(kHw_MFT_ENUM_ADAPTER_LUID, (UINT8 *)&l, sizeof(l), &bl)) && bl == sizeof(l)) { al = luid_pack(l); }
		nAd = enum_adapters(ad, MFHW_MAX_ADAPTERS);
		int pick = -1;
		for (int i = 0; i < nAd && al != 0; i++) { if (ad[i].luid == al) { pick = i; break; } }
		if (pick < 0)
		{
			WCHAR ven[64];
			unsigned v = 0;
			ven[0] = 0;
			if (SUCCEEDED(act->GetString(kHw_MFT_ENUM_HARDWARE_VENDOR_ID, ven, 63, &got))) { ven[63] = 0; if (ven[0] == L'V' && ven[1] == L'E' && ven[2] == L'N' && ven[3] == L'_') { v = (unsigned)wcstoul(ven + 4, NULL, 16); } }
			for (int i = 0; i < nAd; i++) { if (!ad[i].software && (v == 0 || ad[i].vendor == v)) { pick = i; break; } }
		}
		if (pick >= 0)
		{
			out->vendor = ad[pick].vendor; out->device = ad[pick].device; out->subsys = ad[pick].subsys; out->adapterRev = ad[pick].rev; out->luid = ad[pick].luid;
			copy_str(out->driver, sizeof(out->driver), ad[pick].driver);
		}
	}
	act->Release();
	return 1;
}

// ------------------------------------------------------------------------------------------------------------------ waiting for events

// Waits (at most ms) until the MFT has asked for an input (wantNeed) or has an output (wantHave). 1 = it has, 0 = the deadline passed, -1 = an error event or the queue ended (*hr says which).
static int hw_wait(mfhw *h, DWORD ms, int wantNeed, int wantHave, HRESULT *hr)
{
	const double t0 = qpc_ms();
	for (;;)
	{
		EventPump::Ev e;
		while (h->pump->pop(&e))
		{
			if (e.type == METransformNeedInput) { h->needInput = 1; h->st.frames_in += 0; h->info.events_need++; }
			else if (e.type == METransformHaveOutput) { h->pendHave++; h->info.events_have++; }
			else if (e.type == MEError) { *hr = FAILED(e.status) ? e.status : E_FAIL; return -1; }
			else if (e.type == 0xFFFF) { *hr = FAILED(e.status) ? e.status : E_FAIL; return -1; }	// the queue ended (the MFT was shut down, or GetEvent failed)
			else { h->info.events_other++; }
		}
		if ((wantNeed && h->needInput) || (wantHave && h->pendHave > 0)) { return 1; }
		const double left = (double)ms - (qpc_ms() - t0);
		if (left <= 0.0) { return 0; }
		h->pump->wait((DWORD)(left < 1.0 ? 1.0 : left));
	}
}

// ------------------------------------------------------------------------------------------------------------------ open

static void destroy_hw(mfhw *h, int leak)
{
	if (h == NULL) { return; }
	if (leak) { return; }	// the encoder may be blocked in the driver: nothing is released (the memory, the objects and the thread of the events stay)
	if (h->mft != NULL)
	{
		h->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
		h->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
	}
	if (h->act != NULL) { h->act->ShutdownObject(); }		// the thread of the events ends when GetEvent fails
	if (h->pump != NULL) { h->pump->release_by_encoder(500); h->pump = NULL; }
	for (int i = 0; i < MFHW_RING; i++) { rel(h->inBuf[i]); rel(h->inSample[i]); }
	rel(h->outBuf); rel(h->outSample);
	rel(h->gen); rel(h->api); rel(h->mft); rel(h->act);
	free(h->seqHdr);
	free(h->raw);
	free(h);
}

static void refresh_seq_header(mfhw *h)
{
	IMFMediaType *t = NULL;
	if (FAILED(h->mft->GetOutputCurrentType(0, &t)) || t == NULL) { return; }
	UINT32 sz = 0;
	if (SUCCEEDED(t->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &sz)) && sz > 4 && sz < 4096)
	{
		uint8_t *b = (uint8_t *)malloc(sz);
		UINT32 got = 0;
		if (b != NULL && SUCCEEDED(t->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, b, sz, &got)) && got == sz && start_code_len(b, sz) != 0) { free(h->seqHdr); h->seqHdr = b; h->seqHdrLen = sz; }
		else { free(b); }
	}
	t->Release();
	h->info.has_seq_header = (h->seqHdr != NULL);
	h->info.seq_header_len = (int)h->seqHdrLen;
	if (h->seqHdr != NULL)
	{
		int p = 0, l = 0;
		if (kvm_annexb_sps_ids(h->seqHdr, h->seqHdrLen, &p, &l) == 0) { h->info.sps_profile_idc = p; h->info.sps_level_idc = l; }	// (the sequence header of the AMD MFT says level 42, its IDR 40: the IDR is what counts)
	}
}

static int setup_output(mfhw *h, struct mfh264_err *err)
{
	MFT_OUTPUT_STREAM_INFO si;
	memset(&si, 0, sizeof(si));
	HRESULT hr = h->mft->GetOutputStreamInfo(0, &si);
	if (FAILED(hr)) { return mfp_fail(err, MFH264_E_START, hr, "GetOutputStreamInfo"); }
	h->outProvides = (si.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) ? 1 : 0;
	h->info.out_provides_samples = h->outProvides;
	h->info.out_cb_size = (int)si.cbSize;
	if (!h->outProvides)
	{
		size_t want = (size_t)h->cfg.width * (size_t)h->cfg.height * 3 / 2 + 65536;
		if (si.cbSize > want) { want = si.cbSize; }
		if (h->outSample == NULL || want > h->outCap)
		{
			rel(h->outBuf); rel(h->outSample);
			if (FAILED(p_MFCreateSample(&h->outSample)) || FAILED(p_MFCreateMemoryBuffer((DWORD)want, &h->outBuf)) || FAILED(h->outSample->AddBuffer(h->outBuf)))
			{
				rel(h->outBuf); rel(h->outSample);
				return mfp_fail(err, MFH264_E_ALLOC, E_OUTOFMEMORY, "output sample");
			}
			h->outCap = (DWORD)want;
		}
	}
	return MFH264_OK;
}

// The functions that the enumeration needs (loaded at run time, nothing in the import table). Called by hw_init and by kvm_mfhw_select, which runs on the thread of the SOFTWARE backend as well (Media Foundation and
// COM are initialised there by its init: the selection only asks, it does not need the hardware backend to be the one that runs). 1 = all there.
static int hw_load_funcs(void)
{
	if (p_MFTEnumEx == NULL || p_CoTaskMemFree == NULL || p_MFCreateAttributes == NULL)
	{
		HMODULE mf = GetModuleHandleW(L"mfplat.dll"), ole = GetModuleHandleW(L"ole32.dll");
		if (mf == NULL) { mf = mfp_load_system_dll(L"mfplat.dll"); }
		if (ole == NULL) { ole = mfp_load_system_dll(L"ole32.dll"); }
		if (mf != NULL)
		{
			p_MFCreateAttributes = (PFN_MFCreateAttributes)GetProcAddress(mf, "MFCreateAttributes");
			p_MFTEnumEx = (PFN_MFTEnumEx)GetProcAddress(mf, "MFTEnumEx");
			p_MFTEnum2 = (PFN_MFTEnum2)GetProcAddress(mf, "MFTEnum2");			// Windows 10 and later: not required
		}
		if (ole != NULL) { p_CoTaskMemFree = (PFN_CoTaskMemFree)GetProcAddress(ole, "CoTaskMemFree"); }
	}
	return (p_MFTEnumEx != NULL && p_CoTaskMemFree != NULL && p_MFCreateAttributes != NULL) ? 1 : 0;
}

static int hw_init_impl(struct mfh264_err *err)
{
	int r = mfp_init_impl(err);
	if (r != MFH264_OK) { return r; }
	if (!hw_load_funcs()) { mfp_shutdown_impl(); return mfp_fail(err, MFH264_E_NOMF, S_OK, "MFTEnumEx / CoTaskMemFree / MFCreateAttributes"); }
	return MFH264_OK;
}

static int hw_qp_form_ok(int m) { return m == MFH264_QP_ENCODEQP_UI4 || m == MFH264_QP_FRAMETYPEQP || m == MFH264_QP_QUALITY; }

// the settings of the codec API in one place, called in two orders. NVIDIA keeps the GOP of 30 (SetValue(0xFFFFFFFF) says S_OK and GetValue reads 30) when it is set after the types: every candidate is read back.
static int api_read_u32(ICodecAPI *a, const GUID &g, ULONG *out)
{
	VARIANT v;
	memset(&v, 0, sizeof(v));
	if (FAILED(a->GetValue(&g, &v)) || (v.vt != VT_UI4 && v.vt != VT_I4)) { return 0; }
	*out = v.ulVal;
	return 1;
}

static void tr_add(char *tr, size_t cap, const char *what, unsigned long a, long b, long long c)
{
	const size_t n = strlen(tr);
	if (n + 2 < cap) { snprintf(tr + n, cap - n, "%s%lx:%08lx:%lld", what, a, (unsigned long)b, c); }
}

// the low latency mode, no B pictures, one reference, the GOP (the first candidate that is read back as it was set; else the last one that was accepted), the rate control mode, the bit rate. 0 = fine
static int hw_apply_api(mfhw *h, struct mfh264_err *err)
{
	char *tr = h->info.gop_trace;
	const size_t cap = sizeof(h->info.gop_trace);
	ULONG cand[3];
	int nc = 0;
	h->info.low_latency_hr = (long)mfp_api_set_bool(h->api, CODECAPI_AVLowLatencyMode, 1); h->info.low_latency_ok = SUCCEEDED(h->info.low_latency_hr);
	h->info.bframes_hr = (long)mfp_api_set_u32(h->api, CODECAPI_AVEncMPVDefaultBPictureCount, 0); h->info.bframes_ok = SUCCEEDED(h->info.bframes_hr);
	h->info.maxref_hr = (long)mfp_api_set_u32(h->api, CODECAPI_AVEncVideoMaxNumRefFrame, 1); h->info.maxref_ok = SUCCEEDED(h->info.maxref_hr);
	if (h->cfg.gop_only != 0) { cand[nc++] = h->cfg.gop_only; }
	else if (h->cfg.gop > 0) { cand[nc++] = (ULONG)h->cfg.gop; }
	else
	{
		VARIANT vmin, vmax, vstep;
		memset(&vmin, 0, sizeof(vmin)); memset(&vmax, 0, sizeof(vmax)); memset(&vstep, 0, sizeof(vstep));
		if (SUCCEEDED(h->api->GetParameterRange(&CODECAPI_AVEncMPVGOPSize, &vmin, &vmax, &vstep)) && (vmax.vt == VT_UI4 || vmax.vt == VT_I4) && vmax.ulVal > 0) { cand[nc++] = vmax.ulVal; }
		cand[nc++] = 0x7FFFFFFFul;
		cand[nc++] = 0xFFFFFFFFul;
	}
	h->info.gop_pass = 0;
	for (int k = 0; k < nc && !h->info.gop_pass; k++)
	{
		ULONG rd = 0;
		const HRESULT hr = mfp_api_set_u32(h->api, CODECAPI_AVEncMPVGOPSize, cand[k]);
		h->info.gop_hr = (long)hr;
		if (FAILED(hr)) { tr_add(tr, cap, k ? "," : " try=", cand[k], (long)hr, -2); continue; }
		const int got = api_read_u32(h->api, CODECAPI_AVEncMPVGOPSize, &rd);
		h->info.gop_ok = 1; h->info.gop_value = (int64_t)cand[k];
		tr_add(tr, cap, k ? "," : " try=", cand[k], (long)hr, got ? (long long)rd : -1);
		if (!got || rd == cand[k]) { h->info.gop_pass = 1; }
	}
	{
		const ULONG mode = (h->cfg.rc == MFH264_RC_CBR) ? 0u : (h->cfg.rc == MFH264_RC_LDVBR ? 4u : 3u);	// CBR, LowDelayVBR, Quality
		h->info.rc_mode_hr = (long)mfp_api_set_u32(h->api, CODECAPI_AVEncCommonRateControlMode, mode);
		h->info.rc_mode_ok = SUCCEEDED(h->info.rc_mode_hr);
		if (!h->info.rc_mode_ok) { return mfp_fail(err, MFH264_E_CODECAPI, (HRESULT)h->info.rc_mode_hr, "AVEncCommonRateControlMode"); }
		{ const size_t n = strlen(tr); if (n + 2 < cap) { snprintf(tr + n, cap - n, " rc set=%lu", mode); } }
		if (h->cfg.rc != MFH264_RC_QUALITY)
		{
			mfp_api_set_u32(h->api, CODECAPI_AVEncCommonMeanBitRate, (ULONG)h->cfg.bitrate);
			// no BufferSize (the probe that worked on NVIDIA did not set it; NVIDIA ignores it anyway); only the diagnostics of the self test ask for one (cfg.vbv)
			if (h->cfg.vbv > 0) { mfp_api_set_u32(h->api, CODECAPI_AVEncCommonBufferSize, (ULONG)(h->cfg.bitrate / h->cfg.vbv)); }
		}
	}
	h->info.qvs_ok = 1; h->info.workers_ok = 1;
	return MFH264_OK;
}

// after the types and the settings: what the encoder says its GOP and its mode are now
static void hw_read_back(mfhw *h)
{
	ULONG v = 0;
	char *tr = h->info.gop_trace;
	const size_t cap = sizeof(h->info.gop_trace);
	const size_t n = strlen(tr);
	h->info.gop_read = api_read_u32(h->api, CODECAPI_AVEncMPVGOPSize, &v) ? (int64_t)v : -1;
	h->info.rc_read = api_read_u32(h->api, CODECAPI_AVEncCommonRateControlMode, &v) ? (int)v : -1;
	if (n + 2 < cap) { snprintf(tr + n, cap - n, " read=%lld rc_read=%d", (long long)h->info.gop_read, h->info.rc_read); }
	h->gopPass = h->info.gop_pass;
}

static struct mfh264 *hw_open_core(const struct mfh264_cfg *cfg, struct mfh264_err *err, int order)
{
	int r;
	if (!mfp_check_cfg(cfg)) { mfp_fail(err, MFH264_E_ARG, S_OK, "open args"); return NULL; }
	if (cfg->rc != MFH264_RC_QUALITY && cfg->rc != MFH264_RC_CBR && cfg->rc != MFH264_RC_LDVBR) { mfp_fail(err, MFH264_E_ARG, S_OK, "hardware encoder: rate control mode"); return NULL; }
	if (cfg->rc == MFH264_RC_QUALITY && !hw_qp_form_ok(cfg->qp_method)) { mfp_fail(err, MFH264_E_ARG, S_OK, "hardware encoder: the form of the QP (UI4, FrameTypeQP or Quality; not the packed UI8)"); return NULL; }
	if (!mfp_os_ok()) { mfp_fail(err, MFH264_E_OS, S_OK, "os"); return NULL; }
	if (p_MFTEnumEx == NULL) { mfp_fail(err, MFH264_E_NOINIT, S_OK, "open (hw init was not called on this thread)"); return NULL; }
	const int level = mfp_level_for_cfg(cfg, err);
	if (level == 0) { return NULL; }
	mfhw *h = (mfhw *)calloc(1, sizeof(mfhw));
	if (h == NULL) { mfp_fail(err, MFH264_E_ALLOC, E_OUTOFMEMORY, "calloc"); return NULL; }
	const double t0 = qpc_ms();
	h->cfg = *cfg;
	h->frameDur = (LONGLONG)10000000 / MFH264_NOMINAL_FPS;
	h->expectIdr = 1;
	h->info.backend = 1;
	h->info.width = cfg->width; h->info.height = cfg->height; h->info.profile_requested = cfg->profile; h->info.level_requested = level;

	int onLuid = 0;
	HRESULT hr = enum_pick(cfg->hw_luid, &h->act, &onLuid, NULL);
	if (FAILED(hr) || h->act == NULL) { destroy_hw(h, 0); mfp_fail(err, MFH264_E_CREATE, FAILED(hr) ? hr : E_FAIL, "no hardware H.264 encoder (MFTEnumEx)"); return NULL; }
	{
		WCHAR name[128];
		UINT32 got = 0;
		name[0] = 0;
		if (SUCCEEDED(h->act->GetString(MFT_FRIENDLY_NAME_Attribute, name, 127, &got))) { name[127] = 0; wide_to_ascii(h->info.hw_name, sizeof(h->info.hw_name), name); }
	}
	hr = h->act->ActivateObject(kHw_IID_IMFTransform, (void **)&h->mft);
	if (FAILED(hr) || h->mft == NULL) { destroy_hw(h, 0); mfp_fail(err, MFH264_E_CREATE, hr, "ActivateObject"); return NULL; }
	{
		IMFAttributes *at = NULL;
		UINT32 v = 0;
		if (SUCCEEDED(h->mft->GetAttributes(&at)) && at != NULL)
		{
			if (SUCCEEDED(at->GetUINT32(MF_TRANSFORM_ASYNC, &v)) && v != 0) { h->info.async = 1; }
			if (h->info.async)
			{
				hr = at->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
				if (FAILED(hr)) { at->Release(); destroy_hw(h, 0); mfp_fail(err, MFH264_E_ASYNC, hr, "MF_TRANSFORM_ASYNC_UNLOCK"); return NULL; }
			}
			at->Release();
		}
	}
	if (!h->info.async) { destroy_hw(h, 0); mfp_fail(err, MFH264_E_ASYNC, S_OK, "the hardware MFT is synchronous (not driven by this backend)"); return NULL; }
	if (FAILED(h->mft->QueryInterface(kHw_IID_ICodecAPI, (void **)&h->api)) || h->api == NULL) { h->api = NULL; destroy_hw(h, 0); mfp_fail(err, MFH264_E_CODECAPI, E_NOINTERFACE, "QueryInterface(ICodecAPI)"); return NULL; }
	if (FAILED(h->mft->QueryInterface(kHw_IID_IMFMediaEventGenerator, (void **)&h->gen)) || h->gen == NULL) { h->gen = NULL; destroy_hw(h, 0); mfp_fail(err, MFH264_E_ASYNC, E_NOINTERFACE, "QueryInterface(IMFMediaEventGenerator)"); return NULL; }
	h->pump = new EventPump();
	if (!h->pump->start(h->gen)) { delete h->pump; h->pump = NULL; destroy_hw(h, 0); mfp_fail(err, MFH264_E_ALLOC, E_FAIL, "thread of the events"); return NULL; }
	{
		DWORD in = 0, out = 0;
		hr = h->mft->GetStreamIDs(1, &in, 1, &out);	// E_NOTIMPL: the stream ids are 0
		(void)hr;
	}

	// order A (the one that the AMD MFT was measured with): the media types first (the output type, then the input type), then the properties of the codec API; order B: the codec API, then the types
	h->info.gop_order = order ? 'B' : 'A';
	snprintf(h->info.gop_trace, sizeof(h->info.gop_trace), "gop order=%c", order ? 'B' : 'A');
	if (order == 0)
	{
		r = mfp_set_types(h->mft, h->cfg, level, &h->info.color_attrs_ok, &h->info.level_set_ok, err);
		if (r != MFH264_OK) { destroy_hw(h, 0); return NULL; }
	}
	r = hw_apply_api(h, err);
	if (r != MFH264_OK) { destroy_hw(h, 0); return NULL; }
	if (order != 0)
	{
		r = mfp_set_types(h->mft, h->cfg, level, &h->info.color_attrs_ok, &h->info.level_set_ok, err);
		if (r != MFH264_OK) { destroy_hw(h, 0); return NULL; }
	}
	hw_read_back(h);

	MFT_INPUT_STREAM_INFO ii;
	memset(&ii, 0, sizeof(ii));
	if (SUCCEEDED(h->mft->GetInputStreamInfo(0, &ii))) { h->info.in_cb_size = (int)ii.cbSize; h->info.in_alignment = (int)ii.cbAlignment; }
	r = setup_output(h, err);
	if (r == MFH264_OK)
	{
		const DWORD need = (DWORD)((size_t)h->cfg.width * (size_t)h->cfg.height * 3 / 2);
		for (int i = 0; i < MFHW_RING; i++)
		{
			if (FAILED(p_MFCreateSample(&h->inSample[i])) || FAILED(p_MFCreateMemoryBuffer(need, &h->inBuf[i])) || FAILED(h->inSample[i]->AddBuffer(h->inBuf[i]))) { r = mfp_fail(err, MFH264_E_ALLOC, E_OUTOFMEMORY, "input sample"); break; }
		}
	}
	if (r != MFH264_OK) { destroy_hw(h, 0); return NULL; }
	hr = h->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
	if (FAILED(hr)) { destroy_hw(h, 0); mfp_fail(err, MFH264_E_START, hr, "NOTIFY_BEGIN_STREAMING"); return NULL; }
	hr = h->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
	if (FAILED(hr)) { destroy_hw(h, 0); mfp_fail(err, MFH264_E_START, hr, "NOTIFY_START_OF_STREAM"); return NULL; }
	// the MFT says that it is ready: the first request for an input
	{
		HRESULT whr = S_OK;
		const int w = hw_wait(h, g_openMs, 1, 0, &whr);
		if (w != 1)
		{
			const int hang = (w == 0);
			if (hang) { h->dead = 1; }
			mfp_fail(err, hang ? MFH264_E_HANG : MFH264_E_START, hang ? S_OK : whr, hang ? "no METransformNeedInput after START_OF_STREAM" : "event of the encoder after START_OF_STREAM");
			destroy_hw(h, hang);
			return NULL;
		}
		// how many inputs the encoder asked for at the start: its depth (the contract of the glue is 1)
		h->initCredits = 1u;
		// (the events of an encoder that asks for several inputs at the start arrive one after the other: collected until there are none for 30 ms, at most 120 ms)
		for (int quiet = 0, tries = 0; quiet < 3 && tries < 12; tries++)
		{
			EventPump::Ev e;
			int got = 0;
			Sleep(10);
			while (h->pump->pop(&e)) { got++; if (e.type == METransformNeedInput) { h->initCredits++; h->info.events_need++; } else if (e.type == METransformHaveOutput) { h->pendHave++; h->info.events_have++; } else { h->info.events_other++; } }
			quiet = got ? 0 : quiet + 1;
		}
		h->info.max_inflight = (int)h->initCredits;
	}
	refresh_seq_header(h);
	h->info.open_ms = (int)(qpc_ms() - t0);
	return reinterpret_cast<struct mfh264 *>(h);
}

// ------------------------------------------------------------------------------------------------------------------ settings

static int hw_set_qp_impl(struct mfh264 *hh, int qp, struct mfh264_err *err)
{
	mfhw *h = H(hh);
	if (h == NULL || h->api == NULL) { return mfp_fail(err, MFH264_E_ARG, S_OK, "set_qp args"); }
	if (h->dead) { return mfp_fail(err, MFH264_E_HANG, S_OK, "the hardware encoder was given up"); }
	if (qp < 0 || qp > 51) { return mfp_fail(err, MFH264_E_RANGE, S_OK, "qp"); }
	if (h->cfg.rc != MFH264_RC_QUALITY) { return mfp_fail(err, MFH264_E_ARG, S_OK, "set_qp: the rate control is not Quality"); }
	HRESULT hr;
	const ULONGLONG q = (ULONGLONG)qp;
	switch (h->cfg.qp_method)
	{
		case MFH264_QP_ENCODEQP_UI4: hr = mfp_api_set_u32(h->api, CODECAPI_AVEncVideoEncodeQP, (ULONG)qp); break;
		case MFH264_QP_FRAMETYPEQP: hr = mfp_api_set_u64(h->api, CODECAPI_AVEncVideoEncodeFrameTypeQP, q | (q << 16) | (q << 32)); break;
		case MFH264_QP_QUALITY: hr = mfp_api_set_u32(h->api, CODECAPI_AVEncCommonQuality, (ULONG)(100 - qp * 100 / 51)); break;
		default: return mfp_fail(err, MFH264_E_ARG, S_OK, "set_qp: no form of the QP");
	}
	if (FAILED(hr)) { return mfp_fail(err, MFH264_E_CODECAPI, hr, "set qp"); }
	h->info.qp_method = h->cfg.qp_method;
	h->curQp = qp;
	return MFH264_OK;
}

static int hw_set_bitrate_impl(struct mfh264 *hh, int bps, struct mfh264_err *err)
{
	mfhw *h = H(hh);
	if (h == NULL || h->api == NULL) { return mfp_fail(err, MFH264_E_ARG, S_OK, "set_bitrate args"); }
	if (h->dead) { return mfp_fail(err, MFH264_E_HANG, S_OK, "the hardware encoder was given up"); }
	if (bps < 50000 || bps > 400000000) { return mfp_fail(err, MFH264_E_RANGE, S_OK, "bitrate"); }
	const HRESULT hr = mfp_api_set_u32(h->api, CODECAPI_AVEncCommonMeanBitRate, (ULONG)bps);
	if (FAILED(hr)) { return mfp_fail(err, MFH264_E_CODECAPI, hr, "AVEncCommonMeanBitRate"); }
	if (h->cfg.rc != MFH264_RC_QUALITY && h->cfg.vbv > 0) { mfp_api_set_u32(h->api, CODECAPI_AVEncCommonBufferSize, (ULONG)(bps / h->cfg.vbv)); }	// (only the diagnostics: the hardware self test)
	return MFH264_OK;
}

static int hw_force_idr_impl(struct mfh264 *hh, struct mfh264_err *err)
{
	mfhw *h = H(hh);
	if (h == NULL || h->api == NULL) { return mfp_fail(err, MFH264_E_ARG, S_OK, "force_idr args"); }
	if (h->dead) { return mfp_fail(err, MFH264_E_HANG, S_OK, "the hardware encoder was given up"); }
	const HRESULT hr = mfp_api_set_u32(h->api, CODECAPI_AVEncVideoForceKeyFrame, 1);
	if (FAILED(hr)) { return mfp_fail(err, MFH264_E_CODECAPI, hr, "AVEncVideoForceKeyFrame"); }
	h->st.force_calls++;
	h->expectIdr = 1;
	return MFH264_OK;
}

static int hw_set_sample_qp(struct mfh264 *h, int mode, int qp) { (void)h; (void)qp; return mode == 0 ? MFH264_OK : MFH264_E_ARG; }	// the QP is not given in the sample (the hardware encoder takes it from the codec API only)

static int hw_last_out_qp(const struct mfh264 *hh, uint64_t *raw)
{
	const mfhw *h = H(hh);
	if (raw != NULL) { *raw = (h != NULL && h->outQpHave) ? (uint64_t)h->outQpRaw : 0u; }
	return (h != NULL && h->outQpHave) ? 1 : 0;
}

static size_t hw_seq_header(const struct mfh264 *hh, const uint8_t **p)
{
	const mfhw *h = H(hh);
	if (p != NULL) { *p = (h != NULL) ? h->seqHdr : NULL; }
	return h != NULL ? h->seqHdrLen : 0;
}

static size_t hw_max_au_size(const struct mfh264 *hh)
{
	const mfhw *h = H(hh);
	if (h == NULL) { return 0; }
	return (size_t)h->cfg.width * (size_t)h->cfg.height * 3 / 2 + 65536 + h->seqHdrLen;
}

// ------------------------------------------------------------------------------------------------------------------ encoding

// The frame that was accepted gave no usable access unit (or the stream is not in step): flush, start the stream again, forget the credit and the events of before the flush (a NeedInput for an input that the flush
// threw away would make the next ProcessInput come without a request), make the next frame an IDR. A flush gives the credit back (depth 1, the stream is empty): when no fresh request comes within 100 ms it is assumed;
// a wrong guess is answered by the MFT with MF_E_NOTACCEPTING, which is another broken frame (the glue counts three in a row as an error and goes back to the software encoder). Returns MFH264_E_STREAM_BROKEN, or
// MFH264_E_HANG when the encoder is not answering after the flush (the handle is then dead).
static int hw_break(mfhw *h, struct mfh264_err *err, HRESULT hr, const char *stage, int reason)
{
	h->st.broken++;
	switch (reason)
	{
		case MFH264_BROKEN_NO_OUTPUT: h->st.no_output++; break;
		case MFH264_BROKEN_EXTRA_OUTPUT: h->st.extra_output++; break;
		case MFH264_BROKEN_INCOMPLETE: h->st.incomplete++; break;
		case MFH264_BROKEN_OVERSIZE: h->st.oversize++; break;
	}
	h->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
	h->needInput = 0; h->pendHave = 0;
	h->pump->clear();
	h->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
	mfp_api_set_u32(h->api, CODECAPI_AVEncVideoForceKeyFrame, 1);
	h->expectIdr = 1;
	h->info.flushes++;
	{
		HRESULT whr = S_OK;
		const int w = hw_wait(h, 100, 1, 0, &whr);
		if (w == 0) { h->needInput = 1; }
		else if (w < 0) { h->dead = 1; return mfp_fail(err, MFH264_E_HANG, FAILED(whr) ? whr : E_FAIL, "event of the encoder after the flush (error or end of the queue): the encoder is given up"); }
	}
	return mfp_fail_r(err, MFH264_E_STREAM_BROKEN, hr, stage, reason);	// the contract: every restart of the stream is STREAM_BROKEN (what failed is in hr / stage / reason)
}

// One ProcessOutput after a HaveOutput: the access unit into h->raw. 1 = it has one, 0 = the encoder said NEED_MORE_INPUT (no output after all), -1 = error (err filled), -2 = the MFT gave no sample.
// The caller has used up one HaveOutput event for this call. An asynchronous MFT allows one ProcessOutput per event: the answer MF_E_TRANSFORM_STREAM_CHANGE uses the event up (the Intel MFT answers a
// second call without a new event with E_UNEXPECTED), so after the new output type the next HaveOutput is waited for (the deadline of an output) and only then is ProcessOutput called again. An MFT that
// does not send a new event (the output was still owed) gets one ProcessOutput without an event after the deadline; when that fails too the encoder is given up (MFH264_E_HANG, the handle is dead).
static int hw_pull(mfhw *h, struct mfh264_err *err, int *parts)
{
	int nparts = 0;
	int noEventTry = 0;		// the stream change was not followed by an event: this ProcessOutput is the one try without it
	h->rawLen = 0;
	for (int attempt = 0; attempt < 16; attempt++)
	{
		MFT_OUTPUT_DATA_BUFFER ob;
		DWORD status = 0;
		memset(&ob, 0, sizeof(ob));
		ob.dwStreamID = 0;
		ob.pSample = h->outProvides ? NULL : h->outSample;
		if (!h->outProvides && h->outBuf != NULL) { h->outBuf->SetCurrentLength(0); }
		HRESULT hr = h->mft->ProcessOutput(0, 1, &ob, &status);
		if (ob.pEvents != NULL) { ob.pEvents->Release(); ob.pEvents = NULL; }
		IMFSample *s = ob.pSample;
		if (noEventTry && hr != MF_E_TRANSFORM_STREAM_CHANGE && FAILED(hr))
		{
			if (h->outProvides && s != NULL) { s->Release(); }
			h->info.out_fail++; h->info.out_fail_hr = (long)hr;
			h->dead = 1;
			return mfp_fail(err, MFH264_E_HANG, hr, "no METransformHaveOutput after a stream change (ProcessOutput without an event failed)");
		}
		if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
		{
			if (h->outProvides && s != NULL) { s->Release(); }
			if (nparts > 0) { return mfp_fail_r(err, MFH264_E_STREAM_BROKEN, hr, "output announced more parts, then wanted input", MFH264_BROKEN_INCOMPLETE); }
			return 0;
		}
		if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
		{
			if (h->outProvides && s != NULL) { s->Release(); }
			IMFMediaType *t = NULL;
			hr = h->mft->GetOutputAvailableType(0, 0, &t);
			if (SUCCEEDED(hr)) { hr = h->mft->SetOutputType(0, t, 0); t->Release(); }
			if (FAILED(hr)) { return mfp_fail(err, MFH264_E_STREAMCHANGE, hr, "stream change: SetOutputType"); }
			h->st.stream_changes++;
			if (setup_output(h, err) != MFH264_OK) { return err != NULL ? err->code : MFH264_E_ALLOC; }
			refresh_seq_header(h);
			h->info.stream_changes++;
			{
				// the output is still owed: wait for its HaveOutput (the NeedInput events that come meanwhile are kept by hw_wait)
				HRESULT whr = S_OK;
				const double w0 = qpc_ms();
				const int w = hw_wait(h, g_outMs, 0, 1, &whr);
				const double dt = qpc_ms() - w0;
				h->info.sc_wait_ms_last = (unsigned)(dt < 0.0 ? 0.0 : dt);
				if (h->info.sc_wait_ms_last > h->info.sc_wait_ms_max) { h->info.sc_wait_ms_max = h->info.sc_wait_ms_last; }
				if (w < 0) { h->dead = 1; return mfp_fail(err, MFH264_E_HANG, FAILED(whr) ? whr : E_FAIL, "event of the encoder after a stream change (error or end of the queue): the encoder is given up"); }
				if (w > 0) { h->pendHave--; h->info.sc_event++; noEventTry = 0; }
				else { h->info.sc_no_event++; noEventTry = 1; }
			}
			continue;
		}
		if (FAILED(hr))
		{
			if (h->outProvides && s != NULL) { s->Release(); }
			h->info.out_fail++; h->info.out_fail_hr = (long)hr;
			return mfp_fail(err, MFH264_E_OUTPUT, hr, "ProcessOutput");
		}
		if (s == NULL) { return -2; }
		{
			UINT64 qv = 0;
			h->outQpHave = SUCCEEDED(s->GetUINT64(MFSampleExtension_VideoEncodeQP, &qv)) ? 1 : 0;	// the AMD MFT does not write it
			h->outQpRaw = h->outQpHave ? (ULONGLONG)qv : 0;
		}
		IMFMediaBuffer *mb = NULL;
		int rc = 1;
		hr = s->ConvertToContiguousBuffer(&mb);
		if (SUCCEEDED(hr))
		{
			BYTE *p = NULL;
			DWORD mx = 0, cur = 0;
			hr = mb->Lock(&p, &mx, &cur);
			if (SUCCEEDED(hr))
			{
				if (h->rawLen + cur > h->rawCap)
				{
					uint8_t *n = (uint8_t *)realloc(h->raw, h->rawLen + (size_t)cur + 4096);
					if (n == NULL) { rc = mfp_fail(err, MFH264_E_ALLOC, E_OUTOFMEMORY, "raw output buffer"); }
					else { h->raw = n; h->rawCap = h->rawLen + (size_t)cur + 4096; }
				}
				if (rc == 1) { memcpy(h->raw + h->rawLen, p, cur); h->rawLen += cur; }
				mb->Unlock();
			}
			else { rc = mfp_fail(err, MFH264_E_OUTPUT, hr, "output buffer Lock"); }
			mb->Release();
		}
		else { rc = mfp_fail(err, MFH264_E_OUTPUT, hr, "ConvertToContiguousBuffer"); }
		if (h->outProvides) { s->Release(); }
		if (rc != 1) { return rc; }
		nparts++;
		if (ob.dwStatus & MFT_OUTPUT_DATA_BUFFER_INCOMPLETE)
		{
			if (nparts >= MFHW_MAX_PARTS) { return mfp_fail_r(err, MFH264_E_STREAM_BROKEN, S_OK, "output split in too many parts", MFH264_BROKEN_INCOMPLETE); }
			continue;
		}
		if (parts != NULL) { *parts = nparts; }
		return 1;
	}
	return mfp_fail(err, MFH264_E_STREAMCHANGE, S_OK, "too many stream changes");
}

// nv12 != NULL: an NV12 picture of the coded size (the NV12 view) that is copied into the input buffer; else the BGRA picture is converted into it
static int hw_encode_impl(struct mfh264 *hh, const uint8_t *nv12, size_t nv12_len, const uint8_t *bgra, int sw, int sh, int sstride, int64_t ts_100ns, uint8_t *out, size_t out_cap, size_t *out_len, int *is_idr, struct mfh264_err *err)
{
	mfhw *h = H(hh);
	if (out_len != NULL) { *out_len = 0; }
	if (is_idr != NULL) { *is_idr = 0; }
	if (h == NULL || out == NULL || out_len == NULL || is_idr == NULL || (bgra == NULL && nv12 == NULL)) { return mfp_fail(err, MFH264_E_ARG, S_OK, "encode args"); }
	if (h->dead) { return mfp_fail(err, MFH264_E_HANG, S_OK, "the hardware encoder was given up"); }
	const size_t need = (size_t)h->cfg.width * (size_t)h->cfg.height * 3 / 2;
	if (nv12 == NULL && (sw < 1 || sh < 1 || sw > h->cfg.width || sh > h->cfg.height)) { return mfp_fail(err, MFH264_E_ARG, S_OK, "bgra size"); }
	if (nv12 != NULL && nv12_len != need) { return mfp_fail(err, MFH264_E_ARG, S_OK, "nv12 size"); }
	HRESULT whr = S_OK;

	// 1. the request for an input (METransformNeedInput). An output that nobody asked for (an access unit of an earlier frame that came late) is a lost reference: the stream starts again with an IDR.
	if (!h->needInput)
	{
		const int w = hw_wait(h, g_inMs, 1, 0, &whr);
		if (w == 0) { h->dead = 1; return mfp_fail(err, MFH264_E_HANG, S_OK, "no METransformNeedInput (the encoder does not ask for the next frame)"); }
		if (w < 0) { h->dead = 1; return mfp_fail(err, MFH264_E_HANG, FAILED(whr) ? whr : E_FAIL, "event of the encoder (error or end of the queue): the encoder is given up"); }
	}
	{
		// events that came meanwhile (a HaveOutput that nobody waits for)
		EventPump::Ev e;
		while (h->pump->pop(&e))
		{
			if (e.type == METransformNeedInput) { h->needInput = 1; h->info.events_need++; }
			else if (e.type == METransformHaveOutput) { h->pendHave++; h->info.events_have++; }
			else if (e.type == MEError || e.type == 0xFFFF) { h->dead = 1; return mfp_fail(err, MFH264_E_HANG, FAILED(e.status) ? e.status : E_FAIL, "event of the encoder (error or end of the queue): the encoder is given up"); }
			else { h->info.events_other++; }
		}
	}
	if (h->pendHave > 0)
	{
		// HaveOutput events that nobody waited for: an access unit of an earlier frame that came late is a lost reference (the stream starts again with an IDR). An event that has no unit behind it (ProcessOutput says
		// NEED_MORE_INPUT: a late event of before a flush) is only counted.
		struct mfh264_err ignore;
		int parts = 0, real = 0;
		memset(&ignore, 0, sizeof(ignore));
		while (h->pendHave > 0) { h->pendHave--; if (hw_pull(h, &ignore, &parts) != 0) { real = 1; } if (h->dead) { break; } }
		if (h->dead) { if (err != NULL) { *err = ignore; } return ignore.code; }	// the encoder did not answer after a stream change: given up
		if (real) { return hw_break(h, err, S_OK, "an access unit that nobody asked for", MFH264_BROKEN_EXTRA_OUTPUT); }
		h->info.events_other++;
	}

	// 2. the picture into the next input sample
	const int idx = h->inIdx;
	h->inIdx = (idx + 1) % MFHW_RING;
	BYTE *dst = NULL;
	HRESULT hr = h->inBuf[idx]->Lock(&dst, NULL, NULL);
	if (FAILED(hr)) { return mfp_fail(err, MFH264_E_ALLOC, hr, "input buffer Lock"); }
	int cr = 0;
	if (nv12 != NULL) { memcpy(dst, nv12, need); }
	else { cr = kvm_yuv_bgra_to_nv12(bgra, sw, sh, sstride, dst, h->cfg.width, h->cfg.height, KVM_YUV_AUTO); }
	h->inBuf[idx]->Unlock();
	if (cr != 0) { return mfp_fail(err, MFH264_E_ARG, S_OK, "bgra conversion arguments"); }
	h->inBuf[idx]->SetCurrentLength((DWORD)need);
	h->inSample[idx]->SetSampleTime(ts_100ns);
	h->inSample[idx]->SetSampleDuration(h->frameDur);

	// 3. input; the credit is used
	const double t0 = qpc_ms();
	hr = h->mft->ProcessInput(0, h->inSample[idx], 0);
	if (FAILED(hr)) { return hw_break(h, err, hr, hr == MF_E_NOTACCEPTING ? "ProcessInput (MF_E_NOTACCEPTING: no request for an input)" : "ProcessInput", MFH264_BROKEN_ERROR); }
	h->needInput = 0;
	h->st.frames_in++;

	// 4. the output (METransformHaveOutput), with a deadline
	{
		const int w = hw_wait(h, g_outMs, 0, 1, &whr);
		if (w == 0) { h->dead = 1; return mfp_fail(err, MFH264_E_HANG, S_OK, "no METransformHaveOutput (the encoder does not deliver the frame)"); }
		if (w < 0) { h->dead = 1; return mfp_fail(err, MFH264_E_HANG, FAILED(whr) ? whr : E_FAIL, "event of the encoder (error or end of the queue): the encoder is given up"); }
	}
	h->pendHave--;
	{
		const double us = (qpc_ms() - t0) * 1000.0;
		h->info.out_wait_us_last = (unsigned)(us < 0.0 ? 0.0 : us);
		if (h->info.out_wait_us_last > h->info.out_wait_us_max) { h->info.out_wait_us_max = h->info.out_wait_us_last; }
	}
	int parts = 0;
	const int r = hw_pull(h, err, &parts);
	if (r == -2) { return hw_break(h, err, S_OK, "ProcessOutput gave no sample", MFH264_BROKEN_NO_OUTPUT); }
	if (r < 0)
	{
		if (h->dead) { return r; }	// the encoder did not answer after a stream change: given up (err says so)
		// an error of ProcessOutput or of the copy, or an output that did not complete: the stream is started again (the cause stays in hr / stage; an INCOMPLETE reason is kept)
		const int reason = (err != NULL && err->reason != 0) ? err->reason : MFH264_BROKEN_ERROR;
		const long ehr = (err != NULL) ? err->hr : 0;
		const char *stage = (err != NULL && err->stage != NULL) ? err->stage : "ProcessOutput";
		return hw_break(h, err, (HRESULT)ehr, stage, reason);
	}
	if (r == 0) { return hw_break(h, err, S_OK, "HaveOutput, then NEED_MORE_INPUT (no access unit)", MFH264_BROKEN_NO_OUTPUT); }
	if (parts > 1) { h->st.split_outputs++; }

	// 5. Annex B, as for the software encoder
	size_t limit = out_cap;
	if (h->cfg.max_au_size > 0 && limit > (size_t)h->cfg.max_au_size) { limit = (size_t)h->cfg.max_au_size; }
	struct kvm_au_info info;
	size_t n = 0;
	const int nr = kvm_annexb_normalize(h->raw, h->rawLen, h->seqHdr, h->seqHdrLen, out, limit, &n, &info);
	if (nr == KVM_ANNEXB_E_SIZE)
	{
		*out_len = n;
		return hw_break(h, err, S_OK, "access unit does not fit into the output buffer / max_au_size", MFH264_BROKEN_OVERSIZE);
	}
	if (nr != KVM_ANNEXB_OK)
	{
		return hw_break(h, err, S_OK, nr == KVM_ANNEXB_E_FORMAT ? "output is not Annex B / has no slice" : (nr == KVM_ANNEXB_E_NOHEADER ? "IDR without SPS/PPS and no sequence header" : "IDR does not start with the SPS"), MFH264_BROKEN_FORMAT);
	}
	*out_len = n;
	*is_idr = info.idr;
	h->st.frames_out++;
	h->st.aud_removed += (uint64_t)info.aud_removed;
	if (info.header_added) { h->st.headers_added++; }
	if (info.sps_profile_idc >= 0) { h->info.sps_profile_idc = info.sps_profile_idc; h->info.sps_level_idc = info.sps_level_idc; }
	if (info.idr)
	{
		h->st.idr++;
		if (h->expectIdr) { h->st.idr_forced++; h->expectIdr = 0; }
		else { h->st.idr_unforced++; }
	}
	return MFH264_OK;
}

// ------------------------------------------------------------------------------------------------------------------ guards

// Every call that reaches the driver is guarded (MSVC): a structured exception sets the flag of THIS backend (the software encoder stays usable) and the handle is dead.
#ifdef _MSC_VER
static int hw_fault(struct mfh264_err *e, unsigned long code)
{
	InterlockedExchange(&g_hwFaulted, 1);
	if (e != NULL) { e->code = MFH264_E_EXCEPTION; e->hr = (long)code; e->stage = "structured exception (hardware encoder)"; e->reason = 0; e->exc_code = code; }
	return MFH264_E_EXCEPTION;
}
#define MFHW_GUARDED(expr, err) \
	if (g_hwFaulted) { return mfp_fail((err), MFH264_E_EXCEPTION, S_OK, "hardware encoder: off after an earlier structured exception"); } \
	__try { return (expr); } __except (EXCEPTION_EXECUTE_HANDLER) { return hw_fault((err), GetExceptionCode()); }
#else
#define MFHW_GUARDED(expr, err) \
	if (g_hwFaulted) { return mfp_fail((err), MFH264_E_EXCEPTION, S_OK, "hardware encoder: off after an earlier structured exception"); } \
	return (expr);
#endif

static int hw_init(struct mfh264_err *err) { MFHW_GUARDED(hw_init_impl(err), err) }
static int hw_set_qp(struct mfh264 *h, int qp, struct mfh264_err *err) { MFHW_GUARDED(hw_set_qp_impl(h, qp, err), err) }
static int hw_set_bitrate(struct mfh264 *h, int bps, struct mfh264_err *err) { MFHW_GUARDED(hw_set_bitrate_impl(h, bps, err), err) }
static int hw_force_idr(struct mfh264 *h, struct mfh264_err *err) { MFHW_GUARDED(hw_force_idr_impl(h, err), err) }
static int hw_encode_bgra(struct mfh264 *h, const uint8_t *bgra, int width, int height, int stride, int64_t ts, uint8_t *out, size_t out_cap, size_t *out_len, int *is_idr, struct mfh264_err *err)
{
	MFHW_GUARDED(hw_encode_impl(h, NULL, 0, bgra, width, height, stride, ts, out, out_cap, out_len, is_idr, err), err)
}

static int hw_encode_nv12(struct mfh264 *h, const uint8_t *nv12, size_t nv12_len, int64_t ts, uint8_t *out, size_t out_cap, size_t *out_len, int *is_idr, struct mfh264_err *err)
{
	MFHW_GUARDED(hw_encode_impl(h, nv12, nv12_len, NULL, 0, 0, 0, ts, out, out_cap, out_len, is_idr, err), err)
}
static int hw_faulted(void) { return g_hwFaulted != 0; }

// the order A; when no GOP candidate holds (read back as it was set), the order B with a new encoder; when the GOP still reads as a periodic key frame (<= 10000) the open fails: such an encoder
// sends a key frame of 300 KB every second. The diagnostics (cfg.order 1 / 2) keep what they get.
static struct mfh264 *hw_open_impl(const struct mfh264_cfg *cfg, struct mfh264_err *err)
{
	struct mfh264 *hh = hw_open_core(cfg, err, cfg->order == 2 ? 1 : 0);
	char traceA[200];
	mfhw *h = H(hh);
	if (h == NULL) { return hh; }
	if (cfg->order == 0 && !h->gopPass)
	{
		snprintf(traceA, sizeof(traceA), "%s", h->info.gop_trace);
		destroy_hw(h, 0);
		hh = hw_open_core(cfg, err, 1);
		h = H(hh);
		if (h == NULL) { return hh; }
		{
			char both[sizeof(traceA)];
			snprintf(both, sizeof(both), "%.90s | %.100s", traceA, h->info.gop_trace);
			snprintf(h->info.gop_trace, sizeof(h->info.gop_trace), "%s", both);
		}
	}
	if (cfg->order == 0 && h->info.gop_read >= 0 && h->info.gop_read <= 10000)
	{
		destroy_hw(h, 0);
		mfp_fail(err, MFH264_E_CODECAPI, S_OK, "GOP periodic");
		return NULL;
	}
	return hh;
}

static struct mfh264 *hw_open(const struct mfh264_cfg *cfg, struct mfh264_err *err)
{
	if (g_hwFaulted) { mfp_fail(err, MFH264_E_EXCEPTION, S_OK, "hardware encoder: off after an earlier structured exception"); return NULL; }
#ifdef _MSC_VER
	__try { return hw_open_impl(cfg, err); }
	__except (EXCEPTION_EXECUTE_HANDLER) { hw_fault(err, GetExceptionCode()); return NULL; }
#else
	return hw_open_impl(cfg, err);
#endif
}

static void hw_shutdown(void)
{
	if (g_hwFaulted) { return; }	// MFShutdown with a live (faulted) encoder: leaked on purpose
#ifdef _MSC_VER
	__try { mfp_shutdown_impl(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { hw_fault(NULL, GetExceptionCode()); }
#else
	mfp_shutdown_impl();
#endif
}

static void hw_close(struct mfh264 *hh)
{
	mfhw *h = H(hh);
	if (h == NULL) { return; }
	if (g_hwFaulted || h->dead) { return; }	// leaked on purpose (the encoder may be blocked in the driver, or the driver faulted)
#ifdef _MSC_VER
	__try { destroy_hw(h, 0); }
	__except (EXCEPTION_EXECUTE_HANDLER) { hw_fault(NULL, GetExceptionCode()); }
#else
	destroy_hw(h, 0);
#endif
}

static void hw_get_info(const struct mfh264 *hh, struct mfh264_info *info)
{
	const mfhw *h = H(hh);
	if (info == NULL) { return; }
	if (h == NULL) { memset(info, 0, sizeof(*info)); return; }
	*info = h->info;
	info->max_au_size = (int)hw_max_au_size(hh);
	info->backend = 1;
}

void kvm_mfhw_get_stats(const struct mfh264 *hh, struct mfh264_stats *st)
{
	if (st == NULL) { return; }
	if (hh == NULL) { memset(st, 0, sizeof(*st)); return; }
	*st = H(hh)->st;
}

int kvm_mfhw_select(int64_t luid, kvm_mfhw_sel *out)
{
	if (out == NULL) { return 0; }
	memset(out, 0, sizeof(*out));
	if (g_hwFaulted) { return 0; }
#ifdef _MSC_VER
	__try { return select_impl(luid, out); }
	__except (EXCEPTION_EXECUTE_HANDLER) { hw_fault(NULL, GetExceptionCode()); return 0; }
#else
	return select_impl(luid, out);
#endif
}

// The selection on a thread of its own with a time limit: the enumeration of the MFTs and of the adapters is the driver's code and may hang; the thread of the video encoder, which
// may be the software one, must not be the one that waits for it. -1 = the time passed (the thread is left to finish by itself, and frees what it holds), 0 = no hardware encoder, 1 = found.
namespace
{
	struct SelJob { volatile LONG refs; int64_t luid; unsigned delayMs; kvm_mfhw_sel sel; int found; };
	volatile unsigned g_selDelayMs = 0;
	DWORD WINAPI sel_thread(LPVOID p)
	{
		SelJob *j = (SelJob *)p;
		if (p_CoInitializeEx != NULL) { p_CoInitializeEx(NULL, COINIT_MULTITHREADED); }
		if (j->delayMs != 0) { Sleep(j->delayMs); }
		j->found = kvm_mfhw_select(j->luid, &j->sel);
		if (InterlockedDecrement(&j->refs) == 0) { free(j); }
		return 0;
	}
}

void kvm_mfhw_test_select_delay(unsigned ms) { g_selDelayMs = ms; }

int kvm_mfhw_select_timed(int64_t luid, kvm_mfhw_sel *out, unsigned timeoutMs)
{
	SelJob *j;
	HANDLE th;
	int r;
	if (out == NULL) { return 0; }
	memset(out, 0, sizeof(*out));
	j = (SelJob *)calloc(1, sizeof(SelJob));
	if (j == NULL) { return 0; }
	j->refs = 2; j->luid = luid; j->delayMs = g_selDelayMs;
	th = CreateThread(NULL, 0, sel_thread, j, 0, NULL);
	if (th == NULL) { free(j); return 0; }
	if (WaitForSingleObject(th, timeoutMs) == WAIT_OBJECT_0) { *out = j->sel; r = (j->found && out->found) ? 1 : 0; }
	else { r = -1; }
	CloseHandle(th);
	if (InterlockedDecrement(&j->refs) == 0) { free(j); }
	return r;
}

// the table is filled by position: a field that the encoder thread adds to kvm_encbe (a pointer, before or after the others) has to be given here too, and this stops the build until it is
static_assert(sizeof(kvm_encbe) == 2 * sizeof(void *) + 15 * sizeof(void *), "kvm_encbe has another number of fields: kvm_encbe_mfhw (mf_h264_hw.cpp) and kvm_encbe_mf (kvm_encbe_mf.cpp) are filled by position");

extern "C" const kvm_encbe kvm_encbe_mfhw =
{
	"hardware MFT", KVM_ENCBE_CAP_HW | KVM_ENCBE_CAP_QP,		// (depth 1 and blocking inside: not CAP_ASYNC)
	hw_init, hw_shutdown, hw_faulted, hw_open, hw_close, hw_set_qp, hw_set_bitrate, hw_set_sample_qp, hw_last_out_qp, hw_force_idr,
	hw_encode_bgra, hw_encode_nv12, hw_get_info, hw_max_au_size, hw_seq_header
};

