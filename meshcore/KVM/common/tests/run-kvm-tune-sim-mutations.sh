#!/bin/sh
# Mutation check of the simulation of the smoothness work (kvm_tune_sim_test.c, the build against the tree: the criteria for Docker): each line below changes one key rule of the code in a copy, and the tests have to turn
# red (the test program exits with a failure); a mutation that does not compile is reported as invalid, one that the tests do not notice as SURVIVED (exit status 1 at the end).
# Usage: run-kvm-tune-sim-mutations.sh
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
kvm_motion.c @@ if (near_motion(m, now, row, col)) { m->sNear++; return 1; } @@ if (0) { m->sNear++; return 1; } @@ the first change near a moving tile is JPEG (the edge of a dragged window)
kvm_stream.c @@ if (s->rateMem == 0) { return 0; } @@ return 0; @@ the rate is not remembered through a pause
kvm_stream.c @@ if (age < (int32_t)KVM_STREAM_MEM_HOLD_MS) { return s->rateMem; } @@ if (age < (int32_t)KVM_STREAM_MEM_HOLD_MS) { return s->rateMem / 2u; } @@ the memory holds half of the rate
kvm_motion.c @@ if (streak >= m->streakMin && sinceMs <= (int32_t)m->recentMs && kvm_motion_recent(m, now)) @@ if (0) @@ a tile that changed twice in a row is not motion
kvm_motion.c @@ if (realChanged >= m->tilesMin) { m->frameAt = frameAt ? frameAt : 1u; } @@ if (realChanged >= m->tilesMin) { m->frameAt = 1u; } @@ a frame of motion is never recent
kvm_sharpen.c @@ g->activeMs = ms < (uint32_t)KVM_SHARP_ACTIVE_MIN_MS ? (uint32_t)KVM_SHARP_ACTIVE_MIN_MS : ms; @@ g->activeMs = 4000u; @@ a movement lasts four seconds (the sharpening waits for the settle)
kvm_motion.c @@ if (a != 0 && dms(now, a) < (int32_t)m->recallMs) { m->sRecall++; return 1; } @@ if (0) { m->sRecall++; return 1; } @@ the memory of the place is not looked at (the first frame of a series is a JPEG of 300 KB)
kvm_motion.c @@ return (sourceChange && !crcIsReset) ? 1 : 0; @@ return 0; @@ the safety net never catches a tile (the picture after a change of the source is sent again)
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
  if ! gcc -std=c99 -O1 -w -o /tmp/m/t /tmp/m/tests/kvm_tune_sim_test.c /tmp/m/kvm_pace.c /tmp/m/kvm_video_rc.c /tmp/m/kvm_video_core.c /tmp/m/kvm_stream.c /tmp/m/kvm_annexb.c /tmp/m/kvm_sharpen.c /tmp/m/kvm_motion.c -lm 2>/tmp/m/err; then echo "INVALID  ($desc): does not compile"; invalid=$((invalid + 1)); continue; fi
  if /tmp/m/t >/tmp/m/out 2>&1; then echo "SURVIVED ($desc)"; survived=$((survived + 1)); else echo "killed   ($desc): $(grep -m1 FAIL /tmp/m/out | cut -c1-110)"; killed=$((killed + 1)); fi
done < /tmp/mutations.txt
echo "mutations: $n, killed $killed, survived $survived, invalid $invalid"
[ $survived -eq 0 ] && [ $invalid -eq 0 ]
' sh
