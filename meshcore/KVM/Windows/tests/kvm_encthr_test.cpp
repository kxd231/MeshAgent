// Tests of kvm_encthr.cpp on Linux: the real thread (pthreads in place of Win32) against a fake encoder backend. Run with run-kvm-encthr-tests.sh: plain, ASan + UBSan and
// ThreadSanitizer. What is checked: the order of the jobs and of the calls on the thread, cancel, the watchdog (a backend that hangs), init that fails, the modes inline / manual, and a stress
// of many sessions with random delays and random cancels (every wait of the "main loop" is bounded: a deadlock is a failed test, not a hang).
#include "kvm_encthr.h"
#include "../common/kvm_yuv.h"
#include "../common/kvm_nv12view.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <atomic>
#include <mutex>
#include <vector>

static int gFail = 0, gChecks = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); gChecks++; if (_a != _b) { gFail++; printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

// ---- the fake backend: the "encoder" writes the job's number into the access unit; it records which thread called it ----
static pthread_t gBeThread;
static pthread_mutex_t gBeMu = PTHREAD_MUTEX_INITIALIZER;
static bool gBeThreadSet;
static std::atomic<int> gHang;			// the encode blocks while this is set
static std::atomic<int> gDelayUs;		// the encode takes this long
static std::atomic<int> gFailInit, gFailEncodeAt, gFailExc;
static std::atomic<int> gEncoded, gOpens, gCloses, gInits, gShutdowns, gWrongThread;
static std::atomic<int> gForce, gQp, gBps;
struct mfh264 { int id; };
static struct mfh264 gH;

static void note_thread(void)
{
	pthread_mutex_lock(&gBeMu);
	if (!gBeThreadSet) { gBeThread = pthread_self(); gBeThreadSet = true; }
	else if (!pthread_equal(gBeThread, pthread_self())) { gWrongThread++; }
	pthread_mutex_unlock(&gBeMu);
}
static int be_init(struct mfh264_err *e) { pthread_mutex_lock(&gBeMu); gBeThreadSet = false; pthread_mutex_unlock(&gBeMu); note_thread(); gInits++; if (gFailInit) { e->code = MFH264_E_NOMF; e->stage = "fake init"; return MFH264_E_NOMF; } return 0; }
static void be_shutdown(void) { note_thread(); gShutdowns++; }
static int be_faulted(void) { return 0; }
static struct mfh264 *be_open(const struct mfh264_cfg *c, struct mfh264_err *e) { (void)c; (void)e; note_thread(); gOpens++; return &gH; }
static void be_close(struct mfh264 *h) { (void)h; note_thread(); gCloses++; }
static int be_set_qp(struct mfh264 *h, int qp, struct mfh264_err *e) { (void)h; (void)e; note_thread(); gQp = qp; return 0; }
static int be_set_bitrate(struct mfh264 *h, int bps, struct mfh264_err *e) { (void)h; (void)e; note_thread(); gBps = bps; return 0; }
static int be_set_sample_qp(struct mfh264 *h, int m, int q) { (void)h; (void)m; (void)q; note_thread(); return 0; }
static int be_last_out_qp(const struct mfh264 *h, uint64_t *raw) { (void)h; *raw = 28; return 1; }
static int be_force_idr(struct mfh264 *h, struct mfh264_err *e) { (void)h; (void)e; note_thread(); gForce++; return 0; }
static int be_encode(struct mfh264 *h, const uint8_t *bgra, int w, int hh, int stride, int64_t ts, uint8_t *out, size_t cap, size_t *len, int *idr, struct mfh264_err *e)
{
	(void)h; (void)w; (void)hh; (void)stride; (void)cap;
	note_thread();
	while (gHang) { usleep(1000); }
	if (gDelayUs.load() > 0) { usleep((useconds_t)gDelayUs.load()); }
	gEncoded++;
	if (gFailExc) { e->code = MFH264_E_EXCEPTION; e->exc_code = 0xC0000005ul; e->stage = "fake exception"; return MFH264_E_EXCEPTION; }
	if (gFailEncodeAt.load() > 0 && gEncoded.load() == gFailEncodeAt.load()) { e->code = MFH264_E_STREAM_BROKEN; e->stage = "fake"; e->reason = MFH264_BROKEN_NO_OUTPUT; return MFH264_E_STREAM_BROKEN; }
	out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 1; out[4] = (uint8_t)ts; out[5] = bgra[0];
	*len = 6; *idr = gForce.load() > 0 && (ts & 1) ? 1 : 0;
	return 0;
}
static std::mutex gViewMu;
static std::vector<uint8_t> gViewSeen;
static std::atomic<int> gNv12Calls(0);
static int be_encode_nv12(struct mfh264 *h, const uint8_t *nv12, size_t len, int64_t ts, uint8_t *out, size_t cap, size_t *outLen, int *idr, struct mfh264_err *e)
{
	(void)h; (void)cap; (void)e;
	note_thread();
	gNv12Calls++;
	{ std::lock_guard<std::mutex> lk(gViewMu); gViewSeen.assign(nv12, nv12 + len); }
	out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 1; out[4] = (uint8_t)ts; out[5] = nv12[0];
	*outLen = 6; *idr = 0;
	return 0;
}
static void be_get_info(const struct mfh264 *h, struct mfh264_info *i) { (void)h; memset(i, 0, sizeof(*i)); i->sps_profile_idc = 100; }
static size_t be_max_au(const struct mfh264 *h) { (void)h; return 1 << 20; }
static size_t be_seq(const struct mfh264 *h, const uint8_t **p) { (void)h; *p = NULL; return 0; }
static const kvm_encbe gBe = { "fake", 0, be_init, be_shutdown, be_faulted, be_open, be_close, be_set_qp, be_set_bitrate, be_set_sample_qp, be_last_out_qp, be_force_idr, be_encode, be_encode_nv12, be_get_info, be_max_au, be_seq };

// a second backend (the software one after a hung hardware one: the hardware encoder): the same fake under another name and with other caps
static const kvm_encbe gBeB = { "fake B", KVM_ENCBE_CAP_QP, be_init, be_shutdown, be_faulted, be_open, be_close, be_set_qp, be_set_bitrate, be_set_sample_qp, be_last_out_qp, be_force_idr, be_encode, be_encode_nv12, be_get_info, be_max_au, be_seq };
static const kvm_encbe gBeA = { "fake A", KVM_ENCBE_CAP_HW | KVM_ENCBE_CAP_ASYNC, be_init, be_shutdown, be_faulted, be_open, be_close, be_set_qp, be_set_bitrate, be_set_sample_qp, be_last_out_qp, be_force_idr, be_encode, be_encode_nv12, be_get_info, be_max_au, be_seq };

static void reset(void) { gHang = 0; gDelayUs = 0; gFailInit = 0; gFailEncodeAt = 0; gFailExc = 0; gEncoded = 0; gOpens = 0; gCloses = 0; gInits = 0; gShutdowns = 0; gWrongThread = 0; gForce = 0; gQp = 0; gBps = 0; pthread_mutex_lock(&gBeMu); gBeThreadSet = false; pthread_mutex_unlock(&gBeMu); }

static uint8_t gPic[16];
static kvm_ejob mkjob(uint32_t gen, int64_t ts, uint8_t *out)
{
	kvm_ejob j; memset(&j, 0, sizeof(j));
	j.gen = gen; j.top = gPic; j.width = 16; j.height = 16; j.stride = 64; j.ts = ts; j.out = out; j.outCap = 64;
	return j;
}

// waits for a result like the main loop does (bounded)
static int wait_ready(unsigned ms) { return kvm_encthr_wait(ms); }

static void open_fn(kvm_encctx *c, void *arg) { struct mfh264_cfg cfg = MFH264_CFG_INIT(16, 16); struct mfh264_err e; (void)arg; memset(&e, 0, sizeof(e)); c->enc = c->be->open(&cfg, &e); }
static void close_fn(kvm_encctx *c, void *arg) { (void)arg; if (c->enc) { c->be->close(c->enc); c->enc = NULL; } }
static void order_fn(kvm_encctx *c, void *arg) { (void)c; *(int *)arg = gEncoded.load(); }	// records how many jobs had been made when it ran

static void test_threaded_basic(void)
{
	struct mfh264_err err;
	static uint8_t out[3][64];
	kvm_ejob j, jc;
	kvm_ejres r;
	int c, s, v = -1;
	reset();
	CHECK_EQ(kvm_encthr_start(&gBe, KVM_ENCTHR_THREADED, 1, 3000, &err), 0);
	CHECK_EQ(gInits, 1);
	CHECK(kvm_encthr_started());
	CHECK(kvm_encthr_handle() == NULL);
	CHECK_EQ(kvm_encthr_call(open_fn, NULL, 3000), 0);
	CHECK(kvm_encthr_handle() != NULL);
	gPic[0] = 77;
	j = mkjob(5, 1, out[0]); j.forceIdr = 1; j.setQp = 30;
	s = kvm_encthr_submit(&j, 1000);
	CHECK_EQ(s, 0);
	CHECK_EQ(kvm_encthr_free_slot(), -1);							// depth 1: the slot is in use
	CHECK_EQ(kvm_encthr_submit(&j, 1000), -1);
	CHECK(wait_ready(3000));
	CHECK_EQ(kvm_encthr_collect(&jc, &r, &c), 0);
	CHECK_EQ(c, 0); CHECK_EQ(r.code, 0); CHECK_EQ(r.len, 6); CHECK_EQ(out[0][4], 1); CHECK_EQ(out[0][5], 77);
	CHECK_EQ(r.isIdr, 1);
	CHECK_EQ(gQp.load(), 30); CHECK_EQ(gForce.load(), 1);
	CHECK_EQ(r.qpApplied, 30); CHECK(r.haveQp); CHECK_EQ(r.qpRaw, 28);
	CHECK_EQ(jc.gen, 5);
	// a bit rate
	j = mkjob(5, 2, out[1]); j.setBitrate = 123456;
	kvm_encthr_submit(&j, 1000);
	CHECK(wait_ready(3000)); kvm_encthr_collect(&jc, &r, &c);
	CHECK_EQ(gBps.load(), 123456);
	// a call runs after the job that was submitted before it (on the same thread)
	gDelayUs = 30000;
	j = mkjob(5, 3, out[2]);
	kvm_encthr_submit(&j, 1000);
	CHECK_EQ(kvm_encthr_call(order_fn, &v, 3000), 0);
	CHECK_EQ(v, 3);												// the third job had been made when the call ran
	CHECK(wait_ready(1000)); kvm_encthr_collect(&jc, &r, &c);
	gDelayUs = 0;
	CHECK_EQ(kvm_encthr_call(close_fn, NULL, 3000), 0);
	CHECK(kvm_encthr_handle() == NULL);
	kvm_encthr_stop(3000);
	CHECK(!kvm_encthr_started());
	CHECK_EQ(gShutdowns, 1);
	CHECK_EQ(gWrongThread, 0);									// init, open, encode, close, shutdown: all on the one thread
	(void)s;
}

// the event of the encoder stays set after a job was collected without waiting for it: a later wait must not end with that stale wake
static void test_stale_event(void)
{
	struct mfh264_err err;
	static uint8_t out[2][64];
	kvm_ejob j, jc;
	kvm_ejres r;
	int c;
	reset();
	CHECK_EQ(kvm_encthr_start(&gBe, KVM_ENCTHR_THREADED, 1, 3000, &err), 0);
	kvm_encthr_call(open_fn, NULL, 3000);
	j = mkjob(1, 1, out[0]); kvm_encthr_submit(&j, 0);
	while (!kvm_encthr_ready()) { usleep(500); }						// polled, not waited for: the event stays set
	CHECK_EQ(kvm_encthr_collect(&jc, &r, &c), 0);
	gDelayUs = 40000;
	j = mkjob(1, 2, out[1]); kvm_encthr_submit(&j, 0);
	CHECK(kvm_encthr_wait(3000));									// the second job takes 40 ms: the stale event must not end the wait
	CHECK_EQ(kvm_encthr_collect(&jc, &r, &c), 0);
	gDelayUs = 0;
	kvm_encthr_stop(3000);
}

static void test_cancel_and_errors(void)
{
	struct mfh264_err err;
	static uint8_t out[64];
	kvm_ejob j, jc;
	kvm_ejres r;
	int c;
	reset();
	CHECK_EQ(kvm_encthr_start(&gBe, KVM_ENCTHR_THREADED, 2, 3000, &err), 0);
	kvm_encthr_call(open_fn, NULL, 3000);
	// a cancelled job is not encoded
	gHang = 1;
	j = mkjob(1, 1, out); kvm_encthr_submit(&j, 0);
	usleep(20000);													// the thread is inside the hung encode
	CHECK_EQ(kvm_encthr_cancel_all(), 1);
	gHang = 0;
	CHECK(wait_ready(3000)); kvm_encthr_collect(&jc, &r, &c);
	CHECK_EQ(c, 1);
	// cancelled before the thread took it (it is inside the hung encode of the one before): not encoded at all
	{
		const int before = gEncoded.load();
		gHang = 1;
		j = mkjob(1, 2, out); kvm_encthr_submit(&j, 0);
		usleep(10000);
		j = mkjob(1, 3, out); CHECK(kvm_encthr_submit(&j, 0) >= 0);
		CHECK_EQ(kvm_encthr_cancel_all(), 2);
		gHang = 0;
		CHECK(wait_ready(3000)); CHECK(kvm_encthr_collect(&jc, &r, &c) >= 0); CHECK_EQ(c, 1);
		CHECK(wait_ready(3000)); CHECK(kvm_encthr_collect(&jc, &r, &c) >= 0); CHECK_EQ(c, 1);
		CHECK_EQ(gEncoded.load() - before, 1);					// the first was inside the encoder; the second never was
	}
	// an error of the encoder comes back as a result
	gFailEncodeAt = gEncoded.load() + 1;
	j = mkjob(1, 3, out); kvm_encthr_submit(&j, 0);
	CHECK(wait_ready(3000)); kvm_encthr_collect(&jc, &r, &c);
	CHECK_EQ(r.code, MFH264_E_STREAM_BROKEN); CHECK_EQ(r.reason, MFH264_BROKEN_NO_OUTPUT); CHECK(r.stage != NULL); CHECK_EQ(r.phase, 0);
	kvm_encthr_stop(3000);
	// init that fails
	reset(); gFailInit = 1;
	CHECK_EQ(kvm_encthr_start(&gBe, KVM_ENCTHR_THREADED, 1, 3000, &err), MFH264_E_NOMF);
	CHECK(!kvm_encthr_started());
	CHECK(err.stage != NULL);
	kvm_encthr_stop(100);
}

static void test_watchdog(void)
{
	struct mfh264_err err;
	static uint8_t out[64];
	kvm_ejob j, jc;
	kvm_ejres r;
	int c;
	uint32_t now = 1000;
	reset();
	CHECK_EQ(kvm_encthr_start(&gBe, KVM_ENCTHR_THREADED, 1, 3000, &err), 0);
	kvm_encthr_call(open_fn, NULL, 3000);
	gHang = 1;
	j = mkjob(1, 1, out); kvm_encthr_submit(&j, now);
	usleep(10000);
	CHECK_EQ(kvm_encthr_oldest_age(now + 700), 700);
	CHECK(!wait_ready(30));											// a bounded wait: nothing comes
	CHECK_EQ(kvm_encthr_abandon(), 1);								// the watchdog gives up
	CHECK(kvm_encthr_abandoned());
	CHECK_EQ(kvm_encthr_submit(&j, now), -1);
	CHECK_EQ(kvm_encthr_call(close_fn, NULL, 50), -1);
	CHECK_EQ(kvm_encthr_collect(&jc, &r, &c), -1);
	gHang = 0;														// the hung encoder wakes up later: its result is dropped, nothing crashes
	usleep(30000);
	CHECK_EQ(kvm_encthr_collect(&jc, &r, &c), -1);
	kvm_encthr_stop(100);											// leaks the thread's state on purpose (the process ends)
	CHECK(!kvm_encthr_started());
	// a call that does not finish in time abandons the thread as well
	reset();
	CHECK_EQ(kvm_encthr_start(&gBe, KVM_ENCTHR_THREADED, 1, 3000, &err), 0);
	kvm_encthr_call(open_fn, NULL, 3000);
	gHang = 1;
	j = mkjob(1, 1, out); kvm_encthr_submit(&j, now);
	usleep(10000);
	{ static int v = 0; CHECK_EQ(kvm_encthr_call(order_fn, &v, 40), -1); }		// the argument of a call that is given up must outlive it (static, not the stack): the thread may still write it
	CHECK(kvm_encthr_abandoned());
	gHang = 0;
	usleep(30000);
	kvm_encthr_stop(100);
	{ static uint8_t out2[64]; // a buffer of its own: the abandoned threads may still write the old one (it is leaked in production)
	// a new encoder after an abandoned one (the software encoder after a hung hardware one)
	reset();
	CHECK_EQ(kvm_encthr_start(&gBe, KVM_ENCTHR_THREADED, 1, 3000, &err), 0);
	CHECK(!kvm_encthr_abandoned());
	kvm_encthr_call(open_fn, NULL, 3000);
	j = mkjob(2, 9, out2); CHECK_EQ(kvm_encthr_submit(&j, now), 0);
	CHECK(wait_ready(3000)); CHECK_EQ(kvm_encthr_collect(&jc, &r, &c), 0); CHECK_EQ(r.code, 0);
	kvm_encthr_stop(3000);
	}
}

static void test_inline_and_manual(void)
{
	struct mfh264_err err;
	static uint8_t out[2][64];
	kvm_ejob j, jc;
	kvm_ejres r;
	int c, v = 0;
	reset();
	CHECK_EQ(kvm_encthr_start(&gBe, KVM_ENCTHR_INLINE, 1, 100, &err), 0);
	CHECK(kvm_encthr_event() == NULL);
	CHECK_EQ(kvm_encthr_call(open_fn, NULL, 100), 0);
	j = mkjob(1, 1, out[0]);
	CHECK_EQ(kvm_encthr_submit(&j, 5), 0);
	CHECK(kvm_encthr_ready());										// made before submit returned: the old synchronous behaviour
	CHECK_EQ(kvm_encthr_collect(&jc, &r, &c), 0);
	CHECK_EQ(r.len, 6);
	kvm_encthr_stop(100);
	CHECK_EQ(gShutdowns, 1);
	// manual: nothing runs until the test says so
	reset();
	CHECK_EQ(kvm_encthr_start(&gBe, KVM_ENCTHR_MANUAL, 2, 100, &err), 0);
	kvm_encthr_call(open_fn, NULL, 100);
	j = mkjob(1, 1, out[0]); CHECK_EQ(kvm_encthr_submit(&j, 5), 0);
	j = mkjob(1, 2, out[1]); CHECK_EQ(kvm_encthr_submit(&j, 6), 1);
	CHECK(!kvm_encthr_ready());
	CHECK_EQ(kvm_encthr_inflight(), 2);
	CHECK_EQ(kvm_encthr_test_step(1), 1);
	CHECK(kvm_encthr_ready());
	CHECK_EQ(kvm_encthr_call(order_fn, &v, 100), 0);				// a call in manual mode first makes the waiting jobs
	CHECK_EQ(v, 2);
	CHECK_EQ(kvm_encthr_collect(&jc, &r, &c), 0); CHECK_EQ(out[0][4], 1);
	CHECK_EQ(kvm_encthr_collect(&jc, &r, &c), 1); CHECK_EQ(out[1][4], 2);
	kvm_encthr_stop(100);
}

// a hung backend is given up, then the thread starts with another backend (the software encoder after the hardware one hung); the late work of the old thread touches nothing of the new one
static pthread_t gCallThread;
static void whoami_fn(kvm_encctx *c, void *arg) { (void)c; (void)arg; gCallThread = pthread_self(); }
static void test_backend_swap(void)
{
	struct mfh264_err err;
	static uint8_t outA[64], outB[64];
	kvm_ejob j, jc;
	kvm_ejres r;
	int c;
	reset();
	CHECK_EQ(kvm_encthr_start(&gBeA, KVM_ENCTHR_THREADED, 3, 3000, &err), 0);
	CHECK(kvm_encthr_backend() == &gBeA);
	CHECK(kvm_encthr_backend()->caps & KVM_ENCBE_CAP_HW);
	kvm_encthr_call(open_fn, NULL, 3000);
	j = mkjob(1, 1, outA);
	kvm_encthr_submit(&j, 100);
	CHECK(wait_ready(3000)); kvm_encthr_collect(&jc, &r, &c);
	CHECK(r.backend != NULL && strcmp(r.backend, "fake A") == 0); CHECK_EQ(r.structural, 0);
	// a structural error (an exception) says so
	gFailExc = 1;
	j = mkjob(1, 2, outA); kvm_encthr_submit(&j, 100);
	CHECK(wait_ready(3000)); kvm_encthr_collect(&jc, &r, &c);
	CHECK_EQ(r.code, MFH264_E_EXCEPTION); CHECK_EQ(r.structural, 1); CHECK(strcmp(r.backend, "fake A") == 0);
	gFailExc = 0;
	// a lost access unit is about one frame
	gFailEncodeAt = gEncoded.load() + 1;
	j = mkjob(1, 3, outA); kvm_encthr_submit(&j, 100);
	CHECK(wait_ready(3000)); kvm_encthr_collect(&jc, &r, &c);
	CHECK_EQ(r.code, MFH264_E_STREAM_BROKEN); CHECK_EQ(r.structural, 0);
	// the hardware hangs: the watchdog gives the thread up
	gHang = 1;
	j = mkjob(1, 4, outA); kvm_encthr_submit(&j, 100);
	usleep(10000);
	CHECK_EQ(kvm_encthr_abandon(), 1);
	CHECK(kvm_encthr_abandoned());
	// the software backend starts on a thread of its own, at once, while the hung one is still stuck
	CHECK_EQ(kvm_encthr_start(&gBeB, KVM_ENCTHR_THREADED, 1, 3000, &err), 0);		// (the old state is left to the process)
	CHECK(!kvm_encthr_abandoned());
	CHECK(kvm_encthr_backend() == &gBeB);
	CHECK(kvm_encthr_backend()->caps & KVM_ENCBE_CAP_QP);
	kvm_encthr_call(open_fn, NULL, 3000);
	j = mkjob(2, 9, outB); CHECK_EQ(kvm_encthr_submit(&j, 200), 0);
	gHang = 0;													// the old thread wakes up now: it finishes into the dead state (outA) and the new one is not disturbed
	CHECK(wait_ready(3000)); CHECK_EQ(kvm_encthr_collect(&jc, &r, &c), 0);
	CHECK_EQ(r.code, 0); CHECK_EQ(outB[4], 9); CHECK(strcmp(r.backend, "fake B") == 0);
	usleep(30000);
	CHECK_EQ(kvm_encthr_inflight(), 0);
	kvm_encthr_stop(3000);
	CHECK(!kvm_encthr_started());
}

// a new start while one runs ends the old one (its encoder is closed and shut down on its own thread)
static void test_restart(void)
{
	struct mfh264_err err;
	reset();
	CHECK_EQ(kvm_encthr_start(&gBe, KVM_ENCTHR_THREADED, 1, 3000, &err), 0);
	kvm_encthr_call(open_fn, NULL, 3000);
	CHECK_EQ(gInits.load(), 1);
	CHECK_EQ(kvm_encthr_start(&gBeB, KVM_ENCTHR_THREADED, 1, 3000, &err), 0);
	CHECK_EQ(gInits.load(), 2);
	CHECK_EQ(gShutdowns.load(), 1);
	CHECK_EQ(gCloses.load(), 1);
	CHECK(kvm_encthr_backend() == &gBeB);
	kvm_encthr_stop(3000);
	CHECK_EQ(gShutdowns.load(), 2);
}

// any function runs on the thread of the encoder (not on the caller's): the enumeration of the hardware MFTs goes there
static void test_call_any_function(void)
{
	struct mfh264_err err;
	reset();
	CHECK_EQ(kvm_encthr_start(&gBe, KVM_ENCTHR_THREADED, 1, 3000, &err), 0);
	gCallThread = pthread_self();
	CHECK_EQ(kvm_encthr_call(whoami_fn, NULL, 3000), 0);
	CHECK(!pthread_equal(gCallThread, pthread_self()));						// not the caller
	{
		pthread_t first = gCallThread;
		CHECK_EQ(kvm_encthr_call(whoami_fn, NULL, 3000), 0);
		CHECK(pthread_equal(first, gCallThread));							// always the same one: the thread that called init
		CHECK_EQ(gBeThreadSet, true);
		CHECK(pthread_equal(first, gBeThread));
	}
	kvm_encthr_stop(3000);
}

// the stress: many short sessions
// , a "main loop" that submits, cancels, collects and calls at random, with random delays; all its waits are bounded
static unsigned gSeed;
static unsigned rnd(void) { gSeed = gSeed * 1103515245u + 12345u; return gSeed >> 16; }

static void test_stress(int sessions)
{
	int s;
	for (s = 0; s < sessions; s++)
	{
		struct mfh264_err err;
		static uint8_t out[3][64];
		int depth = 1 + (s % 3), step, submitted = 0, collected = 0, done = 0;
		reset();
		gSeed = (unsigned)s * 2654435761u + 7u;
		CHECK_EQ(kvm_encthr_start(&gBe, KVM_ENCTHR_THREADED, depth, 3000, &err), 0);
		kvm_encthr_call(open_fn, NULL, 3000);
		for (step = 0; step < 60 && !done; step++)
		{
			const unsigned op = rnd() % 8;
			gDelayUs = (int)(rnd() % 3) * 100;
			if (op <= 2)
			{
				kvm_ejob j;
				int slot = kvm_encthr_free_slot() >= 0 ? 1 : 0;
				if (slot)
				{
					j = mkjob(1, step + 1, out[step % 3]);
					CHECK(kvm_encthr_submit(&j, (uint32_t)step) >= 0);
					submitted++;
				}
			}
			else if (op <= 4)
			{
				kvm_ejob jc; kvm_ejres r; int c;
				(void)kvm_encthr_wait(rnd() % 3);
				if (kvm_encthr_collect(&jc, &r, &c) >= 0) { collected++; CHECK(c == 0 || c == 1); }
			}
			else if (op == 5) { (void)kvm_encthr_cancel_all(); }
			else if (op == 6) { int v; CHECK_EQ(kvm_encthr_call(order_fn, &v, 3000), 0); }
			else
			{
				// a quiesce like the barrier: wait for every job, bounded
				int k;
				for (k = 0; k < 200 && kvm_encthr_inflight() > 0; k++)
				{
					kvm_ejob jc; kvm_ejres r; int c;
					if (kvm_encthr_collect(&jc, &r, &c) >= 0) { collected++; } else { (void)kvm_encthr_wait(5); }
				}
				CHECK_EQ(kvm_encthr_inflight(), 0);
			}
		}
		// the end of the session with jobs in flight: cancel, close, stop
		(void)kvm_encthr_cancel_all();
		CHECK_EQ(kvm_encthr_call(close_fn, NULL, 3000), 0);
		kvm_encthr_stop(3000);
		CHECK(!kvm_encthr_started());
		CHECK_EQ(gWrongThread, 0);
		CHECK(collected <= submitted);
		CHECK_EQ(gInits, 1); CHECK_EQ(gShutdowns, 1); CHECK_EQ(gOpens, gCloses);
	}
}


// a job through the view of the thread: the map is a copy in the job, the view is the thread's, the encoder is given the view (NV12), not the picture
static void paint32(uint8_t *p, int w, int h, int seed) { int x, y; for (y = 0; y < h; y++) { for (x = 0; x < w; x++) { uint8_t *q = p + ((size_t)y * w + x) * 4; q[0] = (uint8_t)(seed * 41 + x * 3); q[1] = (uint8_t)(seed * 17 + y * 5); q[2] = (uint8_t)(seed * 7 + x + y); q[3] = 255; } } }
static void test_view(void)
{
	struct mfh264_err err;
	static uint8_t out[8][64], pa[32 * 32 * 4], pb[32 * 32 * 4], pc[32 * 32 * 4], p64[64 * 64 * 4], refA[32 * 32 * 3 / 2], refB[32 * 32 * 3 / 2], refC[32 * 32 * 3 / 2];
	kvm_ejob j, jc;
	kvm_ejres r;
	int c;
	static uint8_t map1[4] = { 1, 0, 0, 0 }, map3[4] = { 0, 0, 0, 1 }, mapBad[12] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
	reset(); gNv12Calls = 0;
	paint32(pa, 32, 32, 1); paint32(pb, 32, 32, 2); paint32(pc, 32, 32, 3);
	CHECK_EQ(kvm_yuv_bgra_to_nv12(pa, 32, 32, 128, refA, 32, 32, KVM_YUV_SCALAR), 0);
	CHECK_EQ(kvm_yuv_bgra_to_nv12(pb, 32, 32, 128, refB, 32, 32, KVM_YUV_SCALAR), 0);
	CHECK_EQ(kvm_yuv_bgra_to_nv12(pc, 32, 32, 128, refC, 32, 32, KVM_YUV_SCALAR), 0);
	CHECK_EQ(kvm_encthr_start(&gBe, KVM_ENCTHR_THREADED, 1, 3000, &err), 0);
	CHECK_EQ(kvm_encthr_call(open_fn, NULL, 3000), 0);
	/* the first frame: complete */
	memset(&j, 0, sizeof(j));
	j.gen = 1; j.top = pa; j.width = 32; j.height = 32; j.stride = 128; j.ts = 1; j.out = out[0]; j.outCap = 64; j.viewMode = 1; j.viewFull = 1; j.mbCols = 2; j.mbRows = 2;
	CHECK_EQ(kvm_encthr_submit(&j, 1000), 0);
	CHECK(wait_ready(3000)); CHECK_EQ(kvm_encthr_collect(&jc, &r, &c), 0);
	CHECK_EQ(r.code, 0); CHECK_EQ(r.convMbs, 4u); CHECK_EQ(gNv12Calls.load(), 1); CHECK_EQ(gEncoded.load(), 0);
	{ std::lock_guard<std::mutex> lk(gViewMu); CHECK(gViewSeen.size() == sizeof(refA) && memcmp(&gViewSeen[0], refA, sizeof(refA)) == 0); }
	/* the next: one macroblock of the map (a copy: the caller may change its own map at once) */
	memset(&j, 0, sizeof(j));
	j.gen = 1; j.top = pb; j.width = 32; j.height = 32; j.stride = 128; j.ts = 2; j.out = out[1]; j.outCap = 64; j.viewMode = 1; j.mbCols = 2; j.mbRows = 2; j.viewMap = map1;
	CHECK_EQ(kvm_encthr_submit(&j, 1000), 0);
	map1[0] = 0; map1[3] = 1;			/* the map is copied by the submit: a change after it is not seen */
	CHECK(wait_ready(3000)); CHECK_EQ(kvm_encthr_collect(&jc, &r, &c), 0);
	CHECK(jc.viewMap == NULL);		/* the copy of the map stays in the slot */
	CHECK_EQ(r.code, 0); CHECK_EQ(r.convMbs, 1u);
	{
		std::lock_guard<std::mutex> lk(gViewMu);
		CHECK(gViewSeen[0] == refB[0] && gViewSeen[15 * 32 + 15] == refB[15 * 32 + 15]);			/* macroblock (0,0): the new picture */
		CHECK(gViewSeen[16 * 32 + 16] == refA[16 * 32 + 16] && gViewSeen[31 * 32 + 31] == refA[31 * 32 + 31]);	/* the others: as they were (frozen) */
	}
	/* a flat frame: the view is one colour outside the map */
	memset(&j, 0, sizeof(j));
	j.gen = 1; j.top = pc; j.width = 32; j.height = 32; j.stride = 128; j.ts = 3; j.out = out[2]; j.outCap = 64; j.viewMode = 1; j.viewFlat = 1; j.mbCols = 2; j.mbRows = 2; j.viewMap = map3;
	CHECK_EQ(kvm_encthr_submit(&j, 1000), 0);
	CHECK(wait_ready(3000)); CHECK_EQ(kvm_encthr_collect(&jc, &r, &c), 0);
	CHECK_EQ(r.code, 0); CHECK_EQ(r.convMbs, 1u);
	{
		std::lock_guard<std::mutex> lk(gViewMu);
		CHECK(gViewSeen[16 * 32 + 16] == refC[16 * 32 + 16] && gViewSeen[31 * 32 + 31] == refC[31 * 32 + 31]);
		CHECK(gViewSeen[0] == gViewSeen[15 * 32 + 15] && gViewSeen[0] == gViewSeen[16] && gViewSeen[0] == gViewSeen[16 * 32]);
	}
	/* another size: a new view (the plan of the loop made the frame complete) */
	paint32(p64, 64, 64, 4);
	memset(&j, 0, sizeof(j));
	j.gen = 1; j.top = p64; j.width = 64; j.height = 64; j.stride = 256; j.ts = 4; j.out = out[3]; j.outCap = 64; j.viewMode = 1; j.viewFull = 1; j.mbCols = 4; j.mbRows = 4;
	CHECK_EQ(kvm_encthr_submit(&j, 1000), 0);
	CHECK(wait_ready(3000)); CHECK_EQ(kvm_encthr_collect(&jc, &r, &c), 0);
	CHECK_EQ(r.code, 0); CHECK_EQ(r.convMbs, 16u);
	{ std::lock_guard<std::mutex> lk(gViewMu); CHECK_EQ(gViewSeen.size(), 64u * 64u * 3u / 2u); }
	/* a map of another grid is refused (an error of the frame, not a crash) */
	memset(&j, 0, sizeof(j));
	j.gen = 1; j.top = p64; j.width = 64; j.height = 64; j.stride = 256; j.ts = 5; j.out = out[4]; j.outCap = 64; j.viewMode = 1; j.mbCols = 3; j.mbRows = 4; j.viewMap = mapBad;
	CHECK_EQ(kvm_encthr_submit(&j, 1000), 0);
	CHECK(wait_ready(3000)); CHECK_EQ(kvm_encthr_collect(&jc, &r, &c), 0);
	CHECK_EQ(r.code, MFH264_E_ARG);
	/* viewMode 0: the picture as it is */
	memset(&j, 0, sizeof(j));
	j.gen = 1; j.top = pa; j.width = 32; j.height = 32; j.stride = 128; j.ts = 6; j.out = out[5]; j.outCap = 64;
	CHECK_EQ(kvm_encthr_submit(&j, 1000), 0);
	CHECK(wait_ready(3000)); CHECK_EQ(kvm_encthr_collect(&jc, &r, &c), 0);
	CHECK_EQ(r.code, 0); CHECK_EQ(r.convMbs, 0u); CHECK_EQ(gEncoded.load(), 1);
	CHECK_EQ(kvm_encthr_call(close_fn, NULL, 3000), 0);
	kvm_encthr_stop(3000);
	CHECK_EQ(gWrongThread, 0);
}

int main(int argc, char **argv)
{
	const int sessions = argc > 1 ? atoi(argv[1]) : 300;
	test_threaded_basic();
	test_stale_event();
	test_view();
	test_cancel_and_errors();
	test_watchdog();
	test_inline_and_manual();
	test_backend_swap();
	test_call_any_function();
	test_restart();
	test_stress(sessions);
	printf("%d checks, %d failed\n", gChecks, gFail);
	return gFail ? 1 : 0;
}
