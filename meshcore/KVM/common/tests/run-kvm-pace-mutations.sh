#!/bin/sh
# Mutation check of the tests of kvm_pace.c: each line below changes one rule of the code in a copy, and the tests (tests/kvm_pace_test.c with the simulation) have to turn red; a mutation
# that does not compile is reported as invalid, one that the tests do not notice as SURVIVED (exit status 1 at the end).
# Usage: run-kvm-pace-mutations.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
COMMON=$(dirname "$HERE")
IMG=kvm-ctest:local
docker image inspect "$IMG" >/dev/null 2>&1 || { echo "run run-kvm-pace-tests.sh first (it builds the image)"; exit 2; }
exec docker run --rm --user "$(id -u):$(id -g)" -v "$COMMON:/src:ro" -w /tmp "$IMG" sh -c '
set -u
cat > /tmp/mutations.txt <<"EOT"
kvm_pace.c @@ next = cur + periodMs; @@ next = cur + periodMs + 1; @@ tick: the slot is one millisecond too far (the rate drifts down)
kvm_pace.c @@ next = cur + periodMs; @@ next = now + periodMs; @@ tick: the next slot counts from the end of the work (the old rule, the lateness of every wake adds up)
kvm_pace.c @@ if (over >= (int32_t)periodMs) { t->st.resync++; next = now; } @@ ; @@ tick: a debt of many periods is paid back with a burst
kvm_pace.c @@ if (over >= (int32_t)periodMs) @@ if (over > 0) @@ tick: every overrun starts the schedule again (no payback of a small debt)
kvm_pace.c @@ if (t->have && dms(started, t->due) < -(int32_t)KVM_PACE_EARLY_MS) @@ if (0) @@ tick: an iteration woken early uses up the slot of the schedule
kvm_pace.c @@ void kvm_tick_idle(kvm_tick *t) { t->have = 0; } @@ void kvm_tick_idle(kvm_tick *t) { (void)t; } @@ tick: an idle loop keeps its schedule (credit for the time nothing happened)
kvm_pace.c @@ if (!t->have) { cur = started; } @@ if (!t->have) { cur = now; } @@ tick: the first iteration after an idle loop is counted from its end
kvm_pace.c @@ if (over > 0) { t->st.over++; } @@ ; @@ tick: overruns are not counted
kvm_pace.c @@ s = lastSlot + intervalMs; @@ s = now; @@ slot: a frame belongs to the moment it was written
kvm_pace.c @@ if (late >= (int32_t)intervalMs) @@ if (0) @@ slot: a whole interval behind does not start again (a burst of frames)
kvm_pace.c @@ return sleep_until(lastSlot + intervalMs, now); @@ return sleep_until(lastSlot + intervalMs + 1, now); @@ wake: the loop sleeps past the slot
kvm_pace.c @@ if (!haveLast || intervalMs == 0) { return 0; } @@ if (intervalMs == 0) { return 0; } @@ wake: the first frame waits for a slot that does not exist
kvm_pace.c @@ m = ((uint64_t)b->cores * 1000u * KVM_PACE_CPU_SHARE_PCT) / 100u; @@ m = (uint64_t)b->cores * 1000u; @@ budget: the whole machine
kvm_pace.c @@ m = ((uint64_t)b->cores * 1000u * KVM_PACE_CPU_SHARE_PCT) / 100u; @@ m = ((uint64_t)b->cores * 1000u * KVM_PACE_CPU_SHARE_PCT) / 100u; if (m < 1000u) { m = 1000u; } @@ budget: at least one core also on one processor (the whole machine)
kvm_pace.c @@ if (par10 > (uint64_t)b->cores * 10u) { par10 = (uint64_t)b->cores * 10u; } @@ ; @@ floor: the encoder keeps more cores busy than the machine has
kvm_pace.c @@ den = (uint64_t)10u * b->budgetMilli; @@ den = (uint64_t)10u * 1000u; @@ floor: the budget is always one core
kvm_pace.c @@ return (uint32_t)((need + den - 1u) / den); @@ return (uint32_t)(need / den); @@ floor: rounded down (the encoder is over its budget by up to a millisecond)
kvm_pace.c @@ if (f > v) { v = f; } @@ ; @@ interval: the floor of the encoder is ignored
kvm_pace.c @@ if (v > KVM_PACE_INTERVAL_MAX_MS && v > periodMs) { v = periodMs > KVM_PACE_INTERVAL_MAX_MS ? periodMs : KVM_PACE_INTERVAL_MAX_MS; } @@ ; @@ interval: no maximum
kvm_pace.c @@ if (p < 10u) { p = 10u; } @@ ; @@ window: a parallelism below one core is believed
kvm_pace.c @@ if (p > lim) { p = lim; } @@ ; @@ window: a parallelism above the cores is believed
kvm_pace.c @@ if (wall >= (uint64_t)KVM_PACE_MIN_ENC_MS * 1000u && cpu > 0) @@ if (1) @@ window: a window without encoding or without a readable clock moves the estimate
kvm_pace.c @@ if (avg == b->parX10 && p != b->parX10) { avg = p > b->parX10 ? avg + 1u : avg - 1u; } @@ ; @@ window: the estimate stops one step short of the value
kvm_pace.c @@ b->parX10 = avg; @@ ; @@ window: the estimate is never updated
kvm_pace.c @@ b->winStart = now; b->winWallUs = b->winCpuUs = 0;\n	b->windows++; @@ b->winStart = now;\n	b->windows++; @@ window: the sums of the encode calls are not cleared
kvm_pace.c @@ if (dms(now, b->winStart) < (int32_t)KVM_PACE_WINDOW_MS) { return 0; } @@ if (dms(now, b->winStart) < 1) { return 0; } @@ window: closed at every call
kvm_video_core.c @@ return kvm_vctl_write_wait_ms(c, now, intervalMs) == 0; @@ return 1; @@ the burst after a late frame (the write gap of the pace)
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
  if ! gcc -std=c99 -O1 -w -o /tmp/m/t /tmp/m/tests/kvm_pace_test.c /tmp/m/kvm_pace.c /tmp/m/kvm_video_core.c /tmp/m/kvm_stream.c /tmp/m/kvm_annexb.c 2>/tmp/m/err; then echo "INVALID  ($desc): does not compile"; invalid=$((invalid + 1)); continue; fi
  if /tmp/m/t >/tmp/m/out 2>&1; then echo "SURVIVED ($desc)"; survived=$((survived + 1)); else echo "killed   ($desc): $(grep -m1 FAIL /tmp/m/out | cut -c1-110)"; killed=$((killed + 1)); fi
done < /tmp/mutations.txt
echo "mutations: $n, killed $killed, survived $survived, invalid $invalid"
[ $survived -eq 0 ] && [ $invalid -eq 0 ]
' sh
