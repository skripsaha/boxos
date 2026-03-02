#include "route_test.h"
#include "system_deck.h"
#include "system_deck_route.h"
#include "guide.h"
#include "process.h"
#include "klib.h"
#include "error.h"
#include "events.h"
#include "result_page.h"
#include "vmm.h"
#include "listen_table.h"
#include "kernel_config.h"

#if CONFIG_RUN_STARTUP_TESTS

void test_listen_table(void) {
    kprintf("\n");
    kprintf("LISTEN TABLE UNIT TEST\n");

    int passed = 0;
    int total = 0;

    listen_table_init();

    total++;
    kprintf("\n[TEST %d] Add listener (PID=10, KEYBOARD)\n", total);
    int result = listen_table_add(10, LISTEN_KEYBOARD, 0);
    if (result == 0) {
        debug_printf("[TEST %d] PASS: Listener added successfully\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: listen_table_add returned %d\n", total, result);
    }

    total++;
    kprintf("\n[TEST %d] Find listeners for KEYBOARD\n", total);
    uint32_t found_pids[8];
    uint32_t found = listen_table_find(LISTEN_KEYBOARD, found_pids, 8);
    if (found == 1 && found_pids[0] == 10) {
        debug_printf("[TEST %d] PASS: Found PID 10 for KEYBOARD\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: Expected 1 listener (PID 10), got %u\n", total, found);
    }

    total++;
    kprintf("\n[TEST %d] Add duplicate listener (PID=10, KEYBOARD)\n", total);
    result = listen_table_add(10, LISTEN_KEYBOARD, 0);
    if (result == -ERR_LISTEN_ALREADY) {
        debug_printf("[TEST %d] PASS: Duplicate correctly rejected (err=%d)\n", total, result);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: Expected -ERR_LISTEN_ALREADY, got %d\n", total, result);
    }

    total++;
    kprintf("\n[TEST %d] Add listeners for different source types\n", total);
    int r1 = listen_table_add(10, LISTEN_MOUSE, 0);
    int r2 = listen_table_add(20, LISTEN_KEYBOARD, LISTEN_FLAG_EXCLUSIVE);
    int r3 = listen_table_add(30, LISTEN_NETWORK, 0);
    if (r1 == 0 && r2 == 0 && r3 == 0) {
        debug_printf("[TEST %d] PASS: Added 3 more listeners\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: Some adds failed (%d, %d, %d)\n", total, r1, r2, r3);
    }

    total++;
    kprintf("\n[TEST %d] Find all KEYBOARD listeners\n", total);
    found = listen_table_find(LISTEN_KEYBOARD, found_pids, 8);
    if (found == 2) {
        debug_printf("[TEST %d] PASS: Found %u KEYBOARD listeners\n", total, found);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: Expected 2 KEYBOARD listeners, got %u\n", total, found);
    }

    total++;
    kprintf("\n[TEST %d] Remove listener (PID=10, KEYBOARD)\n", total);
    result = listen_table_remove(10, LISTEN_KEYBOARD);
    if (result == 0) {
        found = listen_table_find(LISTEN_KEYBOARD, found_pids, 8);
        if (found == 1 && found_pids[0] == 20) {
            debug_printf("[TEST %d] PASS: Listener removed, only PID 20 remains\n", total);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: After removal, unexpected state (found=%u)\n", total, found);
        }
    } else {
        debug_printf("[TEST %d] FAIL: listen_table_remove returned %d\n", total, result);
    }

    total++;
    kprintf("\n[TEST %d] Remove non-existent listener (PID=99, KEYBOARD)\n", total);
    result = listen_table_remove(99, LISTEN_KEYBOARD);
    if (result == -1) {
        debug_printf("[TEST %d] PASS: Non-existent removal returns -1\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: Expected -1, got %d\n", total, result);
    }

    total++;
    kprintf("\n[TEST %d] cleanup_pid for PID=10 (has MOUSE entry)\n", total);
    listen_table_add(10, LISTEN_NETWORK, 0);
    listen_table_remove_pid(10);
    uint32_t mouse_found = listen_table_find(LISTEN_MOUSE, found_pids, 8);
    uint32_t net_pids[8];
    uint32_t net_found = listen_table_find(LISTEN_NETWORK, net_pids, 8);
    bool pid10_in_mouse = false;
    for (uint32_t i = 0; i < mouse_found; i++) {
        if (found_pids[i] == 10) pid10_in_mouse = true;
    }
    bool pid10_in_net = false;
    for (uint32_t i = 0; i < net_found; i++) {
        if (net_pids[i] == 10) pid10_in_net = true;
    }
    if (!pid10_in_mouse && !pid10_in_net) {
        debug_printf("[TEST %d] PASS: All entries for PID 10 cleaned up\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: PID 10 still found after cleanup\n", total);
    }

    total++;
    kprintf("\n[TEST %d] Fill table to MAX_LISTENERS, then overflow\n", total);
    listen_table_init();
    bool fill_ok = true;
    for (uint32_t i = 0; i < MAX_LISTENERS; i++) {
        result = listen_table_add(1000 + i, (uint8_t)(i % 3), 0);
        if (result != 0) {
            debug_printf("[TEST %d] FAIL: Could not add entry %u (err=%d)\n", total, i, result);
            fill_ok = false;
            break;
        }
    }
    if (fill_ok) {
        result = listen_table_add(9999, LISTEN_KEYBOARD, 0);
        if (result == -ERR_LISTEN_TABLE_FULL) {
            debug_printf("[TEST %d] PASS: Table full correctly reported\n", total);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: Expected -ERR_LISTEN_TABLE_FULL, got %d\n", total, result);
        }
    }

    listen_table_init();

    kprintf("\n");
    kprintf("LISTEN TABLE TEST COMPLETE\n");
    kprintf("Results: %d/%d tests passed\n", passed, total);
}

void test_route_direct(void) {
    kprintf("\n");
    kprintf("ROUTE DIRECT (PID-to-PID) TEST\n");

    int passed = 0;
    int total = 0;

    process_t* proc_a = process_create("app");
    process_t* proc_b = process_create("app");

    if (!proc_a || !proc_b) {
        debug_printf("[TEST] ERROR: Cannot create test processes\n");
        return;
    }

    debug_printf("[TEST] proc_a PID=%u, proc_b PID=%u\n", proc_a->pid, proc_b->pid);

    total++;
    kprintf("\n[TEST %d] Route A -> B (valid direct route)\n", total);
    {
        Event event;
        event_init(&event, proc_a->pid, 100);
        event.prefixes[0] = 0xFF40;
        event.prefix_count = 1;
        event.current_prefix_idx = 0;
        event.route_target = proc_b->pid;

        int result = system_deck_route(&event);
        if (result == 0 &&
            event.state == EVENT_STATE_PROCESSING &&
            event.sender_pid == proc_a->pid) {
            debug_printf("[TEST %d] PASS: Route succeeded, state=PROCESSING, sender=%u\n",
                    total, event.sender_pid);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: result=%d state=%u sender=%u\n",
                    total, result, event.state, event.sender_pid);
        }
    }

    total++;
    kprintf("\n[TEST %d] Route to self (should fail ROUTE_SELF)\n", total);
    {
        Event event;
        event_init(&event, proc_a->pid, 101);
        event.prefixes[0] = 0xFF40;
        event.prefix_count = 1;
        event.current_prefix_idx = 0;
        event.route_target = proc_a->pid;

        int result = system_deck_route(&event);
        if (result == -1 && event.error_code == ERR_ROUTE_SELF) {
            debug_printf("[TEST %d] PASS: Route to self correctly rejected\n", total);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: Expected ROUTE_SELF error, got result=%d err=%u\n",
                    total, result, event.error_code);
        }
    }

    total++;
    kprintf("\n[TEST %d] Route to target=0 (should fail INVALID_ARGUMENT)\n", total);
    {
        Event event;
        event_init(&event, proc_a->pid, 102);
        event.prefixes[0] = 0xFF40;
        event.prefix_count = 1;
        event.current_prefix_idx = 0;
        event.route_target = 0;

        int result = system_deck_route(&event);
        if (result == -1 && event.error_code == ERR_INVALID_ARGUMENT) {
            debug_printf("[TEST %d] PASS: Zero target correctly rejected\n", total);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: Expected INVALID_ARGUMENT, got result=%d err=%u\n",
                    total, result, event.error_code);
        }
    }

    total++;
    kprintf("\n[TEST %d] Route to non-existent PID (should fail PROCESS_NOT_FOUND)\n", total);
    {
        Event event;
        event_init(&event, proc_a->pid, 103);
        event.prefixes[0] = 0xFF40;
        event.prefix_count = 1;
        event.current_prefix_idx = 0;
        event.route_target = 99999;

        int result = system_deck_route(&event);
        if (result == -1 && event.error_code == ERR_PROCESS_NOT_FOUND) {
            debug_printf("[TEST %d] PASS: Non-existent PID correctly rejected\n", total);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: Expected PROCESS_NOT_FOUND, got result=%d err=%u\n",
                    total, result, event.error_code);
        }
    }

    process_destroy(proc_a);
    process_destroy(proc_b);

    kprintf("\n");
    kprintf("ROUTE DIRECT TEST COMPLETE\n");
    kprintf("Results: %d/%d tests passed\n", passed, total);
}

void test_tag_wildcard(void) {
    kprintf("\n");
    kprintf("TAG WILDCARD UNIT TEST\n");

    int passed = 0;
    int total = 0;

    // tag_is_wildcard tests
    total++;
    kprintf("\n[TEST %d] tag_is_wildcard: 'type:...' = true\n", total);
    if (tag_is_wildcard("type:...")) { passed++; debug_printf("[TEST %d] PASS\n", total); }
    else { debug_printf("[TEST %d] FAIL\n", total); }

    total++;
    kprintf("\n[TEST %d] tag_is_wildcard: '...:note' = true\n", total);
    if (tag_is_wildcard("...:note")) { passed++; debug_printf("[TEST %d] PASS\n", total); }
    else { debug_printf("[TEST %d] FAIL\n", total); }

    total++;
    kprintf("\n[TEST %d] tag_is_wildcard: '...:...' = true\n", total);
    if (tag_is_wildcard("...:...")) { passed++; debug_printf("[TEST %d] PASS\n", total); }
    else { debug_printf("[TEST %d] FAIL\n", total); }

    total++;
    kprintf("\n[TEST %d] tag_is_wildcard: 'type:editor' = false\n", total);
    if (!tag_is_wildcard("type:editor")) { passed++; debug_printf("[TEST %d] PASS\n", total); }
    else { debug_printf("[TEST %d] FAIL\n", total); }

    total++;
    kprintf("\n[TEST %d] tag_is_wildcard: 'app' = false\n", total);
    if (!tag_is_wildcard("app")) { passed++; debug_printf("[TEST %d] PASS\n", total); }
    else { debug_printf("[TEST %d] FAIL\n", total); }

    // tag_match tests
    total++;
    kprintf("\n[TEST %d] tag_match: 'type:...' vs 'type:editor' = true\n", total);
    if (tag_match("type:...", "type:editor")) { passed++; debug_printf("[TEST %d] PASS\n", total); }
    else { debug_printf("[TEST %d] FAIL\n", total); }

    total++;
    kprintf("\n[TEST %d] tag_match: 'type:...' vs 'type:viewer' = true\n", total);
    if (tag_match("type:...", "type:viewer")) { passed++; debug_printf("[TEST %d] PASS\n", total); }
    else { debug_printf("[TEST %d] FAIL\n", total); }

    total++;
    kprintf("\n[TEST %d] tag_match: 'type:...' vs 'lang:c' = false\n", total);
    if (!tag_match("type:...", "lang:c")) { passed++; debug_printf("[TEST %d] PASS\n", total); }
    else { debug_printf("[TEST %d] FAIL\n", total); }

    total++;
    kprintf("\n[TEST %d] tag_match: '...:editor' vs 'type:editor' = true\n", total);
    if (tag_match("...:editor", "type:editor")) { passed++; debug_printf("[TEST %d] PASS\n", total); }
    else { debug_printf("[TEST %d] FAIL\n", total); }

    total++;
    kprintf("\n[TEST %d] tag_match: '...:editor' vs 'type:viewer' = false\n", total);
    if (!tag_match("...:editor", "type:viewer")) { passed++; debug_printf("[TEST %d] PASS\n", total); }
    else { debug_printf("[TEST %d] FAIL\n", total); }

    total++;
    kprintf("\n[TEST %d] tag_match: '...:...' vs 'type:editor' = true\n", total);
    if (tag_match("...:...", "type:editor")) { passed++; debug_printf("[TEST %d] PASS\n", total); }
    else { debug_printf("[TEST %d] FAIL\n", total); }

    total++;
    kprintf("\n[TEST %d] tag_match: '...:...' vs 'app' = false (no colon)\n", total);
    if (!tag_match("...:...", "app")) { passed++; debug_printf("[TEST %d] PASS\n", total); }
    else { debug_printf("[TEST %d] FAIL\n", total); }

    total++;
    kprintf("\n[TEST %d] tag_match: 'app' vs 'app' = true (exact)\n", total);
    if (tag_match("app", "app")) { passed++; debug_printf("[TEST %d] PASS\n", total); }
    else { debug_printf("[TEST %d] FAIL\n", total); }

    total++;
    kprintf("\n[TEST %d] tag_match: 'type:editor' vs 'type:editor' = true (exact)\n", total);
    if (tag_match("type:editor", "type:editor")) { passed++; debug_printf("[TEST %d] PASS\n", total); }
    else { debug_printf("[TEST %d] FAIL\n", total); }

    // process_has_tag wildcard tests
    process_t* proc = process_create("app,type:editor,lang:c");
    if (proc) {
        total++;
        kprintf("\n[TEST %d] process_has_tag: 'type:...' on 'app,type:editor,lang:c'\n", total);
        if (process_has_tag(proc, "type:...")) { passed++; debug_printf("[TEST %d] PASS\n", total); }
        else { debug_printf("[TEST %d] FAIL\n", total); }

        total++;
        kprintf("\n[TEST %d] process_has_tag: '...:editor' on 'app,type:editor,lang:c'\n", total);
        if (process_has_tag(proc, "...:editor")) { passed++; debug_printf("[TEST %d] PASS\n", total); }
        else { debug_printf("[TEST %d] FAIL\n", total); }

        total++;
        kprintf("\n[TEST %d] process_has_tag: '...:...' on 'app,type:editor,lang:c'\n", total);
        if (process_has_tag(proc, "...:...")) { passed++; debug_printf("[TEST %d] PASS\n", total); }
        else { debug_printf("[TEST %d] FAIL\n", total); }

        total++;
        kprintf("\n[TEST %d] process_has_tag: '...:python' (no match)\n", total);
        if (!process_has_tag(proc, "...:python")) { passed++; debug_printf("[TEST %d] PASS\n", total); }
        else { debug_printf("[TEST %d] FAIL\n", total); }

        process_destroy(proc);
    }

    kprintf("\n");
    kprintf("TAG WILDCARD TEST COMPLETE\n");
    kprintf("Results: %d/%d tests passed\n", passed, total);
}

void test_route_tag(void) {
    kprintf("\n");
    kprintf("ROUTE TAG (MULTICAST) TEST\n");

    int passed = 0;
    int total = 0;

    process_t* proc_a = process_create("app");
    process_t* proc_b = process_create("app,type:editor");
    process_t* proc_c = process_create("app,type:viewer");

    if (!proc_a || !proc_b || !proc_c) {
        debug_printf("[TEST] ERROR: Cannot create test processes\n");
        if (proc_a) process_destroy(proc_a);
        if (proc_b) process_destroy(proc_b);
        if (proc_c) process_destroy(proc_c);
        return;
    }

    debug_printf("[TEST] proc_a PID=%u, proc_b PID=%u, proc_c PID=%u\n",
            proc_a->pid, proc_b->pid, proc_c->pid);

    process_set_state(proc_a, PROC_WORKING);
    process_set_state(proc_b, PROC_WORKING);
    process_set_state(proc_c, PROC_WORKING);

    total++;
    kprintf("\n[TEST %d] Route by tag 'type:editor' (exact match)\n", total);
    {
        Event event;
        event_init(&event, proc_a->pid, 200);
        event.prefixes[0] = 0xFF41;
        event.prefix_count = 1;
        event.current_prefix_idx = 0;
        strncpy(event.route_tag, "type:editor", sizeof(event.route_tag) - 1);

        int result = system_deck_route_tag(&event);
        if (result == 0 && event.state != EVENT_STATE_ERROR) {
            debug_printf("[TEST %d] PASS: Tag route succeeded (state=%u)\n", total, event.state);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: Tag route failed, result=%d state=%u err=%u\n",
                    total, result, event.state, event.error_code);
        }
    }

    total++;
    kprintf("\n[TEST %d] Route by tag 'type:...' (wildcard match)\n", total);
    {
        Event event;
        event_init(&event, proc_a->pid, 201);
        event.prefixes[0] = 0xFF41;
        event.prefix_count = 1;
        event.current_prefix_idx = 0;
        strncpy(event.route_tag, "type:...", sizeof(event.route_tag) - 1);

        int result = system_deck_route_tag(&event);
        if (result == 0 && event.state != EVENT_STATE_ERROR) {
            debug_printf("[TEST %d] PASS: Wildcard tag route succeeded\n", total);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: Wildcard tag route failed, result=%d err=%u\n",
                    total, result, event.error_code);
        }
    }

    total++;
    kprintf("\n[TEST %d] Route by tag '...:editor' (key wildcard)\n", total);
    {
        Event event;
        event_init(&event, proc_a->pid, 204);
        event.prefixes[0] = 0xFF41;
        event.prefix_count = 1;
        event.current_prefix_idx = 0;
        strncpy(event.route_tag, "...:editor", sizeof(event.route_tag) - 1);

        int result = system_deck_route_tag(&event);
        if (result == 0 && event.state != EVENT_STATE_ERROR) {
            debug_printf("[TEST %d] PASS: Key wildcard route succeeded\n", total);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: Key wildcard route failed, result=%d err=%u\n",
                    total, result, event.error_code);
        }
    }

    total++;
    kprintf("\n[TEST %d] Route by tag '...:...' (full wildcard)\n", total);
    {
        Event event;
        event_init(&event, proc_a->pid, 205);
        event.prefixes[0] = 0xFF41;
        event.prefix_count = 1;
        event.current_prefix_idx = 0;
        strncpy(event.route_tag, "...:...", sizeof(event.route_tag) - 1);

        int result = system_deck_route_tag(&event);
        if (result == 0 && event.state != EVENT_STATE_ERROR) {
            debug_printf("[TEST %d] PASS: Full wildcard route succeeded\n", total);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: Full wildcard route failed, result=%d err=%u\n",
                    total, result, event.error_code);
        }
    }

    total++;
    kprintf("\n[TEST %d] Route by tag 'nonexistent' (no subscribers)\n", total);
    {
        Event event;
        event_init(&event, proc_a->pid, 202);
        event.prefixes[0] = 0xFF41;
        event.prefix_count = 1;
        event.current_prefix_idx = 0;
        strncpy(event.route_tag, "nonexistent", sizeof(event.route_tag) - 1);

        int result = system_deck_route_tag(&event);
        if (result == -1 && event.error_code == ERR_ROUTE_NO_SUBSCRIBERS) {
            debug_printf("[TEST %d] PASS: No subscribers correctly reported\n", total);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: Expected ROUTE_NO_SUBSCRIBERS, got result=%d err=%u\n",
                    total, result, event.error_code);
        }
    }

    total++;
    kprintf("\n[TEST %d] Route with empty tag (should fail INVALID_ARGUMENT)\n", total);
    {
        Event event;
        event_init(&event, proc_a->pid, 203);
        event.prefixes[0] = 0xFF41;
        event.prefix_count = 1;
        event.current_prefix_idx = 0;
        event.route_tag[0] = '\0';

        int result = system_deck_route_tag(&event);
        if (result == -1 && event.error_code == ERR_INVALID_ARGUMENT) {
            debug_printf("[TEST %d] PASS: Empty tag correctly rejected\n", total);
            passed++;
        } else {
            debug_printf("[TEST %d] FAIL: Expected INVALID_ARGUMENT, got result=%d err=%u\n",
                    total, result, event.error_code);
        }
    }

    process_destroy(proc_a);
    process_destroy(proc_b);
    process_destroy(proc_c);

    kprintf("\n");
    kprintf("ROUTE TAG TEST COMPLETE\n");
    kprintf("Results: %d/%d tests passed\n", passed, total);
}

void test_listen_via_system_deck(void) {
    kprintf("\n");
    kprintf("LISTEN VIA SYSTEM DECK TEST\n");

    int passed = 0;
    int total = 0;

    listen_table_init();

    process_t* proc = process_create("app");
    if (!proc) {
        debug_printf("[TEST] ERROR: Cannot create test process\n");
        return;
    }

    debug_printf("[TEST] proc PID=%u\n", proc->pid);

    total++;
    kprintf("\n[TEST %d] Register KEYBOARD listener via system_deck_listen\n", total);
    {
        Event event;
        event_init(&event, proc->pid, 300);
        event.prefixes[0] = 0xFF42;
        event.prefix_count = 1;
        event.current_prefix_idx = 0;
        event.data[0] = LISTEN_KEYBOARD;
        event.data[1] = 0;

        int result = system_deck_listen(&event);
        if (result == 0 && event.state == EVENT_STATE_COMPLETED) {
            uint32_t pids[4];
            uint32_t found = listen_table_find(LISTEN_KEYBOARD, pids, 4);
            if (found == 1 && pids[0] == proc->pid) {
                debug_printf("[TEST %d] PASS: Keyboard listener registered\n", total);
                passed++;
            } else {
                debug_printf("[TEST %d] FAIL: Listener not found in table (found=%u)\n", total, found);
            }
        } else {
            debug_printf("[TEST %d] FAIL: system_deck_listen returned %d, state=%u\n",
                    total, result, event.state);
        }
    }

    total++;
    kprintf("\n[TEST %d] Register same listener again (should fail LISTEN_ALREADY)\n", total);
    {
        Event event;
        event_init(&event, proc->pid, 301);
        event.prefixes[0] = 0xFF42;
        event.prefix_count = 1;
        event.current_prefix_idx = 0;
        event.data[0] = LISTEN_KEYBOARD;
        event.data[1] = 0;

        int result = system_deck_listen(&event);
        if (result == -1 && event.state == EVENT_STATE_ERROR) {
            uint32_t err_code = *((uint32_t*)event.data);
            if (err_code == (uint32_t)ERR_LISTEN_ALREADY) {
                debug_printf("[TEST %d] PASS: Duplicate correctly rejected (err=%u)\n", total, err_code);
                passed++;
            } else {
                debug_printf("[TEST %d] FAIL: Wrong error code in data (%u)\n", total, err_code);
            }
        } else {
            debug_printf("[TEST %d] FAIL: Expected error, got result=%d state=%u\n",
                    total, result, event.state);
        }
    }

    total++;
    kprintf("\n[TEST %d] Register MOUSE listener (different source type)\n", total);
    {
        Event event;
        event_init(&event, proc->pid, 302);
        event.prefixes[0] = 0xFF42;
        event.prefix_count = 1;
        event.current_prefix_idx = 0;
        event.data[0] = LISTEN_MOUSE;
        event.data[1] = LISTEN_FLAG_EXCLUSIVE;

        int result = system_deck_listen(&event);
        if (result == 0 && event.state == EVENT_STATE_COMPLETED) {
            uint32_t pids[4];
            uint32_t found = listen_table_find(LISTEN_MOUSE, pids, 4);
            if (found == 1 && pids[0] == proc->pid) {
                debug_printf("[TEST %d] PASS: Mouse listener registered with exclusive flag\n", total);
                passed++;
            } else {
                debug_printf("[TEST %d] FAIL: Mouse listener not found (found=%u)\n", total, found);
            }
        } else {
            debug_printf("[TEST %d] FAIL: system_deck_listen returned %d, state=%u\n",
                    total, result, event.state);
        }
    }

    process_destroy(proc);
    listen_table_init();

    kprintf("\n");
    kprintf("LISTEN VIA SYSTEM DECK TEST COMPLETE\n");
    kprintf("Results: %d/%d tests passed\n", passed, total);
}

void test_route_security(void) {
    kprintf("\n");
    kprintf("ROUTE/LISTEN SECURITY GATE TEST\n");

    int passed = 0;
    int total = 0;

    process_t* app_proc = process_create("app");
    process_t* utility_proc = process_create("utility");
    process_t* system_proc = process_create("system");
    process_t* display_proc = process_create("display");
    process_t* bare_proc = process_create("bare");

    if (!app_proc || !utility_proc || !system_proc || !display_proc || !bare_proc) {
        debug_printf("[TEST] ERROR: Cannot create test processes\n");
        if (app_proc) process_destroy(app_proc);
        if (utility_proc) process_destroy(utility_proc);
        if (system_proc) process_destroy(system_proc);
        if (display_proc) process_destroy(display_proc);
        if (bare_proc) process_destroy(bare_proc);
        return;
    }

    total++;
    kprintf("\n[TEST %d] ROUTE: 'app' process can route\n", total);
    if (system_security_gate(app_proc->pid, SYSTEM_DECK_ID, SYSTEM_OP_ROUTE)) {
        debug_printf("[TEST %d] PASS: app allowed ROUTE\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: app should be allowed ROUTE\n", total);
    }

    total++;
    kprintf("\n[TEST %d] ROUTE: 'utility' process can route\n", total);
    if (system_security_gate(utility_proc->pid, SYSTEM_DECK_ID, SYSTEM_OP_ROUTE)) {
        debug_printf("[TEST %d] PASS: utility allowed ROUTE\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: utility should be allowed ROUTE\n", total);
    }

    total++;
    kprintf("\n[TEST %d] ROUTE: 'system' process can route\n", total);
    if (system_security_gate(system_proc->pid, SYSTEM_DECK_ID, SYSTEM_OP_ROUTE)) {
        debug_printf("[TEST %d] PASS: system allowed ROUTE\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: system should be allowed ROUTE\n", total);
    }

    total++;
    kprintf("\n[TEST %d] ROUTE: 'bare' process CANNOT route\n", total);
    if (!system_security_gate(bare_proc->pid, SYSTEM_DECK_ID, SYSTEM_OP_ROUTE)) {
        debug_printf("[TEST %d] PASS: bare correctly denied ROUTE\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: bare should be denied ROUTE\n", total);
    }

    total++;
    kprintf("\n[TEST %d] ROUTE_TAG: 'app' process can route_tag\n", total);
    if (system_security_gate(app_proc->pid, SYSTEM_DECK_ID, SYSTEM_OP_ROUTE_TAG)) {
        debug_printf("[TEST %d] PASS: app allowed ROUTE_TAG\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: app should be allowed ROUTE_TAG\n", total);
    }

    total++;
    kprintf("\n[TEST %d] ROUTE_TAG: 'bare' process CANNOT route_tag\n", total);
    if (!system_security_gate(bare_proc->pid, SYSTEM_DECK_ID, SYSTEM_OP_ROUTE_TAG)) {
        debug_printf("[TEST %d] PASS: bare correctly denied ROUTE_TAG\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: bare should be denied ROUTE_TAG\n", total);
    }

    total++;
    kprintf("\n[TEST %d] LISTEN: 'app' process can listen\n", total);
    if (system_security_gate(app_proc->pid, SYSTEM_DECK_ID, SYSTEM_OP_LISTEN)) {
        debug_printf("[TEST %d] PASS: app allowed LISTEN\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: app should be allowed LISTEN\n", total);
    }

    total++;
    kprintf("\n[TEST %d] LISTEN: 'display' process can listen\n", total);
    if (system_security_gate(display_proc->pid, SYSTEM_DECK_ID, SYSTEM_OP_LISTEN)) {
        debug_printf("[TEST %d] PASS: display allowed LISTEN\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: display should be allowed LISTEN\n", total);
    }

    total++;
    kprintf("\n[TEST %d] LISTEN: 'system' process can listen\n", total);
    if (system_security_gate(system_proc->pid, SYSTEM_DECK_ID, SYSTEM_OP_LISTEN)) {
        debug_printf("[TEST %d] PASS: system allowed LISTEN\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: system should be allowed LISTEN\n", total);
    }

    total++;
    kprintf("\n[TEST %d] LISTEN: 'bare' process CANNOT listen\n", total);
    if (!system_security_gate(bare_proc->pid, SYSTEM_DECK_ID, SYSTEM_OP_LISTEN)) {
        debug_printf("[TEST %d] PASS: bare correctly denied LISTEN\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: bare should be denied LISTEN\n", total);
    }

    total++;
    kprintf("\n[TEST %d] LISTEN: 'utility' process CAN listen\n", total);
    if (system_security_gate(utility_proc->pid, SYSTEM_DECK_ID, SYSTEM_OP_LISTEN)) {
        debug_printf("[TEST %d] PASS: utility allowed LISTEN\n", total);
        passed++;
    } else {
        debug_printf("[TEST %d] FAIL: utility should be allowed LISTEN\n", total);
    }

    process_destroy(app_proc);
    process_destroy(utility_proc);
    process_destroy(system_proc);
    process_destroy(display_proc);
    process_destroy(bare_proc);

    kprintf("\n");
    kprintf("ROUTE/LISTEN SECURITY TEST COMPLETE\n");
    kprintf("Results: %d/%d tests passed\n", passed, total);
}

#else

void test_listen_table(void) { (void)0; }
void test_route_direct(void) { (void)0; }
void test_tag_wildcard(void) { (void)0; }
void test_route_tag(void) { (void)0; }
void test_listen_via_system_deck(void) { (void)0; }
void test_route_security(void) { (void)0; }

#endif
