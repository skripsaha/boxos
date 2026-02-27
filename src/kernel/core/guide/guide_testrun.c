#include "guide.h"
#include "process.h"
#include "klib.h"
#include "error.h"

void guide_testrun(void) {
    kprintf("\n");
    kprintf("====================================\n");
    kprintf("GUIDE DISPATCHER TEST\n");
    kprintf("====================================\n");

    debug_printf("[TEST] Creating test process for Guide...\n");
    process_t* proc = process_create("app guide_test");
    if (!proc) {
        debug_printf("[TEST] FAILED: Could not create process\n");
        return;
    }

    debug_printf("[TEST] Manually creating test event...\n");
    Event event;
    event_init(&event, proc->pid, guide_alloc_event_id());
    event.prefixes[0] = 0x0000;
    event.prefix_count = 1;
    event.current_prefix_idx = 0;

    debug_printf("[TEST] Pushing event to EventRing...\n");
    process_ref_inc(proc);
    if (event_ring_push(kernel_event_ring, &event) != BOXOS_OK) {
        debug_printf("[TEST] FAILED: EventRing full\n");
        process_ref_dec(proc);
        process_destroy(proc);
        return;
    }

    debug_printf("[TEST] Event queued (event_id=%u, PID=%u)\n", event.event_id, proc->pid);

    process_set_state(proc, PROC_BLOCKED);
    debug_printf("[TEST] Process state set to BLOCKED\n");

    debug_printf("[TEST] Running Guide dispatcher...\n");
    guide_run();

    debug_printf("[TEST] Checking process state after Guide run...\n");
    process_state_t state = process_get_state(proc);
    if (state == PROC_READY) {
        debug_printf("[TEST] SUCCESS: Process state = READY\n");
    } else {
        debug_printf("[TEST] FAILED: Process state = %u (expected READY)\n", state);
    }

    if (proc->result_there) {
        debug_printf("[TEST] SUCCESS: result_there flag set\n");
    } else {
        debug_printf("[TEST] FAILED: result_there flag not set\n");
    }

    if (proc->score == 50) {
        debug_printf("[TEST] SUCCESS: score = 50 (Hot Result bonus)\n");
    } else {
        debug_printf("[TEST] FAILED: score = %d (expected 50)\n", proc->score);
    }

    debug_printf("[TEST] Cleaning up...\n");
    process_set_state(proc, PROC_TERMINATED);
    while (process_ref_count(proc) > 0) {
        process_ref_dec(proc);
    }
    process_destroy(proc);

    debug_printf("[TEST] Guide Dispatcher test complete!\n");
    kprintf("====================================\n");
    kprintf("\n");
}
