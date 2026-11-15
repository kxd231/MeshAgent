/*
the app-limited samples (the principle of app-limited samples; the 36 scenes): a session on a LAN (100 Mb/s): the first picture, 10 s of continuous video (its key frame is
acknowledged), then an idle phase of 3, 30 or 300 s with a trickle (a cursor and a caret: 0, 5 or 8 KB of data and a position every 850 ms, the ACK 600 ms late), then the content comes back with
(1) a new generation on the reused encoder (op 2 -> op 1), (2) a new generation with a fresh encoder (the rate control starts again), (3) op 3 (a key frame in the same generation: the recording, every 30 s),
(4) no key frame: a JPEG of 300 KB at once. In 1 - 3 a JPEG of 100 KB follows the ACK of the new key frame (the scene E). Measured from the new key frame (4: from the end of the idle phase): the first P
frame, the P frames in 3 s / 10 s, R at +0.5 / +3 / +8 s, the refusals of the window. The reference is the start of a session without a trickle: 83 / 293 frames (scene E of kvm_start_sim_test.c).

Built by run-kvm-idle-sim-tests.sh against the files of and the tree; the checks (-DSIM_CHECK) are for the tree. Numbers of are in kIdleRef (the tree must give at least
as many frames in 3 s and in 10 s).
*/
#include "../kvm_video_rc.h"
#include "../kvm_video_core.h"
#include "../kvm_pace.h"
#include "../kvm_stream.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SIM_SOFT
#define SIM_NOTE_LINK_HELD(s, t) kvm_stream_note_link_held((s), (t))
#define SIM_REFUSED_SOFT(rc) kvm_vrc_window_refused_soft(rc)
#else
#define SIM_NOTE_LINK_HELD(s, t) (kvm_stream_note_link_held((s), (t)), 1)
#define SIM_REFUSED_SOFT(rc) kvm_vrc_window_refused(rc)
#endif

#ifdef SIM_CHECK
static int gFail = 0, gChecks = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#endif

typedef struct pos_t { uint32_t seq, ts; uint64_t cum; int ready; uint32_t readyAt; uint32_t extraMs; } pos_t;
static pos_t pos[400000];

typedef struct idle_res { int firstP; unsigned f3, f10, held; double r[3]; } idle_res;

static idle_res run(const char *label, double linkBps, uint32_t trickleData, uint32_t idleMs, int mode, int verbose)
{
	const int em = mode >= 5 ? (mode == 5 ? 1 : 4) : mode;	/* 5: op 1 with a key frame, 6: no key frame, both on a slow link (5 Mb/s) with heavy content: the window holds the sender back, the memory is a window-limited sample */
	const int heavy = mode >= 5;
	kvm_stream s; kvm_vctl ctl; kvm_vrc rc;
	size_t npos = 0, firstUnready = 0, firstUndelivered = 0;
	double served = 0.0, srcAcc = 0.0;
	const uint32_t t0 = 5000, videoAt = 3900u, run1 = 10000u;
	const uint32_t epochRel = videoAt + run1 + idleMs, tEnd = t0 + epochRel + 12000u;
	uint32_t t, lastAck = 0, nextIter = t0 + 100u, lastFrameT = 0, nextTrickle = 0, tKey = 0, nextLog = 0, idrBytes = 0;
	int pendFlag = 0, vidOn = 0, picDone = 0, epochDone = 0, keyAfter = 0, jpegDue = 0, jpegDone = 0, firstP = -1, bigLeft = 0;
	unsigned f3 = 0, f10 = 0, heldAfter = 0;
	double rAt[3] = { -1, -1, -1 };
	double encAvgUs = 8000.0;
	{
		kvm_ctrl op; kvm_state_msg reply;
		kvm_stream_init(&s, 1, 1, 1, 6);
		memset(&op, 0, sizeof(op)); op.ver = 1; op.op = KVM_OP_VIDEO_ON; op.flags = 7;
		(void)kvm_stream_on_ctrl(&s, &op, t0, &reply, NULL);
		kvm_vctl_init(&ctl, 1);
	}
	memset(pos, 0, sizeof(pos));	/* (the array is shared by the scenes: a position that is written without the delay of the viewer (extraMs) must not inherit the 600 ms of a trickle of the scene before; the first version of this simulation did and found starvation that was its own) */
	kvm_vrc_init(&rc, KVM_VIDEO_QP, t0);
	kvm_vrc_set_picture(&rc, 1920u * 1088u);
	for (t = t0; t < tEnd; t++)
	{
		const uint32_t rel = t - t0;
		const int idle = rel >= videoAt + run1 && rel < epochRel;
		const int after = rel >= epochRel;
		served += linkBps / 1000.0;
		if (served > (double)s.bytesWritten) { served = (double)s.bytesWritten; }
		while (firstUnready < npos && (double)pos[firstUnready].cum <= served + 0.5) { pos[firstUnready].ready = 1; pos[firstUnready].readyAt = t + 20u + pos[firstUnready].extraMs; firstUnready++; }
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
		if (!picDone && rel >= 100u)
		{
			uint8_t mk[KVM_MARKER_BYTES]; int probe = 0;
			picDone = 1;
			kvm_stream_note_write(&s, 574366u, KVM_W_DATA, t);
			if (kvm_stream_marker_wanted(&s, t, 0, &probe)) { kvm_stream_make_marker(&s, t, probe, mk); kvm_stream_note_write(&s, KVM_MARKER_BYTES, KVM_W_POSITION, t); pos[npos].seq = s.seqCounter; pos[npos].ts = t; pos[npos].cum = s.bytesWritten; npos++; }
		}
		if (idle && trickleData > 0 && (int32_t)(t - nextTrickle) >= 0)
		{
			uint8_t mk[KVM_MARKER_BYTES]; uint32_t seq;
			kvm_stream_note_write(&s, trickleData, KVM_W_DATA, t);
			seq = kvm_stream_add_position(&s, t, KVM_MARKER_BYTES);
			kvm_stream_build_marker(mk, seq, t, s.gen);
			kvm_stream_note_write(&s, KVM_MARKER_BYTES, KVM_W_POSITION, t);
			pos[npos].seq = seq; pos[npos].ts = t; pos[npos].cum = s.bytesWritten; pos[npos].extraMs = 600u; npos++;
			nextTrickle = t + 850u;
		}
		srcAcc += 1000.0;
		if (srcAcc >= 33333.0) { srcAcc -= 33333.0; if (rel + 100u >= videoAt && !idle) { pendFlag = 1; } }
		if ((int32_t)(t - nextIter) < 0) { continue; }
		{
			uint32_t next = t + 16u;
			(void)kvm_stream_tick(&s, t, 0, NULL);
			if (rel + 100u >= videoAt && !vidOn) { kvm_state_msg mm; (void)kvm_vctl_begin_gen(&ctl, &s, 100, 42, 1920, 1088, 1, &mm); vidOn = 1; }
			if (after && !epochDone)
			{
				epochDone = 1;
				if (em == 1 || em == 2) { kvm_state_msg mm; (void)kvm_vctl_begin_gen(&ctl, &s, 100, 42, 1920, 1088, em == 2, &mm); if (em == 2) { kvm_vrc_init(&rc, rc.qp, t); kvm_vrc_set_picture(&rc, 1920u * 1088u); } }
				else if (em == 3) { kvm_vctl_op3(&ctl, KVM_ACT_IDR); }
				else { bigLeft = 300000; }
			}
			if (vidOn)
			{
				kvm_vrc_update(&rc, t, 0, kvm_stream_rate_or_mem(&s, t), kvm_stream_link_limited(&s, t));
				(void)kvm_vrc_tick(&rc, t);
				{
					uint32_t ackAt = 0, ts = 0, queueMs = 0, minRtt = 0;
					if (kvm_stream_watch_result(&s, &ackAt, &ts, &queueMs, &minRtt))
					{
						kvm_vrc_idr_acked(&rc, t, idrBytes, ts, ackAt, queueMs, minRtt);
						if (after && keyAfter && !jpegDone && em != 4) { jpegDue = 1; }
					}
				}
				if (jpegDue)
				{
					uint8_t mk[KVM_MARKER_BYTES]; int pr = 0;
					jpegDue = 0; jpegDone = 1;
					kvm_stream_note_write(&s, 100000u, KVM_W_DATA, t);
					if (kvm_stream_marker_wanted(&s, t, 0, &pr)) { kvm_stream_make_marker(&s, t, pr, mk); kvm_stream_note_write(&s, KVM_MARKER_BYTES, KVM_W_POSITION, t); pos[npos].seq = s.seqCounter; pos[npos].ts = t; pos[npos].cum = s.bytesWritten; npos++; }
				}
				if (bigLeft > 0)
				{
					if (kvm_stream_window_allows(&s, KVM_GATE_BIG, t))
					{
						uint8_t mk[KVM_MARKER_BYTES]; int pr = 0;
						kvm_stream_note_write(&s, (size_t)bigLeft, KVM_W_DATA, t); bigLeft = 0;
						if (kvm_stream_marker_wanted(&s, t, 0, &pr)) { kvm_stream_make_marker(&s, t, pr, mk); kvm_stream_note_write(&s, KVM_MARKER_BYTES, KVM_W_POSITION, t); pos[npos].seq = s.seqCounter; pos[npos].ts = t; pos[npos].cum = s.bytesWritten; npos++; }
					}
					else { kvm_stream_note_link_held(&s, t); }
				}
				if (pendFlag)
				{
					kvm_vplan_in in; kvm_vplan plan;
					in.now = t; in.pending = 872u; in.gateOpen = kvm_stream_window_allows(&s, KVM_GATE_VIDEO, t); in.rateOpen = kvm_vrc_peek(&rc, t);
					in.intervalMs = 16u; in.rate = kvm_stream_rate_or_mem(&s, t);
					kvm_vctl_plan(&ctl, &s, &in, &plan);
					if (plan.held) { if (SIM_NOTE_LINK_HELD(&s, t)) { kvm_vrc_window_refused(&rc); } else { SIM_REFUSED_SOFT(&rc); } if (after) { heldAfter++; } }
					if (plan.heldRate) { (void)kvm_vrc_may_encode(&rc, t, 0); }
					if (plan.encode)
					{
						const int idr = ctl.idrFirst || plan.forceIdr || ctl.idrForce;
						const int qp = kvm_vrc_qp(&rc, idr);
						const double dt = lastFrameT ? (double)(t - lastFrameT) : 100.0;
						double b; uint32_t bytes, seq, tEncEnd = t + (idr ? 50u : 10u);
						(void)kvm_vrc_may_encode(&rc, t, idr);
						if (idr) { b = 228.0 * 1024.0 / pow(0.94, 8.0) * pow(0.94, (double)(qp - 30)); }
						else { b = (heavy ? 30.0 : 7.0) * 872.0 * pow(0.89, (double)(qp - 26)) * (1.0 + 0.25 * (dt > 16.7 ? ((dt > 100.0 ? 100.0 : dt) / 16.7 - 1.0) : 0.0)); }
						bytes = (uint32_t)b;
						{ const int hadFrame = ctl.haveFrame; const uint32_t prevSlot = ctl.lastFrame; (void)kvm_vctl_frame_done(&ctl, idr, bytes, t); ctl.lastFrame = kvm_pace_slot(hadFrame, prevSlot, t, 16u, NULL); }
						kvm_vctl_note_write(&ctl, tEncEnd, (uint32_t)((encAvgUs + 500.0) / 1000.0));
						encAvgUs = encAvgUs * 0.875 + (idr ? 50000.0 : 10000.0) * 0.125;
						if (idr) { idrBytes = bytes; if (after && !keyAfter) { keyAfter = 1; tKey = t; } }
						kvm_vrc_frame(&rc, t, bytes, qp, idr, (idr ? 50000u : 10000u));
						seq = kvm_stream_add_position(&s, t, bytes);
						kvm_stream_note_write(&s, bytes, KVM_W_POSITION, t);
						if (idr) { kvm_stream_watch(&s, seq); }
						pos[npos].seq = seq; pos[npos].ts = t; pos[npos].cum = s.bytesWritten; npos++;
						lastFrameT = t; pendFlag = 0;
						if (after && !idr)
						{
							const uint32_t base = (em == 4) ? t0 + epochRel : tKey;
							if ((em == 4 || keyAfter))
							{
								if (firstP < 0) { firstP = (int)(t - base); }
								if (t - base < 3000u) { f3++; }
								if (t - base < 10000u) { f10++; }
							}
						}
						next = tEncEnd;
					}
				}
				{
					uint8_t mk[KVM_MARKER_BYTES]; int probe = 0;
					if (kvm_stream_marker_wanted(&s, t, 0, &probe) && npos < 399000) { kvm_stream_make_marker(&s, t, probe, mk); kvm_stream_note_write(&s, KVM_MARKER_BYTES, KVM_W_POSITION, t); pos[npos].seq = s.seqCounter; pos[npos].ts = t; pos[npos].cum = s.bytesWritten; npos++; }
				}
				if (after)
				{
					const uint32_t base = (em == 4) ? t0 + epochRel : (keyAfter ? tKey : 0u);
					static const uint32_t at[3] = { 500, 3000, 8000 };
					int k;
					if (base != 0) { for (k = 0; k < 3; k++) { if (t - base >= at[k] && rAt[k] < 0) { rAt[k] = (double)rc.rate * 8.0 / 1000.0; } } }
					if (verbose && (int32_t)(t - nextLog) >= 0)
					{
						nextLog = t + 250u;
						printf("   +%6u ms: R=%6.0f cap=%8llu floor=%8llu rate=%8llu mem=%8llu W=%7u lim=%d qp=%d held=%u f3=%u\n", (unsigned)(rel - epochRel), (double)rc.rate * 8.0 / 1000.0, (unsigned long long)rc.cap, (unsigned long long)rc.capFloor,
							(unsigned long long)kvm_stream_rate(&s, t), (unsigned long long)kvm_stream_rate_or_mem(&s, t), (unsigned)kvm_stream_window(&s, t), kvm_stream_link_limited(&s, t), rc.qp, heldAfter, f3);
					}
				}
			}
			nextIter = next > t ? next : t + 1u;
		}
	}
	printf("| %s | %d | %u / %u | %.0f / %.0f / %.0f | %u |\n", label, firstP, f3, f10, rAt[0], rAt[1], rAt[2], heldAfter);
	{ idle_res o; o.firstP = firstP; o.f3 = f3; o.f10 = f10; o.held = heldAfter; o.r[0] = rAt[0]; o.r[1] = rAt[1]; o.r[2] = rAt[2]; return o; }
}

int main(int argc, char **argv)
{
	static const char *mn[5] = { "", "op 1 (new gen, reused encoder)", "new gen, fresh encoder", "op 3 (IDR, same gen)", "no IDR, JPEG 300 KB" };
	static const uint32_t tr[3] = { 0u, 5000u, 8000u };
	static const uint32_t idl[3] = { 3000u, 30000u, 300000u };
	const char *only = argc > 1 ? argv[1] : NULL;
	int m, k, i;
	static idle_res res[36], res5[4];
#ifdef SIM_BUILD
	printf("build: %s\n", SIM_BUILD);
#endif
	printf("| scene (LAN 100 Mb/s; 10 s video, idle with a trickle, then) | first P ms | P frames 3 s / 10 s | R +0.5 / +3 / +8 s kb/s | window refusals |\n|---|---|---|---|---|\n");
	for (m = 1; m <= 4; m++)
		for (k = 0; k < 3; k++)
			for (i = 0; i < 3; i++)
			{
				char lab[160];
				snprintf(lab, sizeof(lab), "M%d-%u-%u %s, trickle %u B, idle %u s", m, (unsigned)tr[k], (unsigned)(idl[i] / 1000u), mn[m], (unsigned)tr[k], (unsigned)(idl[i] / 1000u));
				if (only && strncmp(lab, only, strlen(only)) != 0) { continue; }
				res[(m - 1) * 9 + k * 3 + i] = run(lab, 12500000.0, tr[k], idl[i], m, only != NULL && argc > 2);
			}
	// a slow link (5 Mb/s) and heavy content (26 KB a frame): the window holds the sender back for 10 s, so the memory is a window-limited sample (evidence of the link);
	// after 300 s of idle it has decayed to a few B/s and, held no more, is no evidence: a trickle (0 or 5 KB) and the new key frame (op 1 with a JPEG of 100 KB behind its ACK) or none (a JPEG of 300 KB)
	for (m = 5; m <= 6; m++)
		for (k = 0; k < 2; k++)
		{
			char lab[160];
			snprintf(lab, sizeof(lab), "M%d-%u-300 %s, 5 Mb/s, heavy, trickle %u B, idle 300 s", m, (unsigned)tr[k], m == 5 ? "op 1 with a key frame" : "no key frame, JPEG 300 KB", (unsigned)tr[k]);
			if (only && strncmp(lab, only, strlen(only)) != 0) { continue; }
			res5[(m - 5) * 2 + k] = run(lab, 625000.0, tr[k], 300000u, m, only != NULL && argc > 2);
		}
	(void)res; (void)res5;
#ifdef SIM_CHECK
	if (!only)
	{
		// every scene: as many frames as the start of a session without a trickle (83 / 293, scene E) within 3 %, and R not at its floor. The files of and give 1 / 104 frames and
		// R 100 kb/s after 300 s of idle without a trickle (the remembered rate expired, the first refusal behind the key frame took R from a rate that was no evidence of the link), 21 - 63 frames in 3 s after
		// 3 - 30 s (a fresh encoder), 46 / 256 after 30 s (op 1 and op 3): the estimate of the idle phase was the cursor and the caret, or nothing
		for (m = 0; m < 36; m++)
		{
			CHECK(res[m].f3 >= 80u && res[m].f10 >= 290u);
			CHECK(res[m].r[0] >= 1500.0 && res[m].r[1] >= 1500.0);
			CHECK(res[m].firstP >= 0 && res[m].firstP <= 500);
		}
		for (m = 0; m < 4; m++)
		{
			CHECK(res5[m].f3 >= 70u && res5[m].f10 >= 280u);	// (earlier versions: 2 / 27 without a trickle, 53 / 263 with it and without a key frame)
			CHECK(res5[m].r[0] >= 1500.0 && res5[m].r[1] >= 1500.0);
		}
	}
	printf("%d checks, %d failed\n", gChecks, gFail);
	return gFail == 0 ? 0 : 1;
#else
	return 0;
#endif
}
