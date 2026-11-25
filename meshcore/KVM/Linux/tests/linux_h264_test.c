/*
Tests of linux_h264.c, the openh264 encoder of the Linux capture process (loaded with dlopen). Needs libopenh264.so.7 or .8 (the container of run-linux-h264-tests.sh has it).
With the argument "nolib" it checks the answers for a library that is not there, which has to be a process of its own (the load is made once per process).
With the argument "dump <dir>" it also writes the Annex B streams to files for the decoder check (ffmpeg, in the same script).
*/

#include "../linux_h264.h"
#include "../../common/kvm_annexb.h"
#include "../../common/kvm_video_core.h"
#include "../../common/kvm_yuv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gFail = 0, gChecks = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); gChecks++; if (_a != _b) { gFail++; printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

// a picture with something in it: a gradient, text-like stripes and a block that moves with `t`
static void picture(uint8_t *i420, int w, int h, int t)
{
	uint8_t *y = i420, *u = i420 + (size_t)w * h, *v = u + (size_t)(w / 2) * (h / 2);
	int x, r;
	for (r = 0; r < h; r++) { for (x = 0; x < w; x++) { y[(size_t)r * w + x] = (uint8_t)(16 + ((x + r) & 0x7f) + (((x / 3 + r / 5) & 1) ? 40 : 0)); } }
	for (r = 0; r < h / 2; r++) { for (x = 0; x < w / 2; x++) { u[(size_t)r * (w / 2) + x] = (uint8_t)(100 + (x & 31)); v[(size_t)r * (w / 2) + x] = (uint8_t)(140 - (r & 31)); } }
	for (r = 64; r < 128 && r < h; r++) { for (x = 32 + t * 16; x < 96 + t * 16 && x < w; x++) { y[(size_t)r * w + x] = (uint8_t)(200 - t); } }
}

static void write_file(const char *dir, const char *name, const uint8_t *p, size_t n)
{
	char path[512];
	FILE *f;
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "wb");
	if (f != NULL) { fwrite(p, 1, n, f); fclose(f); }
}

static void test_nolib(void)
{
	char why[200];
	int err = 0;
	CHECK_EQ(kvml_h264_load(why, sizeof(why)), KVML_H264_E_NOLIB);
	CHECK(strstr(why, "no libopenh264") != NULL);
	CHECK(kvml_h264_open(1024, 768, 66, 40, 28, 30, &err) == NULL);
	CHECK_EQ(err, KVML_H264_E_NOLIB);
	CHECK_EQ(kvml_h264_load(NULL, 0), KVML_H264_E_NOLIB);	// the same answer again
}

// one stream: the first frame is a key frame with SPS and PPS that the protocol's check accepts, the P frames follow, a forced key frame is one
static void run_stream(int w, int h, int profile, int level, const char *dump, const char *name)
{
	uint8_t *i420 = (uint8_t *)malloc(kvm_yuv_nv12_size(w, h));
	const size_t cap = 4u << 20;
	uint8_t *au = (uint8_t *)malloc(cap), *norm = (uint8_t *)malloc(cap), *all = (uint8_t *)malloc(cap * 4);
	size_t len = 0, nlen = 0, total = 0;
	int isIdr = 0, k, err = 0, fl = 0, lv = 0;
	kvml_h264 *e = kvml_h264_open(w, h, profile, level, 28, 30, &err);
	CHECK(e != NULL);
	if (e == NULL) { printf("open %dx%d profile %d level %d: error %d\n", w, h, profile, level, err); goto out; }
	CHECK_EQ(kvm_vcore_pick_level((unsigned)w, (unsigned)h, 0, &lv, &fl), 0);
	for (k = 0; k < 6; k++)
	{
		struct kvm_au_info info;
		struct kvm_sps_info sps;
		picture(i420, w, h, k);
		CHECK_EQ(kvml_h264_encode(e, i420, i420 + (size_t)w * h, i420 + (size_t)w * h + (size_t)(w / 2) * (h / 2), k == 4, au, cap, &len, &isIdr), KVML_H264_OK);
		CHECK(len > 8);
		CHECK_EQ(kvm_annexb_normalize(au, len, NULL, 0, norm, cap, &nlen, &info), KVM_ANNEXB_OK);
		CHECK_EQ(info.idr, (k == 0 || k == 4) ? 1 : 0);
		CHECK_EQ(isIdr, (k == 0 || k == 4) ? 1 : 0);
		CHECK_EQ(info.aud_removed, 0);
		if (isIdr)
		{
			CHECK_EQ(kvm_annexb_sps_info(norm, nlen, &sps), 0);
			CHECK_EQ(sps.profile, profile);
			CHECK_EQ(sps.level, level);
			CHECK_EQ(sps.numRefFrames, 1);
			CHECK_EQ(sps.mbsW * 16, w);
			CHECK_EQ(sps.mbsH * 16, h);
			CHECK_EQ(sps.cropping, 0);
			CHECK_EQ(sps.frameMbsOnly, 1);
			CHECK_EQ(sps.chromaFormat, 1);
			CHECK_EQ(sps.bitDepthLuma, 8);
			CHECK_EQ(kvm_vcore_check_sps(&sps, profile == 100 ? KVM_PROFILE_HIGH : profile == 77 ? KVM_PROFILE_MAIN : KVM_PROFILE_CBP, 0, (unsigned)w, (unsigned)h), 0);
		}
		if (total + nlen <= cap * 4) { memcpy(all + total, norm, nlen); total += nlen; }
	}
	if (dump != NULL) { write_file(dump, name, all, total); }
out:
	kvml_h264_close(e);
	free(i420); free(au); free(norm); free(all);
}

static void test_streams(const char *dump)
{
	char why[200];
	unsigned ma = 0, mi = 0, re = 0;
	CHECK_EQ(kvml_h264_load(why, sizeof(why)), KVML_H264_OK);
	printf("%s\n", why);
	kvml_h264_version(&ma, &mi, &re);
	CHECK_EQ(ma, 2);
	run_stream(1024, 768, 66, 31, dump, "cbp-1024x768.h264");
	run_stream(1920, 1088, 66, 40, dump, "cbp-1920x1088.h264");
	run_stream(1920, 1088, 77, 40, dump, "main-1920x1088.h264");
	run_stream(1920, 1088, 100, 40, dump, "high-1920x1088.h264");
	run_stream(3840, 2176, 100, 51, NULL, NULL);
	run_stream(32, 32, 66, 30, NULL, NULL);
}

static void test_params(void)
{
	int err = 0;
	CHECK(kvml_h264_open(1000, 768, 66, 40, 28, 30, &err) == NULL);		// not a multiple of 16
	CHECK_EQ(err, KVML_H264_E_PARAM);
	CHECK(kvml_h264_open(1024, 768, 88, 40, 28, 30, &err) == NULL);		// no extended profile
	CHECK(kvml_h264_open(1024, 768, 66, 99, 28, 30, &err) == NULL);		// not a level
	CHECK(kvml_h264_open(0, 0, 66, 40, 28, 30, &err) == NULL);
	kvml_h264_close(NULL);
	CHECK_EQ(kvml_h264_encode(NULL, NULL, NULL, NULL, 0, NULL, 0, NULL, NULL), KVML_H264_E_PARAM);
}

// the quantizer decides the size; a small output buffer is an error, not an overrun
static void test_qp_and_size(void)
{
	const int w = 1280, h = 720;
	uint8_t *i420 = (uint8_t *)malloc(kvm_yuv_nv12_size(w, h));
	const size_t cap = 4u << 20;
	uint8_t *au = (uint8_t *)malloc(cap), small[64];
	size_t lenLow = 0, lenHigh = 0, len = 0;
	int isIdr = 0, err = 0;
	kvml_h264 *e = kvml_h264_open(w, h, 100, 40, 20, 30, &err);
	CHECK(e != NULL);
	if (e != NULL)
	{
		picture(i420, w, h, 0);
		CHECK_EQ(kvml_h264_encode(e, i420, i420 + (size_t)w * h, i420 + (size_t)w * h + (size_t)(w / 2) * (h / 2), 0, au, cap, &lenLow, &isIdr), KVML_H264_OK);
		CHECK_EQ(kvml_h264_set_qp(e, 30), KVML_H264_OK);
		picture(i420, w, h, 1);
		CHECK_EQ(kvml_h264_encode(e, i420, i420 + (size_t)w * h, i420 + (size_t)w * h + (size_t)(w / 2) * (h / 2), 0, au, cap, &len, &isIdr), KVML_H264_OK);
		CHECK_EQ(isIdr, 0);							// a change of the quantizer does not make a key frame by itself
		CHECK_EQ(kvml_h264_set_qp(e, 40), KVML_H264_OK);
		CHECK_EQ(kvml_h264_encode(e, i420, i420 + (size_t)w * h, i420 + (size_t)w * h + (size_t)(w / 2) * (h / 2), 1, au, cap, &lenHigh, &isIdr), KVML_H264_OK);
		CHECK(isIdr);
		printf("key frame 1280x720: qp 20 %zu bytes, qp 40 %zu bytes\n", lenLow, lenHigh);
		CHECK(lenHigh < lenLow);
		CHECK_EQ(kvml_h264_encode(e, i420, i420 + (size_t)w * h, i420 + (size_t)w * h + (size_t)(w / 2) * (h / 2), 1, small, sizeof(small), &len, &isIdr), KVML_H264_E_SIZE);
		CHECK_EQ(len, 0);
		kvml_h264_close(e);
	}
	free(i420); free(au);
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "nolib") == 0) { test_nolib(); }
	else
	{
		test_streams(argc > 2 && strcmp(argv[1], "dump") == 0 ? argv[2] : NULL);
		test_params();
		test_qp_and_size();
	}
	printf("%d checks, %d failed\n", gChecks, gFail);
	return gFail == 0 ? 0 : 1;
}
