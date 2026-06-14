#ifndef BOX_CURRENT_H
#define BOX_CURRENT_H

/* ============================================================================
 * Current — the BoxOS-native I/O spine.
 *
 * A Current is a TAG-NAMED, ROLE-BASED, optionally TYPED channel that a
 * program opens to read or write a sequence of data. It is BoxOS's answer to
 * the Unix stdin/stdout/stderr/fd model — but it is NOT that model:
 *
 *   Unix fd                          Current
 *   -------                          -------
 *   numbered (0/1/2), inherited      named by tag, opened on demand
 *   untyped bytes                    typed items (framed) or bytes
 *   destination set by the parent    destination IS the tag (late binding)
 *   EOF (Ctrl-D / file size)         honest "writer closed" (CURRENT_CLOSED)
 *   copy through the kernel          lock-free / cross-cabin where the
 *                                    backing allows it
 *
 * Each Current is backed by an existing BoxOS primitive, chosen from the tag:
 *
 *   "screen"      -> console output  (display daemon / VGA; byte sink)
 *   "keyboard"    -> console input   (line-oriented blocking source)
 *   "log"         -> serial log      (kdbg diagnostic byte sink)
 *   "file:NAME"   -> a TagFS file    (seekable byte storage)
 *   <other tag>   -> a Brook stream  (framed, cross-cabin, with honest close)
 *
 * screen / keyboard / log are the three CONVENTIONAL Currents — the named
 * counterpart of stdout / stdin / stderr — but they are not special, magic,
 * or limited to three: any other tag opens a Brook stream between cabins.
 *
 * Deliberately NOT Currents: Bay (random-access shared memory) and Touch
 * (multicast events). Those are different shapes and keep their own APIs;
 * folding them under one "stream" concept would repeat the Unix
 * "everything is a file" mistake.
 *
 * Honesty rule: a Current never fakes a capability its backing lacks. Query
 * current_caps(); operations a backing does not support return
 * -ERR_INVALID_OPERATION rather than silently no-op'ing.
 * ========================================================================== */

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"
#include "box/error.h"

typedef struct Current Current;

/* Role — pass exactly one to current_open. */
#define CURRENT_READ        0x01u
#define CURRENT_WRITE       0x02u

/* Open flags. */
#define CURRENT_CREATE      0x10u  /* create the backing if absent (stream / file writer) */
#define CURRENT_NONBLOCK    0x20u  /* put/take never block: -ERR_WOULD_BLOCK when full/empty */

/* Capability bits — query with current_caps(). Honest per-backing. */
#define CURRENT_CAP_READ          0x01u  /* current_read / current_take usable */
#define CURRENT_CAP_WRITE         0x02u  /* current_write / current_put usable */
#define CURRENT_CAP_FRAMED        0x04u  /* item-granular transfer (fixed item_size) */
#define CURRENT_CAP_BACKPRESSURE  0x08u  /* writer can block until the consumer catches up */
#define CURRENT_CAP_CLOSEABLE     0x10u  /* a reader can observe end-of-stream (CURRENT_CLOSED) */
#define CURRENT_CAP_SEEKABLE      0x20u  /* random access by byte offset (current_seek) */

/* Reader sentinel: current_read / current_take return CURRENT_CLOSED (0) when
 * no more data will arrive — the writer closed a stream, or a file's content
 * is exhausted. Only CURRENT_CAP_CLOSEABLE backings ever return it; a live
 * keyboard has no end and blocks instead. This is the honest replacement for
 * Unix EOF: tied to the writer's lifetime, never to "end of file". */
#define CURRENT_CLOSED      0

/* Writer signal: current_write / current_put return CURRENT_NO_READER when the
 * stream's reader has released or crashed and no one will consume further
 * items. The honest Current name for the underlying peer-gone condition. */
#define CURRENT_NO_READER   (-ERR_PROCESS_TERMINATED)

/* --------------------------------------------------------------------------
 * Open / close
 * ------------------------------------------------------------------------ */

/* Open (or, with CURRENT_CREATE, create) the Current named `tag`.
 *
 *   role      : CURRENT_READ or CURRENT_WRITE (exactly one).
 *   item_size : framed backings (stream) — bytes per item (>= 1; values < 8
 *               are padded to the Brook minimum transparently, <= 65536).
 *               byte backings (screen/keyboard/log/file) — pass 0.
 *   flags     : CURRENT_CREATE | CURRENT_NONBLOCK.
 *
 * Returns a handle, or NULL on failure (bad args, role unsupported by the
 * backing, unknown tag without CREATE, shape mismatch on a stream). */
Current *current_open(const char *tag, uint32_t role, uint32_t item_size, uint32_t flags);

/* Release this cabin's handle. For a stream WRITER this also closes the stream
 * (the reader observes CURRENT_CLOSED once it drains). The pointer is invalid
 * afterward. Returns OK or -ERR_*. Passing NULL is a no-op (OK). */
int current_release(Current *c);

/* Writer: announce end-of-stream now, WITHOUT releasing the handle. On a
 * closeable backing the reader observes CURRENT_CLOSED after draining; on a
 * byte backing this flushes. Idempotent. */
int current_close(Current *c);

/* --------------------------------------------------------------------------
 * Transfer — byte core
 * ------------------------------------------------------------------------ */

/* Write bytes. Byte backings accept any length. Framed (stream) backings
 * require len == item_size (exactly one item per call).
 * Returns bytes accepted (== len on success), CURRENT_NO_READER (stream reader
 * gone), -ERR_WOULD_BLOCK (NONBLOCK and full), or -ERR_*. */
int current_write(Current *c, const void *data, size_t len);

/* Read bytes (up to `len`; a stream returns exactly one item). Blocks until
 * data is available unless CURRENT_NONBLOCK.
 * Returns bytes produced (> 0), CURRENT_CLOSED (0) at end-of-stream,
 * -ERR_WOULD_BLOCK (NONBLOCK and empty), or -ERR_*. */
int current_read(Current *c, void *buf, size_t len);

/* Flush any buffered bytes to the backing now (byte backings; no-op for
 * streams, which never buffer in the handle). */
int current_flush(Current *c);

/* --------------------------------------------------------------------------
 * Transfer — framed sugar (CURRENT_CAP_FRAMED backings). One item of exactly
 * item_size bytes. Equivalent to current_write/current_read at item_size.
 * ------------------------------------------------------------------------ */
static inline int current_put(Current *c, const void *item);
static inline int current_take(Current *c, void *item);

/* --------------------------------------------------------------------------
 * Random access (CURRENT_CAP_SEEKABLE backings — file). Sets the byte cursor
 * used by subsequent current_read / current_write.
 * ------------------------------------------------------------------------ */
int      current_seek(Current *c, uint64_t offset);
uint64_t current_tell(const Current *c);

/* --------------------------------------------------------------------------
 * Introspection
 * ------------------------------------------------------------------------ */
uint32_t current_caps(const Current *c);       /* CURRENT_CAP_* bitset */
uint32_t current_item_size(const Current *c);  /* framed: bytes/item; byte: 0 */

/* Framed sugar definitions (need current_item_size). */
static inline int current_put(Current *c, const void *item)
{
    return current_write(c, item, current_item_size(c));
}
static inline int current_take(Current *c, void *item)
{
    return current_read(c, item, current_item_size(c));
}

#ifdef __cplusplus
}
#endif

#endif /* BOX_CURRENT_H */
