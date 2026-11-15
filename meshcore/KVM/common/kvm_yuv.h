/*
KVM: BGRA to NV12 conversion for the H.264 encoders (portable C99, SSE2 fast path on x86).

Source:  8 bit B,G,R,A bytes per pixel (the alpha byte is ignored). `bgra` points to the FIRST ROW OF THE IMAGE (the top row), row y is at
         bgra + y * stride. stride may be any value with |stride| >= width * 4 (no alignment needed) and may be NEGATIVE: for a bottom-up
         bitmap (DIB section with a positive height, like the capture buffers of tile.cpp) pass bgra = bits + (height - 1) * pitch and
         stride = -pitch. Nothing is read outside rows 0 .. height-1, each of width * 4 bytes.
Result:  NV12 (4:2:0), BT.709 limited range: Y 16..235, Cb/Cr 16..240.
           Y  = 16  + 219/255 * (0.2126 R + 0.7152 G + 0.0722 B)
           Cb = 128 + 224/255 * 0.5 * (B - Y') / (1 - 0.0722)       (Y' = the full range luma of the same colour)
           Cr = 128 + 224/255 * 0.5 * (R - Y') / (1 - 0.2126)
         Chroma is the average of the 2x2 pixels, i.e. the sample sits at the CENTRE of the four pixels. The H.264 default for 4:2:0 is
         "left" siting (the sample lies on the left column of the block, vertically between the rows), so decoders that honour it put the
         chroma 0.5 pixel to the right of where it was measured. The encoders here do not signal a siting; the shift is accepted (it is
         invisible on text and flat colours, a slight colour fringe of half a pixel on sharp coloured edges).
         Integer fixed point (coefficients with 15 fractional bits, rounding to nearest); the result is within +-1 of the
         exact value. Grey stays exactly grey (Cb = Cr = 128). Black is (16,128,128), white is (235,128,128).

Layout of the output buffer (the caller allocates kvm_yuv_nv12_size(codedW, codedH) bytes, any alignment):
           Y plane   codedW x codedH bytes, stride codedW
           UV plane  codedW x codedH / 2 bytes, stride codedW, Cb and Cr interleaved (Cb first), directly after the Y plane

Padding: the coded size is the source size rounded up to a multiple of 32 (kvm_yuv_coded()), both dimensions. The area
outside the source is filled by EDGE REPLICATION: the Y columns and rows beyond the source repeat the last Y column and row, the
Cb/Cr samples beyond the source repeat the last chroma column and row (a copy, not a new average). An odd source width or height
needs no special case by the caller: the last chroma sample averages the last pixel with its own copy.
(Any even coded size >= the source size is accepted by kvm_yuv_bgra_to_nv12(); the agent passes the multiple of 32.)

Implementations: the scalar one is the reference; the SSE2 one (x86-64, or x86 built with SSE2) gives bit-identical
results. KVM_YUV_AUTO uses SSE2 where it was compiled in. No allocation, no globals, no calls to the operating system:
the functions can be called from any thread on different buffers.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef KVM_YUV_H
#define KVM_YUV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KVM_YUV_ALIGN 32		/* coded sizes are multiples of this */

enum
{
	KVM_YUV_AUTO = 0,			/* SSE2 if compiled in, else scalar */
	KVM_YUV_SCALAR = 1,			/* the reference implementation */
	KVM_YUV_SSE2 = 2			/* fails (returns -2) when SSE2 was not compiled in */
};

/* v rounded up to a multiple of KVM_YUV_ALIGN (v >= 1). */
int kvm_yuv_coded(int v);

/* Bytes of an NV12 image of the (even) coded size: codedW * codedH * 3 / 2. */
size_t kvm_yuv_nv12_size(int codedW, int codedH);

/* 1 when the SSE2 implementation is compiled in. */
int kvm_yuv_have_sse2(void);

/* Converts. Returns 0, -1 for bad arguments (NULL, width/height < 1, |stride| < width * 4, codedW < width, codedH < height,
   odd coded size, sizes above 16384), -2 when impl is KVM_YUV_SSE2 and SSE2 is not available. */
int kvm_yuv_bgra_to_nv12(const uint8_t *bgra, int width, int height, int stride,
						 uint8_t *nv12, int codedW, int codedH, int impl);

/* converts a REGION of the coded picture and leaves every other byte of nv12 alone. (x0, y0, rw, rh) are in the coded picture (x0, y0, rw, rh even,
   x0 + rw <= codedW, y0 + rh <= codedH); the region may reach into the padding. The bytes of the region are IDENTICAL to what kvm_yuv_bgra_to_nv12() writes there
   (SSE2 and scalar alike), including the padding by edge replication, whatever the rest of nv12 holds. Same arguments and results as kvm_yuv_bgra_to_nv12. */
int kvm_yuv_bgra_to_nv12_region(const uint8_t *bgra, int width, int height, int stride, uint8_t *nv12, int codedW, int codedH,
								int x0, int y0, int rw, int rh, int impl);

/* the whole picture one flat colour: Y, then Cb Cr interleaved (the values of kvm_yuv_rgb_to_yuv, or any). Returns 0, -1 for bad arguments. */
int kvm_yuv_fill_flat(uint8_t *nv12, int codedW, int codedH, uint8_t y, uint8_t cb, uint8_t cr);

/* the Y, Cb, Cr that a flat picture of the colour (b, g, r) has in the conversion above (the same fixed point formulas). */
void kvm_yuv_rgb_to_yuv(int b, int g, int r, uint8_t *y, uint8_t *cb, uint8_t *cr);

/* the dominant colour of the picture: a grid of at most 32 x 32 sample points, each channel quantised to 5 bits, the most frequent cell (the first one
   when equal) wins and its members are averaged. Returns 0, -1 for bad arguments. Cost: about a thousand pixel reads. */
int kvm_yuv_dominant(const uint8_t *bgra, int width, int height, int stride, int *b, int *g, int *r);

#ifdef __cplusplus
}
#endif

#endif /* KVM_YUV_H */
