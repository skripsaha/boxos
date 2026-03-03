#include "guide.h"
#include "execution_deck.h"
#include "system_deck.h"
#include "operations_deck.h"
#include "hardware_deck.h"
#include "storage_deck.h"
#include "pending_results.h"
#include "listen_table.h"
#include "process.h"
#include "klib.h"
#include "atomics.h"
#include "pmm.h"
#include "vmm.h"
#include "notify_page.h"
#include "error.h"
#include "async_io.h"
#include "ata_dma.h"
#include "ahci.h"
#include "cpu_calibrate.h"

EventRingBuffer *kernel_event_ring = NULL;

static void guide_process_ahci_completions(void);

event_ring_wait_queue_t event_ring_waiters;

static volatile uint8_t guide_idle = 1;
static volatile uint32_t next_event_id = 1;

static DeckEntry deck_table[] = {
    {0xFF, "System Deck", system_deck_handler},
    {0x01, "Operations Deck", operations_deck_handler},
    {0x02, "Storage Deck", storage_deck_handler},
    {0x03, "Hardware Deck", hardware_deck_handler},
    {0x00, NULL, NULL}};

// legacy wrapper: converts old int-based deck return value to error_t
static inline error_t wrap_legacy_deck_handler(deck_handler_t handler, Event* evt) {
    if (!handler) return ERR_INVALID_DECK_ID;
    int ret = handler(evt);
    return error_from_legacy_int(ret);
}

void guide_init_wait_queue(void)
{
    event_ring_waiters.head = NULL;
    event_ring_waiters.tail = NULL;
    spinlock_init(&event_ring_waiters.lock);
    event_ring_waiters.count = 0;

    debug_printf("[GUIDE] EventRing wait queue initialized\n");
}

void guide_wait_on_event_ring(process_t *proc)
{
    if (!proc)
        return;

    // hold ref so the process isn't destroyed while sitting in the wait queue
    process_ref_inc(proc);

    spin_lock(&event_ring_waiters.lock);

    proc->next_waiting = NULL;

    if (event_ring_waiters.tail)
    {
        event_ring_waiters.tail->next_waiting = proc;
        event_ring_waiters.tail = proc;
    }
    else
    {
        event_ring_waiters.head = proc;
        event_ring_waiters.tail = proc;
    }

    event_ring_waiters.count++;

    debug_printf("[GUIDE] Process PID %u blocked on EventRing (wait queue: %u)\n",
                 proc->pid, event_ring_waiters.count);

    spin_unlock(&event_ring_waiters.lock);
}

void guide_wakeup_waiters(void)
{
    spin_lock(&event_ring_waiters.lock);

    size_t available = event_ring_available(kernel_event_ring);
    uint32_t wakeup_count = 0;
    uint32_t cleanup_count = 0;

    process_t **current_ptr = &event_ring_waiters.head;

    while (*current_ptr)
    {
        process_t *proc = *current_ptr;

        if (process_get_state(proc) == PROC_WAITING &&
            proc->wait_reason == WAIT_RING_FULL)
        {
            if (available > 0)
            {
                proc->wait_reason = WAIT_NONE;
                process_set_state(proc, PROC_WORKING);

                *current_ptr = proc->next_waiting;
                proc->next_waiting = NULL;

                event_ring_waiters.count--;
                available--;
                wakeup_count++;

                process_ref_dec(proc);
            }
            else
            {
                current_ptr = &proc->next_waiting;
            }
        }
        else
        {
            // process terminated or otherwise invalid: remove and drop orphaned ref
            *current_ptr = proc->next_waiting;
            proc->next_waiting = NULL;
            event_ring_waiters.count--;
            cleanup_count++;

            process_ref_dec(proc);
        }
    }

    if (event_ring_waiters.head == NULL)
    {
        event_ring_waiters.tail = NULL;
    }

    spin_unlock(&event_ring_waiters.lock);

    if (wakeup_count > 0)
    {
        debug_printf("[GUIDE] Woke up %u processes (EventRing freed slots)\n",
                     wakeup_count);
    }

    if (cleanup_count > 0)
    {
        debug_printf("[GUIDE] Cleaned up %u terminated processes from wait queue\n",
                     cleanup_count);
    }
}

void guide_init(void)
{
    debug_printf("[GUIDE] Initializing Guide Dispatcher...\n");

    size_t initial_capacity = EVENT_RING_MIN_CAPACITY;
    kernel_event_ring = event_ring_create(initial_capacity);
    if (!kernel_event_ring)
    {
        panic("guide_init: Failed to create dynamic EventRing");
    }

    kprintf("[GUIDE] Dynamic EventRing created: initial_capacity=%zu, max=%zu\n",
            event_ring_capacity(kernel_event_ring),
            (size_t)EVENT_RING_MAX_CAPACITY);

    atomic_store_u8(&guide_idle, 1);
    atomic_store_u32(&next_event_id, 1);

    guide_init_wait_queue();

    pending_results_init();

    listen_table_init();

    debug_printf("[GUIDE] EventRing initialized, Guide idle\n");
}

deck_handler_t guide_get_deck_handler(uint8_t deck_id)
{
    for (size_t i = 0; deck_table[i].name != NULL; i++)
    {
        if (deck_table[i].deck_id == deck_id)
        {
            return deck_table[i].handler;
        }
    }
    return NULL;
}

void guide_wake(void)
{
    atomic_store_u8(&guide_idle, 0);
}

bool guide_is_idle(void)
{
    return atomic_load_u8(&guide_idle) != 0;
}

void guide_run(void)
{
    Event event;
    uint32_t processed_count = 0;
    static uint64_t last_timeout_check = 0;
    static uint64_t last_async_dispatch = 0;

    if (last_timeout_check == 0) {
        last_timeout_check = rdtsc();
    }
    if (last_async_dispatch == 0) {
        last_async_dispatch = rdtsc();
    }

    while (!event_ring_is_empty(kernel_event_ring))
    {
        if (event_ring_pop(kernel_event_ring, &event) != OK)
        {
            break;
        }

        if (!event_validate(&event))
        {
            continue;
        }

        event.error_code = OK;
        event.first_error = OK;
        event.error_deck_idx = 0xFF;
        event.state = EVENT_STATE_PROCESSING;

        bool need_execution_deck = true;

        while (event.current_prefix_idx < event.prefix_count)
        {
            if (event.current_prefix_idx >= event.prefix_count ||
                event.current_prefix_idx >= EVENT_MAX_PREFIXES)
            {
                error_t err = ERR_INVALID_EVENT;
                event_set_error(&event, err, event.current_prefix_idx);
                debug_printf("[GUIDE] ERROR: Invalid prefix index %u (count=%u): %s\n",
                             event.current_prefix_idx, event.prefix_count, error_string(err));
                event.state = EVENT_STATE_ERROR;
                execution_deck_handler(&event);
                need_execution_deck = false;
                break;
            }

            uint16_t prefix = event_current_prefix(&event);

            if (prefix == 0x0000)
            {
                execution_deck_handler(&event);
                need_execution_deck = false;
                break;
            }

            uint8_t deck_id = event_get_deck_id(&event, event.current_prefix_idx);
            uint8_t opcode = event_get_opcode(&event, event.current_prefix_idx);

            // Execution Deck (0x00) is only called at end of chain or on errors;
            // it is intentionally exempt from the security gate because it is a
            // finalizer that must always run to deliver results.

            bool security_ok = system_security_gate(event.pid, deck_id, opcode);
            if (!security_ok)
            {
                error_t sec_err = ERR_ACCESS_DENIED;
                event_set_error(&event, sec_err, event.current_prefix_idx);
                debug_printf("[GUIDE] Security gate failed for PID %u deck=0x%02x op=0x%02x prefix=0x%04x: %s\n",
                           event.pid, deck_id, opcode, prefix, error_string(sec_err));
                event.state = EVENT_STATE_ACCESS_DENIED;
                execution_deck_handler(&event);
                need_execution_deck = false;
                break;
            }

            deck_handler_t handler = guide_get_deck_handler(deck_id);
            if (!handler)
            {
                error_t err = ERR_INVALID_DECK_ID;
                event_set_error(&event, err, event.current_prefix_idx);
                debug_printf("[GUIDE] Unknown deck 0x%02x: %s\n", deck_id, error_string(err));
                event.state = EVENT_STATE_ERROR;
                execution_deck_handler(&event);
                need_execution_deck = false;
                break;
            }

            error_t deck_err = wrap_legacy_deck_handler(handler, &event);

            if (IS_ERROR(deck_err))
            {
                event_set_error(&event, deck_err, event.current_prefix_idx);
                debug_printf("[GUIDE] Deck 0x%02x failed: %s\n", deck_id, error_string(deck_err));
                if (event.state == EVENT_STATE_PROCESSING)
                {
                    event.state = EVENT_STATE_ERROR;
                }
                execution_deck_handler(&event);
                need_execution_deck = false;
                break;
            }

            event_advance(&event);
        }

        if (event.state == EVENT_STATE_RETRY)
        {
            execution_deck_handler(&event);
            need_execution_deck = false;
            continue;
        }

        if (need_execution_deck)
        {
            if (event.state == EVENT_STATE_PROCESSING)
            {
                event.state = EVENT_STATE_COMPLETED;
            }
            execution_deck_handler(&event);
        }

        processed_count++;
    }

    if (processed_count > 0 && event_ring_waiters.count > 0)
    {
        guide_wakeup_waiters();
    }

    guide_process_ahci_completions();

    uint64_t now = rdtsc();
    if ((now - last_async_dispatch) > cpu_ms_to_tsc(CONFIG_ASYNC_DISPATCH_INTERVAL_MS)) {
        if (async_io_pending_count() > 0 && ahci_can_submit_port(0)) {
            async_io_request_t io_req;
            if (async_io_dequeue(&io_req)) {
                error_t err = ahci_start_async_transfer((struct async_io_request*)&io_req);
                if (IS_ERROR(err)) {
                    async_io_mark_failed(io_req.event_id);

                    Event error_event;
                    event_init(&error_event, io_req.pid, io_req.event_id);
                    error_event.error_code = err;
                    error_event.state = EVENT_STATE_ERROR;
                    event_ring_push(kernel_event_ring, &error_event);

                    debug_printf("[GUIDE] AHCI async I/O dispatch failed for event_id=%u: %s\n",
                                io_req.event_id, error_string(err));
                }
            }
        }
        last_async_dispatch = now;
    }

    if ((now - last_timeout_check) > cpu_ms_to_tsc(CONFIG_DMA_TIMEOUT_CHECK_INTERVAL_MS)) {
        ahci_check_timeouts();
        last_timeout_check = now;
    }

    if (event_ring_is_empty(kernel_event_ring))
    {
        atomic_store_u8(&guide_idle, 1);
    }

    size_t ring_count = event_ring_count(kernel_event_ring);
    size_t ring_capacity = event_ring_capacity(kernel_event_ring);
    size_t ring_usage_percent = ring_capacity > 0 ? (ring_count * 100) / ring_capacity : 0;
    uint8_t backpressure_flag = (ring_usage_percent > 90) ? 1 : 0;

    extern process_t *process_get_first(void);
    process_t *proc = process_get_first();
    while (proc)
    {
        if (proc->notify_page_phys != 0)
        {
            notify_page_t *notify_page = (notify_page_t *)vmm_phys_to_virt(proc->notify_page_phys);
            if (notify_page)
            {
                atomic_store_u8(&notify_page->event_ring_full, backpressure_flag);
            }
        }
        proc = proc->next;
    }
}

uint32_t guide_alloc_event_id(void)
{
    uint32_t id;
    do {
        id = atomic_load_u32(&next_event_id);
        if (id == 0) id = 1;
    } while (!atomic_cas_u32(&next_event_id, id, id + 1));
    return id;
}

static void guide_process_ahci_completions(void)
{
    ahci_port_t* state = ahci_get_port_state(0);
    if (!state) {
        return;
    }

    uint32_t completed = __sync_fetch_and_and(&state->completed_slots, 0);
    mfence();
    if (completed == 0) {
        return;
    }

    volatile ahci_port_regs_t* port = ahci_get_port_regs_pub(0);

    for (uint8_t slot = 0; slot < 32; slot++) {
        if (!(completed & (1U << slot))) {
            continue;
        }

        uint32_t event_id = state->event_id[slot];
        uint32_t pid = state->pid[slot];
        uint64_t submit_tsc = state->submit_tsc[slot];

        bool error = (port->tfd & 0x01);

        if (error) {
            async_io_mark_failed(event_id);
        } else {
            async_io_mark_completed_with_latency(event_id, submit_tsc);
        }

        Event evt;
        event_init(&evt, pid, event_id);
        evt.state = error ? EVENT_STATE_ERROR : EVENT_STATE_COMPLETED;
        if (error) {
            evt.error_code = ERR_IO;
        }

        event_push_result_t result = event_ring_push_priority(
            kernel_event_ring,
            &evt,
            EVENT_PRIORITY_SYSTEM
        );

        if (result != EVENT_PUSH_SYSTEM_OK) {
            panic("EventRing overflow: lost I/O completion event (PID %u)", evt.pid);
        }

        ahci_free_slot(0, slot);
    }
}
