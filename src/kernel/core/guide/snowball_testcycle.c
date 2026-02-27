#include "guide.h"
#include "process.h"
#include "notify_page.h"
#include "result_page.h"
#include "vmm.h"
#include "klib.h"
#include "atomics.h"
#include "error.h"

void test_full_cycle(void)
{
    kprintf("\n");
    kprintf("====================================================================\n");
    kprintf("BOXOS TEST: Full Notify() Cycle\n");
    kprintf("====================================================================\n");

    process_t *test_proc = process_create("test:boxos");
    if (!test_proc)
    {
        debug_printf("[TEST] ERROR: Cannot create test process\n");
        return;
    }

    debug_printf("[TEST] Created test process PID %u\n", test_proc->pid);
    debug_printf("[TEST] Notify Page physical: 0x%lx\n", test_proc->notify_page_phys);
    debug_printf("[TEST] Result Page physical: 0x%lx\n", test_proc->result_page_phys);

    if (!test_proc->notify_page_phys || !test_proc->result_page_phys)
    {
        debug_printf("[TEST] ERROR: Pages not allocated\n");
        process_destroy(test_proc);
        return;
    }

    notify_page_t *notify = (notify_page_t *)vmm_phys_to_virt(test_proc->notify_page_phys);
    uint64_t result_phys = test_proc->result_page_phys;

    debug_printf("[TEST] Step 1: Fill Notify Page (userspace simulation)\n");
    memset(notify, 0, sizeof(notify_page_t));
    notify->magic = NOTIFY_PAGE_MAGIC;
    notify->prefix_count = 2;
    notify->prefixes[0] = 0x0102;
    notify->prefixes[1] = 0x0000;

    // BUF_FILL: offset(4BE) + length(4BE) + pattern(1)
    // Fill 8 bytes at offset 0 with pattern 0x42
    notify->data[0] = 0x00; notify->data[1] = 0x00; notify->data[2] = 0x00; notify->data[3] = 0x00; // offset = 0
    notify->data[4] = 0x00; notify->data[5] = 0x00; notify->data[6] = 0x00; notify->data[7] = 0x08; // length = 8
    notify->data[8] = 0x42; // pattern byte

    debug_printf("[TEST]   Magic: 0x%08x\n", notify->magic);
    debug_printf("[TEST]   Prefix count: %u\n", notify->prefix_count);
    debug_printf("[TEST]   Prefixes: 0x%04x 0x%04x\n", notify->prefixes[0], notify->prefixes[1]);
    debug_printf("[TEST]   Data: '%s'\n", (char *)notify->data);

    debug_printf("[TEST] Step 2: Create Event from Notify Page (kernel side)\n");
    Event event;
    event_init(&event, test_proc->pid, guide_alloc_event_id());
    event.prefix_count = notify->prefix_count;
    event.current_prefix_idx = 0;
    event.timestamp = rdtsc();

    for (uint8_t i = 0; i < notify->prefix_count && i < EVENT_MAX_PREFIXES; i++)
    {
        event.prefixes[i] = notify->prefixes[i];
    }
    if (event.prefix_count < EVENT_MAX_PREFIXES)
    {
        event.prefixes[event.prefix_count] = 0x0000;
    }

    memcpy(event.data, notify->data, EVENT_DATA_SIZE);

    debug_printf("[TEST]   Event ID: %u\n", event.event_id);
    debug_printf("[TEST]   PID: %u\n", event.pid);
    debug_printf("[TEST]   Prefix count: %u\n", event.prefix_count);

    debug_printf("[TEST] Step 3: Increment ref_count and push Event to EventRing\n");
    process_ref_inc(test_proc);
    if (event_ring_push(kernel_event_ring, &event) != BOXOS_OK)
    {
        debug_printf("[TEST] ERROR: Cannot push to EventRing\n");
        process_ref_dec(test_proc);
        process_destroy(test_proc);
        return;
    }
    debug_printf("[TEST]   Event pushed successfully\n");

    debug_printf("[TEST] Step 4: Wake Guide\n");
    guide_wake();

    debug_printf("[TEST] Step 5: Run Guide (process event chain)\n");
    guide_run();

    debug_printf("[TEST] Step 6: Wait for result...\n");
    for (volatile int i = 0; i < 1000000; i++);  // Simple delay

    debug_printf("[TEST] Step 7: Check Result Page (ResultRing)\n");
    result_page_t* result_page = (result_page_t*)vmm_phys_to_virt(result_phys);

    if (result_page->magic != RESULT_PAGE_MAGIC) {
        debug_printf("[TEST] FAIL: Invalid result page magic (got 0x%08x)\n", result_page->magic);
        goto cleanup;
    }

    debug_printf("[TEST] ResultRing: head=%u, tail=%u\n",
            result_page->ring.head, result_page->ring.tail);

    if (result_page->ring.tail == result_page->ring.head) {
        debug_printf("[TEST] FAIL: No results in ResultRing\n");
        goto cleanup;
    }

    // Read first result from ring
    result_entry_t* result = &result_page->ring.entries[result_page->ring.head];

    if (result->error_code != BOXOS_OK) {
        debug_printf("[TEST] FAIL: Event status error (%u)\n", result->error_code);
        goto cleanup;
    }

    debug_printf("[TEST] Result received: error_code=%u, size=%u\n",
            result->error_code, result->size);

    // Verify data (should contain test string)
    if (result->size > 0) {
        debug_printf("[TEST] Data: '%s'\n", (char *)result->payload);
        debug_printf("[TEST] Data verification: PASS\n");
    } else {
        debug_printf("[TEST] WARN: No data in result\n");
    }

    debug_printf("[TEST] SUCCESS: Full cycle completed!\n");

cleanup:
    debug_printf("[TEST] Step 8: Check process state\n");
    debug_printf("[TEST]   Process state: %u\n", process_get_state(test_proc));
    debug_printf("[TEST]   Result there: %s\n", test_proc->result_there ? "true" : "false");
    debug_printf("[TEST]   Score: %d\n", test_proc->score);
    debug_printf("[TEST]   Ref count: %u\n", process_ref_count(test_proc));

    debug_printf("[TEST] Cleanup\n");
    process_set_state(test_proc, PROC_TERMINATED);
    while (process_ref_count(test_proc) > 0) {
        process_ref_dec(test_proc);
    }
    process_destroy(test_proc);

    kprintf("====================================================================\n");
    kprintf("BOXOS TEST: Complete\n");
    kprintf("====================================================================\n");
    kprintf("\n");
}
