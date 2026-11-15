#!/bin/sh
# Runs kvm_encthr_test.cpp (the real encoder thread on pthreads against a fake backend) and the fbuf thread stress in a container: plain, AddressSanitizer + UBSan, ThreadSanitizer.
# kvm_encthr.cpp, kvm_encjob.c are the production files. Usage: run-kvm-encthr-tests.sh [sessions]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
KVM=$(cd "$HERE/../.." && pwd)
IMG=kvm-ctestpp:local
if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    docker build -q -t "$IMG" - <<'DOCKERFILE'
FROM debian:bookworm-slim
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends gcc g++ libc6-dev make >/dev/null && rm -rf /var/lib/apt/lists/*
DOCKERFILE
fi
N=${1:-300}
exec docker run --rm --user "$(id -u):$(id -g)" -v "$KVM:/src:ro" -w /tmp "$IMG" sh -c '
set -eu
cp -r /src /tmp/kvm
cd /tmp/kvm/Windows/tests
build() { # $1 flags, $2 out
  gcc -std=c99 $1 -c ../../common/kvm_encjob.c -o ej.o
  gcc -std=c99 $1 -c ../../common/kvm_nv12view.c -o nv.o
  gcc -std=c99 $1 -c ../../common/kvm_yuv.c -o yuv.o
  g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter $1 -I.. -I../../common -pthread -o $2 kvm_encthr_test.cpp ../kvm_encthr.cpp ej.o nv.o yuv.o
}
echo "== plain"
build "-O2" t; ./t '"$N"'
echo "== AddressSanitizer + UndefinedBehaviorSanitizer"
build "-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined" ta; ./ta '"$N"'
echo "== ThreadSanitizer"
build "-O1 -g -fsanitize=thread" tt; ./tt '"$N"'
'
