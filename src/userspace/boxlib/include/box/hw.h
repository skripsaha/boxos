#ifndef BOX_HW_H
#define BOX_HW_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/error.h"


typedef enum {
    HW_LAM_NONE = 0,
    HW_LAM_U48  = 1,
    HW_LAM_U57  = 2,
} hw_lam_mode_t;

int hw_lam_get(void);

int hw_lam_set(hw_lam_mode_t mode);

typedef struct {
    uint8_t   tme_active;
    uint8_t   mk_active;
    uint8_t   num_keyid_bits;
    uint8_t   activated_alg;
    uint16_t  max_keyid;
    uint16_t  pool_programmed;
    uint16_t  in_use;
    uint16_t  per_proc_quota;
    uint8_t   reduced_maxphyaddr;
    uint8_t   _pad[3];
    uint16_t  this_proc_held;
} hw_tme_state_t;

int hw_tme_state(hw_tme_state_t *out);

#ifdef __cplusplus
}
#endif

#endif