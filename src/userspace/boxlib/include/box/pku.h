#ifndef BOX_PKU_H
#define BOX_PKU_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/error.h"


#define PKU_MAX_KEYS  16u

uint32_t pku_read_pkru(void);

void     pku_write_pkru(uint32_t value);

int      pku_set_rights(uint8_t pkey, int ad, int wd);

int      pku_get_rights(uint8_t pkey, int *out_ad, int *out_wd);

int      pku_apply_region(uint32_t region_id, uint8_t pkey);

#ifdef __cplusplus
}
#endif

#endif