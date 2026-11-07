/* screen.c — the screen and the log ring, from userspace. See box/screen.h.
 * The opcodes and the read's layout are boxos_decks.h's. */

#include "box/screen.h"
#include "box/core/manifest.h"
#include "box/error.h"
#include "box/string.h"
#include "box/timeouts.h"
#include "boxos_decks.h"

/* Every op here is a memcpy or a page-table walk answered on the spot. */
static int HwCall(uint16_t op, const void *params, uint16_t param_size,
                  const void *in, uint32_t in_size,
                  void *out, uint32_t out_cap, uint32_t *out_actual)
{
    int rc = MfCall1(DECK_HARDWARE, op, params, param_size, in, in_size,
                     out, out_cap, out_actual, BOX_ANSWER_GUARANTEED, NULL);
    if (rc == OK) return OK;
    return rc > 0 ? -rc : rc;
}

int screen_claim(ScreenDescription *out)
{
    if (!out) return -ERR_NULL_POINTER;
    uint32_t got = 0;
    int rc = HwCall(HW_SCREEN_CLAIM, NULL, 0, NULL, 0, out, sizeof(*out), &got);
    if (rc != OK) return rc;
    return got == sizeof(*out) ? OK : -ERR_INTERNAL;
}

int screen_release(void)
{
    return HwCall(HW_SCREEN_RELEASE, NULL, 0, NULL, 0, NULL, 0, NULL);
}

int log_say(const char *bytes, uint32_t len, uint64_t *start, uint64_t *end)
{
    if (!bytes || len == 0) return -ERR_INVALID_ARGUMENT;
    if (len > LOG_SAY_MAX)  return -ERR_OUT_OF_RANGE;

    uint64_t at[2] = {0, 0};
    int rc = HwCall(HW_LOG_SAY, NULL, 0, bytes, len, at, sizeof(at), NULL);
    if (rc != OK) return rc;
    if (start) *start = at[0];
    if (end)   *end   = at[1];
    return OK;
}

int log_bell(uint64_t from, bool *news)
{
    if (!news) return -ERR_NULL_POINTER;
    uint8_t said = 0;
    int rc = HwCall(HW_LOG_BELL, &from, sizeof(from), NULL, 0, &said, 1, NULL);
    if (rc != OK) return rc;
    *news = said != 0;
    return OK;
}

int log_read_tones(uint64_t from, char *bytes, uint8_t *tones, uint32_t max,
                   uint64_t *oldest, uint64_t *written)
{
    if (!bytes || !tones || max == 0) return -ERR_INVALID_ARGUMENT;
    if (max > HW_LOG_READ_CHUNK) max = HW_LOG_READ_CHUNK;

    uint8_t params[12];
    uint32_t flags = HW_LOG_READ_TONES;
    memcpy(params,     &from,  8);
    memcpy(params + 8, &flags, 4);

    /* The answer says its own length in its header, as every HW_LOG_READ
     * does (current.c); the crate carries no size back for it. */
    uint8_t buf[HW_LOG_READ_HEADER + 2u * HW_LOG_READ_CHUNK];
    int rc = HwCall(HW_LOG_READ, params, sizeof(params), NULL, 0,
                    buf, HW_LOG_READ_HEADER + 2u * max, NULL);
    if (rc != OK) return rc;

    uint64_t o, w, copied;
    memcpy(&o,      buf,      8);
    memcpy(&w,      buf + 8,  8);
    memcpy(&copied, buf + 16, 8);
    if (copied > max) return -ERR_CORRUPTED;

    memcpy(bytes, buf + HW_LOG_READ_HEADER, (size_t)copied);
    memcpy(tones, buf + HW_LOG_READ_HEADER + copied, (size_t)copied);
    if (oldest)  *oldest  = o;
    if (written) *written = w;
    return (int)copied;
}
