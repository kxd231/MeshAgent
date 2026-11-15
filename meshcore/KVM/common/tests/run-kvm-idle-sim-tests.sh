#!/bin/sh
# the simulation of a new key frame / a new generation / op 3 / a JPEG after an idle phase (kvm_idle_sim_test.c, 36 scenes). The checks (-DSIM_CHECK) run for the tree (with ASan and UBSan as
# well). The same program can also be built against the files of earlier commits of this repository, named in C1, C2, C3 (none by default); for them the checks are information only.
# Usage: [C1=<commit> [C2=<commit> [C3=<commit>]]] run-kvm-idle-sim-tests.sh [scene prefix [v]]. Time is simulated: no benchmark lock is needed.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
COMMON=$(dirname "$HERE")
REPO=$(cd "$COMMON" && git rev-parse --show-toplevel 2>/dev/null || true)
IMG=kvm-ctest:local
if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    docker build -q -t "$IMG" - <<'DOCKERFILE'
FROM debian:bookworm-slim
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends gcc libc6-dev make >/dev/null && rm -rf /var/lib/apt/lists/*
DOCKERFILE
fi
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
for pair in "c1:${C1:-}" "c2:${C2:-}" "c3:${C3:-}"; do
    d=${pair%%:*}; c=${pair#*:}
    [ -n "$c" ] || continue
    mkdir -p "$TMP/$d/tests"
    for f in kvm_stdin.h kvm_video_rc.c kvm_video_rc.h kvm_video_core.c kvm_video_core.h kvm_stream.c kvm_stream.h kvm_annexb.c kvm_annexb.h kvm_pace.c kvm_pace.h kvm_sharpen.c kvm_sharpen.h; do
        git -C "$REPO" show "$c:meshcore/KVM/common/$f" > "$TMP/$d/$f"
    done
    cp "$HERE/kvm_idle_sim_test.c" "$TMP/$d/tests/"
    echo "$c" > "$TMP/$d/COMMIT"
done
exec docker run --rm --user "$(id -u):$(id -g)" -v "$COMMON:/src:ro" -v "$TMP:/old:ro" -w /tmp "$IMG" sh -c '
set -eu
cp -r /src /tmp/common; cp -r /old /tmp/oldtree
FILES="../kvm_pace.c ../kvm_video_rc.c ../kvm_video_core.c ../kvm_stream.c ../kvm_annexb.c ../kvm_sharpen.c"
for d in c1 c2 c3; do
  [ -d /tmp/oldtree/$d ] || continue
  c=$(cat /tmp/oldtree/$d/COMMIT)
  cd /tmp/oldtree/$d/tests
  echo "== $c"
  gcc -std=c99 -O2 -w -DSIM_BUILD="\"$c\"" -o sim kvm_idle_sim_test.c $FILES -lm
  ./sim '"$*"'
  echo "-- the same checks on the files of $c (information)"
  gcc -std=c99 -O2 -w -DSIM_CHECK -o sim_chk kvm_idle_sim_test.c $FILES -lm
  ./sim_chk | grep -E "checks" || true
done
cd /tmp/common/tests
echo "== the tree"
gcc -std=c99 -O2 -Wall -Wextra -Werror -pedantic -DSIM_SOFT -DSIM_CHECK -DSIM_BUILD="\"the tree\"" -o sim_new kvm_idle_sim_test.c $FILES -lm
./sim_new '"$*"'
gcc -std=c99 -O1 -g -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-sanitize-recover=undefined -DSIM_SOFT -DSIM_CHECK -o sim_asan kvm_idle_sim_test.c $FILES -lm
./sim_asan | grep -E "FAIL|checks"
'
