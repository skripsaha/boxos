#include "idt.h"
#include "serial.h"
#include "gdt.h"
#include "tss.h"
#include "klib.h"
#include "io.h"
#include "pic.h"
#include "irqchip.h"
#include "lapic.h"
#include "process.h"
#include "guide.h"
#include "ready_queue.h"
#include "pocket_ring.h"
#include "kring.h"
#include "vmm.h"
#include "uaccess.h"
#include "nameplate.h"
#include "nameplate_format.h"
#include "atomics.h"
#include "touch.h"
#include "scheduler.h"
#include "context_switch.h"
#include "keyboard.h"
#include "ahci.h"
#include "idle.h"
#include "amp.h"
#include "kcore.h"
#include "xhci_interrupt.h"
#include "linker_symbols.h"
#include "cpu_calibrate.h"
#include "touch_queue.h"
#include "nightwatch.h"
#include "clockboard.h"
#include "pit.h"
#include "baton.h"

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
        uint8_t ist = 0;

        switch (i)
        {
        case 1:
            ist = IST_DEBUG;
            break;
        case 2:
            ist = IST_NMI;
            break;
        case 8:
            ist = IST_DOUBLE_FAULT;
            break;
        case 12:
            ist = IST_STACK_FAULT;
            break;
        case 18:
            ist = IST_MACHINE_CHECK;
            break;
        }

        idt_set_entry(i, (uint64_t)isr_table[i], GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, ist);
    }

    debug_printf("[IDT] Setting up IRQ handlers (32-55)...\n");

    for (int i = 32; i < 56; i++)
    {
        idt_set_entry(i, (uint64_t)isr_table[i], GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);
    }

    debug_printf("[IDT] Setting up syscall handler (INT 0x80)...\n");

    idt_set_entry(SYSCALL_VECTOR, (uint64_t)isr_table[SYSCALL_VECTOR], GDT_KERNEL_CODE, IDT_TYPE_USER_INTERRUPT, 0);

    idt_set_entry(LAPIC_TIMER_VECTOR, (uint64_t)isr_table[LAPIC_TIMER_VECTOR],
                  GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);
    idt_set_entry(LAPIC_SPURIOUS_VECTOR, (uint64_t)isr_table[LAPIC_SPURIOUS_VECTOR],
                  GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);

    idt_set_entry(AHCI_MSI_VECTOR, (uint64_t)isr_table[AHCI_MSI_VECTOR],
                  GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);

    for (uint32_t v = XHCI_MSI_VECTOR; v <= XHCI_MSI_VECTOR_LAST; v++)
    {
        idt_set_entry((uint8_t)v, (uint64_t)isr_table[v],
                      GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);
    }

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
static volatile uint32_t g_bringup_held = 0;

static void bringup_hold_forever(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}
#endif

static bool panic_probe_present(uint64_t va)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    const uint64_t ADDR_MASK = 0x000FFFFFFFFFF000ULL;
    const uint64_t P = 1ULL << 0;
    const uint64_t PS = 1ULL << 7;

    uint64_t *table = (uint64_t *)vmm_phys_to_virt(cr3 & ADDR_MASK);
    if (!table)
        return false;

    for (int level = 3; level >= 0; level--)
    {
        uint64_t entry = table[(va >> (12 + level * 9)) & 0x1FF];
        if (!(entry & P))
            return false;
        if (level > 0 && (entry & PS))
            return true;
        table = (uint64_t *)vmm_phys_to_virt(entry & ADDR_MASK);
        if (!table)
            return false;
    }
    return true;
}

static void panic_claim_or_halt(bool *already_claimed_here)
{
    if (*already_claimed_here)
        return;

    static volatile uint32_t panic_claimed = 0;
    if (__atomic_exchange_n(&panic_claimed, 1u, __ATOMIC_ACQ_REL) != 0)
    {
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    *already_claimed_here = true;
}

extern const char __nameplate_start[];
extern const char __nameplate_end[];

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

static bool panic_probe_range(uint64_t va, uint64_t len)
{
    return panic_probe_present(va) &&
           panic_probe_present(va + len - 1);
}

void exception_handler(interrupt_frame_t *frame)
{
    bool panic_claimed_here = false;

    uint64_t fault_addr;
    __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));

    atomic_fetch_add_u64(&exception_count, 1);

#ifdef CONFIG_BRINGUP_HOLD_ON_FIRST_FAULT
    if (g_bringup_held)
        bringup_hold_forever();
#endif

    if (frame->vector == 2)
    {
        extern bool apei_ghes_nmi_check(void);
        (void)apei_ghes_nmi_check();
    }

    if (frame->vector == 18)
    {
        extern bool mce_handle(interrupt_frame_t *);
        if (mce_handle(frame))
        {
            return;
        }
    }

    if (frame->vector == 21)
    {
        uint16_t cp_type = (uint16_t)(frame->error_code & 0x7FFFu);
        bool cp_enclave = (frame->error_code & 0x8000u) != 0;
        kprintf("[CET] #CP fired: type=%u enclave=%d RIP=0x%lx\n",
                (unsigned)cp_type, (int)cp_enclave, frame->rip);
        if ((frame->cs & 3) == 0)
            panic_name_addr("[CET]   in ", frame->rip, false);
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
        {
            extern void cet_lifecycle_record_cp_fault(void);
            cet_lifecycle_record_cp_fault();
        }
    }

    if (frame->vector == 14)
    {
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

        if ((frame->cs & 3) == 0)
        {
            panic_claim_or_halt(&panic_claimed_here);

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
        }
    }

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
            {
                NameplateName here;
                if (proc->nameplate_va && proc->nameplate_bytes &&
                    nameplate_name_at(proc->nameplate_va, proc->nameplate_bytes,
                                      (uintptr_t)frame->rip, &here))
                    kprintf("[EXCEPTION]   in %s+0x%lx\n", here.Text, here.Offset);
            }
            kprintf("[EXCEPTION] TagBits: 0x%lx\n", proc->cabin ? proc->cabin->tag_bits : 0);
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

                    if (nameplate_name_at(proc->nameplate_va, proc->nameplate_bytes,
                                          (uintptr_t)(ret - 1), &site))
                        kprintf("[EXCEPTION]    0x%lx  %s+0x%lx\n", ret, site.Text,
                                site.Offset + 1);
                    else
                        kprintf("[EXCEPTION]    0x%lx\n", ret);

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

            process_set_state(proc, PROC_CRASHED);

            schedule(frame);
            return;
        }
    }

    if (frame->vector == 8 || frame->vector == 12)
    {
        process_t *overflow_proc = find_process_by_kernel_stack_overflow(frame->rsp);
        if (!overflow_proc)
        {
            overflow_proc = process_get_current();
            if (overflow_proc && process_is_idle(overflow_proc))
            {
                kprintf("\n");
                kprintf("================================================================\n");
                kprintf("FATAL: Idle process (PID 0) kernel stack overflow\n");
                kprintf("  Exception #%u  RSP: 0x%lx  RIP: 0x%lx\n",
                        frame->vector, frame->rsp, frame->rip);
                kprintf("================================================================\n");
                overflow_proc = NULL;
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

    kprintf("Stack trace:\n");
    uint64_t walk_rbp = frame->rbp;
    for (uint32_t depth = 0; depth < 20; depth++)
    {
        if (walk_rbp == 0 || (walk_rbp & 7) != 0)
            break;
        if (!vmm_is_kernel_addr(walk_rbp))
            break;
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
            kprintf("  #%u  %016lx  [!]\n", depth, ret_addr);
        }

        if (saved_rbp <= walk_rbp || saved_rbp == 0)
            break;
        walk_rbp = saved_rbp;
    }

    kprintf("====================================================================\n");
    kprintf("System halted.\n");

    WireForceRelease();
    WireDrain();

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

volatile bool g_kernel_ready_for_interrupts = false;

void irq_handler(interrupt_frame_t *frame)
{
    uint8_t vector = frame->vector;

    if (!g_kernel_ready_for_interrupts)
    {
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

    if (vector == LAPIC_SPURIOUS_VECTOR)
    {
        return;
    }

    if (vector == LAPIC_TIMER_VECTOR)
    {
        scheduler_state_t *s = scheduler_get_state();
        if (s) s->total_ticks++;
        if (amp_is_appcore() || g_amp.total_cores == 1)
        {
            schedule(frame);
        }
        lapic_timer_rearm();
        lapic_send_eoi();
        return;
    }

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

    if (vector == AHCI_MSI_VECTOR)
    {
        ahci_irq_handler();
        lapic_send_eoi();
        return;
    }

    if (vector >= XHCI_MSI_VECTOR && vector <= XHCI_MSI_VECTOR_LAST)
    {
        xhci_irq_handler_vector((uint8_t)vector);
        lapic_send_eoi();
        return;
    }

    uint8_t irq = irq_vector_to_gsi(vector);
    if (irq >= IRQ_MAX_COUNT)
    {
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
        scheduler_state_t *sched = scheduler_get_state();
        if (sched) sched->total_ticks++;
        pit_tick();

        __atomic_store_n(&g_global_tick,
                         pit_get_uptime_us() / (1000000ULL / SCHEDULER_DEFAULT_TICK_HZ),
                         __ATOMIC_RELAXED);

        scheduler_recalc_parameters();

        {
            extern void apei_ghes_poll_tick(void);
            apei_ghes_poll_tick();
        }

        cpu_tsc_recal_tick(pit_get_uptime_us());

        keyboard_timer_tick();

        TouchQueueTick(__atomic_load_n(&g_global_tick, __ATOMIC_RELAXED));

        xhci_tick();

        ahci_watchdog_scan();

        {
            extern void bmide_watchdog_scan(void);
            bmide_watchdog_scan();
        }

        if (g_amp.total_cores == 1 && (frame->cs & 3) == 3)
        {
            BatonPump(0);
        }

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
        break;
    }

    irqchip_send_eoi(irq);
}


static void sync_syscall_dispatch(process_t *proc, interrupt_frame_t *frame)
{
    context_save_from_frame(proc, frame);
    ready_queue_push(&g_ready_queue, proc);
    guide();
    static uint32_t reap_tick = 0;
    if ((++reap_tick & 0x7u) == 0u)
    {
        process_reap_strands();
        process_cleanup_deferred();
    }
    schedule(frame);
}

static void async_syscall_dispatch(process_t *proc, interrupt_frame_t *frame)
{
    (void)frame;

    uint8_t core_idx = amp_get_core_index();

    if (atomic_cas_u8(&proc->kcore_pending, 0, 1))
    {
        debug_printf("[A%u] ASYNC submit PID %u to K-Core\n", core_idx, proc->pid);
        kcore_submit(proc);
    }
    else
    {
        debug_printf("[A%u] ASYNC skip PID %u (already pending)\n", core_idx, proc->pid);
    }
}

static void (*g_syscall_dispatch)(process_t *, interrupt_frame_t *) = sync_syscall_dispatch;
static bool g_gate_async;

void idt_set_syscall_mode(bool multicore)
{
    g_gate_async = multicore;
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

    if (__atomic_load_n(&proc->owed_count, __ATOMIC_RELAXED) != 0)
        TouchOwedHandOver(proc);

    if (frame->rdi == GATE_YIELD)
    {
        if (!g_gate_async)
        {
            sync_syscall_dispatch(proc, frame);
            return;
        }
        if (!KPocketIsEmpty(proc))
            async_syscall_dispatch(proc, frame);
        context_save_from_frame(proc, frame);
        schedule(frame);
        return;
    }

    g_syscall_dispatch(proc, frame);
}