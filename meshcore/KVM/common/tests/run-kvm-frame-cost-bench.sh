#!/bin/sh
# the cost per 1080p frame of the per-frame work of the smoothness work / the first key frame proof (kvm_frame_cost_bench.c) at gcc -O2, -O2 without the vectorizer (the scalar code of an MSVC build) and -O1.
# Takes the benchmark lock named in KVM_BENCH_LOCK, if that file exists (the load of the machine changes the numbers). Usage: run-kvm-frame-cost-bench.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
COMMON=$(dirname "$HERE")
IMG=kvm-ctest:local
LOCK=${KVM_BENCH_LOCK:-}
if [ "${KVM_CTEST_LOCKED:-}" != 1 ] && [ -n "$LOCK" ] && [ -e "$LOCK" ]; then KVM_CTEST_LOCKED=1 exec flock -w 900 "$LOCK" env KVM_CTEST_LOCKED=1 sh "$HERE/$(basename "$0")" "$@"; fi
if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    docker build -q -t "$IMG" - <<'DOCKERFILE'
FROM debian:bookworm-slim
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends gcc libc6-dev make >/dev/null && rm -rf /var/lib/apt/lists/*
DOCKERFILE
fi
exec docker run --rm --user "$(id -u):$(id -g)" -v "$COMMON:/src:ro" -w /tmp "$IMG" sh -c '
set -eu
gcc --version | head -1
for opt in "-O2" "-O2 -fno-tree-vectorize" "-O1"; do
  gcc -std=gnu11 $opt -Wall -I/src -o bench /src/tests/kvm_frame_cost_bench.c /src/kvm_motion.c /src/kvm_yuv.c /src/kvm_video_core.c /src/kvm_stream.c /src/kvm_annexb.c /src/kvm_pace.c /src/kvm_video_rc.c /src/kvm_sharpen.c -lm
  ./bench "gcc $opt"
done
'
