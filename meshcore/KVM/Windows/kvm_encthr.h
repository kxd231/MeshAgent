/*
KVM (Windows): the thread of the encoder.

The video frame is encoded by a thread of its own while the main loop goes on with the next picture (CRC of the tiles, classes, JPEG). The thread is a function of its input: the picture of the
job (read only), the parameters of the frame (key frame, quantizer, bit rate) -> the access unit (or an error), made by the ENCODER BEHIND IT. Nothing of the model of the video (the tiles, the
rate control, the stream, the pipe) is touched by the thread: the main loop does all of that when it collects the result.

THE BOUNDARY IS THE ENCODER BACKEND (kvm_encbe): the software Microsoft H.264 MFT (mf_h264.cpp, kvm_encbe_mf) is one table of functions; a hardware MFT is another one with the same calls. The
thread, the slots of the jobs (kvm_encjob), the watchdog and the tiles in flight know nothing about Media Foundation. The data types of mf_h264.h (mfh264_cfg, mfh264_err, mfh264_info) are the vocabulary
of the interface; a backend that works asynchronously inside keeps the contract "one job in, one result out, in the order of the jobs" and may take up to KVM_EJ_MAX jobs at a time (the depth).

Threads and COM: mfh264_init(), every call of the backend and mfh264_shutdown() run on the thread of the encoder (mf_h264.h: the thread that called init). The main loop reaches the backend only through
kvm_encthr_call() (open, close, queries: they run on the thread, after the jobs that were submitted before) and through jobs.

Modes: threaded (the normal one), inline (MESH_KVM_ENC_THREAD=0: the same code, the job is made in kvm_encthr_submit() by the caller: the old synchronous behaviour), manual (tests: the test says when
the thread "runs", kvm_encthr_test_step()).

Locking: one critical section (a leaf: nothing else is taken while it is held, and never across a call of the backend); two auto-reset events. The thread takes no other lock and does not log.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef KVM_ENCTHR_H
#define KVM_ENCTHR_H

#include <stddef.h>
#include <stdint.h>
#include "mf_h264.h"
#include "../common/kvm_encjob.h"

#ifdef __cplusplus
extern "C" {
#endif

// The encoder behind the thread. Every function is called on the thread of the encoder.
// What a backend can do (kvm_encbe.caps)
#define KVM_ENCBE_CAP_HW 1			// the encoder is hardware (a driver: the first thing to blame, and a software backend is the way back)
#define KVM_ENCBE_CAP_ASYNC 2		// it works asynchronously inside: it may take up to KVM_EJ_MAX jobs before the first result (the contract of the results in the order of the jobs holds)
#define KVM_ENCBE_CAP_QP 4			// it listens to a constant quantizer (set_qp); without it the bit rate steers

typedef struct kvm_encbe
{
	const char *name;
	unsigned caps;				// KVM_ENCBE_CAP_*: what the glue may expect of the backend (and what it logs)
	int (*init)(struct mfh264_err *err);
	void (*shutdown)(void);
	int (*faulted)(void);
	struct mfh264 *(*open)(const struct mfh264_cfg *cfg, struct mfh264_err *err);
	void (*close)(struct mfh264 *h);
	int (*set_qp)(struct mfh264 *h, int qp, struct mfh264_err *err);
	int (*set_bitrate)(struct mfh264 *h, int bps, struct mfh264_err *err);
	int (*set_sample_qp)(struct mfh264 *h, int mode, int qp);
	int (*last_out_qp)(const struct mfh264 *h, uint64_t *raw);
	int (*force_idr)(struct mfh264 *h, struct mfh264_err *err);
	int (*encode_bgra)(struct mfh264 *h, const uint8_t *bgra, int width, int height, int stride, int64_t ts, uint8_t *out, size_t out_cap, size_t *out_len, int *is_idr, struct mfh264_err *err);
	int (*encode_nv12)(struct mfh264 *h, const uint8_t *nv12, size_t nv12_len, int64_t ts, uint8_t *out, size_t out_cap, size_t *out_len, int *is_idr, struct mfh264_err *err);	// an NV12 picture of the coded size (the view of the encoder thread)
	void (*get_info)(const struct mfh264 *h, struct mfh264_info *info);
	size_t (*max_au_size)(const struct mfh264 *h);
	size_t (*seq_header)(const struct mfh264 *h, const uint8_t **p);
} kvm_encbe;

extern const kvm_encbe kvm_encbe_mf;	// the software MFT of mf_h264.cpp

// 1 when a result code means that the encoder will fail like this again (an exception, the system or Media Foundation or the MFT is not there, asynchronous MFT that is not driven): the backend is given up, not
// the frame (a lost access unit, a refused input, a failed output are about one frame: kvm_vctl_frame_broken counts them). The log says it, and the glue of the hardware encoder decides on it.
int kvm_encthr_error_structural(int code);
// The backend of the thread that runs (NULL when none): its name goes into the log lines of the errors
const kvm_encbe *kvm_encthr_backend(void);

// What a call that runs on the thread gets: the backend and the open encoder (NULL when none; the call may open or close it)
typedef struct kvm_encctx
{
	const kvm_encbe *be;
	struct mfh264 *enc;
} kvm_encctx;
typedef void (*kvm_encfn)(kvm_encctx *ctx, void *arg);

#define KVM_ENCTHR_INLINE 0
#define KVM_ENCTHR_THREADED 1
#define KVM_ENCTHR_MANUAL 2		// tests: nothing runs until kvm_encthr_test_step()

// Starts the encoder side: the thread (in the threaded mode) makes itself a COM thread and runs be->init(). Waits for that (<= initTimeoutMs). Returns 0, or the code of init (*err filled),
// or MFH264_E_NOINIT when the thread cannot be made or does not answer (it is then abandoned). `depth` = the jobs that may be in flight (1 .. KVM_EJ_MAX).
int kvm_encthr_start(const kvm_encbe *be, int mode, int depth, uint32_t initTimeoutMs, struct mfh264_err *err);
// Ends it: closes the encoder, be->shutdown(), joins the thread (<= timeoutMs; after that the thread and its state are left to the process). Safe when not started.
void kvm_encthr_stop(uint32_t timeoutMs);
int kvm_encthr_started(void);
int kvm_encthr_mode(void);
// The encoder handle for "is there one" tests by the main loop (only the thread changes it, and only while the main loop waits in kvm_encthr_call or has no job in flight); NULL when none
struct mfh264 *kvm_encthr_handle(void);

// Runs fn(ctx, arg) on the thread of the encoder, after the jobs that were submitted before it, and waits for it (<= timeoutMs). fn is ANY function of the caller (the one that opens the encoder, the one that
// enumerates the hardware MFTs of the hardware encoder): it runs on the thread of the encoder, which never calls SetThreadDesktop (the main loop and the capture thread do), so MFTEnumEx and the creation of a hardware MFT are
// not done on a thread that was moved to the desktop of the user. 0 = it ran; -1 = it did not finish in time or the encoder
// was abandoned: the thread is then given up for good (kvm_encthr_abandoned()), what fn may still be doing belongs to nobody.
int kvm_encthr_call(kvm_encfn fn, void *arg, uint32_t timeoutMs);

// ---- jobs (the main loop) ----
int kvm_encthr_submit(const kvm_ejob *job, uint32_t now);			// slot, or -1 (no free slot, abandoned, not started). Inline mode: the frame is made before it returns.
int kvm_encthr_collect(kvm_ejob *job, kvm_ejres *res, int *cancelled);	// the oldest job if it is done (slot), else -1; no waiting
int kvm_encthr_ready(void);
int kvm_encthr_inflight(void);										// jobs between submit and collect
int kvm_encthr_free_slot(void);										// the slot that the next submit would use, or -1 (all in use, abandoned, not started)
int kvm_encthr_wait(uint32_t ms);									// waits until a result is ready (<= ms): 1 = ready
void *kvm_encthr_event(void);										// Windows: the event (HANDLE) that is set when a job is done, for WaitForMultipleObjects; NULL otherwise
int kvm_encthr_cancel_all(void);									// the jobs in flight are given up (their results are marked); returns how many
uint32_t kvm_encthr_oldest_age(uint32_t now);						// ms since the submit of the oldest job that is not done (0 = none)
// The watchdog gave up: every job in flight is abandoned for good, and so is the thread. Returns the number of jobs.
int kvm_encthr_abandon(void);
int kvm_encthr_abandoned(void);

// A clock in microseconds for the time of a frame in the encoder (default: the system's; tests set their own)
void kvm_encthr_set_clock(long long (*clockUs)(void));
// Manual mode: the thread makes up to n jobs (and runs a waiting call); returns how many jobs it made
int kvm_encthr_test_step(int n);
// Manual mode: on = 1 makes the thread "hang" (test_step does nothing, kvm_encthr_wait lets onWait(ms) pass the time of the test); 0 lets it run again
void kvm_encthr_test_hang(int on, void (*onWait)(uint32_t ms));

#ifdef __cplusplus
}
#endif

#endif
