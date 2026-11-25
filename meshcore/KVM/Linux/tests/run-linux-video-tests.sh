#!/bin/sh
# Runs the tests of linux_video.c (the H.264 video path of the Linux capture process) in a container with libopenh264 and ffmpeg (the host has no compilers): a plain build with
# -Wall -Wextra -Werror and a build with AddressSanitizer and UndefinedBehaviorSanitizer; the answers without a library and with the video switched off (processes of their own: the library is
# loaded once per process); and the check of the result: the stream of a recorded scenario is decoded by ffmpeg (only here, as a check; it is not part of the agent) and compared with the
# pictures that were encoded, inside the rectangles of every frame. Usage: run-linux-video-tests.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
# name=image=package: openh264 2.3.1 (Debian 12), 2.4.1 (Ubuntu 24.04), 2.6.0 (Debian 13), 2.7.0 (Arch; no ffmpeg there: the decoding steps are left out)
for DIST in bookworm=debian:bookworm-slim=libopenh264-7 noble=ubuntu:24.04=libopenh264-7 trixie=debian:trixie-slim=libopenh264-8 arch=archlinux:base=openh264; do
NAME=${DIST%%=*}; REST=${DIST#*=}; BASE=${REST%%=*}; PKG=${REST#*=}
IMG=kvm-h264test:$NAME
if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    if [ "$NAME" = arch ]; then
        docker build -q -t "$IMG" - <<DOCKERFILE
FROM $BASE
RUN pacman -Syu --noconfirm gcc make $PKG >/dev/null
DOCKERFILE
    else
        docker build -q -t "$IMG" - <<DOCKERFILE
FROM $BASE
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends gcc libc6-dev make $PKG ffmpeg >/dev/null && rm -rf /var/lib/apt/lists/*
DOCKERFILE
    fi
fi
echo "######## $NAME: $(docker run --rm --entrypoint sh "$IMG" -c 'ls /usr/lib /usr/lib/x86_64-linux-gnu 2>/dev/null | grep "^libopenh264.so\.[0-9]*\.[0-9.]*$"')"
docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT/meshcore:/src/meshcore:ro" -w /tmp "$IMG" sh -c '
set -eu
cp -r /src/meshcore /tmp/meshcore
cd /tmp/meshcore/KVM/Linux/tests
C=../../common
SRC="linux_video_test.c ../linux_video.c ../linux_h264.c ../linux_kvm_stream.c $C/kvm_stream.c $C/kvm_video_core.c $C/kvm_annexb.c $C/kvm_motion.c $C/kvm_sharpen.c $C/kvm_video_rc.c $C/kvm_yuv.c $C/kvm_pace.c"
echo "== plain build (-std=gnu99 -O2 -Wall -Wextra -Werror)"
gcc -std=gnu99 -O2 -Wall -Wextra -Werror -DKVML_TEST -I/tmp -o linux_video_test $SRC -lm -ldl
./linux_video_test
echo "== no library / switched off"
MESH_KVM_OPENH264=/nonexistent/libopenh264.so.7 ./linux_video_test nolib
MESH_KVM_VIDEO=0 ./linux_video_test off
echo "== the encoder fails"
MESH_KVM_H264_FAIL=1 ./linux_video_test fail
echo "== AddressSanitizer + UndefinedBehaviorSanitizer"
gcc -std=gnu99 -O1 -g -Wall -Wextra -Werror -DKVML_TEST -I/tmp -fsanitize=address,undefined -fno-sanitize-recover=undefined -o linux_video_test_asan $SRC -lm -ldl
./linux_video_test_asan
echo "== the decoder reads the stream of the recorded scenario"
mkdir -p /tmp/rec
./linux_video_test run /tmp/rec
if command -v ffmpeg >/dev/null; then
    ffmpeg -v error -i /tmp/rec/stream.h264 -vsync 0 -pix_fmt rgb24 -f rawvideo /tmp/rec/decoded.rgb
    ./linux_video_test check /tmp/rec
else echo "(no ffmpeg in this image: the stream is not decoded)"; fi
'
done
