/*
KVM (Linux): the end-to-end ACK of the commands 240-243 in the capture process.

The protocol and the window are in meshcore/KVM/common/kvm_stream.c (portable, tested in Docker); this is the glue of the Linux capture process (the "slave", X11 and DRM
alike). The slave is one thread: the main loop reads the commands of the viewer, writes the pictures and the answers, so nothing here takes a lock. Every byte that the
slave writes to the pipe goes through kvml_write (kvm_slave_send in linux_kvm.c), so that the byte counter of the window and the positions in its ring describe the same
order. Only the JPEG picture is there: a viewer that asks for H.264 is told "no video in this build" (242 reason 1) and still gets the ACK window.

Off with -DKVM_STREAM_DISABLE (then the agent eats 240 and 241 like an agent from before them and never sends 242) and, per process, with MESH_KVM_ACK=0 (caps 0,
op 1 and op 4 are answered with reason 6, nothing is limited). MESH_KVM_STREAM_LOG=1 writes the commands, the answers and a line of statistics every 5 s to stderr.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef LINUX_KVM_STREAM_H_
#define LINUX_KVM_STREAM_H_

#include <stddef.h>

#ifndef KVM_STREAM_DISABLE
#define KVM_STREAM_ENABLED 1
#endif

#ifdef KVM_STREAM_ENABLED

// Start of the session in this process (the top of the main loop, before the first write); the counting is on from here
void kvml_session_begin(void);
void kvml_session_end(void);
// 242 reason 11: this process has started; the viewer has to repeat its request
void kvml_announce(void);
// A 240 or a 241 of exactly 16 bytes from the stdin of the slave (kvm_server_inputdata): queued for kvml_poll
void kvml_input(const char *block, int type);
void kvml_input_bad(void);
// Every iteration of the main loop, before the picture: the queued commands, then the timers (2 s / 15 s without ACK); the answers go out before the next picture
void kvml_poll(void);
// After the writes of an iteration (and when a loop turn did not make a picture): rule M1 (a position after bytes that were written) and the probe marker
void kvml_iteration_end(void);
// The window of the viewer: 1 = a rectangle may be written now (always 1 while ACK is not on). A refusal is counted as "the link is the limit".
int kvml_gate_big(void);
// One write to the pipe of the slave: counts it and writes it with kvm_slave_write_raw. kind: KVM_W_DATA / KVM_W_POSITION / KVM_W_STATE of kvm_stream.h; 0 = written, -1 = error
int kvml_write(const void *buffer, size_t len, int kind);

// The tile scan: a rectangle that the window refused leaves the scan of this frame, and the next frame starts at the tile where it stopped (neither the top of the screen nor the start of a row must starve the rest)
void kvml_scan_deferred(int tile);		// the scan stopped at this tile (index = row * tiles per row + column)
int kvml_scan_start(int tiles);			// the tile where the next scan starts (0 after a scan that went to the end)

#else

static inline void kvml_session_begin(void) {}
static inline void kvml_session_end(void) {}
static inline void kvml_announce(void) {}
static inline void kvml_input_bad(void) {}
static inline void kvml_poll(void) {}
static inline void kvml_iteration_end(void) {}
static inline int kvml_gate_big(void) { return 1; }
static inline int kvml_scan_start(int tiles) { (void)tiles; return 0; }
static inline void kvml_scan_deferred(int tile) { (void)tile; }

#endif

// Set by getTileAt (linux_tile.c) when the window of the viewer refused a changed rectangle; the scan of the frame stops and the caller clears it
extern int g_tileDeferred;

// The write that is not counted (linux_kvm.c): the DRM backend's draining writer, or a blocking write with the partial writes finished
int kvm_slave_write_raw(const void *buffer, size_t len);
// What every sender in the slave calls (counted when the stream layer is on)
int kvm_slave_send(const void *buffer, size_t len);

#endif /* LINUX_KVM_STREAM_H_ */
