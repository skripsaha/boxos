#include "hardware_test.h"
#include "hardware_deck.h"
#include "process.h"
#include "guide.h"
#include "klib.h"
#include "result_page.h"
#include "vmm.h"
#include "rtc.h"

void test_hardware_deck(void) {
    kprintf("\n====================================\n");
    kprintf("HARDWARE DECK TEST\n");
    kprintf("====================================\n");

    process_t* proc_no_tags = process_create("test:hardware:notags");
    if (!proc_no_tags) {
        debug_printf("[TEST] ERROR: Cannot create process without tags\n");
        return;
    }

    result_page_t* result_page = (result_page_t*)vmm_phys_to_virt(proc_no_tags->result_page_phys);
    if (!result_page) {
        debug_printf("[TEST] ERROR: Cannot map result page\n");
        process_destroy(proc_no_tags);
        return;
    }

    int passed = 0;
    int total = 0;

    // Test 1: TIMER_GET_TICKS (READ - no tags required)
    {
        total++;
        kprintf("\n[TEST %d] TIMER_GET_TICKS: Read current ticks (no tags)\n", total);

        Event event;
        event_init(&event, proc_no_tags->pid, guide_alloc_event_id());
        event.prefix_count = 2;
        event.prefixes[0] = 0x0310;
        event.prefixes[1] = 0x0000;

        process_ref_inc(proc_no_tags);
        event_ring_push(kernel_event_ring, &event);
        guide_wake();
        guide_run();

        for (volatile int i = 0; i < 1000000; i++);

        uint32_t head = result_page->ring.head;
        uint32_t tail = result_page->ring.tail;

        if (head != tail) {
            result_entry_t* entry = &result_page->ring.entries[head % RESULT_RING_SIZE];

            if (entry->error_code == BOXOS_OK) {
                uint64_t ticks = ((uint64_t)entry->payload[0] << 56) |
                                 ((uint64_t)entry->payload[1] << 48) |
                                 ((uint64_t)entry->payload[2] << 40) |
                                 ((uint64_t)entry->payload[3] << 32) |
                                 ((uint64_t)entry->payload[4] << 24) |
                                 ((uint64_t)entry->payload[5] << 16) |
                                 ((uint64_t)entry->payload[6] << 8) |
                                 ((uint64_t)entry->payload[7]);

                debug_printf("[TEST %d] PASS: Got %llu ticks (timer operational)\n", total, ticks);
                passed++;
            } else {
                debug_printf("[TEST %d] FAIL: Event status %u\n", total, entry->error_code);
            }

            result_page->ring.head = (head + 1) % RESULT_RING_SIZE;
        } else {
            debug_printf("[TEST %d] FAIL: No result\n", total);
        }
        process_ref_dec(proc_no_tags);
    }

    // Test 2: TIMER_GET_MS (READ - no tags required)
    {
        total++;
        kprintf("\n[TEST %d] TIMER_GET_MS: Read milliseconds (no tags)\n", total);

        Event event;
        event_init(&event, proc_no_tags->pid, guide_alloc_event_id());
        event.prefix_count = 2;
        event.prefixes[0] = 0x0311;
        event.prefixes[1] = 0x0000;

        process_ref_inc(proc_no_tags);
        event_ring_push(kernel_event_ring, &event);
        guide_wake();
        guide_run();

        for (volatile int i = 0; i < 1000000; i++);

        uint32_t head = result_page->ring.head;
        uint32_t tail = result_page->ring.tail;

        if (head != tail) {
            result_entry_t* entry = &result_page->ring.entries[head % RESULT_RING_SIZE];

            if (entry->error_code == BOXOS_OK) {
                uint64_t ms = ((uint64_t)entry->payload[0] << 56) |
                              ((uint64_t)entry->payload[1] << 48) |
                              ((uint64_t)entry->payload[2] << 40) |
                              ((uint64_t)entry->payload[3] << 32) |
                              ((uint64_t)entry->payload[4] << 24) |
                              ((uint64_t)entry->payload[5] << 16) |
                              ((uint64_t)entry->payload[6] << 8) |
                              ((uint64_t)entry->payload[7]);

                debug_printf("[TEST %d] PASS: Got %llu ms since boot\n", total, ms);
                passed++;
            } else {
                debug_printf("[TEST %d] FAIL: Event status %u\n", total, entry->error_code);
            }

            result_page->ring.head = (head + 1) % RESULT_RING_SIZE;
        } else {
            debug_printf("[TEST %d] FAIL: No result\n", total);
        }
        process_ref_dec(proc_no_tags);
    }

    // Create process with hardware tag for privileged operations
    process_t* proc_hw = process_create("test:hardware:privileged");
    if (!proc_hw) {
        debug_printf("[TEST] ERROR: Cannot create process with hardware tag\n");
        process_destroy(proc_no_tags);
        return;
    }
    process_add_tag(proc_hw, "hardware");

    result_page_t* result_page_hw = (result_page_t*)vmm_phys_to_virt(proc_hw->result_page_phys);
    if (!result_page_hw) {
        debug_printf("[TEST] ERROR: Cannot map result page for hw process\n");
        process_destroy(proc_no_tags);
        process_destroy(proc_hw);
        return;
    }

    // Test 3: PORT_INB (allowed port 0x80, requires hardware tag)
    {
        total++;
        kprintf("\n[TEST %d] PORT_INB: Read from POST code port (0x80) with hardware tag\n", total);

        Event event;
        event_init(&event, proc_hw->pid, guide_alloc_event_id());
        event.prefix_count = 2;
        event.prefixes[0] = 0x0320;
        event.prefixes[1] = 0x0000;

        event.data[0] = 0x00;
        event.data[1] = 0x80;

        process_ref_inc(proc_hw);
        event_ring_push(kernel_event_ring, &event);
        guide_wake();
        guide_run();

        for (volatile int i = 0; i < 1000000; i++);

        uint32_t head = result_page_hw->ring.head;
        uint32_t tail = result_page_hw->ring.tail;

        if (head != tail) {
            result_entry_t* entry = &result_page_hw->ring.entries[head % RESULT_RING_SIZE];

            if (entry->error_code == BOXOS_OK) {
                uint8_t value = entry->payload[0];
                debug_printf("[TEST %d] PASS: Read value 0x%02x from port 0x80\n", total, value);
                passed++;
            } else {
                debug_printf("[TEST %d] FAIL: Event status %u (expected COMPLETED)\n", total, entry->error_code);
            }

            result_page_hw->ring.head = (head + 1) % RESULT_RING_SIZE;
        } else {
            debug_printf("[TEST %d] FAIL: No result\n", total);
        }
        process_ref_dec(proc_hw);
    }

    // Test 4: PORT_INB (denied port 0x20 - PIC, even with hardware tag)
    {
        total++;
        kprintf("\n[TEST %d] PORT_INB: Read from PIC port (0x20) - should be DENIED\n", total);

        Event event;
        event_init(&event, proc_hw->pid, guide_alloc_event_id());
        event.prefix_count = 2;
        event.prefixes[0] = 0x0320;
        event.prefixes[1] = 0x0000;

        event.data[0] = 0x00;
        event.data[1] = 0x20;

        process_ref_inc(proc_hw);
        event_ring_push(kernel_event_ring, &event);
        guide_wake();
        guide_run();

        for (volatile int i = 0; i < 1000000; i++);

        uint32_t head = result_page_hw->ring.head;
        uint32_t tail = result_page_hw->ring.tail;

        if (head != tail) {
            result_entry_t* entry = &result_page_hw->ring.entries[head % RESULT_RING_SIZE];

            if (entry->error_code == BOXOS_ERR_ACCESS_DENIED) {
                debug_printf("[TEST %d] PASS: Port access correctly denied\n", total);
                passed++;
            } else {
                debug_printf("[TEST %d] FAIL: Expected ACCESS_DENIED, got status %u\n", total, entry->error_code);
            }

            result_page_hw->ring.head = (head + 1) % RESULT_RING_SIZE;
        } else {
            debug_printf("[TEST %d] FAIL: No result\n", total);
        }
        process_ref_dec(proc_hw);
    }

    // Test 5: IRQ_ENABLE (valid IRQ, requires hardware tag)
    {
        total++;
        kprintf("\n[TEST %d] IRQ_ENABLE: Enable IRQ 3 (COM2) with hardware tag\n", total);

        Event event;
        event_init(&event, proc_hw->pid, guide_alloc_event_id());
        event.prefix_count = 2;
        event.prefixes[0] = 0x0332;
        event.prefixes[1] = 0x0000;

        event.data[0] = 3;

        process_ref_inc(proc_hw);
        event_ring_push(kernel_event_ring, &event);
        guide_wake();
        guide_run();

        for (volatile int i = 0; i < 1000000; i++);

        uint32_t head = result_page_hw->ring.head;
        uint32_t tail = result_page_hw->ring.tail;

        if (head != tail) {
            result_entry_t* entry = &result_page_hw->ring.entries[head % RESULT_RING_SIZE];

            if (entry->error_code == BOXOS_OK && entry->payload[0] == 0) {
                debug_printf("[TEST %d] PASS: IRQ 3 enabled\n", total);
                passed++;
            } else {
                debug_printf("[TEST %d] FAIL: Status %u, result %u\n", total, entry->error_code, entry->payload[0]);
            }

            result_page_hw->ring.head = (head + 1) % RESULT_RING_SIZE;
        } else {
            debug_printf("[TEST %d] FAIL: No result\n", total);
        }
        process_ref_dec(proc_hw);
    }

    // Test 6: RTC_GET_TIME (opcode 0x15, no tags required)
    {
        total++;
        kprintf("\n[TEST %d] RTC_GET_TIME: Read current RTC time (no tags)\n", total);

        Event event;
        event_init(&event, proc_no_tags->pid, guide_alloc_event_id());
        event.prefix_count = 2;
        event.prefixes[0] = 0x0315;
        event.prefixes[1] = 0x0000;

        process_ref_inc(proc_no_tags);
        event_ring_push(kernel_event_ring, &event);
        guide_wake();
        guide_run();

        for (volatile int i = 0; i < 1000000; i++);

        uint32_t head = result_page->ring.head;
        uint32_t tail = result_page->ring.tail;

        if (head != tail) {
            result_entry_t* entry = &result_page->ring.entries[head % RESULT_RING_SIZE];

            if (entry->error_code == BOXOS_OK) {
                uint64_t seconds = ((uint64_t)entry->payload[0] << 56) |
                                   ((uint64_t)entry->payload[1] << 48) |
                                   ((uint64_t)entry->payload[2] << 40) |
                                   ((uint64_t)entry->payload[3] << 32) |
                                   ((uint64_t)entry->payload[4] << 24) |
                                   ((uint64_t)entry->payload[5] << 16) |
                                   ((uint64_t)entry->payload[6] << 8)  |
                                   ((uint64_t)entry->payload[7]);
                uint32_t nanosec = ((uint32_t)entry->payload[8] << 24) |
                                   ((uint32_t)entry->payload[9] << 16) |
                                   ((uint32_t)entry->payload[10] << 8) |
                                   ((uint32_t)entry->payload[11]);
                uint16_t year  = (uint16_t)(((uint16_t)entry->payload[12] << 8) | entry->payload[13]);
                uint8_t  month   = entry->payload[14];
                uint8_t  day     = entry->payload[15];
                uint8_t  hour    = entry->payload[16];
                uint8_t  minute  = entry->payload[17];
                uint8_t  second  = entry->payload[18];
                uint8_t  weekday = entry->payload[19];
                (void)weekday;
                debug_printf("[TEST %d] PASS: Current time: %04u-%02u-%02u %02u:%02u:%02u (seconds=%llu, ns=%u)\n",
                    total, year, month, day, hour, minute, second, seconds, nanosec);
                passed++;
            } else {
                debug_printf("[TEST %d] FAIL: Event status %u\n", total, entry->error_code);
            }

            result_page->ring.head = (head + 1) % RESULT_RING_SIZE;
        } else {
            debug_printf("[TEST %d] FAIL: No result\n", total);
        }
        process_ref_dec(proc_no_tags);
    }

    // Test 7: RTC_GET_UNIX64 (opcode 0x16, no tags required)
    {
        total++;
        kprintf("\n[TEST %d] RTC_GET_UNIX64: Read seconds since epoch (no tags)\n", total);

        Event event;
        event_init(&event, proc_no_tags->pid, guide_alloc_event_id());
        event.prefix_count = 2;
        event.prefixes[0] = 0x0316;
        event.prefixes[1] = 0x0000;

        process_ref_inc(proc_no_tags);
        event_ring_push(kernel_event_ring, &event);
        guide_wake();
        guide_run();

        for (volatile int i = 0; i < 1000000; i++);

        uint32_t head = result_page->ring.head;
        uint32_t tail = result_page->ring.tail;

        if (head != tail) {
            result_entry_t* entry = &result_page->ring.entries[head % RESULT_RING_SIZE];

            if (entry->error_code == BOXOS_OK) {
                uint64_t epoch_secs = ((uint64_t)entry->payload[0] << 56) |
                                     ((uint64_t)entry->payload[1] << 48) |
                                     ((uint64_t)entry->payload[2] << 40) |
                                     ((uint64_t)entry->payload[3] << 32) |
                                     ((uint64_t)entry->payload[4] << 24) |
                                     ((uint64_t)entry->payload[5] << 16) |
                                     ((uint64_t)entry->payload[6] << 8)  |
                                     ((uint64_t)entry->payload[7]);
                debug_printf("[TEST %d] PASS: Seconds since epoch: %llu\n", total, epoch_secs);
                passed++;
            } else {
                debug_printf("[TEST %d] FAIL: Event status %u\n", total, entry->error_code);
            }

            result_page->ring.head = (head + 1) % RESULT_RING_SIZE;
        } else {
            debug_printf("[TEST %d] FAIL: No result\n", total);
        }
        process_ref_dec(proc_no_tags);
    }

    // Test 8: RTC_GET_UPTIME (opcode 0x17, no tags required)
    {
        total++;
        kprintf("\n[TEST %d] RTC_GET_UPTIME: Read system uptime in nanoseconds (no tags)\n", total);

        Event event;
        event_init(&event, proc_no_tags->pid, guide_alloc_event_id());
        event.prefix_count = 2;
        event.prefixes[0] = 0x0317;
        event.prefixes[1] = 0x0000;

        process_ref_inc(proc_no_tags);
        event_ring_push(kernel_event_ring, &event);
        guide_wake();
        guide_run();

        for (volatile int i = 0; i < 1000000; i++);

        uint32_t head = result_page->ring.head;
        uint32_t tail = result_page->ring.tail;

        if (head != tail) {
            result_entry_t* entry = &result_page->ring.entries[head % RESULT_RING_SIZE];

            if (entry->error_code == BOXOS_OK) {
                uint64_t uptime_ns = ((uint64_t)entry->payload[0] << 56) |
                                    ((uint64_t)entry->payload[1] << 48) |
                                    ((uint64_t)entry->payload[2] << 40) |
                                    ((uint64_t)entry->payload[3] << 32) |
                                    ((uint64_t)entry->payload[4] << 24) |
                                    ((uint64_t)entry->payload[5] << 16) |
                                    ((uint64_t)entry->payload[6] << 8)  |
                                    ((uint64_t)entry->payload[7]);
                uint64_t uptime_ms = uptime_ns / 1000000;
                debug_printf("[TEST %d] PASS: Uptime: %llu ns (%llu ms)\n", total, uptime_ns, uptime_ms);
                passed++;
            } else {
                debug_printf("[TEST %d] FAIL: Event status %u\n", total, entry->error_code);
            }

            result_page->ring.head = (head + 1) % RESULT_RING_SIZE;
        } else {
            debug_printf("[TEST %d] FAIL: No result\n", total);
        }
        process_ref_dec(proc_no_tags);
    }

    process_destroy(proc_no_tags);
    process_destroy(proc_hw);

    kprintf("\n====================================\n");
    kprintf("HARDWARE DECK TEST COMPLETE\n");
    kprintf("Results: %d/%d tests passed\n", passed, total);
    kprintf("====================================\n\n");
}
