/*
KVM (Windows): the self test of the hardware H.264 encoder as a process, the cache file of its verdicts and the files of the switches.

The KVM child starts the same executable with `-kvmhwtest W H level maxLevel profile luid rev key` (ServiceMain.c; this module: kvm_hwtest_main) in the background: a short process of its own, with the token, the session and the
desktop of the child (the environment that the encoder is used in), in a job object that kills it with the child, with a limit of 25 s and of memory. The test process runs the checks of kvm_hwtest_core.c on the hardware
encoder and prints the result (kvm_hwp_result) on its STDOUT; the child reads it, compares its key with its own and writes the verdict into the cache. A driver that hangs or crashes kills the test process only.

The files (next to the executable, like the log of the KVM child; written as a temporary file and MoveFileEx, under the named mutex Global\MeshKvmHwCache because the KVM children of -kvm0 and -kvm1 may overlap):
  <exe>-kvmhw.txt        the cache of the verdicts and the marker of a try that crashed (kvm_hwpolicy.h). Only a cache: deleting it means "test again".
  <exe>-kvmhw-mode.txt   one word: auto / sw / hw. Read when a KVM process STARTS, never written by the agent. "sw" takes effect with the next session (the next KVM child), without restarting the service: it can be
                         written by a remote command (Run Commands), e.g.  echo sw > "C:\Program Files\Mesh Agent\MeshAgent-kvmhw-mode.txt".

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef KVM_HWTEST_H
#define KVM_HWTEST_H

#include <stddef.h>
#include <stdint.h>
#include "../common/kvm_hwpolicy.h"
#include "kvm_encbe_mfhw.h"

#ifdef __cplusplus
extern "C" {
#endif

// KVM_HW_SELFTEST_REV is raised whenever mf_h264_hw.cpp, kvm_hwtest_core.c or this self test changes in a way that can change a verdict: the key of the cache has it, so every change is tested again once. 1 = the hardware encoder, 2 = (the frames by the view, the flat key frame), 3 = (the second pass "rate" for an encoder that ignores the QP), 4 = (the plan of that pass as the probe, no BufferSize in CBR / LDVBR), 5 = (the GOP is read back, two orders of the settings, rule "gop"), 6 = (the QP pass has the plan of the pass "rate": NVIDIA takes the QP at the open only), 7 = (the hardware encoder waits for the next HaveOutput after a stream change: the Intel MFT answers a second ProcessOutput without an event with E_UNEXPECTED; the test process runs with timeBeginPeriod(1) like the session).
#ifndef KVM_HW_SELFTEST_REV
#define KVM_HW_SELFTEST_REV 7		// 7: after MF_E_TRANSFORM_STREAM_CHANGE the encoder waits for the next HaveOutput before it calls ProcessOutput again (an Intel MFT gave no frame before), the failures of the encoder are told on stderr, the test runs with timeBeginPeriod(1) like the session; 6: the pass "qp" has the plan of the pass "rate" (a key frame paid back, windows measured from the 4th picture); 5: the GOP is read back after the types, the order of the settings B (the codec API before the types) when it does not hold, FAIL "gop" when it still is a periodic key frame; 4: the plan of the pass "rate" as the probe (a key frame paid back, windows measured from the 4th picture), no AVEncCommonBufferSize in CBR / LDVBR; 3: an encoder that ignores the QP is tried again with the bit rate (forms 8 and 9, rule "rate"); 2: the frames go by the NV12 view like a session's (encode_nv12), a hang or an exception on the flat key frame is a failure
#endif
#ifndef KVM_HWTEST_TIMEOUT_MS
#define KVM_HWTEST_TIMEOUT_MS 25000u
#endif
#ifndef KVM_HWTEST_MEMORY_MB
#define KVM_HWTEST_MEMORY_MB 512u
#endif

#define KVM_HWTEST_TOKEN_OTHER 0
#define KVM_HWTEST_TOKEN_SYSTEM_IN_SESSION 1	// the KVM child: SYSTEM in a session of a user
#define KVM_HWTEST_TOKEN_USER 2

// The key of the cache for this machine: the encoder chosen for the adapter `luid` (must be called on a thread that did be->init(), i.e. on the encoder thread), this Windows, this token, this size class. 1 = made, 0 = there is no hardware encoder.
int kvm_hwtest_make_key(int64_t luid, unsigned codedW, unsigned codedH, char *key, size_t cap, kvm_mfhw_sel *selOut);
// The selection of the encoder on a thread of its own with a time limit (kvm_mfhw_select_timed): 1 = found, 0 = no hardware encoder, -1 = the enumeration did not return in time.
int kvm_hwtest_select(int64_t luid, kvm_mfhw_sel *sel);
// The key of the cache for a selected encoder and the coded size (1 = made).
int kvm_hwtest_key_from_sel(const kvm_mfhw_sel *sel, unsigned codedW, unsigned codedH, char *key, size_t cap);
// The key of the entry "the enumeration hangs here": made of what is known without it (the adapter of the captured output).
int kvm_hwtest_select_key(unsigned vendor, unsigned device, unsigned subsys, unsigned adapterRev, char *key, size_t cap);
int kvm_hwtest_token_class(void);

// ---- the files ----
// Loads the cache (an empty one when the file is missing or not ours), changes it with fn under the mutex, writes it back. fn returns 0 when it changed nothing (no write). 0 = done, -1 = the mutex or the file did not work (the caller goes on without the cache).
typedef int (*kvm_hwcache_fn)(kvm_hwp_cache *c, void *arg);
int kvm_hwcache_update(kvm_hwcache_fn fn, void *arg);
int kvm_hwcache_read(kvm_hwp_cache *c);									// only a read (no mutex: the file is replaced as a whole)
int kvm_hwmode_read(void);												// KVM_HWP_MODE_*
int kvm_capturefile_read(void);											// 1 when <exe>-kvm-capture.txt says "gdi" (read at the start of a KVM process; no file, "auto", another word: 0)
int kvm_hwenv_read(void);												// KVM_HWP_ENV_* of MESH_KVM_VIDEO_HW
int kvm_hw_pid_alive(uint32_t pid);										// a process with this id exists and is not this one (the marker of a try)

// ---- the marker of a try that the PARENT resolves ----
// The KVM child with this pid has ended or is being ended with exitCode (1067 = TerminateProcess at the end of a session): clears its marker; a crash status (>= 0xC0000000) is an exception of weight 2.
int kvm_hwtest_child_gone(uint32_t pid, uint32_t exitCode);

// ---- the process of the test (the parent side) ----
typedef struct kvm_hwtest_run
{
	void *proc, *job, *pipe;
	unsigned long long startedMs;	// the clock that does not count the sleep of the machine
	char buf[2048];
	size_t n;
	int running, killed;
} kvm_hwtest_run;

// Starts the test process (suspended, put into the job, then resumed). 0 = started, -1 = no (nothing is running).
int kvm_hwtest_start(kvm_hwtest_run *r, unsigned codedW, unsigned codedH, int level, int maxLevel, int profile, int64_t luid, const char *key);
// 0 = still running; 1 = finished (or killed after the time limit): *verdict is KVM_HWP_V_PASS / FAIL / SLOW with *res valid, or KVM_HWP_V_CRASH (the process died without a result) / KVM_HWP_V_TIMEOUT.
int kvm_hwtest_poll(kvm_hwtest_run *r, int *verdict, kvm_hwp_result *res);	// never waits (the loop of the KVM child calls it): a process that was killed after the time limit is seen by a later poll
// The KVM child ends (or the test is not wanted any more): kills the process and the job; no verdict (an interrupted test is not a failure).
void kvm_hwtest_abort(kvm_hwtest_run *r);

// ---- the process of the test (its own side): ServiceMain.c calls this for -kvmhwtest ----
int kvm_hwtest_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif
