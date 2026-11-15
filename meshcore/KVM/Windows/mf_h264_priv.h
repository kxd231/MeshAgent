// KVM (Windows): what mf_h264.cpp (the software Microsoft H.264 MFT) shares with mf_h264_hw.cpp (the hardware MFT): the run time loading of Media Foundation, the OS version, the codec API setters,
// the media types and the level of a picture. Nothing here is a public interface of the agent (mf_h264.h is); it is the part of the two encoders that is the same. The caller has included <windows.h>,
// <mfapi.h>, <mftransform.h>, <strmif.h>, <codecapi.h> and mf_h264.h. The GUIDs of those headers are defined in mf_h264.cpp only (INITGUID), so both files link with the one definition.

#ifndef KVM_MF_H264_PRIV_H
#define KVM_MF_H264_PRIV_H

typedef HRESULT(WINAPI *PFN_MFStartup)(ULONG, DWORD);
typedef HRESULT(WINAPI *PFN_MFShutdown)(void);
typedef HRESULT(WINAPI *PFN_MFCreateMediaType)(IMFMediaType **);
typedef HRESULT(WINAPI *PFN_MFCreateSample)(IMFSample **);
typedef HRESULT(WINAPI *PFN_MFCreateMemoryBuffer)(DWORD, IMFMediaBuffer **);
typedef HRESULT(WINAPI *PFN_CoInitializeEx)(LPVOID, DWORD);
typedef void(WINAPI *PFN_CoUninitialize)(void);
typedef HRESULT(WINAPI *PFN_CoCreateInstance)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
typedef LONG(WINAPI *PFN_RtlGetVersion)(PRTL_OSVERSIONINFOW);

// loaded by mfp_load_libs() (NULL before it, and after it failed)
extern PFN_MFStartup p_MFStartup;
extern PFN_MFShutdown p_MFShutdown;
extern PFN_MFCreateMediaType p_MFCreateMediaType;
extern PFN_MFCreateSample p_MFCreateSample;
extern PFN_MFCreateMemoryBuffer p_MFCreateMemoryBuffer;
extern PFN_CoInitializeEx p_CoInitializeEx;
extern PFN_CoUninitialize p_CoUninitialize;
extern PFN_CoCreateInstance p_CoCreateInstance;

HMODULE mfp_load_system_dll(const wchar_t *name);
int mfp_os_ok(void);														// Windows 8 or later
int mfp_os_build(void);
int mfp_load_libs(void);													// mfplat.dll / ole32.dll and their functions: 1 = loaded
int mfp_fail_r(struct mfh264_err *e, int code, HRESULT hr, const char *stage, int reason);	// fills *e (if any), returns code
int mfp_fail(struct mfh264_err *e, int code, HRESULT hr, const char *stage);
// COM (MTA) and MFStartup for the calling thread, counted per thread, like mfh264_init() / mfh264_shutdown() but WITHOUT their guard of the process wide fault flag of the software encoder
int mfp_init_impl(struct mfh264_err *err);
void mfp_shutdown_impl(void);
HRESULT mfp_api_set_u32(ICodecAPI *a, const GUID &g, ULONG v);
HRESULT mfp_api_set_u64(ICodecAPI *a, const GUID &g, ULONGLONG v);
HRESULT mfp_api_set_bool(ICodecAPI *a, const GUID &g, int on);
// H.264 output type or NV12 input type of the size of the configuration at the nominal frame rate; withColor: BT.709, transfer 709, limited range 16-235; level (level_idc, 0 = none) goes into the output type
HRESULT mfp_make_type(int isOutput, const mfh264_cfg &c, int withColor, int level, IMFMediaType **pt);
// Output type, then input type, in this order, with colour and level, level only, colour only, neither: the first that the MFT accepts. *colorOk / *levelOk say what was accepted (either may be NULL).
int mfp_set_types(IMFTransform *mft, const mfh264_cfg &c, int level, int *colorOk, int *levelOk, struct mfh264_err *err);
int mfp_check_cfg(const mfh264_cfg *c);										// 1 = the configuration is usable
int mfp_level_for_cfg(const mfh264_cfg *c, struct mfh264_err *err);			// the level_idc to announce; 0 and *err (MFH264_E_LEVEL) when the picture is too big for the caller

#endif
