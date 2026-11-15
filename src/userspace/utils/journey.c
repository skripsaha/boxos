/* journey — where the time went, from power-on to the display's glass.
 *
 * Gangway marks each step of its own part by the processor's time-stamp
 * counter and carries the moments on the Boarding Pass; the kernel marks its
 * part on the same clock (SYSTEM_OP_JOURNEY, boxos_journey.h). This draws
 * them as one line: the firmware before Gangway began, Gangway step by step,
 * the kernel step by step.
 *
 *   journey      -> the journey, a bar to a part and every step's share
 *
 * ‼ WHAT THE FIRST LINE CAN AND CANNOT SAY. The counter starts at power-on on
 * a machine of its own, so the time before Gangway's first moment is the
 * firmware's. Under a hypervisor it starts wherever the host started it, and
 * that line is said to be nobody's rather than shown as a number it is not.
 */
#include "box/print.h"
#include "box/form.h"
#include "box/system.h"
#include "box/string.h"

BOX_FORM("");

#define BAR_CELLS 30u

typedef struct {
    const char          *name;      /* the part: firmware, Gangway, kernel */
    uint64_t             from;      /* the counter where it began */
    uint64_t             to;        /* and where its last step was reached */
    const JourneyMoment *steps;     /* its moments, the first its start */
    uint16_t             count;
} JourneyPart;

/* `ticks` of the counter as microseconds. */
static uint64_t Micros(uint64_t ticks, uint32_t khz)
{
    return khz ? ticks * 1000u / khz : 0;
}

/* A span in words a person reads at a glance: "740 us", "3.4 ms", "84 ms",
 * "1.92 s". */
static void SayTime(char *out, size_t room, uint64_t us)
{
    if (us < 1000u)            snprintf(out, room, "%llu us", (unsigned long long)us);
    else if (us < 10000u)      snprintf(out, room, "%llu.%llu ms", (unsigned long long)(us / 1000u),
                                        (unsigned long long)(us % 1000u / 100u));
    else if (us < 1000000u)    snprintf(out, room, "%llu ms", (unsigned long long)(us / 1000u));
    else                       snprintf(out, room, "%llu.%02llu s", (unsigned long long)(us / 1000000u),
                                        (unsigned long long)(us % 1000000u / 10000u));
}

/* A step's word, NUL-terminated. */
static void Word(char out[JOURNEY_WORD + 1], const JourneyMoment *m)
{
    memcpy(out, m->what, JOURNEY_WORD);
    out[JOURNEY_WORD] = '\0';
}

/* One part: its name, a bar against the longest part, how long it took, and
 * each step after its first with the time it took to reach. */
static void SayPart(const JourneyPart *part, uint64_t longest, uint32_t khz)
{
    uint64_t span  = part->to - part->from;
    uint32_t cells = longest ? (uint32_t)((span * BAR_CELLS + longest - 1u) / longest) : 0;
    char     bar[BAR_CELLS * 3u + 1u];
    size_t   at = 0;
    for (uint32_t i = 0; i < BAR_CELLS; i++) {
        const char *cell = i < cells ? "\xe2\x96\x88" : " ";   /* U+2588 FULL BLOCK */
        for (const char *c = cell; *c; c++) bar[at++] = *c;
    }
    bar[at] = '\0';

    char took[24];
    SayTime(took, sizeof(took), Micros(span, khz));
    printf("  %-9s %s  %-8s", part->name, bar, took);

    for (uint16_t i = 1; i < part->count; i++) {
        char word[JOURNEY_WORD + 1], step[24];
        Word(word, &part->steps[i]);
        SayTime(step, sizeof(step), Micros(part->steps[i].tsc - part->steps[i - 1].tsc, khz));
        printf("%s%s %s", i == 1 ? "  " : " \xc2\xb7 ", word, step);   /* U+00B7 MIDDLE DOT */
    }
    printf("\n");
}

int main(void)
{
    JourneyHead   head;
    JourneyMoment moments[JOURNEY_MOMENTS_MAX];
    int rc = journey_read(&head, moments);
    if (rc < 0) {
        printf("The journey could not be read (error %d)\n", -rc);
        exit(1);
        return 1;
    }
    if (head.count == 0) {
        println("Nothing marked the journey — neither the loader nor the kernel said when a step was reached.");
        exit(0);
        return 0;
    }

    const JourneyMoment *loader = moments;
    const JourneyMoment *kernel = moments + head.from_loader;
    uint16_t             own    = (uint16_t)(head.count - head.from_loader);

    JourneyPart parts[3];
    unsigned    n = 0;
    bool        firmware_known = !(head.flags & JOURNEY_UNDER_HYPERVISOR) && head.from_loader > 0;
    if (firmware_known)
        parts[n++] = (JourneyPart){ "firmware", 0, loader[0].tsc, NULL, 0 };
    if (head.from_loader > 0)
        parts[n++] = (JourneyPart){ "Gangway", loader[0].tsc, loader[head.from_loader - 1].tsc,
                                    loader, head.from_loader };
    if (own > 0)
        parts[n++] = (JourneyPart){ "kernel", kernel[0].tsc, kernel[own - 1].tsc, kernel, own };

    uint64_t longest = 0;
    for (unsigned i = 0; i < n; i++)
        if (parts[i].to - parts[i].from > longest) longest = parts[i].to - parts[i].from;

    if (head.tsc_khz)
        printf("From power-on to the display, by the processor's clock (%u.%02u GHz):\n\n",
               head.tsc_khz / 1000000u, head.tsc_khz % 1000000u / 10000u);
    else
        println("From power-on to the display — the clock's speed was not measured, so no time can be told:\n");

    for (unsigned i = 0; i < n; i++) SayPart(&parts[i], longest, head.tsc_khz);

    if (head.from_loader == 0)
        println("  The loader marked no moments: it is older than the journey, or not Gangway.");
    if (head.flags & JOURNEY_UNDER_HYPERVISOR)
        println("  Under a hypervisor the clock does not start at power-on, so the time before "
                "the loader is nobody's and is not shown.");

    const JourneyMoment *last  = own > 0 ? &kernel[own - 1] : &loader[head.from_loader - 1];
    uint64_t             start = firmware_known ? 0 : moments[0].tsc;
    char                 total[24], word[JOURNEY_WORD + 1];
    SayTime(total, sizeof(total), Micros(last->tsc - start, head.tsc_khz));
    Word(word, last);
    printf("\n  %s from %s to %s\n", total,
           firmware_known ? "power-on" : "the loader's first moment", word);
    exit(0);
    return 0;
}
