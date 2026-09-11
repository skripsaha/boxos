
#include "crate_io_selftest.h"
#include "crate_io.h"
#include "pmm.h"
#include "boxos_crate.h"
#include "op_registry.h"
#include "system_deck.h"
#include "klib.h"

#define CRATE_IO_SELFTEST_VA   0x0000000040000000ULL
#define CRATE_IO_OVERFLOW      64u
#define CRATE_IO_POISON        0x5Au
#define CRATE_IO_IPC_HEAP_OFF  0x0000000000100000ULL

static const unsigned CRATE_IO_K[] = {
    1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u, 256u, 512u, 1024u, 2048u
};

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

    m->block       = pmm_alloc(3);
    m->guard_frame = pmm_alloc(1);
    if (!m->block || !m->guard_frame) { NonContigPagesTeardown(m); return false; }

    m->page_a = (uint8_t *)vmm_phys_to_virt((uintptr_t)m->block);
    m->spacer = (uint8_t *)vmm_phys_to_virt((uintptr_t)m->block + VMM_PAGE_SIZE);
    m->page_b = (uint8_t *)vmm_phys_to_virt((uintptr_t)m->block + 2u * VMM_PAGE_SIZE);
    m->guard  = (uint8_t *)vmm_phys_to_virt((uintptr_t)m->guard_frame);

    m->phys_a = (uintptr_t)m->block;
    m->phys_b = (uintptr_t)m->block + 2u * VMM_PAGE_SIZE;
    uintptr_t phys_spacer = (uintptr_t)m->block + VMM_PAGE_SIZE;

    if (phys_spacer != m->phys_a + VMM_PAGE_SIZE ||
        m->phys_b   != m->phys_a + 2u * VMM_PAGE_SIZE) {
        NonContigPagesTeardown(m);
        return false;
    }

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
    const unsigned  n    = k + over;
    const uintptr_t va   = m->base_va + VMM_PAGE_SIZE - k;

    OpContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.proc = m->proc;

    Crate crate;
    memset(&crate, 0, sizeof(crate));
    crate.magic    = CRATE_MAGIC;
    crate.kind     = CRATE_KIND_INOUT;
    crate.addr     = (uint64_t)va;
    crate.capacity = n;

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

    memset(m->page_a, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(m->spacer, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(m->page_b, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memcpy(m->page_a + (VMM_PAGE_SIZE - k), g_src, k);
    memcpy(m->page_b, g_src + k, over);

    if (vmm_translate_user_addr(m->vmm, va, n) != NULL)
        FAILK("read backstop: straddle translate not fail-closed", k);

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

static bool run_ipc_k(NonContigPages *sender, NonContigPages *target, unsigned k)
{
    const unsigned  over   = CRATE_IO_OVERFLOW;
    const unsigned  n      = k + over;
    const uintptr_t src_va = sender->base_va + VMM_PAGE_SIZE - k;
    const uintptr_t dst_va = target->base_va + VMM_PAGE_SIZE - k;

    memset(sender->page_a, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(sender->spacer, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(sender->page_b, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(sender->guard,  CRATE_IO_POISON, VMM_PAGE_SIZE);
    memcpy(sender->page_a + (VMM_PAGE_SIZE - k), g_src, k);
    memcpy(sender->page_b, g_src + k, over);

    if (vmm_translate_user_addr(sender->vmm, src_va, n) != NULL)
        FAILK("ipc src backstop: straddle translate not fail-closed", k);

    uint8_t *kbuf = (uint8_t *)vmm_user_buf_in(sender->vmm, src_va, n);
    if (!kbuf) FAILK("ipc src fix: vmm_user_buf_in returned NULL", k);
    if (memcmp(kbuf, g_src, n) != 0) {
        vmm_user_buf_free(kbuf);
        FAILK("ipc src fix: bounce mismatch across sender seam", k);
    }

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

    if (!ok) return ERR_INTERNAL;

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
    target.cabin->buf_heap_next = (uint64_t)target.base_va + CRATE_IO_IPC_HEAP_OFF;

    bool ipc_ok = true;
    for (size_t i = 0; i < sizeof(CRATE_IO_K) / sizeof(CRATE_IO_K[0]); i++) {
        if (!run_ipc_k(&sender, &target, CRATE_IO_K[i])) { ipc_ok = false; break; }
    }

    NonContigPagesTeardown(&target);
    NonContigPagesTeardown(&sender);

    if (!ipc_ok) return ERR_INTERNAL;

    kprintf("[CRATE-IO] SELFTEST PASS\n");
    return OK;
}