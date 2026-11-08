/* klib_panic.c — one panic, one account, and every other core stopped.
 *
 * Everything an account of a panic prints touches something: the glass,
 * the serial UART, and — for the dump's stack trace — memory the fault may
 * already have proved untrustworthy. A fault raised in the middle of that
 * comes back here, and so does every other core that hit the same bug at the
 * same instant. Without a claim the screen fills and the first account, the
 * only one whose CR2 still names the original cause, scrolls away. That is
 * what a six-core machine did on its first real boot: the path already ended
 * in `cli; hlt`, and it still never got there.
 *
 * One claim for both accounts. The exception dump (idt.c) had one; panic()
 * (klib_print.c) took none, so two panics at once wove two banners into one,
 * and a panic beside a dump broke the console lock out from under the dump's
 * line (spin_force_release) and said its banner into the middle of it.
 * Claimed BEFORE the first line, not after: the loser must leave no trace at
 * all, because half a line of someone else's account on the end of the one
 * that matters is its own kind of confusion.
 *
 * The claim is never given back, and its owner never leaves its account:
 * panic() and the dump end standing (PanicEndAccount), a fault or a panic
 * inside either ends it here, one line saying by what, standing — and what
 * brings the owner back here once the account is said whole, a fatal #MC in
 * its drain or waking it from its stand, one line saying what came after it,
 * standing again; and the one road the exception handler has back to work —
 * the recovery of a kernel stack overflow, which discards the context and
 * schedules another — is closed to the owner (idt.c). That is what makes
 * "the owner of a panic waits on no lock" (klib_lock.c) sound: with every
 * other core stopped, the only code left running is the account, and the
 * account only says. */
#include "klib_panic.h"
#include "klib.h"
#include "klib_logring.h" /* LogKeepFlush — what was said outlives a warm reset */
#include "serial.h"       /* WireDrain — and reaches the wire */
#include "emergency_screen.h" /* the glass the account is drawn on, whoever held it */
#include "amp.h"          /* g_amp — the cores there are to stop */
#include "lapic.h"        /* lapic_is_mapped, LapicSendNmi */
#include "mce.h"          /* MCE_MSR_MCG_STATUS — no machine check left in progress */

/* 0 while no core has panicked; else the claimant's core index + 1. Set once,
 * by the exchange its winner makes, and published before the first NMI is
 * sent (a locked CMPXCHG; the xAPIC ICR is an uncacheable store after it, and
 * the x2APIC send fences before its WRMSR — lapic.c, icr_send). */
static volatile uint32_t g_panic_owner;

/* How many times the owner has come to the claim: 1 for its account, 2 for a
 * fault or a panic inside it — or for what brings it back once the account is
 * said whole (g_panic_account_said) — 3 for one inside the line that says so.
 * Touched by the owner alone, after it holds the claim, so it needs no
 * atomic. */
static uint32_t g_panic_depth;

/* Set by the owner once the last line of its account is said, before that
 * account goes out to the memory the next boot reads and to the wire
 * (PanicEndAccount); read by it alone, when something brings it back to the
 * claim after that — in the drain, or woken from the stand. Volatile: made
 * before the hlt, not after a loop the compiler knows never ends. */
static volatile bool g_panic_account_said;

static uint32_t PanicMe(void)
{
    return (uint32_t)amp_get_core_index() + 1u;
}

/* CR4.MCE (Intel SDM Vol 3A, "Control Registers"). */
#define PANIC_CR4_MCE (1ull << 6)

/* No machine check left in progress on a core a panic stands or holds.
 * MCG_STATUS.MCIP is set from the moment a #MC is delivered until its handler
 * clears it at the end of its walk of the banks (mce.c, mce_handle), and "the
 * occurrence of a second Machine-Check Event while MCIP is set will cause the
 * processor to enter a shutdown state" (SDM Vol 3B, "IA32_MCG_STATUS MSR") —
 * which most chipsets answer by resetting the platform, the account with it.
 * A #MC handler does not hold NMIs back, so the NMI that stops a core can stop
 * it inside that walk; a broadcast #MC puts every core in one at once, and a
 * second may follow on the very memory the dump then walks. The fixed vector
 * the stop was before waited for the handler's IRET, MCIP already clear. So
 * the stand clears it as the walk would have — one MSR write, nothing said or
 * taken, the banks keeping their records for whoever reads them after the
 * reset — and so does the claim's winner, whose account a fault inside its
 * own walk may be. Asked only where CR4.MCE is set: mce.c sets it only where
 * CPUID reports MCE and MCA, so the MSR is there, and where it is clear a #MC
 * is a shutdown whatever this does. */
static void PanicLeaveMachineCheck(void)
{
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (!(cr4 & PANIC_CR4_MCE))
        return;
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MCE_MSR_MCG_STATUS));
    if (lo & MCE_MCG_STATUS_MCIP)
        __asm__ volatile("wrmsr" : : "c"(MCE_MSR_MCG_STATUS), "a"(0u), "d"(0u));
}

static __attribute__((noreturn)) void PanicStand(void)
{
    PanicLeaveMachineCheck();
    for (;;)
        __asm__ volatile("cli; hlt");
}

/* Every other core stopped, by an NMI — which a core takes whatever its
 * RFLAGS.IF says. The fixed vector this used to be (IPI_HALT_VECTOR, which
 * only the halt sends now, system_halt.c) waits in the LAPIC's IRR of a core
 * with interrupts off for as long as it keeps them off, and the startup tests
 * run on the BSP with interrupts off from the first line of kernel_main to the
 * sti that ends it: a K-Core that panicked beside them stopped every core but
 * that one, and the BSP went on saying things while the account of the panic
 * sank behind it. Intel SDM Vol 3A, "Interrupt
 * Command Register (ICR)": Delivery Mode 100 (NMI) "delivers an NMI interrupt
 * to the target processor or processors. The vector information is ignored."
 *
 * Sent to each core the kernel runs — online, and not this one — and not by
 * the "all excluding self" shorthand. The shorthand reaches every APIC on the
 * bus: a processor this kernel never started (past MAX_CORES, or parked by
 * the firmware in a loop over an IDT the kernel has since taken back), and an
 * AP still in its trampoline with no IDT of the kernel's yet (amp.c: a late
 * core). The fixed vector found those with interrupts off and stayed in their
 * IRR; an NMI goes through whatever IDT they hold, and a triple fault there is
 * a shutdown, which most chipsets answer by resetting the platform — the
 * account with it. A core is online only once its own IDT and IST stacks are
 * live (ap_init.c, the release store after per_core_init_ap), and that is all
 * the walk asks — not g_amp.multicore_active, which amp_boot_aps raises only
 * once the last AP has been waited for, while the ones already up run their
 * loops (amp.c). Linux stops the other CPUs of a panic the same way:
 * native_stop_other_cpus sends its NMI to the online mask. A core the orderly
 * halt's roll call gave up on is online no more (system_halt.c: it never
 * answered the halt's vector, so it runs with interrupts off) and is not sent
 * one either; that halt has named it already, and a panic after it leaves it
 * where it runs.
 *
 * Guarded on the LAPIC being mapped: a panic raised before lapic_init has no
 * way to reach another core and no other core to reach — they are started
 * through that same LAPIC — and writing through an unmapped MMIO window would
 * fault inside the panic, which is what turned this machine's first panic into
 * a fault at address 0x20. NOT waited for: a core that will never answer must
 * not cost the machine the only account of why it died.
 *
 * A core this stops stops wherever it stands — inside a spin_lock section too,
 * which the fixed vector never could reach (spin_lock runs with interrupts off
 * to its unlock). So a lock such a core held is never given back, and the
 * account must be said without waiting on one: the owner takes a held lock as
 * it stands (klib_lock.c, spin_lock), and a kprintf begun after the claim
 * stands before it touches the console (klib_print.c).
 *
 * An NMI on a core already standing is harmless. A loser stands in `cli; hlt`
 * at the claim; the NMI wakes it, finds the claim another core's and stands it
 * again, now with NMIs blocked. An NMI on a core inside its own NMI handler
 * waits until that handler's IRET (SDM Vol 3A, "Handling Multiple NMIs") and
 * stops it there; that handler prints at most a line, and its kprintf stands
 * it before then. An NMI on a core inside a #MC, #DF or #DB handler on its IST
 * stack is not held back by it: the NMI runs on its own IST stack (IST_NMI)
 * and the core stands there, the other handler's frame never returned to — a
 * #MC's with MCIP still set, which the stand clears (PanicLeaveMachineCheck).
 * And a #MC on a core standing in the NMI handler runs on IST_MACHINE_CHECK; if it
 * recovers, its IRET unblocks NMIs and returns to the `hlt` — a later NMI then
 * lands over the frame of the first, which is never returned through, and
 * stands the core again; if it does not, the dump finds the claim another
 * core's and stands. */
static void PanicHaltPeers(void)
{
    if (!lapic_is_mapped())
        return;

    uint32_t me = PanicMe() - 1u;
    for (uint32_t c = 0; c < g_amp.total_cores; c++)
    {
        if (c != me && amp_core_online(&g_amp.cores[c]))
            LapicSendNmi(g_amp.cores[c].lapic_id);
    }
}

/* FORMAT with ARGS said by SAY: the ring's half of kvprintf or the glass's
 * (klib_print.c). */
static void PanicSayf(int (*say)(const char *, va_list), const char *format, ...)
{
    va_list args;
    va_start(args, format);
    say(format, args);
    va_end(args);
}

/* The one line that ends an account a fault or a panic broke off, "Account
 * broken off by " BY and then FORMAT with ARGS, said by SAY — or, the account
 * said whole before it came, "After the account: " and the same: the lines
 * above it are all there are, and a reader told they broke off would look for
 * the rest. */
static void PanicSayBreakBy(int (*say)(const char *, va_list), const char *by,
                            const char *format, va_list args)
{
    PanicSayf(say, "\n%s%s",
              g_panic_account_said ? "After the account: " : "Account broken off by ", by);
    say(format, args);
    PanicSayf(say, "\n");
}

/* That line, said where it is read whatever else broke first. What the account
 * said goes out to the memory the next boot reads (which takes nothing) and to
 * the wire — driven by nothing while interrupts are off once it has gone over
 * to its interrupt (serial.c) — then the line into the ring, and out after it;
 * only then the glass, which draws it from the ring. The glass comes last
 * because its road — a framebuffer — is the longer one, and a fault on it,
 * which this coming may itself be, would otherwise cost the wire the line too;
 * a machine with no wire loses nothing by the order (WireDrain returns at
 * once). A fault anywhere in here is the third coming, and stands at once
 * (PanicClaim). */
static void PanicSayBreak(const char *by, const char *format, va_list args)
{
    LogKeepFlush();
    WireDrain();
    PanicSayBreakBy(KvprintfRing, by, format, args);
    LogKeepFlush();
    WireDrain();

    console_lock_acquire();
    EmergencyScreenFollow();
    console_lock_release();
}

/* The same, for the numbers an exception's line is made of. */
static void PanicSayBreakf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    PanicSayBreak("", format, args);
    va_end(args);
}

/* The claim, for either account. False to the account that is to be said —
 * its winner's first coming, or the same account further down the same page;
 * true to the owner come again, which its caller ends with the line that says
 * by what. A loser, and the owner's third coming, stand here. */
static bool PanicClaim(bool *already_claimed_here)
{
    if (*already_claimed_here)
        return false; /* same account, further down the same page */

    uint32_t me    = PanicMe();
    uint32_t owner = 0;
    if (__atomic_compare_exchange_n(&g_panic_owner, &owner, me, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        g_panic_depth         = 1;
        *already_claimed_here = true;
        PanicLeaveMachineCheck();

        /* Peers stopped by the WINNER, here, before its first line — and by
         * the winner only: a loser that sent them would stop the winner
         * mid-sentence. */
        PanicHaltPeers();

        /* And the glass taken, whoever held it: the display's core is among
         * the stopped, and the account is drawn by the kernel's own hand
         * through the kernel's own mapping, its window left as it is. */
        EmergencyScreenPanicTake();
        return false;
    }

    if (owner != me)
        PanicStand(); /* the loser: not a word */

    /* The owner again: a fault inside its own account, or a panic inside it.
     * One panic, one account: the second is not said — on a screen with no
     * wire it would scroll away the first, the one whose CR2 names the cause.
     * Nor is the first stood over in silence, its reader left to guess why it
     * stops mid-line; Linux says a second report on the CPU already in one
     * (oops_begin's nest count, panic()'s panic_cpu). Here one line says the
     * account broke off, and by what — numbers its caller already holds, or
     * the message of the panic inside it; nothing walked, named, or read
     * through a pointer of the dead account's. What was said goes out before
     * that line (PanicSayBreak), so a fault inside the line itself — the third
     * coming, which stands at once — costs the account nothing but the line.
     * Once the account is said whole (PanicEndAccount), what brings it here
     * broke nothing off — a #MC taken in its drain, or one waking the owner
     * from its stand, which a #MC does whatever the interrupt flag: the record
     * of a memory error found after the account; the same one line says it
     * came after the account, which is whole above it. */
    if (++g_panic_depth != 2)
        PanicStand();
    return true;
}

void PanicClaimForException(bool *already_claimed_here, const interrupt_frame_t *frame,
                            uint64_t cr2)
{
    if (!PanicClaim(already_claimed_here))
        return;
    if (frame->vector == 14)
        PanicSayBreakf("vector %lu, error 0x%lx, RIP 0x%lx, CR2 0x%lx",
                       frame->vector, frame->error_code, frame->rip, cr2);
    else
        PanicSayBreakf("vector %lu, error 0x%lx, RIP 0x%lx",
                       frame->vector, frame->error_code, frame->rip);
    PanicStand();
}

void PanicClaimForMessage(bool *already_claimed_here, const char *message, va_list args)
{
    if (!PanicClaim(already_claimed_here))
        return;
    PanicSayBreak("panic: ", message, args);
    PanicStand();
}

void PanicEndAccount(void)
{
    /* Said whole: its last line is said — in the ring, and on the glass, every
     * line of it drawn from the ring as it was said (klib_print.c, kvprintf).
     * Marked here, before it goes out, not after: once the wire has gone over
     * to its interrupt, what the transmitter has not taken of the account
     * waits in the ring for the drain below — a byte every 86.8 us at 115200
     * baud (serial.c, wire_program_port), a whole dump the best part of a
     * tenth of a second — and a #MC taken in that time broke nothing off
     * either: its line drains what was said before it says itself
     * (PanicSayBreak), so it still comes after the whole account. After this,
     * a second coming broke nothing. */
    g_panic_account_said = true;

    /* The account out of this core's cache and into the memory the NEXT boot
     * reads. A warm reset invalidates the caches without writing them back
     * (Intel SDM Vol 3A §11.5.1, Table 11-7), so an account that never left
     * the cache is an account nobody will ever see — and a panicked machine is
     * reset by hand. Takes no lock and waits on nothing, which is what makes
     * it callable with every peer frozen. */
    LogKeepFlush();

    /* Everything said, onto the wire: interrupts are off, so the line cannot
     * drive itself. Nothing to break first: a lock on the way that this core
     * holds itself, or a stopped core left held, the owner of the panic takes
     * as it stands (klib_lock.c). */
    WireDrain();

    /* And the whole account drawn once more: a core stopped in the middle of
     * a draw — the display's, or one still in the Emergency Screen when the
     * claim was made — may have had pixels in its write-combining buffers,
     * and the NMI that stopped it let them go onto the glass after the
     * account began. */
    EmergencyScreenRedraw();

    PanicStand();
}

void PanicStandIfClaimedElsewhere(void)
{
    uint32_t owner = __atomic_load_n(&g_panic_owner, __ATOMIC_ACQUIRE);
    if (owner != 0 && owner != PanicMe())
        PanicStand();
}

bool PanicOwnedHere(void)
{
    uint32_t owner = __atomic_load_n(&g_panic_owner, __ATOMIC_ACQUIRE);
    return owner != 0 && owner == PanicMe();
}

bool PanicClaimed(void)
{
    return __atomic_load_n(&g_panic_owner, __ATOMIC_ACQUIRE) != 0;
}
