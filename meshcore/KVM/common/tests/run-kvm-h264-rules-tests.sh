#!/bin/sh
# Runs the tests of kvm_h264_rules.c (the viewer's rules on an H.264 stream, used by the hardware probe of kvm_h264_check) in a container (gcc): a plain build with
# -Wall -Wextra -Werror -pedantic (C99), then AddressSanitizer + UndefinedBehaviorSanitizer (the fuzz loops are the point of that run). Nothing is measured: no benchmark lock.
# Usage: run-kvm-h264-rules-tests.sh
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
SRC="kvm_h264_rules_test.c ../kvm_h264_rules.c ../kvm_annexb.c ../kvm_video_core.c ../kvm_stream.c"
echo "== plain build (-std=c99 -O2 -Wall -Wextra -Werror -pedantic)"
gcc -std=c99 -O2 -Wall -Wextra -Werror -pedantic -o kvm_h264_rules_test $SRC
./kvm_h264_rules_test
echo "== AddressSanitizer + UndefinedBehaviorSanitizer"
gcc -std=c99 -O1 -g -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-sanitize-recover=undefined -o kvm_h264_rules_test_asan $SRC
./kvm_h264_rules_test_asan | grep -E "FAIL|DISAGREE|checks"
'
