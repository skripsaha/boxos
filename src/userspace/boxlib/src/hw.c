#include "box/hw.h"
#include "box/core/manifest.h"
#include "box/string.h"
#include "box/error.h"
#include "boxos_decks.h"  /* SYSTEM_OP_HW_LAM_* — single source */

int hw_lam_get(void) {
    uint8_t mode = 0;
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_HW_LAM_GET,
                     0, 0,
                     0, 0,
                     &mode, sizeof(mode), 0,
                     30000, 0);
    if (rc != 0) return rc;
    return (int)mode;
}

int hw_lam_set(hw_lam_mode_t mode) {
    uint8_t params[1];
    params[0] = (uint8_t)mode;
    return MfCall1(DECK_SYSTEM, SYSTEM_OP_HW_LAM_SET,
                   params, sizeof(params),
                   0, 0,
                   0, 0, 0,
                   30000, 0);
}
