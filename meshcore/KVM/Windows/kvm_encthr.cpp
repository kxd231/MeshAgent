// KVM (Windows): the thread of the encoder. See kvm_encthr.h.
//
// The operating system layer is at the top (Windows: a critical section, events, CreateThread; elsewhere pthreads, so that the thread can be tested with ThreadSanitizer in Docker). Below it
// nothing depends on the system: the slots of the jobs are kvm_encjob.c, the encoder is the backend table.

#include "kvm_encthr.h"
#include "../common/kvm_nv12view.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
typedef CRITICAL_SECTION OsMutex;
typedef HANDLE OsEvent;
typedef HANDLE OsThread;
static void os_mutex_init(OsMutex *m) { InitializeCriticalSection(m); }
static void os_mutex_destroy(OsMutex *m) { DeleteCriticalSection(m); }
static void os_lock(OsMutex *m) { EnterCriticalSection(m); }
static void os_unlock(OsMutex *m) { LeaveCriticalSection(m); }
static int os_event_create(OsEvent *e) { *e = CreateEvent(NULL, FALSE, FALSE, NULL); return *e != NULL; }
static void os_event_destroy(OsEvent *e) { if (*e != NULL) { CloseHandle(*e); *e = NULL; } }
static void os_event_set(OsEvent *e) { SetEvent(*e); }
static int os_event_wait(OsEvent *e, uint32_t ms) { return WaitForSingleObject(*e, ms) == WAIT_OBJECT_0; }	// 1 = it was set
struct OsThreadStart { void (*fn)(void *); void *arg; };
static DWORD WINAPI os_thread_proc(LPVOID p) { OsThreadStart s = *(OsThreadStart *)p; free(p); s.fn(s.arg); return 0; }
static int os_thread_create(OsThread *t, void (*fn)(void *), void *arg)
{
	OsThreadStart *s = (OsThreadStart *)malloc(sizeof(*s));
	if (s == NULL) { return 0; }
	s->fn = fn; s->arg = arg;
	*t = CreateThread(NULL, 0, os_thread_proc, s, 0, NULL);
	if (*t == NULL) { free(s); return 0; }
	return 1;
}
static int os_thread_join(OsThread *t, uint32_t ms) { if (WaitForSingleObject(*t, ms) != WAIT_OBJECT_0) { return 0; } CloseHandle(*t); *t = NULL; return 1; }
static long long os_clock_us(void)
{
	static LARGE_INTEGER f;
	LARGE_INTEGER c;
	if (f.QuadPart == 0 && !QueryPerformanceFrequency(&f)) { f.QuadPart = 1; }
	QueryPerformanceCounter(&c);
	return (long long)((double)c.QuadPart * 1000000.0 / (double)f.QuadPart);
}
#else
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <time.h>
#include <errno.h>
typedef pthread_mutex_t OsMutex;
struct OsEvent { pthread_mutex_t m; pthread_cond_t c; int set; int ok; };
typedef OsEvent OsEvent;
typedef pthread_t OsThread;
static void os_mutex_init(OsMutex *m) { pthread_mutex_init(m, NULL); }
static void os_mutex_destroy(OsMutex *m) { pthread_mutex_destroy(m); }
static void os_lock(OsMutex *m) { pthread_mutex_lock(m); }
static void os_unlock(OsMutex *m) { pthread_mutex_unlock(m); }
static int os_event_create(OsEvent *e) { pthread_mutex_init(&e->m, NULL); pthread_cond_init(&e->c, NULL); e->set = 0; e->ok = 1; return 1; }
static void os_event_destroy(OsEvent *e) { if (e->ok) { pthread_cond_destroy(&e->c); pthread_mutex_destroy(&e->m); e->ok = 0; } }
static void os_event_set(OsEvent *e) { pthread_mutex_lock(&e->m); e->set = 1; pthread_cond_signal(&e->c); pthread_mutex_unlock(&e->m); }
static int os_event_wait(OsEvent *e, uint32_t ms)
{
	int got;
	struct timespec ts;
	pthread_mutex_lock(&e->m);
	if (ms != 0xFFFFFFFFu)
	{
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += ms / 1000u; ts.tv_nsec += (long)(ms % 1000u) * 1000000L;
		if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
		while (!e->set) { if (pthread_cond_timedwait(&e->c, &e->m, &ts) == ETIMEDOUT) { break; } }
	}
	else { while (!e->set) { pthread_cond_wait(&e->c, &e->m); } }
	got = e->set; e->set = 0;
	pthread_mutex_unlock(&e->m);
	return got;
}
struct OsThreadStart { void (*fn)(void *); void *arg; };
static void *os_thread_proc(void *p) { OsThreadStart s = *(OsThreadStart *)p; free(p); s.fn(s.arg); return NULL; }
static int os_thread_create(OsThread *t, void (*fn)(void *), void *arg)
{
	OsThreadStart *s = (OsThreadStart *)malloc(sizeof(*s));
	if (s == NULL) { return 0; }
	s->fn = fn; s->arg = arg;
	if (pthread_create(t, NULL, os_thread_proc, s) != 0) { free(s); return 0; }
	return 1;
}
static int os_thread_join(OsThread *t, uint32_t ms)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += ms / 1000u; ts.tv_nsec += (long)(ms % 1000u) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
	return pthread_timedjoin_np(*t, NULL, &ts) == 0;
}
static long long os_clock_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000; }
#endif

// ---------------------------------------------------------------------------------------------------------------------------------------
// the state
// ---------------------------------------------------------------------------------------------------------------------------------------

struct Cmd { int state; kvm_encfn fn; void *arg; };	// 0 none, 1 posted, 2 running, 3 done

struct Enc
{
	const kvm_encbe *be;
	int mode;
	OsMutex mu;				// guards ej, cmd, quit, abandoned
	kvm_ej ej;
	Cmd cmd;
	int quit, abandoned;
	kvm_encctx ctx;			// the encoder: touched by the thread (and by the main loop in the inline and manual modes)
	uint8_t *view;			// the NV12 view of the encoder (the coded picture): owned by the thread, freed with the Enc (it stays with an abandoned thread, like the buffers of a hung frame)
	size_t viewCap;
	int viewW, viewH;
	OsEvent evWork, evDone, evCmd, evInit;
	OsThread th;
	int haveThread, haveSync;
	int initRc;
	struct mfh264_err initErr;
};

static Enc *gE;
static long long (*gClock)(void);
static int gHang;					// manual mode: the thread does not run (a hung encoder)
static void (*gWaitHook)(uint32_t ms);

static long long now_us(void) { return gClock != NULL ? gClock() : os_clock_us(); }
void kvm_encthr_set_clock(long long (*clockUs)(void)) { gClock = clockUs; }

int kvm_encthr_error_structural(int code)
{
	switch (code)
	{
		case MFH264_E_EXCEPTION: case MFH264_E_OS: case MFH264_E_NOMF: case MFH264_E_NOINIT: case MFH264_E_CREATE: case MFH264_E_ASYNC: case MFH264_E_CODECAPI: return 1;
		default: return 0;
	}
}

const kvm_encbe *kvm_encthr_backend(void) { return gE != NULL ? gE->be : NULL; }

static void fill_err(kvm_ejres *r, int phase, int code, const struct mfh264_err *err)
{
	r->phase = phase; r->code = code; r->hr = err->hr; r->stage = err->stage; r->reason = err->reason; r->exc = err->exc_code;
	r->structural = kvm_encthr_error_structural(code);
}

// One job, on the thread of the encoder. A cancelled job is not encoded (the main loop will not write it).
static void run_job(Enc *E, const kvm_ejob *j, int cancelled, kvm_ejres *r)
{
	struct mfh264_err err;
	const kvm_encbe *be = E->be;
	struct mfh264 *h = E->ctx.enc;
	size_t len = 0;
	int isIdr = 0, rc;
	long long t0, t1;
	memset(r, 0, sizeof(*r));
	r->backend = be->name;
	if (cancelled) { return; }
	memset(&err, 0, sizeof(err));
	if (h == NULL) { r->phase = 0; r->code = MFH264_E_NOINIT; r->stage = "no encoder"; r->structural = 1; return; }
	if (j->forceIdr && (rc = be->force_idr(h, &err)) != 0) { fill_err(r, 1, rc, &err); return; }
	if (j->setBitrate > 0)
	{
		if ((rc = be->set_bitrate(h, j->setBitrate, &err)) != 0) { fill_err(r, 3, rc, &err); return; }
	}
	else if (j->setQp > 0)
	{
		if ((rc = be->set_qp(h, j->setQp, &err)) != 0) { fill_err(r, 2, rc, &err); return; }
		r->qpApplied = j->setQp;
	}
	memset(&err, 0, sizeof(err));
	t0 = now_us();
	if (j->viewMode != 0 && be->encode_nv12 != NULL)
	{
		// the thread's view of the picture is brought up to date as the plan of the loop says (the map is a copy in the job), then it is encoded
		const size_t need = (size_t)j->width * (size_t)j->height * 3 / 2;
		long long c0, c1;
		int conv;
		if (E->view == NULL || E->viewW != j->width || E->viewH != j->height)
		{
			free(E->view);
			E->view = (uint8_t *)calloc(1, need);
			E->viewW = j->width; E->viewH = j->height; E->viewCap = E->view != NULL ? need : 0;
		}
		if (E->view == NULL) { r->phase = 0; r->code = MFH264_E_ALLOC; r->stage = "the view of the encoder"; r->structural = 0; return; }
		c0 = now_us();
		conv = kvm_nv12v_apply(E->view, j->top, j->width, j->height, j->stride, j->width, j->height, j->viewMap, j->mbCols, j->mbRows, j->viewFull, j->viewFlat);
		c1 = now_us();
		r->convUs = c1 > c0 ? (unsigned)(c1 - c0 > 0xFFFFFFFFll ? 0xFFFFFFFFll : c1 - c0) : 0u;
		if (conv < 0) { r->phase = 0; r->code = MFH264_E_ARG; r->stage = "the view of the encoder: the plan"; r->structural = 0; return; }
		r->convMbs = (unsigned)conv;
		rc = be->encode_nv12(h, E->view, need, j->ts, j->out, j->outCap, &len, &isIdr, &err);
	}
	else { rc = be->encode_bgra(h, j->top, j->width, j->height, j->stride, j->ts, j->out, j->outCap, &len, &isIdr, &err); }
	t1 = now_us();
	r->encUs = t1 > t0 ? (unsigned)(t1 - t0 > 0xFFFFFFFFll ? 0xFFFFFFFFll : t1 - t0) : 0u;
	r->len = len; r->isIdr = isIdr;
	fill_err(r, 0, rc, &err);
	if (rc == 0) { uint64_t raw = 0; r->haveQp = be->last_out_qp(h, &raw) ? 1 : 0; r->qpRaw = raw; }
}

// What the thread has to do now: every job that was submitted, then a call that was posted. Returns the number of jobs made.
static int step(Enc *E)
{
	int n = 0, runCmd = 0;
	kvm_encfn fn = NULL;
	void *arg = NULL;
	for (;;)
	{
		kvm_ejob job;
		kvm_ejres res;
		int slot, cancelled = 0;
		os_lock(&E->mu);
		slot = kvm_ej_take(&E->ej, &job, &cancelled);
		os_unlock(&E->mu);
		if (slot < 0) { break; }
		run_job(E, &job, cancelled, &res);
		os_lock(&E->mu);
		kvm_ej_finish(&E->ej, slot, &res);
		os_unlock(&E->mu);
		os_event_set(&E->evDone);
		n++;
	}
	os_lock(&E->mu);
	if (E->cmd.state == 1) { E->cmd.state = 2; fn = E->cmd.fn; arg = E->cmd.arg; runCmd = 1; }
	os_unlock(&E->mu);
	if (runCmd)
	{
		fn(&E->ctx, arg);
		os_lock(&E->mu);
		E->cmd.state = 3;
		os_unlock(&E->mu);
		os_event_set(&E->evCmd);
	}
	return n;
}

static void end_encoder(Enc *E)
{
	if (E->ctx.enc != NULL) { E->be->close(E->ctx.enc); E->ctx.enc = NULL; }
	E->be->shutdown();
}

static void thread_main(void *p)
{
	Enc *E = (Enc *)p;
	memset(&E->initErr, 0, sizeof(E->initErr));
	E->initRc = E->be->init(&E->initErr);
	os_event_set(&E->evInit);
	if (E->initRc != 0) { return; }
	for (;;)
	{
		int quit;
		(void)os_event_wait(&E->evWork, 0xFFFFFFFFu);
		(void)step(E);
		os_lock(&E->mu);
		quit = E->quit;
		os_unlock(&E->mu);
		if (quit) { break; }
	}
	end_encoder(E);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the interface
// ---------------------------------------------------------------------------------------------------------------------------------------

static void destroy(Enc *E)
{
	os_event_destroy(&E->evWork); os_event_destroy(&E->evDone); os_event_destroy(&E->evCmd); os_event_destroy(&E->evInit);
	os_mutex_destroy(&E->mu);
	free(E->view);
	free(E);
}

int kvm_encthr_start(const kvm_encbe *be, int mode, int depth, uint32_t initTimeoutMs, struct mfh264_err *err)
{
	Enc *E;
	struct mfh264_err local;
	if (err == NULL) { err = &local; }
	memset(err, 0, sizeof(*err));
	if (gE != NULL) { kvm_encthr_stop(2000); }
	E = (Enc *)calloc(1, sizeof(Enc));
	if (E == NULL) { err->code = MFH264_E_ALLOC; err->stage = "encoder thread: memory"; return MFH264_E_ALLOC; }
	E->be = be; E->mode = mode;
	kvm_ej_init(&E->ej, depth);
	os_mutex_init(&E->mu);
	if (!os_event_create(&E->evWork) || !os_event_create(&E->evDone) || !os_event_create(&E->evCmd) || !os_event_create(&E->evInit)) { destroy(E); err->code = MFH264_E_ALLOC; err->stage = "encoder thread: events"; return MFH264_E_ALLOC; }
	E->ctx.be = be; E->ctx.enc = NULL;
	if (mode != KVM_ENCTHR_THREADED)
	{
		int rc = be->init(err);
		if (rc != 0) { destroy(E); return rc; }
		gE = E;
		return 0;
	}
	if (!os_thread_create(&E->th, thread_main, E)) { destroy(E); err->code = MFH264_E_NOINIT; err->stage = "encoder thread: CreateThread"; return MFH264_E_NOINIT; }
	E->haveThread = 1;
	if (!os_event_wait(&E->evInit, initTimeoutMs))
	{
		// the thread does not answer: it is left to the process with its state (it may still be inside the initialisation)
		os_lock(&E->mu); E->abandoned = 1; os_unlock(&E->mu);
		err->code = MFH264_E_NOINIT; err->stage = "encoder thread: init timeout";
		return MFH264_E_NOINIT;
	}
	if (E->initRc != 0)
	{
		*err = E->initErr;
		(void)os_thread_join(&E->th, 2000);
		destroy(E);
		return err->code != 0 ? err->code : MFH264_E_NOINIT;
	}
	gE = E;
	return 0;
}

void kvm_encthr_stop(uint32_t timeoutMs)
{
	Enc *E = gE;
	if (E == NULL) { return; }
	gE = NULL;
	if (E->abandoned) { if (E->mode != KVM_ENCTHR_THREADED) { destroy(E); } return; }	// the thread and its state stay: it may still touch them (no thread in the inline and manual modes: nothing does)
	if (E->mode != KVM_ENCTHR_THREADED) { end_encoder(E); destroy(E); return; }
	os_lock(&E->mu); E->quit = 1; os_unlock(&E->mu);
	os_event_set(&E->evWork);
	if (os_thread_join(&E->th, timeoutMs)) { destroy(E); }
	// else: left to the process (it ends with it)
}

int kvm_encthr_started(void) { return gE != NULL; }
int kvm_encthr_mode(void) { return gE != NULL ? gE->mode : -1; }
struct mfh264 *kvm_encthr_handle(void) { return gE != NULL ? gE->ctx.enc : NULL; }

int kvm_encthr_abandoned(void)
{
	int a;
	if (gE == NULL) { return 0; }
	os_lock(&gE->mu); a = gE->abandoned; os_unlock(&gE->mu);
	return a;
}

int kvm_encthr_abandon(void)
{
	int n;
	if (gE == NULL) { return 0; }
	os_lock(&gE->mu);
	gE->abandoned = 1;
	n = kvm_ej_abandon_all(&gE->ej);
	os_unlock(&gE->mu);
	return n;
}

int kvm_encthr_call(kvm_encfn fn, void *arg, uint32_t timeoutMs)
{
	Enc *E = gE;
	if (E == NULL || kvm_encthr_abandoned()) { return -1; }
	if (E->mode != KVM_ENCTHR_THREADED)
	{
		(void)step(E);		// the jobs that are waiting come first
		fn(&E->ctx, arg);
		return 0;
	}
	os_lock(&E->mu);
	E->cmd.state = 1; E->cmd.fn = fn; E->cmd.arg = arg;
	os_unlock(&E->mu);
	os_event_set(&E->evWork);
	if (!os_event_wait(&E->evCmd, timeoutMs))
	{
		os_lock(&E->mu); E->abandoned = 1; (void)kvm_ej_abandon_all(&E->ej); os_unlock(&E->mu);
		return -1;
	}
	os_lock(&E->mu); E->cmd.state = 0; os_unlock(&E->mu);
	return 0;
}

int kvm_encthr_submit(const kvm_ejob *job, uint32_t now)
{
	Enc *E = gE;
	int slot;
	if (E == NULL) { return -1; }
	os_lock(&E->mu);
	slot = E->abandoned ? -1 : kvm_ej_submit(&E->ej, job, now);
	os_unlock(&E->mu);
	if (slot < 0) { return -1; }
	if (E->mode == KVM_ENCTHR_THREADED) { os_event_set(&E->evWork); }
	else if (E->mode == KVM_ENCTHR_INLINE) { (void)step(E); }
	return slot;
}

int kvm_encthr_collect(kvm_ejob *job, kvm_ejres *res, int *cancelled)
{
	Enc *E = gE;
	int slot;
	if (E == NULL) { return -1; }
	os_lock(&E->mu);
	slot = kvm_ej_collect(&E->ej, job, res, cancelled);
	os_unlock(&E->mu);
	return slot;
}

int kvm_encthr_ready(void)
{
	int r;
	if (gE == NULL) { return 0; }
	os_lock(&gE->mu); r = kvm_ej_ready(&gE->ej); os_unlock(&gE->mu);
	return r;
}

int kvm_encthr_inflight(void)
{
	int r;
	if (gE == NULL) { return 0; }
	os_lock(&gE->mu); r = kvm_ej_inflight(&gE->ej); os_unlock(&gE->mu);
	return r;
}

int kvm_encthr_free_slot(void)
{
	int r;
	if (gE == NULL) { return -1; }
	os_lock(&gE->mu); r = gE->abandoned ? -1 : kvm_ej_free_slot(&gE->ej); os_unlock(&gE->mu);
	return r;
}

int kvm_encthr_wait(uint32_t ms)
{
	if (gE == NULL) { return 0; }
	if (kvm_encthr_ready()) { return 1; }
	if (gE->mode == KVM_ENCTHR_THREADED && ms > 0)
	{
		// the event may be set by a job that the loop has collected already (it is not reset by collect): a wake with nothing to collect is not the end of the wait
		const long long end = os_clock_us() + (long long)ms * 1000ll;
		for (;;)
		{
			long long rem;
			if (kvm_encthr_ready()) { return 1; }
			rem = (end - os_clock_us()) / 1000ll;
			if (rem <= 0) { break; }
			(void)os_event_wait(&gE->evDone, (uint32_t)rem);
		}
	}
	if (gE->mode == KVM_ENCTHR_MANUAL)
	{
		// the thread of the manual mode makes progress while the loop waits: one job (unless the test made it "hang"), and the hook lets the fake clock pass
		if (gHang) { if (gWaitHook != NULL) { gWaitHook(ms); } }
		else { (void)kvm_encthr_test_step(1); }
	}
	return kvm_encthr_ready();
}

void *kvm_encthr_event(void)
{
#ifdef _WIN32
	return gE != NULL && gE->mode == KVM_ENCTHR_THREADED ? (void *)gE->evDone : NULL;
#else
	return NULL;
#endif
}

int kvm_encthr_cancel_all(void)
{
	int n;
	if (gE == NULL) { return 0; }
	os_lock(&gE->mu); n = kvm_ej_cancel_all(&gE->ej); os_unlock(&gE->mu);
	return n;
}

uint32_t kvm_encthr_oldest_age(uint32_t now)
{
	uint32_t a;
	if (gE == NULL) { return 0; }
	os_lock(&gE->mu); a = kvm_ej_oldest_age(&gE->ej, now); os_unlock(&gE->mu);
	return a;
}

void kvm_encthr_test_hang(int on, void (*onWait)(uint32_t ms)) { gHang = on; gWaitHook = onWait; }

int kvm_encthr_test_step(int n)
{
	int made = 0;
	if (gE == NULL || gE->mode != KVM_ENCTHR_MANUAL || gHang) { return 0; }
	// one job at a time (n jobs), then a call that waits
	while (n-- > 0)
	{
		kvm_ejob job;
		kvm_ejres res;
		int slot, cancelled = 0;
		os_lock(&gE->mu);
		slot = kvm_ej_take(&gE->ej, &job, &cancelled);
		os_unlock(&gE->mu);
		if (slot < 0) { break; }
		run_job(gE, &job, cancelled, &res);
		os_lock(&gE->mu);
		kvm_ej_finish(&gE->ej, slot, &res);
		os_unlock(&gE->mu);
		made++;
	}
	return made;
}
