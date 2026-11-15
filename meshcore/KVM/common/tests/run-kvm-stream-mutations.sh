#!/bin/sh
# Mutation check of the tests of the command parser rules rules in kvm_stream.c / kvm_stdin.h (the inbox of the 240 commands, the framing of the stdin, the bits of the veto): each line below changes one
# rule of the code in a copy, and the tests (kvm_stream_test.c) have to turn red; a mutation that does not compile is reported as invalid, one that the tests do not notice as SURVIVED
# (exit status 1 at the end). Line format: file @@ text @@ replacement @@ description. Usage: run-kvm-stream-mutations.sh
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
kvm_stream.c @@ q->vetoPulse = (uint8_t)(q->vetoPulse | src); } @@ } @@ an op 5 followed by an op 6 before the next iteration is not seen by the stream (no stop of the video)
kvm_stream.c @@ else { q->vetoTarget = (uint8_t)(q->vetoTarget & ~src); } @@ else { } @@ an op 6 never clears the target
kvm_stream.c @@ if (q->tokensMilli < 1000u) { q->nLimited++; return KVM_INBOX_LIMITED; } @@ if (0) { q->nLimited++; return KVM_INBOX_LIMITED; } @@ the ordinary commands are not limited
kvm_stream.c @@ if (c->op == KVM_OP_VETO || c->op == KVM_OP_VETO_END)\n\t{\n\t\tuint8_t src @@ if (0)\n\t{\n\t\tuint8_t src @@ the veto passes the token bucket and is joined like the other commands
kvm_stream.c @@ uint8_t src = (uint8_t)(c->flags & KVM_VETO_ALL); @@ uint8_t src = c->flags; @@ the inbox keeps the reserved bits of the veto flags
kvm_stream.c @@ (b->vetoPulse | (tgt & ~curVeto)) @@ (b->vetoPulse) @@ a stream that was reset behind the inbox does not get the veto back
kvm_stream.c @@ uint8_t clr = (uint8_t)(((curVeto | set) & ~tgt) & KVM_VETO_ALL); @@ uint8_t clr = (uint8_t)((curVeto & ~tgt) & KVM_VETO_ALL); @@ an op 6 that follows an op 5 of the same iteration is not applied
kvm_stream.c @@ uint8_t clr = (uint8_t)(((curVeto | set) & ~tgt) & KVM_VETO_ALL); @@ uint8_t clr = 0; @@ the veto is never cleared by the main loop
kvm_stream.c @@ q->repairFlags = (uint8_t)(q->repairFlags | (c->flags & (KVM_REPAIR_IDR | KVM_REPAIR_TILES))); @@ q->repairFlags = (uint8_t)(c->flags & (KVM_REPAIR_IDR | KVM_REPAIR_TILES)); @@ op 3 flags are not joined, the last one wins
kvm_stream.c @@ if (q->repairFlags != 0) { q->nMerged++; } else { q->repairAt = at; } @@ q->repairAt = at; @@ op 3 takes the time of the last one
kvm_stream.c @@ q->state = *c; q->stateAt = at; q->haveState = 1; @@ if (!q->haveState) { q->state = *c; q->stateAt = at; q->haveState = 1; } @@ the first of op 1 / 2 / 4 wins
kvm_stream.c @@ q->vetoPulse = 0; q->haveState = 0; q->repairFlags = 0; @@ q->haveState = 0; q->repairFlags = 0; @@ the pulse of the veto is never emptied
kvm_stream.c @@ q->vetoPulse = 0; q->haveState = 0; q->repairFlags = 0; @@ q->vetoPulse = 0; q->repairFlags = 0; @@ the last request of the viewer is handled again at every iteration
kvm_stream.c @@ q->vetoPulse = 0; q->haveState = 0; q->repairFlags = 0; @@ q->vetoPulse = 0; q->haveState = 0; @@ the repair is handled again at every iteration
kvm_stream.c @@ uint64_t t = (uint64_t)q->tokensMilli + ((uint64_t)el * KVM_INBOX_RATE); @@ uint64_t t = (uint64_t)q->tokensMilli + ((uint64_t)el * KVM_INBOX_RATE * 100u); @@ the bucket of the ordinary commands refills 100 times too fast
kvm_stream.c @@ uint64_t cap = (uint64_t)KVM_INBOX_BURST * 1000u; @@ uint64_t cap = (uint64_t)KVM_INBOX_BURST * 4000u; @@ the bucket of the ordinary commands holds a burst 4 times too large
kvm_stream.c @@ q->tokensMilli -= 1000u;\n\t} @@ ;\n\t} @@ a command does not cost a token
kvm_stream.c @@ if (b->haveState) { out[n].c = b->state; out[n].at = b->stateAt; n++; } @@ if (b->haveState) { n++; } @@ the request of the viewer is not passed on
kvm_stream.c @@ s->vetoMask = (uint8_t)(s->vetoMask | (c->flags & KVM_VETO_ALL)); @@ s->vetoMask = (uint8_t)(s->vetoMask | c->flags); @@ the stream keeps the reserved bits of an op 5
kvm_stream.c @@ uint8_t src = (uint8_t)(t->buf[6] & KVM_VETO_ALL); @@ uint8_t src = t->buf[6]; @@ the tracker of the parent keeps the reserved bits and hands them to the next child
kvm_stream.c @@ (c.flags & KVM_VETO_ALL) == 0)) { return KVM_REJ_BAD_FLAGS; } @@ (c.flags) == 0)) { return KVM_REJ_BAD_FLAGS; } @@ an op 5 without a defined source is accepted
kvm_stdin.h @@ if (size < KVM_STDIN_HEADER || size > KVM_STDIN_MAX_CMD) { return KVM_STDIN_HEADER; } @@ if (size < KVM_STDIN_HEADER) { return KVM_STDIN_HEADER; } @@ a size above the buffer waits for data that cannot come
kvm_stdin.h @@ if (size < KVM_STDIN_HEADER || size > KVM_STDIN_MAX_CMD) { return KVM_STDIN_HEADER; } @@ if (size == 0 || size > KVM_STDIN_MAX_CMD) { return KVM_STDIN_HEADER; } @@ a size of 1 to 3 is a command of 1 to 3 bytes (the old desynchronisation)
kvm_stdin.h @@ if (size < KVM_STDIN_HEADER || size > KVM_STDIN_MAX_CMD) { return KVM_STDIN_HEADER; } @@ if (size < KVM_STDIN_HEADER || size > KVM_STDIN_MAX_CMD) { return 0; } @@ a bad size stops the parser (the old hang)
kvm_stdin.h @@ if (size < KVM_STDIN_HEADER || size > KVM_STDIN_MAX_CMD) { return KVM_STDIN_HEADER; } @@ if (size < KVM_STDIN_HEADER || size > KVM_STDIN_MAX_CMD) { return blocklen; } @@ a bad size drops the rest of the buffer
kvm_stdin.h @@ if (size > blocklen) { return 0; } @@ ; @@ a command that is not complete yet is dispatched
kvm_stdin.h @@ if (wellFramed != NULL) { *wellFramed = 1; } @@ ; @@ a well framed command is reported as not well framed
kvm_stdin.h @@ if (ptr > 0) { memmove(buf, buf + ptr, rest); } @@ ; @@ the incomplete command is not moved to the start of the buffer
kvm_stream.c @@ (size < KVM_STDIN_HEADER) ? 0u : (uint32_t)(size - KVM_STDIN_HEADER) @@ (size < KVM_STDIN_HEADER || size > KVM_STDIN_MAX_CMD) ? 0u : (uint32_t)(size - KVM_STDIN_HEADER) @@ the tracker of the parent reads the body of an oversized frame as commands
kvm_stream.c @@ (size < KVM_STDIN_HEADER) ? 0u : (uint32_t)(size - KVM_STDIN_HEADER) @@ (size < 1) ? 0u : (uint32_t)(size - KVM_STDIN_HEADER) @@ the tracker of the parent reads a size of 1 to 3 as a command
kvm_stream.c @@ (size < KVM_STDIN_HEADER) ? 0u : (uint32_t)(size - KVM_STDIN_HEADER) @@ (size < KVM_STDIN_HEADER) ? 0u : (uint32_t)(size - KVM_STDIN_HEADER + 1) @@ the tracker of the parent skips one byte too many of a body
kvm_stdin.h @@ if (size > KVM_STDIN_MAX_CMD) { p->discard = (uint32_t)(size - KVM_STDIN_HEADER); } @@ ; @@ the body of an oversized frame is read as commands
kvm_stdin.h @@ p->discard = (uint32_t)(size - KVM_STDIN_HEADER); @@ p->discard = (uint32_t)(size - KVM_STDIN_HEADER - 1); @@ one byte of the body of an oversized frame is read as a command
kvm_stdin.h @@ p->discard -= (uint32_t)used; @@ ; @@ the discard counter never runs out
kvm_stdin.h @@ if (b == NULL || blocklen == 0) { return 0; } @@ if (b == NULL || blocklen < KVM_STDIN_HEADER) { return 0; } @@ the rest of a body is not thrown away when a read is shorter than a header
kvm_stdin.h @@ used = blocklen < (size_t)p->discard ? blocklen : (size_t)p->discard; @@ used = (size_t)p->discard; @@ the discard counter takes more than the read has
kvm_stream.c @@ else if ((cmd == KVM_CMD_COMPRESSION || cmd == KVM_CMD_FRAME_RATE) && size >= 6 && size <= KVM_CTRL_SIZE) { t->collect = (uint8_t)size; } @@ else if (0) { t->collect = (uint8_t)size; } @@ the parent does not remember commands 5 and 13
kvm_stream.c @@ && size >= 6 && size <= KVM_CTRL_SIZE) { t->collect @@ && size >= 6) { t->collect @@ a command 5 above the buffer is collected
kvm_stream.c @@ && size >= 6 && size <= KVM_CTRL_SIZE) { t->collect @@ && size <= KVM_CTRL_SIZE) { t->collect @@ a command 5 below 6 bytes is collected
kvm_stream.c @@ if (cmd == KVM_CMD_COMPRESSION) { memcpy(t->cmd5, t->buf, t->collect); t->len5 = t->collect; t->rateLast = 0; } @@ if (cmd == KVM_CMD_COMPRESSION) { memcpy(t->cmd5, t->buf, t->collect); t->len5 = t->collect; } @@ the order of 5 and 13 is not followed (5 later)
kvm_stream.c @@ t->len13 = t->collect; t->rateLast = 1; } @@ t->len13 = t->collect; } @@ the order of 5 and 13 is not followed (13 later)
kvm_stream.c @@ const int five = (pass == 0) == (t->rateLast != 0); @@ const int five = (pass == 0); @@ the replay is always 5 then 13
kvm_stream.c @@ if (len > 0 && n + len <= cap) { memcpy(out + n, src, len); n += len; } @@ if (len > 0) { memcpy(out + n, src, len); n += len; } @@ the replay ignores the size of the buffer
kvm_stream.c @@ memcpy(t->cmd13, t->buf, t->collect); @@ memcpy(t->cmd13, t->buf, t->collect - 1); @@ the last byte of command 13 is lost
kvm_stream.c @@ if (s->watchOn && !s->watchDone && a->seq == s->watchSeq) { @@ if (s->watchOn && !s->watchDone) { @@ any ACK is taken for the ACK of the watched position
kvm_stream.c @@ s->watchTs = p->ts; @@ s->watchTs = now; @@ the time stamp of the watched position is the time of the ACK
kvm_stream.c @@ s->watchQueueMs = a->queueMs; @@ s->watchQueueMs = 0; @@ the time that the viewer held the position is not passed on
kvm_stream.c @@ s->watchOn = 0; s->watchDone = 0; @@ ; @@ a result can be read twice
kvm_stream.c @@ void kvm_stream_watch(kvm_stream *s, uint32_t seq) { s->watchOn = 1; s->watchDone = 0; s->watchSeq = seq; } @@ void kvm_stream_watch(kvm_stream *s, uint32_t seq) { s->watchOn = 1; s->watchSeq = seq; } @@ a new watch keeps the result of the old one
kvm_stream.c @@ if (age < (int32_t)KVM_STREAM_MEM_HOLD_MS) { return s->rateMem; } @@ if (age < 0) { return s->rateMem; } @@ the memory is not held, it halves at once
kvm_stream.c @@ steps = 1u + (uint32_t)(age - (int32_t)KVM_STREAM_MEM_HOLD_MS) / (uint32_t)KVM_STREAM_MEM_HALF_MS; @@ steps = (uint32_t)(age - (int32_t)KVM_STREAM_MEM_HOLD_MS) / (uint32_t)KVM_STREAM_MEM_HALF_MS; @@ the first halving comes one period late
kvm_stream.c @@ return steps >= 40u ? 0u : (s->rateMem >> steps); @@ return s->rateMem; @@ the memory never decays
kvm_stream.c @@ s->stallNoted = 1; s->st.stalls++; s->rateMem = 0; } @@ s->stallNoted = 1; s->st.stalls++; } @@ a real stall does not void the memory
kvm_stream.c @@ if (w > KVM_STREAM_W_RESUME) { w = KVM_STREAM_W_RESUME; } @@ ; @@ the window after a pause is the whole remembered one (a changed link is queued for seconds)
kvm_stream.c @@ if (s->minRtt != 0 && (uint32_t)rtt > 2u * s->minRtt && (uint32_t)rtt >= s->minRtt + 20u) { s->rateMem = 0; } @@ ; @@ a doubled round trip does not void the memory
kvm_stream.c @@ if (s->minRtt != 0 && (uint32_t)rtt > 2u * s->minRtt && (uint32_t)rtt >= s->minRtt + 20u) { s->rateMem = 0; } @@ if (s->minRtt != 0 && (uint32_t)rtt > s->minRtt) { s->rateMem = 0; } @@ any probe that is slower voids the memory
kvm_stream.c @@ s->rateMem = r; s->rateMemAt = now ? now : 1u; @@ s->rateMem = r; @@ a confirmation does not renew the memory
kvm_stream.c @@ if (dms(h->t, s->busyFrom) < 0) { break; } @@ ; @@ a sample spans the silence between two series
kvm_stream.c @@ (!s->haveEmpty || dms(now, s->emptySince) > (int32_t)KVM_STREAM_BUSY_GAP_MS)) { s->busyFrom @@ (1)) { s->busyFrom @@ every write into an empty pipe begins a new period of work (sparse traffic makes no sample)
kvm_stream.c @@ if (s->bytesWritten == s->ackedCum) { s->emptySince = now; s->haveEmpty = 1; } @@ ; @@ the pipe is never known to have been empty (the first write after a pause is not a new period)
kvm_stream.c @@ if (r != 0 && r >= mem_eff(s, now)) { s->rateMemFull = @@ if (r != 0) { s->rateMemFull = @@ a lower sample lowers what is remembered (the memory follows the last burst)
kvm_stream.c @@ if (r != 0 && kvm_stream_link_limited(s, now) && max_rate_full(s, now) != 0) { return r; }\n	return r > m ? r : m; @@ return r != 0 ? r : m; @@ a low sample that the window did not cause replaces the remembered rate (the cold start after a trickle)
kvm_stream.c @@ if (r != 0 && kvm_stream_link_limited(s, now) && max_rate_full(s, now) != 0) { return r; }\n	return r > m ? r : m; @@ return r > m ? r : m; @@ the samples never say that the link got slower (the rate follows the highest)
kvm_stream.c @@ if (max_rate(s, now) == 0)\n	{ @@ if (0)\n	{ @@ the remembered rate sizes the window without the cap of the resume
kvm_stream.c @@ return r != 0 ? r : mem_eff(s, now);\n} @@ return r > mem_eff(s, now) ? r : mem_eff(s, now);\n} @@ the sharpening takes its share of the highest rate that was seen
kvm_stream.c @@ if (d * 100u < mem_eff(s, now) * (uint64_t)KVM_STREAM_RESUME_PCT) { s->rateMem = d; @@ if (0) { s->rateMem = d; @@ no check of the resume (the memory of a link that got slower stays 30 s)
kvm_stream.c @@ int32_t el = dms(now, p->ts) - (int32_t)a->queueMs - (int32_t)s->minRtt; @@ int32_t el = dms(now, p->ts) - (int32_t)a->queueMs; @@ the network round trip is taken for a slow link
kvm_stream.c @@ p->flight >= KVM_STREAM_RESUME_BYTES) @@ p->flight >= 1u) @@ the resume check looks at a flight of a few bytes
kvm_stream.c @@ s->resumeArmed = (mem_eff(s, now) > max_rate(s, now)) ? 1u : 0u; @@ s->resumeArmed = 0u; @@ the resume check is never armed
kvm_stream.c @@ const int appLtd = (!linkLimited && s->haveEmpty && dms(s->emptySince, h->t) >= 0) ? 1 : 0; @@ const int appLtd = 0; @@ no sample is app-limited (a trickle is what the link takes: R 100 kb/s)
kvm_stream.c @@ const int appLtd = (!linkLimited && s->haveEmpty && dms(s->emptySince, h->t) >= 0) ? 1 : 0; @@ const int appLtd = (!linkLimited) ? 1 : 0; @@ a clogged slow link is app-limited (the pipe never emptied, and its samples are dropped)
kvm_stream.c @@ const int appLtd = (!linkLimited && s->haveEmpty && dms(s->emptySince, h->t) >= 0) ? 1 : 0; @@ const int appLtd = (!linkLimited && s->haveEmpty) ? 1 : 0; @@ the pipe emptied some time before the span, not in it
kvm_stream.c @@ const int appLtd = (!linkLimited && s->haveEmpty && dms(s->emptySince, h->t) >= 0) ? 1 : 0; @@ const int appLtd = (s->haveEmpty && dms(s->emptySince, h->t) >= 0) ? 1 : 0; @@ a span in which the window held the sender back is app-limited
kvm_stream.c @@ if (!appLtd || rate > cur) { sample_add(s, now, rate, appLtd); } @@ if (!appLtd) { sample_add(s, now, rate, appLtd); } @@ an app-limited sample never raises the estimate
kvm_stream.c @@ if (!appLtd || rate > cur) { sample_add(s, now, rate, appLtd); } @@ if (rate > cur) { sample_add(s, now, rate, appLtd); } @@ a clogged slow link counts only when it raises (the window of the start stays 256 KB)
kvm_stream.c @@ if (max_rate_full(s, now) != 0) { return 0; } @@  @@ a lower bound with samples that are none: a refusal of the window is no evidence while the pipe is clogged
kvm_stream.c @@ if (mem_eff(s, now) != 0 && s->rateMemFull && dms(now, s->rateMemAt) < (int32_t)KVM_STREAM_MEM_HOLD_MS) { return 0; } @@  @@ the memory of a window-limited sample is no evidence of the link
kvm_stream.c @@ if (mem_eff(s, now) != 0 && s->rateMemFull && dms(now, s->rateMemAt) < (int32_t)KVM_STREAM_MEM_HOLD_MS) { return 0; } @@ if (mem_eff(s, now) != 0 && s->rateMemFull) { return 0; } @@ the app-limited samples: a memory that is no longer held (decayed to a few B/s after 300 s of idle) is still evidence of the link
kvm_stream.c @@ && !estimate_is_lower_bound(s, now); } @@ ; } @@ a refusal of the window is evidence of the link whatever the estimate is made of (R 100 kb/s of the scene E)
kvm_stream.c @@ && max_rate_full(s, now) != 0) { return r; } @@ && 1) { return r; } @@ a refusal of the window makes the samples the rate even when they are lower bounds (a trickle after a pause lowers the estimate)
kvm_stream.c @@ return s->probeState != 1 && kvm_stream_link_limited(s, now); @@ return 1; @@ kvm_stream_note_link_held says every refusal is evidence
kvm_stream.c @@ return s->probeState != 1 && kvm_stream_link_limited(s, now); @@ return 0; @@ kvm_stream_note_link_held says no refusal is evidence
kvm_stream.c @@ s->rateMemFull = (max_rate_full(s, now) >= r) ? 1u : 0u; @@ s->rateMemFull = 1u; @@ a memory raised by a lower bound counts as evidence of the link
kvm_stream.c @@ s->rateMemFull = (max_rate_full(s, now) >= r) ? 1u : 0u; @@ s->rateMemFull = 0u; @@ the memory of a window-limited sample is a lower bound
kvm_stream.c @@ if (kind != KVM_GATE_VIDEO) { inflight += s->reservedBytes; } @@ ; @@ the JPEG and the sharpening do not count the frame that is being encoded
kvm_stream.c @@ if (kind != KVM_GATE_VIDEO) { inflight += s->reservedBytes; } @@ inflight += s->reservedBytes; @@ the gate of the video counts the frame that it lets in
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
  if ! gcc -std=c99 -O1 -w -o /tmp/m/t /tmp/m/tests/kvm_stream_test.c /tmp/m/kvm_stream.c 2>/tmp/m/err; then echo "INVALID  ($desc): does not compile"; invalid=$((invalid + 1)); continue; fi
  if timeout 120 /tmp/m/t >/tmp/m/out 2>&1; then echo "SURVIVED ($desc)"; survived=$((survived + 1)); else echo "killed   ($desc): $(grep -m1 FAIL /tmp/m/out | cut -c1-110)"; killed=$((killed + 1)); fi
done < /tmp/mutations.txt
echo "mutations: $n, killed $killed, survived $survived, invalid $invalid"
[ $survived -eq 0 ] && [ $invalid -eq 0 ]
' sh
