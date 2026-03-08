#include "file_table.h"

static FileTableEntry* g_entries     = NULL;
static uint32_t        g_capacity    = 0;
static uint32_t        g_first_block = 0;
static uint32_t        g_block_count = 0;
static bool            g_dirty       = false;
static spinlock_t      g_lock;

int file_table_init(uint32_t first_block, uint32_t block_count) {
    g_first_block = first_block;
    g_block_count = block_count;

    uint32_t capacity = block_count * TAGFS_FTABLE_PER_BLOCK;
    if (capacity == 0) {
        debug_printf("[FileTable] init: block_count is zero\n");
        return -1;
    }

    g_entries = kmalloc(sizeof(FileTableEntry) * capacity);
    if (!g_entries) {
        debug_printf("[FileTable] init: alloc failed (capacity=%u)\n", capacity);
        return -1;
    }
    memset(g_entries, 0, sizeof(FileTableEntry) * capacity);

    g_capacity = capacity;
    g_dirty    = false;
    spinlock_init(&g_lock);

    uint32_t block      = first_block;
    uint32_t block_idx  = 0;

    while (block != 0) {
        FileTableBlock fb;
        int ret = tagfs_read_block(block, &fb);
        if (ret < 0) {
            debug_printf("[FileTable] init: read failed at block=%u\n", block);
            break;
        }

        if (fb.magic != TAGFS_FILETBL_MAGIC) {
            debug_printf("[FileTable] init: bad magic at block=%u (got 0x%x)\n",
                         block, fb.magic);
            break;
        }

        uint32_t entry_offset = block_idx * TAGFS_FTABLE_PER_BLOCK;
        uint32_t count        = fb.entry_count;
        if (count > TAGFS_FTABLE_PER_BLOCK) {
            count = TAGFS_FTABLE_PER_BLOCK;
        }

        for (uint32_t i = 0; i < count; i++) {
            uint32_t idx = entry_offset + i;
            if (idx < g_capacity) {
                g_entries[idx] = fb.entries[i];
            }
        }

        block_idx++;
        block = fb.next_block;
    }

    debug_printf("[FileTable] initialized: first_block=%u block_count=%u capacity=%u\n",
                 first_block, block_count, capacity);
    return 0;
}

void file_table_shutdown(void) {
    file_table_flush();

    if (g_entries) {
        kfree(g_entries);
        g_entries = NULL;
    }
    g_capacity    = 0;
    g_first_block = 0;
    g_block_count = 0;
    g_dirty       = false;

    debug_printf("[FileTable] shutdown\n");
}

int file_table_lookup(uint32_t file_id, uint32_t* out_block, uint32_t* out_offset) {
    spin_lock(&g_lock);

    if (file_id >= g_capacity || g_entries[file_id].meta_block == 0) {
        spin_unlock(&g_lock);
        return -1;
    }

    *out_block  = g_entries[file_id].meta_block;
    *out_offset = g_entries[file_id].meta_offset;

    spin_unlock(&g_lock);
    return 0;
}

int file_table_update(uint32_t file_id, uint32_t meta_block, uint32_t meta_offset) {
    spin_lock(&g_lock);

    if (file_id >= g_capacity) {
        uint32_t new_capacity = g_capacity * 2;
        if (new_capacity <= file_id) {
            new_capacity = file_id + 1;
        }

        FileTableEntry* new_entries = kmalloc(sizeof(FileTableEntry) * new_capacity);
        if (!new_entries) {
            spin_unlock(&g_lock);
            debug_printf("[FileTable] update: grow alloc failed (file_id=%u new_capacity=%u)\n",
                         file_id, new_capacity);
            return -1;
        }

        memcpy(new_entries, g_entries, sizeof(FileTableEntry) * g_capacity);
        memset(new_entries + g_capacity, 0,
               sizeof(FileTableEntry) * (new_capacity - g_capacity));

        kfree(g_entries);
        g_entries  = new_entries;
        g_capacity = new_capacity;

        debug_printf("[FileTable] grew capacity to %u\n", new_capacity);
    }

    g_entries[file_id].meta_block  = meta_block;
    g_entries[file_id].meta_offset = meta_offset;
    g_dirty = true;

    spin_unlock(&g_lock);
    return 0;
}

int file_table_delete(uint32_t file_id) {
    spin_lock(&g_lock);

    if (file_id >= g_capacity) {
        spin_unlock(&g_lock);
        return -1;
    }

    g_entries[file_id].meta_block  = 0;
    g_entries[file_id].meta_offset = 0;
    g_dirty = true;

    spin_unlock(&g_lock);
    return 0;
}

int file_table_flush(void) {
    spin_lock(&g_lock);

    if (!g_dirty) {
        spin_unlock(&g_lock);
        return 0;
    }

    uint32_t total_entries  = g_capacity;
    uint32_t current_block  = g_first_block;
    uint32_t entry_offset   = 0;

    while (entry_offset < total_entries) {
        uint32_t entries_this_block = total_entries - entry_offset;
        if (entries_this_block > TAGFS_FTABLE_PER_BLOCK) {
            entries_this_block = TAGFS_FTABLE_PER_BLOCK;
        }

        uint32_t remaining_after = total_entries - entry_offset - entries_this_block;

        uint32_t next_block = 0;
        if (remaining_after > 0) {
            int ret = tagfs_alloc_blocks(1, &next_block);
            if (ret < 0) {
                spin_unlock(&g_lock);
                debug_printf("[FileTable] flush: alloc failed at entry_offset=%u\n", entry_offset);
                return -1;
            }
        }

        FileTableBlock block;
        block.magic       = TAGFS_FILETBL_MAGIC;
        block.next_block  = next_block;
        block.entry_count = entries_this_block;
        block.reserved    = 0;

        for (uint32_t i = 0; i < entries_this_block; i++) {
            block.entries[i] = g_entries[entry_offset + i];
        }
        for (uint32_t i = entries_this_block; i < TAGFS_FTABLE_PER_BLOCK; i++) {
            block.entries[i].meta_block  = 0;
            block.entries[i].meta_offset = 0;
        }

        int ret = tagfs_write_block(current_block, &block);
        if (ret < 0) {
            spin_unlock(&g_lock);
            debug_printf("[FileTable] flush: write failed at block=%u\n", current_block);
            return -1;
        }

        entry_offset  += entries_this_block;
        current_block  = next_block;
    }

    g_dirty = false;
    spin_unlock(&g_lock);
    return 0;
}
