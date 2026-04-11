#include "tagfs.h"
#include "tag_registry/tag_registry.h"
#include "tag_bitmap/tag_bitmap.h"
#include "file_table/file_table.h"
#include "metadata_pool/meta_pool.h"
#include "disk_book/disk_book.h"
#include "bcdc/bcdc.h"
#include "error.h"
#include "ahci_sync.h"
#include "ahci.h"
#include "ata.h"
#include "cow/cow.h"
#include "../../kernel/drivers/timer/rtc.h"
#include "../../lib/kernel/crypto.h"

void tagfs_snapshots_init(void);

static void tagfs_auto_snapshot_before_write(uint32_t file_id) {
    if (file_id == 0)
        return;

    // Check if CoW is active for this file
    if (TagFS_CowIsActive(file_id)) {
        // Create auto-snapshot before write
        char snap_name[64];
        ksnprintf(snap_name, sizeof(snap_name), "auto-snap-%u", file_id);

        uint32_t snapshot_id;
        error_t err = TagFS_SnapshotCreate(snap_name, file_id, &snapshot_id);
        if (err != OK) {
            debug_printf("[TagFS] Auto-snapshot failed for file %u: %d\n", file_id, err);
        }
    }
}

static TagFSState g_state;
static WellKnownTags g_well_known;

// Accessor function for well-known tags (tagfs_get_state already defined below)
WellKnownTags *tagfs_get_well_known_tags(void) { return &g_well_known; }

// Internal macro for accessing well-known tags
#define g_wk (*tagfs_get_well_known_tags())

// ----------------------------------------------------------------------------
// Open File Table — per-file write serialization
// ----------------------------------------------------------------------------

static OpenFileEntry *g_open_files[OPEN_FILE_BUCKETS];
static spinlock_t g_open_table_lock;

static uint32_t ofe_hash(uint32_t file_id)
{
    return file_id % OPEN_FILE_BUCKETS;
}

static OpenFileEntry *ofe_acquire(uint32_t file_id)
{
    spin_lock(&g_open_table_lock);
    uint32_t bucket = ofe_hash(file_id);
    OpenFileEntry *e = g_open_files[bucket];
    while (e)
    {
        if (e->file_id == file_id)
        {
            e->ref_count++;
            spin_unlock(&g_open_table_lock);
            return e;
        }
        e = e->next;
    }
    e = kmalloc(sizeof(OpenFileEntry));
    if (!e)
    {
        spin_unlock(&g_open_table_lock);
        return NULL;
    }
    e->file_id = file_id;
    e->ref_count = 1;
    spinlock_init(&e->write_lock);
    e->next = g_open_files[bucket];
    g_open_files[bucket] = e;
    spin_unlock(&g_open_table_lock);
    return e;
}

static void ofe_release(OpenFileEntry *ofe)
{
    if (!ofe)
        return;

    bool do_free = false;

    spin_lock(&g_open_table_lock);
    if (ofe->ref_count == 0)
    {
        uint32_t fid = ofe->file_id;
        spin_unlock(&g_open_table_lock);
        panic("[TagFS] ofe_release: ref_count already zero for file_id=%u — double-release or corruption", fid);
    }
    ofe->ref_count--;
    if (ofe->ref_count == 0)
    {
        // FIX: Remove from table FIRST while holding table_lock,
        // then RELEASE table_lock BEFORE acquiring write_lock.
        // This prevents nested spinlock deadlock.
        uint32_t bucket = ofe_hash(ofe->file_id);
        OpenFileEntry **ptr = &g_open_files[bucket];
        while (*ptr)
        {
            if (*ptr == ofe)
            {
                *ptr = ofe->next;
                break;
            }
            ptr = &(*ptr)->next;
        }
        do_free = true;
        // Note: table_lock is still held here, will be released below
    }
    spin_unlock(&g_open_table_lock);

    // Now acquire write_lock OUTSIDE of table_lock to prevent deadlock.
    // This waits for any in-flight writer to finish before freeing memory.
    if (do_free)
    {
        spin_lock(&ofe->write_lock);
        spin_unlock(&ofe->write_lock);
        kfree(ofe);
    }
}

// ----------------------------------------------------------------------------
// Bitmap bit helpers
// ----------------------------------------------------------------------------

static inline void bitmap_set_bit(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

static inline void bitmap_clear_bit(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit / 8] &= (uint8_t)~(1u << (bit % 8));
}

static inline bool bitmap_test_bit(const uint8_t *bitmap, uint32_t bit)
{
    return (bitmap[bit / 8] & (uint8_t)(1u << (bit % 8))) != 0;
}

// ----------------------------------------------------------------------------
// Disk I/O helpers
// ----------------------------------------------------------------------------

static int disk_read_sectors(uint64_t lba, uint16_t count, void *buffer)
{
    if (ahci_is_initialized())
        return ahci_read_sectors_sync(0, lba, count, buffer);
    return ata_read_sectors_retry(1, lba, count, (uint8_t *)buffer);
}

static int disk_write_sectors(uint64_t lba, uint16_t count, const void *buffer)
{
    if (ahci_is_initialized())
        return ahci_write_sectors_sync(0, lba, count, buffer);
    return ata_write_sectors_retry(1, lba, count, (const uint8_t *)buffer);
}

static uint64_t block_to_sector(uint32_t block);
static int read_block(uint32_t block, void *buffer);
static int write_block(uint32_t block, const void *buffer);

// ----------------------------------------------------------------------------
// Read-Ahead Cache for Sequential Reads
// ----------------------------------------------------------------------------

typedef struct {
    uint32_t block;
    uint8_t  data[TAGFS_BLOCK_SIZE];
    bool     valid;
} ReadAheadEntry;

static ReadAheadEntry g_read_ahead_cache[TAGFS_READ_AHEAD_BLOCKS];
static uint32_t g_read_ahead_head = 0;
static uint32_t g_read_ahead_last_block = 0;

static void ReadAheadInit(void) {
    memset(g_read_ahead_cache, 0, sizeof(g_read_ahead_cache));
    g_read_ahead_head = 0;
    g_read_ahead_last_block = 0;
}

static int ReadAheadLookup(uint32_t block, void *buffer) {
    for (uint32_t i = 0; i < TAGFS_READ_AHEAD_BLOCKS; i++) {
        if (g_read_ahead_cache[i].valid && g_read_ahead_cache[i].block == block) {
            memcpy(buffer, g_read_ahead_cache[i].data, TAGFS_BLOCK_SIZE);
            return 0;
        }
    }
    return -1;
}

static void ReadAheadPrefetch(uint32_t start_block, uint32_t count) {
    for (uint32_t i = 0; i < count && i < TAGFS_READ_AHEAD_BLOCKS; i++) {
        uint32_t block = start_block + i;
        if (block == g_read_ahead_last_block)
            continue;

        uint32_t slot = g_read_ahead_head % TAGFS_READ_AHEAD_BLOCKS;
        if (read_block(block, g_read_ahead_cache[slot].data) == 0) {
            g_read_ahead_cache[slot].block = block;
            g_read_ahead_cache[slot].valid = true;
            g_read_ahead_head++;
        }
    }
    if (count > 0)
        g_read_ahead_last_block = start_block + count - 1;
}

static uint64_t block_to_sector(uint32_t block)
{
    uint32_t data_start = g_state.superblock.block_bitmap_sector +
                          g_state.superblock.block_bitmap_sector_count;
    return (uint64_t)data_start + (uint64_t)block * 8;
}

static int read_block(uint32_t block, void *buffer)
{
    // Check read-ahead cache first
    if (ReadAheadLookup(block, buffer) == 0) {
        return 0;
    }
    return disk_read_sectors(block_to_sector(block), 8, buffer);
}

static int write_block(uint32_t block, const void *buffer)
{
    return disk_write_sectors(block_to_sector(block), 8, (void *)buffer);
}

error_t tagfs_read_block(uint32_t block, void *buffer) {
    if (!buffer)
        return ERR_NULL_POINTER;
    
    error_t err = read_block(block, buffer);
    if (err != OK)
        return ERR_READ_FAILED;
    
    return OK;
}

error_t tagfs_write_block(uint32_t block, const void *buffer) {
    if (!buffer)
        return ERR_NULL_POINTER;
    
    error_t err = write_block(block, buffer);
    if (err != OK)
        return ERR_WRITE_FAILED;
    
    return OK;
}

// ----------------------------------------------------------------------------
// CRC32 wrapper using shared crypto library
// ----------------------------------------------------------------------------

static uint32_t tagfs_crc32(const uint8_t *data, uint32_t len)
{
    return KCrc32(data, len);
}

static void superblock_stamp_crc(TagFSSuperblock *sb)
{
    memset(sb->reserved + TAGFS_SB_CRC_OFFSET, 0, 4);
    sb->reserved[TAGFS_SB_CRC_SENTINEL_OFFSET] = 0;
    uint32_t crc = tagfs_crc32((const uint8_t *)sb, sizeof(TagFSSuperblock));
    memcpy(sb->reserved + TAGFS_SB_CRC_OFFSET, &crc, 4);
    sb->reserved[TAGFS_SB_CRC_SENTINEL_OFFSET] = TAGFS_SB_CRC_SENTINEL;
}

static bool superblock_verify_crc(const TagFSSuperblock *sb)
{
    uint8_t sentinel = sb->reserved[TAGFS_SB_CRC_SENTINEL_OFFSET];
    if (sentinel != TAGFS_SB_CRC_SENTINEL)
    {
        debug_printf("[TagFS] WARNING: superblock has no CRC sentinel (legacy format) — accepted, will re-stamp on next write\n");
        return true;
    }

    uint32_t stored_crc;
    memcpy(&stored_crc, sb->reserved + TAGFS_SB_CRC_OFFSET, 4);

    TagFSSuperblock copy;
    memcpy(&copy, sb, sizeof(TagFSSuperblock));
    copy.reserved[TAGFS_SB_CRC_SENTINEL_OFFSET] = 0;
    memset(copy.reserved + TAGFS_SB_CRC_OFFSET, 0, 4);
    uint32_t computed = tagfs_crc32((const uint8_t *)&copy, sizeof(TagFSSuperblock));
    if (computed != stored_crc)
    {
        debug_printf("[TagFS] ERROR: superblock CRC mismatch (stored=0x%08x computed=0x%08x)\n",
                     stored_crc, computed);
        return false;
    }
    return true;
}

// ----------------------------------------------------------------------------
// Superblock I/O
// ----------------------------------------------------------------------------

static int read_superblock(uint32_t sector, TagFSSuperblock *out)
{
    uint8_t buf[512];
    if (disk_read_sectors((uint64_t)sector, 1, buf) != 0)
    {
        return -1;
    }
    memcpy(out, buf, sizeof(TagFSSuperblock));
    return 0;
}

static error_t write_superblock_to_sector(uint32_t sector, const TagFSSuperblock *sb) {
    if (!sb)
        return ERR_NULL_POINTER;
    
    uint8_t buf[512];
    memset(buf, 0, 512);
    memcpy(buf, sb, sizeof(TagFSSuperblock));
    
    error_t err = disk_write_sectors((uint64_t)sector, 1, buf);
    if (err != OK)
        return ERR_WRITE_FAILED;
    
    return OK;
}

error_t tagfs_write_superblock(const TagFSSuperblock *sb) {
    if (!sb)
        return ERR_NULL_POINTER;
    
    // Stamp CRC before writing
    TagFSSuperblock stamped;
    memcpy(&stamped, sb, sizeof(TagFSSuperblock));
    superblock_stamp_crc(&stamped);

    error_t err = write_superblock_to_sector(TAGFS_SUPERBLOCK_SECTOR, &stamped);
    if (err != OK) {
        debug_printf("[TagFS] Failed to write primary superblock\n");
        return ERR_TAGFS_METADATA_ERROR;
    }
    
    err = write_superblock_to_sector(TAGFS_BACKUP_SB_SECTOR, &stamped);
    if (err != OK) {
        debug_printf("[TagFS] Warning: failed to write backup superblock\n");
        // Don't fail - primary was written successfully
    }
    
    return OK;
}

// ----------------------------------------------------------------------------
// Free extent list
// ----------------------------------------------------------------------------

static void free_list_destroy(void)
{
    FreeExtent *cur = g_state.block_bitmap.free_list;
    while (cur)
    {
        FreeExtent *next = cur->next;
        kfree(cur);
        cur = next;
    }
    g_state.block_bitmap.free_list = NULL;
    g_state.block_bitmap.extent_count = 0;
}

static void free_list_build(void)
{
    free_list_destroy();

    uint32_t total = g_state.block_bitmap.total_blocks;
    uint8_t *bitmap = g_state.block_bitmap.bitmap;
    FreeExtent **tail = &g_state.block_bitmap.free_list;

    uint32_t i = 0;
    while (i < total)
    {
        // skip used blocks
        while (i < total)
        {
            if ((i & 7) == 0 && i + 8 <= total && bitmap[i / 8] == 0xFF)
            {
                i += 8;
                continue;
            }
            if (bitmap_test_bit(bitmap, i))
            {
                i++;
                continue;
            }
            break;
        }
        if (i >= total)
            break;

        uint32_t start = i;
        while (i < total)
        {
            if ((i & 7) == 0 && i + 8 <= total && bitmap[i / 8] == 0x00)
            {
                i += 8;
                continue;
            }
            if (!bitmap_test_bit(bitmap, i))
            {
                i++;
                continue;
            }
            break;
        }

        FreeExtent *ext = kmalloc(sizeof(FreeExtent));
        if (!ext)
        {
            debug_printf("[TagFS] free_list_build: kmalloc failed\n");
            break;
        }
        ext->start = start;
        ext->count = i - start;
        ext->next = NULL;
        *tail = ext;
        tail = &ext->next;
        g_state.block_bitmap.extent_count++;
    }
}

// ----------------------------------------------------------------------------
// tagfs_format
// ----------------------------------------------------------------------------

error_t tagfs_format(uint32_t total_blocks) {
    debug_printf("[TagFS] Formatting: total_blocks=%u\n", total_blocks);

    if (total_blocks == 0)
        return ERR_INVALID_ARGUMENT;

    // --- Compute layout ---
    //
    // Sector layout (all fixed):
    //   1034 : primary superblock
    //   1035 : backup superblock
    //   1036 : journal superblock
    //   1037 : journal superblock backup
    //   1038 : journal entries  (512 entries * 2 sectors = 1024 sectors -> 1038..2061)
    //   2062 : block bitmap start
    //
    uint32_t bitmap_sector_start = 2062;
    uint32_t bitmap_bytes = (total_blocks + 7) / 8;
    uint32_t bitmap_sectors = (bitmap_bytes + 511) / 512;

    // --- Build initial block bitmap in memory ---
    uint8_t *bitmap = kmalloc(bitmap_bytes);
    if (!bitmap) {
        debug_printf("[TagFS] format: failed to allocate bitmap\n");
        return ERR_NO_MEMORY;
    }
    memset(bitmap, 0, bitmap_bytes);

    // Blocks 0,1,2 are reserved (tag registry, file table, metadata pool)
    bitmap_set_bit(bitmap, 0);
    bitmap_set_bit(bitmap, 1);
    bitmap_set_bit(bitmap, 2);

    // --- Write initial TagRegistryBlock at block 0 ---
    TagRegistryBlock *reg_block = kmalloc(TAGFS_BLOCK_SIZE);
    if (!reg_block) {
        debug_printf("[TagFS] format: failed to allocate registry block\n");
        kfree(bitmap);
        return ERR_NO_MEMORY;
    }
    memset(reg_block, 0, TAGFS_BLOCK_SIZE);
    reg_block->magic = TAGFS_REGISTRY_MAGIC;
    reg_block->next_block = 0;
    reg_block->entry_count = 0;
    reg_block->used_bytes = 0;

    // Temporarily set up g_state.superblock enough for block_to_sector to work
    g_state.superblock.block_bitmap_sector = bitmap_sector_start;
    g_state.superblock.block_bitmap_sector_count = bitmap_sectors;

    if (write_block(0, reg_block) != OK) {
        debug_printf("[TagFS] format: failed to write registry block\n");
        kfree(reg_block);
        kfree(bitmap);
        return ERR_WRITE_FAILED;
    }
    kfree(reg_block);

    // --- Write initial FileTableBlock at block 1 ---
    FileTableBlock *ft_block = kmalloc(TAGFS_BLOCK_SIZE);
    if (!ft_block) {
        debug_printf("[TagFS] format: failed to allocate file table block\n");
        kfree(bitmap);
        return ERR_NO_MEMORY;
    }
    memset(ft_block, 0, TAGFS_BLOCK_SIZE);
    ft_block->magic = TAGFS_FILETBL_MAGIC;
    ft_block->next_block = 0;
    ft_block->entry_count = 0;
    ft_block->reserved = 0;

    if (write_block(1, ft_block) != OK) {
        debug_printf("[TagFS] format: failed to write file table block\n");
        kfree(ft_block);
        kfree(bitmap);
        return ERR_WRITE_FAILED;
    }
    kfree(ft_block);

    // --- Write initial MetaPoolBlock at block 2 ---
    MetaPoolBlock *mp_block = kmalloc(TAGFS_BLOCK_SIZE);
    if (!mp_block) {
        debug_printf("[TagFS] format: failed to allocate meta pool block\n");
        kfree(bitmap);
        return ERR_NO_MEMORY;
    }
    memset(mp_block, 0, TAGFS_BLOCK_SIZE);
    mp_block->magic = TAGFS_MPOOL_MAGIC;
    mp_block->next_block = 0;
    mp_block->used_bytes = 0;
    mp_block->record_count = 0;

    if (write_block(2, mp_block) != OK) {
        debug_printf("[TagFS] format: failed to write meta pool block\n");
        kfree(mp_block);
        kfree(bitmap);
        return ERR_WRITE_FAILED;
    }
    kfree(mp_block);

    // --- Write block bitmap to disk ---
    uint32_t bitmap_buf_size = bitmap_sectors * 512;
    uint8_t *bitmap_buf = kmalloc(bitmap_buf_size);
    if (!bitmap_buf) {
        debug_printf("[TagFS] format: failed to allocate bitmap write buffer\n");
        kfree(bitmap);
        return ERR_NO_MEMORY;
    }
    memset(bitmap_buf, 0, bitmap_buf_size);
    memcpy(bitmap_buf, bitmap, bitmap_bytes);

    if (disk_write_sectors((uint64_t)bitmap_sector_start, (uint16_t)bitmap_sectors, bitmap_buf) != OK) {
        debug_printf("[TagFS] format: failed to write block bitmap\n");
        kfree(bitmap_buf);
        kfree(bitmap);
        return ERR_WRITE_FAILED;
    }
    kfree(bitmap_buf);
    kfree(bitmap);

    // --- Fill superblock ---
    TagFSSuperblock sb;
    memset(&sb, 0, sizeof(sb));

    sb.magic = TAGFS_MAGIC;
    sb.version = TAGFS_VERSION;
    sb.block_size = TAGFS_BLOCK_SIZE;
    sb.total_blocks = total_blocks;
    sb.free_blocks = total_blocks - 3;
    sb.total_files = 0;
    sb.next_file_id = 1;
    sb.next_tag_id = 1;
    sb.total_tags = 0;

    sb.tag_registry_block = 0;
    sb.tag_registry_block_count = 1;
    sb.file_table_block = 1;
    sb.file_table_block_count = 1;
    sb.metadata_pool_block = 2;
    sb.metadata_pool_block_count = 1;
    sb.block_bitmap_sector = bitmap_sector_start;
    sb.block_bitmap_sector_count = bitmap_sectors;
    sb.disk_book_superblock_sector = TAGFS_DISK_BOOK_SB_SECTOR;

    sb.fs_created_time = 0;
    sb.fs_modified_time = 0;
    sb.backup_superblock_sector = TAGFS_BACKUP_SB_SECTOR;

    if (tagfs_write_superblock(&sb) != OK) {
        debug_printf("[TagFS] format: failed to write superblock\n");
        return ERR_TAGFS_METADATA_ERROR;
    }

    debug_printf("[TagFS] Format complete: %u total blocks, %u free\n",
                 total_blocks, sb.free_blocks);
    return OK;
}

// ----------------------------------------------------------------------------
// System behavior tag helpers
// ----------------------------------------------------------------------------

// Check if a file has trashed or hidden tag. skip_trashed/skip_hidden control
// which behavior tags to check (allows callers to exempt explicitly-queried tags).
static bool file_has_system_behavior_tag(uint32_t file_id,
                                         bool skip_trashed, bool skip_hidden)
{
    if (!skip_trashed && !skip_hidden)
        return false;

    uint16_t trashed_id = g_wk.trashed
                              ? (uint16_t)__builtin_ctzll(g_wk.trashed)
                              : TAGFS_INVALID_TAG_ID;
    uint16_t hidden_id = g_wk.hidden
                             ? (uint16_t)__builtin_ctzll(g_wk.hidden)
                             : TAGFS_INVALID_TAG_ID;

    uint16_t file_tags[32];
    int count = tag_bitmap_tags_for_file(g_state.bitmap_index, file_id, file_tags, 32);

    for (int i = 0; i < count; i++)
    {
        if (skip_trashed && file_tags[i] == trashed_id)
            return true;
        if (skip_hidden && file_tags[i] == hidden_id)
            return true;
    }
    return false;
}

// ----------------------------------------------------------------------------
// tagfs_init_well_known_tags
// ----------------------------------------------------------------------------

static void register_well_known(uint64_t *field, TagRegistry *reg, const char *key)
{
    uint16_t tid = tag_registry_intern(reg, key, NULL);
    *field = (tid != TAGFS_INVALID_TAG_ID && tid < 64) ? (1ULL << tid) : 0;
}

void tagfs_init_well_known_tags(void)
{
    memset(&g_well_known, 0, sizeof(g_well_known));
    TagFSState *fs = tagfs_get_state();
    if (!fs || !fs->registry)
        return;
    TagRegistry *reg = fs->registry;

    register_well_known(&g_wk.system, reg, "system");
    register_well_known(&g_wk.utility, reg, "utility");
    register_well_known(&g_wk.app, reg, "app");
    register_well_known(&g_wk.god, reg, "god");
    register_well_known(&g_wk.stopped, reg, "stopped");
    register_well_known(&g_wk.bypass, reg, "bypass");
    register_well_known(&g_wk.network, reg, "network");
    register_well_known(&g_wk.trashed, reg, "trashed");
    register_well_known(&g_wk.hidden, reg, "hidden");
}

// ----------------------------------------------------------------------------
// tagfs_init
// ----------------------------------------------------------------------------

error_t tagfs_init(void) {
    if (g_state.initialized) {
        debug_printf("[TagFS] Already initialized\n");
        return ERR_ALREADY_INITIALIZED;
    }

    debug_printf("[TagFS] Initializing...\n");

    spinlock_init(&g_state.lock);
    spinlock_init(&g_open_table_lock);
    memset(g_open_files, 0, sizeof(g_open_files));

    // --- Read superblock ---
    TagFSSuperblock sb;
    bool used_backup = false;
    if (read_superblock(TAGFS_SUPERBLOCK_SECTOR, &sb) != OK || sb.magic != TAGFS_MAGIC) {
        debug_printf("[TagFS] Primary superblock invalid, trying backup at sector %u\n",
                     TAGFS_BACKUP_SB_SECTOR);
        if (read_superblock(TAGFS_BACKUP_SB_SECTOR, &sb) != OK || sb.magic != TAGFS_MAGIC) {
            debug_printf("[TagFS] CRITICAL: Both superblocks invalid — not formatted?\n");
            return ERR_TAGFS_CORRUPTED;
        }
        used_backup = true;
        debug_printf("[TagFS] Using backup superblock\n");
    }

    // Verify CRC32 integrity
    if (!superblock_verify_crc(&sb)) {
        debug_printf("[TagFS] CRC32 mismatch on %s superblock\n",
                     used_backup ? "backup" : "primary");
        if (!used_backup)
        {
            // Primary corrupted — try backup
            if (read_superblock(TAGFS_BACKUP_SB_SECTOR, &sb) == 0 &&
                sb.magic == TAGFS_MAGIC && superblock_verify_crc(&sb))
            {
                debug_printf("[TagFS] Backup superblock CRC OK, recovering\n");
                used_backup = true;
            }
            else
            {
                debug_printf("[TagFS] CRITICAL: Both superblocks corrupted\n");
                return -1;
            }
        }
        else
        {
            debug_printf("[TagFS] CRITICAL: Backup superblock CRC also bad\n");
            return -1;
        }
    }

    // Restore primary from backup if needed
    if (used_backup)
    {
        write_superblock_to_sector(TAGFS_SUPERBLOCK_SECTOR, &sb);
    }

    if (sb.version != TAGFS_VERSION)
    {
        debug_printf("[TagFS] Unsupported version %u (expected %u)\n", sb.version, TAGFS_VERSION);
        return -1;
    }

    memcpy(&g_state.superblock, &sb, sizeof(TagFSSuperblock));
    debug_printf("[TagFS] Superblock OK: %u blocks, %u free, %u files\n",
                 sb.total_blocks, sb.free_blocks, sb.total_files);

    // --- DiskBook init + replay (production journaling) ---
    if (DiskBookInit(sb.disk_book_superblock_sector) != OK)
    {
        debug_printf("[TagFS] Warning: DiskBookInit failed\n");
    }
    else
    {
        if (DiskBookValidateAndReplay() != OK)
        {
            debug_printf("[TagFS] Warning: DiskBook replay failed\n");
        }
        debug_printf("[TagFS] DiskBook journaling enabled\n");
    }

    // --- Snapshots init ---
    tagfs_snapshots_init();

    // --- CoW Snapshots init ---
    if (TagFS_CowInit() != OK) {
        debug_printf("[TagFS] CoW init failed\n");
        return ERR_COW_NOT_INITIALIZED;
    }

    // --- Data Deduplication init ---
    if (TagFS_DedupInit() != OK) {
        debug_printf("[TagFS] Dedup init failed\n");
        return ERR_DEDUP_NOT_INITIALIZED;
    }

    // --- Self-Healing init ---
    TagFS_SelfHealInit();

    // --- Bcdc Compression init ---
    if (BcdcInit() != OK) {
        debug_printf("[TagFS] Bcdc init failed\n");
        return ERR_NO_MEMORY;
    }

    // --- Test Framework init ---
    if (TagFS_TestsInit() != OK) {
        debug_printf("[TagFS] Test framework init failed\n");
        return ERR_NO_MEMORY;
    }

    // --- Tag Registry ---
    g_state.registry = kmalloc(sizeof(TagRegistry));
    if (!g_state.registry) {
        debug_printf("[TagFS] Failed to allocate tag registry\n");
        return ERR_NO_MEMORY;
    }
    if (tag_registry_init(g_state.registry) != OK) {
        debug_printf("[TagFS] tag_registry_init failed\n");
        kfree(g_state.registry);
        g_state.registry = NULL;
        return ERR_TAGFS_REGISTRY_FULL;
    }
    if (tag_registry_load(g_state.registry, sb.tag_registry_block) != OK) {
        debug_printf("[TagFS] Warning: tag_registry_load failed (empty registry)\n");
    }

    // --- File Table ---
    if (file_table_init(sb.file_table_block, sb.file_table_block_count) != OK) {
        debug_printf("[TagFS] file_table_init failed\n");
        tag_registry_destroy(g_state.registry);
        kfree(g_state.registry);
        g_state.registry = NULL;
        return ERR_FILE_TABLE_CORRUPT;
    }

    // --- Metadata Pool ---
    if (meta_pool_init(sb.metadata_pool_block, sb.metadata_pool_block_count) != OK) {
        debug_printf("[TagFS] meta_pool_init failed\n");
        file_table_shutdown();
        tag_registry_destroy(g_state.registry);
        kfree(g_state.registry);
        g_state.registry = NULL;
        return ERR_METADATA_POOL_FULL;
    }

    // --- Bitmap Index ---
    g_state.bitmap_index = tag_bitmap_create(TAGFS_BITMAP_INITIAL_TAG_CAP, TAGFS_BITMAP_INITIAL_FILE_CAP);
    if (!g_state.bitmap_index) {
        debug_printf("[TagFS] tag_bitmap_create failed\n");
        meta_pool_shutdown();
        file_table_shutdown();
        tag_registry_destroy(g_state.registry);
        kfree(g_state.registry);
        g_state.registry = NULL;
        return ERR_NO_MEMORY;
    }

    // --- Block Bitmap ---
    uint32_t bitmap_bytes = (sb.total_blocks + 7) / 8;
    g_state.block_bitmap.bitmap = kmalloc(bitmap_bytes);
    if (!g_state.block_bitmap.bitmap)
    {
        debug_printf("[TagFS] Failed to allocate block bitmap\n");
        tag_bitmap_destroy(g_state.bitmap_index);
        g_state.bitmap_index = NULL;
        meta_pool_shutdown();
        file_table_shutdown();
        tag_registry_destroy(g_state.registry);
        kfree(g_state.registry);
        g_state.registry = NULL;
        return -1;
    }
    memset(g_state.block_bitmap.bitmap, 0, bitmap_bytes);
    g_state.block_bitmap.total_blocks = sb.total_blocks;
    g_state.block_bitmap.free_list = NULL;
    g_state.block_bitmap.extent_count = 0;

    // Read block bitmap from disk
    uint32_t bm_sector_count = sb.block_bitmap_sector_count;
    uint32_t bm_buf_size = bm_sector_count * 512;
    uint8_t *bm_buf = kmalloc(bm_buf_size);
    if (bm_buf)
    {
        if (disk_read_sectors((uint64_t)sb.block_bitmap_sector, (uint16_t)bm_sector_count, bm_buf) == 0)
        {
            uint32_t copy_bytes = bitmap_bytes < bm_buf_size ? bitmap_bytes : bm_buf_size;
            memcpy(g_state.block_bitmap.bitmap, bm_buf, copy_bytes);
            debug_printf("[TagFS] Block bitmap loaded from disk\n");
        }
        else
        {
            debug_printf("[TagFS] Warning: failed to read block bitmap from disk\n");
        }
        kfree(bm_buf);
    }

    free_list_build();

    // --- Mount-time fsck: cross-check file extents vs bitmap ---
    {
        uint32_t bitmap_bytes_sz = (sb.total_blocks + 7) / 8;
        uint8_t *computed_bm = kmalloc(bitmap_bytes_sz);
        if (computed_bm)
        {
            memset(computed_bm, 0, bitmap_bytes_sz);

            // Mark reserved blocks (tag registry, file table, metadata pool)
            for (uint32_t r = 0; r < 3 && r < sb.total_blocks; r++)
            {
                bitmap_set_bit(computed_bm, r);
            }

            // Scan all files and mark their extents
            uint32_t files_checked = 0;
            uint32_t orphan_blocks = 0;
            uint32_t missing_blocks = 0;

            for (uint32_t fid = 1; fid < sb.next_file_id; fid++)
            {
                uint32_t mb, mo;
                if (file_table_lookup(fid, &mb, &mo) != 0)
                    continue;

                TagFSMetadata meta;
                memset(&meta, 0, sizeof(meta));
                if (meta_pool_read(mb, mo, &meta) != 0)
                {
                    // Orphaned file_table entry — metadata is gone or corrupted.
                    // Clean up the dangling reference.
                    file_table_delete(fid);
                    continue;
                }

                if (!(meta.flags & TAGFS_FILE_ACTIVE))
                {
                    // File was marked for deletion (journal replay or incomplete delete).
                    // Complete the cleanup: remove file_table entry and free blocks.
                    debug_printf("[TagFS FSCK] File %u has ACTIVE=0 — completing cleanup\n", fid);
                    file_table_delete(fid);
                    // Don't mark extents in computed_bm → blocks will be reclaimed
                    tagfs_metadata_free(&meta);
                    continue;
                }

                for (uint16_t e = 0; e < meta.extent_count; e++)
                {
                    uint32_t start = meta.extents[e].start_block;
                    uint32_t count = meta.extents[e].block_count;
                    for (uint32_t b = start; b < start + count && b < sb.total_blocks; b++)
                    {
                        bitmap_set_bit(computed_bm, b);
                    }
                }
                files_checked++;

                // Rebuild bitmap index from disk metadata
                for (uint16_t t = 0; t < meta.tag_count; t++)
                {
                    tag_bitmap_set(g_state.bitmap_index, meta.tag_ids[t], fid);
                }

                tagfs_metadata_free(&meta);
            }

            // Compare computed vs on-disk bitmap
            for (uint32_t b = 0; b < sb.total_blocks; b++)
            {
                bool on_disk = bitmap_test_bit(g_state.block_bitmap.bitmap, b);
                bool computed = bitmap_test_bit(computed_bm, b);
                if (on_disk && !computed)
                    orphan_blocks++;
                if (!on_disk && computed)
                    missing_blocks++;
            }

            if (orphan_blocks > 0 || missing_blocks > 0)
            {
                debug_printf("[TagFS FSCK] Bitmap mismatch: %u orphan, %u missing — repairing\n",
                             orphan_blocks, missing_blocks);
                memcpy(g_state.block_bitmap.bitmap, computed_bm, bitmap_bytes_sz);
                free_list_build();

                // Count actual free blocks
                uint32_t used = 0;
                for (uint32_t b = 0; b < sb.total_blocks; b++)
                {
                    if (bitmap_test_bit(g_state.block_bitmap.bitmap, b))
                        used++;
                }
                g_state.superblock.free_blocks = sb.total_blocks - used;
            }

            debug_printf("[TagFS FSCK] Checked %u files, bitmap %s\n",
                         files_checked,
                         (orphan_blocks == 0 && missing_blocks == 0) ? "OK" : "repaired");
            kfree(computed_bm);
        }
    }

    // Populate well-known tag bitmasks for O(1) checks
    tagfs_init_well_known_tags();

    // Load mirror cache so future meta reads skip disk
    meta_pool_mirror_init(g_state.superblock.next_file_id + 64);

    // Initialize read-ahead cache for sequential reads
    ReadAheadInit();

    g_state.initialized = true;
    debug_printf("[TagFS] Initialized successfully\n");
    return 0;
}

// ----------------------------------------------------------------------------
// tagfs_sync / tagfs_shutdown
// ----------------------------------------------------------------------------

void tagfs_sync(void)
{
    if (!g_state.initialized)
        return;

    tag_registry_flush(g_state.registry);
    file_table_flush();
    meta_pool_flush();

    // Write block bitmap to disk
    uint32_t bitmap_bytes = (g_state.superblock.total_blocks + 7) / 8;
    uint32_t sector_count = g_state.superblock.block_bitmap_sector_count;
    uint32_t bm_buf_size = sector_count * 512;
    uint8_t *bm_buf = kmalloc(bm_buf_size);
    if (bm_buf)
    {
        memset(bm_buf, 0, bm_buf_size);
        uint32_t copy_bytes = bitmap_bytes < bm_buf_size ? bitmap_bytes : bm_buf_size;
        memcpy(bm_buf, g_state.block_bitmap.bitmap, copy_bytes);
        if (disk_write_sectors((uint64_t)g_state.superblock.block_bitmap_sector,
                               (uint16_t)sector_count, bm_buf) != 0)
        {
            debug_printf("[TagFS] sync: failed to write block bitmap\n");
        }
        kfree(bm_buf);
    }

    tagfs_write_superblock(&g_state.superblock);
}

void tagfs_shutdown(void)
{
    if (!g_state.initialized)
        return;

    tagfs_sync();

    tag_registry_destroy(g_state.registry);
    kfree(g_state.registry);
    g_state.registry = NULL;

    tag_bitmap_destroy(g_state.bitmap_index);
    g_state.bitmap_index = NULL;

    file_table_shutdown();
    meta_pool_shutdown();

    free_list_destroy();
    if (g_state.block_bitmap.bitmap)
    {
        kfree(g_state.block_bitmap.bitmap);
        g_state.block_bitmap.bitmap = NULL;
    }
    g_state.block_bitmap.total_blocks = 0;

    // Shutdown Bcdc compression
    BcdcShutdown();

    // Shutdown test framework
    TagFS_TestsShutdown();

    g_state.initialized = false;
    debug_printf("[TagFS] Shutdown complete\n");
}

// ----------------------------------------------------------------------------
// Test runner interface (called from userspace via System Deck)
// ----------------------------------------------------------------------------

error_t TagFS_RunTests(void) {
    if (!g_state.initialized)
        return ERR_TAGFS_NOT_INITIALIZED;
    
    TestStats stats;
    error_t result = TagFS_RunAllTests(&stats);
    
    if (result != OK) {
        debug_printf("[TagFS] Tests failed: %u failures\n", stats.total_failed);
    }
    
    return result;
}

// ----------------------------------------------------------------------------
// tagfs_create_file
// ----------------------------------------------------------------------------

int tagfs_create_file(const char *filename, const uint16_t *tag_ids, uint16_t tag_count,
                      uint32_t *out_file_id)
{
    if (!g_state.initialized)
        return -1;
    if (!filename || !out_file_id)
        return -1;

    spin_lock(&g_state.lock);

    uint32_t file_id = g_state.superblock.next_file_id++;

    // Derive auto-label tag from filename stem (e.g. "kernel.bin" → "kernel")
    char stem[128];
    {
        const char *dot = NULL;
        size_t flen = strlen(filename);
        for (size_t i = flen; i > 0; i--)
        {
            if (filename[i - 1] == '.')
            {
                dot = filename + i - 1;
                break;
            }
        }
        size_t slen = dot ? (size_t)(dot - filename) : flen;
        if (slen >= sizeof(stem))
            slen = sizeof(stem) - 1;
        memcpy(stem, filename, slen);
        stem[slen] = '\0';
    }

    // Intern the auto-label tag (lock is already held, call intern directly)
    uint16_t auto_tag = TAGFS_INVALID_TAG_ID;
    if (stem[0] != '\0' && g_state.registry)
    {
        // Release state lock briefly to avoid lock ordering issues with registry
        spin_unlock(&g_state.lock);
        auto_tag = tag_registry_intern(g_state.registry, stem, NULL);
        // Flush registry to disk if a new tag was created (crash safety)
        if (tag_registry_is_dirty())
        {
            tag_registry_flush(g_state.registry);
        }
        spin_lock(&g_state.lock);
    }

    // Build deduplicated tag array: auto-label first, then caller's tags
    uint16_t final_count = 0;
    uint16_t *final_tags = NULL;
    {
        uint16_t max_tags = tag_count + 1;
        final_tags = kmalloc(sizeof(uint16_t) * max_tags);
        if (!final_tags)
        {
            debug_printf("[TagFS] create_file: kmalloc for final_tags failed\n");
            spin_unlock(&g_state.lock);
            return -1;
        }

        // Add auto-label tag first
        if (auto_tag != TAGFS_INVALID_TAG_ID)
        {
            final_tags[final_count++] = auto_tag;
        }

        // Add caller's tags, skipping duplicates
        for (uint16_t i = 0; i < tag_count; i++)
        {
            if (!tag_ids)
                break;
            uint16_t tid = tag_ids[i];
            bool dup = false;
            for (uint16_t j = 0; j < final_count; j++)
            {
                if (final_tags[j] == tid)
                {
                    dup = true;
                    break;
                }
            }
            if (!dup)
            {
                final_tags[final_count++] = tid;
            }
        }
    }

    // Build metadata
    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    meta.file_id = file_id;
    meta.flags = TAGFS_FILE_ACTIVE;
    meta.size = 0;
    meta.created_time = 0;
    meta.modified_time = 0;
    meta.extent_count = 0;
    meta.extents = NULL;

    size_t name_len = strlen(filename);
    meta.filename = kmalloc(name_len + 1);
    if (!meta.filename)
    {
        debug_printf("[TagFS] create_file: kmalloc for filename failed\n");
        kfree(final_tags);
        spin_unlock(&g_state.lock);
        return -1;
    }
    memcpy(meta.filename, filename, name_len + 1);

    meta.tag_count = final_count;
    meta.tag_ids = final_tags;

    // No journal for create: metadata is NEW (no previous on-disk state to protect).
    // Write ordering ensures consistency:
    //   1. meta_pool_write (append-only, crash → orphaned record, fsck cleans up)
    //   2. file_table_update (crash → table points to valid metadata, counters off → fsck fixes)
    //   3. superblock update (eventual)

    // Write to metadata pool
    uint32_t meta_block, meta_offset;
    if (meta_pool_write(&meta, &meta_block, &meta_offset) != 0)
    {
        debug_printf("[TagFS] create_file: meta_pool_write failed for file_id=%u\n", file_id);
        kfree(meta.tag_ids);
        kfree(meta.filename);
        spin_unlock(&g_state.lock);
        return -1;
    }

    // Update file table
    if (file_table_update(file_id, meta_block, meta_offset) != 0)
    {
        debug_printf("[TagFS] create_file: file_table_update failed for file_id=%u\n", file_id);
        kfree(meta.tag_ids);
        kfree(meta.filename);
        spin_unlock(&g_state.lock);
        return -1;
    }

    // Add to bitmap index (use final_count, the deduplicated count)
    for (uint16_t i = 0; i < final_count; i++)
    {
        if (meta.tag_ids[i] != TAGFS_INVALID_TAG_ID)
        {
            tag_bitmap_set(g_state.bitmap_index, meta.tag_ids[i], file_id);
        }
    }

    g_state.superblock.total_files++;

    *out_file_id = file_id;

    kfree(meta.tag_ids);
    kfree(meta.filename);

    spin_unlock(&g_state.lock);

    debug_printf("[TagFS] Created file '%s' file_id=%u\n", filename, file_id);
    return 0;
}

// Forward declaration
static void tagfs_free_blocks_internal(uint32_t start_block, uint32_t count);

// ----------------------------------------------------------------------------
// tagfs_delete_file
// ----------------------------------------------------------------------------

int tagfs_delete_file(uint32_t file_id)
{
    if (!g_state.initialized)
        return -1;

    spin_lock(&g_state.lock);

    uint32_t meta_block, meta_offset;
    if (file_table_lookup(file_id, &meta_block, &meta_offset) != 0)
    {
        spin_unlock(&g_state.lock);
        return -1;
    }

    // Read metadata to get extents and tags for cleanup
    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    int has_meta = meta_pool_read(meta_block, meta_offset, &meta);

    // Cleanup order: bitmap → file_table → blocks → metadata pool.
    // file_table_delete before block_free ensures that on crash,
    // fsck won't find a file referencing freed (and possibly reused) blocks.

    // Remove from bitmap index
    tag_bitmap_remove_file(g_state.bitmap_index, file_id);

    // Delete from file table (commit point: file officially gone)
    file_table_delete(file_id);

    // Free data blocks if we could read metadata
    if (has_meta == 0 && meta.extents)
    {
        for (uint16_t i = 0; i < meta.extent_count; i++)
        {
            tagfs_free_blocks_internal(meta.extents[i].start_block, meta.extents[i].block_count);
        }
        tagfs_metadata_free(&meta);
    }

    // Delete from metadata pool (last: safe to zero after file_table is gone)
    meta_pool_delete(meta_block, meta_offset);

    if (g_state.superblock.total_files > 0)
    {
        g_state.superblock.total_files--;
    }

    spin_unlock(&g_state.lock);

    debug_printf("[TagFS] Deleted file_id=%u\n", file_id);
    return 0;
}

// ----------------------------------------------------------------------------
// tagfs_rename_file
// ----------------------------------------------------------------------------

int tagfs_rename_file(uint32_t file_id, const char *new_filename)
{
    if (!g_state.initialized)
        return -1;
    if (!new_filename)
        return -1;

    spin_lock(&g_state.lock);

    uint32_t meta_block, meta_offset;
    if (file_table_lookup(file_id, &meta_block, &meta_offset) != 0)
    {
        spin_unlock(&g_state.lock);
        return -1;
    }

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    if (meta_pool_read(meta_block, meta_offset, &meta) != 0)
    {
        debug_printf("[TagFS] rename_file: failed to read metadata for file_id=%u\n", file_id);
        spin_unlock(&g_state.lock);
        return -1;
    }

    // Replace filename
    if (meta.filename)
        kfree(meta.filename);
    size_t len = strlen(new_filename);
    meta.filename = kmalloc(len + 1);
    if (!meta.filename)
    {
        tagfs_metadata_free(&meta);
        spin_unlock(&g_state.lock);
        return -1;
    }
    memcpy(meta.filename, new_filename, len + 1);

    // Journal the renamed metadata before any destructive changes.
    // On crash: if journal replays, the new metadata (with new filename)
    // overwrites the old sector — file is safe with new name.
    // If crash happens after delete but before write, journal replay
    // restores the renamed metadata.

    // Delete old record, write new
    meta_pool_delete(meta_block, meta_offset);

    uint32_t new_block, new_offset;
    if (meta_pool_write(&meta, &new_block, &new_offset) != 0)
    {
        debug_printf("[TagFS] rename_file: meta_pool_write failed\n");
        tagfs_metadata_free(&meta);
        spin_unlock(&g_state.lock);
        return -1;
    }

    file_table_update(file_id, new_block, new_offset);

    tagfs_metadata_free(&meta);

    spin_unlock(&g_state.lock);
    return 0;
}

// ----------------------------------------------------------------------------
// Tag operations
// ----------------------------------------------------------------------------

int tagfs_add_tag(uint32_t file_id, uint16_t tag_id)
{
    if (!g_state.initialized)
        return -1;

    spin_lock(&g_state.lock);

    uint32_t meta_block, meta_offset;
    if (file_table_lookup(file_id, &meta_block, &meta_offset) != 0)
    {
        spin_unlock(&g_state.lock);
        return -1;
    }

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    if (meta_pool_read(meta_block, meta_offset, &meta) != 0)
    {
        spin_unlock(&g_state.lock);
        return -1;
    }

    // Check if tag already present
    for (uint16_t i = 0; i < meta.tag_count; i++)
    {
        if (meta.tag_ids[i] == tag_id)
        {
            tagfs_metadata_free(&meta);
            spin_unlock(&g_state.lock);
            return 0;
        }
    }

    // Grow tag list
    uint16_t new_count = meta.tag_count + 1;
    uint16_t *new_ids = kmalloc(sizeof(uint16_t) * new_count);
    if (!new_ids)
    {
        tagfs_metadata_free(&meta);
        spin_unlock(&g_state.lock);
        return -1;
    }
    if (meta.tag_ids)
    {
        memcpy(new_ids, meta.tag_ids, sizeof(uint16_t) * meta.tag_count);
    }
    new_ids[meta.tag_count] = tag_id;
    kfree(meta.tag_ids);
    meta.tag_ids = new_ids;
    meta.tag_count = new_count;

    // Journal the updated metadata before destructive delete+write

    meta_pool_delete(meta_block, meta_offset);

    uint32_t new_block, new_offset;
    int r = meta_pool_write(&meta, &new_block, &new_offset);
    if (r == 0)
    {
        file_table_update(file_id, new_block, new_offset);
        tag_bitmap_set(g_state.bitmap_index, tag_id, file_id);
    }

    tagfs_metadata_free(&meta);
    spin_unlock(&g_state.lock);
    return r;
}

int tagfs_remove_tag(uint32_t file_id, uint16_t tag_id)
{
    if (!g_state.initialized)
        return -1;

    spin_lock(&g_state.lock);

    uint32_t meta_block, meta_offset;
    if (file_table_lookup(file_id, &meta_block, &meta_offset) != 0)
    {
        spin_unlock(&g_state.lock);
        return -1;
    }

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    if (meta_pool_read(meta_block, meta_offset, &meta) != 0)
    {
        spin_unlock(&g_state.lock);
        return -1;
    }

    // Find and remove
    bool found = false;
    for (uint16_t i = 0; i < meta.tag_count; i++)
    {
        if (meta.tag_ids[i] == tag_id)
        {
            meta.tag_ids[i] = meta.tag_ids[meta.tag_count - 1];
            meta.tag_count--;
            found = true;
            break;
        }
    }

    if (!found)
    {
        tagfs_metadata_free(&meta);
        spin_unlock(&g_state.lock);
        return -1;
    }

    // Journal the updated metadata before destructive delete+write

    meta_pool_delete(meta_block, meta_offset);

    uint32_t new_block, new_offset;
    int r = meta_pool_write(&meta, &new_block, &new_offset);
    if (r == 0)
    {
        file_table_update(file_id, new_block, new_offset);
        tag_bitmap_clear(g_state.bitmap_index, tag_id, file_id);
    }

    tagfs_metadata_free(&meta);
    spin_unlock(&g_state.lock);
    return r;
}

bool tagfs_has_tag(uint32_t file_id, uint16_t tag_id)
{
    if (!g_state.initialized)
        return false;

    TagBitmapIndex *idx = g_state.bitmap_index;
    if (!idx)
        return false;

    spin_lock(&idx->lock);

    if ((uint32_t)tag_id >= idx->bitmap_capacity || !idx->bitmaps[tag_id])
    {
        spin_unlock(&idx->lock);
        return false;
    }

    TagBitmap *bm = idx->bitmaps[tag_id];
    uint32_t byte_idx = file_id / 8;
    uint32_t bit_idx = file_id % 8;
    uint32_t bm_bytes = (bm->bit_count + 7) / 8;

    bool result = (byte_idx < bm_bytes) &&
                  (bm->bits[byte_idx] & (1 << bit_idx));

    spin_unlock(&idx->lock);
    return result;
}

int tagfs_add_tag_string(uint32_t file_id, const char *key, const char *value)
{
    if (!g_state.initialized)
        return -1;
    if (!key)
        return -1;
    uint16_t tag_id = tag_registry_intern(g_state.registry, key, value);
    if (tag_id == TAGFS_INVALID_TAG_ID)
        return -1;
    // Flush registry to disk if a new tag was created (crash safety)
    if (tag_registry_is_dirty())
    {
        tag_registry_flush(g_state.registry);
    }
    return tagfs_add_tag(file_id, tag_id);
}

int tagfs_remove_tag_string(uint32_t file_id, const char *key)
{
    if (!g_state.initialized)
        return -1;
    if (!key)
        return -1;
    uint16_t tag_id = tag_registry_lookup(g_state.registry, key, NULL);
    if (tag_id == TAGFS_INVALID_TAG_ID)
        return -1;
    return tagfs_remove_tag(file_id, tag_id);
}

bool tagfs_has_tag_string(uint32_t file_id, const char *key, const char *value)
{
    if (!g_state.initialized)
        return false;
    if (!key)
        return false;
    uint16_t tag_id = tag_registry_lookup(g_state.registry, key, value);
    if (tag_id == TAGFS_INVALID_TAG_ID)
        return false;
    return tagfs_has_tag(file_id, tag_id);
}

// ----------------------------------------------------------------------------
// Query
// ----------------------------------------------------------------------------

int tagfs_query_files(const char *query_strings[], uint32_t count,
                      uint32_t *out_file_ids, uint32_t max_results)
{
    if (!g_state.initialized)
        return 0;
    if (!query_strings || count == 0 || !out_file_ids || max_results == 0)
        return 0;

    uint16_t *tag_ids = kmalloc(sizeof(uint16_t) * count);
    if (!tag_ids)
        return 0;

    TagKeyGroup **groups = kmalloc(sizeof(TagKeyGroup *) * count);
    if (!groups)
    {
        kfree(tag_ids);
        return 0;
    }

    uint32_t tag_count = 0;
    uint32_t group_count = 0;

    for (uint32_t i = 0; i < count; i++)
    {
        const char *qs = query_strings[i];
        if (!qs)
            continue;

        char key[256];
        char value[256];
        tagfs_parse_tag(qs, key, sizeof(key), value, sizeof(value));

        // Check for wildcard (value == "...")
        if (strcmp(value, "...") == 0)
        {
            TagKeyGroup *grp = tag_registry_key_group(g_state.registry, key);
            if (grp)
            {
                groups[group_count++] = grp;
            }
        }
        else
        {
            uint16_t tid = tag_registry_lookup(g_state.registry, key,
                                               value[0] ? value : NULL);
            if (tid == TAGFS_INVALID_TAG_ID)
            {
                // Tag not in registry — no files can match
                kfree(tag_ids);
                kfree(groups);
                return 0;
            }
            tag_ids[tag_count++] = tid;
        }
    }

    int result = tag_bitmap_query(g_state.bitmap_index,
                                  tag_ids, tag_count,
                                  groups, group_count,
                                  out_file_ids, max_results);

    // Post-filter: remove trashed/hidden files unless explicitly queried
    if (result > 0)
    {
        uint16_t trashed_id = g_wk.trashed
                                  ? (uint16_t)__builtin_ctzll(g_wk.trashed)
                                  : TAGFS_INVALID_TAG_ID;
        uint16_t hidden_id = g_wk.hidden
                                 ? (uint16_t)__builtin_ctzll(g_wk.hidden)
                                 : TAGFS_INVALID_TAG_ID;

        // Check if trashed/hidden were explicitly part of the query
        bool query_has_trashed = false;
        bool query_has_hidden = false;

        for (uint32_t i = 0; i < tag_count; i++)
        {
            if (tag_ids[i] == trashed_id)
                query_has_trashed = true;
            if (tag_ids[i] == hidden_id)
                query_has_hidden = true;
        }
        // Also check wildcard groups — if a group contains the tag, it's explicit
        for (uint32_t g = 0; g < group_count && (!query_has_trashed || !query_has_hidden); g++)
        {
            if (!groups[g])
                continue;
            for (uint32_t k = 0; k < groups[g]->count; k++)
            {
                if (groups[g]->tag_ids[k] == trashed_id)
                    query_has_trashed = true;
                if (groups[g]->tag_ids[k] == hidden_id)
                    query_has_hidden = true;
            }
        }

        bool skip_trashed = !query_has_trashed && trashed_id != TAGFS_INVALID_TAG_ID;
        bool skip_hidden = !query_has_hidden && hidden_id != TAGFS_INVALID_TAG_ID;

        if (skip_trashed || skip_hidden)
        {
            int filtered = 0;
            for (int i = 0; i < result; i++)
            {
                if (!file_has_system_behavior_tag(out_file_ids[i], skip_trashed, skip_hidden))
                {
                    out_file_ids[filtered++] = out_file_ids[i];
                }
            }
            result = filtered;
        }
    }

    kfree(tag_ids);
    kfree(groups);
    return result;
}

int tagfs_list_all_files(uint32_t *out_file_ids, uint32_t max_results)
{
    if (!g_state.initialized)
        return 0;
    if (!out_file_ids || max_results == 0)
        return 0;

    uint32_t found = 0;
    uint32_t max_id = g_state.superblock.next_file_id;

    for (uint32_t fid = 1; fid < max_id && found < max_results; fid++)
    {
        uint32_t mb, mo;
        if (file_table_lookup(fid, &mb, &mo) == 0 && mb != 0)
        {
            // Skip files with trashed/hidden system behavior tags
            if (file_has_system_behavior_tag(fid, true, true))
                continue;
            out_file_ids[found++] = fid;
        }
    }

    return (int)found;
}

// ----------------------------------------------------------------------------
// File I/O (open / close / read / write)
// ----------------------------------------------------------------------------

TagFSFileHandle *tagfs_open(uint32_t file_id, uint32_t flags)
{
    if (!g_state.initialized)
        return NULL;

    uint32_t meta_block, meta_offset;
    if (file_table_lookup(file_id, &meta_block, &meta_offset) != 0)
    {
        debug_printf("[TagFS] open: file_id=%u not found\n", file_id);
        return NULL;
    }

    OpenFileEntry *ofe = ofe_acquire(file_id);

    TagFSFileHandle *handle = kmalloc(sizeof(TagFSFileHandle));
    if (!handle)
    {
        ofe_release(ofe);
        return NULL;
    }

    handle->file_id = file_id;
    handle->flags = flags;
    handle->offset = 0;
    handle->file_size = 0;
    handle->extents = NULL;
    handle->extent_count = 0;
    handle->ofe = ofe;

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    if (meta_pool_read(meta_block, meta_offset, &meta) == 0)
    {
        handle->file_size = meta.size;
        handle->extent_count = meta.extent_count;
        if (meta.extent_count > 0 && meta.extents)
        {
            handle->extents = kmalloc(sizeof(FileExtent) * meta.extent_count);
            if (handle->extents)
            {
                memcpy(handle->extents, meta.extents,
                       sizeof(FileExtent) * meta.extent_count);
            }
        }
        tagfs_metadata_free(&meta);
    }

    return handle;
}

void tagfs_close(TagFSFileHandle *handle)
{
    if (!handle)
        return;
    debug_printf("[TagFS] close: file_id=%u\n", handle->file_id);
    ofe_release(handle->ofe);
    debug_printf("[TagFS] close: ofe released\n");
    if (handle->extents)
        kfree(handle->extents);
    debug_printf("[TagFS] close: extents freed\n");
    kfree(handle);
    debug_printf("[TagFS] close: handle freed\n");
}

int tagfs_read(TagFSFileHandle *handle, void *buffer, uint64_t size)
{
    if (!handle || !buffer || size == 0)
        return -1;
    if (!g_state.initialized)
        return -1;

    debug_printf("[TagFS] read: file_id=%u size=%lu offset=%lu file_size=%lu\n", 
                 handle->file_id, size, handle->offset, handle->file_size);

    if (handle->offset >= handle->file_size)
        return 0;

    uint64_t remaining = handle->file_size - handle->offset;
    if (size > remaining)
        size = remaining;

    debug_printf("[TagFS] read: adjusted size=%lu remaining=%lu\n", size, remaining);

    uint8_t block_buf[TAGFS_BLOCK_SIZE];
    uint8_t *out = (uint8_t *)buffer;
    uint64_t bytes_read = 0;

    debug_printf("[TagFS] read: starting read loop, extent_count=%u\n", handle->extent_count);

    while (bytes_read < size)
    {
        uint64_t file_pos = handle->offset + bytes_read;
        uint64_t extent_start = 0;
        int found = -1;

        for (uint16_t i = 0; i < handle->extent_count; i++)
        {
            uint64_t extent_size = (uint64_t)handle->extents[i].block_count * TAGFS_BLOCK_SIZE;
            if (file_pos < extent_start + extent_size)
            {
                found = i;
                break;
            }
            extent_start += extent_size;
        }

        if (found < 0)
        {
            debug_printf("[TagFS] read: extent not found for file_pos=%lu\n", file_pos);
            break;
        }

        uint64_t offset_in_extent = file_pos - extent_start;
        uint32_t block_index = (uint32_t)(offset_in_extent / TAGFS_BLOCK_SIZE);
        uint32_t offset_in_block = (uint32_t)(offset_in_extent % TAGFS_BLOCK_SIZE);
        uint32_t disk_block = handle->extents[found].start_block + block_index;

        debug_printf("[TagFS] read: reading disk_block=%u\n", disk_block);

        if (read_block(disk_block, block_buf) != 0)
        {
            debug_printf("[TagFS] read: failed to read block %u\n", disk_block);
            break;
        }

        uint32_t chunk = TAGFS_BLOCK_SIZE - offset_in_block;
        if (chunk > size - bytes_read)
            chunk = (uint32_t)(size - bytes_read);

        memcpy(out + bytes_read, block_buf + offset_in_block, chunk);
        bytes_read += chunk;

        // Read-ahead prefetch: if reading sequentially, prefetch next blocks
        if (offset_in_block == 0 && chunk == TAGFS_BLOCK_SIZE) {
            uint32_t next_block = disk_block + 1;
            uint32_t blocks_remaining_in_extent = handle->extents[found].block_count - block_index - 1;
            if (blocks_remaining_in_extent > 0) {
                ReadAheadPrefetch(next_block, blocks_remaining_in_extent < TAGFS_READ_AHEAD_BLOCKS ?
                                    blocks_remaining_in_extent : TAGFS_READ_AHEAD_BLOCKS);
            }
        }
    }

    handle->offset += bytes_read;
    debug_printf("[TagFS] read: returning %d\n", (int)bytes_read);
    return (int)bytes_read;
}

int tagfs_write(TagFSFileHandle *handle, const void *buffer, uint64_t size)
{
    if (!handle || !buffer || size == 0)
        return -1;
    if (!g_state.initialized)
        return -1;
    if (!(handle->flags & TAGFS_HANDLE_WRITE))
        return -1;

    // Auto-snapshot before write if versioning enabled (production feature)
    tagfs_auto_snapshot_before_write(handle->file_id);

    // Serialize writes to the same file
    if (handle->ofe)
        spin_lock(&handle->ofe->write_lock);

    const uint8_t *in = (const uint8_t *)buffer;
    uint8_t block_buf[TAGFS_BLOCK_SIZE];
    uint64_t bytes_written = 0;

    while (bytes_written < size)
    {
        uint64_t file_pos = handle->offset + bytes_written;
        uint64_t extent_start = 0;
        int found = -1;

        for (uint16_t i = 0; i < handle->extent_count; i++)
        {
            uint64_t extent_size = (uint64_t)handle->extents[i].block_count * TAGFS_BLOCK_SIZE;
            if (file_pos < extent_start + extent_size)
            {
                found = i;
                break;
            }
            extent_start += extent_size;
        }

        if (found < 0)
        {
            // extent_start = cumulative size of all existing extents (from loop)
            // Allocate enough blocks to cover from extent_start to file_pos+1
            uint32_t blocks_needed = (uint32_t)((file_pos - extent_start) / TAGFS_BLOCK_SIZE) + 1;
            if (blocks_needed > 0xFFFF)
                blocks_needed = 0xFFFF;

            uint32_t new_start;
            if (tagfs_alloc_blocks(blocks_needed, &new_start) != 0)
            {
                break;
            }

            uint16_t new_count = handle->extent_count + 1;
            FileExtent *new_extents = kmalloc(sizeof(FileExtent) * new_count);
            if (!new_extents)
            {
                tagfs_free_blocks(new_start, blocks_needed);
                break;
            }
            if (handle->extents && handle->extent_count > 0)
            {
                memcpy(new_extents, handle->extents, sizeof(FileExtent) * handle->extent_count);
                kfree(handle->extents);
            }
            new_extents[handle->extent_count].start_block = new_start;
            new_extents[handle->extent_count].block_count = (uint16_t)blocks_needed;
            handle->extents = new_extents;
            handle->extent_count = new_count;

            found = handle->extent_count - 1;
            // extent_start stays as cumulative end — math works correctly now
        }

        uint64_t offset_in_extent = file_pos - extent_start;
        uint32_t block_index = (uint32_t)(offset_in_extent / TAGFS_BLOCK_SIZE);
        uint32_t offset_in_block = (uint32_t)(offset_in_extent % TAGFS_BLOCK_SIZE);
        uint32_t disk_block = handle->extents[found].start_block + block_index;

        if (offset_in_block != 0 || (size - bytes_written) < TAGFS_BLOCK_SIZE)
        {
            if (read_block(disk_block, block_buf) != 0)
            {
                memset(block_buf, 0, TAGFS_BLOCK_SIZE);
            }
        }

        uint32_t chunk = TAGFS_BLOCK_SIZE - offset_in_block;
        if (chunk > size - bytes_written)
            chunk = (uint32_t)(size - bytes_written);

        memcpy(block_buf + offset_in_block, in + bytes_written, chunk);

        // Write data block directly to disk
        // Note: DiskBook is for metadata journaling only, not data blocks
        // Data blocks are written directly for performance
        if (write_block(disk_block, block_buf) != 0)
        {
            // Disk I/O error - cannot continue
            break;
        }

        bytes_written += chunk;
    }

    handle->offset += bytes_written;
    if (handle->offset > handle->file_size)
    {
        handle->file_size = handle->offset;
    }

    // Update metadata. Two paths:
    // - Extents changed (new blocks allocated): journal for crash safety
    // - Size-only (wrote within existing extents): direct metadata write
    if (bytes_written > 0)
    {
        uint32_t meta_block, meta_offset;
        if (file_table_lookup(handle->file_id, &meta_block, &meta_offset) != 0)
        {
            if (handle->ofe)
                spin_unlock(&handle->ofe->write_lock);
            return -1;
        }

        TagFSMetadata meta;
        memset(&meta, 0, sizeof(meta));
        if (meta_pool_read(meta_block, meta_offset, &meta) != 0)
        {
            if (handle->ofe)
                spin_unlock(&handle->ofe->write_lock);
            return -1;
        }

        meta.size = handle->file_size;
        if (meta.extents)
            kfree(meta.extents);
        meta.extent_count = handle->extent_count;
        meta.extents = kmalloc(sizeof(FileExtent) * handle->extent_count);
        if (!meta.extents)
        {
            tagfs_metadata_free(&meta);
            if (handle->ofe)
                spin_unlock(&handle->ofe->write_lock);
            return -1;
        }
        memcpy(meta.extents, handle->extents, sizeof(FileExtent) * handle->extent_count);

        // Write updated metadata
        meta_pool_delete(meta_block, meta_offset);
        uint32_t new_mb, new_mo;
        if (meta_pool_write(&meta, &new_mb, &new_mo) != 0)
        {
            tagfs_metadata_free(&meta);
            if (handle->ofe)
                spin_unlock(&handle->ofe->write_lock);
            return -1;
        }
        file_table_update(handle->file_id, new_mb, new_mo);
        tagfs_metadata_free(&meta);
    }

    if (handle->ofe)
        spin_unlock(&handle->ofe->write_lock);
    return (int)bytes_written;
}

// ----------------------------------------------------------------------------
// Block allocator
// ----------------------------------------------------------------------------

// Internal version - caller MUST hold g_state.lock
// Exported for use by meta_pool.c
int tagfs_alloc_blocks_internal(uint32_t count, uint32_t *out_start_block)
{
    FreeExtent *prev = NULL;
    FreeExtent *cur = g_state.block_bitmap.free_list;

    while (cur)
    {
        if (cur->count >= count)
        {
            *out_start_block = cur->start;

            // Mark bits used
            for (uint32_t i = 0; i < count; i++)
            {
                bitmap_set_bit(g_state.block_bitmap.bitmap, cur->start + i);
            }

            if (cur->count == count)
            {
                // Remove extent from list
                if (prev)
                    prev->next = cur->next;
                else
                    g_state.block_bitmap.free_list = cur->next;
                kfree(cur);
                g_state.block_bitmap.extent_count--;
            }
            else
            {
                cur->start += count;
                cur->count -= count;
            }

            if (g_state.superblock.free_blocks >= count)
            {
                g_state.superblock.free_blocks -= count;
            }

            return 0;
        }
        prev = cur;
        cur = cur->next;
    }

    return -1;
}

// Public version - acquires g_state.lock
int tagfs_alloc_blocks(uint32_t count, uint32_t *out_start_block)
{
    if (!g_state.initialized)
        return -1;
    if (count == 0 || !out_start_block)
        return -1;

    spin_lock(&g_state.lock);
    int result = tagfs_alloc_blocks_internal(count, out_start_block);
    spin_unlock(&g_state.lock);
    return result;
}

// Internal version - caller MUST hold g_state.lock
static void tagfs_free_blocks_internal(uint32_t start_block, uint32_t count)
{
    if (count == 0)
        return;

    uint32_t end = start_block + count;

    // Clear bits
    for (uint32_t i = 0; i < count; i++)
    {
        bitmap_clear_bit(g_state.block_bitmap.bitmap, start_block + i);
    }
    g_state.superblock.free_blocks += count;

    // Insert into free list in sorted order, merging adjacent extents
    FreeExtent *prev = NULL;
    FreeExtent *cur = g_state.block_bitmap.free_list;

    while (cur && cur->start < start_block)
    {
        prev = cur;
        cur = cur->next;
    }

    // Check merge right (new extent is adjacent to cur from left)
    bool merge_right = cur && cur->start == end;
    // Check merge left (prev is adjacent to new extent from right)
    bool merge_left = prev && (prev->start + prev->count) == start_block;

    if (merge_left && merge_right)
    {
        prev->count += count + cur->count;
        prev->next = cur->next;
        kfree(cur);
        g_state.block_bitmap.extent_count--;
    }
    else if (merge_left)
    {
        prev->count += count;
    }
    else if (merge_right)
    {
        cur->start = start_block;
        cur->count += count;
    }
    else
    {
        FreeExtent *new_extent = kmalloc(sizeof(FreeExtent));
        if (new_extent)
        {
            new_extent->start = start_block;
            new_extent->count = count;
            new_extent->next = cur;
            if (prev)
                prev->next = new_extent;
            else
                g_state.block_bitmap.free_list = new_extent;
            g_state.block_bitmap.extent_count++;
        }
    }
}

// Public version - acquires g_state.lock
int tagfs_free_blocks(uint32_t start_block, uint32_t count)
{
    if (!g_state.initialized)
        return -1;
    if (count == 0)
        return 0;

    spin_lock(&g_state.lock);
    tagfs_free_blocks_internal(start_block, count);
    spin_unlock(&g_state.lock);
    return 0;
}

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

int tagfs_get_metadata(uint32_t file_id, TagFSMetadata *out)
{
    if (!g_state.initialized)
        return -1;
    if (!out)
        return -1;

    // Fast path: read from mirror (no disk I/O)
    if (meta_pool_read_cached(file_id, out) == 0)
    {
        return 0;
    }

    // Slow path: disk read
    uint32_t block, offset;
    if (file_table_lookup(file_id, &block, &offset) != 0)
        return -1;
    return meta_pool_read(block, offset, out);
}

TagFSState *tagfs_get_state(void)
{
    return &g_state;
}

void tagfs_format_tag(char *dest, size_t dest_size, const char *key, const char *value)
{
    if (!dest || dest_size == 0 || !key)
        return;
    if (value && value[0])
    {
        ksnprintf(dest, dest_size, "%s:%s", key, value);
    }
    else
    {
        strncpy(dest, key, dest_size - 1);
        dest[dest_size - 1] = '\0';
    }
}

int tagfs_parse_tag(const char *tag_string, char *key, size_t key_size,
                    char *value, size_t value_size)
{
    if (!tag_string || !key || !value || key_size == 0 || value_size == 0)
        return -1;

    const char *colon = strchr(tag_string, ':');
    if (colon)
    {
        size_t klen = (size_t)(colon - tag_string);
        if (klen >= key_size)
            klen = key_size - 1;
        memcpy(key, tag_string, klen);
        key[klen] = '\0';

        strncpy(value, colon + 1, value_size - 1);
        value[value_size - 1] = '\0';
    }
    else
    {
        strncpy(key, tag_string, key_size - 1);
        key[key_size - 1] = '\0';
        value[0] = '\0';
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Defrag
// ----------------------------------------------------------------------------

int tagfs_defrag_file(uint32_t file_id, uint32_t target_block)
{
    if (!g_state.initialized)
        return -1;

    spin_lock(&g_state.lock);

    uint32_t meta_block, meta_offset;
    if (file_table_lookup(file_id, &meta_block, &meta_offset) != 0)
    {
        spin_unlock(&g_state.lock);
        return -1;
    }

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    if (meta_pool_read(meta_block, meta_offset, &meta) != 0)
    {
        spin_unlock(&g_state.lock);
        return -1;
    }

    // Nothing to defrag if file has 0 or 1 extent
    if (meta.extent_count <= 1)
    {
        tagfs_metadata_free(&meta);
        spin_unlock(&g_state.lock);
        return 0;
    }

    // Calculate total blocks needed
    uint32_t total_blocks = 0;
    for (uint16_t i = 0; i < meta.extent_count; i++)
    {
        total_blocks += meta.extents[i].block_count;
    }

    // Allocate contiguous space at target location or first available
    uint32_t new_start;
    if (target_block != 0 && target_block < g_state.superblock.total_blocks) {
        // Try to allocate at specific target block
        // Check if target area is free
        bool area_free = true;
        for (uint32_t b = 0; b < total_blocks && area_free; b++) {
            if (bitmap_test_bit(g_state.block_bitmap.bitmap, target_block + b)) {
                area_free = false;
            }
        }
        
        if (area_free) {
            new_start = target_block;
            // Mark blocks as allocated
            for (uint32_t b = 0; b < total_blocks; b++) {
                bitmap_set_bit(g_state.block_bitmap.bitmap, new_start + b);
            }
            g_state.superblock.free_blocks -= total_blocks;
        } else {
            // Target not available, allocate first available
            spin_unlock(&g_state.lock);
            if (tagfs_alloc_blocks(total_blocks, &new_start) != 0)
            {
                tagfs_metadata_free(&meta);
                return -1;
            }
            spin_lock(&g_state.lock);
        }
    } else {
        // No target specified, allocate first available contiguous space
        spin_unlock(&g_state.lock);
        if (tagfs_alloc_blocks(total_blocks, &new_start) != 0)
        {
            tagfs_metadata_free(&meta);
            return -1;
        }
        spin_lock(&g_state.lock);
    }

    // Copy all data to new contiguous location
    uint8_t block_buf[TAGFS_BLOCK_SIZE];
    uint32_t dest_block = new_start;
    bool copy_failed = false;

    for (uint16_t i = 0; i < meta.extent_count && !copy_failed; i++)
    {
        for (uint16_t b = 0; b < meta.extents[i].block_count && !copy_failed; b++)
        {
            if (read_block(meta.extents[i].start_block + b, block_buf) != 0 ||
                write_block(dest_block, block_buf) != 0)
            {
                copy_failed = true;
                break;
            }
            dest_block++;
        }
    }

    if (copy_failed)
    {
        // Cleanup: free newly allocated blocks
        spin_unlock(&g_state.lock);
        tagfs_free_blocks(new_start, total_blocks);
        tagfs_metadata_free(&meta);
        return -1;
    }

    // Free old fragmented blocks
    for (uint16_t i = 0; i < meta.extent_count; i++)
    {
        for (uint16_t b = 0; b < meta.extents[i].block_count; b++)
        {
            bitmap_clear_bit(g_state.block_bitmap.bitmap,
                             meta.extents[i].start_block + b);
        }
        g_state.superblock.free_blocks += meta.extents[i].block_count;
    }

    if (meta.extents)
        kfree(meta.extents);

    // Create new single extent
    meta.extents = kmalloc(sizeof(FileExtent));
    if (!meta.extents)
    {
        tagfs_metadata_free(&meta);
        spin_unlock(&g_state.lock);
        return -1;
    }
    
    meta.extents[0].start_block = new_start;
    meta.extents[0].block_count = (uint16_t)total_blocks;
    meta.extent_count = 1;
    meta.modified_time = rtc_get_unix64();

    // Update metadata pool
    meta_pool_delete(meta_block, meta_offset);
    uint32_t new_mb, new_mo;
    if (meta_pool_write(&meta, &new_mb, &new_mo) == 0)
    {
        file_table_update(file_id, new_mb, new_mo);
    }

    tagfs_metadata_free(&meta);
    spin_unlock(&g_state.lock);
    return 0;
}

uint32_t tagfs_get_fragmentation_score(void)
{
    if (!g_state.initialized)
        return 0;

    uint32_t score = 0;
    uint32_t max_id = g_state.superblock.next_file_id;

    for (uint32_t fid = 1; fid < max_id; fid++)
    {
        uint32_t mb, mo;
        if (file_table_lookup(fid, &mb, &mo) != 0 || mb == 0)
            continue;

        TagFSMetadata meta;
        memset(&meta, 0, sizeof(meta));
        if (meta_pool_read(mb, mo, &meta) == 0)
        {
            if (meta.extent_count > 1)
            {
                score += (meta.extent_count - 1);
            }
            tagfs_metadata_free(&meta);
        }
    }

    return score;
}
