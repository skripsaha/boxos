/*
 * clipboard.c — the Clipboard, spoken to the kernel through the System Deck.
 *
 * Five ops, their crates told in boxos_decks.h: system.clipboard.put takes
 * the bytes, list answers a ClipboardPage and the records after it, take the
 * bytes asked for, drop and clear say what went. A clip is as long as the
 * program made it, so every op is built here with crates of its own length —
 * not MfCall1's, whose 32-bit crates would cut a long clip short.
 */

#include "box/clipboard.h"
#include "box/core/manifest.h"
#include "box/core/crate.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/error.h"
#include "boxos_decks.h"    /* SYSTEM_OP_CLIPBOARD_* — single source */

/* The longest params a clipboard op carries: take's number and offset. */
#define CLIPBOARD_PARAMS_MOST 16u

/* One clipboard op, waited for; *answered (optional) how many bytes the out
 * crate came back holding. Returns 0 or a negative -error_t. */
static int Ask(uint16_t opcode, const void *params, uint16_t param_size, const void *in, uint64_t in_size,
               void *out, uint64_t out_capacity, uint64_t *answered)
{
    uint8_t mbuf[sizeof(Manifest) + sizeof(ManifestOp) + CLIPBOARD_PARAMS_MOST] __attribute__((aligned(8)));

    ManifestBuilder mb;
    if (ManifestBuilderInit(&mb, mbuf, sizeof(mbuf)) != 0) return -ERR_INVALID_ARGUMENT;

    Crate    crates[2];
    uint16_t count   = 0;
    uint16_t in_at   = CRATE_INDEX_NONE;
    uint16_t out_at  = CRATE_INDEX_NONE;
    if (in) {
        CrateSetInput(&crates[count], (void *)in, in_size);
        in_at = count++;
    }
    if (out) {
        CrateSetOutput(&crates[count], out, out_capacity);
        out_at = count++;
    }

    if (ManifestBuilderAddOp(&mb, DECK_SYSTEM, opcode, 0, in_at, out_at, params, param_size) != 0 ||
        ManifestBuilderFinalize(&mb) != 0)
        return -ERR_INVALID_ARGUMENT;

    int rc = ManifestSubmit((Manifest *)mbuf, crates, count, NULL);
    if (rc == 0 && answered) *answered = out ? crates[out_at].size : 0;
    return box_fail(rc);
}

int64_t clipboard_put(const void *bytes, size_t size, const ClipTerms *terms)
{
    if (!bytes || size == 0) return -ERR_INVALID_ARGUMENT;

    uint32_t params[2] = { 0, 0 };
    if (terms) {
        params[0] = terms->Takes;
        params[1] = terms->WhileAlive ? CLIPBOARD_WHILE_ALIVE : 0;
    }

    uint64_t number   = 0;
    uint64_t answered = 0;
    int      rc       = Ask(SYSTEM_OP_CLIPBOARD_PUT, params, sizeof(params), bytes, size, &number, sizeof(number),
                            &answered);
    if (rc < 0) return rc;
    if (answered < sizeof(number)) return -ERR_INTERNAL;
    return (int64_t)number;
}

/* The kernel answers the page's head and its records in one crate; they are
 * set apart here, into the caller's page and array. */
int clipboard_list(uint64_t from, ClipboardRecord *out, size_t room, ClipboardPage *page)
{
    if (!page || (room > 0 && !out)) return -ERR_INVALID_ARGUMENT;

    const size_t capacity = sizeof(ClipboardPage) + room * sizeof(ClipboardRecord);
    uint8_t     *answer   = malloc(capacity);
    if (!answer) return -ERR_NO_MEMORY;

    uint64_t answered = 0;
    int      rc       = Ask(SYSTEM_OP_CLIPBOARD_LIST, &from, sizeof(from), NULL, 0, answer, capacity, &answered);
    if (rc == 0) {
        memcpy(page, answer, sizeof(*page));
        if (page->delivered > room ||
            answered != sizeof(ClipboardPage) + (uint64_t)page->delivered * sizeof(ClipboardRecord)) {
            rc = -ERR_INTERNAL;
        } else {
            memcpy(out, answer + sizeof(ClipboardPage), (size_t)page->delivered * sizeof(ClipboardRecord));
        }
    }
    free(answer);
    return rc;
}

int64_t clipboard_take(uint64_t number, uint64_t offset, void *buf, size_t room)
{
    if (!buf || room == 0) return -ERR_INVALID_ARGUMENT;

    const uint64_t params[2] = { number, offset };
    uint64_t       answered  = 0;
    int            rc        = Ask(SYSTEM_OP_CLIPBOARD_TAKE, params, sizeof(params), NULL, 0, buf, room, &answered);
    return rc < 0 ? rc : (int64_t)answered;
}

int clipboard_drop(uint64_t number)
{
    return Ask(SYSTEM_OP_CLIPBOARD_DROP, &number, sizeof(number), NULL, 0, NULL, 0, NULL);
}

int64_t clipboard_clear(void)
{
    uint64_t went     = 0;
    uint64_t answered = 0;
    int      rc       = Ask(SYSTEM_OP_CLIPBOARD_CLEAR, NULL, 0, NULL, 0, &went, sizeof(went), &answered);
    if (rc < 0) return rc;
    if (answered < sizeof(went)) return -ERR_INTERNAL;
    return (int64_t)went;
}
