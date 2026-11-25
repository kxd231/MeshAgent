/*
Tests of linux_kvm_stream.c, the glue of the end-to-end ACK in the Linux capture process (the protocol and the window are tested with kvm_stream.c in common/tests).
The clock is moved by hand (-DKVML_TEST) and the pipe is a buffer, so every scenario is deterministic. Run with run-linux-stream-tests.sh (gcc in a container).
*/

#include "../linux_kvm_stream.h"
#include "../../common/kvm_stream.h"
#include "../linux_tileinfo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// the globals of the capture process that the glue reads (the tiles)
int SCREEN_WIDTH = 0, SCREEN_HEIGHT = 0, TILE_WIDTH = 32, TILE_HEIGHT = 32, TILE_WIDTH_COUNT = 0, TILE_HEIGHT_COUNT = 0, FRAME_RATE_TIMER = 100;
struct tileInfo_t **g_tileInfo = NULL;

extern uint32_t kvml_test_now;
extern const kvm_stream *kvml_test_stream(void);
static uint32_t gWriteBlockMs = 0;	// the write of the test "takes" this long (the clock of the glue moves while it blocks)

static int gFail = 0, gChecks = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); gChecks++; if (_a != _b) { gFail++; printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

// the pipe: every byte that the glue writes
static uint8_t gPipe[1 << 20];
static size_t gPipeLen = 0;
static int gFailWrites = 0;
int kvm_slave_write_raw(const void *buffer, size_t len)
{
	if (gFailWrites) { return -1; }
	kvml_test_now += gWriteBlockMs;
	if (gPipeLen + len > sizeof(gPipe)) { return -1; }
	memcpy(gPipe + gPipeLen, buffer, len);
	gPipeLen += len;
	return 0;
}

static size_t gParsed = 0;	// how much of the pipe the test has looked at
static void pipe_reset(void) { gPipeLen = 0; gParsed = 0; }

// the next message in the pipe: a 242 (16 bytes) or a marker (JUMBO of 28 bytes) or other bytes (cmd 3 stands for tiles: 4 byte header and size)
typedef struct msg { int cmd; size_t len; const uint8_t *b; } msg;
static int next_msg(msg *m)
{
	const uint8_t *p;
	if (gParsed + 4 > gPipeLen) { return 0; }
	p = gPipe + gParsed;
	m->cmd = (p[0] << 8) | p[1];
	if (m->cmd == 27) { m->len = 8 + (((size_t)p[4] << 24) | ((size_t)p[5] << 16) | ((size_t)p[6] << 8) | p[7]); }
	else { m->len = ((size_t)p[2] << 8) | p[3]; }
	m->b = p;
	if (gParsed + m->len > gPipeLen) { return 0; }
	gParsed += m->len;
	return 1;
}

static void send_ctrl(int op, int flags)
{
	uint8_t b[16];
	kvm_ctrl c;
	memset(&c, 0, sizeof(c));
	c.ver = 1; c.op = (uint8_t)op; c.flags = (uint8_t)flags;
	kvm_stream_build_ctrl(b, &c);
	kvml_input((const char *)b, 240);
}
static void send_ack(uint32_t seq, uint32_t ts)
{
	uint8_t b[16];
	kvm_ack a;
	memset(&a, 0, sizeof(a));
	a.seq = seq; a.ts = ts;
	kvm_stream_build_ack(b, &a);
	kvml_input((const char *)b, 241);
}
// writes of the capture loop: n bytes of "tiles", as cmd 3 messages of at most 60000 bytes each
static void write_tiles(size_t n)
{
	while (n > 0)
	{
		size_t k = n > 60000 ? 60000 : n;
		uint8_t *b = (uint8_t *)calloc(1, k);
		b[0] = 0; b[1] = 3; b[2] = (uint8_t)(k >> 8); b[3] = (uint8_t)k;
		CHECK_EQ(kvml_write(b, k, KVM_W_DATA), 0);
		free(b);
		n -= k;
	}
}
static int expect_state(int video, int ack, int reason, int caps)
{
	msg m;
	kvm_state_msg s;
	if (!next_msg(&m) || m.cmd != 242 || m.len != 16) { CHECK(0 && "a 242 was expected"); return 0; }
	CHECK_EQ(kvm_stream_parse_state(m.b, 16, &s), KVM_REJ_NONE);
	CHECK_EQ(s.video, video); CHECK_EQ(s.ack, ack); CHECK_EQ(s.reason, reason); CHECK_EQ(s.caps, caps);
	return 1;
}
// the marker that the iteration ended with: returns its seq and ts
static int expect_marker(uint32_t *seq, uint32_t *ts)
{
	msg m;
	kvm_frame f;
	if (!next_msg(&m) || m.cmd != 27 || m.len != KVM_MARKER_BYTES) { CHECK(0 && "a marker was expected"); return 0; }
	CHECK_EQ(kvm_stream_parse_frame(m.b, m.len, 0, 0, &f), KVM_REJ_NONE);
	CHECK(f.flags & KVM_FRAME_MARKER);
	*seq = f.seq; *ts = f.ts;
	return 1;
}
static int pipe_empty(void) { msg m; return !next_msg(&m) && gParsed == gPipeLen; }

static void start(void)
{
	pipe_reset();
	kvml_test_now = 1000;
	kvml_session_begin();
}

static void test_announce_and_requests(void)
{
	start();
	kvml_announce();
	CHECK(expect_state(0, 0, KVM_REASON_STARTED, KVM_CAPS_ACK));	// a new process: no video, no ACK, the viewer has to ask
	CHECK(pipe_empty());
	// ACK only
	send_ctrl(KVM_OP_ACK_ONLY, 0);
	kvml_poll();
	CHECK(expect_state(0, 1, KVM_REASON_OK, KVM_CAPS_ACK));
	// video: no video in this build, the ACK stays on, the picture stays JPEG (H-1a behaviour: reason 1, caps = 2)
	send_ctrl(KVM_OP_VIDEO_ON, KVM_PROFILE_HIGH | KVM_PROFILE_MAIN);
	kvml_poll();
	CHECK(expect_state(0, 1, KVM_REASON_NO_VIDEO_BUILD, KVM_CAPS_ACK));
	// repair is never answered and ignored without video
	send_ctrl(KVM_OP_REPAIR, KVM_REPAIR_IDR);
	kvml_poll();
	CHECK(pipe_empty());
	// video off, then the ACK window stays
	send_ctrl(KVM_OP_VIDEO_OFF, 0);
	kvml_poll();
	CHECK(expect_state(0, 1, KVM_REASON_OK, KVM_CAPS_ACK));
	kvml_session_end();
}

static void test_window_and_markers(void)
{
	uint32_t seq, ts, seq2, ts2;
	start();
	kvml_announce(); pipe_reset();
	// ACK is not on: nothing is limited and the loop writes no marker
	write_tiles(500000);
	kvml_iteration_end();
	{ msg m; int n = 0; while (next_msg(&m)) { CHECK_EQ(m.cmd, 3); n++; } CHECK_EQ(n, 9); }	// the tiles only
	pipe_reset();
	CHECK(kvml_gate_big() == 1);
	// ACK on
	send_ctrl(KVM_OP_ACK_ONLY, 0);
	kvml_poll();
	CHECK(expect_state(0, 1, 0, KVM_CAPS_ACK));
	CHECK(kvml_gate_big() == 1);
	// what was written before the activation is not waited for; a picture of 100 KB is written, the iteration ends with a marker (rule M1)
	kvml_test_now = 1100;
	write_tiles(60000); write_tiles(40000);
	CHECK(kvml_gate_big() == 1);	// 100 KB in flight, the window starts at 256 KB
	kvml_iteration_end();
	{ msg m; CHECK(next_msg(&m)); CHECK(next_msg(&m)); }
	CHECK(expect_marker(&seq, &ts));
	CHECK(pipe_empty());
	CHECK_EQ(ts, 1100);
	kvml_iteration_end();	// nothing was written since: no second marker
	CHECK(pipe_empty());
	// more than the window in flight: the gate is closed
	kvml_test_now = 1150;
	write_tiles(60000); write_tiles(60000); write_tiles(60000);
	{ uint32_t denied = kvml_test_stream()->st.gateDenied; CHECK(kvml_gate_big() == 0); CHECK_EQ(kvml_test_stream()->st.gateDenied, denied + 1); }	// a refusal is counted: the link is the limit
	CHECK(kvml_test_stream()->linkHeldEver);
	kvml_iteration_end();
	{ msg m; CHECK(next_msg(&m)); CHECK(next_msg(&m)); CHECK(next_msg(&m)); }
	CHECK(expect_marker(&seq2, &ts2));
	CHECK(seq2 != seq);
	// the ACK of the first position opens it again: 100 + 180 KB written, 100 acknowledged
	kvml_test_now = 1160;
	send_ack(seq, ts);
	kvml_poll();
	CHECK(kvml_gate_big() == 1);
	// a forged ACK (a position that was never written) changes nothing; the ACK of the second one drains the pipe
	send_ack(seq2 + 5, 77);
	send_ack(seq2, ts2);
	kvml_poll();
	CHECK(pipe_empty());
	CHECK(kvml_gate_big() == 1);
	CHECK_EQ(kvm_stream_inflight(kvml_test_stream()), 0);	// both ACKs of the queue were handled, the forged one in between was ignored (counted)
	CHECK_EQ(kvml_test_stream()->st.acksUnknown, 1);
	// no ACK for 2 s with data in flight: the probe marker goes (rule M2) although the loop wrote nothing
	write_tiles(10000);
	kvml_iteration_end();
	{ msg m; CHECK(next_msg(&m)); }
	CHECK(expect_marker(&seq, &ts));
	kvml_test_now = 1160 + 3000;
	kvml_poll();
	kvml_iteration_end();
	CHECK(pipe_empty() == 0);
	kvml_session_end();
}

static void test_timeout(void)
{
	start();
	kvml_announce(); send_ctrl(KVM_OP_ACK_ONLY, 0); kvml_poll(); pipe_reset();
	write_tiles(30000);
	kvml_iteration_end();
	pipe_reset();
	// the viewer is gone: 15 s without an ACK with data in flight switch ACK off with 242 reason 9 (the probe markers on the way do not count as progress)
	{
		uint32_t t;
		for (t = 1000; t < 1000 + 16000; t += 500) { kvml_test_now = t; kvml_poll(); kvml_iteration_end(); }
	}
	{
		msg m; kvm_state_msg s; int got = 0;
		while (next_msg(&m)) { if (m.cmd == 242) { CHECK_EQ(kvm_stream_parse_state(m.b, 16, &s), KVM_REJ_NONE); if (s.reason == KVM_REASON_NO_ACK) { got = 1; CHECK_EQ(s.ack, 0); } } }
		CHECK(got);
	}
	CHECK(kvml_gate_big() == 1);	// the writes are no longer limited
	kvml_session_end();
}

static void test_veto_and_bad_input(void)
{
	uint8_t b[16];
	start();
	kvml_announce(); pipe_reset();
	send_ctrl(KVM_OP_ACK_ONLY, 0);
	kvml_poll();
	CHECK(expect_state(0, 1, 0, KVM_CAPS_ACK));
	// a second viewer: op 5 from meshcore takes the ACK away (reason 5) and nothing is limited; op 6 gives it back (reason 10, the viewer asks again)
	send_ctrl(KVM_OP_VETO, KVM_VETO_MESHCORE);
	kvml_poll();
	CHECK(expect_state(0, 0, KVM_REASON_VETO, KVM_CAPS_ACK));
	write_tiles(600000);
	CHECK(kvml_gate_big() == 1);
	pipe_reset();
	send_ctrl(KVM_OP_VETO_END, KVM_VETO_MESHCORE);
	kvml_poll();
	CHECK(expect_state(0, 0, KVM_REASON_AVAILABLE, KVM_CAPS_ACK));
	// a command with a wrong version or op is dropped and counted, never answered
	pipe_reset();
	memset(b, 0, sizeof(b)); b[1] = 240; b[3] = 16; b[4] = 9; b[5] = 1; b[6] = 4;	// version 9
	kvml_input((const char *)b, 240);
	b[4] = 1; b[5] = 77;															// op 77
	kvml_input((const char *)b, 240);
	memset(b, 0, sizeof(b)); b[1] = 241; b[3] = 16;									// an ACK with seq 0
	kvml_input((const char *)b, 241);
	kvml_poll();
	CHECK(pipe_empty());
	kvml_session_end();
}

static void test_switched_off(void)
{
	setenv("MESH_KVM_ACK", "0", 1);
	start();
	kvml_announce();
	CHECK(expect_state(0, 0, KVM_REASON_STARTED, 0));	// caps 0
	send_ctrl(KVM_OP_ACK_ONLY, 0);
	kvml_poll();
	CHECK(expect_state(0, 0, KVM_REASON_DISABLED, 0));
	send_ctrl(KVM_OP_VIDEO_ON, KVM_PROFILE_HIGH);
	kvml_poll();
	CHECK(expect_state(0, 0, KVM_REASON_DISABLED, 0));
	write_tiles(900000);
	CHECK(kvml_gate_big() == 1);
	kvml_iteration_end();
	{ msg m; int n = 0; while (next_msg(&m)) { CHECK_EQ(m.cmd, 3); n++; } CHECK_EQ(n, 15); }	// the tiles only: no marker without ACK
	kvml_session_end();
	unsetenv("MESH_KVM_ACK");
}

static void test_write_errors_and_before_start(void)
{
	// before the session begins (and in the master) a write is the raw write and counts nothing
	pipe_reset();
	{
		uint8_t b[5] = { 0, 88, 0, 5, 1 };
		CHECK_EQ(kvml_write(b, sizeof(b), KVM_W_DATA), 0);
		CHECK_EQ(gPipeLen, 5);
	}
	start();
	gFailWrites = 1;
	{
		uint8_t b[5] = { 0, 88, 0, 5, 1 };
		CHECK_EQ(kvml_write(b, sizeof(b), KVM_W_DATA), -1);	// the error reaches the caller (the loop ends the session)
	}
	gFailWrites = 0;
	kvml_session_end();
}

static void test_round_trip_is_measured_from_the_arrival(void)
{
	uint32_t seq, ts;
	start();
	kvml_announce(); send_ctrl(KVM_OP_ACK_ONLY, 0); kvml_poll(); pipe_reset();
	// the first position: its ACK empties the pipe, from then on a round trip is measured
	kvml_test_now = 2000; write_tiles(5000); kvml_iteration_end();
	{ msg m; CHECK(next_msg(&m)); } CHECK(expect_marker(&seq, &ts));
	kvml_test_now = 2010; send_ack(seq, ts); kvml_poll();
	CHECK_EQ(kvm_stream_inflight(kvml_test_stream()), 0);
	// the second one is written at 3000, the ACK is read from the pipe at 3040 but the loop handles it at 3500: the round trip is 40 ms, not 500
	kvml_test_now = 3000; write_tiles(5000); kvml_iteration_end();
	{ msg m; CHECK(next_msg(&m)); } CHECK(expect_marker(&seq, &ts));
	kvml_test_now = 3040; send_ack(seq, ts);
	kvml_test_now = 3500; kvml_poll();
	CHECK_EQ(kvml_test_stream()->minRtt, 40);
	kvml_session_end();
}

static void test_a_blocked_write_stops_the_clocks(void)
{
	uint32_t seq, ts;
	start();
	kvml_announce(); send_ctrl(KVM_OP_ACK_ONLY, 0); kvml_poll(); pipe_reset();
	kvml_test_now = 1000; write_tiles(5000); kvml_iteration_end();
	{ msg m; CHECK(next_msg(&m)); } CHECK(expect_marker(&seq, &ts));
	// the pipe is full (the link does not take the data): a write blocks for 20 s; the viewer cannot acknowledge what it has not got, so this is not a missing ACK (no 15 s timeout)
	gWriteBlockMs = 20000;
	write_tiles(5000);
	gWriteBlockMs = 0;
	kvml_poll();
	{
		msg m; int timeout = 0;
		while (next_msg(&m)) { if (m.cmd == 242) { kvm_state_msg s; CHECK_EQ(kvm_stream_parse_state(m.b, 16, &s), KVM_REJ_NONE); if (s.reason == KVM_REASON_NO_ACK) { timeout = 1; } } }
		CHECK(!timeout);
	}
	CHECK(kvm_stream_ack_active(kvml_test_stream()));
	kvml_session_end();
}

static void test_scan_tiles(void)
{
	start();
	CHECK_EQ(kvml_scan_start(400), 0);
	kvml_scan_deferred(171);				// the tile index (row * tiles per row + column): resumes in the middle of a row
	CHECK_EQ(kvml_scan_start(400), 171);
	CHECK_EQ(kvml_scan_start(400), 0);		// once: a scan that goes to the end starts at the first tile again
	kvml_scan_deferred(455);
	CHECK_EQ(kvml_scan_start(400), 0);		// a tile beyond the screen (the size changed) is not used
	kvml_session_end();
}

// The scan is deferred by the window while the coalescing has already marked tiles of the row below (new CRC, TILE_MARKED_NOT_SENT) to be sent later in the frame. The next frame sets
// every flag to TODO and compares the CRC of the picture with the stored one: a tile that kept its new CRC would look unchanged and its change would be lost for good.
static void test_deferred_scan_keeps_the_marked_tiles(void)
{
	struct tileInfo_t rows[3][3], *p[3];
	int r, c;
	const int crcA = 1111, crcB = 2222, crcC = 3333;
	start();
	TILE_HEIGHT_COUNT = 3; TILE_WIDTH_COUNT = 3;
	for (r = 0; r < 3; r++) { p[r] = rows[r]; for (c = 0; c < 3; c++) { rows[r][c].crc = 100 + r * 3 + c; rows[r][c].flag = TILE_TODO; } }
	g_tileInfo = p;
	rows[0][0].crc = crcA; rows[0][0].flag = TILE_SENT;				// the seed, sent
	rows[1][0].crc = crcB; rows[1][0].flag = TILE_MARKED_NOT_SENT;	// the row below: changed, did not join, to be sent later
	rows[2][2].crc = crcC; rows[2][2].flag = TILE_MARKED_NOT_SENT;	// another one that the coalescing marked
	rows[0][1].crc = 77; rows[0][1].flag = TILE_TODO;				// the tile that the window refused: not looked at, its CRC is the old one
	kvml_scan_deferred(1);
	CHECK_EQ(rows[1][0].crc, 0xFF);									// the next frame finds it different from the picture (crcB) and sends it
	CHECK_EQ(rows[2][2].crc, 0xFF);
	CHECK_EQ(rows[0][0].crc, crcA);									// what was sent keeps its CRC
	CHECK_EQ(rows[0][1].crc, 77);									// what was not looked at is left alone
	CHECK_EQ(rows[1][1].crc, 104);
	CHECK(rows[1][0].crc != crcB);									// (the old code: equal, and the change was lost)
	g_tileInfo = NULL;
	kvml_scan_deferred(1);											// no tile table: nothing to do
	kvml_session_end();
}

int main(void)
{
	test_announce_and_requests();
	test_window_and_markers();
	test_timeout();
	test_veto_and_bad_input();
	test_switched_off();
	test_write_errors_and_before_start();
	test_round_trip_is_measured_from_the_arrival();
	test_a_blocked_write_stops_the_clocks();
	test_scan_tiles();
	test_deferred_scan_keeps_the_marked_tiles();
	printf("%d checks, %d failed\n", gChecks, gFail);
	return gFail ? 1 : 0;
}
