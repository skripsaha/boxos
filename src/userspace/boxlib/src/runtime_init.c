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

extern RuntimeHook __init_array_start[];
extern RuntimeHook __init_array_end[];
extern RuntimeHook __fini_array_start[];
extern RuntimeHook __fini_array_end[];

/* Provided by boxcxx when the binary contains C++; resolves to NULL in
 * pure-C binaries (weak reference against a static link). */
void __cxa_finalize(void *dso) __attribute__((weak));

void __box_runtime_init(void);
void __box_runtime_fini(void);

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

    if (__cxa_finalize) __cxa_finalize(NULL);

    for (RuntimeHook *fn = __fini_array_end; fn != __fini_array_start; )
    {
        --fn;
        (*fn)();
    }
}
