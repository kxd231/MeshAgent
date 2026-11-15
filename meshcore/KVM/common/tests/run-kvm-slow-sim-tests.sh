#!/bin/sh
# the simulation of slow links (kvm_slow_sim_test.c: 128 kb/s, 256 kb/s, 1 Mb/s; the start of a session with a small first picture, a pause of 8 minutes, a link that fell in a pause).
# The checks (-DSIM_CHECK) run for the tree (with ASan and UBSan as well). The same program can also be built against the files of earlier commits of this repository, named in the
# variables DEPLOYED_COMMIT, RATE_COMMIT, EMPTY_COMMIT, IDR_COMMIT (none by default); for them the checks are information only.
# Usage: [DEPLOYED_COMMIT=<commit> ...] run-kvm-slow-sim-tests.sh [scene] (a scene name such as L3 gives its trace every 500 ms). Time is simulated: no benchmark lock is needed.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
COMMON=$(dirname "$HERE")
REPO=$(cd "$COMMON" && git rev-parse --show-toplevel 2>/dev/null || true)
DEPLOYED=${DEPLOYED_COMMIT:-}
RATEF=${RATE_COMMIT:-}
EMPTYF=${EMPTY_COMMIT:-}
IDRF=${IDR_COMMIT:-}
IMG=kvm-ctest:local
if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    docker build -q -t "$IMG" - <<'DOCKERFILE'
FROM debian:bookworm-slim
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends gcc libc6-dev make >/dev/null && rm -rf /var/lib/apt/lists/*
DOCKERFILE
fi
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
for pair in "deployed:$DEPLOYED" "ratef:$RATEF" "emptyf:$EMPTYF" "idrf:$IDRF"; do
    d=${pair%%:*}; c=${pair#*:}
    [ -n "$c" ] || continue
    mkdir -p "$TMP/$d/tests"
    for f in kvm_stdin.h kvm_video_rc.c kvm_video_rc.h kvm_video_core.c kvm_video_core.h kvm_stream.c kvm_stream.h kvm_annexb.c kvm_annexb.h kvm_pace.c kvm_pace.h kvm_sharpen.c kvm_sharpen.h; do
        git -C "$REPO" show "$c:meshcore/KVM/common/$f" > "$TMP/$d/$f"
    done
    cp "$HERE/kvm_slow_sim_test.c" "$TMP/$d/tests/"
    echo "$c" > "$TMP/$d/COMMIT"
done
mkdir -p "$TMP"
exec docker run --rm --user "$(id -u):$(id -g)" -v "$COMMON:/src:ro" -v "$TMP:/old:ro" -w /tmp "$IMG" sh -c '
set -eu
cp -r /src /tmp/common; cp -r /old /tmp/oldtree
FILES="../kvm_pace.c ../kvm_video_rc.c ../kvm_video_core.c ../kvm_stream.c ../kvm_annexb.c ../kvm_sharpen.c"
for d in deployed ratef emptyf idrf; do
  [ -d /tmp/oldtree/$d ] || continue
  c=$(cat /tmp/oldtree/$d/COMMIT)
  cd /tmp/oldtree/$d/tests
  echo "== $c"
  gcc -std=c99 -O2 -Wall -Wextra -Werror -pedantic -DSIM_BUILD="\"$c\"" -o sim kvm_slow_sim_test.c $FILES -lm
  ./sim '"$*"'
  echo "-- the same checks on the files of $c (information)"
  gcc -std=c99 -O2 -w -DSIM_CHECK -o sim_chk kvm_slow_sim_test.c $FILES -lm
  ./sim_chk | grep -E "FAIL|checks" || true
done
cd /tmp/common/tests
echo "== the tree"
gcc -std=c99 -O2 -Wall -Wextra -Werror -pedantic -DSIM_CHECK -DSIM_BUILD="\"the tree\"" -DSIM_SOFT -o sim_new kvm_slow_sim_test.c $FILES -lm
./sim_new '"$*"'
gcc -std=c99 -O1 -g -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-sanitize-recover=undefined -DSIM_CHECK -DSIM_SOFT -o sim_asan kvm_slow_sim_test.c $FILES -lm
./sim_asan | grep -E "FAIL|checks"
'
