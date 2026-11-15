#!/bin/sh
# Runs the C tests of meshcore/KVM/common in a container (the host has no compilers). The timing line is informative only; the run holds
# the benchmark lock (the file named in KVM_BENCH_LOCK, if it exists) because the machine's load changes the numbers.
# Usage: run-in-docker.sh [make target...]   (default: all = plain build and run, then the sanitizer build and run)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
COMMON=$(dirname "$HERE")
IMG=kvm-ctest:local
LOCK=${KVM_BENCH_LOCK:-}
if [ "${KVM_CTEST_LOCKED:-}" != 1 ] && [ -n "$LOCK" ] && [ -e "$LOCK" ]; then KVM_CTEST_LOCKED=1 exec flock -w 900 "$LOCK" env KVM_CTEST_LOCKED=1 "$0" "$@"; fi
if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    docker build -q -t "$IMG" - <<'DOCKERFILE'
FROM debian:bookworm-slim
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends gcc libc6-dev make >/dev/null && rm -rf /var/lib/apt/lists/*
DOCKERFILE
fi
exec docker run --rm --user "$(id -u):$(id -g)" -v "$COMMON:/src:ro" -w /tmp "$IMG" sh -c 'cp -r /src /tmp/common && make -C /tmp/common/tests "$@"' sh "${@:-all}"
