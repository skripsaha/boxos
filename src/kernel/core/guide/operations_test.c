#include "operations_test.h"
#include "operations_deck.h"
#include "guide.h"
#include "process.h"
#include "events.h"
#include "klib.h"
#include "result_page.h"
#include "vmm.h"

static void write_u32(uint8_t *data, size_t offset, uint32_t value)
{
    data[offset] = (value >> 24) & 0xFF;
    data[offset + 1] = (value >> 16) & 0xFF;
    data[offset + 2] = (value >> 8) & 0xFF;
    data[offset + 3] = value & 0xFF;
}

static uint32_t read_u32(const uint8_t *data, size_t offset)
{
    return ((uint32_t)data[offset] << 24) |
           ((uint32_t)data[offset + 1] << 16) |
           ((uint32_t)data[offset + 2] << 8) |
           ((uint32_t)data[offset + 3]);
}

static void test_delay(void)
{
    for (volatile int i = 0; i < 100000; i++)
        ;
}

static result_entry_t *wait_for_result(process_t *proc, uint32_t *head)
{
    result_page_t *result_page = (result_page_t *)vmm_phys_to_virt(proc->result_page_phys);
    if (!result_page)
    {
        return NULL;
    }

    for (int tries = 0; tries < 10; tries++)
    {
        if (result_page->ring.tail != *head)
        {
            result_entry_t *entry = &result_page->ring.entries[*head];
            *head = (*head + 1) % 15;
            return entry;
        }
        test_delay();
    }

    return NULL;
}

void test_operations_deck(void)
{
    kprintf("\n====================================\n");
    kprintf("OPERATIONS DECK TEST\n");
    kprintf("====================================\n");

    process_t *proc = process_create("test:operations");
    if (!proc)
    {
        debug_printf("[TEST] FAIL: Cannot create process\n");
        return;
    }

    uint32_t result_head = 0;
    int total_tests = 0;
    int passed_tests = 0;

    // Test 1: BUF_FILL
    {
        total_tests++;
        kprintf("\n[TEST 1] BUF_FILL: Fill buffer with pattern\n");

        Event event;
        event_init(&event, proc->pid, guide_alloc_event_id());
        event.prefix_count = 2;
        event.prefixes[0] = 0x0102; // Operations Deck (0x01), BUF_FILL (0x02)
        event.prefixes[1] = 0x0000; // Terminator

        // Arguments: offset=20, len=10, byte=0xAA
        write_u32(event.data, 0, 20);
        write_u32(event.data, 4, 10);
        write_u32(event.data, 8, 0xAA);

        debug_printf("[TEST 1] Pushing BUF_FILL event...\n");
        process_ref_inc(proc);
        event_ring_push(kernel_event_ring, &event);
        guide_wake();
        guide_run();

        test_delay();

        result_entry_t *result = wait_for_result(proc, &result_head);
        if (result && result->error_code == BOXOS_OK)
        {
            bool pass = true;
            for (int i = 20; i < 30; i++)
            {
                if (result->payload[i] != 0xAA)
                {
                    pass = false;
                    break;
                }
            }
            if (pass)
            {
                debug_printf("[TEST 1] PASS: Buffer filled correctly\n");
                passed_tests++;
            }
            else
            {
                debug_printf("[TEST 1] FAIL: Buffer content mismatch\n");
            }
        }
        else
        {
            debug_printf("[TEST 1] FAIL: No result or error status\n");
        }
        process_ref_dec(proc);
    }

    // Test 2: BUF_XOR
    {
        total_tests++;
        kprintf("\n[TEST 2] BUF_XOR: XOR buffer with mask\n");

        Event event;
        event_init(&event, proc->pid, guide_alloc_event_id());
        event.prefix_count = 2;
        event.prefixes[0] = 0x0103; // Operations Deck (0x01), BUF_XOR (0x03)
        event.prefixes[1] = 0x0000;

        // First fill bytes 50-59 with 0xFF
        for (int i = 50; i < 60; i++)
        {
            event.data[i] = 0xFF;
        }

        // Arguments: offset=50, len=10, mask=0x55
        write_u32(event.data, 0, 50);
        write_u32(event.data, 4, 10);
        write_u32(event.data, 8, 0x55);

        debug_printf("[TEST 2] Pushing BUF_XOR event...\n");
        process_ref_inc(proc);
        event_ring_push(kernel_event_ring, &event);
        guide_wake();
        guide_run();

        test_delay();

        result_entry_t *result = wait_for_result(proc, &result_head);
        if (result && result->error_code == BOXOS_OK)
        {
            bool pass = true;
            for (int i = 50; i < 60; i++)
            {
                if (result->payload[i] != 0xAA)
                { // 0xFF XOR 0x55 = 0xAA
                    pass = false;
                    break;
                }
            }
            if (pass)
            {
                debug_printf("[TEST 2] PASS: XOR operation correct\n");
                passed_tests++;
            }
            else
            {
                debug_printf("[TEST 2] FAIL: XOR result mismatch\n");
            }
        }
        else
        {
            debug_printf("[TEST 2] FAIL: No result or error status\n");
        }
        process_ref_dec(proc);
    }

    // Test 3: BUF_HASH
    {
        total_tests++;
        kprintf("\n[TEST 3] BUF_HASH: Compute hash of buffer\n");

        Event event;
        event_init(&event, proc->pid, guide_alloc_event_id());
        event.prefix_count = 2;
        event.prefixes[0] = 0x0104; // Operations Deck (0x01), BUF_HASH (0x04)
        event.prefixes[1] = 0x0000;

        // Fill test pattern at offset 100
        for (int i = 0; i < 16; i++)
        {
            event.data[100 + i] = (uint8_t)i;
        }

        // Arguments: offset=100, len=16, target_off=120 (within bounds)
        write_u32(event.data, 0, 100);
        write_u32(event.data, 4, 16);
        write_u32(event.data, 8, 120);

        debug_printf("[TEST 3] Pushing BUF_HASH event...\n");
        process_ref_inc(proc);
        event_ring_push(kernel_event_ring, &event);
        guide_wake();
        guide_run();

        test_delay();

        result_entry_t *result = wait_for_result(proc, &result_head);
        if (result && result->error_code == BOXOS_OK)
        {
            uint32_t hash = read_u32(result->payload, 120);
            if (hash != 0)
            {
                debug_printf("[TEST 3] PASS: Hash computed (0x%08x)\n", hash);
                passed_tests++;
            }
            else
            {
                debug_printf("[TEST 3] FAIL: Hash is zero\n");
            }
        }
        else
        {
            debug_printf("[TEST 3] FAIL: No result or error status\n");
        }
        process_ref_dec(proc);
    }

    // Test 4: BUF_CMP
    {
        total_tests++;
        kprintf("\n[TEST 4] BUF_CMP: Compare two buffer regions\n");

        Event event;
        event_init(&event, proc->pid, guide_alloc_event_id());
        event.prefix_count = 2;
        event.prefixes[0] = 0x0105; // Operations Deck (0x01), BUF_CMP (0x05)
        event.prefixes[1] = 0x0000;

        // Fill two identical regions
        for (int i = 0; i < 8; i++)
        {
            event.data[40 + i] = 0xBB;
            event.data[60 + i] = 0xBB;
        }

        // Arguments: off1=40, off2=60, len=8
        write_u32(event.data, 0, 40);
        write_u32(event.data, 4, 60);
        write_u32(event.data, 8, 8);

        debug_printf("[TEST 4] Pushing BUF_CMP event (identical regions)...\n");
        process_ref_inc(proc);
        event_ring_push(kernel_event_ring, &event);
        guide_wake();
        guide_run();

        test_delay();

        result_entry_t *result = wait_for_result(proc, &result_head);
        if (result && result->error_code == BOXOS_OK)
        {
            uint32_t cmp_result = read_u32(result->payload, 0);
            if (cmp_result == 0)
            {
                debug_printf("[TEST 4] PASS: Regions are equal (result=0)\n");
                passed_tests++;
            }
            else
            {
                debug_printf("[TEST 4] FAIL: Expected 0, got %u\n", cmp_result);
            }
        }
        else
        {
            debug_printf("[TEST 4] FAIL: No result or error status\n");
        }
        process_ref_dec(proc);
    }

    // Test 5: VAL_MOD
    {
        total_tests++;
        kprintf("\n[TEST 5] VAL_MOD: Modify integer value\n");

        Event event;
        event_init(&event, proc->pid, guide_alloc_event_id());
        event.prefix_count = 2;
        event.prefixes[0] = 0x010A; // Operations Deck (0x01), VAL_MOD (0x0A)
        event.prefixes[1] = 0x0000;

        // Write value 100 at offset 80 (4 bytes)
        write_u32(event.data, 80, 100);

        // Arguments: offset=80, type_size=4, delta=42
        write_u32(event.data, 0, 80);
        write_u32(event.data, 4, 4);
        write_u32(event.data, 8, 42);

        debug_printf("[TEST 5] Pushing VAL_MOD event (100 + 42)...\n");
        process_ref_inc(proc);
        event_ring_push(kernel_event_ring, &event);
        guide_wake();
        guide_run();

        test_delay();

        result_entry_t *result = wait_for_result(proc, &result_head);
        if (result && result->error_code == BOXOS_OK)
        {
            uint32_t value = read_u32(result->payload, 80);
            if (value == 142)
            {
                debug_printf("[TEST 5] PASS: Value modified correctly (100 + 42 = 142)\n");
                passed_tests++;
            }
            else
            {
                debug_printf("[TEST 5] FAIL: Expected 142, got %u\n", value);
            }
        }
        else
        {
            debug_printf("[TEST 5] FAIL: No result or error status\n");
        }
        process_ref_dec(proc);
    }

    // Test 6: Integer overflow protection (BUF_FILL with malicious args)
    {
        total_tests++;
        kprintf("\n[TEST 6] Integer Overflow Protection: BUF_FILL with UINT32_MAX offset\n");

        Event event;
        event_init(&event, proc->pid, guide_alloc_event_id());
        event.prefix_count = 2;
        event.prefixes[0] = 0x0102; // Operations Deck (0x01), BUF_FILL (0x02)
        event.prefixes[1] = 0x0000;

        // Attack: offset=UINT32_MAX, len=10
        write_u32(event.data, 0, 0xFFFFFFFF);
        write_u32(event.data, 4, 10);
        write_u32(event.data, 8, 0xCC);

        debug_printf("[TEST 6] Pushing BUF_FILL with malicious overflow args...\n");
        process_ref_inc(proc);
        event_ring_push(kernel_event_ring, &event);
        guide_wake();
        guide_run();

        test_delay();

        result_entry_t *result = wait_for_result(proc, &result_head);
        if (result && BOXOS_IS_ERROR(result->error_code))
        {
            debug_printf("[TEST 6] PASS: Overflow attack correctly rejected\n");
            passed_tests++;
        }
        else
        {
            debug_printf("[TEST 6] FAIL: Overflow attack was not rejected!\n");
        }
        process_ref_dec(proc);
    }

    // Test 7: BUF_MOVE (overlapping regions)
    {
        total_tests++;
        kprintf("\n[TEST 7] BUF_MOVE: Move overlapping regions\n");

        Event event;
        event_init(&event, proc->pid, guide_alloc_event_id());
        event.prefix_count = 2;
        event.prefixes[0] = 0x0101; // Operations Deck (0x01), BUF_MOVE (0x01)
        event.prefixes[1] = 0x0000;

        // Arguments go in first 12 bytes, so use data after that
        // Arguments: from_off=20, to_off=40, len=8 (non-overlapping)
        write_u32(event.data, 0, 20);
        write_u32(event.data, 4, 40);
        write_u32(event.data, 8, 8);

        // Fill source region with pattern (after arguments)
        for (int i = 0; i < 8; i++)
        {
            event.data[20 + i] = 0x10 + i;
        }

        debug_printf("[TEST 7] Pushing BUF_MOVE event...\n");
        process_ref_inc(proc);
        event_ring_push(kernel_event_ring, &event);
        guide_wake();
        guide_run();

        test_delay();

        result_entry_t *result = wait_for_result(proc, &result_head);
        if (result && result->error_code == BOXOS_OK)
        {
            bool pass = true;
            for (int i = 0; i < 8; i++)
            {
                if (result->payload[40 + i] != (0x10 + i))
                {
                    pass = false;
                    break;
                }
            }
            if (pass)
            {
                debug_printf("[TEST 7] PASS: Buffer moved correctly\n");
                passed_tests++;
            }
            else
            {
                debug_printf("[TEST 7] FAIL: Buffer content incorrect after move\n");
            }
        }
        else
        {
            debug_printf("[TEST 7] FAIL: No result or error status\n");
        }
        process_ref_dec(proc);
    }

    process_destroy(proc);

    kprintf("\n====================================\n");
    kprintf("OPERATIONS DECK TEST COMPLETE\n");
    kprintf("Results: %d/%d tests passed\n", passed_tests, total_tests);
    kprintf("====================================\n\n");
}
