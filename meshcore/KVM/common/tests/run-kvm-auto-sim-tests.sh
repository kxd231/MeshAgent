#!/bin/sh
# the closed loop of the Auto mode of the frame rate (kvm_auto_sim_test.c): the real stream layer, rate control, pace and kvm_auto on models of the link, the capture and the encoder, every scene with
# Auto and with the fixed Medium (the baseline). Time is injected, nothing is measured. Prints the tables of the report; the checks are the criteria.
# Usage: run-kvm-auto-sim-tests.sh [-v [scene-number]]   (-v prints a line of kvm_auto for every window of the scene, of all scenes without a number; the first scene is 0)
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
exec docker run --rm --label kvm-auto-agent=1 --user "$(id -u):$(id -g)" -v "$COMMON:/src:ro" -w /tmp "$IMG" sh -c '
set -eu
cp -r /src /tmp/common
cd /tmp/common/tests
SRC="kvm_auto_sim_test.c ../kvm_auto.c ../kvm_pace.c ../kvm_video_rc.c ../kvm_video_core.c ../kvm_stream.c ../kvm_annexb.c"
echo "== plain build (-std=c99 -O2 -Wall -Wextra -Werror -pedantic)"
gcc -std=c99 -O2 -Wall -Wextra -Werror -pedantic -o kvm_auto_sim $SRC -lm
./kvm_auto_sim '"$*"'
echo "== AddressSanitizer + UndefinedBehaviorSanitizer (table not repeated)"
gcc -std=c99 -O1 -g -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-sanitize-recover=undefined -o kvm_auto_sim_asan $SRC -lm
./kvm_auto_sim_asan | grep -E "FAIL|checks"
'
