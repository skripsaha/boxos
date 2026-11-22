#include "retire.h"
#include "../file_map/file_map.h"
#include "../ink/ink.h"
#include "../show/show.h"
#include "../mount/book_mount.h"
#include "../settle/settle_internal.h"
#include "../tag_registry/tag_registry.h"
#include "atomics.h"

#define RETIRE_PAGE         64u
#define RETIRE_NOT_BLOCKED  UINT64_MAX

static volatile uint32_t s_busy;
static volatile uint32_t s_held;
static volatile uint32_t s_owed;
static volatile uint64_t s_next_us;
static volatile uint64_t s_blocked_at;   /* the cycles settled when a look met a refusal */

void RetireOwed(void)
{
    __atomic_store_n(&s_owed, 1u, __ATOMIC_RELEASE);
}

bool RetireFileGone(uint64_t fid)
{
    FileRecord* rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    FileRecordRelease(rec);
    return !rec && !BookFileLocked(fid);
}

/* Every file that wears one of `ids`, each given one Ink that takes them all
 * off it — a page of files a time, from the least id on: a file that left the
 * index since was given up, and needs none. */
static error_t RetireFilesClear(const uint32_t* ids, uint32_t count)
{
    TagFSState* fs = tagfs_get_state();
    uint64_t    files[RETIRE_PAGE];
    for (uint32_t i = 0; i < count; i++) {
        uint64_t from = 0;
        do {
            TagFSPage page;
            if (TagBitmapQueryPage(fs->bitmap_index, &ids[i], 1, NULL, 0, NULL, 0, from, files, RETIRE_PAGE, false,
                                   &page) != 0)
                return ERR_TAGFS_NOT_INITIALIZED;
            for (uint32_t k = 0; k < page.delivered; k++) {
                InkReceipt receipt;
                error_t    rc = InkTagsOff(files[k], ids, count, &receipt);
                if (rc != OK && rc != ERR_FILE_NOT_FOUND)
                    return rc;
            }
            from = page.next;
        } while (from);
    }
    return OK;
}

/* One program's tags `ids`, `count` of them, taken off every file and retired. */
static error_t RetireProgram(const uint32_t* ids, uint32_t count)
{
    TagRegistry* reg = tagfs_get_state()->registry;
    error_t      rc  = OK;
    InkReceipt   receipt;
    for (uint32_t i = 0; rc == OK && i < count; i++)
        rc = TagRegistryRetire(reg, ids[i]);
    if (rc == OK)
        rc = RetireFilesClear(ids, count);
    if (rc == OK)
        rc = InkTagsRetire(ids, count, &receipt);
    return rc;
}

/* Every program that left taken out of the lists of the programs that show
 * their tags to it (FileRecord.shown_to): one Ink a reader a list, the files
 * that show asked of the map's list of them — of every file when it is short
 * of room. A list whose program left went with its card. */
static error_t RetireReaders(void)
{
    FileMap* map = tagfs_get_state()->files;
    uint64_t files[RETIRE_PAGE];
    for (uint64_t from = 0;;) {
        uint32_t n = FileMapShowing(map, from, files, RETIRE_PAGE);
        if (n == UINT32_MAX)
            n = FileMapFiles(map, from, files, RETIRE_PAGE);
        if (n == 0)
            return OK;
        for (uint32_t k = 0; k < n; k++) {
            FileRecord* rec = FileMapRecordHold(map, files[k], NULL);
            error_t     rc  = OK;
            for (uint32_t i = 0; rc == OK && rec && i < rec->shown_count; i++) {
                InkReceipt receipt;
                if (rec->shown_to[i] && RetireFileGone(rec->shown_to[i]))
                    rc = InkShown(files[k], rec->shown_to[i], false, &receipt);
            }
            FileRecordRelease(rec);
            if (rc != OK && rc != ERR_FILE_NOT_FOUND)
                return rc;
        }
        from = files[n - 1u] + 1u;
    }
}

static int RetireByProgram(const void* a, const void* b, void* ctx)
{
    const TagProgramTag* x = a;
    const TagProgramTag* y = b;
    (void)ctx;
    if (x->fid != y->fid)
        return x->fid < y->fid ? -1 : 1;
    return x->id < y->id ? -1 : x->id > y->id;
}

static error_t RetireSweep(uint32_t* retired)
{
    TagProgramTag* tags  = NULL;
    uint32_t       count = 0;
    error_t        rc    = TagRegistryProgramTags(tagfs_get_state()->registry, &tags, &count);
    uint32_t*      ids   = rc == OK && count ? kmalloc(sizeof(uint32_t) * count) : NULL;
    if (rc == OK && count && !ids)
        rc = ERR_NO_MEMORY;
    if (rc == OK && count) {
        KSort(tags, count, sizeof(*tags), RetireByProgram, NULL);
        for (uint32_t i = 0; i < count; i++)
            ids[i] = tags[i].id;
    }
    for (uint32_t from = 0, to; rc == OK && from < count; from = to) {
        for (to = from; to < count && tags[to].fid == tags[from].fid;)
            to++;
        if (!RetireFileGone(tags[from].fid))
            continue;
        rc = RetireProgram(&ids[from], to - from);
        if (rc == OK)
            *retired += to - from;
    }
    kfree(ids);
    kfree(tags);
    if (rc == OK)
        rc = RetireReaders();
    if (rc == OK)
        rc = ShowAsksForgetGone();
    return rc;
}

error_t RetireSweepNow(uint32_t* retired)
{
    *retired = 0;
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t rc = RetireSweep(retired);
    tagfs_leave();
    return rc;
}

/* Whether a look is due: the periodic one, or one owed that a refusal does
 * not hold back — it waits for a cycle that settled since, which gave room
 * back or let a medium come to rest. */
static bool RetireDue(void)
{
    if (SettleNowUs() >= __atomic_load_n(&s_next_us, __ATOMIC_ACQUIRE))
        return true;
    if (!__atomic_load_n(&s_owed, __ATOMIC_ACQUIRE))
        return false;
    uint64_t at = __atomic_load_n(&s_blocked_at, __ATOMIC_ACQUIRE);
    return at == RETIRE_NOT_BLOCKED || SettledCycles() > at;
}

void RetireServe(void)
{
    if (!RetireDue())
        return;
    if (__atomic_exchange_n(&s_busy, 1u, __ATOMIC_ACQUIRE))
        return;
    __atomic_store_n(&s_next_us, SettleNowUs() + RETIRE_LOOK_US, __ATOMIC_RELEASE);
    __atomic_store_n(&s_owed, 0u, __ATOMIC_RELEASE);
    if (!__atomic_load_n(&s_held, __ATOMIC_ACQUIRE)) {
        uint32_t retired = 0;
        error_t  rc      = RetireSweepNow(&retired);
        if (retired)
            kprintf("[TagFS] %u tag(s) of programs that left retired\n", retired);
        if (rc != OK && rc != ERR_TAGFS_NOT_INITIALIZED) {
            __atomic_store_n(&s_blocked_at, SettledCycles(), __ATOMIC_RELEASE);
            RetireOwed();
        } else {
            __atomic_store_n(&s_blocked_at, RETIRE_NOT_BLOCKED, __ATOMIC_RELEASE);
        }
    }
    __atomic_store_n(&s_busy, 0u, __ATOMIC_RELEASE);
}

void RetireMountReset(void)
{
    __atomic_store_n(&s_next_us, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_blocked_at, RETIRE_NOT_BLOCKED, __ATOMIC_RELEASE);
    RetireOwed();
}

bool RetireHold(bool held)
{
    bool was = __atomic_exchange_n(&s_held, held ? 1u : 0u, __ATOMIC_ACQ_REL) != 0;
    if (held)
        while (__atomic_load_n(&s_busy, __ATOMIC_ACQUIRE))
            cpu_pause();
    return was;
}
