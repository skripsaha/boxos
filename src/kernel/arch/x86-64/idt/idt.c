#include "idt.h"
#include "gdt.h"
#include "tss.h"
#include "klib.h"
#include "io.h"
#include "pic.h"
#include "irqchip.h"
#include "lapic.h"
#include "process.h"
#include "guide.h"
#include "ready_queue.h" // for g_ready_queue
#include "pocket_ring.h"
#include "kring.h"
#include "vmm.h"
#include "uaccess.h"          /* UACCESS_USER_VA_MAX for SMAP-fault diagnostic */
#include "nameplate.h"        /* nameplate_name_at — name the frames of a user fault */
#include "nameplate_format.h" /* NameplateHeader — the kernel table's own minimum size */
#include "atomics.h"
#include "touch.h" /* TouchTag types — Phase 2K #CP publish */
#include "scheduler.h"
#include "context_switch.h"
#include "keyboard.h"
#include "ahci.h"
#include "idle.h"
#include "amp.h"
#include "kcore.h"
#include "xhci_interrupt.h"
#include "linker_symbols.h"
#include "cpu_calibrate.h" // cpu_tsc_recal_tick (periodic recalibration)
#include "touch_queue.h"
#include "pit.h"
#include "irq_defer.h"

static idt_entry_t idt[IDT_ENTRIES];
static idt_descriptor_t idt_desc;

static irq_callback_t irq_callbacks[IRQ_MAX_COUNT] = {NULL};

static volatile uint64_t exception_count = 0;
static volatile uint64_t irq_count[IRQ_MAX_COUNT] = {0};

static void idt_load_asm(uint64_t idt_desc_addr)
{
    asm volatile("lidt (%0)" : : "r"(idt_desc_addr) : "memory");
}

void idt_load(void)
{
    idt_load_asm((uint64_t)&idt_desc);
}

void idt_set_entry(int index, uint64_t handler, uint16_t selector, uint8_t type_attr, uint8_t ist)
{
    idt[index].offset_low = handler & 0xFFFF;
    idt[index].selector = selector;
    idt[index].ist = ist & 0x07;
    idt[index].type_attr = type_attr;
    idt[index].offset_middle = (handler >> 16) & 0xFFFF;
    idt[index].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[index].reserved = 0;
}

void idt_init(void)
{
    debug_printf("[IDT] Initializing Interrupt Descriptor Table (minimal)...\n");

    memset(idt, 0, sizeof(idt));

    idt_desc.limit = sizeof(idt) - 1;
    idt_desc.base = (uint64_t)idt;

    debug_printf("[IDT] Setting up exception handlers (0-31)...\n");

    for (int i = 0; i < 32; i++)
    {
        uint8_t ist = 0; // No IST by default

        // Critical exceptions use IST
        switch (i)
        {
        case 1: // Debug
            ist = IST_DEBUG;
            break;
        case 2: // NMI
            ist = IST_NMI;
            break;
        case 8: // Double Fault
            ist = IST_DOUBLE_FAULT;
            break;
        case 12: // Stack Fault
            ist = IST_STACK_FAULT;
            break;
        case 18: // Machine Check
            ist = IST_MACHINE_CHECK;
            break;
        }

        idt_set_entry(i, (uint64_t)isr_table[i], GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, ist);
    }

    debug_printf("[IDT] Setting up IRQ handlers (32-55)...\n");

    // Vectors 32-55: Hardware IRQs (PIC IRQ 0-15 + IO-APIC GSI 16-23)
    for (int i = 32; i < 56; i++)
    {
        idt_set_entry(i, (uint64_t)isr_table[i], GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);
    }

    debug_printf("[IDT] Setting up syscall handler (INT 0x80)...\n");

    // Syscall handler at 0x80 (Ring 3 accessible)
    idt_set_entry(SYSCALL_VECTOR, (uint64_t)isr_table[SYSCALL_VECTOR], GDT_KERNEL_CODE, IDT_TYPE_USER_INTERRUPT, 0);

    // LAPIC timer vector (254) and spurious vector (255)
    idt_set_entry(LAPIC_TIMER_VECTOR, (uint64_t)isr_table[LAPIC_TIMER_VECTOR],
                  GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);
    idt_set_entry(LAPIC_SPURIOUS_VECTOR, (uint64_t)isr_table[LAPIC_SPURIOUS_VECTOR],
                  GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);

    // AHCI MSI vector (0x70) — message-signalled interrupt from the HBA
    idt_set_entry(AHCI_MSI_VECTOR, (uint64_t)isr_table[AHCI_MSI_VECTOR],
                  GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);

    // xHCI MSI vectors — one per USB host controller, so an interrupt names
    // which of them raised it instead of being offered to all in turn.
    for (uint32_t v = XHCI_MSI_VECTOR; v <= XHCI_MSI_VECTOR_LAST; v++)
    {
        idt_set_entry((uint8_t)v, (uint64_t)isr_table[v],
                      GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);
    }

    // AMP IPI vectors (0xF0-0xF2)
    idt_set_entry(IPI_WAKE_VECTOR, (uint64_t)isr_table[IPI_WAKE_VECTOR],
                  GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);
    idt_set_entry(IPI_SHOOTDOWN_VECTOR, (uint64_t)isr_table[IPI_SHOOTDOWN_VECTOR],
                  GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);
    idt_set_entry(IPI_PANIC_VECTOR, (uint64_t)isr_table[IPI_PANIC_VECTOR],
                  GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);

    idt_load_asm((uint64_t)&idt_desc);

    debug_printf("[IDT] IDT loaded successfully at 0x%lx (limit=%d)\n", idt_desc.base, idt_desc.limit);
}

void irq_register_handler(uint8_t irq, irq_callback_t callback)
{
    if (irq >= IRQ_MAX_COUNT)
    {
        debug_printf("[IDT] ERROR: Invalid IRQ/GSI %u (max %u)\n", irq, IRQ_MAX_COUNT - 1);
        return;
    }

    if (irq_callbacks[irq] != NULL)
    {
        debug_printf("[IDT] WARNING: Overwriting existing handler for IRQ %u\n", irq);
    }

    irq_callbacks[irq] = callback;
    debug_printf("[IDT] Registered callback for IRQ %u\n", irq);
}

void irq_unregister_handler(uint8_t irq)
{
    if (irq >= IRQ_MAX_COUNT)
    {
        debug_printf("[IDT] ERROR: Invalid IRQ/GSI %u (max %u)\n", irq, IRQ_MAX_COUNT - 1);
        return;
    }

    irq_callbacks[irq] = NULL;
    debug_printf("[IDT] Unregistered callback for IRQ %u\n", irq);
}

static process_t *find_process_by_kernel_stack_overflow(uint64_t rsp)
{
    // Called from IST exception handlers (double fault / stack fault).
    // process_lock may be held by the interrupted code, so use trylock
    // to avoid deadlock. If we can't get the lock, fall back to unlocked
    // iteration — best-effort crash recovery.
    bool locked = spin_trylock_process_list();

    process_t *proc = process_get_first();
    while (proc)
    {
        if (proc->magic == PROCESS_MAGIC && proc->kernel_stack_guard_base)
        {
            uintptr_t guard_start = (uintptr_t)proc->kernel_stack_guard_base;
            uintptr_t guard_end = guard_start + (CONFIG_KERNEL_STACK_GUARD_PAGES * CONFIG_PAGE_SIZE);
            if (rsp >= guard_start && rsp < guard_end)
            {
                if (locked)
                    process_list_unlock();
                return proc;
            }
        }
        proc = proc->next;
    }

    if (locked)
        process_list_unlock();
    return NULL;
}

/* Friendly mnemonic for the first 32 Intel exception vectors (SDM Vol 3
 * §6.15 Table 6-1). Returned strings are static literals; safe to embed
 * in kprintf without lifetime concerns. Vector >= 32 (= IRQ) returns
 * "INT" — exception_handler is only called for the 0-31 range, but the
 * defensive branch keeps the helper total. */
static const char *exception_mnemonic(uint8_t vector)
{
    switch (vector)
    {
    case 0:
        return "#DE";
    case 1:
        return "#DB";
    case 2:
        return "NMI";
    case 3:
        return "#BP";
    case 4:
        return "#OF";
    case 5:
        return "#BR";
    case 6:
        return "#UD";
    case 7:
        return "#NM";
    case 8:
        return "#DF";
    case 10:
        return "#TS";
    case 11:
        return "#NP";
    case 12:
        return "#SS";
    case 13:
        return "#GP";
    case 14:
        return "#PF";
    case 16:
        return "#MF";
    /* #AC (vector 17) — Intel SDM Vol 3 §6.15 Table 6-1. Two distinct
     * triggers reach here: (1) classical alignment-check from a
     * misaligned user-mode memory access while CR0.AM=1 and EFLAGS.AC=1,
     * and (2) split-lock detection when IA32_CORE_CAPABILITIES.bit5 +
     * TEST_CTL.bit29 are both set and a LOCK-prefixed instruction spans
     * a cache line. BoxOS clears TEST_CTL.bit29 at every BSP/AP bringup
     * (see cpu_test_ctl_init in cpuid.c), so #AC reaching this handler
     * indicates either a misconfigured boot path, a hostile guest VM
     * lying about CPUID, or a real classical AC — all worth a clear
     * diagnostic instead of an anonymous "Exception #17". */
    case 17:
        return "#AC";
    case 18:
        return "#MC";
    case 19:
        return "#XF";
    case 20:
        return "#VE";
    case 21:
        return "#CP";
    default:
        return "INT";
    }
}

#ifdef CONFIG_BRINGUP_HOLD_ON_FIRST_FAULT
/*
 * Bring-up hold. Off in every normal build; `make BRINGUP=on` turns it on.
 *
 * The user-mode path below is right for a running system: a process faults,
 * the kernel kills it and schedules the next one, and the machine survives.
 * On the first boot of a new machine it is exactly wrong. The next process
 * faults too, the screen scrolls, and the one dump that mattered — the FIRST
 * fault, the one with the cause still in CR2 and the stack not yet unwound —
 * is gone before anyone can read it. There is no scrollback on a bare screen
 * and, until the serial line is wired, nowhere for it to have gone.
 *
 * So: print the first fault in full, then stop. Every core that faults after
 * the hold is set stops without printing a word, because a second core's
 * dump would scroll the first one away just as effectively as a loop.
 */
static volatile uint32_t g_bringup_held = 0;

static void bringup_hold_forever(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}
#endif

/*
 * panic_probe_present — is this virtual address backed by a present page?
 *
 * Walked straight off CR3, level by level, exactly as the hardware would. It
 * deliberately consults nothing the kernel maintains — not the VMM's context
 * structures, not a cached translation, not a bitmap — because everything the
 * kernel maintains is on the list of things that might have caused the panic
 * this is being asked during. CR3 and the page tables it points at are the one
 * description of memory the CPU itself is already obeying.
 *
 * The only assumption left is that the direct map is intact, which is what
 * makes reading the tables possible at all. That is a far smaller surface than
 * dereferencing an arbitrary saved frame pointer and hoping.
 */
static bool panic_probe_present(uint64_t va)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    const uint64_t ADDR_MASK = 0x000FFFFFFFFFF000ULL;
    const uint64_t P = 1ULL << 0;
    const uint64_t PS = 1ULL << 7; /* 1 GiB / 2 MiB leaf */

    uint64_t *table = (uint64_t *)vmm_phys_to_virt(cr3 & ADDR_MASK);
    if (!table)
        return false;

    for (int level = 3; level >= 0; level--)
    {
        uint64_t entry = table[(va >> (12 + level * 9)) & 0x1FF];
        if (!(entry & P))
            return false;
        if (level > 0 && (entry & PS))
            return true; /* large page, and it is present */
        table = (uint64_t *)vmm_phys_to_virt(entry & ADDR_MASK);
        if (!table)
            return false;
    }
    return true;
}

/*
 * One panic, one dump.
 *
 * Everything the dump prints touches something: the VGA buffer, the serial
 * UART, and — for the stack trace — memory the fault may already have proved
 * untrustworthy. A fault raised in the middle of that re-enters the handler,
 * and so does every other core that hit the same bug at the same instant.
 * Without this claim the screen fills and the first dump, the only one whose
 * CR2 still names the original cause, scrolls away. That is what a six-core
 * machine did on its first real boot: the path already ended in `cli; hlt`,
 * and it still never got there.
 *
 * Claimed BEFORE the first line is printed, not after: the loser must leave no
 * trace at all, because half a line of someone else's dump on the end of the
 * one that matters is its own kind of confusion.
 */
static void panic_claim_or_halt(bool *already_claimed_here)
{
    if (*already_claimed_here)
        return; /* same dump, further down the same page */

    static volatile uint32_t panic_claimed = 0;
    if (__atomic_exchange_n(&panic_claimed, 1u, __ATOMIC_ACQ_REL) != 0)
    {
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    *already_claimed_here = true;
}

/* The kernel's own Nameplate, linked in by the second pass — see the linker
 * script and the Makefile. Riveted to the image, so naming a frame costs no
 * disk, no lock and no matching kernel.elf at the other end of the world;
 * which is the situation a panic on a machine you are not sitting at leaves
 * you in, and the situation this table exists for. */
extern const char __nameplate_start[];
extern const char __nameplate_end[];

/* Print an address, and its name when the image can supply one.
 *
 * `for_return` turns a return address into the byte that belongs to the call
 * before naming it: the saved address points PAST the call, so naming it
 * directly names the NEXT function whenever a call is the last instruction of
 * its own. The address printed is still the one that was saved, because that
 * is what a reader compares against a disassembly. */
static void panic_name_addr(const char *prefix, uint64_t addr, bool for_return)
{
    NameplateName site;
    uint64_t bytes = (uint64_t)(__nameplate_end - __nameplate_start);
    uintptr_t look = (uintptr_t)(for_return && addr ? addr - 1 : addr);

    if (bytes >= sizeof(NameplateHeader) &&
        nameplate_name_at_kernel((uintptr_t)__nameplate_start, bytes, look, &site))
        kprintf("%s%016lx  %s+0x%lx\n", prefix, addr, site.Text,
                site.Offset + (for_return ? 1u : 0u));
    else
        kprintf("%s%016lx\n", prefix, addr);
}

/* Readable across `len` bytes, page boundary included. Two probes cover every
 * length this file asks about; nothing here reads more than sixteen bytes. */
static bool panic_probe_range(uint64_t va, uint64_t len)
{
    return panic_probe_present(va) &&
           panic_probe_present(va + len - 1);
}

void exception_handler(interrupt_frame_t *frame)
{
    /* Scoped to THIS invocation. A kernel #PF claims the dump at the [VMM]
     * line and then walks on into the panic block below, which claims again;
     * without a per-invocation record the second claim sees the flag its own
     * first claim set and halts the dump it was about to print. A re-entrant
     * fault gets a fresh handler frame, a fresh false, and is stopped — which
     * is the whole point. */
    bool panic_claimed_here = false;

    /*
     * ‼ CR2 IS READ HERE, FIRST, AND NOWHERE ELSE.
     *
     * CR2 holds the address that caused the LAST page fault on this core, and
     * nothing preserves it: the next #PF overwrites it. That makes "read it
     * when you happen to need it" wrong, and it was wrong — the user-mode dump
     * below read it AFTER nameplate_name_at, which walks the faulting
     * process's own symbol table through get_user_u32. That helper is built on
     * a .uaccess_fixup entry: the load really does fault and the handler
     * really does recover it, so a process whose nameplate is not fully mapped
     * — a half-loaded image, say, which is the very case worth dumping —
     * rewrote CR2 with the nameplate's address before the dump printed it. The
     * line then names an address nobody faulted on, which on a board is a
     * morning spent hunting a pointer that does not exist.
     *
     * Nothing can spoil it before this point: the gates are interrupt gates
     * (IDT_TYPE_INTERRUPT_GATE, IF clear), so no interrupt nests here, and the
     * only thing that writes CR2 is a page fault — which, between the CPU
     * delivering this one and this line, could only come from the stub's own
     * pushes onto the kernel stack, and that is a #DF, not a recoverable #PF.
     *
     * Meaningful only for vector 14; for every other vector it is whatever the
     * last fault on this core left, which is exactly what the panic block used
     * to print anyway.
     */
    uint64_t fault_addr;
    __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));

    atomic_fetch_add_u64(&exception_count, 1);

#ifdef CONFIG_BRINGUP_HOLD_ON_FIRST_FAULT
    if (g_bringup_held)
        bringup_hold_forever(); /* the screen already holds the fault that matters */
#endif

    /* NMI (vector 2) — server-class firmware can deliver APEI/GHES
     * notifications via NMI when a HEST source's notify type == 4. The
     * GHES runtime walks its NMI-notify sources, defers GESB processing
     * to K-Core via irq_defer, and returns true if any source had a
     * pending block_status. We don't `return` after a consumed NMI —
     * the kernel still needs to clear the NMI source on the LAPIC/PIC
     * path (if any) and fall through to the standard NMI logging
     * below for non-APEI NMI causes (watchdog, performance counters,
     * etc.). */
    if (frame->vector == 2)
    {
        extern bool apei_ghes_nmi_check(void);
        (void)apei_ghes_nmi_check();
        /* fall through to generic NMI logging */
    }

    /* Phase 2F — #MC (vector 18) routes to MCE subsystem. The handler
     * runs on the IST_MACHINE_CHECK stack (set by idt_init), walks every
     * IA32_MC<i>_STATUS bank, poisons phys pages, publishes Touch
     * events, and returns true if the error was recoverable (UC=0 or
     * UCR with RIPV=1). On unrecoverable error: fall through to the
     * standard kill-process / system_halt path below. */
    if (frame->vector == 18)
    {
        extern bool mce_handle(interrupt_frame_t *);
        if (mce_handle(frame))
        {
            return; /* recovered — IRET back to user/kernel */
        }
        /* fatal — drop into the generic exception path. (cs ring tells
         * exception_handler whether to kill the process or halt.) */
    }

    /* Phase 2K — #CP (vector 21) Control-Protection Exception. Fires
     * when a shadow-stack mismatch or IBT violation occurs. Error code
     * bits (Intel SDM Vol 3D §17.7):
     *   bits 14:0  — CP error type:
     *     1 = NEAR_RET     shadow-stack mismatch on RET
     *     2 = FAR_RET_IRET shadow-stack mismatch on FAR RET/IRET
     *     3 = ENDBRANCH    indirect branch missing ENDBR target
     *     4 = RSTORSSP     RSTORSSP token validation failed
     *     5 = SETSSBSY     SETSSBSY token validation failed
     *   bit 15     — ENCL  set when fault is in SGX enclave
     *
     * We log + publish Touch and fall through to the generic kill-process
     * path. Recovery is the future per-process CET lifecycle's job. */
    if (frame->vector == 21)
    {
        uint16_t cp_type = (uint16_t)(frame->error_code & 0x7FFFu);
        bool cp_enclave = (frame->error_code & 0x8000u) != 0;
        kprintf("[CET] #CP fired: type=%u enclave=%d RIP=0x%lx\n",
                (unsigned)cp_type, (int)cp_enclave, frame->rip);
        if ((frame->cs & 3) == 0)
            panic_name_addr("[CET]   in ", frame->rip, false);
        /* Touch publish via pre-resolved tag (resolved in vmm_cet_probe
         * at BSP boot, outside IRQ context). TouchLogbookIntern in this
         * handler would take registry locks → deadlock against any
         * thread holding the Touch lock at the moment #CP fired. */
        TouchTag cp_tag = vmm_get_cet_cp_tag();
        struct
        {
            uint64_t rip;
            uint16_t cp_type;
            uint8_t enclave;
            uint8_t pad;
        } ev = {
            .rip = frame->rip,
            .cp_type = cp_type,
            .enclave = cp_enclave ? 1u : 0u,
            .pad = 0,
        };
        if (cp_tag != TOUCH_TAG_INVALID)
        {
            TouchPublishIrqPair(cp_tag, TOUCH_TAG_INVALID,
                                &ev, (uint16_t)sizeof(ev), 0u, 0u);
        }
        /* Bump the cet_lifecycle stat counter (RELAXED — never load-
         * bearing). Visible via `hw cet status`. */
        {
            extern void cet_lifecycle_record_cp_fault(void);
            cet_lifecycle_record_cp_fault();
        }
        /* Fall through to generic kill — Phase 2K is observe-only. */
    }

    if (frame->vector == 14)
    {
        /* uaccess fixup — kernel-mode #PF whose RIP lies inside a
         * copy_to/from_user / put_user / get_user inline asm region
         * gets handled here, BEFORE the demand-paging path runs. The
         * fixup table maps fault_rip → recovery_rip; we set
         * frame->rip to the recovery label and IRET. The recovery
         * code path executes CLAC + returns an error from the
         * affected helper.
         *
         * Why before vmm_handle_page_fault: that path can mutate
         * page-table state (demand-page-in, COW, etc.) which would
         * be the wrong response for a SMAP-blocked user pointer.
         * The fixup is the correct response: the user pointer is
         * bad (or its page got unmapped under us), bubble the
         * error to the syscall caller. */
        if ((frame->cs & 3) == 0)
        {
            extern uintptr_t uaccess_lookup_fixup(uintptr_t fault_rip);
            uintptr_t fixup_rip = uaccess_lookup_fixup(frame->rip);
            if (fixup_rip != 0)
            {
                frame->rip = fixup_rip;
                return;
            }
        }

        int result = vmm_handle_page_fault(fault_addr, frame->error_code);
        if (result == 0)
        {
            return;
        }

        /* Unhandled ring-0 #PF — fall through to KERNEL PANIC.
         *
         * Previously this branch logged + returned, which meant iretq
         * resumed at the faulting RIP and immediately re-faulted. Under
         * stress (UEFI 16c write_stress) the retry loop was indistinguishable
         * from a silent system hang. Production demands fail-loud over
         * fail-silent: every unhandled kernel #PF is a real bug (missing
         * uaccess fixup, wild pointer, stack overflow past the guard) and
         * must surface with full diagnostic context, not livelock.
         *
         * SMAP-candidate hint: ring-0 #PF on a user-half VA with P=1 is
         * the canonical SMAP signature. The fixup table already missed
         * (else we'd have redirected RIP earlier), so this is a forgotten
         * STAC/CLAC bracket — adjacent kernel code dereferenced a user-
         * mapped page with RFLAGS.AC=0 and CR4.SMAP=1. The panic block
         * below dumps RFLAGS so the operator can read the AC bit. */
        if ((frame->cs & 3) == 0)
        {
            /* This line is the first thing an unhandled ring-0 #PF prints, so
             * the claim belongs here rather than at the panic banner below —
             * otherwise a fault raised while printing the dump announces
             * itself once before being silenced, and the screen ends with a
             * second address that is a consequence, not a cause. */
            panic_claim_or_halt(&panic_claimed_here);

            /* RSVD (bit 3) is asked FIRST, and it silences the SMAP claim.
             *
             * A reserved-bit fault and a SMAP fault look alike to the old
             * test — both have P=1, both can land on a low address — and the
             * old test only asked those two things, so it called every RSVD
             * fault a SMAP candidate. That is not a near miss: it names the
             * wrong subsystem, and it named it on both real machines BoxOS
             * has booted, where the actual cause was bit 63 in a PTE while
             * IA32_EFER.NXE was still 0 (Intel SDM Vol 3A §4.5 — bit 63 is
             * RESERVED until NXE is set; Table 4-15 for the error code).
             * A fault BoxOS cannot have any more, but a diagnostic that
             * points away from the truth is worth removing regardless. */
            bool is_rsvd = (frame->error_code & 0x8ULL) != 0;
            bool is_smap_candidate = !is_rsvd &&
                                     (frame->error_code & 0x1ULL) &&
                                     fault_addr < UACCESS_USER_VA_MAX;
            kprintf("\n[VMM] Unhandled kernel #PF at 0x%lx err=0x%lx%s%s\n",
                    fault_addr, frame->error_code,
                    is_rsvd ? "  [reserved bit set in a paging entry]" : "",
                    is_smap_candidate ? "  [SMAP candidate]" : "");
            if (is_rsvd)
            {
                /* Read EFER here rather than guess: whether NXE is on is the
                 * single question that separates "bit 63 was illegal" from
                 * every other reserved-bit cause, and the operator cannot
                 * read an MSR off a halted screen. */
                uint32_t nxe_lo, nxe_hi;
                __asm__ volatile("rdmsr" : "=a"(nxe_lo), "=d"(nxe_hi)
                                         : "c"(0xC0000080u));
                bool nxe = (nxe_lo & (1u << 11)) != 0;
                kprintf("[VMM]   A paging-structure entry on the way to this "
                        "address has a reserved bit set.\n"
                        "[VMM]   IA32_EFER.NXE=%u — %s\n",
                        (unsigned)nxe,
                        nxe
                          ? "bit 63 is legal here, so look at bits above "
                            "MAXPHYADDR or at PS/PAT in a non-leaf entry"
                          : "bit 63 (NX) is RESERVED with NXE off, and an "
                            "entry carrying it faults on ANY access");
            }
            if (is_smap_candidate)
            {
                kprintf("[VMM]   Kernel dereferenced user-mapped page via "
                        "RIP 0x%lx without STAC/CLAC bracket.\n"
                        "[VMM]   Wrap the access in copy_to/from_user / "
                        "put_user / get_user, OR translate via "
                        "vmm_translate_user_addr() first.\n",
                        frame->rip);
            }
            /* Fall through to the kernel-panic block at the bottom of
             * exception_handler — full GPR + stack trace + halt-all-cores. */
        }
    }

    // User-mode exception: kill the faulting process, don't crash the kernel
    if ((frame->cs & 3) == 3)
    {
        process_t *proc = process_get_current();
        if (proc)
        {
            kprintf("\n");
            kprintf("[EXCEPTION] User-mode %s (vector %u) in PID %u\n",
                    exception_mnemonic((uint8_t)frame->vector),
                    frame->vector, proc->pid);
            kprintf("[EXCEPTION] RIP=0x%lx RSP=0x%lx Error=0x%lx\n",
                    frame->rip, frame->rsp, frame->error_code);
            /* Named from the process's own table, not the kernel's: the
             * address belongs to its image, and the backtrace below already
             * reads that table. Naming the faulting instruction while naming
             * everything that called it was an odd place to stop. */
            {
                NameplateName here;
                if (proc->nameplate_va && proc->nameplate_bytes &&
                    nameplate_name_at(proc->nameplate_va, proc->nameplate_bytes,
                                      (uintptr_t)frame->rip, &here))
                    kprintf("[EXCEPTION]   in %s+0x%lx\n", here.Text, here.Offset);
            }
            kprintf("[EXCEPTION] TagBits: 0x%lx\n", proc->cabin ? proc->cabin->tag_bits : 0);
            /* Split-lock #AC hint: if userspace fired #AC with error_code==0
             * while BoxOS was supposed to clear TEST_CTL.bit29, the
             * configuration drifted (BIOS re-asserted bit29 mid-runtime
             * is rare but documented). Surface the hint inline so the
             * operator knows what to investigate. */
            if (frame->vector == 17 && frame->error_code == 0)
            {
                kprintf("[EXCEPTION] Note: #AC error_code=0 suggests a "
                        "cache-line-spanning LOCK access; check TEST_CTL "
                        "MSR 0x33 bit 29 state on this core.\n");
            }

            if (frame->vector == 14)
            {
                kprintf("[EXCEPTION] Page Fault at 0x%lx (P=%d W=%d U=%d R=%d)\n",
                        fault_addr,
                        (int)(frame->error_code & 0x1),
                        (int)((frame->error_code >> 1) & 0x1),
                        (int)((frame->error_code >> 2) & 0x1),
                        (int)((frame->error_code >> 3) & 0x1));
                /* Full register dump on user page fault — enables remote
                 * debugging of "wild pointer in r10" / similar memory-
                 * corruption bugs without requiring a connected gdb. */
                kprintf("[EXCEPTION]  RAX=%016lx RBX=%016lx RCX=%016lx\n",
                        frame->rax, frame->rbx, frame->rcx);
                kprintf("[EXCEPTION]  RDX=%016lx RSI=%016lx RDI=%016lx\n",
                        frame->rdx, frame->rsi, frame->rdi);
                kprintf("[EXCEPTION]  RBP=%016lx  R8=%016lx  R9=%016lx\n",
                        frame->rbp, frame->r8, frame->r9);
                kprintf("[EXCEPTION]  R10=%016lx R11=%016lx R12=%016lx\n",
                        frame->r10, frame->r11, frame->r12);
                kprintf("[EXCEPTION]  R13=%016lx R14=%016lx R15=%016lx\n",
                        frame->r13, frame->r14, frame->r15);
            }

            /* Who called the faulting code.
             *
             * The registers above name the instruction that died; they never
             * name the caller, so a fault inside a shared leaf like memcpy or
             * memset says nothing about which call site passed the bad
             * pointer — and those are exactly the faults worth catching.
             *
             * Apps and the shell are compiled without -O, so RBP is a genuine
             * frame pointer: [RBP] is the caller's RBP and [RBP+8] its return
             * address. get_user_u64 reads through the SMAP bracket with
             * page-fault fixup, so a wild RBP — the very thing this dump
             * exists to report — ends the walk with -1 instead of faulting
             * the kernel inside its own exception handler.
             *
             * boxlib, though, is built at -O2 and omits frame pointers, so a
             * fault inside one of its leaves (memcpy, memset, strlen) reports
             * the chain starting at ITS caller's caller — the leaf's immediate
             * caller owns the RBP we start from and therefore names itself
             * only through the call instruction just before the first address
             * printed. Verified against a two-deep probe whose answer was
             * known in advance; do not read the first entry as "the function
             * that faulted".
             *
             * The names come from the image itself. Every BoxOS image carries
             * a Nameplate — a table the linker step builds from its own
             * symbols (src/include/nameplate_format.h) — and the loader noted
             * where it landed. Reading it means reaching into the faulting
             * process's address space, which is done the same way the frame
             * walk itself is done: through get_user, with page-fault fixup, so
             * a process that corrupted its own table gets an unnamed frame
             * rather than taking the kernel down while being diagnosed.
             *
             * A frame with no name is not a failure worth hiding — an address
             * in a leaf no symbol claims, or an image linked without a table,
             * prints as the bare address it always did. */
            {
                uint64_t fp = frame->rbp;
                kprintf("[EXCEPTION]  called from:\n");
                for (int depth = 0; depth < 8; depth++)
                {
                    uint64_t next = 0, ret = 0;
                    NameplateName site;

                    if (fp == 0 || (fp & 7) != 0)
                        break;
                    if (get_user_u64(&next, (const uint64_t *)(uintptr_t)fp) != 0)
                        break;
                    if (get_user_u64(&ret, (const uint64_t *)(uintptr_t)(fp + 8)) != 0)
                        break;
                    if (ret == 0)
                        break;

                    /* The return address points PAST the call, so the byte
                     * that belongs to the call is ret-1; naming ret itself
                     * would name the next function whenever a call is the
                     * last instruction of its own. */
                    if (nameplate_name_at(proc->nameplate_va, proc->nameplate_bytes,
                                          (uintptr_t)(ret - 1), &site))
                        /* +1 turns the offset of the looked-up byte back into
                         * the offset of the return address, which is what a
                         * reader compares against a disassembly. */
                        kprintf("[EXCEPTION]    0x%lx  %s+0x%lx\n", ret, site.Text,
                                site.Offset + 1);
                    else
                        kprintf("[EXCEPTION]    0x%lx\n", ret);

                    /* Frames march toward higher addresses. A chain that
                     * stalls or reverses is a smashed stack, not a caller. */
                    if (next <= fp)
                        break;
                    fp = next;
                }
            }

#ifdef CONFIG_BRINGUP_HOLD_ON_FIRST_FAULT
            g_bringup_held = 1;
            kprintf("\n");
            kprintf("====================================================================\n");
            kprintf("FIRST FAULT — HELD FOR BRING-UP. Everything above is the whole of it:\n");
            kprintf("the faulting address in CR2, every register, and the call chain by\n");
            kprintf("name. Nothing further will print. Photograph this screen.\n");
            kprintf("(Normal builds kill the process and carry on; this is `make BRINGUP=on`.)\n");
            kprintf("====================================================================\n");
            bringup_hold_forever();
#endif

            kprintf("[EXCEPTION] Killing PID %u and scheduling next process\n", proc->pid);

            // Mark process as crashed so scheduler won't pick it again
            process_set_state(proc, PROC_CRASHED);

            // Force a reschedule via the frame
            schedule(frame);
            return;
        }
    }

    // Kernel stack overflow recovery via double fault / stack fault
    if (frame->vector == 8 || frame->vector == 12)
    {
        process_t *overflow_proc = find_process_by_kernel_stack_overflow(frame->rsp);
        if (!overflow_proc)
        {
            overflow_proc = process_get_current();
            if (overflow_proc && process_is_idle(overflow_proc))
            {
                // idle process stack overflow is fatal — it cannot be killed
                kprintf("\n");
                kprintf("================================================================\n");
                kprintf("FATAL: Idle process (PID 0) kernel stack overflow\n");
                kprintf("  Exception #%u  RSP: 0x%lx  RIP: 0x%lx\n",
                        frame->vector, frame->rsp, frame->rip);
                kprintf("================================================================\n");
                overflow_proc = NULL;
                // fall through to kernel panic
            }
        }

        if (overflow_proc)
        {
            kprintf("\n");
            kprintf("================================================================\n");
            kprintf("KERNEL STACK OVERFLOW: Exception #%u recovered\n", frame->vector);
            kprintf("================================================================\n");
            kprintf("  PID: %u  TagBits: 0x%lx\n", overflow_proc->pid, overflow_proc->cabin ? overflow_proc->cabin->tag_bits : 0);
            kprintf("  RSP: 0x%lx  RIP: 0x%lx\n", frame->rsp, frame->rip);
            if ((frame->cs & 3) == 0)
                panic_name_addr("  in  ", frame->rip, false);
            if (overflow_proc->kernel_stack_guard_base)
            {
                kprintf("  Guard: 0x%lx  Stack: 0x%lx-0x%lx\n",
                        (uintptr_t)overflow_proc->kernel_stack_guard_base,
                        (uintptr_t)overflow_proc->kernel_stack,
                        (uintptr_t)overflow_proc->kernel_stack_top);
            }
            kprintf("  Killing PID %u, system continues.\n", overflow_proc->pid);
            kprintf("================================================================\n");

            process_set_state(overflow_proc, PROC_CRASHED);

            // If the faulting code held the scheduler lock, force-release it
            // so schedule() can acquire it without deadlock.
            // We use spin_force_release (NOT spin_unlock) because we're on an
            // IST stack — the original holder's saved_flags are meaningless here.
            // Safety: scheduler_get_state() returns THIS core's state (IST never
            // migrates to another core), so we always release our own lock.
            scheduler_state_t *sched = scheduler_get_state();
            if (sched->scheduler_lock.locked)
            {
                debug_printf("[IST] Core %u: force-releasing scheduler lock for crashed PID %u\n",
                             amp_get_core_index(), overflow_proc->pid);
                spin_force_release(&sched->scheduler_lock);
            }
            __asm__ volatile("cli");

            schedule(frame);
            return;
        }
    }

    /* Kernel-mode exception: this is a real kernel panic.
     *
     * One panic, one dump. Everything below prints — and printing touches the
     * VGA buffer, the serial UART and, for the stack trace, memory the fault
     * may already have proved untrustworthy. Any fault raised in the middle of
     * that re-enters here, and so does every other core that hits the same bug
     * at the same moment. Without this claim the screen fills with dumps and
     * the first one, the only one with the cause still in CR2, scrolls away.
     * That is what a six-core machine did on its first real boot: the panic
     * path already ended in `cli; hlt`, and it still never got there.
     *
     * The loser of the race stops without printing a word, because a second
     * dump scrolls the first one away just as effectively as a loop does. */
    panic_claim_or_halt(&panic_claimed_here);

    kprintf("\n");
    kprintf("====================================================================\n");
    kprintf("KERNEL PANIC: %s (vector %u)\n",
            exception_mnemonic((uint8_t)frame->vector), frame->vector);
    kprintf("====================================================================\n");
    kprintf("Error code: 0x%lx\n", frame->error_code);
    if (frame->vector == 17)
    {
        kprintf("Hint: #AC in kernel mode means BoxOS code emitted a "
                "cache-line-spanning LOCK instruction. Either a regression "
                "in atomic-target alignment or TEST_CTL bit 29 was re-asserted "
                "by firmware after boot. Fix the alignment in the offending "
                "atomic, don't mask the detection.\n");
    }
    kprintf("Exception count: %lu  Core: %u\n", atomic_load_u64(&exception_count), (uint32_t)amp_get_core_index());

    // Full GPR dump
    kprintf("  RAX=%016lx  RBX=%016lx\n", frame->rax, frame->rbx);
    kprintf("  RCX=%016lx  RDX=%016lx\n", frame->rcx, frame->rdx);
    kprintf("  RSI=%016lx  RDI=%016lx\n", frame->rsi, frame->rdi);
    kprintf("  RBP=%016lx  RSP=%016lx\n", frame->rbp, frame->rsp);
    kprintf("  R8 =%016lx  R9 =%016lx\n", frame->r8, frame->r9);
    kprintf("  R10=%016lx  R11=%016lx\n", frame->r10, frame->r11);
    kprintf("  R12=%016lx  R13=%016lx\n", frame->r12, frame->r13);
    kprintf("  R14=%016lx  R15=%016lx\n", frame->r14, frame->r15);
    kprintf("  RIP=%016lx  RFL=%016lx\n", frame->rip, frame->rflags);
    kprintf("  CS=%04lx  SS=%04lx\n", frame->cs, frame->ss);
    panic_name_addr("  fault at ", frame->rip, false);

    /* The same value the handler read on the way in — see the note at the top.
     * CR3 is read here because nothing about a fault disturbs it. */
    uint64_t panic_cr2 = fault_addr, panic_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(panic_cr3));
    kprintf("  CR2=%016lx  CR3=%016lx\n", panic_cr2, panic_cr3);

    if (frame->vector == 14)
    {
        kprintf("Page Fault at 0x%lx (P=%d W=%d U=%d R=%d I=%d)\n",
                panic_cr2,
                (int)(frame->error_code & 0x1),
                (int)((frame->error_code >> 1) & 0x1),
                (int)((frame->error_code >> 2) & 0x1),
                (int)((frame->error_code >> 3) & 0x1),
                (int)((frame->error_code >> 4) & 0x1));
    }

    // RBP-chain stack walk
    kprintf("Stack trace:\n");
    uint64_t walk_rbp = frame->rbp;
    for (uint32_t depth = 0; depth < 20; depth++)
    {
        if (walk_rbp == 0 || (walk_rbp & 7) != 0)
            break;
        if (!vmm_is_kernel_addr(walk_rbp))
            break;
        /* vmm_is_kernel_addr answers a question about the address, not about
         * the memory: a kernel-half address is not therefore a mapped one, and
         * the two frame words below are read from whatever the faulting code
         * left in RBP. Reading them unverified is how a stack trace becomes a
         * second page fault, which is how the dump naming the first one gets
         * replaced by a dump naming itself. */
        if (!panic_probe_range(walk_rbp, 16))
        {
            kprintf("  #%u  <frame at %016lx is not mapped — chain ends here>\n",
                    depth, walk_rbp);
            break;
        }

        uint64_t *fp = (uint64_t *)walk_rbp;
        uint64_t saved_rbp = fp[0];
        uint64_t ret_addr = fp[1];

        bool in_text = (ret_addr >= (uint64_t)_text_start && ret_addr < (uint64_t)_text_end);
        if (in_text)
        {
            kprintf("  #%u  ", depth);
            panic_name_addr("", ret_addr, true);
        }
        else
        {
            /* Outside .text: not a return address at all, so there is nothing
             * to name and naming the nearest thing would be a guess wearing a
             * function's clothes. */
            kprintf("  #%u  %016lx  [!]\n", depth, ret_addr);
        }

        if (saved_rbp <= walk_rbp || saved_rbp == 0)
            break;
        walk_rbp = saved_rbp;
    }

    kprintf("====================================================================\n");
    kprintf("System halted.\n");

    /* Halt all cores via IPI_PANIC. Guarded on the LAPIC actually being
     * mapped: lapic_send_ipi writes through the same MMIO window whose absence
     * turned this machine's first panic into a fault at address 0x20. A panic
     * raised before lapic_init() has no way to reach the other cores — and no
     * other cores to reach, since they are started through that same LAPIC. */
    if (g_amp.multicore_active && lapic_is_mapped())
    {
        for (uint8_t c = 0; c < g_amp.total_cores; c++)
        {
            if (c == amp_get_core_index())
                continue;
            if (!amp_core_online(&g_amp.cores[c]))
                continue;
            lapic_send_ipi(g_amp.cores[c].lapic_id, IPI_PANIC_VECTOR);
        }
    }

    while (1)
    {
        asm volatile("cli; hlt");
    }
}

/*
 * Is this kernel ready to be interrupted at all?
 *
 * Set once, from kernel_main, the moment scheduler_init() returns — because
 * that is what irq_handler needs in order to do its job, and interrupts
 * arriving before it find a handler with nowhere to record a tick. Anything
 * delivered while this is false got in through a door the kernel did not
 * open, and that is worth a sentence: on an i5-9400F booting UEFI the first
 * HPET tick landed thirty-six lines early, inside TSC calibration, and the
 * only evidence was a #PF at 0x18 with no hint of how an interrupt got in.
 * The handler is safe either way now; this names the how.
 */
volatile bool g_kernel_ready_for_interrupts = false;

void irq_handler(interrupt_frame_t *frame)
{
    uint8_t vector = frame->vector;

    if (!g_kernel_ready_for_interrupts)
    {
        /* Once. A storm of early interrupts is one fact, not a hundred. */
        static volatile bool said = false;
        if (!said)
        {
            said = true;
            kprintf("[IRQ] vector %u arrived before this kernel was ready to "
                    "be interrupted — something outside it set RFLAGS.IF "
                    "(a loader, or firmware returning from a runtime call)\n",
                    vector);
        }
    }

    // LAPIC spurious vector: do NOT send EOI
    if (vector == LAPIC_SPURIOUS_VECTOR)
    {
        return;
    }

    /* LAPIC timer vector: per-core tick counter + scheduling.
     *
     * Multi-core layout: BSP is a K-Core running kcore_run_loop, App
     * Cores run user processes. Only App Cores need preemptive
     * `schedule()` — calling it on a K-Core would hijack the kernel
     * loop. Hence the `amp_is_appcore()` gate.
     *
     * Single-core layout (AMP NOT active, or only one core total):
     * the BSP IS the App Core. The previous code never scheduled in
     * that case — processes were started but never switched, the
     * initial process held the CPU forever (audit 2026-04-30, BIOS
     * and UEFI single-core both reproduced).
     *
     * Schedule when either: (a) we're an App Core in multi-core, or
     * (b) AMP is inactive — i.e. there is no separate K-Core. */
    if (vector == LAPIC_TIMER_VECTOR)
    {
        /* A tick can land before there is a scheduler to count it into.
         * scheduler_get_state() answers NULL until scheduler_init() has
         * allocated the per-core array, and the timer is armed long before
         * that — irqchip_init and hpet_start_legacy_tick both run earlier in
         * kernel_main. An interrupt handler may never assume the init order
         * of anything it touches; the tick is still real, it simply has
         * nowhere to be recorded yet. */
        scheduler_state_t *s = scheduler_get_state();
        if (s) s->total_ticks++;
        /* Only App Cores reschedule on timer. K-Cores must NEVER call
         * schedule() — it would pick idle and hijack the K-Core's stack.
         *
         * Use total_cores (set ONCE during amp_init() before any AP boots)
         * instead of multicore_active (set LATE, after every AP comes
         * online). The previous `!g_amp.multicore_active` check
         * incorrectly fired for AP K-Cores whose LAPIC timer ticked
         * during the AP-boot window — schedule() ran on the K-Core and
         * permanently hijacked it into the per-core idle process. */
        if (amp_is_appcore() || g_amp.total_cores == 1)
        {
            schedule(frame);
        }
        /* Re-arm the next tick. No-op unless the timer runs in TSC-deadline
         * mode (one-shot per deadline); periodic mode reloads itself. */
        lapic_timer_rearm();
        lapic_send_eoi();
        return;
    }

    // IPI_WAKE: On App Cores, reschedule to pick up newly-woken processes.
    // On K-Cores, just ACK — the interrupt breaks HLT in kcore_run_loop.
    if (vector == IPI_WAKE_VECTOR)
    {
        if (amp_is_appcore() || g_amp.total_cores == 1)
        {
            schedule(frame);
        }
        lapic_send_eoi();
        return;
    }

    if (vector == IPI_SHOOTDOWN_VECTOR)
    {
        vmm_tlb_shootdown_handler();
        lapic_send_eoi();
        return;
    }

    if (vector == IPI_PANIC_VECTOR)
    {
        lapic_send_eoi();
        while (1)
        {
            asm volatile("cli; hlt");
        }
    }

    /* AHCI message-signalled interrupt. MSI is delivered point-to-point to
     * the LAPIC, so it is acknowledged with a LAPIC EOI — never the IOAPIC.
     * The handler itself is allocation-free and defers heavy work. */
    if (vector == AHCI_MSI_VECTOR)
    {
        ahci_irq_handler();
        lapic_send_eoi();
        return;
    }

    /* xHCI message-signalled interrupt. Same reasoning as AHCI above: MSI is
     * point-to-point to the LAPIC and is acknowledged there. This is the path
     * a real PCH takes — its INTx line is frequently absent or mis-described
     * in PCI configuration space, and a USB keyboard whose interrupts never
     * arrive is a keyboard that does not type. */
    if (vector >= XHCI_MSI_VECTOR && vector <= XHCI_MSI_VECTOR_LAST)
    {
        xhci_irq_handler_vector((uint8_t)vector);
        lapic_send_eoi();
        return;
    }

    // Standard hardware IRQ (vectors 32-55 -> IRQ 0-23)
    uint8_t irq = irq_vector_to_gsi(vector);
    if (irq >= IRQ_MAX_COUNT)
    {
        // Unknown vector outside our IRQ range — send EOI to prevent stuck interrupts
        irqchip_send_eoi(0);
        return;
    }
    atomic_fetch_add_u64(&irq_count[irq], 1);

    if (irq_callbacks[irq] != NULL)
    {
        irq_callbacks[irq]();
        irqchip_send_eoi(irq);
        return;
    }

    switch (irq)
    {
    case 0:
    {
        // Timer IRQ (PIT via PIC or IO-APIC GSI 0)
        //
        // ‼ The NULL check is not defensive dressing — it is the whole
        // difference between a machine that boots and one that does not, and
        // it took a board to say so. On an i5-9400F booting UEFI the first
        // HPET tick arrived inside cpu_calibrate_tsc, thirty-six lines of
        // kernel_main before scheduler_init() allocated any state, and this
        // line wrote through the NULL it got back: #PF at 0x18, dead. The
        // BIOS path never showed it because stage2 hands the kernel a machine
        // with interrupts already off, so nothing could be delivered this
        // early; _start now clears IF for both paths, which closes the window
        // rather than making it narrower. Everything below this point is
        // safe to run without a scheduler, and must stay that way.
        scheduler_state_t *sched = scheduler_get_state();
        if (sched) sched->total_ticks++;
        pit_tick();

        /* Advance the global scheduler clock from the MONOTONIC wall-clock at a
         * FIXED logical rate, decoupled from the adaptive PIT IRQ frequency.
         * The PIT IRQ rate swings 10-500 Hz with load, so a per-IRQ "+1" made a
         * tick's wall-clock duration drift — starvation/affinity thresholds
         * (counted in ticks) wobbled with load. Deriving the tick from
         * pit_get_uptime_us() pins it to SCHEDULER_DEFAULT_TICK_HZ (the rate
         * consumers were tuned for), now stable. Monotonic (uptime only grows);
         * single writer (BSP IRQ0); all consumers use deltas. */
        __atomic_store_n(&g_global_tick,
                         pit_get_uptime_us() / (1000000ULL / SCHEDULER_DEFAULT_TICK_HZ),
                         __ATOMIC_RELAXED);

        /* Periodic scheduler parameter recalculation */
        scheduler_recalc_parameters();

        /* APEI/GHES periodic poll — walks Polled-notify HEST sources
         * whose TSC deadline has elapsed and processes their GESB.
         * Cheap when nothing is due (per-source TSC compare in the
         * runtime path; no work outside that). Routes firmware-side
         * ECC events into the same poison + page-migration pipeline
         * as #MC-delivered events. */
        {
            extern void apei_ghes_poll_tick(void);
            apei_ghes_poll_tick();
        }

        /* Periodic TSC recalibration tick. Cheap — just stamps a
         * timestamp + sets a pending flag every TSC_RECAL_INTERVAL_US.
         * The actual ~20ms measurement runs in idle context
         * (cpu_tsc_recal_if_pending) — never in IRQ. */
        cpu_tsc_recal_tick(pit_get_uptime_us());

        /* Software key repeat driven by PIT tick */
        keyboard_timer_tick();

        /* Deferred touch event delivery */
        TouchQueueTick(__atomic_load_n(&g_global_tick, __ATOMIC_RELAXED));

        /* xHCI: poll for events when the controller has no usable interrupt,
         * and run the command and enumeration watchdogs either way. */
        xhci_tick();

        /* Ф26 M1 — AHCI async-completion watchdog (safety backstop). Reconciles
         * a lost/coalesced completion MSI from the port's PxSACT/PxCI level, and
         * fails + COMRESETs a genuinely wedged port so a lost completion cannot
         * hang a waiter forever. BSP-only (this is IRQ0) — the same core the
         * AHCI MSI targets; no-op unless multi-core async I/O is in flight. */
        ahci_watchdog_scan();

        /* Ф26 BMIDE — the legacy-PATA analogue: reconcile a lost IDE INTRQ from
         * the channel's BMISR latch, and fail + SRST-recover a genuinely wedged
         * channel so a dropped completion cannot hang a BMIDE sync waiter
         * forever. Same BSP-only tick (both need the disk's irqsave locks); the
         * SRST itself is deferred to a K-Core. No-op unless multi-core BMIDE
         * async I/O is in flight (dormant whenever AHCI owns block I/O). */
        {
            extern void bmide_watchdog_scan(void);
            bmide_watchdog_scan();
        }

        /* Single-core mode: drain irq_defer here, when the PIT IRQ
         * interrupted USERSPACE code.
         *
         * Background: irq_defer is the universal IRQ→K-Core hand-off
         * for bottom-half work that touches kmalloc / tagfs / process
         * tables. In multi-core configurations, K-Cores drain their
         * own rings inside `kcore_run_loop`. Single-core mode does NOT
         * run kcore_run_loop (the BSP runs userspace directly via
         * scheduler), so without an explicit pump nothing ever drains
         * the queued deferred handlers (keyboard Touch, USB Touch,
         * SCI/GPE notifications, write_job completions, ...).
         *
         * Lock-safety: the deferred handlers take heap_lock, tagfs
         * locks, process_lock, etc. If the PIT IRQ interrupted a
         * kernel-mode syscall holding one of those locks, pumping here
         * would deadlock against the interrupted thread on the same
         * core. We gate the pump on `(frame->cs & 3) == 3` — the
         * interrupted code was in user mode (CS=USER_CS, RPL=3) — which
         * guarantees no kernel lock is held. Multi-core takes the
         * `amp_is_appcore()` branch above and does not pump here. */
        if (g_amp.total_cores == 1 && (frame->cs & 3) == 3)
        {
            irq_defer_pump(0);
        }

        /* PIT IRQ 0: same scheduling rule as the LAPIC timer above —
         * preempt on App Cores or in single-core mode where the BSP
         * itself runs userspace. Use total_cores (set during amp_init
         * before any AP boots) — `multicore_active` is set LATE and was
         * causing AP K-Cores to be hijacked into idle on early ticks. */
        if (amp_is_appcore() || g_amp.total_cores == 1)
        {
            schedule(frame);
        }
        break;
    }

    case 1:
    {
        uint8_t scancode = inb(0x60);
        keyboard_handle_scancode(scancode);
        break;
    }

    default:
        /* Legacy IDE IRQ14/15 are intentionally not dispatched. The
         * PIO driver sets nIEN=1 on every detected drive and leaves
         * the IOAPIC pins masked, so the channel IRQ line never
         * fires. If something in the future re-enables it, falling
         * through here sends a clean EOI without acting on the
         * vector. */
        break;
    }

    irqchip_send_eoi(irq);
}

// ---------------------------------------------------------------------------
// Syscall dispatch: sync (single-core) vs async (multi-core)
// ---------------------------------------------------------------------------

// Sync path: process Pockets immediately on the calling core (N=1).
static void sync_syscall_dispatch(process_t *proc, interrupt_frame_t *frame)
{
    context_save_from_frame(proc, frame);
    ready_queue_push(&g_ready_queue, proc);
    /* Being served is a state of the counter, not of the passenger, and it
     * used to be written here as PROC_WAITING — "queued for the guide, not
     * for the scheduler". It established nothing: this core arrives with IF
     * clear (SFMASK carries IF for SYSCALL; vector 0x80 is an interrupt gate;
     * spin_lock saves and restores RFLAGS rather than enabling), there is no
     * second core on this path, and the strand is running, so no scheduler
     * pass can observe the mark between here and guide()'s return.
     *
     * What it did do was destroy every park a handler took. A handler that
     * parks — addr_park, touch_await, ObjRead, ObjWriteAsync — writes the same
     * PROC_WAITING to mean the opposite thing: "woken by event, not by the
     * next pass". Arriving already marked, its write was a no-op (old == new,
     * so no sched_dequeue), and guide()'s matching restore then read the mark
     * as its own and enqueued the strand it was supposed to leave asleep. A
     * 300 ms sleep cost 279 ms of processor time, the machine never reached
     * idle, and every wake gated on PROC_WAITING — touch_interrupt_deliver,
     * touch_queue_fire_wake, both arms of sync_ops — was skipped, because the
     * state those paths look for no longer existed on a single core.
     *
     * PROC_WAITING now has exactly one writer's intent in the kernel: a
     * handler put this strand to sleep. Do not mark anything here again. */
    guide();
    /* P5b: single-core never runs kcore_run_loop, so the strand reaper +
     * deferred cleanup must be driven from here (throttled). Without it,
     * exited strands accumulate and std::thread-style churn exhausts the
     * process table on uniprocessor. Cheap on 1c: vmm shootdown degrades to
     * a local invlpg (total_cores <= 1), so no IPI round-trips.
     *
     * The reaper MAY select the calling proc: if this very syscall was a
     * strand's own strand_exit, guide() above already flipped it to PROC_DONE,
     * so it now matches the reaper's filter. That is safe — it is still
     * current_process on this core, so process_destroy's is-running scan
     * declines it (and the snapshot ref is released); it is reaped a later
     * tick after schedule() switches away. */
    static uint32_t reap_tick = 0;
    if ((++reap_tick & 0x7u) == 0u)
    {
        process_reap_strands();
        process_cleanup_deferred();
    }
    schedule(frame);
}

// Async path: non-blocking doorbell to K-Core (N>1).
// Process stays PROC_WORKING and returns to userspace immediately.
// K-Core processes Pockets in parallel, writes Results to ResultRing.
// Process spins in result_wait() with pause until Result appears.
// No state change, no schedule(), no IPI back — true parallelism.
static void async_syscall_dispatch(process_t *proc, interrupt_frame_t *frame)
{
    (void)frame; // not needed — process continues running

    uint8_t core_idx = amp_get_core_index();

    // CAS: only submit if not already queued (dedup)
    if (atomic_cas_u8(&proc->kcore_pending, 0, 1))
    {
        debug_printf("[A%u] ASYNC submit PID %u to K-Core\n", core_idx, proc->pid);
        kcore_submit(proc);
    }
    else
    {
        debug_printf("[A%u] ASYNC skip PID %u (already pending)\n", core_idx, proc->pid);
    }
    // Return to userspace — iretq restores frame, process keeps running.
}

// Function pointer set at boot — never changes at runtime.
static void (*g_syscall_dispatch)(process_t *, interrupt_frame_t *) = sync_syscall_dispatch;

void idt_set_syscall_mode(bool multicore)
{
    if (multicore)
    {
        g_syscall_dispatch = async_syscall_dispatch;
        debug_printf("[IDT] Syscall mode: ASYNC (multi-core, non-blocking)\n");
    }
    else
    {
        g_syscall_dispatch = sync_syscall_dispatch;
        debug_printf("[IDT] Syscall mode: SYNC (single-core)\n");
    }
}

// ---------------------------------------------------------------------------
// syscall_handler — unified entry point for SYSCALL and INT 0x80
// ---------------------------------------------------------------------------

void syscall_handler(interrupt_frame_t *frame)
{
    scheduler_state_t *sched_state = scheduler_get_state();

    spin_lock(&sched_state->scheduler_lock);
    process_t *proc = sched_state->current_process;
    if (!proc || proc->magic != PROCESS_MAGIC)
    {
        spin_unlock(&sched_state->scheduler_lock);
        frame->rax = (uint64_t)-1;
        return;
    }
    spin_unlock(&sched_state->scheduler_lock);

    frame->rax = 0;

    /* Yield short-circuit. Cooperative-scheduling hint: pockets tagged
     * with POCKET_FLAG_YIELD skip guide() — the process stays WORKING
     * and just gives up its timeslice, returning to the run queue on
     * the next tick. Must be checked BEFORE the async dispatch path
     * to avoid a kcore_pending re-arm race. */
    Pocket *peek = KPocketPeek(proc);

    if (peek && (peek->flags & POCKET_FLAG_YIELD))
    {
        KPocketPop(proc);
        context_save_from_frame(proc, frame);
        schedule(frame);
        return;
    }

    // Dispatch: sync blocks + schedules, async returns immediately.
    g_syscall_dispatch(proc, frame);
}
