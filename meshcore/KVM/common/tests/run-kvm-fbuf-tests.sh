#!/bin/sh
# Runs the tests of kvm_fbuf.c (the picture buffers of the capture) in a container: gcc -Wall -Wextra -Werror -pedantic; ASan + UBSan with the thread stress; ThreadSanitizer with the thread stress.
# Usage: run-kvm-fbuf-tests.sh
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
gcc -std=c99 -O2 -Wall -Wextra -Werror -pedantic -o kvm_fbuf_test kvm_fbuf_test.c ../kvm_fbuf.c
./kvm_fbuf_test
echo "== AddressSanitizer + UndefinedBehaviorSanitizer, with the threads"
gcc -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -DKVM_FBUF_THREADS -pthread -o kvm_fbuf_test_asan kvm_fbuf_test.c ../kvm_fbuf.c
./kvm_fbuf_test_asan
echo "== ThreadSanitizer"
gcc -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=thread -DKVM_FBUF_THREADS -pthread -o kvm_fbuf_test_tsan kvm_fbuf_test.c ../kvm_fbuf.c
./kvm_fbuf_test_tsan
'
