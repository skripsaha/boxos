#include "guide.h"
#include "execution_deck.h"
#include "system_deck.h"
#include "operations_deck.h"
#include "hardware_deck.h"
#include "storage_deck.h"
#include "listen_table.h"
#include "process.h"
#include "klib.h"
#include "atomics.h"
#include "pmm.h"
#include "vmm.h"
#include "pocket_ring.h"
#include "result_ring.h"
#include "error.h"
#include "async_io.h"
#include "ata_dma.h"
#include "ahci.h"
#include "cpu_calibrate.h"

ReadyQueue g_ready_queue;

static void guide_process_ahci_completions(void);

static DeckEntry deck_table[] = {
    {0xFF, "System Deck", system_deck_handler},
    {0x01, "Operations Deck", operations_deck_handler},
    {0x02, "Storage Deck", storage_deck_handler},
    {0x03, "Hardware Deck", hardware_deck_handler},
    {0x00, NULL, NULL}};

void guide_init(void)
{
    debug_printf("[GUIDE] Initializing Guide Dispatcher...\n");

    ready_queue_init(&g_ready_queue);

    listen_table_init();

    debug_printf("[GUIDE] ReadyQueue initialized (capacity=%u)\n",
                 READY_QUEUE_CAPACITY);
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

// Process one Pocket from a process's PocketRing through the prefix chain.
static void guide_process_pocket(process_t *proc)
{
    if (!proc || !proc->cabin)
        return;

    // Access PocketRing via physical address (identity mapping)
    PocketRing *pring = (PocketRing *)vmm_phys_to_virt(proc->pocket_ring_phys);
    if (!pring)
        return;

    Pocket *pocket = pocket_ring_peek(pring);
    if (!pocket)
        return;

    // Fill in the source PID (kernel sets this, not userspace)
    pocket->pid = proc->pid;
    pocket->error_code = OK;
    pocket->current_prefix_idx = 0;

    // Yield pockets: just consume, no processing or Result
    if (pocket->flags & POCKET_FLAG_YIELD)
    {
        pocket_ring_pop(pring);
        return;
    }

    bool need_execution_deck = true;

    while (pocket->current_prefix_idx < pocket->prefix_count)
    {
        if (pocket->current_prefix_idx >= POCKET_MAX_PREFIXES)
        {
            pocket->error_code = ERR_INVALID_EVENT;
            debug_printf("[GUIDE] ERROR: Invalid prefix index %u (count=%u)\n",
                         pocket->current_prefix_idx, pocket->prefix_count);
            execution_deck_handler(pocket);
            need_execution_deck = false;
            break;
        }

        uint16_t prefix = pocket_current_prefix(pocket);

        if (prefix == 0x0000)
        {
            execution_deck_handler(pocket);
            need_execution_deck = false;
            break;
        }

        uint8_t deck_id = pocket_get_deck_id(pocket, pocket->current_prefix_idx);
        uint8_t opcode = pocket_get_opcode(pocket, pocket->current_prefix_idx);

        bool security_ok = system_security_gate(pocket->pid, deck_id, opcode);
        if (!security_ok)
        {
            pocket->error_code = ERR_ACCESS_DENIED;
            debug_printf("[GUIDE] Security gate failed for PID %u deck=0x%02x op=0x%02x\n",
                         pocket->pid, deck_id, opcode);
            execution_deck_handler(pocket);
            need_execution_deck = false;
            break;
        }

        deck_handler_t handler = guide_get_deck_handler(deck_id);
        if (!handler)
        {
            pocket->error_code = ERR_INVALID_DECK_ID;
            debug_printf("[GUIDE] Unknown deck 0x%02x\n", deck_id);
            execution_deck_handler(pocket);
            need_execution_deck = false;
            break;
        }

        uint64_t deck_start = rdtsc();
        int deck_ret = handler(pocket);
        uint64_t deck_elapsed_us = cpu_tsc_to_us(rdtsc() - deck_start);

        if (deck_ret < 0)
        {
            if (pocket->error_code == OK)
            {
                pocket->error_code = error_from_legacy_int(deck_ret);
            }
            debug_printf("[GUIDE] Deck 0x%02x failed: %s\n",
                         deck_id, error_string(pocket->error_code));
            execution_deck_handler(pocket);
            need_execution_deck = false;
            break;
        }

#if CONFIG_DEBUG_ENABLED
        if (deck_elapsed_us > 50)
        {
            debug_printf("[GUIDE] PID %u deck=0x%02x op=0x%02x: %llu us\n",
                         pocket->pid, deck_id, opcode, deck_elapsed_us);
        }
#endif

        pocket_advance(pocket);
    }

    if (need_execution_deck)
    {
        execution_deck_handler(pocket);
    }

    // Consume the Pocket from the ring
    pocket_ring_pop(pring);
}

void guide(void)
{
    uint64_t guide_start = rdtsc();
    uint32_t pockets_processed = 0;

    // Process all ready processes from the ReadyQueue
    while (!ready_queue_is_empty(&g_ready_queue))
    {
        process_t *proc = ready_queue_pop(&g_ready_queue);
        if (!proc)
            break;

        // Process all pending Pockets for this process
        PocketRing *pring = (PocketRing *)vmm_phys_to_virt(proc->pocket_ring_phys);
        if (pring)
        {
            while (!pocket_ring_is_empty(pring))
            {
                guide_process_pocket(proc);
                pockets_processed++;
            }
        }

        // Wake the process — all its pockets have been processed
        if (process_get_state(proc) == PROC_WAITING)
        {
            process_set_state(proc, PROC_WORKING);
        }
    }

    // Periodic cleanup of finished processes (runs in syscall context, not IRQ)
    {
        static uint64_t last_cleanup_tsc = 0;
        uint64_t now_tsc = rdtsc();
        if (last_cleanup_tsc == 0)
            last_cleanup_tsc = now_tsc;

        if ((now_tsc - last_cleanup_tsc) > cpu_ms_to_tsc(1000))
        {
            uint32_t cleanup_pids[16];
            uint32_t cleanup_count = 0;

            process_list_lock();
            process_t *p = process_get_first();
            while (p && cleanup_count < 16)
            {
                process_state_t state = process_get_state(p);
                uint32_t refs = process_ref_count(p);
                if ((state == PROC_CRASHED || state == PROC_DONE) && refs == 0)
                {
                    cleanup_pids[cleanup_count++] = p->pid;
                }
                p = p->next;
            }
            process_list_unlock();

            for (uint32_t i = 0; i < cleanup_count; i++)
            {
                process_t *cp = process_find(cleanup_pids[i]);
                if (cp)
                {
                    if (process_get_state(cp) == PROC_DONE)
                    {
                        process_set_state(cp, PROC_CRASHED);
                    }
                    process_destroy_safe(cp);
                }
            }

            last_cleanup_tsc = now_tsc;
        }
    }

    // Handle async I/O completions and dispatch
    guide_process_ahci_completions();

    static uint64_t last_timeout_check = 0;
    static uint64_t last_async_dispatch = 0;

    if (last_timeout_check == 0)
        last_timeout_check = rdtsc();
    if (last_async_dispatch == 0)
        last_async_dispatch = rdtsc();

    uint64_t now = rdtsc();
    if ((now - last_async_dispatch) > cpu_ms_to_tsc(CONFIG_ASYNC_DISPATCH_INTERVAL_MS))
    {
        if (async_io_pending_count() > 0 && ahci_can_submit_port(0))
        {
            async_io_request_t io_req;
            if (async_io_dequeue(&io_req))
            {
                error_t err = ahci_start_async_transfer((struct async_io_request *)&io_req);
                if (IS_ERROR(err))
                {
                    async_io_mark_failed(io_req.event_id);
                    debug_printf("[GUIDE] AHCI async I/O dispatch failed for event_id=%u: %s\n",
                                 io_req.event_id, error_string(err));
                }
            }
        }
        last_async_dispatch = now;
    }

    if ((now - last_timeout_check) > cpu_ms_to_tsc(CONFIG_DMA_TIMEOUT_CHECK_INTERVAL_MS))
    {
        ahci_check_timeouts();
        async_io_expire_stale(cpu_ms_to_tsc(CONFIG_ASYNC_IO_QUEUE_TIMEOUT_MS));
        last_timeout_check = now;
    }

#if CONFIG_DEBUG_ENABLED
    {
        uint64_t guide_us = cpu_tsc_to_us(rdtsc() - guide_start);
        if (guide_us > 200 && pockets_processed > 0)
        {
            debug_printf("[GUIDE] Processed %u pockets in %llu us\n",
                         pockets_processed, guide_us);
        }
    }
#endif
}

static void guide_process_ahci_completions(void)
{
    ahci_port_t *state = ahci_get_port_state(0);
    if (!state)
    {
        return;
    }

    uint32_t completed = __sync_fetch_and_and(&state->completed_slots, 0);
    mfence();
    if (completed == 0)
    {
        return;
    }

    volatile ahci_port_regs_t *port = ahci_get_port_regs_pub(0);

    for (uint8_t slot = 0; slot < 32; slot++)
    {
        if (!(completed & (1U << slot)))
        {
            continue;
        }

        uint32_t event_id = state->event_id[slot];
        uint32_t pid = state->pid[slot];
        uint64_t submit_tsc = state->submit_tsc[slot];

        bool error = (port->tfd & 0x01);

        if (error)
        {
            async_io_mark_failed(event_id);
        }
        else
        {
            async_io_mark_completed_with_latency(event_id, submit_tsc);
        }

        // Deliver I/O completion result directly to the process's ResultRing
        process_t *proc = process_find(pid);
        if (proc && proc->result_ring_phys)
        {
            ResultRing *rring = (ResultRing *)vmm_phys_to_virt(proc->result_ring_phys);
            if (rring)
            {
                Result result;
                result.error_code = error ? ERR_IO : OK;
                result.data_length = 0;
                result.data_addr = 0;
                result.sender_pid = 0;
                result._reserved = 0;
                result_ring_push(rring, &result);
            }
        }

        ahci_free_slot(0, slot);
    }
}
