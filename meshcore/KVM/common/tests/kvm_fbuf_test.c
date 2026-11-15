// Tests of kvm_fbuf.c: the cache of the picture buffers with a reference count. With -DKVM_FBUF_THREADS also a stress with a capture thread that gets and a main thread that
// releases (run under ThreadSanitizer by run-kvm-encthr-tests.sh).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kvm_fbuf.h"
#ifdef KVM_FBUF_THREADS
#include <pthread.h>
static pthread_mutex_t gMu = PTHREAD_MUTEX_INITIALIZER;
static void mlock_(void *c) { (void)c; pthread_mutex_lock(&gMu); }
static void munlock_(void *c) { (void)c; pthread_mutex_unlock(&gMu); }
#endif

static int gFail = 0, gChecks = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); gChecks++; if (_a != _b) { gFail++; printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

static void test_basic(void)
{
	kvm_fbuf_stats st;
	void *a, *b, *c, *d, *foreign;
	kvm_fbuf_setup(NULL);
	kvm_fbuf_set_size(1000);
	a = kvm_fbuf_get(1000);
	CHECK(a != NULL);
	memset(a, 0x5A, 1000);							// the whole buffer is ours to write (ASan sees an overrun)
	CHECK_EQ(kvm_fbuf_refs(a), 1);
	kvm_fbuf_ref(a);
	CHECK_EQ(kvm_fbuf_refs(a), 2);
	kvm_fbuf_release(a);
	CHECK_EQ(kvm_fbuf_refs(a), 1);					// one owner left
	kvm_fbuf_get_stats(&st);
	CHECK_EQ(st.cached, 0);
	kvm_fbuf_release(a);							// the last one: into the cache
	kvm_fbuf_get_stats(&st);
	CHECK_EQ(st.cached, 1); CHECK_EQ(st.releases, 1);
	CHECK_EQ(kvm_fbuf_refs(a), -1);					// a cached buffer has no owner
	kvm_fbuf_release(a);							// released again: noted, not freed twice (ASan would see it)
	kvm_fbuf_get_stats(&st);
	CHECK_EQ(st.doubleRelease, 1); CHECK_EQ(st.cached, 1);
	b = kvm_fbuf_get(1000);							// the same buffer again, no malloc
	CHECK(b == a);
	kvm_fbuf_get_stats(&st);
	CHECK_EQ(st.reused, 1); CHECK_EQ(st.mallocs, 1);
	CHECK_EQ(kvm_fbuf_refs(b), 1);
	// a size that is not the cached one is not reused
	c = kvm_fbuf_get(2000);
	CHECK(c != NULL && c != b);
	kvm_fbuf_release(c);							// the size of the last request is the current one: cached
	kvm_fbuf_get_stats(&st);
	CHECK_EQ(st.cached, 1); CHECK_EQ(st.frees, 0);
	kvm_fbuf_release(b);							// b has the old size now: freed, not cached
	kvm_fbuf_get_stats(&st);
	CHECK_EQ(st.cached, 1); CHECK_EQ(st.frees, 1);
	b = kvm_fbuf_get(1000);							// asking for the old size drops the cached 2000
	kvm_fbuf_get_stats(&st);
	CHECK_EQ(st.cached, 0); CHECK_EQ(st.trimmed, 1);
	// the cache is limited
	d = kvm_fbuf_get(1000);
	{
		void *x[KVM_FBUF_CACHE_MAX + 2];
		int i;
		kvm_fbuf_release(d);
		kvm_fbuf_release(b);
		for (i = 0; i < KVM_FBUF_CACHE_MAX + 2; i++) { x[i] = kvm_fbuf_get(1000); CHECK(x[i] != NULL); }
		for (i = 0; i < KVM_FBUF_CACHE_MAX + 2; i++) { kvm_fbuf_release(x[i]); }
		kvm_fbuf_get_stats(&st);
		CHECK_EQ(st.cached, KVM_FBUF_CACHE_MAX);
	}
	// a new size drops the cached buffers of the old one
	kvm_fbuf_set_size(500);
	kvm_fbuf_get_stats(&st);
	CHECK_EQ(st.cached, 0);
	// a foreign pointer is freed, not looked at
	foreign = malloc(100);
	kvm_fbuf_release(foreign);
	kvm_fbuf_get_stats(&st);
	CHECK_EQ(st.foreign, 1);
	kvm_fbuf_ref(NULL); kvm_fbuf_release(NULL);
	CHECK(kvm_fbuf_get(0) == NULL);
	CHECK(kvm_fbuf_get((size_t)-1) == NULL);
	// trim
	a = kvm_fbuf_get(500); kvm_fbuf_release(a);
	kvm_fbuf_get_stats(&st); CHECK_EQ(st.cached, 1);
	kvm_fbuf_trim();
	kvm_fbuf_get_stats(&st); CHECK_EQ(st.cached, 0);
	kvm_fbuf_setup(NULL);
}

static void test_registry_full(void)
{
	void *p[20];
	int i, n = 0;
	kvm_fbuf_setup(NULL);
	kvm_fbuf_set_size(64);
	for (i = 0; i < 20; i++) { p[i] = kvm_fbuf_get(64); if (p[i] != NULL) { n++; } }
	CHECK_EQ(n, 16);								// a leak does not grow without a limit: the capture fails
	for (i = 0; i < 20; i++) { kvm_fbuf_release(p[i]); }
	kvm_fbuf_setup(NULL);
}

#ifdef KVM_FBUF_THREADS
#define N_ROUNDS 20000
static void *volatile gHand[2];
static volatile int gHandFull[2];
static void *capture_thread(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < N_ROUNDS; i++)
	{
		void *b = kvm_fbuf_get(4096);
		const int k = i & 1;
		while (__atomic_load_n(&gHandFull[k], __ATOMIC_ACQUIRE)) { }
		memset(b, i & 0xFF, 4096);
		gHand[k] = b;
		__atomic_store_n(&gHandFull[k], 1, __ATOMIC_RELEASE);
	}
	return NULL;
}
static void test_threads(void)
{
	pthread_t th;
	kvm_fbuf_sys sys = { mlock_, munlock_, NULL };
	int i;
	kvm_fbuf_stats st;
	kvm_fbuf_setup(&sys);
	kvm_fbuf_set_size(4096);
	pthread_create(&th, NULL, capture_thread, NULL);
	for (i = 0; i < N_ROUNDS; i++)
	{
		const int k = i & 1;
		void *b;
		while (!__atomic_load_n(&gHandFull[k], __ATOMIC_ACQUIRE)) { }
		b = gHand[k];
		CHECK(((unsigned char *)b)[4095] == (unsigned char)(i & 0xFF));
		kvm_fbuf_ref(b);
		kvm_fbuf_release(b);
		__atomic_store_n(&gHandFull[k], 0, __ATOMIC_RELEASE);
		kvm_fbuf_release(b);
	}
	pthread_join(th, NULL);
	kvm_fbuf_get_stats(&st);
	CHECK(st.mallocs < N_ROUNDS / 20);						// the cache works: not one malloc per frame
	CHECK_EQ(st.doubleRelease, 0);
	kvm_fbuf_setup(NULL);
}
#endif

int main(void)
{
	test_basic();
	test_registry_full();
#ifdef KVM_FBUF_THREADS
	test_threads();
#endif
	printf("%d checks, %d failed\n", gChecks, gFail);
	return gFail ? 1 : 0;
}
