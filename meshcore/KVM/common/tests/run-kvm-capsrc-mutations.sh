#!/bin/sh
# Mutation check of the tests and the simulation of kvm_capsrc.c: each line below changes one rule of the code in a copy, and the tests have to turn red (the test program
# exits with a failure); a mutation that does not compile is reported as invalid, one that the tests do not notice as SURVIVED (exit status 1 at the end).
# Line format: file @@ text @@ replacement @@ description. Usage: run-kvm-capsrc-mutations.sh
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
kvm_capsrc.c @@ if (refPx == 0) { c->diffRun = 0; return KVM_CAPSRC_ACT_NONE; } @@ if (0) { c->diffRun = 0; return KVM_CAPSRC_ACT_NONE; } @@ a static screen counts as silence (a change of the screen is not needed)
kvm_capsrc.c @@ if (!refOk) { c->needRef = 1; return KVM_CAPSRC_ACT_NONE; }\t// a failed capture of the reference must not end the proof of silence for good: nothing else would ask again\n\t\tif (refPx == 0) @@ if (refPx == 0) @@ silence is judged without a usable reference
kvm_capsrc.c @@ const int refOk = (refPx >= 0 && kvm_capsrc_ref_usable(c)); @@ const int refOk = (refPx >= 0); @@ a reference that images have outdated is used
kvm_capsrc.c @@ const int need = KVM_CAPSRC_CONFIRM_DIFFS; @@ const int need = 1; @@ one difference is enough to switch to GDI
kvm_capsrc.c @@ if (c->updates == 0)\n	{\n		// DXGI has reported @@ if (0)\n	{\n		// DXGI has reported @@ silence is never told from a difference (the old behaviour: reopen, then the latch)
kvm_capsrc.c @@ if (persistentPx < 0 && refPx < 0)\n	{\n		// Nothing is learned @@ if (0)\n	{\n		// Nothing is learned @@ a failed comparison counts as a comparison
kvm_capsrc.c @@ c->cmpCount++;\n	if (c->probation) @@ if (c->probation) @@ the fast comparisons never end
kvm_capsrc.c @@ if (persistentPx == 0)\n	{\n		// The picture DXGI keeps @@ if (0)\n	{\n		// The picture DXGI keeps @@ equal pictures do not end the "after re-creation" state or the latch
kvm_capsrc.c @@ if (c->systematic) { c->systematic = 0; c->st.cleared++; act = KVM_CAPSRC_ACT_CLEARED; } @@ ; @@ the latch is not cleared when the pictures agree
kvm_capsrc.c @@ if (c->systematic && dms(now, c->systematicAt) >= (int32_t)KVM_CAPSRC_SYSTEMATIC_MS) { c->systematic = 0; c->afterReinit = 0; } @@ ; @@ the latch never expires
kvm_capsrc.c @@ if (refOk && refPx > 0)\n		{\n			if (++c->diffRun >= need) @@ if (0)\n		{\n			if (++c->diffRun >= need) @@ silence behind the latch is not recognised (the picture stands for good)
kvm_capsrc.c @@ if (refOk && refPx > 0) @@ if (refPx > 0) @@ behind the latch a reference that images have outdated proves silence
kvm_capsrc.c @@ if (!refOk || refPx > 0) { c->needRef = 1; } @@ ; @@ behind the latch the reference is never moved on
kvm_capsrc.c @@ if (!refOk || refPx > 0) { c->needRef = 1; } @@ if (refPx > 0) { c->needRef = 1; } @@ behind the latch an outdated reference is not replaced
kvm_capsrc.c @@ c->needRef = 1;		// behind the latch @@ ;		// behind the latch @@ the latch does not ask for a reference
kvm_capsrc.c @@ c->systematic = 0;\n	c->afterReinit = 0;\n	c->diffRun = 0;\n	if (c->mode == KVM_CAPSRC_MODE_GDI)\n	{\n		// A probe @@ if (c->mode == KVM_CAPSRC_MODE_GDI)\n	{\n		// A probe @@ a reset does not end the latches
kvm_capsrc.c @@ 		c->probation = 0;\n		c->nextProbe = now; @@ 		c->nextProbe = now; @@ a reset does not end a probe in the shadow
kvm_capsrc.c @@ 		c->nextProbe = now;\n	} @@ 		;\n	} @@ a change of geometry does not make DXGI be tried at once
kvm_capsrc.c @@ for (i = 0; i < c->fails && iv < KVM_CAPSRC_PROBE_MAX_MS; i++) { iv *= 2u; } @@ ; @@ the pause between probes never grows
kvm_capsrc.c @@ return iv > KVM_CAPSRC_PROBE_MAX_MS ? KVM_CAPSRC_PROBE_MAX_MS : iv; @@ return iv; @@ the pause between probes has no cap
kvm_capsrc.c @@ c->fails++;\n		c->st.probeFails++; @@ c->st.probeFails++; @@ a failed probe does not lengthen the pause
kvm_capsrc.c @@ c->fails = 0;\n	c->reason = KVM_CAPSRC_R_NONE;\n	c->st.recovered++; @@ c->reason = KVM_CAPSRC_R_NONE;\n	c->st.recovered++; @@ a recovery does not reset the pause
kvm_capsrc.c @@ c->mode = KVM_CAPSRC_MODE_DXGI;\n	c->fails = 0; @@ c->fails = 0; @@ a recovery does not make DXGI the source
kvm_capsrc.c @@ c->st.recovered++;\n	restart_counts(c, now); @@ c->st.recovered++; @@ the persistent image after a recovery is not started again
kvm_capsrc.c @@ if (c->updates >= KVM_CAPSRC_PROBE_UPDATES && dms(now, c->probeAt) >= (int32_t)KVM_CAPSRC_PROBE_MIN_MS) { return recover(c, now); } @@ if (dms(now, c->probeAt) >= (int32_t)KVM_CAPSRC_PROBE_MIN_MS) { return recover(c, now); } @@ a probe succeeds without a single image (a static screen)
kvm_capsrc.c @@ if (c->updates >= KVM_CAPSRC_PROBE_UPDATES && dms(now, c->probeAt) >= (int32_t)KVM_CAPSRC_PROBE_MIN_MS) { return recover(c, now); } @@ if (c->updates >= KVM_CAPSRC_PROBE_UPDATES) { return recover(c, now); } @@ a probe succeeds before it ran for the minimum time
kvm_capsrc.c @@ 	c->noted = 1;\n	if (++c->diffRun >= KVM_CAPSRC_PROBE_NEED) @@ 	if (++c->diffRun >= KVM_CAPSRC_PROBE_NEED) @@ a difference noted in a probe is forgotten
kvm_capsrc.c @@ 		c->diffRun = 0;\n		c->noted = 0; @@ 		c->diffRun = 0; @@ a difference that was found gone stays noted
kvm_capsrc.c @@ if (++c->diffRun >= KVM_CAPSRC_PROBE_NEED) @@ if (++c->diffRun >= 1) @@ a probe fails on a single difference (a frame that is late)
kvm_capsrc.c @@ return c->noted ? enter_gdi(c, now, KVM_CAPSRC_R_SILENT) : recover(c, now); @@ return recover(c, now); @@ a probe with a difference noted is a success at the end of the time
kvm_capsrc.c @@ if (c->noted) { return enter_gdi(c, now, KVM_CAPSRC_R_SILENT); } @@ ; @@ a probe with a difference noted is inconclusive at the end of the time
kvm_capsrc.c @@ (c->updates >= KVM_CAPSRC_PROBE_UPDATES ? KVM_CAPSRC_PROBE_BUSY_MS : KVM_CAPSRC_PROBATION_MS) @@ KVM_CAPSRC_PROBATION_MS @@ a busy screen waits for the whole time of a probe
kvm_capsrc.c @@ (c->updates >= KVM_CAPSRC_PROBE_UPDATES ? KVM_CAPSRC_PROBE_BUSY_MS : KVM_CAPSRC_PROBATION_MS) @@ KVM_CAPSRC_PROBE_BUSY_MS @@ a static screen is judged after the short time of a busy one
kvm_capsrc.c @@ if (!dxOpen) { return enter_gdi(c, now, KVM_CAPSRC_R_UNAVAILABLE); } @@ ; @@ a probe on a duplication that was never open is called inconclusive
kvm_capsrc.c @@ c->nextProbe = now + kvm_capsrc_probe_interval(c);\n	c->st.inconclusive++; @@ c->st.inconclusive++; @@ an inconclusive probe is followed by another at once
kvm_capsrc.c @@ c->st.inconclusive++; @@ ; @@ inconclusive probes are not counted
kvm_capsrc.c @@ if (c->probation)\n	{\n		// A shadow probe: the viewer @@ if (0)\n	{\n		// A shadow probe: the viewer @@ probation compares at the pace of the steady state
kvm_capsrc.c @@ if (dms(now, c->lastImage) < (int32_t)KVM_CAPSRC_PROBE_QUIET_MS) { return 0; } @@ ; @@ the pictures are compared in a probe while images arrive
kvm_capsrc.c @@ if (c->diffRun > 0) { return dms(now, c->lastCmp) >= (int32_t)KVM_CAPSRC_PROBE_CONFIRM_MS; } @@ ; @@ a difference in a probe is not looked at again soon
kvm_capsrc.c @@ (c->cmpCount < KVM_CAPSRC_FAST_COUNT ? KVM_CAPSRC_PROBE_QUIET_MS : KVM_CAPSRC_PROBE_PERIOD_MS) @@ KVM_CAPSRC_PROBE_QUIET_MS @@ a probe on a static screen compares every 0.6 s for the whole time
kvm_capsrc.c @@ if (c->mode != KVM_CAPSRC_MODE_DXGI) { return 0; } @@ ; @@ comparisons also in GDI mode outside a probe
kvm_capsrc.c @@ if (dms(now, c->lastImage) < (int32_t)KVM_CAPSRC_QUIET_MS) { return 0; } @@ ; @@ the pictures are compared while images arrive
kvm_capsrc.c @@ if (c->diffRun > 0) { return dms(now, c->lastCmp) >= (int32_t)KVM_CAPSRC_CONFIRM_MS; } @@ ; @@ the confirmation waits for the next period
kvm_capsrc.c @@ if (c->updates == 0 && !c->systematic)\n	{ @@ if (0)\n	{ @@ no fast comparisons after the duplication was opened
kvm_capsrc.c @@ if (c->cmpCount < KVM_CAPSRC_FAST_COUNT) { return dms(now, c->lastCmp) >= (int32_t)KVM_CAPSRC_FAST_MS; } @@ ; @@ the fast comparisons never end
kvm_capsrc.c @@ if (c->cmpCount == 0) { return dms(now, c->openAt) >= (int32_t)KVM_CAPSRC_FIRST_MS; } @@ ; @@ the first comparison is not delayed
kvm_capsrc.c @@ if (c->st.toGdi > 0 && dms(now, c->openAt) < (int32_t)KVM_CAPSRC_WATCH_WINDOW_MS) @@ if (dms(now, c->openAt) < (int32_t)KVM_CAPSRC_WATCH_WINDOW_MS) @@ every machine is watched closely (cost)
kvm_capsrc.c @@ if (c->st.toGdi > 0 && dms(now, c->openAt) < (int32_t)KVM_CAPSRC_WATCH_WINDOW_MS) @@ if (c->st.toGdi > 0) @@ the close watch never ends
kvm_capsrc.c @@ if (c->st.toGdi > 0 && dms(now, c->openAt) < (int32_t)KVM_CAPSRC_WATCH_WINDOW_MS) @@ if (0) @@ a machine that fell to GDI before is not watched closely
kvm_capsrc.c @@ if (c->afterReinit)\n	{\n		c->systematic = 1; @@ if (0)\n	{\n		c->systematic = 1; @@ the latch is never set (a differing DXGI is re-created for ever)
kvm_capsrc.c @@ c->afterReinit = 1;\n		c->diffRun = 0; @@ c->diffRun = 0; @@ the duplication is created again every time, never latched
kvm_capsrc.c @@ 		c->diffRun = 0;\n		c->afterReinit = 0;\n		if (c->systematic) @@ 		c->diffRun = 0;\n		if (c->systematic) @@ equal pictures do not end the "after re-creation" state
kvm_capsrc.c @@ if (!c->gdiActive) { c->gdiActive = 1; c->gdiSince = now; } @@ c->gdiActive = 1; c->gdiSince = now; @@ every failed probe restarts the time in GDI
kvm_capsrc.c @@ c->nextProbe = now + kvm_capsrc_probe_interval(c); @@ c->nextProbe = now; @@ DXGI is tried again at once, in every frame
kvm_capsrc.c @@ return c->mode == KVM_CAPSRC_MODE_GDI && !c->probation && dms(now, c->nextProbe) >= 0; @@ return c->mode == KVM_CAPSRC_MODE_GDI && dms(now, c->nextProbe) >= 0; @@ a probe is started again while one runs
kvm_capsrc.c @@ return c->mode == KVM_CAPSRC_MODE_GDI && !c->probation && dms(now, c->nextProbe) >= 0; @@ return c->mode == KVM_CAPSRC_MODE_GDI && !c->probation && dms(now, c->nextProbe) > 0; @@ the probe is due one tick late
kvm_capsrc.c @@ if (c->gdiActive) { t += (uint32_t)dms(now, c->gdiSince); } @@ ; @@ the time in GDI leaves out the current period
kvm_capsrc.c @@ c->systematic = 0;\n	c->afterReinit = 0;\n	c->st.probes++; @@ c->st.probes++; @@ a probe starts with the latches of the old duplication
kvm_capsrc.c @@ c->probation = 1;		// the mode stays GDI @@ c->probation = 1; c->mode = KVM_CAPSRC_MODE_DXGI;		// the mode stays GDI @@ a probe makes DXGI the source at once (the viewer waits for it)
kvm_capsrc.c @@ 		if (c->updates == 0 || c->systematic || c->probation) { c->needRef = 1; }\n		c->refValid = 0; @@ 		c->refValid = 0; @@ a full copy does not ask for a reference
kvm_capsrc.c @@ void kvm_capsrc_fed(kvm_capsrc *c)\n{\n	c->needRef = 1; @@ void kvm_capsrc_fed(kvm_capsrc *c)\n{ @@ a fill from GDI does not ask for a reference
kvm_capsrc.c @@ return c->refValid && c->refUpdates == c->updates; @@ return c->refValid; @@ an image does not outdate the reference
kvm_capsrc.c @@ c->refValid = ok ? 1 : 0;\n	c->refUpdates = c->updates; @@ c->refValid = ok ? 1 : 0; @@ the reference does not remember the images it was taken after
kvm_capsrc.c @@ c->refValid = ok ? 1 : 0; @@ c->refValid = 1; @@ a reference that could not be taken is used
kvm_capsrc.c @@ c->refValid = c->needRef = 0; @@ c->needRef = 0; @@ the reference of the old duplication is used for the new one
kvm_capsrc.c @@ { c->needRef = 1; return KVM_CAPSRC_ACT_NONE; }\t// no usable reference @@ { return KVM_CAPSRC_ACT_NONE; }\t// no usable reference @@ a probe whose reference could not be taken never asks for another
kvm_capsrc.c @@ { c->needRef = 1; return KVM_CAPSRC_ACT_NONE; }\t// a failed capture @@ { return KVM_CAPSRC_ACT_NONE; }\t// a failed capture @@ one failed capture of the reference ends the proof of silence for good (the picture stands)
kvm_capsrc.c @@ if (ref_lacking(c)) { c->needRef = 1; }\n		return KVM_CAPSRC_ACT_NONE; @@ return KVM_CAPSRC_ACT_NONE; @@ a comparison that learns nothing does not ask for a reference
kvm_capsrc.c @@ 		if (ref_lacking(c)) { c->needRef = 1; }\n		return act; @@ 		return act; @@ equal pictures do not ask for a missing reference (a change just after a failed reference is baked into the next one)
kvm_capsrc.c @@ if (c->updates == 0 || c->systematic || c->probation) { c->needRef = 1; } @@ c->needRef = 1; @@ every full copy costs a GDI capture, also where no reference is used
kvm_capsrc.c @@ return (dxOpen && dxImages > 0) ? 2 : 0; @@ return dxOpen ? 2 : 0; @@ an open duplication that has not delivered an image is the source
kvm_capsrc.c @@ return (dxOpen && dxImages > 0) ? 2 : 0; @@ return (dxImages > 0) ? 2 : 0; @@ the images of a duplication that is closed count
kvm_capsrc.c @@ return (dxOpen && dxImages > 0) ? 2 : 0; @@ return (dxOpen && dxImages > 1) ? 2 : 0; @@ the first image is not enough
kvm_capsrc.c @@ if (dxDisabled || gdiSource) { return 1; } @@ if (gdiSource) { return 1; } @@ DXGI that is off for the process is waited for
kvm_capsrc.c @@ if (dxDisabled || gdiSource) { return 1; } @@ if (dxDisabled) { return 1; } @@ a GDI source is waited for
kvm_capsrc.c @@ if (!inited) { return 0; } @@ ; @@ a capture that has not run is known
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
  S="$search" R="$replace" perl -0pi -e "BEGIN { \$s = \$ENV{S}; \$r = \$ENV{R}; \$s =~ s/\\\\n/\\n/g; \$r =~ s/\\\\n/\\n/g; \$s =~ s/\\\\t/\\t/g; \$r =~ s/\\\\t/\\t/g; } \$c += s/\\Q\$s\\E/\$r/g; END { exit(\$c >= 1 ? 0 : 3) }" /tmp/m/$file
  rc=$?
  if [ $rc -ne 0 ]; then echo "INVALID  ($desc): the text was not found"; invalid=$((invalid + 1)); continue; fi
  if ! gcc -std=c99 -O1 -w -o /tmp/m/t /tmp/m/tests/kvm_capsrc_test.c /tmp/m/kvm_capsrc.c 2>/tmp/m/err; then echo "INVALID  ($desc): does not compile"; invalid=$((invalid + 1)); continue; fi
  if timeout 120 /tmp/m/t >/tmp/m/out 2>&1; then echo "SURVIVED ($desc)"; survived=$((survived + 1)); else echo "killed   ($desc): $(grep -m1 FAIL /tmp/m/out | cut -c1-110)"; killed=$((killed + 1)); fi
done < /tmp/mutations.txt
echo "mutations: $n, killed $killed, survived $survived, invalid $invalid"
[ $survived -eq 0 ] && [ $invalid -eq 0 ]
' sh
