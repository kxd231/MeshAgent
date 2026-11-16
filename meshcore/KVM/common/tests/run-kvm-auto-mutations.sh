#!/bin/sh
# Mutation check of the tests of kvm_auto.c: each line below changes one rule of the code in a copy, and the tests (tests/kvm_auto_test.c) have to turn red; a mutation that does
# not compile is reported as invalid, one that the tests do not notice as SURVIVED (exit status 1 at the end).
# Usage: run-kvm-auto-mutations.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
COMMON=$(dirname "$HERE")
IMG=kvm-ctest:local
docker image inspect "$IMG" >/dev/null 2>&1 || { echo "run run-kvm-auto-tests.sh first (it builds the image)"; exit 2; }
exec docker run --rm --label kvm-auto-agent=1 --user "$(id -u):$(id -g)" -v "$COMMON:/src:ro" -w /tmp "$IMG" sh -c '
set -u
cat > /tmp/mutations.txt <<"EOT"
kvm_auto.c @@ to = env->minRttMs < (uint32_t)KVM_AUTO_START_RTT_MS ? 0 : 1; @@ to = env->minRttMs <= (uint32_t)KVM_AUTO_START_RTT_MS ? 0 : 1; @@ start: a round trip of 20 ms is a LAN
kvm_auto.c @@ a->step = (uint8_t)to; a->changes++; @@ a->step = (uint8_t)to; a->changes++; a->ups++; a->haveUp = 1; a->lastUpAt = now; @@ start: the start at 60 counts as a step up that can fail
kvm_auto.c @@ while (to < KVM_AUTO_STEPS - 1 && (uint32_t)kFps[to] > env->maxFps) { to++; } @@ while (to < KVM_AUTO_STEPS - 1 && (uint32_t)kFps[to] >= env->maxFps) { to++; } @@ ceiling: the step below the ceiling is taken
kvm_auto.c @@ set_step(a, to, env->maxReason != 0 ? env->maxReason : KVM_AUTO_R_SCREEN, now, dec); @@ set_step(a, to, KVM_AUTO_R_SCREEN, now, dec); @@ ceiling: the reason is always the screen
kvm_auto.c @@ if (!env->valid || w->key || w->motionMs < (uint32_t)KVM_AUTO_MIN_MOTION_MS) { return 0; } @@ if (w->key || w->motionMs < (uint32_t)KVM_AUTO_MIN_MOTION_MS) { return 0; } @@ window: evaluated while no video runs
kvm_auto.c @@ if (!env->valid || w->key || w->motionMs < (uint32_t)KVM_AUTO_MIN_MOTION_MS) { return 0; } @@ if (!env->valid || w->key) { return 0; } @@ window: evaluated without motion (a silent screen is a slow link)
kvm_auto.c @@ if (!env->valid || w->key || w->motionMs < (uint32_t)KVM_AUTO_MIN_MOTION_MS) { return 0; } @@ if (!env->valid || w->key || w->motionMs <= (uint32_t)KVM_AUTO_MIN_MOTION_MS) { return 0; } @@ window: 300 ms of motion is not enough
kvm_auto.c @@ ach10 = ((uint64_t)w->made * 10000u) / (uint64_t)w->motionMs; @@ ach10 = ((uint64_t)w->made * 10000u) / (uint64_t)w->windowMs; @@ achieved rate: over the whole window, not over the motion
kvm_auto.c @@ fA = (ach10 * 100u < (uint64_t)kFps[step] * 10u * (uint64_t)KVM_AUTO_KEEP_PCT && held == 0 && w->pendingIters > 0 && srcFaster) ? 1 : 0; @@ fA = (ach10 * 100u <= (uint64_t)kFps[step] * 10u * (uint64_t)KVM_AUTO_KEEP_PCT && held == 0 && w->pendingIters > 0 && srcFaster) ? 1 : 0; @@ 45 of 60 frames a second is not kept up
kvm_auto.c @@ && held == 0 && w->pendingIters > 0 && srcFaster) ? 1 : 0; @@ && w->pendingIters > 0 && srcFaster) ? 1 : 0; @@ frames held back by the link count as the machine
kvm_auto.c @@ && held == 0 && w->pendingIters > 0 && srcFaster) ? 1 : 0; @@ && held == 0 && srcFaster) ? 1 : 0; @@ a source that stands still is a slow machine
kvm_auto.c @@ && held == 0 && w->pendingIters > 0 && srcFaster) ? 1 : 0; @@ && held == 0 && w->pendingIters > 0) ? 1 : 0; @@ a slow source is a slow machine
kvm_auto.c @@ fD = (w->gapMedMs > 0 && @@ fD = (0 && @@ loop gap: never limits
kvm_auto.c @@ srcFaster = (w->dwmX100 == 0 || w->dwmX100 >= (uint32_t)KVM_AUTO_DWM_SLOW_X100) ? 1 : 0; @@ srcFaster = (w->dwmX100 == 0 || w->dwmX100 > (uint32_t)KVM_AUTO_DWM_SLOW_X100) ? 1 : 0; @@ dwm: 1.10 frames per image is not the loop leaving images out
kvm_auto.c @@ srcFaster = (w->dwmX100 == 0 || w->dwmX100 >= (uint32_t)KVM_AUTO_DWM_SLOW_X100) ? 1 : 0; @@ srcFaster = (w->dwmX100 >= (uint32_t)KVM_AUTO_DWM_SLOW_X100) ? 1 : 0; @@ dwm: unknown (GDI) is a slow source
kvm_auto.c @@ ceilQp = kCeil[cls][step]; @@ ceilQp = kCeil[0][step]; @@ class: the video class has the ceilings of the UI
kvm_auto.c @@ if (a->cpuStreak >= KVM_AUTO_DOWN_WINDOWS && step < KVM_AUTO_STEPS - 1 && canMove) @@ if (a->cpuStreak >= 1 && step < KVM_AUTO_STEPS - 1 && canMove) @@ cpu: one bad window is enough
kvm_auto.c @@ if (a->qpStreak >= KVM_AUTO_DOWN_WINDOWS && step < KVM_AUTO_STEPS - 1 && canMove) @@ if (a->qpStreak >= 1 && step < KVM_AUTO_STEPS - 1 && canMove) @@ link: one bad window is enough
kvm_auto.c @@ set_step(a, step + 1, KVM_AUTO_R_LINK, now, dec); @@ set_step(a, step + 2 > KVM_AUTO_STEPS - 1 ? KVM_AUTO_STEPS - 1 : step + 2, KVM_AUTO_R_LINK, now, dec); @@ link: two steps at once
kvm_auto.c @@ if (limited) { a->cpuStreak = (uint8_t)sat8(a->cpuStreak + 1u); a->qpStreak = 0; a->goodStreak = 0; } @@ if (limited) { a->cpuStreak = (uint8_t)sat8(a->cpuStreak + 1u); a->goodStreak = 0; } @@ streaks: a machine window does not clear the link streak
kvm_auto.c @@ else { a->cpuStreak = 0; a->qpStreak = 0; } @@ else { a->qpStreak = 0; } @@ streaks: a good window does not clear the machine streak
kvm_auto.c @@ if (heldPct >= (uint32_t)KVM_AUTO_HELD_UP_PCT) { ok = 0; } @@ if (heldPct > (uint32_t)KVM_AUTO_HELD_UP_PCT) { ok = 0; } @@ up: 5 % held is fine
kvm_auto.c @@ if (heldPct >= (uint32_t)KVM_AUTO_HELD_UP_PCT) { ok = 0; } @@ if (0) { ok = 0; } @@ up: held frames do not matter
kvm_auto.c @@ if (ach10 * 100u < (uint64_t)kFps[step] * 10u * (uint64_t)KVM_AUTO_SAT_PCT) { ok = 0; } @@ if (0) { ok = 0; } @@ up: no use for frames is no reason against
kvm_auto.c @@ / 10000u > (uint64_t)env->budgetMilli) { ok = 0; } @@ / 10000u >= (uint64_t)env->budgetMilli) { ok = 0; } @@ up: a CPU use equal to the budget is over it
kvm_auto.c @@ if (env->encAvgUs > 0 && env->budgetMilli > 0 && @@ if (env->encAvgUs > 0 && 0 && @@ up: the CPU budget is not looked at
kvm_auto.c @@ if (env->maxFps > 0 && nfps > env->maxFps) { ok = 0; } @@ if (env->maxFps > 0 && nfps >= env->maxFps) { ok = 0; } @@ up: a ceiling equal to the faster step blocks
kvm_auto.c @@ if (env->maxFps > 0 && nfps > env->maxFps) { ok = 0; } @@ if (0) { ok = 0; } @@ up: the ceiling from outside is exceeded by a step up
kvm_auto.c @@ if (a->goodStreak >= a->upNeed && (!a->haveChange @@ if (a->goodStreak > a->upNeed && (!a->haveChange @@ up: one more window than the patience
kvm_auto.c @@ uint32_t n = a->upNeed * 2u; @@ uint32_t n = a->upNeed * 3u; @@ patience: it triples
kvm_auto.c @@ a->upNeed = n > (uint32_t)KVM_AUTO_UP_MAX ? (uint32_t)KVM_AUTO_UP_MAX : n; @@ a->upNeed = n; @@ patience: no maximum
kvm_auto.c @@ if (reason == KVM_AUTO_R_LINK && a->upNeed < (uint32_t)KVM_AUTO_LINK_WAIT) @@ if (0 && a->upNeed < (uint32_t)KVM_AUTO_LINK_WAIT) @@ link: a step down for the link has no wait of its own
kvm_auto.c @@ if (reason == KVM_AUTO_R_LINK && a->upNeed < (uint32_t)KVM_AUTO_LINK_WAIT) @@ if (a->upNeed < (uint32_t)KVM_AUTO_LINK_WAIT) @@ link: every step down waits, the CPU and a ceiling from outside too
kvm_auto.c @@ if (reason == KVM_AUTO_R_LINK && a->upNeed < (uint32_t)KVM_AUTO_LINK_WAIT) @@ if (reason != KVM_AUTO_R_CPU && a->upNeed < (uint32_t)KVM_AUTO_LINK_WAIT) @@ link: the capture waits as the link does
kvm_auto.c @@ if (reason == KVM_AUTO_R_LINK && a->upNeed < (uint32_t)KVM_AUTO_LINK_WAIT) @@ if (reason == KVM_AUTO_R_LINK) @@ link: the wait replaces a patience that is longer
kvm_auto.c @@ if (reason == KVM_AUTO_R_LINK && a->upNeed < (uint32_t)KVM_AUTO_LINK_WAIT) { a->upNeed = (uint32_t)KVM_AUTO_LINK_WAIT; } @@ if (reason == KVM_AUTO_R_LINK && a->upNeed < (uint32_t)KVM_AUTO_LINK_WAIT) { a->upNeed = (uint32_t)KVM_AUTO_LINK_WAIT - 1u; } @@ link: the wait is one window short
kvm_auto.h @@ #define KVM_AUTO_LINK_WAIT 64 @@ #define KVM_AUTO_LINK_WAIT 32 @@ link: the wait is 32 windows
kvm_auto.c @@ 		a->haveUp = 0;\n		a->downs++; @@ 		a->downs++; @@ patience: a second down counts for the same up
kvm_auto.c @@ if (pending == 0) { a->haveCap = 0; a->haveImage = 0; return; } @@ if (pending == 0) { a->haveCap = 0; return; } @@ images: a gap is measured across a break of the series
kvm_auto.c @@ if (pending == 0) { a->haveCap = 0; a->haveImage = 0; return; } @@ if (pending == 0) { a->haveImage = 0; return; } @@ images: the capture of an image without motion is counted for the next one
kvm_auto.c @@ if (gap > 0 && gap < (int32_t)KVM_AUTO_GAP_MAX_MS) @@ if (gap > 0 && gap < 5000) @@ images: a pause between series is an iteration
kvm_auto.c @@ (uint32_t)(((uint64_t)a->dwmFrames * 100u) / a->dwmImages) @@ (uint32_t)(((uint64_t)a->dwmFrames * 10u) / a->dwmImages) @@ dwm: the ratio is x 10
kvm_auto.c @@ a->qpN++; a->qpSum += (uint32_t)qp; a->qpEnd = (uint32_t)qp; @@ a->qpN++; a->qpSum += (uint32_t)qp; @@ frames: the QP at the end of the window is not kept
kvm_auto.c @@ void kvm_auto_note_held(kvm_auto *a, int kind)\n{\n	if (!a->on) { return; } @@ void kvm_auto_note_held(kvm_auto *a, int kind)\n{\n @@ off: a refusal is noted with the bit down
kvm_auto.c @@ if (a->on) { return; }\n		reset(a); @@ reset(a); @@ set: a second call with the bit up starts again
kvm_auto.c @@ else { reset(a); } @@ else { a->on = 0; } @@ set: the bit down leaves the state
kvm_auto.c @@ if (!a->on) { return 0; }\n	dec->intervalMs @@ if (0) { return 0; }\n	dec->intervalMs @@ off: the decision works with the bit down
kvm_auto.c @@ if (!a->on) { memset(dec, 0, sizeof(*dec)); dec->from = dec->to = a->step; return 0; } @@ /* removed */ @@ off: the tick works with the bit down
kvm_auto.c @@ qpBad = (qpNow10 > 0 && kvm_auto_pred_qp10(qpNow10, heldPct) > (uint32_t)ceilQp * 10u) ? 1 : 0; @@ qpBad = (qpNow10 > 0 && kvm_auto_pred_qp10(qpNow10, heldPct) >= (uint32_t)ceilQp * 10u) ? 1 : 0; @@ link: a QP at the ceiling is above it
kvm_auto.c @@ qpBad = (qpNow10 > 0 && kvm_auto_pred_qp10(qpNow10, heldPct) > (uint32_t)ceilQp * 10u) ? 1 : 0; @@ qpBad = (qpNow10 > 0 && qpNow10 > (uint32_t)ceilQp * 10u) ? 1 : 0; @@ link: the refusals are not a rise of the QP to come
kvm_auto.c @@ qpBad = (qpNow10 > 0 && kvm_auto_pred_qp10(qpNow10, heldPct) > (uint32_t)ceilQp * 10u) ? 1 : 0; @@ qpBad = (qpNow10 > 0 && heldPct > 20 && kvm_auto_pred_qp10(qpNow10, heldPct) > (uint32_t)ceilQp * 10u) ? 1 : 0; @@ link: the QP above the ceiling without refusals is fine (the literal rule of the design)
kvm_auto.c @@ qpNow10 = env->qpNow > 0 ? env->qpNow * 10u : (w->qpN > 0 ? w->qpEnd * 10u : 0u); @@ qpNow10 = w->qpN > 0 ? w->qpEnd * 10u : 0u; @@ link: the QP of the regulator is not used
kvm_auto.c @@ qpNow10 = env->qpNow > 0 ? env->qpNow * 10u : (w->qpN > 0 ? w->qpEnd * 10u : 0u); @@ qpNow10 = env->qpNow > 0 ? env->qpNow * 10u : 0u; @@ link: the QP of the last frame is not the fallback
kvm_auto.c @@ static const uint16_t kRise10[11] = { 0, 9, 19, 31, 44, 60, 79, 103, 138, 198, 300 }; @@ static const uint16_t kRise10[11] = { 0, 9, 19, 31, 44, 60, 79, 103, 138, 198, 0 }; @@ prediction: all refused is no rise
kvm_auto.c @@ if (heldPct > 100u) { heldPct = 100u; } @@ /* removed */ @@ prediction: a share above 100 reads past the table
kvm_auto.c @@ if (!env->valid || w->key || w->motionMs < (uint32_t)KVM_AUTO_MIN_MOTION_MS) { return 0; } @@ if (!env->valid || w->motionMs < (uint32_t)KVM_AUTO_MIN_MOTION_MS) { return 0; } @@ key: a window with a key frame is evaluated
kvm_auto.c @@ until = now + (uint32_t)KVM_AUTO_KEY_FREEZE_MS + needMs; @@ until = now + (uint32_t)KVM_AUTO_KEY_FREEZE_MS; @@ key: the time the key frame needs on the link is not counted
kvm_auto.c @@ if (needMs > 60000u) { needMs = 60000u; } @@ /* removed */ @@ key: no bound of the freeze
kvm_auto.c @@ if (env->queueMs >= (uint32_t)KVM_AUTO_UP_QUEUE_MS) { ok = 0; } @@ if (env->queueMs > (uint32_t)KVM_AUTO_UP_QUEUE_MS) { ok = 0; } @@ queue: 100 ms is not a queue
kvm_auto.c @@ if (env->queueMs >= (uint32_t)KVM_AUTO_UP_QUEUE_MS) { ok = 0; } @@ if (0) { ok = 0; } @@ queue: a step up into a full link
kvm_auto.c @@ p = (int)qpNow10 + qp_delta10(ratio100); @@ p = (int)qpNow10 + 60; @@ up: the faster step always costs 6 QP
kvm_auto.c @@ return (uint32_t)(p < floor10 ? floor10 : p); @@ return (uint32_t)p; @@ up: the QP does not stop at the floor of the regulator
kvm_auto.c @@ const int floor10 = (int)(qpFloor > 0 ? qpFloor : (uint32_t)KVM_AUTO_QP_FLOOR) * 10; @@ const int floor10 = (int)KVM_AUTO_QP_FLOOR * 10; @@ up: the floor of the hardware encoder is not used
kvm_auto.c @@ if (rateBps > 0 && bytes > 0 && motionMs > 0) @@ if (0) @@ up: the utilization of R is not looked at
kvm_auto.c @@ ratio100 = bps * scale100 / rateBps; @@ ratio100 = bps * 100 / rateBps; @@ up: the faster rate is not counted in the stream
kvm_auto.c @@ const uint64_t bps = (uint64_t)bytes * 1000u / motionMs; @@ const uint64_t bps = (uint64_t)bytes / motionMs; @@ up: the stream is bytes a millisecond
kvm_auto.c @@ const uint64_t scale100 = (uint64_t)kFps[n] * 100u / (uint64_t)kFps[clamp_step(step)]; @@ const uint64_t scale100 = 200u; @@ up: the faster step is always twice the rate
kvm_auto.c @@ { 100, 0 }, { 110, 8 } @@ { 100, 0 }, { 110, 30 } @@ up: the table of the QP
kvm_auto.c @@ if (w->qpN == 0 || qpNow10 == 0 || kvm_auto_up_qp10( @@ if (kvm_auto_up_qp10( @@ up: a window without a frame is good
kvm_auto.c @@ kvm_auto_up_qp10(step, qpNow10, w->bytes, w->motionMs, env->rateBps, env->qpFloor) > (uint32_t)(ceilN - KVM_AUTO_UP_QP_HEADROOM) * 10u @@ kvm_auto_up_qp10(step, qpNow10, w->bytes, w->motionMs, env->rateBps, env->qpFloor) > (uint32_t)ceilN * 10u @@ up: no headroom under the ceiling of the faster step
kvm_auto.c @@ kvm_auto_up_qp10(step, qpNow10, w->bytes, w->motionMs, env->rateBps, env->qpFloor) @@ kvm_auto_up_qp10(step, qpNow10, w->bytes, w->motionMs, env->rateBps, 0) @@ up: the floor of the hardware encoder is not given
kvm_auto.c @@ return qpNow10 + kRise10[(heldPct + 5u) / 10u]; @@ return qpNow10 + kRise10[heldPct / 10u]; @@ prediction: the deciles are floored, not rounded
kvm_auto.c @@ a->bytes += bytes; @@ /* removed */ @@ frames: the bytes are not noted
kvm_auto.c @@ fB = (env->floorMs > 0 && (uint64_t)env->floorMs * 1000u > (uint64_t)kUs[step]) ? 1 : 0; @@ fB = (env->floorMs > 0 && (uint64_t)env->floorMs * 1000u >= (uint64_t)kUs[step]) ? 1 : 0; @@ budget: a floor equal to the target limits
kvm_auto.c @@ static const uint32_t kUs[KVM_AUTO_STEPS] = { 16667, 33333, 50000, 66667, 100000 }; @@ static const uint32_t kUs[KVM_AUTO_STEPS] = { 16000, 33000, 50000, 66000, 100000 }; @@ cost: the interval of 60 is 16 ms, not 16.67
kvm_auto.c @@ fC = (w->capP75Ms > 0 && over(w->capP75Ms, kUs[step]) && srcFaster) ? 1 : 0; @@ fC = (w->capP75Ms > 0 && over(w->capP75Ms, kUs[step])) ? 1 : 0; @@ capture: a capture that waits for a slow source limits
kvm_auto.c @@ fD = (w->gapMedMs > 0 && over(w->gapMedMs, kUs[step]) && srcFaster) ? 1 : 0; @@ fD = (w->gapMedMs > 0 && over(w->gapMedMs, kUs[step])) ? 1 : 0; @@ loop gap: a slow source limits
kvm_auto.c @@ static uint32_t hist_med(const uint16_t *h, uint32_t n) { return hist_pct(h, n, 1u, 2u); } @@ static uint32_t hist_med(const uint16_t *h, uint32_t n) { return hist_pct(h, n, 3u, 4u); } @@ loop gap: the P75, not the median
kvm_auto.c @@ need = (n * num + den - 1u) / den; @@ need = (n * num) / den; @@ percentile: rounded down
kvm_auto.c @@ while (to < KVM_AUTO_STEPS - 1 && over(need, kUs[to])) { to++; } @@ while (to < KVM_AUTO_STEPS - 1 && 0) { to++; } @@ cpu: always one step
kvm_auto.c @@ (fC || fD || (fA && !fB && w->dwmX100 == 0)) ? KVM_AUTO_R_CAPTURE : KVM_AUTO_R_CPU @@ (fC || fD) ? KVM_AUTO_R_CAPTURE : KVM_AUTO_R_CPU @@ cpu: a low rate without dwm is the CPU
kvm_auto.c @@ (fC || fD || (fA && !fB && w->dwmX100 == 0)) ? KVM_AUTO_R_CAPTURE : KVM_AUTO_R_CPU @@ KVM_AUTO_R_CPU @@ cpu: the reason is never the capture
kvm_auto.c @@ if (w->capP75Ms > 0 && over(w->capP75Ms, nus)) { ok = 0; } @@ if (w->capP75Ms > 0 && (uint64_t)w->capP75Ms * 83u > (uint64_t)nus / 10u) { ok = 0; } @@ up: the capture has a stricter threshold than staying
kvm_auto.c @@ if (w->capP75Ms > 0 && over(w->capP75Ms, nus)) { ok = 0; } @@ /* removed */ @@ up: the capture is not looked at
kvm_auto.c @@ if (env->encAvgUs > 0 && (uint64_t)env->encAvgUs * (uint64_t)KVM_AUTO_KEEP_PCT > (uint64_t)nus * 100u) { ok = 0; } @@ /* removed */ @@ up: the encode time is not looked at
kvm_auto.c @@ if ((uint64_t)env->floorMs * 1000u > (uint64_t)nus) { ok = 0; } @@ if ((uint64_t)env->floorMs * 1000u >= (uint64_t)nus) { ok = 0; } @@ up: a floor equal to the faster interval blocks
kvm_auto.c @@ else if (since(now, a->beginAt) >= (uint32_t)KVM_AUTO_START_WAIT_MS) { to = 1; } @@ else if (since(now, a->beginAt) >= (uint32_t)KVM_AUTO_START_WAIT_MS) { to = 0; } @@ start: without a round trip the start is 60
kvm_auto.c @@ else if (since(now, a->beginAt) >= (uint32_t)KVM_AUTO_START_WAIT_MS) { to = 1; } @@ else if (since(now, a->beginAt) > (uint32_t)KVM_AUTO_START_WAIT_MS) { to = 1; } @@ start: the wait for the round trip is one millisecond too long
kvm_auto.c @@ if (a->haveEval && since(now, a->lastEvalAt) > (uint32_t)KVM_AUTO_STALE_MS) @@ if (a->haveEval && since(now, a->lastEvalAt) >= (uint32_t)KVM_AUTO_STALE_MS) @@ stale: exactly 30 s apart is no streak
kvm_auto.c @@ if (a->haveEval && since(now, a->lastEvalAt) > (uint32_t)KVM_AUTO_STALE_MS) @@ if (0) @@ stale: a streak across a long silence
kvm_auto.c @@ (!a->haveChange || since(now, a->lastChangeAt) >= (uint32_t)KVM_AUTO_MIN_CHANGE_MS) ? 1 : 0; @@ 1; @@ timing: a step down right after a step
kvm_auto.c @@ (!a->haveChange || since(now, a->lastChangeAt) >= (uint32_t)KVM_AUTO_MIN_CHANGE_MS) ? 1 : 0; @@ (!a->haveChange || since(now, a->lastChangeAt) > (uint32_t)KVM_AUTO_MIN_CHANGE_MS) ? 1 : 0; @@ timing: 2 s between steps is not enough
kvm_auto.c @@ (!a->haveChange || since(now, a->lastChangeAt) >= (uint32_t)KVM_AUTO_UP_AFTER_MS)) @@ (!a->haveChange || since(now, a->lastChangeAt) > (uint32_t)KVM_AUTO_UP_AFTER_MS)) @@ up: 4 s after the previous step is not enough
kvm_auto.c @@ (!a->haveChange || since(now, a->lastChangeAt) >= (uint32_t)KVM_AUTO_UP_AFTER_MS)) @@ (1)) @@ up: an improvement right after a step
kvm_auto.c @@ if (a->haveUp && since(now, a->lastUpAt) <= (uint32_t)KVM_AUTO_FAIL_MS) @@ if (a->haveUp && since(now, a->lastUpAt) < (uint32_t)KVM_AUTO_FAIL_MS) @@ patience: a down exactly 10 s after the up is no failure
kvm_auto.c @@ if (since(env->now, a->winStart) < (uint32_t)KVM_AUTO_WINDOW_MS) { return @@ if (since(env->now, a->winStart) <= (uint32_t)KVM_AUTO_WINDOW_MS) { return @@ tick: the window closes one millisecond late
kvm_auto.c @@ w.key = (uint8_t)((a->keyInWin || key_live(a, env->now)) ? 1 : 0); @@ w.key = (uint8_t)(a->keyInWin ? 1 : 0); @@ key: the windows after the key frame are evaluated
kvm_auto.c @@ w.key = (uint8_t)((a->keyInWin || key_live(a, env->now)) ? 1 : 0); @@ w.key = (uint8_t)(key_live(a, env->now) ? 1 : 0); @@ key: the window of the key frame itself is not frozen
kvm_auto.c @@ if (!a->haveKey || !key_live(a, now) || dms(until, a->keyUntil) > 0) { a->keyUntil = until; } @@ a->keyUntil = until; @@ key: a shorter freeze replaces a longer one
kvm_auto.c @@ if (!a->haveKey || !key_live(a, now) || dms(until, a->keyUntil) > 0) { a->keyUntil = until; } @@ if (!a->haveKey || dms(until, a->keyUntil) > 0) { a->keyUntil = until; } @@ key: a stamp that went round the clock keeps the freeze long
kvm_auto.c @@ return (a->haveKey && rem > 0 && rem <= (int32_t)(KVM_AUTO_KEY_FREEZE_MS + 60000u + 10000u)) ? 1 : 0; @@ return (a->haveKey && rem > 0) ? 1 : 0; @@ key: a stamp 2^31 ms back freezes for 24.9 days
kvm_auto.c @@ if (a->haveKey && !key_live(a, env->now)) { a->haveKey = 0; } @@ /* removed */ @@ key: the stamp is not cleared
kvm_auto.c @@ return d > -(int32_t)KVM_AUTO_FUTURE_MS ? 0u : 0x7FFFFFFFu; @@ return 0u; @@ clock: a stamp that went round is a stamp of the future
kvm_auto.c @@ return d > -(int32_t)KVM_AUTO_FUTURE_MS ? 0u : 0x7FFFFFFFu; @@ return 0x7FFFFFFFu; @@ clock: a late note is long ago
kvm_auto.c @@ a->step == a->upStep && since(now, a->lastUpAt) >= (uint32_t)KVM_AUTO_STABLE_MS) @@ a->step == a->upStep && since(now, a->lastUpAt) > (uint32_t)KVM_AUTO_STABLE_MS) @@ patience: 60 s on the faster step is one millisecond too long
kvm_auto.c @@ if (a->haveUp && a->step == a->upStep && since( @@ if (a->haveUp && since( @@ patience: a step up that was left again counts as held
kvm_auto.c @@ if (a->haveUp && a->step == a->upStep && since(now, a->lastUpAt) >= (uint32_t)KVM_AUTO_STABLE_MS) { a->upNeed = (uint32_t)KVM_AUTO_UP_WINDOWS; a->haveUp = 0; } @@ if (a->haveChange && since(now, a->lastChangeAt) >= (uint32_t)KVM_AUTO_STABLE_MS) { a->upNeed = (uint32_t)KVM_AUTO_UP_WINDOWS; } @@ patience: 60 s without a change gives the first patience back (the old rule)
kvm_auto.c @@ if (a->haveUp && a->step == a->upStep && since(now, a->lastUpAt) >= (uint32_t)KVM_AUTO_STABLE_MS) { a->upNeed = (uint32_t)KVM_AUTO_UP_WINDOWS; a->haveUp = 0; } @@ if (0) { a->upNeed = (uint32_t)KVM_AUTO_UP_WINDOWS; a->haveUp = 0; } @@ patience: never back to the first one
kvm_auto.c @@ w.key = (uint8_t)((a->keyInWin @@ w.key = (uint8_t)((0 && a->keyInWin @@ key: the key frame in the window is not noted
kvm_auto.c @@ a->motionAcc += gap <= (uint32_t)KVM_AUTO_MOTION_GAP_MS ? gap : (uint32_t)kMs[clamp_step(a->step)]; @@ a->motionAcc += gap < (uint32_t)KVM_AUTO_MOTION_GAP_MS ? gap : (uint32_t)kMs[clamp_step(a->step)]; @@ motion: a gap of exactly 250 ms starts a series
kvm_auto.c @@ a->motionAcc += gap <= (uint32_t)KVM_AUTO_MOTION_GAP_MS ? gap : (uint32_t)kMs[clamp_step(a->step)]; @@ a->motionAcc += gap; @@ motion: a long gap is motion
kvm_auto.c @@ a->motionAcc += gap <= (uint32_t)KVM_AUTO_MOTION_GAP_MS ? gap : (uint32_t)kMs[clamp_step(a->step)]; @@ a->motionAcc += (uint32_t)kMs[clamp_step(a->step)]; @@ motion: every image is a step (the slots of old)
kvm_auto.c @@ a->motionAcc += gap <= (uint32_t)KVM_AUTO_MOTION_GAP_MS ? gap : (uint32_t)kMs[clamp_step(a->step)]; @@ a->motionAcc += gap <= (uint32_t)KVM_AUTO_MOTION_GAP_MS ? gap : 0u; @@ motion: the first image after a long gap is no motion
kvm_auto.c @@ 	else { a->motionAcc += (uint32_t)kMs[clamp_step(a->step)]; } @@ 	else { a->motionAcc += 0u; } @@ motion: the first image of a series is no motion
kvm_auto.c @@ 	w.motionMs = a->motionAcc; @@ 	w.motionMs = a->motionAcc + (uint32_t)KVM_AUTO_MOTION_GAP_MS / 10u; @@ motion: the time of the window is too long
kvm_auto.c @@ a->haveWin = 0; a->motionAcc = 0; a->haveMotion = 0; a->keyInWin = 0; a->haveKey = 0; @@ a->haveWin = 0; a->keyInWin = 0; a->haveKey = 0; @@ resume: the series and the time of motion stay
kvm_auto.c @@ a->started = 0; a->step = 1; a->beginAt = now; @@ a->started = 0; a->beginAt = now; @@ resume: the step stays 60 until the start
kvm_auto.c @@ return (a->on && a->started) ? (uint32_t)kMs[clamp_step(a->step)] : 0u; } @@ return a->on ? (uint32_t)kMs[clamp_step(a->step)] : 0u; } @@ the provisional step is an interval before the start
kvm_auto.c @@ if (gap > (uint32_t)KVM_AUTO_STALE_MS)\n	{ @@ if (gap >= (uint32_t)KVM_AUTO_STALE_MS)\n	{ @@ resume: exactly 30 s is a long pause
kvm_auto.c @@ if (gap > (uint32_t)KVM_AUTO_STALE_MS)\n	{ @@ if (0)\n	{ @@ resume: a long pause keeps the step
kvm_auto.c @@ uint32_t kvm_auto_interval_ms(const kvm_auto *a) { return (a->on && a->started) ? (uint32_t)kMs[clamp_step(a->step)] : 0u; } @@ uint32_t kvm_auto_interval_ms(const kvm_auto *a) { return (uint32_t)kMs[clamp_step(a->step)]; } @@ off: the interval is not zero with the bit down
kvm_auto.c @@ a->motionAcc += gap <= (uint32_t)KVM_AUTO_MOTION_GAP_MS ? gap : (uint32_t)kMs[clamp_step(a->step)]; @@ a->motionAcc += gap <= (uint32_t)KVM_AUTO_MOTION_GAP_MS ? gap : (uint32_t)kMs[clamp_step(a->step)] * 2u; @@ motion: a new series is two steps
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
  if ! gcc -std=c99 -O1 -w -o /tmp/m/t /tmp/m/tests/kvm_auto_test.c /tmp/m/kvm_auto.c 2>/tmp/m/err; then echo "INVALID  ($desc): does not compile"; invalid=$((invalid + 1)); continue; fi
  if /tmp/m/t >/tmp/m/out 2>&1; then echo "SURVIVED ($desc)"; survived=$((survived + 1)); else echo "killed   ($desc): $(grep -m1 FAIL /tmp/m/out | cut -c1-110)"; killed=$((killed + 1)); fi
done < /tmp/mutations.txt
echo "mutations: $n, killed $killed, survived $survived, invalid $invalid"
[ $survived -eq 0 ] && [ $invalid -eq 0 ]
' sh
