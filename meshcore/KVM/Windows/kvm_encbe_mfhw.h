/*
KVM (Windows): the hardware H.264 encoder (a hardware Media Foundation transform: the GPU's own encoder, e.g. AMDh264Encoder) as an encoder backend of the encoder thread.

It is the second table of kvm_encbe (kvm_encthr.h) next to the software MFT (kvm_encbe_mf): the same calls of mf_h264.h, the same contract "one frame in, one access unit out" with a bit more behind it:

  - the hardware MFTs are ASYNCHRONOUS although their registration says nothing: MF_TRANSFORM_ASYNC_UNLOCK, a thread that waits for the events of the MFT (METransformNeedInput, METransformHaveOutput), one input
    only when the MFT has asked for it, ProcessOutput only after HaveOutput, and the MFT delivers its own output sample. Measured on the AMD GPU of the AMD laptop: depth 1, 8 ms, 1 NeedInput per frame.
  - EVERY wait has a deadline (KVM_MFHW_IN_MS 250, KVM_MFHW_OUT_MS 500, KVM_MFHW_OPEN_MS 2000). A deadline that passes (MFH264_E_HANG) means that the encoder may be blocked in the driver: the handle is dead, it is
    never called again, and mfh264_close() of it releases nothing (it leaks on purpose, like after an exception). The caller (the encoder thread) gives the backend up and goes on with the software one.
  - the QP only as VT_UI4 of AVEncVideoEncodeQP (MFH264_QP_ENCODEQP_UI4), FrameTypeQP or AVEncCommonQuality (cfg.qp_method; the packed form UI8 of the software encoder is refused by the AMD MFT), rate control Quality.
    IsModifiable is not asked (it says false for everything). The properties are set AFTER the media types (the only order that was measured).
  - the SPS of the stream is the one in the IDR access unit (level 40 on the AMD GPU while the sequence header says 42): the glue reads mfh264_info after the first IDR.
  - a structured exception in the driver sets the flag of the HARDWARE backend only (kvm_mfhw_faulted()); the software encoder stays usable.

THE CODES THAT THE CALLS RETURN (the contract that the glue relies on):
  MFH264_E_STREAM_BROKEN  the frame gave no usable access unit and the stream was started again (flush, START_OF_STREAM, forced IDR; the credit and the events of before the flush are forgotten): the handle is usable, the next
                          frame is an IDR. err->reason says why (NO_OUTPUT, EXTRA_OUTPUT, INCOMPLETE, OVERSIZE, FORMAT, ERROR = a call of the encoder failed), err->hr and err->stage the details. EVERY restart is this code,
                          also when ProcessInput refused the frame (MF_E_NOTACCEPTING) or ProcessOutput failed. Three in a row are the glue's to count (KVM_VIDEO_BROKEN_MAX).
  MFH264_E_HANG           the handle is DEAD: a deadline passed, MEError came, the queue of the events ended, or the encoder did not answer after a flush. It is not called again by anything (set_qp, force_idr, encode
                          return this at once) and mfh264_close() of it releases nothing. The glue goes back to the software encoder.
  MFH264_E_EXCEPTION      a structured exception in the driver (the flag of this backend is set: kvm_mfhw_faulted()). The glue ends the KVM process after it has written the counter.
  other codes             a call that failed before anything was started (arguments, the form of the QP, a range): the handle is usable.

The calls run on the thread of the encoder (kvm_encthr: be->init on the thread, every later call too), which never calls SetThreadDesktop. No D3D device, no window, no import: dxgi.dll and the Media Foundation functions
are loaded at run time.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef KVM_ENCBE_MFHW_H
#define KVM_ENCBE_MFHW_H

#include "kvm_encthr.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const kvm_encbe kvm_encbe_mfhw;

#ifndef KVM_MFHW_IN_MS
#define KVM_MFHW_IN_MS 250		// the encoder has to ask for the next input (METransformNeedInput) within this time
#endif
#ifndef KVM_MFHW_OUT_MS
#define KVM_MFHW_OUT_MS 500		// ... and to deliver the access unit (METransformHaveOutput) within this time of ProcessInput (8 ms measured, 25 ms for the first frame)
#endif
#ifndef KVM_MFHW_OPEN_MS
#define KVM_MFHW_OPEN_MS 2000	// ... and to be ready (the first NeedInput) within this time of the start of the stream
#endif

// What the selection of the encoder found (kvm_mfhw_select): the key of the cache of the self test (kvm_hwpolicy.h) is made of it
typedef struct kvm_mfhw_sel
{
	int found;							// a hardware encoder that takes NV12 and gives H.264 exists
	char name[48];						// MFT_FRIENDLY_NAME_Attribute
	char clsid[40];						// MFT_TRANSFORM_CLSID_Attribute, "{...}"
	unsigned vendor, device, subsys, adapterRev;	// PCI ids of the adapter of the encoder (DXGI), 0 when not known
	char driver[40];					// the version of the user mode driver of that adapter ("31.0.12018.5")
	int64_t luid;						// the LUID of the adapter (high << 32 | low), 0 when not known
	int nHw;							// hardware encoders that the system lists
	int onLuid;							// the encoder is the one of the adapter that was asked for (cfg.hw_luid)
} kvm_mfhw_sel;

// Which encoder mfh264_open() would use for this adapter (luid = the adapter of the captured output, 0 = any); nothing is activated. Needs be->init() to have run on the calling thread. 1 = found.
int kvm_mfhw_select(int64_t luid, kvm_mfhw_sel *out);
// The same on a thread of its own, at most timeoutMs: 1 = found, 0 = none, -1 = the time passed (the thread is left to end by itself; the caller treats the machine as one whose enumeration hangs). Needs Media
// Foundation to have been started in the process (the software or the hardware backend did it).
int kvm_mfhw_select_timed(int64_t luid, kvm_mfhw_sel *out, unsigned timeoutMs);
// Tests: the thread of kvm_mfhw_select_timed waits this long before it asks (a hang of the enumeration).
void kvm_mfhw_test_select_delay(unsigned ms);
// 1 after a structured exception in a call of the hardware backend: it is never called again in this process.
int kvm_mfhw_faulted(void);
// The counters of the hardware encoder (frames in and out, IDR, lost units, stream changes ...), as mfh264_get_stats() has them for the software one.
void kvm_mfhw_get_stats(const struct mfh264 *h, struct mfh264_stats *st);
// Tests: the deadlines in milliseconds (0 = leave); returns nothing. The scenarios of the Wine test with a mock encoder that does not answer would take minutes with the real ones.
void kvm_mfhw_test_set_deadlines(unsigned inMs, unsigned outMs, unsigned openMs);

#ifdef __cplusplus
}
#endif

#endif
