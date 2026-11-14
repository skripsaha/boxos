/* rise.c — a program that falls, raised again by the system. See rise.h. */

#include "rise.h"

#include "autostart.h"
#include "baton.h"
#include "cabin.h"
#include "cabin_info.h"
#include "kernel_clock.h"
#include "kernel_config.h"
#include "klib.h"
#include "proc_exit.h"
#include "process.h"
#include "touch_queue.h"
#include "vmm.h"

#define RISE_TICKS(Seconds) ((uint64_t)(Seconds) * CONFIG_SCHED_DEFAULT_TICK_HZ)

enum {
    RISE_PAUSE_FIRST_S = 1,    /* the pause before the raise after a first fall */
    RISE_PAUSE_MOST_S  = 16,   /* the longest it grows to, doubling */
    RISE_SOON_S        = 10,   /* a fall this soon after the start is one of a run */
    RISE_SOON_FALLS    = 3,    /* so many in a row, and it is let be */
    RISE_CALM_S        = 60,   /* a run this long before a fall starts the pause over */
};

/* A program the system raises when it falls, by its file — the one thing that
 * stays the same from one life of it to the next. */
typedef struct Riser {
    struct Riser *next;
    uint64_t      file;
    uint32_t      pid;          /* its life now, by its main strand; 0 while it is down */
    uint32_t      generation;
    uint64_t      started;      /* the tick its life now began */
    uint64_t      due;          /* the tick it is raised again at; 0 when none is owed */
    uint32_t      pause_s;      /* what its next fall waits before the raise */
    uint32_t      soon;         /* falls in a row, each within RISE_SOON_S of its start */
    uint32_t      raised;       /* times raised again */
    bool          raising;      /* being started again: past the door, not yet watched */
    char          name[];       /* as the volume calls the file */
} Riser;

static spinlock_t     g_lock;   /* every Riser, and the node's arming — taken last, holding no other */
static Riser         *g_risers;
static TouchQueueNode g_due;    /* the touch queue's call at the earliest raise owed */
static Knock          g_door;   /* the call's knock: a K-Core starts the raise */
static volatile bool  g_halting; /* the machine is going off: nothing is raised */

static Riser *RiserOfFile(uint64_t file)
{
    for (Riser *r = g_risers; r; r = r->next)
        if (r->file == file) return r;
    return NULL;
}

static Riser *RiserOfLife(uint32_t pid, uint32_t generation)
{
    for (Riser *r = g_risers; r; r = r->next)
        if (r->pid != 0 && r->pid == pid && r->generation == generation) return r;
    return NULL;
}

static void RiserUnlink(Riser *gone)
{
    for (Riser **at = &g_risers; *at; at = &(*at)->next) {
        if (*at != gone) continue;
        *at = gone->next;
        return;
    }
}

/* Called by the tick: the door knocked on, nothing more — a start is no
 * interrupt's work. */
static void RiseDue(void)
{
    KnockOn(&g_door);
}

/* The touch queue's call armed at the earliest raise owed, or disarmed when
 * none is. Under the lock, so two changes never arm out of order. */
static void RiseArmLocked(void)
{
    uint64_t earliest = 0;
    for (Riser *r = g_risers; r; r = r->next)
        if (r->due != 0 && (earliest == 0 || r->due < earliest)) earliest = r->due;
    if (earliest != 0) TouchQueueArmCall(&g_due, RiseDue, earliest);
    else TouchQueueDisarm(&g_due);
}

/* Every raise whose tick came, started: one at a time, the lock let go while
 * the volume is read. Past the lock the Riser is looked for again by its file:
 * the start's own watch gives it a life, and that life may end — and free it —
 * before the lock is taken back. One still raising was not watched. */
static void RiseRun(void *ctx)
{
    (void)ctx;
    KnockOpen(&g_door);
    for (;;) {
        uint64_t file = 0;
        spin_lock(&g_lock);
        if (__atomic_load_n(&g_halting, __ATOMIC_ACQUIRE)) {
            spin_unlock(&g_lock);
            return;
        }
        const uint64_t now = kernel_tick_get();
        for (Riser *r = g_risers; r && file == 0; r = r->next) {
            if (r->due == 0 || r->due > now) continue;
            r->due     = 0;
            r->raising = true;
            r->raised++;
            file = r->file;
        }
        RiseArmLocked();
        spin_unlock(&g_lock);
        if (file == 0) return;

        process_t *proc      = AutostartAgain(file);
        Riser     *unwatched = NULL;
        spin_lock(&g_lock);
        Riser *r = RiserOfFile(file);
        if (r && r->raising) {
            RiserUnlink(r);
            unwatched = r;
            kprintf("[RISE] '%s' is not raised again: %s\n", r->name,
                    proc ? "its box no longer says rise"
                         : "its file is gone, or could not be started");
        }
        spin_unlock(&g_lock);
        kfree(unwatched);
        if (proc) {
            if (!ProcessStartCreated(proc))
                kprintf("[RISE] PID %u was stopped or ended before it could run\n", proc->pid);
            process_ref_dec(proc);
        }
    }
}

void RiseHalt(void)
{
    spin_lock(&g_lock);
    __atomic_store_n(&g_halting, true, __ATOMIC_RELEASE);
    TouchQueueDisarm(&g_due);
    spin_unlock(&g_lock);
}

void RiseInit(void)
{
    spinlock_init(&g_lock);
    KnockInit(&g_door, RiseRun, NULL);
}

void RiseWatch(process_t *proc, uint64_t file, const char *name)
{
    if (!proc || !proc->cabin || !proc->cabin->rise) return;

    /* Its Riser, made when it is first watched: asked for outside the lock,
     * which is held by no allocation. */
    Riser *fresh = NULL;
    for (;;) {
        spin_lock(&g_lock);
        Riser *r = RiserOfFile(file);
        if (!r && fresh) {
            r          = fresh;
            fresh      = NULL;
            r->next    = g_risers;
            g_risers   = r;
        }
        if (r) {
            r->pid        = proc->pid;
            r->generation = proc->generation;
            r->started    = kernel_tick_get();
            r->raising    = false;
            const uint32_t raised = r->raised;
            spin_unlock(&g_lock);
            kfree(fresh);
            CabinInfo *ci = (CabinInfo *)vmm_phys_to_virt(proc->cabin->cabin_info_phys);
            ci->raised    = raised;
            return;
        }
        spin_unlock(&g_lock);

        const size_t length = strlen(name);
        fresh = kmalloc(sizeof(Riser) + length + 1);
        if (!fresh) {
            kprintf("[RISE] no memory to watch '%s': if it falls, it is not raised again\n", name);
            return;
        }
        memset(fresh, 0, sizeof(Riser));
        fresh->file    = file;
        fresh->pause_s = RISE_PAUSE_FIRST_S;
        memcpy(fresh->name, name, length + 1);
    }
}

void RiseProgramEnded(uint32_t pid, uint32_t generation, int32_t disposition)
{
    /* The machine is going off: its programs are ended by the halt's sweep,
     * and no end of one is a fall. */
    if (__atomic_load_n(&g_halting, __ATOMIC_ACQUIRE)) return;
    const bool fell = disposition == PROC_EXIT_CRASHED || disposition == PROC_EXIT_STACK_OVERFLOW;
    Riser     *gone = NULL;

    spin_lock(&g_lock);
    Riser *r = RiserOfLife(pid, generation);
    if (!r) {
        spin_unlock(&g_lock);
        return;
    }
    r->pid = r->generation = 0;
    const uint64_t now   = kernel_tick_get();
    const uint64_t lived = now - r->started;
    if (!fell) {
        RiserUnlink(r);
        gone = r;
    } else {
        r->soon = lived <= RISE_TICKS(RISE_SOON_S) ? r->soon + 1 : 0;
        if (r->soon >= RISE_SOON_FALLS) {
            RiserUnlink(r);
            gone = r;
            kprintf("[RISE] '%s' fell %u times in a row, each within %u s of its start: "
                    "it is let be, not raised again\n", r->name, (unsigned)RISE_SOON_FALLS,
                    (unsigned)RISE_SOON_S);
        } else {
            if (lived >= RISE_TICKS(RISE_CALM_S)) r->pause_s = RISE_PAUSE_FIRST_S;
            const uint32_t pause = r->pause_s;
            r->pause_s = pause * 2 < RISE_PAUSE_MOST_S ? pause * 2 : RISE_PAUSE_MOST_S;
            r->due     = now + RISE_TICKS(pause);
            RiseArmLocked();
            kprintf("[RISE] '%s' fell; it is raised again in %u s\n", r->name, pause);
        }
    }
    spin_unlock(&g_lock);
    kfree(gone);
}
