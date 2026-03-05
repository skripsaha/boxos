#include "ata_dma.h"
#include "pci.h"
#include "pmm.h"
#include "io.h"
#include "klib.h"
#include "atomics.h"
#include "ata.h"
#include "guide.h"
#include "cpu_calibrate.h"
#include "kernel_config.h"
#include "boxos_memory.h"
#include "async_io.h"
#include "storage_deck.h"
#include "tagfs.h"

#if CONFIG_ATA_DMA_DEBUG
    #define ATA_DMA_DEBUG(...) debug_printf(__VA_ARGS__)
#else
    #define ATA_DMA_DEBUG(...) ((void)0)
#endif

static ata_dma_state_t dma_state;

static __attribute__((aligned(4096), section(".bss"))) uint8_t prd_table_static[4096];

static int ata_dma_setup_prd(ata_dma_request_t* req) {
    if (!req || !req->buffer_phys) {
        return -1;
    }

    ata_prd_entry_t* prd = (ata_prd_entry_t*)dma_state.prd_table_virt;
    uint32_t bytes_remaining = req->sector_count * ATA_SECTOR_SIZE;
    uintptr_t phys_addr = req->buffer_phys;
    uint8_t entry_count = 0;

    while (bytes_remaining > 0 && entry_count < ATA_DMA_PRD_MAX_ENTRIES) {
        uint32_t chunk_size = bytes_remaining;
        if (chunk_size > 65536) {
            chunk_size = 65536;
        }

        uintptr_t boundary_check = (phys_addr + chunk_size - 1) ^ phys_addr;
        if (boundary_check >= 65536) {
            chunk_size = 65536 - (phys_addr & 0xFFFF);
        }

        prd[entry_count].phys_addr = (uint32_t)phys_addr;
        prd[entry_count].byte_count = (uint16_t)chunk_size;
        prd[entry_count].flags = 0;

        phys_addr += chunk_size;
        bytes_remaining -= chunk_size;
        entry_count++;
    }

    if (bytes_remaining > 0) {
        debug_printf("[ATA DMA] ERROR: Transfer too large for PRD table\n");
        return -1;
    }

    if (entry_count > 0) {
        prd[entry_count - 1].flags = ATA_PRD_EOT;
    }

    mfence();

    return 0;
}

static int ata_dma_send_command(ata_dma_request_t* req) {
    uint16_t base = dma_state.bus_master_base;

    outb(base + 0, ATA_DMA_CMD_STOP);

    uint32_t prd_phys_low = (uint32_t)dma_state.prd_table_phys;
    outl(base + 4, prd_phys_low);

    outb(base + 2, ATA_DMA_STATUS_IRQ | ATA_DMA_STATUS_ERROR);

    uint8_t drive_select = req->is_master ? ATA_DRIVE_MASTER : ATA_DRIVE_SLAVE;
    drive_select |= ((req->lba >> 24) & 0x0F);
    outb(ATA_PRIMARY_DRIVE, drive_select);

    for (int i = 0; i < 4; i++) {
        inb(ATA_PRIMARY_ALTSTATUS);
    }

    outb(ATA_PRIMARY_SECCOUNT, req->sector_count);
    outb(ATA_PRIMARY_LBA_LO, req->lba & 0xFF);
    outb(ATA_PRIMARY_LBA_MID, (req->lba >> 8) & 0xFF);
    outb(ATA_PRIMARY_LBA_HI, (req->lba >> 16) & 0xFF);

    if (req->is_write) {
        outb(ATA_PRIMARY_COMMAND, ATA_CMD_WRITE_DMA);
        outb(base + 0, ATA_DMA_CMD_START);
    } else {
        outb(ATA_PRIMARY_COMMAND, ATA_CMD_READ_DMA);
        outb(base + 0, ATA_DMA_CMD_START | ATA_DMA_CMD_READ);
    }

    return 0;
}

int ata_dma_init(void) {
    debug_printf("[ATA DMA] Initializing DMA subsystem...\n");

    memset(&dma_state, 0, sizeof(dma_state));
    spinlock_init(&dma_state.lock);

    pci_device_t ide_controller;
    if (pci_find_device_by_class(PCI_CLASS_STORAGE, PCI_SUBCLASS_IDE, 0xFF, &ide_controller) != 0) {
        debug_printf("[ATA DMA] ERROR: No IDE controller found\n");
        return -1;
    }

    debug_printf("[ATA DMA] Found IDE controller: %02x:%02x.%x (Vendor: 0x%04x, Device: 0x%04x)\n",
                 ide_controller.bus, ide_controller.device, ide_controller.function,
                 ide_controller.vendor_id, ide_controller.device_id);

    if (pci_enable_bus_master(&ide_controller) != 0) {
        debug_printf("[ATA DMA] ERROR: Failed to enable bus mastering\n");
        return -1;
    }

    debug_printf("[ATA DMA] Bus mastering enabled\n");

    uint32_t bar4 = pci_read_bar(&ide_controller, 4);
    if (bar4 == 0 || bar4 == 0xFFFFFFFF) {
        debug_printf("[ATA DMA] ERROR: Invalid BAR4 value: 0x%08x\n", bar4);
        return -1;
    }

    dma_state.bus_master_base = (uint16_t)(bar4 & 0xFFFC);
    debug_printf("[ATA DMA] Bus Master Base: 0x%04x\n", dma_state.bus_master_base);

    dma_state.prd_table_virt = prd_table_static;
    dma_state.prd_table_phys = (uintptr_t)prd_table_static;

    debug_printf("[ATA DMA] PRD Table: virt=0x%p phys=0x%lx\n",
                 dma_state.prd_table_virt, dma_state.prd_table_phys);

    if (!IS_32BIT_SAFE(dma_state.prd_table_phys)) {
        debug_printf("[ATA DMA] ERROR: PRD table above 4GB boundary\n");
        return -1;
    }

    uint64_t boundary_check = (dma_state.prd_table_phys & 0xFFFF0000ULL);
    uint64_t end_boundary = ((dma_state.prd_table_phys + 4096 - 1) & 0xFFFF0000ULL);
    if (boundary_check != end_boundary) {
        debug_printf("[ATA DMA] ERROR: PRD table crosses 64KB boundary - ABORT init\n");
        debug_printf("[ATA DMA] System will fallback to PIO mode\n");
        dma_state.available = false;
        return -1;
    }

    memset(dma_state.prd_table_virt, 0, 4096);

    memset(&dma_state.active_request, 0, sizeof(ata_dma_request_t));
    dma_state.active_request.status = ATA_DMA_STATUS_FREE;

    dma_state.active_request_idx = 0xFF;

    atomic_init_u32(&dma_state.total_requests, 0);
    atomic_init_u32(&dma_state.completed_requests, 0);
    atomic_init_u32(&dma_state.failed_requests, 0);
    atomic_init_u32(&dma_state.timeout_count, 0);

    dma_state.available = true;

    ATA_DMA_DEBUG("[ATA DMA] Initialization complete\n");

    return 0;
}

bool ata_dma_available(void) {
    return dma_state.available;
}

int ata_dma_read_async(uint8_t is_master, uint32_t lba, uint16_t sector_count,
                       uint8_t* buffer_virt, uint32_t event_id, uint32_t pid) {
    if (!dma_state.available) {
        return -1;
    }

    if (!buffer_virt || sector_count == 0 || sector_count > 256) {
        return -1;
    }

    spin_lock(&dma_state.lock);

    if (dma_state.active_request_idx != 0xFF) {
        // Queue full (PIIX4 limitation: only 1 active DMA transfer)
        spin_unlock(&dma_state.lock);
        return -1;
    }

    void* dma_buffer = pmm_alloc(1);
    if (!dma_buffer) {
        spin_unlock(&dma_state.lock);
        debug_printf("[ATA DMA] ERROR: Failed to allocate DMA buffer\n");
        return -1;
    }

    ata_dma_request_t* req = &dma_state.active_request;
    memset(req, 0, sizeof(ata_dma_request_t));

    req->event_id = event_id;
    req->pid = pid;
    req->lba = lba;
    req->sector_count = sector_count;
    req->is_master = is_master;
    req->is_write = 0;
    req->buffer_virt = buffer_virt;
    req->buffer_phys = (uintptr_t)dma_buffer;
    req->buffer_size = sector_count * ATA_SECTOR_SIZE;
    req->status = ATA_DMA_STATUS_PENDING;
    req->start_time = rdtsc();
    req->retry_count = 0;

    if (ata_dma_setup_prd(req) != 0) {
        pmm_free(dma_buffer, 1);
        req->status = ATA_DMA_STATUS_FREE;
        spin_unlock(&dma_state.lock);
        debug_printf("[ATA DMA] ERROR: Failed to setup PRD\n");
        return -1;
    }

    if (ata_dma_send_command(req) != 0) {
        pmm_free(dma_buffer, 1);
        req->status = ATA_DMA_STATUS_FREE;
        spin_unlock(&dma_state.lock);
        debug_printf("[ATA DMA] ERROR: Failed to send DMA command\n");
        return -1;
    }

    req->status = ATA_DMA_STATUS_IN_PROGRESS;
    dma_state.active_request_idx = 0;
    atomic_fetch_add_u32(&dma_state.total_requests, 1);

    spin_unlock(&dma_state.lock);

    ATA_DMA_DEBUG("[ATA DMA] Read async: LBA=%u count=%u event_id=%u\n",
                  lba, sector_count, event_id);

    return 0;
}

int ata_dma_write_async(uint8_t is_master, uint32_t lba, uint16_t sector_count,
                        const uint8_t* buffer_virt, uint32_t event_id, uint32_t pid) {
    if (!dma_state.available) {
        return -1;
    }

    if (!buffer_virt || sector_count == 0 || sector_count > 256) {
        return -1;
    }

    spin_lock(&dma_state.lock);

    if (dma_state.active_request_idx != 0xFF) {
        // Queue full (PIIX4 limitation: only 1 active DMA transfer)
        spin_unlock(&dma_state.lock);
        return -1;
    }

    void* dma_buffer = pmm_alloc(1);
    if (!dma_buffer) {
        spin_unlock(&dma_state.lock);
        debug_printf("[ATA DMA] ERROR: Failed to allocate DMA buffer\n");
        return -1;
    }

    memcpy(dma_buffer, buffer_virt, sector_count * ATA_SECTOR_SIZE);
    mfence();

    ata_dma_request_t* req = &dma_state.active_request;
    memset(req, 0, sizeof(ata_dma_request_t));

    req->event_id = event_id;
    req->pid = pid;
    req->lba = lba;
    req->sector_count = sector_count;
    req->is_master = is_master;
    req->is_write = 1;
    req->buffer_virt = (uint8_t*)buffer_virt;
    req->buffer_phys = (uintptr_t)dma_buffer;
    req->buffer_size = sector_count * ATA_SECTOR_SIZE;
    req->status = ATA_DMA_STATUS_PENDING;
    req->start_time = rdtsc();
    req->retry_count = 0;

    if (ata_dma_setup_prd(req) != 0) {
        pmm_free(dma_buffer, 1);
        req->status = ATA_DMA_STATUS_FREE;
        spin_unlock(&dma_state.lock);
        debug_printf("[ATA DMA] ERROR: Failed to setup PRD\n");
        return -1;
    }

    if (ata_dma_send_command(req) != 0) {
        pmm_free(dma_buffer, 1);
        req->status = ATA_DMA_STATUS_FREE;
        spin_unlock(&dma_state.lock);
        debug_printf("[ATA DMA] ERROR: Failed to send DMA command\n");
        return -1;
    }

    req->status = ATA_DMA_STATUS_IN_PROGRESS;
    dma_state.active_request_idx = 0;
    atomic_fetch_add_u32(&dma_state.total_requests, 1);

    spin_unlock(&dma_state.lock);

    ATA_DMA_DEBUG("[ATA DMA] Write async: LBA=%u count=%u event_id=%u\n",
                  lba, sector_count, event_id);

    return 0;
}

void ata_dma_irq_handler(void) {
    if (!dma_state.available) {
        return;
    }

    uint16_t base = dma_state.bus_master_base;
    uint8_t status = inb(base + 2);

    if ((status & ATA_DMA_STATUS_IRQ) == 0) {
        return;
    }

    outb(base + 2, ATA_DMA_STATUS_IRQ | ATA_DMA_STATUS_ERROR);
    outb(base + 0, ATA_DMA_CMD_STOP);

    spin_lock(&dma_state.lock);

    if (dma_state.active_request_idx == 0xFF) {
        spin_unlock(&dma_state.lock);
        return;
    }

    ata_dma_request_t* req = &dma_state.active_request;

    if (req->status != ATA_DMA_STATUS_IN_PROGRESS) {
        spin_unlock(&dma_state.lock);
        return;
    }

    bool transfer_success = !(status & ATA_DMA_STATUS_ERROR);

    if (!transfer_success) {
        debug_printf("[ATA DMA] IRQ: DMA error status detected\n");
        req->status = ATA_DMA_STATUS_ERROR_DMA;
        atomic_fetch_add_u32(&dma_state.failed_requests, 1);
        async_io_mark_failed(req->event_id);
    } else {
        req->status = ATA_DMA_STATUS_COMPLETED;
        atomic_fetch_add_u32(&dma_state.completed_requests, 1);
        async_io_mark_completed_with_latency(req->event_id, req->start_time);

        ATA_DMA_DEBUG("[ATA DMA] IRQ: Transfer completed for event_id=%u\n", req->event_id);
    }

    Event completion_event;
    event_init(&completion_event, req->pid, req->event_id);
    completion_event.state = transfer_success ? EVENT_STATE_COMPLETED : EVENT_STATE_ERROR;

    if (transfer_success) {
        if (!req->is_write) {
            size_t copy_size = req->buffer_size;
            if (copy_size > EVENT_DATA_SIZE) {
                copy_size = EVENT_DATA_SIZE;
            }
            memcpy(completion_event.data, (void*)req->buffer_phys, copy_size);
        } else {
            obj_write_response_t* resp = (obj_write_response_t*)completion_event.data;
            memset(resp, 0, sizeof(obj_write_response_t));
            resp->bytes_written = req->buffer_size;

            uint64_t write_end = req->write_offset + req->buffer_size;
            resp->new_file_size = (write_end > req->original_file_size)
                                  ? write_end
                                  : req->original_file_size;
            resp->error_code = OK;

            TagFSMetadata* meta = tagfs_get_metadata(req->file_id);
            if (meta && (meta->flags & TAGFS_FILE_ACTIVE)) {
                if (resp->new_file_size > meta->size) {
                    meta->size = resp->new_file_size;
                    meta->modified_time++;
                    tagfs_write_metadata(req->file_id, meta);
                }
            }
        }
    }

    if (!event_ring_push(kernel_event_ring, &completion_event)) {
        debug_printf("[ATA DMA] CRITICAL: EventRing full (system overload), marking failed immediately\n");
        atomic_fetch_add_u32(&dma_state.failed_requests, 1);
        async_io_mark_failed(req->event_id);
        req->status = ATA_DMA_STATUS_ERROR_DMA;
    }

    pmm_free((void*)req->buffer_phys, 1);
    req->status = ATA_DMA_STATUS_FREE;
    dma_state.active_request_idx = 0xFF;

    spin_unlock(&dma_state.lock);

    guide_wake();
}

void ata_dma_check_timeouts(void) {
    if (!dma_state.available) {
        return;
    }

    spin_lock(&dma_state.lock);

    if (dma_state.active_request_idx == 0xFF) {
        spin_unlock(&dma_state.lock);
        return;
    }

    ata_dma_request_t* req = &dma_state.active_request;

    if (req->status != ATA_DMA_STATUS_IN_PROGRESS) {
        spin_unlock(&dma_state.lock);
        return;
    }

    uint64_t elapsed = rdtsc() - req->start_time;
    uint64_t timeout_cycles = cpu_ms_to_tsc(ATA_DMA_TIMEOUT_MS);

    if (elapsed > timeout_cycles) {
        debug_printf("[ATA DMA] Timeout: event_id=%u LBA=%u\n", req->event_id, req->lba);

        outb(dma_state.bus_master_base + 0, ATA_DMA_CMD_STOP);

        req->status = ATA_DMA_STATUS_ERROR_TIMEOUT;
        atomic_fetch_add_u32(&dma_state.timeout_count, 1);
        atomic_fetch_add_u32(&dma_state.failed_requests, 1);

        async_io_mark_failed(req->event_id);

        Event timeout_event;
        event_init(&timeout_event, req->pid, req->event_id);
        timeout_event.state = EVENT_STATE_ERROR;

        if (!event_ring_push(kernel_event_ring, &timeout_event)) {
            debug_printf("[ATA DMA] CRITICAL: EventRing full during timeout handling\n");
        }

        pmm_free((void*)req->buffer_phys, 1);
        req->status = ATA_DMA_STATUS_FREE;
        dma_state.active_request_idx = 0xFF;

        guide_wake();
    }

    spin_unlock(&dma_state.lock);
}

int ata_read_sectors_dma(uint8_t is_master, uint32_t lba, uint8_t count, uint8_t* buffer) {
    if (!dma_state.available || dma_state.active_request_idx != 0xFF) {
        // DMA unavailable or queue full - fallback to PIO
        ATA_DMA_DEBUG("[ATA DMA] Falling back to PIO\n");
        return ata_read_sectors(is_master, lba, count, buffer);
    }

    if (!buffer || count == 0) {
        debug_printf("[ATA DMA SYNC] Invalid parameters\n");
        return -1;
    }

    ATA_DMA_DEBUG("[ATA DMA SYNC] Starting synchronous DMA read\n");

    void* dma_buffer = pmm_alloc(1);
    if (!dma_buffer) {
        return -1;
    }

    spin_lock(&dma_state.lock);

    if (dma_state.active_request_idx != 0xFF) {
        spin_unlock(&dma_state.lock);
        pmm_free(dma_buffer, 1);
        return -1;
    }

    ata_dma_request_t* req = &dma_state.active_request;
    memset(req, 0, sizeof(ata_dma_request_t));

    req->event_id = 0;
    req->pid = 0;
    req->lba = lba;
    req->sector_count = count;
    req->is_master = is_master;
    req->is_write = 0;
    req->buffer_virt = buffer;
    req->buffer_phys = (uintptr_t)dma_buffer;
    req->buffer_size = count * ATA_SECTOR_SIZE;
    req->status = ATA_DMA_STATUS_PENDING;
    req->start_time = rdtsc();
    req->retry_count = 0;

    if (ata_dma_setup_prd(req) != 0) {
        pmm_free(dma_buffer, 1);
        req->status = ATA_DMA_STATUS_FREE;
        spin_unlock(&dma_state.lock);
        return -1;
    }

    if (ata_dma_send_command(req) != 0) {
        pmm_free(dma_buffer, 1);
        req->status = ATA_DMA_STATUS_FREE;
        spin_unlock(&dma_state.lock);
        return -1;
    }

    req->status = ATA_DMA_STATUS_IN_PROGRESS;
    dma_state.active_request_idx = 0;

    spin_unlock(&dma_state.lock);

    uint64_t timeout_cycles = cpu_ms_to_tsc(ATA_DMA_TIMEOUT_MS);
    uint64_t start = rdtsc();

    while (1) {
        uint16_t base = dma_state.bus_master_base;
        uint8_t status = inb(base + 2);

        if (status & ATA_DMA_STATUS_IRQ) {
            outb(base + 2, ATA_DMA_STATUS_IRQ | ATA_DMA_STATUS_ERROR);
            outb(base + 0, ATA_DMA_CMD_STOP);

            spin_lock(&dma_state.lock);

            if (status & ATA_DMA_STATUS_ERROR) {
                req->status = ATA_DMA_STATUS_ERROR_DMA;
                atomic_fetch_add_u32(&dma_state.failed_requests, 1);
                pmm_free(dma_buffer, 1);
                req->status = ATA_DMA_STATUS_FREE;
                dma_state.active_request_idx = 0xFF;
                spin_unlock(&dma_state.lock);
                return -1;
            }

            memcpy(buffer, dma_buffer, req->buffer_size);

            req->status = ATA_DMA_STATUS_COMPLETED;
            atomic_fetch_add_u32(&dma_state.completed_requests, 1);

            pmm_free(dma_buffer, 1);
            req->status = ATA_DMA_STATUS_FREE;
            dma_state.active_request_idx = 0xFF;

            spin_unlock(&dma_state.lock);
            return 0;
        }

        uint64_t elapsed = rdtsc() - start;
        if (elapsed > timeout_cycles) {
            spin_lock(&dma_state.lock);

            outb(dma_state.bus_master_base + 0, ATA_DMA_CMD_STOP);
            req->status = ATA_DMA_STATUS_ERROR_TIMEOUT;
            atomic_fetch_add_u32(&dma_state.failed_requests, 1);

            pmm_free(dma_buffer, 1);
            req->status = ATA_DMA_STATUS_FREE;
            dma_state.active_request_idx = 0xFF;

            spin_unlock(&dma_state.lock);
            return -1;
        }

        cpu_pause();
    }
}

int ata_write_sectors_dma(uint8_t is_master, uint32_t lba, uint8_t count, const uint8_t* buffer) {
    if (!dma_state.available || dma_state.active_request_idx != 0xFF) {
        ATA_DMA_DEBUG("[ATA DMA] Falling back to PIO\n");
        return ata_write_sectors(is_master, lba, count, buffer);
    }

    if (!buffer || count == 0) {
        return -1;
    }

    void* dma_buffer = pmm_alloc(1);
    if (!dma_buffer) {
        return -1;
    }

    memcpy(dma_buffer, buffer, count * ATA_SECTOR_SIZE);
    mfence();

    spin_lock(&dma_state.lock);

    if (dma_state.active_request_idx != 0xFF) {
        spin_unlock(&dma_state.lock);
        pmm_free(dma_buffer, 1);
        return -1;
    }

    ata_dma_request_t* req = &dma_state.active_request;
    memset(req, 0, sizeof(ata_dma_request_t));

    req->event_id = 0;
    req->pid = 0;
    req->lba = lba;
    req->sector_count = count;
    req->is_master = is_master;
    req->is_write = 1;
    req->buffer_virt = (uint8_t*)buffer;
    req->buffer_phys = (uintptr_t)dma_buffer;
    req->buffer_size = count * ATA_SECTOR_SIZE;
    req->status = ATA_DMA_STATUS_PENDING;
    req->start_time = rdtsc();
    req->retry_count = 0;

    if (ata_dma_setup_prd(req) != 0) {
        pmm_free(dma_buffer, 1);
        req->status = ATA_DMA_STATUS_FREE;
        spin_unlock(&dma_state.lock);
        return -1;
    }

    if (ata_dma_send_command(req) != 0) {
        pmm_free(dma_buffer, 1);
        req->status = ATA_DMA_STATUS_FREE;
        spin_unlock(&dma_state.lock);
        return -1;
    }

    req->status = ATA_DMA_STATUS_IN_PROGRESS;
    dma_state.active_request_idx = 0;

    spin_unlock(&dma_state.lock);

    uint64_t timeout_cycles = cpu_ms_to_tsc(ATA_DMA_TIMEOUT_MS);
    uint64_t start = rdtsc();

    while (1) {
        uint16_t base = dma_state.bus_master_base;
        uint8_t status = inb(base + 2);

        if (status & ATA_DMA_STATUS_IRQ) {
            outb(base + 2, ATA_DMA_STATUS_IRQ | ATA_DMA_STATUS_ERROR);
            outb(base + 0, ATA_DMA_CMD_STOP);

            spin_lock(&dma_state.lock);

            if (status & ATA_DMA_STATUS_ERROR) {
                req->status = ATA_DMA_STATUS_ERROR_DMA;
                atomic_fetch_add_u32(&dma_state.failed_requests, 1);
                pmm_free(dma_buffer, 1);
                req->status = ATA_DMA_STATUS_FREE;
                dma_state.active_request_idx = 0xFF;
                spin_unlock(&dma_state.lock);
                return -1;
            }

            req->status = ATA_DMA_STATUS_COMPLETED;
            atomic_fetch_add_u32(&dma_state.completed_requests, 1);

            pmm_free(dma_buffer, 1);
            req->status = ATA_DMA_STATUS_FREE;
            dma_state.active_request_idx = 0xFF;

            spin_unlock(&dma_state.lock);

            /* Flush write cache to ensure data reaches persistent storage */
            ata_flush_cache(is_master);

            return 0;
        }

        uint64_t elapsed = rdtsc() - start;
        if (elapsed > timeout_cycles) {
            spin_lock(&dma_state.lock);

            outb(dma_state.bus_master_base + 0, ATA_DMA_CMD_STOP);
            req->status = ATA_DMA_STATUS_ERROR_TIMEOUT;
            atomic_fetch_add_u32(&dma_state.failed_requests, 1);

            pmm_free(dma_buffer, 1);
            req->status = ATA_DMA_STATUS_FREE;
            dma_state.active_request_idx = 0xFF;

            spin_unlock(&dma_state.lock);
            return -1;
        }

        cpu_pause();
    }
}

void ata_dma_get_stats(ata_dma_stats_t *stats) {
    if (!stats) return;

    spin_lock(&dma_state.lock);
    stats->dma_available = dma_state.available;
    stats->bus_master_base = dma_state.bus_master_base;
    stats->total_requests = atomic_load_u32(&dma_state.total_requests);
    stats->completed_requests = atomic_load_u32(&dma_state.completed_requests);
    stats->failed_requests = atomic_load_u32(&dma_state.failed_requests);
    stats->timeout_count = atomic_load_u32(&dma_state.timeout_count);
    stats->active_requests = (dma_state.active_request_idx != 0xFF) ? 1 : 0;
    spin_unlock(&dma_state.lock);
}

void ata_dma_print_stats(void) {
    ata_dma_stats_t stats;
    ata_dma_get_stats(&stats);

    debug_printf("ATA DMA Statistics:\n");
    debug_printf("DMA Available:       %s\n", stats.dma_available ? "YES" : "NO");
    debug_printf("Bus Master Base:     0x%04X\n", stats.bus_master_base);
    debug_printf("Total Requests:      %u\n", stats.total_requests);
    debug_printf("Completed:           %u\n", stats.completed_requests);
    debug_printf("Failed:              %u\n", stats.failed_requests);
    debug_printf("Timeouts:            %u\n", stats.timeout_count);
    debug_printf("Active Requests:     %u\n", stats.active_requests);

    if (stats.total_requests > 0) {
        uint32_t success_rate = (stats.completed_requests * 100) / stats.total_requests;
        debug_printf("Success Rate:        %u%%\n", success_rate);
    }
}

bool ata_dma_is_idle(void) {
    // Compiler barrier to prevent caching active_request_idx in a register across calls
    asm volatile("" ::: "memory");
    uint8_t idx = dma_state.active_request_idx;
    return idx == 0xFF;
}

int ata_dma_start_async_transfer(async_io_request_t* req) {
    if (!req) {
        return -1;
    }

    if (!dma_state.available) {
        debug_printf("[ATA DMA ASYNC] DMA not available\n");
        return -1;
    }

    if (req->sector_count == 0 || req->sector_count > 256) {
        debug_printf("[ATA DMA ASYNC] Invalid sector_count: %u\n", req->sector_count);
        return -1;
    }

    spin_lock(&dma_state.lock);

    // PIIX4 limitation: only 1 active transfer at a time
    if (dma_state.active_request_idx != 0xFF) {
        spin_unlock(&dma_state.lock);
        ATA_DMA_DEBUG("[ATA DMA ASYNC] Queue full (PIIX4 limitation)\n");
        return -1;
    }

    void* dma_buffer = pmm_alloc(1);
    if (!dma_buffer) {
        spin_unlock(&dma_state.lock);
        debug_printf("[ATA DMA ASYNC] Failed to allocate DMA buffer\n");
        return -1;
    }

    if (req->op == ASYNC_IO_OP_WRITE && req->buffer_virt) {
        uint32_t total_size = req->sector_count * ATA_SECTOR_SIZE;
        uint32_t copy_size = req->data_length;
        if (copy_size > total_size) {
            copy_size = total_size;
        }

        /* Zero entire DMA buffer first, then overlay valid data.
         * This ensures no stale data leaks into sectors beyond data_length. */
        memset(dma_buffer, 0, total_size);
        memcpy(dma_buffer, req->buffer_virt, copy_size);
        mfence();
    }

    ata_dma_request_t* dma_req = &dma_state.active_request;
    memset(dma_req, 0, sizeof(ata_dma_request_t));

    dma_req->event_id = req->event_id;
    dma_req->pid = req->pid;
    dma_req->lba = req->lba;
    dma_req->sector_count = req->sector_count;
    dma_req->is_master = req->is_master;
    dma_req->is_write = (req->op == ASYNC_IO_OP_WRITE) ? 1 : 0;
    dma_req->buffer_virt = NULL;
    dma_req->buffer_phys = (uintptr_t)dma_buffer;
    dma_req->buffer_size = req->sector_count * ATA_SECTOR_SIZE;
    dma_req->status = ATA_DMA_STATUS_PENDING;
    dma_req->retry_count = 0;

    dma_req->file_id = req->file_id;
    dma_req->write_offset = req->write_offset;
    dma_req->original_file_size = req->original_file_size;

    // For WRITE: measure DMA transfer time only (memcpy already done above).
    // For READ: measure full async latency from original submit.
    if (req->op == ASYNC_IO_OP_WRITE) {
        dma_req->start_time = rdtsc();
    } else {
        dma_req->start_time = req->submit_time;
    }

    if (ata_dma_setup_prd(dma_req) != 0) {
        pmm_free(dma_buffer, 1);
        dma_req->status = ATA_DMA_STATUS_FREE;
        spin_unlock(&dma_state.lock);
        debug_printf("[ATA DMA ASYNC] Failed to setup PRD\n");
        return -1;
    }

    if (ata_dma_send_command(dma_req) != 0) {
        pmm_free(dma_buffer, 1);
        dma_req->status = ATA_DMA_STATUS_FREE;
        spin_unlock(&dma_state.lock);
        debug_printf("[ATA DMA ASYNC] Failed to send DMA command\n");
        return -1;
    }

    dma_req->status = ATA_DMA_STATUS_IN_PROGRESS;
    dma_state.active_request_idx = 0;
    atomic_fetch_add_u32(&dma_state.total_requests, 1);

    spin_unlock(&dma_state.lock);

    ATA_DMA_DEBUG("[ATA DMA ASYNC] Transfer started: event_id=%u LBA=%u count=%u %s\n",
                  req->event_id, req->lba, req->sector_count,
                  dma_req->is_write ? "WRITE" : "READ");

    return 0;
}
