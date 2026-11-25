#!/bin/sh
# Mutations of linux_video.c: each one changes a rule of the video path (the claim of a rectangle, the class of a tile, the sharpening, the gate of the window, the position of a frame, the
# veto, the REFRESH, the repair, the geometry, the first key frame); the tests of linux_video_test.c (without the decoder check) must fail for every one. Runs in a container with
# libopenh264 like run-linux-video-tests.sh (which has to have been run once: it builds the image). Usage: run-linux-video-mutations.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
IMG=kvm-h264test:bookworm
docker image inspect "$IMG" >/dev/null 2>&1 || { echo "run run-linux-video-tests.sh first (it builds $IMG)" >&2; exit 2; }
exec docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT/meshcore:/src/meshcore:ro" -w /tmp "$IMG" sh -c '
set -u
cp -r /src/meshcore /tmp/meshcore
cd /tmp/meshcore/KVM/Linux
C=../common
cp linux_video.c /tmp/orig.c
killed=0; total=0; invalid=0
mutate() {   # name, sed expression
    cp /tmp/orig.c linux_video.c
    sed -i "$2" linux_video.c
    if cmp -s /tmp/orig.c linux_video.c; then echo "MUTATION $1 did not change the file"; invalid=$((invalid + 1)); return; fi
    if ! gcc -std=gnu99 -O1 -DKVML_TEST -I/tmp -o /tmp/mut tests/linux_video_test.c linux_video.c linux_h264.c linux_kvm_stream.c $C/kvm_stream.c $C/kvm_video_core.c $C/kvm_annexb.c $C/kvm_motion.c $C/kvm_sharpen.c $C/kvm_video_rc.c $C/kvm_yuv.c $C/kvm_pace.c -lm -ldl >/dev/null 2>&1; then echo "MUTATION $1 does not compile"; invalid=$((invalid + 1)); return; fi
    total=$((total + 1))
    if timeout 120 /tmp/mut >/dev/null 2>&1; then echo "SURVIVED $1"; else killed=$((killed + 1)); echo "killed   $1"; fi
}
mutate claim-refused          "s|if (cls != 1 \\|\\| !model_current()|if (1 \\|\\| !model_current()|"
mutate class-never-motion     "s|return kvm_motion_class(&gV.motion, gV.now, row, col, gV.streak\\[i\\], dms(gV.now, gV.lastChange\\[i\\]));|return 0;|"
mutate sharpen-never          "s|if (!model_current() \\|\\| g_tileInfo == NULL \\|\\| gV.tiles.nVideo == 0) { return; }|return;|"
mutate jpeg-not-a-tile-reset  "s|			kvm_vtiles_jpeg(&gV.tiles, r, c);.*|			(void)0;|"
mutate window-gate-ignored    "s|in.gateOpen = kvm_stream_window_allows(s, KVM_GATE_VIDEO, now);|in.gateOpen = 1;|"
mutate frame-not-position     "s|kvml_write(gV.frame, total, KVM_W_POSITION)|kvml_write(gV.frame, total, KVM_W_DATA)|"
mutate veto-not-stopping      "s|if (gV.ctl.state == KVM_VS_ON \\&\\& !s->videoOn) { stop_video(\"stream layer\"); }|if (0) { stop_video(\"stream layer\"); }|"
mutate geometry-ignored       "s|	if (model_current()) { return; }|	return;|"
mutate pending-not-to-jpeg    "s|			if (map\\[((size_t)r \\* (size_t)gV.cols) + (size_t)c\\]) { g_tileInfo\\[r\\]\\[c\\].crc = 0xFF; }|			(void)map;|"
mutate level-limit-ignored    "s|kvm_vcore_pick_level((unsigned)cw, (unsigned)ch, gV.ctl.maxLevel, \\&level, \\&fpsCap) != 0|kvm_vcore_pick_level((unsigned)cw, (unsigned)ch, 0, \\&level, \\&fpsCap) != 0|"
mutate repair-ignored         "s|kvm_vctl_repair_due(&gV.ctl, now) \\&\\& have_model()|0 \\&\\& have_model()|"
mutate frames-not-landed      "s|(void)kvm_vtiles_land(&gV.tiles, 0, gV.map);|memset(gV.map, 0, (size_t)gV.rows * (size_t)gV.cols);|"
mutate no-sharpen-account     "s|	if (sharp \\&\\& bytes > 0)|	if (0)|"
mutate request-not-handled    "s|	for (i = 0; i < n; i++) { handle_request(i, now); }|	(void)n;|"
# not here, because they change nothing that a test can see (the settle time of 150 ms is also kept by the governor of kvm_sharpen: its bucket limits how many tiles are marked): the flag of the sharpening is cleared by the next change of the content (note_crc); a REFRESH is also seen by note_crc (the CRC 0xFF);
# the flat picture of the first key frame only makes it smaller (common/tests); the SPS check is kvm_vcore_check_sps (common/tests); a second note_crc of a tile in a frame sees the same CRC
cp /tmp/orig.c linux_video.c
echo "$killed of $total mutations killed ($invalid did not apply or compile)"
[ "$killed" -eq "$total" ] && [ "$invalid" -eq 0 ]
'
