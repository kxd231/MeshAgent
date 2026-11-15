#!/bin/sh
# Runs the C tests of kvm_video_core.c (and of the frame builder of kvm_stream.c) in a container (the host has no compilers): a plain build with
# -Wall -Wextra -Werror -pedantic (C99), then a build with AddressSanitizer and UndefinedBehaviorSanitizer. Time is injected in the tests, so there is nothing
# to measure and no benchmark lock is needed.
# Usage: run-kvm-video-core-tests.sh [mutations]   (mutations: also run the mutation check, run-kvm-video-mutations.sh)
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
echo "== plain build (-std=c99 -O2 -Wall -Wextra -Werror -pedantic)"
gcc -std=c99 -O2 -Wall -Wextra -Werror -pedantic -o kvm_video_core_test kvm_video_core_test.c ../kvm_video_core.c ../kvm_stream.c ../kvm_annexb.c
./kvm_video_core_test
echo "== the modules alone at -O0"
gcc -std=c99 -O0 -Wall -Wextra -Werror -pedantic -c -o /tmp/kvm_video_core_conv.o ../kvm_video_core.c
echo "== AddressSanitizer + UndefinedBehaviorSanitizer"
gcc -std=c99 -O1 -g -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-sanitize-recover=undefined -o kvm_video_core_test_asan kvm_video_core_test.c ../kvm_video_core.c ../kvm_stream.c ../kvm_annexb.c
./kvm_video_core_test_asan
echo "== the stream tests still pass (the frame builder was refactored)"
gcc -std=c99 -O2 -Wall -Wextra -Werror -pedantic -o kvm_stream_test kvm_stream_test.c ../kvm_stream.c
./kvm_stream_test
'
