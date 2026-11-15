#!/bin/sh
# Mutation check of the tests of kvm_fbuf.c: each line changes one rule of the picture buffers in a copy, and the tests have to turn red. INVALID = does not compile / text not found,
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
kvm_fbuf.c @@ if (--h->refs > 0) { ul(); return; } @@ if (--h->refs > 1) { ul(); return; } @@ the buffer is cached while an owner is left
kvm_fbuf.c @@ if (--h->refs > 0) { ul(); return; } @@ --h->refs; @@ the buffer goes into the cache with owners left (a job still reads it)
kvm_fbuf.c @@ if (i >= 0 && gReg[i]->magic == FB_MAGIC) { gReg[i]->refs++; } @@ ; @@ a reference for the job is not counted
kvm_fbuf.c @@ if (i < 0) { gSt.foreign++; ul(); free(p); return; } @@ if (i < 0) { gSt.foreign++; ul(); return; } @@ a foreign pointer is not freed (leak)
kvm_fbuf.c @@ if (h->magic != FB_MAGIC) { gSt.doubleRelease++; ul(); return; } @@ if (h->magic != FB_MAGIC) { gSt.doubleRelease++; } @@ a second release puts the buffer into the cache twice
kvm_fbuf.c @@ if (h->size == gSize && gNCache < KVM_FBUF_CACHE_MAX) { @@ if (gNCache < KVM_FBUF_CACHE_MAX) { @@ a buffer of another size is cached
kvm_fbuf.c @@ if (h->size == gSize && gNCache < KVM_FBUF_CACHE_MAX) { @@ if (h->size == gSize) { @@ the cache is not limited
kvm_fbuf.c @@ h->magic = FB_MAGIC;\n	h->refs = 1; @@ h->magic = FB_MAGIC; @@ a reused buffer keeps its old count
kvm_fbuf.c @@ if (size != gSize) { set_size_locked(size); } @@ ; @@ the cache of an old size stays when the size changes

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
  if ! gcc -std=c99 -O1 -w -fsanitize=address -o /tmp/m/t /tmp/m/tests/kvm_fbuf_test.c /tmp/m/kvm_fbuf.c 2>/tmp/m/err; then echo "INVALID  ($desc): does not compile"; invalid=$((invalid + 1)); continue; fi
  if /tmp/m/t >/tmp/m/out 2>&1; then echo "SURVIVED ($desc)"; survived=$((survived + 1)); else echo "killed   ($desc): $(grep -m1 FAIL /tmp/m/out | cut -c1-110)"; killed=$((killed + 1)); fi
done < /tmp/mutations.txt
echo "mutations: $n, killed $killed, survived $survived, invalid $invalid"
[ $survived -eq 0 ] && [ $invalid -eq 0 ]
' sh
