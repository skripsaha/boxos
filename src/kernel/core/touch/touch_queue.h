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
 *   KNOCK    a Knock knocked at the tick, for a waiter whose next move is due
 *            at a time rather than at an event of its own — the Settle's Lull.
 *
 * A WAKE or KNOCK node armed again while it waits moves to the new tick: one
 * node, one deadline.
 */

struct process_t;
struct Knock;

typedef enum {
    TOUCH_QUEUE_PUBLISH = 0,
    TOUCH_QUEUE_WAKE    = 1,
    TOUCH_QUEUE_KNOCK   = 2,
} TouchQueueKind;

typedef struct TouchQueueNode {
    struct TouchQueueNode *next;
    uint64_t               fire_tick;
    uint8_t                kind;     /* TouchQueueKind */
    uint8_t                armed;    /* in the queue — under the queue's lock */
    union {
        struct {
            uint16_t tag_id;
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
        struct Knock *knock;
    } u;
} TouchQueueNode;

void TouchQueueInit(void);

/* A delayed publish's node, its payload copied into it: NULL when there is no
 * memory, which the sender says to its caller. Armed, it is the queue's. */
TouchQueueNode *TouchQueuePublishNode(uint16_t tag_id, const void *payload, uint32_t plen,
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

/* `k` knocked at `fire_tick`, carried by the caller's node `n`. */
void TouchQueueArmKnock(TouchQueueNode *n, struct Knock *k, uint64_t fire_tick);

/* A WAKE or KNOCK node taken out of the queue unfired; nothing when it is not
 * armed. */
void TouchQueueDisarm(TouchQueueNode *n);

void TouchQueueTick(uint64_t now);

#endif /* TOUCH_QUEUE_H */
