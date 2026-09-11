
#include "box/current.h"
#include "box/print.h"
#include "box/debug.h"
#include "box/file.h"
#include "box/brook.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/core/manifest.h"
#include "boxos_decks.h"
#include "box/timeouts.h"

#define HW_LOG_READ      0x83
#define HW_LOG_PREVIOUS  0x84


typedef enum {
    CurScreen = 0,
    CurKeyboard,
    CurLogSerial,
    CurLogFile,
    CurFile,
    CurStream,
} CurrentBackend;

struct Current {
    CurrentBackend backend;
    uint32_t       role;
    uint32_t       flags;
    uint32_t       caps;
    uint32_t       item_size;

    Brook         *brook;
    uint32_t       frame_bytes;
    uint8_t       *frame_buf;
    bool           closed;

    uint32_t       file_id;
    uint64_t       file_pos;

    uint8_t       *log_buf;
    uint64_t       log_pos;
    uint64_t       log_end;
    uint64_t       log_lost;
    uint16_t       log_op;
};

#define STREAM_FRAMES_DEFAULT  256u
#define BROOK_FRAME_MIN        8u
#define BROOK_FRAME_MAX        65536u


#define CUR_LOG_HEADER  24u
#define CUR_LOG_CHUNK   4096u

static int cur_log_pull(Current *c, uint64_t from, uint32_t want,
                        uint64_t *out_oldest, uint64_t *out_written)
{
    uint8_t params[8];
    for (unsigned i = 0; i < 8; i++) params[i] = (uint8_t)(from >> (i * 8));

    if (want > CUR_LOG_CHUNK) want = CUR_LOG_CHUNK;

    int rc = MfCall1(DECK_HARDWARE, c->log_op,
                     params, sizeof(params),
                     NULL, 0,
                     c->log_buf, CUR_LOG_HEADER + want,
                     NULL, BOX_ANSWER_GUARANTEED, NULL);
    if (rc != OK) return rc > 0 ? -rc : rc;

    uint64_t oldest, written, copied;
    memcpy(&oldest,  c->log_buf,      sizeof(uint64_t));
    memcpy(&written, c->log_buf + 8,  sizeof(uint64_t));
    memcpy(&copied,  c->log_buf + 16, sizeof(uint64_t));

    if (copied > want) return -ERR_CORRUPTED;

    if (out_oldest)  *out_oldest  = oldest;
    if (out_written) *out_written = written;
    return (int)copied;
}

static CurrentBackend ResolveBackend(const char *tag, const char **name_out)
{
    *name_out = NULL;
    if (strcmp(tag, "screen") == 0)    return CurScreen;
    if (strcmp(tag, "keyboard") == 0)  return CurKeyboard;
    if (strcmp(tag, "log:serial") == 0) return CurLogSerial;
    if (strcmp(tag, "log:file") == 0)   return CurLogFile;
    if (strcmp(tag, "log:previous") == 0) return CurLogFile;
    if (strncmp(tag, "file:", 5) == 0) { *name_out = tag + 5; return CurFile; }
    return CurStream;
}


Current *current_open(const char *tag, uint32_t role, uint32_t item_size, uint32_t flags)
{
    return current_open_ex(tag, role, item_size, flags, NULL);
}

Current *current_open_ex(const char *tag, uint32_t role, uint32_t item_size,
                         uint32_t flags, error_t *out_err)
{
#define CUR_FAIL(err) do { if (out_err) *out_err = (err); return NULL; } while (0)

    if (!tag) CUR_FAIL(ERR_INVALID_ARGUMENT);
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

    case CurLogSerial:
        if (role != CURRENT_WRITE) { free(c); CUR_FAIL(ERR_INVALID_ARGUMENT); }
        c->caps = CURRENT_CAP_WRITE;
        if (out_err) *out_err = OK;
        return c;

    case CurLogFile: {
        if (role != CURRENT_READ) { free(c); CUR_FAIL(ERR_INVALID_ARGUMENT); }

        c->log_op = (strcmp(tag, "log:previous") == 0) ? HW_LOG_PREVIOUS
                                                       : HW_LOG_READ;

        c->log_buf = (uint8_t *)malloc(CUR_LOG_HEADER + CUR_LOG_CHUNK);
        if (!c->log_buf) { free(c); CUR_FAIL(ERR_NO_MEMORY); }

        uint64_t oldest = 0, written = 0;
        int rc = cur_log_pull(c, 0, 0, &oldest, &written);
        if (rc < 0) {
            error_t why = (error_t)(-rc);
            free(c->log_buf); free(c);
            CUR_FAIL(why);
        }
        c->log_pos  = oldest;
        c->log_end  = written;
        c->log_lost = oldest;

        c->caps = CURRENT_CAP_READ;
        if (out_err) *out_err = OK;
        return c;
    }

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
            free(c); CUR_FAIL(ERR_FILE_NOT_FOUND);
        }
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
        if (role == (CURRENT_READ | CURRENT_WRITE)) { free(c); CUR_FAIL(ERR_INVALID_ARGUMENT); }
        if (item_size == 0 || item_size > BROOK_FRAME_MAX) { free(c); CUR_FAIL(ERR_INVALID_ARGUMENT); }
        uint32_t frame_bytes = item_size < BROOK_FRAME_MIN ? BROOK_FRAME_MIN : item_size;

        Brook *b;
        if (role == CURRENT_WRITE) {
            b = brook_open(tag, frame_bytes, STREAM_FRAMES_DEFAULT,
                           BROOK_WRITER | BROOK_CREATE);
        } else {
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
            brook_release(c->brook);
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
    case CurLogFile:
        if (c->log_buf) free(c->log_buf);
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


static int cur_pop_try(Brook *b, void *f, uint32_t ms)   { (void)ms; return brook_try_pop(b, f); }
static int cur_pop_block(Brook *b, void *f, uint32_t ms) { (void)ms; return brook_pop(b, f); }

static int cur_stream_take(Current *c, void *item,
                           int (*pop)(Brook *, void *, uint32_t), uint32_t ms)
{
    if (!c->brook) return CURRENT_CLOSED;
    void *dst = c->frame_buf ? (void *)c->frame_buf : item;
    int rc = pop(c->brook, dst, ms);
    if (rc == 0) {
        if (c->frame_buf) memcpy(item, c->frame_buf, c->item_size);
        return (int)c->item_size;
    }
    if (rc == -ERR_STREAM_CLOSED) return CURRENT_CLOSED;
    return rc;
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

    case CurLogSerial: {
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
        size_t  req = len > (size_t)INT32_MAX ? (size_t)INT32_MAX : len;
        int64_t n   = fwrite(c->file_id, c->file_pos, data, req);
        if (n < 0) return -ERR_WRITE_FAILED;
        c->file_pos += (uint64_t)n;
        return (int)n;
    }

    case CurStream: {
        if (len != c->item_size) return -ERR_INVALID_ARGUMENT;
        if (!c->brook) return CURRENT_NO_READER;
        const void *frame = data;
        if (c->frame_buf) {
            memset(c->frame_buf, 0, c->frame_bytes);
            memcpy(c->frame_buf, data, c->item_size);
            frame = c->frame_buf;
        }
        int rc = (c->flags & CURRENT_NONBLOCK)
               ? brook_try_push(c->brook, frame)
               : brook_push(c->brook, frame);
        if (rc == 0) return (int)len;
        if (rc == -ERR_PROCESS_TERMINATED) return CURRENT_NO_READER;
        return rc;
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
        int n = readline((char *)buf, len);
        if (n < 0) return -ERR_READ_FAILED;
        return n;
    }

    case CurFile: {
        size_t  req = len > (size_t)INT32_MAX ? (size_t)INT32_MAX : len;
        int64_t n   = fread(c->file_id, c->file_pos, buf, req);
        if (n < 0) return -ERR_READ_FAILED;
        if (n == 0) return CURRENT_CLOSED;
        c->file_pos += (uint64_t)n;
        return (int)n;
    }

    case CurLogFile: {
        if (c->log_pos >= c->log_end) return CURRENT_CLOSED;

        uint64_t remain = c->log_end - c->log_pos;
        uint32_t want   = (len < remain) ? (uint32_t)len : (uint32_t)remain;

        uint64_t oldest = 0, written = 0;
        int n = cur_log_pull(c, c->log_pos, want, &oldest, &written);
        if (n < 0)  return n;

        if (oldest > c->log_pos) {
            c->log_lost += oldest - c->log_pos;
            c->log_pos   = oldest;
        }
        if (n == 0) return CURRENT_CLOSED;

        memcpy(buf, c->log_buf + CUR_LOG_HEADER, (size_t)n);
        c->log_pos += (uint64_t)n;
        return n;
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
    return -ERR_INVALID_OPERATION;
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


int64_t current_size(const Current *c)
{
    if (!c) return -ERR_NULL_POINTER;
    if (!(c->caps & CURRENT_CAP_RESIZABLE)) return -ERR_INVALID_OPERATION;

    file_info_t info;
    int rc = file_info(c->file_id, &info);
    if (rc < 0)  return (int64_t)rc;
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

    if (c->file_pos > new_size) c->file_pos = new_size;
    return OK;
}


uint32_t current_caps(const Current *c)      { return c ? c->caps : 0; }
uint32_t current_item_size(const Current *c) { return c ? c->item_size : 0; }

uint64_t current_lost(const Current *c)
{
    return (c && c->backend == CurLogFile) ? c->log_lost : 0;
}