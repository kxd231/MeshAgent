// KVM (Windows): the encoder backend of the encoder thread that is the software Microsoft H.264 MFT (mf_h264.cpp). One table of the calls of mf_h264.h; a hardware MFT gets
// a table of its own next to this one and is chosen where kvm_encthr_start() is called. See kvm_encthr.h.

#include "kvm_encthr.h"

extern "C" const kvm_encbe kvm_encbe_mf =
{
	"software MFT", KVM_ENCBE_CAP_QP,
	mfh264_init, mfh264_shutdown, mfh264_faulted, mfh264_open, mfh264_close, mfh264_set_qp, mfh264_set_bitrate, mfh264_set_sample_qp, mfh264_last_out_qp, mfh264_force_idr,
	mfh264_encode_bgra, mfh264_encode, mfh264_get_info, mfh264_max_au_size, mfh264_seq_header
};
