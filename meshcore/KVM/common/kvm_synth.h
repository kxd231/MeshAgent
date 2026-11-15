/*
KVM: deterministic test pictures in NV12. Portable C99, no operating system API.

The self test of the hardware encoder (kvm_hwtest.cpp) needs pictures that look like a desktop (text, a title bar, a side panel, small updates), that scroll, and that have a moving noisy area (video),
and it needs the same ones every time, so that two runs on the same machine give the same sizes. This is a small generator for them: a page of "text" (glyphs of 5 x 7 cells made of a hash of the
character code: not a font, but dense like one), coloured words, and the three kinds below. Not ClearType; the sizes of the access units of such pictures say nothing about a real desktop.
NV12: Y plane (width x height, limited range 16..235), then the interleaved UV plane (width x height / 2, 16..240), stride = width.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef KVM_SYNTH_H
#define KVM_SYNTH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum
{
	KVM_SYNTH_TEXT = 0,		/* the page; frame t: a blinking caret and a clock field (small updates of a still desktop) */
	KVM_SYNTH_SCROLL = 1,	/* the page scrolled by 4 rows a frame (it repeats every `height` rows: no jump) */
	KVM_SYNTH_VIDEO = 2		/* the page, and a noisy moving area in the middle quarter of the screen */
};

typedef struct kvm_synth
{
	int w, h;
	uint8_t *y, *uv;		/* the page */
} kvm_synth;

/* w, h: multiples of 16, at least 64 and at most 8192. 0, or -1 (arguments, memory). */
int kvm_synth_init(kvm_synth *s, int w, int h);
void kvm_synth_free(kvm_synth *s);
size_t kvm_synth_size(const kvm_synth *s);		/* bytes of one picture: w * h * 3 / 2 */
/* Picture number t (>= 0) of the kind into nv12[kvm_synth_size()]. Deterministic: the same (kind, t) gives the same bytes. */
void kvm_synth_frame(const kvm_synth *s, int kind, int t, uint8_t *nv12);

/* The same picture as a BGRA bitmap (top row first, stride in bytes, alpha 255): NV12 -> RGB with BT.709, limited range (the inverse of kvm_yuv.h). For the self test of the hardware encoder, which takes BGRA like the agent does. */
void kvm_synth_frame_bgra(const kvm_synth *s, int kind, int t, uint8_t *bgra, int stride);

#ifdef __cplusplus
}
#endif

#endif /* KVM_SYNTH_H */
