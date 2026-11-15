/*
KVM: the NV12 VIEW of the encoder. Portable C99, no operating system API. The encoder is shown a persistent NV12 picture V (the "view") instead of a fresh conversion of the whole screen:
the viewer paints only the rectangles of a frame from the decoded picture, so what the encoder sees outside them decides nothing but the size of the frames and how 1 - 3 pixels at the
edges look. Two switches:

  FROZEN (A): V is converted completely at every key frame (like before) and in the other frames only the macroblocks of the rectangles of the frame plus a RING of one macroblock (16 pixels)
              around them, where the deblocking filter reads the neighbours. Outside, V stays as it was: the encoder sees no change there, so the P frames carry nothing for it.
  FLAT (B):   at a key frame V is first made one flat colour (the dominant colour of the picture) and only the macroblocks of the frame, of the tiles that the viewer shows from the video (VIDEO or in
              flight) and their ring are converted: the key frame is proportional to the area that matters. The macroblocks that are touched after that for the first time (VIRGIN: ring included) are
              coded against the flat colour, which costs about their intra size once; the plan counts them so that the rate control can reserve and discount them.

Who does what (threads): kvm_nv12v_plan runs in the main loop (it keeps the map of the macroblocks that V holds real content of, `touched`); the plan is COPIED into the slot of the job of the encoder
(kvm_ej_submit copies the map, no pointer into the loop's memory goes to the thread); kvm_nv12v_apply runs on the encoder thread, on the buffer that thread owns. The plan and the apply never share memory.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef KVM_NV12VIEW_H
#define KVM_NV12VIEW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KVM_NV12V_TILE 32			/* the tile of the video tile bookkeeping (kvm_video_core.h KVM_VT_TILE) */
#define KVM_NV12V_MB 16
#define KVM_NV12V_MB_MAX 36864		/* the biggest grid of macroblocks that a job carries (4096 x 2304); a bigger picture is converted completely */
#define KVM_NV12V_VIDEO 2			/* the state VIDEO of kvm_vtiles (kvm_video_core.h KVM_VT_VIDEO) */

typedef struct kvm_nv12v
{
	int codedW, codedH, mbCols, mbRows;
	int frozen, flat, firstFull;	/* the switches; firstFull: the first key frame of the session is complete although FLAT is on (the proof of the first key frame) */
	int valid;						/* V holds a whole picture (a complete conversion or a flat key frame has been planned): until then every plan is complete */
	int sessionIdr;					/* a key frame has been planned since init */
	uint8_t *touched;				/* mbRows * mbCols: 1 = V has real content there since the last reset */
	uint8_t *map;					/* mbRows * mbCols: the macroblocks to convert (the plan) */
	uint8_t *tmp, *base;
	unsigned total;
} kvm_nv12v;

typedef struct kvm_nv12v_plan
{
	const uint8_t *map;				/* the macroblocks to convert (v->map, valid until the next plan); NULL for a complete conversion or `legacy` */
	unsigned realMbs;				/* macroblocks converted (the whole picture for a complete conversion) */
	unsigned virginMbs;				/* of them: never converted since the last flat key frame (0 unless FLAT) */
	unsigned totalMbs;
	int full;						/* convert the whole picture (kvm_yuv_bgra_to_nv12) */
	int flat;						/* make V flat first (dominant colour), then convert the map */
	int legacy;						/* the grid is too big for a job: the caller encodes the picture as it did before */
} kvm_nv12v_pl;

/* 0, or -1 (arguments, memory). codedW, codedH: the coded size (multiples of 32). flat implies frozen. */
int kvm_nv12v_init(kvm_nv12v *v, int codedW, int codedH, int frozen, int flat, int firstFull);
void kvm_nv12v_free(kvm_nv12v *v);
/* V is not what the encoder was given before (a new encoder or thread, a lost frame that the encoder has to be told about): the next plan is complete. */
void kvm_nv12v_invalidate(kvm_nv12v *v);

/* The plan of the next frame. tileRows x tileCols: the grid of the tiles (tileRows * 32 >= the screen height, <= codedH); cover: the tiles of the frame (the rectangles); st, fl: the base state and the mask of the
   frames in flight of every tile (kvm_vtiles; NULL = none); isKey: the frame is a key frame (the encoder is asked for one, or it is the first of a generation or follows a lost access unit). Returns 0 or -1. */
int kvm_nv12v_plan(kvm_nv12v *v, int tileRows, int tileCols, const uint8_t *cover, const uint8_t *st, const uint8_t *fl, int isKey, kvm_nv12v_pl *out);

/* On the encoder thread: brings `view` (the thread's NV12 buffer of the coded size) up to date from the picture bgra (width x height, stride, the coded size codedW x codedH) as the plan says: `map` is
   the copy of the plan's map (mbCols * mbRows bytes). full / flat as in the plan. Returns the number of macroblocks converted, or a negative number for bad arguments. */
int kvm_nv12v_apply(uint8_t *view, const uint8_t *bgra, int width, int height, int stride, int codedW, int codedH, const uint8_t *map, int mbCols, int mbRows, int full, int flat);

#ifdef __cplusplus
}
#endif

#endif /* KVM_NV12VIEW_H */
