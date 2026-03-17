#include "klib.h"
#include "ahci.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "io.h"
#include "irqchip.h"
#include "atomics.h"
#include "async_io.h"
#include "result_ring.h"
#include "error.h"
#include "pic.h"
#include "process.h"
#include "cpu_calibrate.h"
#include "boxos_memory.h"
#include "idt.h"

static ahci_controller_t ahci_ctrl;

// 2 seconds timeout in TSC cycles (calibrated at boot)
#define AHCI_TIMEOUT_MS 2000

// Helper: push a Result to a process's ResultRing
static void ahci_push_result(uint32_t pid, uint32_t error_code) {
    process_t* proc = process_find(pid);
    if (!proc || !proc->result_ring_phys) return;
    ResultRing* rring = (ResultRing*)vmm_phys_to_virt(proc->result_ring_phys);
    if (!rring) return;
    Result r;
    memset(&r, 0, sizeof(Result));
    r.error_code = error_code;
    r.sender_pid = 0;  // kernel
    result_ring_push(rring, &r);
}

static inline ahci_port_regs_t* ahci_get_port_regs(uint8_t port_num) {
    uintptr_t port_base = (uintptr_t)ahci_ctrl.hba_mem + 0x100 + (port_num * 0x80);
    return (ahci_port_regs_t*)port_base;
}

static ahci_port_t* ahci_get_port(uint8_t port_num) {
    if (port_num >= AHCI_MAX_PORTS || !ahci_ctrl.initialized)
        return NULL;
    ahci_port_t* p = &ahci_ctrl.ports[port_num];
    return p->active ? p : NULL;
}

ahci_port_t* ahci_get_port_state(uint8_t port_num) {
    return ahci_get_port(port_num);
}

volatile ahci_port_regs_t* ahci_get_port_regs_pub(uint8_t port_num) {
    ahci_port_t* port = ahci_get_port(port_num);
    if (!port) return NULL;
    return ahci_get_port_regs(port_num);
}

static int ahci_port_stop(ahci_port_t* port) {
    if (!port || !port->regs) {
        return -1;
    }

    ahci_port_regs_t* regs = port->regs;

    regs->cmd &= ~AHCI_PCMD_ST;

    // AHCI spec: 500ms for CR to clear after ST cleared
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(500);
    while (rdtsc() < deadline) {
        if ((regs->cmd & AHCI_PCMD_CR) == 0) {
            break;
        }
        cpu_pause();
    }

    if (regs->cmd & AHCI_PCMD_CR) {
        debug_printf("[AHCI] Port %u: Failed to stop (CR still set)\n", port->port_num);
        return -1;
    }

    regs->cmd &= ~AHCI_PCMD_FRE;

    // AHCI spec: 500ms for FR to clear after FRE cleared
    deadline = rdtsc() + cpu_ms_to_tsc(500);
    while (rdtsc() < deadline) {
        if ((regs->cmd & AHCI_PCMD_FR) == 0) {
            break;
        }
        cpu_pause();
    }

    if (regs->cmd & AHCI_PCMD_FR) {
        debug_printf("[AHCI] Port %u: Failed to stop FIS receive (FR still set)\n", port->port_num);
        return -1;
    }

    return 0;
}

static int ahci_port_start(ahci_port_t* port) {
    if (!port || !port->regs) {
        return -1;
    }

    ahci_port_regs_t* regs = port->regs;

    // Wait up to 500ms for CR to clear before starting
    uint64_t start_deadline = rdtsc() + cpu_ms_to_tsc(500);
    while (rdtsc() < start_deadline) {
        if ((regs->cmd & AHCI_PCMD_CR) == 0) {
            break;
        }
        cpu_pause();
    }

    if (regs->cmd & AHCI_PCMD_CR) {
        debug_printf("[AHCI] Port %u: Cannot start - CR still set\n", port->port_num);
        return -1;
    }

    regs->cmd |= AHCI_PCMD_FRE;
    regs->cmd |= AHCI_PCMD_ST;

    return 0;
}

void ahci_irq_handler(void) {
    // Called from irq_handler with interrupts disabled (interrupt gate).
    // Do NOT call cli/sti or send EOI here — irq_handler handles EOI after return.

    if (!ahci_ctrl.initialized) {
        return;
    }

    uint32_t is = ahci_ctrl.hba_mem->is;
    if (is == 0) {
        return;
    }

    __sync_fetch_and_add(&ahci_ctrl.total_interrupts, 1);

    uint32_t ports_pending = is & ahci_ctrl.port_implemented;
    for (uint8_t i = 0; i < AHCI_MAX_PORTS && ports_pending; i++) {
        if (!(ports_pending & (1U << i))) continue;
        ports_pending &= ~(1U << i);

        ahci_port_t* state = &ahci_ctrl.ports[i];
        if (!state->active) continue;

        volatile ahci_port_regs_t* port = ahci_get_port_regs(i);
        uint32_t port_is = port->is;

        uint32_t completed = state->ci_snapshot ^ port->ci;
        completed &= state->ci_snapshot;

        if (completed) {
            __sync_fetch_and_or(&state->completed_slots, completed);
            __atomic_store_n(&state->ci_snapshot, port->ci, __ATOMIC_RELEASE);
        }

        if (port_is & (1 << 30)) {
            __sync_fetch_and_add(&state->ncq_errors, 1);
        }

        port->is = port_is;
    }

    ahci_ctrl.hba_mem->is = is;
}

void ahci_port_enable_irq(uint8_t port_num) {
    ahci_port_t* port = ahci_get_port(port_num);
    if (!port) return;

    volatile ahci_port_regs_t* regs = port->regs;

    regs->ie = (1 << 0)  |  // DHRE
               (1 << 1)  |  // PSE
               (1 << 2)  |  // DSE
               (1 << 3)  |  // SDBS (NCQ)
               (1 << 29) |  // TFES
               (1 << 30);   // HBFS

    ahci_ctrl.hba_mem->ghc |= (1 << 1);

    debug_printf("[AHCI] Port %u: Interrupts enabled (IE=0x%08x)\n", port_num, regs->ie);
}

void ahci_init_irq(void) {
    if (!ahci_ctrl.initialized) {
        return;
    }

    if (ahci_ctrl.irq_vector == 0xFF || ahci_ctrl.irq_vector >= IRQ_MAX_COUNT) {
        debug_printf("[AHCI] No valid IRQ, using polling mode\n");
        return;
    }

    irq_register_handler(ahci_ctrl.irq_vector, ahci_irq_handler);
    irqchip_enable_irq(ahci_ctrl.irq_vector);

    ahci_ctrl.irq_enabled = true;
    debug_printf("[AHCI] IRQ %u registered and enabled\n", ahci_ctrl.irq_vector);
}

int ahci_alloc_slot(uint8_t port_num) {
    ahci_port_t* port = ahci_get_port(port_num);
    if (!port) return -1;

    spin_lock(&port->lock);

    if (!port->active) {
        spin_unlock(&port->lock);
        return -1;
    }

    int slot = -1;
    for (uint8_t s = 0; s < AHCI_MAX_SLOTS; s++) {
        if (port->slot_bitmap & (1U << s)) {
            port->slot_bitmap &= ~(1U << s);
            slot = s;
            break;
        }
    }

    spin_unlock(&port->lock);
    return slot;
}

void ahci_free_slot(uint8_t port_num, uint8_t slot) {
    if (slot >= AHCI_MAX_SLOTS) return;
    ahci_port_t* port = ahci_get_port(port_num);
    if (!port) return;

    spin_lock(&port->lock);
    if (!port->active) {
        spin_unlock(&port->lock);
        return;
    }
    port->slot_bitmap |= (1U << slot);
    spin_unlock(&port->lock);
}

bool ahci_can_submit_port(uint8_t port_num) {
    ahci_port_t* port = ahci_get_port(port_num);
    if (!port) return false;
    return port->slot_bitmap != 0;
}

error_t ahci_build_ncq_read(uint8_t port_num, uint8_t slot, uint64_t lba,
                                   uint16_t sector_count, void* buffer_phys) {
    if (slot >= AHCI_MAX_SLOTS) return ERR_INVALID_ARGUMENT;
    ahci_port_t* port = ahci_get_port(port_num);
    if (!port) return ERR_DEVICE_NOT_READY;

    ahci_cmd_header_t* cmdheader = (ahci_cmd_header_t*)port->clb_virt;
    cmdheader[slot].cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmdheader[slot].w = 0;
    cmdheader[slot].prdtl = 1;
    cmdheader[slot].prdbc = 0;

    ahci_cmd_table_t* cmdtbl = (ahci_cmd_table_t*)port->ctba_virt[slot];
    memset(cmdtbl, 0, sizeof(ahci_cmd_table_t));

    cmdtbl->prdt[0].dba = (uint32_t)(uintptr_t)buffer_phys;
    cmdtbl->prdt[0].dbau = (uint32_t)((uintptr_t)buffer_phys >> 32);
    cmdtbl->prdt[0].dbc = (sector_count * 512) - 1;
    cmdtbl->prdt[0].i = 1;

    fis_reg_h2d_t* cmdfis = (fis_reg_h2d_t*)&cmdtbl->cfis[0];
    memset(cmdfis, 0, sizeof(fis_reg_h2d_t));

    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c = 1;
    cmdfis->command = ATA_CMD_READ_FPDMA_QUEUED;

    cmdfis->lba0 = (lba >> 0) & 0xFF;
    cmdfis->lba1 = (lba >> 8) & 0xFF;
    cmdfis->lba2 = (lba >> 16) & 0xFF;
    cmdfis->lba3 = (lba >> 24) & 0xFF;
    cmdfis->lba4 = (lba >> 32) & 0xFF;
    cmdfis->lba5 = (lba >> 40) & 0xFF;

    cmdfis->device = (1 << 6);

    cmdfis->featurel = sector_count & 0xFF;
    cmdfis->featureh = (sector_count >> 8) & 0xFF;

    cmdfis->countl = (slot << 3);  // NCQ tag in bits [7:3]
    cmdfis->counth = 0;

    return OK;
}

error_t ahci_build_ncq_write(uint8_t port_num, uint8_t slot, uint64_t lba,
                                    uint16_t sector_count, void* buffer_phys) {
    if (slot >= AHCI_MAX_SLOTS) return ERR_INVALID_ARGUMENT;
    ahci_port_t* port = ahci_get_port(port_num);
    if (!port) return ERR_DEVICE_NOT_READY;

    ahci_cmd_header_t* cmdheader = (ahci_cmd_header_t*)port->clb_virt;
    cmdheader[slot].cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmdheader[slot].w = 1;
    cmdheader[slot].prdtl = 1;
    cmdheader[slot].prdbc = 0;

    ahci_cmd_table_t* cmdtbl = (ahci_cmd_table_t*)port->ctba_virt[slot];
    memset(cmdtbl, 0, sizeof(ahci_cmd_table_t));

    cmdtbl->prdt[0].dba = (uint32_t)(uintptr_t)buffer_phys;
    cmdtbl->prdt[0].dbau = (uint32_t)((uintptr_t)buffer_phys >> 32);
    cmdtbl->prdt[0].dbc = (sector_count * 512) - 1;
    cmdtbl->prdt[0].i = 1;

    fis_reg_h2d_t* cmdfis = (fis_reg_h2d_t*)&cmdtbl->cfis[0];
    memset(cmdfis, 0, sizeof(fis_reg_h2d_t));

    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c = 1;
    cmdfis->command = ATA_CMD_WRITE_FPDMA_QUEUED;

    cmdfis->lba0 = (lba >> 0) & 0xFF;
    cmdfis->lba1 = (lba >> 8) & 0xFF;
    cmdfis->lba2 = (lba >> 16) & 0xFF;
    cmdfis->lba3 = (lba >> 24) & 0xFF;
    cmdfis->lba4 = (lba >> 32) & 0xFF;
    cmdfis->lba5 = (lba >> 40) & 0xFF;

    cmdfis->device = (1 << 6);

    cmdfis->featurel = sector_count & 0xFF;
    cmdfis->featureh = (sector_count >> 8) & 0xFF;

    cmdfis->countl = (slot << 3);  // NCQ tag in bits [7:3]
    cmdfis->counth = 0;

    return OK;
}

error_t ahci_start_async_transfer(struct async_io_request* req_raw) {
    async_io_request_t* req = (async_io_request_t*)req_raw;

    if (!req || !ahci_ctrl.initialized) {
        return ERR_NULL_POINTER;
    }

    if (req->sector_count == 0 || req->sector_count > 8) {
        return ERR_INVALID_ARGUMENT;
    }

    // TODO: async_io_request_t needs port_num field for multi-port async I/O
    ahci_port_t* state = &ahci_ctrl.ports[0];
    volatile ahci_port_regs_t* port = state->regs;

    int slot = ahci_alloc_slot(0);
    if (slot < 0) {
        return ERR_IO_QUEUE_FULL;
    }

    state->event_id[slot] = req->event_id;
    state->pid[slot] = req->pid;
    state->submit_tsc[slot] = rdtsc();

    void* target_phys;

    if (req->op == ASYNC_IO_OP_WRITE) {
        if (!req->buffer_virt) {
            ahci_free_slot(0, slot);
            return ERR_NULL_POINTER;
        }

        uint32_t total_size = req->sector_count * 512;
        uint32_t copy_size = req->data_length < total_size ? req->data_length : total_size;
        memset(state->staging_virt[slot], 0, total_size);
        memcpy(state->staging_virt[slot], req->buffer_virt, copy_size);
        target_phys = state->staging_phys[slot];
        state->ncq_writes++;

        if (ahci_build_ncq_write(0, slot, req->lba, req->sector_count, target_phys) != OK) {
            ahci_free_slot(0, slot);
            return ERR_IO;
        }
    } else {
        // READ: translate user virtual -> physical for DMA
        process_t* proc = process_find(req->pid);
        if (!proc || !proc->cabin) {
            ahci_free_slot(0, slot);
            return ERR_INVALID_ARGUMENT;
        }

        uintptr_t target_phys_addr = vmm_virt_to_phys(proc->cabin, (uintptr_t)req->buffer_virt);
        if (target_phys_addr == 0) {
            ahci_free_slot(0, slot);
            return ERR_INVALID_ADDRESS;
        }

        target_phys = (void*)target_phys_addr;
        state->ncq_reads++;

        if (ahci_build_ncq_read(0, slot, req->lba, req->sector_count, target_phys) != OK) {
            ahci_free_slot(0, slot);
            return ERR_IO;
        }
    }

    mfence();

    port->sact |= (1U << slot);
    port->ci |= (1U << slot);
    __sync_fetch_and_or(&state->ci_snapshot, (1U << slot));

    return OK;
}

void ahci_check_timeouts(void) {
    if (!ahci_ctrl.initialized) {
        return;
    }

    uint64_t now = rdtsc();

    for (uint8_t p = 0; p < AHCI_MAX_PORTS; p++) {
        ahci_port_t* state = &ahci_ctrl.ports[p];
        if (!state->active) continue;

        uint32_t snapshot = __atomic_load_n(&state->ci_snapshot, __ATOMIC_ACQUIRE);

        for (uint8_t slot = 0; slot < AHCI_MAX_SLOTS; slot++) {
            if (!(snapshot & (1U << slot))) {
                continue;
            }

            if ((now - state->submit_tsc[slot]) < cpu_ms_to_tsc(AHCI_TIMEOUT_MS)) {
                continue;
            }

            async_io_mark_failed(state->event_id[slot]);

            ahci_push_result(state->pid[slot], ERR_TIMEOUT);

            ahci_free_slot(p, slot);
            __sync_fetch_and_and(&state->ci_snapshot, ~(1U << slot));
            __sync_fetch_and_add(&state->ncq_timeouts, 1);

            debug_printf("[AHCI] Port %u: Timeout on slot %u (event_id=%u)\n", p, slot, state->event_id[slot]);
        }
    }
}

static int ahci_port_init(uint8_t port_num) {
    if (port_num >= AHCI_MAX_PORTS) {
        return -1;
    }

    ahci_port_t* port = &ahci_ctrl.ports[port_num];
    memset(port, 0, sizeof(ahci_port_t));

    port->port_num = port_num;
    port->regs = ahci_get_port_regs(port_num);

    if (ahci_port_stop(port) != 0) {
        debug_printf("[AHCI] Port %u: Failed to stop\n", port_num);
        return -1;
    }

    void* clb_page = pmm_alloc(1);
    if (!clb_page) {
        debug_printf("[AHCI] Port %u: Failed to allocate CLB page\n", port_num);
        return -1;
    }

    uintptr_t clb_phys = (uintptr_t)clb_page;
    void* clb_virt = vmm_phys_to_virt(clb_phys);
    memset(clb_virt, 0, 4096);

    port->clb_virt = clb_virt;
    port->clb_phys = clb_phys;
    port->fis_virt = (uint8_t*)clb_virt + 1024;
    port->fis_phys = clb_phys + 1024;

    for (uint8_t slot = 0; slot < AHCI_MAX_SLOTS; slot++) {
        void* ctba_page = pmm_alloc(1);
        if (!ctba_page) {
            debug_printf("[AHCI] Port %u: Failed to allocate CTBA for slot %u\n", port_num, slot);
            for (uint8_t i = 0; i < slot; i++) {
                pmm_free((void*)port->ctba_phys[i], 1);
            }
            pmm_free(clb_page, 1);
            return -1;
        }

        uintptr_t ctba_phys = (uintptr_t)ctba_page;
        void* ctba_virt = vmm_phys_to_virt(ctba_phys);
        memset(ctba_virt, 0, 4096);
        port->ctba_virt[slot] = ctba_virt;
        port->ctba_phys[slot] = ctba_phys;

        ahci_cmd_header_t* cmdheader = (ahci_cmd_header_t*)port->clb_virt;
        cmdheader[slot].prdtl = 0;
        cmdheader[slot].prdbc = 0;
        cmdheader[slot].ctba = (uint32_t)port->ctba_phys[slot];
        cmdheader[slot].ctbau = (uint32_t)(port->ctba_phys[slot] >> 32);
    }

    port->regs->clb = (uint32_t)port->clb_phys;
    port->regs->clbu = (uint32_t)(port->clb_phys >> 32);
    port->regs->fb = (uint32_t)port->fis_phys;
    port->regs->fbu = (uint32_t)(port->fis_phys >> 32);

    port->regs->is = 0xFFFFFFFF;
    port->regs->serr = 0xFFFFFFFF;

    for (int i = 0; i < AHCI_MAX_SLOTS; i++) {
        void* staging_phys = pmm_alloc(1);
        if (!staging_phys) {
            debug_printf("[AHCI] Port %u: Failed to allocate staging buffer for slot %u\n", port_num, i);
            for (int j = 0; j < i; j++) {
                pmm_free(port->staging_phys[j], 1);
            }
            for (uint8_t k = 0; k < AHCI_MAX_SLOTS; k++) {
                pmm_free((void*)port->ctba_phys[k], 1);
            }
            pmm_free(clb_page, 1);
            return -1;
        }

        void* staging_virt = vmm_phys_to_virt((uintptr_t)staging_phys);
        memset(staging_virt, 0, 4096);

        port->staging_phys[i] = staging_phys;
        port->staging_virt[i] = staging_virt;
    }

    port->ci_snapshot = 0;
    port->completed_slots = 0;
    memset(port->event_id, 0, sizeof(port->event_id));
    memset(port->pid, 0, sizeof(port->pid));
    memset(port->submit_tsc, 0, sizeof(port->submit_tsc));

    port->ncq_reads = 0;
    port->ncq_writes = 0;
    port->ncq_errors = 0;
    port->ncq_timeouts = 0;

    port->slot_bitmap = 0xFFFFFFFF;
    port->active = 1;

    spinlock_init(&port->lock);
    memset(&port->stats, 0, sizeof(ahci_port_stats_t));
    port->status = AHCI_PORT_ACTIVE;

    if (ahci_port_start(port) != 0) {
        debug_printf("[AHCI] Port %u: Failed to start\n", port_num);
        for (uint8_t i = 0; i < AHCI_MAX_SLOTS; i++) {
            pmm_free((void*)port->ctba_phys[i], 1);
        }
        pmm_free(clb_page, 1);
        return -1;
    }

    port->signature = port->regs->sig;

    debug_printf("[AHCI] Port %u: Initialized (sig=0x%08x)\n", port_num, port->signature);

    return 0;
}

static int ahci_init_ports(void) {
    if (!ahci_ctrl.initialized) {
        return -1;
    }

    uint32_t pi = ahci_ctrl.hba_mem->pi;
    ahci_ctrl.port_implemented = pi;
    debug_printf("[AHCI] Ports Implemented: 0x%08x\n", pi);

    for (uint8_t i = 0; i < AHCI_MAX_PORTS; i++) {
        if (!(pi & (1U << i))) continue;

        volatile ahci_port_regs_t* regs = ahci_get_port_regs(i);
        uint32_t ssts = regs->ssts;
        uint8_t det = (ssts >> AHCI_SSTS_DET_SHIFT) & AHCI_SSTS_DET_MASK;

        if (det != AHCI_SSTS_DET_PRESENT) {
            debug_printf("[AHCI] Port %u: No device present (SSTS=0x%08x)\n", i, ssts);
            continue;
        }

        debug_printf("[AHCI] Port %u: Device detected, initializing...\n", i);

        if (ahci_port_init(i) == 0) {
            ahci_ctrl.num_active_ports++;
            debug_printf("[AHCI] Port %u: Active\n", i);
        } else {
            debug_printf("[AHCI] Port %u: Initialization failed\n", i);
        }
    }

    return ahci_ctrl.num_active_ports > 0 ? 0 : -1;
}

error_t ahci_port_comreset(ahci_port_t* port) {
    if (!port || !port->regs) {
        return ERR_NULL_POINTER;
    }

    volatile ahci_port_regs_t* regs = port->regs;

    debug_printf("[AHCI] Port %u: Performing COMRESET...\n", port->port_num);

    port->stats.comreset_count++;

    if (ahci_port_stop(port) != 0) {
        debug_printf("[AHCI] Port %u: Failed to stop engine for COMRESET\n", port->port_num);
        port->stats.comreset_fail_count++;
        return ERR_IO;
    }

    regs->serr = 0xFFFFFFFF;

    uint32_t sctl = regs->sctl;
    sctl = (sctl & ~0xF) | 0x1;
    regs->sctl = sctl;

    uint64_t timeout_tsc = rdtsc() + cpu_ms_to_tsc(AHCI_TIMEOUT_COMRESET_WAIT);
    while (rdtsc() < timeout_tsc) {
        cpu_pause();
    }

    sctl = regs->sctl;
    sctl = (sctl & ~0xF);
    regs->sctl = sctl;

    timeout_tsc = rdtsc() + cpu_ms_to_tsc(AHCI_TIMEOUT_COMRESET_WAIT);
    while (rdtsc() < timeout_tsc) {
        uint32_t ssts = regs->ssts;
        uint8_t det = (ssts >> AHCI_SSTS_DET_SHIFT) & AHCI_SSTS_DET_MASK;
        if (det == AHCI_SSTS_DET_PRESENT) {
            debug_printf("[AHCI] Port %u: COMRESET successful (SSTS=0x%08x)\n", port->port_num, ssts);
            regs->serr = 0xFFFFFFFF;
            regs->is = 0xFFFFFFFF;
            if (ahci_port_start(port) == 0) {
                return OK;
            }
            break;
        }
        cpu_pause();
    }

    debug_printf("[AHCI] Port %u: COMRESET failed (timeout)\n", port->port_num);
    port->stats.comreset_fail_count++;
    return ERR_TIMEOUT;
}

error_t ahci_port_recover(ahci_port_t* port) {
    if (!port || !port->regs) {
        return ERR_NULL_POINTER;
    }

    debug_printf("[AHCI] Port %u: Starting error recovery...\n", port->port_num);

    port->stats.error_count++;

    for (int attempt = 0; attempt < AHCI_MAX_COMRESET_ATTEMPTS; attempt++) {
        error_t result = ahci_port_comreset(port);
        if (result == OK) {
            debug_printf("[AHCI] Port %u: Recovery successful (attempt %d/%d)\n",
                         port->port_num, attempt + 1, AHCI_MAX_COMRESET_ATTEMPTS);
            port->status = AHCI_PORT_ACTIVE;
            return OK;
        }

        debug_printf("[AHCI] Port %u: COMRESET attempt %d/%d failed\n",
                     port->port_num, attempt + 1, AHCI_MAX_COMRESET_ATTEMPTS);
    }

    debug_printf("[AHCI] Port %u: Recovery failed after %d attempts\n",
                 port->port_num, AHCI_MAX_COMRESET_ATTEMPTS);
    port->status = AHCI_PORT_ERROR;
    return ERR_IO;
}

const char* ahci_decode_serr(uint32_t serr) {
    if (serr & (1 << 26)) return "Exchanged";
    if (serr & (1 << 25)) return "UnrecognizedFIS";
    if (serr & (1 << 24)) return "TransportStateTrans";
    if (serr & (1 << 23)) return "LinkSeqError";
    if (serr & (1 << 22)) return "HandshakeError";
    if (serr & (1 << 21)) return "CRCError";
    if (serr & (1 << 20)) return "Disparity";
    if (serr & (1 << 19)) return "10bTo8bDecodeError";
    if (serr & (1 << 18)) return "CommWake";
    if (serr & (1 << 17)) return "PhyInternalError";
    if (serr & (1 << 16)) return "PhyRdyChange";
    if (serr & (1 << 11)) return "InternalError";
    if (serr & (1 << 10)) return "ProtocolError";
    if (serr & (1 << 9))  return "PersistentCommError";
    if (serr & (1 << 8))  return "TransientDataError";
    if (serr & (1 << 1))  return "RecoveredCommError";
    if (serr & (1 << 0))  return "RecoveredDataError";
    return "NoError";
}

void ahci_log_error(ahci_port_t* port, uint32_t pxis) {
    if (!port || !port->regs) {
        return;
    }

    volatile ahci_port_regs_t* regs = port->regs;
    uint32_t serr = regs->serr;
    uint32_t tfd = regs->tfd;

    port->stats.last_error_tsc = rdtsc();
    port->stats.last_serr = serr;
    port->stats.last_tfd = tfd;

    if (pxis & AHCI_PIS_TFES) {
        port->stats.tfes_count++;
    }

    debug_printf("[AHCI] Port %u ERROR: IS=0x%08x SERR=0x%08x TFD=0x%08x (%s)\n",
                 port->port_num, pxis, serr, tfd, ahci_decode_serr(serr));
}

void ahci_get_port_stats(uint8_t port_num, ahci_port_stats_t* stats_out) {
    if (!stats_out) return;
    ahci_port_t* port = ahci_get_port(port_num);
    if (!port) return;
    memcpy(stats_out, &port->stats, sizeof(ahci_port_stats_t));
}

int ahci_init(void) {
    debug_printf("[AHCI] Initializing AHCI driver...\n");

    memset(&ahci_ctrl, 0, sizeof(ahci_controller_t));

    if (pci_find_device_by_class(0x01, 0x06, 0x01, &ahci_ctrl.pci_dev) != 0) {
        debug_printf("[AHCI] No AHCI controller found (Class 01:06:01)\n");
        return -1;
    }

    debug_printf("[AHCI] Found controller: %04x:%04x at %02x:%02x.%x\n",
                 ahci_ctrl.pci_dev.vendor_id,
                 ahci_ctrl.pci_dev.device_id,
                 ahci_ctrl.pci_dev.bus,
                 ahci_ctrl.pci_dev.device,
                 ahci_ctrl.pci_dev.function);

    // IRQ line is at PCI config offset 0x3C (BIOS-assigned IRQ / GSI)
    uint8_t irq_line = pci_config_read_byte(ahci_ctrl.pci_dev.bus,
                                             ahci_ctrl.pci_dev.device,
                                             ahci_ctrl.pci_dev.function,
                                             0x3C);
    ahci_ctrl.irq_vector = irq_line;
    ahci_ctrl.irq_enabled = false;
    ahci_ctrl.total_interrupts = 0;

    if (irq_line == 0xFF || irq_line >= IRQ_MAX_COUNT) {
        debug_printf("[AHCI] WARNING: Invalid IRQ line %u, IRQ disabled\n", irq_line);
        ahci_ctrl.irq_vector = 0xFF;  // Mark as invalid
    } else {
        debug_printf("[AHCI] IRQ line: %u\n", ahci_ctrl.irq_vector);
    }

    uint16_t cmd = pci_config_read_word(ahci_ctrl.pci_dev.bus,
                                        ahci_ctrl.pci_dev.device,
                                        ahci_ctrl.pci_dev.function,
                                        PCI_COMMAND);
    cmd |= PCI_CMD_MEM_SPACE;
    pci_config_write_word(ahci_ctrl.pci_dev.bus,
                          ahci_ctrl.pci_dev.device,
                          ahci_ctrl.pci_dev.function,
                          PCI_COMMAND,
                          cmd);

    if (pci_enable_bus_master(&ahci_ctrl.pci_dev) != 0) {
        debug_printf("[AHCI] Failed to enable bus mastering\n");
        return -1;
    }

    uint64_t bar5_full = pci_read_bar64(&ahci_ctrl.pci_dev, 5);
    if (bar5_full == 0 || bar5_full == 0xFFFFFFFF) {
        debug_printf("[AHCI] Invalid BAR5: 0x%lx\n", (unsigned long)bar5_full);
        return -1;
    }

    // Check raw BAR for I/O space bit
    uint32_t bar5_raw = pci_config_read_dword(ahci_ctrl.pci_dev.bus, ahci_ctrl.pci_dev.device,
                                               ahci_ctrl.pci_dev.function, PCI_BAR5);
    if (bar5_raw & 0x1) {
        debug_printf("[AHCI] BAR5 is I/O space (not MMIO)\n");
        return -1;
    }

    ahci_ctrl.hba_phys = bar5_full;
    debug_printf("[AHCI] BAR5: 0x%lx\n", (unsigned long)ahci_ctrl.hba_phys);

    ahci_ctrl.hba_mem = (ahci_hba_mem_t*)vmm_map_mmio(ahci_ctrl.hba_phys,
                                                       4096,
                                                       VMM_FLAGS_KERNEL_RW);
    if (!ahci_ctrl.hba_mem) {
        debug_printf("[AHCI] Failed to map BAR5 MMIO\n");
        return -1;
    }

    debug_printf("[AHCI] BAR5: phys=0x%08lx mapped to virt=0x%p\n",
                 ahci_ctrl.hba_phys, ahci_ctrl.hba_mem);

    ahci_ctrl.cap = ahci_ctrl.hba_mem->cap;

    ahci_ctrl.s64a_support = (ahci_ctrl.cap & AHCI_CAP_S64A) != 0;
    if (ahci_ctrl.s64a_support) {
        debug_printf("[AHCI] 64-bit DMA addressing enabled\n");
    }

    ahci_ctrl.ncq_support = (ahci_ctrl.cap & AHCI_CAP_SNCQ) != 0;
    ahci_ctrl.num_slots = ((ahci_ctrl.cap >> AHCI_CAP_NCS_SHIFT) & AHCI_CAP_NCS_MASK) + 1;

    debug_printf("[AHCI] CAP: NCQ=%s, Slots=%u, S64A=%s\n",
                 ahci_ctrl.ncq_support ? "yes" : "no",
                 ahci_ctrl.num_slots,
                 ahci_ctrl.s64a_support ? "yes" : "no");

    debug_printf("[AHCI] Performing HBA reset...\n");
    ahci_ctrl.hba_mem->ghc |= AHCI_GHC_HR;

    // AHCI spec: controller must complete reset within 1 second
    uint64_t reset_deadline = rdtsc() + cpu_ms_to_tsc(1000);
    while (rdtsc() < reset_deadline) {
        if ((ahci_ctrl.hba_mem->ghc & AHCI_GHC_HR) == 0) {
            break;
        }
        cpu_pause();
    }

    if (ahci_ctrl.hba_mem->ghc & AHCI_GHC_HR) {
        debug_printf("[AHCI] HBA reset timeout\n");
        vmm_unmap_mmio(ahci_ctrl.hba_mem, 4096);
        return -1;
    }

    debug_printf("[AHCI] HBA reset complete\n");

    ahci_ctrl.hba_mem->ghc |= AHCI_GHC_AE;

    if ((ahci_ctrl.hba_mem->ghc & AHCI_GHC_AE) == 0) {
        debug_printf("[AHCI] Failed to enable AHCI mode\n");
        vmm_unmap_mmio(ahci_ctrl.hba_mem, 4096);
        return -1;
    }

    debug_printf("[AHCI] AHCI mode enabled\n");

    ahci_ctrl.initialized = true;

    if (ahci_init_ports() != 0) {
        debug_printf("[AHCI] No active ports found\n");
        vmm_unmap_mmio(ahci_ctrl.hba_mem, 4096);
        ahci_ctrl.initialized = false;
        return -1;
    }

    debug_printf("[AHCI] Initialization complete (%u active port(s))\n", ahci_ctrl.num_active_ports);

    ahci_test_read();

    return 0;
}

void ahci_test_read(void) {
    if (!ahci_ctrl.initialized) {
        debug_printf("[AHCI] Test READ: Not initialized\n");
        return;
    }

    ahci_port_t* port = &ahci_ctrl.ports[0];
    if (!port->active) {
        debug_printf("[AHCI] Test READ: Port 0 not active\n");
        return;
    }

    debug_printf("[AHCI] Test READ: Reading sector 0 (MBR)...\n");

    void* dma_page = pmm_alloc(1);
    if (!dma_page) {
        debug_printf("[AHCI] Test READ: Failed to allocate DMA buffer\n");
        return;
    }

    uintptr_t dma_phys = (uintptr_t)dma_page;
    void* dma_virt = vmm_phys_to_virt(dma_phys);
    memset(dma_virt, 0, 4096);

    if (!ahci_ctrl.s64a_support && !IS_32BIT_SAFE(dma_phys)) {
        debug_printf("[AHCI] Test READ: DMA buffer above 4GB (0x%lx), allocating below 4GB\n", dma_phys);
        pmm_free(dma_page, 1);
        return;
    }

    uint8_t slot = 0;
    port->slot_bitmap &= ~(1 << slot);

    ahci_cmd_header_t* cmdheader = (ahci_cmd_header_t*)port->clb_virt;
    cmdheader[slot].cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmdheader[slot].w = 0;
    cmdheader[slot].prdtl = 1;
    cmdheader[slot].prdbc = 0;

    ahci_cmd_table_t* cmdtbl = (ahci_cmd_table_t*)port->ctba_virt[slot];
    memset(cmdtbl, 0, sizeof(ahci_cmd_table_t));

    cmdtbl->prdt[0].dba = (uint32_t)dma_phys;
    cmdtbl->prdt[0].dbau = (uint32_t)(dma_phys >> 32);
    cmdtbl->prdt[0].dbc = 511;  // 512 - 1 (0-based byte count)
    cmdtbl->prdt[0].i = 1;

    fis_reg_h2d_t* cmdfis = (fis_reg_h2d_t*)&cmdtbl->cfis[0];
    memset(cmdfis, 0, sizeof(fis_reg_h2d_t));

    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c = 1;
    cmdfis->command = ATA_CMD_READ_DMA_EXT;

    cmdfis->lba0 = 0;
    cmdfis->lba1 = 0;
    cmdfis->lba2 = 0;
    cmdfis->lba3 = 0;
    cmdfis->lba4 = 0;
    cmdfis->lba5 = 0;

    cmdfis->device = 1 << 6;  // LBA mode

    cmdfis->countl = 1;
    cmdfis->counth = 0;

    mfence();

    port->regs->ci = 1 << slot;

    // Wait up to 5 seconds for IDENTIFY command to complete
    uint64_t id_deadline = rdtsc() + cpu_ms_to_tsc(5000);
    while (rdtsc() < id_deadline) {
        if ((port->regs->ci & (1 << slot)) == 0) {
            break;
        }
        cpu_pause();
    }

    if (port->regs->ci & (1 << slot)) {
        debug_printf("[AHCI] Test READ: Timeout waiting for command completion\n");
        pmm_free(dma_page, 1);
        return;
    }

    uint32_t tfd = port->regs->tfd;
    if (tfd & AHCI_PTFD_STS_ERR) {
        debug_printf("[AHCI] Test READ: Error bit set in TFD (0x%08x)\n", tfd);
        pmm_free(dma_page, 1);
        return;
    }

    uint8_t* mbr = (uint8_t*)dma_virt;
    uint8_t sig1 = mbr[510];
    uint8_t sig2 = mbr[511];

    if (sig1 == 0x55 && sig2 == 0xAA) {
        debug_printf("[AHCI] Test READ: SUCCESS (MBR signature 0x55AA verified)\n");
        debug_printf("[AHCI] Test READ: First 16 bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                     mbr[0], mbr[1], mbr[2], mbr[3], mbr[4], mbr[5], mbr[6], mbr[7],
                     mbr[8], mbr[9], mbr[10], mbr[11], mbr[12], mbr[13], mbr[14], mbr[15]);
    } else {
        debug_printf("[AHCI] Test READ: FAILED (expected 0x55AA, got 0x%02x%02x)\n", sig1, sig2);
    }

    port->slot_bitmap |= (1 << slot);
    pmm_free(dma_page, 1);
}

uint8_t ahci_get_active_port_count(void) {
    return ahci_ctrl.num_active_ports;
}

uint32_t ahci_get_active_port_mask(void) {
    if (!ahci_ctrl.initialized) return 0;
    uint32_t mask = 0;
    for (uint8_t i = 0; i < AHCI_MAX_PORTS; i++) {
        if (ahci_ctrl.ports[i].active) {
            mask |= (1U << i);
        }
    }
    return mask;
}

bool ahci_is_initialized(void) {
    return ahci_ctrl.initialized;
}
