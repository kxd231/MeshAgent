/*
KVM: framing of the stdin of the KVM process (the byte stream that the viewer's commands arrive in).

Header only, portable C99, no operating system API: the same rule is used by the Windows agent (Windows/kvm.c, every build, with and without the commands 240-243), by the
tracker of the veto in the parent process (kvm_stream.c) and by the tests.

The only framing in that stream is the size field of each command: u16 type, u16 size, and size covers the 4 bytes of the header. A read of the pipe can end anywhere, so the
parser may see a command in pieces. Rule:
  - fewer than 4 bytes: wait for more;
  - size < 4: the command is shorter than its header, so it has no body. Exactly the 4 bytes of the header are consumed, nothing is dispatched, and the parser goes on with the bytes
    behind them. Never "size" bytes for size 1..3 (that moved the parser into the middle of a header) and never a wait for size 0 (that stopped the parser for good);
  - size > KVM_STDIN_MAX_CMD: a command that can never be completed in the input buffer (it used to hold the parser until the buffer was full and the session was closed). Its header
    and then its WHOLE declared length are consumed and nothing is dispatched: the body is data that the sender put inside one command, never further commands (a body skipped as commands let a viewer smuggle a 240 op 5 / op 6 past meshcore, which frames it as one unknown command). The bytes still to be discarded are kept in
    kvm_stdin_parser, so the rule holds across reads of any size;
  - size > bytes available: wait for more;
  - otherwise the command is size bytes.
The parent process (kvm_veto_track) applies the same rule (skip = size - 4 for every size >= 4, header only below), so the parent and the child stay at the same position in the stream,
and meshcore (agents/meshcore.js, kvmInputFeed) must frame the same way: an oversized frame is a frame it does not pass on.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef KVM_STDIN_H
#define KVM_STDIN_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define KVM_STDIN_BUF 30000				// the input buffer of the KVM process (Windows/kvm.c); no command can be longer
#define KVM_STDIN_HEADER 4
#define KVM_STDIN_MAX_CMD KVM_STDIN_BUF

// Stateless part: the number of bytes to consume (0 = more data is needed). *wellFramed (may be NULL) is 1 when the bytes are a command of the length returned, 0 when only a bad
// header is consumed (the caller that has to skip the body of an oversized frame uses kvm_stdin_next).
static inline size_t kvm_stdin_frame(const uint8_t *b, size_t blocklen, int *wellFramed)
{
	size_t size;
	if (wellFramed != NULL) { *wellFramed = 0; }
	if (b == NULL || blocklen < KVM_STDIN_HEADER) { return 0; }
	size = ((size_t)b[2] << 8) | (size_t)b[3];
	if (size < KVM_STDIN_HEADER || size > KVM_STDIN_MAX_CMD) { return KVM_STDIN_HEADER; }
	if (size > blocklen) { return 0; }
	if (wellFramed != NULL) { *wellFramed = 1; }
	return size;
}

// The parser with the one piece of state it needs: the bytes of an oversized frame that are still to be thrown away (across reads). Reset to zero at the start of a session.
typedef struct kvm_stdin_parser
{
	uint32_t discard;
} kvm_stdin_parser;
#define KVM_STDIN_KIND_CMD 1			// a command of the length returned: dispatch it
#define KVM_STDIN_KIND_BAD 2			// a header that is not a command (size < 4 or above the buffer): counted once; for an oversized frame the body follows as KVM_STDIN_KIND_SKIP
#define KVM_STDIN_KIND_SKIP 3			// bytes of the body of an oversized frame: thrown away
// Returns the number of bytes to consume (0 = more data is needed); *kind (may be NULL) says what they are.
static inline size_t kvm_stdin_next(kvm_stdin_parser *p, const uint8_t *b, size_t blocklen, int *kind)
{
	int ok = 0;
	size_t used, size;
	if (kind != NULL) { *kind = 0; }
	if (b == NULL || blocklen == 0) { return 0; }
	if (p->discard > 0)
	{
		used = blocklen < (size_t)p->discard ? blocklen : (size_t)p->discard;
		p->discard -= (uint32_t)used;
		if (kind != NULL) { *kind = KVM_STDIN_KIND_SKIP; }
		return used;
	}
	used = kvm_stdin_frame(b, blocklen, &ok);
	if (used == 0) { return 0; }
	if (ok) { if (kind != NULL) { *kind = KVM_STDIN_KIND_CMD; } return used; }
	size = ((size_t)b[2] << 8) | (size_t)b[3];
	if (size > KVM_STDIN_MAX_CMD) { p->discard = (uint32_t)(size - KVM_STDIN_HEADER); }
	if (kind != NULL) { *kind = KVM_STDIN_KIND_BAD; }
	return used;
}

// After the commands in buf[ptr] have been consumed: len bytes were read, ptr of them used. The bytes of a command that is not complete yet move to the start of the buffer, so that the
// next read has room (without it a stream that always ended in the middle of a command filled the buffer, the read asked for 0 bytes and the session was closed). Returns the new len;
// the caller sets ptr to 0.
static inline size_t kvm_stdin_compact(uint8_t *buf, size_t len, size_t ptr)
{
	size_t rest;
	if (ptr >= len) { return 0; }
	rest = len - ptr;
	if (ptr > 0) { memmove(buf, buf + ptr, rest); }
	return rest;
}

#endif // KVM_STDIN_H
