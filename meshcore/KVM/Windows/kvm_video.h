/*
KVM (Windows): the H.264 video path of the agent.

Motion in the picture is not sent as JPEG tiles but as H.264 frames (command 243 in MNG_JUMBO) that paint only the rectangles of the tiles that moved; the tiles
are sharpened as JPEG by WA-2 when the motion stops. The pieces:
  - ../common/kvm_video_core.c: everything that needs no operating system (tile bookkeeping, rectangles, level, IDR and error rules; tested in Docker);
  - mf_h264.cpp: the encoder (Microsoft H.264 Encoder MFT, loaded at run time, no new imports);
  - this module: the state machine of chapter 5.2 around them, the tile hooks of tile.cpp / kvm_adapt.cpp, the 243 and 242 frames through the glue of kvm.c.

Everything runs on the main loop thread of the KVM child process, except kvm_video_note_refresh() (input thread, one volatile counter). The state of the stream layer
(kvm_stream) is read and changed only under the lock of the glue in kvm.c (kvm_glue_*), the encoder is called outside it.

Only for the x64 MeshService build with the stream layer (KVM_VIDEO_ENABLED, kvm_adapt.h); everywhere else the stubs below keep the behaviour of the end-to-end ACK.
Switches: /DKVM_VIDEO_DISABLE (compile time), MESH_KVM_VIDEO=0 in the environment of the service (run time). Both: 242 reason 6 for op 1.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef KVM_VIDEO_H
#define KVM_VIDEO_H

#include <stdint.h>
#include "kvm_adapt.h"

#ifdef KVM_VIDEO_ENABLED

#include "../common/kvm_stream.h"


#ifdef __cplusplus
extern "C" {
#endif

// ---- implemented in kvm.c (the glue of the stream layer); main loop thread ----
void kvm_glue_lock(void);									// the lock of the stream state and of every write to the pipe
void kvm_glue_unlock(void);
kvm_stream *kvm_glue_stream(void);							// valid only while the lock is held
int  kvm_glue_write_locked(char *buffer, int len, int kind);	// lock held; kind KVM_W_*; returns the ILibTransport_DoneState
void kvm_glue_state_locked(const kvm_state_msg *m);		// lock held; writes the 242 and queues its log line
void kvm_glue_log_flush(void);								// after the lock is released
unsigned long long kvm_glue_cpu_us(void);					// CPU time of the process (kernel + user) in microseconds; 0 when the system does not say
unsigned long long kvm_glue_capture_cpu_us(void);			// the same for the capture thread alone (0 when unknown): the encoder's CPU is what the process used minus this
unsigned kvm_glue_cores(void);									// logical processors of the machine, at least 1
unsigned long long kvm_glue_nonenc_cpu_us(void);			// CPU time of the main loop thread and of the capture thread together (0 when unknown): the encoder's CPU is what the process used less this
// A video frame was written to the pipe (by kvm_video_service, a barrier or kvm_video_emit): res = the ILibTransport_DoneState of the write (1 complete, 0 incomplete = pause, -4 error), with the bytes and the time in the write
void kvm_glue_video_written(int res, long long bytes, unsigned writeUs);

// ---- implemented in kvm_video.cpp ----
// Start of the KVM session (main loop thread, before kvm_stream_session_begin): checks the switches and the system (Windows 8, Media Foundation). 1 = the video is possible; else 0 and
// *reason is the reason of the 242 for op 1 (2 no Media Foundation or Windows older than 8, 6 switched off).
int  kvm_video_session_begin(int *reason);
void kvm_video_session_end(void);
// The size of the picture is known / changed (kvm_server_SetResolution, after the new tileInfo exists): the model of the tiles is built again; a running video begins a new generation.
void kvm_video_geometry(int tileRows, int tileCols, int screenW, int screenH);
// A 240 that kvm_stream_on_ctrl has handled (lock held, called from kvm_stream_poll): the video code remembers what it has to do (it does the work in kvm_video_poll)
void kvm_video_ctrl(const kvm_ctrl *c, unsigned actions, unsigned atMs);
// Every iteration of the loop, after kvm_stream_poll, no lock held: stream state against video state, requests, start of a generation, retry after an error, repair
void kvm_video_poll(void);
// The viewer asked for a REFRESH (input thread): every tile goes back to JPEG, the model is cleared in the next poll
void kvm_video_note_refresh(void);
int  kvm_video_on(void);										// 1 while a generation is running
int  kvm_video_scale_allowed(int down);						// the scale ladder wants a change (down = towards a smaller picture): with the video running only the return upwards, 10 s after the last generation began
int  kvm_video_active(void);									// 1 while a generation runs or is about to start (also while it waits for the encoder)
int  kvm_video_pending(void);									// tiles that wait for the next frame (pendingVideo)
int  kvm_video_want_image(void);								// 1: a picture is wanted although nothing changed (pending tiles, and a frame may go now)
int  kvm_video_wake_ms(void);									// >= 1: the loop may sleep this long, the next frame is due then (its slot); -1: the video is not waiting for its slot
int  kvm_video_loop_period(int viewerPeriodMs);					// the pause of an iteration of the loop: the step of Auto while Auto is on, else viewerPeriodMs
void kvm_video_state_caps(kvm_state_msg *m);					// adds bit 2 of the caps of the 242 (this agent knows Auto) to a 242 that is about to be written
void kvm_video_note_capture(unsigned ms, unsigned dwmFrames);	// the image that kvm_video_emit looks at next: what its capture took (ms) and the frames of the desktop manager it stands for (0 = not known); Auto's
int  kvm_video_interval_ms(void);								// the frame interval now (the period of the viewer, or longer when the CPU budget of the encoder says so); the sharpening governor and the log use it
unsigned kvm_video_r_kbps(void);								// R, the target rate of the video, kbit/s (0 = no video running): the log of the series
// tile.cpp, get_tile_at, after the rectangle was coalesced: 1 = the rectangle [row0..row1] x [col0..col1] of the class cls (kvm_adapt_class) goes to pendingVideo and is not sent as JPEG
int  kvm_video_claim_rect(int row0, int col0, int row1, int col1, int cls);
void kvm_video_tile_jpeg(int row, int col);					// a tile was sent as JPEG: it leaves pendingVideo and VIDEO (I3)
int  kvm_video_tile_pending(int row, int col);					// 1: the tile is in pendingVideo (sharpening skips it)
// After the tile loop of a frame: the picture (`desktop`, a kvm_fbuf buffer: the bottom-up picture of the frame, PIXEL_SIZE 4) is held for a frame of the pending tiles; the frame is started (handed to the thread of the
// encoder) if the encoder is free and the window, the pace and the bucket allow, else it is started later by kvm_video_service(). The frame is written when the encoder is done (kvm_glue_video_written).
void kvm_video_emit(const void *desktop, long long desktopSize);
// Whenever the loop may (the top of an iteration, while it waits for the capture or sleeps): results of the encoder are written, the watchdog runs, a waiting picture is started. Never waits.
void kvm_video_service(void);
// 1 while a frame is in the encoder
int  kvm_video_busy(void);
// The event (a Windows HANDLE) that is set when the encoder finishes a frame, for WaitForMultipleObjects of the loop; NULL when the encoder runs inline
void *kvm_video_event(void);
// tile.cpp, get_tile_at: a JPEG of the rectangle [row0..row1] x [col0..col1] is about to be made; a frame in flight that covers a tile of it is written first
void kvm_video_before_jpeg(int row0, int col0, int row1, int col1);
const struct kvm_auto *kvm_video_test_auto(void);					// tests only (KVM_VIDEO_TEST): the state of Auto
void kvm_video_test_counts(unsigned *nInflight, unsigned *nVideo, unsigned *nPending, unsigned *jpegOnInflight);	// tests only
unsigned kvm_video_test_rc_flat_milli(void);			// tests only: the floor of a flat key frame that the rate control forecasts with (thousandths of a byte per macroblock)
void kvm_video_test_qp_ineffective(void);					// tests only: the verdict of the detector of the QP that the encoder does not listen to (what the stage of the encoder says next)
unsigned kvm_video_test_rc_win_bytes(void);				// tests only: the bytes of the P frames that the regulator counts in this window (virgin macroblocks are not in them)
unsigned kvm_video_test_qpdet_pframes(void);			// tests only: the P frames that the QP detector has seen (frames with virgin macroblocks are not among them)
void kvm_video_leaked_free(void);									// tests only: frees the buffers that a hung encoder was given up with
void kvm_video_note_sharpen(unsigned bytes, int videoActive);	// bytes of sharpening that went out (kvm_adapt.cpp): the rate control pays for them while the video moves
void kvm_video_stats_flush(int final);							// the "video stats" line (with the statistics line of the stream layer)
// the hardware encoder, the PARENT (the service): the KVM child with this pid ends or is about to be killed (exit code 1067 = TerminateProcess at the end of a session): the marker "a try with the hardware encoder is under way" is cleared,
// or counted as an exception when the code is a status of a crash (kvm_hwtest.cpp). Cheap when there is no marker; implemented in kvm_hwtest.cpp.
int kvm_hwtest_child_gone(uint32_t pid, uint32_t exitCode);

#ifdef __cplusplus
}
#endif

#else // !KVM_VIDEO_ENABLED: stubs, the behaviour of the end-to-end ACK

static __inline int  kvm_video_on(void) { return 0; }
static __inline int  kvm_video_scale_allowed(int down) { (void)down; return 1; }
static __inline int  kvm_video_active(void) { return 0; }
static __inline int  kvm_video_pending(void) { return 0; }
static __inline int  kvm_video_interval_ms(void) { return 0; }
static __inline int  kvm_video_loop_period(int viewerPeriodMs) { return viewerPeriodMs; }
static __inline void kvm_video_note_capture(unsigned ms, unsigned dwmFrames) { (void)ms; (void)dwmFrames; }
static __inline unsigned kvm_video_r_kbps(void) { return 0; }
static __inline int  kvm_video_want_image(void) { return 0; }
static __inline void kvm_video_tile_jpeg(int row, int col) { (void)row; (void)col; }
static __inline void kvm_video_before_jpeg(int row0, int col0, int row1, int col1) { (void)row0; (void)col0; (void)row1; (void)col1; }
static __inline int  kvm_video_tile_pending(int row, int col) { (void)row; (void)col; return 0; }
static __inline void kvm_video_note_refresh(void) {}
static __inline void kvm_video_note_sharpen(unsigned bytes, int videoActive) { (void)bytes; (void)videoActive; }
static __inline int  kvm_video_claim_rect(int row0, int col0, int row1, int col1, int cls) { (void)row0; (void)col0; (void)row1; (void)col1; (void)cls; return 0; }
static __inline int kvm_hwtest_child_gone(uint32_t pid, uint32_t exitCode) { (void)pid; (void)exitCode; return 0; }

#endif // KVM_VIDEO_ENABLED
#endif // KVM_VIDEO_H
