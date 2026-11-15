/* Tests of kvm_hwpolicy.c: the cache file, the errors and the marker, the decision, the key and the result of the self test. */
#include "../kvm_hwpolicy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gFail = 0, gChecks = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define DAY 86400u
#define T0 1731600000u

static kvm_hwp_in base_in(const kvm_hwp_entry *e)
{
	kvm_hwp_in in;
	memset(&in, 0, sizeof(in));
	in.env = KVM_HWP_ENV_AUTO; in.mode = KVM_HWP_MODE_AUTO; in.osOk = 1; in.hwPresent = 1; in.sizeOk = 1; in.displayActive = 1; in.desktopDefault = 1; in.entry = e; in.nowSec = T0;
	return in;
}

static void test_roundtrip(void)
{
	kvm_hwp_cache c, d;
	char buf[4096];
	size_t n;
	kvm_hwp_entry *e;
	memset(&c, 0, sizeof(c));
	e = kvm_hwp_touch(&c, "rev:1,mft:AMD,sz:1", T0);
	e->verdict = KVM_HWP_V_PASS; e->at = T0; e->qpForm = 1; e->openMs = 25; e->p50Us = 8200; e->p95Us = 9600; e->idrKb = 420;
	kvm_hwp_note_error(&c, "rev:1,mft:AMD,sz:1", 1, T0 + 5);
	kvm_hwp_note_test(&c, "rev:1,mft:AMD,sz:2", KVM_HWP_V_SLOW, 0, 30, 25000, 70000, 0, T0 + 6);
	kvm_hwp_note_test(&c, "rev:1,mft:AMD,sz:3", KVM_HWP_V_FAIL, 0, 0, 0, 0, 0, T0 + 8);
	kvm_hwp_note_flat(&c, "rev:1,mft:AMD,sz:1", 1234); kvm_hwp_note_flat(&c, "rev:1,mft:NONE", 77); kvm_hwp_note_flat(&c, "rev:1,mft:AMD,sz:2", 99999);	/* an entry that does not exist is not made; the value is limited */
	kvm_hwp_begin_try(&c, "rev:1,mft:AMD,sz:1", 1234, T0 + 7);
	n = kvm_hwp_format(&c, buf, sizeof(buf));
	CHECK(n > 0 && n < sizeof(buf));
	CHECK(kvm_hwp_parse(&d, buf, n) == 3);
	CHECK(kvm_hwp_find(&d, "rev:1,mft:AMD,sz:3") != NULL && kvm_hwp_find(&d, "rev:1,mft:AMD,sz:3")->failAt == T0 + 8 && kvm_hwp_find(&d, "rev:1,mft:AMD,sz:3")->attempts == 1);
	CHECK(d.trying == 1 && d.tryPid == 1234 && d.tryAt == T0 + 7 && strcmp(d.tryKey, "rev:1,mft:AMD,sz:1") == 0);
	e = kvm_hwp_find(&d, "rev:1,mft:AMD,sz:1");
	CHECK(e != NULL && e->flatMilli == 1234 && kvm_hwp_find(&d, "rev:1,mft:NONE") == NULL && kvm_hwp_find(&d, "rev:1,mft:AMD,sz:2")->flatMilli == 4000u);
	CHECK(e != NULL && e->verdict == KVM_HWP_V_RETEST && e->nErrs == 1 && e->errs[0].at == T0 + 5 && e->errs[0].weight == 1 && e->qpForm == 1 && e->openMs == 25 && e->p50Us == 8200 && e->p95Us == 9600 && e->idrKb == 420);
	e = kvm_hwp_find(&d, "rev:1,mft:AMD,sz:2");
	CHECK(e != NULL && e->verdict == KVM_HWP_V_SLOW && e->at == T0 + 6 && e->attempts == 0);
	/* a second format of the parsed cache is the same text */
	{
		char buf2[4096];
		size_t n2 = kvm_hwp_format(&d, buf2, sizeof(buf2));
		CHECK(n2 == n && memcmp(buf, buf2, n) == 0);
		CHECK(kvm_hwp_format(&d, buf2, 5) == 0);			/* too small: no text */
		CHECK(kvm_hwp_format(&d, buf2, 10) == 0);
		CHECK(kvm_hwp_format(&d, buf2, n - 1) == 0);
	}
}

static void test_damaged(void)
{
	kvm_hwp_cache c;
	const char *t1 = "garbage\nkvmhw 1\n";
	const char *t1b = "garbage 1\ne k v=1 at=1 used=1\n";
	const char *t2 = "kvmhw 2\ne k v=1 at=1 used=1\n";
	const char *t3 = "kvmhw 1\ne k v=9 at=1 used=1\ne k2 v=1 at=x used=1\ne k3 v=1 at=5 used=5 err=5:0\ne k4 v=1 at=5 used=5 err=5:1,6\ne ok v=1 at=5 used=5 bogus=7 att=1\n\ne\nt\nt pid=x\n";
	const char *t4 = "kvmhw 1\ne k v=1 at=1 used=1\ne k v=2 at=2 used=2\n";
	CHECK(kvm_hwp_parse(&c, t1, strlen(t1)) == 0 && c.n == 0 && !c.trying);
	CHECK(kvm_hwp_parse(&c, t1b, strlen(t1b)) == 0 && c.n == 0);
	CHECK(kvm_hwp_parse(&c, t2, strlen(t2)) == 0);
	CHECK(kvm_hwp_parse(&c, t3, strlen(t3)) == 1 && strcmp(c.e[0].key, "ok") == 0 && c.e[0].attempts == 1 && !c.trying);
	CHECK(kvm_hwp_parse(&c, t4, strlen(t4)) == 1 && c.e[0].verdict == KVM_HWP_V_PASS);	/* the same key twice: the first wins */
	CHECK(kvm_hwp_parse(&c, "", 0) == 0);
	CHECK(kvm_hwp_parse(&c, "kvmhw", 5) == 0);
	{
		/* no crash on every cut of a good text and on random bytes (ASan) */
		kvm_hwp_cache g, h;
		char buf[2048];
		size_t n, i;
		unsigned seed = 7;
		memset(&g, 0, sizeof(g));
		kvm_hwp_note_test(&g, "a:b", KVM_HWP_V_PASS, 1, 10, 1, 2, 3, T0);
		kvm_hwp_note_error(&g, "a:b", 1, T0);
		kvm_hwp_begin_try(&g, "a:b", 4, T0);
		n = kvm_hwp_format(&g, buf, sizeof(buf));
		for (i = 0; i <= n; i++) { char *cp = (char *)malloc(i ? i : 1); memcpy(cp, buf, i); kvm_hwp_parse(&h, cp, i); free(cp); }
		for (i = 0; i < 2000; i++)
		{
			char r[200];
			size_t k, len = (size_t)(rand_r(&seed) % 200);
			for (k = 0; k < len; k++) { r[k] = "kvmhw 1\ne t v=0123456789 at=:,\n"[rand_r(&seed) % 32]; }
			kvm_hwp_parse(&h, r, len);
		}
	}
}

static void test_eviction(void)
{
	kvm_hwp_cache c;
	char k[32];
	int i;
	memset(&c, 0, sizeof(c));
	for (i = 0; i < KVM_HWP_ENTRIES; i++) { snprintf(k, sizeof(k), "k%d", i); kvm_hwp_touch(&c, k, T0 + 10u * (unsigned)i)->verdict = KVM_HWP_V_PASS; }
	CHECK(c.n == KVM_HWP_ENTRIES);
	kvm_hwp_touch(&c, "k0", T0 + 100);					/* k0 is used again: k1 is the oldest now */
	kvm_hwp_touch(&c, "new", T0 + 101)->verdict = KVM_HWP_V_PASS;
	CHECK(c.n == KVM_HWP_ENTRIES && kvm_hwp_find(&c, "k1") == NULL && kvm_hwp_find(&c, "k0") != NULL && kvm_hwp_find(&c, "new") != NULL && kvm_hwp_find(&c, "k2") != NULL);
	/* the clock wraps around: still the oldest by difference */
	memset(&c, 0, sizeof(c));
	kvm_hwp_touch(&c, "a", 0xFFFFFFF0u); kvm_hwp_touch(&c, "b", 5); kvm_hwp_touch(&c, "c", 6); kvm_hwp_touch(&c, "d", 7);
	kvm_hwp_touch(&c, "e", 8);
	CHECK(kvm_hwp_find(&c, "a") == NULL && kvm_hwp_find(&c, "b") != NULL);
}

static void test_errors(void)
{
	kvm_hwp_cache c;
	kvm_hwp_entry *e;
	int i;
	memset(&c, 0, sizeof(c));
	kvm_hwp_note_test(&c, "k", KVM_HWP_V_PASS, 1, 10, 1, 2, 3, T0);
	e = kvm_hwp_find(&c, "k");
	CHECK(kvm_hwp_error_weight(e, T0) == 0 && !kvm_hwp_is_bad(e, T0));
	kvm_hwp_note_error(&c, "k", 1, T0 + 10);
	CHECK(e->verdict == KVM_HWP_V_RETEST && kvm_hwp_error_weight(e, T0 + 11) == 1 && !kvm_hwp_is_bad(e, T0 + 11));
	kvm_hwp_note_test(&c, "k", KVM_HWP_V_PASS, 1, 10, 1, 2, 3, T0 + 20);		/* the test passes again: the error still stands */
	CHECK(e->verdict == KVM_HWP_V_PASS && kvm_hwp_error_weight(e, T0 + 21) == 1);
	CHECK(kvm_hwp_error_weight(e, T0 + 10 + KVM_HWP_ERROR_TTL_S) == 1);		/* exactly the TTL: counts */
	CHECK(kvm_hwp_error_weight(e, T0 + 11 + KVM_HWP_ERROR_TTL_S) == 0);		/* older: does not */
	kvm_hwp_note_error(&c, "k", 1, T0 + 30);
	CHECK(kvm_hwp_is_bad(e, T0 + 31));										/* two errors: bad */
	CHECK(!kvm_hwp_is_bad(e, T0 + 20 + KVM_HWP_ERROR_TTL_S));				/* the first has expired, the second has not: weight 1 */
	/* an error with a time in the future (the clock went back) counts */
	memset(&c, 0, sizeof(c));
	kvm_hwp_note_error(&c, "f", 1, T0 + 1000);
	CHECK(kvm_hwp_error_weight(kvm_hwp_find(&c, "f"), T0) == 1);
	/* one exception is bad at once */
	memset(&c, 0, sizeof(c));
	kvm_hwp_note_test(&c, "x", KVM_HWP_V_PASS, 1, 1, 1, 1, 1, T0);
	kvm_hwp_note_error(&c, "x", 2, T0 + 1);
	CHECK(kvm_hwp_is_bad(kvm_hwp_find(&c, "x"), T0 + 2));
	/* an error of an entry that was not tested: the entry is created, RETEST */
	kvm_hwp_note_error(&c, "y", 1, T0);
	CHECK(kvm_hwp_find(&c, "y") != NULL && kvm_hwp_find(&c, "y")->verdict == KVM_HWP_V_RETEST);
	/* the history keeps the newest KVM_HWP_ERR_MAX */
	memset(&c, 0, sizeof(c));
	for (i = 0; i < 9; i++) { kvm_hwp_note_error(&c, "z", 1, T0 + (unsigned)i); }
	e = kvm_hwp_find(&c, "z");
	CHECK(e->nErrs == KVM_HWP_ERR_MAX && e->errs[KVM_HWP_ERR_MAX - 1].at == T0 + 8 && e->errs[0].at == T0 + 9 - KVM_HWP_ERR_MAX);
	/* the weight is clamped */
	kvm_hwp_note_error(&c, "w", 0, T0); kvm_hwp_note_error(&c, "w", 100, T0);
	CHECK(kvm_hwp_find(&c, "w")->errs[0].weight == 1 && kvm_hwp_find(&c, "w")->errs[1].weight == 9);
	/* tests that did not pass */
	memset(&c, 0, sizeof(c));
	kvm_hwp_note_test(&c, "t", KVM_HWP_V_FAIL, 0, 0, 0, 0, 0, T0);
	e = kvm_hwp_find(&c, "t");
	CHECK(e->attempts == 1 && !kvm_hwp_is_bad(e, T0));
	kvm_hwp_note_test(&c, "t", KVM_HWP_V_CRASH, 0, 0, 0, 0, 0, T0);
	CHECK(e->attempts == 2 && kvm_hwp_is_bad(e, T0));
	kvm_hwp_note_test(&c, "t", KVM_HWP_V_PASS, 1, 0, 0, 0, 0, T0);
	CHECK(e->attempts == 0 && !kvm_hwp_is_bad(e, T0));
	kvm_hwp_note_test(&c, "s", KVM_HWP_V_SLOW, 0, 0, 0, 0, 0, T0); kvm_hwp_note_test(&c, "s", KVM_HWP_V_SLOW, 0, 0, 0, 0, 0, T0); kvm_hwp_note_test(&c, "s", KVM_HWP_V_SLOW, 0, 0, 0, 0, 0, T0);
	CHECK(kvm_hwp_find(&c, "s")->attempts == 0);							/* slow never makes an entry bad */
	kvm_hwp_note_test(&c, "u", KVM_HWP_V_TIMEOUT, 0, 0, 0, 0, 0, T0);
	CHECK(kvm_hwp_find(&c, "u")->attempts == 1);
}

static void test_marker(void)
{
	kvm_hwp_cache c;
	memset(&c, 0, sizeof(c));
	CHECK(kvm_hwp_recover_try(&c, 0, T0) == 0);
	kvm_hwp_note_test(&c, "k", KVM_HWP_V_PASS, 1, 1, 1, 1, 1, T0);
	kvm_hwp_begin_try(&c, "k", 77, T0 + 1);
	CHECK(c.trying && c.tryPid == 77);
	CHECK(kvm_hwp_recover_try(&c, 1, T0 + 2) == 0 && c.trying);			/* the writer is alive (another child): nothing happens */
	/* nobody resolved it (the parent died too): ONE error, and the entry has to be tested again; a second one makes it bad */
	CHECK(kvm_hwp_recover_try(&c, 0, T0 + 3) == 1 && !c.trying);
	CHECK(!kvm_hwp_is_bad(kvm_hwp_find(&c, "k"), T0 + 3) && kvm_hwp_error_weight(kvm_hwp_find(&c, "k"), T0 + 3) == 1);
	CHECK(kvm_hwp_find(&c, "k")->verdict == KVM_HWP_V_RETEST);
	kvm_hwp_begin_try(&c, "k", 78, T0 + 4);
	CHECK(kvm_hwp_recover_try(&c, 0, T0 + 5) == 1 && kvm_hwp_is_bad(kvm_hwp_find(&c, "k"), T0 + 5));
	kvm_hwp_begin_try(&c, "k", 79, T0 + 6);
	kvm_hwp_end_try(&c);
	CHECK(!c.trying && c.tryKey[0] == 0 && kvm_hwp_recover_try(&c, 0, T0 + 7) == 0);
	/* a marker without a key (damaged) is cleared without an error */
	kvm_hwp_begin_try(&c, "", 1, T0);
	CHECK(kvm_hwp_recover_try(&c, 0, T0) == 1 && kvm_hwp_find(&c, "") == NULL);
}

/* the parent knows how the child ended: killed at the end of the session is no error, a crash is an exception */
static void test_resolve(void)
{
	kvm_hwp_cache c;
	memset(&c, 0, sizeof(c));
	kvm_hwp_note_test(&c, "k", KVM_HWP_V_PASS, 1, 1, 1, 1, 1, T0);
	/* 1067 (TerminateProcess of kvm_cleanup), 0, 1: the session ended; nothing is held against the encoder */
	{
		static const uint32_t codes[] = { 0u, 1u, 1067u, 3u, 0x7FFFFFFFu, 0xBFFFFFFFu };
		size_t i;
		for (i = 0; i < sizeof(codes) / sizeof(codes[0]); i++)
		{
			kvm_hwp_begin_try(&c, "k", 100, T0 + 1);
			CHECK(kvm_hwp_resolve_try(&c, 100, codes[i], T0 + 2) == 1 && !c.trying);
			CHECK(kvm_hwp_error_weight(kvm_hwp_find(&c, "k"), T0 + 2) == 0 && kvm_hwp_find(&c, "k")->verdict == KVM_HWP_V_PASS && !kvm_hwp_is_bad(kvm_hwp_find(&c, "k"), T0 + 2));
		}
	}
	/* a crash: 0xC0000005 (access violation), 0xC00000FD (stack overflow), 0xC0000409 (abort): an exception, bad at once */
	{
		static const uint32_t codes[] = { 0xC0000005u, 0xC00000FDu, 0xC0000409u, 0xC0000000u, 0xFFFFFFFFu };
		size_t i;
		for (i = 0; i < sizeof(codes) / sizeof(codes[0]); i++)
		{
			memset(&c, 0, sizeof(c));
			kvm_hwp_note_test(&c, "k", KVM_HWP_V_PASS, 1, 1, 1, 1, 1, T0);
			kvm_hwp_begin_try(&c, "k", 100, T0 + 1);
			CHECK(kvm_hwp_resolve_try(&c, 100, codes[i], T0 + 2) == 1 && !c.trying);
			CHECK(kvm_hwp_is_bad(kvm_hwp_find(&c, "k"), T0 + 2) && kvm_hwp_find(&c, "k")->verdict == KVM_HWP_V_RETEST);
		}
	}
	/* the marker of another child is not the parent's to resolve; nothing marked: nothing to do */
	memset(&c, 0, sizeof(c));
	kvm_hwp_note_test(&c, "k", KVM_HWP_V_PASS, 1, 1, 1, 1, 1, T0);
	kvm_hwp_begin_try(&c, "k", 100, T0 + 1);
	CHECK(kvm_hwp_resolve_try(&c, 101, 0xC0000005u, T0 + 2) == 0 && c.trying && c.tryPid == 100 && kvm_hwp_error_weight(kvm_hwp_find(&c, "k"), T0 + 2) == 0);
	kvm_hwp_end_try(&c);
	CHECK(kvm_hwp_resolve_try(&c, 100, 0xC0000005u, T0 + 2) == 0);
	/* the session ends before the first access unit (a viewer that looked and left): the next child starts as if nothing had happened */
	kvm_hwp_begin_try(&c, "k", 102, T0 + 3);
	CHECK(kvm_hwp_resolve_try(&c, 102, 1067u, T0 + 4) == 1 && kvm_hwp_recover_try(&c, 0, T0 + 5) == 0);
	CHECK(KVM_HWP_PROVEN_FRAMES == 1);
}

/* the marker stays for the whole hardware session ("used" after the first access unit); a crash of the driver later, or the exit 0x4857 of a child whose counter was not
   written, is an exception of weight 2; the restarts of the child end because the next one finds the entry bad */
static void test_marker_used(void)
{
	kvm_hwp_cache c, d;
	char buf[4096];
	size_t n;
	memset(&c, 0, sizeof(c));
	kvm_hwp_note_test(&c, "k", KVM_HWP_V_PASS, 1, 1, 1, 1, 1, T0);
	kvm_hwp_begin_try(&c, "k", 100, T0 + 1);
	CHECK(!c.tryUsed);
	kvm_hwp_mark_used(&c, 999); CHECK(!c.tryUsed);						/* another process: not its marker */
	kvm_hwp_mark_used(&c, 100); CHECK(c.tryUsed && c.trying);
	/* it survives the file */
	n = kvm_hwp_format(&c, buf, sizeof(buf));
	CHECK(n > 0 && kvm_hwp_parse(&d, buf, n) == 1 && d.trying && d.tryPid == 100 && d.tryUsed == 1 && strcmp(d.tryKey, "k") == 0);
	/* a crash after the first access unit: counted, bad, the next child uses the software encoder */
	CHECK(kvm_hwp_resolve_try(&d, 100, 0xC0000005u, T0 + 50) == 1 && !d.trying && !d.tryUsed);
	CHECK(kvm_hwp_is_bad(kvm_hwp_find(&d, "k"), T0 + 50) && kvm_hwp_error_weight(kvm_hwp_find(&d, "k"), T0 + 50) == 2);
	/* the child that ended itself after an exception (0x4857) and could not write its counter */
	memset(&c, 0, sizeof(c));
	kvm_hwp_note_test(&c, "k", KVM_HWP_V_PASS, 1, 1, 1, 1, 1, T0);
	kvm_hwp_begin_try(&c, "k", 100, T0 + 1); kvm_hwp_mark_used(&c, 100);
	CHECK(kvm_hwp_resolve_try(&c, 100, KVM_HWP_EXC_EXIT, T0 + 50) == 1 && kvm_hwp_is_bad(kvm_hwp_find(&c, "k"), T0 + 50));
	/* ... also before the first access unit */
	memset(&c, 0, sizeof(c));
	kvm_hwp_note_test(&c, "k", KVM_HWP_V_PASS, 1, 1, 1, 1, 1, T0);
	kvm_hwp_begin_try(&c, "k", 100, T0 + 1);
	CHECK(kvm_hwp_resolve_try(&c, 100, KVM_HWP_EXC_EXIT, T0 + 2) == 1 && kvm_hwp_is_bad(kvm_hwp_find(&c, "k"), T0 + 2));
	/* the normal ends of a hardware session count for nothing: the session ended (1067), orderly (0), 1 */
	{
		static const uint32_t codes[] = { 0u, 1u, 1067u };
		size_t i;
		for (i = 0; i < sizeof(codes) / sizeof(codes[0]); i++)
		{
			memset(&c, 0, sizeof(c));
			kvm_hwp_note_test(&c, "k", KVM_HWP_V_PASS, 1, 1, 1, 1, 1, T0);
			kvm_hwp_begin_try(&c, "k", 100, T0 + 1); kvm_hwp_mark_used(&c, 100);
			CHECK(kvm_hwp_resolve_try(&c, 100, codes[i], T0 + 2) == 1 && !c.trying && kvm_hwp_error_weight(kvm_hwp_find(&c, "k"), T0 + 2) == 0);
		}
	}
	/* a marker "in use" that nobody resolved (the service ended too): only cleared, no error (it says nothing of the encoder); "a start under way" that nobody resolved is one error */
	memset(&c, 0, sizeof(c));
	kvm_hwp_note_test(&c, "k", KVM_HWP_V_PASS, 1, 1, 1, 1, 1, T0);
	kvm_hwp_begin_try(&c, "k", 100, T0 + 1); kvm_hwp_mark_used(&c, 100);
	CHECK(kvm_hwp_recover_try(&c, 0, T0 + 2) == 1 && !c.trying && kvm_hwp_error_weight(kvm_hwp_find(&c, "k"), T0 + 2) == 0);
	kvm_hwp_begin_try(&c, "k", 101, T0 + 3);
	CHECK(kvm_hwp_recover_try(&c, 0, T0 + 4) == 1 && kvm_hwp_error_weight(kvm_hwp_find(&c, "k"), T0 + 4) == 1);
	/* the codes that the system gives when it ends a process at a logoff or a shutdown are no fault of the hardware: the marker is cleared, nothing is counted */
	CHECK(kvm_hwp_exit_counts(0xC000013Au) == 0 && kvm_hwp_exit_counts(0xC000026Bu) == 0);
	CHECK(kvm_hwp_exit_counts(0xC0000005u) == 1 && kvm_hwp_exit_counts(0xC0000142u) == 1 && kvm_hwp_exit_counts(0xC000013Bu) == 1 && kvm_hwp_exit_counts(0xC000026Au) == 1 && kvm_hwp_exit_counts(KVM_HWP_EXC_EXIT) == 1);
	CHECK(kvm_hwp_exit_counts(0u) == 0 && kvm_hwp_exit_counts(1u) == 0 && kvm_hwp_exit_counts(1067u) == 0);
	{
		static const uint32_t codes[] = { 0xC000013Au, 0xC000026Bu };
		size_t i;
		for (i = 0; i < sizeof(codes) / sizeof(codes[0]); i++)
		{
			memset(&c, 0, sizeof(c));
			kvm_hwp_note_test(&c, "k", KVM_HWP_V_PASS, 1, 1, 1, 1, 1, T0);
			kvm_hwp_begin_try(&c, "k", 100, T0 + 1); kvm_hwp_mark_used(&c, 100);
			CHECK(kvm_hwp_resolve_try(&c, 100, codes[i], T0 + 2) == 1 && !c.trying && kvm_hwp_error_weight(kvm_hwp_find(&c, "k"), T0 + 2) == 0);
		}
	}
	/* an old file without the field: "a start under way" */
	CHECK(kvm_hwp_parse(&d, "kvmhw 1\nt pid=7 at=5 key=k\n", 27) == 0 && d.trying && d.tryPid == 7 && d.tryUsed == 0);
}

/* failed tests expire like errors */
static void test_attempts_expire(void)
{
	kvm_hwp_cache c;
	kvm_hwp_entry *e;
	memset(&c, 0, sizeof(c));
	kvm_hwp_note_test(&c, "t", KVM_HWP_V_TIMEOUT, 0, 0, 0, 0, 0, T0);
	e = kvm_hwp_find(&c, "t");
	CHECK(e->attempts == 1 && e->failAt == T0 && !kvm_hwp_is_bad(e, T0));
	kvm_hwp_note_test(&c, "t", KVM_HWP_V_CRASH, 0, 0, 0, 0, 0, T0 + KVM_HWP_ERROR_TTL_S);			/* exactly the TTL later: counts, two tests: bad */
	CHECK(e->attempts == 2 && kvm_hwp_is_bad(e, T0 + KVM_HWP_ERROR_TTL_S));
	CHECK(!kvm_hwp_is_bad(e, T0 + 2 * KVM_HWP_ERROR_TTL_S + 1) && kvm_hwp_is_bad(e, T0 + 2 * KVM_HWP_ERROR_TTL_S));	/* bad for 14 days after the last failure, then tried again */
	memset(&c, 0, sizeof(c));
	kvm_hwp_note_test(&c, "t", KVM_HWP_V_FAIL, 0, 0, 0, 0, 0, T0);
	kvm_hwp_note_test(&c, "t", KVM_HWP_V_FAIL, 0, 0, 0, 0, 0, T0 + KVM_HWP_ERROR_TTL_S + 1);	/* the first is old: this one is the first again */
	e = kvm_hwp_find(&c, "t");
	CHECK(e->attempts == 1 && !kvm_hwp_is_bad(e, T0 + KVM_HWP_ERROR_TTL_S + 2));
	/* an error in a session does not make an old failed test young */
	memset(&c, 0, sizeof(c));
	kvm_hwp_note_test(&c, "t", KVM_HWP_V_FAIL, 0, 0, 0, 0, 0, T0);
	kvm_hwp_note_test(&c, "t", KVM_HWP_V_FAIL, 0, 0, 0, 0, 0, T0 + 10);
	kvm_hwp_note_error(&c, "t", 1, T0 + KVM_HWP_ERROR_TTL_S + 100);
	e = kvm_hwp_find(&c, "t");
	CHECK(e->failAt == T0 + 10 && !kvm_hwp_is_bad(e, T0 + KVM_HWP_ERROR_TTL_S + 200));
}

static void test_parse_switches(void)
{
	CHECK(kvm_hwp_env_parse(NULL) == KVM_HWP_ENV_AUTO && kvm_hwp_env_parse("") == KVM_HWP_ENV_AUTO && kvm_hwp_env_parse("1") == KVM_HWP_ENV_AUTO);
	CHECK(kvm_hwp_env_parse("0") == KVM_HWP_ENV_OFF && kvm_hwp_env_parse("force") == KVM_HWP_ENV_FORCE && kvm_hwp_env_parse("FORCE") == KVM_HWP_ENV_AUTO && kvm_hwp_env_parse("00") == KVM_HWP_ENV_AUTO);
	CHECK(kvm_hwp_mode_parse("sw\n", 3) == KVM_HWP_MODE_SW && kvm_hwp_mode_parse("  hw", 4) == KVM_HWP_MODE_HW && kvm_hwp_mode_parse("auto", 4) == KVM_HWP_MODE_AUTO);
	CHECK(kvm_hwp_mode_parse("swx", 3) == KVM_HWP_MODE_AUTO && kvm_hwp_mode_parse("", 0) == KVM_HWP_MODE_AUTO && kvm_hwp_mode_parse(NULL, 0) == KVM_HWP_MODE_AUTO && kvm_hwp_mode_parse("s", 1) == KVM_HWP_MODE_AUTO);
	CHECK(kvm_hwp_mode_parse("hw sw", 5) == KVM_HWP_MODE_HW && kvm_hwp_mode_parse("sw", 1) == KVM_HWP_MODE_AUTO);	/* the length counts, not the terminator */
}

static void test_capture_file(void)
{
	CHECK(kvm_hwp_capture_parse("gdi", 3) == 1 && kvm_hwp_capture_parse("gdi\r\n", 5) == 1 && kvm_hwp_capture_parse("  gdi  ", 7) == 1 && kvm_hwp_capture_parse("gdi auto", 8) == 1);
	CHECK(kvm_hwp_capture_parse("auto", 4) == 0 && kvm_hwp_capture_parse("", 0) == 0 && kvm_hwp_capture_parse(NULL, 0) == 0 && kvm_hwp_capture_parse("\n\n", 2) == 0);
	CHECK(kvm_hwp_capture_parse("GDI", 3) == 0 && kvm_hwp_capture_parse("gdix", 4) == 0 && kvm_hwp_capture_parse("gd", 2) == 0 && kvm_hwp_capture_parse("1", 1) == 0 && kvm_hwp_capture_parse("xgdi", 4) == 0);
	CHECK(kvm_hwp_capture_parse("gdi", 2) == 0);			/* the length counts, not the terminator */
	CHECK(kvm_hwp_capture_parse("auto gdi", 8) == 0);		/* only the first word */
}

static void test_decide(void)
{
	kvm_hwp_entry e;
	kvm_hwp_in in;
	kvm_hwp_out o;
	memset(&e, 0, sizeof(e));
	/* no entry: a test in the background, software meanwhile */
	in = base_in(NULL); kvm_hwp_decide(&in, &o);
	CHECK(!o.useHardware && o.startTest && strcmp(o.why, "test_started") == 0);
	in.testRunning = 1; kvm_hwp_decide(&in, &o);
	CHECK(!o.useHardware && !o.startTest && strcmp(o.why, "test_running") == 0);
	in.testRunning = 0; in.testDone = 1; kvm_hwp_decide(&in, &o);
	CHECK(!o.useHardware && !o.startTest && strcmp(o.why, "test_done") == 0);
	in = base_in(NULL); in.displayActive = 0; kvm_hwp_decide(&in, &o);
	CHECK(!o.useHardware && !o.startTest && strcmp(o.why, "gdi_no_output") == 0);
	in = base_in(NULL); in.desktopDefault = 0; kvm_hwp_decide(&in, &o);
	CHECK(!o.useHardware && !o.startTest && strcmp(o.why, "winlogon") == 0);
	/* pass: hardware from the first generation, no test */
	e.verdict = KVM_HWP_V_PASS; e.at = T0 - 5;
	in = base_in(&e); kvm_hwp_decide(&in, &o);
	CHECK(o.useHardware && !o.startTest && strcmp(o.why, "cached") == 0);
	in.desktopDefault = 0; kvm_hwp_decide(&in, &o);
	CHECK(o.useHardware);													/* Winlogon uses a result that stands */
	in = base_in(&e); in.displayActive = 0; kvm_hwp_decide(&in, &o);
	CHECK(!o.useHardware && strcmp(o.why, "gdi_no_output") == 0);			/* not started in the lid-closed state ... */
	in.hwRunning = 1; kvm_hwp_decide(&in, &o);
	CHECK(o.useHardware);													/* ... but one that runs stays */
	/* switches and the system */
	in = base_in(&e); in.env = KVM_HWP_ENV_OFF; kvm_hwp_decide(&in, &o); CHECK(!o.useHardware && !o.startTest && strcmp(o.why, "env_off") == 0);
	in = base_in(NULL); in.env = KVM_HWP_ENV_OFF; kvm_hwp_decide(&in, &o); CHECK(!o.startTest && strcmp(o.why, "env_off") == 0);
	in = base_in(&e); in.osOk = 0; kvm_hwp_decide(&in, &o); CHECK(!o.useHardware && strcmp(o.why, "os") == 0);
	in = base_in(&e); in.hwPresent = 0; kvm_hwp_decide(&in, &o); CHECK(!o.useHardware && !o.startTest && strcmp(o.why, "no_hw") == 0);
	in = base_in(NULL); in.hwPresent = 0; kvm_hwp_decide(&in, &o); CHECK(!o.startTest && strcmp(o.why, "no_hw") == 0);	/* no test on a machine without hardware */
	in = base_in(&e); in.sizeOk = 0; kvm_hwp_decide(&in, &o); CHECK(!o.useHardware && strcmp(o.why, "size") == 0);
	in = base_in(&e); in.mode = KVM_HWP_MODE_SW; kvm_hwp_decide(&in, &o); CHECK(!o.useHardware && !o.startTest && strcmp(o.why, "mode_sw") == 0);
	in = base_in(&e); in.mode = KVM_HWP_MODE_HW; kvm_hwp_decide(&in, &o); CHECK(o.useHardware);
	in = base_in(&e); in.processDisabled = 1; kvm_hwp_decide(&in, &o); CHECK(!o.useHardware && !o.startTest && strcmp(o.why, "process_disabled") == 0);
	in = base_in(NULL); in.processDisabled = 1; kvm_hwp_decide(&in, &o); CHECK(!o.startTest && strcmp(o.why, "process_disabled") == 0);
	in = base_in(&e); in.processDisabled = 1; in.env = KVM_HWP_ENV_FORCE; kvm_hwp_decide(&in, &o); CHECK(!o.useHardware);	/* force does not override this process's own error */
	/* an error in a session: test again before the hardware is used */
	e.verdict = KVM_HWP_V_RETEST; e.nErrs = 1; e.errs[0].at = T0 - 100; e.errs[0].weight = 1;
	in = base_in(&e); kvm_hwp_decide(&in, &o); CHECK(!o.useHardware && o.startTest && strcmp(o.why, "test_started") == 0);
	/* two errors: bad until the key changes; force and mode hw try anyway */
	e.verdict = KVM_HWP_V_PASS; e.nErrs = 2; e.errs[1].at = T0 - 50; e.errs[1].weight = 1;
	in = base_in(&e); kvm_hwp_decide(&in, &o); CHECK(!o.useHardware && !o.startTest && strcmp(o.why, "bad_persisted") == 0);
	in.env = KVM_HWP_ENV_FORCE; kvm_hwp_decide(&in, &o); CHECK(o.useHardware);
	in = base_in(&e); in.mode = KVM_HWP_MODE_HW; kvm_hwp_decide(&in, &o); CHECK(o.useHardware);
	in = base_in(&e); in.nowSec = T0 + KVM_HWP_ERROR_TTL_S + 10; kvm_hwp_decide(&in, &o); CHECK(o.useHardware);	/* the errors have expired */
	/* slow: software, a new test after a day */
	memset(&e, 0, sizeof(e)); e.verdict = KVM_HWP_V_SLOW; e.at = T0 - 10;
	in = base_in(&e); kvm_hwp_decide(&in, &o); CHECK(!o.useHardware && !o.startTest && strcmp(o.why, "slow") == 0);
	in.nowSec = T0 - 10 + KVM_HWP_SLOW_RETEST_S - 1; kvm_hwp_decide(&in, &o); CHECK(!o.startTest && strcmp(o.why, "slow") == 0);
	in.nowSec = T0 - 10 + KVM_HWP_SLOW_RETEST_S; kvm_hwp_decide(&in, &o); CHECK(o.startTest);
	/* a test that failed once is repeated, twice is bad */
	memset(&e, 0, sizeof(e)); e.verdict = KVM_HWP_V_FAIL; e.attempts = 1; e.at = T0 - 10; e.failAt = T0 - 10;
	in = base_in(&e); kvm_hwp_decide(&in, &o); CHECK(!o.useHardware && o.startTest);
	e.attempts = 2; kvm_hwp_decide(&in, &o); CHECK(!o.startTest && strcmp(o.why, "bad_persisted") == 0);
	e.verdict = KVM_HWP_V_CRASH; e.attempts = 1; kvm_hwp_decide(&in, &o); CHECK(o.startTest);
	e.verdict = KVM_HWP_V_TIMEOUT; kvm_hwp_decide(&in, &o); CHECK(o.startTest);
	e.verdict = KVM_HWP_V_NONE; e.attempts = 0; kvm_hwp_decide(&in, &o); CHECK(o.startTest && !o.useHardware);
}

static void test_key(void)
{
	kvm_hwp_sig s;
	char k1[KVM_HWP_KEY_MAX], k2[KVM_HWP_KEY_MAX];
	size_t n;
	memset(&s, 0, sizeof(s));
	s.rev = 1; strcpy(s.name, "AMDh264Encoder (x)"); strcpy(s.clsid, "{ADC9BC80-0F41-46C6-AB75-D693D793597D}"); s.vendor = 0x1002; s.device = 0x164C; s.subsys = 0x12345678; s.adapterRev = 0xC1;
	strcpy(s.driver, "31.0.12018.5"); s.osBuild = 26200; s.tokenClass = 1; s.sizeClass = 2;
	n = kvm_hwp_key_build(&s, k1, sizeof(k1));
	CHECK(n > 0 && n == strlen(k1) && strchr(k1, ' ') == NULL && strchr(k1, '\n') == NULL && strchr(k1, '(') == NULL);
	CHECK(strstr(k1, "ven:1002") && strstr(k1, "dev:164c") && strstr(k1, "drv:31.0.12018.5") && strstr(k1, "os:26200") && strstr(k1, "tok:1") && strstr(k1, "sz:2") && strstr(k1, "rev:1,"));
	/* every part of the signature changes the key */
#define DIFF(stmt) do { kvm_hwp_sig t = s; stmt; CHECK(kvm_hwp_key_build(&t, k2, sizeof(k2)) > 0 && strcmp(k1, k2) != 0); } while (0)
	DIFF(t.rev = 2); DIFF(t.name[0] = 'B'); DIFF(t.clsid[1] = '0'); DIFF(t.vendor = 0x10de); DIFF(t.device = 0x1); DIFF(t.subsys = 1); DIFF(t.adapterRev = 1); DIFF(t.driver[0] = '4'); DIFF(t.driver[1] = '2');
	DIFF(t.osBuild = 26201); DIFF(t.tokenClass = 0); DIFF(t.sizeClass = 3);
	{ kvm_hwp_sig t = s; strcpy(t.name, "AMDh264Encoder__x_"); CHECK(kvm_hwp_key_build(&t, k2, sizeof(k2)) > 0 && strcmp(k1, k2) == 0); }	/* the characters outside the set are the same '_' */
	CHECK(kvm_hwp_key_build(&s, k2, 20) == 0 && k2[0] == 0);				/* does not fit */
	CHECK(kvm_hwp_size_class(1920, 1088) == 1 && kvm_hwp_size_class(1920, 1080) == 1 && kvm_hwp_size_class(1920, 1216) == 2 && kvm_hwp_size_class(1920, 1200) == 2 && kvm_hwp_size_class(2560, 1440) == 3);
	CHECK(kvm_hwp_size_class(3840, 2160) == 4 && kvm_hwp_size_class(4096, 2304) == 4 && kvm_hwp_size_class(4096, 2320) == 0 && kvm_hwp_size_class(8192, 4320) == 0 && kvm_hwp_size_class(0, 0) == 0 && kvm_hwp_size_class(1280, 720) == 1);
}

static void test_result(void)
{
	kvm_hwp_result r, q;
	char buf[1024];
	size_t n;
	memset(&r, 0, sizeof(r));
	r.rev = 3; r.verdict = KVM_HWP_V_PASS; strcpy(r.rule, "ok"); r.qpForm = 1; r.openMs = 25; r.p50Us = 8200; r.p95Us = 9600; r.idrKb = 420; r.frames = 40; r.flatMilli = 1234; strcpy(r.key, "rev:3,mft:AMD,sz:1");
	n = kvm_hwp_result_format(&r, buf, sizeof(buf));
	CHECK(n > 0 && n < sizeof(buf) && n == strlen(buf));
	kvm_hwp_result_parse(&q, buf, n);
	CHECK(q.valid && q.rev == 3 && q.verdict == KVM_HWP_V_PASS && strcmp(q.rule, "ok") == 0 && q.qpForm == 1 && q.openMs == 25 && q.p50Us == 8200 && q.p95Us == 9600 && q.idrKb == 420 && q.frames == 40 && q.flatMilli == 1234 && strcmp(q.key, r.key) == 0);
	/* a result that was cut (the process was killed) or is not ours is not valid */
	kvm_hwp_result_parse(&q, buf, n - 4); CHECK(!q.valid);
	kvm_hwp_result_parse(&q, buf, 0); CHECK(!q.valid);
	kvm_hwp_result_parse(&q, "hello\nverdict=1\nend\n", 20); CHECK(!q.valid);
	kvm_hwp_result_parse(&q, "kvmhwtest 1\nend\n", 16); CHECK(!q.valid);					/* no verdict */
	kvm_hwp_result_parse(&q, "kvmhwtest 1\nverdict=7\nend\n", 26); CHECK(!q.valid);		/* a verdict that is none of PASS, FAIL, SLOW */
	r.verdict = KVM_HWP_V_FAIL; strcpy(r.rule, "bad rule: x y"); r.key[0] = 0;
	n = kvm_hwp_result_format(&r, buf, sizeof(buf));
	kvm_hwp_result_parse(&q, buf, n);
	CHECK(q.valid && q.verdict == KVM_HWP_V_FAIL && strchr(q.rule, ' ') == NULL && q.key[0] == 0);
	CHECK(kvm_hwp_result_format(&r, buf, 20) == 0);
}

int main(void)
{
	test_roundtrip(); test_damaged(); test_eviction(); test_errors(); test_marker(); test_resolve(); test_marker_used(); test_attempts_expire(); test_parse_switches(); test_capture_file(); test_decide(); test_key(); test_result();
	printf("kvm_hwpolicy_test: %d checks, %d failed\n", gChecks, gFail);
	return gFail ? 1 : 0;
}
