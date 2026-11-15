/*
KVM: the buffers of the captured pictures. Portable C99.

The capture thread makes a picture of 8 MB (1080p) or more for every frame (a copy of the persistent image or the scaled one) and the main loop frees it at the end of the iteration. With the
encoder in its own thread the picture is also owned by the job in flight, so it has a reference count; and instead of malloc / free of 8 MB for every frame (the system zeroes the new pages
and the capture thread pays for the first touch) the freed buffers are cached, up to a small number.

The buffer is an ordinary pointer to the pixels, so the code that uses it (tile.cpp, kvm.c) does not change its types. In front of the pixels there is a header of 64 bytes (the size, the count). The module keeps a registry of its buffers: kvm_fbuf_release() of a pointer that is not in it (a path of the capture code that still uses malloc) is a plain free() and is counted, it does not damage the heap.

Ownership: the reference count is changed only by the thread that owns the buffer at that moment (the capture thread makes it with 1 and hands it over through the event of the capture; the main
loop takes a reference for a job and releases it after the result was collected), so it needs no atomic operation. The cache is shared by the capture thread (get) and the main loop (release):
it is guarded by the lock that the caller gives (a critical section on Windows; a no-op for tests with one thread). A buffer that a hung encoder may still read is never released: the caller leaks it.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef KVM_FBUF_H
#define KVM_FBUF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KVM_FBUF_HDR 64u
#define KVM_FBUF_CACHE_MAX 3		// freed buffers that are kept (the frame being captured, the one the loop works on, the one in the encoder)

typedef struct kvm_fbuf_sys
{
	void (*lock)(void *ctx);
	void (*unlock)(void *ctx);
	void *ctx;
} kvm_fbuf_sys;

typedef struct kvm_fbuf_stats
{
	unsigned long long gets, reused, mallocs, releases, frees, foreign, doubleRelease, trimmed;
	unsigned cached;
} kvm_fbuf_stats;

// Sets the lock (NULL = none) and empties the cache. Call before the capture thread starts and after it stopped.
void kvm_fbuf_setup(const kvm_fbuf_sys *sys);
// A buffer of `size` bytes (contents undefined), reference count 1; NULL when the memory is short. Takes a cached buffer of exactly that size when there is one.
void *kvm_fbuf_get(size_t size);
// One more owner (a job in flight)
void kvm_fbuf_ref(void *p);
// One owner less; the last one puts the buffer into the cache (when its size is the current one and the cache is not full) or frees it. NULL is allowed. A pointer without the magic is freed.
void kvm_fbuf_release(void *p);
// The size of the pictures changed: cached buffers of another size are freed now (and later ones are freed instead of cached). kvm_fbuf_get() does this by itself when it is asked for another size.
void kvm_fbuf_set_size(size_t size);
// Frees the cache (the video is off for a while, the session ends)
void kvm_fbuf_trim(void);
// References now (for the tests and the log); -1 for a foreign pointer
int kvm_fbuf_refs(const void *p);
void kvm_fbuf_get_stats(kvm_fbuf_stats *st);
// Tests only: frees every buffer that exists, in use or not (what a hung encoder was given up with is leaked on purpose; a test frees it to let the leak checker see the rest)
void kvm_fbuf_free_all(void);

#ifdef __cplusplus
}
#endif

#endif
