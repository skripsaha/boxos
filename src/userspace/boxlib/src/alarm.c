/*
 * alarm.c — the alarm's two calls and its one listener (box/alarm.h).
 */

#include "box/alarm.h"
#include "box/core/manifest.h"
#include "box/error.h"
#include "box/string.h"
#include "box/touch.h"
#include "boxos_decks.h"

int alarm_set(uint64_t moment_ms)
{
    uint8_t         mbuf[96];
    ManifestBuilder mb;
    if (ManifestBuilderInit(&mb, mbuf, sizeof(mbuf)) != 0) return -ERR_INVALID_ARGUMENT;
    if (ManifestBuilderAddOp(&mb, DECK_SYSTEM, SYSTEM_OP_ALARM, 0, CRATE_INDEX_NONE, CRATE_INDEX_NONE,
                             &moment_ms, sizeof(moment_ms)) != 0)
        return -ERR_INVALID_ARGUMENT;
    if (ManifestBuilderFinalize(&mb) != 0) return -ERR_INVALID_ARGUMENT;

    /* Small enough to ride inside the ring slot: this frame may die the moment
     * the push returns (the enclosure rule in manifest.c). */
    int rc = ManifestSubmitNoWait((const Manifest *)mbuf, NULL, 0, 0);
    return rc == OK ? OK : -rc;
}

int alarm_cancel(void)
{
    return alarm_set(0);
}

bool alarm_heard(uint64_t *moment_ms)
{
    static TouchTag tag = TOUCH_TAG_INVALID;
    if (tag == TOUCH_TAG_INVALID) tag = touch_pair_choose(touch_intern(ALARM_TOUCH_TAG));

    Touch event;
    if (!touch_try_pop_tag(tag, &event)) return false;
    AlarmEvent heard;
    if (event.payload_len < sizeof(heard)) return false;
    memcpy(&heard, event.payload, sizeof(heard));
    if (moment_ms) *moment_ms = heard.moment_ms;
    return true;
}
