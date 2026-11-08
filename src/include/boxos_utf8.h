#ifndef BOXOS_UTF8_H
#define BOXOS_UTF8_H

/*
 * boxos_utf8.h — where a run of UTF-8 may be cut. Shared kernel/userspace;
 * the includer supplies uint8_t and size_t.
 *
 * A character is never cut in two between pieces of text that travel apart —
 * console frames, runs said into the log ring, reads of the ring. Half a
 * character on either side of somebody else's text is two pieces of rubbish
 * wherever it is drawn and in every log that keeps it. The length is in the
 * lead byte, so the cut moves back to before it.
 */

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
