#!/bin/sh
# the start of a video session with a SMALL first key frame (the flat key frame of the NV12 view B converts a part of the picture) against the proof of throughput of the first key frame (capFloor, C_idr of
# kvm_vrc_idr_acked): R and the frames of the first seconds with the first key frame of 228 (the whole picture), 40, 20 and 5 KB, with and without a minimum size of the key frame for the proof
# (KVM_VRC_IDR_PROOF_MIN_BYTES = 0 / 65536), on 100, 40, 10 and 3 Mb/s (kvm_start_sim_test.c, the model of the start). Usage: run-kvm-idr1-start-sim.sh
# The run takes seconds and does not depend on the time of the machine.
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
set -eu
cp -r /src /tmp/common
cd /tmp/common/tests
FILES="../kvm_pace.c ../kvm_video_rc.c ../kvm_video_core.c ../kvm_stream.c ../kvm_annexb.c ../kvm_sharpen.c"
for kb in 228 40 20 5; do
  for pm in 0 65536; do
    gcc -std=c99 -O2 -w -DSIM_T6 -DSIM_SOFT -DSIM_IDR_KB=$kb -DKVM_VRC_IDR_PROOF_MIN_BYTES=$pm -o sim_${kb}_${pm} kvm_start_sim_test.c $FILES -lm
    ./sim_${kb}_${pm} | grep "^IDR1-SIM"
  done
done
'
