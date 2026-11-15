#!/bin/sh
# Mutation check of the tests of kvm_sharpen.c: each line below changes one key rule of the code in a copy, and the tests have to turn
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
kvm_sharpen.h @@ return (int32_t)(now - lastChange) >= (int32_t)settleMs; @@ return (uint32_t)(now - lastChange) >= settleMs; @@ the settle rule is unsigned again (a tile painted after the clock of the iteration counts as still)
kvm_sharpen.h @@ return (int32_t)(now - then); } @@ return (int32_t)(then - now); } @@ the elapsed time has the wrong sign
kvm_sharpen.h @@ (int32_t)(now - lastChange) >= (int32_t)settleMs; @@ (int32_t)(now - lastChange) > (int32_t)settleMs; @@ a tile is settled one millisecond later than the settle time
kvm_sharpen.c @@ return g->haveMotion && dms(now, g->lastMotion) < (int32_t)(g->activeMs != 0 ? g->activeMs : (uint32_t)KVM_SHARP_ACTIVE_MS); @@ return g->haveMotion && dms(now, g->lastMotion) <= (int32_t)(g->activeMs != 0 ? g->activeMs : (uint32_t)KVM_SHARP_ACTIVE_MS); @@ a movement lasts one millisecond too long
kvm_sharpen.c @@ return g->haveMotion && dms(now, g->lastMotion) < (int32_t)(g->activeMs @@ return dms(now, g->lastMotion) < (int32_t)(g->activeMs @@ a session without a frame is a movement
kvm_sharpen.c @@ if (g->tokens <= 0) { g->sBucketRefused++; allowed = 0; } @@ if (0) { g->sBucketRefused++; allowed = 0; } @@ an empty bucket refuses nothing
kvm_sharpen.c @@ if (allowed > g->tokens) { allowed = g->tokens; } @@ ; @@ the budget of an iteration ignores the bucket
kvm_sharpen.c @@ if (allowed > cap) { allowed = cap; g->sIterCapped++; } @@ ; @@ an iteration may send more than a quarter of the window
kvm_sharpen.c @@ if (window > 0) @@ if (0) @@ the quarter of the window is never looked at
kvm_sharpen.c @@ g->tokens += (share_of(rate) * dt) / 1000; @@ g->tokens += 0; @@ the bucket is never refilled
kvm_sharpen.c @@ g->tokens += (share_of(rate) * dt) / 1000; @@ g->tokens += (share_of(rate) * dt) / 100; @@ the bucket refills ten times too fast
kvm_sharpen.c @@ if (g->tokens > cap) { g->tokens = cap; } @@ ; @@ the bucket has no burst limit
kvm_sharpen.c @@ return b < KVM_SHARP_BURST_MIN ? KVM_SHARP_BURST_MIN : b; @@ return b; @@ no minimum burst (one packet would not fit at a low rate)
kvm_sharpen.c @@ if (rate == 0) { rate = KVM_SHARP_DEFAULT_RATE; } @@ ; @@ an unknown rate gives no share at all
kvm_sharpen.c @@ if (inflight * 100u < window * KVM_SHARP_PACKET_WINDOW_PCT) { return 1; } @@ if (inflight * 100u <= window * KVM_SHARP_PACKET_WINDOW_PCT) { return 1; } @@ the window gate lets a packet go at exactly half of W
kvm_sharpen.c @@ if (inflight * 100u < window * KVM_SHARP_PACKET_WINDOW_PCT) { return 1; } @@ if (inflight * 100u < window * 100u) { return 1; } @@ the window gate is W, not W / 2
kvm_sharpen.c @@ if (window == 0) { return 1; } @@ ; @@ without a window nothing may be sent
kvm_sharpen.c @@ g->iterActive = (uint8_t)(kvm_sharp_active(g, now) ? 1 : 0); @@ g->iterActive = 1; @@ the sharpening is limited when the video is still (the picture would converge at the speed of the share)
kvm_sharpen.c @@ g->iterActive = (uint8_t)(kvm_sharp_active(g, now) ? 1 : 0); @@ g->iterActive = 0; @@ the sharpening is never limited
kvm_sharpen.c @@ if (g->tokens <= 0) @@ if (g->tokens < 0) @@ an empty bucket (exactly 0) still sends
kvm_sharpen.c @@ 		g->tokens -= bytes; @@ 		; @@ a sent packet is not charged to the bucket
kvm_sharpen.c @@ (void)now;\n	if (bytes <= 0) { return; }\n	g->tokens -= bytes; @@ (void)now;\n	if (bytes <= 0) { return; } @@ a forced packet does not pay
kvm_sharpen.c @@ if (g->tokens < -2 * g->cap) { g->tokens = -2 * g->cap; } @@ ; @@ the debt of the bucket is not limited
kvm_sharpen.h @@ return stamp != 0 && (int32_t)(now - stamp) < (int32_t)ms; @@ return stamp != 0 && (uint32_t)(now - stamp) < ms; @@ an input stamped after the clock was read counts as no recent input
kvm_sharpen.h @@ return stamp != 0 && (int32_t)(now - stamp) < (int32_t)ms; @@ return (int32_t)(now - stamp) < (int32_t)ms; @@ a stamp of 0 (never) counts as recent
kvm_sharpen.h @@ return stamp != 0 && (int32_t)(now - stamp) < (int32_t)ms; @@ return stamp != 0 && (int32_t)(now - stamp) <= (int32_t)ms; @@ the recent window is one millisecond longer
kvm_sharpen.c @@ return g->haveMotion && dms(now, g->lastMotion) < (int32_t)(g->activeMs != 0 ? g->activeMs : (uint32_t)KVM_SHARP_ACTIVE_MS); @@ return g->haveMotion && dms(now, g->lastMotion) < (int32_t)KVM_SHARP_ACTIVE_MS; @@ the movement lasts 300 ms whatever the interval
kvm_sharpen.c @@ g->activeMs = ms < (uint32_t)KVM_SHARP_ACTIVE_MIN_MS ? (uint32_t)KVM_SHARP_ACTIVE_MIN_MS : ms; @@ g->activeMs = ms; @@ no floor of 100 ms on the movement
kvm_sharpen.c @@ uint32_t ms = intervalMs * (uint32_t)KVM_SHARP_ACTIVE_FRAMES; @@ uint32_t ms = intervalMs * 2u; @@ the movement is two intervals
kvm_sharpen.c @@ if (intervalMs > 1000u) { ms = 1000u * (uint32_t)KVM_SHARP_ACTIVE_FRAMES; } @@ ; @@ a huge interval overflows the movement time
kvm_sharpen.c @@ return g->haveMotion && dms(now, g->lastMotion) < (int32_t)KVM_SHARP_POST_MS; @@ return dms(now, g->lastMotion) < (int32_t)KVM_SHARP_POST_MS; @@ a session without a movement is a movement (the quarter gate for ever)
kvm_sharpen.c @@ return g->haveMotion && dms(now, g->lastMotion) < (int32_t)KVM_SHARP_POST_MS; @@ return g->haveMotion && dms(now, g->lastMotion) <= (int32_t)KVM_SHARP_POST_MS; @@ the second after a movement is one millisecond longer
kvm_sharpen.c @@ const unsigned pct = kvm_sharp_post_motion(g, now) ? (unsigned)KVM_SHARP_POST_WINDOW_PCT : (unsigned)KVM_SHARP_PACKET_WINDOW_PCT; @@ const unsigned pct = (unsigned)KVM_SHARP_PACKET_WINDOW_PCT; @@ the gate after a movement is the half
kvm_sharpen.c @@ const unsigned pct = kvm_sharp_post_motion(g, now) ? (unsigned)KVM_SHARP_POST_WINDOW_PCT : (unsigned)KVM_SHARP_PACKET_WINDOW_PCT; @@ const unsigned pct = kvm_sharp_post_motion(g, now) ? (unsigned)KVM_SHARP_POST_WINDOW_PCT : 100u; @@ the gate outside a movement is the whole window
kvm_sharpen.c @@ if (inflight * 100u < window * pct) { return 1; }\n	g->sWindowRefused++; @@ if (inflight * 100u <= window * pct) { return 1; }\n	g->sWindowRefused++; @@ the gate lets a packet go at exactly a quarter of W
kvm_sharpen.c @@ if (inflight * 100u < window * pct) { return 1; }\n	g->sWindowRefused++; @@ if (inflight * 100u < window * pct) { return 1; }\n	; @@ a refusal of the gate after a movement is not counted
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
  if ! gcc -std=c99 -O1 -w -o /tmp/m/t /tmp/m/tests/kvm_sharpen_test.c /tmp/m/kvm_sharpen.c -lm 2>/tmp/m/err; then echo "INVALID  ($desc): does not compile"; invalid=$((invalid + 1)); continue; fi
  if /tmp/m/t >/tmp/m/out 2>&1; then echo "SURVIVED ($desc)"; survived=$((survived + 1)); else echo "killed   ($desc): $(grep -m1 FAIL /tmp/m/out | cut -c1-110)"; killed=$((killed + 1)); fi
done < /tmp/mutations.txt
echo "mutations: $n, killed $killed, survived $survived, invalid $invalid"
[ $survived -eq 0 ] && [ $invalid -eq 0 ]
' sh
