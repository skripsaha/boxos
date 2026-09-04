#include "box/timeouts.h"
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
                     BOX_ANSWER_WATCHDOG_MS, 0);
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
                   BOX_ANSWER_WATCHDOG_MS, 0);
}

int hw_tme_state(hw_tme_state_t *out) {
    if (!out) return -ERR_NULL_POINTER;
    return MfCall1(DECK_SYSTEM, SYSTEM_OP_HW_TME_STATE,
                   0, 0,
                   0, 0,
                   out, sizeof(*out), 0,
                   BOX_ANSWER_WATCHDOG_MS, 0);
}
