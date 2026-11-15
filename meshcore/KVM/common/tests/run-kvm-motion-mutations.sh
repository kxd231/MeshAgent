#!/bin/sh
# Mutation check of the tests of kvm_motion.c: each line below changes one key rule of the code in a copy, and the tests have to turn
# red (the test program exits with a failure); a mutation that does not compile is reported as invalid, one that the tests do not notice as SURVIVED (exit status 1 at the end).
# Usage: run-kvm-motion-mutations.sh
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
kvm_motion.c @@ if (realChanged >= m->tilesMin) @@ if (realChanged > m->tilesMin) @@ a frame of 16 changes is not a frame of motion
kvm_motion.c @@ return m->frameAt != 0 && dms(now, m->frameAt) <= (int32_t)m->recentMs; @@ return m->frameAt != 0 && dms(now, m->frameAt) < (int32_t)m->recentMs; @@ a frame of motion counts one millisecond too short
kvm_motion.c @@ return m->frameAt != 0 && dms(now, m->frameAt) <= (int32_t)m->recentMs; @@ return dms(now, m->frameAt) <= (int32_t)m->recentMs; @@ a session without a frame of motion is motion
kvm_motion.c @@ return m->frameAt != 0 && dms(now, m->frameAt) <= (int32_t)m->recentMs; @@ return m->frameAt != 0 && (uint32_t)(now - m->frameAt) <= m->recentMs; @@ the frame of this iteration (a stamp after the clock) is not recent
kvm_motion.c @@ if (sourceChange) { return 0; }	// the picture of another source, not a change of the content @@ ; @@ a change of the source of the picture is a change of the content
kvm_motion.c @@ return (streak > 0 && sinceMs <= (int32_t)m->recentMs) ? (streak < 255u ? streak + 1u : 255u) : 1u; @@ return (streak > 0) ? (streak < 255u ? streak + 1u : 255u) : 1u; @@ the streak never ends
kvm_motion.c @@ return (streak > 0 && sinceMs <= (int32_t)m->recentMs) ? (streak < 255u ? streak + 1u : 255u) : 1u; @@ return (streak > 0 && sinceMs < (int32_t)m->recentMs) ? (streak < 255u ? streak + 1u : 255u) : 1u; @@ a change 250 ms after the previous one ends the streak
kvm_motion.c @@ return (streak > 0 && sinceMs <= (int32_t)m->recentMs) ? (streak < 255u ? streak + 1u : 255u) : 1u; @@ return (streak > 0 && sinceMs <= (int32_t)m->recentMs) ? streak + 1u : 1u; @@ the streak counter wraps (unsigned char in the tile)
kvm_motion.c @@ if (streak >= m->streakMin && sinceMs <= (int32_t)m->recentMs && kvm_motion_recent(m, now)) @@ if (streak >= 1 && sinceMs <= (int32_t)m->recentMs && kvm_motion_recent(m, now)) @@ the first change of a tile is motion (typing goes to the video)
kvm_motion.c @@ if (streak >= m->streakMin && sinceMs <= (int32_t)m->recentMs && kvm_motion_recent(m, now)) @@ if (streak >= m->streakMin && sinceMs <= (int32_t)m->recentMs) @@ two changes in a row are motion without a frame of motion (a text field)
kvm_motion.c @@ if (streak >= m->streakMin && sinceMs <= (int32_t)m->recentMs && kvm_motion_recent(m, now)) @@ if (streak >= m->streakMin && kvm_motion_recent(m, now)) @@ a change that is too old to be a streak is motion
kvm_motion.c @@ r0 = row - m->radius; r1 = row + m->radius; c0 = col - m->radius; c1 = col + m->radius; @@ r0 = row - 1; r1 = row + m->radius; c0 = col - m->radius; c1 = col + m->radius; @@ the near rule does not look two tiles up
kvm_motion.c @@ r0 = row - m->radius; r1 = row + m->radius; c0 = col - m->radius; c1 = col + m->radius; @@ r0 = row - m->radius; r1 = row + m->radius; c0 = col - m->radius; c1 = col + m->radius + 1; @@ the near rule looks three tiles to the right
kvm_motion.c @@ r0 = row - m->radius; r1 = row + m->radius; c0 = col - m->radius; c1 = col + m->radius; @@ r0 = row - m->radius; r1 = row + 1; c0 = col - m->radius; c1 = col + m->radius; @@ the near rule does not look two tiles down
kvm_motion.c @@ if (line[c] != 0 && dms(now, line[c]) <= (int32_t)m->recentMs) { return 1; } @@ if (line[c] != 0) { return 1; } @@ a stamp never expires
kvm_motion.c @@ if (line[c] != 0 && dms(now, line[c]) <= (int32_t)m->recentMs) { return 1; } @@ if (line[c] != 0 && dms(now, line[c]) < (int32_t)m->recentMs) { return 1; } @@ a stamp expires one millisecond early
kvm_motion.c @@ if (near_motion(m, now, row, col)) { m->sNear++; return 1; } @@ if (near_motion(m, now, row, col)) { if (m->at != NULL) { m->at[(size_t)row * (size_t)m->cols + (size_t)col] = now ? now : 1u; } m->sNear++; return 1; } @@ a tile taken by the near rule stamps too (typing beside a video spreads)
kvm_motion.c @@ if (m->at != NULL && row >= 0 && col >= 0 && row < m->rows && col < m->cols) { m->at[(size_t)row * (size_t)m->cols + (size_t)col] = now ? now : 1u; } @@ ; @@ a tile of the streak rule leaves no stamp
kvm_motion.c @@ if (m->at == NULL || m->radius <= 0) { return 0; } @@ if (m->at == NULL) { return 0; } @@ a radius of 0 still looks around (the tile itself)
kvm_motion.c @@ if (m->at != NULL && m->rows == rows && m->cols == cols) { memset(m->at, 0, (size_t)rows * (size_t)cols * sizeof(uint32_t)); return 0; } @@ if (m->at != NULL && m->rows == rows && m->cols == cols) { return 0; } @@ a new grid keeps the stamps of the old one
kvm_motion.c @@ if (m->at != NULL) { memset(m->at, 0, (size_t)m->rows * (size_t)m->cols * sizeof(uint32_t)); } @@ ; @@ a reset keeps the stamps
kvm_motion.c @@ m->frameAt = 0;\n} @@ \n} @@ a reset keeps the frame of motion
kvm_motion.c @@ if (rows <= 0 || cols <= 0 || (size_t)rows > 4096u || (size_t)cols > 4096u) @@ if (rows <= 0 || cols <= 0) @@ the size of the map is not limited
kvm_motion.c @@ if (g >= (int32_t)KVM_SERIES_MERGE_MS && g <= (int32_t)KVM_SERIES_GAP_MAX_MS) { gap_note(s, (uint32_t)g, intervalMs); } @@ if (g >= 0 && g <= (int32_t)KVM_SERIES_GAP_MAX_MS) { gap_note(s, (uint32_t)g, intervalMs); } @@ the writes of one iteration are gaps
kvm_motion.c @@ if (g >= (int32_t)KVM_SERIES_MERGE_MS && g <= (int32_t)KVM_SERIES_GAP_MAX_MS) { gap_note(s, (uint32_t)g, intervalMs); } @@ if (g >= (int32_t)KVM_SERIES_MERGE_MS) { gap_note(s, (uint32_t)g, intervalMs); } @@ a pause inside a series is a gap
kvm_motion.c @@ if (intervalMs > 0 && g > 2u * intervalMs) { s->over2x++; } @@ if (intervalMs > 0 && g > intervalMs) { s->over2x++; } @@ a gap of more than one interval is a stall
kvm_motion.c @@ if (video && !s->haveVideo) @@ if (video) @@ every video frame is the start of the video
kvm_motion.c @@ if (s->inSeries && !s->haveVideo) { s->noVideo++; } @@ ; @@ a series without a video frame is not counted
kvm_motion.c @@ if (s->inSeries && dms(now, s->lastMotion) > (int32_t)KVM_SERIES_QUIET_MS) { series_close(s); } @@ ; @@ a series never ends
kvm_motion.c @@ need = (s->nGaps * 95u + 99u) / 100u; @@ need = (s->nGaps * 50u + 99u) / 100u; @@ the P95 is the median
kvm_motion.c @@ pct = paints > 0 ? (s->paintsJpeg * 100u) / paints : 0; @@ pct = paints > 0 ? (s->paintsVideo * 100u) / paints : 0; @@ the share of JPEG is the share of the video
kvm_motion.c @@ if (rKbps > 0 && rKbps < s->rStartMin) @@ if (rKbps < s->rStartMin) @@ no video (R 0) is the lowest R of a series
kvm_motion.c @@ s->haveWrite = 1; s->lastWrite = now; @@ s->haveWrite = 1; @@ the gap is measured from the first write of the series
kvm_motion.c @@ if (!s->inSeries) { return; } @@ ; @@ a picture outside a series is counted
kvm_motion.c @@ if (s->nSeries == 0 && paints == 0) @@ if (0) @@ a line is written when nothing happened
kvm_motion.c @@ return n > 0 && (size_t)n < cap ? (size_t)n : 0u; @@ return n > 0 ? (size_t)n : 0u; @@ the length of a truncated line is the length that did not fit
kvm_motion.c @@ if (a != 0 && dms(now, a) < (int32_t)m->recallMs) { m->sRecall++; return 1; } @@ if (0) { m->sRecall++; return 1; } @@ the memory of the place is not looked at (the first frame of a series is JPEG)
kvm_motion.c @@ if (a != 0 && dms(now, a) < (int32_t)m->recallMs) { m->sRecall++; return 1; } @@ if (a != 0 && dms(now, a) <= (int32_t)m->recallMs) { m->sRecall++; return 1; } @@ the memory of the place lasts a millisecond too long
kvm_motion.c @@ if (a != 0 && dms(now, a) < (int32_t)m->recallMs) { m->sRecall++; return 1; } @@ if (dms(now, a) < (int32_t)m->recallMs) { m->sRecall++; return 1; } @@ a tile that never moved is remembered as moving
kvm_motion.c @@ return (sourceChange && !crcIsReset) ? 1 : 0; @@ return sourceChange ? 1 : 0; @@ a tile that the viewer has never been given is absorbed (it is never sent)
kvm_motion.c @@ return (sourceChange && !crcIsReset) ? 1 : 0; @@ return 0; @@ the safety net never catches a tile
kvm_motion.c @@ v &= 0x00FFFFFFu; @@ v &= 0xFF00FFFFu; @@ the alpha byte is not the one that is cleared
kvm_motion.c @@ for (i = 0; i + 4 <= bytes; i += 4) @@ for (i = 0; i + 8 <= bytes; i += 4) @@ the last pixel keeps its alpha
kvm_motion.c @@ if (a != 0 && dms(now, a) < (int32_t)m->recallMs) { m->sRecall++; return 1; } @@ if (a != 0 && dms(now, a) < (int32_t)m->recallMs) { m->sRecall++; m->at[(size_t)row * (size_t)m->cols + (size_t)col] = now; return 1; } @@ a tile taken by the memory stamps (the area spreads for ever)
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
  if ! gcc -std=c99 -O1 -w -o /tmp/m/t /tmp/m/tests/kvm_motion_test.c /tmp/m/kvm_motion.c 2>/tmp/m/err; then echo "INVALID  ($desc): does not compile"; invalid=$((invalid + 1)); continue; fi
  if /tmp/m/t >/tmp/m/out 2>&1; then echo "SURVIVED ($desc)"; survived=$((survived + 1)); else echo "killed   ($desc): $(grep -m1 FAIL /tmp/m/out | cut -c1-110)"; killed=$((killed + 1)); fi
done < /tmp/mutations.txt
echo "mutations: $n, killed $killed, survived $survived, invalid $invalid"
[ $survived -eq 0 ] && [ $invalid -eq 0 ]
' sh
