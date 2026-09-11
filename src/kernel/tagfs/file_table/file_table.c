#include "file_table.h"

static FileTableEntry* g_entries     = NULL;
static uint32_t        g_capacity    = 0;
static bool            g_dirty       = false;
static spinlock_t      g_lock;

static uint32_t* g_chain     = NULL;
static uint32_t  g_chain_len = 0;
static uint32_t  g_chain_cap = 0;

static void chain_release(void) {
    if (g_chain) kfree(g_chain);
    g_chain     = NULL;
    g_chain_len = 0;
    g_chain_cap = 0;
}

static int chain_append(uint32_t block) {
    if (g_chain_len == g_chain_cap) {
        uint32_t cap = g_chain_cap ? g_chain_cap * 2 : 8;
        uint32_t* grown = kmalloc(sizeof(uint32_t) * cap);
        if (!grown) return -1;
        if (g_chain && g_chain_len)
            memcpy(grown, g_chain, sizeof(uint32_t) * g_chain_len);
        if (g_chain) kfree(g_chain);
        g_chain     = grown;
        g_chain_cap = cap;
    }
    g_chain[g_chain_len++] = block;
    return 0;
}

static int table_grow_to(uint32_t want) {
    if (want <= g_capacity) return 0;

    FileTableEntry* grown = kmalloc(sizeof(FileTableEntry) * want);
    if (!grown) return -1;

    if (g_entries && g_capacity)
        memcpy(grown, g_entries, sizeof(FileTableEntry) * g_capacity);
    memset(grown + g_capacity, 0, sizeof(FileTableEntry) * (want - g_capacity));

    if (g_entries) kfree(g_entries);
    g_entries  = grown;
    g_capacity = want;
    return 0;
}

static void table_release(void) {
    if (g_entries) kfree(g_entries);
    g_entries  = NULL;
    g_capacity = 0;
    chain_release();
}

int file_table_init(uint32_t first_block) {
    table_release();

    g_dirty = false;
    spinlock_init(&g_lock);

    TagFSState* fs  = tagfs_get_state();
    uint32_t    run = fs ? fs->layout.data_blocks : 0;
    if (run == 0) {
        kprintf("[FileTable] this volume states a data run of no blocks at "
                "all, so there is nowhere for a file table to be — it is not "
                "mounted\n");
        return -1;
    }

    uint32_t block = first_block;
    uint32_t hops  = 0;

    for (;;) {
        if (block >= run) {
            kprintf("[FileTable] the chain points at block %u and this "
                    "volume's data run is %u blocks — that is outside the "
                    "volume, so it is not mounted\n", block, run);
            table_release();
            return -1;
        }

        FileTableBlock fb;
        if (tagfs_read_block(block, &fb) != OK) {
            kprintf("[FileTable] block %u would not read — this volume's file "
                    "table cannot be reached, so it is not mounted\n", block);
            table_release();
            return -1;
        }

        if (fb.magic != TAGFS_FILETBL_MAGIC) {
            kprintf("[FileTable] block %u holds 0x%08x where a file table "
                    "should be — this volume is not mounted\n", block, fb.magic);
            table_release();
            return -1;
        }

        uint32_t entry_offset = g_chain_len * TAGFS_FTABLE_PER_BLOCK;

        if (chain_append(block) != 0 ||
            table_grow_to(entry_offset + TAGFS_FTABLE_PER_BLOCK) != 0) {
            kprintf("[FileTable] there is no memory to hold this volume's file "
                    "table at %u entries — it is not mounted\n",
                    entry_offset + TAGFS_FTABLE_PER_BLOCK);
            table_release();
            return -1;
        }

        uint32_t count = fb.entry_count;
        if (count > TAGFS_FTABLE_PER_BLOCK) {
            count = TAGFS_FTABLE_PER_BLOCK;
        }
        for (uint32_t i = 0; i < count; i++) {
            g_entries[entry_offset + i] = fb.entries[i];
        }

        if (fb.next_block == 0) break;

        if (++hops >= run) {
            kprintf("[FileTable] this volume's file table chain has taken %u "
                    "hops in a data run of %u blocks, so it is going round in "
                    "a circle — it is not mounted\n", hops, run);
            table_release();
            return -1;
        }
        block = fb.next_block;
    }

    debug_printf("[FileTable] initialized: first_block=%u blocks=%u capacity=%u\n",
                 first_block, g_chain_len, g_capacity);
    return 0;
}

void file_table_shutdown(bool write_back) {
    if (write_back) {
        file_table_flush();
    }

    table_release();
    g_dirty = false;

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
        uint32_t want = g_capacity * 2;
        if (want <= file_id) {
            want = file_id + 1;
        }

        if (table_grow_to(want) != 0) {
            spin_unlock(&g_lock);
            debug_printf("[FileTable] update: grow alloc failed (file_id=%u want=%u)\n",
                         file_id, want);
            return -1;
        }

        debug_printf("[FileTable] grew capacity to %u\n", g_capacity);
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

    uint32_t total_entries = g_capacity;
    uint32_t entry_offset  = 0;
    uint32_t link          = 0;

    while (entry_offset < total_entries) {
        uint32_t entries_this_block = total_entries - entry_offset;
        if (entries_this_block > TAGFS_FTABLE_PER_BLOCK) {
            entries_this_block = TAGFS_FTABLE_PER_BLOCK;
        }

        uint32_t remaining_after = total_entries - entry_offset - entries_this_block;

        uint32_t next_block = 0;
        if (remaining_after > 0) {
            if (link + 1 < g_chain_len) {
                next_block = g_chain[link + 1];
            } else {
                if (tagfs_alloc_blocks(1, &next_block) != 0) {
                    spin_unlock(&g_lock);
                    debug_printf("[FileTable] flush: alloc failed at entry_offset=%u\n",
                                 entry_offset);
                    return -1;
                }
                if (chain_append(next_block) != 0) {
                    spin_unlock(&g_lock);
                    debug_printf("[FileTable] flush: no memory to record block %u\n",
                                 next_block);
                    return -1;
                }
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

        if (link >= g_chain_len) {
            spin_unlock(&g_lock);
            kprintf("[FileTable] the table wants a %u'th block and only %u are "
                    "on its chain — nothing is written\n", link + 1, g_chain_len);
            return -1;
        }

        uint32_t here = g_chain[link];
        if (tagfs_write_block(here, &block) != OK) {
            spin_unlock(&g_lock);
            debug_printf("[FileTable] flush: write failed at block=%u\n", here);
            return -1;
        }

        entry_offset += entries_this_block;
        link++;
    }

    g_dirty = false;
    spin_unlock(&g_lock);
    return 0;
}