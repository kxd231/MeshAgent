/*
KVM (Windows): quality that depends on motion, sharpening of still tiles and the congestion regulator (WA-2).

Tiles that change again and again while a lot of the screen moves (scrolling, video) are sent with a lower JPEG quality and 4:2:0
chroma subsampling ("motion class"); when a tile has been still for KVM_ADAPT_SETTLE_MS it is sent again with the quality the user
asked for and the normal subsampling ("sharpening"), as an ordinary picture command. The regulator watches how long the writes to the
pipe block (a congested link stops the stream) and lowers the motion quality and then the frame rate.

WA-2b adds a bandwidth budget: the bytes written in the 500 ms windows are compared with KVM_ADAPT_BUDGET_KBPS; over the budget a ladder
(B0..B5) lowers the motion quality and the frame rate of the motion class in turn, with hysteresis. The frame limit of the motion class
(KVM_ADAPT_MOTION_FPS, off by default; the ladder sets it from B2 on) holds a change back and sends it in the next allowed frame.
Optimized Huffman tables for the JPEGs (KVM_ADAPT_JPEG_OPT) are used where the measurement says they pay.

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
#define KVM_ADAPT_MIN_FRAME_TIMER 10	// shortest frame timer (ms) the agent accepts: 10 here (the viewer offers 16 = 60 frames per second), 20 in the builds without WA-2

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
// ---- WA-2b: bandwidth budget, motion frame limit, JPEG tables ----
#ifndef KVM_ADAPT_BUDGET_KBPS
#define KVM_ADAPT_BUDGET_KBPS 0			// kilobits per second written to the pipe (all of it, sharpening included); 0 = no budget (the default: the viewer chooses). A value from the viewer (command 5, bytes 10-11, units of 100 kbit/s) overrides MESH_KVM_BUDGET_KBPS, which overrides this.
#endif
#ifndef KVM_ADAPT_BUDGET_UP_PCT
#define KVM_ADAPT_BUDGET_UP_PCT 110		// a window above this share of the budget counts as "over" (the budget is a target: a steady stream a few percent above it is left alone)
#endif
#ifndef KVM_ADAPT_BUDGET_DOWN_PCT
#define KVM_ADAPT_BUDGET_DOWN_PCT 100	// a window below this share of the budget counts as "under": after KVM_ADAPT_BUDGET_DOWN_WINDOWS of them the ladder tries one step back (a failed try costs the doubling of the patience)
#endif
#ifndef KVM_ADAPT_BUDGET_UP_WINDOWS
#define KVM_ADAPT_BUDGET_UP_WINDOWS 2	// windows over the budget in a row before the ladder goes one step down (the first image of a session is a single burst)
#endif
#ifndef KVM_ADAPT_BUDGET_DOWN_WINDOWS
#define KVM_ADAPT_BUDGET_DOWN_WINDOWS 6	// windows clearly under the budget in a row before the ladder goes one step up (doubled each time the step is lost again soon)
#endif
#ifndef KVM_ADAPT_BUDGET_WARM_MS
#define KVM_ADAPT_BUDGET_WARM_MS 3000	// the first part of a session (first image and its sharpening, settings of the viewer arriving) does not count for the ladder
#endif
// The ladder: motion quality (never above the quality of the user) and motion frame limit (0 = loop pace) per step B0..B5.
#ifndef KVM_ADAPT_BQ1
#define KVM_ADAPT_BQ1 40
#endif
#ifndef KVM_ADAPT_BQ2
#define KVM_ADAPT_BQ2 40
#endif
#ifndef KVM_ADAPT_BQ3
#define KVM_ADAPT_BQ3 30
#endif
#ifndef KVM_ADAPT_BQ4
#define KVM_ADAPT_BQ4 25
#endif
#ifndef KVM_ADAPT_BQ5
#define KVM_ADAPT_BQ5 25
#endif
#ifndef KVM_ADAPT_BF2
#define KVM_ADAPT_BF2 30
#endif
#ifndef KVM_ADAPT_BF3
#define KVM_ADAPT_BF3 30
#endif
#ifndef KVM_ADAPT_BF4
#define KVM_ADAPT_BF4 20
#endif
#ifndef KVM_ADAPT_BF5
#define KVM_ADAPT_BF5 15
#endif
// Below B5 (see the table in kvm_adapt.cpp): quality to KVM_ADAPT_Q_FLOOR (hard floor of every class), then the scale of the picture (a share of the scale the viewer chose,
// KVM_ADAPT_SCALE_S1 in 1/1024; 1024 leaves the step out), and only then fewer motion frames per second (the byte bucket of the budget paces them). The scale changes only
// during motion, with a minimum time between changes, and goes back to the scale of the viewer when the screen has been still for KVM_ADAPT_SCALE_RESTORE_MS.
// 75 % (768) has the SSE2 scaler of the agent (3-5 ms for 1920x1080 on a VM); 87.5 % and 70 % use the generic filter (17-28 ms), which would limit the frames: not used.
#ifndef KVM_ADAPT_Q_FLOOR
#define KVM_ADAPT_Q_FLOOR 20
#endif
#ifndef KVM_ADAPT_SCALE_S1
#define KVM_ADAPT_SCALE_S1 768			// 75 % (the lowest allowed is 70 %)
#endif
#ifndef KVM_ADAPT_SCALE_DOWN_MS
#define KVM_ADAPT_SCALE_DOWN_MS 3000	// at least this long between two changes of the scale, going down
#endif
#ifndef KVM_ADAPT_SCALE_UP_MS
#define KVM_ADAPT_SCALE_UP_MS 10000		// ... going up while the screen still moves
#endif
#ifndef KVM_ADAPT_SCALE_RESTORE_MS
#define KVM_ADAPT_SCALE_RESTORE_MS 3500	// no movement (frames with at least KVM_ADAPT_MOTION_TILES changed tiles) for this long: back to the scale of the viewer
#endif
#ifndef KVM_ADAPT_SCALE_BAN_MS
#define KVM_ADAPT_SCALE_BAN_MS 20000	// after such a return the scale does not go down again for this long (the bucket paces the frames meanwhile)
#endif
#ifndef KVM_ADAPT_SCALE_INPUT_MS
#define KVM_ADAPT_SCALE_INPUT_MS 1500	// the scale does not change within this time of the last mouse or keyboard input of the viewer (clicks around a change land beside)
#endif
// The budget is a hard limit: a byte bucket (refilled at the budget rate, KVM_ADAPT_BUCKET_BURST_MS of burst, debt at most KVM_ADAPT_BUCKET_DEBT_MS) gates every rectangle that is
// not small; what does not pass is held back (the "deferred" tiles) and sent later in the quality of the ladder step. KVM_ADAPT_BUCKET_SHARPEN_PCT of the rate is reserved for sharpening,
// so that it always finishes. At and below step KVM_ADAPT_ALLMOTION_STEP every rectangle above KVM_ADAPT_BUCKET_SMALL_TILES tiles is sent in the quality of the step (and sharpened later).
#ifndef KVM_ADAPT_BUCKET_BURST_MS
#define KVM_ADAPT_BUCKET_BURST_MS 500		// small, so that a 5 s window cannot exceed the budget by more than about 10 % (a REFRESH of a still screen found a full bucket)
#endif
#ifndef KVM_ADAPT_BUCKET_DEBT_MS
#define KVM_ADAPT_BUCKET_DEBT_MS 3000
#endif
#ifndef KVM_ADAPT_BUCKET_SHARPEN_PCT
#define KVM_ADAPT_BUCKET_SHARPEN_PCT 20
#endif
#ifndef KVM_ADAPT_BUCKET_SMALL_TILES
#define KVM_ADAPT_BUCKET_SMALL_TILES 8		// rectangles up to this many 32x32 tiles (typing, cursor) pass while the debt is below one second of the budget
#endif
#ifndef KVM_ADAPT_BUCKET_AGE_MS
#define KVM_ADAPT_BUCKET_AGE_MS 1000		// the oldest held-back change has waited this long: from then on held-back changes are served in the order of their age, not of their place on the screen
#endif
#ifndef KVM_ADAPT_BUCKET_AGE_TOL_MS
#define KVM_ADAPT_BUCKET_AGE_TOL_MS 100	// a rectangle goes out when it holds a tile that was held back no later than the oldest one + this
#endif
#ifndef KVM_ADAPT_BUCKET_SMALL_PCT
#define KVM_ADAPT_BUCKET_SMALL_PCT 20		// share of the rate reserved for small rectangles (typing, the cursor, small animations): they do not wait in the queue of the large ones
#endif
#ifndef KVM_ADAPT_BUCKET_SMALL_GATE_MS
#define KVM_ADAPT_BUCKET_SMALL_GATE_MS 1000	// a small rectangle passes on its reserved share only while the debt of the main bucket is shallower than this (a flood of small rectangles must not push the main bucket to its limit: then bytes would be forgiven and the held-back large changes would never go out); a deeper debt: it is held back and takes its place in the queue by age. A full-size rectangle alone can take the main bucket to about -0.5 s, so a lower value would also hold back typing during motion
#endif
#ifndef KVM_ADAPT_BUCKET_MAX_TILES
#define KVM_ADAPT_BUCKET_MAX_TILES 64		// under a budget a rectangle has at most this many tiles, and no more than about half a second of the budget (by the size of the last ones)
#endif
#ifndef KVM_ADAPT_ALLMOTION_STEP
#define KVM_ADAPT_ALLMOTION_STEP 3
#endif
#ifndef KVM_ADAPT_BUDGET_MAX_STEP
#define KVM_ADAPT_BUDGET_MAX_STEP 7
#endif
#ifndef KVM_ADAPT_MOTION_FPS
#define KVM_ADAPT_MOTION_FPS 0			// fixed limit of the motion class in frames per second, 0 = none (the ladder may still set one). MESH_KVM_MOTION_FPS overrides.
#endif
#ifndef KVM_ADAPT_JPEG_OPT
#define KVM_ADAPT_JPEG_OPT 2			// optimized Huffman tables: 0 = never, 1 = sharpening only, 2 = every rectangle, 3 = sharpening and rectangles up to KVM_ADAPT_JPEG_OPT_TILES tiles. MESH_KVM_JPEG_OPT overrides.
#endif
#ifndef KVM_ADAPT_JPEG_OPT_TILES
#define KVM_ADAPT_JPEG_OPT_TILES 16		// mode 3: largest rectangle (in 32x32 tiles) that gets optimized tables
#endif
#ifndef KVM_ADAPT_MAX_PAUSE_MS
#define KVM_ADAPT_MAX_PAUSE_MS 250		// longest pause between frames the regulator asks for
#endif

#ifdef __cplusplus
extern "C" {
#endif
struct tileInfo_t;

void kvm_adapt_set_budget(int kbps);									// the viewer chose a budget (command 5): kbit/s, 0 = none; called from the input thread, takes effect at the next window
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
void kvm_adapt_note_write(unsigned int us, long long bytes, int sharpen);	// time blocked in a write to the pipe and its size (sharpen: the packet is a sharpening)
int  kvm_adapt_user_scale(int ns);									// command 5 brought the scale of the viewer: returns the scale to ask for (viewer scale x budget multiplier)
int  kvm_adapt_bucket_closed(int tiles, unsigned int oldestDeferredAt);
int  kvm_adapt_max_rect_tiles(void);									// under a budget: the most tiles a rectangle may have (0 = no limit)
void kvm_adapt_note_input(void);										// the viewer sent a mouse or key command
void kvm_adapt_note_refresh(void);									// the viewer asked for a REFRESH (the full picture is not movement)								// 1: the budget holds back a rectangle of this many tiles now (it is deferred)
int  kvm_adapt_force_motion(int tiles);								// 1: this rectangle of the full class is sent in the quality of the ladder step (budget, low step)
int  kvm_adapt_gate_closed(int cls);									// 1: the motion frame limit holds this rectangle (class cls) back in this frame
void kvm_adapt_defer(int row0, int col0, int row1, int col1);			// the rectangle was held back: the tiles must be looked at again and sent in the next allowed frame
int  kvm_adapt_frame_deferred(void);									// 1: this frame held something back (keeps the loop pace and the capture overlap)
int  kvm_adapt_jpeg_opt(int sharpen, int tiles);								// 1: encode with optimized Huffman tables (sharpen: the call is the sharpening of still tiles)
void kvm_adapt_note_encode(int sharpen, unsigned int us, int tiles);	// time of one JPEG encode and the number of tiles in it
void kvm_adapt_note_loop(unsigned int waitUs, unsigned int procUs, unsigned int pauseUs); // one loop iteration: waiting for the capture, processing the frame, the pause
int  kvm_adapt_frame_pause(int base);								// frame pause in ms (the regulator stretches it and limits the rate)
void kvm_adapt_stats_flush(int final);								// writes "adapt stats" and resets the counters (called with the "stats" line)
#ifdef __cplusplus
}
#endif

#else // !KVM_ADAPT_ENABLED: stubs, the behaviour of WA-1

#define KVM_ADAPT_MIN_FRAME_TIMER 20

struct tileInfo_t;
static __inline void kvm_adapt_set_budget(int kbps) { (void)kbps; }
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
static __inline void kvm_adapt_note_write(unsigned int us, long long bytes, int sharpen) { (void)us; (void)bytes; (void)sharpen; }
static __inline int  kvm_adapt_user_scale(int ns) { return ns; }
static __inline int  kvm_adapt_bucket_closed(int tiles, unsigned int oldestDeferredAt) { (void)tiles; (void)oldestDeferredAt; return 0; }
static __inline int  kvm_adapt_max_rect_tiles(void) { return 0; }
static __inline void kvm_adapt_note_input(void) {}
static __inline void kvm_adapt_note_refresh(void) {}
static __inline int  kvm_adapt_force_motion(int tiles) { (void)tiles; return 0; }
static __inline int  kvm_adapt_gate_closed(int cls) { (void)cls; return 0; }
static __inline void kvm_adapt_defer(int row0, int col0, int row1, int col1) { (void)row0; (void)col0; (void)row1; (void)col1; }
static __inline int  kvm_adapt_frame_deferred(void) { return 0; }
static __inline int  kvm_adapt_jpeg_opt(int sharpen, int tiles) { (void)sharpen; (void)tiles; return 0; }
static __inline void kvm_adapt_note_encode(int sharpen, unsigned int us, int tiles) { (void)sharpen; (void)us; (void)tiles; }
static __inline void kvm_adapt_note_loop(unsigned int waitUs, unsigned int procUs, unsigned int pauseUs) { (void)waitUs; (void)procUs; (void)pauseUs; }
static __inline int  kvm_adapt_frame_pause(int base) { return base; }
static __inline void kvm_adapt_stats_flush(int final) { (void)final; }

#endif // KVM_ADAPT_ENABLED
#endif // KVM_ADAPT_H
