#ifndef BOX_CURRENT_H
#define BOX_CURRENT_H


#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"
#include "box/error.h"

typedef struct Current Current;

#define CURRENT_READ        0x01u
#define CURRENT_WRITE       0x02u

#define CURRENT_CREATE      0x10u
#define CURRENT_NONBLOCK    0x20u
#define CURRENT_TRUNCATE    0x40u

#define CURRENT_CAP_READ          0x01u
#define CURRENT_CAP_WRITE         0x02u
#define CURRENT_CAP_FRAMED        0x04u
#define CURRENT_CAP_BACKPRESSURE  0x08u
#define CURRENT_CAP_CLOSEABLE     0x10u
#define CURRENT_CAP_SEEKABLE      0x20u
#define CURRENT_CAP_RESIZABLE     0x40u

#define CURRENT_CLOSED      0

#define CURRENT_NO_READER   (-ERR_PROCESS_TERMINATED)


Current *current_open(const char *tag, uint32_t role, uint32_t item_size, uint32_t flags);

Current *current_open_ex(const char *tag, uint32_t role, uint32_t item_size,
                         uint32_t flags, error_t *out_err);

int current_release(Current *c);

int current_close(Current *c);


int current_write(Current *c, const void *data, size_t len);

int current_read(Current *c, void *buf, size_t len);

int current_flush(Current *c);

static inline int current_put(Current *c, const void *item);
static inline int current_take(Current *c, void *item);

int current_take_now(Current *c, void *item);

int current_take_for(Current *c, void *item, uint32_t ms);

int      current_seek(Current *c, uint64_t offset);
uint64_t current_tell(const Current *c);


int64_t current_size(const Current *c);

int current_resize(Current *c, uint64_t new_size);

uint32_t current_caps(const Current *c);
uint32_t current_item_size(const Current *c);

uint64_t current_lost(const Current *c);

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

#endif