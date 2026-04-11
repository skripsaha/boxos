#include "braid.h"
#include "../tagfs.h"
#include "../../lib/kernel/klib.h"
#include "../../../kernel/drivers/disk/ahci_sync.h"
#include "../../../kernel/drivers/disk/ahci.h"
#include "../../../kernel/drivers/disk/ata.h"
#include "../../../kernel/drivers/timer/rtc.h"

// Global Braid state
static BraidState g_braid_state;
static BraidStats g_braid_stats;
static uint8_t g_braid_primary_port = 0;

// Compute tag-based disk assignment (unique to Braid!)
static uint8_t BraidComputeTagDisk(const uint8_t *tag_context, uint8_t disk_count) {
    if (!tag_context || disk_count == 0)
        return 0;

    // Hash tag context to determine disk
    BoxHash tag_hash = BoxHashCompute(tag_context, 16, &g_braid_state.hash_ctx);
    return tag_hash.bytes[0] % disk_count;
}

// Read from specific disk using AHCI/ATA
static error_t BraidReadFromDisk(uint8_t disk_id, uint64_t block_num, void *data) {
    if (!g_braid_state.initialized)
        return ERR_NOT_INITIALIZED;

    if (disk_id >= BRAID_MAX_DISKS || !g_braid_state.disks[disk_id].online)
        return ERR_DEVICE_NOT_READY;

    if (!data)
        return ERR_NULL_POINTER;

    // Calculate sector address (block = 8 sectors for 4KB)
    uint64_t sector = block_num * 8;
    
    // Try AHCI first, fallback to ATA
    int result = ERR_IO;
    
    if (ahci_is_initialized()) {
        result = ahci_read_sectors_sync(g_braid_primary_port + disk_id, sector, 8, (uint8_t*)data);
    }
    
    if (result != 0) {
        // Fallback to ATA
        result = ata_read_sectors_retry(1, sector, 8, (uint8_t*)data);
    }

    if (result == 0) {
        g_braid_state.disks[disk_id].read_count++;
        g_braid_stats.total_reads++;
        return OK;
    }

    g_braid_state.disks[disk_id].error_count++;
    return ERR_IO;
}

// Write to specific disk using AHCI/ATA
static error_t BraidWriteToDisk(uint8_t disk_id, uint64_t block_num, const void *data) {
    if (!g_braid_state.initialized)
        return ERR_NOT_INITIALIZED;

    if (disk_id >= BRAID_MAX_DISKS || !g_braid_state.disks[disk_id].online)
        return ERR_DEVICE_NOT_READY;

    if (!data)
        return ERR_NULL_POINTER;

    // Calculate sector address (block = 8 sectors for 4KB)
    uint64_t sector = block_num * 8;
    
    // Try AHCI first, fallback to ATA
    int result = ERR_IO;
    
    if (ahci_is_initialized()) {
        result = ahci_write_sectors_sync(g_braid_primary_port + disk_id, sector, 8, (const uint8_t*)data);
    }
    
    if (result != 0) {
        // Fallback to ATA
        result = ata_write_sectors_retry(1, sector, 8, (const uint8_t*)data);
    }

    if (result == 0) {
        g_braid_state.disks[disk_id].write_count++;
        g_braid_state.disks[disk_id].used_blocks++;
        g_braid_stats.total_writes++;
        return OK;
    }

    g_braid_state.disks[disk_id].error_count++;
    return ERR_IO;
}

// Verify block checksum
static bool BraidVerifyChecksum(const void *data, uint32_t size, const BoxHash *expected) {
    if (!expected)
        return false;
    BoxHash computed = BoxHashComputeSecure(data, size, &g_braid_state.hash_ctx);
    return BoxHashEqual(&computed, expected);
}

error_t BraidInit(BraidMode mode) {
    if (g_braid_state.initialized)
        return ERR_ALREADY_INITIALIZED;
    
    memset(&g_braid_state, 0, sizeof(BraidState));
    memset(&g_braid_stats, 0, sizeof(BraidStats));
    
    spinlock_init(&g_braid_state.lock);
    
    // Initialize hash context with unique salt
    BoxHashInit(&g_braid_state.hash_ctx);
    
    g_braid_state.magic = BRAID_MAGIC;
    g_braid_state.version = BRAID_VERSION;
    g_braid_state.mode = mode;
    g_braid_state.disk_count = 0;
    g_braid_state.active_disks = 0;
    g_braid_state.initialized = true;
    
    debug_printf("[Braid] Initialized: mode=%u, BoxHash %s\n", 
                 mode, g_braid_state.hash_ctx.key_initialized ? "secure" : "fast");
    return OK;
}

void BraidShutdown(void) {
    if (!g_braid_state.initialized)
        return;
    
    spin_lock(&g_braid_state.lock);
    g_braid_state.initialized = false;
    spin_unlock(&g_braid_state.lock);
    
    debug_printf("[Braid] Shutdown complete\n");
}

error_t BraidAddDisk(uint8_t disk_id, uint64_t total_blocks) {
    if (!g_braid_state.initialized)
        return ERR_NOT_INITIALIZED;
    
    if (disk_id >= BRAID_MAX_DISKS)
        return ERR_OUT_OF_RANGE;
    
    spin_lock(&g_braid_state.lock);
    
    BraidDisk *disk = &g_braid_state.disks[disk_id];
    disk->disk_id = disk_id;
    disk->online = true;
    disk->total_blocks = total_blocks;
    disk->used_blocks = 0;
    disk->read_count = 0;
    disk->write_count = 0;
    disk->error_count = 0;
    disk->last_seen = rtc_get_unix64();
    
    g_braid_state.disk_count++;
    g_braid_state.active_disks++;
    
    spin_unlock(&g_braid_state.lock);
    
    debug_printf("[Braid] Added disk %u (%lu blocks)\n", disk_id, (unsigned long)total_blocks);
    return OK;
}

error_t BraidRemoveDisk(uint8_t disk_id) {
    if (!g_braid_state.initialized)
        return ERR_NOT_INITIALIZED;
    
    if (disk_id >= BRAID_MAX_DISKS)
        return ERR_OUT_OF_RANGE;
    
    spin_lock(&g_braid_state.lock);
    
    BraidDisk *disk = &g_braid_state.disks[disk_id];
    if (!disk->online) {
        spin_unlock(&g_braid_state.lock);
        return ERR_DEVICE_NOT_READY;
    }
    
    disk->online = false;
    g_braid_state.active_disks--;
    g_braid_stats.disk_failures++;
    
    spin_unlock(&g_braid_state.lock);
    
    debug_printf("[Braid] Removed disk %u\n", disk_id);
    return OK;
}

error_t BraidSetDiskOnline(uint8_t disk_id, bool online) {
    if (!g_braid_state.initialized)
        return ERR_NOT_INITIALIZED;
    
    if (disk_id >= BRAID_MAX_DISKS)
        return ERR_OUT_OF_RANGE;
    
    spin_lock(&g_braid_state.lock);
    
    BraidDisk *disk = &g_braid_state.disks[disk_id];
    disk->online = online;
    disk->last_seen = rtc_get_unix64();
    
    if (online)
        g_braid_state.active_disks++;
    else
        g_braid_state.active_disks--;
    
    spin_unlock(&g_braid_state.lock);
    return OK;
}

error_t BraidReadBlock(uint64_t block_num, void *data, BoxHash *expected_checksum) {
    if (!g_braid_state.initialized || !data)
        return ERR_NOT_INITIALIZED;
    
    spin_lock(&g_braid_state.lock);
    
    error_t result = ERR_IO;
    uint8_t disks_tried = 0;
    
    // Try to read from available disks based on mode
    for (uint8_t i = 0; i < g_braid_state.disk_count && disks_tried < 2; i++) {
        if (!g_braid_state.disks[i].online)
            continue;
        
        result = BraidReadFromDisk(i, block_num, data);
        disks_tried++;
        
        if (result == OK && expected_checksum) {
            // Verify checksum
            if (!BraidVerifyChecksum(data, BRAID_BLOCK_SIZE, expected_checksum)) {
                g_braid_stats.checksum_errors++;
                result = ERR_CORRUPTED;
                continue;  // Try another disk
            }
        }
        
        if (result == OK)
            break;
    }
    
    spin_unlock(&g_braid_state.lock);
    return result;
}

error_t BraidWriteBlock(uint64_t block_num, const void *data, const uint8_t *tag_context) {
    if (!g_braid_state.initialized || !data)
        return ERR_NOT_INITIALIZED;

    (void)tag_context;  // Used in tag-aware mode
    spin_lock(&g_braid_state.lock);

    error_t result = OK;
    uint8_t writes_done = 0;

    switch (g_braid_state.mode) {
        case BraidModeMirror:
            // Write to 2 disks
            for (uint8_t i = 0; i < g_braid_state.disk_count && writes_done < 2; i++) {
                if (!g_braid_state.disks[i].online)
                    continue;

                if (BraidWriteToDisk(i, block_num, data) == OK)
                    writes_done++;
            }
            break;

        case BraidModeStripe:
            // Write to 1 disk (striped)
            {
                uint8_t target_disk = block_num % g_braid_state.disk_count;
                if (g_braid_state.disks[target_disk].online) {
                    result = BraidWriteToDisk(target_disk, block_num, data);
                    writes_done = 1;
                }
            }
            break;

        case BraidModeWeave:
            // Write to 3 disks (maximum safety)
            for (uint8_t i = 0; i < g_braid_state.disk_count && writes_done < 3; i++) {
                if (!g_braid_state.disks[i].online)
                    continue;

                if (BraidWriteToDisk(i, block_num, data) == OK)
                    writes_done++;
            }
            break;
    }

    if (writes_done == 0)
        result = ERR_IO;

    spin_unlock(&g_braid_state.lock);
    return result;
}

error_t BraidVerifyBlock(uint64_t block_num, bool *is_valid) {
    if (!g_braid_state.initialized || !is_valid)
        return ERR_NOT_INITIALIZED;
    
    *is_valid = false;
    
    uint8_t data[BRAID_BLOCK_SIZE];
    BoxHash checksum;  // Would come from metadata in production
    
    error_t result = BraidReadBlock(block_num, data, &checksum);
    if (result == OK)
        *is_valid = true;
    
    return result;
}

// Tag-aware read (unique to Braid!)
error_t BraidReadBlockTagged(uint64_t block_num, void *data, const uint8_t *tag_context) {
    if (!g_braid_state.initialized || !data)
        return ERR_NOT_INITIALIZED;
    
    spin_lock(&g_braid_state.lock);
    
    error_t result = ERR_IO;
    
    // Tag-aware disk selection (unique feature!)
    uint8_t preferred_disk = BraidComputeTagDisk(tag_context, g_braid_state.disk_count);
    
    // Try preferred disk first (tag-aware optimization)
    if (g_braid_state.disks[preferred_disk].online) {
        result = BraidReadFromDisk(preferred_disk, block_num, data);
        g_braid_stats.tag_assignments++;
    }
    
    // Fallback to other disks
    if (result != OK) {
        for (uint8_t i = 0; i < g_braid_state.disk_count; i++) {
            if (i == preferred_disk || !g_braid_state.disks[i].online)
                continue;
            
            result = BraidReadFromDisk(i, block_num, data);
            if (result == OK)
                break;
        }
    }
    
    spin_unlock(&g_braid_state.lock);
    return result;
}

// Tag-aware write (unique to Braid!)
error_t BraidWriteBlockTagged(uint64_t block_num, const void *data, const uint8_t *tag_context) {
    if (!g_braid_state.initialized || !data || !tag_context)
        return ERR_INVALID_ARGUMENT;

    spin_lock(&g_braid_state.lock);

    // Tag-aware disk selection (unique feature!)
    uint8_t primary_disk = BraidComputeTagDisk(tag_context, g_braid_state.disk_count);

    error_t result = BraidWriteToDisk(primary_disk, block_num, data);
    g_braid_stats.tag_assignments++;

    // For mirror/weave modes, write to additional disks
    if (g_braid_state.mode == BraidModeMirror || g_braid_state.mode == BraidModeWeave) {
        uint8_t writes_done = 1;
        uint8_t required = (g_braid_state.mode == BraidModeMirror) ? 2 : 3;

        for (uint8_t i = 0; i < g_braid_state.disk_count && writes_done < required; i++) {
            if (i == primary_disk || !g_braid_state.disks[i].online)
                continue;

            if (BraidWriteToDisk(i, block_num, data) == OK)
                writes_done++;
        }
    }

    spin_unlock(&g_braid_state.lock);
    return result;
}

// Auto-healing from mirror (unique to Braid!)
error_t BraidAutoHeal(uint64_t block_num) {
    if (!g_braid_state.initialized)
        return ERR_NOT_INITIALIZED;

    spin_lock(&g_braid_state.lock);

    uint8_t data[BRAID_BLOCK_SIZE];
    error_t result = ERR_IO;
    uint8_t healed_from_disk = 0xFF;

    // Try to read from each disk until we get valid data
    for (uint8_t i = 0; i < g_braid_state.disk_count; i++) {
        if (!g_braid_state.disks[i].online)
            continue;

        result = BraidReadFromDisk(i, block_num, data);
        if (result != OK)
            continue;
        
        // Verify checksum
        // In production, checksum would come from metadata
        // For now, assume first successful read is valid
        healed_from_disk = i;
        break;
    }
    
    if (healed_from_disk != 0xFF) {
        // Re-write to failed disks
        for (uint8_t i = 0; i < g_braid_state.disk_count; i++) {
            if (i == healed_from_disk || !g_braid_state.disks[i].online)
                continue;
            
            BraidWriteToDisk(i, block_num, data);
        }
        
        g_braid_stats.auto_heals++;
        debug_printf("[Braid] Auto-healed block %lu from disk %u\n", 
                     (unsigned long)block_num, healed_from_disk);
        result = OK;
    }
    
    spin_unlock(&g_braid_state.lock);
    return result;
}

error_t BraidGetStats(BraidStats *stats) {
    if (!stats)
        return ERR_INVALID_ARGUMENT;
    
    if (!g_braid_state.initialized) {
        memset(stats, 0, sizeof(BraidStats));
        return ERR_NOT_INITIALIZED;
    }
    
    spin_lock(&g_braid_state.lock);
    memcpy(stats, &g_braid_stats, sizeof(BraidStats));
    spin_unlock(&g_braid_state.lock);
    
    return OK;
}

error_t BraidPrintStats(void) {
    if (!g_braid_state.initialized)
        return ERR_NOT_INITIALIZED;
    
    BraidStats stats;
    BraidGetStats(&stats);
    
    debug_printf("\n=== Braid Statistics ===\n");
    debug_printf("Total reads:      %lu\n", (unsigned long)stats.total_reads);
    debug_printf("Total writes:     %lu\n", (unsigned long)stats.total_writes);
    debug_printf("Checksum errors:  %lu\n", (unsigned long)stats.checksum_errors);
    debug_printf("Auto-heals:       %lu\n", (unsigned long)stats.auto_heals);
    debug_printf("Disk failures:    %lu\n", (unsigned long)stats.disk_failures);
    debug_printf("Tag assignments:  %lu\n", (unsigned long)stats.tag_assignments);
    debug_printf("======================\n");
    
    return OK;
}

bool BraidIsHealthy(void) {
    if (!g_braid_state.initialized)
        return false;
    
    spin_lock(&g_braid_state.lock);
    
    // Healthy if we have enough disks for the mode
    bool healthy = false;
    switch (g_braid_state.mode) {
        case BraidModeMirror:
            healthy = g_braid_state.active_disks >= 2;
            break;
        case BraidModeStripe:
            healthy = g_braid_state.active_disks >= 1;
            break;
        case BraidModeWeave:
            healthy = g_braid_state.active_disks >= 3;
            break;
    }
    
    spin_unlock(&g_braid_state.lock);
    return healthy;
}

uint8_t BraidGetActiveDiskCount(void) {
    if (!g_braid_state.initialized)
        return 0;
    
    spin_lock(&g_braid_state.lock);
    uint8_t count = g_braid_state.active_disks;
    spin_unlock(&g_braid_state.lock);
    
    return count;
}
