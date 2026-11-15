#!/bin/sh
# Tests of kvm_hwtest_core.c with a fake encoder, in a container (gcc): plain build with -Wall -Wextra -Werror, then AddressSanitizer + UndefinedBehaviorSanitizer.
# Usage: run-kvm-hwtest-core-tests.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
KVM=$(cd "$HERE/../.." && pwd)
IMG=kvm-ctest:local
if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    docker build -q -t "$IMG" - <<'DOCKERFILE'
FROM debian:bookworm-slim
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends gcc libc6-dev make >/dev/null && rm -rf /var/lib/apt/lists/*
DOCKERFILE
fi
exec docker run --rm --user "$(id -u):$(id -g)" -v "$KVM:/src:ro" -w /tmp "$IMG" sh -c '
set -eu
cp -r /src /tmp/kvm
cd /tmp/kvm/Windows/tests
SRC="kvm_hwtest_core_test.c ../kvm_hwtest_core.c ../../common/kvm_hwpolicy.c ../../common/kvm_synth.c ../../common/kvm_yuv.c ../../common/kvm_h264_rules.c ../../common/kvm_annexb.c ../../common/kvm_video_core.c ../../common/kvm_stream.c"
echo "== plain build (-std=gnu99 -O2 -Wall -Wextra -Werror)"
gcc -std=gnu99 -O2 -Wall -Wextra -Werror -Wno-unused-function -I.. -o t $SRC -lm
./t
echo "== AddressSanitizer + UndefinedBehaviorSanitizer"
gcc -std=gnu99 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -fsanitize=address,undefined -fno-sanitize-recover=undefined -I.. -o t_asan $SRC -lm
./t_asan
'
