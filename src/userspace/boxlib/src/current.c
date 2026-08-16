/* ============================================================================
 * current.c — Current, the BoxOS-native I/O spine.
 *
 * Dispatches one tag-named, role-based channel onto an existing BoxOS
 * primitive (console / serial / TagFS / Brook). See box/current.h for the
 * concept and the contract. No new kernel mechanism: this is a userspace
 * spine over primitives that already exist.
 * ========================================================================== */

#include "box/current.h"
#include "box/print.h"    /* print_bytes, readline, io_flush      */
#include "box/debug.h"    /* kdbg                                 */
#include "box/file.h"     /* fread, fwrite, find_file_by_name, create */
#include "box/brook.h"    /* brook_open/push/pop/release/...      */
#include "box/memory.h"   /* malloc, free                         */
#include "box/string.h"   /* strcmp, strncmp, memcpy, memset      */

/* --------------------------------------------------------------------------
 * Backend selection + handle layout
 * ------------------------------------------------------------------------ */

typedef enum {
    CurScreen = 0,
    CurKeyboard,
    CurLog,
    CurFile,
    CurStream,
} CurrentBackend;

struct Current {
    CurrentBackend backend;
    uint32_t       role;        /* CURRENT_READ | CURRENT_WRITE */
    uint32_t       flags;       /* CURRENT_NONBLOCK, ...         */
    uint32_t       caps;        /* CURRENT_CAP_* bitset          */
    uint32_t       item_size;   /* framed logical item size; 0 for byte backings */

    /* stream (Brook) backing */
    Brook         *brook;
    uint32_t       frame_bytes; /* actual Brook frame size (>= item_size)        */
    uint8_t       *frame_buf;   /* staging buffer when frame_bytes != item_size  */
    bool           closed;      /* writer announced close                        */

    /* file (TagFS) backing */
    uint32_t       file_id;
    uint64_t       file_pos;
};

#define STREAM_FRAMES_DEFAULT  256u
#define BROOK_FRAME_MIN        8u
#define BROOK_FRAME_MAX        65536u

static CurrentBackend ResolveBackend(const char *tag, const char **name_out)
{
    *name_out = NULL;
    if (strcmp(tag, "screen") == 0)    return CurScreen;
    if (strcmp(tag, "keyboard") == 0)  return CurKeyboard;
    if (strcmp(tag, "log") == 0)       return CurLog;
    if (strncmp(tag, "file:", 5) == 0) { *name_out = tag + 5; return CurFile; }
    return CurStream;
}

/* --------------------------------------------------------------------------
 * Open / close
 * ------------------------------------------------------------------------ */

Current *current_open(const char *tag, uint32_t role, uint32_t item_size, uint32_t flags)
{
    return current_open_ex(tag, role, item_size, flags, NULL);
}

Current *current_open_ex(const char *tag, uint32_t role, uint32_t item_size,
                         uint32_t flags, error_t *out_err)
{
#define CUR_FAIL(err) do { if (out_err) *out_err = (err); return NULL; } while (0)

    if (!tag) CUR_FAIL(ERR_INVALID_ARGUMENT);
    /* One role, except on the file backing, which genuinely is both. A stream
     * writer and a stream reader are different ends of a pipe and cannot be
     * the same handle; a file is one object that can be read and written
     * through one cursor. The honesty rule cuts both ways — a Current must not
     * claim a capability its backing lacks, and it should not hide one it has.
     * Every other backing still takes exactly one role, checked per case. */
    if (role != CURRENT_READ && role != CURRENT_WRITE &&
        role != (CURRENT_READ | CURRENT_WRITE))
        CUR_FAIL(ERR_INVALID_ARGUMENT);

    const char    *name    = NULL;
    CurrentBackend  backend = ResolveBackend(tag, &name);

    Current *c = (Current *)malloc(sizeof(Current));
    if (!c) CUR_FAIL(ERR_NO_MEMORY);
    memset(c, 0, sizeof(*c));
    c->backend = backend;
    c->role    = role;
    c->flags   = flags;

    switch (backend) {
    case CurScreen:
        if (role != CURRENT_WRITE) { free(c); CUR_FAIL(ERR_INVALID_ARGUMENT); }
        c->caps = CURRENT_CAP_WRITE;
        if (out_err) *out_err = OK;
        return c;

    case CurLog:
        if (role != CURRENT_WRITE) { free(c); CUR_FAIL(ERR_INVALID_ARGUMENT); }
        c->caps = CURRENT_CAP_WRITE;
        if (out_err) *out_err = OK;
        return c;

    case CurKeyboard:
        if (role != CURRENT_READ) { free(c); CUR_FAIL(ERR_INVALID_ARGUMENT); }
        c->caps = CURRENT_CAP_READ;
        if (out_err) *out_err = OK;
        return c;

    case CurFile: {
        if (!name || name[0] == '\0') { free(c); CUR_FAIL(ERR_INVALID_ARGUMENT); }
        uint32_t    ids[4];
        file_info_t infos[4];
        int n = find_file_by_name(name, ids, infos, 4);
        if (n > 0) {
            c->file_id = ids[0];
        } else if ((role & CURRENT_WRITE) && (flags & CURRENT_CREATE)) {
            int fid = create(name, "");
            if (fid < 0) { free(c); CUR_FAIL(box_errno_of(fid)); }
            c->file_id = (uint32_t)fid;
        } else {
            /* read of a missing file, or write w/o CREATE */
            free(c); CUR_FAIL(ERR_FILE_NOT_FOUND);
        }
        /* CURRENT_TRUNCATE: start the writer from an empty file rather than
         * over the top of the old one. Without this a shorter rewrite leaves
         * the previous tail readable past the new content — the hole that
         * made <fstream>'s ios_base::out impossible to implement honestly.
         * A refusal is fatal to the open: silently keeping the old bytes is
         * the one outcome the caller definitely did not ask for. */
        if ((role & CURRENT_WRITE) && (flags & CURRENT_TRUNCATE)) {
            int trc = file_truncate(c->file_id, 0);
            if (trc != 0) { free(c); CUR_FAIL(box_errno_of(trc)); }
        }
        c->file_pos = 0;
        c->caps = ((role & CURRENT_WRITE) ? CURRENT_CAP_WRITE : 0u)
                | ((role & CURRENT_READ)  ? CURRENT_CAP_READ  : 0u)
                | CURRENT_CAP_SEEKABLE | CURRENT_CAP_CLOSEABLE
                | CURRENT_CAP_RESIZABLE;
        if (out_err) *out_err = OK;
        return c;
    }

    case CurStream: {
        /* A Brook end is a producer or a consumer, never both: the two roles
         * open different objects. Reject the combined role here rather than
         * letting it fall through the reader branch below. */
        if (role == (CURRENT_READ | CURRENT_WRITE)) { free(c); CUR_FAIL(ERR_INVALID_ARGUMENT); }
        if (item_size == 0 || item_size > BROOK_FRAME_MAX) { free(c); CUR_FAIL(ERR_INVALID_ARGUMENT); }
        uint32_t frame_bytes = item_size < BROOK_FRAME_MIN ? BROOK_FRAME_MIN : item_size;

        Brook *b;
        if (role == CURRENT_WRITE) {
            /* The SPSC stream writer is always its own creator (a second
             * writer gets ERR_BUSY), so creation is implicit — a stream
             * writer needs no CURRENT_CREATE ceremony. */
            b = brook_open(tag, frame_bytes, STREAM_FRAMES_DEFAULT,
                           BROOK_WRITER | BROOK_CREATE);
        } else {
            /* Reader inherits the live stream's shape, then we verify it
             * matches the caller's expected item size. */
            b = brook_open(tag, 0, 0, BROOK_READER);
            if (b) frame_bytes = brook_frame_size(b);
        }
        if (!b) { free(c); CUR_FAIL(ERR_IO); }

        if (role == CURRENT_READ) {
            uint32_t want = item_size < BROOK_FRAME_MIN ? BROOK_FRAME_MIN : item_size;
            if (frame_bytes != want) { brook_release(b); free(c); CUR_FAIL(ERR_INVALID_ARGUMENT); }
        }

        c->brook       = b;
        c->item_size   = item_size;
        c->frame_bytes = frame_bytes;
        if (frame_bytes != item_size) {
            c->frame_buf = (uint8_t *)malloc(frame_bytes);
            if (!c->frame_buf) { brook_release(b); free(c); CUR_FAIL(ERR_NO_MEMORY); }
        }
        c->caps = (role == CURRENT_WRITE ? CURRENT_CAP_WRITE : CURRENT_CAP_READ)
                | CURRENT_CAP_FRAMED | CURRENT_CAP_BACKPRESSURE | CURRENT_CAP_CLOSEABLE;
        if (out_err) *out_err = OK;
        return c;
    }

    default:
        free(c);
        CUR_FAIL(ERR_INVALID_ARGUMENT);
    }

#undef CUR_FAIL
}

int current_close(Current *c)
{
    if (!c) return -ERR_NULL_POINTER;
    switch (c->backend) {
    case CurStream:
        if (c->brook && !c->closed) {
            brook_release(c->brook);   /* the reader observes CURRENT_CLOSED once drained */
            c->brook  = NULL;
            c->closed = true;
        }
        return OK;
    case CurScreen:
        io_flush();
        return OK;
    default:
        return OK;
    }
}

int current_release(Current *c)
{
    if (!c) return OK;
    switch (c->backend) {
    case CurStream:
        if (c->brook)     brook_release(c->brook);
        if (c->frame_buf) free(c->frame_buf);
        break;
    case CurScreen:
        io_flush();
        break;
    default:
        break;
    }
    free(c);
    return OK;
}

/* --------------------------------------------------------------------------
 * Transfer
 * ------------------------------------------------------------------------ */

/* Uniform-signature shims so one helper drives try / timeout / blocking pops. */
static int cur_pop_try(Brook *b, void *f, uint32_t ms)   { (void)ms; return brook_try_pop(b, f); }
static int cur_pop_block(Brook *b, void *f, uint32_t ms) { (void)ms; return brook_pop(b, f); }
/* brook_pop_timeout already matches (Brook*, void*, uint32_t). */

/* Shared CurStream framed-take core: stage through frame_buf when the logical
 * item is smaller than the Brook frame, map the writer-leave terminal. */
static int cur_stream_take(Current *c, void *item,
                           int (*pop)(Brook *, void *, uint32_t), uint32_t ms)
{
    if (!c->brook) return CURRENT_CLOSED;              /* writer side already released it */
    void *dst = c->frame_buf ? (void *)c->frame_buf : item;
    int rc = pop(c->brook, dst, ms);
    if (rc == 0) {
        if (c->frame_buf) memcpy(item, c->frame_buf, c->item_size);
        return (int)c->item_size;
    }
    if (rc == -ERR_STREAM_CLOSED) return CURRENT_CLOSED;
    return rc;                                          /* -ERR_WOULD_BLOCK / -ERR_TIMEOUT / -ERR_* */
}

int current_write(Current *c, const void *data, size_t len)
{
    if (!c) return -ERR_NULL_POINTER;
    if (!(c->caps & CURRENT_CAP_WRITE)) return -ERR_INVALID_OPERATION;
    if (!data && len) return -ERR_NULL_POINTER;

    switch (c->backend) {
    case CurScreen:
        print_bytes((const char *)data, len);
        return (int)len;

    case CurLog: {
        /* Serial diagnostic log is a TEXT channel: kdbg takes NUL-terminated
         * strings, so bytes are emitted in NUL-bounded chunks. Embedded NULs
         * truncate a chunk (documented: log carries text). */
        const char *p   = (const char *)data;
        size_t      rem = len;
        char        tmp[256];
        while (rem > 0) {
            size_t chunk = rem < sizeof(tmp) - 1 ? rem : sizeof(tmp) - 1;
            memcpy(tmp, p, chunk);
            tmp[chunk] = '\0';
            kdbg(tmp);
            p   += chunk;
            rem -= chunk;
        }
        return (int)len;
    }

    case CurFile: {
        /* current_write is an int-returning channel primitive; cap the request
         * so the widened int64 fwrite count fits an int (short-write — callers
         * loop) instead of truncating a 2-4 GiB count into a negative int. */
        size_t  req = len > (size_t)INT32_MAX ? (size_t)INT32_MAX : len;
        int64_t n   = fwrite(c->file_id, c->file_pos, data, req);
        if (n < 0) return -ERR_WRITE_FAILED;
        c->file_pos += (uint64_t)n;
        return (int)n;
    }

    case CurStream: {
        if (len != c->item_size) return -ERR_INVALID_ARGUMENT;
        if (!c->brook) return CURRENT_NO_READER;   /* already closed */
        const void *frame = data;
        if (c->frame_buf) {                        /* small item: zero-pad to frame */
            memset(c->frame_buf, 0, c->frame_bytes);
            memcpy(c->frame_buf, data, c->item_size);
            frame = c->frame_buf;
        }
        int rc = (c->flags & CURRENT_NONBLOCK)
               ? brook_try_push(c->brook, frame)
               : brook_push(c->brook, frame);
        if (rc == 0) return (int)len;
        if (rc == -ERR_PROCESS_TERMINATED) return CURRENT_NO_READER;
        return rc;   /* -ERR_WOULD_BLOCK, -ERR_* */
    }

    default:
        return -ERR_INVALID_OPERATION;
    }
}

int current_read(Current *c, void *buf, size_t len)
{
    if (!c) return -ERR_NULL_POINTER;
    if (!(c->caps & CURRENT_CAP_READ)) return -ERR_INVALID_OPERATION;
    if (!buf || len == 0) return -ERR_INVALID_ARGUMENT;

    switch (c->backend) {
    case CurKeyboard: {
        int n = readline((char *)buf, len);   /* one edited line, newline stripped */
        if (n < 0) return -ERR_READ_FAILED;
        return n;                              /* never CURRENT_CLOSED — a live console has no end */
    }

    case CurFile: {
        size_t  req = len > (size_t)INT32_MAX ? (size_t)INT32_MAX : len;
        int64_t n   = fread(c->file_id, c->file_pos, buf, req);
        if (n < 0) return -ERR_READ_FAILED;
        if (n == 0) return CURRENT_CLOSED;     /* content exhausted */
        c->file_pos += (uint64_t)n;
        return (int)n;
    }

    case CurStream: {
        if (len < c->item_size) return -ERR_BUFFER_TOO_SMALL;
        return cur_stream_take(c, buf,
                   (c->flags & CURRENT_NONBLOCK) ? cur_pop_try : cur_pop_block, 0);
    }

    default:
        return -ERR_INVALID_OPERATION;
    }
}

int current_take_now(Current *c, void *item)
{
    if (!c) return -ERR_NULL_POINTER;
    if (!(c->caps & CURRENT_CAP_READ)) return -ERR_INVALID_OPERATION;
    if (!item) return -ERR_INVALID_ARGUMENT;
    if (c->backend == CurStream) return cur_stream_take(c, item, cur_pop_try, 0);
    return -ERR_INVALID_OPERATION;   /* keyboard/file/screen/log are not framed streams */
}

int current_take_for(Current *c, void *item, uint32_t ms)
{
    if (!c) return -ERR_NULL_POINTER;
    if (!(c->caps & CURRENT_CAP_READ)) return -ERR_INVALID_OPERATION;
    if (!item) return -ERR_INVALID_ARGUMENT;
    if (c->backend == CurStream) return cur_stream_take(c, item, brook_pop_timeout, ms);
    return -ERR_INVALID_OPERATION;
}

int current_flush(Current *c)
{
    if (!c) return -ERR_NULL_POINTER;
    if (c->backend == CurScreen) io_flush();
    return OK;
}

/* --------------------------------------------------------------------------
 * Random access (file)
 * ------------------------------------------------------------------------ */

int current_seek(Current *c, uint64_t offset)
{
    if (!c) return -ERR_NULL_POINTER;
    if (!(c->caps & CURRENT_CAP_SEEKABLE)) return -ERR_INVALID_OPERATION;
    c->file_pos = offset;
    return OK;
}

uint64_t current_tell(const Current *c)
{
    if (!c || !(c->caps & CURRENT_CAP_SEEKABLE)) return 0;
    return c->file_pos;
}

/* --------------------------------------------------------------------------
 * Extent (file)
 * ------------------------------------------------------------------------ */

int64_t current_size(const Current *c)
{
    if (!c) return -ERR_NULL_POINTER;
    if (!(c->caps & CURRENT_CAP_RESIZABLE)) return -ERR_INVALID_OPERATION;

    file_info_t info;
    int rc = file_info(c->file_id, &info);
    if (rc < 0)  return (int64_t)rc;      /* already a negative -error_t */
    if (rc != 0) return -ERR_IO;
    return (int64_t)info.size;
}

int current_resize(Current *c, uint64_t new_size)
{
    if (!c) return -ERR_NULL_POINTER;
    if (!(c->caps & CURRENT_CAP_RESIZABLE)) return -ERR_INVALID_OPERATION;
    if (!(c->caps & CURRENT_CAP_WRITE))     return -ERR_INVALID_OPERATION;

    int rc = file_truncate(c->file_id, new_size);
    if (rc != 0) return rc;

    /* Keep the cursor inside the file. Leaving it past the end would make the
     * next write re-grow the file through a gap of blocks nobody wrote. */
    if (c->file_pos > new_size) c->file_pos = new_size;
    return OK;
}

/* --------------------------------------------------------------------------
 * Introspection
 * ------------------------------------------------------------------------ */

uint32_t current_caps(const Current *c)      { return c ? c->caps : 0; }
uint32_t current_item_size(const Current *c) { return c ? c->item_size : 0; }
