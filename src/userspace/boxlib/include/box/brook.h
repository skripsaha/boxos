#ifndef BOX_BROOK_H
#define BOX_BROOK_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/error.h"


#define BROOK_WRITER     0x01u
#define BROOK_READER     0x02u
#define BROOK_CREATE     0x10u
#define BROOK_STREAM     0x20u

typedef struct Brook Brook;

Brook *brook_open(const char *tag,
                  uint32_t    frame_size,
                  uint32_t    frame_count,
                  uint32_t    flags);

int brook_release(Brook *b);

int brook_push(Brook *b, const void *frame);

int brook_try_push(Brook *b, const void *frame);

int brook_push_timeout(Brook *b, const void *frame, uint32_t timeout_ms);

int brook_pop(Brook *b, void *frame);

int brook_try_pop(Brook *b, void *frame);
int brook_pop_timeout(Brook *b, void *frame, uint32_t timeout_ms);

uint32_t brook_frame_size(const Brook *b);
uint32_t brook_frame_count(const Brook *b);

uint32_t brook_available(const Brook *b);
uint32_t brook_free(const Brook *b);

bool brook_writer_ever_attached(const Brook *b);

int brook_bell_hang(Brook *b, uint32_t strand_pid);
int brook_bell_take(Brook *b);

uint64_t brook_handle_header_va(const Brook *b);

#ifdef __cplusplus
}
#endif

#endif