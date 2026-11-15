/*
a simulation of the start of a video session and of a link that falls while the video runs, with the real pieces of the agent that decide the rate: the stream layer (ACK window, rate
samples and their memory), the controller and the plan of the video (kvm_video_core.c), the rate control (kvm_video_rc.c) and the pace (kvm_pace.c). The tiles are not modelled: the screen shows
continuous video (the Windows 11 VM, scenario `video`: 872 tiles move, the generator draws about 30 frames a second), so every source frame leaves all of them pending.

The trace to reproduce is one measured on the Windows 11 VM (scenario `video`, an agent build before the proof of the first key frame): the first key frame of the session (228 KB, QP 38) at +3.9 s, a rate sample of 33 B/s
alive at that moment (link_ms=6877823 in the log line of the key frame), R at its floor of 100 kb/s a moment later, a byte bucket of -89 KB, nothing sent for 7 s, R climbing back after 12 s. The log
does not show where the sample of 33 B/s comes from (the window of the log begins at the key frame), so the pre-phase offers it as an input: positions of the size of a marker (28 B), one every 850 ms
(28 B / 0.85 s = 33 B/s: what the M2 probe marker and the position after a cursor tile give when the screen stands). Switch it off with trickle=0 for the same start without it.

The same program is built against the tree and, with the commit variables of run-kvm-start-sim-tests.sh, against the files of earlier commits. -DSIM_T6 = the files have
kvm_vctl_note_write. Output: tables (markdown) for the report. The checks are for the build against the tree only (-DSIM_CHECK).
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

static int gFail = 0, gChecks = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

typedef struct pos_t { uint32_t seq, ts; uint64_t cum; int ready; uint32_t readyAt; uint32_t extraMs; } pos_t;

typedef struct cfg
{
	const char *label;
	uint64_t linkBps, linkBps2;	// bytes per second, and from dropAt (ms after the start of the video)
	int dropAt;
	uint32_t oneWayMs;
	int trickle;				// positions of a marker's size every 850 ms before the video starts, each acknowledged trickleDelayMs late (a viewer that is busy)
	uint32_t trickleDelayMs;
	uint32_t trickleData;		// bytes of data (a cursor, a caret) written with every trickle position, 0 = the position only (28 B); 5000 B per 850 ms is 5.9 KB/s
	int gatedPicture;			// the first picture goes in pieces of 16 KB through the window (the gate of a big rectangle: a refusal is a refusal of the window), not in one write: a slow link
	uint32_t jpegAfterAck;		// bytes of a JPEG rectangle written right behind the ACK of the key frame (a text tile or a window next to the motion): with the window of 64 KB sized by the trickle, the video frame behind it is refused
	int series;					// 1: the content moves in series of 0.4 s every 2.8 s (an animation, a dragged window) from the key frame on, not all the time
	int pausedSession;			// 1: a session that has been running (R, the rate and the window known: the state of a series after a pause), the video begins with an IDR at +3.9 s as well
	uint32_t videoAtMs;			// the first video frame (the first IDR) comes this long after the start of the session
	int seconds;				// of the video
	double pTileBytes;			// bytes of a tile of a P frame at QP 26
	double idrBytesQp30;		// the key frame at QP 30
	int verbose;
} cfg;

typedef struct res
{
	uint32_t rAt[8];			// R kb/s at +0.5, +1, +1.5, +2, +3, +5, +8, +12 s after the key frame
	int bucketAt[8];			// bucket KB at the same times
	double rMin3Kbps;			// the lowest R in the first 3 s
	unsigned denied3, held3, frames3, frames10;
	double firstPMs;			// from the write of the key frame to the first P frame
	double r2MbpsMs;			// from the key frame until R >= 2 Mb/s (-1: not in the run)
	double idrKB, qpIdr;
	uint64_t rateAtIdr;			// B/s, what the stream layer told the rate control when the key frame was written
	int linkLimitedAtIdr;
	double queueMaxMs;
	double rEndKbps, fpsEnd;
	double rAfterDropKbps, qMaxAfterDropMs, rDropAt1s, rDropAt3s, rDropMin3s;	// rDropMin3s: the lowest R in the first 3 s after the fall (R of a link that fell saws between the link and the ceiling of the estimate: the remembered rate of the fast link, 30 s, is a lower bound that is never lowered)
	unsigned framesAfter, framesAll;
	uint32_t rAt65, rAt85;
	double firstSampleMs;		// from the start of the session to the first rate sample of the stream layer (-1: none in the run)
	uint64_t sampleAtFirst, rateEnd, winEnd, rateMemEnd;	// the first sample B/s; at the end of the run: the sample, the window, the remembered rate
} res;

static int cmpd(const void *a, const void *b) { const double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : (x > y ? 1 : 0); }

static res run(const cfg *c)
{
	res R;
	kvm_stream s;
	kvm_vctl ctl;
	kvm_vrc rc;
	static pos_t pos[100000];
	size_t npos = 0, firstUnready = 0, firstUndelivered = 0;
	double served = 0.0, srcAcc = 0.0;
	const uint32_t t0 = 5000, tEnd = 5000 + c->videoAtMs + (uint32_t)c->seconds * 1000u;
	uint32_t t, lastAck = 0, nextIter = 0, lastFrameT = 0, nextTrickle = 0, tIdr = 0, nextLog = 0;
	uint64_t linkBps = c->linkBps;
	int pendFlag = 0, haveIdr = 0, firstPDone = 0, picDone = 0, vidOn = 0, k;
	unsigned idrCount = 0, nTricklePos = 0;
	double encAvgUs = 8000.0, qMax = 0.0, rMin3 = 1e12;
	uint32_t idrBytes = 0;
	uint32_t picLeft = 574366u;
	uint32_t srcPeriodUs = 33333u;
	double dropT = -1.0;
	memset(&R, 0, sizeof(R));
	R.r2MbpsMs = -1.0; R.firstPMs = -1.0; R.firstSampleMs = -1.0;
	{
		kvm_ctrl op;
		kvm_state_msg reply;
		kvm_stream_init(&s, 1, 1, 1, 6);
		memset(&op, 0, sizeof(op)); op.ver = 1; op.op = KVM_OP_VIDEO_ON; op.flags = 7;
		(void)kvm_stream_on_ctrl(&s, &op, t0, &reply, NULL);
		kvm_vctl_init(&ctl, 1);
	}
	kvm_vrc_init(&rc, KVM_VIDEO_QP, t0);
	kvm_vrc_set_picture(&rc, 1920u * 1088u);
	if (c->pausedSession)
	{
		// the state of a session in a pause between two series (a link of 40 Mb/s that was measured, R 15 Mb/s, QP 26): the video of the next series begins with a P frame, no key frame
		rc.cap = 1975000u; rc.qp = 26; kvm_vrc_set_target(&rc, 1975000u, t0);
		s.rateMem = 1825000u; s.minRtt = 5u;
#ifdef SIM_T6
		s.rateMemAt = t0;
#endif
	}
	nextIter = t0 + c->videoAtMs - 1000u; nextTrickle = t0 + 1000u;
	for (t = t0; t < tEnd; t++)
	{
		const uint32_t rel = t - t0;
		// ---- the link ----
		if (c->dropAt > 0 && rel >= c->videoAtMs + (uint32_t)c->dropAt && linkBps != c->linkBps2) { linkBps = c->linkBps2; dropT = (double)rel; }
		served += (double)linkBps / 1000.0;
		if (served > (double)s.bytesWritten) { served = (double)s.bytesWritten; }
		while (firstUnready < npos && (double)pos[firstUnready].cum <= served + 0.5) { pos[firstUnready].ready = 1; pos[firstUnready].readyAt = t + c->oneWayMs + pos[firstUnready].extraMs; firstUnready++; }
		if (t - lastAck >= 3)
		{
			size_t newest = (size_t)-1, i;
			for (i = firstUndelivered; i < firstUnready; i++) { if (pos[i].ready && (int32_t)(t - pos[i].readyAt) >= 0) { newest = i; } }
			if (newest != (size_t)-1)
			{
				kvm_ack a;
				a.seq = pos[newest].seq; a.ts = pos[newest].ts; a.queueMs = 0; a.flags = 0;
				(void)kvm_stream_on_ack(&s, &a, t);
				firstUndelivered = newest + 1; lastAck = t;
			}
		}
		if (R.firstSampleMs < 0.0 && kvm_stream_rate(&s, t) > 0u) { R.firstSampleMs = (double)rel; R.sampleAtFirst = kvm_stream_rate(&s, t); }
		// ---- before the video: the first picture of the session (JPEG, 574 KB: bytes_full of the log), then a screen that stands: only positions of a marker's size ----
		if (!picDone && rel >= 100u)
		{
			uint32_t piece = picLeft;
			if (c->gatedPicture)
			{
				// pieces of 16 KB while the window lets a big rectangle go (kvm_ack_gate: a refusal is "the link holds the sender back"); at most one piece a millisecond
				piece = 0;
				if (kvm_stream_window_allows(&s, KVM_GATE_BIG, t)) { piece = picLeft > 16384u ? 16384u : picLeft; }
				else { kvm_stream_note_link_held(&s, t); }
			}
			if (piece > 0)
			{
				uint8_t mk[KVM_MARKER_BYTES];
				int probe = 0;
				kvm_stream_note_write(&s, piece, KVM_W_DATA, t);
				picLeft -= piece;
				if (picLeft == 0) { picDone = 1; }
				if (kvm_stream_marker_wanted(&s, t, 0, &probe)) { kvm_stream_make_marker(&s, t, probe, mk); kvm_stream_note_write(&s, KVM_MARKER_BYTES, KVM_W_POSITION, t); pos[npos].seq = s.seqCounter; pos[npos].ts = t; pos[npos].cum = s.bytesWritten; npos++; }
			}
		}
		if (c->trickle && !vidOn && (int32_t)(t - nextTrickle) >= 0 && rel + 900u < c->videoAtMs + 100u)
		{
			// one position of the size of a marker (nothing else was written): what the viewer acknowledges is 28 bytes
			uint8_t mk[KVM_MARKER_BYTES];
			uint32_t seq;
			if (c->trickleData > 0) { kvm_stream_note_write(&s, c->trickleData, KVM_W_DATA, t); }	// what the cursor and the caret wrote before the position
			seq = kvm_stream_add_position(&s, t, KVM_MARKER_BYTES);
			kvm_stream_build_marker(mk, seq, t, s.gen);
			kvm_stream_note_write(&s, KVM_MARKER_BYTES, KVM_W_POSITION, t);
			pos[npos].seq = seq; pos[npos].ts = t; pos[npos].cum = s.bytesWritten; pos[npos].extraMs = c->trickleDelayMs; npos++;
			nextTrickle = t + 850u; nTricklePos++;
		}
		// ---- the content: a source frame every 33 ms leaves every tile pending ----
		srcAcc += 1000.0;
		if (srcAcc >= (double)srcPeriodUs) { srcAcc -= (double)srcPeriodUs; if (rel + 100u >= c->videoAtMs && (!c->series || ((rel + 100u - c->videoAtMs) % 2800u) < 400u)) { pendFlag = 1; } }
		if ((int32_t)(t - nextIter) < 0) { continue; }
		// ---- one iteration of the video loop ----
		{
			const uint32_t interval = 16u;
			uint32_t next = t + 16u;
			(void)kvm_stream_tick(&s, t, 0, NULL);
			if (rel + 100u >= c->videoAtMs && !vidOn)
			{
				kvm_state_msg mm;
				(void)kvm_vctl_begin_gen(&ctl, &s, 100, 42, 1920, 1088, 1, &mm);
				vidOn = 1;
			}
			if (vidOn)
			{
				kvm_vrc_update(&rc, t, 0, kvm_stream_rate_or_mem(&s, t), kvm_stream_link_limited(&s, t));
				(void)kvm_vrc_tick(&rc, t);
				// the acknowledgement of the key frame (kvm_video_poll)
				{
					uint32_t ackAt = 0, ts = 0, queueMs = 0, minRtt = 0;
					if (kvm_stream_watch_result(&s, &ackAt, &ts, &queueMs, &minRtt)) { kvm_vrc_idr_acked(&rc, t, idrBytes, ts, ackAt, queueMs, minRtt);
						if (c->jpegAfterAck > 0)
						{
							uint8_t mk2[KVM_MARKER_BYTES]; int pr = 0;
							kvm_stream_note_write(&s, c->jpegAfterAck, KVM_W_DATA, t);
							if (kvm_stream_marker_wanted(&s, t, 0, &pr)) { kvm_stream_make_marker(&s, t, pr, mk2); kvm_stream_note_write(&s, KVM_MARKER_BYTES, KVM_W_POSITION, t); pos[npos].seq = s.seqCounter; pos[npos].ts = t; pos[npos].cum = s.bytesWritten; npos++; }
						}
						if (c->verbose) { printf("   +%u ms: idr acked ms=%d queue=%u min_rtt=%u c_idr=%llu B/s\n", (unsigned)(t - tIdr), (int)(ackAt - ts), (unsigned)queueMs, (unsigned)minRtt, (unsigned long long)rc.cIdr); } }
				}
				if (pendFlag)
				{
					kvm_vplan_in in;
					kvm_vplan plan;
					in.now = t; in.pending = 872u; in.gateOpen = kvm_stream_window_allows(&s, KVM_GATE_VIDEO, t); in.rateOpen = kvm_vrc_peek(&rc, t);
					in.intervalMs = interval; in.rate = kvm_stream_rate_or_mem(&s, t);
					kvm_vctl_plan(&ctl, &s, &in, &plan);
					if (plan.held) { if (SIM_NOTE_LINK_HELD(&s, t)) { kvm_vrc_window_refused(&rc); } else { SIM_REFUSED_SOFT(&rc); } if (haveIdr && rel < tIdr - t0 + 3000u) { R.held3++; } }
					if (plan.heldRate) { (void)kvm_vrc_may_encode(&rc, t, 0); }
					if (plan.encode)
					{
						const int idr = ctl.idrFirst || plan.forceIdr || ctl.idrForce;
						const int qp = kvm_vrc_qp(&rc, idr);
						const double dt = lastFrameT ? (double)(t - lastFrameT) : 100.0;
						double b;
						uint32_t bytes, seq, tEncEnd = t + (idr ? 50u : 10u);
						(void)kvm_vrc_may_encode(&rc, t, idr);
						if (idr) { b = c->idrBytesQp30 * pow(0.94, (double)(qp - 30)); }
						else { b = c->pTileBytes * 872.0 * pow(0.89, (double)(qp - 26)) * (1.0 + 0.25 * (dt > 16.7 ? ((dt > 100.0 ? 100.0 : dt) / 16.7 - 1.0) : 0.0)); }
						bytes = (uint32_t)b;
						{ const int hadFrame = ctl.haveFrame; const uint32_t prevSlot = ctl.lastFrame; (void)kvm_vctl_frame_done(&ctl, idr, bytes, t); ctl.lastFrame = kvm_pace_slot(hadFrame, prevSlot, t, interval, NULL); }
#ifdef SIM_T6
						kvm_vctl_note_write(&ctl, tEncEnd, (uint32_t)((encAvgUs + 500.0) / 1000.0));
#endif
						encAvgUs = encAvgUs * 0.875 + (idr ? 50000.0 : 10000.0) * 0.125;
						if (idr)
						{
							R.idrKB = (double)bytes / 1024.0; R.qpIdr = qp; R.rateAtIdr = in.rate; R.linkLimitedAtIdr = kvm_stream_link_limited(&s, t); tIdr = t; haveIdr = 1; idrCount++; idrBytes = bytes;
						}
						kvm_vrc_frame(&rc, t, bytes, qp, idr, (idr ? 50000u : 10000u));
						seq = kvm_stream_add_position(&s, t, bytes);
						kvm_stream_note_write(&s, bytes, KVM_W_POSITION, t);
						if (idr) { kvm_stream_watch(&s, seq); }
						pos[npos].seq = seq; pos[npos].ts = t; pos[npos].cum = s.bytesWritten; npos++;
						lastFrameT = t; pendFlag = 0;
						if (haveIdr && !idr)
						{
							if (!firstPDone) { firstPDone = 1; R.firstPMs = (double)(t - tIdr); }
							R.framesAll++;
							if (t - tIdr < 3000u) { R.frames3++; }
							if (t - tIdr < 10000u) { R.frames10++; }
							if (dropT >= 0.0 && (double)rel > dropT + 1000.0) { R.framesAfter++; }
						}
						next = tEncEnd;
					}
				}
				{
					uint8_t mk[KVM_MARKER_BYTES];
					int probe = 0;
					if (kvm_stream_marker_wanted(&s, t, 0, &probe) && npos < 99000) { kvm_stream_make_marker(&s, t, probe, mk); kvm_stream_note_write(&s, KVM_MARKER_BYTES, KVM_W_POSITION, t); pos[npos].seq = s.seqCounter; pos[npos].ts = t; pos[npos].cum = s.bytesWritten; npos++; }
				}
				if (haveIdr)
				{
					const uint32_t since = t - tIdr;
					const double rk = (double)rc.rate * 8.0 / 1000.0;
					static const uint32_t at[8] = { 500, 1000, 1500, 2000, 3000, 5000, 8000, 12000 };
					if (since >= 65000u && R.rAt65 == 0) { R.rAt65 = (uint32_t)rk; }
					if (since >= 85000u && R.rAt85 == 0) { R.rAt85 = (uint32_t)rk; }
					if (since < 3000u && rk < rMin3) { rMin3 = rk; }
					for (k = 0; k < 8; k++) { if (since >= at[k] && R.rAt[k] == 0) { R.rAt[k] = (uint32_t)rk; R.bucketAt[k] = (int)(rc.tokens / 1024); } }
					if (R.r2MbpsMs < 0.0 && rk >= 2000.0 && since > 100u) { R.r2MbpsMs = (double)since; }
					if (dropT >= 0.0 && (double)rel >= dropT && (double)rel < dropT + 3000.0 && (R.rDropMin3s == 0.0 || rk < R.rDropMin3s)) { R.rDropMin3s = rk; }
					if (dropT >= 0.0 && (double)rel >= dropT + 1000.0 && R.rDropAt1s == 0.0) { R.rDropAt1s = rk; }
					if (dropT >= 0.0 && (double)rel >= dropT + 3000.0 && R.rDropAt3s == 0.0) { R.rDropAt3s = rk; }
					R.rEndKbps = rk;
				}
				if (c->verbose && haveIdr && (int32_t)(t - nextLog) >= 0) { nextLog = t + 500u; printf("   +%5u ms: R=%6.0f kb/s rate=%8llu B/s W=%7u limited=%d bucket=%6d KB qp=%d denied=%u held=%u frames=%u\n", (unsigned)(t - tIdr), (double)rc.rate * 8.0 / 1000.0, (unsigned long long)kvm_stream_rate_or_mem(&s, t), (unsigned)kvm_stream_window(&s, t), kvm_stream_link_limited(&s, t), (int)(rc.tokens / 1024), rc.qp, rc.sDenied, rc.sHeld, ctl.frames); }
				if (haveIdr && t - tIdr < 3000u) { R.denied3 = rc.sDenied; }
			}
			{
				const double q = (double)(s.bytesWritten - (uint64_t)served) * 1000.0 / (double)linkBps;
				if (haveIdr && q > qMax) { qMax = q; }
				if (dropT >= 0.0 && (double)rel >= dropT && q > R.qMaxAfterDropMs) { R.qMaxAfterDropMs = q; }
			}
			nextIter = next > t ? next : t + 1u;
		}
	}
	R.rateEnd = kvm_stream_rate(&s, tEnd); R.winEnd = kvm_stream_window(&s, tEnd); R.rateMemEnd = kvm_stream_rate_or_mem(&s, tEnd);
	R.rMin3Kbps = rMin3 < 1e11 ? rMin3 : 0.0;
	R.queueMaxMs = qMax;
	R.fpsEnd = 0.0;
	(void)nTricklePos; (void)cmpd;
	return R;
}

static cfg base(const char *label, uint64_t bps)
{
	cfg c;
	memset(&c, 0, sizeof(c));
	c.label = label; c.linkBps = bps; c.linkBps2 = bps; c.oneWayMs = 20; c.trickle = 1; c.trickleDelayMs = 600u; c.videoAtMs = 3900u; c.seconds = 20;
	c.pTileBytes = 7.0; c.idrBytesQp30 = 228.0 * 1024.0 / pow(0.94, 8.0);
#ifdef SIM_IDR_KB
	c.idrBytesQp30 = (double)(SIM_IDR_KB) * 1024.0 / pow(0.94, 8.0);	/* the first key frame of the session is this big (a flat key frame is a fraction of the picture) */
#endif
	return c;
}

int main(int argc, char **argv)
{
	cfg c;
	res w, w0, slow, drop1, drop4, run40, early1, early05, late, ser, ser0;
	res er3, er5, er8, wr5, wr8, sl128, sl256, sl128r, sl256r, sl1m;
	int verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
#ifdef SIM_BUILD
	printf("simulation build: %s\n\n", SIM_BUILD);
#endif
	// 1. the trace of the Windows 11 VM: a fast LAN (100 Mb/s, one way 20 ms), the key frame at +3.9 s with the rate sample of 33 B/s alive
	c = base("Windows 11 VM video", 12500000u); c.verbose = verbose; w = run(&c);
	// 2. the same start without a trickle before it (nothing known: the rate control starts from 2 Mb/s)
	c = base("no sample before the key frame", 12500000u); c.trickle = 0; c.verbose = 0; w0 = run(&c);
	// 3. a slow link from the start (1 Mb/s): the safety of the link-limited case; the trickle is there too
	c = base("1 Mb/s from the start", 125000u); c.verbose = 0; c.seconds = 30; slow = run(&c);
	// 4. the link falls while the video runs: 100 Mb/s to 1 Mb/s and to 4 Mb/s at +6 s of the video
	c = base("link 100 -> 1 Mb/s", 12500000u); c.trickle = 0; c.linkBps2 = 125000u; c.dropAt = 6000; c.verbose = 0; drop1 = run(&c);
	c = base("link 100 -> 4 Mb/s", 12500000u); c.trickle = 0; c.linkBps2 = 500000u; c.dropAt = 6000; c.verbose = 0; drop4 = run(&c);
	// 4b. the link falls right behind the proof of the key frame (+1 s after the key frame, the proof of the key frame still stands): to 1 Mb/s and to 0.5 Mb/s
	c = base("fall at +1 s to 1 Mb/s", 12500000u); c.linkBps2 = 125000u; c.dropAt = 1000; c.seconds = 20; early1 = run(&c);
	c = base("fall at +1 s to 0.5 Mb/s", 12500000u); c.linkBps2 = 62500u; c.dropAt = 1000; c.seconds = 30; early05 = run(&c);
	// 4c. a JPEG of 100 KB right behind the ACK of the key frame (the window of 64 KB refuses the video frame before there is a sample: the proof of the key frame is taken away
	// by the refusal); and a session of series of 0.4 s every 2.8 s for 90 s (the rate memory makes no sample of a series of less than 0.5 s, the proof expires after 60 s), with and without the trickle
	c = base("JPEG behind the ACK of the key frame", 12500000u); c.jpegAfterAck = 100000u; c.seconds = 20; late = run(&c);
	c = base("series 0.4 s every 2.8 s, 90 s", 12500000u); c.series = 1; c.seconds = 90; ser = run(&c);
	c = base("series, no trickle", 12500000u); c.series = 1; c.trickle = 0; c.seconds = 90; ser0 = run(&c);
	// 4d. a trickle richer than a marker: a text cursor and a caret write 3, 5 or 8 KB in every 850 ms (3.5 - 9.4 KB/s: below the smallest R of 12.5 KB/s, but more than 4 KB per span, which
	// passed the 4 KB filter as a rate sample): the first sample of a session, 100 KB of JPEG behind the ACK of the key frame, and the trace of the Windows 11 VM with the same trickle
	c = base("E 3 KB/850 ms", 12500000u); c.jpegAfterAck = 100000u; c.trickleData = 3000u; er3 = run(&c);
	c = base("E 5 KB/850 ms", 12500000u); c.jpegAfterAck = 100000u; c.trickleData = 5000u; er5 = run(&c);
	c = base("E 8 KB/850 ms", 12500000u); c.jpegAfterAck = 100000u; c.trickleData = 8000u; er8 = run(&c);
	c = base("Windows 11 VM 5 KB/850 ms", 12500000u); c.trickleData = 5000u; wr5 = run(&c);
	c = base("Windows 11 VM 8 KB/850 ms", 12500000u); c.trickleData = 8000u; wr8 = run(&c);
	// 4e. a link that is slow (128 and 256 kb/s): the samples are small because the link is slow and the window holds the sender back (the first picture goes through the gate in pieces): they must still be samples
	c = base("128 kb/s", 16000u); c.gatedPicture = 1; c.seconds = 40; sl128 = run(&c);
	c = base("256 kb/s", 32000u); c.gatedPicture = 1; c.seconds = 40; sl256 = run(&c);
	c = base("128 kb/s, 5 KB trickle", 16000u); c.gatedPicture = 1; c.trickleData = 5000u; c.seconds = 40; sl128r = run(&c);
	c = base("256 kb/s, 5 KB trickle", 32000u); c.gatedPicture = 1; c.trickleData = 5000u; c.seconds = 40; sl256r = run(&c);
	c = base("1 Mb/s, gated picture", 125000u); c.gatedPicture = 1; c.seconds = 30; sl1m = run(&c);
	// 5. steady video on a 40 Mb/s link, a session that has run (R and the rate remembered)
	c = base("steady 40 Mb/s", 5000000u); c.trickle = 0; c.pausedSession = 1; c.verbose = 0; run40 = run(&c);
	printf("Table 1: the start of the video (the key frame at +3.9 s of the session). R, the rate that the stream layer gave when the key frame was written, the bucket and the first P frame\n\n");
	printf("| scene | rate at the key frame B/s | link limited | IDR KB | R kb/s at +0.5 / +1 / +2 / +3 s | bucket KB at +0.5 / +1 / +2 / +3 s | bucket refusals / window refusals in the first 3 s | first P frame after ms | lowest R in 3 s kb/s | R >= 2 Mb/s after ms | R at +5 s / +8 s kb/s | queue max ms |\n|---|---|---|---|---|---|---|---|---|---|---|---|\n");
	{
		const struct { const char *n; const res *r; } all[] = { { "Windows 11 VM video, 100 Mb/s", &w }, { "the same, no sample before the key frame", &w0 }, { "1 Mb/s from the start, trickle", &slow } };
		size_t i;
		for (i = 0; i < sizeof(all) / sizeof(all[0]); i++)
		{
			const res *r = all[i].r;
			printf("| %s | %llu | %d | %.0f | %u / %u / %u / %u | %d / %d / %d / %d | %u / %u | %.0f | %.0f | %.0f | %u / %u | %.0f |\n", all[i].n, (unsigned long long)r->rateAtIdr, r->linkLimitedAtIdr, r->idrKB, r->rAt[0], r->rAt[1], r->rAt[3], r->rAt[4],
				r->bucketAt[0], r->bucketAt[1], r->bucketAt[3], r->bucketAt[4], r->denied3, r->held3, r->firstPMs, r->rMin3Kbps, r->r2MbpsMs, r->rAt[5], r->rAt[6], r->queueMaxMs);
		}
	}
	printf("\nTable 1b: the first key frame proof. JPEG of 100 KB behind the ACK of the key frame (video frames in 3 s / 10 s, first P frame ms, R at +8 s kb/s) and 90 s of series of 0.4 s every 2.8 s (video frames in 90 s, R at +65 s / +85 s kb/s)\n\n| scene | video frames in 3 s / 10 s | first P frame ms | R at +8 s kb/s | video frames in 90 s | R at +65 s / +85 s kb/s |\n|---|---|---|---|---|---|\n");
	printf("| JPEG behind the ACK, trickle | %u / %u | %.0f | %u | - | - |\n| series, trickle | %u / %u | %.0f | - | %u | %u / %u |\n| series, no trickle | %u / %u | %.0f | - | %u | %u / %u |\n", late.frames3, late.frames10, late.firstPMs, late.rAt[6], ser.frames3, ser.frames10, ser.firstPMs, ser.framesAll, ser.rAt65, ser.rAt85, ser0.frames3, ser0.frames10, ser0.firstPMs, ser0.framesAll, ser0.rAt65, ser0.rAt85);
	printf("\nTable 1c: a trickle of 3 / 5 / 8 KB in every 850 ms before the key frame (E: and 100 KB of JPEG behind the ACK; Windows 11 VM: its trace), video frames in 3 s / 10 s, first P frame ms, R at +0.5 s and +8 s kb/s, the first sample of the stream layer (ms after the start of the session, B/s)\n\n| scene | video frames in 3 s / 10 s | first P frame ms | R at +0.5 s / +8 s kb/s | first sample ms / B/s |\n|---|---|---|---|---|\n");
	{
		const struct { const char *n; const res *r; } all[] = { { "E, marker only (reference)", &late }, { "E, 3 KB / 850 ms", &er3 }, { "E, 5 KB / 850 ms", &er5 }, { "E, 8 KB / 850 ms", &er8 }, { "Windows 11 VM, marker only (reference)", &w }, { "Windows 11 VM, 5 KB / 850 ms", &wr5 }, { "Windows 11 VM, 8 KB / 850 ms", &wr8 } };
		size_t i;
		for (i = 0; i < sizeof(all) / sizeof(all[0]); i++)
		{
			const res *r = all[i].r;
			printf("| %s | %u / %u | %.0f | %u / %u | %.0f / %llu |\n", all[i].n, r->frames3, r->frames10, r->firstPMs, r->rAt[0], r->rAt[6], r->firstSampleMs, (unsigned long long)r->sampleAtFirst);
		}
	}
	printf("\nTable 1d: a slow link from the start, the first picture through the gate of the window: the first rate sample of the stream layer, at the end of the run (40 s of the session or 30 s) the sample, the window and the remembered rate, R at +8 s, queue max ms\n\n| scene | first sample ms / B/s | sample / window KiB / remembered rate B/s at the end | R at +3 s / +8 s kb/s | queue max ms |\n|---|---|---|---|---|\n");
	{
		const struct { const char *n; const res *r; } all[] = { { "128 kb/s (16 KB/s)", &sl128 }, { "128 kb/s, trickle 5 KB / 850 ms", &sl128r }, { "256 kb/s (32 KB/s)", &sl256 }, { "256 kb/s, trickle 5 KB / 850 ms", &sl256r }, { "1 Mb/s, gated picture", &sl1m } };
		size_t i;
		for (i = 0; i < sizeof(all) / sizeof(all[0]); i++)
		{
			const res *r = all[i].r;
			printf("| %s | %.0f / %llu | %llu / %llu / %llu | %u / %u | %.0f |\n", all[i].n, r->firstSampleMs, (unsigned long long)r->sampleAtFirst, (unsigned long long)r->rateEnd, (unsigned long long)(r->winEnd / 1024u), (unsigned long long)r->rateMemEnd, r->rAt[4], r->rAt[6], r->queueMaxMs);
		}
	}
	printf("\nvideo frames in the first 3 s after the key frame / in the first 10 s: Windows 11 VM %u / %u, no sample %u / %u, 1 Mb/s %u / %u\n", w.frames3, w.frames10, w0.frames3, w0.frames10, slow.frames3, slow.frames10);
	printf("\nTable 2: the link falls while the video runs (at +6 s of the video; the last two rows 1 s after the start of the video, with the trickle before the key frame, behind the proof of the key frame)\n\n| scene | R kb/s 1 s / 3 s after the fall | queue max after the fall ms | video frames later than 1 s after the fall |\n|---|---|---|---|\n");
	printf("| 100 -> 1 Mb/s | %.0f / %.0f | %.0f | %u |\n| 100 -> 4 Mb/s | %.0f / %.0f | %.0f | %u |\n| 100 -> 1 Mb/s at +1 s | %.0f / %.0f | %.0f | %u |\n| 100 -> 0.5 Mb/s at +1 s | %.0f / %.0f | %.0f | %u |\n", drop1.rDropAt1s, drop1.rDropAt3s, drop1.qMaxAfterDropMs, drop1.framesAfter, drop4.rDropAt1s, drop4.rDropAt3s, drop4.qMaxAfterDropMs, drop4.framesAfter, early1.rDropAt1s, early1.rDropAt3s, early1.qMaxAfterDropMs, early1.framesAfter, early05.rDropAt1s, early05.rDropAt3s, early05.qMaxAfterDropMs, early05.framesAfter);
	printf("\nsteady video on 40 Mb/s with the state of a session that has run: R at the end %.0f kb/s, P frames in the first 10 s %u\n", run40.rEndKbps, run40.frames10);
#ifdef SIM_IDR_KB
	{
		// the start of the video with a small first key frame (SIM_IDR_KB) and with / without the minimum size for the proof of a key frame (KVM_VRC_IDR_PROOF_MIN_BYTES): R and the frames after the key frame
		const struct { const char *n; uint64_t bps; int trickle; } scn[] = { { "100 Mb/s, trickle", 12500000u, 1 }, { "40 Mb/s, trickle", 5000000u, 1 }, { "40 Mb/s, no sample", 5000000u, 0 }, { "10 Mb/s, trickle", 1250000u, 1 }, { "3 Mb/s, trickle", 375000u, 1 } };
		size_t i;
		for (i = 0; i < sizeof(scn) / sizeof(scn[0]); i++)
		{
			cfg q = base(scn[i].n, scn[i].bps);
			res x;
			q.trickle = scn[i].trickle;
			x = run(&q);
			printf("IDR1-SIM idr_kb=%d proof_min=%d scene=\"%s\": key frame %.0f KB, R kb/s at +0.5 / +1 / +2 / +3 / +8 s = %u / %u / %u / %u / %u, frames 3 s / 10 s = %u / %u, first P %.0f ms, R >= 2 Mb/s after %.0f ms, queue max %.0f ms\n", (int)(SIM_IDR_KB), (int)(KVM_VRC_IDR_PROOF_MIN_BYTES), scn[i].n, x.idrKB, x.rAt[0], x.rAt[1], x.rAt[3], x.rAt[4], x.rAt[6], x.frames3, x.frames10, x.firstPMs, x.r2MbpsMs, x.queueMaxMs);
		}
	}
#endif
#ifdef SIM_CHECK
	// the criteria of no starvation. A trickle before the first key frame is not worse than no sample at all (what is compared is the start without the trickle, not a time that the model produces:
	// the key frame of the model is written at R of 100 kb/s when the trickle sets it, so its debt is small and the first P frame comes early, in the real trace the key frame went at 2 Mb/s and the debt took 0.2 s)
	CHECK(w.firstPMs >= 0.0 && w.firstPMs <= 500.0);					// the first P frame behind the key frame
	CHECK(w.rAt[0] >= 1500u && w.rAt[1] >= 1500u);						// R at +0.5 s and +1 s: not the floor of 100 kb/s (the model of the ceiling: 4 Mb/s)
	CHECK(w.bucketAt[1] > -40);											// no debt that holds the video for seconds
	CHECK(w.frames3 * 10u >= w0.frames3 * 9u && w.frames10 * 10u >= w0.frames10 * 9u);	// the frames of the first 3 s and 10 s: at least 0.9 x those of the start without the trickle
	CHECK(w0.firstPMs >= 0.0 && w0.firstPMs <= 500.0);
	CHECK(w0.frames3 >= 70u);
	CHECK(late.frames3 * 10u >= w0.frames3 * 9u && late.frames10 * 10u >= w0.frames10 * 9u);	// a refusal of the window between the ACK of the key frame and the first sample: no starvation
	CHECK(late.firstPMs >= 0.0 && late.firstPMs <= 500.0);
	CHECK(ser.framesAll * 10u >= ser0.framesAll * 9u);					// 90 s of series: as many frames as without the trickle
	CHECK(ser.rAt65 >= 1000u && ser.rAt85 >= 1000u);					// ... and R after the 60 s of the proof is not the floor
	// the rate filter: a trickle of 3 - 8 KB in 850 ms before the key frame is not the first sample (an earlier version: E 5 and 8 KB 4 / 139 frames and the first P frame after 1378 ms)
	CHECK(er3.frames3 * 10u >= w0.frames3 * 9u && er3.frames10 * 10u >= w0.frames10 * 9u && er3.firstPMs >= 0.0 && er3.firstPMs <= 500.0);
	CHECK(er5.frames3 * 10u >= w0.frames3 * 9u && er5.frames10 * 10u >= w0.frames10 * 9u && er5.firstPMs >= 0.0 && er5.firstPMs <= 500.0);
	CHECK(er8.frames3 * 10u >= w0.frames3 * 9u && er8.frames10 * 10u >= w0.frames10 * 9u && er8.firstPMs >= 0.0 && er8.firstPMs <= 500.0);
	CHECK(er5.rAt[0] >= 1500u && er8.rAt[0] >= 1500u && er5.rAt[6] >= 1500u && er8.rAt[6] >= 1500u);	// R is not the floor of 100 kb/s
	CHECK(wr5.frames3 * 10u >= w0.frames3 * 9u && wr8.frames3 * 10u >= w0.frames3 * 9u && wr5.firstPMs <= 500.0 && wr8.firstPMs <= 500.0 && wr5.rAt[0] >= 1500u && wr8.rAt[0] >= 1500u);
	// the other side: a slow link (128 and 256 kb/s) whose samples are small because the link is slow and the window holds the sender back still makes samples, sizes the window and keeps R at the link
	CHECK(sl128.firstSampleMs >= 0.0 && sl128.firstSampleMs <= 4000.0 && sl128.rateEnd >= 12000u && sl128.rateEnd <= 20000u && sl128.winEnd <= 64u * 1024u);
	CHECK(sl256.firstSampleMs >= 0.0 && sl256.firstSampleMs <= 4000.0 && sl256.rateEnd >= 24000u && sl256.rateEnd <= 40000u && sl256.winEnd <= 64u * 1024u);
	CHECK(sl128r.firstSampleMs >= 0.0 && sl128r.firstSampleMs <= 4000.0 && sl128r.rateEnd >= 12000u && sl128r.rateEnd <= 20000u && sl128r.winEnd <= 64u * 1024u);
	CHECK(sl256r.firstSampleMs >= 0.0 && sl256r.firstSampleMs <= 4000.0 && sl256r.rateEnd >= 24000u && sl256r.rateEnd <= 40000u && sl256r.winEnd <= 64u * 1024u);
	CHECK(sl1m.firstSampleMs >= 0.0 && sl1m.rAt[6] <= 1500u && sl1m.queueMaxMs <= 2500.0);
	CHECK(slow.rAt[4] <= 1000u);										// 1 Mb/s: R does not run away from the link (3 s after the key frame)
	CHECK(slow.rAt[6] <= 1200u && slow.rAt[7] <= 1200u);
	CHECK(slow.queueMaxMs <= 2000.0);									// the key frame of 228 KB is 1.8 s at 1 Mb/s; nothing piles up on top of it
	CHECK(drop1.qMaxAfterDropMs <= 700.0);								// the link falls: the queue is bounded (580 ms before the first key frame proof and alike)
	CHECK(drop1.rDropAt1s <= 1600.0 && drop1.rDropMin3s <= 900.0);	// R is at the link within 3 s of the fall (it saws afterwards and the tree alike: 1565 / 851 kb/s at +1 s / +3 s there, 851 / 1565 here)
	CHECK(drop4.rDropAt1s <= 4500.0);
	CHECK(early1.rDropAt3s <= 900.0 && early1.qMaxAfterDropMs <= 700.0);	// the link falls behind the proof of the key frame: the proof is gone with the first refusal of the window
	CHECK(early05.rDropAt3s <= 450.0 && early05.qMaxAfterDropMs <= 1600.0);	//
	CHECK(run40.rEndKbps >= 1500.0);									// a session that has run keeps R
#endif
	printf("%d checks, %d failed\n", gChecks, gFail);
	return gFail == 0 ? 0 : 1;
}
