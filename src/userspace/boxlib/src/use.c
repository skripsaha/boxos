
#include "box/use.h"
#include "box/core/manifest.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/error.h"
#include "box/timeouts.h"
#include "boxos_decks.h"

int use_set(const char *tags, bool *remembered)
{
    if (!tags || tags[0] == '\0') return use_clear(remembered);

    uint8_t  kept       = 0;
    uint32_t out_actual = 0;
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_USE_SET,
                     NULL, 0,
                     tags, (uint32_t)strlen(tags),
                     &kept, sizeof(kept), &out_actual,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (remembered) *remembered = (rc == 0 && out_actual >= 1 && kept != 0);
    return box_fail(rc);
}

int use_clear(bool *remembered)
{
    uint8_t  kept       = 0;
    uint32_t out_actual = 0;
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_USE_CLEAR,
                     NULL, 0, NULL, 0,
                     &kept, sizeof(kept), &out_actual,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (remembered) *remembered = (rc == 0 && out_actual >= 1 && kept != 0);
    return box_fail(rc);
}

int use_get(char *buf, size_t cap, size_t *needed)
{
    if (!buf && cap != 0) return -ERR_INVALID_ARGUMENT;
    if (needed) *needed = 0;

    uint32_t wire_cap = (uint32_t)(cap * 2 + 8);
    uint8_t *wire = malloc(wire_cap);
    if (!wire) return -ERR_NO_MEMORY;

    uint32_t out_actual = 0;
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_USE_GET,
                     NULL, 0,
                     NULL, 0,
                     wire, wire_cap, &out_actual,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0) {
        free(wire);
        return box_fail(rc);
    }
    if (out_actual < 8) {
        free(wire);
        return -ERR_INTERNAL;
    }

    uint32_t count = 0, need32 = 0;
    memcpy(&count,  wire,     4);
    memcpy(&need32, wire + 4, 4);
    if (needed) *needed = need32;

    uint32_t pos     = 8;
    size_t   used    = 0;
    int      written = 0;
    if (buf && cap > 0) buf[0] = '\0';
    for (uint32_t i = 0; i < count; i++) {
        if (pos + 2 > out_actual) break;
        uint16_t len = 0;
        memcpy(&len, wire + pos, 2);
        pos += 2;
        if (pos + len > out_actual) break;

        size_t sep = written ? 1 : 0;
        if (!buf || used + sep + len + 1 > cap) break;
        if (sep) buf[used++] = ',';
        memcpy(buf + used, wire + pos, len);
        used += len;
        buf[used] = '\0';
        pos += len;
        written++;
    }

    free(wire);
    return written;
}