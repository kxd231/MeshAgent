#!/bin/sh
# Mutation check of the tests of kvm_video_core.c and of the frame builder of kvm_stream.c: each line below changes one key rule of the code in a copy, and the tests have to turn
# red (the test program exits with a failure); a mutation that does not compile is reported as invalid, one that the tests do not notice as SURVIVED (exit status 1 at the end).
# Usage: run-kvm-video-mutations.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
COMMON=$(dirname "$HERE")
IMG=kvm-ctest:local
if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    docker build -q -t "$IMG" - <<'DOCKERFILE'
FROM debian:bookworm-slim
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends gcc libc6-dev make >/dev/null && rm -rf /var/lib/apt/lists/*
DOCKERFILE
fi
exec docker run --rm --user "$(id -u):$(id -g)" -v "$COMMON:/src:ro" -w /tmp "$IMG" sh -c '
set -u
cat > /tmp/mutations.txt <<"EOT"
kvm_video_core.c @@ while (n > KVM_FRAME_MAX_RECTS) @@ while (n > KVM_FRAME_MAX_RECTS + 1) @@ more than 256 rectangles are allowed
kvm_video_core.c @@ cover[(size_t)r * (size_t)t->cols + (size_t)c] = 1; } @@ ; } @@ the cover is not painted
kvm_video_core.c @@ if (x1 > screenW) { x1 = screenW; } @@ ; @@ a rectangle reaches beyond the screen
kvm_video_core.c @@ set_state(t, (size_t)row * (size_t)t->cols + (size_t)col, KVM_VT_NONE); @@ ; @@ a JPEG tile stays pending
kvm_video_core.c @@ if (t->st[i] == KVM_VT_PENDING && t->fl[i] == 0) { map[i] = 1; count++; set_state(t, i, KVM_VT_NONE); } @@ if (t->st[i] == KVM_VT_PENDING && t->fl[i] == 0) { map[i] = 1; count++; } @@ video off leaves tiles pending
kvm_video_core.c @@ if (!cover[i]) { continue; } @@ ; @@ a frame paints tiles outside its cover
kvm_video_core.c @@ if (t > sp) { sp = t; } @@ ; @@ the IDR spacing ignores the size of the IDR
kvm_video_core.c @@ if (sp > KVM_VIDEO_IDR_MAX_MS) { sp = KVM_VIDEO_IDR_MAX_MS; } @@ ; @@ the IDR spacing is not capped
kvm_video_core.c @@ if (c->idrFirst && !isIdr) @@ if (0 && c->idrFirst && !isIdr) @@ the first frame of a generation may be a P frame
kvm_video_core.c @@ kvm_stream_bump_gen(s); @@ ; @@ a new generation does not change the generation
kvm_video_core.c @@ dms(now, c->lastOp1) < (int32_t)KVM_VIDEO_OP1_COALESCE_MS @@ dms(now, c->lastOp1) <= (int32_t)KVM_VIDEO_OP1_COALESCE_MS @@ op 1 is joined for one ms too long
kvm_video_core.c @@ if (c->errorCount >= KVM_VIDEO_MAX_ERRORS) @@ if (c->errorCount > KVM_VIDEO_MAX_ERRORS) @@ a fourth error is needed to stop the video
kvm_video_core.c @@ if (c->haveRepair && dms(now, c->lastRepair) < (int32_t)KVM_VIDEO_REPAIR_MIN_MS) { return 0; } @@ ; @@ repair is not limited to once a second
kvm_video_core.c @@ i = level_index(mbs, KVM_VIDEO_MAX_FPS); @@ i = level_index(mbs, 30); @@ the level is computed for 30 frames per second
kvm_video_core.c @@ if (qp < KVM_VIDEO_QP_MIN) { return KVM_VIDEO_QP_MIN; } @@ ; @@ the quantizer may go below the floor
kvm_video_core.c @@ if (in->pending == 0) { return; } @@ ; @@ a frame is planned without motion
kvm_video_core.c @@ if (!in->gateOpen) { out->held = 1; return; } @@ ; @@ a frame goes into a closed window
kvm_video_core.c @@ return !c->haveOpen || dms(now, c->lastOpen) >= (int32_t)KVM_VIDEO_OPEN_MIN_MS; @@ return 1; @@ the encoder is created more than once in 2 s
kvm_video_core.c @@ if (c->idrForce) { out->forceIdr = 1; } @@ if (c->idrForce) { } @@ a lost access unit is not followed by an IDR
kvm_video_core.c @@ else if (c->idrReq && (!c->haveIdr @@ else if (c->idrReq && (1 || !c->haveIdr @@ an IDR that was asked for is not delayed
kvm_video_core.c @@ c->idrFirst = c->idrForce = c->idrReq = c->encFresh = 0;\n	}\n	return 0; @@ c->idrForce = c->idrReq = c->encFresh = 0;\n	}\n	return 0; @@ the first IDR of a generation does not end the wait for it
kvm_stream.c @@ wr32(out + 4, (uint32_t)n); @@ wr32(out + 4, (uint32_t)n + 1u); @@ N of the 243 is wrong
kvm_stream.c @@ if (n > KVM_FRAME_MAX_N) { return 0; } @@ if (n > KVM_FRAME_MAX_N + 1u) { return 0; } @@ a frame of more than 8 MiB is built
kvm_stream.c @@ if (out == NULL || p->nRects > KVM_FRAME_MAX_RECTS) { return 0; } @@ if (out == NULL || p->nRects > KVM_FRAME_MAX_RECTS + 1) { return 0; } @@ a frame of 257 rectangles is built
kvm_video_core.c @@ if (s->profile == 66 && (s->constraint & 0x40) == 0) { return KVM_REASON_PROFILE; } @@ ; @@ Baseline without constraint_set1 is accepted
kvm_video_core.c @@ if (s->numRefFrames > 1 || @@ if (0 || @@ more than one reference frame is accepted
kvm_video_core.c @@ if (s->level > maxL || s->level > 52 || s->level < 10) { return KVM_REASON_RESOLUTION; } @@ ; @@ a level above the viewer limit is accepted
kvm_video_core.c @@ if (s->cropping || @@ if (0 || @@ frame cropping is accepted
kvm_video_core.c @@ if (window > 0 && inflight >= 2u * window) { return 0; } @@ ; @@ sharpening is forced into a window that is more than twice full
kvm_video_core.c @@ if (budgetKbps > 0 && (uint64_t)budgetKbps * 125u < rate) { rate = (uint64_t)budgetKbps * 125u; } @@ ; @@ the forced sharpening ignores the viewer budget
kvm_video_core.c @@ interval = share > 0 ? (b * 1000u) / share : 1000u; @@ interval = KVM_VIDEO_SHARPEN_FORCE_MS; @@ the forced sharpening ignores the share of the rate
kvm_video_core.c @@ if (c->haveOp1 && c->state != KVM_VS_OFF && @@ if (c->haveOp1 && @@ op 1 is joined with a start that failed
kvm_video_core.c @@ c->nxFlags = (uint8_t)(op->flags & 7); c->nxLevel = op->maxLevelIdc; c->nxW = op->maxW; c->nxH = op->maxH; c->nxValid = 1; @@ c->nxValid = 0; @@ the limits of a joined op 1 are forgotten
kvm_video_core.c @@ c->profFlags = c->nxFlags; c->maxLevel = c->nxLevel; c->maxW = c->nxW; c->maxH = c->nxH; @@ ; @@ the limits of a joined op 1 are never applied
kvm_video_core.c @@ if (down) { return 0; } @@ ; @@ the scale ladder may step down while the video runs
kvm_video_core.c @@ return msSinceGen >= (uint32_t)KVM_VIDEO_SCALE_RETURN_MS; @@ return 1; @@ the scale goes back at once after a generation began
kvm_video_core.c @@ if (c->idrForce) { return KVM_IDR_WHY_BROKEN; } @@ ; @@ a key frame after a lost access unit is not named
kvm_video_core.c @@ if (c->idrFirst) { return KVM_IDR_WHY_FIRST; } @@ ; @@ the first key frame of a generation is not named
kvm_video_core.c @@ if (plan != NULL && plan->forceIdr && c->idrReq) { return KVM_IDR_WHY_OP3; } @@ if (plan != NULL && plan->forceIdr) { return KVM_IDR_WHY_OP3; } @@ a forced key frame nobody asked for with op 3 is called op3
kvm_video_core.c @@ if (plan != NULL && plan->forceIdr && c->idrReq) { return KVM_IDR_WHY_OP3; } @@ if (c->idrReq) { return KVM_IDR_WHY_OP3; } @@ an op 3 that waits for the spacing is called a reason of a frame that is no key frame
kvm_video_core.c @@ if (c->idrForce) { return KVM_IDR_WHY_BROKEN; }\n	if (c->idrFirst) { return KVM_IDR_WHY_FIRST; } @@ if (c->idrFirst) { return KVM_IDR_WHY_FIRST; }\n	if (c->idrForce) { return KVM_IDR_WHY_BROKEN; } @@ the first key frame of a generation hides the lost access unit
kvm_video_core.c @@ if (!isIdr) { return "none"; } @@ ; @@ a frame that is no key frame is named
kvm_video_core.c @@ return profileIdc == 100 ? kbit * 1250u : kbit * 1000u; @@ return kbit * 1000u; @@ High profile has the same bit rate limit as Main
kvm_video_core.c @@ case 41: case 42: kbit = 50000; break; @@ case 41: case 42: kbit = 20000; break; @@ level 4.1 / 4.2 allow 20 Mb/s
kvm_video_core.c @@ default: kbit = 10000; break; @@ default: kbit = 240000; break; @@ an unknown level has the highest limit
kvm_video_core.c @@ if (dms(now, c->lastFrame) < (int32_t)(intervalMs - early)) { return 0; }\n	return kvm_vctl_write_wait_ms(c, now, intervalMs) == 0; @@ if (dms(now, c->lastFrame) < (int32_t)(intervalMs - early)) { return 0; }\n	return 1; @@ the pace does not look at the write of the previous frame
kvm_video_core.c @@ gap = (uint32_t)(((uint64_t)intervalMs * KVM_VIDEO_WRITE_GAP_PCT) / 100u); @@ gap = (uint32_t)(((uint64_t)intervalMs * 50u) / 100u); @@ the gap between two writes is half an interval
kvm_video_core.c @@ gap = (uint32_t)(((uint64_t)intervalMs * KVM_VIDEO_WRITE_GAP_PCT) / 100u); @@ gap = (uint32_t)(((uint64_t)intervalMs * 100u) / 100u); @@ the gap between two writes is the whole interval
kvm_video_core.c @@ enc = c->encMs > intervalMs / 2u && !c->encThreaded ? intervalMs / 2u : c->encMs; @@ enc = c->encMs; @@ a slow average of the encode time holds the frames back (or lets the next one in right behind a late one)
kvm_video_core.c @@ enc = c->encMs > intervalMs / 2u && !c->encThreaded ? intervalMs / 2u : c->encMs; @@ enc = 0; @@ the encode time is not taken into account (the frames at High wait for each other)
kvm_video_core.c @@ if (!c->haveWrite || intervalMs == 0) { return 0; } @@ if (intervalMs == 0) { return 0; } @@ a generation without a write has a write at time 0
kvm_video_core.c @@ c->haveWrite = 0;	// the first frame of a generation waits for nothing @@ ; @@ the first frame of a new generation waits for the write of the old one
kvm_video_core.c @@ wait = dms(c->lastWrite + gap, now + enc); @@ wait = dms(c->lastWrite + gap, now); @@ the write of this frame is taken for the start
kvm_video_core.c @@ c->haveWrite = 1; c->lastWrite = writeAt; c->encMs = encMs; @@ c->haveWrite = 1; c->encMs = encMs; @@ the write time is not remembered
kvm_video_core.c @@ set_state(t, i, KVM_VT_VIDEO);\n		if (t->fl[i] == 0) { t->nInflight++; } @@ if (t->fl[i] == 0) { t->nInflight++; } @@ a frame that starts does not make its tiles VIDEO (they stay pending and are sent twice)
kvm_video_core.c @@ if (t->fl[i] == 0) { t->nInflight++; }\n		t->fl[i] |= bit; @@ t->fl[i] |= bit; @@ the number of tiles in flight is not counted
kvm_video_core.c @@ count++;\n		fl_clear(t, i, bit);\n	}\n	return count;\n}\n\nunsigned kvm_vtiles_abort @@ count++;\n	}\n	return count;\n}\n\nunsigned kvm_vtiles_abort @@ a frame that was written keeps its tiles in flight (they are never sharpened, a JPEG waits for ever)
kvm_video_core.c @@ if (map != NULL) { map[i] = 1; }\n		count++;\n		fl_clear(t, i, bit); @@ if (map != NULL) { map[i] = 1; }\n		count++;\n		fl_clear(t, i, bit);\n		set_state(t, i, KVM_VT_VIDEO); @@ the encoder thread: the landing of a frame makes a tile that changed again VIDEO (the new change is lost)
kvm_video_core.c @@ if (t->st[i] == KVM_VT_VIDEO) { set_state(t, i, KVM_VT_PENDING); } @@ ; @@ the encoder thread: a lost access unit does not make its tiles pending again (they stay blurred or old)
kvm_video_core.c @@ count++;\n		set_state(t, i, KVM_VT_NONE); @@ count++; @@ a frame that is given up leaves its tiles as they were (not sent as JPEG)
kvm_video_core.c @@ const uint8_t bits = slot < 0 ? (uint8_t)0xFF : (uint8_t)(1u << (unsigned)(slot & 7)); @@ const uint8_t bits = (uint8_t)(1u << (unsigned)(slot & 7)); @@ cancelling every frame cancels one slot only
kvm_video_core.c @@ if (t->st[i] == KVM_VT_PENDING && t->fl[i] == 0) { @@ if (t->st[i] == KVM_VT_PENDING) { @@ I7 takes the tiles that a frame in flight covers
kvm_video_core.c @@ if (t->fl[i] == 0 && (t->st[i] == KVM_VT_VIDEO @@ if ((t->st[i] == KVM_VT_VIDEO @@ a repair takes the tiles that a frame in flight covers
kvm_video_core.c @@ if (t->fl[(size_t)row * (size_t)t->cols + (size_t)col] != 0) { t->jpegOnInflight++; } @@ ; @@ the encoder thread: a JPEG of a tile in flight is not counted as a violation
kvm_video_core.c @@ if (t->st == NULL || t->nInflight == 0) { return 0; }\n	for (r = row0 @@ if (t->st == NULL) { return 0; }\n	for (r = row0 + 1 @@ the rectangle check skips its first row
kvm_video_core.c @@ return t->fl[(size_t)row * (size_t)t->cols + (size_t)col] != 0 ? 1 : 0; @@ return 0; @@ no tile is ever in flight for the barrier
kvm_video_core.c @@ enc = c->encMs > intervalMs / 2u && !c->encThreaded ? intervalMs / 2u : c->encMs; @@ enc = c->encMs > intervalMs / 2u ? intervalMs / 2u : c->encMs; @@ the write gap caps the encode time at half the interval also when the encoder is in a thread
kvm_video_core.c @@ void kvm_vctl_set_threaded(kvm_vctl *c, int threaded) { c->encThreaded = (uint8_t)(threaded ? 1 : 0); } @@ void kvm_vctl_set_threaded(kvm_vctl *c, int threaded) { (void)threaded; } @@ the threaded encoder is not told to the write gap
EOT
survived=0; invalid=0; killed=0; n=0
while IFS= read -r line; do
  [ -z "$line" ] && continue
  n=$((n + 1))
  file=$(printf "%s" "$line" | awk -F " @@ " "{print \$1}")
  search=$(printf "%s" "$line" | awk -F " @@ " "{print \$2}")
  replace=$(printf "%s" "$line" | awk -F " @@ " "{print \$3}")
  desc=$(printf "%s" "$line" | awk -F " @@ " "{print \$4}")
  rm -rf /tmp/m && cp -r /src /tmp/m
  S="$search" R="$replace" perl -0pi -e "BEGIN { \$s = \$ENV{S}; \$r = \$ENV{R}; \$s =~ s/\\\\n/\\n/g; \$r =~ s/\\\\n/\\n/g; } \$c += s/\\Q\$s\\E/\$r/g; END { exit(\$c >= 1 ? 0 : 3) }" /tmp/m/$file
  rc=$?
  if [ $rc -ne 0 ]; then echo "INVALID  ($desc): the text was not found"; invalid=$((invalid + 1)); continue; fi
  if ! gcc -std=c99 -O1 -w -o /tmp/m/t /tmp/m/tests/kvm_video_core_test.c /tmp/m/kvm_video_core.c /tmp/m/kvm_stream.c /tmp/m/kvm_annexb.c 2>/tmp/m/err; then echo "INVALID  ($desc): does not compile"; invalid=$((invalid + 1)); continue; fi
  if /tmp/m/t >/tmp/m/out 2>&1; then echo "SURVIVED ($desc)"; survived=$((survived + 1)); else echo "killed   ($desc): $(grep -m1 FAIL /tmp/m/out | cut -c1-110)"; killed=$((killed + 1)); fi
done < /tmp/mutations.txt
echo "mutations: $n, killed $killed, survived $survived, invalid $invalid"
[ $survived -eq 0 ] && [ $invalid -eq 0 ]
' sh
