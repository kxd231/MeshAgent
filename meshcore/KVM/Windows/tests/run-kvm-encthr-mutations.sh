#!/bin/sh
# Mutation check of kvm_encthr_test.cpp: each line changes one rule of kvm_encthr.cpp / kvm_encjob.c in a copy; the test (the real thread, pthreads) has to fail or hang (a timeout counts as killed).
# Usage: run-kvm-encthr-mutations.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
KVM=$(cd "$HERE/../.." && pwd)
IMG=kvm-ctestpp:local
docker image inspect "$IMG" >/dev/null 2>&1 || { echo "run run-kvm-video-harness.sh first (it builds the image)"; exit 2; }
exec docker run --rm --user "$(id -u):$(id -g)" -v "$KVM:/src:ro" -w /tmp "$IMG" sh -c '
set -u
cat > /tmp/mutations.txt <<"EOT"
Windows/kvm_encthr.cpp @@ os_unlock(&E->mu);\n\tif (slot < 0) { return -1; }\n\tif (E->mode == KVM_ENCTHR_THREADED) { os_event_set(&E->evWork); } @@ os_unlock(&E->mu);\n\tif (slot < 0) { return -1; } @@ a submitted job does not wake the thread
Windows/kvm_encthr.cpp @@ run_job(E, &job, cancelled, &res);\n\t\tos_lock(&E->mu); @@ res.code = 0; res.len = 6;\n\t\tos_lock(&E->mu); @@ the job is not made
Windows/kvm_encthr.cpp @@ if (cancelled) { return; }\n\tmemset(&err, 0, sizeof(err));\n\tif (h == NULL) @@ memset(&err, 0, sizeof(err));\n\tif (h == NULL) @@ a cancelled job is encoded
Windows/kvm_encthr.cpp @@ if (j->forceIdr && (rc = be->force_idr(h, &err)) != 0) @@ if (0 && (rc = be->force_idr(h, &err)) != 0) @@ the key frame is not forced
Windows/kvm_encthr.cpp @@ r->structural = kvm_encthr_error_structural(code); @@ r->structural = 0; @@ an exception is not told from a lost access unit
Windows/kvm_encthr.cpp @@ case MFH264_E_EXCEPTION: case MFH264_E_OS: @@ case MFH264_E_OS: @@ the exception is not structural
Windows/kvm_encthr.cpp @@ \tif (!os_event_wait(&E->evCmd, timeoutMs))\n\t{\n\t\tos_lock(&E->mu); E->abandoned = 1; @@ \tif (!os_event_wait(&E->evCmd, timeoutMs))\n\t{\n\t\tos_lock(&E->mu); @@ a call that times out does not give the thread up
Windows/kvm_encthr.cpp @@ \t\t(void)step(E);\t\t// the jobs that are waiting come first\n @@ \n @@ in the manual and inline modes a call does not make the waiting jobs first
Windows/kvm_encthr.cpp @@ \tif (E->abandoned) { if (E->mode != KVM_ENCTHR_THREADED) { destroy(E); } return; } @@ \tif (E->abandoned) { destroy(E); return; } @@ a hung thread is freed under it
Windows/kvm_encthr.cpp @@ \tif (E->initRc != 0)\n\t{\n\t\t*err = E->initErr; @@ \tif (0)\n\t{\n\t\t*err = E->initErr; @@ a failed init of the thread is not reported
Windows/kvm_encthr.cpp @@ \tif (gE != NULL) { kvm_encthr_stop(2000); } @@ \tif (0) { } @@ a new start does not end the old one
Windows/kvm_encthr.cpp @@ os_lock(&E->mu); E->quit = 1; os_unlock(&E->mu);\n\tos_event_set(&E->evWork); @@ os_lock(&E->mu); E->quit = 1; os_unlock(&E->mu); @@ stop does not wake the thread
Windows/kvm_encthr.cpp @@ int kvm_encthr_abandon(void)\n{\n\tint n;\n\tif (gE == NULL) { return 0; }\n\tos_lock(&gE->mu);\n\tgE->abandoned = 1; @@ int kvm_encthr_abandon(void)\n{\n\tint n;\n\tif (gE == NULL) { return 0; }\n\tos_lock(&gE->mu); @@ the watchdog leaves the thread usable
common/kvm_encjob.c @@ if (e->slot[slot].state != KVM_EJS_RUNNING) { return; } @@ if (e->slot[slot].state == KVM_EJS_FREE) { return; } @@ a late result of an abandoned job is taken
Windows/kvm_encthr.cpp @@ 			if (rem <= 0) { break; } @@ 			break; @@ a wait ends with the first wake of the event (a stale one: the job was collected before)
Windows/kvm_encthr.cpp @@ if (j->viewMode != 0 && be->encode_nv12 != NULL) @@ if (0) @@ a job with a view is encoded as the picture
Windows/kvm_encthr.cpp @@ j->viewMap, j->mbCols, j->mbRows, j->viewFull, j->viewFlat); @@ j->viewMap, j->mbCols, j->mbRows, 0, j->viewFlat); @@ the complete conversion of a job is partial
Windows/kvm_encthr.cpp @@ j->viewMap, j->mbCols, j->mbRows, j->viewFull, j->viewFlat); @@ j->viewMap, j->mbCols, j->mbRows, j->viewFull, 0); @@ the flat view of a job is not flat
Windows/kvm_encthr.cpp @@ E->view == NULL || E->viewW != j->width || E->viewH != j->height @@ E->view == NULL @@ the view is not made again for another size
Windows/kvm_encthr.cpp @@ r->convMbs = (unsigned)conv; @@ ; @@ the conversion is not counted
Windows/kvm_encthr.cpp @@ if (conv < 0) { r->phase = 0; r->code = MFH264_E_ARG; @@ if (0) { r->phase = 0; r->code = MFH264_E_ARG; @@ a plan that does not fit is not an error of the frame
EOT
survived=0; invalid=0; killed=0; n=0
while IFS= read -r line; do
  [ -z "$line" ] && continue
  case "$line" in *equivalent*) continue;; esac
  n=$((n + 1))
  file=$(printf "%s" "$line" | awk -F " @@ " "{print \$1}")
  search=$(printf "%s" "$line" | awk -F " @@ " "{print \$2}")
  replace=$(printf "%s" "$line" | awk -F " @@ " "{print \$3}")
  desc=$(printf "%s" "$line" | awk -F " @@ " "{print \$4}")
  rm -rf /tmp/kvm && cp -r /src /tmp/kvm
  S="$search" R="$replace" perl -0pi -e "BEGIN { \$s = \$ENV{S}; \$r = \$ENV{R}; \$s =~ s/\\\\n/\\n/g; \$r =~ s/\\\\n/\\n/g; \$s =~ s/\\\\t/\\t/g; \$r =~ s/\\\\t/\\t/g; } \$c += s/\\Q\$s\\E/\$r/; END { exit(\$c == 1 ? 0 : 3) }" /tmp/kvm/$file
  rc=$?
  if [ $rc -ne 0 ]; then echo "INVALID  ($desc): the text was not found exactly once"; invalid=$((invalid + 1)); continue; fi
  cd /tmp/kvm/Windows/tests
  if ! ( gcc -std=c99 -O1 -w -c ../../common/kvm_encjob.c -o ej.o && gcc -std=c99 -O1 -w -c ../../common/kvm_nv12view.c -o nv.o && gcc -std=c99 -O1 -w -c ../../common/kvm_yuv.c -o yuv.o && g++ -std=c++17 -O1 -w -I.. -I../../common -pthread -o t kvm_encthr_test.cpp ../kvm_encthr.cpp ej.o nv.o yuv.o ) 2>/tmp/err; then echo "INVALID  ($desc): does not compile"; invalid=$((invalid + 1)); cd /tmp; continue; fi
  if timeout 120 ./t 40 >/tmp/out 2>&1; then echo "SURVIVED ($desc)"; survived=$((survived + 1)); else echo "killed   ($desc): $(grep -m1 FAIL /tmp/out | cut -c1-100)"; killed=$((killed + 1)); fi
  cd /tmp
done < /tmp/mutations.txt
echo "mutations: $n, killed $killed, survived $survived, invalid $invalid"
[ $survived -eq 0 ] && [ $invalid -eq 0 ]
' sh
