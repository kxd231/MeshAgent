/*
KVM: the hand-off of frames between the main loop and the thread of the encoder. Portable C99, no operating system API: the slots of the jobs, their order, cancel and
abandon. The callers add the locking (Windows/kvm_encthr.cpp holds gEncLock around every call) and the events; the tests drive the "thread" calls (take, finish) from the test, so the
interleavings are deterministic.

Who calls what: the main loop calls init, submit, collect, cancel_all, abandon_all and the queries; the encoder thread calls take and finish. A job goes
FREE -> SUBMITTED (submit) -> RUNNING (take) -> DONE (finish) -> FREE (collect). Results come out of collect in the order of submit (FIFO), also when more than one job is in flight (depth up to
KVM_EJ_MAX: the pipeline of a later asynchronous encoder). A cancelled job is still taken and finished (so the thread never works on a slot the main loop has given up), but its result is
marked cancelled and the caller does not write it .
An abandoned slot (the encoder hung, the watchdog gave up) never becomes FREE again and a late finish() is ignored: the buffers of the job stay with the caller, which leaks them.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef KVM_ENCJOB_H
#define KVM_ENCJOB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KVM_EJ_MAX 3
#define KVM_EJ_MB_MAX 36864		// the macroblocks of the map that a job carries (kvm_nv12view.h KVM_NV12V_MB_MAX)

#define KVM_EJS_FREE 0
#define KVM_EJS_SUBMITTED 1
#define KVM_EJS_RUNNING 2
#define KVM_EJS_DONE 3
#define KVM_EJS_ABANDONED 4

// What the encoder gets (set by the main loop). The picture is read only; `out` is the buffer for the access unit (the caller's frame buffer, after the header).
typedef struct kvm_ejob
{
	uint32_t gen;				// generation of the stream the job belongs to (the caller compares it with the stream's when the result comes)
	int forceIdr;				// the frame is to be an IDR (call force_idr before)
	int setQp;					// > 0: set this quantizer before the frame; 0: leave
	int setBitrate;				// > 0: set this bit rate (bits per second) before the frame; 0: leave
	const uint8_t *top;			// the top row of the BGRA picture
	int width, height, stride;	// coded size and stride in bytes (negative for a bottom-up bitmap)
	int64_t ts;					// time stamp, 100 ns
	// the NV12 view: viewMode 0 = the picture is encoded as it is; 1 = the thread brings its NV12 view up to date from the picture as the map says and encodes the view. viewFull: the whole picture;
	// viewFlat: the view is made flat first; viewMap: mbCols x mbRows bytes, 1 = convert. The map is not part of the job: kvm_ej_submit COPIES it from the pointer of the caller (which has to be valid
	// only during the call) into the slot of the job, and the job that kvm_ej_take gives the thread points at that copy, which nothing else writes until finish. So there is no pointer into the
	// memory of the loop in a running job (B1), and a job is not 36 KB. kvm_ej_collect gives viewMap NULL.
	int viewMode, viewFull, viewFlat, mbCols, mbRows;
	const uint8_t *viewMap;
	uint8_t *out;				// buffer of the access unit
	size_t outCap;
} kvm_ejob;

// What the encoder says (set by the thread)
typedef struct kvm_ejres
{
	int phase;					// where the code comes from: 0 the frame, 1 the forced key frame, 2 the quantizer, 3 the bit rate (the caller names the stage in its log)
	int code;					// 0 = access unit made, else the encoder's negative result (MFH264_E_*)
	long hr;					// details of an error
	const char *stage;			// static string
	int reason;
	unsigned long exc;
	size_t len;					// size of the access unit
	int isIdr;
	unsigned encUs;				// wall time of the frame in the encoder, microseconds (the conversion of the view included)
	unsigned convUs;			// of it: the conversion of the view, and the macroblocks that it converted
	unsigned convMbs;
	const char *backend;		// the name of the backend that made (or failed) the frame (a static string): the log of the errors says which one it was
	int structural;				// the error is of the kind that repeats for every frame (an exception, the encoder is not there): not a lost access unit (set by the thread)
	int haveQp;					// qpRaw is valid
	uint64_t qpRaw;				// the QP attribute of the output sample as the encoder wrote it
	int qpApplied;				// the quantizer the frame was encoded with (0 = not known / bit rate mode)
} kvm_ejres;

typedef struct kvm_ejslot
{
	uint8_t state;
	uint8_t cancelled;
	uint32_t order;				// submit counter: the FIFO order
	uint32_t startedAt;			// time of the submit (the watchdog's clock), ms
	kvm_ejob job;
	kvm_ejres res;
	uint8_t map[KVM_EJ_MB_MAX];	// the copy of the map of the view (job.viewMap points here from the submit to the collect)
} kvm_ejslot;

typedef struct kvm_ej
{
	int depth;					// slots in use (1 .. KVM_EJ_MAX)
	uint32_t nextOrder;
	uint32_t collectOrder;		// the order that collect() delivers next
	uint32_t submitted, collected, cancelledN, abandonedN;	// counters
	kvm_ejslot slot[KVM_EJ_MAX];
} kvm_ej;

void kvm_ej_init(kvm_ej *e, int depth);

// ---- main loop ----
// A free slot (the lowest index), or -1 when all slots are in use or abandoned.
int kvm_ej_free_slot(const kvm_ej *e);
// The job goes to the encoder. Returns the slot, or -1 when none is free. The slot is SUBMITTED. The map of the view (job->viewMap, mbCols x mbRows bytes) is copied into the slot; a job of the view whose
// map is missing (and is not a complete conversion) or does not fit KVM_EJ_MB_MAX is made a job of the picture as it is (viewMode 0: always right, only without the saving).
int kvm_ej_submit(kvm_ej *e, const kvm_ejob *job, uint32_t now);
// The oldest job that is in the encoder (SUBMITTED, RUNNING or DONE): its slot, else -1. For depth 1 this is the one job.
int kvm_ej_oldest(const kvm_ej *e);
// Jobs between submit and collect (not FREE, not abandoned)
int kvm_ej_inflight(const kvm_ej *e);
// 1 when the oldest job is DONE, so collect() would deliver it
int kvm_ej_ready(const kvm_ej *e);
// Delivers the oldest job if it is DONE: copies the job (viewMap NULL: the copy of the map is not given back) and the result, *cancelled says whether the main loop gave it up; the slot becomes FREE. Returns the slot, or -1.
int kvm_ej_collect(kvm_ej *e, kvm_ejob *job, kvm_ejres *res, int *cancelled);
// The jobs in flight are given up: their results will not be written. Returns the number of jobs marked.
int kvm_ej_cancel_all(kvm_ej *e);
// The watchdog: every job in flight becomes ABANDONED for good (the slots are never used again). Returns the number of slots. Afterwards free_slot() is -1 for them.
int kvm_ej_abandon_all(kvm_ej *e);
// Age in ms of the oldest job that has not been collected yet (SUBMITTED or RUNNING counts from the submit; a DONE job has no age); 0 when there is none
uint32_t kvm_ej_oldest_age(const kvm_ej *e, uint32_t now);
// 1 when slot is abandoned (the buffers belong to nobody)
int kvm_ej_is_abandoned(const kvm_ej *e, int slot);

// ---- the thread of the encoder ----
// The next job to work on (the oldest SUBMITTED); it becomes RUNNING. Returns the slot or -1. *job gets a copy (its viewMap points at the copy of the map in the slot: valid until finish), *cancelled is set when the job was given up (the thread then does not encode: it calls finish with code 0 and len 0).
int kvm_ej_take(kvm_ej *e, kvm_ejob *job, int *cancelled);
// The job of the slot is done. Ignored when the slot was abandoned (a late result).
void kvm_ej_finish(kvm_ej *e, int slot, const kvm_ejres *res);

#ifdef __cplusplus
}
#endif

#endif
