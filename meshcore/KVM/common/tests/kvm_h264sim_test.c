/*
a simulation of the sharpening and the quality control of the H.264 video on a limited link, calibrated on the measurement: the Windows 11 VM, a gradient video of 1280x688
(876 tiles of 32x32), a link of 1 / 3 / 5 Mb/s, ACK window, an IDR every 10 s (the recording of the server asked for one), a loop of ~34 ms, 11 ms of encoding.

The real pieces are used where they are portable C: the stream layer (ACK window, rate measurement), the controller and the plan of the video (kvm_video_core.c), the rate control (kvm_video_rc.c), the
pace (kvm_pace.c) and the governor of the sharpening (kvm_sharpen.c). What is not portable lives in Windows/kvm_adapt.cpp and is MODELLED here, in the "legacy" form, line by line from:
  - the candidates: kvm_adapt.cpp is_candidate()  =  lossy && !deferred && (DWORD)(gA.now - t->lastChange) >= 150 && crc == contentCrc && !pending   (gA.now = the start of the iteration)
  - the tile that a video frame paints: kvm_video.cpp tiles_painted()  lastChange = timeGetTime() AFTER the encoder (here t + encode time)
  - the budget of an iteration: kvm_adapt_sharpen_budget()  96 KiB (level 0 of the regulator), taken once if inflight < 0.5 W; then the loop of kvm.c sends packets of up to 8 tiles until it is spent,
    WITHOUT looking at the window again.
Switches of the model: z0 (the signed comparison, kvm_sharp_settled, instead of the unsigned one), z12 (the governor: bucket, per-packet window gate, 0.25 W per iteration). The QP rules of the rate
control are in kvm_video_rc.c itself: the same program built against the rc of (-DSIM_OLD_RC, run-kvm-h264sim-tests.sh) gives the "before" rows.

Output: tables (markdown) for the report. The checks are the bounds of the plan: reproduction of the measurement by the legacy model, and the targets of 6.2 for the new one.
*/

#include "../kvm_video_rc.h"
#include "../kvm_video_core.h"
#include "../kvm_pace.h"
#include "../kvm_stream.h"
#include "../kvm_sharpen.h"
#ifdef SIM_OLD_RC
#define SIM_RATE_SHARP(s, t) kvm_stream_rate_or_mem((s), (t))
#else
#define SIM_RATE_SHARP(s, t) kvm_stream_rate_recent((s), (t))
#endif
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

static int sim_settled_fixed(uint32_t now, uint32_t lc, uint32_t ms) { return kvm_sharp_settled(now, lc, ms); }
// SETTLED: the candidate rule. Fixed : the signed comparison; legacy: kvm_adapt.cpp is_candidate, (DWORD)(gA.now - t->lastChange) >= KVM_ADAPT_SETTLE_MS
#define SETTLED(nowv, lcv) (c->z0 ? sim_settled_fixed((nowv), (lcv), SETTLE) : ((uint32_t)((nowv) - (lcv)) >= SETTLE))

static uint32_t gRng = 777u;
static double rnd01(void) { gRng = (gRng * 1664525u) + 1013904223u; return (double)(gRng >> 8) / 16777216.0; }

#ifndef SIM_P_BASE
#define SIM_P_BASE 5000.0	// bytes of a P frame at QP 26 that follows 100 ms of the gradient (measured: 7.3 KB at 12-24 frames a second, 10.5 KB at 2 frames a second)
#endif
#ifndef SIM_P_EXP
#define SIM_P_EXP 1.0		// ... and how it grows with the time since the previous frame
#endif
#define NT 876
#define SETTLE 150u
enum { T_JPEG = 0, T_PEND = 1, T_VIDEO = 2 };

typedef struct simcfg
{
	const char *name;
	uint64_t linkBps;
	int z0, z12;
	double qpSlope;			// size per QP step: 0.89 = the encoder listens to the QP; 1.0 = it does not (Windows 11 build 26200)
	double quietFrac;		// share of the tiles that change only now and then (the background of the gradient between the circles)
	double quietP;			// ... with this probability per capture
	int stopMs;				// the motion stops
	int seconds;
	int idrEveryMs;
	int verbose;
} simcfg;

typedef struct simres
{
	double fps, videoKBs, sharpKBs, share, sharpMotionKBs, qpAvg, qpMax, latAvg, latP95, settleMs, settleKB, idrKB, idrQp, idrQpAvg, idrLatMax, rAvg;
	unsigned qpUp, qpDown, qpBlocked;
	long frames;
	long sharpPackets, forced;
	double bucketRefusedPerS, windowRefusedPerS, iterCappedPerS;
} simres;

typedef struct pos_t { uint32_t seq, ts; uint64_t cum; int ready; uint32_t readyAt; } pos_t;
static float gLat[200000];

static int cmpf(const void *a, const void *b) { const float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : (x > y ? 1 : 0); }

static simres simulate(const simcfg *c)
{
	simres res;
	kvm_stream s;
	kvm_vctl ctl;
	kvm_vrc rc;
	kvm_budget budget;
	static pos_t pos[300000];
	kvm_sharp sg;
	uint8_t st[NT];
	uint32_t lc[NT];
	size_t npos = 0, firstUnready = 0, firstUndelivered = 0;
	double served = 0.0, encAvgUs = 11000.0;
	const uint32_t t0 = 5000, tStart = 15000, tEnd = 5000 + (uint32_t)c->seconds * 1000u;
	uint32_t t, lastAck = 0, nextIter = 0, lastFrameT = 0, lastForced = 0, nextIdr = 0, dueSince = 0, stopT;
	int forcedHave = 0, cursor = 0, nLat = 0, k, pend = 0, nVideo = 0, settled = 0;
	long forcedN = 0;
	int idrCount = 0, idrQpN = 0;
#ifndef SIM_OLD_RC
	uint32_t idrWatchBytes = 0;
#endif
	double idrQpSum = 0.0;
	int64_t normalBytesDbg = 0;
	const int nBusy = (int)((1.0 - c->quietFrac) * NT);
	const uint32_t encMs = 11;
	long frames = 0, packets = 0;
	uint64_t vBytes = 0, shBytes = 0, shMotion = 0, bytesQ = 0;
	double qpSum = 0, rSum = 0; long qpN = 0, rN = 0;
	unsigned up0 = 0, down0 = 0, blocked0 = 0;
	int measuredStarted = 0;
	uint64_t settleB0 = 0; int stopped = 0;
	uint32_t cBucket0 = 0, cWin0 = 0, cCap0 = 0;
	memset(&res, 0, sizeof(res));
	{
		kvm_ctrl op;
		kvm_state_msg m, reply;
		kvm_stream_init(&s, 1, 1, 1, 6);
		memset(&op, 0, sizeof(op)); op.ver = 1; op.op = KVM_OP_VIDEO_ON; op.flags = 7;
		(void)kvm_stream_on_ctrl(&s, &op, t0, &reply, NULL);
		kvm_vctl_init(&ctl, 1);
		(void)kvm_vctl_begin_gen(&ctl, &s, 100, 42, 1920, 1088, 1, &m);
	}
	kvm_vrc_init(&rc, KVM_VIDEO_QP, t0);
	kvm_vrc_set_picture(&rc, 1920u * 1088u);
	kvm_budget_init(&budget, 4);
	kvm_sharp_init(&sg, t0);
	for (k = 0; k < NT; k++) { st[k] = T_PEND; lc[k] = t0; }
	// the start of a session: the whole picture goes out as JPEG first (measured: 34 rectangles, 900 KB in the first 10 s), the link is busy with it while the video starts
	{ kvm_stream_note_write(&s, 900000, KVM_W_DATA, t0); }
	gRng = 777u;
	nextIdr = t0 + (uint32_t)c->idrEveryMs;
	stopT = t0 + (uint32_t)c->stopMs;
	nextIter = t0;
	for (t = t0; t < tEnd; t++)
	{
		const int measured = t >= tStart && t < stopT;
		const uint64_t linkBps = c->linkBps;
		// ---- the link ----
		served += (double)linkBps / 1000.0;
		if (served > (double)s.bytesWritten) { served = (double)s.bytesWritten; }
		while (firstUnready < npos && (double)pos[firstUnready].cum <= served + 0.5) { pos[firstUnready].ready = 1; pos[firstUnready].readyAt = t + 6; firstUnready++; }
		if (t - lastAck >= 10)
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
		if ((int32_t)(t - nextIter) < 0) { continue; }
		nextIter = t + 33u + (uint32_t)(rnd01() * 3.0);		// the loop: ~34 ms an iteration (loop_n 280-290 per 10 s)
		// ---- one iteration of the main loop ----
		{
			const uint32_t gnow = t;					// gA.now of kvm_adapt_iteration / kvm_adapt_frame_begin
			const int motion = t < stopT;
			uint32_t tEncEnd = t;
			int encoded = 0;
			(void)kvm_stream_tick(&s, t, 0, NULL);
#ifndef SIM_OLD_RC
			if (idrWatchBytes != 0)
			{
				uint32_t ackAt, ts, qms, mrtt;
				if (kvm_stream_watch_result(&s, &ackAt, &ts, &qms, &mrtt)) { kvm_vrc_idr_acked(&rc, t, idrWatchBytes, ts, ackAt, qms, mrtt); idrWatchBytes = 0; }
			}
#endif
			kvm_vrc_update(&rc, t, 0, kvm_stream_rate_or_mem(&s, t), kvm_stream_link_limited(&s, t));
			(void)kvm_vrc_tick(&rc, t);
			(void)kvm_budget_window(&budget, t);
			if (measured && !measuredStarted) { measuredStarted = 1; up0 = rc.sQpUp; down0 = rc.sQpDown;
#ifndef SIM_OLD_RC
				blocked0 = rc.sQpBlocked;
#endif
				cBucket0 = sg.sBucketRefused; cWin0 = sg.sWindowRefused; cCap0 = sg.sIterCapped;
			}
			// the capture: tiles that changed are claimed by the video (kvm_adapt_note_crc sets lastChange = gA.now, kvm_video_claim_rect makes them PENDING)
			if (motion)
			{
				for (k = 0; k < NT; k++)
				{
					if (k < nBusy || rnd01() < c->quietP) { st[k] = T_PEND; lc[k] = gnow; }
				}
			}
			pend = 0; for (k = 0; k < NT; k++) { if (st[k] == T_PEND) { pend++; } }
			// the video frame
			if (pend > 0)
			{
				kvm_vplan_in in;
				kvm_vplan plan;
				in.now = t; in.pending = (unsigned)pend; in.gateOpen = kvm_stream_window_allows(&s, KVM_GATE_VIDEO, t); in.rateOpen = kvm_vrc_peek(&rc, t);
				in.intervalMs = kvm_pace_interval(&budget, 30, (uint32_t)encAvgUs); in.rate = kvm_stream_rate_or_mem(&s, t);
				kvm_vctl_plan(&ctl, &s, &in, &plan);
				if (plan.held) { if (SIM_NOTE_LINK_HELD(&s, t)) { kvm_vrc_window_refused(&rc); } else { SIM_REFUSED_SOFT(&rc); } }
				if (plan.heldRate) { (void)kvm_vrc_may_encode(&rc, t, 0); }
				if (plan.encode)
				{
					const int idr = ctl.idrFirst || plan.forceIdr || ctl.idrForce;
					const int qp = kvm_vrc_qp(&rc, idr);
					const double dt = lastFrameT ? (double)(t - lastFrameT) : 100.0;
					double b;
					uint32_t bytes, seq;
					(void)kvm_vrc_may_encode(&rc, t, idr);
					if (idr) { b = (idrCount == 0 ? 500000.0 : 175245.0) * pow(0.94, (double)(qp - 30)); idrCount++; }	// the first key frame of a session is the whole screen: 500 KB at QP 30 in the measurement
					else { b = SIM_P_BASE * pow(c->qpSlope, (double)(qp - 26)) * pow(1.0 + dt / 100.0, SIM_P_EXP) * sqrt((double)pend / (double)NT); }
					b *= 0.85 + 0.3 * rnd01();
					bytes = (uint32_t)b;
					tEncEnd = t + encMs;
					{ const int hadFrame = ctl.haveFrame; const uint32_t prevSlot = ctl.lastFrame; (void)kvm_vctl_frame_done(&ctl, idr, bytes, t); ctl.lastFrame = kvm_pace_slot(hadFrame, prevSlot, t, in.intervalMs, NULL); }
					kvm_vrc_frame(&rc, t, bytes, qp, idr, 11000);
					seq = kvm_stream_add_position(&s, t, bytes);
					kvm_stream_note_write(&s, bytes, KVM_W_POSITION, t);
#ifndef SIM_OLD_RC
					if (idr) { kvm_stream_watch(&s, seq); idrWatchBytes = bytes; }
#endif
					pos[npos].seq = seq; pos[npos].ts = t; pos[npos].cum = s.bytesWritten; npos++;
					// tiles_painted(): lastChange = the clock after the encoder
					for (k = 0; k < NT; k++) { if (st[k] == T_PEND) { st[k] = T_VIDEO; lc[k] = tEncEnd; } }
					kvm_sharp_motion(&sg, tEncEnd);
					lastFrameT = t; encoded = 1; frames++;
					if (measured) { vBytes += bytes; res.frames++; }
					if (idr)
					{
						const double lat = (double)(s.bytesWritten - (uint64_t)served) * 1000.0 / (double)linkBps + 12.0;
						if (measured || (t >= tStart && t < tEnd)) { res.idrKB = (res.idrKB * 0 + (double)bytes / 1024.0); res.idrQp = qp; if (idrCount > 1) { idrQpSum += qp; idrQpN++; } if (lat > res.idrLatMax) { res.idrLatMax = lat; } }
					}
					qpSum += qp; qpN++;
				}
			}
			if (nextIdr && (int32_t)(t - nextIdr) >= 0) { kvm_vctl_op3(&ctl, KVM_ACT_IDR); nextIdr += (uint32_t)c->idrEveryMs; }
			if (pend > 0) { kvm_sharp_motion(&sg, t); }
			(void)encoded;
			// ---- the sharpening ----
			{
				const uint32_t snow = gnow;						// gA.now of the start of the iteration (the glue of the sharpening governor reads the clock again after the encoder: a tile painted by the frame is then never later than it; the model keeps the stale clock, the harder case for the signed comparison)
				int64_t sb = 0, normalBytes = 0;
				int pass, anyDue = 0;
				const uint64_t W = kvm_stream_window(&s, tEncEnd);
				nVideo = 0; for (k = 0; k < NT; k++) { if (st[k] == T_VIDEO) { nVideo++; if (SETTLED(snow, lc[k])) { anyDue = 1; } } }
				if (!c->z12)
				{
					// kvm_adapt_sharpen_budget -> sharpen_budget_normal: the gate once per iteration, then 96 KiB
					if (!kvm_stream_window_allows(&s, KVM_GATE_SHARPEN, tEncEnd)) { if (nVideo > 0) { kvm_stream_note_held_back(&s, tEncEnd); } sb = 0; }
					else { sb = 98304; }
				}
				else { sb = kvm_sharp_begin(&sg, tEncEnd, SIM_RATE_SHARP(&s, tEncEnd), W, 98304); }
				normalBytesDbg = sb;
				for (pass = 0; pass < 2; pass++)
				{
					int guard = 0;
					if (pass == 1)
					{
						// video_sharpen_force: tiles due for KVM_VIDEO_SHARPEN_WAIT_MS without a packet of the normal budget (only when the normal budget is nil)
						uint32_t fb = 0;
						if (!anyDue) { dueSince = 0; continue; }
						if (dueSince == 0) { dueSince = t ? t : 1u; continue; }
						if (normalBytes > 0 || (int32_t)(t - dueSince) < (int32_t)KVM_VIDEO_SHARPEN_WAIT_MS) { continue; }
						if (!kvm_vcore_force_sharpen(SIM_RATE_SHARP(&s, t), 0, kvm_stream_inflight(&s), W, forcedHave, lastForced, t, &fb)) { continue; }
						sb = (int64_t)fb; lastForced = t; forcedHave = 1; forcedN++;
					}
					while (sb > 0 && guard++ < 2000)
					{
						int run = 0, idx = -1, n;
						for (n = 0; n < NT; n++) { const int i = (cursor + n) % NT; if (st[i] == T_VIDEO && SETTLED(snow, lc[i])) { idx = i; break; } }
						if (idx < 0) { break; }
						if (c->z12 && pass == 0 && !kvm_sharp_window_ok(&sg, kvm_stream_inflight(&s), W)) { kvm_stream_note_held_back(&s, tEncEnd); break; }
						// a run of up to 8 neighbouring candidates in the row (KVM_ADAPT_SHARPEN_RUN); rows of 40 tiles
						while (run < 8 && idx + run < NT && (idx + run) / 40 == idx / 40 && st[idx + run] == T_VIDEO && SETTLED(snow, lc[idx + run])) { run++; }
						{
							const uint32_t bytes = (uint32_t)((60.0 + 66.0 * run) * (0.9 + 0.2 * rnd01()));
							int active = 0;
							for (k = 0; k < run; k++) { st[idx + k] = T_JPEG; }
							kvm_stream_note_write(&s, bytes, KVM_W_DATA, tEncEnd);
							sb -= bytes; cursor = (idx + run) % NT; packets++;
							shBytes += bytes;
							if (pass == 0) { kvm_sharp_sent(&sg, tEncEnd, bytes); } else { kvm_sharp_forced(&sg, tEncEnd, bytes); }
							active = kvm_sharp_active(&sg, tEncEnd);
#ifndef SIM_OLD_RC
							kvm_vrc_note_sharpen(&rc, tEncEnd, bytes, active);
#endif
							if (pass == 0) { normalBytes += bytes; dueSince = 0; }
							if (measured) { bytesQ += bytes; if (active) { shMotion += bytes; } }
						}
					}
				}
			}
			// rule M1: a position after an iteration that wrote bytes
			{
				int probe = 0;
				if (kvm_stream_marker_wanted(&s, tEncEnd, 0, &probe) && npos < 299000)
				{
					uint8_t mk[KVM_MARKER_BYTES];
					kvm_stream_make_marker(&s, tEncEnd, probe, mk);
					kvm_stream_note_write(&s, KVM_MARKER_BYTES, KVM_W_POSITION, tEncEnd);
					pos[npos].seq = s.seqCounter; pos[npos].ts = tEncEnd; pos[npos].cum = s.bytesWritten; npos++;
				}
			}
			// ---- measurements ----
			if (measured)
			{
				const double lat = (double)(s.bytesWritten - (uint64_t)served) * 1000.0 / (double)linkBps + 12.0;
				if (nLat < 200000) { gLat[nLat++] = (float)lat; }
				rSum += (double)rc.rate; rN++;
				if ((double)rc.qp > res.qpMax) { res.qpMax = rc.qp; }
			}
			if (!stopped && t >= stopT) { stopped = 1; settleB0 = s.bytesWritten; }
			if (stopped && !settled)
			{
				int left = 0; for (k = 0; k < NT; k++) { if (st[k] != T_JPEG) { left++; } }
				if (left == 0) { settled = 1; res.settleMs = (double)(t - stopT) + (double)(s.bytesWritten - (uint64_t)served) * 1000.0 / (double)linkBps; res.settleKB = (double)(s.bytesWritten - settleB0) / 1024.0; }
			}
			if (c->verbose > 1 && t >= stopT && t < stopT + 3000u) { printf("   +%ums sb_normal=%lld video_tiles=%d pend=%d inflight=%llu W=%llu\n", (unsigned)(t - stopT), (long long)normalBytesDbg, nVideo, pend, (unsigned long long)kvm_stream_inflight(&s), (unsigned long long)kvm_stream_window(&s, t)); }
			if (c->verbose && ((t - t0) % 5000u) < 34) { printf("   t=%2us qp=%d R=%5.0f kb/s tokens=%6lld inflight=%7llu W=%7llu video_tiles=%d\n", (unsigned)((t - t0) / 1000u), rc.qp, (double)rc.rate * 8 / 1000, (long long)rc.tokens, (unsigned long long)kvm_stream_inflight(&s), (unsigned long long)kvm_stream_window(&s, t), nVideo); }
		}
	}
	{
		const double sec = (double)(stopT - tStart) / 1000.0;
		res.fps = (double)res.frames / sec;
		res.videoKBs = (double)vBytes / 1024.0 / sec;
		res.sharpKBs = (double)bytesQ / 1024.0 / sec;
		res.share = (double)bytesQ / (double)(bytesQ + vBytes + 1);
		res.sharpMotionKBs = (double)shMotion / 1024.0 / sec;
		res.qpAvg = qpN ? qpSum / (double)qpN : 0.0;
		res.rAvg = rN ? rSum / (double)rN * 8.0 / 1000.0 : 0.0;
		if (nLat > 0) { double sum = 0; int i; for (i = 0; i < nLat; i++) { sum += gLat[i]; } res.latAvg = sum / nLat; qsort(gLat, (size_t)nLat, sizeof(float), cmpf); res.latP95 = gLat[(int)(nLat * 0.95)]; }
		res.qpUp = rc.sQpUp - up0; res.qpDown = rc.sQpDown - down0; (void)blocked0;
#ifndef SIM_OLD_RC
		res.qpBlocked = rc.sQpBlocked - blocked0;
#endif
		{
		res.bucketRefusedPerS = (double)(sg.sBucketRefused - cBucket0) / sec; res.windowRefusedPerS = (double)(sg.sWindowRefused - cWin0) / sec; res.iterCappedPerS = (double)(sg.sIterCapped - cCap0) / sec;
		}
		res.sharpPackets = packets; res.forced = forcedN; res.idrQpAvg = idrQpN > 0 ? idrQpSum / idrQpN : res.idrQp;
	}
	return res;
}

static simcfg base_cfg(const char *name, uint64_t bps, int z0, int z12)
{
	simcfg c;
	memset(&c, 0, sizeof(c));
	c.name = name; c.linkBps = bps; c.z0 = z0; c.z12 = z12;
	c.qpSlope = 0.89; c.quietFrac = 0.06; c.quietP = 0.30; c.stopMs = 55000; c.seconds = 70; c.idrEveryMs = 10000;
	return c;
}

static void head(void)
{
	printf("| step | link | fps | video KB/s | sharpening KB/s | sharpening share | of it inside the movement KB/s | QP avg / max | QP up / down | IDR KB / QP (last, mean of the later ones) / queue ms | latency avg / P95 ms | settle after the stop ms (KB) |\n|---|---|---|---|---|---|---|---|---|---|---|---|\n");
}

static void row(const char *step, const char *link, const simres *r)
{
	printf("| %s | %s | %.1f | %.0f | %.0f | %.0f %% | %.0f | %.1f / %.0f | %u / %u | %.0f / %.0f (%.1f) / %.0f | %.0f / %.0f | %.0f (%.0f) |\n", step, link, r->fps, r->videoKBs, r->sharpKBs, r->share * 100.0, r->sharpMotionKBs, r->qpAvg, r->qpMax, r->qpUp, r->qpDown, r->idrKB, r->idrQp, r->idrQpAvg, r->idrLatMax, r->latAvg, r->latP95, r->settleMs, r->settleKB);
}

static void rows_for(const char *title, double slope, double quietFrac, double quietP, int firstStep, int verbose, simres r[4][3])
{
	static const struct { const char *name; uint64_t bps; } links[] = { { "1 Mb/s", 125000 }, { "3 Mb/s", 375000 }, { "5 Mb/s", 625000 }, { "LAN 50 Mb/s", 6250000 } };
	static const char *const nm[] = { "0: earlier rate control", "signed settle", "governor" };
#ifdef SIM_OLD_RC
	const char *rcName = "rc";
#else
	const char *rcName = "rc";
#endif
	size_t li;
	int step;
	printf("\n%s\n\n", title);
	head();
	for (li = 0; li < 4; li++)
	{
		for (step = firstStep; step < 3; step++)
		{
			simcfg c = base_cfg(links[li].name, links[li].bps, step >= 1, step >= 2);
			char lab[96];
			c.qpSlope = slope; c.quietFrac = quietFrac; c.quietP = quietP; c.verbose = verbose;
			r[li][step] = simulate(&c);
			snprintf(lab, sizeof(lab), "%s, %s", nm[step], rcName);
			row(lab, links[li].name, &r[li][step]);
		}
	}
}

int main(int argc, char **argv)
{
	simres a[4][3], b[4][3], q[4][3];
	size_t li;
	int verbose = argc > 1 && strcmp(argv[1], "-v") == 0 ? 1 : (argc > 1 && strcmp(argv[1], "-vv") == 0 ? 2 : 0);
#ifdef SIM_OLD_RC
	const int first = 0;
#else
	const int first = 2;		// (the steps before the governor with the new rate control would mix the legacy flood with the new accounting: not a real configuration)
#endif
	memset(a, 0, sizeof(a)); memset(b, 0, sizeof(b)); memset(q, 0, sizeof(q));
	printf("simulation build: %s\n", first == 0 ? "the earlier files (rate control before the sharpening governor) + the governor of the sharpening" : "the tree");
	rows_for("Table A: the encoder listens to the QP (-11 % per step, Windows 10); the gradient of the measurement", 0.89, 0.06, 0.30, first, verbose, a);
	rows_for("Table B: the encoder does not listen to the QP (Windows 11 build 26200: the size of a P frame is the same at every QP)", 1.0, 0.06, 0.30, first, verbose, b);
	rows_for("Table C (stress): half of the tiles of the video are quiet for stretches (a picture with large still parts), the encoder listens", 0.89, 0.50, 0.12, first == 0 ? 1 : 1, verbose, q);
	printf("\n");
	// ---- the checks ----
#ifdef SIM_OLD_RC
	for (li = 0; li < 3; li++)
	{
		// reproduction of the measurement by the model of: fps 2.0 / 12.7 / 23.9 (within -35 % .. +60 % (1 Mb/s: the model gives 3.1, the measurement 2.0): a model of ~25 parameters), the sharpening takes 55-78 % of the bytes, the QP of 3 and 5 Mb/s on the floor
		static const double fpsM2[3] = { 2.0, 12.7, 23.9 };
		CHECK(a[li][0].fps >= fpsM2[li] * 0.65 && a[li][0].fps <= fpsM2[li] * 1.60);
		CHECK(a[li][0].share >= 0.55 && a[li][0].share <= 0.78);
		CHECK(a[li][0].sharpMotionKBs >= 0.95 * a[li][0].sharpKBs);		// all of it inside the movement
		if (li > 0) { CHECK(a[li][0].qpAvg <= 26.6); }
		// alone: the sharpening in the movement disappears
		CHECK(a[li][1].share <= 0.15);
		CHECK(a[li][1].fps >= a[li][0].fps);
		CHECK(b[li][1].share <= 0.15);
	}
#else
	for (li = 0; li < 3; li++)
	{
		// the targets of 6.2: at least 10 fps at 1 Mb/s and 22 at 3 Mb/s, the sharpening in the movement at most 25 % of the bytes, the picture settles in a second
		static const double fpsMin[3] = { 10.0, 22.0, 22.0 };
		CHECK(a[li][2].fps >= fpsMin[li]);
		CHECK(a[li][2].share <= 0.25);
		CHECK(a[li][2].settleMs <= 1000.0);
		CHECK(b[li][2].share <= 0.25);
		CHECK(a[li][2].qpUp + a[li][2].qpDown <= 16);					// the QP does not swing with the link (40 s measured: a step every 3 s at most)
		CHECK(b[li][2].qpUp + b[li][2].qpDown <= 16);
		CHECK(b[li][2].fps >= (li == 0 ? 8.0 : 15.0));
		CHECK(q[li][2].sharpMotionKBs <= 0.25 * (q[li][2].videoKBs + q[li][2].sharpMotionKBs) / 0.75 + 1.0);
	}
	// the key frame is sized from what the last key frame was seen to take. 1 Mb/s: the queue of the key frame stays what it was before the confirmation rule (940 ms,
	//); the LAN: the key frame has the QP of the P frames or close to it
	CHECK(a[0][2].idrLatMax <= 960.0);
	CHECK(a[0][2].idrQpAvg >= 36.0);
	CHECK(a[3][2].idrQpAvg <= 30.0);
	CHECK(a[3][2].idrLatMax <= 200.0);
#endif
	printf("%d checks, %d failed\n", gChecks, gFail);
	return gFail == 0 ? 0 : 1;
}
