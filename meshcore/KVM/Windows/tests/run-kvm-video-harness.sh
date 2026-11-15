#!/bin/sh
# Runs kvm_video_harness_test.cpp in a container (the host has no compilers): the production kvm_video.cpp, kvm_video_core.c, kvm_stream.c and kvm_annexb.c compiled for Linux against a
# shim of <windows.h> (tests/shim) with a fake encoder, fake glue and fake globals; plain build with -Wall -Wextra, then AddressSanitizer + UndefinedBehaviorSanitizer.
# The default is the one of the product: no -DKVM_NV12_FROZEN_DEFAULT, so the value in kvm_video.cpp counts (the NV12 view on; the random sessions check that frames went through the view); a further pass built
# with -DKVM_NV12_FROZEN_DEFAULT=0 proves the old path. FROZEN=0 (or 1) makes the whole run use that value.
# Usage: run-kvm-video-harness.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
KVM=$(dirname "$(dirname "$HERE")")/KVM
KVM=$(cd "$HERE/../.." && pwd)
IMG=kvm-ctestpp:local
if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    docker build -q -t "$IMG" - <<'DOCKERFILE'
FROM debian:bookworm-slim
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends gcc g++ libc6-dev make >/dev/null && rm -rf /var/lib/apt/lists/*
DOCKERFILE
fi
OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT
docker run --rm --label kvm-auto-agent=1 -e FROZEN="${FROZEN:-}" --user "$(id -u):$(id -g)" -v "$KVM:/src:ro" -v "$OUT:/out" -w /tmp "$IMG" sh -c '
set -eu
cp -r /src /tmp/kvm
cd /tmp/kvm/Windows/tests
cp shim/Windows.h shim/windows.h   # (not in the repository: two files that differ only in case do not survive a checkout on Windows)
DEFS="-D_LINKVM -DKVM_DXGI -DKVM_DXGI_DISABLE -D_M_X64 -D_TURBOJPEG -DWIN32 -DWIN64 ${FROZEN:+-DKVM_NV12_FROZEN_DEFAULT=$FROZEN} -DKVM_VIDEO_TEST"
INC="-Ishim -I.. -I../../common"
CXX="g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-format-security -fno-strict-aliasing"
build() { # $1 extra flags, $2 output
  gcc -std=c99 $1 -c ../../common/kvm_video_core.c -o vc.o && gcc -std=c99 $1 -c ../../common/kvm_stream.c -o vs.o && gcc -std=c99 $1 -c ../../common/kvm_annexb.c -o va.o && gcc -std=c99 $1 -c ../../common/kvm_video_rc.c -o vr.o && gcc -std=c99 $1 -c ../../common/kvm_pace.c -o vp.o && gcc -std=c99 $1 -c ../../common/kvm_qpdet.c -o vq.o && gcc -std=c99 $1 -c ../../common/kvm_encjob.c -o ve.o && gcc -std=c99 $1 -c ../../common/kvm_fbuf.c -o vf.o && gcc -std=c99 $1 -c ../../common/kvm_hwpolicy.c -o vh.o && gcc -std=c99 $1 -c ../../common/kvm_auto.c -o vau.o && gcc -std=c99 $1 -c ../../common/kvm_nv12view.c -o vn.o && gcc -std=c99 $1 -c ../../common/kvm_yuv.c -o vy.o
  $CXX $1 $DEFS $INC -pthread -o $2 kvm_video_harness_test.cpp ../kvm_video.cpp ../kvm_encthr.cpp ../kvm_encbe_mf.cpp vc.o vs.o va.o vr.o vp.o vq.o ve.o vf.o vh.o vau.o vn.o vy.o
}
build_tsan() { # the real thread under ThreadSanitizer (the production files compiled with it, the fakes lock)
  for f in kvm_video_core kvm_stream kvm_annexb kvm_video_rc kvm_pace kvm_qpdet kvm_encjob kvm_fbuf kvm_hwpolicy kvm_nv12view kvm_yuv kvm_auto; do gcc -std=c99 -O1 -g -fsanitize=thread -c ../../common/$f.c -o t_$f.o; done
  $CXX -O1 -g -fsanitize=thread $DEFS $INC -pthread -o harness_tsan kvm_video_harness_test.cpp ../kvm_video.cpp ../kvm_encthr.cpp ../kvm_encbe_mf.cpp t_kvm_video_core.o t_kvm_stream.o t_kvm_annexb.o t_kvm_video_rc.o t_kvm_pace.o t_kvm_qpdet.o t_kvm_encjob.o t_kvm_fbuf.o t_kvm_hwpolicy.o t_kvm_auto.o t_kvm_nv12view.o t_kvm_yuv.o
}
echo "== plain build"
build "-O2" harness
./harness
echo "== AddressSanitizer + UndefinedBehaviorSanitizer"
build "-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined" harness_asan
./harness_asan
echo "== the real thread of the encoder: 1000 random sessions, plain"
./harness sessions 1000 threaded
echo "== the real thread of the encoder under ThreadSanitizer: 300 random sessions"
TSAN_OPTIONS=halt_on_error=1 build_tsan
TSAN_OPTIONS=halt_on_error=1 ./harness_tsan sessions 300 threaded
echo "== the view switched off by default (MESH_KVM_NV12_FROZEN=0: the picture goes to the encoder as before the NV12 view): plain, then 300 random sessions with the real thread"
DEFS="$(printf "%s" "$DEFS" | sed "s/-DKVM_NV12_FROZEN_DEFAULT=[01]//") -DKVM_NV12_FROZEN_DEFAULT=0"
build "-O2" harness_off
./harness_off
./harness_off sessions 300 threaded
echo "== the frames of a session as a tunnel stream (for the viewer validator)"
HARNESS_DUMP=/out/stream-H.bin ./harness
'
# the viewer's side: the reference validator and the parser of the viewer (agent-desktop-h264.js) of the MeshCentral fork (VIEWER_REPO: a MeshCentral checkout that has them; without it this step is skipped)
VIEWER=${VIEWER_REPO:-}
if [ -n "$VIEWER" ] && [ -d "$VIEWER/public/scripts" ] && docker image inspect node:24-bookworm-slim >/dev/null 2>&1; then
    echo "== viewer validator on the stream"
    docker run --rm --label kvm-auto-agent=1 --user "$(id -u):$(id -g)" -v "$VIEWER:/src:ro" -v "$HERE/analyze-capture.js:/t/analyze-capture.js:ro" -v "$OUT:/cap" node:24-bookworm-slim node /t/analyze-capture.js /cap > "$OUT/analysis.json"
    node_ok=$(python3 -c "import json;d=json.load(open('$OUT/analysis.json'));r=d['H'];print('frames',r['frames'],'idr',r['idr'],'rej',r['rej'],'viewerErr',r['viewerErr'],'gaps',r['gaps'],'sps',r['sps']);import sys;sys.exit(0 if r['frames']>=10 and not r['rej'] and not r['viewerErr'] and r['gaps']==0 else 1)")
    echo "$node_ok"
else echo "(viewer validator skipped: VIEWER_REPO is not set to a MeshCentral checkout, or there is no node image)"; fi
