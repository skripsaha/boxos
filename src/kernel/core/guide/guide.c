#include "guide.h"
#include "execution_deck.h"
#include "system_deck.h"
#include "operations_deck.h"
#include "hardware_deck.h"
#include "storage_deck.h"
#include "listen_table.h"
#include "process.h"
#include "klib.h"
#include "vmm.h"
#include "pocket_ring.h"
#include "error.h"
#include "perf_trace.h"
#include "amp.h"

ReadyQueue g_ready_queue;

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

    perf_trace_init();

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

    // Debug: show pocket processing start
    uint8_t core_idx = amp_get_core_index();
    const char* core_type = amp_is_kcore() ? "K" : (core_idx == g_amp.bsp_index ? "BSP" : "A");
    debug_printf("[%s%u] GUIDE pocket PID %u prefixes=%u\n", 
                 core_type, core_idx, proc->pid, pocket->prefix_count);

    bool need_execution_deck = true;

    while (pocket->current_prefix_idx < pocket->prefix_count)
    {
        if (pocket->current_prefix_idx >= POCKET_MAX_PREFIXES)
        {
            pocket->error_code = ERR_INVALID_POCKET;
            debug_printf("[GUIDE] ERROR: Invalid prefix index %u (count=%u)\n",
                         pocket->current_prefix_idx, pocket->prefix_count);
            execution_deck_handler(pocket, proc);
            need_execution_deck = false;
            break;
        }

        uint16_t prefix = pocket_current_prefix(pocket);

        if (prefix == 0x0000)
        {
            execution_deck_handler(pocket, proc);
            need_execution_deck = false;
            break;
        }

        uint8_t deck_id = pocket_get_deck_id(pocket, pocket->current_prefix_idx);
        uint8_t opcode  = pocket_get_opcode(pocket, pocket->current_prefix_idx);

        bool security_ok = system_security_gate(proc, deck_id, opcode);
        if (!security_ok)
        {
            pocket->error_code = ERR_ACCESS_DENIED;
            debug_printf("[GUIDE] Security gate failed for PID %u deck=0x%02x op=0x%02x\n",
                         pocket->pid, deck_id, opcode);
            execution_deck_handler(pocket, proc);
            need_execution_deck = false;
            break;
        }

        deck_handler_t handler = guide_get_deck_handler(deck_id);
        if (!handler)
        {
            pocket->error_code = ERR_INVALID_DECK_ID;
            debug_printf("[GUIDE] Unknown deck 0x%02x\n", deck_id);
            execution_deck_handler(pocket, proc);
            need_execution_deck = false;
            break;
        }

        PERF_TRACE_START(perf_start);

        debug_printf("[%s%u]   -> Deck 0x%02x op=0x%02x\n", core_type, core_idx, deck_id, opcode);

        int deck_ret = handler(pocket, proc);

        PERF_TRACE_END(perf_start, pocket->pid, deck_id, opcode,
                       (uint16_t)pocket->error_code);

        if (deck_ret < 0)
        {
            if (pocket->error_code == OK)
            {
                pocket->error_code = ERR_INTERNAL;
            }
            debug_printf("[GUIDE] Deck 0x%02x failed: %s\n",
                         deck_id, error_string(pocket->error_code));
            execution_deck_handler(pocket, proc);
            need_execution_deck = false;
            break;
        }

        pocket_advance(pocket);
    }

    if (need_execution_deck)
    {
        execution_deck_handler(pocket, proc);
    }

    // Consume the Pocket from the ring
    pocket_ring_pop(pring);
}

void guide(void)
{
    uint32_t pockets_processed = 0;
    uint32_t perf_snapshot = perf_trace_snapshot();

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

    // Flush trace entries AFTER all measurements are done (serial I/O here is safe)
    if (pockets_processed > 0)
    {
        perf_trace_flush_since(perf_snapshot);
    }
}

// Process all pending Pockets for one process (K-Core guide loop entry point).
// Drains PocketRing completely and writes Results. Does NOT change process state.
void guide_process_one(process_t *proc)
{
    if (!proc || !proc->cabin)
        return;

    PocketRing *pring = (PocketRing *)vmm_phys_to_virt(proc->pocket_ring_phys);
    if (!pring)
        return;

    uint32_t perf_snapshot = perf_trace_snapshot();
    uint32_t count = 0;

    while (!pocket_ring_is_empty(pring))
    {
        guide_process_pocket(proc);
        count++;
    }

    if (count > 0)
    {
        perf_trace_flush_since(perf_snapshot);
    }
}

