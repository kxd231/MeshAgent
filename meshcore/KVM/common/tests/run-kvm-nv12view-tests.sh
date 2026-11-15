#!/bin/sh
# Tests of kvm_nv12view.c in a container (gcc): a plain build with -Wall -Wextra -Werror -pedantic (C99), then AddressSanitizer + UndefinedBehaviorSanitizer. Usage: run-kvm-nv12view-tests.sh
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
echo "== plain build (-std=gnu99 -O2 -Wall -Wextra -Werror -pedantic)"
gcc -std=gnu99 -O2 -Wall -Wextra -Werror -pedantic -o kvm_nv12view_test kvm_nv12view_test.c ../kvm_nv12view.c ../kvm_yuv.c -lm
./kvm_nv12view_test
echo "== AddressSanitizer + UndefinedBehaviorSanitizer"
gcc -std=gnu99 -O1 -g -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-sanitize-recover=undefined -o kvm_nv12view_test_asan kvm_nv12view_test.c ../kvm_nv12view.c ../kvm_yuv.c -lm
./kvm_nv12view_test_asan
'
