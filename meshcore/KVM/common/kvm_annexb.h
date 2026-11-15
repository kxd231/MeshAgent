/*
KVM: Annex B access unit clean-up for the H.264 encoders (portable C99, no API of the operating system).

The encoders (Media Foundation on Windows, openh264 later) give access units that do not match the stream rules of the KVM video protocol
: the Microsoft encoder starts every access unit with an access unit delimiter (NAL 9) and may leave out
SPS/PPS. kvm_annexb_normalize() makes the access unit conform:
  - every NAL unit of type 9 (access unit delimiter) is removed, wherever it is; all other NAL units (SEI, slices, ...) are kept as they are;
  - an IDR access unit (the first slice NAL unit is type 5) gets the SPS and the PPS that it lacks, copied from `seqhdr` (an Annex B
    byte stream with SPS and PPS, e.g. MF_MT_MPEG_SEQUENCE_HEADER), in front of the first SEI or slice NAL unit;
  - an IDR access unit has to START with the SPS (after the clean-up); if something else is in front of it, the result is an error;
  - a non-IDR access unit starts with whatever the encoder put first (normally the first slice).
Start codes are copied as they are (3 or 4 bytes). The input has to begin with a start code and contain at least one slice (type 1 or 5).
*/

#ifndef KVM_ANNEXB_H
#define KVM_ANNEXB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum
{
	KVM_ANNEXB_OK = 0,
	KVM_ANNEXB_E_FORMAT = -1,		/* no start code at the beginning, or no slice NAL unit */
	KVM_ANNEXB_E_SIZE = -2,			/* out[] is too small: *out_len = the size that would be needed, nothing was written */
	KVM_ANNEXB_E_NOHEADER = -3,		/* IDR without SPS/PPS and seqhdr has none to give */
	KVM_ANNEXB_E_ORDER = -4			/* IDR access unit that does not start with the SPS after the clean-up */
};

struct kvm_au_info
{
	int idr;						/* the first slice is an IDR slice */
	int aud_removed;				/* number of NAL units of type 9 removed */
	int header_added;				/* SPS and/or PPS were copied from seqhdr */
	int slices;						/* number of slice NAL units (type 1 and 5) */
	int sps_profile_idc;			/* of the first SPS in the result, -1 if there is none */
	int sps_level_idc;				/* the same, level_idc */
};

/* raw[0..n) -> out[0..cap). seqhdr may be NULL (hn = 0). info may be NULL. Returns KVM_ANNEXB_OK or a negative KVM_ANNEXB_E_*. */
int kvm_annexb_normalize(const uint8_t *raw, size_t n, const uint8_t *seqhdr, size_t hn, uint8_t *out, size_t cap, size_t *out_len, struct kvm_au_info *info);

/* profile_idc and level_idc of the first SPS (type 7) in an Annex B buffer; 0 if found, -1 if there is none. */
int kvm_annexb_sps_ids(const uint8_t *p, size_t n, int *profile_idc, int *level_idc);

/* The fields of the first SPS of an Annex B buffer that the viewer's limits are checked with (profile, constraint flags byte: bit 0x40 = constraint_set1, level, max_num_ref_frames,
   size in macroblocks, frame_mbs_only, frame_cropping_flag, chroma format, luma bit depth). 0, or -1 when there is no SPS or it is cut short. */
struct kvm_sps_info
{
	int profile, constraint, level, numRefFrames, mbsW, mbsH, frameMbsOnly, cropping, chromaFormat, bitDepthLuma;
};
int kvm_annexb_sps_info(const uint8_t *p, size_t n, struct kvm_sps_info *info);

/* 1 if the buffer starts with a 3 or 4 byte start code and has a NAL unit header after it. */
int kvm_annexb_valid_start(const uint8_t *p, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* KVM_ANNEXB_H */
