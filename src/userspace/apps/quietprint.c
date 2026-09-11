
#include "box/print.h"
#include "box/touch.h"
#include "box/brook.h"
#include "box/strand.h"
#include "box/clock.h"
#include "box/cpu.h"
#include "box/string.h"
#include "box/system.h"

#define QUIET_MS    3000u
#define QUIET_TAG   "quietprint:nobody"
#define STREAM_TAG  "quietprint:stream"
#define STREAM_FS   8u
#define STREAM_FC   4u

static void be_quiet(uint32_t ms)
{
    static TouchTag tag = TOUCH_TAG_INVALID;
    if (tag == TOUCH_TAG_INVALID) {
        tag = touch_pair_choose(touch_intern(QUIET_TAG));
        if (tag != TOUCH_TAG_INVALID) touch_claim(tag, TOUCH_REST, 0, 0);
    }

    uint64_t deadline = clock_uptime_ms() + ms;
    for (;;) {
        uint64_t now = clock_uptime_ms();
        if (now >= deadline) return;
        Touch t;
        if (tag == TOUCH_TAG_INVALID) return;
        (void)touch_await(tag, &t, (uint32_t)(deadline - now));
    }
}

static void stream_writer(void *arg)
{
    (void)arg;
    be_quiet(QUIET_MS);

    Brook *w = brook_open(STREAM_TAG, STREAM_FS, STREAM_FC, BROOK_WRITER);
    if (w) {
        uint8_t frame[STREAM_FS];
        memset(frame, 0xA5, sizeof(frame));
        (void)brook_push(w, frame);
    }

    be_quiet(QUIET_MS);
    if (w) brook_release(w);
    strand_exit();
}

int main(void)
{
    uint64_t began = clock_uptime_ms();

    printf("[QP] line 1 at %lu ms\n", (unsigned long)(clock_uptime_ms() - began));
    io_flush();

    const char *how = "quiet";
    Brook *r = cpu_has_fsgsbase()
             ? brook_open(STREAM_TAG, STREAM_FS, STREAM_FC,
                          BROOK_READER | BROOK_CREATE)
             : 0;
    if (r && strand_spawn(stream_writer, 0) != 0) {
        uint8_t frame[STREAM_FS];
        if (brook_pop(r, frame) == 0) how = "brook";
        brook_release(r);
    } else {
        if (r) brook_release(r);
        be_quiet(QUIET_MS);
    }

    printf("[QP] line 2 at %lu ms (%s)\n",
           (unsigned long)(clock_uptime_ms() - began), how);
    io_flush();

    be_quiet(QUIET_MS);

    printf("[QP] done after %lu ms\n", (unsigned long)(clock_uptime_ms() - began));
    io_flush();
    return 0;
}