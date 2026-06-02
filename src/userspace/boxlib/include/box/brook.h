#ifndef BOX_BROOK_H
#define BOX_BROOK_H

#include "box/types.h"
#include "box/error.h"

/*
 * Brook — single-producer/single-consumer (SPSC) ordered streaming
 * primitive over fixed-size frames. Tag-driven discovery, refcounted
 * lifetime, lock-free hot path.
 *
 * Open a Brook with a role and a shape. Both peers MUST agree on
 * frame_size and frame_count (the kernel rejects opens with mismatched
 * shape — stream shape is part of tag identity in BoxOS).
 *
 *   // writer
 *   Brook *b = brook_open("metric:tick", 64, 1024, BROOK_WRITER | BROOK_CREATE);
 *   uint8_t frame[64];
 *   build_frame(frame);
 *   brook_push(b, frame);             // blocks if reader hasn't drained
 *   brook_release(b);
 *
 *   // reader
 *   Brook *b = brook_open("metric:tick", 64, 1024, BROOK_READER);
 *   uint8_t frame[64];
 *   while (brook_pop(b, frame) == OK) {  // blocks if empty
 *       handle_frame(frame);
 *   }
 *
 * Hot path runs entirely in userspace (atomic head/tail updates, plain
 * memcpy) and never enters the kernel. The kernel is only invoked
 * when the ring is full (writer) or empty (reader) and the caller
 * asked for blocking semantics — at which point a futex-style
 * re-check inside the syscall closes the lost-wakeup race against the
 * peer's atomic clear of the wake-request flag.
 *
 * Backpressure: brook_push blocks when full (no frame loss). Use
 * brook_try_push for drop-on-full or brook_push_timeout for bounded
 * waits. Reader-side mirrors.
 *
 * Peer death: when one side releases (or its cabin is destroyed), the
 * surviving peer's next blocking call returns:
 *   - writer: -ERR_PROCESS_TERMINATED  (reader gone — no further drain)
 *   - reader: -ERR_END_OF_FILE         (writer gone AND ring empty)
 *   - reader: still OK while frames remain in the ring after writer
 *     left; the EOF marker fires only when the ring drains
 *
 * Limits:
 *   frame_size  ∈ [8, 65536]                 (must be ≥ 8 bytes, ≤ 64 KiB)
 *   frame_count ∈ [2, 16384]   AND power-of-2
 *   frame_size × frame_count ≤ 1 GiB
 *
 * Larger payloads belong in Bay (zero-copy shared memory); use Brook
 * for control events / metric ticks / log lines / audit records.
 */

#define BROOK_WRITER     0x01u
#define BROOK_READER     0x02u
#define BROOK_CREATE     0x10u
/* BROOK_STREAM — opt-in streaming semantics. Default (single-session)
 * mode: pop returns -ERR_END_OF_FILE on writer-leave (after draining
 * any remaining frames); push returns -ERR_PROCESS_TERMINATED on
 * reader-leave. That terminal signal is RACE-FREE via an atomic
 * compare-and-swap on the shared *_alive flag — once the survivor
 * commits to terminal, no future writer/reader can attach to this
 * Brook session (the next brook_open with the same tag fails with
 * -ERR_INVALID_STATE; callers must use a different tag).
 *
 * BROOK_STREAM disables the terminal signal: pop blocks through
 * writer-leave/re-attach cycles forever (use brook_pop_timeout for
 * bounded waits), push blocks through reader-leave/re-attach. Suitable
 * for long-running daemons that legitimately swap producers/consumers
 * over a stream's lifetime. The race-elimination CAS isn't needed in
 * this mode — there's no terminal decision to race against. */
#define BROOK_STREAM     0x20u

typedef struct Brook Brook;

/* Open or create a Brook. Returns NULL on failure (check box_last_error
 * if your runtime tracks it; the underlying kernel error is in errno-
 * style negative codes returned by the underlying syscall). flags MUST
 * contain exactly one of {BROOK_WRITER, BROOK_READER}. Add BROOK_CREATE
 * to create the Brook if its tag is unbound; CREATE requires non-zero
 * frame_size and frame_count.
 *
 * For BROOK_OPEN (no CREATE) you may pass frame_size=0 and frame_count=0
 * — the existing Brook's shape wins and is reported via brook_frame_size
 * / brook_frame_count. If you pass non-zero values, they MUST match the
 * existing Brook or open fails with ERR_ALREADY_EXISTS. */
Brook *brook_open(const char *tag,
                  uint32_t    frame_size,
                  uint32_t    frame_count,
                  uint32_t    flags);

/* Release this cabin's claim on the Brook. After release the Brook
 * pointer is invalid. If both peers have released, backing pages
 * return to PMM; if a peer is still attached, its next blocking
 * push/pop sees peer-death and returns the appropriate error code. */
int brook_release(Brook *b);

/* Push one frame_size-byte frame. Blocking on full ring.
 * Returns:
 *   OK (0)                     — frame written
 *   -ERR_PROCESS_TERMINATED    — reader released or died
 *   -ERR_INVALID_ARGUMENT      — NULL pointer / bad handle */
int brook_push(Brook *b, const void *frame);

/* Non-blocking push. Returns -ERR_WOULD_BLOCK immediately when ring full. */
int brook_try_push(Brook *b, const void *frame);

/* Bounded-wait push. timeout_ms == 0 is the same as brook_push (infinite).
 * Returns -ERR_TIMEOUT if the deadline expires without a free slot. */
int brook_push_timeout(Brook *b, const void *frame, uint32_t timeout_ms);

/* Pop one frame_size-byte frame. Blocking on empty ring.
 * Returns:
 *   OK (0)                     — frame copied into `frame`
 *   -ERR_END_OF_FILE           — writer gone AND ring empty (clean EOS)
 *   -ERR_INVALID_ARGUMENT      — NULL pointer / bad handle */
int brook_pop(Brook *b, void *frame);

int brook_try_pop(Brook *b, void *frame);
int brook_pop_timeout(Brook *b, void *frame, uint32_t timeout_ms);

/* Shape queries — read from the shared header (no syscall). */
uint32_t brook_frame_size(const Brook *b);
uint32_t brook_frame_count(const Brook *b);

/* Live counters — frames currently in ring / free slots remaining. */
uint32_t brook_available(const Brook *b);
uint32_t brook_free(const Brook *b);

#endif /* BOX_BROOK_H */
