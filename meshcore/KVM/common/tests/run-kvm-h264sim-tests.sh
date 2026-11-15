#!/bin/sh
# the simulation of the sharpening and the quality control (kvm_h264sim_test.c). The program is built against the files of the tree; when OLD_COMMIT names a commit of this repository
# the rate control of that commit is built too (-DSIM_OLD_RC: the rows "before"). Usage: [OLD_COMMIT=<commit>] run-kvm-h264sim-tests.sh [-v]. Needs git only for OLD_COMMIT.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
COMMON=$(dirname "$HERE")
REPO=$(cd "$COMMON" && git rev-parse --show-toplevel 2>/dev/null || true)
OLD=${OLD_COMMIT:-}
IMG=kvm-ctest:local
if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    docker build -q -t "$IMG" - <<'DOCKERFILE'
FROM debian:bookworm-slim
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends gcc libc6-dev make >/dev/null && rm -rf /var/lib/apt/lists/*
DOCKERFILE
fi
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/old/tests"
if [ -n "$OLD" ]; then
    for f in kvm_stdin.h kvm_video_rc.c kvm_video_rc.h kvm_video_core.c kvm_video_core.h kvm_stream.c kvm_stream.h kvm_annexb.c kvm_annexb.h kvm_pace.c kvm_pace.h; do
        git -C "$REPO" show "$OLD:meshcore/KVM/common/$f" > "$TMP/old/$f"
    done
    cp "$HERE/kvm_h264sim_test.c" "$TMP/old/tests/"
    cp "$COMMON/kvm_sharpen.c" "$COMMON/kvm_sharpen.h" "$TMP/old/"
fi
exec docker run --rm --user "$(id -u):$(id -g)" -v "$COMMON:/src:ro" -v "$TMP/old:/old:ro" -w /tmp "$IMG" sh -c '
set -eu
cp -r /src /tmp/common; cp -r /old /tmp/oldtree
if [ -n "'"$OLD"'" ]; then
cd /tmp/oldtree/tests
echo "== the rate control and the files of '"$OLD"' (the rows before)"
gcc -std=c99 -O2 -Wall -Wextra -Werror -pedantic -DSIM_OLD_RC -o sim_old kvm_h264sim_test.c ../kvm_pace.c ../kvm_video_rc.c ../kvm_video_core.c ../kvm_stream.c ../kvm_annexb.c ../kvm_sharpen.c -lm
./sim_old '"$*"' || true
fi
cd /tmp/common/tests
echo "== the tree"
SRC="kvm_h264sim_test.c ../kvm_pace.c ../kvm_video_rc.c ../kvm_video_core.c ../kvm_stream.c ../kvm_annexb.c ../kvm_sharpen.c"
gcc -std=c99 -O2 -Wall -Wextra -Werror -pedantic -DSIM_SOFT -o sim_new $SRC -lm
./sim_new '"$*"'
gcc -std=c99 -O1 -g -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-sanitize-recover=undefined -DSIM_SOFT -o sim_asan $SRC -lm
./sim_asan | grep -E "FAIL|checks"
'
