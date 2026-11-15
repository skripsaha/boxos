#ifndef BOXOS_JOURNEY_H
#define BOXOS_JOURNEY_H

/*
 * The journey — the machine's way from power-on to a shell, in moments.
 *
 * SYSTEM_OP_JOURNEY answers it: a JourneyHead, then `count` JourneyMoments —
 * the loader's first, `from_loader` of them, as its Boarding Pass carried them
 * (boarding_pass.h, BoardingMoment), then the kernel's own, each part in the
 * order its steps were reached. A step is a word, not a number: whoever shows
 * the journey shows the words it was given, so a loader or a kernel that
 * learns to mark a new step needs nobody else taught about it.
 *
 * `tsc` is the time-stamp counter — one clock, the loader's and the kernel's
 * both, counting from power-on on a machine of its own — and `tsc_khz` turns
 * its ticks into time. The time before the first moment is the firmware's,
 * except under a hypervisor (JOURNEY_UNDER_HYPERVISOR): there the counter
 * starts wherever the host started it, and that time is nobody's.
 *
 * Includer must provide uint8_t, uint16_t, uint32_t and uint64_t BEFORE
 * including this header.
 */

#define JOURNEY_WORD             8

#define JOURNEY_UNDER_HYPERVISOR 0x01u

/* The most moments one answer carries: the thirty-two a Boarding Pass has
 * room for, and the sixteen the kernel keeps. */
#define JOURNEY_LOADER_MAX       32u
#define JOURNEY_KERNEL_MAX       16u
#define JOURNEY_MOMENTS_MAX      (JOURNEY_LOADER_MAX + JOURNEY_KERNEL_MAX)

typedef struct __attribute__((packed)) JourneyHead {
    uint32_t tsc_khz;       /* the counter's ticks in a millisecond; 0 when not measured */
    uint16_t count;         /* moments after this head */
    uint8_t  from_loader;   /* the first of them that are the loader's */
    uint8_t  flags;         /* JOURNEY_* */
} JourneyHead;

typedef struct __attribute__((packed)) JourneyMoment {
    char     what[JOURNEY_WORD];   /* NUL-padded, not necessarily NUL-terminated */
    uint64_t tsc;
} JourneyMoment;

#endif /* BOXOS_JOURNEY_H */
