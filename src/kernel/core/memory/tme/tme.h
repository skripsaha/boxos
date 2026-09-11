#ifndef TME_H
#define TME_H

#include "ktypes.h"
#include "error.h"


#define TME_MAX_KEYIDS  256u

#define TME_CMD_SET_KEY_DIRECT  0u
#define TME_CMD_SET_KEY_RANDOM  1u
#define TME_CMD_CLEAR_KEY       2u
#define TME_CMD_NO_ENCRYPT      3u

#define TME_PCONFIG_SUCCESS                0u
#define TME_PCONFIG_INVALID_PROG_CMD       1u
#define TME_PCONFIG_ENTROPY_ERROR          2u
#define TME_PCONFIG_INVALID_KEYID          3u
#define TME_PCONFIG_INVALID_ENC_ALG        4u
#define TME_PCONFIG_DEVICE_BUSY            5u

typedef struct {
    bool      mk_active;
    bool      tme_active;
    uint8_t   num_keyid_bits;
    uint16_t  max_keyid;
    uint16_t  reserved;
    uint8_t   reduced_maxphyaddr;
    uint8_t   activated_alg;
    uint8_t   pconfig_alg;
    uint8_t   pad;
    uint32_t  pool_size;
    uint32_t  pool_programmed;
    uint32_t  in_use;
} TmeState;

extern TmeState g_tme;

error_t tme_init_bsp(void);

error_t tme_keyid_alloc(uint16_t *out_keyid);

error_t tme_keyid_free(uint16_t keyid);

uint64_t tme_phys_with_keyid(uint64_t phys, uint16_t keyid);

uint64_t tme_phys_strip_keyid(uint64_t phys_with_keyid);

error_t tme_zero_pages_with_keyid(uintptr_t phys_base, size_t pages,
                                   uint16_t keyid, bool huge_2m);

void tme_dump_state(void);

bool tme_is_safe_for_dma(uintptr_t phys_with_keyid);

#endif