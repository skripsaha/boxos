#ifndef CRATE_STAGE_H
#define CRATE_STAGE_H

#include "ktypes.h"
#include "error.h"
#include "boxos_crate.h"

#include "vmm.h"

error_t crate_stage_in(vmm_context_t        *cabin,
                        uint64_t              user_uaddr,
                        uint16_t              count,
                        Crate               **out_crates);

error_t crate_stage_commit_and_release(Crate                *crates,
                                        uint16_t              count,
                                        vmm_context_t        *cabin,
                                        uint64_t              user_uaddr);

void crate_stage_release_no_commit(Crate *crates);

#endif