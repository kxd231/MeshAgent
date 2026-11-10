/*
KVM (Windows): quality that depends on motion, sharpening of still tiles and the congestion regulator (WA-2).

Tiles that change again and again while a lot of the screen moves (scrolling, video) are sent with a lower JPEG quality and 4:2:0
chroma subsampling ("motion class"); when a tile has been still for KVM_ADAPT_SETTLE_MS it is sent again with the quality the user
asked for and the normal subsampling ("sharpening"), as an ordinary picture command. The regulator watches how long the writes to the
pipe block (a congested link stops the stream) and lowers the motion quality and then the frame rate.

Only for the x64 MeshService build with TurboJPEG (JPEG types 1 and 4, 24/32 bit pixels); everywhere else the stubs below keep the
behaviour of WA-1: every tile is "full class", nothing is sharpened, the pause is the frame-rate timer.
Switches: /DKVM_ADAPT_DISABLE (compile time), MESH_KVM_ADAPT=0 in the environment of the service (run time).

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef KVM_ADAPT_H
#define KVM_ADAPT_H

#include "dxgi_capture.h" // KVM_STATS_LOG (defined together with KVM_DXGI on x64)

#if defined(_TURBOJPEG) && defined(KVM_STATS_LOG) && !defined(KVM_ADAPT_DISABLE)
#define KVM_ADAPT_ENABLED 1
#endif

#ifdef KVM_ADAPT_ENABLED

// Thresholds, all can be overridden with /D on the compiler command line.
#ifndef KVM_ADAPT_Q_MOTION
#define KVM_ADAPT_Q_MOTION 50			// JPEG quality of the motion class at level 0 (never above the quality of the user)
#endif
#ifndef KVM_ADAPT_Q_MOTION_MIN
#define KVM_ADAPT_Q_MOTION_MIN 20		// lowest motion quality of the regulator
#endif
#ifndef KVM_ADAPT_Q_STEP
#define KVM_ADAPT_Q_STEP 10				// quality step per regulator level
#endif
#ifndef KVM_ADAPT_MOTION_MS
#define KVM_ADAPT_MOTION_MS 250			// a tile that changes again within this time is "moving"
#endif
#ifndef KVM_ADAPT_MOTION_STREAK
#define KVM_ADAPT_MOTION_STREAK 2		// changes in a row (each within KVM_ADAPT_MOTION_MS of the previous one) before a tile is "moving"
#endif
#ifndef KVM_ADAPT_MOTION_TILES
#define KVM_ADAPT_MOTION_TILES 16		// ...and only while at least this many tiles changed in the previous frame (typing stays sharp)
#endif
#ifndef KVM_ADAPT_SETTLE_MS
#define KVM_ADAPT_SETTLE_MS 150			// a lossy tile that has not changed for this long is sent again in full quality
#endif
#ifndef KVM_ADAPT_SHARPEN_BYTES
#define KVM_ADAPT_SHARPEN_BYTES 98304	// bytes of sharpening per frame at regulator level 0
#endif
#ifndef KVM_ADAPT_SHARPEN_RUN
#define KVM_ADAPT_SHARPEN_RUN 8			// neighbouring tiles of a row sharpened as one rectangle
#endif
#ifndef KVM_ADAPT_WINDOW_MS
#define KVM_ADAPT_WINDOW_MS 500			// window of the congestion signal
#endif
#ifndef KVM_ADAPT_CONGEST_PCT
#define KVM_ADAPT_CONGEST_PCT 15		// share of the window spent blocked in the pipe write that raises the level
#endif
#ifndef KVM_ADAPT_CALM_PCT
#define KVM_ADAPT_CALM_PCT 3			// ... and the share below which a window counts as calm
#endif
#ifndef KVM_ADAPT_CALM_WINDOWS
#define KVM_ADAPT_CALM_WINDOWS 3		// calm windows in a row that lower the level again
#endif
#ifndef KVM_ADAPT_MAX_LEVEL
#define KVM_ADAPT_MAX_LEVEL 5
#endif
#ifndef KVM_ADAPT_RATE_FRACTION_PCT
#define KVM_ADAPT_RATE_FRACTION_PCT 70	// from level 4 on the output is limited to this share of the measured accepted rate
#endif
#ifndef KVM_ADAPT_MAX_PAUSE_MS
#define KVM_ADAPT_MAX_PAUSE_MS 250		// longest pause between frames the regulator asks for
#endif

#ifdef __cplusplus
extern "C" {
#endif
struct tileInfo_t;

void kvm_adapt_session_start(void);									// start of the KVM loop: reads MESH_KVM_ADAPT, resets the regulator, logs "adapt start"
void kvm_adapt_session_end(void);									// end of the KVM loop: last statistics
void kvm_adapt_iteration(void);										// every iteration of the loop: regulator window, review of the tiles
int  kvm_adapt_want_image(void);									// 1: a picture is wanted although nothing changed (lossy tiles are due for sharpening)
void kvm_adapt_frame_begin(void);									// before the tile loop of a frame with a picture
void kvm_adapt_note_crc(struct tileInfo_t *t, int crc);				// after the CRC of a tile was computed (t->crc still has the previous value)
int  kvm_adapt_class(const struct tileInfo_t *t);					// 0 = full class, 1 = motion class
void kvm_adapt_params(int cls, int motionSub, int fullSub, int *q, int *sub); // quality and subsampling for a tile class
void kvm_adapt_sent(int row0, int col0, int row1, int col1, int cls, int q, int lossySub, long long bytes); // a rectangle of tiles was encoded
int  kvm_adapt_sharpen_next(void *desktop, long long budgetLeft, void **packet, long long *size); // 1: *packet must be written (and freed)
long long kvm_adapt_sharpen_budget(void);							// bytes that may be sharpened after this frame
void kvm_adapt_note_write(unsigned int us, long long bytes);		// time blocked in a write to the pipe and its size
int  kvm_adapt_frame_pause(int base);								// frame pause in ms (the regulator stretches it and limits the rate)
void kvm_adapt_stats_flush(int final);								// writes "adapt stats" and resets the counters (called with the "stats" line)
#ifdef __cplusplus
}
#endif

#else // !KVM_ADAPT_ENABLED: stubs, the behaviour of WA-1

struct tileInfo_t;
static __inline void kvm_adapt_session_start(void) {}
static __inline void kvm_adapt_session_end(void) {}
static __inline void kvm_adapt_iteration(void) {}
static __inline int  kvm_adapt_want_image(void) { return 0; }
static __inline void kvm_adapt_frame_begin(void) {}
static __inline void kvm_adapt_note_crc(struct tileInfo_t *t, int crc) { (void)t; (void)crc; }
static __inline int  kvm_adapt_class(const struct tileInfo_t *t) { (void)t; return 0; }
static __inline void kvm_adapt_sent(int row0, int col0, int row1, int col1, int cls, int q, int lossySub, long long bytes) { (void)row0; (void)col0; (void)row1; (void)col1; (void)cls; (void)q; (void)lossySub; (void)bytes; }
static __inline int  kvm_adapt_sharpen_next(void *desktop, long long budgetLeft, void **packet, long long *size) { (void)desktop; (void)budgetLeft; (void)packet; (void)size; return 0; }
static __inline long long kvm_adapt_sharpen_budget(void) { return 0; }
static __inline void kvm_adapt_note_write(unsigned int us, long long bytes) { (void)us; (void)bytes; }
static __inline int  kvm_adapt_frame_pause(int base) { return base; }
static __inline void kvm_adapt_stats_flush(int final) { (void)final; }

#endif // KVM_ADAPT_ENABLED
#endif // KVM_ADAPT_H
