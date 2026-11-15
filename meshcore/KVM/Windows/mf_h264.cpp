// KVM (Windows): H.264 encoder around the software Microsoft H.264 Encoder MFT. See mf_h264.h for the interface, the thread model, the
// rules for a lost access unit and for faults. Media Foundation, COM and the OS version call are loaded at run time; nothing is added to the
// import table. This file must not include microstack or agent headers.

#include <windows.h>
#include <initguid.h> // the GUIDs of the Media Foundation headers are defined in this file only (no mfuuid.lib / strmiids.lib)
#include <mfapi.h>
#include <mftransform.h>
#include <mfidl.h>   // (the IIDs of IMFMediaEventGenerator and IMFActivate are defined here for mf_h264_hw.cpp, see INITGUID above)
#include <mferror.h>
#include <strmif.h>
#include <codecapi.h>
#include <stdlib.h>
#include <string.h>

#include "mf_h264.h"
#include "mf_h264_priv.h"
#include "../common/kvm_yuv.h"
#include "../common/kvm_annexb.h"

// {6CA50344-051A-4DED-9779-A43305165E35} CLSID_CMSH264EncoderMFT. Defined here: the MSVC wmcodecdsp.h only declares it for __uuidof, there is no library for it.
DEFINE_GUID(kMfh264_CLSID_EncoderMFT, 0x6ca50344, 0x051a, 0x4ded, 0x97, 0x79, 0xa4, 0x33, 0x05, 0x16, 0x5e, 0x35);

// Interface ids: the MSVC headers declare them for __uuidof (no library). MinGW's headers define IID_IMFTransform (INITGUID above), but its
// strmif.h has the ICodecAPI class without an IID_ICodecAPI: defined here (the build check with MinGW is only a syntax / type check).
#ifdef _MSC_VER
#define MFH264_IID(T) __uuidof(T)
#else
DEFINE_GUID(kMfh264_IID_ICodecAPI, 0x901db4c7, 0x31ce, 0x41a2, 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda);
#define MFH264_IID_IMFTransform IID_IMFTransform
#define MFH264_IID_ICodecAPI kMfh264_IID_ICodecAPI
#define MFH264_IID(T) MFH264_IID_##T
#endif

// Values of enumerations that are used as plain numbers (the headers are not needed for them).
#define RC_CBR 0
#define RC_PEAK_VBR 1
#define RC_LOWDELAY_VBR 4
#define RC_VBR_UNCONSTRAINED 2
#define RC_QUALITY 3
#define MFH264_MAX_DIM 8192
#define MFH264_IN_RING 3        // input samples in rotation: the encoder has delivered the output of a frame long before its buffer is used again
#define MFH264_MAX_PARTS 8      // parts of one access unit (MFT_OUTPUT_DATA_BUFFER_INCOMPLETE)

// ------------------------------------------------------------------------------------------------------------- run time linking


PFN_MFStartup p_MFStartup = NULL;
PFN_MFShutdown p_MFShutdown = NULL;
PFN_MFCreateMediaType p_MFCreateMediaType = NULL;
PFN_MFCreateSample p_MFCreateSample = NULL;
PFN_MFCreateMemoryBuffer p_MFCreateMemoryBuffer = NULL;
PFN_CoInitializeEx p_CoInitializeEx = NULL;
PFN_CoUninitialize p_CoUninitialize = NULL;
PFN_CoCreateInstance p_CoCreateInstance = NULL;
static volatile LONG g_loadState = 0; // 0 = not tried, 1 = loaded, 2 = failed
static volatile LONG g_faulted = 0;   // a structured exception happened: video is off for this process
static int g_osMajor = 0, g_osMinor = 0, g_osBuild = 0;

#ifdef _MSC_VER
#define MFH264_TLS __declspec(thread)
#else
#define MFH264_TLS __thread
#endif
static MFH264_TLS int tl_inits = 0;     // successful mfh264_init() calls of this thread, not yet shut down
static MFH264_TLS int tl_coUninit = 0;  // CoInitializeEx of this thread needs CoUninitialize

HMODULE mfp_load_system_dll(const wchar_t *name)
{
	// LOAD_LIBRARY_SEARCH_SYSTEM32 does not exist on Windows 7 without a patch, then use the plain call (same as dxgi_capture.cpp).
	HMODULE m = LoadLibraryExW(name, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (m == NULL && GetLastError() == ERROR_INVALID_PARAMETER) { m = LoadLibraryW(name); }
	return m;
}

static void read_os_version(void)
{
	HMODULE nt = GetModuleHandleW(L"ntdll.dll");
	PFN_RtlGetVersion f = (nt != NULL) ? (PFN_RtlGetVersion)GetProcAddress(nt, "RtlGetVersion") : NULL;
	RTL_OSVERSIONINFOW vi;
	memset(&vi, 0, sizeof(vi));
	vi.dwOSVersionInfoSize = sizeof(vi);
	if (f != NULL && f(&vi) == 0) { g_osMajor = (int)vi.dwMajorVersion; g_osMinor = (int)vi.dwMinorVersion; g_osBuild = (int)vi.dwBuildNumber; }
}

int mfp_os_ok(void)
{
	if (g_osMajor == 0) { read_os_version(); }
	return g_osMajor > 6 || (g_osMajor == 6 && g_osMinor >= 2); // Windows 8 = 6.2
}

int mfp_os_build(void) { if (g_osMajor == 0) { read_os_version(); } return g_osBuild; }

int mfp_load_libs(void)
{
	if (g_loadState != 0) { return g_loadState == 1; }
	HMODULE mf = mfp_load_system_dll(L"mfplat.dll");
	HMODULE ole = mfp_load_system_dll(L"ole32.dll");
	if (mf == NULL || ole == NULL) { g_loadState = 2; return 0; }
	p_MFStartup = (PFN_MFStartup)GetProcAddress(mf, "MFStartup");
	p_MFShutdown = (PFN_MFShutdown)GetProcAddress(mf, "MFShutdown");
	p_MFCreateMediaType = (PFN_MFCreateMediaType)GetProcAddress(mf, "MFCreateMediaType");
	p_MFCreateSample = (PFN_MFCreateSample)GetProcAddress(mf, "MFCreateSample");
	p_MFCreateMemoryBuffer = (PFN_MFCreateMemoryBuffer)GetProcAddress(mf, "MFCreateMemoryBuffer");
	p_CoInitializeEx = (PFN_CoInitializeEx)GetProcAddress(ole, "CoInitializeEx");
	p_CoUninitialize = (PFN_CoUninitialize)GetProcAddress(ole, "CoUninitialize");
	p_CoCreateInstance = (PFN_CoCreateInstance)GetProcAddress(ole, "CoCreateInstance");
	if (!p_MFStartup || !p_MFShutdown || !p_MFCreateMediaType || !p_MFCreateSample || !p_MFCreateMemoryBuffer || !p_CoInitializeEx || !p_CoUninitialize || !p_CoCreateInstance)
	{
		g_loadState = 2;
		return 0;
	}
	g_loadState = 1;
	return 1;
}

// ------------------------------------------------------------------------------------------------------------------------ helpers

int mfp_fail_r(struct mfh264_err *e, int code, HRESULT hr, const char *stage, int reason)
{
	if (e != NULL) { e->code = code; e->hr = (long)hr; e->stage = stage; e->reason = reason; e->exc_code = 0; }
	return code;
}

int mfp_fail(struct mfh264_err *e, int code, HRESULT hr, const char *stage) { return mfp_fail_r(e, code, hr, stage, 0); }

template <class T> static void rel(T *&p)
{
	if (p != NULL) { p->Release(); p = NULL; }
}

const char *mfh264_result_name(int code)
{
	switch (code)
	{
		case MFH264_OK: return "MFH264_OK";
		case MFH264_E_ARG: return "MFH264_E_ARG";
		case MFH264_E_OS: return "MFH264_E_OS";
		case MFH264_E_NOMF: return "MFH264_E_NOMF";
		case MFH264_E_NOINIT: return "MFH264_E_NOINIT";
		case MFH264_E_CREATE: return "MFH264_E_CREATE";
		case MFH264_E_ASYNC: return "MFH264_E_ASYNC";
		case MFH264_E_OUTTYPE: return "MFH264_E_OUTTYPE";
		case MFH264_E_INTYPE: return "MFH264_E_INTYPE";
		case MFH264_E_CODECAPI: return "MFH264_E_CODECAPI";
		case MFH264_E_START: return "MFH264_E_START";
		case MFH264_E_ALLOC: return "MFH264_E_ALLOC";
		case MFH264_E_INPUT: return "MFH264_E_INPUT";
		case MFH264_E_OUTPUT: return "MFH264_E_OUTPUT";
		case MFH264_E_STREAM_BROKEN: return "MFH264_E_STREAM_BROKEN";
		case MFH264_E_FORMAT: return "MFH264_E_FORMAT";
		case MFH264_E_STREAMCHANGE: return "MFH264_E_STREAMCHANGE";
		case MFH264_E_EXCEPTION: return "MFH264_E_EXCEPTION";
		case MFH264_E_RANGE: return "MFH264_E_RANGE";
		case MFH264_E_LEVEL: return "MFH264_E_LEVEL";
		case MFH264_E_HANG: return "MFH264_E_HANG";
		case MFH264_E_HWREJECT: return "MFH264_E_HWREJECT";
	}
	return "MFH264_?";
}

int mfh264_faulted(void) { return g_faulted != 0; }

// Table A-1 of the standard: level, MaxMBPS (macroblocks per second), MaxFS (macroblocks per picture). Levels 4.0 and 4.1 have the same limits.
static const struct { int level; unsigned mbps, fs; } g_levels[] = {
	{ 30, 40500, 1620 }, { 31, 108000, 3600 }, { 32, 216000, 5120 }, { 40, 245760, 8192 }, { 42, 522240, 8704 }, { 50, 589824, 22080 }, { 51, 983040, 36864 }, { 52, 2073600, 36864 } };

int mfh264_level_for_size(int width, int height)
{
	if (width < 1 || height < 1 || width > 16384 || height > 16384) { return 0; }
	const unsigned mbs = (unsigned)((width + 15) / 16) * (unsigned)((height + 15) / 16);
	const unsigned mbps = mbs * MFH264_NOMINAL_FPS;
	for (size_t i = 0; i < sizeof(g_levels) / sizeof(g_levels[0]); i++)
	{
		if (mbs <= g_levels[i].fs && mbps <= g_levels[i].mbps) { return g_levels[i].level; }
	}
	return 0;
}

// ---------------------------------------------------------------------------------------------------------- COM and Media Foundation

int mfp_init_impl(struct mfh264_err *err)
{
	if (tl_inits > 0) { tl_inits++; return MFH264_OK; }
	if (!mfp_os_ok()) { return mfp_fail(err, MFH264_E_OS, S_OK, "os"); }
	if (!mfp_load_libs()) { return mfp_fail(err, MFH264_E_NOMF, S_OK, "load mfplat/ole32"); }
	HRESULT hr = p_CoInitializeEx(NULL, COINIT_MULTITHREADED);
	int needUninit = 1;
	if (hr == RPC_E_CHANGED_MODE) { needUninit = 0; } // the thread is an STA already (not tested with the MFT, see the header)
	else if (FAILED(hr)) { return mfp_fail(err, MFH264_E_NOMF, hr, "CoInitializeEx"); }
	hr = p_MFStartup(MF_VERSION, MFSTARTUP_LITE);
	if (FAILED(hr))
	{
		if (needUninit) { p_CoUninitialize(); }
		return mfp_fail(err, MFH264_E_NOMF, hr, "MFStartup");
	}
	tl_coUninit = needUninit;
	tl_inits = 1;
	return MFH264_OK;
}

void mfp_shutdown_impl(void)
{
	if (tl_inits <= 0) { return; }
	if (--tl_inits > 0) { return; }
	p_MFShutdown();
	if (tl_coUninit) { p_CoUninitialize(); }
	tl_coUninit = 0;
}

// --------------------------------------------------------------------------------------------------------------- codec API access

HRESULT mfp_api_set_u32(ICodecAPI *a, const GUID &g, ULONG v)
{
	VARIANT var;
	memset(&var, 0, sizeof(var));
	var.vt = VT_UI4;
	var.ulVal = v;
	return a->SetValue(&g, &var);
}

HRESULT mfp_api_set_u64(ICodecAPI *a, const GUID &g, ULONGLONG v)
{
	VARIANT var;
	memset(&var, 0, sizeof(var));
	var.vt = VT_UI8;
	var.ullVal = v;
	return a->SetValue(&g, &var);
}

HRESULT mfp_api_set_bool(ICodecAPI *a, const GUID &g, int on)
{
	VARIANT var;
	memset(&var, 0, sizeof(var));
	var.vt = VT_BOOL;
	var.boolVal = on ? VARIANT_TRUE : VARIANT_FALSE;
	return a->SetValue(&g, &var);
}

// Integer value of a VARIANT that the encoder returned (no allocated members for these types, so no VariantClear).
static int var_to_i64(const VARIANT &v, int64_t *out)
{
	switch (v.vt)
	{
		case VT_I1: *out = v.cVal; return 1;
		case VT_UI1: *out = v.bVal; return 1;
		case VT_I2: *out = v.iVal; return 1;
		case VT_UI2: *out = v.uiVal; return 1;
		case VT_I4: case VT_INT: *out = v.lVal; return 1;
		case VT_UI4: case VT_UINT: *out = (int64_t)v.ulVal; return 1;
		case VT_I8: *out = v.llVal; return 1;
		case VT_UI8: *out = (int64_t)v.ullVal; return 1;
		case VT_BOOL: *out = v.boolVal ? 1 : 0; return 1;
	}
	return 0;
}

struct prop_def { const char *name; const GUID *guid; };

static const prop_def g_props[] = {
	{ "AVLowLatencyMode", &CODECAPI_AVLowLatencyMode },
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
	{ "AVEncSliceControlMode", &CODECAPI_AVEncSliceControlMode },
	{ "AVEncNumWorkerThreads", &CODECAPI_AVEncNumWorkerThreads },
	{ "AVEncVideoMeanAbsoluteDifference", &CODECAPI_AVEncVideoMeanAbsoluteDifference },
};
#define N_PROPS ((int)(sizeof(g_props) / sizeof(g_props[0])))
static_assert(N_PROPS <= MFH264_MAX_PROPS, "MFH264_MAX_PROPS too small");

// ------------------------------------------------------------------------------------------------------------------ MFT and types

static HRESULT create_mft(IMFTransform **mft, ICodecAPI **api)
{
	*mft = NULL;
	*api = NULL;
	HRESULT hr = p_CoCreateInstance(kMfh264_CLSID_EncoderMFT, NULL, CLSCTX_INPROC_SERVER, MFH264_IID(IMFTransform), (void **)mft);
	if (FAILED(hr)) { return hr; }
	hr = (*mft)->QueryInterface(MFH264_IID(ICodecAPI), (void **)api);
	if (FAILED(hr)) { *api = NULL; } // the caller decides whether it can do without
	return S_OK;
}

static int mft_is_async(IMFTransform *mft)
{
	IMFAttributes *at = NULL;
	UINT32 v = 0;
	int r = 0;
	if (SUCCEEDED(mft->GetAttributes(&at)) && at != NULL)
	{
		if (SUCCEEDED(at->GetUINT32(MF_TRANSFORM_ASYNC, &v)) && v != 0) { r = 1; }
		at->Release();
	}
	return r;
}

static UINT64 pack32(int hi, int lo) { return ((UINT64)(UINT32)hi << 32) | (UINT32)lo; }

// H.264 output type or NV12 input type of the given size at the nominal frame rate. withColor adds the BT.709, limited range signalling;
// level (level_idc, 0 = none) is written into the output type.
HRESULT mfp_make_type(int isOutput, const mfh264_cfg &c, int withColor, int level, IMFMediaType **pt)
{
	IMFMediaType *t = NULL;
	HRESULT hr = p_MFCreateMediaType(&t);
	if (FAILED(hr)) { return hr; }
	hr = t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	if (SUCCEEDED(hr)) { hr = t->SetGUID(MF_MT_SUBTYPE, isOutput ? MFVideoFormat_H264 : MFVideoFormat_NV12); }
	if (SUCCEEDED(hr) && isOutput) { hr = t->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)c.bitrate); }
	if (SUCCEEDED(hr)) { hr = t->SetUINT64(MF_MT_FRAME_SIZE, pack32(c.width, c.height)); }
	if (SUCCEEDED(hr)) { hr = t->SetUINT64(MF_MT_FRAME_RATE, pack32(MFH264_NOMINAL_FPS, 1)); }
	if (SUCCEEDED(hr)) { hr = t->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, pack32(1, 1)); }
	if (SUCCEEDED(hr)) { hr = t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive); }
	if (SUCCEEDED(hr) && isOutput) { hr = t->SetUINT32(MF_MT_MPEG2_PROFILE, (UINT32)c.profile); }
	if (SUCCEEDED(hr) && isOutput && level > 0) { hr = t->SetUINT32(MF_MT_MPEG2_LEVEL, (UINT32)level); }
	if (SUCCEEDED(hr) && isOutput && c.keyspace > 0) { (void)t->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, (UINT32)c.keyspace); }	//
	if (SUCCEEDED(hr) && !isOutput) { hr = t->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)c.width); }
	if (SUCCEEDED(hr) && withColor)
	{
		hr = t->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
		if (SUCCEEDED(hr)) { hr = t->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709); }
		if (SUCCEEDED(hr)) { hr = t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709); }
		if (SUCCEEDED(hr)) { hr = t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235); }
	}
	if (FAILED(hr)) { t->Release(); return hr; }
	*pt = t;
	return S_OK;
}

// Output type, then input type. Tried in this order: colour signalling and level, level only, colour only, neither. *colorOk / *levelOk say what was accepted.
int mfp_set_types(IMFTransform *mft, const mfh264_cfg &c, int level, int *colorOk, int *levelOk, struct mfh264_err *err)
{
	int firstCode = 0;
	HRESULT firstHr = S_OK;
	const char *firstStage = "set types";
	for (int att = 0; att < 4; att++)
	{
		const int color = (att == 0 || att == 2);
		const int lv = (att < 2) ? level : 0;
		if (att < 2 && level == 0) { continue; }
		IMFMediaType *o = NULL, *in = NULL;
		int code = 0;
		const char *stage = "";
		HRESULT hr = mfp_make_type(1, c, color, lv, &o);
		if (FAILED(hr)) { return mfp_fail(err, MFH264_E_OUTTYPE, hr, "build output type"); }
		hr = mft->SetOutputType(0, o, 0);
		o->Release();
		if (FAILED(hr)) { code = MFH264_E_OUTTYPE; stage = "SetOutputType"; }
		else
		{
			hr = mfp_make_type(0, c, color, 0, &in);
			if (FAILED(hr)) { return mfp_fail(err, MFH264_E_INTYPE, hr, "build input type"); }
			hr = mft->SetInputType(0, in, 0);
			in->Release();
			if (FAILED(hr)) { code = MFH264_E_INTYPE; stage = "SetInputType"; }
		}
		if (code == 0)
		{
			if (colorOk != NULL) { *colorOk = color; }
			if (levelOk != NULL) { *levelOk = (lv != 0); }
			return MFH264_OK;
		}
		if (firstCode == 0) { firstCode = code; firstHr = hr; firstStage = stage; }
	}
	return mfp_fail(err, firstCode != 0 ? firstCode : MFH264_E_INTYPE, firstHr, firstStage);
}

int mfp_check_cfg(const mfh264_cfg *c)
{
	if (c == NULL) { return 0; }
	if (c->width < 16 || c->height < 16 || c->width > MFH264_MAX_DIM || c->height > MFH264_MAX_DIM || (c->width & 15) || (c->height & 15)) { return 0; }
	if (c->profile != MFH264_PROFILE_BASELINE && c->profile != MFH264_PROFILE_MAIN && c->profile != MFH264_PROFILE_HIGH) { return 0; }
	if (c->max_level_idc < 0 || c->max_level_idc > 62 || c->rc < MFH264_RC_DEFAULT || c->rc > MFH264_RC_PEAKVBR) { return 0; }
	if (c->bitrate < 50000 || c->bitrate > 400000000 || c->qp_method < MFH264_QP_NONE || c->qp_method > MFH264_QP_QUALITY) { return 0; }
	if ((c->rc == MFH264_RC_QUALITY || c->rc == MFH264_RC_VBR) && c->qp_method == MFH264_QP_NONE) { return 0; } // no automatic choice of the form
	if (c->quality_vs_speed > 100 || c->gop < 0 || c->worker_threads < 0 || c->worker_threads > 64 || c->max_au_size < 0 || c->level_idc < 0 || c->level_idc > 62) { return 0; }
	return 1;
}

// The level of a picture of this size; 0 + MFH264_E_LEVEL in *code if it is too big for the caller.
int mfp_level_for_cfg(const mfh264_cfg *c, struct mfh264_err *err)
{
	int level = mfh264_level_for_size(c->width, c->height);
	const int maxLevel = c->max_level_idc > 0 ? c->max_level_idc : 52;
	if (level == 0) { mfp_fail(err, MFH264_E_LEVEL, S_OK, "picture is bigger than level 5.2 allows"); return 0; }
	if (c->level_idc != 0)
	{
		// the level of the caller (at a higher frame rate than the nominal one): it has to hold the picture
		const unsigned mbs = (unsigned)((c->width + 15) / 16) * (unsigned)((c->height + 15) / 16);
		int found = 0;
		for (size_t i = 0; i < sizeof(g_levels) / sizeof(g_levels[0]); i++)
		{
			if (g_levels[i].level == c->level_idc) { found = 1; if (mbs > g_levels[i].fs) { found = -1; } break; }
		}
		if (found != 1) { mfp_fail(err, MFH264_E_LEVEL, S_OK, "level_idc is unknown or cannot hold the picture"); return 0; }
		level = c->level_idc;
	}
	if (level > maxLevel) { mfp_fail(err, MFH264_E_LEVEL, S_OK, "picture needs a higher level than the viewer accepts"); return 0; }
	return level;
}

// ------------------------------------------------------------------------------------------------------------------------- probe

static int probe_impl(struct mfh264_caps *caps, struct mfh264_err *err)
{
	int i, m;
	if (caps == NULL) { return mfp_fail(err, MFH264_E_ARG, S_OK, "probe args"); }
	memset(caps, 0, sizeof(*caps));
	caps->os_ok = mfp_os_ok();
	caps->os_major = g_osMajor; caps->os_minor = g_osMinor; caps->os_build = g_osBuild;
	if (!caps->os_ok) { return mfp_fail(err, MFH264_E_OS, S_OK, "os"); }
	caps->mf_loaded = mfp_load_libs();
	if (!caps->mf_loaded) { return mfp_fail(err, MFH264_E_NOMF, S_OK, "load mfplat/ole32"); }
	if (tl_inits <= 0) { return mfp_fail(err, MFH264_E_NOINIT, S_OK, "probe"); }
	IMFTransform *mft = NULL;
	ICodecAPI *api = NULL;
	HRESULT hr = create_mft(&mft, &api);
	caps->create_hr = (long)hr;
	if (FAILED(hr)) { return mfp_fail(err, MFH264_E_CREATE, hr, "CoCreateInstance(CLSID_CMSH264EncoderMFT)"); }
	caps->mft_created = 1;
	caps->is_async = mft_is_async(mft);
	if (api == NULL) { rel(mft); return mfp_fail(err, MFH264_E_CODECAPI, E_NOINTERFACE, "QueryInterface(ICodecAPI)"); }
	caps->n_props = N_PROPS;
	for (i = 0; i < N_PROPS; i++)
	{
		mfh264_prop &p = caps->props[i];
		VARIANT vmin, vmax, vstep, vdef;
		memset(&vmin, 0, sizeof(vmin)); memset(&vmax, 0, sizeof(vmax)); memset(&vstep, 0, sizeof(vstep)); memset(&vdef, 0, sizeof(vdef));
		p.name = g_props[i].name;
		hr = api->IsSupported(g_props[i].guid);
		p.hr_supported = (long)hr;
		p.supported = (hr == S_OK);
		if (!p.supported) { continue; }
		p.modifiable = (api->IsModifiable(g_props[i].guid) == S_OK);
		if (SUCCEEDED(api->GetParameterRange(g_props[i].guid, &vmin, &vmax, &vstep)))
		{
			p.has_range = var_to_i64(vmin, &p.min) && var_to_i64(vmax, &p.max);
			if (!var_to_i64(vstep, &p.step)) { p.step = 0; }
			p.vt = vmin.vt;
		}
		if (SUCCEEDED(api->GetDefaultValue(g_props[i].guid, &vdef)))
		{
			p.has_default = var_to_i64(vdef, &p.def);
			if (p.vt == 0) { p.vt = vdef.vt; }
		}
	}
	for (m = 0; m < 7; m++) { caps->rc_mode_ok[m] = SUCCEEDED(mfp_api_set_u32(api, CODECAPI_AVEncCommonRateControlMode, (ULONG)m)); }
	api->Release();
	mft->Release();
	return MFH264_OK;
}

static int probe_size_impl(int width, int height, int profile, struct mfh264_err *err)
{
	if (width < 16 || height < 16 || width > 16384 || height > 16384 || (width & 15) || (height & 15)) { return mfp_fail(err, MFH264_E_ARG, S_OK, "size"); }
	if (!mfp_os_ok()) { return mfp_fail(err, MFH264_E_OS, S_OK, "os"); }
	if (!mfp_load_libs()) { return mfp_fail(err, MFH264_E_NOMF, S_OK, "load mfplat/ole32"); }
	if (tl_inits <= 0) { return mfp_fail(err, MFH264_E_NOINIT, S_OK, "probe_size"); }
	mfh264_cfg c = MFH264_CFG_INIT(width, height);
	c.profile = profile;
	const int level = mfp_level_for_cfg(&c, err);
	if (level == 0) { return MFH264_E_LEVEL; }
	IMFTransform *mft = NULL;
	ICodecAPI *api = NULL;
	HRESULT hr = create_mft(&mft, &api);
	if (FAILED(hr)) { return mfp_fail(err, MFH264_E_CREATE, hr, "CoCreateInstance(CLSID_CMSH264EncoderMFT)"); }
	int colorOk = 0, levelOk = 0;
	const int r = mfp_set_types(mft, c, level, &colorOk, &levelOk, err);
	rel(api);
	rel(mft);
	return r;
}

// ---------------------------------------------------------------------------------------------------------------------- encoder

struct mfh264
{
	IMFTransform *mft;
	ICodecAPI *api;
	IMFSample *inSample[MFH264_IN_RING];    // input samples with their buffers, in rotation
	IMFMediaBuffer *inBuf[MFH264_IN_RING];
	int inIdx;
	IMFSample *outSample;       // output sample that the MFT writes into (when it does not provide its own)
	IMFMediaBuffer *outBuf;
	DWORD outCap;
	int outProvides;
	LONGLONG frameDur;          // 100 ns at the nominal frame rate
	mfh264_cfg cfg;
	uint8_t *seqHdr;            // Annex B SPS+PPS from MF_MT_MPEG_SEQUENCE_HEADER
	size_t seqHdrLen;
	uint8_t *raw;               // the encoder's output before the clean-up
	size_t rawCap, rawLen;
	int expectIdr;              // an IDR is due: the first frame, mfh264_force_idr(), or after a flush
	int sampleQp, sampleQpValue;// mfh264_set_sample_qp(): the QP also goes into the input samples (1 = that of set_qp, 2 = sampleQpValue)
	int curQp;                  // the last QP of mfh264_set_qp() (0 = none)
	int outQpHave;              // the last output sample had MFSampleExtension_VideoEncodeQP ...
	ULONGLONG outQpRaw;         // ... with this value
	mfh264_stats st;
	mfh264_info info;
};

static void destroy_impl(mfh264 *h)
{
	if (h == NULL) { return; }
	if (h->mft != NULL)
	{
		h->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
		h->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
	}
	for (int i = 0; i < MFH264_IN_RING; i++) { rel(h->inBuf[i]); rel(h->inSample[i]); }
	rel(h->outBuf);
	rel(h->outSample);
	rel(h->api);
	rel(h->mft);
	free(h->seqHdr);
	free(h->raw);
	free(h);
}

static int start_code_len(const uint8_t *p, size_t left)
{
	if (left >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) { return 4; }
	if (left >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) { return 3; }
	return 0;
}

// Reads MF_MT_MPEG_SEQUENCE_HEADER of the current output type; keeps it only if it is an Annex B stream. Also takes profile / level from its SPS.
static void refresh_seq_header(mfh264 *h)
{
	IMFMediaType *t = NULL;
	if (FAILED(h->mft->GetOutputCurrentType(0, &t)) || t == NULL) { return; }
	UINT32 sz = 0;
	if (SUCCEEDED(t->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &sz)) && sz > 4 && sz < 4096)
	{
		uint8_t *b = (uint8_t *)malloc(sz);
		UINT32 got = 0;
		if (b != NULL && SUCCEEDED(t->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, b, sz, &got)) && got == sz && start_code_len(b, sz) != 0)
		{
			free(h->seqHdr);
			h->seqHdr = b;
			h->seqHdrLen = sz;
		}
		else { free(b); }
	}
	t->Release();
	h->info.has_seq_header = (h->seqHdr != NULL);
	h->info.seq_header_len = (int)h->seqHdrLen;
	if (h->seqHdr != NULL)
	{
		int p = 0, l = 0;
		if (kvm_annexb_sps_ids(h->seqHdr, h->seqHdrLen, &p, &l) == 0) { h->info.sps_profile_idc = p; h->info.sps_level_idc = l; }
	}
}

// Output stream info, and the output sample when the MFT does not provide its own.
static int setup_output(mfh264 *h, struct mfh264_err *err)
{
	MFT_OUTPUT_STREAM_INFO si;
	memset(&si, 0, sizeof(si));
	HRESULT hr = h->mft->GetOutputStreamInfo(0, &si);
	if (FAILED(hr)) { return mfp_fail(err, MFH264_E_START, hr, "GetOutputStreamInfo"); }
	h->outProvides = (si.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) ? 1 : 0; // CAN_PROVIDE (optional) is not used: we always offer our own sample
	h->info.out_provides_samples = h->outProvides;
	h->info.out_cb_size = (int)si.cbSize;
	if (!h->outProvides)
	{
		size_t want = (size_t)h->cfg.width * (size_t)h->cfg.height * 3 / 2 + 65536;
		if (si.cbSize > want) { want = si.cbSize; }
		if (h->outSample == NULL || want > h->outCap)
		{
			rel(h->outBuf);
			rel(h->outSample);
			if (FAILED(p_MFCreateSample(&h->outSample)) || FAILED(p_MFCreateMemoryBuffer((DWORD)want, &h->outBuf)) || FAILED(h->outSample->AddBuffer(h->outBuf)))
			{
				rel(h->outBuf);
				rel(h->outSample);
				return mfp_fail(err, MFH264_E_ALLOC, E_OUTOFMEMORY, "output sample");
			}
			h->outCap = (DWORD)want;
		}
	}
	return MFH264_OK;
}

// The input samples (3 in rotation), each with a memory buffer of one NV12 frame.
static int setup_input(mfh264 *h, struct mfh264_err *err)
{
	const DWORD need = (DWORD)((size_t)h->cfg.width * (size_t)h->cfg.height * 3 / 2);
	for (int i = 0; i < MFH264_IN_RING; i++)
	{
		if (FAILED(p_MFCreateSample(&h->inSample[i])) || FAILED(p_MFCreateMemoryBuffer(need, &h->inBuf[i])) || FAILED(h->inSample[i]->AddBuffer(h->inBuf[i])))
		{
			return mfp_fail(err, MFH264_E_ALLOC, E_OUTOFMEMORY, "input sample");
		}
	}
	return MFH264_OK;
}

static struct mfh264 *open_impl(const struct mfh264_cfg *cfg, struct mfh264_err *err)
{
	int r;
	if (!mfp_check_cfg(cfg)) { mfp_fail(err, MFH264_E_ARG, S_OK, "open args"); return NULL; }
	if (!mfp_os_ok()) { mfp_fail(err, MFH264_E_OS, S_OK, "os"); return NULL; }
	if (!mfp_load_libs()) { mfp_fail(err, MFH264_E_NOMF, S_OK, "load mfplat/ole32"); return NULL; }
	if (tl_inits <= 0) { mfp_fail(err, MFH264_E_NOINIT, S_OK, "open"); return NULL; }
	const int level = mfp_level_for_cfg(cfg, err);
	if (level == 0) { return NULL; }
	mfh264 *h = (mfh264 *)calloc(1, sizeof(mfh264));
	if (h == NULL) { mfp_fail(err, MFH264_E_ALLOC, E_OUTOFMEMORY, "calloc"); return NULL; }
	h->cfg = *cfg;
	h->frameDur = (LONGLONG)10000000 / MFH264_NOMINAL_FPS;
	h->expectIdr = 1;
	h->info.width = cfg->width;
	h->info.height = cfg->height;
	h->info.profile_requested = cfg->profile;
	h->info.level_requested = level;

	HRESULT hr = create_mft(&h->mft, &h->api);
	if (FAILED(hr)) { destroy_impl(h); mfp_fail(err, MFH264_E_CREATE, hr, "CoCreateInstance(CLSID_CMSH264EncoderMFT)"); return NULL; }
	if (h->api == NULL) { destroy_impl(h); mfp_fail(err, MFH264_E_CODECAPI, E_NOINTERFACE, "QueryInterface(ICodecAPI)"); return NULL; }
	h->info.async = mft_is_async(h->mft);
	if (h->info.async) { destroy_impl(h); mfp_fail(err, MFH264_E_ASYNC, S_OK, "MF_TRANSFORM_ASYNC"); return NULL; }

	// Properties that have to be there before streaming. They are set before the media types and, where the encoder refused them
	// that early, once more after the types (the result of the last attempt is in mfh264_info).
	for (int pass = 0; pass < 2; pass++)
	{
		if (pass == 1)
		{
			r = mfp_set_types(h->mft, h->cfg, level, &h->info.color_attrs_ok, &h->info.level_set_ok, err);
			if (r != MFH264_OK) { destroy_impl(h); return NULL; }
		}
		if (pass == 0 || !h->info.low_latency_ok) { h->info.low_latency_hr = (long)mfp_api_set_bool(h->api, CODECAPI_AVLowLatencyMode, 1); h->info.low_latency_ok = SUCCEEDED(h->info.low_latency_hr); }
		if (pass == 0 || !h->info.bframes_ok) { h->info.bframes_hr = (long)mfp_api_set_u32(h->api, CODECAPI_AVEncMPVDefaultBPictureCount, 0); h->info.bframes_ok = SUCCEEDED(h->info.bframes_hr); }
		// at most one reference frame: the viewer's decoder is set up for max_num_ref_frames <= 1 (docs 3.4); the encoder's default is not relied on
		if (pass == 0 || !h->info.maxref_ok) { h->info.maxref_hr = (long)mfp_api_set_u32(h->api, CODECAPI_AVEncVideoMaxNumRefFrame, 1); h->info.maxref_ok = SUCCEEDED(h->info.maxref_hr); }
		if (pass == 0 || !h->info.gop_ok)
		{
			// "largest GOP": the upper end of the range the encoder reports, else 0xFFFFFFFF, else 0x7FFFFFFF
			ULONG cand[3];
			int nc = 0;
			VARIANT vmin, vmax, vstep;
			memset(&vmin, 0, sizeof(vmin)); memset(&vmax, 0, sizeof(vmax)); memset(&vstep, 0, sizeof(vstep));
			if (h->cfg.gop > 0) { cand[nc++] = (ULONG)h->cfg.gop; }
			else
			{
				int64_t mx = 0;
				if (SUCCEEDED(h->api->GetParameterRange(&CODECAPI_AVEncMPVGOPSize, &vmin, &vmax, &vstep)) && var_to_i64(vmax, &mx) && mx > 0 && mx <= 0xFFFFFFFFll) { cand[nc++] = (ULONG)mx; }
				cand[nc++] = 0xFFFFFFFFul;
				cand[nc++] = 0x7FFFFFFFul;
			}
			h->info.gop_ok = 0;
			for (int k = 0; k < nc && !h->info.gop_ok; k++)
			{
				h->info.gop_hr = (long)mfp_api_set_u32(h->api, CODECAPI_AVEncMPVGOPSize, cand[k]);
				h->info.gop_ok = SUCCEEDED(h->info.gop_hr);
				if (h->info.gop_ok) { h->info.gop_value = (int64_t)cand[k]; }
			}
		}
		if (pass == 0 || !h->info.rc_mode_ok)
		{
			if (h->cfg.rc == MFH264_RC_DEFAULT) { h->info.rc_mode_ok = 1; h->info.rc_mode_hr = 0; }
			else
			{
				const ULONG mode = (h->cfg.rc == MFH264_RC_CBR) ? RC_CBR : (h->cfg.rc == MFH264_RC_QUALITY ? RC_QUALITY : (h->cfg.rc == MFH264_RC_LDVBR ? RC_LOWDELAY_VBR : (h->cfg.rc == MFH264_RC_PEAKVBR ? RC_PEAK_VBR : RC_VBR_UNCONSTRAINED)));
				h->info.rc_mode_hr = (long)mfp_api_set_u32(h->api, CODECAPI_AVEncCommonRateControlMode, mode);
				h->info.rc_mode_ok = SUCCEEDED(h->info.rc_mode_hr);
				if (h->cfg.rc == MFH264_RC_CBR || h->cfg.rc == MFH264_RC_LDVBR || h->cfg.rc == MFH264_RC_PEAKVBR) { mfp_api_set_u32(h->api, CODECAPI_AVEncCommonMeanBitRate, (ULONG)h->cfg.bitrate); }
				if (h->cfg.rc == MFH264_RC_PEAKVBR) { mfp_api_set_u32(h->api, CODECAPI_AVEncCommonMaxBitRate, (ULONG)h->cfg.bitrate); }
			}
		}
		if (h->cfg.quality_vs_speed >= 0 && (pass == 0 || !h->info.qvs_ok)) { h->info.qvs_hr = (long)mfp_api_set_u32(h->api, CODECAPI_AVEncCommonQualityVsSpeed, (ULONG)h->cfg.quality_vs_speed); h->info.qvs_ok = SUCCEEDED(h->info.qvs_hr); }
		else if (h->cfg.quality_vs_speed < 0) { h->info.qvs_ok = 1; }
		if (h->cfg.worker_threads > 0 && (pass == 0 || !h->info.workers_ok)) { h->info.workers_hr = (long)mfp_api_set_u32(h->api, CODECAPI_AVEncNumWorkerThreads, (ULONG)h->cfg.worker_threads); h->info.workers_ok = SUCCEEDED(h->info.workers_hr); }
		else if (h->cfg.worker_threads == 0) { h->info.workers_ok = 1; }
	}

	MFT_INPUT_STREAM_INFO ii;
	memset(&ii, 0, sizeof(ii));
	if (SUCCEEDED(h->mft->GetInputStreamInfo(0, &ii))) { h->info.in_cb_size = (int)ii.cbSize; h->info.in_alignment = (int)ii.cbAlignment; }
	r = setup_output(h, err);
	if (r == MFH264_OK) { r = setup_input(h, err); }
	if (r != MFH264_OK) { destroy_impl(h); return NULL; }
	hr = h->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
	if (FAILED(hr)) { destroy_impl(h); mfp_fail(err, MFH264_E_START, hr, "NOTIFY_BEGIN_STREAMING"); return NULL; }
	hr = h->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
	if (FAILED(hr)) { destroy_impl(h); mfp_fail(err, MFH264_E_START, hr, "NOTIFY_START_OF_STREAM"); return NULL; }
	refresh_seq_header(h);
	return h;
}

static HRESULT try_qp_method(mfh264 *h, int method, int qp)
{
	const ULONGLONG q = (ULONGLONG)qp;
	switch (method)
	{
		case MFH264_QP_ENCODEQP_UI4: return mfp_api_set_u32(h->api, CODECAPI_AVEncVideoEncodeQP, (ULONG)qp);
		case MFH264_QP_ENCODEQP_UI8: return mfp_api_set_u64(h->api, CODECAPI_AVEncVideoEncodeQP, q | (q << 16) | (q << 32));
		case MFH264_QP_FRAMETYPEQP: return mfp_api_set_u64(h->api, CODECAPI_AVEncVideoEncodeFrameTypeQP, q | (q << 16) | (q << 32));
		case MFH264_QP_QUALITY: return mfp_api_set_u32(h->api, CODECAPI_AVEncCommonQuality, (ULONG)(100 - qp * 100 / 51));
	}
	return E_INVALIDARG;
}

static int set_qp_impl(struct mfh264 *h, int qp, struct mfh264_err *err)
{
	if (h == NULL || h->api == NULL) { return mfp_fail(err, MFH264_E_ARG, S_OK, "set_qp args"); }
	if (qp < 0 || qp > 51) { return mfp_fail(err, MFH264_E_RANGE, S_OK, "qp"); }
	if (h->cfg.qp_method == MFH264_QP_NONE) { return mfp_fail(err, MFH264_E_ARG, S_OK, "set_qp: the configuration has no QP form"); }
	const HRESULT hr = try_qp_method(h, h->cfg.qp_method, qp);
	if (FAILED(hr)) { return mfp_fail(err, MFH264_E_CODECAPI, hr, "set qp"); }
	h->info.qp_method = h->cfg.qp_method;
	h->curQp = qp;
	return MFH264_OK;
}

static int set_bitrate_impl(struct mfh264 *h, int bps, struct mfh264_err *err)
{
	if (h == NULL || h->api == NULL) { return mfp_fail(err, MFH264_E_ARG, S_OK, "set_bitrate args"); }
	if (bps < 50000 || bps > 400000000) { return mfp_fail(err, MFH264_E_RANGE, S_OK, "bitrate"); }
	const HRESULT hr = mfp_api_set_u32(h->api, CODECAPI_AVEncCommonMeanBitRate, (ULONG)bps);
	if (FAILED(hr)) { return mfp_fail(err, MFH264_E_CODECAPI, hr, "AVEncCommonMeanBitRate"); }
	if (h->cfg.rc == MFH264_RC_PEAKVBR) { mfp_api_set_u32(h->api, CODECAPI_AVEncCommonMaxBitRate, (ULONG)bps); }	// (the drain rate of the leaky bucket of the constrained mode follows the target)
	return MFH264_OK;
}

static int set_sample_qp_impl(struct mfh264 *h, int mode, int qp)
{
	if (h == NULL || mode < 0 || mode > 2 || qp < 0 || qp > 51) { return MFH264_E_ARG; }
	h->sampleQp = mode; h->sampleQpValue = qp;
	if (mode == 0) { for (int i = 0; i < MFH264_IN_RING; i++) { if (h->inSample[i] != NULL) { h->inSample[i]->DeleteItem(MFSampleExtension_VideoEncodeQP); } } }
	return MFH264_OK;
}

static int force_idr_impl(struct mfh264 *h, struct mfh264_err *err)
{
	if (h == NULL || h->api == NULL) { return mfp_fail(err, MFH264_E_ARG, S_OK, "force_idr args"); }
	const HRESULT hr = mfp_api_set_u32(h->api, CODECAPI_AVEncVideoForceKeyFrame, 1);
	if (FAILED(hr)) { return mfp_fail(err, MFH264_E_CODECAPI, hr, "AVEncVideoForceKeyFrame"); }
	h->st.force_calls++;
	h->expectIdr = 1;
	return MFH264_OK;
}

size_t mfh264_seq_header(const struct mfh264 *h, const uint8_t **p)
{
	if (p != NULL) { *p = (h != NULL) ? h->seqHdr : NULL; }
	return h != NULL ? h->seqHdrLen : 0;
}

size_t mfh264_max_au_size(const struct mfh264 *h)
{
	if (h == NULL) { return 0; }
	const size_t base = h->outProvides ? (size_t)h->cfg.width * (size_t)h->cfg.height * 3 / 2 + 65536 : (size_t)h->outCap;
	return base + h->seqHdrLen;
}

// The frame that was accepted gave no usable access unit: flush the encoder, restart the stream and make the next frame an IDR. Always returns `code`.
static int break_stream(mfh264 *h, struct mfh264_err *err, int code, HRESULT hr, const char *stage, int reason)
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
	h->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
	mfp_api_set_u32(h->api, CODECAPI_AVEncVideoForceKeyFrame, 1);
	h->expectIdr = 1;
	return mfp_fail_r(err, code, hr, stage, reason);
}

// One access unit from the encoder into h->raw (a unit that comes in several parts, MFT_OUTPUT_DATA_BUFFER_INCOMPLETE, is joined; a stream change
// is followed). Returns 1 = h->raw has an access unit, 0 = the encoder wants more input, < 0 = error (err filled; err->reason set for INCOMPLETE).
static int pull_raw(mfh264 *h, struct mfh264_err *err, int *parts)
{
	int nparts = 0;
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
			continue;
		}
		if (FAILED(hr))
		{
			if (h->outProvides && s != NULL) { s->Release(); }
			return mfp_fail(err, MFH264_E_OUTPUT, hr, "ProcessOutput");
		}
		if (s == NULL) { return mfp_fail(err, MFH264_E_OUTPUT, E_POINTER, "ProcessOutput gave no sample"); }
		{
			UINT64 qv = 0;
			h->outQpHave = SUCCEEDED(s->GetUINT64(MFSampleExtension_VideoEncodeQP, &qv)) ? 1 : 0;	// what the encoder says it used (does the QP we ask for reach it?)
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
			// more output for the same access unit follows: join it
			if (nparts >= MFH264_MAX_PARTS) { return mfp_fail_r(err, MFH264_E_STREAM_BROKEN, S_OK, "output split in too many parts", MFH264_BROKEN_INCOMPLETE); }
			continue;
		}
		if (parts != NULL) { *parts = nparts; }
		return 1;
	}
	return mfp_fail(err, MFH264_E_STREAMCHANGE, S_OK, "too many stream changes");
}

// nv12 != NULL: copy of an NV12 frame; else conversion of the BGRA picture straight into the input buffer.
static int encode_impl(struct mfh264 *h, const uint8_t *nv12, size_t nv12_len, const uint8_t *bgra, int sw, int sh, int sstride, int64_t ts_100ns,
					   uint8_t *out, size_t out_cap, size_t *out_len, int *is_idr, struct mfh264_err *err)
{
	if (out_len != NULL) { *out_len = 0; }
	if (is_idr != NULL) { *is_idr = 0; }
	if (h == NULL || out == NULL || out_len == NULL || is_idr == NULL || (nv12 == NULL && bgra == NULL)) { return mfp_fail(err, MFH264_E_ARG, S_OK, "encode args"); }
	const size_t need = (size_t)h->cfg.width * (size_t)h->cfg.height * 3 / 2;
	if (nv12 != NULL && nv12_len != need) { return mfp_fail(err, MFH264_E_ARG, S_OK, "nv12 size"); }
	if (nv12 == NULL && (sw < 1 || sh < 1 || sw > h->cfg.width || sh > h->cfg.height)) { return mfp_fail(err, MFH264_E_ARG, S_OK, "bgra size"); }

	const int idx = h->inIdx;
	h->inIdx = (idx + 1) % MFH264_IN_RING;
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
	if (h->sampleQp && (h->sampleQp == 2 ? h->sampleQpValue : h->curQp) > 0) { const ULONGLONG q = (ULONGLONG)(h->sampleQp == 2 ? h->sampleQpValue : h->curQp); h->inSample[idx]->SetUINT64(MFSampleExtension_VideoEncodeQP, q | (q << 16) | (q << 32)); }	// per-frame QP in the sample
	hr = h->mft->ProcessInput(0, h->inSample[idx], 0);
	if (FAILED(hr)) { return break_stream(h, err, MFH264_E_INPUT, hr, hr == MF_E_NOTACCEPTING ? "ProcessInput (MF_E_NOTACCEPTING: output pending, not 1 in 1 out)" : "ProcessInput", MFH264_BROKEN_NONE); }
	h->st.frames_in++;

	int parts = 0;
	int r = pull_raw(h, err, &parts);
	if (r < 0)
	{
		// the encoder failed or the output did not complete: the frame is lost
		const int code = (err != NULL) ? err->code : r;
		const int reason = (err != NULL) ? err->reason : 0;
		const long ehr = (err != NULL) ? err->hr : 0;
		const char *stage = (err != NULL && err->stage != NULL) ? err->stage : "ProcessOutput";
		return break_stream(h, err, code, (HRESULT)ehr, stage, reason);
	}
	if (r == 0) { return break_stream(h, err, MFH264_E_STREAM_BROKEN, S_OK, "no output for the frame (not 1 in 1 out)", MFH264_BROKEN_NO_OUTPUT); }
	if (parts > 1) { h->st.split_outputs++; }

	size_t limit = out_cap;
	if (h->cfg.max_au_size > 0 && limit > (size_t)h->cfg.max_au_size) { limit = (size_t)h->cfg.max_au_size; }
	struct kvm_au_info info;
	size_t n = 0;
	r = kvm_annexb_normalize(h->raw, h->rawLen, h->seqHdr, h->seqHdrLen, out, limit, &n, &info);
	if (r == KVM_ANNEXB_E_SIZE)
	{
		*out_len = n; // the size of the access unit that did not fit; nothing was written into out[]
		return break_stream(h, err, MFH264_E_STREAM_BROKEN, S_OK, "access unit does not fit into the output buffer / max_au_size", MFH264_BROKEN_OVERSIZE);
	}
	if (r != KVM_ANNEXB_OK)
	{
		return break_stream(h, err, MFH264_E_STREAM_BROKEN, S_OK, r == KVM_ANNEXB_E_FORMAT ? "output is not Annex B / has no slice" : (r == KVM_ANNEXB_E_NOHEADER ? "IDR without SPS/PPS and no sequence header" : "IDR does not start with the SPS"), MFH264_BROKEN_FORMAT);
	}

	// 1 in, 1 out: a second ProcessOutput must say "need more input". If it gives another access unit, the stream is broken (it would be lost).
	{
		struct mfh264_err ignore;
		memset(&ignore, 0, sizeof(ignore));
		if (pull_raw(h, &ignore, NULL) == 1) { *out_len = 0; return break_stream(h, err, MFH264_E_STREAM_BROKEN, S_OK, "a second access unit for one frame", MFH264_BROKEN_EXTRA_OUTPUT); }
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

// ------------------------------------------------------------------------------------------------------------------ public API
// Every call that reaches Media Foundation is guarded (MSVC): a structured exception switches the process wide fault flag on.

#ifdef _MSC_VER
static int fault(struct mfh264_err *e, unsigned long code)
{
	InterlockedExchange(&g_faulted, 1);
	if (e != NULL) { e->code = MFH264_E_EXCEPTION; e->hr = (long)code; e->stage = "structured exception"; e->reason = 0; e->exc_code = code; }
	return MFH264_E_EXCEPTION;
}
#endif

static int faulted_result(struct mfh264_err *e)
{
	return mfp_fail(e, MFH264_E_EXCEPTION, S_OK, "video is off after an earlier structured exception");
}

#ifdef _MSC_VER
#define MFH264_GUARDED(expr, err) \
	if (g_faulted) { return faulted_result(err); } \
	__try { return (expr); } __except (EXCEPTION_EXECUTE_HANDLER) { return fault((err), GetExceptionCode()); }
#else
#define MFH264_GUARDED(expr, err) \
	if (g_faulted) { return faulted_result(err); } \
	return (expr);
#endif

int mfh264_init(struct mfh264_err *err) { MFH264_GUARDED(mfp_init_impl(err), err) }
int mfh264_probe(struct mfh264_caps *caps, struct mfh264_err *err) { MFH264_GUARDED(probe_impl(caps, err), err) }
int mfh264_probe_size(int width, int height, int profile, struct mfh264_err *err) { MFH264_GUARDED(probe_size_impl(width, height, profile, err), err) }
int mfh264_set_qp(struct mfh264 *h, int qp, struct mfh264_err *err) { MFH264_GUARDED(set_qp_impl(h, qp, err), err) }
int mfh264_set_bitrate(struct mfh264 *h, int bps, struct mfh264_err *err) { MFH264_GUARDED(set_bitrate_impl(h, bps, err), err) }
int mfh264_force_idr(struct mfh264 *h, struct mfh264_err *err) { MFH264_GUARDED(force_idr_impl(h, err), err) }
int mfh264_set_sample_qp(struct mfh264 *h, int mode, int qp) { MFH264_GUARDED(set_sample_qp_impl(h, mode, qp), (struct mfh264_err *)NULL) }
int mfh264_last_out_qp(const struct mfh264 *h, uint64_t *raw)
{
	if (raw != NULL) { *raw = (h != NULL && h->outQpHave) ? (uint64_t)h->outQpRaw : 0u; }
	return (h != NULL && h->outQpHave) ? 1 : 0;
}

int mfh264_encode(struct mfh264 *h, const uint8_t *nv12, size_t nv12_len, int64_t ts_100ns, uint8_t *out, size_t out_cap, size_t *out_len, int *is_idr, struct mfh264_err *err)
{
	MFH264_GUARDED(encode_impl(h, nv12, nv12_len, NULL, 0, 0, 0, ts_100ns, out, out_cap, out_len, is_idr, err), err)
}

int mfh264_encode_bgra(struct mfh264 *h, const uint8_t *bgra, int width, int height, int stride, int64_t ts_100ns, uint8_t *out, size_t out_cap, size_t *out_len, int *is_idr, struct mfh264_err *err)
{
	MFH264_GUARDED(encode_impl(h, NULL, 0, bgra, width, height, stride, ts_100ns, out, out_cap, out_len, is_idr, err), err)
}

struct mfh264 *mfh264_open(const struct mfh264_cfg *cfg, struct mfh264_err *err)
{
	if (g_faulted) { faulted_result(err); return NULL; }
#ifdef _MSC_VER
	__try { return open_impl(cfg, err); }
	__except (EXCEPTION_EXECUTE_HANDLER) { fault(err, GetExceptionCode()); return NULL; }
#else
	return open_impl(cfg, err);
#endif
}

void mfh264_shutdown(void)
{
	if (g_faulted) { return; } // MFShutdown with a live (faulted) encoder: leaked on purpose
#ifdef _MSC_VER
	__try { mfp_shutdown_impl(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { fault(NULL, GetExceptionCode()); }
#else
	mfp_shutdown_impl();
#endif
}

void mfh264_close(struct mfh264 *h)
{
	if (g_faulted) { return; } // leaked on purpose
#ifdef _MSC_VER
	__try { destroy_impl(h); }
	__except (EXCEPTION_EXECUTE_HANDLER) { fault(NULL, GetExceptionCode()); }
#else
	destroy_impl(h);
#endif
}

void mfh264_get_stats(const struct mfh264 *h, struct mfh264_stats *st)
{
	if (st == NULL) { return; }
	if (h == NULL) { memset(st, 0, sizeof(*st)); return; }
	*st = h->st;
}

void mfh264_get_info(const struct mfh264 *h, struct mfh264_info *info)
{
	if (info == NULL) { return; }
	if (h == NULL) { memset(info, 0, sizeof(*info)); return; }
	*info = h->info;
	info->max_au_size = (int)mfh264_max_au_size(h);
}
