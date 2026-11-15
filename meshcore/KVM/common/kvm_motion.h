/*
KVM: which tiles are motion while the H.264 video runs, and the diagnostics of the series of motion. Portable C99, no operating system API, time is an injected
millisecond counter (uint32_t, wraps; every comparison of two times is a signed difference). The decisions of kvm_adapt.cpp that could not be tested in Docker live here; the glue only
reads the clock and the tile state and calls these functions.

Why. Before, a tile went to the video (class 1) only when it had changed twice in a row within 250 ms AND the previous captured frame had changed at least 16 tiles.
The first change of every tile of an animation, the edge that a dragged window uncovers and everything after a frame with a few changes (a text cursor, a clock, the pointer) went out as JPEG
in the quality of the user, ten times the bytes of the video, and without the byte bucket of the video: on the AMD laptop 17.4 JPEG paints a second against 6.0 of the video, the inside of the
motion stood while the edges moved (tearing).

  the class of a tile (kvm_motion_class), video running only:
        - the streak rule as before (two changes in a row, each within 250 ms of the previous one), but the condition on the neighbourhood of the frame is "a frame with at least 16 changes
          that really happened within the last 250 ms" (kvm_motion_frame), not "the frame before this one": a frame with a few changes in the middle of a motion does not end it;
        - the near rule: the first change of a tile within KVM_MOTION_NEAR tiles (2 = 64 px) of a tile that was classified as motion by the streak rule in the last 250 ms goes to the video too
          (the edge of a dragged window, the next frame of an animation). A tile that was only taken by the near rule does not extend the area (typing next to a video does not spread);
        - everything else is class 0 (JPEG): typing, the caret, a window that appears (its first frame has no motion around it).
  a change that comes with a change of the SOURCE of the picture (GDI fill -> the first copy of DXGI at the start, back from a probe of DXGI, DXGI -> GDI) is not a change of the content
      (kvm_motion_note_change): the streak starts again and the tile is not counted as moved: no key frame of the whole screen and no sharpening of it at the start of a session.
  kvm_series: the series of motion (a frame with at least 16 changes after at least 500 ms without one), how long the video took to come (start_ms), R at the start, the gaps between the pictures
      that were written in a series (P95, maximum, those above twice the frame interval) and the share of the JPEG paints in the motion, one short line per period.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef KVM_MOTION_H
#define KVM_MOTION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef KVM_MOTION_MS
#define KVM_MOTION_MS 250			// (T) a tile that changes again within this time is "moving"; a frame with many changes counts as motion for this long (kvm_adapt.h KVM_ADAPT_MOTION_MS)
#endif
#ifndef KVM_MOTION_STREAK
#define KVM_MOTION_STREAK 2			// (T) changes in a row before a tile is "moving" (KVM_ADAPT_MOTION_STREAK)
#endif
#ifndef KVM_MOTION_TILES
#define KVM_MOTION_TILES 16			// (T) a frame with at least this many tiles that really changed is a frame of motion (KVM_ADAPT_MOTION_TILES)
#endif
#ifndef KVM_MOTION_NEAR
#define KVM_MOTION_NEAR 2			// (T) the near rule: this many tiles (32 px each) around a tile that is moving (KVM_ADAPT_VIDEO_NEAR)
#endif
#ifndef KVM_MOTION_RECALL_MS
#define KVM_MOTION_RECALL_MS 3000	// (T) the memory of the place: the first change of a tile whose own last motion (streak rule) is younger than this goes to the video (the animation that comes back to where the last one was; covers the pauses of the work with an interface); 0 = off
#endif
#ifndef KVM_SERIES_QUIET_MS
#define KVM_SERIES_QUIET_MS 500		// (T) a series of motion ends after this long without a frame of motion
#endif
#ifndef KVM_SERIES_GAP_MAX_MS
#define KVM_SERIES_GAP_MAX_MS 1000	// (T) a gap between two pictures of one series longer than this is not a gap of the series (the motion stood still)
#endif
#ifndef KVM_SERIES_MERGE_MS
#define KVM_SERIES_MERGE_MS 4		// (T) writes of one iteration (JPEG rectangles, the video frame) closer than this are one picture (the viewer paints them together)
#endif
#define KVM_GAP_BUCKETS 16

typedef struct kvm_motion
{
	int rows, cols;
	uint32_t *at;				// rows * cols: when the tile was last classified as motion by the streak rule (0 = never); NULL when there is no memory (the near rule is off)
	uint32_t frameAt;			// time of the last frame with at least `tilesMin` changes that really happened (0 = never)
	uint32_t recentMs, recallMs;
	unsigned streakMin, tilesMin;
	int radius;
	// statistics (the log reads and clears them)
	uint32_t sStreak, sNear, sRecall, sPlain;	// tiles classified as motion by the streak rule / by the near rule / by the memory of the place / not as motion
} kvm_motion;

void kvm_motion_init(kvm_motion *m);	// defaults of the macros above, no map
void kvm_motion_set(kvm_motion *m, uint32_t recentMs, unsigned streakMin, unsigned tilesMin, int radius, uint32_t recallMs);	// the glue passes its own constants
void kvm_motion_free(kvm_motion *m);
// The grid of tiles: the map is cleared (and made if the size changed). 0, or -1 when there is no memory (the map stays empty, the near rule is off).
int kvm_motion_resize(kvm_motion *m, int rows, int cols);
void kvm_motion_reset(kvm_motion *m);	// forgets everything (a new session, a REFRESH)
// The end of a frame's scan, or the start of the next frame: `frameAt` = the time of that frame, `realChanged` = tiles of it that changed in content (not a tile seen for the first time).
void kvm_motion_frame(kvm_motion *m, uint32_t frameAt, unsigned realChanged);
// 1: a frame with at least tilesMin real changes happened within recentMs before `now`
int kvm_motion_recent(const kvm_motion *m, uint32_t now);
// The new streak of a tile whose content changed (the CRC differs), `sinceMs` = the time since its last change (signed: a stamp later than `now` is "just now"). A change that comes with a change of
// the source of the picture is none: 0. Else one more than before when the previous change is at most recentMs old, otherwise 1.
unsigned kvm_motion_note_change(const kvm_motion *m, unsigned streak, int32_t sinceMs, int sourceChange);
// The class of a tile that is to be sent: 1 = motion (video), 0 = full. `streak` and `sinceMs` as kept by the tile (the time since its last change). Stamps the tile when the streak rule gave 1.
// Third rule, after the first two: the tile's own stamp is younger than recallMs (it was motion lately, the same place moves again): motion, without a stamp (the area does not spread).
// Only meant for the time the video runs: without it the glue keeps the rule of WA-2.
int kvm_motion_class(kvm_motion *m, uint32_t now, int row, int col, unsigned streak, int32_t sinceMs);
void kvm_motion_clear_stats(kvm_motion *m);
// safety net: a tile whose hash differs in the first complete frame from another source of pixels (what the alpha byte does not explain) is not sent at once: its new hash becomes what the viewer
// is taken to have and the tile is marked lossy, so that the sharpening repaints it behind the motion. 1 = do that for this tile. Not for a tile that the viewer has never been given (crcIsReset:
// the hash is 0xFF after a REFRESH or at the start): that one has to be sent.
int kvm_motion_source_absorb(int sourceChange, int crcIsReset);
// The 4th byte of every pixel of `bytes` bytes (32 bit pixels) set to 0: the tile hash counts it, DXGI delivers it as 0 and GDI as it likes (the cause: 2022 of 2040 tiles "changed" at the first copy of DXGI)
void kvm_clear_alpha(uint8_t *p, size_t bytes);

// ---- the series of motion ----
typedef struct kvm_series
{
	uint8_t inSeries, haveVideo, haveWrite;
	uint32_t lastMotion, seriesStart, lastWrite;
	uint32_t rStartKbps;
	// the period
	uint32_t nSeries, nStarts, noVideo, startSum, startMax, rStartMin;
	uint32_t gap[KVM_GAP_BUCKETS];
	uint32_t nGaps, gapMax, over2x;
	uint32_t paintsJpeg, paintsVideo, tilesJpeg;
} kvm_series;

void kvm_series_init(kvm_series *s);
// A frame with at least KVM_MOTION_TILES real changes was seen at `now`; rKbps = R of the video at this moment (0 = no video). Opens a series when the last one ended.
void kvm_series_motion(kvm_series *s, uint32_t now, uint32_t rKbps);
// A picture was written at `now`: a video frame (`video` = 1) or the JPEG rectangles of an iteration (tiles = their number; not used for a frame). Counted only inside a series; `intervalMs` = the frame interval
// (a gap longer than twice that is counted separately).
void kvm_series_write(kvm_series *s, uint32_t now, int video, unsigned tiles, uint32_t intervalMs);
// The line for the log: writes up to cap bytes ("video burst ..."), clears the period. Returns the length, 0 when there was nothing to say (no series, no picture in one).
size_t kvm_series_format(kvm_series *s, uint32_t now, uint32_t intervalMs, char *buf, size_t cap);
// the upper edge (ms) of the bucket that holds the P95 of the gaps (0 = no gap), for the tests
uint32_t kvm_series_gap_p95(const kvm_series *s);

#ifdef __cplusplus
}
#endif

#endif // KVM_MOTION_H
