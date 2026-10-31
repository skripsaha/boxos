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
#include "auth_tags.h"
#include "atomics.h"
#include "bay.h"       /* BayCleanupCabin — the claims are the cabin's */
#include "brook.h"     /* BrookCleanupCabin — the mapping half of a Brook claim */

/*
 * cabin_set_tag_bit — intern a single tag id into the cabin's fast-path
 * bitfield or overflow array.  Mirrors the former process_set_tag_bit logic.
 * Caller must hold whatever lock guards tag mutation (process_lock in P1).
 */
static int cabin_set_tag_bit(cabin_t *cabin, uint32_t tag_id)
{
    if (tag_id < 64)
    {
        __atomic_or_fetch(&cabin->tag_bits, ((uint64_t)1 << tag_id), __ATOMIC_RELAXED);
        return 0;
    }

    for (uint32_t i = 0; i < cabin->tag_overflow_count; i++)
    {
        if (cabin->tag_overflow_ids[i] == tag_id)
            return 0;
    }

    if (cabin->tag_overflow_count >= cabin->tag_overflow_capacity)
    {
        uint32_t new_cap = cabin->tag_overflow_capacity == 0 ? 8 : cabin->tag_overflow_capacity * 2;
        uint32_t *new_ids = kmalloc(sizeof(uint32_t) * new_cap);
        if (!new_ids)
            return -1;

        uint32_t *old_ids = cabin->tag_overflow_ids;
        if (old_ids)
            memcpy(new_ids, old_ids, sizeof(uint32_t) * cabin->tag_overflow_count);

        __atomic_store_n(&cabin->tag_overflow_ids, new_ids, __ATOMIC_RELEASE);
        __atomic_store_n(&cabin->tag_overflow_capacity, new_cap, __ATOMIC_RELEASE);

        mfence();

        if (old_ids)
            kfree(old_ids);
    }

    /* The number before the count that shows it, for the Use Context's
     * lock-free count of what a cabin wears. */
    cabin->tag_overflow_ids[cabin->tag_overflow_count] = tag_id;
    __atomic_store_n(&cabin->tag_overflow_count, cabin->tag_overflow_count + 1, __ATOMIC_RELEASE);
    return 0;
}

/*
 * The cabin's tags, read by the one spelling (tag_spelling.h) out of the
 * comma list in place. A tag that does not spell, or that the volume refuses a
 * number, refuses the cabin: a process is never started without a tag it was
 * given. With no volume up there is no number to have, and the tag is only
 * not a membership yet — the cabin is made.
 */
static error_t cabin_wear_tags(cabin_t *cabin, const char *tags)
{
    size_t len = tags ? strlen(tags) : 0;
    size_t at  = 0;
    for (;;)
    {
        TagSpelling tag;
        error_t rc = TagListNext(tags, len, &at, &tag);
        if (rc != OK || !tag.key)
            return rc;

        /* The intern tells the user's context the number, if the context
         * names this tag and had none for it yet. */
        uint32_t tid;
        rc = TagFSInternSpelling(&tag, &tid);
        if (rc == ERR_TAGFS_NOT_INITIALIZED)
            continue;
        if (rc != OK)
            return rc;

        if (cabin_set_tag_bit(cabin, tid) != 0)
            return ERR_NO_MEMORY;
        /* Mirror the fixed auth bit for a bare auth key. Plain OR: the cabin
         * is not published to any core yet. */
        if (!tag.value)
            cabin->auth_bits |= AuthBitForKey(tag.key, tag.key_len);
    }
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

    if (cabin_wear_tags(cabin, tags) != OK)
    {
        cabin_destroy(cabin);
        return NULL;
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

    kfree(cabin->hammock_free);
    cabin->hammock_free = NULL;

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

    /* Every Bay window this cabin holds goes now, while its page tables are
     * still standing. It cannot go at strand death: the window is in an
     * address space the siblings are still running in. And it cannot go after
     * the line below: vmm_destroy_context would walk Bay-owned pages as though
     * they were this cabin's alone and hand them back to the PMM while another
     * cabin still holds the same tag. */
    BayCleanupCabin(cabin);

    /* Brook's mapping half, for the same reason and with the same deadline.
     * Its seats are already vacated — each strand did that as it died — but
     * the header and slot pages are still in these page tables, and they are
     * shared with whatever cabin holds the other end of the stream. Left for
     * vmm_destroy_context they would be handed back to the PMM under a live
     * peer. */
    BrookCleanupCabin(cabin);

    /* The leaves of the MemTag held-tag set. Unlike Bay and Brook above
     * these are plain kernel-heap bitmaps with no relationship to this
     * cabin's page tables, so vmm_destroy_context is not a deadline they
     * race — but they go here rather than earlier because everything
     * above still gets to ask what this cabin held while it unwinds
     * (Bay and Brook gate their windows on memtag guards), and they go
     * before vmm_destroy_context so all of the cabin's heap-side
     * bookkeeping is released in one place ahead of the point where the
     * address space stops existing.
     *
     * Freeing at all is safe only here: a leaf is install-once precisely
     * so the lock-free readers (MemRegionAccessAllowed, reached from #PF)
     * may dereference one without a lock, and strand_count has already
     * reached 0, so no strand remains that could resolve this cabin
     * through process_find_ref and start such a read. */
    MemTagHeldSetRelease(cabin->active_memtags);

    if (cabin->vmm)
    {
        vmm_destroy_context(cabin->vmm);
        cabin->vmm = NULL;
    }

    kfree(cabin);
}

bool cabin_hammock_take(cabin_t *cabin, uint64_t *slot_base)
{
    bool taken = true;

    spin_lock(&cabin->hammock_lock);
    if (cabin->hammock_free_count > 0)
    {
        *slot_base = cabin->hammock_free[--cabin->hammock_free_count];
    }
    else if (cabin->hammock_va_next + CABIN_HAMMOCK_SLOT_SIZE <= CABIN_HAMMOCK_END)
    {
        *slot_base = cabin->hammock_va_next;
        cabin->hammock_va_next += CABIN_HAMMOCK_SLOT_SIZE;
    }
    else
    {
        taken = false;
    }
    spin_unlock(&cabin->hammock_lock);
    return taken;
}

void cabin_hammock_give_back(cabin_t *cabin, uint64_t slot_base)
{
    for (;;)
    {
        spin_lock(&cabin->hammock_lock);

        if (cabin->hammock_free_count < cabin->hammock_free_capacity)
        {
            cabin->hammock_free[cabin->hammock_free_count++] = slot_base;
            spin_unlock(&cabin->hammock_lock);
            return;
        }

        uint32_t want = cabin->hammock_free_capacity ? cabin->hammock_free_capacity * 2 : 16;
        spin_unlock(&cabin->hammock_lock);

        uint64_t *bigger = kmalloc((size_t)want * sizeof(uint64_t));
        if (!bigger)
        {
            spin_lock(&cabin->hammock_lock);
            if (slot_base + CABIN_HAMMOCK_SLOT_SIZE == cabin->hammock_va_next)
            {
                cabin->hammock_va_next = slot_base;
                spin_unlock(&cabin->hammock_lock);
                return;
            }
            spin_unlock(&cabin->hammock_lock);
            kprintf("[STRAND] hammock slot 0x%lx not given back: no memory for the list; "
                    "the cabin's address space keeps it unused\n", (unsigned long)slot_base);
            return;
        }

        spin_lock(&cabin->hammock_lock);
        if (cabin->hammock_free_capacity >= want)
        {
            spin_unlock(&cabin->hammock_lock);
            kfree(bigger);
            continue;
        }
        uint64_t *smaller = cabin->hammock_free;
        if (smaller)
            memcpy(bigger, smaller, (size_t)cabin->hammock_free_count * sizeof(uint64_t));
        cabin->hammock_free = bigger;
        cabin->hammock_free_capacity = want;
        spin_unlock(&cabin->hammock_lock);
        kfree(smaller);
    }
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
