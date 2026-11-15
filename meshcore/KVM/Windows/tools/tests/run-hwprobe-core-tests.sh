#!/bin/sh
# Tests of the portable part of kvm_h264_check --hw (kvm_hwprobe_core.cpp) and of the rules module it uses: g++ and gcc in a container (the host has no compilers), a plain
# build with -Wall -Wextra -Werror -pedantic, then AddressSanitizer + UndefinedBehaviorSanitizer; the JSON document that the test writes is parsed by node in a second container.
# Usage: run-hwprobe-core-tests.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
KVM=$(cd "$HERE/../../.." && pwd)
IMG=kvm-ctestpp:local
if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    docker build -q -t "$IMG" - <<'DOCKERFILE'
FROM debian:bookworm-slim
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends gcc g++ libc6-dev make >/dev/null && rm -rf /var/lib/apt/lists/*
DOCKERFILE
fi
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
docker run --rm --user "$(id -u):$(id -g)" -v "$KVM:/src:ro" -v "$OUT:/o" -w /tmp "$IMG" sh -c '
set -eu
cp -r /src /tmp/kvm
cd /tmp/kvm/Windows/tools/tests
gcc -std=c99 -O2 -Wall -Wextra -Werror -pedantic -c -o /tmp/rules.o ../../../common/kvm_h264_rules.c
gcc -std=c99 -O2 -Wall -Wextra -Werror -pedantic -c -o /tmp/annexb.o ../../../common/kvm_annexb.c
echo "== plain build (-std=c++11 -O2 -Wall -Wextra -Werror -pedantic)"
g++ -std=c++11 -O2 -Wall -Wextra -Werror -pedantic -o /tmp/core_test hwprobe_core_test.cpp ../kvm_hwprobe_core.cpp /tmp/rules.o /tmp/annexb.o -lm -lpthread
/tmp/core_test /o/doc.json
echo "== AddressSanitizer + UndefinedBehaviorSanitizer"
g++ -std=c++11 -O1 -g -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-sanitize-recover=undefined -o /tmp/core_test_asan hwprobe_core_test.cpp ../kvm_hwprobe_core.cpp ../../../common/kvm_h264_rules.c ../../../common/kvm_annexb.c -lm -lpthread
/tmp/core_test_asan | grep -E "^FAIL|checks"
'
docker run --rm --user "$(id -u):$(id -g)" -v "$OUT:/o:ro" --entrypoint node node:24-bookworm-slim -e '
const j = JSON.parse(require("fs").readFileSync("/o/doc.json", "utf8"));
console.log("== JSON of the test document parses: kind=" + j.kind + " incomplete=" + j.incomplete + " keys=" + Object.keys(j).join(","));'
