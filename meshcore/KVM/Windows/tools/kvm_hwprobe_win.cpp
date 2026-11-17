// kvm_h264_check --hw: probe, benchmark and dump of the HARDWARE H.264 encoder (Media Foundation transforms that MFTEnumEx lists with MFT_ENUM_FLAG_HARDWARE), next to the
// software encoder for comparison. A tool only: no capture, no KVM session, no desktop, no change of the system, no secrets in the output. Nothing here is linked into the agent.
//
//   kvm_h264_check --hw [probe,bench,dump] [--out DIR] [--max-seconds N] [--frames N] [--size WxH] [--candidate K] [--no-software] [--no-d3d] [--dump-software]
//   kvm_h264_check --hw check FILE.h264 [codedW codedH [maxLevel]]     the rules of the viewer on a stream from a file (no Media Foundation)
//
//   probe   MFTEnumEx for the H.264 encoders (hardware / synchronous / asynchronous, NV12 in), the adapters (DXGI and MFTEnum2), per hardware encoder: name, vendor, flags,
//           async, MF_SA_D3D11_AWARE, available types, profiles and sizes it accepts, the codec API properties (IsSupported / IsModifiable / range / default / value, before and
//           after the media types), the rate control modes it accepts, and "qpmid": for every way of steering the quantizer (EncodeQP as UI8 and UI4, FrameTypeQP, AVEncCommonQuality,
//           CBR, LowDelayVBR, PeakConstrainedVBR) 3 phases of 12 pictures with the setting changed in the middle of the stream, the size of the P pictures and the QP that the
//           encoder reports in its output sample after each change.
//   bench   1080p (1920x1088 coded), 1920x1200 (1920x1216) and the size of the display when it is known: text / scroll / video pictures (NV12 in system memory), constant QP 26 / 30 / 34
//           (or the closest rate control mode), per picture the time from ProcessInput to the output sample (P50 / P95 / max), throughput, sizes, IDR size, a forced IDR in the middle, the
//           CPU time of the process; the same runs with the software encoder; one run with the D3D manager set when the encoder is D3D11 aware. Asynchronous encoders are driven by their events.
//   dump    the first IDR (SPS / PPS), a forced IDR and a few access units to files, and the access units checked against the rules of the viewer (kvm_h264_rules.c): PASS / FAIL per
//           rule, on the encoder's raw output and on what kvm_annexb_normalize makes of it (that is what the agent would send).
//
// Output: a human summary on stdout, one JSON document (hwprobe.json in --out, also the last line of stdout that starts with {"kind":"hwprobe"}). It is rewritten after every stage, so a
// partial result exists when the process is stopped. Every stage runs in its own thread with a time limit; a stage that does not return is abandoned (its thread leaks, the hardware
// stages after it are skipped); a watchdog thread ends the process 10 s after the total limit (--max-seconds, default 100, at most 110), so the run never lasts more than 2 minutes.
//
// Exit codes (the highest priority first): 2 usage error or no Media Foundation; 4 a stage hung or the watchdog ended the process; 3 no hardware H.264 encoder found (the probe and the
// software runs are complete); 1 a stage or a run failed (open refused, no output); 5 the time budget was used up and runs were skipped; 0 everything that was asked for ran.

#include "kvm_hwprobe.h"
#include "kvm_hwprobe_core.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <initguid.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mferror.h>
#include <mfobjects.h>
#include <mfidl.h>
#include <strmif.h>
#include <codecapi.h>
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "../mf_h264.h"
#include "../../common/kvm_yuv.h"
#include "../../common/kvm_annexb.h"
#include "../../common/kvm_h264_rules.h"

// ---------------------------------------------------------------------------------------------------------------------- GUIDs and interfaces

#ifdef _MSC_VER
#define HW_IID(T) __uuidof(T)
#else
DEFINE_GUID(kHw_IID_ICodecAPI, 0x901db4c7, 0x31ce, 0x41a2, 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda);
#define IID_ICodecAPI kHw_IID_ICodecAPI
#define HW_IID(T) IID_##T
#endif

// Attributes that the MinGW headers lack (the Windows SDK has them under these names; defined here with other names, no clash).
DEFINE_GUID(kHw_MFT_ENUM_HARDWARE_VENDOR_ID, 0x3aecb0cc, 0x035b, 0x4bcc, 0x81, 0x85, 0x2b, 0x8d, 0x55, 0x1e, 0xf3, 0xaf);
DEFINE_GUID(kHw_MFT_ENUM_ADAPTER_LUID, 0x1d39518c, 0xe220, 0x4da8, 0xa0, 0x7f, 0xba, 0x17, 0x25, 0x52, 0xd6, 0xb1);
DEFINE_GUID(kHw_MFT_GFX_DRIVER_VERSION_ID, 0xf34b9093, 0x05e0, 0x4b16, 0x99, 0x3d, 0x3e, 0x2a, 0x2c, 0xde, 0x6a, 0xd3);
// {6CA50344-051A-4DED-9779-A43305165E35} CLSID_CMSH264EncoderMFT (as in mf_h264.cpp)
DEFINE_GUID(kHw_CLSID_SoftwareEncoder, 0x6ca50344, 0x051a, 0x4ded, 0x97, 0x79, 0xa4, 0x33, 0x05, 0x16, 0x5e, 0x35);

// Test hook (the Wine test with a mock encoder registered with MFTRegisterLocal): the hardware enumerations also list local transforms and treat them as hardware.
#ifdef HWPROBE_TEST_LOCAL
#define HW_ENUM_LOCAL MFT_ENUM_FLAG_LOCALMFT
#else
#define HW_ENUM_LOCAL 0
#endif

#define HW_RC_CBR 0
#define HW_RC_PEAK 1
#define HW_RC_UVBR 2
#define HW_RC_QUALITY 3
#define HW_RC_LDVBR 4

// ---------------------------------------------------------------------------------------------------------------------- run time linking

typedef HRESULT(WINAPI *PFN_MFStartup)(ULONG, DWORD);
typedef HRESULT(WINAPI *PFN_MFShutdown)(void);
typedef HRESULT(WINAPI *PFN_MFCreateMediaType)(IMFMediaType **);
typedef HRESULT(WINAPI *PFN_MFCreateSample)(IMFSample **);
typedef HRESULT(WINAPI *PFN_MFCreateMemoryBuffer)(DWORD, IMFMediaBuffer **);
typedef HRESULT(WINAPI *PFN_MFCreateAttributes)(IMFAttributes **, UINT32);
typedef HRESULT(WINAPI *PFN_MFTEnumEx)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO *, const MFT_REGISTER_TYPE_INFO *, IMFActivate ***, UINT32 *);
typedef HRESULT(WINAPI *PFN_MFTEnum2)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO *, const MFT_REGISTER_TYPE_INFO *, IMFAttributes *, IMFActivate ***, UINT32 *);
typedef HRESULT(WINAPI *PFN_MFCreateDXGIDeviceManager)(UINT *, IMFDXGIDeviceManager **);
typedef HRESULT(WINAPI *PFN_CoInitializeEx)(LPVOID, DWORD);
typedef void(WINAPI *PFN_CoUninitialize)(void);
typedef void(WINAPI *PFN_CoTaskMemFree)(LPVOID);
typedef HRESULT(WINAPI *PFN_CreateDXGIFactory1)(REFIID, void **);
typedef LONG(WINAPI *PFN_RtlGetVersion)(PRTL_OSVERSIONINFOW);
typedef LONG(WINAPI *PFN_RegGetValueW)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);

struct HwApi
{
	PFN_MFStartup MFStartup; PFN_MFShutdown MFShutdown; PFN_MFCreateMediaType MFCreateMediaType; PFN_MFCreateSample MFCreateSample;
	PFN_MFCreateMemoryBuffer MFCreateMemoryBuffer; PFN_MFCreateAttributes MFCreateAttributes; PFN_MFTEnumEx MFTEnumEx; PFN_MFTEnum2 MFTEnum2;
	PFN_MFCreateDXGIDeviceManager MFCreateDXGIDeviceManager; PFN_CoInitializeEx CoInitializeEx; PFN_CoUninitialize CoUninitialize; PFN_CoTaskMemFree CoTaskMemFree;
	PFN_D3D11_CREATE_DEVICE D3D11CreateDevice; PFN_CreateDXGIFactory1 CreateDXGIFactory1; PFN_RegGetValueW RegGetValueW;
};
static HwApi g_api;
static int g_osMajor = 0, g_osMinor = 0, g_osBuild = 0;

static HMODULE load_system_dll(const wchar_t *name)
{
	HMODULE m = LoadLibraryExW(name, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (m == NULL && GetLastError() == ERROR_INVALID_PARAMETER) { m = LoadLibraryW(name); }
	return m;
}

// 0 when Media Foundation and COM are there, else an explanation in *why.
static int load_api(std::string *why)
{
	memset(&g_api, 0, sizeof(g_api));
	HMODULE nt = GetModuleHandleW(L"ntdll.dll");
	PFN_RtlGetVersion rv = (nt != NULL) ? (PFN_RtlGetVersion)GetProcAddress(nt, "RtlGetVersion") : NULL;
	RTL_OSVERSIONINFOW vi;
	memset(&vi, 0, sizeof(vi));
	vi.dwOSVersionInfoSize = sizeof(vi);
	if (rv != NULL && rv(&vi) == 0) { g_osMajor = (int)vi.dwMajorVersion; g_osMinor = (int)vi.dwMinorVersion; g_osBuild = (int)vi.dwBuildNumber; }
	if (g_osMajor < 6 || (g_osMajor == 6 && g_osMinor < 2)) { *why = "Windows 8 or later is needed"; return 1; }
	HMODULE mf = load_system_dll(L"mfplat.dll"), ole = load_system_dll(L"ole32.dll");
	if (mf == NULL || ole == NULL) { *why = "mfplat.dll or ole32.dll cannot be loaded"; return 1; }
	g_api.MFStartup = (PFN_MFStartup)GetProcAddress(mf, "MFStartup");
	g_api.MFShutdown = (PFN_MFShutdown)GetProcAddress(mf, "MFShutdown");
	g_api.MFCreateMediaType = (PFN_MFCreateMediaType)GetProcAddress(mf, "MFCreateMediaType");
	g_api.MFCreateSample = (PFN_MFCreateSample)GetProcAddress(mf, "MFCreateSample");
	g_api.MFCreateMemoryBuffer = (PFN_MFCreateMemoryBuffer)GetProcAddress(mf, "MFCreateMemoryBuffer");
	g_api.MFCreateAttributes = (PFN_MFCreateAttributes)GetProcAddress(mf, "MFCreateAttributes");
	g_api.MFTEnumEx = (PFN_MFTEnumEx)GetProcAddress(mf, "MFTEnumEx");
	g_api.MFTEnum2 = (PFN_MFTEnum2)GetProcAddress(mf, "MFTEnum2");
	g_api.MFCreateDXGIDeviceManager = (PFN_MFCreateDXGIDeviceManager)GetProcAddress(mf, "MFCreateDXGIDeviceManager");
	g_api.CoInitializeEx = (PFN_CoInitializeEx)GetProcAddress(ole, "CoInitializeEx");
	g_api.CoUninitialize = (PFN_CoUninitialize)GetProcAddress(ole, "CoUninitialize");
	g_api.CoTaskMemFree = (PFN_CoTaskMemFree)GetProcAddress(ole, "CoTaskMemFree");
	if (!g_api.MFStartup || !g_api.MFShutdown || !g_api.MFCreateMediaType || !g_api.MFCreateSample || !g_api.MFCreateMemoryBuffer || !g_api.MFCreateAttributes || !g_api.MFTEnumEx
		|| !g_api.CoInitializeEx || !g_api.CoUninitialize || !g_api.CoTaskMemFree) { *why = "a Media Foundation or COM function is missing"; return 1; }
	HMODULE d3d = load_system_dll(L"d3d11.dll"), dx = load_system_dll(L"dxgi.dll"), adv = load_system_dll(L"advapi32.dll");
	if (d3d != NULL) { g_api.D3D11CreateDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(d3d, "D3D11CreateDevice"); }
	if (dx != NULL) { g_api.CreateDXGIFactory1 = (PFN_CreateDXGIFactory1)GetProcAddress(dx, "CreateDXGIFactory1"); }
	if (adv != NULL) { g_api.RegGetValueW = (PFN_RegGetValueW)GetProcAddress(adv, "RegGetValueW"); }
	return 0;
}

// ---------------------------------------------------------------------------------------------------------------------- small helpers

static std::string utf8(const wchar_t *w)
{
	if (w == NULL || w[0] == 0) { return ""; }
	const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	if (n <= 1) { return ""; }
	std::string s((size_t)n, 0);
	WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, NULL, NULL);
	s.resize((size_t)n - 1);
	return s;
}

static std::string guid_str(const GUID &g)
{
	char b[48];
	_snprintf(b, sizeof(b), "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
	return b;
}

// FOURCC based media subtypes by name ("NV12"), others as the GUID.
static std::string subtype_name(const GUID &g)
{
	static const unsigned char tail[8] = { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
	if (g.Data2 == 0 && g.Data3 == 0x0010 && memcmp(g.Data4, tail, 8) == 0)
	{
		const unsigned long d = g.Data1;
		char c[5] = { (char)(d & 0xFF), (char)((d >> 8) & 0xFF), (char)((d >> 16) & 0xFF), (char)((d >> 24) & 0xFF), 0 };
		bool ok = d > 0xFFFF;
		for (int i = 0; i < 4; i++) { if (c[i] < 0x20 || c[i] > 0x7E) { ok = false; } }
		if (ok) { return c; }
		char b[24];
		_snprintf(b, sizeof(b), "D3DFMT_%lu", d);
		return b;
	}
	return guid_str(g);
}

static const char *hr_name(HRESULT hr)
{
	switch ((unsigned long)hr)
	{
		case 0x00000000: return "S_OK";
		case 0x00000001: return "S_FALSE";
		case 0x80004001: return "E_NOTIMPL";
		case 0x80004002: return "E_NOINTERFACE";
		case 0x80004003: return "E_POINTER";
		case 0x80004005: return "E_FAIL";
		case 0x8007000E: return "E_OUTOFMEMORY";
		case 0x80070057: return "E_INVALIDARG";
		case 0x8000FFFF: return "E_UNEXPECTED";
		case 0x80070005: return "E_ACCESSDENIED";
		case 0x887A0005: return "DXGI_ERROR_DEVICE_REMOVED";
		case 0x887A0006: return "DXGI_ERROR_DEVICE_HUNG";
		case 0x887A0007: return "DXGI_ERROR_DEVICE_RESET";
		case 0xC00D36B4: return "MF_E_INVALIDMEDIATYPE";
		case 0xC00D36B5: return "MF_E_NOTACCEPTING";
		case 0xC00D36E6: return "MF_E_ATTRIBUTENOTFOUND";
		case 0xC00D3E80: return "MF_E_NO_EVENTS_AVAILABLE";
		case 0xC00D3E85: return "MF_E_SHUTDOWN";
		case 0xC00D6D60: return "MF_E_TRANSFORM_TYPE_NOT_SET";
		case 0xC00D6D61: return "MF_E_TRANSFORM_STREAM_CHANGE";
		case 0xC00D6D72: return "MF_E_TRANSFORM_NEED_MORE_INPUT";
		case 0xC00D6D76: return "MF_E_UNSUPPORTED_D3D_TYPE";
		case 0xC00D6D77: return "MF_E_TRANSFORM_ASYNC_LOCKED";
	}
	return "";
}

static std::string hr_json(HRESULT hr)
{
	char b[24];
	_snprintf(b, sizeof(b), "0x%08lX", (unsigned long)hr);
	JObj o;
	o.str("hr", b);
	if (hr_name(hr)[0] != 0) { o.str("name", hr_name(hr)); }
	return o.done();
}

template <class T> static void rel(T *&p)
{
	if (p != NULL) { p->Release(); p = NULL; }
}

static std::string attr_string(IMFAttributes *a, const GUID &k)
{
	LPWSTR w = NULL;
	UINT32 n = 0;
	std::string s;
	if (a != NULL && SUCCEEDED(a->GetAllocatedString(k, &w, &n)) && w != NULL) { s = utf8(w); g_api.CoTaskMemFree(w); }
	return s;
}


// ---------------------------------------------------------------------------------------------------------------------- run state, budget, stages, watchdog

struct HwCfg
{
	bool probe, bench, dump;
	std::string outDir;
	int totalS, frames, candidate;
	bool noSoftware, noD3d, dumpSoftware;
	std::vector<std::pair<int, int> > sizes;   // extra sizes of the pictures (visible), e.g. the display
	HwCfg() : probe(false), bench(false), dump(false), outDir("."), totalS(100), frames(90), candidate(0), noSoftware(false), noD3d(false), dumpSoftware(false) {}
};

static HwCfg g_cfg;
static HwDoc g_doc;
static double g_t0 = 0, g_deadline = 0;            // start of the run, end of the budget (ms of hw_now_ms)
static volatile LONG g_hungStages = 0;             // stages abandoned after their time limit
static volatile LONG g_failed = 0;                 // stages or runs that failed
static volatile LONG g_skipped = 0;                // runs skipped because the budget was used up
static volatile LONG g_hwTainted = 0;              // a hardware stage hung: no more hardware stages (a hung driver call can hold a lock that the next one needs)
static volatile LONG g_finished = 0;
static std::string g_curStage;
static HwLock g_stageLock;

static double left_ms(void) { return g_deadline - hw_now_ms(); }
static double elapsed_s(void) { return (hw_now_ms() - g_t0) / 1000.0; }

static std::string out_path(const char *name)
{
	std::string p = g_cfg.outDir;
	if (!p.empty() && p[p.size() - 1] != '\\' && p[p.size() - 1] != '/') { p += "\\"; }
	return p + name;
}

static bool write_file(const std::string &path, const void *data, size_t n)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (f == NULL) { return false; }
	const bool ok = n == 0 || fwrite(data, 1, n, f) == n;
	fclose(f);
	return ok;
}

// The document is written to a temporary file and moved over hwprobe.json, so a reader never sees half of it.
static void write_snapshot(bool incomplete, const char *note)
{
	const std::string j = g_doc.render(incomplete, note);
	const std::string tmp = out_path("hwprobe.json.tmp"), dst = out_path("hwprobe.json");
	if (write_file(tmp, j.data(), j.size())) { MoveFileExA(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING); }
}

struct Problem { static void add(const char *stage, const char *what, HRESULT hr) { JObj o; o.str("stage", stage).str("what", what).hex("hr", (unsigned long)hr); g_doc.append("problems", o.done()); } };

#ifdef _MSC_VER
#define HW_TRY __try
#define HW_EXCEPT(code) __except (EXCEPTION_EXECUTE_HANDLER) { code }
#else
#define HW_TRY
#define HW_EXCEPT(code)
#endif

struct StageCtx
{
	void (*fn)(void *);
	void *arg;
	volatile LONG done;
	volatile LONG exc;
	volatile DWORD excCode;
};

static DWORD WINAPI stage_thread(LPVOID p)
{
	StageCtx *c = (StageCtx *)p;
	g_api.CoInitializeEx(NULL, COINIT_MULTITHREADED);
	HW_TRY { c->fn(c->arg); }
	HW_EXCEPT(c->exc = 1; c->excCode = GetExceptionCode();)
	c->done = 1;
	return 0;
}

// Runs fn(arg) in a thread of its own and waits at most timeoutMs. Returns 1 when it finished, 0 when it did not (the thread is abandoned: it leaks with everything it holds, that is
// the price of a driver call that never returns), -1 when it ended with a structured exception. arg has to live on the heap when the stage may be abandoned.
static int run_stage(const char *name, int timeoutMs, void (*fn)(void *), void *arg, bool hardware)
{
	if (hardware && g_hwTainted) { Problem::add(name, "skipped: an earlier hardware stage did not return", 0); return 0; }
	{ HwGuard g(g_stageLock); g_curStage = name; }
	const double budget = left_ms();
	if (budget < 1500) { g_skipped++; hw_say("  [%s] skipped: the time budget is used up\n", name); return 0; }
	const int limit = (int)std::min<double>((double)timeoutMs, budget + 4000.0);
	StageCtx *c = new StageCtx;
	c->fn = fn; c->arg = arg; c->done = 0; c->exc = 0; c->excCode = 0;
	HANDLE h = CreateThread(NULL, 0, stage_thread, c, 0, NULL);
	if (h == NULL) { delete c; Problem::add(name, "CreateThread failed", (HRESULT)GetLastError()); g_failed++; return 0; }
	const DWORD w = WaitForSingleObject(h, (DWORD)limit);
	if (w != WAIT_OBJECT_0)
	{
		g_hungStages++;
		if (hardware) { g_hwTainted = 1; }
		hw_say("  [%s] DID NOT RETURN within %d ms: the stage is abandoned%s\n", name, limit, hardware ? ", the hardware stages after it are skipped" : "");
		Problem::add(name, "did not return within its time limit (stage abandoned)", 0);
		CloseHandle(h);
		return 0;
	}
	CloseHandle(h);
	const int r = c->exc ? -1 : 1;
	if (c->exc)
	{
		g_failed++;
		char b[64];
		_snprintf(b, sizeof(b), "structured exception 0x%08lX", (unsigned long)c->excCode);
		hw_say("  [%s] %s\n", name, b);
		Problem::add(name, b, (HRESULT)c->excCode);
		if (hardware) { g_hwTainted = 1; }
	}
	delete c;
	write_snapshot(true, "run in progress");
	return r;
}

static DWORD WINAPI watchdog_thread(LPVOID)
{
	for (;;)
	{
		Sleep(250);
		if (g_finished) { return 0; }
		if (hw_now_ms() > g_deadline + 10000.0)
		{
			std::string st;
			{ HwGuard g(g_stageLock); st = g_curStage; }
			char note[160];
			_snprintf(note, sizeof(note), "watchdog: the total limit was passed while the stage \"%s\" was running", st.c_str());
			hw_say("WATCHDOG: %s; the process ends now\n", note);
			Problem::add(st.c_str(), "watchdog: total time limit passed", 0);
			write_snapshot(true, note);
			const std::string j = g_doc.render(true, note);
			hw_say("%s\n", j.c_str());
			TerminateProcess(GetCurrentProcess(), 4);
			return 0;
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------------- machine (no Media Foundation)

static std::string reg_string(HKEY root, const wchar_t *sub, const wchar_t *val)
{
	wchar_t buf[256];
	DWORD sz = sizeof(buf);
	memset(buf, 0, sizeof(buf));
	if (g_api.RegGetValueW != NULL && g_api.RegGetValueW(root, sub, val, 0x2 /* RRF_RT_REG_SZ */, NULL, buf, &sz) == ERROR_SUCCESS) { return utf8(buf); }
	return "";
}

struct Adapter { std::string name; unsigned vendor, device; unsigned long long vram, shared; unsigned flags; LUID luid; std::string driver; bool software; };
static std::vector<Adapter> g_adapters;

static void stage_machine(void *)
{
	JObj o;
	SYSTEM_INFO si;
	GetNativeSystemInfo(&si);
	{ char v[48]; _snprintf(v, sizeof(v), "%d.%d.%d", g_osMajor, g_osMinor, g_osBuild); o.str("os", v); }
	o.i("logical_processors", (long long)si.dwNumberOfProcessors);
	o.str("cpu", reg_string(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString"));
	{
		DWORD sess = 0;
		typedef BOOL(WINAPI * PFN_ProcessIdToSessionId)(DWORD, DWORD *);
		PFN_ProcessIdToSessionId f = (PFN_ProcessIdToSessionId)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ProcessIdToSessionId");
		if (f != NULL && f(GetCurrentProcessId(), &sess)) { o.i("session_id", (long long)sess); }
	}
	// the current mode of the display, only read (EnumDisplaySettings does not touch the desktop; in a service session it can be the default 1024x768 or nothing)
	{
		DEVMODEW dm;
		memset(&dm, 0, sizeof(dm));
		dm.dmSize = sizeof(dm);
		if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsWidth > 0 && dm.dmPelsHeight > 0)
		{
			o.raw("display_mode", JObj().i("w", (long long)dm.dmPelsWidth).i("h", (long long)dm.dmPelsHeight).i("hz", (long long)dm.dmDisplayFrequency).i("bpp", (long long)dm.dmBitsPerPel).done());
			hw_say("display mode (read only): %lux%lu @ %lu Hz\n", (unsigned long)dm.dmPelsWidth, (unsigned long)dm.dmPelsHeight, (unsigned long)dm.dmDisplayFrequency);
			const int w = (int)dm.dmPelsWidth, h = (int)dm.dmPelsHeight;
			bool known = (w == 1920 && (h == 1080 || h == 1200));
			for (size_t i = 0; i < g_cfg.sizes.size(); i++) { if (g_cfg.sizes[i].first == w && g_cfg.sizes[i].second == h) { known = true; } }
			// a size that the benchmark does not run anyway, within what the encoders take: added as an extra size (at most one from the display)
			if (!known && w >= 640 && h >= 360 && w <= 3840 && h <= 2400 && g_cfg.sizes.size() < 2) { g_cfg.sizes.push_back(std::make_pair(w, h)); }
		}
		else { hw_say("display mode: not available in this session\n"); }
	}
	// DXGI adapters: name, ids, memory, driver version (no device is created)
	JArr ad;
	if (g_api.CreateDXGIFactory1 != NULL)
	{
		IDXGIFactory1 *fac = NULL;
		if (SUCCEEDED(g_api.CreateDXGIFactory1(HW_IID(IDXGIFactory1), (void **)&fac)) && fac != NULL)
		{
			for (UINT i = 0;; i++)
			{
				IDXGIAdapter1 *a = NULL;
				if (fac->EnumAdapters1(i, &a) != S_OK || a == NULL) { break; }
				DXGI_ADAPTER_DESC1 d;
				memset(&d, 0, sizeof(d));
				if (SUCCEEDED(a->GetDesc1(&d)))
				{
					Adapter x;
					x.name = utf8(d.Description); x.vendor = d.VendorId; x.device = d.DeviceId; x.vram = (unsigned long long)d.DedicatedVideoMemory; x.shared = (unsigned long long)d.SharedSystemMemory;
					x.flags = d.Flags; x.luid = d.AdapterLuid; x.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
					LARGE_INTEGER ver;
					memset(&ver, 0, sizeof(ver));
					char vs[48] = "";
					if (SUCCEEDED(a->CheckInterfaceSupport(HW_IID(IDXGIDevice), &ver))) { _snprintf(vs, sizeof(vs), "%u.%u.%u.%u", (unsigned)(ver.HighPart >> 16), (unsigned)(ver.HighPart & 0xFFFF), (unsigned)(ver.LowPart >> 16), (unsigned)(ver.LowPart & 0xFFFF)); }
					x.driver = vs;
					g_adapters.push_back(x);
					char luid[32];
					_snprintf(luid, sizeof(luid), "%08lX-%08lX", (unsigned long)x.luid.HighPart, (unsigned long)x.luid.LowPart);
					ad.raw(JObj().str("name", x.name).hex("vendor_id", x.vendor).hex("device_id", x.device).u("dedicated_mb", x.vram >> 20).u("shared_mb", x.shared >> 20).str("driver_version", x.driver)
						.b("software", x.software).str("luid", luid).done());
					hw_say("adapter %u: %s  vendor 0x%04X device 0x%04X  %llu MB dedicated  driver %s%s\n", i, x.name.c_str(), x.vendor, x.device, x.vram >> 20, x.driver.c_str(), x.software ? "  (software)" : "");
				}
				a->Release();
			}
			fac->Release();
		}
	}
	o.raw("adapters", ad.done());
	g_doc.set("machine", o.done());
}

// ---------------------------------------------------------------------------------------------------------------------- encoders that MFTEnumEx lists

struct Cand
{
	std::string key, name, vendor, driver, url, clsidStr, inTypes, outTypes;
	GUID clsid;
	bool haveClsid, hw, async, haveLuid;
	UINT32 flags, merit;
	LUID luid;
	int index;
	Cand() : haveClsid(false), hw(false), async(false), haveLuid(false), flags(0), merit(0), index(0) { memset(&clsid, 0, sizeof(clsid)); memset(&luid, 0, sizeof(luid)); }
};

static std::vector<Cand> g_hwCands;       // hardware encoders, best first
static Cand g_swCand;                     // the software encoder of Windows (for the comparison)
static bool g_haveSw = false;

static HRESULT enum_mfts(UINT32 flags, bool nv12In, IMFActivate ***acts, UINT32 *n)
{
	MFT_REGISTER_TYPE_INFO in, out;
	in.guidMajorType = MFMediaType_Video; in.guidSubtype = MFVideoFormat_NV12;
	out.guidMajorType = MFMediaType_Video; out.guidSubtype = MFVideoFormat_H264;
	*acts = NULL; *n = 0;
	return g_api.MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nv12In ? &in : NULL, &out, acts, n);
}

static void free_acts(IMFActivate **acts, UINT32 n)
{
	if (acts == NULL) { return; }
	for (UINT32 i = 0; i < n; i++) { if (acts[i] != NULL) { acts[i]->Release(); } }
	g_api.CoTaskMemFree(acts);
}

static std::string type_list(IMFAttributes *a, const GUID &key)
{
	UINT8 *blob = NULL;
	UINT32 sz = 0;
	std::string s;
	if (a != NULL && SUCCEEDED(a->GetAllocatedBlob(key, &blob, &sz)) && blob != NULL)
	{
		const MFT_REGISTER_TYPE_INFO *t = (const MFT_REGISTER_TYPE_INFO *)blob;
		for (UINT32 i = 0; i < sz / sizeof(MFT_REGISTER_TYPE_INFO); i++) { if (!s.empty()) { s += ","; } s += subtype_name(t[i].guidSubtype); }
		g_api.CoTaskMemFree(blob);
	}
	return s;
}

static void fill_cand(IMFActivate *a, Cand *c)
{
	UINT32 v = 0;
	c->name = attr_string(a, MFT_FRIENDLY_NAME_Attribute);
	c->vendor = attr_string(a, kHw_MFT_ENUM_HARDWARE_VENDOR_ID);
	c->url = attr_string(a, MFT_ENUM_HARDWARE_URL_Attribute);
	c->driver = attr_string(a, kHw_MFT_GFX_DRIVER_VERSION_ID);
	if (SUCCEEDED(a->GetUINT32(MF_TRANSFORM_FLAGS_Attribute, &v))) { c->flags = v; }
	if (SUCCEEDED(a->GetUINT32(MFT_CODEC_MERIT_Attribute, &v))) { c->merit = v; }
	c->hw = (c->flags & MFT_ENUM_FLAG_HARDWARE) != 0;
	c->async = (c->flags & MFT_ENUM_FLAG_ASYNCMFT) != 0;
	if (SUCCEEDED(a->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &c->clsid))) { c->haveClsid = true; c->clsidStr = guid_str(c->clsid); }
	c->inTypes = type_list(a, MFT_INPUT_TYPES_Attributes);
	c->outTypes = type_list(a, MFT_OUTPUT_TYPES_Attributes);
	UINT32 got = 0;
	if (SUCCEEDED(a->GetBlob(kHw_MFT_ENUM_ADAPTER_LUID, (UINT8 *)&c->luid, sizeof(c->luid), &got)) && got == sizeof(c->luid)) { c->haveLuid = true; }
}

static std::string cand_json(const Cand &c)
{
	JObj o;
	o.str("key", c.key).str("name", c.name).str("vendor_id", c.vendor).str("clsid", c.clsidStr).hex("transform_flags", c.flags).b("hardware", c.hw).b("async_registered", c.async).u("merit", c.merit)
		.str("input_types", c.inTypes).str("output_types", c.outTypes).str("driver_version_attr", c.driver).str("url", c.url);
	if (c.haveLuid) { char b[32]; _snprintf(b, sizeof(b), "%08lX-%08lX", (unsigned long)c.luid.HighPart, (unsigned long)c.luid.LowPart); o.str("adapter_luid", b); }
	return o.done();
}

// A fresh activation object for a candidate (the same CLSID again): ActivateObject / ShutdownObject of one object is not a thing to rely on for a second session.
static IMFActivate *fresh_activate(const Cand &c)
{
	IMFActivate **acts = NULL;
	UINT32 n = 0;
	IMFActivate *pick = NULL;
	const UINT32 flags = (c.hw ? (MFT_ENUM_FLAG_HARDWARE | HW_ENUM_LOCAL) : (MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_TRANSCODE_ONLY)) | MFT_ENUM_FLAG_SORTANDFILTER;
	for (int pass = 0; pass < 2 && pick == NULL; pass++)
	{
		if (FAILED(enum_mfts(flags, pass == 0, &acts, &n)) || acts == NULL) { continue; }
		for (UINT32 i = 0; i < n && pick == NULL; i++)
		{
			GUID g;
			if (c.haveClsid ? (SUCCEEDED(acts[i]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &g)) && memcmp(&g, &c.clsid, sizeof(GUID)) == 0) : ((int)i == c.index)) { pick = acts[i]; pick->AddRef(); }
		}
		free_acts(acts, n);
	}
	return pick;
}

static void list_cands(const char *label, UINT32 flags, bool nv12In, JArr *ja)
{
	IMFActivate **acts = NULL;
	UINT32 n = 0;
	HRESULT hr = enum_mfts(flags, nv12In, &acts, &n);
	hw_say("  %s: %s%u encoder(s)%s\n", label, FAILED(hr) ? "failed " : "", (unsigned)n, FAILED(hr) ? hr_name(hr) : "");
	for (UINT32 i = 0; SUCCEEDED(hr) && i < n; i++)
	{
		Cand c;
		fill_cand(acts[i], &c);
		c.key = label; c.index = (int)i;
		hw_say("    [%u] %s  vendor %s  flags 0x%02lX%s%s  merit %u  in {%s}  clsid %s\n", (unsigned)i, c.name.c_str(), c.vendor.empty() ? "-" : c.vendor.c_str(), (unsigned long)c.flags, c.hw ? " hardware" : "", c.async ? " async" : "", (unsigned)c.merit, c.inTypes.c_str(), c.clsidStr.c_str());
		if (ja != NULL) { ja->raw(cand_json(c)); }
	}
	free_acts(acts, n);
}

static void stage_enum(void *)
{
	JObj o;
	hw_say("\nencoders (MFTEnumEx, category video encoder, output H.264)\n");
	{
		JArr a;
		list_cands("hardware, NV12 in", MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER | HW_ENUM_LOCAL, true, &a);
		o.raw("hardware_nv12", a.done());
	}
	{
		JArr a;
		list_cands("hardware, any input", MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER | HW_ENUM_LOCAL, false, &a);
		o.raw("hardware_any_input", a.done());
	}
	{
		JArr a;
		list_cands("synchronous", MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_TRANSCODE_ONLY | MFT_ENUM_FLAG_SORTANDFILTER, true, &a);
		o.raw("synchronous", a.done());
	}
	{
		JArr a;
		list_cands("asynchronous", MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, true, &a);
		o.raw("asynchronous", a.done());
	}
	{
		JArr a;
		list_cands("all kinds, any input", MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_FIELDOFUSE | MFT_ENUM_FLAG_TRANSCODE_ONLY, false, &a);
		o.raw("all", a.done());
	}
	// the candidates of the tool
	g_hwCands.clear();
	{
		IMFActivate **acts = NULL;
		UINT32 n = 0;
		if (SUCCEEDED(enum_mfts(MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER | HW_ENUM_LOCAL, true, &acts, &n)))
		{
			for (UINT32 i = 0; i < n; i++)
			{
				Cand c;
				fill_cand(acts[i], &c);
#ifdef HWPROBE_TEST_LOCAL
				c.hw = true;
#endif
				c.index = (int)i;
				char k[16];
				_snprintf(k, sizeof(k), "hw%u", (unsigned)i);
				c.key = k;
				g_hwCands.push_back(c);
			}
			free_acts(acts, n);
		}
	}
	g_haveSw = false;
	{
		IMFActivate **acts = NULL;
		UINT32 n = 0;
		if (SUCCEEDED(enum_mfts(MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_TRANSCODE_ONLY | MFT_ENUM_FLAG_SORTANDFILTER, true, &acts, &n)))
		{
			for (UINT32 i = 0; i < n && !g_haveSw; i++)
			{
				Cand c;
				fill_cand(acts[i], &c);
				c.index = (int)i;
				c.key = "sw";
				if (!c.hw) { g_swCand = c; g_haveSw = true; }
			}
			free_acts(acts, n);
		}
	}
	// which adapter has which encoder (MFTEnum2 with the adapter's LUID, Windows 10 and later): the adapter of the encoder is the one a D3D device has to be created on
	{
		JArr a;
		if (g_api.MFTEnum2 == NULL || g_api.MFCreateAttributes == NULL) { hw_say("  MFTEnum2: not available on this Windows\n"); }
		else
		{
			for (size_t i = 0; i < g_adapters.size(); i++)
			{
				if (g_adapters[i].software) { continue; }
				IMFAttributes *at = NULL;
				IMFActivate **acts = NULL;
				UINT32 n = 0;
				MFT_REGISTER_TYPE_INFO in, out;
				in.guidMajorType = MFMediaType_Video; in.guidSubtype = MFVideoFormat_NV12;
				out.guidMajorType = MFMediaType_Video; out.guidSubtype = MFVideoFormat_H264;
				if (SUCCEEDED(g_api.MFCreateAttributes(&at, 1)) && at != NULL && SUCCEEDED(at->SetBlob(kHw_MFT_ENUM_ADAPTER_LUID, (const UINT8 *)&g_adapters[i].luid, sizeof(LUID))))
				{
					const HRESULT hr = g_api.MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER | HW_ENUM_LOCAL, &in, &out, at, &acts, &n);
					JArr names;
					for (UINT32 k = 0; SUCCEEDED(hr) && k < n; k++)
					{
						Cand c;
						fill_cand(acts[k], &c);
						names.str(c.name.c_str());
						for (size_t q = 0; q < g_hwCands.size(); q++)
						{
							if (c.haveClsid && g_hwCands[q].haveClsid && memcmp(&c.clsid, &g_hwCands[q].clsid, sizeof(GUID)) == 0 && !g_hwCands[q].haveLuid) { g_hwCands[q].luid = g_adapters[i].luid; g_hwCands[q].haveLuid = true; }
						}
					}
					hw_say("  MFTEnum2 on adapter %u (%s): %s%u encoder(s)\n", (unsigned)i, g_adapters[i].name.c_str(), FAILED(hr) ? "failed " : "", (unsigned)n);
					a.raw(JObj().str("adapter", g_adapters[i].name).i("count", (long long)n).raw("encoders", names.done()).hex("hr", (unsigned long)hr).done());
					if (SUCCEEDED(hr)) { free_acts(acts, n); }
				}
				rel(at);
			}
		}
		o.raw("by_adapter", a.done());
	}
	JArr hw;
	for (size_t i = 0; i < g_hwCands.size(); i++) { hw.raw(cand_json(g_hwCands[i])); }
	o.raw("candidates_hardware", hw.done());
	if (g_haveSw) { o.raw("candidate_software", cand_json(g_swCand)); }
	g_doc.set("encoders", o.done());
	hw_say("hardware H.264 encoders for NV12 input: %u%s\n", (unsigned)g_hwCands.size(), g_hwCands.empty() ? "  (none: see exit code 3)" : "");
}

// ---------------------------------------------------------------------------------------------------------------------- codec API access

static HRESULT api_set_u32(ICodecAPI *a, const GUID &g, ULONG v) { VARIANT var; memset(&var, 0, sizeof(var)); var.vt = VT_UI4; var.ulVal = v; return a->SetValue(&g, &var); }
static HRESULT api_set_u64(ICodecAPI *a, const GUID &g, ULONGLONG v) { VARIANT var; memset(&var, 0, sizeof(var)); var.vt = VT_UI8; var.ullVal = v; return a->SetValue(&g, &var); }
static HRESULT api_set_bool(ICodecAPI *a, const GUID &g, int on) { VARIANT var; memset(&var, 0, sizeof(var)); var.vt = VT_BOOL; var.boolVal = on ? VARIANT_TRUE : VARIANT_FALSE; return a->SetValue(&g, &var); }

static int var_to_i64(const VARIANT &v, long long *out)
{
	switch (v.vt)
	{
		case VT_I1: *out = v.cVal; return 1;
		case VT_UI1: *out = v.bVal; return 1;
		case VT_I2: *out = v.iVal; return 1;
		case VT_UI2: *out = v.uiVal; return 1;
		case VT_I4: case VT_INT: *out = v.lVal; return 1;
		case VT_UI4: case VT_UINT: *out = (long long)v.ulVal; return 1;
		case VT_I8: *out = v.llVal; return 1;
		case VT_UI8: *out = (long long)v.ullVal; return 1;
		case VT_BOOL: *out = v.boolVal ? 1 : 0; return 1;
	}
	return 0;
}

struct PropDef { const char *name; const GUID *guid; };
static const PropDef g_props[] = {
	{ "AVLowLatencyMode", &CODECAPI_AVLowLatencyMode },
	{ "AVEncCommonLowLatency", &CODECAPI_AVEncCommonLowLatency },
	{ "AVEncCommonRealTime", &CODECAPI_AVEncCommonRealTime },
	{ "AVEncCommonRateControlMode", &CODECAPI_AVEncCommonRateControlMode },
	{ "AVEncCommonMeanBitRate", &CODECAPI_AVEncCommonMeanBitRate },
	{ "AVEncCommonMaxBitRate", &CODECAPI_AVEncCommonMaxBitRate },
	{ "AVEncCommonBufferSize", &CODECAPI_AVEncCommonBufferSize },
	{ "AVEncCommonQuality", &CODECAPI_AVEncCommonQuality },
	{ "AVEncCommonQualityVsSpeed", &CODECAPI_AVEncCommonQualityVsSpeed },
	{ "AVEncMPVDefaultBPictureCount", &CODECAPI_AVEncMPVDefaultBPictureCount },
	{ "AVEncMPVGOPSize", &CODECAPI_AVEncMPVGOPSize },
	{ "AVEncVideoForceKeyFrame", &CODECAPI_AVEncVideoForceKeyFrame },
	{ "AVEncVideoEncodeQP", &CODECAPI_AVEncVideoEncodeQP },
	{ "AVEncVideoEncodeFrameTypeQP", &CODECAPI_AVEncVideoEncodeFrameTypeQP },
	{ "AVEncVideoMinQP", &CODECAPI_AVEncVideoMinQP },
	{ "AVEncVideoMaxQP", &CODECAPI_AVEncVideoMaxQP },
	{ "AVEncVideoMaxNumRefFrame", &CODECAPI_AVEncVideoMaxNumRefFrame },
	{ "AVEncH264CABACEnable", &CODECAPI_AVEncH264CABACEnable },
	{ "AVEncVideoContentType", &CODECAPI_AVEncVideoContentType },
	{ "AVEncVideoTemporalLayerCount", &CODECAPI_AVEncVideoTemporalLayerCount },
	{ "AVEncVideoLTRBufferControl", &CODECAPI_AVEncVideoLTRBufferControl },
	{ "AVEncVideoROIEnabled", &CODECAPI_AVEncVideoROIEnabled },
	{ "AVEncSliceControlMode", &CODECAPI_AVEncSliceControlMode },
	{ "AVEncNumWorkerThreads", &CODECAPI_AVEncNumWorkerThreads },
	{ "AVEncVideoMeanAbsoluteDifference", &CODECAPI_AVEncVideoMeanAbsoluteDifference },
};
#define N_PROPS ((int)(sizeof(g_props) / sizeof(g_props[0])))

// Every property of the table: IsSupported, IsModifiable, range, default and the current value. One JSON array.
static std::string props_json(ICodecAPI *api, int *nSupported)
{
	JArr a;
	int ns = 0;
	for (int i = 0; i < N_PROPS; i++)
	{
		VARIANT vmin, vmax, vstep, vdef, vcur;
		memset(&vmin, 0, sizeof(vmin)); memset(&vmax, 0, sizeof(vmax)); memset(&vstep, 0, sizeof(vstep)); memset(&vdef, 0, sizeof(vdef)); memset(&vcur, 0, sizeof(vcur));
		JObj o;
		const HRESULT hs = api->IsSupported(g_props[i].guid);
		o.str("name", g_props[i].name).b("supported", hs == S_OK).hex("hr_supported", (unsigned long)hs);
		if (hs == S_OK)
		{
			long long x = 0;
			ns++;
			o.b("modifiable", api->IsModifiable(g_props[i].guid) == S_OK);
			if (SUCCEEDED(api->GetParameterRange(g_props[i].guid, &vmin, &vmax, &vstep)))
			{
				long long lo = 0, hi = 0, st = 0;
				if (var_to_i64(vmin, &lo) && var_to_i64(vmax, &hi)) { var_to_i64(vstep, &st); o.i("min", lo).i("max", hi).i("step", st).i("vt", vmin.vt); }
			}
			if (SUCCEEDED(api->GetDefaultValue(g_props[i].guid, &vdef)) && var_to_i64(vdef, &x)) { o.i("default", x); if (vdef.vt != 0) { o.i("default_vt", vdef.vt); } }
			if (SUCCEEDED(api->GetValue(g_props[i].guid, &vcur)) && var_to_i64(vcur, &x)) { o.i("value", x); }
		}
		a.raw(o.done());
	}
	if (nSupported != NULL) { *nSupported = ns; }
	return a.done();
}

// ---------------------------------------------------------------------------------------------------------------------- the event queue of an asynchronous MFT

class EventPump
{
public:
	struct Ev { DWORD type; HRESULT status; };
	EventPump() : m_thread(NULL), m_gen(NULL), m_stop(0), m_ended(0), m_refs(2) { m_ev = CreateEventW(NULL, FALSE, FALSE, NULL); }	// two owners: the session and the thread
	bool start(IMFMediaEventGenerator *g)
	{
		m_gen = g;
		g->AddRef();
		m_thread = CreateThread(NULL, 0, proc, this, 0, NULL);
		return m_thread != NULL;
	}
	~EventPump() { if (m_ev != NULL) { CloseHandle(m_ev); } }
	bool wait(DWORD ms) { return WaitForSingleObject(m_ev, ms) == WAIT_OBJECT_0; }
	bool pop(Ev *e)
	{
		HwGuard g(m_lock);
		if (m_q.empty()) { return false; }
		*e = m_q.front();
		m_q.erase(m_q.begin());
		return true;
	}
	void request_stop() { m_stop = 1; }
	bool ended() const { return m_ended != 0; }
	// The session lets go of the pump: waits a moment for the thread (GetEvent returns an error when the MFT is shut down); a thread that is still blocked keeps the object and ends it
	// itself (with the MFT it holds) when GetEvent returns, and nothing waits for it.
	void release_by_session(DWORD ms)
	{
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
	void loop()
	{
		g_api.CoInitializeEx(NULL, COINIT_MULTITHREADED);
		while (!m_stop)
		{
			IMFMediaEvent *e = NULL;
			HRESULT hr = m_gen->GetEvent(0, &e);	// blocks until an event arrives or the MFT is shut down
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
			{ HwGuard g(m_lock); m_q.push_back(ev); }
			SetEvent(m_ev);
			if (FAILED(hr)) { break; }
		}
		m_ended = 1;
		SetEvent(m_ev);
		rel(m_gen);
	}
	HANDLE m_thread, m_ev;
	IMFMediaEventGenerator *m_gen;
	volatile LONG m_stop, m_ended, m_refs;
	HwLock m_lock;
	std::vector<Ev> m_q;
};

// ---------------------------------------------------------------------------------------------------------------------- the encoder session

enum { MODE_NONE = 0, MODE_QP_UI8 = 1, MODE_QP_UI4 = 2, MODE_QP_FRAMETYPE = 3, MODE_QP_QUALITY = 4, MODE_CBR = 5, MODE_LDVBR = 6, MODE_PEAKVBR = 7, MODE_COUNT = 8 };

static const char *mode_name(int m)
{
	switch (m)
	{
		case MODE_QP_UI8: return "quality + EncodeQP (UI8 I|P|B)";
		case MODE_QP_UI4: return "quality + EncodeQP (UI4)";
		case MODE_QP_FRAMETYPE: return "quality + FrameTypeQP (UI8)";
		case MODE_QP_QUALITY: return "quality + AVEncCommonQuality";
		case MODE_CBR: return "CBR";
		case MODE_LDVBR: return "LowDelayVBR";
		case MODE_PEAKVBR: return "PeakConstrainedVBR";
	}
	return "none";
}
static const char *mode_id(int m)
{
	switch (m)
	{
		case MODE_QP_UI8: return "qp_ui8";
		case MODE_QP_UI4: return "qp_ui4";
		case MODE_QP_FRAMETYPE: return "qp_frametype";
		case MODE_QP_QUALITY: return "qp_quality";
		case MODE_CBR: return "cbr";
		case MODE_LDVBR: return "ldvbr";
		case MODE_PEAKVBR: return "peakvbr";
	}
	return "none";
}
static bool is_qp_mode(int m) { return m >= MODE_QP_UI8 && m <= MODE_QP_QUALITY; }

struct EncCfg
{
	int w, h;			// coded size (multiple of 16; the caller pads to 32)
	int profile;		// 66 / 77 / 100
	int level;			// level_idc for the output type, 0 = none
	int bitrate;		// MF_MT_AVG_BITRATE
	int mode, qp, rateBps;
	bool d3d, apiFirst, capture;	// capture: list the available types
	EncCfg() : w(1920), h(1088), profile(100), level(42), bitrate(8000000), mode(MODE_QP_UI8), qp(30), rateBps(8000000), d3d(false), apiFirst(false), capture(false) {}
};

struct Change { int at, kind, value; };	// kind 1 = QP, 2 = bit rate

struct RunReq
{
	HwGen *gen;
	int content, frames;
	double endMs;				// soft end of the run (absolute)
	std::vector<Change> changes;
	int forceAt;
	struct kvmh_check *rules;	// every output is checked when given
	std::vector<std::vector<uint8_t> > *keep;	// the first keepMax outputs and every output from keepFrom to keepFrom + 2
	int keepMax, keepFrom;
	RunReq() : gen(NULL), content(0), frames(0), endMs(0), forceAt(-1), rules(NULL), keep(NULL), keepMax(0), keepFrom(-1) {}
};

struct OutRec { double submitMs, outMs; size_t bytes; int idr; int bSlices; uint32_t types; bool hasQp, clean; unsigned long long qpRaw; long long pts; };

struct RunRes
{
	std::vector<double> submit;
	std::vector<OutRec> outs;
	std::vector<int> keptIndex;
	int submitted, streamChanges, needInput, haveOutput, notAccepting, ptsMismatch, maxInflight, forceOutIndex;
	HRESULT err, forceHr;
	std::string errStage;
	bool timedOut, drained, drainTimedOut;
	double wallMs, cpuMs, sourceMs;
	std::vector<HRESULT> changeHr;
	int haveBeforeInput = 0;		// HaveOutput events that came before the first input
	int scEvent = 0, scNoEvent = 0, scNeedWait = 0;	// stream changes followed by a new HaveOutput / by none within 500 ms (then one call without it); NeedInput events that came while it waited
	double scWaitMax = 0;		// the longest wait for that HaveOutput, ms
	HRESULT scSetHr = S_OK; int scTypeIdx = -1, scTypeN = 0; std::string scSubtype;	// what the last stream change chose and how SetOutputType answered
	RunRes() : submitted(0), streamChanges(0), needInput(0), haveOutput(0), notAccepting(0), ptsMismatch(0), maxInflight(0), forceOutIndex(-1), err(S_OK), forceHr(S_OK), timedOut(false), drained(false), drainTimedOut(false), wallMs(0), cpuMs(0), sourceMs(0) {}
};

class Enc
{
public:
	Enc() : act(NULL), mft(NULL), api(NULL), gen(NULL), pump(NULL), dev(NULL), mgr(NULL), async(false), d3dAware(false), provides(false), inId(0), outId(0), outCap(0), outSample(NULL), failHr(S_OK), apiFailHr(S_OK) {}
	HRESULT open(const Cand &c, const EncCfg &cfg);
	void close();
	HRESULT set_qp(int qp);
	HRESULT set_rate(int bps);
	HRESULT force_idr();
	void run(const RunReq &rq, RunRes *res);
	std::string steps_json() const;
	std::string api_json() const;
	HRESULT scSetHr = S_OK;		// the result of SetOutputType at the last stream change (E_FAIL: no H.264 type among the available ones)
	int scTypeIdx = -1, scTypeN = 0;	// the available output type that was chosen (the first with the subtype H.264) and how many the MFT lists
	std::string scSubtype;		// the subtype of the chosen type

	IMFActivate *act; IMFTransform *mft; ICodecAPI *api; IMFMediaEventGenerator *gen; EventPump *pump;
	ID3D11Device *dev; IMFDXGIDeviceManager *mgr;
	bool async, d3dAware, provides;
	DWORD inId, outId, outCap;
	IMFSample *outSample;
	EncCfg cfg;
	std::vector<std::pair<std::string, HRESULT> > steps;
	std::vector<std::pair<std::string, HRESULT> > apiSets;
	std::string failStage, outAvail, inAvail, outTypeNow, propsFresh, propsTyped, rcModes, rcText;
	HRESULT failHr, apiFailHr;
	std::vector<uint8_t> seqHdr;
	int outLevel, outProfile;
	bool levelSet;
private:
	bool step(const char *name, HRESULT hr) { steps.push_back(std::make_pair(std::string(name), hr)); if (FAILED(hr) && failStage.empty()) { failStage = name; failHr = hr; } return SUCCEEDED(hr); }
	HRESULT fail(const char *name, HRESULT hr) { steps.push_back(std::make_pair(std::string(name), hr)); failStage = name; failHr = hr; return hr; }
	HRESULT set_types(const EncCfg &c);
	HRESULT apply_api(const EncCfg &c);
	HRESULT setup_d3d(const Cand &c);
	void read_output_type();
	std::string avail_types(bool input);
	HRESULT pull_one(std::vector<uint8_t> *data, OutRec *rec, bool *got);
	bool record_out(const RunReq &rq, RunRes *res, std::vector<uint8_t> &au, OutRec &rec);
	void note(const char *name, HRESULT hr) { apiSets.push_back(std::make_pair(std::string(name), hr)); }
};

std::string Enc::steps_json() const
{
	JArr a;
	for (size_t i = 0; i < steps.size(); i++) { char b[24]; _snprintf(b, sizeof(b), "0x%08lX", (unsigned long)steps[i].second); JObj o; o.str("step", steps[i].first).str("hr", b); if (hr_name(steps[i].second)[0]) { o.str("name", hr_name(steps[i].second)); } a.raw(o.done()); }
	return a.done();
}

std::string Enc::api_json() const
{
	JArr a;
	for (size_t i = 0; i < apiSets.size(); i++) { char b[24]; _snprintf(b, sizeof(b), "0x%08lX", (unsigned long)apiSets[i].second); JObj o; o.str("set", apiSets[i].first).str("hr", b); if (hr_name(apiSets[i].second)[0]) { o.str("name", hr_name(apiSets[i].second)); } a.raw(o.done()); }
	return a.done();
}

HRESULT Enc::setup_d3d(const Cand &c)
{
	if (g_api.D3D11CreateDevice == NULL || g_api.MFCreateDXGIDeviceManager == NULL) { return fail("D3D11 or MFCreateDXGIDeviceManager is not available", E_NOTIMPL); }
	IDXGIAdapter1 *ad = NULL;
	if (c.haveLuid && g_api.CreateDXGIFactory1 != NULL)
	{
		IDXGIFactory1 *fac = NULL;
		if (SUCCEEDED(g_api.CreateDXGIFactory1(HW_IID(IDXGIFactory1), (void **)&fac)) && fac != NULL)
		{
			for (UINT i = 0; ad == NULL; i++)
			{
				IDXGIAdapter1 *a = NULL;
				DXGI_ADAPTER_DESC1 d;
				if (fac->EnumAdapters1(i, &a) != S_OK || a == NULL) { break; }
				if (SUCCEEDED(a->GetDesc1(&d)) && d.AdapterLuid.HighPart == c.luid.HighPart && d.AdapterLuid.LowPart == c.luid.LowPart) { ad = a; } else { a->Release(); }
			}
			fac->Release();
		}
	}
	D3D_FEATURE_LEVEL got;
	HRESULT hr = g_api.D3D11CreateDevice(ad, ad != NULL ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &dev, &got, NULL);
	if (ad != NULL) { ad->Release(); }
	if (!step("D3D11CreateDevice(VIDEO_SUPPORT)", hr)) { return hr; }
	{
		ID3D10Multithread *mt = NULL;
		if (SUCCEEDED(dev->QueryInterface(HW_IID(ID3D10Multithread), (void **)&mt)) && mt != NULL) { mt->SetMultithreadProtected(TRUE); mt->Release(); }
	}
	UINT token = 0;
	hr = g_api.MFCreateDXGIDeviceManager(&token, &mgr);
	if (!step("MFCreateDXGIDeviceManager", hr)) { return hr; }
	hr = mgr->ResetDevice(dev, token);
	if (!step("IMFDXGIDeviceManager::ResetDevice", hr)) { return hr; }
	hr = mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)mgr);
	step("ProcessMessage(SET_D3D_MANAGER)", hr);
	return hr;
}

static std::string type_summary(IMFMediaType *t)
{
	GUID sub;
	UINT32 w = 0, h = 0, v = 0;
	JObj o;
	if (SUCCEEDED(t->GetGUID(MF_MT_SUBTYPE, &sub))) { o.str("subtype", subtype_name(sub)); }
	if (SUCCEEDED(MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h))) { o.i("w", (long long)w).i("h", (long long)h); }
	if (SUCCEEDED(t->GetUINT32(MF_MT_MPEG2_PROFILE, &v))) { o.i("profile", (long long)v); }
	if (SUCCEEDED(t->GetUINT32(MF_MT_MPEG2_LEVEL, &v))) { o.i("level", (long long)v); }
	if (SUCCEEDED(t->GetUINT32(MF_MT_AVG_BITRATE, &v))) { o.i("avg_bitrate", (long long)v); }
	return o.done();
}

std::string Enc::avail_types(bool input)
{
	JArr a;
	for (DWORD i = 0; i < 24; i++)
	{
		IMFMediaType *t = NULL;
		const HRESULT hr = input ? mft->GetInputAvailableType(inId, i, &t) : mft->GetOutputAvailableType(outId, i, &t);
		if (FAILED(hr) || t == NULL)
		{
			if (i == 0) { a.raw(JObj().hex("hr", (unsigned long)hr).done()); }
			break;
		}
		a.raw(type_summary(t));
		t->Release();
	}
	return a.done();
}

HRESULT Enc::set_types(const EncCfg &c)
{
	HRESULT hr;
	IMFMediaType *o = NULL;
	if (c.capture) { outAvail = avail_types(false); }
	for (int attempt = 0; attempt < 2; attempt++)
	{
		o = NULL;
		hr = g_api.MFCreateMediaType(&o);
		if (FAILED(hr)) { return fail("MFCreateMediaType", hr); }
		o->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		o->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
		o->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)c.bitrate);
		MFSetAttributeSize(o, MF_MT_FRAME_SIZE, (UINT32)c.w, (UINT32)c.h);
		MFSetAttributeRatio(o, MF_MT_FRAME_RATE, 30, 1);
		MFSetAttributeRatio(o, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
		o->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		o->SetUINT32(MF_MT_MPEG2_PROFILE, (UINT32)c.profile);
		if (attempt == 0 && c.level > 0) { o->SetUINT32(MF_MT_MPEG2_LEVEL, (UINT32)c.level); }
		hr = mft->SetOutputType(outId, o, 0);
		o->Release();
		if (SUCCEEDED(hr)) { levelSet = (attempt == 0 && c.level > 0); step(attempt == 0 && c.level > 0 ? "SetOutputType(H264 with level)" : "SetOutputType(H264)", hr); break; }
		step(attempt == 0 && c.level > 0 ? "SetOutputType(H264 with level)" : "SetOutputType(H264)", hr);
		if (attempt == 0 && c.level > 0) { failStage.clear(); failHr = S_OK; continue; }
		return hr;
	}
	if (c.capture) { inAvail = avail_types(true); }
	IMFMediaType *in = NULL;
	hr = g_api.MFCreateMediaType(&in);
	if (FAILED(hr)) { return fail("MFCreateMediaType", hr); }
	in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
	MFSetAttributeSize(in, MF_MT_FRAME_SIZE, (UINT32)c.w, (UINT32)c.h);
	MFSetAttributeRatio(in, MF_MT_FRAME_RATE, 30, 1);
	MFSetAttributeRatio(in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	in->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)c.w);
	hr = mft->SetInputType(inId, in, 0);
	in->Release();
	if (FAILED(hr))
	{
		// the encoder's own NV12 type (it may want other attributes), when it offers one
		step("SetInputType(NV12)", hr);
		failStage.clear(); failHr = S_OK;
		for (DWORD i = 0; i < 24; i++)
		{
			IMFMediaType *t = NULL;
			GUID sub;
			if (FAILED(mft->GetInputAvailableType(inId, i, &t)) || t == NULL) { break; }
			if (SUCCEEDED(t->GetGUID(MF_MT_SUBTYPE, &sub)) && memcmp(&sub, &MFVideoFormat_NV12, sizeof(GUID)) == 0)
			{
				MFSetAttributeSize(t, MF_MT_FRAME_SIZE, (UINT32)c.w, (UINT32)c.h);
				MFSetAttributeRatio(t, MF_MT_FRAME_RATE, 30, 1);
				hr = mft->SetInputType(inId, t, 0);
				t->Release();
				return step("SetInputType(NV12 offered by the encoder)", hr) ? S_OK : hr;
			}
			t->Release();
		}
		return fail("SetInputType(NV12): no NV12 type", hr);
	}
	step("SetInputType(NV12)", hr);
	return S_OK;
}

// The settings that the plan asks for (3.1 item 5). A failure of the rate control or of the QP is returned (the mode is not usable), the others are only noted.
HRESULT Enc::apply_api(const EncCfg &c)
{
	if (api == NULL) { return E_NOINTERFACE; }
	apiFailHr = S_OK;
	note("AVLowLatencyMode=1", api_set_bool(api, CODECAPI_AVLowLatencyMode, 1));
	note("AVEncCommonLowLatency=1", api_set_bool(api, CODECAPI_AVEncCommonLowLatency, 1));
	note("AVEncMPVDefaultBPictureCount=0", api_set_u32(api, CODECAPI_AVEncMPVDefaultBPictureCount, 0));
	{
		VARIANT lo, hi, st;
		ULONG gop = 0xFFFFFFFFul;
		long long mx = 0;
		memset(&lo, 0, sizeof(lo)); memset(&hi, 0, sizeof(hi)); memset(&st, 0, sizeof(st));
		if (SUCCEEDED(api->GetParameterRange(&CODECAPI_AVEncMPVGOPSize, &lo, &hi, &st)) && var_to_i64(hi, &mx) && mx > 0 && mx < 0xFFFFFFFFLL) { gop = (ULONG)mx; }
		const HRESULT hr = api_set_u32(api, CODECAPI_AVEncMPVGOPSize, gop);
		char nm[48];
		_snprintf(nm, sizeof(nm), "AVEncMPVGOPSize=%lu", gop);
		note(nm, hr);
	}
	note("AVEncVideoMaxNumRefFrame=1", api_set_u32(api, CODECAPI_AVEncVideoMaxNumRefFrame, 1));
	HRESULT hr = S_OK;
	if (c.mode == MODE_NONE) { return S_OK; }	// inspection only: no rate control is set
	if (is_qp_mode(c.mode))
	{
		hr = api_set_u32(api, CODECAPI_AVEncCommonRateControlMode, HW_RC_QUALITY);
		note("AVEncCommonRateControlMode=Quality", hr);
		if (FAILED(hr)) { apiFailHr = hr; return hr; }
		hr = set_qp(c.qp);
		if (FAILED(hr)) { apiFailHr = hr; return hr; }
	}
	else
	{
		const ULONG rc = c.mode == MODE_CBR ? HW_RC_CBR : (c.mode == MODE_LDVBR ? HW_RC_LDVBR : HW_RC_PEAK);
		hr = api_set_u32(api, CODECAPI_AVEncCommonRateControlMode, rc);
		note(c.mode == MODE_CBR ? "AVEncCommonRateControlMode=CBR" : (c.mode == MODE_LDVBR ? "AVEncCommonRateControlMode=LowDelayVBR" : "AVEncCommonRateControlMode=PeakConstrainedVBR"), hr);
		if (FAILED(hr)) { apiFailHr = hr; return hr; }
		hr = set_rate(c.rateBps);
		if (FAILED(hr)) { apiFailHr = hr; return hr; }
		if (c.mode == MODE_PEAKVBR) { note("AVEncCommonMaxBitRate", api_set_u32(api, CODECAPI_AVEncCommonMaxBitRate, (ULONG)c.rateBps)); }
	}
	return S_OK;
}

HRESULT Enc::set_qp(int qp)
{
	HRESULT hr = E_NOTIMPL;
	char nm[64];
	if (api == NULL) { return E_NOINTERFACE; }
	if (qp < 0) { qp = 0; }
	if (qp > 51) { qp = 51; }
	switch (cfg.mode)
	{
		case MODE_QP_UI8: { const ULONGLONG v = (ULONGLONG)qp | ((ULONGLONG)qp << 16) | ((ULONGLONG)qp << 32); hr = api_set_u64(api, CODECAPI_AVEncVideoEncodeQP, v); _snprintf(nm, sizeof(nm), "AVEncVideoEncodeQP(UI8)=%d", qp); break; }
		case MODE_QP_UI4: hr = api_set_u32(api, CODECAPI_AVEncVideoEncodeQP, (ULONG)qp); _snprintf(nm, sizeof(nm), "AVEncVideoEncodeQP(UI4)=%d", qp); break;
		case MODE_QP_FRAMETYPE: { const ULONGLONG v = (ULONGLONG)qp | ((ULONGLONG)qp << 16) | ((ULONGLONG)qp << 32); hr = api_set_u64(api, CODECAPI_AVEncVideoEncodeFrameTypeQP, v); _snprintf(nm, sizeof(nm), "AVEncVideoEncodeFrameTypeQP=%d", qp); break; }
		case MODE_QP_QUALITY: hr = api_set_u32(api, CODECAPI_AVEncCommonQuality, (ULONG)(100 - qp * 100 / 51)); _snprintf(nm, sizeof(nm), "AVEncCommonQuality(qp %d)", qp); break;
		default: return E_NOTIMPL;
	}
	note(nm, hr);
	return hr;
}

HRESULT Enc::set_rate(int bps)
{
	char nm[64];
	if (api == NULL) { return E_NOINTERFACE; }
	const HRESULT hr = api_set_u32(api, CODECAPI_AVEncCommonMeanBitRate, (ULONG)bps);
	_snprintf(nm, sizeof(nm), "AVEncCommonMeanBitRate=%d", bps);
	note(nm, hr);
	return hr;
}

HRESULT Enc::force_idr()
{
	if (api == NULL) { return E_NOINTERFACE; }
	const HRESULT hr = api_set_u32(api, CODECAPI_AVEncVideoForceKeyFrame, 1);
	note("AVEncVideoForceKeyFrame=1", hr);
	return hr;
}

void Enc::read_output_type()
{
	IMFMediaType *t = NULL;
	if (mft != NULL && SUCCEEDED(mft->GetOutputCurrentType(outId, &t)) && t != NULL)
	{
		UINT32 v = 0;
		UINT8 *blob = NULL;
		UINT32 sz = 0;
		outTypeNow = type_summary(t);
		if (SUCCEEDED(t->GetUINT32(MF_MT_MPEG2_PROFILE, &v))) { outProfile = (int)v; }
		if (SUCCEEDED(t->GetUINT32(MF_MT_MPEG2_LEVEL, &v))) { outLevel = (int)v; }
		if (SUCCEEDED(t->GetAllocatedBlob(MF_MT_MPEG_SEQUENCE_HEADER, &blob, &sz)) && blob != NULL) { seqHdr.assign(blob, blob + sz); g_api.CoTaskMemFree(blob); }
		t->Release();
	}
}

HRESULT Enc::open(const Cand &c, const EncCfg &cf)
{
	cfg = cf;
	outLevel = outProfile = 0;
	levelSet = false;
	act = fresh_activate(c);
	if (act == NULL) { return fail("enumerate again (the encoder is gone)", E_FAIL); }
	HRESULT hr = act->ActivateObject(HW_IID(IMFTransform), (void **)&mft);
	if (!step("ActivateObject", hr)) { return hr; }
	{
		IMFAttributes *at = NULL;
		UINT32 v = 0;
		if (SUCCEEDED(mft->GetAttributes(&at)) && at != NULL)
		{
			if (SUCCEEDED(at->GetUINT32(MF_TRANSFORM_ASYNC, &v)) && v != 0) { async = true; }
			if (SUCCEEDED(at->GetUINT32(MF_SA_D3D11_AWARE, &v)) && v != 0) { d3dAware = true; }
			if (async) { hr = at->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE); step("MF_TRANSFORM_ASYNC_UNLOCK", hr); }
			at->Release();
			if (async && FAILED(hr)) { return hr; }
		}
		else { step("IMFTransform::GetAttributes", E_FAIL); failStage.clear(); failHr = S_OK; }
	}
	if (SUCCEEDED(mft->QueryInterface(HW_IID(ICodecAPI), (void **)&api))) { step("QueryInterface(ICodecAPI)", S_OK); } else { api = NULL; steps.push_back(std::make_pair(std::string("QueryInterface(ICodecAPI)"), (HRESULT)E_NOINTERFACE)); }
	if (async)
	{
		if (!step("QueryInterface(IMFMediaEventGenerator)", mft->QueryInterface(HW_IID(IMFMediaEventGenerator), (void **)&gen))) { return failHr; }
		pump = new EventPump();
		if (!pump->start(gen)) { return fail("start of the event thread", E_FAIL); }
	}
	hr = mft->GetStreamIDs(1, &inId, 1, &outId);
	if (hr == E_NOTIMPL) { inId = outId = 0; hr = S_OK; }
	step("GetStreamIDs", hr);
	failStage.clear(); failHr = S_OK;
	if (cfg.capture && api != NULL) { propsFresh = props_json(api, NULL); }
	if (cfg.d3d) { hr = setup_d3d(c); if (FAILED(hr)) { return hr; } }
	// the software encoder of Microsoft takes its properties BEFORE the media types (that is the order of mf_h264.cpp; set after them it accepts them and then writes B slices and two
	// reference frames: the AMD laptop run of 2026-11-14 measured it that way by mistake); a hardware encoder gets them after the types unless the caller asks for the other order
	if (cfg.apiFirst || !c.hw)
	{
		hr = apply_api(cfg);
		if (FAILED(hr)) { return fail("codec API settings", hr); }
		hr = set_types(cfg);
		if (FAILED(hr)) { return hr; }
	}
	else
	{
		hr = set_types(cfg);
		if (FAILED(hr)) { return hr; }
		hr = apply_api(cfg);
		if (FAILED(hr)) { return fail("codec API settings", hr); }
	}
	read_output_type();
	if (cfg.capture && api != NULL)
	{
		propsTyped = props_json(api, NULL);
		// the rate control modes that SetValue accepts (set one after the other, read back); the last one stays set, the session is only inspected
		JArr a;
		static const char *rcn[] = { "CBR", "PeakConstrainedVBR", "UnconstrainedVBR", "Quality", "LowDelayVBR", "GlobalVBR", "GlobalLowDelayVBR" };
		rcText.clear();
		for (int m = 0; m < 7; m++)
		{
			VARIANT v;
			long long back = -1;
			const HRESULT h = api_set_u32(api, CODECAPI_AVEncCommonRateControlMode, (ULONG)m);
			memset(&v, 0, sizeof(v));
			if (SUCCEEDED(h) && SUCCEEDED(api->GetValue(&CODECAPI_AVEncCommonRateControlMode, &v))) { var_to_i64(v, &back); }
			a.raw(JObj().str("mode", rcn[m]).b("accepted", SUCCEEDED(h)).hex("hr", (unsigned long)h).i("read_back", back).done());
			rcText += std::string(m ? ", " : "") + rcn[m] + (SUCCEEDED(h) ? (back == m ? "=yes" : "=yes (read back differs)") : "=no");
		}
		rcModes = a.done();
	}
	{
		MFT_OUTPUT_STREAM_INFO osi;
		MFT_INPUT_STREAM_INFO isi;
		memset(&osi, 0, sizeof(osi)); memset(&isi, 0, sizeof(isi));
		hr = mft->GetOutputStreamInfo(outId, &osi);
		step("GetOutputStreamInfo", hr);
		provides = SUCCEEDED(hr) && (osi.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
		outCap = std::max<DWORD>(SUCCEEDED(hr) ? osi.cbSize : 0, (DWORD)cfg.w * (DWORD)cfg.h * 3);
		mft->GetInputStreamInfo(inId, &isi);
		failStage.clear(); failHr = S_OK;
	}
	hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
	step("NOTIFY_BEGIN_STREAMING", hr);
	hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
	if (!step("NOTIFY_START_OF_STREAM", hr)) { return hr; }
	return S_OK;
}

void Enc::close()
{
	if (mft != NULL)
	{
		mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
		mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
		mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
	}
	if (pump != NULL) { pump->request_stop(); }
	if (act != NULL) { act->ShutdownObject(); }
	if (pump != NULL) { pump->release_by_session(150); pump = NULL; }
	rel(outSample);
	rel(gen); rel(api); rel(mft); rel(act); rel(mgr); rel(dev);
}

// ---------------------------------------------------------------------------------------------------------------------- driving the encoder

// One call of ProcessOutput. *got = 1 and the access unit in *data when a sample came out (all parts of an incomplete output joined). MF_E_TRANSFORM_STREAM_CHANGE is followed here
// (the new output type is the first available type with the subtype H.264) and returned so that the caller counts it (an asynchronous MFT used the event of the output up for it: the caller waits for the next HaveOutput; a
// synchronous one is asked again); MF_E_TRANSFORM_NEED_MORE_INPUT means nothing is ready.
HRESULT Enc::pull_one(std::vector<uint8_t> *data, OutRec *rec, bool *got)
{
	*got = false;
	data->clear();
	memset(rec, 0, sizeof(*rec));
	for (int part = 0; part < 8; part++)
	{
		MFT_OUTPUT_DATA_BUFFER db;
		memset(&db, 0, sizeof(db));
		db.dwStreamID = outId;
		if (!provides)
		{
			if (outSample == NULL)
			{
				IMFMediaBuffer *b = NULL;
				HRESULT h = g_api.MFCreateSample(&outSample);
				if (SUCCEEDED(h)) { h = g_api.MFCreateMemoryBuffer(outCap, &b); }
				if (SUCCEEDED(h)) { h = outSample->AddBuffer(b); }
				rel(b);
				if (FAILED(h)) { rel(outSample); return h; }
			}
			IMFMediaBuffer *b0 = NULL;
			if (SUCCEEDED(outSample->GetBufferByIndex(0, &b0)) && b0 != NULL) { b0->SetCurrentLength(0); b0->Release(); }
			db.pSample = outSample;
		}
		DWORD status = 0;
		const HRESULT hr = mft->ProcessOutput(0, 1, &db, &status);
		if (db.pEvents != NULL) { db.pEvents->Release(); db.pEvents = NULL; }
		if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
		{
			if (provides && db.pSample != NULL) { db.pSample->Release(); }
			// the first available type with the subtype H.264 (an MFT may list another subtype first or as well)
			IMFMediaType *t = NULL;
			HRESULT h2 = E_FAIL;
			scTypeIdx = -1; scTypeN = 0; scSubtype.clear();
			for (int ti = 0; ti < 32; ti++)
			{
				IMFMediaType *c = NULL;
				GUID sub;
				if (FAILED(mft->GetOutputAvailableType(outId, (DWORD)ti, &c)) || c == NULL) { break; }
				scTypeN++;
				if (t == NULL && SUCCEEDED(c->GetGUID(MF_MT_SUBTYPE, &sub)) && memcmp(&sub, &MFVideoFormat_H264, sizeof(GUID)) == 0) { t = c; scTypeIdx = ti; scSubtype = subtype_name(sub); }
				else { c->Release(); }
			}
			if (t != NULL) { h2 = mft->SetOutputType(outId, t, 0); t->Release(); }
			scSetHr = h2;
			steps.push_back(std::make_pair(std::string(scTypeIdx >= 0 ? "STREAM_CHANGE: SetOutputType(first H.264 type)" : "STREAM_CHANGE: no H.264 output type"), h2));
			read_output_type();
			MFT_OUTPUT_STREAM_INFO osi;
			memset(&osi, 0, sizeof(osi));
			if (SUCCEEDED(mft->GetOutputStreamInfo(outId, &osi))) { provides = (osi.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0; if (!provides && osi.cbSize > outCap) { outCap = osi.cbSize; rel(outSample); } }
			return hr;
		}
		if (FAILED(hr)) { if (provides && db.pSample != NULL) { db.pSample->Release(); } return hr; }
		IMFSample *s = db.pSample;
		if (s == NULL) { return E_POINTER; }
		IMFMediaBuffer *buf = NULL;
		if (SUCCEEDED(s->ConvertToContiguousBuffer(&buf)) && buf != NULL)
		{
			BYTE *p = NULL;
			DWORD mx = 0, cur = 0;
			if (SUCCEEDED(buf->Lock(&p, &mx, &cur)) && p != NULL) { data->insert(data->end(), p, p + cur); buf->Unlock(); }
			buf->Release();
		}
		if (part == 0)
		{
			LONGLONG t = 0;
			UINT32 v = 0;
			UINT64 q = 0;
			if (SUCCEEDED(s->GetSampleTime(&t))) { rec->pts = t; } else { rec->pts = -1; }
			if (SUCCEEDED(s->GetUINT32(MFSampleExtension_CleanPoint, &v)) && v != 0) { rec->clean = true; }
			if (SUCCEEDED(s->GetUINT64(MFSampleExtension_VideoEncodeQP, &q))) { rec->hasQp = true; rec->qpRaw = q; }
		}
		if (provides) { s->Release(); }
		*got = true;
		if (!(db.dwStatus & MFT_OUTPUT_DATA_BUFFER_INCOMPLETE)) { return S_OK; }
	}
	return S_OK;
}

bool Enc::record_out(const RunReq &rq, RunRes *res, std::vector<uint8_t> &au, OutRec &rec)
{
	const size_t idx = res->outs.size();
	struct kvmh_au_info in;
	kvmh_au_info(au.empty() ? NULL : &au[0], au.size(), &in);
	rec.bytes = au.size();
	rec.idr = in.idr; rec.bSlices = in.bSlices; rec.types = in.types;
	rec.outMs = hw_now_ms();
	rec.submitMs = idx < res->submit.size() ? res->submit[idx] : rec.outMs;
	if (rec.pts >= 0 && rec.pts != (long long)idx * 333333LL) { res->ptsMismatch++; }
	if (rq.rules != NULL) { kvmh_au(rq.rules, au.empty() ? NULL : &au[0], au.size(), -1); }
	if (rq.keep != NULL && ((int)idx < rq.keepMax || (rq.keepFrom >= 0 && (int)idx >= rq.keepFrom && (int)idx < rq.keepFrom + 3)))
	{
		rq.keep->push_back(au);
		res->keptIndex.push_back((int)idx);
	}
	res->outs.push_back(rec);
	return true;
}

void Enc::run(const RunReq &rq, RunRes *res)
{
	const double w0 = hw_now_ms(), c0 = hw_cpu_ms();
	const size_t len = rq.gen->nv12_size();
	int submitted = 0, credits = 0, pendingOut = 0, notAcceptingRun = 0;
	bool drainSent = false;
	double drainAt = 0;
	double scAt = 0;		// the time of a stream change that no HaveOutput has followed yet (0 = none)
	std::vector<uint8_t> au;
	OutRec rec;
	res->submit.reserve((size_t)rq.frames);
	for (;;)
	{
		const double now = hw_now_ms();
		if (now > rq.endMs) { res->timedOut = true; break; }
		if (drainSent && now > drainAt + 3000.0) { res->drainTimedOut = true; break; }
		if (res->outs.empty() && now > w0 + 4000.0 && submitted > 0) { res->timedOut = true; res->err = HRESULT_FROM_WIN32(ERROR_TIMEOUT); res->errStage = "no output within 4 s of the first picture (the encoder stalled)"; break; }
		if (submitted == 0 && now > w0 + 4000.0) { res->timedOut = true; res->err = HRESULT_FROM_WIN32(ERROR_TIMEOUT); res->errStage = "the encoder did not ask for a picture within 4 s (no METransformNeedInput)"; break; }
		if (async)
		{
			EventPump::Ev ev;
			if (credits == 0 && pendingOut == 0) { pump->wait(25); }
			while (pump->pop(&ev))
			{
				if (ev.type == METransformNeedInput) { credits++; res->needInput++; if (scAt != 0) { res->scNeedWait++; } }
				else if (ev.type == METransformHaveOutput) { pendingOut++; res->haveOutput++; if (submitted == 0) { res->haveBeforeInput++; } if (scAt != 0) { res->scEvent++; const double w = hw_now_ms() - scAt; if (w > res->scWaitMax) { res->scWaitMax = w; } scAt = 0; } }
				else if (ev.type == METransformDrainComplete) { res->drained = true; }
				else if (ev.type == MEError) { res->err = FAILED(ev.status) ? ev.status : E_FAIL; res->errStage = "MEError event"; }
				else if (ev.type == 0xFFFF) { if (!drainSent || !res->drained) { res->err = ev.status; res->errStage = "GetEvent (the event queue ended)"; } }
			}
			if (FAILED(res->err)) { break; }
			// a stream change used the event of its output up: the next ProcessOutput waits for the next HaveOutput; an MFT that sends none gets one call without it after 500 ms
			if (scAt != 0 && pendingOut == 0 && hw_now_ms() > scAt + 500.0) { pendingOut = 1; res->scNoEvent++; if (500.0 > res->scWaitMax) { res->scWaitMax = 500.0; } scAt = 0; }
		}
		// submit
		bool progressed = false;
		while (submitted < rq.frames && !drainSent && (!async || credits > 0))
		{
			for (size_t k = 0; k < rq.changes.size(); k++)
			{
				if (rq.changes[k].at == submitted) { res->changeHr.push_back(rq.changes[k].kind == 1 ? set_qp(rq.changes[k].value) : set_rate(rq.changes[k].value)); }
			}
			if (rq.forceAt == submitted) { res->forceHr = force_idr(); res->forceOutIndex = submitted; }
			IMFSample *s = NULL;
			IMFMediaBuffer *b = NULL;
			const double g0 = hw_now_ms();
			HRESULT hr = g_api.MFCreateSample(&s);
			if (SUCCEEDED(hr)) { hr = g_api.MFCreateMemoryBuffer((DWORD)len, &b); }
			if (SUCCEEDED(hr))
			{
				BYTE *p = NULL;
				DWORD mx = 0, cur = 0;
				hr = b->Lock(&p, &mx, &cur);
				if (SUCCEEDED(hr)) { rq.gen->frame(rq.content, submitted, p); b->Unlock(); b->SetCurrentLength((DWORD)len); }
			}
			if (SUCCEEDED(hr)) { hr = s->AddBuffer(b); }
			if (SUCCEEDED(hr)) { s->SetSampleTime((LONGLONG)submitted * 333333LL); s->SetSampleDuration(333333LL); }
			res->sourceMs += hw_now_ms() - g0;
			if (FAILED(hr)) { rel(b); rel(s); res->err = hr; res->errStage = "building the input sample"; break; }
			const double t = hw_now_ms();
			hr = mft->ProcessInput(inId, s, 0);
			rel(b); rel(s);
			if (hr == MF_E_NOTACCEPTING)
			{
				res->notAccepting++;
				if (++notAcceptingRun > 2000) { res->err = hr; res->errStage = "ProcessInput kept answering NOTACCEPTING"; break; }
				if (async) { credits = 0; } else { /* pull the output first (below) and try this picture again */ }
				break;
			}
			if (FAILED(hr)) { res->err = hr; res->errStage = "ProcessInput"; break; }
			notAcceptingRun = 0;
			res->submit.push_back(t);
			submitted++;
			progressed = true;
			if (async) { credits--; } else { break; }	// a synchronous encoder: its output is pulled after every picture
		}
		if (FAILED(res->err)) { break; }
		res->submitted = submitted;
		// outputs
		if (async)
		{
			while (pendingOut > 0)
			{
				bool got = false;
				const HRESULT hr = pull_one(&au, &rec, &got);
				if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { res->streamChanges++; res->scSetHr = scSetHr; res->scTypeIdx = scTypeIdx; res->scTypeN = scTypeN; res->scSubtype = scSubtype; pendingOut--; scAt = pendingOut == 0 ? hw_now_ms() : 0; break; }
				pendingOut--;
				if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) { continue; }
				if (FAILED(hr)) { res->err = hr; res->errStage = "ProcessOutput"; break; }
				if (got) { record_out(rq, res, au, rec); progressed = true; scAt = 0; }
			}
		}
		else
		{
			for (int k = 0; k < 16; k++)
			{
				bool got = false;
				const HRESULT hr = pull_one(&au, &rec, &got);
				if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { res->streamChanges++; continue; }
				if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) { break; }
				if (FAILED(hr)) { res->err = hr; res->errStage = "ProcessOutput"; break; }
				if (got) { record_out(rq, res, au, rec); progressed = true; }
			}
		}
		if (FAILED(res->err)) { break; }
		{ const int infl = submitted - (int)res->outs.size(); if (infl > res->maxInflight) { res->maxInflight = infl; } }
		if (submitted >= rq.frames && !drainSent)
		{
			drainSent = true;
			drainAt = hw_now_ms();
			const HRESULT hr = mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
			steps.push_back(std::make_pair(std::string("COMMAND_DRAIN"), hr));
		}
		if (drainSent && (int)res->outs.size() >= submitted) { break; }
		if (!async && !progressed && drainSent) { break; }	// nothing more comes out of a synchronous encoder that was drained
		if (!async && !progressed && !drainSent) { Sleep(1); }
	}
	res->wallMs = hw_now_ms() - w0;
	res->cpuMs = hw_cpu_ms() - c0;
	res->submitted = submitted;
}

// ---------------------------------------------------------------------------------------------------------------------- results shared between stages

struct ModeResult { bool opened, effective, stalled; double p[3]; int rate; };
struct ModeChoice { bool known; int mode; bool effective; std::string note; ModeChoice() : known(false), mode(MODE_NONE), effective(false) {} };
static std::map<std::string, std::map<int, ModeResult> > g_modeResults;   // candidate key -> mode -> what qpmid found
static std::map<std::string, ModeChoice> g_modes;
static std::map<std::string, bool> g_d3dAware;
static HwLock g_resLock;

struct RunSummary { std::string key, label; double p50, p95, max, fps, cpuPerFrame, pMean, idrBytes; bool ok; int rulesFailed; };
static std::vector<RunSummary> g_summaries;
static std::map<std::string, int> g_benchFailRun;   // consecutive runs of a candidate that failed (open refused, no output, stalled): after 2 the rest of its runs is skipped

static std::map<std::string, HwGen *> g_gens;
static HwGen *get_gen(int w, int h)
{
	HwGuard g(g_resLock);
	char k[32];
	_snprintf(k, sizeof(k), "%dx%d", w, h);
	std::map<std::string, HwGen *>::iterator it = g_gens.find(k);
	if (it != g_gens.end()) { return it->second; }
	HwGen *gen = new HwGen();
	if (!gen->init(w, h)) { delete gen; return NULL; }
	g_gens[k] = gen;
	return gen;
}

// ---------------------------------------------------------------------------------------------------------------------- probe: describe

struct DescArg { Cand c; };

static void describe_session(const Cand &c, const EncCfg &cfg, const char *label, JArr *out)
{
	Enc e;
	const double t0 = hw_now_ms();
	const HRESULT hr = e.open(c, cfg);
	JObj o;
	o.str("what", label).b("opened", SUCCEEDED(hr)).hex("hr", (unsigned long)hr).str("fail_stage", e.failStage).d("open_ms", hw_now_ms() - t0, 0);
	if (SUCCEEDED(hr))
	{
		o.i("profile_in_output_type", e.outProfile).i("level_in_output_type", e.outLevel).b("level_accepted_in_type", e.levelSet);
		if (!e.seqHdr.empty()) { o.i("sequence_header_bytes", (long long)e.seqHdr.size()); }
	}
	o.raw("steps", e.steps_json());
	if (out != NULL) { out->raw(o.done()); }
	hw_say("    %-34s %s  %s%s\n", label, SUCCEEDED(hr) ? "accepted" : "REFUSED ", e.failStage.empty() ? "" : e.failStage.c_str(), SUCCEEDED(hr) ? "" : (std::string(" ") + hr_name(hr)).c_str());
	e.close();
}

static void stage_describe(void *p)
{
	DescArg *a = (DescArg *)p;
	const Cand &c = a->c;
	JObj r;
	hw_say("\n== %s: %s (vendor %s)\n", c.key.c_str(), c.name.c_str(), c.vendor.empty() ? "-" : c.vendor.c_str());
	r.raw("candidate", cand_json(c));
	{
		EncCfg cfg;
		cfg.mode = MODE_NONE; cfg.capture = true;
		Enc e;
		const double t0 = hw_now_ms();
		const HRESULT hr = e.open(c, cfg);
		r.b("opened", SUCCEEDED(hr)).hex("open_hr", (unsigned long)hr).str("fail_stage", e.failStage).d("open_ms", hw_now_ms() - t0, 0);
		r.b("async", e.async).b("d3d11_aware", e.d3dAware).b("provides_samples", e.provides).i("output_buffer_bytes", (long long)e.outCap);
		r.raw("steps", e.steps_json()).raw("codec_api_sets", e.api_json());
		if (!e.outAvail.empty()) { r.raw("output_types_available", e.outAvail); }
		if (!e.inAvail.empty()) { r.raw("input_types_available", e.inAvail); }
		if (!e.outTypeNow.empty()) { r.raw("output_type_now", e.outTypeNow); }
		if (!e.propsFresh.empty()) { r.raw("codec_api_before_types", e.propsFresh); }
		if (!e.propsTyped.empty()) { r.raw("codec_api_after_types", e.propsTyped); }
		if (!e.rcModes.empty()) { r.raw("rate_control_modes", e.rcModes); }
		{ HwGuard g(g_resLock); g_d3dAware[c.key] = e.d3dAware; }
		hw_say("  open 1920x1088 High: %s (%.0f ms)  async=%d  D3D11 aware=%d  provides its own output samples=%d%s%s\n", SUCCEEDED(hr) ? "ok" : "REFUSED", hw_now_ms() - t0, e.async, e.d3dAware, e.provides,
			e.failStage.empty() ? "" : "  at ", e.failStage.c_str());
		if (SUCCEEDED(hr) && !e.propsTyped.empty())
		{
			// the lines of the table that matter for the plan
			hw_say("  codec API (after the media types), supported properties: ");
			int ns = 0;
			for (int i = 0; i < N_PROPS; i++) { if (e.api->IsSupported(g_props[i].guid) == S_OK) { hw_say("%s%s", ns ? ", " : "", g_props[i].name); ns++; } }
			hw_say("%s\n", ns ? "" : "(none)");
			hw_say("  not supported:");
			for (int i = 0; i < N_PROPS; i++) { if (e.api->IsSupported(g_props[i].guid) != S_OK) { hw_say(" %s", g_props[i].name); } }
			hw_say("\n");
		}
		if (!e.rcText.empty()) { hw_say("  rate control modes (SetValue): %s\n", e.rcText.c_str()); }
		if (!e.seqHdr.empty()) { r.str("sequence_header_hex", hw_hex(&e.seqHdr[0], e.seqHdr.size(), 64)); }
		e.close();
	}
	{
		hw_say("  profiles (1920x1088, level 4.2 asked):\n");
		JArr a;
		static const int profs[] = { 100, 77, 66 };
		for (int i = 0; i < 3; i++)
		{
			EncCfg cfg;
			cfg.mode = MODE_NONE; cfg.profile = profs[i];
			describe_session(c, cfg, profs[i] == 100 ? "High (100)" : profs[i] == 77 ? "Main (77)" : "Baseline (66)", &a);
		}
		r.raw("profiles", a.done());
	}
	{
		hw_say("  sizes (High, level of the size at 30 fps):\n");
		JArr a;
		static const int sz[][2] = { { 1280, 736 }, { 1920, 1088 }, { 1920, 1216 }, { 2560, 1440 }, { 3840, 2176 } };
		for (int i = 0; i < 5; i++)
		{
			EncCfg cfg;
			char lab[48];
			cfg.mode = MODE_NONE; cfg.w = sz[i][0]; cfg.h = sz[i][1]; cfg.level = mfh264_level_for_size(cfg.w, cfg.h);
			_snprintf(lab, sizeof(lab), "%dx%d level_idc %d", cfg.w, cfg.h, cfg.level);
			describe_session(c, cfg, lab, &a);
		}
		r.raw("sizes", a.done());
	}
	if (!g_cfg.noD3d && c.hw)
	{
		hw_say("  with the D3D manager (a D3D11 device with VIDEO_SUPPORT on the adapter of the encoder):\n");
		JArr a;
		EncCfg cfg;
		cfg.mode = MODE_NONE; cfg.d3d = true;
		describe_session(c, cfg, "SET_D3D_MANAGER, NV12 in system memory", &a);
		r.raw("d3d_manager", a.done());
	}
	{
		hw_say("  codec API set before the media types (order B):\n");
		JArr a;
		EncCfg cfg;
		cfg.mode = MODE_NONE; cfg.apiFirst = true;
		describe_session(c, cfg, "settings first, then the types", &a);
		r.raw("order_api_first", a.done());
	}
	JObj outer;
	outer.str("key", c.key).raw("describe", r.done());
	g_doc.append("candidates_probe", outer.done());
	delete a;
}

// ---------------------------------------------------------------------------------------------------------------------- probe: qpmid (a setting changed in the middle of the stream)

struct QpArg { Cand c; int mode; };

static double mean_p(const RunRes &r, int from, int to)
{
	HwSeries s;
	for (int i = from; i < to && i < (int)r.outs.size(); i++) { if (!r.outs[i].idr) { s.add((double)r.outs[i].bytes); } }
	return s.mean();
}

static void stage_qpmid(void *p)
{
	QpArg *a = (QpArg *)p;
	const Cand &c = a->c;
	const bool qp = is_qp_mode(a->mode);
	EncCfg cfg;
	cfg.mode = a->mode; cfg.qp = 20; cfg.rateBps = 600000; cfg.bitrate = 600000;
	HwGen *gen = get_gen(1920, 1088);
	JObj o;
	ModeResult mr;
	memset(&mr, 0, sizeof(mr));
	o.str("key", c.key).str("mode", mode_id(a->mode)).str("mode_text", mode_name(a->mode));
	if (gen == NULL) { o.str("error", "pictures"); g_doc.append("qpmid", o.done()); delete a; return; }
	Enc e;
	const HRESULT hr = e.open(c, cfg);
	o.b("opened", SUCCEEDED(hr)).hex("hr", (unsigned long)hr).str("fail_stage", e.failStage);
	if (FAILED(hr))
	{
		o.raw("steps", e.steps_json()).raw("codec_api_sets", e.api_json());
		hw_say("    %-34s refused at %s (%s)\n", mode_name(a->mode), e.failStage.c_str(), hr_name(hr)[0] ? hr_name(hr) : "hr");
	}
	else
	{
		RunReq rq;
		RunRes rr;
		rq.gen = gen; rq.content = HW_CONTENT_SCROLL; rq.frames = 36; rq.endMs = std::min(hw_now_ms() + 10000.0, g_deadline);
		Change c1, c2;
		c1.at = 12; c1.kind = qp ? 1 : 2; c1.value = qp ? 40 : 4000000;
		c2.at = 24; c2.kind = qp ? 1 : 2; c2.value = qp ? 30 : 1000000;
		rq.changes.push_back(c1); rq.changes.push_back(c2);
		e.run(rq, &rr);
		const double m0 = mean_p(rr, 3, 12), m1 = mean_p(rr, 15, 24), m2 = mean_p(rr, 27, 36);
		mr.opened = true; mr.stalled = rr.outs.empty(); mr.p[0] = m0; mr.p[1] = m1; mr.p[2] = m2;
		if (qp) { mr.effective = m0 > 1.4 * m1 && m2 < m0 && m2 > m1; }
		else { mr.effective = m1 > 1.4 * m0 && m2 > m0 && m2 < m1; }
		JArr rep;
		for (int ph = 0; ph < 3; ph++)
		{
			const int last = ph * 12 + 11;
			if (last < (int)rr.outs.size() && rr.outs[last].hasQp) { char b[32]; _snprintf(b, sizeof(b), "0x%llX", rr.outs[last].qpRaw); rep.str(b); } else { rep.str("none"); }
		}
		JArr chg;
		for (size_t i = 0; i < rr.changeHr.size(); i++) { chg.raw(hr_json(rr.changeHr[i])); }
		o.i("outputs", (long long)rr.outs.size()).d("p_bytes_phase1", m0, 0).d("p_bytes_phase2", m1, 0).d("p_bytes_phase3", m2, 0).b("effective", mr.effective).raw("reported_qp_last_of_phase", rep.done())
			.raw("changes_hr", chg.done()).b("timed_out", rr.timedOut);
		if (!rr.outs.empty()) { o.i("first_bytes", (long long)rr.outs[0].bytes).b("first_is_idr", rr.outs[0].idr != 0); }
		if (FAILED(rr.err)) { o.str("run_error", rr.errStage).hex("run_error_hr", (unsigned long)rr.err); }
		o.raw("codec_api_sets", e.api_json());
		hw_say("    %-34s P bytes %7.0f -> %7.0f -> %7.0f (%s %s %s)   reported %s   %s%s\n", mode_name(a->mode), m0, m1, m2, qp ? "QP 20" : "600k", qp ? "40" : "4M", qp ? "30" : "1M",
			rep.done().c_str(), mr.effective ? "EFFECTIVE" : "no effect", FAILED(rr.err) ? ("  error: " + rr.errStage).c_str() : "", rr.outs.size() < 36 ? "  (fewer outputs than pictures)" : "");
	}
	e.close();
	{ HwGuard g(g_resLock); g_modeResults[c.key][a->mode] = mr; }
	g_doc.append("qpmid", o.done());
	delete a;
}

// The way of steering the encoder that the bench uses: the first effective constant QP form, else the first effective rate mode, else the first form that was accepted (marked as not effective).
static ModeChoice choose_mode(const std::string &key)
{
	ModeChoice ch;
	HwGuard g(g_resLock);
	std::map<int, ModeResult> &m = g_modeResults[key];
	static const int order[] = { MODE_QP_UI8, MODE_QP_UI4, MODE_QP_FRAMETYPE, MODE_QP_QUALITY, MODE_CBR, MODE_LDVBR, MODE_PEAKVBR };
	for (int pass = 0; pass < 2 && !ch.known; pass++)
	{
		for (size_t i = 0; i < sizeof(order) / sizeof(order[0]) && !ch.known; i++)
		{
			std::map<int, ModeResult>::iterator it = m.find(order[i]);
			if (it != m.end() && it->second.opened && (pass == 1 || it->second.effective)) { ch.known = true; ch.mode = order[i]; ch.effective = it->second.effective; }
		}
	}
	if (ch.known) { ch.note = ch.effective ? "the first form that changes the size of the pictures" : "no form changed the size of the pictures; the first form that was accepted"; }
	else { ch.note = "no form of rate control was accepted"; }
	return ch;
}

static const char *mode_for_label(const ModeChoice &c) { return c.known ? mode_id(c.mode) : "none"; }

static void qpmid_candidate(const Cand &c, bool full);

static void probe_candidate(const Cand &c, bool full)
{
	DescArg *d = new DescArg;
	d->c = c;
	char lab[48];
	_snprintf(lab, sizeof(lab), "describe %s", c.key.c_str());
	run_stage(lab, 40000, stage_describe, d, c.hw);	// d is not freed here: an abandoned stage still uses it
	if (c.hw && g_hwTainted) { return; }
	qpmid_candidate(c, full);
}

static void qpmid_candidate(const Cand &c, bool full)
{
	char lab[48];
	hw_say("  steering the quantizer, changed in the middle of the stream (3 phases of 12 pictures, 1920x1088 scroll):\n");
	static const int forms[] = { MODE_QP_UI8, MODE_QP_UI4, MODE_QP_FRAMETYPE, MODE_QP_QUALITY, MODE_CBR, MODE_LDVBR, MODE_PEAKVBR };
	int stalledRun = 0;
	for (size_t i = 0; i < sizeof(forms) / sizeof(forms[0]); i++)
	{
		if (stalledRun >= 2) { hw_say("    (the last 2 forms gave no output at all: the other forms are not tried)\n"); break; }
		if (!full && forms[i] != MODE_QP_UI8 && forms[i] != MODE_QP_UI4 && forms[i] != MODE_CBR) { continue; }
		QpArg *q = new QpArg;
		q->c = c; q->mode = forms[i];
		_snprintf(lab, sizeof(lab), "qpmid %s %s", c.key.c_str(), mode_id(forms[i]));
		if (run_stage(lab, 20000, stage_qpmid, q, c.hw) == 0 && c.hw && g_hwTainted) { return; }
		{
			HwGuard g(g_resLock);
			std::map<int, ModeResult>::iterator it = g_modeResults[c.key].find(forms[i]);
			if (it != g_modeResults[c.key].end() && it->second.opened && it->second.stalled) { stalledRun++; } else if (it != g_modeResults[c.key].end() && it->second.opened) { stalledRun = 0; }
		}
	}
	ModeChoice ch = choose_mode(c.key);
	{ HwGuard g(g_resLock); g_modes[c.key] = ch; }
	JObj o;
	o.str("key", c.key).b("known", ch.known).str("mode", mode_for_label(ch)).b("effective", ch.effective).str("note", ch.note);
	g_doc.append("mode_choice", o.done());
	hw_say("  chosen for the benchmark: %s (%s)\n", ch.known ? mode_name(ch.mode) : "none", ch.note.c_str());
}

// ---------------------------------------------------------------------------------------------------------------------- bench

struct BenchArg
{
	Cand c;
	int w, h, content, qp;
	bool d3d;
	int frames;
	double endMs;
	std::string label;
};

static const char *content_label(int k) { return hw_content_name(k); }

static std::string rules_brief(const struct kvmh_check &k, int *nFailed)
{
	JArr a;
	int n = 0;
	for (int i = 0; i < KVMH_NRULES; i++)
	{
		if (k.r[i].fails > 0 && i != KVMH_R_VUI) { a.raw(JObj().str("id", kvmh_rule_id(i)).u("access_units", k.r[i].fails).str("first", k.r[i].first).done()); n++; }
	}
	if (nFailed != NULL) { *nFailed = n; }
	return a.done();
}

static void stage_bench_run(void *p)
{
	BenchArg *a = (BenchArg *)p;
	const Cand &c = a->c;
	ModeChoice ch;
	{ HwGuard g(g_resLock); std::map<std::string, ModeChoice>::iterator it = g_modes.find(c.key); if (it != g_modes.end()) { ch = it->second; } }
	if (!ch.known) { ch.known = true; ch.mode = MODE_QP_UI8; ch.note = "default (qpmid did not run)"; }
	const int cw = kvm_yuv_coded(a->w), chh = kvm_yuv_coded(a->h);
	HwGen *gen = get_gen(cw, chh);
	JObj o;
	RunSummary sum;
	sum.key = c.key; sum.label = a->label; sum.p50 = sum.p95 = sum.max = sum.fps = sum.cpuPerFrame = sum.pMean = sum.idrBytes = 0; sum.ok = false; sum.rulesFailed = 0;
	char sz[32], cz[32];
	_snprintf(sz, sizeof(sz), "%dx%d", a->w, a->h);
	_snprintf(cz, sizeof(cz), "%dx%d", cw, chh);
	o.str("key", c.key).str("name", c.name).str("size", sz).str("coded", cz).str("content", content_label(a->content)).str("mode", mode_id(ch.mode)).b("mode_effective", ch.effective).b("d3d_manager", a->d3d).str("label", a->label);
	if (gen == NULL) { o.str("error", "pictures"); g_doc.append("bench", o.done()); g_failed++; delete a; return; }
	EncCfg cfg;
	cfg.w = cw; cfg.h = chh; cfg.mode = ch.mode; cfg.qp = a->qp; cfg.d3d = a->d3d; cfg.level = mfh264_level_for_size(cw, chh);
	{
		const double scale = (double)cw * (double)chh / (1920.0 * 1088.0);
		// the closest rate control mode when no QP form works: 12 / 8 / 5 Mb/s at 1080p for QP 26 / 30 / 34
		const double base = a->qp <= 26 ? 12000000.0 : (a->qp <= 30 ? 8000000.0 : 5000000.0);
		cfg.rateBps = (int)(base * scale);
		cfg.bitrate = cfg.rateBps;
		if (is_qp_mode(ch.mode)) { cfg.bitrate = 8000000; }
		o.i("qp", a->qp);
		if (!is_qp_mode(ch.mode)) { o.i("rate_bps", cfg.rateBps); }
	}
	Enc e;
	const double t0 = hw_now_ms();
	const HRESULT hr = e.open(c, cfg);
	o.d("open_ms", hw_now_ms() - t0, 0).b("opened", SUCCEEDED(hr));
	if (FAILED(hr))
	{
		o.hex("hr", (unsigned long)hr).str("fail_stage", e.failStage).raw("steps", e.steps_json());
		hw_say("  %-28s %-4s OPEN REFUSED at %s (%s)\n", a->label.c_str(), c.key.c_str(), e.failStage.c_str(), hr_name(hr));
		g_failed++;
		e.close();
		g_doc.append("bench", o.done());
		HwGuard g(g_resLock); g_summaries.push_back(sum); g_benchFailRun[c.key]++;
		delete a;
		return;
	}
	struct kvmh_check rules;
	kvmh_init(&rules, (unsigned)cw, (unsigned)chh, 0);
	RunReq rq;
	RunRes rr;
	const int forceAt = a->frames >= 60 ? a->frames / 2 : -1;
	rq.gen = gen; rq.content = a->content; rq.frames = a->frames; rq.endMs = a->endMs; rq.forceAt = forceAt; rq.rules = &rules;
	e.run(rq, &rr);
	// statistics
	HwSeries lat, pb, idrb;
	size_t total = 0;
	for (size_t i = 0; i < rr.outs.size(); i++)
	{
		const OutRec &r = rr.outs[i];
		lat.add(r.outMs - r.submitMs);
		total += r.bytes;
		if (r.idr) { idrb.add((double)r.bytes); } else { pb.add((double)r.bytes); }
	}
	const size_t n = rr.outs.size();
	const double spanMs = n > 0 ? rr.outs[n - 1].outMs - rr.submit[0] : 0;
	const double fps = spanMs > 0 ? (double)n * 1000.0 / spanMs : 0;
	const double cpuEnc = std::max(0.0, rr.cpuMs - rr.sourceMs);
	o.i("frames_asked", a->frames).i("submitted", rr.submitted).i("outputs", (long long)n).b("async", e.async).b("provides_samples", e.provides);
	o.raw("latency_ms", JObj().d("p50", lat.pct(0.5)).d("p95", lat.pct(0.95)).d("p99", lat.pct(0.99)).d("max", lat.max()).d("mean", lat.mean()).d("first", n > 0 ? lat.v[0] : 0).done());
	o.d("throughput_fps", fps, 1).d("wall_ms", rr.wallMs, 0);
	o.raw("bytes", JObj().d("p_mean", pb.mean(), 0).d("p_p50", pb.pct(0.5), 0).d("p_max", pb.max(), 0).d("idr_first", idrb.count() > 0 ? idrb.v[0] : 0, 0).i("idr_count", (long long)idrb.count()).u("total", total)
		.d("kbps_at_30fps", n > 0 ? (double)total * 8.0 * 30.0 / (double)n / 1000.0 : 0, 0).done());
	o.raw("cpu", JObj().d("process_ms", rr.cpuMs, 0).d("source_ms", rr.sourceMs, 0).d("encode_estimate_ms", cpuEnc, 0).d("per_frame_ms", n > 0 ? cpuEnc / (double)n : 0, 2).d("pct_of_one_core", rr.wallMs > 0 ? 100.0 * cpuEnc / rr.wallMs : 0, 0).done());
	o.raw("events", JObj().i("need_input", rr.needInput).i("have_output", rr.haveOutput).i("stream_changes", rr.streamChanges).i("not_accepting", rr.notAccepting).i("max_inflight", rr.maxInflight).i("pts_mismatch", rr.ptsMismatch)
		.i("have_before_input", rr.haveBeforeInput).i("sc_event", rr.scEvent).i("sc_no_event", rr.scNoEvent).i("sc_need_wait", rr.scNeedWait).d("sc_wait_ms_max", rr.scWaitMax, 0)
		.hex("sc_set_output_type_hr", (unsigned long)rr.scSetHr).i("sc_type_idx", rr.scTypeIdx).i("sc_type_n", rr.scTypeN).str("sc_subtype", rr.scSubtype).done());
	if (n > 0)
	{
		unsigned long long q0 = 0, q1 = 0;
		bool h0 = false, h1 = false;
		for (size_t i = 0; i < n; i++) { if (rr.outs[i].hasQp) { if (!h0) { q0 = rr.outs[i].qpRaw; h0 = true; } q1 = rr.outs[i].qpRaw; h1 = true; } }
		if (h0 || h1) { o.raw("reported_qp_raw", JObj().u("first", q0).u("last", q1).done()); }
	}
	if (forceAt >= 0)
	{
		int idx = -1;
		for (size_t i = (size_t)forceAt; i < n; i++) { if (rr.outs[i].idr) { idx = (int)i; break; } }
		JObj f;
		f.i("asked_at_picture", forceAt).hex("hr", (unsigned long)rr.forceHr).b("idr_found", idx >= 0 && idx <= forceAt + 8);
		if (idx >= 0) { f.i("first_idr_output_index", idx).i("delay_outputs", idx - forceAt).u("bytes", rr.outs[(size_t)idx].bytes); }
		o.raw("forced_idr", f.done());
	}
	{
		int nf = 0;
		kvmh_finish(&rules);
		o.raw("rules_failed", rules_brief(rules, &nf));
		o.u("rules_checked_access_units", rules.aus);
		sum.rulesFailed = nf;
	}
	if (rr.timedOut) { o.b("truncated_by_time_limit", true); }
	if (rr.drainTimedOut) { o.b("drain_timed_out", true); }
	if (FAILED(rr.err)) { o.str("error", rr.errStage).hex("error_hr", (unsigned long)rr.err); g_failed++; }
	else if (n == 0) { o.str("error", "no output"); g_failed++; }
	else if ((int)n < rr.submitted) { o.b("lost_outputs", true); }
	sum.p50 = lat.pct(0.5); sum.p95 = lat.pct(0.95); sum.max = lat.max(); sum.fps = fps; sum.cpuPerFrame = n > 0 ? cpuEnc / (double)n : 0; sum.pMean = pb.mean(); sum.idrBytes = idrb.count() ? idrb.v[0] : 0; sum.ok = n > 0 && SUCCEEDED(rr.err);
	hw_say("  %-28s %-4s lat P50 %6.1f P95 %6.1f max %6.1f ms  %5.1f fps  P %7.0f B  IDR %7.0f B  cpu %5.2f ms/frame  rules failed %d%s%s\n", a->label.c_str(), c.key.c_str(), lat.pct(0.5), lat.pct(0.95), lat.max(), fps,
		pb.mean(), idrb.count() ? idrb.v[0] : 0.0, n > 0 ? cpuEnc / (double)n : 0.0, sum.rulesFailed, rr.timedOut ? "  TIME LIMIT" : "", FAILED(rr.err) ? ("  error: " + rr.errStage).c_str() : "");
	e.close();
	g_doc.append("bench", o.done());
	{ HwGuard g(g_resLock); g_summaries.push_back(sum); if (sum.ok) { g_benchFailRun[c.key] = 0; } else { g_benchFailRun[c.key]++; } }
	delete a;
}

static void bench_one(const Cand &c, int w, int h, int content, int qp, bool d3d, double benchEnd)
{
	if (left_ms() < 2500 || hw_now_ms() > benchEnd) { g_skipped++; hw_say("  (skipped: time budget) %s %dx%d %s QP %d\n", c.key.c_str(), w, h, content_label(content), qp); return; }
	{
		HwGuard g(g_resLock);
		if (g_benchFailRun[c.key] >= 2)
		{
			hw_say("  (skipped: the last 2 runs of %s failed) %dx%d %s QP %d\n", c.key.c_str(), w, h, content_label(content), qp);
			return;
		}
	}
	BenchArg *a = new BenchArg;
	a->c = c; a->w = w; a->h = h; a->content = content; a->qp = qp; a->d3d = d3d; a->frames = g_cfg.frames;
	a->endMs = std::min(hw_now_ms() + 25000.0, benchEnd);
	char lab[96];
	_snprintf(lab, sizeof(lab), "%dx%d %s QP%d%s", w, h, content_label(content), qp, d3d ? " +D3D" : "");
	a->label = lab;
	char stage[128];
	_snprintf(stage, sizeof(stage), "bench %s %s", c.key.c_str(), lab);
	run_stage(stage, (int)std::min(32000.0, std::max(3000.0, benchEnd - hw_now_ms() + 3000.0)), stage_bench_run, a, c.hw);
}

static void run_bench(double benchEnd)
{
	struct Cfg { int w, h, content, qp; };
	std::vector<Cfg> list;
	const Cfg base[] = { { 1920, 1080, HW_CONTENT_SCROLL, 30 }, { 1920, 1080, HW_CONTENT_TEXT, 30 }, { 1920, 1080, HW_CONTENT_VIDEO, 30 }, { 1920, 1200, HW_CONTENT_SCROLL, 30 },
		{ 1920, 1080, HW_CONTENT_SCROLL, 26 }, { 1920, 1080, HW_CONTENT_SCROLL, 34 }, { 1920, 1080, HW_CONTENT_TEXT, 26 }, { 1920, 1080, HW_CONTENT_TEXT, 34 },
		{ 1920, 1080, HW_CONTENT_VIDEO, 26 }, { 1920, 1080, HW_CONTENT_VIDEO, 34 }, { 1920, 1200, HW_CONTENT_TEXT, 30 } };
	for (size_t i = 0; i < sizeof(base) / sizeof(base[0]); i++) { list.push_back(base[i]); }
	for (size_t i = 0; i < g_cfg.sizes.size(); i++) { Cfg x = { g_cfg.sizes[i].first, g_cfg.sizes[i].second, HW_CONTENT_SCROLL, 30 }; list.insert(list.begin() + std::min<size_t>(list.size(), 4 + i), x); }
	hw_say("\nbench: %d pictures per run, NV12 in system memory, latency = ProcessInput -> output sample\n", g_cfg.frames);
	const bool haveHw = !g_hwCands.empty() && g_cfg.candidate >= 0 && g_cfg.candidate < (int)g_hwCands.size();
	for (size_t i = 0; i < list.size(); i++)
	{
		if (haveHw && !g_hwTainted) { bench_one(g_hwCands[(size_t)g_cfg.candidate], list[i].w, list[i].h, list[i].content, list[i].qp, false, benchEnd); }
		if (g_haveSw && !g_cfg.noSoftware) { bench_one(g_swCand, list[i].w, list[i].h, list[i].content, list[i].qp, false, benchEnd); }
		if (i == 0 && haveHw && !g_hwTainted && !g_cfg.noD3d)
		{
			bool aware = false;
			{ HwGuard g(g_resLock); aware = g_d3dAware[g_hwCands[(size_t)g_cfg.candidate].key]; }
			if (aware) { bench_one(g_hwCands[(size_t)g_cfg.candidate], list[i].w, list[i].h, list[i].content, list[i].qp, true, benchEnd); }
		}
		if (i == 1 && !g_hwTainted)
		{
			// the other hardware encoders (hybrid laptops), two runs each
			for (size_t k = 0; k < g_hwCands.size(); k++)
			{
				if ((int)k == g_cfg.candidate) { continue; }
				bench_one(g_hwCands[k], 1920, 1080, HW_CONTENT_SCROLL, 30, false, benchEnd);
			}
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------------- dump

struct DumpArg { Cand c; double endMs; };

// the first NAL unit of a type of an Annex B buffer, with its start code (empty when there is none)
static std::vector<uint8_t> nal_with_start(const uint8_t *p, size_t n, int type, int which)
{
	std::vector<uint8_t> r;
	int seen = 0;
	for (size_t i = 0; i + 4 < n; i++)
	{
		if (p[i] == 0 && p[i + 1] == 0 && (p[i + 2] == 1 || (p[i + 2] == 0 && p[i + 3] == 1)))
		{
			const size_t hdr = (p[i + 2] == 1) ? i + 3 : i + 4;
			if (hdr < n && (p[hdr] & 0x1F) == type && seen++ == which)
			{
				size_t j = hdr + 1;
				while (j + 2 < n && !(p[j] == 0 && p[j + 1] == 0 && (p[j + 2] == 1 || (j + 3 < n && p[j + 2] == 0 && p[j + 3] == 1)))) { j++; }
				if (j + 2 >= n) { j = n; }
				r.assign(p + i, p + j);
				return r;
			}
		}
	}
	return r;
}

static void stage_dump(void *p)
{
	DumpArg *a = (DumpArg *)p;
	const Cand &c = a->c;
	ModeChoice ch;
	{ HwGuard g(g_resLock); std::map<std::string, ModeChoice>::iterator it = g_modes.find(c.key); if (it != g_modes.end()) { ch = it->second; } }
	if (!ch.known) { ch.known = true; ch.mode = MODE_QP_UI8; }
	HwGen *gen = get_gen(1920, 1088);
	JObj o;
	o.str("key", c.key).str("name", c.name).str("mode", mode_id(ch.mode));
	if (gen == NULL) { g_doc.append("dump", o.done()); delete a; return; }
	EncCfg cfg;
	cfg.mode = ch.mode; cfg.qp = 30; cfg.rateBps = cfg.bitrate = 8000000; cfg.level = mfh264_level_for_size(1920, 1088);
	Enc e;
	const HRESULT hr = e.open(c, cfg);
	if (FAILED(hr))
	{
		o.b("opened", false).str("fail_stage", e.failStage).hex("hr", (unsigned long)hr);
		hw_say("  dump %s: open refused at %s\n", c.key.c_str(), e.failStage.c_str());
		e.close();
		g_failed++;
		g_doc.append("dump", o.done());
		delete a;
		return;
	}
	std::vector<std::vector<uint8_t> > kept;
	RunReq rq;
	RunRes rr;
	rq.gen = gen; rq.content = HW_CONTENT_SCROLL; rq.frames = 40; rq.endMs = a->endMs; rq.forceAt = 20; rq.keep = &kept; rq.keepMax = 40;
	e.run(rq, &rr);
	std::vector<uint8_t> seq = e.seqHdr;
	// files
	JArr files;
	size_t streamBytes = 0;
	std::string stream, table;
	char line[160];
	const std::string pre = c.key + "_";
	for (size_t i = 0; i < kept.size(); i++)
	{
		stream.append((const char *)(kept[i].empty() ? NULL : &kept[i][0]), kept[i].size());
		char nl[64];
		kvmh_nal_list(kept[i].empty() ? NULL : &kept[i][0], kept[i].size(), nl, sizeof(nl));
		_snprintf(line, sizeof(line), "%u %u idr=%d nal=[%s] pts=%lld qp=%llu\n", (unsigned)i, (unsigned)kept[i].size(), (int)(i < rr.outs.size() ? rr.outs[i].idr : 0), nl, i < rr.outs.size() ? rr.outs[i].pts : -1LL,
			i < rr.outs.size() && rr.outs[i].hasQp ? rr.outs[i].qpRaw : 0ULL);
		table += line;
		streamBytes += kept[i].size();
	}
	{
		const std::string fn = pre + "stream.h264";
		if (write_file(out_path(fn.c_str()), stream.data(), stream.size())) { files.raw(JObj().str("file", fn).u("bytes", stream.size()).done()); }
		const std::string ft = pre + "aus.txt";
		if (write_file(out_path(ft.c_str()), table.data(), table.size())) { files.raw(JObj().str("file", ft).u("bytes", table.size()).done()); }
	}
	for (size_t i = 0; i < kept.size(); i++)
	{
		const int idx = i < rr.keptIndex.size() ? rr.keptIndex[i] : (int)i;
		if (idx < 6 || (idx >= 20 && idx < 23))
		{
			char fn[64];
			_snprintf(fn, sizeof(fn), "%sau_%03d.h264", pre.c_str(), idx);
			if (write_file(out_path(fn), kept[i].empty() ? NULL : &kept[i][0], kept[i].size())) { files.raw(JObj().str("file", fn).u("bytes", kept[i].size()).done()); }
		}
	}
	if (!kept.empty())
	{
		// SPS and PPS of the first IDR (from the access unit, else from the sequence header of the output type)
		std::vector<uint8_t> sps = nal_with_start(&kept[0][0], kept[0].size(), 7, 0), pps = nal_with_start(&kept[0][0], kept[0].size(), 8, 0);
		if (sps.empty() && !seq.empty()) { sps = nal_with_start(&seq[0], seq.size(), 7, 0); }
		if (pps.empty() && !seq.empty()) { pps = nal_with_start(&seq[0], seq.size(), 8, 0); }
		if (!sps.empty()) { const std::string fn = pre + "sps.bin"; if (write_file(out_path(fn.c_str()), &sps[0], sps.size())) { files.raw(JObj().str("file", fn).u("bytes", sps.size()).done()); } o.str("sps_hex", hw_hex(&sps[0], sps.size(), 48)); }
		if (!pps.empty()) { const std::string fn = pre + "pps.bin"; if (write_file(out_path(fn.c_str()), &pps[0], pps.size())) { files.raw(JObj().str("file", fn).u("bytes", pps.size()).done()); } o.str("pps_hex", hw_hex(&pps[0], pps.size(), 32)); }
	}
	if (!seq.empty()) { const std::string fn = pre + "seqhdr.bin"; if (write_file(out_path(fn.c_str()), &seq[0], seq.size())) { files.raw(JObj().str("file", fn).u("bytes", seq.size()).done()); } }
	// the rules: on the raw access units, and on what kvm_annexb_normalize makes of them (the agent would send that)
	struct kvmh_check raw, norm;
	kvmh_init(&raw, 1920, 1088, 0);
	kvmh_init(&norm, 1920, 1088, 0);
	int normErrors = 0, normAud = 0, normAdded = 0;
	for (size_t i = 0; i < kept.size(); i++)
	{
		const uint8_t *d = kept[i].empty() ? NULL : &kept[i][0];
		kvmh_au(&raw, d, kept[i].size(), i == 0 ? 1 : -1);
		std::vector<uint8_t> out(kept[i].size() + seq.size() + 256);
		size_t ol = 0;
		struct kvm_au_info info;
		const int r = kvm_annexb_normalize(d, kept[i].size(), seq.empty() ? NULL : &seq[0], seq.size(), &out[0], out.size(), &ol, &info);
		if (r == KVM_ANNEXB_OK) { kvmh_au(&norm, &out[0], ol, i == 0 ? 1 : -1); normAud += info.aud_removed; normAdded += info.header_added; }
		else { normErrors++; kvmh_au(&norm, NULL, 0, i == 0 ? 1 : -1); if (normErrors == 1) { _snprintf(line, sizeof(line), "kvm_annexb_normalize refused access unit %u (%d)", (unsigned)i, r); } }
	}
	kvmh_finish(&raw);
	kvmh_finish(&norm);
	o.b("opened", true).i("access_units", (long long)kept.size()).u("stream_bytes", streamBytes).raw("files", files.done());
	o.raw("rules_raw", hw_rules_json(raw)).raw("rules_after_normalize", hw_rules_json(norm));
	o.raw("normalize", JObj().i("refused", normErrors).i("aud_removed", normAud).i("headers_added", normAdded).done());
	hw_say("\n== dump %s (%s): %u access units of the scroll picture, forced IDR at 20, files %s*\n", c.key.c_str(), c.name.c_str(), (unsigned)kept.size(), pre.c_str());
	hw_rules_print(raw, "the output of the encoder as it is");
	hw_say("  kvm_annexb_normalize: %d refused, %d AUD removed, %d headers added from the sequence header\n", normErrors, normAud, normAdded);
	hw_rules_print(norm, "the output after kvm_annexb_normalize (what the agent would send)");
	if (FAILED(rr.err)) { o.str("error", rr.errStage); }
	e.close();
	g_doc.append("dump", o.done());
	delete a;
}

// ---------------------------------------------------------------------------------------------------------------------- the rules on a file (no Media Foundation)

static int check_file(int argc, char **argv)
{
	if (argc < 4) { hw_say("{\"error\":\"usage: kvm_h264_check --hw check FILE.h264 [codedW codedH [maxLevel]]\"}\n"); return 2; }
	FILE *f = fopen(argv[3], "rb");
	if (f == NULL) { hw_say("{\"error\":\"cannot open the file\"}\n"); return 2; }
	std::vector<uint8_t> d;
	uint8_t buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && d.size() < (size_t)256 << 20) { d.insert(d.end(), buf, buf + n); }
	fclose(f);
	const unsigned w = argc > 4 ? (unsigned)atoi(argv[4]) : 0, h = argc > 5 ? (unsigned)atoi(argv[5]) : 0;
	const int maxLevel = argc > 6 ? atoi(argv[6]) : 0;
	struct kvmh_check c;
	kvmh_init(&c, w, h, maxLevel);
	size_t pos = 0;
	while (pos < d.size())
	{
		size_t e = kvmh_next_au(&d[0], d.size(), pos);
		if (e <= pos) { e = d.size(); }
		kvmh_au(&c, &d[pos], e - pos, -1);
		pos = e;
	}
	const int fails = kvmh_finish(&c);
	hw_rules_print(c, argv[3]);
	hw_say("%s\n", JObj().str("kind", "hwprobe_check").raw("result", hw_rules_json(c)).done().c_str());
	return fails ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------------------------------- main

static void usage(void)
{
	hw_say("{\"error\":\"usage: kvm_h264_check --hw [probe,bench,dump] [--out DIR] [--max-seconds N] [--frames N] [--size WxH] [--candidate K] [--no-software] [--no-d3d] [--dump-software] | --hw check FILE [W H [maxLevel]]\"}\n");
}

static void summary_table(void)
{
	HwGuard g(g_resLock);
	if (g_summaries.empty()) { return; }
	hw_say("\nsummary (latency = ProcessInput to output sample; cpu = process CPU without the picture source, per picture)\n");
	hw_say("  %-26s %-5s %8s %8s %8s %7s %9s %9s %8s %5s\n", "run", "enc", "P50 ms", "P95 ms", "max ms", "fps", "P bytes", "IDR bytes", "cpu ms", "rules");
	JArr a;
	for (size_t i = 0; i < g_summaries.size(); i++)
	{
		const RunSummary &s = g_summaries[i];
		hw_say("  %-26s %-5s %8.1f %8.1f %8.1f %7.1f %9.0f %9.0f %8.2f %5d%s\n", s.label.c_str(), s.key.c_str(), s.p50, s.p95, s.max, s.fps, s.pMean, s.idrBytes, s.cpuPerFrame, s.rulesFailed, s.ok ? "" : "  FAILED");
		a.raw(JObj().str("run", s.label).str("encoder", s.key).d("p50_ms", s.p50).d("p95_ms", s.p95).d("max_ms", s.max).d("fps", s.fps, 1).d("p_bytes", s.pMean, 0).d("idr_bytes", s.idrBytes, 0).d("cpu_ms_per_frame", s.cpuPerFrame).i("rules_failed", s.rulesFailed).b("ok", s.ok).done());
	}
	g_doc.set("summary", a.done());
}

int hwprobe_main(int argc, char **argv)
{
	g_t0 = hw_now_ms();
	std::string stages = "probe,bench,dump";
	int i = 2;
	if (argc > 2 && std::string(argv[2]) == "check") { return check_file(argc, argv); }
	if (argc > 2 && argv[2][0] != '-') { stages = argv[2]; i = 3; }
	for (; i < argc; i++)
	{
		const std::string a = argv[i];
		const bool more = i + 1 < argc;
		if (a == "--out" && more) { g_cfg.outDir = argv[++i]; }
		else if (a == "--max-seconds" && more) { g_cfg.totalS = atoi(argv[++i]); }
		else if (a == "--frames" && more) { g_cfg.frames = atoi(argv[++i]); }
		else if (a == "--candidate" && more) { g_cfg.candidate = atoi(argv[++i]); }
		else if (a == "--size" && more) { int w = 0, h = 0; if (sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w >= 64 && h >= 64 && w <= 4096 && h <= 4096 && g_cfg.sizes.size() < 2) { g_cfg.sizes.push_back(std::make_pair(w, h)); } else { usage(); return 2; } }
		else if (a == "--no-software") { g_cfg.noSoftware = true; }
		else if (a == "--no-d3d") { g_cfg.noD3d = true; }
		else if (a == "--dump-software") { g_cfg.dumpSoftware = true; }
		else { usage(); return 2; }
	}
	{
		size_t p = 0;
		while (p <= stages.size())
		{
			size_t q = stages.find(',', p);
			if (q == std::string::npos) { q = stages.size(); }
			const std::string s = stages.substr(p, q - p);
			if (s == "probe") { g_cfg.probe = true; } else if (s == "bench") { g_cfg.bench = true; } else if (s == "dump") { g_cfg.dump = true; } else if (!s.empty()) { usage(); return 2; }
			p = q + 1;
		}
	}
	if (!g_cfg.probe && !g_cfg.bench && !g_cfg.dump) { usage(); return 2; }
	if (g_cfg.totalS < 20) { g_cfg.totalS = 20; }
	if (g_cfg.totalS > 110) { g_cfg.totalS = 110; }
	if (g_cfg.frames < 20) { g_cfg.frames = 20; }
	if (g_cfg.frames > 300) { g_cfg.frames = 300; }
	if (g_cfg.candidate < 0) { g_cfg.candidate = 0; }
	if (!g_cfg.outDir.empty() && g_cfg.outDir != ".") { CreateDirectoryA(g_cfg.outDir.c_str(), NULL); }
	g_deadline = g_t0 + (double)g_cfg.totalS * 1000.0;
	std::string why;
	if (load_api(&why) != 0)
	{
		hw_say("kvm_h264_check --hw: %s\n%s\n", why.c_str(), JObj().str("kind", "hwprobe").b("incomplete", true).str("error", why).done().c_str());
		return 2;
	}
	g_api.CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (FAILED(g_api.MFStartup(MF_VERSION, 0)))
	{
		hw_say("kvm_h264_check --hw: MFStartup failed\n%s\n", JObj().str("kind", "hwprobe").b("incomplete", true).str("error", "MFStartup failed").done().c_str());
		return 2;
	}
	CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL);
	{
		JObj m;
		m.str("tool", "kvm_h264_check --hw").str("stages", stages).i("max_seconds", g_cfg.totalS).i("frames_per_run", g_cfg.frames).i("candidate", g_cfg.candidate).b("software_comparison", !g_cfg.noSoftware).b("d3d_variant", !g_cfg.noD3d);
		m.str("note", "synthetic NV12 pictures in system memory, no capture, no desktop, no KVM session");
		g_doc.set("meta", m.done());
	}
	hw_say("kvm_h264_check --hw %s: total limit %d s, %d pictures per run, output in %s\n", stages.c_str(), g_cfg.totalS, g_cfg.frames, g_cfg.outDir.c_str());
	run_stage("machine", 15000, stage_machine, NULL, false);
	run_stage("enum", 20000, stage_enum, NULL, true);
	write_snapshot(true, "enumerated");
	const bool noHw = g_hwCands.empty();
	if (g_cfg.probe)
	{
		for (size_t k = 0; k < g_hwCands.size() && k < 3 && !g_hwTainted; k++) { probe_candidate(g_hwCands[k], k == 0); }
		if (g_haveSw && !g_cfg.noSoftware) { probe_candidate(g_swCand, false); }
	}
	else if (g_cfg.bench || g_cfg.dump)
	{
		// the way of steering the encoder is needed by the benchmark and the dump: find it with a short qpmid when the probe did not run
		if (!g_hwCands.empty() && g_cfg.candidate < (int)g_hwCands.size()) { qpmid_candidate(g_hwCands[(size_t)g_cfg.candidate], false); }
		if (g_haveSw && !g_cfg.noSoftware) { qpmid_candidate(g_swCand, false); }
	}
	write_snapshot(true, "probe done");
	const double dumpReserve = g_cfg.dump ? 14000.0 : 0.0;
	if (g_cfg.bench) { run_bench(g_deadline - dumpReserve); write_snapshot(true, "bench done"); }
	if (g_cfg.dump)
	{
		bool done = false;
		if (!g_hwCands.empty() && g_cfg.candidate < (int)g_hwCands.size() && !g_hwTainted)
		{
			DumpArg *d = new DumpArg;
			d->c = g_hwCands[(size_t)g_cfg.candidate]; d->endMs = std::min(hw_now_ms() + 12000.0, g_deadline);
			run_stage("dump hw", 16000, stage_dump, d, true);
			done = true;
		}
		if (g_haveSw && (g_cfg.dumpSoftware || !done))
		{
			DumpArg *d = new DumpArg;
			d->c = g_swCand; d->endMs = std::min(hw_now_ms() + 12000.0, g_deadline);
			run_stage("dump sw", 16000, stage_dump, d, false);
		}
	}
	summary_table();
	// the result
	int code = 0;
	if (g_failed > 0) { code = 1; }
	if (g_skipped > 0 && code == 0) { code = 5; }
	if (noHw && code != 4) { code = 3; }
	if (g_hungStages > 0) { code = 4; }
	{
		JObj r;
		r.i("exit_code", code).i("hardware_encoders", (long long)g_hwCands.size()).i("failed_stages_or_runs", (long long)g_failed).i("hung_stages", (long long)g_hungStages).i("skipped_runs", (long long)g_skipped).d("elapsed_s", elapsed_s(), 1);
		g_doc.set("result", r.done());
	}
	g_finished = 1;
	write_snapshot(false, NULL);
	hw_say("\nelapsed %.1f s, exit code %d (%s)\n", elapsed_s(), code, code == 0 ? "everything that was asked for ran" : code == 1 ? "a stage or a run failed" : code == 3 ? "no hardware H.264 encoder found" : code == 4 ? "a stage did not return" : code == 5 ? "the time budget was used up, runs were skipped" : "?");
	hw_say("%s\n", g_doc.render(false, NULL).c_str());
	if (g_hungStages > 0) { fflush(stdout); TerminateProcess(GetCurrentProcess(), (UINT)code); }
	// the encoders are closed in their stages; the Media Foundation platform is not shut down on purpose (a driver thread of the platform may still be running)
	return code;
}
