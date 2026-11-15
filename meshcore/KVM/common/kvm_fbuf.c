/*
KVM: the buffers of the captured pictures. See kvm_fbuf.h. Portable C99.

A pointer is ours when it is in the registry of the buffers that this module allocated (a pointer from malloc must not be looked at through a header in front of it).

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#include "kvm_fbuf.h"
#include <stdlib.h>
#include <string.h>

#define FB_MAGIC 0x4B464231u	// "KFB1": a buffer in use
#define FB_DEAD  0xDEADF00Du	// a buffer in the cache (refs 0)
#define FB_REG_MAX 16

typedef struct fb_hdr
{
	uint32_t magic;
	int32_t refs;
	size_t size;
	uint8_t pad[KVM_FBUF_HDR - 8u - sizeof(size_t)];
} fb_hdr;

static kvm_fbuf_sys gSys;
static fb_hdr *gReg[FB_REG_MAX];	// every buffer that exists (in use or cached)
static fb_hdr *gCache[KVM_FBUF_CACHE_MAX];
static unsigned gNCache;
static size_t gSize;
static kvm_fbuf_stats gSt;

static void lk(void) { if (gSys.lock != NULL) { gSys.lock(gSys.ctx); } }
static void ul(void) { if (gSys.unlock != NULL) { gSys.unlock(gSys.ctx); } }

static void set_size_locked(size_t size);

// (lock held) the registry slot of a pixel pointer, or -1
static int reg_find(const void *p)
{
	int i;
	if (p == NULL) { return -1; }
	for (i = 0; i < FB_REG_MAX; i++) { if (gReg[i] != NULL && (const uint8_t *)gReg[i] + KVM_FBUF_HDR == (const uint8_t *)p) { return i; } }
	return -1;
}

void kvm_fbuf_setup(const kvm_fbuf_sys *sys)
{
	unsigned i;
	for (i = 0; i < gNCache; i++) { free(gCache[i]); }	// the cached ones; a buffer that is still in use belongs to its owner (a hung encoder may be reading it: it is not freed here, the registry just forgets it)
	memset(gReg, 0, sizeof(gReg));
	memset(gCache, 0, sizeof(gCache));
	gNCache = 0;
	gSize = 0;
	memset(&gSt, 0, sizeof(gSt));
	memset(&gSys, 0, sizeof(gSys));
	if (sys != NULL) { gSys = *sys; }
}

void *kvm_fbuf_get(size_t size)
{
	fb_hdr *h = NULL;
	unsigned i;
	int slot = -1;
	if (size == 0 || size > ((size_t)-1) - KVM_FBUF_HDR) { return NULL; }
	lk();
	gSt.gets++;
	if (size != gSize) { set_size_locked(size); }	// the size of the pictures changed (a new geometry): the cache of the old size is useless
	for (i = 0; i < gNCache; i++)
	{
		if (gCache[i]->size == size) { h = gCache[i]; gCache[i] = gCache[--gNCache]; gCache[gNCache] = NULL; gSt.reused++; break; }
	}
	if (h == NULL) { for (i = 0; i < FB_REG_MAX; i++) { if (gReg[i] == NULL) { slot = (int)i; break; } } }
	ul();
	if (h == NULL)
	{
		if (slot < 0) { return NULL; }	// sixteen buffers exist: something leaks (the caller counts it as a failed capture)
		h = (fb_hdr *)malloc(KVM_FBUF_HDR + size);
		if (h == NULL) { return NULL; }
		h->size = size;
		lk();
		if (gReg[slot] != NULL) { for (slot = 0; slot < FB_REG_MAX && gReg[slot] != NULL; slot++) { } }
		if (slot >= FB_REG_MAX) { ul(); free(h); return NULL; }
		gReg[slot] = h; gSt.mallocs++;
		ul();
	}
	h->magic = FB_MAGIC;
	h->refs = 1;
	return (uint8_t *)h + KVM_FBUF_HDR;
}

void kvm_fbuf_ref(void *p)
{
	int i;
	if (p == NULL) { return; }
	lk(); i = reg_find(p); ul();
	if (i >= 0 && gReg[i]->magic == FB_MAGIC) { gReg[i]->refs++; }
}

void kvm_fbuf_release(void *p)
{
	fb_hdr *h;
	int i;
	if (p == NULL) { return; }
	lk();
	i = reg_find(p);
	if (i < 0) { gSt.foreign++; ul(); free(p); return; }	// not ours (a path of the capture code that still uses malloc): a plain free
	h = gReg[i];
	if (h->magic != FB_MAGIC) { gSt.doubleRelease++; ul(); return; }
	if (--h->refs > 0) { ul(); return; }
	gSt.releases++;
	if (h->size == gSize && gNCache < KVM_FBUF_CACHE_MAX) { h->magic = FB_DEAD; gCache[gNCache++] = h; ul(); return; }
	gSt.frees++;
	gReg[i] = NULL;
	ul();
	free(h);
}

static void drop_cached(unsigned idx)
{
	int i;
	for (i = 0; i < FB_REG_MAX; i++) { if (gReg[i] == gCache[idx]) { gReg[i] = NULL; break; } }
	free(gCache[idx]);
	gSt.trimmed++;
}

// (lock held) the size of the pictures is now `size`: the cached buffers of another size are freed
static void set_size_locked(size_t size)
{
	unsigned i, k = 0;
	gSize = size;
	for (i = 0; i < gNCache; i++)
	{
		if (gCache[i]->size == size) { gCache[k++] = gCache[i]; } else { drop_cached(i); }
	}
	for (i = k; i < gNCache; i++) { gCache[i] = NULL; }
	gNCache = k;
}

void kvm_fbuf_set_size(size_t size)
{
	lk();
	set_size_locked(size);
	ul();
}

void kvm_fbuf_trim(void)
{
	unsigned i;
	lk();
	for (i = 0; i < gNCache; i++) { drop_cached(i); gCache[i] = NULL; }
	gNCache = 0;
	ul();
}

int kvm_fbuf_refs(const void *p)
{
	int i, r = -1;
	lk();
	i = reg_find(p);
	if (i >= 0 && gReg[i]->magic == FB_MAGIC) { r = gReg[i]->refs; }
	ul();
	return r;
}

void kvm_fbuf_free_all(void)
{
	int i;
	lk();
	for (i = 0; i < FB_REG_MAX; i++) { free(gReg[i]); gReg[i] = NULL; }
	memset(gCache, 0, sizeof(gCache));
	gNCache = 0;
	ul();
}

void kvm_fbuf_get_stats(kvm_fbuf_stats *st)
{
	lk();
	*st = gSt;
	st->cached = gNCache;
	ul();
}
