#!/bin/sh
# Mutation check of the tests of kvm_yuv.c (the regions, the flat fill, the dominant colour): each line below changes one rule of the code in a copy, and the tests have to turn red (the test program
# exits with a failure); a mutation that does not compile is reported as invalid, one that the tests do not notice as SURVIVED (exit status 1 at the end).
# Line format: file @@ text @@ replacement @@ description. Usage: run-kvm-yuv-mutations.sh
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
kvm_yuv.c @@ const int ra = (2 * j < height) ? 2 * j : 2 * ((height - 1) / 2); @@ const int ra = (2 * j < height) ? 2 * j : 2 * ((height - 1) / 2) - 2; @@ the chroma below the source is not the chroma of the last row pair
kvm_yuv.c @@ const int rb = (ra + 1 < height) ? ra + 1 : height - 1; @@ const int rb = (ra + 1 < height) ? ra + 1 : height - 2; @@ an odd height does not repeat the last row for the chroma
kvm_yuv.c @@ y_row_sse2(r0 + (size_t)x * 4, d + x, n16); @@ y_row_sse2(r0 + (size_t)x * 4, d + x + 2, n16); @@ the SSE2 luma of a region is written two bytes off
kvm_yuv.c @@ uv_row_sse2(r0 + (size_t)x * 4, r1 + (size_t)x * 4, d + x, n16); @@ uv_row_sse2(r0 + (size_t)x * 4, r1 + (size_t)x * 4, d + x / 2, n16); @@ the SSE2 chroma of a region goes to the wrong column
kvm_yuv.c @@ for (; x < xe; x++) { d[x] = y_pix(r0, width, x); } @@ for (; x < xe - 1; x++) { d[x] = y_pix(r0, width, x); } @@ the last luma column of a region is not written
kvm_yuv.c @@ for (i = x / 2; i < xe / 2; i++) { uint8_t cb, cr; @@ for (i = x / 2; i < xe / 2 - 1; i++) { uint8_t cb, cr; @@ the last chroma sample of a region is not written
kvm_yuv.c @@ if (i >= nreal) { i = nreal - 1; } @@ if (i >= nreal) { i = nreal - 2 < 0 ? 0 : nreal - 2; } @@ the chroma beyond the source does not repeat the last sample
kvm_yuv.c @@ const uint8_t *p = src + (size_t)(x < width ? x : width - 1) * 4; @@ const uint8_t *p = src + (size_t)(x < width ? x : width - 2 < 0 ? 0 : width - 2) * 4; @@ the luma beyond the source does not repeat the last pixel
kvm_yuv.c @@ nv12[n + i] = cb; nv12[n + i + 1] = cr; @@ nv12[n + i] = cr; nv12[n + i + 1] = cb; @@ the flat fill swaps Cb and Cr
kvm_yuv.c @@ memset(nv12, y, n); @@ memset(nv12, 0, n); @@ the flat fill leaves the luma black
kvm_yuv.c @@ *cb = (uint8_t)((UB * 4 * b + UG * 4 * g + UR * 4 * r + C_OFF) >> 17); @@ *cb = (uint8_t)((UB * b + UG * g + UR * r + C_OFF) >> 17); @@ the colour of a flat picture counts one pixel for the four of a chroma sample
kvm_yuv.c @@ cell[gx].first < cell[best].first @@ cell[gx].first > cell[best].first @@ a tie of the dominant colour goes to the last sample
kvm_yuv.c @@ const int key = ((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3); @@ const int key = ((p[0] >> 3) << 10) | ((p[1] >> 3) << 5); @@ the red channel is not part of the cell
kvm_yuv.c @@ if (x0 < 0 || y0 < 0 || rw < 0 || rh < 0 || (x0 & 1) || (y0 & 1) || (rw & 1) || (rh & 1) || @@ if (x0 < 0 || y0 < 0 || rw < 0 || rh < 0 || (y0 & 1) || (rw & 1) || (rh & 1) || @@ an odd x0 of a region is accepted
kvm_yuv.c @@ rw > codedW - x0 || rh > codedH - y0) { return -1; } @@ rw > codedW - x0) { return -1; } @@ a region below the picture is accepted
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
  if ! gcc -std=c99 -O1 -w -o /tmp/m/t /tmp/m/tests/kvm_yuv_test.c /tmp/m/kvm_yuv.c -lm 2>/tmp/m/err; then echo "INVALID  ($desc): does not compile"; invalid=$((invalid + 1)); continue; fi
  if timeout 120 /tmp/m/t >/tmp/m/out 2>&1; then echo "SURVIVED ($desc)"; survived=$((survived + 1)); else echo "killed   ($desc): $(grep -m1 FAIL /tmp/m/out | cut -c1-110)"; killed=$((killed + 1)); fi
done < /tmp/mutations.txt
echo "mutations: $n, killed $killed, survived $survived, invalid $invalid"
[ $survived -eq 0 ] && [ $invalid -eq 0 ]
' sh
