/*
KVM (Linux): the H.264 video path of the capture process (X11 and DRM alike).

Motion in the picture is not sent as JPEG tiles but as H.264 frames (command 243 in MNG_JUMBO) that paint only the rectangles of the tiles that moved; the tiles are sharpened as
JPEG when the motion stops (150 ms without a change). The pieces:
  - ../common/kvm_video_core.c, kvm_motion.c, kvm_sharpen.c, kvm_video_rc.c, kvm_yuv.c, kvm_annexb.c: everything that needs no operating system (tested in Docker);
  - linux_h264.c: the encoder (openh264, loaded from the system at run time);
  - this module: the state machine of the video around them, the hooks of the tile loop (linux_tile.c) and of the capture loops (linux_kvm.c, linux_kvm_drm.c), the 243 frames
    through the glue of linux_kvm_stream.c.

Everything runs on the thread of the capture loop; the encoder is called in line (a frame of a few tiles takes a few milliseconds, a key frame of the whole screen some tens).
The differences from the Windows agent are deliberate and small: no encoder thread, no hardware encoder, no Auto of the agent (the viewer's Auto steers the frame timer with
command 5 / 13, as it does for an agent that has none), and no forced sharpening of tiles that wait for the window longer than a second (they wait until it opens).

Switches: -DKVM_VIDEO_DISABLE (compile time), MESH_KVM_VIDEO=0 in the environment of the agent (run time): 242 reason 6 for op 1. No libopenh264.so.7 / .8: reason 2.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef LINUX_VIDEO_H_
#define LINUX_VIDEO_H_

#include <stddef.h>
#include <stdint.h>
#include "linux_kvm_stream.h"

#if defined(KVM_STREAM_ENABLED) && !defined(KVM_VIDEO_DISABLE)
#define KVML_VIDEO_ENABLED 1
#endif

#ifdef KVML_VIDEO_ENABLED

#include "meshcore/KVM/common/kvm_stream.h"

// Start of the session in this process (kvml_session_begin, before kvm_stream_init): is the video possible? 1 = yes; else 0 and *reason is the reason of the 242 for op 1
// (2 no encoder on this system, 6 switched off).
int kvml_video_session_begin(int *reason);
void kvml_video_session_end(void);
// A 240 that kvm_stream_on_ctrl has handled (kvml_poll): the video code remembers what it has to do
void kvml_video_ctrl(const kvm_ctrl *c, unsigned actions, uint32_t atMs);
// Every iteration of the loop, after kvml_poll: stream state against video state, requests, start of a generation, retry after an error, repair, the rate control
void kvml_video_poll(void);
// The viewer asked for a REFRESH: every tile goes back to JPEG
void kvml_video_refresh(void);
int kvml_video_on(void);					// 1 while a generation is running
// ---- the tile loop (linux_tile.c) ----
// Start of the scan of a frame (after the picture was taken): the clock of the frame, the tiles that wait to be sharpened are marked
void kvml_video_frame_begin(void);
// The CRC of a tile was computed (getTileAt, once per tile and frame)
void kvml_video_note_crc(int row, int col, int crc);
// 1 = the tile that is sent now is motion (goes to the video), 0 = a picture (JPEG)
int kvml_video_class(int row, int col);
// 1 when the sharpening of this tile is wanted (the window gate of the sharpening applies to it)
int kvml_video_tile_sharpening(int row, int col);
// After the rectangle was coalesced: 1 = the rectangle [row0..row1] x [col0..col1] of class cls goes to the video and is not sent as JPEG
int kvml_video_claim_rect(int row0, int col0, int row1, int col1, int cls);
// A JPEG of the rectangle was made (bytes): its tiles leave the video, the bytes of the sharpening are counted
void kvml_video_jpeg_sent(int row0, int col0, int row1, int col1, long long bytes);
// After the tile loop of a frame: `desktop` is the picture (3 bytes a pixel, R G B, top row first, the width and the height padded to the tiles); the frame of the pending tiles is encoded
// and written if the window, the pace and the bucket allow
void kvml_video_emit(const void *desktop, long long desktopSize);

#else

static inline int kvml_video_session_begin(int *reason) { if (reason != NULL) { *reason = 1; } return 0; }
static inline void kvml_video_session_end(void) {}
static inline void kvml_video_poll(void) {}
static inline void kvml_video_refresh(void) {}
static inline int kvml_video_on(void) { return 0; }
static inline void kvml_video_frame_begin(void) {}
static inline void kvml_video_note_crc(int row, int col, int crc) { (void)row; (void)col; (void)crc; }
static inline int kvml_video_class(int row, int col) { (void)row; (void)col; return 0; }
static inline int kvml_video_tile_sharpening(int row, int col) { (void)row; (void)col; return 0; }
static inline int kvml_video_claim_rect(int row0, int col0, int row1, int col1, int cls) { (void)row0; (void)col0; (void)row1; (void)col1; (void)cls; return 0; }
static inline void kvml_video_jpeg_sent(int row0, int col0, int row1, int col1, long long bytes) { (void)row0; (void)col0; (void)row1; (void)col1; (void)bytes; }
static inline void kvml_video_emit(const void *desktop, long long desktopSize) { (void)desktop; (void)desktopSize; }

#endif /* KVML_VIDEO_ENABLED */
#endif /* LINUX_VIDEO_H_ */
