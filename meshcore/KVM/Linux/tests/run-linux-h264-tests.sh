#!/bin/sh
# Runs the tests of linux_h264.c (the openh264 encoder of the Linux capture process) in a container with libopenh264 (the host has no compilers): a plain build with
# -Wall -Wextra -Werror, a build with AddressSanitizer and UndefinedBehaviorSanitizer, the check for a library that is not there, and the decoding of the streams with ffmpeg
# (the decoder is only here, as a check of the result; it is not part of the agent). Usage: run-linux-h264-tests.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
for DIST in bookworm:libopenh264-7 trixie:libopenh264-8; do
IMG=kvm-h264test:${DIST%%:*}
if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    docker build -q -t "$IMG" - <<DOCKERFILE
FROM debian:${DIST%%:*}-slim
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends gcc libc6-dev make ${DIST##*:} ffmpeg >/dev/null && rm -rf /var/lib/apt/lists/*
DOCKERFILE
fi
echo "######## ${DIST%%:*}: $(docker run --rm --entrypoint sh "$IMG" -c 'ls /usr/lib/x86_64-linux-gnu | grep "^libopenh264.so\.[0-9]*\.[0-9.]*$"')"
docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT/meshcore:/src/meshcore:ro" -w /tmp "$IMG" sh -c '
set -eu
cp -r /src/meshcore /tmp/meshcore
cd /tmp/meshcore/KVM/Linux/tests
SRC="linux_h264_test.c ../linux_h264.c ../../common/kvm_annexb.c ../../common/kvm_video_core.c ../../common/kvm_yuv.c ../../common/kvm_stream.c"
echo "== plain build (-std=gnu99 -O2 -Wall -Wextra -Werror)"
gcc -std=gnu99 -O2 -Wall -Wextra -Werror -I/tmp -o linux_h264_test $SRC -ldl
mkdir -p /tmp/dump
./linux_h264_test dump /tmp/dump
echo "== a library that is not there"
MESH_KVM_OPENH264=/nonexistent/libopenh264.so.7 ./linux_h264_test nolib
echo "== AddressSanitizer + UndefinedBehaviorSanitizer"
gcc -std=gnu99 -O1 -g -Wall -Wextra -Werror -I/tmp -fsanitize=address,undefined -fno-sanitize-recover=undefined -o linux_h264_test_asan $SRC -ldl
./linux_h264_test_asan
echo "== the decoder of ffmpeg reads every stream"
for f in /tmp/dump/*.h264; do
    n=$(ffmpeg -v error -i "$f" -f null - -progress - 2>&1 | grep -c "frame=" || true)
    fr=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames,width,height,profile,level,color_space,pix_fmt -of csv=p=0 "$f")
    echo "$(basename "$f"): $fr"
done
'
done
