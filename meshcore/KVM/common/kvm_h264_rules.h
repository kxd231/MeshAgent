/*
KVM: offline check of an H.264 Annex B stream against the rules of the viewer (portable C99, no API of the operating system).

The viewer (public/scripts/agent-desktop-h264.js: analyse() and checkSps() on top of avc420.js) refuses a picture that breaks one of the rules below, the agent
(kvm_vcore_check_sps) refuses an SPS that the viewer would refuse. A hardware encoder gives no guarantee for any of them. This module runs the same rules on the
access units that an encoder gave, one verdict per rule (PASS / FAIL / WARN), so that a stream can be judged without a viewer: the hardware probe of
kvm_h264_check (--hw), and the test of the rules with generated and ffmpeg streams.

The rules are numbered; kvmh_rule_id() / kvmh_rule_text() name them. Every rule keeps the number of access units it was checked on, the number that broke it and
the first detail text. The SPS fields that kvm_annexb_sps_info() reads are used as they are (the agent decides with the same function); the fields it does not
read (sps id, picture order count type, VUI bitstream restriction) come from a second parser of this file, and a disagreement between the two on the fields they
share is reported as a failure of the rule "parse" (kvm_annexb_sps_info used to drop the second 03 of 00 00 03 03 and misread such an SPS; fixed, see kvm_annexb_test.c).
*/

#ifndef KVM_H264_RULES_H
#define KVM_H264_RULES_H

#include <stddef.h>
#include <stdint.h>

#include "kvm_annexb.h"

#ifdef __cplusplus
extern "C" {
#endif

enum
{
	KVMH_R_ANNEXB = 0,		/* the access unit is an Annex B byte stream (3 or 4 byte start codes), not length prefixed (AVCC) */
	KVMH_R_SLICES,			/* at least one slice NAL unit; slices of an IDR picture and of a non-IDR picture are not mixed */
	KVMH_R_IDRFLAG,			/* the IDR flag that the caller expects (when it gave one) matches the slices; the first picture is an IDR */
	KVMH_R_IDR_PS,			/* an IDR picture has an SPS and a PPS in front of its first slice */
	KVMH_R_NALTYPES,		/* no data partitioning (NAL 2 to 4) and no SVC / MVC NAL units (14, 15, 20) */
	KVMH_R_PSSIZE,			/* every SPS and PPS at most 1024 bytes, at most 4 PPS in a picture, the SPS of one picture all identical */
	KVMH_R_PROFILE,			/* profile_idc 66 (only with constraint_set1), 77 or 100 */
	KVMH_R_LEVEL,			/* level_idc is a level of the standard up to 5.2 (9 = 1b of High) and not above the viewer's limit */
	KVMH_R_FORMAT,			/* 4:2:0, 8 bit luma and chroma, frame_mbs_only_flag = 1 (progressive) */
	KVMH_R_REFS,			/* max_num_ref_frames <= 1 */
	KVMH_R_GEOMETRY,		/* no frame cropping; the coded size is the expected one; at most 36864 macroblocks and 8192 samples per side */
	KVMH_R_NOB,				/* no B slice (slice_type 1 or 6) in any picture */
	KVMH_R_SPSSTABLE,		/* an SPS in a non-IDR picture is identical to the running one; a PPS refers to the SPS in use */
	KVMH_R_VUI,				/* (optional, WARN only) VUI bitstream_restriction with num_reorder_frames = 0 and max_dec_frame_buffering <= 1 */
	KVMH_R_PARSE,			/* the SPS, PPS and slice headers parse to the end and the two SPS parsers agree */
	KVMH_NRULES
};

enum { KVMH_NOTRUN = 0, KVMH_PASS = 1, KVMH_FAIL = 2, KVMH_WARN = 3 };

struct kvmh_rule
{
	int status;				/* KVMH_*: valid after kvmh_finish() */
	unsigned checks;		/* access units the rule was applied to */
	unsigned fails;			/* access units that broke it */
	char first[96];			/* detail of the first failure ("" if none) */
};

struct kvmh_sps_extra
{
	int spsId, pocType;
	int vui, restriction;	/* VUI present, bitstream_restriction_flag */
	int reorder, buffering;	/* num_reorder_frames, max_dec_frame_buffering (-1 when there is no restriction) */
	int refs, mbsW, mbsH, frameMbsOnly, cropping;	/* the fields that both parsers read (compared with kvm_annexb_sps_info) */
	int signal, fullRange, colourDesc;	/* VUI video_signal_type_present_flag, video_full_range_flag, colour_description_present_flag */
	int primaries, transfer, matrix;	/* colour_primaries, transfer_characteristics, matrix_coefficients (2 = unspecified when there is no colour description) */
};

struct kvmh_check
{
	unsigned codedW, codedH;	/* expected coded size (0 = not checked) */
	int viewerMaxLevel;			/* highest level_idc the viewer accepts, 0 = 52 */
	struct kvmh_rule r[KVMH_NRULES];
	/* what was seen */
	unsigned aus, idrs, pAus, bSlices, spsSeen, spsChanges, ppsSeen, auBytesMax;
	int haveRun;				/* a running SPS exists (the SPS of the last IDR picture) */
	uint8_t run[1100];
	size_t runLen;
	int runSpsId;
	struct kvm_sps_info sps;	/* of the last SPS (kvm_annexb_sps_info) */
	struct kvmh_sps_extra extra;
	int haveSps;
};

/* codedW / codedH: the picture size the stream is expected to have (the padded size the encoder was given), 0 to skip that check. viewerMaxLevel 0 = 52. */
void kvmh_init(struct kvmh_check *c, unsigned codedW, unsigned codedH, int viewerMaxLevel);

/* One access unit. wantIdr: 1 / 0 = the caller knows whether it has to be an IDR picture, -1 = not known (only the first picture has to be one). Returns the number of
   rules that this access unit broke (0 = it passes every rule that applies to it). */
int kvmh_au(struct kvmh_check *c, const uint8_t *au, size_t n, int wantIdr);

/* Sets the final status of every rule (PASS when it was checked and never broke, FAIL, WARN for the optional one, NOTRUN when it never applied). Returns the number of FAIL. */
int kvmh_finish(struct kvmh_check *c);

const char *kvmh_rule_id(int rule);		/* short name, e.g. "annexb" */
const char *kvmh_rule_text(int rule);	/* the rule in words */
const char *kvmh_status_name(int status);	/* "PASS", "FAIL", "WARN", "n/a" */

/* The end of the access unit that starts at p[start]: the offset of the first byte of the next one, or n. A new access unit begins at an AUD, at an SPS / PPS / SEI after a
   slice, or at a slice with first_mb_in_slice = 0 after a slice. Works on a stream without AUD (x264, hardware encoders). */
size_t kvmh_next_au(const uint8_t *p, size_t n, size_t start);

/* What kvmh_au() found in one access unit, without judging it (for the tool's tables). types: bit n set when a NAL unit of type n is present (n < 32). */
struct kvmh_au_info
{
	int annexb;				/* starts with a start code */
	int idr;				/* the first slice is type 5 */
	int slices, bSlices;
	int nSps, nPps;
	uint32_t types;
};
void kvmh_au_info(const uint8_t *au, size_t n, struct kvmh_au_info *o);

/* The colour that the SPS of a stream signals against what the agent sends (BT.709, limited range: kvm_yuv.h). KVMH_COLOUR_BT709: signalled and equal; KVMH_COLOUR_UNSPECIFIED: not signalled
   (or primaries / transfer 0 or 2 and matrix 2: reserved / unspecified, as the AMD encoder writes them 0 / 0 / 2; the viewer sets the colour space of its decoder itself, so this is allowed and only noted); KVMH_COLOUR_WRONG: full range or another colour space (the hardware encoder self-test: the hardware encoder is refused). */
enum { KVMH_COLOUR_BT709 = 0, KVMH_COLOUR_UNSPECIFIED = 1, KVMH_COLOUR_WRONG = -1 };
int kvmh_colour_verdict(const struct kvmh_sps_extra *x);

/* Writes the NAL types of an access unit ("7 8 5") into dst (at most 12 of them). */
void kvmh_nal_list(const uint8_t *au, size_t n, char *dst, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* KVM_H264_RULES_H */
