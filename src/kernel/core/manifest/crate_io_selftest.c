/*
 * crate_io_selftest — ironclad in-kernel proof of the crate-boundary straddle
 * fix.
 *
 * Builds two virtually-adjacent, physically NON-adjacent user pages (frame A
 * and frame B, separated by an unmapped physical "spacer" frame) plus a poison
 * guard page, then for a range of straddle offsets k drives both the LEGACY
 * single-frame path and the page-walked crate_io path over a Crate that spans
 * the A|B seam:
 *
 *   WRITE BACKSTOP raw vmm_translate_user_addr on a page-crossing range now
 *                  returns NULL (commit 7 fail-closed backstop), so the legacy
 *                  "translate + memcpy" can no longer reach the physically-next
 *                  SPACER frame: the straddle is refused and every frame stays
 *                  POISON.
 *   WRITE FIX      crate_write — both frame A's tail and frame B's head carry
 *                  the bytes, the spacer is UNtouched, the guard is intact.
 *   READ  BACKSTOP raw vmm_translate_user_addr on the straddle returns NULL, so
 *                  the legacy read can no longer snapshot the foreign SPACER
 *                  frame.
 *   READ  FIX      crate_read — the snapshot equals the planted bytes across
 *                  the seam, sourced from the two real frames.
 *
 * The BACKSTOP assertions prove the primitive is fail-closed: it refuses the
 * straddle and leaves every foreign frame untouched. The whole test is
 * deterministic — the contiguous 3-frame block guarantees frame B is two
 * physical frames above frame A, so the frame after A is always the spacer.
 *
 * The IPC section reuses the same NonContigPages helper for two cabins (a fake
 * SENDER and a fake TARGET) and proves the system/IPC straddle fix: it plants
 * a payload across the SENDER's seam and drives the converted ipc_copy_to_heap
 * source leg (vmm_user_buf_in) and dest leg (vmm_user_buf_commit_out) plus the
 * whole ipc_copy_to_heap end-to-end. BACKSTOP legs replay the legacy single-
 * frame read/write and assert the primitive now refuses the straddle (NULL),
 * leaving the foreign spacer untouched; FIX legs assert both real frames carry
 * the payload, spacer and guard intact. One "[CRATE-IO] SELFTEST PASS" covers
 * storage + IPC.
 */

#include "crate_io_selftest.h"
#include "crate_io.h"
#include "pmm.h"
#include "boxos_crate.h"
#include "op_registry.h"   /* OpContext */
#include "system_deck.h"   /* ipc_copy_to_heap — the converted IPC leaf */
#include "klib.h"

#define CRATE_IO_SELFTEST_VA   0x0000000040000000ULL  /* 1 GiB: 3 free user pages */
#define CRATE_IO_OVERFLOW      64u   /* bytes that overrun the A|B seam into B   */
#define CRATE_IO_POISON        0x5Au /* even; the source pattern is always odd   */
#define CRATE_IO_IPC_HEAP_OFF  0x0000000000100000ULL  /* target buf-heap VA, clear of A|B|guard */

/* Straddle offsets: k bytes precede the seam in frame A, OVERFLOW bytes follow
 * it in frame B. Iterating k moves the seam across the payload. */
static const unsigned CRATE_IO_K[] = {
    1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u, 256u, 512u, 1024u, 2048u
};

/* Source pattern + read-back scratch. Static (BSS), not stack — each is a full
 * page, and a single run never exceeds CRATE_IO_OVERFLOW + max(k) = 2112 bytes.
 * Bytes are always odd so they never collide with the even POISON value. */
static uint8_t g_src[VMM_PAGE_SIZE];
static uint8_t g_dst[VMM_PAGE_SIZE];

static void fill_src(void)
{
    for (size_t i = 0; i < sizeof(g_src); i++)
        g_src[i] = (uint8_t)(i * 2u + 1u);
}

static bool all_bytes(const uint8_t *p, unsigned len, uint8_t val)
{
    for (unsigned i = 0; i < len; i++)
        if (p[i] != val) return false;
    return true;
}

bool NonContigPagesSetup(NonContigPages *m, uintptr_t base_va)
{
    memset(m, 0, sizeof(*m));
    m->base_va = base_va;

    m->vmm = vmm_create_context();
    if (!m->vmm) return false;

    m->cabin = kmalloc(sizeof(cabin_t));
    m->proc  = kmalloc(sizeof(process_t));
    if (!m->cabin || !m->proc) { NonContigPagesTeardown(m); return false; }
    memset(m->cabin, 0, sizeof(cabin_t));
    memset(m->proc,  0, sizeof(process_t));
    m->cabin->vmm  = m->vmm;
    m->proc->cabin = m->cabin;

    /* One contiguous 3-frame block: frame A | spacer | frame B. */
    m->block       = pmm_alloc(3);
    m->guard_frame = pmm_alloc(1);
    if (!m->block || !m->guard_frame) { NonContigPagesTeardown(m); return false; }

    /* pmm_alloc returns PHYSICAL addresses; reach the frames through the Pull
     * Map (the boot identity map is gone after vmm_init, so a low physical
     * value is not a usable pointer). The physical addresses come straight
     * from the allocated block bases. */
    m->page_a = (uint8_t *)vmm_phys_to_virt((uintptr_t)m->block);
    m->spacer = (uint8_t *)vmm_phys_to_virt((uintptr_t)m->block + VMM_PAGE_SIZE);
    m->page_b = (uint8_t *)vmm_phys_to_virt((uintptr_t)m->block + 2u * VMM_PAGE_SIZE);
    m->guard  = (uint8_t *)vmm_phys_to_virt((uintptr_t)m->guard_frame);

    m->phys_a = (uintptr_t)m->block;
    m->phys_b = (uintptr_t)m->block + 2u * VMM_PAGE_SIZE;
    uintptr_t phys_spacer = (uintptr_t)m->block + VMM_PAGE_SIZE;

    /* Layout sanity (catches an arithmetic slip): the frame after A is the
     * spacer, B is two frames above A. The contiguous 3-frame block is
     * guaranteed by the buddy allocator; the real non-adjacency PROOF is
     * runtime — the WRITE CONTROL below corrupts the spacer and leaves B
     * intact, which can only happen if the frame after A is not B. */
    if (phys_spacer != m->phys_a + VMM_PAGE_SIZE ||
        m->phys_b   != m->phys_a + 2u * VMM_PAGE_SIZE) {
        NonContigPagesTeardown(m);
        return false;
    }

    /* Map A and B at adjacent VAs (spacer deliberately unmapped) and the guard
     * one page above B. USER-RW so the page-walk and vmm_translate_user_addr
     * accept them. */
    vmm_map_result_t ra = vmm_map_page(m->vmm, base_va,
                                       m->phys_a, VMM_FLAGS_USER_RW);
    vmm_map_result_t rb = vmm_map_page(m->vmm, base_va + VMM_PAGE_SIZE,
                                       m->phys_b, VMM_FLAGS_USER_RW);
    vmm_map_result_t rg = vmm_map_page(m->vmm, base_va + 2u * VMM_PAGE_SIZE,
                                       (uintptr_t)m->guard_frame,
                                       VMM_FLAGS_USER_RW);
    if (!ra.success || !rb.success || !rg.success) {
        NonContigPagesTeardown(m);
        return false;
    }

    /* Re-resolve through the page tables: prove the mapping points where we
     * think — the exact translation the crate path will follow. */
    if (vmm_virt_to_phys(m->vmm, base_va) != m->phys_a ||
        vmm_virt_to_phys(m->vmm, base_va + VMM_PAGE_SIZE) != m->phys_b) {
        NonContigPagesTeardown(m);
        return false;
    }
    return true;
}

void NonContigPagesTeardown(NonContigPages *m)
{
    if (!m) return;

    if (m->vmm) {
        vmm_unmap_page(m->vmm, m->base_va);
        vmm_unmap_page(m->vmm, m->base_va + VMM_PAGE_SIZE);
        vmm_unmap_page(m->vmm, m->base_va + 2u * VMM_PAGE_SIZE);
    }
    /* Frames are owned here, not by the context — unmap cleared the PTEs, so
     * vmm_destroy_context frees only the page-table structures, never these. */
    if (m->block)       pmm_free(m->block, 3);
    if (m->guard_frame) pmm_free(m->guard_frame, 1);
    if (m->vmm)         vmm_destroy_context(m->vmm);
    if (m->proc)        kfree(m->proc);
    if (m->cabin)       kfree(m->cabin);

    memset(m, 0, sizeof(*m));
}

#define FAILK(reason, k) do {                                                  \
        kprintf("[CRATE-IO] SELFTEST FAIL: " reason " (k=%u)\n", (unsigned)(k)); \
        return false;                                                          \
    } while (0)

static bool run_k(NonContigPages *m, unsigned k)
{
    const unsigned  over = CRATE_IO_OVERFLOW;
    const unsigned  n    = k + over;                       /* straddling length  */
    const uintptr_t va   = m->base_va + VMM_PAGE_SIZE - k; /* k in A, over in B  */

    OpContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.proc = m->proc;

    Crate crate;
    memset(&crate, 0, sizeof(crate));
    crate.magic    = CRATE_MAGIC;
    crate.kind     = CRATE_KIND_INOUT;
    crate.addr     = (uint64_t)va;
    crate.capacity = n;

    /* ---- WRITE BACKSTOP — fail-closed translate refuses the straddle. -----
     * vmm_translate_user_addr now returns NULL the instant a range crosses a
     * page boundary, so the legacy "translate + memcpy" can no longer reach the
     * physically-next spacer frame: the straddle is refused and both the spacer
     * and frame B stay POISON. */
    memset(m->page_a, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(m->spacer, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(m->page_b, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(m->guard,  CRATE_IO_POISON, VMM_PAGE_SIZE);

    if (vmm_translate_user_addr(m->vmm, va, n) != NULL)
        FAILK("write backstop: straddle translate not fail-closed", k);
    if (!all_bytes(m->spacer, VMM_PAGE_SIZE, CRATE_IO_POISON))
        FAILK("write backstop: spacer disturbed by refused straddle", k);
    if (!all_bytes(m->page_b, VMM_PAGE_SIZE, CRATE_IO_POISON))
        FAILK("write backstop: frame B disturbed by refused straddle", k);

    /* ---- WRITE FIX — crate_write page-walks to BOTH real frames. --------- */
    memset(m->page_a, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(m->spacer, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(m->page_b, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(m->guard,  CRATE_IO_POISON, VMM_PAGE_SIZE);

    crate.size = 0;
    if (crate_write(&crate, &ctx, g_src, n) != OK)
        FAILK("write fix: crate_write returned error", k);
    if (crate.size != n)
        FAILK("write fix: crate.size != n", k);
    if (memcmp(m->page_a + (VMM_PAGE_SIZE - k), g_src, k) != 0)
        FAILK("write fix: frame A tail wrong", k);
    if (memcmp(m->page_b, g_src + k, over) != 0)
        FAILK("write fix: frame B head not delivered", k);
    if (!all_bytes(m->spacer, VMM_PAGE_SIZE, CRATE_IO_POISON))
        FAILK("write fix: spacer was corrupted", k);
    if (!all_bytes(m->guard, VMM_PAGE_SIZE, CRATE_IO_POISON))
        FAILK("write fix: guard page overrun", k);

    /* ---- READ BACKSTOP — fail-closed translate refuses the straddle. ------
     * Plant the user's intended source across the seam (k bytes in A's tail,
     * over bytes in B's head); the spacer keeps POISON. A straddling
     * vmm_translate_user_addr now returns NULL, so the legacy read can no longer
     * snapshot the foreign spacer. The planted bytes feed the READ FIX leg. */
    memset(m->page_a, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(m->spacer, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(m->page_b, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memcpy(m->page_a + (VMM_PAGE_SIZE - k), g_src, k);
    memcpy(m->page_b, g_src + k, over);

    if (vmm_translate_user_addr(m->vmm, va, n) != NULL)
        FAILK("read backstop: straddle translate not fail-closed", k);

    /* ---- READ FIX — crate_read page-walks to BOTH real frames. ----------- */
    crate.size = n;
    memset(g_dst, 0, n);
    if (crate_read(&crate, &ctx, g_dst, n) != OK)
        FAILK("read fix: crate_read returned error", k);
    if (memcmp(g_dst, g_src, n) != 0)
        FAILK("read fix: snapshot mismatch across seam", k);

    return true;
}

#undef FAILK

#define FAILK(reason, k) do {                                                  \
        kprintf("[CRATE-IO] SELFTEST FAIL: " reason " (k=%u)\n", (unsigned)(k)); \
        return false;                                                          \
    } while (0)

/* IPC straddle proof across two non-contiguous cabins. The SENDER carries the
 * planted payload across its A|B seam; the TARGET is where the delivered bytes
 * land. Mirrors how the converted ipc_copy_to_heap reads the sender and writes
 * the target — plus the function itself, end-to-end. */
static bool run_ipc_k(NonContigPages *sender, NonContigPages *target, unsigned k)
{
    const unsigned  over   = CRATE_IO_OVERFLOW;
    const unsigned  n      = k + over;                          /* straddling length */
    const uintptr_t src_va = sender->base_va + VMM_PAGE_SIZE - k;
    const uintptr_t dst_va = target->base_va + VMM_PAGE_SIZE - k;

    /* Plant the IPC payload across the SENDER seam: k bytes in A's tail, over
     * bytes in B's head; the sender spacer keeps POISON (distinct from the odd
     * source pattern). */
    memset(sender->page_a, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(sender->spacer, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(sender->page_b, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(sender->guard,  CRATE_IO_POISON, VMM_PAGE_SIZE);
    memcpy(sender->page_a + (VMM_PAGE_SIZE - k), g_src, k);
    memcpy(sender->page_b, g_src + k, over);

    /* ---- SOURCE BACKSTOP — fail-closed translate refuses the sender straddle.
     * The legacy single-frame read (old ipc_copy_to_heap source leg) can no
     * longer pull the sender's foreign spacer in place of frame B:
     * vmm_translate_user_addr returns NULL on the page-crossing range. The
     * payload planted above feeds the SOURCE FIX leg below. ---------------- */
    if (vmm_translate_user_addr(sender->vmm, src_va, n) != NULL)
        FAILK("ipc src backstop: straddle translate not fail-closed", k);

    /* ---- SOURCE FIX — vmm_user_buf_in (converted source leg) page-walks both
     * sender frames into the bounce buffer. ------------------------------- */
    uint8_t *kbuf = (uint8_t *)vmm_user_buf_in(sender->vmm, src_va, n);
    if (!kbuf) FAILK("ipc src fix: vmm_user_buf_in returned NULL", k);
    if (memcmp(kbuf, g_src, n) != 0) {
        vmm_user_buf_free(kbuf);
        FAILK("ipc src fix: bounce mismatch across sender seam", k);
    }

    /* ---- DEST BACKSTOP — fail-closed translate refuses the target straddle.
     * The legacy single-frame write (old ipc_copy_to_heap dest leg) can no
     * longer spill the bounce into the target's foreign spacer:
     * vmm_translate_user_addr returns NULL, so every target frame stays
     * POISON. ------------------------------------------------------------ */
    memset(target->page_a, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(target->spacer, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(target->page_b, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(target->guard,  CRATE_IO_POISON, VMM_PAGE_SIZE);
    if (vmm_translate_user_addr(target->vmm, dst_va, n) != NULL)
        { vmm_user_buf_free(kbuf); FAILK("ipc dst backstop: straddle translate not fail-closed", k); }
    if (!all_bytes(target->spacer, VMM_PAGE_SIZE, CRATE_IO_POISON))
        { vmm_user_buf_free(kbuf); FAILK("ipc dst backstop: spacer disturbed by refused straddle", k); }
    if (!all_bytes(target->page_b, VMM_PAGE_SIZE, CRATE_IO_POISON))
        { vmm_user_buf_free(kbuf); FAILK("ipc dst backstop: frame B disturbed by refused straddle", k); }

    /* ---- DEST FIX — vmm_user_buf_commit_out (converted dest leg) delivers to
     * BOTH target frames; spacer + guard untouched. --------------------- */
    memset(target->page_a, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(target->spacer, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(target->page_b, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(target->guard,  CRATE_IO_POISON, VMM_PAGE_SIZE);
    error_t wrc = vmm_user_buf_commit_out(target->vmm, dst_va, kbuf, n);
    vmm_user_buf_free(kbuf);
    if (wrc != OK) FAILK("ipc dst fix: commit_out returned error", k);
    if (memcmp(target->page_a + (VMM_PAGE_SIZE - k), g_src, k) != 0)
        FAILK("ipc dst fix: frame A tail wrong", k);
    if (memcmp(target->page_b, g_src + k, over) != 0)
        FAILK("ipc dst fix: frame B head not delivered", k);
    if (!all_bytes(target->spacer, VMM_PAGE_SIZE, CRATE_IO_POISON))
        FAILK("ipc dst fix: spacer was corrupted", k);
    if (!all_bytes(target->guard, VMM_PAGE_SIZE, CRATE_IO_POISON))
        FAILK("ipc dst fix: guard page overrun", k);

    /* ---- END-TO-END — drive the converted ipc_copy_to_heap and read the
     * delivered payload back through the target page tables. The function
     * allocates + maps its own target frames at buf_heap_next; a correct
     * page-walk delivers the full A+B payload. -------------------------- */
    uint64_t tvaddr = ipc_copy_to_heap(sender->proc, target->proc, src_va, n);
    if (tvaddr == 0) FAILK("ipc e2e: ipc_copy_to_heap returned 0", k);

    memset(g_dst, 0, n);
    if (vmm_user_buf_in_into(target->vmm, tvaddr, n, g_dst) != OK)
        FAILK("ipc e2e: read-back translate failed", k);
    if (memcmp(g_dst, g_src, n) != 0)
        FAILK("ipc e2e: delivered payload mismatch across seam", k);
    if (!all_bytes(sender->spacer, VMM_PAGE_SIZE, CRATE_IO_POISON))
        FAILK("ipc e2e: sender spacer disturbed", k);
    if (!all_bytes(sender->guard, VMM_PAGE_SIZE, CRATE_IO_POISON))
        FAILK("ipc e2e: sender guard disturbed", k);

    /* Free the frames ipc_copy_to_heap mapped into the target so each k
     * iteration leaves no live mapping or leaked frame behind. */
    uint32_t pages = (n + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    for (uint32_t i = 0; i < pages; i++) {
        uintptr_t va   = tvaddr + (uintptr_t)i * PMM_PAGE_SIZE;
        uintptr_t phys = vmm_virt_to_phys(target->vmm, va);
        vmm_unmap_page(target->vmm, va);
        if (phys) pmm_free((void *)phys, 1);
    }

    return true;
}

#undef FAILK

error_t CrateIoSelfTest(void)
{
    debug_printf("[CRATE-IO][selftest] starting\n");
    fill_src();

    /* --- Storage: one non-contiguous mapping, fixed crate_read / crate_write. */
    NonContigPages map;
    if (!NonContigPagesSetup(&map, CRATE_IO_SELFTEST_VA)) {
        kprintf("[CRATE-IO] SELFTEST FAIL: non-contiguous mapping setup\n");
        return ERR_INTERNAL;
    }

    bool ok = true;
    for (size_t i = 0; i < sizeof(CRATE_IO_K) / sizeof(CRATE_IO_K[0]); i++) {
        if (!run_k(&map, CRATE_IO_K[i])) { ok = false; break; }
    }

    NonContigPagesTeardown(&map);

    if (!ok) return ERR_INTERNAL;   /* run_k already printed the FAIL line */

    /* --- IPC: two non-contiguous cabins, ipc_copy_to_heap sender -> target. */
    NonContigPages sender, target;
    if (!NonContigPagesSetup(&sender, CRATE_IO_SELFTEST_VA)) {
        kprintf("[CRATE-IO] SELFTEST FAIL: IPC sender mapping setup\n");
        return ERR_INTERNAL;
    }
    if (!NonContigPagesSetup(&target, CRATE_IO_SELFTEST_VA)) {
        NonContigPagesTeardown(&sender);
        kprintf("[CRATE-IO] SELFTEST FAIL: IPC target mapping setup\n");
        return ERR_INTERNAL;
    }
    /* ipc_copy_to_heap maps fresh payload pages at the target's buf_heap
     * cursor; seat it clear of the A|B|guard window. */
    target.cabin->buf_heap_next = (uint64_t)target.base_va + CRATE_IO_IPC_HEAP_OFF;

    bool ipc_ok = true;
    for (size_t i = 0; i < sizeof(CRATE_IO_K) / sizeof(CRATE_IO_K[0]); i++) {
        if (!run_ipc_k(&sender, &target, CRATE_IO_K[i])) { ipc_ok = false; break; }
    }

    NonContigPagesTeardown(&target);
    NonContigPagesTeardown(&sender);

    if (!ipc_ok) return ERR_INTERNAL;   /* run_ipc_k already printed the FAIL */

    kprintf("[CRATE-IO] SELFTEST PASS\n");
    return OK;
}
