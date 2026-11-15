#!/bin/sh
# runs the tests of kvm_auto.c (the decision of the Auto mode of the frame rate: kvm_auto_test.c) in a container (gcc, -Wall -Wextra -Werror -pedantic, then ASan + UBSan). Time is
# injected, so nothing is measured and no benchmark lock is needed. The closed-loop simulation is run-kvm-auto-sim-tests.sh.
# Usage: run-kvm-auto-tests.sh
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
SRC="kvm_auto_test.c ../kvm_auto.c"
echo "== plain build (-std=c99 -O2 -Wall -Wextra -Werror -pedantic)"
gcc -std=c99 -O2 -Wall -Wextra -Werror -pedantic -o kvm_auto_test $SRC
./kvm_auto_test
echo "== AddressSanitizer + UndefinedBehaviorSanitizer"
gcc -std=c99 -O1 -g -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-sanitize-recover=undefined -o kvm_auto_test_asan $SRC
./kvm_auto_test_asan | grep -E "FAIL|checks"
'
