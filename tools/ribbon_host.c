/*
 * ribbon_host — where a book's writer goes on past the end of its ribbon, and
 * how long each stitch it lays is, held on the host with the kernel's own
 * rules in volume_ribbon.h (`make ribbon-host`): seconds, no machine.
 *
 * spec_v2 §1.5, §16.2 п.11, §16.6 п.2. A writer never writes an atom of the
 * medium a settled sector lies in: past an end off the medium's atom it goes
 * on only at the start of a next_quire laid on that atom, and nowhere — the
 * volume under glass — with none named; never in an atom smaller than the
 * medium's, whatever atom laid the ribbon before it. On a removable medium,
 * whose ribbon is written in flash pages, every stitch ends on a page counted
 * from the volume's first sector: the stitch after it begins on one and shares
 * it with no settled stitch. Only the first stitch past an end inside a page
 * does — where no next_quire laid on a page is named to go on in instead —
 * and a stitch the rest of a Quire a writer of a smaller atom laid has no
 * page left for, with no next_quire named.
 */

#include <stdint.h>
#include <stdio.h>

#include "volume_ribbon.h"

#define M_FIXED  8u     /* 4 KiB, a medium's atom, in sectors */
#define M_COARSE 16u    /* 8 KiB, a coarser medium's */
#define PAGE     32u    /* 16 KiB, a flash page */

static int s_failed;

static void Say(int ok, const char* what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    s_failed += !ok;
}

/* Where the writer goes on: an end at sector `off` of Quire 128+256, the
 * next_quire `nb`+`nn`. */
static uint32_t On(uint64_t off, uint64_t nb, uint64_t nn, uint32_t m, uint32_t r)
{
    return RibbonGoesOn(128u, 256u, off, nb, nn, m, r);
}

static void GoesOnTests(void)
{
    const uint32_t end = RIBBON_ON_END, next = RIBBON_ON_NEXT, shared = RIBBON_ON_SHARED;
    printf("ribbon_host: where a writer goes on past the end of its ribbon\n");
    Say(On(32u, 512u, 256u, M_FIXED, PAGE) == end, "an end on a page, in a Quire laid on pages: at the end");
    Say(On(40u, 512u, 256u, M_FIXED, PAGE) == next,
        "an end inside a page, a next_quire laid on pages named: at its start, sharing no page (rule (б))");
    Say(On(40u, 513u, 256u, M_FIXED, PAGE) == (end | shared),
        "an end inside a page, the next_quire named begun off one: at the end, the first stitch sharing that page");
    Say(On(40u, 0u, 0u, M_FIXED, PAGE) == (end | shared),
        "an end inside a page, no next_quire named: at the end, the first stitch sharing that page");
    Say(RibbonGoesOn(129u, 256u, 24u, 0u, 0u, M_FIXED, PAGE) == end,
        "a Quire begun off a page whose end lies on one: at the end, sharing nothing");
    Say(On(2048u, 512u, 256u, M_FIXED, PAGE) == next, "a Quire filled: at the next_quire's start");
    Say(On(2048u, 513u, 256u, M_FIXED, PAGE) == (next | shared),
        "a Quire filled, its next_quire begun off a page: at its start, the stitch there sharing a page with what "
        "lies before it");
    Say(On(40u, 512u, 256u, M_COARSE, M_COARSE) == next,
        "a medium of 8 KiB, an end off its atom: at the start of a next_quire laid on it (§1.5)");
    Say(On(40u, 513u, 256u, M_COARSE, M_COARSE) == 0,
        "a medium of 8 KiB, an end off its atom, the next_quire named off it too: nowhere — no atom smaller than "
        "the medium's is written (§1.5)");
    Say(On(40u, 0u, 0u, M_COARSE, M_COARSE) == 0, "a medium of 8 KiB, an end off its atom, none named: nowhere");
    Say(On(2048u, 513u, 256u, M_COARSE, M_COARSE) == 0,
        "a medium of 8 KiB, its Quire filled, the next_quire off its atom: nowhere");
    Say(On(40u, 514u, 256u, M_COARSE, PAGE) == (next | shared),
        "a removable medium of 8 KiB, an end off its atom, a next_quire on its atom but off a page: at its start, "
        "shared");
    Say(On(48u, 0u, 0u, M_COARSE, PAGE) == (end | shared),
        "a removable medium of 8 KiB, an end on its atom inside a page: at the end, shared — never below 8 KiB");
}

static void EndsTests(void)
{
    printf("ribbon_host: where a stitch ends\n");
    Say(RibbonStitchEnds(8u, 1024u, 2048u, 4096u, PAGE) == 32u, "begun on a page: whole pages");
    Say(RibbonStitchEnds(40u, 1024u, 2048u, 4096u, PAGE) == 64u, "longer than a page: whole pages");
    Say(RibbonStitchEnds(8u, 1032u, 2040u, 4096u, PAGE) == 24u,
        "begun inside a page: on to its end, the next begun on a page");
    Say(RibbonStitchEnds(32u, 1032u, 2040u, 4096u, PAGE) == 56u, "begun inside a page and longer: on a page");
    Say(RibbonStitchEnds(16u, 1064u, 32u, 4096u, PAGE) == 24u,
        "in a Quire ending inside a page: on a page while one lies inside it");
    Say(RibbonStitchEnds(8u, 1088u, 8u, 4096u, PAGE) == 8u,
        "the page past that Quire's end: the stitch ends with the Quire, the next at the next_quire's start");
    Say(RibbonStitchEnds(16u, 1088u, 8u, 4096u, PAGE) == 40u,
        "run on into the next_quire: its tail whole pages of it");
    Say(RibbonStitchEnds(16u, 1088u, 8u, 4104u, PAGE) == 32u,
        "run on into a next_quire begun off a page: its tail ends on a page");
    Say(RibbonStitchEnds(8u, 1024u, 72u, 0u, PAGE) == 32u,
        "no next_quire named, a page and an atom past it left: whole pages");
    Say(RibbonStitchEnds(40u, 1024u, 72u, 0u, PAGE) == 40u,
        "no next_quire named, the atom kept past it not left on a page: on the medium's atom alone (§3.6)");
    Say(RibbonStitchEnds(16u, 1032u, 2040u, 4096u, M_FIXED) == 16u, "a fixed medium: the medium's atom");
}

int main(void)
{
    GoesOnTests();
    EndsTests();
    printf("ribbon_host: %s\n", s_failed ? "FAIL" : "ok");
    return s_failed ? 1 : 0;
}
