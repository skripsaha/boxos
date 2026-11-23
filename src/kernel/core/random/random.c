#include "random.h"
#include "cpuid.h"
#include "crypto.h"
#include "atomics.h"
#include "klib.h"

/* Intel's guide to its generator: ten tries before calling an underflow a
 * failure — the generator refills in far less than ten instructions. */
#define RANDOM_HARDWARE_TRIES  10u
/* What a generator that lost its state answers, flag raised, every time
 * (AMD family 15h/16h after a resume, early Zen 2 microcode). Drawn twice in
 * a row it is no longer believed: by chance that is one in 2^128. */
#define RANDOM_HARDWARE_LOST   (~(uint64_t)0)
/* The TSC deltas of a short wait each, the seed's raw stuff. */
#define RANDOM_SEED_SAMPLES    64u

static volatile uint32_t g_hardware;       /* 1 while RDRAND is believed */
static uint8_t           g_seed[32];
static volatile uint64_t g_count;

static bool RandomHardwareOnce(uint64_t* out)
{
    for (uint32_t i = 0; i < RANDOM_HARDWARE_TRIES; i++) {
        uint8_t ok;
        __asm__ __volatile__("rdrand %0\n\tsetc %1" : "=r"(*out), "=qm"(ok) : : "cc");
        if (ok)
            return true;
    }
    return false;
}

/* The processor's number, or false — and then never asked again, said. */
static bool RandomHardware(uint64_t* out)
{
    if (!__atomic_load_n(&g_hardware, __ATOMIC_ACQUIRE))
        return false;
    uint64_t v = 0;
    bool     ok = RandomHardwareOnce(&v);
    if (ok && v == RANDOM_HARDWARE_LOST)
        ok = RandomHardwareOnce(&v) && v != RANDOM_HARDWARE_LOST;
    if (ok) {
        *out = v;
        return true;
    }
    if (__atomic_exchange_n(&g_hardware, 0u, __ATOMIC_ACQ_REL))
        kprintf("[Random] the processor's generator stopped answering — the kernel's own answers from now on\n");
    return false;
}

static uint64_t RandomOwn(void)
{
    struct {
        uint8_t  seed[32];
        uint64_t count;
        uint64_t tsc;
    } in;
    uint8_t  hash[32];
    uint64_t v;
    memcpy(in.seed, g_seed, sizeof(in.seed));
    in.count = atomic_fetch_add_u64(&g_count, 1);
    in.tsc   = rdtsc();
    KSha256((const uint8_t*)&in, (uint32_t)sizeof(in), hash);
    memcpy(&v, hash, sizeof(v));
    return v;
}

void RandomInit(void)
{
    KSha256Ctx ctx;
    uint64_t   v = 0;
    KSha256Init(&ctx);
    for (uint32_t i = 0; i < RANDOM_SEED_SAMPLES; i++) {
        uint64_t before = rdtsc();
        for (uint32_t spin = 0; spin < 64u + (uint32_t)(before & 63u); spin++)
            cpu_pause();
        uint64_t delta = rdtsc() - before;
        KSha256Update(&ctx, (const uint8_t*)&before, (uint32_t)sizeof(before));
        KSha256Update(&ctx, (const uint8_t*)&delta, (uint32_t)sizeof(delta));
    }

    __atomic_store_n(&g_hardware, g_cpu_caps.has_rdrand ? 1u : 0u, __ATOMIC_RELEASE);
    bool hardware = RandomHardware(&v);
    if (hardware)
        KSha256Update(&ctx, (const uint8_t*)&v, (uint32_t)sizeof(v));
    KSha256Final(&ctx, g_seed);
    __atomic_store_n(&g_count, rdtsc(), __ATOMIC_RELEASE);

    kprintf("[Random] %s\n", hardware ? "the processor's generator (RDRAND) answers"
                             : g_cpu_caps.has_rdrand ? "the processor's generator (RDRAND) gives no number it can be "
                                                      "believed for — the kernel's own answers, seeded by the boot's timing"
                                                    : "the processor has no generator — the kernel's own answers, seeded "
                                                      "by the boot's timing");
}

uint64_t Random64(void)
{
    uint64_t v;
    return RandomHardware(&v) ? v : RandomOwn();
}

void RandomBytes(void* out, size_t bytes)
{
    uint8_t* at = (uint8_t*)out;
    while (bytes) {
        uint64_t v    = Random64();
        size_t   take = bytes < sizeof(v) ? bytes : sizeof(v);
        memcpy(at, &v, take);
        at    += take;
        bytes -= take;
    }
}

/* Rejection: a draw at or past the largest multiple of `bound` 64 bits hold
 * is drawn again, so no residue is likelier than another. At most one draw
 * in two is given back, for a bound just past 2^63; for the bounds asked of
 * it, almost never. */
uint64_t RandomBelow(uint64_t bound)
{
    if (bound == 0)
        return 0;
    uint64_t limit = (~(uint64_t)0 / bound) * bound;
    uint64_t v;
    do {
        v = Random64();
    } while (v >= limit);
    return v % bound;
}
