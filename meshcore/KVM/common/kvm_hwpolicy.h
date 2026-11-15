/*
KVM: the policy of the hardware H.264 encoder. Portable C99, no operating system API, time is passed in (seconds since 1970 for the file, milliseconds for nothing here).

The hardware encoder is ON by default and is used only when a self test of this machine has passed (the test is a short process of its own, kvm_hwtest.cpp, started by the KVM child in the background).
This module is everything about it that needs no operating system, so that it can be tested in a container:
  - the CACHE: a small text file next to the executable (<exe>-kvmhw.txt) with the verdicts of the self test, one entry per KEY (encoder, adapter, driver, Windows build, revision of the test, token class,
    size class), at most KVM_HWP_ENTRIES of them (the oldest is dropped), and the error history of the encoder in sessions. The agent only caches: deleting the file means "test again".
  - the MODE FILE (<exe>-kvmhw-mode.txt): "auto" / "sw" / "hw", read at the start of the process, never written by the agent (A/B of a measurement without restarting the service).
  - the ERRORS: an error of the hardware in a session (hang, exception, no first frame, lost access units, refusal) is written at once; errors older than KVM_HWP_ERROR_TTL_S do not count; a weight of 2 total
    (one exception, or two other errors) makes the entry BAD until the key changes. One error: the next process tests again before it uses the hardware.
  - the MARKER "the hardware is in use by this process": written before the encoder is OPENED, marked "used" when the first access unit has come out of it (KVM_HWP_PROVEN_FRAMES), and cleared only when the
    hardware was given up or closed in an orderly way (an error, a return to the software encoder, the end of the session): a driver crashes at the start (the driver loads, the encoder is made, the first frame)
    and also later, in a thread of its own, where no __try of ours reaches it. The KVM child is killed by its parent at the end of EVERY session (TerminateProcess, code 1067),
    so a marker that is only found when the writer is gone cannot be told from a crash by itself: the PARENT decides, by the exit code of the child (kvm_hwp_resolve_try): killed or exited = the marker is
    cleared, a status of a crash (>= 0xC0000000: access violation, stack overflow, abort), or the code of a child that ended itself after an exception in the driver (KVM_HWP_EXC_EXIT; its own counter may
    not have been written) = an exception of weight 2: the next child finds the entry bad and uses the software encoder, so the restarts of the child end. A marker that nobody resolved (the parent died as well) is found at the start of the next
    process (kvm_hwp_recover_try) and counts as ONE error: it cannot be told apart from a session that was cut short.
  - the DECISION (kvm_hwp_decide): hardware or software, whether to start the self test, and WHY (a short code for the log).
  - the RESULT of the self test process (text on its stdout) and the key (kvm_hwp_key_build).

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef KVM_HWPOLICY_H
#define KVM_HWPOLICY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef KVM_HWP_ENTRIES
#define KVM_HWP_ENTRIES 4				// keys that the cache remembers (a 1200p panel and a 1080p monitor do not erase each other)
#endif
#ifndef KVM_HWP_ERROR_TTL_S
#define KVM_HWP_ERROR_TTL_S (14u * 86400u)	// an error of the hardware older than this does not count
#endif
#ifndef KVM_HWP_SLOW_RETEST_S
#define KVM_HWP_SLOW_RETEST_S 86400u		// a self test that was only too slow is repeated after this long
#endif
#ifndef KVM_HWP_BAD_WEIGHT
#define KVM_HWP_BAD_WEIGHT 2				// weight of the errors (in the TTL) from which the entry is BAD
#endif
#ifndef KVM_HWP_MAX_TESTS
#define KVM_HWP_MAX_TESTS 2					// failed, crashed or timed out self tests of one key before the entry is BAD
#endif
#ifndef KVM_HWP_PROVEN_FRAMES
#define KVM_HWP_PROVEN_FRAMES 1				// access units of the hardware encoder after which the marker "a try is under way" is cleared (the first one: the start is what can crash)
#endif
#define KVM_HWP_EXC_EXIT 0x4857u			// the exit code of a KVM process that ended itself after an exception in the driver (its counter may not have been written: the marker says it)
#define KVM_HWP_STATUS_CONTROL_C 0xC000013Au	// STATUS_CONTROL_C_EXIT and ...
#define KVM_HWP_STATUS_LOGOFF 0xC000026Bu		// ... STATUS_DLL_INIT_FAILED_LOGOFF: the system ended the process (logoff, shutdown): no fault of the hardware
#define KVM_HWP_CRASH_STATUS 0xC0000000u	// an exit code from here on is an NTSTATUS error: the process crashed
#define KVM_HWP_KEY_MAX 256
// the "form" (the qp= field of an entry) 8 and 9 are not forms of the QP (MFH264_QP_* are 1..4): the encoder ignores the QP and the bit rate steers it (CBR; LowDelayVBR when the open refuses CBR)
#define KVM_HWP_FORM_BR_CBR 8
#define KVM_HWP_FORM_BR_LDVBR 9
#define KVM_HWP_ERR_MAX 4

enum { KVM_HWP_V_NONE = 0, KVM_HWP_V_PASS = 1, KVM_HWP_V_FAIL = 2, KVM_HWP_V_SLOW = 3, KVM_HWP_V_CRASH = 4, KVM_HWP_V_TIMEOUT = 5, KVM_HWP_V_RETEST = 6 };	// RETEST: an error in a session, the test has to pass again
enum { KVM_HWP_ENV_AUTO = 0, KVM_HWP_ENV_OFF = 1, KVM_HWP_ENV_FORCE = 2 };		// MESH_KVM_VIDEO_HW: unset or 1, 0, force
enum { KVM_HWP_MODE_AUTO = 0, KVM_HWP_MODE_SW = 1, KVM_HWP_MODE_HW = 2 };			// the mode file

typedef struct kvm_hwp_err { uint32_t at; uint8_t weight; } kvm_hwp_err;

typedef struct kvm_hwp_entry
{
	char key[KVM_HWP_KEY_MAX];
	uint8_t verdict;					// KVM_HWP_V_*
	uint32_t at;						// seconds: when the verdict was made
	uint32_t used;						// seconds: when the entry was last looked at (the oldest entry is dropped)
	uint32_t failAt;					// seconds: when the last test that did not pass was made (the attempts expire KVM_HWP_ERROR_TTL_S after it)
	uint8_t attempts;					// self tests that did not pass (FAIL, CRASH, TIMEOUT) within KVM_HWP_ERROR_TTL_S of each other (an old failure does not count: kvm_hwp_note_test, kvm_hwp_is_bad)
	uint8_t qpForm;						// the form of the QP that the test found (MFH264_QP_*), 0 = none; KVM_HWP_FORM_BR_CBR / _LDVBR (8 / 9): no QP, the bit rate steers
	uint32_t openMs, p50Us, p95Us, idrKb;	// what the test measured (information)
	uint32_t flatMilli;					// the key frame of a flat picture, thousandths of a byte per macroblock (the floor of a flat key frame of this encoder; 0 = not measured)
	kvm_hwp_err errs[KVM_HWP_ERR_MAX];	// errors in sessions, newest last
	uint8_t nErrs;
} kvm_hwp_entry;

typedef struct kvm_hwp_cache
{
	kvm_hwp_entry e[KVM_HWP_ENTRIES];
	int n;
	int tryUsed;						// the marker is of a process whose hardware encoder has produced an access unit (it is "in use": it stays until the hardware is closed in an orderly way)
	int trying;							// the marker: a process is using the hardware for the first time since the marker was cleared
	uint32_t tryPid, tryAt;
	char tryKey[KVM_HWP_KEY_MAX];
} kvm_hwp_cache;

// The text of the file -> the cache (an unknown line or a damaged entry is skipped; a text that is not ours gives an empty cache). Returns the number of entries.
int kvm_hwp_parse(kvm_hwp_cache *c, const char *text, size_t n);
// The cache -> text. Returns the length (0 when cap is too small; the text is then not usable).
size_t kvm_hwp_format(const kvm_hwp_cache *c, char *out, size_t cap);
kvm_hwp_entry *kvm_hwp_find(kvm_hwp_cache *c, const char *key);
// The entry of the key: found, or a new one (the oldest is dropped when the cache is full); `used` is set to now.
kvm_hwp_entry *kvm_hwp_touch(kvm_hwp_cache *c, const char *key, uint32_t nowSec);
// Weight of the errors that still count (within KVM_HWP_ERROR_TTL_S of now).
unsigned kvm_hwp_error_weight(const kvm_hwp_entry *e, uint32_t nowSec);
int kvm_hwp_is_bad(const kvm_hwp_entry *e, uint32_t nowSec);	// BAD: errors of weight >= KVM_HWP_BAD_WEIGHT, or KVM_HWP_MAX_TESTS tests that did not pass

int kvm_hwp_env_parse(const char *value);						// NULL / "" / "1" / anything else: AUTO; "0": OFF; "force": FORCE
int kvm_hwp_mode_parse(const char *text, size_t n);				// the first word of the mode file: "sw", "hw", else AUTO (also for an unreadable file)
// The file of the capture (<exe>-kvm-capture.txt, read when a KVM process starts, never written by the agent): its first word "gdi" forces the GDI capture exactly like MESH_KVM_FORCE_GDI=1.
int kvm_hwp_capture_parse(const char *text, size_t n);			// 1 = "gdi"; "auto", no file, any other word: 0

// The self test has finished (or did not): the verdict goes into the entry of the key. KVM_HWP_V_PASS clears `attempts`; FAIL, CRASH and TIMEOUT count; SLOW does not.
void kvm_hwp_note_test(kvm_hwp_cache *c, const char *key, int verdict, int qpForm, uint32_t openMs, uint32_t p50Us, uint32_t p95Us, uint32_t idrKb, uint32_t nowSec);
// An error of the hardware in a session (weight 1; 2 for an exception / a hang that did not return). The entry gets the error and, when it was PASS, becomes RETEST. Written to the file by the caller at once.
void kvm_hwp_note_error(kvm_hwp_cache *c, const char *key, int weight, uint32_t nowSec);
// the floor of a flat key frame that the self test measured (thousandths of a byte per macroblock), kept with the entry; only for an entry that exists.
void kvm_hwp_note_flat(kvm_hwp_cache *c, const char *key, uint32_t flatMilli);
// The marker. begin: before the hardware is used in this process; end: the hardware has proved itself, or was given up.
void kvm_hwp_begin_try(kvm_hwp_cache *c, const char *key, uint32_t pid, uint32_t nowSec);
void kvm_hwp_end_try(kvm_hwp_cache *c);
// The first access unit of the hardware encoder is out: the marker is no longer "a start under way" but "the hardware is in use" (it stays for the whole hardware session: a crash of the driver later, in a thread
// of its own, is as much an error as one at the start). It is cleared by an orderly end (kvm_hwp_end_try), a return to the software encoder, or by the parent (kvm_hwp_resolve_try).
void kvm_hwp_mark_used(kvm_hwp_cache *c, uint32_t pid);
// At the start of a process: a marker whose writer (`alive` = the process with tryPid still exists and is not this one) is gone and that the parent did not resolve: counted as ONE error and cleared (a marker that is "in use" is only cleared: the service or the machine ended, which says nothing of the encoder). Returns 1 when it did.
int kvm_hwp_recover_try(kvm_hwp_cache *c, int alive, uint32_t nowSec);
// 1 when this exit code of a KVM child is a fault of the hardware encoder if the child had the marker: a status of a crash (>= KVM_HWP_CRASH_STATUS) or KVM_HWP_EXC_EXIT, except the two that the system
// gives when it ends a process at a logoff or a shutdown.
int kvm_hwp_exit_counts(uint32_t exitCode);
// The parent knows how the child `pid` ended (exitCode): when the marker is its, an exit with a status of a crash (>= KVM_HWP_CRASH_STATUS) or KVM_HWP_EXC_EXIT (the child ended itself after an exception, perhaps without its counter) is an exception of weight 2, any other (0, 1, 1067 = killed by the
// parent at the end of the session ...) is no error; the marker is cleared. Returns 1 when the marker was the child's.
int kvm_hwp_resolve_try(kvm_hwp_cache *c, uint32_t pid, uint32_t exitCode, uint32_t nowSec);

// What the decision knows. (The caller measures; this module only decides.)
typedef struct kvm_hwp_in
{
	int env;							// KVM_HWP_ENV_*
	int mode;							// KVM_HWP_MODE_*
	int osOk;							// Windows 8 or later, Media Foundation
	int hwPresent;						// the system lists a hardware H.264 encoder (MFTEnumEx)
	int sizeOk;							// the coded size is within what the hardware is tried on (kvm_hwp_size_class != 0)
	int displayActive;					// DXGI sees an active output (not the lid-closed GDI state)
	int desktopDefault;					// the child runs on the desktop "Default" (not Winlogon / UAC)
	int hwRunning;						// the hardware encoder runs in this process now
	int processDisabled;				// an error of the hardware in this process: off until it ends
	int testRunning;					// a self test of this process is under way
	int testDone;						// a self test of this process has finished (a second one is not started in the same process)
	const kvm_hwp_entry *entry;			// the entry of the key, NULL = none
	uint32_t nowSec;
} kvm_hwp_in;

typedef struct kvm_hwp_out
{
	int useHardware;					// the next generation uses the hardware encoder
	int startTest;						// the caller starts the self test now
	const char *why;					// short code for the log: env_off os no_hw size mode_sw process_disabled bad_persisted gdi_no_output winlogon test_running slow test_done cached test_started
} kvm_hwp_out;

void kvm_hwp_decide(const kvm_hwp_in *in, kvm_hwp_out *out);

// The size class of a coded size (the key holds it: a result of 1080p says nothing about 4K): 1 up to 8160 macroblocks (1920x1088), 2 up to 9120 (1920x1216), 3 up to 14400 (2560x1440), 4 up to 36864 (4096x2304); 0 above.
int kvm_hwp_size_class(unsigned codedW, unsigned codedH);

// What the key is made of (all of it must be the same for a verdict to hold)
typedef struct kvm_hwp_sig
{
	int rev;							// KVM_HW_SELFTEST_REV: raised when mf_h264_hw.cpp or the self test changes (not the version of the agent: every test deployment would start on the software encoder)
	char name[48];						// the name of the encoder MFT
	char clsid[40];
	unsigned vendor, device, subsys, adapterRev;
	char driver[40];					// the version of the user mode driver
	unsigned osBuild;
	int tokenClass;						// 0 other, 1 SYSTEM in a user session (the KVM child), 2 a user
	int sizeClass;						// kvm_hwp_size_class
} kvm_hwp_sig;
// The key as a line of text without spaces (anything but letters, digits and - . _ : is turned into '_'). Returns the length, 0 if it does not fit.
size_t kvm_hwp_key_build(const kvm_hwp_sig *s, char *out, size_t cap);

// The result of the self test process, as text on its stdout ("kvmhwtest 1" first line, then key=value lines)
typedef struct kvm_hwp_result
{
	int valid;							// the text was complete (ended with "end")
	int rev;							// KVM_HW_SELFTEST_REV of the test process
	int verdict;						// KVM_HWP_V_PASS, FAIL, SLOW
	char rule[40];						// what failed: a rule of the viewer, "open", "qp", "idr", "inflight", "latency", "colour", ...
	int qpForm;
	uint32_t openMs, p50Us, p95Us, idrKb, frames, flatMilli;
	char key[KVM_HWP_KEY_MAX];			// the key the test process worked out: the child compares it with its own (the encoder or the adapter changed meanwhile: the result is dropped)
} kvm_hwp_result;
size_t kvm_hwp_result_format(const kvm_hwp_result *r, char *out, size_t cap);
void kvm_hwp_result_parse(kvm_hwp_result *r, const char *text, size_t n);

#ifdef __cplusplus
}
#endif

#endif // KVM_HWPOLICY_H
