#!/bin/sh
# Runs the tests of kvm_pace.c (the schedule of the loop, the slots of the video frames, the CPU budget) and the simulation of the loop in a container (gcc, -Wall -Wextra -Werror -pedantic,
# then ASan + UBSan). Prints the table of the report. Time is injected, so nothing is measured and no benchmark lock is needed.
# Usage: run-kvm-pace-tests.sh
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
SRC="kvm_pace_test.c ../kvm_pace.c ../kvm_video_core.c ../kvm_stream.c ../kvm_annexb.c"
echo "== plain build (-std=c99 -O2 -Wall -Wextra -Werror -pedantic)"
gcc -std=c99 -O2 -Wall -Wextra -Werror -pedantic -o kvm_pace_test $SRC
./kvm_pace_test
echo "== AddressSanitizer + UndefinedBehaviorSanitizer (table not repeated)"
gcc -std=c99 -O1 -g -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-sanitize-recover=undefined -o kvm_pace_test_asan $SRC
./kvm_pace_test_asan | grep -E "FAIL|checks"
'
