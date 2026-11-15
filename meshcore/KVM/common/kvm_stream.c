/*
KVM: stream protocol of the commands 240-243 and the end-to-end ACK window. See kvm_stream.h.
Portable C99, no operating system API.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#include "kvm_stream.h"
#include <string.h>

// ---- helpers ----
static unsigned rd16(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }
static uint32_t rd32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void wr16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static int32_t dms(uint32_t a, uint32_t b) { return (int32_t)(a - b); }	// a - b on the wrapping millisecond counter

static const char *const kRejNames[KVM_REJ_COUNT] = {
	"NONE", "BAD_SIZE", "TRUNCATED", "BAD_VERSION", "BAD_OP", "BAD_FLAGS", "BAD_LIMIT", "BAD_FIELD", "BAD_SEQ", "FRAME_TOO_BIG",
	"BAD_MARKER", "BAD_RECT_COUNT", "CODED_SIZE", "NO_SCREEN", "BAD_RECT", "NO_PAYLOAD", "NOT_243"
};

const char *kvm_stream_rej_name(kvm_rej r) { return ((unsigned)r < (unsigned)KVM_REJ_COUNT) ? kRejNames[r] : "?"; }

unsigned kvm_stream_pad32(unsigned v) { return ((v + (KVM_TILE_PAD - 1)) / KVM_TILE_PAD) * KVM_TILE_PAD; }

// ---- parsers ----

// 4.1 steps 1-3: length and size field of a 16 byte command
static kvm_rej ctrl_frame(int cmd, const uint8_t *b, size_t len)
{
	if (b == NULL || len < 4) { return KVM_REJ_TRUNCATED; }
	if (rd16(b + 2) != KVM_CTRL_SIZE) { return KVM_REJ_BAD_SIZE; }
	if (len < KVM_CTRL_SIZE) { return KVM_REJ_TRUNCATED; }
	if (len > KVM_CTRL_SIZE) { return KVM_REJ_BAD_SIZE; }
	if ((int)rd16(b) != cmd) { return KVM_REJ_BAD_OP; }	// not the command this function parses (not a step of 4.1)
	return KVM_REJ_NONE;
}

kvm_rej kvm_stream_parse_ctrl(const uint8_t *b, size_t len, kvm_ctrl *out)
{
	kvm_ctrl c;
	kvm_rej r = ctrl_frame(KVM_CMD_VIDEO_CTRL, b, len);
	if (r != KVM_REJ_NONE) { return r; }
	c.ver = b[4]; c.op = b[5]; c.flags = b[6]; c.maxLevelIdc = b[7];
	c.maxW = (uint16_t)rd16(b + 8); c.maxH = (uint16_t)rd16(b + 10); c.reserved = rd32(b + 12);
	if (c.ver != 1) { return KVM_REJ_BAD_VERSION; }
	if (c.op < KVM_OP_VIDEO_ON || c.op > KVM_OP_VETO_END) { return KVM_REJ_BAD_OP; }
	if ((c.op == KVM_OP_VIDEO_ON && (c.flags & 7) == 0) || (c.op == KVM_OP_REPAIR && (c.flags & 3) == 0) ||
		((c.op == KVM_OP_VETO || c.op == KVM_OP_VETO_END) && (c.flags & KVM_VETO_ALL) == 0)) { return KVM_REJ_BAD_FLAGS; }
	if (c.op == KVM_OP_VIDEO_ON && (c.maxW > KVM_FRAME_MAX_DIM || c.maxH > KVM_FRAME_MAX_DIM)) { return KVM_REJ_BAD_LIMIT; }
	if (out != NULL) { *out = c; }
	return KVM_REJ_NONE;
}

kvm_rej kvm_stream_parse_ack(const uint8_t *b, size_t len, kvm_ack *out)
{
	kvm_ack a;
	kvm_rej r = ctrl_frame(KVM_CMD_STREAM_ACK, b, len);
	if (r != KVM_REJ_NONE) { return r; }
	a.seq = rd32(b + 4); a.ts = rd32(b + 8); a.queueMs = (uint16_t)rd16(b + 12); a.flags = (uint16_t)rd16(b + 14);
	if (a.seq == 0) { return KVM_REJ_BAD_SEQ; }
	if (out != NULL) { *out = a; }
	return KVM_REJ_NONE;
}

kvm_rej kvm_stream_parse_state(const uint8_t *b, size_t len, kvm_state_msg *out)
{
	kvm_state_msg m;
	kvm_rej r = ctrl_frame(KVM_CMD_VIDEO_STATE, b, len);
	if (r != KVM_REJ_NONE) { return r; }
	m.ver = b[4]; m.video = b[5]; m.ack = b[6]; m.reason = b[7]; m.profile = b[8]; m.level = b[9];
	m.codedW = (uint16_t)rd16(b + 10); m.codedH = (uint16_t)rd16(b + 12); m.caps = b[14]; m.gen = b[15];
	if (m.ver != 1) { return KVM_REJ_BAD_VERSION; }
	if (m.video > 1 || m.ack > 1 || (m.video == 1 && m.ack == 0)) { return KVM_REJ_BAD_FIELD; }
	if (out != NULL) { *out = m; }
	return KVM_REJ_NONE;
}

// 4.2 steps 1-10. The NAL checks (step 11) are the viewer's.
kvm_rej kvm_stream_parse_frame(const uint8_t *b, size_t len, unsigned screenW, unsigned screenH, kvm_frame *out)
{
	kvm_frame f;
	const uint8_t *in;
	unsigned i;
	if (b == NULL || len < 12 || rd16(b) != KVM_CMD_JUMBO || rd16(b + 2) != 8) { return KVM_REJ_BAD_SIZE; }
	f.n = rd32(b + 4);
	if (f.n > KVM_FRAME_MAX_N) { return KVM_REJ_FRAME_TOO_BIG; }
	if (f.n < KVM_MARKER_INNER) { return KVM_REJ_BAD_SIZE; }
	if ((uint64_t)len < 8u + (uint64_t)f.n) { return KVM_REJ_TRUNCATED; }	// bytes after 8 + N are ignored (padding added by a transport)
	in = b + 8;
	if (rd16(in) != KVM_CMD_VIDEO_FRAME) { return KVM_REJ_NOT_243; }
	f.seq = rd32(in + 4); f.ts = rd32(in + 8); f.flags = in[12]; f.gen = in[13];
	f.codedW = (uint16_t)rd16(in + 14); f.codedH = (uint16_t)rd16(in + 16); f.nRects = (uint16_t)rd16(in + 18);
	f.rects = NULL; f.annexB = NULL; f.annexLen = 0;
	if (f.seq == 0) { return KVM_REJ_BAD_SEQ; }
	if ((f.flags & (KVM_FRAME_IDR | KVM_FRAME_MARKER)) == (KVM_FRAME_IDR | KVM_FRAME_MARKER)) { return KVM_REJ_BAD_MARKER; }
	if (f.flags & KVM_FRAME_MARKER)
	{
		if (f.n != KVM_MARKER_INNER || f.nRects != 0 || f.codedW != 0 || f.codedH != 0) { return KVM_REJ_BAD_MARKER; }
		if (out != NULL) { *out = f; }
		return KVM_REJ_NONE;
	}
	if (f.nRects < 1 || f.nRects > KVM_FRAME_MAX_RECTS) { return KVM_REJ_BAD_RECT_COUNT; }	// before the length of the data
	if (20u + (8u * f.nRects) > f.n) { return KVM_REJ_TRUNCATED; }
	if (screenW == 0 || screenH == 0) { return KVM_REJ_NO_SCREEN; }
	if (f.codedW != kvm_stream_pad32(screenW) || f.codedH != kvm_stream_pad32(screenH) || f.codedW < KVM_TILE_PAD || f.codedH < KVM_TILE_PAD ||
		f.codedW > KVM_FRAME_MAX_DIM || f.codedH > KVM_FRAME_MAX_DIM) { return KVM_REJ_CODED_SIZE; }
	for (i = 0; i < f.nRects; i++)
	{
		const uint8_t *r = in + 20 + (8u * i);
		unsigned x = rd16(r), y = rd16(r + 2), w = rd16(r + 4), h = rd16(r + 6);
		if (w < 1 || h < 1 || x + w > screenW || y + h > screenH) { return KVM_REJ_BAD_RECT; }
	}
	f.annexLen = (size_t)f.n - 20u - (8u * f.nRects);
	if (f.annexLen < 5) { return KVM_REJ_NO_PAYLOAD; }
	f.rects = in + 20;
	f.annexB = in + 20 + (8u * f.nRects);
	if (out != NULL) { *out = f; }
	return KVM_REJ_NONE;
}

kvm_rej kvm_stream_validate(int cmd, const uint8_t *b, size_t len, unsigned screenW, unsigned screenH)
{
	switch (cmd)
	{
	case KVM_CMD_VIDEO_CTRL: return kvm_stream_parse_ctrl(b, len, NULL);
	case KVM_CMD_STREAM_ACK: return kvm_stream_parse_ack(b, len, NULL);
	case KVM_CMD_VIDEO_STATE: return kvm_stream_parse_state(b, len, NULL);
	case KVM_CMD_VIDEO_FRAME: return kvm_stream_parse_frame(b, len, screenW, screenH, NULL);
	default: return KVM_REJ_BAD_OP;
	}
}

// ---- builders ----

void kvm_stream_build_ctrl(uint8_t out[16], const kvm_ctrl *c)
{
	wr16(out, KVM_CMD_VIDEO_CTRL); wr16(out + 2, KVM_CTRL_SIZE);
	out[4] = c->ver; out[5] = c->op; out[6] = c->flags; out[7] = c->maxLevelIdc;
	wr16(out + 8, c->maxW); wr16(out + 10, c->maxH); wr32(out + 12, c->reserved);
}

void kvm_stream_build_ack(uint8_t out[16], const kvm_ack *a)
{
	wr16(out, KVM_CMD_STREAM_ACK); wr16(out + 2, KVM_CTRL_SIZE);
	wr32(out + 4, a->seq); wr32(out + 8, a->ts); wr16(out + 12, a->queueMs); wr16(out + 14, a->flags);
}

void kvm_stream_build_state(uint8_t out[16], const kvm_state_msg *m)
{
	wr16(out, KVM_CMD_VIDEO_STATE); wr16(out + 2, KVM_CTRL_SIZE);
	out[4] = m->ver; out[5] = m->video; out[6] = m->ack; out[7] = m->reason; out[8] = m->profile; out[9] = m->level;
	wr16(out + 10, m->codedW); wr16(out + 12, m->codedH); out[14] = m->caps; out[15] = m->gen;
}

size_t kvm_stream_frame_header_len(unsigned nRects) { return 28u + (8u * (size_t)nRects); }

size_t kvm_stream_build_frame_header(uint8_t *out, size_t cap, const kvm_frame_params *p)
{
	uint64_t n;
	size_t hdr;
	unsigned i;
	int marker = (p->flags & KVM_FRAME_MARKER) != 0;
	if (out == NULL || p->nRects > KVM_FRAME_MAX_RECTS) { return 0; }
	if (!marker && p->nRects == 0) { return 0; }
	if (p->nRects > 0 && p->rects == NULL) { return 0; }
	n = 20u + (8u * (uint64_t)p->nRects) + (uint64_t)p->annexLen;
	if (n > KVM_FRAME_MAX_N) { return 0; }
	hdr = kvm_stream_frame_header_len(p->nRects);
	if (hdr > cap) { return 0; }
	wr16(out, KVM_CMD_JUMBO); wr16(out + 2, 8); wr32(out + 4, (uint32_t)n);
	wr16(out + 8, KVM_CMD_VIDEO_FRAME); wr16(out + 10, 0); wr32(out + 12, p->seq); wr32(out + 16, p->ts);
	out[20] = p->flags; out[21] = p->gen; wr16(out + 22, p->codedW); wr16(out + 24, p->codedH); wr16(out + 26, p->nRects);
	for (i = 0; i < p->nRects; i++)
	{
		wr16(out + 28 + (8u * i), p->rects[(4u * i)]); wr16(out + 30 + (8u * i), p->rects[(4u * i) + 1]);
		wr16(out + 32 + (8u * i), p->rects[(4u * i) + 2]); wr16(out + 34 + (8u * i), p->rects[(4u * i) + 3]);
	}
	return hdr;
}

size_t kvm_stream_build_frame(uint8_t *out, size_t cap, const kvm_frame_params *p)
{
	size_t hdr, total;
	if (out == NULL || p->nRects > KVM_FRAME_MAX_RECTS) { return 0; }
	if (p->annexLen > 0 && p->annexB == NULL) { return 0; }
	if (p->annexLen > (size_t)KVM_FRAME_MAX_N) { return 0; }
	total = kvm_stream_frame_header_len(p->nRects) + p->annexLen;
	if (total > cap) { return 0; }
	hdr = kvm_stream_build_frame_header(out, cap, p);
	if (hdr == 0) { return 0; }
	if (p->annexLen > 0) { memcpy(out + hdr, p->annexB, p->annexLen); }
	return total;
}

void kvm_stream_build_marker(uint8_t out[KVM_MARKER_BYTES], uint32_t seq, uint32_t ts, uint8_t gen)
{
	kvm_frame_params p;
	memset(&p, 0, sizeof(p));
	p.seq = seq; p.ts = ts; p.flags = KVM_FRAME_MARKER; p.gen = gen;
	(void)kvm_stream_build_frame(out, KVM_MARKER_BYTES, &p);
}

uint32_t kvm_stream_seq_next(uint32_t last) { return (last == 0xFFFFFFFFu) ? 1u : (last + 1u); }

int kvm_stream_seq_is_gap(int haveLast, uint32_t last, uint32_t seq, int idr)
{
	return haveLast && !idr && seq != kvm_stream_seq_next(last);
}

// ---- state ----

void kvm_stream_init(kvm_stream *s, int capVideo, int capAck, int videoOffReason, int ackOffReason)
{
	memset(s, 0, sizeof(*s));
	s->capVideo = (uint8_t)(capVideo ? 1 : 0);
	s->capAck = (uint8_t)(capAck ? 1 : 0);
	s->capAckCfg = s->capAck;
	s->adaptOk = 1;
	s->videoOffReason = (uint8_t)videoOffReason;
	s->ackOffReason = (uint8_t)ackOffReason;
	s->want = KVM_WANT_NONE;
}

void kvm_stream_state_msg(const kvm_stream *s, int reason, kvm_state_msg *out)
{
	memset(out, 0, sizeof(*out));
	out->ver = 1;
	out->video = s->videoOn ? 1 : 0;
	out->ack = s->ackActive ? 1 : 0;
	out->reason = (uint8_t)reason;
	out->caps = (uint8_t)((s->capVideo ? KVM_CAPS_H264 : 0) | (s->capAck ? KVM_CAPS_ACK : 0));
	out->gen = s->gen;
}

void kvm_stream_start_state(const kvm_stream *s, kvm_state_msg *reply) { kvm_stream_state_msg(s, KVM_REASON_STARTED, reply); }

void kvm_stream_bump_gen(kvm_stream *s) { s->gen = (uint8_t)(s->gen + 1u); }

int kvm_stream_ack_active(const kvm_stream *s) { return s->ackActive; }
uint64_t kvm_stream_inflight(const kvm_stream *s) { return s->ackActive ? (s->bytesWritten - s->ackedCum) : 0; }

static void ack_activate(kvm_stream *s, uint32_t now)
{
	s->ackActive = 1;
	s->dirty = 0; s->everHeld = 0; s->stallNoted = 0;
	s->lastProgress = now; s->lastProbe = now; s->lastHeldBack = now; s->linkHeldEver = 0; s->lastLinkHeld = now;
	s->minRtt = 0; s->rttStamp = now; s->armed = 0; s->armedOrd = 0; s->rateMem = 0; s->rateMemAt = 0; s->busyFrom = 0; s->emptySince = 0; s->haveEmpty = 0; s->resumeArmed = 0; s->rateMemFull = 0;
	s->probeState = 0; s->lastProbeEnd = now;
	s->ackedCum = s->bytesWritten;	// what was written before the activation is not waited for
	s->nextOrd = 1; s->ackedOrd = 0;
	s->tbTime = now; s->tokensMilli = (uint32_t)KVM_STREAM_ACK_BURST * 1000u;
	s->nHist = 0; s->histStart = 0; s->nSamples = 0; s->sampleHead = 0;
}

static void ack_deactivate(kvm_stream *s)
{
	s->ackActive = 0; s->videoOn = 0; s->dirty = 0; s->probeState = 0;
	s->nextOrd = 1; s->ackedOrd = 0;
	s->nHist = 0; s->nSamples = 0;
}

// ackOn = capAck and want >= ACK and no veto (5.1); called after every change of those
static void ack_update(kvm_stream *s, uint32_t now)
{
	int on = s->capAck && s->want >= KVM_WANT_ACK && s->vetoMask == 0;
	if (on && !s->ackActive) { ack_activate(s, now); }
	else if (!on && s->ackActive) { ack_deactivate(s); }
}

int kvm_stream_set_ack_capable(kvm_stream *s, int capable, uint32_t now, kvm_state_msg *reply)
{
	capable = capable ? 1 : 0;
	if (capable == s->adaptOk) { return 0; }
	s->adaptOk = (uint8_t)capable;
	if (!s->capAckCfg) { return 0; }	// switched off by the build or the environment anyway
	if (!capable)
	{
		s->capAck = 0;
		s->want = KVM_WANT_NONE;	// the viewer asks again when ACK is available again
		ack_update(s, now);
		if (reply != NULL) { kvm_stream_state_msg(s, s->ackOffReason, reply); }
		return 1;
	}
	s->capAck = 1;
	if (reply != NULL) { kvm_stream_state_msg(s, KVM_REASON_AVAILABLE, reply); }
	return 1;
}

int kvm_stream_on_ctrl(kvm_stream *s, const kvm_ctrl *c, uint32_t now, kvm_state_msg *reply, unsigned *actions)
{
	unsigned act = 0;
	int reason = KVM_REASON_OK, send = 1;
	int veto = s->vetoMask != 0;
	int wasVideo = s->videoOn;
	s->st.ctrlOk++;
	switch (c->op)
	{
	case KVM_OP_VIDEO_ON:
		if (veto) { reason = KVM_REASON_VETO; }
		else if (!s->capAck) { reason = s->ackOffReason; }
		else if (!s->capVideo)
		{
			s->want = KVM_WANT_ACK;		// the ACK window works, the picture stays JPEG
			ack_update(s, now);
			reason = s->videoOffReason;
		}
		else
		{
			s->want = KVM_WANT_VIDEO;	// the video code starts the encoder and answers (gen++, IDR, profile, level, coded)
			s->lastOp1 = now; s->haveOp1 = 1;
			ack_update(s, now);
			act |= KVM_ACT_VIDEO_REQUEST;
			send = 0;
		}
		break;
	case KVM_OP_VIDEO_OFF:
		if (veto) { reason = KVM_REASON_VETO; }
		else if (!s->capAck) { reason = s->ackOffReason; }
		else
		{
			if (s->want == KVM_WANT_VIDEO) { s->want = KVM_WANT_ACK; }
			s->videoOn = 0;
			if (wasVideo) { act |= KVM_ACT_VIDEO_STOP; }
		}
		break;
	case KVM_OP_REPAIR:
		send = 0;	// never answered; ignored while video is off
		if (s->videoOn)
		{
			if (c->flags & KVM_REPAIR_IDR) { act |= KVM_ACT_IDR; }
			if (c->flags & KVM_REPAIR_TILES) { act |= KVM_ACT_REPAIR; }
		}
		break;
	case KVM_OP_ACK_ONLY:
		if (veto) { reason = KVM_REASON_VETO; }
		else if (!s->capAck) { reason = s->ackOffReason; }
		else
		{
			s->want = KVM_WANT_ACK;
			s->videoOn = 0;
			if (wasVideo) { act |= KVM_ACT_VIDEO_STOP; }
			ack_update(s, now);
		}
		break;
	case KVM_OP_VETO:
		s->vetoMask = (uint8_t)(s->vetoMask | (c->flags & KVM_VETO_ALL));	// only the defined sources: a reserved bit is never kept
		if (wasVideo) { act |= KVM_ACT_VIDEO_STOP; }
		ack_update(s, now);	// ACK off: the window no longer limits the writes
		reason = KVM_REASON_VETO;
		break;
	case KVM_OP_VETO_END:
	{
		uint8_t old = s->vetoMask;
		s->vetoMask = (uint8_t)(s->vetoMask & ~(c->flags & KVM_VETO_ALL));
		if (s->vetoMask != 0) { reason = KVM_REASON_VETO; }
		else if (old != 0)
		{
			s->want = KVM_WANT_NONE;	// the viewer has to ask again
			ack_update(s, now);
			reason = KVM_REASON_AVAILABLE;
		}
		break;	// a spurious op 6 (no veto set): nothing changes, answered with the current state, reason 0
	}
	default:
		send = 0;
		break;
	}
	if (actions != NULL) { *actions = act; }
	if (send && reply != NULL) { kvm_stream_state_msg(s, reason, reply); }
	return send;
}

// ---- ring, rate, window ----

static const kvm_stream_pos *ring_find(const kvm_stream *s, uint32_t seq, uint32_t ts, uint64_t *ordOut)
{
	uint64_t lo = (s->nextOrd > (uint64_t)KVM_STREAM_RING) ? (s->nextOrd - (uint64_t)KVM_STREAM_RING) : 1u;
	uint64_t o;
	for (o = s->nextOrd; o-- > lo;)
	{
		const kvm_stream_pos *p = &s->ring[o % KVM_STREAM_RING];
		if (p->seq == seq && p->ts == ts) { *ordOut = o; return p; }
	}
	return NULL;
}

static uint64_t max_rate(const kvm_stream *s, uint32_t now)
{
	uint64_t m = 0;
	uint32_t i;
	for (i = 0; i < s->nSamples; i++)
	{
		const kvm_stream_sample *x = &s->samples[i];
		if (dms(now, x->t) < KVM_STREAM_RATE_SPAN_MS && x->rate > m) { m = x->rate; }
	}
	return m;
}

// the samples of the last 2 s that are no lower bound (the window held the sender back, or the pipe never emptied: what the link takes)
static uint64_t max_rate_full(const kvm_stream *s, uint32_t now)
{
	uint64_t m = 0;
	uint32_t i;
	for (i = 0; i < s->nSamples; i++)
	{
		const kvm_stream_sample *x = &s->samples[i];
		if (!x->appLtd && dms(now, x->t) < KVM_STREAM_RATE_SPAN_MS && x->rate > m) { m = x->rate; }
	}
	return m;
}

uint64_t kvm_stream_rate(const kvm_stream *s, uint32_t now) { return s->ackActive ? max_rate(s, now) : 0; }

// the remembered rate as it is now: kept KVM_STREAM_MEM_HOLD_MS without a confirmation, then halved, and halved again every KVM_STREAM_MEM_HALF_MS (a link that nobody has used for minutes
// is not known any more, but a pause of a few seconds between two series of motion is not a slow link)
static uint64_t mem_eff(const kvm_stream *s, uint32_t now)
{
	int32_t age;
	uint32_t steps;
	if (s->rateMem == 0) { return 0; }
	age = dms(now, s->rateMemAt);
	if (age < (int32_t)KVM_STREAM_MEM_HOLD_MS) { return s->rateMem; }
	steps = 1u + (uint32_t)(age - (int32_t)KVM_STREAM_MEM_HOLD_MS) / (uint32_t)KVM_STREAM_MEM_HALF_MS;
	return steps >= 40u ? 0u : (s->rateMem >> steps);
}

// The rate to size the window and R with. While the window held a video frame or a big JPEG rectangle back in the last second (kvm_stream_link_limited) the samples of the last 2 s are what the link
// takes. Otherwise a sample only says how much was sent (a trickle after a pause, the tail of a sharpening that its own gate held), so the remembered rate (the highest one seen, kept 30 s) counts too:
// the larger of the two (a series of motion after a pause does not begin with the rate of the last trickle)
static uint64_t rate_or_mem(const kvm_stream *s, uint32_t now);
// a refusal of the window is evidence that the link is the limit only while the estimate is not just lower bounds. When all there is are samples (and a memory) of a pipe that was empty (a cursor,
// a caret, light work: how little there was to send), the window was sized by what the sender had to send, and its refusal says that the estimate is too low, not that the link is slow: R must not be
// taken from it (measured on the Windows 11 VM, and the scene E: R 100 kb/s). With no estimate at all (the window of the start) a refusal is evidence, as it always was.
static int estimate_is_lower_bound(const kvm_stream *s, uint32_t now)
{
	if (max_rate_full(s, now) != 0) { return 0; }
	// the memory is evidence of the link only while it is held (KVM_STREAM_MEM_HOLD_MS): what has decayed to a few B/s after minutes of idle is no estimate of anything, and a refusal behind the key frame must not take R from it
	if (mem_eff(s, now) != 0 && s->rateMemFull && dms(now, s->rateMemAt) < (int32_t)KVM_STREAM_MEM_HOLD_MS) { return 0; }
	return max_rate(s, now) != 0 || mem_eff(s, now) != 0;
}

int kvm_stream_link_limited(const kvm_stream *s, uint32_t now) { return s->ackActive && s->linkHeldEver && dms(now, s->lastLinkHeld) < (int32_t)KVM_STREAM_HELD_MS && !estimate_is_lower_bound(s, now); }

static uint64_t rate_or_mem(const kvm_stream *s, uint32_t now)
{
	const uint64_t r = max_rate(s, now), m = mem_eff(s, now);
	// only samples that are no lower bound (the window held the sender back, or the pipe never emptied) can lower the estimate below the memory: a refusal of the window while there are only
	// samples of a pipe that was empty (a trickle after a pause) does not make the trickle the rate
	if (r != 0 && kvm_stream_link_limited(s, now) && max_rate_full(s, now) != 0) { return r; }
	return r > m ? r : m;
}

uint64_t kvm_stream_rate_or_mem(const kvm_stream *s, uint32_t now) { return s->ackActive ? rate_or_mem(s, now) : 0; }

uint64_t kvm_stream_rate_recent(const kvm_stream *s, uint32_t now)
{
	uint64_t r;
	if (!s->ackActive) { return 0; }
	r = max_rate(s, now);
	return r != 0 ? r : mem_eff(s, now);
}

// bytes in flight right after a position up to which its round trip is "clean" (no queue of its own in front of it)
static uint64_t flight_low(const kvm_stream *s, uint32_t now)
{
	uint64_t f = (rate_or_mem(s, now) * KVM_STREAM_PROBE_QDELAY_MS) / 1000u;
	return f > KVM_STREAM_PROBE_FLIGHT ? f : KVM_STREAM_PROBE_FLIGHT;
}

static void sample_add(kvm_stream *s, uint32_t now, uint64_t rate, int appLtd)
{
	s->samples[s->sampleHead].t = now; s->samples[s->sampleHead].rate = rate; s->samples[s->sampleHead].appLtd = (uint8_t)appLtd;
	s->sampleHead = (s->sampleHead + 1u) % KVM_STREAM_SAMPLES;
	if (s->nSamples < KVM_STREAM_SAMPLES) { s->nSamples++; }
}

// Records an accepted ACK and, when a record at least KVM_STREAM_RATE_MIN_MS old exists, a rate sample over the span from it to now
// (the bytes acknowledged since that record). A sample from a time when the sender was never held back by the window only says how much it had to
// send, not what the link can do: it is kept only when it raises the rate (the same rule as K2 in the multiplexer).
static void rate_on_ack(kvm_stream *s, uint32_t now)
{
	uint32_t i, base = 0xFFFFFFFFu;
	if (s->nHist == KVM_STREAM_HIST) { s->histStart = (s->histStart + 1u) % KVM_STREAM_HIST; s->nHist--; }
	{
		uint32_t at = (s->histStart + s->nHist) % KVM_STREAM_HIST;
		s->hist[at].t = now; s->hist[at].d = s->ackedCum;
		s->nHist++;
	}
	for (i = s->nHist; i-- > 0;)
	{
		const kvm_stream_hist *h = &s->hist[(s->histStart + i) % KVM_STREAM_HIST];
		if (dms(h->t, s->busyFrom) < 0) { break; }	// a record from before the silence that this period of work began after is no start of a span
		if (dms(now, h->t) >= KVM_STREAM_RATE_MIN_MS) { base = i; break; }
	}
	if (base == 0xFFFFFFFFu) { return; }
	s->histStart = (s->histStart + base) % KVM_STREAM_HIST;	// older records are not needed again
	s->nHist -= base;
	{
		const kvm_stream_hist *h = &s->hist[s->histStart];
		uint32_t dt = (uint32_t)dms(now, h->t);
		uint64_t rate;
		rate = ((s->ackedCum - h->d) * 1000u) / dt;
		int linkLimited = s->everHeld && dms(s->lastHeldBack, h->t) >= 0;
		uint64_t cur = max_rate(s, now);
		// app-limited samples (BBR, GoogCC: the sender had nothing to send). A span in which the pipe was empty at some moment (emptySince is the last ACK that emptied it; the silence
		// before a period of work is cut off above by busyFrom) and which the window did not hold back says how much the sender had to send: a lower bound of what the link takes. It can raise the
		// estimate and the memory and never lowers them (while the window does not hold the sender back the estimate is the larger of the samples and the memory: rate_or_mem; the samples list only says how much is being delivered now). A span in which the window held the sender back, or the pipe never emptied (a clogged link, slow or fast), is what the link takes: it counts
		// at any rate, also a low one (a link of 128 kb/s) and also when it lowers
		const int appLtd = (!linkLimited && s->haveEmpty && dms(s->emptySince, h->t) >= 0) ? 1 : 0;
		if (rate == 0 && !linkLimited) { return; }
		if (!appLtd || rate > cur) { sample_add(s, now, rate, appLtd); }
	}
}

uint32_t kvm_stream_window(const kvm_stream *s, uint32_t now)
{
	uint64_t rate, w;
	if (!s->ackActive) { return KVM_STREAM_W_MAX; }
	if ((s->bytesWritten != s->ackedCum) && dms(now, s->lastProgress) >= KVM_STREAM_STALL_MS) { return KVM_STREAM_W_MIN; }	// no ACK for a while with data in flight
	rate = rate_or_mem(s, now);
	if (rate == 0) { return KVM_STREAM_W_START; }	// before the first sample of this activation
	if (max_rate(s, now) == 0)
	{
		// no sample in the last 2 s: the remembered rate sizes the window (a pause, a series that begins), but the link may have changed meanwhile, so not above KVM_STREAM_W_RESUME until the check of the resume or a sample says so
		w = (rate * ((uint64_t)s->minRtt + KVM_STREAM_QUEUE_DELAY_MS)) / 1000u;
		if (w > KVM_STREAM_W_RESUME) { w = KVM_STREAM_W_RESUME; }
		if (w < KVM_STREAM_W_MIN) { w = KVM_STREAM_W_MIN; }
		return (uint32_t)w;
	}
	w = (rate * ((uint64_t)s->minRtt + KVM_STREAM_QUEUE_DELAY_MS)) / 1000u;
	if (w < KVM_STREAM_W_MIN) { w = KVM_STREAM_W_MIN; }
	if (w > KVM_STREAM_W_MAX) { w = KVM_STREAM_W_MAX; }
	return (uint32_t)w;
}

int kvm_stream_window_allows(const kvm_stream *s, int kind, uint32_t now)
{
	uint64_t inflight, w;
	if (!s->ackActive) { return 1; }
	if (s->probeState == 1) { return 0; }	// RTT probe: nothing new until the pipe has drained (the marker of the probe goes then)
	inflight = s->bytesWritten - s->ackedCum;
	if (kind != KVM_GATE_VIDEO) { inflight += s->reservedBytes; }
	w = kvm_stream_window(s, now);
	switch (kind)
	{
	case KVM_GATE_SMALL: return inflight * 100u < w * KVM_STREAM_SMALL_PCT;
	case KVM_GATE_SHARPEN: return inflight * 100u < w * KVM_STREAM_SHARPEN_PCT;
	default: return inflight < w;	// big rectangles and video frames: checked before the write, the size of the write is not added
	}
}

void kvm_stream_set_reserved(kvm_stream *s, uint32_t bytes) { s->reservedBytes = bytes; }

void kvm_stream_watch(kvm_stream *s, uint32_t seq) { s->watchOn = 1; s->watchDone = 0; s->watchSeq = seq; }

int kvm_stream_watch_result(kvm_stream *s, uint32_t *ackAt, uint32_t *ts, uint32_t *queueMs, uint32_t *minRtt)
{
	if (!s->watchOn || !s->watchDone) { return 0; }
	*ackAt = s->watchAckAt; *ts = s->watchTs; *queueMs = s->watchQueueMs; *minRtt = s->watchMinRtt;
	s->watchOn = 0; s->watchDone = 0;
	return 1;
}

void kvm_stream_note_held_back(kvm_stream *s, uint32_t now)
{
	s->lastHeldBack = now; s->everHeld = 1; s->st.gateDenied++;
}

int kvm_stream_note_link_held(kvm_stream *s, uint32_t now)
{
	kvm_stream_note_held_back(s, now);
	if (s->probeState != 1) { s->lastLinkHeld = now; s->linkHeldEver = 1; }	// during the drain of the RTT probe every gate is closed: that is the probe, not the link
	return s->probeState != 1 && kvm_stream_link_limited(s, now);	// 1 = this refusal is evidence that the link is the limit; 0 = the window was sized by a lower bound (app-limited samples only), the refusal says it was too small
}

int kvm_stream_on_ack(kvm_stream *s, const kvm_ack *a, uint32_t now)
{
	const kvm_stream_pos *p;
	uint64_t ord = 0;
	int32_t el;
	if (!s->ackActive) { s->st.acksOff++; return KVM_ACK_OFF; }
	// token bucket first: a flood is cheap to refuse, a search of the ring is not
	el = dms(now, s->tbTime);
	if (el > 0)
	{
		uint64_t t = (uint64_t)s->tokensMilli + ((uint64_t)el * KVM_STREAM_ACK_RATE);
		uint64_t cap = (uint64_t)KVM_STREAM_ACK_BURST * 1000u;
		s->tokensMilli = (uint32_t)(t > cap ? cap : t);
		s->tbTime = now;
	}
	if (s->tokensMilli < 1000u) { s->st.acksFlood++; return KVM_ACK_FLOOD; }
	s->tokensMilli -= 1000u;
	p = ring_find(s, a->seq, a->ts, &ord);
	if (p == NULL) { s->st.acksUnknown++; return KVM_ACK_UNKNOWN; }
	if (ord <= s->ackedOrd) { s->st.acksStale++; return KVM_ACK_STALE; }
	if (s->watchOn && !s->watchDone && a->seq == s->watchSeq) { s->watchDone = 1; s->watchAckAt = now; s->watchTs = p->ts; s->watchQueueMs = a->queueMs; s->watchMinRtt = s->minRtt; }
	s->ackedOrd = ord;
	s->ackedCum = p->cum;
	if (s->bytesWritten == s->ackedCum) { s->emptySince = now; s->haveEmpty = 1; }	// the pipe is empty from now on, until the next write
	if (dms(now, s->lastProgress) > 0) { s->lastProgress = now; }	// the arrival time of an ACK that waited in a queue can be older than a blocked write that ended since
	s->stallNoted = 0;
	// The pipe is empty after this ACK for the first time: the queue of what was written before (the first picture of the session) is gone, and only positions written
	// from now on say something about the path.
	if (!s->armed && s->bytesWritten == s->ackedCum) { s->armed = 1; s->armedOrd = ord; }
	// Network round trip (8.1): the viewer says how long it held the position. minRtt is the minimum of the last KVM_STREAM_RTT_WINDOW_MS. Positions written behind a queue
	// include its serialization; the RTT probe (kvm_stream_tick) is what gets a clean sample when the window is the limit and the pipe is never empty.
	{
		int32_t rtt = dms(now, p->ts) - (int32_t)a->queueMs;
		int clean = (uint64_t)p->flight <= flight_low(s, now);
		if (rtt < 1) { rtt = 1; }
		if (p->rttProbe)
		{
			// a round trip that has more than doubled (and by 20 ms at least) says the path is not the one the rate was measured on: the remembered rate is void
			if (s->minRtt != 0 && (uint32_t)rtt > 2u * s->minRtt && (uint32_t)rtt >= s->minRtt + 20u) { s->rateMem = 0; }
			s->minRtt = (uint32_t)rtt; s->rttStamp = now;	// the probe replaces the old value, larger or not
			s->probeState = 0; s->lastProbeEnd = now; s->st.rttProbeDone++;
		}
		else if (s->armed && ord > s->armedOrd)
		{
			if (s->minRtt == 0 || (uint32_t)rtt <= s->minRtt || (clean && dms(now, s->rttStamp) > KVM_STREAM_RTT_WINDOW_MS)) { s->minRtt = (uint32_t)rtt; s->rttStamp = now; }
		}
	}
	// the smoothness work, the check of the resume: a period of work began on the remembered rate alone (the link may have changed in the pause). The first position of it with at least KVM_STREAM_RESUME_BYTES in flight
	// when it was written says what the link delivered of that flight: flight / (time to the ACK less what the viewer held it and the network round trip). That is a lower bound of the capacity and does not
	// depend on how fast the sender sent (a slow sender has a small flight). Below half of the remembered rate the memory becomes it, so the window and R follow at once and not after a sample of 500 ms
	if (s->resumeArmed && dms(p->ts, s->busyFrom) >= 0 && p->flight >= KVM_STREAM_RESUME_BYTES)
	{
		int32_t el = dms(now, p->ts) - (int32_t)a->queueMs - (int32_t)s->minRtt;
		const uint64_t d = ((uint64_t)p->flight * 1000u) / (uint64_t)(el < 10 ? 10 : el);
		s->resumeArmed = 0;
		if (d * 100u < mem_eff(s, now) * (uint64_t)KVM_STREAM_RESUME_PCT) { s->rateMem = d; s->rateMemFull = 1; s->rateMemAt = now ? now : 1u; }
	}
	rate_on_ack(s, now);
	{
		// what is remembered is the highest rate that was seen (a maximum filter, as BBR keeps its bottleneck bandwidth): a sample that is lower says how much the sender had to send, or how much
		// a window that held it back took of a burst, not what the link can do. It falls by itself (mem_eff), at a real stall and when the round trip has doubled. While samples exist they, not the memory,
		// size the window and R (rate_or_mem), so a link that got slower during the work is followed within 2 s
		const uint64_t r = max_rate(s, now);
		if (r != 0 && r >= mem_eff(s, now)) { s->rateMemFull = (max_rate_full(s, now) >= r) ? 1u : 0u; s->rateMem = r; s->rateMemAt = now ? now : 1u; }	// (a lower bound raises the memory too, and says so)
	}
	s->st.acksOk++;
	return KVM_ACK_OK;
}

// RTT probe ("ProbeRTT-lite"): minRtt older than KVM_STREAM_RTT_WINDOW_MS, the window is what limits the sender, and none for KVM_STREAM_PROBE_INTERVAL_MS: close the gates until the
// data in flight has drained, then write a marker into the empty pipe; its ACK is a clean round trip. Decided at the start of an iteration, so no picture is cut in the middle.
static void probe_step(kvm_stream *s, uint32_t now)
{
	switch (s->probeState)
	{
	case 0:
		if (dms(now, s->rttStamp) >= KVM_STREAM_RTT_WINDOW_MS && dms(now, s->lastProbeEnd) >= KVM_STREAM_PROBE_INTERVAL_MS &&
			s->everHeld && dms(now, s->lastHeldBack) < KVM_STREAM_HELD_MS)
		{
			uint64_t rate = rate_or_mem(s, now), drain = rate != 0 ? ((s->bytesWritten - s->ackedCum) * 1000u) / rate : 0;
			uint32_t extra = s->minRtt > KVM_STREAM_PROBE_MIN_MS ? s->minRtt : KVM_STREAM_PROBE_MIN_MS;
			if (drain > 5000u) { drain = 5000u; }
			s->probeState = 1;
			s->probeDeadline = now + (uint32_t)drain + extra;
			s->st.rttProbeStarts++;
		}
		break;
	case 1:
		if (dms(now, s->probeDeadline) >= 0) { s->probeState = 0; s->lastProbeEnd = now; s->st.rttProbeAborts++; }
		break;
	default:
		if (dms(now, s->probeSentAt) >= KVM_STREAM_PROBE_WAIT_MS) { s->probeState = 0; s->lastProbeEnd = now; s->st.rttProbeAborts++; }
		break;
	}
}

int kvm_stream_tick(kvm_stream *s, uint32_t now, int blocked, kvm_state_msg *reply)
{
	if (!s->ackActive) { return 0; }
	if (!blocked) { probe_step(s, now); }
	if (blocked || s->bytesWritten == s->ackedCum)
	{
		s->lastProgress = now;	// a blocked write is not the viewer's fault, and nothing in flight is not a missing ACK
		s->stallNoted = 0;
		return 0;
	}
	if (dms(now, s->lastProgress) >= KVM_STREAM_STALL_MS && !s->stallNoted) { s->stallNoted = 1; s->st.stalls++; s->rateMem = 0; }	// a real stall (data in flight, no acknowledgement): what was remembered is void
	if (dms(now, s->lastProgress) >= KVM_STREAM_TIMEOUT_MS)
	{
		s->want = KVM_WANT_NONE;	// the viewer asks again (7.1)
		ack_update(s, now);
		s->st.timeouts++;
		if (reply != NULL) { kvm_stream_state_msg(s, KVM_REASON_NO_ACK, reply); }
		return 1;
	}
	return 0;
}

void kvm_stream_note_blocked(kvm_stream *s, uint32_t to)
{
	if (!s->ackActive) { return; }
	if (dms(to, s->lastProgress) > 0) { s->lastProgress = to; s->stallNoted = 0; }
	if (dms(to, s->lastProbe) > 0) { s->lastProbe = to; }
}

void kvm_stream_note_write(kvm_stream *s, size_t len, int kind, uint32_t now)
{
	if (s->ackActive && s->bytesWritten == s->ackedCum && (!s->haveEmpty || dms(now, s->emptySince) > (int32_t)KVM_STREAM_BUSY_GAP_MS)) { s->busyFrom = now ? now : 1u; s->resumeArmed = (mem_eff(s, now) > max_rate(s, now)) ? 1u : 0u; }	// the first write after the pipe stood empty for a while begins a new period of work
	if (s->ackActive && s->bytesWritten == s->ackedCum) { s->lastProgress = now; s->stallNoted = 0; }	// data starts to be in flight
	s->bytesWritten += (uint64_t)len;
	if (kind == KVM_W_DATA && s->ackActive) { s->dirty = 1; }
}

static uint32_t add_position_ex(kvm_stream *s, uint32_t now, size_t msgBytes, int rttProbe)
{
	uint32_t seq = kvm_stream_seq_next(s->seqCounter);
	kvm_stream_pos *p;
	s->seqCounter = seq;
	if (s->ackActive)
	{
		p = &s->ring[s->nextOrd % KVM_STREAM_RING];	// a full ring overwrites the oldest position
		uint64_t fl;
		p->seq = seq; p->ts = now; p->cum = s->bytesWritten + (uint64_t)msgBytes;
		fl = p->cum - s->ackedCum;
		p->flight = (fl > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)fl;
		p->rttProbe = (uint8_t)(rttProbe ? 1 : 0);
		s->nextOrd++;
	}
	s->dirty = 0;
	return seq;
}

uint32_t kvm_stream_add_position(kvm_stream *s, uint32_t now, size_t msgBytes) { return add_position_ex(s, now, msgBytes, 0); }

int kvm_stream_marker_wanted(const kvm_stream *s, uint32_t now, int blocked, int *probe)
{
	if (probe != NULL) { *probe = 0; }
	if (!s->ackActive || blocked) { return 0; }
	if (s->probeState == 1 && (uint64_t)(s->bytesWritten - s->ackedCum) <= flight_low(s, now))	// the pipe has drained: the marker of the RTT probe
	{
		if (probe != NULL) { *probe = 2; }
		return 1;
	}
	if (s->dirty) { return 1; }	// M1
	if (s->bytesWritten != s->ackedCum && dms(now, s->lastProgress) >= KVM_STREAM_PROBE_MS && dms(now, s->lastProbe) >= KVM_STREAM_PROBE_MS)	// M2
	{
		if (probe != NULL) { *probe = 1; }
		return 1;
	}
	return 0;
}

void kvm_stream_make_marker(kvm_stream *s, uint32_t now, int probe, uint8_t out[KVM_MARKER_BYTES])
{
	uint32_t seq = add_position_ex(s, now, KVM_MARKER_BYTES, probe == 2);
	kvm_stream_build_marker(out, seq, now, s->gen);
	if (probe == 1) { s->lastProbe = now; s->st.probes++; }
	if (probe == 2) { s->probeState = 2; s->probeSentAt = now; }
	s->st.markers++;
}

// ---- inbox of the 240 commands (input thread -> main loop), see kvm_stream.h ----

void kvm_inbox_init(kvm_inbox *q, uint32_t now)
{
	memset(q, 0, sizeof(*q));
	q->tbTime = now;
	q->tokensMilli = (uint32_t)KVM_INBOX_BURST * 1000u;
}

int kvm_inbox_put(kvm_inbox *q, const kvm_ctrl *c, uint32_t at)
{
	if (c->op == KVM_OP_VETO || c->op == KVM_OP_VETO_END)
	{
		uint8_t src = (uint8_t)(c->flags & KVM_VETO_ALL);
		if (c->op == KVM_OP_VETO) { q->vetoTarget = (uint8_t)(q->vetoTarget | src); q->vetoPulse = (uint8_t)(q->vetoPulse | src); }
		else { q->vetoTarget = (uint8_t)(q->vetoTarget & ~src); }
		q->vetoAt = at;
		return KVM_INBOX_STORED;
	}
	{
		int32_t el = dms(at, q->tbTime);	// token bucket first, as for the ACK: a flood is cheap to refuse
		if (el > 0)
		{
			uint64_t t = (uint64_t)q->tokensMilli + ((uint64_t)el * KVM_INBOX_RATE);
			uint64_t cap = (uint64_t)KVM_INBOX_BURST * 1000u;
			q->tokensMilli = (uint32_t)(t > cap ? cap : t);
			q->tbTime = at;
		}
		if (q->tokensMilli < 1000u) { q->nLimited++; return KVM_INBOX_LIMITED; }
		q->tokensMilli -= 1000u;
	}
	if (c->op == KVM_OP_REPAIR)
	{
		if (q->repairFlags != 0) { q->nMerged++; } else { q->repairAt = at; }
		q->repairFlags = (uint8_t)(q->repairFlags | (c->flags & (KVM_REPAIR_IDR | KVM_REPAIR_TILES)));
	}
	else
	{
		if (q->haveState) { q->nMerged++; }	// the last request of the viewer wins: it is what the viewer wants now
		q->state = *c; q->stateAt = at; q->haveState = 1;
	}
	return KVM_INBOX_STORED;
}

void kvm_inbox_take(kvm_inbox *q, kvm_inbox *batch)
{
	*batch = *q;
	q->vetoPulse = 0; q->haveState = 0; q->repairFlags = 0;
}

size_t kvm_inbox_expand(const kvm_inbox *b, uint8_t curVeto, kvm_inbox_op out[KVM_INBOX_OPS])
{
	size_t n = 0;
	uint8_t tgt = (uint8_t)(b->vetoTarget & KVM_VETO_ALL);
	uint8_t set = (uint8_t)((b->vetoPulse | (tgt & ~curVeto)) & KVM_VETO_ALL);	// the second term heals a stream that was reset behind the inbox
	uint8_t clr = (uint8_t)(((curVeto | set) & ~tgt) & KVM_VETO_ALL);
	if (set != 0)
	{
		memset(&out[n], 0, sizeof(out[n]));
		out[n].c.ver = 1; out[n].c.op = KVM_OP_VETO; out[n].c.flags = set; out[n].at = b->vetoAt; n++;
	}
	if (clr != 0)
	{
		memset(&out[n], 0, sizeof(out[n]));
		out[n].c.ver = 1; out[n].c.op = KVM_OP_VETO_END; out[n].c.flags = clr; out[n].at = b->vetoAt; n++;
	}
	if (b->haveState) { out[n].c = b->state; out[n].at = b->stateAt; n++; }
	if (b->repairFlags != 0)
	{
		memset(&out[n], 0, sizeof(out[n]));
		out[n].c.ver = 1; out[n].c.op = KVM_OP_REPAIR; out[n].c.flags = b->repairFlags; out[n].at = b->repairAt; n++;
	}
	return n;
}

// ---- framing of the stdin of the KVM process, veto tracking in the parent ----

size_t kvm_stream_stdin_consume(const uint8_t *b, size_t blocklen, int *deliver)
{
	int ok = 0;
	size_t n = kvm_stdin_frame(b, blocklen, &ok);
	if (deliver != NULL) { *deliver = (ok && n == KVM_CTRL_SIZE) ? 1 : 0; }
	return n;	// a wrong size field: the command is as long as it says (as for every command), never a guessed 16; below 4 or above the buffer: the 4 bytes of the header
}

void kvm_veto_track(kvm_veto_tracker *t, const uint8_t *b, size_t len)
{
	while (len > 0)
	{
		size_t n;
		if (t->skip > 0)
		{
			n = len < t->skip ? len : t->skip;
			t->skip -= (uint32_t)n; b += n; len -= n;
			continue;
		}
		n = (t->collect ? (size_t)t->collect : 4u) - t->have;
		if (n > len) { n = len; }
		memcpy(t->buf + t->have, b, n);
		t->have = (uint8_t)(t->have + n); b += n; len -= n;
		if (!t->collect && t->have == 4)
		{
			unsigned cmd = rd16(t->buf), size = rd16(t->buf + 2);
			if (cmd == KVM_CMD_VIDEO_CTRL && size == KVM_CTRL_SIZE) { t->collect = KVM_CTRL_SIZE; }	// keep the 4 bytes, collect the rest
			else if ((cmd == KVM_CMD_COMPRESSION || cmd == KVM_CMD_FRAME_RATE) && size >= 6 && size <= KVM_CTRL_SIZE) { t->collect = (uint8_t)size; }	// the last of these goes to a new child
			else { t->skip = (size < KVM_STDIN_HEADER) ? 0u : (uint32_t)(size - KVM_STDIN_HEADER); t->have = 0; }	// size < 4: the 4 bytes are the command; above the buffer: the whole declared length is thrown away by the child (kvm_stdin_next), so it is skipped here too
		}
		else if (t->collect && t->have == t->collect)
		{
			const unsigned cmd = rd16(t->buf);
			if (cmd == KVM_CMD_COMPRESSION) { memcpy(t->cmd5, t->buf, t->collect); t->len5 = t->collect; t->rateLast = 0; }
			else if (cmd == KVM_CMD_FRAME_RATE) { memcpy(t->cmd13, t->buf, t->collect); t->len13 = t->collect; t->rateLast = 1; }
			else if (t->buf[4] == 1)
			{
				uint8_t src = (uint8_t)(t->buf[6] & KVM_VETO_ALL);	// the mask the parent hands to a new child never holds a bit that is not a defined source
				if (t->buf[5] == KVM_OP_VETO) { t->mask = (uint8_t)(t->mask | src); }
				else if (t->buf[5] == KVM_OP_VETO_END) { t->mask = (uint8_t)(t->mask & ~src); }
			}
			t->collect = 0; t->have = 0;
		}
	}
}

size_t kvm_veto_replay(const kvm_veto_tracker *t, uint8_t *out, size_t cap)
{
	size_t n = 0;
	int pass;
	for (pass = 0; pass < 2; pass++)
	{
		const int five = (pass == 0) == (t->rateLast != 0);	// the one that came first goes first
		const uint8_t *src = five ? t->cmd5 : t->cmd13;
		const size_t len = five ? t->len5 : t->len13;
		if (len > 0 && n + len <= cap) { memcpy(out + n, src, len); n += len; }
	}
	return n;
}
