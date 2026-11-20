/*
 * System Deck — helpers (Phase 12: post-prefix-chain).
 *
 * The legacy switch-style dispatcher and per-opcode helpers are gone. All
 * system ops now live in system_ops.c on the Manifest path. This file keeps
 * what they share: the deposit window, where a program receives the bytes of
 * a message or a question (cabin.h, deposit_*).
 *
 * A deposit is lent: whole pages of the receiver's own, mapped for it and
 * written before it hears of them, and its until they are given back — a
 * question's by its answer, a message's by the program once it has read it
 * (system.give_back), and every one by the program's end. A place given back
 * goes to the next deposit, the lowest free place first, and its page stays
 * mapped there for it while the program keeps fewer than DEPOSIT_KEPT_MOST
 * such pages: a program told something every frame is told it into pages
 * already its, with no page taken from the machine, no mapping and no
 * unmapping — which asked every core the program runs on to forget the page,
 * one page at a time. Past that a page goes back to the machine, a run of
 * them unmapped with one such ask; so a program that lives for years holds
 * what it has not read yet and DEPOSIT_KEPT_MOST pages more at most.
 */

#include "system_deck.h"
#include "klib.h"
#include "process.h"
#include "vmm.h"
#include "pmm.h"

/* The most places a book holds: a page's place, and the place past a run of
 * them, are 32-bit numbers — sixteen TiB of window, well below the rings
 * (cabin_layout.h). */
#define DEPOSIT_WORDS_MAX  (0x100000000ull / 64u - 1u)

/* The most pages a program keeps mapped at free places for its next
 * deposits: 128 KiB, what a program told something every frame by a few
 * others has in flight. */
#define DEPOSIT_KEPT_MOST  32u

static bool DepositHas(const uint64_t *map, uint32_t place)
{
    return (map[place >> 6] >> (place & 63u)) & 1u;
}

static void DepositMark(uint64_t *map, uint32_t first, uint32_t pages, bool on)
{
    for (uint32_t place = first; place < first + pages; place++) {
        if (on) map[place >> 6] |= 1ull << (place & 63u);
        else    map[place >> 6] &= ~(1ull << (place & 63u));
    }
}

/* The first `pages` free places in a row at or above deposit_low, or
 * UINT32_MAX when the book has none that long. Caller holds deposit_lock. */
static uint32_t DepositFind(const cabin_t *cabin, uint32_t pages)
{
    uint64_t total = (uint64_t)cabin->deposit_words * 64u;
    uint32_t run   = 0;
    for (uint64_t place = cabin->deposit_low; place < total; place++) {
        if ((place & 63u) == 0 && cabin->deposit_held[place >> 6] == ~0ull) {
            run    = 0;
            place += 63u;
            continue;
        }
        if (DepositHas(cabin->deposit_held, (uint32_t)place)) {
            run = 0;
            continue;
        }
        if (++run == pages) return (uint32_t)(place + 1u - pages);
    }
    return UINT32_MAX;
}

/* The book made long enough to hold `pages` more past what it holds now.
 * The room is made outside the lock; a book another deposit grew meanwhile is
 * kept and the room let go. False when there is no room to make. */
static bool DepositGrow(cabin_t *cabin, uint32_t words, uint32_t pages)
{
    uint64_t grown = words ? (uint64_t)words * 2u : 1u;
    while (grown * 64u < (uint64_t)words * 64u + pages) grown *= 2u;
    if (grown > DEPOSIT_WORDS_MAX) return false;

    uint64_t *maps = kmalloc((size_t)grown * 3u * sizeof(uint64_t));
    if (!maps) return false;
    memset(maps, 0, (size_t)grown * 3u * sizeof(uint64_t));

    uint64_t *old = maps;
    spin_lock(&cabin->deposit_lock);
    if (cabin->deposit_words == words) {
        if (words) {
            memcpy(maps, cabin->deposit_held, (size_t)words * sizeof(uint64_t));
            memcpy(maps + grown, cabin->deposit_lent, (size_t)words * sizeof(uint64_t));
            memcpy(maps + 2u * grown, cabin->deposit_mapped, (size_t)words * sizeof(uint64_t));
        }
        old                   = cabin->deposit_held;
        cabin->deposit_held   = maps;
        cabin->deposit_lent   = maps + grown;
        cabin->deposit_mapped = maps + 2u * grown;
        cabin->deposit_words  = (uint32_t)grown;
    }
    spin_unlock(&cabin->deposit_lock);
    kfree(old);
    return true;
}

static uint32_t DepositPages(uint32_t length)
{
    return (uint32_t)(((uint64_t)length + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);
}

/* A run unmapped at most this many pages at a time: their frames known
 * before, freed after. */
#define DEPOSIT_UNMAP_RUN  64u

/* The pages at places `first` on, `pages` of them, unmapped a run at a time —
 * one ask of every core the program runs on to forget a run, not one a page —
 * each frame back to the machine only once no core can reach it through the
 * program's mapping, and the places no longer mapped. The places are held by
 * the caller, so no deposit is mapped over them meanwhile; a place no page is
 * mapped at is passed over. */
static void DepositUnmap(cabin_t *cabin, uint32_t first, uint32_t pages)
{
    uintptr_t frames[DEPOSIT_UNMAP_RUN];
    for (uint32_t done = 0; done < pages;) {
        uint32_t  run = pages - done < DEPOSIT_UNMAP_RUN ? pages - done : DEPOSIT_UNMAP_RUN;
        uintptr_t va  = (uintptr_t)(cabin->aslr_buf_heap_base + (uint64_t)(first + done) * PMM_PAGE_SIZE);
        for (uint32_t i = 0; i < run; i++)
            frames[i] = vmm_virt_to_phys(cabin->vmm, va + (uintptr_t)i * PMM_PAGE_SIZE);
        (void)vmm_unmap_range(cabin->vmm, va, run);
        for (uint32_t i = 0; i < run; i++)
            if (frames[i]) pmm_free((void *)(frames[i] & ~(uintptr_t)(PMM_PAGE_SIZE - 1)), 1);
        done += run;
    }
    spin_lock(&cabin->deposit_lock);
    DepositMark(cabin->deposit_mapped, first, pages, false);
    spin_unlock(&cabin->deposit_lock);
}

/* Places taken back into the free: lowest-first finds them again. */
static void DepositFree(cabin_t *cabin, uint32_t first, uint32_t pages)
{
    spin_lock(&cabin->deposit_lock);
    DepositMark(cabin->deposit_held, first, pages, false);
    if (first < cabin->deposit_low) cabin->deposit_low = first;
    spin_unlock(&cabin->deposit_lock);
}

/* The places `first` on given back: their pages kept mapped for the next
 * deposit while the program keeps fewer than DEPOSIT_KEPT_MOST, else back to
 * the machine — and the places free. */
static void DepositGiveBack(cabin_t *cabin, uint32_t first, uint32_t pages)
{
    spin_lock(&cabin->deposit_lock);
    bool keep = cabin->deposit_kept + pages <= DEPOSIT_KEPT_MOST;
    if (keep) {
        cabin->deposit_kept += pages;
        DepositMark(cabin->deposit_held, first, pages, false);
        if (first < cabin->deposit_low) cabin->deposit_low = first;
    }
    spin_unlock(&cabin->deposit_lock);
    if (keep) return;
    DepositUnmap(cabin, first, pages);
    DepositFree(cabin, first, pages);
}

uint64_t cabin_heap_place(process_t *target, uint32_t length)
{
    if (!target || !target->cabin || length == 0) return 0;
    cabin_t *cabin = target->cabin;
    uint32_t pages = DepositPages(length);

    /* The places, and which of them a kept page is mapped at already: those
     * are the deposit's as they stand, kept no longer. */
    uint32_t first, kept = 0;
    for (;;) {
        spin_lock(&cabin->deposit_lock);
        first = cabin->deposit_words ? DepositFind(cabin, pages) : UINT32_MAX;
        if (first != UINT32_MAX) {
            DepositMark(cabin->deposit_held, first, pages, true);
            if (first == cabin->deposit_low) cabin->deposit_low = first + pages;
            for (uint32_t i = 0; i < pages; i++)
                kept += DepositHas(cabin->deposit_mapped, first + i) ? 1u : 0u;
            cabin->deposit_kept -= kept;
            spin_unlock(&cabin->deposit_lock);
            break;
        }
        uint32_t words = cabin->deposit_words;
        spin_unlock(&cabin->deposit_lock);
        if (!DepositGrow(cabin, words, pages)) return 0;
    }

    uint64_t va = cabin->aslr_buf_heap_base + (uint64_t)first * PMM_PAGE_SIZE;
    if (kept == pages) return va;
    for (uint32_t i = 0; i < pages; i++) {
        spin_lock(&cabin->deposit_lock);
        bool mapped = DepositHas(cabin->deposit_mapped, first + i);
        spin_unlock(&cabin->deposit_lock);
        if (mapped) continue;
        void *page = pmm_alloc(1);
        if (page && vmm_map_page(cabin->vmm, va + (uint64_t)i * PMM_PAGE_SIZE, (uint64_t)page,
                                 VMM_FLAGS_USER_RW).success) {
            spin_lock(&cabin->deposit_lock);
            DepositMark(cabin->deposit_mapped, first + i, 1, true);
            spin_unlock(&cabin->deposit_lock);
            continue;
        }
        if (page) pmm_free(page, 1);
        DepositUnmap(cabin, first, pages);
        DepositFree(cabin, first, pages);
        return 0;
    }
    return va;
}

void cabin_heap_lend(process_t *target, uint64_t addr, uint32_t length, bool written)
{
    cabin_t *cabin = target->cabin;
    uint32_t first = (uint32_t)((addr - cabin->aslr_buf_heap_base) / PMM_PAGE_SIZE);
    uint32_t pages = DepositPages(length);
    if (!written) {
        DepositGiveBack(cabin, first, pages);
        return;
    }
    spin_lock(&cabin->deposit_lock);
    DepositMark(cabin->deposit_lent, first, pages, true);
    spin_unlock(&cabin->deposit_lock);
}

error_t cabin_heap_take_back(process_t *target, uint64_t addr, uint32_t length)
{
    if (!target || !target->cabin || length == 0) return ERR_INVALID_ARGUMENT;
    cabin_t *cabin = target->cabin;
    if (addr < cabin->aslr_buf_heap_base || (addr - cabin->aslr_buf_heap_base) % PMM_PAGE_SIZE)
        return ERR_INVALID_ADDRESS;
    uint64_t first = (addr - cabin->aslr_buf_heap_base) / PMM_PAGE_SIZE;
    uint32_t pages = DepositPages(length);

    /* Every page lent, or nothing is taken: the lent bits go first, under the
     * lock, so the same pages given back twice at once are taken once. The
     * places stay held until their pages are kept or gone, so no deposit is
     * mapped over them meanwhile. */
    spin_lock(&cabin->deposit_lock);
    bool lent = first + pages <= (uint64_t)cabin->deposit_words * 64u;
    for (uint32_t i = 0; lent && i < pages; i++)
        lent = DepositHas(cabin->deposit_lent, (uint32_t)first + i);
    if (lent) DepositMark(cabin->deposit_lent, (uint32_t)first, pages, false);
    spin_unlock(&cabin->deposit_lock);
    if (!lent) return ERR_INVALID_ADDRESS;

    DepositGiveBack(cabin, (uint32_t)first, pages);
    return OK;
}

uint64_t cabin_heap_deposit(process_t *target, const void *kbuf, uint32_t length)
{
    if (!kbuf) return 0;
    uint64_t va = cabin_heap_place(target, length);
    if (!va) return 0;

    /* Page-walk-copy the kernel buffer into the freshly-mapped target pages.
     * commit_out walks each page, so a multi-page target range (whose frames
     * pmm_alloc may hand out non-contiguously) is delivered correctly too. */
    bool written = vmm_user_buf_commit_out(target->cabin->vmm, va, kbuf, length) == OK;
    cabin_heap_lend(target, va, length, written);
    return written ? va : 0;
}

uint64_t ipc_copy_to_heap(process_t *sender, process_t *target,
                          uint64_t src_addr, uint32_t length)
{
    if (!sender || !target || length == 0 || src_addr == 0) {
        return 0;
    }

    /* Page-walk-copy the sender payload into a kernel bounce buffer. A
     * straddling source range is copied across every backing frame; the old
     * single-page translation clamped the range to its first page and the
     * memcpy below overran into whatever physical frame happened to follow
     * it, delivering a foreign frame's bytes to the target. */
    void *kbuf = vmm_user_buf_in(sender->cabin ? sender->cabin->vmm : NULL,
                                 src_addr, length);
    if (!kbuf) return 0;

    uint64_t target_vaddr = cabin_heap_deposit(target, kbuf, length);
    vmm_user_buf_free(kbuf);
    return target_vaddr;
}
