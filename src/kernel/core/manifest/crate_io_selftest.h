#ifndef CRATE_IO_SELFTEST_H
#define CRATE_IO_SELFTEST_H

#include "ktypes.h"
#include "error.h"
#include "vmm.h"
#include "process.h"


typedef struct {
    vmm_context_t *vmm;
    process_t     *proc;
    cabin_t       *cabin;
    uintptr_t      base_va;
    void          *block;
    void          *guard_frame;
    uint8_t       *page_a;
    uint8_t       *spacer;
    uint8_t       *page_b;
    uint8_t       *guard;
    uintptr_t      phys_a;
    uintptr_t      phys_b;
} NonContigPages;

bool NonContigPagesSetup(NonContigPages *m, uintptr_t base_va);

void NonContigPagesTeardown(NonContigPages *m);

error_t CrateIoSelfTest(void);

#endif