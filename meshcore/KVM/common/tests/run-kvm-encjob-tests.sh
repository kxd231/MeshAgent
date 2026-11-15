#!/bin/sh
# Runs the tests of kvm_encjob.c (the slots of the jobs of the encoder thread) in a container (gcc, -Wall -Wextra -Werror -pedantic, then ASan + UBSan).
# Usage: run-kvm-encjob-tests.sh
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
gcc -std=c99 -O2 -Wall -Wextra -Werror -pedantic -o kvm_encjob_test kvm_encjob_test.c ../kvm_encjob.c
./kvm_encjob_test
echo "== AddressSanitizer + UndefinedBehaviorSanitizer"
gcc -std=c99 -O1 -g -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-sanitize-recover=undefined -o kvm_encjob_test_asan kvm_encjob_test.c ../kvm_encjob.c
./kvm_encjob_test_asan
'
