#ifndef BOXOS_POCKET_H
#define BOXOS_POCKET_H


#ifndef __packed
#define __packed __attribute__((packed))
#endif


#define GATE_YIELD                   1u
#define POCKET_FLAG_MANIFEST         0x40
#define POCKET_FLAG_MANIFEST_HANDLE  0x20
#define POCKET_FLAG_ENCLOSED         0x10

#define POCKET_ENCLOSURE_MAX         88u

typedef struct __packed {
    uint32_t pid;
    uint32_t target_pid;
    uint32_t error_code;
    uint8_t  flags;
    uint8_t  cookie24[3];
    uint32_t manifest_size;
    uint16_t crate_count;
    uint16_t pier_id;
    uint64_t manifest_addr;
    uint64_t crates_addr;
    uint8_t  enclosure[POCKET_ENCLOSURE_MAX];
} Pocket;

#if !defined(__cplusplus) && !defined(static_assert) && \
    (!defined(__STDC_VERSION__) || (__STDC_VERSION__ < 202311L))
#define static_assert _Static_assert
#endif

static_assert(sizeof(Pocket) == 128, "Pocket must be 128 bytes (one ring slot)");

static inline uint32_t PocketCookie24(const Pocket *p)
{
    return (uint32_t)p->cookie24[0]
         | ((uint32_t)p->cookie24[1] << 8)
         | ((uint32_t)p->cookie24[2] << 16);
}

static inline void PocketSetCookie24(Pocket *p, uint32_t ck)
{
    p->cookie24[0] = (uint8_t)(ck & 0xFFu);
    p->cookie24[1] = (uint8_t)((ck >> 8) & 0xFFu);
    p->cookie24[2] = (uint8_t)((ck >> 16) & 0xFFu);
}
static_assert(__builtin_offsetof(Pocket, enclosure) == 40,
              "Pocket envelope fields must stay 40 bytes so the enclosure fills the slot");

#endif