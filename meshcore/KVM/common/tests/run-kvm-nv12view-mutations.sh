#!/bin/sh
# Mutation check of the tests of kvm_nv12view.c (the plan and the apply of the NV12 view): each line below changes one rule of the code in a copy, and the tests have to turn red (the test program
# exits with a failure); a mutation that does not compile is reported as invalid, one that the tests do not notice as SURVIVED (exit status 1 at the end).
# Line format: file @@ text @@ replacement @@ description. Usage: run-kvm-nv12view-mutations.sh
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
kvm_nv12view.c @@ (c > 0 && b[c - 1]) ||  @@  @@ the ring does not reach to the left
kvm_nv12view.c @@ || (c + 1 < C && b[c + 1]) @@  @@ the ring does not reach to the right
kvm_nv12view.c @@ (r > 0 && tmp[(size_t)(r - 1) * C + c]) || @@  @@ the ring does not reach upwards
kvm_nv12view.c @@ || (r + 1 < R && tmp[(size_t)(r + 1) * C + c]) @@  @@ the ring does not reach downwards
kvm_nv12view.c @@ if (!v->valid && !(isKey && v->flat && !v->firstFull)) { goto complete; } @@ if (0) { goto complete; } @@ a P frame on a view that holds no picture is partial
kvm_nv12view.c @@ if (isKey && (!v->flat || (v->firstFull && !v->sessionIdr))) { goto complete; } @@ if (isKey && (!v->flat)) { goto complete; } @@ the first key frame of the session is not complete although it has to be
kvm_nv12view.c @@ if (isKey && (!v->flat || (v->firstFull && !v->sessionIdr))) { goto complete; } @@ if (isKey && (v->firstFull && !v->sessionIdr)) { goto complete; } @@ a key frame without FLAT is partial
kvm_nv12view.c @@ (st != NULL && st[k] == KVM_NV12V_VIDEO) ||  @@  @@ the tiles shown from the video are not converted at a key frame
kvm_nv12view.c @@ || (fl != NULL && fl[k] != 0) @@  @@ the tiles in flight are not converted at a key frame
kvm_nv12view.c @@ if (cover[k] || (isKey @@ if (0 || (isKey @@ the tiles of the frame are not converted
kvm_nv12view.c @@ memcpy(v->touched, v->map, v->total); @@  @@ a flat key frame leaves what was touched
kvm_nv12view.c @@ if (v->map[i] && !v->touched[i]) { virgin++; v->touched[i] = 1; } @@ if (v->map[i] && !v->touched[i]) { virgin++; } @@ the same macroblocks stay virgin
kvm_nv12view.c @@ if (v->map[i] && !v->touched[i]) { virgin++; v->touched[i] = 1; } @@ if (v->map[i]) { virgin++; v->touched[i] = 1; } @@ the macroblocks that were touched are virgin again
kvm_nv12view.c @@ memset(v->touched, 1, v->total);\n	v->valid = 1; @@ v->valid = 1; @@ a complete conversion does not mark the view as touched
kvm_nv12view.c @@ \tv->valid = 0;\n @@  @@ invalidate does not invalidate
kvm_nv12view.c @@ if (full) { return kvm_yuv_bgra_to_nv12 @@ if (0) { return kvm_yuv_bgra_to_nv12 @@ a complete conversion is partial
kvm_nv12view.c @@ \tif (flat)\n\t{\n\t\tint b = 0 @@ \tif (0)\n\t{\n\t\tint b = 0 @@ a flat key frame does not make the view flat
kvm_nv12view.c @@ for (c1 = c; c1 < mbCols && row[c1]; c1++) {} @@ for (c1 = c; c1 < mbCols - 1 && row[c1]; c1++) {} @@ the last macroblock of a row is never converted
kvm_nv12view.c @@ if (v->total > KVM_NV12V_MB_MAX) { @@ if (0) { @@ a picture that does not fit in a job is planned
kvm_nv12view.c @@ if (map == NULL || mbCols != codedW / KVM_NV12V_MB || mbRows != codedH / KVM_NV12V_MB) { return -1; } @@ if (map == NULL) { return -1; } @@ a map of another grid is applied
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
  S="$search" R="$replace" perl -0pi -e "BEGIN { \$s = \$ENV{S}; \$r = \$ENV{R}; \$s =~ s/\\\\n/\\n/g; \$r =~ s/\\\\n/\\n/g; \$s =~ s/\\\\t/\\t/g; \$r =~ s/\\\\t/\\t/g; } \$c += s/\\Q\$s\\E/\$r/g; END { exit(\$c >= 1 ? 0 : 3) }" /tmp/m/$file
  rc=$?
  if [ $rc -ne 0 ]; then echo "INVALID  ($desc): the text was not found"; invalid=$((invalid + 1)); continue; fi
  if ! gcc -std=c99 -O1 -w -o /tmp/m/t /tmp/m/tests/kvm_nv12view_test.c /tmp/m/kvm_nv12view.c /tmp/m/kvm_yuv.c -lm 2>/tmp/m/err; then echo "INVALID  ($desc): does not compile"; invalid=$((invalid + 1)); continue; fi
  if timeout 120 /tmp/m/t >/tmp/m/out 2>&1; then echo "SURVIVED ($desc)"; survived=$((survived + 1)); else echo "killed   ($desc): $(grep -m1 FAIL /tmp/m/out | cut -c1-110)"; killed=$((killed + 1)); fi
done < /tmp/mutations.txt
echo "mutations: $n, killed $killed, survived $survived, invalid $invalid"
[ $survived -eq 0 ] && [ $invalid -eq 0 ]
' sh
