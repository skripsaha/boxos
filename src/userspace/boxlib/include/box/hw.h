#ifndef BOX_HW_H
#define BOX_HW_H

#include "box/types.h"
#include "box/error.h"

/*
 * Real-HW per-process knobs (LAM mode today; CET SSP, TME context KeyID
 * land here as they ship). Region-tag operations live in box/memtag.h
 * because they target a region, not the calling process.
 */

/* Linear Address Masking (Intel SDM Vol 3A §5.6) — per-process opt-in.
 * Sets CR3.LAM_U48 (bit 62) or CR3.LAM_U57 (bit 61) on the next reload.
 * U48 ignores bits 62..48 of user virtual addresses (HWASAN-style tag
 * stash); U57 needs 5-level paging which BoxOS doesn't enable, so set
 * with U57 currently returns -ERR_INVALID_ARGS. */
typedef enum {
    HW_LAM_NONE = 0,
    HW_LAM_U48  = 1,
    HW_LAM_U57  = 2,
} hw_lam_mode_t;

/* Read the current process's LAM mode. Returns hw_lam_mode_t on success
 * or -ERR_* on failure. */
int hw_lam_get(void);

/* Set the current process's LAM mode. Returns 0 on success or -ERR_*. */
int hw_lam_set(hw_lam_mode_t mode);

#endif /* BOX_HW_H */
