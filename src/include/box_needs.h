#ifndef BOX_NEEDS_H
#define BOX_NEEDS_H

/*
 * The words of a box's `needs` part (box_format.h): what a program needs of
 * the processor, said the way compilers say it (-mavx2 is "avx2").
 *
 *     needs
 *     cpu avx2
 *     cpu bmi2
 *
 * Each word is a feature CPUID reports, and, for one whose registers the
 * system must save and restore across a switch, the XCR0 state the system has
 * to have enabled — AVX on a processor whose system never turned the YMM
 * state on is an invalid-opcode fault, not a slower program. A machine that
 * lacks a word refuses the program before it starts, with the word ("edit
 * needs avx2; this machine has none"); a word not in this list is a word this
 * system cannot check, and is refused as such.
 *
 * Integer types come from the includer (see box_format.h).
 */

#define BOX_NEEDS_EAX  0u
#define BOX_NEEDS_EBX  1u
#define BOX_NEEDS_ECX  2u
#define BOX_NEEDS_EDX  3u

/* XCR0 components (Intel SDM Vol. 1 §13.1). The XMM state needs none: every
 * x86-64 system saves it (SSE2 is the architecture's floor, kept by FXSAVE
 * where there is no XSAVE). */
#define BOX_NEEDS_STATE_AVX     0x06ull   /* XMM and the upper halves of YMM */
#define BOX_NEEDS_STATE_AVX512  0xE6ull   /* that, the opmask registers and all of ZMM */

typedef struct {
    const char *Word;
    uint32_t    Leaf;
    uint32_t    Subleaf;
    uint8_t     Register;
    uint8_t     Bit;
    uint64_t    State;     /* XCR0 bits the system must have enabled; 0 for none */
} BoxNeedsCpu;

static const BoxNeedsCpu BoxNeedsCpuWords[] = {
    { "sse3",      0x1u,        0, BOX_NEEDS_ECX, 0,  0 },
    { "pclmul",    0x1u,        0, BOX_NEEDS_ECX, 1,  0 },
    { "ssse3",     0x1u,        0, BOX_NEEDS_ECX, 9,  0 },
    { "fma",       0x1u,        0, BOX_NEEDS_ECX, 12, BOX_NEEDS_STATE_AVX },
    { "cx16",      0x1u,        0, BOX_NEEDS_ECX, 13, 0 },
    { "sse4.1",    0x1u,        0, BOX_NEEDS_ECX, 19, 0 },
    { "sse4.2",    0x1u,        0, BOX_NEEDS_ECX, 20, 0 },
    { "movbe",     0x1u,        0, BOX_NEEDS_ECX, 22, 0 },
    { "popcnt",    0x1u,        0, BOX_NEEDS_ECX, 23, 0 },
    { "aes",       0x1u,        0, BOX_NEEDS_ECX, 25, 0 },
    { "avx",       0x1u,        0, BOX_NEEDS_ECX, 28, BOX_NEEDS_STATE_AVX },
    { "f16c",      0x1u,        0, BOX_NEEDS_ECX, 29, BOX_NEEDS_STATE_AVX },
    { "rdrand",    0x1u,        0, BOX_NEEDS_ECX, 30, 0 },
    { "bmi1",      0x7u,        0, BOX_NEEDS_EBX, 3,  0 },
    { "avx2",      0x7u,        0, BOX_NEEDS_EBX, 5,  BOX_NEEDS_STATE_AVX },
    { "bmi2",      0x7u,        0, BOX_NEEDS_EBX, 8,  0 },
    { "avx512f",   0x7u,        0, BOX_NEEDS_EBX, 16, BOX_NEEDS_STATE_AVX512 },
    { "avx512dq",  0x7u,        0, BOX_NEEDS_EBX, 17, BOX_NEEDS_STATE_AVX512 },
    { "rdseed",    0x7u,        0, BOX_NEEDS_EBX, 18, 0 },
    { "adx",       0x7u,        0, BOX_NEEDS_EBX, 19, 0 },
    { "avx512cd",  0x7u,        0, BOX_NEEDS_EBX, 28, BOX_NEEDS_STATE_AVX512 },
    { "sha",       0x7u,        0, BOX_NEEDS_EBX, 29, 0 },
    { "avx512bw",  0x7u,        0, BOX_NEEDS_EBX, 30, BOX_NEEDS_STATE_AVX512 },
    { "avx512vl",  0x7u,        0, BOX_NEEDS_EBX, 31, BOX_NEEDS_STATE_AVX512 },
    { "lzcnt",     0x80000001u, 0, BOX_NEEDS_ECX, 5,  0 },
};

#define BOX_NEEDS_CPU_COUNT (sizeof(BoxNeedsCpuWords) / sizeof(BoxNeedsCpuWords[0]))

/* The feature a `cpu` word names, or NULL for a word this list does not have. */
static inline const BoxNeedsCpu *BoxNeedsCpuFind(const uint8_t *word, uint64_t length)
{
    for (unsigned i = 0; i < BOX_NEEDS_CPU_COUNT; i++) {
        const char *known = BoxNeedsCpuWords[i].Word;
        uint64_t    at    = 0;
        while (at < length && known[at] != '\0' && (uint8_t)known[at] == word[at]) at++;
        if (at == length && known[at] == '\0') return &BoxNeedsCpuWords[i];
    }
    return 0;
}

#endif /* BOX_NEEDS_H */
