/*
KVM (Linux): the H.264 encoder of the capture process, openh264 loaded from the system. See linux_h264.h.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#include "linux_h264.h"
#include "openh264/codec_api.h"

#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The layouts of the structures that the library fills in or reads. They only grew at the end between the releases that are known (2.3 and 2.4 have the same layout, 2.6 and 2.7 have the
// PSNR fields), but SLayerBSInfo is the element of an array inside SFrameBSInfo, so its size decides where the frame type is (see KVML_LAYER_NEW). Which of the two the loaded release
// has is not taken from its version number (that guess was wrong for 2.4): the library writes the frame type at one of the two places, the buffer is zero at the other. The structures that the library writes (SFrameBSInfo) or reads (SSourcePicture) are in zeroed buffers that are larger than either layout, and read through the offsets of the
// release that is loaded; the ones of the headers in openh264/ (2.3.1) are the prefix of both.
#define KVML_BS_BYTES 16384
// The sizes are computed for the platform from the structure of the headers in the tree (2.3.1: sizeof) and the three floats of the newer one, rounded to its alignment: 40 and 56 on
// 64 bit, 28 and 40 on 32 bit (checked by compiling the headers of 2.3.1 and 2.6.0 for both).
#define KVML_LAYER_OLD (sizeof(SLayerBSInfo))
#define KVML_LAYER_NEW ((((sizeof(SLayerBSInfo) + (3u * sizeof(float))) + (__alignof__(SLayerBSInfo) - 1u)) / __alignof__(SLayerBSInfo)) * __alignof__(SLayerBSInfo))
#define KVML_SRC_BYTES 256
struct kvml_h264
{
	ISVCEncoder *enc;
	int w, h, qp;
	long long frame;
	unsigned char *bs;		// the SFrameBSInfo of the library
};

typedef int (*create_fn)(ISVCEncoder **);
typedef void (*destroy_fn)(ISVCEncoder *);
typedef void (*version_fn)(OpenH264Version *);

static struct
{
	int done, rc;
	void *lib;
	create_fn create;
	destroy_fn destroy;
	OpenH264Version ver;
	char why[160];
} gLib;
static int gFail = -1;	// -1: the environment has not been read yet

static int load_fail(int rc, const char *what, const char *detail)
{
	snprintf(gLib.why, sizeof(gLib.why), "%s%s%s", what, detail != NULL ? ": " : "", detail != NULL ? detail : "");
	gLib.rc = rc;
	if (gLib.lib != NULL) { dlclose(gLib.lib); gLib.lib = NULL; }
	return rc;
}

void kvml_h264_layer_sizes(size_t *oldSize, size_t *newSize)
{
	if (oldSize != NULL) { *oldSize = (size_t)KVML_LAYER_OLD; }
	if (newSize != NULL) { *newSize = (size_t)KVML_LAYER_NEW; }
}

int kvml_h264_load(char *why, size_t cap)
{
	if (!gLib.done)
	{
		static const char *const names[] = { "libopenh264.so.8", "libopenh264.so.7", NULL };
		const char *over = getenv("MESH_KVM_OPENH264");
		version_fn ver;
		const char *used = NULL;
		int i;
		gLib.done = 1;
		if (over != NULL && over[0] != 0) { gLib.lib = dlopen(over, RTLD_NOW | RTLD_LOCAL); used = over; }
		else
		{
			for (i = 0; names[i] != NULL && gLib.lib == NULL; i++) { gLib.lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL); used = names[i]; }
		}
		if (gLib.lib == NULL) { (void)load_fail(KVML_H264_E_NOLIB, "no libopenh264.so.7 or .8 on this system", over != NULL && over[0] != 0 ? over : NULL); goto out; }
		gLib.create = (create_fn)dlsym(gLib.lib, "WelsCreateSVCEncoder");
		gLib.destroy = (destroy_fn)dlsym(gLib.lib, "WelsDestroySVCEncoder");
		ver = (version_fn)dlsym(gLib.lib, "WelsGetCodecVersionEx");
		if (gLib.create == NULL || gLib.destroy == NULL || ver == NULL) { (void)load_fail(KVML_H264_E_SYMBOL, "libopenh264 lacks WelsCreateSVCEncoder, WelsDestroySVCEncoder or WelsGetCodecVersionEx", used); goto out; }
		memset(&gLib.ver, 0, sizeof(gLib.ver));
		ver(&gLib.ver);
		// The structures of the headers (2.3.1) are the prefix of those of 2.3 to 2.7 (see KVML_LAYER_NEW; 2.4 to 2.7 have the same layout: checked for 2.6.0 by running, for 2.7.0 by the
		// difference of the headers only). The upper limit is deliberate: a release that nobody has looked at may change a structure, and an unknown one answers with reason 2; the parameters are filled in by the library itself (GetDefaultParams, into a
		// buffer that is larger than ours). Another major version, or an older minor one, is not trusted with them
		if (gLib.ver.uMajor != 2 || gLib.ver.uMinor < 3 || gLib.ver.uMinor > 7)
		{
			char d[48];
			snprintf(d, sizeof(d), "%u.%u.%u", gLib.ver.uMajor, gLib.ver.uMinor, gLib.ver.uRevision);
			(void)load_fail(KVML_H264_E_VERSION, "libopenh264 version is not one this agent knows", d);
			goto out;
		}
		gLib.rc = KVML_H264_OK;
		snprintf(gLib.why, sizeof(gLib.why), "libopenh264 %u.%u.%u (%s)", gLib.ver.uMajor, gLib.ver.uMinor, gLib.ver.uRevision, used != NULL ? used : "?");
	}
out:
	if (why != NULL && cap > 0) { snprintf(why, cap, "%s", gLib.why); }
	return gLib.rc;
}

void kvml_h264_version(unsigned *major, unsigned *minor, unsigned *revision)
{
	if (major != NULL) { *major = gLib.rc == KVML_H264_OK ? gLib.ver.uMajor : 0; }
	if (minor != NULL) { *minor = gLib.rc == KVML_H264_OK ? gLib.ver.uMinor : 0; }
	if (revision != NULL) { *revision = gLib.rc == KVML_H264_OK ? gLib.ver.uRevision : 0; }
}

static int level_enum(int idc)
{
	switch (idc)
	{
		case 30: case 31: case 32: case 40: case 41: case 42: case 50: case 51: case 52: return idc;
		default: return 0;
	}
}

kvml_h264 *kvml_h264_open(int codedW, int codedH, int profile, int level, int qp, int fps, int *err)
{
	kvml_h264 *e;
	union { SEncParamExt p; unsigned char pad[4096]; } u;	// the library fills the structure in: room for a newer one that is longer than ours
	SEncParamExt *p = &u.p;
	if (err != NULL) { *err = KVML_H264_E_NOLIB; }
	if (kvml_h264_load(NULL, 0) != KVML_H264_OK) { return NULL; }
	if (err != NULL) { *err = KVML_H264_E_PARAM; }
	if (codedW < 16 || codedH < 16 || (codedW & 15) != 0 || (codedH & 15) != 0 || codedW > 8192 || codedH > 8192) { return NULL; }
	if (profile != 66 && profile != 77 && profile != 100) { return NULL; }
	if (level_enum(level) == 0) { return NULL; }
	if (qp < 0) { qp = 0; } else if (qp > 51) { qp = 51; }
	if (fps < 1) { fps = 1; } else if (fps > 60) { fps = 60; }
	e = (kvml_h264 *)calloc(1, sizeof(*e));
	if (e == NULL) { if (err != NULL) { *err = KVML_H264_E_MEMORY; } return NULL; }
	e->bs = (unsigned char *)calloc(1, KVML_BS_BYTES);
	if (e->bs == NULL) { free(e); if (err != NULL) { *err = KVML_H264_E_MEMORY; } return NULL; }
	if (gLib.create(&e->enc) != 0 || e->enc == NULL) { free(e->bs); free(e); if (err != NULL) { *err = KVML_H264_E_PARAM; } return NULL; }
	{
		int trace = 0;											// WELS_LOG_QUIET (before the parameters are checked): the library does not write to the stderr of the capture process
		(void)(*e->enc)->SetOption(e->enc, ENCODER_OPTION_TRACE_LEVEL, &trace);
	}
	memset(&u, 0, sizeof(u));
	if ((*e->enc)->GetDefaultParams(e->enc, p) != 0) { goto fail; }
	p->iUsageType = SCREEN_CONTENT_REAL_TIME;
	p->iPicWidth = codedW;
	p->iPicHeight = codedH;
	p->iTargetBitrate = 8000000;							// (not used with the rate control off; the level limits what it may be)
	p->iRCMode = RC_OFF_MODE;
	p->fMaxFrameRate = (float)fps;
	p->iTemporalLayerNum = 1;
	p->iSpatialLayerNum = 1;
	p->iComplexityMode = LOW_COMPLEXITY;
	p->uiIntraPeriod = 0;									// a key frame is the video code's decision (the first frame, op 3, a lost access unit)
	p->iNumRefFrame = 1;									// the SPS check of the protocol wants one reference frame
	p->eSpsPpsIdStrategy = CONSTANT_ID;
	p->bPrefixNalAddingCtrl = false;
	p->bEnableSSEI = false;
	p->iPaddingFlag = 0;
	p->iEntropyCodingModeFlag = (profile >= 77) ? 1 : 0;
	p->bEnableFrameSkip = false;
	p->iMaxQp = 51;
	p->iMinQp = 0;
	p->iMultipleThreadIdc = 1;								// the capture loop is one thread, and a frame that is made by several is not faster on the few tiles of a motion
	p->bEnableDenoise = false;
	p->bEnableBackgroundDetection = false;
	p->bEnableAdaptiveQuant = false;
	p->bEnableFrameCroppingFlag = false;					// the coded size is the size of the picture (multiples of 32): no cropping in the SPS (3.4)
	p->bEnableSceneChangeDetect = false;
	p->sSpatialLayers[0].iVideoWidth = codedW;
	p->sSpatialLayers[0].iVideoHeight = codedH;
	p->sSpatialLayers[0].fFrameRate = (float)fps;
	p->sSpatialLayers[0].iSpatialBitrate = p->iTargetBitrate;
	p->sSpatialLayers[0].iMaxSpatialBitrate = 0;
	p->sSpatialLayers[0].uiProfileIdc = (EProfileIdc)profile;
	p->sSpatialLayers[0].uiLevelIdc = (ELevelIdc)level_enum(level);
	p->sSpatialLayers[0].iDLayerQp = qp;
	p->sSpatialLayers[0].sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;
	p->sSpatialLayers[0].bVideoSignalTypePresent = true;		// BT.709, limited range: what kvm_yuv.c converts to
	p->sSpatialLayers[0].uiVideoFormat = 5;
	p->sSpatialLayers[0].bFullRange = false;
	p->sSpatialLayers[0].bColorDescriptionPresent = true;
	p->sSpatialLayers[0].uiColorPrimaries = 1;
	p->sSpatialLayers[0].uiTransferCharacteristics = 1;
	p->sSpatialLayers[0].uiColorMatrix = 1;
	if ((*e->enc)->InitializeExt(e->enc, p) != 0) { goto fail; }
	e->w = codedW; e->h = codedH; e->qp = qp;
	if (err != NULL) { *err = KVML_H264_OK; }
	return e;
fail:
	gLib.destroy(e->enc);
	free(e->bs);
	free(e);
	return NULL;
}

void kvml_h264_close(kvml_h264 *e)
{
	if (e == NULL) { return; }
	if (e->enc != NULL) { (*e->enc)->Uninitialize(e->enc); gLib.destroy(e->enc); }
	free(e->bs);
	free(e);
}

int kvml_h264_set_qp(kvml_h264 *e, int qp)
{
	if (e == NULL) { return KVML_H264_E_PARAM; }
	if (qp < 0) { qp = 0; } else if (qp > 51) { qp = 51; }
	if (qp == e->qp) { return KVML_H264_OK; }
	{
		// a change of the quantizer of the layer is a change of the parameters: read, change, set (the library keeps the stream running when only the quantizer changed)
		union { SEncParamExt p; unsigned char pad[4096]; } u;
		memset(&u, 0, sizeof(u));
		if ((*e->enc)->GetOption(e->enc, ENCODER_OPTION_SVC_ENCODE_PARAM_EXT, &u.p) != 0) { return KVML_H264_E_PARAM; }
		u.p.sSpatialLayers[0].iDLayerQp = qp;
		if ((*e->enc)->SetOption(e->enc, ENCODER_OPTION_SVC_ENCODE_PARAM_EXT, &u.p) != 0) { return KVML_H264_E_PARAM; }
	}
	e->qp = qp;
	return KVML_H264_OK;
}

int kvml_h264_encode(kvml_h264 *e, const uint8_t *y, const uint8_t *u, const uint8_t *v, int forceIdr, uint8_t *out, size_t cap, size_t *len, int *isIdr)
{
	union { SSourcePicture p; unsigned char pad[KVML_SRC_BYTES]; } us;
	SSourcePicture *src = &us.p;
	size_t total = 0;
	int l, rc, layers;
	size_t stride = (size_t)KVML_LAYER_OLD;
	const unsigned char *frameType;
	if (len != NULL) { *len = 0; }
	if (isIdr != NULL) { *isIdr = 0; }
	if (gFail < 0) { const char *f = getenv("MESH_KVM_H264_FAIL"); gFail = (f != NULL && f[0] != 0 && f[0] != '0') ? 1 : 0; }
	if (gFail) { return KVML_H264_E_ENCODE; }	// MESH_KVM_H264_FAIL=1: the encoder fails at every frame (the tests of the way back to JPEG)
	if (e == NULL || y == NULL || u == NULL || v == NULL || out == NULL) { return KVML_H264_E_PARAM; }
	memset(&us, 0, sizeof(us));
	memset(e->bs, 0, KVML_BS_BYTES);
	src->iColorFormat = videoFormatI420;
	src->iPicWidth = e->w;
	src->iPicHeight = e->h;
	src->iStride[0] = e->w;
	src->iStride[1] = e->w / 2;
	src->iStride[2] = e->w / 2;
	src->pData[0] = (unsigned char *)y;
	src->pData[1] = (unsigned char *)u;
	src->pData[2] = (unsigned char *)v;
	src->uiTimeStamp = (long long)(e->frame * 33);
	if (forceIdr) { (void)(*e->enc)->ForceIntraFrame(e->enc, true); }
	rc = (*e->enc)->EncodeFrame(e->enc, src, (SFrameBSInfo *)(void *)e->bs);
	e->frame++;
	// SFrameBSInfo: iLayerNum, the layers (sizeof depends on the release), then the frame type
	memcpy(&layers, e->bs, sizeof(layers));
	{
		EVideoFrameType ft, ftNew;
		frameType = e->bs + offsetof(SFrameBSInfo, sLayerInfo) + ((size_t)MAX_LAYER_NUM_OF_FRAME * stride);
		memcpy(&ft, frameType, sizeof(ft));
		memcpy(&ftNew, e->bs + offsetof(SFrameBSInfo, sLayerInfo) + ((size_t)MAX_LAYER_NUM_OF_FRAME * (size_t)KVML_LAYER_NEW), sizeof(ftNew));
		if ((ft == videoFrameTypeInvalid) && (ftNew != videoFrameTypeInvalid)) { ft = ftNew; stride = (size_t)KVML_LAYER_NEW; }	// the layout with the PSNR fields
		if (rc != cmResultSuccess || ft == videoFrameTypeInvalid || ft == videoFrameTypeSkip || layers < 1 || layers > MAX_LAYER_NUM_OF_FRAME) { return KVML_H264_E_ENCODE; }
		if (isIdr != NULL) { *isIdr = (ft == videoFrameTypeIDR) ? 1 : 0; }
	}
	for (l = 0; l < layers; l++)
	{
		SLayerBSInfo lay;
		int n, bytes = 0;
		memcpy(&lay, e->bs + offsetof(SFrameBSInfo, sLayerInfo) + ((size_t)l * stride), sizeof(lay));	// (the prefix of the layer of any release)
		if (lay.iNalCount < 0 || lay.iNalCount > 128 || lay.pNalLengthInByte == NULL || lay.pBsBuf == NULL) { if (isIdr != NULL) { *isIdr = 0; } return KVML_H264_E_ENCODE; }
		for (n = 0; n < lay.iNalCount; n++) { bytes += lay.pNalLengthInByte[n]; }
		if (bytes < 0 || total + (size_t)bytes > cap) { if (isIdr != NULL) { *isIdr = 0; } return KVML_H264_E_SIZE; }
		memcpy(out + total, lay.pBsBuf, (size_t)bytes);
		total += (size_t)bytes;
	}
	if (total == 0) { if (isIdr != NULL) { *isIdr = 0; } return KVML_H264_E_ENCODE; }
	if (len != NULL) { *len = total; }
	return KVML_H264_OK;
}
