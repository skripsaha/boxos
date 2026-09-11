#include "auth_decouple_selftest.h"
#include "auth_tags.h"
#include "manifest_auth.h"
#include "tagfs.h"
#include "klib.h"

#define AUTHDEC_FILLERS 70u

static error_t intern_god_high(uint16_t *out_god_id)
{
    TagRegistry reg;
    if (tag_registry_init(&reg) != 0) {
        kprintf("[AUTHDEC] FAIL: scratch registry init\n");
        return ERR_INTERNAL;
    }

    for (unsigned i = 0; i < AUTHDEC_FILLERS; i++) {
        char filler[24];
        ksnprintf(filler, sizeof(filler), "authdec.filler.%u", i);
        if (tag_registry_intern(&reg, filler, NULL) == TAGFS_INVALID_TAG_ID) {
            kprintf("[AUTHDEC] FAIL: filler intern %u\n", i);
            tag_registry_destroy(&reg);
            return ERR_INTERNAL;
        }
    }

    uint16_t god_id = tag_registry_intern(&reg, "god", NULL);
    tag_registry_destroy(&reg);

    if (god_id == TAGFS_INVALID_TAG_ID) {
        kprintf("[AUTHDEC] FAIL: god intern\n");
        return ERR_INTERNAL;
    }
    if (god_id < 64) {
        kprintf("[AUTHDEC] FAIL: god id %u < 64 — high-id path not exercised\n",
                (unsigned)god_id);
        return ERR_INTERNAL;
    }

    *out_god_id = god_id;
    return OK;
}

error_t AuthDecoupleSelfTest(void)
{
    uint16_t god_id = 0;
    error_t rc = intern_god_high(&god_id);
    if (rc != OK)
        return rc;

    if (auth_bit_for_key("god") != AUTH_TAG_GOD) {
        kprintf("[AUTHDEC] FAIL: auth_bit_for_key(god) != AUTH_TAG_GOD\n");
        return ERR_INTERNAL;
    }
    if (!auth_level_permits(AUTH_TAG_GOD, OP_AUTH_SYSTEM)) {
        kprintf("[AUTHDEC] FAIL: god@id%u did not override OP_AUTH_SYSTEM\n",
                (unsigned)god_id);
        return ERR_INTERNAL;
    }
    kprintf("[AUTHDEC] god@id%u override=OK\n", (unsigned)god_id);

    if (auth_bit_for_key("stopped") != AUTH_TAG_STOPPED) {
        kprintf("[AUTHDEC] FAIL: auth_bit_for_key(stopped) != AUTH_TAG_STOPPED\n");
        return ERR_INTERNAL;
    }
    if (auth_level_permits(AUTH_TAG_STOPPED, OP_AUTH_APP)) {
        kprintf("[AUTHDEC] FAIL: stopped process permitted OP_AUTH_APP\n");
        return ERR_INTERNAL;
    }
    kprintf("[AUTHDEC] stopped deny=OK\n");

    kprintf("[AUTHDEC] PASS\n");
    return OK;
}