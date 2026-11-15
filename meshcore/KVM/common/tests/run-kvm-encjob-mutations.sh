#!/bin/sh
# Mutation check of the tests of kvm_encjob.c: each line changes one rule of the job slots in a copy, and the tests have to turn red. INVALID = does not compile / text not found,
# SURVIVED = the tests do not notice (exit status 1 at the end).
# Usage: run-kvm-encjob-mutations.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
COMMON=$(dirname "$HERE")
IMG=kvm-ctest:local
docker image inspect "$IMG" >/dev/null 2>&1 || { echo "run run-kvm-encjob-tests.sh first (it builds the image)"; exit 2; }
exec docker run --rm --user "$(id -u):$(id -g)" -v "$COMMON:/src:ro" -w /tmp "$IMG" sh -c '
set -u
cat > /tmp/mutations.txt <<"EOT"
kvm_encjob.c @@ if (best < 0 || (int32_t)(e->slot[i].order - e->slot[best].order) < 0) { best = i; }\n	}\n	return best;\n}\n\nint kvm_ej_inflight @@ if (best < 0 || (int32_t)(e->slot[i].order - e->slot[best].order) > 0) { best = i; }\n	}\n	return best;\n}\n\nint kvm_ej_inflight @@ collect delivers the newest job first
kvm_encjob.c @@ if (e->slot[i].state != KVM_EJS_SUBMITTED) { continue; }\n		if (best < 0 || (int32_t)(e->slot[i].order - e->slot[best].order) < 0) { best = i; } @@ if (e->slot[i].state != KVM_EJS_SUBMITTED) { continue; }\n		if (best < 0 || (int32_t)(e->slot[i].order - e->slot[best].order) > 0) { best = i; } @@ the thread takes the newest job first
kvm_encjob.c @@ if (i < 0 || e->slot[i].state != KVM_EJS_DONE) { return -1; } @@ if (i < 0) { return -1; } @@ collect delivers a job that is not done
kvm_encjob.c @@ e->slot[i].cancelled = 0;\n	e->slot[i].order @@ e->slot[i].order @@ a new job inherits the cancel flag of the old one
kvm_encjob.c @@ if (in_flight_state(e->slot[i].state) && !e->slot[i].cancelled) { e->slot[i].cancelled = 1; n++; e->cancelledN++; } @@ if (in_flight_state(e->slot[i].state)) { e->slot[i].cancelled = 1; n++; e->cancelledN++; } @@ cancel counts a job that was cancelled already
kvm_encjob.c @@ if (cancelled != NULL) { *cancelled = e->slot[i].cancelled; }\n	e->slot[i].state @@ if (cancelled != NULL) { *cancelled = 0; }\n	e->slot[i].state @@ a cancelled result is delivered as a good one
kvm_encjob.c @@ if (cancelled != NULL) { *cancelled = e->slot[best].cancelled; } @@ if (cancelled != NULL) { *cancelled = 0; } @@ the thread does not learn that the job was given up
kvm_encjob.c @@ e->slot[i].state = KVM_EJS_ABANDONED; e->slot[i].cancelled = 1; n++; @@ e->slot[i].state = KVM_EJS_FREE; e->slot[i].cancelled = 1; n++; @@ an abandoned slot is used again
kvm_encjob.c @@ if (e->slot[slot].state != KVM_EJS_RUNNING) { return; }	// abandoned meanwhile (or a bad call): the result is dropped @@ if (e->slot[slot].state == KVM_EJS_FREE) { return; } @@ a late result of an abandoned job is accepted
kvm_encjob.c @@ if (e->slot[i].state == KVM_EJS_SUBMITTED || e->slot[i].state == KVM_EJS_RUNNING) @@ if (e->slot[i].state != KVM_EJS_FREE) @@ a finished job that waits counts as a hung encoder
kvm_encjob.c @@ static int in_flight_state(int s) { return s == KVM_EJS_SUBMITTED || s == KVM_EJS_RUNNING || s == KVM_EJS_DONE; } @@ static int in_flight_state(int s) { return s == KVM_EJS_SUBMITTED || s == KVM_EJS_RUNNING; } @@ a finished job is not in flight
kvm_encjob.c @@ e->slot[i].startedAt = now; @@ ; @@ the watchdog clock is not started by the submit
kvm_encjob.c @@ e->slot[best].state = KVM_EJS_RUNNING; @@ ; @@ a taken job stays SUBMITTED (is taken twice)
kvm_encjob.c @@ e->slot[slot].res = *res; @@ ; @@ the result is not stored
kvm_encjob.c @@ e->depth = depth < 1 ? 1 : (depth > KVM_EJ_MAX ? KVM_EJ_MAX : depth); @@ e->depth = depth; @@ the depth is not limited
kvm_encjob.c @@ memcpy(e->slot[i].map, job->viewMap, (size_t)n); @@ ; @@ the map of the view is not copied by the submit
kvm_encjob.c @@ e->slot[i].job.viewMap = e->slot[i].map; @@ e->slot[i].job.viewMap = job->viewMap; @@ the job of the thread points at the map of the caller
kvm_encjob.c @@ *job = e->slot[i].job; job->viewMap = NULL; } @@ *job = e->slot[i].job; } @@ the collect gives the pointer to the copy of the map back
kvm_encjob.c @@ n <= (long)KVM_EJ_MB_MAX) @@ n <= (long)KVM_EJ_MB_MAX * 4) @@ a grid that does not fit the slot is copied
kvm_encjob.c @@ else if (!job->viewFull) { e->slot[i].job.viewMode = 0; } @@ else if (0) { e->slot[i].job.viewMode = 0; } @@ a partial job without a map stays a job of the view
kvm_encjob.c @@ if (job->viewMap != NULL && n > 0 && @@ if (job->viewMap != NULL && @@ an empty grid is copied
kvm_encjob.c @@ e->slot[i].job = *job;\n	e->slot[i].job.viewMap = NULL; @@ e->slot[i].job = *job; @@ a job that is not of the view keeps the pointer of the caller
kvm_encjob.c @@ if (job->viewMode != 0)\n	{\n		const long n @@ if (1)\n	{\n		const long n @@ the map is copied for a job of the picture too
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
  if ! gcc -std=c99 -O1 -w -o /tmp/m/t /tmp/m/tests/kvm_encjob_test.c /tmp/m/kvm_encjob.c 2>/tmp/m/err; then echo "INVALID  ($desc): does not compile"; invalid=$((invalid + 1)); continue; fi
  if /tmp/m/t >/tmp/m/out 2>&1; then echo "SURVIVED ($desc)"; survived=$((survived + 1)); else echo "killed   ($desc): $(grep -m1 FAIL /tmp/m/out | cut -c1-110)"; killed=$((killed + 1)); fi
done < /tmp/mutations.txt
echo "mutations: $n, killed $killed, survived $survived, invalid $invalid"
[ $survived -eq 0 ] && [ $invalid -eq 0 ]
' sh
