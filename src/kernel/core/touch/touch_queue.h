#ifndef TOUCH_QUEUE_H
#define TOUCH_QUEUE_H

#include "ktypes.h"

/*
 * The touch queue — what the PIT tick does at a tick named in advance.
 *
 * ‼ THE NODE BELONGS TO WHOEVER ARMS IT. The queue used to allocate a node for
 * every arm and drop the arm without a word when the heap had none: a park's
 * deadline that never fired, a delayed publish nobody heard. Now the one who
 * arms brings the node — a process's wake lives in the process, a Knock's in
 * its owner, a delayed publish's is made by its sender, who hears the refusal
 * when there is no memory for it — so arming cannot fail.
 *
 * Three kinds:
 *   PUBLISH  a delayed Touch: published at the tick and freed by the queue,
 *            which owns it from its arm (TouchQueuePublishNode made it);
 *   WAKE     a timed park's deadline, embedded in the process (park_wake);
 *            armed, it holds a reference on the process until it fires or is
 *            disarmed;
 *   CALL     a function of the armer's, called by the tick itself, for a waiter
 *            whose next move is due at a time rather than at an event of its
 *            own — the Settle's Lull, which wakes its committer's core. In the
 *            interrupt: it allocates nothing, waits for nothing and takes no
 *            lock but an IRQ-safe one.
 *
 * A WAKE or CALL node armed again while it waits moves to the new tick: one
 * node, one deadline.
 */

struct process_t;

typedef enum {
    TOUCH_QUEUE_PUBLISH = 0,
    TOUCH_QUEUE_WAKE    = 1,
    TOUCH_QUEUE_CALL    = 2,
} TouchQueueKind;

/* What a node's fire does, by its kind. */
typedef union {
    struct {
        uint32_t tag_id;
        uint16_t flags;
        uint32_t source_pid;
        uint32_t plen;
        uint8_t *payload;         /* inside the node's own allocation */
    } publish;
    struct {
        struct process_t *target;
        uint32_t          owes_result;
        uint32_t          park_seq;
    } wake;
    void (*call)(void);
} TouchQueueFire;

/* A tick takes every node whose tick came out of the queue in one pass, onto
 * a list of its own (fired_next) with what each fire does as it stood then
 * (fired_kind, fired): a WAKE or CALL node armed again before its fire runs
 * changes only its queue fields, never the tick's. */
typedef struct TouchQueueNode {
    struct TouchQueueNode *next;
    struct TouchQueueNode *fired_next;
    uint64_t               fire_tick;
    uint8_t                kind;     /* TouchQueueKind */
    uint8_t                armed;    /* in the queue — under the queue's lock */
    uint8_t                fired_kind;
    TouchQueueFire         u;
    TouchQueueFire         fired;
} TouchQueueNode;

void TouchQueueInit(void);

/* A delayed publish's node, its payload copied into it: NULL when there is no
 * memory, which the sender says to its caller. Armed, it is the queue's. */
TouchQueueNode *TouchQueuePublishNode(uint32_t tag_id, const void *payload, uint32_t plen,
                                      uint32_t source_pid, uint16_t flags);

/* A PUBLISH node from TouchQueuePublishNode, fired at `fire_tick`. */
void TouchQueueArm(TouchQueueNode *n, uint64_t fire_tick);

/*
 * The deadline of `target`'s timed park, at `fire_tick` PIT ticks, in the node
 * the process carries (process_t.park_wake).
 *
 *   park_seq     the caller's process_t.park_seq at arm time. On expiry the
 *                PIT tick acts (reschedules the target, passes its deadline
 *                baton) ONLY while the strand is still in that same sleep. A
 *                wait that ended early leaves its wake armed, and the next park
 *                moves it to its own tick and number — see process_t.park_seq.
 *   owes_result  1 for an addr_park: on expiry the kernel owes ERR_TIMEOUT as
 *                a Result, delivered by a K-Core on the process's own baton.
 *                SysTouchAwait registers no addr_wait entry and passes 0: it
 *                is owed no Result, only the reschedule.
 */
void TouchQueueArmWake(struct process_t *target, uint64_t fire_tick,
                       uint32_t owes_result, uint32_t park_seq);

/* `call` called by the tick at `fire_tick`, carried by the caller's node `n`. */
void TouchQueueArmCall(TouchQueueNode *n, void (*call)(void), uint64_t fire_tick);

/* A WAKE or CALL node taken out of the queue unfired; nothing when it is not
 * armed. */
void TouchQueueDisarm(TouchQueueNode *n);

/* Every node whose tick came fired: taken out of the queue in one pass under
 * its lock, fired with the lock let go. One tick at a time — PIT IRQ0 — and a
 * tick that finds another under way leaves its nodes to the next. */
void TouchQueueTick(uint64_t now);

#ifdef CONFIG_VOLUME_TESTS
/* The most nodes one tick looked at, since it was last asked — which starts
 * it again. */
uint32_t TouchQueueTickMost(void);
#endif

#endif /* TOUCH_QUEUE_H */
