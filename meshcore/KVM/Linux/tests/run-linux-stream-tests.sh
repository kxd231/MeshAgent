#!/bin/sh
# Runs the C tests of linux_kvm_stream.c (the glue of the end-to-end ACK in the Linux capture process) in a container (the host has no compilers): a plain build with
# -Wall -Wextra -Werror, then a build with AddressSanitizer and UndefinedBehaviorSanitizer. The clock is injected, so no benchmark lock is needed.
# Usage: run-linux-stream-tests.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
IMG=kvm-ctest:local
if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    docker build -q -t "$IMG" - <<'DOCKERFILE'
FROM debian:bookworm-slim
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends gcc libc6-dev make >/dev/null && rm -rf /var/lib/apt/lists/*
DOCKERFILE
fi
exec docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT/meshcore:/src/meshcore:ro" -w /tmp "$IMG" sh -c '
set -eu
cp -r /src/meshcore /tmp/meshcore
cd /tmp/meshcore/KVM/Linux/tests
SRC="linux_kvm_stream_test.c ../linux_kvm_stream.c ../../common/kvm_stream.c"
echo "== plain build (-std=gnu99 -O2 -Wall -Wextra -Werror)"
gcc -std=gnu99 -O2 -Wall -Wextra -Werror -DKVML_TEST -DKVM_VIDEO_DISABLE -I/tmp -o linux_kvm_stream_test $SRC
./linux_kvm_stream_test
echo "== AddressSanitizer + UndefinedBehaviorSanitizer"
gcc -std=gnu99 -O1 -g -Wall -Wextra -Werror -DKVML_TEST -DKVM_VIDEO_DISABLE -I/tmp -fsanitize=address,undefined -fno-sanitize-recover=undefined -o linux_kvm_stream_test_asan $SRC
./linux_kvm_stream_test_asan
echo "== the glue is not compiled with -DKVM_STREAM_DISABLE"
gcc -std=gnu99 -O2 -Wall -Wextra -Werror -DKVM_STREAM_DISABLE -I/tmp -c -o /tmp/off.o ../linux_kvm_stream.c
'
