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
 * (system.give_back), and every one by the program's end. A page given back
 * goes back to the machine and its place to the next deposit, the lowest
 * free place first; so a program that lives for years and is told something
 * every frame holds only what it has not read yet.
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

    uint64_t *maps = kmalloc((size_t)grown * 2u * sizeof(uint64_t));
    if (!maps) return false;
    memset(maps, 0, (size_t)grown * 2u * sizeof(uint64_t));

    uint64_t *old = maps;
    spin_lock(&cabin->deposit_lock);
    if (cabin->deposit_words == words) {
        if (words) {
            memcpy(maps, cabin->deposit_held, (size_t)words * sizeof(uint64_t));
            memcpy(maps + grown, cabin->deposit_lent, (size_t)words * sizeof(uint64_t));
        }
        old                  = cabin->deposit_held;
        cabin->deposit_held  = maps;
        cabin->deposit_lent  = maps + grown;
        cabin->deposit_words = (uint32_t)grown;
    }
    spin_unlock(&cabin->deposit_lock);
    kfree(old);
    return true;
}

static uint32_t DepositPages(uint32_t length)
{
    return (uint32_t)(((uint64_t)length + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);
}

/* Unmap `pages` pages from `va` and give each frame back to the machine. */
static void DepositUnmap(cabin_t *cabin, uint64_t va, uint32_t pages)
{
    for (uint32_t i = 0; i < pages; i++) {
        uintptr_t at   = (uintptr_t)va + (uintptr_t)i * PMM_PAGE_SIZE;
        uintptr_t phys = vmm_virt_to_phys(cabin->vmm, at);
        vmm_unmap_page(cabin->vmm, at);
        if (phys) pmm_free((void *)(phys & ~(uintptr_t)(PMM_PAGE_SIZE - 1)), 1);
    }
}

/* Places taken back into the free: lowest-first finds them again. */
static void DepositFree(cabin_t *cabin, uint32_t first, uint32_t pages)
{
    spin_lock(&cabin->deposit_lock);
    DepositMark(cabin->deposit_held, first, pages, false);
    if (first < cabin->deposit_low) cabin->deposit_low = first;
    spin_unlock(&cabin->deposit_lock);
}

uint64_t cabin_heap_place(process_t *target, uint32_t length)
{
    if (!target || !target->cabin || length == 0) return 0;
    cabin_t *cabin = target->cabin;
    uint32_t pages = DepositPages(length);

    uint32_t first;
    for (;;) {
        spin_lock(&cabin->deposit_lock);
        first = cabin->deposit_words ? DepositFind(cabin, pages) : UINT32_MAX;
        if (first != UINT32_MAX) {
            DepositMark(cabin->deposit_held, first, pages, true);
            if (first == cabin->deposit_low) cabin->deposit_low = first + pages;
            spin_unlock(&cabin->deposit_lock);
            break;
        }
        uint32_t words = cabin->deposit_words;
        spin_unlock(&cabin->deposit_lock);
        if (!DepositGrow(cabin, words, pages)) return 0;
    }

    uint64_t va = cabin->aslr_buf_heap_base + (uint64_t)first * PMM_PAGE_SIZE;
    for (uint32_t i = 0; i < pages; i++) {
        void *page = pmm_alloc(1);
        if (page && vmm_map_page(cabin->vmm, va + (uint64_t)i * PMM_PAGE_SIZE, (uint64_t)page,
                                 VMM_FLAGS_USER_RW).success)
            continue;
        if (page) pmm_free(page, 1);
        DepositUnmap(cabin, va, i);
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
        DepositUnmap(cabin, addr, pages);
        DepositFree(cabin, first, pages);
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
     * places stay held until the pages are gone, so no deposit is mapped over
     * them meanwhile. */
    spin_lock(&cabin->deposit_lock);
    bool lent = first + pages <= (uint64_t)cabin->deposit_words * 64u;
    for (uint32_t i = 0; lent && i < pages; i++)
        lent = DepositHas(cabin->deposit_lent, (uint32_t)first + i);
    if (lent) DepositMark(cabin->deposit_lent, (uint32_t)first, pages, false);
    spin_unlock(&cabin->deposit_lock);
    if (!lent) return ERR_INVALID_ADDRESS;

    DepositUnmap(cabin, addr, pages);
    DepositFree(cabin, (uint32_t)first, pages);
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
