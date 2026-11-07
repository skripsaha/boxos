/* panic_stop_proof.c — `make PANICSTOP=panic|fault|nested`: a K-Core dies
 * while the BSP runs with its interrupts off and the console in its hand.
 *
 * The case is the one the startup tests put by accident: they run on the BSP
 * before the sti that opens it to interrupts (main.c), and a K-Core that
 * panicked beside them stopped every core that could take an interrupt and
 * not the one that could not — the BSP went on, and the account of the panic
 * waited behind it. Here it is put on purpose (logcheck panicstop):
 *
 *   - the BSP says it takes the console and takes the console lock — what a
 *     kprintf stopped in the middle of a line leaves behind, the lock never
 *     given back — and from then on reads the clock in a loop with interrupts off, for
 *     ever, saying nothing more; so the only way to see it run is to look at
 *     it, which is what QEMU's monitor does; a core that says nothing is the
 *     case, because a kprintf begun after a panic is claimed stands its core
 *     by itself;
 *   - the first K-Core beside it is handed a one-way pass (baton.h: work that
 *     does not come back) that waits until the BSP holds the console, and then
 *     calls panic() (PANICSTOP=panic), executes ud2 (PANICSTOP=fault), or
 *     calls panic() with a string that lies at a non-canonical address
 *     (PANICSTOP=nested) — a panic whose own message is the next bug — so
 *     reading it raises #GP on the core that holds the claim, inside the first
 *     line of its account, after the words before it are said.
 *
 * A machine that stops at its panic has every core standing with interrupts
 * off, the BSP among them. The console the BSP holds is the lock a core
 * stopped where it stands never gives back: the account has to be said, and
 * seen on the glass the panic takes, with it left as it is. Never in a
 * shipped build. */
#include "klib.h"
#include "amp.h"
#include "baton.h"
#include "serial.h"

#ifdef CONFIG_PANIC_STOP_PROOF

#if CONFIG_PANIC_STOP_PROOF == 3
/* Bits 63..47 not all equal: no paging mode makes this an address, so the
 * read of it is a #GP whatever the page tables hold (Intel SDM Vol 1,
 * "Canonical Addressing"). */
#define PANIC_STOP_PROOF_WILD ((const char *)0x8000000000000000ull)
#endif

static __attribute__((noreturn)) void PanicStopProofStand(const char *why)
{
    kprintf("[PANICSTOP] %s — the case is not put\n", why);
    WireDrain();
    for (;;)
        __asm__ volatile("cli; hlt");
}

/* Raised by the BSP once it holds the console; the K-Core dies on it. */
static volatile uint32_t s_console_held;

/* The K-Core is past its pass's own line and in the case. */
static volatile uint32_t s_die_entered;

static void PanicStopProofDie(void *ctx)
{
    (void)ctx;
    __atomic_store_n(&s_die_entered, 1u, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&s_console_held, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
#if CONFIG_PANIC_STOP_PROOF == 2
    __builtin_trap();
#elif CONFIG_PANIC_STOP_PROOF == 3
    panic("core %u faults inside its own account: %s",
          (unsigned)amp_get_core_index(), PANIC_STOP_PROOF_WILD);
#else
    panic("core %u dies while the BSP holds the console with interrupts off",
          (unsigned)amp_get_core_index());
#endif
}

/* The first online K-Core that is not the BSP, or MAX_CORES. */
static uint32_t PanicStopProofCore(void)
{
    for (uint32_t c = 0; c < g_amp.total_cores; c++) {
        const CoreDescriptor *d = &g_amp.cores[c];
        if (d->is_kcore && !d->is_bsp && amp_core_online(d))
            return c;
    }
    return MAX_CORES;
}

__attribute__((noreturn)) void PanicStopProof(void)
{
    uint32_t core = PanicStopProofCore();
    if (core == MAX_CORES)
        PanicStopProofStand("no K-Core beside the BSP: boot with CORES=4 or more");
    if (!BatonPassOneWay((uint8_t)core, PanicStopProofDie, NULL))
        PanicStopProofStand("the one-way pass to the K-Core was refused");
    kprintf("[PANICSTOP] core %u is to %s once the BSP holds the console\n", core,
            CONFIG_PANIC_STOP_PROOF == 2 ? "execute ud2" :
            CONFIG_PANIC_STOP_PROOF == 3 ? "fault inside its own account" : "call panic()");
    kprintf("[PANICSTOP] the BSP takes the console and runs on with interrupts off\n");
    WireDrain();

    /* ‼ THE PASS SAYS ITS OWN LINE ON ITS WAY IN, AND THAT LINE TAKES THE
     * CONSOLE. A one-way pass prints "[BATON] core N takes a one-way pass"
     * before it runs PanicStopProofDie (baton.c). Take the console first and
     * the K-Core stands at that line, on a lock held for good, with no panic
     * claimed to let it through — and the case is never put: SEEN once in ten
     * legs, the BATON line missing and sixty seconds of silence. So the
     * console is taken only once the K-Core is inside the case. */
    while (!__atomic_load_n(&s_die_entered, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    console_lock_acquire();
    __atomic_store_n(&s_console_held, 1u, __ATOMIC_RELEASE);
    for (;;) {
        /* The clock read into registers: a core QEMU looks at twice and finds
         * them changed ran in between the looks. */
        __asm__ volatile("rdtsc; pause" ::: "rax", "rdx");
    }
}

#endif
