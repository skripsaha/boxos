#include "cabin.h"
#include "process.h"
#include "klib.h"
#include "pmm.h"
#include "vmm.h"
#include "memtag.h"
#include "kring.h"
#include "touch_ring.h"
#include "aslr.h"
#include "cabin_layout.h"
#include "tagfs.h"
#include "use_context.h"
#include "auth_tags.h"
#include "atomics.h"

/*
 * cabin_set_tag_bit — intern a single tag id into the cabin's fast-path
 * bitfield or overflow array.  Mirrors the former process_set_tag_bit logic.
 * Caller must hold whatever lock guards tag mutation (process_lock in P1).
 */
static int cabin_set_tag_bit(cabin_t *cabin, uint16_t tag_id)
{
    if (tag_id < 64)
    {
        __atomic_or_fetch(&cabin->tag_bits, ((uint64_t)1 << tag_id), __ATOMIC_RELAXED);
        return 0;
    }

    for (uint16_t i = 0; i < cabin->tag_overflow_count; i++)
    {
        if (cabin->tag_overflow_ids[i] == tag_id)
            return 0;
    }

    if (cabin->tag_overflow_count >= cabin->tag_overflow_capacity)
    {
        uint16_t new_cap = cabin->tag_overflow_capacity == 0 ? 8 : cabin->tag_overflow_capacity * 2;
        uint16_t *new_ids = kmalloc(sizeof(uint16_t) * new_cap);
        if (!new_ids)
            return -1;

        uint16_t *old_ids = cabin->tag_overflow_ids;
        if (old_ids)
            memcpy(new_ids, old_ids, sizeof(uint16_t) * cabin->tag_overflow_count);

        __atomic_store_n(&cabin->tag_overflow_ids, new_ids, __ATOMIC_RELEASE);
        __atomic_store_n(&cabin->tag_overflow_capacity, new_cap, __ATOMIC_RELEASE);

        mfence();

        if (old_ids)
            kfree(old_ids);
    }

    cabin->tag_overflow_ids[cabin->tag_overflow_count++] = tag_id;
    return 0;
}

cabin_t *cabin_create(uint32_t pid, const char *tags)
{
    cabin_t *cabin = kmalloc(sizeof(cabin_t));
    if (!cabin)
        return NULL;

    memset(cabin, 0, sizeof(cabin_t));

    uint64_t info_phys   = 0;
    uint64_t pocket_phys = 0;
    uint64_t result_phys = 0;
    uint64_t touch_phys  = 0;

    vmm_context_t *vmm = vmm_create_cabin(&info_phys, &pocket_phys, &result_phys, &touch_phys);
    if (!vmm)
    {
        kfree(cabin);
        return NULL;
    }

    cabin->vmm              = vmm;
    cabin->cabin_info_phys  = info_phys;
    cabin->pocket_ring_phys = pocket_phys;
    cabin->result_ring_phys = result_phys;
    cabin->touch_ring_phys  = touch_phys;

    KRingPocketInit((PocketRing *)vmm_phys_to_virt(pocket_phys));
    KRingResultInit((ResultRing *)vmm_phys_to_virt(result_phys));
    KTouchRingInit((TouchRing *)vmm_phys_to_virt(touch_phys));

    char cabin_tag[32];
    ksnprintf(cabin_tag, sizeof(cabin_tag), "cabin:%u", pid);

    MemTagApplyByPhys(pocket_phys, CABIN_POCKET_RING_PAGES, "purpose:shared");
    MemTagApplyByPhys(pocket_phys, CABIN_POCKET_RING_PAGES, "purpose:pocket-ring");
    MemTagApplyByPhys(pocket_phys, CABIN_POCKET_RING_PAGES, cabin_tag);

    MemTagApplyByPhys(result_phys, CABIN_RESULT_RING_PAGES, "purpose:shared");
    MemTagApplyByPhys(result_phys, CABIN_RESULT_RING_PAGES, "purpose:result-ring");
    MemTagApplyByPhys(result_phys, CABIN_RESULT_RING_PAGES, cabin_tag);

    MemTagApplyByPhys(touch_phys,  CABIN_TOUCH_RING_PAGES,  "purpose:shared");
    MemTagApplyByPhys(touch_phys,  CABIN_TOUCH_RING_PAGES,  "purpose:touch-ring");
    MemTagApplyByPhys(touch_phys,  CABIN_TOUCH_RING_PAGES,  cabin_tag);

    cabin->code_start = VMM_CABIN_CODE_START;
    cabin->code_size  = 0;

    spinlock_init(&cabin->subs_lock);
    cabin->subs_head = NULL;

    spinlock_init(&cabin->bay_lock);
    cabin->bay_claims_head = NULL;
    cabin->bay_va_next     = CABIN_BAY_BASE;
    cabin->tme_keyids_held = 0;

    spinlock_init(&cabin->brook_lock);
    cabin->brook_claims_head = NULL;
    cabin->brook_va_next     = CABIN_BROOK_BASE;

    spinlock_init(&cabin->hammock_lock);
    cabin->hammock_va_next   = CABIN_HAMMOCK_BASE;

    aslr_offsets_t aslr   = aslr_generate();
    cabin->aslr_stack_top     = VMM_USER_STACK_TOP - aslr.stack_offset;
    cabin->aslr_heap_base     = CABIN_HEAP_BASE + aslr.heap_offset;
    cabin->aslr_buf_heap_base = CABIN_BUF_HEAP_START + aslr.buf_heap_offset;
    cabin->buf_heap_next      = cabin->aslr_buf_heap_base;

    if (tags && tags[0] != '\0')
    {
        const char *pos = tags;
        while (*pos)
        {
            const char *comma = strchr(pos, ',');
            size_t len = comma ? (size_t)(comma - pos) : strlen(pos);
            if (len > 0 && len < 256)
            {
                char tag_buf[256];
                memcpy(tag_buf, pos, len);
                tag_buf[len] = '\0';

                char key[256], value[256];
                tagfs_parse_tag(tag_buf, key, sizeof(key), value, sizeof(value));

                uint16_t tid = tagfs_tag_intern(tag_buf);
                if (tid != TAGFS_INVALID_TAG_ID)
                {
                    /* The user's context may name this tag without yet having
                     * a number for it; this is the number. */
                    UseContextBindTag(tag_buf, tid);
                    cabin_set_tag_bit(cabin, tid);
                    /* Mirror the fixed auth bit for a bare auth key. Plain
                     * OR: the cabin is not published to any core yet. */
                    if (!value[0])
                        cabin->auth_bits |= auth_bit_for_key(key);
                }
            }
            if (!comma)
                break;
            pos = comma + 1;
        }
    }

    atomic_store_u32(&cabin->strand_count, 1);

    return cabin;
}

void cabin_destroy(cabin_t *cabin)
{
    if (!cabin)
        return;

    /* Clear the MemTag bookkeeping on the shared IPC ring pages before
     * vmm_destroy_context frees them.  These tags are CABIN-wide (every
     * strand shares the rings), so the clear belongs here at last-strand
     * teardown — NOT in per-strand process_destroy, where a non-last
     * strand's exit would wrongly strip the tags from rings its siblings
     * are still using.  Mirrors the apply in cabin_create. */
    if (cabin->pocket_ring_phys) {
        MemTagClearByPhys(cabin->pocket_ring_phys, "purpose:shared");
        MemTagClearByPhys(cabin->pocket_ring_phys, "purpose:pocket-ring");
    }
    if (cabin->result_ring_phys) {
        MemTagClearByPhys(cabin->result_ring_phys, "purpose:shared");
        MemTagClearByPhys(cabin->result_ring_phys, "purpose:result-ring");
    }
    if (cabin->touch_ring_phys) {
        MemTagClearByPhys(cabin->touch_ring_phys, "purpose:shared");
        MemTagClearByPhys(cabin->touch_ring_phys, "purpose:touch-ring");
    }

    if (cabin->tag_overflow_ids)
    {
        kfree(cabin->tag_overflow_ids);
        cabin->tag_overflow_ids      = NULL;
        cabin->tag_overflow_count    = 0;
        cabin->tag_overflow_capacity = 0;
    }

    /* Free retired overflow buffers (kept alive past each realloc so a
     * lock-free reader never dereferenced freed memory — see
     * TagOverflowRetired).  Safe to free now: cabin teardown means no
     * strand survives to read them. */
    {
        TagOverflowRetired *r = cabin->tag_overflow_retired;
        while (r)
        {
            TagOverflowRetired *next = r->next;
            kfree(r->buf);
            kfree(r);
            r = next;
        }
        cabin->tag_overflow_retired = NULL;
    }

    if (cabin->vmm)
    {
        vmm_destroy_context(cabin->vmm);
        cabin->vmm = NULL;
    }

    kfree(cabin);
}

void cabin_ref_inc(cabin_t *cabin)
{
    if (!cabin)
        return;
    atomic_fetch_add_u32(&cabin->strand_count, 1);
}

void cabin_ref_dec(cabin_t *cabin)
{
    if (!cabin)
        return;
    uint32_t old = atomic_fetch_sub_u32(&cabin->strand_count, 1);
    if (old == 1)
        cabin_destroy(cabin);
}
