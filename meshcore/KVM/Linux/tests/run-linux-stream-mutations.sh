#!/bin/sh
# Mutations of linux_kvm_stream.c: each one changes a rule of the glue (the gate, the marker, the queue of the ACKs, the announcement, the veto, the switch-off, the scan tile,
# the counting of the writes); the tests of linux_kvm_stream_test.c must fail for every one. Runs in a container like run-linux-stream-tests.sh.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
IMG=kvm-ctest:local
docker image inspect "$IMG" >/dev/null 2>&1 || { echo "run run-linux-stream-tests.sh first (it builds $IMG)" >&2; exit 2; }
exec docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT/meshcore:/src/meshcore:ro" -w /tmp "$IMG" sh -c '
set -u
cp -r /src/meshcore /tmp/meshcore
cd /tmp/meshcore/KVM/Linux
cp linux_kvm_stream.c /tmp/orig.c
killed=0; total=0
mutate() {   # name, sed expression
    total=$((total + 1))
    cp /tmp/orig.c linux_kvm_stream.c
    sed -i "$2" linux_kvm_stream.c
    if cmp -s /tmp/orig.c linux_kvm_stream.c; then echo "MUTATION $1 did not change the file"; return; fi
    if gcc -std=gnu99 -O1 -DKVML_TEST -DKVM_VIDEO_DISABLE -I/tmp -o /tmp/mut tests/linux_kvm_stream_test.c linux_kvm_stream.c ../common/kvm_stream.c >/dev/null 2>&1 && timeout 20 /tmp/mut >/dev/null 2>&1; then echo "SURVIVED $1"; else killed=$((killed + 1)); echo "killed   $1"; fi
}
mutate gate-always-open        "s|ok = kvm_stream_window_allows(&gStream, KVM_GATE_BIG, now);|ok = 1; (void)now;|"
mutate gate-small-kind         "s|KVM_GATE_BIG, now);|KVM_GATE_SHARPEN, now);|"
mutate refusal-not-noted       "s|if (!ok) { (void)kvm_stream_note_link_held(&gStream, now); }||"
mutate no-marker               "s|if (kvm_stream_marker_wanted(&gStream, now, 0, &probe))|if (0)|"
mutate marker-as-data          "s|(void)kvml_write(marker, KVM_MARKER_BYTES, KVM_W_POSITION);|(void)kvml_write(marker, KVM_MARKER_BYTES, KVM_W_DATA);|"
mutate acks-ignored            "s|(void)kvm_stream_on_ack(&gStream, &a, it->at);|(void)a;|"
mutate ack-stamped-late        "s|it.at = kvml_now();|it.at = kvml_now() + 500;|"
mutate ack-queue-not-drained   "s|^		gAckCount--;|		gAckCount -= 1; gAckHead = 0; gAckCount = 0;|"
mutate ctrl-ignored            "s|if (kvm_stream_on_ctrl(&gStream, &c, ctl\[i\].at, &reply, &actions)) { kvml_state(&reply); }|(void)kvm_stream_on_ctrl(\&gStream, \&c, ctl[i].at, \&reply, \&actions);|"
mutate no-announce             "s|kvm_stream_start_state(&gStream, &m);|memset(\&m, 0, sizeof(m)); m.ver = 0;|"
mutate announce-without-caps   "s|kvm_stream_init(&gStream, capVideo, capAck, |kvm_stream_init(\&gStream, capVideo, 0, |"
mutate ack-switch-off-ignored  "s|if (kvml_env_off(\"MESH_KVM_ACK\")) { capAck = 0; }||"
mutate video-build-reason      "s|capVideo ? KVM_REASON_OK : videoReason|KVM_REASON_DISABLED|"
mutate tick-missing            "s|if (kvm_stream_tick(&gStream, now, 0, &reply)) { kvml_state(&reply); }||"
mutate write-not-counted       "s|kvm_stream_note_write(&gStream, len, kind, t0);||"
mutate blocked-not-noted       "s|if ((uint32_t)(t1 - t0) > KVML_BLOCKED_WRITE_MS) { kvm_stream_note_blocked(&gStream, t1); }||"
mutate write-error-lost        "s|r = kvm_slave_write_raw(buffer, len);|r = kvm_slave_write_raw(buffer, len); r = 0;|"
mutate scan-tile-kept          "s|^	gResumeTile = 0;\$|	(void)0;|"
mutate scan-tile-ignored       "s|return (r > 0 \&\& r < tiles) ? r : 0;|return 0;|"
mutate scan-tile-out-of-range  "s|return (r > 0 \&\& r < tiles) ? r : 0;|return r;|"
mutate deferred-marked-kept     "s|if (g_tileInfo\[r\]\[c\].flag == TILE_MARKED_NOT_SENT) { g_tileInfo\[r\]\[c\].crc = 0xFF; }|(void)0;|"
mutate deferred-all-reset        "s|if (g_tileInfo\[r\]\[c\].flag == TILE_MARKED_NOT_SENT) { g_tileInfo\[r\]\[c\].crc = 0xFF; }|g_tileInfo[r][c].crc = 0xFF;|"
mutate bad-input-accepted      "s|if (kvm_stream_parse_ctrl(it.b, KVM_CTRL_SIZE, \&c) != KVM_REJ_NONE) { kvml_input_bad(); return; }||"
mutate state-not-written       "s|(void)kvml_write(b, KVM_CTRL_SIZE, KVM_W_STATE);||"
cp /tmp/orig.c linux_kvm_stream.c
echo "$killed of $total mutations killed"
[ "$killed" -eq "$total" ]
'
