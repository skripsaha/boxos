#include "keep.h"
#include "../file_map/file_map.h"
#include "../ink/ink.h"
#include "../settle/settle_internal.h"
#include "rtc.h"
#include "atomics.h"

#define KEEP_PAGE  64u

static volatile uint32_t s_busy;
static volatile uint32_t s_held;
static volatile uint64_t s_next_us;

uint64_t KeepNowNs(void)
{
    return rtc_get_unix64() * 1000000000ull;
}

uint64_t KeepUntil(uint64_t term_seconds)
{
    const uint64_t now = KeepNowNs();
    if (term_seconds > (UINT64_MAX - now) / 1000000000ull)
        return UINT64_MAX;
    return now + term_seconds * 1000000000ull;
}

static bool KeepExpired(uint64_t fid, uint64_t now_ns)
{
    FileRecord* rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    if (!rec)
        return false;
    const bool due = (rec->flags & TAGFS_FILE_DELETED) && rec->keep_until <= now_ns;
    FileRecordRelease(rec);
    return due;
}

uint32_t KeepSweepNow(uint64_t now_ns)
{
    uint64_t ids[KEEP_PAGE];
    uint64_t from    = 1;
    uint32_t let_go  = 0;
    for (;;) {
        TagFSPage page;
        if (TagFSQueryKeptPage(NULL, 0, NULL, 0, from, ids, KEEP_PAGE, false, &page) != OK)
            break;
        for (uint32_t i = 0; i < page.delivered; i++) {
            InkReceipt receipt;
            if (KeepExpired(ids[i], now_ns) && InkQuitclaim(ids[i], INK_WARRANTED, &receipt) == OK)
                let_go++;
        }
        if (!page.next)
            break;
        from = page.next;
    }
    if (let_go)
        kprintf("[TagFS] %u deleted file(s) kept past their term, let go\n", let_go);
    return let_go;
}

void KeepServe(void)
{
    if (SettleNowUs() < __atomic_load_n(&s_next_us, __ATOMIC_ACQUIRE))
        return;
    if (__atomic_exchange_n(&s_busy, 1u, __ATOMIC_ACQUIRE))
        return;
    __atomic_store_n(&s_next_us, SettleNowUs() + KEEP_LOOK_US, __ATOMIC_RELEASE);
    if (!__atomic_load_n(&s_held, __ATOMIC_ACQUIRE) && tagfs_enter()) {
        (void)KeepSweepNow(KeepNowNs());
        tagfs_leave();
    }
    __atomic_store_n(&s_busy, 0u, __ATOMIC_RELEASE);
}

void KeepMountReset(void)
{
    __atomic_store_n(&s_next_us, 0, __ATOMIC_RELEASE);
}

bool KeepHold(bool held)
{
    bool was = __atomic_exchange_n(&s_held, held ? 1u : 0u, __ATOMIC_ACQ_REL) != 0;
    if (held)
        while (__atomic_load_n(&s_busy, __ATOMIC_ACQUIRE))
            cpu_pause();
    return was;
}
