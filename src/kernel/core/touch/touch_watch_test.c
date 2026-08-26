#include "touch.h"
#include "logbook.h"
#include "klib.h"

/* -------------------------------------------------------------------------
 * TouchWatch self-test.
 *
 * A kernel ear is the kind of thing that looks obviously correct and is
 * obviously correct right up until two of them exist, or one is cleared while
 * a publish is walking the list, or a callback publishes. Those are the cases
 * that cost real sessions, so they are the cases written down here — with the
 * expected counts stated as literals rather than computed by the code under
 * test.
 *
 * It runs immediately after guide_init, where Touch is up and no process
 * exists yet. That is deliberate: the whole reason this subsystem exists is
 * that the kernel needs to listen during bring-up, before there is any
 * userspace to listen on its behalf.
 * ------------------------------------------------------------------------- */

typedef struct {
    uint32_t calls;
    TouchTag last_tag;
    uint32_t last_plen;
    uint32_t last_value;   /* first 4 payload bytes, when present */
} WatchProbe;

static void watch_probe_fn(TouchTag tag_id, const void *payload, uint32_t plen,
                           uint32_t source_pid, void *ctx)
{
    (void)source_pid;
    WatchProbe *p = (WatchProbe *)ctx;
    p->calls++;
    p->last_tag  = tag_id;
    p->last_plen = plen;
    p->last_value = (payload && plen >= sizeof(uint32_t))
                    ? *(const uint32_t *)payload : 0;
}

/* Re-entry probe: publishes to a second tag from inside its own callback. */
static TouchTag  g_reentry_target = TOUCH_TAG_INVALID;
static WatchProbe g_reentry_inner;

static void watch_reentry_fn(TouchTag tag_id, const void *payload, uint32_t plen,
                             uint32_t source_pid, void *ctx)
{
    (void)tag_id; (void)payload; (void)plen; (void)source_pid;
    WatchProbe *p = (WatchProbe *)ctx;
    p->calls++;
    uint32_t inner = 0xBEEF;
    TouchPublishId(g_reentry_target, &inner, sizeof(inner), 0, TOUCH_FLAG_KERNEL);
}

void TouchWatchSelfTest(void)
{
    kprintf("[TOUCH WATCH TEST] begin\n");
    int pass = 0, fail = 0;

    #define WATCH_CHECK(cond, label) do {                                        \
        if (cond) { pass++; kprintf("[TOUCH WATCH TEST] PASS: " label "\n"); }    \
        else      { fail++; kprintf("[TOUCH WATCH TEST] FAIL: " label "\n"); }    \
    } while (0)

    /* Named through the kernel's own book, which is the only vocabulary
     * guaranteed to exist here — no volume has been mounted yet. */
    TouchTag full = TOUCH_TAG_INVALID, bare = TOUCH_TAG_INVALID;
    TouchLogbookResolve("watchtest:one", &full, &bare);
    if (full == TOUCH_TAG_INVALID || bare == TOUCH_TAG_INVALID) {
        kprintf("[TOUCH WATCH TEST] FAILED: the Logbook would not name the tag\n");
        return;
    }
    WATCH_CHECK((full & 0x8000u) && (bare & 0x8000u),
                "a name from the Logbook carries the kernel bit");

    /* --- one ear hears one publish, with its payload intact --- */
    WatchProbe a = {0};
    TouchWatch *wa = TouchWatchSet(full, watch_probe_fn, &a);
    if (!wa) { kprintf("[TOUCH WATCH TEST] FAILED: no memory for a watch\n"); return; }

    WATCH_CHECK(TouchWatchCount(full) == 1, "one ear is counted on the tag");
    WATCH_CHECK(TouchHasAnyListenersForTag(full),
                "an ear alone makes the tag worth publishing to");

    uint32_t payload = 0x1234u;
    TouchPublishId(full, &payload, sizeof(payload), 0, TOUCH_FLAG_KERNEL);
    WATCH_CHECK(a.calls == 1,           "the ear was called exactly once");
    WATCH_CHECK(a.last_tag == full,     "it was told which tag it heard");
    WATCH_CHECK(a.last_plen == 4,       "it was told how many bytes arrived");
    WATCH_CHECK(a.last_value == 0x1234u,"the payload arrived unchanged");

    /* --- two ears on one tag both hear it: this is multicast, not a queue --- */
    WatchProbe b = {0};
    TouchWatch *wb = TouchWatchSet(full, watch_probe_fn, &b);
    if (!wb) { kprintf("[TOUCH WATCH TEST] FAILED: no memory for a second watch\n"); TouchWatchClear(wa); return; }

    WATCH_CHECK(TouchWatchCount(full) == 2, "two ears are counted");
    TouchPublishId(full, &payload, sizeof(payload), 0, TOUCH_FLAG_KERNEL);
    WATCH_CHECK(a.calls == 2 && b.calls == 1, "both ears heard the same publish");

    /* --- clearing the SECOND-added ear (the list head) must leave the other
     *     linked. Getting head-vs-middle unlink wrong is the classic way a
     *     list like this loses a listener silently. --- */
    TouchWatchClear(wb);
    WATCH_CHECK(TouchWatchCount(full) == 1, "clearing one ear leaves one");
    TouchPublishId(full, &payload, sizeof(payload), 0, TOUCH_FLAG_KERNEL);
    WATCH_CHECK(a.calls == 3, "the surviving ear still hears");
    WATCH_CHECK(b.calls == 1, "the cleared ear hears nothing more");

    /* --- the bare id is the wildcard: publishing the pair reaches an ear on
     *     the key even though the value was never named to it --- */
    WatchProbe c = {0};
    TouchWatch *wc = TouchWatchSet(bare, watch_probe_fn, &c);
    if (wc) {
        TouchPublishPair(full, bare, &payload, sizeof(payload), 0, TOUCH_FLAG_KERNEL);
        WATCH_CHECK(c.calls == 1, "an ear on the bare key hears a valued publish");
        WATCH_CHECK(a.calls == 4, "and the ear on the full name hears it too");
        TouchWatchClear(wc);
    } else {
        fail++; kprintf("[TOUCH WATCH TEST] FAIL: no memory for a bare-key watch\n");
    }

    /* --- an ear may publish from inside itself; the nested publish is
     *     delivered, and the stack-headroom guard is what stops it running
     *     away --- */
    TouchTag inner_full = TOUCH_TAG_INVALID, inner_bare = TOUCH_TAG_INVALID;
    TouchLogbookResolve("watchtest:inner", &inner_full, &inner_bare);
    g_reentry_target = inner_full;
    g_reentry_inner = (WatchProbe){0};

    WatchProbe outer = {0};
    TouchWatch *w_inner = TouchWatchSet(inner_full, watch_probe_fn, &g_reentry_inner);
    TouchWatch *w_outer = TouchWatchSet(full, watch_reentry_fn, &outer);
    if (w_inner && w_outer) {
        TouchPublishId(full, &payload, sizeof(payload), 0, TOUCH_FLAG_KERNEL);
        WATCH_CHECK(outer.calls == 1, "the publishing ear ran once");
        WATCH_CHECK(g_reentry_inner.calls == 1,
                    "the publish it made from inside itself was delivered");
        WATCH_CHECK(g_reentry_inner.last_value == 0xBEEFu,
                    "the nested payload arrived unchanged");
    } else {
        fail++; kprintf("[TOUCH WATCH TEST] FAIL: no memory for the re-entry pair\n");
    }
    if (w_outer) TouchWatchClear(w_outer);
    if (w_inner) TouchWatchClear(w_inner);
    g_reentry_target = TOUCH_TAG_INVALID;

    /* --- the last ear leaving makes the tag quiet again --- */
    uint32_t before = a.calls;
    TouchWatchClear(wa);
    WATCH_CHECK(TouchWatchCount(full) == 0, "no ears left on the tag");
    WATCH_CHECK(!TouchHasAnyListenersForTag(full),
                "a tag nobody listens to reports nobody listening");
    TouchPublishId(full, &payload, sizeof(payload), 0, TOUCH_FLAG_KERNEL);
    WATCH_CHECK(a.calls == before, "publishing into an empty tag calls nobody");

    #undef WATCH_CHECK

    if (fail == 0) kprintf("[TOUCH WATCH TEST] PASSED: all %d checks OK\n", pass);
    else           kprintf("[TOUCH WATCH TEST] FAILED: %d of %d checks failed\n",
                           fail, pass + fail);
}
