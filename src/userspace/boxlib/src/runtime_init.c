/*
 * runtime_init.c — ELF .init_array / .fini_array execution for every BoxOS
 * userspace binary (C and C++ alike).
 *
 * The bounds come from linker symbols in user.ld. Pure-C binaries carry
 * empty arrays (start == end) and both walkers fall straight through, so
 * this costs nothing for the existing fleet.
 *
 * Itanium C++ ABI ordering at exit: __cxa_finalize callbacks (std::atexit
 * + static destructors registered via __cxa_atexit) run BEFORE the
 * .fini_array entries, and .fini_array is walked in reverse.
 */

#include "box/defs.h"

typedef void (*RuntimeHook)(void);

/* Defined by whichever links the program — user.ld, or BoxASM's linker — and
 * so inside it: hidden, they are reached relative to the code, as a program
 * placed at any base reaches them, and never through a table of addresses. */
extern RuntimeHook __init_array_start[] __attribute__((visibility("hidden")));
extern RuntimeHook __init_array_end[] __attribute__((visibility("hidden")));
extern RuntimeHook __fini_array_start[] __attribute__((visibility("hidden")));
extern RuntimeHook __fini_array_end[] __attribute__((visibility("hidden")));

/* What a runtime in the program runs at exit before the .fini_array entries:
 * C++'s __cxa_finalize, handed over by boxcxx when something is first
 * registered for the end (__box_runtime_finalize_with). A program in which
 * nothing was has nothing to finalize. */
static void (*g_finalize)(void *);

void __box_runtime_init(void);
void __box_runtime_fini(void);
void __box_runtime_finalize_with(void (*finalize)(void *));

void __box_runtime_finalize_with(void (*finalize)(void *))
{
    g_finalize = finalize;
}

void __box_runtime_init(void)
{
    for (RuntimeHook *fn = __init_array_start; fn != __init_array_end; ++fn)
        (*fn)();
}

void __box_runtime_fini(void)
{
    /* exit() can be re-entered (a destructor calling exit()); run the
     * teardown exactly once. Single thread per cabin — no atomics needed. */
    static uint8_t done = 0;
    if (done) return;
    done = 1;

    if (g_finalize) g_finalize(NULL);

    for (RuntimeHook *fn = __fini_array_end; fn != __fini_array_start; )
    {
        --fn;
        (*fn)();
    }
}
