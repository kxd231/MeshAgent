#!/bin/sh
# Mutation check of the tests of kvm_qpdet.c: each line below changes one key rule of the code in a copy, and the tests have to turn
# red (the test program exits with a failure); a mutation that does not compile is reported as invalid, one that the tests do not notice as SURVIVED (exit status 1 at the end).
# Usage: run-kvm-video-mutations.sh
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
kvm_qpdet.c @@ #define LN089 (-0.116533816) @@ #define LN089 (-0.0) @@ the expected effect of a QP step is nil
kvm_qpdet.c @@ #define LN089 (-0.116533816) @@ #define LN089 (0.116533816) @@ the expected effect has the wrong sign
kvm_qpdet.c @@ if (step >= KVM_QPDET_MIN_STEP && d->nHist >= KVM_QPDET_PRE) @@ if (step >= 1 && d->nHist >= KVM_QPDET_PRE) @@ a change of one step is an event
kvm_qpdet.c @@ if (step >= KVM_QPDET_MIN_STEP && d->nHist >= KVM_QPDET_PRE) @@ if (step >= KVM_QPDET_MIN_STEP) @@ an event without a history before it
kvm_qpdet.c @@ d->skipLeft = 2; @@ d->skipLeft = 0; @@ the frames right after the change are measured
kvm_qpdet.c @@ if (d->inEvent) { d->inEvent = 0; }			// moved again @@ if (0) { d->inEvent = 0; }			// moved again @@ an event goes on after the QP moved again
kvm_qpdet.c @@ else if (follow < 0.25) { l += (double)KVM_QPDET_REPORT_WEIGHT; d->repBad++; } @@ else if (follow < 0.25) { d->repBad++; } @@ a reported QP that does not follow is no evidence
kvm_qpdet.c @@ else if (follow < 0.25) { l += (double)KVM_QPDET_REPORT_WEIGHT; d->repBad++; } @@ else if (follow > 0.25) { l += (double)KVM_QPDET_REPORT_WEIGHT; d->repBad++; } @@ a reported QP that follows is evidence against
kvm_qpdet.c @@ if (d->llr >= (double)KVM_QPDET_LLR_LIMIT && @@ if (d->llr >= 0.5 && @@ INEFFECTIVE at the slightest evidence
kvm_qpdet.c @@ else if (d->llr <= -(double)KVM_QPDET_LLR_LIMIT) { d->verdict = KVM_QPDET_EFFECTIVE; } @@ else if (d->llr <= -0.5) { d->verdict = KVM_QPDET_EFFECTIVE; } @@ EFFECTIVE at the slightest evidence
kvm_qpdet.c @@ double l = (e * e - 2.0 * r * e) / (2.0 * s2); @@ double l = (e * e + 2.0 * r * e) / (2.0 * s2); @@ the likelihood ratio has the wrong sign of the observed change
kvm_qpdet.c @@ if (isIdr || qpAsked <= 0) { return d->verdict; } @@ if (qpAsked <= 0) { return d->verdict; } @@ key frames count as P frames
kvm_qpdet.c @@ ((d->probes == 0 && d->pFrames >= KVM_QPDET_PROBE_AFTER) @@ ((d->probes == 0 && d->pFrames >= 1) @@ the first probe at once
kvm_qpdet.c @@ || (d->probes > 0 && d->sinceProbe >= KVM_QPDET_PROBE_EVERY)) @@ || (d->probes > 0 && d->sinceProbe >= 1)) @@ the probes follow each other at once
kvm_qpdet.c @@ d->probes < KVM_QPDET_PROBE_MAX && @@ 1 && @@ there is no limit on the number of probes
kvm_qpdet.c @@ d->probeLeft == 0 || d->verdict != KVM_QPDET_UNKNOWN) { return 0; } @@ d->probeLeft == 0) { return 0; } @@ the probe goes on after the verdict
kvm_qpdet.c @@ if (qp + KVM_QPDET_PROBE_STEP <= maxQp) { return KVM_QPDET_PROBE_STEP; } @@ if (1) { return KVM_QPDET_PROBE_STEP; } @@ the probe steps over the ceiling
kvm_qpdet.c @@ if (qp - KVM_QPDET_PROBE_STEP >= minQp) { return -KVM_QPDET_PROBE_STEP; } @@ ; @@ the probe never goes down
kvm_qpdet.c @@ d->probes++; d->probeLeft = KVM_QPDET_PROBE_FRAMES; d->sinceProbe = 0; @@ d->probes++; d->probeLeft = KVM_QPDET_PROBE_FRAMES; @@ the distance between probes is counted from the start of the stream
kvm_qpdet.c @@ d->evPreBytes < (double)KVM_QPDET_MIN_BYTES || @@ 0 || @@ frames that are only overhead count
kvm_qpdet.c @@ (d->events > 0 && d->pFrames - d->lastEventAt < (uint32_t)KVM_QPDET_EVENT_GAP) @@ 0 @@ two events on the same content count twice
kvm_qpdet.c @@ if (areaRatio > 1.0 + (double)KVM_QPDET_AREA_TOL_PCT / 100.0 || @@ if (0 || @@ an event in which the area grew (a burst) counts
kvm_qpdet.c @@ areaRatio < 1.0 / (1.0 + (double)KVM_QPDET_AREA_TOL_PCT / 100.0) || @@ 0 || @@ an event in which the area shrank counts
kvm_qpdet.c @@ if (l > (double)KVM_QPDET_EVENT_MAX) { l = (double)KVM_QPDET_EVENT_MAX; } else if (l < -(double)KVM_QPDET_EVENT_MAX) { l = -(double)KVM_QPDET_EVENT_MAX; } @@ ; @@ one event may decide alone
kvm_qpdet.c @@ const double ln = kvm_qpdet_ln(bytes) - kvm_qpdet_ln(area > 0 ? area : 1u); @@ const double ln = kvm_qpdet_ln(bytes); @@ the size is not judged per covered tile
kvm_qpdet.c @@ if (follow >= 0.5) { l = -(double)KVM_QPDET_EVENT_MAX; d->repOk++; } @@ if (follow >= 0.5) { d->repOk++; } @@ a reported QP that follows does not decide the event
kvm_qpdet.c @@ && !(d->repOk > 0 && d->repBad == 0)) @@ ) @@ the verdict is ineffective although the reported QP followed and never failed to
kvm_qpdet.c @@ else if (floored) { l = 0.0; } @@ else if (0) { l = 0.0; } @@ the floor of the encoder counts as deafness
kvm_qpdet.c @@ (d->evPostRep == d->evPreRep && d->evPreRep > d->evFrom && d->evPostRep > d->evTo) @@ (d->evPostRep == d->evPreRep) @@ a reported QP that is stuck below the asked one counts as a floor
kvm_qpdet.c @@ if (!d->noProbes && d->verdict == KVM_QPDET_UNKNOWN && d->probeLeft == 0 @@ if (d->verdict == KVM_QPDET_UNKNOWN && d->probeLeft == 0 @@ a detector with the probes off arms a probe
kvm_qpdet.c @@ if (d->noProbes || d->probeLeft == 0 || d->verdict != KVM_QPDET_UNKNOWN) { return 0; } @@ if (d->probeLeft == 0 || d->verdict != KVM_QPDET_UNKNOWN) { return 0; } @@ the probe offset ignores the switch
kvm_qpdet.c @@ const uint8_t np = d->noProbes; const double ls = d->lnStep; memset(d, 0, sizeof(*d)); d->noProbes = np; d->lnStep = ls; @@ const double ls = d->lnStep; memset(d, 0, sizeof(*d)); d->lnStep = ls; @@ reset forgets that the probes are off
kvm_qpdet.c @@ d->noProbes = on ? 0 : 1; if (!on) { d->probeLeft = 0; } @@ d->noProbes = on ? 0 : 1; @@ a probe that was armed goes on after the probes were switched off (the offset is still 0, but the state is wrong)
kvm_qpdet.c @@ d->noProbes = on ? 0 : 1; @@ d->noProbes = on ? 1 : 0; @@ the switch is the wrong way round
kvm_qpdet.c @@ const double e = (double)step * (d->lnStep != 0.0 ? d->lnStep : LN089); @@ const double e = (double)step * LN089; @@ the expected step of the detector is always that of the software encoder
kvm_qpdet.c @@ d->noProbes = np; d->lnStep = ls; @@ d->noProbes = np; @@ a reset forgets the expected step
kvm_qpdet.c @@ if (pct < 50 || pct > 99) { d->lnStep = 0.0; return; } @@ if (0) { d->lnStep = 0.0; return; } @@ a percent out of range is taken
kvm_qpdet.c @@ d->lnStep = kvm_qpdet_ln((uint32_t)pct) - kvm_qpdet_ln(100u); @@ d->lnStep = kvm_qpdet_ln(100u) - kvm_qpdet_ln((uint32_t)pct); @@ the sign of the expected step
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
  if ! gcc -std=c99 -O1 -w -o /tmp/m/t /tmp/m/tests/kvm_qpdet_test.c /tmp/m/kvm_qpdet.c -lm 2>/tmp/m/err; then echo "INVALID  ($desc): does not compile"; invalid=$((invalid + 1)); continue; fi
  if /tmp/m/t >/tmp/m/out 2>&1; then echo "SURVIVED ($desc)"; survived=$((survived + 1)); else echo "killed   ($desc): $(grep -m1 FAIL /tmp/m/out | cut -c1-110)"; killed=$((killed + 1)); fi
done < /tmp/mutations.txt
echo "mutations: $n, killed $killed, survived $survived, invalid $invalid"
[ $survived -eq 0 ] && [ $invalid -eq 0 ]
' sh
