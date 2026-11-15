/*
Tests of kvm_stream.c: the commands 240-243 and the end-to-end ACK window. Time is injected, so every scenario is deterministic.
Run with run-kvm-stream-tests.sh (gcc in a container: plain build with -Wall -Wextra -Werror, then AddressSanitizer + UndefinedBehaviorSanitizer).
The vector table kvm_stream_vectors.h is generated from test/unit/fixtures/kvm-h264/vectors.json of the MeshCentral fork (gen_stream_vectors.py).
*/

#include "../kvm_stream.h"
#include "kvm_stream_vectors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MNG_MOUSE 2
#define MNG_KEY 1
static int gFail = 0, gChecks = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); gChecks++; if (_a != _b) { gFail++; printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

// ---------------------------------------------------------------------------------------------------------------------------------------
// vectors.json
// ---------------------------------------------------------------------------------------------------------------------------------------

static int hexval(char c) { return (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1; }

static void test_vectors(void)
{
	size_t i, nvalid = 0, ninvalid = 0;
	for (i = 0; i < KV_COUNT; i++)
	{
		const kv_vector *v = &kVectors[i];
		kvm_rej r = kvm_stream_validate(v->cmd, v->bytes, v->len, v->screenW, v->screenH);
		const char *want = v->expect[0] ? v->expect : "NONE";
		if (strcmp(kvm_stream_rej_name(r), want) != 0) { printf("vector %s: got %s, expected %s\n", v->name, kvm_stream_rej_name(r), want); }
		CHECK(strcmp(kvm_stream_rej_name(r), want) == 0);
		if (v->expect[0]) { ninvalid++; continue; }
		nvalid++;
		// the fields of a valid vector, in the order of gen_stream_vectors.py
		if (v->cmd == 240)
		{
			kvm_ctrl c;
			CHECK_EQ(kvm_stream_parse_ctrl(v->bytes, v->len, &c), KVM_REJ_NONE);
			CHECK_EQ(c.ver, v->f[0]); CHECK_EQ(c.op, v->f[1]); CHECK_EQ(c.flags, v->f[2]); CHECK_EQ(c.maxLevelIdc, v->f[3]);
			CHECK_EQ(c.maxW, v->f[4]); CHECK_EQ(c.maxH, v->f[5]); CHECK_EQ((long long)c.reserved, v->f[6]);
			{ uint8_t out[16]; kvm_stream_build_ctrl(out, &c); CHECK(memcmp(out, v->bytes, 16) == 0); }	// build is the inverse of parse
		}
		else if (v->cmd == 241)
		{
			kvm_ack a;
			CHECK_EQ(kvm_stream_parse_ack(v->bytes, v->len, &a), KVM_REJ_NONE);
			CHECK_EQ((long long)a.seq, v->f[0]); CHECK_EQ((long long)a.ts, v->f[1]); CHECK_EQ(a.queueMs, v->f[2]); CHECK_EQ(a.flags, v->f[3]);
			{ uint8_t out[16]; kvm_stream_build_ack(out, &a); CHECK(memcmp(out, v->bytes, 16) == 0); }
		}
		else if (v->cmd == 242)
		{
			kvm_state_msg m;
			CHECK_EQ(kvm_stream_parse_state(v->bytes, v->len, &m), KVM_REJ_NONE);
			CHECK_EQ(m.ver, v->f[0]); CHECK_EQ(m.video, v->f[1]); CHECK_EQ(m.ack, v->f[2]); CHECK_EQ(m.reason, v->f[3]); CHECK_EQ(m.profile, v->f[4]);
			CHECK_EQ(m.level, v->f[5]); CHECK_EQ(m.codedW, v->f[6]); CHECK_EQ(m.codedH, v->f[7]); CHECK_EQ(m.caps, v->f[8]); CHECK_EQ(m.gen, v->f[9]);
			{ uint8_t out[16]; kvm_stream_build_state(out, &m); CHECK(memcmp(out, v->bytes, 16) == 0); }
		}
		else if (v->cmd == 243)
		{
			kvm_frame f;
			int k;
			CHECK_EQ(kvm_stream_parse_frame(v->bytes, v->len, v->screenW, v->screenH, &f), KVM_REJ_NONE);
			CHECK_EQ((long long)f.n, v->f[0]); CHECK_EQ((long long)f.seq, v->f[1]); CHECK_EQ((long long)f.ts, v->f[2]); CHECK_EQ(f.flags, v->f[3]);
			CHECK_EQ(f.gen, v->f[4]); CHECK_EQ(f.codedW, v->f[5]); CHECK_EQ(f.codedH, v->f[6]); CHECK_EQ(f.nRects, v->f[7]); CHECK_EQ(f.nRects, v->nRects);
			CHECK_EQ(f.annexLen, strlen(v->annexHex) / 2);
			for (k = 0; k < v->nRects && k < KV_MAX_RECTS && k < (int)f.nRects; k++)
			{
				const uint8_t *r = f.rects + (8 * k);
				CHECK_EQ((r[0] << 8) | r[1], v->rects[k][0]); CHECK_EQ((r[2] << 8) | r[3], v->rects[k][1]);
				CHECK_EQ((r[4] << 8) | r[5], v->rects[k][2]); CHECK_EQ((r[6] << 8) | r[7], v->rects[k][3]);
			}
			for (k = 0; (size_t)k < f.annexLen && f.annexB != NULL; k++)
			{
				CHECK_EQ(f.annexB[k], (hexval(v->annexHex[2 * k]) << 4) | hexval(v->annexHex[(2 * k) + 1]));
			}
			if (f.flags & KVM_FRAME_MARKER) { CHECK(f.rects == NULL && f.annexB == NULL); }
			// build is the inverse of parse: the same message without the padding after 8 + N
			{
				uint8_t out[512];
				kvm_frame_params p;
				uint16_t rects[16];
				int q;
				memset(&p, 0, sizeof(p));
				p.seq = f.seq; p.ts = f.ts; p.flags = f.flags; p.gen = f.gen; p.codedW = f.codedW; p.codedH = f.codedH; p.nRects = f.nRects;
				for (q = 0; q < (int)f.nRects * 4 && q < 16; q++) { rects[q] = (uint16_t)((f.rects[2 * q] << 8) | f.rects[(2 * q) + 1]); }
				p.rects = rects; p.annexB = f.annexB; p.annexLen = f.annexLen;
				{
					size_t n = kvm_stream_build_frame(out, sizeof(out), &p);
					CHECK_EQ(n, 8 + f.n);
					CHECK(n == 8 + f.n && memcmp(out, v->bytes, n) == 0);
				}
			}
		}
	}
	CHECK_EQ(nvalid, KV_VALID);
	CHECK(ninvalid >= 30);
	printf("vectors: %zu valid, %zu invalid\n", nvalid, ninvalid);
}

// The generators of vectors.json that are too big for the table
static void test_generators(void)
{
	// frame_256_rects / frame_257_rects: screen 1920x1080, coded 1920x1088, P frame, gen 1, seq 1, 60 rects per row of 32x32
	{
		static const uint8_t annex[] = { 0, 0, 0, 1, 0x41, 0x9a, 0, 0, 0 };
		uint16_t rects[257 * 4];
		uint8_t buf[8 + 20 + (257 * 8) + 9];
		kvm_frame_params p;
		size_t n;
		unsigned i;
		for (i = 0; i < 257; i++) { rects[4 * i] = (uint16_t)((i % 60) * 32); rects[(4 * i) + 1] = (uint16_t)((i / 60) * 32); rects[(4 * i) + 2] = 32; rects[(4 * i) + 3] = 32; }
		memset(&p, 0, sizeof(p));
		p.seq = 1; p.gen = 1; p.codedW = 1920; p.codedH = 1088; p.rects = rects; p.annexB = annex; p.annexLen = sizeof(annex);
		p.nRects = 256;
		n = kvm_stream_build_frame(buf, sizeof(buf), &p);
		CHECK_EQ(n, 8 + 20 + (256 * 8) + 9);
		CHECK_EQ(buf[7], (20 + (256 * 8) + 9) & 255);	// N = 20 + 256*8 + 9
		CHECK_EQ(kvm_stream_parse_frame(buf, n, 1920, 1080, NULL), KVM_REJ_NONE);
		p.nRects = 257;
		CHECK_EQ(kvm_stream_build_frame(buf, sizeof(buf), &p), 0);	// the builder refuses it ...
		{	// ... so build the bytes by hand: the parser must say BAD_RECT_COUNT before it looks at the data
			uint8_t raw[8 + 20 + (257 * 8) + 9];
			memset(raw, 0, sizeof(raw));
			p.nRects = 256;
			(void)kvm_stream_build_frame(raw, sizeof(raw), &p);
			raw[27] = 1; raw[26] = 1;	// nRects = 257
			raw[7] = (uint8_t)((20 + (257 * 8) + 9) & 255); raw[6] = (uint8_t)((20 + (257 * 8) + 9) >> 8);
			CHECK_EQ(kvm_stream_parse_frame(raw, sizeof(raw), 1920, 1080, NULL), KVM_REJ_BAD_RECT_COUNT);
		}
	}
	// frame_N_exactly_8MiB (valid) and frame_N_8MiB_plus_1 (FRAME_TOO_BIG, message with the data of N = 8 MiB)
	{
		size_t total = 8u + KVM_FRAME_MAX_N;
		uint8_t *big = (uint8_t *)calloc(total, 1);
		static const uint16_t rect[4] = { 0, 0, 32, 32 };
		kvm_frame_params p;
		size_t annex = KVM_FRAME_MAX_N - 20 - 8;
		uint8_t *an = (uint8_t *)calloc(annex, 1);
		CHECK(big != NULL && an != NULL);
		if (big != NULL && an != NULL)
		{
			memset(&p, 0, sizeof(p));
			p.seq = 1; p.gen = 1; p.codedW = 1920; p.codedH = 1088; p.nRects = 1; p.rects = rect;
			an[3] = 1; an[4] = 0x41;
			p.annexB = an; p.annexLen = annex;
			CHECK_EQ(kvm_stream_build_frame(big, total, &p), total);
			CHECK_EQ(kvm_stream_parse_frame(big, total, 1920, 1080, NULL), KVM_REJ_NONE);
			p.annexLen = annex + 1;
			CHECK_EQ(kvm_stream_build_frame(big, total + 1, &p), 0);	// 8 MiB + 1 is refused by the builder as well
			big[4] = 0x00; big[5] = 0x80; big[6] = 0x00; big[7] = 0x01;	// N = 8 MiB + 1, the data of N = 8 MiB: the N check comes before the length check
			CHECK_EQ(kvm_stream_parse_frame(big, total, 1920, 1080, NULL), KVM_REJ_FRAME_TOO_BIG);
		}
		free(big); free(an);
	}
	// seq_wrap: 0xFFFFFFFE, 0xFFFFFFFF, 1, 2 are gapless; after 0xFFFFFFFF, seq 0 is not valid (BAD_SEQ in the parser) and seq 2 is a gap
	CHECK_EQ(kvm_stream_seq_next(0xFFFFFFFEu), 0xFFFFFFFFu);
	CHECK_EQ(kvm_stream_seq_next(0xFFFFFFFFu), 1);
	CHECK_EQ(kvm_stream_seq_next(1), 2);
	CHECK(!kvm_stream_seq_is_gap(1, 0xFFFFFFFEu, 0xFFFFFFFFu, 0));
	CHECK(!kvm_stream_seq_is_gap(1, 0xFFFFFFFFu, 1, 0));
	CHECK(!kvm_stream_seq_is_gap(1, 1, 2, 0));
	CHECK(kvm_stream_seq_is_gap(1, 0xFFFFFFFFu, 2, 0));
	CHECK(kvm_stream_seq_is_gap(1, 0xFFFFFFFFu, 0, 0));
	// seq_gap: 10, 11, 13: the third is a gap; an IDR with seq 500 after 13 is accepted; the first position after a 242 is never a gap
	CHECK(!kvm_stream_seq_is_gap(1, 10, 11, 0));
	CHECK(kvm_stream_seq_is_gap(1, 11, 13, 0));
	CHECK(!kvm_stream_seq_is_gap(1, 13, 500, 1));
	CHECK(!kvm_stream_seq_is_gap(0, 0, 77, 0));
	// padding to 32
	CHECK_EQ(kvm_stream_pad32(1080), 1088); CHECK_EQ(kvm_stream_pad32(1920), 1920); CHECK_EQ(kvm_stream_pad32(1366), 1376); CHECK_EQ(kvm_stream_pad32(1), 32);
}

// A few cases of the parsers that are not in the vectors
static void test_parsers_extra(void)
{
	uint8_t b[16];
	kvm_ctrl c;
	kvm_ack a;
	kvm_state_msg m;
	kvm_frame f;
	uint8_t mk[KVM_MARKER_BYTES];
	memset(&c, 0, sizeof(c));
	c.ver = 1; c.op = KVM_OP_ACK_ONLY;
	kvm_stream_build_ctrl(b, &c);
	CHECK_EQ(kvm_stream_parse_ctrl(b, 16, NULL), KVM_REJ_NONE);
	CHECK_EQ(kvm_stream_parse_ctrl(b, 3, NULL), KVM_REJ_TRUNCATED);
	CHECK_EQ(kvm_stream_parse_ctrl(NULL, 16, NULL), KVM_REJ_TRUNCATED);
	CHECK_EQ(kvm_stream_parse_ctrl(b, 0, NULL), KVM_REJ_TRUNCATED);
	CHECK_EQ(kvm_stream_parse_ack(b, 16, &a), KVM_REJ_BAD_OP);	// a 240 given to the 241 parser
	b[3] = 0; CHECK_EQ(kvm_stream_parse_ctrl(b, 16, NULL), KVM_REJ_BAD_SIZE); b[3] = 16;
	b[2] = 1; CHECK_EQ(kvm_stream_parse_ctrl(b, 16, NULL), KVM_REJ_BAD_SIZE); b[2] = 0;
	// op 2 and op 4: flags, level and limits are reserved and ignored
	c.op = KVM_OP_VIDEO_OFF; c.flags = 0xFF; c.maxW = 60000; c.maxH = 60000; c.maxLevelIdc = 99;
	kvm_stream_build_ctrl(b, &c);
	CHECK_EQ(kvm_stream_parse_ctrl(b, 16, NULL), KVM_REJ_NONE);
	// op 1 limits: 8192 passes, 8193 does not
	c.op = KVM_OP_VIDEO_ON; c.flags = 1; c.maxW = 8192; c.maxH = 8192;
	kvm_stream_build_ctrl(b, &c); CHECK_EQ(kvm_stream_parse_ctrl(b, 16, NULL), KVM_REJ_NONE);
	c.maxH = 8193; kvm_stream_build_ctrl(b, &c); CHECK_EQ(kvm_stream_parse_ctrl(b, 16, NULL), KVM_REJ_BAD_LIMIT);
	// op 1 with flags 8 only (a reserved bit, no profile): BAD_FLAGS
	c.maxH = 0; c.flags = 8; kvm_stream_build_ctrl(b, &c); CHECK_EQ(kvm_stream_parse_ctrl(b, 16, NULL), KVM_REJ_BAD_FLAGS);
	// op 6 with flags 4: BAD_FLAGS
	c.op = KVM_OP_VETO_END; c.flags = 4; kvm_stream_build_ctrl(b, &c); CHECK_EQ(kvm_stream_parse_ctrl(b, 16, NULL), KVM_REJ_BAD_FLAGS);
	// 242 parse
	memset(&m, 0, sizeof(m)); m.ver = 1; m.caps = 2;
	kvm_stream_build_state(b, &m); CHECK_EQ(kvm_stream_parse_state(b, 16, &m), KVM_REJ_NONE);
	// the marker: 28 bytes, parses in any screen, also without a screen (marker comes before NO_SCREEN)
	kvm_stream_build_marker(mk, 5, 1234, 3);
	CHECK_EQ(kvm_stream_parse_frame(mk, sizeof(mk), 0, 0, &f), KVM_REJ_NONE);
	CHECK(f.seq == 5 && f.ts == 1234 && f.gen == 3 && f.n == 20 && (f.flags & KVM_FRAME_MARKER));
	CHECK_EQ(kvm_stream_parse_frame(mk, sizeof(mk) - 1, 0, 0, NULL), KVM_REJ_TRUNCATED);
	mk[4] = 0; mk[5] = 0; mk[6] = 0; mk[7] = 19; CHECK_EQ(kvm_stream_parse_frame(mk, sizeof(mk), 0, 0, NULL), KVM_REJ_BAD_SIZE);
	// a JUMBO whose inner command is not 243
	kvm_stream_build_marker(mk, 5, 1234, 3); mk[9] = 3; CHECK_EQ(kvm_stream_parse_frame(mk, sizeof(mk), 1920, 1080, NULL), KVM_REJ_NOT_243);
	// robustness: every prefix of a valid frame and of every vector is rejected or accepted without reading outside the buffer (ASan checks it)
	{
		size_t i, k;
		for (i = 0; i < KV_COUNT; i++)
		{
			uint8_t *copy = (uint8_t *)malloc(kVectors[i].len ? kVectors[i].len : 1);
			if (copy == NULL) { continue; }
			for (k = 0; k <= kVectors[i].len; k++)
			{
				uint8_t *exact = (uint8_t *)malloc(k ? k : 1);	// a buffer of exactly k bytes, so that ASan sees an overread
				if (exact == NULL) { continue; }
				memcpy(exact, kVectors[i].bytes, k);
				(void)kvm_stream_validate(kVectors[i].cmd, exact, k, kVectors[i].screenW, kVectors[i].screenH);
				free(exact);
			}
			free(copy);
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// state machine (5.1, 5.2)
// ---------------------------------------------------------------------------------------------------------------------------------------

static kvm_ctrl mkctrl(int op, int flags)
{
	kvm_ctrl c;
	memset(&c, 0, sizeof(c));
	c.ver = 1; c.op = (uint8_t)op; c.flags = (uint8_t)flags;
	return c;
}

static int ctrl(kvm_stream *s, int op, int flags, uint32_t now, kvm_state_msg *r, unsigned *act)
{
	kvm_ctrl c = mkctrl(op, flags);
	return kvm_stream_on_ctrl(s, &c, now, r, act);
}

static void test_state_machine(void)
{
	kvm_stream s;
	kvm_state_msg r;
	unsigned act;
	uint8_t bytes[16];
	size_t i;

	// the build of ACK yes, video no (reason 1)
	kvm_stream_init(&s, 0, 1, KVM_REASON_NO_VIDEO_BUILD, KVM_REASON_DISABLED);
	kvm_stream_start_state(&s, &r);
	CHECK(r.reason == KVM_REASON_STARTED && r.video == 0 && r.ack == 0 && r.caps == KVM_CAPS_ACK && r.ver == 1);
	CHECK_EQ(s.want, KVM_WANT_NONE); CHECK(!kvm_stream_ack_active(&s));

	// op 1: video unavailable in this build, the ACK window starts; the answer is the vector state_no_video_build
	CHECK_EQ(ctrl(&s, KVM_OP_VIDEO_ON, 7, 100, &r, &act), 1);
	CHECK(r.reason == KVM_REASON_NO_VIDEO_BUILD && r.video == 0 && r.ack == 1 && r.caps == KVM_CAPS_ACK && act == 0);
	CHECK(kvm_stream_ack_active(&s)); CHECK_EQ(s.want, KVM_WANT_ACK);
	kvm_stream_build_state(bytes, &r);
	for (i = 0; i < KV_COUNT; i++) { if (strcmp(kVectors[i].name, "state_no_video_build") == 0) { CHECK(memcmp(bytes, kVectors[i].bytes, 16) == 0); } }

	// op 2 while ACK is on: ACK stays, reason 0
	CHECK_EQ(ctrl(&s, KVM_OP_VIDEO_OFF, 0, 110, &r, &act), 1);
	CHECK(r.reason == KVM_REASON_OK && r.video == 0 && r.ack == 1); CHECK(kvm_stream_ack_active(&s));
	// op 3 is never answered, and does nothing without video
	CHECK_EQ(ctrl(&s, KVM_OP_REPAIR, 3, 120, &r, &act), 0); CHECK_EQ(act, 0);
	// op 4: ACK only
	CHECK_EQ(ctrl(&s, KVM_OP_ACK_ONLY, 0, 130, &r, &act), 1);
	CHECK(r.reason == KVM_REASON_OK && r.ack == 1 && r.video == 0);

	// veto from the mux: ACK off, answered with reason 5; op 1 and op 4 refused while it lasts
	CHECK_EQ(ctrl(&s, KVM_OP_VETO, KVM_VETO_MUX, 140, &r, &act), 1);
	CHECK(r.reason == KVM_REASON_VETO && r.ack == 0 && r.video == 0); CHECK(!kvm_stream_ack_active(&s));
	CHECK_EQ(ctrl(&s, KVM_OP_VIDEO_ON, 7, 150, &r, &act), 1); CHECK(r.reason == KVM_REASON_VETO && r.ack == 0); CHECK(!kvm_stream_ack_active(&s));
	CHECK_EQ(ctrl(&s, KVM_OP_ACK_ONLY, 0, 150, &r, &act), 1); CHECK(r.reason == KVM_REASON_VETO && r.ack == 0);
	CHECK_EQ(ctrl(&s, KVM_OP_VIDEO_OFF, 0, 150, &r, &act), 1); CHECK(r.reason == KVM_REASON_VETO);
	// a second source: the first one ending does not lift the veto
	CHECK_EQ(ctrl(&s, KVM_OP_VETO, KVM_VETO_MESHCORE, 160, &r, &act), 1); CHECK_EQ(s.vetoMask, 3);
	CHECK_EQ(ctrl(&s, KVM_OP_VETO_END, KVM_VETO_MUX, 170, &r, &act), 1); CHECK(r.reason == KVM_REASON_VETO); CHECK_EQ(s.vetoMask, 1);
	CHECK(!kvm_stream_ack_active(&s));
	CHECK_EQ(ctrl(&s, KVM_OP_VETO_END, KVM_VETO_MESHCORE, 180, &r, &act), 1);
	CHECK(r.reason == KVM_REASON_AVAILABLE && r.ack == 0 && r.video == 0); CHECK_EQ(s.vetoMask, 0); CHECK_EQ(s.want, KVM_WANT_NONE);
	CHECK(!kvm_stream_ack_active(&s));	// the viewer has to ask again
	// a repeated veto of the same source is idempotent; one source can set both bits at once
	CHECK_EQ(ctrl(&s, KVM_OP_VETO, 3, 190, &r, &act), 1); CHECK_EQ(ctrl(&s, KVM_OP_VETO, 1, 190, &r, &act), 1); CHECK_EQ(s.vetoMask, 3);
	CHECK_EQ(ctrl(&s, KVM_OP_VETO_END, 3, 200, &r, &act), 1); CHECK(r.reason == KVM_REASON_AVAILABLE); CHECK_EQ(s.vetoMask, 0);
	// op 6 with no veto set: nothing changes
	CHECK_EQ(ctrl(&s, KVM_OP_ACK_ONLY, 0, 210, &r, &act), 1); CHECK(kvm_stream_ack_active(&s));
	CHECK_EQ(ctrl(&s, KVM_OP_VETO_END, 2, 220, &r, &act), 1); CHECK(r.reason == KVM_REASON_OK && r.ack == 1); CHECK(kvm_stream_ack_active(&s));

	// the child restarts (UAC): a new process starts without veto; kvm_relay injects op 5 with the remembered mask before any other input
	kvm_stream_init(&s, 0, 1, KVM_REASON_NO_VIDEO_BUILD, KVM_REASON_DISABLED);
	kvm_stream_start_state(&s, &r); CHECK_EQ(r.reason, KVM_REASON_STARTED);
	CHECK_EQ(ctrl(&s, KVM_OP_VETO, KVM_VETO_MESHCORE | KVM_VETO_MUX, 0, &r, &act), 1); CHECK_EQ(r.reason, KVM_REASON_VETO);
	CHECK_EQ(ctrl(&s, KVM_OP_VIDEO_ON, 7, 5, &r, &act), 1); CHECK(r.reason == KVM_REASON_VETO && r.ack == 0 && !kvm_stream_ack_active(&s));

	// ACK switched off in this process (MESH_KVM_ACK=0, MESH_KVM_ADAPT=0): reason 6, caps 0
	kvm_stream_init(&s, 0, 0, KVM_REASON_DISABLED, KVM_REASON_DISABLED);
	kvm_stream_start_state(&s, &r); CHECK_EQ(r.caps, 0);
	CHECK_EQ(ctrl(&s, KVM_OP_VIDEO_ON, 7, 0, &r, &act), 1); CHECK(r.reason == KVM_REASON_DISABLED && r.ack == 0 && r.video == 0 && r.caps == 0);
	CHECK_EQ(ctrl(&s, KVM_OP_ACK_ONLY, 0, 0, &r, &act), 1); CHECK(r.reason == KVM_REASON_DISABLED && r.ack == 0);
	CHECK(!kvm_stream_ack_active(&s));

	// a build with video: op 1 is handed to the video code, which answers; op 3 / op 2 / veto produce actions
	kvm_stream_init(&s, 1, 1, 0, KVM_REASON_DISABLED);
	CHECK_EQ(ctrl(&s, KVM_OP_VIDEO_ON, 5, 1000, &r, &act), 0); CHECK_EQ(act, KVM_ACT_VIDEO_REQUEST); CHECK_EQ(s.want, KVM_WANT_VIDEO); CHECK(kvm_stream_ack_active(&s));
	CHECK_EQ(ctrl(&s, KVM_OP_REPAIR, 3, 1001, &r, &act), 0); CHECK_EQ(act, 0);	// video not on yet
	s.videoOn = 1;
	CHECK_EQ(ctrl(&s, KVM_OP_REPAIR, 3, 1002, &r, &act), 0); CHECK_EQ(act, KVM_ACT_IDR | KVM_ACT_REPAIR);
	CHECK_EQ(ctrl(&s, KVM_OP_REPAIR, 2, 1002, &r, &act), 0); CHECK_EQ(act, KVM_ACT_REPAIR);
	CHECK_EQ(ctrl(&s, KVM_OP_VIDEO_OFF, 0, 1003, &r, &act), 1); CHECK(act == KVM_ACT_VIDEO_STOP && r.video == 0 && r.ack == 1 && r.reason == 0 && s.want == KVM_WANT_ACK);
	s.videoOn = 1; s.want = KVM_WANT_VIDEO;
	CHECK_EQ(ctrl(&s, KVM_OP_VETO, 2, 1004, &r, &act), 1); CHECK(act == KVM_ACT_VIDEO_STOP && r.video == 0 && r.ack == 0 && r.caps == 3);
	// gen++ is followed by a 242: the message carries the new gen
	kvm_stream_init(&s, 1, 1, 0, KVM_REASON_DISABLED);
	kvm_stream_bump_gen(&s); kvm_stream_bump_gen(&s);
	kvm_stream_state_msg(&s, KVM_REASON_OK, &r); CHECK_EQ(r.gen, 2);
	s.gen = 255; kvm_stream_bump_gen(&s); CHECK_EQ(s.gen, 0);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the window (8)
// ---------------------------------------------------------------------------------------------------------------------------------------

typedef struct sim_pos { uint32_t seq, ts; } sim_pos;

// A tiny agent loop: writes through the stream like Windows/kvm.c does.
static uint32_t wr_data(kvm_stream *s, uint32_t now, size_t len) { kvm_stream_note_write(s, len, KVM_W_DATA, now); return (uint32_t)len; }
static sim_pos wr_marker(kvm_stream *s, uint32_t now, int probe)
{
	uint8_t mk[KVM_MARKER_BYTES];
	sim_pos p;
	kvm_stream_make_marker(s, now, probe, mk);
	kvm_stream_note_write(s, sizeof(mk), KVM_W_POSITION, now);
	p.seq = ((uint32_t)mk[12] << 24) | ((uint32_t)mk[13] << 16) | ((uint32_t)mk[14] << 8) | mk[15];
	p.ts = now;
	return p;
}
static int send_ack(kvm_stream *s, sim_pos p, uint32_t now, unsigned queueMs)
{
	kvm_ack a;
	a.seq = p.seq; a.ts = p.ts; a.queueMs = (uint16_t)queueMs; a.flags = 0;
	return kvm_stream_on_ack(s, &a, now);
}
static void activate(kvm_stream *s, uint32_t now)
{
	kvm_state_msg r;
	kvm_stream_init(s, 0, 1, KVM_REASON_NO_VIDEO_BUILD, KVM_REASON_DISABLED);
	CHECK_EQ(ctrl(s, KVM_OP_ACK_ONLY, 0, now, &r, NULL), 1);
	CHECK(kvm_stream_ack_active(s));
}

// the ACK of one watched position (the last key frame) is remembered with its times
static void test_watch(uint32_t t0)
{
	static kvm_stream s;
	sim_pos m1, m2, m3;
	uint32_t ackAt = 0, ts = 0, q = 0, rtt = 0;
	activate(&s, t0);
	CHECK_EQ(kvm_stream_watch_result(&s, &ackAt, &ts, &q, &rtt), 0);			// nothing watched
	wr_data(&s, t0 + 10, 5000); m1 = wr_marker(&s, t0 + 11, 0);
	wr_data(&s, t0 + 20, 6000); m2 = wr_marker(&s, t0 + 21, 0);
	kvm_stream_watch(&s, m2.seq);
	CHECK_EQ(kvm_stream_watch_result(&s, &ackAt, &ts, &q, &rtt), 0);			// not acknowledged yet
	CHECK_EQ(send_ack(&s, m1, t0 + 50, 3), KVM_ACK_OK);
	CHECK_EQ(kvm_stream_watch_result(&s, &ackAt, &ts, &q, &rtt), 0);			// another position
	CHECK_EQ(send_ack(&s, m2, t0 + 90, 7), KVM_ACK_OK);
	CHECK_EQ(kvm_stream_watch_result(&s, &ackAt, &ts, &q, &rtt), 1);
	CHECK_EQ(ackAt, t0 + 90); CHECK_EQ(ts, t0 + 21); CHECK_EQ(q, 7); CHECK_EQ(rtt, s.minRtt);
	CHECK_EQ(kvm_stream_watch_result(&s, &ackAt, &ts, &q, &rtt), 0);			// once
	// an ACK that came after a later position was acknowledged (stale) tells nothing about the watched one: no result
	wr_data(&s, t0 + 100, 100); m3 = wr_marker(&s, t0 + 101, 0);
	wr_data(&s, t0 + 110, 100); { sim_pos m4 = wr_marker(&s, t0 + 111, 0); kvm_stream_watch(&s, m3.seq); CHECK_EQ(send_ack(&s, m4, t0 + 150, 0), KVM_ACK_OK); CHECK_EQ(send_ack(&s, m3, t0 + 151, 0), KVM_ACK_STALE); }
	CHECK_EQ(kvm_stream_watch_result(&s, &ackAt, &ts, &q, &rtt), 0);
	// a forged ACK for the watched seq (wrong ts) is no result
	wr_data(&s, t0 + 200, 100); m1 = wr_marker(&s, t0 + 201, 0);
	kvm_stream_watch(&s, m1.seq);
	{ sim_pos bad = m1; bad.ts += 1; CHECK_EQ(send_ack(&s, bad, t0 + 220, 0), KVM_ACK_UNKNOWN); }
	CHECK_EQ(kvm_stream_watch_result(&s, &ackAt, &ts, &q, &rtt), 0);
	CHECK_EQ(send_ack(&s, m1, t0 + 230, 0), KVM_ACK_OK);
	CHECK_EQ(kvm_stream_watch_result(&s, &ackAt, &ts, &q, &rtt), 1);
	// a result that was not read is not the result of the next watch
	wr_data(&s, t0 + 260, 100); m1 = wr_marker(&s, t0 + 261, 0); kvm_stream_watch(&s, m1.seq);
	CHECK_EQ(send_ack(&s, m1, t0 + 270, 0), KVM_ACK_OK);
	wr_data(&s, t0 + 280, 100); m2 = wr_marker(&s, t0 + 281, 0); kvm_stream_watch(&s, m2.seq);
	CHECK_EQ(kvm_stream_watch_result(&s, &ackAt, &ts, &q, &rtt), 0);
	CHECK_EQ(send_ack(&s, m2, t0 + 290, 0), KVM_ACK_OK);
	CHECK_EQ(kvm_stream_watch_result(&s, &ackAt, &ts, &q, &rtt), 1); CHECK_EQ(ts, t0 + 281);
	// a new watch replaces the old one
	wr_data(&s, t0 + 300, 100); m1 = wr_marker(&s, t0 + 301, 0); kvm_stream_watch(&s, m1.seq);
	wr_data(&s, t0 + 310, 100); m2 = wr_marker(&s, t0 + 311, 0); kvm_stream_watch(&s, m2.seq);
	CHECK_EQ(send_ack(&s, m2, t0 + 350, 0), KVM_ACK_OK);
	CHECK_EQ(kvm_stream_watch_result(&s, &ackAt, &ts, &q, &rtt), 1); CHECK_EQ(ts, t0 + 311);
}

static void test_ack_basic(uint32_t t0)
{
	static kvm_stream s;
	sim_pos m1, m2;
	activate(&s, t0);
	CHECK_EQ(kvm_stream_inflight(&s), 0);
	// 242 and cmd 7 before the first position: counted, and covered by the first marker
	{ uint8_t st[16] = {0}; (void)st; kvm_stream_note_write(&s, 16, KVM_W_STATE, t0 + 1); }
	CHECK(!kvm_stream_marker_wanted(&s, t0 + 2, 0, NULL));	// a 242 alone does not ask for a marker
	wr_data(&s, t0 + 10, 5000);
	CHECK(kvm_stream_marker_wanted(&s, t0 + 11, 0, NULL));	// M1
	m1 = wr_marker(&s, t0 + 11, 0);
	CHECK(!kvm_stream_marker_wanted(&s, t0 + 12, 0, NULL));	// the marker is covered, and does not ask for another one
	CHECK_EQ(kvm_stream_inflight(&s), 16 + 5000 + KVM_MARKER_BYTES);
	// a forged ACK: wrong ts, unknown seq, wrong everything
	{
		sim_pos bad = m1;
		bad.ts += 1; CHECK_EQ(send_ack(&s, bad, t0 + 20, 0), KVM_ACK_UNKNOWN);
		bad = m1; bad.seq += 1; CHECK_EQ(send_ack(&s, bad, t0 + 20, 0), KVM_ACK_UNKNOWN);
		bad.seq = 0xFFFFFFFFu; bad.ts = 0; CHECK_EQ(send_ack(&s, bad, t0 + 20, 0), KVM_ACK_UNKNOWN);
		CHECK_EQ(kvm_stream_inflight(&s), 16 + 5000 + KVM_MARKER_BYTES);	// nothing moved
	}
	CHECK_EQ(send_ack(&s, m1, t0 + 50, 3), KVM_ACK_OK);
	CHECK_EQ(kvm_stream_inflight(&s), 0);
	CHECK_EQ(send_ack(&s, m1, t0 + 60, 0), KVM_ACK_STALE);	// repeated
	// a second position; ACK for the second, then a late ACK for the first is stale
	wr_data(&s, t0 + 100, 100); m2 = wr_marker(&s, t0 + 100, 0);
	wr_data(&s, t0 + 101, 100);
	CHECK_EQ(send_ack(&s, m2, t0 + 120, 0), KVM_ACK_OK);
	CHECK_EQ(send_ack(&s, m1, t0 + 121, 0), KVM_ACK_STALE);
	CHECK_EQ(kvm_stream_inflight(&s), 100);	// the 100 bytes written after the marker wait for the next position
	CHECK(kvm_stream_marker_wanted(&s, t0 + 122, 0, NULL));	// ... and they are dirty
	// the window of a fresh activation
	CHECK_EQ(kvm_stream_window(&s, t0 + 122), KVM_STREAM_W_START);
}

// the access unit that the encoder thread is making is counted as in flight by the gates of the JPEG and of the sharpening, not by the gate of the video
static void test_reserved(uint32_t t0)
{
	static kvm_stream s;
	activate(&s, t0);
	wr_data(&s, t0 + 1, 100u * 1024u);							// 100 KiB: below 0.5 W (128 KiB)
	CHECK(kvm_stream_window_allows(&s, KVM_GATE_SHARPEN, t0 + 2));
	kvm_stream_set_reserved(&s, 40u * 1024u);					// a frame of ~40 KiB is on its way: 140 KiB
	CHECK(!kvm_stream_window_allows(&s, KVM_GATE_SHARPEN, t0 + 2));
	CHECK(kvm_stream_window_allows(&s, KVM_GATE_BIG, t0 + 2));
	CHECK(kvm_stream_window_allows(&s, KVM_GATE_SMALL, t0 + 2));
	CHECK(kvm_stream_window_allows(&s, KVM_GATE_VIDEO, t0 + 2));
	kvm_stream_set_reserved(&s, 200u * 1024u);					// 300 KiB >= W (256 KiB): the big JPEG waits, the video gate is the frame's own
	CHECK(!kvm_stream_window_allows(&s, KVM_GATE_BIG, t0 + 2));
	CHECK(kvm_stream_window_allows(&s, KVM_GATE_VIDEO, t0 + 2));
	CHECK(kvm_stream_window_allows(&s, KVM_GATE_SMALL, t0 + 2));	// 300 KiB < 1.25 W (320 KiB)
	kvm_stream_set_reserved(&s, 230u * 1024u);					// 330 KiB
	CHECK(!kvm_stream_window_allows(&s, KVM_GATE_SMALL, t0 + 2));
	kvm_stream_set_reserved(&s, 0);
	CHECK(kvm_stream_window_allows(&s, KVM_GATE_BIG, t0 + 2));
	CHECK_EQ(kvm_stream_inflight(&s), 100u * 1024u);			// the real in-flight count (the rate samples) does not include it
}

static void test_gates(uint32_t t0)
{
	static kvm_stream s;
	sim_pos m;
	activate(&s, t0);
	// below W everything goes
	wr_data(&s, t0 + 1, 100u * 1024u);
	CHECK(kvm_stream_window_allows(&s, KVM_GATE_BIG, t0 + 2));
	CHECK(kvm_stream_window_allows(&s, KVM_GATE_SMALL, t0 + 2));
	CHECK(kvm_stream_window_allows(&s, KVM_GATE_SHARPEN, t0 + 2));	// 100 KiB < 128 KiB (0.5 W)
	wr_data(&s, t0 + 2, 40u * 1024u);	// 140 KiB: sharpening is closed (>= 0.5 W), the rest goes
	CHECK(!kvm_stream_window_allows(&s, KVM_GATE_SHARPEN, t0 + 3));
	CHECK(kvm_stream_window_allows(&s, KVM_GATE_BIG, t0 + 3));
	wr_data(&s, t0 + 3, 130u * 1024u);	// 270 KiB >= W (256 KiB): big closed, small (up to 320 KiB) open
	CHECK(!kvm_stream_window_allows(&s, KVM_GATE_BIG, t0 + 4));
	CHECK(!kvm_stream_window_allows(&s, KVM_GATE_VIDEO, t0 + 4));
	CHECK(kvm_stream_window_allows(&s, KVM_GATE_SMALL, t0 + 4));
	wr_data(&s, t0 + 4, 60u * 1024u);	// 330 KiB >= 1.25 W (320 KiB): small closed as well
	CHECK(!kvm_stream_window_allows(&s, KVM_GATE_SMALL, t0 + 5));
	// exact edges: inflight == W is closed for big, inflight + 1 below 1.25 W is open for small
	{
		static kvm_stream e;
		activate(&e, t0);
		wr_data(&e, t0 + 1, KVM_STREAM_W_START - 1); CHECK(kvm_stream_window_allows(&e, KVM_GATE_BIG, t0 + 2));
		wr_data(&e, t0 + 1, 1); CHECK(!kvm_stream_window_allows(&e, KVM_GATE_BIG, t0 + 2));
	}
	// the marker frees the window
	m = wr_marker(&s, t0 + 6, 0);
	CHECK_EQ(send_ack(&s, m, t0 + 20, 0), KVM_ACK_OK);
	CHECK(kvm_stream_window_allows(&s, KVM_GATE_SHARPEN, t0 + 21));
	CHECK(kvm_stream_window_allows(&s, KVM_GATE_BIG, t0 + 21));
	// without ACK nothing is limited
	{
		static kvm_stream off;
		kvm_stream_init(&off, 0, 1, 1, 6);
		kvm_stream_note_write(&off, 100u * 1024u * 1024u, KVM_W_DATA, t0);
		CHECK(kvm_stream_window_allows(&off, KVM_GATE_BIG, t0 + 1)); CHECK(kvm_stream_window_allows(&off, KVM_GATE_SHARPEN, t0 + 1));
		CHECK(!kvm_stream_marker_wanted(&off, t0 + 5000, 0, NULL));
		CHECK_EQ(kvm_stream_inflight(&off), 0);
	}
}

// An IDR bigger than W goes when nothing is in flight, and then closes the window
static void test_idr_larger_than_window(uint32_t t0)
{
	static kvm_stream s;
	uint32_t seq;
	sim_pos m;
	activate(&s, t0);
	CHECK(kvm_stream_window(&s, t0) < 400u * 1024u);
	CHECK(kvm_stream_window_allows(&s, KVM_GATE_VIDEO, t0 + 1));	// inflight 0: the 400 KiB IDR may go
	seq = kvm_stream_add_position(&s, t0 + 1, 400u * 1024u);
	kvm_stream_note_write(&s, 400u * 1024u, KVM_W_POSITION, t0 + 1);
	CHECK(!kvm_stream_window_allows(&s, KVM_GATE_VIDEO, t0 + 2));	// the next frame waits for the ACK
	CHECK(!kvm_stream_marker_wanted(&s, t0 + 2, 0, NULL));	// the frame carries its position
	m.seq = seq; m.ts = t0 + 1;
	CHECK_EQ(send_ack(&s, m, t0 + 30, 0), KVM_ACK_OK);
	CHECK(kvm_stream_window_allows(&s, KVM_GATE_VIDEO, t0 + 31));
	// a frame that is not an IDR is checked the same way: only inflight < W matters, never the size of the frame
	wr_data(&s, t0 + 40, KVM_STREAM_W_START - 10);
	CHECK(kvm_stream_window_allows(&s, KVM_GATE_VIDEO, t0 + 41));
}

// 1024 positions are remembered; the oldest is overwritten
static void test_ring(uint32_t t0)
{
	static kvm_stream s;
	static sim_pos pos[3000];
	int i;
	activate(&s, t0);
	for (i = 0; i < 1500; i++)
	{
		wr_data(&s, t0 + i, 10);
		pos[i] = wr_marker(&s, t0 + i, 0);
	}
	CHECK_EQ(send_ack(&s, pos[0], t0 + 2000, 0), KVM_ACK_UNKNOWN);	// overwritten
	CHECK_EQ(send_ack(&s, pos[1500 - 1024 - 1], t0 + 2000, 0), KVM_ACK_UNKNOWN);
	CHECK_EQ(send_ack(&s, pos[1500 - 1024], t0 + 2005, 0), KVM_ACK_OK);	// the oldest one still in the ring
	CHECK(kvm_stream_inflight(&s) > 0);
	CHECK_EQ(send_ack(&s, pos[1499], t0 + 2010, 0), KVM_ACK_OK);	// one ACK of the newest frees everything
	CHECK_EQ(kvm_stream_inflight(&s), 0);
}

// seq wrap-around in the ring: the positions 0xFFFFFFFE, 0xFFFFFFFF, 1, 2 (0 is skipped)
static void test_seq_wrap(uint32_t t0)
{
	static kvm_stream s;
	sim_pos p[4];
	int i;
	activate(&s, t0);
	s.seqCounter = 0xFFFFFFFDu;
	for (i = 0; i < 4; i++) { wr_data(&s, t0 + i, 100); p[i] = wr_marker(&s, t0 + i, 0); }
	CHECK_EQ(p[0].seq, 0xFFFFFFFEu); CHECK_EQ(p[1].seq, 0xFFFFFFFFu); CHECK_EQ(p[2].seq, 1); CHECK_EQ(p[3].seq, 2);
	CHECK_EQ(send_ack(&s, p[1], t0 + 10, 0), KVM_ACK_OK);	// 0xFFFFFFFF
	CHECK_EQ(send_ack(&s, p[0], t0 + 11, 0), KVM_ACK_STALE);	// older, although the number is smaller by 1 and larger than 2
	CHECK_EQ(send_ack(&s, p[3], t0 + 12, 0), KVM_ACK_OK);	// 2 is newer than 0xFFFFFFFF
	CHECK_EQ(kvm_stream_inflight(&s), 0);
	{	// seq 0 cannot be acknowledged (it is never handed out); the parser refuses it before it gets here
		sim_pos z; z.seq = 0; z.ts = t0; CHECK_EQ(send_ack(&s, z, t0 + 13, 0), KVM_ACK_UNKNOWN);
	}
}

// ACK flood: 200 per second with a burst of 50
static void test_flood(uint32_t t0)
{
	static kvm_stream s;
	static sim_pos pos[400];
	int i, ok = 0, flood = 0, other = 0;
	activate(&s, t0);
	for (i = 0; i < 300; i++) { wr_data(&s, t0, 10); pos[i] = wr_marker(&s, t0, 0); }
	for (i = 0; i < 300; i++)	// all in the same millisecond
	{
		int r = send_ack(&s, pos[i], t0 + 10, 0);
		if (r == KVM_ACK_OK) { ok++; } else if (r == KVM_ACK_FLOOD) { flood++; } else { other++; }
	}
	CHECK_EQ(ok, KVM_STREAM_ACK_BURST); CHECK_EQ(flood, 300 - KVM_STREAM_ACK_BURST); CHECK_EQ(other, 0);
	CHECK(kvm_stream_inflight(&s) > 0);	// the refused ones did not count
	// forged ACKs also use up the tokens (the search of the ring is the expensive part)
	{
		sim_pos bad; bad.seq = 12345; bad.ts = 1;
		for (i = 0; i < 1000; i++) { (void)send_ack(&s, bad, t0 + 10, 0); }
		CHECK(s.st.acksFlood >= (uint32_t)(300 - KVM_STREAM_ACK_BURST + 900));
	}
	// 5 ms later one token (200/s) is back, and a good ACK passes
	CHECK_EQ(send_ack(&s, pos[299], t0 + 15, 0), KVM_ACK_OK);
	CHECK_EQ(kvm_stream_inflight(&s), 0);
	// a long pause refills the bucket to the burst only
	{
		int n = 0;
		for (i = 0; i < 100; i++) { sim_pos bad; bad.seq = 777; bad.ts = (uint32_t)i; if (send_ack(&s, bad, t0 + 10000, 0) == KVM_ACK_UNKNOWN) { n++; } }
		CHECK_EQ(n, KVM_STREAM_ACK_BURST);
	}
	// 200 ACKs in one second, evenly spaced, are all accepted
	{
		static kvm_stream f;
		static sim_pos q[250];
		int good = 0;
		activate(&f, t0);
		for (i = 0; i < 200; i++) { wr_data(&f, t0 + (uint32_t)(i * 5), 10); q[i] = wr_marker(&f, t0 + (uint32_t)(i * 5), 0); }
		for (i = 0; i < 200; i++) { if (send_ack(&f, q[i], t0 + 2000 + (uint32_t)(i * 5), 0) == KVM_ACK_OK) { good++; } }
		CHECK_EQ(good, 200);
	}
}

// Deadlock 1: the multiplexer drops entries (overflow): the bytes are gone, no ACK for them. The probe marker (M2) frees the window.
static void test_deadlock_mux_drop(uint32_t t0)
{
	static kvm_stream s;
	sim_pos m, probe;
	int probeFlag = 0;
	uint32_t t;
	activate(&s, t0);
	// 300 KiB written in pieces of 30 KiB, each iteration with its marker; the mux throws all of it away: no ACK arrives
	for (t = 0; t < 10; t++) { wr_data(&s, t0 + t * 10, 30u * 1024u); m = wr_marker(&s, t0 + t * 10, 0); }
	(void)m;
	CHECK(kvm_stream_inflight(&s) > KVM_STREAM_W_START);
	CHECK(!kvm_stream_window_allows(&s, KVM_GATE_BIG, t0 + 200));
	CHECK(!kvm_stream_marker_wanted(&s, t0 + 200, 0, &probeFlag));	// no data was written since the last marker, and it is too early for the probe
	CHECK(!kvm_stream_marker_wanted(&s, t0 + KVM_STREAM_PROBE_MS - 1, 0, &probeFlag));
	// 1 s after the last progress the probe marker is due, once per second
	CHECK(kvm_stream_marker_wanted(&s, t0 + KVM_STREAM_PROBE_MS, 0, &probeFlag)); CHECK(probeFlag);
	probe = wr_marker(&s, t0 + KVM_STREAM_PROBE_MS, 1);
	CHECK(!kvm_stream_marker_wanted(&s, t0 + KVM_STREAM_PROBE_MS + 500, 0, &probeFlag));
	CHECK(!kvm_stream_marker_wanted(&s, t0 + (2 * KVM_STREAM_PROBE_MS) - 1, 0, &probeFlag));
	// the viewer receives the probe (the mux is no longer overflowing) and acknowledges it: the lost bytes are released with it
	CHECK_EQ(send_ack(&s, probe, t0 + KVM_STREAM_PROBE_MS + 30, 0), KVM_ACK_OK);
	CHECK_EQ(kvm_stream_inflight(&s), 0);
	CHECK(kvm_stream_window_allows(&s, KVM_GATE_BIG, t0 + KVM_STREAM_PROBE_MS + 31));
	CHECK_EQ(s.st.probes, 1);
	// a blocked write never gets a probe
	{
		static kvm_stream b;
		activate(&b, t0);
		wr_data(&b, t0, 1000); (void)wr_marker(&b, t0, 0);
		CHECK(!kvm_stream_marker_wanted(&b, t0 + 5000, 1, NULL));
		CHECK(kvm_stream_marker_wanted(&b, t0 + 5000, 0, NULL));
	}
}

// Deadlock 2: the viewer is gone (tab closed, network down). After 2 s W = 64 KiB; after 15 s ACK is off with reason 9; the agent works as before.
static void test_viewer_gone(uint32_t t0)
{
	static kvm_stream s;
	kvm_state_msg r;
	uint32_t t;
	int answered = 0;
	activate(&s, t0);
	wr_data(&s, t0, 100u * 1024u); (void)wr_marker(&s, t0, 0);
	CHECK_EQ(kvm_stream_window(&s, t0 + 1999), KVM_STREAM_W_START);
	CHECK_EQ(kvm_stream_window(&s, t0 + 2000), KVM_STREAM_W_MIN);	// stall: no ACK for 2 s with data in flight
	CHECK(!kvm_stream_window_allows(&s, KVM_GATE_BIG, t0 + 2000));	// 100 KiB in flight >= 64 KiB
	CHECK(!kvm_stream_window_allows(&s, KVM_GATE_SMALL, t0 + 2000));	// 100 KiB >= 1.25 * 64 KiB = 80 KiB
	for (t = 0; t <= KVM_STREAM_TIMEOUT_MS + 100; t += 10)
	{
		if (kvm_stream_tick(&s, t0 + t, 0, &r)) { answered++; CHECK(t >= KVM_STREAM_TIMEOUT_MS); break; }
	}
	CHECK_EQ(answered, 1);
	CHECK(r.reason == KVM_REASON_NO_ACK && r.ack == 0 && r.video == 0);
	CHECK(!kvm_stream_ack_active(&s)); CHECK_EQ(s.want, KVM_WANT_NONE);
	CHECK(kvm_stream_window_allows(&s, KVM_GATE_BIG, t0 + 20000)); CHECK(kvm_stream_window_allows(&s, KVM_GATE_SHARPEN, t0 + 20000));
	CHECK(!kvm_stream_marker_wanted(&s, t0 + 20000, 0, NULL));
	CHECK_EQ(s.st.timeouts, 1); CHECK(s.st.stalls >= 1);
	// the viewer comes back and asks again: the window starts fresh
	CHECK_EQ(ctrl(&s, KVM_OP_VIDEO_ON, 7, t0 + 30000, &r, NULL), 1); CHECK(r.ack == 1 && kvm_stream_ack_active(&s));
	CHECK_EQ(kvm_stream_inflight(&s), 0);
	CHECK_EQ(kvm_stream_window(&s, t0 + 30000), KVM_STREAM_W_START);

	// a blocked write (pause) stops the clock
	{
		static kvm_stream b;
		activate(&b, t0);
		wr_data(&b, t0, 1000); (void)wr_marker(&b, t0, 0);
		for (t = 0; t < 60000; t += 100) { CHECK(!kvm_stream_tick(&b, t0 + t, 1, &r)); }
		CHECK(kvm_stream_ack_active(&b));
		CHECK_EQ(kvm_stream_window(&b, t0 + 60000), KVM_STREAM_W_START);	// not a stall either
		for (t = 60000; t < 60000 + KVM_STREAM_TIMEOUT_MS - 100; t += 100) { CHECK(!kvm_stream_tick(&b, t0 + t, 0, &r)); }	// and the 15 s start when it ends
		CHECK(kvm_stream_ack_active(&b));
		CHECK(kvm_stream_tick(&b, t0 + 60000 + KVM_STREAM_TIMEOUT_MS + 200, 0, &r));
	}
	// a write that blocked for 40 s (the pipe was full): the clocks did not run during it, and the ACKs that waited in the queue of the input thread count
	{
		static kvm_stream b2;
		sim_pos m;
		activate(&b2, t0);
		wr_data(&b2, t0, 1000); m = wr_marker(&b2, t0, 0);
		kvm_stream_note_blocked(&b2, t0 + 40000);
		CHECK(!kvm_stream_tick(&b2, t0 + 40001, 0, &r));
		CHECK_EQ(kvm_stream_window(&b2, t0 + 40001), KVM_STREAM_W_START);
		CHECK(!kvm_stream_marker_wanted(&b2, t0 + 40500, 0, NULL));
		CHECK_EQ(send_ack(&b2, m, t0 + 20000, 0), KVM_ACK_OK);	// arrived during the block
		CHECK_EQ(kvm_stream_inflight(&b2), 0);
		wr_data(&b2, t0 + 40100, 10); (void)wr_marker(&b2, t0 + 40100, 0);
		CHECK(!kvm_stream_tick(&b2, t0 + 40100 + KVM_STREAM_TIMEOUT_MS - 1, 0, &r));
		CHECK(kvm_stream_tick(&b2, t0 + 40100 + KVM_STREAM_TIMEOUT_MS, 0, &r));
	}
	// nothing in flight is never "no ACK": hours of silence, then data, then the clocks start at the data
	{
		static kvm_stream z;
		sim_pos m;
		activate(&z, t0);
		wr_data(&z, t0 + 10, 100); m = wr_marker(&z, t0 + 10, 0);
		CHECK_EQ(send_ack(&z, m, t0 + 30, 0), KVM_ACK_OK);
		for (t = 100; t < 600000; t += 1000) { CHECK(!kvm_stream_tick(&z, t0 + t, 0, &r)); }
		CHECK(kvm_stream_ack_active(&z)); CHECK_EQ(kvm_stream_window(&z, t0 + 600000), KVM_STREAM_W_START);
		wr_data(&z, t0 + 600000, 100); (void)wr_marker(&z, t0 + 600000, 0);
		CHECK(!kvm_stream_tick(&z, t0 + 600000 + KVM_STREAM_TIMEOUT_MS - 1, 0, &r));
		CHECK(kvm_stream_tick(&z, t0 + 600000 + KVM_STREAM_TIMEOUT_MS, 0, &r));
	}
	// the veto switches the window off at once and the gates open
	{
		static kvm_stream v;
		activate(&v, t0);
		wr_data(&v, t0, 1000000); (void)wr_marker(&v, t0, 0);
		CHECK(!kvm_stream_window_allows(&v, KVM_GATE_BIG, t0 + 1));
		CHECK_EQ(ctrl(&v, KVM_OP_VETO, 1, t0 + 2, &r, NULL), 1);
		CHECK(kvm_stream_window_allows(&v, KVM_GATE_BIG, t0 + 3));
		CHECK_EQ(kvm_stream_inflight(&v), 0);
	}
}

// ---- a simulated link: the window follows the delivery rate ----

typedef struct sim_ack { sim_pos p; uint32_t at; } sim_ack;

typedef struct sim_link
{
	uint32_t rateBps;		// bytes per second the link takes
	uint32_t oneWayMs;		// base round trip (without queueing)
	uint64_t freeUs;		// time (us since t0) when the link has delivered everything written so far
	sim_ack q[8192];		// positions on their way back as ACKs
	unsigned qh, qt;
	uint32_t chunk;			// bytes the sender writes in each iteration (0 = nothing: the pipe drains)
	int noGate;				// the sender does not ask the window (content that the window has not refused yet: a link that is clogged without a refusal)
} sim_link;

// Runs the agent loop for ms milliseconds: every 5 ms an iteration that writes one chunk (l->chunk bytes) when the BIG gate is open (the sender always has more to
// send, at chunk / 5 ms), and the marker that ends the iteration; the link takes the bytes at its rate, the viewer acknowledges each position when its last
// byte is through.
static void run_sim(kvm_stream *s, sim_link *l, uint32_t t0, uint32_t *tnow, uint32_t ms, uint64_t *wMin, uint64_t *wMax, uint32_t *accepted)
{
	uint32_t t, end = *tnow + ms;
	kvm_state_msg r;
	if (wMin != NULL) { *wMin = ~0ull; }
	if (wMax != NULL) { *wMax = 0; }
	for (t = *tnow; t != end; t++)
	{
		while (l->qh != l->qt && (int32_t)(t - l->q[l->qh % 8192].at) >= 0)
		{
			if (send_ack(s, l->q[l->qh % 8192].p, t, 0) == KVM_ACK_OK && accepted != NULL) { (*accepted)++; }
			l->qh++;
		}
		(void)kvm_stream_tick(s, t, 0, &r);
		if (((t - t0) % 5) == 0)
		{
			uint64_t nowUs = (uint64_t)(t - t0) * 1000u;
			if (l->chunk > 0)
			{
				if (l->noGate || kvm_stream_window_allows(s, KVM_GATE_BIG, t))
				{
					if (l->freeUs < nowUs) { l->freeUs = nowUs; }
					l->freeUs += ((uint64_t)l->chunk * 1000000ull) / l->rateBps;
					wr_data(s, t, l->chunk);
				}
				else { kvm_stream_note_link_held(s, t); }	// (kvm_ack_gate of kvm.c: a big rectangle that the window refuses says that the link is the limit)
			}
			{
				int pr = 0;
				if (kvm_stream_marker_wanted(s, t, 0, &pr))
				{
				sim_pos p = wr_marker(s, t, pr);
				if (l->freeUs < nowUs) { l->freeUs = nowUs; }
				l->freeUs += (KVM_MARKER_BYTES * 1000000ull) / l->rateBps;
				l->q[l->qt % 8192].p = p; l->q[l->qt % 8192].at = t0 + (uint32_t)(l->freeUs / 1000u) + l->oneWayMs; l->qt++;
				}
			}
			if (wMin != NULL) { uint64_t w = kvm_stream_window(s, t); if (w < *wMin) { *wMin = w; } if (w > *wMax) { *wMax = w; } }
		}
	}
	*tnow = end;
}

// One position written into an empty pipe (a cursor move, a keystroke) measures the round trip of the path
static void light_probe(kvm_stream *s, sim_link *l, uint32_t t0, uint32_t *tnow)
{
	uint32_t t, end;
	sim_pos p;
	uint32_t saved = l->chunk;
	l->chunk = 0;
	run_sim(s, l, t0, tnow, 1500, NULL, NULL, NULL);	// the sender stops, everything in the pipe is acknowledged
	l->chunk = saved;
	t = *tnow; end = *tnow + 2000;
	wr_data(s, t, 40); p = wr_marker(s, t, 0);
	for (; t != end; t++)
	{
		if (t == *tnow + l->oneWayMs + 1) { (void)send_ack(s, p, t, 0); }
	}
	*tnow = end;
}

static void test_window_follows_link(uint32_t t0)
{
	static kvm_stream s;
	static sim_link l;
	uint32_t t = t0;
	uint64_t w, expect;
	uint32_t accepted = 0;
	activate(&s, t0);
	memset(&l, 0, sizeof(l));
	l.rateBps = 5u * 1000u * 1000u; l.oneWayMs = 40; l.chunk = 32768;	// 40 Mb/s, 40 ms; the sender can do 6.5 MB/s
	run_sim(&s, &l, t0, &t, 6000, NULL, NULL, &accepted);
	w = kvm_stream_window(&s, t);
	expect = (uint64_t)l.rateBps * (s.minRtt + KVM_STREAM_QUEUE_DELAY_MS) / 1000u;
	printf("window 40 Mb/s: W = %llu KiB, expected about %llu KiB, minRtt = %u ms, rate = %llu KB/s, acks = %u\n", (unsigned long long)(w / 1024), (unsigned long long)(expect / 1024), s.minRtt, (unsigned long long)(kvm_stream_rate(&s, t) / 1000), accepted);
	CHECK(s.minRtt <= 100);
	CHECK(w >= expect * 7 / 10 && w <= expect * 13 / 10);
	CHECK(w >= 800u * 1024u && w <= 1600u * 1024u);	// 5 MB/s * (0.2 s + minRtt)
	CHECK(accepted > 100);
	// the link slows to a quarter: after the old samples left the 2 s span, W is about a quarter of it
	l.rateBps = 1250u * 1000u;	// 10 Mb/s
	run_sim(&s, &l, t0, &t, 8000, NULL, NULL, &accepted);
	w = kvm_stream_window(&s, t);
	expect = (uint64_t)l.rateBps * (s.minRtt + KVM_STREAM_QUEUE_DELAY_MS) / 1000u;
	printf("window 10 Mb/s: W = %llu KiB, expected about %llu KiB\n", (unsigned long long)(w / 1024), (unsigned long long)(expect / 1024));
	CHECK(w >= expect * 7 / 10 && w <= expect * 13 / 10);
	CHECK(w >= 200u * 1024u && w <= 420u * 1024u);	// 1.25 MB/s * (0.2 s + minRtt)
	// and grows again when the link recovers
	l.rateBps = 5u * 1000u * 1000u;
	run_sim(&s, &l, t0, &t, 8000, NULL, NULL, &accepted);
	w = kvm_stream_window(&s, t);
	expect = (uint64_t)l.rateBps * (s.minRtt + KVM_STREAM_QUEUE_DELAY_MS) / 1000u;
	printf("window 40 Mb/s again: W = %llu KiB, expected about %llu KiB\n", (unsigned long long)(w / 1024), (unsigned long long)(expect / 1024));
	CHECK(w >= expect * 7 / 10 && w <= expect * 13 / 10);
	// a light position (empty pipe) measures the path: 40 ms, and the window grows by rate * 40 ms at the next sample
	{
		uint32_t rtt;
		light_probe(&s, &l, t0, &t);
		rtt = s.minRtt;
		printf("minRtt after a light position: %u ms\n", rtt);
		CHECK(rtt >= 40 && rtt <= 46);
	}
	// a slow link: W stays at the floor and never above the start value
	{
		static kvm_stream s2;
		static sim_link l2;
		uint32_t t2 = t0;
		uint64_t wmin, wmax;
		activate(&s2, t0);
		memset(&l2, 0, sizeof(l2));
		l2.rateBps = 62500; l2.oneWayMs = 100; l2.chunk = 32768;	// 500 kb/s
		run_sim(&s2, &l2, t0, &t2, 10000, &wmin, &wmax, NULL);
		w = kvm_stream_window(&s2, t2);
		printf("window 500 kb/s: W = %llu KiB, inflight %llu KiB\n", (unsigned long long)(w / 1024), (unsigned long long)(kvm_stream_inflight(&s2) / 1024));
		CHECK(w >= KVM_STREAM_W_MIN && w <= 2 * KVM_STREAM_W_MIN);
		CHECK(wmax <= KVM_STREAM_W_START);
		// the queue the sender builds is bounded by W (plus the chunk that crossed it)
		CHECK(kvm_stream_inflight(&s2) < (uint64_t)(w + 40u * 1024u));
	}
	// ceiling
	{
		static kvm_stream s3;
		static sim_link l3;
		uint32_t t3 = t0;
		activate(&s3, t0);
		memset(&l3, 0, sizeof(l3));
		l3.rateBps = 400u * 1000u * 1000u; l3.oneWayMs = 300; l3.chunk = 128u * 1024u;
		run_sim(&s3, &l3, t0, &t3, 25000, NULL, NULL, NULL);
		CHECK(kvm_stream_window(&s3, t3) <= KVM_STREAM_W_MAX);
		printf("window 3200 Mb/s: W = %u KiB, rate %llu KB/s, inflight %llu KiB\n", kvm_stream_window(&s3, t3) / 1024, (unsigned long long)(kvm_stream_rate(&s3, t3) / 1000), (unsigned long long)(kvm_stream_inflight(&s3) / 1024));
		CHECK(kvm_stream_window(&s3, t3) >= 3u * 1024u * 1024u);
	}
}

// ---- more cases ----

// the bytes of a bad 240 / 241 are consumed by their size field, and the outcome does not depend on how the reads of the pipe cut the stream
static size_t sim_consume(const uint8_t *b, size_t blocklen, int *deliver)
{
	unsigned type;
	*deliver = 0;
	int ok = 0;
	size_t used = kvm_stdin_frame(b, blocklen, &ok);	// kvm_server_inputdata: one rule for every command
	if (used == 0) { return 0; }
	type = (unsigned)((b[0] << 8) | b[1]);
	if (type == KVM_CMD_VIDEO_CTRL || type == KVM_CMD_STREAM_ACK) { return kvm_stream_stdin_consume(b, blocklen, deliver); }
	*deliver = 0;
	return used;	// every other command: the size field
}

typedef struct stream_result { int delivered240, delivered241, other, otherIntact; } stream_result;

static void feed(uint8_t *buf, size_t *len, size_t *ptr, const uint8_t *chunk, size_t n, stream_result *r)
{
	size_t used;
	int deliver;
	memcpy(buf + *len, chunk, n); *len += n;
	while ((used = sim_consume(buf + *ptr, *len - *ptr, &deliver)) != 0)
	{
		const uint8_t *c = buf + *ptr;
		unsigned type = (unsigned)((c[0] << 8) | c[1]);
		if (type == KVM_CMD_VIDEO_CTRL && deliver && kvm_stream_parse_ctrl(c, 16, NULL) == KVM_REJ_NONE) { r->delivered240++; }
		else if (type == KVM_CMD_STREAM_ACK && deliver && kvm_stream_parse_ack(c, 16, NULL) == KVM_REJ_NONE) { r->delivered241++; }
		else if (type != KVM_CMD_VIDEO_CTRL && type != KVM_CMD_STREAM_ACK)
		{
			r->other++;
			if (used >= 6 && c[4] == 0xAB && c[used - 1] == 0xCD) { r->otherIntact++; }	// the bytes of the command are the ones that were sent
		}
		*ptr += used;
	}
	if (*ptr == *len) { *len = 0; *ptr = 0; }
}

static size_t mkcmd(uint8_t *out, unsigned type, unsigned size, int fill)
{
	size_t i;
	out[0] = (uint8_t)(type >> 8); out[1] = (uint8_t)type; out[2] = (uint8_t)(size >> 8); out[3] = (uint8_t)size;
	for (i = 4; i < size; i++) { out[i] = (uint8_t)(fill ? 0xAB : 0); }
	if (fill && size >= 6) { out[size - 1] = 0xCD; }
	return size;
}

static void test_stdin_framing(void)
{
	uint8_t stream[512], buf[1024];
	size_t n = 0, p, q;
	stream_result full, r;
	kvm_ctrl c = mkctrl(KVM_OP_ACK_ONLY, 0);
	kvm_ack a;
	a.seq = 5; a.ts = 77; a.queueMs = 1; a.flags = 0;
	n += mkcmd(stream + n, MNG_MOUSE, 10, 1);
	kvm_stream_build_ctrl(stream + n, &c); n += 16;					// valid 240
	n += mkcmd(stream + n, KVM_CMD_VIDEO_CTRL, 12, 1);				// 240 whose size field is 12
	n += mkcmd(stream + n, MNG_KEY, 6, 1);
	n += mkcmd(stream + n, KVM_CMD_STREAM_ACK, 20, 1);				// 241 whose size field is 20
	kvm_stream_build_ack(stream + n, &a); n += 16;					// valid 241
	n += mkcmd(stream + n, KVM_CMD_VIDEO_CTRL, 4, 0);				// 240 of 4 bytes
	n += mkcmd(stream + n, MNG_MOUSE, 12, 1);
	kvm_stream_build_ctrl(stream + n, &c); n += 16;
	{	// whole stream in one read
		size_t len = 0, ptr = 0;
		memset(&full, 0, sizeof(full));
		feed(buf, &len, &ptr, stream, n, &full);
		CHECK_EQ(full.delivered240, 2); CHECK_EQ(full.delivered241, 1); CHECK_EQ(full.other, 3); CHECK_EQ(full.otherIntact, 3); CHECK_EQ(ptr + len, 0);
	}
	for (p = 1; p < n; p++)	// split in two at every position
	{
		size_t len = 0, ptr = 0;
		memset(&r, 0, sizeof(r));
		feed(buf, &len, &ptr, stream, p, &r); feed(buf, &len, &ptr, stream + p, n - p, &r);
		CHECK(memcmp(&r, &full, sizeof(r)) == 0);
		CHECK_EQ(len, 0);
	}
	for (p = 1; p < n; p += 3)	// and in three
	{
		for (q = p + 1; q < n; q += 2)
		{
			size_t len = 0, ptr = 0;
			memset(&r, 0, sizeof(r));
			feed(buf, &len, &ptr, stream, p, &r); feed(buf, &len, &ptr, stream + p, q - p, &r); feed(buf, &len, &ptr, stream + q, n - q, &r);
			CHECK(memcmp(&r, &full, sizeof(r)) == 0);
		}
	}
	{	// byte by byte
		size_t len = 0, ptr = 0, i;
		memset(&r, 0, sizeof(r));
		for (i = 0; i < n; i++) { feed(buf, &len, &ptr, stream + i, 1, &r); }
		CHECK(memcmp(&r, &full, sizeof(r)) == 0);
	}
	{	// the function itself
		int d;
		uint8_t m[40];
		memset(m, 0, sizeof(m));
		CHECK_EQ(kvm_stream_stdin_consume(m, 3, &d), 0);
		mkcmd(m, KVM_CMD_VIDEO_CTRL, 20, 0); CHECK_EQ(kvm_stream_stdin_consume(m, 19, &d), 0); CHECK_EQ(kvm_stream_stdin_consume(m, 40, &d), 20); CHECK_EQ(d, 0);
		mkcmd(m, KVM_CMD_VIDEO_CTRL, 8, 0); CHECK_EQ(kvm_stream_stdin_consume(m, 40, &d), 8); CHECK_EQ(d, 0);
		mkcmd(m, KVM_CMD_VIDEO_CTRL, 16, 0); CHECK_EQ(kvm_stream_stdin_consume(m, 40, &d), 16); CHECK_EQ(d, 1); CHECK_EQ(kvm_stream_stdin_consume(m, 15, &d), 0);
		m[2] = 0; m[3] = 2; CHECK_EQ(kvm_stream_stdin_consume(m, 40, &d), 4); CHECK_EQ(d, 0);	// size < 4: the 4 bytes of the header
		m[2] = 0; m[3] = 0; CHECK_EQ(kvm_stream_stdin_consume(m, 40, &d), 4); CHECK_EQ(d, 0);
		m[2] = 0xFF; m[3] = 0xFF; CHECK_EQ(kvm_stream_stdin_consume(m, 40, &d), 4); CHECK_EQ(d, 0);
	}
}

// the veto sources tracked in the parent follow the byte stream whatever the writes are
static void test_veto_tracker(void)
{
	uint8_t stream[256];
	size_t n = 0, p, q;
	kvm_ctrl c;
	unsigned step;
	// op 5 (meshcore), mouse, op 5 (mux), key, op 6 (meshcore), a 240 op 4 (not a veto), op 6 with ver 2 (ignored), op 6 (mux)
	struct { int op, flags, ver; unsigned fill; } seq[] = { { KVM_OP_VETO, 1, 1, 0 }, { 0, 0, 0, MNG_MOUSE }, { KVM_OP_VETO, 2, 1, 0 }, { 0, 0, 0, MNG_KEY }, { KVM_OP_VETO_END, 1, 1, 0 },
		{ KVM_OP_ACK_ONLY, 0, 1, 0 }, { KVM_OP_VETO_END, 2, 2, 0 }, { KVM_OP_VETO_END, 2, 1, 0 } };
	uint8_t expect[] = { 1, 1, 3, 3, 2, 2, 2, 0 };	// after each element
	for (step = 0; step < sizeof(seq) / sizeof(seq[0]); step++)
	{
		if (seq[step].fill != 0) { n += mkcmd(stream + n, seq[step].fill, seq[step].fill == MNG_MOUSE ? 10 : 6, 1); }
		else
		{
			memset(&c, 0, sizeof(c)); c.ver = (uint8_t)seq[step].ver; c.op = (uint8_t)seq[step].op; c.flags = (uint8_t)seq[step].flags;
			kvm_stream_build_ctrl(stream + n, &c); n += 16;
		}
	}
	{	// whole, step by step, and the final mask
		kvm_veto_tracker t;
		size_t off = 0;
		memset(&t, 0, sizeof(t));
		for (step = 0; step < sizeof(seq) / sizeof(seq[0]); step++)
		{
			size_t sz = seq[step].fill == MNG_MOUSE ? 10 : (seq[step].fill != 0 ? 6 : 16);
			kvm_veto_track(&t, stream + off, sz); off += sz;
			CHECK_EQ(t.mask, expect[step]);
		}
		CHECK_EQ(off, n);
	}
	for (p = 1; p < n; p++)
	{
		kvm_veto_tracker t;
		memset(&t, 0, sizeof(t));
		kvm_veto_track(&t, stream, p); kvm_veto_track(&t, stream + p, n - p);
		CHECK_EQ(t.mask, 0);
	}
	for (p = 1; p < n; p += 2)
	{
		for (q = p + 1; q < n; q += 3)
		{
			kvm_veto_tracker t;
			memset(&t, 0, sizeof(t));
			kvm_veto_track(&t, stream, p); kvm_veto_track(&t, stream + p, q - p); kvm_veto_track(&t, stream + q, n - q);
			CHECK_EQ(t.mask, 0);
		}
	}
	{	// the mask survives the writes: the first part sets, the second part does not touch
		kvm_veto_tracker t;
		memset(&t, 0, sizeof(t));
		memset(&c, 0, sizeof(c)); c.ver = 1; c.op = KVM_OP_VETO; c.flags = 3;
		kvm_stream_build_ctrl(stream, &c);
		for (p = 1; p < 16; p++)
		{
			memset(&t, 0, sizeof(t));
			kvm_veto_track(&t, stream, p); CHECK_EQ(t.mask, 0);	// not complete yet
			kvm_veto_track(&t, stream + p, 16 - p); CHECK_EQ(t.mask, 3);
		}
	}
	{	// commands of a size field that is too small cannot trap the tracker
		kvm_veto_tracker t;
		uint8_t junk[8] = { 0, 1, 0, 0, 0, 1, 0, 0 };
		memset(&t, 0, sizeof(t));
		kvm_veto_track(&t, junk, sizeof(junk));
		memset(&c, 0, sizeof(c)); c.ver = 1; c.op = KVM_OP_VETO; c.flags = 1;
		kvm_stream_build_ctrl(stream, &c);
		kvm_veto_track(&t, stream, 16);
		CHECK_EQ(t.mask, 1);
		{	// sizes below 4 have no body: the 4 bytes of the header are the command, the veto behind them is seen. Sizes above the buffer: the whole declared length is skipped
			static const unsigned bad[] = { 0, 1, 2, 3 };
			size_t k;
			for (k = 0; k < sizeof(bad) / sizeof(bad[0]); k++)
			{
				uint8_t h[4] = { 0, 9, (uint8_t)(bad[k] >> 8), (uint8_t)bad[k] };
				memset(&t, 0, sizeof(t));
				kvm_veto_track(&t, h, 4); kvm_veto_track(&t, stream, 16);
				CHECK_EQ(t.mask, 1);
				memset(&t, 0, sizeof(t));
				kvm_veto_track(&t, h, 2); kvm_veto_track(&t, h + 2, 2); kvm_veto_track(&t, stream, 7); kvm_veto_track(&t, stream + 7, 9);	// cut anywhere
				CHECK_EQ(t.mask, 1);
			}
		}
	}
}

// the parent also remembers the last command 5 and the last command 13 and gives them to a new child (UAC, lock screen), in the order in which they came
static void test_replay_tracker(void)
{
	uint8_t a[12], b[10], c13[6], c13b[6], out[64], stream[200], veto[16];
	size_t n, p, q, k;
	kvm_veto_tracker t;
	kvm_ctrl c;
	for (k = 0; k < sizeof(a); k++) { a[k] = (uint8_t)(0x10 + k); }
	a[0] = 0; a[1] = KVM_CMD_COMPRESSION; a[2] = 0; a[3] = 12;					// command 5, size 12 (budget field)
	for (k = 0; k < sizeof(b); k++) { b[k] = (uint8_t)(0x30 + k); }
	b[0] = 0; b[1] = KVM_CMD_COMPRESSION; b[2] = 0; b[3] = 10;					// command 5, size 10
	c13[0] = 0; c13[1] = KVM_CMD_FRAME_RATE; c13[2] = 0; c13[3] = 6; c13[4] = 0; c13[5] = 33;
	c13b[0] = 0; c13b[1] = KVM_CMD_FRAME_RATE; c13b[2] = 0; c13b[3] = 6; c13b[4] = 0; c13b[5] = 16;
	memset(&c, 0, sizeof(c)); c.ver = 1; c.op = KVM_OP_VETO; c.flags = 1;
	kvm_stream_build_ctrl(veto, &c);
	memset(&t, 0, sizeof(t));
	CHECK_EQ(kvm_veto_replay(&t, out, sizeof(out)), 0);							// nothing passed yet: nothing to give
	// command 5, the veto between, mouse, command 13: replayed as 5 then 13, the veto is not part of it
	n = 0;
	memcpy(stream + n, a, 12); n += 12;
	memcpy(stream + n, veto, 16); n += 16;
	n += mkcmd(stream + n, MNG_MOUSE, 10, 1);
	memcpy(stream + n, c13, 6); n += 6;
	kvm_veto_track(&t, stream, n);
	CHECK_EQ(t.mask, 1);
	CHECK_EQ(kvm_veto_replay(&t, out, sizeof(out)), 18);
	CHECK(memcmp(out, a, 12) == 0); CHECK(memcmp(out + 12, c13, 6) == 0);
	// a later command 5 replaces the earlier one; a later 13 as well; the order is the order of arrival (13 first, then 5)
	memset(&t, 0, sizeof(t));
	kvm_veto_track(&t, a, 12); kvm_veto_track(&t, c13, 6); kvm_veto_track(&t, c13b, 6); kvm_veto_track(&t, b, 10);
	CHECK_EQ(kvm_veto_replay(&t, out, sizeof(out)), 16);
	CHECK(memcmp(out, c13b, 6) == 0); CHECK(memcmp(out + 6, b, 10) == 0);
	kvm_veto_track(&t, c13, 6);														// now the 13 is the later one
	CHECK_EQ(kvm_veto_replay(&t, out, sizeof(out)), 16);
	CHECK(memcmp(out, b, 10) == 0); CHECK(memcmp(out + 10, c13, 6) == 0);
	CHECK_EQ(kvm_veto_replay(&t, out, 15), 10);									// a buffer that is too small gets what fits whole (the first)
	// whatever the writes are (cut anywhere, in two and three pieces) the same bytes come out
	n = 0;
	memcpy(stream + n, a, 12); n += 12;
	n += mkcmd(stream + n, MNG_KEY, 6, 1);
	memcpy(stream + n, c13b, 6); n += 6;
	memcpy(stream + n, veto, 16); n += 16;
	for (p = 1; p < n; p++)
	{
		memset(&t, 0, sizeof(t));
		kvm_veto_track(&t, stream, p); kvm_veto_track(&t, stream + p, n - p);
		CHECK_EQ(kvm_veto_replay(&t, out, sizeof(out)), 18);
		CHECK(memcmp(out, a, 12) == 0 && memcmp(out + 12, c13b, 6) == 0);
		for (q = p + 1; q < n; q += 3)
		{
			memset(&t, 0, sizeof(t));
			kvm_veto_track(&t, stream, p); kvm_veto_track(&t, stream + p, q - p); kvm_veto_track(&t, stream + q, n - q);
			CHECK_EQ(kvm_veto_replay(&t, out, sizeof(out)), 18);
			CHECK(memcmp(out, a, 12) == 0 && memcmp(out + 12, c13b, 6) == 0);
		}
	}
	// commands 5 and 13 of a size that is not a command (below 6, above the buffer) are not remembered and do not trap the tracker: the veto behind them is still seen
	{
		static const unsigned bad[] = { 0, 3, 5, 17, 40 };
		for (k = 0; k < sizeof(bad) / sizeof(bad[0]); k++)
		{
			uint8_t h[40];
			memset(h, 0xEE, sizeof(h));
			h[0] = 0; h[1] = KVM_CMD_COMPRESSION; h[2] = (uint8_t)(bad[k] >> 8); h[3] = (uint8_t)bad[k];
			memset(&t, 0, sizeof(t));
			kvm_veto_track(&t, h, bad[k] >= 4 ? bad[k] : 4u);
			kvm_veto_track(&t, veto, 16);
			CHECK_EQ(t.mask, 1);
			CHECK_EQ(kvm_veto_replay(&t, out, sizeof(out)), 0);
		}
	}
	// a 240 of size 16 is never taken for a command 5 and the other way round
	memset(&t, 0, sizeof(t));
	kvm_veto_track(&t, veto, 16);
	CHECK_EQ(kvm_veto_replay(&t, out, sizeof(out)), 0); CHECK_EQ(t.mask, 1);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// sizes below 4 and above the buffer on the stdin, simulated with the loop of kvm_mainloopinput_ex (buffer, compaction, kvm_stdin_next with the discard counter)
// ---------------------------------------------------------------------------------------------------------------------------------------

typedef struct stdin_sim
{
	uint8_t buf[KVM_STDIN_BUF];
	size_t len, ptr;
	kvm_stdin_parser parser;
	kvm_inbox q;		// what the child would hold of the 240 that were delivered (the veto)
	int zeroRead;		// a read that would have asked for 0 bytes (the session would have been closed)
	int d240, d241, bad, others;
	size_t skipped;		// bytes of oversized frames that were thrown away
} stdin_sim;

static void stdin_init(stdin_sim *x)
{
	memset(x, 0, sizeof(*x));
	kvm_inbox_init(&x->q, 0);
}

// one read of the pipe (chunk), then the loop of the input thread: kvm_server_inputdata until it returns 0, then the buffer is emptied or compacted
static size_t stdin_read(stdin_sim *x, const uint8_t *chunk, size_t n)
{
	size_t used;
	if (KVM_STDIN_BUF - x->len == 0) { x->zeroRead++; return n; }
	if (n > KVM_STDIN_BUF - x->len) { n = KVM_STDIN_BUF - x->len; }
	memcpy(x->buf + x->len, chunk, n); x->len += n;
	for (;;)
	{
		int kind = 0, deliver = 0;
		const uint8_t *c = x->buf + x->ptr;
		unsigned type = x->len - x->ptr >= 2 ? (unsigned)((c[0] << 8) | c[1]) : 0;
		used = kvm_stdin_next(&x->parser, c, x->len - x->ptr, &kind);
		if (used == 0) { break; }
		if (kind == KVM_STDIN_KIND_BAD) { x->bad++; }
		else if (kind == KVM_STDIN_KIND_SKIP) { x->skipped += used; }
		else if (type == KVM_CMD_VIDEO_CTRL || type == KVM_CMD_STREAM_ACK)
		{
			kvm_ctrl cc;
			(void)kvm_stream_stdin_consume(c, x->len - x->ptr, &deliver);
			if (deliver && type == KVM_CMD_VIDEO_CTRL && kvm_stream_parse_ctrl(c, 16, &cc) == KVM_REJ_NONE) { x->d240++; (void)kvm_inbox_put(&x->q, &cc, 0); }
			else if (deliver && type == KVM_CMD_STREAM_ACK && kvm_stream_parse_ack(c, 16, NULL) == KVM_REJ_NONE) { x->d241++; }
			else { x->bad++; }
		}
		else { x->others++; }
		x->ptr += used;
	}
	x->len = kvm_stdin_compact(x->buf, x->len, x->ptr); x->ptr = 0;
	return n;	// what the read took (ReadFile asks for the room that is left, never more)
}

// the stream is fed in reads of `chunk` bytes
static void stdin_feed(stdin_sim *x, const uint8_t *s, size_t n, size_t chunk)
{
	size_t p;
	for (p = 0; p < n;) { p += stdin_read(x, s + p, (n - p < chunk) ? n - p : chunk); }
}

static void test_stdin_bad_sizes(void)
{
	static const unsigned small[] = { 0, 1, 2, 3 };
	static const unsigned big[] = { KVM_STDIN_MAX_CMD + 1, 40000, 65535 };
	static uint8_t stream[70000];
	size_t k, split;
	kvm_ctrl c = mkctrl(KVM_OP_ACK_ONLY, 0);
	// kvm_stdin_frame itself
	{
		uint8_t m[8];
		int ok;
		memset(m, 0, sizeof(m));
		CHECK_EQ(kvm_stdin_frame(m, 3, &ok), 0); CHECK_EQ(ok, 0);
		CHECK_EQ(kvm_stdin_frame(NULL, 8, &ok), 0);
		CHECK_EQ(kvm_stdin_frame(m, 8, &ok), 4); CHECK_EQ(ok, 0);					// size 0: the header, never a wait
		m[3] = 1; CHECK_EQ(kvm_stdin_frame(m, 8, &ok), 4); CHECK_EQ(ok, 0);
		m[3] = 3; CHECK_EQ(kvm_stdin_frame(m, 4, &ok), 4); CHECK_EQ(ok, 0);
		m[3] = 4; CHECK_EQ(kvm_stdin_frame(m, 4, &ok), 4); CHECK_EQ(ok, 1);			// size 4 is a command
		m[3] = 8; CHECK_EQ(kvm_stdin_frame(m, 8, &ok), 8); CHECK_EQ(ok, 1);
		m[3] = 9; CHECK_EQ(kvm_stdin_frame(m, 8, &ok), 0); CHECK_EQ(ok, 0);			// not complete yet
		m[2] = 0xFF; m[3] = 0xFF; CHECK_EQ(kvm_stdin_frame(m, 8, &ok), 4); CHECK_EQ(ok, 0);	// 65535: can never be completed
		m[2] = 0x75; m[3] = 0x30; CHECK_EQ(kvm_stdin_frame(m, 8, &ok), 0); CHECK_EQ(ok, 0);	// 30000 = the buffer: waits (it fits)
		m[2] = 0x75; m[3] = 0x31; CHECK_EQ(kvm_stdin_frame(m, 8, &ok), 4); CHECK_EQ(ok, 0);	// 30001: does not fit
	}
	// kvm_stdin_next: the discard counter
	{
		uint8_t m[8];
		int kind;
		kvm_stdin_parser p;
		memset(m, 0, sizeof(m)); memset(&p, 0, sizeof(p));
		m[2] = 0x9C; m[3] = 0x40;													// 40000
		CHECK_EQ(kvm_stdin_next(&p, m, 8, &kind), 4); CHECK_EQ(kind, KVM_STDIN_KIND_BAD); CHECK_EQ(p.discard, 39996);
		CHECK_EQ(kvm_stdin_next(&p, m, 2, &kind), 2); CHECK_EQ(kind, KVM_STDIN_KIND_SKIP); CHECK_EQ(p.discard, 39994);	// less than a header: still thrown away
		CHECK_EQ(kvm_stdin_next(&p, m, 8, &kind), 8); CHECK_EQ(kind, KVM_STDIN_KIND_SKIP); CHECK_EQ(p.discard, 39986);
		CHECK_EQ(kvm_stdin_next(&p, m, 0, &kind), 0);
		p.discard = 3; CHECK_EQ(kvm_stdin_next(&p, m, 8, &kind), 3); CHECK_EQ(p.discard, 0);	// the end of the body: the next bytes are a header again
		m[2] = 0; m[3] = 2; CHECK_EQ(kvm_stdin_next(&p, m, 8, &kind), 4); CHECK_EQ(kind, KVM_STDIN_KIND_BAD); CHECK_EQ(p.discard, 0);	// size 2: the header only
	}
	// sizes 0-3 at the head, valid commands behind them, whatever the cuts of the reads are
	for (k = 0; k < sizeof(small) / sizeof(small[0]); k++)
	{
		size_t n = 0;
		memset(stream, 0, 200);
		stream[n++] = 0; stream[n++] = 7; stream[n++] = (uint8_t)(small[k] >> 8); stream[n++] = (uint8_t)small[k];	// type 7, wrong size
		kvm_stream_build_ctrl(stream + n, &c); n += 16;									// valid 240 right behind the 4 header bytes
		n += mkcmd(stream + n, MNG_MOUSE, 10, 1);
		kvm_stream_build_ctrl(stream + n, &c); n += 16;
		for (split = 0; split <= n; split++)
		{
			stdin_sim x;
			stdin_init(&x);
			if (split > 0) { stdin_read(&x, stream, split); }
			if (split < n) { stdin_read(&x, stream + split, n - split); }
			CHECK_EQ(x.zeroRead, 0); CHECK_EQ(x.bad, 1); CHECK_EQ(x.d240, 2); CHECK_EQ(x.others, 1); CHECK_EQ(x.len, 0); CHECK_EQ(x.skipped, 0);
		}
	}
	// sizes above the buffer: the body is thrown away, a valid 240 inside it is NOT a command. Behind the frame the stream goes on as usual.
	for (k = 0; k < sizeof(big) / sizeof(big[0]); k++)
	{
		static const size_t chunks[] = { 1, 7, 4096, 30000 };
		size_t n = 0, ci, body, i, tail;
		int variant;
		for (variant = 0; variant < 3; variant++)	// 0: op 5 mux, 1: op 6 + op 1, 2: op 5 meshcore (all inside one frame, the first at several offsets)
		{
			kvm_veto_tracker t;
			n = 0;
			memset(stream, 0, sizeof(stream));
			stream[n++] = 0; stream[n++] = 2; stream[n++] = (uint8_t)(big[k] >> 8); stream[n++] = (uint8_t)big[k];	// a mouse frame (type 2) of a size that does not fit
			body = big[k] - 4;
			for (i = 0; i + 16 <= body; i += 16 * 37)	// smuggled 240 every 592 bytes, from the first byte of the body (right behind the header) on
			{
				kvm_ctrl s = variant == 0 ? mkctrl(KVM_OP_VETO, KVM_VETO_MUX) : variant == 1 ? mkctrl(KVM_OP_VETO_END, KVM_VETO_ALL) : mkctrl(KVM_OP_VETO, KVM_VETO_MESHCORE);
				kvm_stream_build_ctrl(stream + n + i, &s);
				if (variant == 1 && i + 32 <= body) { s = mkctrl(KVM_OP_VIDEO_ON, KVM_PROFILE_HIGH); kvm_stream_build_ctrl(stream + n + i + 16, &s); }
			}
			{	// a 240 also at the very end of the body, and one that starts at the cut of a 4096 read
				kvm_ctrl s = mkctrl(KVM_OP_VETO, KVM_VETO_ALL);
				kvm_stream_build_ctrl(stream + n + body - 16, &s);
				if (body > 4096 + 16) { kvm_stream_build_ctrl(stream + n + 4096 - 4, &s); }
			}
			n += body;
			tail = n;
			kvm_stream_build_ctrl(stream + n, &c); n += 16;								// behind the frame: a valid 240 (op 4), a mouse move, an op 5 meshcore (real)
			n += mkcmd(stream + n, MNG_MOUSE, 10, 1);
			{ kvm_ctrl s = mkctrl(KVM_OP_VETO, KVM_VETO_MESHCORE); kvm_stream_build_ctrl(stream + n, &s); n += 16; }
			for (ci = 0; ci < sizeof(chunks) / sizeof(chunks[0]); ci++)
			{
				static stdin_sim x;
				stdin_init(&x);
				stdin_feed(&x, stream, n, chunks[ci]);
				CHECK_EQ(x.zeroRead, 0); CHECK_EQ(x.bad, 1); CHECK_EQ(x.skipped, body); CHECK_EQ(x.d240, 2); CHECK_EQ(x.others, 1); CHECK_EQ(x.len, 0);
				CHECK_EQ(x.parser.discard, 0);
				CHECK_EQ(x.q.vetoTarget, KVM_VETO_MESHCORE);							// only the real op 5 behind the frame
				memset(&t, 0, sizeof(t));												// the parent's tracker reads the same stream the same way
				{
					size_t p;
					for (p = 0; p < n; p += chunks[ci]) { kvm_veto_track(&t, stream + p, (n - p < chunks[ci]) ? n - p : chunks[ci]); }
				}
				CHECK_EQ(t.mask, x.q.vetoTarget);
				CHECK_EQ(t.mask, KVM_VETO_MESHCORE);
			}
			{	// cut at every position around the header and the end of the body
				size_t cuts[] = { 1, 2, 3, 4, 5, 6, 7, 8, 100, 4092, 4093, 4096, 4100, tail - 1, tail, tail + 1, tail + 15, tail + 16 };
				for (ci = 0; ci < sizeof(cuts) / sizeof(cuts[0]); ci++)
				{
					static stdin_sim x;
					kvm_veto_tracker t2;
					size_t cut = cuts[ci] < n ? cuts[ci] : n;
					stdin_init(&x);
					stdin_feed(&x, stream, cut, 30000); stdin_feed(&x, stream + cut, n - cut, 30000);
					CHECK_EQ(x.bad, 1); CHECK_EQ(x.skipped, body); CHECK_EQ(x.d240, 2); CHECK_EQ(x.q.vetoTarget, KVM_VETO_MESHCORE);
					memset(&t2, 0, sizeof(t2));
					kvm_veto_track(&t2, stream, cut); kvm_veto_track(&t2, stream + cut, n - cut);
					CHECK_EQ(t2.mask, KVM_VETO_MESHCORE);
				}
			}
		}
	}
	// size 0 repeated for a long time: the parser advances 4 bytes per header, the buffer never fills, the next valid command is still found
	{
		stdin_sim x;
		uint8_t z[400], v[16];
		size_t i;
		stdin_init(&x); memset(z, 0, sizeof(z));
		for (i = 0; i < 2000; i++) { stdin_read(&x, z, sizeof(z)); }					// 800 000 bytes of zeros
		kvm_stream_build_ctrl(v, &c);
		stdin_read(&x, v, 16);
		CHECK_EQ(x.zeroRead, 0); CHECK_EQ(x.len, 0); CHECK_EQ(x.bad, 200000); CHECK_EQ(x.d240, 1);
	}
	// a stream where every read ends in the middle of a command: the compaction keeps the buffer from filling up (before: the read asked for 0 bytes and the session was closed)
	{
		static stdin_sim x;
		static uint8_t all[10 * 40000];
		size_t i, pos = 0;
		stdin_init(&x);
		for (i = 0; i < 40000; i++) { mkcmd(all + 10 * i, MNG_MOUSE, 10, 1); }
		while (pos < sizeof(all))
		{
			size_t end = pos == 0 ? 23 : pos + 20;
			if (end > sizeof(all)) { end = sizeof(all); }
			stdin_read(&x, all + pos, end - pos);
			pos = end;
		}
		CHECK_EQ(x.zeroRead, 0); CHECK_EQ(x.others, 40000); CHECK_EQ(x.len, 0);
	}
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the inbox of the 240 commands (input thread -> main loop). The main loop is simulated as kvm_stream_poll does it: take, expand, kvm_stream_on_ctrl.
// ---------------------------------------------------------------------------------------------------------------------------------------

static int inbox_put(kvm_inbox *q, int op, int flags, uint32_t at)
{
	kvm_ctrl c = mkctrl(op, flags);
	return kvm_inbox_put(q, &c, at);
}

// one iteration of the main loop; returns the number of commands that were handled and ORs the actions
static size_t inbox_poll(kvm_inbox *q, kvm_stream *s, uint32_t now, unsigned *acts, int *ops)
{
	kvm_inbox b;
	kvm_inbox_op o[KVM_INBOX_OPS];
	size_t n, i;
	kvm_inbox_take(q, &b);
	n = kvm_inbox_expand(&b, s->vetoMask, o);
	for (i = 0; i < n; i++)
	{
		kvm_state_msg r;
		unsigned a = 0;
		(void)kvm_stream_on_ctrl(s, &o[i].c, now, &r, &a);
		if (acts != NULL) { *acts |= a; }
		if (ops != NULL) { ops[i] = o[i].c.op; }
	}
	return n;
}

static void test_inbox_veto_survives_flood(void)
{
	kvm_inbox q;
	kvm_stream s;
	unsigned acts;
	int ops[KVM_INBOX_OPS];
	size_t n;
	int i;
	// op 5 behind 1000 x op 3 (what the queue of 32 used to lose): applied by the next iteration, whatever the flood did
	kvm_stream_init(&s, 1, 1, KVM_REASON_OK, KVM_REASON_DISABLED);
	kvm_inbox_init(&q, 1000);
	for (i = 0; i < 1000; i++) { inbox_put(&q, KVM_OP_REPAIR, KVM_REPAIR_IDR, 1000); }
	CHECK_EQ(inbox_put(&q, KVM_OP_VETO, KVM_VETO_MESHCORE, 1000), KVM_INBOX_STORED);
	CHECK_EQ(q.nLimited, 1000 - KVM_INBOX_BURST);										// the bucket let the burst through and refused the rest of the flood
	acts = 0;
	n = inbox_poll(&q, &s, 1001, &acts, ops);
	CHECK_EQ(s.vetoMask, KVM_VETO_MESHCORE);
	CHECK_EQ(ops[0], KVM_OP_VETO);														// first, before the repairs and any request for video
	CHECK_EQ(n, 2); CHECK_EQ(ops[1], KVM_OP_REPAIR);									// the 32 op 3 are one command
	// op 6 behind a flood of op 1 / op 3: the veto is gone in the next iteration
	for (i = 0; i < 1000; i++) { inbox_put(&q, (i & 1) ? KVM_OP_VIDEO_ON : KVM_OP_REPAIR, 7, 1001); }
	CHECK_EQ(inbox_put(&q, KVM_OP_VETO_END, KVM_VETO_MESHCORE, 1001), KVM_INBOX_STORED);
	(void)inbox_poll(&q, &s, 1002, NULL, ops);
	CHECK_EQ(s.vetoMask, 0);
	// the same with the bucket empty from the start (a flood from a long time ago): the veto does not pass the bucket
	kvm_stream_init(&s, 1, 1, KVM_REASON_OK, KVM_REASON_DISABLED);
	kvm_inbox_init(&q, 5000);
	for (i = 0; i < 5000; i++) { inbox_put(&q, KVM_OP_REPAIR, 3, 5000); }
	CHECK_EQ(q.tokensMilli < 1000u, 1);
	inbox_put(&q, KVM_OP_VETO, KVM_VETO_MUX, 5000);
	inbox_put(&q, KVM_OP_VETO, KVM_VETO_MESHCORE, 5000);
	(void)inbox_poll(&q, &s, 5001, NULL, ops);
	CHECK_EQ(s.vetoMask, KVM_VETO_ALL);
	// 1 x op 5 per iteration, forever, with a flood in between: the stream follows
	for (i = 0; i < 200; i++)
	{
		int want = (i & 1) ? 0 : 1;
		inbox_put(&q, KVM_OP_REPAIR, 1, (uint32_t)(6000 + i));
		inbox_put(&q, want ? KVM_OP_VETO : KVM_OP_VETO_END, KVM_VETO_MESHCORE, (uint32_t)(6000 + i));
		(void)inbox_poll(&q, &s, (uint32_t)(6000 + i), NULL, ops);
		CHECK_EQ(s.vetoMask & KVM_VETO_MESHCORE, want ? KVM_VETO_MESHCORE : 0);
		CHECK_EQ(s.vetoMask & KVM_VETO_MUX, KVM_VETO_MUX);								// the other source is not touched
	}
}

static void test_inbox_set_then_clear(void)
{
	kvm_inbox q;
	kvm_stream s;
	unsigned acts = 0;
	int ops[KVM_INBOX_OPS];
	size_t n;
	// op 5 and op 6 between two iterations: the stream still sees the veto begin and end (the video stops, the viewer has to ask again), as when every command was handled in turn
	kvm_stream_init(&s, 1, 1, KVM_REASON_OK, KVM_REASON_DISABLED);
	kvm_inbox_init(&q, 1000);
	s.videoOn = 1; s.want = KVM_WANT_VIDEO;
	inbox_put(&q, KVM_OP_VETO, KVM_VETO_MESHCORE, 1000);
	inbox_put(&q, KVM_OP_VETO_END, KVM_VETO_MESHCORE, 1000);
	n = inbox_poll(&q, &s, 1001, &acts, ops);
	CHECK_EQ(n, 2); CHECK_EQ(ops[0], KVM_OP_VETO); CHECK_EQ(ops[1], KVM_OP_VETO_END);
	CHECK_EQ(s.vetoMask, 0); CHECK((acts & KVM_ACT_VIDEO_STOP) != 0); CHECK_EQ(s.want, KVM_WANT_NONE);
	// clear, set: ends with the veto
	inbox_put(&q, KVM_OP_VETO, KVM_VETO_MUX, 1002);
	(void)inbox_poll(&q, &s, 1002, NULL, ops);
	inbox_put(&q, KVM_OP_VETO_END, KVM_VETO_MUX, 1003);
	inbox_put(&q, KVM_OP_VETO, KVM_VETO_MUX, 1003);
	n = inbox_poll(&q, &s, 1003, NULL, ops);
	CHECK_EQ(s.vetoMask, KVM_VETO_MUX); CHECK_EQ(n, 1); CHECK_EQ(ops[0], KVM_OP_VETO);
	// nothing waiting, nothing to do; and a spurious op 6 changes nothing
	CHECK_EQ(inbox_poll(&q, &s, 1004, NULL, ops), 0);
	inbox_put(&q, KVM_OP_VETO_END, KVM_VETO_MESHCORE, 1004);
	CHECK_EQ(inbox_poll(&q, &s, 1005, NULL, ops), 0); CHECK_EQ(s.vetoMask, KVM_VETO_MUX);
	// the stream was reset behind the inbox (a new session in the same process): the target is applied again
	kvm_stream_init(&s, 1, 1, KVM_REASON_OK, KVM_REASON_DISABLED);
	n = inbox_poll(&q, &s, 1006, NULL, ops);
	CHECK_EQ(n, 1); CHECK_EQ(s.vetoMask, KVM_VETO_MUX);
	// and the other way: the inbox was reset behind a stream that still holds a veto (target 0): the stream follows the inbox
	kvm_inbox_init(&q, 1007);
	CHECK_EQ(inbox_poll(&q, &s, 1007, NULL, ops), 1); CHECK_EQ(s.vetoMask, 0);
}

static void test_inbox_merge(void)
{
	kvm_inbox q, b;
	kvm_inbox_op o[KVM_INBOX_OPS];
	kvm_ctrl on = mkctrl(KVM_OP_VIDEO_ON, KVM_PROFILE_HIGH);
	size_t n;
	kvm_inbox_init(&q, 1000);
	// op 3 joined: the flags are ORed, one command, the time of the first
	inbox_put(&q, KVM_OP_REPAIR, KVM_REPAIR_IDR, 1000);
	inbox_put(&q, KVM_OP_REPAIR, KVM_REPAIR_TILES, 1004);
	inbox_put(&q, KVM_OP_REPAIR, KVM_REPAIR_IDR, 1008);
	kvm_inbox_take(&q, &b);
	n = kvm_inbox_expand(&b, 0, o);
	CHECK_EQ(n, 1); CHECK_EQ(o[0].c.op, KVM_OP_REPAIR); CHECK_EQ(o[0].c.flags, KVM_REPAIR_IDR | KVM_REPAIR_TILES); CHECK_EQ(o[0].at, 1000); CHECK_EQ(q.nMerged, 2);
	// taken: nothing waits any more
	kvm_inbox_take(&q, &b); CHECK_EQ(kvm_inbox_expand(&b, 0, o), 0);
	// op 1, op 2, op 4: the last wins, the whole command (flags, limits) is the one of the last op 1 when that is the last
	on.maxW = 1920; on.maxH = 1080; on.maxLevelIdc = 41;
	inbox_put(&q, KVM_OP_VIDEO_ON, KVM_PROFILE_CBP, 1100);
	inbox_put(&q, KVM_OP_VIDEO_OFF, 0, 1101);
	kvm_inbox_put(&q, &on, 1102);
	kvm_inbox_take(&q, &b);
	n = kvm_inbox_expand(&b, 0, o);
	CHECK_EQ(n, 1); CHECK_EQ(o[0].c.op, KVM_OP_VIDEO_ON); CHECK_EQ(o[0].c.flags, KVM_PROFILE_HIGH); CHECK_EQ(o[0].c.maxW, 1920); CHECK_EQ(o[0].c.maxLevelIdc, 41); CHECK_EQ(o[0].at, 1102);
	kvm_inbox_take(&q, &b); CHECK_EQ(kvm_inbox_expand(&b, 0, o), 0);							// taken: the request is not handled twice
	inbox_put(&q, KVM_OP_VIDEO_ON, KVM_PROFILE_CBP, 1200);
	inbox_put(&q, KVM_OP_ACK_ONLY, 0, 1201);
	kvm_inbox_take(&q, &b);
	n = kvm_inbox_expand(&b, 0, o);
	CHECK_EQ(n, 1); CHECK_EQ(o[0].c.op, KVM_OP_ACK_ONLY);
	// everything at once: veto, request, repair in this order (all four slots used)
	inbox_put(&q, KVM_OP_REPAIR, KVM_REPAIR_IDR, 1300);
	inbox_put(&q, KVM_OP_VIDEO_ON, KVM_PROFILE_MAIN, 1301);
	inbox_put(&q, KVM_OP_VETO, KVM_VETO_MESHCORE, 1302);
	inbox_put(&q, KVM_OP_VETO_END, KVM_VETO_MUX, 1303);
	kvm_inbox_take(&q, &b);
	n = kvm_inbox_expand(&b, KVM_VETO_MUX, o);
	CHECK_EQ(n, 4); CHECK_EQ(o[0].c.op, KVM_OP_VETO); CHECK_EQ(o[0].c.flags, KVM_VETO_MESHCORE); CHECK_EQ(o[1].c.op, KVM_OP_VETO_END); CHECK_EQ(o[1].c.flags, KVM_VETO_MUX);
	CHECK_EQ(o[2].c.op, KVM_OP_VIDEO_ON); CHECK_EQ(o[3].c.op, KVM_OP_REPAIR);
	CHECK_EQ(o[0].c.ver, 1); CHECK_EQ(o[1].c.ver, 1); CHECK_EQ(o[3].c.ver, 1);
}

static void test_inbox_rate(void)
{
	kvm_inbox q;
	int i, accepted = 0;
	uint32_t t;
	kvm_inbox_init(&q, 1000);
	// a legitimate viewer: a few commands per second, never limited, also for a long time
	for (i = 0; i < 600; i++)
	{
		accepted += inbox_put(&q, (i % 3) == 0 ? KVM_OP_VIDEO_ON : KVM_OP_REPAIR, 3, 1000 + (uint32_t)i * 250) == KVM_INBOX_STORED;
		if ((i % 8) == 0) { accepted += inbox_put(&q, KVM_OP_REPAIR, 1, 1000 + (uint32_t)i * 250 + 1) == KVM_INBOX_STORED; }
	}
	CHECK_EQ(q.nLimited, 0); CHECK(accepted > 600);
	// the burst of a viewer that starts (video on, repair, ack only ...) passes in full
	kvm_inbox_init(&q, 5000);
	for (i = 0; i < KVM_INBOX_BURST; i++) { CHECK_EQ(inbox_put(&q, KVM_OP_REPAIR, 1, 5000), KVM_INBOX_STORED); }
	CHECK_EQ(inbox_put(&q, KVM_OP_REPAIR, 1, 5000), KVM_INBOX_LIMITED);
	CHECK_EQ(q.nLimited, 1);
	// 1 command per ms for 1 s: the burst plus the rate
	kvm_inbox_init(&q, 10000);
	accepted = 0;
	for (t = 0; t < 1000; t++) { accepted += inbox_put(&q, KVM_OP_REPAIR, 1, 10000 + t) == KVM_INBOX_STORED; }
	CHECK(accepted >= KVM_INBOX_BURST + KVM_INBOX_RATE - 2 && accepted <= KVM_INBOX_BURST + KVM_INBOX_RATE + 1);
	CHECK_EQ(q.nLimited, 1000 - accepted);
	// after a second of silence the bucket is full again
	accepted = 0;
	for (i = 0; i < 100; i++) { accepted += inbox_put(&q, KVM_OP_REPAIR, 1, 12000) == KVM_INBOX_STORED; }
	CHECK_EQ(accepted, KVM_INBOX_BURST);
	// the counter wraps: 2^32 ms later is the same time
	kvm_inbox_init(&q, 0xFFFFFF00u);
	for (i = 0; i < KVM_INBOX_BURST; i++) { inbox_put(&q, KVM_OP_REPAIR, 1, 0xFFFFFF00u); }
	CHECK_EQ(inbox_put(&q, KVM_OP_REPAIR, 1, 0xFFFFFF00u), KVM_INBOX_LIMITED);
	CHECK_EQ(inbox_put(&q, KVM_OP_REPAIR, 1, 0x00000200u), KVM_INBOX_STORED);		// 0x300 ms later
}

// only the defined sources are ever kept, in the stream, in the inbox and in the tracker that the parent hands to a new child
static void test_veto_bits(void)
{
	kvm_stream s;
	kvm_state_msg r;
	kvm_inbox q, b;
	kvm_inbox_op o[KVM_INBOX_OPS];
	kvm_veto_tracker t;
	kvm_ctrl c;
	uint8_t m[16];
	int f;
	kvm_stream_init(&s, 0, 1, KVM_REASON_NO_VIDEO_BUILD, KVM_REASON_DISABLED);
	CHECK_EQ(KVM_VETO_ALL, 3);
	c = mkctrl(KVM_OP_VETO, 0xFC); CHECK_EQ(kvm_stream_parse_ctrl((kvm_stream_build_ctrl(m, &c), m), 16, NULL), KVM_REJ_BAD_FLAGS);	// no defined source: refused
	c = mkctrl(KVM_OP_VETO_END, 0x80); CHECK_EQ(kvm_stream_parse_ctrl((kvm_stream_build_ctrl(m, &c), m), 16, NULL), KVM_REJ_BAD_FLAGS);
	for (f = 1; f < 256; f++)
	{
		int exp = f & 3;
		c = mkctrl(KVM_OP_VETO, f);
		kvm_stream_build_ctrl(m, &c);
		memset(&t, 0, sizeof(t));
		kvm_veto_track(&t, m, 16);
		CHECK_EQ(t.mask, exp);
		kvm_stream_init(&s, 0, 1, KVM_REASON_NO_VIDEO_BUILD, KVM_REASON_DISABLED);
		(void)kvm_stream_on_ctrl(&s, &c, 100, &r, NULL);
		CHECK_EQ(s.vetoMask, exp);
		kvm_inbox_init(&q, 0);
		if (exp != 0) { kvm_inbox_put(&q, &c, 0); kvm_inbox_take(&q, &b); CHECK_EQ(kvm_inbox_expand(&b, 0, o), 1); CHECK_EQ(o[0].c.flags, exp); CHECK_EQ(b.vetoTarget, exp); }
		// op 6 with unknown bits clears only the defined ones and keeps the rest of the mask
		c = mkctrl(KVM_OP_VETO_END, f | 0x80);
		kvm_stream_build_ctrl(m, &c);
		kvm_veto_track(&t, m, 16);
		CHECK_EQ(t.mask, 0);
		(void)kvm_stream_on_ctrl(&s, &c, 101, &r, NULL);
		CHECK_EQ(s.vetoMask, 0);
	}
	// a mux bit is not cleared by an op 6 that only has reserved bits besides the meshcore bit
	memset(&t, 0, sizeof(t));
	c = mkctrl(KVM_OP_VETO, 3); kvm_stream_build_ctrl(m, &c); kvm_veto_track(&t, m, 16);
	c = mkctrl(KVM_OP_VETO_END, 0xFD); kvm_stream_build_ctrl(m, &c); kvm_veto_track(&t, m, 16);
	CHECK_EQ(t.mask, KVM_VETO_MUX);
}

// a pause in the traffic does not send the window back to its start value
static void test_window_memory(uint32_t t0)
{
	static kvm_stream s;
	static sim_link l;
	uint32_t t = t0;
	uint64_t before, after, mid;
	sim_pos m;
	activate(&s, t0);
	memset(&l, 0, sizeof(l));
	l.rateBps = 62500; l.oneWayMs = 40; l.chunk = 16384;	// 0.5 Mb/s, 40 ms: an earlier simulation
	run_sim(&s, &l, t0, &t, 8000, NULL, NULL, NULL);
	before = kvm_stream_window(&s, t);
	CHECK(before <= 100u * 1024u);	// the load made the window small
	l.chunk = 0;
	run_sim(&s, &l, t0, &t, 3000, NULL, NULL, NULL);	// 3 s of nothing (the viewer reads): the samples are older than 2 s
	after = kvm_stream_window(&s, t);
	mid = kvm_stream_rate(&s, t);
	printf("window after a pause: before %llu KiB, after %llu KiB, rate of the last 2 s %llu\n", (unsigned long long)(before / 1024), (unsigned long long)(after / 1024), (unsigned long long)mid);
	CHECK_EQ(mid, 0);
	CHECK_EQ(after, before);
	CHECK(after < KVM_STREAM_W_START);
	// the first traffic after the pause is let in up to this window, not to 256 KiB
	l.chunk = 16384;
	run_sim(&s, &l, t0, &t, 300, NULL, NULL, NULL);
	CHECK(kvm_stream_inflight(&s) <= after + 16384u);
	// the 2 s rule is the one that lowers W: data in flight, no ACK
	wr_data(&s, t, 40000); m = wr_marker(&s, t, 0); (void)m;
	CHECK(kvm_stream_window(&s, t + 2000) == KVM_STREAM_W_MIN);
}

// the remembered rate does not fall through a pause between two series of motion. AMD laptop (17:52:55): after 2 s of silence the first acknowledgements (a text cursor, a few KB) became the rate,
// the window went back to 64 KiB and the series that followed (an animation lasts 0.2-0.5 s) ran into a cold start. The memory is kept 30 s, then halved every 30 s, and voided by a real stall.
static void tune_trickle(kvm_stream *s, uint32_t t)
{
	// what the loop writes while nothing moves: a few hundred bytes (cursor, a caret) and the position that ends the iteration; the viewer acknowledges it 5 ms later
	sim_pos p;
	wr_data(s, t, 300); p = wr_marker(s, t, 0);
	(void)send_ack(s, p, t + 5, 0);
}

static void test_rate_memory_pause(uint32_t t0)
{
	static kvm_stream s;
	static sim_link l;
	uint32_t t = t0;
	uint64_t memBefore, wBefore;
	int i;
	activate(&s, t0);
	memset(&l, 0, sizeof(l));
	l.rateBps = 5u * 1000u * 1000u; l.oneWayMs = 5; l.chunk = 32768;	// 40 Mb/s, RTT 5 ms (the AMD laptop): window limited sender
	run_sim(&s, &l, t0, &t, 6000, NULL, NULL, NULL);
	memBefore = kvm_stream_rate_or_mem(&s, t); wBefore = kvm_stream_window(&s, t);
	CHECK(memBefore >= 2000000u);
	CHECK(wBefore > KVM_STREAM_W_RESUME);
	// 3 s of silence, then a trickle every 100 ms for 0.4 s (a caret, a clock): the rate and the window of the next series are what they were, the window not above W_RESUME
	l.chunk = 0;
	run_sim(&s, &l, t0, &t, 3000, NULL, NULL, NULL);
	CHECK_EQ(kvm_stream_rate(&s, t), 0);
	for (i = 0; i < 4; i++) { tune_trickle(&s, t); run_sim(&s, &l, t0, &t, 100, NULL, NULL, NULL); }	// (0.4 s: no sample yet)
	printf("rate memory after the pause: remembered %llu -> %llu KB/s, window %llu -> %llu KiB\n", (unsigned long long)(memBefore / 1000), (unsigned long long)(kvm_stream_rate_or_mem(&s, t) / 1000), (unsigned long long)(wBefore / 1024), (unsigned long long)(kvm_stream_window(&s, t) / 1024));
	CHECK(kvm_stream_rate_or_mem(&s, t) >= memBefore * 9 / 10);
	CHECK_EQ(kvm_stream_window(&s, t), KVM_STREAM_W_RESUME);
	// the first video frame of the next series fits (the ACK window is not the limit at the start of a series)
	wr_data(&s, t, 200 * 1024);
	CHECK(kvm_stream_window_allows(&s, KVM_GATE_VIDEO, t));
}

static void test_rate_memory_decay(uint32_t t0)
{
	static kvm_stream s;
	static sim_link l;
	uint32_t t = t0, held = 0;
	uint64_t mem;
	activate(&s, t0);
	memset(&l, 0, sizeof(l));
	l.rateBps = 5u * 1000u * 1000u; l.oneWayMs = 5; l.chunk = 32768;
	run_sim(&s, &l, t0, &t, 6000, NULL, NULL, NULL);
	mem = kvm_stream_rate_or_mem(&s, t);
	l.chunk = 0;
	run_sim(&s, &l, t0, &t, 2500, NULL, NULL, NULL);
	// kept for the hold time, then halved, and halved again every KVM_STREAM_MEM_HALF_MS
	while (held < 24000u) { run_sim(&s, &l, t0, &t, 1000, NULL, NULL, NULL); held += 1000; }
	CHECK(kvm_stream_rate_or_mem(&s, t) >= mem * 9 / 10);
	{
		uint32_t from = s.rateMemAt, at;
		uint64_t v;
		at = from + KVM_STREAM_MEM_HOLD_MS - 1; v = kvm_stream_rate_or_mem(&s, at); CHECK_EQ(v, s.rateMem);
		at = from + KVM_STREAM_MEM_HOLD_MS; v = kvm_stream_rate_or_mem(&s, at); CHECK_EQ(v, s.rateMem / 2);
		at = from + KVM_STREAM_MEM_HOLD_MS + KVM_STREAM_MEM_HALF_MS - 1; v = kvm_stream_rate_or_mem(&s, at); CHECK_EQ(v, s.rateMem / 2);
		at = from + KVM_STREAM_MEM_HOLD_MS + KVM_STREAM_MEM_HALF_MS; v = kvm_stream_rate_or_mem(&s, at); CHECK_EQ(v, s.rateMem / 4);
		at = from + KVM_STREAM_MEM_HOLD_MS + 3u * KVM_STREAM_MEM_HALF_MS; v = kvm_stream_rate_or_mem(&s, at); CHECK_EQ(v, s.rateMem / 16);
		at = from + 100u * KVM_STREAM_MEM_HALF_MS; v = kvm_stream_rate_or_mem(&s, at); CHECK_EQ(v, 0);
		printf("rate memory decay: %llu KB/s -> hold %u s -> %llu -> %llu KB/s\n", (unsigned long long)(s.rateMem / 1000), (unsigned)(KVM_STREAM_MEM_HOLD_MS / 1000), (unsigned long long)(s.rateMem / 2000), (unsigned long long)(s.rateMem / 4000));
	}
}

// a real stall voids it: data in flight and no acknowledgement for KVM_STREAM_STALL_MS
static void test_rate_memory_stall(uint32_t t0)
{
	static kvm_stream s;
	static sim_link l;
	uint32_t t = t0;
	kvm_state_msg r;
	activate(&s, t0);
	memset(&l, 0, sizeof(l));
	l.rateBps = 5u * 1000u * 1000u; l.oneWayMs = 5; l.chunk = 32768;
	run_sim(&s, &l, t0, &t, 6000, NULL, NULL, NULL);
	l.chunk = 0;
	run_sim(&s, &l, t0, &t, 2500, NULL, NULL, NULL);
	CHECK(kvm_stream_rate_or_mem(&s, t) > 0);
	wr_data(&s, t, 40000); (void)wr_marker(&s, t, 0);	// a position that is never acknowledged
	CHECK(kvm_stream_rate_or_mem(&s, t) > 0);
	for (; (int32_t)(t - t0) < 12000; t++) { (void)kvm_stream_tick(&s, t, 0, &r); if (s.st.stalls > 0) { break; } }
	CHECK_EQ(s.st.stalls, 1);
	CHECK_EQ(kvm_stream_rate_or_mem(&s, t), 0);
	CHECK_EQ(s.rateMem, 0);
}

// the link falls from 40 Mb/s to 4 Mb/s while nobody looks: the first series is not queued for seconds (the window after a pause is capped) and the rate follows the link within a second
static void test_rate_memory_link_drop(uint32_t t0)
{
	static kvm_stream s;
	static sim_link l;
	uint32_t t = t0, end;
	uint64_t maxFlight = 0, rateAfter = 0;
	activate(&s, t0);
	memset(&l, 0, sizeof(l));
	l.rateBps = 5u * 1000u * 1000u; l.oneWayMs = 5; l.chunk = 32768;
	run_sim(&s, &l, t0, &t, 6000, NULL, NULL, NULL);
	l.chunk = 0;
	run_sim(&s, &l, t0, &t, 4000, NULL, NULL, NULL);	// the pause; 4 Mb/s from now on
	l.rateBps = 500u * 1000u; l.chunk = 8192; l.freeUs = (uint64_t)(t - t0) * 1000u;
	end = t + 1000;
	while (t != end)
	{
		uint64_t fl;
		run_sim(&s, &l, t0, &t, 5, NULL, NULL, NULL);
		fl = kvm_stream_inflight(&s); if (fl > maxFlight) { maxFlight = fl; }
	}
	rateAfter = kvm_stream_rate_or_mem(&s, t);
	printf("rate memory link 40 -> 4 Mb/s in the pause: queue of the first second %llu ms, the rate after it %llu KB/s (link 500 KB/s)\n", (unsigned long long)(maxFlight * 1000u / l.rateBps), (unsigned long long)(rateAfter / 1000));
	CHECK(maxFlight * 1000u / l.rateBps <= (KVM_STREAM_W_RESUME * 1000u) / l.rateBps + 100u);	// the queue is the window of the resume (256 KiB: 0.5 s at 4 Mb/s), not the remembered 978 KiB (2 s)
	run_sim(&s, &l, t0, &t, 1000, NULL, NULL, NULL);
	rateAfter = kvm_stream_rate_or_mem(&s, t);
	CHECK(rateAfter <= 750u * 1000u);						// within two seconds of the new link the rate is the link's, not the old 5 MB/s
	CHECK(s.rateMem <= 700u * 1000u);						// the check of the resume (test_resume_check) has lowered the memory to the link after the first 64 KB
}

// the round trip of a probe has more than doubled: the path is not the one the rate was measured on
static void test_rate_memory_rtt_jump(uint32_t t0)
{
	static kvm_stream s;
	static sim_link l;
	uint32_t t = t0;
	uint8_t mk[KVM_MARKER_BYTES];
	sim_pos p;
	activate(&s, t0);
	memset(&l, 0, sizeof(l));
	l.rateBps = 5u * 1000u * 1000u; l.oneWayMs = 5; l.chunk = 32768;
	run_sim(&s, &l, t0, &t, 6000, NULL, NULL, NULL);
	light_probe(&s, &l, t0, &t);	// a position into the empty pipe: minRtt is measured
	printf("rate memory rtt jump: minRtt %u ms\n", s.minRtt);
	CHECK(s.minRtt >= 1 && s.minRtt <= 20);
	CHECK(s.rateMem > 0);
	// a probe with 8 ms more than minRtt: not a jump
	kvm_stream_make_marker(&s, t, 2, mk); kvm_stream_note_write(&s, sizeof(mk), KVM_W_POSITION, t);
	p.seq = ((uint32_t)mk[12] << 24) | ((uint32_t)mk[13] << 16) | ((uint32_t)mk[14] << 8) | mk[15]; p.ts = t;
	CHECK_EQ(send_ack(&s, p, t + s.minRtt + 8, 0), KVM_ACK_OK);
	CHECK(s.rateMem > 0);
	// a probe that took 60 ms (more than twice, and 20 ms more): the memory is void
	t += 100;
	kvm_stream_make_marker(&s, t, 2, mk); kvm_stream_note_write(&s, sizeof(mk), KVM_W_POSITION, t);
	p.seq = ((uint32_t)mk[12] << 24) | ((uint32_t)mk[13] << 16) | ((uint32_t)mk[14] << 8) | mk[15]; p.ts = t;
	CHECK_EQ(send_ack(&s, p, t + 60, 0), KVM_ACK_OK);
	CHECK_EQ(s.rateMem, 0);
	CHECK_EQ(kvm_stream_rate_or_mem(&s, t + 60), 0);
}

// a sample never spans the silence between two series: a burst of 0.3 s is no sample (measured over 0.5 s it would say two thirds of what the link took), a burst of a second is, and an accurate one
static void test_rate_sample_burst(uint32_t t0)
{
	static kvm_stream s;
	static sim_link l;
	uint32_t t = t0;
	uint64_t mem;
	activate(&s, t0);
	memset(&l, 0, sizeof(l));
	l.rateBps = 5u * 1000u * 1000u; l.oneWayMs = 5; l.chunk = 32768;
	run_sim(&s, &l, t0, &t, 6000, NULL, NULL, NULL);
	mem = kvm_stream_rate_or_mem(&s, t);
	l.chunk = 0;
	run_sim(&s, &l, t0, &t, 2600, NULL, NULL, NULL);
	// 0.3 s of work (the window holds the sender back): no sample, the remembered rate is what sizes the window
	l.chunk = 32768;
	run_sim(&s, &l, t0, &t, 300, NULL, NULL, NULL);
	l.chunk = 0;
	run_sim(&s, &l, t0, &t, 100, NULL, NULL, NULL);
	CHECK_EQ(kvm_stream_rate(&s, t), 0);
	CHECK(kvm_stream_rate_or_mem(&s, t) >= mem * 9 / 10);
	CHECK(s.rateMem >= mem * 9 / 10);
	// a pause again and a work of a second: a sample of what the link took
	run_sim(&s, &l, t0, &t, 2600, NULL, NULL, NULL);
	l.chunk = 32768;
	run_sim(&s, &l, t0, &t, 1100, NULL, NULL, NULL);
	printf("rate memory burst of 1.1 s after a pause: the rate sample %llu KB/s (link %u KB/s)\n", (unsigned long long)(kvm_stream_rate(&s, t) / 1000), l.rateBps / 1000u);
	CHECK(kvm_stream_rate(&s, t) >= (uint64_t)l.rateBps * 8 / 10);
	CHECK(kvm_stream_rate(&s, t) <= (uint64_t)l.rateBps * 12 / 10);
}

// a low sample that the window did not cause (a trickle, the tail of a sharpening that its own gate held back) does not pull the rate below what is remembered; once the window has held a video frame or a big
// rectangle back, the samples are what the link takes
static void test_rate_samples_vs_memory(uint32_t t0)
{
	static kvm_stream s;
	static sim_link l;
	uint32_t t = t0;
	uint64_t mem;
	activate(&s, t0);
	memset(&l, 0, sizeof(l));
	l.rateBps = 5u * 1000u * 1000u; l.oneWayMs = 5; l.chunk = 32768;
	run_sim(&s, &l, t0, &t, 6000, NULL, NULL, NULL);
	l.chunk = 0;
	run_sim(&s, &l, t0, &t, 2500, NULL, NULL, NULL);
	mem = s.rateMem;
	CHECK(mem > 4000u * 1000u);
	// a sample of 20 KB/s that the sharpening gate caused (note_held_back only): the memory wins
	s.samples[s.sampleHead].t = t; s.samples[s.sampleHead].rate = 20000u; s.sampleHead = (s.sampleHead + 1u) % KVM_STREAM_SAMPLES; if (s.nSamples < KVM_STREAM_SAMPLES) { s.nSamples++; }
	kvm_stream_note_held_back(&s, t);
	CHECK(!kvm_stream_link_limited(&s, t));
	CHECK_EQ(kvm_stream_rate(&s, t), 20000);
	CHECK_EQ(kvm_stream_rate_or_mem(&s, t), mem);
	CHECK_EQ(kvm_stream_rate_recent(&s, t), 20000);						// what the sharpening takes its share of: the delivered rate, not the highest seen
	CHECK(kvm_stream_window(&s, t) > KVM_STREAM_W_RESUME);						// (a sample exists, the memory sizes the window without the cap of the resume; the check of the resume watches the first 64 KB)
	// the window held a big rectangle back: the sample is what the link takes
	kvm_stream_note_link_held(&s, t);
	CHECK(kvm_stream_link_limited(&s, t));
	CHECK_EQ(kvm_stream_rate_or_mem(&s, t), 20000);
	CHECK_EQ(kvm_stream_window(&s, t), KVM_STREAM_W_MIN);
	// a second later it is not limited any more (KVM_STREAM_HELD_MS): the memory again
	t += KVM_STREAM_HELD_MS + 1;
	CHECK(!kvm_stream_link_limited(&s, t));
	CHECK_EQ(kvm_stream_rate_or_mem(&s, t), mem);
	CHECK_EQ(kvm_stream_rate_recent(&s, t), 20000);
	// no sample in the last 2 s: the memory
	CHECK_EQ(kvm_stream_rate_recent(&s, t + 2000), mem);
	// a sample that is higher than the memory always wins
	s.samples[s.sampleHead].t = t; s.samples[s.sampleHead].rate = mem * 2u; s.sampleHead = (s.sampleHead + 1u) % KVM_STREAM_SAMPLES; if (s.nSamples < KVM_STREAM_SAMPLES) { s.nSamples++; }
	CHECK_EQ(kvm_stream_rate_or_mem(&s, t), mem * 2u);
}

// sparse traffic (a frame every 100 ms, the pipe is empty in between) is one period of work: it makes samples (R at its floor sends 2 frames a second and must still be measured)
static void test_rate_sparse_traffic(uint32_t t0)
{
	static kvm_stream s;
	uint32_t t;
	sim_pos p;
	activate(&s, t0);
	for (t = t0; t < t0 + 3000u; t++)
	{
		if (((t - t0) % 100u) == 0) { wr_data(&s, t, 19000); p = wr_marker(&s, t, 0); (void)send_ack(&s, p, t + 10, 0); }
		(void)kvm_stream_tick(&s, t, 0, NULL);
	}
	printf("rate memory sparse traffic (19 KB every 100 ms): sample %llu KB/s\n", (unsigned long long)(kvm_stream_rate(&s, t) / 1000));
	CHECK(kvm_stream_rate(&s, t) >= 150000u && kvm_stream_rate(&s, t) <= 230000u);
	// a pause of 600 ms (more than KVM_STREAM_BUSY_GAP_MS) and another burst: a new period of work, the first span is the new one
	t += 600;
	CHECK(s.busyFrom <= t);
	wr_data(&s, t, 19000);
	CHECK((int32_t)(s.busyFrom - t) == 0);
}

// the check of the resume: a period of work that begins on the remembered rate alone finds out at once, from the first position with 64 KB in flight, that the link got slower in the pause
static void resume_series(kvm_stream *s, sim_link *l, uint32_t t0, uint32_t *t, uint32_t ms, uint32_t chunk)
{
	l->chunk = chunk;
	run_sim(s, l, t0, t, ms, NULL, NULL, NULL);
}

static void test_resume_check(uint32_t t0)
{
	static kvm_stream s, f;
	static sim_link l, lf;
	uint32_t t = t0, tf = t0;
	uint64_t mem;
	// the link falls from 40 to 4 Mb/s in a pause: after the first 64 KB the memory is the link (not 30 s later), the window and R follow
	activate(&s, t0);
	memset(&l, 0, sizeof(l));
	l.rateBps = 5u * 1000u * 1000u; l.oneWayMs = 5; l.chunk = 32768;
	run_sim(&s, &l, t0, &t, 6000, NULL, NULL, NULL);
	light_probe(&s, &l, t0, &t);					// minRtt is known
	mem = s.rateMem;
	CHECK(mem > 4000u * 1000u);
	l.chunk = 0; run_sim(&s, &l, t0, &t, 3000, NULL, NULL, NULL);
	l.rateBps = 500u * 1000u; l.freeUs = (uint64_t)(t - t0) * 1000u;
	resume_series(&s, &l, t0, &t, 20, 4096);		// a few KB: a flight below 64 KB says nothing
	CHECK(s.resumeArmed);
	CHECK_EQ(s.rateMem, mem);
	resume_series(&s, &l, t0, &t, 600, 8192);
	printf("rate memory the check of the resume, link 40 -> 4 Mb/s: the memory %llu -> %llu KB/s (link 500 KB/s), window %llu KiB\n", (unsigned long long)(mem / 1000), (unsigned long long)(s.rateMem / 1000), (unsigned long long)(kvm_stream_window(&s, t) / 1024));
	CHECK(!s.resumeArmed);
	CHECK(s.rateMem * 2 <= mem);
	CHECK(s.rateMem >= 200u * 1000u && s.rateMem <= 700u * 1000u);
	// the link as it was: nothing changes (the check is a lower bound of the capacity, a fast link is never lowered)
	activate(&f, t0);
	memset(&lf, 0, sizeof(lf));
	lf.rateBps = 5u * 1000u * 1000u; lf.oneWayMs = 5; lf.chunk = 32768;
	run_sim(&f, &lf, t0, &tf, 6000, NULL, NULL, NULL);
	light_probe(&f, &lf, t0, &tf);
	mem = f.rateMem;
	lf.chunk = 0; run_sim(&f, &lf, t0, &tf, 3000, NULL, NULL, NULL);
	resume_series(&f, &lf, t0, &tf, 600, 32768);
	CHECK(!f.resumeArmed);
	CHECK(f.rateMem >= mem);
	// a link that is 20 % slower than remembered is not "slower": the check lowers the memory only below half of it
	activate(&f, t0);
	memset(&lf, 0, sizeof(lf)); tf = t0;
	lf.rateBps = 5u * 1000u * 1000u; lf.oneWayMs = 5; lf.chunk = 32768;
	run_sim(&f, &lf, t0, &tf, 6000, NULL, NULL, NULL);
	light_probe(&f, &lf, t0, &tf);
	mem = f.rateMem;
	lf.chunk = 0; run_sim(&f, &lf, t0, &tf, 3000, NULL, NULL, NULL);
	lf.rateBps = 4u * 1000u * 1000u; lf.freeUs = (uint64_t)(tf - t0) * 1000u;
	resume_series(&f, &lf, t0, &tf, 600, 32768);
	CHECK(!f.resumeArmed);
	CHECK_EQ(f.rateMem, mem);
	// the same on a link with a round trip of 80 ms: the round trip is not taken for a slow link
	activate(&f, t0);
	memset(&lf, 0, sizeof(lf)); tf = t0;
	lf.rateBps = 5u * 1000u * 1000u; lf.oneWayMs = 80; lf.chunk = 32768;
	run_sim(&f, &lf, t0, &tf, 8000, NULL, NULL, NULL);
	light_probe(&f, &lf, t0, &tf);
	mem = f.rateMem;
	lf.chunk = 0; run_sim(&f, &lf, t0, &tf, 3000, NULL, NULL, NULL);
	resume_series(&f, &lf, t0, &tf, 900, 32768);
	CHECK(f.rateMem >= mem * 9 / 10);
}

// the first acknowledgements after a pause (a text cursor every 500 ms) do not take the rate and the window down: the span of a sample is never the silence, and a low sample that the window did not cause
// does not count below the remembered rate
static void test_rate_sample_after_silence(uint32_t t0)
{
	static kvm_stream s;
	static sim_link l;
	uint32_t t = t0;
	uint64_t mem;
	int i;
	activate(&s, t0);
	memset(&l, 0, sizeof(l));
	l.rateBps = 5u * 1000u * 1000u; l.oneWayMs = 5; l.chunk = 32768;
	run_sim(&s, &l, t0, &t, 6000, NULL, NULL, NULL);
	l.chunk = 0;
	run_sim(&s, &l, t0, &t, 2600, NULL, NULL, NULL);
	mem = kvm_stream_rate_or_mem(&s, t);
	tune_trickle(&s, t); run_sim(&s, &l, t0, &t, 20, NULL, NULL, NULL);
	CHECK_EQ(kvm_stream_rate(&s, t), 0);						// no sample from the first acknowledgement after the pause (a span needs two)
	for (i = 0; i < 6; i++) { run_sim(&s, &l, t0, &t, 530, NULL, NULL, NULL); tune_trickle(&s, t); run_sim(&s, &l, t0, &t, 20, NULL, NULL, NULL); }
	printf("rate memory trickle after the pause: the sample %llu B/s, the rate and the window are the remembered ones: %llu KB/s, %llu KiB\n", (unsigned long long)kvm_stream_rate(&s, t), (unsigned long long)(kvm_stream_rate_or_mem(&s, t) / 1000), (unsigned long long)(kvm_stream_window(&s, t) / 1024));
	CHECK(kvm_stream_rate(&s, t) < 20000u);						// (the sample, if any, is that of the trickle: a few KB/s)
	CHECK(kvm_stream_rate_or_mem(&s, t) >= mem * 9 / 10);
	CHECK_EQ(kvm_stream_window(&s, t), KVM_STREAM_W_RESUME);
}


// the principle of the app-limited samples (BBR, GoogCC), which replaced the filters of the first key frame proof (4 KB of the span) and of the rate filter (25 KB/s, until the first key frame). A span in which the
// pipe was empty at some moment (the sender had nothing to send) is a lower bound of what the link takes: it can raise the estimate and the memory, never lowers them, and a refusal of the window while
// the estimate is only such lower bounds is no evidence that the link is the limit (kvm_stream_note_link_held returns 0). A span in which the window held the sender back, or the pipe never emptied (a
// clogged link, also a slow one: the test after this one), is what the link takes and counts at any rate. The history: the markers of a screen that stands (28 B every 850 ms, the viewer is 600 ms late)
// gave 33 B/s as the first sample of a session and R 100 kb/s on the Windows 11 VM; a cursor with a caret (5 - 8 KB in 850 ms) the same behind a JPEG after the ACK of the key frame (scene E).
// the trickle: `dataBytes` of data and a position every 850 ms, each acknowledged 600 ms late, for 6 periods after the first picture
static void trickle_run(kvm_stream *s, uint32_t t0, uint32_t dataBytes, int heldBack)
{
	uint32_t t = t0;
	int i;
	sim_pos p;
	activate(s, t0);
	t += 100; wr_data(s, t, 574366u); p = wr_marker(s, t, 0); (void)send_ack(s, p, t + 70, 0);
	t += 900;
	for (i = 0; i < 6; i++)
	{
		if (dataBytes > 0) { wr_data(s, t, dataBytes); }
		if (heldBack && i == 3) { kvm_stream_note_held_back(s, t + 50); }	// the window refuses something after the acknowledgement of the previous period (the base of the span) and before the next one
		p = wr_marker(s, t, 1); (void)send_ack(s, p, t + 600, 0);
		t += 850; (void)kvm_stream_tick(s, t, 0, NULL);
	}
}

static void test_app_limited(uint32_t t0)
{
	static kvm_stream s;
	const uint32_t tEnd = t0 + 100u + 900u + 6u * 850u;
	// no estimate at all (the window of the start): a refusal of the window is evidence, as it always was
	activate(&s, t0);
	CHECK_EQ(kvm_stream_note_link_held(&s, t0 + 10), 1);
	CHECK_EQ(kvm_stream_link_limited(&s, t0 + 10), 1);
	// the trickle of markers, 3, 5 and 8 KB: a sample exists (a lower bound), it sizes nothing that a refusal can use: the refusal is no evidence, R is not taken from it
	{
		static const uint32_t kb[4] = { 0u, 3000u, 5000u, 8000u };
		int k;
		for (k = 0; k < 4; k++)
		{
			trickle_run(&s, t0, kb[k], 0);
			CHECK(kvm_stream_rate(&s, tEnd) > 0 && kvm_stream_rate(&s, tEnd) < 12000u);
			CHECK_EQ(kvm_stream_note_link_held(&s, tEnd), 0);
			CHECK_EQ(kvm_stream_link_limited(&s, tEnd), 0);
			CHECK_EQ(kvm_stream_window(&s, tEnd), KVM_STREAM_W_MIN);
			CHECK_EQ(s.rateMemFull, 0);												// (the memory that the trickle raised from nothing is a lower bound)
		}
	}
	// the same trickle, but the window held the sender back in the span (a refused gate): it is what the link took, at any rate: a refusal is evidence
	trickle_run(&s, t0, 5000u, 1);
	CHECK(kvm_stream_rate(&s, tEnd) > 0 && kvm_stream_rate(&s, tEnd) < 12000u);
	CHECK_EQ(kvm_stream_note_link_held(&s, tEnd), 1);
	// it only raises and never lowers: 5 MB/s remembered (samples of a window that held the sender back), a pause, then a trickle and a refusal of the window: the estimate stays the memory
	{
		static kvm_stream f;
		static sim_link l;
		uint32_t t = t0;
		uint64_t mem;
		int i;
		activate(&f, t0);
		memset(&l, 0, sizeof(l));
		l.rateBps = 5u * 1000u * 1000u; l.oneWayMs = 5; l.chunk = 32768;
		run_sim(&f, &l, t0, &t, 6000, NULL, NULL, NULL);
		l.chunk = 0;
		run_sim(&f, &l, t0, &t, 2600, NULL, NULL, NULL);
		mem = kvm_stream_rate_or_mem(&f, t);
		CHECK(mem > 1000000u);
		CHECK_EQ(f.rateMemFull, 1);												// (the memory of a window-limited sample is evidence of the link)
		for (i = 0; i < 6; i++)
		{
			sim_pos p;
			wr_data(&f, t, 5000u); p = wr_marker(&f, t, 1); (void)send_ack(&f, p, t + 600, 0);
			t += 850; (void)kvm_stream_tick(&f, t, 0, NULL);
		}
		CHECK(kvm_stream_rate(&f, t) > 0 && kvm_stream_rate(&f, t) < 20000u);	// (what the trickle says)
		CHECK_EQ(kvm_stream_note_link_held(&f, t), 1);							// the memory is evidence of the link: a refusal is evidence
		CHECK(kvm_stream_rate_or_mem(&f, t) >= mem * 9 / 10);					// ... and the trickle is not the rate, with or without a refusal: it never lowers
		// the memory is evidence of the link only while it is held (KVM_STREAM_MEM_HOLD_MS): after 300 s of idle it has decayed to 5 KB/s (a memory of 5 MB/s halved ten times), a trickle of 3.5 KB/s does not
		// raise it, and a refusal of the window behind the new key frame is no evidence: R must not be taken from a rate that is a few B/s (the scene M5 of kvm_idle_sim_test.c: 2 / 27 frames before)
		{
			sim_pos q;
			uint32_t t2;
			CHECK_EQ(f.rateMemFull, 1);
			l.chunk = 0;
			run_sim(&f, &l, t0, &t, 300000, NULL, NULL, NULL);
			CHECK(kvm_stream_rate_or_mem(&f, t) < 10000u && kvm_stream_rate_or_mem(&f, t) > 0);
			for (i = 0; i < 6; i++)
			{
				wr_data(&f, t, 2000u); q = wr_marker(&f, t, 1); (void)send_ack(&f, q, t + 600, 0);
				t += 850; (void)kvm_stream_tick(&f, t, 0, NULL);
			}
			t2 = t;
			CHECK_EQ(f.rateMemFull, 1);										// (the flag itself stays: the trickle is below the memory and raises nothing)
			CHECK_EQ(kvm_stream_note_link_held(&f, t2), 0);
		}
	}
	// a link that falls (40 Mb/s to 800 kb/s) while the sender sends 1.6 MB/s: first the pipe empties (the memory is a lower bound), then the window holds the sender back: samples that the link took,
	// the refusal is evidence, and the estimate follows the link down although the memory is higher
	{
		static kvm_stream f;
		static sim_link l;
		uint32_t t = t0;
		int i;
		sim_pos p;
		activate(&f, t0);
		memset(&l, 0, sizeof(l));
		for (i = 0; i < 60; i++)											// 6 s: 80 KB every 100 ms (800 KB/s) on a fast link, each acknowledged 10 ms later: the pipe is empty every time
		{
			t += 100; wr_data(&f, t, 80000u); p = wr_marker(&f, t, 0); (void)send_ack(&f, p, t + 10, 0);
		}
		CHECK(kvm_stream_rate_or_mem(&f, t + 10) > 500000u);
		CHECK_EQ(f.rateMemFull, 0);											// (lower bounds: the pipe emptied all the time)
		l.rateBps = 100u * 1000u; l.oneWayMs = 5; l.chunk = 2000; l.freeUs = (uint64_t)(t + 10 - t0) * 1000u;	// the link falls to 800 kb/s, the sender sends 400 KB/s
		t += 10;
		run_sim(&f, &l, t0, &t, 8000, NULL, NULL, NULL);
		CHECK(kvm_stream_link_limited(&f, t));
		CHECK(kvm_stream_rate_or_mem(&f, t) < 200000u);
	}
	// the two sides of the evidence of a span (the pipe was empty at some moment or never): the same bytes and the same time, only the pipe differs
	{
		static kvm_stream a, c;
		uint32_t u = t0, v = t0;
		sim_pos p;
		activate(&a, t0); activate(&c, t0);
		// a: three writes, each acknowledged 10 ms later (the pipe empties): app-limited. c: the same writes, the acknowledgements 400 ms late (a position is always in flight): the link is clogged
		u += 100; wr_data(&a, u, 574366u); p = wr_marker(&a, u, 0); (void)send_ack(&a, p, u + 70, 0);
		v += 100; wr_data(&c, v, 574366u); p = wr_marker(&c, v, 0); (void)send_ack(&c, p, v + 70, 0);
		u += 900; v += 900;
		{
			int i;
			sim_pos prev = p;
			for (i = 0; i < 4; i++)
			{
				wr_data(&a, u, 6000u); p = wr_marker(&a, u, 0); (void)send_ack(&a, p, u + 10, 0); u += 300;
				// c: each position is acknowledged 400 ms after it was written, so the next one (written after 300 ms) is always in flight when it comes
				wr_data(&c, v, 6000u); { const sim_pos cur = wr_marker(&c, v, 0); if (i > 0) { (void)send_ack(&c, prev, v + 100, 0); } prev = cur; }
				v += 300;
			}
			(void)send_ack(&c, prev, v + 100, 0);
		}
		CHECK_EQ(kvm_stream_note_link_held(&a, u + 200), 0);
		CHECK_EQ(kvm_stream_note_link_held(&c, v + 150), 1);
	}
}

// the rate filter: a slow link that is clogged without the window having refused anything (the content is above what the link takes but below the window, or the memory of the
// rate is gone) makes a slow span in which the pipe NEVER empties: that is what the link takes, and it is a sample (the filter of the trickle may drop only a span in which the sender had nothing to
// send). Without the sample the window stays W_START (256 KB, 16 s at 128 kb/s) until the first refusal. 16 KB/s = 128 kb/s.
static void test_clogged_slow_link(uint32_t t0)
{
	static kvm_stream s, s2;
	static sim_link l, l2;
	uint32_t t = t0, u = t0;
	uint64_t w;
	// a fresh session: 32 KB/s offered, 16 KB/s taken, no gate: the pipe never empties
	activate(&s, t0);
	memset(&l, 0, sizeof(l));
	l.rateBps = 16000; l.oneWayMs = 20; l.chunk = 160; l.noGate = 1;
	run_sim(&s, &l, t0, &t, 6000, NULL, NULL, NULL);
	printf("clogged 128 kb/s, fresh session: the sample %llu B/s, window %llu KiB\n", (unsigned long long)kvm_stream_rate(&s, t), (unsigned long long)(kvm_stream_window(&s, t) / 1024));
	CHECK(kvm_stream_rate(&s, t) >= 12000u && kvm_stream_rate(&s, t) <= 20000u);
	CHECK_EQ(kvm_stream_window(&s, t), KVM_STREAM_W_MIN);
	// after a pause of 8 minutes (the memory of the rate is gone), the same clogged link
	activate(&s2, t0);
	memset(&l2, 0, sizeof(l2));
	l2.rateBps = 16000; l2.oneWayMs = 20; l2.chunk = 800;
	run_sim(&s2, &l2, t0, &u, 8000, NULL, NULL, NULL);			// the window holds the sender back: samples, the memory
	CHECK(kvm_stream_rate(&s2, u) > 0);
	l2.chunk = 0;
	run_sim(&s2, &l2, t0, &u, 480000, NULL, NULL, NULL);			// 8 minutes of nothing
	CHECK_EQ(kvm_stream_rate_or_mem(&s2, u), 0);
	l2.chunk = 160; l2.noGate = 1;
	run_sim(&s2, &l2, t0, &u, 6000, NULL, NULL, NULL);
	w = kvm_stream_window(&s2, u);
	printf("clogged 128 kb/s after a pause of 8 min: the sample %llu B/s, window %llu KiB\n", (unsigned long long)kvm_stream_rate(&s2, u), (unsigned long long)(w / 1024));
	CHECK(kvm_stream_rate(&s2, u) >= 12000u && kvm_stream_rate(&s2, u) <= 20000u);
	CHECK_EQ(w, KVM_STREAM_W_MIN);
}

// the first minRtt does not take the queue of the first picture; the RTT probe takes the standing queue out of minRtt
static void test_rtt_probe(uint32_t t0)
{
	static kvm_stream s;
	static sim_link l;
	uint32_t t = t0;
	uint64_t w, expect;
	uint32_t starts, i;
	activate(&s, t0);
	memset(&l, 0, sizeof(l));
	l.rateBps = 375000; l.oneWayMs = 40; l.chunk = 6000;	// 3 Mb/s, 40 ms; the sender wants 1.2 MB/s
	// 1 MB of the first picture is in the pipe when op 4 arrives: the link is busy with it for 2.7 s before the first position
	l.freeUs = 2666000;
	wr_data(&s, t0, 1000000);	// part of what the link is busy with (written after the activation, in flight)
	l.chunk = 6000;
	// first 3 s: no sample from the queue of the picture may become minRtt
	{
		uint32_t tt = t;
		run_sim(&s, &l, t0, &tt, 3000, NULL, NULL, NULL);
		t = tt;
	}
	printf("after 3 s: armed %d minRtt %u ms inflight %llu KiB\n", s.armed, s.minRtt, (unsigned long long)(kvm_stream_inflight(&s) / 1024));
	CHECK(s.minRtt == 0 || s.minRtt < 400);	// never the 700 ms of an earlier table
	{
		uint32_t tt = t;
		uint64_t wmin, wmax;
		run_sim(&s, &l, t0, &tt, 27000, &wmin, &wmax, NULL);	// 30 s in all
		t = tt;
		(void)wmin; (void)wmax;
	}
	w = kvm_stream_window(&s, t);
	expect = (uint64_t)l.rateBps * (40u + KVM_STREAM_QUEUE_DELAY_MS) / 1000u;
	starts = s.st.rttProbeStarts;
	printf("3 Mb/s with a first picture: minRtt %u ms, W = %llu KiB (rate x (RTT + 200 ms) = %llu KiB), inflight %llu KiB (%llu ms of queue), probes: %u started, %u done, %u given up\n", s.minRtt,
		(unsigned long long)(w / 1024), (unsigned long long)(expect / 1024), (unsigned long long)(kvm_stream_inflight(&s) / 1024), (unsigned long long)(kvm_stream_inflight(&s) * 1000 / l.rateBps), starts, s.st.rttProbeDone, s.st.rttProbeAborts);
	CHECK(s.minRtt >= 40 && s.minRtt <= 80);
	CHECK(w >= expect * 8 / 10 && w <= expect * 14 / 10);
	CHECK(kvm_stream_inflight(&s) * 1000 / l.rateBps < 450);	// the standing queue is a fraction of the 0.9 s of an earlier version
	CHECK(starts >= 1 && starts <= 3);	// at most one per 10 s (30 s)
	CHECK_EQ(s.probeState, 0);
	// not more than once per KVM_STREAM_PROBE_INTERVAL_MS: the times of the starts are far apart (count in 20 s windows)
	CHECK(s.st.rttProbeDone + s.st.rttProbeAborts <= 3);
	// the queue of the first picture is not a round trip: nothing is measured until the pipe has been empty once
	{
		static kvm_stream a;
		sim_pos q1, q2, q3, q4;
		activate(&a, t0);
		wr_data(&a, t0 + 1, 500000); q1 = wr_marker(&a, t0 + 1, 0);
		wr_data(&a, t0 + 300, 1000); q2 = wr_marker(&a, t0 + 300, 0);
		wr_data(&a, t0 + 600, 1000); q3 = wr_marker(&a, t0 + 600, 0);
		CHECK_EQ(send_ack(&a, q1, t0 + 2800, 0), KVM_ACK_OK);	// the picture is through after 2.8 s
		CHECK_EQ(send_ack(&a, q2, t0 + 2850, 0), KVM_ACK_OK);
		CHECK_EQ(a.minRtt, 0); CHECK(!a.armed);
		CHECK_EQ(send_ack(&a, q3, t0 + 2900, 0), KVM_ACK_OK);	// the pipe is empty: armed, but these positions were written before
		CHECK(a.armed); CHECK_EQ(a.minRtt, 0);
		wr_data(&a, t0 + 3000, 100); q4 = wr_marker(&a, t0 + 3000, 0);
		CHECK_EQ(send_ack(&a, q4, t0 + 3045, 0), KVM_ACK_OK);
		CHECK_EQ(a.minRtt, 45);
	}
	// unit steps of the probe: the gates are closed while it drains, the marker goes when the pipe is empty, the ACK refreshes minRtt (replacing a larger value)
	{
		static kvm_stream p;
		sim_pos pos[3];
		int pr = 0;
		uint32_t base = t0;
		activate(&p, base);
		wr_data(&p, base + 1, 200000); pos[0] = wr_marker(&p, base + 1, 0);
		CHECK_EQ(send_ack(&p, pos[0], base + 300, 0), KVM_ACK_OK);	// the pipe is empty: armed
		CHECK(p.armed);
		for (i = 0; i < 50; i++)
		{
			wr_data(&p, base + 400 + i * 10, 40000); (void)wr_marker(&p, base + 400 + i * 10, 0);
			(void)kvm_stream_window_allows(&p, KVM_GATE_BIG, base + 400 + i * 10);
			kvm_stream_note_held_back(&p, base + 400 + i * 10);	// the window keeps refusing
		}
		p.minRtt = 900;	// an old, inflated value
		CHECK(!kvm_stream_tick(&p, base + 5000, 0, NULL)); CHECK_EQ(p.probeState, 0);	// too early (RTT window 10 s)
		p.rttStamp = base;	// the value is older than 10 s from now on
		kvm_stream_note_held_back(&p, base + 10000);
		CHECK(!kvm_stream_tick(&p, base + 10000, 0, NULL));
		CHECK_EQ(p.probeState, 1); CHECK_EQ(p.st.rttProbeStarts, 1);
		CHECK(!kvm_stream_window_allows(&p, KVM_GATE_BIG, base + 10001)); CHECK(!kvm_stream_window_allows(&p, KVM_GATE_SMALL, base + 10001)); CHECK(!kvm_stream_window_allows(&p, KVM_GATE_SHARPEN, base + 10001));
		(void)kvm_stream_marker_wanted(&p, base + 10001, 0, &pr); CHECK(pr != 2);	// data still in flight: no RTT probe marker yet
		// everything is acknowledged (the newest position)
		{
			const kvm_stream_pos *last = &p.ring[(p.nextOrd - 1) % KVM_STREAM_RING];
			sim_pos lp; lp.seq = last->seq; lp.ts = last->ts;
			CHECK_EQ(send_ack(&p, lp, base + 10100, 0), KVM_ACK_OK);
		}
		CHECK_EQ(kvm_stream_inflight(&p), 0);
		CHECK(kvm_stream_marker_wanted(&p, base + 10101, 0, &pr)); CHECK_EQ(pr, 2);
		pos[1] = wr_marker(&p, base + 10101, pr);
		CHECK_EQ(p.probeState, 2);
		CHECK(kvm_stream_window_allows(&p, KVM_GATE_BIG, base + 10102));	// the gates are open again, the ACK is waited for
		CHECK(!kvm_stream_marker_wanted(&p, base + 10102, 0, &pr));
		CHECK_EQ(send_ack(&p, pos[1], base + 10145, 0), KVM_ACK_OK);
		CHECK_EQ(p.minRtt, 44); CHECK_EQ(p.probeState, 0); CHECK_EQ(p.st.rttProbeDone, 1);
		// no second probe within the interval, although the value looks old and the window limits
		p.rttStamp = base;
		kvm_stream_note_held_back(&p, base + 15000);
		(void)kvm_stream_tick(&p, base + 15000, 0, NULL); CHECK_EQ(p.probeState, 0);
		kvm_stream_note_held_back(&p, base + 10145 + KVM_STREAM_PROBE_INTERVAL_MS);
		(void)kvm_stream_tick(&p, base + 10145 + KVM_STREAM_PROBE_INTERVAL_MS, 0, NULL); CHECK_EQ(p.probeState, 1);
		// a probe that does not get an empty pipe in time is given up, the gates open, and the next one waits for the interval
		(void)kvm_stream_tick(&p, base + 10145 + KVM_STREAM_PROBE_INTERVAL_MS + 8000, 0, NULL);
		CHECK_EQ(p.probeState, 0); CHECK_EQ(p.st.rttProbeAborts, 1);
		CHECK(kvm_stream_window_allows(&p, KVM_GATE_BIG, base + 30000) || kvm_stream_inflight(&p) > 0);
		// no probe when the window is not what limits the sender (nothing was refused for a long time)
		{
			static kvm_stream q;
			activate(&q, base);
			q.minRtt = 50; q.rttStamp = base;
			(void)kvm_stream_tick(&q, base + 30000, 0, NULL);
			CHECK_EQ(q.probeState, 0);
		}
	}
}

// the window is available only while the adaptive path can use it
static void test_ack_capable(void)
{
	kvm_stream s;
	kvm_state_msg r;
	unsigned act;
	kvm_stream_init(&s, 0, 1, KVM_REASON_NO_VIDEO_BUILD, KVM_REASON_DISABLED);
	CHECK_EQ(ctrl(&s, KVM_OP_ACK_ONLY, 0, 10, &r, &act), 1); CHECK(kvm_stream_ack_active(&s));
	CHECK_EQ(kvm_stream_set_ack_capable(&s, 1, 20, &r), 0);	// no change
	CHECK_EQ(kvm_stream_set_ack_capable(&s, 0, 30, &r), 1);	// the picture type is no longer JPEG
	CHECK(r.reason == KVM_REASON_DISABLED && r.ack == 0 && r.video == 0 && r.caps == 0);
	CHECK(!kvm_stream_ack_active(&s)); CHECK_EQ(s.want, KVM_WANT_NONE);
	CHECK_EQ(ctrl(&s, KVM_OP_ACK_ONLY, 0, 40, &r, &act), 1); CHECK(r.reason == KVM_REASON_DISABLED && !kvm_stream_ack_active(&s));
	CHECK_EQ(kvm_stream_set_ack_capable(&s, 1, 50, &r), 1);	// JPEG again
	CHECK(r.reason == KVM_REASON_AVAILABLE && r.caps == KVM_CAPS_ACK);
	CHECK_EQ(ctrl(&s, KVM_OP_ACK_ONLY, 0, 60, &r, &act), 1); CHECK(kvm_stream_ack_active(&s));
	// switched off by the environment: the type changes do not switch it on
	kvm_stream_init(&s, 0, 0, KVM_REASON_NO_VIDEO_BUILD, KVM_REASON_DISABLED);
	CHECK_EQ(kvm_stream_set_ack_capable(&s, 0, 1, &r), 0); CHECK_EQ(kvm_stream_set_ack_capable(&s, 1, 2, &r), 0); CHECK_EQ(s.capAck, 0);
}

// Rule M1 as a property: whatever mix of writes the agent makes, the iteration ends with a position that covers every byte that is not a 242
static void test_marker_rule_property(uint32_t t0)
{
	static kvm_stream s;
	uint32_t rnd = 12345, t = t0;
	int iter, k;
	activate(&s, t0);
	for (iter = 0; iter < 20000; iter++)
	{
		int n;
		rnd = (rnd * 1103515245u) + 12345u;
		n = (int)((rnd >> 16) % 5);
		for (k = 0; k < n; k++)
		{
			rnd = (rnd * 1103515245u) + 12345u;
			if (((rnd >> 16) % 4) == 0) { kvm_stream_note_write(&s, 16, KVM_W_STATE, t); }
			else { wr_data(&s, t, 1 + ((rnd >> 8) % 3000)); }
		}
		if (kvm_stream_marker_wanted(&s, t, 0, NULL)) { (void)wr_marker(&s, t, 0); }
		// invariant: no data byte is without a later position
		CHECK(!s.dirty);
		{
			uint64_t lastCum = (s.nextOrd > 1) ? s.ring[(s.nextOrd - 1) % KVM_STREAM_RING].cum : s.ackedCum;
			uint64_t after = s.bytesWritten - lastCum;	// bytes after the last position: only 242 may be there
			CHECK(after % 16 == 0);
			if (after % 16 != 0) { break; }
		}
		t += 1 + (rnd % 7);
		if (iter % 3 == 0 && s.nextOrd > 1)	// the viewer acknowledges the newest position now and then
		{
			const kvm_stream_pos *p = &s.ring[(s.nextOrd - 1) % KVM_STREAM_RING];
			sim_pos sp; sp.seq = p->seq; sp.ts = p->ts;
			(void)send_ack(&s, sp, t + 500u, 0);
		}
	}
}

// The same scenarios when the millisecond counter wraps in the middle
static void test_time_wrap(void)
{
	test_ack_basic(0xFFFFFFF0u);
	test_gates(0xFFFFFF00u);
	test_reserved(0xFFFFFF00u);
	test_deadlock_mux_drop(0xFFFFFC00u);
	test_viewer_gone(0xFFFFF000u);
	test_window_follows_link(0xFFFFE000u);
	test_window_memory(0xFFFFD000u);
	test_rtt_probe(0xFFFFFE00u);
}

int main(void)
{
	test_vectors();
	test_watch(1000);
	test_watch(0xFFFFFF00u);
	test_generators();
	test_parsers_extra();
	test_state_machine();
	test_stdin_framing();
	test_veto_tracker();
	test_replay_tracker();
	test_stdin_bad_sizes();
	test_inbox_veto_survives_flood();
	test_inbox_set_then_clear();
	test_inbox_merge();
	test_inbox_rate();
	test_veto_bits();
	test_ack_capable();
	test_ack_basic(1000);
	test_gates(1000);
	test_idr_larger_than_window(1000);
	test_ring(5000);
	test_seq_wrap(1000);
	test_flood(1000);
	test_deadlock_mux_drop(1000);
	test_viewer_gone(1000);
	test_window_follows_link(1000);
	test_window_memory(1000);
	test_rate_memory_pause(1000);
	test_rate_memory_decay(1000);
	test_rate_memory_stall(1000);
	test_rate_memory_link_drop(1000);
	test_rate_memory_rtt_jump(1000);
	test_rate_sample_after_silence(1000);
	test_app_limited(1000);
	test_app_limited(0xFFFFF000u);
	test_clogged_slow_link(1000);
	test_clogged_slow_link(0xFFFFF000u);
	test_rate_sample_burst(1000);
	test_rate_sparse_traffic(1000);
	test_resume_check(1000);
	test_rate_samples_vs_memory(1000);
	test_rtt_probe(1000);
	test_marker_rule_property(1000);
	test_time_wrap();
	printf("%d checks, %d failed\n", gChecks, gFail);
	return gFail == 0 ? 0 : 1;
}
