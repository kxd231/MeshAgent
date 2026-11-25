#!/bin/sh
# Runs the tests of linux_video.c (the H.264 video path of the Linux capture process) in a container with libopenh264 and ffmpeg (the host has no compilers): a plain build with
# -Wall -Wextra -Werror and a build with AddressSanitizer and UndefinedBehaviorSanitizer; the answers without a library and with the video switched off (processes of their own: the library is
# loaded once per process); and the check of the result: the stream of a recorded scenario is decoded by ffmpeg (only here, as a check; it is not part of the agent) and compared with the
# pictures that were encoded, inside the rectangles of every frame. Usage: run-linux-video-tests.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
IMG=kvm-h264test:local
if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    docker build -q -t "$IMG" - <<'DOCKERFILE'
FROM debian:bookworm-slim
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends gcc libc6-dev make libopenh264-7 ffmpeg >/dev/null && rm -rf /var/lib/apt/lists/*
DOCKERFILE
fi
exec docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT/meshcore:/src/meshcore:ro" -w /tmp "$IMG" sh -c '
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
echo "== AddressSanitizer + UndefinedBehaviorSanitizer"
gcc -std=gnu99 -O1 -g -Wall -Wextra -Werror -DKVML_TEST -I/tmp -fsanitize=address,undefined -fno-sanitize-recover=undefined -o linux_video_test_asan $SRC -lm -ldl
./linux_video_test_asan
echo "== the decoder reads the stream of the recorded scenario"
mkdir -p /tmp/rec
./linux_video_test run /tmp/rec
ffmpeg -v error -i /tmp/rec/stream.h264 -vsync 0 -pix_fmt rgb24 -f rawvideo /tmp/rec/decoded.rgb
./linux_video_test check /tmp/rec
'
