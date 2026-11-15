#!/bin/sh
# Mutation check of the tests and the link simulation of kvm_video_rc.c: each line below changes one key rule of the code in a copy, and the tests have to turn
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
kvm_video_rc.c @@ r = ((uint64_t)budgetKbps * 125u * KVM_VRC_BUDGET_PCT) / 100u; @@ r = ((uint64_t)budgetKbps * 125u * 100u) / 100u; @@ the budget is used in full (no 20 % for sharpening)
kvm_video_rc.c @@ if (!have || capacity < r) { r = capacity; } @@ if (!have) { r = capacity; } @@ the link capacity does not limit R below the budget
kvm_video_rc.c @@ if (m > r->cap) { r->cap = m; } @@ ; @@ the capacity estimate never follows the delivered rate up
kvm_video_rc.c @@ if (rate > 0 && linkLimited) { r->cap = m; } @@ if (0) { r->cap = m; } @@ a link-limited sender does not take 0.85 x rate as the capacity
kvm_video_rc.c @@ r->cap += (r->cap * (uint64_t)KVM_VRC_PROBE_PCT * (uint64_t)dt) / 100000u; @@ ; @@ no probing for more capacity
kvm_video_rc.c @@ if (q > KVM_VRC_IDR_QP_MAX) { q = KVM_VRC_IDR_QP_MAX; } @@ ; @@ the IDR QP is not clamped at the top
kvm_video_rc.c @@ if (r->tokens > 0 || never_refused) { return 1; } @@ return 1; @@ the byte bucket refuses nothing
kvm_video_rc.c @@ if (r->tokens < floor) { r->tokens = floor; } @@ ; @@ the debt of a key frame is not limited
kvm_video_rc.c @@ r->idrReal = 0; r->idrTotal = 0; return; } @@ r->idrReal = 0; r->idrTotal = 0; } @@ a key frame counts as content for the regulator
kvm_video_rc.c @@ (uint64_t)r->winBytes * 100u > limit * KVM_VRC_UP_PCT) || @@ (uint64_t)r->winBytes * 100u > limit * 1000u) || @@ the regulator does not see a stream above 110 % of R
kvm_video_rc.c @@ if (++r->belowRun >= KVM_VRC_DOWN_WINDOWS) @@ if (++r->belowRun >= 1) @@ no hysteresis for the QP going down
kvm_video_rc.c @@ if (r->qp > KVM_VIDEO_QP_MAX) { r->qp = KVM_VIDEO_QP_MAX; } @@ ; @@ +2 steps over the QP ceiling
kvm_video_rc.c @@ if (r->qp > r->qpMin) { r->qp -= KVM_VRC_QP_DOWN; @@ if (1) { r->qp -= KVM_VRC_QP_DOWN; @@ the QP has no floor
kvm_video_rc.c @@ || (r->winDenied > 0 && (uint64_t)r->winBytes * 100u > limit * KVM_VRC_REFUSED_PCT); @@ ; @@ refusals by the bucket do not raise the QP
kvm_video_rc.c @@ under = r->winFrames >= KVM_VRC_MIN_FRAMES && @@ under = @@ a window with one frame lowers the QP
kvm_video_rc.c @@ { int64_t cap = (int64_t)(r->rate * KVM_VRC_BURST_MS / 1000u); if (r->tokens > cap) { r->tokens = cap; } } @@ ; @@ a lower target leaves the old burst in the bucket
kvm_video_rc.c @@ (budgetKbps == 0 || r->cap < ((uint64_t)budgetKbps * 125u * KVM_VRC_BUDGET_PCT) / 100u)) @@ 1) @@ probing goes on while the budget is R (the estimate runs away)
kvm_video_rc.c @@ if (r->cap > ceilCap) { r->cap = ceilCap; } @@ ; @@ no ceiling of 2 x max(0.85 x rate, 2 Mb/s) on the estimate
kvm_video_rc.c @@ if (rate > (1ull << 40)) { rate = 1ull << 40; } @@ ; @@ the arithmetic of the estimate is not protected against a huge rate
kvm_video_rc.c @@ ceilCap = ((m > KVM_VRC_DEFAULT_RATE ? m : KVM_VRC_DEFAULT_RATE) * KVM_VRC_CAP_MAX_PCT) / 100u; @@ ceilCap = (m * KVM_VRC_CAP_MAX_PCT) / 100u; @@ the ceiling has no 2 Mb/s floor
kvm_stream.c @@ s->lastHeldBack = now; s->everHeld = 1; s->st.gateDenied++; @@ s->lastHeldBack = now; s->everHeld = 1; s->lastLinkHeld = now; s->linkHeldEver = 1; s->st.gateDenied++; @@ every refusal (the sharpening gate) is link limited
kvm_stream.c @@ if (s->probeState != 1) { s->lastLinkHeld = now; @@ if (1) { s->lastLinkHeld = now; @@ the closed gates of the RTT probe are link limited
kvm_video_rc.c @@ if (link > q) { q = link; } @@ ; @@ the key frame QP ignores the link
kvm_video_rc.c @@ b = (b * r->idrSlopePct) / 100u; q++; @@ q++; @@ the predicted key frame does not shrink with the QP
kvm_video_rc.c @@ const uint64_t fit = (basis * KVM_VRC_IDR_MS) / 1000u; @@ const uint64_t fit = (basis * 5000u) / 1000u; @@ the key frame may take 5 s
kvm_video_rc.c @@ dms(now, r->lastFrame) >= (int32_t)KVM_VRC_IDLE_MS && r->qp > KVM_VRC_IDLE_QP @@ 0 @@ the QP does not go back after a silence
kvm_video_rc.c @@ r->starved = dms(now, r->starveSince) >= (int32_t)KVM_VRC_STARVED_MS ? 1 : 0; @@ r->starved = 1; @@ starved at once
kvm_video_rc.c @@ r->sFrames < (uint32_t)KVM_VRC_STARVED_FPS && (r->sDenied > 0 || r->sHeld > 0) @@ r->sFrames < (uint32_t)KVM_VRC_STARVED_FPS @@ a still screen at the ceiling is starved
kvm_video_rc.c @@ (uint64_t)r->winBytes * 100u < limit * KVM_VRC_CALM_PCT) @@ (uint64_t)r->winBytes * 100u < limit * 100u) @@ confirm: the quiet window is below 100 % of R (it oscillates)
kvm_video_rc.c @@ (r->sDenied > 0 || r->sHeld > 0) @@ r->sDenied > 0 @@ confirm: starved ignores the refusals of the window
kvm_video_rc.c @@ void kvm_vrc_window_refused(kvm_vrc *r) { r->winHeld++; r->sHeld++; r->cIdr = 0; r->capFloor = 0; } @@ void kvm_vrc_window_refused(kvm_vrc *r) { r->sHeld++; r->cIdr = 0; r->capFloor = 0; } @@ confirm: the window refusals do not count for the regulator
kvm_pace.c @@ need = (uint64_t)encAvgUs * par10; @@ need = 0; @@ the frame interval ignores the encode time
kvm_stream.c @@ return s->ackActive && s->linkHeldEver && dms(now, s->lastLinkHeld) < (int32_t)KVM_STREAM_HELD_MS && !estimate_is_lower_bound(s, now); @@ return 1; @@ the sender is always taken for link limited
kvm_video_rc.c @@ if (link > q) { q = link; } @@ q = link; @@ the IDR QP can be better than the P frames
kvm_video_rc.h @@ #define KVM_VRC_IDR_MS 300 @@ #define KVM_VRC_IDR_MS 750 @@ a key frame may take 0.75 s of R again
kvm_video_rc.c @@ if (under && r->winHeld > 0) { under = 0; r->sQpBlocked++; } @@ ; @@ the QP goes down in a window in which the window of the viewer refused a frame
kvm_video_rc.c @@ limit - sh; @@ limit; @@ the sharpening is not taken off R by the regulator
kvm_video_rc.c @@ (sh >= limit - limit / 4u) ? limit / 4u : @@ (sh >= limit - limit / 4u) ? 0 : @@ the video may be left with nothing of R
kvm_video_rc.c @@ if (!videoActive || bytes == 0) { return; } @@ if (bytes == 0) { return; } @@ sharpening outside a movement is charged to the bucket of the video
kvm_video_rc.c @@ r->tokens -= (int64_t)bytes;\n		if (r->tokens < floor) { r->tokens = floor; }\n		r->winSharp @@ r->winSharp @@ the sharpening does not pay from the bucket of the video
kvm_video_rc.c @@ if (r->bitrateMode) { @@ if (0) { @@ the QP regulator runs in the bit rate mode
kvm_video_rc.c @@ if (b > 400000000u) { b = 400000000u; } @@ ; @@ the bit rate has no ceiling
kvm_video_rc.c @@ uint64_t b = r->rate * 8u; @@ uint64_t b = r->rate * 4u; @@ the bit rate is half of R
kvm_video_rc.c @@ || (uint64_t)r->winBytes * 100u < limit * KVM_VRC_CALM_PCT); @@ ); @@ confirm: no step down at 80 % when nothing was refused (QP stuck at the ceiling)
kvm_video_rc.c @@ budgetKbps > 0 || rate == 0 || held_windows(r) >= KVM_VRC_IDR_LIMITED_WINDOWS @@ rate == 0 || held_windows(r) >= KVM_VRC_IDR_LIMITED_WINDOWS @@ a viewer budget does not make the link the limit of the key frame
kvm_video_rc.c @@ budgetKbps > 0 || rate == 0 || held_windows(r) >= KVM_VRC_IDR_LIMITED_WINDOWS @@ budgetKbps > 0 || held_windows(r) >= KVM_VRC_IDR_LIMITED_WINDOWS @@ an unknown rate is not cautious
kvm_video_rc.c @@ held_windows(r) >= KVM_VRC_IDR_LIMITED_WINDOWS @@ held_windows(r) > KVM_VRC_IDR_LIMITED_WINDOWS @@ one refusing window more is needed for a full link
kvm_video_rc.c @@ held_windows(r) >= KVM_VRC_IDR_LIMITED_WINDOWS @@ 0 @@ the link is never taken for full
kvm_video_rc.c @@ if (!r->idrLimited && r->cIdr != 0) @@ if (0) @@ the key frame is always sized from R
kvm_video_rc.c @@ uint64_t basis = r->rate; @@ uint64_t basis = r->idrCap; @@ the key frame is always sized from the ceiling
kvm_video_rc.c @@ r->heldHist = (r->heldHist << 1) | (r->winHeld > 0 ? 1ull : 0ull); @@ r->heldHist = (r->heldHist << 1); @@ the refusals of the window are not remembered
kvm_video_rc.c @@ for (k = 1; k < nw && k < 64; k++) { r->heldHist <<= 1; } @@ ; @@ a silence does not push the refusals out of the memory
kvm_video_rc.c @@ uint64_t c = (r->cIdr * KVM_VRC_CIDR_PCT) / 100u; @@ uint64_t c = r->cIdr; @@ the key frame is sized for all of what the last one took
kvm_video_rc.c @@ if (c > r->idrCap) { c = r->idrCap; } @@ ; @@ the evidence has no cap
kvm_video_rc.c @@ if (c > basis) { basis = c; } @@ basis = c; @@ the evidence may make the key frame smaller than R allows
kvm_video_rc.c @@ if (!r->idrLimited && r->cIdr != 0) @@ if (r->cIdr != 0) @@ the evidence counts on a link that refuses frames
kvm_video_rc.c @@ r->sHeld++; r->cIdr = 0; r->capFloor = 0; } @@ r->sHeld++; r->capFloor = 0; } @@ a refusal of the window does not take the evidence away
kvm_video_rc.c @@ if (dur < 10) { dur = 10; } @@ ; @@ no minimum of 10 ms for the key frame
kvm_video_rc.c @@ if (r->cIdr != 0 && dms(now, r->cIdrAt) > (int32_t)KVM_VRC_CIDR_MS) { r->cIdr = 0; } @@ ; @@ the evidence never expires
kvm_video_rc.c @@ (int64_t)(int32_t)(ackAt - ts) @@ (int64_t)(uint32_t)(ackAt - ts) @@ the time of the key frame is unsigned
kvm_video_rc.c @@ r->idrCap = ceilCap * 2u; @@ r->idrCap = ceilCap; @@ the cap of the evidence is the ceiling itself
kvm_video_rc.c @@ if (r->cap < KVM_VRC_MIN_RATE) { r->cap = KVM_VRC_MIN_RATE; } @@ ; @@ the estimate may be 30 B/s (the probing never begins: R stays at its floor)
kvm_video_rc.c @@ if (r->capFloor > eff && held_windows(r) < KVM_VRC_IDR_LIMITED_WINDOWS) { @@ if (0) { @@ the proof of the key frame is not used for R
kvm_video_rc.c @@ r->capFloor > eff && held_windows(r) < KVM_VRC_IDR_LIMITED_WINDOWS @@ r->capFloor > eff @@ the proof of a key frame counts on a link that is full
kvm_video_rc.c @@ r->cIdr = 0; r->capFloor = 0; } @@ r->cIdr = 0; } @@ a refusal of the window does not take the proof away (the link may have fallen)
kvm_video_rc.c @@ if (r->capFloor != 0 && dms(now, r->capFloorAt) > (int32_t)KVM_VRC_FLOOR_MS) { r->capFloor = 0; } @@ ; @@ the proof never expires
kvm_video_rc.c @@ - (int64_t)queueMs;\n		if (whole < 10) @@ - (int64_t)queueMs - (int64_t)minRtt;\n		if (whole < 10) @@ the proof takes off the round trip (minRtt of a queue makes a key frame "infinitely" fast)
kvm_video_rc.c @@ if (eff > ceilCap) { eff = ceilCap; } @@ ; @@ the proof has no ceiling (R of 60 Mb/s from one key frame)
kvm_video_rc.c @@ * KVM_VRC_RATE_PCT / 100u;\n		r->capFloorAt @@ ;\n		r->capFloorAt @@ the proof is all of what the key frame took (no 15 % margin)
kvm_video_rc.c @@ (int64_t)(int32_t)(ackAt - ts) - (int64_t)queueMs;\n		if (whole @@ (int64_t)(uint32_t)(ackAt - ts) - (int64_t)queueMs;\n		if (whole @@ the time of the proof is unsigned
kvm_video_rc.c @@ if (whole < 10) { whole = 10; } @@ ; @@ no minimum of 10 ms for the proof
kvm_video_rc.c @@ r->cap = KVM_VRC_DEFAULT_RATE; @@ r->cap = rate > 0 ? m : KVM_VRC_DEFAULT_RATE; @@ a lower bound sets the first estimate (28 B/s: R 100 kb/s)
kvm_video_rc.c @@ void kvm_vrc_window_refused_soft(kvm_vrc *r) { r->sHeld++; } @@ void kvm_vrc_window_refused_soft(kvm_vrc *r) { r->winHeld++; r->sHeld++; r->cIdr = 0; r->capFloor = 0; } @@ a refusal that is no evidence of the link takes the proof of the key frame away
kvm_video_rc.c @@ void kvm_vrc_window_refused_soft(kvm_vrc *r) { r->sHeld++; } @@ void kvm_vrc_window_refused_soft(kvm_vrc *r) { } @@ a soft refusal is not counted
kvm_video_rc.c @@ r->rFromFloor = (uint8_t)(target > kvm_vrc_target(budgetKbps, r->cap) ? 1 : 0); @@ r->rFromFloor = 0; @@ the log never says that R is the proof of a key frame
kvm_video_rc.c @@ r->rFromFloor = (uint8_t)(target > kvm_vrc_target(budgetKbps, r->cap) ? 1 : 0); @@ r->rFromFloor = (uint8_t)(eff > r->cap ? 1 : 0); @@ the log says that R is the proof when the budget of the viewer holds R down and the proof changes nothing
kvm_video_rc.c @@ r->rFromFloor = (uint8_t)(target > kvm_vrc_target(budgetKbps, r->cap) ? 1 : 0); @@ r->rFromFloor = (uint8_t)(r->capFloor != 0 ? 1 : 0); @@ the log keeps saying that R is the proof after it was taken away or is not used
kvm_video_rc.c @@ if (!r->idrMeasured) { r->lastIdrBytes = r->idrPriorBytes; @@ { r->lastIdrBytes = r->idrPriorBytes; @@ a measured key frame is replaced by the prior when the backend is set
kvm_video_rc.c @@ r->lastIdrQp = qp; r->idrMeasured = 1; @@ r->lastIdrQp = qp; @@ a measured key frame is not remembered as measured
kvm_video_rc.c @@ if (r->sIdr == 0 && !r->idrMeasured && pixels > 0) @@ if (r->sIdr == 0 && pixels > 0) @@ the size of the picture overwrites a measured key frame after the statistics were cleared
kvm_video_rc.c @@ (b * r->idrSlopePct) / 100u @@ (b * KVM_VRC_IDR_SLOPE_PCT) / 100u @@ the prediction upwards ignores the slope of the backend
kvm_video_rc.c @@ (b * 100u) / r->idrSlopePct @@ (b * 100u) / KVM_VRC_IDR_SLOPE_PCT @@ the prediction downwards ignores the slope of the backend
kvm_video_rc.c @@ r->idrSlopePct = hw ? KVM_VRC_HW_IDR_SLOPE_PCT : KVM_VRC_IDR_SLOPE_PCT; @@ r->idrSlopePct = 0; @@ the backend sets no slope
kvm_video_rc.c @@ r->idrPriorBytes = hw ? KVM_VRC_HW_IDR_PRIOR_BYTES : KVM_VRC_IDR_PRIOR_BYTES; @@ r->idrPriorBytes = 0; @@ the backend sets no prior
kvm_video_rc.c @@ r->qpMin = hw ? KVM_VIDEO_QP_MIN - KVM_HW_QP_OFFSET : KVM_VIDEO_QP_MIN; @@ r->qpMin = KVM_VIDEO_QP_MIN; @@ the hardware encoder has the floor of the software one
kvm_video_rc.c @@ r->qpMin = hw ? KVM_VIDEO_QP_MIN - KVM_HW_QP_OFFSET : KVM_VIDEO_QP_MIN; @@ r->qpMin = hw ? KVM_VIDEO_QP_MIN - KVM_HW_QP_OFFSET : KVM_VIDEO_QP_MIN + 1; @@ the software floor is wrong after a hardware generation
kvm_video_rc.c @@ if (r->qp < r->qpMin) { r->qp = r->qpMin; } @@ ; @@ the QP stays below the new floor
kvm_video_rc.c @@ if (r->qp > r->qpMin) { r->qp -= KVM_VRC_QP_DOWN; @@ if (r->qp > KVM_VIDEO_QP_MIN) { r->qp -= KVM_VRC_QP_DOWN; @@ the regulator goes below the floor of the hardware encoder
kvm_video_rc.c @@ return r->qpMin > KVM_VRC_IDR_QP_MIN ? r->qpMin : KVM_VRC_IDR_QP_MIN; @@ return KVM_VRC_IDR_QP_MIN; @@ the key frame goes below the floor of the hardware encoder
kvm_video_rc.c @@ r->qpMin = KVM_VIDEO_QP_MIN;\n @@ r->qpMin = 0;\n @@ a regulator without a backend call has no floor
kvm_video_core.c @@ int q = regulatorQp + KVM_HW_QP_OFFSET; @@ int q = regulatorQp; @@ no offset for the hardware encoder
kvm_video_core.c @@ if (q < KVM_HW_QP_FLOOR) { q = KVM_HW_QP_FLOOR; } @@ ; @@ the hardware QP goes below the floor of the encoder
kvm_video_core.c @@ if (q > 51) { q = 51; } @@ ; @@ the hardware QP goes above 51
kvm_video_rc.c @@ if (hw) { kvm_vrc_set_backend(r, 1); } @@ ; @@ a new regulator for the hardware encoder forgets which encoder it is for
kvm_video_rc.c @@ void kvm_vrc_init(kvm_vrc *r, int startQp, uint32_t now) { kvm_vrc_init_backend(r, startQp, now, 0); } @@ void kvm_vrc_init(kvm_vrc *r, int startQp, uint32_t now) { kvm_vrc_init_backend(r, startQp, now, 1); } @@ the software init makes a regulator for the hardware encoder
kvm_video_rc.h @@ #define KVM_VRC_HW_IDR_PRIOR_BYTES ((KVM_VRC_IDR_PRIOR_BYTES * 11) / 10) @@ #define KVM_VRC_HW_IDR_PRIOR_BYTES (KVM_VRC_IDR_PRIOR_BYTES) @@ the prior of the hardware key frame is the software one
kvm_video_rc.c @@ return ((uint64_t)r->flatMilli * total + c_milli(r, qp) * real + 500u) / 1000u; @@ return (c_milli(r, qp) * real + 500u) / 1000u; @@ the forecast of a key frame has no floor
kvm_video_rc.c @@ return ((uint64_t)r->flatMilli * total + c_milli(r, qp) * real + 500u) / 1000u; @@ return ((uint64_t)r->flatMilli * total + 500u) / 1000u; @@ the forecast of a key frame does not grow with the area
kvm_video_rc.c @@ const uint64_t above = last > floorB ? last - floorB : last / 8u; @@ const uint64_t above = last; @@ the cost per macroblock counts the floor
kvm_video_rc.c @@ lastReal = r->lastIdrReal != 0 ? r->lastIdrReal : total; @@ lastReal = r->lastIdrReal != 0 ? r->lastIdrReal : 1u; @@ the last key frame of the whole picture is taken for one of a macroblock
kvm_video_rc.c @@ r->lastIdrReal = r->idrReal; @@ ; @@ the area of the measured key frame is forgotten
kvm_video_rc.c @@ r->idrReal = 0; r->idrTotal = 0; return; } @@ return; } @@ the area of a key frame that was made stays planned for the next one
kvm_video_rc.c @@ r->winFrames++; r->winBytes += bytes > virginBytes ? bytes - virginBytes : 0u; @@ r->winFrames++; r->winBytes += bytes; @@ the virgin macroblocks are the content getting busier
kvm_video_rc.c @@ r->tokens -= (int64_t)bytes;\n	if (r->tokens < floor) @@ r->tokens -= (int64_t)(bytes > virginBytes ? bytes - virginBytes : 0u);\n	if (r->tokens < floor) @@ the bucket is not charged with the virgin macroblocks
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
  if ! gcc -std=c99 -O1 -w -o /tmp/m/t /tmp/m/tests/kvm_video_rc_test.c /tmp/m/kvm_pace.c /tmp/m/kvm_video_rc.c /tmp/m/kvm_video_core.c /tmp/m/kvm_stream.c /tmp/m/kvm_annexb.c -lm 2>/tmp/m/err; then echo "INVALID  ($desc): does not compile"; invalid=$((invalid + 1)); continue; fi
  if /tmp/m/t >/tmp/m/out 2>&1; then echo "SURVIVED ($desc)"; survived=$((survived + 1)); else echo "killed   ($desc): $(grep -m1 FAIL /tmp/m/out | cut -c1-110)"; killed=$((killed + 1)); fi
done < /tmp/mutations.txt
echo "mutations: $n, killed $killed, survived $survived, invalid $invalid"
[ $survived -eq 0 ] && [ $invalid -eq 0 ]
' sh
