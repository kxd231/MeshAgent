/*
KVM (Linux): the H.264 encoder of the capture process, openh264 loaded from the system at run time.

The library is never part of the agent (its patent licence covers only the binary that Cisco builds, and the distributions' own builds): dlopen() of libopenh264.so.7 or
libopenh264.so.8 (or the file named by MESH_KVM_OPENH264, which is for the tests), then the version it reports is checked against the layout of the structures of the
headers in openh264/ (BSD licensed, copied from the 2.3.1 release). A library that is not there or not understood is not an error of the agent: the answer to the viewer is
242 reason 2 and the picture stays JPEG.

One encoder per generation of the video: constant quantizer, one slice, no rate control, no frame skipping, no periodic key frame (the video code asks for them), the
profile and the level of the request, the SPS with the colour description of BT.709 (the conversion of kvm_yuv.c). The input is planar I420 of the coded size (three planes).

Everything runs on the thread of the capture loop; nothing here is thread safe.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef LINUX_H264_H_
#define LINUX_H264_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// results of kvml_h264_load and kvml_h264_open
#define KVML_H264_OK 0
#define KVML_H264_E_NOLIB -1		// no libopenh264.so.7 / .8 (or the file of MESH_KVM_OPENH264 does not load)
#define KVML_H264_E_SYMBOL -2		// the library lacks a function that is needed
#define KVML_H264_E_VERSION -3		// a version whose structures are not the ones of the headers (major 2, minor 3 to 7 are accepted)
#define KVML_H264_E_PARAM -4		// the encoder does not accept the parameters (size, profile, level)
#define KVML_H264_E_MEMORY -5
#define KVML_H264_E_ENCODE -6		// the encoder failed or gave nothing for a frame
#define KVML_H264_E_SIZE -7			// the access unit does not fit the buffer of the caller

typedef struct kvml_h264 kvml_h264;

// The sizes of the layer of the frame info that were computed for this platform (old = up to 2.3, new = from 2.4): tests
void kvml_h264_layer_sizes(size_t *oldSize, size_t *newSize);
// Loads the library once per process (later calls give the same answer). `why` (may be NULL) gets a line for the log. Returns KVML_H264_OK or a KVML_H264_E_* code.
int kvml_h264_load(char *why, size_t cap);
// The version of the loaded library (0 when it is not loaded)
void kvml_h264_version(unsigned *major, unsigned *minor, unsigned *revision);
// An encoder for pictures of codedW x codedH (multiples of 16), profile_idc 66 (constrained baseline, CAVLC), 77 or 100 (CABAC), level_idc (30 .. 52) and a quantizer (the
// frames of the first IDR and the P frames alike), at most `fps` frames a second. Returns NULL and *err (may be NULL) on a failure.
kvml_h264 *kvml_h264_open(int codedW, int codedH, int profile, int level, int qp, int fps, int *err);
void kvml_h264_close(kvml_h264 *e);
// The quantizer of the next frames (0..51)
int kvml_h264_set_qp(kvml_h264 *e, int qp);
// One picture: the Y plane (codedW x codedH, stride codedW), the U plane and the V plane (each codedW/2 x codedH/2, stride codedW/2; the planes are given separately, so that the
// Y plane of the converter of the capture loop is used as it is). forceIdr = the frame is a key frame. The access unit
// (Annex B, with the start codes; for a key frame with SPS and PPS in front) is written to out[0..cap) and its length to *len, *isIdr says whether it is a key frame.
// An encoder that skipped the frame (it never does with this setup) gives KVML_H264_E_ENCODE.
int kvml_h264_encode(kvml_h264 *e, const uint8_t *y, const uint8_t *u, const uint8_t *v, int forceIdr, uint8_t *out, size_t cap, size_t *len, int *isIdr);

#ifdef __cplusplus
}
#endif

#endif /* LINUX_H264_H_ */
