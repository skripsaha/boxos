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
    if (!tag) return NULL;
    if (role != CURRENT_READ && role != CURRENT_WRITE) return NULL;

    const char    *name    = NULL;
    CurrentBackend  backend = ResolveBackend(tag, &name);

    Current *c = (Current *)malloc(sizeof(Current));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    c->backend = backend;
    c->role    = role;
    c->flags   = flags;

    switch (backend) {
    case CurScreen:
        if (role != CURRENT_WRITE) { free(c); return NULL; }
        c->caps = CURRENT_CAP_WRITE;
        return c;

    case CurLog:
        if (role != CURRENT_WRITE) { free(c); return NULL; }
        c->caps = CURRENT_CAP_WRITE;
        return c;

    case CurKeyboard:
        if (role != CURRENT_READ) { free(c); return NULL; }
        c->caps = CURRENT_CAP_READ;
        return c;

    case CurFile: {
        if (!name || name[0] == '\0') { free(c); return NULL; }
        uint32_t    ids[4];
        file_info_t infos[4];
        int n = find_file_by_name(name, ids, infos, 4);
        if (n > 0) {
            c->file_id = ids[0];
        } else if (role == CURRENT_WRITE && (flags & CURRENT_CREATE)) {
            int fid = create(name, "");
            if (fid < 0) { free(c); return NULL; }
            c->file_id = (uint32_t)fid;
        } else {
            free(c); return NULL;   /* read of a missing file, or write w/o CREATE */
        }
        c->file_pos = 0;
        c->caps = (role == CURRENT_WRITE ? CURRENT_CAP_WRITE : CURRENT_CAP_READ)
                | CURRENT_CAP_SEEKABLE | CURRENT_CAP_CLOSEABLE;
        return c;
    }

    case CurStream: {
        if (item_size == 0 || item_size > BROOK_FRAME_MAX) { free(c); return NULL; }
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
        if (!b) { free(c); return NULL; }

        if (role == CURRENT_READ) {
            uint32_t want = item_size < BROOK_FRAME_MIN ? BROOK_FRAME_MIN : item_size;
            if (frame_bytes != want) { brook_release(b); free(c); return NULL; }
        }

        c->brook       = b;
        c->item_size   = item_size;
        c->frame_bytes = frame_bytes;
        if (frame_bytes != item_size) {
            c->frame_buf = (uint8_t *)malloc(frame_bytes);
            if (!c->frame_buf) { brook_release(b); free(c); return NULL; }
        }
        c->caps = (role == CURRENT_WRITE ? CURRENT_CAP_WRITE : CURRENT_CAP_READ)
                | CURRENT_CAP_FRAMED | CURRENT_CAP_BACKPRESSURE | CURRENT_CAP_CLOSEABLE;
        return c;
    }

    default:
        free(c);
        return NULL;
    }
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
        int n = fwrite(c->file_id, c->file_pos, data, len);
        if (n < 0) return -ERR_WRITE_FAILED;
        c->file_pos += (uint64_t)n;
        return n;
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
        int n = fread(c->file_id, c->file_pos, buf, len);
        if (n < 0) return -ERR_READ_FAILED;
        if (n == 0) return CURRENT_CLOSED;     /* content exhausted */
        c->file_pos += (uint64_t)n;
        return n;
    }

    case CurStream: {
        if (len < c->item_size) return -ERR_BUFFER_TOO_SMALL;
        void *dst = c->frame_buf ? (void *)c->frame_buf : buf;
        int rc = (c->flags & CURRENT_NONBLOCK)
               ? brook_try_pop(c->brook, dst)
               : brook_pop(c->brook, dst);
        if (rc == 0) {
            if (c->frame_buf) memcpy(buf, c->frame_buf, c->item_size);
            return (int)c->item_size;
        }
        if (rc == -ERR_STREAM_CLOSED) return CURRENT_CLOSED;
        return rc;   /* -ERR_WOULD_BLOCK, -ERR_* */
    }

    default:
        return -ERR_INVALID_OPERATION;
    }
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
 * Introspection
 * ------------------------------------------------------------------------ */

uint32_t current_caps(const Current *c)      { return c ? c->caps : 0; }
uint32_t current_item_size(const Current *c) { return c ? c->item_size : 0; }
