/*
KVM (Windows): H.264 encoder around the software Microsoft H.264 Encoder MFT (CLSID_CMSH264EncoderMFT, synchronous).

A small C interface (C++ inside). One access unit (Annex B byte stream) comes out for every NV12 frame that goes in. Media Foundation,
COM and the MFT are loaded at run time (mfplat.dll, ole32.dll, ntdll.dll through LoadLibrary/GetProcAddress, like dxgi_capture.cpp), so the
program still starts on systems without Media Foundation (Windows N editions, Windows 7) and nothing is added to the import table. The GUIDs
are defined in mf_h264.cpp (INITGUID): no mfuuid.lib / strmiids.lib. This header includes no Windows, agent or microstack header.
Needs Windows 8 or later (RtlGetVersion); on older systems every function that would create the encoder returns MFH264_E_OS.

Stream rules. What the encoder writes is cleaned up by kvm_annexb.c: access unit delimiters are removed, every IDR
access unit starts with SPS and PPS (copied from MF_MT_MPEG_SEQUENCE_HEADER if the encoder left them out), other NAL units (SEI) are kept.
The encoder is set to constant quantizer: rate control "Quality" and a QP form that was measured on Windows 10 19044 (VT_UI8 with the QP in
the three 16 bit fields I, P, B; VT_UI4 is refused, VBR and the default mode refuse any form). B-frames 0, low latency, GOP = the largest value,
so only IDR pictures that are asked for (the first one and mfh264_force_idr()) appear. The nominal frame rate in the media types is fixed at
30 fps (MFH264_NOMINAL_FPS); with a constant quantizer it does not influence the rate. The level is computed from the number of macroblocks
(mfh264_level_for_size) and set explicitly; the level and profile that the encoder really wrote are read from the SPS (mfh264_info).

A LOST ACCESS UNIT. Every call of mfh264_encode() that does not return 0 means: no usable access unit came out for this frame (nothing may be
sent; the contents of out[] are undefined). The result MFH264_E_STREAM_BROKEN covers: no output for the frame (the encoder is not 1 in, 1 out),
a second output for one frame, an output that is split in parts and does not complete, an access unit bigger than the caller's buffer or than
cfg.max_au_size (never truncated), an output that is not Annex B. The wrapper flushes the encoder and arranges that the next frame becomes an
IDR picture; the caller answers by sending that IDR (and, in the protocol, the repair of the tiles of the lost frame) and counts it for the
rule "at most one forced IDR per second" as its own business. mfh264_max_au_size() is the buffer size with which the "bigger than the buffer"
case cannot happen.

INPUT SAMPLES (assumption). The encoder is driven one frame in, one frame out: mfh264_encode() hands a frame to ProcessInput() and pulls its access unit with
ProcessOutput() before it returns. The input buffers are a ring of MFH264_IN_RING = 3 samples used in turn; a buffer is overwritten again three frames later.
That is only correct if the MFT has released the input of a frame by the time its output has been delivered (it does not keep a reference to a frame that already gave its
access unit, which is true of the software MFT with B-frames off and low latency on: the PSNR of every frame in the encoder probe measurement confirms it). An encoder that holds
input longer (reordering, B-frames, a hardware MFT with a pipeline) would read a buffer that is already being written: increase the ring (or the number of frames the
encoder may hold) before such an encoder is used. If the encoder refuses a frame because it has output pending (MF_E_NOTACCEPTING) the stream is reported broken.

Threads. mfh264_init() makes the calling thread an MTA COM thread (CoInitializeEx(COINIT_MULTITHREADED); RPC_E_CHANGED_MODE, i.e. the thread
is already an STA, is accepted; in the KVM child process wmain has called CoInitializeEx(MTA) already, so the call returns S_FALSE and
is balanced by its own CoUninitialize) and calls MFStartup(MF_VERSION, MFSTARTUP_LITE). All other calls on one encoder handle must come from
the thread that called mfh264_init(); the agent's design is one thread (the KVM main loop) for init, open, encode, set_qp, force_idr, close and
shutdown (mfh264_close() of every handle comes before mfh264_shutdown()). Different handles may be used from different threads if each of
them called mfh264_init(). Nothing here is thread safe on one handle. That the MFT may be used from an STA thread was not tested.

FAULTS. Every function that calls Media Foundation, COM or the encoder (init, probe, probe_size, open, set_qp, set_bitrate, force_idr, encode,
encode_bgra, close, shutdown) is wrapped in __try/__except in MSVC builds (the agent has /EHa). A structured exception sets the process wide
flag mfh264_faulted(): the call returns MFH264_E_EXCEPTION (err->exc_code = GetExceptionCode()), and from then on every call returns
MFH264_E_EXCEPTION at once, except mfh264_close() and mfh264_shutdown(), which do NOTHING (the encoder objects and Media Foundation stay
loaded and leaked on purpose: releasing or shutting down after a fault in the codec may fault again or hang). The caller switches video off
for the rest of the process. In builds that are not MSVC there is no guard.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef KVM_MF_H264_H
#define KVM_MF_H264_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MFH264_NOMINAL_FPS 30			/* frame rate written into the media types, see above */
#define MFH264_MAX_MBS 36864			/* macroblocks of a picture: the limit of levels 5.1 and 5.2 (4096x2304) */

/* Results. 0 = ok, < 0 = error (details in mfh264_err: stage, HRESULT, reason). */
enum
{
	MFH264_OK = 0,
	MFH264_E_ARG = -1,
	MFH264_E_OS = -2,				/* Windows older than 8 */
	MFH264_E_NOMF = -3,				/* mfplat.dll / ole32.dll or one of their functions is missing */
	MFH264_E_NOINIT = -4,			/* mfh264_init() was not called (or failed) */
	MFH264_E_CREATE = -5,			/* the encoder MFT cannot be created (CoCreateInstance) */
	MFH264_E_ASYNC = -6,			/* the MFT is asynchronous, which this wrapper does not drive */
	MFH264_E_OUTTYPE = -7,			/* SetOutputType refused (size, profile, level) */
	MFH264_E_INTYPE = -8,			/* SetInputType refused (size too large, NV12 not accepted) */
	MFH264_E_CODECAPI = -9,			/* ICodecAPI is missing or a property that is required failed */
	MFH264_E_START = -10,			/* ProcessMessage(BEGIN_STREAMING / START_OF_STREAM) failed */
	MFH264_E_ALLOC = -11,			/* memory / media buffer / sample allocation failed */
	MFH264_E_INPUT = -12,			/* ProcessInput failed (the encoder was flushed, the next frame is an IDR) */
	MFH264_E_OUTPUT = -13,			/* ProcessOutput failed (the encoder was flushed, the next frame is an IDR) */
	MFH264_E_STREAM_BROKEN = -14,	/* no usable access unit for this frame, see "A LOST ACCESS UNIT" above; err->reason says why */
	MFH264_E_FORMAT = -15,			/* the output is not an Annex B byte stream or cannot be made to follow the rules (treated like STREAM_BROKEN) */
	MFH264_E_STREAMCHANGE = -16,	/* stream change that could not be followed */
	MFH264_E_EXCEPTION = -17,		/* structured exception (MSVC), or an earlier one: video is off for this process */
	MFH264_E_RANGE = -18,			/* value out of range (QP, bit rate) */
	MFH264_E_LEVEL = -19,			/* the picture is bigger than level 5.2 allows (more than MFH264_MAX_MBS macroblocks) or above cfg.max_level_idc */
	MFH264_E_HANG = -20,			/* hardware encoder: a deadline passed (no credit, no output): the encoder is not called again (it may be blocked in the driver); the caller gives it up */
	MFH264_E_HWREJECT = -21			/* hardware encoder: it works, but its stream breaks a rule that the viewer has (SPS, B slices, cropping ...): err->stage says which */
};

/* err->reason of MFH264_E_STREAM_BROKEN */
enum
{
	MFH264_BROKEN_NONE = 0,
	MFH264_BROKEN_NO_OUTPUT = 1,	/* the frame gave no output (MF_E_TRANSFORM_NEED_MORE_INPUT) */
	MFH264_BROKEN_EXTRA_OUTPUT = 2,	/* a second access unit for one frame (MFT_OUTPUT_DATA_BUFFER_INCOMPLETE continues are joined, this is something else) */
	MFH264_BROKEN_INCOMPLETE = 3,	/* the output kept announcing more parts (MFT_OUTPUT_DATA_BUFFER_INCOMPLETE) beyond the limit or a part failed */
	MFH264_BROKEN_OVERSIZE = 4,		/* the access unit does not fit into the caller's buffer / cfg.max_au_size; *out_len = its size */
	MFH264_BROKEN_FORMAT = 5,		/* not Annex B, no slice, no SPS/PPS to put in front of an IDR, IDR not starting with SPS */
	MFH264_BROKEN_ERROR = 6			/* hardware encoder: a call of the encoder failed (ProcessInput refused the frame, ProcessOutput or the copy of its output failed: err->hr and err->stage say which); the stream was started again */
};

struct mfh264_err
{
	int code;						/* MFH264_E_* */
	long hr;						/* HRESULT of the failed call (0 if there was none; the exception code after MFH264_E_EXCEPTION) */
	const char *stage;				/* short name of the failed step, static string */
	int reason;						/* MFH264_BROKEN_* for MFH264_E_STREAM_BROKEN, else 0 */
	unsigned long exc_code;			/* GetExceptionCode() after MFH264_E_EXCEPTION */
};

/* Names of the results, e.g. "MFH264_E_OUTTYPE" (for logs). */
const char *mfh264_result_name(int code);

/* 1 after a structured exception in any call: video is switched off for the process (see FAULTS above). */
int mfh264_faulted(void);

/* Level (level_idc: 30 31 32 40 41 42 50 51 52) for a picture of this size at MFH264_NOMINAL_FPS (macroblocks per picture and per second
   against the limits of Table A-1), 0 if the picture is bigger than MFH264_MAX_MBS macroblocks. No encoder needed. */
int mfh264_level_for_size(int width, int height);

/* Loads the libraries, CoInitializeEx(MTA), MFStartup(LITE). Returns 0 or a negative MFH264_E_*. Calls are counted: every successful
   mfh264_init() needs one mfh264_shutdown() on the same thread. */
int mfh264_init(struct mfh264_err *err);
void mfh264_shutdown(void);

/* ------------------------------------------------------------------------------------------------------------------ probe */

enum { MFH264_MAX_PROPS = 24 };

struct mfh264_prop
{
	const char *name;				/* "AVLowLatencyMode", ... */
	int supported;					/* ICodecAPI::IsSupported == S_OK */
	int modifiable;					/* ICodecAPI::IsModifiable == S_OK (valid when supported; S_FALSE before the media types are set) */
	int has_range;					/* GetParameterRange succeeded: min / max / step are valid */
	int has_default;				/* GetDefaultValue succeeded */
	int64_t min, max, step, def;	/* integers as the encoder reports them (booleans: 0 / 1) */
	int vt;							/* VARTYPE of the default / range values (VT_UI4 = 19, VT_UI8 = 21, VT_BOOL = 11, ...), 0 if unknown */
	long hr_supported;				/* HRESULT of IsSupported */
};

struct mfh264_caps
{
	int os_major, os_minor, os_build;
	int os_ok;						/* Windows 8 or later */
	int mf_loaded;					/* mfplat.dll and its functions */
	int mft_created;				/* the encoder MFT exists */
	int is_async;					/* the MFT reports MF_TRANSFORM_ASYNC (unexpected for the software encoder) */
	long create_hr;					/* HRESULT of the creation */
	int n_props;
	struct mfh264_prop props[MFH264_MAX_PROPS];
	/* AVEncCommonRateControlMode values that SetValue accepted on a fresh encoder (index = eAVEncCommonRateControlMode_*: 0 CBR,
	   1 PeakConstrainedVBR, 2 UnconstrainedVBR, 3 Quality, 4 LowDelayVBR, 5 GlobalVBR, 6 GlobalLowDelayVBR) */
	int rc_mode_ok[8];
};

/* Creates a throw-away encoder and fills in everything above. Needs mfh264_init(). Returns 0 or a negative result (caps->create_hr and
   err tell more; the OS fields are filled in whatever happens). */
int mfh264_probe(struct mfh264_caps *caps, struct mfh264_err *err);

/* 0 when a throw-away encoder accepts this frame size (multiple of 16; output type with the computed level, then NV12 input type) for the
   given profile; else a negative result (MFH264_E_LEVEL above level 5.2, MFH264_E_OUTTYPE / MFH264_E_INTYPE ...) and err->hr. */
int mfh264_probe_size(int width, int height, int profile, struct mfh264_err *err);

/* ----------------------------------------------------------------------------------------------------------------- encoder */

enum { MFH264_PROFILE_BASELINE = 66, MFH264_PROFILE_MAIN = 77, MFH264_PROFILE_HIGH = 100 };

enum
{
	MFH264_RC_DEFAULT = 0,			/* do not touch the rate control mode of the encoder (tool only) */
	MFH264_RC_CBR = 1,				/* AVEncCommonRateControlMode = CBR, AVEncCommonMeanBitRate = bitrate (tool only: it follows the target too slowly) */
	MFH264_RC_QUALITY = 2,			/* mode = Quality: the quantizer is set with mfh264_set_qp(). THE PRESET OF THE AGENT */
	MFH264_RC_VBR = 3,				/* mode = UnconstrainedVBR (tool only: refuses every QP form) */
	MFH264_RC_LDVBR = 4,			/* mode = LowDelayVBR (4), bit rate with mfh264_set_bitrate() (the way to steer an encoder that ignores the QP; whether this MFT takes it is what the tool's qpsample finds out) */
	MFH264_RC_PEAKVBR = 5			/* mode = PeakConstrainedVBR (1), AVEncCommonMaxBitRate = the bit rate (tool and the sharpening governor fallback) */
};

enum
{
	MFH264_QP_NONE = 0,				/* only with MFH264_RC_DEFAULT / MFH264_RC_CBR (mfh264_set_qp() is then not allowed) */
	MFH264_QP_ENCODEQP_UI4 = 1,		/* AVEncVideoEncodeQP as VT_UI4: refused by Windows 10 19044 (kept for the tool's detection) */
	MFH264_QP_ENCODEQP_UI8 = 2,		/* CODECAPI_AVEncVideoEncodeQP as VT_UI8, the same QP in the three 16 bit fields I, P, B. THE PRESET OF THE AGENT */
	MFH264_QP_FRAMETYPEQP = 3,		/* CODECAPI_AVEncVideoEncodeFrameTypeQP as VT_UI8 (QP of I | P << 16 | B << 32): measured identical to the above */
	MFH264_QP_QUALITY = 4			/* AVEncCommonQuality = 100 - qp * 100 / 51 (works but is coarse; tool only) */
};

struct mfh264_cfg
{
	int width, height;				/* coded size, multiples of 16 (a size that is not a multiple of 16 would get frame cropping in the SPS, which the
									   protocol forbids); the caller pads to 32 (kvm_yuv_coded); no cropping is signalled */
	int profile;					/* MFH264_PROFILE_* */
	int max_level_idc;				/* highest level that the caller can announce (the viewer's maxLevelIdc), 0 = 52. A picture that needs more: MFH264_E_LEVEL */
	int rc;							/* MFH264_RC_*, the agent uses MFH264_RC_QUALITY */
	int bitrate;					/* bits per second: MF_MT_AVG_BITRATE (required by the encoder, no effect with Quality) and the CBR target */
	int qp_method;					/* MFH264_QP_*: 1..4 with MFH264_RC_QUALITY / MFH264_RC_VBR (there is no automatic choice here), 0 otherwise */
	int quality_vs_speed;			/* CODECAPI_AVEncCommonQualityVsSpeed 0..100 (0 = fastest); < 0 leaves the default */
	int gop;						/* AVEncMPVGOPSize; 0 = the largest value the encoder accepts (only forced IDR pictures) */
	int worker_threads;				/* CODECAPI_AVEncNumWorkerThreads, 0 = the encoder's default (the measurement showed 1.5 of 2 cores in use) */
	int max_au_size;				/* largest access unit that may be returned, bytes; 0 = mfh264_max_au_size(). Bigger: MFH264_E_STREAM_BROKEN / OVERSIZE */
	int level_idc;					/* level written into the output type (30 31 32 40 42 50 51 52); 0 = the level for the size at MFH264_NOMINAL_FPS (mfh264_level_for_size).
									   A level that cannot hold the picture (MaxFS) or is above max_level_idc: MFH264_E_LEVEL. kvm_video computes the level at 60 frames per second (docs 5.2) */
	int64_t hw_luid;				/* hardware encoder: the adapter (LUID, high part << 32 | low part) of the output that is captured: its encoder is preferred, 0 = none (GDI) */
	int order;						/* hardware encoder, the diagnostics of the self test: 0 = the order of the settings A (types, then the codec API) and, when the GOP does not hold, B (the codec API, then the types); 1 = only A; 2 = only B */
	unsigned gop_only;				/* the diagnostics: the only GOP candidate (0 = the chain: range maximum, 0x7FFFFFFF, 0xFFFFFFFF) */
	int keyspace;					/* the diagnostics: MF_MT_MAX_KEYFRAME_SPACING in the output type (0 = not set) */
	int vbv;						/* hardware encoder with CBR / LDVBR: 0 = AVEncCommonBufferSize is not set (the agent); n = it is set to bit rate / n (the diagnostics of the self test: 30 = one frame) */
};

#define MFH264_CFG_INIT(w, h) { (w), (h), MFH264_PROFILE_HIGH, 0, MFH264_RC_QUALITY, 4000000, MFH264_QP_ENCODEQP_UI8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }

struct mfh264_stats
{
	uint64_t frames_in;				/* frames given to the encoder */
	uint64_t frames_out;			/* access units returned */
	uint64_t no_output;				/* frames that gave no output (MFH264_BROKEN_NO_OUTPUT) */
	uint64_t extra_output;			/* a second output for one frame (MFH264_BROKEN_EXTRA_OUTPUT) */
	uint64_t incomplete;			/* outputs that did not complete (MFH264_BROKEN_INCOMPLETE) */
	uint64_t oversize;				/* access units that did not fit (MFH264_BROKEN_OVERSIZE) */
	uint64_t broken;				/* all calls that returned a negative result after the frame was accepted (flush + IDR) */
	uint64_t split_outputs;			/* access units that came in several parts and were joined */
	uint64_t stream_changes;		/* MF_E_TRANSFORM_STREAM_CHANGE handled */
	uint64_t idr;					/* access units with an IDR slice (first slice type 5) */
	uint64_t idr_forced;			/* of them: the frame had mfh264_force_idr() before it, or follows a flush */
	uint64_t idr_unforced;			/* IDR access units other than the first one that nobody asked for (periodic IDR: GOP does not do what was asked) */
	uint64_t headers_added;			/* IDR access units that got SPS+PPS from MF_MT_MPEG_SEQUENCE_HEADER */
	uint64_t aud_removed;			/* access unit delimiters removed */
	uint64_t force_calls;			/* mfh264_force_idr() calls that the encoder accepted */
};

struct mfh264_info
{
	int width, height;
	int profile_requested;
	int level_requested;			/* level_idc that was computed, 0 if none */
	int level_set_ok;				/* the output type with MF_MT_MPEG2_LEVEL was accepted (else it was set without) */
	int sps_profile_idc;			/* profile_idc / level_idc of the SPS of the encoder (from its sequence header, then from every IDR), 0 until known */
	int sps_level_idc;
	int async;
	int out_provides_samples;
	int in_cb_size, in_alignment;	/* the encoder's input stream info (cbSize may be 0) */
	int out_cb_size;
	int max_au_size;				/* mfh264_max_au_size() */
	int has_seq_header;				/* MF_MT_MPEG_SEQUENCE_HEADER found (Annex B) */
	int seq_header_len;
	int low_latency_ok;				/* the properties below were accepted at open (1) or not (0) */
	int bframes_ok;
	int gop_ok;
	int gop_pass;					/* hardware encoder: a GOP candidate was accepted and read back as it was set (or could not be read back) */
	int64_t gop_read;				/* the AVEncMPVGOPSize that the encoder reads back after the types and the settings (-1 = it does not say); a finite value <= 10000 is a periodic key frame */
	int gop_order;					/* the order of the settings that was kept: 'A' or 'B' (0 = software encoder) */
	int rc_read;					/* the AVEncCommonRateControlMode that is read back (-1 = it does not say) */
	char gop_trace[200];			/* the diagnostics: order, every GOP candidate (value:hr:read), the mode that was set and read */
	int maxref_ok;					/* AVEncVideoMaxNumRefFrame = 1 was accepted */
	long maxref_hr;
	int64_t gop_value;				/* the AVEncMPVGOPSize that was applied (0 if none) */
	int rc_mode_ok;
	int qvs_ok;
	int workers_ok;
	int qp_method;					/* MFH264_QP_* that mfh264_set_qp() last used successfully, 0 = none yet */
	long low_latency_hr, bframes_hr, gop_hr, rc_mode_hr, qvs_hr, workers_hr;
	int color_attrs_ok;				/* BT.709 / limited range attributes accepted on both media types */
	/* what a hardware encoder says about itself (zero for the software encoder) */
	int backend;					/* 0 = software MFT, 1 = hardware MFT */
	unsigned hw_vendor, hw_device;	/* PCI ids of the adapter that the encoder was chosen for (0 = not known) */
	int max_inflight;				/* inputs given to the encoder before its first output of a stream came back (the contract is 1) */
	int open_ms;					/* time of the creation of the encoder up to its first request for input */
	unsigned out_wait_us_last, out_wait_us_max;	/* ProcessInput to HaveOutput of the last frame and the longest of this encoder (the glue's enc_ms includes the conversion) */
	unsigned events_need, events_have, events_other;	/* METransformNeedInput, METransformHaveOutput and the other events counted */
	unsigned flushes;				/* flushes of the stream after a lost frame (credits and the event queue were cleared) */
	unsigned stream_changes;		/* hardware encoder: MF_E_TRANSFORM_STREAM_CHANGE answers of ProcessOutput (each one uses up its HaveOutput event) */
	unsigned sc_event, sc_no_event;	/* of them: followed by a new HaveOutput within the deadline / by none (then one ProcessOutput without an event is tried) */
	unsigned sc_wait_ms_last, sc_wait_ms_max;	/* the wait for the HaveOutput after a stream change (the last one, the longest) in milliseconds */
	unsigned open_have;				/* HaveOutput events that came before the first input (counted at the end of the open) */
	unsigned sc_unasked;			/* stream changes met by the call of ProcessOutput for an event that nobody waited for (before a frame) */
	unsigned sc_need_wait;			/* NeedInput events that came while the wait after a stream change ran */
	long sc_set_hr;					/* the result of SetOutputType at the last stream change (MF_E_NO_MORE_TYPES: no H.264 type among the available ones) */
	int sc_type_idx;				/* the index of the available output type that was chosen at the last stream change (the first with the subtype H.264), -1 = none; valid when stream_changes > 0 */
	unsigned sc_type_n, sc_subtype;	/* how many output types the MFT lists, and Data1 of the subtype GUID of the chosen one (0x34363248 = H264) */
	unsigned out_fail;				/* ProcessOutput calls that failed (not NEED_MORE_INPUT, not a stream change) */
	long out_fail_hr;				/* the HRESULT of the last of them (0 = none) */
	char hw_name[48];				/* the name of the MFT */
};

struct mfh264;						/* opaque */

/* Creates the encoder and starts the stream. Order: codec API properties, output type (H.264, level), input type (NV12), BEGIN_STREAMING /
   START_OF_STREAM. Returns NULL with *err set on failure. */
struct mfh264 *mfh264_open(const struct mfh264_cfg *cfg, struct mfh264_err *err);

/* Constant quantizer for the next frames (0..51), with the form cfg.qp_method. Returns 0 or a negative result. */
int mfh264_set_qp(struct mfh264 *h, int qp, struct mfh264_err *err);

/* Mean bit rate (bits per second) for the following frames (a rate control mode that listens to it: CBR, LowDelayVBR; with PeakConstrainedVBR the maximum bit rate too). The documentation of the
   H.264 encoder lets the average bit rate be set during encoding on Windows 8 and later ("if the bit rate changes, the encoder uses adaptive encoding"). */
int mfh264_set_bitrate(struct mfh264 *h, int bps, struct mfh264_err *err);

/* the QP of the next frames is set ALSO in the input sample (MFSampleExtension_VideoEncodeQP, UINT64, packed like AVEncVideoEncodeQP: I | P << 16 | B << 32), from mfh264_set_qp() on.
   The attribute is documented as an OUTPUT attribute of the H.264 encoder ("the QP that was used"); whether this MFT takes it on its input is what qpsample of kvm_h264_check finds out.
   mode 0 = off, 1 = the attribute carries the QP of mfh264_set_qp(), 2 = the attribute carries `qp` whatever the codec API was given (the tool: the attribute alone). */
int mfh264_set_sample_qp(struct mfh264 *h, int mode, int qp);
/* The QP attribute of the last output sample (MFSampleExtension_VideoEncodeQP) as the encoder wrote it: 1 = present, *raw = the UINT64 (0 and 0 when absent). */
int mfh264_last_out_qp(const struct mfh264 *h, uint64_t *raw);

/* The next frame given to mfh264_encode() becomes an IDR picture (SPS+PPS in front). */
int mfh264_force_idr(struct mfh264 *h, struct mfh264_err *err);

/* Size of the output buffer with which MFH264_BROKEN_OVERSIZE cannot happen because of the encoder's own output buffer: the encoder's output
   buffer plus the sequence header. Applies unless cfg.max_au_size is smaller. */
size_t mfh264_max_au_size(const struct mfh264 *h);

/* The sequence header (Annex B SPS + PPS, MF_MT_MPEG_SEQUENCE_HEADER) of the open encoder, valid until mfh264_close(); *p = NULL and 0 when the encoder gave none. The caller checks the SPS against the
   viewer's limits (kvm_annexb_sps_info, kvm_vcore_check_sps) before the first frame is sent. */
size_t mfh264_seq_header(const struct mfh264 *h, const uint8_t **p);

/* One NV12 frame (width * height * 3 / 2 bytes, stride = width, UV plane directly after the Y plane: kvm_yuv.h layout) in, one access unit out
   in Annex B format into out[0 .. out_cap), cleaned up as described above. *out_len = its size; *is_idr = 1 if it contains an IDR slice
   (every IDR starts with SPS and PPS). ts_100ns: presentation time of the frame (any increasing value; it does not matter with a constant
   quantizer). Returns 0 or a negative result: then nothing is delivered for this frame and the next successful access unit is an IDR. */
int mfh264_encode(struct mfh264 *h, const uint8_t *nv12, size_t nv12_len, int64_t ts_100ns, uint8_t *out, size_t out_cap, size_t *out_len, int *is_idr, struct mfh264_err *err);

/* The same from a BGRA picture (see kvm_yuv.h: pointer to the top row, stride may be negative for a bottom-up bitmap), converted straight
   into the encoder's input buffer (no intermediate NV12 buffer, no extra copy). width / height <= the coded size of cfg. */
int mfh264_encode_bgra(struct mfh264 *h, const uint8_t *bgra, int width, int height, int stride, int64_t ts_100ns, uint8_t *out, size_t out_cap, size_t *out_len, int *is_idr, struct mfh264_err *err);

void mfh264_get_stats(const struct mfh264 *h, struct mfh264_stats *st);
void mfh264_get_info(const struct mfh264 *h, struct mfh264_info *info);

/* Releases everything (NULL is allowed). Does nothing after a fault (see FAULTS above). */
void mfh264_close(struct mfh264 *h);

#ifdef __cplusplus
}
#endif

#endif /* KVM_MF_H264_H */
