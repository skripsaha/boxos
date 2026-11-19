#ifndef TOUCH_QUEUE_H
#define TOUCH_QUEUE_H

#include "ktypes.h"

/*
 * The touch queue — what the kernel does at a moment named in advance.
 *
 * A moment is the kernel's uptime in microseconds (pit_get_uptime_us — the
 * HPET where there is one), the one clock every deadline is named in. The
 * queue keeps its nodes nearest first, and its keeper — the core that ran
 * TouchQueueInit, the BSP — holds its LAPIC one-shot at the nearest: when it
 * interrupts (DEADLINE_VECTOR, TouchQueueDeadline) every node whose moment
 * came fires, and the one-shot is set for the next. A core that arms a node
 * nearer than the one-shot is set for tells the keeper with an IPI at the
 * same vector; nearer or not is decided under the queue's lock, against the
 * moment the keeper set under it, so a nearer node is never left to wait.
 * The kernel's tick fires what is due as well (TouchQueueFireDue): the way the
 * queue keeps time where the keeper has no one-shot, and a net under it
 * where it has.
 *
 * ‼ THE NODE BELONGS TO WHOEVER ARMS IT. The queue used to allocate a node for
 * every arm and drop the arm without a word when the heap had none: a park's
 * deadline that never fired, a delayed publish nobody heard. Now the one who
 * arms brings the node — a process's wake lives in the process, a Knock's in
 * its owner, a delayed publish's is made by its sender, who hears the refusal
 * when there is no memory for it — so arming cannot fail.
 *
 * Four kinds:
 *   PUBLISH  a delayed Touch: published at its moment and freed by the queue,
 *            which owns it from its arm (TouchQueuePublishNode made it);
 *   WAKE     a timed park's deadline, embedded in the process (park_wake);
 *            armed, it holds a reference on the process until it fires or is
 *            disarmed;
 *   ALARM    a strand's alarm, embedded in the process (alarm_node); armed, it
 *            holds a reference on the process as a WAKE does. Its fire is the
 *            alarm's delivery (alarm_ops.c);
 *   CALL     a function of the armer's, called by the fire itself, for a waiter
 *            whose next move is due at a time rather than at an event of its
 *            own — the Settle's Lull, which wakes its committer's core. In the
 *            interrupt: it allocates nothing, waits for nothing and takes no
 *            lock but an IRQ-safe one.
 *
 * A WAKE, ALARM or CALL node armed again while it waits moves to the new moment:
 * one node, one deadline.
 */

struct process_t;

typedef enum {
    TOUCH_QUEUE_PUBLISH = 0,
    TOUCH_QUEUE_WAKE    = 1,
    TOUCH_QUEUE_CALL    = 2,
    TOUCH_QUEUE_ALARM   = 3,
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
    struct {
        struct process_t *target;
    } alarm;
    void (*call)(void);
} TouchQueueFire;

/* A pass takes every node whose moment came out of the queue at once, onto a
 * list of its own (fired_next) with what each fire does as it stood then
 * (fired_kind, fired): a WAKE or CALL node armed again before its fire runs
 * changes only its queue fields, never the pass's. */
typedef struct TouchQueueNode {
    struct TouchQueueNode *next;
    struct TouchQueueNode *fired_next;
    uint64_t               fire_us;  /* the moment it fires at, the kernel's uptime in µs */
    uint8_t                kind;     /* TouchQueueKind */
    uint8_t                armed;    /* in the queue — under the queue's lock */
    uint8_t                fired_kind;
    TouchQueueFire         u;
    TouchQueueFire         fired;
} TouchQueueNode;

/* The queue, empty, and its keeper's one-shot: the calling core keeps it. */
void TouchQueueInit(void);

/* A delayed publish's node, its payload copied into it: NULL when there is no
 * memory, which the sender says to its caller. Armed, it is the queue's. */
TouchQueueNode *TouchQueuePublishNode(uint32_t tag_id, const void *payload, uint32_t plen,
                                      uint32_t source_pid, uint16_t flags);

/* A PUBLISH node from TouchQueuePublishNode, fired at `fire_us`. */
void TouchQueueArm(TouchQueueNode *n, uint64_t fire_us);

/*
 * The deadline of `target`'s timed park, at the moment `fire_us`, in the node
 * the process carries (process_t.park_wake).
 *
 *   park_seq     the caller's process_t.park_seq at arm time. On expiry the
 *                fire acts (reschedules the target, passes its deadline
 *                baton) ONLY while the strand is still in that same sleep. A
 *                wait that ended early leaves its wake armed, and the next park
 *                moves it to its own moment and number — see process_t.park_seq.
 *   owes_result  1 for an addr_park: on expiry the kernel owes ERR_TIMEOUT as
 *                a Result, delivered by a K-Core on the process's own baton.
 *                SysTouchAwait registers no addr_wait entry and passes 0: it
 *                is owed no Result, only the reschedule.
 */
void TouchQueueArmWake(struct process_t *target, uint64_t fire_us,
                       uint32_t owes_result, uint32_t park_seq);

/* The alarm of `target`, in the node the process carries (process_t.alarm_node),
 * fired at `fire_us`: it holds a reference on the process until it fires or is
 * disarmed. Armed again while it waits, it moves to the new moment. */
void TouchQueueArmAlarm(struct process_t *target, uint64_t fire_us);

/* `call` called at `fire_us`, in the interrupt that fires it, carried by the
 * caller's node `n`. */
void TouchQueueArmCall(TouchQueueNode *n, void (*call)(void), uint64_t fire_us);

/* A WAKE, ALARM or CALL node taken out of the queue unfired; nothing when it is not
 * armed. */
void TouchQueueDisarm(TouchQueueNode *n);

/* Every node whose moment is at or before `now_us` fired: taken out of the
 * queue in one pass under its lock, fired with the lock let go. One pass at a
 * time; a pass that finds another under way leaves its nodes to the next. */
void TouchQueueFireDue(uint64_t now_us);

/* The keeper's one-shot came, or a core named a nearer moment: what is due
 * fired, the one-shot set for the next. DEADLINE_VECTOR, on the keeper. */
void TouchQueueDeadline(void);

#ifdef CONFIG_VOLUME_TESTS
/* The most nodes one pass looked at, since it was last asked — which starts
 * it again. */
uint32_t TouchQueueFireMost(void);
#endif

#endif /* TOUCH_QUEUE_H */
