#include "klib.h"
#include "ahci.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "io.h"
#include "irqchip.h"
#include "atomics.h"
#include "error.h"
#include "pic.h"
#include "cpu_calibrate.h"
#include "boxos_memory.h"
#include "idt.h"
#include "amp.h"

#define AHCI_TIMEOUT_MS CONFIG_AHCI_CMD_TIMEOUT_MS

static ahci_controller_t ahci_ctrl;

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

static void ahci_deferred_recover(void* ctx) {
    ahci_port_t* port = (ahci_port_t*)ctx;
    ahci_port_recover(port);
    __atomic_store_n(&port->recovering, 0, __ATOMIC_RELEASE);
}

static inline void ahci_retire_slots(ahci_port_t* state, uint8_t port_idx,
                                     uint32_t mask, error_t status) {
    while (mask) {
        uint8_t slot = (uint8_t)__builtin_ctz(mask);
        mask &= mask - 1;
        void (*cb)(uint8_t, uint8_t, error_t, void*) = state->cb[slot];
        void *ctx = state->cb_ctx[slot];
        state->cb[slot]     = NULL;
        state->cb_ctx[slot] = NULL;
        ahci_free_slot(port_idx, slot);
        if (cb) {
            cb(port_idx, slot, status, ctx);
        }
    }
}

static inline uint32_t ahci_claim(ahci_port_t* state, uint32_t candidate) {
    return __sync_fetch_and_and(&state->issued_mask, ~candidate) & candidate;
}

void ahci_irq_handler(void) {
    if (!ahci_ctrl.initialized) {
        return;
    }

    uint32_t is = __atomic_load_n(&ahci_ctrl.hba_mem->is, __ATOMIC_ACQUIRE);
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

        port->is = port_is;

        enum { AHCI_DRAIN_MAX = 4096 };
        for (uint32_t pass = 0; pass < AHCI_DRAIN_MAX; pass++) {
            uint32_t snapshot    = __atomic_load_n(&state->issued_mask, __ATOMIC_ACQUIRE);
            uint32_t outstanding = state->ncq
                ? __atomic_load_n(&port->sact, __ATOMIC_ACQUIRE)
                : __atomic_load_n(&port->ci,   __ATOMIC_ACQUIRE);
            uint32_t completed   = snapshot & ~outstanding;
            if (!completed) break;
            uint32_t won = ahci_claim(state, completed);
            if (!won) continue;
            __sync_fetch_and_or(&state->completed_slots, won);
            ahci_retire_slots(state, i, won, OK);
        }

        if (port_is & (AHCI_PIS_TFES | AHCI_PIS_HBFS | AHCI_PIS_IFS)) {
            __sync_fetch_and_add(&state->ncq_errors, 1);
            __atomic_fetch_add(&state->stats.tfes_count, 1, __ATOMIC_RELAXED);
            __atomic_store_n(&state->stats.last_tfd,       port->tfd,  __ATOMIC_RELAXED);
            __atomic_store_n(&state->stats.last_serr,      port->serr, __ATOMIC_RELAXED);
            __atomic_store_n(&state->stats.last_error_tsc, rdtsc(),    __ATOMIC_RELAXED);

            uint32_t stuck = __atomic_load_n(&state->issued_mask, __ATOMIC_ACQUIRE);
            if (stuck) {
                uint32_t won = ahci_claim(state, stuck);
                if (won) ahci_retire_slots(state, i, won, ERR_IO);
            }

            if (g_amp.total_cores > 1 &&
                __sync_bool_compare_and_swap(&state->recovering, 0, 1)) {
                BatonPass(&state->recover_node);
            }
        }
    }

    ahci_ctrl.hba_mem->is = is;
}

void ahci_watchdog_scan(void) {
    if (!ahci_ctrl.initialized || g_amp.total_cores <= 1) return;

    uint64_t now         = rdtsc();
    uint64_t overdue_tsc = cpu_ms_to_tsc(CONFIG_AHCI_IO_TIMEOUT_MS);

    for (uint8_t i = 0; i < AHCI_MAX_PORTS; i++) {
        ahci_port_t* state = &ahci_ctrl.ports[i];
        if (!state->active) continue;

        uint32_t snapshot = __atomic_load_n(&state->issued_mask, __ATOMIC_ACQUIRE);
        if (snapshot == 0) continue;

        volatile ahci_port_regs_t* regs = ahci_get_port_regs(i);

        uint32_t pxis = __atomic_load_n(&regs->is, __ATOMIC_ACQUIRE);
        bool port_error = (pxis & (AHCI_PIS_TFES | AHCI_PIS_HBFS | AHCI_PIS_IFS)) != 0;

        if (!port_error) {
            uint32_t outstanding = state->ncq
                ? __atomic_load_n(&regs->sact, __ATOMIC_ACQUIRE)
                : __atomic_load_n(&regs->ci,   __ATOMIC_ACQUIRE);
            uint32_t completed = snapshot & ~outstanding;
            if (completed) {
                uint32_t won = ahci_claim(state, completed);
                if (won) {
                    __sync_fetch_and_or(&state->completed_slots, won);
                    debug_printf("[AHCI] watchdog: reconciled lost completion, port %u slots 0x%08x\n",
                                 i, won);
                    ahci_retire_slots(state, i, won, OK);
                }
            }
        }

        spin_lock(&state->lock);
        uint32_t still   = __atomic_load_n(&state->issued_mask, __ATOMIC_ACQUIRE);
        uint32_t overdue = 0;
        for (uint32_t m = still; m; m &= m - 1) {
            uint8_t s = (uint8_t)__builtin_ctz(m);
            if (now - state->submit_tsc[s] > overdue_tsc) overdue |= (1U << s);
        }
        uint32_t won = (overdue || port_error) ? ahci_claim(state, still) : 0;
        spin_unlock(&state->lock);

        if (won) {
            __atomic_store_n(&state->stats.last_error_tsc, now, __ATOMIC_RELAXED);
            kprintf("[AHCI] port %u %s — failing slots 0x%08x and resetting "
                    "the link\n", i,
                    port_error ? "reported a fatal error (lost MSI)"
                               : "went past its deadline", won);
            ahci_retire_slots(state, i, won, ERR_IO);
            if (__sync_bool_compare_and_swap(&state->recovering, 0, 1)) {
                BatonPass(&state->recover_node);
            }
        }
    }
}

void ahci_port_enable_irq(uint8_t port_num) {
    ahci_port_t* port = ahci_get_port(port_num);
    if (!port) return;

    volatile ahci_port_regs_t* regs = port->regs;

    regs->ie = AHCI_PIS_DHRS |
               AHCI_PIS_PSS  |
               AHCI_PIS_DSS  |
               AHCI_PIS_SDBS |
               AHCI_PIS_HBFS |
               AHCI_PIS_TFES;

    ahci_ctrl.hba_mem->ghc |= AHCI_GHC_IE;

    debug_printf("[AHCI] Port %u: Interrupts enabled (IE=0x%08x)\n", port_num, regs->ie);
}

void ahci_init_irq(void) {
    if (!ahci_ctrl.initialized) {
        return;
    }

    int msi = pci_msi_enable(ahci_ctrl.pci_dev.bus,
                             ahci_ctrl.pci_dev.device,
                             ahci_ctrl.pci_dev.function,
                             AHCI_MSI_VECTOR,
                             g_amp.bsp_lapic_id);
    if (msi == 0) {
        ahci_ctrl.irq_vector  = AHCI_MSI_VECTOR;
        ahci_ctrl.irq_enabled = true;
        debug_printf("[AHCI] MSI enabled (vector 0x%02x -> LAPIC %u)\n",
                     AHCI_MSI_VECTOR, g_amp.bsp_lapic_id);
        return;
    }

    if (ahci_ctrl.irq_vector == 0xFF || ahci_ctrl.irq_vector >= IRQ_MAX_COUNT) {
        debug_printf("[AHCI] No MSI capability and no usable INTx line; polling mode\n");
        ahci_ctrl.irq_enabled = false;
        return;
    }

    irq_register_handler(ahci_ctrl.irq_vector, ahci_irq_handler);
    irqchip_enable_irq(ahci_ctrl.irq_vector);
    ahci_ctrl.irq_enabled = true;
    debug_printf("[AHCI] INTx IRQ %u registered (MSI unavailable)\n", ahci_ctrl.irq_vector);
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
            port->cb[s]     = NULL;
            port->cb_ctx[s] = NULL;
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
    return __atomic_load_n(&port->slot_bitmap, __ATOMIC_RELAXED) != 0;
}

error_t ahci_build_io(uint8_t port_num, uint8_t slot, uint64_t lba,
                      uint16_t sector_count, void* buffer_phys, bool write) {
    if (slot >= AHCI_MAX_SLOTS) return ERR_INVALID_ARGUMENT;
    ahci_port_t* port = ahci_get_port(port_num);
    if (!port) return ERR_DEVICE_NOT_READY;

    if (!ahci_ctrl.s64a_support && ((uintptr_t)buffer_phys >> 32)) {
        return ERR_INVALID_ADDRESS;
    }

    ahci_cmd_header_t* cmdheader = (ahci_cmd_header_t*)port->clb_virt;
    cmdheader[slot].cfl   = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmdheader[slot].w     = write ? 1 : 0;
    cmdheader[slot].prdtl = 1;
    cmdheader[slot].prdbc = 0;

    ahci_cmd_table_t* cmdtbl = (ahci_cmd_table_t*)port->ctba_virt[slot];
    memset(cmdtbl, 0, sizeof(ahci_cmd_table_t));

    cmdtbl->prdt[0].dba  = (uint32_t)(uintptr_t)buffer_phys;
    cmdtbl->prdt[0].dbau = (uint32_t)((uintptr_t)buffer_phys >> 32);
    cmdtbl->prdt[0].dbc  = (sector_count * 512u) - 1u;
    cmdtbl->prdt[0].i    = 1;

    fis_reg_h2d_t* cmdfis = (fis_reg_h2d_t*)&cmdtbl->cfis[0];
    memset(cmdfis, 0, sizeof(fis_reg_h2d_t));

    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c        = 1;
    cmdfis->device   = (1 << 6);

    cmdfis->lba0 = (lba >>  0) & 0xFF;
    cmdfis->lba1 = (lba >>  8) & 0xFF;
    cmdfis->lba2 = (lba >> 16) & 0xFF;
    cmdfis->lba3 = (lba >> 24) & 0xFF;
    cmdfis->lba4 = (lba >> 32) & 0xFF;
    cmdfis->lba5 = (lba >> 40) & 0xFF;

    if (port->ncq) {
        cmdfis->command  = write ? ATA_CMD_WRITE_FPDMA_QUEUED
                                 : ATA_CMD_READ_FPDMA_QUEUED;
        cmdfis->featurel = sector_count & 0xFF;
        cmdfis->featureh = (sector_count >> 8) & 0xFF;
        cmdfis->countl   = (uint8_t)(slot << 3);
        cmdfis->counth   = 0;
    } else {
        cmdfis->command  = write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
        cmdfis->countl   = sector_count & 0xFF;
        cmdfis->counth   = (sector_count >> 8) & 0xFF;
    }

    return OK;
}

void ahci_arm_slot(uint8_t port_num, uint8_t slot) {
    ahci_port_t* port = ahci_get_port(port_num);
    if (!port || slot >= AHCI_MAX_SLOTS) return;
    volatile ahci_port_regs_t* regs = port->regs;

    spin_lock(&port->lock);
    if (port->ncq) {
        regs->sact = (1U << slot);
    }
    regs->ci = (1U << slot);
    port->submit_tsc[slot] = rdtsc();
    __sync_fetch_and_or(&port->issued_mask, (1U << slot));
    spin_unlock(&port->lock);
}

bool ahci_port_is_ncq(uint8_t port_num) {
    ahci_port_t* port = ahci_get_port(port_num);
    return port ? port->ncq : false;
}

static error_t ahci_port_identify(ahci_port_t* port, uint16_t* id_out) {
    volatile ahci_port_regs_t* regs = port->regs;

    void* buf_page = pmm_alloc(1, PHYS_TAG_DMA32);
    if (!buf_page) {
        return ERR_NO_MEMORY;
    }
    uintptr_t buf_phys = (uintptr_t)buf_page;
    void* buf_virt = vmm_phys_to_virt(buf_phys);
    memset(buf_virt, 0, 512);

    ahci_cmd_header_t* cmdheader = (ahci_cmd_header_t*)port->clb_virt;
    memset(&cmdheader[0], 0, sizeof(ahci_cmd_header_t));
    cmdheader[0].cfl   = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmdheader[0].w     = 0;
    cmdheader[0].prdtl = 1;
    cmdheader[0].ctba  = (uint32_t)port->ctba_phys[0];
    cmdheader[0].ctbau = (uint32_t)(port->ctba_phys[0] >> 32);

    ahci_cmd_table_t* cmdtbl = (ahci_cmd_table_t*)port->ctba_virt[0];
    memset(cmdtbl, 0, sizeof(ahci_cmd_table_t));
    cmdtbl->prdt[0].dba  = (uint32_t)buf_phys;
    cmdtbl->prdt[0].dbau = (uint32_t)(buf_phys >> 32);
    cmdtbl->prdt[0].dbc  = 512 - 1;
    cmdtbl->prdt[0].i    = 0;

    fis_reg_h2d_t* cmdfis = (fis_reg_h2d_t*)&cmdtbl->cfis[0];
    memset(cmdfis, 0, sizeof(fis_reg_h2d_t));
    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c        = 1;
    cmdfis->command  = ATA_CMD_IDENTIFY_DEVICE;
    cmdfis->device   = 0;

    mfence();
    regs->ci = (1U << 0);
    mfence();

    error_t result = ERR_TIMEOUT;
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(AHCI_TIMEOUT_CMD_DEFAULT);
    while (rdtsc() < deadline) {
        if ((regs->ci & (1U << 0)) == 0) {
            result = OK;
            break;
        }
        if (regs->is & (AHCI_PIS_TFES | AHCI_PIS_HBFS | AHCI_PIS_IFS)) {
            result = ERR_IO;
            break;
        }
        cpu_pause();
    }

    if (result == OK && (regs->tfd & AHCI_PTFD_STS_ERR)) {
        result = ERR_IO;
    }
    if (result == OK) {
        memcpy(id_out, buf_virt, 512);
    }

    uint32_t isr = regs->is;
    regs->is = isr;
    pmm_free(buf_page, 1);
    return result;
}

static int ahci_port_init(uint8_t port_num) {
    if (port_num >= AHCI_MAX_PORTS) {
        return -1;
    }

    ahci_port_t* port = &ahci_ctrl.ports[port_num];
    memset(port, 0, sizeof(ahci_port_t));

    port->port_num = port_num;
    port->regs = ahci_get_port_regs(port_num);
    spinlock_init(&port->lock);

    port->recover_node.run = ahci_deferred_recover;
    port->recover_node.ctx = port;

    port->status = AHCI_PORT_FAILED;

    volatile ahci_port_regs_t* regs = port->regs;

    if (ahci_port_stop(port) != 0) {
        kprintf("[AHCI] port %u: would not stop, so it cannot be set up — "
                "no disk on it\n", port_num);
        return -1;
    }

    void* clb_page = pmm_alloc(1, PHYS_TAG_DMA32);
    if (!clb_page) {
        kprintf("[AHCI] port %u: no DMA32 page for its command list — "
                "no disk on it\n", port_num);
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
        void* ctba_page = pmm_alloc(1, PHYS_TAG_DMA32);
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
        cmdheader[slot].ctba  = (uint32_t)port->ctba_phys[slot];
        cmdheader[slot].ctbau = (uint32_t)(port->ctba_phys[slot] >> 32);
    }

    regs->clb  = (uint32_t)port->clb_phys;
    regs->clbu = (uint32_t)(port->clb_phys >> 32);
    regs->fb   = (uint32_t)port->fis_phys;
    regs->fbu  = (uint32_t)(port->fis_phys >> 32);

    regs->is   = 0xFFFFFFFF;
    regs->serr = 0xFFFFFFFF;

    regs->cmd |= AHCI_PCMD_FRE;

    if (ahci_ctrl.cap & AHCI_CAP_SSS) {
        uint32_t cmd = regs->cmd;
        cmd |= AHCI_PCMD_SUD | AHCI_PCMD_POD;
        cmd  = (cmd & ~((uint32_t)AHCI_PCMD_ICC_MASK << AHCI_PCMD_ICC_SHIFT))
               | ((uint32_t)AHCI_PCMD_ICC_ACTIVE << AHCI_PCMD_ICC_SHIFT);
        regs->cmd = cmd;
        mfence();
    }

    uint8_t det = 0;
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(1000);
    while (rdtsc() < deadline) {
        det = (regs->ssts >> AHCI_SSTS_DET_SHIFT) & AHCI_SSTS_DET_MASK;
        if (det == AHCI_SSTS_DET_PRESENT) {
            break;
        }
        cpu_pause();
    }
    if (det != AHCI_SSTS_DET_PRESENT) {
        debug_printf("[AHCI] Port %u: no device after spin-up (DET=%u)\n", port_num, det);
        goto fail_free;
    }

    regs->serr = 0xFFFFFFFF;

    deadline = rdtsc() + cpu_ms_to_tsc(1000);
    while (rdtsc() < deadline) {
        if ((regs->tfd & (AHCI_PTFD_STS_BSY | AHCI_PTFD_STS_DRQ)) == 0) {
            break;
        }
        cpu_pause();
    }
    if (regs->tfd & (AHCI_PTFD_STS_BSY | AHCI_PTFD_STS_DRQ)) {
        debug_printf("[AHCI] Port %u: device not ready (TFD=0x%08x)\n", port_num, regs->tfd);
        goto fail_free;
    }

    regs->cmd |= AHCI_PCMD_ST;
    mfence();

    port->signature = regs->sig;

    if (port->signature != AHCI_SIG_ATA) {
        debug_printf("[AHCI] Port %u: non-ATA device (sig=0x%08x), skipping\n",
                     port_num, port->signature);
        ahci_port_stop(port);
        goto fail_free;
    }

    uint16_t id[256];
    if (ahci_port_identify(port, id) != OK) {
        debug_printf("[AHCI] Port %u: IDENTIFY DEVICE failed\n", port_num);
        ahci_port_stop(port);
        goto fail_free;
    }

    uint32_t logical = 512;
    if ((id[106] & (1u << 14)) && !(id[106] & (1u << 15)) && (id[106] & (1u << 12))) {
        uint32_t words = (uint32_t)id[117] | ((uint32_t)id[118] << 16);
        if (words >= 256) {
            logical = words * 2u;
        }
    }
    port->logical_sector_size = logical;

    port->physical_sector_size = logical;
    if ((id[106] & (1u << 14)) && !(id[106] & (1u << 15)) && (id[106] & (1u << 13))) {
        uint32_t exponent = id[106] & 0x0Fu;
        if (exponent < 16 && logical <= (0xFFFFFFFFu >> exponent)) {
            port->physical_sector_size = logical << exponent;
        }
    }

    port->lba48 = (id[83] & (1u << 10)) != 0;
    if (port->lba48) {
        uint64_t s = 0;
        memcpy(&s, &id[100], sizeof(uint64_t));
        port->total_sectors = s;
    } else {
        uint32_t s = 0;
        memcpy(&s, &id[60], sizeof(uint32_t));
        port->total_sectors = s;
    }

    bool dev_ncq = (id[76] != 0x0000 && id[76] != 0xFFFF) && (id[76] & (1u << 8));
    port->ncq = ahci_ctrl.ncq_support && dev_ncq;

    if (port->logical_sector_size != 512) {
        kprintf("[AHCI] port %u: %u-byte logical sectors, and this stack is "
                "built on 512 — not using it\n",
                port_num, port->logical_sector_size);
        ahci_port_stop(port);
        goto fail_free;
    }

    uint32_t usable = (ahci_ctrl.num_slots >= 32)
                      ? 0xFFFFFFFFu
                      : ((1u << ahci_ctrl.num_slots) - 1u);
    port->slot_bitmap = port->ncq ? usable : 0x1u;

    port->status = AHCI_PORT_ACTIVE;
    port->active = 1;

    debug_printf("[AHCI] Port %u: ATA disk, %llu sectors, sec=%uB, LBA%s, NCQ=%s (sig=0x%08x)\n",
                 port_num, (unsigned long long)port->total_sectors,
                 port->logical_sector_size, port->lba48 ? "48" : "28",
                 port->ncq ? "yes" : "no", port->signature);

    return 0;

fail_free:
    for (uint8_t i = 0; i < AHCI_MAX_SLOTS; i++) {
        if (port->ctba_phys[i]) {
            pmm_free((void*)port->ctba_phys[i], 1);
        }
    }
    pmm_free(clb_page, 1);
    return -1;
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

        if (!(ahci_ctrl.cap & AHCI_CAP_SSS)) {
            volatile ahci_port_regs_t* regs = ahci_get_port_regs(i);
            uint8_t det = (regs->ssts >> AHCI_SSTS_DET_SHIFT) & AHCI_SSTS_DET_MASK;
            if (det != AHCI_SSTS_DET_PRESENT) {
                debug_printf("[AHCI] Port %u: No device present (DET=%u)\n", i, det);
                continue;
            }
        }

        debug_printf("[AHCI] Port %u: bringing up...\n", i);

        if (ahci_port_init(i) == 0) {
            ahci_ctrl.num_active_ports++;
            debug_printf("[AHCI] Port %u: Active\n", i);
        } else {
            debug_printf("[AHCI] Port %u: not active\n", i);
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

    __atomic_fetch_add(&port->stats.comreset_count, 1, __ATOMIC_RELAXED);

    if (ahci_port_stop(port) != 0) {
        debug_printf("[AHCI] Port %u: Failed to stop engine for COMRESET\n", port->port_num);
        __atomic_fetch_add(&port->stats.comreset_fail_count, 1, __ATOMIC_RELAXED);
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
    __atomic_fetch_add(&port->stats.comreset_fail_count, 1, __ATOMIC_RELAXED);
    return ERR_TIMEOUT;
}

error_t ahci_port_recover(ahci_port_t* port) {
    if (!port || !port->regs) {
        return ERR_NULL_POINTER;
    }

    debug_printf("[AHCI] Port %u: Starting error recovery...\n", port->port_num);

    __atomic_fetch_add(&port->stats.error_count, 1, __ATOMIC_RELAXED);

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

    __atomic_store_n(&port->stats.last_error_tsc, rdtsc(), __ATOMIC_RELAXED);
    __atomic_store_n(&port->stats.last_serr,      serr,    __ATOMIC_RELAXED);
    __atomic_store_n(&port->stats.last_tfd,       tfd,     __ATOMIC_RELAXED);

    if (pxis & AHCI_PIS_TFES) {
        __atomic_fetch_add(&port->stats.tfes_count, 1, __ATOMIC_RELAXED);
    }

    debug_printf("[AHCI] Port %u ERROR: IS=0x%08x SERR=0x%08x TFD=0x%08x (%s)\n",
                 port->port_num, pxis, serr, tfd, ahci_decode_serr(serr));
}

void ahci_get_port_stats(uint8_t port_num, ahci_port_stats_t* stats_out) {
    if (!stats_out) return;
    ahci_port_t* port = ahci_get_port(port_num);
    if (!port) return;

    stats_out->cmd_count           = __atomic_load_n(&port->stats.cmd_count,           __ATOMIC_RELAXED);
    stats_out->error_count         = __atomic_load_n(&port->stats.error_count,         __ATOMIC_RELAXED);
    stats_out->timeout_count       = __atomic_load_n(&port->stats.timeout_count,       __ATOMIC_RELAXED);
    stats_out->tfes_count          = __atomic_load_n(&port->stats.tfes_count,          __ATOMIC_RELAXED);
    stats_out->comreset_count      = __atomic_load_n(&port->stats.comreset_count,      __ATOMIC_RELAXED);
    stats_out->comreset_fail_count = __atomic_load_n(&port->stats.comreset_fail_count, __ATOMIC_RELAXED);
    stats_out->last_error_tsc      = __atomic_load_n(&port->stats.last_error_tsc,      __ATOMIC_RELAXED);
    stats_out->last_serr           = __atomic_load_n(&port->stats.last_serr,           __ATOMIC_RELAXED);
    stats_out->last_tfd            = __atomic_load_n(&port->stats.last_tfd,            __ATOMIC_RELAXED);
}

static void ahci_bios_handoff(void) {
    if (!(ahci_ctrl.hba_mem->cap2 & AHCI_CAP2_BOH)) {
        return;
    }

    debug_printf("[AHCI] BIOS/OS handoff: requesting OS ownership...\n");

    ahci_ctrl.hba_mem->bohc |= AHCI_BOHC_OOS;
    mfence();

    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(1000);
    while (rdtsc() < deadline) {
        if ((ahci_ctrl.hba_mem->bohc & AHCI_BOHC_BOS) == 0) {
            break;
        }
        cpu_pause();
    }

    if (ahci_ctrl.hba_mem->bohc & AHCI_BOHC_BOS) {
        debug_printf("[AHCI] WARNING: BIOS did not release HBA within 1s (continuing)\n");
    }

    if (ahci_ctrl.hba_mem->bohc & AHCI_BOHC_BB) {
        debug_printf("[AHCI] BIOS busy after handoff; waiting up to 2s...\n");
        deadline = rdtsc() + cpu_ms_to_tsc(2000);
        while (rdtsc() < deadline) {
            if ((ahci_ctrl.hba_mem->bohc & AHCI_BOHC_BB) == 0) {
                break;
            }
            cpu_pause();
        }
    }

    debug_printf("[AHCI] BIOS/OS handoff complete (BOHC=0x%08x)\n",
                 ahci_ctrl.hba_mem->bohc);
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

    uint8_t irq_line = pci_config_read_byte(ahci_ctrl.pci_dev.bus,
                                             ahci_ctrl.pci_dev.device,
                                             ahci_ctrl.pci_dev.function,
                                             0x3C);
    ahci_ctrl.irq_vector = irq_line;
    ahci_ctrl.irq_enabled = false;
    ahci_ctrl.total_interrupts = 0;

    if (irq_line == 0xFF || irq_line >= IRQ_MAX_COUNT) {
        debug_printf("[AHCI] WARNING: Invalid IRQ line %u, IRQ disabled\n", irq_line);
        ahci_ctrl.irq_vector = 0xFF;
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

    uint32_t bar5_raw = pci_config_read_dword(ahci_ctrl.pci_dev.bus, ahci_ctrl.pci_dev.device,
                                               ahci_ctrl.pci_dev.function, PCI_BAR5);
    if (bar5_raw & 0x1) {
        debug_printf("[AHCI] BAR5 is I/O space (not MMIO)\n");
        return -1;
    }

    ahci_ctrl.hba_phys = bar5_full;
    debug_printf("[AHCI] BAR5: 0x%lx\n", (unsigned long)ahci_ctrl.hba_phys);

    ahci_ctrl.hba_mem = (ahci_hba_mem_t*)vmm_map_mmio(ahci_ctrl.hba_phys,
                                                       AHCI_ABAR_SPAN,
                                                       VMM_FLAGS_KERNEL_RW);
    if (!ahci_ctrl.hba_mem) {
        debug_printf("[AHCI] Failed to map BAR5 MMIO\n");
        return -1;
    }

    debug_printf("[AHCI] BAR5: phys=0x%08lx mapped to virt=0x%p (span=0x%x)\n",
                 ahci_ctrl.hba_phys, ahci_ctrl.hba_mem, (unsigned)AHCI_ABAR_SPAN);

    ahci_ctrl.cap = ahci_ctrl.hba_mem->cap;

    ahci_bios_handoff();

    ahci_ctrl.hba_mem->ghc |= AHCI_GHC_AE;
    mfence();

    debug_printf("[AHCI] Performing HBA reset...\n");
    ahci_ctrl.hba_mem->ghc |= AHCI_GHC_HR;
    mfence();

    uint64_t reset_deadline = rdtsc() + cpu_ms_to_tsc(1000);
    while (rdtsc() < reset_deadline) {
        if ((ahci_ctrl.hba_mem->ghc & AHCI_GHC_HR) == 0) {
            break;
        }
        cpu_pause();
    }

    if (ahci_ctrl.hba_mem->ghc & AHCI_GHC_HR) {
        debug_printf("[AHCI] HBA reset timeout\n");
        vmm_unmap_mmio(ahci_ctrl.hba_mem, AHCI_ABAR_SPAN);
        return -1;
    }

    debug_printf("[AHCI] HBA reset complete\n");

    ahci_ctrl.hba_mem->ghc |= AHCI_GHC_AE;
    mfence();

    if ((ahci_ctrl.hba_mem->ghc & AHCI_GHC_AE) == 0) {
        debug_printf("[AHCI] Failed to enable AHCI mode\n");
        vmm_unmap_mmio(ahci_ctrl.hba_mem, AHCI_ABAR_SPAN);
        return -1;
    }

    debug_printf("[AHCI] AHCI mode enabled\n");

    ahci_ctrl.s64a_support = (ahci_ctrl.cap & AHCI_CAP_S64A) != 0;
    ahci_ctrl.ncq_support  = (ahci_ctrl.cap & AHCI_CAP_SNCQ) != 0;
    ahci_ctrl.num_slots    = ((ahci_ctrl.cap >> AHCI_CAP_NCS_SHIFT) & AHCI_CAP_NCS_MASK) + 1;

    debug_printf("[AHCI] CAP: NCQ=%s, Slots=%u, S64A=%s, CAP2=0x%08x\n",
                 ahci_ctrl.ncq_support ? "yes" : "no",
                 ahci_ctrl.num_slots,
                 ahci_ctrl.s64a_support ? "yes" : "no",
                 ahci_ctrl.hba_mem->cap2);

    ahci_ctrl.initialized = true;

    if (ahci_init_ports() != 0) {
        debug_printf("[AHCI] No active ports found\n");
        vmm_unmap_mmio(ahci_ctrl.hba_mem, AHCI_ABAR_SPAN);
        ahci_ctrl.initialized = false;
        return -1;
    }

    debug_printf("[AHCI] Initialization complete (%u active port(s))\n", ahci_ctrl.num_active_ports);

#if defined(CONFIG_AHCI_SELFTEST) && CONFIG_AHCI_SELFTEST
    ahci_test_read();
#endif

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

    void* dma_page = pmm_alloc(1, PHYS_TAG_DMA32);
    if (!dma_page) {
        debug_printf("[AHCI] Test READ: Failed to allocate DMA buffer\n");
        return;
    }

    uintptr_t dma_phys = (uintptr_t)dma_page;
    void* dma_virt = vmm_phys_to_virt(dma_phys);
    memset(dma_virt, 0, 4096);

    uint8_t slot = 0;
    spin_lock(&port->lock);
    port->slot_bitmap &= ~(1 << slot);
    spin_unlock(&port->lock);

    ahci_cmd_header_t* cmdheader = (ahci_cmd_header_t*)port->clb_virt;
    cmdheader[slot].cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmdheader[slot].w = 0;
    cmdheader[slot].prdtl = 1;
    cmdheader[slot].prdbc = 0;

    ahci_cmd_table_t* cmdtbl = (ahci_cmd_table_t*)port->ctba_virt[slot];
    memset(cmdtbl, 0, sizeof(ahci_cmd_table_t));

    cmdtbl->prdt[0].dba = (uint32_t)dma_phys;
    cmdtbl->prdt[0].dbau = (uint32_t)(dma_phys >> 32);
    cmdtbl->prdt[0].dbc = 511;
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

    cmdfis->device = 1 << 6;

    cmdfis->countl = 1;
    cmdfis->counth = 0;

    mfence();

    port->regs->ci = 1 << slot;

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

    spin_lock(&port->lock);
    port->slot_bitmap |= (1 << slot);
    spin_unlock(&port->lock);
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