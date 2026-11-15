// the scenarios of the hardware encoder in the harness (included by kvm_video_harness_test.cpp, which has the fakes and the helpers). What is real here: kvm_video.cpp, kvm_encthr.cpp (the manual mode), kvm_hwpolicy.c,
// the rate control, the stream layer. What is fake: the two encoder backends (the software one is kvm_encbe_mf over the fake mfh264_*, the hardware one is kvm_encbe_mfhw below, with the faults that the scenarios
// switch on), the self test process (kvm_hwtest_start / poll: the result is what the scenario says, after so many polls), the cache file (in memory), the mode file, the environment, the capture mode, the desktop.

#include "kvm_hwtest.h"
#include "kvm_encbe_mfhw.h"

enum { HW_OK = 0, HW_HANG = 1, HW_EXCEPTION = 2, HW_BROKEN = 3, HW_NOQP = 4 };
static int gHwPresent = 0, gHwMode = KVM_HWP_MODE_AUTO, gHwEnv = KVM_HWP_ENV_AUTO, gHwCaptureDxgi = 1, gHwPidAlive = 0;
static std::atomic<int> gHwFault(HW_OK);
static int gHwFaultAfter = 0;							// the fault starts with this frame of the hardware encoder (0 = the first)
static int gHwPeriodic = 0;							// the fake hardware encoder makes a key frame of its own every N frames (a GOP that is finite)
static int gHwAllowRate = 0;							// the fake hardware encoder takes the bit rate (CBR / LDVBR with no QP form)
static int gHwOpenRc = 0, gHwOpens = 0, gHwCloses = 0, gHwInits = 0, gHwShutdowns = 0, gHwEncodes = 0, gHwForces = 0, gHwLastForm = 0, gHwTryAtOpen = -1, gHwSetQpRc = 0;
static int64_t gHwLastLuid = 0;
static std::vector<int> gHwQps;
static kvm_hwp_cache gHwCache;
static int gHwCacheWrites = 0, gHwCacheFail = 0;
static int gTestStarts = 0, gTestAborts = 0, gTestWait = 0, gTestVerdict = KVM_HWP_V_PASS, gTestForm = MFH264_QP_ENCODEQP_UI4, gTestKeyWrong = 0;
static unsigned gTestStartW = 0, gTestStartH = 0; static int gTestStartLevel = 0; static int64_t gTestStartLuid = 0;
static char gTestKey[KVM_HWP_KEY_MAX];
static unsigned gTerminated = 0; static int gTerminateCalls = 0;
static char gDesktopName[32] = "Default";
static std::atomic<int> gHwDead(0);

extern "C"
{
HDESK GetThreadDesktop(DWORD tid) { (void)tid; return (HDESK)1; }
DWORD GetCurrentThreadId(void) { return 1; }
BOOL GetUserObjectInformationA(HDESK d, int index, void *info, DWORD len, DWORD *needed) { (void)d; (void)index; (void)needed; if (len < strlen(gDesktopName) + 1) { return 0; } strcpy((char *)info, gDesktopName); return 1; }
DWORD GetCurrentProcessId(void) { return 4242; }
HANDLE GetCurrentProcess(void) { return (HANDLE)1; }
BOOL TerminateProcess(HANDLE p, unsigned code) { (void)p; gTerminated = code; gTerminateCalls++; return 1; }
long long kvm_dxgi_adapter_luid(void) { return 0x1234500000001LL; }
int kvm_capture_is_dxgi(void) { return gHwCaptureDxgi; }
static int gHwCapUnknown = 0;
int kvm_capture_state(void) { return gHwCapUnknown ? 0 : (gHwCaptureDxgi ? 2 : 1); }	// the fake capture: unknown (before the first open of DXGI), GDI, DXGI

// the files and the process of kvm_hwtest.cpp
static int gHwCacheReads = 0;
int kvm_hwcache_read(kvm_hwp_cache *c) { gHwCacheReads++; *c = gHwCache; return 0; }
int kvm_hwcache_update(kvm_hwcache_fn fn, void *arg)
{
	kvm_hwp_cache c = gHwCache;
	if (gHwCacheFail) { return -1; }
	if (fn(&c, arg)) { gHwCache = c; gHwCacheWrites++; }
	return 0;
}
int kvm_hwmode_read(void) { return gHwMode; }
int kvm_hwenv_read(void) { return gHwEnv; }
int kvm_hw_pid_alive(uint32_t pid) { (void)pid; return gHwPidAlive; }
int kvm_hwtest_token_class(void) { return KVM_HWTEST_TOKEN_SYSTEM_IN_SESSION; }
static int gSelCalls = 0, gSelHang = 0, gSigOk = 1;
int kvm_hwtest_select(int64_t luid, kvm_mfhw_sel *sel)
{
	gSelCalls++;
	memset(sel, 0, sizeof(*sel));
	if (gSelHang) { return -1; }										// the enumeration did not return in time (the real one waits on a thread of its own)
	if (!gHwPresent) { return 0; }
	sel->found = 1; snprintf(sel->name, sizeof(sel->name), "FakeHW"); sel->vendor = 0x1002; sel->device = 0x164c; snprintf(sel->driver, sizeof(sel->driver), "31.0.12018.5"); sel->luid = luid; sel->nHw = 1;
	return 1;
}
int kvm_hwtest_key_from_sel(const kvm_mfhw_sel *sel, unsigned codedW, unsigned codedH, char *key, size_t cap)
{
	if (sel == NULL || !sel->found) { if (cap > 0) { key[0] = 0; } return 0; }
	snprintf(key, cap, "rev:%d,mft:FakeHW,sz:%d", KVM_HW_SELFTEST_REV, kvm_hwp_size_class(codedW, codedH));
	return 1;
}
int kvm_hwtest_select_key(unsigned vendor, unsigned device, unsigned subsys, unsigned adapterRev, char *key, size_t cap)
{
	snprintf(key, cap, "sel,rev:%d,ven:%04x,dev:%04x,sub:%08x,arev:%02x", KVM_HW_SELFTEST_REV, vendor, device, subsys, adapterRev);
	return 1;
}
int kvm_dxgi_adapter_sig(unsigned *ven, unsigned *dev, unsigned *sub, unsigned *rev) { if (!gSigOk) { return 0; } *ven = 0x1002; *dev = 0x164c; *sub = 0x1234; *rev = 0xC1; return 1; }
int kvm_hwtest_start(kvm_hwtest_run *r, unsigned codedW, unsigned codedH, int level, int maxLevel, int profile, int64_t luid, const char *key)
{
	(void)maxLevel; (void)profile;
	memset(r, 0, sizeof(*r));
	r->running = 1;
	gTestStarts++; gTestStartW = codedW; gTestStartH = codedH; gTestStartLevel = level; gTestStartLuid = luid;
	snprintf(gTestKey, sizeof(gTestKey), "%s", key);
	return 0;
}
int kvm_hwtest_poll(kvm_hwtest_run *r, int *verdict, kvm_hwp_result *res)
{
	if (!r->running) { return 0; }
	if (gTestWait > 0) { gTestWait--; return 0; }
	r->running = 0;
	memset(res, 0, sizeof(*res));
	res->valid = 1; res->rev = KVM_HW_SELFTEST_REV; res->verdict = gTestVerdict; res->qpForm = gTestForm; res->openMs = 150; res->p50Us = 8000; res->p95Us = 9500; res->idrKb = 400; res->frames = 20; res->flatMilli = 600;
	snprintf(res->key, sizeof(res->key), "%s", gTestKeyWrong ? "rev:1,mft:Other" : gTestKey);
	if (gTestVerdict == KVM_HWP_V_FAIL) { snprintf(res->rule, sizeof(res->rule), "refs"); }
	*verdict = gTestVerdict;
	return 1;
}
void kvm_hwtest_abort(kvm_hwtest_run *r) { if (r->running) { gTestAborts++; } r->running = 0; }
}

// the hardware backend: the fake encoder behind another table, with the faults of the scenario
static int fh_init(struct mfh264_err *e) { (void)e; gHwInits++; return 0; }
static void fh_shutdown(void) { gHwShutdowns++; }
static int fh_faulted(void) { return gHwFault == HW_EXCEPTION ? 1 : 0; }
static struct mfh264 *fh_open(const struct mfh264_cfg *cfg, struct mfh264_err *err)
{
	struct mfh264 *h;
	gHwOpens++; gHwLastForm = cfg->qp_method; gHwLastLuid = cfg->hw_luid; gHwTryAtOpen = gHwCache.trying;
	memset(err, 0, sizeof(*err));
	if (gHwAllowRate && (cfg->rc == MFH264_RC_CBR || cfg->rc == MFH264_RC_LDVBR) && cfg->qp_method == MFH264_QP_NONE) { /* the hardware self test */ }
	else if (cfg->rc != MFH264_RC_QUALITY || (cfg->qp_method != MFH264_QP_ENCODEQP_UI4 && cfg->qp_method != MFH264_QP_FRAMETYPEQP && cfg->qp_method != MFH264_QP_QUALITY)) { err->code = MFH264_E_ARG; err->stage = "fake hw: form"; return NULL; }
	if (gHwOpenRc != 0) { err->code = gHwOpenRc; err->stage = "fake hw open"; if (gHwOpenRc == MFH264_E_EXCEPTION) { err->exc_code = 0xC0000005ul; } return NULL; }
	h = mfh264_open(cfg, err);
	gHwEncodes = 0;
	return h;
}
static void fh_close(struct mfh264 *h) { gHwCloses++; mfh264_close(h); }
static int fh_set_qp(struct mfh264 *h, int qp, struct mfh264_err *err)
{
	memset(err, 0, sizeof(*err));
	if (gHwSetQpRc != 0) { err->code = gHwSetQpRc; err->stage = "fake hw set qp"; return gHwSetQpRc; }
	gHwQps.push_back(qp);
	return mfh264_set_qp(h, qp, err);
}
static int fh_set_bitrate(struct mfh264 *h, int b, struct mfh264_err *e) { return mfh264_set_bitrate(h, b, e); }
static int fh_set_sample_qp(struct mfh264 *h, int m, int q) { (void)h; (void)q; return m == 0 ? 0 : MFH264_E_ARG; }
static int fh_last_qp(const struct mfh264 *h, uint64_t *raw) { (void)h; if (raw != NULL) { *raw = 0; } return 0; }	// (the AMD encoder does not report the QP)
static int fh_force(struct mfh264 *h, struct mfh264_err *e) { gHwForces++; return mfh264_force_idr(h, e); }
static int fh_encode(struct mfh264 *h, const uint8_t *bgra, int w, int hh, int stride, int64_t ts, uint8_t *out, size_t cap, size_t *len, int *idr, struct mfh264_err *err)
{
	const int n = gHwEncodes++;
	memset(err, 0, sizeof(*err));
	*len = 0; *idr = 0;
	if (gHwFault != HW_OK && n >= gHwFaultAfter)
	{
		if (gHwFault == HW_HANG) { err->code = MFH264_E_HANG; err->stage = "fake hw: no HaveOutput"; gHwDead = 1; return MFH264_E_HANG; }
		if (gHwFault == HW_EXCEPTION) { err->code = MFH264_E_EXCEPTION; err->exc_code = 0xC0000005ul; err->stage = "fake hw exception"; return MFH264_E_EXCEPTION; }
		if (gHwFault == HW_BROKEN) { err->code = MFH264_E_STREAM_BROKEN; err->reason = MFH264_BROKEN_ERROR; err->stage = "fake hw: ProcessInput refused"; return MFH264_E_STREAM_BROKEN; }
	}
	if (gHwPeriodic > 0 && n > 0 && n % gHwPeriodic == 0) { h->idrNext = 1; }
	return mfh264_encode_bgra(h, bgra, w, hh, stride, ts, out, cap, len, idr, err);
}
static int fh_encode_nv12(struct mfh264 *h, const uint8_t *nv12, size_t nv12len, int64_t ts, uint8_t *out, size_t cap, size_t *len, int *idr, struct mfh264_err *err)
{
	const int n = gHwEncodes++;
	memset(err, 0, sizeof(*err));
	*len = 0; *idr = 0;
	if (gHwFault != HW_OK && n >= gHwFaultAfter)
	{
		if (gHwFault == HW_HANG) { err->code = MFH264_E_HANG; err->stage = "fake hw: no HaveOutput"; gHwDead = 1; return MFH264_E_HANG; }
		if (gHwFault == HW_EXCEPTION) { err->code = MFH264_E_EXCEPTION; err->exc_code = 0xC0000005ul; err->stage = "fake hw exception"; return MFH264_E_EXCEPTION; }
		if (gHwFault == HW_BROKEN) { err->code = MFH264_E_STREAM_BROKEN; err->reason = MFH264_BROKEN_ERROR; err->stage = "fake hw: ProcessInput refused"; return MFH264_E_STREAM_BROKEN; }
	}
	if (gHwPeriodic > 0 && n > 0 && n % gHwPeriodic == 0) { h->idrNext = 1; }
	return mfh264_encode(h, nv12, nv12len, ts, out, cap, len, idr, err);
}
static void fh_info(const struct mfh264 *h, struct mfh264_info *info) { mfh264_get_info(h, info); info->backend = 1; info->max_inflight = 1; snprintf(info->hw_name, sizeof(info->hw_name), "FakeHW"); info->sps_level_idc = 40; }	// (the sequence header of the AMD encoder says 42, its IDR 40: what is announced is the computed level either way)
static size_t fh_max_au(const struct mfh264 *h) { return mfh264_max_au_size(h); }
static size_t fh_seq(const struct mfh264 *h, const uint8_t **p) { return mfh264_seq_header(h, p); }

extern "C" const kvm_encbe kvm_encbe_mfhw = { "hardware MFT", KVM_ENCBE_CAP_HW | KVM_ENCBE_CAP_QP, fh_init, fh_shutdown, fh_faulted, fh_open, fh_close, fh_set_qp, fh_set_bitrate, fh_set_sample_qp, fh_last_qp, fh_force, fh_encode, fh_encode_nv12, fh_info, fh_max_au, fh_seq };

static void hw_fakes_reset(void)
{
	gHwPeriodic = 0; gHwAllowRate = 0; gHwPresent = 0; gHwMode = KVM_HWP_MODE_AUTO; gHwEnv = KVM_HWP_ENV_AUTO; gHwCaptureDxgi = 1; gHwCapUnknown = 0; gHwPidAlive = 0;
	gHwFault = HW_OK; gHwFaultAfter = 0; gHwOpenRc = 0; gHwOpens = gHwCloses = gHwInits = gHwShutdowns = gHwEncodes = gHwForces = gHwLastForm = 0; gHwTryAtOpen = -1; gHwSetQpRc = 0; gHwLastLuid = 0;
	gHwQps.clear(); memset(&gHwCache, 0, sizeof(gHwCache)); gHwCacheWrites = 0; gHwCacheFail = 0;
	gTestStarts = gTestAborts = gTestWait = 0; gTestVerdict = KVM_HWP_V_PASS; gTestForm = MFH264_QP_ENCODEQP_UI4; gTestKeyWrong = 0; gTestStartW = gTestStartH = 0; gTestStartLevel = 0; gTestStartLuid = 0; gTestKey[0] = 0;
	gSelCalls = 0; gSelHang = 0; gSigOk = 1; gTerminated = 0; gTerminateCalls = 0; snprintf(gDesktopName, sizeof(gDesktopName), "Default"); gHwDead = 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------------

// the keys of the cache have the revision of the self test in them (KVM_HW_SELFTEST_REV): the tests follow it
#define HW_REV_S_(x) #x
#define HW_REV_S(x) HW_REV_S_(x)
static const char *HW_KEY_1088 = "rev:" HW_REV_S(KVM_HW_SELFTEST_REV) ",mft:FakeHW,sz:1";
static int gHwCrc = 7000;
// n frames, 40 ms apart, each with a new rectangle of motion: how many were written
static int hw_frames(int n)
{
	int wrote = 0;
	for (int i = 0; i < n; i++) { gTestNowMs += 40; move_rect(5, 10, 8, 20, ++gHwCrc); if (emit() == 1) { wrote++; } }
	return wrote;
}
static void hw_seed_pass(const char *key = HW_KEY_1088)
{
	kvm_hwp_note_test(&gHwCache, key, KVM_HWP_V_PASS, MFH264_QP_ENCODEQP_UI4, 150, 8000, 9500, 400, (uint32_t)time(NULL));
}
static int last_gen(void) { return (int)last_state().gen; }
static bool qps_in(const std::vector<int> &v, int lo, int hi) { for (size_t i = 0; i < v.size(); i++) { if (v[i] < lo || v[i] > hi) { return false; } } return !v.empty(); }

// no hardware encoder on the machine (a VM): the software one, no test, a reason in the log
static void test_hw_none(void)
{
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(kvm_video_on());
	CHECK(log_has("video backend be=sw why=no_hw"));
	CHECK_EQ(gTestStarts, 0); CHECK_EQ(gHwInits, 0); CHECK_EQ(gHwOpens, 0);
	CHECK(hw_frames(3) >= 2);
	CHECK(log_has("be=sw"));
	end_session();
	CHECK_EQ(gHwCacheWrites, 0);
}

// an encoder that passed the test before: the hardware one from the first generation, no test; the QP the encoder is given is the mapped one, the regulator keeps its own scale
static void test_hw_cached(void)
{
	begin_session(1920, 1080);
	settle_all();
	gHwPresent = 1; hw_seed_pass();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(kvm_video_on());
	CHECK(log_has("video backend be=hw why=cached"));
	CHECK_EQ(gTestStarts, 0); CHECK_EQ(gHwInits, 1); CHECK_EQ(gHwOpens, 1); CHECK_EQ(gHwLastForm, MFH264_QP_ENCODEQP_UI4); CHECK_EQ(gHwLastLuid, 0x1234500000001LL);
	CHECK_EQ(gHwTryAtOpen, 1);											// the marker of a try was written before the encoder was opened ...
	CHECK_EQ(gHwCache.trying, 1);
	{
		kvm_state_msg m = last_state();
		CHECK_EQ(m.video, 1); CHECK_EQ(m.profile, 100); CHECK_EQ(m.level, 42);	// the level that was announced is the computed one (the fake says 42 in its info; the real hardware encoder says 42 in the sequence header and 40 in its IDR)
	}
	CHECK(hw_frames(3) >= 2);
	CHECK_EQ(gHwCache.trying, 1); CHECK_EQ(gHwCache.tryUsed, 1);		// ... and "in use" after the first access unit that was written (it stays: a crash of the driver later is counted by the parent)
	CHECK(qps_in(gHwQps, 26, 34));
	CHECK_EQ(gHwQps.empty() ? 0 : gHwQps[0], kvm_vcore_hw_qp(38));	// the first QP is that of a key frame (the cautious rule at the unknown link: 38, the ceiling of the regulator): the encoder is given 34 ...
	CHECK(gHwQps.size() >= 2 && gHwQps[1] == 26);						// ... and the P frames of the regulator's floor (30 for this encoder) are given 26: what the regulator remembers is its own scale
	hw_frames(8);
	CHECK(gHwQps.size() <= 3);											// the QP is set when the regulator's QP changes, not at every frame (the mapped QP is not compared with the regulator's)
	CHECK_EQ(gOpens, 1);												// the software encoder was made once, for nothing: no. (gOpens counts every fake open: the hardware one is the only one)
	{
		std::vector<frame_info> fr = frames();
		CHECK(fr.size() >= 2 && fr[0].ok && (fr[0].f.flags & KVM_FRAME_IDR) != 0);
	}
	CHECK(log_has("be=hw"));
	CHECK(log_has("video idr reason=first frame_n=1 qp=38"));			// what the rate control remembers of the key frame is the regulator's QP (38), not the 34 that the encoder was given
	gTestNowMs += 1500; kvm_video_poll();
	CHECK(log_has(" qp=30 qp_idr="));									// the regulator's floor for the hardware encoder is 30
	end_session();
	CHECK_EQ(gHwCloses, 1); CHECK_EQ(gHwShutdowns, 1);
	CHECK_EQ(gHwCache.trying, 0);
}

// no result yet: the software encoder runs, the test is started in the background, the hardware encoder comes with a new generation when the test has passed and the video is quiet
static void test_hw_test_flow(void)
{
	begin_session(1920, 1080);
	settle_all();
	gHwPresent = 1; gTestWait = 3;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(kvm_video_on());
	CHECK(log_has("video backend be=sw why=test_started"));
	CHECK_EQ(gTestStarts, 1); CHECK_EQ(gTestStartW, 1920u); CHECK_EQ(gTestStartH, 1088u); CHECK_EQ(gTestStartLevel, 42); CHECK_EQ(gTestStartLuid, 0x1234500000001LL);
	CHECK_EQ(gHwInits, 0); CHECK_EQ(gHwOpens, 0);						// the software encoder runs meanwhile
	CHECK_EQ(last_gen(), 1);
	CHECK(hw_frames(3) >= 2);
	CHECK_EQ(gHwOpens, 0);
	// the test passes while the video moves: nothing happens until it is quiet for a second
	for (int i = 0; i < 4; i++) { kvm_video_poll(); }
	CHECK_EQ(gHwCache.n, 1); CHECK_EQ(gHwCache.e[0].verdict, KVM_HWP_V_PASS); CHECK_EQ(gHwCache.e[0].qpForm, MFH264_QP_ENCODEQP_UI4); CHECK(log_has("video hw self test verdict=1"));
	CHECK_EQ(gHwCache.e[0].flatMilli, 600u);								// the floor of a flat key frame that the test measured is in the entry
	CHECK_EQ(last_gen(), 1);
	gTestNowMs += 500; kvm_video_poll(); CHECK_EQ(last_gen(), 1);		// not yet: the last frame was written less than a second ago
	gTestNowMs += 700; kvm_video_poll(); CHECK_EQ(last_gen(), 1);		// (quiet, but the encoder was opened less than 2 s ago: the limit of the encoders holds for this switch too)
	gTestNowMs += 1500; kvm_video_poll();
	CHECK_EQ(last_gen(), 2);											// a new generation (242 before its first frame), on the hardware encoder
	CHECK(log_has("video hw switch"));
	CHECK_EQ(gHwInits, 1); CHECK_EQ(gHwOpens, 1); CHECK_EQ(gCloses, 1);	// the software encoder was closed
	CHECK_EQ(kvm_video_test_rc_flat_milli(), 600u);						// the rate control forecasts the key frames of the hardware encoder with the floor that its self test measured
	CHECK(log_has("video backend be=hw why=cached"));
	CHECK(hw_frames(3) >= 2);
	{
		std::vector<frame_info> fr = frames();
		int idrAfter = 0;
		for (size_t i = 0; i < fr.size(); i++) { if (fr[i].ok && fr[i].f.gen == 2 && (fr[i].f.flags & KVM_FRAME_IDR) != 0) { idrAfter++; } }
		CHECK_EQ(idrAfter, 1);											// the first frame of the new generation is an IDR
	}
	CHECK(qps_in(gHwQps, 26, 34));
	end_session();
	CHECK_EQ(gHwCloses, 1);
	CHECK_EQ(gTestAborts, 0);
}

// the test does not pass, or is not ours, or the session ends first
static void test_hw_test_results(void)
{
	// FAIL: the verdict is cached, the software encoder stays
	begin_session(1920, 1080);
	settle_all();
	gHwPresent = 1; gTestVerdict = KVM_HWP_V_FAIL;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(hw_frames(2) >= 1);
	kvm_video_poll(); kvm_video_poll();
	CHECK_EQ(gHwCache.n, 1); CHECK_EQ(gHwCache.e[0].verdict, KVM_HWP_V_FAIL); CHECK_EQ(gHwCache.e[0].attempts, 1);
	gTestNowMs += 3000; kvm_video_poll();
	CHECK_EQ(last_gen(), 1); CHECK_EQ(gHwOpens, 0);
	end_session();
	// the key of the result is not the one that the child worked out: dropped, nothing written
	begin_session(1920, 1080);
	settle_all();
	gHwPresent = 1; gTestKeyWrong = 1;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	kvm_video_poll(); kvm_video_poll();
	CHECK_EQ(gHwCache.n, 0); CHECK(log_has("result dropped"));
	gTestNowMs += 3000; kvm_video_poll(); CHECK_EQ(last_gen(), 1);
	end_session();
	// the session ends while the test runs: the test is aborted, no verdict
	begin_session(1920, 1080);
	settle_all();
	gHwPresent = 1; gTestWait = 1000;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(gTestStarts, 1);
	end_session();
	CHECK_EQ(gTestAborts, 1); CHECK_EQ(gHwCache.n, 0);
	// SLOW: software, tried again after a day (the cache says so, the decision is kvm_hwpolicy's)
	begin_session(1920, 1080);
	settle_all();
	gHwPresent = 1; gTestVerdict = KVM_HWP_V_SLOW;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	kvm_video_poll(); kvm_video_poll();
	CHECK_EQ(gHwCache.e[0].verdict, KVM_HWP_V_SLOW);
	gTestNowMs += 3000; kvm_video_poll(); CHECK_EQ(last_gen(), 1);
	end_session();
}

// the switches: the environment, the mode file (read for every generation), the capture mode, the desktop, the size, the inline mode
static void test_hw_switches(void)
{
	// MESH_KVM_VIDEO_HW=0
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass(); gHwEnv = KVM_HWP_ENV_OFF;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(log_has("why=env_off")); CHECK_EQ(gHwOpens, 0); CHECK_EQ(gTestStarts, 0);
	end_session();
	// the mode file says sw
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass(); gHwMode = KVM_HWP_MODE_SW;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(log_has("why=mode_sw")); CHECK_EQ(gHwOpens, 0); CHECK_EQ(gTestStarts, 0);
	end_session();
	// the mode file is read for every generation: sw written while the hardware encoder runs takes effect with the next generation (a new size here)
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(gHwOpens, 1); CHECK(hw_frames(2) >= 1);
	gHwMode = KVM_HWP_MODE_SW;
	CHECK_EQ(gHwOpens, 1);
	send_ctrl(KVM_OP_VIDEO_ON, 7);										// (op 1 again: the same size reuses the encoder: no new generation, no change)
	set_geometry(1280, 720);
	gHwCache.n = 0; gHwPresent = 1; hw_seed_pass(HW_KEY_1088);
	gTestNowMs += 3000; kvm_video_poll();
	CHECK(log_has("why=mode_sw"));
	CHECK_EQ(gHwCache.trying, 0);										// the software encoder took over in an orderly way: the marker "the hardware is in use" is gone
	CHECK(hw_frames(2) >= 0);
	end_session();
	// the capture is GDI (the lid is closed): the hardware encoder is not started, no test either
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass(); gHwCaptureDxgi = 0;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(log_has("why=gdi_no_output")); CHECK_EQ(gHwOpens, 0); CHECK_EQ(gSelCalls, 0);	// (nothing is enumerated for a machine that would not start the hardware anyway)
	end_session();
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; gHwCaptureDxgi = 0;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(gTestStarts, 0);
	end_session();
	// Winlogon: no test (a result that stands is used)
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; snprintf(gDesktopName, sizeof(gDesktopName), "Winlogon");
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(gTestStarts, 0); CHECK(log_has("why=winlogon"));
	end_session();
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass(); snprintf(gDesktopName, sizeof(gDesktopName), "Winlogon");
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(gHwOpens, 1);
	end_session();
	// the inline mode of the encoder thread (MESH_KVM_ENC_THREAD=0): never
	begin_session(1920, 1080);
	setenv("MESH_KVM_ENC_THREAD", "0", 1);
	end_session();
	{
		int reason = 0;
		free_alive();
		hw_fakes_reset();
		gHwPresent = 1; hw_seed_pass();
		setenv("MESH_KVM_ENC_THREAD", "0", 1);
		kvm_fbuf_setup(NULL);
		kvm_video_session_begin(&reason);
		kvm_stream_init(&gS, 1, 1, reason, KVM_REASON_DISABLED);
		set_geometry(1920, 1080);
		settle_all();
		send_ctrl(KVM_OP_VIDEO_ON, 7);
		CHECK(log_has("why=inline_mode")); CHECK_EQ(gHwOpens, 0); CHECK_EQ(gHwInits, 0);
		end_session();
		setenv("MESH_KVM_ENC_THREAD", "2", 1);
	}
	// a size above 4K: software
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass();
	set_geometry(4160, 2336);
	send_ctrl(KVM_OP_VIDEO_ON, 7, 0, 0, 0);
	CHECK_EQ(gHwOpens, 0);
	end_session();
}

// the hardware encoder fails in a session: the video goes on with the software encoder in a new generation; the error is in the cache
static void test_hw_failures(void)
{
	// a hang: a frame comes back with E_HANG (a deadline of the backend passed)
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(hw_frames(2) >= 1);
	CHECK_EQ(last_gen(), 1);
	gHwFault = HW_HANG; gHwFaultAfter = gHwEncodes;
	hw_frames(1);
	kvm_video_poll();
	CHECK(log_has("video hw fallback why=encode weight=1"));
	CHECK(kvm_video_on() || last_state().gen >= 1);
	hw_frames(1); kvm_video_poll();
	hw_frames(3);
	CHECK_EQ(last_gen(), 2);											// a new generation, the first frame of it an IDR, on the software encoder
	CHECK(log_has("video backend be=sw why=process_disabled"));
	CHECK_EQ(gHwCloses, 1);
	CHECK_EQ(gHwCache.n, 1); CHECK_EQ(kvm_hwp_error_weight(&gHwCache.e[0], (uint32_t)time(NULL)), 1u); CHECK(!kvm_hwp_is_bad(&gHwCache.e[0], (uint32_t)time(NULL))); CHECK_EQ(gHwCache.e[0].verdict, KVM_HWP_V_RETEST);	// (a deadline that passed once: tested again before the hardware is used again)
	CHECK_EQ(gHwCache.trying, 0);
	CHECK_EQ(gTerminateCalls, 0);										// a hang is not an exception: the process goes on
	{
		std::vector<frame_info> fr = frames();
		int idr = 0; for (size_t i = 0; i < fr.size(); i++) { if (fr[i].ok && fr[i].f.gen == 2 && (fr[i].f.flags & KVM_FRAME_IDR) != 0) { idr++; } }
		CHECK(idr >= 1);
	}
	end_session();
	// the hardware encoder is not used again in this process (a new generation: a new size)
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	hw_frames(1);
	gHwFault = HW_HANG; gHwFaultAfter = gHwEncodes;
	hw_frames(2); kvm_video_poll(); hw_frames(2);
	{
		const int opens = gHwOpens;
		gHwFault = HW_OK;
		set_geometry(1280, 720);
		gTestNowMs += 3000; kvm_video_poll(); hw_frames(2);
		CHECK_EQ(gHwOpens, opens);
		CHECK(log_has("why=process_disabled"));
	}
	end_session();
	// an exception in the driver: the counter is written (weight 2) and this process ends
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	hw_frames(1);
	gHwFault = HW_EXCEPTION; gHwFaultAfter = gHwEncodes;
	hw_frames(1); kvm_video_poll();
	CHECK_EQ(gTerminateCalls, 1); CHECK_EQ(gTerminated, 0x4857u);
	CHECK(kvm_hwp_is_bad(&gHwCache.e[0], (uint32_t)time(NULL))); CHECK_EQ(gHwCache.trying, 0);
	end_session();
	// three lost access units in a row: the same way back, weight 1 (the entry is tested again before it is used)
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	hw_frames(2);
	gHwFault = HW_BROKEN; gHwFaultAfter = gHwEncodes;
	hw_frames(5); kvm_video_poll(); hw_frames(3);
	CHECK(log_has("video hw fallback")); CHECK_EQ(kvm_hwp_error_weight(&gHwCache.e[0], (uint32_t)time(NULL)), 1u); CHECK_EQ(gHwCache.e[0].verdict, KVM_HWP_V_RETEST); CHECK(!kvm_hwp_is_bad(&gHwCache.e[0], (uint32_t)time(NULL)));
	CHECK_EQ(gTerminateCalls, 0);
	end_session();
	// the encoder does not open: not a failed request of the viewer, the software encoder
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass(); gHwOpenRc = MFH264_E_CREATE;
	send_ctrl(KVM_OP_VIDEO_ON, 7); kvm_video_poll(); kvm_video_poll();
	hw_frames(3);
	CHECK(log_has("video hw fallback why=open")); CHECK(kvm_video_on()); CHECK_EQ(last_state().reason, 0);
	CHECK_EQ(kvm_hwp_error_weight(&gHwCache.e[0], (uint32_t)time(NULL)), 1u);
	end_session();
	// the QP is refused at open
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass(); gHwSetQpRc = MFH264_E_CODECAPI;
	send_ctrl(KVM_OP_VIDEO_ON, 7); kvm_video_poll(); kvm_video_poll();
	hw_frames(3);
	CHECK(log_has("video hw fallback why=set_qp_open")); CHECK(kvm_video_on());
	end_session();
}

// the marker of a try
static void test_hw_marker(void)
{
	// a marker that nobody resolved (the writer is gone): one error, the entry is tested again before it is used
	begin_session(1920, 1080);
	end_session();
	free_alive(); hw_fakes_reset();
	gHwPresent = 1; hw_seed_pass(); kvm_hwp_begin_try(&gHwCache, HW_KEY_1088, 777, (uint32_t)time(NULL)); gHwPidAlive = 0;
	{
		int reason = 0;
		setenv("MESH_KVM_ENC_THREAD", "2", 1);
		kvm_fbuf_setup(NULL);
		kvm_video_session_begin(&reason);
		kvm_stream_init(&gS, 1, 1, reason, KVM_REASON_DISABLED);
		set_geometry(1920, 1080);
		settle_all();
		CHECK_EQ(gHwCache.trying, 0);
		CHECK_EQ(kvm_hwp_error_weight(&gHwCache.e[0], (uint32_t)time(NULL)), 1u); CHECK_EQ(gHwCache.e[0].verdict, KVM_HWP_V_RETEST);
		send_ctrl(KVM_OP_VIDEO_ON, 7);
		CHECK_EQ(gHwOpens, 0); CHECK_EQ(gTestStarts, 1);				// tested again, software meanwhile
		end_session();
	}
	// a marker of a child that is alive (the other one of -kvm0 / -kvm1): left alone
	free_alive(); hw_fakes_reset();
	gHwPresent = 1; hw_seed_pass(); kvm_hwp_begin_try(&gHwCache, HW_KEY_1088, 777, (uint32_t)time(NULL)); gHwPidAlive = 1;
	{
		int reason = 0;
		kvm_fbuf_setup(NULL);
		kvm_video_session_begin(&reason);
		kvm_stream_init(&gS, 1, 1, reason, KVM_REASON_DISABLED);
		set_geometry(1920, 1080);
		CHECK_EQ(gHwCache.trying, 1); CHECK_EQ(gHwCache.e[0].nErrs, 0);
		end_session();
	}
	// the session ends orderly before the first access unit: the marker is cleared, no error
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(gHwCache.trying, 1);
	end_session();
	CHECK_EQ(gHwCache.trying, 0); CHECK_EQ(gHwCache.e[0].nErrs, 0);
	// a cache that can be read but not written (read-only, locked, the disk is full) from the start: no marker, no hardware (a crash of the driver would be counted by nobody and the restarts would not end);
	// the video goes on with the software encoder. (A fault of the hardware that is planned for the 4th frame never comes: the hardware encoder is not opened.)
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass(); gHwCacheFail = 1;
	gHwFault = HW_EXCEPTION; gHwFaultAfter = 3;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(kvm_video_on()); CHECK_EQ(last_state().reason, 0);
	CHECK(log_has("video hw marker could not be written")); CHECK(log_has("video backend be=sw why=marker_write_failed"));
	CHECK(hw_frames(6) >= 5);
	CHECK_EQ(gHwOpens, 0); CHECK_EQ(gHwInits, 0); CHECK_EQ(gTerminateCalls, 0); CHECK_EQ(gHwCache.trying, 0);
	// ... and it stays so for the generations of this process (no second attempt)
	set_geometry(1280, 720);
	gTestNowMs += 3000; kvm_video_poll(); hw_frames(2);
	CHECK_EQ(gHwOpens, 0); CHECK(log_has("why=process_disabled"));
	end_session();
}

// details of the glue that a mutation of the code has to show: the sequence header of the hardware encoder is not judged at open; the hardware encoder is not given the stage of the software one
static void test_hw_glue_details(void)
{
	// the sequence header that the AMD encoder writes is not the stream (42 against the 40 of the IDR): nothing of it is refused at open (the SPS of the first IDR is)
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass();
	gSpsProfile = 66; gSpsConstraint = 0x80;							// a header that check_open_sps would refuse
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(kvm_video_on());
	CHECK(!log_has("video sps refused"));
	CHECK_EQ(gHwOpens, 1);
	end_session();
	// the software encoder that did not listen to the QP (the bit rate stage) and the hardware encoder after it: the hardware encoder is opened for the QP again
	begin_session(1920, 1080); settle_all();
	gHwPresent = 1; hw_seed_pass(); gHwMode = KVM_HWP_MODE_SW;				// (the mode file keeps the first encoder software)
	gSizeByQp = 0; gReportMode = -1; gPadBytes = 3000;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(log_has("video backend be=sw why=mode_sw"));
	feed_p(150, 0);
	gHwMode = KVM_HWP_MODE_AUTO;											// ... and read again at the next generation
	for (int i = 0; i < 400 && log_count("video qp ineffective stage=1") == 0; i++) { (void)pframe(100 + i); }
	CHECK_EQ(log_count("video qp ineffective stage=1"), 1);
	gTestNowMs += 2500; kvm_video_poll(); kvm_video_poll();
	CHECK(log_has("video backend be=hw"));
	CHECK_EQ(gHwOpens, 1); CHECK_EQ(gHwLastForm, MFH264_QP_ENCODEQP_UI4);
	CHECK(!log_has("video hw fallback"));
	CHECK(kvm_video_on());
	end_session();
	gPadBytes = 0;
}

// the marker stays for the whole hardware session; the exit code of a child that crashed later, or ended itself without its counter, is counted by the parent and the next child uses
// the software encoder), B2 (the enumeration of the encoders hangs: the software encoder is not touched, once per process, written under a key of its own)
static void test_hw_review8(void)
{
	const uint32_t now = (uint32_t)time(NULL);
	kvm_hwp_cache keep;
	// B1: an exception in the driver in the 4th frame, and the counter cannot be written (the file, its mutex): the process ends with 0x4857, the marker is still there, the parent counts it
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(hw_frames(4) >= 3);
	CHECK_EQ(gHwCache.trying, 1); CHECK_EQ(gHwCache.tryUsed, 1);
	gHwCacheFail = 1;
	gHwFault = HW_EXCEPTION; gHwFaultAfter = gHwEncodes;
	hw_frames(1); kvm_video_poll();
	CHECK_EQ(gTerminateCalls, 1); CHECK_EQ(gTerminated, 0x4857u);
	CHECK(log_has("video hw cache not written"));
	CHECK(gHwCache.trying && gHwCache.tryUsed); CHECK_EQ(kvm_hwp_error_weight(&gHwCache.e[0], now), 0u);	// nothing was written
	CHECK_EQ(kvm_hwp_resolve_try(&gHwCache, 4242u, 0x4857u, now), 1);	// the parent
	CHECK(kvm_hwp_is_bad(&gHwCache.e[0], now));
	keep = gHwCache;
	end_session();
	// ... the next child (the service starts it again) finds the entry bad: the software encoder, the restarts end
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; gHwCache = keep;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(kvm_video_on()); CHECK(log_has("video backend be=sw why=bad_persisted")); CHECK_EQ(gHwOpens, 0);
	end_session();
	// B1: the driver crashes in a thread of its own after some seconds of video: the process is gone with 0xC0000005, nothing of ours has run; the marker is what the parent finds
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(hw_frames(6) >= 5);
	keep = gHwCache;
	CHECK(keep.trying && keep.tryUsed && keep.tryPid == 4242u);
	CHECK_EQ(kvm_hwp_resolve_try(&keep, 4242u, 0xC0000005u, now), 1);
	CHECK(kvm_hwp_is_bad(&keep.e[0], now));
	end_session();
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; gHwCache = keep;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(log_has("video backend be=sw why=bad_persisted")); CHECK_EQ(gHwOpens, 0);
	end_session();
	// a normal end of a hardware session: the marker is cleared by the child, and the parent finds nothing
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(hw_frames(3) >= 2); CHECK_EQ(gHwCache.trying, 1);
	end_session();
	CHECK_EQ(gHwCache.trying, 0); CHECK_EQ(gHwCache.e[0].nErrs, 0);
	// the marker is written once per process: a new size does not write it again (one write at the start, one when the first access unit is out, one at the end)
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(hw_frames(3) >= 2);
	{
		const int writes = gHwCacheWrites;
		set_geometry(1280, 720);
		gTestNowMs += 3000; kvm_video_poll(); hw_frames(2);
		CHECK_EQ(gHwOpens, 2); CHECK_EQ(gHwCacheWrites, writes);
	}
	end_session();

	// B2: the enumeration of the encoders hangs: the video goes on with the software encoder (it is not disabled), the enumeration is not asked again in this process, an entry of its own is written
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass(); gSelHang = 1;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(kvm_video_on()); CHECK_EQ(last_state().reason, 0);
	CHECK(log_has("video hw select hung")); CHECK(log_has("video backend be=sw why=select_hung")); CHECK(!log_has("video encoder hung"));
	CHECK_EQ(gHwOpens, 0); CHECK_EQ(gSelCalls, 1);
	CHECK(hw_frames(3) >= 2);
	{
		kvm_hwp_entry *se = kvm_hwp_find(&gHwCache, "sel,rev:" HW_REV_S(KVM_HW_SELFTEST_REV) ",ven:1002,dev:164c,sub:00001234,arev:c1");
		CHECK(se != NULL); CHECK(se != NULL && kvm_hwp_is_bad(se, now));
	}
	set_geometry(1280, 720);
	gTestNowMs += 3000; kvm_video_poll(); hw_frames(2);
	CHECK_EQ(gSelCalls, 1); CHECK(kvm_video_on());
	keep = gHwCache;
	end_session();
	// the next process: no enumeration at all while the entry stands, the software encoder
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; gHwCache = keep; hw_seed_pass();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(kvm_video_on()); CHECK(log_has("why=select_hung_before")); CHECK_EQ(gSelCalls, 0); CHECK_EQ(gHwOpens, 0);
	end_session();
	// the mode file "hw" tries again (the user's switch)
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; gHwCache = keep; hw_seed_pass(); gHwMode = KVM_HWP_MODE_HW;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(gSelCalls, 1); CHECK_EQ(gHwOpens, 1);
	end_session();
	// the mode file "hw" with a hang: the entry does not stop the enumeration, but the process does not ask again (the hardware is off in it)
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass(); gHwMode = KVM_HWP_MODE_HW; gSelHang = 1;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(kvm_video_on()); CHECK_EQ(gSelCalls, 1);
	set_geometry(1280, 720);
	gTestNowMs += 3000; kvm_video_poll(); hw_frames(2);
	CHECK_EQ(gSelCalls, 1); CHECK(log_has("why=process_disabled")); CHECK(kvm_video_on());
	end_session();
	// the enumeration is asked once per process: five generations (sizes), one call
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	for (int i = 0; i < 4; i++) { set_geometry(i % 2 == 0 ? 1280 : 1600, i % 2 == 0 ? 720 : 896); gTestNowMs += 3000; kvm_video_poll(); hw_frames(2); }
	CHECK_EQ(gSelCalls, 1); CHECK(gHwOpens >= 3);
	end_session();

	// the SPS of the hardware encoder is outside what this viewer takes: no error of the hardware is written, the software encoder takes over
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	gSpsProfile = 66; gSpsConstraint = 0x80;
	hw_frames(1);
	CHECK(log_has("video sps refused (IDR) of the hardware encoder")); CHECK(log_has("video hw fallback why=sps_refused weight=0"));
	CHECK_EQ(gHwCache.trying, 0); CHECK_EQ(gHwCache.e[0].nErrs, 0); CHECK_EQ(gHwCache.e[0].verdict, KVM_HWP_V_PASS);
	gSpsProfile = 0; gSpsConstraint = -1;
	gTestNowMs += 100; kvm_video_poll(); hw_frames(3);
	CHECK(kvm_video_on()); CHECK(log_has("video backend be=sw why=process_disabled"));
	end_session();

	// the self test passes while the software encoder runs, and the mode file says "sw" by then: no new generation for nothing
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; gTestWait = 3;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(hw_frames(3) >= 2);
	for (int i = 0; i < 4; i++) { kvm_video_poll(); }
	CHECK(log_has("video hw self test verdict=1"));
	gHwMode = KVM_HWP_MODE_SW;
	gTestNowMs += 2200; kvm_video_poll(); gTestNowMs += 1500; kvm_video_poll();
	CHECK_EQ(last_gen(), 1); CHECK(!log_has("video hw switch")); CHECK_EQ(gHwOpens, 0);
	end_session();

	// the lid is closed (the capture is GDI) while the hardware encoder runs: it stays for the next generation (a cold start in that state is software: gdi_no_output)
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(hw_frames(2) >= 1);
	gHwCaptureDxgi = 0;
	set_geometry(1280, 720);
	gTestNowMs += 3000; kvm_video_poll(); hw_frames(2);
	CHECK_EQ(gHwOpens, 2); CHECK(log_count("why=cached") >= 2);
	end_session();
}

// (the log of the AMD laptop: op 1 before the first open of DXGI, then the whole session on the software encoder): the start waits for the capture to be known; a capture that becomes DXGI asks for another
// decision; a switch that finds no quiet moment is made after 5 s
static void test_hw_capture(void)
{
	// op 1 before the capture is known, the entry says pass: the start waits (the tiles go as JPEG), the capture opens DXGI, the session is hardware from the first generation
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass(); gHwCapUnknown = 1;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(log_has("video hw choice waits for the capture")); CHECK(!kvm_video_on()); CHECK_EQ(gHwOpens, 0); CHECK_EQ(gSelCalls, 0); CHECK(!log_has("why=gdi_no_output"));
	gTestNowMs += 40; kvm_video_poll(); CHECK(!kvm_video_on()); CHECK_EQ(gHwOpens, 0);
	gHwCapUnknown = 0;											// DXGI is open
	gTestNowMs += 20; kvm_video_poll(); kvm_video_poll();
	CHECK(kvm_video_on()); CHECK(log_has("video backend be=hw why=cached")); CHECK_EQ(gHwOpens, 1); CHECK_EQ(last_gen(), 1);
	CHECK(hw_frames(3) >= 2);
	end_session();
	// the capture is never known: after 1 s it is decided as it is (software), and nothing is waited for again
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass(); gHwCapUnknown = 1;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	gTestNowMs += 600; kvm_video_poll(); CHECK(!kvm_video_on());
	gTestNowMs += 600; kvm_video_poll(); kvm_video_poll();
	CHECK(kvm_video_on()); CHECK(log_has("the capture is not known after")); CHECK(log_has("video backend be=sw why=gdi_no_output")); CHECK_EQ(gHwOpens, 0);
	set_geometry(1280, 720); gTestNowMs += 3000; kvm_video_poll(); kvm_video_poll();
	CHECK_EQ(log_count("video hw choice waits for the capture"), 1);	// (once per process: the next generation does not wait again)
	// ... and when it is known to be DXGI, the choice is made again and the switch follows (a quiet moment: nothing moves)
	gHwCapUnknown = 0;
	gTestNowMs += 100; kvm_video_poll(); CHECK(log_has("video hw recheck"));
	gTestNowMs += 2500; kvm_video_poll(); gTestNowMs += 100; kvm_video_poll();
	CHECK(log_has("video hw switch")); CHECK_EQ(last_gen(), 3); CHECK_EQ(gHwOpens, 1);
	end_session();
	// R1-a: DXGI is open but has delivered no image yet (the lid is closed: seconds of silence): not the source. After the wait the software encoder, NO self test; the first image asks for another decision
	// (the test starts then, it passes, the hardware encoder comes with the switch)
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; gHwCapUnknown = 1;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	gTestNowMs += 1200; kvm_video_poll(); kvm_video_poll();
	CHECK(kvm_video_on()); CHECK(log_has("video backend be=sw why=gdi_no_output")); CHECK_EQ(gTestStarts, 0); CHECK_EQ(gHwOpens, 0);
	gTestNowMs += 2000; kvm_video_poll(); CHECK_EQ(gTestStarts, 0);			// (still silent)
	gHwCapUnknown = 0;											// the first image
	gTestNowMs += 100; kvm_video_poll();
	CHECK(log_has("video hw recheck") || gTestStarts == 1); CHECK_EQ(gTestStarts, 1);
	for (int i = 0; i < 3; i++) { gTestNowMs += 100; kvm_video_poll(); }
	gTestNowMs += 2500; kvm_video_poll(); gTestNowMs += 100; kvm_video_poll();
	CHECK(log_has("video hw switch")); CHECK_EQ(gHwOpens, 1);
	end_session();
	// nothing to wait for when there is no hardware encoder at all (the answer is no on the first look), the mode file says sw, or the switch is off
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass(); gHwCapUnknown = 1; gHwMode = KVM_HWP_MODE_SW;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(kvm_video_on()); CHECK(log_has("why=mode_sw")); CHECK(!log_has("video hw choice waits"));
	end_session();
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass(); gHwCapUnknown = 1; gHwEnv = KVM_HWP_ENV_OFF;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(kvm_video_on()); CHECK(!log_has("video hw choice waits"));
	end_session();

	// GDI to DXGI in a session (the lid is opened): hardware from then on; without a hardware encoder no generation is made
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass(); gHwCaptureDxgi = 0;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(log_has("why=gdi_no_output")); CHECK_EQ(gHwOpens, 0); CHECK_EQ(gSelCalls, 0);
	CHECK(hw_frames(3) >= 2);
	gHwCaptureDxgi = 1;
	gTestNowMs += 100; kvm_video_poll();
	CHECK(log_has("video hw recheck")); CHECK_EQ(gSelCalls, 1);
	CHECK_EQ(gHwCache.trying, 0);								// (a decision, not a start: no marker)
	gTestNowMs += 2500; kvm_video_poll(); gTestNowMs += 100; kvm_video_poll();
	CHECK(log_has("video hw switch")); CHECK_EQ(last_gen(), 2); CHECK_EQ(gHwOpens, 1);
	CHECK(hw_frames(3) >= 2);
	end_session();
	begin_session(1920, 1080); settle_all(); gHwPresent = 0; gHwCaptureDxgi = 0;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(hw_frames(3) >= 2);
	gHwCaptureDxgi = 1;
	gTestNowMs += 100; kvm_video_poll(); gTestNowMs += 1500; kvm_video_poll();
	CHECK(!log_has("video hw recheck")); CHECK_EQ(last_gen(), 1); CHECK_EQ(gHwOpens, 0);
	{
		const int reads = gHwCacheReads;										// (no new decision while the capture stays DXGI: nothing changed)
		for (int i = 0; i < 4; i++) { gTestNowMs += 1500; kvm_video_poll(); }
		CHECK_EQ(gHwCacheReads, reads);
	}
	end_session();
	// a screen that never rests: after the self test has passed there is no quiet moment (a frame every 40 ms for 6 s); the switch is made after 5 s anyway, once
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; gTestWait = 2;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	{
		int sw5 = 0;
		for (int i = 0; i < 160 && !sw5; i++) { hw_frames(1); kvm_video_poll(); if (log_has("video hw switch")) { sw5 = i + 1; } }
		CHECK(sw5 > 100);											// not before the 5 s of motion were over (125 frames of 40 ms; the test had passed after a few)
		CHECK(log_has("the screen never rested"));
		hw_frames(1); gTestNowMs += 100; kvm_video_poll(); hw_frames(3);
		CHECK_EQ(last_gen(), 2); CHECK_EQ(gHwOpens, 1); CHECK(log_has("video backend be=hw why=cached"));
	}
	end_session();
}

// the thread of the encoder is given up by the watchdog while it is in the driver (kvm_encthr, not the deadlines of the backend): the hardware encoder is an error of weight 2, the software one takes over
// on the new thread, nothing of the given-up call is shared with the next ones (the call of the selection lives on the heap)
static void test_hw_watchdog(void)
{
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(hw_frames(2) >= 1);
	CHECK_EQ(gHwOpens, 1);
	CHECK_EQ(last_gen(), 1);
	kvm_encthr_test_hang(1, [](uint32_t ms) { gTestNowMs += ms; });
	gTestNowMs += 40; move_rect(5, 10, 8, 20, ++gHwCrc);
	emit_only(0x71);
	CHECK(kvm_video_busy());
	gTestNowMs += 2100;
	kvm_video_service();												// the watchdog: "video encoder hung", the hardware encoder falls back
	CHECK(log_has("video encoder hung backend=hardware MFT"));
	CHECK(log_has("video hw fallback why=hung weight=2"));
	CHECK_EQ(gHwCloses, 0);												// the handle of the given-up thread is not touched
	CHECK_EQ(gTerminateCalls, 0);
	CHECK_EQ(gHwCache.n, 1); CHECK_EQ(kvm_hwp_error_weight(&gHwCache.e[0], (uint32_t)time(NULL)), 2u); CHECK(kvm_hwp_is_bad(&gHwCache.e[0], (uint32_t)time(NULL)));
	CHECK_EQ(gHwCache.trying, 0);
	kvm_encthr_test_hang(0, NULL);
	kvm_video_poll(); hw_frames(1); kvm_video_poll();
	CHECK(hw_frames(3) >= 1);
	CHECK(kvm_video_on());												// not the end of the video: another backend
	CHECK(log_has("video backend be=sw"));
	CHECK(last_gen() >= 2);
	CHECK_EQ(gHwOpens, 1);												// the hardware encoder is not opened again in this process
	end_session();
	kvm_video_leaked_free(); kvm_fbuf_free_all();
}

// the encoder that ignores the QP (the self test found the bit rate: form 8 CBR, 9 LDVBR): the hardware encoder runs in stage 2 (the bit rate, no QP, no Auto of the agent); when it is given up the software
// encoder starts from stage 0 and the Auto that the viewer asked for is the agent's again
static void test_hw_bitrate(void)
{
	// the self test passes with form 8 while the software encoder runs; the switch to the hardware encoder (a new generation) opens it in CBR with no QP form
	begin_session(1920, 1080); settle_all();
	FRAME_RATE_TIMER = 33;
	gHwPresent = 1; gHwAllowRate = 1; gTestForm = KVM_HWP_FORM_BR_CBR; gTestWait = 1;
	send_ctrl(KVM_OP_VIDEO_ON, 7 | KVM_AUTO_OP1_FLAG);
	CHECK(log_has("video backend be=sw why=test_started"));
	CHECK(last_state().caps & KVM_CAPS_AUTO);							// the software encoder listens to the QP: the agent has Auto
	CHECK(hw_frames(3) >= 2);
	for (int i = 0; i < 3; i++) { kvm_video_poll(); }
	CHECK_EQ(gHwCache.n, 1); CHECK_EQ(gHwCache.e[0].qpForm, KVM_HWP_FORM_BR_CBR);
	gTestNowMs += 3500; kvm_video_poll();
	CHECK_EQ(gHwOpens, 1); CHECK_EQ(gHwLastForm, MFH264_QP_NONE); CHECK_EQ(gLastRc, MFH264_RC_CBR);
	CHECK(log_has("video backend be=hw why=cached")); CHECK(log_has("video bitrate mode=0 rc_mode_ok=1"));
	CHECK_EQ(last_state().caps & KVM_CAPS_AUTO, 0);						// the 242 of the generation has no caps bit 2
	CHECK(!kvm_video_test_auto()->on); CHECK_EQ(kvm_video_loop_period(100), 100);
	CHECK(hw_frames(6) >= 3);
	CHECK(gHwQps.empty());												// no set_qp, no step of the QP detector: the bit rate steers
	// the hardware encoder is given up: the software one starts at stage 0 (the QP, not CBR) and the Auto of the op 1 is on again
	gHwFault = HW_HANG; gHwFaultAfter = gHwEncodes;
	hw_frames(1); kvm_video_poll(); hw_frames(1); kvm_video_poll(); hw_frames(3);
	CHECK(log_has("video hw fallback why=encode weight=1")); CHECK(log_has("video backend be=sw why=process_disabled"));
	CHECK_EQ(gLastRc, MFH264_RC_QUALITY);
	CHECK(last_state().caps & KVM_CAPS_AUTO);
	CHECK(kvm_video_test_auto()->on);
	end_session();
	// the form 9: LowDelayVBR (the open refused CBR)
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; gHwAllowRate = 1;
	kvm_hwp_note_test(&gHwCache, HW_KEY_1088, KVM_HWP_V_PASS, KVM_HWP_FORM_BR_LDVBR, 150, 8000, 9500, 400, (uint32_t)time(NULL));
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(gHwOpens, 1); CHECK_EQ(gLastRc, MFH264_RC_LDVBR);
	end_session();
}

// a hardware encoder that makes a key frame of its own (its GOP is finite): the first one is let go, the second one gives the hardware up (once, the software encoder takes over); the key frames that were asked for do not count
static void test_hw_periodic(void)
{
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	gHwPeriodic = 5;
	CHECK(hw_frames(8) >= 6);
	CHECK(!log_has("video hw fallback"));								// one key frame of its own (the 6th picture) so far
	hw_frames(8); kvm_video_poll(); hw_frames(4);
	CHECK(log_has("video hw fallback why=periodic_idr weight=1"));
	CHECK(log_has("video backend be=sw why=process_disabled"));
	end_session();
	// a key frame that was asked for (op 3) is not the encoder's own
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	hw_frames(4);
	gHwForces = 0;
	for (int i = 0; i < 3; i++) { send_ctrl(KVM_OP_REPAIR, KVM_REPAIR_IDR); gTestNowMs += 1500; hw_frames(3); }
	CHECK(!log_has("video hw fallback"));
	end_session();
}

// the hardware encoder takes the QP at the open and ignores a change (NVIDIA in the order B; the self test of an older revision passed it): the session notices (qp_ineffective), opens the same encoder again with the bit rate
// (form 8, stage 2, CBR) and the cache says so; the software encoder only when that try fails
static void test_hw_qp_to_rate(void)
{
	for (int fail = 0; fail < 2; fail++)
	{
		begin_session(1920, 1080); settle_all(); gHwPresent = 1; gHwAllowRate = 1; hw_seed_pass();
		gSizeByQp = 0; gReportMode = -1; gPadBytes = 3000;
		send_ctrl(KVM_OP_VIDEO_ON, 7 | KVM_AUTO_OP1_FLAG);
		CHECK(log_has("video backend be=hw")); CHECK_EQ(gHwLastForm, MFH264_QP_ENCODEQP_UI4);
		CHECK(last_state().caps & KVM_CAPS_AUTO);
		hw_frames(4); kvm_video_test_qp_ineffective();						// (the verdict of the detector: its own steps are the business of kvm_qpdet_test)
		if (fail) { gHwOpenRc = MFH264_E_CODECAPI; }							// the mode is refused at the second open: the software encoder
		kvm_video_poll();
		CHECK_EQ(log_count("video qp ineffective (hardware encoder)"), 1);
		gTestNowMs += 2500; kvm_video_poll(); kvm_video_poll();
		if (!fail)
		{
			CHECK(log_has("video hw: the encoder takes the QP at the open only"));
			CHECK_EQ(gHwOpens, 2); CHECK_EQ(gLastRc, MFH264_RC_CBR); CHECK_EQ(gHwLastForm, MFH264_QP_NONE);
			CHECK(!log_has("video hw fallback"));
			CHECK_EQ(gHwCache.e[0].qpForm, KVM_HWP_FORM_BR_CBR);				// the next session starts with it
			CHECK_EQ(last_state().caps & KVM_CAPS_AUTO, 0); CHECK(kvm_video_on());
			CHECK(hw_frames(3) >= 2);
			CHECK_EQ(log_count("video qp ineffective (hardware encoder)"), 1);	// once: stage 2 has no detector
		}
		else
		{
			CHECK(log_has("video hw fallback why=open weight=1"));
			CHECK(log_has("video backend be=sw")); CHECK_EQ(gLastRc, MFH264_RC_QUALITY);
		}
		end_session();
	}
	gPadBytes = 0;
}

static void test_hw_all(void)
{
	test_hw_none();
	test_hw_cached();
	test_hw_test_flow();
	test_hw_test_results();
	test_hw_switches();
	test_hw_failures();
	test_hw_marker();
	test_hw_watchdog();
	test_hw_glue_details();
	test_hw_review8();
	test_hw_capture();
	test_hw_bitrate();
	test_hw_periodic();
	test_hw_qp_to_rate();
	setenv("MESH_KVM_ENC_THREAD", "2", 1);
}
