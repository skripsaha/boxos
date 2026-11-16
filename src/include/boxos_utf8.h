#ifndef BOXOS_UTF8_H
#define BOXOS_UTF8_H

/*
 * boxos_utf8.h — UTF-8 read into characters, and where a run of it may be cut.
 * Shared by the kernel, the loader and userspace; the includer supplies
 * uint8_t, uint32_t and size_t.
 *
 * A character is never cut in two between pieces of text that travel apart —
 * console frames, runs said into the log ring, reads of the ring. Half a
 * character on either side of somebody else's text is two pieces of rubbish
 * wherever it is drawn and in every log that keeps it. The length is in the
 * lead byte, so the cut moves back to before it.
 */

/* The bytes of the character Lead begins, Lead included; 0 when it begins
 * none — a continuation byte, 0xC0 and 0xC1 (only ever an overlong form),
 * 0xF5 and up (past U+10FFFF). */
static inline size_t BoxUtf8Promise(uint8_t Lead)
{
    if (Lead < 0x80u) return 1;
    if (Lead >= 0xC2u && Lead <= 0xDFu) return 2;
    if (Lead >= 0xE0u && Lead <= 0xEFu) return 3;
    if (Lead >= 0xF0u && Lead <= 0xF4u) return 4;
    return 0;
}

/* The character Bytes[0..N) begins with, N > 0, in *Character, and how many
 * bytes it takes. What is no character — a byte that begins none, a sequence
 * cut short or broken, an overlong form, a surrogate — is U+FFFD and takes its
 * first byte: the next is read from the byte after it. */
static inline size_t BoxUtf8Next(const char *Bytes, size_t N, uint32_t *Character)
{
    const uint8_t Lead = (uint8_t)Bytes[0];
    const size_t  Need = BoxUtf8Promise(Lead);
    *Character = 0xFFFDu;
    if (Need == 0 || Need > N) return 1;
    if (Need == 1) {
        *Character = Lead;
        return 1;
    }

    uint32_t Value = Lead & (0x7Fu >> Need);
    for (size_t At = 1; At < Need; At++) {
        const uint8_t Next = (uint8_t)Bytes[At];
        if ((Next & 0xC0u) != 0x80u) return 1;
        Value = (Value << 6) | (Next & 0x3Fu);
    }
    const uint32_t Least = Need == 2 ? 0x80u : Need == 3 ? 0x800u : 0x10000u;
    if (Value < Least || Value > 0x10FFFFu || (Value >= 0xD800u && Value <= 0xDFFFu)) return 1;
    *Character = Value;
    return Need;
}

/* How much of bytes[0..n) ends on a whole character: a sequence the end would
 * split is left for the next piece. Bytes that are no UTF-8 at all go as they
 * are — the hand that draws shows them as raw cells. 0 only when all n bytes
 * are the beginning of one unfinished character. */
static inline size_t BoxUtf8Whole(const char *bytes, size_t n)
{
    size_t back = 0;
    while (back < 3u && back < n && ((uint8_t)bytes[n - 1u - back] & 0xC0u) == 0x80u)
        back++;
    if (back == n) return n;

    uint8_t lead = (uint8_t)bytes[n - 1u - back];
    if (lead >= 0xC2u && lead <= 0xF4u) {
        size_t need = lead >= 0xF0u ? 4u : lead >= 0xE0u ? 3u : 2u;
        if (back + 1u < need) return n - 1u - back;
    }
    return n;
}

#endif /* BOXOS_UTF8_H */
