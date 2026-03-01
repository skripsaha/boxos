#include "system_test.h"
#include "system_deck.h"
#include "system_deck_process.h"
#include "system_deck_context.h"
#include "scheduler.h"
#include "process.h"
#include "guide.h"
#include "klib.h"
#include "pmm.h"
#include "kernel_config.h"

#if CONFIG_RUN_STARTUP_TESTS

void test_system_deck_security(void) {
    kprintf("\n");
    kprintf("SYSTEM DECK SECURITY TEST\n");

    int passed = 0;
    int total = 0;

    debug_printf("[TEST] Creating test processes...\n");
    process_t* normal = process_create("app");
    process_t* hw_proc = process_create("app,hw_access");
    process_t* system_proc = process_create("system");

    if (!normal || !hw_proc || !system_proc) {
        debug_printf("[TEST] ERROR: Cannot create test processes\n");
        return;
    }

    total++;
    kprintf("\n[TEST %d] Operations Deck access for normal process\n", total);
    if (system_security_gate(normal->pid, 0x01, 0x00)) {
        debug_printf("[TEST %d] PASS: Operations Deck allowed\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: Operations Deck should be allowed\n", total);
    }

    total++;
    kprintf("\n[TEST %d] Hardware Deck access for normal process (should DENY)\n", total);
    if (!system_security_gate(normal->pid, 0x03, 0x00)) {
        debug_printf("[TEST %d] PASS: Hardware Deck correctly denied\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: Hardware Deck should be denied\n", total);
    }

    total++;
    kprintf("\n[TEST %d] Hardware Deck access for hw_access process\n", total);
    if (system_security_gate(hw_proc->pid, 0x03, 0x34)) {
        debug_printf("[TEST %d] PASS: Hardware Deck allowed with hw_access tag\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: Hardware Deck should be allowed\n", total);
    }

    total++;
    kprintf("\n[TEST %d] TAG_ADD for normal process (should DENY)\n", total);
    if (!system_security_gate(normal->pid, 0xFF, 0x20)) {
        debug_printf("[TEST %d] PASS: TAG_ADD correctly denied\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: TAG_ADD should be denied\n", total);
    }

    total++;
    kprintf("\n[TEST %d] TAG_ADD for system process\n", total);
    if (system_security_gate(system_proc->pid, 0xFF, 0x20)) {
        debug_printf("[TEST %d] PASS: TAG_ADD allowed for system process\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: TAG_ADD should be allowed for system\n", total);
    }

    total++;
    kprintf("\n[TEST %d] Add and check tag\n", total);
    process_add_tag(normal, "test_tag");
    if (process_has_tag(normal, "test_tag")) {
        debug_printf("[TEST %d] PASS: Tag added and found\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: Tag not found after adding\n", total);
    }

    total++;
    kprintf("\n[TEST %d] Remove tag\n", total);
    process_remove_tag(normal, "test_tag");
    if (!process_has_tag(normal, "test_tag")) {
        debug_printf("[TEST %d] PASS: Tag removed successfully\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: Tag still present after removal\n", total);
    }

    total++;
    kprintf("\n[TEST %d] Storage Deck access for normal process\n", total);
    if (system_security_gate(normal->pid, 0x02, 0x01)) {
        debug_printf("[TEST %d] PASS: Storage Deck allowed for app\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: Storage Deck should be allowed for app\n", total);
    }

    kprintf("\n[TEST] Cleaning up test processes...\n");
    process_destroy(normal);
    process_destroy(hw_proc);
    process_destroy(system_proc);

    kprintf("\n");
    kprintf("SYSTEM DECK SECURITY TEST COMPLETE\n");
    kprintf("Results: %d/%d tests passed\n", passed, total);
}

void test_system_deck_process_management(void) {
    kprintf("\n");
    kprintf("SYSTEM DECK PROCESS MANAGEMENT TEST\n");

    int passed = 0;
    int total = 0;

    uint8_t test_binary[256];
    for (int i = 0; i < 256; i++) {
        test_binary[i] = (uint8_t)i;
    }

    void* binary_phys_page = pmm_alloc(1);
    if (!binary_phys_page) {
        debug_printf("[TEST] ERROR: Cannot allocate memory for binary\n");
        return;
    }

    void* binary_virt = vmm_phys_to_virt((uintptr_t)binary_phys_page);
    memcpy(binary_virt, test_binary, sizeof(test_binary));

    Event spawn_event;
    event_init(&spawn_event, 1, 1);
    spawn_event.prefixes[0] = 0xFF01;
    spawn_event.prefix_count = 1;

    proc_spawn_event_t* spawn_data = (proc_spawn_event_t*)spawn_event.data;
    spawn_data->binary_phys_addr = (uint64_t)binary_phys_page;
    spawn_data->binary_size = sizeof(test_binary);
    strncpy(spawn_data->tags, "app,test", sizeof(spawn_data->tags) - 1);

    total++;
    kprintf("\n[TEST %d] PROC_SPAWN with invalid binary (should reject)\n", total);
    int result = system_deck_proc_spawn(&spawn_event);
    if (result != 0 || spawn_event.state == EVENT_STATE_ERROR) {
        debug_printf("[TEST %d] PASS: Non-ELF binary correctly rejected\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: Non-ELF binary should be rejected\n", total);
        proc_spawn_response_t* response = (proc_spawn_response_t*)spawn_event.data;
        uint32_t new_pid = response->new_pid;
        process_t* proc = process_find(new_pid);
        if (proc) {
            process_destroy(proc);
        }
    }

    total++;
    kprintf("\n[TEST %d] PROC_SPAWN with invalid arguments (zero address)\n", total);
    Event bad_spawn_event;
    event_init(&bad_spawn_event, 1, 5);
    bad_spawn_event.prefixes[0] = 0xFF01;
    bad_spawn_event.prefix_count = 1;

    proc_spawn_event_t* bad_spawn_data = (proc_spawn_event_t*)bad_spawn_event.data;
    bad_spawn_data->binary_phys_addr = 0;
    bad_spawn_data->binary_size = 1024;
    strncpy(bad_spawn_data->tags, "app", sizeof(bad_spawn_data->tags) - 1);

    result = system_deck_proc_spawn(&bad_spawn_event);
    if (result != 0 && bad_spawn_event.state == EVENT_STATE_ERROR) {
        debug_printf("[TEST %d] PASS: Invalid spawn correctly rejected\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: Invalid spawn should be rejected\n", total);
    }

    total++;
    kprintf("\n[TEST %d] PROC_KILL with invalid PID\n", total);
    Event bad_kill_event;
    event_init(&bad_kill_event, 1, 6);
    bad_kill_event.prefixes[0] = 0xFF02;
    bad_kill_event.prefix_count = 1;

    proc_kill_event_t* bad_kill_data = (proc_kill_event_t*)bad_kill_event.data;
    bad_kill_data->target_pid = 99999;

    result = system_deck_proc_kill(&bad_kill_event);
    if (result != 0 && bad_kill_event.state == EVENT_STATE_ERROR) {
        debug_printf("[TEST %d] PASS: Invalid kill correctly rejected\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: Invalid kill should be rejected\n", total);
    }

    total++;
    kprintf("\n[TEST %d] PROC_INFO with non-existent PID\n", total);
    Event bad_info_event;
    event_init(&bad_info_event, 1, 7);
    bad_info_event.prefixes[0] = 0xFF03;
    bad_info_event.prefix_count = 1;

    proc_info_event_t* bad_info_data = (proc_info_event_t*)bad_info_event.data;
    bad_info_data->target_pid = 99999;

    result = system_deck_proc_info(&bad_info_event);
    if (result != 0 && bad_info_event.state == EVENT_STATE_ERROR) {
        debug_printf("[TEST %d] PASS: Info for non-existent process correctly rejected\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: Info for non-existent process should be rejected\n", total);
    }

    pmm_free(binary_phys_page, 1);

    kprintf("\n");
    kprintf("PROCESS MANAGEMENT TEST COMPLETE\n");
    kprintf("Results: %d/%d tests passed\n", passed, total);
}

void test_system_deck_tag_management(void) {
    kprintf("\n");
    kprintf("SYSTEM DECK TAG MANAGEMENT TEST\n");

    int passed = 0;
    int total = 0;

    process_t* target_proc = process_create("app");
    process_t* system_proc = process_create("system");

    if (!target_proc || !system_proc) {
        debug_printf("[TEST] ERROR: Cannot create test processes\n");
        return;
    }

    total++;
    kprintf("\n[TEST %d] TAG_CHECK for existing tag\n", total);
    Event check_event;
    event_init(&check_event, system_proc->pid, 1);
    check_event.prefixes[0] = 0xFF22;
    check_event.prefix_count = 1;

    tag_check_event_t* check_data = (tag_check_event_t*)check_event.data;
    check_data->target_pid = target_proc->pid;
    strncpy(check_data->tag, "app", sizeof(check_data->tag) - 1);

    int result = system_deck_tag_check(&check_event);
    if (result == 0 && check_event.state == EVENT_STATE_COMPLETED) {
        tag_check_response_t* check_response = (tag_check_response_t*)check_event.data;
        if (check_response->has_tag) {
            debug_printf("[TEST %d] PASS: Correctly found existing tag\n", total);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: Tag should exist\n", total);
        }
    } else {
        debug_printf("[TEST %d] FAIL: TAG_CHECK failed\n", total);
    }

    total++;
    kprintf("\n[TEST %d] TAG_CHECK for non-existing tag\n", total);
    Event check_event2;
    event_init(&check_event2, system_proc->pid, 2);
    check_event2.prefixes[0] = 0xFF22;
    check_event2.prefix_count = 1;

    tag_check_event_t* check_data2 = (tag_check_event_t*)check_event2.data;
    check_data2->target_pid = target_proc->pid;
    strncpy(check_data2->tag, "nonexistent", sizeof(check_data2->tag) - 1);

    result = system_deck_tag_check(&check_event2);
    if (result == 0 && check_event2.state == EVENT_STATE_COMPLETED) {
        tag_check_response_t* check_response2 = (tag_check_response_t*)check_event2.data;
        if (!check_response2->has_tag) {
            debug_printf("[TEST %d] PASS: Correctly reported tag not found\n", total);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: Tag should not exist\n", total);
        }
    } else {
        debug_printf("[TEST %d] FAIL: TAG_CHECK failed\n", total);
    }

    total++;
    kprintf("\n[TEST %d] TAG_ADD new tag\n", total);
    Event add_event;
    event_init(&add_event, system_proc->pid, 3);
    add_event.prefixes[0] = 0xFF20;
    add_event.prefix_count = 1;

    tag_modify_event_t* add_data = (tag_modify_event_t*)add_event.data;
    add_data->target_pid = target_proc->pid;
    strncpy(add_data->tag, "test_tag", sizeof(add_data->tag) - 1);

    result = system_deck_tag_add(&add_event);
    if (result == 0 && add_event.state == EVENT_STATE_COMPLETED) {
        if (process_has_tag(target_proc, "test_tag")) {
            debug_printf("[TEST %d] PASS: Tag added successfully\n", total);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: Tag not found after adding\n", total);
        }
    } else {
        debug_printf("[TEST %d] FAIL: TAG_ADD failed\n", total);
    }

    total++;
    kprintf("\n[TEST %d] TAG_ADD duplicate tag (should fail)\n", total);
    Event add_dup_event;
    event_init(&add_dup_event, system_proc->pid, 4);
    add_dup_event.prefixes[0] = 0xFF20;
    add_dup_event.prefix_count = 1;

    tag_modify_event_t* add_dup_data = (tag_modify_event_t*)add_dup_event.data;
    add_dup_data->target_pid = target_proc->pid;
    strncpy(add_dup_data->tag, "test_tag", sizeof(add_dup_data->tag) - 1);

    result = system_deck_tag_add(&add_dup_event);
    if (result != 0 && add_dup_event.state == EVENT_STATE_ERROR) {
        tag_modify_response_t* dup_response = (tag_modify_response_t*)add_dup_event.data;
        if (dup_response->error_code == SYSTEM_ERR_TAG_EXISTS) {
            debug_printf("[TEST %d] PASS: Duplicate tag correctly rejected\n", total);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: Wrong error code\n", total);
        }
    } else {
        debug_printf("[TEST %d] FAIL: Duplicate tag should be rejected\n", total);
    }

    total++;
    kprintf("\n[TEST %d] TAG_REMOVE existing tag\n", total);
    Event remove_event;
    event_init(&remove_event, system_proc->pid, 5);
    remove_event.prefixes[0] = 0xFF21;
    remove_event.prefix_count = 1;

    tag_modify_event_t* remove_data = (tag_modify_event_t*)remove_event.data;
    remove_data->target_pid = target_proc->pid;
    strncpy(remove_data->tag, "test_tag", sizeof(remove_data->tag) - 1);

    result = system_deck_tag_remove(&remove_event);
    if (result == 0 && remove_event.state == EVENT_STATE_COMPLETED) {
        if (!process_has_tag(target_proc, "test_tag")) {
            debug_printf("[TEST %d] PASS: Tag removed successfully\n", total);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: Tag still present after removal\n", total);
        }
    } else {
        debug_printf("[TEST %d] FAIL: TAG_REMOVE failed\n", total);
    }

    total++;
    kprintf("\n[TEST %d] TAG_REMOVE non-existing tag (should fail)\n", total);
    Event remove_bad_event;
    event_init(&remove_bad_event, system_proc->pid, 6);
    remove_bad_event.prefixes[0] = 0xFF21;
    remove_bad_event.prefix_count = 1;

    tag_modify_event_t* remove_bad_data = (tag_modify_event_t*)remove_bad_event.data;
    remove_bad_data->target_pid = target_proc->pid;
    strncpy(remove_bad_data->tag, "nonexistent", sizeof(remove_bad_data->tag) - 1);

    result = system_deck_tag_remove(&remove_bad_event);
    if (result != 0 && remove_bad_event.state == EVENT_STATE_ERROR) {
        tag_modify_response_t* bad_response = (tag_modify_response_t*)remove_bad_event.data;
        if (bad_response->error_code == SYSTEM_ERR_TAG_NOT_FOUND) {
            debug_printf("[TEST %d] PASS: Non-existing tag correctly rejected\n", total);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: Wrong error code\n", total);
        }
    } else {
        debug_printf("[TEST %d] FAIL: Non-existing tag removal should be rejected\n", total);
    }

    total++;
    kprintf("\n[TEST %d] TAG_ADD with invalid PID (should fail)\n", total);
    Event add_bad_pid_event;
    event_init(&add_bad_pid_event, system_proc->pid, 7);
    add_bad_pid_event.prefixes[0] = 0xFF20;
    add_bad_pid_event.prefix_count = 1;

    tag_modify_event_t* add_bad_pid_data = (tag_modify_event_t*)add_bad_pid_event.data;
    add_bad_pid_data->target_pid = 99999;
    strncpy(add_bad_pid_data->tag, "test", sizeof(add_bad_pid_data->tag) - 1);

    result = system_deck_tag_add(&add_bad_pid_event);
    if (result != 0 && add_bad_pid_event.state == EVENT_STATE_ERROR) {
        debug_printf("[TEST %d] PASS: Invalid PID correctly rejected\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: Invalid PID should be rejected\n", total);
    }

    total++;
    kprintf("\n[TEST %d] TAG_CHECK with empty tag (should fail)\n", total);
    Event check_empty_event;
    event_init(&check_empty_event, system_proc->pid, 8);
    check_empty_event.prefixes[0] = 0xFF22;
    check_empty_event.prefix_count = 1;

    tag_check_event_t* check_empty_data = (tag_check_event_t*)check_empty_event.data;
    check_empty_data->target_pid = target_proc->pid;
    check_empty_data->tag[0] = '\0';

    result = system_deck_tag_check(&check_empty_event);
    if (result != 0 && check_empty_event.state == EVENT_STATE_ERROR) {
        debug_printf("[TEST %d] PASS: Empty tag correctly rejected\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: Empty tag should be rejected\n", total);
    }

    process_destroy(target_proc);
    process_destroy(system_proc);

    kprintf("\n");
    kprintf("TAG MANAGEMENT TEST COMPLETE\n");
    kprintf("Results: %d/%d tests passed\n", passed, total);
}

void test_system_deck_buffer_management(void) {
    kprintf("\n");
    kprintf("SYSTEM DECK BUFFER MANAGEMENT TEST\n");

    int passed = 0;
    int total = 0;

    process_t* test_proc = process_create("app");
    if (!test_proc) {
        debug_printf("[TEST] ERROR: Cannot create test process\n");
        return;
    }

    total++;
    kprintf("\n[TEST %d] BUF_ALLOC with valid size\n", total);
    Event alloc_event;
    event_init(&alloc_event, test_proc->pid, 1);
    alloc_event.prefixes[0] = 0xFF10;
    alloc_event.prefix_count = 1;

    buf_alloc_event_t* alloc_data = (buf_alloc_event_t*)alloc_event.data;
    alloc_data->size = 8192;
    alloc_data->flags = 0;

    int result = system_deck_buf_alloc(&alloc_event);
    uint64_t buffer_handle = 0;
    if (result == 0 && alloc_event.state == EVENT_STATE_COMPLETED) {
        buf_alloc_response_t* alloc_response = (buf_alloc_response_t*)alloc_event.data;
        buffer_handle = alloc_response->buffer_handle;
        if (buffer_handle > 0 && alloc_response->phys_addr > 0) {
            debug_printf("[TEST %d] PASS: Buffer allocated (handle=%lu, phys=0x%lx, size=%lu)\n",
                    total, buffer_handle, alloc_response->phys_addr, alloc_response->actual_size);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: Invalid buffer handle or address\n", total);
        }
    } else {
        debug_printf("[TEST %d] FAIL: BUF_ALLOC failed\n", total);
    }

    total++;
    kprintf("\n[TEST %d] BUF_ALLOC with zero size (should fail)\n", total);
    Event alloc_zero_event;
    event_init(&alloc_zero_event, test_proc->pid, 2);
    alloc_zero_event.prefixes[0] = 0xFF10;
    alloc_zero_event.prefix_count = 1;

    buf_alloc_event_t* alloc_zero_data = (buf_alloc_event_t*)alloc_zero_event.data;
    alloc_zero_data->size = 0;
    alloc_zero_data->flags = 0;

    result = system_deck_buf_alloc(&alloc_zero_event);
    if (result != 0 && alloc_zero_event.state == EVENT_STATE_ERROR) {
        debug_printf("[TEST %d] PASS: Zero size correctly rejected\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: Zero size should be rejected\n", total);
    }

    total++;
    kprintf("\n[TEST %d] BUF_ALLOC with excessive size (should fail)\n", total);
    Event alloc_big_event;
    event_init(&alloc_big_event, test_proc->pid, 3);
    alloc_big_event.prefixes[0] = 0xFF10;
    alloc_big_event.prefix_count = 1;

    buf_alloc_event_t* alloc_big_data = (buf_alloc_event_t*)alloc_big_event.data;
    alloc_big_data->size = BUF_MAX_SIZE + 1;
    alloc_big_data->flags = 0;

    result = system_deck_buf_alloc(&alloc_big_event);
    if (result != 0 && alloc_big_event.state == EVENT_STATE_ERROR) {
        debug_printf("[TEST %d] PASS: Excessive size correctly rejected\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: Excessive size should be rejected\n", total);
    }

    if (buffer_handle > 0) {
        total++;
        kprintf("\n[TEST %d] BUF_FREE with valid handle\n", total);
        Event free_event;
        event_init(&free_event, test_proc->pid, 4);
        free_event.prefixes[0] = 0xFF11;
        free_event.prefix_count = 1;

        buf_free_event_t* free_data = (buf_free_event_t*)free_event.data;
        free_data->buffer_handle = buffer_handle;

        result = system_deck_buf_free(&free_event);
        if (result == 0 && free_event.state == EVENT_STATE_COMPLETED) {
            debug_printf("[TEST %d] PASS: Buffer freed successfully\n", total);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: BUF_FREE failed\n", total);
        }

        total++;
        kprintf("\n[TEST %d] BUF_FREE with same handle (should fail)\n", total);
        Event free_dup_event;
        event_init(&free_dup_event, test_proc->pid, 5);
        free_dup_event.prefixes[0] = 0xFF11;
        free_dup_event.prefix_count = 1;

        buf_free_event_t* free_dup_data = (buf_free_event_t*)free_dup_event.data;
        free_dup_data->buffer_handle = buffer_handle;

        result = system_deck_buf_free(&free_dup_event);
        if (result != 0 && free_dup_event.state == EVENT_STATE_ERROR) {
            debug_printf("[TEST %d] PASS: Double free correctly rejected\n", total);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: Double free should be rejected\n", total);
        }
    }

    total++;
    kprintf("\n[TEST %d] BUF_FREE with invalid handle (should fail)\n", total);
    Event free_bad_event;
    event_init(&free_bad_event, test_proc->pid, 6);
    free_bad_event.prefixes[0] = 0xFF11;
    free_bad_event.prefix_count = 1;

    buf_free_event_t* free_bad_data = (buf_free_event_t*)free_bad_event.data;
    free_bad_data->buffer_handle = 99999;

    result = system_deck_buf_free(&free_bad_event);
    if (result != 0 && free_bad_event.state == EVENT_STATE_ERROR) {
        debug_printf("[TEST %d] PASS: Invalid handle correctly rejected\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: Invalid handle should be rejected\n", total);
    }

    total++;
    kprintf("\n[TEST %d] BUF_RESIZE (stub - should return not implemented)\n", total);
    Event resize_event;
    event_init(&resize_event, test_proc->pid, 7);
    resize_event.prefixes[0] = 0xFF12;
    resize_event.prefix_count = 1;

    buf_resize_event_t* resize_data = (buf_resize_event_t*)resize_event.data;
    resize_data->buffer_handle = 1;
    resize_data->new_size = 16384;

    result = system_deck_buf_resize(&resize_event);
    if (result != 0 && resize_event.state == EVENT_STATE_ERROR) {
        buf_resize_response_t* resize_response = (buf_resize_response_t*)resize_event.data;
        if (resize_response->error_code == SYSTEM_ERR_NOT_IMPLEMENTED) {
            debug_printf("[TEST %d] PASS: BUF_RESIZE correctly returns not implemented\n", total);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: Wrong error code\n", total);
        }
    } else {
        debug_printf("[TEST %d] FAIL: BUF_RESIZE should return error\n", total);
    }

    total++;
    kprintf("\n[TEST %d] Multiple buffer allocation test\n", total);
    uint64_t handles[5];
    int alloc_count = 0;
    for (int i = 0; i < 5; i++) {
        Event multi_alloc_event;
        event_init(&multi_alloc_event, test_proc->pid, 100 + i);
        multi_alloc_event.prefixes[0] = 0xFF10;
        multi_alloc_event.prefix_count = 1;

        buf_alloc_event_t* multi_alloc_data = (buf_alloc_event_t*)multi_alloc_event.data;
        multi_alloc_data->size = 4096;
        multi_alloc_data->flags = 0;

        result = system_deck_buf_alloc(&multi_alloc_event);
        if (result == 0 && multi_alloc_event.state == EVENT_STATE_COMPLETED) {
            buf_alloc_response_t* multi_response = (buf_alloc_response_t*)multi_alloc_event.data;
            handles[i] = multi_response->buffer_handle;
            alloc_count++;
        }
    }

    if (alloc_count == 5) {
        debug_printf("[TEST %d] PASS: Multiple buffers allocated (%d/5)\n", total, alloc_count);
        passed++;

        for (int i = 0; i < 5; i++) {
            Event multi_free_event;
            event_init(&multi_free_event, test_proc->pid, 200 + i);
            multi_free_event.prefixes[0] = 0xFF11;
            multi_free_event.prefix_count = 1;

            buf_free_event_t* multi_free_data = (buf_free_event_t*)multi_free_event.data;
            multi_free_data->buffer_handle = handles[i];

            system_deck_buf_free(&multi_free_event);
        }
    } else {
        debug_printf("[TEST %d] FAIL: Could not allocate all buffers (%d/5)\n", total, alloc_count);
    }

    process_destroy(test_proc);

    kprintf("\n");
    kprintf("BUFFER MANAGEMENT TEST COMPLETE\n");
    kprintf("Results: %d/%d tests passed\n", passed, total);
}

void test_system_deck_ctx_use(void) {
    kprintf("\n");
    kprintf("SYSTEM DECK CTX_USE TEST\n");

    int passed = 0;
    int total = 0;

    process_t* test_proc = process_create("app,project:demo");
    process_t* other_proc = process_create("app,type:text");
    if (!test_proc || !other_proc) {
        debug_printf("[TEST] ERROR: Cannot create test processes\n");
        return;
    }

    total++;
    kprintf("\n[TEST %d] CTX_USE with single tag\n", total);
    Event ctx_event;
    event_init(&ctx_event, 1, 1);
    ctx_event.prefixes[0] = 0xFF04;
    ctx_event.prefix_count = 1;

    ctx_use_event_t* ctx_data = (ctx_use_event_t*)ctx_event.data;
    strncpy(ctx_data->context_string, "project:demo", sizeof(ctx_data->context_string) - 1);

    int result = system_deck_ctx_use(&ctx_event);
    if (result == 0 && ctx_event.state == EVENT_STATE_COMPLETED) {
        ctx_use_response_t* ctx_response = (ctx_use_response_t*)ctx_event.data;
        if (ctx_response->error_code == CTX_USE_ERR_SUCCESS) {
            if (scheduler_matches_use_context(test_proc)) {
                debug_printf("[TEST %d] PASS: Context set and process matches\n", total);
                kprintf("         Response: %s\n", ctx_response->message);
                passed++;
            } else {
                debug_printf("[TEST %d] FAIL: Process should match context\n", total);
            }
        } else {
            debug_printf("[TEST %d] FAIL: Unexpected error code\n", total);
        }
    } else {
        debug_printf("[TEST %d] FAIL: CTX_USE failed\n", total);
    }

    total++;
    kprintf("\n[TEST %d] CTX_USE clear context (empty string)\n", total);
    Event clear_event;
    event_init(&clear_event, 1, 2);
    clear_event.prefixes[0] = 0xFF04;
    clear_event.prefix_count = 1;

    ctx_use_event_t* clear_data = (ctx_use_event_t*)clear_event.data;
    clear_data->context_string[0] = '\0';

    result = system_deck_ctx_use(&clear_event);
    if (result == 0 && clear_event.state == EVENT_STATE_COMPLETED) {
        if (!scheduler_matches_use_context(test_proc)) {
            debug_printf("[TEST %d] PASS: Context cleared, no matches\n", total);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: Context should be cleared\n", total);
        }
    } else {
        debug_printf("[TEST %d] FAIL: CTX_USE clear failed\n", total);
    }

    total++;
    kprintf("\n[TEST %d] CTX_USE with multiple tags (2 tags)\n", total);
    Event multi_event;
    event_init(&multi_event, 1, 3);
    multi_event.prefixes[0] = 0xFF04;
    multi_event.prefix_count = 1;

    ctx_use_event_t* multi_data = (ctx_use_event_t*)multi_event.data;
    strncpy(multi_data->context_string, "project:demo,type:text", sizeof(multi_data->context_string) - 1);

    result = system_deck_ctx_use(&multi_event);
    if (result == 0 && multi_event.state == EVENT_STATE_COMPLETED) {
        if (scheduler_matches_use_context(test_proc) && scheduler_matches_use_context(other_proc)) {
            debug_printf("[TEST %d] PASS: Multiple tags set, both processes match\n", total);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: Both processes should match\n", total);
        }
    } else {
        debug_printf("[TEST %d] FAIL: CTX_USE multi-tag failed\n", total);
    }

    total++;
    kprintf("\n[TEST %d] CTX_USE with too many tags (should fail)\n", total);
    Event overflow_event;
    event_init(&overflow_event, 1, 4);
    overflow_event.prefixes[0] = 0xFF04;
    overflow_event.prefix_count = 1;

    ctx_use_event_t* overflow_data = (ctx_use_event_t*)overflow_event.data;
    strncpy(overflow_data->context_string, "tag1,tag2,tag3,tag4", sizeof(overflow_data->context_string) - 1);

    result = system_deck_ctx_use(&overflow_event);
    if (result != 0 && overflow_event.state == EVENT_STATE_ERROR) {
        ctx_use_response_t* overflow_response = (ctx_use_response_t*)overflow_event.data;
        if (overflow_response->error_code == CTX_USE_ERR_TOO_MANY_TAGS) {
            debug_printf("[TEST %d] PASS: Too many tags correctly rejected\n", total);
            kprintf("         Error: %s\n", overflow_response->message);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: Wrong error code\n", total);
        }
    } else {
        debug_printf("[TEST %d] FAIL: Should reject too many tags\n", total);
    }

    total++;
    kprintf("\n[TEST %d] CTX_USE scheduler bonus verification\n", total);
    Event bonus_event;
    event_init(&bonus_event, 1, 5);
    bonus_event.prefixes[0] = 0xFF04;
    bonus_event.prefix_count = 1;

    ctx_use_event_t* bonus_data = (ctx_use_event_t*)bonus_event.data;
    strncpy(bonus_data->context_string, "project:demo", sizeof(bonus_data->context_string) - 1);

    result = system_deck_ctx_use(&bonus_event);
    if (result == 0) {
        test_proc->state = PROC_READY;
        other_proc->state = PROC_READY;

        extern int32_t scheduler_calculate_score(process_t* proc);
        int32_t score_match = scheduler_calculate_score(test_proc);
        int32_t score_no_match = scheduler_calculate_score(other_proc);

        if (score_match >= score_no_match + 20) {
            debug_printf("[TEST %d] PASS: Scheduler +20 bonus applied (match=%d, no_match=%d)\n",
                    total, score_match, score_no_match);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: Bonus not applied correctly (match=%d, no_match=%d)\n",
                    total, score_match, score_no_match);
        }
    } else {
        debug_printf("[TEST %d] FAIL: CTX_USE setup failed\n", total);
    }

    scheduler_clear_use_context();
    process_destroy(test_proc);
    process_destroy(other_proc);

    kprintf("\n");
    kprintf("CTX_USE TEST COMPLETE\n");
    kprintf("Results: %d/%d tests passed\n", passed, total);
}

#else

void test_system_deck_security(void) { (void)0; }
void test_system_deck_process_management(void) { (void)0; }
void test_system_deck_tag_management(void) { (void)0; }
void test_system_deck_buffer_management(void) { (void)0; }
void test_system_deck_ctx_use(void) { (void)0; }

#endif
