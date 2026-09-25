/* Lifeline — surviving a fault inside foreign firmware code.
 *
 * The contract, the shape and what this is NOT are in lifeline.h. This file is
 * the mechanism: an assembly capture that behaves like setjmp, a per-core
 * record of what is armed, and a catch that rewrites the interrupt frame so
 * isr_common's own epilogue performs the throwback.
 */

#include "lifeline.h"
#include "klib.h"
#include "amp.h"

/* ─── The pad's byte offsets, as the assembly below addresses them ───────── */

#define LIFELINE_PAD_RIP     0x00
#define LIFELINE_PAD_RSP     0x08
#define LIFELINE_PAD_RBP     0x10
#define LIFELINE_PAD_RBX     0x18
#define LIFELINE_PAD_R12     0x20
#define LIFELINE_PAD_R13     0x28
#define LIFELINE_PAD_R14     0x30
#define LIFELINE_PAD_R15     0x38
#define LIFELINE_PAD_RFLAGS  0x40
#define LIFELINE_PAD_CR3     0x48
#define LIFELINE_PAD_RANGES  0x50
#define LIFELINE_PAD_COUNT   0x58

#define LIFELINE_STR2(x) #x
#define LIFELINE_STR(x)  LIFELINE_STR2(x)

_Static_assert(__builtin_offsetof(LifelinePad, Rip)        == LIFELINE_PAD_RIP,    "pad layout");
_Static_assert(__builtin_offsetof(LifelinePad, Rsp)        == LIFELINE_PAD_RSP,    "pad layout");
_Static_assert(__builtin_offsetof(LifelinePad, Rbp)        == LIFELINE_PAD_RBP,    "pad layout");
_Static_assert(__builtin_offsetof(LifelinePad, Rbx)        == LIFELINE_PAD_RBX,    "pad layout");
_Static_assert(__builtin_offsetof(LifelinePad, R12)        == LIFELINE_PAD_R12,    "pad layout");
_Static_assert(__builtin_offsetof(LifelinePad, R13)        == LIFELINE_PAD_R13,    "pad layout");
_Static_assert(__builtin_offsetof(LifelinePad, R14)        == LIFELINE_PAD_R14,    "pad layout");
_Static_assert(__builtin_offsetof(LifelinePad, R15)        == LIFELINE_PAD_R15,    "pad layout");
_Static_assert(__builtin_offsetof(LifelinePad, Rflags)     == LIFELINE_PAD_RFLAGS, "pad layout");
_Static_assert(__builtin_offsetof(LifelinePad, Cr3)        == LIFELINE_PAD_CR3,    "pad layout");
_Static_assert(__builtin_offsetof(LifelinePad, Ranges)     == LIFELINE_PAD_RANGES, "pad layout");
_Static_assert(__builtin_offsetof(LifelinePad, RangeCount) == LIFELINE_PAD_COUNT,  "pad layout");

/* ─── What each core has armed ───────────────────────────────────────────── */

/* One station per core, cache-line sized so an AP arming its own lifeline
 * never dirties a line the BSP is reading. Only the owning core writes its
 * station and only the owning core's exception handler reads it, so there is
 * no cross-core ordering to establish — the atomics are here to stop the
 * COMPILER from caching `Armed` across the foreign call, nothing more. */
typedef struct
{
    LifelinePad *Armed;      /* NULL when this core has no lifeline out */
    uint64_t     PendingCr2; /* CR2 of the fault exception_handler is serving */
    LifelineFall Fall;
} __attribute__((aligned(64))) LifelineStation;

static LifelineStation g_lifeline[MAX_CORES];

/* ‼ WHICH CORE, WHEN GS MAY NOT BE VALID.
 *
 * amp_get_core_index() reads this core's dense index through %gs once
 * g_per_core_active is set, and the entry stubs keep the kernel GS base
 * installed in every kernel context, so inside exception_handler the read is
 * always the right one. Before per-core GS is live it falls back to the APIC
 * ID and, earlier still, returns 0 — which is not a guess, because nothing but
 * the BSP runs that early (amp.c documents both steps).
 *
 * What that means here: an arm and its catch are separated by one call, so
 * both sides land in the same window and read the same index. There is no path
 * on which one side gets the GS answer and the other the fallback. A core
 * index at or past MAX_CORES cannot happen — the type is uint8_t and MAX_CORES
 * is 256 — but the station is only ever reached through this helper, so if the
 * index ever widens, one function needs the bound, not five call sites. */
static LifelineStation *LifelineStationHere(void)
{
    unsigned idx = amp_get_core_index();
    if (idx >= MAX_CORES)
        return &g_lifeline[0];
    return &g_lifeline[idx];
}

/* ─── The vectors a lifeline will catch ──────────────────────────────────── */

/* Intel SDM Vol 3A §6.15, Table 6-1, gives each vector's class. A lifeline
 * catches a vector only when BOTH hold: foreign firmware can plausibly raise
 * it, and the class is Fault or Trap — an Abort leaves state the SDM declares
 * undefined, and a throwback needs the state it is throwing back FROM to be
 * true.
 *
 *   0  #DE  divide error
 *   5  #BR  BOUND range exceeded
 *   6  #UD  invalid opcode — firmware using an instruction we disabled in CR4
 *   7  #NM  device not available — firmware touching x87/SSE with CR0.TS set
 *   10 #TS  invalid TSS
 *   11 #NP  segment not present — a descriptor the firmware expects, gone
 *   13 #GP  general protection — the classic answer to firmware reaching for
 *           a descriptor, an MSR or an I/O port this kernel does not grant
 *   14 #PF  page fault — the observed failure on the owner's laptop
 *   16 #MF  x87 floating-point error
 *   17 #AC  alignment check
 *   19 #XM  SIMD floating-point error
 *
 * DELIBERATELY ABSENT, each for its own reason:
 *   8  #DF  ‼ Abort class, and it arrives on IST 1. The SDM's Double Fault
 *           entry says the saved CS:RIP is undefined and the program cannot be
 *           resumed or restarted — so the RIP test that is the whole safety of
 *           this mechanism cannot be trusted, and neither can the pad's claim
 *           to describe a live context. idt.c already answers #DF: a guard-page
 *           hit is recovered, a miss panics. A lifeline must not take that away.
 *   12 #SS  ‼ Fault class, and firmware can raise it — but it is one of the
 *           five vectors idt_init hands an IST (IST_STACK_FAULT), so it takes
 *           isr.asm's PARANOID entry rather than the ordinary one, and that
 *           path is what rules it out. The paranoid entry saves the GS base as
 *           it found it and installs this core's kernel base; the paranoid
 *           exit, for a frame returning to ring 0, WRMSRs the found base back
 *           (IA32_GS_BASE, Intel SDM Vol 2B "SWAPGS" — the shadow
 *           IA32_KERNEL_GS_BASE is reachable only by WRMSR at CPL 0, so the
 *           exit is replaying a fact about the INTERRUPTED code). That replay
 *           is right exactly while the frame still returns to the code that
 *           was interrupted. A throwback breaks that premise: it returns to
 *           OUR code, which never owned the firmware's GS base, and the exit
 *           would install that base underneath it — after which
 *           amp_get_core_index and every other gs: read in this kernel is
 *           reading firmware memory.
 *           A WRMSR from the catch cannot mend it, because the paranoid exit
 *           runs afterwards and overwrites whatever the catch wrote. Mending
 *           it means teaching that exit the frame was redirected, which is a
 *           change in isr.asm. Until that exists, a #SS inside firmware
 *           panics — the same outcome every vector had before this file, and
 *           a far smaller loss than a kernel running on a GS base it does not
 *           own.
 *   18 #MC  Abort class as well, and mce_handle has already judged it and
 *           returned false — which means fatal — before this file is reached.
 *   2  NMI  answered and returned in full by handle_nmi long before the catch.
 *   1  #DB / 3 #BP  a debugger's traps. Swallowing a breakpoint because a
 *           lifeline happened to be armed hides the one thing whoever set it
 *           was looking for.
 *   21 #CP  a shadow-stack or IBT violation is a statement about the return
 *           path itself, which is precisely what the throwback uses.
 *   4  #OF  INTO does not exist in 64-bit mode (SDM Vol 2A, INTO).
 */
#define LIFELINE_CATCHABLE                                                  \
    ((1u << 0) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 10) |           \
     (1u << 11) | (1u << 13) | (1u << 14) | (1u << 16) |                    \
     (1u << 17) | (1u << 19))

static bool LifelineVectorIsCatchable(uint64_t vector)
{
    if (vector >= 32)
        return false;
    return (LIFELINE_CATCHABLE & (1u << (unsigned)vector)) != 0;
}

static bool LifelineRipIsForeign(const LifelinePad *pad, uint64_t rip)
{
    for (uint64_t i = 0; i < pad->RangeCount; i++)
    {
        if (rip >= pad->Ranges[i].lo && rip < pad->Ranges[i].hi)
            return true;
    }
    return false;
}

/* ─── Arming ─────────────────────────────────────────────────────────────── */

void LifelineArmPublish(LifelinePad *pad);

/* Called from LifelineArm's assembly, with the pad already filled in.
 * Non-static because the assembly names it; nothing else may call it. */
__attribute__((used)) void LifelineArmPublish(LifelinePad *pad)
{
    LifelineStation *st = LifelineStationHere();

    if (!pad->Ranges || pad->RangeCount == 0)
    {
        kprintf("[LIFELINE] refused: a lifeline with no range of foreign code "
                "would catch nothing and hide the arming site's own faults\n");
        return;
    }

    if (__atomic_load_n(&st->Armed, __ATOMIC_RELAXED) != NULL)
    {
        /* Not a chain. A lifeline is tied around ONE call, and the only caller
         * in this kernel arms and disarms on adjacent lines. The outer pad is
         * kept rather than replaced: if a fault does arrive, unwinding too far
         * lands on a stack frame that still exists, while unwinding to a pad
         * whose frame has returned lands nowhere. */
        kprintf("[LIFELINE] core %u already has a lifeline out — the outer one "
                "is kept and this call is unprotected\n",
                (unsigned)amp_get_core_index());
        return;
    }

    __atomic_store_n(&st->Armed, pad, __ATOMIC_RELEASE);
}

/* ‼ WHY THIS IS ASSEMBLY AND NOT C.
 *
 * The throwback resumes at the instruction after `call LifelineArm`, and at
 * that instruction the SysV AMD64 ABI (§3.2.1, Figure 3.4) entitles the caller
 * to find RBX, RBP, R12-R15, RSP and RFLAGS.DF as it left them. Those values
 * must therefore be captured AS THE CALLER LEFT THEM — before any prologue of
 * ours has pushed or reused them. A C function cannot promise that: the
 * compiler owns its own prologue and may spill RBX on one build and not the
 * next, so a capture written in C would save this function's copies and the
 * throwback would hand the caller registers it never had. `__attribute__
 * ((naked))` would not help either, because GCC forbids C statements in a
 * naked function on x86.
 *
 * So the capture is the first thing the function does, in assembly, addressing
 * the pad by offsets that _Static_assert above binds to the struct. The return
 * address at (%rsp) and `lea 8(%rsp)` are the resume point and the stack
 * pointer the caller will have once the `ret` has popped — the same two facts
 * setjmp saves, for the same reason.
 *
 * No throwback assembly is needed to match it: interrupt_frame_t already
 * carries every GPR plus RIP/CS/RFLAGS/RSP/SS, and isr_common's epilogue pops
 * all of them and IRETQs, so LifelineCatch writes C to that struct and the
 * existing exit path performs the jump. In 64-bit mode IRET pops RSP and SS
 * unconditionally, even returning to the same privilege level (SDM Vol 2A,
 * IRET/IRETD/IRETQ), which is what makes the stack switch in the frame real.
 *
 * ENDBR64 is here because the tree builds with -fcf-protection=full and
 * cet_lifecycle keeps IA32_S_CET.ENDBR_EN set supervisor-side. The direct call
 * from C does not require it; an ENDBR at every function entry is the rule the
 * rest of the image follows and costs four bytes. The `int3` after `ret`
 * matches -mharden-sls=all, which the compiler applies to every other return
 * in this image.
 */
__asm__(
    ".text\n"
    ".globl LifelineArm\n"
    ".type  LifelineArm, @function\n"
    "LifelineArm:\n"
    "    endbr64\n"
    "    movq  (%rsp), %rax\n"                              /* return address */
    "    movq  %rax, " LIFELINE_STR(LIFELINE_PAD_RIP)    "(%rdi)\n"
    "    leaq  8(%rsp), %rax\n"                            /* RSP after `ret` */
    "    movq  %rax, " LIFELINE_STR(LIFELINE_PAD_RSP)    "(%rdi)\n"
    "    movq  %rbp, " LIFELINE_STR(LIFELINE_PAD_RBP)    "(%rdi)\n"
    "    movq  %rbx, " LIFELINE_STR(LIFELINE_PAD_RBX)    "(%rdi)\n"
    "    movq  %r12, " LIFELINE_STR(LIFELINE_PAD_R12)    "(%rdi)\n"
    "    movq  %r13, " LIFELINE_STR(LIFELINE_PAD_R13)    "(%rdi)\n"
    "    movq  %r14, " LIFELINE_STR(LIFELINE_PAD_R14)    "(%rdi)\n"
    "    movq  %r15, " LIFELINE_STR(LIFELINE_PAD_R15)    "(%rdi)\n"
    "    pushfq\n"
    "    popq  %rax\n"
    "    movq  %rax, " LIFELINE_STR(LIFELINE_PAD_RFLAGS) "(%rdi)\n"
    "    mov   %cr3, %rax\n"
    "    movq  %rax, " LIFELINE_STR(LIFELINE_PAD_CR3)    "(%rdi)\n"
    "    movq  %rsi, " LIFELINE_STR(LIFELINE_PAD_RANGES) "(%rdi)\n"
    "    movl  %edx, %eax\n"                     /* unsigned n, zero-extended */
    "    movq  %rax, " LIFELINE_STR(LIFELINE_PAD_COUNT)  "(%rdi)\n"
    /* RSP is 8 mod 16 at entry; one push of our own makes it 0 at the call,
     * which SysV AMD64 ABI §3.2.2 requires. */
    "    subq  $8, %rsp\n"
    "    call  LifelineArmPublish\n"
    "    addq  $8, %rsp\n"
    "    xorl  %eax, %eax\n"
    "    ret\n"
    "    int3\n"
    ".size  LifelineArm, .-LifelineArm\n");

void LifelineDisarm(void)
{
    LifelineStation *st = LifelineStationHere();
    __atomic_store_n(&st->Armed, (LifelinePad *)NULL, __ATOMIC_RELEASE);
}

/* ─── Catching ───────────────────────────────────────────────────────────── */

void LifelineNoteFaultAddress(uint64_t cr2)
{
    LifelineStationHere()->PendingCr2 = cr2;
}

bool LifelineCatch(interrupt_frame_t *frame)
{
    LifelineStation *st = LifelineStationHere();
    LifelinePad *pad = __atomic_load_n(&st->Armed, __ATOMIC_ACQUIRE);

    if (!pad)
        return false;

    /* Ring 0 only. A ring-3 fault is a process's business and is killed by the
     * path below this one; a lifeline has nothing to say about it. */
    if ((frame->cs & 3) != 0)
        return false;

    if (!LifelineVectorIsCatchable(frame->vector))
        return false;

    /* ‼ THE ONE RULE THIS WHOLE FILE EXISTS TO ENFORCE.
     *
     * The fault must have happened in code this kernel did not compile. A
     * fault at a kernel RIP while a lifeline is armed is a kernel bug, and it
     * panics exactly as it did before this file existed. Without this test a
     * lifeline would be a blanket thrown over every ring-0 fault raised during
     * the window, which is how a recovery mechanism becomes a way of never
     * finding out. */
    if (!LifelineRipIsForeign(pad, frame->rip))
        return false;

    /* ─── From here on, the throwback is certain. ───
     *
     * CR3 FIRST, and before anything that reads memory the firmware may have
     * stopped describing. UEFI 2.10 §2.3.4 (x64 calling conventions) makes a
     * runtime service responsible for preserving the processor state it was
     * called with, but a firmware that faults mid-service has already broken
     * one promise and is not owed the benefit of the doubt on another; if it
     * loaded its own page tables and died before restoring ours, every load
     * after this point — the kprintf below among them — would fault a second
     * time, and that one nothing catches.
     *
     * Only when it differs: a needless CR3 write is a full TLB flush (SDM
     * Vol 3A §4.10.4.1) on the common path, where the firmware behaved.
     *
     * WHAT IS DELIBERATELY NOT RESTORED, and why:
     *   IDTR  the exception was delivered through OUR gate — that is how this
     *         code is running — so IDTR is already ours. Proven, not assumed.
     *   GDTR  delivery loaded CS from that gate and the processor validated
     *         the descriptor; frame->cs is a kernel selector, so the GDT it
     *         was validated against is ours.
     *   TR    reloading it means clearing the busy bit in a descriptor and
     *         executing LTR, a procedure that can itself #GP (SDM Vol 3A
     *         §7.2.3). A restore that can fail worse than the thing it repairs
     *         is not a restore. For IST vectors the CPU read the IST pointer
     *         out of the TSS to deliver this fault, so TR was ours anyway.
     *   FS/GS base  if the firmware had clobbered the kernel GS base,
     *         amp_get_core_index() above would already have read garbage and
     *         this decision would have been made on the wrong core's station.
     *         There is no way to check the base after the fact that is not
     *         also a way to be wrong about it; the tree's one true anchor is
     *         TR + SGDT (per_core.h), and putting a second copy of that
     *         machinery here is not this lane's work. Named as exposure.
     *   EFER, PAT, STAR/LSTAR/CSTAR/SFMASK  these belong to per-core init,
     *         which holds the only correct values. This file would have to
     *         cache a copy, and a cached copy that drifts writes a WRONG value
     *         over a right one. UEFI 2.10 §2.3.4 places them among the state
     *         the firmware must preserve; where it did not, the machine is
     *         past what a lifeline can mend.
     */
    uint64_t cr3_now;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_now));
    if (cr3_now != pad->Cr3)
        __asm__ volatile("mov %0, %%cr3" ::"r"(pad->Cr3) : "memory");

    /* CR2 belongs to vector 14 and to nothing else — Intel SDM Vol 3A §4.7
     * gives page-fault delivery as the only writer, so on any other vector the
     * register still names an earlier page fault on this core. Recording it
     * anyway is how the fall record came to carry an address the fall never
     * touched, and how the reader came to resolve that address against the EFI
     * memory map and name a descriptor with conviction. Zero, and a flag that
     * says there is nothing to say, is the true answer. */
    st->Fall.vector = frame->vector;
    st->Fall.error_code = frame->error_code;
    st->Fall.rip = frame->rip;
    st->Fall.cr2_valid = (frame->vector == 14);
    st->Fall.cr2 = st->Fall.cr2_valid ? st->PendingCr2 : 0;
    st->Fall.cr3 = cr3_now;
    st->Fall.valid = true;

    /* Disarmed by the catch, not only by the caller. The lifeline has done the
     * one job it was tied for, and leaving it armed would mean a second fault
     * — now necessarily on the way back through our own code — meeting a pad
     * whose frame is already being unwound. The caller's LifelineDisarm on the
     * thrown-back path then finds nothing to do, which is what idempotent
     * means. */
    __atomic_store_n(&st->Armed, (LifelinePad *)NULL, __ATOMIC_RELEASE);

    kprintf("[LIFELINE] core %u: vector %lu at RIP 0x%lx is inside foreign "
            "code — throwing back (err=0x%lx, CR3 was 0x%lx%s)\n",
            (unsigned)amp_get_core_index(), frame->vector, frame->rip,
            frame->error_code, cr3_now,
            cr3_now != pad->Cr3 ? ", NOT ours — restored" : "");
    if (st->Fall.cr2_valid)
        kprintf("[LIFELINE]   fault address CR2=0x%lx (LINEAR — the window it "
                "was reached through decides what physical page that is)\n",
                st->Fall.cr2);
    else
        kprintf("[LIFELINE]   vector %lu sets no fault address; CR2 is left "
                "holding an earlier page fault's and is not printed\n",
                frame->vector);

    frame->rip = pad->Rip;
    frame->rsp = pad->Rsp;
    frame->rbp = pad->Rbp;
    frame->rbx = pad->Rbx;
    frame->r12 = pad->R12;
    frame->r13 = pad->R13;
    frame->r14 = pad->R14;
    frame->r15 = pad->R15;
    frame->rflags = pad->Rflags;
    frame->rax = 1; /* LifelineArm's second return */

    return true;
}

const LifelineFall *LifelineLastFall(void)
{
    return &LifelineStationHere()->Fall;
}
