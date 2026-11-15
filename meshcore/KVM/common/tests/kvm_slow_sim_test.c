/*
the rate filter: a simulation of a SLOW link (128 kb/s, 256 kb/s, 1 Mb/s) with the real pieces of the agent that decide the rate: the stream layer, the controller and the plan of the
video, the rate control and the pace. Derived from an earlier simulation and from kvm_start_sim_test.c: a plain link of a fixed rate that may change at
the start of a pause, the first picture through the gate of a big rectangle in pieces of 16 KB, light or heavy content. Measured from an epoch (the start of the session, or the end of the pause): the first
refusal of the window and the queue at that moment, the first rate sample, the queue, R.

The point: a link that is clogged without the window having refused anything (the first picture is smaller than W_START, the memory of the rate is gone after a pause of 8 minutes, the link fell
in a pause and the content is light) has to make a rate sample from the slow span in which the pipe never emptied; the filter of the trickle (KVM_STREAM_TRICKLE_RATE) may drop only a span in which the
pipe was empty. dropped them all: the window stayed 256 KB (16 s of queue at 128 kb/s) until the first refusal.

Built by run-kvm-slow-sim-tests.sh against the files of and the tree; the checks (-DSIM_CHECK) are for the tree.
*/
#include "../kvm_video_rc.h"
#include "../kvm_video_core.h"
#include "../kvm_pace.h"
#include "../kvm_stream.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef SIM_SOFT	/* the tree: a refusal of the window that is no evidence of the link does not wipe the proof of the key frame; the old files have neither */
#define SIM_NOTE_LINK_HELD(s, t) kvm_stream_note_link_held((s), (t))
#define SIM_REFUSED_SOFT(rc) kvm_vrc_window_refused_soft(rc)
#else
#define SIM_NOTE_LINK_HELD(s, t) (kvm_stream_note_link_held((s), (t)), 1)
#define SIM_REFUSED_SOFT(rc) kvm_vrc_window_refused(rc)
#endif

typedef struct pos_t { uint32_t seq, ts; uint64_t cum; int ready; uint32_t readyAt; } pos_t;
typedef struct cfg
{
	const char *label;
	double linkBps, linkBps2;		/* bytes/s; linkBps2 from the start of the pause */
	uint32_t oneWayMs;
	uint32_t picBytes;				/* first picture, through the BIG gate in 16 KB pieces */
	double pTileBytes;				/* P frame tile bytes at QP 26 */
	double pTileBytes2;				/* from the end of the pause (0: the same): D7 - D10, light work, a pause, then heavy content */
	uint32_t videoAtMs;				/* first video frame (IDR) after the session start */
	uint32_t runMs;					/* video before the pause (0: no pause, epoch = session start) */
	int jpegOnly;					/* no video: the content is JPEG rectangles of 16 KB through the gate of a big rectangle (the mode of a viewer without H.264, a build without Media Foundation): light (20 KB/s in rectangles of 2 KB) before the pause, as much as the window lets through (16 KB rectangles) after it */
	uint32_t pauseMs;				/* content off this long */
	uint32_t afterMs;				/* measured after the epoch */
	int verbose;
} cfg;

#ifdef SIM_CHECK
static int gFail = 0, gChecks = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#endif

typedef struct res { double firstRefusal, qAtRefusal, firstSample, qMax, q5, r[4]; unsigned frames; } res;

static pos_t pos[400000];

static res gRes;
static res run(const cfg *c)
{
	kvm_stream s; kvm_vctl ctl; kvm_vrc rc;
	size_t npos = 0, firstUnready = 0, firstUndelivered = 0;
	double served = 0.0, srcAcc = 0.0, linkBps = c->linkBps;
	const uint32_t t0 = 5000;
	const uint32_t epochRel = c->runMs ? c->videoAtMs + c->runMs + c->pauseMs : 0u;
	const uint32_t tEnd = t0 + epochRel + c->afterMs + (c->runMs ? 0u : c->videoAtMs);
	uint32_t t, lastAck = 0, nextIter = 0, lastFrameT = 0, picLeft = c->picBytes, nextLog = 0;
	int pendFlag = 0, haveIdr = 0, vidOn = 0, epochSeen = 0;
	double jAcc = 0.0;
	double firstRefusal = -1, firstSample = -1, qAtRefusal = -1, qMax = 0, q5 = -1, rAt[4] = { -1, -1, -1, -1 };
	uint64_t memAtEpoch = 0, sampleAtFirst = 0; uint32_t wAtEpoch = 0, wMax = 0;
	uint64_t bytesAtEpoch = 0; unsigned framesAfter = 0, heldAfter = 0;
	double encAvgUs = 8000.0;
	uint32_t idrBytes = 0;
	{
		kvm_ctrl op; kvm_state_msg reply;
		kvm_stream_init(&s, 1, 1, 1, 6);
		memset(&op, 0, sizeof(op)); op.ver = 1; op.op = KVM_OP_VIDEO_ON; op.flags = 7;
		(void)kvm_stream_on_ctrl(&s, &op, t0, &reply, NULL);
		kvm_vctl_init(&ctl, 1);
	}
	kvm_vrc_init(&rc, KVM_VIDEO_QP, t0);
	kvm_vrc_set_picture(&rc, 1920u * 1088u);
	nextIter = t0 + 100u;
	for (t = t0; t < tEnd; t++)
	{
		const uint32_t rel = t - t0;
		const int inPause = c->runMs && rel >= c->videoAtMs + c->runMs && rel < epochRel;
		const int afterEpoch = rel >= epochRel;
		if (c->runMs && rel == c->videoAtMs + c->runMs) { linkBps = c->linkBps2; }
		served += linkBps / 1000.0;
		if (served > (double)s.bytesWritten) { served = (double)s.bytesWritten; }
		while (firstUnready < npos && (double)pos[firstUnready].cum <= served + 0.5) { pos[firstUnready].ready = 1; pos[firstUnready].readyAt = t + c->oneWayMs; firstUnready++; }
		if (t - lastAck >= 3)
		{
			size_t newest = (size_t)-1, i;
			for (i = firstUndelivered; i < firstUnready; i++) { if (pos[i].ready && (int32_t)(t - pos[i].readyAt) >= 0) { newest = i; } }
			if (newest != (size_t)-1)
			{
				kvm_ack a; a.seq = pos[newest].seq; a.ts = pos[newest].ts; a.queueMs = 0; a.flags = 0;
				(void)kvm_stream_on_ack(&s, &a, t);
				firstUndelivered = newest + 1; lastAck = t;
			}
		}
		if (afterEpoch && !epochSeen)
		{
			epochSeen = 1; memAtEpoch = kvm_stream_rate_or_mem(&s, t); wAtEpoch = kvm_stream_window(&s, t); bytesAtEpoch = s.bytesWritten;
		}
		if (afterEpoch && firstSample < 0 && kvm_stream_rate(&s, t) > 0u && (c->runMs == 0 || (int32_t)(t - (t0 + epochRel)) >= 0))
		{
			/* after a pause of more than 2 s the samples of before are gone: the first one that exists after the epoch is new */
			firstSample = (double)(rel - epochRel); sampleAtFirst = kvm_stream_rate(&s, t);
		}
		if (afterEpoch) { uint32_t w = kvm_stream_window(&s, t); if (w > wMax) { wMax = w; } }
		/* the first picture of the session, 16 KB pieces through the gate of a big rectangle */
		if (picLeft > 0 && rel >= 100u)
		{
			uint32_t piece = 0;
			if (kvm_stream_window_allows(&s, KVM_GATE_BIG, t)) { piece = picLeft > 16384u ? 16384u : picLeft; }
			else { kvm_stream_note_link_held(&s, t); if (afterEpoch && firstRefusal < 0) { firstRefusal = (double)(rel - epochRel); qAtRefusal = ((double)s.bytesWritten - served) * 1000.0 / linkBps; } }
			if (piece > 0)
			{
				uint8_t mk[KVM_MARKER_BYTES]; int probe = 0;
				kvm_stream_note_write(&s, piece, KVM_W_DATA, t);
				picLeft -= piece;
				if (kvm_stream_marker_wanted(&s, t, 0, &probe)) { kvm_stream_make_marker(&s, t, probe, mk); kvm_stream_note_write(&s, KVM_MARKER_BYTES, KVM_W_POSITION, t); pos[npos].seq = s.seqCounter; pos[npos].ts = t; pos[npos].cum = s.bytesWritten; npos++; }
			}
		}
		if (c->jpegOnly && rel >= c->videoAtMs && picLeft == 0)
		{
			const int light = !afterEpoch;
			jAcc += inPause ? 0.0 : (light ? 20.0 : 16384.0);	/* bytes a millisecond: 20 KB/s of light work, then as much as the gate lets through */
			if (jAcc > 65536.0) { jAcc = 65536.0; }
			while (jAcc >= (light ? 2048.0 : 16384.0))
			{
				if (kvm_stream_window_allows(&s, KVM_GATE_BIG, t))
				{
					uint8_t mk[KVM_MARKER_BYTES]; int probe = 0;
					kvm_stream_note_write(&s, light ? 2048u : 16384u, KVM_W_DATA, t); jAcc -= light ? 2048.0 : 16384.0;
					if (kvm_stream_marker_wanted(&s, t, 0, &probe)) { kvm_stream_make_marker(&s, t, probe, mk); kvm_stream_note_write(&s, KVM_MARKER_BYTES, KVM_W_POSITION, t); pos[npos].seq = s.seqCounter; pos[npos].ts = t; pos[npos].cum = s.bytesWritten; npos++; }
					if (!light) { framesAfter++; }
				}
				else
				{
					(void)SIM_NOTE_LINK_HELD(&s, t);
					if (afterEpoch && firstRefusal < 0) { firstRefusal = (double)(rel - epochRel); qAtRefusal = ((double)s.bytesWritten - served) * 1000.0 / linkBps; }
					break;
				}
			}
		}
		srcAcc += 1000.0;
		if (srcAcc >= 33333.0) { srcAcc -= 33333.0; if (rel + 100u >= c->videoAtMs && !inPause) { pendFlag = 1; } }
		if ((int32_t)(t - nextIter) < 0) { continue; }
		{
			const uint32_t interval = 16u;
			uint32_t next = t + 16u;
			(void)kvm_stream_tick(&s, t, 0, NULL);
			if (!c->jpegOnly && rel + 100u >= c->videoAtMs && !vidOn) { kvm_state_msg mm; (void)kvm_vctl_begin_gen(&ctl, &s, 100, 42, 1920, 1088, 1, &mm); vidOn = 1; }
			if (vidOn)
			{
				kvm_vrc_update(&rc, t, 0, kvm_stream_rate_or_mem(&s, t), kvm_stream_link_limited(&s, t));
				(void)kvm_vrc_tick(&rc, t);
				{
					uint32_t ackAt = 0, ts = 0, queueMs = 0, minRtt = 0;
					if (kvm_stream_watch_result(&s, &ackAt, &ts, &queueMs, &minRtt)) { kvm_vrc_idr_acked(&rc, t, idrBytes, ts, ackAt, queueMs, minRtt); }
				}
				if (pendFlag)
				{
					kvm_vplan_in in; kvm_vplan plan;
					in.now = t; in.pending = 872u; in.gateOpen = kvm_stream_window_allows(&s, KVM_GATE_VIDEO, t); in.rateOpen = kvm_vrc_peek(&rc, t);
					in.intervalMs = interval; in.rate = kvm_stream_rate_or_mem(&s, t);
					kvm_vctl_plan(&ctl, &s, &in, &plan);
					if (plan.held)
					{
						if (SIM_NOTE_LINK_HELD(&s, t)) { kvm_vrc_window_refused(&rc); } else { SIM_REFUSED_SOFT(&rc); }
						if (afterEpoch) { heldAfter++; if (firstRefusal < 0) { firstRefusal = (double)(rel - epochRel); qAtRefusal = ((double)s.bytesWritten - served) * 1000.0 / linkBps; } }
					}
					if (plan.heldRate) { (void)kvm_vrc_may_encode(&rc, t, 0); }
					if (plan.encode)
					{
						const int idr = ctl.idrFirst || plan.forceIdr || ctl.idrForce;
						const int qp = kvm_vrc_qp(&rc, idr);
						const double dt = lastFrameT ? (double)(t - lastFrameT) : 100.0;
						double b; uint32_t bytes, seq, tEncEnd = t + (idr ? 50u : 10u);
						(void)kvm_vrc_may_encode(&rc, t, idr);
						if (idr) { b = 228.0 * 1024.0 / pow(0.94, 8.0) * pow(0.94, (double)(qp - 30)); }
						else { b = ((afterEpoch && c->pTileBytes2 > 0.0) ? c->pTileBytes2 : c->pTileBytes) * 872.0 * pow(0.89, (double)(qp - 26)) * (1.0 + 0.25 * (dt > 16.7 ? ((dt > 100.0 ? 100.0 : dt) / 16.7 - 1.0) : 0.0)); }
						bytes = (uint32_t)b; if (bytes < 60u) { bytes = 60u; }
						{ const int hadFrame = ctl.haveFrame; const uint32_t prevSlot = ctl.lastFrame; (void)kvm_vctl_frame_done(&ctl, idr, bytes, t); ctl.lastFrame = kvm_pace_slot(hadFrame, prevSlot, t, interval, NULL); }
						kvm_vctl_note_write(&ctl, tEncEnd, (uint32_t)((encAvgUs + 500.0) / 1000.0));
						encAvgUs = encAvgUs * 0.875 + (idr ? 50000.0 : 10000.0) * 0.125;
						if (idr) { haveIdr = 1; idrBytes = bytes; }
						kvm_vrc_frame(&rc, t, bytes, qp, idr, (idr ? 50000u : 10000u));
						seq = kvm_stream_add_position(&s, t, bytes);
						kvm_stream_note_write(&s, bytes, KVM_W_POSITION, t);
						if (idr) { kvm_stream_watch(&s, seq); }
						pos[npos].seq = seq; pos[npos].ts = t; pos[npos].cum = s.bytesWritten; npos++;
						lastFrameT = t; pendFlag = 0;
						if (afterEpoch) { framesAfter++; }
						next = tEncEnd;
					}
				}
				{
					uint8_t mk[KVM_MARKER_BYTES]; int probe = 0;
					if (kvm_stream_marker_wanted(&s, t, 0, &probe) && npos < 399000) { kvm_stream_make_marker(&s, t, probe, mk); kvm_stream_note_write(&s, KVM_MARKER_BYTES, KVM_W_POSITION, t); pos[npos].seq = s.seqCounter; pos[npos].ts = t; pos[npos].cum = s.bytesWritten; npos++; }
				}
			}
			if (afterEpoch)
			{
				const double q = ((double)s.bytesWritten - served) * 1000.0 / linkBps;
				const uint32_t since = rel - epochRel;
				static const uint32_t at[4] = { 1000, 3000, 8000, 15000 };
				int k;
				if (q > qMax) { qMax = q; }
				if (since >= 5000u && q5 < 0) { q5 = q; }
				for (k = 0; k < 4; k++) { if (since >= at[k] && rAt[k] < 0) { rAt[k] = (double)rc.rate * 8.0 / 1000.0; } }
				if (c->verbose && (int32_t)(t - nextLog) >= 0)
				{
					nextLog = t + 500u;
					printf("   +%6u ms: R=%6.0f kb/s cap=%7llu rate=%7llu mem=%7llu W=%7u lim=%d qp=%d q=%6.0f ms inflight=%7llu frames=%u held=%u\n", (unsigned)since, (double)rc.rate * 8.0 / 1000.0, (unsigned long long)rc.cap,
						(unsigned long long)kvm_stream_rate(&s, t), (unsigned long long)kvm_stream_rate_or_mem(&s, t), (unsigned)kvm_stream_window(&s, t), kvm_stream_link_limited(&s, t), rc.qp, q,
						(unsigned long long)(s.bytesWritten - s.ackedCum), framesAfter, heldAfter);
				}
			}
			nextIter = next > t ? next : t + 1u;
		}
	}
	(void)haveIdr;
	{ res o; o.firstRefusal = firstRefusal; o.qAtRefusal = qAtRefusal; o.firstSample = firstSample; o.qMax = qMax; o.q5 = q5; memcpy(o.r, rAt, sizeof(o.r)); o.frames = framesAfter; gRes = o; }
	printf("| %s | %.0f | %.0f (%.0f) | %.0f / %llu | %llu / %u / %u | %.0f | %.0f | %.0f / %.0f / %.0f / %.0f | %.1f | %u |\n", c->label, firstRefusal, qAtRefusal, qAtRefusal >= 0 ? qAtRefusal * linkBps / 1024000.0 : -1.0,
		firstSample, (unsigned long long)sampleAtFirst, (unsigned long long)memAtEpoch, (unsigned)(wAtEpoch / 1024u), (unsigned)(wMax / 1024u), qMax, q5, rAt[0], rAt[1], rAt[2], rAt[3],
		(double)(s.bytesWritten - bytesAtEpoch) / (double)(c->afterMs) , framesAfter);
	return gRes;
}

static cfg base(const char *label, double bps)
{
	cfg c; memset(&c, 0, sizeof(c));
	c.label = label; c.linkBps = bps; c.linkBps2 = bps; c.oneWayMs = 20; c.picBytes = 574366u; c.pTileBytes = 7.0; c.videoAtMs = 3900u; c.afterMs = 20000u;
	return c;
}

int main(int argc, char **argv)
{
	cfg c;
	static res rJ1, rJ2, rJ3, rD7, rD8, rD9, rD10, rS1, rS2, rS3, rS4, rP1, rP2, rP3, rP4, rL1, rL2, rL3, rL4, rD1, rD2, rD3, rD4, rD5, rD2L, rD6, rL3L;
	const char *only = argc > 1 ? argv[1] : NULL;
#ifdef SIM_BUILD
	printf("build: %s\n", SIM_BUILD);
#endif
	printf("| scene | first refusal ms | queue at it ms (KB) | first sample ms / B/s | rate_or_mem B/s / W KiB at epoch / W max KiB | queue max ms | queue at +5 s ms | R +1/+3/+8/+15 s kb/s | KB/s sent | frames |\n|---|---|---|---|---|---|---|---|---|---|\n");
#define RUN(tag) if (!only || strcmp(only, #tag) == 0) { c.verbose = only != NULL; r##tag = run(&c); }
	/* session start */
	c = base("S1 start 128 kb/s, picture 574 KB, heavy", 16000.0); RUN(S1);
	c = base("S2 start 128 kb/s, picture 150 KB, light 20 KB/s", 16000.0); c.picBytes = 150000u; c.pTileBytes = 0.61; RUN(S2);
	c = base("S3 start 256 kb/s, picture 150 KB, light 40 KB/s", 32000.0); c.picBytes = 150000u; c.pTileBytes = 1.22; RUN(S3);
	c = base("S4 start 1 Mb/s, picture 150 KB, light 150 KB/s", 125000.0); c.picBytes = 150000u; c.pTileBytes = 4.6; RUN(S4);
	/* a pause on a slow link: 30 s of video (the window holds the sender: samples, memory), 5 s without content, then the same content */
	c = base("P1 128 kb/s, 30 s video, pause 5 s, heavy", 16000.0); c.runMs = 30000u; c.pauseMs = 5000u; RUN(P1);
	c = base("P2 128 kb/s, 30 s video, pause 5 s, light 20 KB/s", 16000.0); c.runMs = 30000u; c.pauseMs = 5000u; c.pTileBytes = 0.61; RUN(P2);
	c = base("P3 256 kb/s, 30 s video, pause 5 s, light 40 KB/s", 32000.0); c.runMs = 30000u; c.pauseMs = 5000u; c.pTileBytes = 1.22; RUN(P3);
	c = base("P4 1 Mb/s, 30 s video, pause 5 s, heavy", 125000.0); c.runMs = 30000u; c.pauseMs = 5000u; RUN(P4);
	/* a long pause: the memory decays (30 s held, halved every 30 s) */
	c = base("L1 128 kb/s, 30 s video, pause 480 s, light 20 KB/s", 16000.0); c.runMs = 30000u; c.pauseMs = 480000u; c.pTileBytes = 0.61; RUN(L1);
	c = base("L2 256 kb/s, 30 s video, pause 480 s, light 40 KB/s", 32000.0); c.runMs = 30000u; c.pauseMs = 480000u; c.pTileBytes = 1.22; RUN(L2);
	c = base("L3 128 kb/s, 30 s video, pause 480 s, heavy", 16000.0); c.runMs = 30000u; c.pauseMs = 480000u; RUN(L3);
	c = base("L4 1 Mb/s, 30 s video, pause 480 s, light 150 KB/s", 125000.0); c.runMs = 30000u; c.pauseMs = 480000u; c.pTileBytes = 4.6; RUN(L4);
	/* the link got slow in the pause (40 Mb/s -> slow): the check of the resume */
	c = base("D1 40 Mb/s -> 128 kb/s in a 5 s pause, heavy", 5000000.0); c.linkBps2 = 16000.0; c.runMs = 30000u; c.pauseMs = 5000u; RUN(D1);
	c = base("D2 40 Mb/s -> 128 kb/s in a 5 s pause, light 20 KB/s", 5000000.0); c.linkBps2 = 16000.0; c.runMs = 30000u; c.pauseMs = 5000u; c.pTileBytes = 0.61; RUN(D2);
	c = base("D3 40 Mb/s -> 256 kb/s in a 5 s pause, light 40 KB/s", 5000000.0); c.linkBps2 = 32000.0; c.runMs = 30000u; c.pauseMs = 5000u; c.pTileBytes = 1.22; RUN(D3);
	c = base("D4 40 Mb/s -> 1 Mb/s in a 5 s pause, heavy", 5000000.0); c.linkBps2 = 125000.0; c.runMs = 30000u; c.pauseMs = 5000u; RUN(D4);
	c = base("D5 40 Mb/s -> 128 kb/s in a 60 s pause, light 20 KB/s", 5000000.0); c.linkBps2 = 16000.0; c.runMs = 30000u; c.pauseMs = 60000u; c.pTileBytes = 0.61; RUN(D5);
	c = base("D2L same as D2, 90 s after the pause", 5000000.0); c.linkBps2 = 16000.0; c.runMs = 30000u; c.pauseMs = 5000u; c.pTileBytes = 0.61; c.afterMs = 90000u; RUN(D2L);
	c = base("D6 40 Mb/s -> 256 kb/s in a 5 s pause, light 30 KB/s, 90 s", 5000000.0); c.linkBps2 = 32000.0; c.runMs = 30000u; c.pauseMs = 5000u; c.pTileBytes = 0.92; c.afterMs = 90000u; RUN(D6);
	/* light work for 30 s on a fast link (20 KB/s: the pipe empties, no clogged span), the link falls in the pause, then HEAVY content */
	c = base("D7 40 Mb/s -> 128 kb/s in a 5 s pause, light 20 KB/s, then heavy", 5000000.0); c.linkBps2 = 16000.0; c.runMs = 30000u; c.pauseMs = 5000u; c.pTileBytes = 0.61; c.pTileBytes2 = 7.0; RUN(D7);
	c = base("D8 40 Mb/s -> 256 kb/s in a 5 s pause, light 20 KB/s, then heavy", 5000000.0); c.linkBps2 = 32000.0; c.runMs = 30000u; c.pauseMs = 5000u; c.pTileBytes = 0.61; c.pTileBytes2 = 7.0; RUN(D8);
	c = base("D9 40 Mb/s -> 1 Mb/s in a 5 s pause, light 20 KB/s, then heavy", 5000000.0); c.linkBps2 = 125000.0; c.runMs = 30000u; c.pauseMs = 5000u; c.pTileBytes = 0.61; c.pTileBytes2 = 7.0; RUN(D9);
	c = base("D10 40 Mb/s -> 128 kb/s in a 60 s pause, light 20 KB/s, then heavy", 5000000.0); c.linkBps2 = 16000.0; c.runMs = 30000u; c.pauseMs = 60000u; c.pTileBytes = 0.61; c.pTileBytes2 = 7.0; RUN(D10);
	/* the mode of JPEG with ACK (no video): light work for 30 s, the link falls in the pause, then heavy JPEG */
	c = base("J1 JPEG only: 40 Mb/s -> 128 kb/s in a 5 s pause, light 20 KB/s, then heavy", 5000000.0); c.linkBps2 = 16000.0; c.runMs = 30000u; c.pauseMs = 5000u; c.jpegOnly = 1; RUN(J1);
	c = base("J2 JPEG only: 40 Mb/s -> 256 kb/s in a 5 s pause, light 20 KB/s, then heavy", 5000000.0); c.linkBps2 = 32000.0; c.runMs = 30000u; c.pauseMs = 5000u; c.jpegOnly = 1; RUN(J2);
	c = base("J3 JPEG only: 128 kb/s, 30 s light, pause 480 s, then heavy", 16000.0); c.runMs = 30000u; c.pauseMs = 480000u; c.jpegOnly = 1; RUN(J3);
	c = base("L3L same as L3, 60 s after", 16000.0); c.runMs = 30000u; c.pauseMs = 480000u; c.afterMs = 60000u; RUN(L3L);
	(void)rS1; (void)rS2; (void)rS3; (void)rS4; (void)rP1; (void)rP2; (void)rP3; (void)rP4; (void)rL1; (void)rL2; (void)rL3; (void)rL4; (void)rD1; (void)rD2; (void)rD3; (void)rD4; (void)rD5; (void)rD2L; (void)rD6; (void)rL3L; (void)rJ1; (void)rJ2; (void)rJ3; (void)rD7; (void)rD8; (void)rD9; (void)rD10;
#ifdef SIM_CHECK
	if (!only)
	{
		// 128 kb/s after a pause of 8 minutes (the memory of the rate is gone, the content is above the link and below the window): the window is sized by the sample of the clogged
		// link within a second, the first refusal comes early with a short queue (an earlier version: refusal after 5132 ms at 3995 ms of queue; the first one: after 9730 ms at 16 423 ms)
		CHECK(rL3.firstRefusal >= 0.0 && rL3.firstRefusal <= 8000.0);
		CHECK(rL3.qAtRefusal >= 0.0 && rL3.qAtRefusal <= 5000.0 && rL3.qMax <= 5000.0);
		CHECK(rL3.firstSample >= 0.0 && rL3.firstSample <= 2000.0);
		CHECK(rL3L.qMax <= 5000.0);
		// a link that fell to 128 kb/s in a pause of 5 s, light content (20 KB/s: the window is never refused by the content itself): the first refusal comes in seconds, not after 67 s with 256 KB
		// in the pipe (an earlier version: 16 432 ms at 4049 ms of queue; the first one: 66 832 ms at 16 365 ms)
		CHECK(rD2L.firstRefusal >= 0.0 && rD2L.firstRefusal <= 30000.0);
		CHECK(rD2L.qMax <= 5000.0 && rD2L.firstSample >= 0.0 && rD2L.firstSample <= 2000.0);
		// the start of a session on 128 kb/s with a first picture below W_START and light content: the queue after 5 s is the one of the window of 64 KB
		CHECK(rS2.q5 >= 0.0 && rS2.q5 <= 6000.0);
		// the other slow scenes do not change: a window that is sized by the samples that the window caused
		CHECK(rP1.qMax <= 4500.0 && rP2.qMax <= 4500.0 && rP3.qMax <= 400.0 && rL2.qMax <= 400.0 && rS3.qMax <= 9000.0);
		// light work, the link falls in a pause, then heavy content: the memory of the light work keeps the window of the new period at 64 KB (an earlier version: queue 4497 ms at 128 kb/s, 2095 ms
		// at 256 kb/s; another earlier version: 17 066 and 7407 ms with the window of 256 KB)
		CHECK(rD7.qMax <= 5500.0 && rD10.qMax <= 5000.0 && rD8.qMax <= 2600.0 && rD9.qMax <= 900.0);
		// the mode of JPEG with ACK, no video: the same scene as D7 and D8 with JPEG rectangles: 4088 and 2036 ms; the filters of and 16 396 and 8190 ms (the window of
		// 256 KB: light work left no memory). After 480 s of pause (J3) nothing is remembered in any version: the window of the start, 16 s
		CHECK(rJ1.qMax <= 5000.0 && rJ2.qMax <= 2600.0);
		CHECK(rD1.qMax <= 6000.0 && rD3.qMax <= 2500.0 && rD4.qMax <= 1500.0 && rD5.qMax <= 6000.0 && rD6.qMax <= 2500.0);
	}
	printf("%d checks, %d failed\n", gChecks, gFail);
	return gFail == 0 ? 0 : 1;
#else
	return 0;
#endif
}
