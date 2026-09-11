
#include "box/defs.h"

typedef void (*RuntimeHook)(void);

extern RuntimeHook __init_array_start[];
extern RuntimeHook __init_array_end[];
extern RuntimeHook __fini_array_start[];
extern RuntimeHook __fini_array_end[];

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