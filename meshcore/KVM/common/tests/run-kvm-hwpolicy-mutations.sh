#!/bin/sh
# Mutation check of the tests of kvm_hwpolicy.c: each line below changes one key rule of the code in a copy, and the tests have to turn
# red (the test program exits with a failure); a mutation that does not compile is reported as invalid, one that the tests do not notice as SURVIVED (exit status 1 at the end).
# Usage: run-kvm-hwpolicy-mutations.sh
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
set -u
cat > /tmp/mutations.txt <<"EOT"
kvm_hwpolicy.c @@ if (w < 0 || (size_t)w >= cap) { return 0; }\n	n = (size_t)w; @@ n = (size_t)w; @@ the cache format ignores a buffer that is too small
kvm_hwpolicy.c @@ if (!(tl == 5 && memcmp(tok, "kvmhw", 5) == 0)) { return 0; } @@ ; @@ a file that is not ours is read
kvm_hwpolicy.c @@ if (tl != 1 || tok[0] != 0x31) { return 0; } @@ ; @@ another version of the format is read
kvm_hwpolicy.c @@ ok = parse_u32(val, vn, &v) && v >= 1 && v <= 6; @@ ok = parse_u32(val, vn, &v); @@ a verdict out of range is accepted
kvm_hwpolicy.c @@ && kvm_hwp_find(c, e.key) == NULL) { c->e[c->n++] = e; } @@ ) { c->e[c->n++] = e; } @@ the same key twice
kvm_hwpolicy.c @@ || w == 0 || w > 9) { return 0; } @@ ) { return 0; } @@ an error without weight
kvm_hwpolicy.c @@ if ((int32_t)(c->e[i].used - c->e[old].used) < 0) { old = i; } @@ if (c->e[i].used < c->e[old].used) { old = i; } @@ the oldest entry is found without the wrap of the clock
kvm_hwpolicy.c @@ if ((int32_t)(c->e[i].used - c->e[old].used) < 0) { old = i; } @@ if ((int32_t)(c->e[i].used - c->e[old].used) > 0) { old = i; } @@ the newest entry is dropped
kvm_hwpolicy.c @@ if ((int32_t)age < 0 || age <= KVM_HWP_ERROR_TTL_S) { w += e->errs[i].weight; } @@ if (age <= KVM_HWP_ERROR_TTL_S) { w += e->errs[i].weight; } @@ an error from the future does not count
kvm_hwpolicy.c @@ if ((int32_t)age < 0 || age <= KVM_HWP_ERROR_TTL_S) { w += e->errs[i].weight; } @@ if ((int32_t)age < 0 || age < KVM_HWP_ERROR_TTL_S) { w += e->errs[i].weight; } @@ an error that is exactly as old as the TTL does not count
kvm_hwpolicy.c @@ if ((int32_t)age < 0 || age <= KVM_HWP_ERROR_TTL_S) { w += e->errs[i].weight; } @@ { w += e->errs[i].weight; } @@ errors never expire
kvm_hwpolicy.c @@ kvm_hwp_error_weight(e, nowSec) >= (unsigned)KVM_HWP_BAD_WEIGHT || testsCount @@ kvm_hwp_error_weight(e, nowSec) > (unsigned)KVM_HWP_BAD_WEIGHT || testsCount @@ two errors are not bad yet
kvm_hwpolicy.c @@ kvm_hwp_error_weight(e, nowSec) >= (unsigned)KVM_HWP_BAD_WEIGHT || testsCount @@ kvm_hwp_error_weight(e, nowSec) >= (unsigned)KVM_HWP_BAD_WEIGHT @@ tests that did not pass never make an entry bad
kvm_hwpolicy.c @@ if (verdict == KVM_HWP_V_PASS) { e->attempts = 0; } @@ if (0) { e->attempts = 0; } @@ a passed test does not clear the failed attempts
kvm_hwpolicy.c @@ else if (verdict == KVM_HWP_V_FAIL || verdict == KVM_HWP_V_CRASH || verdict == KVM_HWP_V_TIMEOUT) @@ else if (verdict != KVM_HWP_V_PASS) @@ a slow test counts as failed
kvm_hwpolicy.c @@ if (e->verdict == KVM_HWP_V_PASS || e->verdict == KVM_HWP_V_NONE) { e->verdict = KVM_HWP_V_RETEST; e->at = nowSec; } @@ ; @@ an error in a session leaves the verdict PASS
kvm_hwpolicy.c @@ if (weight > 9) { weight = 9; } @@ ; @@ an error with an enormous weight
kvm_hwpolicy.c @@ if (weight < 1) { weight = 1; } @@ ; @@ an error with no weight
kvm_hwpolicy.c @@ if (!c->trying || alive) { return 0; } @@ if (!c->trying) { return 0; } @@ a marker of a live process is a crash
kvm_hwpolicy.c @@ kvm_hwp_note_error(c, c->tryKey, 1, nowSec); }	// nobody resolved it @@ kvm_hwp_note_error(c, c->tryKey, 2, nowSec); }	// nobody resolved it @@ a marker that nobody resolved counts as an exception
kvm_hwpolicy.c @@ kvm_hwp_end_try(c);\n	return 1; @@ return 1; @@ the marker of a crash stays
kvm_hwpolicy.c @@ if (strcmp(value, "0") == 0) { return KVM_HWP_ENV_OFF; } @@ ; @@ MESH_KVM_VIDEO_HW=0 does not switch it off
kvm_hwpolicy.c @@ if (i - a == 2 && memcmp(text + a, "sw", 2) == 0) { return KVM_HWP_MODE_SW; } @@ ; @@ mode sw is not read
kvm_hwpolicy.c @@ if (in->env == KVM_HWP_ENV_OFF) { out->why = "env_off"; return; } @@ ; @@ the switch off is ignored
kvm_hwpolicy.c @@ if (in->processDisabled) { out->why = "process_disabled"; return; } @@ ; @@ the hardware is used again after an error in this process
kvm_hwpolicy.c @@ if (!in->hwPresent) { out->why = "no_hw"; return; } @@ ; @@ a self test on a machine without a hardware encoder
kvm_hwpolicy.c @@ if (e != NULL && kvm_hwp_is_bad(e, in->nowSec) && in->env != KVM_HWP_ENV_FORCE && in->mode != KVM_HWP_MODE_HW) @@ if (0) @@ a bad entry is used
kvm_hwpolicy.c @@ if (e != NULL && kvm_hwp_is_bad(e, in->nowSec) && in->env != KVM_HWP_ENV_FORCE && in->mode != KVM_HWP_MODE_HW) @@ if (e != NULL && kvm_hwp_is_bad(e, in->nowSec)) @@ force does not override a bad entry
kvm_hwpolicy.c @@ if (!in->displayActive && !in->hwRunning) { out->why = "gdi_no_output"; return; } @@ ; @@ the hardware starts in the lid-closed state
kvm_hwpolicy.c @@ if (!in->displayActive && !in->hwRunning) { out->why = "gdi_no_output"; return; } @@ if (!in->displayActive) { out->why = "gdi_no_output"; return; } @@ the hardware that runs is dropped when the lid closes
kvm_hwpolicy.c @@ if (in->testRunning) { out->why = "test_running"; return; } @@ ; @@ a second self test is started while one runs
kvm_hwpolicy.c @@ if (in->testDone) { out->why = "test_done"; return; } @@ ; @@ a second self test in one process
kvm_hwpolicy.c @@ if (!in->displayActive) { out->why = "gdi_no_output"; return; }\n	if (!in->desktopDefault) @@ if (!in->desktopDefault) @@ the self test starts without an active output
kvm_hwpolicy.c @@ if (!in->desktopDefault) { out->why = "winlogon"; return; } @@ ; @@ the self test starts on the Winlogon desktop
kvm_hwpolicy.c @@ if ((uint32_t)(in->nowSec - e->at) >= KVM_HWP_SLOW_RETEST_S) @@ if ((uint32_t)(in->nowSec - e->at) > KVM_HWP_SLOW_RETEST_S) @@ a slow verdict is retested a second late
kvm_hwpolicy.c @@ if ((uint32_t)(in->nowSec - e->at) >= KVM_HWP_SLOW_RETEST_S) { needTest = 1; } else { out->why = "slow"; return; } @@ needTest = 1; @@ a slow verdict is tested at once again
kvm_hwpolicy.c @@ else if (e->verdict != KVM_HWP_V_PASS) { needTest = 1; }	// FAIL, CRASH, TIMEOUT that are not BAD yet: one more try @@ ; @@ a test that failed once is not repeated
kvm_hwpolicy.c @@ if (mbs <= 8160ul) { return 1; } @@ if (mbs <= 8000ul) { return 1; } @@ 1920x1088 is no size class 1
kvm_hwpolicy.c @@ if (codedW < 16 || codedH < 16 || mbs > 36864ul) { return 0; } @@ if (codedW < 16 || codedH < 16) { return 0; } @@ a size above 4K is tried
kvm_hwpolicy.c @@ drv, s->osBuild, @@ drv, 0u, @@ the key does not hold the Windows build
kvm_hwpolicy.c @@ ,drv:%s,os:%u,tok:%d,sz:%d", s->rev, name, clsid, s->vendor & 0xFFFFu, s->device & 0xFFFFu, s->subsys, s->adapterRev & 0xFFu, drv, @@ ,drv:%s,os:%u,tok:%d,sz:%d", s->rev, name, clsid, s->vendor & 0xFFFFu, s->device & 0xFFFFu, s->subsys, s->adapterRev & 0xFFu, "x", @@ the key does not hold the driver
kvm_hwpolicy.c @@ if (w < 0 || (size_t)w >= cap || (size_t)w >= KVM_HWP_KEY_MAX) { if (cap > 0) { out[0] = 0; } return 0; } @@ if (w < 0) { return 0; } @@ a key that does not fit is used
kvm_hwpolicy.c @@ if (tl == 3 && memcmp(tok, "end", 3) == 0) { r->valid = seenVerdict ? 1 : 0; return; } @@ if (tl == 3 && memcmp(tok, "end", 3) == 0) { r->valid = 1; return; } @@ a result without a verdict is valid
kvm_hwpolicy.c @@ if (parse_u32(val, vn, &v) && (v == KVM_HWP_V_PASS || v == KVM_HWP_V_FAIL || v == KVM_HWP_V_SLOW)) @@ if (parse_u32(val, vn, &v)) @@ a result with a verdict that is none of PASS, FAIL, SLOW
kvm_hwpolicy.c @@ if (!(tl == 9 && memcmp(tok, "kvmhwtest", 9) == 0)) { return; } @@ ; @@ a text that is no result is read
kvm_hwpolicy.c @@ if (!c->trying || c->tryPid != pid) { return 0; } @@ if (!c->trying) { return 0; } @@ the parent resolves the marker of another child
kvm_hwpolicy.c @@ return (exitCode >= KVM_HWP_CRASH_STATUS || exitCode == KVM_HWP_EXC_EXIT) ? 1 : 0; @@ return 1; @@ a session that the parent ended counts as a crash
kvm_hwpolicy.c @@ return (exitCode >= KVM_HWP_CRASH_STATUS || exitCode == KVM_HWP_EXC_EXIT) ? 1 : 0; @@ return (exitCode > KVM_HWP_CRASH_STATUS || exitCode == KVM_HWP_EXC_EXIT) ? 1 : 0; @@ the first status of a crash is not one
kvm_hwpolicy.c @@ if (kvm_hwp_exit_counts(exitCode) && c->tryKey[0] != 0) { kvm_hwp_note_error(c, c->tryKey, 2, nowSec); } @@ if (kvm_hwp_exit_counts(exitCode) && c->tryKey[0] != 0) { kvm_hwp_note_error(c, c->tryKey, 1, nowSec); } @@ a crash is one error only
kvm_hwpolicy.c @@ if (kvm_hwp_exit_counts(exitCode) && c->tryKey[0] != 0) { kvm_hwp_note_error(c, c->tryKey, 2, nowSec); }\n	kvm_hwp_end_try(c); @@ if (kvm_hwp_exit_counts(exitCode) && c->tryKey[0] != 0) { kvm_hwp_note_error(c, c->tryKey, 2, nowSec); } @@ the marker stays after the parent resolved it
kvm_hwpolicy.c @@ if (e->attempts > 0 && (int32_t)age >= 0 && age > KVM_HWP_ERROR_TTL_S) { e->attempts = 0; } @@ ; @@ old failed tests count
kvm_hwpolicy.c @@ e->failAt = nowSec; @@ ; @@ the time of the failed test is not remembered
kvm_hwpolicy.c @@ const uint32_t age = nowSec - e->failAt;\n	const int testsCount @@ const uint32_t age = 0;\n	const int testsCount @@ failed tests never expire
kvm_hwpolicy.c @@ if (!c->tryUsed && c->tryKey[0] != 0) { kvm_hwp_note_error @@ if (c->tryKey[0] != 0) { kvm_hwp_note_error @@ a marker in use that nobody resolved counts as an error of the encoder
kvm_hwpolicy.c @@ return (exitCode >= KVM_HWP_CRASH_STATUS || exitCode == KVM_HWP_EXC_EXIT) ? 1 : 0; @@ return (exitCode >= KVM_HWP_CRASH_STATUS) ? 1 : 0; @@ the exit code of a child that ended itself after an exception is no error
kvm_hwpolicy.c @@ if (c->trying && c->tryPid == pid) { c->tryUsed = 1; } @@ if (c->trying) { c->tryUsed = 1; } @@ the first access unit of another process marks the marker as in use
kvm_hwpolicy.c @@ if (c->trying && c->tryPid == pid) { c->tryUsed = 1; } @@ ; @@ the marker is never in use
kvm_hwpolicy.c @@ c->tryUsed = used != 0 ? 1 : 0; @@ c->tryUsed = 0; @@ the file forgets that the marker is in use
kvm_hwpolicy.c @@ (unsigned)(c->tryUsed ? 1 : 0), c->tryKey @@ 0u, c->tryKey @@ the file never says that the marker is in use
kvm_hwpolicy.c @@ c->trying = 0; c->tryPid = 0; c->tryAt = 0; c->tryUsed = 0; @@ c->trying = 0; c->tryPid = 0; c->tryAt = 0; @@ a cleared marker stays in use
kvm_hwpolicy.c @@ if (exitCode == KVM_HWP_STATUS_CONTROL_C || exitCode == KVM_HWP_STATUS_LOGOFF) { return 0; } @@ ; @@ the exit codes of a logoff count as a fault of the hardware
kvm_hwpolicy.c @@ exitCode == KVM_HWP_STATUS_CONTROL_C || exitCode == KVM_HWP_STATUS_LOGOFF @@ exitCode == KVM_HWP_STATUS_CONTROL_C @@ the code of a logoff counts as a fault of the hardware
kvm_hwpolicy.c @@ exitCode == KVM_HWP_STATUS_CONTROL_C || exitCode == KVM_HWP_STATUS_LOGOFF @@ exitCode == KVM_HWP_STATUS_LOGOFF @@ the code of a Ctrl-C exit counts as a fault of the hardware
kvm_hwpolicy.c @@ if (kvm_hwp_exit_counts(exitCode) && c->tryKey[0] != 0) { @@ if (c->tryKey[0] != 0) { @@ every exit code counts
kvm_hwpolicy.c @@ if (e != NULL) { e->flatMilli = flatMilli > 4000u ? 4000u : flatMilli; } @@ if (e != NULL) { e->flatMilli = flatMilli; } @@ the floor of a flat key frame is not limited
kvm_hwpolicy.c @@ else if (tok_val(tok, tl, "flat", &val, &vn)) { ok = parse_u32(val, vn, &e.flatMilli); } @@ else if (tok_val(tok, tl, "flat", &val, &vn)) { ok = 1; } @@ the cache forgets the floor of a flat key frame
kvm_hwpolicy.c @@ else if (tok_val(tok, tl, "flat", &val, &vn)) { parse_u32(val, vn, &r->flatMilli); } @@ else if (tok_val(tok, tl, "flat", &val, &vn)) { ; } @@ the result of the self test forgets the floor of a flat key frame
EOT
survived=0; invalid=0; killed=0; n=0
while IFS= read -r line; do
  [ -z "$line" ] && continue
  n=$((n + 1))
  file=$(printf "%s" "$line" | awk -F " @@ " "{print \$1}")
  search=$(printf "%s" "$line" | awk -F " @@ " "{print \$2}")
  replace=$(printf "%s" "$line" | awk -F " @@ " "{print \$3}")
  desc=$(printf "%s" "$line" | awk -F " @@ " "{print \$4}")
  rm -rf /tmp/m && cp -r /src /tmp/m
  S="$search" R="$replace" perl -0pi -e "BEGIN { \$s = \$ENV{S}; \$r = \$ENV{R}; \$s =~ s/\\\\n/\\n/g; \$r =~ s/\\\\n/\\n/g; } \$c += s/\\Q\$s\\E/\$r/g; END { exit(\$c >= 1 ? 0 : 3) }" /tmp/m/$file
  rc=$?
  if [ $rc -ne 0 ]; then echo "INVALID  ($desc): the text was not found"; invalid=$((invalid + 1)); continue; fi
  if ! gcc -std=c99 -O1 -w -o /tmp/m/t /tmp/m/tests/kvm_hwpolicy_test.c /tmp/m/kvm_hwpolicy.c 2>/tmp/m/err; then echo "INVALID  ($desc): does not compile"; invalid=$((invalid + 1)); continue; fi
  if /tmp/m/t >/tmp/m/out 2>&1; then echo "SURVIVED ($desc)"; survived=$((survived + 1)); else echo "killed   ($desc): $(grep -m1 FAIL /tmp/m/out | cut -c1-110)"; killed=$((killed + 1)); fi
done < /tmp/mutations.txt
echo "mutations: $n, killed $killed, survived $survived, invalid $invalid"
[ $survived -eq 0 ] && [ $invalid -eq 0 ]
' sh
