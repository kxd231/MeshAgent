/*
KVM (Linux): the end-to-end ACK of the commands 240-243 in the capture process. See linux_kvm_stream.h.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#include "linux_kvm_stream.h"

int g_tileDeferred = 0;

#ifdef KVM_STREAM_ENABLED

#include "meshcore/KVM/common/kvm_stream.h"
#include "meshcore/meshdefines.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define KVML_ACK_QUEUE 128
#define KVML_BLOCKED_WRITE_MS 100	// a write that takes longer than this counts as blocked: the timers of the window did not run meanwhile
#define KVML_STATS_MS 5000

typedef struct kvml_item
{
	uint8_t b[KVM_CTRL_SIZE];
	uint32_t at;	// when the command was read from the pipe: the round trip is measured from this, not from the moment the loop gets to it
} kvml_item;

static kvm_stream gStream;
static kvm_inbox gInbox;
static kvml_item gAckQ[KVML_ACK_QUEUE];
static int gAckHead = 0, gAckCount = 0;
static int gOn = 0;
static int gLog = 0;
static uint32_t gInputBad = 0, gAckDropped = 0, gStatsAt = 0;
static int gResumeRow = 0;

#ifdef KVML_TEST
uint32_t kvml_test_now = 0;	// the tests (tests/linux_kvm_stream_test.c) move the clock by hand
static uint32_t kvml_now(void) { return kvml_test_now; }
#else
static uint32_t kvml_now(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) { return 0; }
	return (uint32_t)(((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL));
}
#endif

static int kvml_env_off(const char *name)
{
	const char *v = getenv(name);
	return (v != NULL && v[0] == '0') ? 1 : 0;
}

int kvml_write(const void *buffer, size_t len, int kind)
{
	int r;
	uint32_t t0, t1;
	if (!gOn) { return kvm_slave_write_raw(buffer, len); }
	t0 = kvml_now();
	kvm_stream_note_write(&gStream, len, kind, t0);
	r = kvm_slave_write_raw(buffer, len);
	t1 = kvml_now();
	if ((uint32_t)(t1 - t0) > KVML_BLOCKED_WRITE_MS) { kvm_stream_note_blocked(&gStream, t1); }
	return r;
}

static void kvml_send_state(const kvm_state_msg *m)
{
	uint8_t b[KVM_CTRL_SIZE];
	kvm_stream_build_state(b, m);
	(void)kvml_write(b, KVM_CTRL_SIZE, KVM_W_STATE);
	if (gLog) { fprintf(stderr, "KVM stream: state video=%d ack=%d reason=%d caps=%d gen=%d veto=%d\n", (int)m->video, (int)m->ack, (int)m->reason, (int)m->caps, (int)m->gen, (int)gStream.vetoMask); }
}

void kvml_session_begin(void)
{
	int capAck = 1;
	const char *l = getenv("MESH_KVM_STREAM_LOG");
	gLog = (l != NULL && l[0] != 0 && l[0] != '0') ? 1 : 0;
	if (kvml_env_off("MESH_KVM_ACK")) { capAck = 0; }
	// no video code in this build: an op 1 is answered with reason 1 and the ACK window is on; the picture stays JPEG
	kvm_stream_init(&gStream, 0, capAck, KVM_REASON_NO_VIDEO_BUILD, KVM_REASON_DISABLED);
	kvm_inbox_init(&gInbox, kvml_now());
	gAckHead = gAckCount = 0;
	gInputBad = gAckDropped = 0;
	gResumeRow = 0;
	g_tileDeferred = 0;
	gStatsAt = kvml_now();
	gOn = 1;
	if (gLog) { fprintf(stderr, "KVM stream: start cap_ack=%d\n", capAck); }
}

static void kvml_stats(int final)
{
	uint32_t now = kvml_now();
	const kvm_stream_stats *st = &gStream.st;
	if (!gLog) { return; }
	fprintf(stderr, "KVM stream: stats%s ack=%d want=%d veto=%d win_kb=%u inflight_kb=%llu rate_kbps=%llu min_rtt_ms=%u acks_ok=%u acks_off=%u acks_flood=%u acks_unknown=%u acks_stale=%u markers=%u probes=%u stalls=%u timeouts=%u gate_denied=%u ctrl_ok=%u input_bad=%u ctl_dropped=%u ack_dropped=%u\n",
		final ? " final=1" : "", kvm_stream_ack_active(&gStream), (int)gStream.want, (int)gStream.vetoMask, (unsigned)(kvm_stream_window(&gStream, now) / 1024u),
		(unsigned long long)(kvm_stream_inflight(&gStream) / 1024ull), (unsigned long long)(kvm_stream_rate(&gStream, now) * 8ull / 1000ull), (unsigned)gStream.minRtt,
		st->acksOk, st->acksOff, st->acksFlood, st->acksUnknown, st->acksStale, st->markers, st->probes, st->stalls, st->timeouts, st->gateDenied, st->ctrlOk,
		gInputBad, (unsigned)gInbox.nLimited, gAckDropped);
}

#ifdef KVML_TEST
const kvm_stream *kvml_test_stream(void) { return &gStream; }	// the tests look at the state of the window
#endif

void kvml_session_end(void)
{
	if (!gOn) { return; }
	kvml_stats(1);
	gOn = 0;
}

void kvml_announce(void)
{
	kvm_state_msg m;
	if (!gOn) { return; }
	kvm_stream_start_state(&gStream, &m);
	kvml_send_state(&m);
}

void kvml_input_bad(void) { gInputBad++; }

void kvml_input(const char *block, int type)
{
	kvml_item it;
	if (!gOn) { return; }
	memcpy(it.b, block, KVM_CTRL_SIZE);
	it.at = kvml_now();
	if (type == MNG_KVM_VIDEO_CTRL)
	{
		kvm_ctrl c;
		if (kvm_stream_parse_ctrl(it.b, KVM_CTRL_SIZE, &c) != KVM_REJ_NONE) { kvml_input_bad(); return; }
		(void)kvm_inbox_put(&gInbox, &c, it.at);	// never refused for lack of room; over the rate only the ordinary commands are dropped (counted in the inbox)
	}
	else
	{
		if (kvm_stream_parse_ack(it.b, KVM_CTRL_SIZE, NULL) != KVM_REJ_NONE) { kvml_input_bad(); return; }
		if (gAckCount == KVML_ACK_QUEUE) { gAckHead = (gAckHead + 1) % KVML_ACK_QUEUE; gAckCount--; gAckDropped++; }	// an ACK covers the older ones: keep the newest
		gAckQ[(gAckHead + gAckCount++) % KVML_ACK_QUEUE] = it;
	}
}

void kvml_poll(void)
{
	kvm_inbox_op ctl[KVM_INBOX_OPS];
	kvm_inbox batch;
	size_t nctl, i;
	kvm_state_msg reply;
	uint32_t now;
	if (!gOn) { return; }
	kvm_inbox_take(&gInbox, &batch);
	// the veto first (the stream gets the veto of the inbox before any request for video is looked at), then the last request of the viewer, then the repair
	nctl = kvm_inbox_expand(&batch, gStream.vetoMask, ctl);
	for (i = 0; i < nctl; i++)
	{
		kvm_ctrl c = ctl[i].c;
		unsigned actions = 0;
		if (gLog) { fprintf(stderr, "KVM stream: ctrl op=%d flags=%d\n", (int)c.op, (int)c.flags); }
		if (kvm_stream_on_ctrl(&gStream, &c, ctl[i].at, &reply, &actions)) { kvml_send_state(&reply); }
	}
	while (gAckCount > 0)
	{
		kvm_ack a;
		const kvml_item *it = &gAckQ[gAckHead];
		if (kvm_stream_parse_ack(it->b, KVM_CTRL_SIZE, &a) == KVM_REJ_NONE) { (void)kvm_stream_on_ack(&gStream, &a, it->at); }
		gAckHead = (gAckHead + 1) % KVML_ACK_QUEUE;
		gAckCount--;
	}
	now = kvml_now();
	// A blocked write stops the clocks (kvml_write: kvm_stream_note_blocked); the loop itself is never "paused" for them
	if (kvm_stream_tick(&gStream, now, 0, &reply)) { kvml_send_state(&reply); }
	if (gLog && (uint32_t)(now - gStatsAt) >= KVML_STATS_MS) { gStatsAt = now; kvml_stats(0); }
}

void kvml_iteration_end(void)
{
	uint8_t marker[KVM_MARKER_BYTES];
	int probe = 0;
	uint32_t now;
	if (!gOn) { return; }
	now = kvml_now();
	if (kvm_stream_marker_wanted(&gStream, now, 0, &probe))
	{
		kvm_stream_make_marker(&gStream, now, probe, marker);
		(void)kvml_write(marker, KVM_MARKER_BYTES, KVM_W_POSITION);
	}
}

int kvml_gate_big(void)
{
	uint32_t now;
	int ok;
	if (!gOn) { return 1; }
	now = kvml_now();
	ok = kvm_stream_window_allows(&gStream, KVM_GATE_BIG, now);
	if (!ok) { (void)kvm_stream_note_link_held(&gStream, now); }	// a refused rectangle says that the link is the limit
	return ok;
}

void kvml_scan_deferred(int row) { gResumeRow = row; }

int kvml_scan_start(int rows)
{
	int r = gResumeRow;
	gResumeRow = 0;
	return (r > 0 && r < rows) ? r : 0;
}

#endif /* KVM_STREAM_ENABLED */
