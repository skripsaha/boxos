#include "buildplate.h"
#include "klib.h"

/*
 * The plate itself.
 *
 * `const` so it lands in read-only data and rides inside the image; `volatile`
 * because the bytes are patched by tools/buildplate AFTER this translation
 * unit is compiled — without it the compiler is entitled to read the
 * initializers below at build time and fold every test on them, and the kernel
 * would then report "unstamped" from an image that carries a perfectly good
 * plate. `used` so nothing drops it for being written by nobody.
 *
 * The Tag is a char array and not a string literal on purpose: a literal could
 * be merged with an identical one elsewhere in the image, and the tool's
 * guarantee is that the tag appears EXACTLY ONCE.
 */
__attribute__((used, aligned(16)))
static const volatile BuildPlate g_buildplate = {
    .Tag             = BUILDPLATE_TAG_INIT,
    .Magic           = BUILDPLATE_MAGIC,
    .Verdict         = BUILDPLATE_VERDICT_UNSTAMPED,
    .TreeFingerprint = 0,
    .StampedUnix     = 0,
    .FileCount       = 0,
    .Keys            = { 'n','o','t',' ','s','t','a','m','p','e','d', 0 },
};

const BuildPlate *BuildPlateRead(void)
{
    return (const BuildPlate *)&g_buildplate;
}

bool BuildPlateUnderTest(void)
{
    const BuildPlate *p = BuildPlateRead();
    return p->Magic == BUILDPLATE_MAGIC &&
           p->Verdict == BUILDPLATE_VERDICT_UNDER_TEST;
}

/* The keys are copied out before printing rather than passed straight to
 * kprintf: the plate is volatile, %s would read it through a non-volatile
 * pointer, and a plate that somehow lost its terminator would walk off the end
 * of the structure. Bounded here, once, where the bound is visible. */
static void plate_keys(char *out, uint32_t cap)
{
    const BuildPlate *p = BuildPlateRead();
    uint32_t i = 0;
    for (; i + 1 < cap && i < BUILDPLATE_KEYS_MAX; i++) {
        char c = p->Keys[i];
        if (c == 0)
            break;
        out[i] = c;
    }
    out[i] = 0;
}

void BuildPlateSay(void)
{
    const BuildPlate *p = BuildPlateRead();

    if (p->Magic != BUILDPLATE_MAGIC) {
        /* Not a plate at all. Said rather than skipped: an image whose plate
         * cannot be read is an image nothing can be concluded from, and that
         * is worth a line of its own. */
        kprintf("[BuildPlate] this image carries no readable plate (magic "
                "0x%x) — nothing here can say which tree it was made from\n",
                p->Magic);
        return;
    }

    char keys[BUILDPLATE_KEYS_MAX];
    plate_keys(keys, sizeof(keys));

    switch (p->Verdict) {
    case BUILDPLATE_VERDICT_CLEAN:
        kprintf("[BuildPlate] tree %016llx, %u file(s), keys: %s\n",
                (unsigned long long)p->TreeFingerprint, p->FileCount, keys);
        break;

    case BUILDPLATE_VERDICT_UNDER_TEST:
        /* ‼ The line the Zenbook run needed and did not have. Two lines here
         * and not one, because this is the single fact that decides whether
         * anything else in the log is evidence about this kernel. */
        kprintf("[BuildPlate] ‼ UNDER TEST — this image was packed while the "
                "test harness had the tree mutated\n");
        kprintf("[BuildPlate] tree %016llx, %u file(s), keys: %s — what this "
                "machine prints below describes the harness as much as the "
                "kernel\n",
                (unsigned long long)p->TreeFingerprint, p->FileCount, keys);
        break;

    default:
        kprintf("[BuildPlate] unstamped: this kernel was linked without a "
                "plate being riveted, so no run from it can be tied to a "
                "tree\n");
        break;
    }
}
