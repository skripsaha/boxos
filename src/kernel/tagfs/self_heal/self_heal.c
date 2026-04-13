#include "self_heal.h"
#include "../disk_book/disk_book.h"
#include "../../lib/kernel/klib.h"
#include "../../lib/kernel/crypto.h"
#include "../../../kernel/drivers/timer/rtc.h"

// Global state
static HealState g_self_heal_state;

// CRC32 wrapper using shared crypto library
uint32_t TagFS_SelfHealComputeCrc32(const uint8_t *data, uint32_t length) {
    return KCrc32(data, length);
}

// Get severity based on corruption pattern
static uint8_t SelfHealGetSeverity(uint32_t pattern) {
    switch (pattern) {
        case HEAL_PATTERN_ZERO: return 2;
        case HEAL_PATTERN_ONES: return 3;
        case HEAL_PATTERN_RANDOM: return 2;
        default: return 1;
    }
}

// Add corruption record
static error_t SelfHealAddRecord(uint32_t block_number, uint32_t pattern,
                                 const uint8_t *data, bool recovered, const uint8_t *recovered_data) {
    // Write at current head, then advance head.  count only grows until the ring is full.
    HealCorruptionRecord *rec = &g_self_heal_state.records[g_self_heal_state.record_head];
    g_self_heal_state.record_head = (g_self_heal_state.record_head + 1) % HEAL_MAX_RECORDS;
    if (g_self_heal_state.record_count < HEAL_MAX_RECORDS)
        g_self_heal_state.record_count++;
    memset(rec, 0, sizeof(HealCorruptionRecord));
    
    rec->block_number = block_number;
    rec->detect_time = rtc_get_unix64();
    rec->corruption_type = pattern;
    rec->severity = SelfHealGetSeverity(pattern);
    rec->recovered = recovered ? 1 : 0;
    
    if (data)
        memcpy(rec->original_data, data, MIN(64, TAGFS_BLOCK_SIZE));
    if (recovered_data)
        memcpy(rec->recovered_data, recovered_data, MIN(64, TAGFS_BLOCK_SIZE));
    
    return OK;
}

error_t TagFS_SelfHealInit(void) {
    if (g_self_heal_state.initialized)
        return ERR_ALREADY_INITIALIZED;
    
    memset(&g_self_heal_state, 0, sizeof(HealState));
    spinlock_init(&g_self_heal_state.lock);
    
    g_self_heal_state.scrub_interval_ms = HEAL_SCRUB_INTERVAL_MS;
    g_self_heal_state.last_scrub_time = rtc_get_unix64();
    g_self_heal_state.stats.last_scrub_time = g_self_heal_state.last_scrub_time;
    g_self_heal_state.stats.next_scrub_time = g_self_heal_state.last_scrub_time + HEAL_SCRUB_INTERVAL_MS;
    
    g_self_heal_state.magic = HEAL_MAGIC;
    g_self_heal_state.version = HEAL_VERSION;
    g_self_heal_state.enabled = true;
    g_self_heal_state.initialized = true;
    
    debug_printf("[Self-Heal] Initialized: %d mirrors, scrub interval=%lu ms\n",
                 HEAL_MIRROR_COUNT, (unsigned long)HEAL_SCRUB_INTERVAL_MS);
    return OK;
}

void TagFS_SelfHealShutdown(void) {
    if (!g_self_heal_state.initialized)
        return;
    
    spin_lock(&g_self_heal_state.lock);
    g_self_heal_state.enabled = false;
    g_self_heal_state.initialized = false;
    spin_unlock(&g_self_heal_state.lock);
    
    debug_printf("[Self-Heal] Shutdown complete\n");
}

void TagFS_SelfHealEnable(bool enable) {
    if (!g_self_heal_state.initialized)
        return;
    
    spin_lock(&g_self_heal_state.lock);
    g_self_heal_state.enabled = enable;
    spin_unlock(&g_self_heal_state.lock);
    
    debug_printf("[Self-Heal] %s\n", enable ? "enabled" : "disabled");
}

bool TagFS_SelfHealIsEnabled(void) {
    return g_self_heal_state.initialized && g_self_heal_state.enabled;
}

error_t TagFS_SelfHealOnMetadataWrite(uint32_t block_number, const uint8_t *data) {
    if (!g_self_heal_state.initialized || !g_self_heal_state.enabled || !data)
        return ERR_NOT_INITIALIZED;
    
    spin_lock(&g_self_heal_state.lock);

    // Locate an existing slot for this block or evict the oldest via round-robin.
    // This ensures different blocks occupy different mirror slots so that
    // multiple recent blocks are protected simultaneously.
    int slot = -1;
    for (int i = 0; i < HEAL_MIRROR_COUNT; i++) {
        if (g_self_heal_state.mirrors[i].is_valid &&
            g_self_heal_state.mirrors[i].block_number == block_number) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = (int)g_self_heal_state.mirror_head;
        g_self_heal_state.mirror_head = (g_self_heal_state.mirror_head + 1) % HEAL_MIRROR_COUNT;
    }

    HealMirrorEntry *m = &g_self_heal_state.mirrors[slot];
    m->block_number = block_number;
    memcpy(m->data, data, TAGFS_BLOCK_SIZE);
    m->crc32 = TagFS_SelfHealComputeCrc32(data, TAGFS_BLOCK_SIZE);
    m->last_verified = rtc_get_unix64();
    m->is_valid = 1;
    m->mirror_index = (uint8_t)slot;

    g_self_heal_state.stats.mirror_syncs++;
    spin_unlock(&g_self_heal_state.lock);
    return OK;
}

error_t TagFS_SelfHealOnMetadataRead(uint32_t block_number, uint8_t *data, bool *recovered) {
    if (!g_self_heal_state.initialized || !g_self_heal_state.enabled || !data || !recovered)
        return ERR_NOT_INITIALIZED;
    
    *recovered = false;
    
    spin_lock(&g_self_heal_state.lock);
    
    int valid_mirror = -1;
    for (int i = 0; i < HEAL_MIRROR_COUNT; i++) {
        if (g_self_heal_state.mirrors[i].is_valid && g_self_heal_state.mirrors[i].block_number == block_number) {
            uint32_t crc = TagFS_SelfHealComputeCrc32(g_self_heal_state.mirrors[i].data, TAGFS_BLOCK_SIZE);
            if (crc == g_self_heal_state.mirrors[i].crc32) {
                valid_mirror = i;
                break;
            }
            g_self_heal_state.stats.crc_errors++;
        }
    }
    
    if (valid_mirror >= 0) {
        uint32_t data_crc = TagFS_SelfHealComputeCrc32(data, TAGFS_BLOCK_SIZE);
        if (data_crc != g_self_heal_state.mirrors[valid_mirror].crc32) {
            memcpy(data, g_self_heal_state.mirrors[valid_mirror].data, TAGFS_BLOCK_SIZE);
            *recovered = true;
            g_self_heal_state.stats.corruptions_detected++;
            g_self_heal_state.stats.corruptions_fixed++;
            SelfHealAddRecord(block_number, 0, data, true, g_self_heal_state.mirrors[valid_mirror].data);
        }
    }
    
    spin_unlock(&g_self_heal_state.lock);
    return OK;
}

error_t TagFS_SelfHealScrubBlock(uint32_t block_number) {
    if (!g_self_heal_state.initialized || !g_self_heal_state.enabled)
        return ERR_NOT_INITIALIZED;
    
    spin_lock(&g_self_heal_state.lock);
    
    for (int i = 0; i < HEAL_MIRROR_COUNT; i++) {
        HealMirrorEntry *m = &g_self_heal_state.mirrors[i];
        if (m->is_valid && m->block_number == block_number) {
            uint32_t crc = TagFS_SelfHealComputeCrc32(m->data, TAGFS_BLOCK_SIZE);
            if (crc != m->crc32) {
                m->is_valid = 0;
                g_self_heal_state.stats.corruptions_detected++;
                SelfHealAddRecord(block_number, 0, m->data, false, NULL);
            } else {
                m->last_verified = rtc_get_unix64();
            }
        }
    }
    
    g_self_heal_state.stats.blocks_scrubbed++;
    spin_unlock(&g_self_heal_state.lock);
    return OK;
}

error_t TagFS_SelfHealScrubRun(void) {
    if (!g_self_heal_state.initialized || !g_self_heal_state.enabled)
        return ERR_NOT_INITIALIZED;
    
    spin_lock(&g_self_heal_state.lock);
    g_self_heal_state.scrub_active = true;
    g_self_heal_state.stats.scrub_runs++;
    debug_printf("[Self-Heal] Starting scrub...\n");
    
    uint32_t scrubbed = 0;
    for (uint32_t i = 0; i < HEAL_SCRUB_BATCH_SIZE; i++) {
        uint32_t block = g_self_heal_state.scrub_block_current++;
        
        bool has_mirror = false;
        for (int j = 0; j < HEAL_MIRROR_COUNT; j++) {
            if (g_self_heal_state.mirrors[j].is_valid && g_self_heal_state.mirrors[j].block_number == block) {
                has_mirror = true;
                break;
            }
        }
        
        if (has_mirror) {
            spin_unlock(&g_self_heal_state.lock);
            TagFS_SelfHealScrubBlock(block);
            spin_lock(&g_self_heal_state.lock);
            scrubbed++;
        }
    }
    
    g_self_heal_state.last_scrub_time = rtc_get_unix64();
    g_self_heal_state.stats.last_scrub_time = g_self_heal_state.last_scrub_time;
    g_self_heal_state.stats.next_scrub_time = g_self_heal_state.last_scrub_time + g_self_heal_state.scrub_interval_ms;
    g_self_heal_state.scrub_active = false;
    
    debug_printf("[Self-Heal] Scrub complete: %u blocks\n", scrubbed);
    spin_unlock(&g_self_heal_state.lock);
    return OK;
}

error_t TagFS_SelfHealScheduleScrub(void) {
    if (!g_self_heal_state.initialized)
        return ERR_NOT_INITIALIZED;
    
    spin_lock(&g_self_heal_state.lock);
    
    uint64_t now = rtc_get_unix64();
    if (now >= g_self_heal_state.stats.next_scrub_time) {
        spin_unlock(&g_self_heal_state.lock);
        return TagFS_SelfHealScrubRun();
    }
    
    spin_unlock(&g_self_heal_state.lock);
    return OK;
}

error_t TagFS_SelfHealRecover(uint32_t block_number, uint8_t *recovered_data, bool *success) {
    if (!g_self_heal_state.initialized || !g_self_heal_state.enabled || !recovered_data || !success)
        return ERR_NOT_INITIALIZED;
    
    *success = false;
    
    spin_lock(&g_self_heal_state.lock);
    
    for (int i = 0; i < HEAL_MIRROR_COUNT; i++) {
        HealMirrorEntry *m = &g_self_heal_state.mirrors[i];
        if (m->is_valid && m->block_number == block_number) {
            uint32_t crc = TagFS_SelfHealComputeCrc32(m->data, TAGFS_BLOCK_SIZE);
            if (crc == m->crc32) {
                memcpy(recovered_data, m->data, TAGFS_BLOCK_SIZE);
                *success = true;
                g_self_heal_state.stats.corruptions_fixed++;
                debug_printf("[Self-Heal] Recovered block %u from mirror %u\n", block_number, i);
                break;
            }
        }
    }
    
    spin_unlock(&g_self_heal_state.lock);

    if (!*success) {
        spin_lock(&g_self_heal_state.lock);
        SelfHealAddRecord(block_number, 0, NULL, false, NULL);
        g_self_heal_state.stats.corruptions_unrecoverable++;
        spin_unlock(&g_self_heal_state.lock);
    }

    return OK;
}

error_t TagFS_SelfHealGetStats(HealStats *stats) {
    if (!stats)
        return ERR_INVALID_ARGUMENT;
    
    if (!g_self_heal_state.initialized) {
        memset(stats, 0, sizeof(HealStats));
        return ERR_NOT_INITIALIZED;
    }
    
    spin_lock(&g_self_heal_state.lock);
    memcpy(stats, &g_self_heal_state.stats, sizeof(HealStats));
    spin_unlock(&g_self_heal_state.lock);
    return OK;
}

error_t TagFS_SelfHealPrintStats(void) {
    if (!g_self_heal_state.initialized)
        return ERR_NOT_INITIALIZED;
    
    HealStats stats;
    TagFS_SelfHealGetStats(&stats);
    
    debug_printf("\n=== Self-Heal Statistics ===\n");
    debug_printf("Scrubbed:      %lu\n", (unsigned long)stats.blocks_scrubbed);
    debug_printf("Detected:      %lu\n", (unsigned long)stats.corruptions_detected);
    debug_printf("Fixed:         %lu\n", (unsigned long)stats.corruptions_fixed);
    debug_printf("Unrecoverable: %lu\n", (unsigned long)stats.corruptions_unrecoverable);
    debug_printf("Scrub runs:    %u\n", stats.scrub_runs);
    debug_printf("Mirror syncs:  %u\n", stats.mirror_syncs);
    debug_printf("=============================\n");
    
    return OK;
}

error_t TagFS_SelfHealGetCorruptionRecords(HealCorruptionRecord *records, uint32_t max_records, uint32_t *count) {
    if (!records || !count || max_records == 0)
        return ERR_INVALID_ARGUMENT;
    
    if (!g_self_heal_state.initialized)
        return ERR_NOT_INITIALIZED;
    
    spin_lock(&g_self_heal_state.lock);
    
    uint32_t copy_count = max_records < g_self_heal_state.record_count ? max_records : g_self_heal_state.record_count;
    for (uint32_t i = 0; i < copy_count; i++) {
        // record_head points to the NEXT write slot, so head-1 is the most recent entry.
        uint32_t idx = (g_self_heal_state.record_head - 1 - i + HEAL_MAX_RECORDS) % HEAL_MAX_RECORDS;
        records[i] = g_self_heal_state.records[idx];
    }
    
    *count = copy_count;
    spin_unlock(&g_self_heal_state.lock);
    return OK;
}

error_t TagFS_SelfHealClearRecords(void) {
    if (!g_self_heal_state.initialized)
        return ERR_NOT_INITIALIZED;
    
    spin_lock(&g_self_heal_state.lock);
    memset(g_self_heal_state.records, 0, sizeof(HealCorruptionRecord) * HEAL_MAX_RECORDS);
    g_self_heal_state.record_count = 0;
    g_self_heal_state.record_head = 0;
    spin_unlock(&g_self_heal_state.lock);
    return OK;
}

bool TagFS_SelfHealIsInitialized(void) {
    return g_self_heal_state.initialized;
}
