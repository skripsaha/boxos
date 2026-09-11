#ifndef BOXOS_KCTX_H
#define BOXOS_KCTX_H


typedef enum {
    KCTX_NONE   = 0,
    KCTX_PMM    = 1,
    KCTX_VMM    = 2,
    KCTX_TAGFS  = 3,
    KCTX_AHCI   = 4,
    KCTX_SCHED  = 5,
    KCTX_IPC    = 6,
    KCTX_GUIDE  = 7,
    KCTX_FRIEND = 8,
    KCTX_TOUCH  = 9,
    KCTX_STORAGE = 10,
} KResultContext;

#define KCTX_KIND(ctx)        ((uint32_t)(ctx) & 0xFFu)
#define KCTX_COOKIE24(ctx)    ((uint32_t)(ctx) >> 8)
#define KCTX_PACK24(kind, ck) ((uint32_t)(kind) | ((uint32_t)(ck) << 8))

#endif